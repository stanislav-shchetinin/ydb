#include "backup_pacer.h"

#include <cmath>

namespace NKikimr {
namespace NHive {

bool TBackupPacer::TSettings::IsValid() const {
    // Bound conversions to ui64 and timer durations as well as rejecting NaN/inf.
    return std::isfinite(Rate) && Rate >= 1e-6 && Rate <= 1e9
        && std::isfinite(Burst) && Burst >= 1 && Burst <= 1e9
        && WindowLimit > 0;
}

void TBackupPacer::Refill(TInstant now, double rate, double burst) {
    if (!Initialized) {
        // Recovery must not replenish a full burst on every Hive restart.
        Tokens = std::min(burst, 1.0);
        TokensUpdated = now;
        Initialized = true;
    } else if (now > TokensUpdated) {
        Tokens = std::min(burst, Tokens + rate * (now - TokensUpdated).SecondsFloat());
        TokensUpdated = now;
    } else {
        Tokens = std::min(burst, Tokens);
    }
}

ui64 TBackupPacer::GetBudget(TInstant now, const TSettings& settings, ui64 ownInflight) {
    if (!settings.IsValid()) {
        return 0;
    }
    Refill(now, settings.Rate, settings.Burst);
    if (ownInflight >= settings.WindowLimit) {
        return 0;
    }
    return std::min(static_cast<ui64>(Tokens + TOKEN_EPSILON), settings.WindowLimit - ownInflight);
}

void TBackupPacer::Spend(ui64 count) {
    Tokens = std::max(0.0, Tokens - static_cast<double>(count));
}

std::optional<TDuration> TBackupPacer::GetTimeToNextToken(const TSettings& settings) const {
    if (!settings.IsValid() || Tokens + TOKEN_EPSILON >= 1) {
        return std::nullopt;
    }
    // Coalesce high rates into batches without waiting longer than it takes to
    // fill the bucket. A small burst intentionally limits the possible batch.
    double batch = std::min(settings.Burst,
        std::max(1.0, settings.Rate * BATCH_PERIOD.SecondsFloat()));
    double seconds = (batch - Tokens) / settings.Rate;
    return std::max(MIN_WAKEUP,
        TDuration::MicroSeconds(static_cast<ui64>(std::ceil(seconds * 1000000))));
}

} // NHive
} // NKikimr
