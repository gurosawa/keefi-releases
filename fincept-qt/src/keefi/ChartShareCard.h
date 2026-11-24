#pragma once
#include "keefi/ChartDrawings.h"
#include "keefi/ChartStudies.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/StockChartTypes.h"
#include <QImage>
#include <QSet>
#include <QSize>
#include <optional>

namespace keefi {
enum class ChartShareSource { Upbit, BinanceSpot, Toss };
enum class ChartShareAvailability { Ready, Waiting, Unavailable, Stale, Partial };
struct ChartShareSourceInfo {
    ChartShareSource provider = ChartShareSource::Upbit;
    QString canonicalUrl;
    qint64 sourceTimeMs = 0, receivedAtMs = 0;
    bool delayed = false;
    StockAdjustment adjustment = StockAdjustment::Unknown;
};
struct ChartShareViewport { qint64 fromMs = 0, toMs = 0; };
struct ChartShareDrawing {
    ChartDrawingKind kind = ChartDrawingKind::Horizontal;
    QList<ChartDrawingPoint> anchors; // Explicit opt-in geometry only; no id/list/rule/profile text.
};
struct ChartShareSnapshot {
    WatchlistIdentity identity;
    QString currency;
    qint64 intervalMs = 0, asOfMs = 0;
    QByteArray displayTimeZoneId;
    QList<MarketCandle> closed; // Actual completed calculation prefix; never forming or a widget capture.
    std::optional<ChartStudyTimeline> stockTimeline;
    ChartStudySet studies; // Prefix calculator output; arbitrary reason/evidence labels are never painted.
    ChartShareSourceInfo source;
    ChartShareViewport viewport;
    qint64 calculatedThroughMs = 0; // Actual last completion, separate from UTC viewport endpoints.
    QSet<QString> visiblePublicLayers;
    QList<ChartShareDrawing> drawings;
    MovementScheme colors = MovementScheme::GreenRed;
    ChartShareAvailability availability = ChartShareAvailability::Waiting;
};
struct ChartShareOptions {
    QSize outputSize{1200,900};
    bool includePublicDrawings = false;
    bool maskPersonalValues = true; // False is invalid; no unmask pathway exists.
};
struct ChartShareRenderResult {
    bool available = false;
    QString reason;
    QImage image;
    QStringList publicCaptions; // Fixed labels and validated public facts only; never PNG metadata.
};
class ChartShareCard final {
  public:
    static constexpr int MaxClosedBars = 200;
    static constexpr int MaxDrawings = 100;
    static constexpr int MinWidth = 640, MinHeight = 480, MaxDimension = 4096;
    static ChartShareRenderResult render(const ChartShareSnapshot&,const ChartShareOptions& = {});
    static bool savePng(const ChartShareSnapshot&,const ChartShareOptions&,const QString& absoluteLocalPath,QString* error = nullptr);
};
}
