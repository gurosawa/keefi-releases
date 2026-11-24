#include "keefi/FuturesTradingController.h"
#include "keefi/TradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/BinanceUsdmResponses.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <type_traits>
using namespace keefi;
using namespace keefi::test;
namespace {
class TimeTransport final : public TradingTransport {
  public:
    QList<RequestSpec> requests;
    QList<int> delays;
    int timeCount = 0, timeStatus = 200, accountStatus = 200;
    bool virtualTiming = false;
    qint64 timingMs = 0;
    qint64 signedTime = 0;
    QElapsedTimer sampleAge;
    static constexpr qint64 ServerSample = 1700000000000LL;
    void abort() override { ++generation; }
    void send(quint64 token, const RequestSpec& request) override {
        requests.append(request);
        auto response = usdmResponse(request);
        int delay = 1;
        if (request.operation == Operation::Time) {
            delay = delays.value(timeCount++, 1);
            response = usdmJson(QJsonObject{{"serverTime", QString::number(ServerSample)}}, timeStatus);
            if (timeStatus != 200) {
                response = usdmJson(QJsonObject{{"code", timeStatus == 503 ? "-1000" : "-1003"}, {"msg", "FAKE_ONLY"}}, timeStatus);
                response.headers["retry-after"] = "60";
            }
        } else if (request.operation == Operation::Account && request.url.host() == "api.binance.com") {
            response = usdmJson(QJsonObject{{"canTrade", true}, {"accountType", "SPOT"}, {"balances", QJsonArray{
                QJsonObject{{"asset", "USDT"}, {"free", "10"}, {"locked", "0"}}}}});
        }
        if (request.operation == Operation::Account && accountStatus != 200) {
            response = usdmJson(QJsonObject{{"code", "-1003"}, {"msg", "FAKE_ONLY"}}, accountStatus);
            response.headers["retry-after"] = "60";
        }
        if (request.authenticated && !signedTime)
            signedTime = QUrlQuery(request.url).queryItemValue("timestamp").toLongLong() - sampleAge.elapsed();
        const auto epoch = generation;
        QTimer::singleShot(virtualTiming ? 1 : delay, Qt::PreciseTimer, this, [this, token, response, epoch, delay, operation = request.operation] {
            if (epoch != generation) return;
            if (operation == Operation::Time) { timingMs += delay; sampleAge.start(); }
            emit finished(token, response);
        });
    }
  private:
    quint64 generation = 0;
};
template<class Controller> std::unique_ptr<Controller> makeController(Vault& vault, TimeTransport& transport) {
    // The old-commit RED companion has no injected RTT clock. Keep the same
    // behavioural cases runnable there with actual delayed fake responses.
    if constexpr (std::is_constructible_v<Controller, Vault&, TradingTransport*, QObject*, std::function<qint64()>>) {
        transport.virtualTiming = true;
        return std::make_unique<Controller>(vault, &transport, static_cast<QObject*>(nullptr), [&transport] { return transport.timingMs; });
    } else return std::make_unique<Controller>(vault, &transport);
}
bool addAccount(Vault& vault, bool futures) {
    auto account = usdmAccount("live"); account.id.clear();
    if (!futures) account.market = "spot";
    return vault.addAccount(account, {{"api_key", "TimeFakeKey2026Only"}, {"secret", "TimeFakeSecret2026Only"}}, StorageMode::EncryptedVault);
}
template<class Controller> void checkRead(Vault& vault, TimeTransport& transport, int retryDelay, bool success) {
    auto owner = makeController<Controller>(vault, transport); auto& controller = *owner;
    QVERIFY(controller.selectAccount(vault.accounts().first().id));
    QVERIFY(controller.refreshAssets());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 18000);
    QCOMPARE(transport.timeCount, retryDelay >= 0 ? 2 : 1);
    const bool accountRequested = std::any_of(transport.requests.cbegin(), transport.requests.cend(), [](const auto& r) { return r.operation == Operation::Account; });
    QVERIFY2(accountRequested == success, qPrintable(controller.status()));
    QCOMPARE(!controller.assetCapture().snapshot.id.isEmpty(), success);
    QVERIFY(!controller.liveEnabled());
    if (success) {
        // A sample produced before the response must not be advanced by half
        // a slow RTT: Binance also rejects timestamps >= serverTime + 1000.
        QVERIFY(qAbs(transport.signedTime - TimeTransport::ServerSample) < 50);
    } else {
        QVERIFY2(controller.status().contains("서버 시간"), qPrintable(controller.status()));
        QVERIFY2(controller.status().contains("왕복"), qPrintable(controller.status()));
        QVERIFY2(controller.status().contains("다시 조회"), qPrintable(controller.status()));
        QVERIFY(!controller.status().contains("주문"));
    }
}
template<class Controller> void checkPreviousSnapshot(Vault& vault, TimeTransport& transport) {
    auto owner = makeController<Controller>(vault, transport); auto& controller = *owner;
    QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 3000);
    const auto previous = controller.assetCapture().snapshot; QVERIFY2(!previous.id.isEmpty(), qPrintable(controller.status()));
    QTest::qWait(400); transport.timeStatus = 503;
    QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 3000);
    QCOMPARE(transport.timeCount, 3); // One initial success, then two failed Time reads.
    const auto retained = controller.assetCapture().snapshot;
    QCOMPARE(retained.id, previous.id); QCOMPARE(retained.timeMs, previous.timeMs);
    QVERIFY2(controller.status().contains("서버 시간"), qPrintable(controller.status()));
    QVERIFY2(controller.status().contains("HTTP 503"), qPrintable(controller.status()));
    QVERIFY(controller.status().contains("다시 조회")); QVERIFY(!controller.status().contains("주문"));
}
}
class BalanceTimeTest : public QObject {
    Q_OBJECT
  private slots:
    void assetsReadUsesItsOwnTimeBudget_data() {
        QTest::addColumn<bool>("futures"); QTest::addColumn<int>("delay");
        QTest::addColumn<int>("retryDelay"); QTest::addColumn<bool>("success");
        for (bool futures : {false, true}) {
            const QByteArray prefix = futures ? "futures-" : "spot-";
            QTest::newRow((prefix + "1999").constData()) << futures << 1999 << -1 << true;
            QTest::newRow((prefix + "2001").constData()) << futures << 2001 << -1 << true;
            QTest::newRow((prefix + "3999").constData()) << futures << 3999 << -1 << true;
            QTest::newRow((prefix + "4000-recovery").constData()) << futures << 4000 << 1 << true;
            QTest::newRow((prefix + "5000-recovery").constData()) << futures << 5000 << 1 << true;
            QTest::newRow((prefix + "5000-failure").constData()) << futures << 5000 << 5000 << false;
        }
    }
    void assetsReadUsesItsOwnTimeBudget() {
        QFETCH(bool, futures); QFETCH(int, delay); QFETCH(int, retryDelay); QFETCH(bool, success);
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create("Time fake password 2026 only!"));
        QVERIFY(addAccount(vault, futures)); TimeTransport transport; transport.delays = {delay};
        if (retryDelay >= 0) transport.delays.append(retryDelay);
        if (futures) checkRead<FuturesTradingController>(vault, transport, retryDelay, success);
        else checkRead<TradingController>(vault, transport, retryDelay, success);
    }
    void orderPreparationKeepsTheStrictTimeBudget_data() {
        QTest::addColumn<bool>("futures");
        QTest::newRow("spot") << false; QTest::newRow("futures") << true;
    }
    void orderPreparationKeepsTheStrictTimeBudget() {
        QFETCH(bool, futures); QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Time fake password 2026 only!")); QVERIFY(addAccount(vault, futures));
        TimeTransport transport; transport.delays = {2001};
        if (futures) {
            auto owner = makeController<FuturesTradingController>(vault, transport); auto& controller = *owner; QVERIFY(controller.selectAccount(vault.accounts().first().id));
            controller.refresh("BTCUSDT"); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 6000);
            QVERIFY(!controller.connected()); QVERIFY(!controller.liveEnabled());
        } else {
            auto owner = makeController<TradingController>(vault, transport); auto& controller = *owner; QVERIFY(controller.selectAccount(vault.accounts().first().id));
            controller.refresh("BTCUSDT"); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 6000);
            QVERIFY(!controller.connected()); QVERIFY(!controller.liveEnabled());
        }
        QCOMPARE(transport.requests.size(), 1); QCOMPARE(transport.timeCount, 1);
    }
    void rateLimitNeverRetriesTime_data() {
        QTest::addColumn<bool>("futures"); QTest::addColumn<int>("status");
        QTest::newRow("spot-429") << false << 429; QTest::newRow("spot-418") << false << 418;
        QTest::newRow("futures-429") << true << 429; QTest::newRow("futures-418") << true << 418;
    }
    void rateLimitNeverRetriesTime() {
        QFETCH(bool, futures); QFETCH(int, status); QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Time fake password 2026 only!")); QVERIFY(addAccount(vault, futures));
        TimeTransport transport; transport.timeStatus = status;
        if (futures) {
            auto owner = makeController<FuturesTradingController>(vault, transport); auto& controller = *owner; QVERIFY(controller.selectAccount(vault.accounts().first().id));
            QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
            QTest::qWait(400);
            QVERIFY(!controller.refreshAssets()); QVERIFY(!controller.reconnectAssets());
            QVERIFY2(!controller.status().contains("주문"), qPrintable(controller.status()));
        } else {
            auto owner = makeController<TradingController>(vault, transport); auto& controller = *owner; QVERIFY(controller.selectAccount(vault.accounts().first().id));
            QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
            QTest::qWait(400);
            QVERIFY(!controller.refreshAssets()); QVERIFY(!controller.reconnectAssets());
            QVERIFY2(!controller.status().contains("주문"), qPrintable(controller.status()));
        }
        QTest::qWait(400); QCOMPARE(transport.requests.size(), 1); QCOMPARE(transport.timeCount, 1);
    }
    void failedTimeReadKeepsTheOriginalSnapshotAge_data() {
        QTest::addColumn<bool>("futures"); QTest::newRow("spot") << false; QTest::newRow("futures") << true;
    }
    void failedTimeReadKeepsTheOriginalSnapshotAge() {
        QFETCH(bool, futures); QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Time fake password 2026 only!")); QVERIFY(addAccount(vault, futures)); TimeTransport transport;
        if (futures) checkPreviousSnapshot<FuturesTradingController>(vault, transport);
        else checkPreviousSnapshot<TradingController>(vault, transport);
    }
    void balanceRateLimitDoesNotUseOrderInstructions_data() {
        QTest::addColumn<bool>("futures"); QTest::newRow("spot") << false; QTest::newRow("futures") << true;
    }
    void balanceRateLimitDoesNotUseOrderInstructions() {
        QFETCH(bool, futures); QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Time fake password 2026 only!")); QVERIFY(addAccount(vault, futures));
        TimeTransport transport; transport.accountStatus = 429;
        const auto check = [&](auto& controller) {
            QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 3000);
            QVERIFY2(!controller.status().contains("주문"), qPrintable(controller.status()));
            QVERIFY(controller.status().contains("잔고")); QVERIFY(controller.status().contains("왕복"));
            QCOMPARE(transport.timeCount, 1);
            QCOMPARE(std::count_if(transport.requests.cbegin(), transport.requests.cend(), [](const auto& r) { return r.operation == Operation::Account; }), 1);
        };
        if (futures) { auto owner = makeController<FuturesTradingController>(vault, transport); check(*owner); }
        else { auto owner = makeController<TradingController>(vault, transport); check(*owner); }
    }
};
QTEST_GUILESS_MAIN(BalanceTimeTest)
#include "tst_balance_time.moc"
