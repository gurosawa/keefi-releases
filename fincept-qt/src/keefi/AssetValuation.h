#pragma once
#include <QList>
#include <QSet>
#include <QStringList>

namespace keefi {
struct AssetEvidence {
    QString source, url, scope;
    qint64 sourceTimeMs = 0, receivedAtMs = 0; // 0 means not provided, never synthesized.
    bool complete = false;
    bool operator==(const AssetEvidence&) const = default;
};
enum class AssetComponentKind { Holding, Cash, Equity, Liability, BuyingPower, Withdrawable,
    PositionNotional, UnrealizedPnl, ProviderPnl };
struct AssetComponent {
    QString id, asset, quantity, costBasis, costCurrency;
    QString reportedValue, reportedValueCurrency, providerUnrealizedPnl;
    QString valuationGroup = "account"; // Equity replaces components only inside this group.
    AssetComponentKind kind = AssetComponentKind::Holding;
    AssetEvidence evidence;
    // Display metadata never replaces the canonical asset (Solana mint).
    QString displaySymbol, displayName, metadataIssue, priceIssue;
    AssetEvidence metadataEvidence;
    bool operator==(const AssetComponent&) const = default;
};
struct AssetSnapshot {
    QString id, accountScope, provider, environment, market;
    qint64 timeMs = 0;
    bool balanceComplete = false, reconciled = false;
    QString coverage;
    QList<AssetComponent> components;
    AssetEvidence evidence;
    QString connectionId; // Opaque local registration ID; never an account number or credential.
    QString connectionBinding; // Encrypted cache binding; never shown or exported.
    bool operator==(const AssetSnapshot&) const = default;
};
struct AssetQuote {
    QString baseAsset, quoteAsset, price;
    AssetEvidence evidence;
    bool operator==(const AssetQuote&) const = default;
};
enum class AssetEventKind { ExternalIn, ExternalOut, TransferIn, TransferOut, Fee, Funding,
    Fill, Dividend, CorporateAction, Unknown };
struct AssetEvent {
    QString id, accountScope, asset, quantity, officialId;
    QString transferReference, peerAccountScope; // Both official legs required to exclude a transfer.
    qint64 timeMs = 0;
    AssetEventKind kind = AssetEventKind::Unknown;
    AssetEvidence evidence;
    bool operator==(const AssetEvent&) const = default;
};
struct AssetOrderObservation {
    QString accountScope, provider, market, orderId, orderFamily = "ordinary";
    qint64 observedAtMs = 0;
    QString side, type, status, price, quantity, executed, quoteAmount, quantityUnit, currency;
    AssetEvidence evidence;
    bool operator==(const AssetOrderObservation&) const = default;
};
struct AssetValueRow {
    QString accountScope, componentId, asset, quantity, value, currency, unrealizedPnl, reason;
    QString providerUnrealizedPnl, providerPnlCurrency;
    bool included = false, excludedByUser = false;
    AssetEvidence priceEvidence; // Evidence actually used for this valuation, distinct from balance receipt.
    QList<AssetEvidence> conversionEvidence; // Original legs for an explicitly supported cross price.
    QString referenceValue; // Display only: never included in totals, cost/PnL or performance.
    bool operator==(const AssetValueRow&) const = default;
};
struct AssetValuationResult {
    QString currency, netAssets, knownSubtotal;
    qint64 timeMs = 0;
    bool complete = false, reconciled = false;
    QStringList accountScopes, issues;
    QList<AssetValueRow> rows;
    bool operator==(const AssetValuationResult&) const = default;
};
struct AssetFlowResult {
    struct Flow { QString eventId, amount; qint64 timeMs = 0; };
    QString currency, netExternal;
    QStringList accountScopes;
    qint64 fromMs = 0, toMs = 0;
    QList<Flow> external;
    QStringList internalEventIds, issues;
    bool complete = false;
};
struct AssetPerformanceCoverage {
    qint64 fromMs = 0, toMs = 0;
    QStringList accountScopes;
    bool externalFlowsComplete = false, balancesReconciled = false, hasGaps = true;
};
enum class AssetRateState { Available, InsufficientData, NoSolution, MultipleSolutions, UniquenessUnverified };
struct AssetRateResult {
    AssetRateState state = AssetRateState::InsufficientData;
    QString rate, reason;
    bool annualized = false;
    QString method;
};
struct AssetPerformanceResult {
    QString currency, openingNetAssets, closingNetAssets, externalNet, investmentPnl;
    bool complete = false;
    QStringList issues;
    AssetRateResult mwr;
};
struct AssetTwrPoint {
    qint64 timeMs = 0;
    QString beforeFlowValue, afterFlowValue;
    bool reconciled = false;
};
// A transient read gap keeps the last official evidence for at most 180 s.
// Price streaming and account polling never renew these original timestamps.
constexpr qint64 DisplayFreshnessMs = 180000;
constexpr qint64 ReferenceMaxAgeMs = 24LL * 60 * 60 * 1000;

// Exact signed sum with the 80-digit amount limit applied after cancellation.
// Empty means an invalid operand or a final sum beyond that limit; no rows is 0.
QString sumAssetAmounts(const QStringList& amounts);

class AssetValuation final {
  public:
    static AssetValuationResult value(const QList<AssetSnapshot>&, const QList<AssetQuote>&,
        const QString& currency, qint64 asOfMs, qint64 maxAgeMs = 60000,
        const QList<AssetQuote>& displayOnlyReferences = {}, const QSet<QString>& excludedAssets = {});
    static AssetFlowResult flows(const QList<AssetEvent>&, const QStringList& accountScopes,
        const QList<AssetQuote>&, const QString& currency, qint64 fromMs, qint64 toMs,
        bool coverageComplete, qint64 quoteMaxAgeMs = 60000);
    static AssetPerformanceResult performance(const AssetValuationResult& opening,
        const AssetValuationResult& closing, const AssetFlowResult&, const AssetPerformanceCoverage&);
    // Every external-flow instant must have an independently reconciled before/after valuation.
    static AssetRateResult twr(const QList<AssetTwrPoint>&, const AssetFlowResult&,
        const AssetPerformanceCoverage&);
    // Cash flows use investor signs: investments negative, proceeds positive.
    static AssetRateResult mwr(const QList<AssetFlowResult::Flow>&, bool complete);
};
} // namespace keefi
