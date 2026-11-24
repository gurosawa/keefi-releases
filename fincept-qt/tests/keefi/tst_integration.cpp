#include "keefi/AccountModel.h"
#include "keefi/PlatformSecurity.h"
#include "keefi/ProfilePaths.h"
#include "keefi/SecretWorker.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

class IntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void existingLegacyProfileIsReusedWithoutMigration() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString legacy = root.filePath(QStringLiteral("KEEFI-G2"));
        const QString fallback = root.filePath(QStringLiteral("KEEFI/KEEFI"));
        QVERIFY(QDir().mkpath(legacy));
        QFile marker(QDir(legacy).filePath(QStringLiteral("fake-profile-marker")));
        const QByteArray contents("FAKE_PROFILE_CONTENTS_ONLY");
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(marker.write(contents), contents.size());
        marker.close();
        const auto rootEntries = QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        const auto legacyEntries = QDir(legacy).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);

        QCOMPARE(keefi::resolveProfileDirectory(std::nullopt, fallback, legacy), legacy);

        QVERIFY(!QFileInfo::exists(fallback));
        QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), rootEntries);
        QCOMPARE(QDir(legacy).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), legacyEntries);
        QVERIFY(marker.open(QIODevice::ReadOnly));
        QCOMPARE(marker.readAll(), contents);
    }

    void missingLegacyProfileKeepsDefaultWithoutCreatingDirectories() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString legacy = root.filePath(QStringLiteral("KEEFI-G2"));
        const QString fallback = root.filePath(QStringLiteral("KEEFI/KEEFI"));

        QCOMPARE(keefi::resolveProfileDirectory(std::nullopt, fallback, legacy), fallback);

        QVERIFY(!QFileInfo::exists(legacy));
        QVERIFY(!QFileInfo::exists(fallback));
        QVERIFY(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }

    void explicitProfileOverridesExistingLegacyWithoutCreatingDirectories() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString legacy = root.filePath(QStringLiteral("KEEFI-G2"));
        const QString fallback = root.filePath(QStringLiteral("KEEFI/KEEFI"));
        QVERIFY(QDir().mkpath(legacy));
        const QString requested = root.filePath(QStringLiteral("unused/../explicit-profile"));
        const QString expected = root.filePath(QStringLiteral("explicit-profile"));

        QCOMPARE(keefi::resolveProfileDirectory(std::optional<QString>{requested}, fallback, legacy), expected);

        QVERIFY(QFileInfo(legacy).isDir());
        QVERIFY(!QFileInfo::exists(expected));
        QVERIFY(!QFileInfo::exists(fallback));
        QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("KEEFI-G2")});
    }

    void developmentProfileSkipsProductionLegacyWithoutMigration() {
        QTemporaryDir root;QVERIFY(root.isValid());
        const QString legacy=root.filePath("KEEFI-G2"),development=root.filePath("KEEFI/KEEFI Dev");
        QVERIFY(QDir().mkpath(legacy));QFile marker(QDir(legacy).filePath("fake-production-marker"));
        QVERIFY(marker.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(marker.write("FAKE_PRODUCTION_PROFILE"),qint64(23));marker.close();
        QCOMPARE(keefi::resolveProfileDirectory(std::nullopt,development,legacy,false),development);
        QVERIFY(!QFileInfo::exists(development));QVERIFY(marker.open(QIODevice::ReadOnly));QCOMPARE(marker.readAll(),QByteArray("FAKE_PRODUCTION_PROFILE"));
        const auto name=QCoreApplication::applicationName();
        QCoreApplication::setApplicationName("KEEFI");const auto productionDefault=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QCoreApplication::setApplicationName("KEEFI Dev");const auto developmentDefault=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QCoreApplication::setApplicationName(name);QVERIFY(!productionDefault.isEmpty());QVERIFY(!developmentDefault.isEmpty());QVERIFY(productionDefault!=developmentDefault);
    }

    void vaultLockCancelsBorrowedSecretWorker() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        keefi::PlatformSecurity security;
        QVERIFY2(security.initialize(), qPrintable(security.statusCode()));
        keefi::Vault vault(profile.filePath("credentials.keefi-vault"));
        QString error;
        QVERIFY2(vault.create(QStringLiteral("KEEFI fake password for testing 2026!"), &error), qPrintable(error));
        keefi::AccountRecord record;
        record.provider = "upbit";
        record.alias = QStringLiteral("가짜 계좌");
        record.environment = "live";
        record.market = "spot";
        record.currency = "KRW";
        const QString marker = QStringLiteral("KEEFI_INTEGRATION_FAKE_NOT_A_REAL_KEY_7831");
        QVERIFY2(vault.addAccount(record, {{"access_key", marker}, {"secret", marker}},
                                 keefi::StorageMode::EncryptedVault, &error), qPrintable(error));
        const auto account = vault.accounts().first();
        QCOMPARE(account.connectionState, QStringLiteral("not_checked"));
        QCOMPARE(account.permissionState, QStringLiteral("unverified"));
        QVERIFY(account.readOnly);
        QFile source(QString::fromUtf8(KEEFI_SECURITY_FIXTURE));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray hash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
        keefi::SecretWorker worker(&security, QString::fromUtf8(KEEFI_TEST_PYTHON),
                                  QString::fromUtf8(KEEFI_SECURITY_FIXTURE), hash);
        connect(&vault, &keefi::Vault::aboutToLock, &worker, &keefi::SecretWorker::stop);
        QSignalSpy finished(&worker, &keefi::SecretWorker::finished);
        bool started = false;
        QVERIFY2(vault.withCredentials(account.id, [&](const keefi::CredentialSet& credentials) {
            const auto& bytes = credentials.at(QStringLiteral("secret"));
            // Fake data only: JSON is this fixture's protocol, not a production broker adapter.
            QJsonObject payload{{"secret", QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                                                            static_cast<qsizetype>(bytes.size()))},
                                {"mode", "hold"}};
            started = worker.start(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        }, &error), qPrintable(error));
        QVERIFY(started);
        QTRY_VERIFY_WITH_TIMEOUT(worker.processId() > 0, 2000);
        vault.lock();
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.at(0).at(1).toBool());
        QVERIFY(!worker.running());
        QVERIFY(!vault.isUnlocked());
        QVERIFY(vault.accounts().isEmpty());
        bool called = false;
        QVERIFY(!vault.withCredentials(account.id, [&](const auto&) { called = true; }, &error));
        QVERIFY(!called);
        QTest::qWait(350);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!worker.running());
        QDirIterator files(profile.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains(marker.toUtf8()));
        }
    }
};
QTEST_GUILESS_MAIN(IntegrationTest)
#include "tst_integration.moc"
