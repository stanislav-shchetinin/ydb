#include "hive_impl.h"

namespace NKikimr::NHive {

class TTxReadInitialBoot : public TTransactionBase<THive> {
    TEvHive::TEvLookupTablet::TPtr Request;
    NKikimrHive::TEvCreateTabletReply Reply;

public:
    TTxReadInitialBoot(TEvHive::TEvLookupTablet::TPtr& ev, THive* hive)
        : TBase(hive), Request(ev) {}

    TTxType GetTxType() const override { return TXTYPE_READ_INITIAL_BOOT; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const auto& request = Request->Get()->Record;
        Reply.Clear();
        Reply.SetOwner(request.GetOwner());
        Reply.SetOrigin(Self->TabletID());
        Reply.SetStatus(NKikimrProto::OK);
        NIceDb::TNiceDb db(txc.DB);
        for (const auto& key : request.GetBootKeys()) {
            const ui64 id = key.GetTabletID();
            auto* result = Reply.AddResults();
            result->SetOwnerIdx(key.GetOwnerIdx());
            if (!id || !key.HasOwnerIdx()) {
                result->SetStatus(NKikimrProto::ERROR);
                continue;
            }
            using T = Schema::Tablet;
            auto row = db.Table<T>().Key(id).Select<T::Owner, T::State, T::InitialBootPending, T::SeizedByChild, T::NeedToReleaseFromParent>();
            if (!row.IsReady()) {
                return false;
            }
            if (row.EndOfSet()) {
                NKikimrHive::TForwardRequest redirect;
                if (Self->CheckForForwardTabletRequest(id, redirect)) {
                    result->SetStatus(NKikimrProto::INVALID_OWNER);
                    *result->MutableForwardRequest() = redirect;
                } else {
                    result->SetStatus(NKikimrProto::NODATA);
                }
            } else if (row.GetValue<T::Owner>() != TOwnerIdxType::TValueType{request.GetOwner(), key.GetOwnerIdx()}
                    || row.GetValue<T::State>() == ETabletState::Deleting) {
                result->SetStatus(NKikimrProto::ERROR);
            } else if (row.GetValueOrDefault<T::SeizedByChild>() || row.GetValueOrDefault<T::NeedToReleaseFromParent>()) {
                result->SetStatus(NKikimrProto::TRYLATER);
            } else {
                result->SetStatus(NKikimrProto::OK);
                result->SetTabletID(id);
                result->SetInitialBootPending(row.GetValueOrDefault<T::InitialBootPending>());
            }
        }
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        auto reply = MakeHolder<TEvHive::TEvCreateTabletReply>();
        reply->Record.Swap(&Reply);
        ctx.Send(Request->Sender, reply.Release(), 0, Request->Cookie);
    }
};

ITransaction* THive::CreateReadInitialBoot(TEvHive::TEvLookupTablet::TPtr& ev) {
    return new TTxReadInitialBoot(ev, this);
}

} // namespace NKikimr::NHive
