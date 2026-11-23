#include "schemeshard_export_prepare.h"
#include "schemeshard_export_tablet_request.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_export_flow_proposals.h"

#include <ydb/core/base/hive.h>
#include <util/generic/xrange.h>

#include <algorithm>

namespace NKikimr::NSchemeShard::NExportPrepare {

void PersistBatch(NIceDb::TNiceDb& db, ui64 exportId, ui32 item, ui32 index, const TBatch& data) {
    using TRows = Schema::ExportPreparedTabletBatches;
    db.Table<TRows>().Key(exportId, item, index).Update<TRows::Batch>(data);
}

TMaybe<TPathId> ReservedPath(TSchemeShard& ss, TTxId txId, TPathId source, const TString& destination) {
    const auto* operation = ss.TxIdToExport.FindPtr(txId);
    if (!operation) {
        return {};
    }
    const auto& info = *ss.Exports.at(operation->first);
    if (info.State != TExportInfo::EState::CopyTables) {
        return {};
    }
    const auto [begin, end] = info.PreparedItemsBySource.equal_range(source);
    for (auto it = begin; it != end; ++it) {
        const auto& item = info.Items[it->second];
        if (ExportItemPathName(&ss, info, it->second) == destination) {
            return TPathId(ss.TabletID(), item.PreparedTablets.front().GetCreate().GetObjectId());
        }
    }
    return {};
}

const TVector<TBatch>* FindBatches(const TSchemeShard& ss, TPathId pathId) {
    if (const auto* location = ss.PreparedBackupPaths.FindPtr(pathId)) {
        return &ss.Exports.at(location->first)->Items.at(location->second).PreparedTablets;
    }
    return nullptr;
}

TTabletId PreparedTablet(const TVector<TBatch>& batches, TShardIdx shardIdx) {
    const auto& first = batches.front().GetCreate();
    const ui64 index = ui64(shardIdx.GetLocalId());
    if (ui64(shardIdx.GetOwnerId()) != first.GetOwner() || index < first.GetOwnerIdx()) {
        return InvalidTabletId;
    }
    const ui64 offset = index - first.GetOwnerIdx();
    const ui64 batch = offset / first.GetCount();
    const ui32 slot = offset % first.GetCount();
    if (batch >= batches.size() || slot >= batches[batch].TabletsSize()) {
        return InvalidTabletId;
    }
    const ui64 tablet = batches[batch].GetTablets(slot).GetTabletId();
    return tablet ? TTabletId(tablet) : InvalidTabletId;
}

bool ReserveTablets(TSchemeShard& ss, TExportInfo& info, NIceDb::TNiceDb& db, TString& error) {
    struct TPlan {
        ui32 Item;
        ui32 Count;
        TTabletId Hive;
        NKikimrHive::TEvCreateTablet Parameters;
    };
    TVector<TPlan> plans;
    for (ui32 itemIdx : xrange(info.Items.size())) {
        const auto& item = info.Items[itemIdx];
        const auto* table = ss.Tables.FindPtr(item.SourcePathId);
        if (!table || (*table)->GetPartitions().empty()) {
            continue;
        }
        const auto domain = ss.ResolvePathIdForDomain(item.SourcePathId);
        TChannelsBindings channels;
        if (ss.IsStorageConfigLogic(*table)) {
            TVector<TStorageRoom> rooms;
            rooms.emplace_back(0);
            THashMap<ui32, ui32> familyRooms;
            if (!ss.GetBindingsRooms(domain, (*table)->PartitionConfig(), rooms, familyRooms, channels, error)) {
                return false;
            }
        } else if (ss.IsCompatibleChannelProfileLogic(domain, *table)) {
            if (!ss.GetChannelsBindings(domain, *table, channels, error)) {
                return false;
            }
        } else {
            continue;
        }
        auto& plan = plans.emplace_back();
        plan.Item = itemIdx;
        plan.Count = (*table)->GetPartitions().size();
        plan.Hive = ss.ResolveHive(item.SourcePathId);
        plan.Parameters = TEvHive::TEvCreateTablet(ss.TabletID(), 0, TTabletTypes::DataShard, channels).Record;
        plan.Parameters.SetIsBackup(true);
        plan.Parameters.AddFollowerGroups(); // OmitFollowers on the consistent copy.
        SetCreateTabletDomains(plan.Parameters, domain, &ss);
    }
    const ui32 limit = AppData()->HiveConfig.GetMaxCreateTabletBatchSize();
    if (!limit && !plans.empty()) {
        error = "MaxCreateTabletBatchSize must be positive to prepare a backup";
        return false;
    }
    for (auto& plan : plans) {
        const auto destination = ss.AllocatePathId();
        plan.Parameters.SetObjectId(destination.LocalPathId);
        ss.PreparedBackupPaths.emplace(destination, std::make_pair(info.Id, plan.Item));
        info.PreparedItemsBySource.emplace(info.Items[plan.Item].SourcePathId, plan.Item);
        auto& batches = info.Items[plan.Item].PreparedTablets;
        const ui64 first = ui64(ss.ReserveShardIdxs(plan.Count).GetLocalId());
        info.PendingCreateCount += plan.Count;
        for (ui32 offset = 0; offset < plan.Count;) {
            const ui32 count = Min(limit, plan.Count - offset);
            auto& batch = batches.emplace_back();
            *batch.MutableCreate() = plan.Parameters;
            batch.MutableCreate()->SetOwnerIdx(first + offset);
            batch.MutableCreate()->SetCount(count);
            batch.SetHive(ui64(plan.Hive));
            batch.MutableTablets()->Reserve(count);
            for (ui32 i = 0; i < count; ++i) {
                batch.AddTablets();
            }
            PersistBatch(db, info.Id, plan.Item, batches.size() - 1, batch);
            offset += count;
        }
    }
    ss.PersistUpdateNextPathId(db);
    ss.PersistUpdateNextShardIdx(db);
    info.PrepareStarted = TAppData::TimeProvider->Now();
    db.Table<Schema::Exports>().Key(info.Id).Update<Schema::Exports::PrepareStartedMs>(info.PrepareStarted.MilliSeconds());
    ss.TabletCounters->Simple()[COUNTER_PREPARE_COPY_PENDING_METADATA].Add(info.PendingCreateCount);
    return true;
}

void ReleaseReservations(TSchemeShard& ss, TExportInfo& info, NIceDb::TNiceDb& db) {
    info.PreparedItemsBySource.clear();
    info.PreparedTabletIds.clear();
    for (ui32 item : xrange(info.Items.size())) {
        auto& batches = info.Items[item].PreparedTablets;
        if (batches.empty()) {
            continue;
        }
        ss.PreparedBackupPaths.erase(TPathId(ss.TabletID(), batches.front().GetCreate().GetObjectId()));
        for (ui32 index : xrange(batches.size())) {
            db.Table<Schema::ExportPreparedTabletBatches>().Key(info.Id, item, index).Delete();
        }
        batches.clear();
    }
}

static TMaybe<TWork> NextWork(TSchemeShard& ss, const TKey& key) {
    const auto [id, itemIdx, batchIdx] = key;
    const auto* info = ss.Exports.FindPtr(id);
    if (!info || ((*info)->State != TExportInfo::EState::PrepareCopyTables &&
                  (*info)->State != TExportInfo::EState::CleaningPreparedTablets &&
                  (*info)->State != TExportInfo::EState::ReleasingUnusedTablets)) {
        return {};
    }
    const auto& batch = (*info)->Items.at(itemIdx).PreparedTablets.at(batchIdx);
    const bool cleanup = (*info)->State != TExportInfo::EState::PrepareCopyTables;
    const ui32 limit = cleanup ? 1u : Max(1u, AppData()->HiveConfig.GetMaxCreateTabletBatchSize());
    const bool checkBoot = !cleanup && std::none_of(batch.GetTablets().begin(), batch.GetTablets().end(),
        [](const auto& slot) { return slot.GetState() == TBatch::Pending; });
    TWork work{id, itemIdx, batchIdx, 0, {}, cleanup, checkBoot, {}};
    for (ui32 offset : xrange(batch.TabletsSize())) {
        const auto& slot = batch.GetTablets(offset);
        if (cleanup) {
            const auto shard = TShardIdx(batch.GetCreate().GetOwner(), TLocalShardIdx(batch.GetCreate().GetOwnerIdx() + offset));
            if (slot.GetState() == TBatch::Deleted || slot.GetState() == TBatch::Conflict || ss.ShardInfos.contains(shard)) {
                continue;
            }
        } else if (slot.GetState() != (checkBoot ? TBatch::Created : TBatch::Pending)) {
            continue;
        }
        const ui64 hive = slot.HasHive() ? slot.GetHive() : batch.GetHive();
        if (work.Hive && work.Hive != hive) {
            continue;
        }
        work.Hive = hive;
        work.Offsets.push_back(offset);
        if (work.Offsets.size() == limit) {
            break;
        }
    }
    if (work.Offsets.empty()) {
        return {};
    }
    return work;
}

void TDispatcher::QueueBatch(TSchemeShard& ss, const TKey& key, const TActorContext&) {
    if (QueuedOrRunning.contains(key)) {
        return;
    }
    const auto [id, item, batch] = key;
    if (auto work = NextWork(ss, key)) {
        QueuedOrRunning.insert(key);
        Queue[work->Hive].push_back(key);
    } else if (auto* remaining = Remaining.FindPtr(id)) {
        remaining->erase({item, batch});
    }
}

void TDispatcher::Begin(TSchemeShard& ss, ui64 id, const TActorContext& ctx) {
    if (!Remaining.contains(id)) {
        const auto& info = *ss.Exports.at(id);
        auto& remaining = Remaining[id];
        for (ui32 i : xrange(info.Items.size())) {
            for (ui32 b : xrange(info.Items[i].PreparedTablets.size())) {
                remaining.emplace(i, b);
                QueueBatch(ss, {id, i, b}, ctx);
            }
        }
    }
    Pump(ss, ctx);
}

void TDispatcher::Pump(TSchemeShard& ss, const TActorContext& ctx) {
    const ui32 limit = Max(1u, AppData()->SchemeShardConfig.GetMaxBackupCreateBatchesInFlightPerHive());
    TVector<TKey> redirected;
    for (auto it = Queue.begin(); it != Queue.end();) {
        const ui64 hive = it->first;
        auto& queue = it->second;
        while (!queue.empty() && InFlight[hive] < limit) {
            const TKey key = queue.front();
            queue.pop_front();
            auto work = NextWork(ss, key);
            if (!work || work->Hive != hive) {
                QueuedOrRunning.erase(key);
                redirected.push_back(key);
                continue;
            }
            const auto& info = *ss.Exports.at(work->ExportId);
            const auto& batch = info.Items.at(work->Item).PreparedTablets.at(work->Batch);
            const auto& range = batch.GetCreate();
            IActor* request = nullptr;
            if (work->Delete) {
                const ui32 offset = work->Offsets.front();
                request = CreateTabletCleanupRequest(ss.SelfId(), hive, {
                    .ShardOwner = range.GetOwner(), .OwnerIdx = range.GetOwnerIdx() + offset,
                    .ObjectId = range.GetObjectId(), .TabletId = batch.GetTablets(offset).GetTabletId(),
                    .CurrentHive = ui64(ss.ResolveHive(info.DomainPathId))});
            } else if (work->CheckBoot) {
                TVector<std::pair<ui64, ui64>> tablets;
                for (ui32 offset : work->Offsets) {
                    tablets.emplace_back(range.GetOwnerIdx() + offset, batch.GetTablets(offset).GetTabletId());
                }
                request = CreateTabletBootCheck(ss.SelfId(), hive, range.GetOwner(), tablets);
                ss.TabletCounters->Cumulative()[COUNTER_PREPARE_COPY_BOOT_CHECKS].Increment(1);
            } else {
                const bool sparse = batch.GetRetried() || work->Offsets.size() != range.GetCount();
                request = CreateTabletBatchRequest(ss.SelfId(), hive, range, work->Offsets, sparse);
                ss.TabletCounters->Simple()[COUNTER_BACKUP_CREATE_BATCHES_IN_FLIGHT].Add(1);
                ss.TabletCounters->Cumulative()[COUNTER_BACKUP_CREATE_BATCHES].Increment(1);
                ss.TabletCounters->Cumulative()[COUNTER_BACKUP_CREATE_ITEMS].Increment(work->Offsets.size());
                if (sparse) {
                    ss.TabletCounters->Cumulative()[COUNTER_BACKUP_CREATE_SPARSE_BATCHES].Increment(1);
                }
            }
            const auto worker = ctx.Register(request);
            work->SentAt = ctx.Now();
            ++InFlight[hive];
            Workers.emplace(worker, std::move(*work));
        }
        if (queue.empty()) {
            Queue.erase(it++);
        } else {
            ++it;
        }
    }
    for (const auto& [id, item, batch] : redirected) {
        ctx.Send(ss.SelfId(), new TEvPrivate::TEvDispatchPreparedTablets(id, item, batch));
    }
}

void TDispatcher::ResumeBatch(TSchemeShard& ss, const TKey& key, const TActorContext& ctx) {
    if (!Remaining.contains(std::get<0>(key))) {
        return;
    }
    QueueBatch(ss, key, ctx);
    Pump(ss, ctx);
}

const TWork* TDispatcher::Find(const TActorId& actor) const {
    return Workers.FindPtr(actor);
}

void TDispatcher::Finished(TSchemeShard& ss, const TActorId& actor, bool retry, const TActorContext& ctx) {
    const auto* work = Find(actor);
    if (!work) {
        return;
    }
    const auto [id, item, batch] = TKey{work->ExportId, work->Item, work->Batch};
    if (!--InFlight.at(work->Hive)) {
        InFlight.erase(work->Hive);
    }
    if (!work->Delete && !work->CheckBoot) {
        ss.TabletCounters->Simple()[COUNTER_BACKUP_CREATE_BATCHES_IN_FLIGHT].Sub(1);
        ss.TabletCounters->Percentile()[COUNTER_BACKUP_CREATE_ATTEMPT_TIME_US].IncrementFor((ctx.Now() - work->SentAt).MicroSeconds());
    }
    Workers.erase(actor);
    QueuedOrRunning.erase({id, item, batch});
    auto* next = new TEvPrivate::TEvDispatchPreparedTablets(id, item, batch);
    if (retry) {
        ctx.Schedule(TDuration::MilliSeconds(250), next);
    } else {
        ctx.Send(ss.SelfId(), next);
    }
    Pump(ss, ctx);
}

bool TDispatcher::IsDrained(ui64 id) const {
    const auto* remaining = Remaining.FindPtr(id);
    return remaining && remaining->empty();
}

void TDispatcher::Shutdown(const TActorContext& ctx) {
    for (const auto& [actor, work] : Workers) {
        ctx.Send(actor, new TEvents::TEvPoison());
    }
}

} // namespace NKikimr::NSchemeShard::NExportPrepare

namespace NKikimr::NSchemeShard {

void TSchemeShard::Handle(TEvPrivate::TEvDispatchPreparedTablets::TPtr& ev, const TActorContext& ctx) {
    const auto& msg = *ev->Get();
    const auto* info = Exports.FindPtr(msg.ExportId);
    if (!info || ((*info)->State != TExportInfo::EState::PrepareCopyTables &&
                  (*info)->State != TExportInfo::EState::CleaningPreparedTablets &&
                  (*info)->State != TExportInfo::EState::ReleasingUnusedTablets)) {
        return;
    }
    if ((*info)->State == TExportInfo::EState::CleaningPreparedTablets && (*info)->WaitTxId != InvalidTxId) {
        return;
    }
    if (msg.Item == Max<ui32>()) {
        PreparedTabletDispatcher.Begin(*this, msg.ExportId, ctx);
    } else {
        PreparedTabletDispatcher.ResumeBatch(*this, {msg.ExportId, msg.Item, msg.Batch}, ctx);
    }
    if (PreparedTabletDispatcher.IsDrained(msg.ExportId) && PreparedTabletDispatcher.MarkFinishing(msg.ExportId)) {
        Execute(CreateTxProgressExport(msg.ExportId), ctx);
    }
}

struct TSchemeShard::TExport::TTxPreparedTabletsResult : NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TEvPrivate::TEvPreparedTabletsResult::TPtr Event;
    bool Retry = false;
    ui64 Cleanup = 0;

    TTxPreparedTabletsResult(TSchemeShard* ss, TEvPrivate::TEvPreparedTabletsResult::TPtr& ev)
        : TBase(ss), Event(ev) {}

    TTxType GetTxType() const override { return TXTYPE_EXPORT_PROGRESS; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const auto* work = Self->PreparedTabletDispatcher.Find(Event->Sender);
        if (!work || !Self->Exports.contains(work->ExportId)) {
            return true;
        }
        auto& info = *Self->Exports.at(work->ExportId);
        auto& batch = info.Items.at(work->Item).PreparedTablets.at(work->Batch);
        using TBatch = NExportPrepare::TBatch;
        using TEvent = TEvPrivate::TEvPreparedTabletsResult;
        const auto& result = *Event->Get();
        TString failure;
        bool changed = !work->CheckBoot;
        if (result.Status == TEvent::EStatus::Retry) {
            Retry = true;
        } else if (result.Status == TEvent::EStatus::Error) {
            failure = result.Error;
        } else if (work->Delete) {
            const auto& deleted = result.DeleteReply;
            auto& slot = *batch.MutableTablets(work->Offsets.front());
            if (deleted.GetStatus() == NKikimrProto::OK) {
                slot.SetState(TBatch::Deleted);
            } else if (deleted.GetStatus() == NKikimrProto::INVALID_OWNER && deleted.GetForwardRequest().GetHiveTabletId()) {
                slot.SetHive(deleted.GetForwardRequest().GetHiveTabletId());
            } else {
                Retry = true;
            }
        } else {
            for (const auto& entry : result.Reply.GetResults()) {
                auto& slot = *batch.MutableTablets(entry.GetOwnerIdx() - batch.GetCreate().GetOwnerIdx());
                switch (entry.GetStatus()) {
                case NKikimrProto::OK:
                    if (work->CheckBoot) {
                        if (info.State == TExportInfo::EState::PrepareCopyTables) {
                            if (entry.GetInitialBootPending()) {
                                Retry = true;
                            } else {
                                changed = true;
                                slot.SetState(TBatch::Ready);
                                --info.WaitingForBoot;
                                Self->TabletCounters->Simple()[COUNTER_PREPARE_COPY_WAITING_BOOT].Sub(1);
                                Self->TabletCounters->Cumulative()[COUNTER_PREPARE_COPY_READY].Increment(1);
                            }
                        }
                        break;
                    }
                    if (!info.PreparedTabletIds.insert(entry.GetTabletID()).second) {
                        failure = "Hive returned the same tablet ID for multiple reserved shards";
                        break;
                    }
                    if (info.State == TExportInfo::EState::PrepareCopyTables && slot.GetState() == TBatch::Pending) {
                        --info.PendingCreateCount;
                        ++info.WaitingForBoot;
                        Self->TabletCounters->Simple()[COUNTER_PREPARE_COPY_WAITING_BOOT].Add(1);
                        Self->TabletCounters->Simple()[COUNTER_PREPARE_COPY_PENDING_METADATA].Sub(1);
                        Self->TabletCounters->Cumulative()[COUNTER_PREPARE_COPY_CREATED].Increment(1);
                    }
                    slot.SetState(TBatch::Created);
                    slot.SetTabletId(entry.GetTabletID());
                    slot.SetHive(work->Hive);
                    break;
                case NKikimrProto::INVALID_OWNER:
                    changed |= slot.GetHive() != entry.GetForwardRequest().GetHiveTabletId();
                    slot.SetHive(entry.GetForwardRequest().GetHiveTabletId());
                    break;
                case NKikimrProto::TRYLATER:
                    Retry = true;
                    break;
                default:
                    if (entry.GetErrorReason() == NKikimrHive::ERROR_REASON_CREATE_PARAMETERS_MISMATCH) {
                        slot.SetState(TBatch::Conflict);
                    }
                    failure = entry.ShortDebugString();
                    break;
                }
            }
        }
        NIceDb::TNiceDb db(txc.DB);
        if (changed) {
            batch.SetRetried(true);
            NExportPrepare::PersistBatch(db, work->ExportId, work->Item, work->Batch, batch);
        }
        if (failure && info.State == TExportInfo::EState::PrepareCopyTables) {
            Self->TabletCounters->Simple()[COUNTER_PREPARE_COPY_WAITING_BOOT].Sub(std::exchange(info.WaitingForBoot, 0));
            Self->TabletCounters->Simple()[COUNTER_PREPARE_COPY_PENDING_METADATA].Sub(std::exchange(info.PendingCreateCount, 0));
            info.Issue = TStringBuilder() << "Failed to prepare backup tablets: " << failure;
            info.State = TExportInfo::EState::CleaningPreparedTablets;
            Self->PersistExportState(db, info);
            Cleanup = info.Id;
        }
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        if (Cleanup) {
            Self->PreparedTabletDispatcher.Forget(Cleanup);
            Self->PreparedTabletDispatcher.Begin(*Self, Cleanup, ctx);
        }
        Self->PreparedTabletDispatcher.Finished(*Self, Event->Sender, Retry, ctx);
    }
};

void TSchemeShard::Handle(TEvPrivate::TEvPreparedTabletsResult::TPtr& ev, const TActorContext& ctx) {
    if (PreparedTabletDispatcher.Find(ev->Sender)) {
        Execute(new TExport::TTxPreparedTabletsResult(this, ev), ctx);
    }
}

} // namespace NKikimr::NSchemeShard
