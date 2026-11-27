#include "hive_impl.h"

#include <ydb/library/actors/core/log.h>

#include <cmath>

namespace NKikimr::NHive {

void THive::StartPhysicalDelete(TLeaderTabletInfo& tablet, TSideEffects& sideEffects, TDuration waited) {
    const bool inserted = ActiveDeletes.emplace(tablet.Id, tablet.PacedDelete).second;
    AFL_ENSURE(inserted)("hive_id", TabletID())("tablet_id", tablet.Id);
    ++DeleteTabletInProgress;
    ActiveBackupDeletes += tablet.PacedDelete;
    if (TabletCounters) {
        TabletCounters->Percentile()[tablet.PacedDelete ? COUNTER_BACKUP_DELETE_WAIT_MS : COUNTER_ORDINARY_DELETE_WAIT_MS].IncrementFor(waited.MilliSeconds());
        TabletCounters->Cumulative()[tablet.PacedDelete ? COUNTER_BACKUP_DELETE_STARTED : COUNTER_ORDINARY_DELETE_STARTED].Increment(1);
    }
    if (!tablet.InitiateBlockStorage(sideEffects, std::numeric_limits<ui32>::max())) {
        DeleteTabletWithoutStorage(&tablet, sideEffects);
    }
    UpdateCounterTabletsDeleting();
    UpdateDeleteClassCounters(TActivationContext::Now());
}

void THive::ReleaseDeleteSlot(TTabletId id, bool completed) {
    auto it = ActiveDeletes.find(id);
    if (it == ActiveDeletes.end()) {
        return;
    }
    const bool backup = it->second;
    ActiveDeletes.erase(it);
    --DeleteTabletInProgress;
    ActiveBackupDeletes -= backup;
    if (TabletCounters && completed) {
        TabletCounters->Cumulative()[backup ? COUNTER_BACKUP_DELETE_COMPLETED : COUNTER_ORDINARY_DELETE_COMPLETED].Increment(1);
    }
    UpdateCounterTabletsDeleting();
    UpdateDeleteClassCounters(TActivationContext::Now());
    ScheduleDeleteQueue();
}

void THive::UpdateDeleteClassCounters(TInstant now) {
    if (!TabletCounters) {
        return;
    }
    auto& counters = TabletCounters->Simple();
    counters[COUNTER_BACKUP_DELETE_SIZE].Set(BackupDeleteQueue.size());
    counters[COUNTER_ORDINARY_DELETE_SIZE].Set(DeleteTabletQueue.size());
    counters[COUNTER_BACKUP_DELETE_IN_FLIGHT].Set(ActiveBackupDeletes);
    counters[COUNTER_ORDINARY_DELETE_IN_FLIGHT].Set(DeleteTabletInProgress - ActiveBackupDeletes);
    counters[COUNTER_BACKUP_DELETE_OLDEST_MS].Set(BackupDeleteQueue.empty() ? 0 : (now - BackupDeleteQueue.front().EnqueuedAt).MilliSeconds());
    counters[COUNTER_ORDINARY_DELETE_OLDEST_MS].Set(DeleteTabletQueue.empty() ? 0 : (now - DeleteTabletQueue.front().EnqueuedAt).MilliSeconds());
}

void THive::ScheduleDeleteQueue() {
    if ((!DeleteTabletQueue.empty() || !BackupDeleteQueue.empty()) && !DeleteQueueScheduled) {
        DeleteQueueScheduled = true;
        Send(SelfId(), new TEvPrivate::TEvProcessDeleteQueue());
    }
}

void THive::Handle(TEvPrivate::TEvProcessDeleteQueue::TPtr& ev) {
    if (ev->Get()->QuotaTimer) {
        DeleteQuotaWakeupScheduled = false;
        ScheduleDeleteQueue();
    } else {
        Execute(CreateProcessDeleteQueue());
    }
}

void THive::DrainDeleteQueue(NIceDb::TNiceDb& db, TSideEffects& sideEffects) {
    DeleteQueueScheduled = false;
    const auto now = TActivationContext::Now();
    const i64 total = GetMaxDeleteTabletInProgress();
    const auto& limits = CurrentConfig.GetBackupDelete();
    const i64 reserve = total > 1 ? std::clamp<i64>(limits.GetReservedOrdinarySlots(), 1, total - 1) : 0;
    const i64 backupLimit = std::min<i64>(std::max(1u, limits.GetMaxInFlight()), total - reserve);
    const ui32 burst = std::max(1u, limits.GetBurst());
    const ui32 rate = std::max(1u, limits.GetRatePerSecond());
    const auto period = TDuration::MicroSeconds(std::max<ui64>(1, ui64(burst) * 1000000 / rate));
    if (BackupDeleteQuota.BucketSize != burst || BackupDeleteQuota.BucketDuration != period) {
        BackupDeleteQuota = TCountedLeakyBucket(burst, period, now);
    }
    BackupDeleteQuota.Update(now);
    while (DeleteTabletInProgress < total && (!DeleteTabletQueue.empty() || !BackupDeleteQueue.empty())) {
        const bool ordinaryWaiting = !DeleteTabletQueue.empty();
        const bool backupWaiting = !BackupDeleteQueue.empty() && ActiveBackupDeletes < backupLimit;
        const bool needsOrdinarySlot = DeleteTabletInProgress - ActiveBackupDeletes < reserve;
        bool backup = backupWaiting && (!ordinaryWaiting || (!needsOrdinarySlot &&
            BackupDeleteQueue.front().EnqueuedAt < DeleteTabletQueue.front().EnqueuedAt));
        if (backup && !BackupDeleteQuota.CanPush(1)) {
            if (!DeleteQuotaWakeupScheduled) {
                DeleteQuotaWakeupScheduled = true;
                const double missing = std::max(0.25, 1.0 - BackupDeleteQuota.Available);
                const auto delay = TDuration::MicroSeconds(std::max<ui64>(1, std::ceil(missing * period.MicroSeconds() / burst)));
                sideEffects.Schedule(delay, new TEvPrivate::TEvProcessDeleteQueue(true));
            }
            backup = false;
        }
        if (!backup && !ordinaryWaiting) {
            break;
        }
        auto& queue = backup ? BackupDeleteQueue : DeleteTabletQueue;
        const auto queued = queue.front();
        queue.pop();
        QueuedDeletes.erase(queued.Tablet);
        auto* tablet = FindTabletEvenInDeleting(queued.Tablet);
        if (!tablet || !tablet->IsDeleting()) {
            continue;
        }
        if (backup) {
            BackupDeleteQuota.Push(now, 1);
            tablet->InitiateStop(sideEffects, EHiveEventReason::Deleting);
            db.Table<Schema::Tablet>().Key(tablet->Id).Update<Schema::Tablet::LeaderNode>(0);
            for (auto& follower : tablet->Followers) {
                follower.InitiateStop(sideEffects, EHiveEventReason::Deleting);
                db.Table<Schema::TabletFollowerTablet>().Key(follower.GetFullTabletId()).Update<Schema::TabletFollowerTablet::FollowerNode>(0);
            }
        }
        StartPhysicalDelete(*tablet, sideEffects, now - queued.EnqueuedAt);
    }
    UpdateCounterDeleteTabletQueueSize();
    UpdateDeleteClassCounters(now);
}

class TTxProcessDeleteQueue : public TTransactionBase<THive> {
    TSideEffects SideEffects;
public:
    explicit TTxProcessDeleteQueue(THive* self) : TBase(self) {}
    TTxType GetTxType() const override { return TXTYPE_PROCESS_DELETE_QUEUE; }
    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        SideEffects.Reset(Self->SelfId());
        NIceDb::TNiceDb db(txc.DB);
        Self->DrainDeleteQueue(db, SideEffects);
        return true;
    }
    void Complete(const TActorContext& ctx) override {
        SideEffects.Complete(ctx, Self->Requests);
    }
};

ITransaction* THive::CreateProcessDeleteQueue() {
    return new TTxProcessDeleteQueue(this);
}

} // namespace NKikimr::NHive
