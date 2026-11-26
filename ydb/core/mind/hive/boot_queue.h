#pragma once

#include "hive.h"
#include "tablet_info.h"
#include <set>

namespace NKikimr {
namespace NHive {

struct TBootQueue {
    struct TBootQueueRecord {
        TTabletId TabletId;
        double Priority;
        TFollowerId FollowerId;
        TNodeId SuggestedNodeId;
        TInstant QueuedAt;

        bool operator <(const TBootQueueRecord& o) const {
            return Priority < o.Priority;
        }

        TBootQueueRecord(const TTabletInfo& tablet, double priority, TNodeId suggestedNodeId);
    };

    static_assert(sizeof(TBootQueueRecord) <= 32);

    using TQueue = TPriorityQueue<TBootQueueRecord>;
    using TTabletTypeToBootPriority = TMap<TTabletTypes::EType, double>;

    TQueue BootQueue;
    TQueue WaitQueue; // tablets from BootQueue waiting for new nodes
private:
    std::multiset<TInstant> EnqueueTimes;
    bool ProcessWaitQueue = false;
    bool NextFromWaitQueue = false;

    TTabletTypeToBootPriority TabletTypeToBootPriority;

public:
    void AddToBootQueue(TBootQueueRecord record);
    void AddToBootQueue(const TTabletInfo &tablet, TNodeId node, TInstant queuedAt = {});
    void UpdateTabletBootQueuePriorities(const NKikimrConfig::THiveConfig& hiveConfig);
    TBootQueueRecord PopFromBootQueue();
    void AddToWaitQueue(TBootQueueRecord record);
    void IncludeWaitQueue();
    void ExcludeWaitQueue();
    bool Empty() const;
    bool HasNextWaiter() const { return ProcessWaitQueue && NextFromWaitQueue && !WaitQueue.empty(); }
    size_t Size() const;
    TInstant Oldest() const { return EnqueueTimes.empty() ? TInstant() : *EnqueueTimes.begin(); }

private:
    TQueue& GetCurrentQueue();
    double GetBootPriority(const TTabletInfo& tablet) const;
};

}
}
