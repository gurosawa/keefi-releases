#include "keefi/CandleTradeOverlay.h"
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Minute=60000, Start=1792598400000, Now=Start+10000;
MarketCandle anchor(QString close="100",QString volume="2"){
    return {Start,0,"100","110","90",close,volume};
}
QJsonObject trade(qint64 id,qint64 at,QString price="105",QString quantity="0.1"){
    return {{"e","trade"},{"s","SYNUSDT"},{"t",QString::number(id)},
        {"T",QString::number(at)},{"E",QString::number(at)},{"p",price},{"q",quantity}};
}
CandleTradeOverlay overlay(){
    CandleTradeOverlay result;result.reset("coin:binance:SYNUSDT","binance","SYNUSDT",Minute);
    result.setBinanceAnchor(anchor(),100,Now);return result;
}
}
class CandleTradeOverlayTest:public QObject {
    Q_OBJECT
  private slots:
    void exactTailAndLateAnchorDoNotDoubleCount(){
        auto value=overlay();
        QVERIFY(value.receive(trade(101,Now+1,"111","0.1"),Now+1));
        QVERIFY(value.receive(trade(102,Now+2,"89","0.2"),Now+2));
        auto formed=value.forming(Now+2);QVERIFY(formed);
        QCOMPARE(formed->open,QString("100"));QCOMPARE(formed->high,QString("111"));
        QCOMPARE(formed->low,QString("89"));QCOMPARE(formed->close,QString("89"));QCOMPARE(formed->volume,QString("2.3"));
        auto first=anchor("111","2.1");first.high="111";
        value.setBinanceAnchor(first,101,Now+3);
        formed=value.forming(Now+3);QVERIFY(formed);QCOMPARE(formed->volume,QString("2.3"));QCOMPARE(formed->close,QString("89"));
        QVERIFY(!value.receive(trade(102,Now+2,"89","0.2"),Now+4));
        QCOMPARE(value.forming(Now+4)->volume,QString("2.3"));
    }
    void aMissingIdFallsBackUntilAnOfficialSnapshotCoversIt(){
        auto value=overlay();QVERIFY(value.receive(trade(102,Now+1,"106"),Now+1));
        QVERIFY(!value.forming(Now+1));QVERIFY(value.observation(Now+1).available);
        value.setBinanceAnchor(anchor("105","2.1"),101,Now+2);
        const auto formed=value.forming(Now+2);QVERIFY(formed);
        QCOMPARE(formed->close,QString("106"));QCOMPARE(formed->volume,QString("2.2"));
    }
    void unseenOutOfOrderIdCannotRepairTheTail(){
        auto value=overlay();value.receive(trade(102,Now+2,"106"),Now+2);
        QVERIFY(!value.receive(trade(101,Now+1,"105"),Now+3));QVERIFY(!value.forming(Now+3));
        value.setBinanceAnchor(anchor("105","2.1"),101,Now+4);QVERIFY(!value.forming(Now+4));
        value.setBinanceAnchor(anchor("106","2.2"),102,Now+5);
        QVERIFY(value.receive(trade(103,Now+6,"107"),Now+6));
        QVERIFY(value.forming(Now+6));QCOMPARE(value.forming(Now+6)->volume,QString("2.3"));
    }
    void newIntervalIsDisplayOnlyAndGapsStayAbsent(){
        auto value=overlay();const auto at=Start+3*Minute+1;
        QVERIFY(value.receive(trade(101,at,"107","0.4"),at));
        const auto formed=value.forming(at);QVERIFY(formed);
        QCOMPARE(formed->startsAtMs,Start+3*Minute);QCOMPARE(formed->open,QString("107"));
        QCOMPARE(formed->high,QString("107"));QCOMPARE(formed->low,QString("107"));QCOMPARE(formed->volume,QString("0.4"));
    }
    void inconsistentDuplicateNotifiesThatTheOverlayWasRevoked(){
        auto value=overlay();QVERIFY(value.receive(trade(101,Now+1,"111","0.1"),Now+1));
        QVERIFY(value.forming(Now+1));
        QVERIFY(value.receive(trade(101,Now+1,"111","0.2"),Now+2));
        QVERIFY(!value.forming(Now+2));
        QVERIFY(!value.receive(trade(101,Now+1,"111","0.2"),Now+3));
    }
    void invalidOrFutureTradeDoesNotPublishPrice_data(){
        QTest::addColumn<QString>("kind");
        for(const auto* kind:{"identity","future","event-before-trade","negative-price","non-finite","newline","zero-quantity","oversized-price","identifier-overflow"})QTest::newRow(kind)<<QString(kind);
    }
    void invalidOrFutureTradeDoesNotPublishPrice(){
        QFETCH(QString,kind);auto value=overlay();auto input=trade(101,Now+1);
        if(kind=="identity")input["s"]="OTHERUSDT";
        if(kind=="future")input["E"]=QString::number(Now+2);
        if(kind=="event-before-trade")input["E"]=QString::number(Now);
        if(kind=="negative-price")input["p"]="-1";
        if(kind=="non-finite")input["p"]="NaN";
        if(kind=="newline")input["p"]="105\n";
        if(kind=="zero-quantity")input["q"]="0";
        if(kind=="oversized-price")input["p"]=QString(81,'9');
        if(kind=="identifier-overflow")input["t"]="9223372036854775808";
        QVERIFY(!value.receive(input,Now+1));QVERIFY(!value.observation(Now+1).available);QVERIFY(!value.forming(Now+1));
    }
    void resetAndClockRollbackRevokeTheDisplay(){
        auto value=overlay();value.receive(trade(101,Now+1),Now+1);
        QVERIFY(value.observation(Now+1).available);QVERIFY(!value.observation(Now).available);
        QVERIFY(!value.observation(Now+1+MarketCandles::FreshnessMs+1).available);
        value.reset("coin:binance:SYNUSDT","binance","SYNUSDT",Minute);
        QVERIFY(!value.observation(Now+2).available);QVERIFY(!value.forming(Now+2));
    }
    void overflowingTheReplayBufferWaitsForAnOfficialFence(){
        auto value=overlay();
        for(int i=1;i<=CandleTradeOverlay::BufferLimit+1;++i)
            QVERIFY(value.receive(trade(100+i,Now+i,"105","0.01"),Now+i));
        QVERIFY(!value.forming(Now+CandleTradeOverlay::BufferLimit+1));
        const auto last=100+CandleTradeOverlay::BufferLimit+1;
        value.setBinanceAnchor(anchor("105","42.97"),last,Now+CandleTradeOverlay::BufferLimit+2);
        const auto at=Now+CandleTradeOverlay::BufferLimit+3;
        QVERIFY(value.receive(trade(last+1,at,"106","0.01"),at));
        QVERIFY(value.forming(at));QCOMPARE(value.forming(at)->volume,QString("42.98"));
    }
    void upbitExposesOnlyTheIndividualTrade(){
        CandleTradeOverlay value;value.reset("coin:upbit:KRW-SYN","upbit","KRW-SYN",Minute);
        const QJsonObject input{{"type","trade"},{"code","KRW-SYN"},{"stream_type","REALTIME"},
            {"sequential_id","17877285547970000"},{"trade_timestamp",QString::number(Now+1)},{"timestamp",QString::number(Now+1)},
            {"trade_price","111.00"},{"trade_volume","0.10"}};
        QVERIFY(value.receive(input,Now+1));const auto quote=value.observation(Now+1);
        QVERIFY(quote.available);QCOMPARE(quote.identity,QString("coin:upbit:KRW-SYN"));QCOMPARE(quote.price,QString("111"));
        QCOMPARE(quote.tradeAtMs,Now+1);QVERIFY(!value.forming(Now+1));
        QVERIFY(!value.receive(input,Now+2));
    }
};
QTEST_GUILESS_MAIN(CandleTradeOverlayTest)
#include "tst_candle_trade_overlay.moc"
