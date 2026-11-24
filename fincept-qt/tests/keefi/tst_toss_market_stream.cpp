#include "keefi/TossAdapter.h"
#include "keefi/TossMarketStream.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/TossResponses.h"
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QTest>

using namespace keefi;
using namespace keefi::test;
namespace {
class FakeTransport final : public TradingTransport {
public:
    QList<RequestSpec> requests;
    QList<quint64> tokens;
    quint64 epoch = 0;
    bool respond = true;
    int catalogRows = 0;
    void abort() override { ++epoch; }
    void send(quint64 token, const RequestSpec& request) override {
        requests.append(request); tokens.append(token); if (!respond) return;
        OrderInput input; input.cursor = QUrlQuery(request.url).queryItemValue("market");
        input.symbol = QUrlQuery(request.url).queryItemValue("symbols");
        auto response = tossResponse(request.operation, input); auto generation = epoch;
        if (request.operation == Operation::Markets && catalogRows && input.cursor == "KOSPI") {
            QJsonArray rows;
            for (int i = 0; i < catalogRows; ++i) rows.append(QJsonObject{{"symbol", QString::number(100000 + i)},
                {"name", "합성 종목"}, {"securityType", "STOCK"}, {"isCommonShare", true}, {"isinCode", "KR0000000000"}});
            response = tossEnvelope(rows);
        }
        QTimer::singleShot(0, this, [=, this] { if (generation == epoch) emit finished(token, response); });
    }
    int authentications() const {
        return std::count_if(requests.begin(), requests.end(), [](const auto& r) { return r.operation == Operation::Authenticate; });
    }
};
class FakeSocket final : public TossMarketSocket {
public:
    int opens = 0, aborts = 0;
    bool borrowedExpectedToken = false;
    QList<QByteArray> sent;
    void open(QByteArrayView token) override { ++opens; borrowedExpectedToken = token == QByteArrayView("fakeheader.fakepayload.fakesignature"); }
    void send(const QByteArray& bytes) override { sent.append(bytes); }
    void abort() override { ++aborts; }
};
bool addToss(Vault& vault) {
    AccountRecord a; a.provider = "toss"; a.alias = "가짜 토스"; a.market = "domestic_equity"; a.environment = "live"; a.currency = "KRW";
    return vault.addAccount(a, {{"client_id", "FAKE_CLIENT_ID"}, {"client_secret", "FAKE_CLIENT_SECRET"}}, StorageMode::EncryptedVault);
}
}
class TossMarketStreamTest final : public QObject {
    Q_OBJECT
private slots:
    void nativeHandshakeClassifiesAuthenticationFailures_data() {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("challenge");
        QTest::addColumn<int>("failureStatus");
        QTest::newRow("401-missing-challenge") << 401 << QByteArray{} << 401;
        QTest::newRow("401-bearer-challenge") << 401 << QByteArray("WWW-Authenticate: Bearer realm=\"synthetic\"\r\n") << 401;
        QTest::newRow("403-forbidden") << 403 << QByteArray{} << 403;
        QTest::newRow("503-retryable") << 503 << QByteArray{} << 0;
    }
    void nativeHandshakeClassifiesAuthenticationFailures() {
        QFETCH(int, httpStatus); QFETCH(QByteArray, challenge); QFETCH(int, failureStatus);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        bool receivedSyntheticHeader = false;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* connection = server.nextPendingConnection();
            connect(connection, &QTcpSocket::readyRead, connection,
                [&, connection, buffer = QByteArray{}]() mutable {
                    buffer += connection->readAll(); if (!buffer.contains("\r\n\r\n")) return;
                    for (const auto& line : buffer.split('\n')) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0 && line.left(colon).compare("Authorization", Qt::CaseInsensitive) == 0)
                            receivedSyntheticHeader = line.mid(colon + 1).trimmed() == "Bearer SYNTHETIC_NATIVE_HANDSHAKE_TOKEN";
                    }
                    const QByteArray response = "HTTP/1.1 " + QByteArray::number(httpStatus) + " Synthetic\r\n" + challenge +
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
                    connection->write(response); connection->disconnectFromHost();
                });
        });
        auto socket = TossMarketSocket::forLoopbackTest(server.serverPort()); QVERIFY(socket);
        QSignalSpy failures(socket.get(), &TossMarketSocket::failed);
        QSignalSpy connections(socket.get(), &TossMarketSocket::connected);
        socket->open(QByteArrayView("SYNTHETIC_NATIVE_HANDSHAKE_TOKEN"));
        QTRY_VERIFY_WITH_TIMEOUT(!failures.isEmpty(), 3000);
        QVERIFY(receivedSyntheticHeader); QCOMPARE(connections.size(), 0);
        QCOMPARE(failures.first().first().toInt(), failureStatus);
        socket->abort();
    }
    void borrowsExistingSessionOnly() {
        TossAdapter adapter;
        int calls = 0; quint64 revision = 0;
        const auto borrow = [&](QByteArrayView token, qint64 remaining, quint64 generation) {
            ++calls; QVERIFY(!token.isEmpty()); QVERIFY(remaining > 0); QVERIFY(generation > 0); revision = generation;
        };
        QVERIFY(!adapter.withMarketAuthorization(borrow));
        QVERIFY(adapter.parse(Operation::Authenticate, tossResponse(Operation::Authenticate), {}).ok);
        QVERIFY(adapter.withMarketAuthorization(borrow));
        QCOMPARE(calls, 1);
        QVERIFY(adapter.withMarketAuthorization([&](QByteArrayView, qint64, quint64 g) { QCOMPARE(g, revision); }));
        adapter.clearSession();
        QVERIFY(!adapter.withMarketAuthorization(borrow));
        QCOMPARE(calls, 1);
    }
    void streamSharesSessionAndStopsAtVaultLock() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id));
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        TossMarketStream stream(&broker, &marketHttp, std::move(socket));
        stream.setTargets({{"005930", "KR", 0}});
        QCOMPARE(fake->opens, 0);
        broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        QVERIFY(broker.authenticated());
        broker.loadMarkets();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        QCOMPARE(broker.marketDataCountry("005930"), QString("KR"));
        QTRY_COMPARE(fake->opens, 1);
        QVERIFY(fake->borrowedExpectedToken);
        QCOMPARE(brokerHttp.authentications(), 1);
        emit fake->connected();
        QTRY_VERIFY(!fake->sent.isEmpty());
        const auto declaration = QJsonDocument::fromJson(fake->sent.first()).array();
        QCOMPARE(declaration.at(1).toObject().value("type").toString(), QString("trade:kr"));
        QCOMPARE(declaration.at(1).toObject().value("codes").toArray(), QJsonArray{"005930"});
        const int beforeLock = fake->aborts;
        vault.lock();
        QVERIFY(fake->aborts > beforeLock);
        QVERIFY(stream.quotes().isEmpty());
        QVERIFY(!broker.withMarketAuthorization([](QByteArrayView, qint64, quint64) {}));
        QCOMPARE(brokerHttp.authentications(), 1);
    }
    void acknowledgesBeforeQuotesAndReconcilesAfterReconnect() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        TossMarketStream stream(&broker, &marketHttp, std::move(socket));
        stream.setTargets({{"005930", "KR", 0}}); emit fake->connected();
        const auto at = QDateTime::currentDateTimeUtc();
        const auto trade = QJsonDocument(QJsonObject{{"type", "message"}, {"topic", "trade:kr:005930"},
            {"data", QJsonObject{{"price", "70100"}, {"volume", "1"}, {"currency", "KRW"}, {"timestamp", at.toString(Qt::ISODateWithMs)}}}}).toJson();
        emit fake->received(trade); QVERIFY(stream.quotes().isEmpty());
        auto acknowledge = [&] {
            const auto id = QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
            emit fake->received(QJsonDocument(QJsonObject{{"type", "subscriptions"}, {"id", id},
                {"subscribed", QJsonArray{"trade:kr:005930"}}, {"rejected", QJsonArray{}}}).toJson());
        };
        acknowledge();
        QTRY_COMPARE_WITH_TIMEOUT(marketHttp.requests.size(), 1, 1000);
        const auto request = marketHttp.requests.first();
        QCOMPARE(request.method, QByteArray("GET")); QCOMPARE(request.url.path(), QString("/api/v1/prices"));
        QCOMPARE(QUrlQuery(request.url).queryItemValue("symbols"), QString("005930"));
        QCOMPARE(request.headers.value("Authorization"), QByteArray("Bearer fakeheader.fakepayload.fakesignature"));
        QVERIFY(!request.headers.contains("X-Account-Seq")); QVERIFY(request.body.isEmpty());
        emit fake->received(trade);
        QCOMPARE(stream.quotes().size(), 1); QCOMPARE(stream.quotes().first().price, QString("70100"));
        const auto older = tossEnvelope(QJsonArray{QJsonObject{{"symbol", "005930"}, {"lastPrice", "70000"},
            {"currency", "KRW"}, {"timestamp", at.addSecs(-1).toString(Qt::ISODateWithMs)}}});
        emit marketHttp.finished(marketHttp.tokens.last(), older);
        QCOMPARE(stream.quotes().first().price, QString("70100"));
        emit fake->failed(); QVERIFY(stream.quotes().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fake->opens, 2, 3000); emit fake->connected(); acknowledge();
        QTRY_COMPARE_WITH_TIMEOUT(marketHttp.requests.size(), 2, 1000);
        QCOMPARE(brokerHttp.authentications(), 1);
    }
    void subscriptionCapPrioritizesVisibleSelectedHoldingsThenRest() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; brokerHttp.catalogRows = 102; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        TossMarketStream stream(&broker, &marketHttp, std::move(socket));
        QList<TossMarketTarget> targets;
        for (int i = 0; i < 102; ++i) targets.append({QString::number(100000 + i), "KR", 3});
        targets.append({"100101", "KR", 1}); targets.append({"100100", "KR", 0});
        targets.append({"100099", "KR", 2}); targets.append({"100100", "KR", -1});
        targets.append({"100100", "KR", 0}); targets.append({"100099", "US", 0});
        targets.append({"AAPL", "US", 0}); targets.append({"005930", "", 0});
        stream.setTargets(targets);
        QCOMPARE(stream.targets().size(), 100);
        QCOMPARE(stream.targets().first().symbol, QString("100100"));
        QCOMPARE(stream.targets().at(1).symbol, QString("100101"));
        QCOMPARE(stream.targets().at(2).symbol, QString("100099"));
        QCOMPARE(stream.targets().last().symbol, QString("100096"));
        QCOMPARE(stream.restOnlyTargets().size(),2);QCOMPARE(stream.restOnlyTargets().first().symbol,QString("100097"));
        emit fake->connected(); const auto declaration = QJsonDocument::fromJson(fake->sent.last()).array();
        QCOMPARE(declaration.size(), 2); QCOMPARE(declaration.at(1).toObject().value("codes").toArray().size(), 100);
        QCOMPARE(brokerHttp.authentications(), 1);
    }
    void sendsTextPingAndExpiresWithoutNewAuthentication() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); qint64 now = 0;
        TossMarketStream stream(&broker, &marketHttp, std::move(socket), [&] { return now; });
        stream.setTargets({{"005930", "KR", 0}}); emit fake->connected();
        now = 61000;
        QTRY_VERIFY_WITH_TIMEOUT(fake->sent.contains("PING"), 1000);
        const int beforeExpiry = fake->aborts;
        now = 90000000;
        QTRY_VERIFY_WITH_TIMEOUT(fake->aborts > beforeExpiry, 1000);
        QVERIFY(stream.quotes().isEmpty()); QVERIFY(stream.status().contains("만료"));
        QTest::qWait(250); QCOMPARE(fake->opens, 1); QCOMPARE(brokerHttp.authentications(), 1);
    }
    void futureQuoteIsNotVisibleBeforeItsSourceTime() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000); broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        TossMarketStream stream(&broker, &marketHttp, std::move(socket));
        stream.setTargets({{"005930", "KR", 0}}); emit fake->connected();
        auto id = QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
        emit fake->received(QJsonDocument(QJsonObject{{"type", "subscriptions"}, {"id", id}, {"subscribed", QJsonArray{"trade:kr:005930"}}, {"rejected", QJsonArray{}}}).toJson());
        const auto future = QDateTime::currentDateTimeUtc().addMSecs(1100);
        emit fake->received(QJsonDocument(QJsonObject{{"type", "message"}, {"topic", "trade:kr:005930"},
            {"data", QJsonObject{{"price", "70100"}, {"volume", "1"}, {"currency", "KRW"}, {"timestamp", future.toString(Qt::ISODateWithMs)}}}}).toJson());
        QVERIFY(stream.quotes().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(stream.quotes().size(), 1, 2000);
    }
    void replacementDeclarationsArePacedAndRejectedTopicsStayExcluded() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000); broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); qint64 now = 0;
        TossMarketStream stream(&broker, &marketHttp, std::move(socket), [&] { return now; });
        stream.setTargets({{"005930", "KR", 0}}); emit fake->connected();
        stream.setTargets({{"005930", "KR", 0}, {"035720", "KR", 1}});
        stream.setTargets({{"035720", "KR", 0}, {"005930", "KR", 1}});
        QCOMPARE(fake->opens, 1); QCOMPARE(fake->sent.size(), 1);
        now = 1100; QTRY_COMPARE_WITH_TIMEOUT(fake->sent.size(), 2, 1000);
        auto id = QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
        emit fake->received(QJsonDocument(QJsonObject{{"type", "subscriptions"}, {"id", id},
            {"subscribed", QJsonArray{"trade:kr:005930"}}, {"rejected", QJsonArray{QJsonObject{{"target", "trade:kr:035720"}, {"code", "stock-not-found"}, {"message", "untrusted text"}}}}}).toJson());
        QVERIFY(stream.rejectedTopics().contains("trade:kr:035720"));
        now = 2200; QTRY_COMPARE_WITH_TIMEOUT(fake->sent.size(), 3, 1000);
        QVERIFY(!fake->sent.last().contains("035720"));
        id = QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
        emit fake->received(QJsonDocument(QJsonObject{{"type", "error"}, {"id", id}, {"error", QJsonObject{{"code", "rate-limit-exceeded"}}}}).toJson());
        QCOMPARE(fake->opens, 1); now = 3300; QTRY_COMPARE_WITH_TIMEOUT(fake->sent.size(), 4, 1000); QCOMPARE(fake->opens, 1);
        emit fake->failed(401); const auto before = fake->opens; QTest::qWait(1400); QCOMPARE(fake->opens, before);
        QVERIFY(stream.quotes().isEmpty()); QVERIFY(stream.status().contains("인증")); QCOMPARE(brokerHttp.authentications(), 1);
    }
    void overLimitRotatesEveryVerifiedTargetThroughBoundedRestBatches() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; brokerHttp.catalogRows = 505; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000); broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); qint64 now = 0;
        TossMarketStream stream(&broker, &marketHttp, std::move(socket), [&] { return now; });
        QList<TossMarketTarget> targets; for (int i = 0; i < 505; ++i) targets.append({QString::number(100000 + i), "KR", 3});
        targets.append({"UNVERIFIED", "US", 0}); targets.append({"100004", "US", 0});
        stream.setTargets(targets); emit fake->connected();
        QCOMPARE(stream.targets().size(), 100);
        QCOMPARE(stream.restOnlyTargets().size(), 405);
        QCOMPARE(stream.restOnlyTargets().last().symbol, QString("100504"));
        const auto declaration = QJsonDocument::fromJson(fake->sent.last()).array();
        QJsonArray subscribed; for (int i = 0; i < 100; ++i) subscribed.append("trade:kr:" + QString::number(100000 + i));
        emit fake->received(QJsonDocument(QJsonObject{{"type", "subscriptions"}, {"id", declaration.first().toObject().value("id")}, {"subscribed", subscribed}, {"rejected", QJsonArray{}}}).toJson());
        QCOMPARE(marketHttp.requests.size(), 1);
        const auto symbols = QUrlQuery(marketHttp.requests.first().url).queryItemValue("symbols").split(',');
        QCOMPARE(symbols.size(), 200); QCOMPARE(symbols.last(), QString("100199"));
        emit marketHttp.finished(marketHttp.tokens.last(), tossEnvelope(QJsonArray{QJsonObject{{"symbol", "100150"}, {"lastPrice", "700"},
            {"currency", "KRW"}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}}));
        QCOMPARE(stream.quotes().size(), 1); QVERIFY(stream.quotes().first().evidence.source.contains("REST"));
        now = 999; QTest::qWait(250); QCOMPARE(marketHttp.requests.size(), 1);
        now = 1000; QTRY_COMPARE_WITH_TIMEOUT(marketHttp.requests.size(), 2, 1000);
        const auto second = QUrlQuery(marketHttp.requests[1].url).queryItemValue("symbols").split(',');
        QCOMPARE(second.size(), 200); QCOMPARE(second.first(), QString("100200")); QCOMPARE(second.last(), QString("100399"));
        // A verified symbol in a later chunk is not evidence for this request.
        emit marketHttp.finished(marketHttp.tokens.last(), tossEnvelope(QJsonArray{QJsonObject{{"symbol", "100504"}, {"lastPrice", "701"},
            {"currency", "KRW"}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}}));
        QCOMPARE(stream.quotes().size(), 1);
        now = 1999; QTest::qWait(250); QCOMPARE(marketHttp.requests.size(), 2);
        now = 2000; QTRY_COMPARE_WITH_TIMEOUT(marketHttp.requests.size(), 3, 1000);
        const auto third = QUrlQuery(marketHttp.requests[2].url).queryItemValue("symbols").split(',');
        QCOMPARE(third.size(), 105); QCOMPARE(third.first(), QString("100400")); QCOMPARE(third.last(), QString("100504"));
        emit marketHttp.finished(marketHttp.tokens.last(), tossEnvelope(QJsonArray{QJsonObject{{"symbol", "100504"}, {"lastPrice", "702"},
            {"currency", "KRW"}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}}));
        QCOMPARE(stream.quotes().size(), 2);
        QStringList requested = symbols + second + third;
        QCOMPARE(requested.size(), 505); requested.removeDuplicates(); QCOMPARE(requested.size(), 505);
        QVERIFY(!requested.contains("UNVERIFIED"));
        now = 61999; QTest::qWait(250); QCOMPARE(marketHttp.requests.size(), 3);
        now = 62000; QTRY_COMPARE_WITH_TIMEOUT(marketHttp.requests.size(), 4, 1000);
        QCOMPARE(QUrlQuery(marketHttp.requests.last().url).queryItemValue("symbols").split(','), symbols);
        QCOMPARE(brokerHttp.authentications(), 1);
    }
    void overflowBatchesCancelBeforeLateReplies_data() {
        QTest::addColumn<QString>("ending");
        QTest::newRow("hidden-stop") << QString("stop");
        QTest::newRow("metadata-mismatch") << QString("metadata");
        QTest::newRow("vault-lock") << QString("lock");
    }
    void overflowBatchesCancelBeforeLateReplies() {
        QFETCH(QString, ending);
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; brokerHttp.catalogRows = 305; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000); broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); qint64 now = 0;
        TossMarketStream stream(&broker, &marketHttp, std::move(socket), [&] { return now; });
        QList<TossMarketTarget> targets; for (int i = 0; i < 305; ++i) targets.append({QString::number(100000 + i), "KR", 3});
        stream.setTargets(targets); emit fake->connected();
        QCOMPARE(stream.restOnlyTargets().size(), 205);
        const auto declaration = QJsonDocument::fromJson(fake->sent.last()).array();
        QJsonArray subscribed; for (int i = 0; i < 100; ++i) subscribed.append("trade:kr:" + QString::number(100000 + i));
        emit fake->received(QJsonDocument(QJsonObject{{"type", "subscriptions"}, {"id", declaration.first().toObject().value("id")},
            {"subscribed", subscribed}, {"rejected", QJsonArray{}}}).toJson());
        QCOMPARE(marketHttp.requests.size(), 1);
        const auto oldToken = marketHttp.tokens.last(); const auto beforeAbort = marketHttp.epoch;
        if (ending == "stop") stream.stop();
        else if (ending == "lock") vault.lock();
        else stream.setTargets({{"100304", "US", 0}, {"UNKNOWN", "KR", 0}});
        QVERIFY(marketHttp.epoch > beforeAbort);
        QVERIFY(stream.targets().isEmpty()); QVERIFY(stream.restOnlyTargets().isEmpty());
        emit marketHttp.finished(oldToken, tossEnvelope(QJsonArray{QJsonObject{{"symbol", "100150"}, {"lastPrice", "700"},
            {"currency", "KRW"}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}}));
        now = 61000; QTest::qWait(450);
        QVERIFY(stream.quotes().isEmpty()); QCOMPARE(marketHttp.requests.size(), 1);
        QCOMPARE(brokerHttp.authentications(), 1);
    }
    void handshakeTimeoutClosesBeforeRetryWithoutReauthentication() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000); broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); qint64 now = 0;
        TossMarketStream stream(&broker, &marketHttp, std::move(socket), [&] { return now; });
        stream.setTargets({{"005930", "KR", 0}}); QCOMPARE(fake->opens, 1);
        const int beforeTimeout = fake->aborts;
        now = 15000; QTest::qWait(250); QCOMPARE(fake->aborts, beforeTimeout);
        now = 15001; QTRY_VERIFY_WITH_TIMEOUT(fake->aborts > beforeTimeout, 1000);
        QCOMPARE(fake->opens, 1); QVERIFY(stream.quotes().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fake->opens, 2, 2000);
        QCOMPARE(brokerHttp.authentications(), 1);
    }
    void idleTimeoutClearsPricesBeforeRetryWithoutReauthentication() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake broker vault password 2026!")); QVERIFY(addToss(vault));
        FakeTransport brokerHttp, marketHttp; marketHttp.respond = false;
        BrokerTradingController broker(vault, &brokerHttp); broker.setAutomaticRefresh(false);
        QVERIFY(broker.selectConnection(vault.accounts().first().id)); broker.authenticate();
        QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000); broker.loadMarkets(); QTRY_VERIFY_WITH_TIMEOUT(!broker.busy(), 5000);
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); qint64 now = 0;
        TossMarketStream stream(&broker, &marketHttp, std::move(socket), [&] { return now; });
        stream.setTargets({{"005930", "KR", 0}}); emit fake->connected();
        const auto id = QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
        emit fake->received(QJsonDocument(QJsonObject{{"type", "subscriptions"}, {"id", id}, {"subscribed", QJsonArray{"trade:kr:005930"}}, {"rejected", QJsonArray{}}}).toJson());
        emit fake->received(QJsonDocument(QJsonObject{{"type", "message"}, {"topic", "trade:kr:005930"},
            {"data", QJsonObject{{"price", "70100"}, {"volume", "1"}, {"currency", "KRW"}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}}}).toJson());
        QCOMPARE(stream.quotes().size(), 1); const int beforeTimeout = fake->aborts;
        now = 180000; QTest::qWait(250); QCOMPARE(fake->aborts, beforeTimeout);
        now = 180001; QTRY_VERIFY_WITH_TIMEOUT(fake->aborts > beforeTimeout, 1000);
        QVERIFY(stream.quotes().isEmpty()); QCOMPARE(fake->opens, 1);
        QTRY_COMPARE_WITH_TIMEOUT(fake->opens, 2, 2000); QCOMPARE(brokerHttp.authentications(), 1);
    }
};
QTEST_GUILESS_MAIN(TossMarketStreamTest)
#include "tst_toss_market_stream.moc"
