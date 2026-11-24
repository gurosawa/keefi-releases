#include "keefi/PublicQuoteStream.h"
#include "keefi/PublicKrwPrices.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
using namespace keefi;
namespace {
class FakeSocket final:public PublicTickerSocket {
public:
    QList<QUrl> urls;QList<QByteArray> sent;int aborted=0,pings=0;
    void open(const QUrl& u)override{urls.append(u);}
    void send(const QByteArray& b)override{sent.append(b);}
    void ping()override{++pings;}
    void abort()override{++aborted;}
};
QByteArray tick(QString code,QString price,qint64 at,bool binance=false){
    return QJsonDocument(binance?QJsonObject{{"e","24hrMiniTicker"},{"s",code},{"c",price},{"E",QString::number(at)}}:
        QJsonObject{{"type","ticker"},{"code",code},{"trade_price",price},{"timestamp",QString::number(at)},
            {"prev_closing_price","100"},{"signed_change_rate","-0.01"}}).toJson(QJsonDocument::Compact);
}
class CatalogueTransport final:public TradingTransport {
public:
    QList<RequestSpec> sent;bool large=false;qint64 usdcTime=0;
    void abort()override{}
    void send(quint64 id,const RequestSpec& request)override{
        QVERIFY(NetworkTradingTransport::allowed(request));QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());sent.append(request);
        HttpResult r;r.status=200;
        r.body=R"({"symbols":[{"symbol":"BNBUSDT","baseAsset":"BNB","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},{"symbol":"USDCUSDT","baseAsset":"USDC","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},{"symbol":"BADUSDT","baseAsset":"BAD","quoteAsset":"USDT","status":"BREAK","isSpotTradingAllowed":true}]})";
        if(request.url.host()=="api.upbit.com"){
            QJsonArray rows;
            for(const auto& market:QUrlQuery(request.url).queryItemValue("markets").split(','))
                rows.append(QJsonObject{{"market",market},{"trade_price","1400"},{"timestamp",QString::number(market=="KRW-USDC"?usdcTime:QDateTime::currentMSecsSinceEpoch())}});
            r.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
        }
        if(large&&request.url.host()=="api.binance.com"){r.body.chop(1);r.body+=",\"cataloguePadding\":\""+QByteArray(4*1024*1024,'x')+"\"}";}
        QTimer::singleShot(0,this,[this,id,r]{emit finished(id,r);});
    }
};
AssetQuote quote(QString asset,QString currency,QString price,qint64 at){return {asset,currency,price,
    {currency=="USDT"?"Binance 공식 USDT 현재가":"업비트 공식 KRW 현재가",currency=="USDT"?"wss://stream.binance.com:9443/ws":"wss://api.upbit.com/websocket/v1",
        currency=="USDT"?"public:binance:"+asset+"USDT":"public:upbit:KRW-"+asset,at,at,true}};}
AssetSnapshot snapshot(QString asset,qint64 at){
    AssetSnapshot s;s.id="fake";s.accountScope="fake-only";s.timeMs=at;s.balanceComplete=true;s.reconciled=true;
    s.evidence={"fake","https://example.invalid","fake-only",at,at,true};
    AssetComponent c;c.id="fake";c.asset=asset;c.quantity="2";c.evidence=s.evidence;s.components={c};return s;
}
}
class PublicStreamTest final:public QObject {
    Q_OBJECT
private slots:
    void streamCoalescesAndPreservesOriginalTime(){
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Upbit,std::move(socket));
        QList<PublicTick> received;int deliveries=0;connect(&stream,&PublicQuoteStream::ticks,this,[&](const auto& data){received=data;++deliveries;});
        stream.setMarkets({"KRW-BTC"});QTRY_COMPARE(fake->urls.size(),1);QCOMPARE(fake->urls.first(),QUrl("wss://api.upbit.com/websocket/v1"));
        emit fake->connected();QCOMPARE(fake->sent.size(),1);QVERIFY(!fake->sent.first().contains("Authorization"));
        const auto at=QDateTime::currentMSecsSinceEpoch()-100;
        emit fake->received(tick("KRW-BTC","100",at));emit fake->received(tick("KRW-BTC","101",at+1));
        emit fake->received(tick("KRW-ETH","999",at+2));emit fake->received(tick("KRW-BTC","0",at+2));
        QCOMPARE(deliveries,0);QTRY_COMPARE_WITH_TIMEOUT(deliveries,1,1300);QCOMPARE(received.size(),1);
        QCOMPARE(received.first().quote.price,QString("101"));QCOMPARE(received.first().quote.evidence.sourceTimeMs,at+1);
        const auto receipt=received.first().quote.evidence.receivedAtMs;
        emit fake->received(tick("KRW-BTC","101",at+1));emit fake->received(tick("KRW-BTC","90",at));
        QTest::qWait(1100);QCOMPARE(deliveries,1);QCOMPARE(received.first().quote.evidence.receivedAtMs,receipt);
        stream.setMarkets({});emit fake->received(tick("KRW-BTC","102",at+2));QTest::qWait(1100);QCOMPARE(deliveries,1);
    }
    void utcDayTurnoverIsSeparateOptionalAndEpochBounded(){
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();
        PublicQuoteStream stream(PublicQuoteStream::Provider::Upbit,std::move(socket));
        QMap<QString,PublicTick> received;
        connect(&stream,&PublicQuoteStream::ticks,this,[&](const auto& rows){for(const auto& row:rows)received[row.market]=row;});
        stream.setMarkets({"KRW-BTC","KRW-ETH","KRW-SOL"});QTRY_COMPARE(fake->urls.size(),1);emit fake->connected();
        const auto at=QDateTime::currentMSecsSinceEpoch()-100;
        const auto day=QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyyMMdd");
        auto payload=[&](QString market,QString counter,QString kind){
            auto o=QJsonDocument::fromJson(tick(market,"100",at)).object();
            o.insert("acc_trade_price",counter);o.insert("acc_trade_price_24h","999999999");
            o.insert("trade_date",day);o.insert("trade_timestamp",QString::number(at));o.insert("stream_type",kind);
            return QJsonDocument(o).toJson(QJsonDocument::Compact);
        };
        emit fake->received(payload("KRW-BTC","0","REALTIME"));
        emit fake->received(payload("KRW-ETH","123.000000000000000001","SNAPSHOT"));
        emit fake->received(payload("KRW-SOL","invalid","REALTIME"));
        QTRY_COMPARE_WITH_TIMEOUT(received.size(),3,1500);
        QCOMPARE(received["KRW-BTC"].tradeValueUtcDay,QString("0"));QVERIFY(received["KRW-BTC"].realtime);
        QCOMPARE(received["KRW-BTC"].tradeDateUtc,day);QCOMPARE(received["KRW-BTC"].tradeTimestampMs,at);
        QCOMPARE(received["KRW-ETH"].tradeValueUtcDay,QString("123.000000000000000001"));QVERIFY(!received["KRW-ETH"].realtime);
        QVERIFY(received["KRW-SOL"].tradeValueUtcDay.isEmpty());QCOMPARE(received["KRW-SOL"].tradeValue24h,QString("999999999"));
        const auto epoch=received["KRW-BTC"].streamEpoch;QVERIFY(epoch>0);
        QCOMPARE(stream.streamEpoch(),epoch);
        emit fake->failed();QTRY_COMPARE_WITH_TIMEOUT(fake->urls.size(),2,1500);emit fake->connected();
        auto o=QJsonDocument::fromJson(payload("KRW-BTC","1","REALTIME")).object();
        const auto newer=QDateTime::currentMSecsSinceEpoch()-10;
        o.insert("timestamp",QString::number(newer));o.insert("trade_timestamp",QString::number(newer));
        emit fake->received(QJsonDocument(o).toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(received["KRW-BTC"].streamEpoch>epoch,1500);
        QCOMPARE(stream.streamEpoch(),received["KRW-BTC"].streamEpoch);
        QCOMPARE(received["KRW-BTC"].quote.price,QString("100"));
    }
    void binanceViewportChangesKeepCurrentTickAndConnection(){
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::move(socket));
        QList<PublicTick> delivered;connect(&stream,&PublicQuoteStream::ticks,this,[&](const auto& ticks){delivered.append(ticks);});
        stream.setMarkets({"BTCUSDT","ETHUSDT"});QTRY_COMPARE(fake->urls.size(),1);emit fake->connected();
        const auto epoch=stream.streamEpoch();const auto at=QDateTime::currentMSecsSinceEpoch()-100;
        emit fake->received(tick("BTCUSDT","101",at,true));emit fake->received(tick("ETHUSDT","50",at,true));
        stream.setMarkets({"BTCUSDT","ADAUSDT"});stream.setMarkets({"BTCUSDT","SOLUSDT"});
        QTRY_COMPARE(fake->sent.size(),3);QCOMPARE(fake->urls.size(),1);QCOMPARE(stream.streamEpoch(),epoch);
        QVERIFY(fake->sent[1].contains("UNSUBSCRIBE")&&fake->sent[1].contains("ethusdt"));QVERIFY(fake->sent[2].contains("solusdt"));QVERIFY(!fake->sent[2].contains("btcusdt"));
        QTRY_COMPARE(delivered.size(),1);QCOMPARE(delivered[0].market,"BTCUSDT");QCOMPARE(delivered[0].quote.price,"101");
        emit fake->received(tick("ETHUSDT","51",at+1,true));emit fake->received(tick("SOLUSDT","9",at+1,true));
        QTRY_COMPARE(delivered.size(),2);QCOMPARE(delivered.last().market,"SOLUSDT");QCOMPARE(fake->urls.size(),1);
    }
    void disconnectBacksOffAndStopCancelsReconnect(){
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::move(socket));
        stream.setMarkets({"BNBUSDT"});QTRY_COMPARE(fake->urls.size(),1);emit fake->connected();
        QCOMPARE(fake->urls.first(),QUrl("wss://stream.binance.com:9443/ws"));QVERIFY(fake->sent.first().contains("bnbusdt@miniTicker"));
        emit fake->failed();emit fake->failed();QTest::qWait(500);QCOMPARE(fake->urls.size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(fake->urls.size(),2,900);stream.setMarkets({});emit fake->failed();QTest::qWait(1100);QCOMPARE(fake->urls.size(),2);
        stream.setMarkets({"evil.example/USDT"});QTest::qWait(300);QCOMPARE(fake->urls.size(),2);
    }
    void binanceMiniTickerAddsOptionalChangeWithoutChangingPriceOrValuation(){
        // Public socket seam only. Binance documents c/o as the close/open of
        // the rolling 24h window, rather than the UTC calendar day.
        struct Case {QString market,price,open,change,value;bool withOpen=true;};
        const QString precise="2.500000000000000001",preciseValue="7000.0000000000000028";
        const QList<Case> cases{
            {"UPUSDT","2.50000000","2","+25.00%","7000"},
            {"DOWNUSDT","2.5","5","-50.00%","7000"},
            {"FLATUSDT","2.5","2.5","0.00%","7000"},
            {"FRACTIONUSDT","0.0025","0.0010","+150.00%","7"},
            {"PRECISEUSDT",precise,"2","+25.00%",preciseValue},
            {"MISSINGUSDT",precise,{}, {},preciseValue,false},
            {"EMPTYUSDT",precise,{}, {},preciseValue},
            {"ZEROUSDT",precise,"0",{},preciseValue},
            {"NEGATIVEUSDT",precise,"-2",{},preciseValue},
            {"TEXTUSDT",precise,"invalid",{},preciseValue},
            {"NANUSDT",precise,"NaN",{},preciseValue},
            {"INFUSDT",precise,"Infinity",{},preciseValue},
            {"PADUSDT",precise,"2 ",{},preciseValue},
            {"NEWLINEUSDT",precise,"2\n",{},preciseValue},
            {"EXPUSDT",precise,"1e309",{},preciseValue}
        };
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();
        PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::move(socket));
        QMap<QString,PublicTick> received;
        connect(&stream,&PublicQuoteStream::ticks,this,[&](const auto& ticks){for(const auto& item:ticks)received.insert(item.market,item);});
        QStringList markets;for(const auto& item:cases)markets.append(item.market);
        stream.setMarkets(markets);QTRY_COMPARE(fake->urls.size(),1);emit fake->connected();
        QCOMPARE(fake->urls.first(),QUrl("wss://stream.binance.com:9443/ws"));QCOMPARE(fake->sent.size(),1);
        const auto before=QDateTime::currentMSecsSinceEpoch(),at=before-100;
        for(const auto& item:cases){
            QJsonObject message{{"e","24hrMiniTicker"},{"s",item.market},{"c",item.price},{"E",QString::number(at)}};
            if(item.withOpen)message.insert("o",item.open);
            emit fake->received(QJsonDocument(message).toJson(QJsonDocument::Compact));
        }
        QTRY_COMPARE_WITH_TIMEOUT(received.size(),cases.size(),1500);
        for(const auto& item:cases){
            const auto actual=received.value(item.market);const auto& e=actual.quote.evidence;
            QCOMPARE(actual.quote.price,item.price);QCOMPARE(actual.quote.baseAsset,item.market.chopped(4));
            QCOMPARE(actual.quote.quoteAsset,QString("USDT"));QVERIFY(e.complete);
            QCOMPARE(e.url,QString("wss://stream.binance.com:9443/ws"));QCOMPARE(e.scope,"public:binance:"+item.market);
            QCOMPARE(e.sourceTimeMs,at);QVERIFY(e.receivedAtMs>=before);QVERIFY(e.receivedAtMs<=QDateTime::currentMSecsSinceEpoch());
            const auto valuation=AssetValuation::value({snapshot(actual.quote.baseAsset,at)},
                {actual.quote,quote("USDT","KRW","1400",at)},"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs);
            QVERIFY(valuation.complete);QCOMPARE(valuation.netAssets,item.value);
            if(item.change.isEmpty())QVERIFY2(actual.signedChangeRate.isEmpty(),qPrintable(item.market));
            else QCOMPARE(changePercentText(actual.signedChangeRate),item.change);
        }
    }
    void publicCacheValuesImmediatelyAndBinanceUsesOfficialCatalogue(){
        const auto now=QDateTime::currentMSecsSinceEpoch();CatalogueTransport transport;transport.large=true;PublicKrwPrices prices(&transport);
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::move(socket));prices.setBinanceStream(&stream);
        prices.setPublicQuotes({quote("BTC","KRW","100",now),quote("USDT","KRW","1400",now)},{"KRW-BTC","KRW-USDT"});
        QList<AssetQuote> received;connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& q){received.append(q);});
        prices.refresh({snapshot("BTC",now)});QVERIFY(!received.isEmpty());QCOMPARE(transport.sent.size(),0);QTRY_VERIFY(!prices.busy());
        prices.refresh({snapshot("BNB",now),snapshot("BAD",now),snapshot("UNKNOWN",now)});
        QTRY_VERIFY(!prices.busy());QCOMPARE(transport.sent.size(),1);QCOMPARE(transport.sent.first().url.host(),QString("api.binance.com"));
        QTRY_COMPARE(fake->urls.size(),1);emit fake->connected();QVERIFY(fake->sent.first().contains("bnbusdt@miniTicker"));QVERIFY(!fake->sent.first().contains("badusdt"));
        emit fake->received(tick("BNBUSDT","2.5",now,true));QTRY_VERIFY_WITH_TIMEOUT(std::any_of(received.cbegin(),received.cend(),[](const AssetQuote& q){return q.baseAsset=="BNB";}),1500);
        QCOMPARE(prices.report().unsupported,2);prices.clear();const auto count=received.size();emit fake->received(tick("BNBUSDT","3",now+1,true));QTest::qWait(1100);QCOMPARE(received.size(),count);
    }
    void crossPriceRequiresBothLegsAndKeepsEvidence_data(){
        QTest::addColumn<QString>("fault");for(const auto& f:{"fresh","stale-coin","stale-fx","missing-fx","future-fx","conflict","direct","stale-direct"})QTest::newRow(f)<<QString(f);
    }
    void crossPriceRequiresBothLegsAndKeepsEvidence(){
        QFETCH(QString,fault);const qint64 now=1792200000000LL;auto s=snapshot("BNB",now);
        auto coin=quote("BNB","USDT","2.123456789123456789",now-1000),fx=quote("USDT","KRW","1400.1",now-100);
        QList<AssetQuote> quotes{coin,fx},refs;
        if(fault=="stale-coin"){quotes[0].evidence.sourceTimeMs=now-180001;refs.append(quotes[0]);}
        if(fault=="stale-fx"){quotes[1].evidence.sourceTimeMs=now-180001;refs.append(quotes[1]);}
        if(fault=="missing-fx")quotes.removeLast();
        if(fault=="future-fx")quotes[1].evidence.sourceTimeMs=now+1000;
        if(fault=="conflict"){auto conflict=coin;conflict.price="3";quotes.append(conflict);}
        if(fault=="direct")quotes.append(quote("BNB","KRW","4",now));
        if(fault=="stale-direct")quotes.append(quote("BNB","KRW","4",now-180001));
        const auto r=AssetValuation::value({s},quotes,"KRW",now,DisplayFreshnessMs,refs);
        const auto expected=decimal::multiply("2",decimal::multiply(coin.price,fx.price));
        if(fault=="fresh"||fault=="stale-direct"){QCOMPARE(r.netAssets,expected);QCOMPARE(r.rows.first().conversionEvidence,QList<AssetEvidence>({coin.evidence,fx.evidence}));}
        else if(fault=="direct"){QCOMPARE(r.netAssets,QString("8"));QVERIFY(r.rows.first().conversionEvidence.isEmpty());}
        else{QVERIFY(!r.complete);QCOMPARE(r.knownSubtotal,QString("0"));
            if(fault.startsWith("stale")){QCOMPARE(r.rows.first().referenceValue,expected);QCOMPARE(r.rows.first().conversionEvidence.size(),2);}
            else QVERIFY(r.rows.first().referenceValue.isEmpty());}
    }
    void staleDirectUsdcUsesFreshCrossPriceAndReturnsToDirect_data(){
        QTest::addColumn<bool>("initiallyFresh");QTest::newRow("already-stale")<<false;QTest::newRow("expires-without-refresh")<<true;
    }
    void staleDirectUsdcUsesFreshCrossPriceAndReturnsToDirect(){
        QFETCH(bool,initiallyFresh);const auto now=QDateTime::currentMSecsSinceEpoch();CatalogueTransport transport;transport.usdcTime=now-(initiallyFresh?177000:600000);PublicKrwPrices prices(&transport);
        auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::move(socket));prices.setBinanceStream(&stream);
        const auto stale=quote("USDC","KRW","1400",transport.usdcTime);
        prices.setPublicQuotes({stale,quote("USDT","KRW","1400",now-1000)},{"KRW-USDC","KRW-USDT"});
        // The portfolio can retain a formerly fresh direct price after it expires.
        QMap<QString,AssetQuote> received{{"USDC:KRW",stale}};
        connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& quotes){for(const auto& q:quotes)received[q.baseAsset+":"+q.quoteAsset]=q;});
        prices.refresh({snapshot("USDC",now)});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);
        if(initiallyFresh){QCOMPARE(AssetValuation::value({snapshot("USDC",now)},received.values(),"KRW",now,DisplayFreshnessMs).netAssets,QString("2800"));QCOMPARE(prices.report().streamPending,0);}
        QTRY_VERIFY2_WITH_TIMEOUT(!fake->urls.isEmpty(),"USDC remains unvalued: an existing stale KRW market blocks the Binance subscription",1000);
        emit fake->connected();QVERIFY(fake->sent.last().contains("usdcusdt@miniTicker"));
        emit fake->received(tick("USDCUSDT","0.9997",now,true));
        const auto valuation=[&]{return AssetValuation::value({snapshot("USDC",now)},received.values(),"KRW",qMax(QDateTime::currentMSecsSinceEpoch(),now+5000),DisplayFreshnessMs,prices.referenceQuotes());};
        QTRY_VERIFY_WITH_TIMEOUT(valuation().complete,1500);QCOMPARE(valuation().netAssets,QString("2799.16"));
        QCOMPARE(valuation().rows.first().conversionEvidence.size(),2);QCOMPARE(prices.report().streamPending,0);
        if(!initiallyFresh)QCOMPARE(prices.referenceQuotes().first().evidence.sourceTimeMs,transport.usdcTime);
        prices.setPublicQuotes({quote("USDC","KRW","1410",QDateTime::currentMSecsSinceEpoch())},{"KRW-USDC","KRW-USDT"});
        QCOMPARE(valuation().netAssets,QString("2820"));QVERIFY(valuation().rows.first().conversionEvidence.isEmpty());
        prices.clear();QVERIFY(prices.referenceQuotes().isEmpty());
    }
    void expiredDirectCannotMakeInvalidCrossPriceCurrent_data(){
        QTest::addColumn<QString>("fault");for(const auto& f:{"stale-coin","stale-fx","stale-receipt","missing-fx","future-coin","conflict"})QTest::newRow(f)<<QString(f);
    }
    void expiredDirectCannotMakeInvalidCrossPriceCurrent(){
        QFETCH(QString,fault);const qint64 now=1792200000000LL;
        const auto direct=quote("USDC","KRW","1400",now-600000);
        auto coin=quote("USDC","USDT","0.9997",now-1000),fx=quote("USDT","KRW","1400",now-1000);
        if(fault=="stale-coin")coin.evidence.sourceTimeMs=now-180001;
        if(fault=="stale-fx")fx.evidence.sourceTimeMs=now-180001;
        if(fault=="stale-receipt")coin.evidence.receivedAtMs=now-180001;
        if(fault=="future-coin")coin.evidence.sourceTimeMs=now+1;
        QList<AssetQuote> quotes{direct,coin};if(fault!="missing-fx")quotes.append(fx);
        if(fault=="conflict"){auto conflict=coin;conflict.price="1.1";quotes.append(conflict);}
        const auto r=AssetValuation::value({snapshot("USDC",now)},quotes,"KRW",now,DisplayFreshnessMs,{direct});
        QVERIFY(!r.complete);QVERIFY(r.netAssets.isEmpty());QCOMPARE(r.knownSubtotal,QString("0"));QVERIFY(r.rows.first().value.isEmpty());
    }
    void livePublicFrames_data(){QTest::addColumn<bool>("binance");QTest::newRow("upbit")<<false;QTest::newRow("binance")<<true;}
    void livePublicFrames(){
        if(!qEnvironmentVariableIsSet("KEEFI_LIVE_PUBLIC"))QSKIP("Explicit public-network receipt only; no account access");
        QFETCH(bool,binance);PublicQuoteStream stream(binance?PublicQuoteStream::Provider::Binance:PublicQuoteStream::Provider::Upbit);
        int deliveries=0;QElapsedTimer elapsed;elapsed.start();qint64 first=0,last=0;QList<PublicTick> latest;
        connect(&stream,&PublicQuoteStream::ticks,this,[&](const auto& data){latest=data;++deliveries;if(!first)first=elapsed.elapsed();last=elapsed.elapsed();});
        stream.setMarkets(binance?QStringList{"BNBUSDT"}:QStringList{"KRW-BTC","KRW-USDT"});
        QTRY_VERIFY2_WITH_TIMEOUT(deliveries>=3,qPrintable(stream.status()),15000);
        QVERIFY(last-first>=1700);QVERIFY(!latest.isEmpty());qInfo("Public-only frames: %d batches, first=%lld ms, last=%lld ms",deliveries,first,last);
        stream.setMarkets({});
    }
    void livePublicValuation_data(){QTest::addColumn<QString>("asset");QTest::newRow("BNB")<<QString("BNB");QTest::newRow("USDC")<<QString("USDC");}
    void livePublicValuation(){
        if(!qEnvironmentVariableIsSet("KEEFI_LIVE_PUBLIC"))QSKIP("Explicit public-network receipt only; synthetic quantity");
        QFETCH(QString,asset);
        NetworkTradingTransport transport;PublicKrwPrices prices(&transport);PublicQuoteStream stream(PublicQuoteStream::Provider::Binance);prices.setBinanceStream(&stream);
        QMap<QString,AssetQuote> received;QElapsedTimer elapsed;elapsed.start();
        connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& quotes){for(const auto& q:quotes)received[q.baseAsset+":"+q.quoteAsset]=q;});
        const auto now=QDateTime::currentMSecsSinceEpoch();prices.refresh({snapshot(asset,now)});
        const auto ready=[&]{return received.contains(asset+":USDT")&&AssetValuation::value({snapshot(asset,now)},received.values(),"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs).complete;};
        QTRY_VERIFY2_WITH_TIMEOUT(ready(),qPrintable(prices.details()+"\n"+stream.status()),20000);
        const auto valuation=AssetValuation::value({snapshot(asset,now)},received.values(),"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs);
        const auto cross=AssetValuation::value({snapshot(asset,now)},{received.value(asset+":USDT"),received.value("USDT:KRW")},"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs);
        QVERIFY(cross.complete);QCOMPARE(cross.rows.first().conversionEvidence.size(),2);
        qInfo("Public-only synthetic %s valuation: first=%lld ms, selected conversion legs=%lld, verified cross legs=2",qPrintable(asset),elapsed.elapsed(),qint64(valuation.rows.first().conversionEvidence.size()));
        prices.clear();
    }
};
QTEST_GUILESS_MAIN(PublicStreamTest)
#include "tst_public_stream.moc"
