#include "keefi/PortfolioCheckpoint.h"
#include <QDateTime>
#include <QMap>
#include <QTimeZone>
#include <algorithm>

namespace keefi {
namespace {
// Snapshots the publisher reported later than the checkpoint, or that a clock
// skew places beyond it, never enter it.
bool knownBy(const AssetEvidence& e, qint64 timeMs) { return e.receivedAtMs <= timeMs && e.sourceTimeMs <= timeMs; }
} // namespace

PortfolioCheckpoint checkpointAt(const QList<AssetSnapshot>& history, const QList<AssetQuote>& quotes,
    const CheckpointSelection& selection, qint64 timeMs) {
    PortfolioCheckpoint result; result.timeMs = timeMs; result.policyVersion = ValuationPolicyVersion;
    const auto unavailable = [&](const QString& reason) { result.reason = reason; return result; };
    if (selection.connections.isEmpty()) return unavailable(QStringLiteral("비교할 연결이 선택되지 않았습니다."));
    QMap<QString, QString> bindings;
    for (const auto& c : selection.connections) {
        if (c.connectionId.isEmpty() || c.binding.isEmpty() || bindings.contains(c.connectionId))
            return unavailable(QStringLiteral("연결 선택이 올바르지 않습니다."));
        bindings.insert(c.connectionId, c.binding);
    }
    // Latest record per account scope of each selected connection, as known at timeMs.
    QMap<QString, AssetSnapshot> latest;
    for (const auto& s : history) {
        if (!bindings.contains(s.connectionId) || s.timeMs > timeMs || !knownBy(s.evidence, timeMs)) continue;
        const auto found = latest.constFind(s.accountScope);
        if (found == latest.cend() || found->timeMs < s.timeMs) latest.insert(s.accountScope, s);
    }
    QSet<QString> covered;
    QList<AssetSnapshot> used;
    QSet<QString> scopes;
    for (const auto& s : latest) {
        // A scope that stopped being read is outside this instant; the comparison
        // later refuses if the set of accounts differs between its two ends.
        if (timeMs - s.timeMs > DisplayFreshnessMs) continue;
        if (!s.balanceComplete) return unavailable(QStringLiteral("이 시각의 잔고 조회가 완료되지 않은 연결이 있습니다."));
        if (s.connectionBinding != bindings.value(s.connectionId))
            return unavailable(QStringLiteral("이 시각 이후 연결 정보가 바뀌어 같은 범위로 볼 수 없습니다."));
        covered.insert(s.connectionId); used.append(s); scopes.insert(s.accountScope);
    }
    if (covered.size() != bindings.size())
        return unavailable(QStringLiteral("이 시각에 함께 기록된 잔고가 없는 연결이 있습니다."));
    QList<AssetQuote> known;
    for (const auto& q : quotes)
        if (knownBy(q.evidence, timeMs) && (q.evidence.scope.startsWith("public:") || scopes.contains(q.evidence.scope))) known.append(q);
    result.valuation = AssetValuation::value(used, known, "KRW", timeMs, DisplayFreshnessMs, {}, selection.excludedAssets);
    result.usedSnapshots = std::move(used);
    result.available = true;
    return result;
}

PortfolioCheckpoint dailyBaseline(const QList<AssetSnapshot>& history, const QList<AssetQuote>& quotes,
    const CheckpointSelection& selection, qint64 nowMs) {
    const QTimeZone seoul("Asia/Seoul");
    const auto day = QDateTime::fromMSecsSinceEpoch(nowMs, seoul).date();
    const auto midnight = QDateTime(day, QTime(0, 0), seoul).toMSecsSinceEpoch();
    QSet<QString> selected;
    for (const auto& c : selection.connections) selected.insert(c.connectionId);
    QList<qint64> candidates;
    for (const auto& s : history)
        if (selected.contains(s.connectionId) && s.timeMs >= midnight && s.timeMs <= nowMs) candidates.append(s.timeMs);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const auto t : candidates) {
        const auto point = checkpointAt(history, quotes, selection, t);
        // An all-unvalued balance is not a numeric baseline. A partial valuation
        // with at least one valued row remains eligible and stays explicitly partial.
        if (point.available && point.valuation.issues.isEmpty() && (point.valuation.complete ||
            std::any_of(point.valuation.rows.cbegin(), point.valuation.rows.cend(),
                [](const AssetValueRow& row) { return row.included && !row.value.isEmpty(); }))) return point;
    }
    PortfolioCheckpoint none; none.timeMs = midnight; none.policyVersion = ValuationPolicyVersion;
    none.reason = QStringLiteral("오늘 0시 이후 모든 연결이 함께 평가 가능한 기록이 아직 없습니다.");
    return none;
}

bool jointHistoryDue(const QList<AssetSnapshot>& current, qint64 nowMs, qint64 lastJointMs) {
    if (current.isEmpty() || (lastJointMs > 0 && nowMs - lastJointMs < JointHistoryIntervalMs)) return false;
    // The same 2 s lead the price checks allow for a publisher clock ahead of this PC.
    return std::all_of(current.cbegin(), current.cend(), [&](const AssetSnapshot& s) {
        return s.balanceComplete && s.timeMs - nowMs <= 2000 && nowMs - s.timeMs <= DisplayFreshnessMs;
    });
}
} // namespace keefi
