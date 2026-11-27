#include "hive_impl.h"
#include "hive_log.h"

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::HIVE

namespace NKikimr {
namespace NHive {

class TTxDeleteBase : public TTransactionBase<THive> {
protected:
    TSideEffects SideEffects;

public:
    TTxType GetTxType() const override { return NHive::TXTYPE_DELETE_TABLET; }

    TTxDeleteBase(THive* hive)
        : TBase(hive)
    {}

    void DeleteTablet(TTabletId tabletId, NIceDb::TNiceDb& db) {
        YDB_LOG_DEBUG("THive::TTxDeleteTablet::Execute deleting tablet",
            {"logPrefix", GetLogPrefix()},
            {"tabletId", tabletId});
        TLeaderTabletInfo* tablet = Self->FindTabletEvenInDeleting(tabletId);
        if (tablet != nullptr) {
            if (tablet->SeizedByChild) {
                YDB_LOG_WARN("THive::TTxDeleteTablet tablet seized by child, delete skipped",
                    {"logPrefix", GetLogPrefix()},
                    {"tabletId", tabletId});
                return;
            }
            if (tablet->State != ETabletState::Deleting) {
                if (tablet->IsStarting()) {
                    Self->UpdateCounterTabletsStarting(-1);
                }
                tablet->State = ETabletState::Deleting;
                Self->RecordTabletEvent(*tablet, EHiveEventType::Deleting, EHiveEventReason::OwnerRequest);
                db.Table<Schema::Tablet>().Key(tablet->Id).Update<Schema::Tablet::State, Schema::Tablet::LeaderNode>(ETabletState::Deleting, 0);
                for (const TActorId& actor : tablet->ActorsToNotifyOnRestart) {
                    SideEffects.Send(actor, new TEvPrivate::TEvRestartComplete(tablet->GetFullTabletId(), "delete"));
                }
                tablet->ActorsToNotifyOnRestart.clear();
                if (std::exchange(tablet->IsMarkedForReassign, false)) {
                    Self->UpdateCounterTabletsReassigning(-1);
                }
                tablet->InitiateStop(SideEffects, EHiveEventReason::Deleting);
                for (TTabletInfo& follower : tablet->Followers) {
                    for (const TActorId& actor : follower.ActorsToNotifyOnRestart) {
                        SideEffects.Send(actor, new TEvPrivate::TEvRestartComplete(follower.GetFullTabletId(), "delete"));
                    }
                    follower.ActorsToNotifyOnRestart.clear();
                    if (follower.IsStarting()) {
                        Self->UpdateCounterTabletsStarting(-1);
                    }
                    follower.InitiateStop(SideEffects, EHiveEventReason::Deleting);
                    db.Table<Schema::TabletFollowerTablet>().Key(follower.GetFullTabletId()).Update<Schema::TabletFollowerTablet::FollowerNode>(0);
                }
                Self->BlockStorageForDelete(tabletId, SideEffects);
            } else {
                YDB_LOG_DEBUG("THive::TTxDeleteTablet::Execute tablet already in Deleting state, skipping",
                    {"logPrefix", GetLogPrefix()},
                    {"tabletId", tabletId});
            }
        } else {
            YDB_LOG_WARN("THive::TTxDeleteTablet tablet not found",
                {"logPrefix", GetLogPrefix()},
                {"tabletId", tabletId});
        }
    }
};

class TTxDeleteTablet : public TTxDeleteBase {
    TEvHive::TEvDeleteTablet::TPtr Event;
    // Both legacy scalar constructors set TxId_Deprecated, including for tx id zero.
    // A guarded delete can arrive after pacing is disabled or after Hive recovery.
    bool IsGuarded() const {
        const auto& record = Event->Get()->Record;
        return record.TabletIDSize() && !record.HasTxId_Deprecated();
    }

    const bool Batch;
    const TInstant ReceivedAt;

public:
    TTxDeleteTablet(TEvHive::TEvDeleteTablet::TPtr& ev, THive* hive)
        : TTxDeleteBase(hive)
        , Event(ev)
        , Batch(IsGuarded() && Event->Get()->Record.ShardLocalIdxSize() > 1)
        , ReceivedAt(Batch ? TActivationContext::Now() : TInstant())
    {}

    TTxType GetTxType() const override {
        return Batch ? NHive::TXTYPE_DELETE_TABLET_BATCH : NHive::TXTYPE_DELETE_TABLET;
    }

    void RespondToSender(NKikimrProto::EReplyStatus status, const NKikimrHive::TForwardRequest& forwardRequest = {}) {
        const NKikimrHive::TEvDeleteTablet& rec = Event->Get()->Record;
        auto response = MakeHolder<TEvHive::TEvDeleteTabletReply>(status, Self->TabletID(), rec);
        if (forwardRequest.GetHiveTabletId() != 0) {
            response->Record.MutableForwardRequest()->CopyFrom(forwardRequest);
        }
        YDB_LOG_DEBUG("THive::TTxDeleteTablet::Execute sending delete tablet reply",
            {"logPrefix", GetLogPrefix()},
            {"response", response->Record.ShortDebugString()});
        SideEffects.Send(Event->Sender, response.Release(), 0, Event->Cookie);
    }

    // Resolve every identity and route before changing any tablet state.
    void DeleteByIdentity(const NKikimrHive::TEvDeleteTablet& request, NIceDb::TNiceDb& db) {
        const auto size = request.ShardLocalIdxSize();
        if (!request.GetShardOwnerId() || !size || size != request.TabletIDSize()
                || (Batch && size > Self->CurrentConfig.GetMaxDeleteTabletBatchSize())) {
            return RespondToSender(NKikimrProto::ERROR);
        }
        THashSet<ui64> indices;
        THashSet<TTabletId> ids;
        NKikimrHive::TForwardRequest redirect;
        size_t remote = 0;
        for (size_t pos = 0; pos < size; ++pos) {
            const ui64 idx = request.GetShardLocalIdx(pos);
            const TTabletId id = request.GetTabletID(pos);
            const TOwnerIdxType::TValueType owner(request.GetShardOwnerId(), idx);
            if (!id || !indices.insert(idx).second || !ids.insert(id).second) {
                return RespondToSender(NKikimrProto::ERROR);
            }
            if (const auto* tablet = Self->FindTabletEvenInDeleting(id)) {
                if (!tablet->IsBackup || tablet->Owner != owner) {
                    return RespondToSender(NKikimrProto::ERROR);
                }
                if (tablet->SeizedByChild || tablet->NeedToReleaseFromParent) {
                    return RespondToSender(NKikimrProto::TRYLATER);
                }
            } else {
                NKikimrHive::TForwardRequest next;
                if (Self->CheckForForwardTabletRequest(id, next)) {
                    if (redirect.HasHiveTabletId() && redirect.GetHiveTabletId() != next.GetHiveTabletId()) {
                        return RespondToSender(NKikimrProto::ERROR);
                    }
                    redirect = next;
                    ++remote;
                }
                // A missing local ID has already been removed. Its owner index may be reused.
                continue;
            }
            // Validate the owner index of a local tablet before changing its state.
            const auto byOwner = Self->OwnerToTablet.find(owner);
            if (byOwner != Self->OwnerToTablet.end() && byOwner->second != id) {
                return RespondToSender(NKikimrProto::ERROR);
            }
        }
        if (remote) {
            if (remote == size) {
                return RespondToSender(NKikimrProto::INVALID_OWNER, redirect);
            }
            return RespondToSender(NKikimrProto::ERROR);
        }
        // A pending create may reuse the owner index and has no matching TabletID yet.
        for (const TTabletId id : request.GetTabletID()) {
            DeleteTablet(id, db);
        }
        RespondToSender(NKikimrProto::OK);
    }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        SideEffects.Reset(Self->SelfId());
        const NKikimrHive::TEvDeleteTablet& rec = Event->Get()->Record;
        YDB_LOG_DEBUG("THive::TTxDeleteTablet::Execute processing delete tablet request",
            {"logPrefix", GetLogPrefix()},
            {"request", rec.ShortDebugString()});
        if (IsGuarded()) {
            NIceDb::TNiceDb db(txc.DB);
            DeleteByIdentity(rec, db);
            return true;
        }
        // resolving ownerid:owneridx to tabletids
        std::vector<TTabletId> tablets;
        tablets.reserve(rec.ShardLocalIdxSize());
        ui64 owner = rec.GetShardOwnerId();
        for (size_t pos = 0; pos < rec.ShardLocalIdxSize(); ++pos) {
            ui64 idx = rec.GetShardLocalIdx(pos);
            TOwnerIdxType::TValueType ownerIdx(owner, idx);
            auto it = Self->OwnerToTablet.find(ownerIdx);
            if (it != Self->OwnerToTablet.end()) {
                tablets.push_back(it->second);
            } else {
                if (pos < rec.TabletIDSize()) {
                    TTabletId tabletId = rec.GetTabletID(pos);
                    YDB_LOG_WARN("THive::TTxDeleteTablet tablet not found by owner index, using supplied tablet id",
                        {"logPrefix", GetLogPrefix()},
                        {"ownerIdx", ownerIdx},
                        {"tabletId", tabletId});
                    tablets.push_back(tabletId);
                } else {
                    YDB_LOG_WARN("THive::TTxDeleteTablet tablet not found by owner index",
                        {"logPrefix", GetLogPrefix()},
                        {"ownerIdx", ownerIdx});
                }
            }
            if (Self->PendingCreateTablets.erase({owner, idx}) != 0) {
                YDB_LOG_NOTICE("THive::TTxDeleteTablet cleared tablet from pending creates",
                    {"logPrefix", GetLogPrefix()},
                    {"ownerIdx", ownerIdx});
            }
        }
        // checking for possible forwards
        NKikimrHive::TForwardRequest forwardRequest;
        for (TTabletId tabletId : tablets) {
            TTabletId prevForwardHiveTabletId = forwardRequest.GetHiveTabletId();
            if (Self->CheckForForwardTabletRequest(tabletId, forwardRequest)) {
                if (prevForwardHiveTabletId != 0 && prevForwardHiveTabletId != forwardRequest.GetHiveTabletId()) {
                    YDB_LOG_ERROR("THive::TTxDeleteTablet cannot forward delete request, tablets have different owners",
                        {"logPrefix", GetLogPrefix()});
                    RespondToSender(NKikimrProto::ERROR);
                    return true; // abort transaction
                }
            }
        }
        // respond with forward
        if (forwardRequest.GetHiveTabletId() != 0) {
            // actually this status from blob storage, but I think it fits this situation perfectly
            RespondToSender(NKikimrProto::INVALID_OWNER, forwardRequest);
            return true; // abort transaction
        }
        // checking for possible migration
        for (TTabletId tabletId : tablets) {
           TLeaderTabletInfo* tablet = Self->FindTabletEvenInDeleting(tabletId);
            if (tablet != nullptr) {
                if (tablet->SeizedByChild) {
                    YDB_LOG_WARN("THive::TTxDeleteTablet tablet seized by child, delete skipped",
                        {"logPrefix", GetLogPrefix()},
                        {"tabletId", tabletId});
                    RespondToSender(NKikimrProto::ERROR);
                    return true; // abort transaction
                }
            }
        }
        NIceDb::TNiceDb db(txc.DB);
        for (TTabletId tabletId : tablets) {
            DeleteTablet(tabletId, db);
        }
        RespondToSender(NKikimrProto::OK);
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        YDB_LOG_DEBUG("THive::TTxDeleteTablet::Complete",
            {"logPrefix", GetLogPrefix()},
            {"sideEffects", SideEffects});
        if (Batch) {
            auto& counters = Self->TabletCounters->Percentile();
            counters[NHive::COUNTER_DELETE_TABLET_BATCH_SIZE].IncrementFor(Event->Get()->Record.ShardLocalIdxSize());
            counters[NHive::COUNTER_DELETE_TABLET_BATCH_DURATION_MS].IncrementFor((ctx.Now() - ReceivedAt).MilliSeconds());
        }
        SideEffects.Complete(ctx, Self->Requests);
    }
};

class TTxDeleteOwnerTablets : public TTxDeleteBase {
    TEvHive::TEvDeleteOwnerTablets::TPtr Event;

public:
    TTxDeleteOwnerTablets(TEvHive::TEvDeleteOwnerTablets::TPtr& ev, THive* hive)
        : TTxDeleteBase(hive)
        , Event(ev)
    {}

    void RespondToSender(NKikimrProto::EReplyStatus status) {
        const NKikimrHive::TEvDeleteOwnerTablets& rec = Event->Get()->Record;
        auto response = MakeHolder<TEvHive::TEvDeleteOwnerTabletsReply>(status, Self->TabletID(), rec.GetOwner(), rec.GetTxId());
        YDB_LOG_DEBUG("THive::TTxDeleteOwnerTablets::Execute sending delete owner tablets reply",
            {"logPrefix", GetLogPrefix()},
            {"response", response->Record.ShortDebugString()});
        SideEffects.Send(Event->Sender, response.Release(), 0, Event->Cookie);
    }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        SideEffects.Reset(Self->SelfId());
        const NKikimrHive::TEvDeleteOwnerTablets& rec = Event->Get()->Record;
        YDB_LOG_DEBUG("THive::TTxDeleteOwnerTablets::Execute processing delete owner tablets request",
            {"logPrefix", GetLogPrefix()},
            {"request", rec.ShortDebugString()});
        // resolving owner to tabletids
        std::vector<TTabletId> tablets;
        ui64 owner = rec.GetOwner();
        for (const auto& [ownerIdx, tabletId] : Self->OwnerToTablet) {
            if (ownerIdx.first != owner) {
                continue;
            }
            tablets.push_back(tabletId);
            if (Self->PendingCreateTablets.erase(ownerIdx) != 0) {
                YDB_LOG_NOTICE("THive::TTxDeleteOwnerTablets cleared tablet from pending creates",
                    {"logPrefix", GetLogPrefix()},
                    {"ownerIdx", ownerIdx});
            }
        }
        // checking for possible migration
        for (TTabletId tabletId : tablets) {
           TLeaderTabletInfo* tablet = Self->FindTabletEvenInDeleting(tabletId);
            if (tablet != nullptr) {
                if (tablet->SeizedByChild) {
                    YDB_LOG_WARN("THive::TTxDeleteOwnerTablets tablet seized by child, delete aborted",
                        {"logPrefix", GetLogPrefix()},
                        {"tabletId", tabletId});
                    RespondToSender(NKikimrProto::ERROR);
                    return true; // abort transaction
                }
            }
        }
        NIceDb::TNiceDb db(txc.DB);
        for (TTabletId tabletId : tablets) {
            DeleteTablet(tabletId, db);
        }
        db.Table<Schema::BlockedOwner>().Key(rec.GetOwner()).Update();
        Self->BlockedOwners.emplace(Event->Get()->Record.GetOwner());
        RespondToSender(NKikimrProto::OK);
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        YDB_LOG_DEBUG("THive::TTxDeleteOwnerTablets::Complete",
            {"logPrefix", GetLogPrefix()},
            {"ownerId", Event->Get()->Record.GetOwner()},
            {"sideEffects", SideEffects});
        SideEffects.Complete(ctx, Self->Requests);
    }
};

ITransaction* THive::CreateDeleteTablet(TEvHive::TEvDeleteTablet::TPtr& ev) {
    return new TTxDeleteTablet(ev, this);
}

ITransaction* THive::CreateDeleteOwnerTablets(TEvHive::TEvDeleteOwnerTablets::TPtr& ev) {
    return new TTxDeleteOwnerTablets(ev, this);
}

} // NHive
} // NKikimr
