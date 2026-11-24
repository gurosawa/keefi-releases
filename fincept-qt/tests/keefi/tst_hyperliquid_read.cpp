#include "keefi/HyperliquidReadController.h"
#include "keefi/AssetValuation.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <functional>
using namespace keefi;
namespace {
const QString Password = "Fake Hyperliquid Reads! 2026";
const QString Address = "0x1111111111111111111111111111111111111111";
QJsonObject state() {
    const QJsonObject position{{"coin", "ETH"}, {"szi", "-0.1"}, {"entryPx", "3022.5"},
        {"positionValue", "300"}, {"unrealizedPnl", "2.25"}, {"marginUsed", "10"},
        {"liquidationPx", "3900"}, {"leverage", QJsonObject{{"type", "isolated"}, {"value", "30"}}}};
    return {{"marginSummary", QJsonObject{{"accountValue", "100.25"}, {"totalNtlPos", "300"}, {"totalRawUsd", "98"}, {"totalMarginUsed", "10"}}},
        {"crossMarginSummary", QJsonObject{{"accountValue", "100.25"}, {"totalNtlPos", "300"}, {"totalRawUsd", "98"}, {"totalMarginUsed", "10"}}},
        {"crossMaintenanceMarginUsed", "1"}, {"withdrawable", "90.25"}, {"time", QString::number(QDateTime::currentMSecsSinceEpoch())},
        {"assetPositions", QJsonArray{QJsonObject{{"type", "oneWay"}, {"position", position}}}}};
}
class FakeTransport final : public TradingTransport {
  public:
    QList<RequestSpec> requests;
    QString abstraction = "disabled";
    QJsonObject accountState = state();
    QJsonObject spotState{{"balances", QJsonArray{QJsonObject{{"coin", "USDC"}, {"token", "0"}, {"total", "125.5"}, {"hold", "10"}, {"entryNtl", "0"}}}}};
    std::function<void(const RequestSpec&, HttpResult&)> transform;
    QList<QPair<quint64, HttpResult>> pending;
    bool hold = false;
    int aborted = 0;
    void abort() override { ++aborted; }
    void release() {
        hold = false;
        const auto responses = pending; pending.clear();
        for (const auto& response : responses)
            QTimer::singleShot(0, this, [this, response] { emit finished(response.first, response.second); });
    }
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(!isMutation(request.operation)); QVERIFY(!request.authenticated);
        QVERIFY(NetworkTradingTransport::allowed(request));
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.url, QUrl("https://api.hyperliquid.xyz/info"));
        QCOMPARE(request.headers.size(), 1);
        QCOMPARE(request.headers.value("Content-Type"), QByteArray("application/json"));
        requests.append(request);
        const auto body = QJsonDocument::fromJson(request.body).object();
        const auto type = body.value("type").toString();
        HttpResult response; response.status = 200;
        if (type == "userAbstraction") response.body = QJsonDocument(QJsonArray{abstraction}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
        else if (type == "metaAndAssetCtxs") response.body = QJsonDocument(QJsonArray{QJsonObject{{"collateralToken", "0"}, {"universe", QJsonArray{QJsonObject{{"name", "ETH"}}}}}, QJsonArray{QJsonObject{}}}).toJson();
        else if (type == "clearinghouseState") response.body = QJsonDocument(accountState).toJson();
        else if (type == "spotClearinghouseState") response.body = QJsonDocument(spotState).toJson();
        else QFAIL("Unexpected info type");
        if (transform) transform(request, response);
        if (hold) pending.append({token, response});
        else QTimer::singleShot(0, this, [this, token, response] { emit finished(token, response); });
    }
};
}
class HyperliquidReadTest final : public QObject {
    Q_OBJECT
  private slots:
    void readAddressValidationIsStrictAndAcceptsHexCase() {
        QVERIFY(HyperliquidReadController::validAddress(Address));
        QVERIFY(HyperliquidReadController::validAddress("0xAbCdEf0123456789AbCdEf0123456789AbCdEf01"));
        for (const auto& invalid : QStringList{QString(), "0x" + QString(40, '0'), Address + "\n", " " + Address,
             Address + " ", Address.mid(2), "0X" + Address.mid(2), Address.left(41), Address + "1", "0x" + QString(40, 'z')})
            QVERIFY2(!HyperliquidReadController::validAddress(invalid), qPrintable(invalid));
    }
    void accountEquityIsNotAddedToPositionExposureOrPnl() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE PERPS"; account.currency = "USDC";
        QString error;
        QVERIFY2(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault, &error), qPrintable(error));
        FakeTransport transport;
        HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        const auto capture = controller.assetCapture();
        QVERIFY(capture.snapshot.balanceComplete);
        QCOMPARE(capture.snapshot.market, QString("hyperliquid_perps"));
        QCOMPARE(capture.snapshot.components.size(), 2);
        QCOMPARE(capture.snapshot.components.first().kind, AssetComponentKind::Equity);
        QCOMPARE(capture.snapshot.components.first().quantity, QString("100.25"));
        QCOMPARE(capture.snapshot.components.first().asset, QString("USDC"));
        QCOMPARE(capture.snapshot.components.last().kind, AssetComponentKind::PositionNotional);
        QCOMPARE(controller.positions().first().amount, QString("-0.1"));
        QVERIFY(capture.quotes.isEmpty()); // No assumed USDC/USD peg.
        const auto value = AssetValuation::value({capture.snapshot}, {}, "USDC", QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(value.netAssets, QString("100.25"));
        QVERIFY(value.complete);
        QVERIFY(!value.rows.last().included);
        QCOMPARE(transport.requests.size(), 3);
    }
    void unifiedAccountReportsSharedUsdcWithoutInventingCompleteEquity() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE UNIFIED PERPS"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport; transport.abstraction = "unifiedAccount";
        HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        const auto capture = controller.assetCapture();
        QVERIFY2(capture.snapshot.timeMs > 0, qPrintable(controller.status()));
        QVERIFY(!capture.snapshot.balanceComplete);
        QCOMPARE(capture.snapshot.components.first().kind, AssetComponentKind::Cash);
        // The documented shared token total is neither total-hold nor total+perp PnL.
        QCOMPARE(capture.snapshot.components.first().quantity, QString("125.5"));
        QCOMPARE(capture.snapshot.components.first().asset, QString("USDC"));
        QCOMPARE(capture.snapshot.components.first().evidence.sourceTimeMs, qint64(0));
        QVERIFY(capture.snapshot.components.last().evidence.sourceTimeMs > 0);
        QVERIFY(capture.snapshot.coverage.contains("통합"));
        QVERIFY(capture.snapshot.coverage.contains("순자산 미확정"));
        QVERIFY(capture.quotes.isEmpty());
        const auto value = AssetValuation::value({capture.snapshot}, {}, "USDC", QDateTime::currentMSecsSinceEpoch());
        QVERIFY(!value.complete); QVERIFY(value.netAssets.isEmpty());
        QCOMPARE(value.knownSubtotal, QString("125.5"));
        QVERIFY(!value.rows.last().included);
        QCOMPARE(transport.requests.size(), 4);
        const auto scope = capture.snapshot.accountScope;
        transport.abstraction = "disabled"; transport.accountState = state();
        QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QVERIFY(controller.assetCapture().snapshot.balanceComplete);
        QCOMPARE(controller.assetCapture().snapshot.accountScope, scope);
    }
    void lockAtThePublicChangedBoundaryPreventsDispatch() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE LOCK BOUNDARY"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport; HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        bool locked = false;
        connect(&controller, &HyperliquidReadController::changed, &controller, [&] {
            if (controller.busy() && !locked) { locked = true; vault.lock(); }
        });
        controller.refreshAssets();
        QVERIFY(locked); QVERIFY(!vault.isUnlocked()); QVERIFY(!controller.busy());
        QVERIFY2(transport.requests.isEmpty(), "A synchronous lock from changed() must revoke the pending dispatch, including its account address.");
        QVERIFY(controller.assetCapture().snapshot.accountScope.isEmpty());
    }
    void trailingNewlineInAmountIsNotAValidBalance() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE STRICT DECIMAL"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport;
        auto summary = transport.accountState.value("marginSummary").toObject();
        summary["accountValue"] = "100.25\n"; transport.accountState["marginSummary"] = summary;
        HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QVERIFY2(controller.assetCapture().snapshot.timeMs == 0, "Malformed financial strings must not produce a successful balance capture.");
        QVERIFY(controller.status().contains("실패"));
    }
    void duplicateRefreshInterruptAndLateRepliesCannotReplaceTheSelectedRead() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE LATE READ"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport; transport.hold = true;
        HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets()); QVERIFY(controller.busy());
        const int aborted = transport.aborted;
        QVERIFY(controller.refreshAssets()); QCOMPARE(transport.requests.size(), 1); QCOMPARE(transport.aborted, aborted);
        const auto late = transport.pending.takeFirst();
        QVERIFY(controller.interruptRead()); QVERIFY(!controller.busy());
        QVERIFY(controller.refreshAssets()); QCOMPARE(transport.requests.size(), 2);
        emit transport.finished(late.first, late.second);
        QCOMPARE(transport.requests.size(), 2); QVERIFY(controller.busy());
        transport.release(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QVERIFY(controller.assetCapture().snapshot.balanceComplete); QCOMPARE(transport.requests.size(), 4);
        transport.hold = true; QVERIFY(controller.refreshAssets());
        const auto stopped = transport.pending.takeFirst(); controller.stop();
        emit transport.finished(stopped.first, stopped.second);
        QVERIFY(!controller.busy()); QVERIFY(controller.assetCapture().snapshot.accountScope.isEmpty());
        QVERIFY(controller.positions().isEmpty());
        vault.lock(); QVERIFY(!controller.canReconnectAssets());
    }
    void failedReadsKeepOriginalEvidenceAndRateLimitRequiresManualRetry() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE RETAINED READ"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport; HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        const auto original = controller.assetCapture().snapshot; QVERIFY(original.balanceComplete);
        transport.transform = [](const RequestSpec& request, HttpResult& response) {
            if (request.operation == Operation::Account) { response.status = 503; response.body = "PRIVATE_SYNTHETIC_ERROR"; }
        };
        QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QCOMPARE(controller.assetCapture().snapshot, original);
        QVERIFY(controller.status().contains("실패")); QVERIFY(controller.status().contains("이전"));
        QVERIFY(!controller.status().contains("PRIVATE_SYNTHETIC_ERROR"));
        transport.transform = {};
        transport.accountState["time"] = QString::number(original.evidence.sourceTimeMs - 1);
        QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QCOMPARE(controller.assetCapture().snapshot, original); // Fresh-looking but older source evidence is rejected.
        QVERIFY(controller.status().contains("실패"));
        transport.transform = [](const RequestSpec&, HttpResult& response) { response.status = 429; response.headers["Retry-After"] = "1"; };
        QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        const auto sent = transport.requests.size();
        QVERIFY(!controller.canReconnectAssets()); QVERIFY(!controller.refreshAssets()); QCOMPARE(transport.requests.size(), sent);
        QCOMPARE(controller.assetCapture().snapshot, original);
        QTest::qWait(1100); QCOMPARE(transport.requests.size(), sent); QVERIFY(controller.canReconnectAssets());
        transport.transform = {}; transport.accountState = state();
        QVERIFY(controller.refreshAssets()); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QVERIFY(controller.assetCapture().snapshot.balanceComplete);
        QVERIFY(controller.assetCapture().snapshot.evidence.receivedAtMs >= original.evidence.receivedAtMs);
        vault.lock(); QVERIFY(controller.assetCapture().snapshot.accountScope.isEmpty()); QVERIFY(controller.positions().isEmpty());
    }
    void caseVariantsAndDuplicateRegistrationsKeepOneUnderlyingScope() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        const QString address = "0xAbCdEf0123456789AbCdEf0123456789AbCdEf01";
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE CASE ONE"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", address}}, StorageMode::EncryptedVault));
        FakeTransport transport; HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        const auto first = controller.assetCapture().snapshot; QVERIFY(first.balanceComplete);
        QCOMPARE(QJsonDocument::fromJson(transport.requests.first().body).object().value("user").toString(), address.toLower());
        account.alias = "FAKE CASE TWO";
        QVERIFY(vault.addAccount(account, {{"address", address.toLower()}}, StorageMode::EncryptedVault));
        QString secondId;
        for (const auto& row : vault.accounts()) if (row.alias == account.alias) secondId = row.id;
        QVERIFY(!secondId.isEmpty()); QVERIFY(secondId != first.connectionId);
        QVERIFY(controller.selectAccount(secondId)); transport.accountState = state(); QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        const auto second = controller.assetCapture().snapshot; QVERIFY(second.balanceComplete);
        QCOMPARE(second.accountScope, first.accountScope); QCOMPARE(second.connectionId, secondId);
    }
    void unsupportedAbstractionsNeverBecomeZeroBalances_data() {
        QTest::addColumn<QString>("mode");
        for (const auto& mode : QStringList{"default", "portfolioMargin", "dexAbstraction", "unknown", ""})
            QTest::newRow(mode.isEmpty() ? "missing" : qPrintable(mode)) << mode;
    }
    void unsupportedAbstractionsNeverBecomeZeroBalances() {
        QFETCH(QString, mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE UNSUPPORTED MODE"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport; transport.abstraction = mode;
        HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QVERIFY(controller.assetCapture().snapshot.accountScope.isEmpty()); QVERIFY(controller.status().contains("실패"));
        QCOMPARE(transport.requests.size(), 1);
    }
    void malformedOrStaleEvidenceNeverBecomesACompleteCapture_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& fault : QStringList{"missing-equity", "bad-equity", "stale-time", "future-time", "missing-time", "duplicate-position", "unknown-position", "missing-size", "wrong-collateral", "missing-usdc", "duplicate-usdc", "hold-exceeds-total"})
            QTest::newRow(qPrintable(fault)) << fault;
    }
    void malformedOrStaleEvidenceNeverBecomesACompleteCapture() {
        QFETCH(QString, fault);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password));
        AccountRecord account; account.provider = "phantom"; account.market = "hyperliquid_perps";
        account.environment = "mainnet"; account.alias = "FAKE MALFORMED READ"; account.currency = "USDC";
        QVERIFY(vault.addAccount(account, {{"address", Address}}, StorageMode::EncryptedVault));
        FakeTransport transport;
        auto summary = transport.accountState.value("marginSummary").toObject();
        if (fault == "missing-equity") summary.remove("accountValue");
        if (fault == "bad-equity") summary["accountValue"] = "NaN";
        transport.accountState["marginSummary"] = summary;
        if (fault == "stale-time") transport.accountState["time"] = QString::number(QDateTime::currentMSecsSinceEpoch() - 60000);
        if (fault == "future-time") transport.accountState["time"] = QString::number(QDateTime::currentMSecsSinceEpoch() + 60000);
        if (fault == "missing-time") transport.accountState.remove("time");
        auto positions = transport.accountState.value("assetPositions").toArray();
        if (fault == "duplicate-position") positions.append(positions.first());
        auto wrapper = positions.first().toObject(), position = wrapper.value("position").toObject();
        if (fault == "unknown-position") position["coin"] = "xyz:HIP3";
        if (fault == "missing-size") position.remove("szi");
        wrapper["position"] = position; positions[0] = wrapper; transport.accountState["assetPositions"] = positions;
        if (fault == "wrong-collateral") transport.transform = [](const RequestSpec& request, HttpResult& response) {
            if (request.operation != Operation::Rules) return;
            auto values = QJsonDocument::fromJson(response.body).array(); auto meta = values[0].toObject();
            meta["collateralToken"] = "1"; values[0] = meta; response.body = QJsonDocument(values).toJson();
        };
        if (QStringList{"missing-usdc", "duplicate-usdc", "hold-exceeds-total"}.contains(fault)) {
            transport.abstraction = "unifiedAccount";
            auto balances = transport.spotState.value("balances").toArray();
            if (fault == "missing-usdc") balances = {};
            if (fault == "duplicate-usdc") balances.append(balances.first());
            if (fault == "hold-exceeds-total") { auto balance = balances[0].toObject(); balance["hold"] = "200"; balances[0] = balance; }
            transport.spotState["balances"] = balances;
        }
        HyperliquidReadController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id)); QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 1500);
        QVERIFY2(controller.assetCapture().snapshot.accountScope.isEmpty(), qPrintable(fault));
        QVERIFY(controller.status().contains("실패"));
    }
};
QTEST_GUILESS_MAIN(HyperliquidReadTest)
#include "tst_hyperliquid_read.moc"
