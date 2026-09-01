#include "service_import.h"
#include "fs_path_validation.h"

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::TX_PROXY

#include "rpc_list_objects_in_export.h"

#include <ydb/public/api/protos/ydb_import.pb.h>

#include <util/folder/path.h>

namespace NKikimr::NGRpcService {

struct TListObjectsInFsExportTraits {
    using TApiRequest = Ydb::Import::ListObjectsInFsExportRequest;
    using TApiResponse = Ydb::Import::ListObjectsInFsExportResponse;
    using TSchemeShardRequestEvent = NSchemeShard::TEvImport::TEvListObjectsInFsExportRequest;
    using TSchemeShardResponseEvent = NSchemeShard::TEvImport::TEvListObjectsInFsExportResponse;

    static TStringBuf LogPrefix() {
        return "[ListObjectsInFsExport]"sv;
    }

    static bool ValidateRequest(const TApiRequest& request, TString& error) {
        const auto& settings = request.settings();
        if (!TFsPath(settings.base_path()).IsAbsolute()) {
            error = "base_path must be an absolute path";
            return false;
        }
        return ValidateFsPath(settings.base_path(), "base_path", error);
    }

    template <typename TRecord>
    static void CopySettings(const TApiRequest& request, TRecord& record) {
        *record.MutableSettings() = request.settings();
        record.MutableSettings()->set_base_path(StripTrailingSlashes(record.GetSettings().base_path()));
    }
};

void DoListObjectsInFsExportRequest(std::unique_ptr<IRequestOpCtx> request, const IFacilityProvider& facility) {
    DoListObjectsInExportRequest<TListObjectsInFsExportTraits>(std::move(request), facility);
}

} // namespace NKikimr::NGRpcService
