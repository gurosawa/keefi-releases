#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include <QTest>
#include <QDateTime>
#include <QJsonDocument>
using namespace keefi;
namespace {
constexpr qint64 Now = 1792602000000;
AssetQuote quote(QString base, QString unit, QString price, QString source, qint64 at = Now - 1000) {
    return {base, unit, price, {source, "https://example.test/public", "public", at, Now, true}};
}
class PublicTransport final : public TradingTransport {
  public:
    QList<RequestSpec> sent; quint64 token = 0; int aborts = 0;
    void send(quint64 next, const RequestSpec& request) override { token = next; sent.append(request); }
    void abort() override { ++aborts; }
    void answer(QByteArray body) { HttpResult r; r.status = 200; r.body = body; emit finished(token, r); }
};
}
class MarketMetricsTest : public QObject {
    Q_OBJECT
  private slots:
    void contractFlowUsesMatchedObservedIntervals(){
        const ContractObservation a{"BTCUSDT","100","20",Now-61000,Now-60000};
        auto b=ContractObservation{"BTCUSDT","110","22",Now-1000,Now};
        auto result=MarketMetrics::contractFlow(a,b,Now);QVERIFY(result.available);
        QCOMPARE(result.priceDirection,1);QCOMPARE(result.quantityDirection,1);QCOMPARE(result.fromMs,a.quantityAtMs);
        b.quantity="19";result=MarketMetrics::contractFlow(a,b,Now);QVERIFY(result.available);QCOMPARE(result.quantityDirection,-1);
        b.priceAtMs=Now-31000;QVERIFY(!MarketMetrics::contractFlow(a,b,Now).available);
        b.priceAtMs=Now+1;QVERIFY(!MarketMetrics::contractFlow(a,b,Now).available);
        b.priceAtMs=Now-1000;b.symbol="ETHUSDT";QVERIFY(!MarketMetrics::contractFlow(a,b,Now).available);
        b.symbol="BTCUSDT";auto old=a;old.priceAtMs-=120001;old.quantityAtMs-=120001;
        QVERIFY(!MarketMetrics::contractFlow(old,b,Now).available);
    }
    void priceGapUsesActualUsdtBridge();
    void actualPublicMetrics();
    void onlyExactPublicReads();
    void parsesPublishedFundingAndOiWithoutInventedDefaults();
    void successfulContractRecheckKeepsPreviouslyReceivedMetrics();
    void fundingIntervalsAcceptOfficialUnicodeIdentitiesWithoutWeakeningValidation();
    void gapRejectsMissingStaleFutureAndMismatchedLegs();
    void selectedCoinReadsAreIsolatedAndStopWhenCleared();
};
void MarketMetricsTest::priceGapUsesActualUsdtBridge() {
    const auto result = MarketMetrics::exchangePriceGap(quote("BTC", "KRW", "142800000", "Upbit"),
        quote("BTC", "USDT", "100000", "Binance"), quote("USDT", "KRW", "1400", "Upbit"), Now);
    QVERIFY(result.available);
    QCOMPARE(result.percent, "2");
    QCOMPARE(result.legs.size(), 3);
    QCOMPARE(result.legs.last().price, "1400");
}
void MarketMetricsTest::selectedCoinReadsAreIsolatedAndStopWhenCleared() {
    PublicTransport transport; MarketMetrics metrics(&transport);
    metrics.setAsset("BTC");
    QTRY_COMPARE(transport.sent.size(), 1);
    QVERIFY(transport.sent.first().url.path().endsWith("exchangeInfo"));
    transport.answer(R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})");
    QTRY_COMPARE(transport.sent.size(), 2); transport.answer("[]");
    QTRY_COMPARE(transport.sent.size(), 3);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0001"},{"interestRate","0.0001"},
        {"time", double(now)},{"nextFundingTime",double(now + 3600000)}}).toJson());
    QTRY_COMPARE(transport.sent.size(), 4);
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"openInterest","123.45"},{"time",double(now)}}).toJson());
    QCOMPARE(metrics.derivatives().fundingIntervalHours, 8);
    QCOMPARE(metrics.derivatives().openInterest, "123.45");
    QVERIFY(metrics.derivatives(now + 180001).openInterest.isEmpty());
    metrics.setAsset("ETH"); QVERIFY(metrics.derivatives().openInterest.isEmpty());
    QTRY_COMPARE(transport.sent.size(), 5);
    const auto late = transport.token;
    metrics.setAsset({}); QVERIFY(metrics.derivatives().symbol.isEmpty());
    HttpResult result; result.status = 200; emit transport.finished(late, result);
    QVERIFY(metrics.derivatives().symbol.isEmpty()); QVERIFY(transport.aborts > 0);
}
void MarketMetricsTest::parsesPublishedFundingAndOiWithoutInventedDefaults() {
    PublicDerivativesMetrics m;
    const auto apply = [&](MarketMetrics::Read read, QByteArray body, int status = 200) {
        HttpResult r; r.status = status; r.body = body;
        return MarketMetrics::applyResponse(read, "BTCUSDT", r, Now, &m);
    };
    QVERIFY(apply(MarketMetrics::Read::Contracts, R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})"));
    QCOMPARE(m.baseAsset, "BTC");
    QVERIFY(apply(MarketMetrics::Read::FundingIntervals, R"([{"symbol":"BTCUSDT","fundingIntervalHours":4}])"));
    QCOMPARE(m.fundingIntervalHours, 4); QVERIFY(!m.fundingIntervalDefault);
    QVERIFY(apply(MarketMetrics::Read::Funding, R"({"symbol":"BTCUSDT","lastFundingRate":"-0.00012500","interestRate":"0.0001","nextFundingTime":1792605600000,"time":1792601999000})"));
    QCOMPARE(m.fundingRate, "-0.000125"); QCOMPARE(m.interestRate, "0.0001");
    QCOMPARE(m.fundingEvidence.sourceTimeMs, Now - 1000);
    QVERIFY(apply(MarketMetrics::Read::OpenInterest, R"({"symbol":"BTCUSDT","openInterest":"10659.509","time":1792601999000})"));
    QCOMPARE(m.openInterest, "10659.509"); QCOMPARE(m.openInterestUnit, "수량");
    QVERIFY(!apply(MarketMetrics::Read::FundingIntervals, "{}", 503));
    QCOMPARE(m.fundingIntervalHours, 0); QVERIFY(!m.intervalReason.isEmpty());
    QVERIFY(apply(MarketMetrics::Read::FundingIntervals, "[]"));
    QCOMPARE(m.fundingIntervalHours, 8); QVERIFY(m.fundingIntervalDefault);
    QVERIFY(!apply(MarketMetrics::Read::Funding, R"({"symbol":"ETHUSDT","lastFundingRate":"0.001","time":1792602000001})"));
    QVERIFY(m.fundingRate.isEmpty()); QCOMPARE(m.openInterest, "10659.509");
    QVERIFY(!apply(MarketMetrics::Read::OpenInterest, R"({"symbol":"BTCUSDT","openInterest":"1","time":1792602000001})"));
    QVERIFY(m.openInterest.isEmpty());
}
void MarketMetricsTest::successfulContractRecheckKeepsPreviouslyReceivedMetrics() {
    PublicDerivativesMetrics metrics;
    const QByteArray contract = R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})";
    const auto apply = [&](MarketMetrics::Read read, const QByteArray& body, qint64 received) {
        HttpResult response; response.status = 200; response.body = body;
        return MarketMetrics::applyResponse(read, "BTCUSDT", response, received, &metrics);
    };
    QVERIFY(apply(MarketMetrics::Read::Contracts, contract, Now));
    QVERIFY(apply(MarketMetrics::Read::FundingIntervals, "[]", Now));
    QVERIFY(apply(MarketMetrics::Read::Funding, R"({"symbol":"BTCUSDT","lastFundingRate":"0.0001","interestRate":"0.0001","markPrice":"100000","nextFundingTime":1792605600000,"time":1792601999000})", Now));
    QVERIFY(apply(MarketMetrics::Read::OpenInterest, R"({"symbol":"BTCUSDT","openInterest":"10659.509","time":1792601999000})", Now));

    // Revalidating the same supported contract is not a new funding or OI observation.
    QVERIFY(apply(MarketMetrics::Read::Contracts, contract, Now + 60000));
    QCOMPARE(metrics.fundingRate, QString("0.0001"));
    QCOMPARE(metrics.fundingIntervalHours, 8);
    QCOMPARE(metrics.openInterest, QString("10659.509"));
    QCOMPARE(metrics.fundingEvidence.receivedAtMs, Now);
    QCOMPARE(metrics.openInterestEvidence.receivedAtMs, Now);
    QCOMPARE(metrics.fundingEvidence.sourceTimeMs, Now - 1000);
    // A contract no longer present in the official response must still revoke it.
    QVERIFY(!apply(MarketMetrics::Read::Contracts, R"({"symbols":[]})", Now + 60001));
    QVERIFY(metrics.baseAsset.isEmpty());QVERIFY(metrics.fundingRate.isEmpty());QVERIFY(metrics.openInterest.isEmpty());
}
void MarketMetricsTest::gapRejectsMissingStaleFutureAndMismatchedLegs() {
    auto a = quote("BTC", "KRW", "1400", "Upbit");
    auto b = quote("BTC", "USDT", "1", "Binance");
    auto c = quote("USDT", "KRW", "1400", "Upbit");
    c.quoteAsset = "USD"; QVERIFY(!MarketMetrics::exchangePriceGap(a,b,c,Now).available); c.quoteAsset = "KRW";
    b.evidence.sourceTimeMs = Now + 1; QVERIFY(!MarketMetrics::exchangePriceGap(a,b,c,Now).available);
    b.evidence.sourceTimeMs = Now - 180001; QVERIFY(!MarketMetrics::exchangePriceGap(a,b,c,Now).available);
    b.evidence.sourceTimeMs = Now - 31001; QVERIFY(!MarketMetrics::exchangePriceGap(a,b,c,Now).available);
    b.evidence.sourceTimeMs = Now - 1000; b.price = "0"; QVERIFY(!MarketMetrics::exchangePriceGap(a,b,c,Now).available);
    b.price = "1"; a.price = "1386"; QCOMPARE(MarketMetrics::exchangePriceGap(a,b,c,Now).percent, "-1");
}
void MarketMetricsTest::fundingIntervalsAcceptOfficialUnicodeIdentitiesWithoutWeakeningValidation() {
    PublicDerivativesMetrics m;
    const auto apply = [&](MarketMetrics::Read read, const QByteArray& body) {
        HttpResult response; response.status = 200; response.body = body;
        return MarketMetrics::applyResponse(read, "BTCUSDT", response, Now, &m);
    };
    QVERIFY(apply(MarketMetrics::Read::Contracts, R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})"));
    // Real official adjustment lists include unrelated non-ASCII contract identities.
    const QByteArray unicodeRows = R"({"symbol":"币安人生USDT","fundingIntervalHours":1},{"symbol":"我踏马来了USDT","fundingIntervalHours":4},{"symbol":"龙虾USDT","fundingIntervalHours":1},{"symbol":"牛来USDT","fundingIntervalHours":4},{"symbol":"哈基米USDT","fundingIntervalHours":1})";
    QVERIFY(apply(MarketMetrics::Read::FundingIntervals, "[" + unicodeRows + "]"));
    QCOMPARE(m.fundingIntervalHours, 8); QVERIFY(m.fundingIntervalDefault);
    QVERIFY(apply(MarketMetrics::Read::FundingIntervals, "[" + unicodeRows + R"(,{"symbol":"BTCUSDT","fundingIntervalHours":4}])"));
    QCOMPARE(m.fundingIntervalHours, 4); QVERIFY(!m.fundingIntervalDefault);
    const QList<QByteArray> invalid = {
        R"([{"symbol":"币安人生USDT","fundingIntervalHours":1},{"symbol":"币安人生USDT","fundingIntervalHours":1}])",
        R"([{"symbol":"币安人生USDT","fundingIntervalHours":0}])",
        R"([{"symbol":"币安人生USDT","fundingIntervalHours":25}])",
        R"([{"symbol":"","fundingIntervalHours":1}])",
        R"([{"symbol":"BTC USDT","fundingIntervalHours":1}])",
        R"([{"symbol":"BTC\u0000USDT","fundingIntervalHours":1}])",
        R"([{"symbol":"BTC\ud800USDT","fundingIntervalHours":1}])",
        QByteArray("[{\"symbol\":\"BTC") + char(0xff) + "USDT\",\"fundingIntervalHours\":1}]"
    };
    for (const auto& body : invalid) {
        QVERIFY(!apply(MarketMetrics::Read::FundingIntervals, body));
        QCOMPARE(m.fundingIntervalHours, 0); QVERIFY(!m.intervalEvidence.complete);
    }
    QVERIFY(!MarketMetrics::request(MarketMetrics::Read::Funding, QString::fromUtf8("币安人生USDT")).url.isValid());
}
void MarketMetricsTest::onlyExactPublicReads() {
    for (const auto read : {MarketMetrics::Read::Contracts, MarketMetrics::Read::Funding,
                           MarketMetrics::Read::FundingIntervals, MarketMetrics::Read::OpenInterest}) {
        const auto r = MarketMetrics::request(read, "BTCUSDT");
        QVERIFY(MarketMetrics::isRequest(r)); QVERIFY(NetworkTradingTransport::allowed(r));
        auto bad = r; bad.method = "POST"; QVERIFY(!MarketMetrics::isRequest(bad));
        bad = r; bad.authenticated = true; QVERIFY(!MarketMetrics::isRequest(bad));
        bad = r; bad.headers.insert("X-MBX-APIKEY", "secret"); QVERIFY(!MarketMetrics::isRequest(bad));
        bad = r; bad.url.setQuery(r.url.query() + "&extra=1"); QVERIFY(!MarketMetrics::isRequest(bad));
        bad = r; bad.url.setHost("example.test"); QVERIFY(!MarketMetrics::isRequest(bad));
        bad = r; bad.body = "{}"; QVERIFY(!MarketMetrics::isRequest(bad));
    }
    QVERIFY(!MarketMetrics::request(MarketMetrics::Read::Funding, "BTCUSD").url.isValid());
    QVERIFY(!MarketMetrics::request(MarketMetrics::Read::Funding, "../order").url.isValid());
}
void MarketMetricsTest::actualPublicMetrics() {
    if(!qEnvironmentVariableIsSet("KEEFI_G8_LIVE_PUBLIC"))QSKIP("opt-in official unsigned public GET only");
    NetworkTradingTransport transport;MarketMetrics model(&transport);QList<int> statuses;
    connect(&transport,&TradingTransport::finished,this,[&](quint64,const HttpResult& r){statuses.append(r.status);});
    model.setAsset("BTC");QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty(),15000);
    if(!model.derivatives().baseAsset.isEmpty())QTRY_COMPARE_WITH_TIMEOUT(statuses.size(),4,45000);
    const auto data=model.derivatives();
    for(const auto status:statuses)qInfo("Official Binance public HTTP status: %d",status);
    qInfo("Validated contract: %d; funding: %d; interval: %d; OI: %d",!data.baseAsset.isEmpty(),!data.fundingRate.isEmpty(),data.fundingIntervalHours>0,!data.openInterest.isEmpty());
    if(data.baseAsset.isEmpty()){QVERIFY(data.fundingRate.isEmpty());QVERIFY(data.openInterest.isEmpty());}
    model.setAsset({});QVERIFY(model.derivatives().openInterest.isEmpty());
}
QTEST_GUILESS_MAIN(MarketMetricsTest)
#include "tst_market_metrics.moc"
