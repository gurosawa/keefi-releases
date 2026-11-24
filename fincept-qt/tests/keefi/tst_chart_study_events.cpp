#include "keefi/ChartStudyEventMonitor.h"
#include <QtTest>
using namespace keefi;
class ChartStudyEventsTest final:public QObject{
    Q_OBJECT
private slots:
    void completedTransitionsOnly(){
        ChartStudyEventMonitor monitor;MarketCandleSeries series;series.available=true;series.identity="coin:upbit:KRW-SYN";series.intervalMs=60000;
        ExtendedChartStudySet studies;studies.available=true;studies.intervalMs=60000;studies.lastCompletedAtMs=1728000000000;
        studies.rsi.state={true,IndicatorDirection::Info,{}};series.receivedAtMs=studies.lastCompletedAtMs+1000;
        series.candles={{studies.lastCompletedAtMs-60000,0,"100","110","90","100","1"}};
        QVERIFY(monitor.observe(series,studies,true).isEmpty());
        studies.lastCompletedAtMs+=60000;series.receivedAtMs+=60000;series.candles.last().startsAtMs+=60000;
        studies.rsi.state.direction=IndicatorDirection::Up;
        auto events=monitor.observe(series,studies,true);QCOMPARE(events.size(),1);QCOMPARE(events[0].kind,QString("rsi"));
        QCOMPARE(events[0].direction,MovementDirection::Up);QVERIFY(events[0].liveEligible);
        QVERIFY(monitor.observe(series,studies,true).isEmpty());
        studies.lastCompletedAtMs+=60000;series.receivedAtMs+=60000;series.candles.last().startsAtMs+=60000;
        QVERIFY(monitor.observe(series,studies,true).isEmpty());
        studies.lastCompletedAtMs+=120000;series.receivedAtMs+=120000;series.candles.last().startsAtMs+=120000;
        studies.rsi.state.direction=IndicatorDirection::Down;QVERIFY(monitor.observe(series,studies,true).isEmpty());
        monitor.clear();QVERIFY(monitor.observe(series,studies,true).isEmpty());
    }
    void unavailableAndContextReset(){
        ChartStudyEventMonitor monitor;MarketCandleSeries series;ExtendedChartStudySet studies;
        series.available=true;series.identity="coin:upbit:KRW-SYN";series.intervalMs=60000;series.receivedAtMs=1728000001000;
        series.candles={{1727999940000,0,"100","110","90","100","1"}};
        studies.available=true;studies.intervalMs=60000;studies.lastCompletedAtMs=1728000000000;studies.macd.state={true,IndicatorDirection::Down,{}};
        QVERIFY(monitor.observe(series,studies,true).isEmpty());
        studies.lastCompletedAtMs+=60000;series.receivedAtMs+=60000;series.candles.last().startsAtMs+=60000;
        studies.macd.state.direction=IndicatorDirection::Up;QVERIFY(monitor.observe(series,studies,false).isEmpty());
        QVERIFY(monitor.observe(series,studies,true).isEmpty());
        series.identity="coin:upbit:KRW-OTHER";QVERIFY(monitor.observe(series,studies,true).isEmpty());
    }
};
QTEST_GUILESS_MAIN(ChartStudyEventsTest)
#include "tst_chart_study_events.moc"
