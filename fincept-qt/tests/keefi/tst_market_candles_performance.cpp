#include "keefi/MarketCandles.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Minute=60000, Start=1792598400000;
class Http final:public TradingTransport {
  public:
    QList<quint64> tokens;QList<RequestSpec> requests;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{}
    void answer(const QJsonArray& rows){HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();emit finished(tokens.last(),result);}
};
class Socket final:public PublicCandleSocket {
  public:
    QList<quint64> epochs;QList<QByteArray> sent;
    void open(quint64 epoch,const QUrl&)override{epochs.append(epoch);}
    void send(quint64,const QByteArray& body)override{sent.append(body);}
    void abort()override{}
    void ready(){emit connected(epochs.last());}
    void packet(const QJsonObject& body){emit received(epochs.last(),QJsonDocument(body).toJson(QJsonDocument::Compact));}
};
QJsonObject upbit(qint64 at,const QString& symbol="KRW-SYN"){
    return {{"market",symbol},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(at+1000)},{"opening_price","100"},{"high_price","110"},{"low_price","90"},
        {"trade_price","104"},{"candle_acc_trade_volume","7"},{"unit","1"}};
}
QJsonArray page(int index,const QString& symbol="KRW-SYN"){
    QJsonArray rows;for(int i=200;i>0;--i)rows.append(upbit(Start-(index*200+i)*Minute,symbol));return rows;
}
MarketCandle archiveRow(qint64 at,const QString& close="104",const QString& volume="7"){
    return {at,at+1000,"100","110","90",close,volume};
}
MarketCandleSeries archiveOffer(qint64 clock,const QList<MarketCandle>& rows){
    MarketCandleSeries result;result.available=true;result.identity="coin:upbit:KRW-SYN";
    result.asset="SYN";result.currency="KRW";result.intervalMs=Minute;result.receivedAtMs=clock;
    result.sourceUrl=MarketCandles::request(result.identity,Minute).url.toString();result.candles=rows;return result;
}
QJsonArray binance(qint64 at){return {QString::number(at),"100","110","90","104","7",QString::number(at+Minute-1),"700","5","1","100","0"};}
MarketInstrument instrument(){MarketInstrument item;item.symbol="SYNUSDT";item.baseAsset="SYN";item.quoteAsset="USDT";
    item.market="spot";item.status="TRADING";item.tradable=true;return item;}
QJsonObject kline(qint64 event,qint64 lastId,QString close="104",QString volume="7"){
    return {{"e","kline"},{"E",QString::number(event)},{"s","SYNUSDT"},{"k",QJsonObject{
        {"t",QString::number(Start)},{"T",QString::number(Start+Minute-1)},{"s","SYNUSDT"},{"i","1m"},
        {"o","100"},{"h","120"},{"l","90"},{"c",close},{"v",volume},{"n","5"},{"x",false},
        {"q","700"},{"V","1"},{"Q","100"},{"B","0"},{"L",QString::number(lastId)}}}};
}
QJsonObject trade(qint64 at,qint64 id,QString price="111",QString quantity="0.1"){
    return {{"e","trade"},{"s","SYNUSDT"},{"t",QString::number(id)},{"T",QString::number(at)},
        {"E",QString::number(at)},{"p",price},{"q",quantity}};
}
QJsonObject upbitTrade(qint64 at,qint64 id=17877285547970000LL){return {{"type","trade"},{"code","KRW-SYN"},{"stream_type","REALTIME"},
    {"sequential_id",QString::number(id)},{"trade_timestamp",QString::number(at)},{"timestamp",QString::number(at)},
    {"trade_price","111"},{"trade_volume","0.1"}};}
void seedBinance(MarketCandles& model,Http& http,Socket& wire,qint64 clock){
    model.setInstruments("binance",{instrument()});model.setSelection("coin:binance:SYNUSDT",Minute);wire.ready();
    QJsonArray rows;rows.append(binance(Start-Minute));rows.append(binance(Start));http.answer(rows);wire.packet(kline(clock,100));
}
}
class MarketCandlesPerformanceTest:public QObject {
    Q_OBJECT
  private slots:
    void archiveRowsKeepLastWinsAcrossNormalizationAndReplacement(){
        Http http;qint64 clock=Start+10000;MarketCandles model(&http,{},[&]{return clock;});
        model.setSelection("coin:upbit:KRW-SYN",Minute);
        HttpResult unavailable;unavailable.status=503;emit http.finished(http.tokens.last(),unavailable);
        const QList<MarketCandle> first{archiveRow(Start-2*Minute,"101","1"),archiveRow(Start-3*Minute,"102","2"),
            archiveRow(Start-2*Minute,"103","3"),archiveRow(Start-Minute,"104","4")};
        QMap<qint64,MarketCandle> history;
        const auto append=[](auto& destination,const QList<MarketCandle>& rows){for(const auto& row:rows)destination.insert(row.startsAtMs,row);};
        const auto verify=[&](const QList<MarketCandle>& current){
            auto expected=history;append(expected,current);const auto actual=model.displayObservation().candles;
            QCOMPARE(actual.size(),expected.size());qsizetype i=0;
            for(const auto& row:expected){
                QCOMPARE(actual[i].startsAtMs,row.startsAtMs);QCOMPARE(actual[i].lastStoredTickMs,row.lastStoredTickMs);
                QCOMPARE(actual[i].open,row.open);QCOMPARE(actual[i].high,row.high);QCOMPARE(actual[i].low,row.low);
                QCOMPARE(actual[i].close,row.close);QCOMPARE(actual[i].volume,row.volume);++i;
            }
        };
        QCOMPARE(model.setStoredCompleted(archiveOffer(clock,first)),StoredCandlesOffer::Accepted);
        clock+=1100;model.requestOlder();append(history,first);verify(first);
        QCOMPARE(model.historyStatus().completedCount,3);
        QCOMPARE(http.requests.last().url,MarketCandles::historyRequest("coin:upbit:KRW-SYN",Minute,Start-3*Minute).url);
        http.answer({upbit(Start-4*Minute),upbit(Start-5*Minute)});
        append(history,{archiveRow(Start-4*Minute),archiveRow(Start-5*Minute)});verify(first);
        const auto before=model.historyStatus();QCOMPARE(before.completedCount,5);
        model.clearStoredCompleted();QVERIFY(model.displayObservation().candles.isEmpty());
        const QList<MarketCandle> replacement{archiveRow(Start-4*Minute,"105","5"),archiveRow(Start-6*Minute,"106","6"),
            archiveRow(Start-4*Minute,"108","8"),archiveRow(Start-2*Minute,"109","9"),archiveRow(Start-Minute,"104","4")};
        QCOMPARE(model.setStoredCompleted(archiveOffer(clock,replacement)),StoredCandlesOffer::Accepted);
        verify(replacement);verify(replacement); // repeated cached reads retain identical precedence
        QCOMPARE(model.historyStatus().completedCount,before.completedCount);
        QCOMPARE(model.historyStatus().oldestAtMs,before.oldestAtMs);QCOMPARE(model.historyStatus().state,before.state);
        clock+=1100;model.requestOlder();
        QCOMPARE(http.requests.last().url,MarketCandles::historyRequest("coin:upbit:KRW-SYN",Minute,Start-5*Minute).url);
        auto older=upbit(Start-6*Minute);older["trade_price"]="101";http.answer({older});
        append(history,{archiveRow(Start-6*Minute,"101")});verify(replacement);
        QCOMPARE(model.displayObservation().candles.first().close,QString("106")); // current archive still wins over history
    }
    void archiveMergeCacheIsPurgedAtIdentityAndMetadataBoundaries(){
        Http http;qint64 clock=Start+10000;MarketCandles model(&http,{},[&]{return clock;});
        model.setVerifiedUpbitMarkets({"KRW-SYN","KRW-OTHER"},true);model.setSelection("coin:upbit:KRW-SYN",Minute);
        const auto fill=[&]{
            QCOMPARE(model.setStoredCompleted(archiveOffer(clock,{archiveRow(Start-Minute)})),StoredCandlesOffer::Accepted);
            model.requestOlder();QCOMPARE(model.displayObservation().candles.size(),1);
            QVERIFY(model.displayObservation().fromArchive);QVERIFY(!model.series().available);
        };
        fill();model.clearDisplayCache();QVERIFY(model.displayObservation().candles.isEmpty());
        fill();model.setSelection("coin:upbit:KRW-OTHER",Minute);QVERIFY(model.displayObservation().candles.isEmpty());
        model.setSelection("coin:upbit:KRW-SYN",Minute);QVERIFY(model.displayObservation().candles.isEmpty());
        fill();model.setVerifiedUpbitMarkets({"KRW-OTHER"},true);QVERIFY(model.displayObservation().candles.isEmpty());
        model.setVerifiedUpbitMarkets({"KRW-SYN","KRW-OTHER"},true);QVERIFY(model.displayObservation().candles.isEmpty());
        QCOMPARE(model.historyStatus().completedCount,0);
    }
    void revisitingPreservesPagedHistoryAndContinuesAtItsOldestBoundary(){
        Http http;qint64 clock=Start+10000;MarketCandles model(&http,{},[&]{return clock;});
        model.setSelection("coin:upbit:KRW-SYN",Minute);http.answer(page(0));
        clock+=1100;model.requestOlder();http.answer(page(1));
        const auto original=model.displayObservation();QCOMPARE(original.candles.size(),400);
        model.setSelection("coin:upbit:KRW-OTHER",Minute);model.setSelection("coin:upbit:KRW-SYN",Minute);
        const auto cached=model.displayObservation();QCOMPARE(cached.candles.size(),400);
        QCOMPARE(cached.candles.first().startsAtMs,original.candles.first().startsAtMs);
        QVERIFY(cached.fromMemoryCache);QVERIFY(cached.delayed);QVERIFY(!cached.available);
        QVERIFY(model.series().candles.isEmpty());QVERIFY(model.restObservation().candles.isEmpty());
        http.answer(page(0));QCOMPARE(model.series().candles.size(),200);
        clock+=1100;model.requestOlder();
        QCOMPARE(http.requests.last().url,MarketCandles::historyRequest("coin:upbit:KRW-SYN",Minute,Start-400*Minute).url);
        http.answer(page(2));QCOMPARE(model.displayObservation().candles.size(),600);
        QCOMPARE(original.candles.size(),400); // returned snapshots cannot be mutated by later history merges
    }
    void lockAndMetadataRemovalDiscardInactivePagedHistory(){
        Http http;qint64 clock=Start+10000;MarketCandles model(&http,{},[&]{return clock;});
        model.setVerifiedUpbitMarkets({"KRW-SYN","KRW-OTHER"},true);model.setSelection("coin:upbit:KRW-SYN",Minute);http.answer(page(0));
        clock+=1100;model.requestOlder();http.answer(page(1));
        model.setSelection("coin:upbit:KRW-OTHER",Minute);model.clearDisplayCache();model.setSelection("coin:upbit:KRW-SYN",Minute);
        QVERIFY(model.displayObservation().candles.isEmpty());QCOMPARE(model.historyStatus().completedCount,0);
        http.answer(page(0));clock+=1100;model.requestOlder();http.answer(page(1));model.setSelection("coin:upbit:KRW-OTHER",Minute);
        model.setVerifiedUpbitMarkets({"KRW-OTHER"},true);model.setVerifiedUpbitMarkets({"KRW-SYN","KRW-OTHER"},true);
        model.setSelection("coin:upbit:KRW-SYN",Minute);QVERIFY(model.displayObservation().candles.isEmpty());
    }
    void historyCacheBudgetKeepsTheMostRecentlyUsedRanges(){
        Http http;qint64 clock=Start+10000;MarketCandles model(&http,{},[&]{return clock;});
        for(const auto& symbol:QStringList{"KRW-A","KRW-B","KRW-C"}){
            model.setSelection("coin:upbit:"+symbol,Minute);http.answer(page(0,symbol));
            for(int i=1;i<13;++i){clock+=1100;model.requestOlder();http.answer(page(i,symbol));}
            QCOMPARE(model.displayObservation().candles.size(),2600);
        }
        model.setSelection("coin:upbit:KRW-A",Minute);
        QCOMPARE(model.displayObservation().candles.size(),200); // recent evidence remains in the eight-entry LRU
        model.setSelection("coin:upbit:KRW-B",Minute);const auto b=model.historyStatus().completedCount;
        model.setSelection("coin:upbit:KRW-C",Minute);const auto c=model.historyStatus().completedCount;
        QCOMPARE(b+c,MarketCandles::HistoryLimit);QCOMPARE(c,2600);
    }
    void tradeBurstOnlyChangesDisplayAndEmitsOneLightweightUpdate(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});seedBinance(model,http,*wire,clock);
        const auto official=model.series(),rest=model.restObservation();const auto requests=http.requests.size();
        QSignalSpy general(&model,&MarketCandles::changed),live(&model,&MarketCandles::liveChanged);
        for(int i=1;i<=20;++i){++clock;wire->packet(trade(clock,100+i));}
        QCOMPARE(model.series().forming->close,official.forming->close);QCOMPARE(model.series().forming->volume,official.forming->volume);
        QCOMPARE(model.series().receivedAtMs,official.receivedAtMs);QCOMPARE(model.series().sourceTimeMs,official.sourceTimeMs);
        QCOMPARE(model.restObservation().restRevision,rest.restRevision);QCOMPARE(model.restObservation().candles.size(),rest.candles.size());
        QCOMPARE(model.displayObservation().forming->close,QString("111"));QCOMPARE(model.displayObservation().forming->volume,QString("9"));
        QVERIFY(model.liveTradeObservation().available);QCOMPARE(model.liveTradeObservation().price,QString("111"));
        QCOMPARE(http.requests.size(),requests);QCOMPARE(general.size(),0);QCOMPARE(live.size(),0);
        QTRY_COMPARE_WITH_TIMEOUT(live.size(),1,1000);QTest::qWait(40);QCOMPARE(live.size(),1);
    }
    void lateOfficialCandleReplaysOnlyItsUncoveredTradeTail(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});seedBinance(model,http,*wire,clock);
        wire->packet(trade(++clock,101,"111","0.1"));wire->packet(trade(++clock,102,"112","0.2"));
        QCOMPARE(model.displayObservation().forming->volume,QString("7.3"));
        wire->packet(kline(++clock,101,"111","7.1"));
        QCOMPARE(model.series().forming->close,QString("111"));QCOMPARE(model.series().forming->volume,QString("7.1"));
        QCOMPARE(model.displayObservation().forming->close,QString("112"));QCOMPARE(model.displayObservation().forming->volume,QString("7.3"));
    }
    void missingTradeFallsBackWithoutLosingAnExactLatestPrice(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});seedBinance(model,http,*wire,clock);
        wire->packet(trade(++clock,102));QCOMPARE(model.displayObservation().forming->close,QString("104"));
        QCOMPARE(model.liveTradeObservation().price,QString("111"));
        wire->packet(kline(++clock,101,"105","7.1"));
        QCOMPARE(model.displayObservation().forming->close,QString("111"));QCOMPARE(model.displayObservation().forming->volume,QString("7.2"));
    }
    void inconsistentDuplicateSendsALightweightFallbackNotification(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});seedBinance(model,http,*wire,clock);
        QSignalSpy live(&model,&MarketCandles::liveChanged);
        wire->packet(trade(++clock,101,"111","0.1"));QTRY_COMPARE_WITH_TIMEOUT(live.size(),1,1000);
        QCOMPARE(model.displayObservation().forming->close,QString("111"));
        wire->packet(trade(clock,101,"111","0.2"));
        QCOMPARE(model.displayObservation().forming->close,QString("104"));
        QTRY_COMPARE_WITH_TIMEOUT(live.size(),2,1000);
    }
    void tradeCannotBypassTheRestSeedOrRevokedGeneration(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setInstruments("binance",{instrument()});
        model.setSelection("coin:binance:SYNUSDT",Minute);wire->ready();const auto old=wire->epochs.last();
        wire->packet(kline(clock,100));wire->packet(trade(++clock,101));
        QVERIFY(!model.liveTradeObservation().available);QVERIFY(!model.displayObservation().forming);
        QJsonArray rows;rows.append(binance(Start-Minute));rows.append(binance(Start));http.answer(rows);
        QVERIFY(model.liveTradeObservation().available);QCOMPARE(model.displayObservation().forming->close,QString("111"));
        model.clearDisplayCache();QVERIFY(!model.liveTradeObservation().available);QVERIFY(wire->epochs.last()!=old);
        emit wire->received(old,QJsonDocument(trade(++clock,102)).toJson());QVERIFY(!model.liveTradeObservation().available);
        model.setSelection({},Minute);QVERIFY(!model.liveTradeObservation().available);
    }
    void tradeCannotKeepExpiredCompletedEvidenceFresh(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});seedBinance(model,http,*wire,clock);
        const auto receipt=model.series().receivedAtMs;
        clock+=MarketCandles::FreshnessMs-1;wire->packet(trade(clock,101));QVERIFY(model.liveTradeObservation().available);
        QCOMPARE(model.series().receivedAtMs,receipt);clock+=2;
        QVERIFY(!model.series().available);QVERIFY(!model.liveTradeObservation().available);QVERIFY(!model.displayObservation().forming);
    }
    void upbitTradePreservesOfficialOhlcvAndDoesNotFetchRest(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setSelection("coin:upbit:KRW-SYN",Minute);wire->ready();
        http.answer({upbit(Start-Minute),upbit(Start)});const auto official=model.series();const auto requests=http.requests.size();
        QSignalSpy changes(&model,&MarketCandles::changed);wire->packet(upbitTrade(++clock));
        QVERIFY(model.liveTradeObservation().available);QCOMPARE(model.liveTradeObservation().price,QString("111"));
        QCOMPARE(model.series().forming->close,official.forming->close);QCOMPARE(model.series().forming->volume,official.forming->volume);
        QCOMPARE(model.displayObservation().forming->close,official.forming->close);QCOMPARE(model.displayObservation().forming->volume,official.forming->volume);
        QCOMPARE(model.series().receivedAtMs,official.receivedAtMs);QCOMPARE(http.requests.size(),requests);QCOMPARE(changes.size(),0);
    }
    void rawSeventeenDigitUpbitIdsRemainDistinctThroughTheExactParser(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});model.setSelection("coin:upbit:KRW-SYN",Minute);wire->ready();
        http.answer({upbit(Start-Minute),upbit(Start)});++clock;
        const auto raw=[&](const QString& id,const QString& price){
            // Keep the 17-digit identifier as a JSON numeric token all the way
            // into parseExactJson; constructing a QJson double here would lose it.
            const auto body=QStringLiteral(R"({"type":"trade","code":"KRW-SYN","stream_type":"REALTIME","sequential_id":%1,"trade_timestamp":%2,"timestamp":%2,"trade_price":%3,"trade_volume":0.1})")
                .arg(id,QString::number(clock),price).toUtf8();
            emit wire->received(wire->epochs.last(),body);
        };
        raw("17877285547970000","111");QVERIFY(model.liveTradeObservation().available);
        QCOMPARE(model.liveTradeObservation().price,QString("111"));
        raw("17877285547970001","112");QCOMPARE(model.liveTradeObservation().price,QString("112"));
        raw("17877285547970000","113");QCOMPARE(model.liveTradeObservation().price,QString("112"));
        QCOMPARE(model.series().forming->close,QString("104"));
    }
    void futureAndWrongIdentityTradesAreIgnored(){
        Http http;auto socket=std::make_unique<Socket>();auto* wire=socket.get();qint64 clock=Start+10000;
        MarketCandles model(&http,std::move(socket),[&]{return clock;});seedBinance(model,http,*wire,clock);
        wire->packet(trade(clock+1,101));QVERIFY(!model.liveTradeObservation().available);
        auto other=trade(clock,101);other["s"]="OTHERUSDT";wire->packet(other);QVERIFY(!model.liveTradeObservation().available);
        wire->packet(trade(++clock,101));QVERIFY(model.liveTradeObservation().available);
        model.setInstruments("binance",{});QVERIFY(!model.liveTradeObservation().available);
    }
};
QTEST_GUILESS_MAIN(MarketCandlesPerformanceTest)
#include "tst_market_candles_performance.moc"
