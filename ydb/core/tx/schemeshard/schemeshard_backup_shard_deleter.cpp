#include "schemeshard_impl.h"
#include "schemeshard_shard_deleter.h"

#include <ydb/core/base/tablet_pipecache.h>

#include <algorithm>

namespace NKikimr::NSchemeShard {

void TShardDeleter::QueueBackup(TTabletId hive, TShardIdx shard, TTabletId tablet, TSchemeShard& ss) {
    if (BackupShards.emplace(shard, TBackupShard{hive, tablet}).second) {
        BackupQueues[hive].ByOwner[shard.GetOwnerId()].push_back({shard});
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
                const ui64 cookie = TakeBackupRequest(hive, queue, size);
                if (!cookie) {
                    continue;
                }
                SendBackup(cookie, ss, ctx);
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

ui64 TShardDeleter::TakeBackupRequest(TTabletId hive, TBackupQueue& queue, ui32 size) {
    auto owner = queue.ByOwner.begin();
    auto& pending = owner->second;
    ui64 cookie = pending.front().RetryCookie;
    if (cookie) {
        pending.pop_front();
        auto it = BackupRequests.find(cookie);
        if (it == BackupRequests.end() || it->second.Committing) {
            // A delayed reply settled this request while another operation held the slot.
            cookie = 0;
        } else if (it->second.Shards.size() > size) {
            // The feature flag or the configured batch limit changed while waiting.
            for (auto shard : it->second.Shards) {
                BackupShards.at(shard).Single = true;
                pending.push_back({shard});
            }
            BackupRequests.erase(it);
            cookie = 0;
        }
    } else {
        cookie = ++NextBackupCookie;
        auto& request = BackupRequests[cookie];
        request.Hive = hive;
        while (!pending.empty() && !pending.front().RetryCookie && request.Shards.size() < size) {
            const auto shard = pending.front().Shard;
            const bool single = BackupShards.at(shard).Single;
            if (single && !request.Shards.empty()) {
                break;
            }
            pending.pop_front();
            request.Shards.push_back(shard);
            if (single) {
                break;
            }
        }
    }
    if (pending.empty()) {
        queue.ByOwner.erase(owner);
    }
    return cookie;
}

void TShardDeleter::SendBackup(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx) {
    auto& request = BackupRequests.at(cookie);
    request.InFlight = true;
    auto message = MakeHolder<TEvHive::TEvDeleteTablet>();
    message->Record.SetShardOwnerId(ui64(request.Shards.front().GetOwnerId()));
    for (auto shard : request.Shards) {
        message->Record.AddShardLocalIdx(ui64(shard.GetLocalId()));
        message->Record.AddTabletID(ui64(BackupShards.at(shard).Tablet));
    }
    ss.TabletCounters->Cumulative()[COUNTER_BACKUP_DELETE_REQUESTS].Increment(1);
    ss.TabletCounters->Cumulative()[COUNTER_BACKUP_DELETE_ITEMS].Increment(request.Shards.size());
    ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Sub(request.Shards.size());
    ctx.Send(MakePipePerNodeCacheID(EPipePerNodeCache::Leader),
        new TEvPipeCache::TEvForward(message.Release(), ui64(request.Hive), false), 0, cookie);
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
    if (request.InFlight) {
        ReleaseBackupSlot(request.Hive, ss);
    } else {
        ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Sub(request.Shards.size());
    }
    for (auto shard : request.Shards) {
        if (success) {
            BackupShards.erase(shard);
        } else {
            auto& info = BackupShards.at(shard);
            info.Single |= singles;
            if (redirect) {
                info.Hive = redirect;
            }
            BackupQueues[info.Hive].ByOwner[shard.GetOwnerId()].push_back({shard});
            ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Add(1);
        }
    }
    DispatchBackup(ss, ctx);
}

void TShardDeleter::BackupCommitted(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx) {
    FinishBackup(cookie, true, false, InvalidTabletId, ss, ctx);
}

void TShardDeleter::RetryBackup(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx) {
    auto* request = BackupRequests.FindPtr(cookie);
    if (request && request->InFlight && !request->Committing) {
        // A timeout releases capacity. Hive can still acknowledge the original request.
        request->InFlight = false;
        ReleaseBackupSlot(request->Hive, ss);
        const auto first = request->Shards.front();
        BackupQueues.at(request->Hive).ByOwner[first.GetOwnerId()].push_back({first, cookie});
        ss.TabletCounters->Simple()[COUNTER_BACKUP_DELETE_QUEUED].Add(request->Shards.size());
        DispatchBackup(ss, ctx);
    }
}

void TSchemeShard::Handle(TEvPrivate::TEvRetryBackupDelete::TPtr& ev, const TActorContext& ctx) {
    ShardDeleter.RetryBackup(ev->Get()->Cookie, *this, ctx);
}

} // namespace NKikimr::NSchemeShard
