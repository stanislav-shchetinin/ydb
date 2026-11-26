#pragma once

#include <ydb/library/actors/core/actor.h>
#include <util/generic/array_ref.h>
#include <utility>

namespace NKikimrHive {
class TEvCreateTablet;
}

namespace NKikimr::NSchemeShard::NExportPrepare {

// One bounded Hive call. Replies are delivered to owner as TEvPreparedTabletsResult.
// Offsets select tablets in a reserved range. Set sparse when retrying specific indices.
NActors::IActor* CreateTabletBatchRequest(NActors::TActorId owner, ui64 hive, const NKikimrHive::TEvCreateTablet& range,
    TArrayRef<const ui32> offsets, bool sparse);

// Tuples are (owner index, expected TabletID) from an acknowledged create.
NActors::IActor* CreateTabletBootCheck(NActors::TActorId owner, ui64 hive, ui64 shardOwner,
    TArrayRef<const std::pair<ui64, ui64>> tablets);

struct TCleanupTarget {
    ui64 ShardOwner;
    ui64 OwnerIdx;
    ui64 ObjectId;
    ui64 TabletId; // Zero when the create result was lost.
    ui64 CurrentHive;
};

NActors::IActor* CreateTabletCleanupRequest(NActors::TActorId owner, ui64 hive, const TCleanupTarget& target);

} // namespace NKikimr::NSchemeShard::NExportPrepare
