#include "keefi/PortfolioHistory.h"
#include <QDateTime>
#include <QMap>
#include <QTimeZone>
#include <algorithm>

namespace keefi {
namespace {
QString unusableValue(const AssetValuationResult& value) {
    if (!value.issues.isEmpty() || value.accountScopes.isEmpty())
        return QStringLiteral("계좌 조회 범위 또는 평가 입력의 문제가 있어 표시할 수 없습니다.");
    if (value.knownSubtotal.isEmpty() || (!value.complete && std::none_of(value.rows.cbegin(), value.rows.cend(),
        [](const AssetValueRow& row) { return row.included && !row.value.isEmpty(); })))
        return QStringLiteral("이 시각에 평가 가능한 자산이 없습니다.");
    return {};
}
} // namespace
qint64 portfolioHistoryDayStart(qint64 nowMs) {
    const QTimeZone seoul("Asia/Seoul");
    return QDateTime(QDateTime::fromMSecsSinceEpoch(nowMs, seoul).date(), QTime(0, 0), seoul).toMSecsSinceEpoch();
}
PortfolioDayHistory portfolioDayHistory(const QList<AssetSnapshot>& history, const QList<AssetQuote>& quotes,
    const CheckpointSelection& selection, const AssetValuationResult& currentValue, qint64 nowMs) {
    PortfolioDayHistory result;
    const auto unavailable = [&](const QString& reason) {
        result.baseline.reason = result.comparison.reason = reason;
        return result;
    };
    if (nowMs <= 0 || currentValue.timeMs != nowMs)
        return unavailable(QStringLiteral("현재 평가 시각이 요청한 시각과 같지 않아 비교할 수 없습니다."));
    result.closingValuation = currentValue; // Exact existing endpoint, never a later refresh.
    const auto start = portfolioHistoryDayStart(nowMs);
    QList<AssetSnapshot> today;
    QList<AssetQuote> knownQuotes;
    QList<qint64> times;
    QMap<QString, QString> selected;
    QMap<QString, qint64> firstSeen;
    for (const auto& c : selection.connections) {
        if (c.connectionId.isEmpty() || c.binding.isEmpty() || selected.contains(c.connectionId))
            return unavailable(QStringLiteral("연결 선택이 올바르지 않습니다."));
        selected.insert(c.connectionId, c.binding);
    }
    if (selected.isEmpty()) return unavailable(QStringLiteral("비교할 연결이 선택되지 않았습니다."));
    for (const auto& s : history) {
        if (s.timeMs < start || s.timeMs > nowMs || s.evidence.sourceTimeMs > nowMs || s.evidence.receivedAtMs > nowMs) continue;
        // This API receives the full active set. A stored, now absent ID or an
        // older credential binding is evidence of changed coverage, not a reason
        // to start a new daily baseline silently.
        if (!selected.contains(s.connectionId) || selected.value(s.connectionId) != s.connectionBinding)
            return unavailable(QStringLiteral("오늘 기록 이후 연결 또는 인증정보가 바뀌어 같은 범위로 비교할 수 없습니다."));
        if (!firstSeen.contains(s.connectionId) || s.timeMs < firstSeen.value(s.connectionId))
            firstSeen.insert(s.connectionId, s.timeMs);
        today.append(s); times.append(s.timeMs);
    }
    if (!times.isEmpty()) {
        const auto firstTime = *std::min_element(times.cbegin(), times.cend());
        // Joint captures can contain asynchronous responses within 180 seconds.
        // A connection first appearing after that window may have been added or
        // may belong to legacy asynchronous history. Neither proves an unchanged
        // daily scope, so do not silently rebase to the later complete subset.
        for (const auto time : firstSeen)
            if (time - firstTime > DisplayFreshnessMs)
                return unavailable(QStringLiteral("오늘 연결 범위가 같은지 확인할 수 없어 비교할 수 없습니다."));
    }
    for (const auto& quote : quotes) {
        const auto& e = quote.evidence;
        if (e.receivedAtMs >= start && e.receivedAtMs <= nowMs && e.sourceTimeMs <= nowMs) knownQuotes.append(quote);
    }
    result.baseline = dailyBaseline(today, knownQuotes, selection, nowMs);
    if (result.baseline.available)
        result.comparison = comparePortfolioValues(result.baseline.valuation, result.baseline.policyVersion,
                                                   currentValue, ValuationPolicyVersion);
    else result.comparison.reason = result.baseline.reason;

    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    AssetValuationResult previousValue;
    bool previousAvailable = false;
    qint64 previousTime = 0;
    const auto append = [&](qint64 time, const AssetValuationResult& value, QString reason) {
        PortfolioHistoryPoint point; point.timeMs = time;
        if (reason.isEmpty()) reason = unusableValue(value);
        point.available = reason.isEmpty(); point.reason = reason;
        if (point.available) {
            point.value = value.knownSubtotal; point.complete = value.complete;
            if (previousAvailable) {
                if (time - previousTime > JointHistoryIntervalMs + DisplayFreshnessMs)
                    point.reason = QStringLiteral("이 구간에 함께 기록된 자료가 없어 선을 잇지 않습니다.");
                else {
                    const auto compared = comparePortfolioValues(previousValue, ValuationPolicyVersion, value, ValuationPolicyVersion);
                    point.connectToPrevious = compared.available; point.reason = compared.reason;
                }
            }
        }
        result.points.append(point);
        previousAvailable = point.available; previousTime = time;
        previousValue = point.available ? value : AssetValuationResult{};
    };
    for (const auto time : times) {
        if (time == nowMs) continue; // Current observation is appended exactly once below.
        const auto checkpoint = checkpointAt(today, knownQuotes, selection, time);
        append(time, checkpoint.valuation, checkpoint.reason);
    }
    append(nowMs, currentValue, {});
    return result;
}
} // namespace keefi
