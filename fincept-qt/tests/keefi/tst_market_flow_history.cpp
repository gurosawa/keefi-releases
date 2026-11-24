#include "keefi/MarketFlowHistory.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QSignalSpy>
#include <QLocale>
#include <QTimeZone>
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Now=1792992120000,Start=1792900800000,Close=1792904400000;
class FakeTransport final : public TradingTransport {
  public:
    struct Sent {quint64 token;RequestSpec request;};QList<Sent> sent;int aborts=0;
    void send(quint64 token,const RequestSpec& request)override{sent.append({token,request});}
    void abort()override{++aborts;}
    void answer(quint64 token,QByteArray body,int status=200){HttpResult response;response.status=status;response.body=body;emit finished(token,response);}
};
MarketInstrument instrument(){MarketInstrument item;item.symbol="BTCUSDT";item.baseAsset="BTC";item.quoteAsset="USDT";item.status="TRADING";item.market="spot";item.tradable=true;return item;}
QByteArray localRow(const QByteArray& market,const QByteArray& price){return "[{\"market\":\""+market+"\",\"candle_date_time_utc\":\"2026-10-25T04:00:00\",\"opening_price\":"+price+",\"high_price\":"+price+",\"low_price\":"+price+",\"trade_price\":"+price+",\"timestamp\":1792904399000,\"candle_acc_trade_volume\":1,\"unit\":60}]";}
QByteArray remoteRow(){return "[[1792900800000,\"100000\",\"100000\",\"100000\",\"100000\",\"1\",1792904399999,\"1\",1,\"1\",\"1\",\"0\"]]";}
}
class MarketFlowHistoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void completedHourCacheExpiresAndPublicDisplayNameDoesNotInvalidate(){
        qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
        source.setAsset("BTC");source.setInstruments({instrument()});source.setActive(true);
        transport.answer(transport.sent.last().token,localRow("KRW-BTC","142800000"));clock+=1000;source.tick();
        transport.answer(transport.sent.last().token,remoteRow());clock+=1000;source.tick();
        transport.answer(transport.sent.last().token,localRow("KRW-USDT","1400"));QVERIFY(source.snapshot().available);
        source.setActive(false);source.setActive(true);QCOMPARE(transport.sent.size(),3);
        auto renamed=instrument();renamed.name="Synthetic display name";source.setInstruments({renamed});QCOMPARE(transport.sent.size(),3);QVERIFY(source.snapshot().available);
        const auto nextHour=(clock/3600000+1)*3600000;QVERIFY(!source.snapshot(nextHour).available);
        QSignalSpy changed(&source,&MarketFlowHistory::changed);clock=nextHour;source.tick();
        QCOMPARE(transport.sent.size(),4);QVERIFY(!source.snapshot().available);QCOMPARE(changed.size(),1);
        source.tick();QCOMPARE(transport.sent.size(),4); // One pending request, no repeated invalidation.
    }
    void nativeIdentityFormingAndMissingTargetNeverPublish(){
        for(const auto kind:{"identity","forming","missing-target"}){
            qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
            source.setAsset("BTC");source.setInstruments({instrument()});source.setActive(true);
            auto row=localRow("KRW-BTC","142800000");
            if(QByteArray(kind)=="identity")row.replace("KRW-BTC","KRW-ETH");
            if(QByteArray(kind)=="forming"){row.replace("2026-10-25T04:00:00","2026-10-26T05:00:00");row.replace("1792904399000","1792992120000");}
            if(QByteArray(kind)=="missing-target")row.replace("2026-10-25T04:00:00","2026-10-25T03:00:00");
            transport.answer(transport.sent.last().token,row);clock+=1000;source.tick();
            if(transport.sent.size()==2){transport.answer(transport.sent.last().token,remoteRow());clock+=1000;source.tick();}
            if(transport.sent.size()==3)transport.answer(transport.sent.last().token,localRow("KRW-USDT","1400"));
            QVERIFY(!source.snapshot().available);
        }
    }
    void retryAfterAnd503PauseBeforeRestartingWholeCycle_data(){
        QTest::addColumn<int>("status");QTest::addColumn<QByteArray>("header");QTest::addColumn<int>("localSeconds");QTest::addColumn<qint64>("delay");
        QTest::newRow("429-seconds")<<429<<QByteArray("5")<<0<<qint64(5000);
        QTest::newRow("429-local-longer")<<429<<QByteArray("5")<<7<<qint64(7000);
        QTest::newRow("429-imf-date")<<429<<QByteArray("Mon, 26 Oct 2026 05:22:07 GMT")<<0<<qint64(7000);
        QTest::newRow("503")<<503<<QByteArray{}<<0<<qint64(30000);
    }
    void retryAfterAnd503PauseBeforeRestartingWholeCycle(){
        QFETCH(int,status);QFETCH(QByteArray,header);QFETCH(int,localSeconds);QFETCH(qint64,delay);
        qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
        source.setAsset("BTC");source.setInstruments({instrument()});source.setActive(true);QCOMPARE(transport.sent.size(),1);
        HttpResult response;response.status=status;response.localRetryAfterSeconds=localSeconds;if(!header.isEmpty())response.headers.insert("retry-after",header);
        emit transport.finished(transport.sent.last().token,response);QVERIFY(!source.snapshot().available);
        clock+=delay-1;source.refresh();source.tick();QCOMPARE(transport.sent.size(),1);
        ++clock;source.tick();QCOMPARE(transport.sent.size(),2);
        QCOMPARE(transport.sent.last().request.url,transport.sent.first().request.url); // Restart all facts, never mix an old partial leg.
    }
    void timedOutResponseCannotPopulateCache(){
        qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
        source.setAsset("BTC");source.setInstruments({instrument()});source.setActive(true);const auto old=transport.sent.last().token;
        clock+=90001;source.tick();QCOMPARE(transport.aborts,1);
        transport.answer(old,localRow("KRW-BTC","142800000"));QVERIFY(!source.snapshot().available);QCOMPARE(transport.sent.size(),1);
    }
    void changedSelectionHiddenPageAndRevokedCatalogCancelLateResponses(){
        qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
        source.setAsset("BTC");source.setInstruments({instrument()});source.setActive(true);QCOMPARE(transport.sent.size(),1);
        QSignalSpy changed(&source,&MarketFlowHistory::changed);const auto old=transport.sent.first().token;
        source.setAsset("ETH");QCOMPARE(transport.aborts,1);
        transport.answer(old,localRow("KRW-BTC","142800000"));QCOMPARE(transport.sent.size(),1);QVERIFY(!source.snapshot().available);
        auto eth=instrument();eth.symbol="ETHUSDT";eth.baseAsset="ETH";source.setInstruments({eth});clock+=1000;source.tick();QCOMPARE(transport.sent.size(),2);
        const auto next=transport.sent.last().token;QVERIFY(next!=old);
        transport.answer(old,localRow("KRW-BTC","142800000"));QCOMPARE(transport.sent.size(),2);
        source.setActive(false);QCOMPARE(transport.aborts,2);
        transport.answer(next,localRow("KRW-ETH","100"));clock+=5000;source.tick();QCOMPARE(transport.sent.size(),2);
        source.setActive(true);QCOMPARE(transport.sent.size(),3);const auto revoked=transport.sent.last().token;
        source.setInstruments({});QCOMPARE(transport.aborts,3);transport.answer(revoked,localRow("KRW-ETH","100"));
        clock+=5000;source.tick();QCOMPARE(transport.sent.size(),3);QVERIFY(!source.snapshot().available);QCOMPARE(changed.size(),0);
    }
    void destructionAbortsItsDedicatedPendingTransport(){
        qint64 clock=Now;FakeTransport transport;
        auto source=std::make_unique<MarketFlowHistory>(&transport,[&]{return clock;});
        source->setAsset("BTC");source->setInstruments({instrument()});source->setActive(true);QCOMPARE(transport.sent.size(),1);
        const auto token=transport.sent.first().token;source.reset();QCOMPARE(transport.aborts,1);
        transport.answer(token,localRow("KRW-BTC","142800000")); // No callback remains after destruction.
    }
    void unverifiedOrAmbiguousCatalogCannotStartReads_data(){
        QTest::addColumn<QString>("kind");
        for(const auto kind:{"missing","duplicate","base","quote","market","status","tradable","symbol","asset"})QTest::newRow(kind)<<QString(kind);
    }
    void unverifiedOrAmbiguousCatalogCannotStartReads(){
        QFETCH(QString,kind);qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
        auto item=instrument();
        if(kind=="base")item.baseAsset="ETH";if(kind=="quote")item.quoteAsset="USD";if(kind=="market")item.market="usdm";
        if(kind=="status")item.status="BREAK";if(kind=="tradable")item.tradable=false;if(kind=="symbol")item.symbol="ETHUSDT";
        QList<MarketInstrument> items{item};if(kind=="missing")items.clear();if(kind=="duplicate")items.append(item);
        source.setInstruments(items);source.setAsset(kind=="asset"?"../BTC":"BTC");source.setActive(true);
        clock+=1000;source.tick();QVERIFY(transport.sent.isEmpty());QVERIFY(!source.snapshot().available);
    }
    void visibleVerifiedSelectionFetchesThreeAllowedHoursAndPublishesOnce(){
        qint64 clock=Now;FakeTransport transport;MarketFlowHistory source(&transport,[&]{return clock;});
        source.setAsset("BTC");source.setInstruments({instrument()});QVERIFY(transport.sent.isEmpty());
        source.setActive(true);QCOMPARE(transport.sent.size(),1);
        QSignalSpy changed(&source,&MarketFlowHistory::changed);
        QCOMPARE(transport.sent[0].request.url, QUrl("https://api.upbit.com/v1/candles/minutes/60?market=KRW-BTC&count=200"));
        transport.answer(transport.sent[0].token,localRow("KRW-BTC","142800000"));
        source.tick();QCOMPARE(transport.sent.size(),1);
        clock+=1000;source.tick();QCOMPARE(transport.sent.size(),2);
        QCOMPARE(transport.sent[1].request.url,QUrl("https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=200"));
        transport.answer(transport.sent[1].token,remoteRow());clock+=1000;source.tick();QCOMPARE(transport.sent.size(),3);
        QCOMPARE(transport.sent[2].request.url,QUrl("https://api.upbit.com/v1/candles/minutes/60?market=KRW-USDT&count=200"));
        transport.answer(transport.sent[2].token,localRow("KRW-USDT","1400"));
        const auto value=source.snapshot();QVERIFY(value.available);QCOMPARE(value.percent,QString("2"));QCOMPARE(value.comparisonCloseMs,Close);
        QCOMPARE(value.receivedAtMs,Now);QCOMPARE(value.asset,QString("BTC"));QCOMPARE(changed.size(),1);
        for(const auto& sent:transport.sent){QVERIFY(MarketCandles::isRequest(sent.request));QVERIFY(NetworkTradingTransport::allowed(sent.request));QVERIFY(!sent.request.authenticated);}
        source.refresh();source.tick();QCOMPARE(transport.sent.size(),3); // Immutable completed hour is cached.
    }
};
QTEST_GUILESS_MAIN(MarketFlowHistoryTest)
#include "tst_market_flow_history.moc"
