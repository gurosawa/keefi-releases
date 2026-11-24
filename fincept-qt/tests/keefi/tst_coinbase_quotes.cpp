#include "keefi/CoinbaseQuotes.h"
#include "keefi/TradingTransport.h"
#include <QTest>
#include <QSignalSpy>
#include <QDateTime>
#include <QElapsedTimer>
using namespace keefi;
namespace {
constexpr qint64 Now=1792602000000;
AssetQuote quote(QString base,QString currency,QString price){return {base,currency,price,{"official test","https://example.test/public","public",Now-1000,Now,true}};}
}
class CoinbaseQuotesTest:public QObject {
    Q_OBJECT
  private slots:
    void stoppedSelectionDropsOldRepliesAndRejectsExpiredPrices(){
        class Fake:public TradingTransport { public: QList<quint64> tokens;int aborts=0;
            void send(quint64 token,const RequestSpec& r)override{QVERIFY(CoinbaseQuotes::isRequest(r));tokens.append(token);}void abort()override{++aborts;}
        } transport;
        CoinbaseQuotes model(&transport);model.setAsset("BTC");QCOMPARE(transport.tokens.size(),1);
        const auto old=transport.tokens.last();HttpResult r;r.status=200;
        r.body=QJsonDocument(QJsonObject{{"price","100"},{"time",QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODateWithMs)}}).toJson();
        emit transport.finished(old,r);QCOMPARE(model.quotes().size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),2,2000);emit transport.finished(transport.tokens.last(),r);
        QCOMPARE(model.quotes().size(),2);QVERIFY(model.quotes(QDateTime::currentMSecsSinceEpoch()+181000).isEmpty());
        model.setAsset({});QVERIFY(model.quotes().isEmpty());emit transport.finished(old,r);QVERIFY(model.quotes().isEmpty());
    }
    void passiveObservationSeparatesResponsesFromDemandStop(){
        class Fake:public TradingTransport { public: QList<quint64> tokens;
            void send(quint64 token,const RequestSpec& request)override{QVERIFY(CoinbaseQuotes::isRequest(request));tokens.append(token);}void abort()override{}
        } transport;
        CoinbaseQuotes model(&transport);QSignalSpy observed(&model,SIGNAL(quoteObserved(QString)));
        QVERIFY2(observed.isValid(),"Passive display observers must distinguish a completed quote from demand clearing");
        model.setAsset("USDT");QCOMPARE(transport.tokens.size(),1);QCOMPARE(observed.size(),0);
        HttpResult result;result.status=200;result.body=QJsonDocument(QJsonObject{{"price","0.998"},{"time",QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODateWithMs)}}).toJson();
        emit transport.finished(transport.tokens.last(),result);QCOMPARE(observed.size(),1);QCOMPARE(observed.last().first().toString(),QString("USDT"));QCOMPARE(model.quotes().value("USDT").price,QString("0.998"));
        model.setAsset({});QCOMPARE(observed.size(),1);QVERIFY(model.quotes().isEmpty());QCOMPARE(transport.tokens.size(),1);
        model.setAsset("USDT");QTRY_COMPARE_WITH_TIMEOUT(transport.tokens.size(),2,2000);result.status=503;
        emit transport.finished(transport.tokens.last(),result);QCOMPARE(observed.size(),2);QVERIFY(model.quotes().isEmpty());
        model.setAsset({});emit transport.finished(transport.tokens.last(),result);QCOMPARE(observed.size(),2);QCOMPARE(transport.tokens.size(),2);
    }
    void parallelLegsSurviveOutOfOrderRepliesAndOnlyReuseFreshBridge(){
        class Wire:public TradingTransport{public:QList<quint64> tokens;QStringList paths;
            void send(quint64 t,const RequestSpec& r)override{tokens<<t;paths<<r.url.path();}void abort()override{}
        } asset,bridge;
        CoinbaseQuotes model(&asset,nullptr,&bridge);model.setAsset("BTC");
        QCOMPARE(asset.paths,QStringList{"/products/BTC-USD/ticker"});QCOMPARE(bridge.paths,QStringList{"/products/USDT-USD/ticker"});
        HttpResult ok;ok.status=200;ok.body=QJsonDocument(QJsonObject{{"price","1.002"},{"time",QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODateWithMs)}}).toJson();
        emit bridge.finished(bridge.tokens.last(),ok);QVERIFY(model.quotes().contains("USDT"));QVERIFY(!model.quotes().contains("BTC"));
        const auto old=asset.tokens.last();model.setAsset("ETH");QCOMPARE(bridge.tokens.size(),1);QCOMPARE(asset.paths.last(),QString("/products/ETH-USD/ticker"));
        emit asset.finished(old,ok);QVERIFY(!model.quotes().contains("BTC"));QVERIFY(!model.quotes().contains("ETH"));
        HttpResult error;error.status=503;emit asset.finished(asset.tokens.last(),error);QVERIFY(!model.unlisted("ETH"));
        model.setAsset("FAKE");error.status=404;error.networkError=true;emit asset.finished(asset.tokens.last(),error);QVERIFY(model.unlisted("FAKE"));QVERIFY(!model.quotes().contains("FAKE"));
    }
    void actualPublicTicks(){
        if(!qEnvironmentVariableIsSet("KEEFI_G8_LIVE_PUBLIC"))QSKIP("opt-in official public GET only");
        NetworkTradingTransport transport,bridge;CoinbaseQuotes model(&transport,nullptr,&bridge);int responses=0;QElapsedTimer timer;timer.start();
        for(auto* wire:{&transport,&bridge})connect(wire,&TradingTransport::finished,this,[&](quint64,const HttpResult& r){++responses;qInfo("Coinbase public status: %d after %lld ms",r.status,timer.elapsed());});
        model.setAsset("BTC");QTRY_COMPARE_WITH_TIMEOUT(responses,2,20000);
        const auto values=model.quotes();qInfo("Validated recent public Coinbase legs: %lld",qint64(values.size()));
        // The exchange can have no recent USDT trade. This is unavailable, never a $1 peg.
        for(const auto& q:values)QVERIFY(q.evidence.complete);
        model.setAsset({});QVERIFY(model.quotes().isEmpty());
    }
    void tickerKeepsTradeTimeAndOnlyExactPublicReads(){
        auto r=CoinbaseQuotes::request("BTC");QVERIFY(CoinbaseQuotes::isRequest(r));QVERIFY(NetworkTradingTransport::allowed(r));
        auto bad=r;bad.authenticated=true;QVERIFY(!CoinbaseQuotes::isRequest(bad));
        bad=r;bad.headers.insert("Authorization","secret");QVERIFY(!CoinbaseQuotes::isRequest(bad));
        bad=r;bad.url.setQuery("extra=1");QVERIFY(!CoinbaseQuotes::isRequest(bad));
        QVERIFY(!CoinbaseQuotes::request("../BTC").url.isValid());
        HttpResult response;response.status=200;response.body=R"({"price":"100.25","time":"2026-10-21T16:59:59.123456Z"})";
        const auto value=CoinbaseQuotes::parse("BTC",response,Now);
        QCOMPARE(value.price,QString("100.25"));QCOMPARE(value.quoteAsset,QString("USD"));QCOMPARE(value.evidence.sourceTimeMs,Now-877);
        response.status=503;QVERIFY(CoinbaseQuotes::parse("BTC",response,Now).price.isEmpty());
        response.status=200;QVERIFY(CoinbaseQuotes::parse("BTC",response,Now-2000).price.isEmpty());
    }
    void conversionRequiresObservedUsdUsdtAndMatchingTimes(){
        auto cb=quote("BTC","USD","101"),bn=quote("BTC","USDT","100"),bridge=quote("USDT","USD","1.01");
        const auto r=CoinbaseQuotes::priceGap(cb,bn,bridge,Now);QVERIFY(r.available);QCOMPARE(r.percent,QString("0"));
        bridge.price="1";QCOMPARE(CoinbaseQuotes::priceGap(cb,bn,bridge,Now).percent,QString("1"));
        bridge={};QVERIFY(!CoinbaseQuotes::priceGap(cb,bn,bridge,Now).available);
        bridge=quote("USDT","USD","1.01");bridge.evidence.sourceTimeMs=Now-40000;
        QVERIFY(!CoinbaseQuotes::priceGap(cb,bn,bridge,Now).available);
        bridge=quote("USDT","USD","1.01");bridge.evidence.sourceTimeMs=Now+1;
        QVERIFY(!CoinbaseQuotes::priceGap(cb,bn,bridge,Now).available);
    }
};
QTEST_GUILESS_MAIN(CoinbaseQuotesTest)
#include "tst_coinbase_quotes.moc"
