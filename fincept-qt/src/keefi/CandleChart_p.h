#pragma once

#include "keefi/MarketCandlesView.h"
#include "keefi/ChartTimeAxis.h"
#include "keefi/AutoChartGeometry.h"
#include <QElapsedTimer>
#include <QPixmap>
#include <QRegion>
#include <limits>

class QVariantAnimation;

namespace keefi::market_candles_detail {
struct DrawingPreviewItem {
    AutoChartCandidate candidate;
    bool visible = true;
};

class CandleChart final:public QWidget {
  public:
    explicit CandleChart(QWidget* p);
    void setObservation(const QList<MarketCandle>& rows,const std::optional<MarketCandle>& forming);
    // Public display only: never replaces OHLC, price bounds, studies or saved evidence.
    void setLiveTradePrice(const QString& price,qint64 tradeAtMs);
    void resetViewport();
    std::function<void()> viewportChanged;
    std::function<void()> olderRequested;
    std::function<void(ChartSyncViewport,bool)> userNavigated;
    std::function<void(qint64)> crosshairHovered;
    void returnToLive(bool user=true);
    bool navigateToTime(qint64 at,bool user=false);
    void setPriceLines(const QList<ChartPriceLine>& rows);
    void setCandleEvidenceNumber(int number);
    void setContext(qint64 interval,const QString& currency,const QString& instrument);
    void setStudies(const ChartStudySet& studies,const QSet<QString>& layers);
    void setCoinFlowStudies(const CoinFlowStudySet& studies);
    void setStudyColors(const QMap<QString,QColor>& colors);
    void setExtendedStudies(const ExtendedChartStudySet& studies);
    void setLevels(const ExtendedLevelSet& levels);
    int pricePlotHeight()const;
    struct StudyViewport {
        ChartTimeAxis axis;
        QPair<double,double> slotsRange;
        QRectF priceArea;
    };
    StudyViewport studyViewport() const;
    void setStockTimeline(bool stock,const ChartStudyTimeline& timeline);
    void setDrawingFrame(ChartDrawings* model,const ChartDrawingFrame& frame);
    void setDrawingTool(ChartDrawingTool tool);
    ChartDrawingTool drawingTool()const;
    bool drawingInProgress()const;
    QString selectedDrawing()const;
    void selectDrawing(const QString& id);
    void cancelDrawingGesture();
    void setAutoPreview(const QList<DrawingPreviewItem>& items,const ChartDrawingFrame& frame);
    void setAutoPreviewItemVisible(const QString& key,bool visible);
    void clearAutoPreview();
    void finishAutoPreview();
    bool autoPreviewActive()const;
    bool autoPreviewRunning()const;
    QList<DrawingPreviewItem> autoPreviewItems()const;
    std::function<void()> autoPreviewCancelled;
    std::function<void()> drawingUiChanged;
    std::function<void(const QString&)> drawingFeedback;
    ChartShareViewport shareViewport()const;
    ChartSyncViewport syncViewport()const;
    bool followsLive()const;
    int nominalZoomBars()const;
    void restoreLiveViewport(int zoomBars);
    void applySyncedViewport(const ChartSyncViewport& range,bool matchRange);
    void setSyncedCrosshair(qint64 utc);
    // Keyboard +/- keeps the viewed right edge, as before. Pointer-driven
    // wheel and pinch gestures use zoomAt() instead.
    void zoom(int delta);
    void resetZoom();
    void zoomAt(int delta,const QPointF& position);
    void setRightEdge(double right,const ChartTimeAxis& axis);
    void pan(double delta);
    // Main price drawing: "candles", "bars" (OHLC) or "line" (closes).
    void setChartStyle(const QString& style);
    QString chartStyle()const;
    // Vertical scale of the automatic price range: 1 is automatic, above 1 shows a wider range
    // (shorter candles), below 1 a narrower one. Set by dragging the price axis.
    static constexpr double MinPriceScale=.2,MaxPriceScale=8;
    double priceScale()const;
    void setPriceScale(double scale);
    void resetPriceScale();
    bool onPriceAxis(const QPointF& pos)const;
  protected:
    bool event(QEvent* event)override;
    void wheelEvent(QWheelEvent* event)override;
    void keyPressEvent(QKeyEvent* e)override;
    void keyReleaseEvent(QKeyEvent* e)override;
    void resizeEvent(QResizeEvent* event)override;
    void leaveEvent(QEvent*)override;
    void mousePressEvent(QMouseEvent* e)override;
    void mouseReleaseEvent(QMouseEvent* e)override;
    void mouseDoubleClickEvent(QMouseEvent* e)override;
    void contextMenuEvent(QContextMenuEvent* e)override;
    void mouseMoveEvent(QMouseEvent* e)override;
    // Dev 97: grid, candles, studies and drawings are kept in a pixmap. A pointer move
    // repaints only the crosshair, axis labels and the OHLC line over that pixmap, so
    // the crosshair no longer waits for a full redraw of the chart.
    void refresh();
    // A pointer move changes only the crosshair lines, both axes and the OHLC line. Repainting
    // just those strips keeps overlapping child widgets and most of a large window untouched.
    void hoverUpdate(const QRectF& area,double crosshairX);
    void paintEvent(QPaintEvent* event)override;
    // S paints the cached layer, D the pointer-dependent overlay. Statements that only set
    // painter state run in both passes so each pass sees the state of the single-pass paint.
    void paintChart(QPainter& p,const bool S,const bool D);
    // Top-left OHLC legend: the hovered candle, otherwise the latest one, in plain Korean labels.
    void paintLegend(QPainter& p,const QRectF& area,const std::optional<MarketCandle>& candle)const;
  private:
    QMap<QString,QColor> studyColors_;
    int axisGutter(double low,double high)const;
    struct Geometry {
        bool available=false,volumeLayer=false,flowLayer=false,cvdLayer=false,following=false;
        QList<MarketCandle> rows;ChartTimeAxis axis;QRectF area;QPair<double,double> bounds;
        double low=0,high=0,volumeHeight=0,cvdHeight=0;qint64 start=0,end=0;
    };
    struct StudyPriceBounds {
        qint64 fromMs,toMs;
        double low=std::numeric_limits<double>::infinity();
        double high=-std::numeric_limits<double>::infinity();
    };
    struct VisibleRows {
        QPair<double,double> bounds;
        QList<MarketCandle> rows;
    };
    struct GeometryState {
        QSize size;
        qreal devicePixelRatio=1;
        QPair<double,double> bounds;
        double priceScale=1;
        bool followsLive=false;
        bool operator==(const GeometryState&)const=default;
    };
    struct SimilarityTableLayout {
        bool visible=false,expanded=false,alwaysExpanded=false;
        QRectF box,collapsedRect,projectionRect;
        QList<QRectF> rowRects;
        QList<double> colWidths;
        QString title,disclaimer,summary;
        struct Row { QString rank,endText,corrText,afterText; int direction=0; };
        QList<Row> rows;
    };
    void includeVisiblePriceStudies(Geometry& plot)const;
    Geometry plotGeometry()const;
    Geometry calculatePlotGeometry()const;
    QRectF liveTradeRegion(const Geometry& plot,const QString& price)const;
    void paintLiveTrade(QPainter& painter,const Geometry& plot)const;
    SimilarityTableLayout similarityTableLayout(const Geometry& plot)const;
    void paintSimilarityOverlay(QPainter& painter,const Geometry& plot);
    static bool strongMagnetActive(Qt::KeyboardModifiers eventModifiers);
    void updateMagnetSnap(const Geometry& plot);
    void paintMagnetSnapRing(QPainter& painter)const;
    QPolygonF autoTrendPixels(const QList<ChartDrawingPoint>& anchors,const Geometry& plot)const;
    std::optional<QPointF> drawingPosition(const ChartDrawingPoint& point,const Geometry& plot)const;
    std::optional<ChartDrawingPoint> pointerAnchor(const QPointF& pos,const Geometry& plot,
        Qt::KeyboardModifiers modifiers=Qt::NoModifier,ChartDrawingSnap* snapOut=nullptr)const;
    QPair<QString,int> hitDrawing(const QPointF& pos,const Geometry& plot)const;
    // Whole-shape drag: whole completed bars horizontally (never between bars), price by pixel
    // resolution. A horizontal line keeps its anchor time and moves its price only.
    std::optional<QList<ChartDrawingPoint>> movedAnchors(const QPointF& pos,const Geometry& plot)const;
    void feedback(const QString& text);
    void paintDrawings(QPainter& painter,const Geometry& plot);
    void paintOffscreenFibTags(QPainter& painter,const Geometry& plot)const;
    void paintFibAxisLabels(QPainter& painter,const QRectF& area,qreal axisWidth,QList<qreal>& occupied);
    void paintAutoPreview(QPainter& painter,const Geometry& plot)const;
    ChartTimeAxis timeAxis()const;
    bool showsFollowing(const ChartTimeAxis& axis)const;
    double computedProjectionSlots(const ChartTimeAxis& axis)const;
    double viewSpan(const ChartTimeAxis& axis)const;
    double computedDrawingPadding(const ChartTimeAxis& axis)const;
    double latestRight(const ChartTimeAxis& axis)const;
    QPair<double,double> navigationBounds(const ChartTimeAxis& axis)const;
    QPair<double,double> slotBounds(const QList<MarketCandle>&,const ChartTimeAxis& axis)const;
    qint64 observedFlowEnd(const QList<MarketCandle>& rows)const;
    QString stockRowDescription(qint64 at)const;
    void clampOffset();
    QList<MarketCandle> visible()const;
    void publishViewport();
    void requestOlderIfNeeded(const ChartShareViewport& range);
    void publishUserNavigation(bool zoom);
    void zoomSynced(int delta,double fraction);
    void refreshSyncedMarker();
    QList<MarketCandle> rows_;std::optional<MarketCandle> forming_;QList<ChartPriceLine> lines_;int candleEvidenceNumber_=0;
    mutable std::optional<ChartTimeAxis> timeAxisCache_;
    mutable std::optional<VisibleRows> visibleRowsCache_;
    mutable std::optional<Geometry> geometryCache_;
    mutable GeometryState geometryState_;
    QString liveTradePrice_;qint64 liveTradeAtMs_=0;bool observationContextDirty_=true;
    qint64 intervalMs_=MarketCandles::IntervalMs;QString currency_;
    ChartStudySet studies_;ExtendedChartStudySet extended_;ExtendedLevelSet levels_;QSet<QString> layers_;
    mutable std::optional<StudyPriceBounds> priceStudyBounds_;
    CoinFlowStudySet coinFlow_;
    QPointer<ChartDrawings> drawingModel_;ChartDrawingFrame drawingFrame_;ChartDrawingTool tool_=ChartDrawingTool::Crosshair;
    QString selectedDrawing_,dragDrawing_,moveDrawing_;QList<ChartDrawingPoint> draft_,moveOriginal_,movePreview_;std::optional<ChartDrawingPoint> preview_;int dragAnchor_=-1;quint64 gestureGeneration_=0;
    struct FibAxisLabel {
        QString text;
        QColor color;
        double occupiedY=0;
    };
    QList<DrawingPreviewItem> autoPreview_;
    quint64 autoPreviewGeneration_=0;
    QList<ChartDrawingBar> autoPreviewBars_;
    double autoPreviewProgress_=1.0;
    QVariantAnimation* autoPreviewAnim_=nullptr;
    bool hasVisibleFib_=false;
    QList<FibAxisLabel> fibAxisLabels_;
    std::optional<FibAxisLabel> offscreenTopTag_,offscreenBottomTag_;
    QPointF movePressPos_,hoverPos_;bool hoverInside_=false;
    bool similarityTableHovered_=false;int similarityHoverRow_=-1;
    Qt::KeyboardModifiers lastModifiers_=Qt::NoModifier;
    mutable std::optional<bool> validSnapFrame_;
    std::optional<QPointF> magnetSnapPos_;QString magnetSnapField_,magnetSnapPrice_;bool magnetStrongActive_=false,magnetGeometryDirty_=true;
    static constexpr double DefaultMagnetRadiusPx=8.0,StrongMagnetRadiusPx=24.0;
    static constexpr qint64 StaticLayerMaxAgeMs=500; // Bounds any change that reached the widget without refresh().
    QPixmap staticLayer_;QList<quint64> staticKey_;bool staticDirty_=true;QElapsedTimer staticAge_;QRegion hoverRegion_;
    bool stock_=false;ChartStudyTimeline timeline_;
    int count_=60,hover_=-1,wheelAxis_=0;double offset_=0,projectionSlots_=0,drawingPadding_=0,edgePadding_=0,hoverY_=0,dragX_=0,zoomRemainder_=0;QPointF wheelTravel_;bool dragging_=false;
    QString style_=QStringLiteral("candles");double priceScale_=1,axisPressY_=0,axisPressScale_=1;bool axisDragging_=false,axisHover_=false;
    ChartSyncViewport syncRange_,syncPublishedViewport_;QRectF syncMarkerPlot_;ChartSyncMarker* syncMarker_=nullptr;qint64 syncCrosshairMs_=0;
};

} // namespace keefi::market_candles_detail
