#include <QAccessible>
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/VaultGate.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/PressRevealButton.h"
#include "keefi/MarketCandles.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartAlertFeed.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/BackgroundTray.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/StockChartSource.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QEvent>
#include <QApplication>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTimer>
#include <memory>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QTreeView>
#include <QPushButton>
#include <QProgressBar>
#include <QRegularExpression>
#include <QComboBox>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;
namespace {
const QString Password=QStringLiteral("Fake Gate Password! 2026");
QByteArray bytes(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
QWidget* form(QWidget* gate,const char* name){return gate->findChild<QWidget*>(name);}
QLineEdit* edit(QWidget* owner,const char* name){return owner->findChild<QLineEdit*>(name);}
QPushButton* submit(QWidget* owner){return owner->findChild<QPushButton*>("submitVault");}
class NoReadTransport final:public TradingTransport {
  public:int requests=0,authenticatedRequests=0;quint64 token=0;RequestSpec lastRequest;
    void abort()override{}void send(quint64 value,const RequestSpec& request)override{token=value;lastRequest=request;++requests;if(request.authenticated)++authenticatedRequests;}
};
class GatePaintObserver final:public QObject {
  public:
    bool seen=false;std::function<void()> firstPaint;
    bool eventFilter(QObject*,QEvent* event)override{
        if(event->type()==QEvent::Paint&&!seen){seen=true;if(firstPaint)firstPaint();}return false;
    }
};
class GateNoticeSink final:public SystemNotificationSink {
  public:
    bool privateEnabled=false;int authorizations=0,posts=0;
    SystemNoticePermission permission()const override{return SystemNoticePermission::Unavailable;}
    QString status()const override{return QStringLiteral("synthetic unavailable");}
    void refreshPermission()override{}void requestAuthorization()override{++authorizations;}
    void post(const SystemNotice&)override{++posts;}void remove(const QStringList&)override{}
    void setPrivateContext(quint64,bool enabled)override{privateEnabled=enabled;}void stop()override{}
};
class GateTray final:public BackgroundTray {
  public:
    bool shown=false;
    bool available()const override{return true;}
    bool visible()const override{return shown;}
    void setMenu(QMenu*)override{}
    void show()override{shown=true;}void hide()override{shown=false;}
};
}

class VaultGateTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Vault Gate");
        QCoreApplication::setApplicationName("Synthetic Vault Gate");
        QCoreApplication::setApplicationVersion("0.18.0-synthetic");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());
    }
    void cleanup(){KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(ThemeMode::System);}
    void gateShowsTheFormThatFitsTheFileAndKeepsThemeAccessible(){
        QTemporaryDir dir;Vault vault(dir.filePath("absent-vault"));AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();window.show();
        auto* gate=window.findChild<VaultGate*>("vaultGate");QVERIFY(gate);
        // No tabs: the vault file decides which form is shown.
        QVERIFY(!gate->findChild<QPushButton*>("vaultGateExistingTab"));QVERIFY(!gate->findChild<QPushButton*>("vaultGateNewTab"));
        QVERIFY(form(gate,"vaultGateCreateForm")->isVisibleTo(&window));QVERIFY(!form(gate,"vaultGateUnlockForm")->isVisibleTo(&window));QVERIFY(!vault.exists());
        QCOMPARE(gate->findChild<QLabel*>("vaultGateTitle")->text(),QString("새 금고 만들기"));
        {Vault other(vault.filePath());QVERIFY(other.create(Password));}
        gate->refreshMetadata(true,{});
        QVERIFY(form(gate,"vaultGateUnlockForm")->isVisibleTo(&window));QVERIFY(!form(gate,"vaultGateCreateForm")->isVisibleTo(&window));
        QCOMPARE(gate->findChild<QLabel*>("vaultGateTitle")->text(),QString("금고 열기"));
        KeefiTheme::instance()->setMode(ThemeMode::Light);auto* ax=QAccessible::queryAccessibleInterface(gate->findChild<QPushButton*>("vaultGateDark"));QVERIFY(ax&&ax->actionInterface());ax->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());QVERIFY(KeefiTheme::instance()->isDark());
    }
    void deferredWorkbenchStartsAfterGatePaintAndOnlyOnce() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-deferred.bin"));AccountModel accounts(&vault);
        QSettings profile(dir.filePath("synthetic-deferred.ini"),QSettings::IniFormat);
        NoReadTransport publicHttp,chartHttp,metadata;PublicMarketCatalog catalog(&publicHttp);
        MarketCandles candles(&chartHttp,std::unique_ptr<PublicCandleSocket>{});
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);
        window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setSecurityStatus(true);
        int initialized=0,atFirstPaint=-1;GatePaintObserver paint;
        paint.firstPaint=[&]{atFirstPaint=initialized;};window.installEventFilter(&paint);
        window.setDeferredWorkbenchInitializer([&]{++initialized;window.setMarketCandles(&candles);});
        window.showVaultGate();QCOMPARE(initialized,0);QVERIFY(!window.findChild<ChartWorkspace*>());
        window.show();QCOMPARE(initialized,0);QCOMPARE(publicHttp.requests,0);
        QTRY_COMPARE(initialized,1);QCOMPARE(atFirstPaint,0);QVERIFY(window.findChild<ChartWorkspace*>());
        QVERIFY(window.findChild<QWidget*>("vaultGate")->isVisibleTo(&window));
        window.update();QCoreApplication::processEvents();window.repaint();QCoreApplication::processEvents();
        QCOMPARE(initialized,1);QCOMPARE(publicHttp.authenticatedRequests+chartHttp.authenticatedRequests+metadata.authenticatedRequests,0);
    }
    void deferredWorkbenchIsReadyForEarlyGateExit_data() {
        QTest::addColumn<QString>("entry");
        for(const auto& entry:{"browse","create","unlock","restore"})QTest::newRow(entry)<<QString::fromLatin1(entry);
    }
    void deferredWorkbenchIsReadyForEarlyGateExit() {
        QFETCH(QString,entry);QTemporaryDir dir;const auto target=dir.filePath("synthetic-early.bin");
        const auto backup=dir.filePath("synthetic-backup.bin");
        if(entry=="restore"){Vault source(backup);QVERIFY(source.create(Password));source.lock();}
        Vault vault(target);if(entry=="unlock"){QVERIFY(vault.create(Password));vault.lock();}
        AccountModel accounts(&vault);QSettings profile(dir.filePath("synthetic-early.ini"),QSettings::IniFormat);
        NoReadTransport publicHttp,chartHttp,metadata;PublicMarketCatalog catalog(&publicHttp);
        MarketCandles candles(&chartHttp,std::unique_ptr<PublicCandleSocket>{});
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setChartSettings(&profile);
        window.setChartInstrumentTransport(&metadata);window.setSecurityStatus(true);int initialized=0;
        window.setDeferredWorkbenchInitializer([&]{++initialized;window.setMarketCandles(&candles);});
        window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");
        QCOMPARE(initialized,0);QVERIFY(!window.findChild<ChartWorkspace*>());
        // A real button signal before the first queued event turn must not
        // expose a partially constructed workbench or consume setup twice.
        if(entry=="browse")gate->findChild<QPushButton*>("vaultGateBrowse")->click();
        else if(entry=="create"){
            auto* create=form(gate,"vaultGateCreateForm");edit(create,"newPassword")->setText(Password);edit(create,"confirmPassword")->setText(Password);
            QVERIFY(submit(create)->isEnabled());submit(create)->click();
        }else if(entry=="unlock"){
            auto* unlock=form(gate,"vaultGateUnlockForm");edit(unlock,"currentPassword")->setText(Password);
            QVERIFY(submit(unlock)->isEnabled());submit(unlock)->click();
        }else{
            QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,backup)));
            auto* restore=form(gate,"vaultGateRestoreForm");edit(restore,"currentPassword")->setText(Password);
            QVERIFY(submit(restore)->isEnabled());submit(restore)->click();
        }
        QCOMPARE(initialized,1);QVERIFY(window.findChild<ChartWorkspace*>());QVERIFY(!gate->isVisibleTo(&window));
        QCOMPARE(vault.isUnlocked(),entry!="browse");
        QCOMPARE(window.findChild<QTabWidget*>("portfolioPages")->currentWidget()->objectName(),entry=="browse"?QString("explorePage"):QString("portfolioHome"));
        QCoreApplication::processEvents();QCOMPARE(initialized,1);
        QCOMPARE(publicHttp.authenticatedRequests+chartHttp.authenticatedRequests+metadata.authenticatedRequests,0);
    }
    void deferredWorkbenchRestoresPublicAlertsWhileGateStaysLocked() {
        QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-deferred-alerts.ini"),QSettings::IniFormat);
        WatchlistWorkspaceList savedLists;const auto listId=savedLists.createList("합성 시작 알림");
        QVERIFY(savedLists.addIdentity(listId,{"coin","upbit","KRW-SYN"}));savedLists.useList(listId);profile.setValue("chart/watchlists/v2",savedLists.publicState());
        GateNoticeSink sink;ChartPhrasePack phrases;ChartAlertEngine savedEngine(&sink);
        QVERIFY(savedEngine.setPublicLists({{listId,{{"coin","upbit","KRW-SYN"}}}}));
        auto rule=savedEngine.rule(listId);rule.enabled=true;QVERIFY(savedEngine.setRule(rule));
        profile.setValue("chart/alerts/v1",savedEngine.publicState());const auto listBytes=savedLists.publicState(),ruleBytes=savedEngine.publicState();
        Vault vault(dir.filePath("synthetic-deferred-alert-vault.bin"));QVERIFY(vault.create(Password));AccountModel accounts(&vault);
        NoReadTransport chartHttp,metadata,alertHttp,stockHttp,alertStockHttp,publicHttp;PublicMarketCatalog catalog(&publicHttp);
        MarketCandles candles(&chartHttp,std::unique_ptr<PublicCandleSocket>{});StockChartSource stock(&stockHttp),alertStock(&alertStockHttp);
        ChartAlertEngine engine(&sink);ChartAlertFeed feed(&alertHttp);
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setSecurityStatus(true);
        int initialized=0;bool unlockedDuringSetup=true;
        window.setDeferredWorkbenchInitializer([&]{
            ++initialized;unlockedDuringSetup=vault.isUnlocked();window.setMarketCandles(&candles);
            window.setStockChartSources(&stock,&alertStock);window.setChartAlerts(&engine,&feed,&sink,&phrases,&profile);
        });
        window.showVaultGate();window.lockAndClear();window.setSecurityStatus(false,"SYNTHETIC_SECURITY_UNAVAILABLE");window.show();
        QTRY_COMPARE(initialized,1);QVERIFY(!unlockedDuringSetup);QVERIFY(!vault.isUnlocked());QCOMPARE(accounts.rowCount(),0);
        QVERIFY(window.findChild<QWidget*>("vaultGate")->isVisibleTo(&window));QVERIFY(!window.findChild<QPushButton*>("addAccount")->isEnabled());
        QVERIFY(engine.rule(listId).enabled);QCOMPARE(engine.demands().size(),1);QVERIFY(feed.running());
        QTest::qWait(150);QCOMPARE(alertHttp.requests,0);QVERIFY(publicHttp.requests>0);
        QCOMPARE(publicHttp.lastRequest.operation,Operation::Markets);
        QCOMPARE(publicHttp.lastRequest.url.path(),QString("/v1/market/all"));
        HttpResult official;official.status=200;official.body=R"([{"market":"KRW-SYN","korean_name":"합성 공개 종목","english_name":"Synthetic"}])";
        emit publicHttp.finished(publicHttp.token,official);
        QTRY_VERIFY_WITH_TIMEOUT(alertHttp.requests>0,1500);QVERIFY(!sink.privateEnabled);QCOMPARE(sink.authorizations,0);QCOMPARE(sink.posts,0);
        QVERIFY(!vault.isUnlocked());QVERIFY(window.findChild<QWidget*>("vaultGate")->isVisibleTo(&window));
        QCOMPARE(stockHttp.requests+alertStockHttp.requests,0);
        QCOMPARE(chartHttp.authenticatedRequests+metadata.authenticatedRequests+alertHttp.authenticatedRequests+publicHttp.authenticatedRequests,0);
        QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(),listBytes);QCOMPARE(profile.value("chart/alerts/v1").toByteArray(),ruleBytes);
    }
    void earlyCloseHonorsSavedBackgroundOffBeforeFirstPaint() {
        QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-early-close.ini"),QSettings::IniFormat);
        WatchlistWorkspaceList savedLists;const auto listId=savedLists.createList("합성 닫기 알림");
        QVERIFY(savedLists.addIdentity(listId,{"coin","upbit","KRW-SYN"}));savedLists.useList(listId);
        GateTray tray;GateNoticeSink sink;ChartPhrasePack phrases;ChartAlertEngine savedEngine(&sink);
        QVERIFY(savedEngine.setPublicLists({{listId,{{"coin","upbit","KRW-SYN"}}}}));
        auto rule=savedEngine.rule(listId);rule.enabled=true;QVERIFY(savedEngine.setRule(rule));
        ChartAlertsPanel savedPanel(&savedEngine,nullptr,&sink,&phrases);
        savedPanel.setBackgroundEnabled(false);savedPanel.markCloseHintSeen();
        const auto savedUi=savedPanel.publicState();
        profile.setValue("chart/watchlists/v2",savedLists.publicState());
        profile.setValue("chart/alerts/v1",savedEngine.publicState());profile.setValue("chart/alerts/ui/v1",savedUi);
        Vault vault(dir.filePath("synthetic-early-close-vault"));AccountModel accounts(&vault);
        NoReadTransport chartHttp,metadata,alertHttp;
        MarketCandles candles(&chartHttp,std::unique_ptr<PublicCandleSocket>{});ChartAlertEngine engine(&sink);ChartAlertFeed feed(&alertHttp);
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts,nullptr,&tray);window.setPortfolioController(&portfolio);
        window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setSecurityStatus(true);
        int initialized=0;window.setDeferredWorkbenchInitializer([&]{
            ++initialized;window.setMarketCandles(&candles);window.setChartAlerts(&engine,&feed,&sink,&phrases,&profile);
        });
        QSignalSpy quit(&window,&LocalWindow::quitRequested);GatePaintObserver paint;window.installEventFilter(&paint);
        window.showVaultGate();window.show();QCOMPARE(initialized,0);QVERIFY(!paint.seen);
        // The buggy default background choice opens an explanation dialog.
        // Dismiss it without letting a modal window hang this regression test.
        QTimer::singleShot(0,&window,[]{for(auto* widget:QApplication::topLevelWidgets())
            if(auto* message=qobject_cast<QMessageBox*>(widget))message->accept();});
        window.close();
        QCOMPARE(quit.count(),1);
        QCOMPARE(initialized,1);QVERIFY(!window.isVisible());QVERIFY(!tray.visible());
        QVERIFY(engine.rule(listId).enabled);QCOMPARE(engine.demands().size(),1);
        QCOMPARE(profile.value("chart/alerts/ui/v1").toByteArray(),savedUi);
        QCOMPARE(chartHttp.authenticatedRequests+metadata.authenticatedRequests+alertHttp.authenticatedRequests,0);
        QCOMPARE(sink.authorizations,0);QCOMPARE(sink.posts,0);
        QCoreApplication::processEvents();QCOMPARE(initialized,1);QCOMPARE(quit.count(),1);
    }
    void deferredWorkbenchIsCanceledWithWindowLifetime_data() {
        QTest::addColumn<bool>("paintBeforeDestroy");QTest::newRow("before-paint")<<false;QTest::newRow("queued-after-paint")<<true;
    }
    void deferredWorkbenchIsCanceledWithWindowLifetime() {
        QFETCH(bool,paintBeforeDestroy);QTemporaryDir dir;Vault vault(dir.filePath("synthetic-cancel.bin"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        int initialized=0;auto window=std::make_unique<LocalWindow>(vault,accounts);window->setPortfolioController(&portfolio);
        window->setDeferredWorkbenchInitializer([&]{++initialized;});window->setSecurityStatus(true);window->showVaultGate();
        GatePaintObserver paint;window->installEventFilter(&paint);
        if(paintBeforeDestroy){window->show();QVERIFY(!window->grab().isNull());QVERIFY(paint.seen);}
        QCOMPARE(initialized,0);window.reset();QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();QCOMPARE(initialized,0);
    }
    void startupOffersCreateAndPublicBrowseWithoutVault() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault,nullptr,factories);
        NoReadTransport transport;PublicMarketCatalog catalog(&transport);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);
        window.setSecurityStatus(true);window.resize(1280,820);
        QVERIFY2(QMetaObject::invokeMethod(&window,"showVaultGate",Qt::DirectConnection),"Startup must expose the explicit vault gate entry point");
        window.show();QCoreApplication::processEvents();
        auto* gate=window.findChild<QWidget*>("vaultGate");QVERIFY(gate);QVERIFY(gate->isVisibleTo(&window));
        QVERIFY(form(gate,"vaultGateCreateForm")->isVisibleTo(&window));QVERIFY(!form(gate,"vaultGateUnlockForm")->isVisibleTo(&window));
        auto* browse=gate->findChild<QPushButton*>("vaultGateBrowse");QVERIFY(browse);QVERIFY(browse->isVisibleTo(&window));
        QCOMPARE(transport.requests,0);
        QTest::mouseClick(browse,Qt::LeftButton);
        QVERIFY(!gate->isVisibleTo(&window));QVERIFY(!vault.exists());QVERIFY(!vault.isUnlocked());
        QCOMPARE(window.findChild<QTabWidget*>("portfolioPages")->currentWidget()->objectName(),QString("explorePage"));
        QVERIFY(window.findChild<QPushButton*>("createVault")->isVisibleTo(&window));
    }
    void createUsesExistingPasswordRulesAndClearsAfterSuccess() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));AccountModel accounts(&vault);
        LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();window.show();
        auto* gate=window.findChild<QWidget*>("vaultGate");auto* create=form(gate,"vaultGateCreateForm");
        QVERIFY(create);auto* password=edit(create,"newPassword");auto* confirm=edit(create,"confirmPassword");
        password->setText("Short1!");confirm->setText("Short1!");QVERIFY(!submit(create)->isEnabled());
        password->setText(Password);confirm->setText(Password);QVERIFY(submit(create)->isEnabled());
        QTest::mouseClick(submit(create),Qt::LeftButton);
        QVERIFY(vault.isUnlocked());QVERIFY(!gate->isVisibleTo(&window));QVERIFY(password->text().isEmpty());QVERIFY(confirm->text().isEmpty());
        window.lockAndClear();QVERIFY(!vault.isUnlocked());QVERIFY(!gate->isVisibleTo(&window));
    }
    void existingUnlockPreservesCiphertextAndDoesNotExposeConnections() {
        QTemporaryDir dir;const auto path=dir.filePath("synthetic.bin");Vault vault(path);QVERIFY(vault.create(Password));vault.lock();
        const auto before=bytes(path);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
        window.setSecurityStatus(true);window.showVaultGate();window.show();
        auto* gate=window.findChild<QWidget*>("vaultGate");QVERIFY(form(gate,"vaultGateUnlockForm")->isVisibleTo(&window));
        // The locked gate says nothing about what the vault holds or when the file changed.
        const QRegularExpression fileTime(QStringLiteral("\\d{4}-\\d{2}-\\d{2}"));
        for(auto* text:gate->findChildren<QLabel*>())if(text->isVisibleTo(&window))
            QVERIFY2(!text->text().contains("연결")&&!fileTime.match(text->text()).hasMatch(),qPrintable(text->text()));
        auto* unlock=form(gate,"vaultGateUnlockForm");auto* password=edit(unlock,"currentPassword");
        password->setText("Wrong Gate Password! 2026");QTest::mouseClick(submit(unlock),Qt::LeftButton);
        QVERIFY(!vault.isUnlocked());QVERIFY(gate->isVisibleTo(&window));QVERIFY(password->text().isEmpty());
        QVERIFY(!unlock->findChild<QLabel*>("vaultError")->text().isEmpty());QCOMPARE(bytes(path),before);
        password->setText(Password);QTest::mouseClick(submit(unlock),Qt::LeftButton);
        QVERIFY(vault.isUnlocked());QVERIFY(!gate->isVisibleTo(&window));QCOMPARE(bytes(path),before);
    }
    void embeddedPasswordEnterUsesThePrimaryAction_data() {
        QTest::addColumn<bool>("unlock");
        QTest::addColumn<QString>("fieldName");
        QTest::addColumn<int>("key");
        QTest::newRow("create-new-password") << false << QStringLiteral("newPassword") << int(Qt::Key_Return);
        QTest::newRow("create-confirmation") << false << QStringLiteral("confirmPassword") << int(Qt::Key_Enter);
        QTest::newRow("unlock-return") << true << QStringLiteral("currentPassword") << int(Qt::Key_Return);
        QTest::newRow("unlock-keypad-enter") << true << QStringLiteral("currentPassword") << int(Qt::Key_Enter);
    }
    void embeddedPasswordEnterUsesThePrimaryAction() {
        QFETCH(bool, unlock);
        QFETCH(QString, fieldName);
        QFETCH(int, key);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Vault vault(dir.filePath("synthetic-keyboard-vault.bin"));
        if (unlock) {
            QVERIFY(vault.create(Password));
            vault.lock();
        }

        AccountModel accounts(&vault);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.showVaultGate();
        window.resize(1024, 700);
        window.show();
        auto* gate = window.findChild<QWidget*>("vaultGate");
        QVERIFY(gate);
        auto* active = gate->findChild<VaultDialog*>(unlock ? "vaultGateUnlockForm" : "vaultGateCreateForm");
        QVERIFY(active && active->isVisibleTo(&window));
        auto* field = active->findChild<QLineEdit*>(fieldName);
        auto* primary = submit(active);
        QVERIFY(field && primary);
        QSignalSpy submitted(active, &VaultDialog::submitted);

        if (!unlock) {
            edit(active, "newPassword")->setText(Password);
            edit(active, "confirmPassword")->setText(QStringLiteral("Different Fake Password! 2026"));
        }
        QVERIFY(!primary->isEnabled());
        field->setFocus();
        QCOMPARE(window.focusWidget(), field);
        QTest::keyClick(field, Qt::Key(key));
        QCOMPARE(submitted.size(), 0);
        QCOMPARE(vault.exists(), unlock);
        QVERIFY(!vault.isUnlocked());
        QVERIFY(gate->isVisibleTo(&window));
        if (!unlock) {
            QVERIFY(edit(active, "newPassword")->text() == Password);
            QVERIFY(edit(active, "confirmPassword")->text() == QStringLiteral("Different Fake Password! 2026"));
        }

        if (unlock) {
            field->setText(Password);
        } else {
            edit(active, "confirmPassword")->setText(Password);
        }
        QVERIFY(primary->isEnabled());
        QTest::keyClick(field, Qt::Key(key));
        QCOMPARE(submitted.size(), 1);
        QVERIFY(vault.isUnlocked());
        QVERIFY(!gate->isVisibleTo(&window));
        QVERIFY(field->text().isEmpty());
    }
    void pageChangesBrowseLockAndEscapeClearSecretsWithoutLosingTheForm() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));AccountModel accounts(&vault);LocalWindow window(vault,accounts);
        window.setSecurityStatus(true);window.showVaultGate();window.resize(1280,820);window.show();
        auto* gate=window.findChild<QWidget*>("vaultGate");auto* create=form(gate,"vaultGateCreateForm");auto* password=edit(create,"newPassword");
        password->setText(Password);password->setFocus();QTest::keyClick(password,Qt::Key_Escape);
        QVERIFY2(create->isVisibleTo(&window),"Escape must not hide the embedded password form");
        QVERIFY(password->text().isEmpty());
        // Leaving the form for a backup restore and coming back clears it.
        password->setText(Password);QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,dir.filePath("synthetic-backup.bin"))));
        QVERIFY(password->text().isEmpty());QVERIFY(form(gate,"vaultGateRestoreForm")->isVisibleTo(&window));
        gate->findChild<QPushButton*>("vaultGateRestoreCancel")->click();QVERIFY(create->isVisibleTo(&window));password->setText(Password);
        auto* reveal=create->findChild<PressRevealButton*>("newPasswordReveal");QVERIFY(reveal);
        QTest::mousePress(reveal,Qt::LeftButton);QCOMPARE(password->echoMode(),QLineEdit::Normal);
        window.lockAndClear();QVERIFY(password->text().isEmpty());QCOMPARE(password->echoMode(),QLineEdit::Password);
        QTest::mouseRelease(reveal,Qt::LeftButton);password->setText(Password);window.hide();QVERIFY(password->text().isEmpty());
        window.show();password->setText(Password);gate->findChild<QPushButton*>("vaultGateBrowse")->click();QVERIFY(password->text().isEmpty());
    }
    void submitRechecksSecurityBeforeCreatingTheVault() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));AccountModel accounts(&vault);LocalWindow window(vault,accounts);
        window.setSecurityStatus(true);window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");auto* create=form(gate,"vaultGateCreateForm");
        edit(create,"newPassword")->setText(Password);edit(create,"confirmPassword")->setText(Password);
        connect(&window,&LocalWindow::securityCheckRequested,&window,[&]{window.setSecurityStatus(false,"SYNTHETIC_SECURITY_LOSS");});
        submit(create)->click();QVERIFY(!vault.exists());QVERIFY(!vault.isUnlocked());QVERIFY(gate->isVisibleTo(&window));
        QVERIFY(edit(create,"newPassword")->text().isEmpty());QVERIFY(!submit(create)->isEnabled());
        auto* browse=gate->findChild<QPushButton*>("vaultGateBrowse");QVERIFY(browse->isEnabled());
    }
    void backupRestoreUsesTheSamePasswordFormAndKeepsTheSource() {
        QTemporaryDir dir;const auto sourcePath=dir.filePath("synthetic-source.bin");
        {Vault source(sourcePath);QVERIFY(source.create(Password));source.lock();}
        const auto original=bytes(sourcePath);Vault vault(dir.filePath("synthetic-target.bin"));AccountModel accounts(&vault);
        LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");
        QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,sourcePath)));
        auto* restore=form(gate,"vaultGateRestoreForm");QVERIFY(restore->isVisibleTo(&window));
        for(auto* text:gate->findChildren<QLabel*>())QVERIFY(!text->text().contains(sourcePath));
        edit(restore,"currentPassword")->setText("Wrong Gate Password! 2026");submit(restore)->click();
        QVERIFY(!vault.exists());QCOMPARE(bytes(sourcePath),original);QVERIFY(edit(restore,"currentPassword")->text().isEmpty());
        edit(restore,"currentPassword")->setText(Password);submit(restore)->click();
        QVERIFY(vault.isUnlocked());QVERIFY(!gate->isVisibleTo(&window));QCOMPARE(bytes(sourcePath),original);QCOMPARE(bytes(vault.filePath()),original);
        QVERIFY(!QFile::exists(sourcePath+".lock"));
    }
    void stagedBackupStaysLockedAndQuitsNormally() {
        QTemporaryDir dir; const auto sourcePath=dir.filePath("synthetic-source.bin");
        {Vault source(sourcePath);QVERIFY(source.create(Password));source.lock();}
        const auto original=bytes(sourcePath); Vault vault(dir.filePath("synthetic-target.bin")); AccountModel accounts(&vault);
        LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.showVaultGate(); window.show();
        int calls=0; bool inputsMatch=false;
        window.setBackupRestoreHandler([&](const QString& path,const QString& password,QString*) {
            ++calls; inputsMatch=path==sourcePath&&password==Password; return BackupRestoreOutcome::PreparedForRestart;
        });
        QSignalSpy quit(&window,&LocalWindow::quitRequested); int activations=0;
        connect(&vault,&Vault::accountsChanged,&window,[&] {
            if(vault.isUnlocked()||!vault.accounts().isEmpty())++activations;
        });
        auto* gate=window.findChild<QWidget*>("vaultGate");
        QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,sourcePath)));
        auto* restore=form(gate,"vaultGateRestoreForm"); edit(restore,"currentPassword")->setText(Password);
        bool messageSeen=false, secretCleared=false, submitReturned=false, noticeAfterReturn=false;
        QTimer dismiss; dismiss.setInterval(1);
        connect(&dismiss,&QTimer::timeout,&window,[&] {
            for(auto* widget:QApplication::topLevelWidgets()) if(auto* message=qobject_cast<QMessageBox*>(widget)) {
                messageSeen=true; noticeAfterReturn=submitReturned;
                secretCleared=edit(restore,"currentPassword")->text().isEmpty(); message->accept();
            }
        });
        dismiss.start(); submit(restore)->click(); submitReturned=true;
        QTRY_VERIFY(messageSeen); dismiss.stop();
        QCOMPARE(calls,1); QVERIFY(inputsMatch); QVERIFY(messageSeen); QVERIFY(secretCleared);
        QVERIFY2(noticeAfterReturn,"The password submission stack must return before a public modal notice.");
        QCOMPARE(quit.count(),1); QCOMPARE(activations,0); QVERIFY(!vault.isUnlocked()); QVERIFY(!vault.exists());
        QCOMPARE(accounts.rowCount(),0); QCOMPARE(bytes(sourcePath),original); QVERIFY(!window.isVisible());
    }
    void failedStagedBackupNeverFallsBackToImmediateRestore() {
        QTemporaryDir dir; const auto sourcePath=dir.filePath("synthetic-source.bin");
        {Vault source(sourcePath);QVERIFY(source.create(Password));source.lock();}
        const auto original=bytes(sourcePath); Vault vault(dir.filePath("synthetic-target.bin")); AccountModel accounts(&vault);
        LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.showVaultGate(); window.show();
        int calls=0; window.setBackupRestoreHandler([&](const QString&,const QString&,QString* error) {
            ++calls; if(error)*error="SYNTHETIC_RESTORE_CONFLICT"; return BackupRestoreOutcome::Failed;
        });
        QSignalSpy quit(&window,&LocalWindow::quitRequested); auto* gate=window.findChild<QWidget*>("vaultGate");
        QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,sourcePath)));
        auto* restore=form(gate,"vaultGateRestoreForm"); edit(restore,"currentPassword")->setText(Password); submit(restore)->click();
        QCOMPARE(calls,1); QCOMPARE(quit.count(),0); QVERIFY(!vault.exists()); QVERIFY(!vault.isUnlocked());
        QCOMPARE(bytes(sourcePath),original); QVERIFY(edit(restore,"currentPassword")->text().isEmpty());
        QCOMPARE(restore->findChild<QLabel*>("vaultError")->text(),QString("SYNTHETIC_RESTORE_CONFLICT"));
        QVERIFY(gate->isVisibleTo(&window));
    }
    void compatibleBackupHandlerEntersTheUnlockedWorkbench() {
        QTemporaryDir dir; const auto sourcePath=dir.filePath("synthetic-source.bin");
        {Vault source(sourcePath);QVERIFY(source.create(Password));source.lock();}
        const auto original=bytes(sourcePath); Vault vault(dir.filePath("synthetic-target.bin")); AccountModel accounts(&vault);
        LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.showVaultGate(); window.show();
        int calls=0; window.setBackupRestoreHandler([&](const QString& path,const QString& password,QString* error) {
            ++calls; return vault.restoreBackup(path,password,error)?BackupRestoreOutcome::Restored:BackupRestoreOutcome::Failed;
        });
        QSignalSpy quit(&window,&LocalWindow::quitRequested); auto* gate=window.findChild<QWidget*>("vaultGate");
        QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,sourcePath)));
        auto* restore=form(gate,"vaultGateRestoreForm"); edit(restore,"currentPassword")->setText(Password); submit(restore)->click();
        QCOMPARE(calls,1); QCOMPARE(quit.count(),0); QVERIFY(vault.isUnlocked()); QVERIFY(!gate->isVisibleTo(&window));
        QCOMPARE(bytes(vault.filePath()),original); QCOMPARE(bytes(sourcePath),original);
    }
    void stagedBackupHandlerCannotRunAfterSecurityLoss() {
        QTemporaryDir dir; Vault vault(dir.filePath("synthetic-target.bin")); AccountModel accounts(&vault);
        LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.showVaultGate(); window.show();
        int calls=0; window.setBackupRestoreHandler([&](const QString&,const QString&,QString*) {
            ++calls; return BackupRestoreOutcome::PreparedForRestart;
        });
        auto* gate=window.findChild<QWidget*>("vaultGate"); const auto sourcePath=dir.filePath("synthetic-backup.bin");
        QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,sourcePath)));
        auto* restore=form(gate,"vaultGateRestoreForm"); edit(restore,"currentPassword")->setText(Password);
        connect(&window,&LocalWindow::securityCheckRequested,&window,[&]{window.setSecurityStatus(false,"SYNTHETIC_SECURITY_LOSS");});
        submit(restore)->click(); QCOMPARE(calls,0); QVERIFY(!vault.exists()); QVERIFY(!vault.isUnlocked());
        QVERIFY(edit(restore,"currentPassword")->text().isEmpty());
    }
    void abandonedBackupReturnsToAConsistentCreateRoute() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));AccountModel accounts(&vault);
        LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();window.show();
        auto* gate=window.findChild<QWidget*>("vaultGate");
        for(bool lock:{false,true}){
            QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,dir.filePath("synthetic-backup.bin"))));
            auto* restore=form(gate,"vaultGateRestoreForm");QVERIFY(restore->isVisibleTo(&window));
            edit(restore,"currentPassword")->setText(Password);
            if(lock)window.lockAndClear();else{window.hide();window.show();}
            QVERIFY(edit(restore,"currentPassword")->text().isEmpty());
            QVERIFY2(form(gate,"vaultGateCreateForm")->isVisibleTo(&window),"An abandoned backup must not leave a restore form with an empty source path");
            QCOMPARE(gate->findChild<QLabel*>("vaultGateTitle")->text(),QString("새 금고 만들기"));
        }
    }
    void reselectingTheCurrentThemeKeepsItSelected() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));AccountModel accounts(&vault);
        LocalWindow window(vault,accounts);window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");
        for(bool dark:{false,true}){
            KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
            auto* selected=gate->findChild<QPushButton*>(dark?"vaultGateDark":"vaultGateLight");
            auto* other=gate->findChild<QPushButton*>(dark?"vaultGateLight":"vaultGateDark");
            QVERIFY(selected->isChecked());selected->click();QVERIFY(selected->isChecked());QVERIFY(!other->isChecked());
        }
        gate->findChild<QPushButton*>("vaultGateBrowse")->click();
        for(bool dark:{false,true}){
            auto* selected=window.findChild<QPushButton*>(dark?"themeDark":"themeLight");
            auto* other=window.findChild<QPushButton*>(dark?"themeLight":"themeDark");
            selected->click();QVERIFY(selected->isChecked());QVERIFY(!other->isChecked());
            selected->click();QVERIFY(selected->isChecked());QVERIFY(!other->isChecked());
        }
    }
    void privateUnlockHandsOffToTheExistingLoaderAndLockKeepsTheWorkbench() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));QVERIFY(vault.create(Password));
        AccountRecord account;account.provider="binance";account.alias="FAKE_D2_PRIVATE_ALIAS";account.environment="live";account.market="spot";account.currency="USDT";
        QVERIFY(vault.addAccount(account,{{"api_key","FAKE_D2_API_KEY"},{"secret","FAKE_D2_SECRET"}},StorageMode::EncryptedVault));vault.lock();
        NoReadTransport delayed;PortfolioController::Factories factories;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &delayed;};
        PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
        window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.showVaultGate();window.show();
        auto* gate=window.findChild<QWidget*>("vaultGate");
        for(auto* text:gate->findChildren<QLabel*>())QVERIFY(!text->text().contains(account.alias));
        for(auto* text:gate->findChildren<QLabel*>())QVERIFY(!text->text().contains("1개"));QCOMPARE(delayed.requests,0);
        auto* unlock=form(gate,"vaultGateUnlockForm");edit(unlock,"currentPassword")->setText(Password);submit(unlock)->click();
        QVERIFY(vault.isUnlocked());QVERIFY(!gate->isVisibleTo(&window));
        auto* loading=window.findChild<QLabel*>("portfolioLoading");QVERIFY(loading);QVERIFY(loading->isVisibleTo(&window));
        QTRY_VERIFY_WITH_TIMEOUT(delayed.requests>0,2000);QVERIFY(loading->isVisibleTo(&window));
        window.lockAndClear();QVERIFY(!vault.isUnlocked());QCOMPARE(accounts.rowCount(),0);
        QCOMPARE(window.findChild<QTreeView*>("portfolioAssets")->model()->rowCount(),0);
        QVERIFY(!gate->isVisibleTo(&window));QVERIFY(!loading->isVisibleTo(&window));
    }
    void loaderBootstrapStopsWhenLastRegistrationIsRemovedBeforeSync() {
        QTemporaryDir dir;QVERIFY(dir.isValid());Vault vault(dir.filePath("synthetic-bootstrap-removal"));QVERIFY(vault.create(Password));
        AccountRecord record;record.provider="binance";record.alias="SYNTHETIC BOOTSTRAP REMOVAL";record.environment="live";record.market="spot";record.currency="USDT";
        QVERIFY(vault.addAccount(record,{{"api_key","FakeBootstrapRemovalKey123456"},{"secret","FakeBootstrapRemovalSecret123456"}},StorageMode::EncryptedVault));
        const auto id=vault.accounts().first().id;vault.lock();
        NoReadTransport http;PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &http;};
        PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);GateTray tray;LocalWindow window(vault,accounts,nullptr,&tray);
        window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");
        auto* unlock=form(gate,"vaultGateUnlockForm");edit(unlock,"currentPassword")->setText(Password);submit(unlock)->click();
        QVERIFY(vault.isUnlocked());QVERIFY(window.findChild<QLabel*>("portfolioLoading")->isVisibleTo(&window));
        // No event turn has registered the initial controller connections yet.
        QVERIFY(vault.removeAccount(id));QCOMPARE(window.findChild<QProgressBar*>("portfolioLoadProgress")->value(),0);
        QVERIFY(window.findChild<QWidget*>("portfolioAllocation")->accessibleDescription().isEmpty());
        QCOMPARE(window.findChild<QTreeView*>("portfolioAssets")->model()->rowCount(),0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QLabel*>("portfolioLoading")->isVisibleTo(&window),1000);
        QCOMPARE(portfolio.connections().size(),0);QVERIFY(window.findChild<QLabel*>("portfolioTotal")->text()!=QString("0"));
        window.lockAndClear();QCOMPARE(window.findChild<QProgressBar*>("portfolioLoadProgress")->value(),0);
    }
    void loaderBootstrapWithNoSelectedPortfolioReads_data() {
        QTest::addColumn<bool>("jupiter");
        QTest::newRow("jupiter-only")<<true;
        QTest::newRow("empty-selected-environment")<<false;
    }
    void loaderBootstrapWithNoSelectedPortfolioReads() {
        QFETCH(bool,jupiter);QTemporaryDir dir;QVERIFY(dir.isValid());
        Vault vault(dir.filePath("synthetic-no-selected-reads"));QVERIFY(vault.create(Password));
        AccountRecord record;record.alias="SYNTHETIC NO SELECTED READS";
        record.provider=jupiter?"jupiter":"binance";record.market=jupiter?"solana":"spot";
        record.environment=jupiter?"mainnet":"live";record.currency=jupiter?"SOL":"USDT";
        QMap<QString,QString> keys{{"api_key","FakeNoSelectedReadsKey123456"}};
        if(!jupiter)keys.insert("secret","FakeNoSelectedReadsSecret123456");
        QVERIFY(vault.addAccount(record,keys,StorageMode::EncryptedVault));vault.lock();
        NoReadTransport http;PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord&){return &http;};
        PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);GateTray tray;LocalWindow window(vault,accounts,nullptr,&tray);
        window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
        // The product exposes live/mainnet only. An empty selection exercises
        // the retained environment filter without adding a testnet or provider.
        if(!jupiter)window.findChild<QComboBox*>("portfolioEnvironment")->setCurrentIndex(-1);
        window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");
        auto* unlock=form(gate,"vaultGateUnlockForm");edit(unlock,"currentPassword")->setText(Password);submit(unlock)->click();
        QVERIFY(vault.isUnlocked());QVERIFY(!gate->isVisibleTo(&window));QTest::qWait(40);
        QVERIFY(!window.findChild<QLabel*>("portfolioLoading")->isVisibleTo(&window));
        QVERIFY(window.findChild<QWidget*>("portfolioWorkbench")->isVisibleTo(&window));
        QCOMPARE(window.findChild<QProgressBar*>("portfolioLoadProgress")->value(),0);
        QVERIFY(window.findChild<QWidget*>("portfolioAllocation")->accessibleDescription().isEmpty());
        QVERIFY(window.findChild<QLabel*>("portfolioTotal")->text()!=QString("0"));
        if(jupiter){QCOMPARE(portfolio.connections().size(),0);QCOMPARE(http.requests,0);}
        else QTRY_VERIFY_WITH_TIMEOUT(http.requests>0,2000);
        window.lockAndClear();QCOMPARE(window.findChild<QProgressBar*>("portfolioLoadProgress")->value(),0);
        QCOMPARE(window.findChild<QTreeView*>("portfolioAssets")->model()->rowCount(),0);
    }
    void publicLinkShowsKeyboardFocusAndTheShellStaysCentred() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic.bin"));QVERIFY(vault.create(Password));vault.lock();AccountModel accounts(&vault);
        QFile file(vault.filePath());QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::fromString("2026-01-02T03:04:00Z",Qt::ISODate),QFileDevice::FileModificationTime));file.close();
        LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();window.show();
        auto* gate=window.findChild<QWidget*>("vaultGate");
        // The file time is no longer shown; the locked gate carries no file metadata.
        QVERIFY(!gate->findChild<QLabel*>("vaultGateModifiedAt"));
        auto* browse=gate->findChild<QPushButton*>("vaultGateBrowse");browse->clearFocus();QCoreApplication::processEvents();const auto before=browse->grab().toImage();
        browse->setFocus(Qt::TabFocusReason);QCoreApplication::processEvents();QVERIFY(browse->hasFocus());QVERIFY(before!=browse->grab().toImage());
        window.resize(1440,900);QCoreApplication::processEvents();
        auto* shell=gate->findChild<QWidget*>("vaultGateShell");QVERIFY(shell);QCOMPARE(shell->width(),1280);QCOMPARE(shell->mapTo(gate,QPoint()).x(),80);
        const auto target=qEnvironmentVariable("KEEFI_D2_RENDER_DIR");if(!target.isEmpty())QVERIFY(window.grab().save(QDir(target).filePath("d2-keyboard-focus-1440.png")));
    }
    void failedRestoreRefreshesExistenceWithoutOverwritingTheTarget() {
        QTemporaryDir dir;const auto sourcePath=dir.filePath("synthetic-source.bin"),targetPath=dir.filePath("synthetic-target.bin");
        {Vault source(sourcePath);QVERIFY(source.create(Password));}
        const auto sourceBefore=bytes(sourcePath);Vault vault(targetPath);AccountModel accounts(&vault);
        LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();window.show();auto* gate=window.findChild<QWidget*>("vaultGate");
        QVERIFY(QMetaObject::invokeMethod(gate,"selectBackup",Q_ARG(QString,sourcePath)));
        {Vault concurrent(targetPath);QVERIFY(concurrent.create("Other Synthetic Gate! 2026"));}
        const auto targetBefore=bytes(targetPath);auto* restore=form(gate,"vaultGateRestoreForm");
        edit(restore,"currentPassword")->setText(Password);submit(restore)->click();
        QVERIFY(!vault.isUnlocked());QVERIFY(gate->isVisibleTo(&window));QCOMPARE(bytes(targetPath),targetBefore);QCOMPARE(bytes(sourcePath),sourceBefore);
        auto* error=restore->findChild<QLabel*>("vaultError");QVERIFY(!error->text().isEmpty());QVERIFY(error->isVisibleTo(&window));
        // The target now exists, so leaving the restore leads to unlocking it.
        gate->findChild<QPushButton*>("vaultGateRestoreCancel")->click();auto* unlock=form(gate,"vaultGateUnlockForm");QVERIFY(unlock->isVisibleTo(&window));
        edit(unlock,"currentPassword")->setText("Other Synthetic Gate! 2026");QVERIFY(submit(unlock)->isEnabled());
    }

    void compactWindowKeepsOneCentredColumnAt1024() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        Vault vault(profile.filePath("synthetic-unused"));
        AccountModel accounts(&vault);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.showVaultGate();
        window.resize(1024, 700);
        window.show();
        QCoreApplication::processEvents();
        auto* gate = window.findChild<QWidget*>("vaultGate");
        QVERIFY(gate);
        QCOMPARE(gate->findChild<QPushButton*>("vaultGateBrowse")->height(), 28);
        // One centred column with nothing beside it, at every width.
        auto* column = gate->findChild<QWidget*>("vaultGateColumn");
        auto* active = form(gate, "vaultGateCreateForm");
        QVERIFY(column && active && active->isVisibleTo(&window));
        QVERIFY(!gate->findChild<QWidget*>("vaultGateIntroduction"));
        const int centre = column->mapTo(&window, QPoint(column->width() / 2, 0)).x();
        QVERIFY2(qAbs(centre - window.width() / 2) <= 2, qPrintable(QString::number(centre)));
        auto* scroll = gate->findChild<QScrollArea*>("vaultGateScroll");
        QVERIFY(scroll->viewport()->rect().contains(submit(active)->mapTo(scroll->viewport(), submit(active)->rect().bottomRight())));
    }
    void rendersKeepBrowseReachable_data() {
        QTest::addColumn<bool>("dark");QTest::addColumn<bool>("existing");QTest::addColumn<QSize>("size");
        for(bool dark:{false,true})for(bool existing:{false,true})for(const auto& size:{QSize(1280,820),QSize(720,640)})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark?"dark":"light",existing?"unlock":"create").arg(size.width())))<<dark<<existing<<size;
    }
    void rendersKeepBrowseReachable() {
        QFETCH(bool,dark);QFETCH(bool,existing);QFETCH(QSize,size);
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-render.bin"));
        if(existing){QVERIFY(vault.create(Password));vault.lock();QFile file(vault.filePath());QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::fromString("2026-01-02T03:04:00Z",Qt::ISODate),QFileDevice::FileModificationTime));file.close();}
        AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.showVaultGate();
        KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);window.resize(size);window.show();QTest::qWait(40);
        QCOMPARE(window.size(),size);auto* gate=window.findChild<QWidget*>("vaultGate");auto* browse=gate->findChild<QPushButton*>("vaultGateBrowse");
        QVERIFY(window.rect().contains(QRect(browse->mapTo(&window,QPoint()),browse->size())));
        QVERIFY(browse->isVisibleTo(&window));QVERIFY(browse->isEnabled());
        auto* scroll=gate->findChild<QScrollArea*>("vaultGateScroll");QVERIFY(scroll);auto* active=form(gate,existing?"vaultGateUnlockForm":"vaultGateCreateForm");
        // Secondary routes sit as one group centred under the form.
        QRect routes;for(auto* button:gate->findChild<QWidget*>("vaultGateFooter")->findChildren<QPushButton*>())
            if(button->isVisibleTo(&window))routes|=QRect(button->mapTo(&window,QPoint()),button->size());
        const int formCentre=active->mapTo(&window,QPoint(active->width()/2,0)).x();
        QVERIFY2(qAbs(routes.center().x()-formCentre)<=2,qPrintable(QString("%1 vs %2").arg(routes.center().x()).arg(formCentre)));
        QCOMPARE(submit(active)->width(),active->width());QCOMPARE(submit(active)->height(),28);
        // The embedded form starts with its field: the explanation lives in the user guide.
        QVERIFY(!active->findChild<QLabel*>("vaultExplanation")->isVisibleTo(&window));
        QCOMPARE(edit(active,existing?"currentPassword":"newPassword")->mapTo(active,QPoint()).y(),0);
        scroll->ensureWidgetVisible(submit(active));QCoreApplication::processEvents();
        QVERIFY(scroll->viewport()->rect().contains(submit(active)->mapTo(scroll->viewport(),submit(active)->rect().center())));
        const auto target=qEnvironmentVariable("KEEFI_D2_RENDER_DIR");
        if(!target.isEmpty()&&size.width()==720){
            QVERIFY(QDir().mkpath(target));QVERIFY(window.grab().save(QDir(target).filePath(QString("d2-%1-%2-720-form.png").arg(dark?"dark":"light",existing?"unlock":"create"))));
        }
        // Restore the first view for the render; the public footer stays fixed.
        scroll->verticalScrollBar()->setValue(0);QCoreApplication::processEvents();
        if(!target.isEmpty()){
            QVERIFY(QDir().mkpath(target));const auto image=window.grab();
            QVERIFY(image.save(QDir(target).filePath(QString("d2-%1-%2-%3.png").arg(dark?"dark":"light",existing?"unlock":"create").arg(size.width()))));
            qInfo()<<"synthetic render logical"<<size<<"physical"<<image.size()<<"DPR"<<image.devicePixelRatio();
        }
    }

};
QTEST_MAIN(VaultGateTest)
#include "tst_vault_gate.moc"
