#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/TradingTypes.h"

namespace keefi {
enum class ProviderTotalAvailability { Available, Unavailable, Unsupported };
enum class ProviderReconciliationState { Exact, WithinTolerance, Mismatch, Unavailable, Unsupported };

// Transient official observation, never part of the encrypted snapshot schema.
struct ProviderTotalObservation {
    QString connectionId, accountScope, snapshotId, provider, environment, market;
    QString metric, amount, currency, coverage, reason;
    ProviderTotalAvailability availability = ProviderTotalAvailability::Unsupported;
    AssetEvidence evidence, configurationEvidence;
    bool operator==(const ProviderTotalObservation&) const = default;
};

struct ProviderReconciliationResult {
    ProviderTotalObservation official;
    ProviderReconciliationState state = ProviderReconciliationState::Unavailable;
    QString keefiAmount, difference, absoluteDifference, relativeTolerance;
    QString krwDifference, reason;
    AssetEvidence fxEvidence;
};

ProviderTotalObservation unsupportedProviderTotal(const AccountRecord&);
ProviderTotalObservation captureBinanceUsdmProviderTotal(const AssetSnapshot&, const QJsonObject& account,
    const QJsonObject& configuration, const AssetEvidence& configurationEvidence);
ProviderReconciliationResult reconcileProviderTotal(const ProviderTotalObservation&, const AssetSnapshot*,
    const QList<AssetQuote>&, qint64 asOfMs, qint64 maxAgeMs = DisplayFreshnessMs);
QString providerReconciliationLabel(ProviderReconciliationState);
} // namespace keefi
