#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QHash>
#include <QSet>
#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/PortfolioDisplayFx.h"
#include "keefi/ValuationBundle.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/RankingStudies.h"
#include "keefi/HoldingMarketResolver.h"
#include "keefi/AssetShareCard.h"
#include <functional>

class QSettings;
class QPaintEvent;
class QCheckBox;
class QCloseEvent;
class QDialog;
class QBoxLayout;
class QComboBox;
class QGridLayout;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QTimer;
class QListView;
class QPushButton;
class QMenu;
class QTableWidget;
class QTreeView;
class QScrollArea;

namespace keefi {
struct PortfolioConnection;

class ExploreEvidenceFooter;
class ExploreTodayView;
class ExploreCoinSummary;
class PublicQuoteStream;
class MarketMapWidget;
class AssetShareDialog;
class SpotlightCard;
class PublicEquityBoard;
class PublicMarketHistory;
class ExploreWatchlist;
class PortfolioAllocation;
class PortfolioAssetModel;
class LoadProgressBar;
class PortfolioDayView;
class MarketMetrics;
class TodayBtcPremium;
class WorldMarkets;
class MarketFlowHistory;
class CoinbaseQuotes;
class MarketMetricsView;
class MarketOverview;
class OfficialCalendar;
class EarningsCalendar;
class OfficialNotices;
class MarketCandles;
class StockChartSource;
class CoinFlowSource;
class ChartPublicContextSource;
class ChartStudyFeed;
class RankingFuturesFeed;
class RankingStudyRunner;
class MarketBreadthMonitor;
class RapidMoveMonitor;
class ChartInstrumentCatalog;
class ChartWorkspace;
class MarketCompareSource;
class MarketCompareView;
class BackgroundTray;
class ChartAlertEngine;
class ChartAlertFeed;
class SystemNotificationSink;
class ChartPhrasePack;
class ChartAlertsPanel;
class WatchlistWorkspaceList;
class TradingTransport;
class TossMarketStream;
class KiwoomMarketStream;
class MovementColorPolicy;


class AccountModel;
struct AccountRecord;
class ConnectionListModel;
class Vault;
class TradingController;
class BrokerTradingController;
class FuturesTradingController;
class AssetsWindow;
class PortfolioController;
class InfoButton;
class AssetListFilter;
class RecordTransferDialog;
class SwapJournal;
class AssetDetailPanel;
class ListDetailView;
class PublicMarketCatalog;
class ValuationPreferences;
class VaultGate;
class ReleaseUpdater;

enum class StockChartConnectionState;
enum class BackupRestoreOutcome { Failed, Restored, PreparedForRestart };

class LocalWindow final : public QMainWindow {
    Q_OBJECT
  public:
    // Explore content follows the window up to this width; wider windows centre it.
    static constexpr int ExploreMaximumWidth = 2400;
    // Ranking watchlist column: a quarter of the ranking page, 420–460px (name, a nine-digit won price
    // and change fit without sideways scrolling), leaving the three markets their side-by-side width.
    static int rankingWatchlistWidth(int pageWidth) { return pageWidth >= 1720 ? 424 : 400; }
    explicit LocalWindow(Vault& vault, AccountModel& accounts, QWidget* parent = nullptr, BackgroundTray* trayBackend = nullptr);
    ~LocalWindow() override;
    // Optional startup-only view wiring. Install before showVaultGate()/show().
    // Runs once after the first gate paint, or synchronously before leaving it.
    void setDeferredWorkbenchInitializer(std::function<void()> initializer);
    // Synchronous owner callback. Passwords are borrowed for this call only.
    // A staged restore stays locked until the next startup publishes its profile.
    void setBackupRestoreHandler(std::function<BackupRestoreOutcome(const QString&, const QString&, QString*)>);
    // Normal quit only, capped at 3 seconds. Completion reports save status but
    // never vetoes closing. Security lock bypasses preparation; cancel never waits.
    void setQuitPreparation(std::function<void(std::function<void(bool)>)>, std::function<void()> cancel = {});
    void setReleaseUpdater(ReleaseUpdater* updater, std::function<bool(QString*)> startInstaller);
    void setPortfolioController(PortfolioController* controller);
    void setPortfolioStreamGraceMs(int ms) { portfolioStreamGraceMs_ = qMax(0, ms); }
    // Public exploration tab (default home). autoLoad=false avoids network reads, e.g. in start checks.
    void setPublicMarketCatalog(PublicMarketCatalog* catalog, bool autoLoad = true);
    // Keyless public boards (coin market cap, Korean and US stocks) for the market map.
    void setPublicEquityBoard(PublicEquityBoard* board);
    void setPublicMarketHistory(PublicMarketHistory* history);
    void setMarketMetrics(MarketMetrics* metrics,CoinbaseQuotes* coinbase=nullptr, PublicQuoteStream* upbit=nullptr, PublicQuoteStream* binance=nullptr);
    void setMarketFlowHistory(MarketFlowHistory*);
    void setTodayBtcPremium(TodayBtcPremium*); // Independent, fixed BTC public reference; no selected-asset coupling.
    void setWorldMarkets(WorldMarkets*); // Keyless world indices, commodities, FX and the JGB yield; read only while Today or a chart is shown.
    void setMarketOverview(MarketOverview* overview);
    void setOfficialCalendar(OfficialCalendar* calendar);
    void setEarningsCalendar(EarningsCalendar* calendar);
    void setOfficialNotices(OfficialNotices* notices);
    void setChartInstrumentTransport(TradingTransport*); // Dedicated public-only lane; inject before chart creation.
    void setChartSettings(QSettings*); // Optional borrowed profile settings; inject before chart creation.
    void setRapidMoveMonitor(RapidMoveMonitor*);
    void setMarketCandles(MarketCandles* candles);
    void setStockChartSource(StockChartSource*);
    void setStockChartSources(StockChartSource* chart, StockChartSource* alerts, StockChartSource* studies=nullptr);
    void setChartPublicContextSource(ChartPublicContextSource*);
    void setChartStudyFeed(ChartStudyFeed*);
    void setRankingFuturesFeed(RankingFuturesFeed*);
    void setCoinFlowSource(CoinFlowSource*);
    void setCoinFlowMinuteModel(MarketCandles*); // Dedicated native1m REST model for higher-timeframe charts.
    void setChartContractTransport(TradingTransport*);
    void setTossMarketStream(TossMarketStream* stream);
    void setKiwoomMarketStream(KiwoomMarketStream* stream);
    // Public Binance tickers for chart list rows (visible, held, selected); public prices only.
    void setChartRowQuoteStream(PublicQuoteStream* stream);
    void setChartAlerts(ChartAlertEngine*,ChartAlertFeed*,SystemNotificationSink*,ChartPhrasePack*,QSettings*);
    void setMarketCompareSource(MarketCompareSource*); // 시장 비교 탭; inserted after the chart tab.
    // keefi-valuation-bundle/1 for the valuation now on screen; empty while locked, loading,
    // or showing a dated saved view after unlock.
    QJsonObject valuationDiagnostics();

  public slots:
    // Call after service wiring, before show(). Existing workbench callers may
    // continue to present their selected page without a startup gate.
    void showVaultGate();
    void refreshState();
    void lockAndClear();
    void setSecurityStatus(bool ready, const QString& statusCode = {});
    void setKeepAwakeState(bool enabled);

  signals:
    void securityCheckRequested();
    void keepAwakeRequested(bool enabled);
    void quitRequested();

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void paintEvent(QPaintEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    void openVaultDialog(int mode, std::function<void()> afterSuccess = {}, QDialog* owner = nullptr);
    void initializeWorkbenchOnce();
    void leaveVaultGate(bool publicOnly);
    void prepareUnlockedPortfolio();
    void openAccountDialog(bool replacement, const QString& chartIdentity = {}, qint64 chartInterval = 0);
    void openChartBrokerConnection(const QString& identity, qint64 interval);
    void returnToBrokerChart(const QString& identity, qint64 interval, const QString& connectionId);
    void removeSelectedAccount();
    void confirmEraseVault(); // "정말 지우겠습니까?" then Vault::erase; reached from 옵션 · 보안.
    void refreshSelection();
    void refreshAccountList();
    AccountRecord selectedAccount() const;
    void selectConnection(const QString& id);
    void showConnectionAssets(const QString& id);
    void syncPortfolioAccountFilter(const QList<PortfolioConnection>& connections);
    void syncConnectionLayout();
    void closeSensitiveDialog();
    void requestQuit();
    void finishPreparedQuit(bool saved);
    void applyLocalTheme();
    void restoreMovementColors();
    void restoreMoneyCurrency();
    PortfolioMoneyDisplay currentMoneyDisplay(qint64 asOfMs) const;
    void refreshMoneyPresentation(bool force = false);
    void refreshMovementPresentation();
    void connectPublicChartLink(AssetDetailPanel*);
    void refreshPublicDetailPresentation(AssetDetailPanel*,const QString& section,const QString& id,const QString& sourceKey,const QString& unit,const QString& chartIdentity);
    void refreshPortfolio();
    void queuePortfolioPresentation();
    void refreshPortfolioPresentation();
    bool hasSelectedPortfolioReadRegistration() const;
    bool deferPortfolioLoadingProjection() const;
    void flushPortfolioLoadingProjection();
    void resetPortfolioLoadProgress();
    void updatePortfolioLoadProgress(const QList<PortfolioConnection>&);
    void refreshDayHistory(bool request = true);
    void refreshPortfolioEvidence();
    void syncExploreEvidence();
    void queueLegacyExploreEvidence();
    void refreshLegacyExploreEvidence(bool clearingPrivate = false);
    void selectMarketMetric(const QString& section, const QString& asset);
    void applyPortfolioFilter(bool thresholdOnly = false);
    void renderPortfolioAssets();
    void syncPortfolioLayout();
    void showPortfolioDetail();
    std::optional<PortfolioGroupView> selectedPortfolioGroup() const;
    void openAssetShare();
    void refreshAssetShare();
    void clearAssetShare();
    int refreshConnectionStatus(); // Returns the number of connections needing user action.
    void refreshExplore();
    void syncExplorePage();
    void filterExploreRows(const QString& query);
    void refreshPublicObservations();
    void refreshRankingStudies();
    void refreshExploreHoldings();
    void syncMarketFlowHistory();
    // Market flow sources load while the window is open, not only on their page; the page shows
    // a progress line until the selected coin's first full answer.
    void syncMarketFlowBackground();
    void syncMarketFlowAlerts();
    void syncMarketFlowPrivateContext();
    void clearExplorePrivate();
    void showExploreDetail();
    // G6 workbench chrome: header tabs mirror the hidden page tab bar, and the
    // trust pill summarises how the displayed numbers were produced.
    void syncHeaderTabs();
    void refreshMarketMap();
    void showMapSelection();
    void showPublicDetail(AssetDetailPanel* panel, const QString& section, const QString& id);
    // Ranking and watchlist rows open the chart: Upbit KRW first, then Binance spot USDT for
    // coins; verified broker instruments for stocks. Otherwise a short reason, no navigation.
    void openPublicChart(const QString& section, const QString& id);
    HoldingChartRow publicChartRoute(const QString& section, const QString& id) const;
    // Spotlight: CRYPTO · KOSPI · NASDAQ cards, five rows per page, up to four pages.
    void refreshSpotlight();
    void refreshDashboard();
    void openMarketAlertSettings();
    void layoutSpotlight();
    void layoutExploreToday();
    void refreshWatchlist(bool scheduleHistory=true);
    void refreshChartWatchlists();
    void releaseChartRowQuotes();
    HoldingMarketCatalog holdingMarketCatalog(const QList<WatchlistIdentity>& verifiedStocks = {}) const;
    HoldingMarketPreferences holdingMarketPreferences() const;
    void clearHoldingMarketSession();
    void syncChartSubscriptions();
    void syncChartAlerts();
    StockChartConnectionState stockChartConnectionState(BrokerTradingController** owner=nullptr) const;
    void syncStockChartSources();
    void refreshStockConnectionPresentation();
    void syncKiwoomMarketSource();
    void revokeKiwoomMarketAuthorization();
    void syncCoinFlowDemand();
    void syncPublicMarketMetadataDemand();
    void revokeStockChartAuthorization();
    void syncChartRisk();
    void clearChartAlertPrivate();
    void syncChartTab();
    void syncChartPresentation();
    // App-owned settings state outlives these disposable option pages.
    void openOptions();
    void openOptionsPage(const QString& page, const QString& listId = {});
    void exportValuationDiagnostics(QWidget* parent);
    void refreshTrust();
    void showTrustProcess();
    ExploreTodayView* todayDashboard_ = nullptr;
    QWidget* exploreSummaryHeader_ = nullptr;
    MarketBreadthMonitor* marketBreadth_ = nullptr;
    QPointer<RapidMoveMonitor> rapidMoves_;
    bool publicObservationsRefreshing_ = false;
    ExploreCoinSummary* exploreCoinSummary_ = nullptr;
    QWidget* exploreCommonSidebar_ = nullptr;
    MarketOverview* dashboardOverview_ = nullptr;
    OfficialNotices* dashboardNotices_ = nullptr;
    bool dashboardRefreshing_ = false;
    Vault& vault_;
    ValuationPreferences* valuationPreferences_ = nullptr;
    AccountModel& accounts_;
    bool securityReady_ = false;
    bool quitting_ = false;
    ReleaseUpdater* releaseUpdater_ = nullptr;
    std::function<bool(QString*)> startUpdateInstaller_;
    bool restartingForUpdate_ = false;
    bool quitPreparing_ = false;
    std::function<void(std::function<void(bool)>)> quitPreparation_;
    std::function<void()> quitCancellation_;
    QString securityCode_;
    QLabel* vaultStatus_ = nullptr;
    QLabel* platformStatus_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* accountDetails_ = nullptr;
    QLabel* feedback_ = nullptr;
    QListView* accountList_ = nullptr;
    ConnectionListModel* connectionListModel_ = nullptr;
    QComboBox* accountMarket_ = nullptr;
    QWidget* accountDetail_ = nullptr;
    QPushButton* accountBack_ = nullptr;
    QLabel* accountReadState_ = nullptr;
    QLabel* accountReadTime_ = nullptr;
    QLabel* accountIssue_ = nullptr;
    QLabel* accountProviderReconciliation_ = nullptr;
    QLabel* accountActionScope_ = nullptr;
    bool connectionDetailShown_ = false;
    QString selectedConnectionId_;
    QPushButton* createVault_ = nullptr;
    QPushButton* unlockVault_ = nullptr;
    QPushButton* lockVault_ = nullptr;
    QPushButton* changePassword_ = nullptr;
    QPushButton* addAccount_ = nullptr;
    QPushButton* replaceAccount_ = nullptr;
    QPushButton* removeAccount_ = nullptr;

    QCheckBox* background_ = nullptr;
    QCheckBox* keepAwake_ = nullptr;
    QPointer<BackgroundTray> tray_;
    QMenu* trayMenu_ = nullptr;
    bool closeHintSeen_ = false;
    QPointer<QDialog> sensitiveDialog_;

    AssetsWindow* assetsWindow_ = nullptr;
    ExploreEvidenceFooter* portfolioEvidence_ = nullptr;
    bool portfolioEvidenceHasComparison_ = false;
    QLabel* portfolioTotalEvidence_ = nullptr;
    QPointer<RecordTransferDialog> recordTransferDialog_;
    SwapJournal* historicalSwaps_ = nullptr;
    PortfolioController* portfolio_ = nullptr;
    ChartWorkspace* chartWorkspace_ = nullptr;
    MarketCompareView* marketCompareView_ = nullptr;
    QPointer<WatchlistWorkspaceList> chartLists_;
    QPointer<MarketCandles> chartCandles_;
    QPointer<CoinFlowSource> chartCoinFlow_;
    QTimer* chartCoinFlowRefresh_ = nullptr;
    QPointer<MarketCandles> chartFlowMinutes_;
    bool chartCoinFlowSyncing_=false,chartCoinFlowStopping_=false;
    QPointer<StockChartSource> chartStockSource_,alertStockSource_,studyStockSource_;
    QPointer<ChartPublicContextSource> chartPublicContext_;
    QPointer<ChartStudyFeed> chartStudyFeed_;
    QPointer<RankingFuturesFeed> rankingFuturesFeed_;
    RankingStudyRunner* rankingStudyRunner_=nullptr;
    bool rankingRefreshing_=false,rankingQueued_=false;
    QPointer<BrokerTradingController> stockChartBroker_;
    bool stockChartBorrowBlocked_ = true, stockChartSyncing_ = false;
    QString stockChartConnectionMessage_;
    QPointer<QSettings> chartSettings_;
    MovementColorPolicy* movementColors_ = nullptr;
    QByteArray movementPublicState_;
    bool restoringMovementColors_ = false;
    MoneyCurrency moneyCurrency_ = MoneyCurrency::Krw;
    PortfolioMoneyDisplay portfolioMoneyDisplay_;
    PortfolioDisplayFx portfolioDisplayFx_;
    QLabel* portfolioCurrencyStatus_ = nullptr;
    InfoButton* portfolioCurrencyInfo_ = nullptr;
    QPointer<ChartAlertEngine> chartAlerts_;
    QPointer<ChartAlertFeed> chartAlertFeed_;
    QPointer<ChartAlertsPanel> chartAlertsPanel_;
    QPointer<QSettings> chartAlertSettings_;
    QByteArray chartAlertPublicState_,chartAlertUiState_;
    bool chartAlertsSyncing_ = false;
    bool chartRiskSyncing_ = false;
    QHash<QString,QString> chartRiskTokens_;
    QPointer<TossMarketStream> chartTossStream_;
    QPointer<KiwoomMarketStream> chartKiwoomStream_;
    QPointer<PublicQuoteStream> chartRowQuotes_;
    QHash<QString, PublicTick> chartRowTicks_;
    QHash<QString, QPair<QString, qint64>> flowKoreaGaps_; // Market flow 김프 per coin: exact percent and newest source time.
    QStringList chartRowQuoteMarkets_;
    QTimer* chartRowQuoteTimer_ = nullptr;
    QString kiwoomCountry_ = "US";
    quint64 kiwoomEpoch_ = 1;
    bool kiwoomSyncing_ = false, kiwoomRevoking_ = false, kiwoomStopping_ = false;
    QByteArray chartPublicState_;
    QByteArray chartStudyPublicState_;
    QByteArray chartDrawingPublicState_;
    ChartInstrumentCatalog* chartInstruments_ = nullptr;
    QPointer<TradingTransport> chartInstrumentTransport_;
    QStringList chartVisibleIdentities_, chartPublicCoinIdentities_, chartObservedIdentities_;
    bool chartListsRefreshing_ = false, chartPrivateBlocked_ = false, chartSourceResetting_ = false;
    QStringList chartVerifiedUpbitMarkets_;
    bool chartMarketListAvailable_ = false;
    QHash<AssetKey,WatchlistIdentity> holdingMarketSession_; // Private session choice; never persisted.
    QList<HoldingChartRow> holdingRows_;
    QPointer<AssetShareDialog> assetShareDialog_;
    AssetShareSnapshot assetShareValue_;
    AssetKey assetShareKey_;
    QStringList assetShareProviders_;
    quint64 assetShareGeneration_=0;
    bool assetShareRefreshing_=false;
    QWidget* accountPanel_ = nullptr;
    QLabel* portfolioSummary_ = nullptr;
    QLabel* portfolioProgress_ = nullptr;
    QTreeView* portfolioAssets_ = nullptr;
    PortfolioAssetModel* portfolioModel_ = nullptr;
    AssetListFilter* portfolioFilter_ = nullptr;
    AssetDetailPanel* portfolioDetail_ = nullptr;
    ListDetailView* portfolioListDetail_ = nullptr;
    QTableWidget* connectionStatus_ = nullptr;
    PortfolioPresentation portfolioView_; // Current evidence, also used by diagnostics.
    ValuationCapture lastValuation_; // Inputs and outputs of the last on-screen valuation.
    quint64 exportGeneration_ = 0; // Advances on every lock; an export only writes within its own generation.
    QPointer<QDialog> exportDialog_;
    PortfolioPresentation portfolioDisplayView_, portfolioCachedView_;
    QLabel* portfolioRefreshNote_ = nullptr;
    QLabel* portfolioLoading_ = nullptr;
    QWidget* portfolioLoadingPane_ = nullptr;
    QPushButton* portfolioPendingDetails_ = nullptr;
    LoadProgressBar* portfolioLoadProgress_ = nullptr;
    QLabel *portfolioLoadStage_ = nullptr, *portfolioLoadTaskCount_ = nullptr, *portfolioLoadPercent_ = nullptr;
    QSet<QString> portfolioLoadConnections_, portfolioLoadCompleted_;
    bool portfolioLoadCaptured_ = false, portfolioLoadPricesComplete_ = false, portfolioLoadProjectionComplete_ = false;
    bool portfolioPresentationQueued_ = false;
    bool portfolioLoadingProjectionDirty_ = false; // Display-only latest DTO flush; never persisted.
    quint64 portfolioPresentationGeneration_ = 0;
    QString portfolioVerifiedScope_; // Opaque session receipt only; no saved amounts or account metadata.
    bool portfolioRestoreCaptured_ = false, portfolioInitialRefresh_ = false;
    bool portfolioSettleQueued_ = false;
    // First load: after account reads and REST prices finish, a held asset whose first
    // stream quote never arrives may hold the screen only this long (Dev 96).
    qint64 portfolioStreamGraceStartedMs_ = 0;
    int portfolioStreamGraceMs_ = 8000;
    qint64 portfolioInitialStartedMs_ = 0;
    QList<PortfolioGroupView> portfolioScopeGroups_;
    QComboBox* portfolioClass_ = nullptr;
    QComboBox* portfolioAccount_ = nullptr;
    PortfolioAllocation* portfolioAllocation_ = nullptr;
    PortfolioDayView* portfolioDayView_ = nullptr;
    QBoxLayout* portfolioHead_ = nullptr;
    QWidget* portfolioScopeControls_ = nullptr;
    QWidget* connectionsPage_ = nullptr;
    PublicMarketCatalog* markets_ = nullptr;
    QTableWidget* exploreTable_ = nullptr;
    AssetDetailPanel* exploreDetail_ = nullptr;
    ListDetailView* exploreListDetail_ = nullptr;
    QString portfolioRevision_;
    bool portfolioAssetsDirty_ = false; // Transient hidden-row projection; never persisted.
    bool windowShownOnce_ = false;
    QTimer* portfolioExpiryTimer_ = nullptr;
    qint64 portfolioNextExpiryMs_ = 0; // Next instant at which some displayed evidence turns stale.
    bool portfolioComplete_ = false;
    QWidget* headerTabs_ = nullptr;
    QStackedWidget* startupStack_ = nullptr;
    QWidget* workbenchRoot_ = nullptr;
    VaultGate* vaultGate_ = nullptr;
    std::function<BackupRestoreOutcome(const QString&, const QString&, QString*)> backupRestoreHandler_;
    std::function<void()> deferredWorkbenchInitializer_;
    bool workbenchInitializationQueued_ = false;
    QPushButton* trustPill_ = nullptr;
    QLabel* portfolioTotal_ = nullptr;
    QWidget* portfolioGaps_ = nullptr;
    QLabel* accountTitle_ = nullptr;
    QLabel* accountEnvironment_ = nullptr;
    QWidget* accountSteps_ = nullptr;
    QWidget* accountMore_ = nullptr;
    QBoxLayout* homeTools_ = nullptr;
    QLabel* portfolioTotalUnit_ = nullptr;
    QPushButton* problemBanner_ = nullptr;
    QPushButton* refreshSelected_ = nullptr;
    QWidget* homeBody_ = nullptr;
    QWidget* portfolioWorkbench_ = nullptr;
    QWidget* lockedState_ = nullptr;
    MarketMapWidget* marketMap_ = nullptr;
    AssetDetailPanel* mapDetail_ = nullptr;
    ListDetailView* mapListDetail_ = nullptr;
    QStackedWidget* exploreStack_ = nullptr;
    ExploreEvidenceFooter* exploreBoardEvidence_ = nullptr;
    QScrollArea* exploreEvidenceScroll_ = nullptr;
    bool exploreEvidenceQueued_ = false;
    bool searchRowsDirty_ = true, mapSectionsDirty_ = true, rankingCardsDirty_ = true;
    bool marketMapSubscribed_ = false, exploreLayoutPending_ = false;
    int rankingGridColumns_ = 0;
    bool exploreHasPrivateProjection_ = false;
    QTimer* boardTimer_ = nullptr;
    PublicEquityBoard* board_ = nullptr;
    QPushButton* hideSmall_ = nullptr;
    QPushButton* hideUnvalued_ = nullptr;
    QGridLayout* spotlightGrid_ = nullptr;
    QList<SpotlightCard*> spotlightCards_;
    QBoxLayout* spotlightColumns_ = nullptr; // Ranked markets beside (wide) or above (narrow) the watchlist.
    ExploreWatchlist* watchlist_ = nullptr;
    QTimer* watchlistVisibilityTimer_ = nullptr;
    PublicMarketHistory* marketHistory_ = nullptr;
    MarketMetricsView* marketMetricsView_ = nullptr;
    QPointer<TodayBtcPremium> todayBtcPremium_;
    QPointer<WorldMarkets> worldMarkets_;
    void syncWorldMarketsDemand();
    void refreshChartMarketContext();
    QPointer<MarketFlowHistory> marketFlowHistory_;
    QPointer<SystemNotificationSink> marketFlowAlertSink_;
    QByteArray marketMetricPublicState_;
    QBoxLayout* marketFlowColumns_ = nullptr;
    QWidget* marketFlowProgress_ = nullptr;
    QProgressBar* marketFlowProgressBar_ = nullptr;
    QString marketFlowLoadedAsset_;
    QPointer<QDialog> optionsDialog_;
    QString optionsPage_ = "optionsGeneral";
    qint64 smallThresholdTransitionMs_ = 0;
    bool smallThresholdFilterPending_ = false;
    QString smallThresholdKrw_; // Exact KRW-USDT price; empty when unknown or stale.
};

} // namespace keefi
