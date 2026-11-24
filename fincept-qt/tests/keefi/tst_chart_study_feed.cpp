#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/StockChartSource.h"
#include <QtTest>
using namespace keefi;
namespace {
class Http final:public TradingTransport {
public:
    quint64 token=0;QList<RequestSpec> sent;int aborted=0;
    void send(quint64 t,const RequestSpec& r)override{token=t;sent.append(r);}
    void abort()override{++aborted;}
};
constexpr qint64 Now=1728000000000+250*60000+10000;
ChartStudyRequest request(QString code="KRW-SYN",int priority=10){return {{"coin","upbit",code},60000,priority};}
HttpResult response(qint64 now=Now){
    QJsonArray rows;const auto end=now/60000*60000;
    for(int i=30;i>=0;--i){const auto at=end-i*60000;
        rows.append(QJsonObject{{"market","KRW-SYN"},{"unit","1"},{"timestamp",QString::number(at+1000)},
          {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
          {"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","101"},{"candle_acc_trade_volume","2"}});}
    HttpResult r;r.status=200;r.body=QJsonDocument(rows).toJson();return r;
}
}
class ChartStudyFeedTest final:public QObject {
    Q_OBJECT
private slots:
    void metadataRefreshKeepsCompletedObservationAndStopsRequests(){
        Http h;qint64 now=Now;ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.setDemands("chart",{request()});feed.pump();
        emit h.finished(h.token,response());QVERIFY(feed.observation("coin:upbit:KRW-SYN",60000).series.available);
        feed.setVerifiedUpbitMarkets({},false);auto value=feed.observation("coin:upbit:KRW-SYN",60000);
        QVERIFY(!value.series.candles.isEmpty());QVERIFY(value.delayed);QVERIFY(!value.loading);
        now+=61000;feed.pump();QCOMPARE(h.sent.size(),1);
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(h.sent.size(),2);
        QVERIFY(!feed.observation("coin:upbit:KRW-SYN",60000).series.candles.isEmpty());
        feed.setVerifiedUpbitMarkets({"KRW-OTHER"},true);QVERIFY(feed.observation("coin:upbit:KRW-SYN",60000).series.candles.isEmpty());
    }
    void pausedBinanceSpotMarketCannotSendStudyRequest(){
        Http metadata,h;ChartInstrumentCatalog catalog(&metadata);catalog.ensure("binance");
        HttpResult r;r.status=200;r.body=QJsonDocument(QJsonObject{{"symbols",QJsonArray{QJsonObject{
            {"symbol","SYNUSDT"},{"baseAsset","SYN"},{"quoteAsset","USDT"},{"status","BREAK"},{"isSpotTradingAllowed",true}}}}}).toJson();
        emit metadata.finished(metadata.token,r);QCOMPARE(catalog.instruments("binance").size(),1);
        ChartStudyFeed feed(&h,&catalog);feed.setDemands("chart",{{{"coin","binance","SYNUSDT"},60000,0}});feed.pump();
        QCOMPARE(h.sent.size(),0);QCOMPARE(feed.observation("coin:binance:SYNUSDT",60000).status,QString("지원 없음"));
    }
    void unknownAndRevokedMarketsNeverReachSharedStudyTransport(){
        Http h;qint64 now=Now;ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({},false);feed.setDemands("chart",{request()});feed.pump();
        QCOMPARE(h.sent.size(),0);QVERIFY(!feed.observation("coin:upbit:KRW-SYN",60000).loading);
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(h.sent.size(),1);
        const auto token=h.token;feed.setVerifiedUpbitMarkets({"KRW-BTC"},true);
        QCOMPARE(h.aborted,1);emit h.finished(token,response());
        const auto revoked=feed.observation("coin:upbit:KRW-SYN",60000);QVERIFY(!revoked.series.available);QCOMPARE(revoked.status,QString("지원 없음"));
        now+=61000;feed.pump();QCOMPARE(h.sent.size(),1);
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(h.sent.size(),2);
    }
    void missingMarketIsTerminalAcrossAllOwnersAndIntervals(){
        Http h;qint64 now=Now;ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);
        feed.setDemands("chart",{request()});feed.setDemands("watch",{{{"coin","upbit","KRW-SYN"},3600000,20}});feed.pump();
        QCOMPARE(h.sent.size(),1);HttpResult missing;missing.status=404;missing.body=R"({"error":{"name":404,"message":"Code not found"}})";
        emit h.finished(h.token,missing);now+=61000;feed.pump();QCOMPARE(h.sent.size(),1);
        QCOMPARE(feed.observation("coin:upbit:KRW-SYN",60000).status,QString("지원 없음"));
        QCOMPARE(feed.observation("coin:upbit:KRW-SYN",3600000).status,QString("지원 없음"));
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);feed.pump();QCOMPARE(h.sent.size(),1);
    }
    void rankingKeepsAllThreeVisibleMarketsBounded(){
        Http h;ChartStudyFeed feed(&h);QList<ChartStudyRequest> rows;
        for(int i=0;i<70;++i)rows.append(request(QString("KRW-S%1").arg(i)));
        feed.setDemands("explore-ranking",rows);QCOMPARE(feed.demandCount(),60);
        feed.clearOwner("explore-ranking");QCOMPARE(feed.demandCount(),0);
        feed.setDemands("watch",rows);QCOMPARE(feed.demandCount(),32);
    }
    void freshReceiptDoesNotRefreshOldCoinHistory(){
        Http h;qint64 now=Now;ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});
        feed.setDemands("chart",{request()});feed.pump();emit h.finished(h.token,response(now-3600000));
        const auto old=feed.observation("coin:upbit:KRW-SYN",60000);
        QVERIFY(old.series.available);QVERIFY(old.delayed);QCOMPARE(old.status,QStringLiteral("지연"));
    }
    void hiddenOwnerStopsWhileSharedOwnerRetainsRequest(){
        Http h;qint64 now=Now;ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});
        feed.setDemands("watch",{request()});feed.setDemands("chart",{request()});
        QCOMPARE(feed.ownerCount("coin:upbit:KRW-SYN",60000),2);
        feed.pump();QCOMPARE(h.sent.size(),1);const auto token=h.token;
        feed.clearOwner("watch");QCOMPARE(h.aborted,0);
        feed.clearOwner("chart");QCOMPARE(h.aborted,1);QCOMPARE(feed.demandCount(),0);
        QSignalSpy changes(&feed,&ChartStudyFeed::changed);emit h.finished(token,response());
        QCOMPARE(changes.size(),0);now+=100000;feed.pump();QCOMPARE(h.sent.size(),1);
    }
    void cacheAndOneSecondPacing(){
        Http h;qint64 now=Now;ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});
        feed.setDemands("watch",{request("KRW-SECOND",10),request("KRW-SYN",0)});feed.pump();
        QCOMPARE(h.sent.size(),1);QVERIFY(h.sent[0].url.toString().contains("KRW-SYN"));
        emit h.finished(h.token,response());auto result=feed.observation("coin:upbit:KRW-SYN",60000);
        QVERIFY(result.series.available);QCOMPARE(result.series.candles.size(),30);QVERIFY(!result.series.forming);
        now+=999;feed.pump();QCOMPARE(h.sent.size(),1);now+=1;feed.pump();QCOMPARE(h.sent.size(),2);
        feed.clearOwner("watch");feed.setDemands("chart",{request()});feed.pump();QCOMPARE(h.sent.size(),2);
    }
    void failureAndConnectionRemainLocal(){
        Http h,stockHttp;qint64 now=Now;StockChartSource stock(&stockHttp,nullptr,[&]{return now;});
        ChartStudyFeed feed(&h,nullptr,nullptr,[&]{return now;});feed.setStockSource(&stock);
        feed.setDemands("watch",{{{"stock","KR","005930"},86400000,0},request()});feed.pump();
        QVERIFY(stockHttp.sent.isEmpty());QVERIFY(h.sent.isEmpty());
        const auto waiting=feed.observation("stock:KR:005930",86400000);QVERIFY(!waiting.series.available);
        QVERIFY(waiting.status.contains(QStringLiteral("연결"))||waiting.status.contains(QStringLiteral("금고")));
        now+=1000;feed.pump();QCOMPARE(h.sent.size(),1);
        HttpResult failed;failed.status=503;emit h.finished(h.token,failed);
        const auto unavailable=feed.observation("coin:upbit:KRW-SYN",60000);
        QVERIFY(!unavailable.series.available);QVERIFY(!unavailable.status.isEmpty());
        feed.clearPrivate();QVERIFY(!stock.active());QCOMPARE(feed.demandCount(),1);
    }
};
QTEST_GUILESS_MAIN(ChartStudyFeedTest)
#include "tst_chart_study_feed.moc"
