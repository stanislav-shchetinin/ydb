#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/ut_backup_restore_common.h>
#include <ydb/core/backup/common/metadata.h>

#include <ydb/public/api/protos/ydb_export.pb.h>
#include <ydb/public/api/protos/ydb_import.pb.h>
#include <ydb/public/api/protos/ydb_table.pb.h>
#include <ydb/public/lib/ydb_cli/dump/files/files.h>

#include <library/cpp/json/json_writer.h>
#include <util/folder/dirut.h>
#include <util/folder/path.h>
#include <util/folder/tempdir.h>
#include <util/stream/file.h>
#include <util/system/fs.h>

using namespace NSchemeShardUT_Private;

namespace {

class TTempBackupFiles {
public:
    explicit TTempBackupFiles()
    {
    }

    const TString& GetBasePath() const {
        return TempDir.Name();
    }

    void CreateTableBackup(const TString& tablePath, const TString& tableName) {
        CreateTableBackup(
            tablePath,
            tableName,
            {
                {"key", Ydb::Type::UTF8},
                {"value", Ydb::Type::UTF8}
            },
            {"key"}
        );
    }

    void CreateTableBackup(const TString& tablePath, const TString& tableName, 
                          const TVector<std::pair<TString, Ydb::Type::PrimitiveTypeId>>& columns,
                          const TVector<TString>& keyColumns) {
        const TString fullPath = TempDir.Name() + "/" + tablePath;
        MakePathIfNotExist(fullPath.c_str());

        // Create metadata.json
        CreateMetadataFile(fullPath);

        // Create scheme.pb
        CreateTableSchemeFile(fullPath, tableName, columns, keyColumns);

        // Create permissions.pb
        CreatePermissionsFile(fullPath);
    }

    void CreateTableBackupWithData(const TString& tablePath, const TString& tableName,
                                   const TVector<std::pair<TString, TString>>& rows) {
        CreateTableBackupWithData(
            tablePath,
            tableName,
            {
                {"key", Ydb::Type::UTF8},
                {"value", Ydb::Type::UTF8}
            },
            {"key"},
            rows
        );
    }

    void CreateTableBackupWithData(const TString& tablePath, const TString& tableName,
                                   const TVector<std::pair<TString, Ydb::Type::PrimitiveTypeId>>& columns,
                                   const TVector<TString>& keyColumns,
                                   const TVector<std::pair<TString, TString>>& rows) {
        const TString fullPath = TempDir.Name() + "/" + tablePath;
        MakePathIfNotExist(fullPath.c_str());

        // Create metadata.json
        CreateMetadataFile(fullPath);

        // Create scheme.pb
        CreateTableSchemeFile(fullPath, tableName, columns, keyColumns);

        // Create permissions.pb
        CreatePermissionsFile(fullPath);

        // Create data_00.csv with test data
        CreateDataFile(fullPath, rows);
    }

private:
    static void CreateMetadataFile(const TString& dirPath) {
        NBackup::TMetadata metadata;
        metadata.SetVersion(1);
        metadata.SetEnablePermissions(true);

        TString serialized = metadata.Serialize();

        TFileOutput file(dirPath + "/metadata.json");
        file.Write(serialized);
    }

    static void CreateTableSchemeFile(const TString& dirPath, const TString& tableName,
                                      const TVector<std::pair<TString, Ydb::Type::PrimitiveTypeId>>& columns,
                                      const TVector<TString>& keyColumns) {
        Ydb::Table::CreateTableRequest table;
        table.set_path(tableName);

        for (const auto& [colName, colType] : columns) {
            auto* col = table.add_columns();
            col->set_name(colName);
            col->mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(colType);
        }

        for (const auto& keyCol : keyColumns) {
            table.add_primary_key(keyCol);
        }

        TString serialized;
        Y_ABORT_UNLESS(table.SerializeToString(&serialized));

        TFileOutput file(dirPath + "/" + NYdb::NDump::NFiles::TableScheme().FileName);
        file.Write(serialized);
    }

    static void CreatePermissionsFile(const TString& dirPath) {
        Ydb::Scheme::ModifyPermissionsRequest permissions;

        TString serialized;
        Y_ABORT_UNLESS(permissions.SerializeToString(&serialized));

        TFileOutput file(dirPath + "/permissions.pb");
        file.Write(serialized);
    }

    static void CreateDataFile(const TString& dirPath, const TVector<std::pair<TString, TString>>& rows) {
        TStringBuilder csv;
        for (const auto& [key, value] : rows) {
            csv << "\"" << key << "\",\"" << value << "\"\n";
        }

        TFileOutput file(dirPath + "/data_00.csv");
        file.Write(csv);
    }

    TTempDir TempDir;
};

} // namespace

Y_UNIT_TEST_SUITE(TSchemeShardImportFromFsTests) {
    Y_UNIT_TEST(ShouldSucceedCreateImportFromFs) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;
        backup.CreateTableBackup("backup/Table", "Table");

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              items {
                source_path: "backup/Table"
                destination_path: "/MyRoot/RestoredTable"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        const auto& entry = response.GetResponse().GetEntry();
        
        UNIT_ASSERT(entry.HasImportFromFsSettings());
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);
        
        const auto& settings = entry.GetImportFromFsSettings();
        UNIT_ASSERT_VALUES_EQUAL(settings.base_path(), backup.GetBasePath());
        UNIT_ASSERT_VALUES_EQUAL(settings.items_size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(settings.items(0).source_path(), "backup/Table");
        UNIT_ASSERT_VALUES_EQUAL(settings.items(0).destination_path(), "/MyRoot/RestoredTable");

        TestDescribeResult(DescribePath(runtime, "/MyRoot/RestoredTable"), {
            NLs::PathExist,
            NLs::IsTable
        });
    }

    Y_UNIT_TEST(ShouldAcceptNoAclForFs) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;
        backup.CreateTableBackup("backup/Table", "Table");

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              no_acl: true
              items {
                source_path: "backup/Table"
                destination_path: "/MyRoot/RestoredTable"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        const auto& entry = response.GetResponse().GetEntry();
        
        UNIT_ASSERT(entry.HasImportFromFsSettings());
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);
        
        const auto& settings = entry.GetImportFromFsSettings();
        UNIT_ASSERT_VALUES_EQUAL(settings.no_acl(), true);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/RestoredTable"), {
            NLs::PathExist,
            NLs::IsTable
        });
    }

    Y_UNIT_TEST(ShouldAcceptSkipChecksumValidation) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;
        backup.CreateTableBackup("backup/Table", "Table");

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              skip_checksum_validation: true
              items {
                source_path: "backup/Table"
                destination_path: "/MyRoot/RestoredTable"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        const auto& entry = response.GetResponse().GetEntry();
        
        UNIT_ASSERT(entry.HasImportFromFsSettings());
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);
        
        const auto& settings = entry.GetImportFromFsSettings();
        UNIT_ASSERT_VALUES_EQUAL(settings.skip_checksum_validation(), true);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/RestoredTable"), {
            NLs::PathExist,
            NLs::IsTable
        });
    }

    Y_UNIT_TEST(ShouldFailOnInvalidDestinationPath) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Invalid destination path (empty) should fail validation
        TestImport(runtime, ++txId, "/MyRoot", R"(
            ImportFromFsSettings {
              base_path: "/mnt/backups"
              items {
                source_path: "backup/Table"
                destination_path: ""
              }
            }
        )", "", "", Ydb::StatusIds::BAD_REQUEST);
    }

    Y_UNIT_TEST(ShouldFailOnDuplicateDestination) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Duplicate destination paths should fail validation
        TestImport(runtime, ++txId, "/MyRoot", R"(
            ImportFromFsSettings {
              base_path: "/mnt/backups"
              items {
                source_path: "backup/Table1"
                destination_path: "/MyRoot/SameName"
              }
              items {
                source_path: "backup/Table2"
                destination_path: "/MyRoot/SameName"
              }
            }
        )", "", "", Ydb::StatusIds::BAD_REQUEST);
    }

    Y_UNIT_TEST(FsImportWithMultipleTables) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;
        backup.CreateTableBackup("backup/Table1", "Table1");
        backup.CreateTableBackup("backup/Table2", "Table2");

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              items {
                source_path: "backup/Table1"
                destination_path: "/MyRoot/RestoredTable1"
              }
              items {
                source_path: "backup/Table2"
                destination_path: "/MyRoot/RestoredTable2"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        const auto& entry = response.GetResponse().GetEntry();
        
        UNIT_ASSERT(entry.HasImportFromFsSettings());
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);
        
        const auto& settings = entry.GetImportFromFsSettings();
        UNIT_ASSERT_VALUES_EQUAL(settings.items_size(), 2);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/RestoredTable1"), {
            NLs::PathExist,
            NLs::IsTable
        });

        TestDescribeResult(DescribePath(runtime, "/MyRoot/RestoredTable2"), {
            NLs::PathExist,
            NLs::IsTable
        });
    }

    Y_UNIT_TEST(ShouldFailOnMissingBackupFiles) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              items {
                source_path: "backup/NonExistentTable"
                destination_path: "/MyRoot/RestoredTable"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        const auto& entry = response.GetResponse().GetEntry();
        
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLED);
        
        TestDescribeResult(DescribePath(runtime, "/MyRoot/RestoredTable"), {
            NLs::PathNotExist
        });
    }

    Y_UNIT_TEST(ShouldValidateTableSchema) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;
        backup.CreateTableBackup("backup/ComplexTable", "ComplexTable");

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              items {
                source_path: "backup/ComplexTable"
                destination_path: "/MyRoot/ComplexTable"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetEntry().GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);

        auto describe = DescribePath(runtime, "/MyRoot/ComplexTable");
        TestDescribeResult(describe, {
            NLs::PathExist,
            NLs::IsTable
        });

        const auto& table = describe.GetPathDescription().GetTable();
        UNIT_ASSERT_VALUES_EQUAL(table.ColumnsSize(), 2);
        UNIT_ASSERT_VALUES_EQUAL(table.GetColumns(0).GetName(), "key");
        UNIT_ASSERT_VALUES_EQUAL(table.GetColumns(1).GetName(), "value");
        UNIT_ASSERT_VALUES_EQUAL(table.KeyColumnNamesSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(table.GetKeyColumnNames(0), "key");
    }

    Y_UNIT_TEST(ShouldImportTableWithDifferentTypes) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TTempBackupFiles backup;
        backup.CreateTableBackup(
            "backup/TypedTable", 
            "TypedTable",
            {
                {"id", Ydb::Type::UINT64},
                {"name", Ydb::Type::UTF8},
                {"value", Ydb::Type::INT32},
                {"flag", Ydb::Type::BOOL}
            },
            {"id"}
        );

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              items {
                source_path: "backup/TypedTable"
                destination_path: "/MyRoot/TypedTable"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetEntry().GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);

        auto describe = DescribePath(runtime, "/MyRoot/TypedTable");
        TestDescribeResult(describe, {
            NLs::PathExist,
            NLs::IsTable
        });

        const auto& table = describe.GetPathDescription().GetTable();
        UNIT_ASSERT_VALUES_EQUAL(table.ColumnsSize(), 4);
        UNIT_ASSERT_VALUES_EQUAL(table.GetColumns(0).GetName(), "id");
        UNIT_ASSERT_VALUES_EQUAL(table.GetColumns(1).GetName(), "name");
        UNIT_ASSERT_VALUES_EQUAL(table.GetColumns(2).GetName(), "value");
        UNIT_ASSERT_VALUES_EQUAL(table.GetColumns(3).GetName(), "flag");
        UNIT_ASSERT_VALUES_EQUAL(table.KeyColumnNamesSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(table.GetKeyColumnNames(0), "id");
    }

    Y_UNIT_TEST(ShouldImportTableWithData) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);

        TTempBackupFiles backup;
        backup.CreateTableBackupWithData(
            "backup/TableWithData",
            "TableWithData",
            {
                {"key1", "value1"},
                {"key2", "value2"},
                {"key3", "value3"}
            }
        );

        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              skip_checksum_validation: true
              items {
                source_path: "backup/TableWithData"
                destination_path: "/MyRoot/TableWithData"
              }
            }
        )", backup.GetBasePath().c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetEntry().GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/TableWithData"), {
            NLs::PathExist,
            NLs::IsTable
        });

        // Verify data was imported correctly
        auto rowCount = CountRows(runtime, "/MyRoot/TableWithData");
        UNIT_ASSERT_VALUES_EQUAL(rowCount, 3);
    }

    Y_UNIT_TEST(ShouldExportAndThenImportWithData) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);

        // Create source table with data (using ui32 key type for WriteRow compatibility)
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "SourceTable"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Write some data to the source table
        WriteRow(runtime, ++txId, "/MyRoot/SourceTable", 0, 1, "valueA");
        WriteRow(runtime, ++txId, "/MyRoot/SourceTable", 0, 2, "valueB");
        WriteRow(runtime, ++txId, "/MyRoot/SourceTable", 0, 3, "valueC");

        // Create temp directory for export
        TTempDir exportDir;
        const TString basePath = exportDir.Name();

        // Export the table to FS
        TString exportSettings = Sprintf(R"(
            ExportToFsSettings {
              base_path: "%s"
              items {
                source_path: "/MyRoot/SourceTable"
                destination_path: "exported/SourceTable"
              }
            }
        )", basePath.c_str());

        TestExport(runtime, ++txId, "/MyRoot", exportSettings);
        env.TestWaitNotification(runtime, txId);

        TestGetExport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);

        // Verify export files were created
        TFsPath exportPath = TFsPath(basePath) / "exported/SourceTable";
        UNIT_ASSERT_C(exportPath.Exists(), "Export directory should exist");
        UNIT_ASSERT_C((exportPath / "scheme.pb").Exists(), "Scheme file should exist");
        UNIT_ASSERT_C((exportPath / "metadata.json").Exists(), "Metadata file should exist");
        UNIT_ASSERT_C((exportPath / "data_00.csv").Exists(), "Data file should exist");

        // Now import the exported data to a new table
        TString importSettings = Sprintf(R"(
            ImportFromFsSettings {
              base_path: "%s"
              skip_checksum_validation: true
              items {
                source_path: "exported/SourceTable"
                destination_path: "/MyRoot/ImportedTable"
              }
            }
        )", basePath.c_str());

        TestImport(runtime, ++txId, "/MyRoot", importSettings);
        env.TestWaitNotification(runtime, txId);

        auto response = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetEntry().GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/ImportedTable"), {
            NLs::PathExist,
            NLs::IsTable
        });

        // Verify imported data matches source data
        auto importedRows = CountRows(runtime, "/MyRoot/ImportedTable");
        auto sourceRows = CountRows(runtime, "/MyRoot/SourceTable");
        UNIT_ASSERT_VALUES_EQUAL(importedRows, sourceRows);
        UNIT_ASSERT_VALUES_EQUAL(importedRows, 3);
    }
}
