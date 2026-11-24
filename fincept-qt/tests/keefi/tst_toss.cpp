#include "keefi/TossAdapter.h"
#include "keefi/IdentityCompatibility.h"
#include "fixtures/TossResponses.h"
#include <QJsonArray>
#include <QSet>
#include <QUrlQuery>
#include <QtTest>

using namespace keefi;
using namespace keefi::test;
namespace {
AccountRecord account() { AccountRecord a; a.provider = "toss"; a.market = "domestic_equity"; a.environment = "live"; return a; }
OrderInput order() { OrderInput i; i.symbol = "005930"; i.price = "70000"; i.quantity = "10"; i.clientId = "fake-order-001"; return i; }
CredentialSet credentials() {
    CredentialSet c; c.emplace("client_id", SecureBuffer::fromUtf8("fake-client+id"));
    c.emplace("client_secret", SecureBuffer::fromUtf8("fake&secret=value")); return c;
}
bool ready(TossAdapter& adapter) {
    if (!adapter.parse(Operation::Authenticate, tossResponse(Operation::Authenticate), {}).ok ||
        !adapter.parse(Operation::Accounts, tossResponse(Operation::Accounts), {}).ok || adapter.accountChoices().size() != 1) return false;
    QString error; return adapter.selectBrokerAccount(adapter.accountChoices().first().handle, &error);
}
MarketSnapshot snapshot(TossAdapter& adapter) {
    MarketSnapshot s; const auto input = order();
    for (const auto op : adapter.refreshPlan()) {
        const auto parsed = adapter.parse(op, tossResponse(op, input), input);
        if (parsed.ok) { s.data[op] = parsed.data; if (op == Operation::OpenOrders) s.openOrders = parsed.orders; }
    }
    s.quoteReceivedAtMs = QDateTime::currentMSecsSinceEpoch(); return s;
}
HttpResult changed(Operation op, const std::function<void(QJsonObject&)>& mutate, const OrderInput& input = order()) {
    auto r = tossResponse(op, input); auto root = QJsonDocument::fromJson(r.body).object(); mutate(root);
    r.body = QJsonDocument(root).toJson(QJsonDocument::Compact); return r;
}
}
class TossTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        // Preserve the published account-scope vector for an imported profile.
        // Fresh authentication below means another session of this same profile.
        QString error;
        QVERIFY2(configureIdentityScheme(QStringLiteral("legacy-v1"), &error), qPrintable(error));
        QCOMPARE(identitySchemeName(), QStringLiteral("legacy-v1"));
    }
    void authenticatedSessionCannotConstructFinancialRequests() {
        TossAdapter a; QVERIFY(ready(a));
        QVERIFY(a.sessionReady());
        for (const auto operation : {Operation::PlaceOrder,Operation::CancelOrder,Operation::AmendOrder,Operation::TestOrder,Operation::AlgoCancelOrder}) {
            auto input=order(); input.orderId="synthetic-old-order";
            const auto step=a.directStep(operation,input); QString error;
            const auto request=a.buildStep(step,0,&error);
            QVERIFY(request.url.isEmpty()); QVERIFY(!error.isEmpty());
        }
    }

    void publicMetadataStillRequiresTokenAndDoesNotUseAccountHeader() {
        TossAdapter a; QString error; const CredentialSet empty; auto input = order(); input.cursor = "KOSPI";
        a.build(Operation::Markets, account(), input, empty, 0, &error); QVERIFY(!error.isEmpty()); QVERIFY(ready(a));
        const auto request = a.build(Operation::Markets, account(), input, empty, 0, &error);
        QVERIFY(error.isEmpty()); QCOMPARE(request.url.path(), "/api/v1/stocks/all"); QCOMPARE(QUrlQuery(request.url).queryItemValue("market"), "KOSPI");
        QVERIFY(request.headers.contains("Authorization")); QVERIFY(!request.headers.contains("X-Tossinvest-Account"));
        auto unsupported = account(); unsupported.environment = "testnet";
        a.build(Operation::Markets, unsupported, input, empty, 0, &error); QVERIFY(!error.isEmpty());
        QVERIFY(!a.supportsOfficialOrderTest()); a.build(Operation::TestOrder, account(), input, empty, 0, &error); QVERIFY(!error.isEmpty());
    }

    void authenticatesWithFormWithoutBearerOrSecretReturn() {
        TossAdapter a; QString error; const auto c = credentials(); const auto req = a.build(Operation::Authenticate, account(), {}, c, 0, &error);
        QVERIFY(error.isEmpty()); QCOMPARE(req.url.toString(), "https://openapi.tossinvest.com/oauth2/token");
        QCOMPARE(req.method, "POST"); QVERIFY(!req.headers.contains("Authorization"));
        QCOMPARE(req.body, "grant_type=client_credentials&client_id=fake-client%2Bid&client_secret=fake%26secret%3Dvalue");
        const auto parsed = a.parse(Operation::Authenticate, tossResponse(Operation::Authenticate), {});
        QVERIFY(parsed.ok); QVERIFY(parsed.data.isEmpty()); QVERIFY(parsed.orders.isEmpty()); QVERIFY(a.sessionReady());
        QVERIFY(!parsed.tradingPermissionKnown); QVERIFY(a.selectedAccountScope().isEmpty());
    }
    void accountsAreRandomHandlesMaskedAndSelectedPrivately() {
        TossAdapter a; QVERIFY(ready(a)); const auto choices = a.accountChoices();
        QVERIFY(choices.first().handle != "7391"); QCOMPARE(choices.first().handle.size(), 36); QVERIFY(choices.first().label.endsWith("1234"));
        QVERIFY(!choices.first().label.contains("00000001234")); const auto scope = a.selectedAccountScope(); QCOMPARE(scope.size(), 64);
        QString error; const CredentialSet empty;
        auto req = a.build(Operation::Holdings, account(), {}, empty, 0, &error);
        QVERIFY(error.isEmpty()); QCOMPARE(req.headers.value("X-Tossinvest-Account"), "7391");
        QCOMPARE(req.headers.value("Authorization"), "Bearer fakeheader.fakepayload.fakesignature");
        TossAdapter second; QVERIFY(ready(second)); QVERIFY(choices.first().handle != second.accountChoices().first().handle);
        QCOMPARE(scope, second.selectedAccountScope());
        QVERIFY(!second.selectBrokerAccount(choices.first().handle, &error));
        a.clearSession(); QVERIFY(!a.sessionReady()); QVERIFY(a.accountChoices().isEmpty()); QVERIFY(a.selectedAccountScope().isEmpty());
        a.build(Operation::Holdings, account(), {}, empty, 0, &error); QVERIFY(!error.isEmpty());
    }
    void rejectsMalformedAuthenticationAndUsesExpiry() {
        for (const auto& body : {QByteArray(R"({"access_token":"a.b.c","token_type":"Bearer","expires_in":0})"),
            QByteArray(R"({"access_token":"a.b.c\r\nX-Injection:yes","token_type":"Bearer","expires_in":5})"),
            QByteArray(R"({"access_token":"a.b.c","token_type":"Basic","expires_in":5})"),
            QByteArray(R"({"access_token":"a.b.c","token_type":"Bearer","expires_in":2592001})")}) {
            TossAdapter a; HttpResult r; r.status = 200; r.body = body; QVERIFY(!a.parse(Operation::Authenticate, r, {}).ok); QVERIFY(!a.sessionReady());
        }
        TossAdapter a; HttpResult r; r.status = 200; r.body = R"({"access_token":"a.b.c","token_type":"Bearer","expires_in":1})";
        QVERIFY(a.parse(Operation::Authenticate, r, {}).ok); QVERIFY(a.sessionReady()); QTest::qWait(950); QVERIFY(!a.sessionReady());
    }
    void accountScopeIncludesFullNumberNotOnlySequenceOrMaskedSuffix() {
        TossAdapter a; QVERIFY(ready(a)); const auto originalScope = a.selectedAccountScope();
        const auto originalLabel = a.selectedAccountLabel();
        const auto response = changed(Operation::Accounts, [](QJsonObject& root) {
            auto row = root["result"].toArray().first().toObject(); row["accountNo"] = "99999991234";
            root["result"] = QJsonArray{row};
        });
        const auto parsed = a.parse(Operation::Accounts, response, {}); QVERIFY(parsed.ok); QVERIFY(parsed.data.isEmpty());
        QString error; QVERIFY(a.selectBrokerAccount(a.accountChoices().first().handle, &error));
        QCOMPARE(a.selectedAccountLabel(), originalLabel);
        QVERIFY(a.selectedAccountScope() != originalScope); QCOMPARE(a.selectedAccountScope().size(), 64);
        QVERIFY(!a.selectedAccountLabel().contains("9999999"));
    }
    void accountScopePersistsAcrossFreshAuthenticationForSameIdentity() {
        TossAdapter a; QVERIFY(ready(a)); const auto originalScope = a.selectedAccountScope();
        // Independent SHA-256 vector of the documented domain and five length-prefixed fields.
        QCOMPARE(originalScope, "2c451bd01880ac145e05622de3a3afd8ef72fb08db8458c7deb341849075e5ea");
        const auto originalHandle = a.accountChoices().first().handle;
        a.clearSession();
        auto auth = tossResponse(Operation::Authenticate);
        auth.body = R"({"access_token":"other-client-session-token","token_type":"Bearer","expires_in":60})";
        QVERIFY(a.parse(Operation::Authenticate, auth, {}).ok);
        QVERIFY(a.parse(Operation::Accounts, tossResponse(Operation::Accounts), {}).ok);
        QString error; QVERIFY(a.selectBrokerAccount(a.accountChoices().first().handle, &error));
        QCOMPARE(a.selectedAccountScope(), originalScope);
        QVERIFY(a.accountChoices().first().handle != originalHandle);
    }
    void accountScopeDoesNotMergeChangedSequence() {
        TossAdapter a; QVERIFY(ready(a)); const auto originalScope = a.selectedAccountScope();
        const auto response = changed(Operation::Accounts, [](QJsonObject& root) {
            auto row = root["result"].toArray().first().toObject(); row["accountSeq"] = "7392";
            root["result"] = QJsonArray{row};
        });
        QVERIFY(a.parse(Operation::Accounts, response, {}).ok);
        QString error; QVERIFY(a.selectBrokerAccount(a.accountChoices().first().handle, &error));
        QVERIFY(a.selectedAccountScope() != originalScope);
    }

    void accountsRejectDuplicatesAndDoNotGuessUnknownTypes() {
        TossAdapter a; QVERIFY(ready(a));
        auto r = changed(Operation::Accounts, [](QJsonObject& root) { auto rows = root["result"].toArray(); rows.append(rows.first()); root["result"] = rows; });
        QVERIFY(!a.parse(Operation::Accounts, r, {}).ok);
        r = changed(Operation::Accounts, [](QJsonObject& root) { auto row = root["result"].toArray().first().toObject(); row["accountType"] = "NEW_UNKNOWN_TYPE"; root["result"] = QJsonArray{row}; });
        QVERIFY(a.parse(Operation::Accounts, r, {}).ok); QVERIFY(a.accountChoices().isEmpty()); QVERIFY(a.selectedAccountScope().isEmpty());
    }
    void preservesExactAccountInt64AndAcceptsOpaqueBearer() {
        TossAdapter a; HttpResult response; response.status = 200;
        response.body = R"({"access_token":"opaque~token+/==","token_type":"Bearer","expires_in":60})";
        QVERIFY(a.parse(Operation::Authenticate, response, {}).ok);
        response.body = R"({"result":[{"accountNo":"00000001234","accountSeq":9223372036854775807,"accountType":"BROKERAGE"}]})";
        QVERIFY(a.parse(Operation::Accounts, response, {}).ok); QString error;
        QVERIFY(a.selectBrokerAccount(a.accountChoices().first().handle, &error));
        const auto request = a.build(Operation::Holdings, account(), {}, {}, 0, &error); QVERIFY(error.isEmpty());
        QCOMPARE(request.headers.value("X-Tossinvest-Account"), "9223372036854775807");
        QCOMPARE(request.headers.value("Authorization"), "Bearer opaque~token+/==");
    }
    void marketFixturesHaveIndependentUniverses() {
        TossAdapter a; OrderInput i; QSet<QString> symbols;
        for (const auto& market : {"KOSPI", "KOSDAQ", "KR_ETC"}) {
            i.cursor = market; const auto parsed = a.parse(Operation::Markets, tossResponse(Operation::Markets, i), i); QVERIFY(parsed.ok);
            for (const auto& v : parsed.data["instruments"].toArray()) { const auto symbol = v.toObject()["symbol"].toString(); QVERIFY(!symbols.contains(symbol)); symbols.insert(symbol); }
        }
        QCOMPARE(symbols.size(), 2);
    }

    void historyIsAccountWideKstAndCursorEncoded() {
        TossAdapter a; QVERIFY(ready(a)); QString error; const CredentialSet empty; auto i = order();
        i.startTimeMs = QDateTime::fromString("2026-10-16T15:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
        i.endTimeMs = i.startTimeMs + 3600000; i.cursor = "fake+/cursor=";
        const auto request = a.build(Operation::History, account(), i, empty, 0, &error); QVERIFY(error.isEmpty());
        const QUrlQuery query(request.url); QCOMPARE(query.queryItemValue("from"), "2026-10-17"); QCOMPARE(query.queryItemValue("limit"), "100");
        QCOMPARE(query.queryItemValue("cursor", QUrl::FullyDecoded), i.cursor); QVERIFY(!query.hasQueryItem("symbol"));
        const auto open = a.build(Operation::OpenOrders, account(), i, empty, 0, &error); QVERIFY(error.isEmpty()); QCOMPARE(open.url.query(), "status=OPEN");
        i.orderId.clear(); a.build(Operation::QueryOrder, account(), i, empty, 0, &error); QVERIFY(!error.isEmpty());
    }
    void normalizedSnapshotContainsNoCashOrCredentialSubstitution() {
        TossAdapter a; QVERIFY(ready(a)); const auto s = snapshot(a);
        QCOMPARE(s.data.value(Operation::Holdings)["items"].toArray().size(), 1);
        const auto h = s.data.value(Operation::Holdings)["items"].toArray().first().toObject();
        QCOMPARE(h["dailyProfitLoss"].toObject()["amount"].toString(), "-1000");
        QCOMPARE(s.data.value(Operation::BuyingPower)["cashBuyingPower"].toString(), "10000000"); QVERIFY(s.balances.isEmpty());
        const auto r = a.parse(Operation::Holdings, tossResponse(Operation::Holdings, order()), order());
        QVERIFY(r.ok); QVERIFY(!r.tradingPermissionKnown); QVERIFY(!r.data.contains("totalPurchaseAmount"));
        QVERIFY(!a.parse(Operation::Holdings, tossEnvelope(QJsonObject{}), order()).ok);
    }
    void preservesCrossedBooksAndUnknownTimestamp() {
        TossAdapter a; auto r = changed(Operation::Depth, [](QJsonObject& root) { auto o = root["result"].toObject();
            o["asks"] = QJsonArray{QJsonObject{{"price", "69800"}, {"volume", "10"}}}; o["timestamp"] = QJsonValue::Null; root["result"] = o; });
        const auto parsed = a.parse(Operation::Depth, r, order()); QVERIFY(parsed.ok); QCOMPARE(parsed.data["ask"].toString(), "69800");
        QVERIFY(parsed.data["timestamp"].isNull()); QCOMPARE(parsed.data["timestampMs"].toString(), "0");
    }
    void schemaOptionalFieldsRemainUnknownWithoutInventingData() {
        TossAdapter a;
        auto response = changed(Operation::Quote, [](QJsonObject& root) { auto o = root["result"].toArray().first().toObject(); o.remove("timestamp"); root["result"] = QJsonArray{o}; });
        auto parsed = a.parse(Operation::Quote, response, order()); QVERIFY(parsed.ok); QVERIFY(parsed.data["timestamp"].isNull());
        response = changed(Operation::Rules, [](QJsonObject& root) { auto o = root["result"].toArray().first().toObject(); o["englishName"] = "";
            auto d = o["koreanMarketDetail"].toObject(); d["nxtSupported"] = false; d.remove("nxtTradingSuspended"); o["koreanMarketDetail"] = d; root["result"] = QJsonArray{o}; });
        QVERIFY(a.parse(Operation::Rules, response, order()).ok);
        response = changed(Operation::PriceLimits, [](QJsonObject& root) { auto o = root["result"].toObject(); o.remove("upperLimitPrice"); root["result"] = o; });
        parsed = a.parse(Operation::PriceLimits, response, order()); QVERIFY(parsed.ok); QVERIFY(parsed.data["upperLimitPrice"].isNull());
        auto o = tossOrder(); o.remove("orderAmount"); o.remove("canceledAt");
        QVERIFY(a.parse(Operation::OpenOrders, tossEnvelope(QJsonObject{{"orders", QJsonArray{o}}, {"hasNext", false}, {"nextCursor", QJsonValue::Null}}), order()).ok);
    }
    void ordersNeedOfficialMarketMetadataAndPreservePartialCancel() {
        TossAdapter a; const auto i = order(); auto payload = QJsonObject{{"orders", QJsonArray{tossOrder()}}, {"hasNext", false}, {"nextCursor", QJsonValue::Null}};
        auto r = a.parse(Operation::OpenOrders, tossEnvelope(payload), i); QVERIFY(r.ok); QCOMPARE(r.orders.first().market, "unknown"); QVERIFY(!r.orders.first().cancelable);
        QVERIFY(a.parse(Operation::Rules, tossResponse(Operation::Rules, i), i).ok);
        r = a.parse(Operation::OpenOrders, tossEnvelope(payload), i); QVERIFY(r.ok); QVERIFY(r.orders.first().cancelable); QVERIFY(r.orders.first().amendable);
        payload["orders"] = QJsonArray{tossOrder("partial-canceled", "CANCELED", "005930", "10", "3")};
        r = a.parse(Operation::History, tossEnvelope(payload), i); QVERIFY(r.ok); QCOMPARE(r.orders.first().executed, "3"); QCOMPARE(r.orders.first().rawStatus, "CANCELED"); QVERIFY(!r.orders.first().cancelable);
        payload["orders"] = QJsonArray{tossOrder("partial", "PARTIAL_FILLED", "005930", "10", "3")};
        r = a.parse(Operation::History, tossEnvelope(payload), i); QVERIFY(r.ok); QVERIFY(!r.orders.first().cancelable);
        r = a.parse(Operation::OpenOrders, tossEnvelope(payload), i); QVERIFY(r.ok); QVERIFY(r.orders.first().cancelable); QVERIFY(!r.orders.first().amendable);
    }
    void unknownStatusesAndForeignOrdersRemainVisibleWithoutMutation() {
        TossAdapter a; auto i = order(); QVERIFY(a.parse(Operation::Rules, tossResponse(Operation::Rules, i), i).ok);
        const auto payload = QJsonObject{{"orders", QJsonArray{tossOrder("newstate", "FUTURE_UNKNOWN"), tossOrder("foreign", "PENDING", "AAPL")}},
            {"hasNext", false}, {"nextCursor", QJsonValue::Null}};
        const auto r = a.parse(Operation::OpenOrders, tossEnvelope(payload), i); QVERIFY(r.ok); QCOMPARE(r.orders.size(), 2);
        QCOMPARE(r.orders[0].status, "unknown"); QVERIFY(!r.orders[0].cancelable); QVERIFY(!r.orders[1].cancelable);
    }

    void reconcileRejectsSameSymbolDifferentIntent() {
        TossAdapter a; QVERIFY(ready(a)); auto i = order(); snapshot(a); i.operationOrderId = "candidate-order";
        auto parsed = a.parse(Operation::OpenOrders, tossEnvelope(QJsonObject{{"orders", QJsonArray{tossOrder(i.operationOrderId)}},
            {"hasNext", false}, {"nextCursor", QJsonValue::Null}}), i);
        QVERIFY(parsed.ok); QVERIFY(a.reconcile("place", i, parsed.orders).resolved);
        auto wrong = parsed.orders; wrong[0].side = "sell"; QVERIFY(!a.reconcile("place", i, wrong).resolved);
        wrong = parsed.orders; wrong[0].type = "market"; QVERIFY(!a.reconcile("place", i, wrong).resolved);
        wrong = parsed.orders; wrong[0].quantity = "11"; QVERIFY(!a.reconcile("place", i, wrong).resolved);
        wrong = parsed.orders; wrong[0].price = "70100"; QVERIFY(!a.reconcile("place", i, wrong).resolved);
        wrong = parsed.orders; wrong[0].currency = "USD"; QVERIFY(!a.reconcile("place", i, wrong).resolved);
    }
    void reconcileRequiresOriginalAndOperationWithoutConflict() {
        TossAdapter a; QVERIFY(ready(a)); auto i = order(); snapshot(a); i.orderId = "original-order"; i.operationOrderId = "operation-order";
        auto parsed = a.parse(Operation::OpenOrders, tossEnvelope(QJsonObject{{"orders", QJsonArray{tossOrder(i.orderId), tossOrder(i.operationOrderId, "CANCELED")}},
            {"hasNext", false}, {"nextCursor", QJsonValue::Null}}), i);
        QVERIFY(parsed.ok); QVERIFY(!a.reconcile("cancel", i, {parsed.orders.last()}).resolved);
        QVERIFY(!a.reconcile("cancel", i, parsed.orders).resolved);
        parsed.orders[0].rawStatus = "CANCELED"; QVERIFY(a.reconcile("cancel", i, parsed.orders).applied);
        parsed.orders[0].rawStatus = "PENDING"; parsed.orders[1].rawStatus = "PENDING"; QVERIFY(!a.reconcile("amend", i, parsed.orders).resolved);
        parsed.orders[0].rawStatus = "REPLACED"; QVERIFY(a.reconcile("amend", i, parsed.orders).applied);
        parsed.orders[1].quantity = "11"; QVERIFY(!a.reconcile("amend", i, parsed.orders).resolved);
        i.operationOrderId = i.orderId; QVERIFY(!a.reconcile("amend", i, parsed.orders).resolved);
    }
    void partialCancellationQuantitySemanticsStayUnresolved() {
        TossAdapter a; QVERIFY(ready(a)); auto i = order(); snapshot(a); i.orderId = "partial-original"; i.operationOrderId = "partial-cancel";
        auto parsed = a.parse(Operation::History, tossEnvelope(QJsonObject{{"orders", QJsonArray{
            tossOrder(i.orderId, "CANCELED", i.symbol, "10", "3"), tossOrder(i.operationOrderId, "CANCELED", i.symbol, "7", "0")}},
            {"hasNext", false}, {"nextCursor", QJsonValue::Null}}), i);
        QVERIFY(parsed.ok); QCOMPARE(parsed.orders.first().executed, "3");
        QVERIFY(!a.reconcile("cancel", i, parsed.orders).resolved);
        QVERIFY(!a.reconcile("cancel", i, parsed.orders).applied);
        parsed.orders[1].quantity = "10";
        QVERIFY(a.reconcile("cancel", i, parsed.orders).applied);
    }



    void validatesExactAmountsSessionTicksAndLimits() {
        TossAdapter a; QVERIFY(ready(a)); auto s = snapshot(a); auto i = order(); const OrderLimits limits{"2000000", "KRW", "10"};
        auto v = a.validate(i, s, limits); QVERIFY2(v.ok, qPrintable(v.message)); QCOMPARE(v.notional, "700000");
        i.price = "70001"; QVERIFY(!a.validate(i, s, limits).ok); i = order(); i.quantity = "1.5"; QVERIFY(!a.validate(i, s, limits).ok);
        i = order(); QVERIFY(!a.validate(i, s, {"100", "KRW", "10"}).ok); QVERIFY(!a.validate(i, s, {"2000000", "USD", "10"}).ok);
        i.price = "80000"; QVERIFY(!a.validate(i, s, {"2000000", "KRW", "1"}).ok);
        i = order(); i.type = "market"; i.price.clear(); QVERIFY(a.validate(i, s, limits).ok);
        s.data[Operation::Depth]["timestamp"] = QJsonValue::Null; QVERIFY(!a.validate(i, s, limits).ok);
    }
    void marketSellUsesUpperValueBoundAndAdversePriceSeparately() {
        TossAdapter a; QVERIFY(ready(a)); auto s = snapshot(a); auto i = order();
        i.side = "sell"; i.type = "market"; i.price.clear();
        s.data[Operation::Depth]["bids"] = QJsonArray{
            QJsonObject{{"price", "70000"}, {"quantity", "5"}},
            QJsonObject{{"price", "50000"}, {"quantity", "5"}}};
        // Last consumed bid * quantity is 500,000, but cannot bound this sale's value.
        QVERIFY(!a.validate(i, s, {"600000", "KRW", "30"}).ok);
        const auto allowed = a.validate(i, s, {"700000", "KRW", "30"});
        QVERIFY2(allowed.ok, qPrintable(allowed.message)); QCOMPARE(allowed.notional, "700000");
        // The upper bound must not hide the adverse final bid from the deviation check.
        QVERIFY(!a.validate(i, s, {"700000", "KRW", "10"}).ok);
        s.data[Operation::PriceLimits]["upperLimitPrice"] = "65000";
        QVERIFY(!a.validate(i, s, {"700000", "KRW", "30"}).ok);
    }
    void marketBuyStillUsesHighestConsumedAsk() {
        TossAdapter a; QVERIFY(ready(a)); auto s = snapshot(a); auto i = order();
        i.type = "market"; i.price.clear();
        s.data[Operation::Depth]["asks"] = QJsonArray{
            QJsonObject{{"price", "70000"}, {"quantity", "5"}},
            QJsonObject{{"price", "80000"}, {"quantity", "5"}}};
        QVERIFY(!a.validate(i, s, {"750000", "KRW", "20"}).ok);
        const auto allowed = a.validate(i, s, {"800000", "KRW", "20"});
        QVERIFY2(allowed.ok, qPrintable(allowed.message)); QCOMPARE(allowed.notional, "800000");
    }
    void blocksHolidayPartialAmendAndOppositeOrders() {
        TossAdapter a; QVERIFY(ready(a)); auto s = snapshot(a); auto i = order(); const OrderLimits limits{"2000000", "KRW", "10"};
        auto today = s.data[Operation::TradingSchedule]["today"].toObject(); today["integrated"] = QJsonValue::Null;
        s.data[Operation::TradingSchedule]["today"] = today; QVERIFY(!a.validate(i, s, limits).ok); s = snapshot(a);
        OrderRecord existing; existing.orderId = "original"; existing.symbol = i.symbol; existing.side = "sell"; existing.rawStatus = "PENDING_CANCEL";
        s.openOrders = {existing}; QVERIFY(!a.validate(i, s, limits).ok);
        existing.side = "buy"; existing.type = "limit"; existing.market = "domestic_equity"; existing.amendable = false; existing.executed = "1";
        s.openOrders = {existing}; i.orderId = existing.orderId; QVERIFY(!a.validate(i, s, limits).ok);
    }
    void etfTickBelowTwoThousandIsOneWon() {
        TossAdapter a; QVERIFY(ready(a)); auto s = snapshot(a); auto i = order();
        s.data[Operation::Rules]["securityType"] = "ETF"; s.data[Operation::Quote]["lastPrice"] = "1999";
        s.data[Operation::PriceLimits]["lowerLimitPrice"] = "1000"; s.data[Operation::PriceLimits]["upperLimitPrice"] = "3000";
        i.price = "1999"; QVERIFY(a.validate(i, s, {"100000", "KRW", "10"}).ok);
        i.price = "2001"; QVERIFY(!a.validate(i, s, {"100000", "KRW", "10"}).ok);
        i.price = "2005"; QVERIFY(a.validate(i, s, {"100000", "KRW", "10"}).ok);
    }
};
QTEST_GUILESS_MAIN(TossTests)
#include "tst_toss.moc"
