#include "boot_queue.h"
#include "leader_tablet_info.h"

namespace NKikimr {
namespace NHive {

TBootQueue::TBootQueueRecord::TBootQueueRecord(const TTabletInfo& tablet, double priority, TNodeId suggestedNodeId)
    : TabletId(tablet.GetLeader().Id)
    , Priority(priority)
    , FollowerId(tablet.IsLeader() ? 0 : tablet.AsFollower().Id)
    , SuggestedNodeId(suggestedNodeId)
{
}

void TBootQueue::AddToBootQueue(TBootQueueRecord record) {
    BootQueue.push(record);
}

void TBootQueue::AddToBootQueue(const TTabletInfo &tablet, TNodeId node, TInstant now) {
    double priority = GetBootPriority(tablet);
    // IsBackup is only set on the leader, followers keep the default value
    if (PaceBackupTablets && tablet.GetLeader().IsBackup) {
        AddToBackupQueue({.Record = TBootQueueRecord(tablet, priority, node), .EnqueueTime = now});
        return;
    }
    BootQueue.emplace(tablet, priority, node);
}

void TBootQueue::UpdateTabletBootQueuePriorities(const NKikimrConfig::THiveConfig& hiveConfig) {
    // Fill default priorities
    TTabletTypeToBootPriority updated{
        {TTabletTypes::Hive, 4},
        {TTabletTypes::SchemeShard, 3},
        {TTabletTypes::Mediator, 2},
        {TTabletTypes::Coordinator, 2},
        {TTabletTypes::BlobDepot, 2},
        {TTabletTypes::ColumnShard, 0},
    };
    // Update priorities from config
    for (const auto& entry : hiveConfig.GetTabletTypeToBootPriority()) {
        updated.insert_or_assign(entry.GetTabletType(), entry.GetPriority());
    }
    TabletTypeToBootPriority = std::move(updated);
}

TBootQueue::TBootQueueRecord TBootQueue::PopFromBootQueue() {
    TQueue& currentQueue = GetCurrentQueue();
    TBootQueueRecord record = currentQueue.top();
    currentQueue.pop();
    if (ProcessWaitQueue) {
        NextFromWaitQueue = !NextFromWaitQueue;
    }
    return record;
}

void TBootQueue::AddToWaitQueue(TBootQueueRecord record) {
    WaitQueue.push(record);
}

void TBootQueue::IncludeWaitQueue() {
    ProcessWaitQueue = true;
}

void TBootQueue::ExcludeWaitQueue() {
    ProcessWaitQueue = false;
}

bool TBootQueue::Empty() const {
    return MainQueueEmpty() && DeferredQueue.empty() && !HasBlockedRecords()
        && BackupQueueEmpty() && BackupDeferredQueue.empty();
}

size_t TBootQueue::Size() const {
    return MainQueueSize() + DeferredQueue.size() + BackupQueueSize()
        + BlockedQueue.size() + BlockedRetryQueue.size();
}

bool TBootQueue::MainQueueEmpty() const {
    if (ProcessWaitQueue) {
        return BootQueue.empty() && WaitQueue.empty();
    } else {
        return BootQueue.empty();
    }
}

size_t TBootQueue::MainQueueSize() const {
    if (ProcessWaitQueue) {
        return BootQueue.size() + WaitQueue.size();
    } else {
        return BootQueue.size();
    }
}

void TBootQueue::SetPaceBackupTablets(bool pace) {
    PaceBackupTablets = pace;
}

bool TBootQueue::GetPaceBackupTablets() const {
    return PaceBackupTablets;
}

bool TBootQueue::BackupQueueEmpty() const {
    return BackupBootQueue.empty();
}

size_t TBootQueue::BackupQueueSize() const {
    return BackupBootQueue.size() + BackupDeferredQueue.size();
}

TBootQueue::TBackupBootRecord TBootQueue::PopFromBackupQueue() {
    TBackupBootRecord record = BackupBootQueue.front();
    BackupBootQueue.pop_front();
    RemoveBackupEnqueueTime(record.EnqueueTime);
    return record;
}

void TBootQueue::AddToBackupQueue(TBackupBootRecord record) {
    BackupEnqueueTimes.insert(record.EnqueueTime);
    BackupBootQueue.push_back(record);
}

void TBootQueue::ReturnToBackupQueueFront(TBackupBootRecord record) {
    BackupEnqueueTimes.insert(record.EnqueueTime);
    BackupBootQueue.push_front(record);
}

void TBootQueue::AddToBackupWaitQueue(TBackupBootRecord record) {
    BackupEnqueueTimes.insert(record.EnqueueTime);
    BackupWaitQueue.push_back(record);
}

void TBootQueue::IncludeBackupWaitQueue(size_t limit) {
    while (limit-- && !BackupWaitQueue.empty()) {
        BackupBootQueue.push_back(BackupWaitQueue.front());
        BackupWaitQueue.pop_front();
    }
}

std::optional<TInstant> TBootQueue::GetOldestBackupEnqueueTime() const {
    if (BackupEnqueueTimes.empty()) {
        return std::nullopt;
    }
    return *BackupEnqueueTimes.begin();
}

void TBootQueue::RemoveBackupEnqueueTime(TInstant time) {
    auto it = BackupEnqueueTimes.find(time);
    Y_ABORT_UNLESS(it != BackupEnqueueTimes.end());
    BackupEnqueueTimes.erase(it);
}

void TBootQueue::Defer(TBootQueueRecord record, TInstant readyAt) {
    DeferredQueue.emplace(readyAt, record);
}

void TBootQueue::PromoteDeferred(TInstant now, size_t limit) {
    while (limit-- && !DeferredQueue.empty() && DeferredQueue.begin()->first <= now) {
        BootQueue.push(DeferredQueue.begin()->second);
        DeferredQueue.erase(DeferredQueue.begin());
    }
}

bool TBootQueue::HasReadyMainQueue(TInstant now) const {
    return !MainQueueEmpty() || !BlockedRetryQueue.empty()
        || (RetryBlockedRequested && !BlockedQueue.empty())
        || (!DeferredQueue.empty() && DeferredQueue.begin()->first <= now);
}

void TBootQueue::Block(TBootQueueRecord record) {
    BlockedQueue.push(record);
}

void TBootQueue::IncludeBlockedQueue() {
    RetryBlockedRequested = true;
}

void TBootQueue::PromoteBlocked(size_t limit) {
    if (BlockedRetryQueue.empty() && RetryBlockedRequested) {
        // Do not reconsider newly blocked records in the same retry round.
        // Swapping snapshots is O(1), including for a large follower backlog.
        BlockedRetryQueue.swap(BlockedQueue);
        RetryBlockedRequested = false;
    }
    while (limit && !BlockedRetryQueue.empty()) {
        BootQueue.push(BlockedRetryQueue.top());
        BlockedRetryQueue.pop();
        --limit;
    }
}

bool TBootQueue::HasBlockedRecords() const {
    return !BlockedQueue.empty() || !BlockedRetryQueue.empty();
}

void TBootQueue::DeferBackup(TBackupBootRecord record, TInstant readyAt) {
    BackupEnqueueTimes.insert(record.EnqueueTime);
    BackupDeferredQueue.emplace(readyAt, record);
}

void TBootQueue::PromoteBackupDeferred(TInstant now, size_t limit) {
    while (limit-- && !BackupDeferredQueue.empty() && BackupDeferredQueue.begin()->first <= now) {
        BackupBootQueue.push_back(BackupDeferredQueue.begin()->second);
        BackupDeferredQueue.erase(BackupDeferredQueue.begin());
    }
}

void TBootQueue::HandOverBackupQueues(size_t limit) {
    while (limit && !BackupQueueEmpty()) {
        AddToBootQueue(PopFromBackupQueue().Record);
        --limit;
    }
    while (limit && !BackupDeferredQueue.empty()) {
        auto it = BackupDeferredQueue.begin();
        Defer(it->second.Record, it->first);
        RemoveBackupEnqueueTime(it->second.EnqueueTime);
        BackupDeferredQueue.erase(it);
        --limit;
    }
    while (limit && !BackupWaitQueue.empty()) {
        auto record = BackupWaitQueue.front();
        BackupWaitQueue.pop_front();
        RemoveBackupEnqueueTime(record.EnqueueTime);
        AddToWaitQueue(record.Record);
        --limit;
    }
}

std::optional<TInstant> TBootQueue::GetNextDeferredWakeup(TInstant now) const {
    // Due records are handled by bounded continuations or admission wakeups.
    // They must not hide a future foreground deadline or cause a timer spin.
    std::optional<TInstant> next;
    if (auto it = DeferredQueue.upper_bound(now); it != DeferredQueue.end()) {
        next = it->first;
    }
    if (auto it = BackupDeferredQueue.upper_bound(now); it != BackupDeferredQueue.end()) {
        auto backupNext = it->first;
        next = next ? std::min(*next, backupNext) : backupNext;
    }
    return next;
}

TBootQueue::TQueue& TBootQueue::GetCurrentQueue() {
    if (BootQueue.empty()) {
        return WaitQueue;
    }
    if (WaitQueue.empty()) {
        return BootQueue;
    }
    if (ProcessWaitQueue && NextFromWaitQueue) {
        return WaitQueue;
    }
    return BootQueue;
}

double TBootQueue::GetBootPriority(const TTabletInfo& tablet) const {
    double priority = 0;

    if (tablet.IsLeader()) {
        priority = 1;
        auto tabletType = tablet.GetTabletType();
        const auto* it = TabletTypeToBootPriority.FindPtr(tabletType);
        if (it) {
            priority = *it;
        }
    }

    priority += tablet.Weight;
    if (tablet.RestartsOften()) {
        priority -= 5;
    }
    return priority;
}

}
}
