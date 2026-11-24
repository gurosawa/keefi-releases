#include "keefi/AccountModel.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalWindow.h"
#include "keefi/PlatformSecurity.h"
#include "keefi/ProfileMigration.h"
#include "keefi/ProfileSettings.h"
#include "keefi/ValuationPreferences.h"
#include "keefi/Vault.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace keefi;

namespace {
QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool writeBytes(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
        file.write(data) == data.size() && file.flush();
}
const ProfileSettingsIdentity Legacy{"FINIX", "FINIX Dev"};
const ProfileSettingsIdentity Current{"KEEFI", "KEEFI Dev"};
}

class ProfileSettingsTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> root_;
    QString legacyPath() const { return root_->filePath("user/FINIX/FINIX Dev.ini"); }
    QString currentPath() const { return root_->filePath("user/KEEFI/KEEFI Dev.ini"); }

  private slots:
    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); }
    void init() {
        root_ = std::make_unique<QTemporaryDir>();
        QVERIFY(root_->isValid());
        setProfileSettingsIdentity(std::nullopt);
        QCoreApplication::setOrganizationName("KEEFI");
        QCoreApplication::setOrganizationDomain({});
        QCoreApplication::setApplicationName("KEEFI Dev");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root_->filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root_->filePath("system"));
        KeefiTheme::instance()->setSettings(nullptr);
    }
    void cleanup() {
        KeefiTheme::instance()->setSettings(nullptr);
        setProfileSettingsIdentity(std::nullopt);
        QCoreApplication::setOrganizationDomain({});
    }

    void absentIdentityMatchesQtDefault_data() {
        QTest::addColumn<QString>("domain");
        QTest::newRow("organization-name") << QString{};
        QTest::newRow("organization-domain") << QString("synthetic.example.invalid");
    }
    void absentIdentityMatchesQtDefault() {
        QFETCH(QString, domain);
        QCoreApplication::setOrganizationName("Synthetic Default Organization");
        QCoreApplication::setApplicationName("Synthetic Default Application");
        QCoreApplication::setOrganizationDomain(domain);
        QObject owner;
        QSettings expected;
        expected.setValue("unknown/value", "FINIX is an unchanged value");
        expected.sync();
        QCOMPARE(expected.status(), QSettings::NoError);
        ProfileSettings actual(&owner);
        QCOMPARE(actual.fileName(), expected.fileName());
        QCOMPARE(actual.organizationName(), expected.organizationName());
        QCOMPARE(actual.applicationName(), expected.applicationName());
        QCOMPARE(actual.format(), expected.format());
        QCOMPARE(actual.scope(), expected.scope());
        QCOMPARE(actual.fallbacksEnabled(), expected.fallbacksEnabled());
        QCOMPARE(actual.parent(), &owner);
        QCOMPARE(actual.allKeys(), expected.allKeys());
        QCOMPARE(actual.value("unknown/value"), expected.value("unknown/value"));
    }

    void legacyAliasReadsRawIniAndFallbackWithoutRewriting() {
        const QByteArray app("; preserve comment and CRLF\r\n[General]\r\nchoice=old\r\nunknown=FINIX\r\n[rows]\r\nsize=1\r\n1\\name=synthetic\r\n");
        const QByteArray org("[General]\r\nfallback=legacy-only\r\n");
        const QByteArray other("[General]\nchoice=current\n");
        QVERIFY(writeBytes(legacyPath(), app));
        QVERIFY(writeBytes(root_->filePath("user/FINIX.ini"), org));
        QVERIFY(writeBytes(currentPath(), other));
        setProfileSettingsIdentity(Legacy);
        {
            ProfileSettings settings;
            QCOMPARE(settings.format(), QSettings::IniFormat);
            QCOMPARE(settings.fileName(), legacyPath());
            QCOMPARE(settings.value("choice").toString(), QString("old"));
            QCOMPARE(settings.value("unknown").toString(), QString("FINIX"));
            QCOMPARE(settings.value("fallback").toString(), QString("legacy-only"));
            QCOMPARE(settings.beginReadArray("rows"), 1);
            settings.setArrayIndex(0);
            QCOMPARE(settings.value("name").toString(), QString("synthetic"));
            settings.endArray();
        }
        QCOMPARE(QCoreApplication::organizationName(), QString("KEEFI"));
        QCOMPARE(QCoreApplication::applicationName(), QString("KEEFI Dev"));
        QCOMPARE(bytes(legacyPath()), app);
        QCOMPARE(bytes(root_->filePath("user/FINIX.ini")), org);
        QCOMPARE(bytes(currentPath()), other);
    }

    void identityChangesOnlyNewObjects() {
        setProfileSettingsIdentity(Legacy);
        ProfileSettings old;
        setProfileSettingsIdentity(Current);
        ProfileSettings current;
        QCOMPARE(old.fileName(), legacyPath());
        QCOMPARE(current.fileName(), currentPath());
        setProfileSettingsIdentity(std::nullopt);
        QCoreApplication::setApplicationName("Synthetic Later Application");
        ProfileSettings later;
        QSettings expected;
        QCOMPARE(later.fileName(), expected.fileName());
        QCOMPARE(old.fileName(), legacyPath());
        QCOMPARE(current.fileName(), currentPath());
    }

    void normalWritesStayInTheChosenIdentity_data() {
        QTest::addColumn<bool>("legacy");
        QTest::newRow("legacy") << true;
        QTest::newRow("current") << false;
    }
    void normalWritesStayInTheChosenIdentity() {
        QFETCH(bool, legacy);
        const QByteArray old("[General]\nunknown=FINIX\ncanary=old\n");
        const QByteArray current("[General]\nunknown=KEEFI\ncanary=current\n");
        QVERIFY(writeBytes(legacyPath(), old));
        QVERIFY(writeBytes(currentPath(), current));
        setProfileSettingsIdentity(legacy ? Legacy : Current);
        {
            ProfileSettings settings;
            QCOMPARE(settings.fileName(), legacy ? legacyPath() : currentPath());
            settings.setValue("user/chosen", 99);
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
            QCOMPARE(settings.value("unknown").toString(), legacy ? QString("FINIX") : QString("KEEFI"));
        }
        QCOMPARE(bytes(legacy ? currentPath() : legacyPath()), legacy ? current : old);
        QSettings selected(legacy ? legacyPath() : currentPath(), QSettings::IniFormat);
        QCOMPARE(selected.value("user/chosen").toInt(), 99);
        QSettings unchanged(legacy ? currentPath() : legacyPath(), QSettings::IniFormat);
        QVERIFY(!unchanged.contains("user/chosen"));
    }

    void copiedVaultUsesTheExistingWindowLockConnection() {
        const auto base = root_->filePath("migration");
        QVERIFY(QDir().mkpath(base));
        const DevProfilePaths paths{base, QDir(base).filePath("FINIX/FINIX Dev"),
            QDir(base).filePath("KEEFI/KEEFI Dev"), QDir(base).filePath("KEEFI/.keefi-dev-profile-copy")};
        const auto original = bytes(QString::fromUtf8(KEEFI_DEV99_PROFILE_FIXTURE));
        QCOMPARE(original.size(), qsizetype(760));
        const auto oldVault = QDir(paths.legacyDirectory).filePath("credentials.finix-vault");
        QVERIFY(writeBytes(oldVault, original));
        QVERIFY(QFile::setPermissions(paths.legacyDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::setPermissions(oldVault, QFile::ReadOwner | QFile::WriteOwner));
        ProfileMigrationPlan plan;
        plan.action = DevProfileAction::OfferCopy;
        plan.paths = paths;
        plan.selectedDirectory = paths.legacyDirectory;
        plan.settings = Legacy;
        const auto copied = applyDevProfileChoice(plan, ProfileMigrationChoice::Copy, "100");
        QVERIFY2(copied.copied, qPrintable(copied.error));
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, copied.directory);
        setProfileSettingsIdentity(copied.settings);
        Vault vault(QDir(copied.directory).filePath("credentials.finix-vault"));
        AccountModel accounts(&vault);
        QString error;
        QVERIFY2(vault.unlock("FINIX synthetic Dev99 password 2026!", &error), qPrintable(error));
        QCOMPARE(accounts.rowCount(), 1);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.show();
        QTRY_VERIFY(window.isVisible());
        auto* replace = window.findChild<QPushButton*>("replaceCredentials");
        QVERIFY(replace);
        PlatformSecurity security;
        connect(&security, &PlatformSecurity::lockRequested, &window, [&] { window.lockAndClear(); });
        QSignalSpy requested(&security, &PlatformSecurity::lockRequested);
        QSignalSpy locked(&vault, &Vault::locked);
        // Same application connection as main; this does not claim a physical OS screen lock.
        security.requestLock("session_inactive");
        QCOMPARE(requested.size(), 1);
        QCOMPARE(locked.size(), 1);
        QCOMPARE(accounts.rowCount(), 0);
        QVERIFY(!vault.isUnlocked());
        QVERIFY(!replace->isEnabled());
        QVERIFY(vault.accounts().isEmpty());
        bool borrowedKey = false;
        QVERIFY(!vault.withAssetsLedgerKey(false, [&](const SecureBuffer&) { borrowedKey = true; }, &error));
        QVERIFY(!borrowedKey);
        QCOMPARE(bytes(oldVault), original);
        QCOMPARE(bytes(vault.filePath()), original);
    }

    void existingConsumersUseTheSelectedChannel_data() {
        QTest::addColumn<QString>("consumer");
        for (const auto* name : {"watch-read", "watch-write", "local-window", "valuation"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void existingConsumersUseTheSelectedChannel() {
        QFETCH(QString, consumer);
        {
            QSettings old(QSettings::IniFormat, QSettings::UserScope, Legacy.organization, Legacy.application);
            old.setValue("explore/watchlist/coin", QStringList{"KRW-BTC"});
            old.sync();
            QCOMPARE(old.status(), QSettings::NoError);
            QSettings current(QSettings::IniFormat, QSettings::UserScope, Current.organization, Current.application);
            current.setValue("explore/watchlist/coin", QStringList{"KRW-ETH"});
            current.sync();
            QCOMPARE(current.status(), QSettings::NoError);
        }
        const auto other = bytes(currentPath());
        setProfileSettingsIdentity(Legacy);
        if (consumer.startsWith("watch")) {
            ExploreWatchlist watch;
            if (consumer == "watch-read") QCOMPARE(watch.ids("coin"), QStringList{"KRW-BTC"});
            else {
                watch.toggle("coin", "KRW-SYN");
                QSettings old(legacyPath(), QSettings::IniFormat);
                old.sync();
                QCOMPARE(old.value("explore/watchlist/coin").toStringList(), QStringList({"KRW-BTC", "KRW-SYN"}));
            }
        } else if (consumer == "local-window") {
            Vault vault(root_->filePath("profile/credentials.finix-vault"));
            AccountModel accounts(&vault);
            LocalWindow window(vault, accounts);
            const auto settings = window.findChildren<QSettings*>();
            QVERIFY(!settings.isEmpty());
            for (const auto* item : settings) QCOMPARE(item->fileName(), legacyPath());
            QCOMPARE(accounts.rowCount(), 0);
            QVERIFY(!vault.exists());
        } else {
            const auto profile = root_->filePath("profile");
            QString error;
            QVERIFY2(Vault::prepareProfileDirectory(profile, &error), qPrintable(error));
            const auto path = QDir(profile).filePath("credentials.finix-vault");
            QVERIFY(QFile::copy(QString::fromUtf8(KEEFI_DEV99_PROFILE_FIXTURE), path));
            QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner));
            Vault vault(path);
            QVERIFY2(vault.unlock("FINIX synthetic Dev99 password 2026!", &error), qPrintable(error));
            ValuationPreferences preferences(vault);
            const QString asset = "solana:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
            QVERIFY2(preferences.setExcluded(asset, true, &error), qPrintable(error));
            QSettings old(legacyPath(), QSettings::IniFormat);
            old.sync();
            QStringList preferenceKeys;
            for (const auto& key : old.allKeys())
                if (key.startsWith("valuation/excludedMints/")) preferenceKeys.append(key);
            QCOMPARE(preferenceKeys.size(), 1);
            QCOMPARE(old.value(preferenceKeys.first()).toStringList(), QStringList{asset.mid(7)});
            vault.lock();
            QVERIFY(preferences.excludedAssets().isEmpty());
        }
        QCOMPARE(bytes(currentPath()), other);
        QCOMPARE(QCoreApplication::applicationName(), QString("KEEFI Dev"));
        QCOMPARE(QCoreApplication::organizationName(), QString("KEEFI"));
    }
};

QTEST_MAIN(ProfileSettingsTest)
#include "tst_profile_settings.moc"
