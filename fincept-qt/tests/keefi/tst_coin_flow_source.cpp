#include "keefi/CoinFlowSource.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/collector/CollectorRuntime.h"
#include "keefi/TradingTransport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QLocale>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>
using namespace keefi;
namespace {
constexpr qint64 Now=1792710000000;
class Http final:public TradingTransport {
  public:QList<RequestSpec> requests;QList<quint64> tokens;int aborts=0;bool synchronous=false;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{++aborts;if(synchronous&&!tokens.isEmpty()){HttpResult response;response.status=200;response.body="[]";emit finished(tokens.last(),response);}}
    void reply(int i,const HttpResult& response){emit finished(tokens.at(i),response);}
};
class Socket final:public PublicCandleSocket {
  public:QList<QUrl> endpoints;QList<quint64> generations;QList<QByteArray> sent;int aborts=0;
    void open(quint64 generation,const QUrl& url)override{generations.append(generation);endpoints.append(url);}
    void send(quint64,const QByteArray& message)override{sent.append(message);}
    void abort()override{++aborts;}
};
CoinFlowInstrument instrument(CoinFlowVenue venue){CoinFlowInstrument result;result.venue=venue;result.grain=venue==CoinFlowVenue::BinanceSpot?CoinFlowGrain::SpotAggregate:CoinFlowGrain::Futures100msAggregate;result.symbol="SYNUSDT";result.baseAsset="SYN";result.quoteAsset="USDT";result.status="TRADING";result.verified=true;result.identity=venue==CoinFlowVenue::BinanceSpot?"coin:binance:SYNUSDT":"usdm_futures:binance:SYNUSDT";if(venue==CoinFlowVenue::BinanceUsdM){result.marginAsset="USDT";result.contractType="PERPETUAL";result.underlyingType="COIN";}result.evidence={"합성 공개 메타데이터",venue==CoinFlowVenue::BinanceSpot?"https://api.binance.com/api/v3/exchangeInfo":"https://fapi.binance.com/fapi/v1/exchangeInfo",0,Now-1000,Now-1000};return result;}
void catalogs(CoinFlowSource& source){source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument(CoinFlowVenue::BinanceSpot)});source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{instrument(CoinFlowVenue::BinanceUsdM)});}
QByteArray frame(QString id="9007199254740993"){return QJsonDocument(QJsonObject{{"e","aggTrade"},{"E",QString::number(Now-1)},{"s","SYNUSDT"},{"a",id},{"p","100"},{"q","2"},{"f","9007199254740993"},{"l","9007199254740993"},{"T",QString::number(Now-2)},{"m",false}}).toJson(QJsonDocument::Compact);}
MarketCandleSeries minuteSeries(qint64 start,qint64 receipt){MarketCandleSeries result;result.available=true;result.identity="coin:binance:SYNUSDT";result.asset="SYN";result.currency="USDT";result.intervalMs=60000;result.receivedAtMs=receipt;result.sourceUrl=MarketCandles::request(result.identity,60000).url.toString();result.candles.append({start,0,"100","101","99","100","2"});return result;}
CoinFlowInstrument isolatedSpot(qint64 now=Now){auto value=instrument(CoinFlowVenue::BinanceSpot);value.quoteAsset="BTC";value.symbol="SYNBTC";value.identity="coin:binance:SYNBTC";value.evidence.receivedAtMs=value.evidence.knownAtMs=now-1000;return value;}
CoinFlowInstrument upbit(qint64 now=Now){auto value=instrument(CoinFlowVenue::BinanceSpot);value.venue=CoinFlowVenue::UpbitSpot;value.grain=CoinFlowGrain::UpbitTrade;value.quoteAsset="KRW";value.symbol="KRW-SYN";value.identity="coin:upbit:KRW-SYN";value.status.clear();value.evidence={"합성 업비트 메타데이터","https://api.upbit.com/v1/market/all",0,now-1000,now-1000};return value;}
QJsonObject spotRow(const QString& id,qint64 at,QString quantity="2"){return {{"e","aggTrade"},{"E",QString::number(at)},{"s","SYNBTC"},{"a",id},{"p","100"},{"q",quantity},{"f",id},{"l",id},{"T",QString::number(at)},{"m",false}};}
QByteArray spotFrame(const QString& id,qint64 at,QString quantity="2"){return QJsonDocument(spotRow(id,at,quantity)).toJson(QJsonDocument::Compact);}
QJsonObject upbitRow(QString id,qint64 at,bool websocket=false,QString stream="REALTIME"){QJsonObject row{{websocket?"code":"market","KRW-SYN"},{"trade_price","100"},{"trade_volume","2"},{"ask_bid","BID"},{"sequential_id",id},{websocket?"trade_timestamp":"timestamp",QString::number(at)}};if(websocket){row["type"]="trade";row["timestamp"]=QString::number(at);row["stream_type"]=stream;}return row;}
HttpResult rowsResponse(const QJsonArray& rows={}){HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);return response;}
bool coverageHas(const CoinFlowWindow& value,CoinFlowCoverage expected){for(const auto& period:value.periods)if(period.coverage==expected)return true;return false;}
}
class CoinFlowSourceTest final:public QObject {
    Q_OBJECT
  private slots:
    void constructorAndInactiveSelectionHaveNoNetwork();
    void activeUncachedSelectionFetchesOnlyPublicMetadataFirst();
    void verifiedCatalogOpensOnlySelectedSpotAndSamePairCompanion();
    void mismatchedOrExpiredMetadataNeverOpensACompanion();
    void minuteSeedAndLaterDecisionKeepOriginalReceiptAndFirstDecision();
    void metadataRevocationAndStopCannotRehydrateFromLateFrames();
    void retryAfterSurvivesActivityToggle();
    void metadataEvidenceUrlMustBeExactPublicCatalog_data();
    void metadataEvidenceUrlMustBeExactPublicCatalog();
    void identicalMinuteSeriesHasNoSpuriousChangedSignal();
    void replayedOldMinuteWindowDoesNotReinsertEvictedRows();
    void oversizedMinuteWindowDoesNotEmitAgainAfterTrimming();
    void conflictingClosedMinuteIsImmutableAndBreaksFollowingLink();
    void upbitHandshakeAndSnapshotCannotClaimLiveCoverage();
    void reconnectOverlapKeepsFirstObservedClocks();
    void recoveryStopsAtTenPagesWithoutInventingCoverage();
    void upbitRecoveryCrossesExplicitUtcDayWithoutDayZero();
    void stagedOverflowInvalidatesEpochAndLateFrames();
    void duplicateConflictPreservesOriginalAndRevokesOldSocket();
    void stagedConflictPreservesBothRawObservations_data();
    void stagedConflictPreservesBothRawObservations();
    void stagedDuplicateKeepsFirstReceiptAndCountsOneDelivery();
    void collectorStrongEvidenceUsesTheOriginalDecision();
    void metadataTimeoutCancelsAndRejectsLateResponse();
    void lowercaseGmtRetryAfterSurvivesToggle();
    void canonicalMinuteProofSurvivesPublicMetadataRevocation();
    void multipleCatchupMinutesAreHistoricalBaseline();
    void outOfOrderTradesAreHeldInTradeTimeOrder();
    void fullWindowTrimsOldestWholeMinutesInsteadOfWiping();
    void oneMinuteBeyondTheBudgetStillOverflowsHonestly();
};
void CoinFlowSourceTest::constructorAndInactiveSelectionHaveNoNetwork(){
    Http http;auto primary=std::make_unique<Socket>();auto companion=std::make_unique<Socket>();auto* first=primary.get();auto* second=companion.get();qint64 now=Now;
    CoinFlowSource source(&http,std::move(primary),std::move(companion),nullptr,[&]{return now;});source.setSelection("coin:binance:SYNUSDT");catalogs(source);source.pump();QVERIFY(!source.active());QVERIFY(http.requests.isEmpty());QVERIFY(first->endpoints.isEmpty());QVERIFY(second->endpoints.isEmpty());
}
void CoinFlowSourceTest::activeUncachedSelectionFetchesOnlyPublicMetadataFirst(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QVERIFY(source.active());QCOMPARE(http.requests.size(),1);const auto request=http.requests.first();QCOMPARE(request.url.path(),QString("/api/v3/exchangeInfo"));QVERIFY(!request.authenticated);QVERIFY(request.method=="GET"&&request.body.isEmpty());QVERIFY(!request.headers.contains("Authorization"));QVERIFY(!source.status().isEmpty());
}
void CoinFlowSourceTest::verifiedCatalogOpensOnlySelectedSpotAndSamePairCompanion(){
    Http http;auto primary=std::make_unique<Socket>();auto companion=std::make_unique<Socket>();auto* first=primary.get();auto* second=companion.get();qint64 now=Now;CoinFlowSource source(&http,std::move(primary),std::move(companion),nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);
    QCOMPARE(first->endpoints.size(),1);QCOMPARE(second->endpoints.size(),1);QCOMPARE(first->endpoints.first(),QUrl("wss://stream.binance.com:9443/ws/synusdt@aggTrade"));QCOMPARE(second->endpoints.first().path(),QString("/market/stream"));
    const auto snapshot=source.snapshot();QCOMPARE(snapshot.identity,QString("coin:binance:SYNUSDT"));QVERIFY(snapshot.primary.instrument.verified);QVERIFY(snapshot.companion.has_value());QCOMPARE(snapshot.companion->instrument.identity,QString("usdm_futures:binance:SYNUSDT"));
    for(const auto& period:snapshot.primary.periods)QVERIFY(period.coverage!=CoinFlowCoverage::ObservedContinuous);for(const auto& request:http.requests){QVERIFY(!request.authenticated);QVERIFY(request.method=="GET"&&request.body.isEmpty());QVERIFY(!request.url.path().contains("account"));}
}
void CoinFlowSourceTest::mismatchedOrExpiredMetadataNeverOpensACompanion(){
    Http http;auto primary=std::make_unique<Socket>();auto companion=std::make_unique<Socket>();auto* first=primary.get();auto* second=companion.get();qint64 now=Now;CoinFlowSource source(&http,std::move(primary),std::move(companion),nullptr,[&]{return now;});source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument(CoinFlowVenue::BinanceSpot)});auto wrong=instrument(CoinFlowVenue::BinanceUsdM);wrong.baseAsset="OTHER";source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{wrong});source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QCOMPARE(first->endpoints.size(),1);QVERIFY(second->endpoints.isEmpty());QVERIFY(!source.snapshot().companion.has_value());
    source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{instrument(CoinFlowVenue::BinanceUsdM)});QCOMPARE(second->endpoints.size(),1);now+=300001;source.pump();QVERIFY(!source.snapshot().primary.instrument.verified);QVERIFY(!source.snapshot().companion.has_value());
}
void CoinFlowSourceTest::minuteSeedAndLaterDecisionKeepOriginalReceiptAndFirstDecision(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);const auto start=Now-Now%60000-120000;auto series=minuteSeries(start,now);source.setMinuteSeries(series);auto value=source.snapshot();QCOMPARE(value.minutes.size(),1);QVERIFY(!value.minutes.first().liveEligible);QCOMPARE(value.minutes.first().knownAtMs,Now);
    now+=60000;series.candles.append({start+60000,0,"100","102","99","101","3"});series.receivedAtMs=now;source.setMinuteSeries(series);value=source.snapshot();QCOMPARE(value.minutes.size(),2);QVERIFY(value.minutes.last().liveEligible);const auto decision=value.minutes.last().decisionAtMs;QCOMPARE(decision,now);QCOMPARE(value.minutes.first().knownAtMs,Now);
    now+=1000;source.setMinuteSeries(series);value=source.snapshot();QCOMPARE(value.minutes.last().decisionAtMs,decision);QCOMPARE(value.minutes.last().knownAtMs,series.receivedAtMs);
}
void CoinFlowSourceTest::metadataRevocationAndStopCannotRehydrateFromLateFrames(){
    Http http;http.synchronous=true;auto primary=std::make_unique<Socket>();auto companion=std::make_unique<Socket>();auto* first=primary.get();qint64 now=Now;CoinFlowSource source(&http,std::move(primary),std::move(companion),nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QCOMPARE(first->generations.size(),1);const auto generation=first->generations.first();
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{});emit first->connected(generation);emit first->received(generation,frame());QVERIFY(source.snapshot().primary.trades.isEmpty());QVERIFY(!source.snapshot().primary.instrument.verified);const auto opened=first->endpoints.size();source.stop();emit first->received(generation,frame("9007199254740994"));source.pump();QVERIFY(!source.active());QVERIFY(source.snapshot().primary.trades.isEmpty());QCOMPARE(first->endpoints.size(),opened);
}
void CoinFlowSourceTest::retryAfterSurvivesActivityToggle(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QCOMPARE(http.requests.size(),1);HttpResult limited;limited.status=429;limited.headers.insert("retry-after","7");http.reply(0,limited);source.setActive(false);source.setActive(true);now+=6999;source.pump();QCOMPARE(http.requests.size(),1);now+=1;source.pump();QCOMPARE(http.requests.size(),2);
}
void CoinFlowSourceTest::metadataEvidenceUrlMustBeExactPublicCatalog_data(){
    QTest::addColumn<QString>("url");for(const auto* url:{"https://api.binance.com/api/v3/exchangeInfo?token=synthetic","https://api.binance.com:444/api/v3/exchangeInfo","https://api.binance.com/api/v3/exchangeInfo?permissions=MARGIN","https://api.binance.com/api/v3/exchangeInfo?permissions=SPOT&permissions=SPOT"})QTest::newRow(url)<<QString(url);
}
void CoinFlowSourceTest::metadataEvidenceUrlMustBeExactPublicCatalog(){
    QFETCH(QString,url);Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});auto item=isolatedSpot();item.evidence.sourceUrl=url;source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);QVERIFY(wire->endpoints.isEmpty());QVERIFY(!source.snapshot().primary.instrument.verified);
}
void CoinFlowSourceTest::identicalMinuteSeriesHasNoSpuriousChangedSignal(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);const auto series=minuteSeries(Now-120000,now);source.setMinuteSeries(series);QSignalSpy changed(&source,&CoinFlowSource::changed);const auto first=source.snapshot().minutes.first();source.setMinuteSeries(series);QCOMPARE(changed.size(),0);QCOMPARE(source.snapshot().minutes.first().decisionAtMs,first.decisionAtMs);QCOMPARE(source.snapshot().minutes.first().knownAtMs,first.knownAtMs);
}
void CoinFlowSourceTest::replayedOldMinuteWindowDoesNotReinsertEvictedRows(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);
    const auto start=Now-201*60000;auto old=minuteSeries(start,now);old.candles.clear();
    for(int i=0;i<200;++i)old.candles.append({start+i*60000,0,"100","101","99","100","2"});
    source.setMinuteSeries(old);QCOMPARE(source.snapshot().minutes.size(),200);
    now+=60000;auto current=old;current.receivedAtMs=now;current.candles.removeFirst();current.candles.append({start+200*60000,0,"100","102","99","101","3"});source.setMinuteSeries(current);
    const auto original=source.snapshot();QCOMPARE(original.minutes.size(),200);QVERIFY(original.minutes.last().liveEligible);QVERIFY(original.candleSeries.has_value());
    QSignalSpy changed(&source,&CoinFlowSource::changed);
    for(int i=0;i<5;++i){source.setMinuteSeries(old);source.setMinuteSeries(current);}
    QCOMPARE(changed.size(),0);const auto value=source.snapshot();QCOMPARE(value.minutes.first().candle.startsAtMs,start+60000);QCOMPARE(value.minutes.last().knownAtMs,original.minutes.last().knownAtMs);QCOMPARE(value.minutes.last().decisionAtMs,original.minutes.last().decisionAtMs);QCOMPARE(value.candleSeries->receivedAtMs,original.candleSeries->receivedAtMs);
}
void CoinFlowSourceTest::oversizedMinuteWindowDoesNotEmitAgainAfterTrimming(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);
    const auto start=Now-250*60000;auto series=minuteSeries(start,now);series.candles.clear();
    for(int i=0;i<250;++i)series.candles.append({start+i*60000,0,"100","101","99","100","2"});
    source.setMinuteSeries(series);QCOMPARE(source.snapshot().minutes.size(),200);QSignalSpy changed(&source,&CoinFlowSource::changed);
    source.setMinuteSeries(series);QCOMPARE(changed.size(),0);QCOMPARE(source.snapshot().minutes.first().candle.startsAtMs,start+50*60000);
}
void CoinFlowSourceTest::conflictingClosedMinuteIsImmutableAndBreaksFollowingLink(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);auto series=minuteSeries(Now-120000,now);source.setMinuteSeries(series);const auto initial=source.snapshot().minutes.first();now+=60000;series.receivedAtMs=now;series.candles.first().close="100.5";series.candles.append({Now-60000,0,"100","102","99","101","3"});source.setMinuteSeries(series);const auto value=source.snapshot();QCOMPARE(value.minutes.size(),2);QCOMPARE(value.minutes.first().candle.close,QString("100"));QCOMPARE(value.minutes.first().decisionAtMs,initial.decisionAtMs);QCOMPARE(value.minutes.first().knownAtMs,initial.knownAtMs);QVERIFY(!value.minutes.last().connectsPrevious);
}
void CoinFlowSourceTest::upbitHandshakeAndSnapshotCannotClaimLiveCoverage(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=upbit();source.setPublicCatalog(CoinFlowVenue::UpbitSpot,{item});source.setSelection(item.identity);source.setActive(true);QCOMPARE(wire->generations.size(),1);const auto generation=wire->generations.first();QVERIFY(wire->sent.isEmpty());const auto payload=QJsonDocument(upbitRow("1",now-1,true)).toJson(QJsonDocument::Compact);emit wire->received(generation,payload);QVERIFY(source.snapshot().primary.trades.isEmpty());emit wire->connected(generation);QCOMPARE(wire->sent.size(),1);const auto subscription=QJsonDocument::fromJson(wire->sent.first()).array();QCOMPARE(subscription.at(1).toObject().value("type").toString(),QString("trade"));QCOMPARE(subscription.at(1).toObject().value("codes").toArray(),QJsonArray{"KRW-SYN"});http.reply(0,rowsResponse());emit wire->received(generation,QJsonDocument(upbitRow("1",now-1,true,"SNAPSHOT")).toJson(QJsonDocument::Compact));QVERIFY(source.snapshot().primary.trades.isEmpty());QVERIFY(!coverageHas(source.snapshot().primary,CoinFlowCoverage::ObservedContinuous));emit wire->received(generation,payload);const auto value=source.snapshot();QCOMPARE(value.primary.trades.size(),1);QVERIFY(value.primary.trades.first().liveEligible);QCOMPARE(value.primary.trades.first().side,CoinFlowSide::Buy);QCOMPARE(value.primary.trades.first().knownAtMs,now);
}
void CoinFlowSourceTest::reconnectOverlapKeepsFirstObservedClocks(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=isolatedSpot();source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);emit wire->connected(wire->generations.last());QJsonArray seed;seed.append(spotRow("1",Now-1));http.reply(0,rowsResponse(seed));emit wire->failed(wire->generations.last());now+=999;source.pump();QCOMPARE(wire->generations.size(),1);now+=1;source.pump();QCOMPARE(wire->generations.size(),2);const auto current=wire->generations.last();emit wire->connected(current);QCOMPARE(QUrlQuery(http.requests.last().url).queryItemValue("fromId"),QString("1"));const auto firstReceipt=now;emit wire->received(current,spotFrame("2",now-1));now+=500;QJsonArray recovery;recovery.append(spotRow("1",Now-1));recovery.append(spotRow("2",firstReceipt-1));http.reply(1,rowsResponse(recovery));const auto value=source.snapshot();QVERIFY(coverageHas(value.primary,CoinFlowCoverage::RecoveredOverlap));QCOMPARE(value.primary.trades.size(),2);QCOMPARE(value.primary.trades.first().knownAtMs,Now);QCOMPARE(value.primary.trades.last().knownAtMs,firstReceipt);QCOMPARE(value.primary.trades.last().receivedAtMs,firstReceipt);QVERIFY(value.primary.trades.last().liveEligible);
}
void CoinFlowSourceTest::recoveryStopsAtTenPagesWithoutInventingCoverage(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=isolatedSpot();source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);emit wire->connected(wire->generations.last());QJsonArray seed;seed.append(spotRow("1",now-1));http.reply(0,rowsResponse(seed));emit wire->failed(wire->generations.last());now+=1000;source.pump();emit wire->connected(wire->generations.last());emit wire->received(wire->generations.last(),spotFrame("9000",now-1));
    for(int page=0;page<10;++page){QCOMPARE(http.requests.size(),page+2);QJsonArray rows;for(int i=0;i<500;++i)rows.append(spotRow(QString::number(10000+page*500+i),now-1));http.reply(page+1,rowsResponse(rows));now+=500;source.pump();}
    QCOMPARE(http.requests.size(),11);const auto value=source.snapshot();QVERIFY(coverageHas(value.primary,CoinFlowCoverage::Partial));QVERIFY(!coverageHas(value.primary,CoinFlowCoverage::RecoveredOverlap));QVERIFY(value.primary.reason.contains("10페이지"));
}
void CoinFlowSourceTest::upbitRecoveryCrossesExplicitUtcDayWithoutDayZero(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=QDateTime(QDate(2026,10,22),QTime(23,59,59),QTimeZone("UTC")).toMSecsSinceEpoch();CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=upbit(now);source.setPublicCatalog(CoinFlowVenue::UpbitSpot,{item});source.setSelection(item.identity);source.setActive(true);emit wire->connected(wire->generations.last());const auto anchorAt=now-1;QJsonArray seed;seed.append(upbitRow("1",anchorAt));http.reply(0,rowsResponse(seed));emit wire->failed(wire->generations.last());now+=2000;source.pump();emit wire->connected(wire->generations.last());QCOMPARE(http.requests.size(),2);QVERIFY(!QUrlQuery(http.requests.last().url).hasQueryItem("days_ago"));http.reply(1,rowsResponse());now+=500;source.pump();QCOMPARE(http.requests.size(),3);const auto query=QUrlQuery(http.requests.last().url);QCOMPARE(query.queryItemValue("days_ago"),QString("1"));QVERIFY(!query.hasQueryItem("cursor"));QJsonArray previous;previous.append(upbitRow("1",anchorAt));http.reply(2,rowsResponse(previous));QVERIFY(coverageHas(source.snapshot().primary,CoinFlowCoverage::Partial));for(const auto& request:http.requests)QVERIFY(QUrlQuery(request.url).queryItemValue("days_ago")!="0");
}
void CoinFlowSourceTest::stagedOverflowInvalidatesEpochAndLateFrames(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=isolatedSpot();source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);const auto old=wire->generations.last();emit wire->connected(old);for(int i=0;i<10001;++i)emit wire->received(old,spotFrame(QString::number(i+1),now-1));QVERIFY(coverageHas(source.snapshot().primary,CoinFlowCoverage::Overflow));QVERIFY(source.snapshot().primary.trades.isEmpty());emit wire->received(old,spotFrame("20000",now-1));http.reply(0,rowsResponse());QVERIFY(source.snapshot().primary.trades.isEmpty());QVERIFY(source.status().contains("예산"));
}
void CoinFlowSourceTest::duplicateConflictPreservesOriginalAndRevokesOldSocket(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=isolatedSpot();source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);const auto old=wire->generations.last();emit wire->connected(old);QJsonArray seed;seed.append(spotRow("1",now-1));http.reply(0,rowsResponse(seed));emit wire->received(old,spotFrame("1",now-1,"3"));const auto value=source.snapshot();QVERIFY(coverageHas(value.primary,CoinFlowCoverage::Partial));QCOMPARE(value.primary.trades.size(),1);QCOMPARE(value.primary.trades.first().quantity,QString("2"));QCOMPARE(value.primary.conflicts.size(),1);QCOMPARE(value.primary.conflicts.first().first.quantity,QString("2"));QCOMPARE(value.primary.conflicts.first().second.quantity,QString("3"));emit wire->received(old,spotFrame("2",now-1));QCOMPARE(source.snapshot().primary.trades.size(),1);
}
void CoinFlowSourceTest::stagedConflictPreservesBothRawObservations_data(){
    QTest::addColumn<bool>("fromRest");
    QTest::newRow("waiting-websocket") << false;
    QTest::newRow("rest-overlap") << true;
}
void CoinFlowSourceTest::stagedConflictPreservesBothRawObservations(){
    QFETCH(bool,fromRest);
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;
    CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=isolatedSpot();
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);
    const auto old=wire->generations.last();emit wire->connected(old);
    emit wire->received(old,spotFrame("1",Now-1,"2"));
    now+=1;
    if(fromRest)http.reply(0,rowsResponse(QJsonArray{spotRow("1",Now-1,"3")}));
    else emit wire->received(old,spotFrame("1",Now-1,"3"));
    const auto value=source.snapshot().primary;
    QVERIFY(coverageHas(value,CoinFlowCoverage::Partial));
    QCOMPARE(value.conflicts.size(),1);
    QCOMPARE(value.conflicts.first().first.quantity,QString("2"));
    QCOMPARE(value.conflicts.first().second.quantity,QString("3"));
    QCOMPARE(value.conflicts.first().first.receivedAtMs,Now);
    QCOMPARE(value.conflicts.first().second.receivedAtMs,Now+1);
    emit wire->received(old,spotFrame("2",Now,"9"));
    QCOMPARE(source.snapshot().primary.trades.size(),value.trades.size());
}
void CoinFlowSourceTest::stagedDuplicateKeepsFirstReceiptAndCountsOneDelivery(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;
    CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});const auto item=isolatedSpot();
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);
    const auto generation=wire->generations.last();emit wire->connected(generation);
    emit wire->received(generation,spotFrame("1",Now-1));
    now+=1;emit wire->received(generation,spotFrame("1",Now-1));
    http.reply(0,rowsResponse());
    const auto value=source.snapshot().primary;
    QCOMPARE(value.trades.size(),1);
    QCOMPARE(value.trades.first().receivedAtMs,Now);
    QCOMPARE(value.duplicateCount,1);
}
void CoinFlowSourceTest::collectorStrongEvidenceUsesTheOriginalDecision(){
    CoinFlowStudySet studies;
    studies.reversal.state={true,"current",CoinFlowCoverage::ObservedContinuous,Now+60000,Now+120000,Now+120001,999,{}};
    CoinFlowSignal original;
    original.kind=CoinFlowSignalKind::Rebound;original.atMs=Now;original.knownAtMs=Now+1;
    original.state={true,"original",CoinFlowCoverage::ObservedContinuous,Now-300000,Now,Now+1,5,{}};
    CoinReversalCorroboration corroboration;
    corroboration.volume.state={true,"original volume",CoinFlowCoverage::ObservedContinuous,Now-1200000,Now,Now+1,20,{}};
    corroboration.volume.currentVolume="400";corroboration.volume.previousMedian="100";corroboration.volume.multiple=4.0;
    corroboration.spot.state={true,"original spot",CoinFlowCoverage::RecoveredOverlap,Now-300000,Now,Now+1,321,{}};
    corroboration.spot.signedFraction=0.4;corroboration.spot.buyQuoteNotional="700";corroboration.spot.sellQuoteNotional="300";
    original.reversalCorroboration=corroboration;
    auto otherTime=original;otherTime.atMs-=60000;otherTime.state.samples=111;
    auto otherDecision=original;otherDecision.knownAtMs+=1;otherDecision.state.samples=222;
    auto otherKind=original;otherKind.kind=CoinFlowSignalKind::TurnDown;otherKind.state.samples=333;
    studies.entries={otherTime,otherDecision,otherKind,original};
    const auto stored=collectorStrongReversalEvidence(studies,CoinFlowSignalKind::Rebound,Now,Now+1);
    QCOMPARE(stored.value("from").toInteger(),Now-300000);
    QCOMPARE(stored.value("through").toInteger(),Now);
    QCOMPARE(stored.value("k").toInteger(),Now+1);
    QCOMPARE(stored.value("samples").toInt(),5);
    const auto volume=stored.value("volume").toObject(),spot=stored.value("spot").toObject();
    QCOMPARE(volume.value("samples").toInt(),20);QCOMPARE(volume.value("from").toInteger(),Now-1200000);
    QCOMPARE(volume.value("currentVolume").toString(),QString("400"));QCOMPARE(volume.value("multiple").toDouble(),4.0);
    QCOMPARE(spot.value("samples").toInt(),321);QCOMPARE(spot.value("coverage").toString(),QString("recovered_overlap"));
    QCOMPARE(spot.value("signedFraction").toDouble(),0.4);QCOMPARE(spot.value("buyQuoteNotional").toString(),QString("700"));
    const auto missing=collectorStrongReversalEvidence(studies,CoinFlowSignalKind::Rebound,Now-1,Now+1);
    QVERIFY(!missing.value("matched").toBool());QVERIFY(!missing.contains("samples"));
    QVERIFY(!missing.value("why").toString().isEmpty());
}
void CoinFlowSourceTest::metadataTimeoutCancelsAndRejectsLateResponse(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QCOMPARE(http.requests.size(),1);now+=9999;source.pump();QCOMPARE(http.aborts,0);now+=1;source.pump();QCOMPARE(http.aborts,1);HttpResult late;late.status=200;late.body=R"({"symbols":[{"symbol":"SYNUSDT","baseAsset":"SYN","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})";http.reply(0,late);QVERIFY(!source.snapshot().primary.instrument.verified);now+=4999;source.pump();QCOMPARE(http.requests.size(),1);now+=1;source.pump();QCOMPARE(http.requests.size(),2);
}
void CoinFlowSourceTest::lowercaseGmtRetryAfterSurvivesToggle(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});source.setSelection("coin:binance:SYNUSDT");source.setActive(true);HttpResult limited;limited.status=429;limited.headers.insert("retry-after",QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(now+7000,QTimeZone("UTC")),"ddd, dd MMM yyyy HH:mm:ss 'GMT'").toLatin1());http.reply(0,limited);source.setActive(false);source.setActive(true);now+=6999;source.pump();QCOMPARE(http.requests.size(),1);now+=1;source.pump();QCOMPARE(http.requests.size(),2);
}
void CoinFlowSourceTest::canonicalMinuteProofSurvivesPublicMetadataRevocation(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);const auto series=minuteSeries(Now-120000,now);source.setMinuteSeries(series);now+=1000;source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{});const auto snapshot=source.snapshot();QVERIFY(!snapshot.primary.instrument.verified);QCOMPARE(snapshot.minutes.size(),1);QVERIFY(snapshot.candleSeries.has_value());QCOMPARE(snapshot.candleSeries->identity,series.identity);QCOMPARE(snapshot.candleSeries->asset,series.asset);QCOMPARE(snapshot.candleSeries->currency,series.currency);QCOMPARE(snapshot.candleSeries->sourceUrl,series.sourceUrl);QCOMPARE(snapshot.candleSeries->receivedAtMs,Now);QCOMPARE(snapshot.candleSeries->candles.first().close,QString("100"));now+=1000;QCOMPARE(source.snapshot().candleSeries->receivedAtMs,Now);source.stop();QVERIFY(!source.snapshot().candleSeries.has_value());
}
void CoinFlowSourceTest::multipleCatchupMinutesAreHistoricalBaseline(){
    Http http;qint64 now=Now;CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});catalogs(source);source.setSelection("coin:binance:SYNUSDT");source.setActive(true);auto series=minuteSeries(Now-180000,now);source.setMinuteSeries(series);now+=60000;series.receivedAtMs=now;series.candles.append({Now-120000,0,"100","102","99","101","3"});series.candles.append({Now-60000,0,"101","103","100","102","4"});source.setMinuteSeries(series);const auto snapshot=source.snapshot();QCOMPARE(snapshot.minutes.size(),3);QCOMPARE(snapshot.minutes.first().knownAtMs,Now);for(const auto& row:snapshot.minutes)QVERIFY(!row.liveEligible);
}
// The collector inserts each trade at its trade-time position instead of re-sorting the whole
// window per trade (main-thread cost grew with every held trade). Order stays exactly as a stable
// sort by trade time: earlier times first, equal times in arrival order.
void CoinFlowSourceTest::outOfOrderTradesAreHeldInTradeTimeOrder(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});
    const auto item=isolatedSpot();source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{item});source.setSelection(item.identity);source.setActive(true);
    emit wire->connected(wire->generations.last());http.reply(0,rowsResponse());
    const QList<QPair<QString,qint64>> arrivals{{"10",Now-50},{"11",Now-90},{"12",Now-50},{"13",Now-70},{"14",Now-10},{"15",Now-90}};
    for(const auto& trade:arrivals){emit wire->received(wire->generations.last(),spotFrame(trade.first,trade.second));}
    QStringList order;for(const auto& trade:source.snapshot().primary.trades)order<<trade.id;
    QCOMPARE(order,(QStringList{"11","15","13","10","12","14"}));
    // A repeated identical trade is a duplicate, not a second row.
    emit wire->received(wire->generations.last(),spotFrame("13",Now-70));QCOMPARE(source.snapshot().primary.trades.size(),6);QCOMPARE(source.snapshot().primary.duplicateCount,1);
}
// Before: at 10,000 held trades the collector wiped the window, marked everything overflow,
// reconnected and re-read up to 10 REST pages (Binance BTC reaches 10,000 every 15-18 minutes).
// Now it drops the oldest whole minutes with their coverage and keeps observing.
void CoinFlowSourceTest::fullWindowTrimsOldestWholeMinutesInsteadOfWiping(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now-Now%60000;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{isolatedSpot(now)});source.setSelection("coin:binance:SYNBTC");source.setActive(true);
    emit wire->connected(wire->generations.last());http.reply(0,rowsResponse());
    const auto requests=http.requests.size();const auto generation=wire->generations.last();const auto start=now;
    const auto stream=[&](int from,int count,qint64 spacing){for(int i=from;i<from+count;++i){now+=spacing;
        if(i%2000==0)source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{isolatedSpot(now)});
        emit wire->received(wire->generations.last(),spotFrame(QString::number(100000+i),now-1));}};
    stream(0,9999,30);QCOMPARE(source.snapshot().primary.trades.size(),9999);
    stream(9999,2500,30); // Crosses the old wipe point.
    const auto value=source.snapshot().primary;
    QCOMPARE(wire->generations.size(),1);QCOMPARE(wire->generations.last(),generation);QCOMPARE(http.requests.size(),requests);
    QVERIFY2(!source.status().contains(QStringLiteral("예산 초과")),qPrintable(source.status()));
    QVERIFY2(value.trades.size()>=8000&&value.trades.size()<=10000,qPrintable(QString::number(value.trades.size())));
    QCOMPARE(value.trades.last().id,QString::number(100000+12498));
    // Only a prefix of whole minutes went; what is held is still in order and complete after it.
    const auto first=value.trades.first().tradeAtMs,boundary=first-first%60000;QVERIFY(boundary>start);
    for(int i=1;i<value.trades.size();++i){QVERIFY(value.trades[i-1].tradeAtMs<=value.trades[i].tradeAtMs);QCOMPARE(value.trades[i].id.toLongLong(),value.trades[i-1].id.toLongLong()+1);}
    // Coverage before the held trades is withdrawn; no overflow mark; later minutes stay observed.
    bool observedLater=false;
    for(const auto& period:value.periods){QVERIFY2(period.fromMs>=boundary,qPrintable(QString::number(period.fromMs-boundary)));QVERIFY(period.coverage!=CoinFlowCoverage::Overflow);
        if(period.coverage==CoinFlowCoverage::ObservedContinuous)observedLater=true;}
    QVERIFY(observedLater);
    // A trade from a dropped minute is not held again.
    emit wire->received(wire->generations.last(),spotFrame("1",boundary-1000));
    QCOMPARE(source.snapshot().primary.trades.size(),value.trades.size());QCOMPARE(source.snapshot().primary.trades.first().id,value.trades.first().id);
    // Studies still read a full, valid window.
    const auto studies=coinFlowStudies(source.snapshot(),now);QVERIFY2(studies.cvd.state.available,qPrintable(studies.cvd.state.reason));
    QVERIFY(!studies.cvd.points.isEmpty());QVERIFY(studies.cvd.points.first().atMs>=boundary);
}
void CoinFlowSourceTest::oneMinuteBeyondTheBudgetStillOverflowsHonestly(){
    Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 now=Now-Now%60000;CoinFlowSource source(&http,std::move(socket),{},nullptr,[&]{return now;});
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{isolatedSpot(now)});source.setSelection("coin:binance:SYNBTC");source.setActive(true);
    emit wire->connected(wire->generations.last());http.reply(0,rowsResponse());
    // 10,001 trades inside one minute: no whole earlier minute can make room.
    for(int i=0;i<10001;++i){now+=i%2;emit wire->received(wire->generations.last(),spotFrame(QString::number(500000+i),now-1));}
    QVERIFY2(source.status().contains(QStringLiteral("예산 초과")),qPrintable(source.status()));QVERIFY(source.snapshot().primary.trades.isEmpty());
    bool overflow=false;for(const auto& period:source.snapshot().primary.periods)overflow=overflow||period.coverage==CoinFlowCoverage::Overflow;QVERIFY(overflow);
}
QTEST_GUILESS_MAIN(CoinFlowSourceTest)
#include "tst_coin_flow_source.moc"
