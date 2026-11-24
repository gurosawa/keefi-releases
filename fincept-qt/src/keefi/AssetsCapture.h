#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/TradingTypes.h"
#include "keefi/ProviderReconciliation.h"
#include <optional>
namespace keefi {
struct AssetCapture {
    AssetSnapshot snapshot;
    QList<AssetQuote> quotes;
    QList<AssetOrderObservation> orders;
    std::optional<ProviderTotalObservation> providerTotal; // Transient; never encoded in AssetSnapshot/ledger.
};
AssetCapture captureExchangeAssets(const AccountRecord&, const QString& scope, const MarketSnapshot&,
    qint64 balanceReceivedMs, qint64 historyReceivedMs, bool complete, bool reconciled,
    const QString& coverage, const QList<OrderRecord>& history);
AssetCapture captureBrokerAssets(const AccountRecord&, const QString& scope, const QJsonObject& holdings,
    const QJsonObject& cash, const QJsonObject& buyingPower, const AssetEvidence& holdingsEvidence,
    const AssetEvidence& cashEvidence, bool reconciled, const QString& coverage,
    const QList<OrderRecord>& history, const AssetEvidence& historyEvidence);
}
