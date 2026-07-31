#pragma once

#include "hive.h"

#include <optional>

namespace NKikimr {
namespace NHive {

// Rate limiter for background work that must not compete with user requests - booting or deleting
// tablets of backup tables. Three gates, all of which have to let the operation through:
//   * rate     - a token bucket, so that the work is spread over time instead of being a burst
//   * window   - a limit on own operations in flight, shrinking as user operations appear
//   * load     - a multiplier on both of the above, coming from the observed cluster load
// Plus an escape hatch: an operation that has been waiting longer than MaxDelay bypasses the window
// and the load gate, so that a permanently busy cluster still makes progress instead of stalling
// the DDL that is waiting for these tablets.
struct TBackupPacer {
    struct TSettings {
        double Rate = 0;          // operations per second at zero load
        double Burst = 0;         // max operations that can accumulate while idle
        i64 WindowLimit = 0;      // max own operations in flight
        double UserWeight = 0;    // how many own slots one user operation costs
        TDuration MaxDelay;       // after waiting this long, window and load gates are bypassed
        double MinRate = 0;       // rate used once the gates are bypassed
    };

    // A token deficit this small means sub-nanosecond timing and has no physical meaning. It has to
    // be tolerated because TDuration::SecondsFloat() multiplies by (1 / 1000000.0), so 100ms comes
    // out as 0.09999999999999999 and a full token at 10 ops/sec as 0.9999999999999999. Without the
    // tolerance the budget would be zero exactly when a token is due, and the wake-up computed for
    // "time until the next token" would be ~0, spinning until the error accumulates away.
    static constexpr double TOKEN_EPSILON = 1e-9;
    // Never ask to be woken up sooner than this, so that rounding cannot turn into a spin
    static constexpr TDuration MIN_WAKEUP = TDuration::MilliSeconds(1);

    double Tokens = 0;
    TInstant TokensUpdated;

    // How many operations may be started in this pass. Refills the bucket as a side effect, so it
    // has to be called once per pass.
    ui64 GetBudget(TInstant now,
                   const TSettings& settings,
                   double loadFactor,
                   i64 ownInflight,
                   i64 userInflight,
                   std::optional<TInstant> oldestEnqueueTime);

    // Called per attempted operation, not per successful one, so that a pass stays self-limiting
    // under any failure mode
    void Spend(ui64 count = 1);

    // How long to wait before there is a token to spend, or nullopt when there already is one.
    // Must be called after GetBudget, which is what refills the bucket.
    std::optional<TDuration> GetTimeToNextToken(const TSettings& settings, double loadFactor) const;

    bool IsEscalated(TInstant now, const TSettings& settings, std::optional<TInstant> oldestEnqueueTime) const;

private:
    void Refill(TInstant now, double rate, double burst);
};

} // NHive
} // NKikimr
