#include "keefi/LsAdapter.h"
#include "fixtures/LsResponses.h"
#include <QtTest>
#include <QUrlQuery>
#include <sodium.h>
using namespace keefi;using namespace keefi::test;
namespace {
AccountRecord account(QString env="live"){AccountRecord a;a.provider="ls";a.environment=env;a.market="domestic_equity";a.currency="KRW";return a;}
CredentialSet credentials(QString assertion={}){CredentialSet c;c.emplace("app_key",SecureBuffer::fromUtf8("FAKE+LS/KEY"));c.emplace("app_secret",SecureBuffer::fromUtf8("FAKE+LS/SECRET"));if(!assertion.isEmpty())c.emplace("account_number",SecureBuffer::fromUtf8(assertion));return c;}
OrderInput intent(){OrderInput i;i.symbol="005930";i.price="70000";i.quantity="10";return i;}
bool connected(LsAdapter& a,QString env="live",QString assertion={}){QString e;auto c=credentials(assertion);if(!a.configureSession(account(env),c,&e))return false;for(const auto& s:a.plan(BrokerPlanKind::Authenticate,{})){auto r=a.buildStep(s,0,&e);if(!e.isEmpty()||!a.parseStep(s,lsResponse(r)).value.ok)return false;}return a.accountChoices().size()==1&&a.selectBrokerAccount(a.accountChoices().first().handle,&e);}
BrokerStepResult run(LsAdapter& a,const BrokerStep& step){QString error;auto req=a.buildStep(step,0,&error);if(!error.isEmpty())return {};return a.parseStep(step,lsResponse(req));}
bool catalog(LsAdapter& a){for(const auto& s:a.plan(BrokerPlanKind::Catalog,{}))if(!run(a,s).value.ok)return false;return true;}
QList<BrokerEvidence> preflight(LsAdapter& a,const OrderInput& i){QList<BrokerEvidence> result;for(const auto& s:a.plan(BrokerPlanKind::OrderPreflight,i)){auto p=run(a,s);if(!p.value.ok)return {};BrokerEvidence e;e.operation=s.operation;e.variant=s.variant;e.accountScope=a.selectedAccountScope();e.receivedUtc=a.utcNow();e.complete=!p.value.hasMore;e.sourceTime=p.sourceTime;e.data=p.value.data;result<<e;}return result;}
HttpResult changed(HttpResult h,const std::function<void(QJsonObject&)>& edit){auto o=QJsonDocument::fromJson(h.body).object();edit(o);h.body=QJsonDocument(o).toJson(QJsonDocument::Compact);return h;}
HttpResult openRows(QJsonArray rows){return lsJson({{"rsp_cd","00000"},{"t0425OutBlock",QJsonObject{{"cts_ordno",""}}},{"t0425OutBlock1",rows}});}
QString ref(QString n="100"){return LsAdapter::orderReference("20261019","KRX",n);}
}
class LsTests:public QObject{Q_OBJECT
private slots:
    void authenticatedSessionCannotConstructFinancialRequests() {
        LsAdapter a("live",lsNow); QVERIFY(connected(a));
        QVERIFY(a.sessionReady());
        for (const auto operation : {Operation::PlaceOrder,Operation::CancelOrder,Operation::AmendOrder,Operation::TestOrder,Operation::AlgoCancelOrder}) {
            auto input=intent(); input.orderId="synthetic-old-order";
            const auto step=a.directStep(operation,input); QString error;
            const auto request=a.buildStep(step,0,&error);
            QVERIFY(request.url.isEmpty()); QVERIFY(!error.isEmpty());
        }
    }

 void initTestCase(){QVERIFY(sodium_init()>=0);}
 void tokenAliasesAndPrivateAccount(){LsAdapter a("live",lsNow);QVERIFY(connected(a));QCOMPARE(a.selectedAccountScope().size(),64);QVERIFY(!a.selectedAccountLabel().contains("01234567890"));a.clearSession();QVERIFY(!a.sessionReady());LsAdapter b("live",lsNow);QVERIFY(!connected(b,"live","99999999999"));}
 void formEncodingDoesNotTurnPlusIntoSpace(){LsAdapter a("live",lsNow);QString e;auto c=credentials();QVERIFY(a.configureSession(account(),c,&e));auto r=a.buildStep(a.plan(BrokerPlanKind::Authenticate,{}).first(),0,&e);QVERIFY(e.isEmpty());QVERIFY(r.body.contains("FAKE%2BLS%2FKEY"));QCOMPARE(r.url.toString(),"https://openapi.ls-sec.co.kr:8080/oauth2/token");QVERIFY(!r.headers.contains("authorization"));}
 void referenceGrammarKeepsPadding(){LsAdapter a("live");QVERIFY(a.orderReferenceValid(ref("00000100")));QVERIFY(ref("00000100")!=ref());QVERIFY(!a.orderReferenceValid(ref("0")));QVERIFY(!a.orderReferenceValid(ref()+"\n"));}
 void tokensNeedExpiryAndUnauthorizedAlwaysClears(){LsAdapter a("live",lsNow);QString error;auto c=credentials();QVERIFY(a.configureSession(account(),c,&error));auto step=a.directStep(Operation::Authenticate,{});auto h=lsResponse(a.buildStep(step,0,&error));h=changed(h,[](QJsonObject& o){o.remove("expire_in");o["expires_in"]="3600";});QVERIFY(a.parseStep(step,h).value.ok);h=changed(h,[](QJsonObject& o){o["expire_in"]="3599";});QVERIFY(!a.parseStep(step,h).value.ok);QVERIFY(connected(a));HttpResult denied;denied.status=401;denied.body="<html>no</html>";QVERIFY(a.parseStep(a.directStep(Operation::Account,{}),denied).value.authenticationFailed);QVERIFY(!a.sessionReady());}

 void holdingsClassificationStaysUnknownAndPrivateFieldsAreAbsent(){LsAdapter a("live",lsNow);QVERIFY(connected(a));for(auto op:{Operation::Account,Operation::Holdings}){auto r=run(a,a.directStep(op,{}));QVERIFY(r.value.ok);QVERIFY(r.accountReadVerified);const auto json=QJsonDocument(r.value.data).toJson();QVERIFY(!json.contains("01234567890"));QVERIFY(!json.contains("FAKE_PASSWORD"));QVERIFY(!json.contains("FAKE PRIVATE"));if(op==Operation::Account)QCOMPARE(r.value.data.value("depositCash").toString(),QString("7000000"));else{QVERIFY(!r.value.data.contains("depositCash"));QVERIFY(!r.value.data.value("items").toArray().first().toObject().value("cashHolding").toBool());}}}
 void malformedCashAndDuplicatePositionsAreRejected(){LsAdapter a("live",lsNow);QVERIFY(connected(a));QString e;auto step=a.directStep(Operation::Account,{});auto h=lsResponse(a.buildStep(step,0,&e));h=changed(h,[](QJsonObject& o){auto x=o["CSPAQ12200OutBlock2"].toObject();x["Dps"]="garbage1";o["CSPAQ12200OutBlock2"]=x;});QVERIFY(!a.parseStep(step,h).value.ok);step=a.directStep(Operation::Holdings,{});h=lsResponse(a.buildStep(step,0,&e));h=changed(h,[](QJsonObject& o){auto rows=o["t0424OutBlock1"].toArray();rows.append(rows.first());o["t0424OutBlock1"]=rows;});QVERIFY(!a.parseStep(step,h).value.ok);}
 void fullCashPreflightAndEachMissingCondition(){LsAdapter a("live",lsNow);QVERIFY(connected(a));QVERIFY(catalog(a));auto ev=preflight(a,intent());QCOMPARE(ev.size(),10);auto ready=a.actionReadiness(BrokerAction::Place,intent(),ev,{"1000000","KRW","5"});QVERIFY2(ready.ready,qPrintable(ready.message));QCOMPARE(ready.validation.notional,QString("700000"));for(int j=0;j<4;++j){auto missing=ev;missing.removeAt(j);QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),missing,{"1000000","KRW","5"}).ready);}auto bad=ev;bad[0].data["excludedSymbols"]=QJsonArray{"005930"};QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),bad,{"1000000","KRW","5"}).ready);bad=ev;bad[0].data["firstReceivedAt"]=lsNow().addSecs(-61).toString(Qt::ISODateWithMs);QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),bad,{"1000000","KRW","5"}).ready);bad=ev;for(auto& e:bad)if(e.operation==Operation::BuyingPower)e.data["noMarginBuyingPower"]="1";QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),bad,{"1000000","KRW","5"}).ready);}
 void sourceDatesNotInventedAndStaleBarBlocks(){LsAdapter a("live",lsNow);QVERIFY(connected(a));QVERIFY(catalog(a));auto ev=preflight(a,intent());for(auto& e:ev){if(e.operation==Operation::Depth){QCOMPARE(e.sourceTime.quality,SourceTimeQuality::TimeOfDayOnly);QVERIFY(!e.sourceTime.fullTimestamp.isValid());}if(e.operation==Operation::Reference)e.sourceTime.fullTimestamp=lsNow().addSecs(-121);}QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),ev,{"1000000","KRW","5"}).ready);}
 void filterPaginationUsesOriginalReceiptAndAccumulates(){LsAdapter a("live",lsNow);QVERIFY(connected(a));auto step=a.plan(BrokerPlanKind::OrderPreflight,intent()).first();auto h=lsJson({{"rsp_cd","00000"},{"t1404OutBlock",QJsonObject{{"cts_shcode","000001"}}},{"t1404OutBlock1",QJsonArray{QJsonObject{{"shcode","000001"}}}}});h.headers={{"tr_cont","Y"},{"tr_cont_key","opaque-next"}};auto p=a.parseStep(step,h);QVERIFY(p.value.ok);QVERIFY(p.value.hasMore);auto first=p.value.data.value("firstReceivedAt");step.cursorHandle=p.value.nextCursor;QString e;auto req=a.buildStep(step,0,&e);QVERIFY(e.isEmpty());QCOMPARE(req.headers.value("tr_cont"),QByteArray("Y"));h=lsJson({{"rsp_cd","00000"},{"t1404OutBlock",QJsonObject{{"cts_shcode",""}}},{"t1404OutBlock1",QJsonArray{QJsonObject{{"shcode","000002"}}}}});p=a.parseStep(step,h);QVERIFY(p.value.ok);QVERIFY(!p.value.hasMore);QCOMPARE(p.value.data.value("excludedSymbols").toArray().size(),2);QCOMPARE(p.value.data.value("firstReceivedAt"),first);QVERIFY(!a.parseStep(step,h).value.ok);}
 void nonemptyHistoryCannotClaimEmptyOrNormal(){LsAdapter a("live",lsNow);QVERIFY(connected(a));auto step=a.directStep(Operation::History,intent());QString e;auto h=lsResponse(a.buildStep(step,0,&e));QVERIFY(a.parseStep(step,h).value.ok);h=changed(h,[](QJsonObject& o){o["CSPAQ13700OutBlock3"]=QJsonArray{lsDetailOrder()};});QVERIFY(!a.parseStep(step,h).value.ok);h=changed(h,[](QJsonObject& o){o["rsp_cd"]="00999";});auto p=a.parseStep(step,h);QVERIFY(!p.value.ok);QVERIFY(!p.value.notFound);QVERIFY(p.value.orders.isEmpty());}

 void externalOrMissingVenueNeverBecomesCashOrder(){LsAdapter a("live",lsNow);QVERIFY(connected(a));auto row=lsOpenOrder();auto step=a.directStep(Operation::OpenOrders,{});auto p=a.parseStep(step,openRows({row}));QVERIFY(p.value.ok);QVERIFY(!p.value.orders.first().cancelable);row.remove("exchname");p=a.parseStep(step,openRows({row}));QVERIFY(p.value.ok);QCOMPARE(p.value.orders.first().market,QString("unknown"));QVERIFY(!a.orderReferenceValid(p.value.orders.first().orderId));QVERIFY(!p.value.orders.first().cancelable);}









};
QTEST_GUILESS_MAIN(LsTests)
#include "tst_ls.moc"
