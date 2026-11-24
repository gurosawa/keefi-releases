#pragma once
#include "keefi/MarketCandles.h"
#include <QMap>
#include <optional>

namespace keefi {
struct ChartStudyTimeline;
struct ChartDrawingBar;

// Display coordinates only. Source timestamps remain unchanged in the rows.
// A compressed market closure has no unique inverse UTC coordinate: hit tests
// return observed rows only, never a fabricated candle in a closed/missing slot.
class ChartTimeAxis final {
  public:
    bool available = false;
    QString reason;
    static ChartTimeAxis utc(const QList<MarketCandle>&, qint64 intervalMs);
    static ChartTimeAxis stock(const QList<MarketCandle>&, qint64 intervalMs,
                              const ChartStudyTimeline&, qint64 observedTargetFormingAtMs = 0);
    std::optional<double> coordinateAt(qint64 originalUtcMs) const;
    std::optional<qint64> observedTimeAt(double slot, double toleranceSlots = .49) const;
    bool connects(qint64 earlierUtcMs, qint64 laterUtcMs) const;
    // Existing saved UTC anchors only; new anchors still require observed rows.
    std::optional<double> annotationCoordinateAt(qint64 originalUtcMs,
                                                const QList<ChartDrawingBar>& actualClosedBars) const;
  private:
    bool utc_ = false;
    qint64 originMs_ = 0, intervalMs_ = 0;
    QMap<qint64, double> coordinates_;
    QMap<qint64, qint64> previous_;
    QList<qint64> observed_;
};
}
