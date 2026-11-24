// C11 Phase A synthetic behavior assertions; Root owns execution.
// Temporary synthetic Vault/credentials and fake HTTP/socket only. No user profile or real network.
#include "keefi/KiwoomMarketStream.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/KiwoomRequestBudget.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/KiwoomAdapter.h"
#include "keefi/TossAdapter.h"
#include "fixtures/TossResponses.h"
#include <QScopeGuard>
#include "keefi/TradingTransport.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/Vault.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>
using namespace keefi;
namespace {
QString syntheticCode(int n) { return QStringLiteral("SYN")+QChar('A'+n/676%26)+QChar('A'+n/26%26)+QChar('A'+n%26); }
QByteArray header(const RequestSpec& r,const QByteArray& name) {
    for(auto it=r.headers.cbegin();it!=r.headers.cend();++it)if(it.key().compare(name,Qt::CaseInsensitive)==0)return it.value();return {};
}
HttpResult publicResponse(QJsonObject body,const QByteArray& api={}) {
    body["return_code"]=0;HttpResult r;r.status=200;r.headers={{"cont-yn","N"}};if(!api.isEmpty())r.headers["api-id"]=api;
    r.body=QJsonDocument(body).toJson(QJsonDocument::Compact);return r;
}
QJsonObject usMetadataRow(QString symbol="SYNTH",QString exchange="ND") {
    return {{"stex_tp",exchange},{"stk_cd",symbol},{"stk_nm","합성 공개 종목"},{"stk_enm","SYNTHETIC PUBLIC"}};
}
HttpResult usPrice(QString symbol="SYNTH",QString exchange="ND",QString price="+73.2500") {
    return publicResponse({{"stex_tp",exchange},{"stk_cd",symbol},{"curr_unit","USD"},{"cur_prc",price},{"flu_rt","-1.25"}},"usa20100");
}
QByteArray usReal(QString symbol="SYNTH",QString exchange="ND",QString price="73.5000") {
    return QJsonDocument(QJsonObject{{"trnm","REAL"},{"data",QJsonArray{QJsonObject{{"type","FE"},{"item",symbol},{"stexTp",exchange},
        {"values",QJsonObject{{"10",price},{"12","-1.25"},{"20","103015"},{"22","20261023"},{"51020","213015"}}}}}}}).toJson(QJsonDocument::Compact);
}
KiwoomVerifiedInstrument verified(QString country,QString code,QString exchange,qint64 at) {
    KiwoomVerifiedInstrument i;i.identity={"stock",country,code};i.exchange=exchange;i.currency=country=="US"?"USD":"KRW";i.verified=true;
    i.metadataApiId=country=="US"?"usa10098":"ka10100";i.currencyApiId=country=="US"?"usa20100":"ka10001";
    i.metadataEvidence={"Kiwoom public metadata",country=="US"?"https://api.kiwoom.com/api/us/stkinfo":"https://api.kiwoom.com/api/dostk/stkinfo",{},0,at,true};
    i.currencyEvidence={"Kiwoom documented price unit",country=="US"?"https://api.kiwoom.com/api/us/mrkcond":"https://api.kiwoom.com/api/dostk/stkinfo",{},0,at,true};return i;
}
class FakeBrokerHttp final:public TradingTransport {
  public:
    std::function<qint64()> utc;
    QList<RequestSpec> requests;
    void abort() override {}
    void send(quint64 token,const RequestSpec& r) override {
        requests.append(r);HttpResult h;
        if(r.operation==Operation::Authenticate)h=publicResponse({{"token","SYNTHETIC_KIWOOM_TOKEN"},{"token_type","Bearer"},
            {"expires_dt",QDateTime::fromMSecsSinceEpoch(utc()+3600000,QTimeZone("Asia/Seoul")).toString("yyyyMMddHHmmss")}});
        else if(r.operation==Operation::Accounts)h=publicResponse({{"acctNo","9191919191"}}); // Invented fixture account, never exported.
        else{h.status=400;h.body=R"({"return_code":9999})";}
        QTimer::singleShot(0,this,[this,token,h]{emit finished(token,h);});
    }
    int authCount() const {return std::count_if(requests.cbegin(),requests.cend(),[](const auto& r){return r.operation==Operation::Authenticate;});}
};
class FakeMarketHttp final:public TradingTransport {
  public:
    QList<RequestSpec> requests;QList<quint64> tokens;int aborts=0;bool automatic=false;bool ambiguous=false;std::function<void()> onAbort;
    void abort() override {++aborts;if(onAbort)onAbort();}
    void send(quint64 token,const RequestSpec& r) override {
        tokens.append(token);requests.append(r);if(!automatic)return;const auto api=header(r,"api-id");const auto body=QJsonDocument::fromJson(r.body).object();HttpResult h;
        if(api=="usa10098"||api=="usa10099"){
            QJsonArray rows;rows.append(usMetadataRow(body.value("stk_cd").toString("SYNTH")));if(ambiguous)rows.append(usMetadataRow(body.value("stk_cd").toString("SYNTH"),"NY"));h=publicResponse({{"list",rows}},api);
        }else if(api=="usa20100")h=usPrice(body.value("stk_cd").toString("SYNTH"),body.value("stex_tp").toString("ND"));
        else if(api=="ka10100")h=publicResponse({{"code",body.value("stk_cd")},{"name","합성 국내 종목"},{"marketCode","0"},{"nxtEnable","N"}},api);
        else if(api=="ka10099")h=publicResponse({{"list",QJsonArray{QJsonObject{{"code","0101N0"},{"name","합성 국내 종목"},{"marketCode","0"},{"nxtEnable","N"}}}}},api);
        else if(api=="ka10001")h=publicResponse({{"stk_cd",body.value("stk_cd")},{"cur_prc","+12345"}},api);
        else{h.status=400;h.body=R"({"return_code":9999})";}
        QTimer::singleShot(0,this,[this,token,h]{emit finished(token,h);});
    }
};
class FakeSocket final:public KiwoomMarketSocket {
  public:
    struct Open {quint64 generation;QString country;};struct Sent {quint64 generation;QByteArray bytes;};
    QList<Open> opens;QList<Sent> controls;QList<quint64> logins,closes;QSet<quint64> live;int maxLive=0,aborts=0;bool exactBorrow=false;
    void open(quint64 g,const QString& country) override {opens.append({g,country});live.insert(g);maxLive=qMax(maxLive,int(live.size()));}
    void sendLogin(quint64 g,QByteArrayView token) override {logins.append(g);exactBorrow=token==QByteArrayView("SYNTHETIC_KIWOOM_TOKEN");}
    void sendControl(quint64 g,const QByteArray& bytes) override {controls.append({g,bytes});}
    void close(quint64 g) override {closes.append(g);} // Completion is explicit so overlap can be tested.
    void abort() override {++aborts;live.clear();}
    void finishClose(quint64 g) {live.remove(g);emit closed(g);}
    void frame(quint64 g,const QByteArray& bytes) {emit received(g,bytes);}
};
struct Harness {
    qint64 monotonic=0,utc=1792710000000LL;QTemporaryDir directory;Vault vault;FakeBrokerHttp brokerHttp;FakeMarketHttp marketHttp;
    std::unique_ptr<BrokerTradingController> broker;std::shared_ptr<KiwoomRequestBudget> budget;bool initialized=false;
    Harness():vault(directory.filePath("synthetic-only-vault")) {
        brokerHttp.utc=[this]{return utc;};auto limits=std::make_shared<TradingRequestLimits>([this]{return monotonic;});budget=std::make_shared<KiwoomRequestBudget>([this]{return monotonic;},limits);
        if(!directory.isValid()||!vault.create("Synthetic C11 vault password 2026!"))return;
        AccountRecord a;a.provider="kiwoom";a.alias="SYNTHETIC ONLY";a.market="domestic_equity";a.environment="live";a.currency="KRW";
        if(!vault.addAccount(a,{{"app_key","SYNTHETIC_APP_KEY"},{"app_secret","SYNTHETIC_APP_SECRET"}},StorageMode::EncryptedVault))return;
        broker=std::make_unique<BrokerTradingController>(vault,&brokerHttp,nullptr,[this](const AccountRecord&){
            return std::make_unique<KiwoomAdapter>("live",[this]{return QDateTime::fromMSecsSinceEpoch(utc,QTimeZone::UTC);});},budget);
        broker->setAutomaticRefresh(false);if(!broker->selectConnection(vault.accounts().first().id))return;initialized=true;broker->authenticate();
    }
    bool existingSessionReady() {
        // Fake shared time advances while the explicit synthetic Broker precondition completes.
        monotonic+=400;utc+=400;QCoreApplication::processEvents();return broker&&broker->authenticated()&&!broker->busy();
    }
    std::unique_ptr<KiwoomMarketStream> source(std::unique_ptr<FakeSocket> socket) {
        return std::make_unique<KiwoomMarketStream>(broker.get(),&marketHttp,std::move(socket),[this]{return monotonic;},[this]{return utc;},nullptr,budget);
    }
    void advance(KiwoomMarketStream& source,qint64 delta=400) {monotonic+=delta;utc+=delta;source.tick();QCoreApplication::processEvents();}
    void pump(KiwoomMarketStream& source,int count=32) {for(int n=0;n<count;++n)advance(source);}
};
}
class KiwoomMarketStreamTest final:public QObject {
    Q_OBJECT
  private slots:
    void missingExistingSessionIsPassive();
    void dailyTargetReadsKa10081WithTheBorrowedSessionOnly();
    void kiwoomBrokerFeedsOnlyCompletedKrDailyBarsToTheChartSource();
    void failedDailyRefreshAndRejectedAuthorizationReplaceOldBars();
    void productionMetadataToUsdProofPopulatesReadonlyInstruments();
    void exactMetadataWithAmbiguousUsExchangeDoesNotPickFirst();
    void currentLoginAndRegistrationAcksThenRestReconcileGateRealtime();
    void visibleChartHoldingsPriorityUsesOne200SymbolBudget();
    void countrySwitchClosesOldBeforeNewAndRejectsLateFrames();
    void metadataExpirationRevokesOldProofWithoutGetterFreshening();
    void knownAuthorizationDeadlineCannotBeResetByCountryToggle();
    void stopRejectsLateRestFramesAndDoesNotCreateBrokerJobs();
    void repeatedStopDoesNotQueueAnotherChangedNotification();
    void repeatedStopAfterPendingReadKeepsRevocationAndCanStopNewIntent();
    void hiddenUnbindDoesNotResetKnownExpiredRevision();
    void equalRevisionNumbersRemainSeparatedByExistingOwner();
    void authenticatedTossSessionCannotOpenOrAuthorizeKiwoom();
    void abortKeepsLeaseUntilPhysicalAbortReturns_data();
    void abortKeepsLeaseUntilPhysicalAbortReturns();
    void registrationReplacementHidesOldWsUntilNewRestReconciliation();
    void pingPrivateEventsAndReplayedPayloadPreservePublicProvenance();
    void revokedIntentCannotReopenTheStillValidSameRevision();
};
void KiwoomMarketStreamTest::dailyTargetReadsKa10081WithTheBorrowedSessionOnly() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);const int brokerReads=h.brokerHttp.requests.size();
    auto socket=std::make_unique<FakeSocket>();auto source=h.source(std::move(socket));const WatchlistIdentity identity{"stock","KR","005930"};
    QVERIFY(source->setVerifiedInstruments({verified("KR","005930","KRX",h.utc)}));source->setTargets({{identity,"KRX",1,false}});source->setActiveCountry("KR");
    QVERIFY(!source->daily(identity).available);source->setDailyTarget(identity);h.advance(*source);
    const auto daily=std::find_if(h.marketHttp.requests.cbegin(),h.marketHttp.requests.cend(),[](const auto& r){return header(r,"api-id")=="ka10081";});
    QVERIFY(daily!=h.marketHttp.requests.cend());QVERIFY(KiwoomMarketProtocol::isRequest(*daily));QCOMPARE(daily->url,QUrl("https://api.kiwoom.com/api/dostk/chart"));
    QCOMPARE(header(*daily,"authorization"),QByteArray("Bearer SYNTHETIC_KIWOOM_TOKEN"));
    const auto body=QJsonDocument::fromJson(daily->body).object();QCOMPARE(body.value("stk_cd").toString(),QString("005930"));QCOMPARE(body.value("upd_stkpc_tp").toString(),QString("1"));
    const auto base=QDateTime::fromMSecsSinceEpoch(h.utc,QTimeZone("Asia/Seoul")).date();QCOMPARE(body.value("base_dt").toString(),base.toString("yyyyMMdd"));
    QJsonArray rows{QJsonObject{{"dt",base.toString("yyyyMMdd")},{"open_pric","+70000"},{"high_pric","+71500"},{"low_pric","-69800"},{"cur_prc","-70100"},{"trde_qty","1234567"}},
                    QJsonObject{{"dt",base.addDays(-1).toString("yyyyMMdd")},{"open_pric","69000"},{"high_pric","70500"},{"low_pric","68900"},{"cur_prc","70000"},{"trde_qty","987654"}}};
    const auto token=h.marketHttp.tokens.at(daily-h.marketHttp.requests.cbegin());emit h.marketHttp.finished(token,publicResponse({{"stk_cd","005930"},{"stk_dt_pole_chart_qry",rows}},"ka10081"));h.advance(*source);
    const auto series=source->daily(identity);QVERIFY(series.available);QCOMPARE(series.rows.size(),2);QCOMPARE(series.rows.first().date,base.addDays(-1));
    QCOMPARE(series.rows.last().close,QString("70100"));QCOMPARE(series.rows.last().low,QString("69800"));QCOMPARE(series.rows.last().volume,QString("1234567"));
    QVERIFY(series.receivedAtMs>0&&series.requestStartedAtMs>0&&series.requestStartedAtMs<=series.receivedAtMs);
    QVERIFY(!source->daily({"stock","KR","000660"}).available);source->setDailyTarget({});QVERIFY(!source->daily(identity).available);
    QCOMPARE(h.brokerHttp.requests.size(),brokerReads); // Borrowed session only: no new authentication or account read.
}
void KiwoomMarketStreamTest::kiwoomBrokerFeedsOnlyCompletedKrDailyBarsToTheChartSource() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);
    auto stream=h.source(std::make_unique<FakeSocket>());const WatchlistIdentity identity{"stock","KR","005930"};
    QVERIFY(stream->setVerifiedInstruments({verified("KR","005930","KRX",h.utc)}));stream->setTargets({{identity,"KRX",1,false}});stream->setActiveCountry("KR");
    StockChartSource chart(nullptr,nullptr,[&h]{return h.utc;});chart.setKiwoomDailyStream(stream.get());
    chart.setSelection(identity,86400000);chart.setConnectionState(StockChartConnectionState::OtherBroker);chart.setActive(true);
    QCOMPARE(chart.snapshot().source,StockSourceKind::KiwoomDaily);QVERIFY(!chart.snapshot().series.available);
    h.advance(*stream);const auto daily=std::find_if(h.marketHttp.requests.cbegin(),h.marketHttp.requests.cend(),[](const auto& r){return header(r,"api-id")=="ka10081";});QVERIFY(daily!=h.marketHttp.requests.cend());
    const QTimeZone kst("Asia/Seoul");const auto today=QDateTime::fromMSecsSinceEpoch(h.utc,kst).date();QJsonArray rows;
    for(int n=0;n<3;++n)rows.append(QJsonObject{{"dt",today.addDays(-n).toString("yyyyMMdd")},{"open_pric","70000"},{"high_pric","71000"},{"low_pric","69000"},{"cur_prc","+70500"},{"trde_qty",QString::number(1000+n)}});
    emit h.marketHttp.finished(h.marketHttp.tokens.at(daily-h.marketHttp.requests.cbegin()),publicResponse({{"stk_cd","005930"},{"stk_dt_pole_chart_qry",rows}},"ka10081"));h.advance(*stream);
    chart.refresh();const auto snapshot=chart.snapshot();QCOMPARE(snapshot.source,StockSourceKind::KiwoomDaily);QVERIFY2(snapshot.series.available,qPrintable(snapshot.series.reason));
    const bool todayClosed=QDateTime::fromMSecsSinceEpoch(h.utc,kst).time()>=QTime(20,0);
    QCOMPARE(snapshot.series.candles.size(),todayClosed?3:2); // Today's bar waits for 20:00 KST.
    QCOMPARE(snapshot.series.candles.last().close,QString("70500"));QCOMPARE(snapshot.adjustment,StockAdjustment::SourceAdjusted);QVERIFY(!snapshot.realtimeAllowed);QVERIFY(!snapshot.intradayAllowed);
    chart.setConnectionState(StockChartConnectionState::AuthenticationRequired); // Existing Toss session expired.
    QVERIFY(chart.snapshot().series.available);QVERIFY(!chart.snapshot().connectionIssue);
    chart.setPreferKiwoomDaily(true);chart.setConnectionState(StockChartConnectionState::Ready);
    QCOMPARE(chart.snapshot().source,StockSourceKind::KiwoomDaily);QVERIFY(chart.snapshot().series.available);
    chart.setConnectionState(StockChartConnectionState::OtherBroker);
    chart.setSelection(identity,900000);chart.refresh();QVERIFY(chart.snapshot().source!=StockSourceKind::KiwoomDaily);
    QCOMPARE(chart.snapshot().connectionIssue.value(),StockChartConnectionState::OtherBroker);QVERIFY(chart.snapshot().kiwoomDailyAlternative);
    chart.setSelection({"stock","US","SYN"},900000);QVERIFY(!chart.snapshot().kiwoomDailyAlternative);
    QVERIFY(!stream->daily(identity).available);
}
void KiwoomMarketStreamTest::failedDailyRefreshAndRejectedAuthorizationReplaceOldBars() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);
    auto stream=h.source(std::make_unique<FakeSocket>());const WatchlistIdentity identity{"stock","KR","005930"};
    QVERIFY(stream->setVerifiedInstruments({verified("KR","005930","KRX",h.utc)}));stream->setTargets({{identity,"KRX",1,false}});stream->setActiveCountry("KR");
    StockChartSource chart(nullptr,nullptr,[&h]{return h.utc;});chart.setKiwoomDailyStream(stream.get());
    chart.setSelection(identity,86400000);chart.setConnectionState(StockChartConnectionState::OtherBroker);chart.setActive(true);
    int answered=0;const auto answerNextDaily=[&](const HttpResult& response){
        QList<int> daily;for(int tries=0;tries<10&&daily.size()<=answered;++tries){if(tries)h.advance(*stream);daily.clear();for(int n=0;n<h.marketHttp.requests.size();++n)if(header(h.marketHttp.requests[n],"api-id")=="ka10081")daily.append(n);}
        QCOMPARE(daily.size(),answered+1);++answered;emit h.marketHttp.finished(h.marketHttp.tokens.at(daily.last()),response);h.advance(*stream);chart.refresh();};
    const auto today=QDateTime::fromMSecsSinceEpoch(h.utc,QTimeZone("Asia/Seoul")).date();QJsonArray rows;
    for(int n=1;n<4;++n)rows.append(QJsonObject{{"dt",today.addDays(-n).toString("yyyyMMdd")},{"open_pric","70000"},{"high_pric","71000"},{"low_pric","69000"},{"cur_prc","70500"},{"trde_qty","1000"}});
    h.advance(*stream);answerNextDaily(publicResponse({{"stk_cd","005930"},{"stk_dt_pole_chart_qry",rows}},"ka10081"));QVERIFY(chart.snapshot().series.available);
    // A later refresh that fails (server error or changed fields) removes the old bars and says why.
    h.advance(*stream,60001);HttpResult failed;failed.status=500;answerNextDaily(failed);
    QVERIFY(!stream->daily(identity).available);auto snapshot=chart.snapshot();QCOMPARE(snapshot.source,StockSourceKind::KiwoomDaily);
    QVERIFY(!snapshot.series.available);QVERIFY(snapshot.series.candles.isEmpty());QVERIFY(!snapshot.loading);QVERIFY(!snapshot.series.reason.isEmpty());QVERIFY(!snapshot.connectionIssue);
    h.advance(*stream,60001);answerNextDaily(publicResponse({{"stk_cd","005930"},{"unexpected_list",rows}},"ka10081"));
    QVERIFY(!chart.snapshot().series.available);QVERIFY(chart.snapshot().series.candles.isEmpty());
    // A rejected borrowed authorization is not "loading": the chart asks for re-authentication.
    h.advance(*stream,60001);HttpResult rejected;rejected.status=401;answerNextDaily(rejected);
    QVERIFY(!stream->authorizationUsable());snapshot=chart.snapshot();QVERIFY(!snapshot.loading);QVERIFY(!snapshot.series.available);
    QVERIFY2(snapshot.series.reason.contains(QStringLiteral("다시 인증")),qPrintable(snapshot.series.reason));
    QCOMPARE(snapshot.connectionIssue.value(),StockChartConnectionState::KiwoomAuthenticationRequired);
}
void KiwoomMarketStreamTest::missingExistingSessionIsPassive() {
    FakeMarketHttp http;auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();KiwoomMarketStream source(nullptr,&http,std::move(socket));
    source.setTargets({{{"stock","US","SYNTH"},"",0,false}});source.setActiveCountry("US");source.tick();QCOMPARE(fake->opens.size(),0);QCOMPARE(http.requests.size(),0);QVERIFY(source.instruments().isEmpty());QVERIFY(source.quotes().isEmpty());
}
void KiwoomMarketStreamTest::productionMetadataToUsdProofPopulatesReadonlyInstruments() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);const int brokerReads=h.brokerHttp.requests.size();h.marketHttp.automatic=true;
    auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));QCOMPARE(source->broker(),h.broker.get());
    source->setTargets({{{"stock","US","SYNTH"},"",0,false}});source->setActiveCountry("US");h.pump(*source);QVERIFY(!h.marketHttp.requests.isEmpty());QCOMPARE(source->instruments().size(),1);
    const auto i=source->instruments().first();QCOMPARE(i.identity.key(),QString("stock:US:SYNTH"));QCOMPARE(i.exchange,QString("ND"));QCOMPARE(i.currency,QString("USD"));QVERIFY(i.verified);QVERIFY(i.metadataEvidence.complete);QVERIFY(i.currencyEvidence.complete);QCOMPARE(i.metadataEvidence.sourceTimeMs,0LL);QCOMPARE(i.currencyEvidence.sourceTimeMs,0LL);QVERIFY(i.metadataEvidence.receivedAtMs<=i.currencyEvidence.receivedAtMs);QCOMPARE(fake->opens.size(),1);
    const auto metadataAt=i.metadataEvidence.receivedAtMs,currencyAt=i.currencyEvidence.receivedAtMs;const int reads=h.marketHttp.requests.size();for(int n=0;n<4;++n){const auto copy=source->instruments();QCOMPARE(copy.first().metadataEvidence.receivedAtMs,metadataAt);QCOMPARE(copy.first().currencyEvidence.receivedAtMs,currencyAt);}QCOMPARE(h.marketHttp.requests.size(),reads);QCOMPARE(h.brokerHttp.requests.size(),brokerReads);QCOMPARE(h.brokerHttp.authCount(),1);
    for(const auto& r:h.marketHttp.requests){QCOMPARE(r.operation,Operation::Reference);QVERIFY(KiwoomMarketProtocol::isRequest(r));QCOMPARE(header(r,"authorization"),QByteArray("Bearer SYNTHETIC_KIWOOM_TOKEN"));QVERIFY(!r.body.contains("9191919191"));}
}
void KiwoomMarketStreamTest::exactMetadataWithAmbiguousUsExchangeDoesNotPickFirst() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);h.marketHttp.automatic=true;h.marketHttp.ambiguous=true;
    auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));source->setTargets({{{"stock","US","SYNTH"},"",0,false}});source->setActiveCountry("US");h.pump(*source);QVERIFY(!h.marketHttp.requests.isEmpty());QCOMPARE(fake->opens.size(),0);QVERIFY(source->instruments().isEmpty());QVERIFY(source->quotes().isEmpty());
}
void KiwoomMarketStreamTest::currentLoginAndRegistrationAcksThenRestReconcileGateRealtime() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);const int brokerReads=h.brokerHttp.requests.size();
    auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(fake->opens.size(),1);const auto generation=fake->opens.first().generation;
    emit fake->connected(generation);QCOMPARE(fake->logins.size(),1);QVERIFY(fake->exactBorrow);QCOMPARE(fake->controls.size(),0);fake->frame(generation,usReal());QVERIFY(source->quotes().isEmpty());
    fake->frame(generation,R"({"trnm":"LOGIN"})");QCOMPARE(fake->controls.size(),0);fake->frame(generation,R"({"trnm":"LOGIN","return_code":0})");QCOMPARE(fake->controls.size(),1);QCOMPARE(QJsonDocument::fromJson(fake->controls.first().bytes).object().value("trnm").toString(),QString("REG"));fake->frame(generation,usReal());QVERIFY(source->quotes().isEmpty());
    const int before=h.marketHttp.requests.size();fake->frame(generation,R"({"trnm":"REG","return_code":0})");h.advance(*source);QCOMPARE(h.marketHttp.requests.size(),before+1);QCOMPARE(header(h.marketHttp.requests.last(),"api-id"),QByteArray("usa20100"));fake->frame(generation,usReal());QVERIFY(source->quotes().isEmpty());
    const auto token=h.marketHttp.tokens.last();emit h.marketHttp.finished(token,usPrice());h.advance(*source,1);fake->frame(generation,usReal());QCOMPARE(source->quotes().size(),1);const auto q=source->quotes().first();QVERIFY(q.realtime);QCOMPARE(q.price,QString("73.5"));QCOMPARE(q.evidence.sourceTimeMs,0LL);if(q.changeText.isEmpty())QCOMPARE(q.direction,MovementDirection::Unknown);else{QCOMPARE(q.direction,MovementDirection::Down);QVERIFY(q.changeEvidence.receivedAtMs>0);QVERIFY(q.changeEvidence.receivedAtMs<=q.evidence.receivedAtMs);}QCOMPARE(h.brokerHttp.requests.size(),brokerReads);QCOMPARE(h.brokerHttp.authCount(),1);
}
void KiwoomMarketStreamTest::visibleChartHoldingsPriorityUsesOne200SymbolBudget() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));
    QList<KiwoomVerifiedInstrument> proof;QList<KiwoomMarketTarget> targets;for(int n=0;n<205;++n){const auto code=syntheticCode(n);proof.append(verified("US",code,"ND",h.utc));targets.append({{"stock","US",code},"ND",3,false});}
    proof.append(verified("KR","0101N0","KRX",h.utc));targets.append({{"stock","US",syntheticCode(204)},"ND",0,false});targets.append({{"stock","US",syntheticCode(203)},"ND",1,false});targets.append({{"stock","US",syntheticCode(202)},"ND",2,true});targets.append({{"stock","KR","0101N0"},"KRX",0,false});
    QVERIFY(source->setVerifiedInstruments(proof));source->setTargets(targets);source->setActiveCountry("US");h.advance(*source);const auto selected=source->realtimeTargets();QCOMPARE(selected.size(),200);QCOMPARE(selected[0].identity.code,QString(syntheticCode(204)));QCOMPARE(selected[1].identity.code,QString(syntheticCode(203)));QCOMPARE(selected[2].identity.code,QString(syntheticCode(202)));QCOMPARE(source->restOnlyTargets().size(),6);QCOMPARE(fake->opens.size(),1);
    QSet<QString> identities;for(const auto& row:selected){QCOMPARE(row.identity.source,QString("US"));identities.insert(row.identity.key());}QCOMPARE(identities.size(),200);const auto packet=QJsonDocument::fromJson(KiwoomMarketProtocol::subscription("US",selected)).object();QCOMPARE(packet.value("data").toArray().first().toObject().value("item").toArray().size(),200);QCOMPARE(fake->maxLive,1);
}
void KiwoomMarketStreamTest::countrySwitchClosesOldBeforeNewAndRejectsLateFrames() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));
    QVERIFY(source->setVerifiedInstruments({verified("KR","0101N0","KRX",h.utc),verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","KR","0101N0"},"KRX",0,false},{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("KR");h.advance(*source);QCOMPARE(fake->opens.size(),1);const auto old=fake->opens.first().generation;emit fake->connected(old);
    source->setActiveCountry("US");QVERIFY(fake->closes.contains(old));QCOMPARE(fake->opens.size(),1);fake->frame(old,R"({"trnm":"LOGIN","return_code":0})");fake->frame(old,usReal());QVERIFY(source->quotes().isEmpty());fake->finishClose(old);h.advance(*source);QCOMPARE(fake->opens.size(),2);QCOMPARE(fake->opens.last().country,QString("US"));QVERIFY(fake->opens.last().generation!=old);QCOMPARE(fake->maxLive,1);
    const auto current=fake->opens.last().generation;const int controls=fake->controls.size();fake->frame(old,R"({"trnm":"REG","return_code":0})");emit fake->connected(old);QCOMPARE(fake->controls.size(),controls);QVERIFY(source->quotes().isEmpty());emit fake->connected(current);QCOMPARE(fake->logins.last(),current);QCOMPARE(h.brokerHttp.authCount(),1);
}
void KiwoomMarketStreamTest::metadataExpirationRevokesOldProofWithoutGetterFreshening() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));const qint64 original=h.utc;
    QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",original)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(source->instruments().size(),1);QCOMPARE(source->instruments().first().metadataEvidence.receivedAtMs,original);
    h.advance(*source,KiwoomMarketStream::MetadataFreshnessMs+1);QVERIFY(source->instruments().isEmpty());QVERIFY(source->realtimeTargets().isEmpty());QVERIFY(source->quotes().isEmpty());QVERIFY(fake->aborts>0||!fake->closes.isEmpty());QCOMPARE(h.brokerHttp.authCount(),1);
}
void KiwoomMarketStreamTest::knownAuthorizationDeadlineCannotBeResetByCountryToggle() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(fake->opens.size(),1);const auto old=fake->opens.first().generation;emit fake->connected(old);QCOMPARE(fake->logins.size(),1);
    h.advance(*source,3600001);source->setActiveCountry({});source->setActiveCountry("US");source->tick();QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));h.advance(*source);QCOMPARE(fake->opens.size(),1);QVERIFY(source->quotes().isEmpty());QCOMPARE(h.brokerHttp.authCount(),1);QVERIFY(fake->aborts>0||!fake->closes.isEmpty());
}
void KiwoomMarketStreamTest::stopRejectsLateRestFramesAndDoesNotCreateBrokerJobs() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);const int brokerReads=h.brokerHttp.requests.size();auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));
    QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(fake->opens.size(),1);const auto old=fake->opens.first().generation;emit fake->connected(old);fake->frame(old,R"({"trnm":"LOGIN","return_code":0})");fake->frame(old,R"({"trnm":"REG","return_code":0})");h.advance(*source);QVERIFY(!h.marketHttp.tokens.isEmpty());const auto token=h.marketHttp.tokens.last();
    source->stop();const int opens=fake->opens.size(),requests=h.marketHttp.requests.size();fake->frame(old,usReal());emit h.marketHttp.finished(token,usPrice());emit fake->connected(old);source->tick();QVERIFY(source->quotes().isEmpty());QVERIFY(source->instruments().isEmpty());QCOMPARE(fake->opens.size(),opens);QCOMPARE(h.marketHttp.requests.size(),requests);QCOMPARE(h.brokerHttp.requests.size(),brokerReads);QCOMPARE(h.brokerHttp.authCount(),1);
}
void KiwoomMarketStreamTest::repeatedStopDoesNotQueueAnotherChangedNotification() {
    FakeMarketHttp http;KiwoomMarketStream source(nullptr,&http);QSignalSpy changed(&source,&KiwoomMarketStream::changed);
    source.stop();QCoreApplication::sendPostedEvents(&source,QEvent::MetaCall);QCOMPARE(changed.size(),1);
    changed.clear();for(int n=0;n<12;++n){source.stop();QCoreApplication::sendPostedEvents(&source,QEvent::MetaCall);}
    QCOMPARE(changed.size(),0);QVERIFY(http.requests.isEmpty());QVERIFY(source.quotes().isEmpty());QVERIFY(source.instruments().isEmpty());
}
void KiwoomMarketStreamTest::repeatedStopAfterPendingReadKeepsRevocationAndCanStopNewIntent() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);const auto brokerReads=h.brokerHttp.requests.size();
    auto socket=std::make_unique<FakeSocket>();auto* wire=socket.get();auto source=h.source(std::move(socket));
    QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);
    QVERIFY(!wire->opens.isEmpty());const auto generation=wire->opens.last().generation;emit wire->connected(generation);wire->frame(generation,R"({"trnm":"LOGIN","return_code":0})");wire->frame(generation,R"({"trnm":"REG","return_code":0})");h.advance(*source);
    QVERIFY(!h.marketHttp.tokens.isEmpty());const auto token=h.marketHttp.tokens.last();const auto reads=h.marketHttp.requests.size();
    QCoreApplication::sendPostedEvents(source.get(),QEvent::MetaCall);QSignalSpy changed(source.get(),&KiwoomMarketStream::changed);
    const auto aborts=h.marketHttp.aborts;source->stop();QVERIFY(h.marketHttp.aborts>aborts);QCoreApplication::sendPostedEvents(source.get(),QEvent::MetaCall);QCOMPARE(changed.size(),1);changed.clear();
    for(int n=0;n<12;++n){source->stop();QCoreApplication::sendPostedEvents(source.get(),QEvent::MetaCall);}
    QCOMPARE(changed.size(),0);emit h.marketHttp.finished(token,usPrice());wire->frame(generation,usReal());emit wire->connected(generation);source->tick();QCoreApplication::processEvents();
    QVERIFY(source->quotes().isEmpty());QVERIFY(source->instruments().isEmpty());QCOMPARE(h.marketHttp.requests.size(),reads);QCOMPARE(h.brokerHttp.requests.size(),brokerReads);
    QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});QCoreApplication::sendPostedEvents(source.get(),QEvent::MetaCall);changed.clear();
    source->stop();QCoreApplication::sendPostedEvents(source.get(),QEvent::MetaCall);QCOMPARE(changed.size(),1);QVERIFY(source->instruments().isEmpty());
}
void KiwoomMarketStreamTest::hiddenUnbindDoesNotResetKnownExpiredRevision() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));
    const auto bind=[&]{QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);};
    bind();QCOMPARE(fake->opens.size(),1);h.advance(*source,3600001);source->stop();source->setBroker(nullptr);source->setBroker(h.broker.get());bind();QCOMPARE(fake->opens.size(),1);QCOMPARE(h.brokerHttp.authCount(),1);QVERIFY(source->quotes().isEmpty());
}
void KiwoomMarketStreamTest::equalRevisionNumbersRemainSeparatedByExistingOwner() {
    Harness a,b;QVERIFY(a.initialized&&b.initialized);QTRY_VERIFY_WITH_TIMEOUT(a.existingSessionReady(),3000);QTRY_VERIFY_WITH_TIMEOUT(b.existingSessionReady(),3000);
    quint64 revisionA=0,revisionB=0;QVERIFY(a.broker->withMarketAuthorization([&](QByteArrayView,qint64,quint64 rev){revisionA=rev;}));QVERIFY(b.broker->withMarketAuthorization([&](QByteArrayView,qint64,quint64 rev){revisionB=rev;}));QCOMPARE(revisionA,revisionB);QVERIFY(revisionA>0);
    auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=a.source(std::move(socket));
    const auto bind=[&](BrokerTradingController* owner){source->stop();source->setBroker(owner);QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",a.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");a.advance(*source);};
    bind(a.broker.get());QCOMPARE(fake->opens.size(),1);a.advance(*source,3600001);bind(b.broker.get());QCOMPARE(fake->opens.size(),2);QCOMPARE(source->broker(),b.broker.get());bind(a.broker.get());QCOMPARE(fake->opens.size(),2);QCOMPARE(a.brokerHttp.authCount(),1);QCOMPARE(b.brokerHttp.authCount(),1);
}
void KiwoomMarketStreamTest::authenticatedTossSessionCannotOpenOrAuthorizeKiwoom() {
    class TossHttp final:public TradingTransport {public:QList<RequestSpec> requests;void abort()override{}void send(quint64 token,const RequestSpec& r)override{requests.append(r);const auto response=keefi::test::tossResponse(r.operation);QTimer::singleShot(0,this,[this,token,response]{emit finished(token,response);});}};
    QTemporaryDir directory;Vault vault(directory.filePath("synthetic-toss-only"));QVERIFY(vault.create("Synthetic wrong provider password 2026!"));AccountRecord account;account.provider="toss";account.alias="SYNTHETIC ONLY";account.market="domestic_equity";account.environment="live";account.currency="KRW";
    QVERIFY(vault.addAccount(account,{{"client_id","SYNTHETIC_TOSS_ID"},{"client_secret","SYNTHETIC_TOSS_SECRET"}},StorageMode::EncryptedVault));TossHttp brokerHttp;BrokerTradingController broker(vault,&brokerHttp);broker.setAutomaticRefresh(false);QVERIFY(broker.selectConnection(vault.accounts().first().id));broker.authenticate();QTRY_VERIFY_WITH_TIMEOUT(broker.authenticated()&&!broker.busy(),3000);QVERIFY(broker.withMarketAuthorization([](QByteArrayView token,qint64 remaining,quint64 revision){QVERIFY(!token.empty());QVERIFY(remaining>0);QVERIFY(revision>0);}));
    FakeMarketHttp http;auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();qint64 monotonic=0,utc=QDateTime::currentMSecsSinceEpoch();auto budget=std::make_shared<KiwoomRequestBudget>([&]{return monotonic;},std::make_shared<TradingRequestLimits>([&]{return monotonic;}));
    KiwoomMarketStream source(&broker,&http,std::move(socket),[&]{return monotonic;},[&]{return utc;},nullptr,budget);QVERIFY(source.setVerifiedInstruments({verified("KR","0101N0","KRX",utc)}));source.setTargets({{{"stock","KR","0101N0"},"KRX",0,false}});source.setActiveCountry("KR");monotonic+=400;utc+=400;source.tick();
    QCOMPARE(fake->opens.size(),0);QCOMPARE(fake->logins.size(),0);QCOMPARE(http.requests.size(),0);QVERIFY(source.quotes().isEmpty());
}
void KiwoomMarketStreamTest::abortKeepsLeaseUntilPhysicalAbortReturns_data() {QTest::addColumn<bool>("timeout");QTest::newRow("explicit-stop")<<false;QTest::newRow("request-timeout")<<true;}
void KiwoomMarketStreamTest::abortKeepsLeaseUntilPhysicalAbortReturns() {
    QFETCH(bool,timeout);Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);QObject peer,third;auto socket=std::make_unique<FakeSocket>();auto source=h.source(std::move(socket));source->setTargets({{{"stock","US","SYNTH"},"",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(h.marketHttp.requests.size(),1);const auto oldToken=h.marketHttp.tokens.last();
    QCOMPARE(h.budget->tryAcquire(&peer,KiwoomBudgetLane::ExistingBroker).lease,quint64(0));KiwoomBudgetDecision during;int probes=0;h.marketHttp.onAbort=[&]{++probes;during=h.budget->tryAcquire(&peer,KiwoomBudgetLane::ExistingBroker);};const auto cleanup=qScopeGuard([&]{h.marketHttp.onAbort={};if(during.lease)h.budget->finish(during.lease);});
    h.monotonic+=timeout?15001:400;h.utc+=timeout?15001:400;if(timeout)source->tick();else source->stop();QVERIFY(probes>0);QCOMPARE(during.lease,quint64(0));
    const auto after=h.budget->tryAcquire(&peer,KiwoomBudgetLane::ExistingBroker);QVERIFY(after.lease>0);emit h.marketHttp.finished(oldToken,publicResponse({{"list",QJsonArray{usMetadataRow()}}},"usa10098"));QCOMPARE(h.budget->tryAcquire(&third,KiwoomBudgetLane::ExistingBroker).lease,quint64(0));QVERIFY(h.budget->finish(after.lease));
}
void KiwoomMarketStreamTest::registrationReplacementHidesOldWsUntilNewRestReconciliation() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc),verified("US","SYNTHB","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(fake->opens.size(),1);const auto generation=fake->opens.first().generation;emit fake->connected(generation);fake->frame(generation,R"({"trnm":"LOGIN","return_code":0})");fake->frame(generation,R"({"trnm":"REG","return_code":0})");h.advance(*source);QVERIFY(!h.marketHttp.tokens.isEmpty());emit h.marketHttp.finished(h.marketHttp.tokens.last(),usPrice());h.advance(*source,1);fake->frame(generation,usReal());QCOMPARE(source->quotes().size(),1);QVERIFY(source->quotes().first().realtime);QVERIFY(source->quotes().first().evidence.url.startsWith("wss:"));
    source->setTargets({{{"stock","US","SYNTH"},"ND",0,false},{{"stock","US","SYNTHB"},"ND",1,false}});QCOMPARE(QJsonDocument::fromJson(fake->controls.last().bytes).object().value("trnm").toString(),QString("REG"));QVERIFY(source->quotes().isEmpty());
    fake->frame(generation,usReal());QVERIFY(source->quotes().isEmpty());fake->frame(generation,R"({"trnm":"REG","return_code":0})");h.advance(*source);fake->frame(generation,usReal());QVERIFY(source->quotes().isEmpty());
}
void KiwoomMarketStreamTest::pingPrivateEventsAndReplayedPayloadPreservePublicProvenance() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});source->setActiveCountry("US");h.advance(*source);QCOMPARE(fake->opens.size(),1);const auto generation=fake->opens.first().generation;emit fake->connected(generation);
    const QByteArray ping=" { \"trnm\" : \"PING\", \"synthetic\":1 } ";fake->frame(generation,ping);QVERIFY(!fake->controls.isEmpty());QCOMPARE(fake->controls.last().bytes,ping);fake->frame(generation,R"({"trnm":"LOGIN","return_code":0})");fake->frame(generation,R"({"trnm":"REG","return_code":0})");h.advance(*source);QVERIFY(!h.marketHttp.tokens.isEmpty());emit h.marketHttp.finished(h.marketHttp.tokens.last(),usPrice());h.advance(*source,1);const auto live=usReal();fake->frame(generation,live);QCOMPARE(source->quotes().size(),1);const auto original=source->quotes().first();QVERIFY(original.realtime);
    h.advance(*source,400);for(const auto& type:QStringList{"F4","F5","00","04"}){auto packet=QJsonDocument::fromJson(live).object();auto row=packet.value("data").toArray().first().toObject();row["type"]=type;row["account"]="SYNTHETIC_PRIVATE_CANARY";row["values"]=QJsonObject{{"10","999.0000"},{"20","103016"}};packet["data"]=QJsonArray{row};fake->frame(generation,QJsonDocument(packet).toJson());QCOMPARE(source->quotes().size(),1);QCOMPARE(source->quotes().first().price,original.price);QCOMPARE(source->quotes().first().evidence.receivedAtMs,original.evidence.receivedAtMs);QVERIFY(!source->status().contains("SYNTHETIC_PRIVATE_CANARY"));}
    fake->frame(generation,live);QCOMPARE(source->quotes().first().evidence.receivedAtMs,original.evidence.receivedAtMs);
    emit fake->failed(generation,0);QVERIFY(source->quotes().isEmpty());h.advance(*source,1000);QCOMPARE(fake->opens.size(),2);const auto next=fake->opens.last().generation;emit fake->connected(next);fake->frame(next,R"({"trnm":"LOGIN","return_code":0})");fake->frame(next,R"({"trnm":"REG","return_code":0})");h.advance(*source);const auto reconciliationAt=h.utc;emit h.marketHttp.finished(h.marketHttp.tokens.last(),usPrice());h.advance(*source,1);fake->frame(next,live);QCOMPARE(source->quotes().size(),1);QVERIFY(!source->quotes().first().realtime);QVERIFY(source->quotes().first().evidence.url.startsWith("https:"));QCOMPARE(source->quotes().first().evidence.receivedAtMs,reconciliationAt);
}
void KiwoomMarketStreamTest::revokedIntentCannotReopenTheStillValidSameRevision() {
    Harness h;QVERIFY(h.initialized);QTRY_VERIFY_WITH_TIMEOUT(h.existingSessionReady(),3000);auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();auto source=h.source(std::move(socket));
    const auto configure=[&]{QVERIFY(source->setVerifiedInstruments({verified("US","SYNTH","ND",h.utc)}));source->setActiveCountry("US");source->setTargets({{{"stock","US","SYNTH"},"ND",0,false}});h.advance(*source);};
    configure();QCOMPARE(fake->opens.size(),1);const int requests=h.marketHttp.requests.size();QVERIFY(h.broker->authenticated());QVERIFY(QMetaObject::invokeMethod(h.broker.get(),"intentRevoked",Qt::DirectConnection));QVERIFY(h.broker->authenticated());
    source->stop();source->setBroker(nullptr);source->setBroker(h.broker.get());configure();QCOMPARE(fake->opens.size(),1);QCOMPARE(h.marketHttp.requests.size(),requests);QCOMPARE(h.brokerHttp.authCount(),1);QVERIFY(source->quotes().isEmpty());
}
QTEST_GUILESS_MAIN(KiwoomMarketStreamTest)
#include "tst_kiwoom_market_stream.moc"
