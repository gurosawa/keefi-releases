#include "keefi/collector/CollectorRuntime.h"
#include "keefi/MarketCandles.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>
#include <functional>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000;
using Role = CollectorRuntimeDependencies::TransportRole;

HttpResult response(const QByteArray& body) {
    HttpResult result;
    result.status = 200;
    result.body = body;
    return result;
}
QByteArray bytes(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QByteArray bytes(const QJsonArray& array) { return QJsonDocument(array).toJson(QJsonDocument::Compact); }

class Wire final : public TradingTransport {
  public:
    QList<RequestSpec> requests;
    QList<quint64> tokens;
    std::function<HttpResult(const RequestSpec&)> responder;
    int replies = 0;
    void send(quint64 token, const RequestSpec& request) override {
        tokens.append(token);
        requests.append(request);
        if (responder) QTimer::singleShot(0, this, [this, token, request] {
            const auto result = responder(request);
            ++replies;
            emit finished(token, result);
        });
    }
    void abort() override {}
    void answer(const HttpResult& result) {
        ++replies;
        emit finished(tokens.last(), result);
    }
};

class Socket final : public PublicCandleSocket {
  public:
    QUrl endpoint;
    quint64 generation = 0;
    void open(quint64 next, const QUrl& url) override {
        generation = next;
        endpoint = url;
        QTimer::singleShot(0, this, [this, next] { emit connected(next); });
    }
    void send(quint64, const QByteArray&) override {}
    void abort() override {}
    void deliver(const QByteArray& payload) { emit received(generation, payload); }
};

struct Harness {
    QTemporaryDir directory;
    QElapsedTimer elapsed;
    const qint64 base = QDateTime(QDate(2026, 11, 6), QTime(18, 1), QTimeZone::UTC).toMSecsSinceEpoch();
    qint64 advanceMs = 0;
    QHash<int, QList<Wire*>> wires;
    QList<Socket*> sockets;
    QString readError;
    int unexpectedRequests = 0;

    Harness() { elapsed.start(); }
    qint64 now() const { return base + advanceMs + elapsed.elapsed(); }
    QString root() const { return directory.path() + "/collector"; }
    CollectorRuntimeOptions options(bool pipeline = false) const {
        CollectorRuntimeOptions result;
        result.root = root();
        result.revision = "synthetic-runtime-append-retry";
        result.diskFloorBytes = 0;
        result.binanceCoins = pipeline ? QStringList{"SYN"} : QStringList{};
        result.upbitCoins.clear();
        return result;
    }
    CollectorRuntimeDependencies dependencies() {
        CollectorRuntimeDependencies result;
        result.clock = [this] { return now(); };
        result.transport = [this](Role role) -> std::unique_ptr<TradingTransport> {
            auto wire = std::make_unique<Wire>();
            wires[int(role)].append(wire.get());
            if (role == Role::Flow || role == Role::Minutes || role == Role::Clock)
                wire->responder = [this, role](const RequestSpec& request) { return publicResponse(role, request); };
            return wire;
        };
        result.socket = [this]() -> std::unique_ptr<PublicCandleSocket> {
            auto socket = std::make_unique<Socket>();
            sockets.append(socket.get());
            return socket;
        };
        return result;
    }
    Wire* wire(Role role) const { return wires.value(int(role)).isEmpty() ? nullptr : wires.value(int(role)).last(); }
    Socket* socket(bool future) const {
        for (auto* candidate : sockets)
            if (!candidate->endpoint.isEmpty() && (candidate->endpoint.path() == "/market/stream") == future) return candidate;
        return nullptr;
    }
    bool block(const QString& relative) const {
        const auto path = root() + '/' + relative;
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile obstacle(path);
        return obstacle.open(QIODevice::WriteOnly) && obstacle.write("synthetic blocked directory") > 0;
    }
    bool unblock(const QString& relative) const { return QFile::remove(root() + '/' + relative); }
    HttpResult publicResponse(Role role, const RequestSpec& request) {
        // This adapter never delegates to NetworkTradingTransport. Any unexpected
        // endpoint fails locally, so a fixture omission cannot make a real read.
        if (request.authenticated || request.method != "GET") {
            ++unexpectedRequests;
            HttpResult rejected; rejected.status = 403; return rejected;
        }
        const auto path = request.url.path();
        if (role == Role::Clock && path == "/api/v3/time")
            return response(bytes(QJsonObject{{"serverTime", QString::number(now())}}));
        if (role == Role::Flow && path.endsWith("/exchangeInfo")) {
            QJsonObject item{{"symbol", "SYNUSDT"}, {"baseAsset", "SYN"}, {"quoteAsset", "USDT"}, {"status", "TRADING"}};
            if (request.url.host() == "api.binance.com") item.insert("isSpotTradingAllowed", true);
            else {
                item.insert("marginAsset", "USDT"); item.insert("contractType", "PERPETUAL"); item.insert("underlyingType", "COIN");
            }
            return response(bytes(QJsonObject{{"symbols", QJsonArray{item}}}));
        }
        if (role == Role::Flow && path.endsWith("/aggTrades"))
            return response(bytes(QJsonArray{trade("1", base - 1000)}));
        if (role == Role::Flow && path == "/futures/data/openInterestHist") return response("[]");
        if (role == Role::Minutes && path == "/api/v3/klines") {
            const QJsonArray candle{QString::number(base - Minute), "100", "110", "90", "105", "2",
                                    QString::number(base - 1), "210", "5", "1", "105", "0"};
            QJsonArray rows; rows.append(candle);
            return response(bytes(rows));
        }
        ++unexpectedRequests;
        HttpResult rejected; rejected.status = 404; return rejected;
    }
    QJsonObject trade(const QString& id, qint64 at) const {
        return {{"e", "aggTrade"}, {"E", QString::number(at)}, {"s", "SYNUSDT"}, {"a", id}, {"p", "100"}, {"q", "2"},
                {"f", id}, {"l", id}, {"T", QString::number(at)}, {"m", false}};
    }
    void sendTrade(bool future, const QString& id, qint64 at) {
        const auto payload = trade(id, at);
        socket(future)->deliver(future ? bytes(QJsonObject{{"stream", "synusdt@aggTrade"}, {"data", payload}}) : bytes(payload));
    }
    void sendLiquidation() {
        const QJsonObject order{{"s", "SYNUSDT"}, {"S", "SELL"}, {"q", "5"}, {"p", "99"}, {"ap", "100"},
                                {"X", "PARTIALLY_FILLED"}, {"l", "1"}, {"z", "2"}, {"T", QString::number(base - 2)}};
        socket(true)->deliver(bytes(QJsonObject{{"stream", "synusdt@forceOrder"},
            {"data", QJsonObject{{"e", "forceOrder"}, {"E", QString::number(base - 1)}, {"o", order}}}}));
    }
    HttpResult fx(bool ecb) const {
        if (ecb) return response("<Envelope><Cube>"
            "<Cube time=\"2026-11-05\"><Cube currency=\"USD\" rate=\"1\"/><Cube currency=\"KRW\" rate=\"1400\"/></Cube>"
            "<Cube time=\"2026-11-06\"><Cube currency=\"USD\" rate=\"1\"/><Cube currency=\"KRW\" rate=\"1401\"/></Cube>"
            "</Cube></Envelope>");
        const auto posted = QDateTime::fromMSecsSinceEpoch(base - Minute, QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd'T'HH:mm:ss") + "+09:00";
        return response(bytes(QJsonObject{{"isSuccess", true}, {"result", QJsonObject{{"categoryType", "exchange"},
            {"reutersCode", "FX_USDKRW"}, {"closePrice", "1,400.00"}, {"fluctuations", "1.40"}, {"fluctuationsRatio", "0.10"},
            {"marketStatus", "OPEN"}, {"localTradedAt", posted},
            {"stockExchangeType", QJsonObject{{"nameKor", QStringLiteral("하나은행")}, {"delayTime", 0}}}}}}));
    }
    QList<CollectorStoredRecord> saved(const QString& kind) {
        CollectorStoreOptions options;
        options.root = root(); options.diskFloorBytes = 0;
        CollectorStore reader(options, [this] { return now(); });
        if (!reader.open(&readError)) return {};
        return reader.recentRecords(kind, 0);
    }
};

qint64 records(const CollectorRuntime& runtime, const QString& stream) {
    return runtime.summary().value("streams").toObject().value(stream).toObject().value("records").toInteger();
}
qint64 duplicates(const CollectorRuntime& runtime, const QString& venue) {
    return runtime.summary().value("streams").toObject().value("trade/" + venue + "/SYNUSDT").toObject().value("duplicates").toInteger();
}
} // namespace

class CollectorRuntimeTests final : public QObject {
    Q_OBJECT
  private slots:
    void failedFxAppendIsRetriedByTheNextRequest_data() {
        QTest::addColumn<bool>("ecb");
        QTest::newRow("ECB dates and ten-minute storage retry") << true;
        QTest::newRow("Hana posting") << false;
    }
    void failedFxAppendIsRetriedByTheNextRequest() {
        QFETCH(bool, ecb);
        Harness h;
        QVERIFY(h.directory.isValid());
        QVERIFY(h.block("fx"));
        CollectorRuntime runtime(h.options(), h.dependencies());
        QString error;
        QVERIFY2(runtime.start(&error), qPrintable(error));
        auto* wire = h.wire(ecb ? Role::Ecb : Role::Hana);
        QVERIFY(wire);
        QCOMPARE(wire->requests.size(), 1);
        const QString venue = ecb ? "ecb" : "naver-hana";
        const QString stream = "fx/" + venue + "/USDKRW";
        wire->answer(h.fx(ecb));
        QCOMPARE(records(runtime, stream), 0);
        runtime.stop();

        QVERIFY(h.unblock("fx"));
        // Public lifecycle: the same Runtime starts again and issues a new read.
        // An unsaved ECB batch must retain its existing ten-minute retry policy,
        // even though that batch included today's publication.
        h.advanceMs += (ecb ? 10 : 1) * Minute + 1;
        QVERIFY2(runtime.start(&error), qPrintable(error));
        QCOMPARE(wire->requests.size(), 2);
        wire->answer(h.fx(ecb));
        QCOMPARE(records(runtime, stream), ecb ? 2 : 1);
        runtime.stop();

        h.advanceMs += (ecb ? 10 : 1) * Minute + 1;
        QVERIFY2(runtime.start(&error), qPrintable(error));
        if (ecb) QCOMPARE(wire->requests.size(), 2); // Today's successful date is now durable.
        else { QCOMPARE(wire->requests.size(), 3); wire->answer(h.fx(false)); }
        QCOMPARE(records(runtime, stream), ecb ? 2 : 1);
        runtime.stop();
        const auto saved = h.saved("fx");
        QVERIFY2(h.readError.isEmpty(), qPrintable(h.readError));
        QCOMPARE(saved.size(), ecb ? 2 : 1);
        for (const auto& record : saved) QCOMPARE(record.venue, venue);
        QCOMPARE(h.unexpectedRequests, 0);
    }

    void retainedSourceRecordsRetryAfterStorageRecovers_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("completed minute") << QString("kline");
        QTest::newRow("liquidation snapshot") << QString("liquidation");
        QTest::newRow("coverage period") << QString("coverage");
    }
    void retainedSourceRecordsRetryAfterStorageRecovers() {
        QFETCH(QString, kind);
        Harness h;
        QVERIFY(h.directory.isValid());
        QVERIFY(h.block(kind));
        CollectorRuntime runtime(h.options(true), h.dependencies());
        QString error;
        QVERIFY2(runtime.start(&error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(h.socket(false) && h.socket(true), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(h.wire(Role::Minutes) && h.wire(Role::Minutes)->replies == 1, 5000);
        h.sendLiquidation();
        h.sendTrade(false, "2", h.now() - 1);
        // This second raw trade is a checkpoint through the real 2-second
        // collect path after the closed candle / liquidation arrived.
        QTRY_COMPARE_WITH_TIMEOUT(records(runtime, "trade/binance-spot/SYNUSDT"), 2, 5000);
        const QString venue = kind == "liquidation" ? "binance-usdm" : "binance-spot";
        const auto stream = kind + '/' + venue + "/SYNUSDT";
        QCOMPARE(records(runtime, stream), 0);

        QVERIFY(h.unblock(kind));
        h.sendTrade(false, "3", h.now() - 1);
        QTRY_COMPARE_WITH_TIMEOUT(records(runtime, "trade/binance-spot/SYNUSDT"), 3, 5000);
        QCOMPARE(records(runtime, stream), 1);
        h.sendTrade(false, "4", h.now() - 1);
        QTRY_COMPARE_WITH_TIMEOUT(records(runtime, "trade/binance-spot/SYNUSDT"), 4, 5000);
        QCOMPARE(records(runtime, stream), 1); // Saved observations are not appended again.
        runtime.stop();
        const auto saved = h.saved(kind);
        QVERIFY2(h.readError.isEmpty(), qPrintable(h.readError));
        int matching = 0;
        for (const auto& row : saved) if (row.venue == venue && row.symbol == "SYNUSDT") ++matching;
        QCOMPARE(matching, 1);
        QCOMPARE(h.unexpectedRequests, 0);
    }

    void failedDuplicateFlushKeepsCountAndOriginalIntervalPerStream() {
        Harness h;
        QVERIFY(h.directory.isValid());
        QVERIFY(h.block("dup/binance-spot"));
        CollectorRuntime runtime(h.options(true), h.dependencies());
        QString error;
        QVERIFY2(runtime.start(&error), qPrintable(error));
        const auto from = runtime.summary().value("startedAt").toInteger();
        QTRY_VERIFY_WITH_TIMEOUT(h.socket(false) && h.socket(true), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(records(runtime, "trade/binance-spot/SYNUSDT"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(records(runtime, "trade/binance-usdm/SYNUSDT"), 1, 5000);
        h.sendTrade(false, "1", h.base - 1000);
        h.sendTrade(true, "1", h.base - 1000);
        QTRY_COMPARE_WITH_TIMEOUT(duplicates(runtime, "binance-spot"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(duplicates(runtime, "binance-usdm"), 1, 5000);

        // Exercise the production sixty-second flush, without exposing a private
        // timer or changing its cadence. The futures stream succeeds while the
        // spot directory is blocked by an ordinary file in this test's temp root.
        QTRY_COMPARE_WITH_TIMEOUT(records(runtime, "dup/binance-usdm/SYNUSDT"), 1, 65000);
        QCOMPARE(records(runtime, "dup/binance-spot/SYNUSDT"), 0);
        const auto firstFlush = runtime.summary().value("streams").toObject()
            .value("dup/binance-usdm/SYNUSDT").toObject().value("lastReceivedAt").toInteger();
        QVERIFY(firstFlush > from);
        QVERIFY(h.unblock("dup/binance-spot"));
        h.sendTrade(false, "1", h.base - 1000);
        h.sendTrade(true, "1", h.base - 1000);
        QTRY_COMPARE_WITH_TIMEOUT(duplicates(runtime, "binance-spot"), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(duplicates(runtime, "binance-usdm"), 2, 5000);
        runtime.stop(); // Retries the pending counts through the public flush-on-stop path.

        const auto saved = h.saved("dup");
        QVERIFY2(h.readError.isEmpty(), qPrintable(h.readError));
        QList<QJsonObject> spot, futures;
        for (const auto& row : saved) {
            if (row.venue == "binance-spot") spot.append(row.object);
            if (row.venue == "binance-usdm") futures.append(row.object);
        }
        QCOMPARE(spot.size(), 1);
        QCOMPARE(spot.first().value("n").toInteger(), 2);
        QCOMPARE(spot.first().value("from").toInteger(), from);
        QCOMPARE(futures.size(), 2);
        QCOMPARE(futures[0].value("n").toInteger(), 1);
        QCOMPARE(futures[0].value("from").toInteger(), from);
        QCOMPARE(futures[0].value("to").toInteger(), firstFlush);
        QCOMPARE(futures[1].value("n").toInteger(), 1);
        QCOMPARE(futures[1].value("from").toInteger(), firstFlush);
        QVERIFY(spot.first().value("to").toInteger() >= firstFlush);
        QCOMPARE(h.unexpectedRequests, 0);
    }
};

QTEST_GUILESS_MAIN(CollectorRuntimeTests)
#include "tst_collector_runtime.moc"
