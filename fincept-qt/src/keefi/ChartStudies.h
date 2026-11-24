#pragma once
#include "keefi/MarketCandles.h"
namespace keefi {
// Conservative display cutoff on 19 observed log-return pairs. This is not a
// forecast probability or a statistical-significance guarantee.
inline constexpr double ChartSimilarityMinimumCorrelation = 0.80;
// Availability of inputs is distinct from whether a valid observation met a condition.
enum class ChartStudyStatus { Insufficient, NoSignal, Signal };
struct ChartPricePoint { qint64 atMs=0; double price=0; };
struct ChartEvidence { QString id,label; double price=0; };
struct ChartPullback {
    bool available=false,inBox=false;
    QString reason;
    qint64 waveLowAtMs=0,waveHighAtMs=0;
    double waveLow=0,waveHigh=0,boxLow=0,boxHigh=0,atr=0;
    int overlap=0,score=0;
    QList<ChartEvidence> evidence;
    ChartStudyStatus status=ChartStudyStatus::Insufficient;
};
struct ChartSimilarPath {
    qint64 fromMs=0,toMs=0,followingToMs=0;
    double correlation=0;
    QList<ChartPricePoint> points;
};
struct ChartSimilarity {
    bool available=false,volatilityNormalized=false;
    QString reason;
    int queryBars=20,followingBars=20,searchBars=0,eligibleCandidates=0,overlapDropped=0,samples=0;
    int upPaths=0,downPaths=0,flatPaths=0;
    qint64 searchFromMs=0,searchToMs=0;
    QList<ChartSimilarPath> paths;
    QList<ChartPricePoint> mean,minimum,maximum;
    ChartStudyStatus status=ChartStudyStatus::Insufficient;
    // Navigation uses an observed start; session completion need not be start + interval.
    qint64 referenceFromMs=0,referenceThroughMs=0,referenceLastStartMs=0;
};
struct ChartCloudPoint { qint64 atMs=0; double fast=0,slow=0; };
struct ChartVolumePoint { qint64 atMs=0; QString volume; };
struct ChartSignal { qint64 atMs=0,waveLowAtMs=0; double entryClose=0; int score=0; };
struct ChartOutcome { qint64 signalAtMs=0,evaluationAtMs=0; double returnPercent=0; };
struct ChartScorecard {
    bool available=false,costsExcluded=true;
    QString reason;
    qint64 horizonMs=0;
    int samples=0,unresolvedSignals=0,overlappingSignals=0,up=0,down=0,flat=0;
    double meanReturnPercent=0;
    QList<ChartOutcome> outcomes;
};
struct ChartStudySet {
    bool available=false;
    QString reason;
    qint64 intervalMs=0,lastCompletedAtMs=0;
    int observations=0,currentRunBars=0;
    ChartPullback pullback;
    ChartSimilarity similarity;
    QList<ChartCloudPoint> cloud;
    QList<ChartVolumePoint> volumes;
    QList<ChartSignal> entries;
    ChartScorecard scorecard;
};
// Pure public display calculation. The caller supplies only completed native
// candles. No clock, account, transport, persistence, or forming-candle input.
ChartStudySet keefiChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs);
struct ChartStudyTimeline;
// Additive stock path: explicit completion/continuity/eligibility, actual UTC coordinates.
ChartStudySet keefiChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,
    const ChartStudyTimeline& timeline);
// The optional price overlay accepts completed prices when historical restriction coverage alone is absent.
// The returned study and all event/scorecard fields retain the original timeline rules.
ChartStudySet keefiChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,
    const ChartStudyTimeline& timeline,QList<ChartCloudPoint>* displayCloud);
}
