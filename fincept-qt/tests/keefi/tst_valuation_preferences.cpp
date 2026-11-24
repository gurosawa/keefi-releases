#include "keefi/ValuationPreferences.h"
#include "keefi/Vault.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;
namespace {
const QString Password = "Fake Preferences Vault! 2026";
const QString Usdc = "solana:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
const QString Skhy = "solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3";
QByteArray bytes(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
}

class ValuationPreferencesTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("KEEFI Synthetic Preferences Tests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_->filePath("settings"));
        QCoreApplication::setApplicationName("Preferences Test");
    }
    void readingAnEmptyVaultNeverCreatesALedgerKey() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password));
        const auto original = bytes(vault.filePath());
        ValuationPreferences preferences(vault);
        QString error = "old error";
        QVERIFY2(preferences.load(&error), qPrintable(error));
        QVERIFY(error.isEmpty()); QVERIFY(preferences.excludedAssets().isEmpty());
        QVERIFY(preferences.setExcluded(Usdc, false, &error));
        QVERIFY(!vault.withAssetsLedgerKey(false, [](const auto&) {}));
        QCOMPARE(bytes(vault.filePath()), original);
        QVERIFY(QSettings().allKeys().isEmpty());
    }
    void choicesSurviveRestartAndUnlockButClearWhileLocked() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password));
        {
            ValuationPreferences preferences(vault); QSignalSpy changed(&preferences, &ValuationPreferences::changed);
            QString error;
            QVERIFY2(preferences.setExcluded(Usdc, true, &error), qPrintable(error));
            QCOMPARE(preferences.excludedAssets(), QSet<QString>{Usdc}); QCOMPARE(changed.count(), 1);
            QVERIFY(preferences.setExcluded(Usdc, true)); QCOMPARE(changed.count(), 1);
            vault.lock(); QVERIFY(preferences.excludedAssets().isEmpty()); QCOMPARE(changed.count(), 2);
            QVERIFY(!preferences.setExcluded(Skhy, true, &error)); QVERIFY(!error.isEmpty());
            QVERIFY(vault.unlock(Password)); QCOMPARE(preferences.excludedAssets(), QSet<QString>{Usdc});
        }
        ValuationPreferences reopened(vault); QCOMPARE(reopened.excludedAssets(), QSet<QString>{Usdc});
        QVERIFY(reopened.setExcluded(Usdc, false)); QVERIFY(reopened.excludedAssets().isEmpty());
        ValuationPreferences again(vault); QVERIFY(again.excludedAssets().isEmpty());
    }
    void differentAndRecreatedVaultsDoNotInheritChoices() {
        const auto path = dir_->filePath("first.bin");
        {
            Vault first(path); QVERIFY(first.create(Password)); ValuationPreferences firstChoice(first);
            QVERIFY(firstChoice.setExcluded(Usdc, true));
            Vault second(dir_->filePath("second.bin")); QVERIFY(second.create(Password)); ValuationPreferences secondChoice(second);
            QVERIFY(secondChoice.excludedAssets().isEmpty()); QVERIFY(secondChoice.setExcluded(Skhy, true));
            QCOMPARE(firstChoice.excludedAssets(), QSet<QString>{Usdc});
            QCOMPARE(secondChoice.excludedAssets(), QSet<QString>{Skhy});
        }
        QVERIFY(QFile::remove(path));
        Vault recreated(path); QVERIFY(recreated.create(Password)); ValuationPreferences choices(recreated);
        QVERIFY(choices.excludedAssets().isEmpty());
        QVERIFY(choices.setExcluded(Skhy, true)); QCOMPARE(choices.excludedAssets(), QSet<QString>{Skhy});
    }
    void applicationChannelsKeepTheirChoicesSeparate() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password));
        QCoreApplication::setApplicationName("Preferences Production Test");
        ValuationPreferences production(vault); QVERIFY(production.setExcluded(Usdc, true));
        QCoreApplication::setApplicationName("Preferences Development Test");
        ValuationPreferences development(vault); QVERIFY(development.excludedAssets().isEmpty());
        QVERIFY(development.setExcluded(Skhy, true));
        QCOMPARE(production.excludedAssets(), QSet<QString>{Usdc});
        QCoreApplication::setApplicationName("Preferences Production Test");
        ValuationPreferences reopened(vault); QCOMPARE(reopened.excludedAssets(), QSet<QString>{Usdc});
    }
    void onlyCanonicalMintIdsAreAccepted() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password));
        const auto original = bytes(vault.filePath()); ValuationPreferences preferences(vault);
        for (const auto& invalid : QStringList{"USDC", "exchange:USDC", "SOL", "solana:", "solana:0bad", Usdc + " ", " " + Usdc, "Solana:" + Usdc.mid(7)}) {
            QString error;
            QVERIFY(!preferences.setExcluded(invalid, true, &error)); QVERIFY(!error.isEmpty());
            QVERIFY(preferences.excludedAssets().isEmpty());
        }
        QCOMPARE(bytes(vault.filePath()), original); QVERIFY(QSettings().allKeys().isEmpty());
    }
    void storedValuesContainPublicMintsAndAnOpaqueBindingOnly() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password)); ValuationPreferences preferences(vault);
        QVERIFY(preferences.setExcluded(Usdc, true)); QVERIFY(preferences.setExcluded(Skhy, true));
        QSettings settings; settings.sync(); const auto keys = settings.allKeys(); QCOMPARE(keys.size(), 1);
        QVERIFY(keys.first().startsWith("valuation/excludedMints/"));
        const auto binding = keys.first().mid(QString("valuation/excludedMints/").size());
        QCOMPARE(binding.size(), 64);
        QVERIFY(!binding.contains(vault.filePath()) && !binding.contains(Password));
        QStringList expected{Usdc.mid(7), Skhy.mid(7)}; expected.sort();
        QCOMPARE(settings.value(keys.first()).toStringList(), expected);
        const auto stored = bytes(settings.fileName());
        QVERIFY(!stored.contains(Password.toUtf8())); QVERIFY(!stored.contains(vault.filePath().toUtf8()));
        QVERIFY(!stored.contains("api_key") && !stored.contains("quantity") && !stored.contains("wallet_owner"));
    }
    void invalidStoredChoicesFailClosedWithoutRewritingThem() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password)); ValuationPreferences preferences(vault);
        QVERIFY(preferences.setExcluded(Usdc, true)); QSettings settings; const auto key = settings.allKeys().first();
        const QStringList malformed{Usdc.mid(7), "not-a-mint"}; settings.setValue(key, malformed); settings.sync();
        QString error; QVERIFY(!preferences.load(&error)); QVERIFY(!error.isEmpty()); QVERIFY(preferences.excludedAssets().isEmpty());
        QCOMPARE(settings.value(key).toStringList(), malformed);
        settings.setValue(key, QStringList{Skhy.mid(7)}); settings.sync();
        QVERIFY2(preferences.load(&error), qPrintable(error)); QVERIFY(error.isEmpty()); QCOMPARE(preferences.excludedAssets(), QSet<QString>{Skhy});
    }
    void aSettingsWriteFailurePreservesTheConfirmedChoice() {
        Vault vault(dir_->filePath("vault.bin")); QVERIFY(vault.create(Password)); ValuationPreferences preferences(vault);
        QVERIFY(preferences.setExcluded(Usdc, true));
        const auto settingsDirectory = QFileInfo(QSettings().fileName()).absolutePath();
        const auto backup = settingsDirectory + ".backup";
        QVERIFY(QDir().rename(settingsDirectory, backup));
        QFile blocker(settingsDirectory); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("BLOCKED"); blocker.close();
        QString error; const bool saved = preferences.setExcluded(Skhy, true, &error);
        const auto current = preferences.excludedAssets();
        QVERIFY(QFile::remove(settingsDirectory)); QVERIFY(QDir().rename(backup, settingsDirectory));
        QVERIFY(!saved); QVERIFY(!error.isEmpty()); QCOMPARE(current, QSet<QString>{Usdc});
        ValuationPreferences reopened(vault); QVERIFY2(reopened.load(&error), qPrintable(error));
        QCOMPARE(reopened.excludedAssets(), QSet<QString>{Usdc});
        QVERIFY2(preferences.setExcluded(Skhy, true, &error), qPrintable(error));
        const QSet<QString> both{Usdc, Skhy}; QCOMPARE(preferences.excludedAssets(), both);
    }
  private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_GUILESS_MAIN(ValuationPreferencesTest)
#include "tst_valuation_preferences.moc"
