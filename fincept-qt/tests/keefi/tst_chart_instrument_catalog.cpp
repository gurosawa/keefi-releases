#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

using namespace keefi;

namespace {
class PublicTransport final : public TradingTransport {
  public:
    QList<RequestSpec> requests;
    QList<quint64> tokens;
    int aborts = 0;
    void send(quint64 token, const RequestSpec& request) override { tokens.append(token); requests.append(request); }
    void abort() override { ++aborts; }
    void reply(int index, const HttpResult& response) { emit finished(tokens.at(index), response); }
};
HttpResult json(const QJsonObject& object) { HttpResult result; result.status = 200; result.body = QJsonDocument(object).toJson(); return result; }
HttpResult json(const QJsonArray& array) { HttpResult result; result.status = 200; result.body = QJsonDocument(array).toJson(); return result; }
QJsonObject binanceInstrument(QString symbol = "SYNUSDT", QString base = "SYN", QString quote = "USDT") {
    return {{"symbol", symbol}, {"baseAsset", base}, {"quoteAsset", quote}, {"status", "TRADING"}, {"isSpotTradingAllowed", true}};
}
HttpResult binanceResponse() { return json(QJsonObject{{"symbols", QJsonArray{binanceInstrument()}}}); }
HttpResult bithumbResponse() {
    return json(QJsonArray{QJsonObject{{"market", "KRW-SYN"}, {"korean_name", "합성 종목"}, {"english_name", "Synthetic Asset"}, {"market_warning", "NONE"}}});
}
}

class ChartInstrumentCatalogTest final : public QObject {
    Q_OBJECT
  private slots:
    void binanceUsesOnlyTheDedicatedPublicCodec();
    void bithumbPublishesValidatedMarketIdentities();
    void cataloguesRejectMalformedOrAmbiguousSourceRows_data();
    void cataloguesRejectMalformedOrAmbiguousSourceRows();
    void requestsAreSequentialDeduplicatedAndExpire();
    void stopAndRateLimitRejectLateRepliesAndImmediateRetries();
};

void ChartInstrumentCatalogTest::binanceUsesOnlyTheDedicatedPublicCodec() {
    PublicTransport transport;
    ChartInstrumentCatalog catalog(&transport);
    QSignalSpy changed(&catalog, &ChartInstrumentCatalog::changed);
    catalog.ensure("binance");
    QCOMPARE(transport.requests.size(), 1);
    QVERIFY(catalog.busy());
    const auto request = transport.requests.first();
    QCOMPARE(request.operation, Operation::Markets);
    QCOMPARE(request.method, QByteArray("GET"));
    QCOMPARE(request.url.host(), QString("api.binance.com"));
    QCOMPARE(request.url.path(), QString("/api/v3/exchangeInfo"));
    QCOMPARE(QUrlQuery(request.url).queryItemValue("permissions"), QString("SPOT"));
    QCOMPARE(QUrlQuery(request.url).queryItemValue("showPermissionSets"), QString("false"));
    QVERIFY(!request.authenticated); QVERIFY(request.headers.isEmpty()); QVERIFY(request.body.isEmpty());
    transport.reply(0, binanceResponse());
    QVERIFY(!catalog.busy());
    const auto instruments = catalog.instruments("binance");
    QCOMPARE(instruments.size(), 1);
    QCOMPARE(instruments.first().symbol, QString("SYNUSDT"));
    QCOMPARE(instruments.first().baseAsset, QString("SYN"));
    QCOMPARE(instruments.first().quoteAsset, QString("USDT"));
    QCOMPARE(instruments.first().market, QString("spot"));
    QVERIFY(instruments.first().tradable);
    QVERIFY(!catalog.status("binance").isEmpty());
    QVERIFY(!changed.isEmpty());
}

void ChartInstrumentCatalogTest::bithumbPublishesValidatedMarketIdentities() {
    PublicTransport transport;
    ChartInstrumentCatalog catalog(&transport);
    catalog.ensure("bithumb"); QCOMPARE(transport.requests.size(), 1);
    const auto request = transport.requests.first();
    QCOMPARE(request.url.host(), QString("api.bithumb.com"));
    QCOMPARE(request.url.path(), QString("/v1/market/all"));
    QCOMPARE(QUrlQuery(request.url).queryItemValue("isDetails"), QString("true"));
    QVERIFY(!request.authenticated); QVERIFY(request.body.isEmpty());
    QCOMPARE(request.headers.size(), 1);
    QCOMPARE(request.headers.value("Accept"), QByteArray("application/json"));
    transport.reply(0, bithumbResponse());
    const auto instruments = catalog.instruments("bithumb");
    QCOMPARE(instruments.size(), 1);
    QCOMPARE(instruments.first().symbol, QString("KRW-SYN"));
    QCOMPARE(instruments.first().baseAsset, QString("SYN"));
    QCOMPARE(instruments.first().quoteAsset, QString("KRW"));
    QCOMPARE(instruments.first().market, QString("spot"));
    QVERIFY(catalog.instruments("binance").isEmpty());
}

void ChartInstrumentCatalogTest::cataloguesRejectMalformedOrAmbiguousSourceRows_data() {
    QTest::addColumn<QString>("provider"); QTest::addColumn<QByteArray>("body");
    const auto addBinance = [](const char* name, const QJsonArray& rows) {
        QTest::newRow(name) << QString("binance") << QJsonDocument(QJsonObject{{"symbols", rows}}).toJson();
    };
    addBinance("duplicate", {binanceInstrument(), binanceInstrument()});
    addBinance("empty", {});
    addBinance("symbol-base-disagreement", {binanceInstrument("SYNUSDT", "OTHER", "USDT")});
    auto invalid = binanceInstrument(); invalid["isSpotTradingAllowed"] = "true";
    addBinance("boolean-required", {invalid});
    invalid = binanceInstrument(); invalid["symbol"] = "SYNUSDT\n";
    addBinance("newline-symbol", {invalid});
    invalid = binanceInstrument(); invalid["status"] = "TRADING\n";
    addBinance("newline-status", {invalid});
    const auto bithumb = QJsonDocument::fromJson(bithumbResponse().body).array();
    QTest::newRow("bithumb-duplicate") << QString("bithumb") << QJsonDocument(QJsonArray{bithumb.first(), bithumb.first()}).toJson();
}

void ChartInstrumentCatalogTest::cataloguesRejectMalformedOrAmbiguousSourceRows() {
    QFETCH(QString, provider); QFETCH(QByteArray, body);
    PublicTransport transport;
    ChartInstrumentCatalog catalog(&transport);
    catalog.ensure(provider); QCOMPARE(transport.requests.size(), 1);
    HttpResult response; response.status = 200; response.body = body;
    transport.reply(0, response);
    QVERIFY(catalog.instruments(provider).isEmpty());
    QVERIFY(!catalog.busy());
    QVERIFY(!catalog.status(provider).isEmpty());
}

void ChartInstrumentCatalogTest::requestsAreSequentialDeduplicatedAndExpire() {
    PublicTransport transport;
    ChartInstrumentCatalog catalog(&transport);
    catalog.ensure("binance"); catalog.ensure("binance"); catalog.ensure("bithumb"); catalog.ensure("bithumb");
    QCOMPARE(transport.requests.size(), 1);
    transport.reply(0, binanceResponse());
    QTRY_COMPARE(transport.requests.size(), 2);
    QVERIFY(catalog.busy());
    QCOMPARE(transport.requests[1].url.host(), QString("api.bithumb.com"));
    transport.reply(1, bithumbResponse()); QVERIFY(!catalog.busy());
    catalog.ensure("binance"); catalog.ensure("bithumb"); QCOMPARE(transport.requests.size(), 2);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    QVERIFY(!catalog.instruments("binance", now).isEmpty());
    QVERIFY(catalog.instruments("binance", now + ChartInstrumentCatalog::FreshnessMs + 1).isEmpty());
    QVERIFY(catalog.instruments("binance", now - 10000).isEmpty());
    catalog.ensure("okx"); QCOMPARE(transport.requests.size(), 2);
    QVERIFY(catalog.instruments("okx").isEmpty());
}

void ChartInstrumentCatalogTest::stopAndRateLimitRejectLateRepliesAndImmediateRetries() {
    PublicTransport transport;
    ChartInstrumentCatalog catalog(&transport);
    catalog.ensure("binance"); catalog.ensure("bithumb"); QCOMPARE(transport.requests.size(), 1);
    catalog.stop(); QVERIFY(!catalog.busy()); QCOMPARE(transport.aborts, 1);
    transport.reply(0, binanceResponse()); QVERIFY(catalog.instruments("binance").isEmpty());
    catalog.ensure("binance"); QCOMPARE(transport.requests.size(), 1);
    catalog.ensure("bithumb"); QTRY_COMPARE(transport.requests.size(), 2);
    HttpResult limited; limited.status = 429; limited.headers.insert("Retry-After", "600");
    transport.reply(1, limited); QVERIFY(!catalog.busy());
    QVERIFY(catalog.instruments("bithumb").isEmpty());
    QVERIFY(!catalog.status("bithumb").isEmpty());
    catalog.ensure("bithumb"); QCOMPARE(transport.requests.size(), 2);
}

QTEST_GUILESS_MAIN(ChartInstrumentCatalogTest)
#include "tst_chart_instrument_catalog.moc"
