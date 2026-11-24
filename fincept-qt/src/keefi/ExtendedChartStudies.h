#pragma once
#include "keefi/ChartIndicatorState.h"
#include "keefi/ChartStudies.h"
#include "keefi/CoinFlowStudies.h"
#include <QMap>
#include <QSet>
#include <optional>

namespace keefi {
struct ChartStudyTimeline;

// Immutable, public display-only outputs. No accounts, clocks, persistence or I/O.
struct ExtendedStudyState {
    bool available=false;
    IndicatorDirection direction=IndicatorDirection::None;
    QString reason;
};
enum class ExtendedMaKind { Simple, Exponential, Weighted };
struct ExtendedMaSpec {
    QString id;
    int period=20;
    ExtendedMaKind kind=ExtendedMaKind::Simple;
    bool enabled=true;
};
inline constexpr int ChartMovingAverageMaximumLines=4;
inline constexpr int ChartMovingAverageMaximumPeriod=200;
inline constexpr int ChartDisplayBandPeriod=200;
inline constexpr double ChartDisplayBandDeviations=2;
inline QList<ExtendedMaSpec> defaultChartMovingAverages(){
    return {{"ma1",21,ExtendedMaKind::Exponential},{"ma2",50,ExtendedMaKind::Exponential},
            {"ma3",100,ExtendedMaKind::Exponential},{"ma4",200,ExtendedMaKind::Exponential}};
}
struct ExtendedMaLine {
    ExtendedMaSpec spec;
    ExtendedStudyState state;
    QList<ChartPricePoint> points;
};
struct ExtendedScalarSeries {
    ExtendedStudyState state;
    QList<ChartPricePoint> points; // Only valid points, candle START coordinates.
};
struct ExtendedBandPoint { qint64 atMs=0; double lower=0,middle=0,upper=0; };
struct ExtendedBandSeries {
    ExtendedStudyState state;
    QList<ExtendedBandPoint> points;
};
struct ExtendedMacdSeries {
    ExtendedStudyState state;
    QList<ChartPricePoint> line,signal,histogram;
};
struct ExtendedTimedDirection {
    qint64 completesAtMs=0,knownAtMs=0,intervalMs=0;
    ExtendedStudyState state;
};
struct ExtendedSignal {
    QString id;
    qint64 atMs=0;
    double entryClose=0;
    IndicatorDirection direction=IndicatorDirection::None;
    std::optional<int> quality;
};
struct ExtendedOutcome {
    qint64 signalAtMs=0,evaluationAtMs=0;
    IndicatorDirection direction=IndicatorDirection::None;
    std::optional<int> quality;
    double returnPercent=0,directionalReturnPercent=0;
};
struct ExtendedScorecardSlice {
    IndicatorDirection direction=IndicatorDirection::None;
    int qualityBand=-1; // -1 all qualities, -2 unknown, 0=0..4, 1=5..7, 2=8..10.
    bool available=false;
    int samples=0,up=0,down=0,flat=0;
    std::optional<double> meanReturnPercent,meanDirectionalReturnPercent;
};
struct ExtendedScorecard {
    bool available=false,costsExcluded=true;
    QString reason;
    qint64 horizonMs=0;
    int samples=0,unresolvedSignals=0,overlappingSignals=0;
    std::optional<double> meanReturnPercent;
    QList<ExtendedOutcome> outcomes;
    QList<ExtendedScorecardSlice> slices;
};
struct ExtendedBreakout {
    ExtendedStudyState state;
    double previousHigh=0,previousLow=0;
    std::optional<int> quality;
    QString qualityReason;
    QList<ExtendedSignal> entries;
    ExtendedScorecard scorecard;
};
struct ExtendedPriceZone { double low=0,high=0; };
struct ExtendedLowVolumeZones {
    ExtendedStudyState state;
    bool candleApproximation=true;
    int observations=0;
    QList<ExtendedPriceZone> zones; // All observed zones; UI chooses nearest two.
};
struct ExtendedChartOptions {
    // Empty means OFF: no new study is calculated merely because a chart opened.
    QSet<QString> enabled;
    QSet<QString> eventStudies; // Explicit historical-event opt-in; MA alone remains visual-only.
    QList<ExtendedMaSpec> movingAverages{{"ma20",20,ExtendedMaKind::Simple},{"ma60",60,ExtendedMaKind::Simple}};
    int rsiPeriod=14,macdFast=12,macdSlow=26,macdSignal=9;
    // Alerts and event studies keep their own observation window.
    int displayBandPeriod=ChartDisplayBandPeriod;
    double displayBandDeviations=ChartDisplayBandDeviations;
    int bandPeriod=20;
    double bandDeviations=2;
    QList<ExtendedTimedDirection> higherTrends; // Historical known-at snapshots, never future-leaked.
};
struct ExtendedChartStudySet {
    bool available=false;
    QString reason;
    qint64 intervalMs=0,lastCompletedAtMs=0;
    int observations=0,currentRunBars=0;
    ExtendedStudyState movingAverageState;
    QList<ExtendedMaLine> maLines;
    ExtendedBandSeries bb;
    ExtendedScalarSeries rsi;
    ExtendedMacdSeries macd;
    ExtendedBreakout breakout;
    ExtendedLowVolumeZones lowVolumeZones;
    ExtendedTimedDirection trend;
    QMap<QString,QList<ExtendedSignal>> studyEntries;
    QMap<QString,ExtendedScorecard> scorecards;
};
ExtendedChartStudySet extendedChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,
    const ExtendedChartOptions& options={});
ExtendedChartStudySet extendedChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,
    const ExtendedChartOptions& options,const ChartStudyTimeline& timeline);
// Display MA/BB from bounded completed history. Stocks also project RSI/MACD
// from session-verified closes. Display outputs never produce events or scorecards.
ExtendedChartStudySet extendedChartDisplayStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,
    const ExtendedChartOptions& options);
ExtendedChartStudySet extendedChartDisplayStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,
    const ExtendedChartOptions& options,const ChartStudyTimeline& timeline);
IndicatorDirection extendedMovingAverageDirection(double completedClose,const QList<std::optional<double>>& configuredValues);

enum class ExtendedLevelPeriod { Day, Week, Month, EarlyWeek, OpeningGap };
struct ExtendedLevelCandidate {
    QString id,label,price,source;
    ExtendedLevelPeriod period=ExtendedLevelPeriod::Day;
    qint64 fromMs=0,throughMs=0,knownAtMs=0;
    bool complete=false;
};
struct ExtendedLevel {
    QString price;
    QStringList ids,labels;
    QList<ExtendedLevelCandidate> members;
};
struct ExtendedLevelSet {
    ExtendedStudyState state;
    bool approximateTick=false;
    QList<ExtendedLevel> all,visible;
};
// The source verifies period/calendar identity. This calculator independently
// rejects future/uncompleted periods, then merges exact decimals and selects 2+2.
ExtendedLevelSet chartLevels(const QList<ExtendedLevelCandidate>& candidates,const QString& currentPrice,
    const QString& verifiedTick,qint64 chartIntervalMs,qint64 asOfMs);

struct ExtendedDeltaBucket {
    QString id,buyQuote="0",sellQuote="0",signedQuote="0";
    std::optional<double> signedFraction;
    ExtendedStudyState state;
    QList<ChartPricePoint> points; // Completed one-minute cumulative quote notional per epoch.
};
struct ExtendedAmountDelta {
    ExtendedStudyState state;
    QString currency;
    quint64 epoch=0;
    qint64 fromMs=0,throughMs=0,knownAtMs=0;
    bool sourceDirectionOnly=false;
    QList<ExtendedDeltaBucket> buckets;
};
ExtendedAmountDelta extendedAmountDelta(const CoinFlowWindow& window,qint64 asOfMs);
struct ExtendedCompressionGauge {
    ExtendedStudyState state;
    std::optional<bool> compressed;
    std::optional<double> oiChangePercent;
};
ExtendedCompressionGauge extendedCompressionGauge(const CandleCompression&,const CoinOiStudy&);
struct ExtendedBreadthObservation { QString identity; std::optional<double> changePercent; bool fresh=false; };
struct ExtendedBreadth {
    ExtendedStudyState state;
    int ready=0,population=0,up=0,down=0,flat=0;
    std::optional<double> percent;
};
ExtendedBreadth extendedBreadth(const QList<ExtendedBreadthObservation>& observations,double minimumCoverage=.8);
struct ExtendedRelativeStrength {
    ExtendedStudyState state;
    qint64 fromMs=0,throughMs=0;
    std::optional<double> percent;
};
ExtendedRelativeStrength extendedRelativeStrength(const QList<MarketCandle>& alt,const QList<MarketCandle>& benchmark,
    qint64 intervalMs=3600000,int lookbackBars=24);
ExtendedTimedDirection extendedTrend(const QList<MarketCandle>& rows,qint64 intervalMs,int period=20);
ExtendedTimedDirection extendedTrend(const QList<MarketCandle>& rows,qint64 intervalMs,int period,const ChartStudyTimeline&);
enum class ExtendedReversalGrade { None, Basic, Reinforced, Strong, Incomplete };
struct ExtendedReversalEvent {
    CoinFlowSignalKind kind=CoinFlowSignalKind::Rebound;
    IndicatorDirection direction=IndicatorDirection::None;
    ExtendedReversalGrade grade=ExtendedReversalGrade::Incomplete;
    qint64 signalAtMs=0,decisionAtMs=0;
    std::optional<CoinFlowOutcome> outcome;
};
struct ExtendedStrongReversal {
    ExtendedStudyState state;
    ExtendedReversalGrade grade=ExtendedReversalGrade::None;
    qint64 throughMs=0,knownAtMs=0;
    qint64 asOfMs=0,horizonMs=60*60000;
    QString identity;
    QList<ExtendedReversalEvent> events;
};
ExtendedStrongReversal extendedStrongReversal(const CoinReversalStudy&,const CoinVolumeStudy&,const CoinTakerStudy& spot,qint64 decisionAtMs=0);
ExtendedStrongReversal extendedStrongReversal(const CoinReversalStudy&,const CoinVolumeStudy&,const ExtendedAmountDelta& spot,qint64 decisionAtMs=0);
// Immutable joins of original reversal decisions and already-verified exact
// one-hour outcomes. Does not recalculate grades with current market inputs.
ExtendedStrongReversal extendedStrongReversal(const CoinFlowStudySet&,const ExtendedStrongReversal* previous=nullptr);
ExtendedLowVolumeZones extendedTradeLowVolumeZones(const CoinFlowWindow& window,qint64 asOfMs);
} // namespace keefi
