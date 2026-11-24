#include "keefi/AccountDialog.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalComboBox.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QSystemTrayIcon>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

using namespace keefi;
namespace {
class NoNetwork final : public TradingTransport {
  public:
    int requests = 0;
    void send(quint64, const RequestSpec&) override { ++requests; }
    void abort() override {}
};
struct PopupResult {
    bool captured = false, visible = false;
    int boxWidth = 0, popupWidth = 0, viewportWidth = 0, requiredWidth = 0, minimumRowWidth = 0;
    int menuHint = -1;
    QColor windowColor, baseColor;
};
PopupResult inspectPopup(QComboBox* box, QWidget* owner, const QString& name) {
    PopupResult result;
    owner->setWindowTitle(QStringLiteral("KEEFI · 가짜 데이터 드롭다운 검증"));
    owner->show(); owner->activateWindow();
    QTest::qWait(80);
    // Complete this fake widget's first layout/paint before simulating a click.
    // QWidget::grab never captures other application windows or the desktop.
    owner->grab();
    QStyleOptionComboBox option; option.initFrom(box);
    result.menuHint = box->style()->styleHint(QStyle::SH_ComboBox_Popup, &option, box);
    QTimer::singleShot(100, owner, [&] {
        auto* view = box->view();
        auto* popup = view->window();
        const auto ownerImage = owner->grab();
        result.visible = view->isVisible();
        result.boxWidth = box->width();
        result.popupWidth = popup->width();
        result.viewportWidth = view->viewport()->width();
        result.minimumRowWidth = result.viewportWidth;
        for (int row = 0; row < box->count(); ++row) {
            const auto index = box->model()->index(row, box->modelColumn(), box->rootModelIndex());
            result.requiredWidth = qMax(result.requiredWidth, QFontMetrics(view->font()).horizontalAdvance(box->itemText(row)) + 24);
            result.minimumRowWidth = qMin(result.minimumRowWidth, view->visualRect(index).width());
        }
        result.windowColor = popup->palette().color(QPalette::Window);
        result.baseColor = view->palette().color(QPalette::Base);
        const auto artifactDirectory = qEnvironmentVariable("KEEFI_COMBO_ARTIFACT_DIR");
        if (!artifactDirectory.isEmpty()) {
            QDir().mkpath(artifactDirectory);
            ownerImage.save(QDir(artifactDirectory).filePath(name + "-dialog.png"));
            if (result.visible) popup->grab().save(QDir(artifactDirectory).filePath(name + "-popup.png"));
        }
        result.captured = true;
        box->hidePopup();
    });
    box->showPopup();
    for (int i = 0; !result.captured && i < 50; ++i) QTest::qWait(20);
    owner->hide();
    qInfo().noquote() << QJsonDocument(QJsonObject{{"case", name}, {"platform", QGuiApplication::platformName()},
        {"style", QApplication::style()->objectName()}, {"menu_hint", result.menuHint},
        {"box_width", result.boxWidth}, {"popup_width", result.popupWidth},
        {"viewport_width", result.viewportWidth}, {"required_text_width", result.requiredWidth},
        {"minimum_row_width", result.minimumRowWidth}, {"view_visible", result.visible},
        {"window_color", result.windowColor.name()}, {"base_color", result.baseColor.name()}}).toJson(QJsonDocument::Compact);
    return result;
}
}
class ComboPopupTest final : public QObject {
    Q_OBJECT
  private slots:
    void cleanup() { KeefiTheme::instance()->setMode(ThemeMode::System); }
    void popupFitsItsFieldAndText_data() {
        QTest::addColumn<QString>("control");
        QTest::addColumn<bool>("dark");
        // Short choices (environment, market, currency, side, type) are segments without a popup.
        // Services are chosen in the searchable wizard, not a popup.
        for (const bool dark : {false, true}) {
            const auto suffix = dark ? "-dark" : "-light";
            for (const auto* name : {"credentialSourceSelector"})
                QTest::newRow(qPrintable(QString::fromLatin1(name) + suffix)) << QString::fromLatin1(name) << dark;

        }
    }
    void popupFitsItsFieldAndText() {
        QFETCH(QString, control); QFETCH(bool, dark);
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        QTemporaryDir directory;
        Vault vault(QDir(directory.path()).filePath("nonexistent-fake-vault"));
        QVERIFY(vault.create("Fake-popup-vault-2026!"));
        {
            // Credential reuse is offered only for an unlocked persistent
            // vault with an eligible account in the other Binance market.
            AccountRecord source;
            source.provider = "binance"; source.alias = QStringLiteral("합성 선물 연결");
            source.environment = "live"; source.market = "usdm_futures"; source.currency = "USDT";
            QVERIFY(vault.addAccount(source, {{"api_key", "FAKE-POPUP-SOURCE"}, {"secret", "FAKE-POPUP-SOURCE-SECRET"}}, StorageMode::EncryptedVault));
        }
        AccountModel accounts(&vault);
        LocalWindow theme(vault, accounts);
        for (auto* tray : theme.findChildren<QSystemTrayIcon*>()) tray->hide();
        AccountDialog account(vault.isUnlocked(), &theme);
        account.setCredentialSources(vault.accounts());
        auto* provider = account.findChild<QComboBox*>("providerSelector");
        // Binance is already the model's default. Selecting the same index
        // cannot enter the form: follow the real service wizard instead.
        auto* binance = account.findChild<QAbstractButton*>("accountService_binance"); QVERIFY(binance);
        binance->click();
        QCOMPARE(provider->currentData().toString(), QStringLiteral("binance"));
        QCOMPARE(account.findChild<QStackedWidget*>("accountConnectionSteps")->currentIndex(), 1);
        QWidget* owner = &account;
        auto* box = owner->findChild<QComboBox*>(control);
        QVERIFY(box);
        QCOMPARE(box->count(), 2);
        QVERIFY(box->isVisibleTo(owner));
        const auto selectedValue = box->currentData();
        const auto result = inspectPopup(box, owner, control + (dark ? "-dark" : "-light"));
        QCOMPARE(box->currentData(), selectedValue);
        QVERIFY(result.captured);
        QVERIFY(result.visible);
        QVERIFY2(result.boxWidth > owner->width() / 2, "Account selector does not fill its form field");
        QVERIFY2(result.popupWidth >= result.boxWidth, "Popup is narrower than its input field");
        QVERIFY2(result.minimumRowWidth >= result.requiredWidth, "Popup rows clip their labels");
        QCOMPARE(result.menuHint, 0);
        QCOMPARE(result.windowColor, themeColors().input);
        QCOMPARE(result.baseColor, themeColors().input);
    }
    void openPopupChangesThemeWithoutChangingSelection() {
        KeefiTheme::instance()->setMode(ThemeMode::Light);
        QWidget owner;
        auto* layout = new QVBoxLayout(&owner);
        auto* box = new LocalComboBox(&owner);
        layout->addWidget(box);
        box->addItem(QStringLiteral("합성 선택 하나"), "one");
        box->addItem(QStringLiteral("합성 선택 둘"), "two");
        box->setCurrentIndex(1);
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        box->showPopup();
        QTRY_VERIFY(box->view()->isVisible());
        QCOMPARE(box->view()->window()->palette().color(QPalette::Window), themeColors().input);
        const auto previous = themeColors().input;
        KeefiTheme::instance()->setMode(ThemeMode::Dark);
        QCoreApplication::processEvents();
        QVERIFY(box->view()->isVisible());
        QVERIFY(themeColors().input != previous);
        QCOMPARE(box->view()->window()->palette().color(QPalette::Window), themeColors().input);
        QCOMPARE(box->view()->palette().color(QPalette::Base), themeColors().input);
        QCOMPARE(box->view()->palette().color(QPalette::Text), themeColors().text);
        QCOMPARE(box->currentData().toString(), QStringLiteral("two"));
        box->hidePopup();
    }
};
QTEST_MAIN(ComboPopupTest)
#include "tst_combo_popup.moc"
