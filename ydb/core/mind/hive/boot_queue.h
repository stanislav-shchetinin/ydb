#pragma once

#include "hive.h"
#include "tablet_info.h"

#include <deque>
#include <map>
#include <set>

namespace NKikimr {
namespace NHive {

struct TBootQueue {
    struct TBootQueueRecord {
        TTabletId TabletId;
        double Priority;
        TFollowerId FollowerId;
        TNodeId SuggestedNodeId;

        bool operator <(const TBootQueueRecord& o) const {
            return Priority < o.Priority;
        }

        TBootQueueRecord(const TTabletInfo& tablet, double priority, TNodeId suggestedNodeId);
    };

    static_assert(sizeof(TBootQueueRecord) <= 24);

    // Enqueue time is for observability only, never for bypassing admission limits.
    struct TBackupBootRecord {
        TBootQueueRecord Record;
        TInstant EnqueueTime;
    };

    using TQueue = TPriorityQueue<TBootQueueRecord>;
    using TBackupQueue = std::deque<TBackupBootRecord>;
    using TTabletTypeToBootPriority = TMap<TTabletTypes::EType, double>;

    TQueue BootQueue;
    TQueue WaitQueue; // tablets from BootQueue waiting for new nodes
    TBackupQueue BackupBootQueue; // backup tablets, booted at a limited rate
    TBackupQueue BackupWaitQueue; // backup tablets from BackupBootQueue waiting for new nodes
    std::multimap<TInstant, TBootQueueRecord> DeferredQueue;
    std::multimap<TInstant, TBackupBootRecord> BackupDeferredQueue;
private:
    TQueue BlockedQueue; // e.g. foreground followers waiting for their leader
    TQueue BlockedRetryQueue; // snapshot being reconsidered in bounded batches
    bool RetryBlockedRequested = false;
    std::multiset<TInstant> BackupEnqueueTimes;
    bool ProcessWaitQueue = false;
    bool NextFromWaitQueue = false;
    bool PaceBackupTablets = false;

    TTabletTypeToBootPriority TabletTypeToBootPriority;

public:
    void AddToBootQueue(TBootQueueRecord record);
    void AddToBootQueue(const TTabletInfo &tablet, TNodeId node, TInstant now = {});
    void UpdateTabletBootQueuePriorities(const NKikimrConfig::THiveConfig& hiveConfig);
    TBootQueueRecord PopFromBootQueue();
    void AddToWaitQueue(TBootQueueRecord record);
    void Defer(TBootQueueRecord record, TInstant readyAt);
    void PromoteDeferred(TInstant now, size_t limit);
    bool HasReadyMainQueue(TInstant now) const;
    void Block(TBootQueueRecord record);
    void IncludeBlockedQueue();
    void PromoteBlocked(size_t limit);
    bool HasBlockedRecords() const;
    void IncludeWaitQueue();
    void ExcludeWaitQueue();
    bool Empty() const;
    size_t Size() const;

    // Main queue is the one that is processed without any pacing. All the tablets that are not
    // routed into the backup queue live here, so this is what everything except the backup boot
    // pass should look at.
    bool MainQueueEmpty() const;
    size_t MainQueueSize() const;

    void SetPaceBackupTablets(bool pace);
    bool GetPaceBackupTablets() const;
    bool BackupQueueEmpty() const;
    size_t BackupQueueSize() const;
    TBackupBootRecord PopFromBackupQueue();
    void AddToBackupQueue(TBackupBootRecord record);
    void ReturnToBackupQueueFront(TBackupBootRecord record);
    void AddToBackupWaitQueue(TBackupBootRecord record);
    void IncludeBackupWaitQueue(size_t limit);
    void DeferBackup(TBackupBootRecord record, TInstant readyAt);
    void PromoteBackupDeferred(TInstant now, size_t limit);
    void HandOverBackupQueues(size_t limit);
    std::optional<TInstant> GetNextDeferredWakeup(TInstant now) const;
    std::optional<TInstant> GetOldestBackupEnqueueTime() const;

private:
    TQueue& GetCurrentQueue();
    double GetBootPriority(const TTabletInfo& tablet) const;
    void RemoveBackupEnqueueTime(TInstant time);
};

}
}
