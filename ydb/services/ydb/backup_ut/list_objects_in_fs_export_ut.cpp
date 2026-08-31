#include "fs_backup_test_base.h"

#include <ydb/library/testlib/helpers.h>

using namespace NYdb;

class TListObjectsInFsExportTestFixture : public TFsBackupTestFixture {
    void SetUp(NUnitTest::TTestContext& /* context */) override {
        Server().GetRuntime()->GetAppData().FeatureFlags.SetEnableFsBackups(true);
        auto res = YdbQueryClient().ExecuteQuery(R"sql(
            CREATE TABLE `/Root/Table0` (
                key Uint32 NOT NULL,
                value String,
                PRIMARY KEY (key)
            );

            CREATE TABLE `/Root/dir1/Table1` (
                key Uint32 NOT NULL,
                value String,
                PRIMARY KEY (key)
            );

            CREATE TABLE `/Root/dir1/dir2/Table2` (
                key Uint32 NOT NULL,
                value String,
                PRIMARY KEY (key)
            );
        )sql", NQuery::TTxControl::NoTx()).GetValueSync();
        UNIT_ASSERT_C(res.IsSuccess(), res.GetIssues().ToString());
    }

    void TearDown(NUnitTest::TTestContext& /* context */) override {
    }
};

Y_UNIT_TEST_SUITE_F(ListObjectsInFsExport, TListObjectsInFsExportTestFixture) {
    Y_UNIT_TEST(ExportWithSchemaMapping) {
        {
            NExport::TExportToFsSettings exportSettings = MakeExportSettings("");
            auto res = YdbExportClient().ExportToFs(exportSettings).GetValueSync();
            WaitOpSuccess(res);
        }

        ValidateListObjectsInFsExport({
            {"Table0", "Table0"},
            {"dir1/Table1", "dir1/Table1"},
            {"dir1/dir2/Table2", "dir1/dir2/Table2"},
        });

        {
            auto settings = MakeListObjectsInFsExportSettings();
            settings
                .AppendItem({.Path = "dir1"})
                .AppendExcludeRegexp("Table1");

            ValidateListObjectsInFsExport({
                {"dir1/dir2/Table2", "dir1/dir2/Table2"},
            }, settings);
        }
    }

    Y_UNIT_TEST(ExportWithoutSchemaMapping) {
        Server().GetRuntime()->GetAppData().FeatureFlags.SetEnableExportFiltering(false);
        Server().GetRuntime()->GetAppData().FeatureFlags.SetEnableEncryptedExport(false);

        {
            NExport::TExportToFsSettings exportSettings = MakeExportSettings("");
            exportSettings
                .AppendItem({.Src = "/Root/Table0", .Dst = "t0"})
                .AppendItem({.Src = "/Root/dir1/Table1", .Dst = "d1/t1"})
                .AppendItem({.Src = "/Root/dir1/dir2/Table2", .Dst = "d1/d2/t2"});
            auto res = YdbExportClient().ExportToFs(exportSettings).GetValueSync();
            WaitOpSuccess(res);
        }

        ValidateListObjectsInFsExport({
            {"t0", "t0"},
            {"d1/t1", "d1/t1"},
            {"d1/d2/t2", "d1/d2/t2"},
        });
    }

    Y_UNIT_TEST(ExportWithEncryption) {
        {
            NExport::TExportToFsSettings exportSettings = MakeExportSettings("");
            exportSettings.SymmetricEncryption(
                NExport::TEncryptionAlgorithm::AES_128_GCM,
                "Cool random key!");
            auto res = YdbExportClient().ExportToFs(exportSettings).GetValueSync();
            WaitOpSuccess(res);
        }

        auto settings = MakeListObjectsInFsExportSettings();
        settings.SymmetricKey("Cool random key!");
        ValidateListObjectPathsInFsExport({
            "Table0",
            "dir1/Table1",
            "dir1/dir2/Table2",
        }, settings);

        settings.SymmetricKey("Wrong random key!");
        auto res = YdbImportClient().ListObjectsInFsExport(settings).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::BAD_REQUEST,
            "Status: " << res.GetStatus() << ". Issues: " << res.GetIssues().ToString());
    }

    Y_UNIT_TEST(Paging) {
        {
            NExport::TExportToFsSettings exportSettings = MakeExportSettings("");
            auto res = YdbExportClient().ExportToFs(exportSettings).GetValueSync();
            WaitOpSuccess(res);
        }

        auto settings = MakeListObjectsInFsExportSettings();
        auto firstPage = YdbImportClient().ListObjectsInFsExport(settings, 2).GetValueSync();
        UNIT_ASSERT_C(firstPage.IsSuccess(), firstPage.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(firstPage.GetItems().size(), 2);
        UNIT_ASSERT(!firstPage.NextPageToken().empty());

        auto secondPage = YdbImportClient().ListObjectsInFsExport(settings, 2, firstPage.NextPageToken()).GetValueSync();
        UNIT_ASSERT_C(secondPage.IsSuccess(), secondPage.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(secondPage.GetItems().size(), 1);
        UNIT_ASSERT(secondPage.NextPageToken().empty());
    }

    Y_UNIT_TEST(StoragePagingWithoutSchemaMapping) {
        constexpr size_t ObjectsCount = 1001;
        for (size_t i = 0; i < ObjectsCount; ++i) {
            TFsPath objectPath = TFsPath(GetTempDir().Path()) / ToString(i);
            objectPath.MkDirs();
            TFileOutput((objectPath / "metadata.json").GetPath()).Finish();
        }

        auto settings = MakeListObjectsInFsExportSettings();
        auto res = YdbImportClient().ListObjectsInFsExport(settings).GetValueSync();
        UNIT_ASSERT_C(res.IsSuccess(), res.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(res.GetItems().size(), ObjectsCount);
    }

    Y_UNIT_TEST(RelativeBasePathIsRejected) {
        NImport::TListObjectsInFsExportSettings settings;
        settings.BasePath("relative/path");

        auto res = YdbImportClient().ListObjectsInFsExport(settings).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::BAD_REQUEST,
            "Status: " << res.GetStatus() << ". Issues: " << res.GetIssues().ToString());
    }
}
