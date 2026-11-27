#pragma once
#include "schemeshard_identificators.h"  // for TShardIdx
#include "schemeshard_info_types.h"  // for TShardInfo

#include <ydb/core/base/tablet_pipe.h>

#include <ydb/library/actors/core/actorid.h>

#include <util/generic/deque.h>

#include <tuple>
#include <util/generic/map.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>


namespace NKikimrHive { class TEvDeleteTabletReply; }

namespace NActors {
    struct TActorContext;
}

namespace NKikimr::NSchemeShard {

class TSchemeShard;

class TShardDeleter {
    struct TBackupShard {
        TTabletId Hive;
        TTabletId Tablet;
        bool Single = false;
    };
    struct TQueuedBackup {
        TShardIdx Shard;
        ui64 RetryCookie = 0;
    };
    struct TBackupQueue {
        TMap<TOwnerId, TDeque<TQueuedBackup>> ByOwner;
        TDeque<std::tuple<ui64, ui32, ui32>> Prepared;
        bool PreferPrepared = true;
        ui32 InFlight = 0;
    };
    struct TBackupRequest {
        TTabletId Hive;
        TVector<TShardIdx> Shards;
        bool InFlight = false;
        bool Committing = false;
    };
    THashMap<TShardIdx, TBackupShard> BackupShards;
    THashMap<TTabletId, TBackupQueue> BackupQueues;
    THashMap<ui64, TBackupRequest> BackupRequests;
    ui64 NextBackupCookie = 0;

    ui64 TakeBackupRequest(TTabletId hive, TBackupQueue& queue, ui32 size);
    void SendBackup(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx);
    void FinishBackup(ui64 cookie, bool success, bool singles, TTabletId redirect, TSchemeShard& ss, const TActorContext& ctx);

    struct TPerHiveDeletions {
        TActorId PipeToHive;
        THashSet<TShardIdx> ShardsToDelete;
    };

    ui64 MyTabletID;
    // Hive TabletID -> non-acked deletions
    THashMap<TTabletId, TPerHiveDeletions> PerHiveDeletions;
    // Tablet -> Hive TabletID
    THashMap<TShardIdx, TTabletId> ShardHive;
    NTabletPipe::TClientRetryPolicy HivePipeRetryPolicy;

public:
    explicit TShardDeleter(ui64 myTabletId)
        : MyTabletID(myTabletId)
        , HivePipeRetryPolicy({})
    {}

    TShardDeleter(const TShardDeleter&) = delete;
    TShardDeleter& operator=(const TShardDeleter&) = delete;

    void Shutdown(const TActorContext& ctx);
    void QueueBackup(TTabletId hive, TShardIdx shard, TTabletId tablet, TSchemeShard& ss);
    void QueuePreparedCleanup(TTabletId hive, const std::tuple<ui64, ui32, ui32>& key, TSchemeShard& ss);
    void ReleaseBackupSlot(TTabletId hive, TSchemeShard& ss);
    void DispatchBackup(TSchemeShard& ss, const TActorContext& ctx);
    bool HasBackup(TShardIdx shard) const { return BackupShards.contains(shard); }
    bool AcceptBackupReply(ui64 cookie, const NKikimrHive::TEvDeleteTabletReply& reply, TSchemeShard& ss, const TActorContext& ctx);
    void BackupCommitted(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx);
    void RetryBackup(ui64 cookie, TSchemeShard& ss, const TActorContext& ctx);

    void SendDeleteRequests(TTabletId hiveTabletId, const THashSet<TShardIdx>& shardsToDelete,
                            const THashMap<TShardIdx, TShardInfo>& shardsInfos, const TActorContext& ctx);
    void ResendDeleteRequests(TTabletId hiveTabletId,
                              const THashMap<TShardIdx, TShardInfo>& shardsInfos, const TActorContext& ctx);
    void ResendDeleteRequest(TTabletId hiveTabletId,
                             const THashMap<TShardIdx, TShardInfo>& shardsInfos, TShardIdx shardIdx, const TActorContext& ctx);
    void RedirectDeleteRequest(TTabletId hiveFromTabletId, TTabletId hiveToTabletId, TShardIdx shardIdx,
                               const THashMap<TShardIdx, TShardInfo>& shardsInfos, const TActorContext& ctx);
    void ShardDeleted(TShardIdx shardIdx, const TActorContext& ctx);
    bool Has(TTabletId hiveTabletId, TActorId pipeClientActorId) const;
    bool Has(TShardIdx shardIdx) const;
    bool Empty() const;
};

} // namespace NKikimr::NSchemeShard
