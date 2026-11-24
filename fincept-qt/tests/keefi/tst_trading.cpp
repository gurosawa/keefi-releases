#include "keefi/TradingController.h"
#include "keefi/BuildPolicy.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/UpbitResponses.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <functional>
#include <optional>
#include <sodium.h>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
using namespace keefi;
namespace {
const QString Password = "Fake test password for G2 vault!";
class FakeTransport final : public TradingTransport {
  public:
    using TradingTransport::TradingTransport;
    QList<Operation> sent;bool holdHistory=false;quint64 historyToken=0;
    QMap<QString, QString> clients;
    bool timeoutMutation = false, missingQuery = false, holdMutation = false, staleQuote = false;
    bool journalBeforeSend = false, localBlockMutation = false;
    bool zeroRemainingOnce = false, blockTest = false, twoOrders = false;
    QString rejectCancelId;
    QElapsedTimer clock;
    QList<qint64> sendTimes;
    std::function<void(Operation)> beforeResponse;
    std::optional<HttpResult> marketResponse;
    Vault* vault = nullptr;
    QSet<QString> canceled;
    int aborted = 0;
    quint64 epoch = 0;
    void abort() override { ++epoch; ++aborted; }
    void send(quint64 token, const RequestSpec& spec) override {
        if (!clock.isValid()) clock.start();
        sendTimes.append(clock.elapsed());
        sent.append(spec.operation);if(spec.operation==Operation::History&&holdHistory){historyToken=token;return;}
        if(isMutation(spec.operation)&&localBlockMutation){
            journalBeforeSend=vault&&!vault->pendingOperations().isEmpty();
            HttpResult result;result.status=429;result.localRetryAfterSeconds=90;result.headers["Retry-After"]="90";
            QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});return;
        }
        OrderInput input = upbit_fixture::input();
        const auto body = QJsonDocument::fromJson(spec.body).object();
        const QUrlQuery query(spec.url);
        input.orderId = query.queryItemValue("uuid"); input.clientId = query.queryItemValue("identifier");
        if (spec.operation == Operation::PlaceOrder || spec.operation == Operation::TestOrder) {
            input.clientId = body.value("identifier").toString();
            clients["fake-new-order"] = input.clientId;
        }
        if (input.clientId.isEmpty() && clients.contains(input.orderId)) input.clientId = clients.value(input.orderId);
        if (spec.operation == Operation::CancelOrder) canceled.insert(input.orderId);
        if (isMutation(spec.operation)) {
            journalBeforeSend = vault && !vault->pendingOperations().isEmpty();
            if (holdMutation) return;
        }
        HttpResult response = upbit_fixture::response(spec.operation, input);
        if (spec.operation == Operation::Markets && marketResponse) response = *marketResponse;
        if (spec.operation == Operation::OpenOrders && twoOrders)
            response = upbit_fixture::json(QJsonArray{upbit_fixture::order(), upbit_fixture::order("wait", "fake-order-2", "external-2")});
        if (zeroRemainingOnce) { response.headers["Remaining-Req"] = "group=default; min=1800; sec=0"; zeroRemainingOnce = false; }
        if (spec.operation == Operation::TestOrder && blockTest)
            response = upbit_fixture::json(QJsonObject{{"error", QJsonObject{{"name", "temporarily_blocked"}}}}, 418);
        if (spec.operation == Operation::CancelOrder && input.orderId == rejectCancelId) {
            canceled.remove(input.orderId);
            response = upbit_fixture::json(QJsonObject{{"error", QJsonObject{{"name", "validation_error"}}}}, 400);
        }
        if (spec.operation == Operation::QueryOrder) {
            if (missingQuery) response = upbit_fixture::json(QJsonObject{{"error", QJsonObject{{"name", "order_not_found"}}}}, 404);
            else if (canceled.contains(input.orderId)) response = upbit_fixture::json(upbit_fixture::order("cancel", input.orderId));
        }
        if (spec.operation == Operation::Quote && staleQuote) {
            auto array = QJsonDocument::fromJson(response.body).array(); auto quote = array.first().toObject();
            quote["timestamp"] = QDateTime::currentMSecsSinceEpoch() - 60000; array[0] = quote;
            response = upbit_fixture::json(array);
        }
        if (isMutation(spec.operation) && timeoutMutation) { response = {}; response.timedOut = true; response.networkError = true; }
        const auto generation = epoch;
        QTimer::singleShot(1, this, [this, generation, token, response, operation = spec.operation] {
            if (generation != epoch) return;
            if (beforeResponse) beforeResponse(operation);
            if (generation == epoch) emit finished(token, response);
        });
    }
};
class CatalogTransport final : public TradingTransport {
  public:
    using TradingTransport::TradingTransport;
    QList<RequestSpec> requests;
    QList<quint64> tokens;
    int aborted = 0;
    void abort() override { ++aborted; }
    void send(quint64 token, const RequestSpec& request) override { tokens.append(token); requests.append(request); }
    void respond(const HttpResult& response) { emit finished(tokens.last(), response); }
    void respondLate(quint64 token, const HttpResult& response) { emit finished(token, response); }
};
QJsonObject binanceInstrument(const QString& symbol = "BTCUSDT", const QString& base = "BTC",
                             const QString& state = "TRADING", bool spot = true) {
    return {{"symbol", symbol}, {"baseAsset", base}, {"quoteAsset", "USDT"}, {"status", state}, {"isSpotTradingAllowed", spot}};
}
HttpResult binanceCatalog(const QJsonArray& instruments = {binanceInstrument()}) {
    return upbit_fixture::json(QJsonObject{{"symbols", instruments}});
}
bool addFakeBinance(Vault& vault, const QString& environment = "live") {
    AccountRecord account;
    account.provider = "binance"; account.alias = "가짜 Binance"; account.environment = environment;
    account.market = "spot"; account.currency = "USDT";
    return vault.addAccount(account, {{"api_key", "CATALOG_FAKE_KEY_ONLY"}, {"secret", "CATALOG_FAKE_SECRET_ONLY"}}, StorageMode::EncryptedVault);
}
bool addFake(Vault& vault, StorageMode mode = StorageMode::EncryptedVault) {
    auto account = upbit_fixture::account(); account.id.clear();
    return vault.addAccount(account, {{"access_key", "G2_FAKE_KEY_ONLY"}, {"secret", "G2_FAKE_SECRET_ONLY"}}, mode);
}
class BinanceSupplementTransport final:public TradingTransport {
public:
    Vault* vault=nullptr;QString mode,newClient;QList<Operation> sent;bool journalBeforeSend=false,mutated=false;quint64 epoch=0;
    void abort()override{++epoch;}
    QJsonObject row(const QString& id,const QString& state,const QString& client="external-order",const QString& quantity="10",const QString& price="100")const{
        return {{"symbol","BTCUSDT"},{"orderId",id},{"orderListId",mode=="list-close"?"12":"-1"},{"clientOrderId",client},{"price",price},{"origQty",quantity},{"executedQty",id=="124"?"0":"2"},{"cummulativeQuoteQty","0"},{"status",state},{"timeInForce","GTC"},{"type","LIMIT"},{"side","BUY"}};
    }
    void send(quint64 token,const RequestSpec& request)override{
        sent.append(request.operation);QJsonObject value;QJsonArray array;bool isArray=false;HttpResult r;r.status=200;
        switch(request.operation){
        case Operation::Time:value={{"serverTime",QString::number(QDateTime::currentMSecsSinceEpoch())}};break;
        case Operation::Rules:{const auto instrument=parseExactJson(R"({"symbol":"BTCUSDT","status":"TRADING","baseAsset":"BTC","quoteAsset":"USDT","isSpotTradingAllowed":true,"amendAllowed":true,"orderTypes":["LIMIT","MARKET"],"filters":[{"filterType":"PRICE_FILTER","minPrice":"0.01","maxPrice":"1000000","tickSize":"0.01"},{"filterType":"LOT_SIZE","minQty":"0.001","maxQty":"1000","stepSize":"0.001"}]})").object();value={{"symbols",QJsonArray{instrument}},{"exchangeFilters",QJsonArray{}},{"rateLimits",QJsonArray{}}};break;}
        case Operation::Quote:value={{"symbol","BTCUSDT"},{"price","100"}};break;
        case Operation::Depth:value=parseExactJson(R"({"lastUpdateId":1,"bids":[["99","10"]],"asks":[["101","10"]]})").object();break;
        case Operation::Reference:value={{"symbol","BTCUSDT"},{"referencePrice",QJsonValue::Null}};break;
        case Operation::AveragePrice:value={{"mins","5"},{"price","100"},{"closeTime",QString::number(QDateTime::currentMSecsSinceEpoch())}};break;
        case Operation::AssetFilters:value={{"symbolFilters",QJsonArray{}},{"exchangeFilters",QJsonArray{}},{"assetFilters",QJsonArray{}}};break;
        case Operation::Account:value={{"canTrade",true},{"accountType","SPOT"},{"commissionRates",QJsonObject{{"maker","0.001"},{"taker","0.001"}}},{"balances",QJsonArray{QJsonObject{{"asset","USDT"},{"free","20000"},{"locked","0"}},QJsonObject{{"asset","BTC"},{"free","10"},{"locked","0"}}}}};break;
        case Operation::OpenOrders:isArray=true;array={row("123","PARTIALLY_FILLED")};break;
        case Operation::History:QFAIL("Initial trading preparation must not fetch history");break;
        case Operation::TestOrder:break;
        case Operation::AmendOrder:{mutated=true;journalBeforeSend=vault&&!vault->pendingOperations().isEmpty();const auto params=QUrlQuery(QString::fromUtf8(request.body));newClient=params.queryItemValue("newClientOrderId");QCOMPARE(request.url.path(),QString("/api/v3/order/cancelReplace"));
            if(mode=="replace-timeout"){r.timedOut=true;r.status=0;break;}
            r.status=409;value={{"code","-2021"},{"data",QJsonObject{{"cancelResult","SUCCESS"},{"newOrderResult","FAILURE"},{"cancelResponse",row("123","CANCELED")},{"newOrderResponse",QJsonObject{{"code","-2010"}}}}}};break;}
        case Operation::QueryOrder:{const QUrlQuery q(request.url);if(!q.queryItemValue("orderId").isEmpty())value=row("123",mutated&&mode!="replace-timeout"?"CANCELED":"PARTIALLY_FILLED");else{r.status=400;value={{"code","-2013"}};}break;}
        case Operation::CancelOrder:QCOMPARE(request.url.path(),QString("/api/v3/orderList"));mutated=true;journalBeforeSend=vault&&!vault->pendingOperations().isEmpty();[[fallthrough]];
        case Operation::OrderList:value={{"symbol","BTCUSDT"},{"orderListId","12"},{"listOrderStatus","ALL_DONE"},{"orders",QJsonArray{QJsonObject{{"symbol","BTCUSDT"},{"orderId","123"}},QJsonObject{{"symbol","BTCUSDT"},{"orderId","125"}}}}};break;
        default:QFAIL("Unexpected request in supplemental order test");break;
        }
        if(!r.timedOut)r.body=isArray?QJsonDocument(array).toJson():QJsonDocument(value).toJson();const auto generation=epoch;QTimer::singleShot(1,this,[this,token,r,generation]{if(epoch==generation)emit finished(token,r);});
    }
};
template<class Transport> void verifyReadOnlyNoWrites(TradingController& controller, Transport& transport, Vault& vault) {
    QVERIFY(readOnlyBuild()); QVERIFY(!controller.liveEnabled()); QVERIFY(!controller.busy());
    for (auto operation : transport.sent) { QVERIFY(!isMutation(operation)); QVERIFY(operation != Operation::TestOrder); }
    QVERIFY(vault.pendingOperations().isEmpty());
}
void activate(QPushButton& button) {
    button.show(); button.activateWindow(); QApplication::setActiveWindow(&button); button.setFocus();
    QCoreApplication::processEvents();
}
}
class TradingTests final : public QObject {
    Q_OBJECT
  private slots:
    void publicUpbitCatalogNeverAuthenticatesOrEnablesTrading() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFake(vault));
        QFile encrypted(vault.filePath()); QVERIFY(encrypted.open(QIODevice::ReadOnly)); const auto before=encrypted.readAll(); encrypted.close();
        CatalogTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(transport.requests.isEmpty());
        controller.loadMarkets(); controller.loadMarkets(); // Duplicate UI input cannot overlap requests.
        QCOMPARE(transport.requests.size(), 1);
        const auto request = transport.requests.first();
        QCOMPARE(request.operation, Operation::Markets); QVERIFY(!request.authenticated);
        QCOMPARE(request.url.host(), QString("api.upbit.com")); QCOMPARE(request.url.path(), QString("/v1/market/all"));
        QCOMPARE(QUrlQuery(request.url).queryItemValue("is_details"), QString("true"));
        QCOMPARE(request.headers.size(), 1); QCOMPARE(request.headers.value("Accept"), QByteArray("application/json"));
        QVERIFY(request.body.isEmpty());
        auto rows = QJsonDocument::fromJson(upbit_fixture::response(Operation::Markets).body).array();
        auto first = rows.first().toObject();
        first["market_event"] = QJsonObject{{"warning", true}, {"caution", QJsonObject{{"PRICE_FLUCTUATIONS", true}}}};
        rows[0] = first; transport.respond(upbit_fixture::json(rows));
        QVERIFY(!controller.busy()); QVERIFY(controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 1);
        const auto instrument = controller.instruments().first();
        QCOMPARE(instrument.symbol, QString("KRW-BTC")); QCOMPARE(instrument.baseAsset, QString("BTC"));
        QCOMPARE(instrument.quoteAsset, QString("KRW")); QCOMPARE(instrument.name, QString("가짜 비트코인"));
        QCOMPARE(instrument.englishName, QString("Fake Bitcoin")); QVERIFY(instrument.tradable);
        QVERIFY(instrument.status.contains("상태 조회 필요")); QVERIFY(instrument.warning.contains("거래 유의"));
        QVERIFY(instrument.warning.contains("거래 주의"));
        QVERIFY(controller.catalogStatus().contains("Upbit 공식 /v1/market/all"));
        QVERIFY(controller.catalogStatus().contains("수신")); QVERIFY(controller.catalogStatus().contains("5분"));
        QVERIFY(!controller.connected()); QVERIFY(!controller.liveEnabled()); QVERIFY(!controller.quoteFresh());
        QVERIFY(controller.balances().isEmpty()); QVERIFY(controller.symbol().isEmpty()); QVERIFY(controller.coverage().isEmpty());
        QVERIFY(controller.verificationReceipt().value("checks").toObject().isEmpty());
        QVERIFY(!controller.preview(upbit_fixture::input()).ok);
        QVERIFY(encrypted.open(QIODevice::ReadOnly)); QCOMPARE(encrypted.readAll(),before); // Public browsing does not rewrite the existing vault.
    }
    void initTestCase() { QVERIFY(sodium_init() >= 0); }
    void balanceCompletenessFollowsReadLifecycle() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFake(vault));
        CatalogTransport transport; TradingController controller(vault,&transport); QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(!controller.assetReadComplete()); QVERIFY(controller.refreshAssets()); QVERIFY(!controller.assetReadComplete());
        transport.respond(upbit_fixture::response(Operation::Account)); QVERIFY(!controller.busy());
        QVERIFY(controller.assetReadComplete()); QVERIFY(controller.assetCapture().snapshot.balanceComplete);
        vault.lock(); QVERIFY(!controller.assetReadComplete()); QVERIFY(controller.assetCapture().snapshot.id.isEmpty());
    }
    void duplicateCatalogInputKeepsTheInFlightReply_data() {
        QTest::addColumn<bool>("binance"); QTest::newRow("upbit") << false; QTest::newRow("binance") << true;
    }
    void duplicateCatalogInputKeepsTheInFlightReply() {
        QFETCH(bool, binance); QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
        QVERIFY(binance ? addFakeBinance(vault) : addFake(vault)); CatalogTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); controller.loadMarkets();
        const auto firstToken = transport.tokens.first(); const auto abortsBefore = transport.aborted; controller.loadMarkets();
        QCOMPARE(transport.requests.size(), 1); QCOMPARE(transport.aborted, abortsBefore);
        transport.respondLate(firstToken, binance ? binanceCatalog() : upbit_fixture::response(Operation::Markets));
        QVERIFY(controller.catalogFresh()); QVERIFY(!controller.busy());
        controller.loadMarkets(); QCOMPARE(transport.requests.size(), 2); // A completed catalogue can be explicitly refreshed.
    }




    void binanceCatalogPreservesUtf8AndReportsStatus() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFakeBinance(vault, "live"));
        CatalogTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); controller.loadMarkets();
        QCOMPARE(transport.requests.size(), 1);
        const auto request = transport.requests.first(); QVERIFY(!request.authenticated);
        QCOMPARE(request.url.host(), QString("api.binance.com")); QCOMPARE(request.url.path(), QString("/api/v3/exchangeInfo"));
        QVERIFY(!request.headers.contains("X-MBX-APIKEY")); QVERIFY(request.body.isEmpty());
        const QString symbol = QString::fromUtf8("币安人生USDT");
        transport.respond(binanceCatalog({binanceInstrument(symbol, "币安人生"),
            binanceInstrument("ETHUSDT", "ETH", "HALT"), binanceInstrument("BNBUSDT", "BNB", "TRADING", false)}));
        QVERIFY(controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 3);
        const auto rows = controller.instruments();
        QCOMPARE(rows[0].symbol, symbol); QCOMPARE(rows[0].baseAsset, QString("币安人生"));
        QVERIFY(rows[0].name.isEmpty()); QVERIFY(rows[0].englishName.isEmpty()); QVERIFY(rows[0].tradable);
        QVERIFY(!rows[1].tradable); QVERIFY(rows[1].status.contains("중단"));
        QVERIFY(!rows[2].tradable); QVERIFY(rows[2].status.contains("현물 미지원"));
        QVERIFY(controller.catalogStatus().contains("Binance Spot 공식")); QVERIFY(!controller.catalogStatus().contains("Testnet")); QVERIFY(!controller.connected());
        QVERIFY(controller.setLimits({"1000", "币安人生", "5"}));
        controller.refresh(symbol);
        QCOMPARE(transport.requests.last().operation, Operation::Time); // No ASCII-only input rejection.
        transport.respond(upbit_fixture::json(QJsonObject{{"serverTime", QDateTime::currentMSecsSinceEpoch()}}));
        QTRY_COMPARE_WITH_TIMEOUT(transport.requests.last().operation, Operation::Rules, 1500);
        QCOMPARE(QUrlQuery(transport.requests.last().url).queryItemValue("symbol"), symbol);
        controller.stop();
    }
    void failedCatalogPreservesPreviousRowsButDisablesSelection() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFakeBinance(vault));
        CatalogTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); controller.loadMarkets(); transport.respond(binanceCatalog());
        QVERIFY(controller.catalogFresh());
        const QList<HttpResult> invalidResponses{
            binanceCatalog({binanceInstrument(), binanceInstrument()}),
            binanceCatalog({QJsonObject{{"symbol", "BTCUSDT"}}}),
            binanceCatalog({binanceInstrument("BTC/USDT")}),
            upbit_fixture::json(QJsonObject{{"symbols", QJsonArray{}}}),
            upbit_fixture::json(QJsonObject{{"msg", "fake server error"}}, 500),
            HttpResult{200, QByteArray(4 * 1024 * 1024 + 1, ' ')}};
        for (const auto& response : invalidResponses) {
            controller.loadMarkets(); transport.respond(response);
            QVERIFY(!controller.busy()); QVERIFY(!controller.catalogFresh());
            QCOMPARE(controller.instruments().size(), 1); QCOMPARE(controller.instruments().first().symbol, QString("BTCUSDT"));
            QVERIFY(controller.catalogStatus().contains("조회 실패")); QVERIFY(controller.catalogStatus().contains("만료"));
            QVERIFY(controller.catalogStatus().contains("수신")); QVERIFY(!controller.connected());
            QVERIFY(controller.verificationReceipt().value("checks").toObject().isEmpty());
        }
        controller.loadMarkets(); transport.respond(binanceCatalog()); QVERIFY(controller.catalogFresh());
        QVERIFY(!controller.catalogStatus().contains("조회 실패"));
    }
    void stopSwitchAndLockDiscardCatalogAndLateReplies() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFake(vault));
        const auto upbitId = vault.accounts().first().id;
        QVERIFY(addFakeBinance(vault)); QString binanceId;
        for (const auto& account : vault.accounts()) if (account.provider == "binance") binanceId = account.id;
        QVERIFY(!binanceId.isEmpty());
        CatalogTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(upbitId)); controller.loadMarkets(); transport.respond(upbit_fixture::response(Operation::Markets));
        QVERIFY(controller.catalogFresh()); controller.loadMarkets(); const auto oldToken = transport.tokens.last();
        controller.stop(); QVERIFY(controller.instruments().isEmpty()); QVERIFY(!controller.catalogFresh());
        QVERIFY(controller.selectAccount(binanceId)); controller.loadMarkets();
        transport.respondLate(oldToken, upbit_fixture::response(Operation::Markets));
        QVERIFY(controller.busy()); QVERIFY(controller.instruments().isEmpty());
        transport.respond(binanceCatalog()); QVERIFY(controller.catalogFresh()); QCOMPARE(controller.instruments().first().symbol, QString("BTCUSDT"));
        controller.loadMarkets(); const auto lockedToken = transport.tokens.last(); vault.lock();
        QVERIFY(!controller.busy()); QVERIFY(!controller.supported()); QVERIFY(controller.instruments().isEmpty());
        transport.respondLate(lockedToken, binanceCatalog()); QVERIFY(controller.instruments().isEmpty()); QVERIFY(!controller.catalogFresh());
        QVERIFY(transport.aborted >= 3);
    }
    void catalogRateLimitCannotStartAuthenticatedPollingOrRetry() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFake(vault));
        CatalogTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); controller.loadMarkets(); transport.respond(upbit_fixture::response(Operation::Markets));
        controller.loadMarkets();
        transport.respond(upbit_fixture::json(QJsonObject{{"error", QJsonObject{{"name", "temporarily_blocked"}}}}, 418));
        QVERIFY(!controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 1);
        controller.loadMarkets(); QTest::qWait(350);
        QCOMPARE(transport.requests.size(), 2); QVERIFY(!controller.busy()); QVERIFY(!controller.connected());
        QVERIFY(controller.verificationReceipt().value("checks").toObject().isEmpty());
        QVERIFY(controller.catalogStatus().contains("제한"));
    }
    void catalogFailureDoesNotForgeOrEraseAuthenticatedResults() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password)); QVERIFY(addFake(vault));
        FakeTransport transport; TradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); controller.refresh("KRW-BTC");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 15000); QVERIFY(controller.connected());
        QVERIFY(controller.catalogFresh()); QCOMPARE(controller.instruments().first().name, QString("가짜 비트코인"));
        const auto checks = controller.verificationReceipt().value("checks"); const auto coverage = controller.coverage();
        const auto balanceCount = controller.balances().size();
        transport.sent.clear(); transport.marketResponse = upbit_fixture::json(QJsonObject{}, 500);
        controller.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(transport.sent, QList<Operation>{Operation::Markets}); QVERIFY(controller.connected());
        QVERIFY(!controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 1);
        QCOMPARE(controller.verificationReceipt().value("checks"), checks); QCOMPARE(controller.coverage(), coverage);
        QCOMPARE(controller.balances().size(), balanceCount); QVERIFY(!controller.liveEnabled());
        // The same failure in an explicit authenticated refresh must still stop
        // the normal refresh plan, before any authenticated operation is sent.
        transport.sent.clear(); controller.refresh("KRW-BTC"); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(transport.sent, QList<Operation>{Operation::Markets}); QVERIFY(!controller.connected());
        QVERIFY(!controller.catalogFresh()); QCOMPARE(controller.instruments().size(), 1);
    }
    void exactDecimalAndJson() {
        QCOMPARE(decimal::add("0.1", "0.2"), QString("0.3"));
        QCOMPARE(decimal::multiply("12345678901234567890.12345678", "0.00000001"), QString("123456789012.3456789012345678"));
        QCOMPARE(decimal::subtract("100.00000001", "0.00000001"), QString("100"));
        QVERIFY(decimal::multipleOf("0.00000015", "0.00000005"));
        QVERIFY(!decimal::multipleOf("0.00000016", "0.00000005"));
        QVERIFY(!decimal::valid("1e3")); QVERIFY(!decimal::valid("NaN")); QVERIFY(!decimal::valid("-1"));
        QVERIFY(decimal::subtract("1", "2").isEmpty());
        auto json = parseExactJson(R"({"id":9007199254740993,"price":1.234e-7,"literal":"1.2e3","nested":[true,null,-1007]})").object();
        QCOMPARE(json.value("id").toString(), QString("9007199254740993"));
        QCOMPARE(json.value("price").toString(), QString("0.0000001234"));
        QCOMPARE(json.value("literal").toString(), QString("1.2e3"));
        QVERIFY(json.value("nested").toArray()[0].toBool());
        QCOMPARE(json.value("nested").toArray()[2].toString(), QString("-1007"));
        QVERIFY(parseExactJson("{\"x\":01}").isNull());
        QVERIFY(parseExactJson(QByteArray(4 * 1024 * 1024 + 1, ' ')).isNull());
    }
    void exactJsonResponseSizeBoundaries() {
        // Valid JSON padded to exact byte limits, so rejection cannot be
        // explained by malformed syntax. No large payload is printed.
        QByteArray bytes = R"({"id":9007199254740993})";
        bytes.append(MaxStandardResponseBytes - bytes.size(), ' ');
        QCOMPARE(bytes.size(), MaxStandardResponseBytes);
        QCOMPARE(parseExactJson(bytes).object().value("id").toString(), QString("9007199254740993"));
        bytes.append(' ');
        QString error;
        QVERIFY(parseExactJson(bytes, &error).isNull()); QVERIFY(!error.isEmpty());
        QVERIFY(parseExactJson(bytes, nullptr, static_cast<JsonResponseScope>(999)).isNull());
        QCOMPARE(parseExactJson(bytes, nullptr, JsonResponseScope::BinanceMarketCatalog)
            .object().value("id").toString(), QString("9007199254740993"));
        bytes.append(MaxBinanceMarketCatalogResponseBytes - bytes.size(), ' ');
        QCOMPARE(bytes.size(), MaxBinanceMarketCatalogResponseBytes);
        QCOMPARE(parseExactJson(bytes, nullptr, JsonResponseScope::BinanceMarketCatalog)
            .object().value("id").toString(), QString("9007199254740993"));
        QVERIFY(parseExactJson(bytes).isNull());
        bytes.append(' '); error.clear();
        QVERIFY(parseExactJson(bytes, &error, JsonResponseScope::BinanceMarketCatalog).isNull());
        QVERIFY(!error.isEmpty());
    }
    void transportLargeResponseRequiresUnsignedBinanceCatalog() {
        RequestSpec catalog;
        catalog.operation = Operation::Markets;
        catalog.url = QUrl("https://api.binance.com/api/v3/exchangeInfo?permissions=SPOT&showPermissionSets=false");
        catalog.headers.insert("Accept", "application/json");
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(catalog), MaxBinanceMarketCatalogResponseBytes);
        auto altered = catalog;
        altered.url = QUrl("https://testnet.binance.vision/api/v3/exchangeInfo");
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(altered), MaxStandardResponseBytes);
        QVERIFY(!NetworkTradingTransport::allowed(altered));
        for (const auto& url : {
            "https://api.upbit.com/v1/market/all", "https://api.binance.com/api/v3/account",
            "https://api.binance.com/api/v3/openOrders", "https://api.binance.com/api/v3/order",
            "https://api.binance.com/api/v3/exchangeInfo/", "https://api.binance.com/api/v3%2FexchangeInfo",
            "https://api.binance.com.evil.invalid/api/v3/exchangeInfo", "http://api.binance.com/api/v3/exchangeInfo",
            "https://api.binance.com:444/api/v3/exchangeInfo", "https://user@api.binance.com/api/v3/exchangeInfo",
            "https://api.binance.com/api/v3/exchangeInfo#fragment"}) {
            altered = catalog; altered.url = QUrl(url);
            QVERIFY2(NetworkTradingTransport::responseBodyLimit(altered) == MaxStandardResponseBytes, url);
        }
        for (const auto operation : {Operation::Time, Operation::Rules, Operation::Account, Operation::OpenOrders,
            Operation::History, Operation::QueryOrder, Operation::TestOrder, Operation::PlaceOrder,
            Operation::CancelOrder, Operation::AmendOrder}) {
            altered = catalog; altered.operation = operation;
            QCOMPARE(NetworkTradingTransport::responseBodyLimit(altered), MaxStandardResponseBytes);
        }
        for (const auto& method : {"POST", "DELETE", "PUT", "get"}) {
            altered = catalog; altered.method = method;
            QVERIFY2(NetworkTradingTransport::responseBodyLimit(altered) == MaxStandardResponseBytes, method);
        }
        altered = catalog; altered.authenticated = true;
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(altered), MaxStandardResponseBytes);
        altered = catalog; altered.body = "fake-body";
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(altered), MaxStandardResponseBytes);
        for (const auto& header : {"Authorization", "aUtHoRiZaTiOn", "X-MBX-APIKEY", "x-mbx-apikey", "Proxy-Authorization", "Cookie"}) {
            altered = catalog; altered.headers.insert(header, "FAKE_ONLY");
            QVERIFY2(NetworkTradingTransport::responseBodyLimit(altered) == MaxStandardResponseBytes, header);
        }
        for (const auto& parameter : {"signature=FAKE_ONLY", "SIGNATURE=FAKE_ONLY", "%73ignature=FAKE_ONLY", "timestamp=1", "recvWindow=5000"}) {
            altered = catalog;
            altered.url.setQuery(QStringLiteral("permissions=SPOT&") + QString::fromLatin1(parameter));
            QVERIFY2(NetworkTradingTransport::responseBodyLimit(altered) == MaxStandardResponseBytes, parameter);
        }
    }
    void catalogResponseLimitRemainsProviderSpecific() {
        for (const bool binance : {false, true}) {
            QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
            QVERIFY(binance ? addFakeBinance(vault) : addFake(vault));
            CatalogTransport transport; TradingController controller(vault, &transport);
            QVERIFY(controller.selectAccount(vault.accounts().first().id));
            controller.loadMarkets();
            auto response = binance ? binanceCatalog() : upbit_fixture::response(Operation::Markets);
            response.body.append(MaxStandardResponseBytes + 1 - response.body.size(), ' ');
            transport.respond(response);
            QVERIFY(!controller.busy());
            QCOMPARE(controller.catalogFresh(), binance);
            QCOMPARE(controller.instruments().size(), binance ? 1 : 0);
            QVERIFY(!controller.connected()); QVERIFY(!controller.liveEnabled());
            QCOMPARE(transport.requests.size(), 1);
            QVERIFY(!transport.requests.first().authenticated);
        }
    }
    void restrictTransportDestinations() {
        RequestSpec request; request.url = QUrl("https://api.upbit.com/v1/orders"); request.method = "POST";
        QVERIFY(!NetworkTradingTransport::allowed(request)); // A destination alone is not financial authority.
        request.operation=Operation::PlaceOrder;request.authenticated=true;request.headers["Authorization"]="Bearer FAKE";
        request.body=R"({"market":"KRW-BTC","side":"bid","ord_type":"limit","volume":"1","price":"100","identifier":"fake"})";
        QCOMPARE(NetworkTradingTransport::allowed(request),!readOnlyBuild());
        for (const auto& url : {"http://api.upbit.com/v1/orders", "https://attacker.invalid/v1/orders", "https://api.upbit.com:444/v1/orders",
            "https://user:pass@api.upbit.com/v1/orders", "https://api.binance.com/sapi/v1/anything", "https://api.upbit.com/v1/orders#fragment"}) {
            request.url = QUrl(url); QVERIFY(!NetworkTradingTransport::allowed(request));
        }
    }
    void brokerTransportBindsEndpointMethodAndOperation() {
        RequestSpec request; request.url = QUrl("https://openapi.tossinvest.com/oauth2/token");
        request.method = "POST"; request.operation = Operation::Authenticate;
        QVERIFY(NetworkTradingTransport::allowed(request));
        request.method = "GET"; QVERIFY(!NetworkTradingTransport::allowed(request));
        request.method = "POST"; request.operation = Operation::PlaceOrder;
        QVERIFY(!NetworkTradingTransport::allowed(request));
        request.url = QUrl("https://openapi.tossinvest.com/api/v1/orders");
        QCOMPARE(NetworkTradingTransport::allowed(request),!readOnlyBuild());
        request.operation = Operation::TestOrder; QVERIFY(!NetworkTradingTransport::allowed(request));
        request.operation = Operation::CancelOrder;
        request.url = QUrl("https://openapi.tossinvest.com/api/v1/orders/fake-id_01/cancel");
        QCOMPARE(NetworkTradingTransport::allowed(request),!readOnlyBuild());
        for (const auto& path : {"/api/v1/orders/fake-id_01/modify", "/api/v1/orders/fake%2Fid/cancel",
                                "/api/v1/orders/fake-id_01/cancel?token=unused", "/oauth2/token"}) {
            request.url = QUrl(QString("https://openapi.tossinvest.com") + path);
            QVERIFY(!NetworkTradingTransport::allowed(request));
        }
        request.method = "GET"; request.operation = Operation::Markets;
        request.url = QUrl("https://openapi.tossinvest.com/api/v1/stocks/all?market=KOSPI");
        QVERIFY(NetworkTradingTransport::allowed(request));
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(request), MaxStandardResponseBytes);
        request.operation = Operation::Accounts; QVERIFY(!NetworkTradingTransport::allowed(request));
        request.url = QUrl("https://openapi.tossinvest.com/api/v1/accounts");
        QVERIFY(NetworkTradingTransport::allowed(request));
        for (const auto& url : {"https://openapi.tossinvest.com.attacker.invalid/api/v1/accounts",
                                "https://openapi.tossinvest.com:444/api/v1/accounts", "http://openapi.tossinvest.com/api/v1/accounts",
                                "https://user:pass@openapi.tossinvest.com/api/v1/accounts", "https://openapi.tossinvest.com/api/v1/accounts#fragment"}) {
            request.url = QUrl(url); QVERIFY(!NetworkTradingTransport::allowed(request));
        }
    }
    void journalSchemaAndEncryptedRestart() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
        QJsonObject entry{{"id", "fx-journal"}, {"accountId", "fake-account"}, {"fingerprint", QString(64, 'a')},
            {"provider", "upbit"}, {"environment", "live"}, {"symbol", "KRW-BTC"}, {"kind", "place"},
            {"clientId", "fake-client-secret-metadata"}, {"orderId", ""}, {"replacementClientId", ""},
            {"state", "unknown"}, {"createdAt", "1792200000000"}, {"quantity", "0.1"}, {"price", "100000"},
            {"quoteAmount", ""}, {"side", "buy"}, {"type", "limit"}};
        QVERIFY(vault.setPendingOperations({entry}));
        QFile file(vault.filePath()); QVERIFY(file.open(QIODevice::ReadOnly)); const auto disk = file.readAll(); file.close();
        QVERIFY(!disk.contains("fake-client-secret-metadata")); QVERIFY(!disk.contains("KRW-BTC"));
        auto bad = entry; bad["secret"] = "disallowed"; QVERIFY(!vault.setPendingOperations({bad}));
        QCOMPARE(vault.pendingOperations().size(), 1);
        QVERIFY(vault.changePassword(Password, Password + " changed"));
        vault.lock(); QVERIFY(vault.pendingOperations().isEmpty());
        QVERIFY(vault.unlock(Password + " changed")); QCOMPARE(vault.pendingOperations().first().toObject(), entry);
    }










};
QTEST_MAIN(TradingTests)
#include "tst_trading.moc"
