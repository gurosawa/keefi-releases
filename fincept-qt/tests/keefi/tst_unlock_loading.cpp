#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/KeefiFonts.h"
#include "keefi/PortfolioController.h"
#include "keefi/PressRevealButton.h"
#include "keefi/TradingTransport.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>

using namespace keefi;

namespace {
const QString Password = QStringLiteral("Fake Unlock Progress! 2026");

class PaintObserver final : public QObject {
  public:
    bool painted = false;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) painted = true;
        return false;
    }
};

// Production adapters parse these synthetic responses. Account delivery can be
// held after unlock; every request stays in-process and mutations are forbidden.
class PortfolioTransport final : public TradingTransport {
  public:
    explicit PortfolioTransport(bool prices) : prices_(prices) {}
    QList<QPair<quint64, HttpResult>> pending;
    bool hold = false;
    void abort() override { pending.clear(); }
    void release() {
        hold = false;
        const auto responses = pending;
        pending.clear();
        for (const auto& response : responses)
            QTimer::singleShot(0, this, [this, response] { emit finished(response.first, response.second); });
    }
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(!isMutation(request.operation));
        QVERIFY(NetworkTradingTransport::allowed(request));
        HttpResult response; response.status = 200;
        const auto now = QString::number(QDateTime::currentMSecsSinceEpoch());
        if (prices_) {
            QVERIFY(!request.authenticated);
            QVERIFY(request.headers.isEmpty());
            response.body = request.operation == Operation::Markets
                ? QJsonDocument(QJsonArray{QJsonObject{{"market", "KRW-USDT"}}}).toJson()
                : QJsonDocument(QJsonArray{QJsonObject{{"market", "KRW-USDT"}, {"trade_price", "1000"}, {"timestamp", now}}}).toJson();
        } else if (request.operation == Operation::Time) {
            response.body = QJsonDocument(QJsonObject{{"serverTime", now}}).toJson();
        } else {
            response.body = QJsonDocument(QJsonObject{{"canTrade", true}, {"accountType", "SPOT"},
                {"balances", QJsonArray{QJsonObject{{"asset", "USDT"}, {"free", "10"}, {"locked", "0"}}}}}).toJson();
        }
        if (hold && request.operation != Operation::Time) pending.append({token, response});
        else QTimer::singleShot(0, this, [this, token, response] { emit finished(token, response); });
    }
  private:
    bool prices_;
};
}

class UnlockLoadingTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("KEEFI Synthetic Unlock Tests");
        QCoreApplication::setApplicationName("Unlock Loading Test");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }

    void progressIsPaintedBeforePasswordWorkAndDuplicateSubmitIsBlocked() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin"));
        QVERIFY(vault.create(Password));
        vault.lock();

        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* input = dialog.findChild<QLineEdit*>("currentPassword");
        auto* submit = dialog.findChild<QPushButton*>("submitVault");
        auto* progress = dialog.findChild<QLabel*>("vaultProgress");
        auto* bar = dialog.findChild<QProgressBar*>("vaultProgressBar");
        QVERIFY(input); QVERIFY(submit);
        PaintObserver statusPaint, barPaint;
        if (progress) progress->installEventFilter(&statusPaint);
        if (bar) bar->installEventFilter(&barPaint);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        input->setText(Password);

        int submissions = 0;
        bool visibleBeforeWork = false, paintedBeforeWork = false, inputsBlocked = false, unlocked = false;
        QString error;
        connect(&dialog, &VaultDialog::submitted, &dialog, [&](const QString& current, const QString&) {
            if (++submissions != 1) return;
            visibleBeforeWork = progress && progress->isVisibleTo(&dialog) && !progress->text().isEmpty() &&
                bar && bar->isVisibleTo(&dialog) && bar->minimum() == 0 && bar->maximum() == 0;
            paintedBeforeWork = statusPaint.painted && barPaint.painted;
            inputsBlocked = !input->isEnabled() && !submit->isEnabled();
            // A second direct activation must also be rejected at the public button boundary.
            submit->click();
            unlocked = vault.unlock(current, &error);
            if (unlocked) dialog.accept();
            else dialog.setError(error);
        });
        submit->click();

        QVERIFY2(visibleBeforeWork, "Unlock must show an explicit busy status before the synchronous password KDF starts.");
        QVERIFY2(paintedBeforeWork, "The busy status and indeterminate bar must have painted before password work blocks the GUI thread.");
        QVERIFY(inputsBlocked);
        QCOMPARE(submissions, 1);
        QVERIFY2(unlocked, qPrintable(error));
        QVERIFY(input->text().isEmpty());
    }

    void operationErrorRestoresEditingWithoutRestoringSensitiveInputs() {
        for (const auto mode : {VaultDialog::Mode::Unlock, VaultDialog::Mode::Create, VaultDialog::Mode::ChangePassword}) {
            VaultDialog dialog(mode);
            auto* current = dialog.findChild<QLineEdit*>("currentPassword");
            auto* next = dialog.findChild<QLineEdit*>("newPassword");
            auto* confirm = dialog.findChild<QLineEdit*>("confirmPassword");
            auto* submit = dialog.findChild<QPushButton*>("submitVault");
            auto* cancel = dialog.findChild<QPushButton*>("cancelVault");
            auto* progress = dialog.findChild<QLabel*>("vaultProgress");
            auto* bar = dialog.findChild<QProgressBar*>("vaultProgressBar");
            QVERIFY(submit); QVERIFY(cancel); QVERIFY(progress); QVERIFY(bar);
            const auto idleText = submit->text();
            if (current) current->setText(Password);
            if (next) next->setText(Password);
            if (confirm) confirm->setText(Password);
            dialog.show();
            int submissions = 0;
            bool correctInputs = false;
            connect(&dialog, &VaultDialog::submitted, &dialog, [&](const QString& oldPassword, const QString& newPassword) {
                ++submissions;
                correctInputs = oldPassword == (current ? Password : QString()) && newPassword == (next ? Password : QString());
            });
            submit->click();
            QCOMPARE(submissions, 1);
            QVERIFY(correctInputs);
            QVERIFY(progress->isVisibleTo(&dialog)); QVERIFY(bar->isVisibleTo(&dialog));
            for (auto* input : {current, next, confirm}) if (input) {
                QVERIFY(!input->isEnabled()); QVERIFY(input->text().isEmpty());
                QCOMPARE(input->echoMode(), QLineEdit::Password);
            }
            for (auto* reveal : dialog.findChildren<PressRevealButton*>()) QVERIFY(!reveal->isEnabled());
            QVERIFY(!submit->isEnabled()); QVERIFY(!cancel->isEnabled());
            dialog.setError("SYNTHETIC_OPERATION_FAILURE");
            QVERIFY(progress->isHidden()); QVERIFY(bar->isHidden());
            QCOMPARE(dialog.findChild<QLabel*>("vaultError")->text(), QString("SYNTHETIC_OPERATION_FAILURE"));
            QCOMPARE(submit->text(), idleText);
            QVERIFY(cancel->isEnabled()); QVERIFY(!submit->isEnabled());
            for (auto* input : {current, next, confirm}) if (input) {
                QVERIFY(input->isEnabled()); QVERIFY(input->text().isEmpty());
            }
            for (auto* reveal : dialog.findChildren<PressRevealButton*>()) QVERIFY(reveal->isEnabled());
            if (current) current->setText(Password);
            if (next) next->setText(Password);
            if (confirm) confirm->setText(Password);
            QVERIFY(submit->isEnabled());
            submit->click();
            QCOMPARE(submissions, 2);
            dialog.reject();
            QVERIFY(progress->isHidden()); QVERIFY(bar->isHidden());
        }
    }

    void successfulUnlockHandsOffToPortfolioLoadingUntilReadsSettle() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin"));
        QVERIFY(vault.create(Password));
        AccountRecord account;
        account.provider = "binance"; account.alias = "SYNTHETIC UNLOCK LOADING";
        account.environment = "live"; account.market = "spot"; account.currency = "USDT";
        QVERIFY(vault.addAccount(account, {{"api_key", "FakeUnlockLoadingKey123"}, {"secret", "FakeUnlockLoadingSecret123"}}, StorageMode::EncryptedVault));
        vault.lock();
        PortfolioTransport assets(false);
        assets.hold = true;
        auto* prices = new PortfolioTransport(true);
        PortfolioController::Factories factories;
        factories.trading = [&](const AccountRecord&) { return &assets; };
        factories.prices = [prices] { return prices; };
        PortfolioController portfolio(vault, nullptr, factories);
        portfolio.setAutomaticRefresh(true);
        AccountModel accounts(&vault);
        LocalWindow window(vault, accounts);
        window.setPortfolioController(&portfolio);
        window.setSecurityStatus(true);
        window.resize(1200, 800);
        window.show();
        auto* open = window.findChild<QPushButton*>("unlockVault");
        QVERIFY(open);
        open->click();
        auto* dialog = window.findChild<VaultDialog*>("vaultDialog");
        QVERIFY(dialog);
        // A human cannot submit an unmounted dialog. Wait for the real window
        // boundary rather than activating both buttons in the same event turn.
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        const auto dialogPreview = qEnvironmentVariable("KEEFI_LOADING_PREVIEW_DIR");
        if (!dialogPreview.isEmpty()) {
            QVERIFY(QDir().mkpath(dialogPreview));
            QVERIFY(dialog->grab().save(QDir(dialogPreview).filePath("vault-unlock-themed.png")));
        }
        auto* progress = dialog->findChild<QLabel*>("vaultProgress");
        QVERIFY(progress);
        PaintObserver paint;
        progress->installEventFilter(&paint);
        dialog->findChild<QLineEdit*>("currentPassword")->setText(Password);
        dialog->findChild<QPushButton*>("submitVault")->click();
        QVERIFY(paint.painted);
        QVERIFY(vault.isUnlocked()); QVERIFY(!dialog->isVisible());
        auto* loading = window.findChild<QLabel*>("portfolioLoading");
        QVERIFY2(loading && loading->isVisibleTo(&window), "Closing the unlock dialog must hand off to the asset loader before the deferred account synchronization starts.");
        auto* total = window.findChild<QLabel*>("portfolioTotal");
        auto* tree = window.findChild<QTreeView*>("portfolioAssets");
        QVERIFY(loading); QVERIFY(total); QVERIFY(tree);
        QTRY_VERIFY_WITH_TIMEOUT(!assets.pending.isEmpty(), 2000);
        QVERIFY(loading->isVisibleTo(&window));
        QCOMPARE(total->text(), QString("—"));
        QVERIFY(!tree->isVisibleTo(&window));
        auto* bar = window.findChild<QProgressBar*>("portfolioLoadProgress");
        auto* stage = window.findChild<QLabel*>("portfolioLoadStage");
        auto* filters = window.findChild<QWidget*>("portfolioAssetFilter");
        auto* card = window.findChild<QWidget*>("portfolioLoadingCard");
        QVERIFY2(bar && bar->isVisibleTo(&window), "Initial asset reads show actual task progress.");
        QVERIFY(stage);QCOMPARE(stage->text(),QStringLiteral("잔고 불러오기"));
        QVERIFY(card && card->isVisibleTo(&window));
        QVERIFY(filters && !filters->isVisibleTo(&window));
        QVERIFY(!window.findChild<QWidget*>("portfolioClassFilter")->isVisibleTo(&window));
        const auto heldValue = bar->value();
        QTest::qWait(95);QCOMPARE(bar->value(),heldValue);
        // G8 labels retain the exact Pretendard Title17 role; held task values
        // stay fixed until real account/price responses arrive.
        const auto loadingTitle = KeefiFonts::font(FontRole::Title17);
        QCOMPARE(loading->font().pixelSize(), loadingTitle.pixelSize());
        QCOMPARE(loading->font().weight(), loadingTitle.weight());
        QCOMPARE(loading->font().family(), KeefiFonts::bodyFamily());
        QVERIFY(card->width() <= 540);
        const auto preview = qEnvironmentVariable("KEEFI_LOADING_PREVIEW_DIR");
        if (!preview.isEmpty()) {
            QVERIFY(QDir().mkpath(preview));
            QVERIFY(window.grab().save(QDir(preview).filePath("asset-loading-wide.png")));
            window.resize(960, 640);
            QTest::qWait(60);
            QVERIFY(card->isVisibleTo(&window));
            QVERIFY(window.rect().contains(QRect(card->mapTo(&window, QPoint(0, 0)), card->size())));
            QVERIFY(window.grab().save(QDir(preview).filePath("asset-loading-compact.png")));
        }
        assets.release();
        QTRY_COMPARE_WITH_TIMEOUT(total->text(), QString("10,000"), 4000);
        QVERIFY(loading->isHidden()); QVERIFY(tree->isVisibleTo(&window));
        QVERIFY(!bar->isVisibleTo(&window));
        QCOMPARE(bar->value(),bar->maximum());
        QVERIFY(filters->isVisibleTo(&window));
        vault.lock();
        QVERIFY(!bar->isVisibleTo(&window));QCOMPARE(bar->value(),0);
    }

    void anEmptyVaultDoesNotKeepTheAssetLoaderVisible() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Vault vault(directory.filePath("vault.bin")); QVERIFY(vault.create(Password)); vault.lock();
        PortfolioController::Factories factories;
        factories.trading = [](const AccountRecord&) { return static_cast<TradingTransport*>(nullptr); };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        AccountModel accounts(&vault); LocalWindow window(vault, accounts);
        window.setPortfolioController(&portfolio); window.setSecurityStatus(true); window.show();
        window.findChild<QPushButton*>("unlockVault")->click();
        auto* dialog = window.findChild<VaultDialog*>("vaultDialog"); QVERIFY(dialog);
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        dialog->findChild<QLineEdit*>("currentPassword")->setText(Password);
        dialog->findChild<QPushButton*>("submitVault")->click();
        QVERIFY(vault.isUnlocked()); QVERIFY(!dialog->isVisible());
        QTest::qWait(40);
        QVERIFY(!window.findChild<QWidget*>("portfolioLoadingPane")->isVisibleTo(&window));
        QVERIFY(window.findChild<QWidget*>("portfolioWorkbench")->isVisibleTo(&window));
        vault.lock();
    }

  private:
    QTemporaryDir settings_;
};

QTEST_MAIN(UnlockLoadingTest)
#include "tst_unlock_loading.moc"
