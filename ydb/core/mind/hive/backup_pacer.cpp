#include "backup_pacer.h"

namespace NKikimr {
namespace NHive {

bool TBackupPacer::IsEscalated(TInstant now,
                               const TSettings& settings,
                               std::optional<TInstant> oldestEnqueueTime) const {
    return oldestEnqueueTime.has_value() && *oldestEnqueueTime + settings.MaxDelay <= now;
}

void TBackupPacer::Refill(TInstant now, double rate, double burst) {
    if (!TokensUpdated) {
        // An idle bucket is a full bucket
        Tokens = burst;
    } else if (now > TokensUpdated) {
        Tokens = std::min(burst, Tokens + rate * (now - TokensUpdated).SecondsFloat());
    }
    TokensUpdated = now;
}

ui64 TBackupPacer::GetBudget(TInstant now,
                             const TSettings& settings,
                             double loadFactor,
                             i64 ownInflight,
                             i64 userInflight,
                             std::optional<TInstant> oldestEnqueueTime) {
    bool escalated = IsEscalated(now, settings, oldestEnqueueTime);

    double rate = settings.Rate * loadFactor;
    if (escalated) {
        rate = std::max(rate, settings.MinRate);
    }
    Refill(now, rate, std::max(settings.Burst, 1.0));

    i64 windowBudget = static_cast<i64>(settings.WindowLimit * loadFactor)
        - ownInflight
        - static_cast<i64>(settings.UserWeight * std::max<i64>(0, userInflight));
    if (escalated) {
        windowBudget = std::max<i64>(windowBudget, 1);
    }
    if (windowBudget <= 0) {
        return 0;
    }
    return std::min<ui64>(static_cast<ui64>(Tokens + TOKEN_EPSILON), static_cast<ui64>(windowBudget));
}

void TBackupPacer::Spend(ui64 count) {
    Tokens = std::max(0.0, Tokens - static_cast<double>(count));
}

std::optional<TDuration> TBackupPacer::GetTimeToNextToken(const TSettings& settings, double loadFactor) const {
    if (Tokens + TOKEN_EPSILON >= 1) {
        return std::nullopt;
    }
    double rate = settings.Rate * loadFactor;
    if (rate <= 0) {
        return std::nullopt;
    }
    double seconds = (1.0 - Tokens) / rate;
    return std::max(MIN_WAKEUP, TDuration::MicroSeconds(static_cast<ui64>(seconds * 1000000)));
}

} // NHive
} // NKikimr
