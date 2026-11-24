#pragma once

#include "keefi/AssetListFilter.h"
#include "keefi/AssetValuation.h"
#include "keefi/PortfolioController.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace keefi {

// How one holding contributes to the displayed totals. NotCounted rows carry a
// meaning other than net assets (equity-covered balances, buying power,
// positions) and must never be shown as unvalued.
enum class PortfolioValueState { Fresh, Reference, Unvalued, NotCounted };

struct PortfolioHoldingView {
    QString connectionId, componentId, accountAlias, provider, market, asset, quantity, accountScope;
    QString value, referenceValue, reason;
    QString displaySymbol, displayName, metadataIssue, priceIssue;
    AssetEvidence metadataEvidence;
    PortfolioValueState state = PortfolioValueState::Unvalued;
    AssetPresence presence = AssetPresence::NeedsCheck;
    qint64 balanceTimeMs = 0;
    qint64 asOfTimeMs = 0; // Nonzero only for an explicitly dated saved presentation.
    AssetEvidence priceEvidence;
    QList<AssetEvidence> conversionEvidence;
    AssetComponentKind kind = AssetComponentKind::Holding;
    QString providerPnl, providerPnlCurrency, providerPnlSource;
    qint64 providerPnlTimeMs = 0;
    bool excludedByUser = false; // Explicit valuation scope; raw holding quantity remains visible.
    AssetEvidence balanceEvidence; // Exact quantity input; transient presentation only, never persisted.
    bool operator==(const PortfolioHoldingView&) const = default;
};

struct PortfolioGroupView {
    QString identity, asset, displaySymbol, displayName;
    QList<PortfolioHoldingView> holdings;
    AssetPresence presence = AssetPresence::Zero;
    QString freshTotal = "0"; // Signed exact decimal of Fresh holdings only.
    int unvalued = 0, references = 0, notCounted = 0;
    QStringList storages; // Account alias · market, in first-seen order.
    int excluded = 0;
    bool operator==(const PortfolioGroupView&) const = default;
};

struct PortfolioMissingView {
    QString connectionId, alias, market, stage;
    bool active = false;
    bool operator==(const PortfolioMissingView&) const = default;
};

struct PortfolioSummaryView {
    bool complete = false;
    QString netAssets, knownSubtotal;
    int unvaluedGroups = 0, referenceGroups = 0;
    QString referenceTotal = "0"; // Display only, never part of a total.
    QStringList missingAccounts, readingAccounts;
    int excludedGroups = 0;
};

struct PortfolioPresentation {
    QList<PortfolioGroupView> groups; // Valued holdings descending, unknowns, then empty balances.
    QList<PortfolioMissingView> missing;
    PortfolioSummaryView summary;
    qint64 asOfTimeMs = 0; // 0 is the current view, not a reconstructed saved view.
};
struct PortfolioDustSummary {
    QString subtotal, hidden="0", displayed;
    QStringList hiddenIdentities;
};
PortfolioDustSummary portfolioDust(const PortfolioPresentation&,const QString& threshold,bool hide);
// List scope is independent of the full-portfolio summary. Account IDs are local
// registration IDs, never account numbers. Empty keys mean all.
QString portfolioAssetClass(const PortfolioHoldingView& holding);
QString portfolioHoldingKey(const PortfolioHoldingView& holding);
QList<PortfolioGroupView> scopedPortfolioGroups(const QList<PortfolioGroupView>&,
    const QString& assetClass, const QString& connectionId);
QString portfolioAssetLabel(const QString& asset,const QString& symbol);
QString portfolioGroupLabel(const PortfolioGroupView&);
QString portfolioQuantity(const PortfolioGroupView&);
// Durable component kind and account market distinguish OKX contract counts;
// controller-only display metadata is absent from restored derivative caches.
QString portfolioHoldingQuantity(const PortfolioHoldingView&);
QString portfolioAllocationValue(const PortfolioGroupView&);
QString portfolioAllocationTotal(const QList<PortfolioGroupView>&);
// One decimal percentage, rounded with exact decimal comparisons. -1 is unknown.
int portfolioShareTenths(const QString& value, const QString& total);
QString portfolioShareText(const QString& value, const QString& total);
QString providerPnlText(const PortfolioHoldingView&);

// Pure: groups valuation rows by the existing identity rule (exchange ticker,
// domestic stock code, Solana mint) without changing any source amount.
PortfolioPresentation presentPortfolio(const QList<AssetSnapshot>& snapshots,
    const AssetValuationResult& valuation, const QList<PortfolioConnection>& connections);
// Reconstructs a dated saved view using unchanged evidence and the normal
// freshness window. Inputs must already pass the controller's credential
// binding check. No saved inputs or a future/invalid date returns an empty view.
PortfolioPresentation recordedPortfolio(const QList<AssetSnapshot>& snapshots,
    const QList<AssetQuote>& quotes, const QList<PortfolioConnection>& connections,
    qint64 nowMs, const QSet<QString>& excludedAssets = {});

// Adds thousands separators to the integer part only; never rounds.
QString groupedAmount(const QString& exactDecimal);
// KRW totals and valuations: round to whole won for display, without converting
// to floating point or changing the source value. Ties round away from zero.
QString krwAmount(const QString& exactDecimal);
// Market prices only: same exact value without trailing fractional zeros
// (the stream reports "22.10000000"; shown as "22.1").
QString priceAmount(const QString& exactDecimal);

} // namespace keefi
