#include "schemeshard_impl.h"
#include "schemeshard_shard_deleter.h"

#include <ydb/core/base/tablet_pipecache.h>

#include <algorithm>

namespace NKikimr::NSchemeShard {

void TShardDeleter::QueueBackup(TTabletId hive, TShardIdx shard, TTabletId tablet, TSchemeShard& ss) {
    if (BackupShards.emplace(shard, TBackupShard{hive, tablet}).second) {
        BackupQueues[hive].ByOwner[shard.GetOwnerId()].push_back(shard);
        ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Add(1);
    }
}

void TShardDeleter::QueuePreparedCleanup(TTabletId hive, const std::tuple<ui64, ui32, ui32>& key, TSchemeShard& ss) {
    BackupQueues[hive].Prepared.push_back(key);
    ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Add(1);
}

void TShardDeleter::ReleaseBackupSlot(TTabletId hive, TSchemeShard& ss) {
    --BackupQueues.at(hive).InFlight;
    ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_IN_FLIGHT].Sub(1);
}

void TShardDeleter::DispatchBackup(TSchemeShard& ss, const TActorContext& ctx) {
    const ui32 limit = Max(1u, AppData()->SchemeShardConfig.GetMaxBackupDeleteBatchesInFlightPerHive());
    const ui32 size = AppData()->FeatureFlags.GetEnablePacedBackupExport()
        ? Max(1u, AppData()->HiveConfig.GetMaxDeleteTabletBatchSize()) : 1u;
    for (auto it = BackupQueues.begin(); it != BackupQueues.end();) {
        auto& [hive, queue] = *it;
        while (queue.InFlight < limit && (!queue.ByOwner.empty() || !queue.Prepared.empty())) {
            // Alternate cancelled/unused reservations with registered shards when both are waiting.
            if (!queue.Prepared.empty() && (queue.PreferPrepared || queue.ByOwner.empty())) {
                const auto key = queue.Prepared.front();
                queue.Prepared.pop_front();
                ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Sub(1);
                if (!ss.PreparedTabletDispatcher.StartCleanup(ss, key, ui64(hive), ctx)) {
                    continue;
                }
                queue.PreferPrepared = false;
            } else {
                SendBackup(hive, queue, size, ss, ctx);
                queue.PreferPrepared = true;
            }
            ++queue.InFlight;
            ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_IN_FLIGHT].Add(1);
        }
        if (!queue.InFlight && queue.ByOwner.empty() && queue.Prepared.empty()) {
            BackupQueues.erase(it++);
        } else {
            ++it;
        }
    }
}

void TShardDeleter::SendBackup(TTabletId hive, TBackupQueue& queue, ui32 size, TSchemeShard& ss, const TActorContext& ctx) {
    const ui64 cookie = ++NextBackupCookie;
    auto& request = BackupRequests[cookie];
    request.Hive = hive;
    auto message = MakeHolder<TEvHive::TEvDeleteTablet>();
    auto owner = queue.ByOwner.begin();
    message->Record.SetShardOwnerId(ui64(owner->first));
    auto& pending = owner->second;
    while (!pending.empty() && request.Shards.size() < size) {
        const auto shard = pending.front();
        const auto& info = BackupShards.at(shard);
        if (info.Single && !request.Shards.empty()) {
            break;
        }
        pending.pop_front();
        request.Shards.push_back(shard);
        message->Record.AddShardLocalIdx(ui64(shard.GetLocalId()));
        message->Record.AddTabletID(ui64(info.Tablet));
        if (info.Single) {
            break;
        }
    }
    if (pending.empty()) {
        queue.ByOwner.erase(owner);
    }
    ss.TabletCounters->Cumulative()[COUNTER_BACKUP_DELETE_REQUESTS].Increment(1);
    ss.TabletCounters->Cumulative()[COUNTER_BACKUP_DELETE_ITEMS].Increment(request.Shards.size());
    ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Sub(request.Shards.size());
    ctx.Send(MakePipePerNodeCacheID(EPipePerNodeCache::Leader),
        new TEvPipeCache::TEvForward(message.Release(), ui64(hive), false), 0, cookie);
    ctx.Schedule(TDuration::Seconds(5), new TEvPrivate::TEvRetryBackupDelete(cookie));
}

bool TShardDeleter::AcceptBackupReply(ui64 cookie, const NKikimrHive::TEvDeleteTabletReply& reply, TSchemeShard& ss, const TActorContext& ctx) {
    auto* request = BackupRequests.FindPtr(cookie);
    if (!request || request->Committing || ui64(request->Hive) != reply.GetOrigin()) {
        return false;
    }
    THashSet<TShardIdx> echoed;
    for (auto idx : reply.GetShardLocalIdx()) {
        echoed.emplace(reply.GetShardOwnerId(), idx);
    }
    const bool exact = reply.HasStatus() && reply.HasShardOwnerId() && request->Shards.size() == reply.ShardLocalIdxSize()
        && std::ranges::all_of(request->Shards, [&](auto shard) { return echoed.contains(shard); });
    if (exact && reply.GetStatus() == NKikimrProto::OK) {
        request->Committing = true;
        return true;
    }
    if (request->Shards.size() > 1) {
        ss.TabletCounters->Cumulative()[COUNTER_BACKUP_DELETE_FALLBACK].Increment(request->Shards.size());
        FinishBackup(cookie, false, true, InvalidTabletId, ss, ctx);
    } else if (exact && reply.GetStatus() == NKikimrProto::INVALID_OWNER) {
        const TTabletId target(reply.GetForwardRequest().GetHiveTabletId());
        if (ui64(target) != 0 && target != InvalidTabletId && target != request->Hive) {
            FinishBackup(cookie, false, true, target, ss, ctx);
        }
    }
    // Other single-item failures keep their slot until the retry timer fires.
    return false;
}

void TShardDeleter::FinishBackup(ui64 cookie, bool success, bool singles, TTabletId redirect, TSchemeShard& ss, const TActorContext& ctx) {
    auto it = BackupRequests.find(cookie);
    if (it == BackupRequests.end()) {
        return;
    }
    auto request = std::move(it->second);
    BackupRequests.erase(it);
    ReleaseBackupSlot(request.Hive, ss);
    for (auto shard : request.Shards) {
        if (success) {
            BackupShards.erase(shard);
        } else {
            auto& info = BackupShards.at(shard);
            info.Single |= singles;
            if (redirect) {
                info.Hive = redirect;
            }
            BackupQueues[info.Hive].ByOwner[shard.GetOwnerId()].push_back(shard);
            ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Add(1);
        }
    }
    DispatchBackup(ss, ctx);
}

void TShardDeleter::BackupCommitted(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx) {
    FinishBackup(cookie, true, false, InvalidTabletId, ss, ctx);
}

void TShardDeleter::RetryBackup(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx) {
    const auto* request = BackupRequests.FindPtr(cookie);
    if (request && !request->Committing) {
        FinishBackup(cookie, false, false, InvalidTabletId, ss, ctx);
    }
}

void TSchemeShard::Handle(TEvPrivate::TEvRetryBackupDelete::TPtr& ev, const TActorContext& ctx) {
    ShardDeleter.RetryBackup(ev->Get()->Cookie, *this, ctx);
}

} // namespace NKikimr::NSchemeShard
