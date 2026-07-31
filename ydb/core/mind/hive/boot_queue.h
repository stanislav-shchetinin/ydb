#pragma once

#include "hive.h"
#include "tablet_info.h"

#include <deque>

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

    // Backup tablets are not prioritized between themselves - they are booted in FIFO order,
    // so there is no priority queue here and we can afford to remember the enqueue time,
    // which is needed to guarantee progress (see THive::GetBackupBootBudget).
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
private:
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
    void ReturnToBackupQueueFront(TBackupBootRecord record);
    void AddToBackupWaitQueue(TBackupBootRecord record);
    void IncludeBackupWaitQueue();
    std::optional<TInstant> GetOldestBackupEnqueueTime() const;

private:
    TQueue& GetCurrentQueue();
    double GetBootPriority(const TTabletInfo& tablet) const;
};

}
}
