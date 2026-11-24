#include "keefi/MarketCandles.h"
#include "keefi/ChartMarketResolver.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QLocale>
#include <QUrlQuery>
#include <QTest>
#include <QSignalSpy>
using namespace keefi;
namespace {
constexpr qint64 Now=1792598400000;
QJsonObject candle(qint64 start, QString close="100") {
    return {{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(start,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(start+1000)},{"opening_price","100"},{"high_price","110"},{"low_price","90"},
        {"trade_price",close},{"candle_acc_trade_volume","2"},{"unit","15"}};
}
HttpResult response(QJsonArray rows){HttpResult r;r.status=200;r.body=QJsonDocument(rows).toJson();return r;}
QJsonObject upbitCandle(qint64 start,qint64 interval,QString close="105"){
    auto row=candle(start,close);
    if(interval==86400000)row.remove("unit");else row["unit"]=QString::number(interval/60000);
    return row;
}
QJsonArray binanceCandle(qint64 start,qint64 interval){
    return {QString::number(start),"100","110","90","105","2",QString::number(start+interval-1),"210","5","1","105","0"};
}
MarketInstrument binanceInstrument(){
    MarketInstrument item;item.symbol="SYNUSDT";item.baseAsset="SYN";item.quoteAsset="USDT";
    item.market="spot";item.status="TRADING";item.tradable=true;return item;
}
class CandleHttp final:public TradingTransport {
  public:
    QList<quint64> tokens;QList<RequestSpec> requests;int aborts=0;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{++aborts;}
};
class CandleSocket final:public PublicCandleSocket {
  public:
    QList<quint64> generations;QList<QUrl> endpoints;QList<QByteArray> messages;int aborts=0;
    void open(quint64 generation,const QUrl& endpoint)override{generations.append(generation);endpoints.append(endpoint);}
    void send(quint64,const QByteArray& message)override{messages.append(message);}
    void abort()override{++aborts;}
};
QByteArray upbitFrame(qint64 start,qint64 source,QString close="105",QString volume="2"){
    auto row=upbitCandle(start,60000,close);row.remove("market");row.remove("unit");
    row["type"]="candle.1m";row["code"]="KRW-BTC";row["timestamp"]=QString::number(source);
    row["candle_acc_trade_volume"]=volume;row["stream_type"]="REALTIME";
    return QJsonDocument(row).toJson();
}
QByteArray binanceFrame(qint64 start,qint64 event,bool closed,QString close="104",QString volume="7"){
    const QJsonObject kline{{"t",QString::number(start)},{"T",QString::number(start+59999)},
        {"s","SYNUSDT"},{"i","1m"},{"o","100"},{"h","110"},{"l","90"},{"c",close},
        {"v",volume},{"n","5"},{"x",closed},{"q","210"},{"V","1"},{"Q","105"},{"B","0"}};
    return QJsonDocument(QJsonObject{{"e","kline"},{"E",QString::number(event)},{"s","SYNUSDT"},{"k",kline}}).toJson();
}
}
class MarketCandlesTest final:public QObject{
    Q_OBJECT
  private slots:
    void historyRequestsAreCanonicalAndBounded();
    void fivePagesRemainSeparateFromLiveEvidence_data();
    void fivePagesRemainSeparateFromLiveEvidence();
    void historyRetriesDoNotEraseLiveCandles_data();
    void historyRetriesDoNotEraseLiveCandles();
    void lateHistoryReplyCannotPolluteNewSelection();
    void emptyHistoryAndMissingIntervalsAreNotInvented();
    void historyReceiptNeverRefreshesLatestEvidence();
    void revisitingUsesOnlyCompletedDisplayHistory();
    void displayCacheRetainsEightMostRecentlyUsedSelections();
    void displayCacheAgeUsesLastCompletedBoundary_data();
    void displayCacheAgeUsesLastCompletedBoundary();
    void lockPurgesDisplayHistoryAndRevokesOldCallbacks();
    void confirmedMetadataRemovalRevokesInactiveCachedSelections();
    void returningToTheSameIdentityRejectsEarlierGenerations();
    void retryAfterSurvivesSelectionHideAndLockPerProvider();
    void archiveHistoryNeverEntersTheMemoryLru();
    void displayHistoryNeverBecomesCurrentData_data();
    void displayHistoryNeverBecomesCurrentData();
    void formingCandleNeverEntersCompletedStudies();
    void usesActualClosedCandlesAndLeavesMissingIntervals();
    void storedTickMayFollowTheCandleBoundary();
    void restReceiptsKeepDistinctIdsEvenAtTheSameClock();
    void compressionIsDefinedAndNotADirectionalSignal();
    void invalidCandlesClearTheEntireSeries_data();
    void invalidCandlesClearTheEntireSeries();
    void incompleteOrGappedStudyHasNoSignal();
    void nonconstantStudyMatchesIndependentDecimalCalculation();
    void modelDropsFailuresFutureResponsesAndLateReplies();
    void nativeTimeframesKeepProviderIdentityAndUtcBoundaries_data();
    void nativeTimeframesKeepProviderIdentityAndUtcBoundaries();
    void restCompletionUsesRequestStartAcrossTheBoundary();
    void binanceNeedsVerifiedSpotMetadataAndExactTwelveFields();
    void verifiedUnicodeIdentifiersReachNativeCandleRequests();
    void compressionUsesTheSelectedInterval();
    void upbitSameSourceTimestampUsesLatestArrivalWithoutCompletingTheBar();
    void binanceApplicationHistoryBudgetRejectsMoreThanTwoHundredRows();
    void binanceClockLeadWaitsForRealTime_data();
    void binanceClockLeadWaitsForRealTime();
    void binanceDeferredPacketIsDroppedOnSelectionChange();
    void binanceDeferredPacketCoalescesAndPreservesOriginalReceipt();
    void binanceDeferredPacketDoesNotSurviveClockRollback();
    void binanceStreamUsesTheAuthoritativeCloseFlagAndLeavesGaps();
    void reconnectClearsUntilRestSeedAndRejectsOldGenerations();
    void delayedBootstrapCannotRollBackNewerFormingData();
    void restTimeoutAndRetryAfterUseTheInjectedClock();
    void lowercaseHttpDateRetryAfterDoesNotRetryBeforeTheServerDeadline();
    void metadataArrivalAndExpiryCannotRestoreAnOldSelection();
    void upbitDailyStreamOnlyTriggersNativeDailyRest();
    void actualPublicCandles();
    void verifiedMarketRoutingUsesExactOfficialIdentity();
    void missingUpbitMetadataCannotBeMistakenForAnAbsentMarket();
    void metadataRefreshPreservesLastVerifiedCompletedObservation();
    void binanceMetadataPauseRetainsHistoryButConfirmedRemovalRevokesIt();
    void unknownOrRevokedUpbitMarketsNeverSendRequests();
    void unsupportedUpbitResponseStopsRetriesUntilMetadataChanges();
};
void MarketCandlesTest::historyRequestsAreCanonicalAndBounded(){
    const qint64 before=Now/900000*900000;
    const auto upbit=MarketCandles::historyRequest("coin:upbit:KRW-BTC",900000,before);
    QVERIFY(MarketCandles::isRequest(upbit));QUrlQuery uq(upbit.url);
    QCOMPARE(uq.queryItemValue("count"),QString("200"));
    QCOMPARE(uq.queryItemValue("to"),QDateTime::fromMSecsSinceEpoch(before,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss'Z'"));
    const auto binance=MarketCandles::historyRequest("coin:binance:BTCUSDT",900000,before);
    QVERIFY(MarketCandles::isRequest(binance));QUrlQuery bq(binance.url);
    QCOMPARE(bq.queryItemValue("limit"),QString("200"));QCOMPARE(bq.queryItemValue("endTime"),QString::number(before-1));
    QVERIFY(MarketCandles::historyRequest("coin:upbit:KRW-BTC",900000,before+1).url.isEmpty());
    QVERIFY(MarketCandles::historyRequest("coin:upbit:KRW-BTC",0,before).url.isEmpty());
    auto tampered=upbit;QUrlQuery tq(tampered.url);tq.addQueryItem("to",uq.queryItemValue("to"));tampered.url.setQuery(tq);QVERIFY(!MarketCandles::isRequest(tampered));
    tampered=binance;tampered.authenticated=true;QVERIFY(!MarketCandles::isRequest(tampered));
    tampered=binance;QUrlQuery large(tampered.url);large.removeAllQueryItems("limit");large.addQueryItem("limit","1001");tampered.url.setQuery(large);QVERIFY(!MarketCandles::isRequest(tampered));
}
void MarketCandlesTest::fivePagesRemainSeparateFromLiveEvidence_data(){
    QTest::addColumn<bool>("binance");QTest::newRow("upbit-btc-krw-15m-1000")<<false;QTest::newRow("binance-btc-usdt-15m-1000")<<true;
}
void MarketCandlesTest::fivePagesRemainSeparateFromLiveEvidence(){
    QFETCH(bool,binance);CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});
    const qint64 interval=900000,end=Now/interval*interval;const QString identity=binance?"coin:binance:BTCUSDT":"coin:upbit:KRW-BTC";
    if(binance){auto item=binanceInstrument();item.symbol="BTCUSDT";item.baseAsset="BTC";model.setInstruments("binance",{item});}
    model.setSelection(identity,interval);
    const auto page=[&](int index){QJsonArray rows;for(int i=200;i>0;--i){const auto at=end-(index*200+i)*interval;
        if(binance)rows.append(binanceCandle(at,interval));else rows.append(upbitCandle(at,interval));}return response(rows);};
    emit http.finished(http.tokens.last(),page(0));const auto live=model.series(),rest=model.restObservation();QCOMPARE(live.candles.size(),200);
    for(int pageIndex=1;pageIndex<5;++pageIndex){
        clock+=1100;const auto requestCount=http.requests.size();model.requestOlder();model.requestOlder();
        QCOMPARE(http.requests.size(),requestCount+1);QCOMPARE(model.historyStatus().state,CandleHistoryState::Loading);
        const auto cursor=end-pageIndex*200*interval;
        QCOMPARE(http.requests.last().url,MarketCandles::historyRequest(identity,interval,cursor).url);
        const auto token=http.tokens.last();emit http.finished(token,page(pageIndex));emit http.finished(token,page(pageIndex));
        QCOMPARE(model.displayObservation().candles.size(),(pageIndex+1)*200);QCOMPARE(model.series().candles.size(),200);
        QCOMPARE(model.series().receivedAtMs,live.receivedAtMs);QCOMPARE(model.restObservation().restRevision,rest.restRevision);
        QCOMPARE(model.historyStatus().state,CandleHistoryState::Ready);
    }
    const auto shown=model.displayObservation();QCOMPARE(shown.candles.first().startsAtMs,end-1000*interval);
    QCOMPARE(shown.candles.last().startsAtMs,end-interval);QCOMPARE(shown.sourceUrl,MarketCandles::request(identity,interval).url.toString());
    QSet<qint64> unique;for(const auto& row:shown.candles)unique.insert(row.startsAtMs);QCOMPARE(unique.size(),1000);
    model.clearDisplayCache();QCOMPARE(model.displayObservation().candles.size(),200);QCOMPARE(model.historyStatus().completedCount,0);
}
void MarketCandlesTest::historyRetriesDoNotEraseLiveCandles_data(){
    QTest::addColumn<int>("limitedStatus");QTest::newRow("429-rate-limit")<<429;QTest::newRow("418-ip-ban")<<418;
}
void MarketCandlesTest::historyRetriesDoNotEraseLiveCandles(){
    QFETCH(int,limitedStatus);CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});model.setAsset("BTC");
    emit http.finished(http.tokens.last(),response({candle(Now-2*900000),candle(Now-900000)}));
    const auto live=model.series();clock+=1100;model.requestOlder();const auto attempted=http.requests.size();
    HttpResult limited;limited.status=limitedStatus;limited.headers.insert("Retry-After","60");emit http.finished(http.tokens.last(),limited);
    QVERIFY(model.series().available);QCOMPARE(model.series().receivedAtMs,live.receivedAtMs);QCOMPARE(model.displayObservation().candles.size(),2);
    QCOMPARE(model.historyStatus().state,CandleHistoryState::RateLimited);model.requestOlder();QCOMPARE(http.requests.size(),attempted);
    clock+=59000;model.requestOlder();QCOMPARE(http.requests.size(),attempted);
    clock+=1001;model.requestOlder();QCOMPARE(http.requests.size(),attempted+1);
    emit http.finished(http.tokens.last(),response({candle(Now-3*900000)}));QCOMPARE(model.displayObservation().candles.size(),3);
    // The same/overlapping cursor is rejected, not silently reported as an empty end.
    clock+=1100;model.requestOlder();emit http.finished(http.tokens.last(),response({candle(Now-3*900000)}));
    QCOMPARE(model.historyStatus().state,CandleHistoryState::Delayed);QCOMPARE(model.displayObservation().candles.size(),3);
    QVERIFY(model.series().available);
}
void MarketCandlesTest::lateHistoryReplyCannotPolluteNewSelection(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});model.setAsset("BTC");
    emit http.finished(http.tokens.last(),response({candle(Now-900000)}));clock+=1100;model.requestOlder();const auto old=http.tokens.last();
    model.setSelection("coin:upbit:KRW-BTC",60000);const auto current=http.tokens.last();
    emit http.finished(old,response({candle(Now-1800000)}));QVERIFY(model.displayObservation().candles.isEmpty());
    emit http.finished(current,response({upbitCandle(Now-60000,60000)}));QCOMPARE(model.displayObservation().candles.size(),1);
    QCOMPARE(model.displayObservation().intervalMs,qint64(60000));
    clock+=1100;model.requestOlder();const auto hidden=http.tokens.last();model.setSelection({},60000);
    emit http.finished(hidden,response({upbitCandle(Now-120000,60000)}));QVERIFY(model.displayObservation().candles.isEmpty());QCOMPARE(model.historyStatus().completedCount,0);
}
void MarketCandlesTest::emptyHistoryAndMissingIntervalsAreNotInvented(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});model.setAsset("BTC");
    emit http.finished(http.tokens.last(),response({candle(Now-900000)}));clock+=1100;model.requestOlder();
    emit http.finished(http.tokens.last(),response({candle(Now-4*900000),candle(Now-6*900000)}));
    QCOMPARE(model.displayObservation().candles.size(),3);QCOMPARE(model.historyStatus().state,CandleHistoryState::Ready);
    clock+=1100;model.requestOlder();emit http.finished(http.tokens.last(),response({}));
    QCOMPARE(model.historyStatus().state,CandleHistoryState::End);const auto count=http.requests.size();model.requestOlder();QCOMPARE(http.requests.size(),count);
    QCOMPARE(model.displayObservation().candles.size(),3);
    model.setVerifiedUpbitMarkets({},true);QVERIFY(model.displayObservation().candles.isEmpty());QCOMPARE(model.historyStatus().completedCount,0);
}
void MarketCandlesTest::historyReceiptNeverRefreshesLatestEvidence(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});model.setAsset("BTC");
    emit http.finished(http.tokens.last(),response({candle(Now-900000)}));const auto received=model.series().receivedAtMs;
    clock+=1100;model.requestOlder();clock+=MarketCandles::FreshnessMs;
    emit http.finished(http.tokens.last(),response({candle(Now-1800000)}));
    QVERIFY(!model.series().available);QVERIFY(model.series().candles.isEmpty());
    const auto shown=model.displayObservation();QVERIFY(shown.delayed);QVERIFY(!shown.available);QCOMPARE(shown.candles.size(),2);
    QCOMPARE(shown.receivedAtMs,received);QCOMPARE(model.restObservation().receivedAtMs,received);
}
void MarketCandlesTest::revisitingUsesOnlyCompletedDisplayHistory(){
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();qint64 clock=Now+10000;
    MarketCandles model(&http,std::move(socket),[&]{return clock;});
    const QString btc="coin:upbit:KRW-BTC";model.setSelection(btc,60000);
    const auto epoch=wire->generations.last();emit wire->connected(epoch);
    emit http.finished(http.tokens.last(),response({upbitCandle(Now-120000,60000),upbitCandle(Now-60000,60000),upbitCandle(Now,60000)}));
    const auto original=model.series();QVERIFY(original.available);QVERIFY(original.forming);QCOMPARE(original.candles.size(),2);
    model.setSelection("coin:upbit:KRW-OTHER",60000);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection(btc,60000);
    const auto cached=model.displayObservation();
    QCOMPARE(cached.identity,btc);QCOMPARE(cached.currency,QString("KRW"));QCOMPARE(cached.intervalMs,qint64(60000));
    QCOMPARE(cached.candles.size(),2);QVERIFY(cached.fromMemoryCache);QVERIFY(cached.delayed);
    QVERIFY(!cached.available);QVERIFY(!cached.forming);QVERIFY(!cached.fromArchive);
    QCOMPARE(cached.receivedAtMs,original.receivedAtMs);QVERIFY(!model.series().available);
    QVERIFY(model.series().candles.isEmpty());QVERIFY(model.restObservation().candles.isEmpty());
    model.setSelection(btc,300000);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection("coin:upbit:USDT-BTC",60000);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection({},60000);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection(btc,60000);QVERIFY(model.displayObservation().fromMemoryCache);
    auto latest=upbitCandle(Now-60000,60000,"109");
    emit http.finished(http.tokens.last(),response({latest}));
    QVERIFY(model.series().available);QVERIFY(!model.displayObservation().fromMemoryCache);
    QCOMPARE(model.displayObservation().candles.last().close,QString("109"));
}
void MarketCandlesTest::displayCacheRetainsEightMostRecentlyUsedSelections(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});
    const auto key=[](int i){return QString("coin:upbit:KRW-S%1").arg(i);};
    const auto seed=[&](int i){
        model.setSelection(key(i),60000);QJsonArray rows;
        for(int j=200;j>0;--j){auto row=upbitCandle(Now-j*60000,60000);row["market"]=QString("KRW-S%1").arg(i);rows.append(row);}
        emit http.finished(http.tokens.last(),response(rows));QCOMPARE(model.series().candles.size(),200);
    };
    for(int i=0;i<8;++i)seed(i);
    model.setSelection(key(0),60000);QVERIFY(model.displayObservation().fromMemoryCache); // make S0 most recently used
    seed(8);
    model.setSelection(key(1),60000);QVERIFY(model.displayObservation().candles.isEmpty()); // ninth insertion evicted S1
    model.setSelection(key(0),60000);QCOMPARE(model.displayObservation().candles.size(),200);
    QVERIFY(model.displayObservation().fromMemoryCache);QVERIFY(!model.series().available);
    model.setSelection(key(2),60000);QVERIFY(model.displayObservation().fromMemoryCache);
}
void MarketCandlesTest::displayCacheAgeUsesLastCompletedBoundary_data(){
    QTest::addColumn<qint64>("interval");
    QTest::newRow("minute-day-budget")<<qint64(60000);
    QTest::newRow("daily-three-bar-budget")<<qint64(86400000);
}
void MarketCandlesTest::displayCacheAgeUsesLastCompletedBoundary(){
    QFETCH(qint64,interval);const auto end=Now/interval*interval;qint64 clock=end+10000;
    CandleHttp http;MarketCandles model(&http,{},[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",interval);
    emit http.finished(http.tokens.last(),response({upbitCandle(end-interval,interval)}));
    QVERIFY(model.series().available);model.setSelection({},interval);
    clock=end+qMax<qint64>(86400000,3*interval);
    model.setSelection("coin:upbit:KRW-BTC",interval);QVERIFY(model.displayObservation().fromMemoryCache);
    model.setSelection({},interval);++clock;model.setSelection("coin:upbit:KRW-BTC",interval);
    QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection({},interval);clock=end+10000;model.setSelection("coin:upbit:KRW-BTC",interval);
    emit http.finished(http.tokens.last(),response({upbitCandle(end-interval,interval)}));QVERIFY(model.series().available);
    model.setSelection({},interval);clock=end+9999;model.setSelection("coin:upbit:KRW-BTC",interval);
    QVERIFY(model.displayObservation().candles.isEmpty()); // future receipt cannot be revived by a clock rollback
}
void MarketCandlesTest::lockPurgesDisplayHistoryAndRevokesOldCallbacks(){
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();qint64 clock=Now+10000;
    MarketCandles model(&http,std::move(socket),[&]{return clock;});const QString btc="coin:upbit:KRW-BTC";
    model.setSelection(btc,60000);const auto oldToken=http.tokens.last(),oldEpoch=wire->generations.last();emit wire->connected(oldEpoch);
    emit http.finished(oldToken,response({upbitCandle(Now-60000,60000),upbitCandle(Now,60000)}));
    const auto before=model.series();QVERIFY(before.available);QVERIFY(before.forming);
    model.clearDisplayCache();
    const auto retained=model.series();QVERIFY(retained.available);QVERIFY(retained.forming);
    QCOMPARE(retained.identity,btc);QCOMPARE(retained.candles.last().close,before.candles.last().close);
    QVERIFY(wire->generations.last()!=oldEpoch);QVERIFY(http.tokens.last()!=oldToken);
    emit wire->received(oldEpoch,upbitFrame(Now,clock,"109"));emit http.finished(oldToken,response({upbitCandle(Now-60000,60000,"109")}));
    QCOMPARE(model.series().candles.last().close,before.candles.last().close);
    model.setSelection({},60000);model.setSelection(btc,60000);
    QVERIFY(model.displayObservation().candles.isEmpty()); // leaving the preserved current public view cannot restore the purged history
    emit http.finished(oldToken,response({upbitCandle(Now-60000,60000,"109")}));
    QVERIFY(model.displayObservation().candles.isEmpty());
    emit http.finished(http.tokens.last(),response({upbitCandle(Now-60000,60000,"108")}));
    model.setSelection({},60000);model.setSelection(btc,60000);
    QCOMPARE(model.displayObservation().candles.last().close,QString("108")); // only a new generation can repopulate it
}
void MarketCandlesTest::confirmedMetadataRemovalRevokesInactiveCachedSelections(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});
    model.setVerifiedUpbitMarkets({"KRW-BTC","KRW-OTHER"},true);model.setSelection("coin:upbit:KRW-BTC",60000);
    emit http.finished(http.tokens.last(),response({upbitCandle(Now-60000,60000)}));
    model.setSelection("coin:upbit:KRW-OTHER",60000);
    model.setVerifiedUpbitMarkets({"KRW-OTHER"},true);
    model.setVerifiedUpbitMarkets({"KRW-BTC","KRW-OTHER"},true);
    model.setSelection("coin:upbit:KRW-BTC",60000);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setInstruments("binance",{binanceInstrument()});model.setSelection("coin:binance:SYNUSDT",60000);
    QJsonArray rows;rows.append(binanceCandle(Now-60000,60000));emit http.finished(http.tokens.last(),response(rows));
    model.setSelection("coin:upbit:KRW-BTC",60000);auto halted=binanceInstrument();halted.status="BREAK";halted.tradable=false;
    model.setInstruments("binance",{halted});model.setInstruments("binance",{binanceInstrument()});
    model.setSelection("coin:binance:SYNUSDT",60000);QVERIFY(model.displayObservation().candles.isEmpty());
}
void MarketCandlesTest::returningToTheSameIdentityRejectsEarlierGenerations(){
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();qint64 clock=Now+10000;
    MarketCandles model(&http,std::move(socket),[&]{return clock;});const QString btc="coin:upbit:KRW-BTC";
    model.setSelection(btc,60000);const auto oldToken=http.tokens.last(),oldEpoch=wire->generations.last();emit wire->connected(oldEpoch);
    model.setSelection("coin:upbit:KRW-OTHER",60000);const auto otherToken=http.tokens.last();
    model.setSelection(btc,60000);const auto newToken=http.tokens.last(),newEpoch=wire->generations.last();emit wire->connected(newEpoch);
    emit http.finished(oldToken,response({upbitCandle(Now-60000,60000,"109")}));
    emit http.finished(otherToken,response({upbitCandle(Now-60000,60000,"109")}));
    emit wire->received(oldEpoch,upbitFrame(Now,clock,"109"));
    QVERIFY(model.displayObservation().candles.isEmpty());QVERIFY(!model.series().forming);
    emit http.finished(newToken,response({upbitCandle(Now-60000,60000,"101")}));
    QVERIFY(model.series().available);QCOMPARE(model.series().candles.last().close,QString("101"));
    model.setSelection("coin:upbit:KRW-THIRD",60000);const auto thirdToken=http.tokens.last();
    emit http.finished(newToken,response({upbitCandle(Now-60000,60000,"108")}));
    QVERIFY(model.displayObservation().candles.isEmpty());QVERIFY(thirdToken!=newToken);
    model.setSelection(btc,60000);QCOMPARE(model.displayObservation().candles.last().close,QString("101"));
}
void MarketCandlesTest::retryAfterSurvivesSelectionHideAndLockPerProvider(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",60000);HttpResult limited;limited.status=429;limited.headers.insert("Retry-After","7");
    emit http.finished(http.tokens.last(),limited);QCOMPARE(http.tokens.size(),1);
    model.setSelection("coin:upbit:KRW-OTHER",60000);model.setSelection({},60000);
    model.setSelection("coin:upbit:KRW-BTC",60000);model.clearDisplayCache();QCOMPARE(http.tokens.size(),1);
    model.setInstruments("binance",{binanceInstrument()});model.setSelection("coin:binance:SYNUSDT",60000);
    QCOMPARE(http.tokens.size(),2);QCOMPARE(http.requests.last().url.host(),QString("api.binance.com")); // unrelated provider remains usable
    model.setSelection("coin:upbit:KRW-BTC",60000);clock+=6999;QTest::qWait(250);QCOMPARE(http.tokens.size(),2);
    ++clock;QTRY_COMPARE_WITH_TIMEOUT(http.tokens.size(),3,2500);
    QCOMPARE(http.requests.last().url.host(),QString("api.upbit.com"));
}
void MarketCandlesTest::archiveHistoryNeverEntersTheMemoryLru(){
    CandleHttp http;qint64 clock=Now+10000;MarketCandles model(&http,{},[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",60000);
    const auto stored=MarketCandles::parse("coin:upbit:KRW-BTC",60000,response({upbitCandle(Now-60000,60000)}),clock,clock);
    QCOMPARE(model.setStoredCompleted(stored),StoredCandlesOffer::Accepted);QVERIFY(model.displayObservation().fromArchive);
    model.setSelection({},60000);model.setSelection("coin:upbit:KRW-BTC",60000);
    QVERIFY(model.displayObservation().candles.isEmpty());
}
void MarketCandlesTest::binanceMetadataPauseRetainsHistoryButConfirmedRemovalRevokesIt(){
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();qint64 clock=Now+10000;
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});
    const auto instrument=binanceInstrument();auto other=instrument;other.symbol="OTHERUSDT";other.baseAsset="OTHER";
    model.setInstruments("binance",{instrument});model.setSelection("coin:binance:SYNUSDT",60000);
    QJsonArray rows;rows.append(binanceCandle(Now-60000,60000));
    emit transport.finished(transport.tokens.last(),response(rows));QVERIFY(model.series().available);
    const auto requests=transport.requests.size(),connections=wire->endpoints.size();
    model.setInstruments("binance",{});QVERIFY(!model.series().available);
    auto display=model.displayObservation();QVERIFY(!display.candles.isEmpty());QVERIFY(display.delayed);QCOMPARE(display.currency,QString("USDT"));
    model.setInstruments("binance",{});clock+=10000;QTest::qWait(210);
    QCOMPARE(transport.requests.size(),requests);QCOMPARE(wire->endpoints.size(),connections);
    model.setInstruments("binance",{other});QVERIFY(model.displayObservation().candles.isEmpty());
    model.setInstruments("binance",{instrument});QCOMPARE(transport.requests.size(),requests+1);
    emit transport.finished(transport.tokens.last(),response(rows));QVERIFY(model.series().available);
    model.setInstruments("binance",{});QVERIFY(!model.displayObservation().candles.isEmpty());
    auto stopped=instrument;stopped.status="BREAK";stopped.tradable=false;
    model.setInstruments("binance",{stopped});QVERIFY(model.displayObservation().candles.isEmpty());
    model.setInstruments("binance",{instrument});emit transport.finished(transport.tokens.last(),response(rows));
    model.setInstruments("binance",{});model.setSelection("coin:binance:OTHERUSDT",60000);
    QVERIFY(model.displayObservation().candles.isEmpty());
}
void MarketCandlesTest::metadataRefreshPreservesLastVerifiedCompletedObservation(){
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();qint64 clock=Now+10000;
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});
    model.setVerifiedUpbitMarkets({"KRW-BTC"},true);model.setSelection("coin:upbit:KRW-BTC",60000);
    emit transport.finished(transport.tokens.last(),response({upbitCandle(Now-60000,60000)}));
    QVERIFY(model.displayObservation().available);const auto requests=transport.requests.size();
    model.setVerifiedUpbitMarkets({},false);QVERIFY(!model.series().available);
    auto display=model.displayObservation();QVERIFY(!display.candles.isEmpty());QVERIFY(display.delayed);QVERIFY(!display.available);
    clock+=10000;QTest::qWait(210);QCOMPARE(transport.requests.size(),requests);
    model.setVerifiedUpbitMarkets({"KRW-BTC"},true);QCOMPARE(transport.requests.size(),requests+1);
    QVERIFY(!model.displayObservation().candles.isEmpty());
    model.setVerifiedUpbitMarkets({"KRW-OTHER"},true);QVERIFY(model.displayObservation().candles.isEmpty());
}
void MarketCandlesTest::missingUpbitMetadataCannotBeMistakenForAnAbsentMarket(){
    auto instrument=binanceInstrument();instrument.symbol="HYPEUSDT";instrument.baseAsset="HYPE";
    const auto result=resolveChartMarket({"coin","upbit","KRW-HYPE"},{},false,{instrument});
    QVERIFY(!result.available());QCOMPARE(result.availability,ChartMarketAvailability::Checking);QVERIFY(!result.fallbackUsed);
}
void MarketCandlesTest::verifiedMarketRoutingUsesExactOfficialIdentity(){
    PublicMarketRow btc;btc.market="KRW-BTC";btc.base="BTC";
    auto instrument=binanceInstrument();instrument.symbol="HYPEUSDT";instrument.baseAsset="HYPE";
    const WatchlistIdentity requested{"coin","upbit","KRW-HYPE"};
    auto result=resolveChartMarket(requested,{btc},true,{instrument});
    QVERIFY(result.available());QCOMPARE(result.identity.key(),QString("coin:binance:HYPEUSDT"));QVERIFY(result.fallbackUsed);
    result=resolveChartMarket({"coin","upbit","KRW-BTC"},{btc},true,{instrument});
    QVERIFY(result.available());QCOMPARE(result.identity.key(),QString("coin:upbit:KRW-BTC"));QVERIFY(!result.fallbackUsed);
    result=resolveChartMarket(requested,{btc},true,{});QVERIFY(!result.available());
    QCOMPARE(result.availability,ChartMarketAvailability::Checking);
    instrument.tradable=false;instrument.status="BREAK";
    result=resolveChartMarket(requested,{btc},true,{instrument});QVERIFY(!result.available());
    QCOMPARE(result.availability,ChartMarketAvailability::Unsupported);
    instrument.tradable=true;instrument.status="TRADING";instrument.market="future";
    QVERIFY(!resolveChartMarket(requested,{btc},true,{instrument}).available());
    instrument.market="spot";instrument.symbol="HYPEBTC";instrument.quoteAsset="BTC";
    QVERIFY(!resolveChartMarket(requested,{btc},true,{instrument}).available());
    result=resolveChartMarket({"coin","binance","HYPEBTC"},{btc},true,{instrument});
    QVERIFY(result.available());QCOMPARE(result.identity.code,QString("HYPEBTC"));QVERIFY(!result.fallbackUsed);
    instrument.symbol="HYPEUSDT";instrument.quoteAsset="USDT";
    QVERIFY(!resolveChartMarket(requested,{btc},true,{instrument,instrument}).available());
    instrument.baseAsset="OTHER";QVERIFY(!resolveChartMarket(requested,{btc},true,{instrument}).available());
    result=resolveChartMarket(requested,{},false,{});QVERIFY(!result.available());
    QCOMPARE(result.availability,ChartMarketAvailability::Checking);
}
void MarketCandlesTest::unknownOrRevokedUpbitMarketsNeverSendRequests(){
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();qint64 clock=Now+10000;
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});
    model.setVerifiedUpbitMarkets({},false);model.setSelection("coin:upbit:KRW-HYPE",60000);
    QCOMPARE(transport.requests.size(),0);QCOMPARE(wire->endpoints.size(),0);
    model.setVerifiedUpbitMarkets({"KRW-BTC"},true);
    QCOMPARE(transport.requests.size(),0);QVERIFY(model.series().reason.contains("지원"));
    model.setSelection("coin:upbit:KRW-BTC",60000);QCOMPARE(transport.requests.size(),1);
    const auto oldToken=transport.tokens.last();
    emit transport.finished(oldToken,response({upbitCandle(Now-60000,60000),upbitCandle(Now,60000)}));
    QVERIFY(model.series().available);
    model.setVerifiedUpbitMarkets({},false);QVERIFY(!model.series().available);QVERIFY(model.displayObservation().delayed);
    clock+=16000;QTest::qWait(250);QCOMPARE(transport.requests.size(),1);
    emit transport.finished(oldToken,response({upbitCandle(Now-60000,60000)}));QVERIFY(!model.series().available);
    model.setVerifiedUpbitMarkets({"KRW-BTC"},true);QCOMPARE(transport.requests.size(),2);
    model.setVerifiedUpbitMarkets({"KRW-BTC"},true);QCOMPARE(transport.requests.size(),2);
}
void MarketCandlesTest::unsupportedUpbitResponseStopsRetriesUntilMetadataChanges(){
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();qint64 clock=Now;
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});
    model.setVerifiedUpbitMarkets({"KRW-HYPE"},true);model.setSelection("coin:upbit:KRW-HYPE",60000);
    QCOMPARE(transport.requests.size(),1);const auto oldEpoch=wire->generations.last();
    HttpResult missing;missing.status=404;missing.body=R"({"error":{"name":404,"message":"Code not found"}})";
    emit transport.finished(transport.tokens.last(),missing);
    QVERIFY(model.series().reason.contains("지원"));
    clock+=61000;emit wire->failed(oldEpoch);QTest::qWait(250);QCOMPARE(transport.requests.size(),1);
    model.setVerifiedUpbitMarkets({"KRW-HYPE"},true);QCOMPARE(transport.requests.size(),1);
    model.setSelection("coin:upbit:KRW-BTC",60000);model.setSelection("coin:upbit:KRW-HYPE",60000);
    QCOMPARE(transport.requests.size(),1);
    model.setVerifiedUpbitMarkets({"KRW-BTC"},true);model.setVerifiedUpbitMarkets({"KRW-BTC","KRW-HYPE"},true);
    QCOMPARE(transport.requests.size(),2);
}
void MarketCandlesTest::displayHistoryNeverBecomesCurrentData_data(){
    QTest::addColumn<QString>("failure");
    for(const auto* name:{"expiry","disconnect","timeout","http500","http429","invalid"})QTest::newRow(name)<<QString(name);
}
void MarketCandlesTest::displayHistoryNeverBecomesCurrentData(){
    QFETCH(QString,failure);CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();
    qint64 clock=Now+10000;const qint64 minute=60000;
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",minute);
    emit transport.finished(transport.tokens.last(),response({upbitCandle(Now-minute,minute),upbitCandle(Now,minute)}));
    const auto original=model.series();QVERIFY(original.available);QVERIFY(original.forming);
    if(failure=="expiry")clock+=MarketCandles::FreshnessMs+1;
    else{
        const auto epoch=wire->generations.last();emit wire->failed(epoch);
        if(failure!="disconnect"){
            clock+=1200;QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),2,1500);
            if(failure=="timeout"){
                clock+=15000;QTest::qWait(250);
            }else{
                HttpResult failed;failed.status=failure=="http500"?500:failure=="http429"?429:200;
                failed.body="{}";emit transport.finished(transport.tokens.last(),failed);
            }
        }
        emit wire->received(epoch,upbitFrame(Now,clock,"109")); // rejected old generation
    }
    const auto current=model.series();QVERIFY(!current.available);QVERIFY(current.candles.isEmpty());QVERIFY(!current.forming);
    const auto display=model.displayObservation();QVERIFY(!display.available);QVERIFY(display.delayed);
    QCOMPARE(display.candles.size(),1);QCOMPARE(display.candles.first().close,original.candles.first().close);
    QCOMPARE(display.receivedAtMs,original.receivedAtMs);QCOMPARE(display.sourceTimeMs,original.sourceTimeMs);QVERIFY(!display.forming);
    QVERIFY(model.displayObservation(original.receivedAtMs-1).candles.isEmpty()); // clock rollback cannot revive data
    model.setSelection("coin:upbit:KRW-BTC",300000);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection("coin:upbit:KRW-OTHER",minute);QVERIFY(model.displayObservation().candles.isEmpty());
    model.setSelection({},minute);QVERIFY(model.displayObservation().candles.isEmpty());
}
void MarketCandlesTest::formingCandleNeverEntersCompletedStudies(){
    const auto result=MarketCandles::parse("BTC",response({candle(Now,"109"),candle(Now-MarketCandles::IntervalMs,"101")}),Now+10000);
    QVERIFY(result.available);QCOMPARE(result.candles.size(),1);
    QCOMPARE(result.candles.first().close,QString("101"));
    QVERIFY2(result.forming.has_value(),"The observed open candle must be retained separately for display");
    QCOMPARE(result.forming->startsAtMs,Now);QCOMPARE(result.forming->close,QString("109"));
    QVERIFY(!candleCompression(result.candles).available);
    auto malformed=candle(Now);malformed["low_price"]="120";
    const auto rejected=MarketCandles::parse("BTC",response({candle(Now-MarketCandles::IntervalMs),malformed}),Now+10000);
    QVERIFY(!rejected.available);QVERIFY(!rejected.forming.has_value());
}
void MarketCandlesTest::usesActualClosedCandlesAndLeavesMissingIntervals(){
    const auto result=MarketCandles::parse("BTC",response({candle(Now),candle(Now-MarketCandles::IntervalMs),candle(Now-3*MarketCandles::IntervalMs)}),Now+10000);
    QVERIFY(result.available);QCOMPARE(result.candles.size(),2);
    QCOMPARE(result.candles[0].startsAtMs,Now-3*MarketCandles::IntervalMs);
    QCOMPARE(result.candles[1].startsAtMs,Now-MarketCandles::IntervalMs);
    QCOMPARE(result.candles[0].close,QString("100"));
    QVERIFY(MarketCandles::isRequest(MarketCandles::request("BTC")));
}
void MarketCandlesTest::compressionIsDefinedAndNotADirectionalSignal(){
    QList<MarketCandle> rows;
    for(int i=0;i<21;++i)rows.append({Now+i*MarketCandles::IntervalMs,Now+i*MarketCandles::IntervalMs+1000,"100","110","90","100","2"});
    const auto result=candleCompression(rows);QVERIFY(result.available);QVERIFY(result.compressed);
    QCOMPARE(result.bandLow,100.0);QCOMPARE(result.bandHigh,100.0);
    QCOMPARE(result.channelLow,70.0);QCOMPARE(result.channelHigh,130.0);
}
void MarketCandlesTest::restReceiptsKeepDistinctIdsEvenAtTheSameClock(){
    CandleHttp http;MarketCandles model(&http,{},[]{return Now+60000;});
    model.setInstruments("binance",{binanceInstrument()});
    QJsonArray rows;rows.append(binanceCandle(Now-60000,60000));const auto payload=response(rows);
    model.setSelection("coin:binance:SYNUSDT",60000);emit http.finished(http.tokens.last(),payload);
    const auto first=model.restObservation();QVERIFY(first.available);QVERIFY(first.restRevision>0);
    model.setSelection({},60000);QVERIFY(model.restObservation().candles.isEmpty());
    model.setSelection("coin:binance:SYNUSDT",60000);emit http.finished(http.tokens.last(),payload);
    const auto next=model.restObservation();QVERIFY(next.available);
    QCOMPARE(first.restReceivedAtMs,next.restReceivedAtMs);QVERIFY(first.restRevision!=next.restRevision);
    auto stopped=binanceInstrument();stopped.tradable=false;model.setInstruments("binance",{stopped});
    QVERIFY(model.restObservation().candles.isEmpty());
}
void MarketCandlesTest::storedTickMayFollowTheCandleBoundary(){
    // Official timestamp is when the last tick was stored, not its trade time.
    // Actual public rows included storage 1..57ms after the candle boundary.
    auto row=candle(Now-MarketCandles::IntervalMs);row["timestamp"]=QString::number(Now+57);
    const auto result=MarketCandles::parse("BTC",response({row}),Now+1000);
    QVERIFY(result.available);QCOMPARE(result.candles.size(),1);
    QCOMPARE(result.candles.first().startsAtMs,Now-MarketCandles::IntervalMs);
    QCOMPARE(result.candles.first().close,QString("100"));
    row["timestamp"]=QString::number(Now-MarketCandles::IntervalMs-1);
    QVERIFY(!MarketCandles::parse("BTC",response({row}),Now+1000).available);
}
void MarketCandlesTest::invalidCandlesClearTheEntireSeries_data(){
    QTest::addColumn<QString>("mutation");
    for(const auto* name:{"market","negative","ohlc","unit","future-trade","duplicate","timestamp","volume","misaligned","invalid-json"})QTest::newRow(name)<<QString(name);
}
void MarketCandlesTest::invalidCandlesClearTheEntireSeries(){
    QFETCH(QString,mutation);auto row=candle(Now-MarketCandles::IntervalMs);QJsonArray rows{row};
    if(mutation=="market")row["market"]="KRW-ETH";
    if(mutation=="negative")row["low_price"]="-1";
    if(mutation=="ohlc")row["low_price"]="101";
    if(mutation=="unit")row["unit"]="5";
    if(mutation=="future-trade")row["timestamp"]=QString::number(Now+1);
    if(mutation=="timestamp")row["timestamp"]="garbage";
    if(mutation=="volume")row["candle_acc_trade_volume"]="-1";
    if(mutation=="misaligned")row["candle_date_time_utc"]="2026-10-21T15:01:00";
    rows[0]=row;if(mutation=="duplicate")rows.append(row);
    auto r=response(rows);if(mutation=="invalid-json")r.body="{}";
    const auto result=MarketCandles::parse("BTC",r,Now);
    QVERIFY(!result.available);QVERIFY(result.candles.isEmpty());QVERIFY(!result.reason.isEmpty());
}
void MarketCandlesTest::incompleteOrGappedStudyHasNoSignal(){
    QList<MarketCandle> rows;
    for(int i=0;i<21;++i)rows.append({Now+i*MarketCandles::IntervalMs,Now+i*MarketCandles::IntervalMs+1000,"100","110","90","100","2"});
    QVERIFY(!candleCompression(rows.mid(0,20)).available);
    rows[10].startsAtMs+=1000;
    const auto result=candleCompression(rows);QVERIFY(!result.available);QVERIFY(!result.reason.isEmpty());
}
void MarketCandlesTest::nonconstantStudyMatchesIndependentDecimalCalculation(){
    QList<MarketCandle> rows;
    for(int i=0;i<60;++i){const auto close=QString::number((100+(i*7)%13+i)/10.0,'f',1);
        rows.append({Now+i*MarketCandles::IntervalMs,Now+i*MarketCandles::IntervalMs+1000,close,
            decimal::add(close,"2"),decimal::subtract(close,"2"),close,"2"});}
    const auto result=candleCompression(rows);QVERIFY(result.available);
    // Python Decimal(prec=50), fixed 60-bar fixture. Independent arithmetic receipt.
    QVERIFY(qAbs(result.bandLow-14.158117226390024)<1e-10);
    QVERIFY(qAbs(result.bandHigh-16.921882773609976)<1e-10);
    QVERIFY(qAbs(result.channelLow-9.546975407693594)<1e-10);
    QVERIFY(qAbs(result.channelHigh-21.546975407693594)<1e-10);
    if(const auto path=qEnvironmentVariable("KEEFI_G8_CANDLES_RESULT");!path.isEmpty()){
        const QJsonObject output{{"format","keefi-g8-candle-crosscheck/1"},{"available",result.available},{"compressed",result.compressed},
            {"bandLow",QString::number(result.bandLow,'g',17)},{"bandHigh",QString::number(result.bandHigh,'g',17)},
            {"channelLow",QString::number(result.channelLow,'g',17)},{"channelHigh",QString::number(result.channelHigh,'g',17)}};
        QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));const auto bytes=QJsonDocument(output).toJson();QCOMPARE(file.write(bytes),bytes.size());
    }
}
void MarketCandlesTest::modelDropsFailuresFutureResponsesAndLateReplies(){
    class Fake final:public TradingTransport{public:
        QList<quint64> tokens;int aborts=0;
        void send(quint64 token,const RequestSpec& r)override{QVERIFY(!r.authenticated);QVERIFY(MarketCandles::isRequest(r));tokens.append(token);}
        void abort()override{++aborts;}
    } transport;
    MarketCandles model(&transport);model.setAsset("BTC");QTRY_COMPARE(transport.tokens.size(),1);
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto start=(now/MarketCandles::IntervalMs-1)*MarketCandles::IntervalMs;
    emit transport.finished(transport.tokens.last(),response({candle(start)}));QVERIFY(model.series().available);
    QVERIFY(!model.series(now+MarketCandles::FreshnessMs+1000).available);
    model.setAsset("ETH");QTRY_COMPARE(transport.tokens.size(),2);const auto old=transport.tokens.last();
    model.setAsset({});QVERIFY(transport.aborts>0);
    emit transport.finished(old,response({candle(start)}));QVERIFY(model.series().candles.isEmpty());
    model.setAsset("BTC");QTRY_COMPARE(transport.tokens.size(),3);
    HttpResult failed;failed.status=503;emit transport.finished(transport.tokens.last(),failed);
    QVERIFY(!model.series().available);QVERIFY(model.series().candles.isEmpty());
    auto wrong=MarketCandles::request("BTC");wrong.authenticated=true;QVERIFY(!MarketCandles::isRequest(wrong));
    wrong=MarketCandles::request("BTC");wrong.url.setHost("example.invalid");QVERIFY(!MarketCandles::isRequest(wrong));
}
void MarketCandlesTest::nativeTimeframesKeepProviderIdentityAndUtcBoundaries_data(){
    QTest::addColumn<QString>("provider");QTest::addColumn<qint64>("interval");QTest::addColumn<QString>("wireInterval");
    const QList<QPair<qint64,QString>> frames{{60000,"1m"},{300000,"5m"},{900000,"15m"},{3600000,"1h"},{14400000,"4h"},{86400000,"1d"}};
    for(const auto& provider:{QString("upbit"),QString("binance")})for(const auto& frame:frames)
        QTest::newRow(qPrintable(provider+'-'+frame.second))<<provider<<frame.first<<frame.second;
}
void MarketCandlesTest::nativeTimeframesKeepProviderIdentityAndUtcBoundaries(){
    QFETCH(QString,provider);QFETCH(qint64,interval);QFETCH(QString,wireInterval);
    const auto identity=provider=="upbit"?QString("coin:upbit:KRW-BTC"):QString("coin:binance:SYNUSDT");
    const auto request=MarketCandles::request(identity,interval);
    QVERIFY(request.url.isValid());QVERIFY(MarketCandles::isRequest(request));
    QCOMPARE(request.method,QByteArray("GET"));QVERIFY(!request.authenticated);QVERIFY(request.body.isEmpty());
    const QUrlQuery query(request.url);
    if(provider=="upbit"){
        QCOMPARE(request.url.host(),QString("api.upbit.com"));
        QCOMPARE(request.url.path(),interval==86400000?QString("/v1/candles/days"):QString("/v1/candles/minutes/%1").arg(interval/60000));
        QCOMPARE(query.queryItemValue("market"),QString("KRW-BTC"));QCOMPARE(query.queryItemValue("count"),QString("200"));
    }else{
        QCOMPARE(request.url.host(),QString("api.binance.com"));QCOMPARE(request.url.path(),QString("/api/v3/klines"));
        QCOMPARE(query.queryItemValue("symbol"),QString("SYNUSDT"));QCOMPARE(query.queryItemValue("interval"),wireInterval);
        QCOMPARE(query.queryItemValue("limit"),QString("200"));
    }
    const auto start=Now/interval*interval,received=start+interval/2;
    const auto result=MarketCandles::parse(identity,interval,response(provider=="upbit"?
        QJsonArray{upbitCandle(start,interval),upbitCandle(start-interval,interval)}:
        QJsonArray{binanceCandle(start-interval,interval),binanceCandle(start,interval)}),received,received,binanceInstrument());
    QVERIFY2(result.available,qPrintable(result.reason));QCOMPARE(result.identity,identity);QCOMPARE(result.intervalMs,interval);
    QCOMPARE(result.asset,provider=="upbit"?QString("BTC"):QString("SYN"));
    QCOMPARE(result.currency,provider=="upbit"?QString("KRW"):QString("USDT"));
    QCOMPARE(result.candles.size(),1);QCOMPARE(result.candles.first().startsAtMs,start-interval);
    QVERIFY(result.forming.has_value());QCOMPARE(result.forming->startsAtMs,start);
    QCOMPARE(result.sourceUrl,request.url.toString());
    QCOMPARE(result.sourceTimeMs,provider=="upbit"?start+1000:qint64(0));
    auto invalid=request;QUrlQuery extra(invalid.url);extra.addQueryItem("timeZone","8");invalid.url.setQuery(extra);
    QVERIFY(!MarketCandles::isRequest(invalid));
}
void MarketCandlesTest::restCompletionUsesRequestStartAcrossTheBoundary(){
    constexpr qint64 minute=60000;
    const auto start=Now/minute*minute;
    const auto result=MarketCandles::parse("coin:upbit:KRW-BTC",minute,
        response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}),start+minute+500,start+minute-1);
    QVERIFY(result.available);QCOMPARE(result.candles.size(),1);
    QCOMPARE(result.candles.first().startsAtMs,start-minute);QVERIFY(result.forming.has_value());
    QCOMPARE(result.forming->startsAtMs,start);
}
void MarketCandlesTest::verifiedUnicodeIdentifiersReachNativeCandleRequests(){
    const auto base=QString::fromUcs4(U"\U00010400").repeated(36);const auto code=base+"USDT";
    const auto identity="coin:binance:"+code;const auto request=MarketCandles::request(identity,60000);
    QVERIFY(!request.url.isEmpty());QCOMPARE(QUrlQuery(request.url).queryItemValue("symbol"),code);
    QVERIFY(MarketCandles::isRequest(request));auto item=binanceInstrument();item.symbol=code;item.baseAsset=base;
    QJsonArray rows;rows.append(binanceCandle(Now/60000*60000-60000,60000));
    const auto parsed=MarketCandles::parse(identity,60000,response(rows),Now,Now,item);
    QVERIFY(parsed.available);QCOMPARE(parsed.asset,base);QCOMPARE(parsed.identity,identity);
    QVERIFY(MarketCandles::request("coin:binance:"+QString(QChar(0xD801))+"USDT",60000).url.isEmpty());
}
void MarketCandlesTest::binanceNeedsVerifiedSpotMetadataAndExactTwelveFields(){
    constexpr qint64 minute=60000;const auto start=Now/minute*minute;
    QJsonArray rows;rows.append(binanceCandle(start-minute,minute));const auto payload=response(rows);
    const auto parse=[&](const HttpResult& r,const MarketInstrument& item){return MarketCandles::parse("coin:binance:SYNUSDT",minute,r,Now,Now,item);};
    QVERIFY(parse(payload,binanceInstrument()).available);
    QVERIFY(!parse(payload,{}).available);
    auto wrong=binanceInstrument();wrong.baseAsset.clear();QVERIFY(!parse(payload,wrong).available);
    wrong=binanceInstrument();wrong.symbol="OTHERSYNUSDT";QVERIFY(!parse(payload,wrong).available);
    wrong=binanceInstrument();wrong.market="futures";QVERIFY(!parse(payload,wrong).available);
    wrong=binanceInstrument();wrong.status="HALT";QVERIFY(!parse(payload,wrong).available);
    auto extra=binanceCandle(start-minute,minute);extra.append("unrecognized");
    const auto rejected=parse(response({binanceCandle(start-2*minute,minute),extra}),binanceInstrument());
    QVERIFY(!rejected.available);QVERIFY(rejected.candles.isEmpty());QVERIFY(!rejected.forming.has_value());
}
void MarketCandlesTest::compressionUsesTheSelectedInterval(){
    QList<MarketCandle> rows;
    for(int i=0;i<21;++i)rows.append({Now+i*300000,Now+i*300000+1000,"100","110","90","100","2"});
    const auto result=candleCompression(rows,300000);QVERIFY(result.available);QVERIFY(result.compressed);
    QCOMPARE(result.channelLow,70.0);QCOMPARE(result.channelHigh,130.0);
    rows[10].startsAtMs+=300000;QVERIFY(!candleCompression(rows,300000).available);
}
void MarketCandlesTest::upbitSameSourceTimestampUsesLatestArrivalWithoutCompletingTheBar(){
    constexpr qint64 minute=60000;const auto start=Now/minute*minute;qint64 clock=start+20000;
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* fake=socket.get();
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",minute);
    QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),1,2500);QCOMPARE(fake->endpoints.size(),1);
    QCOMPARE(fake->endpoints.first(),QUrl("wss://api.upbit.com/websocket/v1"));
    const auto epoch=fake->generations.last();emit fake->connected(epoch);
    QVERIFY(!fake->messages.isEmpty());
    const auto subscription=QJsonDocument::fromJson(fake->messages.last()).array();
    QCOMPARE(subscription.at(1).toObject().value("type").toString(),QString("candle.1m"));
    QCOMPARE(subscription.at(1).toObject().value("codes").toArray(),QJsonArray{"KRW-BTC"});
    emit transport.finished(transport.tokens.last(),response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}));
    QVERIFY(model.series(clock).available);QCOMPARE(model.series(clock).candles.size(),1);
    emit fake->received(epoch,upbitFrame(start,clock,"106","6"));
    emit fake->received(epoch,upbitFrame(start,clock,"104","7"));
    QVERIFY(model.series(clock).forming.has_value());QCOMPARE(model.series(clock).forming->close,QString("104"));
    QCOMPARE(model.series(clock).forming->volume,QString("7"));QCOMPARE(model.series(clock).candles.size(),1);
    clock=start+minute+500;
    const auto withoutConfirmation=model.series(clock);QCOMPARE(withoutConfirmation.candles.size(),1);
    model.setSelection({},minute);const auto token=transport.tokens.last();
    emit fake->received(epoch,upbitFrame(start,clock,"109"));emit transport.finished(token,response({upbitCandle(start,minute)}));
    QVERIFY(model.series(clock).candles.isEmpty());QVERIFY(!model.series(clock).forming.has_value());QVERIFY(fake->aborts>0);
}
void MarketCandlesTest::binanceApplicationHistoryBudgetRejectsMoreThanTwoHundredRows(){
    constexpr qint64 minute=60000;QJsonArray rows;
    for(int i=200;i>0;--i)rows.append(binanceCandle(Now-i*minute,minute));
    const auto parse=[&]{return MarketCandles::parse("coin:binance:SYNUSDT",minute,response(rows),Now,Now,binanceInstrument());};
    QCOMPARE(parse().candles.size(),200);
    rows.append(binanceCandle(Now-201*minute,minute));
    const auto rejected=parse();QVERIFY(!rejected.available);QVERIFY(rejected.candles.isEmpty());QVERIFY(!rejected.forming.has_value());
}
void MarketCandlesTest::binanceClockLeadWaitsForRealTime_data(){
    QTest::addColumn<int>("lead");QTest::addColumn<bool>("accepted");
    QTest::newRow("current-event-immediate")<<0<<true;
    QTest::newRow("observed-public-82ms-clock-lead")<<82<<true;
    QTest::newRow("excessive-future-rejected")<<1001<<false;
}
void MarketCandlesTest::binanceClockLeadWaitsForRealTime(){
    QFETCH(int,lead);QFETCH(bool,accepted);const qint64 start=Now/60000*60000;qint64 clock=start+20000;
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();
    MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setInstruments("binance",{binanceInstrument()});
    model.setSelection("coin:binance:SYNUSDT",60000);const auto epoch=wire->generations.last();emit wire->connected(epoch);
    emit http.finished(http.tokens.last(),response({binanceCandle(start-60000,60000),binanceCandle(start,60000)}));
    const auto originalReceipt=clock,event=clock+lead;const auto restRevision=model.restObservation().restRevision;
    QSignalSpy changed(&model,&MarketCandles::changed);
    emit wire->received(epoch,binanceFrame(start,event,false,"108"));
    if(lead){
        // Future evidence cannot reach the view, calculations or alerts before
        // the injected real-time clock actually reaches the exchange timestamp.
        QCOMPARE(changed.size(),0);QCOMPARE(model.series().forming->close,QString("105"));
        QTest::qWait(35);QCOMPARE(changed.size(),0);QCOMPARE(model.series().sourceTimeMs,qint64(0));
        clock=event;
    }
    if(accepted){
        QTRY_COMPARE_WITH_TIMEOUT(model.series().forming->close,QString("108"),1000);
        QCOMPARE(model.series().sourceTimeMs,event);QCOMPARE(model.series().receivedAtMs,originalReceipt);
        QCOMPARE(changed.size(),1);QCOMPARE(model.series().candles.size(),1);
    }else{
        QTest::qWait(180);QCOMPARE(changed.size(),0);QCOMPARE(model.series().forming->close,QString("105"));
        QCOMPARE(model.series().sourceTimeMs,qint64(0));
    }
    QCOMPARE(model.restObservation().restRevision,restRevision);
}
void MarketCandlesTest::binanceDeferredPacketIsDroppedOnSelectionChange(){
    const qint64 start=Now/60000*60000;qint64 clock=start+20000;
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();
    MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setInstruments("binance",{binanceInstrument()});
    model.setSelection("coin:binance:SYNUSDT",60000);const auto oldEpoch=wire->generations.last();emit wire->connected(oldEpoch);
    emit http.finished(http.tokens.last(),response({binanceCandle(start-60000,60000),binanceCandle(start,60000)}));
    emit wire->received(oldEpoch,binanceFrame(start,clock+82,false,"109"));
    model.setSelection("coin:upbit:KRW-BTC",60000);emit wire->connected(wire->generations.last());
    emit http.finished(http.tokens.last(),response({upbitCandle(start-60000,60000),upbitCandle(start,60000)}));
    const auto selected=model.series();clock+=100;QTest::qWait(120);
    QCOMPARE(model.series().identity,QString("coin:upbit:KRW-BTC"));QCOMPARE(model.series().forming->close,selected.forming->close);
    QCOMPARE(model.series().sourceTimeMs,selected.sourceTimeMs);QCOMPARE(model.series().receivedAtMs,selected.receivedAtMs);
}
void MarketCandlesTest::binanceDeferredPacketCoalescesAndPreservesOriginalReceipt(){
    const qint64 start=Now/60000*60000,arrival=start+20000;qint64 clock=arrival;
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();
    MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setInstruments("binance",{binanceInstrument()});
    model.setSelection("coin:binance:SYNUSDT",60000);const auto epoch=wire->generations.last();emit wire->connected(epoch);
    emit http.finished(http.tokens.last(),response({binanceCandle(start-60000,60000),binanceCandle(start,60000)}));
    QSignalSpy changed(&model,&MarketCandles::changed);
    emit wire->received(epoch,binanceFrame(start,arrival+82,false,"107"));++clock;
    emit wire->received(epoch,binanceFrame(start,arrival+100,false,"109"));
    clock=arrival+82;QTest::qWait(110);QCOMPARE(changed.size(),0);QCOMPARE(model.series().forming->close,QString("105"));
    clock=arrival+100;QTRY_COMPARE_WITH_TIMEOUT(model.series().forming->close,QString("109"),1000);
    QCOMPARE(changed.size(),1);QCOMPARE(model.series().sourceTimeMs,arrival+100);QCOMPARE(model.series().receivedAtMs,arrival+1);
}
void MarketCandlesTest::binanceDeferredPacketDoesNotSurviveClockRollback(){
    const qint64 start=Now/60000*60000,arrival=start+20000;qint64 clock=arrival;
    CandleHttp http;auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();
    MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setInstruments("binance",{binanceInstrument()});
    model.setSelection("coin:binance:SYNUSDT",60000);const auto epoch=wire->generations.last();emit wire->connected(epoch);
    emit http.finished(http.tokens.last(),response({binanceCandle(start-60000,60000),binanceCandle(start,60000)}));
    emit wire->received(epoch,binanceFrame(start,arrival+82,false,"109"));
    clock=arrival-2000;QTest::qWait(120);clock=arrival+82;QTest::qWait(120);
    QCOMPARE(model.series().forming->close,QString("105"));QCOMPARE(model.series().sourceTimeMs,qint64(0));
}
void MarketCandlesTest::binanceStreamUsesTheAuthoritativeCloseFlagAndLeavesGaps(){
    constexpr qint64 minute=60000;const auto start=Now/minute*minute;qint64 clock=start+20000;
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* fake=socket.get();
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});model.setInstruments("binance",{binanceInstrument()});
    model.setSelection("coin:binance:SYNUSDT",minute);QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),1,2500);
    QCOMPARE(fake->endpoints.last(),QUrl("wss://stream.binance.com:9443/ws/synusdt@kline_1m"));
    const auto epoch=fake->generations.last();emit fake->connected(epoch);
    QJsonArray rows;rows.append(binanceCandle(start-minute,minute));rows.append(binanceCandle(start,minute));
    emit transport.finished(transport.tokens.last(),response(rows));
    QCOMPARE(model.series(clock).sourceTimeMs,qint64(0));QCOMPARE(model.series(clock).candles.size(),1);
    emit fake->received(epoch,binanceFrame(start,clock,false,"109\n"));
    QCOMPARE(model.series(clock).forming->close,QString("105"));
    emit fake->received(epoch,binanceFrame(start,clock,false));
    QCOMPARE(model.series(clock).forming->close,QString("104"));QCOMPARE(model.series(clock).forming->volume,QString("7"));
    QCOMPARE(model.series(clock).sourceTimeMs,clock);QCOMPARE(model.series(clock).candles.size(),1);
    clock=start+minute+500;emit fake->received(epoch,binanceFrame(start,clock,true));
    QCOMPARE(model.series(clock).candles.size(),2);QVERIFY(!model.series(clock).forming.has_value());
    QCOMPARE(model.series(clock).candles.last().startsAtMs,start);
    ++clock;emit fake->received(epoch,binanceFrame(start,clock,false,"109"));
    QCOMPARE(model.series(clock).candles.last().close,QString("104"));QVERIFY(!model.series(clock).forming.has_value());
    clock=start+3*minute+500;emit fake->received(epoch,binanceFrame(start+2*minute,clock,true));
    QCOMPARE(model.series(clock).candles.size(),3);
    QCOMPARE(model.series(clock).candles.last().startsAtMs-model.series(clock).candles.at(1).startsAtMs,2*minute);
}
void MarketCandlesTest::reconnectClearsUntilRestSeedAndRejectsOldGenerations(){
    constexpr qint64 minute=60000;const auto start=Now/minute*minute;qint64 clock=start+20000;
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* fake=socket.get();
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});model.setSelection("coin:upbit:KRW-BTC",minute);
    QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),1,2500);const auto oldToken=transport.tokens.last(),oldEpoch=fake->generations.last();
    emit fake->connected(oldEpoch);
    emit transport.finished(oldToken,response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}));
    QVERIFY(model.series(clock).available);emit fake->failed(oldEpoch);
    QVERIFY(!model.series(clock).available);QVERIFY(model.series(clock).candles.isEmpty());QVERIFY(!model.series(clock).forming.has_value());
    clock+=2000;QTRY_COMPARE_WITH_TIMEOUT(fake->generations.size(),2,2500);
    const auto currentEpoch=fake->generations.last(),currentToken=transport.tokens.last();
    QVERIFY(currentEpoch!=oldEpoch);QVERIFY(currentToken!=oldToken);emit fake->connected(currentEpoch);
    emit fake->received(oldEpoch,upbitFrame(start,clock,"109"));
    emit transport.finished(oldToken,response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}));
    emit fake->received(currentEpoch,upbitFrame(start,clock,"108"));
    QVERIFY(!model.series(clock).available);QVERIFY(model.series(clock).candles.isEmpty());QVERIFY(!model.series(clock).forming.has_value());
    emit transport.finished(currentToken,response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}));
    QVERIFY(model.series(clock).available);QCOMPARE(model.series(clock).candles.size(),1);
    model.setSelection({},minute);emit fake->received(currentEpoch,upbitFrame(start,clock,"109"));
    QVERIFY(model.series(clock).candles.isEmpty());QVERIFY(!model.series(clock).forming.has_value());
}
void MarketCandlesTest::delayedBootstrapCannotRollBackNewerFormingData(){
    constexpr qint64 minute=60000;const auto start=Now/minute*minute;qint64 clock=start+20000;
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* fake=socket.get();
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});model.setSelection("coin:upbit:KRW-BTC",minute);
    QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),1,2500);const auto epoch=fake->generations.last();emit fake->connected(epoch);
    clock+=500;emit fake->received(epoch,upbitFrame(start,clock,"108","5"));
    QVERIFY(!model.series(clock).available);QVERIFY(!model.series(clock).forming.has_value());
    clock+=500;emit transport.finished(transport.tokens.last(),response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}));
    QVERIFY(model.series(clock).available);QVERIFY(model.series(clock).forming.has_value());
    QCOMPARE(model.series(clock).forming->close,QString("108"));QCOMPARE(model.series(clock).forming->volume,QString("5"));
    QCOMPARE(model.series(clock).sourceTimeMs,clock-500);
}
void MarketCandlesTest::restTimeoutAndRetryAfterUseTheInjectedClock(){
    constexpr qint64 minute=60000;qint64 clock=Now+20000;
    CandleHttp transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",minute);QCOMPARE(transport.tokens.size(),1);const auto expired=transport.tokens.first();
    clock+=15001;QTRY_VERIFY_WITH_TIMEOUT(transport.aborts>0,2500);
    clock+=1000;QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),2,2500);
    emit transport.finished(expired,response({upbitCandle(Now-minute,minute)}));QVERIFY(!model.series(clock).available);
    HttpResult limited;limited.status=429;limited.headers.insert("Retry-After","5");limited.localRetryAfterSeconds=7;
    emit transport.finished(transport.tokens.last(),limited);
    clock+=6999;QTest::qWait(250);QCOMPARE(transport.tokens.size(),2);
    clock+=1;QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),3,2500);
    const auto start=clock/minute*minute;
    emit transport.finished(transport.tokens.last(),response({upbitCandle(start-minute,minute),upbitCandle(start,minute)}));
    QVERIFY(model.series(clock).available);
}
void MarketCandlesTest::lowercaseHttpDateRetryAfterDoesNotRetryBeforeTheServerDeadline(){
    constexpr qint64 minute=60000;qint64 clock=Now+20000;
    CandleHttp transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return clock;});
    model.setSelection("coin:upbit:KRW-BTC",minute);QCOMPARE(transport.tokens.size(),1);
    HttpResult limited;limited.status=429;
    limited.headers.insert("retry-after",QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(clock+7000,Qt::UTC),
        "ddd, dd MMM yyyy HH:mm:ss 'GMT'").toLatin1());
    emit transport.finished(transport.tokens.last(),limited);
    clock+=6999;QTest::qWait(300);QCOMPARE(transport.tokens.size(),1);
    clock+=1;QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),2,2500);
}
void MarketCandlesTest::metadataArrivalAndExpiryCannotRestoreAnOldSelection(){
    constexpr qint64 minute=60000;const auto start=Now/minute*minute;qint64 clock=start+20000;
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* fake=socket.get();
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});model.setSelection("coin:binance:SYNUSDT",minute);
    QCOMPARE(transport.tokens.size(),0);QCOMPARE(fake->endpoints.size(),0);QVERIFY(model.series(clock).asset.isEmpty());QVERIFY(model.series(clock).currency.isEmpty());
    model.setInstruments("binance",{binanceInstrument()});QCOMPARE(transport.tokens.size(),1);QCOMPARE(fake->endpoints.size(),1);
    QCOMPARE(model.series(clock).asset,QString("SYN"));QCOMPARE(model.series(clock).currency,QString("USDT"));
    const auto oldToken=transport.tokens.last(),oldEpoch=fake->generations.last();model.setInstruments("binance",{});
    QVERIFY(transport.aborts>0);QVERIFY(model.series(clock).asset.isEmpty());QVERIFY(model.series(clock).currency.isEmpty());
    QJsonArray rows;rows.append(binanceCandle(start-minute,minute));
    emit transport.finished(oldToken,response(rows));emit fake->connected(oldEpoch);emit fake->received(oldEpoch,binanceFrame(start,clock,false));
    QVERIFY(model.series(clock).candles.isEmpty());QVERIFY(!model.series(clock).forming.has_value());
    model.setInstruments("binance",{binanceInstrument()});QCOMPARE(transport.tokens.size(),2);QCOMPARE(fake->endpoints.size(),2);
    emit fake->failed(oldEpoch);emit transport.finished(transport.tokens.last(),response(rows));
    QVERIFY(model.series(clock).available);QCOMPARE(model.series(clock).asset,QString("SYN"));
    QVERIFY(!model.displayObservation(clock).candles.isEmpty());
    model.setInstruments("binance",{});
    QVERIFY(!model.displayObservation(clock).candles.isEmpty());
    QVERIFY(model.displayObservation(clock).delayed);
    QVERIFY(!model.displayObservation(clock).available);
}
void MarketCandlesTest::upbitDailyStreamOnlyTriggersNativeDailyRest(){
    constexpr qint64 day=86400000;const auto start=Now/day*day;qint64 clock=start+600000;
    CandleHttp transport;auto socket=std::make_unique<CandleSocket>();auto* fake=socket.get();
    MarketCandles model(&transport,std::move(socket),[&]{return clock;});model.setSelection("coin:upbit:KRW-BTC",day);
    QCOMPARE(transport.tokens.size(),1);const auto epoch=fake->generations.last();emit fake->connected(epoch);
    const auto subscribe=QJsonDocument::fromJson(fake->messages.last()).array();
    QCOMPARE(subscribe.at(1).toObject().value("type").toString(),QString("candle.1m"));
    emit transport.finished(transport.tokens.last(),response({upbitCandle(start-day,day),upbitCandle(start,day)}));
    clock+=1000;emit fake->received(epoch,upbitFrame(start+600000,clock,"108","20"));
    QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),2,2500);
    QCOMPARE(transport.requests.last().url.path(),QString("/v1/candles/days"));
    QCOMPARE(model.series(clock).candles.size(),1);QCOMPARE(model.series(clock).candles.first().startsAtMs,start-day);
    QCOMPARE(model.series(clock).forming->startsAtMs,start);QCOMPARE(model.series(clock).forming->volume,QString("2"));
}
void MarketCandlesTest::actualPublicCandles(){
    if(!qEnvironmentVariableIsSet("KEEFI_G8_LIVE_PUBLIC"))QSKIP("opt-in official public GET only");
    NetworkTradingTransport transport;MarketCandles model(&transport);model.setAsset("BTC");
    QTRY_VERIFY_WITH_TIMEOUT(model.series().reason!=QStringLiteral("캔들 조회 대기"),15000);
    const auto result=model.series();QVERIFY2(result.available,qPrintable(result.reason));QVERIFY(!result.candles.isEmpty());
    qInfo("Official Upbit closed candles: %lld, latest start: %lld",qint64(result.candles.size()),result.candles.last().startsAtMs);
}
QTEST_GUILESS_MAIN(MarketCandlesTest)
#include "tst_market_candles.moc"
