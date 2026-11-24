#pragma once
#include "keefi/MarketCandles.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QMap>
#include <QObject>
#include <memory>
#include <optional>

namespace keefi {
enum class ChartDrawingKind { Trend, Horizontal, Measure, FibRetracement, FibExtension, Zone };
enum class ChartDrawingTool { Crosshair, Trend, Horizontal, Measure };
struct ChartDrawingPoint {
    qint64 atMs=0;
    QString price;
    bool operator==(const ChartDrawingPoint&) const=default;
};
struct ChartDrawingAlert {
    bool enabled=false;
    QString listId;
    qint64 intervalMs=0; // Exact opt-in interval, never follows a tab switch.
    bool operator==(const ChartDrawingAlert&) const=default;
};
struct ChartDrawing {
    QString id;
    ChartDrawingKind kind=ChartDrawingKind::Horizontal;
    QList<ChartDrawingPoint> anchors; // Horizontal 1, Trend/Measure/FibRetracement/Zone 2, FibExtension 2-3.
    ChartDrawingAlert alert;
    bool visible=true;
    bool locked=false;
    QString autoBatchId;
    bool operator==(const ChartDrawing&) const=default;
};
struct ChartFibLevel {
    double ratio=0;
    double price=0;
    bool operator==(const ChartFibLevel&) const=default;
};
struct ChartDrawingContext {
    WatchlistIdentity identity;
    QString currency;
    bool publicPersistenceAllowed=false;
    // True only for explicit public-chart selection; never inferred from holdings.
    bool operator==(const ChartDrawingContext&) const=default;
};
struct ChartDrawingBar {
    MarketCandle candle;
    qint64 completesAtMs=0;
    bool sourceCompleted=false,connectsPrevious=false;
    // Calendar continuity only: an observed VI row still counts for measurement.
};
struct ChartDrawingFrame {
    WatchlistIdentity identity;
    QString currency;
    qint64 intervalMs=0,asOfMs=0;
    quint64 contextGeneration=0;
    QList<ChartDrawingBar> bars; // Actual closed observations only; no forming/projection.
};
struct ChartDrawingMeasurement {
    bool available=false;
    QString reason;
    std::optional<double> returnPercent;
    std::optional<int> barIntervals;
    // Price result may be available while an intraday gap makes bar count unknown.
};
struct ChartDrawingSnap {
    bool available=false,snapped=false;
    QString reason,field; // field close/open/high/low; no account-price candidate exists.
    ChartDrawingPoint point;
};
struct ChartHorizontalLineRule {
    QString drawingId,listId,revision;
    WatchlistIdentity identity;
    QString currency,price;
    qint64 intervalMs=0;
    bool enabled=false;
    // Transient projection of one stored drawing. No duplicate price persistence.
};

class ChartDrawings final:public QObject {
    Q_OBJECT
  public:
    static constexpr int MaxContexts=128,MaxPerContext=100,MaxTotal=2000;
    static constexpr int MaxPriceChars=512,MaxFrameBars=5000,MaxJsonDepth=8;
    static constexpr qsizetype MaxJsonBytes=2*1024*1024;
    explicit ChartDrawings(QObject* parent=nullptr);
    ~ChartDrawings() override;
    static QString bucketForInterval(qint64 intervalMs);
    bool setContext(const ChartDrawingContext&,qint64 intervalMs);
    void clearContext();
    ChartDrawingContext context() const;
    qint64 intervalMs() const;
    quint64 contextGeneration() const;
    QList<ChartDrawing> drawings() const;
    void setVerifiedInstruments(const QList<WatchlistIdentity>&);
    void setPublicLists(const QMap<QString,QList<WatchlistIdentity>>&);
    QString create(ChartDrawingKind,const QList<ChartDrawingPoint>&,
                   const ChartDrawingFrame&,QString* error=nullptr);
    bool applyBatch(const QList<ChartDrawing>&,const ChartDrawingFrame&,QString* error=nullptr);
    bool moveAnchor(const QString& id,int anchorIndex,const ChartDrawingPoint&,
                    const ChartDrawingFrame&,QString* error=nullptr);
    // Whole-shape move: every anchor is replaced at once with the same checks.
    bool moveDrawing(const QString& id,const QList<ChartDrawingPoint>& anchors,
                     const ChartDrawingFrame&,QString* error=nullptr);
    bool remove(const QString& id,QString* error=nullptr);
    bool clear(); // Current context only; lock blocks mutation, locked shapes stay.
    bool setDrawingVisible(const QString& id,bool visible);
    bool setDrawingLocked(const QString& id,bool locked);
    // Shape edits (create, move, delete, clear) of the current context can be undone and redone,
    // up to MaxUndoSteps each. Alert, visibility and lock of shapes that still exist are never rolled
    // back, and a step that would remove or move a locked shape is refused.
    static constexpr int MaxUndoSteps=50;
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    bool visible() const;
    void setVisible(bool);
    bool locked() const;
    void setLocked(bool);
    bool magnetEnabled() const;
    void setMagnetEnabled(bool);
    bool setAlert(const QString& drawingId,const ChartDrawingAlert&,QString* error=nullptr);
    QList<ChartHorizontalLineRule> horizontalLines() const;
    void clearPrivate(); // Drop ephemeral contexts; never promotes them to public.
    QByteArray publicState() const;
    bool restorePublicState(const QByteArray&,QString* error=nullptr);
    static bool validFrame(const ChartDrawingFrame&);
    static QList<ChartFibLevel> fibLevels(const ChartDrawing&);
    static std::optional<MarketCandle> readOhlc(const ChartDrawingFrame&,qint64 observedStartsAtMs);
    static ChartDrawingSnap snapOhlc(const ChartDrawingFrame&,const ChartDrawingPoint& freePoint,
        double pixelsPerPriceUnit,double radiusPx=8);
    static ChartDrawingMeasurement measure(const QList<ChartDrawingPoint>&,
        const ChartDrawingFrame&);
  signals:
    void changed();
    void publicStateChanged();
    void horizontalLinesChanged();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
