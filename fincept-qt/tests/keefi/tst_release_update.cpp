#include "keefi/ReleaseUpdater.h"
#include "keefi/LocalWindow.h"
#include "keefi/Vault.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <sodium.h>
#include <cstring>

using namespace keefi;
namespace {
const QString Version = "0.18.1", Tag = "v0.18.1-rc.112", Platform = "windows-x64";
const QString ArchiveName = "KEEFI-0.18.1-112-windows-x64.zip";
const QByteArray Payload("Synthetic release contents\n");

struct SignedFixture {
    QByteArray key{crypto_sign_PUBLICKEYBYTES, '\0'};
    QByteArray secret{crypto_sign_SECRETKEYBYTES, '\0'};
    QByteArray manifest, sig;
    SignedFixture() {
        crypto_sign_keypair(reinterpret_cast<unsigned char*>(key.data()), reinterpret_cast<unsigned char*>(secret.data()));
        const QJsonObject asset{{"name", ArchiveName}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(Payload, QCryptographicHash::Sha256).toHex())},
                                {"size", Payload.size()}};
        manifest = QJsonDocument(QJsonObject{{"schema", 1}, {"version", Version}, {"build", 112}, {"tag", Tag},
                                {"assets", QJsonObject{{Platform, asset}}}}).toJson(QJsonDocument::Compact);
        sig.resize(crypto_sign_BYTES);
        crypto_sign_detached(reinterpret_cast<unsigned char*>(sig.data()), nullptr,
                             reinterpret_cast<const unsigned char*>(manifest.constData()), manifest.size(),
                             reinterpret_cast<const unsigned char*>(secret.constData()));
        sig = sig.toBase64();
    }
};

class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request, QByteArray data, QUrl redirect, QObject* parent)
        : QNetworkReply(parent), bytes_(std::move(data)) {
        setUrl(request.url());
        setRequest(request);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, redirect.isEmpty() ? 200 : 302);
        if (!redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, redirect);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] { if (!isFinished()) { emit readyRead(); if (!isFinished()) { setFinished(true); emit finished(); } } });
    }
    void abort() override { if (!isFinished()) { setFinished(true); emit finished(); } }
    qint64 bytesAvailable() const override { return bytes_.size() - pos_ + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char* dest, qint64 n) override {
        const auto amount = qMin(n, bytes_.size() - pos_);
        if (!amount) return -1;
        std::memcpy(dest, bytes_.constData() + pos_, amount);
        pos_ += amount;
        return amount;
    }
private:
    QByteArray bytes_;
    qint64 pos_ = 0;
};

class GitHub final : public QNetworkAccessManager {
public:
    SignedFixture data;
    QByteArray payload = Payload;
    QString redirectHost = "release-assets.githubusercontent.com";
    QStringList hosts;
protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request, QIODevice*) override {
        Q_ASSERT(op == GetOperation);
        const auto url = request.url();
        hosts << url.host();
        QByteArray bytes;
        QUrl redirect;
        if (url.host() == "api.github.com" && url.path() == "/repos/gurosawa/keefi-releases/releases") {
            const QJsonArray assets{QJsonObject{{"name", "keefi-update.json"}}, QJsonObject{{"name", "keefi-update.json.sig"}}};
            bytes = QJsonDocument(QJsonArray{QJsonObject{{"draft", false}, {"tag_name", Tag}, {"assets", assets}}}).toJson();
        } else if (url.fileName() == "keefi-update.json") bytes = data.manifest;
        else if (url.fileName() == "keefi-update.json.sig") bytes = data.sig;
        else if (url.fileName() == ArchiveName && url.host() == "github.com") {
            redirect = QUrl("https://" + redirectHost + "/object");
            bytes = "redirect page";
        } else if (url.host() == "release-assets.githubusercontent.com") bytes = payload;
        return new Reply(request, bytes, redirect, this);
    }
};
}

class ReleaseUpdateTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(sodium_init() >= 0);
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QCoreApplication::setOrganizationName("KeefiUpdateTest");
        QCoreApplication::setApplicationName("KeefiUpdateTest");
    }
    void manifestTrustAndRollback() {
        SignedFixture f;
        ReleaseUpdateAsset asset;
        QString error;
        const auto verify = [&](const QByteArray& manifest, const QByteArray& sig, const QString& tag, const QString& platform,
                                const QString& version, int build) {
            return verifyReleaseManifest(manifest, sig, f.key, tag, platform, version, build, &asset, &error);
        };
        QCOMPARE(verify(f.manifest, f.sig, Tag, Platform, "0.18.0", 111), UpdateEligibility::Available);
        QCOMPARE(asset.name, ArchiveName);
        QCOMPARE(verify(f.manifest, f.sig, Tag, Platform, Version, 112), UpdateEligibility::AlreadyCurrent);
        QCOMPARE(verify(f.manifest, f.sig, Tag, Platform, "0.18.2", 100), UpdateEligibility::AlreadyCurrent);
        QCOMPARE(verify(f.manifest, f.sig, Tag, Platform, "0.17.0", 113), UpdateEligibility::AlreadyCurrent);
        QCOMPARE(verify(f.manifest + " ", f.sig, Tag, Platform, "0.18.0", 111), UpdateEligibility::Invalid);
        QCOMPARE(verify(f.manifest, "invalid", Tag, Platform, "0.18.0", 111), UpdateEligibility::Invalid);
        SignedFixture differentKey;
        QCOMPARE(verify(f.manifest, differentKey.sig, Tag, Platform, "0.18.0", 111), UpdateEligibility::Invalid);
        QCOMPARE(verify(f.manifest, f.sig, "v0.18.1-rc.111", Platform, "0.18.0", 111), UpdateEligibility::Invalid);
        QCOMPARE(verify(f.manifest, f.sig, Tag, "macos-arm64", "0.18.0", 111), UpdateEligibility::Invalid);
        QVERIFY(asset.name.isEmpty());
    }
    void installerBoundary_data() {
        QTest::addColumn<QString>("fault");
        for (const char* name : {"none", "signature", "payload", "redirect", "flush"}) QTest::newRow(name) << QString(name);
    }
    void installerBoundary() {
        QFETCH(QString, fault);
        GitHub network;
        if (fault == "payload") network.payload[0] ^= 1;
        if (fault == "signature") network.data.manifest += " ";
        if (fault == "redirect") network.redirectHost = "example.org";
        ReleaseUpdater update("0.18.0", 111, Platform, &network, network.data.key);
        QTemporaryDir env;
        QVERIFY(env.isValid());
        Vault vault(env.filePath("profile/credentials.keefi-vault"));
        QString error;
        QVERIFY2(vault.create("Fixture password 2026!", &error), qPrintable(error));
        AccountRecord record;
        record.provider="kiwoom"; record.alias="Update fixture"; record.environment="live"; record.market="domestic_equity"; record.currency="KRW";
        QVERIFY2(vault.addAccount(record, {{"app_key","fixture-key"},{"app_secret","fixture-secret"}}, StorageMode::EncryptedVault, &error), qPrintable(error));
        const auto accountId=vault.accounts().first().id;
        AccountModel accounts(&vault);
        LocalWindow window(vault, accounts);
        const bool save = fault != "flush";
        window.setQuitPreparation([save](auto done) { done(save); });
        bool invoked = false, lockedAtInstall = false;
        window.setReleaseUpdater(&update, [&](QString* issue) {
            invoked = true;
            lockedAtInstall = !vault.isUnlocked();
            return verifyUpdateArchive(update.archivePath(), update.asset(), issue);
        });
        QSignalSpy quit(&window, &LocalWindow::quitRequested);
        window.showVaultGate();
        window.show();
        auto* gate = window.findChild<QPushButton*>("vaultGateUpdate");
        QVERIFY(gate && gate->isVisibleTo(&window));
        gate->click();
        auto* check = window.findChild<QPushButton*>("checkReleaseUpdate");
        auto* restart = window.findChild<QPushButton*>("restartForUpdate");
        QVERIFY(check && check->isEnabled() && restart && !restart->isEnabled());
        check->click();
        if (fault == "signature") {
            QTRY_COMPARE(update.state(), ReleaseUpdater::State::Failed);
            QVERIFY(!restart->isEnabled());
        } else {
            QTRY_VERIFY(restart->isEnabled());
            restart->click();
            if (fault == "none") {
                QTRY_COMPARE(quit.count(), 1);
                QVERIFY(invoked && lockedAtInstall && !vault.isUnlocked());
                QVERIFY(vault.unlock("Fixture password 2026!", &error));
                QCOMPARE(vault.accounts().size(), 1);
                QCOMPARE(vault.accounts().first().id, accountId);
                // The test observer stands in for the detached installer, including its cleanup.
                QVERIFY(QDir(QFileInfo(update.archivePath()).absolutePath()).removeRecursively());
                return;
            }
            QTRY_COMPARE(update.state(), ReleaseUpdater::State::Failed);
        }
        QVERIFY(!invoked && !quit.count() && vault.isUnlocked());
        QVERIFY(!network.hosts.contains("example.org"));
    }
};
QTEST_MAIN(ReleaseUpdateTest)
#include "tst_release_update.moc"
