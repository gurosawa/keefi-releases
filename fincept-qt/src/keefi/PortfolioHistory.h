#pragma once
#include "keefi/PortfolioCheckpoint.h"
#include "keefi/PortfolioComparison.h"
#include "keefi/PortfolioContribution.h"

namespace keefi {
struct PortfolioHistoryPoint {
    qint64 timeMs = 0;
    bool available = false, complete = false, connectToPrevious = false;
    QString value, reason;
};
struct PortfolioDayHistory {
    PortfolioCheckpoint baseline;
    PortfolioComparison comparison;
    PortfolioContributions contributions;
    QList<PortfolioHistoryPoint> points;
    // Exact evaluated closing inputs for provenance only; private owner-memory,
    // never persisted, exported or substituted with a later portfolio refresh.
    AssetValuationResult closingValuation;
    QList<AssetSnapshot> closingSnapshots;
    // Display-only private aliases from the current controller state. Never serialized.
    // Empty when the controller cannot identify a currently incomplete connection.
    QStringList blockedConnectionNames;
    // Last complete observation on the previous KST date. The total can be
    // compared across trades even when individual rows cannot be attributed.
    PortfolioContributions previousDay;
};
qint64 portfolioHistoryDayStart(qint64 nowMs);
// Selection is the complete active connection set, with shared registrations
// removed. Both endpoints use the current evaluator and policy; the caller
// recomputes currentValue with these exclusions and the same nowMs.
// Only today's records known by nowMs enter the result. No private state is kept.
PortfolioDayHistory portfolioDayHistory(const QList<AssetSnapshot>& history, const QList<AssetQuote>& quotes,
    const CheckpointSelection& selection, const AssetValuationResult& currentValue, qint64 nowMs);
} // namespace keefi
