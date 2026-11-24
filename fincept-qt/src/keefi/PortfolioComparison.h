#pragma once
#include "keefi/AssetValuation.h"

namespace keefi {
struct PortfolioComparison {
    bool available = false;
    bool complete = false; // Both endpoints fully valued; partial comparisons remain false.
    qint64 fromMs = 0, toMs = 0;
    qsizetype excludedRowCount = 0;
    QString currency, openingValue, closingValue, difference, label, reason;
};

// Observed valuation change only, including possible transfers, trades and fees.
// The caller verifies selected connections, credential bindings and the declared
// account scope before evaluating both original instants under the same policy.
// Original as-of and price-evidence qualification remain with the evaluator;
// AssetValuationResult alone cannot reveal an omitted expected account.
// This does not select a daily baseline, fetch prices or infer investment PnL.
PortfolioComparison comparePortfolioValues(const AssetValuationResult& opening, const QString& openingPolicyVersion,
    const AssetValuationResult& closing, const QString& closingPolicyVersion);
// Same checked account set and evaluation policy. Entire portfolios can be
// compared across trades when every non-excluded asset was valued at both ends.
PortfolioComparison compareCompletePortfolioTotals(const AssetValuationResult& opening, const QString& openingPolicyVersion,
    const AssetValuationResult& closing, const QString& closingPolicyVersion);
} // namespace keefi
