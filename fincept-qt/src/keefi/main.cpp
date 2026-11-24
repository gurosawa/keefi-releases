#include "keefi/BuildPolicy.h"
#include "keefi/KeefiFonts.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/WorldMarkets.h"
#include "keefi/MarketFlowHistory.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/MarketCandles.h"
#include "keefi/PriceArchive.h"
#include "keefi/NetworkPublicCandleSocket.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartAlertFeed.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/RankingFuturesFeed.h"
#include "keefi/RapidMoveMonitor.h"
#include "keefi/ChartStudyRequestPacer.h"
#include "keefi/NetworkPublicFlowSocket.h"
#include "keefi/MarketCompareSource.h"
#include "keefi/CollectorStore.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/StockChartSource.h"
#include "keefi/TossMarketStream.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/OfficialNotices.h"
#include "keefi/UpbitAnnouncements.h"
#include "keefi/MarketOverview.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/EarningsCalendar.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/TradingTransport.h"
#include "keefi/PlatformSecurity.h"
#include "keefi/ProfilePaths.h"
#include "keefi/ProfileUpgrade.h"
#include "keefi/ProfileUpgradeDialog.h"
#include "keefi/ProfileRestore.h"
#include "keefi/StorageMigration.h"
#include "keefi/IdentityCompatibility.h"
#include "keefi/ProfileSettings.h"
#include "keefi/ProductIdentity.h"
#include "keefi/Vault.h"
#include "keefi/TradingController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/FuturesTradingController.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_WIN
#include <QSslSocket>
#endif

int main(int argc, char* argv[]) {
#ifdef KEEFI_DEVELOPMENT_BUILD
    constexpr bool developmentBuild = true;
#else
    constexpr bool developmentBuild = false;
#endif
    QString hardeningError;
    const bool hardened = keefi::PlatformSecurity::hardenCurrentProcess(&hardeningError);
    QApplication app(argc, argv);
    if (!keefi::KeefiFonts::registerBundledFonts()) return 8;
    app.setFont(keefi::KeefiFonts::font(keefi::FontRole::Body15));
    const QString applicationName = developmentBuild ? keefi::devApplicationName() : keefi::productName();
    QCoreApplication::setApplicationName(applicationName);
    QGuiApplication::setApplicationDisplayName(applicationName);
    QCoreApplication::setOrganizationName(keefi::productName());
    QCoreApplication::setApplicationVersion(QStringLiteral(KEEFI_APPLICATION_VERSION));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("%1 — 로컬 금고·통합 자산 조회").arg(keefi::productName()));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("profile-dir"), QStringLiteral("별도의 로컬 프로필 경로"), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("smoke-report"), QStringLiteral("새 프로필의 비민감 시작 검사 결과 파일"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("smoke-screenshot"), QStringLiteral("시작 검사 화면 이미지 파일"), QStringLiteral("file")});
    parser.process(app);
    const bool smoke = parser.isSet(QStringLiteral("smoke-report"));
    if (parser.isSet(QStringLiteral("smoke-screenshot")) && !smoke) return 2;
    if ((smoke || parser.isSet(QStringLiteral("smoke-screenshot"))) &&
        !parser.isSet(QStringLiteral("profile-dir"))) {
        return 2; // Automated verification must never open the personal profile.
    }
    const std::optional<QString> explicitProfile = parser.isSet(QStringLiteral("profile-dir"))
        ? std::optional<QString>{parser.value(QStringLiteral("profile-dir"))} : std::nullopt;
    keefi::PlatformSecurity security;
    security.initialize();
    const auto startupFailure = [&](int code, const QString& phase) {
        if (smoke) {
            // Keep profile paths, OS error text and account information out of diagnostics.
            const QJsonObject status{{"startup_phase", phase}, {"process_hardened", hardened},
                {"security_ready", hardened && security.ready()}, {"security_status", security.statusCode()}};
            QFile report(parser.value(QStringLiteral("smoke-report")));
            if (report.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                report.write(QJsonDocument(status).toJson());
        }
        return code;
    };
    QString profileError;
    const QString base = explicitProfile.has_value() ? QString{}
        : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!explicitProfile.has_value() && base.isEmpty()) {
        if (!smoke) QMessageBox::critical(nullptr, keefi::productName(),
            QStringLiteral("프로필의 기반 경로를 확인할 수 없습니다."));
        return 3;
    }
    // Production builds use a new root without discovering any old production
    // profile. An explicitly selected old profile uses the same separate-copy UI.
    const std::optional<QString> requestedProfile = explicitProfile.has_value() || developmentBuild
        ? explicitProfile
        : std::optional<QString>{QDir(base).filePath(QStringLiteral("KEEFI/Profiles/Local"))};
    auto plan = keefi::planProfileUpgrade(requestedProfile, base);
    if (plan.action == keefi::ProfileUpgradeAction::Unavailable) {
        if (!smoke) QMessageBox::critical(nullptr, QStringLiteral("%1 프로필을 열 수 없습니다").arg(keefi::productName()), plan.error);
        return startupFailure(3, QStringLiteral("plan_profile"));
    }
    const keefi::ProfileStageConverter convert = [](
        const QString& stage, const QString& password, keefi::ProfileConversionReceipt* receipt, QString* error) {
        if (receipt) *receipt = {};
        keefi::StorageConversionReceipt stored;
        if (!keefi::StorageMigration::convertStagedCopy(stage, password, &stored, error)) return false;
        if (receipt) {
            receipt->verified = stored.verified;
            receipt->identityScheme = stored.identityScheme;
            receipt->evidence = stored.evidence;
        }
        return true;
    };
    keefi::ProfileReady selected;
    if (plan.action == keefi::ProfileUpgradeAction::OfferUpgrade
        || plan.action == keefi::ProfileUpgradeAction::ChooseSource) {
        // The automated start check must never prompt for or transform an existing vault.
        if (smoke) return 5;
        keefi::ProfileUpgradeDialog choice(plan);
        if (choice.exec() != QDialog::Accepted) return 0;
        plan = keefi::selectProfileUpgradeSource(plan, choice.sourceKind(), &profileError);
        if (plan.action != keefi::ProfileUpgradeAction::OfferUpgrade) {
            QMessageBox::critical(nullptr, keefi::productName(), profileError);
            return 3;
        }
        if (plan.requiresPassword) {
            if (!hardened || !security.ready()) {
                QMessageBox::critical(nullptr, keefi::productName(), hardened
                    ? security.statusCode() : hardeningError);
                return 3;
            }
            keefi::ProfileUpgradePasswordDialog password(plan, QCoreApplication::applicationVersion(), convert);
            if (!password.error().isEmpty()) {
                QMessageBox::critical(nullptr, keefi::productName(), password.error());
                return 3;
            }
            password.setPrivateAccess(hardened && security.ready());
            QObject::connect(&security, &keefi::PlatformSecurity::lockRequested,
                             &password, &keefi::ProfileUpgradePasswordDialog::lockAndClear);
            QObject::connect(&security, &keefi::PlatformSecurity::statusChanged, &password, [&] {
                if (!hardened || !security.ready()) password.lockAndClear(security.statusCode());
            });
            if (password.exec() != QDialog::Accepted) return 0;
            selected = password.ready();
        } else {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            auto session = keefi::ProfileUpgradeSession::begin(plan, QCoreApplication::applicationVersion(), &profileError);
            if (session && session->copy(&profileError) && session->convert({}, convert, &profileError)
                && session->verify(&profileError) && session->publish(&profileError))
                selected = session->ready();
            session.reset();
            QApplication::restoreOverrideCursor();
        }
    } else if (plan.action == keefi::ProfileUpgradeAction::UseCurrent) {
        selected = keefi::existingProfileReady(plan, &profileError);
    } else if (plan.action == keefi::ProfileUpgradeAction::CreateCurrent) {
        selected = keefi::createFreshProfile(plan, QCoreApplication::applicationVersion(), &profileError);
    }
    if (!selected.directory.isEmpty()) {
        const auto restorePaths = keefi::profileRestorePaths(selected.directory);
        const QFileInfo pendingRestore(restorePaths.transactionDirectory);
        // Start checks use only a fresh synthetic profile and never complete a
        // previously prepared private backup, even if its active vault is absent.
        if (smoke && (pendingRestore.exists() || pendingRestore.isSymLink())) return 5;
        if (!smoke) selected = keefi::completePendingProfileRestore(
            selected, QCoreApplication::applicationVersion(), &profileError);
    }
    if (selected.directory.isEmpty()
        || !keefi::configureIdentityScheme(selected.identityScheme, &profileError)) {
        if (!smoke) QMessageBox::critical(nullptr, QStringLiteral("%1 프로필을 열 수 없습니다").arg(keefi::productName()), profileError);
        return startupFailure(3, plan.action == keefi::ProfileUpgradeAction::CreateCurrent
            ? QStringLiteral("create_profile") : QStringLiteral("open_profile"));
    }
    const QString profile = selected.directory;
    const keefi::ProfileSettingsIdentity settingsIdentity{keefi::productName(), applicationName};
    if (!smoke && !selected.warning.isEmpty())
        QMessageBox::information(nullptr, applicationName, selected.warning);
    if (profile.isEmpty() || !keefi::Vault::prepareProfileDirectory(profile, &profileError)) {
        if (!smoke) QMessageBox::critical(nullptr, QStringLiteral("%1 프로필을 열 수 없습니다").arg(keefi::productName()), profileError);
        return startupFailure(3, QStringLiteral("secure_profile"));
    }
    QLockFile instanceLock(QDir(profile).filePath(QStringLiteral("instance.lock")));
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(0)) {
        if (!smoke) QMessageBox::information(nullptr, keefi::productName(),
            QStringLiteral("같은 프로필을 다른 %1 창에서 사용 중이거나 잠금 파일을 만들 수 없습니다.").arg(keefi::productName()));
        return startupFailure(4, QStringLiteral("lock_profile"));
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
    keefi::setProfileSettingsIdentity(settingsIdentity);
    const QString vaultPath = QDir(profile).filePath(QStringLiteral("credentials.keefi-vault"));
    if (smoke && QFileInfo::exists(vaultPath)) return 5;

    keefi::Vault vault(vaultPath, selected.identityScheme, nullptr);
    keefi::AccountModel accounts(&vault);
    keefi::PortfolioController portfolio(vault);
    std::unique_ptr<keefi::SystemNotificationSink> notifications(smoke?
        keefi::makeUnavailableSystemNotificationSink():keefi::makeSystemNotificationSink());
    QObject::connect(&app,&QGuiApplication::applicationStateChanged,notifications.get(),[sink=notifications.get()](Qt::ApplicationState state){
        if(state==Qt::ApplicationActive)sink->refreshPermission();
    });
    keefi::ChartPhrasePack phrases;
    keefi::ChartAlertEngine alerts(notifications.get());
    keefi::NetworkTradingTransport alertTransport,alertCatalogTransport;
    keefi::ChartInstrumentCatalog alertCatalog(&alertCatalogTransport);
    keefi::ChartAlertFeed alertFeed(&alertTransport,&alertCatalog);
    keefi::NetworkTradingTransport chartStockTransport,alertStockTransport;
    keefi::StockChartSource chartStock(&chartStockTransport),alertStock(&alertStockTransport);
    keefi::NetworkTradingTransport studyStockTransport,studyFeedTransport,chartPublicContextTransport;
    keefi::StockChartSource studyStock(&studyStockTransport);
    keefi::ChartStudyFeed studyFeed(&studyFeedTransport,&alertCatalog);
    keefi::NetworkTradingTransport rankingFuturesTransport;keefi::RankingFuturesFeed rankingFutures(&rankingFuturesTransport);rankingFutures.setCatalog(&alertCatalog);
    keefi::ChartPublicContextSource chartPublicContext(&chartPublicContextTransport,
        smoke?std::unique_ptr<keefi::PublicCandleSocket>{}:keefi::makeNetworkPublicFlowSocket());
    const auto studyPacer=std::make_shared<keefi::ChartStudyRequestPacer>();
    studyFeed.setRequestPacer(studyPacer);chartPublicContext.setRequestPacer(studyPacer);rankingFutures.setRequestPacer(studyPacer);
    QObject::connect(&chartPublicContext,&keefi::ChartPublicContextSource::liquidationObserved,&alerts,
        [&alerts](const QString& identity,const QString& id,const QString& detail,qint64 at,qint64 received){
            alerts.observePublicStudy({keefi::WatchlistWorkspaceList::identityFromKey(identity),"liquidation_feed",id,detail,
                at,received,keefi::MovementDirection::Unknown,true});
        });
    keefi::NetworkTradingTransport coinFlowTransport,coinMinuteTransport;
    keefi::CoinFlowSource coinFlow(&coinFlowTransport,
        smoke?std::unique_ptr<keefi::PublicCandleSocket>{}:keefi::makeNetworkPublicFlowSocket(),
        smoke?std::unique_ptr<keefi::PublicCandleSocket>{}:keefi::makeNetworkPublicFlowSocket());
    keefi::MarketCandles coinMinutes(&coinMinuteTransport); // Dedicated native1m REST; no extra trade socket.
    keefi::NetworkTradingTransport marketCompareTransport; // 시장 비교 탭: keyless klines, USD-M catalog, ECB.
    keefi::MarketCompareSource marketCompare(&marketCompareTransport);
    marketCompare.setCollectorRoot(keefi::collectorDefaultRoot()); // Read-only; the collector writes there.
    keefi::NetworkTradingTransport tossQuotesTransport;
    keefi::TossMarketStream tossQuotes(nullptr,&tossQuotesTransport);
    keefi::NetworkTradingTransport kiwoomQuotesTransport;
    keefi::KiwoomMarketStream kiwoomQuotes(nullptr,&kiwoomQuotesTransport,
        smoke?std::unique_ptr<keefi::KiwoomMarketSocket>{}:keefi::makeNetworkKiwoomMarketSocket());
    keefi::ProfileSettings chartSettings;
    // Public Upbit KRW exploration works without a vault; the start check never reads the network.
    keefi::NetworkTradingTransport marketTransport;
    keefi::PublicMarketCatalog markets(&marketTransport);
    keefi::NetworkTradingTransport boardTransport, binanceRankingTransport;
    keefi::PublicEquityBoard board(&boardTransport, nullptr, &binanceRankingTransport);
    keefi::NetworkTradingTransport historyTransport;
    keefi::PublicMarketHistory history(&historyTransport);
    keefi::NetworkTradingTransport flowHistoryTransport;
    keefi::MarketFlowHistory flowHistory(&flowHistoryTransport);
    keefi::NetworkTradingTransport metricsTransport;
    keefi::MarketMetrics metrics(&metricsTransport);
    keefi::NetworkTradingTransport todayUpbitTransport, todayBinanceTransport, todayFxTransport;
    keefi::TodayBtcPremium todayBtcPremium(&todayUpbitTransport, &todayBinanceTransport, &todayFxTransport);
    keefi::NetworkTradingTransport coinbaseTransport;
    keefi::NetworkTradingTransport coinbaseBridgeTransport;
    keefi::CoinbaseQuotes coinbase(&coinbaseTransport,nullptr,&coinbaseBridgeTransport);
    // Today world board: one transport per lane (Naver quotes, ECB table, MOF table).
    keefi::NetworkTradingTransport worldQuoteTransport, worldEcbTransport, worldMofTransport;
    keefi::WorldMarkets worldMarkets(&worldQuoteTransport, &worldEcbTransport, &worldMofTransport);
    keefi::NetworkTradingTransport overviewTransport;
    keefi::MarketOverview overview(&overviewTransport);
    keefi::NetworkTradingTransport calendarTransport;
    keefi::OfficialCalendar calendar(&calendarTransport);
    keefi::NetworkTradingTransport earningsTransport;
    keefi::EarningsCalendar earnings(&earningsTransport);
    keefi::NetworkTradingTransport candlesTransport;
    keefi::MarketCandles candles(&candlesTransport,smoke?std::unique_ptr<keefi::PublicCandleSocket>{}:keefi::makeNetworkPublicCandleSocket());
    // G9-B: completed public candles, encrypted with a vault-derived key; idle while locked.
    keefi::PriceArchive priceArchive(&vault,QFileInfo(vault.filePath()).dir().filePath("prices.keefi"));
    if(!smoke)priceArchive.watch(&candles);
    keefi::NetworkTradingTransport chartContractTransport;
    keefi::NetworkTradingTransport noticesTransport;
    keefi::OfficialNotices notices(&noticesTransport);
    keefi::UpbitAnnouncements upbitNotices(vault); // Realtime official notices; signs only with the vault Upbit key.
    keefi::PublicQuoteStream upbitPrices(keefi::PublicQuoteStream::Provider::Upbit);
    keefi::PublicQuoteStream binancePrices(keefi::PublicQuoteStream::Provider::Binance);
    keefi::PublicQuoteStream chartRowPrices(keefi::PublicQuoteStream::Provider::Binance); // Chart list rows only.
    keefi::RapidMoveMonitor rapidMoves;
    if(!smoke){
        QObject::connect(&upbitPrices,&keefi::PublicQuoteStream::ticks,&rapidMoves,&keefi::RapidMoveMonitor::observe);
        QObject::connect(&rapidMoves,&keefi::RapidMoveMonitor::eventReady,&alerts,&keefi::ChartAlertEngine::observePublicStudy);
        QObject::connect(&alerts,&keefi::ChartAlertEngine::changed,&rapidMoves,[&]{rapidMoves.setEnabled(alerts.observationEnabled("rapid_move"));});
    }
    // All borrowed backends/settings outlive the window and its queued setup.
    keefi::LocalWindow window(vault, accounts);
    if (!smoke) window.setBackupRestoreHandler([&](
        const QString& backupPath, const QString& password, QString* error) {
        using Outcome = keefi::BackupRestoreOutcome;
        if (!hardened || !security.ready()) {
            if (error) *error = hardened ? security.statusCode() : hardeningError;
            return Outcome::Failed;
        }
        if (vault.exists() || vault.isUnlocked()) {
            if (error) *error = QStringLiteral("백업은 금고가 없는 빈 프로필에만 복원할 수 있습니다.");
            return Outcome::Failed;
        }
        QString backupIdentity;
        if (!keefi::Vault::authenticatedBackupIdentity(backupPath, password, &backupIdentity, error))
            return Outcome::Failed;
        if (backupIdentity == selected.identityScheme)
            return vault.restoreBackup(backupPath, password, error) ? Outcome::Restored : Outcome::Failed;

        // The process identity is already fixed. Restore another identity only
        // into an inactive copy, then publish it before any backend on restart.
        auto session = keefi::ProfileRestoreSession::begin(
            selected, instanceLock, QCoreApplication::applicationVersion(), error);
        const keefi::ProfileBackupRestorer restore = [](
            const QString& stage, const QString& source, const QString& secret,
            keefi::ProfileConversionReceipt* receipt, QString* restoreError) {
            if (receipt) *receipt = {};
            keefi::StorageConversionReceipt stored;
            if (!keefi::StorageMigration::restoreBackupStagedCopy(stage, source, secret, &stored, restoreError))
                return false;
            if (receipt) {
                receipt->verified = stored.verified;
                receipt->identityScheme = stored.identityScheme;
                receipt->evidence = stored.evidence;
            }
            return true;
        };
        if (!session || !session->copy(error)
            || !session->restore(backupPath, password, restore, error)
            || !session->verify(error) || !session->prepareForRestart(error))
            return Outcome::Failed;
        return Outcome::PreparedForRestart;
    });
    if(!smoke)window.setQuitPreparation([&](std::function<void(bool)> done){
        priceArchive.prepareToQuit([done=std::move(done)](bool saved,const QString&){done(saved);});
    },[&]{priceArchive.cancelQuit(true);});
    window.setRapidMoveMonitor(&rapidMoves);
    window.setPortfolioController(&portfolio);
    if(!smoke){
        portfolio.setBinanceBalancePilot(true);
        markets.setStream(&upbitPrices);portfolio.setBinancePriceStream(&binancePrices);
        portfolio.setPublicMarketCatalog(&markets);
    }
    window.setPublicMarketCatalog(&markets, !smoke);
    window.setPublicEquityBoard(&board);
    if (!smoke) {
        // Appearance is ready for the first gate paint; the hidden views follow.
        window.setChartSettings(&chartSettings);
        window.setDeferredWorkbenchInitializer([&] {
            window.setPublicMarketHistory(&history);
            window.setMarketMetrics(&metrics,&coinbase);
            window.setTodayBtcPremium(&todayBtcPremium);
            window.setWorldMarkets(&worldMarkets);
            window.setMarketOverview(&overview);
            window.setOfficialCalendar(&calendar);
            window.setEarningsCalendar(&earnings);
            notices.setUpbitAnnouncements(&upbitNotices);
            window.setOfficialNotices(&notices);
            window.setMarketCandles(&candles);
            window.setMarketFlowHistory(&flowHistory);
            window.setCoinFlowSource(&coinFlow);
            window.setCoinFlowMinuteModel(&coinMinutes);
            window.setChartContractTransport(&chartContractTransport);
            window.setTossMarketStream(&tossQuotes);
            window.setKiwoomMarketStream(&kiwoomQuotes);
            window.setChartRowQuoteStream(&chartRowPrices);
            window.setStockChartSources(&chartStock,&alertStock,&studyStock);
            window.setChartStudyFeed(&studyFeed);
            window.setRankingFuturesFeed(&rankingFutures);
            window.setChartPublicContextSource(&chartPublicContext);
            alertFeed.setStockSource(&alertStock);
            window.setChartAlerts(&alerts,&alertFeed,notifications.get(),&phrases,&chartSettings);
            window.setMarketCompareSource(&marketCompare);
        });
    }
    if (!smoke) board.refresh(); // The start check never reads the network.
    auto refreshSecurity = [&] {
        window.setSecurityStatus(hardened && security.ready(), hardened ? security.statusCode() : hardeningError);
    };
    refreshSecurity();
    QObject::connect(&security, &keefi::PlatformSecurity::statusChanged, &window, refreshSecurity);
    QObject::connect(&window, &keefi::LocalWindow::securityCheckRequested, &security, [&] {
        security.initialize();
        refreshSecurity();
    });
    QObject::connect(&security, &keefi::PlatformSecurity::lockRequested, &window, [&] {
        window.lockAndClear();
    });
    QObject::connect(&window, &keefi::LocalWindow::keepAwakeRequested, &security, [&](bool enabled) {
        security.setKeepAwake(enabled);
        window.setKeepAwakeState(security.keepAwake());
    });
    QObject::connect(&window, &keefi::LocalWindow::quitRequested, &app, &QCoreApplication::quit);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] {
        // Never restart an unbounded synchronous flush after the 3-second budget.
        // Normal Quit is filtered through LocalWindow; forced exit only cancels.
        priceArchive.cancelQuit();
        alertFeed.setRunning(false);window.setCoinFlowSource(nullptr);window.setCoinFlowMinuteModel(nullptr);
        coinFlow.stop();coinMinutes.setSelection({},60000);
        studyFeed.stop();chartPublicContext.setDemand(false,false);
        rapidMoves.setEnabled(false);
        chartStock.stop();alertStock.stop();notifications->stop();
        window.lockAndClear();
        security.setKeepAwake(false);
    });
    window.showVaultGate();
    window.show();

    if (smoke) {
        QTimer::singleShot(700, &window, [&] {
            const auto* startupGate = window.findChild<QWidget*>(QStringLiteral("vaultGate"));
            const bool startupGateVisible = startupGate && startupGate->isVisible();
            QJsonObject result{{"application", applicationName}, {"stage", "G8"}, {"read_only_build", keefi::readOnlyBuild()},
                {"startup_phase", "vault_gate"}, {"process_hardened", hardened},
                {"build_channel", developmentBuild ? "development" : "production"},
                {"application_version", QCoreApplication::applicationVersion()},
                {"window_title", window.windowTitle()},
                {"visible", window.isVisible()}, {"startup_vault_gate_visible", startupGateVisible},
                {"account_count", accounts.rowCount()},
                {"vault_exists", vault.exists()}, {"vault_unlocked", vault.isUnlocked()},
                {"security_ready", hardened && security.ready()}, {"security_status", security.statusCode()}};
            bool ok = hardened && security.ready() && window.isVisible() && startupGateVisible && accounts.rowCount() == 0 &&
                !vault.exists() && !vault.isUnlocked();
#ifdef Q_OS_WIN
            // Test the deployed TLS plugin without authenticating or contacting a broker.
            const bool schannel = QSslSocket::isClassImplemented(QSsl::ImplementedClass::Socket, QStringLiteral("schannel"));
            result.insert(QStringLiteral("windows_schannel_ready"), schannel && QSslSocket::supportsSsl());
            ok = ok && result.value(QStringLiteral("windows_schannel_ready")).toBool();
#endif
            if (parser.isSet(QStringLiteral("smoke-screenshot"))) {
                const QString screenshotPath = parser.value(QStringLiteral("smoke-screenshot"));
                ok = !QFileInfo::exists(screenshotPath) && window.grab().save(screenshotPath) && ok;
            }
            QFile report(parser.value(QStringLiteral("smoke-report")));
            const QByteArray data = QJsonDocument(result).toJson();
            ok = report.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                report.write(data) == data.size() && report.flush() && ok;
            app.exit(ok ? 0 : 6);
        });
    }
    return app.exec();
}
