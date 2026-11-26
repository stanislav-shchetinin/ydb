#pragma once

#include "schemeshard_identificators.h"
#include <ydb/library/actors/core/actor.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>
#include <ydb/core/protos/prepared_backup_tablets.pb.h>
#include <util/generic/deque.h>
#include <util/generic/hash_set.h>

namespace NKikimr::NSchemeShard {

struct TExportInfo;
class TSchemeShard;

namespace NExportPrepare {

using TBatch = NKikimrSchemeShard::TPreparedBackupTabletBatch;

bool ReserveTablets(TSchemeShard& ss, TExportInfo& info, NIceDb::TNiceDb& db, TString& error);
void ReleaseReservations(TSchemeShard& ss, TExportInfo& info, NIceDb::TNiceDb& db);
TMaybe<TPathId> ReservedPath(TSchemeShard& ss, TTxId txId, TPathId source, const TString& destination);
const TVector<TBatch>* FindBatches(const TSchemeShard& ss, TPathId pathId);
TTabletId PreparedTablet(const TVector<TBatch>& batches, TShardIdx shardIdx);
void PersistBatch(NIceDb::TNiceDb& db, ui64 exportId, ui32 item, ui32 batch, const TBatch& data);

struct TWork {
    ui64 ExportId;
    ui32 Item;
    ui32 Batch;
    ui64 Hive;
    TVector<ui32> Offsets;
    bool Delete;
    TInstant SentAt;
};

using TKey = std::tuple<ui64, ui32, ui32>; // export, item, batch

class TDispatcher {
    THashMap<ui64, TDeque<TKey>> Queue;
    THashMap<ui64, ui32> InFlight;
    THashMap<TActorId, TWork> Workers;
    THashSet<TKey> QueuedOrRunning;
    THashMap<ui64, THashSet<std::pair<ui32, ui32>>> Remaining;
    THashSet<ui64> Finishing;

    void QueueBatch(TSchemeShard& ss, const TKey& key, const TActorContext& ctx);
    void Pump(TSchemeShard& ss, const TActorContext& ctx);

public:
    void Begin(TSchemeShard& ss, ui64 exportId, const TActorContext& ctx);
    void ResumeBatch(TSchemeShard& ss, const TKey& key, const TActorContext& ctx);
    const TWork* Find(const TActorId& worker) const;
    void Finished(TSchemeShard& ss, const TActorId& worker, bool retry, const TActorContext& ctx);
    bool IsDrained(ui64 exportId) const;
    bool MarkFinishing(ui64 exportId) { return Finishing.insert(exportId).second; }
    void Forget(ui64 exportId) {
        Remaining.erase(exportId);
        Finishing.erase(exportId);
    }
    void Shutdown(const TActorContext& ctx);
};

} // namespace NExportPrepare
} // namespace NKikimr::NSchemeShard
