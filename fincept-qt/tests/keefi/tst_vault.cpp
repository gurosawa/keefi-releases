#include "fixtures/SuspendedBrokerVault.h"
#include "keefi/AccountModel.h"
#include "keefi/SecureBuffer.h"
#include "keefi/Vault.h"
#include "keefi/OkxProtocol.h"

#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtEndian>
#include <sodium.h>
#include <cstring>
#include <functional>
#include <atomic>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace keefi {
namespace {
const QString Password = QStringLiteral("Correct horse test vault 2026!");
const QString NewPassword = QStringLiteral("Different test vault password 2026!");
const QString LegacyWeakPassword = QStringLiteral("legacy weak passphrase");
// Accepted by earlier builds; new passwords are now printable ASCII only.
const QString LegacyHangulPassword = QStringLiteral("한글 금고 Passphrase1!");
const QString FakeKey = QStringLiteral("KEEFI_FAKE_KEY_ONLY_8f14e45f");
const QString FakeSecret = QStringLiteral("KEEFI_FAKE_SECRET_ONLY_a3bf817a");

AccountRecord record() {
    AccountRecord r;
    r.provider = "upbit";
    r.alias = QStringLiteral("가짜 계좌");
    r.environment = "live";
    r.market = "spot";
    r.currency = "KRW";
    return r;
}

QMap<QString, QString> credentials() { return {{"access_key", FakeKey}, {"secret", FakeSecret}}; }

QJsonObject pendingOrder(const QString& symbol) {
    // Fixed fake reconciliation metadata; no exchange request is made.
    return {{"id", "fx-utf8-journal"}, {"accountId", "fake-binance-account"}, {"fingerprint", QString(64, 'a')},
        {"provider", "binance"}, {"environment", "testnet"}, {"symbol", symbol}, {"kind", "place"},
        {"clientId", "fake-utf8-client"}, {"orderId", ""}, {"replacementClientId", ""},
        {"state", "unknown"}, {"createdAt", "1792200000000"}, {"quantity", "0.1"}, {"price", "100"},
        {"quoteAmount", ""}, {"side", "buy"}, {"type", "limit"}};
}

QJsonObject tossPendingOrder() {
    auto result = pendingOrder(QStringLiteral("005930"));
    result["id"] = "fx-toss-journal";
    result["accountId"] = "fake-toss-local-connection";
    result["provider"] = "toss";
    result["environment"] = "live";
    result["kind"] = "amend";
    result["clientId"] = "";
    result["orderId"] = "fake-original-order";
    result["quantity"] = "1";
    result["price"] = "70000";
    result["market"] = "domestic_equity";
    result["accountScope"] = QString(64, 'b');
    result["operationOrderId"] = "fake-operation-order";
    return result;
}
QJsonObject kiwoomPendingOrder() {
    auto result = tossPendingOrder();
    result["id"] = "fx-kiwoom-journal"; result["provider"] = "kiwoom";
    result["accountId"] = "fake-kiwoom-local-connection";
    result["orderId"] = "W1_20261017_KRX_0000140";
    result["operationOrderId"] = "W1_20261017_KRX_0000141";
    result["baseOriginalOrderId"] = "W1_20261017_KRX_0000139";
    result["baseOriginalProvenance"] = "broker_receipt";
    result["quantityMode"] = "all_remaining"; result["quantity"] = "8";
    return result;
}

QJsonObject lsPendingOrder() {
    auto result = kiwoomPendingOrder();
    result["id"] = "fx-ls-journal"; result["provider"] = "ls"; result["accountId"] = "fake-ls-connection";
    result["orderId"] = "L1_20261019_KRX_0000000140";
    result["operationOrderId"] = "L1_20261019_KRX_0000000141";
    result["baseOriginalOrderId"] = "L1_20261019_KRX_0000000139";
    result["quantityMode"] = "exact";
    return result;
}

QJsonObject okxPendingOrder() {
    auto result = pendingOrder("BTC-USDT");
    result["id"] = "fx-okx-place"; result["accountId"] = "fake-okx-connection";
    result["provider"] = "okx"; result["environment"] = "live";
    result["clientId"] = QString(32, 'a'); result["market"] = "spot"; result["region"] = "global";
    result["accountScope"] = okxAccountScope("global", "live", "123456789");
    result["amendRequestId"] = ""; result["quantityUnit"] = "base"; result["ackState"] = "not_received";
    for (const auto* field : {"beforePrice", "beforeQuantity", "beforeExecuted", "beforeUpdatedAt"}) result[field] = "";
    return result;
}
AccountRecord okxVaultAccount() {
    auto r = record(); r.provider = "okx"; r.environment = "live"; r.currency = "USDT"; r.region = "global"; return r;
}
QMap<QString, QString> okxVaultCredentials() {
    return {{"api_key", FakeKey}, {"secret", FakeSecret}, {"passphrase", "FAKE_OKX_PASSPHRASE_ONLY"}};
}

AccountRecord binanceRecord(QString market="spot", QString environment="live") {
    auto r=record();r.provider="binance";r.currency="USDT";r.market=market;r.environment=environment;return r;
}
QMap<QString,QString> binanceCredentials(){return {{"api_key",FakeKey},{"secret",FakeSecret}};}
QJsonObject futuresPendingOrder() {
    auto result=pendingOrder("BTCUSDT");result["id"]="fx-futures-journal";result["environment"]="live";
    result["market"]="usdm_futures";result["credentialRef"]="00000000-0000-4000-8000-000000000001";
    result["orderFamily"]="ordinary";result["quantity"]="3";
    result["futures"]=QJsonObject{{"positionSide","BOTH"},{"exposure","open"},{"reduceOnly",false},{"closePosition",false},
        {"timeInForce","GTC"},{"workingType",""},{"triggerPrice",""},{"activationPrice",""},{"callbackRate",""},{"modifyId",""}};
    result["before"]=QJsonObject{{"price",""},{"quantity",""},{"executed",""},{"updatedAt",""}};
    result["algoId"]="";result["clientAlgoId"]="";result["actualOrderId"]="";return result;
}

QByteArray read(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size() && f.flush();
}

bool equalSecret(const CredentialSet& fields, const QString& name, const QString& expected) {
    const auto it = fields.find(name);
    if (it == fields.end()) return false;
    const QByteArray bytes = expected.toUtf8(); // All callers use fixed fake secrets.
    return it->second.size() == static_cast<std::size_t>(bytes.size()) &&
        sodium_memcmp(it->second.data(), bytes.constData(), it->second.size()) == 0;
}

// Only fixed fake test files enter this helper. Re-authenticate changed metadata
// to exercise version/schema validation after successful AEAD authentication.
QByteArray rewriteFakeMetadata(QByteArray file, const std::function<void(QJsonObject&)>& mutate,
                               const QString& outputPassword = Password, quint32* frameCount = nullptr) {
    const auto* raw = reinterpret_cast<const unsigned char*>(file.constData());
    SecureBuffer key(32);
    const QByteArray password = Password.toUtf8();
    if (crypto_pwhash(key.data(), key.size(), password.constData(), password.size(), raw + 28,
                     qFromLittleEndian<quint32>(raw + 16), qFromLittleEndian<quint64>(raw + 20),
                     crypto_pwhash_ALG_ARGON2ID13) != 0) return {};
    SecureBuffer plain(file.size() - Vault::HeaderBytes - 16);
    unsigned long long plainSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &plainSize, nullptr,
        raw + Vault::HeaderBytes, file.size() - Vault::HeaderBytes, raw, Vault::HeaderBytes,
        raw + 44, key.data()) != 0) return {};
    const quint32 jsonLength = qFromLittleEndian<quint32>(plain.data() + 4);
    if(frameCount)*frameCount=qFromLittleEndian<quint32>(plain.data()+8+jsonLength);
    auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(plain.data() + 8), jsonLength)).object();
    const int originalSchema=json.value("schema_version").toInt();
    mutate(json);
    if(originalSchema>=6&&json.value("schema_version").toInt()<6){
        // Fixtures here have one frame per account; remove schema6 references
        // when intentionally generating a historical authenticated payload.
        json.remove("credential_sets");auto accounts=json.value("accounts").toArray();
        for(int i=0;i<accounts.size();++i){auto row=accounts[i].toObject();row.remove("credentialRef");accounts[i]=row;}
        json["accounts"]=accounts;
    }
    const QByteArray modified = QJsonDocument(json).toJson(QJsonDocument::Compact);
    std::size_t tailSize=plain.size()-8-jsonLength;
    if(originalSchema>=7&&json.value("schema_version").toInt()<7){
        // Vault tests use empty ledger keys; schema7 adds its explicit zero-length frame.
        if(tailSize<4||qFromLittleEndian<quint32>(plain.data()+plain.size()-4)!=0)return {};
        tailSize-=4;
    }
    SecureBuffer changed(8+modified.size()+tailSize);
    std::memcpy(changed.data(), plain.data(), 4);
    qToLittleEndian<quint32>(modified.size(), changed.data() + 4);
    std::memcpy(changed.data() + 8, modified.constData(), modified.size());
    std::memcpy(changed.data() + 8 + modified.size(), plain.data() + 8 + jsonLength,
                tailSize);
    file.resize(Vault::HeaderBytes + changed.size() + 16);
    auto* out = reinterpret_cast<unsigned char*>(file.data());
    randombytes_buf(out + 44, 24);
    qToLittleEndian<quint32>(changed.size() + 16, out + 68);
    if (outputPassword != Password) {
        // Create a pre-policy fixture with an authenticated schema-1 payload.
        // This bypasses only test fixture construction, never production create().
        const QByteArray weakPassword = outputPassword.toUtf8();
        if (crypto_pwhash(key.data(), key.size(), weakPassword.constData(), weakPassword.size(), out + 28,
            qFromLittleEndian<quint32>(out + 16), qFromLittleEndian<quint64>(out + 20),
            crypto_pwhash_ALG_ARGON2ID13) != 0) return {};
    }
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(out + Vault::HeaderBytes, &length,
        changed.data(), changed.size(), out, Vault::HeaderBytes, nullptr, out + 44, key.data()) != 0) return {};
    return file;
}

QByteArray invalidSchema(QByteArray file, bool legacy = false, const QString& legacyPassword = Password) {
    return rewriteFakeMetadata(std::move(file), [legacy](QJsonObject& json) {
        if (legacy) { json.remove("pending_operations"); json["schema_version"] = 1; }
        else json.insert("unexpected", true);
    }, legacy ? legacyPassword : Password);
}
} // namespace

class VaultTests final : public QObject {
    Q_OBJECT
  private slots:
    void restoreBackupKeepsCiphertextAndUnlockedContents() {
        QTemporaryDir sourceDir, destinationDir;
        const QString sourcePath = sourceDir.filePath("backup.keefi");
        const QString targetPath = destinationDir.filePath("vault.bin");
        Vault source(sourcePath);
        QString error;
        QVERIFY2(source.create(Password, &error), qPrintable(error));
        QVERIFY2(source.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error), qPrintable(error));
        const QJsonArray pending{pendingOrder("BTCUSDT")};
        QVERIFY2(source.setPendingOperations(pending, &error), qPrintable(error));
        QByteArray ledgerKey;
        QVERIFY(source.withAssetsLedgerKey(true, [&](const SecureBuffer& key) {
            // Random key belongs only to this synthetic temporary vault.
            ledgerKey = QByteArray(reinterpret_cast<const char*>(key.data()), key.size());
        }, &error));
        const auto expectedAccounts = source.accounts();
        source.lock();
        const auto original = read(sourcePath);
        const auto sourcePermissions = QFile::permissions(sourcePath);
        QVERIFY(!original.isEmpty());
        QVERIFY(write(sourceDir.filePath("assets.keefi"), "SYNTHETIC SOURCE LEDGER"));
        QVERIFY(write(destinationDir.filePath("assets.keefi"), "SYNTHETIC TARGET LEDGER"));
        const auto sourceFiles = QDir(sourceDir.path()).entryList(QDir::Files | QDir::Hidden);
        Vault restored(targetPath);
        QSignalSpy changed(&restored, &Vault::accountsChanged);
        QVERIFY2(restored.restoreBackup(sourcePath, Password, &error), qPrintable(error));
        QVERIFY(restored.isUnlocked());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(restored.accounts().size(), 1);
        QCOMPARE(restored.accounts().front().id, expectedAccounts.front().id);
        QCOMPARE(restored.accounts().front().alias, expectedAccounts.front().alias);
        QCOMPARE(restored.pendingOperations(), pending);
        bool matched = false;
        QVERIFY(restored.withCredentials(expectedAccounts.front().id, [&](const CredentialSet& fields) {
            matched = equalSecret(fields, "access_key", FakeKey) && equalSecret(fields, "secret", FakeSecret);
        }, &error));
        QVERIFY(matched);
        QVERIFY(restored.withAssetsLedgerKey(false, [&](const SecureBuffer& key) {
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(key.data()), key.size()), ledgerKey);
        }, &error));
        QCOMPARE(read(targetPath), original);
        QCOMPARE(read(sourcePath), original);
        QCOMPARE(QFile::permissions(sourcePath), sourcePermissions);
        QCOMPARE(QDir(sourceDir.path()).entryList(QDir::Files | QDir::Hidden), sourceFiles);
        QVERIFY(!QFileInfo::exists(sourcePath + ".lock"));
        QCOMPARE(read(sourceDir.filePath("assets.keefi")), QByteArray("SYNTHETIC SOURCE LEDGER"));
        QCOMPARE(read(destinationDir.filePath("assets.keefi")), QByteArray("SYNTHETIC TARGET LEDGER"));
        QVERIFY(!(QFile::permissions(targetPath) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
        Vault competing(targetPath);
        QVERIFY(!competing.unlock(Password, &error));
        restored.lock();
        QVERIFY2(competing.unlock(Password, &error), qPrintable(error));
        QCOMPARE(competing.accounts().size(), 1);
        QCOMPARE(competing.pendingOperations(), pending);
        QCOMPARE(read(targetPath), original);
    }

    void restoreBackupPreservesLegacyPasswords_data() {
        QTest::addColumn<QString>("password");
        QTest::newRow("legacy-weak") << LegacyWeakPassword;
        QTest::newRow("legacy-unicode") << LegacyHangulPassword;
    }

    void restoreBackupPreservesLegacyPasswords() {
        QFETCH(QString, password);
        QTemporaryDir sourceDir, destinationDir;
        Vault source(sourceDir.filePath("legacy.keefi"));
        QVERIFY(source.create(Password));
        QVERIFY(source.addAccount(record(), credentials(), StorageMode::EncryptedVault));
        source.lock();
        const auto legacy = invalidSchema(read(source.filePath()), true, password);
        QVERIFY(!legacy.isEmpty());
        QVERIFY(write(source.filePath(), legacy));
        QVERIFY(!Vault::validateNewPassword(password));
        Vault restored(destinationDir.filePath("vault.bin"));
        QString error;
        QVERIFY2(restored.restoreBackup(source.filePath(), password, &error), qPrintable(error));
        QVERIFY(restored.isUnlocked());
        QCOMPARE(restored.accounts().size(), 1);
        QCOMPARE(read(restored.filePath()), legacy);
        restored.lock();
        QVERIFY2(restored.unlock(password, &error), qPrintable(error));
        QCOMPARE(read(restored.filePath()), legacy);
        QCOMPARE(read(source.filePath()), legacy);
        QVERIFY(!QFileInfo::exists(source.filePath() + ".lock"));
    }

    void restoreBackupRejectsInvalidSourceBeforeCreatingDestination_data() {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"password", "header", "authentication", "schema", "truncated", "oversize", "missing"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void restoreBackupRejectsInvalidSourceBeforeCreatingDestination() {
        QFETCH(QString, kind);
        QTemporaryDir sourceDir, destinationDir;
        Vault source(sourceDir.filePath("backup.keefi"));
        QVERIFY(source.create(Password));
        source.lock();
        QByteArray bytes = read(source.filePath());
        QString password = Password;
        if (kind == "password") password = QStringLiteral("Wrong synthetic password 2026!");
        else if (kind == "header") bytes[0] ^= 1;
        else if (kind == "authentication") bytes[bytes.size() - 1] ^= 1;
        else if (kind == "schema") bytes = invalidSchema(bytes);
        else if (kind == "truncated") bytes.resize(Vault::HeaderBytes);
        else if (kind == "oversize") bytes = QByteArray(Vault::MaxFileBytes + 1, 'x');
        if (kind == "missing") QVERIFY(QFile::remove(source.filePath()));
        else QVERIFY(write(source.filePath(), bytes));
        Vault restored(destinationDir.filePath("new-profile/vault.bin"));
        QString error;
        QVERIFY(!restored.restoreBackup(source.filePath(), password, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!restored.isUnlocked());
        QVERIFY(restored.accounts().isEmpty());
        QVERIFY(restored.pendingOperations().isEmpty());
        QVERIFY(!QFileInfo::exists(destinationDir.filePath("new-profile")));
        QVERIFY(!QFileInfo::exists(source.filePath() + ".lock"));
        if (kind != "missing") QCOMPARE(read(source.filePath()), bytes);
        QVERIFY(!error.contains(password));
        QVERIFY(!error.contains(FakeKey));
        QVERIFY(!error.contains(FakeSecret));
    }

    void restoreBackupDoesNotOverwriteExistingDestinationOrLock() {
        QTemporaryDir sourceDir, destinationDir;
        Vault source(sourceDir.filePath("backup.keefi"));
        QVERIFY(source.create(Password));
        source.lock();
        const auto original = read(source.filePath());
        Vault restored(destinationDir.filePath("vault.bin"));
        const QByteArray sentinel("SYNTHETIC EXISTING FILE");
        QVERIFY(write(restored.filePath(), sentinel));
        const auto permissions = QFile::permissions(restored.filePath());
        QString error;
        QVERIFY(!restored.restoreBackup(source.filePath(), Password, &error));
        QCOMPARE(read(restored.filePath()), sentinel);
        QCOMPARE(QFile::permissions(restored.filePath()), permissions);
        QVERIFY(!QFileInfo::exists(restored.filePath() + ".lock"));
        QVERIFY(QFile::remove(restored.filePath()));
        QLockFile other(restored.filePath() + ".lock");
        other.setStaleLockTime(0);
        QVERIFY(other.tryLock());
        const auto lockBytes = read(restored.filePath() + ".lock");
        QVERIFY(!restored.restoreBackup(source.filePath(), Password, &error));
        QVERIFY(other.isLocked());
        QCOMPARE(read(restored.filePath() + ".lock"), lockBytes);
        QVERIFY(!restored.exists());
        other.unlock();
        QVERIFY2(restored.restoreBackup(source.filePath(), Password, &error), qPrintable(error));
        // Re-entering restore must preserve the already open session and file.
        const auto targetBytes = read(restored.filePath());
        QVERIFY(!restored.restoreBackup(source.filePath(), Password, &error));
        QVERIFY(restored.isUnlocked());
        QCOMPARE(read(restored.filePath()), targetBytes);
        QCOMPARE(read(source.filePath()), original);
    }

    void restoreBackupNeverWritesItsSourceAndRefusesUnsafePaths() {
#ifdef Q_OS_UNIX
        QTemporaryDir sourceDir, destinationDir;
        Vault source(sourceDir.filePath("backup.keefi"));
        QVERIFY(source.create(Password));
        source.lock();
        const auto original = read(source.filePath());
        QVERIFY(QFile::setPermissions(source.filePath(), QFile::ReadOwner));
        QVERIFY(QFile::setPermissions(sourceDir.path(), QFile::ReadOwner | QFile::ExeOwner));
        Vault restored(destinationDir.filePath("vault.bin"));
        QString error;
        const bool result = restored.restoreBackup(source.filePath(), Password, &error);
        const auto after = read(source.filePath());
        const auto sourcePermissions = QFile::permissions(source.filePath());
        const bool sourceLockExists = QFileInfo::exists(source.filePath() + ".lock");
        // Always restore only our synthetic directory's permissions for cleanup.
        QVERIFY(QFile::setPermissions(sourceDir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(after, original);
        QVERIFY(!(sourcePermissions & (QFile::WriteOwner | QFile::WriteGroup | QFile::WriteOther)));
        QVERIFY(!sourceLockExists);
        restored.lock();
        QVERIFY(QFile::remove(restored.filePath()));
        const QString sourceLink = sourceDir.filePath("linked.keefi");
        QVERIFY(QFile::link(source.filePath(), sourceLink));
        QVERIFY(!restored.restoreBackup(sourceLink, Password, &error));
        QVERIFY(!restored.exists());
        QVERIFY(QFile::setPermissions(source.filePath(), QFile::ReadOwner | QFile::ReadOther));
        const auto broadPermissions = QFile::permissions(source.filePath());
        QVERIFY(!restored.restoreBackup(source.filePath(), Password, &error));
        QCOMPARE(QFile::permissions(source.filePath()), broadPermissions);
        QVERIFY(!restored.exists());
        QVERIFY(QFile::setPermissions(source.filePath(), QFile::ReadOwner | QFile::WriteOwner));
        // A dangling destination symlink is still an existing target.
        QVERIFY(QFile::link(destinationDir.filePath("missing.keefi"), restored.filePath()));
        QVERIFY(!restored.restoreBackup(source.filePath(), Password, &error));
        QVERIFY(QFileInfo(restored.filePath()).isSymLink());
        QVERIFY(!QFileInfo::exists(destinationDir.filePath("missing.keefi")));
        QCOMPARE(read(source.filePath()), original);
#else
        QSKIP("Unix source permissions and symbolic links; Windows DACL/reparse behavior needs Windows validation.");
#endif
    }

    void restoreBackupWriteFailureLeavesSourceAndDirectoryContentsIntact() {
#ifdef Q_OS_UNIX
        QTemporaryDir sourceDir, destinationDir;
        Vault source(sourceDir.filePath("backup.keefi"));
        QVERIFY(source.create(Password));
        source.lock();
        const auto original = read(source.filePath());
        QVERIFY(write(destinationDir.filePath("assets.keefi"), "SYNTHETIC UNRELATED LEDGER"));
        const auto files = QDir(destinationDir.path()).entryList(QDir::Files | QDir::Hidden);
        QVERIFY(QFile::setPermissions(destinationDir.path(), QFile::ReadOwner | QFile::ExeOwner));
        Vault restored(destinationDir.filePath("vault.bin"));
        QString error;
        const bool result = restored.restoreBackup(source.filePath(), Password, &error);
        QVERIFY(QFile::setPermissions(destinationDir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(!result);
        QVERIFY(!error.isEmpty());
        QVERIFY(!restored.exists());
        QVERIFY(!restored.isUnlocked());
        QCOMPARE(QDir(destinationDir.path()).entryList(QDir::Files | QDir::Hidden), files);
        QCOMPARE(read(destinationDir.filePath("assets.keefi")), QByteArray("SYNTHETIC UNRELATED LEDGER"));
        QCOMPARE(read(source.filePath()), original);
#else
        QSKIP("Unix directory write-permission fault; Windows needs native permission-fault validation.");
#endif
    }

    void restoreBackupAndConcurrentCreatorNeverReplaceEachOther() {
        QTemporaryDir sourceDir;
        Vault source(sourceDir.filePath("backup.keefi"));
        QVERIFY(source.create(Password));
        source.lock();
        const auto original = read(source.filePath());
        const QByteArray sentinel("SYNTHETIC CONCURRENT CREATOR");
        int creatorWins = 0, restoreWins = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            QTemporaryDir destinationDir;
            Vault restored(destinationDir.filePath("vault.bin"));
            const QString targetPath = restored.filePath();
            std::atomic<bool> ready{false}, finished{false};
            bool created = false, written = false;
            std::thread creator([&] {
                ready.store(true);
                // Wait for the destination lock: restore has already checked
                // initial absence and authenticated the source at this point.
                while (!finished.load() && !QFileInfo::exists(targetPath + ".lock"))
                    std::this_thread::yield();
                QFile competing(targetPath);
                created = competing.open(QIODevice::WriteOnly | QIODevice::NewOnly);
                if (created) written = competing.write(sentinel) == sentinel.size() && competing.flush();
            });
            while (!ready.load()) std::this_thread::yield();
            QString error;
            const bool result = restored.restoreBackup(source.filePath(), Password, &error);
            finished.store(true);
            creator.join();
            if (created) {
                ++creatorWins;
                QVERIFY(written);
                QVERIFY(!result);
                QVERIFY(!restored.isUnlocked());
                QCOMPARE(read(targetPath), sentinel);
                QVERIFY(!QFileInfo::exists(targetPath + ".lock"));
            } else {
                ++restoreWins;
                QVERIFY2(result, qPrintable(error));
                QVERIFY(restored.isUnlocked());
                QCOMPARE(read(targetPath), original);
            }
            QVERIFY(QDir(destinationDir.path()).entryList({".keefi-restore-*"}, QDir::Files | QDir::Hidden).isEmpty());
            QCOMPARE(read(source.filePath()), original);
        }
        qInfo("Concurrent creation outcomes: creator=%d restore=%d; both legal winners preserve exact bytes.", creatorWins, restoreWins);
    }

    void retiredPaperAndTestnetConnectionsCannotBeAdded() {
        // 2026-10-21: KEEFI keeps live (Solana mainnet) connections only.
        QTemporaryDir dir; Vault vault(dir.filePath("retired.vault")); QVERIFY(vault.create(Password));
        auto kis = record(); kis.provider = "kis"; kis.market = "domestic_equity"; kis.currency = "KRW"; kis.environment = "paper";
        auto kiwoom = kis; kiwoom.provider = "kiwoom"; auto ls = kis; ls.provider = "ls";
        auto okx = okxVaultAccount(); okx.environment = "demo";
        const auto withId = [](AccountRecord r) { r.id = QUuid::createUuid().toString(QUuid::WithoutBraces); return r; };
        for (auto live : {binanceRecord("spot", "live"), binanceRecord("usdm_futures", "live")}) QVERIFY(AccountModel::validate(withId(live)));
        for (auto retired : {binanceRecord("spot", "testnet"), binanceRecord("usdm_futures", "demo"), okx, kis, kiwoom, ls}) {
            QVERIFY2(!AccountModel::validate(withId(retired)), qPrintable(retired.provider + " " + retired.environment));
            auto live = retired; live.environment = "live"; QVERIFY2(AccountModel::validate(withId(live)), qPrintable(retired.provider)); // Only the environment differs.
        }
        QVERIFY(!vault.addAccount(binanceRecord("spot", "testnet"), binanceCredentials(), StorageMode::EncryptedVault));
        QVERIFY(!vault.addAccount(binanceRecord("usdm_futures", "demo"), binanceCredentials(), StorageMode::EncryptedVault));
        QVERIFY(!vault.addAccount(okx, okxVaultCredentials(), StorageMode::EncryptedVault));
        QVERIFY(vault.accounts().isEmpty());
        // An older vault that still lists such a record must keep opening, so reading accepts it.
        AccountRecord legacy; QVERIFY(AccountModel::fromJson(AccountModel::toJson(withId(okx)), &legacy));
        QCOMPARE(legacy.environment, QStringLiteral("demo"));
    }
    void retiredConnectionsRemainReadableSavableAndRemovable_data() {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("market");
        QTest::addColumn<QString>("environment");
        QTest::newRow("binance-spot-testnet") << QString("binance") << QString("spot") << QString("testnet");
        QTest::newRow("binance-futures-demo") << QString("binance") << QString("usdm_futures") << QString("demo");
        QTest::newRow("okx-demo") << QString("okx") << QString("spot") << QString("demo");
        for (const auto& provider : {QString("kis"), QString("kiwoom"), QString("ls")})
            QTest::newRow(qPrintable(provider + "-paper")) << provider << QString("domestic_equity") << QString("paper");
    }
    void retiredConnectionsRemainReadableSavableAndRemovable() {
        QFETCH(QString, provider); QFETCH(QString, market); QFETCH(QString, environment);
        QTemporaryDir dir; const auto path = dir.filePath("retired-synthetic.bin");
        Vault vault(path);
        auto account = binanceRecord(market);
        account.provider = provider;
        if (provider == "okx") account.region = "global";
        QMap<QString, QString> fakeCredentials = binanceCredentials();
        if (market == "domestic_equity") {
            account.currency = "KRW";
            fakeCredentials = {{"app_key", FakeKey}, {"app_secret", FakeSecret}};
            if (provider == "kis") {
                fakeCredentials["account_number"] = "12345678";
                fakeCredentials["product_code"] = "01";
            }
        } else if (provider == "okx") fakeCredentials = okxVaultCredentials();
        if (AccountModel::isProviderSuspended(provider)) {
            QVERIFY(test::keepSingleSuspendedBroker(vault, provider, Password));
        } else {
            QVERIFY(vault.create(Password));
            QVERIFY(vault.addAccount(account, fakeCredentials, StorageMode::EncryptedVault));
        }
        const auto legacyId = vault.accounts().first().id;
        vault.lock();
        const auto historical = rewriteFakeMetadata(read(path), [&](QJsonObject& root) {
            auto accounts = root["accounts"].toArray(); auto row = accounts[0].toObject();
            row["environment"] = environment; accounts[0] = row; root["accounts"] = accounts;
            auto sets = root["credential_sets"].toArray(); auto set = sets[0].toObject();
            set["environment"] = environment; sets[0] = set; root["credential_sets"] = sets;
        });
        QVERIFY(!historical.isEmpty()); QVERIFY(write(path, historical));
        QVERIFY(vault.unlock(Password)); QCOMPARE(read(path), historical);
        QCOMPARE(vault.accounts().first().environment, environment);
        QVERIFY(vault.addAccount(record(), credentials(), StorageMode::EncryptedVault));
        const auto addedId = vault.accounts().last().id;
        vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.accounts().size(), 2);
        QCOMPARE(vault.accounts().first().id, legacyId);
        QCOMPARE(vault.accounts().first().environment, environment);
        QVERIFY(vault.removeAccount(addedId));
        vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.accounts().size(), 1);
        const auto beforeReplacement = read(path);
        QVERIFY(!vault.replaceCredentials(legacyId, fakeCredentials));
        QCOMPARE(read(path), beforeReplacement); QVERIFY(vault.isUnlocked());
        QVERIFY(vault.removeAccount(legacyId));
        vault.lock(); QVERIFY(vault.unlock(Password)); QVERIFY(vault.accounts().isEmpty());
    }
    void eraseRemovesOnlyThisVaultAndItsRecordsWithoutPassword() {
        QTemporaryDir dir; const QString path = dir.filePath("credentials.keefi-vault");
        Vault vault(path); QVERIFY(vault.create(Password));
        AccountRecord a; a.provider = "binance"; a.alias = "FAKE ERASE"; a.environment = "live"; a.market = "spot"; a.currency = "USDT";
        QVERIFY(vault.addAccount(a, {{"api_key", "FAKEAPIKEY123456789"}, {"secret", "FAKESECRET123456789"}}, StorageMode::EncryptedVault));
        const QStringList keyed{"assets.keefi", "assets-current.keefi", "assets.keefi.archive-0001.keefi", "swaps.keefi", "imported-records.keefi", "prices.keefi",
                                "prices.keefi.unreadable-1792598400000", ".keefi-prices-AbC123", "prices.keefi.exit"};
        const QStringList kept{"instance.lock", "KEEFI.ini", "notes.txt"};
        for (const auto& name : keyed + kept) { QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("fake"); }
        vault.lock();
        QCOMPARE(Vault::erasedFileNames(path).size(), keyed.size() + 1);
        // A forgotten password is the point: erase works on a locked vault.
        QSignalSpy state(&vault, &Vault::stateChanged);
        QString error; QVERIFY2(vault.erase(&error), qPrintable(error));
        QVERIFY(!vault.exists()); QVERIFY(!vault.isUnlocked()); QVERIFY(vault.accounts().isEmpty()); QVERIFY(state.count() >= 1);
        for (const auto& name : keyed) QVERIFY2(!QFileInfo::exists(dir.filePath(name)), qPrintable(name));
        for (const auto& name : kept) QVERIFY2(QFileInfo::exists(dir.filePath(name)), qPrintable(name));
        QVERIFY(Vault::erasedFileNames(path).isEmpty());
        QVERIFY(vault.erase(&error)); // Nothing left: still succeeds.
        QVERIFY(vault.create(NewPassword)); QVERIFY(vault.accounts().isEmpty());
    }
    void sessionOnlyConnectionsAreRejected() {
        QTemporaryDir dir; const QString path = dir.filePath("fake-no-session.bin");
        Vault vault(path); QString error;
        // Without a vault there is no in-memory-only connection mode.
        QVERIFY(!vault.addAccount(record(), credentials(), StorageMode::SessionOnly, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error));
        QVERIFY(vault.accounts().isEmpty()); QVERIFY(!vault.exists()); QVERIFY(!QFileInfo::exists(path + ".lock"));
        QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binanceRecord(), binanceCredentials(), StorageMode::EncryptedVault));
        const auto source = vault.accounts().first(); const auto initial = read(path);
        QVERIFY(!vault.addAccount(record(), credentials(), StorageMode::SessionOnly, &error));
        auto shared = binanceRecord("usdm_futures"); shared.storageMode = StorageMode::SessionOnly;
        QVERIFY(!vault.addAccountUsingCredentials(shared, source.id));
        QCOMPARE(vault.accounts().size(), 1); QCOMPARE(read(path), initial);
        // Locking keeps saved keys only in the encrypted file and clears memory.
        vault.lock(); QVERIFY(vault.accounts().isEmpty()); QVERIFY(vault.exists());
        QVERIFY(!vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error)); QCOMPARE(read(path), initial);
        QVERIFY(vault.unlock(Password)); QCOMPARE(vault.accounts().size(), 1);
        QCOMPARE(vault.accounts().first().id, source.id); QCOMPARE(vault.accounts().first().storageMode, StorageMode::EncryptedVault);
    }
    void sharedSchemaSixReadsLegacyWithoutWritingThenMigrates() {
        QTemporaryDir source;const auto path=source.filePath("source.bin");QByteArray original;QStringList ids,refs;
        { Vault vault(path);QVERIFY(vault.create(Password));
          QVERIFY(vault.addAccount(binanceRecord(),binanceCredentials(),StorageMode::EncryptedVault));
          QVERIFY(vault.addAccount(binanceRecord("usdm_futures"),binanceCredentials(),StorageMode::EncryptedVault));
          QVERIFY(vault.setPendingOperations({pendingOrder("BTCUSDT")}));
          for(const auto& r:vault.accounts()){ids.append(r.id);refs.append(r.credentialRef);}original=read(path); }
        for(int version:{1,2,3,4,5,6}){
            QTemporaryDir dir;const auto legacyPath=dir.filePath("legacy.bin");
            const auto legacy=rewriteFakeMetadata(original,[&](QJsonObject& root){
                root["schema_version"]=version;if(version==1)root.remove("pending_operations");
                auto rows=root["accounts"].toArray();auto r=rows[1].toObject();r["environment"]="testnet";rows[1]=r;root["accounts"]=rows;
                if(version==6){auto sets=root["credential_sets"].toArray();auto set=sets[1].toObject();set["environment"]="testnet";sets[1]=set;root["credential_sets"]=sets;}
            });
            QVERIFY(write(legacyPath,legacy));QFile::setPermissions(legacyPath,QFile::ReadOwner|QFile::WriteOwner);
            Vault vault(legacyPath);QVERIFY(vault.unlock(Password));QCOMPARE(read(legacyPath),legacy);
            QCOMPARE(vault.accounts().size(),2);for(int i=0;i<2;++i){QCOMPARE(vault.accounts()[i].id,ids[i]);QCOMPARE(vault.accounts()[i].credentialRef,version<6?ids[i]:refs[i]);}
            QCOMPARE(vault.accounts()[1].environment,QString("testnet"));
            const auto pending=vault.pendingOperations();QVERIFY(vault.setPendingOperations(pending));
            QJsonObject metadata;quint32 frames=0;
            QVERIFY(!rewriteFakeMetadata(read(legacyPath),[&](QJsonObject& root){metadata=root;},Password,&frames).isEmpty());
            QCOMPARE(metadata["schema_version"],QJsonValue(7));QCOMPARE(metadata["credential_sets"].toArray().size(),2);QCOMPARE(frames,quint32(2));
            QCOMPARE(metadata["pending_operations"].toArray(),pending);QCOMPARE(read(legacyPath).left(28),original.left(28));
            vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.accounts()[1].environment,QString("testnet"));
        }
    }
    void sharedCredentialsHaveOneFrameRejectCrossScopeAndSurviveDeletion() {
        QTemporaryDir dir;Vault vault(dir.filePath("shared.bin"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binanceRecord(),binanceCredentials(),StorageMode::EncryptedVault));const auto source=vault.accounts().first();
        QVERIFY(vault.addAccountUsingCredentials(binanceRecord("usdm_futures"),source.id));
        const auto futures=vault.accounts().last();QCOMPARE(source.credentialRef,futures.credentialRef);QCOMPARE(vault.credentialUsers(source.id).size(),2);
        QJsonObject metadata;quint32 frames=0;QVERIFY(!rewriteFakeMetadata(read(vault.filePath()),[&](QJsonObject& root){metadata=root;},Password,&frames).isEmpty());
        QCOMPARE(metadata["accounts"].toArray().size(),2);QCOMPARE(metadata["credential_sets"].toArray().size(),1);QCOMPARE(frames,quint32(1));
        QVERIFY(!vault.addAccountUsingCredentials(binanceRecord("usdm_futures"),source.id));
        auto bad=binanceRecord("usdm_futures","demo");QVERIFY(!vault.addAccountUsingCredentials(bad,source.id));
        bad=binanceRecord("usdm_futures");bad.storageMode=StorageMode::SessionOnly;QVERIFY(!vault.addAccountUsingCredentials(bad,source.id));
        bad=record();QVERIFY(!vault.addAccountUsingCredentials(bad,source.id));
        bad=binanceRecord();bad.credentialRef=source.credentialRef;QVERIFY(!vault.addAccount(bad,binanceCredentials(),StorageMode::EncryptedVault));
        QVERIFY(!vault.addAccount(binanceRecord("spot","demo"),binanceCredentials(),StorageMode::EncryptedVault));
        QVERIFY(!vault.addAccount(binanceRecord("usdm_futures","testnet"),binanceCredentials(),StorageMode::EncryptedVault));
        QVERIFY(vault.setPendingOperations({futuresPendingOrder()}));const auto pending=vault.pendingOperations();
        QVERIFY(vault.removeAccount(source.id));QVERIFY(vault.withCredentials(futures.id,[&](const auto& values){QVERIFY(equalSecret(values,"secret",FakeSecret));}));
        vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.credentialUsers(futures.id).size(),1);
        QVERIFY(vault.removeAccount(futures.id));QCOMPARE(vault.pendingOperations(),pending);
        QVERIFY(!rewriteFakeMetadata(read(vault.filePath()),[&](QJsonObject& root){metadata=root;},Password,&frames).isEmpty());QCOMPARE(frames,quint32(0));
        QCOMPARE(metadata["credential_sets"].toArray().size(),0);
        // Authenticated metadata tampering cannot create cross-provider/dangling references.
        Vault fresh(dir.filePath("malformed.bin"));QVERIFY(fresh.create(Password));QVERIFY(fresh.addAccount(binanceRecord(),binanceCredentials(),StorageMode::EncryptedVault));
        const auto valid=read(fresh.filePath());fresh.lock();
        for(const auto& mutation:QList<std::function<void(QJsonObject&)>>{
            [](QJsonObject& root){auto rows=root["accounts"].toArray();auto r=rows[0].toObject();r["credentialRef"]="00000000-0000-4000-8000-000000000002";rows[0]=r;root["accounts"]=rows;},
            [](QJsonObject& root){auto rows=root["credential_sets"].toArray();auto r=rows[0].toObject();r["provider"]="okx";rows[0]=r;root["credential_sets"]=rows;},
            [](QJsonObject& root){root["accounts"]=QJsonArray{};}}){
            QVERIFY(write(fresh.filePath(),rewriteFakeMetadata(valid,mutation)));QVERIFY(!fresh.unlock(Password));
        }
    }
    void sharedReplacementIsAtomicAndLockClearsMemory() {
        QTemporaryDir dir;Vault vault(dir.filePath("replace-shared.bin"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binanceRecord(),binanceCredentials(),StorageMode::EncryptedVault));const auto id=vault.accounts().first().id;
        QVERIFY(vault.addAccountUsingCredentials(binanceRecord("usdm_futures"),id));const auto users=vault.credentialUsers(id);
        QSignalSpy changed(&vault,&Vault::accountsChanged);auto replacement=binanceCredentials();replacement["secret"]="FAKE_SHARED_REPLACEMENT_ONLY";
        QVERIFY(vault.replaceCredentials(id,replacement));QCOMPARE(changed.size(),1);
        for(const auto& user:users)QVERIFY(vault.withCredentials(user.id,[&](const auto& values){QVERIFY(equalSecret(values,"secret",replacement["secret"]));}));
        // A separate, authenticated rewrite changes the digest before save. No half replacement is committed.
        const auto external=rewriteFakeMetadata(read(vault.filePath()),[](QJsonObject&){});QVERIFY(write(vault.filePath(),external));
        auto rejected=replacement;rejected["secret"]="FAKE_MUST_NOT_COMMIT";QVERIFY(!vault.replaceCredentials(id,rejected));
        QVERIFY(!vault.isUnlocked());QCOMPARE(read(vault.filePath()),external);QVERIFY(vault.unlock(Password));
        for(const auto& user:users)QVERIFY(vault.withCredentials(user.id,[&](const auto& values){QVERIFY(equalSecret(values,"secret",replacement["secret"]));}));
        auto okx=okxVaultAccount();QVERIFY(vault.addAccount(okx,okxVaultCredentials(),StorageMode::EncryptedVault));const auto okxId=vault.accounts().last().id;
        QVERIFY(vault.replaceCredentials(okxId,okxVaultCredentials(),"us"));vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.accounts().last().region,QString("us"));
        vault.lock();QVERIFY(vault.accounts().isEmpty());QVERIFY(vault.exists());
    }
    void futuresTwentyFiveFieldsRemainSeparateAndStrict() {
        QTemporaryDir dir;Vault vault(dir.filePath("futures-journal.bin"));QVERIFY(vault.create(Password));
        auto amend=futuresPendingOrder();amend["id"]="fx-futures-amend";amend["kind"]="amend";amend["orderId"]="123";
        auto terms=amend["futures"].toObject();terms["modifyId"]="9223372036854775807";amend["futures"]=terms;
        amend["before"]=QJsonObject{{"price","100"},{"quantity","10"},{"executed","2"},{"updatedAt","1792200000000"}};
        auto algo=futuresPendingOrder();algo["id"]="fx-futures-algo";algo["kind"]="cancel";algo["orderFamily"]="conditional";
        algo["clientId"]="";algo["algoId"]="123";algo["type"]="stop_market";algo["price"]="";algo["quantity"]="0";
        terms=algo["futures"].toObject();terms["closePosition"]=true;terms["workingType"]="MARK_PRICE";terms["triggerPrice"]="90";terms["exposure"]="unspecified";algo["futures"]=terms;
        const QJsonArray expected{pendingOrder("BTCUSDT"),tossPendingOrder(),kiwoomPendingOrder(),lsPendingOrder(),okxPendingOrder(),futuresPendingOrder(),amend,algo};
        QString error;QVERIFY2(vault.setPendingOperations(expected,&error),qPrintable(error));const auto bytes=read(vault.filePath());vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.pendingOperations(),expected);
        QList<QJsonObject> invalid;auto bad=amend;bad["extra"]="";invalid.append(bad);bad=amend;bad["environment"]="testnet";invalid.append(bad);
        bad=amend;bad["quantity"]="2";invalid.append(bad);bad=amend;terms=bad["futures"].toObject();terms["reduceOnly"]="true";bad["futures"]=terms;invalid.append(bad);
        bad=amend;terms=bad["futures"].toObject();terms["modifyId"]="9223372036854775808";bad["futures"]=terms;invalid.append(bad);
        bad=algo;bad["orderId"]="123";invalid.append(bad);bad=algo;bad["kind"]="place";invalid.append(bad);
        bad=amend;bad["credentialRef"]="deleted-plain-key";invalid.append(bad);
        for(const auto& row:invalid){QVERIFY(!vault.setPendingOperations({row}));QCOMPARE(vault.pendingOperations(),expected);QCOMPARE(read(vault.filePath()),bytes);}
        vault.lock();QVERIFY(write(vault.filePath(),rewriteFakeMetadata(bytes,[](QJsonObject& root){root["schema_version"]=5;})));QVERIFY(!vault.unlock(Password));
    }

    void conditionalLimitJournalRequiresSchemaNineAndKnownTerms() {
        QTemporaryDir dir;Vault vault(dir.filePath("conditional-limit.bin"));QVERIFY(vault.create(Password));
        auto algo=futuresPendingOrder();algo["orderFamily"]="conditional";algo["clientId"]="";algo["clientAlgoId"]="fake-algo-client";algo["type"]="stop";
        auto terms=algo["futures"].toObject();terms["triggerPrice"]="102";terms["workingType"]="MARK_PRICE";terms["priceProtect"]=false;algo["futures"]=terms;
        QVERIFY(vault.setPendingOperations({algo}));const auto bytes=read(vault.filePath());QJsonObject metadata;
        rewriteFakeMetadata(bytes,[&](QJsonObject& root){metadata=root;});QCOMPARE(metadata.value("schema_version"),QJsonValue(9));
        QVERIFY(!bytes.contains("fake-algo-client"));vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.pendingOperations(),QJsonArray{algo});
        for(const auto& field:QStringList{"clientAlgoId","type","price"}){auto bad=algo;bad[field]="";QVERIFY(!vault.setPendingOperations({bad}));QCOMPARE(read(vault.filePath()),bytes);}
        for(const auto& field:QStringList{"triggerPrice","workingType","priceProtect"}){auto bad=algo;auto f=terms;f.remove(field);bad["futures"]=f;QVERIFY(!vault.setPendingOperations({bad}));}
        auto bad=algo;bad["type"]="stop_market";QVERIFY(!vault.setPendingOperations({bad}));
        bad=algo;auto f=terms;f["priceProtect"]=true;bad["futures"]=f;QVERIFY(!vault.setPendingOperations({bad}));
        vault.lock();for(int version:{7,8}){const auto downgraded=rewriteFakeMetadata(bytes,[version](QJsonObject& root){root["schema_version"]=version;});
            QVERIFY(write(vault.filePath(),downgraded));QVERIFY(!vault.unlock(Password));QCOMPARE(read(vault.filePath()),downgraded);}
        QVERIFY(write(vault.filePath(),bytes));QVERIFY(vault.unlock(Password));algo["algoId"]="300";QVERIFY(vault.setPendingOperations({algo}));
        vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.pendingOperations(),QJsonArray{algo});
        QVERIFY(vault.setPendingOperations({futuresPendingOrder()}));rewriteFakeMetadata(read(vault.filePath()),[&](QJsonObject& root){metadata=root;});
        QCOMPARE(metadata.value("schema_version"),QJsonValue(7));
    }

    void bithumbKeepsSeventeenFieldJournalAndRejectsUnsupportedShapes() {
        QTemporaryDir dir; Vault vault(dir.filePath("bithumb-vault.bin")); QVERIFY(vault.create(Password));
        auto entry=pendingOrder("KRW-BTC"); entry["provider"]="bithumb";entry["environment"]="live";
        entry["id"]="fx-bithumb-journal";entry["clientId"]="fake-bithumb-client";
        const QJsonArray expected{pendingOrder("BTCUSDT"),okxPendingOrder(),entry};
        QString error;QVERIFY2(vault.setPendingOperations(expected,&error),qPrintable(error));
        const auto bytes=read(vault.filePath());vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.pendingOperations(),expected);
        for(const auto& changed:QList<QPair<QString,QString>>{{"environment","demo"},{"kind","amend"},{"market","spot"},{"quantity",""}}){
            auto bad=entry;bad[changed.first]=changed.second;QVERIFY(!vault.setPendingOperations({bad}));QCOMPARE(read(vault.filePath()),bytes);
        }
        auto buy=entry;buy["type"]="market";buy["quantity"]="";buy["price"]="";buy["quoteAmount"]="5000";
        QVERIFY(vault.setPendingOperations({buy}));QCOMPARE(vault.pendingOperations().first().toObject().size(),17);
    }

    void okxRegionAndCredentialsReplaceAtomically() {
        QTemporaryDir dir; Vault vault(dir.filePath("okx-region.bin")); QString error;
        QVERIFY(vault.create(Password)); auto account = okxVaultAccount();
        account.region.clear(); QVERIFY(!vault.addAccount(account,okxVaultCredentials(),StorageMode::EncryptedVault));
        account.region = "global"; QVERIFY(vault.addAccount(account,okxVaultCredentials(),StorageMode::EncryptedVault));
        const auto id = vault.accounts().first().id; const auto before = read(vault.filePath());
        auto replacement = okxVaultCredentials(); replacement["secret"] = "FAKE_OKX_REPLACEMENT_ONLY";
        auto missing = replacement; missing.remove("passphrase");
        QVERIFY(!vault.replaceCredentials(id,missing,"us",&error));
        QVERIFY(!vault.replaceCredentials(id,replacement,"",&error));
        QVERIFY(!vault.replaceCredentials(id,replacement,"unknown",&error));
        QCOMPARE(read(vault.filePath()),before); QCOMPARE(vault.accounts().first().region,QString("global"));
        QVERIFY(vault.withCredentials(id,[&](const auto& fields) { QVERIFY(equalSecret(fields,"secret",FakeSecret)); }));
        QVERIFY(vault.replaceCredentials(id,replacement,"us",&error));
        vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.accounts().first().region,QString("us"));
        QVERIFY(vault.withCredentials(id,[&](const auto& fields) { QVERIFY(equalSecret(fields,"secret","FAKE_OKX_REPLACEMENT_ONLY")); }));
        QVERIFY(vault.replaceCredentials(id,okxVaultCredentials(),&error));
        QCOMPARE(vault.accounts().first().region,QString("us"));
        auto other = record(); other.region = "us"; QVERIFY(!AccountModel::validate(other));
        auto metadata = AccountModel::toJson(vault.accounts().first()); QCOMPARE(metadata.size(),12);
        metadata["region"] = ""; AccountRecord parsed; QVERIFY(!AccountModel::fromJson(metadata,&parsed));
    }
    void legacyOkxRegionReadsWithoutAutomaticMigration() {
        QTemporaryDir source; const auto sourcePath = source.filePath("source.bin"); QByteArray original;
        { Vault vault(sourcePath); QVERIFY(vault.create(Password));
          QVERIFY(vault.addAccount(okxVaultAccount(),okxVaultCredentials(),StorageMode::EncryptedVault)); original = read(sourcePath); }
        for (const int schema : {1,2,3,4}) {
            QTemporaryDir dir; const auto path = dir.filePath("legacy.bin");
            const auto legacy = rewriteFakeMetadata(original,[&](QJsonObject& json) {
                json["schema_version"] = schema; if (schema == 1) json.remove("pending_operations");
                auto accounts = json["accounts"].toArray(); auto row = accounts[0].toObject(); row.remove("region");
                accounts[0] = row; json["accounts"] = accounts;
            });
            QVERIFY(!legacy.isEmpty()); QVERIFY(write(path,legacy)); QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner);
            Vault vault(path); QVERIFY(vault.unlock(Password)); QVERIFY(vault.accounts().first().region.isEmpty());
            QCOMPARE(read(path),legacy);
            const auto id = vault.accounts().first().id;
            QVERIFY(vault.replaceCredentials(id,okxVaultCredentials())); QVERIFY(vault.accounts().first().region.isEmpty());
            QVERIFY(vault.replaceCredentials(id,okxVaultCredentials(),"eea"));
            vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.accounts().first().region,QString("eea"));
        }
        QTemporaryDir invalid; const auto invalidPath = invalid.filePath("schema4-with-region.bin");
        QVERIFY(write(invalidPath,rewriteFakeMetadata(original,[](QJsonObject& json) { json["schema_version"] = 4; })));
        QFile::setPermissions(invalidPath,QFile::ReadOwner|QFile::WriteOwner);
        Vault rejected(invalidPath); QVERIFY(!rejected.unlock(Password));
    }
    void okxSchemaFiveMixedJournalAndStrictBoundaries() {
        QTemporaryDir dir; Vault vault(dir.filePath("okx-journal.bin")); QVERIFY(vault.create(Password));
        auto amend = okxPendingOrder(); amend["id"] = "fx-okx-amend"; amend["kind"] = "amend";
        amend["orderId"] = "0000123"; amend["amendRequestId"] = QString(32,'b'); amend["quantity"] = "3";
        amend["beforePrice"] = "100"; amend["beforeQuantity"] = "2"; amend["beforeExecuted"] = "0.5";
        amend["beforeUpdatedAt"] = "1792200000000"; amend["ackState"] = "accepted";
        auto cancel = okxPendingOrder(); cancel["id"] = "fx-okx-cancel"; cancel["kind"] = "cancel";
        cancel["orderId"] = "456"; cancel["clientId"] = "ExternalOrder"; cancel["side"] = "sell";
        cancel["type"] = "market"; cancel["quantityUnit"] = "quote"; cancel["quantity"] = "";
        cancel["quoteAmount"] = "200"; cancel["price"] = "";
        QJsonArray expected{pendingOrder("BTCUSDT"),tossPendingOrder(),kiwoomPendingOrder(),lsPendingOrder(),okxPendingOrder(),amend,cancel};
        QString error; QVERIFY2(vault.setPendingOperations(expected,&error),qPrintable(error));
        const auto disk = read(vault.filePath()); QVERIFY(!disk.contains("fake-okx-connection")); QVERIFY(!disk.contains("beforeExecuted"));
        QJsonObject metadata; QVERIFY(!rewriteFakeMetadata(disk,[&](QJsonObject& json) { metadata = json; }).isEmpty());
        QCOMPARE(metadata["schema_version"],QJsonValue(7)); QCOMPARE(metadata["pending_operations"].toArray(),expected);
        vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(),expected);
        QList<QJsonObject> invalid;
        auto bad = amend; bad.remove("beforePrice"); invalid.append(bad);
        bad = amend; bad["extra"] = "value"; invalid.append(bad);
        for (const auto& change : QList<QPair<QString,QString>>{{"region",""},{"environment","paper"},{"accountScope",QString(64,'G')},
             {"amendRequestId","old_request"},{"orderId","123x"},{"beforeExecuted","2"},{"beforeUpdatedAt","0"},{"quantity","0.5"}}) {
            bad = amend; bad[change.first] = change.second; invalid.append(bad);
        }
        bad = cancel; bad["quantity"] = "1"; invalid.append(bad);
        bad = cancel; bad["kind"] = "place"; bad["clientId"] = QString(32,'a'); invalid.append(bad); // New sells require base quantity.
        for (const auto& item : invalid) {
            QVERIFY(!vault.setPendingOperations(QJsonArray{item})); QCOMPARE(vault.pendingOperations(),expected); QCOMPARE(read(vault.filePath()),disk);
        }
        for (const auto* type : {"post_only","ioc","fok"}) {
            auto external = okxPendingOrder(); external["kind"] = "cancel"; external["orderId"] = "789"; external["type"] = type;
            QVERIFY(vault.setPendingOperations(QJsonArray{external}));
        }
        QVERIFY(vault.setPendingOperations(expected)); vault.lock();
        const auto older = rewriteFakeMetadata(disk,[](QJsonObject& json) { json["schema_version"] = 4; });
        QVERIFY(write(vault.filePath(),older)); QVERIFY(!vault.unlock(Password));
    }

    void schemaThreeMixedJournalSurvivesEncryptedReopen() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("mixed-journal.bin");
        auto upbit = pendingOrder(QStringLiteral("KRW-BTC"));
        upbit["id"] = "fx-upbit-journal"; upbit["provider"] = "upbit"; upbit["environment"] = "live";
        auto place = tossPendingOrder();
        place["id"] = "fx-toss-place"; place["kind"] = "place";
        place["orderId"] = ""; place["operationOrderId"] = "";
        place["clientId"] = "fake-toss-client";
        auto cancel = tossPendingOrder();
        cancel["id"] = "fx-toss-cancel"; cancel["kind"] = "cancel";
        cancel["operationOrderId"] = QString(128, 'X');
        const QJsonArray expected{upbit, pendingOrder(QStringLiteral("测试测试USDT")), tossPendingOrder(), place, cancel};
        {
            Vault vault(path);
            QVERIFY(vault.create(Password));
            const auto originalHeader = read(path).left(28);
            QString error;
            QVERIFY2(vault.setPendingOperations(expected, &error), qPrintable(error));
            QCOMPARE(vault.pendingOperations(), expected);
            const auto disk = read(path);
            auto duplicate = expected; auto duplicateToss = tossPendingOrder();
            duplicateToss["id"] = upbit.value("id"); duplicate.append(duplicateToss);
            QVERIFY(!vault.setPendingOperations(duplicate));
            QCOMPARE(vault.pendingOperations(), expected); QCOMPARE(read(path), disk);
            QCOMPARE(disk.left(28), originalHeader);
            for (const auto& marker : {"fake-original-order", "fake-operation-order", "fake-toss-local-connection", "domestic_equity"})
                QVERIFY(!disk.contains(marker));
            QVERIFY(!disk.contains(QString(64, 'b').toUtf8()));
            QJsonObject metadata;
            QVERIFY(!rewriteFakeMetadata(disk, [&](QJsonObject& json) { metadata = json; }).isEmpty());
            QCOMPARE(metadata.value("schema_version"), QJsonValue(7));
            QCOMPARE(metadata.value("pending_operations").toArray(), expected);
            vault.lock();
            QVERIFY(vault.pendingOperations().isEmpty());
        }
        Vault reopened(path);
        QVERIFY(reopened.unlock(Password));
        QCOMPARE(reopened.pendingOperations(), expected);
        QCOMPARE(reopened.pendingOperations()[0].toObject().size(), 17);
        QCOMPARE(reopened.pendingOperations()[2].toObject().size(), 20);
    }

    void schemaTwoJournalUpgradesWithoutRewritingLegacyRecords() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("g2-journal.bin");
        Vault vault(path);
        QVERIFY(vault.create(Password));
        const QJsonArray legacy{pendingOrder(QStringLiteral("BTCUSDT"))};
        QVERIFY(vault.setPendingOperations(legacy));
        vault.lock();
        const auto v2 = rewriteFakeMetadata(read(path), [](QJsonObject& json) { json["schema_version"] = 2; });
        QVERIFY(!v2.isEmpty()); QVERIFY(write(path, v2));
        QVERIFY(vault.unlock(Password));
        QCOMPARE(vault.pendingOperations(), legacy);
        QCOMPARE(read(path), v2); // Opening an older payload does not rewrite it.
        QJsonArray mixed = legacy; mixed.append(tossPendingOrder());
        QVERIFY(vault.setPendingOperations(mixed));
        const auto upgraded = read(path);
        QCOMPARE(upgraded.left(28), v2.left(28));
        QJsonObject metadata;
        QVERIFY(!rewriteFakeMetadata(upgraded, [&](QJsonObject& json) { metadata = json; }).isEmpty());
        QCOMPARE(metadata.value("schema_version"), QJsonValue(7));
        QCOMPARE(metadata.value("pending_operations").toArray().first(), legacy.first());
        vault.lock(); QVERIFY(vault.unlock(Password));
        QCOMPARE(vault.pendingOperations(), mixed);
    }

    void kisScopedJournalPreservesLivePaperAndLegacy() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("fake-kis-journal.bin");
        auto live = tossPendingOrder(); live["id"] = "fake-kis-live"; live["provider"] = "kis";
        live["orderId"] = "K1_20261017_00123_0000001234_KRX";
        live["operationOrderId"] = "K1_20261017_00123_0000001235_KRX";
        auto paper = live; paper["id"] = "fake-kis-paper"; paper["environment"] = "paper"; paper["accountScope"] = QString(64, 'c');
        const QJsonArray expected{pendingOrder("BTCUSDT"), tossPendingOrder(), live, paper};
        {
            Vault vault(path); QVERIFY(vault.create(Password)); QVERIFY(vault.setPendingOperations(expected));
            const auto encrypted = read(path); QVERIFY(!encrypted.contains("K1_20261017"));
            QVERIFY(!encrypted.contains(QString(64, 'c').toUtf8()));
            vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(), expected);
            for (const auto& environment : {QStringLiteral("testnet"), QStringLiteral("LIVE"), QString{}}) {
                auto invalid = live; invalid["environment"] = environment;
                QVERIFY(!vault.setPendingOperations({invalid})); QCOMPARE(read(path), encrypted);
            }
            for (const auto* field : {"accountScope", "operationOrderId", "market"}) {
                auto invalid = live; invalid.remove(field); QVERIFY(!vault.setPendingOperations({invalid}));
            }
            auto secret = live; secret["CANO"] = "00000000"; QVERIFY(!vault.setPendingOperations({secret}));
            QCOMPARE(vault.pendingOperations(), expected);
        }
        Vault reopened(path); QVERIFY(reopened.unlock(Password)); QCOMPARE(reopened.pendingOperations(), expected);
        reopened.lock(); const auto valid = read(path);
        for (int version : {1, 2}) {
            const auto oldSchema = rewriteFakeMetadata(valid, [version](QJsonObject& json) { json["schema_version"] = version; });
            QVERIFY(write(path, oldSchema)); QVERIFY(!reopened.unlock(Password));
        }
    }

    void tossJournalRequiresCompleteScopedSchema_data() {
        QTest::addColumn<QJsonObject>("invalid");
        const auto invalidField = [](const char* name, const QString& key, const QJsonValue& value) {
            auto entry = tossPendingOrder(); entry[key] = value;
            QTest::newRow(name) << entry;
        };
        invalidField("empty-scope", "accountScope", "");
        invalidField("short-scope", "accountScope", QString(63, 'a'));
        invalidField("long-scope", "accountScope", QString(65, 'a'));
        invalidField("uppercase-scope", "accountScope", QString(64, 'A'));
        invalidField("nonhex-scope", "accountScope", QString(64, 'g'));
        invalidField("scope-newline", "accountScope", QString(64, 'a') + '\n');
        invalidField("numeric-scope", "accountScope", 1);
        invalidField("wrong-market", "market", "spot");
        invalidField("unknown-provider", "provider", "unknown-broker");
        invalidField("legacy-provider-with-new-fields", "provider", "binance");
        invalidField("testnet-environment", "environment", "testnet");
        invalidField("paper-environment", "environment", "paper");
        invalidField("operation-id-too-long", "operationOrderId", QString(129, 'a'));
        invalidField("operation-id-unicode", "operationOrderId", QStringLiteral("가짜주문"));
        invalidField("operation-id-control", "operationOrderId", "fake-order\n");
        invalidField("operation-id-path-separator", "operationOrderId", "fake/order");
        invalidField("operation-id-number", "operationOrderId", 1);
        invalidField("unknown-field", "unexpected", "forbidden");
        invalidField("private-account-seq", "accountSeq", "FAKE_PRIVATE_ONLY");
        invalidField("private-account-number", "accountNo", "FAKE_PRIVATE_ONLY");
        invalidField("access-token", "access_token", "FAKE_TOKEN_ONLY");
        for (const auto& key : tossPendingOrder().keys()) {
            auto entry = tossPendingOrder(); entry.remove(key);
            QTest::newRow(qPrintable(QStringLiteral("missing-") + key)) << entry;
        }
    }
    void tossJournalRequiresCompleteScopedSchema() {
        QFETCH(QJsonObject, invalid);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("invalid-toss-journal.bin");
        Vault vault(path);
        QVERIFY(vault.create(Password));
        const QJsonArray baseline{tossPendingOrder()};
        QVERIFY(vault.setPendingOperations(baseline));
        const auto before = read(path);
        QString error;
        QVERIFY(!vault.setPendingOperations({invalid}, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(vault.isUnlocked());
        QCOMPARE(vault.pendingOperations(), baseline); QCOMPARE(read(path), before);
        vault.lock();
        const auto malformed = rewriteFakeMetadata(before, [&](QJsonObject& json) { json["pending_operations"] = QJsonArray{invalid}; });
        QVERIFY(!malformed.isEmpty()); QVERIFY(write(path, malformed));
        QVERIFY(!vault.unlock(Password, &error));
        QVERIFY(!vault.isUnlocked()); QVERIFY(vault.pendingOperations().isEmpty());
        QCOMPARE(read(path), malformed);
    }

    void schemaVersionCannotSmuggleTossRecords() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("schema-boundary.bin");
        Vault vault(path);
        QVERIFY(vault.create(Password));
        QVERIFY(vault.setPendingOperations({tossPendingOrder()}));
        vault.lock();
        const auto v3 = read(path);
        for (const int version : {1, 2, 10}) {
            const auto malformed = rewriteFakeMetadata(v3, [version](QJsonObject& json) { json["schema_version"] = version; });
            QVERIFY(!malformed.isEmpty()); QVERIFY(write(path, malformed));
            QVERIFY(!vault.unlock(Password));
            QCOMPARE(read(path), malformed);
        }
        QVERIFY(write(path, v3)); QVERIFY(vault.unlock(Password));
        QCOMPARE(vault.pendingOperations(), QJsonArray{tossPendingOrder()});
    }
    void schemaFourPreservesThreeReferencesAndLegacyReopen() {
        QTemporaryDir dir; const auto path = dir.filePath("fake-schema-four.bin");
        auto kis = tossPendingOrder(); kis["id"] = "fake-kis"; kis["provider"] = "kis";
        const QJsonArray legacy{pendingOrder("BTCUSDT"), tossPendingOrder(), kis};
        Vault vault(path); QVERIFY(vault.create(Password)); QVERIFY(vault.setPendingOperations(legacy)); vault.lock();
        const auto schemaThree = rewriteFakeMetadata(read(path), [](QJsonObject& root) { root["schema_version"] = 3; });
        QVERIFY(write(path, schemaThree)); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(), legacy);
        QCOMPARE(read(path), schemaThree);
        auto live = kiwoomPendingOrder(); auto paper = live;
        paper["id"] = "fake-kiwoom-paper"; paper["environment"] = "paper"; paper["accountScope"] = QString(64, 'd');
        auto unsent = live; unsent["id"] = "fake-kiwoom-before-receipt"; unsent["state"] = "pending";
        unsent["operationOrderId"] = ""; unsent["baseOriginalOrderId"] = ""; unsent["baseOriginalProvenance"] = "";
        auto place = unsent; place["id"] = "fake-kiwoom-place"; place["kind"] = "place";
        place["orderId"] = ""; place["quantityMode"] = "exact";
        auto expected = legacy; for (const auto& entry : {live, paper, unsent, place}) expected.append(entry);
        QVERIFY(vault.setPendingOperations(expected)); const auto encrypted = read(path);
        QCOMPARE(encrypted.left(28), schemaThree.left(28));
        for (const auto* marker : {"W1_20261017", "broker_receipt", "all_remaining", "fake-kiwoom-local-connection"}) QVERIFY(!encrypted.contains(marker));
        QJsonObject metadata; QVERIFY(!rewriteFakeMetadata(encrypted, [&](QJsonObject& root) { metadata = root; }).isEmpty());
        QCOMPARE(metadata.value("schema_version"), QJsonValue(7)); QCOMPARE(metadata.value("pending_operations").toArray(), expected);
        vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(), expected); vault.lock();
        // Schemas 8/9 are now supported. Reading them must preserve the same
        // three-reference journal without silently rewriting the ciphertext.
        for (const int schema : {8, 9}) {
            const auto compatible = rewriteFakeMetadata(encrypted, [schema](QJsonObject& root) { root["schema_version"] = schema; });
            QVERIFY(write(path, compatible)); QVERIFY(vault.unlock(Password));
            QCOMPARE(vault.pendingOperations(), expected); QCOMPARE(read(path), compatible); vault.lock();
        }
        for (const int schema : {1, 2, 3, 10}) {
            const auto invalid = rewriteFakeMetadata(encrypted, [schema](QJsonObject& root) { root["schema_version"] = schema; });
            QVERIFY(write(path, invalid)); QVERIFY(!vault.unlock(Password)); QCOMPARE(read(path), invalid);
        }
        QVERIFY(write(path, encrypted)); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(), expected);
    }
    void schemaFourRejectsMalformedRelationsWithoutChangingCiphertext_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("unknown-provenance") << QString("baseOriginalProvenance") << QJsonValue("user_input");
        QTest::newRow("missing-provenance") << QString("baseOriginalProvenance") << QJsonValue("");
        QTest::newRow("root-without-receipt") << QString("operationOrderId") << QJsonValue("");
        QTest::newRow("provenance-without-root") << QString("baseOriginalOrderId") << QJsonValue("");
        QTest::newRow("bad-date") << QString("baseOriginalOrderId") << QJsonValue("W1_20260230_KRX_0000139");
        QTest::newRow("zero-number") << QString("orderId") << QJsonValue("W1_20261017_KRX_0000000");
        QTest::newRow("bad-venue") << QString("orderId") << QJsonValue("W1_20261017_ANY_0000140");
        QTest::newRow("too-long-id") << QString("orderId") << QJsonValue("W1_20261017_KRX_123456789012345678901");
        QTest::newRow("mode-number") << QString("quantityMode") << QJsonValue(1);
        QTest::newRow("unknown-mode") << QString("quantityMode") << QJsonValue("remaining");
        QTest::newRow("all-remaining-zero") << QString("quantity") << QJsonValue("00");
        QTest::newRow("all-remaining-fraction") << QString("quantity") << QJsonValue("1.5");
        QTest::newRow("place-with-root-mode") << QString("kind") << QJsonValue("place");
        QTest::newRow("wrong-scope") << QString("accountScope") << QJsonValue(QString(64, 'G'));
        QTest::newRow("wrong-environment") << QString("environment") << QJsonValue("testnet");
        QTest::newRow("wrong-provider") << QString("provider") << QJsonValue("toss");
        QTest::newRow("extra-private-field") << QString("acctNo") << QJsonValue("0000000000");
        QTest::newRow("missing-field") << QString("quantityMode") << QJsonValue(QJsonValue::Undefined);
    }
    void schemaFourRejectsMalformedRelationsWithoutChangingCiphertext() {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        QTemporaryDir dir; const auto path = dir.filePath("fake-invalid-schema-four.bin");
        Vault vault(path); QVERIFY(vault.create(Password)); const QJsonArray expected{kiwoomPendingOrder()};
        QVERIFY(vault.setPendingOperations(expected)); const auto encrypted = read(path);
        auto invalid = kiwoomPendingOrder(); if (value.isUndefined()) invalid.remove(field); else invalid[field] = value;
        QString error; QVERIFY(!vault.setPendingOperations({invalid}, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(vault.pendingOperations(), expected); QCOMPARE(read(path), encrypted);
        vault.lock();
        const auto authenticatedInvalid = rewriteFakeMetadata(encrypted, [&](QJsonObject& root) { root["pending_operations"] = QJsonArray{invalid}; });
        QVERIFY(write(path, authenticatedInvalid)); QVERIFY(!vault.unlock(Password)); QCOMPARE(read(path), authenticatedInvalid);
    }

    void schemaFourLsPreservesExactRemainingAndExistingProviders() {
        QTemporaryDir dir; const auto path = dir.filePath("fake-ls-schema-four.bin");
        auto kis = tossPendingOrder(); kis["id"] = "fake-kis-ls-mix"; kis["provider"] = "kis";
        const QJsonArray old{pendingOrder("BTCUSDT"), tossPendingOrder(), kis, kiwoomPendingOrder()};
        Vault vault(path); QVERIFY(vault.create(Password)); QVERIFY(vault.setPendingOperations(old));
        const auto previous = read(path);
        auto live = lsPendingOrder(); auto paper = live; paper["id"] = "fake-ls-paper"; paper["environment"] = "paper";
        auto beforeReceipt = live; beforeReceipt["id"] = "fake-ls-before-receipt"; beforeReceipt["state"] = "pending";
        beforeReceipt["operationOrderId"] = ""; beforeReceipt["baseOriginalOrderId"] = ""; beforeReceipt["baseOriginalProvenance"] = "";
        auto place = beforeReceipt; place["id"] = "fake-ls-place"; place["kind"] = "place"; place["orderId"] = "";
        place["quantity"] = "9007199254740993"; // Decimal text survives without a double round-trip.
        auto expected = old; for (const auto& entry : {live, paper, beforeReceipt, place}) expected.append(entry);
        QVERIFY(vault.setPendingOperations(expected)); const auto encrypted = read(path); QCOMPARE(encrypted.left(28), previous.left(28));
        for (const auto* marker : {"L1_20261019", "fake-ls-connection", "9007199254740993"}) QVERIFY(!encrypted.contains(marker));
        QJsonObject metadata; QVERIFY(!rewriteFakeMetadata(encrypted, [&](QJsonObject& root) { metadata = root; }).isEmpty());
        QCOMPARE(metadata.value("schema_version"), QJsonValue(7)); QCOMPARE(metadata.value("pending_operations").toArray(), expected);
        vault.lock(); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(), expected); vault.lock();
        for (int version : {1, 2, 3}) {
            const auto invalid = rewriteFakeMetadata(encrypted, [version](QJsonObject& root) { root["schema_version"] = version; });
            QVERIFY(write(path, invalid)); QVERIFY(!vault.unlock(Password)); QCOMPARE(read(path), invalid);
        }
        QVERIFY(write(path, encrypted)); QVERIFY(vault.unlock(Password)); QCOMPARE(vault.pendingOperations(), expected);
    }
    void schemaFourLsRejectsWrongModeReferencesAndExtraFields_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("no-zero-means-all") << QString("quantityMode") << QJsonValue("all_remaining");
        QTest::newRow("missing-mode") << QString("quantityMode") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("zero-exact-quantity") << QString("quantity") << QJsonValue("000");
        QTest::newRow("fractional-exact-quantity") << QString("quantity") << QJsonValue("8.5");
        QTest::newRow("quantity-over-sixteen-digits") << QString("quantity") << QJsonValue("10000000000000000");
        QTest::newRow("number-instead-of-decimal-text") << QString("quantity") << QJsonValue(8);
        QTest::newRow("wrong-reference-provider") << QString("orderId") << QJsonValue("W1_20261019_KRX_0000000140");
        QTest::newRow("eleven-digit-order-number") << QString("operationOrderId") << QJsonValue("L1_20261019_KRX_10000000141");
        QTest::newRow("zero-order-number") << QString("orderId") << QJsonValue("L1_20261019_KRX_0000000000");
        QTest::newRow("invalid-reference-date") << QString("baseOriginalOrderId") << QJsonValue("L1_20260230_KRX_139");
        QTest::newRow("invalid-reference-venue") << QString("orderId") << QJsonValue("L1_20261019_UNKNOWN_140");
        QTest::newRow("missing-receipt-with-root") << QString("operationOrderId") << QJsonValue("");
        QTest::newRow("user-association-is-not-receipt") << QString("baseOriginalProvenance") << QJsonValue("user_input");
        QTest::newRow("scope-invalid") << QString("accountScope") << QJsonValue(QString(64, 'X'));
        QTest::newRow("wrong-environment") << QString("environment") << QJsonValue("testnet");
        QTest::newRow("ls-fields-cannot-enter-toss") << QString("provider") << QJsonValue("toss");
        QTest::newRow("ls-fields-cannot-enter-kiwoom") << QString("provider") << QJsonValue("kiwoom");
        QTest::newRow("private-field-is-forbidden") << QString("AcntNo") << QJsonValue("FAKE_ACCOUNT");
    }
    void schemaFourLsRejectsWrongModeReferencesAndExtraFields() {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        QTemporaryDir dir; const auto path = dir.filePath("fake-ls-invalid.bin"); Vault vault(path); QVERIFY(vault.create(Password));
        const QJsonArray expected{lsPendingOrder()}; QVERIFY(vault.setPendingOperations(expected)); const auto encrypted = read(path);
        auto invalid = lsPendingOrder(); if (value.isUndefined()) invalid.remove(field); else invalid[field] = value;
        QString error; QVERIFY(!vault.setPendingOperations({invalid}, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(vault.pendingOperations(), expected); QCOMPARE(read(path), encrypted); vault.lock();
        const auto authenticatedInvalid = rewriteFakeMetadata(encrypted, [&](QJsonObject& root) { root["pending_operations"] = QJsonArray{invalid}; });
        QVERIFY(write(path, authenticatedInvalid)); QVERIFY(!vault.unlock(Password)); QCOMPARE(read(path), authenticatedInvalid);
    }

    void binanceUtf8JournalSurvivesEncryptedReopen_data() {
        QTest::addColumn<QString>("symbol");
        QTest::newRow("utf8-symbol") << QStringLiteral("测试测试USDT");
        QTest::newRow("decomposed-original") << QStringLiteral("e\u0301USDT");
        QTest::newRow("forty-scalars") << QString(40, QChar(0x6d4b));
        QTest::newRow("forty-supplementary-scalars-160-bytes") << QStringLiteral("\U00010400").repeated(40);
    }
    void binanceUtf8JournalSurvivesEncryptedReopen() {
        QFETCH(QString, symbol);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("utf8-vault.bin");
        const QJsonArray expected{pendingOrder(symbol)};
        {
            Vault vault(path);
            QVERIFY(vault.create(Password));
            QString error;
            QVERIFY2(vault.setPendingOperations(expected, &error), qPrintable(error));
            QCOMPARE(vault.pendingOperations(), expected);
            const auto disk = read(path);
            QVERIFY(!disk.isEmpty());
            QVERIFY(!disk.contains(symbol.toUtf8()));
            QVERIFY(!disk.contains("fake-utf8-client"));
            vault.lock();
            QVERIFY(vault.pendingOperations().isEmpty());
        }
        Vault reopened(path);
        QVERIFY(reopened.unlock(Password));
        QCOMPARE(reopened.pendingOperations(), expected);
        QCOMPARE(reopened.pendingOperations().first().toObject().value("symbol").toString().toUtf8(), symbol.toUtf8());
    }

    void invalidBinanceJournalSymbolPreservesSavedRecord_data() {
        QTest::addColumn<QString>("symbol");
        QTest::newRow("empty") << QString();
        QTest::newRow("one-scalar") << QStringLiteral("币");
        QTest::newRow("nul-control") << (QStringLiteral("BTC") + QChar(0) + QStringLiteral("USDT"));
        QTest::newRow("newline-control") << QStringLiteral("BTC\nUSDT");
        QTest::newRow("delete-control") << (QStringLiteral("BTC") + QChar(0x7f) + QStringLiteral("USDT"));
        QTest::newRow("c1-control") << (QStringLiteral("BTC") + QChar(0x85) + QStringLiteral("USDT"));
        QTest::newRow("bidi-format") << QStringLiteral("BTC\u202eUSDT");
        QTest::newRow("joiner-format") << QStringLiteral("BTC\u200dUSDT");
        QTest::newRow("space") << QStringLiteral("BTC USDT");
        QTest::newRow("url-delimiter") << QStringLiteral("BTC/USDT");
        QTest::newRow("unpaired-high-surrogate") << (QStringLiteral("BTC") + QChar(0xd800));
        QTest::newRow("unpaired-low-surrogate") << (QStringLiteral("BTC") + QChar(0xdc00));
        QTest::newRow("high-surrogate-followed-by-letter") << (QString(QChar(0xd800)) + QStringLiteral("USDT"));
        QTest::newRow("forty-one-ascii-scalars") << QString(41, QChar('A'));
        QTest::newRow("forty-one-cjk-scalars") << QString(41, QChar(0x6d4b));
        QTest::newRow("over-160-utf8-bytes") << QStringLiteral("\U00010400").repeated(41);
    }
    void invalidBinanceJournalSymbolPreservesSavedRecord() {
        QFETCH(QString, symbol);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("invalid-symbol-vault.bin");
        Vault vault(path);
        QVERIFY(vault.create(Password));
        const QJsonArray baseline{pendingOrder(QStringLiteral("测试测试USDT"))};
        QVERIFY(vault.setPendingOperations(baseline));
        const auto saved = read(path);
        QString error;
        QVERIFY(!vault.setPendingOperations({pendingOrder(symbol)}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(vault.isUnlocked());
        QCOMPARE(vault.pendingOperations(), baseline);
        QCOMPARE(read(path), saved);
        vault.lock();
        QVERIFY(vault.unlock(Password));
        QCOMPARE(vault.pendingOperations(), baseline);
    }

    void pendingJournalOtherFieldsRetainAsciiValidation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("restricted-vault.bin");
        Vault vault(path);
        QVERIFY(vault.create(Password));
        const QJsonArray baseline{pendingOrder(QStringLiteral("BTCUSDT"))};
        QVERIFY(vault.setPendingOperations(baseline));
        const auto saved = read(path);
        auto nonBinance = pendingOrder(QStringLiteral("测试测试USDT"));
        nonBinance["provider"] = "upbit";
        QVERIFY(!vault.setPendingOperations({nonBinance}));
        for (const auto& field : {"clientId", "orderId", "replacementClientId", "quantity", "price", "quoteAmount"}) {
            auto invalid = baseline.first().toObject();
            invalid[field] = QStringLiteral("测试");
            QString error;
            QVERIFY2(!vault.setPendingOperations({invalid}, &error), field);
            QVERIFY(!error.isEmpty());
        }
        QCOMPARE(vault.pendingOperations(), baseline);
        QCOMPARE(read(path), saved);
    }

    void legacyWeakPasswordRemainsReadableAndCanBeStrengthened() {
        QTemporaryDir dir;
        const auto path = dir.filePath("legacy-vault.bin");
        Vault vault(path); QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(record(), credentials(), StorageMode::EncryptedVault));
        vault.lock();
        const auto legacy = invalidSchema(read(path), true, LegacyWeakPassword);
        QVERIFY(!legacy.isEmpty()); QVERIFY(write(path, legacy));
        QVERIFY(!Vault::validateNewPassword(LegacyWeakPassword));
        QVERIFY(vault.unlock(LegacyWeakPassword)); QCOMPARE(vault.accounts().size(), 1); QVERIFY(vault.pendingOperations().isEmpty());
        bool matched = false;
        QVERIFY(vault.withCredentials(vault.accounts().first().id, [&](const CredentialSet& values) {
            matched = equalSecret(values, "secret", FakeSecret);
        }));
        QVERIFY(matched);
        QString error;
        QVERIFY(!vault.changePassword(LegacyWeakPassword, LegacyWeakPassword, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(vault.isUnlocked()); QCOMPARE(read(path), legacy);
        QVERIFY(vault.changePassword(LegacyWeakPassword, NewPassword));
        vault.lock(); QVERIFY(vault.unlock(NewPassword)); QCOMPARE(vault.accounts().size(), 1);
        const auto strengthened = read(path);
        // Header format, KDF cost, and AEAD selection stay unchanged.
        QCOMPARE(strengthened.left(28), legacy.left(28));
        vault.lock(); QVERIFY(!vault.unlock(LegacyWeakPassword)); QVERIFY(vault.unlock(NewPassword));
    }
    void memoryLockFailureIsClosed() {
#ifdef Q_OS_UNIX
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--mlock-failure-child", dir.filePath("vault.bin")});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        QVERIFY(!QFileInfo::exists(dir.filePath("vault.bin")));
        QVERIFY(!QFileInfo::exists(dir.filePath("vault.bin.lock")));
#else
        QSKIP("RLIMIT_MEMLOCK child fault injection is Unix-specific.");
#endif
    }

    void kdfBaselineTiming() {
        SecureBuffer key(32);
        unsigned char salt[crypto_pwhash_SALTBYTES];
        randombytes_buf(salt, sizeof salt);
        const QByteArray password = Password.toUtf8(); // Fixed fake test input.
        QElapsedTimer timer;
        timer.start();
        const int result = crypto_pwhash(key.data(), key.size(), password.constData(), password.size(), salt,
            Vault::DefaultOpsLimit, Vault::DefaultMemLimit, crypto_pwhash_ALG_ARGON2ID13);
        const qint64 elapsed = timer.elapsed();
        QCOMPARE(result, 0);
        qInfo("Argon2id13 baseline: memory_MiB=64 ops=3 elapsed_ms=%lld", static_cast<long long>(elapsed));
    }

    void secureBufferMoveAndClear() {
        SecureBuffer source = SecureBuffer::fromUtf8(FakeSecret);
        QVERIFY(!source.empty());
        const auto size = source.size();
        SecureBuffer moved(std::move(source));
        QVERIFY(source.empty());
        QVERIFY(source.data() == nullptr);
        QCOMPARE(moved.size(), size);
        moved.clear();
        QVERIFY(moved.empty());
        QVERIFY(moved.data() == nullptr);
    }

    void binanceReplaceAndListJournalsAreEncryptedAndVersionBound(){
        QTemporaryDir dir;const auto path=dir.filePath("vault");Vault vault(path);QVERIFY(vault.create(Password));
        auto replacement=pendingOrder("BTCUSDT");replacement["kind"]="amend";replacement["orderId"]="123";replacement["replacementClientId"]="keefi-replacement";replacement["replaceMode"]="cancel_replace";replacement["replacementState"]="unknown";
        auto group=pendingOrder("BTCUSDT");group["id"]="fake-group-journal";group["kind"]="cancel";group["orderId"]="124";group["orderListId"]="12";
        const QJsonArray entries{replacement,group};QVERIFY(vault.setPendingOperations(entries));const auto cipher=read(path);QVERIFY(!cipher.contains("cancel_replace"));vault.lock();QVERIFY(vault.unlock(Password));QCOMPARE(vault.pendingOperations(),entries);vault.lock();
        const auto older=rewriteFakeMetadata(cipher,[](QJsonObject& root){root["schema_version"]=7;});QVERIFY(write(path,older));QVERIFY(!vault.unlock(Password));QVERIFY(write(path,cipher));QVERIFY(vault.unlock(Password));
        replacement["provider"]="upbit";QVERIFY(!vault.setPendingOperations({replacement}));group["orderListId"]="bad";QVERIFY(!vault.setPendingOperations({group}));QCOMPARE(vault.pendingOperations(),entries);
    }
    void createRoundTripAndLock() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("vault.bin");
        Vault vault(path);
        AccountModel model(&vault);
        QString error;
        QVERIFY2(vault.create(Password, &error), qPrintable(error));
        QVERIFY(vault.isUnlocked());
        QVERIFY2(vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error), qPrintable(error));
        QCOMPARE(model.rowCount(), 1);
        const QString id = model.recordAt(0).id;
        QCOMPARE(model.recordAt(0).permissionState, QString("unverified"));
        QCOMPARE(model.recordAt(0).connectionState, QString("not_checked"));
        QVERIFY(model.recordAt(0).readOnly);
        const QByteArray bytes = read(path);
        QVERIFY(!bytes.contains(FakeKey.toUtf8()));
        QVERIFY(!bytes.contains(FakeSecret.toUtf8()));
        QVERIFY(!bytes.contains(Password.toUtf8()));
        QVERIFY(!bytes.contains(record().alias.toUtf8()));
        QSignalSpy locked(&vault, &Vault::locked);
        vault.lock();
        QVERIFY(!vault.isUnlocked());
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(locked.count(), 1);
        bool called = false;
        QVERIFY(!vault.withCredentials(id, [&](const auto&) { called = true; }, &error));
        QVERIFY(!called);
        QVERIFY2(vault.unlock(Password, &error), qPrintable(error));
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(vault.withCredentials(id, [&](const auto& fields) {
            QVERIFY(equalSecret(fields, "access_key", FakeKey));
            QVERIFY(equalSecret(fields, "secret", FakeSecret));
        }));
    }

    void replaceAndRemove() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        QVERIFY(vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error));
        const QString id = vault.accounts().first().id;
        auto replacement = credentials();
        replacement["secret"] = "KEEFI_FAKE_REPLACEMENT_ONLY";
        QVERIFY(vault.replaceCredentials(id, replacement, &error));
        vault.lock();
        QVERIFY(vault.unlock(Password, &error));
        QVERIFY(vault.withCredentials(id, [&](const auto& fields) {
            QVERIFY(equalSecret(fields, "secret", "KEEFI_FAKE_REPLACEMENT_ONLY"));
        }));
        QVERIFY(vault.removeAccount(id, &error));
        vault.lock();
        QVERIFY(vault.unlock(Password, &error));
        QVERIFY(vault.accounts().isEmpty());
    }

    void saltsNoncesAndRekey() {
        QTemporaryDir one, two;
        Vault first(one.filePath("vault.bin")), second(two.filePath("vault.bin"));
        QString error;
        QVERIFY(first.create(Password, &error));
        QVERIFY(second.create(Password, &error));
        const QByteArray initial = read(first.filePath());
        const QByteArray other = read(second.filePath());
        QVERIFY(initial.mid(28, 16) != other.mid(28, 16));
        QVERIFY(initial.mid(44, 24) != other.mid(44, 24));
        QVERIFY(first.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error));
        const QByteArray saved = read(first.filePath());
        QCOMPARE(initial.mid(28, 16), saved.mid(28, 16));
        QVERIFY(initial.mid(44, 24) != saved.mid(44, 24));
        QVERIFY(first.changePassword(Password, NewPassword, &error));
        const QByteArray rekeyed = read(first.filePath());
        QVERIFY(saved.mid(28, 16) != rekeyed.mid(28, 16));
        QVERIFY(saved.mid(44, 24) != rekeyed.mid(44, 24));
        first.lock();
        QVERIFY(!first.unlock(Password, &error));
        QVERIFY(!first.isUnlocked());
        QVERIFY(first.unlock(NewPassword, &error));
        QCOMPARE(first.accounts().size(), 1);
        // A separately retained old ciphertext still opens with the old password.
        first.lock();
        QVERIFY(write(first.filePath(), saved));
        QVERIFY(first.unlock(Password, &error));
    }

    void wrongPasswordAndExistingFilePreserved() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        const QByteArray original = read(vault.filePath());
        QVERIFY(!vault.create(NewPassword, &error));
        QCOMPARE(read(vault.filePath()), original);
        vault.lock();
        QVERIFY(!vault.unlock(NewPassword, &error));
        QVERIFY(!vault.isUnlocked());
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(vault.filePath()), original);
    }

    void malformedFiles_data() {
        QTest::addColumn<int>("variant");
        for (int i = 0; i < 11; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }

    void malformedFiles() {
        QFETCH(int, variant);
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        QByteArray bytes = read(vault.filePath());
        vault.lock();
        switch (variant) {
        case 0: bytes[0] ^= 1; break;
        case 1: bytes[8] = 3; break;
        case 2: bytes[10] = 2; break;
        case 3: bytes[14] = 3; break;
        case 4: qToLittleEndian<quint32>(0xffffffff, reinterpret_cast<uchar*>(bytes.data()) + 16); break;
        case 5: qToLittleEndian<quint64>(0xffffffffffffffffULL, reinterpret_cast<uchar*>(bytes.data()) + 20); break;
        case 6: bytes[28] ^= 1; break;
        case 7: bytes[44] ^= 1; break;
        case 8: bytes[bytes.size() - 1] ^= 1; break;
        case 9: bytes.truncate(20); break;
        case 10: bytes.chop(1); break;
        }
        QVERIFY(write(vault.filePath(), bytes));
        QVERIFY(!vault.unlock(Password, &error));
        QVERIFY(!vault.isUnlocked());
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(vault.filePath()), bytes);
    }

    void authenticatedInvalidSchemaRefused() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        QByteArray bytes = invalidSchema(read(vault.filePath()));
        QVERIFY(!bytes.isEmpty());
        vault.lock();
        QVERIFY(write(vault.filePath(), bytes));
        QVERIFY(!vault.unlock(Password, &error));
        QVERIFY(!vault.isUnlocked());
        QCOMPARE(read(vault.filePath()), bytes);
    }

    void oversizedFileRefused() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        vault.lock();
        QFile file(vault.filePath());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(Vault::MaxFileBytes + 1));
        file.close();
        QVERIFY(!vault.unlock(Password, &error));
        QCOMPARE(QFileInfo(vault.filePath()).size(), Vault::MaxFileBytes + 1);
    }

    void concurrentInstanceRefused() {
        QTemporaryDir dir;
        Vault first(dir.filePath("vault.bin")), second(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(first.create(Password, &error));
        QVERIFY(!second.unlock(Password, &error));
        QVERIFY(!second.isUnlocked());
        first.lock();
        QVERIFY2(second.unlock(Password, &error), qPrintable(error));
    }

    void externalFileChangePreserved() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        QByteArray changed = read(vault.filePath());
        changed[changed.size() - 1] ^= 1;
        QVERIFY(write(vault.filePath(), changed));
        QVERIFY(!vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error));
        QVERIFY(!vault.isUnlocked());
        QCOMPARE(read(vault.filePath()), changed);
    }

    void privatePermissionsAndSymlink() {
#ifdef Q_OS_UNIX
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        const auto permissions = QFile::permissions(vault.filePath());
        QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
        vault.lock();
        QVERIFY(QFile::setPermissions(vault.filePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        const QByteArray original = read(vault.filePath());
        QVERIFY(!vault.unlock(Password, &error));
        QCOMPARE(read(vault.filePath()), original);
        QVERIFY(QFile::setPermissions(vault.filePath(), QFile::ReadOwner | QFile::WriteOwner));
        const QString link = dir.filePath("linked.bin");
        QVERIFY(QFile::link(vault.filePath(), link));
        Vault linked(link);
        QVERIFY(!linked.unlock(Password, &error));
        QCOMPARE(read(vault.filePath()), original);
#else
        QSKIP("Unix permission and symlink test; Windows DACL requires Windows validation.");
#endif
    }

    void failedAtomicSavePreservesOriginal() {
#ifdef Q_OS_UNIX
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        const QByteArray before = read(vault.filePath());
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
        const bool saved = vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error);
        // Restore the test directory even if the assertion below fails.
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(!saved);
        QVERIFY(!vault.isUnlocked());
        QCOMPARE(read(vault.filePath()), before);
        QVERIFY2(vault.unlock(Password, &error), qPrintable(error));
        QVERIFY(vault.accounts().isEmpty());
#else
        QSKIP("Atomic write fault via Unix directory permission; Windows needs its own fault test.");
#endif
    }

    void residualLockDoesNotRemoveAnotherOwner() {
#ifdef Q_OS_UNIX
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
        const bool saved = vault.addAccount(record(), credentials(), StorageMode::EncryptedVault, &error);
        QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(!saved);
        QVERIFY(QFile::remove(vault.filePath() + ".lock")); // Only this test's orphan.
        QLockFile other(vault.filePath() + ".lock");
        other.setStaleLockTime(0);
        QVERIFY(other.tryLock());
        QVERIFY(!vault.unlock(Password, &error));
        QVERIFY(other.isLocked());
        QVERIFY(QFileInfo::exists(vault.filePath() + ".lock"));
        other.unlock();
        QVERIFY2(vault.unlock(Password, &error), qPrintable(error));
#else
        QSKIP("Unix permission fault; Windows needs native permission fault coverage.");
#endif
    }

    void profileDirectoryPreparation() {
        QTemporaryDir dir;
        QString error;
        const QString profile = dir.filePath("new-profile");
        QVERIFY2(Vault::prepareProfileDirectory(profile, &error), qPrintable(error));
        QVERIFY(Vault::prepareProfileDirectory(profile, &error));
        QVERIFY(!QFileInfo::exists(profile + "/vault.bin"));
        QVERIFY(!Vault::prepareProfileDirectory("relative-profile", &error));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::setPermissions(profile, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther));
        const auto wide = QFile::permissions(profile);
        QVERIFY(!Vault::prepareProfileDirectory(profile, &error));
        QCOMPARE(QFile::permissions(profile), wide);
        QVERIFY(QFile::setPermissions(profile, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString link = dir.filePath("linked-profile");
        QVERIFY(QFile::link(profile, link));
        QVERIFY(!Vault::prepareProfileDirectory(link, &error));
#endif
    }

    void extendedAclRefused() {
#ifdef Q_OS_MACOS
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        vault.lock();
        const QByteArray before = read(vault.filePath());
        QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "everyone allow read", vault.filePath()}), 0);
        const bool opened = vault.unlock(Password, &error);
        QCOMPARE(QProcess::execute("/bin/chmod", {"-N", vault.filePath()}), 0);
        QVERIFY(!opened);
        QCOMPARE(read(vault.filePath()), before);
        QVERIFY2(vault.unlock(Password, &error), qPrintable(error));
        vault.lock();
        QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "everyone allow read", dir.path()}), 0);
        const bool prepared = Vault::prepareProfileDirectory(dir.path(), &error);
        QCOMPARE(QProcess::execute("/bin/chmod", {"-N", dir.path()}), 0);
        QVERIFY(!prepared);
#else
        QSKIP("macOS extended ACL regression test.");
#endif
    }

    void passwordBoundsAndExactUtf8() {
        QString error;
        QVERIFY(!Vault::validateNewPassword("123456", &error));
        QVERIFY(!Vault::validateNewPassword(QString(400, QChar(0xac00)), &error));
        QVERIFY(!Vault::validateNewPassword(QString(1025, 'x'), &error));
        QVERIFY(!Vault::validateNewPassword(QString(12, QChar(0xd800)), &error));
        const QString spaced = QStringLiteral("  Spaced vault Passphrase1!  ");
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QVERIFY(vault.create(spaced, &error));
        vault.lock();
        QVERIFY(!vault.unlock(spaced.trimmed(), &error));
        QVERIFY(vault.unlock(spaced, &error));
    }

    void newPasswordComposition_data() {
        QTest::addColumn<QString>("password");
        QTest::addColumn<QString>("missing");
        QTest::newRow("eleven-characters") << QStringLiteral("Abcdefghi1!") << QStringLiteral("12자 이상");
        QTest::newRow("uppercase-missing") << QStringLiteral("abcdefghij1!") << QStringLiteral("대문자(A-Z)");
        QTest::newRow("lowercase-missing") << QStringLiteral("ABCDEFGHIJ1!") << QStringLiteral("소문자(a-z)");
        QTest::newRow("digit-missing") << QStringLiteral("Abcdefghijk!") << QStringLiteral("숫자(0-9)");
        QTest::newRow("punctuation-missing") << QStringLiteral("Abcdefghijk1") << QStringLiteral("특수문자");
        QTest::newRow("space-not-punctuation") << QStringLiteral("Abcdefghij1 ") << QStringLiteral("특수문자");
        QTest::newRow("tab-not-punctuation") << QStringLiteral("Abcdefghij1\t") << QStringLiteral("영문 입력 상태");
        QTest::newRow("unicode-symbol-not-ascii") << QStringLiteral("Abcdefghij1！") << QStringLiteral("영문 입력 상태");
        QTest::newRow("unicode-uppercase-not-ascii") << QStringLiteral("Àbcdefghij1!") << QStringLiteral("영문 입력 상태");
        QTest::newRow("unicode-lowercase-not-ascii") << QStringLiteral("ABCDEFGHIJ1!é") << QStringLiteral("영문 입력 상태");
        QTest::newRow("unicode-digit-not-ascii") << QStringLiteral("Abcdefghijk!١") << QStringLiteral("영문 입력 상태");
    }
    void newPasswordComposition() {
        QFETCH(QString, password); QFETCH(QString, missing);
        QString error;
        QVERIFY(!Vault::validateNewPassword(password, &error));
        QVERIFY2(error.contains(missing), qPrintable(error));
        QTemporaryDir dir;
        Vault vault(dir.filePath("not-created.bin"));
        QVERIFY(!vault.create(password, &error));
        QVERIFY(!vault.exists()); QVERIFY(!vault.isUnlocked());
    }
    void asciiPunctuationAndUnicodeBounds() {
        QString error;
        QVERIFY(Vault::validateNewPassword(QStringLiteral("Abcdefghij1!"), &error));
        QVERIFY(error.isEmpty());
        const QString punctuation = QStringLiteral("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
        QCOMPARE(punctuation.size(), 32);
        for (const auto character : punctuation) {
            QVERIFY(Vault::validateNewPassword(QStringLiteral("Abcdefghij1") + character, &error));
            QVERIFY(error.isEmpty());
        }
        QVERIFY(Vault::validateNewPassword(QStringLiteral("Abcdefghij1 ~"), &error)); // U+0020 and U+007E bounds
        // Hangul syllables and jamo, full-width, emoji and controls are all rejected.
        for (const auto& rejected : {QStringLiteral("Abcdefghij1!가"), QStringLiteral("Abcdefghij1!ㄱ"),
                                     QStringLiteral("Abcdefghij1！a"), QStringLiteral("Abcdefghij1!😀"),
                                     QStringLiteral("Abcdefghij1!\n"), QStringLiteral("Abcdefghij1!\u007f"),
                                     QStringLiteral("Abcdefghij1!\u00a0")}) {
            QVERIFY(!Vault::validateNewPassword(rejected, &error));
            QVERIFY2(error.contains(QStringLiteral("영문 입력 상태")), qPrintable(error));
        }
        const QString exactBytes = QStringLiteral("Az1!") + QString(1020, QChar('x'));
        QCOMPARE(exactBytes.toUtf8().size(), Vault::MaxPasswordBytes);
        QVERIFY(Vault::validateNewPassword(exactBytes, &error));
        QVERIFY(!Vault::validateNewPassword(exactBytes + QChar('x'), &error));
        QVERIFY(error.contains(QStringLiteral("1024바이트")));
    }

    void hangulLegacyPasswordUnlocksAndChangesToAscii() {
        QTemporaryDir dir;
        const auto path = dir.filePath("hangul-vault.bin");
        Vault vault(path); QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(record(), credentials(), StorageMode::EncryptedVault));
        vault.lock();
        const auto legacy = rewriteFakeMetadata(read(path), [](QJsonObject&) {}, LegacyHangulPassword);
        QVERIFY(!legacy.isEmpty()); QVERIFY(write(path, legacy));
        QString error;
        QVERIFY(!Vault::validateNewPassword(LegacyHangulPassword, &error));
        // The existing password is never checked against the new character rule.
        QVERIFY(!vault.unlock(QStringLiteral("hangul Passphrase1!"), &error)); QCOMPARE(read(path), legacy);
        QVERIFY(vault.unlock(LegacyHangulPassword, &error)); QCOMPARE(vault.accounts().size(), 1);
        // A non-ASCII new password is refused before anything is written.
        QVERIFY(!vault.changePassword(LegacyHangulPassword, QStringLiteral("새 Different Passphrase1!"), &error));
        QVERIFY(error.contains(QStringLiteral("영문 입력 상태"))); QVERIFY(vault.isUnlocked()); QCOMPARE(read(path), legacy);
        // A wrong current password locks the vault and keeps the file intact.
        QVERIFY(!vault.changePassword(QStringLiteral("wrong 한글 Passphrase1!"), NewPassword, &error));
        QVERIFY(!vault.isUnlocked()); QCOMPARE(read(path), legacy);
        QVERIFY(vault.unlock(LegacyHangulPassword, &error));
        QVERIFY(vault.changePassword(LegacyHangulPassword, NewPassword, &error));
        vault.lock(); QVERIFY(!vault.unlock(LegacyHangulPassword)); QVERIFY(vault.unlock(NewPassword));
        QCOMPARE(vault.accounts().size(), 1);
        bool matched = false;
        QVERIFY(vault.withCredentials(vault.accounts().first().id, [&](const CredentialSet& values) {
            matched = equalSecret(values, "secret", FakeSecret);
        }));
        QVERIFY(matched);
    }

    void schemaAndCredentialBounds() {
        QTemporaryDir dir;
        Vault vault(dir.filePath("vault.bin"));
        QString error;
        QVERIFY(vault.create(Password, &error));
        const auto initial = read(vault.filePath());
        auto r = record();
        auto fields = credentials();
        fields.insert("password", "NOT_ALLOWED_FAKE_LOGIN_PASSWORD");
        QVERIFY(!vault.addAccount(r, fields, StorageMode::EncryptedVault, &error));
        fields = credentials();
        fields["secret"] = QString(8193, 'x');
        QVERIFY(!vault.addAccount(r, fields, StorageMode::EncryptedVault, &error));
        fields = credentials();
        fields.remove("secret");
        QVERIFY(!vault.addAccount(r, fields, StorageMode::EncryptedVault, &error));
        r.readOnly = false;
        QVERIFY(!vault.addAccount(r, credentials(), StorageMode::EncryptedVault, &error));
        r = record();
        r.market = "unsupported_market";
        QVERIFY(!vault.addAccount(r, credentials(), StorageMode::EncryptedVault, &error));
        QVERIFY(vault.accounts().isEmpty());
        QCOMPARE(read(vault.filePath()), initial);
    }
};

} // namespace keefi

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 3 && args.at(1) == "--mlock-failure-child") {
#ifdef Q_OS_UNIX
        const rlimit noLockedMemory{0, 0};
        if (::setrlimit(RLIMIT_MEMLOCK, &noLockedMemory) != 0) return 10;
        bool refused = false;
        try { keefi::SecureBuffer forbidden(32); }
        catch (const std::exception& error) { refused = std::strcmp(error.what(), "SECURE_MEMORY_LOCK_FAILED") == 0; }
        if (!refused) return 11;
        keefi::Vault vault(args.at(2));
        QString error;
        if (vault.create(QStringLiteral("Fake child passphrase 2026!"), &error) || vault.isUnlocked() ||
            error.isEmpty() || QFileInfo::exists(args.at(2))) return 12;
        if (vault.addAccount(keefi::record(), keefi::credentials(), keefi::StorageMode::EncryptedVault, &error) ||
            !vault.accounts().isEmpty()) return 13;
        return 0;
#else
        return 14;
#endif
    }
    keefi::VaultTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_vault.moc"
