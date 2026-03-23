#include <ydb/public/api/protos/ydb_export.pb.h>
#include <ydb/public/api/protos/ydb_topic.pb.h>

#include <ydb/core/backup/common/encryption.h>
#include <ydb/core/base/table_index.h>
#include <ydb/core/metering/metering.h>
#include <ydb/core/protos/s3_settings.pb.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/tablet_flat/shared_cache_events.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/testlib/audit_helpers/audit_helper.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/schemeshard/schemeshard_billing_helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/ut_backup_restore_common.h>
#include <ydb/core/util/aws.h>
#include <ydb/core/wrappers/s3_wrapper.h>
#include <ydb/core/wrappers/ut_helpers/s3_mock.h>
#include <ydb/library/testlib/helpers.h>

#include <library/cpp/testing/hook/hook.h>

#include <util/folder/dirut.h>
#include <util/folder/tempdir.h>
#include <util/stream/file.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/printf.h>
#include <util/system/env.h>

using namespace NSchemeShardUT_Private;
using namespace NKikimr::NWrappers::NTestHelpers;

using TTablesWithAttrs = TVector<std::pair<TString, TMap<TString, TString>>>;

using namespace NKikimr::Tests;

namespace {

    Y_TEST_HOOK_BEFORE_RUN(InitAwsAPI) {
        NKikimr::InitAwsAPI();
    }

    Y_TEST_HOOK_AFTER_RUN(ShutdownAwsAPI) {
        NKikimr::ShutdownAwsAPI();
    }

    void Run(TTestBasicRuntime& runtime, TTestEnv& env, const std::variant<TVector<TString>, TTablesWithAttrs>& tablesVar, const TString& request,
            Ydb::StatusIds::StatusCode expectedStatus = Ydb::StatusIds::SUCCESS,
            const TString& dbName = "/MyRoot", bool serverless = false, const TString& userSID = "", const TString& peerName = "",
            const TVector<TString>& cdcStreams = {}, bool checkAutoDropping = false) {

        TTablesWithAttrs tables;

        if (std::holds_alternative<TVector<TString>>(tablesVar)) {
            for (const auto& table : std::get<TVector<TString>>(tablesVar)) {
                tables.emplace_back(table, TMap<TString, TString>{});
            }
        } else {
            tables = std::get<TTablesWithAttrs>(tablesVar);
        }

        ui64 txId = 100;

        ui64 schemeshardId = TTestTxConfig::SchemeShard;
        if (dbName != "/MyRoot") {
            TestCreateExtSubDomain(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "%s"
            )", TStringBuf(serverless ? "/MyRoot/Shared" : dbName).RNextTok('/').data()));
            env.TestWaitNotification(runtime, txId);

            const auto describeResult = DescribePath(runtime, serverless ? "/MyRoot/Shared" : dbName);
            const auto subDomainPathId = describeResult.GetPathId();

            TestAlterExtSubDomain(runtime, ++txId, "/MyRoot", Sprintf(R"(
                PlanResolution: 50
                Coordinators: 1
                Mediators: 1
                TimeCastBucketsPerMediator: 2
                ExternalSchemeShard: true
                Name: "%s"
                StoragePools {
                  Name: "name_User_kind_hdd-1"
                  Kind: "common"
                }
                StoragePools {
                  Name: "name_User_kind_hdd-2"
                  Kind: "external"
                }
            )", TStringBuf(serverless ? "/MyRoot/Shared" : dbName).RNextTok('/').data()));
            env.TestWaitNotification(runtime, txId);

            if (serverless) {
                const auto attrs = AlterUserAttrs({
                    {"cloud_id", "CLOUD_ID_VAL"},
                    {"folder_id", "FOLDER_ID_VAL"},
                    {"database_id", "DATABASE_ID_VAL"}
                });

                TestCreateExtSubDomain(runtime, ++txId, "/MyRoot", Sprintf(R"(
                    Name: "%s"
                    ResourcesDomainKey {
                        SchemeShard: %lu
                        PathId: %lu
                    }
                )", TStringBuf(dbName).RNextTok('/').data(), TTestTxConfig::SchemeShard, subDomainPathId), attrs);
                env.TestWaitNotification(runtime, txId);

                TestAlterExtSubDomain(runtime, ++txId, "/MyRoot", Sprintf(R"(
                    PlanResolution: 50
                    Coordinators: 1
                    Mediators: 1
                    TimeCastBucketsPerMediator: 2
                    ExternalSchemeShard: true
                    ExternalHive: false
                    Name: "%s"
                    StoragePools {
                      Name: "name_User_kind_hdd-1"
                      Kind: "common"
                    }
                    StoragePools {
                      Name: "name_User_kind_hdd-2"
                      Kind: "external"
                    }
                )", TStringBuf(dbName).RNextTok('/').data()));
                env.TestWaitNotification(runtime, txId);
            }

            TestDescribeResult(DescribePath(runtime, dbName), {
                NLs::PathExist,
                NLs::ExtractTenantSchemeshard(&schemeshardId)
            });
        }

        for (const auto& [table, attrs] : tables) {
            TVector<std::pair<TString, TString>> attrsVec;
            attrsVec.assign(attrs.begin(), attrs.end());
            const auto userAttrs = AlterUserAttrs(attrsVec);
            TestCreateTable(runtime, schemeshardId, ++txId, dbName, table, {
                NKikimrScheme::StatusAccepted,
                NKikimrScheme::StatusAlreadyExists,
            }, userAttrs);
            env.TestWaitNotification(runtime, txId, schemeshardId);
        }

        for (const auto& cdcStream : cdcStreams) {
            TestCreateCdcStream(runtime, schemeshardId, ++txId, dbName, cdcStream);
            env.TestWaitNotification(runtime, txId, schemeshardId);
        }

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);

        const auto initialStatus = expectedStatus == Ydb::StatusIds::PRECONDITION_FAILED
            ? expectedStatus
            : Ydb::StatusIds::SUCCESS;
        TestExport(runtime, schemeshardId, ++txId, dbName, request, userSID, peerName, initialStatus);
        env.TestWaitNotification(runtime, txId, schemeshardId);

        if (initialStatus != Ydb::StatusIds::SUCCESS) {
            return;
        }

        const ui64 exportId = txId;
        TestGetExport(runtime, schemeshardId, exportId, dbName, expectedStatus);

        if (!runtime.GetAppData().FeatureFlags.GetEnableExportAutoDropping() && checkAutoDropping) {
          auto desc = DescribePath(runtime, "/MyRoot");
          Cerr << "desc: " << desc.GetPathDescription().ChildrenSize()<< Endl;
          UNIT_ASSERT(desc.GetPathDescription().ChildrenSize() > 1);

          bool foundExportDir = false;
          bool foundOriginalTable = false;

          for (size_t i = 0; i < desc.GetPathDescription().ChildrenSize(); ++i) {
              const auto& child = desc.GetPathDescription().GetChildren(i);
              const auto& name = child.GetName();

              if (name.StartsWith("Table")) {
                  foundOriginalTable = true;
              } else if (name.StartsWith("export-")) {
                  foundExportDir = true;
                  auto exportDirDesc = DescribePath(runtime, "/MyRoot/" + name);
                  UNIT_ASSERT(exportDirDesc.GetPathDescription().ChildrenSize() >= 1);
                  UNIT_ASSERT_EQUAL(exportDirDesc.GetPathDescription().GetChildren(0).GetName(), "0");
              }
          }

          UNIT_ASSERT(foundExportDir);
          UNIT_ASSERT(foundOriginalTable);
        } else if (checkAutoDropping) {
          auto desc = DescribePath(runtime, "/MyRoot");
          Cerr << "desc: " << desc.GetPathDescription().ChildrenSize()<< Endl;
          for (size_t i = 0; i < desc.GetPathDescription().ChildrenSize(); ++i) {
              const auto& child = desc.GetPathDescription().GetChildren(i);
              const auto& name = child.GetName();
              UNIT_ASSERT(!name.StartsWith("export-"));
          }
        }

        TestForgetExport(runtime, schemeshardId, ++txId, dbName, exportId);
        env.TestWaitNotification(runtime, exportId, schemeshardId);

        TestGetExport(runtime, schemeshardId, exportId, dbName, Ydb::StatusIds::NOT_FOUND);
    }

    const Ydb::Table::PartitioningSettings& GetPartitioningSettings(
        const Ydb::Table::CreateTableRequest& tableDescription
    ) {
        UNIT_ASSERT_C(tableDescription.has_partitioning_settings(), tableDescription.DebugString());
        return tableDescription.partitioning_settings();
    }

    const Ydb::Table::PartitioningSettings& GetIndexTablePartitioningSettings(
        const Ydb::Table::CreateTableRequest& tableDescription
    ) {
        UNIT_ASSERT_C(tableDescription.indexes_size(), tableDescription.DebugString());

        const auto& index = tableDescription.indexes(0);
        UNIT_ASSERT_C(index.has_global_index(), index.DebugString());
        UNIT_ASSERT_C(index.global_index().has_settings(), index.DebugString());

        const auto& settings = index.global_index().settings();
        UNIT_ASSERT_C(settings.has_partitioning_settings(), settings.DebugString());
        return settings.partitioning_settings();
    }

    // It might be an overkill to convert expectedString to expectedProto and back to .DebugString(),
    // but it allows us to ignore whitespace differences when comparing the protobufs.
    auto CreateProtoComparator(TString&& expectedString) {
        return [expectedString = std::move(expectedString)](const auto& proto) {
            std::decay_t<decltype(proto)> expectedProto;
            UNIT_ASSERT_C(
                google::protobuf::TextFormat::ParseFromString(expectedString, &expectedProto),
                expectedString
            );
            UNIT_ASSERT_STRINGS_EQUAL(proto.DebugString(), expectedProto.DebugString());
        };
    }

    void CheckTableScheme(const TString& scheme, auto&& fieldGetter, auto&& fieldChecker) {
        Ydb::Table::CreateTableRequest proto;
        UNIT_ASSERT_C(
            google::protobuf::TextFormat::ParseFromString(scheme, &proto),
            scheme
        );
        fieldChecker(fieldGetter(proto));
    }

    void CheckPermissions(const TString& permissions, auto&& fieldChecker) {
        Ydb::Scheme::ModifyPermissionsRequest proto;
        UNIT_ASSERT_C(
            google::protobuf::TextFormat::ParseFromString(permissions, &proto),
            permissions
        );
        fieldChecker(proto);
    }

    struct TExportItem {
        TString SourcePath;
        TString Destination;
    };

    TString MakeS3RequestItems(const TVector<TExportItem>& items) {
        TStringBuilder result;
        for (const auto& item : items) {
            result << "items { source_path: \"" << item.SourcePath << "\" destination_prefix: \"" << item.Destination << "\" } ";
        }
        return result;
    }

    TString MakeS3RequestTpl(const TVector<TExportItem>& items, const TString& extraSettings = "") {
        return TStringBuilder()
            << "ExportToS3Settings { endpoint: \"localhost:%d\" scheme: HTTP "
            << MakeS3RequestItems(items) << " "
            << extraSettings
            << " }";
    }

    TString MakeFsRequestItems(const TVector<TExportItem>& items) {
        TStringBuilder result;
        for (const auto& item : items) {
            result << "items { source_path: \"" << item.SourcePath << "\" destination_path: \"" << item.Destination << "\" } ";
        }
        return result;
    }

    TString MakeFsRequestTpl(const TVector<TExportItem>& items, const TString& extraSettings = "") {
        return TStringBuilder()
            << "ExportToFsSettings { base_path: \"%s\" "
            << MakeFsRequestItems(items) << " "
            << extraSettings
            << " }";
    }

    class TExportFixture : public NUnitTest::TBaseFixture {
    public:
        void ConfigureRuntime(bool isFs = false) {
            Env();
            Runtime().GetAppData().FeatureFlags.SetEnableEncryptedExport(true);
            Runtime().GetAppData().FeatureFlags.SetEnableViewExport(true);
            if (isFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
        }

        TString FsBasePath() {
            if (!FsTempDir_) {
                FsTempDir_.ConstructInPlace();
            }
            return FsTempDir_->Path();
        }

        TString MakeExportRequest(bool isFs, const TVector<TExportItem>& items, const TString& extraSettings = "") {
            if (isFs) {
                return Sprintf(MakeFsRequestTpl(items, extraSettings).c_str(), FsBasePath().c_str());
            } else {
                return Sprintf(MakeS3RequestTpl(items, extraSettings).c_str(), S3Port());
            }
        }

        template <bool IsFs>
        TString FilePrefix(const TString& sourcePath = "/MyRoot/Table") {
            if constexpr (IsFs) {
                auto pos = sourcePath.rfind('/');
                return (pos != TString::npos) ? TString("/") + sourcePath.substr(pos + 1) : TString("/") + sourcePath;
            } else {
                return "";
            }
        }

        template <bool IsFs>
        void RunExport(const TVector<TString>& tables, const TVector<TExportItem>& items,
                       Ydb::StatusIds::StatusCode expectedStatus = Ydb::StatusIds::SUCCESS,
                       bool checkFilesExistence = true, const TString& extraSettings = "") {
            TString requestStr = MakeExportRequest(IsFs, items, extraSettings);

            NKikimrExport::TCreateExportRequest request;
            UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(requestStr, &request));

            ConfigureRuntime(IsFs);

            Run(Runtime(), Env(), tables, requestStr, expectedStatus, "/MyRoot", false);

            bool hasEncryption = request.HasExportToS3Settings() && request.GetExportToS3Settings().has_encryption_settings();
            auto calcPath = [&](const TString& targetPath, const TString& file) {
                TString canonPath = (targetPath.StartsWith("/") || targetPath.empty()) ? targetPath : TString("/") + targetPath;
                TString result = canonPath;
                result += '/';
                result += file;
                if (hasEncryption) {
                    result += ".enc";
                }
                return result;
            };

            if (expectedStatus == Ydb::StatusIds::SUCCESS && checkFilesExistence) {
                for (auto& path : GetExportTargetPaths(requestStr)) {
                    UNIT_ASSERT_C(HasFile<IsFs>(calcPath(path, "metadata.json")), calcPath(path, "metadata.json"));
                    UNIT_ASSERT_C(HasFile<IsFs>(calcPath(path, "scheme.pb")), calcPath(path, "scheme.pb"));
                }
            }
        }

        void RunS3(const TVector<TString>& tables, const TString& requestTpl, Ydb::StatusIds::StatusCode expectedStatus = Ydb::StatusIds::SUCCESS, bool checkS3FilesExistence = true) {
            auto requestStr = Sprintf(requestTpl.c_str(), S3Port());
            NKikimrExport::TCreateExportRequest request;
            UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(requestStr, &request));

            Env(); // Init test env
            Runtime().GetAppData().FeatureFlags.SetEnableEncryptedExport(true);

            Run(Runtime(), Env(), tables, requestStr, expectedStatus, "/MyRoot", false);

            auto calcPath = [&](const TString& targetPath, const TString& file) {
                TString canonPath = (targetPath.StartsWith("/") || targetPath.empty()) ? targetPath : TString("/") + targetPath;
                TString result = canonPath;
                result += '/';
                result += file;
                if (request.GetExportToS3Settings().has_encryption_settings()) {
                    result += ".enc";
                }
                return result;
            };

            if (expectedStatus == Ydb::StatusIds::SUCCESS && checkS3FilesExistence) {
                for (auto& path : GetExportTargetPaths(requestStr)) {
                    UNIT_ASSERT_C(HasS3File(calcPath(path, "metadata.json")), calcPath(path, "metadata.json"));
                    UNIT_ASSERT_C(HasS3File(calcPath(path, "scheme.pb")), calcPath(path, "scheme.pb"));
                }
            }
        }

        template <bool IsFs>
        bool HasFile(const TString& path) {
            if constexpr (IsFs) {
                return HasFsFile(path);
            } else {
                return HasS3File(path);
            }
        }

        template <bool IsFs>
        TString GetFileContent(const TString& path) {
            if constexpr (IsFs) {
                return GetFsFileContent(path);
            } else {
                return GetS3FileContent(path);
            }
        }

        template <bool IsFs>
        THashMap<TString, TString> GetAllFiles() {
            if constexpr (IsFs) {
                return GetAllFsFiles();
            } else {
                return S3Mock().GetData();
            }
        }

        template <bool IsFs>
        size_t FileCount() {
            return GetAllFiles<IsFs>().size();
        }

        template <bool IsFs, class T>
        void CheckHasAllFiles(std::initializer_list<T> paths) {
            for (const T& path : paths) {
                UNIT_ASSERT_C(HasFile<IsFs>(path), "Path \"" << path << "\" is expected to exist");
            }
        }

        template <bool IsFs, class T>
        void CheckNoSuchFiles(std::initializer_list<T> paths) {
            for (const T& path : paths) {
                UNIT_ASSERT_C(!HasFile<IsFs>(path), "Path \"" << path << "\" is expected not to exist");
            }
        }

        template <bool IsFs, class T>
        void CheckNoPrefix(std::initializer_list<T> prefixes) {
            auto files = GetAllFiles<IsFs>();
            for (const T& prefix : prefixes) {
                for (auto&& [fileName, _] : files) {
                    UNIT_ASSERT_C(!fileName.StartsWith(prefix), "Path \"" << fileName << "\" has prefix \"" << prefix << "\", which is not expected prefix");
                }
            }
        }

        template <bool IsFs>
        void CheckPathWithChecksum(const TString& path) {
            UNIT_ASSERT(HasFile<IsFs>(path));
            UNIT_ASSERT(HasFile<IsFs>(path + ".sha256"));
        }

        bool HasS3File(const TString& path) {
            auto it = S3Mock().GetData().find(path);
            return it != S3Mock().GetData().end();
        }

        template <class T>
        void CheckHasAllS3Files(std::initializer_list<T> paths) {
            for (const T& path : paths) {
                UNIT_ASSERT_C(HasS3File(path), "Path \"" << path << "\" is expected to exist in S3");
            }
        }

        template <class T>
        void CheckNoSuchS3Files(std::initializer_list<T> paths) {
            for (const T& path : paths) {
                UNIT_ASSERT_C(!HasS3File(path), "Path \"" << path << "\" is expected not to exist in S3");
            }
        }

        template <class T>
        void CheckNoS3Prefix(std::initializer_list<T> prefixes) {
            for (const T& prefix : prefixes) {
                for (auto&& [fileName, _] : S3Mock().GetData()) {
                    UNIT_ASSERT_C(!fileName.StartsWith(prefix), "S3 path \"" << fileName << "\" has prefix \"" << prefix << "\", which is not expected prefix");
                }
            }
        }

        TString GetS3FileContent(const TString& path) {
            auto it = S3Mock().GetData().find(path);
            if (it != S3Mock().GetData().end()) {
                return it->second;
            }
            return {};
        }

        bool HasFsFile(const TString& path) {
            TString fullPath = FsBasePath() + path;
            return TFsPath(fullPath).Exists();
        }

        TString GetFsFileContent(const TString& path) {
            TString fullPath = FsBasePath() + path;
            if (!TFsPath(fullPath).Exists()) {
                return {};
            }
            return TFileInput(fullPath).ReadAll();
        }

        THashMap<TString, TString> GetAllFsFiles() {
            THashMap<TString, TString> result;
            TString basePath = FsBasePath();
            TVector<TString> files;
            TFsPath(basePath).ListNames(files);
            CollectFsFiles(basePath, basePath, result);
            return result;
        }

        void TearDown(NUnitTest::TTestContext&) override {
            if (S3ServerMock) {
                S3ServerMock = Nothing();
                S3ServerPort = 0;
            }
            FsTempDir_.Clear();
        }

        using TDelayFunc = std::function<bool(TAutoPtr<IEventHandle>&)>;

        void Cancel(const TVector<TString>& tables, const TString& request, TDelayFunc delayFunc, bool isFs = false) {
            std::vector<std::string> auditLines;
            Runtime().AuditLogBackends = std::move(CreateTestAuditLogBackends(auditLines));

            Env(); // Init test env
            if (isFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
            ui64 txId = 100;

            for (const auto& table : tables) {
                TestCreateTable(Runtime(), ++txId, "/MyRoot", table);
                Env().TestWaitNotification(Runtime(), txId);
            }

            Runtime().SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
            Runtime().SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);

            THolder<IEventHandle> delayed;
            auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
                if (delayFunc(ev)) {
                    delayed.Reset(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                return TTestActorRuntime::EEventAction::PROCESS;
            });

            TestExport(Runtime(), ++txId, "/MyRoot", request);
            const ui64 exportId = txId;

            // Check audit record for export start
            {
                auto line = FindAuditLine(auditLines, "operation=EXPORT START");
                UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
                UNIT_ASSERT_STRING_CONTAINS(line, "operation=EXPORT START");
                UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", exportId));
                UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=");  // can't check the value
                UNIT_ASSERT_STRING_CONTAINS(line, "subject={none}");
                UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
                UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
                UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
                UNIT_ASSERT(!line.contains("reason"));
                UNIT_ASSERT(!line.contains("start_time"));
                UNIT_ASSERT(!line.contains("end_time"));
            }

            if (!delayed) {
                TDispatchOptions opts;
                opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                    return bool(delayed);
                });
                Runtime().DispatchEvents(opts);
            }

            Runtime().SetObserverFunc(prevObserver);

            TestCancelExport(Runtime(), ++txId, "/MyRoot", exportId);
            Runtime().Send(delayed.Release(), 0, true);
            Env().TestWaitNotification(Runtime(), exportId);

            // Check audit record for export end
            //
            {
                auto line = FindAuditLine(auditLines, "operation=EXPORT END");
                UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
                UNIT_ASSERT_STRING_CONTAINS(line, "operation=EXPORT END");
                UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", exportId));
                UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=");  // can't check the value
                UNIT_ASSERT_STRING_CONTAINS(line, "subject={none}");
                UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
                UNIT_ASSERT_STRING_CONTAINS(line, "status=ERROR");
                UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=CANCELLED");
                UNIT_ASSERT_STRING_CONTAINS(line, "reason=Cancelled");
                UNIT_ASSERT_STRING_CONTAINS(line, "start_time=");
                UNIT_ASSERT_STRING_CONTAINS(line, "end_time=");
            }

            TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::CANCELLED);

            TestForgetExport(Runtime(), ++txId, "/MyRoot", exportId);
            Env().TestWaitNotification(Runtime(), exportId);

            TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
        }

        void CancelUponTransferringShouldSucceed(const TVector<TString>& tables, const TString& request) {
            Cancel(tables, Sprintf(request.c_str(), S3Port()), [](TAutoPtr<IEventHandle>& ev) {
                if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                    return false;
                }

                return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                    .GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpBackup;
            });
        }

        template <bool IsFs>
        void CancelUponTransferringShouldSucceed(const TVector<TString>& tables, const TVector<TExportItem>& items) {
            TString request = MakeExportRequest(IsFs, items);
            Cancel(tables, request, [](TAutoPtr<IEventHandle>& ev) {
                if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                    return false;
                }

                return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                    .GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpBackup;
            }, IsFs);
        }

        void CancelShouldSucceed(TDelayFunc delayFunc) {
            Cancel({
                R"(
                    Name: "Table"
                    Columns { Name: "key" Type: "Utf8" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                )",
            }, Sprintf(R"(
                ExportToS3Settings {
                  endpoint: "localhost:%d"
                  scheme: HTTP
                  items {
                    source_path: "/MyRoot/Table"
                    destination_prefix: ""
                  }
                }
            )", S3Port()), delayFunc);
        }

        template <bool IsFs>
        void CancelShouldSucceed(TDelayFunc delayFunc) {
            TString request = MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});
            Cancel({
                R"(
                    Name: "Table"
                    Columns { Name: "key" Type: "Utf8" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                )",
            }, request, delayFunc, IsFs);
        }

        void DropCopiesBeforeTransferring(ui32 tablesCount) {
            Env(); // Init test env
            ui64 txId = 100;

            for (ui32 i = 1; i <= tablesCount; ++i) {
                TestCreateTable(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
                    Name: "Table%d"
                    Columns { Name: "key" Type: "Utf8" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                )", i));
                Env().TestWaitNotification(Runtime(), txId);
            }

            Runtime().SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
            Runtime().SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);

            bool dropNotification = false;
            THolder<IEventHandle> delayed;
            auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
                switch (ev->GetTypeRewrite()) {
                case TEvSchemeShard::EvModifySchemeTransaction:
                    break;
                case TEvSchemeShard::EvNotifyTxCompletionResult:
                    if (dropNotification) {
                        delayed.Reset(ev.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    return TTestActorRuntime::EEventAction::PROCESS;
                default:
                    return TTestActorRuntime::EEventAction::PROCESS;
                }

                const auto* msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
                if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables) {
                    dropNotification = true;
                }

                return TTestActorRuntime::EEventAction::PROCESS;
            });

            TStringBuilder items;
            for (ui32 i = 1; i <= tablesCount; ++i) {
                items << "items {"
                    << " source_path: \"/MyRoot/Table" << i << "\""
                    << " destination_prefix: \"\""
                << " }";
            }

            TestExport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
                ExportToS3Settings {
                  endpoint: "localhost:%d"
                  scheme: HTTP
                  %s
                }
            )", S3Port(), items.c_str()));
            const ui64 exportId = txId;

            if (!delayed) {
                TDispatchOptions opts;
                opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                    return bool(delayed);
                });
                Runtime().DispatchEvents(opts);
            }

            Runtime().SetObserverFunc(prevObserver);

            for (ui32 i = 0; i < tablesCount; ++i) {
                TestDropTable(Runtime(), ++txId, Sprintf("/MyRoot/export-%" PRIu64, exportId), ToString(i));
                Env().TestWaitNotification(Runtime(), txId);
            }

            Runtime().Send(delayed.Release(), 0, true);
            Env().TestWaitNotification(Runtime(), exportId);

            TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::CANCELLED);

            TestForgetExport(Runtime(), ++txId, "/MyRoot", exportId);
            Env().TestWaitNotification(Runtime(), exportId);

            TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
        }

        void RebootDuringFinish(bool rejectUploadParts, Ydb::StatusIds::StatusCode expectedStatus) {
            S3Settings().WithRejectUploadParts(rejectUploadParts);

            Env(); // Init test env
            ui64 txId = 100;

            TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
                Name: "Table"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )");
            Env().TestWaitNotification(Runtime(), txId);

            UpdateRow(Runtime(), "Table", 1, "valueA");
            UpdateRow(Runtime(), "Table", 2, "valueB");

            Runtime().SetLogPriority(NKikimrServices::S3_WRAPPER, NActors::NLog::PRI_TRACE);
            Runtime().SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
            Runtime().SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);

            TMaybe<ui64> backupTxId;
            TMaybe<ui64> tabletId;
            bool delayed = false;

            auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
                switch (ev->GetTypeRewrite()) {
                    case TEvDataShard::EvProposeTransaction: {
                        auto& record = ev->Get<TEvDataShard::TEvProposeTransaction>()->Record;
                        if (record.GetTxKind() != NKikimrTxDataShard::ETransactionKind::TX_KIND_SCHEME) {
                            return TTestActorRuntime::EEventAction::PROCESS;
                        }

                        NKikimrTxDataShard::TFlatSchemeTransaction schemeTx;
                        UNIT_ASSERT(schemeTx.ParseFromString(record.GetTxBody()));

                        if (schemeTx.HasBackup()) {
                            backupTxId = record.GetTxId();
                            // hijack
                            schemeTx.MutableBackup()->MutableScanSettings()->SetRowsBatchSize(1);
                            schemeTx.MutableBackup()->MutableS3Settings()->MutableLimits()->SetMinWriteBatchSize(1);
                            record.SetTxBody(schemeTx.SerializeAsString());
                        }

                        return TTestActorRuntime::EEventAction::PROCESS;
                    }

                    case TEvDataShard::EvProposeTransactionResult: {
                        if (!backupTxId) {
                            return TTestActorRuntime::EEventAction::PROCESS;
                        }

                        const auto& record = ev->Get<TEvDataShard::TEvProposeTransactionResult>()->Record;
                        if (record.GetTxId() != *backupTxId) {
                            return TTestActorRuntime::EEventAction::PROCESS;
                        }

                        tabletId = record.GetOrigin();
                        return TTestActorRuntime::EEventAction::PROCESS;
                    }

                    case NWrappers::NExternalStorage::EvCompleteMultipartUploadRequest:
                    case NWrappers::NExternalStorage::EvAbortMultipartUploadRequest:
                        delayed = true;
                        return TTestActorRuntime::EEventAction::DROP;

                    default:
                        return TTestActorRuntime::EEventAction::PROCESS;
                }
            });

            TestExport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
                ExportToS3Settings {
                  endpoint: "localhost:%d"
                  scheme: HTTP
                  items {
                    source_path: "/MyRoot/Table"
                    destination_prefix: ""
                  }
                }
            )", S3Port()));
            const ui64 exportId = txId;

            if (!delayed || !tabletId) {
                TDispatchOptions opts;
                opts.FinalEvents.emplace_back([&delayed, &tabletId](IEventHandle&) -> bool {
                    return delayed && tabletId;
                });
                Runtime().DispatchEvents(opts);
            }

            Runtime().SetObserverFunc(prevObserver);

            RebootTablet(Runtime(), *tabletId, Runtime().AllocateEdgeActor());
            Env().TestWaitNotification(Runtime(), exportId);

            TestGetExport(Runtime(), exportId, "/MyRoot", expectedStatus);

            TestForgetExport(Runtime(), ++txId, "/MyRoot", exportId);
            Env().TestWaitNotification(Runtime(), exportId);

            TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
        }

        void ShouldCheckQuotas(const TSchemeLimits& limits, Ydb::StatusIds::StatusCode expectedFailStatus) {
            ShouldCheckQuotasImpl<false>(limits, expectedFailStatus);
        }

        template <bool IsFs>
        void ShouldCheckQuotasImpl(const TSchemeLimits& limits, Ydb::StatusIds::StatusCode expectedFailStatus) {
            const TString userSID = "user@builtin";
            EnvOptions().SystemBackupSIDs({userSID});
            ConfigureRuntime(IsFs);

            SetSchemeshardSchemaLimits(Runtime(), limits);

            const TVector<TString> tables = {
                R"(
                    Name: "Table"
                    Columns { Name: "key" Type: "Utf8" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                )",
            };
            const TString request = MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});

            Run(Runtime(), Env(), tables, request, expectedFailStatus);
            Run(Runtime(), Env(), tables, request, Ydb::StatusIds::SUCCESS, "/MyRoot", false, userSID);
        }

        void TestTopic(bool enablePermissions = false, ui64 topicsCount = 1, ui64 consumersCount = 0) {
            TestTopicImpl<false>(enablePermissions, topicsCount, consumersCount);
        }

        template <bool IsFs>
        void TestTopicImpl(bool enablePermissions = false, ui64 topicsCount = 1, ui64 consumersCount = 0) {
            EnvOptions().EnablePermissionsExport(enablePermissions);
            Env();
            if constexpr (IsFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
            ui64 txId = 100;

            TVector<TExportItem> items;
            TVector<NDescUT::TSimpleTopic> expected;

            for (ui64 i = 0; i < topicsCount; ++i) {
                auto topic = NDescUT::TSimpleTopic(i, (topicsCount == 1 || i > 0) ? consumersCount : 0);
                TestCreatePQGroup(Runtime(), ++txId, "/MyRoot", topic.GetPrivateProto().DebugString());
                Env().TestWaitNotification(Runtime(), txId);
                TString topicName = Sprintf("Topic_%lu", i);
                items.push_back({TStringBuilder() << "/MyRoot/" << topicName, topicName});
                expected.push_back(topic);
            }

            TString request = MakeExportRequest(IsFs, items);

            auto schemeshardId = TTestTxConfig::SchemeShard;
            TestExport(Runtime(), schemeshardId, ++txId, "/MyRoot", request, "", "", Ydb::StatusIds::SUCCESS);
            Env().TestWaitNotification(Runtime(), txId, schemeshardId);

            TestGetExport(Runtime(), schemeshardId, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

            for (ui64 i = 0; i < topicsCount; ++i) {
                const auto& topicExpected = expected.at(i);
                const auto& topicPath = topicExpected.GetPath();
                UNIT_ASSERT(HasFile<IsFs>(topicPath));
                auto content = GetFileContent<IsFs>(topicPath);
                UNIT_ASSERT_C(topicExpected.CompareWithStringIgnoringFields(content, {"attributes"}),
                    TStringBuilder() << topicExpected.GetPublicProto().DebugString() << "\n\nVS\n\n" << content);

                if (enablePermissions) {
                    auto permissionsPath = topicExpected.GetPermissions().GetPath();
                    UNIT_ASSERT(HasFile<IsFs>(permissionsPath));
                    UNIT_ASSERT(topicExpected.GetPermissions().CompareWithString(GetFileContent<IsFs>(permissionsPath)));
                }
            }
        }

        void CheckPathWithChecksum(const TString& path) {
            UNIT_ASSERT(HasS3File(path));
            UNIT_ASSERT(HasS3File(path + ".sha256"));
        }

        void TestReplication(const TString& scheme, const TString& expected) {
            TestReplicationImpl<false>(scheme, expected);
        }

        template <bool IsFs>
        void TestReplicationImpl(const TString& scheme, const TString& expected) {
            auto options = TTestEnvOptions()
                .InitYdbDriver(true);
            TTestEnv env(Runtime(), options);
            if constexpr (IsFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
            ui64 txId = 100;

            TestCreateReplication(Runtime(), ++txId, "/MyRoot", scheme);
            env.TestWaitNotification(Runtime(), txId);

            TString request = MakeExportRequest(IsFs, {{"/MyRoot/Replication", "Replication"}});

            TestExport(Runtime(), ++txId, "/MyRoot", request);
            env.TestWaitNotification(Runtime(), txId);

            TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

            CheckPathWithChecksum<IsFs>("/Replication/create_async_replication.sql");
            const auto content = GetFileContent<IsFs>("/Replication/create_async_replication.sql");
            UNIT_ASSERT_EQUAL_C(
                content, expected,
                TStringBuilder() << "\nExpected:\n\n" << expected << "\n\nActual:\n\n" << content);

            CheckPathWithChecksum<IsFs>("/Replication/permissions.pb");
            const auto permissions = GetFileContent<IsFs>("/Replication/permissions.pb");
            const auto permissions_expected = "actions {\n  change_owner: \"root@builtin\"\n}\n";
            UNIT_ASSERT_EQUAL_C(
                permissions, permissions_expected,
                TStringBuilder() << "\nExpected:\n\n" << permissions_expected << "\n\nActual:\n\n" << permissions);

        }

        void TestTransfer(const TString& scheme, const TString& expected) {
            TestTransferImpl<false>(scheme, expected);
        }

        template <bool IsFs>
        void TestTransferImpl(const TString& scheme, const TString& expected) {
            auto options = TTestEnvOptions()
                .InitYdbDriver(true);
            TTestEnv env(Runtime(), options);
            if constexpr (IsFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
            ui64 txId = 100;

            TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )");
            env.TestWaitNotification(Runtime(), txId);

            auto topic = NDescUT::TSimpleTopic(0, 0);
            TestCreatePQGroup(Runtime(), ++txId, "/MyRoot", topic.GetPrivateProto().DebugString());
            env.TestWaitNotification(Runtime(), txId);

            TestCreateTransfer(Runtime(), ++txId, "/MyRoot", scheme);
            env.TestWaitNotification(Runtime(), txId);

            TString request = MakeExportRequest(IsFs, {{"/MyRoot/Transfer", "Transfer"}});

            TestExport(Runtime(), ++txId, "/MyRoot", request);
            env.TestWaitNotification(Runtime(), txId);

            TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

            CheckPathWithChecksum<IsFs>("/Transfer/create_transfer.sql");
            const auto content = GetFileContent<IsFs>("/Transfer/create_transfer.sql");
            UNIT_ASSERT_EQUAL_C(
                content, expected,
                TStringBuilder() << "\nExpected:\n\n" << expected << "\n\nActual:\n\n" << content);

            CheckPathWithChecksum<IsFs>("/Transfer/permissions.pb");
            const auto permissions = GetFileContent<IsFs>("/Transfer/permissions.pb");
            const auto permissions_expected = "actions {\n  change_owner: \"root@builtin\"\n}\n";
            UNIT_ASSERT_EQUAL_C(
                permissions, permissions_expected,
                TStringBuilder() << "\nExpected:\n\n" << permissions_expected << "\n\nActual:\n\n" << permissions);

        }

        void TestExternalDataSource(
            const TString& scheme,
            const TVector<TString>& expectedProperties)
        {
            TestExternalDataSourceImpl<false>(scheme, expectedProperties);
        }

        template <bool IsFs>
        void TestExternalDataSourceImpl(
            const TString& scheme,
            const TVector<TString>& expectedProperties)
        {
            Env();
            if constexpr (IsFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
            ui64 txId = 100;

            TestCreateExternalDataSource(Runtime(), ++txId, "/MyRoot", scheme);
            Env().TestWaitNotification(Runtime(), txId);

            TString request = MakeExportRequest(IsFs, {{"/MyRoot/DataSource", "DataSource"}});

            TestExport(Runtime(), ++txId, "/MyRoot", request);
            Env().TestWaitNotification(Runtime(), txId);

            TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

            CheckPathWithChecksum<IsFs>("/DataSource/create_external_data_source.sql");
            const auto content = GetFileContent<IsFs>("/DataSource/create_external_data_source.sql");

            TString expectedHeader = "-- database: \"/MyRoot\"\n"
                "CREATE EXTERNAL DATA SOURCE IF NOT EXISTS `DataSource`\n"
                "WITH (";
            UNIT_ASSERT_C(content.find(expectedHeader) != TString::npos,
                TStringBuilder() << "\nExpected query to start from:\n\n"
                    << expectedHeader << "\n\nActual query:\n\n" << content);

            for (const auto& property : expectedProperties) {
                UNIT_ASSERT_C(content.find(property) != TString::npos,
                    TStringBuilder() << "Property not found:\n"
                    << "\nExpected property:\n\n" << property << "\n\nActual query:\n\n" << content);
            }

            UNIT_ASSERT_EQUAL_C(
                std::ranges::count(content, ','),
                static_cast<long>(expectedProperties.size()) - 1,
                "Properties count mismatch");

            CheckPathWithChecksum<IsFs>("/DataSource/permissions.pb");
            const auto permissions = GetFileContent<IsFs>("/DataSource/permissions.pb");
            const auto permissions_expected = "actions {\n  change_owner: \"root@builtin\"\n}\n";
            UNIT_ASSERT_EQUAL_C(
                permissions, permissions_expected,
                TStringBuilder() << "\nExpected:\n\n" << permissions_expected << "\n\nActual:\n\n" << permissions);
        }

        void TestExternalTable(
            const TString& scheme,
            const TString& expectedStartsWith,
            const TVector<TString>& expectedProperties)
        {
            TestExternalTableImpl<false>(scheme, expectedStartsWith, expectedProperties);
        }

        template <bool IsFs>
        void TestExternalTableImpl(
            const TString& scheme,
            const TString& expectedStartsWith,
            const TVector<TString>& expectedProperties)
        {
            Env();
            if constexpr (IsFs) {
                Runtime().GetAppData().FeatureFlags.SetEnableFsBackups(true);
            }
            ui64 txId = 100;

            const auto dataSourceScheme = R"(
                Name: "DataSource"
                SourceType: "ObjectStorage"
                Location: "https://s3.cloud.net/bucket"
                Auth {
                    Aws {
                        AwsAccessKeyIdSecretName: "id_secret",
                        AwsSecretAccessKeySecretName: "access_secret"
                        AwsRegion: "ru-central-1"
                    }
                }
            )";

            TestCreateExternalDataSource(Runtime(), ++txId, "/MyRoot", dataSourceScheme);
            Env().TestWaitNotification(Runtime(), txId);

            TestCreateExternalTable(Runtime(), ++txId, "/MyRoot", scheme);
            Env().TestWaitNotification(Runtime(), txId);

            TString request = MakeExportRequest(IsFs, {{"/MyRoot/ExternalTable", "ExternalTable"}});

            TestExport(Runtime(), ++txId, "/MyRoot", request);
            Env().TestWaitNotification(Runtime(), txId);

            TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

            CheckPathWithChecksum<IsFs>("/ExternalTable/create_external_table.sql");
            const auto content = GetFileContent<IsFs>("/ExternalTable/create_external_table.sql");

            UNIT_ASSERT_C(content.find(expectedStartsWith) != TString::npos,
                TStringBuilder() << "\nExpected query to start with:\n\n"
                    << expectedStartsWith << "\n\nActual query:\n\n" << content);

            for (const auto& property : expectedProperties) {
                UNIT_ASSERT_C(content.find(property) != TString::npos,
                    TStringBuilder() << "Property not found:\n"
                    << "\nExpected property:\n\n" << property << "\n\nActual query:\n\n" << content);
            }

            UNIT_ASSERT_EQUAL_C(
                std::ranges::count(content, '='),
                static_cast<long>(expectedProperties.size()),
                TStringBuilder() << "Properties count mismatch: ");

            CheckPathWithChecksum<IsFs>("/ExternalTable/permissions.pb");
            const auto permissions = GetFileContent<IsFs>("/ExternalTable/permissions.pb");
            const auto permissions_expected = "actions {\n  change_owner: \"root@builtin\"\n}\n";
            UNIT_ASSERT_EQUAL_C(
                permissions, permissions_expected,
                TStringBuilder() << "\nExpected:\n\n" << permissions_expected << "\n\nActual:\n\n" << permissions);
        }

        void TestIcb() {
            auto options = TTestEnvOptions()
                .InitYdbDriver(true);
            TTestEnv env(Runtime(), options);
            ui64 txId = 100;

            TestCreateReplication(Runtime(), ++txId, "/MyRoot", R"(
                Name: "Replication"
                Config {
                    SrcConnectionParams {
                        Endpoint: "localhost:2135"
                        Database: "/MyRoot"
                        StaticCredentials {
                            User: "user"
                            Password: "pwd"
                            PasswordSecretName: "pwd-secret-name"
                        }
                    }
                    Specific {
                        Targets {
                            SrcPath: "/MyRoot/Table1"
                            DstPath: "/MyRoot/Table1Replica"
                        }
                    }
                }
            )");
            env.TestWaitNotification(Runtime(), txId);

            TString request = Sprintf(R"(
                ExportToS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_path: "/MyRoot/Replication"
                        destination_prefix: "Replication"
                    }
                }
            )", S3Port());

            TControlBoard::SetValue(0, Runtime().GetAppData().Icb->BackupControls.S3Controls.EnableAsyncReplicationExport);

            TestExport(Runtime(), ++txId, "/MyRoot", request, "", "", Ydb::StatusIds::BAD_REQUEST);
            env.TestWaitNotification(Runtime(), txId);
        }

    protected:
        TS3Mock::TSettings& S3Settings() {
            if (!S3ServerSettings) {
                S3ServerPort = PortManager.GetPort();
                S3ServerSettings.ConstructInPlace(S3ServerPort);
            }
            return *S3ServerSettings;
        }

        TS3Mock& S3Mock() {
            if (!S3ServerMock) {
                S3ServerMock.ConstructInPlace(S3Settings());
                UNIT_ASSERT(S3ServerMock->Start());
            }
            return *S3ServerMock;
        }

        ui16 S3Port() {
            S3Mock();
            return S3ServerPort;
        }

        TTestBasicRuntime& Runtime() {
            if (!TestRuntime) {
                TestRuntime.ConstructInPlace();
            }
            return *TestRuntime;
        }

        TTestEnvOptions& EnvOptions() {
            if (!TestEnvOptions) {
                TestEnvOptions.ConstructInPlace();
            }
            return *TestEnvOptions;
        }

        TTestEnv& Env() {
            if (!TestEnv) {
                TestEnv.ConstructInPlace(Runtime(), EnvOptions());
            }
            return *TestEnv;
        }

    private:
        void CollectFsFiles(const TString& basePath, const TString& currentPath, THashMap<TString, TString>& result) {
            TFsPath dir(currentPath);
            if (!dir.Exists()) return;

            TVector<TFsPath> children;
            dir.List(children);
            for (const auto& child : children) {
                if (child.IsFile()) {
                    TString relPath = child.GetPath().substr(basePath.size());
                    if (!relPath.StartsWith("/")) {
                        relPath = "/" + relPath;
                    }
                    result[relPath] = TFileInput(child.GetPath()).ReadAll();
                } else if (child.IsDirectory()) {
                    CollectFsFiles(basePath, child.GetPath(), result);
                }
            }
        }

        TMaybe<TTempDir> FsTempDir_;
        TPortManager PortManager;
        ui16 S3ServerPort = 0;
        TMaybe<TTestBasicRuntime> TestRuntime;
        TMaybe<TS3Mock::TSettings> S3ServerSettings;
        TMaybe<TS3Mock> S3ServerMock;
        TMaybe<TTestEnvOptions> TestEnvOptions;
        TMaybe<TTestEnv> TestEnv;
    };

} // anonymous

Y_UNIT_TEST_SUITE_F(TExportToS3Tests, TExportFixture) {
    Y_UNIT_TEST_TWIN(ShouldSucceedOnSingleShardTable, IsFs) {
        RunExport<IsFs>({
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, {{"/MyRoot/Table", ""}});
    }

    Y_UNIT_TEST_TWIN(ShouldSucceedOnMultiShardTable, IsFs) {
        RunExport<IsFs>({
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                UniformPartitionsCount: 2
            )",
        }, {{"/MyRoot/Table", ""}});
    }

    Y_UNIT_TEST_TWIN(ShouldSucceedOnManyTables, IsFs) {
        RunExport<IsFs>({
            R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
            R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, {
            {"/MyRoot/Table1", "table1"},
            {"/MyRoot/Table2", "table2"},
        });
    }

    Y_UNIT_TEST_TWIN(ShouldOmitNonStrictStorageSettings, IsFs) {
        const TVector<TString> tables = {R"(
            Name: "Table"
            Columns {
              Name: "key"
              Type: "Utf8"
              DefaultFromLiteral {
                type {
                  optional_type {
                    item {
                      type_id: UTF8
                    }
                  }
                }
                value {
                  items {
                    text_value: "b"
                  }
                }
              }
            }
            Columns {
              Name: "value"
              Type: "Utf8"
              DefaultFromLiteral {
                type {
                  optional_type {
                    item {
                      type_id: UTF8
                    }
                  }
                }
                value {
                  items {
                    text_value: "a"
                  }
                }
              }
            }
            KeyColumnNames: ["key"]
            PartitionConfig {
              ColumnFamilies {
                Id: 0
                StorageConfig {
                  SysLog {
                    PreferredPoolKind: "hdd-1"
                    AllowOtherKinds: true
                  }
                  Log {
                    PreferredPoolKind: "hdd-1"
                    AllowOtherKinds: true
                  }
                  Data {
                    PreferredPoolKind: "hdd-1"
                    AllowOtherKinds: true
                  }
                }
              }
            }
        )"};

        ConfigureRuntime(IsFs);
        Run(Runtime(), Env(), tables, MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        auto allFiles = GetAllFiles<IsFs>();
        auto schemeIt = allFiles.find(FilePrefix<IsFs>() + "/scheme.pb");
        UNIT_ASSERT(schemeIt != allFiles.end());
        TString scheme = schemeIt->second;

        UNIT_ASSERT_NO_DIFF(scheme, R"(columns {
  name: "key"
  type {
    optional_type {
      item {
        type_id: UTF8
      }
    }
  }
  from_literal {
    type {
      optional_type {
        item {
          type_id: UTF8
        }
      }
    }
    value {
      items {
        text_value: "b"
      }
    }
  }
}
columns {
  name: "value"
  type {
    optional_type {
      item {
        type_id: UTF8
      }
    }
  }
  from_literal {
    type {
      optional_type {
        item {
          type_id: UTF8
        }
      }
    }
    value {
      items {
        text_value: "a"
      }
    }
  }
}
primary_key: "key"
storage_settings {
  store_external_blobs: DISABLED
}
column_families {
  name: "default"
  compression: COMPRESSION_NONE
}
partitioning_settings {
  partitioning_by_size: DISABLED
  partitioning_by_load: DISABLED
  min_partitions_count: 1
}
)");
    }

    Y_UNIT_TEST_TWIN(ShouldPreserveIncrBackupFlag, IsFs) {
        const TTablesWithAttrs tables{
            {
                R"(
                Name: "Table"
                Columns {
                  Name: "key"
                  Type: "Utf8"
                  DefaultFromLiteral {
                    type {
                      optional_type {
                        item {
                          type_id: UTF8
                        }
                      }
                    }
                    value {
                      items {
                        text_value: "b"
                      }
                    }
                  }
                }
                Columns {
                  Name: "value"
                  Type: "Utf8"
                  DefaultFromLiteral {
                    type {
                      optional_type {
                        item {
                          type_id: UTF8
                        }
                      }
                    }
                    value {
                      items {
                        text_value: "a"
                      }
                    }
                  }
                }
                KeyColumnNames: ["key"]
                )",
                {{"__incremental_backup", "{}"}},
            },
        };

        ConfigureRuntime(IsFs);
        Run(Runtime(), Env(), tables, MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        auto allFiles = GetAllFiles<IsFs>();
        auto schemeIt = allFiles.find(FilePrefix<IsFs>() + "/scheme.pb");
        UNIT_ASSERT(schemeIt != allFiles.end());
        TString scheme = schemeIt->second;

        UNIT_ASSERT_NO_DIFF(scheme, R"(columns {
  name: "key"
  type {
    optional_type {
      item {
        type_id: UTF8
      }
    }
  }
  from_literal {
    type {
      optional_type {
        item {
          type_id: UTF8
        }
      }
    }
    value {
      items {
        text_value: "b"
      }
    }
  }
}
columns {
  name: "value"
  type {
    optional_type {
      item {
        type_id: UTF8
      }
    }
  }
  from_literal {
    type {
      optional_type {
        item {
          type_id: UTF8
        }
      }
    }
    value {
      items {
        text_value: "a"
      }
    }
  }
}
primary_key: "key"
attributes {
  key: "__incremental_backup"
  value: "{}"
}
partitioning_settings {
  partitioning_by_size: DISABLED
  partitioning_by_load: DISABLED
  min_partitions_count: 1
}
)");
    }

    Y_UNIT_TEST_TWIN(CancelUponCreatingExportDirShouldSucceed, IsFs) {
        CancelShouldSucceed<IsFs>([](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpMkDir;
        });
    }

    Y_UNIT_TEST_TWIN(CancelUponCopyingTablesShouldSucceed, IsFs) {
        CancelShouldSucceed<IsFs>([](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables;
        });
    }

    Y_UNIT_TEST_TWIN(CancelUponTransferringSingleShardTableShouldSucceed, IsFs) {
        CancelUponTransferringShouldSucceed<IsFs>({
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, {{"/MyRoot/Table", ""}});
    }

    Y_UNIT_TEST_TWIN(CancelUponTransferringMultiShardTableShouldSucceed, IsFs) {
        CancelUponTransferringShouldSucceed<IsFs>({
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                UniformPartitionsCount: 2
            )",
        }, {{"/MyRoot/Table", ""}});
    }

    Y_UNIT_TEST_TWIN(CancelUponTransferringSingleTableShouldSucceed, IsFs) {
        // same as CancelUponTransferringSingleShardTableShouldSucceed
    }

    Y_UNIT_TEST_TWIN(CancelUponTransferringManyTablesShouldSucceed, IsFs) {
        CancelUponTransferringShouldSucceed<IsFs>({
            R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
            R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, {
            {"/MyRoot/Table1", "table1"},
            {"/MyRoot/Table2", "table2"},
        });
    }

    Y_UNIT_TEST_TWIN(DropSourceTableBeforeTransferring, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        Runtime().SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        Runtime().SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);

        bool dropNotification = false;
        THolder<IEventHandle> delayed;
        auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
            case TEvSchemeShard::EvModifySchemeTransaction:
                break;
            case TEvSchemeShard::EvNotifyTxCompletionResult:
                if (dropNotification) {
                    delayed.Reset(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                return TTestActorRuntime::EEventAction::PROCESS;
            default:
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            const auto* msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
            if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables) {
                dropNotification = true;
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
        const ui64 exportId = txId;

        if (!delayed) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                return bool(delayed);
            });
            Runtime().DispatchEvents(opts);
        }

        Runtime().SetObserverFunc(prevObserver);

        TestDropTable(Runtime(), ++txId, "/MyRoot", "Table");
        Env().TestWaitNotification(Runtime(), txId);

        Runtime().Send(delayed.Release(), 0, true);
        Env().TestWaitNotification(Runtime(), exportId);

        TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::CANCELLED);

        TestForgetExport(Runtime(), ++txId, "/MyRoot", exportId);
        Env().TestWaitNotification(Runtime(), exportId);

        TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
    }

    Y_UNIT_TEST(DropCopiesBeforeTransferring1) {
        DropCopiesBeforeTransferring(1);
    }

    Y_UNIT_TEST(DropCopiesBeforeTransferring2) {
        DropCopiesBeforeTransferring(2);
    }

    Y_UNIT_TEST(RebootDuringCompletion) {
        RebootDuringFinish(false, Ydb::StatusIds::SUCCESS);
    }

    Y_UNIT_TEST(RebootDuringAbortion) {
        RebootDuringFinish(true, Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST_TWIN(ShouldExcludeBackupTableFromStats, IsFs) {
        EnvOptions().DisableStatsBatching(true);
        Env(); // Init test env
        ui64 txId = 100;

        THashSet<ui64> statsCollected;
        Runtime().GetAppData().FeatureFlags.SetEnableExportAutoDropping(true);
        ConfigureRuntime(IsFs);
        Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvDataShard::EvPeriodicTableStats) {
                statsCollected.insert(ev->Get<TEvDataShard::TEvPeriodicTableStats>()->Record.GetDatashardId());
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        });

        auto waitForStats = [&](ui32 count) {
            statsCollected.clear();

            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&](IEventHandle&) -> bool {
                return statsCollected.size() == count;
            });
            Runtime().DispatchEvents(opts);

            return DescribePath(Runtime(), "/MyRoot")
                .GetPathDescription()
                .GetDomainDescription()
                .GetDiskSpaceUsage();
        };

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        for (int i = 1; i < 500; ++i) {
            UpdateRow(Runtime(), "Table", i, "value");
        }

        // trigger memtable's compaction
        TestCopyTable(Runtime(), ++txId, "/MyRoot", "CopyTable", "/MyRoot/Table");
        Env().TestWaitNotification(Runtime(), txId);
        TestDropTable(Runtime(), ++txId, "/MyRoot", "Table");
        Env().TestWaitNotification(Runtime(), txId);

        const auto expected = waitForStats(1);

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/CopyTable", ""}}));
        const ui64 exportId = txId;
        ::NKikimrSubDomains::TDiskSpaceUsage afterExport;

        TTestActorRuntime::TEventObserver prevObserverFunc;
        prevObserverFunc = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& event) {
            if (auto* p = event->CastAsLocal<TEvSchemeShard::TEvModifySchemeTransaction>()) {
                auto& record = p->Record;
                if (record.TransactionSize() >= 1 &&
                    record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpDropTable) {
                    afterExport = waitForStats(2);
                }
            }
            return prevObserverFunc(event);
        });

        Env().TestWaitNotification(Runtime(), exportId);

        TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::SUCCESS);

        UNIT_ASSERT_STRINGS_EQUAL(expected.DebugString(), afterExport.DebugString());

        TestForgetExport(Runtime(), ++txId, "/MyRoot", exportId);
        Env().TestWaitNotification(Runtime(), exportId);

        TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
        const auto afterForget = waitForStats(1);
        UNIT_ASSERT_STRINGS_EQUAL(expected.DebugString(), afterForget.DebugString());
    }

    Y_UNIT_TEST(CheckItemProgress) {
        Env(); // Init test env
        ui64 txId = 100;
        Runtime().GetAppData().FeatureFlags.SetEnableExportAutoDropping(true);
        TBlockEvents<NKikimr::NWrappers::NExternalStorage::TEvPutObjectRequest> blockPartition01(Runtime(), [](auto&& ev) {
            return ev->Get()->Request.GetKey() == "/data_01.csv";
        });

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
            SplitBoundary { KeyPrefix { Tuple { Optional { Uint32: 10 } }}}
        )");
        Env().TestWaitNotification(Runtime(), txId);

        WriteRow(Runtime(), ++txId, "/MyRoot/Table", 0, 1, "v1");
        Env().TestWaitNotification(Runtime(), txId);
        WriteRow(Runtime(), ++txId, "/MyRoot/Table", 1, 100, "v100");
        Env().TestWaitNotification(Runtime(), txId);

        TestExport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: ""
              }
            }
        )", S3Port()));

        Runtime().WaitFor("put object request from 01 partition", [&]{ return blockPartition01.size() >= 1; });
        bool isCompleted = false;

        while (!isCompleted) {
            const auto desc = TestGetExport(Runtime(), txId, "/MyRoot");
            const auto entry = desc.GetResponse().GetEntry();
            const auto& item = entry.GetItemsProgress(0);

            if (item.parts_completed() > 0) {
                isCompleted = true;
                UNIT_ASSERT_VALUES_EQUAL(item.parts_total(), 2);
                UNIT_ASSERT_VALUES_EQUAL(item.parts_completed(), 1);
                UNIT_ASSERT(item.has_start_time());
            } else {
                Runtime().SimulateSleep(TDuration::Seconds(1));
            }
        }

        blockPartition01.Stop();
        blockPartition01.Unblock();

        Env().TestWaitNotification(Runtime(), txId);

        const auto desc = TestGetExport(Runtime(), txId, "/MyRoot");
        const auto entry = desc.GetResponse().GetEntry();

        UNIT_ASSERT_VALUES_EQUAL(entry.ItemsProgressSize(), 1);
    }

    Y_UNIT_TEST_TWIN(ShouldRestartOnScanErrors, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        UpdateRow(Runtime(), "Table", 1, "valueA");

        THolder<IEventHandle> injectResult;
        auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == NSharedCache::EvResult) {
                const auto* msg = ev->Get<NSharedCache::TEvResult>();
                UNIT_ASSERT_VALUES_EQUAL(msg->Status, NKikimrProto::OK);

                auto result = MakeHolder<NSharedCache::TEvResult>(msg->PageCollection, NKikimrProto::ERROR, msg->Cookie);
                std::move(msg->Pages.begin(), msg->Pages.end(), std::back_inserter(result->Pages));

                injectResult = MakeHolder<IEventHandle>(ev->Recipient, ev->Sender, result.Release(), ev->Flags, ev->Cookie);
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        if (!injectResult) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&injectResult](IEventHandle&) -> bool {
                return bool(injectResult);
            });
            Runtime().DispatchEvents(opts);
        }

        Runtime().SetObserverFunc(prevObserver);
        Runtime().Send(injectResult.Release(), 0, true);

        Env().TestWaitNotification(Runtime(), txId);
        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
    }

    Y_UNIT_TEST_TWIN(ShouldSucceedOnConcurrentTxs, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        THolder<IEventHandle> copyTables;
        auto origObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto& record = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
                if (record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables) {
                    copyTables.Reset(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        const auto exportId = ++txId;
        TestExport(Runtime(), exportId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        if (!copyTables) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&copyTables](IEventHandle&) -> bool {
                return bool(copyTables);
            });
            Runtime().DispatchEvents(opts);
        }

        THolder<IEventHandle> proposeTxResult;
        Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvDataShard::EvProposeTransactionResult) {
                proposeTxResult.Reset(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestAlterTable(Runtime(), ++txId, "/MyRoot", R"(
              Name: "Table"
              Columns { Name: "extra"  Type: "Utf8"}
        )");

        if (!proposeTxResult) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&proposeTxResult](IEventHandle&) -> bool {
                return bool(proposeTxResult);
            });
            Runtime().DispatchEvents(opts);
        }

        Runtime().SetObserverFunc(origObserver);
        Runtime().Send(copyTables.Release(), 0, true);
        Runtime().Send(proposeTxResult.Release(), 0, true);
        Env().TestWaitNotification(Runtime(), txId);

        Env().TestWaitNotification(Runtime(), exportId);
        TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::SUCCESS);
    }

    Y_UNIT_TEST_TWIN(ShouldSucceedOnConcurrentExport, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TVector<THolder<IEventHandle>> copyTables;
        auto origObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto& record = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
                if (record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables) {
                    copyTables.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });
        auto waitCopyTables = [this, &copyTables](ui32 size) {
            if (copyTables.size() != size) {
                TDispatchOptions opts;
                opts.FinalEvents.emplace_back([&copyTables, size](IEventHandle&) -> bool {
                    return copyTables.size() == size;
                });
                Runtime().DispatchEvents(opts);
            }
        };

        TVector<ui64> exportIds;
        for (ui32 i = 1; i <= 3; ++i) {
            exportIds.push_back(++txId);
            TestExport(Runtime(), exportIds[i - 1], "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", Sprintf("Table%u", i)}}));
            waitCopyTables(i);
        }

        Runtime().SetObserverFunc(origObserver);
        for (auto& ev : copyTables) {
            Runtime().Send(ev.Release(), 0, true);
        }

        for (ui64 exportId : exportIds) {
            Env().TestWaitNotification(Runtime(), exportId);
            TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        }
    }

    Y_UNIT_TEST(ShouldSucceedOnConcurrentImport) {
        Env(); // Init test env
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        // prepare backup data
        TestExport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: "Backup1"
              }
            }
        )", S3Port()));
        Env().TestWaitNotification(Runtime(), txId);
        TestGetExport(Runtime(), txId, "/MyRoot");

        TVector<THolder<IEventHandle>> delayed;
        auto origObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto& record = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record;
                const auto opType = record.GetTransaction(0).GetOperationType();
                switch (opType) {
                case NKikimrSchemeOp::ESchemeOpRestore:
                case NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables:
                    delayed.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                default:
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        auto waitForDelayed = [this, &delayed](ui32 size) {
            if (delayed.size() != size) {
                TDispatchOptions opts;
                opts.FinalEvents.emplace_back([&delayed, size](IEventHandle&) -> bool {
                    return delayed.size() == size;
                });
                Runtime().DispatchEvents(opts);
            }
        };

        const auto importId = ++txId;
        TestImport(Runtime(), importId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "Backup1"
                destination_path: "/MyRoot/Restored"
              }
            }
        )", S3Port()));
        // wait for restore op
        waitForDelayed(1);

        const auto exportId = ++txId;
        TestExport(Runtime(), exportId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Restored"
                destination_prefix: "Backup2"
              }
            }
        )", S3Port()));
        // wait for copy table op
        waitForDelayed(2);

        Runtime().SetObserverFunc(origObserver);
        for (auto& ev : delayed) {
            Runtime().Send(ev.Release(), 0, true);
        }

        Env().TestWaitNotification(Runtime(), importId);
        TestGetImport(Runtime(), importId, "/MyRoot");
        Env().TestWaitNotification(Runtime(), exportId);
        TestGetExport(Runtime(), exportId, "/MyRoot");
    }

    Y_UNIT_TEST_TWIN(ShouldCheckQuotasExportsLimited, IsFs) {
        ShouldCheckQuotasImpl<IsFs>(TSchemeLimits{.MaxExports = 0}, Ydb::StatusIds::PRECONDITION_FAILED);
    }

    Y_UNIT_TEST_TWIN(ShouldCheckQuotasChildrenLimited, IsFs) {
        ShouldCheckQuotasImpl<IsFs>(TSchemeLimits{.MaxChildrenInDir = 2}, Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(ShouldRetryAtFinalStage) {
        Env(); // Init test env
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        UpdateRow(Runtime(), "Table", 1, "valueA");
        UpdateRow(Runtime(), "Table", 2, "valueB");
        Runtime().SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_DEBUG);

        THolder<IEventHandle> injectResult;
        auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvDataShard::EvProposeTransaction: {
                    auto& record = ev->Get<TEvDataShard::TEvProposeTransaction>()->Record;
                    if (record.GetTxKind() != NKikimrTxDataShard::ETransactionKind::TX_KIND_SCHEME) {
                        return TTestActorRuntime::EEventAction::PROCESS;
                    }

                    NKikimrTxDataShard::TFlatSchemeTransaction schemeTx;
                    UNIT_ASSERT(schemeTx.ParseFromString(record.GetTxBody()));

                    if (schemeTx.HasBackup()) {
                        schemeTx.MutableBackup()->MutableScanSettings()->SetRowsBatchSize(1);
                        schemeTx.MutableBackup()->MutableS3Settings()->MutableLimits()->SetMinWriteBatchSize(1);
                        record.SetTxBody(schemeTx.SerializeAsString());
                    }

                    return TTestActorRuntime::EEventAction::PROCESS;
                }

                case NWrappers::NExternalStorage::EvCompleteMultipartUploadResponse: {
                    auto response = MakeHolder<NWrappers::NExternalStorage::TEvCompleteMultipartUploadResponse>(
                        std::nullopt,
                        Aws::Utils::Outcome<Aws::S3::Model::CompleteMultipartUploadResult, Aws::S3::S3Error>(
                            Aws::Client::AWSError<Aws::S3::S3Errors>(Aws::S3::S3Errors::SLOW_DOWN, true)
                        )
                    );
                    injectResult = MakeHolder<IEventHandle>(ev->Recipient, ev->Sender, response.Release(), ev->Flags, ev->Cookie);
                    return TTestActorRuntime::EEventAction::DROP;
                }

                default: {
                    return TTestActorRuntime::EEventAction::PROCESS;
                }
            }
        });

        const auto exportId = ++txId;
        TestExport(Runtime(), txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              number_of_retries: 10
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: ""
              }
            }
        )", S3Port()));

        if (!injectResult) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&injectResult](IEventHandle&) -> bool {
                return bool(injectResult);
            });
            Runtime().DispatchEvents(opts);
        }

        Runtime().SetObserverFunc(prevObserver);
        Runtime().Send(injectResult.Release(), 0, true);

        Env().TestWaitNotification(Runtime(), exportId);
        TestGetExport(Runtime(), exportId, "/MyRoot");
    }

    Y_UNIT_TEST_TWIN(CorruptedDyNumber, IsFs) {
        EnvOptions().DisableStatsBatching(true);
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "DyNumber" }
                KeyColumnNames: ["key"]
            )");
        Env().TestWaitNotification(Runtime(), txId);

        // Write bad DyNumber
        UploadRow(Runtime(), "/MyRoot/Table", 0, {1}, {2}, {TCell::Make(1u)}, {TCell::Make(1u)});

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
        Env().TestWaitNotification(Runtime(), txId);

        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST_TWIN(UidAsIdempotencyKey, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        const auto request = TStringBuilder()
            << R"(OperationParams { labels { key: "uid" value: "foo" } } )"
            << MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});

        // create operation
        TestExport(Runtime(), ++txId, "/MyRoot", request);
        const ui64 exportId = txId;
        // create operation again with same uid
        TestExport(Runtime(), ++txId, "/MyRoot", request);
        // new operation was not created
        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
        // check previous operation
        TestGetExport(Runtime(), exportId, "/MyRoot");
        Env().TestWaitNotification(Runtime(), exportId);
    }

    Y_UNIT_TEST_TWIN(ExportStartTime, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        Runtime().UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        const auto desc = TestGetExport(Runtime(), txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_PREPARING);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(!entry.HasEndTime());
    }

    Y_UNIT_TEST_TWIN(CompletedExportEndTime, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        Runtime().UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        Runtime().AdvanceCurrentTime(TDuration::Seconds(30)); // doing export

        Env().TestWaitNotification(Runtime(), txId);

        const auto desc = TestGetExport(Runtime(), txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_DONE);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());
    }

    struct TWaitExportItemStateDelayFunc {
        bool GotModify = false;
        bool GotModifyResult = false;

        bool operator()(TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                GotModify |= ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                    .GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpBackup;
            }

            GotModifyResult |= GotModify && ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransactionResult;
            return GotModifyResult && ev->GetTypeRewrite() == TEvSchemeShard::EvNotifyTxCompletion;
        }
    };

    Y_UNIT_TEST_TWIN(CancelledExportEndTime, IsFs) {
        ConfigureRuntime(IsFs);
        Runtime().UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TWaitExportItemStateDelayFunc delayFunc;
        THolder<IEventHandle> delayed;
        auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (delayFunc(ev)) {
                delayed.Reset(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
        const ui64 exportId = txId;

        Runtime().AdvanceCurrentTime(TDuration::Seconds(30)); // doing export

        if (!delayed) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                return bool(delayed);
            });
            Runtime().DispatchEvents(opts);
        }
        // Block TEvSchemeShard::TEvCancelTxResult
        THolder<IEventHandle> cancelAck;
        Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvCancelTxResult) {
                cancelAck.Reset(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestCancelExport(Runtime(), ++txId, "/MyRoot", exportId);

        auto desc = TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        auto entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_CANCELLATION);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(!entry.HasEndTime());

        Runtime().SetObserverFunc(prevObserver);
        Runtime().Send(delayed.Release(), 0, true);
        if (cancelAck) {
            Runtime().Send(cancelAck.Release(), 0, true);
        }
        Env().TestWaitNotification(Runtime(), exportId);

        desc = TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_CANCELLED);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());
    }

    // Based on CompletedExportEndTime
    Y_UNIT_TEST_TWIN(AuditCompletedExport, IsFs) {
        std::vector<std::string> auditLines;
        Runtime().AuditLogBackends = std::move(CreateTestAuditLogBackends(auditLines));
        ConfigureRuntime(IsFs);
        Runtime().UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        // Prepare table to export
        //
        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        // Start export
        //
        const auto request = TStringBuilder()
            << R"(OperationParams { labels { key: "uid" value: "foo" } } )"
            << MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});
        TestExport(Runtime(), ++txId, "/MyRoot", request, /*userSID*/ "user@builtin", /*peerName*/ "127.0.0.1:9876");

        // Check audit record for export start
        {
            auto line = FindAuditLine(auditLines, "operation=EXPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=EXPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", txId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT(!line.contains("start_time"));
            UNIT_ASSERT(!line.contains("end_time"));
        }

        // Do export
        //
        Runtime().AdvanceCurrentTime(TDuration::Seconds(30));

        Env().TestWaitNotification(Runtime(), txId);

        const auto desc = TestGetExport(Runtime(), txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_DONE);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());

        // Check audit record for export end
        //
        {
            auto line = FindAuditLine(auditLines, "operation=EXPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=EXPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", txId));
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT_STRING_CONTAINS(line, "start_time=");
            UNIT_ASSERT_STRING_CONTAINS(line, "end_time=");
        }
    }

    Y_UNIT_TEST_TWIN(AuditCancelledExport, IsFs) {
        std::vector<std::string> auditLines;
        Runtime().AuditLogBackends = std::move(CreateTestAuditLogBackends(auditLines));
        ConfigureRuntime(IsFs);
        Runtime().UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        // Prepare table to export
        //
        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TWaitExportItemStateDelayFunc delayFunc;
        THolder<IEventHandle> delayed;
        auto prevObserver = Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (delayFunc(ev)) {
                delayed.Reset(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        // Start export
        //
        const auto request = TStringBuilder()
            << R"(OperationParams { labels { key: "uid" value: "foo" } } )"
            << MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});
        TestExport(Runtime(), ++txId, "/MyRoot", request, /*userSID*/ "user@builtin", /*peerName*/ "127.0.0.1:9876");
        const ui64 exportId = txId;

        // Check audit record for export start
        {
            auto line = FindAuditLine(auditLines, "operation=EXPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=EXPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", exportId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT(!line.contains("start_time"));
            UNIT_ASSERT(!line.contains("end_time"));
        }

        // Do export (unsuccessfully)
        //
        Runtime().AdvanceCurrentTime(TDuration::Seconds(30));

        if (!delayed) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                return bool(delayed);
            });
            Runtime().DispatchEvents(opts);
        }

        // Block TEvSchemeShard::TEvCancelTxResult
        THolder<IEventHandle> cancelAck;
        Runtime().SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvCancelTxResult) {
                cancelAck.Reset(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        // Cancel export mid-air
        //
        TestCancelExport(Runtime(), ++txId, "/MyRoot", exportId);

        auto desc = TestGetExport(Runtime(), exportId, "/MyRoot");
        auto entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_CANCELLATION);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(!entry.HasEndTime());

        Runtime().SetObserverFunc(prevObserver);
        Runtime().Send(delayed.Release(), 0, true);
        if (cancelAck) {
            Runtime().Send(cancelAck.Release(), 0, true);
        }
        Env().TestWaitNotification(Runtime(), exportId);

        desc = TestGetExport(Runtime(), exportId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_CANCELLED);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());

        // Check audit record for export end
        //
        {
            auto line = FindAuditLine(auditLines, "operation=EXPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=EXPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", exportId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");  // can't check the value
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=ERROR");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=CANCELLED");
            UNIT_ASSERT_STRING_CONTAINS(line, "reason=Cancelled");
            UNIT_ASSERT_STRING_CONTAINS(line, "start_time=");
            UNIT_ASSERT_STRING_CONTAINS(line, "end_time=");
        }
    }

    Y_UNIT_TEST_TWIN(ExportPartitioningSettings, IsFs) {
        ConfigureRuntime(IsFs);
        Run(Runtime(), Env(), TVector<TString>{
                R"(
                    Name: "Table"
                    Columns { Name: "key" Type: "Uint32" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                    PartitionConfig {
                      PartitioningPolicy {
                        MinPartitionsCount: 10
                        SplitByLoadSettings: {
                          Enabled: true
                        }
                      }
                    }
                )"
            },
            MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}})
        );

        auto allFiles = GetAllFiles<IsFs>(); auto* scheme = allFiles.FindPtr(FilePrefix<IsFs>() + "/scheme.pb");
        UNIT_ASSERT(scheme);
        CheckTableScheme(*scheme, GetPartitioningSettings, CreateProtoComparator(R"(
            partitioning_by_size: DISABLED
            partitioning_by_load: ENABLED
            min_partitions_count: 10
        )"));
    }

    Y_UNIT_TEST_TWIN(ExportIndexTablePartitioningSettings, IsFs) {
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateIndexedTable(Runtime(), ++txId, "/MyRoot", R"(
            TableDescription {
              Name: "Table"
              Columns { Name: "key" Type: "Uint32" }
              Columns { Name: "value" Type: "Utf8" }
              KeyColumnNames: ["key"]
            }
            IndexDescription {
              Name: "ByValue"
              KeyColumnNames: ["value"]
              IndexImplTableDescriptions: [ {
                PartitionConfig {
                  PartitioningPolicy {
                    MinPartitionsCount: 10
                    SplitByLoadSettings: {
                      Enabled: true
                    }
                  }
                }
              } ]
            }
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
        Env().TestWaitNotification(Runtime(), txId);

        auto allFiles = GetAllFiles<IsFs>(); auto* scheme = allFiles.FindPtr(FilePrefix<IsFs>() + "/scheme.pb");
        UNIT_ASSERT(scheme);
        CheckTableScheme(*scheme, GetIndexTablePartitioningSettings, CreateProtoComparator(R"(
            partitioning_by_size: DISABLED
            partitioning_by_load: ENABLED
            min_partitions_count: 10
        )"));
    }

    Y_UNIT_TEST_TWIN(UserSID, IsFs) {
        Env(); // Init test env
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        const TString request = MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});
        const TString userSID = "user@builtin";
        TestExport(Runtime(), ++txId, "/MyRoot", request, userSID);

        const auto desc = TestGetExport(Runtime(), txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Export::ExportProgress::PROGRESS_PREPARING);
        UNIT_ASSERT_VALUES_EQUAL(entry.GetUserSID(), userSID);
    }

    Y_UNIT_TEST_TWIN(TablePermissions, IsFs) {
        EnvOptions().EnablePermissionsExport(true);
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        NACLib::TDiffACL diffACL;
        diffACL.AddAccess(NACLib::EAccessType::Allow, NACLib::GenericUse, "user@builtin", NACLib::InheritNone);
        TestModifyACL(Runtime(), ++txId, "/MyRoot", "Table", diffACL.SerializeAsString(), "user@builtin");
        Env().TestWaitNotification(Runtime(), txId);

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
        Env().TestWaitNotification(Runtime(), txId);

        auto allFiles = GetAllFiles<IsFs>(); auto* permissions = allFiles.FindPtr(FilePrefix<IsFs>() + "/permissions.pb");
        UNIT_ASSERT(permissions);
        CheckPermissions(*permissions, CreateProtoComparator(R"(
            actions {
              change_owner: "user@builtin"
            }
            actions {
              grant {
                subject: "user@builtin"
                permission_names: "ydb.generic.use"
              }
            }
        )"));
    }

    Y_UNIT_TEST_TWIN(Checksums, IsFs) {
        EnvOptions().EnablePermissionsExport(true).EnableChecksumsExport(true);
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        UploadRow(Runtime(), "/MyRoot/Table", 0, {1}, {2}, {TCell::Make(1u)}, {TCell::Make(1u)});

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
        Env().TestWaitNotification(Runtime(), txId);

        UNIT_ASSERT_VALUES_EQUAL(FileCount<IsFs>(), 8);
        auto allFiles = GetAllFiles<IsFs>();
        const TString p = FilePrefix<IsFs>();
        const auto* dataChecksum = allFiles.FindPtr(p + "/data_00.csv.sha256");
        UNIT_ASSERT(dataChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*dataChecksum, "19dcd641390a61063ee45f3e6e06b8f0d3acfc33f934b9bf1ba204668a98f21d data_00.csv");

        const auto* metadataChecksum = allFiles.FindPtr(p + "/metadata.json.sha256");
        UNIT_ASSERT(metadataChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*metadataChecksum, "a5a7ca9bce00ac9d7e5b48a30a46f139592845cad0664b3fda92af32583b7d52 metadata.json");

        const auto* schemeChecksum = allFiles.FindPtr(p + "/scheme.pb.sha256");
        UNIT_ASSERT(schemeChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*schemeChecksum, "cb1fb80965ae92e6369acda2b3b5921fd5518c97d6437f467ce00492907f9eb6 scheme.pb");

        const auto* permissionsChecksum = allFiles.FindPtr(p + "/permissions.pb.sha256");
        UNIT_ASSERT(permissionsChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*permissionsChecksum, "b41fd8921ff3a7314d9c702dc0e71aace6af8443e0102add0432895c5e50a326 permissions.pb");
    }

    Y_UNIT_TEST_TWIN(EnableChecksumsPersistance, IsFs) {
        EnvOptions().EnablePermissionsExport(true).EnableChecksumsExport(true);
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        // Create test table
        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        // Add some test data
        UploadRow(Runtime(), "/MyRoot/Table", 0, {1}, {2}, {TCell::Make(1u)}, {TCell::Make(1u)});

        // Block sending backup task to datashards
        TBlockEvents<TEvDataShard::TEvProposeTransaction> block(Runtime(), [](auto& ev) {
            NKikimrTxDataShard::TFlatSchemeTransaction schemeTx;
            UNIT_ASSERT(schemeTx.ParseFromString(ev.Get()->Get()->GetTxBody()));
            return schemeTx.HasBackup();
        });

        // Start export and expect it to be blocked
        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));

        Runtime().WaitFor("backup task is sent to datashards", [&]{ return block.size() >= 1; });

        // Stop blocking new events
        block.Stop();

        // Reboot SchemeShard to resend backup task
        RebootTablet(Runtime(), TTestTxConfig::SchemeShard, Runtime().AllocateEdgeActor());

        // Wait for export to complete
        Env().TestWaitNotification(Runtime(), txId);

        // Verify checksums are created
        UNIT_ASSERT_VALUES_EQUAL(FileCount<IsFs>(), 8);
        auto allFiles = GetAllFiles<IsFs>();
        const TString p = FilePrefix<IsFs>();

        const auto* dataChecksum = allFiles.FindPtr(p + "/data_00.csv.sha256");
        UNIT_ASSERT(dataChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*dataChecksum, "19dcd641390a61063ee45f3e6e06b8f0d3acfc33f934b9bf1ba204668a98f21d data_00.csv");

        const auto* metadataChecksum = allFiles.FindPtr(p + "/metadata.json.sha256");
        UNIT_ASSERT(metadataChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*metadataChecksum, "a5a7ca9bce00ac9d7e5b48a30a46f139592845cad0664b3fda92af32583b7d52 metadata.json");

        const auto* schemeChecksum = allFiles.FindPtr(p + "/scheme.pb.sha256");
        UNIT_ASSERT(schemeChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*schemeChecksum, "cb1fb80965ae92e6369acda2b3b5921fd5518c97d6437f467ce00492907f9eb6 scheme.pb");

        const auto* permissionsChecksum = allFiles.FindPtr(p + "/permissions.pb.sha256");
        UNIT_ASSERT(permissionsChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*permissionsChecksum, "b41fd8921ff3a7314d9c702dc0e71aace6af8443e0102add0432895c5e50a326 permissions.pb");
    }

    Y_UNIT_TEST_TWIN(ChecksumsWithCompression, IsFs) {
        EnvOptions().EnableChecksumsExport(true);
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        Env().TestWaitNotification(Runtime(), txId);

        UploadRow(Runtime(), "/MyRoot/Table", 0, {1}, {2}, {TCell::Make(1u)}, {TCell::Make(1u)});

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}, R"(compression: "zstd")"));
        Env().TestWaitNotification(Runtime(), txId);

        auto allFiles = GetAllFiles<IsFs>(); const auto* dataChecksum = allFiles.FindPtr(FilePrefix<IsFs>() + "/data_00.csv.sha256");
        UNIT_ASSERT(dataChecksum);
        UNIT_ASSERT_VALUES_EQUAL(*dataChecksum, "19dcd641390a61063ee45f3e6e06b8f0d3acfc33f934b9bf1ba204668a98f21d data_00.csv");
    }

    class ChangefeedGenerator {
    public:
        ChangefeedGenerator(const ui64 count, const TS3Mock& s3Mock)
            : Count(count)
            , S3Mock(s3Mock)
            , Changefeeds(GenChangefeeds())
        {}

        const TVector<TString>& GetChangefeeds() const {
            return Changefeeds;
        }

        void Check() {
            for (ui64 i = 1; i <= Count; ++i) {
                auto changefeedDir = "/" + GenChangefeedName(i);
                auto* changefeed = S3Mock.GetData().FindPtr(changefeedDir + "/changefeed_description.pb");
                UNIT_ASSERT_VALUES_EQUAL(*changefeed, Sprintf(R"(name: "update_feed%d"
mode: MODE_UPDATES
format: FORMAT_JSON
state: STATE_ENABLED
)", i));

                auto* topic = S3Mock.GetData().FindPtr(changefeedDir + "/topic_description.pb");
                UNIT_ASSERT(topic);

                Ydb::Topic::DescribeTopicResult actualTopicProto;
                UNIT_ASSERT_C(
                    google::protobuf::TextFormat::ParseFromString(*topic, &actualTopicProto),
                    *topic
                );

                Ydb::Topic::DescribeTopicResult expectedTopicProto;
                TString expectedTopicStr = R"(
                    partitioning_settings {
                        min_active_partitions: 1
                        max_active_partitions: 1
                        auto_partitioning_settings {
                            strategy: AUTO_PARTITIONING_STRATEGY_DISABLED
                            partition_write_speed {
                                stabilization_window {
                                    seconds: 300
                                }
                                up_utilization_percent: 80
                                down_utilization_percent: 20
                            }
                        }
                    }
                    partitions {
                        active: true
                    }
                    retention_period {
                        seconds: 86400
                    }
                    partition_write_speed_bytes_per_second: 1048576
                    partition_write_burst_bytes: 1048576
                )";
                UNIT_ASSERT_C(
                    google::protobuf::TextFormat::ParseFromString(expectedTopicStr, &expectedTopicProto),
                    expectedTopicStr
                );

                actualTopicProto.clear_attributes();
                UNIT_ASSERT_STRINGS_EQUAL(
                    actualTopicProto.partitioning_settings().DebugString(),
                    expectedTopicProto.partitioning_settings().DebugString()
                );

                const auto* changefeedChecksum = S3Mock.GetData().FindPtr(changefeedDir + "/changefeed_description.pb.sha256");
                UNIT_ASSERT(changefeedChecksum);

                const auto* topicChecksum = S3Mock.GetData().FindPtr(changefeedDir + "/topic_description.pb.sha256");
                UNIT_ASSERT(topicChecksum);
            }
        }

    private:
        static TString GenChangefeedName(const ui64 num) {
            return TStringBuilder() << "update_feed" << num;
        }

        TVector<TString> GenChangefeeds() {
            TVector<TString> result(Count);
            std::generate(result.begin(), result.end(), [n = 1]() mutable {
                    return Sprintf(
                        R"(
                            TableName: "Table"
                            StreamDescription {
                                Name: "%s"
                                Mode: ECdcStreamModeUpdate
                                Format: ECdcStreamFormatJson
                                State: ECdcStreamStateReady
                            }
                        )", GenChangefeedName(n++).data()
                    );
                }
            );
            return result;
        }

        const ui64 Count;
        const TS3Mock& S3Mock;
        const TVector<TString> Changefeeds;
    };

    Y_UNIT_TEST(Changefeeds) {
        ChangefeedGenerator gen(3, S3Mock());

        auto request = Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: ""
              }
            }
        )", S3Port());

        EnvOptions().EnableChecksumsExport(true);
        Env(); // Init test env
        Runtime().GetAppData().FeatureFlags.SetEnableChangefeedsExport(true);

        Run(Runtime(), Env(), TVector<TString>{
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, request, Ydb::StatusIds::SUCCESS, "/MyRoot", false, "", "", gen.GetChangefeeds());

        gen.Check();
    }

    Y_UNIT_TEST(SchemaMapping) {
        RunS3({
            R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
            R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              destination_prefix: "my_export"
              items {
                source_path: "/MyRoot/Table1"
              }
              items {
                source_path: "/MyRoot/Table2"
                destination_prefix: "table2_prefix"
              }
            }
        )");

        UNIT_ASSERT(HasS3File("/my_export/metadata.json"));
        UNIT_ASSERT(HasS3File("/my_export/SchemaMapping/metadata.json"));
        UNIT_ASSERT(HasS3File("/my_export/SchemaMapping/mapping.json"));
        UNIT_ASSERT(HasS3File("/my_export/Table1/scheme.pb"));
        UNIT_ASSERT(HasS3File("/my_export/table2_prefix/scheme.pb"));
        UNIT_ASSERT_STRINGS_EQUAL(GetS3FileContent("/my_export/metadata.json"), "{\"kind\":\"SimpleExportV0\",\"checksum\":\"sha256\"}");
    }

    Y_UNIT_TEST(SchemaMappingEncryption) {
        RunS3({
            R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
            R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              destination_prefix: "my_export"
              items {
                source_path: "/MyRoot/Table1"
              }
              items {
                source_path: "/MyRoot/Table2"
                destination_prefix: "table2_prefix"
              }
              encryption_settings {
                encryption_algorithm: "AES-128-GCM"
                symmetric_key {
                    key: "0123456789012345"
                }
              }
            }
        )");

        UNIT_ASSERT(HasS3File("/my_export/metadata.json"));
        UNIT_ASSERT(HasS3File("/my_export/SchemaMapping/metadata.json.enc"));
        UNIT_ASSERT(HasS3File("/my_export/SchemaMapping/mapping.json.enc"));
        UNIT_ASSERT(HasS3File("/my_export/001/scheme.pb.enc"));
        UNIT_ASSERT(HasS3File("/my_export/table2_prefix/scheme.pb.enc"));
    }

    Y_UNIT_TEST(SchemaMappingEncryptionIncorrectKey) {
        RunS3({
            R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
            R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              destination_prefix: "my_export"
              items {
                source_path: "/MyRoot/Table1"
              }
              items {
                source_path: "/MyRoot/Table2"
                destination_prefix: "table2_prefix"
              }
              encryption_settings {
                encryption_algorithm: "AES-128-GCM"
                symmetric_key {
                    key: "123"
                }
              }
            }
        )", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(EncryptedExport) {
        RunS3({
            R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                UniformPartitionsCount: 2
            )",
            R"(
                Name: "Table2"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                UniformPartitionsCount: 2
            )",
        }, R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              destination_prefix: "my_export"
              items {
                source_path: "/MyRoot/Table1"
              }
              items {
                source_path: "/MyRoot/Table2"
              }
              encryption_settings {
                encryption_algorithm: "AES-128-GCM"
                symmetric_key {
                    key: "0123456789012345"
                }
              }
            }
        )");

        CheckHasAllS3Files({
            "/my_export/metadata.json",
            "/my_export/SchemaMapping/metadata.json.enc",
            "/my_export/SchemaMapping/mapping.json.enc",
            "/my_export/001/scheme.pb.enc",
            "/my_export/001/data_00.csv.enc",
            "/my_export/001/data_01.csv.enc",
            "/my_export/002/scheme.pb.enc",
            "/my_export/002/data_00.csv.enc",
            "/my_export/002/data_01.csv.enc",
        });

        THashSet<TString> ivs;
        for (auto [key, content] : S3Mock().GetData()) {
            if (key == "/my_export/metadata.json" || key.EndsWith(".sha256")) {
                continue;
            }

            // All files except backup metadata and checksums must be encrypted
            UNIT_ASSERT_C(key.EndsWith(".enc"), key);

            // Check that we can decrypt content with our key (== it is really encrypted with our key)
            TBuffer decryptedData;
            NBackup::TEncryptionIV iv;
            UNIT_ASSERT_NO_EXCEPTION_C(std::tie(decryptedData, iv) = NBackup::TEncryptedFileDeserializer::DecryptFullFile(
                NBackup::TEncryptionKey("0123456789012345"),
                TBuffer(content.data(), content.size())
            ), key);

            // All ivs are unique
            UNIT_ASSERT_C(ivs.insert(iv.GetBinaryString()).second, key);
        }
    }

    Y_UNIT_TEST_TWIN(AutoDropping, IsFs) {
        ConfigureRuntime(IsFs);
        Runtime().GetAppData().FeatureFlags.SetEnableExportAutoDropping(true);
        auto request = MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});

        Run(Runtime(), Env(), TVector<TString>{
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, request, Ydb::StatusIds::SUCCESS, "/MyRoot", false, "", "", {}, true);
    }

    Y_UNIT_TEST_TWIN(DisableAutoDropping, IsFs) {
        ConfigureRuntime(IsFs);
        Runtime().GetAppData().FeatureFlags.SetEnableExportAutoDropping(false);
        auto request = MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}});

        Run(Runtime(), Env(), TVector<TString>{
            R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )",
        }, request, Ydb::StatusIds::SUCCESS, "/MyRoot", false, "", "", {}, true);
    }

    Y_UNIT_TEST_TWIN(TopicExport, IsFs) {
      TestTopicImpl<IsFs>();
    }

    Y_UNIT_TEST_TWIN(TopicWithPermissionsExport, IsFs) {
      TestTopicImpl<IsFs>(true);
    }

    Y_UNIT_TEST_TWIN(TopicsExport, IsFs) {
      TestTopicImpl<IsFs>(false, 5, 4);
    }

    Y_UNIT_TEST_TWIN(TopicsWithPermissionsExport, IsFs) {
      TestTopicImpl<IsFs>(true, 5, 4);
    }

    Y_UNIT_TEST_TWIN(SystemViewWithPermissionsExport, IsFs) {
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        Runtime().GetAppData().FeatureFlags.SetEnableSysViewPermissionsExport(true);

        TestLs(Runtime(), "/MyRoot/.sys/partition_stats", false, NLs::PathExist);

        NACLib::TDiffACL diffACL;
        diffACL.AddAccess(NACLib::EAccessType::Allow, NACLib::GenericUse, "user0@builtin");
        TestModifyACL(Runtime(), ++txId, "/MyRoot/.sys", "partition_stats", diffACL.SerializeAsString(), "user0@builtin");
        Env().TestWaitNotification(Runtime(), txId);

        auto request = MakeExportRequest(IsFs, {{"/MyRoot/.sys/partition_stats", "/partition_stats"}});

        TestExport(Runtime(), ++txId, "/MyRoot", request);
        Env().TestWaitNotification(Runtime(), txId);

        TestGetExport(Runtime(), txId, "/MyRoot");

        UNIT_ASSERT(HasFile<IsFs>("/partition_stats/system_view.pb"));
        UNIT_ASSERT(HasFile<IsFs>("/partition_stats/permissions.pb"));
        UNIT_ASSERT(HasFile<IsFs>("/partition_stats/metadata.json"));

        const auto sysviewDesc = GetFileContent<IsFs>("/partition_stats/system_view.pb");
        const auto sysviewDescExpected = "sys_view_id: 1\nsys_view_name: \"partition_stats\"\n";
        UNIT_ASSERT_EQUAL_C(
            sysviewDesc, sysviewDescExpected,
            TStringBuilder() << "\nExpected:\n\n" << sysviewDescExpected << "\n\nActual:\n\n" << sysviewDesc);

        const auto permissions = GetFileContent<IsFs>("/partition_stats/permissions.pb");
        CheckPermissions(permissions, CreateProtoComparator(R"(
            actions {
              change_owner: "user0@builtin"
            }
            actions {
              grant {
                subject: "user0@builtin"
                permission_names: "ydb.generic.use"
              }
            }
        )"));
    }

    Y_UNIT_TEST_TWIN(ExportTableWithUniqueIndex, IsFs) {
      ConfigureRuntime(IsFs);
      ui64 txId = 100;

      TestCreateIndexedTable(Runtime(), ++txId, "/MyRoot", R"(
          TableDescription {
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
          }
          IndexDescription {
            Name: "ByValue"
            KeyColumnNames: ["value"]
            Type: EIndexTypeGlobalUnique
          }
      )");
      Env().TestWaitNotification(Runtime(), txId);

      TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table", ""}}));
      Env().TestWaitNotification(Runtime(), txId);

      TestDescribeResult(DescribePrivatePath(Runtime(), "/MyRoot/Table/ByValue"),
            {NLs::PathExist,
             NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobalUnique),
             NLs::IndexState(NKikimrSchemeOp::EIndexStateReady),
             NLs::IndexKeys({"value"})});
    }

    Y_UNIT_TEST(DecimalOutOfRange) {
        EnvOptions().DisableStatsBatching(true);
        Env(); // Init test env
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "value" Type: "Decimal" }
                KeyColumnNames: ["key"]
            )");
        Env().TestWaitNotification(Runtime(), txId);

        // Write a normal decimal value
        // 10.0^13-1 (scale 9) = 0x21e19e0c9ba76a53600
        {
            ui64 key = 1u;
            std::pair<ui64, i64> value = { 0x19e0c9ba76a53600ULL, 0x21eULL };
            UploadRow(Runtime(), "/MyRoot/Table1", 0, {1}, {2}, {TCell::Make(key)}, {TCell::Make(value)});
        }
        // Write a decimal value that is out of range for precision 22
        // 10.0^13 (scale 9) = 10^22 = 0x21e19e0c9bab2400000
        {
            ui64 key = 2u;
            std::pair<ui64, i64> value = { 0x19e0c9bab2400000ULL, 0x21eULL };
            UploadRow(Runtime(), "/MyRoot/Table1", 0, {1}, {2}, {TCell::Make(key)}, {TCell::Make(value)});
        }

        TestExport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table1"
                destination_prefix: "Backup1"
              }
            }
        )", S3Port()));
        Env().TestWaitNotification(Runtime(), txId);

        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

        UNIT_ASSERT(HasS3File("/Backup1/metadata.json"));
        UNIT_ASSERT(HasS3File("/Backup1/data_00.csv"));
        UNIT_ASSERT_STRINGS_EQUAL(GetS3FileContent("/Backup1/data_00.csv"),
            "1,9999999999999\n"
            "2,10000000000000\n");

        TestImport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "Backup1"
                destination_path: "/MyRoot/Table2"
              }
            }
        )", S3Port()));
        Env().TestWaitNotification(Runtime(), txId);

        TestGetImport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

        TestExport(Runtime(), ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table2"
                destination_prefix: "Backup2"
              }
            }
        )", S3Port()));
        Env().TestWaitNotification(Runtime(), txId);

        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

        // Note: out-of-range values are restored as inf
        UNIT_ASSERT(HasS3File("/Backup2/metadata.json"));
        UNIT_ASSERT(HasS3File("/Backup2/data_00.csv"));
        UNIT_ASSERT_STRINGS_EQUAL(GetS3FileContent("/Backup2/data_00.csv"),
            "1,9999999999999\n"
            "2,inf\n");
    }

    Y_UNIT_TEST_TWIN(CorruptedDecimalValue, IsFs) {
        EnvOptions().DisableStatsBatching(true);
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
                Name: "Table1"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "value" Type: "Decimal" }
                KeyColumnNames: ["key"]
            )");
        Env().TestWaitNotification(Runtime(), txId);

        {
            ui64 key = 1u;
            std::pair<ui64, i64> value = { 0x098a224000000000ULL, 0x4b3b4ca85a86c47aULL };
            UploadRow(Runtime(), "/MyRoot/Table1", 0, {1}, {2}, {TCell::Make(key)}, {TCell::Make(value)});
        }

        TestExport(Runtime(), ++txId, "/MyRoot", MakeExportRequest(IsFs, {{"/MyRoot/Table1", "Backup1"}}));
        Env().TestWaitNotification(Runtime(), txId);

        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::CANCELLED);
    }

    void IndexMaterialization(TTestEnv& env, TTestBasicRuntime& runtime, TS3Mock& s3Mock, ui16 s3Port, bool enabled, const TString& indexDesc) {
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
              Name: "Table"
              Columns { Name: "key" Type: "Uint32" }
              Columns { Name: "embedding" Type: "String" }
              Columns { Name: "prefix" Type: "String" }
              Columns { Name: "value" Type: "Utf8" }
              KeyColumnNames: ["key"]
            }
            %s
        )", indexDesc.c_str()));
        env.TestWaitNotification(runtime, txId);

        const auto expectedStatus = enabled ? Ydb::StatusIds::SUCCESS : Ydb::StatusIds::PRECONDITION_FAILED;
        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              include_index_data: true
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: ""
              }
            }
        )", s3Port), "", "", expectedStatus);

        if (!enabled) {
            return;
        }

        env.TestWaitNotification(runtime, txId);

        auto desc = DescribePrivatePath(runtime, "/MyRoot/Table/index");
        const auto& tableIndex = desc.GetPathDescription().GetTableIndex();
        const auto indexType = tableIndex.GetType();
        const TVector<TString> indexColumns(tableIndex.GetKeyColumnNames().begin(), tableIndex.GetKeyColumnNames().end());

        for (const auto implTable : NTableIndex::GetImplTables(indexType, indexColumns)) {
            UNIT_ASSERT(s3Mock.GetData().FindPtr(TStringBuilder() << "/index/" << implTable << "/scheme.pb"));
        }
    }

    Y_UNIT_TEST(IndexMaterializationDisabled) {
        EnvOptions().EnableIndexMaterialization(false);
        IndexMaterialization(Env(), Runtime(), S3Mock(), S3Port(), false, R"(
            IndexDescription {
              Name: "index"
              KeyColumnNames: ["value"]
            }
        )");
    }

    Y_UNIT_TEST(IndexMaterialization) {
        EnvOptions().EnableIndexMaterialization(true);
        IndexMaterialization(Env(), Runtime(), S3Mock(), S3Port(), true, R"(
            IndexDescription {
              Name: "index"
              KeyColumnNames: ["value"]
            }
        )");
    }

    Y_UNIT_TEST(IndexMaterializationGlobal) {
        EnvOptions().EnableIndexMaterialization(true);
        IndexMaterialization(Env(), Runtime(), S3Mock(), S3Port(), true, R"(
            IndexDescription {
              Name: "index"
              KeyColumnNames: ["value"]
              Type: EIndexTypeGlobal
            }
        )");
    }

    Y_UNIT_TEST(IndexMaterializationGlobalAsync) {
        EnvOptions().EnableIndexMaterialization(true);
        IndexMaterialization(Env(), Runtime(), S3Mock(), S3Port(), true, R"(
            IndexDescription {
              Name: "index"
              KeyColumnNames: ["value"]
              Type: EIndexTypeGlobalAsync
            }
        )");
    }

    Y_UNIT_TEST(IndexMaterializationGlobalVectorKmeansTree) {
        EnvOptions().EnableIndexMaterialization(true);
        IndexMaterialization(Env(), Runtime(), S3Mock(), S3Port(), true, R"(
            IndexDescription {
              Name: "index"
              KeyColumnNames: ["embedding"]
              Type: EIndexTypeGlobalVectorKmeansTree
              VectorIndexKmeansTreeDescription {
                Settings {
                  settings {
                    metric: DISTANCE_COSINE
                    vector_type: VECTOR_TYPE_FLOAT
                    vector_dimension: 1024
                  }
                  clusters: 4
                  levels: 5
                }
              }
            }
        )");
    }

    Y_UNIT_TEST(IndexMaterializationGlobalVectorKmeansTreePrefix) {
        EnvOptions().EnableIndexMaterialization(true);
        IndexMaterialization(Env(), Runtime(), S3Mock(), S3Port(), true, R"(
            IndexDescription {
              Name: "index"
              KeyColumnNames: ["prefix", "embedding"]
              Type: EIndexTypeGlobalVectorKmeansTree
              VectorIndexKmeansTreeDescription {
                Settings {
                  settings {
                    metric: DISTANCE_COSINE
                    vector_type: VECTOR_TYPE_FLOAT
                    vector_dimension: 1024
                  }
                  clusters: 4
                  levels: 5
                }
              }
            }
        )");
    }

    Y_UNIT_TEST(IndexMaterializationTwoTables) {
        EnvOptions().EnableIndexMaterialization(true);
        auto& env = Env();
        auto& runtime = Runtime();
        ui64 txId = 100;

        for (const auto tableName : {"Table1", "Table2"}) {
            TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
                TableDescription {
                  Name: "%s"
                  Columns { Name: "key" Type: "Uint32" }
                  Columns { Name: "value" Type: "Utf8" }
                  KeyColumnNames: ["key"]
                }
                IndexDescription {
                  Name: "index"
                  KeyColumnNames: ["value"]
                }
            )", tableName));
            env.TestWaitNotification(runtime, txId);
        }

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              include_index_data: true
              items {
                source_path: "/MyRoot/Table1"
                destination_prefix: "table1"
              }
              items {
                source_path: "/MyRoot/Table2"
                destination_prefix: "table2"
              }
            }
        )", S3Port()));

        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST_TWIN(ReplicationExportWithStaticCredentials, IsFs) {
        TString scheme = R"(
            Name: "Replication"
            Config {
                SrcConnectionParams {
                    Endpoint: "localhost:2135"
                    Database: "/MyRoot"
                    StaticCredentials {
                        User: "user"
                        Password: "pwd"
                        PasswordSecretName: "pwd-secret-name"
                    }
                }
                Specific {
                    Targets {
                        SrcPath: "/MyRoot/Table1"
                        DstPath: "/MyRoot/Table1Replica"
                    }
                }
            }
        )";
        // As passwords are not backuped
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
CREATE ASYNC REPLICATION `Replication`
FOR
  `/MyRoot/Table1` AS `/MyRoot/Table1Replica`
WITH (
  CONNECTION_STRING = 'grpc://localhost:2135/?database=/MyRoot',
  USER = 'user',
  PASSWORD_SECRET_NAME = 'pwd-secret-name',
  CONSISTENCY_LEVEL = 'Row'
);)";
        TestReplicationImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(ReplicationExportWithOAuthCredentials, IsFs) {
        TString scheme = R"(
            Name: "Replication"
            Config {
                SrcConnectionParams {
                    Endpoint: "localhost:2135"
                    Database: "/MyRoot"
                    OAuthToken {
                        Token: "super-secret-token"
                        TokenSecretName: "token-secret-name"
                    }
                }
                Specific {
                    Targets {
                        SrcPath: "/MyRoot/Table1"
                        DstPath: "/MyRoot/Table1Replica"
                    }
                }
            }
        )";
        // As OAuth tokens are not backuped
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
CREATE ASYNC REPLICATION `Replication`
FOR
  `/MyRoot/Table1` AS `/MyRoot/Table1Replica`
WITH (
  CONNECTION_STRING = 'grpc://localhost:2135/?database=/MyRoot',
  TOKEN_SECRET_NAME = 'token-secret-name',
  CONSISTENCY_LEVEL = 'Row'
);)";
        TestReplicationImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(ReplicationExportMultipleItems, IsFs) {
        TString scheme = R"(
            Name: "Replication"
            Config {
                SrcConnectionParams {
                    Endpoint: "localhost:2135"
                    Database: "/MyRoot"
                }
                Specific {
                    Targets {
                        SrcPath: "/MyRoot/Table1"
                        DstPath: "/MyRoot/Table1Replica"
                    }
                    Targets {
                        SrcPath: "/MyRoot/Table2"
                        DstPath: "/MyRoot/Table2Replica"
                    }
                    Targets {
                        SrcPath: "/MyRoot/Table3"
                        DstPath: "/MyRoot/Table3Replica"
                    }
                }
            }
        )";
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
CREATE ASYNC REPLICATION `Replication`
FOR
  `/MyRoot/Table1` AS `/MyRoot/Table1Replica`,
  `/MyRoot/Table2` AS `/MyRoot/Table2Replica`,
  `/MyRoot/Table3` AS `/MyRoot/Table3Replica`
WITH (
  CONNECTION_STRING = 'grpc://localhost:2135/?database=/MyRoot',
  CONSISTENCY_LEVEL = 'Row'
);)";
        TestReplicationImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(ReplicationExportGlobalConsistency, IsFs) {
        TString scheme = R"(
            Name: "Replication"
            Config {
                SrcConnectionParams {
                    Endpoint: "localhost:2135"
                    Database: "/MyRoot"
                }
                ConsistencySettings {
                    Global {
                        CommitIntervalMilliSeconds: 17000
                    }
                }
                Specific {
                    Targets {
                        SrcPath: "/MyRoot/Table1"
                        DstPath: "/MyRoot/Table1Replica"
                    }
                }
            }
        )";
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
CREATE ASYNC REPLICATION `Replication`
FOR
  `/MyRoot/Table1` AS `/MyRoot/Table1Replica`
WITH (
  CONNECTION_STRING = 'grpc://localhost:2135/?database=/MyRoot',
  CONSISTENCY_LEVEL = 'Global',
  COMMIT_INTERVAL = Interval('PT17S')
);)";
        TestReplicationImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(ReplicatedTableExport, IsFs) {
        ConfigureRuntime(IsFs);
        ui64 txId = 100;

        TestCreateTable(Runtime(), ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            KeyColumnNames: ["key"]
            ReplicationConfig {
                Mode: REPLICATION_MODE_READ_ONLY
            }
        )");
        Env().TestWaitNotification(Runtime(), txId);

        TestDescribeResult(DescribePath(Runtime(), "/MyRoot/Table"), {
            NLs::ReplicationMode(NKikimrSchemeOp::TTableReplicationConfig::REPLICATION_MODE_READ_ONLY),
            NLs::UserAttrsEqual({{"__async_replica", "true"}}),
        });

        TString request = MakeExportRequest(IsFs, {{"/MyRoot/Table", "Table"}});

        TestExport(Runtime(), ++txId, "/MyRoot", request, "", "", Ydb::StatusIds::BAD_REQUEST);
        TestGetExport(Runtime(), txId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
    }

    Y_UNIT_TEST_TWIN(TransferExportNoConnString, IsFs) {
        auto lambda = "PRAGMA OrderedColumns;$transformation_lambda = ($msg) -> { return [ <| partition: $msg._partition, offset: $msg._offset, message: CAST($msg._data AS Utf8) |> ]; };$__ydb_transfer_lambda = $transformation_lambda;";

        TString scheme = Sprintf(R"(
            Name: "Transfer"
            Config {
                TransferSpecific {
                    Target {
                        SrcPath: "/MyRoot/Topic_0"
                        DstPath: "/MyRoot/Table"
                        TransformLambda: "%s"
                    }
                }
            }
        )", lambda);
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
$transformation_lambda = ($msg) -> { return [ <| partition: $msg._partition, offset: $msg._offset, message: CAST($msg._data AS Utf8) |> ]; };

CREATE TRANSFER `Transfer`
FROM `/MyRoot/Topic_0` TO `/MyRoot/Table` USING $transformation_lambda
WITH (
  CONNECTION_STRING = 'grpc:///?database=',
  BATCH_SIZE_BYTES = 8388608,
  FLUSH_INTERVAL = Interval('PT60S')
);)";
        TestTransferImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(TransferExportWithConnString, IsFs) {
        auto lambda = "PRAGMA OrderedColumns;$transformation_lambda = ($msg) -> { return [ <| partition: $msg._partition, offset: $msg._offset, message: CAST($msg._data AS Utf8) |> ]; };$__ydb_transfer_lambda = $transformation_lambda;";

        TString scheme = Sprintf(R"(
            Name: "Transfer"
            Config {
                SrcConnectionParams {
                    Endpoint: "localhost:2135"
                    Database: "/MyRoot"
                }
                TransferSpecific {
                    Target {
                        SrcPath: "/MyRoot/Topic_0"
                        DstPath: "/MyRoot/Table"
                        TransformLambda: "%s"
                    }
                }
            }
        )", lambda);
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
$transformation_lambda = ($msg) -> { return [ <| partition: $msg._partition, offset: $msg._offset, message: CAST($msg._data AS Utf8) |> ]; };

CREATE TRANSFER `Transfer`
FROM `/MyRoot/Topic_0` TO `/MyRoot/Table` USING $transformation_lambda
WITH (
  CONNECTION_STRING = 'grpc://localhost:2135/?database=/MyRoot',
  BATCH_SIZE_BYTES = 8388608,
  FLUSH_INTERVAL = Interval('PT60S')
);)";
        TestTransferImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(TransferExportWithConsumer, IsFs) {
        auto lambda = "PRAGMA OrderedColumns;$transformation_lambda = ($msg) -> { return [ <| partition: $msg._partition, offset: $msg._offset, message: CAST($msg._data AS Utf8) |> ]; };$__ydb_transfer_lambda = $transformation_lambda;";

        TString scheme = Sprintf(R"(
            Name: "Transfer"
            Config {
                TransferSpecific {
                    Target {
                        SrcPath: "/MyRoot/Topic_0"
                        DstPath: "/MyRoot/Table"
                        TransformLambda: "%s"
                        ConsumerName: "consumerName"
                    }
                }
            }
        )", lambda);
        TString expected = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
$transformation_lambda = ($msg) -> { return [ <| partition: $msg._partition, offset: $msg._offset, message: CAST($msg._data AS Utf8) |> ]; };

CREATE TRANSFER `Transfer`
FROM `/MyRoot/Topic_0` TO `/MyRoot/Table` USING $transformation_lambda
WITH (
  CONNECTION_STRING = 'grpc:///?database=',
  CONSUMER = 'consumerName',
  BATCH_SIZE_BYTES = 8388608,
  FLUSH_INTERVAL = Interval('PT60S')
);)";
        TestTransferImpl<IsFs>(scheme, expected);
    }

    Y_UNIT_TEST_TWIN(TopicExportWithAllFields, IsFs) {
        EnvOptions().EnablePermissionsExport(true).EnablePqBilling(true);
        ConfigureRuntime(IsFs);
        ui64 txId = 100;
        TString topicProto = R"(
            Name: "topic_full_test"
            TotalGroupCount: 3
            PartitionPerTablet: 3
            PQTabletConfig {
                RequireAuthRead: false
                RequireAuthWrite: false
                AbcId: 123
                AbcSlug: "abc_slug"
                FederationAccount: "federation_account"
                EnableCompactification: false
                TimestampType: "LogAppendTime"
                PartitionConfig {
                    LifetimeSeconds: 12
                    StorageLimitBytes: 104857600
                    WriteSpeedInBytesPerSecond: 1024
                    BurstSize: 2048
                    MaxSizeInPartition: 10
                    SourceIdLifetimeSeconds: 14
                    SourceIdMaxCounts: 10000000
                }
                Codecs {
                    Ids: 0
                    Ids: 1
                    Ids: 2
                }
                MeteringMode: METERING_MODE_RESERVED_CAPACITY
                PartitionStrategy {
                    MinPartitionCount: 3
                    MaxPartitionCount: 10
                    ScaleThresholdSeconds: 400
                    ScaleUpPartitionWriteSpeedThresholdPercent: 91
                    ScaleDownPartitionWriteSpeedThresholdPercent: 31
                    PartitionStrategyType: CAN_SPLIT
                }
                Consumers {
                    Name: "consumer_1"
                    Important: true
                    Codec {
                        Ids: 0
                        Ids: 1
                    }
                }
                Consumers {
                    Name: "consumer_2"
                    Important: false
                    Codec {
                        Ids: 1
                        Ids: 2
                    }
                }
            }
        )";

        TestCreatePQGroup(Runtime(), ++txId, "/MyRoot", topicProto);
        Env().TestWaitNotification(Runtime(), txId);

        auto schemeshardId = TTestTxConfig::SchemeShard;
        TString exportRequest = MakeExportRequest(IsFs, {{"/MyRoot/topic_full_test", "topic_export"}});

        TestExport(Runtime(), schemeshardId, ++txId, "/MyRoot", exportRequest, "", "", Ydb::StatusIds::SUCCESS);
        Env().TestWaitNotification(Runtime(), txId, schemeshardId);
        TestGetExport(Runtime(), schemeshardId, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

        auto topicPath = "/topic_export/create_topic.pb";
        UNIT_ASSERT_C(HasFile<IsFs>(topicPath), "Topic description file should exist");
        auto content = GetFileContent<IsFs>(topicPath);

        Ydb::Topic::CreateTopicRequest topicDescription;
        UNIT_ASSERT_C(
            google::protobuf::TextFormat::ParseFromString(content, &topicDescription),
            "Failed to parse topic description from S3"
        );

        const auto& partSettings = topicDescription.partitioning_settings();
        UNIT_ASSERT_VALUES_EQUAL(partSettings.min_active_partitions(), 3);
        UNIT_ASSERT_VALUES_EQUAL(partSettings.max_active_partitions(), 10);

        const auto& autoPartSettings = partSettings.auto_partitioning_settings();
        UNIT_ASSERT_VALUES_EQUAL(autoPartSettings.strategy(), Ydb::Topic::AutoPartitioningStrategy::AUTO_PARTITIONING_STRATEGY_SCALE_UP);

        const auto& writeSpeed = autoPartSettings.partition_write_speed();
        UNIT_ASSERT_VALUES_EQUAL(writeSpeed.stabilization_window().seconds(), 400);
        UNIT_ASSERT_VALUES_EQUAL(writeSpeed.up_utilization_percent(), 91);
        UNIT_ASSERT_VALUES_EQUAL(writeSpeed.down_utilization_percent(), 31);

        UNIT_ASSERT_VALUES_EQUAL(topicDescription.retention_period().seconds(), 12);

        UNIT_ASSERT_VALUES_EQUAL(topicDescription.retention_storage_mb(), 100);

        UNIT_ASSERT_VALUES_EQUAL(topicDescription.supported_codecs().codecs_size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(topicDescription.supported_codecs().codecs(0), 1); // CODEC_RAW
        UNIT_ASSERT_VALUES_EQUAL(topicDescription.supported_codecs().codecs(1), 2); // CODEC_GZIP
        UNIT_ASSERT_VALUES_EQUAL(topicDescription.supported_codecs().codecs(2), 3); // CODEC_LZOP

        UNIT_ASSERT_VALUES_EQUAL(topicDescription.partition_write_speed_bytes_per_second(), 1024);

        UNIT_ASSERT_VALUES_EQUAL(topicDescription.partition_write_burst_bytes(), 2048);

        UNIT_ASSERT_VALUES_EQUAL(
            static_cast<int>(topicDescription.metering_mode()),
            static_cast<int>(Ydb::Topic::METERING_MODE_RESERVED_CAPACITY)
        );

        UNIT_ASSERT_VALUES_EQUAL(topicDescription.consumers_size(), 2);

        const auto& consumer1 = topicDescription.consumers(0);
        UNIT_ASSERT_VALUES_EQUAL(consumer1.name(), "consumer_1");
        UNIT_ASSERT_VALUES_EQUAL(consumer1.important(), true);
        UNIT_ASSERT_VALUES_EQUAL(consumer1.supported_codecs().codecs_size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(consumer1.supported_codecs().codecs(0), 1);
        UNIT_ASSERT_VALUES_EQUAL(consumer1.supported_codecs().codecs(1), 2);

        const auto& consumer2 = topicDescription.consumers(1);
        UNIT_ASSERT_VALUES_EQUAL(consumer2.name(), "consumer_2");
        UNIT_ASSERT_VALUES_EQUAL(consumer2.important(), false);
        UNIT_ASSERT_VALUES_EQUAL(consumer2.supported_codecs().codecs_size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(consumer2.supported_codecs().codecs(0), 2);
        UNIT_ASSERT_VALUES_EQUAL(consumer2.supported_codecs().codecs(1), 3);

        const auto& attrs = topicDescription.attributes();
        UNIT_ASSERT(attrs.size() > 0);

        // RequireAuthRead: false -> _allow_unauthenticated_read: true
        UNIT_ASSERT(attrs.contains("_allow_unauthenticated_read"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_allow_unauthenticated_read"), "true");

        // RequireAuthWrite: false -> _allow_unauthenticated_write: true
        UNIT_ASSERT(attrs.contains("_allow_unauthenticated_write"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_allow_unauthenticated_write"), "true");

        UNIT_ASSERT(attrs.contains("_abc_id"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_abc_id"), "123");

        UNIT_ASSERT(attrs.contains("_abc_slug"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_abc_slug"), "abc_slug");

        UNIT_ASSERT(attrs.contains("_federation_account"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_federation_account"), "federation_account");

        UNIT_ASSERT(attrs.contains("_timestamp_type"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_timestamp_type"), "LogAppendTime");

        UNIT_ASSERT(attrs.contains("_partitions_per_tablet"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_partitions_per_tablet"), "3");

        UNIT_ASSERT(attrs.contains("_max_partition_storage_size"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_max_partition_storage_size"), "10");

        // SourceIdLifetimeSeconds: 14 -> message_group_seqno_retention_period_ms: 14000
        UNIT_ASSERT(attrs.contains("_message_group_seqno_retention_period_ms"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_message_group_seqno_retention_period_ms"), "14000");

        UNIT_ASSERT(attrs.contains("_max_partition_message_groups_seqno_stored"));
        UNIT_ASSERT_VALUES_EQUAL(attrs.at("_max_partition_message_groups_seqno_stored"), "10000000");

        auto permissionsPath = "/topic_export/permissions.pb";
        UNIT_ASSERT_C(HasFile<IsFs>(permissionsPath), "Permissions file should exist");
    }

    Y_UNIT_TEST_TWIN(ExternalDataSourceAuthNone, IsFs) {
        TString scheme = R"(
            Name: "DataSource"
            SourceType: "ObjectStorage"
            Location: "https://s3.cloud.net/bucket"
            Auth {
                None {}
            }
        )";

        TVector<TString> expectedProperties = {
            "SOURCE_TYPE = 'ObjectStorage'",
            "LOCATION = 'https://s3.cloud.net/bucket'",
            "AUTH_METHOD = 'NONE'",
        };

        TestExternalDataSourceImpl<IsFs>(scheme, expectedProperties);
    }

    Y_UNIT_TEST_TWIN(ExternalDataSourceAuthBasic, IsFs) {
        TString scheme = R"(
            Name: "DataSource"
            SourceType: "ClickHouse"
            Location: "https://clickhousedb.net"
            Auth {
                Basic {
                    Login: "my_login",
                    PasswordSecretName: "password_secret"
                }
            }
            Properties {
                Properties {
                    key: "database_name",
                    value: "clickhouse"
                }
                Properties {
                    key: "protocol",
                    value: "NATIVE"
                }
                Properties {
                    key: "use_tls",
                    value: "TRUE"
                }
            }
        )";

        TVector<TString> expectedProperties = {
            "SOURCE_TYPE = 'ClickHouse'",
            "LOCATION = 'https://clickhousedb.net'",
            "PASSWORD_SECRET_NAME = 'password_secret'",
            "AUTH_METHOD = 'BASIC'",
            "DATABASE_NAME = 'clickhouse'",
            "LOGIN = 'my_login'",
            "PROTOCOL = 'NATIVE'",
            "USE_TLS = 'TRUE'",
        };

        TestExternalDataSourceImpl<IsFs>(scheme, expectedProperties);
    }

    Y_UNIT_TEST_TWIN(ExternalDataSourceAuthAWS, IsFs) {
        TString scheme = R"(
            Name: "DataSource"
            SourceType: "ObjectStorage"
            Location: "https://s3.cloud.net/bucket"
            Auth {
                Aws {
                    AwsAccessKeyIdSecretName: "id_secret",
                    AwsSecretAccessKeySecretName: "access_secret"
                    AwsRegion: "ru-central-1"
                }
            }
        )";

        TVector<TString> expectedProperties = {
            "SOURCE_TYPE = 'ObjectStorage'",
            "LOCATION = 'https://s3.cloud.net/bucket'",
            "AUTH_METHOD = 'AWS'",
            "AWS_ACCESS_KEY_ID_SECRET_NAME = 'id_secret'",
            "AWS_SECRET_ACCESS_KEY_SECRET_NAME = 'access_secret'",
            "AWS_REGION = 'ru-central-1'",
        };

        TestExternalDataSourceImpl<IsFs>(scheme, expectedProperties);
    }

    Y_UNIT_TEST_TWIN(ExternalDataSourceAuthServiceAccount, IsFs) {
        TString scheme = R"(
            Name: "DataSource"
            SourceType: "ObjectStorage"
            Location: "https://s3.cloud.net/bucket"
            Auth {
                ServiceAccount {
                    Id: "id",
                    SecretName: "service_secret"
                }
            }
        )";

        TVector<TString> expectedProperties = {
            "SOURCE_TYPE = 'ObjectStorage'",
            "LOCATION = 'https://s3.cloud.net/bucket'",
            "AUTH_METHOD = 'SERVICE_ACCOUNT'",
            "SERVICE_ACCOUNT_ID = 'id'",
            "SERVICE_ACCOUNT_SECRET_NAME = 'service_secret'",
        };

        TestExternalDataSourceImpl<IsFs>(scheme, expectedProperties);
    }

    Y_UNIT_TEST_TWIN(ExternalDataSourceAuthMdbBasic, IsFs) {
        TString scheme = R"(
            Name: "DataSource"
            SourceType: "PostgreSQL"
            Location: "https://postgresdb.net"
            Auth {
                MdbBasic {
                    ServiceAccountId: "id",
                    ServiceAccountSecretName: "service_secret",
                    Login: "login",
                    PasswordSecretName: "pwd_secret"
                }
            }
            Properties {
                Properties {
                    key: "mdb_cluster_id",
                    value: "id"
                }
                Properties {
                    key: "database_name",
                    value: "postgres"
                }
            }
        )";

        TVector<TString> expectedProperties = {
            "SOURCE_TYPE = 'PostgreSQL'",
            "LOCATION = 'https://postgresdb.net'",
            "AUTH_METHOD = 'MDB_BASIC'",
            "SERVICE_ACCOUNT_ID = 'id'",
            "SERVICE_ACCOUNT_SECRET_NAME = 'service_secret'",
            "LOGIN = 'login'",
            "PASSWORD_SECRET_NAME = 'pwd_secret'",
            "DATABASE_NAME = 'postgres'",
            "MDB_CLUSTER_ID = 'id'",
        };

        TestExternalDataSourceImpl<IsFs>(scheme, expectedProperties);
    }

    Y_UNIT_TEST_TWIN(ExternalTable, IsFs) {
        TString scheme = R"(
            Name: "ExternalTable"
            SourceType: "General"
            DataSourcePath: "/MyRoot/DataSource"
            Location: "bucket"
            Columns { Name: "key" Type: "Uint64" NotNull: true }
            Columns { Name: "value1" Type: "Uint64" }
            Columns { Name: "value2" Type: "Utf8" NotNull: true }
        )";

        TString expectedStartsWith = R"(-- database: "/MyRoot"
-- backup root: "/MyRoot"
CREATE EXTERNAL TABLE IF NOT EXISTS `ExternalTable` (
      key Uint64 NOT NULL,
    value1 Uint64?,
    value2 Utf8 NOT NULL
) WITH ()";

        TVector<TString> expectedProperties = {
            "DATA_SOURCE = '/MyRoot/DataSource'",
            "LOCATION = 'bucket'"
        };

        TestExternalTableImpl<IsFs>(scheme, expectedStartsWith, expectedProperties);
    }

    Y_UNIT_TEST(DisableIcb) {
        TestIcb();
    }

}
