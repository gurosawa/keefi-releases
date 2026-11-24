#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/TradingTransport.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/Vault.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QTimeZone>
#include <QUrlQuery>
using namespace keefi;
namespace {
class Http final:public TradingTransport {
  public: QList<RequestSpec> sent;QList<quint64> tokens;int aborts=0;
    void send(quint64 token,const RequestSpec& request)override{sent.append(request);tokens.append(token);}
    void abort()override{++aborts;}
    void reply(int index,const HttpResult& response){emit finished(tokens.at(index),response);}
};
qint64 at(const QString& value){return QDateTime::fromString(value,Qt::ISODate).toMSecsSinceEpoch();}
HttpResult result(const QJsonValue& value,int status=200){HttpResult response;response.status=status;response.body=QJsonDocument(QJsonObject{{"result",value}}).toJson(QJsonDocument::Compact);return response;}
QJsonObject stock(QString country="KR",QString symbol={}){
    if(symbol.isEmpty())symbol=country=="KR"?"0101N0":"SYN";
    return {{"symbol",symbol},{"name","합성 종목"},{"englishName","Synthetic Stock"},{"isinCode",country=="KR"?"KR7000000000":"US0000000000"},
        {"market",country=="KR"?"KOSPI":"NASDAQ"},{"securityType","STOCK"},{"isCommonShare",true},{"status","ACTIVE"},{"currency",country=="KR"?"KRW":"USD"},
        {"sharesOutstanding","1000"},{"listDate",QJsonValue::Null},{"delistDate",QJsonValue::Null},{"leverageFactor",QJsonValue::Null},
        {"koreanMarketDetail",country=="KR"?QJsonValue(QJsonObject{{"liquidationTrading",false},{"nxtSupported",true},{"krxTradingSuspended",false},{"nxtTradingSuspended",false}}):QJsonValue(QJsonValue::Null)}};
}
QJsonObject marketSession(QString from,QString to){return {{"startTime",from},{"endTime",to}};}
QJsonObject sessionDay(QString country,QString date){
    if(country=="KR")return {{"date",date},{"integrated",QJsonObject{{"preMarket",QJsonValue::Null},{"regularMarket",marketSession(date+"T09:00:00+09:00",date+"T15:30:00+09:00")},{"afterMarket",marketSession(date+"T15:30:00+09:00",date+"T20:00:00+09:00")}}}};
    return {{"date",date},{"dayMarket",QJsonValue::Null},{"preMarket",QJsonValue::Null},{"regularMarket",marketSession(date+"T13:30:00Z",date+"T20:00:00Z")},{"afterMarket",QJsonValue::Null}};
}
QJsonObject calendar(QString country="KR",QString today="2026-10-23",QString previous="2026-10-22",QString next="2026-10-26"){
    return {{"today",sessionDay(country,today)},{"previousBusinessDay",sessionDay(country,previous)},{"nextBusinessDay",sessionDay(country,next)}};
}
QJsonObject candle(QString timestamp,QString currency="KRW",QString close="100",QString volume="1"){
    return {{"timestamp",timestamp},{"openPrice",close},{"highPrice",close},{"lowPrice",close},{"closePrice",close},{"volume",volume},{"currency",currency}};
}
HttpResult candles(const QJsonArray& rows,QJsonValue next=QJsonValue::Null){return result(QJsonObject{{"candles",rows},{"nextBefore",next}});}
QJsonObject side(QString buy,QString sell,QString net){return {{"buyVolume",buy},{"sellVolume",sell},{"netBuyVolume",net}};}
HttpResult auxiliary(const RequestSpec& request,qint64 now){
    const auto path=request.url.path();const auto country=path.endsWith("/US")?QString("US"):QString("KR");
    if(path=="/api/v1/stocks")return result(QJsonArray{stock(QUrlQuery(request.url).queryItemValue("symbols")=="SYN"?"US":"KR",QUrlQuery(request.url).queryItemValue("symbols"))});
    if(path.startsWith("/api/v1/market-calendar/"))return result(calendar(country));
    const auto observed=QDateTime::fromMSecsSinceEpoch(now,QTimeZone("Asia/Seoul")).toString(Qt::ISODateWithMs);
    if(path=="/api/v1/price-limits")return result(QJsonObject{{"timestamp",observed},{"upperLimitPrice","150"},{"lowerLimitPrice","50"},{"currency","KRW"}});
    if(path.endsWith("/warnings"))return result(QJsonArray{});
    if(path.endsWith("/investor-trading"))return result(QJsonObject{{"nextUntil",QJsonValue::Null},{"records",QJsonArray{QJsonObject{{"date","2026-10-23"},{"updatedAt",observed},{"individual",QJsonValue::Null},
        {"foreigner",side("15","10","5")},{"institution",QJsonObject{{"buyVolume","2"},{"sellVolume","6"},{"netBuyVolume","-4"},{"breakdown",QJsonValue::Null}}},
        {"otherCorporation",QJsonValue::Null},{"foreignerHolding",QJsonValue::Null},{"cfd",QJsonValue::Null}}}}});
    if(path.endsWith("/program-trades"))return result(QJsonObject{{"nextUntil",QJsonValue::Null},{"records",QJsonArray{QJsonObject{{"date","2026-10-23"},{"arbitrage",side("1","3","-2")},{"nonArbitrage",side("9","4","5")}}}}});
    return {};
}
bool reachCandles(StockChartSource& source,Http& http,qint64& now,QString country="KR"){
    QStringList paths{"/api/v1/stocks","/api/v1/market-calendar/"+country};
    if(country=="KR")paths.append({"/api/v1/price-limits","/api/v1/stocks/0101N0/warnings","/api/v1/stocks/0101N0/investor-trading","/api/v1/stocks/0101N0/program-trades"});
    for(int index=0;index<paths.size();++index){if(http.sent.size()!=index+1||http.sent[index].url.path()!=paths[index])return false;
        http.reply(index,auxiliary(http.sent[index],now));now+=1000;source.refresh();}
    return http.sent.size()==paths.size()+1&&http.sent.last().url.path()=="/api/v1/candles";
}
QDate syntheticBusinessDay(QDate day,int direction){
    // These dates are written explicitly into the synthetic provider response. Production must not infer this calendar.
    do{day=day.addDays(direction);}while(day.dayOfWeek()>5);return day;
}
HttpResult calendarForRequest(const RequestSpec& request){
    const auto country=request.url.path().endsWith("/US")?QString("US"):QString("KR");
    const auto text=QUrlQuery(request.url).queryItemValue("date");const auto day=text.isEmpty()?QDate(2026,10,23):QDate::fromString(text,Qt::ISODate);
    return result(calendar(country,day.toString(Qt::ISODate),syntheticBusinessDay(day,-1).toString(Qt::ISODate),syntheticBusinessDay(day,1).toString(Qt::ISODate)));
}
bool finishSupportJobs(StockChartSource& source,Http& http,qint64& now,int handled){
    for(int attempt=0;attempt<80;++attempt){
        if(!source.snapshot().loading)return true;
        now+=1000;source.refresh();if(http.sent.size()<=handled)continue;
        const auto& request=http.sent.at(handled++);
        if(request.url.path().startsWith("/api/v1/market-calendar/"))http.reply(handled-1,calendarForRequest(request));
        else if(request.url.path()=="/api/v1/candles"&&QUrlQuery(request.url).queryItemValue("interval")=="1d")http.reply(handled-1,candles({}));
        else return false;
    }return false;
}
QJsonArray dailyRows(QDate latest,int previousCount,bool zeroMedian=false){
    QJsonArray rows;rows.append(candle(latest.toString(Qt::ISODate)+"T00:00:00+09:00","KRW","100","30"));
    for(int i=0;i<previousCount;++i){latest=syntheticBusinessDay(latest,-1);rows.append(candle(latest.toString(Qt::ISODate)+"T00:00:00+09:00","KRW","100",zeroMedian?"0":i==0?"1000":"10"));}
    return rows;
}
bool sameCandles(const QList<MarketCandle>& left,const QList<MarketCandle>& right){
    if(left.size()!=right.size())return false;for(int i=0;i<left.size();++i){const auto& a=left.at(i);const auto& b=right.at(i);
        if(a.startsAtMs!=b.startsAtMs||a.lastStoredTickMs!=b.lastStoredTickMs||a.open!=b.open||a.high!=b.high||a.low!=b.low||a.close!=b.close||a.volume!=b.volume)return false;}
    return true;
}
}
class StockChartSourceTest final:public QObject {
    Q_OBJECT
  private slots:
    void authenticationFailureUsesTemporaryCopyAndSourceFailuresKeepTheirReason();
    void missingAuthorizationIsQuietAndNextPollStartsRequest();
    void changedBrokerReasonIsPublishedOnlyOnce();
    void buildersAllowOnlyDedicatedStockMarketReads();
    void constructorIsLazyAndDoesNotBorrowAuthorization();
    void unconnectedStocksRequireBrokerWithoutAnyRequest_data();
    void unconnectedStocksRequireBrokerWithoutAnyRequest();
    void revokingAuthorizationExplainsConnectionDuringRetryAfter();
    void activeSelectionBorrowsOnceAndNeverStartsAnAccountJob();
    void stoppingRevokesBeforeSynchronousLateTransportCompletion();
    void unknownIdentityCannotBorrowOrSend();
    void metadataCalendarFlowsAndCandlesFormOneVerifiedPipeline();
    void metadataMustBindExactCountryCurrencyAndArray_data();
    void metadataMustBindExactCountryCurrencyAndArray();
    void paginationKeepsFirstCutoffAndDeduplicatesInclusiveBoundary();
    void paginationRejectsConflictsAndRepeatedCursor_data();
    void paginationRejectsConflictsAndRepeatedCursor();
    void fourHourSelectionFetchesNativePagesWithoutInventingChildren();
    void responseRevisionIsReborrowedBeforePromotion();
    void unauthorizedRevisionCannotPumpUntilAuthorizationChanges();
    void retryAfterSurvivesToggleAndTimeoutDoesNotSpin();
    void intentRevokedCancelsBeforeAnyOldAuthorizationCanPump();
    void canonicalCandleSourceUrlIncludesVerifiedRequestContract();
    void sixtySecondRefreshKeepsLastGoodUntilSuccessfulReplacement();
    void expiredKnownAuthorizationCannotBeExtendedBySameRevision();
    void dailyVolumeMultipleUsesTwentyCompletedDayMedian_data();
    void dailyVolumeMultipleUsesTwentyCompletedDayMedian();
    void minuteHistoryUsesSeparateNativeDailyVolumeWithoutRenewingReceipt();
    void stockLevelDemandUsesAuthenticatedDailyCalendarAndClearsOnLock();
    void stockLevelLayerFailureLeavesCandlesAvailable();
    void turningLevelsOffCancelsSupportWithoutRestartingTheChart();
};
void StockChartSourceTest::authenticationFailureUsesTemporaryCopyAndSourceFailuresKeepTheirReason(){
    const auto deniedReason=QStringLiteral("토스증권 인증을 다시 확인해 주세요. 키움증권으로는 국내 일봉을 볼 수 있어요");
    for(const int status:{401,403,500,200}){
        qint64 now=100000;Http http;StockChartSource source(&http,nullptr,[&]{return now;});
        source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_GUIDANCE",60000,1);return true;});
        source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);
        HttpResult response;response.status=status;response.body="{}";http.reply(0,response);
        const auto expected=status==401||status==403?deniedReason:status==500?QStringLiteral("토스 주식 차트 원천 응답 실패"):QStringLiteral("토스 주식 차트 identity·캘린더·페이지 원천 검증 실패");
        QCOMPARE(source.snapshot().series.reason,expected);QSignalSpy changes(&source,&StockChartSource::changed);
        for(int i=0;i<10;++i){now+=100;source.refresh();}
        QCOMPARE(source.snapshot().series.reason,expected);QCOMPARE(changes.count(),0);QCOMPARE(http.sent.size(),1);
    }
}
void StockChartSourceTest::missingAuthorizationIsQuietAndNextPollStartsRequest(){
    qint64 now=100000;bool authorized=false;Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([&](const auto& borrow){if(!authorized)return false;borrow("SYNTHETIC_BEARER",60000,1);return true;});
    source.setSelection({"stock","KR","005930"},60000);source.setActive(true);
    QSignalSpy changes(&source,&StockChartSource::changed);
    for(int i=0;i<10;++i){now+=100;source.refresh();}
    QVERIFY2(changes.count()<=1,qPrintable(QString("idle changed count: %1").arg(changes.count())));QVERIFY(http.sent.isEmpty());
    authorized=true;now+=100;source.refresh();QCOMPARE(http.sent.size(),1);
    QCOMPARE(http.sent.first().url.path(),QString("/api/v1/stocks"));
}
void StockChartSourceTest::changedBrokerReasonIsPublishedOnlyOnce(){
    qint64 now=100000;bool authorized=false;Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([&](const auto& borrow){if(!authorized)return false;borrow("SYNTHETIC_BEARER",60000,1);return true;});
    source.setSelection({"stock","US","SYN"},60000);source.setActive(true);const auto missing=source.snapshot().series.reason;
    authorized=true;now+=100;source.refresh();QCOMPARE(http.sent.size(),1);
    QSignalSpy changes(&source,&StockChartSource::changed);HttpResult denied;denied.status=403;http.reply(0,denied);
    QCOMPARE(changes.count(),1);QVERIFY(source.snapshot().series.reason!=missing);
    now+=100;source.refresh();const auto blocked=source.snapshot().series.reason;
    for(int i=0;i<10;++i){now+=100;source.refresh();}
    QCOMPARE(changes.count(),1);QCOMPARE(source.snapshot().series.reason,blocked);QCOMPARE(http.sent.size(),1);
}
void StockChartSourceTest::buildersAllowOnlyDedicatedStockMarketReads(){
    const WatchlistIdentity identity{"stock","US","SYN"};
    const QList<QPair<StockChartRead,QString>> cases{{StockChartRead::Metadata,"/api/v1/stocks"},{StockChartRead::Calendar,"/api/v1/market-calendar/US"},
        {StockChartRead::Candles,"/api/v1/candles"},{StockChartRead::PriceLimits,"/api/v1/price-limits"},{StockChartRead::Warnings,"/api/v1/stocks/000001/warnings"},
        {StockChartRead::Investors,"/api/v1/stocks/000001/investor-trading"},{StockChartRead::Program,"/api/v1/stocks/000001/program-trades"}};
    for(const auto& [kind,path]:cases){const bool domestic=kind==StockChartRead::Warnings||kind==StockChartRead::Investors||kind==StockChartRead::Program;
        const auto request=StockChartSource::request(kind,domestic?WatchlistIdentity{"stock","KR","000001"}:identity);QVERIFY(StockChartSource::isRequest(request));
        QCOMPARE(request.url.scheme(),QString("https"));QCOMPARE(request.url.host(),QString("openapi.tossinvest.com"));QCOMPARE(request.url.path(),path);
        QCOMPARE(request.method,QByteArray("GET"));QVERIFY(request.body.isEmpty());QVERIFY(!isMutation(request.operation));
        const QUrlQuery query(request.url);QVERIFY(!query.hasQueryItem("account"));QVERIFY(!query.hasQueryItem("quantity"));
        auto malformed=request;malformed.url.setHost("synthetic.invalid");QVERIFY(!StockChartSource::isRequest(malformed));
    }
    const auto candle=StockChartSource::request(StockChartRead::Candles,identity,86400000,"2026-01-02T00:00:00-05:00");
    const QUrlQuery query(candle.url);QCOMPARE(query.queryItemValue("interval"),QString("1d"));QCOMPARE(query.queryItemValue("adjusted"),QString("true"));
    QCOMPARE(query.queryItemValue("before"),QString("2026-01-02T00:00:00-05:00"));
    QVERIFY(!StockChartSource::isRequest(StockChartSource::request(StockChartRead::Candles,identity,300000)));
}
void StockChartSourceTest::constructorIsLazyAndDoesNotBorrowAuthorization(){
    Http http;int borrowed=0;StockChartSource source(&http);source.setAuthorizationProvider([&](const auto& callback){++borrowed;callback("SYNTHETIC_BEARER",60000,1);return true;});
    source.setSelection({"stock","US","SYN"},60000);QCOMPARE(borrowed,0);QVERIFY(http.sent.isEmpty());QVERIFY(!source.active());
}
void StockChartSourceTest::unconnectedStocksRequireBrokerWithoutAnyRequest_data(){
    QTest::addColumn<QString>("country");QTest::addColumn<QString>("symbol");
    QTest::newRow("domestic-daily")<<QString("KR")<<QString("005930");
    QTest::newRow("us-daily")<<QString("US")<<QString("AAPL");
}
void StockChartSourceTest::unconnectedStocksRequireBrokerWithoutAnyRequest(){
    QFETCH(QString,country);QFETCH(QString,symbol);Http http;StockChartSource source(&http);
    source.setSelection({"stock",country,symbol},86400000);source.setActive(true);source.refresh();
    const auto snapshot=source.snapshot();
    QCOMPARE(snapshot.series.reason,QStringLiteral("연결된 증권사가 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QCOMPARE(snapshot.series.identity,QString("stock:")+country+":"+symbol);
    QCOMPARE(snapshot.source,StockSourceKind::None);QVERIFY(!snapshot.series.available);QVERIFY(!snapshot.loading);
    QVERIFY(snapshot.series.candles.isEmpty());QVERIFY(!snapshot.realtimeAllowed);QVERIFY(http.sent.isEmpty());
}
void StockChartSourceTest::activeSelectionBorrowsOnceAndNeverStartsAnAccountJob(){
    Http http;int borrowed=0;StockChartSource source(&http);source.setAuthorizationProvider([&](const auto& callback){++borrowed;callback("SYNTHETIC_BEARER",60000,1);return true;});
    source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QTRY_COMPARE(http.sent.size(),1);QCOMPARE(borrowed,1);QVERIFY(source.active());
    QCOMPARE(http.sent.first().url.path(),QString("/api/v1/stocks"));QCOMPARE(QUrlQuery(http.sent.first().url).queryItemValue("symbols"),QString("SYN"));
    QCOMPARE(http.sent.first().headers.value("Authorization"),QByteArray("Bearer SYNTHETIC_BEARER"));
    QVERIFY(StockChartSource::isRequest(http.sent.first()));QVERIFY(!http.sent.first().url.toString().contains("SYNTHETIC_BEARER"));
}
void StockChartSourceTest::revokingAuthorizationExplainsConnectionDuringRetryAfter(){
    qint64 now=at("2026-10-23T13:35:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_REVOKE_DURING_RETRY",3600000,1);return true;});
    source.setSelection({"stock","US","SYN"},86400000);source.setActive(true);QCOMPARE(http.sent.size(),1);
    HttpResult limited;limited.status=429;limited.localRetryAfterSeconds=180;http.reply(0,limited);
    source.setAuthorizationProvider({});const auto snapshot=source.snapshot();
    QCOMPARE(snapshot.series.reason,QStringLiteral("연결된 증권사가 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QVERIFY(!snapshot.series.available);QVERIFY(snapshot.series.candles.isEmpty());QVERIFY(!snapshot.loading);
    now+=60000;source.refresh();QCOMPARE(http.sent.size(),1);
}
void StockChartSourceTest::stoppingRevokesBeforeSynchronousLateTransportCompletion(){
    class LateHttp final:public TradingTransport {public:int calls=0,aborts=0;quint64 token=0;
        void send(quint64 value,const RequestSpec&)override{++calls;token=value;}
        void abort()override{++aborts;HttpResult r;r.status=200;r.body="{\"result\":{}}";emit finished(token,r);}
    } http;
    StockChartSource source(&http);source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_BEARER",60000,1);return true;});
    source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QTRY_COMPARE(http.calls,1);source.setActive(false);
    QCOMPARE(http.aborts,1);QVERIFY(!source.snapshot().series.available);QVERIFY(!source.active());QTest::qWait(20);QCOMPARE(http.calls,1);
}
void StockChartSourceTest::unknownIdentityCannotBorrowOrSend(){
    Http http;int borrowed=0;StockChartSource source(&http);source.setAuthorizationProvider([&](const auto&){++borrowed;return false;});
    source.setSelection({"stock","UNKNOWN","SYN"},60000);source.setActive(true);QTest::qWait(10);QCOMPARE(borrowed,0);QVERIFY(http.sent.isEmpty());
}
void StockChartSourceTest::metadataCalendarFlowsAndCandlesFormOneVerifiedPipeline(){
    qint64 now=at("2026-10-23T09:35:00+09:00");Http http;int borrows=0;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([&](const auto& callback){++borrows;QByteArray borrowed("SYNTHETIC_PIPELINE_BEARER");callback(borrowed,3600000,4);borrowed.fill('x');return true;});
    source.setSelection({"stock","KR","0101N0"},60000);source.setActive(true);QVERIFY2(reachCandles(source,http,now),"metadata array -> calendar -> limits/warnings/investors/program -> candles must advance");
    const auto received=now;http.reply(http.sent.size()-1,candles({candle("2026-10-23T09:34:00+09:00"),candle("2026-10-23T09:33:00+09:00"),candle("2026-10-23T09:32:00+09:00")}));
    const auto snapshot=source.snapshot();QVERIFY(snapshot.series.available);QCOMPARE(snapshot.series.candles.size(),3);QCOMPARE(snapshot.source,StockSourceKind::Toss);
    QCOMPARE(snapshot.series.identity,QString("stock:KR:0101N0"));QCOMPARE(snapshot.series.currency,QString("KRW"));QCOMPARE(snapshot.series.receivedAtMs,received);QCOMPARE(snapshot.series.sourceTimeMs,qint64(0));
    QCOMPARE(snapshot.adjustment,StockAdjustment::SourceAdjusted);QVERIFY(snapshot.realtimeAllowed);QVERIFY(snapshot.intradayAllowed);QVERIFY(!snapshot.delayed);QVERIFY(snapshot.calendarAvailable);
    QCOMPARE(snapshot.restrictions.currentVI,std::optional<bool>(false));QCOMPARE(snapshot.restrictions.historicalCoverage,StockHistoryCoverage::CurrentOnly);
    QCOMPARE(snapshot.restrictions.upperLimit,std::optional<QString>("150"));QCOMPARE(snapshot.restrictions.lowerLimit,std::optional<QString>("50"));
    QCOMPARE(snapshot.marketStudies.days.size(),1);const auto flow=snapshot.marketStudies.days.first();QCOMPARE(flow.foreignerNetBuyVolume,std::optional<QString>("5"));QCOMPARE(flow.institutionNetBuyVolume,std::optional<QString>("-4"));
    QCOMPARE(flow.arbitrageNetBuyVolume,std::optional<QString>("-2"));QCOMPARE(flow.nonArbitrageNetBuyVolume,std::optional<QString>("5"));QVERIFY(flow.provisional);
    QVERIFY(flow.investorEvidence.source.contains("KRX")&&flow.investorEvidence.source.contains("NXT"));QVERIFY(flow.programEvidence.source.contains("KRX")&&!flow.programEvidence.source.contains("NXT"));
    QVERIFY(flow.investorEvidence.sourceTimeMs>0);QCOMPARE(flow.programEvidence.sourceTimeMs,qint64(0));QVERIFY(!snapshot.marketStudies.volumeMultiple.has_value()); // Three minute bars are not twenty daily observations.
    QVERIFY(!snapshot.marketStudies.foreignerConsecutiveBuyDays.has_value());QCOMPARE(snapshot.marketStudies.institutionConsecutiveBuyDays,std::optional<int>(0));QVERIFY(!snapshot.marketStudies.flowContinuityReason.isEmpty());
    QVERIFY(borrows>=http.sent.size());for(const auto& request:http.sent){QVERIFY(StockChartSource::isRequest(request));QVERIFY(!isMutation(request.operation));QVERIFY(!request.url.toString().contains("SYNTHETIC_PIPELINE_BEARER"));QVERIFY(!request.url.path().contains("account"));}
    now+=1000;QCOMPARE(source.snapshot().series.receivedAtMs,received); // UI reads cannot renew a source receipt.
}
void StockChartSourceTest::metadataMustBindExactCountryCurrencyAndArray_data(){
    QTest::addColumn<QString>("fault");for(const auto* value:{"object-not-array","duplicate","symbol","market-country","currency","inactive","missing-currency"})QTest::newRow(value)<<QString(value);
}
void StockChartSourceTest::metadataMustBindExactCountryCurrencyAndArray(){
    QFETCH(QString,fault);qint64 now=at("2026-10-23T09:35:00+09:00");Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_META",3600000,1);return true;});
    source.setSelection({"stock","KR","0101N0"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);auto row=stock();if(fault=="symbol")row["symbol"]="OTHER";if(fault=="market-country")row["market"]="NASDAQ";if(fault=="currency")row["currency"]="USD";if(fault=="inactive")row["status"]="DELISTED";if(fault=="missing-currency")row.remove("currency");
    QJsonArray rows;rows.append(row);if(fault=="duplicate")rows.append(row);http.reply(0,fault=="object-not-array"?result(row):result(rows));now+=1000;source.refresh();
    QVERIFY(!source.snapshot().series.available);QCOMPARE(http.sent.size(),1);QVERIFY(!source.snapshot().series.reason.isEmpty());
}
void StockChartSourceTest::paginationKeepsFirstCutoffAndDeduplicatesInclusiveBoundary(){
    qint64 now=at("2026-10-23T09:30:50+09:00");Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_PAGE",3600000,1);return true;});
    source.setSelection({"stock","KR","0101N0"},60000);source.setActive(true);QVERIFY(reachCandles(source,http,now));QVERIFY(now<at("2026-10-23T09:31:00+09:00"));
    const auto cursor=QString("2026-10-23T09:30:00+09:00");http.reply(http.sent.size()-1,candles({candle("2026-10-23T09:31:00+09:00"),candle(cursor)},cursor));
    now+=10000;source.refresh();QCOMPARE(http.sent.last().url.path(),QString("/api/v1/candles"));QCOMPARE(QUrlQuery(http.sent.last().url).queryItemValue("before",QUrl::FullyDecoded),cursor);
    QVERIFY(http.sent.last().url.toString(QUrl::FullyEncoded).contains("%2B09:00"));http.reply(http.sent.size()-1,candles({candle(cursor),candle("2026-10-23T09:29:00+09:00")}));
    const auto snapshot=source.snapshot();QVERIFY(snapshot.series.available);QCOMPARE(snapshot.series.candles.size(),2);QCOMPARE(snapshot.series.candles.first().startsAtMs,at("2026-10-23T09:28:00+09:00"));
    QCOMPARE(snapshot.series.candles.last().startsAtMs,at("2026-10-23T09:29:00+09:00"));QVERIFY(!snapshot.pending.isEmpty());QCOMPARE(snapshot.pending.last().observation.sourceLabelAtMs,at("2026-10-23T09:31:00+09:00"));
}
void StockChartSourceTest::paginationRejectsConflictsAndRepeatedCursor_data(){QTest::addColumn<bool>("conflict");QTest::newRow("conflicting-overlap")<<true;QTest::newRow("repeated-cursor")<<false;}
void StockChartSourceTest::paginationRejectsConflictsAndRepeatedCursor(){
    QFETCH(bool,conflict);qint64 now=at("2026-10-23T09:35:00+09:00");Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_BAD_PAGE",3600000,1);return true;});source.setSelection({"stock","KR","0101N0"},60000);source.setActive(true);QVERIFY(reachCandles(source,http,now));
    const auto cursor=QString("2026-10-23T09:33:00+09:00");http.reply(http.sent.size()-1,candles({candle("2026-10-23T09:34:00+09:00"),candle(cursor)},cursor));now+=1000;source.refresh();const auto requests=http.sent.size();
    http.reply(requests-1,candles({candle(cursor,"KRW",conflict?"105":"100"),candle("2026-10-23T09:32:00+09:00")},conflict?QJsonValue(QJsonValue::Null):QJsonValue(cursor)));now+=1000;source.refresh();
    QVERIFY(!source.snapshot().series.available);QCOMPARE(http.sent.size(),requests);QVERIFY(!source.snapshot().series.reason.isEmpty());
}
void StockChartSourceTest::fourHourSelectionFetchesNativePagesWithoutInventingChildren(){
    qint64 now=at("2026-10-23T13:10:00+09:00");Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_FOUR_HOUR",3600000,1);return true;});source.setSelection({"stock","KR","0101N0"},14400000);source.setActive(true);QVERIFY(reachCandles(source,http,now));
    QCOMPARE(QUrlQuery(http.sent.last().url).queryItemValue("interval"),QString("1m"));QCOMPARE(QUrlQuery(http.sent.last().url).queryItemValue("count"),QString("200"));
    const auto first=at("2026-10-23T09:00:00+09:00");QJsonArray pageA,pageB;for(int i=240;i>=41;--i)pageA.append(candle(QDateTime::fromMSecsSinceEpoch(first+i*60000,QTimeZone("Asia/Seoul")).toString(Qt::ISODate)));
    const auto cursor=QDateTime::fromMSecsSinceEpoch(first+41*60000,QTimeZone("Asia/Seoul")).toString(Qt::ISODate);http.reply(http.sent.size()-1,candles(pageA,cursor));now+=1000;source.refresh();
    for(int i=41;i>=1;--i)pageB.append(candle(QDateTime::fromMSecsSinceEpoch(first+i*60000,QTimeZone("Asia/Seoul")).toString(Qt::ISODate)));http.reply(http.sent.size()-1,candles(pageB));
    const auto snapshot=source.snapshot();QVERIFY(snapshot.series.available);QCOMPARE(snapshot.series.intervalMs,qint64(14400000));QCOMPARE(snapshot.series.candles.size(),1);QCOMPARE(snapshot.series.candles.first().startsAtMs,first);QCOMPARE(snapshot.series.candles.first().volume,QString("240"));
}
void StockChartSourceTest::responseRevisionIsReborrowedBeforePromotion(){
    qint64 now=at("2026-10-23T13:35:00Z");quint64 revision=1;Http http;int borrows=0;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([&](const auto& callback){++borrows;callback(revision==1?QByteArrayView("SYNTHETIC_OLD"):QByteArrayView("SYNTHETIC_NEW"),3600000,revision);return true;});source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);
    revision=2;http.reply(0,result(QJsonArray{stock("US")}));now+=1000;source.refresh();QVERIFY(!source.snapshot().series.available);QCOMPARE(http.sent.size(),2);QCOMPARE(http.sent.last().url.path(),QString("/api/v1/stocks"));QCOMPARE(http.sent.last().headers.value("Authorization"),QByteArray("Bearer SYNTHETIC_NEW"));QVERIFY(borrows>=3);
}
void StockChartSourceTest::unauthorizedRevisionCannotPumpUntilAuthorizationChanges(){
    qint64 now=at("2026-10-23T13:35:00Z");quint64 revision=1;Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([&](const auto& callback){callback("SYNTHETIC_401",3600000,revision);return true;});source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);HttpResult denied;denied.status=401;http.reply(0,denied);
    now+=60000;source.refresh();QCOMPARE(http.sent.size(),1);source.setActive(false);source.setActive(true);QCOMPARE(http.sent.size(),1);revision=2;source.refresh();QCOMPARE(http.sent.size(),2);QCOMPARE(http.sent.last().url.path(),QString("/api/v1/stocks"));
}
void StockChartSourceTest::retryAfterSurvivesToggleAndTimeoutDoesNotSpin(){
    qint64 now=at("2026-10-23T13:35:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_429",3600000,1);return true;});source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);
    HttpResult limited;limited.status=429;const auto retryAt=now+180000;limited.headers.insert("retry-after",QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(retryAt,QTimeZone("UTC")),"ddd, dd MMM yyyy HH:mm:ss 'GMT'").toLatin1());http.reply(0,limited);
    source.setActive(false);source.setActive(true);now=retryAt-1;source.refresh();QCOMPARE(http.sent.size(),1);now=retryAt;source.refresh();QCOMPARE(http.sent.size(),2);
    now+=10001;source.refresh();QCOMPARE(http.aborts,1);const auto count=http.sent.size();for(int i=0;i<4;++i){now+=1000;source.refresh();}QCOMPARE(http.sent.size(),count);QVERIFY(!source.snapshot().series.reason.isEmpty());
}
void StockChartSourceTest::intentRevokedCancelsBeforeAnyOldAuthorizationCanPump(){
    QTemporaryDir directory;Vault vault(directory.filePath("synthetic-stock-source-never-opened"));BrokerTradingController broker(vault);qint64 now=at("2026-10-23T13:35:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});source.setBroker(&broker);
    source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_REVOKED_BORROW",3600000,1);return true;});source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);now+=1000;emit broker.intentRevoked();QCOMPARE(http.aborts,1);QCOMPARE(http.sent.size(),1);
    now+=60000;source.refresh();QCOMPARE(http.sent.size(),1);QVERIFY(!source.snapshot().series.available);source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_FRESH_BORROW",3600000,2);return true;});source.refresh();QCOMPARE(http.sent.size(),2);QCOMPARE(http.sent.last().headers.value("Authorization"),QByteArray("Bearer SYNTHETIC_FRESH_BORROW"));
}
void StockChartSourceTest::canonicalCandleSourceUrlIncludesVerifiedRequestContract(){
    qint64 now=at("2026-10-23T13:35:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_CANONICAL",3600000,1);return true;});
    const WatchlistIdentity identity{"stock","US","SYN"};source.setSelection(identity,60000);source.setActive(true);QVERIFY(reachCandles(source,http,now,"US"));
    http.reply(http.sent.size()-1,candles({candle("2026-10-23T13:34:00Z","USD")}));
    QVERIFY(source.snapshot().series.available);QCOMPARE(source.snapshot().series.sourceUrl,StockChartSource::request(StockChartRead::Candles,identity,60000).url.toString());
}
void StockChartSourceTest::sixtySecondRefreshKeepsLastGoodUntilSuccessfulReplacement(){
    qint64 now=at("2026-10-23T13:35:00Z");quint64 revision=1;Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([&](const auto& callback){callback("SYNTHETIC_REFRESH",3600000,revision);return true;});
    source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QVERIFY(reachCandles(source,http,now,"US"));
    const auto receipt=now;http.reply(http.sent.size()-1,candles({candle("2026-10-23T13:34:00Z","USD")}));QVERIFY(finishSupportJobs(source,http,now,http.sent.size()));
    const auto previous=source.snapshot();QVERIFY(previous.series.available);QCOMPARE(previous.series.receivedAtMs,receipt);const auto requests=http.sent.size();
    now+=60000;source.refresh();QCOMPARE(http.sent.size(),requests+1);QCOMPARE(http.sent.last().url.path(),QString("/api/v1/stocks"));
    const auto loading=source.snapshot();QVERIFY(loading.loading);QVERIFY(loading.series.available);QVERIFY(sameCandles(loading.series.candles,previous.series.candles));QCOMPARE(loading.series.receivedAtMs,receipt);
    http.reply(http.sent.size()-1,result(QJsonObject{{"not","the official metadata array"}}));
    const auto rejected=source.snapshot();QVERIFY(!rejected.loading);QVERIFY(rejected.series.available);QVERIFY(sameCandles(rejected.series.candles,previous.series.candles));QCOMPARE(rejected.series.receivedAtMs,receipt);QVERIFY(!rejected.series.reason.isEmpty());
    now=receipt+90001;QVERIFY(!source.snapshot().series.available);QCOMPARE(source.snapshot().series.receivedAtMs,receipt);
    revision=2;now+=30000;source.refresh();QVERIFY(!source.snapshot().series.available);QVERIFY(source.snapshot().series.candles.isEmpty());
}
void StockChartSourceTest::expiredKnownAuthorizationCannotBeExtendedBySameRevision(){
    qint64 now=at("2026-10-23T13:35:00Z");quint64 revision=1;Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([&](const auto& callback){callback("SYNTHETIC_EXPIRY",1000,revision);return true;});
    source.setSelection({"stock","US","SYN"},60000);source.setActive(true);QCOMPARE(http.sent.size(),1);now+=1001;http.reply(0,result(QJsonArray{stock("US")}));
    QVERIFY(!source.snapshot().series.available);QVERIFY(!source.snapshot().loading);now+=1000;source.refresh();QCOMPARE(http.sent.size(),1);
    revision=2;source.refresh();QCOMPARE(http.sent.size(),2);QCOMPARE(http.sent.last().url.path(),QString("/api/v1/stocks"));
}
void StockChartSourceTest::dailyVolumeMultipleUsesTwentyCompletedDayMedian_data(){
    QTest::addColumn<int>("previousCount");QTest::addColumn<bool>("zeroMedian");QTest::addColumn<bool>("available");
    QTest::newRow("median-not-mean")<<20<<false<<true;QTest::newRow("zero-median")<<20<<true<<false;QTest::newRow("nineteen-completed-days")<<19<<false<<false;
}
void StockChartSourceTest::dailyVolumeMultipleUsesTwentyCompletedDayMedian(){
    QFETCH(int,previousCount);QFETCH(bool,zeroMedian);QFETCH(bool,available);qint64 now=at("2026-10-23T21:00:00+09:00");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_DAILY_MEDIAN",3600000,1);return true;});
    source.setSelection({"stock","KR","0101N0"},86400000);source.setActive(true);QVERIFY(reachCandles(source,http,now));QCOMPARE(QUrlQuery(http.sent.last().url).queryItemValue("interval"),QString("1d"));
    const auto receipt=now;http.reply(http.sent.size()-1,candles(dailyRows(QDate(2026,10,23),previousCount,zeroMedian)));QVERIFY(finishSupportJobs(source,http,now,http.sent.size()));
    const auto snapshot=source.snapshot();QVERIFY(snapshot.series.available);QVERIFY(!snapshot.loading);QCOMPARE(snapshot.marketStudies.volumeSamples,previousCount);QCOMPARE(snapshot.marketStudies.volumeMultiple.has_value(),available);
    if(available)QCOMPARE(*snapshot.marketStudies.volumeMultiple,3.0);else QVERIFY(!snapshot.marketStudies.reason.isEmpty());
    QVERIFY(snapshot.marketStudies.volumeScope.contains("20")&&snapshot.marketStudies.volumeScope.contains("중앙값"));QCOMPARE(snapshot.marketStudies.volumeEvidence.receivedAtMs,receipt);QCOMPARE(snapshot.marketStudies.volumeEvidence.sourceTimeMs,qint64(0));
    QCOMPARE(snapshot.series.receivedAtMs,receipt);now+=1000;QCOMPARE(source.snapshot().marketStudies.volumeEvidence.receivedAtMs,receipt);
}
void StockChartSourceTest::minuteHistoryUsesSeparateNativeDailyVolumeWithoutRenewingReceipt(){
    qint64 now=at("2026-10-23T09:35:00+09:00");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& callback){callback("SYNTHETIC_SEPARATE_DAILY",3600000,1);return true;});
    const WatchlistIdentity identity{"stock","KR","0101N0"};source.setSelection(identity,60000);source.setActive(true);QVERIFY(reachCandles(source,http,now));
    const auto primaryReceipt=now;http.reply(http.sent.size()-1,candles({candle("2026-10-23T09:34:00+09:00"),candle("2026-10-23T09:33:00+09:00"),candle("2026-10-23T09:32:00+09:00")}));
    const auto primary=source.snapshot();QVERIFY(primary.series.available);const auto requests=http.sent.size();now+=1000;source.refresh();QCOMPARE(http.sent.size(),requests+1);
    QCOMPARE(http.sent.last().url.path(),QString("/api/v1/candles"));QCOMPARE(QUrlQuery(http.sent.last().url).queryItemValue("interval"),QString("1d"));QVERIFY(source.snapshot().loading);
    const auto dailyReceipt=now;http.reply(http.sent.size()-1,candles(dailyRows(QDate(2026,10,22),20)));QVERIFY(finishSupportJobs(source,http,now,http.sent.size()));
    const auto snapshot=source.snapshot();QVERIFY(snapshot.series.available);QVERIFY(!snapshot.loading);QCOMPARE(snapshot.series.intervalMs,qint64(60000));QVERIFY(sameCandles(snapshot.series.candles,primary.series.candles));QCOMPARE(snapshot.series.receivedAtMs,primaryReceipt);
    QVERIFY(snapshot.marketStudies.volumeMultiple.has_value());QCOMPARE(*snapshot.marketStudies.volumeMultiple,3.0);QCOMPARE(snapshot.marketStudies.volumeSamples,20);QCOMPARE(snapshot.marketStudies.volumeEvidence.receivedAtMs,dailyReceipt);QCOMPARE(snapshot.marketStudies.volumeEvidence.sourceTimeMs,qint64(0));
    QCOMPARE(snapshot.marketStudies.volumeEvidence.url,StockChartSource::request(StockChartRead::Candles,identity,86400000).url.toString());
}
namespace {
QJsonObject levelSession(QDate date) {
    const auto text=date.toString(Qt::ISODate);auto day=sessionDay("US",text);
    day["regularMarket"]=date.dayOfWeek()>5?QJsonValue(QJsonValue::Null):QJsonValue(marketSession(
        QDateTime(date,QTime(9,30),QTimeZone("America/New_York")).toString(Qt::ISODate),
        QDateTime(date,QTime(16,0),QTimeZone("America/New_York")).toString(Qt::ISODate)));
    return day;
}
HttpResult levelCalendar(const RequestSpec& request) {
    const auto text=QUrlQuery(request.url).queryItemValue("date");const auto day=text.isEmpty()?QDate(2026,11,3):QDate::fromString(text,Qt::ISODate);
    return result(QJsonObject{{"today",levelSession(day)},{"previousBusinessDay",levelSession(syntheticBusinessDay(day,-1))},{"nextBusinessDay",levelSession(syntheticBusinessDay(day,1))}});
}
HttpResult levelDaily() {
    QJsonArray rows;for(auto day=QDate(2026,11,2);day>=QDate(2026,9,29);day=day.addDays(-1))if(day.dayOfWeek()<=5)
        rows.append(candle(QDateTime(day,QTime(0,0),QTimeZone("America/New_York")).toString(Qt::ISODate),"USD",QString::number(100+day.day())));
    return candles(rows);
}
}
void StockChartSourceTest::stockLevelDemandUsesAuthenticatedDailyCalendarAndClearsOnLock() {
    qint64 now=at("2026-11-03T15:00:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_LEVEL_BEARER",3600000,1);return true;});
    source.setLevelDemand(true);source.setSelection({"stock","US","SYN"},60000);source.setActive(true);
    http.reply(0,result(QJsonArray{stock("US")}));now+=999;source.refresh();QCOMPARE(http.sent.size(),1);
    ++now;source.refresh();QCOMPARE(http.sent.size(),2);
    int handled=1;bool firstCandleBeforeLevels=false;bool requestedDaily=false;bool requestedPriorMonth=false;QSet<QString> provenCalendarDates;
    for(int attempt=0;attempt<180&&source.snapshot().loading;++attempt) {
        if(http.sent.size()<=handled){now+=1000;source.refresh();continue;}
        const auto req=http.sent.at(handled);QVERIFY(StockChartSource::isRequest(req));QVERIFY(req.headers.value("Authorization").startsWith("Bearer "));
        if(req.url.path().startsWith("/api/v1/market-calendar/")) {
            const auto requestedDate=QUrlQuery(req.url).queryItemValue("date");
            if(!requestedDate.isEmpty())QVERIFY(!provenCalendarDates.contains(requestedDate));
            const auto day=requestedDate.isEmpty()?QDate(2026,11,3):QDate::fromString(requestedDate,Qt::ISODate);
            for(const auto known:{day,syntheticBusinessDay(day,-1),syntheticBusinessDay(day,1)})provenCalendarDates.insert(known.toString(Qt::ISODate));
            if(requestedDate.startsWith("2026-10-"))requestedPriorMonth=true;
            http.reply(handled++,levelCalendar(req));
        } else if(QUrlQuery(req.url).queryItemValue("interval")=="1d") {requestedDaily=true;http.reply(handled++,levelDaily());}
        else {http.reply(handled++,candles({candle("2026-11-03T14:31:00Z","USD","120")}));firstCandleBeforeLevels=source.snapshot().series.available&&source.snapshot().loading;}
        now+=1000;source.refresh();
    }
    const auto value=source.snapshot();QVERIFY(firstCandleBeforeLevels);QVERIFY(requestedDaily);QVERIFY(requestedPriorMonth);QVERIFY(!value.loading);QVERIFY(value.series.available);
    QSet<ExtendedLevelPeriod> periods;for(const auto& row:value.levelCandidates)periods.insert(row.period);
    QVERIFY(periods.contains(ExtendedLevelPeriod::Day));QVERIFY(periods.contains(ExtendedLevelPeriod::Week));QVERIFY(periods.contains(ExtendedLevelPeriod::Month));QVERIFY(periods.contains(ExtendedLevelPeriod::EarlyWeek));QVERIFY(periods.contains(ExtendedLevelPeriod::OpeningGap));
    source.setSelection({"stock","US","OTHER"},60000);handled=http.sent.size();source.setSelection({"stock","US","SYN"},60000);
    for(int attempt=0;attempt<20;++attempt){
        now+=1000;source.refresh();if(http.sent.size()<=handled){if(!source.snapshot().loading&&source.snapshot().series.available)break;continue;}
        const auto req=http.sent.at(handled++);
        if(req.url.path()=="/api/v1/stocks")http.reply(handled-1,result(QJsonArray{stock("US")}));
        else if(req.url.path().startsWith("/api/v1/market-calendar/")){QVERIFY(QUrlQuery(req.url).queryItemValue("date").isEmpty());http.reply(handled-1,levelCalendar(req));}
        else {QCOMPARE(QUrlQuery(req.url).queryItemValue("interval"),QString("1m"));http.reply(handled-1,candles({candle("2026-11-03T14:31:00Z","USD","120")}));}
    }
    QVERIFY(!source.snapshot().levelCandidates.isEmpty());source.setLevelDemand(false);QVERIFY(source.snapshot().levelCandidates.isEmpty());
    source.setActive(false);QVERIFY(source.snapshot().levelCandidates.isEmpty());
    if(!http.tokens.isEmpty())http.reply(http.tokens.size()-1,levelDaily());QVERIFY(source.snapshot().levelCandidates.isEmpty());
}
void StockChartSourceTest::stockLevelLayerFailureLeavesCandlesAvailable() {
    qint64 now=at("2026-11-03T15:00:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_LAYER_FAILURE",3600000,1);return true;});
    source.setLevelDemand(true);source.setSelection({"stock","US","SYN"},60000);source.setActive(true);int handled=0;bool failed=false;
    for(int attempt=0;attempt<20&&!failed;++attempt) {
        if(http.sent.size()<=handled){now+=1000;source.refresh();continue;}
        const auto req=http.sent.at(handled);
        if(req.url.path()=="/api/v1/stocks")http.reply(handled++,result(QJsonArray{stock("US")}));
        else if(req.url.path().startsWith("/api/v1/market-calendar/"))http.reply(handled++,levelCalendar(req));
        else if(QUrlQuery(req.url).queryItemValue("interval")=="1d"){
            const auto count=http.sent.size();HttpResult limited;limited.status=429;limited.headers.insert("Retry-After","10");http.reply(handled++,limited);
            QVERIFY(source.snapshot().series.available);now+=9999;source.refresh();QCOMPARE(http.sent.size(),count);++now;source.refresh();QCOMPARE(http.sent.size(),count+1);
            HttpResult error;error.status=500;http.reply(handled++,error);failed=true;}
        else http.reply(handled++,candles({candle("2026-11-03T14:31:00Z","USD","120")}));
        now+=1000;source.refresh();
    }
    QVERIFY(failed);QVERIFY(source.snapshot().series.available);QVERIFY(source.snapshot().levelCandidates.isEmpty());QVERIFY(source.snapshot().levelsReason.contains("받지 못"));
}
void StockChartSourceTest::turningLevelsOffCancelsSupportWithoutRestartingTheChart(){
    qint64 now=at("2026-11-03T15:00:00Z");Http http;StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_OFF",3600000,1);return true;});
    source.setLevelDemand(true);source.setSelection({"stock","US","SYN"},60000);source.setActive(true);int handled=0;bool support=false;
    for(int attempt=0;attempt<30&&!support;++attempt){
        if(http.sent.size()<=handled){now+=1000;source.refresh();continue;}
        const auto req=http.sent.at(handled);
        if(req.url.path()=="/api/v1/stocks")http.reply(handled++,result(QJsonArray{stock("US")}));
        else if(req.url.path().startsWith("/api/v1/market-calendar/")){
            if(!QUrlQuery(req.url).queryItemValue("date").isEmpty()){support=true;break;}http.reply(handled++,levelCalendar(req));
        }else if(QUrlQuery(req.url).queryItemValue("interval")=="1d")http.reply(handled++,levelDaily());
        else http.reply(handled++,candles({candle("2026-11-03T14:31:00Z","USD","120")}));
        now+=1000;source.refresh();
    }
    QVERIFY(support);QVERIFY(source.snapshot().series.available);const auto count=http.sent.size();const auto aborts=http.aborts;
    source.setLevelDemand(false);QCOMPARE(http.aborts,aborts+1);QVERIFY(source.snapshot().series.available);QVERIFY(!source.snapshot().loading);QVERIFY(source.snapshot().levelCandidates.isEmpty());
    now+=1000;source.refresh();QCOMPARE(http.sent.size(),count);
    http.reply(handled,levelCalendar(http.sent[handled]));QVERIFY(source.snapshot().levelCandidates.isEmpty());
}
QTEST_GUILESS_MAIN(StockChartSourceTest)
#include "tst_stock_chart_source.moc"
