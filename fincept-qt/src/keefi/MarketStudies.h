#pragma once
#include "keefi/MarketCandles.h"
namespace keefi {
struct CandleTrend { int minutes=0,bars=0; bool available=false; double mean=0; int direction=0; qint64 endsAtMs=0; };
struct CandleSession { QString label; int bars=0; double meanPercent=0,meanAbsolutePercent=0; };
struct CandleStudySet {
    bool available=false;
    QString reason;
    int observations=0;
    bool sweptHigh=false, sweptLow=false;
    double previousHigh=0, previousLow=0;
    bool adxAvailable=false;
    double adx=0, plusDi=0, minusDi=0;
    QString regime;
    bool profileAvailable=false, narrowHighVolume=false;
    double profileLow=0, profileHigh=0, profileVolume=0, profileShare=0;
    int profileBars=0, overlap=0;
    QStringList nearbyLevels;
    QList<CandleTrend> trends;
    QList<CandleSession> sessions;
    bool similarityAvailable=false;
    int similaritySamples=0;
    double similarity=0;
    qint64 similarFromMs=0,similarToMs=0;
};
// Display studies of observed, completed candles. Gaps restart the
// window. Values are never used by account valuation or order decisions.
CandleStudySet candleStudies(const QList<MarketCandle>&, qint64 intervalMs=MarketCandles::IntervalMs);
struct ChartStudyTimeline;
CandleStudySet candleStudies(const QList<MarketCandle>&, qint64 intervalMs, const ChartStudyTimeline&);
CandleStudySet stockDisplayAdx(const QList<MarketCandle>&,qint64 intervalMs,const ChartStudyTimeline&);
}
