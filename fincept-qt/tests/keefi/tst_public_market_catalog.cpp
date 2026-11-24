#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicKrwPrices.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUrlQuery>
#include <optional>

using namespace keefi;
namespace {
HttpResult marketsResponse() {
    HttpResult result; result.status = 200;
    result.body = R"([{"market":"KRW-SYN","korean_name":"합성 종목","english_name":"Synthetic","market_event":{"warning":false,"caution":{}}}])";
    return result;
}
HttpResult marketsResponse(const QStringList& codes) {
    QJsonArray rows;
    for (const auto& code : codes) rows.append(QJsonObject{{"market", code}, {"korean_name", code}});
    HttpResult result; result.status = 200; result.body = QJsonDocument(rows).toJson(); return result;
}
PublicTick marketTick(const QString& base, const QString& price, qint64 time) {
    PublicTick tick;
    tick.market = "KRW-" + base;
    tick.signedChangeRate = "0";
    tick.quote = {base, "KRW", price, {"Upbit", "wss://api.upbit.com/websocket/v1",
        "public:upbit:" + tick.market, time, QDateTime::currentMSecsSinceEpoch(), true}};
    return tick;
}
HttpResult quoteResponse(const QString& price = "100", qint64 sourceTime = 0) {
    HttpResult result; result.status = 200;
    QJsonArray rows;
    rows.append(QJsonObject{{"market", "KRW-SYN"}, {"trade_price", price}, {"prev_closing_price", "90"},
        {"signed_change_rate", "0.1"}, {"timestamp", QString::number(sourceTime ? sourceTime : QDateTime::currentMSecsSinceEpoch() - 100)},
        {"acc_trade_price_24h", "12345"}});
    result.body = QJsonDocument(rows).toJson(QJsonDocument::Compact); return result;
}
class ManualTransport final : public TradingTransport {
  public:
    struct Sent { quint64 token; RequestSpec request; qint64 atMs; };
    QList<Sent> sent; int aborts = 0; QElapsedTimer clock; std::optional<HttpResult> responseOnAbort;
    ManualTransport() { clock.start(); }
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(NetworkTradingTransport::allowed(request)); QVERIFY(!request.authenticated); QVERIFY(request.headers.isEmpty());
        sent.append({token, request, clock.elapsed()});
    }
    void abort() override {
        ++aborts;
        if (responseOnAbort && !sent.isEmpty()) { const auto response = *responseOnAbort; responseOnAbort.reset(); emit finished(sent.last().token, response); }
    }
    void reply(qsizetype index, const HttpResult& response) { emit finished(sent[index].token, response); }
};
class FakeSocket final : public PublicTickerSocket {
  public:
    QList<QUrl> opened; QList<QByteArray> sent; int aborts = 0;
    void open(const QUrl& url) override { opened.append(url); }
    void send(const QByteArray& bytes) override { sent.append(bytes); }
    void ping() override {}
    void abort() override { ++aborts; }
};
QByteArray rowState(const PublicMarketCatalog& catalog) {
    QJsonArray rows;
    for (const auto& row : catalog.rows()) rows.append(QJsonObject{
        {"market", row.market}, {"base", row.base}, {"ko", row.koreanName}, {"en", row.englishName},
        {"warning", row.warning}, {"caution", row.caution}, {"priced", row.priced}, {"price", row.price},
        {"previous", row.previousClose}, {"rate", row.signedChangeRate}, {"source", QString::number(row.sourceTimeMs)},
        {"receipt", QString::number(row.receivedAtMs)}, {"url", row.quoteUrl}, {"volume", row.tradeValue24h}});
    return QJsonDocument(rows).toJson(QJsonDocument::Compact);
}
}

class PublicMarketCatalogTest final : public QObject {
    Q_OBJECT
  private slots:
    void incrementalPricesFollowCatalogueOrderAndCatchUpMissedChanges();
    void pricesLinkAfterFirstQuoteAndRetainLastNonemptyMarketList();
    void hiddenMetadataDemandRefreshesOnlyOfficialList();
    void metadataOnlyKeepsPricesAndSharesRateLimitCooldown();
    void activeMetadataRequestSurvivesHideWithoutStartingQuotes();
    void defaultActiveKeepsExistingUnsignedPublicReadContract();
    void pausedRefreshHasNoNetworkDemand();
    void pauseCancelsPendingPageAndIgnoresLateInitialCatalogue();
    void pauseAbortsInflightWithoutChangingCachedEvidenceEvenOnSynchronousReply();
    void hiddenStreamAndFallbackCannotRefreshOrRenewEvidence();
    void resumeUsesCachedCatalogueAndRetainsRequestSpacing();
    void rateLimitCooldownCannotBeBypassedByPresentationToggles();
    void pacingWaitIsReadOnceSoALateTickCannotStallQuotes();
};

void PublicMarketCatalogTest::incrementalPricesFollowCatalogueOrderAndCatchUpMissedChanges() {
    ManualTransport transport;
    qint64 elapsed = 0;
    PublicMarketCatalog catalog(&transport, nullptr, [&]{ return elapsed; });
    PublicQuoteStream stream(PublicQuoteStream::Provider::Upbit, std::make_unique<FakeSocket>());
    catalog.setStream(&stream);
    quint64 seen = 0;
    QVERIFY(catalog.quotesSince(seen).isEmpty());
    catalog.refresh();
    transport.reply(0, marketsResponse({"KRW-B", "KRW-A", "BTC-A", "KRW-C"}));
    const QSet<QString> listed{"KRW-B", "KRW-A", "KRW-C"};
    QCOMPARE(catalog.marketCodes(), listed);
    QVERIFY(catalog.quotesSince(seen).isEmpty());
    const auto oldRevision = seen;
    const auto at = QDateTime::currentMSecsSinceEpoch() - 1000;
    emit stream.ticks({marketTick("A","1",at),marketTick("C","2",at),marketTick("B","3",at)});
    QCOMPARE(catalog.quotesSince(seen), catalog.quotes());
    emit stream.ticks({marketTick("C","4",at+1),marketTick("B","5",at+2),marketTick("C","6",at+3),
                       marketTick("B","7",at+1),marketTick("UNKNOWN","8",at+4)});
    const QList<AssetQuote> expected{catalog.quoteForMarket("KRW-B"), catalog.quoteForMarket("KRW-C")};
    QCOMPARE(catalog.quotesSince(seen), expected);
    QCOMPARE(expected.first().price, QString("5")); QCOMPARE(expected.last().price, QString("6"));
    quint64 missed = oldRevision, initial = 0;
    QCOMPARE(catalog.quotesSince(missed), catalog.quotes());
    QCOMPARE(catalog.quotesSince(initial), catalog.quotes());
    elapsed = 350; QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    transport.reply(1, quoteResponse()); // Outside the requested catalogue. No partially accepted page.
    QVERIFY(!catalog.busy()); QVERIFY(catalog.quotesSince(seen).isEmpty()); QCOMPARE(seen, missed);
    catalog.setPresentationActive(false); catalog.setMetadataDemand(true);
    elapsed = 300001; catalog.refresh();
    QCOMPARE(transport.sent.size(), 3);
    transport.reply(2, marketsResponse({"KRW-C","KRW-B"}));
    QCOMPARE(catalog.quotesSince(seen), (QList<AssetQuote>{expected.last(), expected.first()}));
    QCOMPARE(catalog.marketCodes(), (QSet<QString>{"KRW-C","KRW-B"}));
}

void PublicMarketCatalogTest::pricesLinkAfterFirstQuoteAndRetainLastNonemptyMarketList() {
    ManualTransport transport, pricingTransport;
    qint64 elapsed = 0;
    PublicMarketCatalog catalog(&transport, nullptr, [&]{ return elapsed; });
    PublicQuoteStream stream(PublicQuoteStream::Provider::Upbit, std::make_unique<FakeSocket>());
    catalog.setStream(&stream); catalog.refresh(); transport.reply(0, marketsResponse());
    const auto at = QDateTime::currentMSecsSinceEpoch() - 1000;
    emit stream.ticks({marketTick("SYN","100",at)});
    PublicKrwPrices prices(&pricingTransport);
    prices.setPublicMarketCatalog(&catalog);
    QList<AssetQuote> received;
    connect(&prices, &PublicKrwPrices::completed, this, [&](const auto& batch){ received.append(batch); });
    AssetComponent component; component.asset = "SYN"; component.quantity = "2";
    AssetSnapshot holding; holding.components = {component};
    prices.refresh({holding}); QTRY_VERIFY(!prices.busy());
    QVERIFY(!received.isEmpty()); QCOMPARE(received.last(), catalog.quoteForMarket("KRW-SYN"));
    received.clear();
    emit stream.changed(); QVERIFY(received.isEmpty());
    emit stream.ticks({marketTick("SYN","101",at+1)});
    QCOMPARE(received, catalog.quotes());
    received.clear();
    const auto future = QDateTime::currentMSecsSinceEpoch() + 150;
    emit stream.ticks({marketTick("SYN","102",future)});
    QVERIFY(received.isEmpty());
    catalog.setPresentationActive(false); prices.clear(); // Cancels deferred delivery.
    QTRY_VERIFY(QDateTime::currentMSecsSinceEpoch() >= future);
    emit stream.changed(); // Recheck the catalogue's cached price once it can be accepted.
    elapsed = 300001; catalog.setMetadataDemand(true);
    QCOMPARE(transport.sent.size(), 2); transport.reply(1, marketsResponse({}));
    QVERIFY(catalog.marketCodes().isEmpty());
    received.clear(); prices.refresh({holding}); QTRY_VERIFY(!prices.busy());
    QVERIFY(!received.isEmpty()); QCOMPARE(received.last().price, QString("102"));
    QVERIFY(pricingTransport.sent.isEmpty());
}

void PublicMarketCatalogTest::hiddenMetadataDemandRefreshesOnlyOfficialList() {
    ManualTransport transport;qint64 elapsed=0;PublicMarketCatalog catalog(&transport,nullptr,[&]{return elapsed;});
    auto socket=std::make_unique<FakeSocket>();auto* fake=socket.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Upbit,std::move(socket));
    catalog.setPresentationActive(false);catalog.setStream(&stream);catalog.setMetadataDemand(true);
    QCOMPARE(transport.sent.size(),1);QCOMPARE(transport.sent.last().request.operation,Operation::Markets);
    transport.reply(0,marketsResponse());QVERIFY(!catalog.busy());QVERIFY(catalog.marketListAvailable());
    QCOMPARE(catalog.completedAtMs(),0);QVERIFY(catalog.quotes().isEmpty());QVERIFY(fake->opened.isEmpty());
    catalog.refresh();catalog.setMetadataDemand(true);QCOMPARE(transport.sent.size(),1);
    elapsed=294999;catalog.refresh();QCOMPARE(transport.sent.size(),1);
    elapsed=295000;QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(),2,5500);
    QCOMPARE(transport.sent.last().request.operation,Operation::Markets);QVERIFY(fake->opened.isEmpty());
    QVERIFY(catalog.marketListAvailable()); // Refresh begins before expiry; normal alert lanes need not reset.
    transport.reply(1,marketsResponse());elapsed+=300001;catalog.refresh();QCOMPARE(transport.sent.size(),3);
    catalog.setMetadataDemand(false);QVERIFY(!catalog.busy());QVERIFY(transport.aborts>0);
    transport.reply(2,marketsResponse());QVERIFY(!catalog.marketListAvailable());
    elapsed+=300001;catalog.refresh();QTest::qWait(400);QCOMPARE(transport.sent.size(),3);
}
void PublicMarketCatalogTest::metadataOnlyKeepsPricesAndSharesRateLimitCooldown() {
    ManualTransport transport;qint64 elapsed=0;PublicMarketCatalog catalog(&transport,nullptr,[&]{return elapsed;});
    catalog.refresh();transport.reply(0,marketsResponse());elapsed=350;QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(),2,1000);
    transport.reply(1,quoteResponse());elapsed=700;QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(),1000);
    const auto rows=rowState(catalog);const auto quotes=catalog.quotes();const auto completed=catalog.completedAtMs();
    catalog.setMetadataDemand(true);catalog.setPresentationActive(false);elapsed=300001;catalog.refresh();QCOMPARE(transport.sent.size(),3);
    HttpResult limited;limited.status=429;limited.localRetryAfterSeconds=3;limited.headers.insert("Retry-After","3");transport.reply(2,limited);
    catalog.setMetadataDemand(false);catalog.setMetadataDemand(true);elapsed=303000;catalog.refresh();QCOMPARE(transport.sent.size(),3);
    elapsed=303001;catalog.refresh();QCOMPARE(transport.sent.size(),4);transport.reply(3,marketsResponse());
    QCOMPARE(rowState(catalog),rows);QCOMPARE(catalog.quotes(),quotes);QCOMPARE(catalog.completedAtMs(),completed);QVERIFY(!catalog.busy());
    for(int i=2;i<transport.sent.size();++i)QCOMPARE(transport.sent[i].request.operation,Operation::Markets);
}
void PublicMarketCatalogTest::pacingWaitIsReadOnceSoALateTickCannotStallQuotes() {
    // A busy machine can move the clock between "is the next send due?" and the wait
    // handed to the pacing timer. A wait taken from a second read can be negative, which
    // Qt refuses ("Timers cannot have negative intervals"): the catalogue then stayed busy
    // and never asked for quotes again. Script exactly that: the due check reads 349 ms,
    // every later read 360 ms (the next send is due at 350 ms).
    ManualTransport transport;qint64 elapsed=0;QList<qint64> script;
    PublicMarketCatalog catalog(&transport,nullptr,[&]{return script.isEmpty()?elapsed:script.takeFirst();});
    catalog.refresh();QCOMPARE(transport.sent.size(),1);QCOMPARE(transport.sent.last().request.operation,Operation::Markets);
    transport.reply(0,marketsResponse());QVERIFY(catalog.busy()); // Quotes wait for the 350 ms spacing.
    script={349};elapsed=360;
    QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(),2,2000);QCOMPARE(transport.sent.last().request.operation,Operation::Quote);
    QVERIFY(script.isEmpty());
}
void PublicMarketCatalogTest::activeMetadataRequestSurvivesHideWithoutStartingQuotes() {
    ManualTransport transport;PublicMarketCatalog catalog(&transport);catalog.setMetadataDemand(true);catalog.refresh();
    QCOMPARE(transport.sent.size(),1);catalog.setPresentationActive(false);QCOMPARE(transport.aborts,0);
    transport.reply(0,marketsResponse());QVERIFY(catalog.marketListAvailable());QVERIFY(!catalog.busy());
    QTest::qWait(400);QCOMPARE(transport.sent.size(),1);QVERIFY(catalog.quotes().isEmpty());QCOMPARE(catalog.completedAtMs(),0);
}

void PublicMarketCatalogTest::defaultActiveKeepsExistingUnsignedPublicReadContract() {
    ManualTransport transport; PublicMarketCatalog catalog(&transport);
    QVERIFY(catalog.presentationActive()); catalog.refresh(); QCOMPARE(transport.sent.size(), 1);
    QCOMPARE(transport.sent.first().request.operation, Operation::Markets);
    QCOMPARE(QUrlQuery(transport.sent.first().request.url).queryItemValue("is_details"), QString("true"));
    transport.reply(0, marketsResponse()); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    QCOMPARE(transport.sent[1].request.operation, Operation::Quote);
    transport.reply(1, quoteResponse()); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
    QCOMPARE(catalog.rows().size(), 1); QCOMPARE(catalog.quotes().size(), 1);
    QCOMPARE(catalog.rows().first().price, QString("100")); QVERIFY(catalog.completedAtMs() > 0);
}
void PublicMarketCatalogTest::pausedRefreshHasNoNetworkDemand() {
    ManualTransport transport; PublicMarketCatalog catalog(&transport);
    catalog.setPresentationActive(false); QVERIFY(!catalog.presentationActive());
    catalog.refresh(); catalog.refresh(); QTest::qWait(450);
    QCOMPARE(transport.sent.size(), 0); QVERIFY(!catalog.busy()); QVERIFY(catalog.rows().isEmpty());
    QCOMPARE(catalog.completedAtMs(), 0);
}
void PublicMarketCatalogTest::pauseCancelsPendingPageAndIgnoresLateInitialCatalogue() {
    ManualTransport transport; PublicMarketCatalog catalog(&transport); catalog.refresh();
    QCOMPARE(transport.sent.size(), 1); catalog.setPresentationActive(false);
    QVERIFY(!catalog.busy()); QVERIFY(transport.aborts >= 1);
    transport.reply(0, marketsResponse()); QTest::qWait(450);
    QVERIFY(catalog.rows().isEmpty()); QVERIFY(catalog.quotes().isEmpty()); QCOMPARE(transport.sent.size(), 1);
    catalog.setPresentationActive(true); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    QCOMPARE(transport.sent.last().request.operation, Operation::Markets);
    transport.reply(1, marketsResponse()); QVERIFY(catalog.busy());
    const auto rows = rowState(catalog); catalog.setPresentationActive(false); QTest::qWait(450);
    QCOMPARE(transport.sent.size(), 2); QCOMPARE(rowState(catalog), rows); QCOMPARE(catalog.completedAtMs(), 0);
}
void PublicMarketCatalogTest::pauseAbortsInflightWithoutChangingCachedEvidenceEvenOnSynchronousReply() {
    ManualTransport transport; PublicMarketCatalog catalog(&transport); catalog.refresh();
    transport.reply(0, marketsResponse()); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    transport.reply(1, quoteResponse()); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
    const auto cachedRows = rowState(catalog); const auto cachedQuotes = catalog.quotes(); const auto completed = catalog.completedAtMs();
    catalog.refresh(); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 3, 1000);
    transport.responseOnAbort = quoteResponse("999"); catalog.setPresentationActive(false);
    QVERIFY(transport.aborts >= 1); QVERIFY(!catalog.busy()); QCOMPARE(rowState(catalog), cachedRows);
    QCOMPARE(catalog.quotes(), cachedQuotes); QCOMPARE(catalog.completedAtMs(), completed);
    transport.reply(2, quoteResponse("999")); catalog.refresh(); QTest::qWait(450);
    QCOMPARE(transport.sent.size(), 3); QCOMPARE(rowState(catalog), cachedRows); QCOMPARE(catalog.quotes(), cachedQuotes);
    QCOMPARE(catalog.completedAtMs(), completed);
}
void PublicMarketCatalogTest::hiddenStreamAndFallbackCannotRefreshOrRenewEvidence() {
    ManualTransport transport; auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
    PublicQuoteStream stream(PublicQuoteStream::Provider::Upbit, std::move(socket)); PublicMarketCatalog catalog(&transport);
    catalog.refresh(); transport.reply(0, marketsResponse()); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    transport.reply(1, quoteResponse()); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
    catalog.setStream(&stream); QTRY_COMPARE_WITH_TIMEOUT(fake->opened.size(), 1, 1000); emit fake->connected();
    QVERIFY(!fake->sent.isEmpty()); QVERIFY(fake->sent.last().contains("KRW-SYN"));
    const auto rows = rowState(catalog); const auto quotes = catalog.quotes(); const auto completed = catalog.completedAtMs();
    const int aborts = fake->aborts; catalog.setPresentationActive(false); QVERIFY(fake->aborts > aborts);
    PublicTick late; late.market = "KRW-SYN"; late.previousClose = "90"; late.signedChangeRate = "0.1";
    late.quote = {"SYN", "KRW", "999", {"합성 공개 원천", "wss://api.upbit.com/websocket/v1", "public:upbit:KRW-SYN",
        QDateTime::currentMSecsSinceEpoch(), QDateTime::currentMSecsSinceEpoch(), true}};
    emit stream.ticks({late}); catalog.refresh(); QTest::qWait(5300);
    QCOMPARE(transport.sent.size(), 2); QCOMPARE(fake->opened.size(), 1); QCOMPARE(rowState(catalog), rows);
    QCOMPARE(catalog.quotes(), quotes); QCOMPARE(catalog.completedAtMs(), completed);
    catalog.setPresentationActive(true); QTRY_COMPARE_WITH_TIMEOUT(fake->opened.size(), 2, 1000);
    emit fake->connected(); QCOMPARE(fake->sent.size(), 2); QVERIFY(fake->sent.last().contains("KRW-SYN"));
    QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 3, 1000); QCOMPARE(transport.sent.last().request.operation, Operation::Quote);
    catalog.setPresentationActive(false); const auto requests = transport.sent.size();
    auto replacement = std::make_unique<FakeSocket>(); auto* replacementFake = replacement.get();
    PublicQuoteStream replacementStream(PublicQuoteStream::Provider::Upbit, std::move(replacement));
    catalog.setStream(&replacementStream); QTest::qWait(400);
    QCOMPARE(replacementFake->opened.size(), 0); QCOMPARE(transport.sent.size(), requests);
}
void PublicMarketCatalogTest::resumeUsesCachedCatalogueAndRetainsRequestSpacing() {
    ManualTransport transport; PublicMarketCatalog catalog(&transport); catalog.refresh();
    transport.reply(0, marketsResponse()); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    transport.reply(1, quoteResponse()); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
    const auto rows = rowState(catalog); const auto quotes = catalog.quotes(); const auto completed = catalog.completedAtMs();
    catalog.setPresentationActive(false); catalog.setPresentationActive(false); catalog.setPresentationActive(true); catalog.setPresentationActive(true);
    QCOMPARE(rowState(catalog), rows); QCOMPARE(catalog.quotes(), quotes); QCOMPARE(catalog.completedAtMs(), completed);
    QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 3, 1000);
    QCOMPARE(transport.sent[2].request.operation, Operation::Quote);
    QVERIFY2(transport.sent[2].atMs - transport.sent[1].atMs >= 340, "Visibility toggles preserve the existing 350ms pacing clock");
    transport.reply(2, quoteResponse("101")); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
    QCOMPARE(catalog.rows().first().price, QString("101")); QCOMPARE(transport.sent.size(), 3);
}
void PublicMarketCatalogTest::rateLimitCooldownCannotBeBypassedByPresentationToggles() {
    ManualTransport transport; PublicMarketCatalog catalog(&transport); catalog.refresh();
    transport.reply(0, marketsResponse()); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 2, 1000);
    transport.reply(1, quoteResponse()); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 1000);
    catalog.refresh(); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 3, 1000);
    HttpResult limited; limited.status = 429; limited.localRetryAfterSeconds = 3; limited.headers.insert("Retry-After", "3");
    transport.reply(2, limited); QVERIFY(!catalog.busy()); QVERIFY(catalog.status().contains(QStringLiteral("요청 제한")));
    const auto rows = rowState(catalog); const auto quotes = catalog.quotes(); const auto completed = catalog.completedAtMs();
    for (int i = 0; i < 3; ++i) { catalog.setPresentationActive(false); catalog.setPresentationActive(true); catalog.refresh(); }
    QTest::qWait(500); QCOMPARE(transport.sent.size(), 3); QCOMPARE(rowState(catalog), rows);
    QCOMPARE(catalog.quotes(), quotes); QCOMPARE(catalog.completedAtMs(), completed);
    catalog.setPresentationActive(false); QTest::qWait(2700); QCOMPARE(transport.sent.size(), 3);
    catalog.setPresentationActive(true); QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(), 4, 1000);
    QCOMPARE(transport.sent.last().request.operation, Operation::Quote);
}

QTEST_GUILESS_MAIN(PublicMarketCatalogTest)
#include "tst_public_market_catalog.moc"
