#include "keefi/ChartPublicContextSource.h"
#include "keefi/TradingTransport.h"
#include "keefi/NetworkPublicFlowSocket.h"
#include <QSignalSpy>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
using namespace keefi;
namespace {
using Read=ChartPublicContextSource::Read;
constexpr qint64 Hour=3600000,Day=24*Hour;
qint64 at(const char* iso){return QDateTime::fromString(QString::fromLatin1(iso),Qt::ISODate).toMSecsSinceEpoch();}
const qint64 Now=at("2026-10-29T02:30:00Z");
HttpResult ok(QByteArray body){HttpResult r;r.status=200;r.body=std::move(body);return r;}
class FakeTransport final:public TradingTransport {
 public:
 QList<RequestSpec> sent;QList<quint64> tokens;int aborts=0;
 void send(quint64 token,const RequestSpec& request)override{sent.append(request);tokens.append(token);}
 void abort()override{++aborts;}
 void answer(const HttpResult& r){emit finished(tokens.last(),r);}
};
class FakeSocket final:public PublicCandleSocket {
 public:
 QList<QUrl> opened;quint64 generation=0;int aborts=0;
 void open(quint64 value,const QUrl& endpoint)override{generation=value;opened.append(endpoint);}
 void send(quint64,const QByteArray&)override{}
 void abort()override{++aborts;}
};
QByteArray liquidation(qint64 now,QString average="50000",QString price="0"){
 QJsonObject order{{"s","BTCUSDT"},{"S","SELL"},{"o","LIMIT"},{"f","IOC"},{"q","2"},{"p",price},{"ap",average},{"X","FILLED"},{"l","2"},{"z","2"},{"T",double(now)}};
 return QJsonDocument(QJsonObject{{"e","forceOrder"},{"E",double(now+1)},{"o",order}}).toJson(QJsonDocument::Compact);
}
QByteArray contracts(){return R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})";}
QByteArray funding(qint64 now){return QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0001"},{"interestRate","0.0001"},{"time",double(now)},{"nextFundingTime",double(now+Hour)}}).toJson();}
QJsonObject upbit(qint64 from){return {{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(from,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},{"timestamp",double(from+1)},{"opening_price","100"},{"high_price","120"},{"low_price","90"},{"trade_price","110"},{"candle_acc_trade_volume","10"}};}
QByteArray rows(QJsonArray a){return QJsonDocument(a).toJson();}
QByteArray oi(qint64 now,const char* first="100",const char* last="110"){auto end=now/Hour*Hour;return rows({QJsonObject{{"symbol","BTCUSDT"},{"timestamp",double(end-Hour)},{"sumOpenInterest",first}},QJsonObject{{"symbol","BTCUSDT"},{"timestamp",double(end)},{"sumOpenInterest",last}}});}
void next(ChartPublicContextSource& source,qint64& now){now+=1000;source.pump();}
}
class ChartPublicContextTest:public QObject {
 Q_OBJECT
 private slots:
 void fixedUnsignedRequests(){
  for(const auto id:{QString("coin:upbit:KRW-BTC"),QString("coin:binance:BTCUSDT")})for(auto read:{Read::Day,Read::Week,Read::Month,Read::OpenInterest,Read::TopPosition,Read::Taker,Read::FuturesPrice}){
   const auto request=ChartPublicContextSource::request(read,id,"BTCUSDT");QVERIFY(ChartPublicContextSource::isRequest(request));QVERIFY(NetworkTradingTransport::allowed(request));
   auto bad=request;bad.authenticated=true;QVERIFY(!ChartPublicContextSource::isRequest(bad));bad=request;bad.method="POST";QVERIFY(!ChartPublicContextSource::isRequest(bad));bad=request;bad.body="x";QVERIFY(!ChartPublicContextSource::isRequest(bad));bad=request;bad.headers.insert("X-MBX-APIKEY","secret");QVERIFY(!ChartPublicContextSource::isRequest(bad));bad=request;bad.url.setQuery(bad.url.query()+"&period=1h");QVERIFY(!ChartPublicContextSource::isRequest(bad));bad=request;bad.url.setHost("example.test");QVERIFY(!ChartPublicContextSource::isRequest(bad));
  }
  QVERIFY(ChartPublicContextSource::request(Read::Day,"stock:us:AAPL").url.isEmpty());
 }
 void completedCalendarPeriodsOnly(){
  const auto day=at("2026-10-28T00:00:00Z"),week=at("2026-10-19T00:00:00Z"),month=at("2026-09-01T00:00:00Z");
  for(auto pair:{qMakePair(Read::Day,day),qMakePair(Read::Week,week),qMakePair(Read::Month,month)}){
   const auto result=ChartPublicContextSource::parseLevels(pair.first,"coin:upbit:KRW-BTC",ok(rows({upbit(pair.second)})),Now,Now);QCOMPARE(result.size(),3);for(const auto& c:result){QCOMPARE(c.fromMs,pair.second);QVERIFY(c.complete);QVERIFY(c.throughMs<=Now);QCOMPARE(c.knownAtMs,Now);}
  }
  auto forming=upbit(at("2026-10-29T00:00:00Z"));QVERIFY(ChartPublicContextSource::parseLevels(Read::Day,"coin:upbit:KRW-BTC",ok(rows({forming})),Now,Now).isEmpty());
  QVERIFY(ChartPublicContextSource::parseLevels(Read::Day,"coin:upbit:KRW-BTC",ok(rows({upbit(day-Day)})),Now,Now).isEmpty());
  auto invalid=upbit(day);invalid["high_price"]="95";QVERIFY(ChartPublicContextSource::parseLevels(Read::Day,"coin:upbit:KRW-BTC",ok(rows({invalid})),Now,Now).isEmpty());
  auto early=ChartPublicContextSource::parseLevels(Read::Day,"coin:upbit:KRW-BTC",ok(rows({upbit(day),upbit(at("2026-10-26T00:00:00Z"))})),Now,Now);QCOMPARE(early.size(),5);
 }
 void lazyIndependentPeriodsPacedAndStop(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");QVERIFY(transport.sent.isEmpty());source.setDemand(true,false);QCOMPARE(transport.sent.size(),1);QVERIFY(transport.sent.last().url.path().endsWith("days"));transport.answer(ok(rows({upbit(at("2026-10-28T00:00:00Z"))})));QCOMPARE(transport.sent.size(),1);now+=999;source.pump();QCOMPARE(transport.sent.size(),1);++now;source.pump();QCOMPARE(transport.sent.size(),2);QVERIFY(transport.sent.last().url.path().endsWith("weeks"));transport.answer(ok(rows({upbit(at("2026-10-19T00:00:00Z"))})));next(source,now);QCOMPARE(transport.sent.size(),3);QVERIFY(transport.sent.last().url.path().endsWith("months"));transport.answer(ok(rows({upbit(at("2026-09-01T00:00:00Z"))})));QCOMPARE(source.snapshot().levelCandidates.size(),9);
  source.setDemand(false,false);QVERIFY(source.snapshot().levelCandidates.isEmpty());now+=60000;source.pump();QCOMPARE(transport.sent.size(),3);
 }
 void verifiedFuturesAndZeroQuantity(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);QVERIFY(transport.sent.last().url.path().endsWith("exchangeInfo"));transport.answer(ok(contracts()));QVERIFY(source.snapshot().futuresSupported);next(source,now);transport.answer(ok(funding(now)));next(source,now);QVERIFY(transport.sent.last().url.path().endsWith("openInterestHist"));transport.answer(ok(oi(now,"100","0")));const auto result=source.snapshot();QCOMPARE(result.openInterest.size(),2);QVERIFY(result.oiChange.available);QCOMPARE(result.oiChange.value.toDouble(),-100.0);QCOMPARE(result.openInterestUnit,QString("BTC"));
 }
 void catalogueExpiresBeforeFuturesRefresh(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);transport.answer(ok(contracts()));
  for(int i=0;i<5;++i){next(source,now);transport.answer(ok("[]"));}
  now=Now+181000;source.pump();QVERIFY2(transport.sent.last().url.path().endsWith("exchangeInfo"),qPrintable(transport.sent.last().url.toString()));QVERIFY(!source.snapshot().futuresSupported);
 }
 void deniedTopHiddenAndOtherCellRemains(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);transport.answer(ok(contracts()));next(source,now);transport.answer(ok(funding(now)));next(source,now);transport.answer(ok(oi(now)));next(source,now);transport.answer(ok("[]"));next(source,now);HttpResult denied;denied.status=403;transport.answer(denied);QVERIFY(source.snapshot().topPosition.hidden);QVERIFY(source.snapshot().funding.available);QVERIFY(source.snapshot().oiChange.available);next(source,now);QVERIFY(transport.sent.last().url.path().endsWith("takerlongshortRatio"));
 }
 void sameContractPriceAndOiUseExactHour(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);transport.answer(ok(contracts()));next(source,now);transport.answer(ok(funding(now)));next(source,now);transport.answer(ok(oi(now)));next(source,now);
  QVERIFY(transport.sent.last().url.path()=="/fapi/v1/klines");QVERIFY(transport.sent.last().url.host()=="fapi.binance.com");QVERIFY(transport.sent.last().url.query().contains("symbol=BTCUSDT"));
  const auto end=now/Hour*Hour;
  const auto candle=[](qint64 start,const char* close){return QJsonArray{double(start),"100","130","90",close,"10",double(start+Hour-1),"1000",10,"5","500","0"};};
  transport.answer(ok(rows({candle(end-2*Hour,"100"),candle(end-Hour,"120"),candle(end,"110")})));
  const auto result=source.snapshot();QVERIFY(result.priceChange.available);QCOMPARE(result.priceChange.value.toDouble(),20.0);QCOMPARE(result.priceChange.fromMs,result.oiChange.fromMs);QCOMPARE(result.priceChange.throughMs,result.oiChange.throughMs);QVERIFY(result.priceChange.sourceUrl.contains("fapi.binance.com"));
 }
 void zeroDenominatorAndMalformedOiDoNotInventNumbers(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);transport.answer(ok(contracts()));next(source,now);transport.answer(ok(funding(now)));next(source,now);transport.answer(ok(oi(now,"0","110")));QVERIFY(!source.snapshot().oiChange.available);QCOMPARE(source.snapshot().openInterest.size(),2);
 }
 void repeatedSelectionDoesNotRestartAndDemandHidesProjection(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(true,false);transport.answer(ok(rows({upbit(at("2026-10-28T00:00:00Z"))})));source.setSelection("coin:upbit:KRW-BTC");QCOMPARE(transport.aborts,0);QCOMPARE(source.snapshot().levelCandidates.size(),3);source.setDemand(false,true);QVERIFY(source.snapshot().levelCandidates.isEmpty());
 }
 void liquidationPublishedAtMinuteAndReconnectPayloadDedupe(){
  qint64 now=Now;FakeTransport transport;auto socket=std::make_unique<FakeSocket>();auto* stream=socket.get();ChartPublicContextSource source(&transport,std::move(socket),nullptr,[&]{return now;});QSignalSpy events(&source,&ChartPublicContextSource::liquidationObserved);
  source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);QVERIFY(stream->opened.isEmpty());transport.answer(ok(contracts()));QCOMPARE(stream->opened.size(),1);QVERIFY(isPublicFlowSocketEndpoint(stream->opened.last()));QCOMPARE(stream->opened.last().path(),QString("/market/ws/btcusdt@forceOrder"));emit stream->connected(stream->generation);
  now+=1000;const auto raw=liquidation(now-1);emit stream->received(stream->generation,raw);emit stream->received(stream->generation,raw);QVERIFY(source.snapshot().liquidation.rows.isEmpty());QCOMPARE(events.size(),0);
  now=Now+60000;source.pump();auto feed=source.snapshot().liquidation;QCOMPARE(feed.observedCount,1);QCOMPARE(feed.rows.size(),1);QCOMPARE(feed.rows.first().quoteNotional,QString("100000"));QCOMPARE(feed.rows.first().observation.price,QString("0"));QCOMPARE(feed.rows.first().observation.side,QString("SELL"));QCOMPARE(events.size(),1);QVERIFY(events.first()[2].toString().contains("부분 기록"));
  source.setLiquidationMinimum("200000");QVERIFY(source.snapshot().liquidation.rows.isEmpty());source.setLiquidationMinimum("100000");QCOMPARE(source.snapshot().liquidation.rows.size(),1);QCOMPARE(events.size(),1);
  const auto oldGeneration=stream->generation;emit stream->failed(oldGeneration,0);now+=5000;source.pump();QCOMPARE(stream->opened.size(),2);emit stream->connected(stream->generation);emit stream->received(stream->generation,raw);emit stream->received(oldGeneration,liquidation(now-1));now=Now+120000;source.pump();QCOMPARE(source.snapshot().liquidation.observedCount,1);QCOMPARE(events.size(),1);
  source.setDemand(false,false);QVERIFY(!source.snapshot().liquidation.supported);const auto opens=stream->opened.size();now+=60000;source.pump();QCOMPARE(stream->opened.size(),opens);
 }
 void liquidationMissingAverageIsNotReplacedByOrderPrice(){
  qint64 now=Now;FakeTransport transport;auto socket=std::make_unique<FakeSocket>();auto* stream=socket.get();ChartPublicContextSource source(&transport,std::move(socket),nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(false,true);transport.answer(ok(contracts()));emit stream->connected(stream->generation);now+=1000;emit stream->received(stream->generation,liquidation(now-1,"","50000"));now=Now+60000;source.pump();QCOMPARE(source.snapshot().liquidation.observedCount,0);QVERIFY(source.snapshot().liquidation.rows.isEmpty());
 }
 void selectionInvalidationAndRetryAfterPersist(){
  qint64 now=Now;FakeTransport transport;ChartPublicContextSource source(&transport,nullptr,[&]{return now;});source.setSelection("coin:upbit:KRW-BTC");source.setDemand(true,false);const auto oldToken=transport.tokens.last();HttpResult limited;limited.status=429;limited.headers.insert("Retry-After","30");transport.answer(limited);source.setDemand(false,false);source.setSelection("coin:upbit:KRW-ETH");source.setDemand(true,false);QCOMPARE(transport.sent.size(),1);emit transport.finished(oldToken,ok(rows({upbit(at("2026-10-28T00:00:00Z"))})));QVERIFY(source.snapshot().levelCandidates.isEmpty());now+=29999;source.pump();QCOMPARE(transport.sent.size(),1);++now;source.pump();QCOMPARE(transport.sent.size(),2);QVERIFY(transport.sent.last().url.query().contains("KRW-ETH"));
 }
};
QTEST_GUILESS_MAIN(ChartPublicContextTest)
#include "tst_chart_public_context.moc"
