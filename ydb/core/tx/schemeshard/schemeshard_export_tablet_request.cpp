#include "schemeshard_export_tablet_request.h"
#include "schemeshard_private.h"

#include <ydb/core/base/hive.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <util/generic/hash_set.h>

namespace NKikimr::NSchemeShard::NExportPrepare {

namespace {

using TResult = TEvPrivate::TEvPreparedTabletsResult;

class TWorker : public TActorBootstrapped<TWorker> {
    const TActorId Owner;
    const ui64 Hive;
    const ui64 ShardOwner;
    const ui64 ObjectId;
    const ui64 CurrentHive;
    enum class EStep { Create, BootCheck, Lookup, Inspect, Delete };
    EStep Step = EStep::Create;
    ui64 LookupTabletId = 0;
    THashSet<ui64> Expected;
    THashMap<ui64, ui64> ExpectedBootIds;
    THolder<IEventBase> Request;
    const TActorId PipeCache = MakePipePerNodeCacheID(EPipePerNodeCache::Leader);

    void SendToHive(IEventBase* event) {
        Send(PipeCache, new TEvPipeCache::TEvForward(event, Hive, /* subscribe */ true));
    }

    void Finish(THolder<TResult> result) {
        Send(Owner, result.Release());
        PassAway();
    }

    void Fail(TResult::EStatus status, TString error) {
        auto result = MakeHolder<TResult>();
        result->Status = status;
        result->Error = std::move(error);
        Finish(std::move(result));
    }

    void CleanupTargetMissing(ui64 redirect = 0) {
        auto result = MakeHolder<TResult>();
        if (redirect && redirect != Hive) {
            result->DeleteReply.SetStatus(NKikimrProto::INVALID_OWNER);
            result->DeleteReply.MutableForwardRequest()->SetHiveTabletId(redirect);
        } else {
            result->DeleteReply.SetStatus(NKikimrProto::OK);
        }
        Finish(std::move(result));
    }

    void Handle(TEvHive::TEvCreateTabletReply::TPtr& ev) {
        auto& record = ev->Get()->Record;
        if (Step == EStep::Lookup) {
            if (record.GetOwner() != ShardOwner || !Expected.contains(record.GetOwnerIdx())) {
                return Fail(TResult::EStatus::Retry, "Unexpected lookup reply during backup cleanup");
            }
            if (record.GetStatus() == NKikimrProto::NODATA) {
                return CleanupTargetMissing(CurrentHive);
            }
            if (record.GetStatus() != NKikimrProto::OK || !record.GetTabletID()) {
                return Fail(TResult::EStatus::Retry, "Cannot resolve reserved backup tablet");
            }
            Step = EStep::Inspect;
            LookupTabletId = record.GetTabletID();
            SendToHive(new TEvHive::TEvRequestHiveInfo(LookupTabletId, false));
            return;
        }
        if (Step != EStep::Create && Step != EStep::BootCheck) {
            return;
        }
        if (!record.HasStatus() || record.GetStatus() != NKikimrProto::OK) {
            const auto transient = record.GetStatus() == NKikimrProto::TRYLATER;
            return Fail(transient ? TResult::EStatus::Retry : TResult::EStatus::Error, record.ShortDebugString());
        }
        if (record.GetOwner() != ShardOwner || record.GetOrigin() != Hive || record.ResultsSize() != Expected.size()) {
            return Fail(TResult::EStatus::Error, "Unexpected owner, Hive or size in backup create reply");
        }
        THashSet<ui64> tablets;
        for (const auto& entry : record.GetResults()) {
            if (Step == EStep::BootCheck && entry.GetStatus() == NKikimrProto::OK) {
                const auto* expected = ExpectedBootIds.FindPtr(entry.GetOwnerIdx());
                if (!expected || *expected != entry.GetTabletID() || !entry.HasInitialBootPending()) {
                    return Fail(TResult::EStatus::Error, "Invalid boot confirmation for reserved tablet");
                }
            }
            if (!entry.HasOwnerIdx() || !Expected.erase(entry.GetOwnerIdx()) || !entry.HasStatus()
                    || (entry.GetStatus() == NKikimrProto::OK && (!entry.GetTabletID() || !tablets.insert(entry.GetTabletID()).second))
                    || (entry.GetStatus() == NKikimrProto::INVALID_OWNER && !entry.GetForwardRequest().GetHiveTabletId())) {
                return Fail(TResult::EStatus::Error, "Invalid entry in backup create reply");
            }
        }
        auto result = MakeHolder<TResult>();
        result->Reply.Swap(&record);
        Finish(std::move(result));
    }

    void Handle(TEvHive::TEvResponseHiveInfo::TPtr& ev) {
        const auto& info = ev->Get()->Record;
        if (Step != EStep::Inspect) {
            return;
        }
        if (ui64 redirect = info.GetForwardRequest().GetHiveTabletId()) {
            return CleanupTargetMissing(redirect);
        }
        if (!info.TabletsSize()) {
            return CleanupTargetMissing(CurrentHive);
        }
        const auto& tablet = info.GetTablets(0);
        if (info.TabletsSize() != 1 || tablet.GetTabletID() != LookupTabletId) {
            return Fail(TResult::EStatus::Retry, "Unexpected tablet in Hive lookup");
        }
        if (tablet.GetTabletOwner().GetOwner() != ShardOwner
                || !Expected.contains(tablet.GetTabletOwner().GetOwnerIdx())
                || !tablet.GetIsBackup() || tablet.GetObjectId() != ObjectId || tablet.GetTabletType() != TTabletTypes::DataShard) {
            // A lost create reply can hide a parameter conflict. Never delete that tablet.
            return CleanupTargetMissing();
        }
        Step = EStep::Delete;
        SendToHive(new TEvHive::TEvDeleteTablet(ShardOwner, tablet.GetTabletOwner().GetOwnerIdx(), tablet.GetTabletID(), 0));
    }

    void Handle(TEvHive::TEvDeleteTabletReply::TPtr& ev) {
        if (Step != EStep::Delete) {
            return;
        }
        auto& record = ev->Get()->Record;
        if (!record.HasStatus() || record.GetOrigin() != Hive || record.GetShardOwnerId() != ShardOwner
                || record.ShardLocalIdxSize() != 1 || !Expected.contains(record.GetShardLocalIdx(0))) {
            return Fail(TResult::EStatus::Retry, "Unrecognized backup cleanup reply");
        }
        auto result = MakeHolder<TResult>();
        result->DeleteReply.Swap(&record);
        Finish(std::move(result));
    }

    STRICT_STFUNC(StateWork,
        hFunc(TEvHive::TEvCreateTabletReply, Handle);
        hFunc(TEvHive::TEvResponseHiveInfo, Handle);
        hFunc(TEvHive::TEvDeleteTabletReply, Handle);
        case TEvPipeCache::TEvDeliveryProblem::EventType:
        case TEvents::TEvWakeup::EventType:
            Fail(TResult::EStatus::Retry, "Hive pipe disconnected or timed out");
            break;
        IgnoreFunc(TEvHive::TEvTabletCreationResult);
        sFunc(TEvents::TEvPoison, PassAway);
    )

public:
    TWorker(TActorId owner, ui64 hive, const NKikimrHive::TEvCreateTablet& range,
            TArrayRef<const ui32> offsets, bool sparse)
        : Owner(owner)
        , Hive(hive)
        , ShardOwner(range.GetOwner())
        , ObjectId(range.GetObjectId())
        , CurrentHive(0)
    {
        auto request = MakeHolder<TEvHive::TEvCreateTablet>();
        request->Record = range;
        if (sparse) {
            request->Record.ClearCount();
            request->Record.ClearOwnerIdx();
        }
        for (const auto offset : offsets) {
            const ui64 index = range.GetOwnerIdx() + offset;
            Expected.insert(index);
            if (sparse) {
                request->Record.AddOwnerIdxs(index);
            }
        }
        Request = std::move(request);
    }

    TWorker(TActorId owner, ui64 hive, ui64 shardOwner, TArrayRef<const std::pair<ui64, ui64>> tablets)
        : Owner(owner)
        , Hive(hive)
        , ShardOwner(shardOwner)
        , ObjectId(0)
        , CurrentHive(0)
        , Step(EStep::BootCheck)
    {
        auto request = MakeHolder<TEvHive::TEvLookupTablet>();
        request->Record.SetOwner(ShardOwner);
        for (const auto& [index, id] : tablets) {
            Expected.insert(index);
            ExpectedBootIds.emplace(index, id);
            auto* key = request->Record.AddBootKeys();
            key->SetOwnerIdx(index);
            key->SetTabletID(id);
        }
        Request = std::move(request);
    }

    TWorker(TActorId owner, ui64 hive, const TCleanupTarget& target)
        : Owner(owner)
        , Hive(hive)
        , ShardOwner(target.ShardOwner)
        , ObjectId(target.ObjectId)
        , CurrentHive(target.CurrentHive)
    {
        Expected.insert(target.OwnerIdx);
        if (target.TabletId) {
            Step = EStep::Delete;
            Request = MakeHolder<TEvHive::TEvDeleteTablet>(ShardOwner, target.OwnerIdx, target.TabletId, 0);
        } else {
            // Verify ownership when the create reply was lost before the export persisted it.
            Step = EStep::Lookup;
            Request = MakeHolder<TEvHive::TEvLookupTablet>(ShardOwner, target.OwnerIdx);
        }
    }

    void Bootstrap() {
        Become(&TThis::StateWork);
        SendToHive(Request.Release());
        Schedule(TDuration::Seconds(30), new TEvents::TEvWakeup());
    }

    void PassAway() override {
        Send(PipeCache, new TEvPipeCache::TEvUnlink(Hive));
        TActorBootstrapped::PassAway();
    }
};

} // namespace

IActor* CreateTabletBatchRequest(TActorId owner, ui64 hive, const NKikimrHive::TEvCreateTablet& range,
        TArrayRef<const ui32> offsets, bool sparse) {
    return new TWorker(owner, hive, range, offsets, sparse);
}

IActor* CreateTabletBootCheck(TActorId owner, ui64 hive, ui64 shardOwner, TArrayRef<const std::pair<ui64, ui64>> tablets) {
    return new TWorker(owner, hive, shardOwner, tablets);
}

IActor* CreateTabletCleanupRequest(TActorId owner, ui64 hive, const TCleanupTarget& target) {
    return new TWorker(owner, hive, target);
}

} // namespace NKikimr::NSchemeShard::NExportPrepare
