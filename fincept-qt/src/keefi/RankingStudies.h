#pragma once
#include "keefi/ChartStudyFeed.h"
namespace keefi {
enum class RankingStudy { None, Compression, OpenInterest, SpotFutures, Pullback, Volume, Drawdown };
struct RankingStudyValue {
    bool available=false;
    QString text, information;
    qint64 throughMs=0;
};
QString rankingStudyKey(RankingStudy);
QString rankingStudyLabel(RankingStudy);
RankingStudy rankingStudyFromKey(const QString&);
// Completed native 1h bars only. Stock continuity/adjustment follows its broker timeline.
// Quote freshness and candle freshness stay independent; no portfolio inputs.
RankingStudyValue rankingCandleStudy(RankingStudy,const ChartStudyObservation&);
}
