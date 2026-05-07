#include "fs_storage.h"

#include <ydb/core/base/appdata.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

#include <contrib/libs/aws-sdk-cpp/aws-cpp-sdk-core/include/aws/core/client/AWSError.h>
#include <contrib/libs/aws-sdk-cpp/aws-cpp-sdk-s3/include/aws/s3/S3Errors.h>

#include <util/folder/dirut.h>
#include <util/folder/path.h>
#include <util/stream/file.h>
#include <util/system/fs.h>
#include <util/generic/guid.h>
#include <util/string/hex.h>

#include <openssl/sha.h>

#include <type_traits>

namespace NKikimr::NWrappers::NExternalStorage {

#define FS_LOG_T(stream) LOG_TRACE_S(*TlsActivationContext, NKikimrServices::FS_WRAPPER, stream)
#define FS_LOG_I(stream) LOG_INFO_S(*TlsActivationContext, NKikimrServices::FS_WRAPPER, stream)
#define FS_LOG_D(stream) LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::FS_WRAPPER, stream)
#define FS_LOG_W(stream) LOG_WARN_S(*TlsActivationContext, NKikimrServices::FS_WRAPPER, stream)
#define FS_LOG_E(stream) LOG_ERROR_S(*TlsActivationContext, NKikimrServices::FS_WRAPPER, stream)

#define FS_LOG_T_SAFE(stream) do { if (TlsActivationContext) { FS_LOG_T(stream); } } while (false)
#define FS_LOG_I_SAFE(stream) do { if (TlsActivationContext) { FS_LOG_I(stream); } } while (false)
#define FS_LOG_D_SAFE(stream) do { if (TlsActivationContext) { FS_LOG_D(stream); } } while (false)
#define FS_LOG_W_SAFE(stream) do { if (TlsActivationContext) { FS_LOG_W(stream); } } while (false)
#define FS_LOG_E_SAFE(stream) do { if (TlsActivationContext) { FS_LOG_E(stream); } } while (false)

namespace {

class TFsOperationActor : public TActorBootstrapped<TFsOperationActor> {
private:
    const TString BasePath;

    struct TPartInfo {
        int PartNumber = 0;
        ui64 Size = 0;
        TString Sha256;
    };

    struct TMultipartUploadSession {
        const TString Key;
        TFile File;
        ui64 TotalSize = 0;
        TVector<TPartInfo> PartsInfo;

        explicit TMultipartUploadSession(const TString& key)
            : Key(key)
            , File(key, OpenAlways | WrOnly | ForAppend)
        {
            FsyncParentDir(key);
            File.Flock(LOCK_EX | LOCK_NB);
            File.Resize(0);
        }
    };

    static TString ComputeSha256(const TString& data) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
        return to_lower(HexEncode(hash, SHA256_DIGEST_LENGTH));
    }

    THashMap<TString, TMultipartUploadSession> ActiveUploads;
    static constexpr std::pair<ui64, ui64> EmptyRange = std::make_pair(0, 0);
    static constexpr auto InvalidRange = EmptyRange;

    template<typename TEvResponse>
    struct RequiresKey : std::true_type {};

    template<>
    struct RequiresKey<TEvListObjectsResponse> : std::false_type {};

    template<>
    struct RequiresKey<TEvDeleteObjectsResponse> : std::false_type {};

    template<typename TEvResponse>
    static constexpr bool HasKeyConstructor() {
        return RequiresKey<TEvResponse>::value;
    }

    static void FsyncParentDir(const TFsPath& fsPath) {
        TFile dirFd(fsPath.Parent().GetPath(), RdOnly | Seq);
        dirFd.Flush();
    }

    static void FsyncParentDir(const TString& filePath) {
        FsyncParentDir(TFsPath(filePath));
    }

    template<typename TEvResponse>
    void ReplySuccess(const NActors::TActorId& sender, const std::optional<TString>& key) {
        typename TEvResponse::TAwsResult awsResult;
        Aws::Utils::Outcome<typename TEvResponse::TAwsResult, Aws::S3::S3Error> outcome(std::move(awsResult));

        std::unique_ptr<TEvResponse> response;
        if constexpr (HasKeyConstructor<TEvResponse>()) {
            Y_ENSURE(key, "Key is required for this response type");
            response = std::make_unique<TEvResponse>(*key, std::move(outcome));
        } else {
            response = std::make_unique<TEvResponse>(std::move(outcome));
        }
        this->Send(sender, response.release());
    }

    template<typename TEvResponse>
    auto CreateOutcome(Aws::S3::S3Errors errorType, const TString& exceptionName, const TString& errorMessage, bool retryable) {
        Aws::Client::AWSError<Aws::S3::S3Errors> awsError(
            errorType,
            exceptionName,
            errorMessage,
            retryable
        );
        Aws::S3::S3Error error(std::move(awsError));
        Aws::Utils::Outcome<typename TEvResponse::TAwsResult, Aws::S3::S3Error> outcome(std::move(error));
        return outcome;
    }

    template<typename TEvResponse>
    void ReplyError(
            const NActors::TActorId& sender,
            const TString& errorMessage,
            Aws::S3::S3Errors errorType = Aws::S3::S3Errors::INTERNAL_FAILURE,
            bool retryable = false,
            const TString& exceptionName = "FsStorageError")
    {
        std::unique_ptr<TEvResponse> response;
        response = std::make_unique<TEvResponse>(CreateOutcome<TEvResponse>(errorType, exceptionName, errorMessage, retryable));
        this->Send(sender, response.release());
    }

    template<typename TEvResponse>
    void ReplyError(
            const NActors::TActorId& sender,
            const TString& key,
            const TString& errorMessage,
            Aws::S3::S3Errors errorType = Aws::S3::S3Errors::INTERNAL_FAILURE,
            bool retryable = false,
            const TString& exceptionName = "FsStorageError")
    {
        std::unique_ptr<TEvResponse> response;
        response = std::make_unique<TEvResponse>(key, CreateOutcome<TEvResponse>(errorType, exceptionName, errorMessage, retryable));
        this->Send(sender, response.release());
    }

    template<typename TEvResponse>
    void ReplyError(
            const NActors::TActorId& sender,
            const TString& key,
            const std::pair<ui64, ui64>& range,
            const TString& errorMessage,
            Aws::S3::S3Errors errorType = Aws::S3::S3Errors::INTERNAL_FAILURE,
            bool retryable = false,
            const TString& exceptionName = "FsStorageError")
    {
        std::unique_ptr<TEvResponse> response;
        response = std::make_unique<TEvResponse>(key, range, CreateOutcome<TEvResponse>(errorType, exceptionName, errorMessage, retryable));
        this->Send(sender, response.release());
    }

    template<typename TEvResponse>
    bool HandleFileLockError(
            const TSystemError& ex,
            const NActors::TActorId& sender,
            const TString& key,
            const TString& operation)
    {
        if (ex.Status() == EWOULDBLOCK) {
            FS_LOG_W(operation << ": failed to acquire lock (file is busy)"
                << ": key# " << key);
            ReplyError<TEvResponse>(sender, key, "File is locked by another process",
                Aws::S3::S3Errors::INTERNAL_FAILURE, true /* retryable */);
            return true;
        }
        return false;
    }

public:
    TFsOperationActor(const TString& basePath)
        : BasePath(basePath)
    {
    }

    ~TFsOperationActor() {
        CleanupActiveSessions();
    }

    void Bootstrap() {
        FS_LOG_T("TFsOperationActor Bootstrap called");
        Become(&TThis::StateWork);
    }

    void PassAway() override {
        CleanupActiveSessions();
        NActors::TActorBootstrapped<TFsOperationActor>::PassAway();
    }

private:
    void CleanupActiveSessions() {
        FS_LOG_I_SAFE("TFsOperationActor: cleaning up active sessions"
            << ": count# " << ActiveUploads.size());
        for (auto& [uploadId, session] : ActiveUploads) {
            try {
                const TString filePath = session.Key;
                const ui64 totalSize = session.TotalSize;
                NFs::Remove(filePath);
                session.File.Close();

                FS_LOG_I_SAFE("TFsOperationActor: closed and deleted incomplete file"
                    << ": uploadId# " << uploadId
                    << ", file# " << filePath
                    << ", fileTotalSize# " << totalSize);
            } catch (const std::exception& ex) {
                FS_LOG_W_SAFE("Failed to cleanup MPU session"
                    << ": uploadId# " << uploadId
                    << ", error# " << ex.what());
            }
        }
        ActiveUploads.clear();
    }

public:

    STATEFN(StateWork) {
        FS_LOG_T("TFsOperationActor StateWork received event type"
            << ": type# " << ev->GetTypeRewrite());
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvPutObjectRequest, Handle);
            hFunc(TEvGetObjectRequest, Handle);
            hFunc(TEvHeadObjectRequest, Handle);
            hFunc(TEvDeleteObjectRequest, Handle);
            hFunc(TEvCheckObjectExistsRequest, Handle);
            hFunc(TEvListObjectsRequest, Handle);
            hFunc(TEvDeleteObjectsRequest, Handle);
            hFunc(TEvCreateMultipartUploadRequest, Handle);
            hFunc(TEvUploadPartRequest, Handle);
            hFunc(TEvCompleteMultipartUploadRequest, Handle);
            hFunc(TEvAbortMultipartUploadRequest, Handle);
            hFunc(TEvUploadPartCopyRequest, Handle);
            sFunc(NActors::TEvents::TEvPoison, PassAway);
            default:
                FS_LOG_W("TFsOperationActor StateWork received unknown event type"
                    << ": type# " << ev->GetTypeRewrite());
        }
    }

    TString GetIncompletePath(const char* key) {
        return TStringBuilder() << key << ".incomplete";
    }

    void Handle(TEvPutObjectRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const auto& body = ev->Get()->Body;
        const TString key = TString(request.GetKey().data(), request.GetKey().size());

        FS_LOG_I("PutObject begin"
            << ": key# " << key
            << ", bodySize# " << body.size()
            << ", sender# " << ev->Sender);

        try {
            TFsPath fsPath(key);
            fsPath.Parent().MkDirs();

            TMultipartUploadSession session(key);
            session.File.Write(body.data(), body.size());
            session.File.Flush();
            session.File.Close();

            FS_LOG_I("PutObject succeeded"
                << ": key# " << key
                << ", bytesWritten# " << body.size());

            ReplySuccess<TEvPutObjectResponse>(ev->Sender, key);
        } catch (const TSystemError& ex) {
            if (!HandleFileLockError<TEvPutObjectResponse>(ex, ev->Sender, key, "PutObject")) {
                FS_LOG_E("PutObject failed with system error"
                    << ": key# " << key
                    << ", error# " << ex.what()
                    << ", errno# " << ex.Status());
                ReplyError<TEvPutObjectResponse>(ev->Sender, key, ex.what());
            }
        } catch (const std::exception& ex) {
            FS_LOG_E("PutObject failed"
                << ": key# " << key
                << ", error# " << ex.what());
            ReplyError<TEvPutObjectResponse>(ev->Sender, key, ex.what());
        }
    }

    void Handle(TEvGetObjectRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());

        FS_LOG_D("GetObject"
            << ": key# " << key);

        try {
            TFile file(key, RdOnly);
            const i64 fileSize = file.GetLength();

            if (fileSize == 0) {
                Aws::S3::Model::GetObjectResult awsResult;
                awsResult.SetContentLength(0);
                awsResult.SetETag("\"fs-file\"");
                Aws::Utils::Outcome<Aws::S3::Model::GetObjectResult, Aws::S3::S3Error> outcome(std::move(awsResult));

                auto response = std::make_unique<TEvGetObjectResponse>(key, EmptyRange, std::move(outcome), TString());
                Send(ev->Sender, response.release());
                return;
            }

            TString rangeStr(request.GetRange().data(), request.GetRange().size());
            std::pair<ui64, ui64> range;

            if (!rangeStr.empty()) {
                if (!TEvGetObjectResponse::TryParseRange(rangeStr, range)) {
                    ReplyError<TEvGetObjectResponse>(ev->Sender, key, InvalidRange, TStringBuilder() << "Invalid range format: " << rangeStr);
                    return;
                }
            } else {
                range = std::make_pair(0, fileSize - 1);
            }

            ui64 start = range.first;
            ui64 end = range.second;
            if (start > end) {
                ReplyError<TEvGetObjectResponse>(ev->Sender, key, InvalidRange, TStringBuilder() << "Invalid range: start > end: " << rangeStr);
                return;
            }
            const ui64 length = end - start + 1;

            if ((i64)start >= fileSize) {
                ReplyError<TEvGetObjectResponse>(ev->Sender, key, range, "Range out of bounds");
                return;
            }

            TString data;
            data.resize(length);
            file.Seek(start, sSet);
            size_t bytesRead = file.Read(data.begin(), length);
            data.resize(bytesRead);

            FS_LOG_I("GetObject read"
                << ": bytes# " << bytesRead
                << ", from# " << key);

            Aws::S3::Model::GetObjectResult awsResult;
            awsResult.SetContentLength(bytesRead);
            awsResult.SetETag("\"fs-file\"");
            Aws::Utils::Outcome<Aws::S3::Model::GetObjectResult, Aws::S3::S3Error> outcome(std::move(awsResult));

            auto response = std::make_unique<TEvGetObjectResponse>(key, range, std::move(outcome), std::move(data));
            Send(ev->Sender, response.release());

        } catch (const std::exception& ex) {
            FS_LOG_E("GetObject error"
                << ": key# " << key
                << ", error# " << ex.what());
            ReplyError<TEvGetObjectResponse>(ev->Sender, key, InvalidRange, TString("File read error: ") + ex.what());
        }
    }

    void Handle(TEvHeadObjectRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());

        FS_LOG_D("HeadObject"
            << ": key# " << key);

        try {
            TFile file(key, RdOnly | Seq);
            const i64 fileSize = file.GetLength();

            FS_LOG_I("HeadObject"
                << ": file size# " << fileSize
                << " for# " << key);

            Aws::S3::Model::HeadObjectResult awsResult;
            awsResult.SetContentLength(fileSize);
            awsResult.SetETag("\"fs-file\"");

            Aws::Utils::Outcome<Aws::S3::Model::HeadObjectResult, Aws::S3::S3Error> outcome(std::move(awsResult));
            auto response = std::make_unique<TEvHeadObjectResponse>(key, std::move(outcome));
            Send(ev->Sender, response.release());
        } catch (const TFileError& ex) {
            FS_LOG_W("HeadObject"
                << ": key# " << key
                << ", error# " << "file not found");
                ReplyError<TEvHeadObjectResponse>(ev->Sender, key, TStringBuilder() << "File not found: " << ex.what(), Aws::S3::S3Errors::NO_SUCH_KEY);
        } catch (const std::exception& ex) {
            FS_LOG_E("HeadObject error"
                << ": key# " << key
                << ", error# " << ex.what());
                ReplyError<TEvHeadObjectResponse>(ev->Sender, key, TStringBuilder() << "File head error: " << ex.what());
        }
    }

    void Handle(TEvDeleteObjectRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());
        FS_LOG_W("DeleteObject: not implemented");
        ReplyError<TEvDeleteObjectResponse>(ev->Sender, key, "Not implemented");
    }

    void Handle(TEvCheckObjectExistsRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());
        FS_LOG_W("CheckObjectExists: not implemented");
        ReplyError<TEvCheckObjectExistsResponse>(ev->Sender, key, "Not implemented");
    }

    static constexpr int DefaultMaxListKeys = 1000;

    bool ListFilesRecursive(const TFsPath& dir, const TString& marker, int maxKeys,
        Aws::S3::Model::ListObjectsResult& result)
    {
        TVector<TString> children;
        dir.ListNames(children);
        Sort(children);
        for (const auto& name : children) {
            TFsPath child = dir / name;
            if (child.IsFile()) {
                const TString& path = child.GetPath();
                if (!marker.empty() && path <= marker) {
                    continue;
                }
                if (static_cast<int>(result.GetContents().size()) >= maxKeys) {
                    return true;
                }
                Aws::S3::Model::Object obj;
                obj.SetKey(Aws::String(path.data(), path.size()));
                result.AddContents(std::move(obj));
            } else if (child.IsDirectory()) {
                if (ListFilesRecursive(child, marker, maxKeys, result)) {
                    return true;
                }
            }
        }
        return false;
    }

    void Handle(TEvListObjectsRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString requestPrefix = TString(request.GetPrefix().data(), request.GetPrefix().size());
        const TString prefix = requestPrefix.empty() ? BasePath : requestPrefix;
        const TString marker = TString(request.GetMarker().data(), request.GetMarker().size());
        const int maxKeys = request.GetMaxKeys() > 0
            ? std::min(request.GetMaxKeys(), DefaultMaxListKeys)
            : DefaultMaxListKeys;

        FS_LOG_D("ListObjects"
            << ": prefix# " << prefix
            << ", marker# " << marker
            << ", maxKeys# " << maxKeys);

        try {
            TFsPath dirPath(prefix);

            if (!dirPath.IsNonStrictSubpathOf(BasePath)) {
                ReplyError<TEvListObjectsResponse>(ev->Sender, "Prefix is outside of base path");
                return;
            }

            Aws::S3::Model::ListObjectsResult awsResult;
            bool truncated = false;

            if (dirPath.Exists() && dirPath.IsDirectory()) {
                truncated = ListFilesRecursive(dirPath, marker, maxKeys, awsResult);
            }

            awsResult.SetIsTruncated(truncated);

            Aws::Utils::Outcome<Aws::S3::Model::ListObjectsResult, Aws::S3::S3Error> outcome(std::move(awsResult));
            auto response = std::make_unique<TEvListObjectsResponse>(std::move(outcome));
            Send(ev->Sender, response.release());
        } catch (const std::exception& ex) {
            FS_LOG_E("ListObjects failed"
                << ": prefix# " << prefix
                << ", error# " << ex.what());
            ReplyError<TEvListObjectsResponse>(ev->Sender, TString("ListObjects error: ") + ex.what());
        }
    }

    void Handle(TEvDeleteObjectsRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        FS_LOG_W("DeleteObjects: not implemented, objects count# " << request.GetDelete().GetObjects().size());
        ReplyError<TEvDeleteObjectsResponse>(ev->Sender, "Not implemented");
    }

    void Handle(TEvCreateMultipartUploadRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString originalKey = TString(request.GetKey().data(), request.GetKey().size());

        FS_LOG_I("CreateMultipartUpload begin"
            << ": key# " << originalKey
            << ", sender# " << ev->Sender
            << ", activeSessions# " << ActiveUploads.size());

        try {
            const TString key = GetIncompletePath(originalKey.c_str());
            const TString uploadId = TStringBuilder() << key << "_" << CreateGuidAsString();
            TFsPath fsPath(key);
            fsPath.Parent().MkDirs();

            ActiveUploads.emplace(uploadId, TMultipartUploadSession(key));

            FS_LOG_I("CreateMultipartUpload succeeded"
                << ": originalKey# " << originalKey
                << ", incompleteFile# " << key
                << ", uploadId# " << uploadId
                << ", activeSessions# " << ActiveUploads.size());

            Aws::S3::Model::CreateMultipartUploadResult awsResult;
            awsResult.SetKey(request.GetKey());
            awsResult.SetUploadId(uploadId.c_str());

            Aws::Utils::Outcome<Aws::S3::Model::CreateMultipartUploadResult, Aws::S3::S3Error> outcome(std::move(awsResult));
            auto response = std::make_unique<TEvCreateMultipartUploadResponse>(originalKey, std::move(outcome));
            this->Send(ev->Sender, response.release());
        } catch (const TSystemError& ex) {
            if (!HandleFileLockError<TEvCreateMultipartUploadResponse>(ex, ev->Sender, originalKey, "CreateMultipartUpload")) {
                FS_LOG_E("CreateMultipartUpload failed with system error"
                    << ": key# " << originalKey
                    << ", error# " << ex.what()
                    << ", errno# " << ex.Status());
                ReplyError<TEvCreateMultipartUploadResponse>(ev->Sender, originalKey, ex.what());
            }
        } catch (const std::exception& ex) {
            FS_LOG_E("CreateMultipartUpload failed"
                << ": key# " << originalKey
                << ", error# " << ex.what());
            ReplyError<TEvCreateMultipartUploadResponse>(ev->Sender, originalKey, ex.what());
        }
    }

    void Handle(TEvUploadPartRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const auto& body = ev->Get()->Body;
        const TString originalKey = TString(request.GetKey().data(), request.GetKey().size());
        const TString uploadId = TString(request.GetUploadId().data(), request.GetUploadId().size());
        const int partNumber = request.GetPartNumber();

        FS_LOG_I("UploadPart begin"
            << ": key# " << originalKey
            << ", uploadId# " << uploadId
            << ", part# " << partNumber
            << ", bodySize# " << body.size()
            << ", sender# " << ev->Sender);

        try {
            const TString key = GetIncompletePath(originalKey.c_str());
            auto it = ActiveUploads.find(uploadId);
            if (it == ActiveUploads.end()) {
                // If the UploadId is not found in ActiveUploads,
                // it means that a restart has occurred and all parts have started being written again
                // so we can simply create a new session.
                if (partNumber != 1) {
                    const TString errorMsg = TStringBuilder()
                        << "Cannot create new upload session for part " << partNumber
                        << " (uploadId: " << uploadId << "). Session must start with part 1.";
                    FS_LOG_E("UploadPart: " << errorMsg);
                    ReplyError<TEvUploadPartResponse>(ev->Sender, originalKey, errorMsg, Aws::S3::S3Errors::INTERNAL_FAILURE);
                    return;
                }
                FS_LOG_I("UploadPart: session not found, creating new session for part 1"
                    << ": uploadId# " << uploadId
                    << ", incompleteFile# " << key);
                it = ActiveUploads.emplace(uploadId, TMultipartUploadSession(key)).first;
            }

            auto& session = it->second;
            const ui64 offsetBefore = session.TotalSize;

            const TString partSha256 = ComputeSha256(body);

            session.File.Write(body.data(), body.size());
            session.File.Flush();
            session.TotalSize += body.size();
            session.PartsInfo.push_back({partNumber, body.size(), partSha256});

            const TString etag = TStringBuilder() << "\"part" << partNumber << "\"";

            FS_LOG_I("UploadPart completed"
                << ": uploadId# " << uploadId
                << ", part# " << partNumber
                << ", partSize# " << body.size()
                << ", partSha256# " << partSha256
                << ", fileOffsetBefore# " << offsetBefore
                << ", fileTotalSize# " << session.TotalSize
                << ", eTag# " << etag
                << ", incompleteFile# " << session.Key);

            Aws::S3::Model::UploadPartResult awsResult;
            awsResult.SetETag(etag.c_str());

            Aws::Utils::Outcome<Aws::S3::Model::UploadPartResult, Aws::S3::S3Error> outcome(std::move(awsResult));
            auto response = std::make_unique<TEvUploadPartResponse>(originalKey, std::move(outcome));
            this->Send(ev->Sender, response.release());
        } catch (const TSystemError& ex) {
            if (!HandleFileLockError<TEvUploadPartResponse>(ex, ev->Sender, originalKey, "UploadPart")) {
                FS_LOG_E("UploadPart failed with system error"
                    << ": key# " << originalKey
                    << ", uploadId# " << uploadId
                    << ", part# " << partNumber
                    << ", bodySize# " << body.size()
                    << ", error# " << ex.what()
                    << ", errno# " << ex.Status());
                ReplyError<TEvUploadPartResponse>(ev->Sender, originalKey, ex.what());
            }
        } catch (const std::exception& ex) {
            FS_LOG_E("UploadPart failed"
                << ": key# " << originalKey
                << ", uploadId# " << uploadId
                << ", part# " << partNumber
                << ", bodySize# " << body.size()
                << ", error# " << ex.what());
            ReplyError<TEvUploadPartResponse>(ev->Sender, originalKey, ex.what());
        }
    }

    void Handle(TEvCompleteMultipartUploadRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());
        const TString uploadId = TString(request.GetUploadId().data(), request.GetUploadId().size());

        const auto& parts = request.GetMultipartUpload().GetParts();
        FS_LOG_I("CompleteMultipartUpload begin"
            << ": key# " << key
            << ", uploadId# " << uploadId
            << ", partsInRequest# " << parts.size()
            << ", sender# " << ev->Sender);

        for (const auto& part : parts) {
            FS_LOG_I("CompleteMultipartUpload part"
                << ": uploadId# " << uploadId
                << ", partNumber# " << part.GetPartNumber()
                << ", eTag# " << part.GetETag());
        }

        try {
            const TString incompleteKey = GetIncompletePath(key.c_str());
            auto it = ActiveUploads.find(uploadId);
            if (it == ActiveUploads.end()) {
                FS_LOG_E("CompleteMultipartUpload: session not found"
                    << ": uploadId# " << uploadId
                    << ", key# " << key
                    << ", activeSessions# " << ActiveUploads.size());
                ReplyError<TEvCompleteMultipartUploadResponse>(ev->Sender, key,
                    TStringBuilder() << "Upload session not found: uploadId# " << uploadId,
                    Aws::S3::S3Errors::INTERNAL_FAILURE, 
                    true /* retryable */, 
                    "FsCompleteMultipartUploadFailed"
                );
                return;
            }

            auto& session = it->second;
            session.File.Flush();

            const ui64 totalSize = session.TotalSize;
            const auto& partsInfo = session.PartsInfo;

            FS_LOG_I("CompleteMultipartUpload: flushed, renaming"
                << ": uploadId# " << uploadId
                << ", fileTotalSize# " << totalSize
                << ", uploadedPartsCount# " << partsInfo.size()
                << ", from# " << incompleteKey
                << ", to# " << key);

            for (const auto& pi : partsInfo) {
                FS_LOG_I("CompleteMultipartUpload: uploaded part summary"
                    << ": uploadId# " << uploadId
                    << ", part# " << pi.PartNumber
                    << ", size# " << pi.Size
                    << ", sha256# " << pi.Sha256);
            }

            if (!NFs::Rename(incompleteKey, key)) {
                const TString errorMsg = TStringBuilder()
                    << "Failed to rename " << incompleteKey << " to " << key
                    << ": " << LastSystemErrorText();
                FS_LOG_E("CompleteMultipartUpload: rename failed"
                    << ": uploadId# " << uploadId
                    << ", fileTotalSize# " << totalSize
                    << ", uploadedPartsCount# " << partsInfo.size()
                    << ", error# " << errorMsg);

                session.File.Close();
                NFs::Remove(incompleteKey);
                ActiveUploads.erase(it);

                ReplyError<TEvCompleteMultipartUploadResponse>(ev->Sender, key, errorMsg, 
                    Aws::S3::S3Errors::INTERNAL_FAILURE, 
                    true /* retryable */, 
                    "FsCompleteMultipartUploadFailed"
                );
                return;
            }
            FsyncParentDir(key);
            session.File.Close();

            FS_LOG_I("CompleteMultipartUpload succeeded"
                << ": uploadId# " << uploadId
                << ", key# " << key
                << ", fileTotalSize# " << totalSize
                << ", partsCount# " << parts.size()
                << ", uploadedPartsCount# " << partsInfo.size());

            ActiveUploads.erase(it);

            Aws::S3::Model::CompleteMultipartUploadResult awsResult;
            awsResult.SetKey(request.GetKey());
            const TString etag = "\"completed\"";
            awsResult.SetETag(etag.c_str());

            Aws::Utils::Outcome<Aws::S3::Model::CompleteMultipartUploadResult, Aws::S3::S3Error> outcome(std::move(awsResult));
            auto response = std::make_unique<TEvCompleteMultipartUploadResponse>(key, std::move(outcome));
            this->Send(ev->Sender, response.release());
        } catch (const std::exception& ex) {
            FS_LOG_E("CompleteMultipartUpload failed with exception"
                << ": key# " << key
                << ", uploadId# " << uploadId
                << ", error# " << ex.what());
            ReplyError<TEvCompleteMultipartUploadResponse>(ev->Sender, key, ex.what());
        }
    }

    void Handle(TEvAbortMultipartUploadRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());
        const TString uploadId = TString(request.GetUploadId().data(), request.GetUploadId().size());

        FS_LOG_I("AbortMultipartUpload begin"
            << ": key# " << key
            << ", uploadId# " << uploadId
            << ", sender# " << ev->Sender);

        try {
            auto it = ActiveUploads.find(uploadId);
            if (it == ActiveUploads.end()) {
                FS_LOG_W("AbortMultipartUpload: session not found"
                    << ": uploadId# " << uploadId
                    << ", key# " << key
                    << ", activeSessions# " << ActiveUploads.size());
            } else {
                auto& session = it->second;
                const TString filePath = session.Key;
                const ui64 totalSize = session.TotalSize;

                bool removed = NFs::Remove(filePath);
                FS_LOG_I("AbortMultipartUpload: cleanup"
                    << ": uploadId# " << uploadId
                    << ", file# " << filePath
                    << ", fileTotalSize# " << totalSize
                    << ", fileRemoved# " << removed);

                if (!removed) {
                    FS_LOG_W("AbortMultipartUpload: failed to delete incomplete file"
                        << ": uploadId# " << uploadId
                        << ", file# " << filePath);
                }
                ActiveUploads.erase(it);
            }

            ReplySuccess<TEvAbortMultipartUploadResponse>(ev->Sender, key);
        } catch (const std::exception& ex) {
            FS_LOG_E("AbortMultipartUpload failed"
                << ": key# " << key
                << ", uploadId# " << uploadId
                << ", error# " << ex.what());
            ReplyError<TEvAbortMultipartUploadResponse>(ev->Sender, key, ex.what());
        }
    }

    void Handle(TEvUploadPartCopyRequest::TPtr& ev) {
        const auto& request = ev->Get()->GetRequest();
        const TString key = TString(request.GetKey().data(), request.GetKey().size());

        FS_LOG_W("UploadPartCopy: not implemented");
        ReplyError<TEvUploadPartCopyResponse>(ev->Sender, key, "Not implemented");
    }
};

} // anonymous namespace

TFsExternalStorage::TFsExternalStorage(const TString& basePath)
    : BasePath(basePath)
{
}

TFsExternalStorage::~TFsExternalStorage() {
    Shutdown();
}

void TFsExternalStorage::EnsureActor() const {
    if (ActorCreated) {
        return;
    }

    auto actor = new TFsOperationActor(BasePath);
    OperationActorId = TlsActivationContext->AsActorContext().Register(
        actor, TMailboxType::HTSwap, AppData()->IOPoolId);
    ActorCreated = true;

    FS_LOG_I("TFsExternalStorage: Created persistent actor"
        << ": OperationActorId# " << OperationActorId);
}

void TFsExternalStorage::Shutdown() {
    if (ActorCreated && TlsActivationContext) {
        FS_LOG_I("TFsExternalStorage: Shutting down actor"
            << ": OperationActorId# " << OperationActorId);
        TlsActivationContext->AsActorContext().Send(OperationActorId, new NActors::TEvents::TEvPoison());
        ActorCreated = false;
    }
}

template <typename TEvPtr>
void TFsExternalStorage::ExecuteImpl(TEvPtr& ev) const {
    EnsureActor();
    TlsActivationContext->AsActorContext().Send(ev->Forward(OperationActorId));
}

void TFsExternalStorage::Execute(TEvPutObjectRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvGetObjectRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvHeadObjectRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvDeleteObjectRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvCheckObjectExistsRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvListObjectsRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvDeleteObjectsRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvCreateMultipartUploadRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvUploadPartRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvCompleteMultipartUploadRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvAbortMultipartUploadRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

void TFsExternalStorage::Execute(TEvUploadPartCopyRequest::TPtr& ev) const {
    ExecuteImpl(ev);
}

} // NKikimr::NWrappers::NExternalStorage
