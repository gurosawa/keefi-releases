#include "keefi/BuildPolicy.h"
#include "keefi/BinanceAdapter.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/TradingTransport.h"
#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QTest>
#include <QUrlQuery>
#include <sodium.h>

using namespace keefi;
namespace {
AccountRecord account(const QString& environment = QStringLiteral("live")) {
    AccountRecord result; result.provider = QStringLiteral("binance"); result.market = QStringLiteral("spot"); result.environment = environment;
    result.id = QStringLiteral("fake-account"); result.currency = QStringLiteral("USDT"); return result;
}
CredentialSet credentials() {
    CredentialSet result;
    result.emplace(QStringLiteral("api_key"), SecureBuffer::fromUtf8(QStringLiteral("FakeApiKey123456")));
    result.emplace(QStringLiteral("secret"), SecureBuffer::fromUtf8(QStringLiteral("FakeSecret1234567890")));
    return result;
}
QJsonObject json(const QByteArray& bytes) { return parseExactJson(bytes).object(); }
OrderInput limitOrder() {
    OrderInput input; input.symbol = QStringLiteral("BTCUSDT"); input.side = QStringLiteral("buy"); input.type = QStringLiteral("limit");
    input.price = QStringLiteral("100"); input.quantity = QStringLiteral("0.125"); input.clientId = QStringLiteral("keefi:test/1"); return input;
}
OrderLimits limits() { return {QStringLiteral("1000"), QStringLiteral("USDT"), QStringLiteral("10")}; }
MarketSnapshot snapshot() {
    MarketSnapshot s;
    s.data[Operation::Rules] = json(R"({"symbol":"BTCUSDT","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDT","isSpotTradingAllowed":true,"amendAllowed":true,"quoteOrderQtyMarketAllowed":true,"orderTypes":["LIMIT","MARKET"],"exchangeFilters":[],"filters":[{"filterType":"PRICE_FILTER","minPrice":"0.01","maxPrice":"1000000","tickSize":"0.01"},{"filterType":"LOT_SIZE","minQty":"0.001","maxQty":"1000","stepSize":"0.001"},{"filterType":"MARKET_LOT_SIZE","minQty":"0.001","maxQty":"100","stepSize":"0.001"},{"filterType":"NOTIONAL","minNotional":"10","maxNotional":"10000","applyMinToMarket":true,"applyMaxToMarket":true,"avgPriceMins":5},{"filterType":"PERCENT_PRICE_BY_SIDE","bidMultiplierUp":"1.2","bidMultiplierDown":"0.8","askMultiplierUp":"1.5","askMultiplierDown":"0.5","avgPriceMins":5}]})");
    s.data[Operation::Quote] = json(R"({"symbol":"BTCUSDT","price":"100"})");
    s.data[Operation::Depth] = json(R"({"symbol":"BTCUSDT","lastUpdateId":1,"bids":[["99","10"]],"asks":[["101","10"]]})");
    s.data[Operation::Reference] = json(R"({"symbol":"BTCUSDT","referencePrice":"100"})");
    s.data[Operation::AveragePrice] = json(R"({"mins":5,"price":"100","closeTime":1700000000000})");
    s.data[Operation::AssetFilters] = json(R"({"symbolFilters":[],"exchangeFilters":[],"assetFilters":[]})");
    s.data[Operation::Account] = json(R"({"canTrade":true,"accountType":"SPOT","commissionRates":{"maker":"0.001","taker":"0.002"}})");
    s.data[Operation::OpenOrders] = QJsonObject{};
    s.balances = {{QStringLiteral("BTC"), QStringLiteral("10"), QStringLiteral("1")}, {QStringLiteral("USDT"), QStringLiteral("20000"), QStringLiteral("0")}};
    return s;
}
void addFilter(MarketSnapshot& s, const QJsonObject& filter) {
    auto rules = s.data[Operation::Rules]; auto filters = rules.value(QStringLiteral("filters")).toArray(); filters.append(filter);
    rules.insert(QStringLiteral("filters"), filters); s.data[Operation::Rules] = rules;
}
QByteArray rawOrder(const QByteArray& id = "9007199254740993", const QByteArray& status = "PARTIALLY_FILLED", const QByteArray& list = "-1") {
    return "{\"symbol\":\"BTCUSDT\",\"orderId\":" + id + ",\"orderListId\":" + list +
        ",\"clientOrderId\":\"external-order\",\"price\":\"100\",\"origQty\":\"10\",\"executedQty\":\"2\",\"cummulativeQuoteQty\":\"200\",\"status\":\"" + status +
        "\",\"timeInForce\":\"GTC\",\"type\":\"LIMIT\",\"side\":\"BUY\"}";
}
HttpResult response(const QByteArray& body, int status = 200) { HttpResult r; r.status = status; r.body = body; return r; }
}

class BinanceTest final : public QObject {
    Q_OBJECT
  private slots:
    void orderListQueryRejectsMismatchedIdentity();
    void openOrderQueryRemainsAccountScoped();
    void rateLimitAndSecretRedaction();
    void parsersRejectWrongSchemas();
    void unicodeSymbolsUseEncodedSigningBytes();
    void signatureUsesExactFormBytes();
    void initTestCase() { QVERIFY(sodium_init() >= 0); }
    void generatedRequestsRespectTransportAuthority() {
        BinanceAdapter adapter;auto input=limitOrder();input.orderId="123";input.newClientId="fake-replacement";
        input.startTimeMs=1700000000000;input.endTimeMs=input.startTimeMs+3600000;
        for(const auto& environment:{"live"})for(const auto op:{Operation::Time,Operation::Markets,Operation::Rules,Operation::Quote,Operation::Depth,Operation::Reference,Operation::AveragePrice,Operation::AssetFilters,Operation::Account,Operation::OpenOrders,Operation::History,Operation::QueryOrder,Operation::CancelOrder,Operation::AmendOrder,Operation::TestOrder,Operation::PlaceOrder}) {
            QString error;const auto r=adapter.build(op,account(environment),input,credentials(),1700000000000,&error);
            if (isMutation(op)||op==Operation::TestOrder) { QVERIFY(r.url.isEmpty()); QVERIFY(!error.isEmpty()); continue; }
            QVERIFY2(!r.url.isEmpty(),qPrintable(operationName(op)+error));
            QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild() || (!isMutation(op) && op != Operation::TestOrder));
        }
        input.cancelReplace=true;auto r=adapter.build(Operation::AmendOrder,account(),input,credentials(),1700000000000,nullptr);QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        input.orderListId="123";r=adapter.build(Operation::CancelOrder,account(),input,credentials(),1700000000000,nullptr);QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        r=adapter.build(Operation::OrderList,account(),input,credentials(),1700000000000,nullptr);QVERIFY(NetworkTradingTransport::allowed(r));
    }

    void hostsEnvironmentsAndKeyTypesAreBounded();

    void marketListUsesSpotFilterWithoutChangingRules();
    void exactIdsAndExternalOrders();


    void unrecognizedAndListOrdersStayVisible();


    void historyWindowsAndCursor();
    void historyFullPageUsesExactIdCursor();
    void referenceAbsentDiffersFromFailed();

    void unicodeBalanceNamesRemainReadable();
    void unicodeBalanceFailureIsIsolatedToIdentifierValidation();
    void unicodeIdentifierBoundsAndUnsafeNames();

    void unicodeSymbolsKeepOrderAndValidationIdentity();
    void unicodeBalancesStillRequireValidAmountsAndSchema();
    void validLimitAndMarket();
    void exactStepAndPriceLimits();
    void marketDepthAndSlippage();
    void dynamicPriceBasisAndSides();
    void notionalBoundsAndAssetFilters();
    void amountCurrencyDeviationAndBalances();
    void positionAndOrderCountGuards();
    void unsupportedFilterFailsClosed();

    void snapshotPermissionsAndCompleteness();
};



void BinanceTest::hostsEnvironmentsAndKeyTypesAreBounded() {
    BinanceAdapter adapter; QString error; const auto input = limitOrder();
    auto live = adapter.build(Operation::Time, account(QStringLiteral("live")), input, {}, 0, &error);
    QCOMPARE(live.url.host(), QStringLiteral("api.binance.com")); QVERIFY(!live.authenticated);
    for (const auto& retired : {QStringLiteral("testnet"), QStringLiteral("demo"), QStringLiteral("paper")}) {
        QVERIFY(adapter.build(Operation::Account, account(retired), input, credentials(), 1, &error).url.isEmpty()); QVERIFY(!error.isEmpty());
    }
    auto invalid = account(QStringLiteral("https://attacker.invalid"));
    QVERIFY(adapter.build(Operation::Account, invalid, input, credentials(), 1, &error).url.isEmpty()); QVERIFY(!error.isEmpty());
    invalid = account(); invalid.market = QStringLiteral("usdm_futures");
    QVERIFY(adapter.build(Operation::Account, invalid, input, credentials(), 1, &error).url.isEmpty());
    auto unsupported = credentials(); unsupported[QStringLiteral("secret")] = SecureBuffer::fromUtf8(QStringLiteral("-----BEGIN PRIVATE KEY-----"));
    QVERIFY(adapter.build(Operation::Account, account(), input, unsupported, 1, &error).url.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Ed25519")));
    QVERIFY(adapter.build(Operation::Account, account(), input, credentials(), 0, &error).url.isEmpty());
}

void BinanceTest::marketListUsesSpotFilterWithoutChangingRules() {
    BinanceAdapter adapter; QString error;
    for (const auto& environment : {QStringLiteral("live")}) {
        const auto markets = adapter.build(Operation::Markets, account(environment), limitOrder(), {}, 0, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(markets.method, QByteArray("GET")); QCOMPARE(markets.url.path(), QStringLiteral("/api/v3/exchangeInfo"));
        QCOMPARE(markets.url.query(QUrl::FullyEncoded), QStringLiteral("permissions=SPOT&showPermissionSets=false"));
        QVERIFY(!markets.authenticated); QVERIFY(markets.headers.isEmpty()); QVERIFY(markets.body.isEmpty());
        const auto rules = adapter.build(Operation::Rules, account(environment), limitOrder(), {}, 0, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(rules.url.query(QUrl::FullyEncoded), QStringLiteral("symbol=BTCUSDT"));
        QVERIFY(!rules.authenticated);
    }
}
void BinanceTest::exactIdsAndExternalOrders() {
    BinanceAdapter adapter;
    auto result = adapter.parse(Operation::OpenOrders, response('[' + rawOrder() + ']'), limitOrder());
    QVERIFY(result.ok); QCOMPARE(result.orders.size(), 1); QCOMPARE(result.orders[0].orderId, QStringLiteral("9007199254740993"));
    QCOMPARE(result.orders[0].clientId, QStringLiteral("external-order")); QCOMPARE(result.orders[0].status, QStringLiteral("partial"));
    QCOMPARE(result.orders[0].remaining, QStringLiteral("8")); QVERIFY(result.orders[0].cancelable); QVERIFY(result.orders[0].amendable);
    auto unavailable = rawOrder(); unavailable.replace("\"200\"", "\"-1.00000000\"");
    auto history = adapter.parse(Operation::History, response('[' + unavailable + ']'), limitOrder());
    QVERIFY(history.ok); QVERIFY(history.orders[0].quoteAmount.isEmpty());
}

void BinanceTest::unrecognizedAndListOrdersStayVisible() {
    BinanceAdapter adapter;
    auto list = adapter.parse(Operation::OpenOrders, response('[' + rawOrder("123", "NEW", "12") + ']'), limitOrder());
    QVERIFY(list.ok); QCOMPARE(list.orders.size(), 1); QVERIFY(list.orders[0].cancelable); QCOMPARE(list.orders[0].orderListId,QString("12"));QVERIFY(!list.orders[0].amendable);
    auto pending = adapter.parse(Operation::OpenOrders, response('[' + rawOrder("123", "PENDING_NEW") + ']'), limitOrder());
    QVERIFY(pending.ok); QCOMPARE(pending.orders[0].status, QStringLiteral("unknown")); QVERIFY(!pending.orders[0].cancelable);
}




void BinanceTest::historyWindowsAndCursor() {
    BinanceAdapter adapter; QString error; auto input = limitOrder(); input.startTimeMs = 1700000000000LL; input.endTimeMs = input.startTimeMs + 7LL * 86400000;
    auto request = adapter.build(Operation::History, account(), input, credentials(), 1700000000123LL, &error);
    QVERIFY(error.isEmpty()); const QUrlQuery query(request.url);
    QCOMPARE(query.queryItemValue(QStringLiteral("endTime")), QString::number(input.startTimeMs + 86400000 - 1));
    auto result = adapter.parse(Operation::History, response("[]"), input); QVERIFY(result.ok); QVERIFY(result.hasMore);
    QCOMPARE(result.nextCursor, QString::number(input.startTimeMs + 86400000) + QLatin1Char('|'));
    input.cursor = result.nextCursor; request = adapter.build(Operation::History, account(), input, credentials(), 1, &error);
    QCOMPARE(QUrlQuery(request.url).queryItemValue(QStringLiteral("startTime")), QString::number(input.startTimeMs + 86400000));
    input.cursor = QStringLiteral("bad|&signature=bad"); QVERIFY(adapter.build(Operation::History, account(), input, credentials(), 1, &error).url.isEmpty());
}
void BinanceTest::historyFullPageUsesExactIdCursor() {
    BinanceAdapter adapter; auto input = limitOrder(); input.startTimeMs = 1000; input.endTimeMs = 2000;
    QByteArray rows("[");
    for (qint64 i = 0; i < 1000; ++i) { if (i) rows += ','; rows += rawOrder(QByteArray::number(9007199254740993LL + i)); }
    rows += ']';
    auto result = adapter.parse(Operation::History, response(rows), input); QVERIFY(result.ok); QVERIFY(result.hasMore);
    QCOMPARE(result.nextCursor, QStringLiteral("1000|9007199254741993"));
}
void BinanceTest::referenceAbsentDiffersFromFailed() {
    BinanceAdapter adapter;
    auto absent = adapter.parse(Operation::Reference, response("{\"code\":-2043}", 400), limitOrder());
    QVERIFY(absent.ok); QVERIFY(absent.data.value(QStringLiteral("referencePrice")).isNull());
    QVERIFY(!adapter.parse(Operation::Reference, response("{}", 503), limitOrder()).ok);
}

void BinanceTest::unicodeBalanceNamesRemainReadable() {
    // Public Binance Spot Testnet example asset; no account data or API call.
    const auto fixture = QStringLiteral(R"({"canTrade":false,"accountType":"SPOT","balances":[{"asset":"这是测试币","free":"0.00000000","locked":"0.00000000"}]})").toUtf8();
    const auto result = BinanceAdapter().parse(Operation::Account, response(fixture), limitOrder());
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.balances.size(), 1);
    QCOMPARE(result.balances.first().asset, QStringLiteral("这是测试币"));
    QCOMPARE(result.balances.first().free, QStringLiteral("0.00000000"));
    QVERIFY(result.tradingPermissionKnown); QVERIFY(!result.tradingAllowed);
}
void BinanceTest::unicodeBalanceFailureIsIsolatedToIdentifierValidation() {
    const auto fixture = QStringLiteral(R"({"canTrade":false,"accountType":"SPOT","balances":[{"asset":"这是测试币","free":"0.00000000","locked":"0.00000000"}]})").toUtf8();
    QString parseError;
    const auto parsed = parseExactJson(fixture, &parseError).object();
    QVERIFY(parseError.isEmpty());
    const auto balance = parsed.value(QStringLiteral("balances")).toArray().first().toObject();
    QCOMPARE(balance.value(QStringLiteral("asset")).toString(), QStringLiteral("这是测试币"));
    QVERIFY(parsed.value(QStringLiteral("canTrade")).isBool());
    QVERIFY(decimal::valid(balance.value(QStringLiteral("free")).toString()));
    QVERIFY(decimal::valid(balance.value(QStringLiteral("locked")).toString()));
    auto asciiControl = fixture;
    asciiControl.replace(QStringLiteral("这是测试币").toUtf8(), "LDUSDT");
    const auto result = BinanceAdapter().parse(Operation::Account, response(asciiControl), limitOrder());
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.balances.first().asset, QStringLiteral("LDUSDT"));
}
void BinanceTest::unicodeIdentifierBoundsAndUnsafeNames() {
    const auto parseAsset = [](const QString& asset) {
        const QJsonObject account{{"canTrade", false}, {"accountType", "SPOT"}, {"balances", QJsonArray{
            QJsonObject{{"asset", asset}, {"free", "0.00000000"}, {"locked", "0.00000000"}}}}};
        return BinanceAdapter().parse(Operation::Account,
            response(QJsonDocument(account).toJson(QJsonDocument::Compact)), limitOrder());
    };
    const QStringList rejected{QString{}, QStringLiteral("BTC USDT"), QStringLiteral(" BTC"), QStringLiteral("BTC\t"),
        QStringLiteral("BTC\n"), QStringLiteral("BTC/USDT"), QStringLiteral("BTC&symbol=ETH"), QStringLiteral("BTC#fragment"),
        QStringLiteral("%E8%BF%99"), QStringLiteral("<BTC>"), QStringLiteral("BTC\\USDT"), QStringLiteral("BTC\u202e"),
        QStringLiteral("BTC\u200d"), QStringLiteral("BTC\u00a0"), QStringLiteral("BTC") + QChar(0),
        QString(41, QChar(0x5e01)), QString(1, QChar(0xd800)), QString(1, QChar(0xdc00))};
    for (const auto& name : rejected) {
        QVERIFY(!binanceIdentifierValid(name));
        QVERIFY(!parseAsset(name).ok);
    }
    // Count scalars rather than UTF-16 code units; preserve case and normalization.
    const QString astralLetter = QString::fromUcs4(U"\U00010400");
    QVERIFY(binanceIdentifierValid(astralLetter.repeated(40)));
    QVERIFY(!binanceIdentifierValid(astralLetter.repeated(41)));
    QVERIFY(parseAsset(astralLetter.repeated(40)).ok);
    const QString decomposed = QStringLiteral("a\u0301币");
    auto result = parseAsset(decomposed); QVERIFY(result.ok);
    QCOMPARE(result.balances.first().asset, decomposed);
    QVERIFY(result.balances.first().asset != decomposed.normalized(QString::NormalizationForm_C));
    QCOMPARE(parseAsset(QStringLiteral("lowercase")).balances.first().asset, QStringLiteral("lowercase"));
    QVERIFY(!binanceIdentifierValid(QStringLiteral("I"), 2));
}

void BinanceTest::unicodeSymbolsKeepOrderAndValidationIdentity() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot(); auto l = limits();
    const QString base = QStringLiteral("这是测试币"); input.symbol = base + QStringLiteral("456"); l.currency = QStringLiteral("456");
    auto rules = s.data.value(Operation::Rules);
    rules["symbol"] = input.symbol; rules["baseAsset"] = base; rules["quoteAsset"] = l.currency;
    s.data[Operation::Rules] = rules;
    for (const auto op : {Operation::Quote, Operation::Depth, Operation::Reference}) s.data[op]["symbol"] = input.symbol;
    s.balances[0].asset = base; s.balances[1].asset = l.currency;
    auto validated = adapter.validate(input, s, l); QVERIFY2(validated.ok, qPrintable(validated.message));
    input.side = QStringLiteral("sell"); QVERIFY(adapter.validate(input, s, l).ok);
    auto raw = rawOrder(); raw.replace("BTCUSDT", input.symbol.toUtf8());
    const auto parsed = adapter.parse(Operation::OpenOrders, response('[' + raw + ']'), input);
    QVERIFY(parsed.ok); QCOMPARE(parsed.orders.first().symbol, input.symbol); QVERIFY(parsed.orders.first().cancelable);
    const auto market = adapter.parse(Operation::Markets,
        response(QJsonDocument(QJsonObject{{"symbols", QJsonArray{rules}}}).toJson(QJsonDocument::Compact)), input);
    QVERIFY(market.ok); QCOMPARE(market.data.value("symbols").toArray().first().toObject().value("symbol").toString(), input.symbol);
    QCOMPARE(market.data.value("symbols").toArray().first().toObject().value("baseAsset").toString(), base);
}
void BinanceTest::unicodeBalancesStillRequireValidAmountsAndSchema() {
    const QJsonObject valid{{"canTrade", false}, {"accountType", "SPOT"}, {"balances", QJsonArray{
        QJsonObject{{"asset", QStringLiteral("这是测试币")}, {"free", "0.00000000"}, {"locked", "0.00000000"}}}}};
    const auto parse = [](const QJsonObject& object) {
        return BinanceAdapter().parse(Operation::Account, response(QJsonDocument(object).toJson(QJsonDocument::Compact)), limitOrder());
    };
    for (const auto& invalidAmount : {QStringLiteral("-1"), QStringLiteral("NaN"), QStringLiteral("1e-8"), QString(81, '1')}) {
        auto account = valid; auto balances = account.value("balances").toArray(); auto balance = balances.first().toObject();
        balance["free"] = invalidAmount; balances[0] = balance; account["balances"] = balances;
        QVERIFY(!parse(account).ok);
    }
    auto missing = valid; missing.remove("balances"); QVERIFY(!parse(missing).ok);
    auto wrongType = valid; wrongType["balances"] = QJsonObject{}; QVERIFY(!parse(wrongType).ok);
    wrongType = valid; wrongType["canTrade"] = QStringLiteral("false"); QVERIFY(!parse(wrongType).ok);
}
void BinanceTest::validLimitAndMarket() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot();
    auto result = adapter.validate(input, s, limits()); QVERIFY2(result.ok, qPrintable(result.message)); QCOMPARE(result.notional, QStringLiteral("12.5"));
    input.type = QStringLiteral("market"); input.price.clear(); result = adapter.validate(input, s, limits()); QVERIFY2(result.ok, qPrintable(result.message));
    input.side = QStringLiteral("sell"); result = adapter.validate(input, s, limits()); QVERIFY2(result.ok, qPrintable(result.message));
    input.quantity.clear(); input.quoteAmount = QStringLiteral("12.5"); QVERIFY(!adapter.validate(input, s, limits()).ok);
}
void BinanceTest::marketDepthAndSlippage() {
    BinanceAdapter adapter; auto input = limitOrder(); input.type = QStringLiteral("market"); input.price.clear();
    auto s = snapshot();
    s.data[Operation::Depth] = json(R"({"symbol":"BTCUSDT","lastUpdateId":1,"bids":[["99","10"]],"asks":[["101","0.1"],["109","10"]]})");
    auto result = adapter.validate(input, s, limits()); QVERIFY2(result.ok, qPrintable(result.message)); QCOMPARE(result.notional, QStringLiteral("12.825"));
    auto l = limits(); l.maxNotional = QStringLiteral("12.8"); QVERIFY(!adapter.validate(input, s, l).ok);
    l = limits(); l.maxDeviationPercent = QStringLiteral("8"); QVERIFY(!adapter.validate(input, s, l).ok);
    s.data[Operation::Depth] = json(R"({"symbol":"BTCUSDT","bids":[],"asks":[["101","0.1"]]})");
    QVERIFY(!adapter.validate(input, s, limits()).ok);
    s.data.remove(Operation::Depth); QVERIFY(!adapter.validate(input, s, limits()).ok);
    auto parsed = adapter.parse(Operation::Depth, response(R"({"lastUpdateId":9007199254740993,"bids":[["99","1"]],"asks":[["101","1"]]})"), input);
    QVERIFY(parsed.ok); QCOMPARE(parsed.data.value(QStringLiteral("lastUpdateId")).toString(), QStringLiteral("9007199254740993"));
    QVERIFY(!adapter.parse(Operation::Depth, response(R"({"lastUpdateId":1,"bids":[["102","1"]],"asks":[["101","1"]]})"), input).ok);
}
void BinanceTest::exactStepAndPriceLimits() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot();
    input.quantity = QStringLiteral("0.125000000000000001"); QVERIFY(!adapter.validate(input, s, limits()).ok);
    input = limitOrder(); input.price = QStringLiteral("100.001"); QVERIFY(!adapter.validate(input, s, limits()).ok);
    input.price = QStringLiteral("100.010000000000000000"); QVERIFY(adapter.validate(input, s, limits()).ok);
    input.quantity = QStringLiteral("0"); QVERIFY(!adapter.validate(input, s, limits()).ok);
}
void BinanceTest::dynamicPriceBasisAndSides() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot(); auto l = limits(); l.maxDeviationPercent = QStringLiteral("100");
    input.price = QStringLiteral("130"); QVERIFY(!adapter.validate(input, s, l).ok);
    input.side = QStringLiteral("sell"); QVERIFY(adapter.validate(input, s, l).ok);
    input = limitOrder(); s.data[Operation::Reference] = json(R"({"referencePrice":null})");
    QVERIFY(adapter.validate(input, s, limits()).ok);
    s.data[Operation::AveragePrice] = json(R"({"mins":1,"price":"100"})"); QVERIFY(!adapter.validate(input, s, limits()).ok);
    s.data[Operation::Reference] = json(R"({"referencePrice":"100"})"); QVERIFY(adapter.validate(input, s, limits()).ok);
    s.data.remove(Operation::Reference); QVERIFY(!adapter.validate(input, s, limits()).ok);
}
void BinanceTest::notionalBoundsAndAssetFilters() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot();
    input.quantity = QStringLiteral("0.099"); QVERIFY(!adapter.validate(input, s, limits()).ok);
    input = limitOrder(); input.type = QStringLiteral("market"); input.price.clear();
    s.data[Operation::Reference] = json(R"({"referencePrice":"70"})"); QVERIFY(!adapter.validate(input, s, limits()).ok);
    s = snapshot(); input = limitOrder();
    s.data[Operation::AssetFilters] = json(R"({"symbolFilters":[],"exchangeFilters":[],"assetFilters":[{"filterType":"MAX_ASSET","asset":"USDT","limit":"12"}]})");
    QVERIFY(!adapter.validate(input, s, limits()).ok);
}
void BinanceTest::amountCurrencyDeviationAndBalances() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot(); auto l = limits();
    l.currency = QStringLiteral("KRW"); QVERIFY(!adapter.validate(input, s, l).ok);
    l = limits(); l.maxNotional = QStringLiteral("12.49"); QVERIFY(!adapter.validate(input, s, l).ok);
    l = limits(); input.price = QStringLiteral("111"); QVERIFY(!adapter.validate(input, s, l).ok);
    input = limitOrder(); s.balances[1].free = QStringLiteral("12.5"); QVERIFY(!adapter.validate(input, s, l).ok);
    s.balances[1].free = QStringLiteral("12.525"); QVERIFY(adapter.validate(input, s, l).ok);
}
void BinanceTest::positionAndOrderCountGuards() {
    BinanceAdapter adapter; auto input = limitOrder(); auto s = snapshot();
    addFilter(s, json(R"({"filterType":"MAX_POSITION","maxPosition":"11.1"})"));
    QVERIFY(!adapter.validate(input, s, limits()).ok);
    s = snapshot(); addFilter(s, json(R"({"filterType":"MAX_NUM_ORDERS","maxNumOrders":1})"));
    OrderRecord order; order.symbol = input.symbol; s.openOrders.append(order); QVERIFY(!adapter.validate(input, s, limits()).ok);
    s.openOrders.clear(); QVERIFY(adapter.validate(input, s, limits()).ok);
    s.data.remove(Operation::OpenOrders); QVERIFY(!adapter.validate(input, s, limits()).ok);
}
void BinanceTest::unsupportedFilterFailsClosed() {
    BinanceAdapter adapter; auto s = snapshot(); addFilter(s, json(R"({"filterType":"FUTURE_RULE","value":"1"})"));
    QVERIFY(!adapter.validate(limitOrder(), s, limits()).ok);
    s = snapshot(); addFilter(s, json(R"({"filterType":"TRAILING_DELTA","minTrailingAboveDelta":10})"));
    QVERIFY(adapter.validate(limitOrder(), s, limits()).ok);
}

void BinanceTest::snapshotPermissionsAndCompleteness() {
    BinanceAdapter adapter; auto s = snapshot();
    s.data[Operation::Account].insert(QStringLiteral("canTrade"), false); QVERIFY(!adapter.validate(limitOrder(), s, limits()).ok);
    s = snapshot(); s.data.remove(Operation::AssetFilters); QVERIFY(!adapter.validate(limitOrder(), s, limits()).ok);
    s = snapshot(); s.data[Operation::Rules].insert(QStringLiteral("status"), QStringLiteral("HALT")); QVERIFY(!adapter.validate(limitOrder(), s, limits()).ok);
}
void BinanceTest::signatureUsesExactFormBytes() {
    BinanceAdapter adapter; QString error;
    const auto request = adapter.build(Operation::Account, account(), {}, credentials(), 1700000000123LL, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(request.method, QByteArray("GET"));
    QCOMPARE(request.url.path(), QString("/api/v3/account"));
    QCOMPARE(request.headers.value("X-MBX-APIKEY"), QByteArray("FakeApiKey123456"));
    QVERIFY(request.body.isEmpty());
    const auto wire = request.url.query(QUrl::FullyEncoded).toUtf8();
    const auto separator = wire.lastIndexOf("&signature="); QVERIFY(separator > 0);
    QCOMPARE(wire.left(separator), QByteArray("recvWindow=5000&timestamp=1700000000123"));
    QCOMPARE(wire.mid(separator + 11), QMessageAuthenticationCode::hash(wire.left(separator), "FakeSecret1234567890", QCryptographicHash::Sha256).toHex());
}
void BinanceTest::unicodeSymbolsUseEncodedSigningBytes() {
    BinanceAdapter adapter; QString error; auto input = limitOrder();
    input.symbol = QStringLiteral("这是测试币456");
    const auto request = adapter.build(Operation::QueryOrder, account(), input, credentials(), 1700000000123LL, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(request.url.host(), QStringLiteral("api.binance.com"));
    const qsizetype separator = request.url.query(QUrl::FullyEncoded).toUtf8().lastIndexOf("&signature="); QVERIFY(separator > 0);
    const auto payload = request.url.query(QUrl::FullyEncoded).toUtf8().left(separator);
    const auto encodedSymbol = QUrl::toPercentEncoding(input.symbol);
    QVERIFY(payload.startsWith("symbol=" + encodedSymbol + "&origClientOrderId="));
    QVERIFY(!payload.contains(input.symbol.toUtf8())); QVERIFY(!payload.contains("%25E8"));
    QCOMPARE(request.url.query(QUrl::FullyEncoded).toUtf8().mid(separator + 11), QMessageAuthenticationCode::hash(
        payload, "FakeSecret1234567890", QCryptographicHash::Sha256).toHex());
    auto wronglyUnencoded = payload; wronglyUnencoded.replace(encodedSymbol, input.symbol.toUtf8());
    QVERIFY(request.url.query(QUrl::FullyEncoded).toUtf8().mid(separator + 11) != QMessageAuthenticationCode::hash(
        wronglyUnencoded, "FakeSecret1234567890", QCryptographicHash::Sha256).toHex());
    for (const auto op : {Operation::Rules, Operation::Quote, Operation::Depth, Operation::QueryOrder}) {
        input.orderId = QStringLiteral("123");
        const auto get = adapter.build(op, account(), input, credentials(), 1700000000123LL, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QUrlQuery(get.url).queryItemValue(QStringLiteral("symbol"), QUrl::FullyDecoded), input.symbol);
        QVERIFY(get.url.toEncoded().contains("symbol=" + encodedSymbol));
        QCOMPARE(get.url.host(), QStringLiteral("api.binance.com"));
    }
    input.symbol = QString::fromLatin1(encodedSymbol);
    QVERIFY(adapter.build(Operation::Rules, account(), input, {}, 0, &error).url.isEmpty());
}
void BinanceTest::parsersRejectWrongSchemas() {
    BinanceAdapter adapter;
    for (auto operation : {Operation::Time, Operation::Rules, Operation::Account, Operation::AssetFilters, Operation::AveragePrice, Operation::Quote})
        QVERIFY(!adapter.parse(operation, response("{}"), limitOrder()).ok);
    auto balances = adapter.parse(Operation::Account, response(R"({"canTrade":true,"accountType":"SPOT","balances":[{"asset":"I","free":"1.000000000000000001","locked":"0"}]})"), limitOrder());
    QVERIFY(balances.ok); QCOMPARE(balances.balances[0].free, QStringLiteral("1.000000000000000001"));
}

void BinanceTest::rateLimitAndSecretRedaction() {
    BinanceAdapter adapter; auto r = response("{\"code\":-1003,\"msg\":\"FakeSecret1234567890\"}", 429); r.headers.insert("retry-after", "120");
    auto result = adapter.parse(Operation::Account, r, limitOrder());
    QVERIFY(result.rateLimited); QCOMPARE(result.retryAfterSeconds, 120); QVERIFY(!result.unknown); QVERIFY(!result.message.contains(QStringLiteral("FakeSecret")));
    r = response("{\"code\":-2013}", 400); result = adapter.parse(Operation::QueryOrder, r, limitOrder());
    QVERIFY(result.notFound); QVERIFY(!result.ok); QVERIFY(!result.unknown);
    r = response("{\"code\":-2026}", 400); result = adapter.parse(Operation::QueryOrder, r, limitOrder());
    QVERIFY(result.notFound); QCOMPARE(result.code, QStringLiteral("order_archived"));
}

void BinanceTest::openOrderQueryRemainsAccountScoped() {
    BinanceAdapter adapter; QString error;
    const auto request = adapter.build(Operation::OpenOrders, account(), limitOrder(), credentials(), 1000, &error);
    QVERIFY(error.isEmpty()); QVERIFY(!QUrlQuery(request.url).hasQueryItem(QStringLiteral("symbol")));
}

void BinanceTest::orderListQueryRejectsMismatchedIdentity() {
    BinanceAdapter adapter; auto input = limitOrder(); input.orderId = "123"; input.orderListId = "12";
    QJsonObject body{{"symbol","BTCUSDT"},{"orderListId","12"},{"listOrderStatus","ALL_DONE"},{"orders",QJsonArray{QJsonObject{{"symbol","BTCUSDT"},{"orderId","123"}},QJsonObject{{"symbol","BTCUSDT"},{"orderId","124"}}}}};
    auto parsed = adapter.parse(Operation::OrderList, response(QJsonDocument(body).toJson()), input);
    QVERIFY(parsed.ok); QCOMPARE(parsed.data.value("listOrderStatus").toString(), QString("ALL_DONE"));
    body["orderListId"] = "13"; QVERIFY(!adapter.parse(Operation::OrderList, response(QJsonDocument(body).toJson()), input).ok);
    HttpResult timeout; timeout.timedOut = true;
    const auto failedRead = adapter.parse(Operation::Account, timeout, input); QVERIFY(!failedRead.ok); QVERIFY(!failedRead.unknown);
}

QTEST_APPLESS_MAIN(BinanceTest)
#include "tst_binance.moc"
