#include "keefi/ChartTimeAxis.h"
#include "keefi/StockChartTypes.h"
#include "keefi/ChartDrawings.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace keefi {
ChartTimeAxis ChartTimeAxis::utc(const QList<MarketCandle>& rows, qint64 interval) {
    ChartTimeAxis axis;
    if (rows.isEmpty() || interval <= 0) { axis.reason = QStringLiteral("좌표에 사용할 완료 봉 없음"); return axis; }
    axis.utc_ = true; axis.originMs_ = rows.first().startsAtMs; axis.intervalMs_ = interval;
    qint64 previous = 0;
    for (const auto& row : rows) {
        if (row.startsAtMs <= 0 || row.startsAtMs <= previous) { axis.reason = QStringLiteral("봉 시각 순서 확인 불가"); return axis; }
        axis.observed_.append(row.startsAtMs);
        if (previous && row.startsAtMs - previous == interval) axis.previous_.insert(row.startsAtMs, previous);
        previous = row.startsAtMs;
    }
    axis.available = true; return axis;
}
ChartTimeAxis ChartTimeAxis::stock(const QList<MarketCandle>& rows, qint64 interval, const ChartStudyTimeline& timeline, qint64 formingAt) {
    ChartTimeAxis axis;
    const auto invalid = [&] { ChartTimeAxis rejected; rejected.reason = QStringLiteral("주식 거래 시각 좌표 확인 불가"); return rejected; };
    if (interval <= 0 || rows.size() != timeline.rows.size() || (rows.isEmpty() && formingAt <= 0)) return invalid();
    if (rows.isEmpty()) {
        axis.intervalMs_ = interval; axis.originMs_ = formingAt;
        axis.coordinates_.insert(formingAt, 0); axis.observed_.append(formingAt); axis.available = true; return axis;
    }
    axis.intervalMs_ = interval; axis.originMs_ = rows.first().startsAtMs;
    qint64 previous = 0; double slot = 0;
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const auto at = rows[i].startsAtMs; const auto& context = timeline.rows[i];
        if (at <= 0 || at <= previous || context.startsAtMs != at || (context.completesAtMs != 0 && context.completesAtMs <= at) ||
            (context.missingSlotsBefore && *context.missingSlotsBefore < 0)) return invalid();
        if (i) {
            double missing = context.missingSlotsBefore ? *context.missingSlotsBefore :
                (!context.connectsPrevious && at - previous > interval ? 1 : 0);
            slot += 1 + missing;
            if (context.connectsPrevious && context.eligible && timeline.rows[i - 1].eligible)
                axis.previous_.insert(at, previous);
        }
        axis.coordinates_.insert(at, slot); axis.observed_.append(at); previous = at;
    }
    if (timeline.projectionCovered) {
        if (timeline.nextStartsAtMs.size() != 20) return invalid();
        for (const auto at : timeline.nextStartsAtMs) {
            if (at <= previous) return invalid();
            axis.coordinates_.insert(at, ++slot); previous = at;
        }
    }
    if (formingAt > 0) {
        const auto last = rows.last().startsAtMs;
        if (formingAt <= last) return invalid();
        if (timeline.projectionCovered && !axis.coordinates_.contains(formingAt)) return invalid();
        if (!axis.coordinates_.contains(formingAt))
            axis.coordinates_.insert(formingAt, axis.coordinates_.value(last) + (formingAt - last > interval ? 2 : 1));
        axis.observed_.append(formingAt); // Actual target-forming observation, never an inferred future candle.
    }
    axis.available = true; return axis;
}
std::optional<double> ChartTimeAxis::coordinateAt(qint64 at) const {
    if (!available || at <= 0) return {};
    if (utc_) return static_cast<double>((static_cast<long double>(at) - originMs_) / intervalMs_);
    const auto found = coordinates_.constFind(at);
    return found == coordinates_.cend() ? std::optional<double>{} : std::optional<double>{found.value()};
}
std::optional<qint64> ChartTimeAxis::observedTimeAt(double slot, double tolerance) const {
    if (!available || !std::isfinite(slot) || !std::isfinite(tolerance) || tolerance < 0 || tolerance > .5) return {};
    std::optional<qint64> closest; double distance = std::numeric_limits<double>::infinity();
    const auto beforeSlot = [this](qint64 at,double target) { return coordinateAt(at).value_or(std::numeric_limits<double>::infinity()) < target; };
    const auto after = std::lower_bound(observed_.cbegin(), observed_.cend(), slot, beforeSlot);
    const auto consider = [&](qint64 at) {
        const auto coordinate = coordinateAt(at); if (!coordinate) return;
        const auto gap = std::abs(*coordinate - slot);
        if (gap <= tolerance && gap < distance) { closest = at; distance = gap; }
    };
    // Check the earlier observation first, preserving the original tie rule. The
    // second bound also preserves the earliest UTC when extreme values round to
    // the same double coordinate. Calendar-only projection slots are not observed_.
    if (after != observed_.cbegin()) {
        const auto previous = *std::prev(after);
        const auto coordinate = coordinateAt(previous);
        if (coordinate) consider(*std::lower_bound(observed_.cbegin(), after, *coordinate, beforeSlot));
    }
    if (after != observed_.cend()) consider(*after);
    return closest;
}
bool ChartTimeAxis::connects(qint64 earlier, qint64 later) const {
    const auto found = previous_.constFind(later);
    return available && found != previous_.cend() && found.value() == earlier;
}
std::optional<double> ChartTimeAxis::annotationCoordinateAt(qint64 at,const QList<ChartDrawingBar>& closed) const {
    if(!available||at<=0||closed.isEmpty()||closed.size()>ChartDrawings::MaxFrameBars)return {};
    std::optional<double> result;
    for(const auto& bar:closed){
        const auto start=bar.candle.startsAtMs;
        if(!bar.sourceCompleted||start<=0||bar.completesAtMs<=start||at<start||at>=bar.completesAtMs)continue;
        const auto slot=coordinateAt(start);
        if(!slot||observedTimeAt(*slot,0)!=std::optional<qint64>(start))continue;
        if(result)return {}; // Overlapping source rows do not identify one observed bar.
        result=*slot+static_cast<double>((static_cast<long double>(at)-start)/(bar.completesAtMs-start));
    }
    return result;
}
}
