#pragma once
#include "keefi/PortfolioComparison.h"

namespace keefi {
enum class PortfolioContributionClass { Coin, DomesticStock, UsStock, Cash, Unclassified };
struct PortfolioContributionRow {
    QString accountScope, componentId, asset;
    PortfolioContributionClass assetClass = PortfolioContributionClass::Unclassified;
    QString openingValue, closingValue, difference, reason;
};
struct PortfolioContributionGroup {
    PortfolioContributionClass assetClass = PortfolioContributionClass::Unclassified;
    QString openingValue, closingValue, difference;
    qsizetype rowCount = 0;
};
struct PortfolioContributions {
    bool available = false;
    PortfolioComparison comparison; // Original partial/exclusion/scope contract is preserved.
    QList<PortfolioContributionGroup> groups;
    QList<PortfolioContributionRow> rows;
    QString reason;
};

QString portfolioContributionClassLabel(PortfolioContributionClass assetClass);
// Splits observed valuation change, never investment PnL or underlying exposure.
// Supply the exact snapshots used to evaluate each endpoint. Rows join only by
// (accountScope, componentId, canonical asset). Missing or ambiguous source
// metadata stays in Unclassified, including futures equity and liabilities.
// Market/provider/kind are the classification evidence; symbols, currency and
// display names never infer a stock's country. Existing snapshots have no US
// stock metadata, so UsStock is reserved until an explicit source contract exists.
// The caller retains checkpoint selection, binding and freshness validation.
PortfolioContributions portfolioContributions(const AssetValuationResult& opening, const QString& openingPolicy,
    const QList<AssetSnapshot>& openingSnapshots, const AssetValuationResult& closing, const QString& closingPolicy,
    const QList<AssetSnapshot>& closingSnapshots);
} // namespace keefi
