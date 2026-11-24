#include "keefi/CoinFlowProtocol.h"
#include "keefi/NetworkPublicFlowSocket.h"
#include "keefi/PublicFlowFrameQueue.h"
#include "keefi/TradingTransport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUrlQuery>
using namespace keefi;
namespace {
constexpr qint64 Now=1792710000000;
CoinFlowInstrument instrument(CoinFlowVenue venue=CoinFlowVenue::BinanceSpot){
    CoinFlowInstrument result;result.venue=venue;result.verified=true;result.baseAsset="SYN";result.status="TRADING";
    result.quoteAsset=venue==CoinFlowVenue::UpbitSpot?"KRW":"USDT";result.symbol=venue==CoinFlowVenue::UpbitSpot?"KRW-SYN":"SYNUSDT";
    result.identity=venue==CoinFlowVenue::UpbitSpot?"coin:upbit:KRW-SYN":venue==CoinFlowVenue::BinanceSpot?"coin:binance:SYNUSDT":"usdm_futures:binance:SYNUSDT";
    result.grain=venue==CoinFlowVenue::UpbitSpot?CoinFlowGrain::UpbitTrade:venue==CoinFlowVenue::BinanceSpot?CoinFlowGrain::SpotAggregate:CoinFlowGrain::Futures100msAggregate;
    if(venue==CoinFlowVenue::BinanceUsdM){result.marginAsset="USDT";result.contractType="PERPETUAL";result.underlyingType="COIN";}
    result.evidence={"합성 공식 메타데이터",venue==CoinFlowVenue::BinanceUsdM?"https://fapi.binance.com/fapi/v1/exchangeInfo":venue==CoinFlowVenue::BinanceSpot?"https://api.binance.com/api/v3/exchangeInfo":"https://api.upbit.com/v1/market/all?isDetails=true",0,Now-1000,Now-1000};return result;
}
QJsonObject trade(QString id="9007199254740993",bool maker=false){return {{"e","aggTrade"},{"E",QString::number(Now-1)},{"s","SYNUSDT"},{"a",id},{"p","100.25"},{"q","3"},{"f","90071992547409930"},{"l","90071992547409935"},{"T",QString::number(Now-2)},{"m",maker}};}
QByteArray bytes(const QJsonObject& value){return QJsonDocument(value).toJson(QJsonDocument::Compact);}
HttpResult response(const QJsonValue& value){HttpResult result;result.status=200;result.body=value.isArray()?QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact):QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);return result;}
QJsonObject futureCatalog(){return {{"symbols",QJsonArray{QJsonObject{{"symbol","SYNUSDT"},{"baseAsset","SYN"},{"quoteAsset","USDT"},{"marginAsset","USDT"},{"contractType","PERPETUAL"},{"status","TRADING"},{"underlyingType","COIN"}}}}};}
}
class CoinFlowProtocolTest final:public QObject {
    Q_OBJECT
  private slots:
    void unsignedReadsUseExactReferenceBudgets_data();
    void unsignedReadsUseExactReferenceBudgets();
    void requestVariantsRejectMixedCursorsAndUndocumentedParameters();
    void actualBuildersPassNativeTransportAllowlist();
    void nativeTransportAcceptsVerifiedCombiningMarkSymbol();
    void socketAllowlistKeepsSpotAndRoutedFuturesSeparate();
    void catalogPreservesOfficialPairAndInternalFuturesIdentity();
    void aggregateIdsAndMakerDirectionRemainExact();
    void decimalSourceBoundsRemainExact_data();
    void decimalSourceBoundsRemainExact();
    void normalQuantityUsesExactSubUlpComparison();
    void malformedTradeFrameRejectsAtomically_data();
    void malformedTradeFrameRejectsAtomically();
    void futuresQuantityIncludesRpiAndNormalQuantityStaysSeparate();
    void upbitSnapshotUsesSourceDirectionAndActualClocks();
    void openInterestHistoryKeepsEndTimesAndMissingPeriods();
    void liquidationKeepsLossySnapshotFieldsWithoutInventedOrderId();
    void liquidationMarketSnapshotKeepsZeroPrices();
    void smallClockLeadWaitsWithoutChangingTheFrame();
    void bufferedFramesKeepWireOrderAndExpireOnClockRollback();
    void clockBufferIsBoundedAndResetDropsOldFrames();
    void farFutureFramesRemainRejected();
};
void CoinFlowProtocolTest::unsignedReadsUseExactReferenceBudgets_data(){
    QTest::addColumn<int>("venue");QTest::addColumn<int>("read");QTest::addColumn<QString>("host");QTest::addColumn<QString>("path");
    QTest::newRow("upbit-trades")<<int(CoinFlowVenue::UpbitSpot)<<int(CoinFlowRead::Trades)<<QString("api.upbit.com")<<QString("/v1/trades/ticks");
    QTest::newRow("spot-aggregate")<<int(CoinFlowVenue::BinanceSpot)<<int(CoinFlowRead::Trades)<<QString("api.binance.com")<<QString("/api/v3/aggTrades");
    QTest::newRow("futures-aggregate")<<int(CoinFlowVenue::BinanceUsdM)<<int(CoinFlowRead::Trades)<<QString("fapi.binance.com")<<QString("/fapi/v1/aggTrades");
    QTest::newRow("native-oi5m")<<int(CoinFlowVenue::BinanceUsdM)<<int(CoinFlowRead::OpenInterest)<<QString("fapi.binance.com")<<QString("/futures/data/openInterestHist");
}
void CoinFlowProtocolTest::unsignedReadsUseExactReferenceBudgets(){
    QFETCH(int,venue);QFETCH(int,read);QFETCH(QString,host);QFETCH(QString,path);const auto item=instrument(CoinFlowVenue(venue));
    const auto request=CoinFlowProtocol::request(CoinFlowRead(read),CoinFlowVenue(venue),item.symbol);QVERIFY(CoinFlowProtocol::isRequest(request));
    QCOMPARE(request.operation,Operation::Reference);QCOMPARE(request.method,QByteArray("GET"));QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());QVERIFY(request.body.isEmpty());QCOMPARE(request.url.host(),host);QCOMPARE(request.url.path(),path);
    const QUrlQuery query(request.url);QCOMPARE(query.queryItemValue(read==int(CoinFlowRead::OpenInterest)?"limit":venue==int(CoinFlowVenue::UpbitSpot)?"count":"limit"),QString(read==int(CoinFlowRead::OpenInterest)?"30":"500"));
    if(read==int(CoinFlowRead::OpenInterest))QCOMPARE(query.queryItemValue("period"),QString("5m"));
    auto extra=request;auto q=query;q.addQueryItem("account","synthetic-prohibited");extra.url.setQuery(q);QVERIFY(!CoinFlowProtocol::isRequest(extra));
}
void CoinFlowProtocolTest::requestVariantsRejectMixedCursorsAndUndocumentedParameters(){
    CoinFlowPageCursor cursor;cursor.tradeId="9007199254740993";auto request=CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::BinanceSpot,"SYNUSDT",cursor);QVERIFY(CoinFlowProtocol::isRequest(request));QCOMPARE(QUrlQuery(request.url).queryItemValue("fromId"),cursor.tradeId);
    cursor.fromMs=Now-60000;cursor.throughMs=Now;QVERIFY(!CoinFlowProtocol::isRequest(CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::BinanceSpot,"SYNUSDT",cursor)));
    cursor.tradeId.clear();QVERIFY(CoinFlowProtocol::isRequest(CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::BinanceUsdM,"SYNUSDT",cursor)));
    cursor.throughMs=cursor.fromMs+3600000;QVERIFY(!CoinFlowProtocol::isRequest(CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::BinanceUsdM,"SYNUSDT",cursor)));
    cursor={};cursor.daysAgo=0;QVERIFY(!CoinFlowProtocol::isRequest(CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::UpbitSpot,"KRW-SYN",cursor)));
    cursor.daysAgo=1;request=CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::UpbitSpot,"KRW-SYN",cursor);QVERIFY(CoinFlowProtocol::isRequest(request));QCOMPARE(QUrlQuery(request.url).queryItemValue("days_ago"),QString("1"));
}
void CoinFlowProtocolTest::actualBuildersPassNativeTransportAllowlist(){
    int routes=0;
    for(const auto venue:{CoinFlowVenue::UpbitSpot,CoinFlowVenue::BinanceSpot,CoinFlowVenue::BinanceUsdM}){
        const auto item=instrument(venue);const auto catalog=CoinFlowProtocol::request(CoinFlowRead::Catalog,venue);QVERIFY(CoinFlowProtocol::isRequest(catalog));QVERIFY2(NetworkTradingTransport::allowed(catalog),qPrintable(catalog.url.toString()));++routes;
        for(int variant=0;variant<3;++variant){CoinFlowPageCursor cursor;if(variant==1)cursor.tradeId="9007199254740993";if(variant==2){if(venue==CoinFlowVenue::UpbitSpot)cursor.daysAgo=7;else{cursor.fromMs=Now-60000;cursor.throughMs=Now;}}
            const auto request=CoinFlowProtocol::request(CoinFlowRead::Trades,venue,item.symbol,cursor);QVERIFY(CoinFlowProtocol::isRequest(request));QVERIFY2(NetworkTradingTransport::allowed(request),qPrintable(request.url.toString()));}++routes;
    }
    for(const bool ranged:{false,true}){CoinFlowPageCursor cursor;if(ranged){cursor.fromMs=Now-600000;cursor.throughMs=Now;}const auto request=CoinFlowProtocol::request(CoinFlowRead::OpenInterest,CoinFlowVenue::BinanceUsdM,"SYNUSDT",cursor);QVERIFY(CoinFlowProtocol::isRequest(request));QVERIFY2(NetworkTradingTransport::allowed(request),qPrintable(request.url.toString()));}
    QCOMPARE(routes,6); // Three catalogue and three trade routes, plus OI above.
}
void CoinFlowProtocolTest::nativeTransportAcceptsVerifiedCombiningMarkSymbol(){
    const auto symbol=QString::fromUtf8("E\u0301USDT");const auto request=CoinFlowProtocol::request(CoinFlowRead::Trades,CoinFlowVenue::BinanceSpot,symbol);QVERIFY(CoinFlowProtocol::isRequest(request));QVERIFY2(NetworkTradingTransport::allowed(request),qPrintable(request.url.toString()));
}
void CoinFlowProtocolTest::socketAllowlistKeepsSpotAndRoutedFuturesSeparate(){
    const auto spot=CoinFlowProtocol::streamEndpoint(instrument());QCOMPARE(spot,QUrl("wss://stream.binance.com:9443/ws/synusdt@aggTrade"));QVERIFY(isPublicFlowSocketEndpoint(spot));
    const auto future=CoinFlowProtocol::streamEndpoint(instrument(CoinFlowVenue::BinanceUsdM));QCOMPARE(future.path(),QString("/market/stream"));QCOMPARE(QUrlQuery(future).queryItemValue("streams"),QString("synusdt@aggTrade/synusdt@forceOrder"));QVERIFY(isPublicFlowSocketEndpoint(future));
    QVERIFY(isPublicFlowSocketEndpoint(QUrl("wss://api.upbit.com/websocket/v1")));
    for(const auto& url:{"wss://fstream.binance.com/ws/synusdt@aggTrade","wss://fstream.binance.com/private/ws/synusdt@aggTrade","wss://fstream.binance.com/market/ws/!forceOrder@arr","wss://stream.binance.com:9443/ws/synusdt@miniTicker","wss://user@api.upbit.com/websocket/v1","wss://api.upbit.com/websocket/v1?token=synthetic"})QVERIFY(!isPublicFlowSocketEndpoint(QUrl(url)));
}
void CoinFlowProtocolTest::catalogPreservesOfficialPairAndInternalFuturesIdentity(){
    auto parsed=CoinFlowProtocol::catalog(CoinFlowVenue::BinanceUsdM,response(futureCatalog()),Now);QVERIFY(parsed.available);QCOMPARE(parsed.instruments.size(),1);const auto item=parsed.instruments.first();
    QVERIFY(item.verified);QCOMPARE(item.identity,QString("usdm_futures:binance:SYNUSDT"));QCOMPARE(item.baseAsset,QString("SYN"));QCOMPARE(item.marginAsset,QString("USDT"));QCOMPARE(item.contractType,QString("PERPETUAL"));QCOMPARE(item.evidence.sourceTimeMs,qint64(0));QCOMPARE(item.evidence.receivedAtMs,Now);
    auto bad=futureCatalog();auto rows=bad["symbols"].toArray();rows.append(rows.first());bad["symbols"]=rows;parsed=CoinFlowProtocol::catalog(CoinFlowVenue::BinanceUsdM,response(bad),Now);QVERIFY(!parsed.available);QVERIFY(parsed.instruments.isEmpty());
}
void CoinFlowProtocolTest::aggregateIdsAndMakerDirectionRemainExact(){
    auto first=trade();auto second=trade("9007199254740994",true);auto wire=bytes(first);wire.replace("\"a\":\"9007199254740993\"","\"a\":9007199254740993");auto parsed=CoinFlowProtocol::tradeFrame(instrument(),wire,Now,7);QVERIFY(parsed.available);QCOMPARE(parsed.trades.size(),1);QCOMPARE(parsed.trades.first().id,QString("9007199254740993"));QCOMPARE(parsed.trades.first().side,CoinFlowSide::Buy);QCOMPARE(parsed.trades.first().tradeAtMs,Now-2);QCOMPARE(parsed.trades.first().eventAtMs,Now-1);QCOMPARE(parsed.trades.first().receivedAtMs,Now);QCOMPARE(parsed.trades.first().epoch,quint64(7));
    parsed=CoinFlowProtocol::tradeFrame(instrument(),bytes(second),Now,7);QVERIFY(parsed.available);QCOMPARE(parsed.trades.first().id,QString("9007199254740994"));QCOMPARE(parsed.trades.first().side,CoinFlowSide::Sell);
    first.remove("E");first.remove("e");first.remove("s");QJsonArray rest;rest.append(first);parsed=CoinFlowProtocol::trades(instrument(),response(rest),Now,7);QVERIFY(parsed.available);QCOMPARE(parsed.trades.first().eventAtMs,qint64(0));QVERIFY(!parsed.trades.first().liveEligible);QCOMPARE(parsed.trades.first().knownAtMs,Now);
}
void CoinFlowProtocolTest::decimalSourceBoundsRemainExact_data(){
    QTest::addColumn<QString>("value");QTest::addColumn<bool>("accepted");const auto maximum="1"+QString(100,'0');
    QTest::newRow("tiny-positive")<<("0."+QString(199,'0')+"1")<<true;
    QTest::newRow("exact-upper-bound")<<maximum<<true;
    QTest::newRow("upper-with-zero-fraction")<<(maximum+".000")<<true;
    QTest::newRow("sub-ulp-over-upper")<<(maximum+"."+QString(199,'0')+"1")<<false;
    QTest::newRow("integer-over-upper")<<(maximum+"0")<<false;
    QTest::newRow("512-char-positive")<<("0."+QString(509,'0')+"1")<<true;
    QTest::newRow("513-char-rejected")<<("0."+QString(510,'0')+"1")<<false;
    QTest::newRow("leading-zero")<<QString("01")<<false;
    QTest::newRow("zero-price")<<QString("0.000")<<false;
    QTest::newRow("trailing-linefeed")<<QString("1\n")<<false;
}
void CoinFlowProtocolTest::decimalSourceBoundsRemainExact(){
    QFETCH(QString,value);QFETCH(bool,accepted);auto row=trade();row["p"]=value;row["q"]=value;const auto parsed=CoinFlowProtocol::tradeFrame(instrument(),bytes(row),Now,7);QCOMPARE(parsed.available,accepted);
    if(accepted){QCOMPARE(parsed.trades.size(),1);QCOMPARE(parsed.trades.first().price,value);QCOMPARE(parsed.trades.first().quantity,value);}else QVERIFY(parsed.trades.isEmpty());
}
void CoinFlowProtocolTest::normalQuantityUsesExactSubUlpComparison(){
    const auto unit="1."+QString(199,'0');auto row=trade();row["q"]=unit+"1";row["nq"]=unit+"2";QVERIFY(!CoinFlowProtocol::tradeFrame(instrument(CoinFlowVenue::BinanceUsdM),bytes(row),Now,7).available);
    row["nq"]=unit+"1";const auto parsed=CoinFlowProtocol::tradeFrame(instrument(CoinFlowVenue::BinanceUsdM),bytes(row),Now,7);QVERIFY(parsed.available);QCOMPARE(parsed.trades.first().normalQuantity,std::optional<QString>(unit+"1"));
}
void CoinFlowProtocolTest::malformedTradeFrameRejectsAtomically_data(){QTest::addColumn<QString>("fault");for(const auto* fault:{"newline-price","negative-quantity","maker-string","wrong-symbol","future-time","metadata-unverified"})QTest::newRow(fault)<<QString(fault);}
void CoinFlowProtocolTest::malformedTradeFrameRejectsAtomically(){
    QFETCH(QString,fault);auto row=trade();auto item=instrument();if(fault=="newline-price")row["p"]="100\n";if(fault=="negative-quantity")row["q"]="-1";if(fault=="maker-string")row["m"]="false";if(fault=="wrong-symbol")row["s"]="OTHERUSDT";if(fault=="future-time")row["T"]=QString::number(Now+1);if(fault=="metadata-unverified")item.verified=false;
    const auto parsed=CoinFlowProtocol::tradeFrame(item,bytes(row),Now,7);QVERIFY(!parsed.available);QVERIFY(parsed.trades.isEmpty());
}
void CoinFlowProtocolTest::futuresQuantityIncludesRpiAndNormalQuantityStaysSeparate(){
    auto raw=trade();raw["nq"]="1";raw["st"]="1";auto frame=QJsonObject{{"stream","synusdt@aggTrade"},{"data",raw}};auto parsed=CoinFlowProtocol::tradeFrame(instrument(CoinFlowVenue::BinanceUsdM),bytes(frame),Now,3);QVERIFY(parsed.available);QCOMPARE(parsed.trades.first().quantity,QString("3"));QCOMPARE(parsed.trades.first().normalQuantity,std::optional<QString>("1"));
    raw["st"]="2";frame["data"]=raw;parsed=CoinFlowProtocol::tradeFrame(instrument(CoinFlowVenue::BinanceUsdM),bytes(frame),Now,3);QVERIFY(!parsed.available);QVERIFY(parsed.trades.isEmpty());
}
void CoinFlowProtocolTest::upbitSnapshotUsesSourceDirectionAndActualClocks(){
    QJsonObject row{{"type","trade"},{"code","KRW-SYN"},{"trade_price","100"},{"trade_volume","2"},{"ask_bid","ASK"},{"trade_timestamp",QString::number(Now-2)},{"timestamp",QString::number(Now-1)},{"sequential_id","9007199254740993"},{"stream_type","SNAPSHOT"}};
    const auto parsed=CoinFlowProtocol::tradeFrame(instrument(CoinFlowVenue::UpbitSpot),bytes(row),Now,4);QVERIFY(parsed.available);QCOMPARE(parsed.trades.first().side,CoinFlowSide::Sell);QVERIFY(parsed.trades.first().snapshot);QVERIFY(!parsed.trades.first().liveEligible);QCOMPARE(parsed.trades.first().tradeAtMs,Now-2);QCOMPARE(parsed.trades.first().eventAtMs,Now-1);
}
void CoinFlowProtocolTest::openInterestHistoryKeepsEndTimesAndMissingPeriods(){
    const auto end=Now-Now%300000;QJsonArray rows;for(const auto offset:{-600000LL,0LL})rows.append(QJsonObject{{"symbol","SYNUSDT"},{"sumOpenInterest",offset?"100":"102"},{"sumOpenInterestValue","10000"},{"timestamp",QString::number(end+offset)}});
    const auto parsed=CoinFlowProtocol::openInterest(instrument(CoinFlowVenue::BinanceUsdM),response(rows),Now);QVERIFY(parsed.available);QCOMPARE(parsed.points.size(),2);QCOMPARE(parsed.points.first().periodEndMs,end-600000);QCOMPARE(parsed.points.last().periodEndMs,end);QCOMPARE(parsed.points.first().knownAtMs,Now);QVERIFY(!parsed.points.first().liveEligible);QCOMPARE(parsed.points.first().evidence.sourceTimeMs,qint64(0));
}
void CoinFlowProtocolTest::liquidationKeepsLossySnapshotFieldsWithoutInventedOrderId(){
    QJsonObject order{{"s","SYNUSDT"},{"S","SELL"},{"q","5"},{"p","99"},{"ap","100"},{"X","PARTIALLY_FILLED"},{"l","1"},{"z","2"},{"T",QString::number(Now-2)}};
    const auto raw=QJsonObject{{"e","forceOrder"},{"E",QString::number(Now-1)},{"o",order}};const auto frame=QJsonObject{{"stream","synusdt@forceOrder"},{"data",raw}};
    const auto parsed=CoinFlowProtocol::liquidationFrame(instrument(CoinFlowVenue::BinanceUsdM),bytes(frame),Now,9);QVERIFY(parsed.available);QCOMPARE(parsed.observations.size(),1);const auto value=parsed.observations.first();QCOMPARE(value.side,QString("SELL"));QCOMPARE(value.originalQuantity,QString("5"));QCOMPARE(value.lastFilledQuantity,QString("1"));QCOMPARE(value.cumulativeFilledQuantity,QString("2"));QCOMPARE(value.tradeAtMs,Now-2);QCOMPARE(value.eventAtMs,Now-1);QCOMPARE(value.epoch,quint64(9));
}
void CoinFlowProtocolTest::liquidationMarketSnapshotKeepsZeroPrices(){
    const QJsonObject order{{"s","SYNUSDT"},{"S","SELL"},{"q","5"},{"p","0"},{"ap","0"},{"X","NEW"},{"l","0"},{"z","0"},{"T",QString::number(Now-2)}};
    const auto frame=QJsonObject{{"stream","synusdt@forceOrder"},{"data",QJsonObject{{"e","forceOrder"},{"E",QString::number(Now-1)},{"o",order}}}};
    const auto parsed=CoinFlowProtocol::liquidationFrame(instrument(CoinFlowVenue::BinanceUsdM),bytes(frame),Now,9);QVERIFY(parsed.available);QCOMPARE(parsed.observations.size(),1);QCOMPARE(parsed.observations.first().price,QString("0"));QCOMPARE(parsed.observations.first().averagePrice,QString("0"));QCOMPARE(parsed.observations.first().cumulativeFilledQuantity,QString("0"));
}
void CoinFlowProtocolTest::smallClockLeadWaitsWithoutChangingTheFrame(){
    auto row=trade();row["E"]=QString::number(Now+114);row["T"]=QString::number(Now+100);const auto raw=bytes(row);
    QVERIFY(!CoinFlowProtocol::tradeFrame(instrument(),raw,Now,7).available);
    PublicFlowFrameQueue queue;QVERIFY(queue.push(raw,Now,0));QVERIFY(queue.takeReady(Now,0).isEmpty());
    QCOMPARE(queue.waitMs(Now,0),114);QVERIFY(queue.takeReady(Now+113,113).isEmpty());
    const auto ready=queue.takeReady(Now+114,114);QCOMPARE(ready,QList<QByteArray>{raw});
    const auto parsed=CoinFlowProtocol::tradeFrame(instrument(),ready.first(),Now+114,7);QVERIFY(parsed.available);
    QCOMPARE(parsed.trades.first().eventAtMs,Now+114);QCOMPARE(parsed.trades.first().tradeAtMs,Now+100);
    QCOMPARE(queue.waitMs(Now+114,114),-1);
    const auto combined=bytes(QJsonObject{{"data",QJsonObject{{"E",double(Now+20)},{"o",QJsonObject{{"T",double(Now+30)}}}}}});
    QVERIFY(queue.push(combined,Now,0));QCOMPARE(queue.waitMs(Now,0),30);QCOMPARE(queue.takeReady(Now+30,30),QList<QByteArray>{combined});
    const auto upbit=bytes(QJsonObject{{"timestamp",QString::number(Now+10)},{"trade_timestamp",QString::number(Now+20)}});
    QVERIFY(queue.push(upbit,Now,0));QCOMPARE(queue.waitMs(Now,0),20);
}
void CoinFlowProtocolTest::bufferedFramesKeepWireOrderAndExpireOnClockRollback(){
    auto row=trade();row["E"]=QString::number(Now+200);const auto raw=bytes(row);const auto next=bytes(trade());
    PublicFlowFrameQueue queue;QVERIFY(queue.push(raw,Now,0));QVERIFY(queue.push(next,Now+10,10));
    QVERIFY(queue.takeReady(Now+100,100).isEmpty());QCOMPARE(queue.takeReady(Now+200,200),(QList<QByteArray>{raw,next}));
    QVERIFY(queue.push(raw,Now,0));QVERIFY(queue.takeReady(Now-10000,1999).isEmpty());
    QCOMPARE(queue.takeReady(Now-10000,2000),QList<QByteArray>{raw});
    QVERIFY(!CoinFlowProtocol::tradeFrame(instrument(),raw,Now-10000,7).available);
}
void CoinFlowProtocolTest::clockBufferIsBoundedAndResetDropsOldFrames(){
    PublicFlowFrameQueue queue;const auto raw=bytes(QJsonObject{{"E",QString::number(Now+2000)}});
    for(int i=0;i<128;++i)QVERIFY(queue.push(raw,Now,0));QVERIFY(!queue.push(raw,Now,0));
    queue.clear();QVERIFY(queue.takeReady(Now+2000,2000).isEmpty());QCOMPARE(queue.waitMs(Now,0),-1);
    QVERIFY(!queue.push(QByteArray(65537,'x'),Now,0));
    const auto large=raw+QByteArray(65536-raw.size(),' ');
    for(int i=0;i<32;++i)QVERIFY(queue.push(large,Now,0));QVERIFY(!queue.push(large,Now,0));
}
void CoinFlowProtocolTest::farFutureFramesRemainRejected(){
    PublicFlowFrameQueue queue;auto row=trade();row["E"]=QString::number(Now+2001);const auto raw=bytes(row);
    QVERIFY(queue.push(raw,Now,0));QCOMPARE(queue.takeReady(Now,0),QList<QByteArray>{raw});
    QVERIFY(!CoinFlowProtocol::tradeFrame(instrument(),raw,Now,7).available);
    const QByteArray malformed="{\"E\":1e30}";QVERIFY(queue.push(malformed,Now,0));QCOMPARE(queue.takeReady(Now,0),QList<QByteArray>{malformed});
}
QTEST_GUILESS_MAIN(CoinFlowProtocolTest)
#include "tst_coin_flow_protocol.moc"
