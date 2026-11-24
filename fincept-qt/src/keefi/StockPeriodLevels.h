#pragma once
#include "keefi/StockChartTypes.h"
#include "keefi/ExtendedChartStudies.h"

namespace keefi {
struct StockPeriodLevelResult {
    QList<ExtendedLevelCandidate> candidates;
    QString reason;
};
struct StockLevelMinuteWindow {qint64 fromMs=0,throughMs=0;};
QList<StockLevelMinuteWindow> stockLevelMinuteWindows(const StockChartInstrument&,
    const StockCalendar&,qint64 asOfMs);
// No I/O, calendar inference or price adjustment. A missing immediate period is
// omitted, never replaced by an older one. Minute input is optional (opening gap).
StockPeriodLevelResult stockPeriodLevels(const StockCandlePage& daily,
    const StockCandlePage& minute,const StockCalendar& calendar,qint64 asOfMs);
}
