#include "keefi/BrokerTradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/TossAdapter.h"
#include "keefi/KiwoomAdapter.h"
#include "keefi/KiwoomRequestBudget.h"
#include "fixtures/KiwoomResponses.h"
#include "fixtures/TossResponses.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <functional>
#include <algorithm>
#include <sodium.h>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace keefi;
using namespace keefi::test;
namespace {
const QString Password = "Fake broker vault password 2026!";
OrderInput inputOrder() { OrderInput i; i.symbol = "005930"; i.side = "buy"; i.type = "limit"; i.price = "70000"; i.quantity = "10"; return i; }
bool addConnection(Vault& vault) {
    AccountRecord a; a.provider = "toss"; a.alias = "가짜 토스 연결"; a.market = "domestic_equity"; a.environment = "live"; a.currency = "KRW";
    return vault.addAccount(a, {{"client_id", "BROKER_FAKE_CLIENT_ID"}, {"client_secret", "BROKER_FAKE_CLIENT_SECRET"}}, StorageMode::EncryptedVault);
}
bool idle(BrokerTradingController& c, int timeout = 15000) {
    QElapsedTimer elapsed; elapsed.start();
    while (c.busy() && elapsed.elapsed() < timeout) QTest::qWait(10);
    return !c.busy();
}
bool requestReady(BrokerTradingController& controller, int timeout = 5000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!controller.requestReadiness().ready && elapsed.elapsed() < timeout) QTest::qWait(10);
    return controller.requestReadiness().ready;
}
class FakeBrokerTransport final : public TradingTransport {
  public:
    using TradingTransport::TradingTransport;
    Vault* vault = nullptr;
    QList<Operation> sent;
    QList<qint64> sendTimes;
    QList<QString> catalogMarkets, queriedIds, canceledIds;
    QMap<QString, QJsonObject> orders;
    QElapsedTimer clock;
    quint64 epoch = 0, lastToken = 0;
    int aborts = 0;
    bool timeoutMutation = false, holdMutation = false, pendingAmend = false, changedScope = false;
    bool writeAheadObserved = true, localBlockMutation = false;
    QString failCatalog;
    std::function<void(Operation)> beforeResponse;
    std::function<HttpResult(Operation, const OrderInput&, HttpResult)> overrideResponse;
    void abort() override { ++epoch; ++aborts; }
    void send(quint64 token, const RequestSpec& request) override {
        if (!clock.isValid()) clock.start();
        sent.append(request.operation); sendTimes.append(clock.elapsed()); lastToken = token;
        if(isMutation(request.operation)&&localBlockMutation){
            writeAheadObserved=vault&&!vault->pendingOperations().isEmpty();
            HttpResult result;result.status=429;result.localRetryAfterSeconds=90;result.headers["Retry-After"]="90";
            QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});return;
        }
        OrderInput input = inputOrder();
        const QUrlQuery query(request.url); const auto body = QJsonDocument::fromJson(request.body).object();
        if (query.hasQueryItem("symbols")) input.symbol = query.queryItemValue("symbols");
        if (query.hasQueryItem("symbol")) input.symbol = query.queryItemValue("symbol");
        input.cursor = query.queryItemValue("market");
        if (request.operation == Operation::PlaceOrder || request.operation == Operation::AmendOrder) {
            input.type = body.value("orderType").toString().toLower(); input.price = body.value("price").toString();
            input.quantity = body.value("quantity").toString(); input.clientId = body.value("clientOrderId").toString();
            if (request.operation == Operation::PlaceOrder) { input.symbol = body.value("symbol").toString(); input.side = body.value("side").toString().toLower(); }
        }
        if (request.operation == Operation::QueryOrder || request.operation == Operation::CancelOrder || request.operation == Operation::AmendOrder) {
            input.orderId = request.url.path().split('/', Qt::SkipEmptyParts).value(3);
            if (orders.contains(input.orderId)) {
                const auto original = orders.value(input.orderId); input.symbol = original.value("symbol").toString();
                input.side = original.value("side").toString().toLower();
                if (request.operation != Operation::AmendOrder) {
                    input.type = original.value("orderType").toString().toLower(); input.price = original.value("price").toString(); input.quantity = original.value("quantity").toString();
                }
            }
        }
        HttpResult response = tossResponse(request.operation, input);
        if (request.operation == Operation::Markets) {
            catalogMarkets.append(input.cursor);
            QJsonArray rows;
            if (input.cursor != "KR_ETC") rows.append(QJsonObject{{"symbol", input.cursor == "KOSPI" ? "005930" : "035900"},
                {"name", "가짜 종목"}, {"securityType", "STOCK"}, {"isCommonShare", true}, {"isinCode", "KR0000000000"}});
            response = tossEnvelope(rows);
            if (input.cursor == failCatalog) { response.status = 503; response.body = R"({"error":{"code":"temporary-unavailable"}})"; }
        }
        if (request.operation == Operation::OpenOrders) {
            if (changedScope) { orders.insert("unreviewed-order", tossOrder("unreviewed-order")); changedScope = false; }
            QJsonArray rows;
            for (const auto& row : orders) {
                const auto status = row.value("status").toString();
                if (status == "PENDING" || status == "PARTIAL_FILLED" || status == "PENDING_CANCEL" || status == "PENDING_REPLACE") rows.append(row);
            }
            response = tossEnvelope(QJsonObject{{"orders", rows}, {"hasNext", false}, {"nextCursor", QJsonValue::Null}});
        }
        if (isMutation(request.operation)) {
            const auto journal = vault ? vault->pendingOperations() : QJsonArray{};
            bool currentIntentPersisted = false;
            for (const auto& value : journal) {
                const auto entry = value.toObject();
                const bool sameRequest = request.operation == Operation::PlaceOrder ? entry.value("clientId").toString() == input.clientId : entry.value("orderId").toString() == input.orderId;
                if (sameRequest && entry.size() == 20 && entry.value("state") == "pending" &&
                    !entry.value("accountScope").toString().isEmpty()) currentIntentPersisted = true;
            }
            writeAheadObserved = writeAheadObserved && currentIntentPersisted;
            if (request.operation == Operation::PlaceOrder) {
                auto row = tossOrder("new-order-001", "PENDING", input.symbol, input.quantity);
                row["side"] = input.side.toUpper(); row["orderType"] = input.type.toUpper();
                row["price"] = input.price.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(input.price); orders.insert("new-order-001", row);
            } else {
                const bool cancel = request.operation == Operation::CancelOrder;
                auto original = orders.value(input.orderId);
                original["status"] = cancel ? "CANCELED" : pendingAmend ? "PENDING_REPLACE" : "REPLACED";
                if (cancel) original["canceledAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                orders[input.orderId] = original;
                const QString operationId = (cancel ? "cancel-" : "amend-") + input.orderId;
                auto operation = tossOrder(operationId, cancel ? "CANCELED" : pendingAmend ? "PENDING_REPLACE" : "PENDING", input.symbol, input.quantity);
                operation["side"] = input.side.toUpper(); operation["orderType"] = input.type.toUpper(); operation["price"] = input.price;
                orders.insert(operationId, operation); response = tossEnvelope(QJsonObject{{"orderId", operationId}});
                if (cancel) canceledIds.append(input.orderId);
            }
            if (holdMutation) return;
            if (timeoutMutation) { response = {}; response.networkError = response.timedOut = true; }
        }
        if (request.operation == Operation::QueryOrder) {
            queriedIds.append(input.orderId);
            if (orders.contains(input.orderId)) response = tossEnvelope(orders.value(input.orderId));
            else { response.status = 404; response.body = R"({"error":{"code":"not-found"}})"; }
        }
        if (overrideResponse) response = overrideResponse(request.operation, input, response);
        const auto generation = epoch;
        QTimer::singleShot(1, this, [this, generation, token, response, operation = request.operation] {
            if (generation != epoch) return;
            if (beforeResponse) beforeResponse(operation);
            if (generation == epoch) emit finished(token, response);
        });
    }
};
bool authenticate(BrokerTradingController& controller, const QString& connectionId, bool select = true) {
    if (!controller.selectConnection(connectionId)) return false;
    controller.authenticate();
    if (!idle(controller) || !controller.authenticated() || controller.accountChoices().size() != 1) return false;
    return !select || controller.selectBrokerAccount(controller.accountChoices().first().handle);
}
bool refresh(BrokerTradingController& controller) {
    controller.refresh("005930");
    if (!idle(controller) || !controller.connected()) return false;
    return requestReady(controller);
}


bool addKiwoomBudgetConnection(Vault& vault) {
    AccountRecord account;account.provider="kiwoom";account.alias="FAKE SHARED BUDGET";account.market="domestic_equity";account.environment="live";account.currency="KRW";
    return vault.addAccount(account,{{"app_key","FAKE_SHARED_KIWOOM_KEY"},{"app_secret","FAKE_SHARED_KIWOOM_SECRET"}},StorageMode::EncryptedVault);
}
BrokerTradingController::AdapterFactory kiwoomBudgetFactory() {
    return [](const AccountRecord& account)->std::unique_ptr<BrokerAdapter>{return std::make_unique<KiwoomAdapter>(account.environment,kiwoomNow);};
}
class FakeKiwoomBudgetTransport final : public TradingTransport {
  public:
    QList<Operation> sent;RequestSpec last;quint64 lastToken=0;int aborts=0;
    bool autoRespond=false,synchronousAbort=false;
    std::function<void()> onAbort;
    void send(quint64 token,const RequestSpec& request) override {
        sent.append(request.operation);last=request;lastToken=token;
        if(autoRespond){const auto response=kiwoomResponse(request);QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});}
    }
    void abort() override {++aborts;if(onAbort)onAbort();if(synchronousAbort&&lastToken)emit finished(lastToken,kiwoomResponse(last));}
    void reply(const HttpResult& response) {emit finished(lastToken,response);}
};
}

class BrokerTradingTests final : public QObject {
    Q_OBJECT
  private slots:
    void authenticationAndReadNeverImplyOrderPermission() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addConnection(vault));
        FakeBrokerTransport transport; transport.vault = &vault; BrokerTradingController controller(vault, &transport);
        QVERIFY(controller.selectConnection(vault.accounts().first().id)); QVERIFY(transport.sent.isEmpty());
        controller.loadMarkets(); QVERIFY(transport.sent.isEmpty());
        controller.authenticate(); QVERIFY(idle(controller)); QVERIFY(controller.authenticated()); QVERIFY(!controller.connected()); QVERIFY(!controller.liveEnabled());
        QCOMPARE(transport.sent, (QList<Operation>{Operation::Authenticate, Operation::Accounts}));
        const auto choice = controller.accountChoices().first(); QVERIFY(choice.label.endsWith("1234")); QVERIFY(!choice.label.contains("00000001234"));
        const auto beforeSelection = transport.sent;
        QVERIFY(controller.selectBrokerAccount(choice.handle)); QCOMPARE(transport.sent, beforeSelection);
        QVERIFY(refresh(controller)); QVERIFY(controller.quoteFresh()); QVERIFY(!controller.liveEnabled());
        QVERIFY(controller.coverage().contains("/api/v1/holdings")); QVERIFY(controller.coverage().contains("/api/v1/buying-power")); QVERIFY(controller.coverage().contains("수신"));
        QVERIFY(controller.quoteSummary().contains("공식 시세 시각")); QVERIFY(controller.permissionSummary().contains("조회 완료"));
        QVERIFY(!transport.sent.contains(Operation::TestOrder));
        const auto receipt = QJsonDocument(controller.verificationReceipt()).toJson();
        for (const auto* secret : {"BROKER_FAKE_CLIENT_ID", "BROKER_FAKE_CLIENT_SECRET", "fakeheader", "00000001234", "7391", "70000", "history-order"}) QVERIFY(!receipt.contains(secret));
        QCOMPARE(controller.verificationReceipt().value("transport").toString(), QString("synthetic_test"));
        QVERIFY(!controller.verificationReceipt().value("official_order_test_supported").toBool());
    }
    void initTestCase() { QVERIFY(sodium_init() >= 0); }
    void kiwoomBorrowReusesAuthenticationWithoutAddingBrokerJobs() {
        QTemporaryDir directory;Vault vault(directory.filePath("synthetic-kiwoom-borrow"));QVERIFY(vault.create(Password));QVERIFY(addKiwoomBudgetConnection(vault));
        FakeKiwoomBudgetTransport wire;wire.autoRespond=true;auto gate=std::make_shared<KiwoomRequestBudget>();
        BrokerTradingController broker(vault,&wire,nullptr,kiwoomBudgetFactory(),gate);broker.setAutomaticRefresh(false);
        QVERIFY(authenticate(broker,vault.accounts().first().id,false));QCOMPARE(wire.sent.size(),2);
        auto before=broker.verificationReceipt();before.remove("exported_at_utc");const auto identity=broker.credentialSessionIdentity();int calls=0;quint64 revision=0;
        QVERIFY(broker.withMarketAuthorization([&](QByteArrayView token,qint64 remaining,quint64 current){++calls;QVERIFY(token==QByteArrayView("fake-token.only_for_test"));QVERIFY(remaining>0);revision=current;}));
        auto after=broker.verificationReceipt();after.remove("exported_at_utc");QVERIFY(revision);QCOMPARE(calls,1);QCOMPARE(wire.sent.size(),2);QCOMPARE(after,before);QCOMPARE(broker.credentialSessionIdentity(),identity);
        vault.lock();QVERIFY(!broker.withMarketAuthorization([&](QByteArrayView,qint64,quint64){++calls;}));QCOMPARE(calls,1);QCOMPARE(wire.sent.size(),2);
    }
    void kiwoomExistingControllersShareTheActualDispatchLease() {
        QTemporaryDir directory;Vault firstVault(directory.filePath("synthetic-first-budget")),secondVault(directory.filePath("synthetic-second-budget"));
        QVERIFY(firstVault.create(Password));QVERIFY(secondVault.create(Password));QVERIFY(addKiwoomBudgetConnection(firstVault));QVERIFY(addKiwoomBudgetConnection(secondVault));
        qint64 now=0;auto gate=std::make_shared<KiwoomRequestBudget>([&]{return now;});FakeKiwoomBudgetTransport firstWire,secondWire;firstWire.synchronousAbort=true;
        BrokerTradingController first(firstVault,&firstWire,nullptr,kiwoomBudgetFactory(),gate),second(secondVault,&secondWire,nullptr,kiwoomBudgetFactory(),gate);
        first.setAutomaticRefresh(false);second.setAutomaticRefresh(false);QVERIFY(first.selectConnection(firstVault.accounts().first().id));QVERIFY(second.selectConnection(secondVault.accounts().first().id));
        first.authenticate();QCOMPARE(firstWire.sent.size(),1);second.authenticate();QCOMPARE(secondWire.sent.size(),0);QVERIFY(second.busy());
        first.stop();QCOMPARE(firstWire.sent.size(),1);now=400;QTRY_COMPARE_WITH_TIMEOUT(secondWire.sent.size(),1,2000);
        firstWire.reply(kiwoomResponse(firstWire.last));now=800;QObject display;QCOMPARE(gate->tryAcquire(&display,KiwoomBudgetLane::Display).lease,quint64(0));
        second.stop();const auto next=gate->tryAcquire(&display,KiwoomBudgetLane::Display);QVERIFY(next.lease);QVERIFY(gate->finish(next.lease));
        QCOMPARE(firstWire.sent,QList<Operation>{Operation::Authenticate});QCOMPARE(secondWire.sent,QList<Operation>{Operation::Authenticate});
    }
    void kiwoomBusinessLimitFromAnExistingJobPausesTheDisplayLane() {
        QTemporaryDir directory;Vault vault(directory.filePath("synthetic-rate-budget"));QVERIFY(vault.create(Password));QVERIFY(addKiwoomBudgetConnection(vault));
        qint64 now=0;auto gate=std::make_shared<KiwoomRequestBudget>([&]{return now;});FakeKiwoomBudgetTransport wire;
        BrokerTradingController broker(vault,&wire,nullptr,kiwoomBudgetFactory(),gate);broker.setAutomaticRefresh(false);QVERIFY(broker.selectConnection(vault.accounts().first().id));
        broker.authenticate();QCOMPARE(wire.sent,QList<Operation>{Operation::Authenticate});wire.reply(kiwoomJson({{"return_code",1700}}));
        QObject display;const auto blocked=gate->tryAcquire(&display,KiwoomBudgetLane::Display);QCOMPARE(blocked.lease,quint64(0));QVERIFY(blocked.waitMs>=1000);
        now=999;QVERIFY(gate->remainingMs()>0);now=1000;const auto allowed=gate->tryAcquire(&display,KiwoomBudgetLane::Display);QVERIFY(allowed.lease);QVERIFY(gate->finish(allowed.lease));
        QCOMPARE(wire.sent,QList<Operation>{Operation::Authenticate});QVERIFY(!broker.authenticated());
    }
    void kiwoomStopReleasesOnlyItsOwnLeaseAfterSynchronousAbort() {
        QTemporaryDir directory;Vault vault(directory.filePath("synthetic-abort-budget"));QVERIFY(vault.create(Password));QVERIFY(addKiwoomBudgetConnection(vault));
        qint64 now=0;auto gate=std::make_shared<KiwoomRequestBudget>([&]{return now;});FakeKiwoomBudgetTransport wire;wire.synchronousAbort=true;
        BrokerTradingController broker(vault,&wire,nullptr,kiwoomBudgetFactory(),gate);broker.setAutomaticRefresh(false);QVERIFY(broker.selectConnection(vault.accounts().first().id));
        broker.authenticate();QCOMPARE(wire.sent,QList<Operation>{Operation::Authenticate});QObject otherBroker,display;bool observedAbort=false;quint64 grantInsideAbort=0;
        wire.onAbort=[&]{observedAbort=true;grantInsideAbort=gate->tryAcquire(&otherBroker,KiwoomBudgetLane::ExistingBroker).lease;};
        now=400;broker.stop();wire.onAbort={};QVERIFY(observedAbort);QCOMPARE(grantInsideAbort,quint64(0));QVERIFY(!broker.busy());QVERIFY(!broker.authenticated());
        const auto displayGrant=gate->tryAcquire(&display,KiwoomBudgetLane::Display);QVERIFY(displayGrant.lease);
        wire.reply(kiwoomResponse(wire.last));now=800;QCOMPARE(gate->tryAcquire(&otherBroker,KiwoomBudgetLane::ExistingBroker).lease,quint64(0));
        QVERIFY(gate->finish(displayGrant.lease));const auto replacement=gate->tryAcquire(&otherBroker,KiwoomBudgetLane::ExistingBroker);QVERIFY(replacement.lease);QVERIFY(replacement.lease!=displayGrant.lease);
        QCOMPARE(gate->tryAcquire(&display,KiwoomBudgetLane::Display).lease,quint64(0));QVERIFY(gate->finish(replacement.lease));QCOMPARE(wire.sent,QList<Operation>{Operation::Authenticate});QVERIFY(vault.pendingOperations().isEmpty());
    }
    void brokerContractPreservesTossAuthenticationAndReadBoundaries() {
        TossAdapter adapter; AccountRecord account; account.provider = "toss"; account.environment = "live";
        QVERIFY(!adapter.policy().partialFillAmend);
        account.market = "domestic_equity"; account.currency = "KRW";
        CredentialSet credentials;
        credentials.emplace("client_id", SecureBuffer::fromUtf8("CONTRACT_FAKE_CLIENT"));
        credentials.emplace("client_secret", SecureBuffer::fromUtf8("CONTRACT_FAKE_SECRET"));
        QString error;
        const auto legacyAuth = adapter.build(Operation::Authenticate, account, {}, credentials, 0, &error);
        QVERIFY(error.isEmpty()); QVERIFY(adapter.configureSession(account, credentials, &error));
        const auto plan = adapter.plan(BrokerPlanKind::Authenticate, {}); QCOMPARE(plan.size(), 2);
        QCOMPARE(plan[0].authScope, BrokerAuthScope::CredentialBootstrap); QCOMPARE(plan[1].authScope, BrokerAuthScope::Session);
        const auto request = adapter.buildStep(plan[0], 0, &error);
        QCOMPARE(request.url, legacyAuth.url); QCOMPARE(request.body, legacyAuth.body); QCOMPARE(request.headers, legacyAuth.headers);
        const auto authenticated = adapter.parseStep(plan[0], tossResponse(Operation::Authenticate));
        QVERIFY(authenticated.value.ok); QVERIFY(!authenticated.accountReadVerified);
        const auto accounts = adapter.parseStep(plan[1], tossResponse(Operation::Accounts));
        QVERIFY(accounts.value.ok); QVERIFY(!accounts.accountReadVerified); QVERIFY(accounts.value.data.isEmpty());
        QVERIFY(adapter.selectBrokerAccount(adapter.accountChoices().first().handle, &error));
        auto holdingsStep = adapter.plan(BrokerPlanKind::AccountRead, inputOrder());
        const auto holding = std::find_if(holdingsStep.begin(), holdingsStep.end(), [](const auto& step) { return step.operation == Operation::Holdings; });
        QVERIFY(holding != holdingsStep.end()); QVERIFY(adapter.parseStep(*holding, tossResponse(Operation::Holdings)).accountReadVerified);
        QVERIFY(holdingsStep.first().input.endTimeMs > 0);
        QCOMPARE(holdingsStep.first().input.endTimeMs - holdingsStep.first().input.startTimeMs, 7LL * 86400000);
        for (const auto& step : holdingsStep) {
            QCOMPARE(step.input.startTimeMs, holdingsStep.first().input.startTimeMs);
            QCOMPARE(step.input.endTimeMs, holdingsStep.first().input.endTimeMs);
        }
        auto boundedRead = inputOrder(); boundedRead.endTimeMs = 1792108800000; boundedRead.startTimeMs = boundedRead.endTimeMs - 86400000;
        for (const auto& step : adapter.plan(BrokerPlanKind::AccountRead, boundedRead)) {
            QCOMPARE(step.input.startTimeMs, boundedRead.startTimeMs); QCOMPARE(step.input.endTimeMs, boundedRead.endTimeMs);
        }
        const auto catalog = adapter.plan(BrokerPlanKind::Catalog, {}); QCOMPARE(catalog.size(), 3);
        QCOMPARE(catalog[0].catalogMarket, QStringLiteral("KOSPI")); QCOMPARE(catalog[1].catalogMarket, QStringLiteral("KOSDAQ"));
        QCOMPARE(catalog[2].catalogMarket, QStringLiteral("KR_ETC")); QVERIFY(catalog[2].allowEmptyCatalog);
        for (const auto& step : catalog) { QCOMPARE(step.minimumDelayMs, 1100); QCOMPARE(step.authScope, BrokerAuthScope::Session); }
        QVERIFY(!adapter.policy().exactIntentPreflight); QVERIFY(!adapter.policy().publicCatalog);
        QVERIFY(adapter.plan(BrokerPlanKind::OrderPreflight, inputOrder()).isEmpty());
        QVERIFY(adapter.plan(BrokerPlanKind::CancelPreflight, {}).isEmpty());
        adapter.clearSession(); QVERIFY(!adapter.sessionReady()); QVERIFY(adapter.selectedAccountScope().isEmpty());
    }
    void stoppedSessionDiscardsReadinessTimerAndLateResponse() {
        QTemporaryDir dir; Vault vault(dir.filePath("fake-pacing-session.bin")); QVERIFY(vault.create(Password)); QVERIFY(addConnection(vault));
        FakeBrokerTransport wire; BrokerTradingController controller(vault, &wire);
        QVERIFY(controller.selectConnection(vault.accounts().first().id));
        controller.authenticate(); QVERIFY(idle(controller)); QVERIFY(controller.authenticated());
        const auto lateToken = wire.lastToken; const auto lateOperation = wire.sent.last();
        controller.stop(); const auto sent = wire.sent.size(); QSignalSpy changes(&controller, &BrokerTradingController::changed);
        emit wire.finished(lateToken, tossResponse(lateOperation));
        QTest::qWait(500); // Beyond the old response's 300 ms pacing deadline.
        QCOMPARE(changes.size(), 0); QCOMPARE(wire.sent.size(), sent);
        QCOMPARE(controller.requestReadiness().reason, BrokerNotReady::NotAuthenticated);
        QVERIFY(!controller.busy()); QVERIFY(!controller.liveEnabled()); QCOMPARE(wire.sent.count(Operation::PlaceOrder), 0);
    }




    void allThreeMarketsMustSucceedBeforeCatalogBecomesFresh() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addConnection(vault));
        FakeBrokerTransport transport; BrokerTradingController controller(vault, &transport);
        QVERIFY(authenticate(controller, vault.accounts().first().id, false)); controller.loadMarkets(); QVERIFY(!controller.catalogFresh());
        QVERIFY(idle(controller)); QCOMPARE(transport.catalogMarkets, (QStringList{"KOSPI", "KOSDAQ", "KR_ETC"}));
        QVERIFY(controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 2);
        QCOMPARE(controller.instruments().at(0).market, QString("KOSPI")); QCOMPARE(controller.instruments().at(1).market, QString("KOSDAQ"));
        QVERIFY(controller.catalogStatus().contains("/api/v1/stocks/all")); QVERIFY(controller.catalogStatus().contains("수신")); QVERIFY(!controller.connected());
        QList<qint64> marketTimes;
        for (int i = 0; i < transport.sent.size(); ++i) if (transport.sent[i] == Operation::Markets) marketTimes.append(transport.sendTimes[i]);
        QVERIFY(marketTimes[1] - marketTimes[0] >= 1100); QVERIFY(marketTimes[2] - marketTimes[1] >= 1100);
        transport.failCatalog = "KOSDAQ"; controller.loadMarkets(); QVERIFY(!controller.catalogFresh()); QVERIFY(idle(controller));
        QVERIFY(!controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 2); QVERIFY(controller.catalogStatus().contains("조회 실패"));
        QCOMPARE(transport.catalogMarkets.count("KR_ETC"), 1); // Failure stops the remaining catalogue request.
    }











    void authenticationFailureOrExpiryClearsDataWithoutReissuingToken_data() {
        QTest::addColumn<bool>("expire");
        QTest::newRow("http-401") << false;
        QTest::newRow("token-expiry") << true;
    }
    void authenticationFailureOrExpiryClearsDataWithoutReissuingToken() {
        QFETCH(bool, expire);
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addConnection(vault));
        FakeBrokerTransport transport; transport.vault = &vault;
        if (expire) transport.overrideResponse = [](Operation op, const OrderInput&, HttpResult response) {
            if (op == Operation::Authenticate) response.body = R"({"access_token":"fakeheader.fakepayload.fakesignature","token_type":"Bearer","expires_in":9})";
            return response;
        };
        BrokerTradingController controller(vault, &transport);
        QVERIFY(authenticate(controller, vault.accounts().first().id)); QVERIFY(refresh(controller)); QVERIFY(!controller.holdings().isEmpty());
        if (!expire) {
            transport.overrideResponse = [](Operation, const OrderInput&, HttpResult) { HttpResult response; response.status = 401; response.body = "synthetic non-JSON intermediary"; return response; };
            controller.refresh("005930"); QVERIFY(idle(controller));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!controller.authenticated() && controller.holdings().isEmpty(), 7000);
        QVERIFY(!controller.connected()); QVERIFY(controller.buyingPower().isEmpty()); QVERIFY(controller.accountChoices().isEmpty());
        QVERIFY(controller.openOrders().isEmpty()); QVERIFY(controller.history().isEmpty()); QVERIFY(!controller.liveEnabled());
        QTest::qWait(400); QCOMPARE(transport.sent.count(Operation::Authenticate), 1); QCOMPARE(transport.sent.count(Operation::PlaceOrder), 0);
    }







};
QTEST_MAIN(BrokerTradingTests)
#include "tst_broker_trading.moc"
