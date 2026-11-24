#pragma once
#include "keefi/AssetValuation.h"

namespace keefi {
// Identifies the valuation rules both ends of a comparison are recomputed with.
// Bump it whenever AssetValuation or VALUATION-POLICY changes what a stored
// snapshot and quote set evaluate to; never relabel an older result with it.
inline const QString ValuationPolicyVersion = QStringLiteral("keefi-valuation-v2");
constexpr qint64 JointHistoryIntervalMs = 5 * 60000;

struct CheckpointConnection {
    QString connectionId, binding; // Binding as recorded on snapshots; never shown or exported.
};
struct CheckpointSelection {
    QList<CheckpointConnection> connections;
    QSet<QString> excludedAssets;
};
struct PortfolioCheckpoint {
    bool available = false;
    qint64 timeMs = 0;
    QString policyVersion, reason;
    AssetValuationResult valuation;
    QList<AssetSnapshot> usedSnapshots; // Exact filtered inputs; private, owner-memory only.
};

// Every selected connection needs a complete snapshot taken at or before timeMs,
// no older than DisplayFreshnessMs, under the same binding. Nothing received
// after timeMs is used. The result still carries the evaluator's own issues.
PortfolioCheckpoint checkpointAt(const QList<AssetSnapshot>& history, const QList<AssetQuote>& quotes,
    const CheckpointSelection& selection, qint64 timeMs);
// First valid checkpoint at or after 00:00 KST of nowMs's KST date and not after nowMs.
// Without one the result says so; an earlier day's value is never moved to midnight.
PortfolioCheckpoint dailyBaseline(const QList<AssetSnapshot>& history, const QList<AssetQuote>& quotes,
    const CheckpointSelection& selection, qint64 nowMs);
// History is recorded for all live accounts together, only when every one is
// complete and within DisplayFreshnessMs of nowMs, at most once per interval.
bool jointHistoryDue(const QList<AssetSnapshot>& current, qint64 nowMs, qint64 lastJointMs);
} // namespace keefi
