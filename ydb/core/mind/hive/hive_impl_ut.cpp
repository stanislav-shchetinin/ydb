#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/unittest/tests_data.h>
#include <ydb/library/actors/helpers/selfping_actor.h>
#include <util/stream/null.h>
#include <util/datetime/cputimer.h>
#include <util/system/compiler.h>
#include "hive_impl.h"
#include "balancer.h"
#include "ut_common.h"

#ifdef NDEBUG
#define Ctest Cnull
#else
#define Ctest Cerr
#endif

using namespace NKikimr;
using namespace NHive;

using duration_nano_t = std::chrono::duration<ui64, std::nano>;
using duration_t = std::chrono::duration<double>;

duration_t GetBasePerformance() {
    duration_nano_t accm{};
    for (int i = 0; i < 1000000; ++i) {
        accm += duration_nano_t(NActors::MeasureTaskDurationNs());
    }
    return std::chrono::duration_cast<duration_t>(accm);
}

static double BASE_PERF = GetBasePerformance().count();

Y_UNIT_TEST_SUITE(THiveImplTest) {
    Y_UNIT_TEST(BootQueueSpeed) {
        TBootQueue bootQueue;
        static constexpr ui64 NUM_TABLETS = 1000000;

        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        THive hive(hiveStorage.Get(), TActorId());
        std::unordered_map<ui64, TLeaderTabletInfo> tablets;
        TProfileTimer timer;

        for (ui64 i = 0; i < NUM_TABLETS; ++i) {
            TLeaderTabletInfo& tablet = tablets.emplace(std::piecewise_construct, std::tuple<TTabletId>(i), std::tuple<TTabletId, THive&>(i, hive)).first->second;
            tablet.Weight = RandomNumber<double>();
            bootQueue.AddToBootQueue(tablet, 0UL);
        }

        double passed = timer.Get().SecondsFloat();
        Ctest << "Create = " << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
        UNIT_ASSERT(passed < 3 * BASE_PERF);
#else
        UNIT_ASSERT(passed < 1 * BASE_PERF);
#endif
#endif
        timer.Reset();

        double maxP = 100;
        std::vector<TBootQueue::TBootQueueRecord> records;
        records.reserve(NUM_TABLETS);
        unsigned i = 0;

        while (!bootQueue.Empty()) {
            auto record = bootQueue.PopFromBootQueue();
            UNIT_ASSERT(record.Priority <= maxP);
            maxP = record.Priority;
            UNIT_ASSERT(tablets.contains(record.TabletId));
            records.push_back(record);
            if (++i == NUM_TABLETS / 2) {
                // to test both modes
                bootQueue.IncludeWaitQueue();
            }
        }
        bootQueue.ExcludeWaitQueue();

        i = 0;
        for (auto& record : records) {
            if (++i % 3 == 0) {
                bootQueue.AddToBootQueue(record);
            } else {
                bootQueue.AddToWaitQueue(record);
            }
        }

        passed = timer.Get().SecondsFloat();
        Ctest << "Process = " << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
        UNIT_ASSERT(passed < 10 * BASE_PERF);
#else
        UNIT_ASSERT(passed < 2 * BASE_PERF);
#endif
#endif

        timer.Reset();

        bootQueue.IncludeWaitQueue();
        while (!bootQueue.Empty()) {
            bootQueue.PopFromBootQueue();
        }
        bootQueue.ExcludeWaitQueue();

        passed = timer.Get().SecondsFloat();
        Ctest << "Move = " << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
        UNIT_ASSERT(passed < 2 * BASE_PERF);
#else
        UNIT_ASSERT(passed < 0.1 * BASE_PERF);
#endif
#endif
    }

    Y_UNIT_TEST(BalancerSpeedAndDistribution) {
        static constexpr ui64 NUM_TABLETS = 1000000;
        static constexpr ui64 NUM_BUCKETS = 16;

        auto CheckSpeedAndDistribution = [](
            std::unordered_map<ui64, TLeaderTabletInfo>& allTablets,
            std::function<void(std::vector<TTabletInfo*>::iterator, std::vector<TTabletInfo*>::iterator, EResourceToBalance)> func,
            EResourceToBalance resource) -> void {

            std::vector<TTabletInfo*> tablets;
            for (auto& [id, tab] : allTablets) {
                tablets.emplace_back(&tab);
            }

            TProfileTimer timer;

            func(tablets.begin(), tablets.end(), resource);

            double passed = timer.Get().SecondsFloat();

            Ctest << "Time=" << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
            UNIT_ASSERT(passed < 1 * BASE_PERF);
#else
            UNIT_ASSERT(passed < 1 * BASE_PERF);
#endif
#endif
            std::vector<double> buckets(NUM_BUCKETS, 0);
            size_t revs = 0;
            double prev = 0;
            for (size_t n = 0; n < tablets.size(); ++n) {
                double weight = tablets[n]->GetWeight(resource);
                buckets[n / (NUM_TABLETS / NUM_BUCKETS)] += weight;
                if (n != 0 && weight >= prev) {
                    ++revs;
                }
                prev = weight;
            }

            Ctest << "Indirection=" << revs * 100 / NUM_TABLETS << "%" << Endl;

            Ctest << "Distribution=";
            for (double v : buckets) {
                Ctest << Sprintf("%.2f", v) << " ";
            }
            Ctest << Endl;

            /*char bars[] = " ▁▂▃▄▅▆▇█";
            double mx = *std::max_element(buckets.begin(), buckets.end());
            for (double v : buckets) {
                Ctest << bars[int(ceil(v / mx)) * 8];
            }
            Ctest << Endl;*/

            /*prev = NUM_TABLETS;
            for (double v : buckets) {
                UNIT_ASSERT(v < prev);
                prev = v;
            }*/

            std::sort(tablets.begin(), tablets.end());
            auto unique_end = std::unique(tablets.begin(), tablets.end());
            Ctest << "Duplicates=" << tablets.end() - unique_end << Endl;

            UNIT_ASSERT(tablets.end() - unique_end == 0);
        };

        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        THive hive(hiveStorage.Get(), TActorId());
        std::unordered_map<ui64, TLeaderTabletInfo> allTablets;

        for (ui64 i = 0; i < NUM_TABLETS; ++i) {
            TLeaderTabletInfo& tablet = allTablets.emplace(std::piecewise_construct, std::tuple<TTabletId>(i), std::tuple<TTabletId, THive&>(i, hive)).first->second;
            tablet.GetMutableResourceValues().Memory = RandomNumber<double>();
        }

        Ctest << "HIVE_TABLET_BALANCE_STRATEGY_HEAVIEST" << Endl;
        CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_HEAVIEST>, EResourceToBalance::Memory);

        //Ctest << "HIVE_TABLET_BALANCE_STRATEGY_OLD_WEIGHTED_RANDOM" << Endl;
        //CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_OLD_WEIGHTED_RANDOM>);

        Ctest << "HIVE_TABLET_BALANCE_STRATEGY_WEIGHTED_RANDOM" << Endl;
        CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_WEIGHTED_RANDOM>, EResourceToBalance::Memory);

        Ctest << "HIVE_TABLET_BALANCE_STRATEGY_RANDOM" << Endl;
        CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_RANDOM>, EResourceToBalance::Memory);
    }

    Y_UNIT_TEST(TestShortTabletTypes) {
        // This asserts we don't have different tablet types with same short name
        // In a world with constexpr maps this could have been a static_assert...
        UNIT_ASSERT_VALUES_EQUAL(TABLET_TYPE_SHORT_NAMES.size(), TABLET_TYPE_BY_SHORT_NAME.size());
    }

    Y_UNIT_TEST(TestStDev) {
        using TSingleResource = std::tuple<double>;

        TVector<TSingleResource> values(100, 50.0 / 1'000'000);
        values.front() = 51.0 / 1'000'000;

        double stDev1 = std::get<0>(GetStDev(values));

        std::swap(values.front(), values.back());

        double stDev2 = std::get<0>(GetStDev(values));

        double expectedStDev = sqrt(0.9703) / 1'000'000;

        UNIT_ASSERT_DOUBLES_EQUAL(expectedStDev, stDev1, 1e-6);
        UNIT_ASSERT_VALUES_EQUAL(stDev1, stDev2);
    }

    Y_UNIT_TEST(BootQueueConfigurePriorities) {
        auto hiveStorage = MakeIntrusive<TTabletStorageInfo>();
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());

        // Emulate initial BuildCurrentConfig call
        hive.UpdateConfig([](NKikimrConfig::THiveConfig&){});

        TFollowerGroup followerGroup(hive);

        // Part 1: Test default priorities with empty configuration
        {
            TLeaderTabletInfo ssTablet(1UL, hive);
            ssTablet.SetType(TTabletTypes::SchemeShard);
            hive.AddToBootQueue(&ssTablet);

            TLeaderTabletInfo dummyTablet(2UL, hive);
            dummyTablet.SetType(TTabletTypes::Dummy);
            hive.AddToBootQueue(&dummyTablet);

            TFollowerTabletInfo dummyFollowerTablet(dummyTablet, 3UL, followerGroup);
            hive.AddToBootQueue(&dummyFollowerTablet);

            auto& bootQueue = hive.GetBootQueue();
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 3);

            // Priorities should follow defaults
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 3.0); // SchemeShard default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 1.0); // Leader default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 0.0); // Follower default

            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 0);
        }

        // Part 2: Configure custom priorities for known and unknown types
        {
            constexpr double SS_BOOT_PRIORITY = 2.5;
            constexpr double DUMMY_BOOT_PRIORITY = 0.5;

            hive.UpdateConfig([&](NKikimrConfig::THiveConfig& config) {
                auto* ssItem = config.AddTabletTypeToBootPriority();
                ssItem->SetTabletType(TTabletTypes::SchemeShard);
                ssItem->SetPriority(SS_BOOT_PRIORITY);

                auto* dummyItem = config.AddTabletTypeToBootPriority();
                dummyItem->SetTabletType(TTabletTypes::Dummy);
                dummyItem->SetPriority(DUMMY_BOOT_PRIORITY);
            });

            TLeaderTabletInfo configuredSsTablet(4UL, hive);
            configuredSsTablet.SetType(TTabletTypes::SchemeShard);
            hive.AddToBootQueue(&configuredSsTablet);

            TLeaderTabletInfo configuredDummyTablet(5UL, hive);
            configuredDummyTablet.SetType(TTabletTypes::Dummy);
            hive.AddToBootQueue(&configuredDummyTablet);

            TFollowerTabletInfo configuredDummyFollowerTablet(configuredDummyTablet, 6UL, followerGroup);
            hive.AddToBootQueue(&configuredDummyFollowerTablet);

            auto& bootQueue = hive.GetBootQueue();
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 3);

            // Priorities should use configured values
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, SS_BOOT_PRIORITY); // Configured SchemeShard
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, DUMMY_BOOT_PRIORITY); // Configured Dummy
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 0.0); // Follower default (should not change)

            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 0);
        }

        // Part 3: Clear configuration and verify defaults again
        {
            hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
                config.ClearTabletTypeToBootPriority();
            });

            TLeaderTabletInfo ssTabletAfterClear(7UL, hive);
            ssTabletAfterClear.SetType(TTabletTypes::SchemeShard);
            hive.AddToBootQueue(&ssTabletAfterClear);

            TLeaderTabletInfo dummyTabletAfterClear(8UL, hive);
            dummyTabletAfterClear.SetType(TTabletTypes::Dummy);
            hive.AddToBootQueue(&dummyTabletAfterClear);

            TFollowerTabletInfo dummyFollowerTabletAfterClear(dummyTabletAfterClear, 9UL, followerGroup);
            hive.AddToBootQueue(&dummyFollowerTabletAfterClear);

            auto& bootQueue = hive.GetBootQueue();
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 3);

            // Priorities should revert to defaults
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 3.0); // SchemeShard default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 1.0); // Leader default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 0.0); // Follower default

            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 0);
        }
    }
}

Y_UNIT_TEST_SUITE(THiveBackupBootTest) {
    static constexpr TInstant T0 = TInstant::Seconds(1000);

    THolder<TTestHive> MakeHive(TIntrusivePtr<TTabletStorageInfo>& hiveStorage, bool pacingEnabled) {
        hiveStorage = MakeIntrusive<TTabletStorageInfo>();
        hiveStorage->TabletType = TTabletTypes::Hive;
        auto hive = MakeHolder<TTestHive>(hiveStorage.Get(), TActorId());
        hive->UpdateConfig([pacingEnabled](NKikimrConfig::THiveConfig& config) {
            config.SetBackupBootPacingEnabled(pacingEnabled);
        });
        return hive;
    }

    Y_UNIT_TEST(QueueRouting) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage;
        auto hive = MakeHive(hiveStorage, true);
        auto& bootQueue = hive->GetBootQueue();

        TLeaderTabletInfo userTablet(1UL, *hive);
        userTablet.SetType(TTabletTypes::DataShard);
        bootQueue.AddToBootQueue(userTablet, 0UL, T0);

        TLeaderTabletInfo backupTablet(2UL, *hive);
        backupTablet.SetType(TTabletTypes::DataShard);
        backupTablet.IsBackup = true;
        bootQueue.AddToBootQueue(backupTablet, 0UL, T0);

        // The backup tablet must not be visible to the main pass at all
        UNIT_ASSERT_VALUES_EQUAL(bootQueue.MainQueueSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(bootQueue.BackupQueueSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 2);
        UNIT_ASSERT(!bootQueue.Empty());

        UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().TabletId, 1UL);
        UNIT_ASSERT(bootQueue.MainQueueEmpty());
        UNIT_ASSERT(!bootQueue.Empty());

        UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBackupQueue().Record.TabletId, 2UL);
        UNIT_ASSERT(bootQueue.Empty());
    }

    Y_UNIT_TEST(FollowerUsesLeaderBackupClassification) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        auto& queue = hive->GetBootQueue();
        TLeaderTabletInfo leader(1UL, *hive);
        leader.IsBackup = true;
        TFollowerGroup group(*hive);
        TFollowerTabletInfo follower(leader, 1UL, group);
        queue.AddToBootQueue(follower, 0, T0);
        UNIT_ASSERT(queue.MainQueueEmpty());
        auto record = queue.PopFromBackupQueue();
        UNIT_ASSERT_VALUES_EQUAL(record.Record.TabletId, leader.Id);
        UNIT_ASSERT_VALUES_EQUAL(record.Record.FollowerId, follower.Id);
    }

    Y_UNIT_TEST(QueueRoutingWhenPacingDisabled) {
        // With pacing disabled the behaviour must be exactly as it was before
        TIntrusivePtr<TTabletStorageInfo> hiveStorage;
        auto hive = MakeHive(hiveStorage, false);
        auto& bootQueue = hive->GetBootQueue();

        TLeaderTabletInfo backupTablet(1UL, *hive);
        backupTablet.SetType(TTabletTypes::DataShard);
        backupTablet.IsBackup = true;
        bootQueue.AddToBootQueue(backupTablet, 0UL, T0);

        UNIT_ASSERT_VALUES_EQUAL(bootQueue.MainQueueSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(bootQueue.BackupQueueSize(), 0);
    }

    Y_UNIT_TEST(QueueIsFifo) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage;
        auto hive = MakeHive(hiveStorage, true);
        auto& bootQueue = hive->GetBootQueue();

        std::vector<THolder<TLeaderTabletInfo>> tablets;
        for (TTabletId id = 1; id <= 5; ++id) {
            auto& tablet = tablets.emplace_back(MakeHolder<TLeaderTabletInfo>(id, *hive));
            tablet->SetType(TTabletTypes::DataShard);
            tablet->IsBackup = true;
            // Weight would have changed the priority, it must not affect the order
            tablet->Weight = 5 - id;
            bootQueue.AddToBootQueue(*tablet, 0UL, T0 + TDuration::Seconds(id));
        }

        UNIT_ASSERT_VALUES_EQUAL(*bootQueue.GetOldestBackupEnqueueTime(), T0 + TDuration::Seconds(1));
        for (TTabletId id = 1; id <= 5; ++id) {
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBackupQueue().Record.TabletId, id);
        }
    }

    Y_UNIT_TEST(WaitQueueRetryIsBounded) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        auto& queue = hive->GetBootQueue();
        TLeaderTabletInfo tablet(1UL, *hive);
        tablet.IsBackup = true;
        for (ui64 i = 0; i < 3; ++i) {
            queue.AddToBootQueue(tablet, 0, T0 + TDuration::Seconds(i));
            queue.AddToBackupWaitQueue(queue.PopFromBackupQueue());
        }
        UNIT_ASSERT_VALUES_EQUAL(*queue.GetOldestBackupEnqueueTime(), T0);
        queue.IncludeBackupWaitQueue(1);
        UNIT_ASSERT_VALUES_EQUAL(queue.BackupWaitQueue.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(queue.BackupQueueSize(), 1);
        queue.PopFromBackupQueue();
        UNIT_ASSERT_VALUES_EQUAL(*queue.GetOldestBackupEnqueueTime(), T0 + TDuration::Seconds(1));
    }

    Y_UNIT_TEST(DeferredRecordDoesNotBlockReadyRecords) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        auto& queue = hive->GetBootQueue();
        TLeaderTabletInfo delayed(1UL, *hive);
        delayed.IsBackup = true;
        TLeaderTabletInfo ready(2UL, *hive);
        ready.IsBackup = true;
        queue.AddToBootQueue(delayed, 0, T0);
        queue.AddToBootQueue(ready, 0, T0 + TDuration::Seconds(1));
        queue.DeferBackup(queue.PopFromBackupQueue(), T0 + TDuration::Hours(1));
        UNIT_ASSERT_VALUES_EQUAL(queue.PopFromBackupQueue().Record.TabletId, 2);
        UNIT_ASSERT_VALUES_EQUAL(*queue.GetOldestBackupEnqueueTime(), T0);
        queue.PromoteBackupDeferred(T0 + TDuration::Minutes(1), 1);
        UNIT_ASSERT(queue.BackupQueueEmpty());
        queue.PromoteBackupDeferred(T0 + TDuration::Hours(1), 1);
        UNIT_ASSERT_VALUES_EQUAL(queue.PopFromBackupQueue().Record.TabletId, 1);
        UNIT_ASSERT(!queue.GetOldestBackupEnqueueTime());
    }

    Y_UNIT_TEST(OverdueBackupDoesNotSpinOrHideForegroundDeadline) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        auto& queue = hive->GetBootQueue();
        TLeaderTabletInfo tablet(1UL, *hive);
        tablet.IsBackup = true;
        queue.AddToBootQueue(tablet, 0, T0);
        auto record = queue.PopFromBackupQueue();
        queue.DeferBackup(record, T0);
        UNIT_ASSERT(!queue.GetNextDeferredWakeup(T0));
        queue.Defer(record.Record, T0 + TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(*queue.GetNextDeferredWakeup(T0), T0 + TDuration::Seconds(1));
        UNIT_ASSERT(!queue.GetNextDeferredWakeup(T0 + TDuration::Seconds(1)));
        UNIT_ASSERT(queue.HasReadyMainQueue(T0 + TDuration::Seconds(1)));
    }

    Y_UNIT_TEST(BlockedRetryRoundDoesNotRescanNewlyBlockedRecords) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, false);
        auto& queue = hive->GetBootQueue();
        TLeaderTabletInfo tablet(1UL, *hive);
        for (ui32 i = 0; i < 3; ++i) {
            queue.AddToBootQueue(tablet, 0, T0);
            queue.Block(queue.PopFromBootQueue());
        }
        UNIT_ASSERT(!queue.HasReadyMainQueue(T0));
        UNIT_ASSERT_VALUES_EQUAL(queue.Size(), 3);
        queue.IncludeBlockedQueue();
        for (ui32 i = 0; i < 3; ++i) {
            UNIT_ASSERT(queue.HasReadyMainQueue(T0));
            queue.PromoteBlocked(1);
            UNIT_ASSERT_VALUES_EQUAL(queue.MainQueueSize(), 1);
            queue.Block(queue.PopFromBootQueue());
        }
        UNIT_ASSERT(!queue.HasReadyMainQueue(T0));
        UNIT_ASSERT_VALUES_EQUAL(queue.Size(), 3);
        queue.PromoteBlocked(1);
        UNIT_ASSERT(queue.MainQueueEmpty());
        // A new state event permits a new round, without losing any record.
        queue.IncludeBlockedQueue();
        queue.PromoteBlocked(3);
        UNIT_ASSERT_VALUES_EQUAL(queue.MainQueueSize(), 3);
        UNIT_ASSERT(!queue.HasBlockedRecords());
    }

    Y_UNIT_TEST(DisablePreservesDeferredAndWaitState) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        auto& queue = hive->GetBootQueue();
        TLeaderTabletInfo tablet(1UL, *hive);
        tablet.IsBackup = true;
        queue.AddToBootQueue(tablet, 0, T0);
        queue.DeferBackup(queue.PopFromBackupQueue(), T0 + TDuration::Hours(1));
        queue.AddToBootQueue(tablet, 0, T0);
        queue.AddToBackupWaitQueue(queue.PopFromBackupQueue());
        queue.HandOverBackupQueues(1);
        UNIT_ASSERT_VALUES_EQUAL(queue.DeferredQueue.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(queue.BackupWaitQueue.size(), 1);
        UNIT_ASSERT(!queue.HasReadyMainQueue(T0));
        UNIT_ASSERT(queue.HasReadyMainQueue(T0 + TDuration::Hours(1)));
        queue.HandOverBackupQueues(1);
        UNIT_ASSERT_VALUES_EQUAL(queue.WaitQueue.size(), 1);
        UNIT_ASSERT(!queue.GetOldestBackupEnqueueTime());
    }

    Y_UNIT_TEST(ZeroPerNodeLimitFailsClosed) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        hive->UpdateConfig([](auto& config) {
            config.SetMaxBackupTabletsStartingPerNode(0);
        });
        hive->SetBackupBootTokens(10, T0);
        UNIT_ASSERT_VALUES_EQUAL(hive->TestGetBackupBootBudget(T0), 0);
        hive->UpdateConfig([](auto& config) {
            config.SetMaxBackupTabletsStartingPerNode(2);
        });
        UNIT_ASSERT_VALUES_EQUAL(hive->TestGetBackupBootBudget(T0), 10);
    }

    Y_UNIT_TEST(InflightLimitNeverExpires) {
        TIntrusivePtr<TTabletStorageInfo> storage;
        auto hive = MakeHive(storage, true);
        hive->UpdateConfig([](auto& config) {
            config.SetMaxBackupTabletsStarting(2);
        });
        hive->SetBackupBootTokens(10, T0);
        hive->SetTabletsStarting(2, 2);
        UNIT_ASSERT_VALUES_EQUAL(hive->TestGetBackupBootBudget(T0), 0);
        UNIT_ASSERT_VALUES_EQUAL(hive->TestGetBackupBootBudget(T0 + TDuration::Hours(1)), 0);
        hive->SetTabletsStarting(1, 1);
        UNIT_ASSERT_VALUES_EQUAL(hive->TestGetBackupBootBudget(T0 + TDuration::Hours(1)), 1);
    }
}

Y_UNIT_TEST_SUITE(THiveBackupPacerTest) {
    static constexpr TInstant T0 = TInstant::Seconds(1000);

    TBackupPacer::TSettings MakeSettings() {
        return {.Rate = 10, .Burst = 10, .WindowLimit = 16};
    }

    Y_UNIT_TEST(RateLimitsBudget) {
        auto settings = MakeSettings();
        TBackupPacer pacer;
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0, settings, 0), 1);
        pacer.Spend();
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0, settings, 0), 0);
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0 + TDuration::MilliSeconds(100), settings, 0), 1);
    }

    Y_UNIT_TEST(BurstCapsRefillAndConfigReduction) {
        auto settings = MakeSettings();
        TBackupPacer pacer;
        pacer.GetBudget(T0, settings, 0);
        auto now = T0 + TDuration::Hours(1);
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(now, settings, 0), 10);
        settings.Burst = 2;
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(now, settings, 0), 2);
    }

    Y_UNIT_TEST(WindowIsHardEvenAfterLongWait) {
        auto settings = MakeSettings();
        TBackupPacer pacer;
        pacer.GetBudget(T0, settings, 16);
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0 + TDuration::Hours(1), settings, 16), 0);
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0 + TDuration::Hours(2), settings, 17), 0);
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0 + TDuration::Hours(2), settings, 15), 1);
    }

    Y_UNIT_TEST(RestartDoesNotRestoreFullBurst) {
        auto settings = MakeSettings();
        TBackupPacer before;
        before.GetBudget(T0, settings, 0);
        UNIT_ASSERT_VALUES_EQUAL(before.GetBudget(T0 + TDuration::Hours(1), settings, 0), 10);
        TBackupPacer after;
        UNIT_ASSERT_VALUES_EQUAL(after.GetBudget(T0 + TDuration::Hours(1), settings, 0), 1);
    }

    Y_UNIT_TEST(BackwardClockDoesNotRefillTwice) {
        auto settings = MakeSettings();
        TBackupPacer pacer;
        pacer.GetBudget(T0, settings, 0);
        pacer.Spend();
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0 - TDuration::Seconds(1), settings, 0), 0);
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0, settings, 0), 0);
    }

    Y_UNIT_TEST(ZeroTimestampDoesNotReinitializeBucket) {
        auto settings = MakeSettings();
        TBackupPacer pacer;
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(TInstant::Zero(), settings, 0), 1);
        pacer.Spend();
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(TInstant::Zero(), settings, 0), 0);
    }

    Y_UNIT_TEST(HighRateWakeupsAreBatched) {
        auto settings = MakeSettings();
        settings.Rate = 1000;
        TBackupPacer pacer;
        pacer.GetBudget(T0, settings, 0);
        pacer.Spend();
        auto delay = pacer.GetTimeToNextToken(settings);
        UNIT_ASSERT(delay);
        UNIT_ASSERT_VALUES_EQUAL(*delay, TDuration::MilliSeconds(10));
        UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0 + *delay, settings, 0), 10);
        pacer.Spend(10);
        settings.Burst = 1;
        UNIT_ASSERT_VALUES_EQUAL(*pacer.GetTimeToNextToken(settings), TDuration::MilliSeconds(1));
    }

    Y_UNIT_TEST(TimeToNextTokenIsNeverZero) {
        auto settings = MakeSettings();
        TBackupPacer pacer;
        pacer.GetBudget(T0, settings, 0);
        UNIT_ASSERT(!pacer.GetTimeToNextToken(settings));
        pacer.Spend();
        UNIT_ASSERT_VALUES_EQUAL(*pacer.GetTimeToNextToken(settings), TDuration::MilliSeconds(100));
        pacer.Tokens = 1.0 - 1e-6;
        UNIT_ASSERT_VALUES_EQUAL(*pacer.GetTimeToNextToken(settings), TBackupPacer::MIN_WAKEUP);
    }

    Y_UNIT_TEST(InvalidConfigurationFailsClosed) {
        for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
            auto settings = MakeSettings();
            settings.Rate = invalid;
            TBackupPacer pacer;
            UNIT_ASSERT(!settings.IsValid());
            UNIT_ASSERT_VALUES_EQUAL(pacer.GetBudget(T0, settings, 0), 0);
            UNIT_ASSERT(!pacer.GetTimeToNextToken(settings));
        }
        auto settings = MakeSettings();
        settings.WindowLimit = 0;
        UNIT_ASSERT(!settings.IsValid());
        settings = MakeSettings();
        settings.Burst = 0;
        UNIT_ASSERT(!settings.IsValid());
    }
}

Y_UNIT_TEST_SUITE(THiveBackupDeleteAccountingTest) {
    Y_UNIT_TEST(SharedWindowAndIdempotentCompletion) {
        auto storage = MakeIntrusive<TTabletStorageInfo>();
        storage->TabletType = TTabletTypes::Hive;
        TTestHive hive(storage.Get(), TActorId());
        hive.UpdateConfig([](auto& config) {
            config.SetMaxDeleteTabletInProgress(3);
            config.SetMaxBackupDeleteInProgress(2);
        });
        UNIT_ASSERT(hive.RegisterDeleteInFlight(1, true));
        UNIT_ASSERT(hive.RegisterDeleteInFlight(2, true));
        UNIT_ASSERT(!hive.RegisterDeleteInFlight(3, true));
        UNIT_ASSERT(hive.RegisterDeleteInFlight(3, false));
        UNIT_ASSERT(!hive.RegisterDeleteInFlight(4, false));
        UNIT_ASSERT_VALUES_EQUAL(hive.GetDeleteInFlight(), 3);
        UNIT_ASSERT_VALUES_EQUAL(hive.GetBackupDeleteInFlight(), 2);

        UNIT_ASSERT(hive.CompleteDeleteInFlight(1));
        UNIT_ASSERT(!hive.CompleteDeleteInFlight(1));
        UNIT_ASSERT(!hive.CompleteDeleteInFlight(999));
        UNIT_ASSERT_VALUES_EQUAL(hive.GetDeleteInFlight(), 2);
        UNIT_ASSERT_VALUES_EQUAL(hive.GetBackupDeleteInFlight(), 1);
        UNIT_ASSERT(!hive.RegisterDeleteInFlight(2, false));
        UNIT_ASSERT(hive.RegisterDeleteInFlight(4, false));
        UNIT_ASSERT_VALUES_EQUAL(hive.GetDeleteInFlight(), 3);
        UNIT_ASSERT(hive.CompleteDeleteInFlight(2));
        UNIT_ASSERT(hive.CompleteDeleteInFlight(3));
        UNIT_ASSERT(hive.CompleteDeleteInFlight(4));
        UNIT_ASSERT_VALUES_EQUAL(hive.GetDeleteInFlight(), 0);
        UNIT_ASSERT_VALUES_EQUAL(hive.GetBackupDeleteInFlight(), 0);
    }
}

Y_UNIT_TEST_SUITE(TCutHistoryRestrictions) {

    Y_UNIT_TEST(BasicTest) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("DataShard,Coordinator");
            config.SetCutHistoryDenyList("GraphShard");
        });
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::GraphShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::Hive));
    }

    Y_UNIT_TEST(EmptyAllowList) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("");
            config.SetCutHistoryDenyList("GraphShard");
        });
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::GraphShard));
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::Hive));
    }

    Y_UNIT_TEST(EmptyDenyList) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("DataShard,Coordinator");
            config.SetCutHistoryDenyList("");
        });
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::GraphShard));
    }

    Y_UNIT_TEST(SameTabletInBothLists) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("DataShard,Coordinator");
            config.SetCutHistoryDenyList("SchemeShard,DataShard");
        });
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::SchemeShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::Hive));
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::Coordinator));
    }

    Y_UNIT_TEST(BothListsEmpty) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("");
            config.SetCutHistoryDenyList("");
        });
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
    }
}
