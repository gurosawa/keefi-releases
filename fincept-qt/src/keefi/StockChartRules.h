#pragma once
#include "keefi/StockChartTypes.h"

namespace keefi {
// Pure display-only parsing and rules: no clock, network, account or persistence access.
StockCalendar parseStockCalendar(const QString& marketCountry, const HttpResult&, qint64 receivedAtMs);
StockCandlePage parseTossStockCandles(const StockChartInstrument&, qint64 nativeIntervalMs,
    StockAdjustment, const HttpResult&, qint64 requestStartedAtMs, qint64 receivedAtMs);
StockChartSnapshot buildStockChart(const StockCandlePage&, const StockCalendar&,
    const StockRestrictions&, const StockChartOptions&);
} // namespace keefi
