#include "service_import.h"

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::TX_PROXY

#include "rpc_list_objects_in_export.h"

#include <ydb/public/api/protos/ydb_import.pb.h>

namespace NKikimr::NGRpcService {

struct TListObjectsInS3ExportTraits {
    using TApiRequest = Ydb::Import::ListObjectsInS3ExportRequest;
    using TApiResponse = Ydb::Import::ListObjectsInS3ExportResponse;
    using TSchemeShardRequestEvent = NSchemeShard::TEvImport::TEvListObjectsInS3ExportRequest;
    using TSchemeShardResponseEvent = NSchemeShard::TEvImport::TEvListObjectsInS3ExportResponse;

    static TStringBuf LogPrefix() {
        return "[ListObjectsInS3Export]"sv;
    }

    static bool ValidateRequest(const TApiRequest&, TString&) {
        return true;
    }

    template <typename TRecord>
    static void CopySettings(const TApiRequest& request, TRecord& record) {
        *record.MutableSettings() = request.settings();
    }
};

void DoListObjectsInS3ExportRequest(std::unique_ptr<IRequestOpCtx> request, const IFacilityProvider& facility) {
    DoListObjectsInExportRequest<TListObjectsInS3ExportTraits>(std::move(request), facility);
}

} // namespace NKikimr::NGRpcService
