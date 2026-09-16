#pragma once

#include "hive.h"

#include <optional>

namespace NKikimr {
namespace NHive {

// Static rate and concurrency limits for backup work. Queue age never bypasses
// either limit. The caller additionally enforces shared and per-node capacity.
struct TBackupPacer {
    struct TSettings {
        double Rate = 0;
        double Burst = 0;
        ui64 WindowLimit = 0;

        bool IsValid() const;
    };

    static constexpr double TOKEN_EPSILON = 1e-9;
    static constexpr TDuration MIN_WAKEUP = TDuration::MilliSeconds(1);
    static constexpr TDuration BATCH_PERIOD = TDuration::MilliSeconds(10);

    double Tokens = 0;
    TInstant TokensUpdated;
    bool Initialized = false;

    ui64 GetBudget(TInstant now, const TSettings& settings, ui64 ownInflight);
    void Spend(ui64 count = 1);
    std::optional<TDuration> GetTimeToNextToken(const TSettings& settings) const;

private:
    void Refill(TInstant now, double rate, double burst);
};

} // NHive
} // NKikimr
