#include "keefi/CoinFlowStudyRunner.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QTest>
#include <algorithm>
#include <atomic>
using namespace keefi;
namespace {
CoinFlowSnapshot synthetic(int bars=200,int trades=10000){
    constexpr qint64 start=1792713600000,minute=60000;const qint64 now=start+bars*minute+1000;
    CoinFlowSnapshot input;input.identity="coin:binance:SYNUSDT";auto& item=input.primary.instrument;
    item.identity=input.identity;item.symbol="SYNUSDT";item.baseAsset="SYN";item.quoteAsset="USDT";
    item.venue=CoinFlowVenue::BinanceSpot;item.grain=CoinFlowGrain::SpotAggregate;item.verified=true;item.status="TRADING";
    item.evidence={"synthetic","https://api.binance.com/api/v3/exchangeInfo?permissions=SPOT&showPermissionSets=false",0,now-1,now-1};
    input.primary.evidence=item.evidence;
    for(int i=0;i<bars;++i){CoinFlowClosedMinute row;const auto at=start+i*minute;
        row.candle={at,0,"100","101","99",i%2?"100.1":"100","10"};row.completesAtMs=at+minute;
        row.knownAtMs=row.decisionAtMs=at+minute+1000;row.connectsPrevious=i>0;row.liveEligible=i==bars-1;
        row.evidence={"synthetic","https://example.invalid/fixture",0,row.knownAtMs,row.knownAtMs};input.minutes.append(row);
        input.primary.periods.append({start,at+minute,at+minute+1,1,CoinFlowCoverage::ObservedContinuous,{}});
    }
    for(int i=0;i<trades;++i){CoinTradeObservation row;row.id=QString::number(i+1);row.price="100";row.quantity=QString::number(1+i%23);
        row.side=i%3?CoinFlowSide::Buy:CoinFlowSide::Sell;row.tradeAtMs=start+(qint64(i)*bars*minute/trades);
        row.eventAtMs=row.tradeAtMs+1;row.receivedAtMs=row.knownAtMs=row.tradeAtMs+2;row.epoch=1;row.liveEligible=true;input.primary.trades.append(row);}
    return input;
}
}
class CoinFlowStudyRunnerTest:public QObject {
 Q_OBJECT
private slots:
    void workerDoesNotBlockEventLoop();
    void newestPendingSnapshotWins();
    void invalidationDiscardsLateResult();
    void maximumFixtureBenchmark();
    void extendedStudiesStayOffAndInputRemainsImmutable();
    void reversalGradeUsesOriginalDecisionWindow();
};
void CoinFlowStudyRunnerTest::workerDoesNotBlockEventLoop(){
    bool heartbeat=false,done=false,onWorker=false;QThread* mainThread=QThread::currentThread();
    auto calculate=[&](const CoinFlowSnapshot& input,qint64){onWorker=QThread::currentThread()!=mainThread;QThread::msleep(35);CoinFlowStudySet r;r.identity=input.identity;return r;};
    QTimer::singleShot(0,this,[&]{heartbeat=true;});
    if(qEnvironmentVariableIsSet("KEEFI_FLOW_RUNNER_LEGACY")){calculate({},1);QVERIFY2(heartbeat,"RED baseline: synchronous calculator prevented the queued UI heartbeat");return;}
    CoinFlowStudyRunner runner(nullptr,calculate);connect(&runner,&CoinFlowStudyRunner::resultReady,this,[&](const QString&,const CoinFlowStudySet&,qint64){done=true;QVERIFY(heartbeat);});
    CoinFlowSnapshot snapshot;snapshot.identity="coin:binance:SYNUSDT";runner.request(snapshot,1,"chart:1");
    QTRY_VERIFY(done);QVERIFY(onWorker);
}
void CoinFlowStudyRunnerTest::newestPendingSnapshotWins(){
    QStringList ids;std::atomic_int calls=0;
    CoinFlowStudyRunner runner(nullptr,[&](const CoinFlowSnapshot& input,qint64){++calls;CoinFlowStudySet r;r.identity=input.identity;return r;});
    connect(&runner,&CoinFlowStudyRunner::resultReady,this,[&](const QString&,const CoinFlowStudySet& result,qint64){ids.append(result.identity);});
    CoinFlowSnapshot input;input.identity="first";runner.request(input,1,"same");QTRY_COMPARE(ids.size(),1);
    input.identity="second";runner.request(input,2,"same");input.identity="third";runner.request(input,3,"same");
    QTRY_COMPARE_WITH_TIMEOUT(ids.size(),2,2500);QCOMPARE(ids,QStringList({"first","third"}));QCOMPARE(calls.load(),2);
}
void CoinFlowStudyRunnerTest::invalidationDiscardsLateResult(){
    auto gate=std::make_shared<QSemaphore>();int applied=0,extendedApplied=0;
    CoinFlowStudyRunner runner(nullptr,[gate](const CoinFlowSnapshot&,qint64){gate->acquire();return CoinFlowStudySet{};});
    connect(&runner,&CoinFlowStudyRunner::extendedResultReady,this,[&]{++extendedApplied;});
    connect(&runner,&CoinFlowStudyRunner::resultReady,this,[&]{++applied;});runner.request({},1,"old",{"strong"});runner.invalidate();gate->release();
    QTRY_VERIFY(!runner.pending());QCOMPARE(applied,0);QCOMPARE(extendedApplied,0);runner.setEnabled(false);runner.request({},2,"disabled",{"strong"});QVERIFY(!runner.pending());
}
void CoinFlowStudyRunnerTest::extendedStudiesStayOffAndInputRemainsImmutable(){
    constexpr qint64 base=1728000000000;CoinFlowSnapshot input;input.identity="coin:binance:SYNUSDT";
    auto& w=input.primary;w.instrument.verified=true;w.instrument.venue=CoinFlowVenue::BinanceSpot;w.instrument.grain=CoinFlowGrain::SpotAggregate;
    w.instrument.identity=input.identity;w.instrument.symbol="SYNUSDT";w.instrument.baseAsset="SYN";w.instrument.quoteAsset="USDT";w.instrument.status="TRADING";
    w.periods.append({base,base+300000,base+300001,1,CoinFlowCoverage::ObservedContinuous,{}});
    CoinTradeObservation trade;trade.id="original";trade.price="1";trade.quantity="100000";trade.side=CoinFlowSide::Buy;
    trade.tradeAtMs=base+1000;trade.eventAtMs=base+1001;trade.receivedAtMs=trade.knownAtMs=base+1002;trade.epoch=1;trade.liveEligible=true;w.trades.append(trade);
    CoinFlowStudyRunner runner(nullptr,[](const CoinFlowSnapshot& snapshot,qint64){CoinFlowStudySet r;r.identity=snapshot.identity;return r;});
    QList<ExtendedAmountDelta> deltas;QStringList originalIds;
    connect(&runner,&CoinFlowStudyRunner::extendedResultReady,this,[&](const QString&,const ExtendedAmountDelta& delta,const ExtendedLowVolumeZones&,const ExtendedStrongReversal&){deltas.append(delta);});
    connect(&runner,&CoinFlowStudyRunner::calculatedResultReady,this,[&](const CoinFlowSnapshot& snapshot,const QString&,const CoinFlowStudySet&){originalIds.append(snapshot.primary.trades.first().id);});
    runner.request(input,base+300001,"same");QTRY_COMPARE(deltas.size(),1);QVERIFY(deltas.first().buckets.isEmpty());
    runner.request(input,base+300001,"same",{"cvd"});input.primary.trades.first().id="caller-mutated";QTRY_COMPARE_WITH_TIMEOUT(deltas.size(),2,2500);
    QVERIFY(deltas.last().state.available);QCOMPARE(deltas.last().buckets.size(),3);QCOMPARE(originalIds.last(),QString("original"));
}
void CoinFlowStudyRunnerTest::maximumFixtureBenchmark(){
    if(!qEnvironmentVariableIsSet("KEEFI_FLOW_BENCHMARK"))QSKIP("Optional 30-run synthetic 200-bar/10000-trade timing receipt");
    const auto input=synthetic();const qint64 asOf=input.minutes.last().knownAtMs;QList<qint64> pure,apply;
    CoinFlowStudySet projected;int available=0;
    for(int i=0;i<30;++i){QElapsedTimer timer;timer.start();auto result=coinFlowStudies(input,asOf);pure.append(timer.nsecsElapsed());
        available+=result.available;timer.restart();projected=std::move(result);apply.append(timer.nsecsElapsed());}
    std::sort(pure.begin(),pure.end());std::sort(apply.begin(),apply.end());QCOMPARE(available,30);
    const QJsonObject receipt{{"bars",200},{"trades",10000},{"liveMinuteDecisions",1},{"runs",30},{"calculatorMedianNs",double((pure[14]+pure[15])/2)},
        {"calculatorP95Ns",double(pure[28])},{"dtoApplyMedianNs",double((apply[14]+apply[15])/2)},{"dtoApplyP95Ns",double(apply[28])},
        {"note","DTO assignment cost only; not widget painting or live-account performance"}};
    qInfo().noquote()<<QJsonDocument(receipt).toJson(QJsonDocument::Compact);
}
void CoinFlowStudyRunnerTest::reversalGradeUsesOriginalDecisionWindow(){
    auto input=synthetic(21,1);
    const QStringList closes{"100","96","94","95"};
    for(int i=0;i<4;++i){auto& c=input.minutes[17+i].candle;c.open=c.high=c.low=c.close=closes[i];}
    input.minutes.last().candle.open=input.minutes.last().candle.low="94";
    input.minutes.last().candle.volume="20";
    auto& trade=input.primary.trades.first();trade.tradeAtMs=input.minutes.last().candle.startsAtMs+10;
    trade.eventAtMs=trade.tradeAtMs+1;trade.knownAtMs=trade.receivedAtMs=trade.tradeAtMs+2;trade.side=CoinFlowSide::Buy;
    ExtendedStrongReversal result;int applied=0;CoinFlowStudyRunner runner;
    connect(&runner,&CoinFlowStudyRunner::extendedResultReady,this,[&](const QString&,const ExtendedAmountDelta&,const ExtendedLowVolumeZones&,const ExtendedStrongReversal& value){result=value;++applied;});
    // Advancing evaluation time cannot move a completed reversal's five-minute evidence window.
    runner.request(input,input.minutes.last().decisionAtMs+60000,"frozen",{"strong"});
    QTRY_COMPARE(applied,1);QCOMPARE(result.grade,ExtendedReversalGrade::Strong);
    QCOMPARE(result.throughMs,input.minutes.last().completesAtMs);
    QCOMPARE(result.events.size(),1);QCOMPARE(result.events.first().grade,ExtendedReversalGrade::Strong);
    QVERIFY(!result.events.first().outcome); // The physical one-hour endpoint has not arrived.
    input.primary.trades.clear();
    runner.request(input,input.minutes.last().decisionAtMs+120000,"frozen",{"strong"});
    QTRY_COMPARE_WITH_TIMEOUT(applied,2,2500);QCOMPARE(result.events.first().grade,ExtendedReversalGrade::Strong);
    QCOMPARE(result.grade,ExtendedReversalGrade::Strong);
    runner.invalidate(); // Lock/context generation revokes the remembered grade.
    runner.request(input,input.minutes.last().decisionAtMs+120000,"fresh",{"strong"});
    QTRY_COMPARE_WITH_TIMEOUT(applied,3,2500);QCOMPARE(result.events.first().grade,ExtendedReversalGrade::Incomplete);
    runner.request(input,input.minutes.last().decisionAtMs+60000,"frozen",{});
    QTRY_COMPARE_WITH_TIMEOUT(applied,4,2500);QVERIFY(result.events.isEmpty());
}
QTEST_GUILESS_MAIN(CoinFlowStudyRunnerTest)
#include "tst_coin_flow_study_runner.moc"
