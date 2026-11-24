#pragma once
#include <QHash>
#include <functional>
#include "keefi/TradingTypes.h"
#include <QPointer>
#include <QWidget>
#include <QSet>
#include <QMap>
#include <optional>
#include "keefi/ExtendedChartStudies.h"
#include "keefi/ChartMarketContext.h"
#include "keefi/HoldingMarketResolver.h"
#include "keefi/ChartTimeframePins.h"
#include "keefi/SplitterShare.h"

class QAction;
class QLabel;
class QLineEdit;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QTimer;
class QComboBox;
class QCheckBox;
class QTreeWidget;
class QTreeWidgetItem;
class QToolButton;
class QMenu;
class QDialog;
class QPushButton;
class QHBoxLayout;
class QSettings;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QScrollArea;
class QBoxLayout;

namespace keefi {
class MarketCandles;
class ChartGridView;
class HoldingChartPanel;
class ChartMarketLedger;
class ChartDrawings;
class MarketCandlesView;
class PortfolioController;
class TradingTransport;
class WatchlistWorkspaceList;
struct WatchlistIdentity;
struct WatchlistEntry;
class ChartStudyPreferences;
class StockChartSource;
class CoinFlowSource;
class CoinTapeView;
class ChartStudyFeed;
class ChartPublicContextSource;
class PublicMarketCatalog;
class PublicEquityBoard;
class ChartInstrumentCatalog;
class ChartInstrumentPresentation;
class ChartInstrumentPicker;
struct PositionRecord;
struct MarketInstrument;
struct ChartStudySet;
struct CoinFlowStudySet;

struct ChartFuturesPosition {
    bool available = false, distanceAvailable = false;
    bool quantityAvailable = false; // Display only; does not promote alert eligibility.
    QString reason, provider, accountScope, asset, currency, positionSide;
    QString entryPrice, markPrice, liquidationPrice, sourceUrl;
    QString amount, unrealizedPnl, pnlCurrency; // Exact provider position values, never account aggregates.
    qint64 receivedAtMs = 0;
    double liquidationDistancePercent = 0;
    QString contractSymbol; // Exact verified official contract symbol, never assembled.
};
// Existing provider observations only. Quote currency remains independent of
// the public chart axis; missing instrument identity is never inferred.
ChartFuturesPosition projectChartFuturesPosition(const PositionRecord&, const MarketInstrument&,
    const QString& selectedAsset, const QString& provider, const QString& accountScope,
    qint64 receivedAtMs, qint64 asOfMs = 0);

// Display-only workspace. Private position observations remain transient and
// are cleared independently of the public chart when the vault is locked.
class ChartWorkspace final : public QWidget {
    Q_OBJECT
  public:
    // Right panel beside a single chart. Below CollapseWidth of window width or
    // when the viewport cannot keep the actual CandleChart canvas >= ChartMinimumWidth
    // (680) plus its pane margins and drawing rail (84) beside SidePanelMinimumWidth
    // (280), it moves into the details disclosure under the chart, as in grid mode.
    static constexpr int SidePanelCollapseWidth = 1080, SidePanelMinimumWidth = 280;
    static constexpr int SidePanelDefaultWidth = 320, SidePanelWideWidth = 360, SidePanelWideWindowWidth = 1440;
    static constexpr int ChartMinimumWidth = 680, ChartColumnChromeWidth = 84;
    static constexpr int ChartColumnMinimumWidth = ChartMinimumWidth + ChartColumnChromeWidth;
    explicit ChartWorkspace(MarketCandles*, PortfolioController*, QWidget* parent = nullptr);
    ~ChartWorkspace() override;
    void setAsset(const QString&);
    bool setIdentity(const QString&, bool publicDrawingContext = true);
    bool openStockFromNavigation(const QString& identity);
    QStringList visiblePublicIdentityKeysForDisplay() const;
    void setTimeframe(qint64 intervalMs);
    void setStockSource(StockChartSource*);
    void setCoinFlowSource(CoinFlowSource*);
    void setPublicContextSource(ChartPublicContextSource*);
    void setObservationStudies(const QSet<QString>&);
    void setStudyFeed(ChartStudyFeed*);
    void setPublicNameSources(PublicMarketCatalog*, PublicEquityBoard*, ChartInstrumentCatalog*);
    CoinFlowStudySet coinFlowStudies() const;
    void setIncludeExtendedSessions(bool);
    bool includeExtendedSessions() const;
    qint64 timeframeMs() const;
    QString selectedChartIdentity() const;
    void setContractTransport(TradingTransport*); // Dedicated public metadata transport, borrowed.
    void setFuturesObservationActive(bool);
    QList<ChartFuturesPosition> futuresPositionObservations(qint64 asOfMs=0) const;
    void setWatchlistModel(WatchlistWorkspaceList*);
    void setChartSettings(QSettings*);
    WatchlistWorkspaceList* watchlistModel() const;
    ChartStudyPreferences* indicatorPreferences() const;
    ChartDrawings* drawingModel() const;
    void setDrawingAlertEnabledLists(const QSet<QString>&);
    ChartStudySet chartStudies() const;
    ExtendedChartStudySet extendedStudies() const;
    MarketCandleSeries publicStudySeries() const;
    bool publicStudyRealtimeAllowed() const;
    QString currentListId() const;
    QString selectedIdentity() const;
    void setRealtimeDelayedIdentities(const QStringList&);
    void setHoldingsUnavailableReason(const QString&);
    QString holdingsUnavailableReason() const { return holdingsUnavailableReason_; }
    void setHoldingRows(const QList<HoldingChartRow>&);
    void setPrivateEnabled(bool);
    void clearPrivate();
    // Public header context for one chart identity (won value, premium, related markets).
    // Shown only while that identity is selected; never stored with the chart.
    void setMarketContext(const ChartMarketContext&);
    // The 체결·청산 tab is on screen. Public trade collection follows this.
    bool coinTapeVisible() const;
    bool sidePanelBesideChart() const { return sidePlaced_; }
    const ChartTimeframePins& timeframePins() const { return timeframePins_; }
  signals:
    void holdingMarketSelected(QString assetKey,QString marketKey);
    void assetCardRequested(QString assetKey);
    void vaultOpenRequested();
    void stockConnectionRequested(const QString& identity,qint64 intervalMs);
    void assetSelected(const QString&);
    void identitySelected(const QString&);
    void selectionChanged(const QString& fullIdentity, qint64 intervalMs);
    void sessionPolicyChanged(bool includeExtended);
    void visibleIdentitiesChanged(const QStringList&);
    void listAlertsRequested(const QString&);
    void studiesChanged();
    void coinFlowStudiesChanged();
    void publicFlowObservationReady(const keefi::CoinFlowSnapshot&,const keefi::CoinFlowStudySet&,const keefi::ExtendedStrongReversal&);
    void futuresPositionObservationsChanged();
    void coinTapeVisibilityChanged(bool visible);
  protected:
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
  private:
    void refreshPositions();
    void refreshPositionPresentation();
    void refreshHoldingsPreview();
    void refreshStudyPanels();
    void refreshMarketLedger();
    void renderMarketContext();
    void layoutToolbar();
    void renderHoldingContext();
    void refreshTrendTable();
    void refreshTrendDemands();
    void refreshSelectedStudies();
    void syncStockSourceState();
    void positionInstrumentPanel();
    void closeInstrumentPanel();
    void activateInstrumentResult();
    QTreeWidget* instrumentResultTree() const;
    void requestContracts();
    void cancelContractRead();
    QStringList observedFuturesSymbols(qint64 asOfMs = 0) const;
    void refreshFuturesObservations();
    void synchronizeChartSelection();
    void refreshRoutingSources();
    void refreshMetadataFailurePresentation();
    QString metadataFailureFor(const WatchlistIdentity&) const;
    void updateCatalogRetry(bool upbitFailed, bool upbitLoading, bool binanceFailed);
    void openInstrumentPicker(const QString& query = {});
    void addPublicInstruments(const QStringList&, bool representative);
    void refreshWatchlists();
    void setBrowserMarket(const QString&, bool restoreSelection = true);
    void setBrowserScope(const QString&);
    void filterBrowserRows();
    void showListMessage(const QString&, bool restorePanel = false);
    bool openBrowserRow(QTreeWidgetItem*);
    bool openTrendRow(QTreeWidgetItem*, int column);
    void toggleBrowserFavorite(QTreeWidgetItem*);
    void ensureBrowserCatalog();
    bool knownPublicStock(const QString&) const;
    bool browserPublicDrawingContext(const QString&) const;
    QString browserSortMode() const;
    void sortBrowserColumn(int column);
    void updateBrowserHeaders();
    struct BrowserMetric { double turnover=0, volume=0, cap=0; std::optional<double> change, price; QString unit, base, changeBasis, capBasis; };
    QHash<QString, BrowserMetric> browserMetrics() const;
    QHash<QString, double> browserTradeValues() const;
    void refreshDrawingAlertLists();
    void refreshWatchlistRows();
    void updateWatchlistRows(bool neededForSplit);
    void selectWatchlist(const QString&);
    void resizeBrowserColumns();
    void scheduleVisibleRows();
    void publishVisibleRows();
    void buildWatchlistMenu();
    bool applyBrowserEntryOrder(const QString&, const QList<WatchlistEntry>&, const QStringList&);
    void restoreBrowserSelection(const QStringList&, const QString& section = {});
    void announceBrowserMove(const QStringList&, const QString& section = {});
    void moveBrowserRows(const QString&, const QStringList&, const QString&, const QString&, bool);
    void moveBrowserSection(const QString&, const QString&, const QString&, bool);
    void moveBrowserSelection(const QString&, const QStringList&, const QString&, int);
    void removeBrowserSelection(const QString&, const QStringList&, const QString&);
    void openBrowserContextMenu(const QString&, const QStringList&, const QPoint&);
    void addWatchlistIdentity();
    void openShareCard();
    void buildSidePanel(QBoxLayout* panels);
    void layoutSidePanel();
    void updateCoinTapeVisibility();
    void updateDetailsToggleText();
    void restoreTimeframePins();
    void refreshTimeframeButtons();
    void setTimeframePinned(qint64 intervalMs, bool pinned);
    void openTimeframeList(QWidget* anchor);
    QPointer<PortfolioController> portfolio_;
    QPointer<ChartStudyPreferences> indicatorPreferences_;
    QPointer<ChartDrawings> drawings_;
    QSet<QString> drawingAlertEnabledLists_;
    QPointer<MarketCandles> candleModel_;
    QPointer<StockChartSource> stockSource_;
    QPointer<ChartPublicContextSource> publicContext_;
    QPointer<ChartStudyFeed> studyFeed_;
    QPointer<PublicMarketCatalog> publicMarkets_;
    QPointer<PublicEquityBoard> publicBoard_;
    QPointer<ChartInstrumentCatalog> publicInstruments_;
    QPointer<ChartInstrumentPicker> instrumentPicker_;
    QString pendingChartIdentity_;
    QString chartRouteMessage_;
    bool pendingPublicDrawingContext_ = true;
    bool routingEnabled_ = false, routingRefreshing_ = false;
    ChartInstrumentPresentation* instrumentPresentation_ = nullptr;
    QCheckBox* extendedSessions_ = nullptr;
    bool includeExtended_ = false, stockStateSyncing_ = false;
    MarketCandlesView* candles_ = nullptr;
    ChartGridView* grid_ = nullptr;
    HoldingChartPanel* holdingPanel_ = nullptr;
    QList<HoldingChartRow> holdingRows_;
    QLabel* holdingContext_ = nullptr;
    QAction* assetCardAction_ = nullptr;
    QString selectedHoldingAsset_;
    QComboBox* gridSelector_ = nullptr;
    bool gridActivating_ = false;
    QWidget *list_ = nullptr, *positions_ = nullptr, *chartPane_ = nullptr;
    QWidget *instrumentPanel_ = nullptr, *browserEmpty_ = nullptr;
    QWidget *browserMarkets_ = nullptr, *browserWatchMarkets_ = nullptr, *holdingInformation_ = nullptr, *browserViewModes_ = nullptr;
    QHBoxLayout *browserHeaderLayout_ = nullptr, *browserWatchMarketsLayout_ = nullptr;
    QLabel* browserCount_ = nullptr;
    QPushButton *listCreate_ = nullptr, *browserListMode_ = nullptr;
    QPushButton* instrumentButton_ = nullptr;
    QLabel* instrumentTitle_ = nullptr;
    bool activatingHoldingResult_ = false;
    QWidget *toolbar_=nullptr,*instrumentTools_=nullptr,*frequentTools_=nullptr,*secondaryTools_=nullptr;
    QWidget *timeframeButtons_=nullptr,*layoutButtons_=nullptr;
    QToolButton *moreTools_=nullptr,*syncTools_=nullptr,*styleTools_=nullptr;
    QAction *foldSeparator_=nullptr,*foldLayout_=nullptr,*foldSync_=nullptr,*foldStyle_=nullptr;
    bool toolbarLayoutActive_=false;

    QWidget *positionCard_ = nullptr, *scoreCard_ = nullptr;
    QWidget *statusBody_=nullptr,*scoreBody_=nullptr;
    QLabel *indicatorSummary_=nullptr,*indicatorCompleteness_=nullptr;
    QTreeWidget* indicatorRows_=nullptr;
    QPushButton* scoreToggle_=nullptr;
    bool scoreMode_=false;
    QLabel *positionLocked_ = nullptr, *holdingsEmpty_ = nullptr, *scoreEmpty_ = nullptr;
    QPushButton* positionUnlock_ = nullptr;
    QTreeWidget* holdingsRows_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton *detailsToggle_ = nullptr, *browserRetry_ = nullptr;
    QTimer* browserRetryTimer_ = nullptr;
    struct BrowserState { QString query, selected, sort = "auto"; int scroll = 0; };
    QMap<QString, BrowserState> browserStates_;
    QString browserMarket_ = "upbit", browserScope_ = "all", browserSort_ = "auto";
    // Display-order cache for the traded-value sort; recomputed only on new data or a mode change.
    QHash<QString, int> browserRank_; QString browserRankKey_; qint64 browserRankedAtMs_ = 0, coinMetricsRequestedAtMs_ = 0;
    bool browserRestoring_ = false;
    QLabel *positionText_ = nullptr, *liquidationRibbon_ = nullptr, *chartUnavailable_ = nullptr;
    QLabel *scorecardText_ = nullptr, *similaritySummary_ = nullptr;
    ChartMarketLedger* marketLedger_ = nullptr;
    QLabel *currentPrice_ = nullptr, *currentPriceUnit_ = nullptr, *currentPriceReference_ = nullptr;
    QLabel* priceConversion_ = nullptr;
    QLabel* relatedMarkets_ = nullptr;
    ChartMarketContext marketContext_;
    QWidget* evidenceNotes_ = nullptr;
    QString asset_ = "BTC";
    qint64 timeframeMs_ = 15 * 60000;
    QComboBox* timeframeSelector_ = nullptr;
    bool privateEnabled_ = false;
    QPointer<TradingTransport> contractTransport_;
    QTimer *contractPacing_ = nullptr, *contractRefresh_ = nullptr;
    MarketInstrument contract_;
    QString contractAsset_, contractReason_;
    qint64 contractReceivedMs_ = 0;
    quint64 contractToken_ = 0, nextContractToken_ = 1;
    bool contractPending_ = false;
    bool futuresObservationActive_ = false;
    QMap<QString, MarketInstrument> observationContracts_;
    qint64 observationContractsReceivedMs_ = 0;
    QPointer<WatchlistWorkspaceList> watchlists_;
    QPointer<QSettings> chartSettings_;
    QHash<QString,QHash<QString,bool>> sectionExpansion_;
    QComboBox* listSelector_ = nullptr;
    QComboBox* browserSortSelector_ = nullptr;
    QMap<int,QPushButton*> browserSortButtons_;
    QPushButton* browserManual_ = nullptr;
    QLabel* browserHint_ = nullptr;
    QPushButton* browserExpand_ = nullptr;
    QTreeWidget* watchlistRows_ = nullptr;
    QTreeWidget* trendRows_=nullptr;
    QPushButton* trendToggle_=nullptr;
    bool trendEnabled_=false,trendPending_=false;
    quint64 trendGeneration_=0;
    QMap<QString,ExtendedTimedDirection> trendResults_;
    QByteArray trendKey_;
    QByteArray selectedStudyKey_,selectedStudyAppliedKey_;
    quint64 selectedStudyGeneration_=0;
    bool selectedStudyPending_=false;
    QToolButton* listMenuButton_ = nullptr;
    QMenu* listMenu_ = nullptr;
    QPointer<QMenu> browserContextMenu_;
    QLabel* listStatus_ = nullptr;
    QLabel* listMarkets_ = nullptr;
    QSet<QString> delayedIdentities_;
    QStringList publishedVisibleRows_;
    bool renderingLists_ = false, mutatingLists_ = false, visibleRowsPending_ = false;
    std::function<void()> syncTimeframeButtons_;
    ChartTimeframePins timeframePins_;
    QToolButton* timeframeList_ = nullptr;
    QPointer<QMenu> timeframeMenu_;
    QAction *timeframeFavorites_ = nullptr, *sidePanelAction_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QSplitter* sideSplitter_ = nullptr;
    QWidget *chartColumn_ = nullptr, *sidePanel_ = nullptr;
    QTabBar* sideTabs_ = nullptr;
    QStackedWidget* sideStack_ = nullptr;
    CoinTapeView* tape_ = nullptr;
    SplitterShare sideShare_{0.25};
    bool sidePlaced_ = false, sideOpen_ = true, sideUserSized_ = false, sideLayoutActive_ = false, coinTapeShown_ = false;
    bool clearingPrivate_ = false;
    QString selectedChartIdentity_ = "coin:upbit:KRW-BTC";
    QString renderedListId_;
    QString holdingsUnavailableReason_;
    bool chartSupported_ = true;
    QPointer<QDialog> sharePreview_;
};
}
