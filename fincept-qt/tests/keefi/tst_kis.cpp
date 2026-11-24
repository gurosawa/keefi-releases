#include "keefi/KisAdapter.h"
#include "fixtures/KisResponses.h"
#include "fixtures/KisMasterFixture.h"
#include <QtTest>
#include <algorithm>
#include <QSet>
#include <QJsonArray>
#include <QUrlQuery>
using namespace keefi;
using namespace keefi::test;
namespace {
QDateTime now() { return QDateTime(QDate(2026,10,19), QTime(10,0), QTimeZone("Asia/Seoul")); }
AccountRecord account(const QString& env = "live") { AccountRecord a; a.provider="kis"; a.market="domestic_equity"; a.currency="KRW"; a.environment=env; return a; }
CredentialSet credentials(const QString& number = "12345678", const QString& key = "fake-app-key") {
    CredentialSet c; c.emplace("app_key", SecureBuffer::fromUtf8(key)); c.emplace("app_secret", SecureBuffer::fromUtf8("fake-app-secret"));
    c.emplace("account_number", SecureBuffer::fromUtf8(number)); c.emplace("product_code", SecureBuffer::fromUtf8("01")); return c;
}
OrderInput intent() { OrderInput in; in.symbol="005930"; in.side="buy"; in.type="limit"; in.price="70000"; in.quantity="10"; return in; }
OrderLimits limits() { return {"20000000", "KRW", "5"}; }
bool ready(KisAdapter& a, const QString& env = "live", const QString& number = "12345678", const QString& key = "fake-app-key") {
    QString error; const auto c=credentials(number,key);
    if (!a.configureSession(account(env),c,&error)) return false;
    const auto step=a.plan(BrokerPlanKind::Authenticate,{}).first(); const auto request=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
    if (!error.isEmpty() || !a.parseStep(step,kisResponse(request,now())).value.ok) return false;
    return a.selectBrokerAccount(a.accountChoices().first().handle,&error);
}
bool catalog(KisAdapter& a) {
    auto step=a.plan(BrokerPlanKind::Catalog,{}).first(); HttpResult h; h.status=200; h.body=zip(row("KOSPI"));
    return a.parseStep(step,h).value.ok;
}
HttpResult mutate(const RequestSpec& request, const std::function<void(QJsonObject&)>& change) {
    auto h=kisResponse(request,now()); auto o=QJsonDocument::fromJson(h.body).object(); change(o); h.body=QJsonDocument(o).toJson(QJsonDocument::Compact); return h;
}
QList<BrokerEvidence> evidence(KisAdapter& a, const OrderInput& in, BrokerPlanKind kind=BrokerPlanKind::OrderPreflight) {
    QList<BrokerEvidence> all;
    for (const auto& step:a.plan(kind,in)) {
        QString error; auto request=a.buildStep(step,now().toMSecsSinceEpoch(),&error); if (!error.isEmpty()) return {};
        const auto parsed=a.parseStep(step,kisResponse(request,now())); if (!parsed.value.ok) return {};
        BrokerEvidence e; e.operation=step.operation; e.variant=step.variant; e.data=parsed.value.data; e.complete=!parsed.value.hasMore;
        e.sourceTime=parsed.sourceTime; e.receivedUtc=now(); e.accountScope=a.selectedAccountScope(); all.append(e);
    }
    return all;
}
MarketSnapshot snapshot(const QList<BrokerEvidence>& all) {
    MarketSnapshot s; for (const auto& e:all) s.data[e.operation]=e.data; return s;
}
QString reference(const QString& number="0000000011") { return KisAdapter::orderReference("20261019","00001",number,"KRX"); }
}
class KisTests : public QObject {
    Q_OBJECT
private slots:
    void authenticatedSessionCannotConstructFinancialRequests() {
        KisAdapter a("live",now); QVERIFY(ready(a));
        QVERIFY(a.sessionReady());
        for (const auto operation : {Operation::PlaceOrder,Operation::CancelOrder,Operation::AmendOrder,Operation::TestOrder,Operation::AlgoCancelOrder}) {
            auto input=intent(); input.orderId="synthetic-old-order";
            const auto step=a.directStep(operation,input); QString error;
            const auto request=a.buildStep(step,0,&error);
            QVERIFY(request.url.isEmpty()); QVERIFY(!error.isEmpty());
        }
    }

    void catalogAcceptsUnsupportedMasterCodesWithoutEnablingOrders() {
        KisAdapter a("live",now); QVERIFY(a.catalogSymbolValid("F70100034")); QVERIFY(!a.symbolValid("F70100034"));
        QVERIFY(a.catalogSymbolValid("005930")); QVERIFY(a.catalogSymbolValid("Q005930"));
        for(const auto& value:{"$123456","12345","F701000340","005930\n","abc123","００５９３０"}) QVERIFY(!a.catalogSymbolValid(value));
        QVERIFY(ready(a)); auto in=intent(); in.symbol="F70100034"; QString error;
        a.buildStep(a.directStep(Operation::PlaceOrder,in),now().toMSecsSinceEpoch(),&error); QVERIFY(!error.isEmpty());
    }
    void configuredAccountIsPrivateAndSeparateFromAuthentication() {
        KisAdapter a("live",now); QString error; const auto c=credentials(); QVERIFY(a.configureSession(account(),c,&error));
        QVERIFY(a.accountChoices().isEmpty()); QVERIFY(a.selectedAccountScope().isEmpty());
        auto step=a.plan(BrokerPlanKind::Authenticate,{}).first(); auto r=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        QCOMPARE(r.url.toString(),"https://openapi.koreainvestment.com:9443/oauth2/tokenP"); QCOMPARE(r.method,"POST"); QVERIFY(!r.headers.contains("authorization"));
        auto parsed=a.parseStep(step,kisResponse(r,now())); QVERIFY(parsed.value.ok); QVERIFY(!parsed.accountReadVerified); QVERIFY(!parsed.value.tradingPermissionKnown);
        QVERIFY(!QJsonDocument(parsed.value.data).toJson().contains("fake-app")); QVERIFY(!QJsonDocument(parsed.value.data).toJson().contains("12345678"));
        const auto choice=a.accountChoices().first(); QVERIFY(!choice.label.contains("12345678")); QVERIFY(choice.label.contains("5678"));
        QVERIFY(a.selectBrokerAccount(choice.handle,&error)); QCOMPARE(a.selectedAccountScope().size(),64);
        a.clearSession(); QVERIFY(!a.sessionReady()); QVERIFY(a.accountChoices().isEmpty()); QVERIFY(a.selectedAccountScope().isEmpty());
    }
    void accountScopeSurvivesKeyRotationButNotAccountChange() {
        KisAdapter a("live",now), b("live",now), c("live",now);
        QVERIFY(ready(a)); QVERIFY(ready(b,"live","12345678","another-key")); QVERIFY(ready(c,"live","87654321"));
        QCOMPARE(a.selectedAccountScope(),b.selectedAccountScope()); QVERIFY(a.selectedAccountScope()!=c.selectedAccountScope());
        QVERIFY(a.credentialIdentityFingerprint(credentials())!=a.credentialIdentityFingerprint(credentials("12345678","another-key")));
    }
    void rejectsWrongEnvironmentAndHeaderInjection() {
        KisAdapter a("live",now); QString error; QVERIFY(!a.configureSession(account("paper"),credentials(),&error));
        QVERIFY(!a.configureSession(account(),credentials("12345678","key\r\nX:evil"),&error));
        QVERIFY(!a.configureSession(account(),credentials("1234567"),&error));
    }
    void retiredPaperEnvironmentCannotCreateSignedRequests() {
        KisAdapter a("paper",now); QString error;
        QVERIFY(!a.configureSession(account("paper"),credentials(),&error));
        QVERIFY(!error.isEmpty()); QVERIFY(!a.sessionReady()); QVERIFY(a.accountChoices().isEmpty());
        const auto request=a.buildStep(a.directStep(Operation::Account,intent()),now().toMSecsSinceEpoch(),&error);
        QVERIFY(request.url.isEmpty()); QVERIFY(!error.isEmpty());
        // The live plan still includes the opening calendar and explicit sellable quantity.
        KisAdapter live("live",now); QVERIFY(ready(live));
        QList<Operation> operations; auto sell=intent(); sell.side="sell";
        for (const auto& step:live.plan(BrokerPlanKind::OrderPreflight,sell)) operations.append(step.operation);
        QVERIFY(operations.contains(Operation::TradingSchedule)); QVERIFY(operations.contains(Operation::SellableQuantity));
    }
    void publicCatalogNeedsNoCredentialsAndCarriesSecurityType() {
        KisAdapter a("live",now); auto step=a.plan(BrokerPlanKind::Catalog,{}).first(); QString error; auto req=a.buildStep(step,0,&error);
        QVERIFY(error.isEmpty()); QVERIFY(!req.authenticated); QVERIFY(req.headers.isEmpty()); QVERIFY(req.body.isEmpty());
        HttpResult h; h.status=200; h.body=zip(row("KOSPI")); auto parsed=a.parseStep(step,h);
        QVERIFY(parsed.value.ok); const auto item=parsed.value.data.value("instruments").toArray().first().toObject(); QCOMPARE(item.value("securityType").toString(),"ST");
    }
    void accountOnlyVerifiedAfterCompleteBalanceAndRejectsMalformedCash() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto step=a.directStep(Operation::Holdings,intent()); QString error; auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        auto good=a.parseStep(step,kisResponse(req,now())); QVERIFY(good.value.ok); QVERIFY(good.accountReadVerified); QCOMPARE(good.value.data.value("depositCash").toString(),"10000000");
        for (const auto& value:{"garbage1","1oops","1\n","--1"}) {
            const auto response=mutate(req,[&](QJsonObject& root){root["output2"]=QJsonArray{QJsonObject{{"dnca_tot_amt",value}}};});
            QVERIFY(!a.parseStep(step,response).value.ok);
        }
        auto negative=mutate(req,[](QJsonObject& root){root["output2"]=QJsonArray{QJsonObject{{"dnca_tot_amt","-1000"}}};});
        QVERIFY(a.parseStep(step,negative).value.ok);
    }
    void paginationUsesOpaqueBoundedPrivateCursorAndPreservesAccountScope() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto step=a.directStep(Operation::Holdings,intent()); QString error; auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        auto response=mutate(req,[](QJsonObject& root){root["ctx_area_fk100"]="12345678 PRIVATE";root["ctx_area_nk100"]="private-next";}); response.headers["tr_cont"]="M";
        auto parsed=a.parseStep(step,response); QVERIFY(parsed.value.ok); QVERIFY(parsed.value.hasMore); QVERIFY(!parsed.accountReadVerified); QVERIFY(!parsed.value.nextCursor.contains("12345678"));
        auto next=step; next.cursorHandle=parsed.value.nextCursor; auto nextRequest=a.buildStep(next,now().toMSecsSinceEpoch(),&error);
        QVERIFY(error.isEmpty()); QCOMPARE(nextRequest.headers.value("tr_cont"),"N"); QCOMPARE(QUrlQuery(nextRequest.url).queryItemValue("CTX_AREA_NK100"),"private-next");
        auto wrong=next; wrong.input.symbol="000660"; error.clear(); a.buildStep(wrong,now().toMSecsSinceEpoch(),&error); QVERIFY(!error.isEmpty());
        auto last=a.parseStep(next,kisResponse(nextRequest,now())); QVERIFY(last.value.ok); QVERIFY(last.accountReadVerified);
        error.clear(); a.buildStep(next,now().toMSecsSinceEpoch(),&error); QVERIFY(!error.isEmpty());
    }
    void holdingIdentitySeparatesCashAndLoanAndDoesNotDependOnValuesOrRowOrder() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto step=a.directStep(Operation::Holdings,intent()); QString error;
        const auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error); QVERIFY(error.isEmpty());
        auto h=mutate(req,[](QJsonObject& root){
            auto rows=root["output1"].toArray(); auto loan=rows.first().toObject();
            loan["trad_dvsn_name"]="융자"; loan["loan_dt"]="20261015"; loan["expd_dt"]="20270415"; loan["loan_amt"]="70000";
            rows.append(loan); root["output1"]=rows;
        });
        const auto parsed=a.parseStep(step,h); QVERIFY(parsed.value.ok); auto rows=parsed.value.data["items"].toArray(); QCOMPARE(rows.size(),2);
        const auto cash=rows[0].toObject(), loan=rows[1].toObject(); QCOMPARE(cash["symbol"],loan["symbol"]);
        const auto cashKey=cash["positionKey"].toString(), loanKey=loan["positionKey"].toString(); QCOMPARE(cashKey.size(),64); QVERIFY(cashKey!=loanKey);
        QVERIFY(cash["cashHolding"].toBool()); QVERIFY(!loan["cashHolding"].toBool());
        auto raw=QJsonDocument::fromJson(h.body).object(); auto original=raw["output1"].toArray(); auto altered=original[0].toObject();
        altered["hldg_qty"]="99"; altered["ord_psbl_qty"]="89"; altered["evlu_amt"]="6930000";
        raw["output1"]=QJsonArray{original[1],altered}; auto reordered=a.parseStep(step,kisJson(raw)); QVERIFY(reordered.value.ok);
        rows=reordered.value.data["items"].toArray(); QCOMPARE(rows[0].toObject()["positionKey"].toString(),loanKey); QCOMPARE(rows[1].toObject()["positionKey"].toString(),cashKey);
    }
    void duplicateHoldingIdentityCannotBeHiddenByArrayPositionOrSpacePadding() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto step=a.directStep(Operation::Holdings,intent()); QString error;
        const auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        for(bool padded:{false,true}) {
            auto h=mutate(req,[&](QJsonObject& root){auto rows=root["output1"].toArray();auto copy=rows.first().toObject();
                if(padded) {copy["trad_dvsn_name"]="현금 ";copy["loan_dt"]="        ";copy["expd_dt"]="        ";}
                rows.append(copy);root["output1"]=rows;});
            QVERIFY(!a.parseStep(step,h).value.ok);
        }
    }
    void malformedHoldingIdentityCannotBecomeAnEmptyDateOrCashRow() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto step=a.directStep(Operation::Holdings,intent()); QString error;
        const auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        for(int which=0;which<7;++which) {
            auto h=mutate(req,[&](QJsonObject& root){auto rows=root["output1"].toArray();auto r=rows.first().toObject();
                if(which==0) r.remove("trad_dvsn_name");
                if(which==1) r["loan_dt"]=QJsonValue(QJsonValue::Null);
                if(which==2) r["loan_dt"]="20260230";
                if(which==3) r["expd_dt"]="202710199";
                if(which==4) r["trad_dvsn_name"]="현금\n";
                if(which==5) r["loan_amt"]="garbage0";
                if(which==6) r["trad_dvsn_name"]=QString(61,'A');
                rows[0]=r;root["output1"]=rows;});
            QVERIFY(!a.parseStep(step,h).value.ok);
        }
    }
    void missingContinuationHeaderCannotClaimComplete() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto step=a.directStep(Operation::Holdings,intent());QString error;
        auto r=a.buildStep(step,now().toMSecsSinceEpoch(),&error);auto h=kisResponse(r,now());h.headers.clear();
        auto p=a.parseStep(step,h);QVERIFY(!p.value.ok);QVERIFY(!p.accountReadVerified);
    }
    void accountWideQueriesFixDateWindowAndUseCurrentTr() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto in=intent(); QString error;
        for (const auto op:{Operation::OpenOrders,Operation::History}) {
            const auto step=a.directStep(op,in); QVERIFY(step.input.startTimeMs>0); auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error); QVERIFY(error.isEmpty());
            const QUrlQuery q(req.url); QVERIFY(q.queryItemValue("PDNO").isEmpty()); QCOMPARE(q.queryItemValue("EXCG_ID_DVSN_CD"),"ALL"); QCOMPARE(q.queryItemValue("INQR_DVSN_3"),"01"); QCOMPARE(req.headers.value("tr_id"),"TTTC0081R");
        }
    }
    void exactIntentAndNoMarginMarketCapAreBothRequested() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QSet<QString> variants;
        for (const auto& step:a.plan(BrokerPlanKind::OrderPreflight,intent())) if (step.operation==Operation::BuyingPower) {
            variants.insert(step.variant); QString error; auto r=a.buildStep(step,now().toMSecsSinceEpoch(),&error); QVERIFY(error.isEmpty()); QUrlQuery q(r.url);
            QCOMPARE(q.queryItemValue("ORD_DVSN"),step.variant=="cash-market-cap"?"01":"00"); QCOMPARE(q.queryItemValue("ORD_UNPR"),step.variant=="cash-market-cap"?"0":"70000");
            QCOMPARE(q.queryItemValue("CMA_EVLU_AMT_ICLD_YN"),"N"); QCOMPARE(q.queryItemValue("OVRS_ICLD_YN"),"N");
        }
        QCOMPARE(variants.size(),2);
    }
    void noMarginLimitIsUsedInsteadOfMarginMaximumAndFeesStayUnknown() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QVERIFY(catalog(a)); auto all=evidence(a,intent()); QVERIFY(!all.isEmpty());
        auto ready=a.actionReadiness(BrokerAction::Place,intent(),all,limits()); QVERIFY2(ready.ready,qPrintable(ready.message)); QCOMPARE(ready.validation.notional,"700000");
        for (auto& e:all) if (e.variant=="cash-market-cap") { e.data["noMarginQuantity"]="9"; e.data["max_buy_qty"]="99999"; }
        QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),all,limits()).ready);
        for (const auto& e:all) if (e.operation==Operation::BuyingPower) { QVERIFY(!e.data.value("feeKnown").toBool()); QVERIFY(!e.data.contains("commissionRate")); }
    }
    void exactPriceChangeAndMissingCapDoNotReusePriorEvidence() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QVERIFY(catalog(a)); auto all=evidence(a,intent());
        all.erase(std::remove_if(all.begin(),all.end(),[](const auto& e){return e.variant=="cash-market-cap";}),all.end());
        QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),all,limits()).ready);
        auto s=snapshot(evidence(a,intent())); s.data[Operation::BuyingPower]["symbol"]="000660"; QVERIFY(!a.validate(intent(),s,limits()).ok);
    }
    void sourceTimeOfDayIsNotFabricatedFullTimestamp() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto s=a.directStep(Operation::Depth,intent()); QString error; auto r=a.buildStep(s,now().toMSecsSinceEpoch(),&error);
        auto p=a.parseStep(s,kisResponse(r,now())); QVERIFY(p.value.ok); QCOMPARE(p.sourceTime.quality,SourceTimeQuality::TimeOfDayOnly); QVERIFY(!p.sourceTime.fullTimestamp.isValid()); QVERIFY(p.value.data.value("timestamp").isNull());
    }
    void staleBookClosedDayAndNonRegularStateBlockOnlyNewOrders() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QVERIFY(catalog(a)); const auto base=evidence(a,intent());
        for (int which=0;which<3;++which) {
            auto all=base; for(auto& e:all) {
                if (which==0 && e.operation==Operation::Depth) e.data["sourceTimeOfDay"]="095900";
                if (which==1 && e.operation==Operation::Depth) e.data["marketState"]="00";
                if (which==2 && e.operation==Operation::TradingSchedule) e.data["days"]=QJsonArray{QJsonObject{{"date","20261019"},{"open",false}}};
            }
            QVERIFY(!a.actionReadiness(BrokerAction::Place,intent(),all,limits()).ready); QVERIFY(a.actionReadiness(BrokerAction::Read,intent(),all,limits()).ready);
        }
    }
    void liveCashSellRequiresExplicitSellableQuantity() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QVERIFY(catalog(a)); auto in=intent(); in.side="sell";
        auto all=evidence(a,in); auto ready=a.actionReadiness(BrokerAction::Place,in,all,limits());
        QVERIFY2(ready.ready,qPrintable(ready.message)); QVERIFY(ready.validation.ok);
        all.removeIf([](const auto& item){return item.operation==Operation::SellableQuantity;});
        QVERIFY(!a.actionReadiness(BrokerAction::Place,in,all,limits()).ready);
    }
    void tuplePreservesLeadingZerosAndSeparatesDateBranchAndVenue() {
        KisAdapter a("live",now); QVERIFY(a.orderReferenceValid(reference())); QCOMPARE(reference(),"K1_20261019_00001_0000000011_KRX");
        QVERIFY(reference()!=KisAdapter::orderReference("20261019","00001","11","KRX"));
        QVERIFY(!a.orderReferenceValid("K1_20260230_00001_1_KRX")); QVERIFY(!a.orderReferenceValid(reference()+"\n"));
        QVERIFY(!a.orderReferenceValid("K1_20261019_00001_0_KRX")); QVERIFY(!a.orderReferenceValid("K1_20261019_000001_1_KRX"));
    }
    void symbolValidationRejectsGarbagePrefixInsteadOfMatchingSuffix() {
        KisAdapter a("live",now); QVERIFY(a.symbolValid("005930")); QVERIFY(a.symbolValid("Q005930"));
        for (const auto& value:{"$123456","X005930"," 005930","005930\n","QQ005930"}) QVERIFY(!a.symbolValid(value));
        QVERIFY(ready(a)); auto in=intent(); in.symbol="$123456"; QString error;
        a.buildStep(a.directStep(Operation::Rules,in),now().toMSecsSinceEpoch(),&error); QVERIFY(!error.isEmpty());
    }

    void restoredReceiptBindingRequiresSelectedSessionAndCanonicalNonConflictingIds() {
        KisAdapter a("live",now); auto in=intent(); in.orderId=reference(); in.operationOrderId=reference("0000000033");
        QVERIFY(!a.restoreReconciliationIntent("amend",in)); QVERIFY(ready(a));
        QVERIFY(a.restoreReconciliationIntent("amend",in)); QVERIFY(a.restoreReconciliationIntent("amend",in));
        QVERIFY(!a.restoreReconciliationIntent("place",in));
        for(int which=0;which<6;++which) {
            auto wrong=in;
            if(which==0) wrong.orderId=KisAdapter::orderReference("20261019","00002","0000000011","KRX");
            if(which==1) wrong.orderId=wrong.operationOrderId;
            if(which==2) wrong.operationOrderId=KisAdapter::orderReference("20261018","00001","0000000033","KRX");
            if(which==3) wrong.operationOrderId=KisAdapter::orderReference("20261019","00001","0000000033","NXT");
            if(which==4) wrong.operationOrderId+="\n";
            if(which==5) wrong.orderId.clear();
            QVERIFY(!a.restoreReconciliationIntent("amend",wrong));
        }
        QVERIFY(a.restoreReconciliationIntent("amend",in));
        a.clearSession(); QVERIFY(!a.restoreReconciliationIntent("cancel",in));
    }
    void restoredReceiptBindingIsBoundedButIdenticalRestoreRemainsIdempotent() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto in=intent(); in.orderId=reference();
        for(int i=0;i<1000;++i) {
            in.operationOrderId=reference(QString::number(i+1000).rightJustified(10,'0'));
            QVERIFY(a.restoreReconciliationIntent("cancel",in));
        }
        QVERIFY(a.restoreReconciliationIntent("cancel",in));
        in.operationOrderId=reference("0000009999"); QVERIFY(!a.restoreReconciliationIntent("cancel",in));
    }
    void restoredReceiptStillRequiresServerParentAndExactAmendTerms() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto in=intent();
        // The original and child branches deliberately differ. No branch is
        // inferred from the child row when restoring the persisted relationship.
        in.orderId=KisAdapter::orderReference("20261019","00002","0000000011","KRX");
        in.operationOrderId=reference("0000000033"); in.price="70100"; in.quantity="5";
        auto step=a.directStep(Operation::History,in); QString error; const auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error); QVERIFY(error.isEmpty());
        auto response=mutate(req,[](QJsonObject& root){
            auto original=kisOrder(now()); original["ord_gno_brno"]="00002";
            auto child=kisOrder(now(),"0000000033","5","0","5"); child["orgn_odno"]="0000000011"; child["ord_unpr"]="70100";
            root["output1"]=QJsonArray{original,child};
        });
        auto before=a.parseStep(step,response); QVERIFY(before.value.ok); QCOMPARE(before.value.orders.size(),2);
        QVERIFY(before.value.orders[1].originalOrderId.isEmpty()); QVERIFY(!a.reconcile("amend",in,before.value.orders).resolved);
        QVERIFY(a.restoreReconciliationIntent("amend",in)); QVERIFY(!a.reconcile("amend",in,{}).resolved);
        auto after=a.parseStep(step,response); QVERIFY(after.value.ok); QCOMPARE(after.value.orders[1].originalOrderId,in.orderId);
        QVERIFY(a.reconcile("amend",in,after.value.orders).resolved);
        auto wrongTerms=in; wrongTerms.quantity="6"; QVERIFY(!a.reconcile("amend",wrongTerms,after.value.orders).resolved);
        auto raw=QJsonDocument::fromJson(response.body).object(); auto rows=raw["output1"].toArray(); auto child=rows[1].toObject();
        child["orgn_odno"]="0000000044"; rows[1]=child; raw["output1"]=rows; auto wrong=kisJson(raw);
        const auto mismatch=a.parseStep(step,wrong); QVERIFY(mismatch.value.ok); QVERIFY(mismatch.value.orders[1].originalOrderId.isEmpty());
        QVERIFY(!a.reconcile("amend",in,mismatch.value.orders).resolved);
        a.clearSession(); QVERIFY(ready(a));
        QVERIFY(a.parseStep(step,response).value.orders[1].originalOrderId.isEmpty());
        QVERIFY(a.restoreReconciliationIntent("amend",in)); QVERIFY(a.reconcile("amend",in,a.parseStep(step,response).value.orders).resolved);
    }
    void explicitReferenceQueryMatchesOnlyExactServerTuple() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto in=intent(); in.orderId=reference(); auto s=a.directStep(Operation::QueryOrder,in); QString error; auto req=a.buildStep(s,now().toMSecsSinceEpoch(),&error);
        QVERIFY(error.isEmpty()); const QUrlQuery q(req.url); QCOMPARE(q.queryItemValue("ODNO"),"0000000011"); QCOMPARE(q.queryItemValue("ORD_GNO_BRNO"),"00001");
        auto p=a.parseStep(s,kisResponse(req,now())); QVERIFY(p.value.ok); QCOMPARE(p.value.orders.size(),1); QCOMPARE(p.value.orders.first().orderId,in.orderId);
        auto wrong=mutate(req,[](QJsonObject& root){auto rows=root["output1"].toArray();auto row=rows.first().toObject();row["ord_gno_brno"]="00002";rows[0]=row;root["output1"]=rows;});
        auto missing=a.parseStep(s,wrong); QVERIFY(missing.value.ok); QVERIFY(missing.value.notFound); QVERIFY(missing.value.orders.isEmpty());
    }
    void cancelPreflightMatchesAccountWidePossibleQuantity() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto all=evidence(a,{},BrokerPlanKind::CancelPreflight); QVERIFY(!all.isEmpty()); auto in=intent(); in.orderId=reference();
        QVERIFY(a.actionReadiness(BrokerAction::Cancel,in,all,limits()).ready);
        in.orderId=KisAdapter::orderReference("20261018","00001","0000000011","KRX"); QVERIFY(!a.actionReadiness(BrokerAction::Cancel,in,all,limits()).ready);
    }

    void partiallyFilledOrderCanRepriceOnlyTheAvailablePortion() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QVERIFY(catalog(a)); auto in=intent(); in.orderId=reference();in.price="70100";in.quantity="3";
        auto all=evidence(a,in);
        for(auto& e:all) if(e.variant=="amendable-orders") {
            auto rows=e.data["possibleOrders"].toArray();auto r=rows.first().toObject();r["executed"]="2";r["possibleQuantity"]="8";rows[0]=r;e.data["possibleOrders"]=rows;
        }
        auto ready=a.actionReadiness(BrokerAction::Amend,in,all,limits()); QVERIFY2(ready.ready,qPrintable(ready.message));
        in.quantity="9";QVERIFY(!a.actionReadiness(BrokerAction::Amend,in,all,limits()).ready);
    }
    void amendRequiresFreshPossibleOrdersEvenWhenOtherEvidenceIsFresh() {
        KisAdapter a("live",now); QVERIFY(ready(a)); QVERIFY(catalog(a)); auto in=intent(); in.orderId=reference(); in.price="70100";
        bool ruleFound=false;
        for(const auto& rule:a.policy().freshness.value(BrokerAction::Amend)) if(rule.operation==Operation::OpenOrders) {
            ruleFound=true; QCOMPARE(rule.maxReceiptAgeMs,10000); QVERIFY(rule.requiredForAction);
        }
        QVERIFY(ruleFound); auto all=evidence(a,in);
        // The controller removes expired evidence under the above policy before calling the adapter.
        all.erase(std::remove_if(all.begin(),all.end(),[](const auto& e){return e.variant=="amendable-orders";}),all.end());
        QVERIFY(!a.actionReadiness(BrokerAction::Amend,in,all,limits()).ready);
    }




    void reconciliationRejectsWrongTermsAndNeedsOriginalForChanges() {
        KisAdapter a("live",now); QVERIFY(ready(a));auto in=intent();in.orderId=reference(); auto step=a.directStep(Operation::QueryOrder,in);QString error;auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        auto records=a.parseStep(step,kisResponse(req,now())).value.orders;QCOMPARE(records.size(),1);QVERIFY(a.reconcile("place",in,records).resolved);
        auto wrong=in;wrong.quantity="11";QVERIFY(!a.reconcile("place",wrong,records).resolved);wrong=in;wrong.side="sell";QVERIFY(!a.reconcile("place",wrong,records).resolved);
        auto changed=records.first();changed.orderId=reference("0000000022");changed.originalOrderId=in.orderId;changed.price="70100";changed.quantity="5";in.operationOrderId=changed.orderId;in.price="70100";in.quantity="5";
        QVERIFY(!a.reconcile("amend",in,{changed}).resolved);QVERIFY(a.reconcile("amend",in,{records.first(),changed}).resolved);
        changed.originalOrderId=reference("0000000044");QVERIFY(!a.reconcile("amend",in,{records.first(),changed}).resolved);
    }
    void cancellationReconcilesAcceptedZeroLexicalFormsWithoutChangingIds() {
        KisAdapter a("live",now); QVERIFY(ready(a)); auto in=intent();in.orderId=reference();in.operationOrderId=reference("0000000033");
        QVERIFY(a.restoreReconciliationIntent("cancel",in));auto step=a.directStep(Operation::History,in);QString error;auto req=a.buildStep(step,now().toMSecsSinceEpoch(),&error);
        auto h=mutate(req,[](QJsonObject& root){
            auto original=kisOrder(now(),"0000000011","10","2","00");original["cncl_yn"]="Y";original["cncl_cfrm_qty"]="8";
            auto child=kisOrder(now(),"0000000033","8","0","00");child["orgn_odno"]="0000000011";child["cncl_yn"]="Y";child["cncl_cfrm_qty"]="8";
            root["output1"]=QJsonArray{original,child};
        });
        auto p=a.parseStep(step,h);QVERIFY(p.value.ok);QCOMPARE(p.value.orders.size(),2);QCOMPARE(p.value.orders[0].remaining,"00");
        QCOMPARE(p.value.orders[0].orderId,in.orderId);QCOMPARE(p.value.orders[1].orderId,in.operationOrderId);
        const auto result=a.reconcile("cancel",in,p.value.orders);QVERIFY(result.resolved);QVERIFY(result.applied);
        auto nonzero=p.value.orders;nonzero[0].remaining="01";QVERIFY(!a.reconcile("cancel",in,nonzero).resolved);
    }
};
QTEST_GUILESS_MAIN(KisTests)
#include "tst_kis.moc"
