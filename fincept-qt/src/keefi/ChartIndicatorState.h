#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace keefi {
enum class IndicatorDirection { None, Info, Up, Down, Mix };
struct IndicatorAtom {
    QString group;
    IndicatorDirection direction = IndicatorDirection::None;
    bool ready = false;
    QString reason;
};
// A composite indicator contributes one completeness item, but only its raw
// material atoms vote. Visibility never changes calculation or completeness.
enum class IndicatorDataDetail { Normal, Partial, NoSignal };
struct ChartIndicatorState {
    QString id, context;
    quint64 revision = 0;
    bool enabled = true, hidden = false, stateBearing = true, requiredReady = false;
    QList<IndicatorAtom> atoms;
    QString label, status, reason;
    IndicatorDataDetail dataDetail = IndicatorDataDetail::Normal;
};
struct IndicatorGroupState {
    QString group;
    IndicatorDirection direction = IndicatorDirection::None;
    bool partial = false;
    QStringList indicatorIds, reasons;
};
struct ChartIndicatorSummary {
    int enabled = 0, ready = 0, up = 0, down = 0, mix = 0, info = 0, none = 0;
    std::optional<int> completenessPercent;
    QList<IndicatorGroupState> groups;
};
// Strict immutable-context reducer. Old identity/timeframe/revision results
// cannot affect the selected chart; callers retain an explicit pending row.
ChartIndicatorSummary summarizeChartIndicators(const QList<ChartIndicatorState>&,
    const QString& context, quint64 revision);
// Display-only studies and historical comparisons never enter the current
// direction tally or its completeness denominator, including restored choices.
bool chartIndicatorContributesCurrentState(const QString& id);
QString indicatorDirectionText(IndicatorDirection);
// Display policy only. The underlying C4 sample/outcome calculations stay intact.
inline constexpr int ChartScorecardDisplayMinimumSamples = 20;
bool chartScorecardStatisticsVisible(bool available, int samples);
struct ChartStudySet;
struct CoinFlowStudySet;
struct ExtendedStrongReversal;
struct ChartHistoricalSlice {
    QString key,label;
    int events=0,results=0,pending=0,overlapping=0,up=0,down=0,flat=0;
    std::optional<double> meanReturnPercent,meanDirectionalReturnPercent;
};
// Display-only joins of existing immutable events/outcomes; no raw market calculation.
QList<ChartHistoricalSlice> chartPullbackScoreSlices(const ChartStudySet&);
QList<ChartHistoricalSlice> chartFlowScoreSlices(const CoinFlowStudySet&);
QList<ChartHistoricalSlice> chartStrongReversalScoreSlices(const ExtendedStrongReversal&);
}
