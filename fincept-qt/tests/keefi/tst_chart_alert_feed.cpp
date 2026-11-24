#include "keefi/ChartAlertFeed.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/StockChartSource.h"
#include "keefi/CoinFlowSource.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTest>
#include <QSignalSpy>
#include <algorithm>
using namespace keefi;
namespace {
constexpr qint64 Now=1728000000000+250*60000+10000;
class Http final:public TradingTransport {
  public:
    quint64 token=0;QList<RequestSpec> sent;int aborted=0;
    void send(quint64 t,const RequestSpec& r)override{token=t;sent.append(r);}
    void abort()override{++aborted;}
};
ChartAlertDemand demand(QString code="KRW-SYN",bool privateOnly=false){
    return {{"coin","upbit",code},60000,true,true,privateOnly};
}
HttpResult response(qint64 now=Now){
    const auto end=now/60000*60000;QJsonArray rows;
    for(int i=30;i>=0;--i){const auto at=end-i*60000;
        rows.append(QJsonObject{{"market","KRW-SYN"},{"unit","1"},{"timestamp",QString::number(at+1000)},
          {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
          {"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","101"},{"candle_acc_trade_volume","2.00"}});}
    HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();return result;
}
}
class ChartAlertFeedTest final:public QObject {
    Q_OBJECT
  private slots:
    void unsupportedMarketsCannotReachAlertTransport();
    void marketRevocationCancelsAndRebaselinesAlertLane();
    void unsupportedUpbit404StopsAllAlertTimeframes();
    void stockAuthorizationPollingIsQuietAndRecovers();
    void dedicatedPublicPollsUseOnlyEnabledDemandAndClosedBars();
    void visiblePriorityAndGlobalPacingKeepOtherListsQueued();
    void removingPrivateDemandCancelsAndRejectsLateResponse();
    void removedDemandCannotRestartDuringInvalidationCallback();
    void failureRebaselinesAndCaseInsensitiveRetryAfterPausesAllLanes();
    void unsupportedStockAndUnverifiedBinanceNeverGuessIdentity();
    void stockLaneUsesDedicatedSourceAndStopsBeforeLateCompletion();
    void stockLaneRequiresBrokerWithoutBorrowingAuthentication_data();
    void stockLaneRequiresBrokerWithoutBorrowingAuthentication();
    void whaleOnlyDemandPollsNativeMinuteWithoutC4Warmup();
    void whaleStockDemandNeverCreatesCoinObservations();
    void borrowedCollectorWithoutTradeMetadataKeepsNativeCandleFacts();
    void standaloneWhaleLaneKeepsOnlyClosedProofAndOriginalDecisions();
    void horizontalOnlyDemandUsesExistingRestWithoutC4Warmup();
    void horizontalDemandSurvivesOtherKindRemovalAndCancelsLateReply();
    void stockHorizontalDemandRequiresBrokerAndNeverCreatesObservations();
};
void ChartAlertFeedTest::unsupportedMarketsCannotReachAlertTransport(){
    Http http;ChartAlertFeed feed(&http);feed.setVerifiedUpbitMarkets({},false);feed.setDemands({demand()});feed.setRunning(true);feed.pump();
    QCOMPARE(http.sent.size(),0);QVERIFY(feed.status().contains("확인"));
    feed.setVerifiedUpbitMarkets({"KRW-BTC"},true);feed.pump();QCOMPARE(http.sent.size(),0);QVERIFY(feed.status().contains("지원"));
    Http metadata;ChartInstrumentCatalog catalog(&metadata);catalog.ensure("binance");
    HttpResult r;r.status=200;r.body=QJsonDocument(QJsonObject{{"symbols",QJsonArray{QJsonObject{
        {"symbol","SYNUSDT"},{"baseAsset","SYN"},{"quoteAsset","USDT"},{"status","BREAK"},{"isSpotTradingAllowed",true}}}}}).toJson();
    emit metadata.finished(metadata.token,r);ChartAlertFeed binance(&http,&catalog);auto row=demand();row.identity={"coin","binance","SYNUSDT"};
    binance.setDemands({row});binance.setRunning(true);binance.pump();QCOMPARE(http.sent.size(),0);
}
void ChartAlertFeedTest::marketRevocationCancelsAndRebaselinesAlertLane(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    QSignalSpy started(&feed,&ChartAlertFeed::laneStarted),invalidated(&feed,&ChartAlertFeed::laneInvalidated),observed(&feed,&ChartAlertFeed::observationReady);
    feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.setDemands({demand()});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);
    const auto old=http.token;feed.setVerifiedUpbitMarkets({},false);QCOMPARE(http.aborted,1);QCOMPARE(invalidated.size(),1);
    emit http.finished(old,response());QCOMPARE(observed.size(),0);now+=61000;feed.pump();QCOMPARE(http.sent.size(),1);
    feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(http.sent.size(),2);QCOMPARE(started.size(),2);
    emit http.finished(http.token,response(now));QCOMPARE(observed.size(),1);
    feed.setVerifiedUpbitMarkets({"KRW-BTC"},true);QCOMPARE(invalidated.size(),2);now+=61000;feed.pump();QCOMPARE(http.sent.size(),2);
}
void ChartAlertFeedTest::unsupportedUpbit404StopsAllAlertTimeframes(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    auto second=demand();second.intervalMs=3600000;feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.setDemands({demand(),second});feed.setRunning(true);feed.pump();
    QCOMPARE(http.sent.size(),1);HttpResult missing;missing.status=404;missing.body=R"({"error":{"name":404,"message":"Code not found"}})";
    emit http.finished(http.token,missing);now+=61000;feed.pump();QCOMPARE(http.sent.size(),1);
    feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(http.sent.size(),1);
    feed.setVerifiedUpbitMarkets({"KRW-BTC"},true);feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(http.sent.size(),2);
}
void ChartAlertFeedTest::stockAuthorizationPollingIsQuietAndRecovers(){
    qint64 now=Now;bool authorized=false;Http coins,stocks;StockChartSource source(&stocks,nullptr,[&]{return now;});
    source.setAuthorizationProvider([&](const auto& borrow){if(!authorized)return false;borrow("SYNTHETIC_BEARER",60000,1);return true;});
    ChartAlertFeed feed(&coins,nullptr,nullptr,[&]{return now;});feed.setStockSource(&source);
    auto row=demand();row.identity={"stock","US","SYN"};feed.setDemands({row});feed.setRunning(true);feed.pump();
    QSignalSpy changes(&source,&StockChartSource::changed);
    for(int i=0;i<10;++i){now+=100;source.refresh();feed.pump();}
    QVERIFY2(changes.count()<=1,qPrintable(QString("alert idle changed count: %1").arg(changes.count())));QVERIFY(stocks.sent.isEmpty());QVERIFY(coins.sent.isEmpty());
    authorized=true;now+=100;source.refresh();QCOMPARE(stocks.sent.size(),1);
    changes.clear();HttpResult denied;denied.status=403;emit stocks.finished(stocks.token,denied);QCOMPARE(changes.count(),1);
    now+=100;source.refresh();
    for(int i=0;i<10;++i){now+=100;source.refresh();feed.pump();}
    QCOMPARE(changes.count(),1);QCOMPARE(stocks.sent.size(),1);QVERIFY(coins.sent.isEmpty());
}
void ChartAlertFeedTest::horizontalOnlyDemandUsesExistingRestWithoutC4Warmup(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});auto row=demand();row.pullback=row.results=row.whales=false;row.horizontal=true;
    MarketCandleSeries received;ChartStudySet receivedStudies;int observed=0;connect(&feed,&ChartAlertFeed::observationReady,this,[&](MarketCandleSeries value,ChartStudySet studies,quint64 generation){QVERIFY(generation>0);received=value;receivedStudies=studies;++observed;});
    feed.setDemands({row});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);QVERIFY(MarketCandles::isRequest(http.sent.first()));QVERIFY(!http.sent.first().authenticated);QVERIFY(http.sent.first().body.isEmpty());
    auto result=response();const auto all=QJsonDocument::fromJson(result.body).array();QJsonArray lastThree;for(qsizetype i=all.size()-3;i<all.size();++i)lastThree.append(all[i]);result.body=QJsonDocument(lastThree).toJson();emit http.finished(http.token,result);QCOMPARE(observed,1);QCOMPARE(received.candles.size(),2);QVERIFY(!receivedStudies.pullback.available);QVERIFY(!receivedStudies.similarity.available);QVERIFY(receivedStudies.cloud.isEmpty());QVERIFY(receivedStudies.entries.isEmpty());QCOMPARE(received.receivedAtMs,now);for(const auto& candle:received.candles)QVERIFY(candle.startsAtMs+60000<=now);
}
void ChartAlertFeedTest::horizontalDemandSurvivesOtherKindRemovalAndCancelsLateReply(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});auto mixed=demand();mixed.results=false;mixed.horizontal=true;auto lineOnly=mixed;lineOnly.pullback=false;int observed=0;connect(&feed,&ChartAlertFeed::observationReady,this,[&]{++observed;});
    feed.setDemands({mixed,lineOnly});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);const auto token=http.token;feed.setDemands({lineOnly});QCOMPARE(http.aborted,0);
    auto result=response();const auto all=QJsonDocument::fromJson(result.body).array();QJsonArray lastThree;for(qsizetype i=all.size()-3;i<all.size();++i)lastThree.append(all[i]);result.body=QJsonDocument(lastThree).toJson();emit http.finished(token,result);QCOMPARE(observed,1);feed.setDemands({});now+=90000;feed.pump();emit http.finished(token,result);QCOMPARE(observed,1);QCOMPARE(http.sent.size(),1);QVERIFY(feed.scheduledIdentities().isEmpty());
}
void ChartAlertFeedTest::stockHorizontalDemandRequiresBrokerAndNeverCreatesObservations(){
    qint64 now=Now;Http coins,stocks;StockChartSource source(&stocks,nullptr,[&]{return now;});
    ChartAlertFeed feed(&coins,nullptr,nullptr,[&]{return now;});feed.setStockSource(&source);
    auto row=demand();row.identity={"stock","KR","005930"};row.pullback=row.results=row.whales=false;row.horizontal=true;
    int observed=0,begun=0;connect(&feed,&ChartAlertFeed::laneStarted,this,[&]{++begun;});
    connect(&feed,&ChartAlertFeed::stockObservationReady,this,[&]{++observed;});
    feed.setDemands({row});feed.setRunning(true);feed.pump();
    QVERIFY(source.active());QVERIFY(coins.sent.isEmpty());QVERIFY(stocks.sent.isEmpty());
    QCOMPARE(source.snapshot().series.reason,QStringLiteral("연결된 증권사가 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QVERIFY(!source.snapshot().series.available);QVERIFY(!source.snapshot().realtimeAllowed);
    QCOMPARE(observed,0);QCOMPARE(begun,0);
    now+=60000;feed.pump();QVERIFY(stocks.sent.isEmpty());QCOMPARE(observed,0);
    feed.setRunning(false);QVERIFY(!source.active());
}
void ChartAlertFeedTest::whaleOnlyDemandPollsNativeMinuteWithoutC4Warmup(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    auto row=demand();row.pullback=false;row.results=false;row.whales=true;
    feed.setDemands({row});int observed=0;CoinFlowSnapshot received;
    connect(&feed,&ChartAlertFeed::coinFlowObservationReady,this,[&](CoinFlowSnapshot value,quint64 generation){
        QVERIFY(generation>0);received=value;++observed;
    });
    feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);
    const auto request=http.sent.first();QVERIFY(MarketCandles::isRequest(request));QVERIFY(request.url.path().endsWith("/1"));
    auto result=response();const auto rows=QJsonDocument::fromJson(result.body).array();QJsonArray shortRows;
    for(qsizetype i=rows.size()-5;i<rows.size();++i)shortRows.append(rows[i]);result.body=QJsonDocument(shortRows).toJson();
    emit http.finished(http.token,result);QCOMPARE(observed,1);QCOMPARE(received.identity,row.identity.key());
    QCOMPARE(received.minutes.size(),4);QVERIFY(received.primary.trades.isEmpty());
    for(const auto& minute:received.minutes){QVERIFY(minute.completesAtMs<=now);QVERIFY(minute.knownAtMs==now);QVERIFY(!minute.liveEligible);}
    const auto studies=coinFlowStudies(received,now);QVERIFY(!studies.taker.state.available);QVERIFY(!studies.openInterest.state.available);
}
void ChartAlertFeedTest::whaleStockDemandNeverCreatesCoinObservations(){
    Http http;ChartAlertFeed feed(&http,nullptr,nullptr,[]{return Now;});
    auto row=demand();row.identity={"stock","US","SYN"};row.pullback=false;row.results=false;row.whales=true;
    int observed=0;connect(&feed,&ChartAlertFeed::coinFlowObservationReady,this,[&]{++observed;});
    feed.setDemands({row});feed.setRunning(true);feed.pump();QVERIFY(http.sent.isEmpty());QCOMPARE(observed,0);
}
void ChartAlertFeedTest::borrowedCollectorWithoutTradeMetadataKeepsNativeCandleFacts(){
    Http http,flowHttp;qint64 now=Now;CoinFlowSource source(&flowHttp,{},{},nullptr,[&]{return now;});
    source.setSelection("coin:upbit:KRW-SYN");source.setActive(true);
    ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});feed.setCoinFlowSource(&source);
    auto row=demand();row.pullback=false;row.results=false;row.whales=true;feed.setDemands({row});
    CoinFlowSnapshot value;int observed=0;connect(&feed,&ChartAlertFeed::coinFlowObservationReady,this,[&](CoinFlowSnapshot result,quint64){value=result;++observed;});
    feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);emit http.finished(http.token,response());QVERIFY(observed>0);
    QVERIFY(value.candleSeries.has_value());QCOMPARE(value.candleSeries->identity,row.identity.key());QCOMPARE(value.candleSeries->asset,QString("SYN"));
    QCOMPARE(value.candleSeries->currency,QString("KRW"));QVERIFY(!value.primary.instrument.verified);
    const auto studies=coinFlowStudies(value,now);QVERIFY(studies.volume.state.available);QVERIFY(!studies.largeTrade.state.available);
    QVERIFY(!studies.taker.state.available);QVERIFY(!studies.openInterest.state.available);
}
void ChartAlertFeedTest::standaloneWhaleLaneKeepsOnlyClosedProofAndOriginalDecisions(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    auto row=demand();row.pullback=false;row.results=false;row.whales=true;feed.setDemands({row});
    CoinFlowSnapshot value;connect(&feed,&ChartAlertFeed::coinFlowObservationReady,this,[&](CoinFlowSnapshot result,quint64){value=result;});
    feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);emit http.finished(http.token,response(now));
    QVERIFY(value.candleSeries.has_value());QVERIFY2(!value.candleSeries->forming.has_value(),"Queued whale lanes must pass only actual completed native candles to independent studies");
    QVERIFY(coinFlowStudies(value,now).volume.state.available);const auto first=value.minutes.last();
    now+=60000;feed.pump();QCOMPARE(http.sent.size(),2);auto result=response(now);auto rows=QJsonDocument::fromJson(result.body).array();
    auto latest=rows[rows.size()-2].toObject();latest["candle_acc_trade_volume"]="4";rows[rows.size()-2]=latest;result.body=QJsonDocument(rows).toJson();
    emit http.finished(http.token,result);QVERIFY(value.minutes.last().liveEligible);
    const auto old=std::find_if(value.minutes.cbegin(),value.minutes.cend(),[&](const auto& minute){return minute.candle.startsAtMs==first.candle.startsAtMs;});
    QVERIFY(old!=value.minutes.cend());QCOMPARE(old->knownAtMs,first.knownAtMs);QCOMPARE(old->decisionAtMs,first.decisionAtMs);
    const auto studies=coinFlowStudies(value,now);QVERIFY(studies.volume.state.available);QCOMPARE(studies.volume.multiple.value_or(-1),2.0);
    QVERIFY(std::any_of(studies.entries.cbegin(),studies.entries.cend(),[](const auto& entry){return entry.kind==CoinFlowSignalKind::VolumeBurst;}));
}
void ChartAlertFeedTest::stockLaneRequiresBrokerWithoutBorrowingAuthentication_data(){
    QTest::addColumn<QString>("country");QTest::addColumn<QString>("code");
    QTest::newRow("domestic")<<QString("KR")<<QString("005930");
    QTest::newRow("us")<<QString("US")<<QString("SYN");
}
void ChartAlertFeedTest::stockLaneRequiresBrokerWithoutBorrowingAuthentication(){
    QFETCH(QString,country);QFETCH(QString,code);
    qint64 now=Now;Http coins,stocks;StockChartSource source(&stocks,nullptr,[&]{return now;});
    ChartAlertFeed feed(&coins,nullptr,nullptr,[&]{return now;});feed.setStockSource(&source);
    auto stock=demand();stock.identity={"stock",country,code};feed.setDemands({stock});
    int observations=0,begun=0;connect(&feed,&ChartAlertFeed::laneStarted,this,[&]{++begun;});
    connect(&feed,&ChartAlertFeed::stockObservationReady,this,[&]{++observations;});
    feed.setRunning(true);feed.pump();
    QVERIFY(source.active());QVERIFY(stocks.sent.isEmpty());QVERIFY(coins.sent.isEmpty());
    QCOMPARE(source.snapshot().series.reason,QStringLiteral("연결된 증권사가 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QVERIFY(!source.snapshot().series.available);QVERIFY(!source.snapshot().realtimeAllowed);
    QCOMPARE(observations,0);QCOMPARE(begun,0);
    now+=60000;feed.pump();QVERIFY(stocks.sent.isEmpty());QCOMPARE(observations,0);
    feed.setDemands({});QVERIFY(!source.active());QVERIFY(feed.scheduledIdentities().isEmpty());
}
void ChartAlertFeedTest::stockLaneUsesDedicatedSourceAndStopsBeforeLateCompletion(){
    Http coins,stocks;qint64 now=Now;
    StockChartSource source(&stocks,nullptr,[&]{return now;});source.setAuthorizationProvider([](const auto& cb){cb("SYNTHETIC_BEARER",60000,1);return true;});
    ChartAlertFeed feed(&coins,nullptr,nullptr,[&]{return now;});feed.setStockSource(&source);
    auto stock=demand();stock.identity={"stock","US","SYN"};feed.setDemands({demand(),stock});feed.setPriorityIdentities({stock.identity.key()});
    int stockEvents=0;connect(&feed,&ChartAlertFeed::stockObservationReady,this,[&]{++stockEvents;});
    feed.setRunning(true);feed.pump();QCOMPARE(stocks.sent.size(),1);QVERIFY(source.active());QVERIFY(coins.sent.isEmpty());
    const auto late=stocks.token;feed.setDemands({});QVERIFY(!source.active());QCOMPARE(stocks.aborted,1);
    emit stocks.finished(late,response());QCOMPARE(stockEvents,0);feed.pump();QCOMPARE(stocks.sent.size(),1);
}
void ChartAlertFeedTest::dedicatedPublicPollsUseOnlyEnabledDemandAndClosedBars(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    QCOMPARE(http.sent.size(),0);feed.setDemands({demand()});QCOMPARE(http.sent.size(),0);
    int begun=0,observed=0;quint64 generation=0;MarketCandleSeries received;ChartStudySet studies;
    connect(&feed,&ChartAlertFeed::laneStarted,this,[&](QString key,qint64 interval,quint64 g,bool allowed){QCOMPARE(key,QString("coin:upbit:KRW-SYN"));QCOMPARE(interval,60000);QVERIFY(allowed);generation=g;++begun;});
    connect(&feed,&ChartAlertFeed::observationReady,this,[&](MarketCandleSeries s,ChartStudySet c,quint64 g){QCOMPARE(g,generation);received=s;studies=c;++observed;});
    feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);QCOMPARE(begun,1);
    const auto request=http.sent.first();QVERIFY(MarketCandles::isRequest(request));QVERIFY(!request.authenticated);QVERIFY(request.body.isEmpty());
    emit http.finished(http.token,response());QCOMPARE(observed,1);QVERIFY(received.forming.has_value());QCOMPARE(received.candles.size(),30);
    QCOMPARE(studies.observations,30);QCOMPARE(studies.volumes.size(),30);QCOMPARE(studies.lastCompletedAtMs,now/60000*60000);
    now+=1000;feed.pump();QCOMPARE(http.sent.size(),1);
    feed.setRunning(false);QVERIFY(!feed.running());now+=60000;feed.pump();QCOMPARE(http.sent.size(),1);
}
void ChartAlertFeedTest::visiblePriorityAndGlobalPacingKeepOtherListsQueued(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    feed.setDemands({demand("KRW-OTHER"),demand()});feed.setPriorityIdentities({"coin:upbit:KRW-SYN"});feed.setRunning(true);feed.pump();
    QCOMPARE(http.sent.size(),1);QVERIFY(http.sent.first().url.toString().contains("KRW-SYN"));emit http.finished(http.token,response());
    feed.pump();QCOMPARE(http.sent.size(),1);now+=350;feed.pump();QCOMPARE(http.sent.size(),2);QVERIFY(http.sent.last().url.toString().contains("KRW-OTHER"));
    const auto scheduled=feed.scheduledIdentities();QVERIFY(scheduled.contains("coin:upbit:KRW-SYN"));QVERIFY(scheduled.contains("coin:upbit:KRW-OTHER"));
}
void ChartAlertFeedTest::removingPrivateDemandCancelsAndRejectsLateResponse(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});int observed=0;
    connect(&feed,&ChartAlertFeed::observationReady,this,[&]{++observed;});
    feed.setDemands({demand("KRW-SYN",true)});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);const auto late=http.token;
    feed.setDemands({});QCOMPARE(http.aborted,1);QVERIFY(feed.scheduledIdentities().isEmpty());emit http.finished(late,response());QCOMPARE(observed,0);
}
void ChartAlertFeedTest::failureRebaselinesAndCaseInsensitiveRetryAfterPausesAllLanes(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});int begun=0,invalid=0;
    connect(&feed,&ChartAlertFeed::laneStarted,this,[&]{++begun;});connect(&feed,&ChartAlertFeed::laneInvalidated,this,[&]{++invalid;});
    feed.setDemands({demand()});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);
    HttpResult limited;limited.status=429;limited.headers.insert("retry-after","60");emit http.finished(http.token,limited);QCOMPARE(invalid,1);
    now+=59999;feed.pump();QCOMPARE(http.sent.size(),1);now+=1;feed.pump();QCOMPARE(http.sent.size(),2);QCOMPARE(begun,2);
}
void ChartAlertFeedTest::removedDemandCannotRestartDuringInvalidationCallback(){
    Http http;qint64 now=Now;ChartAlertFeed feed(&http,nullptr,nullptr,[&]{return now;});
    feed.setDemands({demand("KRW-SYN",true)});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),1);
    connect(&feed,&ChartAlertFeed::laneInvalidated,this,[&]{now+=350;feed.pump();});
    feed.setDemands({});QCOMPARE(http.sent.size(),1);QVERIFY(feed.scheduledIdentities().isEmpty());
}
void ChartAlertFeedTest::unsupportedStockAndUnverifiedBinanceNeverGuessIdentity(){
    Http http;ChartAlertFeed feed(&http,nullptr,nullptr,[]{return Now;});
    auto stock=demand();stock.identity={"stock","US","SYN"};auto binance=demand();binance.identity={"coin","binance","SYNUSDT"};
    feed.setDemands({stock,binance});feed.setRunning(true);feed.pump();QCOMPARE(http.sent.size(),0);QVERIFY(!feed.status().isEmpty());
}
QTEST_GUILESS_MAIN(ChartAlertFeedTest)
#include "tst_chart_alert_feed.moc"
