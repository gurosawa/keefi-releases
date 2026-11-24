#include "keefi/AccountModel.h"
#include "keefi/AssetsWindow.h"
#include "keefi/BackgroundTray.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/ChartDrawings.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LocalWindow.h"
#include "keefi/RecordTransferDialog.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/VaultGate.h"
#include "keefi/VaultMark.h"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <memory>

using namespace keefi;

#ifndef KEEFI_DEV99_PROFILE_FIXTURE
#error KEEFI_DEV99_PROFILE_FIXTURE must identify the preserved synthetic Dev99 vault
#endif

namespace {
class IdentityTray final : public BackgroundTray {
  public:
    QPointer<QMenu> menu;
    bool shown = false;
    bool available() const override { return true; }
    bool visible() const override { return shown; }
    void setMenu(QMenu* value) override { menu = value; }
    void show() override { shown = true; }
    void hide() override { shown = false; }
};

class IdentityTransport final : public TradingTransport {
  public:
    int requests = 0;
    void send(quint64, const RequestSpec&) override { ++requests; }
    void abort() override {}
};
}

class ProductIdentityTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;

  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Product Identity");
        QCoreApplication::setApplicationName("KEEFI Dev");
        QGuiApplication::setApplicationDisplayName("KEEFI Dev");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.filePath("system"));
        QVERIFY(KeefiFonts::registerBundledFonts());
    }

    void init() {
        KeefiTheme::instance()->setSettings(nullptr);
        QSettings settings;
        settings.clear();
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        KeefiTheme::instance()->setMode(ThemeMode::Light);
    }

    void cleanup() { KeefiTheme::instance()->setSettings(nullptr); }

    void renamedLiquidationTitlePreservesTheReviewedResourceBytes() {
        const auto plain = ChartPhrasePack::plainJson();
        QVERIFY(!plain.isEmpty());
        QFile resource(":/keefi/phrases/liquidation-serious.json");
        QVERIFY(resource.open(QIODevice::ReadOnly));
        const auto original = resource.readAll();
        const auto digest = QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex();
        QCOMPARE(digest, QByteArray("da608362cb3e221112bca82c7f8462125e43651027747d8e5543fa69b65e562c"));
        QVERIFY(original.contains(QStringLiteral("KEEFI 청산 경고").toUtf8()));
        ChartPhraseFacts facts;
        facts.symbol = "SYNUSDT";
        facts.source = "https://fapi.binance.com/fapi/v3/positionRisk";
        facts.currency = "USDT";
        facts.positionSide = "LONG";
        facts.observedAtMs = 1792627200000;
        facts.distancePercent = 5.0;
        ChartPhrasePack pack;
        for (bool drip : {false, true}) {
            const auto result = pack.render(ChartPhraseKind::Liquidation, facts, drip);
            QVERIFY2(result.available, qPrintable(result.reason));
            QVERIFY(result.serious);
            QCOMPARE(result.title, QStringLiteral("KEEFI 청산 경고"));
            QCOMPARE(result.packSha256, digest);
            QCOMPARE(result.phraseId, QStringLiteral("serious_liquidation"));
            QVERIFY(result.body.contains("SYNUSDT LONG"));
            QVERIFY(result.body.contains("5%"));
            QVERIFY(result.body.contains("Binance Futures"));
        }
        QVERIFY(resource.seek(0));
        QCOMPARE(resource.readAll(), original);
        QCOMPARE(ChartPhrasePack::plainJson(), plain);
    }

    // BEGIN LEGACY_FORMAT_REGRESSION: preserve old format literals as import inputs.
    void previousPublicFormatsRestoreWithoutChangingSavedChoices_data() {
        QTest::addColumn<QString>("model");
        for(const auto* value:{"studies","drawings","movement","watchlists","observations","alert-ui"})
            QTest::newRow(value)<<QString::fromLatin1(value);
    }
    void previousPublicFormatsRestoreWithoutChangingSavedChoices() {
        QFETCH(QString,model);
        const auto previous=[](const QByteArray& current){
            auto object=QJsonDocument::fromJson(current).object();
            const auto key=object.contains("format")?QString("format"):QString("schema");
            auto value=object.value(key).toString();
            if(!value.startsWith("keefi-"))return QByteArray{};
            value.replace(0,6,"finix-");object.insert(key,value);
            return QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        QString error;QByteArray before,legacy,after;bool restored=false;
        if(model=="studies"){
            ChartStudyPreferences original,copy;QVERIFY(original.setEnabled("cloud",false));
            QVERIFY(original.setVisible("whales",false));QVERIFY(original.setChartStyle("line"));
            original.markIntroSeen();before=original.publicState();legacy=previous(before);
            restored=copy.restorePublicState(legacy,&error);after=copy.publicState();
        }else if(model=="drawings"){
            ChartDrawings original,copy;original.setMagnetEnabled(true);before=original.publicState();legacy=previous(before);
            // KEEFI added drawing schema /2 later; FINIX only ever wrote /1. With no shapes the two
            // documents differ only in the schema, so the real FINIX input is this one.
            QVERIFY(legacy.contains("\"finix-chart-drawings/2\""));legacy.replace("\"finix-chart-drawings/2\"","\"finix-chart-drawings/1\"");
            restored=copy.restorePublicState(legacy,&error);after=copy.publicState();
        }else if(model=="movement"){
            MovementColorPolicy original,copy;original.setScheme(MovementScheme::KoreanRedBlue);
            before=original.publicState();legacy=previous(before);restored=copy.restorePublicState(legacy,&error);after=copy.publicState();
        }else if(model=="watchlists"){
            WatchlistWorkspaceList original,copy;const auto id=original.createList("FINIX is my own list label");QVERIFY(!id.isEmpty());
            before=original.publicState();legacy=previous(before);restored=copy.restorePublicState(legacy,&error);after=copy.publicState();
        }else if(model=="observations"){
            ChartAlertEngine original(nullptr),copy(nullptr);QVERIFY(original.setObservationEnabled("strong_reversal",true));
            before=original.observationSettingsState();legacy=previous(before);restored=copy.restoreObservationSettingsState(legacy);after=copy.observationSettingsState();
        }else{
            ChartAlertsPanel original(nullptr,nullptr,nullptr,nullptr),copy(nullptr,nullptr,nullptr,nullptr);
            original.setBackgroundEnabled(true);original.markCloseHintSeen();before=original.publicState();legacy=previous(before);
            restored=copy.restorePublicState(legacy,&error);after=copy.publicState();
        }
        QVERIFY(!legacy.isEmpty());QVERIFY2(restored,qPrintable(error));QCOMPARE(after,before);
    }
    void previouslyReviewedPhrasePackKeepsItsOriginalSha() {
        auto object=QJsonDocument::fromJson(ChartPhrasePack::plainJson()).object();
        object.insert("schema","finix-chart-phrases/1");
        const auto original=QJsonDocument(object).toJson(QJsonDocument::Compact);
        const auto sha=QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex();
        ChartPhrasePack pack;QString error;QVERIFY2(pack.activateReviewed(original,sha,&error),qPrintable(error));
        QCOMPARE(pack.importedSha256(),sha);
        QCOMPARE(QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex(),sha);
        QVERIFY(!pack.activateReviewed(original,QByteArray(64,'0'),&error));
    }
    // END LEGACY_FORMAT_REGRESSION

    void developmentWindowKeepsItsChannel_data() {
        QTest::addColumn<QString>("part");
        QTest::newRow("window-title") << QString("title");
        QTest::newRow("header-wordmark") << QString("brand");
        QTest::newRow("tray-open-action") << QString("tray");
    }

    void developmentWindowKeepsItsChannel() {
        QFETCH(QString, part);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        Vault vault(profile.filePath("credentials.keefi-vault"));
        AccountModel accounts(&vault);
        IdentityTray tray;
        LocalWindow window(vault, accounts, nullptr, &tray);
        window.resize(1440, 900);
        window.show();
        QTRY_VERIFY(window.isVisible());
        QCOMPARE(window.objectName(), QString("keefiLocalWindow"));
        QVERIFY(!vault.exists());
        QCOMPARE(accounts.rowCount(), 0);

        if (part == "title") {
            QCOMPARE(window.windowTitle(), QStringLiteral("KEEFI Dev · 자산과 계좌"));
        } else if (part == "brand") {
            auto* brand = window.findChild<QLabel*>("brandLabel");
            QVERIFY(brand && brand->isVisibleTo(&window));
            QCOMPARE(brand->text(), QStringLiteral("KEEFI Dev"));
        } else {
            QVERIFY(tray.menu);
            QStringList actions;
            for (auto* action : tray.menu->actions()) actions.append(action->text());
            QCOMPARE(actions, QStringList({"KEEFI Dev 열기", "알림 잠시 끄기", "드립 끄기",
                                           "금고 잠금", "", "앱 종료"}));
        }
    }

    void startupGateShowsTheKeefiWordmark() {
        VaultGate gate(false, {});
        gate.resize(1440, 900);
        gate.show();
        // The wordmark is drawn letter by letter; each letter stands for one password rule.
        auto* brand = gate.findChild<VaultMark*>("vaultGateBrand");
        QVERIFY(brand);
        QTRY_VERIFY(brand->isVisibleTo(&gate));
        QCOMPARE(brand->text(), QStringLiteral("KEEFI"));
        QCOMPARE(brand->accessibleName(), QStringLiteral("KEEFI"));
    }

    void logoAccessibilityUsesTheKeefiName() {
        LogoMark logo(36);
        logo.show();
        QTRY_VERIFY(logo.isVisible());
        QCOMPARE(logo.objectName(), QStringLiteral("logoMark"));
        auto* accessible = QAccessible::queryAccessibleInterface(&logo);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("KEEFI"));
    }

    void dev99VaultBytesStayCompatibleWithKeefi() {
        const auto fixturePath = QString::fromUtf8(KEEFI_DEV99_PROFILE_FIXTURE);
        const auto originalPermissions = QFile::permissions(fixturePath);
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QByteArray original = fixture.readAll();
        fixture.close();
        QCOMPARE(original.size(), qsizetype(760));
        QCOMPARE(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex(),
                 QByteArray("2e136e637980c02d9e3499b32452344faa2b722494e44a6ebf68eb24d01e85e6"));
        QCOMPARE(original.left(8), QByteArray("FINIXVLT"));

        // Git does not preserve owner-only modes. Only this private temporary
        // copy is opened by Vault; the historical artifact stays read-only.
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QString error;
        QVERIFY2(Vault::prepareProfileDirectory(profile.path(), &error), qPrintable(error));
        const auto path = profile.filePath("credentials.finix-vault");
        QFile copy(path);
        QVERIFY(copy.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QVERIFY(copy.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(copy.write(original), qint64(original.size()));
        QVERIFY(copy.flush());
        copy.close();
        const auto ciphertext = [](const QString& filePath) {
            QFile file(filePath);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        QCOMPARE(ciphertext(path), original);

        Vault vault(path);
        QVERIFY2(vault.unlock(QStringLiteral("FINIX synthetic Dev99 password 2026!"), &error), qPrintable(error));
        QVERIFY(vault.isUnlocked());
        QCOMPARE(vault.accounts().size(), 1);
        const auto account = vault.accounts().first();
        QCOMPARE(account.id, QStringLiteral("00000000-0000-4000-8000-000000000099"));
        QCOMPARE(account.alias, QStringLiteral("DEV99_SYNTHETIC_ACCOUNT_NOT_REAL"));
        QCOMPARE(account.provider, QStringLiteral("upbit"));
        QCOMPARE(account.environment, QStringLiteral("live"));
        QCOMPARE(account.market, QStringLiteral("spot"));
        QCOMPARE(account.currency, QStringLiteral("KRW"));
        QVERIFY(account.readOnly);
        QVERIFY(vault.hasAssetsLedgerKey());
        bool existingKey = false;
        QVERIFY2(vault.withAssetsLedgerKey(false, [&](const SecureBuffer& key) {
            existingKey = key.size() == 32;
        }, &error), qPrintable(error));
        QVERIFY(existingKey);
        QCOMPARE(ciphertext(path), original);
        vault.lock();
        QVERIFY(!vault.isUnlocked());
        QVERIFY(vault.accounts().isEmpty());
        QCOMPARE(ciphertext(path), original);
        QCOMPARE(ciphertext(fixturePath), original);
        QCOMPARE(QFile::permissions(fixturePath), originalPermissions);
    }

    void ownedDialogsRenameOnlyTheirVisibleTitle_data() {
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QString>("title");
        QTest::newRow("assets") << QString("assetsWindow") << QStringLiteral("KEEFI · 자산과 성과");
        QTest::newRow("record-transfer") << QString("recordTransferDialog") << QStringLiteral("KEEFI · 암호화 기록 이동");
    }

    void ownedDialogsRenameOnlyTheirVisibleTitle() {
        QFETCH(QString, objectName);
        QFETCH(QString, title);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        Vault vault(profile.filePath("credentials.keefi-vault"));
        IdentityTransport transport;
        std::unique_ptr<QDialog> dialog;
        if (objectName == "assetsWindow") dialog = std::make_unique<AssetsWindow>(vault, nullptr, &transport);
        else dialog = std::make_unique<RecordTransferDialog>(vault, nullptr, nullptr);
        dialog->show();
        QTRY_VERIFY(dialog->isVisible());
        QCOMPARE(dialog->objectName(), objectName);
        QCOMPARE(transport.requests, 0);
        QVERIFY(!vault.exists());
        QVERIFY(!vault.isUnlocked());
        QCOMPARE(dialog->windowTitle(), title);
    }

    void studyPickerKeepsInternalIdsWhileRenamingVisibleLabels_data() {
        QTest::addColumn<QString>("part");
        QTest::newRow("category-caption") << QString("category");
        QTest::newRow("tree-group") << QString("group");
    }

    void studyPickerKeepsInternalIdsWhileRenamingVisibleLabels() {
        QFETCH(QString, part);
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.resize(1280, 900);
        workspace.show();
        auto* watch = workspace.findChild<QPushButton*>("chartBrowserScope_watch");
        auto* details = workspace.findChild<QPushButton*>("chartDetailsToggle");
        QVERIFY(watch && details);
        QTest::mouseClick(watch, Qt::LeftButton);
        if (!details->isChecked()) QTest::mouseClick(details, Qt::LeftButton);
        auto* picker = workspace.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisibleTo(&workspace));
        QVERIFY(workspace.indicatorPreferences());
        const auto before = workspace.indicatorPreferences()->publicState();
        bool inspected = false;
        QString categoryId, caption, group;
        QTimer::singleShot(0, &workspace, [&] {
            auto* dialog = workspace.findChild<QDialog*>("chartStudyPickerDialog");
            if (!dialog || !dialog->isVisible()) return;
            QTimer::singleShot(1500, dialog, &QDialog::reject);
            auto* category = dialog->findChild<QComboBox*>("chartStudyCategory");
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            if (category && tree) {
                const int index = category->findData("keefi");
                if (index >= 0) {
                    categoryId = category->itemData(index).toString();
                    caption = category->itemText(index);
                }
                for (QTreeWidgetItemIterator item(tree); *item; ++item)
                    if ((*item)->data(0, Qt::UserRole).toString() == "cloud" && (*item)->parent())
                        group = (*item)->parent()->text(0);
                inspected = true;
            }
            dialog->reject();
        });
        QTimer::singleShot(2000, &workspace, [&] {
            if (auto* dialog = workspace.findChild<QDialog*>("chartStudyPickerDialog")) dialog->reject();
        });
        QTest::mouseClick(picker, Qt::LeftButton);
        QVERIFY(inspected);
        QCOMPARE(categoryId, QStringLiteral("keefi"));
        QVERIFY(workspace.indicatorPreferences()->publicState() == before);
        if (part == "category") QCOMPARE(caption, QStringLiteral("KEEFI Studies"));
        else QCOMPARE(group, QStringLiteral("KEEFI"));
    }
};

QTEST_MAIN(ProductIdentityTest)
#include "tst_product_identity.moc"
