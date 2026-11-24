#include "keefi/CandleAggregation.h"
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Start=1792627200000; // Synthetic UTC midnight.
QList<MarketCandle> minutes(int count,qint64 first=Start,qint64 interval=60000) {
    QList<MarketCandle> result;
    for(int i=0;i<count;++i)result.append({first+i*interval,first+i*interval+1000,
        QString::number(100+i),QString::number(102+i),QString::number(99+i),QString::number(101+i),"0.1"});
    return result;
}
}
class CandleAggregationTest:public QObject {
    Q_OBJECT
  private slots:
    void completeUtcGroupsKeepOhlcvAndExactVolume(){
        const auto rows=minutes(10);
        const auto result=aggregateCompletedCandles(rows,60000,300000,Start+600000);
        QVERIFY(result.available);QCOMPARE(result.candles.size(),2);
        const auto& first=result.candles.first();
        QCOMPARE(first.startsAtMs,Start);QCOMPARE(first.open,QString("100"));
        QCOMPARE(first.high,QString("106"));QCOMPARE(first.low,QString("99"));
        QCOMPARE(first.close,QString("105"));QCOMPARE(first.volume,QString("0.5"));
        QCOMPARE(first.lastStoredTickMs,Start+241000);
    }
    void partialEdgesAndClockCrossingCannotCreateGroups(){
        auto rows=minutes(5,Start+60000);
        QVERIFY(!aggregateCompletedCandles(rows,60000,300000,Start+600000).available);
        rows=minutes(5);
        auto r=aggregateCompletedCandles(rows,60000,300000,Start+299999);
        QVERIFY(!r.available);QVERIFY(r.candles.isEmpty());
        r=aggregateCompletedCandles(rows,60000,300000,Start+300000);
        QVERIFY(r.available);QCOMPARE(r.candles.size(),1);
    }
    void missingChildLeavesActualTimeGap(){
        auto rows=minutes(15);rows.removeAt(7);
        const auto r=aggregateCompletedCandles(rows,60000,300000,Start+900000);
        QVERIFY(r.available);QCOMPARE(r.candles.size(),2);
        QCOMPARE(r.candles[1].startsAtMs-r.candles[0].startsAtMs,600000);
    }
    void invalidInputRejectsAtomically_data(){
        QTest::addColumn<QString>("kind");
        for(const auto name:{"duplicate","out-of-order","unaligned","ohlc","negative-volume","unsupported","reverse","future-source-time"})QTest::newRow(name)<<QString(name);
    }
    void invalidInputRejectsAtomically(){
        QFETCH(QString,kind);auto rows=minutes(5);qint64 target=300000,source=60000;
        if(kind=="duplicate")rows[3]=rows[2];
        if(kind=="out-of-order")std::swap(rows[1],rows[2]);
        if(kind=="unaligned")++rows[0].startsAtMs;
        if(kind=="ohlc")rows[2].low="999";
        if(kind=="negative-volume")rows[2].volume="-1";
        if(kind=="unsupported")target=420000;
        if(kind=="reverse")source=900000;
        if(kind=="future-source-time")rows[0].lastStoredTickMs=Start+999999;
        const auto r=aggregateCompletedCandles(rows,source,target,Start+300000);
        QVERIFY(!r.available);QVERIFY(r.candles.isEmpty());QVERIFY(!r.reason.isEmpty());
    }
    void dailyGroupRequiresSixCompletedFourHourCandles(){
        auto rows=minutes(6,Start,14400000);
        const auto r=aggregateCompletedCandles(rows,14400000,86400000,Start+86400000);
        QVERIFY(r.available);QCOMPARE(r.candles.size(),1);QCOMPARE(r.candles[0].volume,QString("0.6"));
        rows.removeLast();QVERIFY(!aggregateCompletedCandles(rows,14400000,86400000,Start+86400000).available);
    }
    void decimalLexemesRejectTrailingNewlineBeforeArithmetic(){
        auto rows=minutes(5);rows.last().volume="1\n";
        const auto r=aggregateCompletedCandles(rows,60000,300000,Start+300000);
        QVERIFY2(!r.available,"Only exact decimal lexemes may reach display-only OHLCV arithmetic");
        QVERIFY(r.candles.isEmpty());
    }
};
QTEST_GUILESS_MAIN(CandleAggregationTest)
#include "tst_candle_aggregation.moc"
