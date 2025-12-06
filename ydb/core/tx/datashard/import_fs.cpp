#include "backup_restore_traits.h"
#include "datashard_impl.h"
#include "import_common.h"
#include "import_fs.h"

#include <ydb/core/backup/common/checksum.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/protos/datashard_config.pb.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/fs_settings.pb.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/tablet/resource_broker.h>
#include <ydb/core/io_formats/ydb_dump/csv_ydb_dump.h>
#include <ydb/public/lib/scheme_types/scheme_type_id.h>

#include <contrib/libs/zstd/include/zstd.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>

#include <util/folder/path.h>
#include <util/generic/buffer.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/memory/pool.h>
#include <util/stream/file.h>
#include <util/string/builder.h>
#include <util/system/file.h>

namespace {

    struct DestroyZCtx {
        static void Destroy(::ZSTD_DCtx* p) noexcept {
            ZSTD_freeDCtx(p);
        }
    };

} // anonymous

namespace NKikimr {
namespace NDataShard {

using namespace NBackup;
using namespace NBackupRestoreTraits;
using namespace NResourceBroker;

class TFsSettings {
public:
    const TString BasePath;
    const TString RelativePath;
    const ui32 Shard;

    explicit TFsSettings(const NKikimrSchemeOp::TFSSettings& settings, ui32 shard)
        : BasePath(settings.GetBasePath())
        , RelativePath(settings.GetPath())
        , Shard(shard)
    {
    }

    static TFsSettings FromRestoreTask(const NKikimrSchemeOp::TRestoreTask& task) {
        return TFsSettings(task.GetFSSettings(), task.GetShardNum());
    }

    TString GetFullPath() const {
        return TFsPath(BasePath) / RelativePath;
    }

    TString GetDataPath(EDataFormat format, ECompressionCodec codec) const {
        return TFsPath(GetFullPath()) / DataKeySuffix(Shard, format, codec, false);
    }

}; // TFsSettings

class TFsDownloader: public TActorBootstrapped<TFsDownloader> {
    static constexpr ui32 DefaultReadBatchSize = 8 * 1024 * 1024; // 8 MB
    static constexpr ui64 DefaultBufferSizeLimit = 64 * 1024 * 1024; // 64 MB

    class TUploadRowsRequestBuilder {
    public:
        void New(const TTableInfo& tableInfo, const NKikimrSchemeOp::TTableDescription& scheme) {
            Record = std::make_shared<NKikimrTxDataShard::TEvUploadRowsRequest>();
            Record->SetTableId(tableInfo.GetId());

            TVector<TString> columnNames;
            for (const auto& column : scheme.GetColumns()) {
                columnNames.push_back(column.GetName());
            }

            auto& rowScheme = *Record->MutableRowScheme();
            for (ui32 id : tableInfo.GetKeyColumnIds()) {
                rowScheme.AddKeyColumnIds(id);
            }
            for (ui32 id : tableInfo.GetValueColumnIds(columnNames)) {
                rowScheme.AddValueColumnIds(id);
            }

            CellBytes = 0;
        }

        void AddRow(const TVector<TCell>& keys, const TVector<TCell>& values) {
            Y_ENSURE(Record);
            auto& row = *Record->AddRows();
            row.SetKeyColumns(TSerializedCellVec::Serialize(keys));
            row.SetValueColumns(TSerializedCellVec::Serialize(values));

            for (const auto& x : keys) {
                CellBytes += x.Size();
            }
            for (const auto& x : values) {
                CellBytes += x.Size();
            }
        }

        const std::shared_ptr<NKikimrTxDataShard::TEvUploadRowsRequest>& GetRecord() {
            Y_ENSURE(Record);
            return Record;
        }

        ui64 GetCellBytes() const {
            return CellBytes;
        }

    private:
        std::shared_ptr<NKikimrTxDataShard::TEvUploadRowsRequest> Record;
        ui64 CellBytes = 0;

    }; // TUploadRowsRequestBuilder

    struct TCounters {
        struct TLatency {
            TInstant Begin;
            ::NMonitoring::THistogramPtr Counter;

            explicit TLatency(::NMonitoring::THistogramPtr counter)
                : Counter(counter)
            {
            }

            void Start(TInstant begin) {
                Begin = begin;
            }

            void Finish(TInstant end) {
                Counter->Collect((end - Begin).MilliSeconds());
                Begin = TInstant::Zero();
            }
        };

        ::NMonitoring::TDynamicCounters::TCounterPtr BytesRead;
        ::NMonitoring::TDynamicCounters::TCounterPtr BytesWritten;
        TLatency LatencyRead;
        TLatency LatencyProcess;
        TLatency LatencyWrite;

        explicit TCounters(::NMonitoring::TDynamicCounterPtr counters)
            : BytesRead(counters->GetCounter("BytesRead", true))
            , BytesWritten(counters->GetCounter("BytesWritten", true))
            , LatencyRead(counters->GetHistogram("LatencyReadMs", ::NMonitoring::ExponentialHistogram(10, 4, 1)))
            , LatencyProcess(counters->GetHistogram("LatencyProcessMs", ::NMonitoring::ExponentialHistogram(10, 4, 1)))
            , LatencyWrite(counters->GetHistogram("LatencyWriteMs", ::NMonitoring::ExponentialHistogram(10, 4, 1)))
        {
        }

    }; // TCounters

    static TInstant Now() {
        return TlsActivationContext->Now();
    }

    void AllocateResource() {
        IMPORT_LOG_D("AllocateResource");

        const auto* appData = AppData();
        Send(MakeResourceBrokerID(), new TEvResourceBroker::TEvSubmitTask(
            TStringBuilder() << "FsRestore { " << TxId << ":" << DataShard << " }",
            {{ 1, 0 }},
            appData->DataShardConfig.GetRestoreTaskName(),
            appData->DataShardConfig.GetRestoreTaskPriority(),
            nullptr
        ));

        Become(&TThis::StateAllocateResource);
    }

    void Handle(TEvResourceBroker::TEvResourceAllocated::TPtr& ev) {
        IMPORT_LOG_I("Handle TEvResourceBroker::TEvResourceAllocated {"
            << " TaskId: " << ev->Get()->TaskId
        << " }");

        TaskId = ev->Get()->TaskId;
        StartReading();
    }

    bool OpenDataFile() {
        DataFilePath = Settings.GetDataPath(DataFormat, CompressionCodec);
        
        IMPORT_LOG_D("OpenDataFile"
            << ": path# " << DataFilePath
            << ", codec# " << static_cast<int>(CompressionCodec));

        if (!TFsPath(DataFilePath).Exists()) {
            // Try next compression codec
            CompressionCodec = NextCompressionCodec(CompressionCodec);
            if (CompressionCodec == ECompressionCodec::Invalid) {
                // No data file found - this is OK for empty tables
                IMPORT_LOG_I("No data file found, finishing with 0 rows"
                    << ": basePath# " << Settings.GetFullPath());
                NoDataFile = true;
                return true;
            }
            return OpenDataFile();
        }

        try {
            DataFileHandle.Reset(new TFile(DataFilePath, OpenExisting | RdOnly));
            ContentLength = DataFileHandle->GetLength();
            
            IMPORT_LOG_I("Data file opened"
                << ": path# " << DataFilePath
                << ", size# " << ContentLength);
            
            return true;
        } catch (const std::exception& ex) {
            Finish(false, TStringBuilder() << "Failed to open data file " << DataFilePath << ": " << ex.what());
            return false;
        }
    }

    bool ReadChecksumFile() {
        if (!ValidateChecksums) {
            return true;
        }

        const TString checksumPath = ChecksumKey(Settings.GetDataPath(DataFormat, ECompressionCodec::None));
        
        IMPORT_LOG_D("ReadChecksumFile"
            << ": path# " << checksumPath);

        if (!TFsPath(checksumPath).Exists()) {
            IMPORT_LOG_W("Checksum file not found, skipping validation"
                << ": path# " << checksumPath);
            ValidateChecksums = false;
            return true;
        }

        try {
            TFileInput file(checksumPath);
            TString content = file.ReadAll();
            ExpectedChecksum = content.substr(0, content.find(' '));
            
            IMPORT_LOG_D("Checksum read"
                << ": expected# " << ExpectedChecksum);
            
            return true;
        } catch (const std::exception& ex) {
            Finish(false, TStringBuilder() << "Failed to read checksum file " << checksumPath << ": " << ex.what());
            return false;
        }
    }

    void StartReading() {
        IMPORT_LOG_N("StartReading"
            << ": attempt# " << Attempt);

        if (!OpenDataFile()) {
            return;
        }

        // No data file found - this is OK for empty tables
        if (NoDataFile) {
            Finish();
            return;
        }

        if (!ReadChecksumFile()) {
            return;
        }

        // Initialize decompression context if needed
        if (CompressionCodec == ECompressionCodec::Zstd) {
            ZstdContext.Reset(ZSTD_createDCtx());
            ZSTD_DCtx_reset(ZstdContext.Get(), ZSTD_reset_session_only);
        }

        if (ContentLength == 0) {
            // Empty file - nothing to import
            Finish();
            return;
        }

        // Allocate read buffer
        Buffer.Reserve(ReadBatchSize);
        
        Become(&TThis::StateReadData);
        ReadNextChunk();
    }

    void ReadNextChunk() {
        if (ProcessedBytes >= ContentLength) {
            if (!CheckChecksum()) {
                return;
            }
            Finish();
            return;
        }

        Counters.LatencyRead.Start(Now());

        try {
            const ui64 bytesToRead = Min(static_cast<ui64>(ReadBatchSize), ContentLength - ReadBytes);
            TString chunk;
            chunk.resize(bytesToRead);
            
            const size_t bytesRead = DataFileHandle->Read(chunk.begin(), bytesToRead);
            if (bytesRead == 0 && ReadBytes < ContentLength) {
                Finish(false, "Unexpected end of file");
                return;
            }
            
            chunk.resize(bytesRead);
            ReadBytes += bytesRead;
            *Counters.BytesRead += bytesRead;
            
            Counters.LatencyRead.Finish(Now());

            // Append to buffer and process
            if (CompressionCodec == ECompressionCodec::Zstd) {
                if (!DecompressData(chunk)) {
                    return;
                }
            } else {
                Buffer.Append(chunk.data(), chunk.size());
            }

            ProcessBuffer();
        } catch (const std::exception& ex) {
            Finish(false, TStringBuilder() << "Failed to read from data file: " << ex.what());
        }
    }

    bool DecompressData(const TString& compressed) {
        if (!ZstdContext) {
            Finish(false, "Zstd context not initialized");
            return false;
        }

        ZSTD_inBuffer input = { compressed.data(), compressed.size(), 0 };
        
        while (input.pos < input.size) {
            const size_t blockSize = AppData()->ZstdBlockSizeForTest.GetOrElse(ZSTD_BLOCKSIZE_MAX);
            if (Buffer.Size() + blockSize > BufferSizeLimit) {
                Finish(false, "Buffer size limit reached during decompression");
                return false;
            }
            
            Buffer.Reserve(Buffer.Size() + blockSize);
            ZSTD_outBuffer output = { Buffer.Data() + Buffer.Size(), Buffer.Capacity() - Buffer.Size(), 0 };
            
            size_t ret = ZSTD_decompressStream(ZstdContext.Get(), &output, &input);
            if (ZSTD_isError(ret)) {
                Finish(false, TStringBuilder() << "Zstd decompression error: " << ZSTD_getErrorName(ret));
                return false;
            }
            
            Buffer.Proceed(output.pos);
        }

        return true;
    }

    void ProcessBuffer() {
        Counters.LatencyProcess.Start(Now());

        // Find the last complete line (ending with \n)
        TStringBuf bufView(Buffer.Data(), Buffer.Size());
        const size_t lastNewline = bufView.rfind('\n');
        
        if (lastNewline == TString::npos) {
            // No complete line yet
            if (ReadBytes >= ContentLength) {
                // End of file, process remaining data if any
                if (Buffer.Size() > 0) {
                    if (!ProcessData(bufView)) {
                        return;
                    }
                    Buffer.Clear();
                }
                
                Counters.LatencyProcess.Finish(Now());
                
                if (!CheckChecksum()) {
                    return;
                }
                Finish();
                return;
            }
            
            // Need more data
            if (Buffer.Size() >= BufferSizeLimit) {
                Finish(false, "Buffer size limit reached without finding newline");
                return;
            }
            
            Counters.LatencyProcess.Finish(Now());
            ReadNextChunk();
            return;
        }

        TStringBuf dataToProcess = bufView.SubStr(0, lastNewline + 1);
        
        if (Checksum) {
            Checksum->AddData(dataToProcess);
        }

        RequestBuilder.New(TableInfo, Scheme);

        if (!ProcessData(dataToProcess)) {
            return;
        }

        // Keep unprocessed data in buffer
        const size_t processedSize = lastNewline + 1;
        ProcessedBytes += processedSize;
        Buffer.ChopHead(processedSize);

        Counters.LatencyProcess.Finish(Now());

        if (PendingRows > 0) {
            UploadRows();
        } else if (ReadBytes >= ContentLength) {
            if (!CheckChecksum()) {
                return;
            }
            Finish();
        } else {
            ReadNextChunk();
        }
    }

    bool ProcessData(TStringBuf data) {
        TMemoryPool pool(256);
        
        while (data) {
            pool.Clear();

            TStringBuf line = data.NextTok('\n');
            const TStringBuf origLine = line;

            if (!line) {
                if (data) {
                    continue; // skip empty line
                }
                break;
            }

            std::vector<std::pair<i32, NScheme::TTypeInfo>> columnOrderTypes;
            columnOrderTypes.reserve(Scheme.GetColumns().size());

            for (const auto& column : Scheme.GetColumns()) {
                auto typeInfoMod = NScheme::TypeInfoModFromProtoColumnType(column.GetTypeId(),
                    column.HasTypeInfo() ? &column.GetTypeInfo() : nullptr);
                columnOrderTypes.emplace_back(TableInfo.KeyOrder(column.GetName()), typeInfoMod.TypeInfo);
            }

            TVector<TCell> keys;
            TVector<TCell> values;
            TString strError;

            if (!NFormats::TYdbDump::ParseLine(line, columnOrderTypes, pool, keys, values, strError, PendingBytes)) {
                Finish(false, TStringBuilder() << strError << " on line: " << origLine);
                return false;
            }

            if (keys.empty()) {
                continue;
            }

            if (!TableInfo.IsMyKey(keys)) {
                Finish(false, TStringBuilder() << "Key is out of range on line: " << origLine);
                return false;
            }

            RequestBuilder.AddRow(keys, values);
            ++PendingRows;
        }

        return true;
    }

    void UploadRows() {
        const auto& record = RequestBuilder.GetRecord();

        IMPORT_LOG_I("Upload rows"
            << ": count# " << record->RowsSize()
            << ", size# " << record->ByteSizeLong());

        Counters.LatencyWrite.Start(Now());

        // Generate a pseudo-ETag for FS (use file path and size)
        TString pseudoETag = TStringBuilder() << DataFilePath << ":" << ContentLength;

        Send(DataShard, new TEvDataShard::TEvS3UploadRowsRequest(TxId, record, {
            pseudoETag, ProcessedBytes, WrittenBytes, WrittenRows, ProcessedChecksumState, DownloadState
        }));
    }

    void Handle(TEvDataShard::TEvS3UploadRowsResponse::TPtr& ev) {
        IMPORT_LOG_D("Handle TEvS3UploadRowsResponse"
            << ": status# " << ev->Get()->Record.GetStatus());

        *Counters.BytesWritten += RequestBuilder.GetCellBytes();
        Counters.LatencyWrite.Finish(Now());

        const auto& record = ev->Get()->Record;

        if (record.GetStatus() != NKikimrTxDataShard::TError::OK) {
            if (ev->Get()->IsRetriableError()) {
                return RetryOrFinish(record.GetErrorDescription());
            } else {
                return Finish(false, record.GetErrorDescription());
            }
        }

        // Update progress
        WrittenBytes += std::exchange(PendingBytes, 0);
        WrittenRows += std::exchange(PendingRows, 0);

        if (Checksum) {
            ProcessedChecksumState = Checksum->GetState();
        }

        // Continue reading
        if (ReadBytes >= ContentLength && Buffer.Size() == 0) {
            if (!CheckChecksum()) {
                return;
            }
            Finish();
        } else {
            ReadNextChunk();
        }
    }

    bool CheckScheme() {
        auto finish = [this](const TString& error) -> bool {
            IMPORT_LOG_E(error);
            Finish(false, error);

            return false;
        };

        for (const auto& column : Scheme.GetColumns()) {
            if (!TableInfo.HasColumn(column.GetName())) {
                return finish(TStringBuilder() << "Scheme mismatch: cannot find column"
                    << ": name# " << column.GetName());
            }

            const auto type = TableInfo.GetColumnType(column.GetName());
            auto schemeType = NScheme::TypeInfoModFromProtoColumnType(column.GetTypeId(),
                column.HasTypeInfo() ? &column.GetTypeInfo() : nullptr);
            if (type.first != schemeType.TypeInfo || type.second != schemeType.TypeMod) {
                return finish(TStringBuilder() << "Scheme mismatch: column type mismatch"
                    << ": name# " << column.GetName()
                    << ", expected# " << NScheme::TypeName(type.first, type.second)
                    << ", got# " << NScheme::TypeName(schemeType.TypeInfo, schemeType.TypeMod));
            }
        }

        if (TableInfo.GetKeyColumnIds().size() != (ui32)Scheme.KeyColumnNamesSize()) {
            return finish(TStringBuilder() << "Scheme mismatch: key column count mismatch"
                << ": expected# " << TableInfo.GetKeyColumnIds().size()
                << ", got# " << Scheme.KeyColumnNamesSize());
        }

        for (ui32 i = 0; i < (ui32)Scheme.KeyColumnNamesSize(); ++i) {
            const auto& name = Scheme.GetKeyColumnNames(i);
            const ui32 keyOrder = TableInfo.KeyOrder(name);

            if (keyOrder != i) {
                return finish(TStringBuilder() << "Scheme mismatch: key order mismatch"
                    << ": name# " << name
                    << ", expected# " << keyOrder
                    << ", got# " << i);
            }
        }

        return true;
    }

    bool CheckChecksum() {
        if (!ValidateChecksums || !Checksum) {
            return true;
        }

        TString gotChecksum = Checksum->Finalize();
        if (gotChecksum == ExpectedChecksum) {
            return true;
        }

        const TString error = TStringBuilder() << "Checksum mismatch for "
            << DataFilePath
            << " expected# " << ExpectedChecksum
            << ", got# " << gotChecksum;

        IMPORT_LOG_E(error);
        Finish(false, error);

        return false;
    }

    bool CanRetry(const TString&) const {
        return Attempt < Retries;
    }

    void Retry() {
        Delay = Min(Delay * ++Attempt, MaxDelay);
        const TDuration random = TDuration::FromValue(TAppData::RandomProvider->GenRand64() % Delay.MicroSeconds());
        Schedule(Delay + random, new TEvents::TEvWakeup());
    }

    void RetryOrFinish(const TString& error) {
        if (CanRetry(error)) {
            Retry();
        } else {
            Finish(false, error);
        }
    }

    void Finish(bool success = true, const TString& error = TString()) {
        IMPORT_LOG_N("Finish"
            << ": success# " << success
            << ", error# " << error
            << ", writtenBytes# " << WrittenBytes
            << ", writtenRows# " << WrittenRows);

        TAutoPtr<IDestructable> prod = new TImportJobProduct(success, error, WrittenBytes, WrittenRows);
        Send(DataShard, new TDataShard::TEvPrivate::TEvAsyncJobComplete(prod), 0, TxId);

        if (TaskId) {
            Send(MakeResourceBrokerID(), new TEvResourceBroker::TEvFinishTask(TaskId));
        }

        PassAway();
    }

    void NotifyDied() {
        Send(MakeResourceBrokerID(), new TEvResourceBroker::TEvNotifyActorDied());
        PassAway();
    }

    void HandleWakeup() {
        DataFileHandle.Reset();
        ZstdContext.Reset();
        Buffer.Clear();
        NoDataFile = false;
        CompressionCodec = ECompressionCodec::None;
        ReadBytes = 0;
        ProcessedBytes = 0;
        StartReading();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::IMPORT_S3_DOWNLOADER_ACTOR;
    }

    TStringBuf LogPrefix() const {
        return LogPrefix_;
    }

    explicit TFsDownloader(const TActorId& dataShard, ui64 txId, const NKikimrSchemeOp::TRestoreTask& task, const TTableInfo& tableInfo)
        : DataShard(dataShard)
        , TxId(txId)
        , Settings(TFsSettings::FromRestoreTask(task))
        , DataFormat(EDataFormat::Csv)
        , CompressionCodec(ECompressionCodec::None)
        , TableInfo(tableInfo)
        , Scheme(task.GetTableDescription())
        , LogPrefix_(TStringBuilder() << "fs:" << TxId)
        , Retries(task.GetNumberOfRetries())
        , ReadBatchSize(DefaultReadBatchSize)
        , BufferSizeLimit(AppData()->DataShardConfig.GetRestoreReadBufferSizeLimit() 
            ? AppData()->DataShardConfig.GetRestoreReadBufferSizeLimit() 
            : DefaultBufferSizeLimit)
        , ValidateChecksums(task.GetValidateChecksums())
        , Checksum(task.GetValidateChecksums() ? CreateChecksum() : nullptr)
        , ProcessedChecksumState(Checksum ? Checksum->GetState() : NKikimrBackup::TChecksumState())
        , Counters(GetServiceCounters(AppData()->Counters, "tablets")->GetSubgroup("subsystem", "import_fs"))
    {
    }

    void Bootstrap() {
        IMPORT_LOG_D("Bootstrap"
            << ": attempt# " << Attempt
            << ", basePath# " << Settings.BasePath
            << ", relativePath# " << Settings.RelativePath
            << ", shard# " << Settings.Shard);

        if (!CheckScheme()) {
            return;
        }

        AllocateResource();
    }

    STATEFN(StateAllocateResource) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvResourceBroker::TEvResourceAllocated, Handle);
            sFunc(TEvents::TEvPoisonPill, NotifyDied);
        }
    }

    STATEFN(StateReadData) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvDataShard::TEvS3UploadRowsResponse, Handle);

            sFunc(TEvents::TEvWakeup, HandleWakeup);
            sFunc(TEvents::TEvPoisonPill, NotifyDied);
        }
    }

private:
    const TActorId DataShard;
    const ui64 TxId;
    const TFsSettings Settings;
    const EDataFormat DataFormat;
    ECompressionCodec CompressionCodec;
    const TTableInfo TableInfo;
    const NKikimrSchemeOp::TTableDescription Scheme;
    const TString LogPrefix_;

    const ui32 Retries;
    ui32 Attempt = 0;

    TDuration Delay = TDuration::Minutes(1);
    static constexpr TDuration MaxDelay = TDuration::Minutes(10);

    ui64 TaskId = 0;

    TString DataFilePath;
    THolder<TFile> DataFileHandle;
    THolder<::ZSTD_DCtx, DestroyZCtx> ZstdContext;
    TBuffer Buffer;
    bool NoDataFile = false;
    
    const ui32 ReadBatchSize;
    const ui64 BufferSizeLimit;

    ui64 ContentLength = 0;
    ui64 ProcessedBytes = 0;
    ui64 ReadBytes = 0;
    ui64 WrittenBytes = 0;
    ui64 WrittenRows = 0;
    ui64 PendingBytes = 0;
    ui64 PendingRows = 0;
    NKikimrBackup::TS3DownloadState DownloadState;

    TUploadRowsRequestBuilder RequestBuilder;

    bool ValidateChecksums;
    NBackup::IChecksum::TPtr Checksum;
    NKikimrBackup::TChecksumState ProcessedChecksumState;
    TString ExpectedChecksum;

    TCounters Counters;

}; // TFsDownloader

IActor* CreateFsDownloader(const TActorId& dataShard, ui64 txId, const NKikimrSchemeOp::TRestoreTask& task, const TTableInfo& info) {
    return new TFsDownloader(dataShard, txId, task, info);
}

} // NDataShard
} // NKikimr
