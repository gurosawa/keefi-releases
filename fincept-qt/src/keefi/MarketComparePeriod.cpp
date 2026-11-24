#include "keefi/MarketComparePeriod.h"
#include <cmath>
#include <limits>

namespace keefi {
ComparePeriodReturn marketComparePeriodReturn(
    const CompareSeries& series, const ComparePeriodWindow& window,
    const QSet<qint64>& provisionalFxOpenMs) {
    ComparePeriodReturn result;
    const auto fail = [&](ComparePeriodCoverage coverage) {
        result.coverage = coverage;
        return result;
    };
    const qint64 interval = series.intervalMs;
    if (interval <= 0) return fail(ComparePeriodCoverage::InvalidSeries);

    // Check bounds before subtracting, adding a close offset, or counting bars.
    const qint64 maximumTime = std::numeric_limits<qint64>::max();
    const qint64 closeOffset = interval - 1;
    if (window.startOpenMs < 0 || window.endOpenMs <= window.startOpenMs || window.asOfMs <= 0 ||
        window.startOpenMs % interval != 0 || window.endOpenMs % interval != 0 ||
        window.endOpenMs > maximumTime - closeOffset)
        return result;
    const qint64 intervals = (window.endOpenMs - window.startOpenMs) / interval;
    if (intervals >= static_cast<qint64>(std::numeric_limits<qsizetype>::max())) return result;
    result.expectedBars = static_cast<qsizetype>(intervals) + 1;
    if (!series.available || series.bars.isEmpty()) return fail(ComparePeriodCoverage::Unavailable);

    const bool hasFx = !series.fx.isEmpty();
    if (hasFx && series.fx.size() != series.bars.size()) return fail(ComparePeriodCoverage::InvalidSeries);

    // A malformed page is never sorted or repaired here. Gaps and unfinished
    // bars outside the requested window are valid and do not affect its return.
    qint64 previousOpen = -1;
    for (const auto& bar : series.bars) {
        if (bar.openMs < 0 || bar.openMs % interval != 0 || bar.openMs <= previousOpen ||
            bar.openMs > maximumTime - closeOffset || bar.closeMs != bar.openMs + closeOffset ||
            !std::isfinite(bar.close) || bar.close <= 0)
            return fail(ComparePeriodCoverage::InvalidSeries);
        previousOpen = bar.openMs;
    }

    const CompareBar* first = nullptr;
    const CompareBar* last = nullptr;
    bool gap = false;
    bool notClosed = false;
    qsizetype provisionalCount = 0, approximateCount = 0, previousCount = 0;
    for (qsizetype i = 0; i < series.bars.size(); ++i) {
        const auto& bar = series.bars[i];
        if (bar.openMs < window.startOpenMs || bar.openMs > window.endOpenMs) continue;
        ++result.presentBars;
        if (!first) first = &bar;
        if (last && bar.openMs - last->openMs != interval) gap = true;
        last = &bar;
        notClosed = notClosed || bar.closeMs >= window.asOfMs;
        if (hasFx) {
            // Source already selected and applied the FX posting. Read only
            // its prepared flags; do not price or reclassify that choice.
            provisionalCount += provisionalFxOpenMs.contains(bar.openMs) ? 1 : 0;
            approximateCount += series.fx[i].approximate ? 1 : 0;
            previousCount += series.fx[i].previous ? 1 : 0;
        }
    }
    const auto maximumFxCount = std::numeric_limits<int>::max();
    if (provisionalCount > maximumFxCount || approximateCount > maximumFxCount || previousCount > maximumFxCount)
        return fail(ComparePeriodCoverage::InvalidSeries);
    result.provisionalFxBars = static_cast<int>(provisionalCount);
    result.approximateFxBars = static_cast<int>(approximateCount);
    result.previousFxBars = static_cast<int>(previousCount);

    if (!first || first->openMs != window.startOpenMs) return fail(ComparePeriodCoverage::MissingStart);
    if (!last || last->openMs != window.endOpenMs) return fail(ComparePeriodCoverage::MissingEnd);
    if (gap || result.presentBars != result.expectedBars) return fail(ComparePeriodCoverage::Gap);
    if (notClosed) return fail(ComparePeriodCoverage::NotClosed);

    // Use the precise prepared double, never the rounded display string.
    const double ratio = last->close / first->close;
    const double percent = (ratio - 1.0) * 100.0;
    if (!std::isfinite(ratio) || !std::isfinite(percent)) return fail(ComparePeriodCoverage::InvalidSeries);
    result.percent = percent;
    result.coverage = ComparePeriodCoverage::Ready;
    return result;
}
} // namespace keefi
