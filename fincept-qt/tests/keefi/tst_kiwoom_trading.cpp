#include "keefi/BrokerTradingController.h"
#include "keefi/KiwoomAdapter.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/KiwoomResponses.h"
#include "fixtures/BrokerReadOnlyAssertions.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <sodium.h>
using namespace keefi;
using namespace keefi::test;
namespace {
const QString Password = "Fake Kiwoom vault password 2026!";
QString reference(const QString& number) { return KiwoomAdapter::orderReference("20261019", "KRX", number); }
OrderInput orderInput() { OrderInput i; i.symbol="005930"; i.side="buy"; i.type="limit"; i.price="70000"; i.quantity="10"; return i; }
bool idle(BrokerTradingController& c, int timeout=20000) {
    QElapsedTimer timer; timer.start(); while (c.busy() && timer.elapsed()<timeout) QTest::qWait(10); return !c.busy();
}
bool requestReady(BrokerTradingController& c, int timeout=5000) {
    QElapsedTimer timer; timer.start(); while (!c.requestReadiness().ready && timer.elapsed()<timeout) QTest::qWait(10); return c.requestReadiness().ready;
}
bool draftReady(BrokerTradingController& c, const OrderInput& input, int timeout=12000) {
    c.setDraft(input); QElapsedTimer timer; timer.start();
    while (!c.draftReadiness().ready && timer.elapsed()<timeout) QTest::qWait(10);
    return c.draftReadiness().ready;
}
void activate(QPushButton& button) {
    button.show(); button.activateWindow(); QApplication::setActiveWindow(&button); button.setFocus(); QCoreApplication::processEvents();
}
class FakeTransport final : public TradingTransport {
public:
    QList<RequestSpec> requests;
    Vault* vault=nullptr;
    quint64 lastToken=0, heldToken=0;
    HttpResult heldResponse;
    bool holdBuyingPower=false, holdQueries=false, writeAheadObserved=false;
    std::function<HttpResult(const RequestSpec&,HttpResult)> overrideResponse;
    void abort() override {} // Intentional late-reply test seam; no real I/O.
    void send(quint64 token, const RequestSpec& request) override {
        requests.append(request); lastToken=token;
        auto response=kiwoomResponse(request);
        if (isMutation(request.operation)) {
            const auto rows=vault?vault->pendingOperations():QJsonArray{};
            writeAheadObserved=!rows.isEmpty() && rows.last().toObject().size()==23 &&
                rows.last().toObject().value("provider")=="kiwoom" && rows.last().toObject().value("state")=="pending";
        }
        if (overrideResponse) response=overrideResponse(request,response);
        if (holdBuyingPower && request.operation==Operation::BuyingPower) { heldToken=token; heldResponse=response; return; }
        if (holdQueries && request.operation==Operation::QueryOrder) return;
        QTimer::singleShot(0,this,[this,token,response]{ emit finished(token,response); });
    }
    int count(Operation op) const { return int(std::count_if(requests.cbegin(),requests.cend(),[op](const auto& r){return r.operation==op;})); }
    int mutationCount() const { return int(std::count_if(requests.cbegin(),requests.cend(),[](const auto& r){return isMutation(r.operation);})); }
};
struct Context {
    QTemporaryDir dir;
    Vault vault{dir.filePath("profile/fake-vault.keefi")};
    FakeTransport wire;
    BrokerTradingController controller{vault,&wire,nullptr,[](const AccountRecord& a){return std::make_unique<KiwoomAdapter>(a.environment,kiwoomNow);}};
    QString id;
    QString setupError;
    bool valid=false;
    Context() {
        wire.vault=&vault;
        AccountRecord a; a.provider="kiwoom"; a.alias="가짜 키움 연결"; a.market="domestic_equity"; a.environment="live"; a.currency="KRW";
        if (!dir.isValid()) { setupError = "temporary-directory"; return; }
        if (!vault.create(Password, &setupError)) { setupError.prepend("create-vault: "); return; }
        if (!vault.addAccount(a, {{"app_key","KIWOOM_FAKE_APP_KEY"},{"app_secret","KIWOOM_FAKE_SECRET"}},
                              StorageMode::EncryptedVault, &setupError)) {
            setupError.prepend("save-account: "); return;
        }
        valid = true;
        id = vault.accounts().first().id;
    }
    bool authenticate() {
        if (!controller.selectConnection(id)) return false;
        controller.authenticate();
        return idle(controller) && controller.authenticated() && controller.accountChoices().size()==1 &&
            controller.selectBrokerAccount(controller.accountChoices().first().handle);
    }
    bool prepare() {
        if (!authenticate()) return false;
        controller.loadMarkets(); if (!idle(controller) || !controller.catalogFresh()) return false;
        controller.refresh("005930");
        return idle(controller) && controller.connected() && requestReady(controller) && controller.setLimits({"10000000","KRW","5"});
    }
};
QJsonObject body(const RequestSpec& r) { return QJsonDocument::fromJson(r.body).object(); }
HttpResult replaceObject(HttpResult response,const QJsonObject& object) {
    response.body=QJsonDocument(object).toJson(QJsonDocument::Compact); return response;
}
}
class KiwoomTradingTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(sodium_init()>=0); }
    void catalogPaginationPreservesSources_data() {
        QTest::addColumn<bool>("auxiliaryFailure"); QTest::addColumn<bool>("duplicate");
        QTest::newRow("all-pages-and-empty-terminal-page")<<false<<false;
        QTest::newRow("auxiliary-failure-private-list-remains-readable")<<true<<false;
        QTest::newRow("duplicate-private-page-fails-closed")<<false<<true;
    }
    void catalogPaginationPreservesSources() {
        QFETCH(bool,auxiliaryFailure); QFETCH(bool,duplicate);
        Context c; QVERIFY2(c.valid, qPrintable(c.setupError)); int privatePages=0;
        c.wire.overrideResponse=[&](const RequestSpec& request,HttpResult response) {
            if (auxiliaryFailure && request.url.host()=="new.real.download.dws.co.kr" && request.url.path().contains("kospi")) {
                response.status=503; response.body="{}"; return response;
            }
            if (request.headers.value("api-id")=="ka10099" && body(request).value("mrkt_tp")=="0") {
                ++privatePages;
                auto object=QJsonDocument::fromJson(response.body).object(); const auto rows=object.value("list").toArray();
                object["list"]=privatePages==1?QJsonArray{rows.first()}:privatePages==2?QJsonArray{rows.at(duplicate?0:1)}:QJsonArray{};
                response=replaceObject(response,object);
                if (privatePages<3) response.headers={{"cont-yn","Y"},{"next-key",QByteArray("FAKE_PAGE_")+QByteArray::number(privatePages)}};
            }
            return response;
        };
        QVERIFY2(c.authenticate(),qPrintable(c.controller.status())); c.controller.loadMarkets(); QVERIFY(idle(c.controller));
        if (duplicate) { QVERIFY(!c.controller.catalogFresh()); QCOMPARE(privatePages,2); return; }
        QVERIFY2(c.controller.catalogFresh(),qPrintable(c.controller.status())); QCOMPARE(privatePages,3); QCOMPARE(c.controller.instruments().size(),4);
        QCOMPARE(c.controller.catalogSources().size(),2); QCOMPARE(c.wire.mutationCount(),0);
        for (const auto& value:c.controller.catalogSources()) {
            const auto source=value.toObject(); QVERIFY(source.value("url").toString().startsWith("https://new.real.download.dws.co.kr/"));
            QVERIFY(!source.value("httpDate").toString().isEmpty()); QVERIFY(!source.value("lastModified").toString().isEmpty());
            if (source.value("status")=="ok") { QCOMPARE(source.value("contentSha256").toString().size(),64); QCOMPARE(source.value("receivedAt").toString(),kiwoomNow().toString(Qt::ISODateWithMs)); }
            else { QVERIFY(auxiliaryFailure); QVERIFY(source.value("receivedAt").toString().isEmpty()); }
        }
        for (const auto& item:c.controller.instruments()) if (item.symbol=="005930") QCOMPARE(item.tradable,!auxiliaryFailure);
        c.controller.refresh("005930"); QVERIFY(idle(c.controller)); QVERIFY(c.controller.connected());
        QVERIFY(!c.controller.accountSummary().isEmpty()); QVERIFY(!c.controller.holdings().isEmpty());
        if (auxiliaryFailure) {
            QVERIFY(requestReady(c.controller)); QVERIFY(c.controller.setLimits({"10000000","KRW","5"}));
            c.controller.setDraft(orderInput()); QTRY_VERIFY_WITH_TIMEOUT(c.wire.count(Operation::Reference)>0,10000);
            QVERIFY(idle(c.controller)); QVERIFY(!c.controller.draftReadiness().ready); QCOMPARE(c.wire.mutationCount(),0);
        }
    }

    void referenceSourceAgeIsSeparateFromReceiptAge_data() {
        QTest::addColumn<int>("ageSeconds"); QTest::addColumn<bool>("ready");
        QTest::newRow("recent-received-one-minute-source")<<60<<true;
        QTest::newRow("recent-received-expired-source")<<121<<false;
    }
    void referenceSourceAgeIsSeparateFromReceiptAge() {
        QFETCH(int,ageSeconds); QFETCH(bool,ready); Context c; QVERIFY2(c.valid, qPrintable(c.setupError));
        c.wire.overrideResponse=[&](const RequestSpec& request,HttpResult response) {
            if (request.operation!=Operation::Reference) return response;
            return kiwoomJson({{"stk_cd","005930"},{"stk_min_pole_chart_qry",QJsonArray{QJsonObject{{"cntr_tm",kiwoomNow().addSecs(-ageSeconds).toTimeZone(QTimeZone("Asia/Seoul")).toString("yyyyMMddHHmmss")},{"cur_prc","+70000"},{"trde_qty","10"}}}}});
        };
        QVERIFY2(c.prepare(),qPrintable(c.controller.status())); c.controller.setDraft(orderInput());
        QTRY_VERIFY_WITH_TIMEOUT(c.wire.count(Operation::Reference)>0,10000); QVERIFY(idle(c.controller)); QVERIFY(requestReady(c.controller));
        QCOMPARE(c.controller.draftReadiness().ready,ready); QCOMPARE(c.wire.mutationCount(),0);
        if (ready) QVERIFY(c.controller.preview(orderInput()).ok);
    }
    void cancellationUsesAllHistoryPages_data() {
        QTest::addColumn<bool>("duplicate"); QTest::newRow("target-on-first-page")<<false; QTest::newRow("same-id-across-pages")<<true;
    }
    void cancellationUsesAllHistoryPages() {
        QFETCH(bool,duplicate); Context c; QVERIFY2(c.valid, qPrintable(c.setupError)); QVERIFY2(c.prepare(),qPrintable(c.controller.status()));
        int pages=0;
        c.wire.overrideResponse=[&](const RequestSpec& request,HttpResult response) {
            if (request.operation!=Operation::History) return response;
            ++pages; response=kiwoomJson({{"acnt_ord_cntr_prps_dtl",QJsonArray{kiwoomDetailOrder(pages==1||duplicate?"0000100":"0000101")}}});
            if (pages==1) response.headers={{"cont-yn","Y"},{"next-key","FAKE_DETAIL_PAGE"}};
            return response;
        };
        c.controller.prepareCancellation(); QVERIFY(idle(c.controller)); QCOMPARE(pages,2);
        if (duplicate) { QVERIFY(!c.controller.connected()); QVERIFY(!c.controller.cancellationReadiness(c.controller.openOrders().first()).ready); }
        else { QVERIFY(requestReady(c.controller)); QVERIFY2(c.controller.cancellationReadiness(c.controller.openOrders().first()).ready,qPrintable(c.controller.cancellationReadiness(c.controller.openOrders().first()).message)); }
        QCOMPARE(c.wire.mutationCount(),0);
    }


};
QTEST_MAIN(KiwoomTradingTests)
#include "tst_kiwoom_trading.moc"
