#pragma once
#include "keefi/MarketCandles.h"
namespace keefi {
struct CandleAggregation {
    bool available=false;
    QString reason;
    QList<MarketCandle> candles;
};
// Display-only UTC groups. Missing or still-open children are never filled.
CandleAggregation aggregateCompletedCandles(const QList<MarketCandle>&,
    qint64 sourceIntervalMs, qint64 targetIntervalMs, qint64 asOfMs);
}
