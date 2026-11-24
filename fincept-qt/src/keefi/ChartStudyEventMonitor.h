#pragma once
#include "keefi/ChartAlertEngine.h"
#include "keefi/ExtendedChartStudies.h"
#include <QMap>
namespace keefi {
// Public, completed-candle transitions only. Initial REST history, context
// switches, gaps, hidden/off warm-ups and stale revisions cannot notify.
class ChartStudyEventMonitor {
public:
    QList<ChartPublicStudyEvent> observe(const MarketCandleSeries&,const ExtendedChartStudySet&,bool realtimeAllowed);
    void clear();
private:
    struct Lane {qint64 completed=0;IndicatorDirection direction=IndicatorDirection::None;bool seeded=false;};
    QMap<QString,Lane> lanes_;
    QString context_;
};
}
