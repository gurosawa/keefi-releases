#include "keefi/BuildPolicy.h"
#include "keefi/UpbitAdapter.h"
#include "keefi/TradingTransport.h"
#include "fixtures/UpbitResponses.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QUrlQuery>
#include <QtTest>

using namespace keefi;
namespace {
CredentialSet credentials() {
    CredentialSet c;
    c.emplace("access_key", SecureBuffer::fromUtf8("fake-access-key"));
    // Deliberately base64-shaped: Upbit requires the literal bytes, not decoding.
    c.emplace("secret", SecureBuffer::fromUtf8("ZmFrZS1zZWNyZXQ="));
    return c;
}
QList<QByteArray> jwt(const RequestSpec& r) { return r.headers.value("Authorization").mid(7).split('.'); }
QJsonObject claims(const RequestSpec& r) {
    const auto parts = jwt(r);
    return parts.size() == 3 ? QJsonDocument::fromJson(QByteArray::fromBase64(parts[1], QByteArray::Base64UrlEncoding)).object() : QJsonObject{};
}
MarketSnapshot snapshot() {
    UpbitAdapter a;
    MarketSnapshot s;
    for (const auto op : a.refreshPlan()) {
        const auto r = a.parse(op, upbit_fixture::response(op), upbit_fixture::input());
        if (!r.ok) qFatal("Synthetic Upbit fixture did not parse");
        s.data.insert(op, r.data);
        if (op == Operation::Account) s.balances = r.balances;
        if (op == Operation::OpenOrders) s.openOrders = r.orders;
    }
    return s;
}
OrderLimits limits() { return {"1000000", "KRW", "5"}; }
HttpResult errorResponse(int status, const QString& name, const QString& text = "raw secret must never escape") {
    return upbit_fixture::json(QJsonObject{{"error", QJsonObject{{"name", name}, {"message", text}}}}, status);
}
}

class UpbitTest : public QObject {
    Q_OBJECT
  private slots:
    void missingCredentialsAndInvalidTargetsRefused();
    void errorsAreSanitizedAndClassified();
    void invalidSchemasRefused();
    void publicRequestsHaveNoAuth();
    void generatedRequestsRespectTransportAuthority() {
        UpbitAdapter adapter;auto input=upbit_fixture::input();input.orderId="fake-original-order";input.newClientId="fake-new-client";
        for(const auto op:{Operation::Markets,Operation::Reference,Operation::Quote,Operation::Rules,Operation::Account,Operation::OpenOrders,Operation::History,Operation::QueryOrder,Operation::CancelOrder,Operation::PlaceOrder,Operation::TestOrder,Operation::AmendOrder}) {
            QString error;const auto r=adapter.build(op,upbit_fixture::account(),input,credentials(),0,&error);
            if (isMutation(op)||op==Operation::TestOrder) { QVERIFY(r.url.isEmpty()); QVERIFY(!error.isEmpty()); continue; }
            QVERIFY2(!r.url.isEmpty(),qPrintable(operationName(op)+error));
            QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild() || (!isMutation(op) && op != Operation::TestOrder));
        }
        for(const auto& side:{"buy","sell"}) {
            input.type="market";input.side=side;input.price.clear();input.quantity=QString(side)=="sell"?"1":"";input.quoteAmount=QString(side)=="buy"?"5000":"";
            QCOMPARE(NetworkTradingTransport::allowed(adapter.build(Operation::PlaceOrder,upbit_fixture::account(),input,credentials(),0,nullptr)), !readOnlyBuild());
        }
    }
    void jwtUsesRawSecretAndFreshNonce();
    void queryHashPreservesArrayOrder();
    void queryEncodingAndHashAreDistinct();

    void historyUsesFixedWindowWithoutFakePage();
    void parsersAcceptSharedFixtures();
    void accountAveragePriceIsOptionalDisplayOnly();
    void optionalAveragePriceRejectsTrailingNewline();
    void accountReadDoesNotProveTradePermission();
    void canceledMarketOrderPreservesFill();
    void externalIdentifiersAndPartialStatuses();

    void openOrdersAreAccountScopedAndPaginated();
    void saturatedHistorySplitsAndAdmitsIncompleteness();
    void numericLexemesAreExact();
    void limitValidationChecksTickMinimumCapAndFee();
    void limitValidationChecksRulesCurrencyAndPrecision();
    void marketValidationChecksDepthDeviationAndFee();
    void amendmentOnlyCreditsOriginalPrincipal();
    void oversizedArithmeticFailsClosed();

};

void UpbitTest::publicRequestsHaveNoAuth() {
    UpbitAdapter a; CredentialSet empty; QString error;
    for (auto op : {Operation::Markets, Operation::Reference, Operation::Quote}) {
        auto r = a.build(op, upbit_fixture::account(), upbit_fixture::input(), empty, 0, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(r.url.host(), QString("api.upbit.com"));
        QVERIFY(!r.authenticated); QVERIFY(!r.headers.contains("Authorization")); QVERIFY(!r.headers.contains("Origin"));
    }
    QVERIFY(!a.refreshPlan().contains(Operation::Time));
}
void UpbitTest::jwtUsesRawSecretAndFreshNonce() {
    UpbitAdapter a; auto c = credentials(); QString error;
    auto one = a.build(Operation::Account, upbit_fixture::account(), {}, c, 0, &error);
    auto two = a.build(Operation::Account, upbit_fixture::account(), {}, c, 0, &error);
    const auto p = jwt(one); QCOMPARE(p.size(), 3);
    QCOMPARE(QJsonDocument::fromJson(QByteArray::fromBase64(p[0], QByteArray::Base64UrlEncoding)).object().value("alg").toString(), QString("HS512"));
    QCOMPARE(claims(one).value("access_key").toString(), QString("fake-access-key"));
    QVERIFY(claims(one).value("nonce").toString() != claims(two).value("nonce").toString());
    QVERIFY(!claims(one).contains("query_hash"));
    const auto mac = QMessageAuthenticationCode::hash(p[0] + '.' + p[1], "ZmFrZS1zZWNyZXQ=", QCryptographicHash::Sha512);
    QCOMPARE(QByteArray::fromBase64(p[2], QByteArray::Base64UrlEncoding), mac);
    QVERIFY(mac != QMessageAuthenticationCode::hash(p[0] + '.' + p[1], QByteArray::fromBase64("ZmFrZS1zZWNyZXQ="), QCryptographicHash::Sha512));
}
void UpbitTest::queryHashPreservesArrayOrder() {
    UpbitAdapter a; auto c = credentials(); QString error;
    auto r = a.build(Operation::OpenOrders, upbit_fixture::account(), {}, c, 0, &error);
    const QByteArray query = "states[]=wait&states[]=watch&page=1&limit=100&order_by=asc";
    QCOMPARE(claims(r).value("query_hash").toString().toLatin1(), QCryptographicHash::hash(query, QCryptographicHash::Sha512).toHex());
    QCOMPARE(claims(r).value("query_hash_alg").toString(), QString("SHA512"));
    QVERIFY(!QUrlQuery(r.url).hasQueryItem("market"));
    QCOMPARE(QUrlQuery(r.url).allQueryItemValues("states[]"), QStringList({"wait", "watch"}));
}
void UpbitTest::queryEncodingAndHashAreDistinct() {
    UpbitAdapter a; auto c = credentials(); QString error;
    OrderInput i; i.clientId = "external order/+";
    const auto r = a.build(Operation::QueryOrder, upbit_fixture::account(), i, c, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(r.url.toEncoded().contains("external%20order%2F%2B"));
    QCOMPARE(claims(r).value("query_hash").toString().toLatin1(),
        QCryptographicHash::hash("identifier=external order/+", QCryptographicHash::Sha512).toHex());
}




void UpbitTest::historyUsesFixedWindowWithoutFakePage() {
    UpbitAdapter a; auto c = credentials(); QString error; auto i = upbit_fixture::input();
    const auto r = a.build(Operation::History, upbit_fixture::account(), i, c, 0, &error);
    const QUrlQuery q(r.url);
    QCOMPARE(q.queryItemValue("start_time"), QString::number(i.startTimeMs));
    QCOMPARE(q.queryItemValue("end_time"), QString::number(i.endTimeMs));
    QVERIFY(!q.hasQueryItem("page")); QVERIFY(!q.hasQueryItem("market"));
    i.startTimeMs--; QVERIFY(a.build(Operation::History, upbit_fixture::account(), i, c, 0, &error).url.isEmpty());
    i = upbit_fixture::input(); i.cursor = "not-a-window";
    QVERIFY(a.build(Operation::History, upbit_fixture::account(), i, c, 0, &error).url.isEmpty());
}
void UpbitTest::parsersAcceptSharedFixtures() {
    auto s = snapshot(); QCOMPARE(s.balances.size(), 2); QCOMPARE(s.openOrders.size(), 1);
    QCOMPARE(s.data.value(Operation::Quote).value("bid").toString(), QString("100000"));
    QCOMPARE(s.data.value(Operation::Quote).value("levels").toArray().size(), 2);
    QVERIFY(UpbitAdapter().validate(upbit_fixture::input(), s, limits()).ok);
}
void UpbitTest::accountAveragePriceIsOptionalDisplayOnly() {
    // Synthetic account response; no private profile or authenticated read.
    const QJsonObject original{{"currency", "BTC"}, {"balance", "2.5"}, {"locked", "0.25"},
        {"avg_buy_price", "00123.4500"}, {"unit_currency", "KRW"}};
    const auto parse = [](const QJsonObject& row) {
        return UpbitAdapter().parse(Operation::Account, upbit_fixture::json(QJsonArray{row}), {});
    };
    const auto reported = parse(original);
    QVERIFY(reported.ok); QCOMPARE(reported.balances.size(), 1);
    QCOMPARE(reported.balances.first().providerAveragePrice, QString("123.45"));
    QCOMPARE(reported.balances.first().providerAverageCurrency, QString("KRW"));
    QVERIFY(!reported.tradingPermissionKnown);
    const QList<QPair<QJsonValue, QJsonValue>> unavailable{
        {QJsonValue::Undefined, "KRW"}, {QJsonValue::Null, "KRW"}, {"", "KRW"},
        {"0", "KRW"}, {"-12", "KRW"}, {"NaN", "KRW"}, {true, "KRW"},
        {"123.45", QJsonValue::Undefined}, {"123.45", QJsonValue::Null},
        {"123.45", "krw"}, {"123.45", "KRW\n"}, {"123.45", true}};
    for (const auto& [price, currency] : unavailable) {
        auto row = original; row.insert("avg_buy_price", price); row.insert("unit_currency", currency);
        const auto result = parse(row);
        QVERIFY(result.ok); QCOMPARE(result.balances.size(), 1);
        const auto balance = result.balances.first();
        QCOMPARE(balance.free, QString("2.5")); QCOMPARE(balance.locked, QString("0.25"));
        QVERIFY(balance.providerAveragePrice.isEmpty()); QVERIFY(balance.providerAverageCurrency.isEmpty());
    }
}
void UpbitTest::optionalAveragePriceRejectsTrailingNewline() {
    const QJsonObject row{{"currency", "BTC"}, {"balance", "2.5"}, {"locked", "0.25"},
        {"avg_buy_price", "123.45\n"}, {"unit_currency", "KRW"}};
    const auto result = UpbitAdapter().parse(Operation::Account, upbit_fixture::json(QJsonArray{row}), {});
    QVERIFY(result.ok); QCOMPARE(result.balances.size(), 1);
    QVERIFY(result.balances.first().providerAveragePrice.isEmpty());
    QVERIFY(result.balances.first().providerAverageCurrency.isEmpty());
    QCOMPARE(result.balances.first().free, QString("2.5"));
}
void UpbitTest::accountReadDoesNotProveTradePermission() {
    UpbitAdapter a;
    for (auto op : {Operation::Account, Operation::Rules, Operation::OpenOrders}) {
        auto r = a.parse(op, upbit_fixture::response(op), upbit_fixture::input());
        QVERIFY(r.ok); QVERIFY(!r.tradingPermissionKnown); QVERIFY(!r.tradingAllowed);
    }
    QCOMPARE(a.parse(Operation::Account, upbit_fixture::response(Operation::Account), {}).data.value("scope").toString(), QString("api_key_pocket"));
}
void UpbitTest::canceledMarketOrderPreservesFill() {
    auto o = upbit_fixture::order("cancel");
    o["ord_type"] = "price"; o["volume"] = QJsonValue(); o["price"] = "10000";
    o["executed_volume"] = "0.09999999"; o["remaining_volume"] = "0";
    auto r = UpbitAdapter().parse(Operation::QueryOrder, upbit_fixture::json(o), {});
    QVERIFY(r.ok); QCOMPARE(r.orders.first().status, QString("canceled"));
    QCOMPARE(r.orders.first().executed, QString("0.09999999"));
    QCOMPARE(r.orders.first().quoteAmount, QString("10000")); QVERIFY(r.orders.first().price.isEmpty());
}
void UpbitTest::externalIdentifiersAndPartialStatuses() {
    auto o = upbit_fixture::order("wait", "external-order", "manual app/+id"); o["executed_volume"] = "0.04";
    auto r = UpbitAdapter().parse(Operation::OpenOrders, upbit_fixture::json(QJsonArray{o}), {});
    QVERIFY(r.ok); QCOMPARE(r.orders.first().status, QString("partial")); QCOMPARE(r.orders.first().clientId, QString("manual app/+id"));
    o.remove("identifier"); o["state"] = "watch";
    r = UpbitAdapter().parse(Operation::OpenOrders, upbit_fixture::json(QJsonArray{o}), {});
    QVERIFY(r.ok); QVERIFY(r.orders.first().cancelable); QVERIFY(!r.orders.first().amendable);
}




void UpbitTest::openOrdersAreAccountScopedAndPaginated() {
    QJsonArray orders;
    for (int n = 0; n < 100; ++n) orders.append(upbit_fixture::order("wait", QString("external-%1").arg(n), ""));
    auto r = UpbitAdapter().parse(Operation::OpenOrders, upbit_fixture::json(orders), {});
    QVERIFY(r.ok); QCOMPARE(r.orders.size(), 100); QVERIFY(r.hasMore); QCOMPARE(r.nextCursor, QString("2"));
    orders.removeLast(); r = UpbitAdapter().parse(Operation::OpenOrders, upbit_fixture::json(orders), {}); QVERIFY(!r.hasMore);
}
void UpbitTest::saturatedHistorySplitsAndAdmitsIncompleteness() {
    QJsonArray orders;
    for (int n = 0; n < 1000; ++n) orders.append(upbit_fixture::order("done", QString("history-%1").arg(n), ""));
    UpbitAdapter a; auto i = upbit_fixture::input();
    auto r = a.parse(Operation::History, upbit_fixture::json(orders), i);
    QVERIFY(r.ok); QVERIFY(r.hasMore); QVERIFY(!r.nextCursor.isEmpty());
    const auto ranges = QJsonDocument::fromJson(QByteArray::fromBase64(r.nextCursor.toLatin1(), QByteArray::Base64UrlEncoding)).array();
    QCOMPARE(ranges.size(), 2); QCOMPARE(ranges[0].toArray()[1].toString(), ranges[1].toArray()[0].toString());
    i.cursor = r.nextCursor; auto c = credentials(); QString error;
    auto request = a.build(Operation::History, upbit_fixture::account(), i, c, 0, &error);
    QCOMPARE(QUrlQuery(request.url).queryItemValue("end_time"), ranges[0].toArray()[1].toString());
    r = a.parse(Operation::History, upbit_fixture::json(QJsonArray{}), i); QVERIFY(r.hasMore);
    i = upbit_fixture::input(); i.startTimeMs = i.endTimeMs - 1;
    r = a.parse(Operation::History, upbit_fixture::json(orders), i);
    QVERIFY(r.ok); QCOMPARE(r.code, QString("HISTORY_INCOMPLETE")); QVERIFY(!r.data.value("complete").toBool(true)); QVERIFY(!r.hasMore);
}
void UpbitTest::numericLexemesAreExact() {
    HttpResult r; r.status = 200;
    r.body = R"([{"market":"BTC-ETH","timestamp":9007199254740993,"orderbook_units":[{"bid_price":0.00000001,"ask_price":0.00000002,"bid_size":0.12345678,"ask_size":1e-8}]}])";
    auto i = upbit_fixture::input(); i.symbol = "BTC-ETH";
    const auto parsed = UpbitAdapter().parse(Operation::Quote, r, i);
    QVERIFY(parsed.ok); QCOMPARE(parsed.data.value("timestamp").toString(), QString("9007199254740993"));
    QCOMPARE(parsed.data.value("bid").toString(), QString("0.00000001"));
    QCOMPARE(parsed.data.value("levels").toArray()[0].toObject().value("askSize").toString(), QString("0.00000001"));
}
void UpbitTest::limitValidationChecksTickMinimumCapAndFee() {
    UpbitAdapter a; auto s = snapshot(); auto i = upbit_fixture::input(); auto l = limits();
    QVERIFY(a.validate(i, s, l).ok);
    i.price = "100001"; QVERIFY(!a.validate(i, s, l).ok);
    i.price = "100000"; i.quantity = "0.04999999"; QVERIFY(!a.validate(i, s, l).ok);
    i.quantity = "0.05"; QVERIFY(a.validate(i, s, l).ok);
    l.maxNotional = "4999.99999999"; QVERIFY(!a.validate(i, s, l).ok);
    l.maxNotional = "5000"; QVERIFY(a.validate(i, s, l).ok);
    s.balances[0].free = "5002.49999999"; QVERIFY(!a.validate(i, s, l).ok);
    s.balances[0].free = "5002.5"; QVERIFY(a.validate(i, s, l).ok);
    i.quantity = "0.1"; s.balances[0].free = "0"; s.balances[0].locked = "999999999"; QVERIFY(!a.validate(i, s, limits()).ok);
}
void UpbitTest::limitValidationChecksRulesCurrencyAndPrecision() {
    UpbitAdapter a; auto s = snapshot(); auto i = upbit_fixture::input(); auto l = limits();
    i.price = "105000"; QVERIFY(a.validate(i, s, l).ok);
    i.price = "105100"; QVERIFY(!a.validate(i, s, l).ok);
    i = upbit_fixture::input(); i.quantity = "0.100000001"; QVERIFY(!a.validate(i, s, l).ok);
    i.quantity = "0.100000000"; QVERIFY(a.validate(i, s, l).ok);
    l.currency = "USDT"; QVERIFY(!a.validate(i, s, l).ok);
    l = limits(); s.data[Operation::Reference]["tickSize"] = "1000"; QVERIFY(!a.validate(i, s, l).ok);
    s = snapshot(); s.data[Operation::Rules]["state"] = "inactive"; QVERIFY(!a.validate(i, s, l).ok);
    s = snapshot(); s.data[Operation::Rules]["buyTypes"] = QJsonArray{"price"}; QVERIFY(!a.validate(i, s, l).ok);
    s = snapshot(); s.data[Operation::Markets]["symbols"] = QJsonArray{"KRW-ETH"}; QVERIFY(!a.validate(i, s, l).ok);
}
void UpbitTest::marketValidationChecksDepthDeviationAndFee() {
    UpbitAdapter a; auto s = snapshot(); auto i = upbit_fixture::input(); auto l = limits();
    i.type = "market"; i.price.clear(); i.quantity.clear(); i.quoteAmount = "5000";
    QVERIFY(a.validate(i, s, l).ok);
    auto levels = s.data[Operation::Quote]["levels"].toArray();
    auto level = levels[0].toObject(); level["askSize"] = "0.01"; levels[0] = level;
    level = levels[1].toObject(); level["askPrice"] = "106000"; levels[1] = level;
    s.data[Operation::Quote]["levels"] = levels; QVERIFY(!a.validate(i, s, l).ok);
    levels.removeLast(); s.data[Operation::Quote]["levels"] = levels; QVERIFY(!a.validate(i, s, l).ok);
    s = snapshot(); s.balances[0].free = "5002.49"; QVERIFY(!a.validate(i, s, l).ok);
    s = snapshot(); i.side = "sell"; i.quoteAmount.clear(); i.quantity = "0.1";
    auto result = a.validate(i, s, l); QVERIFY(result.ok); QCOMPARE(result.notional, QString("10000"));
    s.balances[1].free = "0.09999999"; QVERIFY(!a.validate(i, s, l).ok);
}
void UpbitTest::amendmentOnlyCreditsOriginalPrincipal() {
    UpbitAdapter a; auto s = snapshot(); auto i = upbit_fixture::input();
    i.orderId = "fake-order-1"; i.newClientId = "fake-next";
    s.balances[0].free = "5"; s.balances[0].locked = "10005";
    QVERIFY(a.validate(i, s, limits()).ok);
    s.balances[0].free = "4.99"; QVERIFY(!a.validate(i, s, limits()).ok);
    s.balances[0].free = "100000"; s.balances[0].locked = "9999"; QVERIFY(!a.validate(i, s, limits()).ok);
    s = snapshot(); i.side = "sell"; QVERIFY(!a.validate(i, s, limits()).ok);
    i.side = "buy"; s.openOrders.clear(); QVERIFY(!a.validate(i, s, limits()).ok);
}
void UpbitTest::oversizedArithmeticFailsClosed() {
    UpbitAdapter a; auto s = snapshot(); auto i = upbit_fixture::input(); auto l = limits();
    i.price = "1" + QString(79, '0'); i.quantity = "0.00000001";
    l.maxNotional = QString(80, '9'); s.balances[0].free = l.maxNotional;
    QVERIFY(!a.validate(i, s, l).ok);
    i = upbit_fixture::input(); s = snapshot();
    s.data[Operation::Rules]["buyFee"] = QString(80, '9');
    QVERIFY(!a.validate(i, s, limits()).ok);
}


void UpbitTest::invalidSchemasRefused() {
    UpbitAdapter a; auto i = upbit_fixture::input();
    auto accounts = QJsonDocument::fromJson(upbit_fixture::response(Operation::Account).body).array();
    accounts.append(accounts.first()); QVERIFY(!a.parse(Operation::Account, upbit_fixture::json(accounts), i).ok);
    auto o = upbit_fixture::order(); o["executed_volume"] = "-1";
    QVERIFY(!a.parse(Operation::QueryOrder, upbit_fixture::json(o), i).ok);
    auto quote = QJsonDocument::fromJson(upbit_fixture::response(Operation::Quote).body).array();
    auto market = quote[0].toObject(); auto levels = market["orderbook_units"].toArray(); auto level = levels[1].toObject();
    level["ask_price"] = 99900; levels[1] = level; market["orderbook_units"] = levels; quote[0] = market;
    QVERIFY(!a.parse(Operation::Quote, upbit_fixture::json(quote), i).ok);
    HttpResult huge; huge.status = 200; huge.body = QByteArray(8 * 1024 * 1024 + 1, 'x');
    const auto result=a.parse(Operation::Account, huge, i);QVERIFY(!result.ok);QVERIFY(!result.unknown);
}

void UpbitTest::errorsAreSanitizedAndClassified() {
    UpbitAdapter adapter;
    auto result=adapter.parse(Operation::Account,errorResponse(401,"expired_access_key"),{});
    QVERIFY(result.authenticationFailed);QCOMPARE(result.code,QString("KEY_EXPIRED"));QVERIFY(!result.message.contains("raw secret"));
    auto limited=errorResponse(429,"too_many_requests");limited.headers["Retry-After"]="3";
    result=adapter.parse(Operation::Account,limited,{});QVERIFY(result.rateLimited);QCOMPARE(result.retryAfterSeconds,3);QVERIFY(!result.unknown);
    result=adapter.parse(Operation::Account,errorResponse(418,"blocked"),{});QVERIFY(result.rateLimited);QCOMPARE(result.retryAfterSeconds,0);
    result=adapter.parse(Operation::QueryOrder,errorResponse(404,"order_not_found"),{});QVERIFY(result.notFound);QVERIFY(!result.ok);
}

void UpbitTest::missingCredentialsAndInvalidTargetsRefused() {
    UpbitAdapter adapter;CredentialSet empty;QString error;auto input=upbit_fixture::input();
    QVERIFY(adapter.build(Operation::Account,upbit_fixture::account(),input,empty,0,&error).url.isEmpty());QVERIFY(!error.isEmpty());
    auto keys=credentials();input.orderId="bad/id";
    QVERIFY(adapter.build(Operation::QueryOrder,upbit_fixture::account(),input,keys,0,&error).url.isEmpty());
    auto account=upbit_fixture::account();account.environment="testnet";
    QVERIFY(adapter.build(Operation::Account,account,input,keys,0,&error).url.isEmpty());
}

QTEST_GUILESS_MAIN(UpbitTest)
#include "tst_upbit.moc"
