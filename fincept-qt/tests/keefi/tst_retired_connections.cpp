#include "fixtures/SuspendedBrokerVault.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/PortfolioController.h"
#include "keefi/SecureBuffer.h"
#include "keefi/TradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cstring>
#include <sodium.h>

using namespace keefi;
namespace {
const QString Password = "Fake retired connection password 2026!";

class RecordingTransport final : public TradingTransport {
public:
    QList<Operation> sent;
    void send(quint64, const RequestSpec& request) override { sent.append(request.operation); }
    void abort() override {}
};

// Only a vault created here with fixed fake credentials reaches this helper.
// Re-authenticate historical metadata without bypassing production unlock().
bool retireSyntheticAccount(const QString& path, const QString& environment) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QByteArray file = input.readAll(); input.close();
    const auto* raw = reinterpret_cast<const unsigned char*>(file.constData());
    SecureBuffer key(32);
    const auto password = Password.toUtf8();
    if (crypto_pwhash(key.data(), key.size(), password.constData(), password.size(), raw + 28,
        qFromLittleEndian<quint32>(raw + 16), qFromLittleEndian<quint64>(raw + 20),
        crypto_pwhash_ALG_ARGON2ID13) != 0) return false;
    SecureBuffer plain(file.size() - Vault::HeaderBytes - 16);
    unsigned long long plainSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &plainSize, nullptr,
        raw + Vault::HeaderBytes, file.size() - Vault::HeaderBytes, raw, Vault::HeaderBytes,
        raw + 44, key.data()) != 0) return false;
    const auto jsonLength = qFromLittleEndian<quint32>(plain.data() + 4);
    auto root = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(plain.data() + 8), jsonLength)).object();
    auto accounts = root["accounts"].toArray();
    auto sets = root["credential_sets"].toArray();
    if (accounts.size() != 1 || sets.size() != 1) return false;
    auto account = accounts[0].toObject(); account["environment"] = environment; accounts[0] = account;
    auto set = sets[0].toObject(); set["environment"] = environment; sets[0] = set;
    root["accounts"] = accounts; root["credential_sets"] = sets;
    const auto metadata = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const auto tailSize = plainSize - 8 - jsonLength;
    SecureBuffer changed(8 + metadata.size() + tailSize);
    std::memcpy(changed.data(), plain.data(), 4);
    qToLittleEndian<quint32>(metadata.size(), changed.data() + 4);
    std::memcpy(changed.data() + 8, metadata.constData(), metadata.size());
    std::memcpy(changed.data() + 8 + metadata.size(), plain.data() + 8 + jsonLength, tailSize);
    file.resize(Vault::HeaderBytes + changed.size() + 16);
    auto* out = reinterpret_cast<unsigned char*>(file.data());
    randombytes_buf(out + 44, 24);
    qToLittleEndian<quint32>(changed.size() + 16, out + 68);
    unsigned long long encryptedSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(out + Vault::HeaderBytes, &encryptedSize,
        changed.data(), changed.size(), out, Vault::HeaderBytes, nullptr, out + 44, key.data()) != 0) return false;
    QFile output(path);
    return output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        output.write(file) == file.size() && output.flush();
}
}

class RetiredConnectionTests final : public QObject {
    Q_OBJECT
private slots:
    void existingLiveBrokersNeverReadOrReauthenticate() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("profile/credentials.keefi-vault"));
        QVERIFY(test::openSuspendedBrokerFixture(vault));
        QCOMPARE(vault.accounts().size(), 2);
        RecordingTransport transport;
        int factoryCalls = 0;
        PortfolioController::Factories factories;
        factories.trading = [&](const AccountRecord&) { ++factoryCalls; return &transport; };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE(portfolio.connections().size(), 2);
        for (const auto& account : vault.accounts()) {
            QVERIFY(AccountModel::isProviderSuspended(account.provider));
            bool borrowed = false;
            QVERIFY(!vault.withCredentials(account.id, [&](const CredentialSet&) { borrowed = true; }));
            QVERIFY(!borrowed);
            auto candidate = account;
            candidate.id.clear();
            candidate.credentialRef.clear();
            QMap<QString,QString> replacement{{"app_key", "fake-app-key"}, {"app_secret", "fake-app-secret"}};
            if (account.provider == "kis") {
                replacement["account_number"] = "12345678";
                replacement["product_code"] = "01";
            }
            QVERIFY(!vault.replaceCredentials(account.id, replacement));
            QVERIFY(!vault.addAccount(candidate, replacement, StorageMode::EncryptedVault));
            QVERIFY(!portfolio.broker(account.id));
            portfolio.refreshConnection(account.id);
            QVERIFY(!portfolio.canReconnectConnection(account.id));
            QVERIFY(!portfolio.reconnectConnection(account.id));
            portfolio.suspend(account.id, true);
            portfolio.suspend(account.id, false);
        }
        portfolio.refreshAll();
        portfolio.stopReads();
        portfolio.setAutomaticRefresh(false);
        portfolio.setAutomaticRefresh(true);
        QTest::qWait(1100); // Cover the automatic read timer as well as explicit refresh.
        for (const auto& connection : portfolio.connections()) {
            QCOMPARE(connection.state, QString("unsupported"));
            QVERIFY(!connection.active);
        }
        QVERIFY(portfolio.snapshots().isEmpty());
        QCOMPARE(factoryCalls, 0);
        QVERIFY(transport.sent.isEmpty());
        for (const auto& account : vault.accounts()) QVERIFY(vault.removeAccount(account.id));
        vault.lock();
        QVERIFY(vault.unlock("Synthetic suspended brokers 2026!"));
        QVERIFY(vault.accounts().isEmpty());
    }
    void legacyConnectionsCannotBeSelectedOrSendRequests_data() {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("market");
        QTest::addColumn<QString>("environment");
        QTest::newRow("binance-spot-testnet") << QString("binance") << QString("spot") << QString("testnet");
        QTest::newRow("okx-spot-demo") << QString("okx") << QString("spot") << QString("demo");
        QTest::newRow("binance-futures-demo") << QString("binance") << QString("usdm_futures") << QString("demo");
        for (const auto& provider : {QString("kis"), QString("kiwoom"), QString("ls")})
            QTest::newRow(qPrintable(provider + "-paper")) << provider << QString("domestic_equity") << QString("paper");
    }
    void legacyConnectionsCannotBeSelectedOrSendRequests() {
        QFETCH(QString, provider); QFETCH(QString, market); QFETCH(QString, environment);
        QTemporaryDir dir;
        const auto path = dir.filePath("profile/synthetic-legacy.bin");
        Vault vault(path);
        AccountRecord account;
        account.provider = provider; account.alias = "가짜 은퇴 연결";
        account.market = market; account.environment = "live";
        account.currency = market == "domestic_equity" ? "KRW" : "USDT";
        QMap<QString, QString> fakeCredentials{{"api_key", "FAKE_RETIRED_KEY_ONLY"}, {"secret", "FAKE_RETIRED_SECRET_ONLY"}};
        if (provider == "okx") {
            account.region = "global"; fakeCredentials["passphrase"] = "FAKE_RETIRED_PASSPHRASE_ONLY";
        } else if (market == "domestic_equity") {
            fakeCredentials = {{"app_key", "FAKE_RETIRED_APP_KEY_ONLY"}, {"app_secret", "FAKE_RETIRED_APP_SECRET_ONLY"}};
            if (provider == "kis") {
                fakeCredentials["account_number"] = "12345678"; fakeCredentials["product_code"] = "01";
            }
        }
        if (AccountModel::isProviderSuspended(provider)) {
            QVERIFY(test::keepSingleSuspendedBroker(vault, provider, Password));
        } else {
            QVERIFY(vault.create(Password));
            QVERIFY(vault.addAccount(account, fakeCredentials, StorageMode::EncryptedVault));
        }
        const auto id = vault.accounts().first().id;
        vault.lock(); QVERIFY(retireSyntheticAccount(path, environment));
        QVERIFY(vault.unlock(Password)); QCOMPARE(vault.accounts().first().environment, environment);
        RecordingTransport transport;
        if (market == "spot") {
            TradingController controller(vault, &transport);
            QVERIFY(!controller.selectAccount(id));
            QVERIFY(!controller.supported());
            QCOMPARE(controller.account().environment, environment);
            QVERIFY(controller.status().contains("지원"));
            QVERIFY(!controller.refreshAssets());
            controller.loadMarkets(); controller.refresh("BTCUSDT");
            QVERIFY(!controller.busy());
        } else if (market == "usdm_futures") {
            FuturesTradingController controller(vault, &transport);
            QVERIFY(!controller.selectAccount(id));
            QVERIFY(!controller.supported());
            QVERIFY(!controller.refreshAssets());
            controller.loadMarkets(); controller.refresh("BTCUSDT");
            QVERIFY(!controller.busy());
        } else {
            BrokerTradingController controller(vault, &transport);
            QVERIFY(!controller.selectConnection(id));
            QVERIFY(!controller.supported());
            QVERIFY(!controller.refreshAssets());
            controller.authenticate(); controller.loadMarkets(); controller.refresh("005930");
            QVERIFY(!controller.busy());
        }
        QCoreApplication::processEvents();
        QVERIFY(transport.sent.isEmpty());
    }
};

QTEST_MAIN(RetiredConnectionTests)
#include "tst_retired_connections.moc"
