#include "keefi/RankingFuturesFeed.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartStudyRequestPacer.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include <QTest>
#include <QJsonArray>
#include <QSignalSpy>
#include <limits>
using namespace keefi;
namespace {
constexpr qint64 Hour=3600000,End=1728000000000,Now=End+10000;
qint64 sameCompletedHourNow(qint64 advanceMs){
    auto now=QDateTime::currentMSecsSinceEpoch()+50;
    // Paired observations and cache reuse need one completed hour. Move only
    // near its end; the real catalogue remains within its five-minute freshness.
    if(now/Hour!=(now+advanceMs)/Hour)now=(now/Hour+1)*Hour+50;
    return now;
}
HttpResult ok(const QJsonArray& rows){HttpResult r;r.status=200;r.body=QJsonDocument(rows).toJson();return r;}
QJsonArray oiRows(){QJsonArray result;for(int i=24;i>=0;--i)result.append(QJsonObject{{"symbol","BTCUSDT"},{"timestamp",QString::number(End-i*Hour)},{"sumOpenInterest",i?"100":"120"},{"sumOpenInterestValue","999999"}});return result;}
QJsonArray candle(qint64 start,QString quote="100",QString buy="60"){
    return {QString::number(start),"100","110","90","101","1",QString::number(start+Hour-1),quote,"5","0.5",buy,"0"};
}
QJsonArray candles(QString buy="60"){return {candle(End-2*Hour,"100",buy),candle(End-Hour,"100",buy),candle(End,"100","0")};}
class Http final:public TradingTransport {
public:
    QList<RequestSpec> sent;QList<quint64> tokens;int aborted=0;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);sent.append(request);}
    void abort()override{++aborted;}
    void answer(const HttpResult& result){emit finished(tokens.last(),result);}
};
HttpResult catalogReply(const QStringList& bases,bool contracts=false){
    QJsonArray rows;for(const auto& base:bases){QJsonObject row{{"symbol",base+"USDT"},{"baseAsset",base},{"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}};
        if(contracts){row["marginAsset"]="USDT";row["contractType"]="PERPETUAL";row["underlyingType"]="COIN";}rows.append(row);}
    HttpResult response;response.status=200;response.body=QJsonDocument(QJsonObject{{"symbols",rows}}).toJson();return response;
}
HttpResult interest(const QString& base,qint64 now){auto rows=oiRows();for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["symbol"]=base+"USDT";row["timestamp"]=QString::number(now/Hour*Hour-(24-i)*Hour);rows[i]=row;}return ok(rows);}
HttpResult flow(qint64 now,QString buy){QJsonArray rows;for(int i=2;i>=0;--i)rows.append(candle(now/Hour*Hour-i*Hour,"100",buy));return ok(rows);}
}
class RankingFuturesFeedTest final:public QObject {
    Q_OBJECT
private slots:
    void unsupportedDemandIsIdempotent(){
        Http http;RankingFuturesFeed feed(&http);QSignalSpy changes(&feed,&RankingFuturesFeed::changed);
        feed.setTargets({},RankingStudy::Compression);feed.setTargets({"BTC"},RankingStudy::Pullback);feed.clear();
        QCOMPARE(changes.size(),0);QVERIFY(http.sent.isEmpty());
    }
    void openInterestUsesExact24hQuantityEndpoints(){
        const auto result=RankingFuturesFeed::parseOpenInterest24h("BTC",ok(oiRows()),Now);
        QVERIFY(result.available);QCOMPARE(result.throughMs,End);QVERIFY(result.text.contains("20.00"));QVERIFY(result.text.contains("▲"));
        auto zero=oiRows();auto row=zero.last().toObject();row["sumOpenInterest"]="0";zero[24]=row;
        const auto z=RankingFuturesFeed::parseOpenInterest24h("BTC",ok(zero),Now);QVERIFY(z.available);QVERIFY(z.text.contains("▼"));QVERIFY(z.text.contains("100.00"));
    }
    void openInterestRequiresNewHourEndpointAfterRollover(){
        const auto before=RankingFuturesFeed::parseOpenInterest24h("BTC",ok(oiRows()),End+Hour-1);
        QVERIFY(before.available);QCOMPARE(before.throughMs,End);
        const auto expired=RankingFuturesFeed::parseOpenInterest24h("BTC",ok(oiRows()),End+Hour);
        QVERIFY(!expired.available);
        const auto refreshed=RankingFuturesFeed::parseOpenInterest24h("BTC",interest("BTC",End+Hour),End+Hour);
        QVERIFY(refreshed.available);QCOMPARE(refreshed.throughMs,End+Hour);
    }
    void openInterestRejectsWrongMissingAndFutureEndpoints(){
        for(const auto fault:{QString("gap"),QString("duplicate"),QString("wrong-base"),QString("future"),QString("value-only"),QString("zero-baseline")}){
            auto input=oiRows();auto row=input[0].toObject();
            if(fault=="gap")input.removeAt(12);
            if(fault=="duplicate")input.append(input[0]);
            if(fault=="wrong-base"){row["symbol"]="ETHUSDT";input[0]=row;}
            if(fault=="future"){row["timestamp"]=QString::number(End+Hour);input[0]=row;}
            if(fault=="value-only"){row.remove("sumOpenInterest");input[0]=row;}
            if(fault=="zero-baseline"){row["sumOpenInterest"]="0";input[0]=row;}
            QVERIFY2(!RankingFuturesFeed::parseOpenInterest24h("BTC",ok(input),Now).available,qPrintable(fault));
        }
    }
    void spotFuturesUsesMatchingCompletedQuoteNotionals(){
        const auto result=RankingFuturesFeed::parseSpotFutures("BTC",ok(candles("60")),ok(candles("40")),Now);
        QVERIFY(result.available);QCOMPARE(result.throughMs,End);QCOMPARE(result.text,QString("현물 ▲ · 선물 ▼"));
        QVERIFY(result.information.contains("USDT"));QVERIFY(result.information.contains("0.2000"));QVERIFY(!result.information.contains("CVD"));
        auto equal=RankingFuturesFeed::parseSpotFutures("BTC",ok(candles("50")),ok(candles("50")),Now);
        QVERIFY(equal.available);QCOMPARE(equal.text,QString("현물 균형 · 선물 균형"));
    }
    void spotFuturesRejectsMalformedDuplicateGapAndFuture(){
        for(const auto fault:{QString("duplicate"),QString("gap"),QString("future"),QString("zero"),QString("buy-over-total"),QString("invalid-decimal"),QString("wrong-end"),QString("missing-last")}){
            auto spot=candles(),future=candles();auto row=spot[1].toArray();
            if(fault=="duplicate")spot.append(spot[0]);
            if(fault=="gap")spot.insert(0,candle(End-4*Hour));
            if(fault=="future")spot.append(candle(End+Hour));
            if(fault=="zero"){row[7]="0";row[10]="0";spot[1]=row;}
            if(fault=="buy-over-total"){row[10]="101";spot[1]=row;}
            if(fault=="invalid-decimal"){row[7]="1e2";spot[1]=row;}
            if(fault=="wrong-end"){row[6]=QString::number(End);spot[1]=row;}
            if(fault=="missing-last")future.removeAt(1);
            QVERIFY2(!RankingFuturesFeed::parseSpotFutures("BTC",ok(spot),ok(future),Now).available,qPrintable(fault));
        }
    }
    void feedRequiresBothExactCataloguesAndKeepsFixedRequests(){
        Http catalogHttp,http;ChartInstrumentCatalog catalog(&catalogHttp);qint64 now=sameCompletedHourNow(2000);
        QCOMPARE(now/Hour,(now+2000)/Hour);
        RankingFuturesFeed feed(&http,nullptr,[&]{return now;});feed.setCatalog(&catalog);feed.setTargets({"BTC"},RankingStudy::SpotFutures);
        QCOMPARE(catalogHttp.sent.size(),1);QVERIFY(http.sent.isEmpty());catalogHttp.answer(catalogReply({"BTC"}));
        QCOMPARE(http.sent.size(),1);QVERIFY(MarketMetrics::isRequest(http.sent.last()));http.answer(catalogReply({"BTC"},true));
        now+=999;feed.pump();QCOMPARE(http.sent.size(),1);++now;feed.pump();QCOMPARE(http.sent.size(),2);
        QVERIFY(MarketCandles::isRequest(http.sent.last()));http.answer(flow(now,"60"));now+=1000;feed.pump();QCOMPARE(http.sent.size(),3);
        QVERIFY(ChartPublicContextSource::isRequest(http.sent.last()));http.answer(flow(now,"40"));
        const auto value=feed.observation("BTC");QVERIFY(value.available);QCOMPARE(value.text,QString("현물 ▲ · 선물 ▼"));
        for(const auto& request:http.sent){QVERIFY(NetworkTradingTransport::allowed(request));QVERIFY(!request.authenticated);QCOMPARE(request.headers.size(),1);QCOMPARE(request.headers.value("Accept"),QByteArray("application/json"));QVERIFY(request.body.isEmpty());QCOMPARE(request.method,QByteArray("GET"));}
        now+=RankingFuturesFeed::FreshnessMs;QVERIFY(feed.observation("BTC").text.contains("지연"));feed.clear();QVERIFY(!feed.observation("BTC").available);
    }
    void feedStopsLateRepliesAndHonorsRetryAfter(){
        Http catalogHttp,http;ChartInstrumentCatalog catalog(&catalogHttp);qint64 now=QDateTime::currentMSecsSinceEpoch()+50;
        RankingFuturesFeed feed(&http,nullptr,[&]{return now;});feed.setCatalog(&catalog);feed.setTargets({"BTC","ETH"},RankingStudy::OpenInterest);
        catalogHttp.answer(catalogReply({"BTC","ETH"}));http.answer(catalogReply({"BTC","ETH"},true));now+=1000;feed.pump();QCOMPARE(http.sent.size(),2);
        HttpResult limited;limited.status=429;limited.headers.insert("Retry-After","120");http.answer(limited);
        now+=119999;feed.pump();QCOMPARE(http.sent.size(),2);++now;feed.pump();QCOMPARE(http.sent.size(),3);
        const auto late=http.tokens.last();feed.clear();QCOMPARE(http.aborted,1);QSignalSpy changes(&feed,&RankingFuturesFeed::changed);
        emit http.finished(late,interest("ETH",now));QCOMPARE(changes.size(),0);now+=200000;feed.pump();QCOMPARE(http.sent.size(),3);
    }
    void extremeRetryAfterCannotWrapDeadline(){
        Http catalogHttp,http;ChartInstrumentCatalog catalog(&catalogHttp);qint64 now=QDateTime::currentMSecsSinceEpoch()+50;
        RankingFuturesFeed feed(&http,nullptr,[&]{return now;});feed.setCatalog(&catalog);feed.setTargets({"BTC","ETH"},RankingStudy::OpenInterest);
        catalogHttp.answer(catalogReply({"BTC","ETH"}));http.answer(catalogReply({"BTC","ETH"},true));now+=1000;feed.pump();QCOMPARE(http.sent.size(),2);
        HttpResult limited;limited.status=429;limited.headers.insert("Retry-After",QByteArray::number(std::numeric_limits<qint64>::max()/1000));http.answer(limited);
        now+=1000;feed.pump();QCOMPARE(http.sent.size(),2);now+=120000;feed.pump();QCOMPARE(http.sent.size(),2);
    }
    void feedBoundsVisibleTargetsAndReusesFreshSnapshot(){
        Http catalogHttp,http;ChartInstrumentCatalog catalog(&catalogHttp);qint64 now=sameCompletedHourNow(22000);
        const auto completedHour=now/Hour*Hour;
        QCOMPARE((now+22000)/Hour*Hour,completedHour);
        QStringList bases;for(int i=0;i<25;++i)bases.append("SYN"+QString::number(i));
        RankingFuturesFeed feed(&http,nullptr,[&]{return now;});feed.setCatalog(&catalog);feed.setTargets(bases,RankingStudy::OpenInterest);
        catalogHttp.answer(catalogReply(bases));http.answer(catalogReply(bases,true));
        for(int i=0;i<20;++i){now+=1000;feed.pump();QCOMPARE(http.sent.size(),i+2);http.answer(interest(bases[i],now));QVERIFY(feed.observation(bases[i]).available);}
        now+=1000;feed.pump();QCOMPARE(http.sent.size(),21);QVERIFY(!feed.observation(bases[20]).available);
        QCOMPARE(feed.observation(bases.first()).throughMs,completedHour);
        feed.clear();feed.setTargets({bases.first()},RankingStudy::OpenInterest);now+=1000;feed.pump();QCOMPARE(http.sent.size(),21);QVERIFY(feed.observation(bases.first()).available);
    }
    void sharedPacerAndUnsupportedContractStayBounded(){
        Http catalogHttp,a,b;ChartInstrumentCatalog catalog(&catalogHttp);qint64 now=QDateTime::currentMSecsSinceEpoch()+50;
        auto pacer=std::make_shared<ChartStudyRequestPacer>([&]{return now;});
        RankingFuturesFeed first(&a,nullptr,[&]{return now;}),second(&b,nullptr,[&]{return now;});
        first.setCatalog(&catalog);second.setCatalog(&catalog);first.setRequestPacer(pacer);second.setRequestPacer(pacer);
        first.setTargets({"BTC"},RankingStudy::OpenInterest);second.setTargets({"BTC"},RankingStudy::OpenInterest);catalogHttp.answer(catalogReply({"BTC"}));
        QCOMPARE(a.sent.size()+b.sent.size(),1);now+=1000;first.pump();second.pump();QCOMPARE(a.sent.size()+b.sent.size(),2);
        a.answer(catalogReply({"ETH"},true));b.answer(catalogReply({"ETH"},true));now+=1000;first.pump();second.pump();
        QCOMPARE(a.sent.size(),1);QCOMPARE(b.sent.size(),1);QCOMPARE(first.observation("BTC").text,QString("지원 없음"));
    }
};
QTEST_GUILESS_MAIN(RankingFuturesFeedTest)
#include "tst_ranking_futures_feed.moc"
