#include "schemeshard_import_getters.h"
#include "schemeshard_private.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

namespace NKikimr {
namespace NSchemeShard {

namespace {

TString GetImportDisabledMessage(const TImportInfo& importInfo) {
    return TStringBuilder() << "Imports from " << importInfo.Kind << " are disabled";
}

} // anonymous namespace

class TSchemeGetterFallback: public TActorBootstrapped<TSchemeGetterFallback> {
public:
    explicit TSchemeGetterFallback(const TActorId& replyTo, TImportInfo::TPtr importInfo, ui32 itemIdx)
        : ReplyTo(replyTo)
        , ImportInfo(std::move(importInfo))
        , ItemIdx(itemIdx)
    {
    }

    void Bootstrap() {
        Send(ReplyTo, new TEvPrivate::TEvImportSchemeReady(ImportInfo->Id, ItemIdx, false, GetImportDisabledMessage(*ImportInfo)));
        PassAway();
    }

private:
    const TActorId ReplyTo;
    TImportInfo::TPtr ImportInfo;
    const ui32 ItemIdx;
}; // TSchemeGetterFallback

class TSchemaMappingGetterFallback: public TActorBootstrapped<TSchemaMappingGetterFallback> {
public:
    explicit TSchemaMappingGetterFallback(const TActorId& replyTo, TImportInfo::TPtr importInfo)
        : ReplyTo(replyTo)
        , ImportInfo(std::move(importInfo))
    {
    }

    void Bootstrap() {
        Send(ReplyTo, new TEvPrivate::TEvImportSchemaMappingReady(ImportInfo->Id, false, GetImportDisabledMessage(*ImportInfo)));
        PassAway();
    }

private:
    const TActorId ReplyTo;
    TImportInfo::TPtr ImportInfo;
}; // TSchemeGetterFallback

template <typename TRequestEvent, typename TResponseEvent>
class TListObjectsInExportGetterFallback
    : public TActorBootstrapped<TListObjectsInExportGetterFallback<TRequestEvent, TResponseEvent>>
{
public:
    using TThis = TListObjectsInExportGetterFallback<TRequestEvent, TResponseEvent>;

    TListObjectsInExportGetterFallback(typename TRequestEvent::TPtr&& ev, TStringBuf storageName)
        : Request(std::move(ev))
        , StorageName(storageName)
    {
    }

    void Bootstrap() {
        auto result = MakeHolder<TResponseEvent>();
        result->Record.set_status(Ydb::StatusIds::UNSUPPORTED);
        result->Record.add_issues()->set_message(TStringBuilder() << StorageName << " listings are disabled");
        this->Send(Request->Sender, std::move(result));
        this->PassAway();
    }

private:
    typename TRequestEvent::TPtr Request;
    const TString StorageName;
}; // TListObjectsInExportGetterFallback

IActor* CreateSchemeGetter(const TActorId& replyTo, TImportInfo::TPtr importInfo, ui32 itemIdx, TMaybe<NBackup::TEncryptionIV>) {
    return new TSchemeGetterFallback(replyTo, std::move(importInfo), itemIdx);
}

IActor* CreateSchemaMappingGetter(const TActorId& replyTo, TImportInfo::TPtr importInfo) {
    return new TSchemaMappingGetterFallback(replyTo, std::move(importInfo));
}

IActor* CreateListObjectsInS3ExportGetter(TEvImport::TEvListObjectsInS3ExportRequest::TPtr&& ev) {
    return new TListObjectsInExportGetterFallback<
        TEvImport::TEvListObjectsInS3ExportRequest,
        TEvImport::TEvListObjectsInS3ExportResponse>(std::move(ev), "S3");
}

IActor* CreateListObjectsInFsExportGetter(TEvImport::TEvListObjectsInFsExportRequest::TPtr&& ev) {
    return new TListObjectsInExportGetterFallback<
        TEvImport::TEvListObjectsInFsExportRequest,
        TEvImport::TEvListObjectsInFsExportResponse>(std::move(ev), "FS");
}

} // NSchemeShard
} // NKikimr
