#pragma once
#include "keefi/MarketCompare.h"
#include <QSet>

namespace keefi {
enum class ComparePeriodCoverage {
    Ready, Unavailable, InvalidWindow, InvalidSeries,
    MissingStart, MissingEnd, Gap, NotClosed
};

// Explicit completed-bar boundaries. The caller supplies its snapshot time;
// this display-only calculation has no clock, IO or FX selection of its own.
struct ComparePeriodWindow {
    qint64 startOpenMs = 0, endOpenMs = 0, asOfMs = 0;
};

struct ComparePeriodReturn {
    ComparePeriodCoverage coverage = ComparePeriodCoverage::InvalidWindow;
    std::optional<double> percent;
    qsizetype expectedBars = 0, presentBars = 0;
    int provisionalFxBars = 0, approximateFxBars = 0, previousFxBars = 0;
};

ComparePeriodReturn marketComparePeriodReturn(
    const CompareSeries&, const ComparePeriodWindow&,
    const QSet<qint64>& provisionalFxOpenMs = {});
} // namespace keefi
