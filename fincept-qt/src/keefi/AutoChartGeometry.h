#pragma once
#include "keefi/ChartDrawings.h"
#include <QList>
#include <QPointF>
#include <QString>

namespace keefi {
class ChartTimeAxis;
struct AutoChartPivot { int bar=-1; bool high=false; QString price; }; // bar = frame.bars 인덱스, price = 그 봉 high 또는 low 문자열 그대로
struct AutoChartCandidate {
    QString key;                       // "fib-retracement", "fib-extension", "zone", "horizontal:1".."horizontal:3", "trend"
    ChartDrawingKind kind=ChartDrawingKind::Horizontal;
    QList<ChartDrawingPoint> anchors;  // atMs는 모두 frame 봉 시작 시각, price는 봉 high/low 원문 문자열
    int touches=0;                     // 미리보기 전용(저장 안 함)
    QList<qint64> sweepAtMs;           // 미리보기 전용(저장 안 함)
    bool trendHighSide=false;          // 추세선 방향(저점선=false, 고점선=true; 미리보기 sweep 표식용)
};
struct AutoChartResult { bool available=false; QString reason; double atr=0; QList<AutoChartPivot> pivots; QList<AutoChartCandidate> candidates; };
class AutoChartGeometry final {
  public:
    static AutoChartResult analyze(const ChartDrawingFrame&);
    // Map a trend between observed bar indices to display slots and prices.
    static QList<QPointF> trendDisplayPath(const QList<ChartDrawingPoint>& anchors,
                                           const ChartDrawingFrame& frame, const ChartTimeAxis& axis);
    static constexpr int MinBars=21, AtrBars=20, ZoneMinBars=8, MaxLevels=3, MinLevelTouches=2, TrendPivotWindow=6, MinTrendTouches=3, MaxTrendPierces=1;
    static constexpr double ZigzagAtr=2.0, LevelToleranceAtr=0.5, ZoneRangeAtr=2.0, TrendToleranceAtr=0.25;
};
}
