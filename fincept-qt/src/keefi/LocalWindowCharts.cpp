#include "keefi/LocalWindow.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/ProfileSettings.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/MarketFlowPresentation.h"
#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/PortfolioController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/MarketMetrics.h"
#include "keefi/WorldMarkets.h"
#include "keefi/ChartMarketContext.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/MarketCompareView.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/BackgroundTray.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartAlertFeed.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/CoinFlowStudyRunner.h"
#include "keefi/RapidMoveMonitor.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/PublicCoinIdentity.h"
#include "keefi/AssetShareDialog.h"
#include "keefi/RankingFuturesFeed.h"
#include "keefi/RankingStudyRunner.h"
#include "keefi/ChartStudyEventMonitor.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/TradingTransport.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/TossMarketView.h"
#include "keefi/TossMarketStream.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/KiwoomMarketView.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/MarketMap.h"
#include "keefi/Spotlight.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/ExploreWatchlist.h"
#include <QCheckBox>
#include <QDialog>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include "keefi/MovementColorPolicy.h"
#include "keefi/ExploreMetricCard.h"
#include <QJsonDocument>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QUuid>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTimer>
#include <QStackedWidget>
#include <QTableWidget>
#include <QScrollArea>
#include <QTreeWidget>
#include <QDateTime>
#include "keefi/AssetValuation.h"
#include "keefi/AssetIdentity.h"
#include "keefi/AssetShareCard.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/HoldingMarketResolver.h"
#include "keefi/KiwoomMarketTypes.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCompareSource.h"
#include "keefi/TradingTypes.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/MovementDirection.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/ValuationBundle.h"
#include <QStyle>
#include <algorithm>
#include <cmath>
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

namespace keefi {
using local_window_detail::showTransientFeedback;

namespace {
QString compactCap(double value, const QString& currency) {
    if (currency == "USD") return value >= 1e12 ? QString("$%1T").arg(value / 1e12, 0, 'f', 2) : QString("$%1B").arg(value / 1e9, 0, 'f', 1);
    return value >= 1e12 ? QObject::tr("%1조원").arg(value / 1e12, 0, 'f', 1) : QObject::tr("%1억원").arg(std::round(value / 1e8), 0, 'f', 0);
}
} // namespace

void LocalWindow::setChartInstrumentTransport(TradingTransport* transport){
    if(!chartWorkspace_)chartInstrumentTransport_=transport;
}
void LocalWindow::setChartSettings(QSettings* settings) {
    if(!chartWorkspace_){chartSettings_=settings;KeefiTheme::instance()->setSettings(settings);restoreMovementColors();restoreMoneyCurrency();}
    if(marketCompareView_)marketCompareView_->setSettings(settings);
}
void LocalWindow::restoreMoneyCurrency(){
    const auto value=chartSettings_?chartSettings_->value("display/moneyCurrency/v1").toString():ProfileSettings().value("display/moneyCurrency/v1").toString();
    moneyCurrency_=value=="USD"?MoneyCurrency::Usd:MoneyCurrency::Krw;
    refreshMoneyPresentation(true);
}
void LocalWindow::restoreMovementColors(){
    if(!chartSettings_||!movementColors_)return;restoringMovementColors_=true;QString reason;
    const auto stored=chartSettings_->value("display/movementColors/v1").toByteArray();
    if(!stored.isEmpty()&&!movementColors_->restorePublicState(stored,&reason))setProperty("movementStorageIssue",reason);
    else setProperty("movementStorageIssue",QVariant{});
    movementPublicState_=movementColors_->publicState();restoringMovementColors_=false;refreshMovementPresentation();
}
void LocalWindow::refreshMovementPresentation(){
    applyLocalTheme();
    if(exploreTable_)for(int row=0;row<exploreTable_->rowCount();++row)if(auto* cell=exploreTable_->item(row,3)){const int direction=cell->data(ChangeDirectionRole).toInt();cell->setForeground(movementColors_->foreground(direction>0?MovementDirection::Up:direction<0?MovementDirection::Down:MovementDirection::Flat));}
    for(auto* widget:findChildren<QWidget*>()){if(widget->property("tone").isValid()){widget->style()->unpolish(widget);widget->style()->polish(widget);}widget->update();}
}
void LocalWindow::setChartAlerts(ChartAlertEngine* engine,ChartAlertFeed* feed,SystemNotificationSink* sink,ChartPhrasePack* pack,QSettings* settings){
    if(chartAlerts_||!engine||!chartWorkspace_)return;
    chartAlerts_=engine;chartAlertFeed_=feed;chartAlertSettings_=settings;marketFlowAlertSink_=sink;
    if(feed)feed->setVerifiedUpbitMarkets(markets_?markets_->verifiedMarkets():QStringList{},markets_&&markets_->marketListAvailable());
    auto studyEvents=std::make_shared<ChartStudyEventMonitor>();
    connect(chartWorkspace_,&ChartWorkspace::studiesChanged,engine,[this,studyEvents]{
        if(!chartWorkspace_||!chartAlerts_)return;
        for(const auto& event:studyEvents->observe(chartWorkspace_->publicStudySeries(),chartWorkspace_->extendedStudies(),
                chartWorkspace_->publicStudyRealtimeAllowed()))chartAlerts_->observePublicStudy(event);
    });
    engine->setPhrasePack(pack);
    // Public list membership must exist before strict saved-rule validation.
    syncChartAlerts();
    QString issue;
    if(settings){const auto saved=settings->value("chart/alerts/v1").toByteArray();
        if(!saved.isEmpty()&&!engine->restorePublicState(saved,&issue))chartWorkspace_->setProperty("alertStorageIssue",issue);}
    if(settings){const auto saved=settings->value("market/flow-alerts/v1").toByteArray();if(!saved.isEmpty()&&!engine->restoreMarketMetricState(saved))setProperty("marketFlowAlertStorageIssue",true);}
    if(settings){const auto saved=settings->value("chart/observations/v1").toByteArray();if(!saved.isEmpty()&&!engine->restoreObservationSettingsState(saved))setProperty("chartObservationStorageIssue",true);}
    chartWorkspace_->setObservationStudies(engine->observationEnabled("strong_reversal")?QSet<QString>{"strong"}:QSet<QString>{});
    marketMetricPublicState_=engine->marketMetricState();syncMarketFlowAlerts();
    // The controller owns session history/phrase-pack state, independently of
    // the chart tab and of the disposable options dialog.
    auto* panel=new ChartAlertsPanel(engine,feed,sink,pack,this);chartAlertsPanel_=panel;
    panel->hide();
    panel->setBackgroundAvailable(tray_&&tray_->available());
    if(settings){const auto saved=settings->value("chart/alerts/ui/v1").toByteArray();
        if(!saved.isEmpty()&&!panel->restorePublicState(saved,&issue))chartWorkspace_->setProperty("alertUiStorageIssue",issue);}
    background_->setChecked(panel->backgroundEnabled());
    chartAlertPublicState_=engine->publicState();chartAlertUiState_=panel->publicState();
    connect(engine,&ChartAlertEngine::changed,this,[this]{
        if(chartWorkspace_)chartWorkspace_->setObservationStudies(chartAlerts_->observationEnabled("strong_reversal")?QSet<QString>{"strong"}:QSet<QString>{});
        syncCoinFlowDemand();
        syncMarketFlowAlerts();
        if(chartAlerts_&&chartAlertSettings_){const auto state=chartAlerts_->publicState();
            if(state!=chartAlertPublicState_){chartAlertSettings_->setValue("chart/alerts/v1",state);chartAlertPublicState_=state;}}
        if(chartAlerts_&&chartAlertSettings_)chartAlertSettings_->setValue("chart/observations/v1",chartAlerts_->observationSettingsState());
        if(markets_)markets_->setPresentationActive((isVisible()&&!isMinimized())||chartAlerts_->observationEnabled("rapid_move"));
        if(chartWorkspace_&&chartLists_){QSet<QString> enabled;
            for(const auto& list:chartLists_->userLists()){const auto rule=chartAlerts_->rule(list.id);if(rule.enabled&&rule.horizontal)enabled.insert(list.id);}
            chartWorkspace_->setDrawingAlertEnabledLists(enabled);}
        syncChartRisk();
    });
    connect(panel,&ChartAlertsPanel::publicStateChanged,this,[this]{
        if(chartAlertsPanel_&&chartAlertSettings_){const auto state=chartAlertsPanel_->publicState();
            if(state!=chartAlertUiState_){chartAlertSettings_->setValue("chart/alerts/ui/v1",state);chartAlertUiState_=state;}}
    });
    connect(panel,&ChartAlertsPanel::backgroundEnabledChanged,background_,&QCheckBox::setChecked);
    connect(panel,&ChartAlertsPanel::settingsRequested,this,[this](const QString& list){openOptionsPage("optionsAlerts",list);});
    connect(panel,&ChartAlertsPanel::generalSettingsRequested,this,[this]{openOptionsPage("optionsGeneral");});
    connect(chartWorkspace_,&ChartWorkspace::listAlertsRequested,this,[this](const QString& list){openOptionsPage("optionsAlerts",list);});
    connect(chartWorkspace_,&ChartWorkspace::futuresPositionObservationsChanged,this,&LocalWindow::syncChartRisk);
    connect(engine,&ChartAlertEngine::openWindowRequested,this,[this]{showNormal();raise();activateWindow();});
    if(feed){
        connect(engine,&ChartAlertEngine::demandsChanged,feed,[engine=QPointer(engine),feed=QPointer(feed)]{if(engine&&feed)feed->setDemands(engine->demands());});
        connect(feed,&ChartAlertFeed::laneStarted,engine,[engine](const QString& key,qint64 interval,quint64 generation,bool realtime){engine->beginLane(WatchlistWorkspaceList::identityFromKey(key),interval,generation,realtime);});
        connect(feed,&ChartAlertFeed::laneInvalidated,engine,[engine](const QString& key,qint64 interval,quint64 generation){engine->invalidateLane(WatchlistWorkspaceList::identityFromKey(key),interval,generation);});
        connect(feed,&ChartAlertFeed::observationReady,engine,&ChartAlertEngine::observe);
        connect(feed,&ChartAlertFeed::stockObservationReady,engine,&ChartAlertEngine::observeStock);
        auto* flowCalculator=new CoinFlowStudyRunner(engine);
        connect(feed,&ChartAlertFeed::coinFlowObservationReady,flowCalculator,[flowCalculator,engine](const CoinFlowSnapshot& snapshot,quint64 generation){
            QSet<QString> enabled;if(engine->observationEnabled("strong_reversal"))enabled.insert("strong");
            flowCalculator->request(snapshot,QDateTime::currentMSecsSinceEpoch(),QString::number(generation),enabled);
        });
        connect(flowCalculator,&CoinFlowStudyRunner::observationReady,engine,[engine](const CoinFlowSnapshot& snapshot,const QString& context,const CoinFlowStudySet& result,const ExtendedStrongReversal& grade){
            bool valid=false;const auto generation=context.toULongLong(&valid);if(valid)engine->observeStrongReversalCalculated(snapshot,result,grade,generation);
        });
        connect(flowCalculator,&CoinFlowStudyRunner::calculatedResultReady,engine,[engine](const CoinFlowSnapshot& snapshot,const QString& context,const CoinFlowStudySet& result){
            bool valid=false;const auto generation=context.toULongLong(&valid);if(valid)engine->observeCoinFlowCalculated(snapshot,result,generation);
        });
        connect(chartWorkspace_,&ChartWorkspace::publicFlowObservationReady,engine,[engine](const CoinFlowSnapshot& snapshot,const CoinFlowStudySet& result,const ExtendedStrongReversal& grade){
            engine->observeStrongReversalCalculated(snapshot,result,grade);
        });
        feed->setCoinFlowSource(chartCoinFlow_);
        connect(feed,&ChartAlertFeed::changed,this,&LocalWindow::syncCoinFlowDemand);
        connect(feed,&ChartAlertFeed::changed,this,&LocalWindow::syncPublicMarketMetadataDemand);
        connect(engine,&ChartAlertEngine::demandsChanged,this,&LocalWindow::syncCoinFlowDemand);
        connect(engine,&ChartAlertEngine::demandsChanged,this,&LocalWindow::syncPublicMarketMetadataDemand);
        if(alertStockSource_)feed->setStockSource(alertStockSource_);
        feed->setDemands(engine->demands());feed->setRunning(true);
        syncPublicMarketMetadataDemand();
    }
    if(sink)sink->refreshPermission();
    syncChartAlerts();
}
void LocalWindow::setMarketCompareSource(MarketCompareSource* source) {
    auto* pages=findChild<QTabWidget*>("portfolioPages");
    if(!source||!pages||marketCompareView_)return;
    marketCompareView_=new MarketCompareView(source,pages);
    marketCompareView_->setSettings(chartSettings_);
    int at=0;
    for(int i=0;i<pages->count();++i)if(pages->tabText(i)==tr("차트")||(at==0&&pages->tabText(i)==tr("탐색")))at=i+1;
    pages->insertTab(at,marketCompareView_,tr("시장 비교"));syncHeaderTabs();
}
void LocalWindow::setMarketCandles(MarketCandles* candles) {
    auto* pages=findChild<QTabWidget*>("portfolioPages");
    if(!candles||!pages||chartWorkspace_)return;
    candles->setVerifiedUpbitMarkets(markets_?markets_->verifiedMarkets():QStringList{},markets_&&markets_->marketListAvailable());
    chartWorkspace_=new ChartWorkspace(candles,portfolio_,pages);
    connect(chartWorkspace_, &ChartWorkspace::stockConnectionRequested, this, &LocalWindow::openChartBrokerConnection);
    connect(chartWorkspace_,&ChartWorkspace::vaultOpenRequested,this,[this]{if(vault_.exists())unlockVault_->click();else createVault_->click();});
    chartCandles_=candles;chartLists_=chartWorkspace_->watchlistModel();
    connect(chartWorkspace_,&ChartWorkspace::holdingMarketSelected,this,[this](const QString& key,const QString& market){
        if(chartPrivateBlocked_||chartSourceResetting_||!securityReady_||!accountReadsAvailable(vault_))return;
        const auto identity=WatchlistWorkspaceList::identityFromKey(market);
        for(const auto& row:holdingRows_)if(row.assetKey==key&&row.candidates.contains(identity)){
            holdingMarketSession_.insert(key,identity);refreshChartWatchlists();break;
        }
    });
    connect(chartWorkspace_,&ChartWorkspace::assetCardRequested,this,[this,pages](const QString& key){
        if(chartPrivateBlocked_||chartSourceResetting_||!securityReady_||!accountReadsAvailable(vault_))return;
        // Keep an already selected child scope; a different asset selects only
        // that currently visible group, never widens a filtered account scope.
        const auto selected=selectedPortfolioGroup();
        if(!selected||selected->identity!=key){
            QModelIndex match;
            auto* model=portfolioAssets_->model();
            for(int i=0;i<model->rowCount();++i) {
                auto index=model->index(i,0);
                if(index.data(Qt::UserRole+1)==key){match=index;break;}
            }
            if(!match.isValid()){showTransientFeedback(feedback_,tr("현재 내 자산 목록 범위에서 이 자산을 확인할 수 없어요."));return;}
            portfolioAssets_->setCurrentIndex(match);
        }
        if(auto* home=findChild<QWidget*>("portfolioHome"))pages->setCurrentWidget(home);
        portfolioListDetail_->showDetail();showPortfolioDetail();openAssetShare();
    });
    chartWorkspace_->setStockSource(chartStockSource_);
    chartWorkspace_->setCoinFlowSource(chartCoinFlow_);
    chartWorkspace_->setPublicContextSource(chartPublicContext_);
    chartWorkspace_->setStudyFeed(chartStudyFeed_);
    // The selected chart publishes study/flow badge changes as it drops its
    // candles on hide. Portfolio, catalogue and alert inputs have separate
    // subscriptions. Rebuild the visible badge rows when the chart is shown.
    const auto refreshVisibleStudyRows = [this] {
        if (!windowShownOnce_ || (chartWorkspace_->isVisible() && !isMinimized()))
            refreshChartWatchlists();
    };
    connect(chartWorkspace_, &ChartWorkspace::coinFlowStudiesChanged, this, refreshVisibleStudyRows);
    if(!chartInstrumentTransport_)chartInstrumentTransport_=new NetworkTradingTransport(this);
    chartInstruments_=new ChartInstrumentCatalog(chartInstrumentTransport_,this);
    chartWorkspace_->setPublicNameSources(markets_,board_,chartInstruments_);
    connect(chartInstruments_,&ChartInstrumentCatalog::changed,this,&LocalWindow::refreshChartWatchlists);
    connect(chartInstruments_,&ChartInstrumentCatalog::changed,this,&LocalWindow::syncMarketFlowHistory);
    connect(chartInstruments_,&ChartInstrumentCatalog::changed,this,&LocalWindow::refreshSpotlight);
    connect(chartInstruments_,&ChartInstrumentCatalog::changed,this,&LocalWindow::refreshAssetShare);
    if(!chartSettings_){chartSettings_=new ProfileSettings(this);restoreMovementColors();}
    chartWorkspace_->setChartSettings(chartSettings_);
    const auto sessions=chartSettings_->value("chart/stock/sessions/v1").toByteArray();
    if(!sessions.isEmpty()){
        QJsonParseError issue;const auto doc=QJsonDocument::fromJson(sessions,&issue);const auto object=doc.object();
        if(issue.error==QJsonParseError::NoError&&doc.isObject()&&object.size()==2&&canonicalPublicFormat(object.value("format").toString())=="keefi-stock-sessions/1"&&object.value("includeExtended").isBool())
            chartWorkspace_->setIncludeExtendedSessions(object.value("includeExtended").toBool());
        else chartWorkspace_->setProperty("stockSessionStorageIssue",tr("주식 세션 설정 형식이 올바르지 않습니다."));
    }
    connect(chartWorkspace_,&ChartWorkspace::sessionPolicyChanged,this,[this](bool include){
        if(chartSettings_)chartSettings_->setValue("chart/stock/sessions/v1",QJsonDocument(QJsonObject{{"format","keefi-stock-sessions/1"},{"includeExtended",include}}).toJson(QJsonDocument::Compact));
    });
    if(auto* preferences=chartWorkspace_->indicatorPreferences()){
        const auto storedStudies=chartSettings_->value("chart/studies/v1").toByteArray();
        QString issue;
        if(!storedStudies.isEmpty()&&!preferences->restorePublicState(storedStudies,&issue))
            chartWorkspace_->setProperty("studyStorageIssue",issue);
        chartStudyPublicState_=preferences->publicState();
        connect(preferences,&ChartStudyPreferences::changed,this,[this,preferences]{
            if(!chartWorkspace_||!chartSettings_)return;
            const auto state=preferences->publicState();
            if(state!=chartStudyPublicState_){chartSettings_->setValue("chart/studies/v1",state);chartStudyPublicState_=state;}
            refreshChartWatchlists();
        });
    }
    if(auto* drawings=chartWorkspace_->drawingModel()){
        const bool hasV2=chartSettings_->contains("chart/drawings/v2");
        const auto saved=chartSettings_->value(hasV2?"chart/drawings/v2":"chart/drawings/v1").toByteArray();QString issue;
        if(!saved.isEmpty()&&!drawings->restorePublicState(saved,&issue)){
            chartSettings_->setValue("chart/drawings/rejected",saved);
            chartWorkspace_->setProperty("drawingStorageIssue",issue);
        }
        chartDrawingPublicState_=drawings->publicState();
        connect(drawings,&ChartDrawings::publicStateChanged,this,[this,drawings]{
            if(!chartWorkspace_||!chartSettings_)return;const auto state=drawings->publicState();
            if(state!=chartDrawingPublicState_){chartSettings_->setValue("chart/drawings/v2",state);chartDrawingPublicState_=state;}
        });
        connect(drawings,&ChartDrawings::horizontalLinesChanged,this,&LocalWindow::syncChartAlerts);
    }
    const auto stored=chartSettings_->value("chart/watchlists/v2").toByteArray();
    QString restoreIssue;
    if(!stored.isEmpty()&&!chartLists_->restorePublicState(stored,&restoreIssue))
        chartWorkspace_->setProperty("listStorageIssue",restoreIssue);
    chartPublicState_=chartLists_->publicState();
    connect(chartLists_,&WatchlistWorkspaceList::changed,this,[this]{
        if(!chartLists_||!chartSettings_)return;
        const auto state=chartLists_->publicState();
        if(state!=chartPublicState_){chartSettings_->setValue("chart/watchlists/v2",state);chartPublicState_=state;}
        if (!chartListsRefreshing_) syncChartSubscriptions();
    });
    chartWorkspace_->setAsset(marketMetricsView_?marketMetricsView_->asset():QStringLiteral("BTC"));
    chartWorkspace_->setPrivateEnabled(securityReady_&&accountReadsAvailable(vault_));
    int at=0;for(int i=0;i<pages->count();++i)if(pages->tabText(i)==tr("탐색"))at=i+1;
    pages->insertTab(at,chartWorkspace_,tr("차트"));syncHeaderTabs();
    connect(chartWorkspace_,&ChartWorkspace::assetSelected,this,[this](const QString& asset){selectMarketMetric("coin",asset);});
    connect(chartWorkspace_,&ChartWorkspace::visibleIdentitiesChanged,this,[this](const QStringList& keys){chartVisibleIdentities_=keys;syncChartSubscriptions();});
    connect(chartWorkspace_,&ChartWorkspace::identitySelected,this,[this](const QString&){syncChartSubscriptions();});
    connect(chartWorkspace_,&ChartWorkspace::selectionChanged,this,[this](const QString&,qint64){refreshChartWatchlists();});
    connect(chartWorkspace_, &ChartWorkspace::studiesChanged, this, refreshVisibleStudyRows);
    connect(pages,&QTabWidget::currentChanged,this,&LocalWindow::syncChartTab,Qt::UniqueConnection);
    connect(candles, &MarketCandles::changed, this, [this, candles] {
        // Clearing a hidden selection cannot introduce a public market or
        // change the account positions. Release display subscriptions now.
        if (windowShownOnce_ && (!chartWorkspace_->isVisible() || isMinimized()) && candles->series().identity.isEmpty()) {
            releaseChartRowQuotes();
            if (!chartListsRefreshing_) syncChartSubscriptions();
            return;
        }
        refreshChartWatchlists();
    });
    auto* timer=new QTimer(chartWorkspace_);timer->setInterval(1000);
    connect(timer,&QTimer::timeout,this,&LocalWindow::refreshChartWatchlists);timer->start();
    refreshChartWatchlists();
}
void LocalWindow::setStockChartSource(StockChartSource* source){setStockChartSources(source,nullptr);}
void LocalWindow::setChartPublicContextSource(ChartPublicContextSource* source){
    if(chartPublicContext_==source)return;
    if(chartPublicContext_)chartPublicContext_->setDemand(false,false);
    chartPublicContext_=source;if(chartWorkspace_)chartWorkspace_->setPublicContextSource(source);
}
void LocalWindow::setChartStudyFeed(ChartStudyFeed* source){
    if(chartStudyFeed_==source)return;clearAssetShare();if(chartStudyFeed_){disconnect(chartStudyFeed_,nullptr,this,nullptr);chartStudyFeed_->stop();}
    chartStudyFeed_=source;if(source){source->setVerifiedUpbitMarkets(markets_?markets_->verifiedMarkets():QStringList{},markets_&&markets_->marketListAvailable());source->setStockSource(studyStockSource_);}
    if(chartWorkspace_)chartWorkspace_->setStudyFeed(source);
    if(source){
        connect(source,&ChartStudyFeed::changed,this,[this](const QString& key,qint64 interval){
            refreshPublicObservations();
            if(assetShareDialog_&&assetShareValue_.market.identity.key()==key&&AssetShareCard::intervalMs(assetShareValue_.period)==interval)refreshAssetShare();
        });
        connect(source,&QObject::destroyed,this,[this]{clearAssetShare();});
    }
    refreshPublicObservations();
}
void LocalWindow::setRankingFuturesFeed(RankingFuturesFeed* source){
    if(rankingFuturesFeed_==source)return;if(rankingFuturesFeed_){disconnect(rankingFuturesFeed_,nullptr,this,nullptr);rankingFuturesFeed_->clear();}
    rankingFuturesFeed_=source;if(source)connect(source,&RankingFuturesFeed::changed,this,&LocalWindow::refreshRankingStudies);refreshRankingStudies();
}
void LocalWindow::setRapidMoveMonitor(RapidMoveMonitor* source){
    if(rapidMoves_==source)return;if(rapidMoves_)disconnect(rapidMoves_,nullptr,this,nullptr);rapidMoves_=source;
    if(source)connect(source,&RapidMoveMonitor::changed,this,&LocalWindow::refreshSpotlight);
    refreshSpotlight();
}
void LocalWindow::setCoinFlowSource(CoinFlowSource* source){
    if(chartCoinFlow_==source)return;
    if(!chartCoinFlowRefresh_){
        chartCoinFlowRefresh_=new QTimer(this);chartCoinFlowRefresh_->setSingleShot(true);chartCoinFlowRefresh_->setInterval(250);
        connect(chartCoinFlowRefresh_,&QTimer::timeout,this,[this]{
            if(chartCoinFlowStopping_||!chartCoinFlow_)return;
            syncCoinFlowDemand();refreshChartWatchlists();
        });
    }
    chartCoinFlowRefresh_->stop();
    const auto previous=chartCoinFlow_;chartCoinFlow_=nullptr;
    if(previous){disconnect(previous,nullptr,this,nullptr);previous->stop();}
    chartCoinFlow_=source;
    if(source){connect(source,&CoinFlowSource::changed,this,[this]{
        if(chartCoinFlowStopping_||!chartCoinFlow_)return;
        if(!chartCoinFlow_->active()){
            chartCoinFlowRefresh_->stop();syncCoinFlowDemand();refreshChartWatchlists();return;
        }
        // Collect every public trade and keep alert processing synchronous.
        // Only presentation/list synchronization takes the latest state at a
        // fixed deadline, so a busy stream cannot postpone the UI indefinitely.
        if(!chartCoinFlowRefresh_->isActive())chartCoinFlowRefresh_->start();
    });connect(source,&QObject::destroyed,this,[this]{chartCoinFlowRefresh_->stop();if(chartFlowMinutes_)chartFlowMinutes_->setSelection({},60000);});}
    if(chartWorkspace_)chartWorkspace_->setCoinFlowSource(source);
    if(chartAlertFeed_)chartAlertFeed_->setCoinFlowSource(source);
    syncCoinFlowDemand();
}
void LocalWindow::setCoinFlowMinuteModel(MarketCandles* model){
    if(chartFlowMinutes_==model)return;
    if(chartFlowMinutes_){disconnect(chartFlowMinutes_,nullptr,this,nullptr);chartFlowMinutes_->setSelection({},60000);}
    chartFlowMinutes_=model;
    if(model)model->setVerifiedUpbitMarkets(markets_?markets_->verifiedMarkets():QStringList{},markets_&&markets_->marketListAvailable());
    if(model)connect(model,&MarketCandles::changed,this,[this]{
        if(!chartCoinFlowStopping_&&chartCoinFlow_&&chartCoinFlow_->active())chartCoinFlow_->setMinuteSeries(chartFlowMinutes_->series());
    });
    syncCoinFlowDemand();
}
void LocalWindow::syncPublicMarketMetadataDemand(){
    if(!markets_)return;
    const bool readable=portfolio_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);
    bool needed=false;
    if(chartAlertFeed_&&chartAlertFeed_->running()&&chartAlerts_)for(const auto& demand:chartAlerts_->demands())
        if(demand.identity.market=="coin"&&demand.identity.source=="upbit"&&
           WatchlistWorkspaceList::validIdentity(demand.identity)&&(!demand.privateOnly||readable)){needed=true;break;}
    markets_->setMetadataDemand(needed);
}
void LocalWindow::syncCoinFlowDemand(){
    if(!chartCoinFlow_||chartCoinFlowSyncing_||chartCoinFlowStopping_)return;
    const QScopedValueRollback syncing(chartCoinFlowSyncing_,true);
    QString wanted;bool selected=false;
    if(chartWorkspace_&&chartWorkspace_->isVisible()&&isVisible()&&!isMinimized()&&
       chartWorkspace_->indicatorPreferences()&&(chartWorkspace_->indicatorPreferences()->enabled("whales")||chartWorkspace_->indicatorPreferences()->enabled("cvd")||chartWorkspace_->indicatorPreferences()->enabled("strong")||chartWorkspace_->indicatorPreferences()->enabled("low_volume")||(chartAlerts_&&chartAlerts_->observationEnabled("strong_reversal")))){
        const auto identity=WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity());
        if(identity.market=="coin"&&(identity.source=="upbit"||identity.source=="binance")){wanted=identity.key();selected=true;}
    }
    if(wanted.isEmpty()&&chartAlerts_&&chartAlertFeed_&&chartAlertFeed_->running()){
        QStringList demands;const bool readable=portfolio_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);
        for(const auto& demand:chartAlerts_->demands())if(demand.whales&&demand.identity.market=="coin"&&
            (demand.identity.source=="upbit"||demand.identity.source=="binance")&&(!demand.privateOnly||readable))demands.append(demand.identity.key());
        demands.removeDuplicates();
        const auto current=chartCoinFlow_->snapshot().identity;
        if(demands.contains(current))wanted=current;else if(!demands.isEmpty())wanted=demands.first();
    }
    chartCoinFlow_->setSelection(wanted);chartCoinFlow_->setActive(!wanted.isEmpty());
    if(chartFlowMinutes_){
        if(selected&&chartWorkspace_->timeframeMs()!=60000){
            const auto instrument=chartCoinFlow_->snapshot().primary.instrument;QList<MarketInstrument> instruments;
            if(instrument.verified&&instrument.identity==wanted&&instrument.venue==CoinFlowVenue::BinanceSpot){
                MarketInstrument item;item.symbol=instrument.symbol;item.baseAsset=instrument.baseAsset;item.quoteAsset=instrument.quoteAsset;
                item.status=instrument.status;item.market="spot";item.tradable=true;instruments.append(item);
            }
            chartFlowMinutes_->setInstruments("binance",instruments);chartFlowMinutes_->setSelection(wanted,60000);
            chartCoinFlow_->setMinuteSeries(chartFlowMinutes_->series());
        }else chartFlowMinutes_->setSelection({},60000);
    }
    if(selected&&chartWorkspace_->timeframeMs()==60000&&chartCandles_)chartCoinFlow_->setMinuteSeries(chartCandles_->series());
    if(chartWorkspace_)chartWorkspace_->setProperty("coinFlowCollectionPolicy",tr("공개 체결은 선택 차트 우선 · 다른 알림 종목은 완료 1분의 거래량·반전만 확인 · 체결 표본 수집 대기"));
}
void LocalWindow::setStockChartSources(StockChartSource* chart,StockChartSource* alerts,StockChartSource* studies){
    if(chartStockSource_==chart&&alertStockSource_==alerts&&studyStockSource_==studies)return;
    for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source){
        disconnect(source,nullptr,this,nullptr);source->setBroker(nullptr);source->setAuthorizationProvider({});source->setRealtimeStream(nullptr);source->setKiwoomDailyStream(nullptr);
    }
    chartStockSource_=chart;alertStockSource_=alerts;studyStockSource_=studies;stockChartBroker_=nullptr;stockChartBorrowBlocked_=true;
    for(auto* source:{chart,alerts,studies})if(source){
        source->setBroker(nullptr);source->setAuthorizationProvider({});source->setRealtimeStream(chartTossStream_);
        source->setKiwoomDailyStream(source==chart?chartKiwoomStream_.data():nullptr); // Kiwoom daily is a chart view only, never alerts.
        connect(source,&StockChartSource::changed,this,[this]{refreshChartWatchlists();refreshStockConnectionPresentation();});
        connect(source,&QObject::destroyed,this,[this]{stockChartBroker_=nullptr;syncStockChartSources();});
    }
    if(chartWorkspace_)chartWorkspace_->setStockSource(chart);
    if(chartAlertFeed_)chartAlertFeed_->setStockSource(alerts);
    if(chartStudyFeed_)chartStudyFeed_->setStockSource(studies);
    syncStockChartSources();refreshChartWatchlists();
}
void LocalWindow::revokeStockChartAuthorization(){
    stockChartBorrowBlocked_=true;stockChartBroker_=nullptr;
    if(chartStudyFeed_)chartStudyFeed_->clearPrivate();
    if(rankingStudyRunner_)rankingStudyRunner_->clear();for(auto* card:spotlightCards_)if(card->key()!="coin")card->setStudyValues({});
    for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source){source->setConnectionState(StockChartConnectionState::VaultClosed);source->setBroker(nullptr);source->setAuthorizationProvider({});}
    refreshRankingStudies();
}
StockChartConnectionState LocalWindow::stockChartConnectionState(BrokerTradingController** selectedOwner) const{
    if(selectedOwner)*selectedOwner=nullptr;
    const bool readable=portfolio_&&!chartPrivateBlocked_&&securityReady_&&accountReadsAvailable(vault_);
    // Never inspect connection existence while the vault is unavailable.
    if(!readable)return StockChartConnectionState::VaultClosed;
    bool hasBroker=false,hasToss=false,kiwoomDenied=false,kiwoomReady=false;BrokerTradingController* owner=nullptr;
    const auto* kiwoomView=findChild<KiwoomMarketView*>();
    const auto kiwoomChoice=kiwoomView?kiwoomView->selectedConnectionId():QString{};
    const auto* flow=findChild<TossMarketView*>();const auto choice=flow?flow->selectedConnectionId():QString{};
    for(const auto& state:portfolio_->connections()){
        if (AccountModel::isProviderSuspended(state.account.provider)) continue;
        hasBroker|=state.account.market=="domestic_equity";
        if(state.account.provider=="kiwoom"&&state.account.environment=="live"&&!state.shared&&
           (kiwoomChoice.isEmpty()||kiwoomChoice==state.account.id)){
            kiwoomDenied|=state.state=="authentication_required";
            kiwoomReady|=state.state=="complete";
        }
        if(state.account.provider!="toss")continue;hasToss=true;
        if(chartSourceResetting_||state.shared||state.account.environment!="live"||state.state!="complete")continue;
        if(!choice.isEmpty()&&state.account.id!=choice)continue;
        auto* candidate=portfolio_->broker(state.account.id); // Existing object; no construction/authentication/job.
        if(!candidate||!candidate->authenticated())continue;
        if(!owner)owner=candidate;if(state.account.id==choice){owner=candidate;break;}
    }
    if(selectedOwner)*selectedOwner=owner;
    if(owner){
        // A rejected authorization revision belongs to the controller that lent it.
        if(owner!=stockChartBroker_)return StockChartConnectionState::Ready;
        quint64 newestRevision=0;
        for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source)newestRevision=qMax(newestRevision,source->observedAuthorizationRevision());
        for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source&&source->authenticationRequired(newestRevision))return StockChartConnectionState::AuthenticationRequired;
        return StockChartConnectionState::Ready;
    }
    if(hasToss)return StockChartConnectionState::AuthenticationRequired;
    if(kiwoomDenied&&!kiwoomReady)return StockChartConnectionState::KiwoomAuthenticationRequired;
    return hasBroker?StockChartConnectionState::OtherBroker:StockChartConnectionState::NoBroker;
}
void LocalWindow::refreshStockConnectionPresentation(){
    const auto message=stockChartConnectionMessage(stockChartConnectionState());
    if(message==stockChartConnectionMessage_)return;stockChartConnectionMessage_=message;
    // Reuse public cells only, including during aboutToLock. Rebuilding a whole
    // detail here could restore private linkage before the vault finishes locking.
    refreshWatchlist(false);
    const auto update=[&](AssetDetailPanel* panel,const QString& section,const QString& id){
        if(!panel||id.isEmpty()||(section!="kr"&&section!="us"))return;
        refreshPublicDetailPresentation(panel,section,id,(section=="kr"?"public:naver:":"public:nasdaq:")+id,
            section=="kr"?QString("KRW"):QString("USD"),(section=="kr"?"stock:KR:":"stock:US:")+id);
    };
    if(marketMap_)update(mapDetail_,marketMap_->selectedSection(),marketMap_->selectedId());
}
void LocalWindow::syncStockChartSources(){
    if(stockChartSyncing_||(!chartStockSource_&&!alertStockSource_&&!studyStockSource_))return;
    const QScopedValueRollback syncing(stockChartSyncing_,true);
    BrokerTradingController* owner=nullptr;const auto state=stockChartConnectionState(&owner);
    for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source)source->setConnectionState(state);
    if(state==StockChartConnectionState::VaultClosed){if(!stockChartBorrowBlocked_||stockChartBroker_)revokeStockChartAuthorization();return;}
    if(stockChartBorrowBlocked_||owner!=stockChartBroker_){
        stockChartBorrowBlocked_=false;stockChartBroker_=owner;
        for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source){source->setBroker(owner);if(!owner)source->setAuthorizationProvider({});}
    }
}
void LocalWindow::revokeKiwoomMarketAuthorization(){
    if(!++kiwoomEpoch_)++kiwoomEpoch_;
    if(kiwoomRevoking_)return;
    const QScopedValueRollback revoking(kiwoomRevoking_,true),syncing(kiwoomSyncing_,true);
    if(auto* view=findChild<KiwoomMarketView*>())view->setPrivateEnabled(false);
    if(chartKiwoomStream_){chartKiwoomStream_->stop();chartKiwoomStream_->setBroker(nullptr);}
}
void LocalWindow::setChartRowQuoteStream(PublicQuoteStream* stream){
    if(chartRowQuotes_==stream)return;
    if(chartRowQuotes_){disconnect(chartRowQuotes_,nullptr,this,nullptr);chartRowQuotes_->setMarkets({});}
    chartRowQuotes_=stream;chartRowTicks_.clear();chartRowQuoteMarkets_.clear();
    if(!chartRowQuoteTimer_){chartRowQuoteTimer_=new QTimer(this);chartRowQuoteTimer_->setSingleShot(true);chartRowQuoteTimer_->setInterval(1000);
        connect(chartRowQuoteTimer_,&QTimer::timeout,this,[this]{if(chartWorkspace_&&chartWorkspace_->isVisible())refreshChartWatchlists();});}
    if(!stream){refreshChartWatchlists();return;}
    connect(stream,&PublicQuoteStream::ticks,this,[this](const QList<PublicTick>& ticks){
        const auto now=QDateTime::currentMSecsSinceEpoch();
        for(const auto& tick:ticks){
            if(!chartRowQuoteMarkets_.contains(tick.market))continue;
            const auto row=projectBinanceWatchlistQuote({"coin","binance",tick.market},tick,now,false,
                chartRowQuotes_?chartRowQuotes_->streamEpoch():0);
            if(row.price.isEmpty()||row.quoteState==WatchlistQuoteState::Delayed)continue;
            const auto previous=chartRowTicks_.constFind(tick.market);
            // Subscription changes reset the socket's duplicate ledger, not
            // the original time of the same public row already on screen.
            if(previous!=chartRowTicks_.cend()&&tick.quote.evidence.sourceTimeMs<=previous->quote.evidence.sourceTimeMs)continue;
            chartRowTicks_.insert(tick.market,tick);
        }
        if(!chartRowQuoteTimer_->isActive())chartRowQuoteTimer_->start();});
    // Status notifications include healthy reconnect/subscribe transitions.
    // Preserve admitted public observations; their original times drive delay.
    connect(stream,&PublicQuoteStream::changed,this,[this]{if(!chartRowQuoteTimer_->isActive())chartRowQuoteTimer_->start();});
    connect(stream,&QObject::destroyed,this,[this]{chartRowTicks_.clear();chartRowQuoteMarkets_.clear();refreshChartWatchlists();});
    refreshChartWatchlists();
}
void LocalWindow::setKiwoomMarketStream(KiwoomMarketStream* stream){
    auto* context=qobject_cast<QScrollArea*>(connectionsPage_)?qobject_cast<QScrollArea*>(connectionsPage_)->widget():nullptr;
    if(!stream||!portfolio_||!context||findChild<KiwoomMarketView*>())return;
    chartKiwoomStream_=stream;revokeKiwoomMarketAuthorization();if(chartStockSource_)chartStockSource_->setKiwoomDailyStream(stream);
    auto* view=new KiwoomMarketView(portfolio_,stream,context);view->setProperty("card",true);view->setAttribute(Qt::WA_StyledBackground,true);
    if(auto* layout=qobject_cast<QVBoxLayout*>(context->layout()))layout->addWidget(view);
    connect(view,&KiwoomMarketView::selectedConnectionChanged,this,[this]{if(kiwoomStopping_||kiwoomRevoking_)return;syncKiwoomMarketSource();refreshChartWatchlists();});
    connect(view,&KiwoomMarketView::activeCountryRequested,this,[this](const QString& country){
        if(kiwoomStopping_||kiwoomRevoking_||(country!="KR"&&country!="US"))return;kiwoomCountry_=country;syncKiwoomMarketSource();refreshChartWatchlists();
    });
    connect(stream,&KiwoomMarketStream::changed,this,[this]{if(!kiwoomStopping_&&!kiwoomRevoking_)refreshChartWatchlists();});
    connect(stream,&QObject::destroyed,this,[this]{chartKiwoomStream_=nullptr;revokeKiwoomMarketAuthorization();if(!kiwoomStopping_)refreshChartWatchlists();});
    if(exploreStack_)connect(exploreStack_,&QStackedWidget::currentChanged,this,[this]{syncKiwoomMarketSource();});
    syncKiwoomMarketSource();
}
void LocalWindow::syncKiwoomMarketSource(){
    if(kiwoomSyncing_||kiwoomRevoking_||kiwoomStopping_||!chartKiwoomStream_)return;
    const QScopedValueRollback syncing(kiwoomSyncing_,true);
    const auto readable=[this]{return portfolio_&&!kiwoomStopping_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);};
    if(!readable()){revokeKiwoomMarketAuthorization();return;}
    const auto epoch=kiwoomEpoch_;const auto current=[&]{return epoch==kiwoomEpoch_&&readable()&&chartKiwoomStream_;};
    auto* view=findChild<KiwoomMarketView*>();if(!view)return;
    view->setPrivateEnabled(true);if(!current())return;
    const bool chartVisible=isVisible()&&!isMinimized()&&chartWorkspace_&&chartWorkspace_->isVisible();
    const auto* flow=findChild<QWidget*>("marketFlowContext");const bool flowVisible=isVisible()&&!isMinimized()&&flow&&flow->isVisible();
    if(!chartVisible&&!flowVisible){chartKiwoomStream_->stop();if(current())chartKiwoomStream_->setBroker(nullptr);return;}
    BrokerTradingController* owner=nullptr;const auto choice=view->selectedConnectionId();
    for(const auto& state:portfolio_->connections()){
        if(state.shared||state.account.provider!="kiwoom"||state.account.environment!="live"||state.state!="complete")continue;
        if(!choice.isEmpty()&&state.account.id!=choice)continue;
        auto* candidate=portfolio_->broker(state.account.id); // Borrow existing object; no job/authentication/account selection.
        if(!candidate||!candidate->authenticated())continue;
        owner=candidate;break;
    }
    if(!current())return;
    if(chartKiwoomStream_->broker()!=owner){
        chartKiwoomStream_->stop();if(!current())return;
        chartKiwoomStream_->setBroker(owner);if(!current())return;
    }
    if(!owner){chartKiwoomStream_->stop();return;}
    QString country=kiwoomCountry_;
    if(chartVisible){const auto selected=WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity());if(selected.market=="stock")country=selected.source;}
    view->setActiveCountryDisplay(country);if(!current())return;
    QList<KiwoomMarketTarget> targets;QSet<QString> publicKeys;
    if(chartLists_)for(const auto& list:chartLists_->userLists())for(const auto& entry:list.entries)if(entry.identity.market=="stock")publicKeys.insert(entry.identity.key());
    const auto append=[&](const WatchlistIdentity& identity,int priority,bool privateOnly){
        if(identity.market!="stock"||!WatchlistWorkspaceList::validIdentity(identity))return;
        targets.append({identity,{},priority,privateOnly&&!publicKeys.contains(identity.key())});
    };
    const bool holdingsList=chartWorkspace_&&chartWorkspace_->currentListId()=="@holdings";
    if(chartVisible){
        append(WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity()),0,holdingsList);
        // Seeing a public catalog row alone must never expand the broker lane.
        for(const auto& key:chartVisibleIdentities_)if(publicKeys.contains(key))append(WatchlistWorkspaceList::identityFromKey(key),2,false);
    }
    if(flowVisible&&marketMap_){const auto section=marketMap_->selectedSection();if(section=="kr"||section=="us")append({"stock",section=="kr"?QString("KR"):QString("US"),marketMap_->selectedId()},0,false);}
    if(chartLists_){for(const auto& identity:chartLists_->holdings())append(identity,1,true);
        for(const auto& list:chartLists_->userLists())for(const auto& entry:list.entries)append(entry.identity,3,false);}
    if(!current())return;
    chartKiwoomStream_->setActiveCountry(country);if(!current())return;
    chartKiwoomStream_->setTargets(targets);
}
void LocalWindow::setTossMarketStream(TossMarketStream* stream) {
    auto* context = qobject_cast<QScrollArea*>(connectionsPage_)?qobject_cast<QScrollArea*>(connectionsPage_)->widget():nullptr;
    if (!stream || !portfolio_ || !context || findChild<TossMarketView*>()) return;
    chartTossStream_=stream;
    for(auto* source:{chartStockSource_.data(),alertStockSource_.data(),studyStockSource_.data()})if(source)source->setRealtimeStream(stream);
    connect(stream,&TossMarketStream::changed,this,&LocalWindow::refreshChartWatchlists);
    auto* view = new TossMarketView(portfolio_, stream, context); view->setProperty("card", false); view->setAttribute(Qt::WA_StyledBackground,true);
    connect(view,&TossMarketView::selectedConnectionChanged,this,&LocalWindow::syncStockChartSources);
    auto* layout = qobject_cast<QVBoxLayout*>(context->layout()); layout->addWidget(view);
    if (watchlist_) {
        view->setWatchlist(watchlist_->ids("kr") + watchlist_->ids("us"));
        connect(watchlist_, &ExploreWatchlist::favouritesChanged, view, [this, view] {
            view->setWatchlist(watchlist_->ids("kr") + watchlist_->ids("us"));
        });
    }
    refreshMarketMap();syncChartSubscriptions();
}
HoldingMarketCatalog LocalWindow::holdingMarketCatalog(const QList<WatchlistIdentity>& stocks) const{
    HoldingMarketCatalog result;
    if(markets_){result.upbitRows=markets_->rows();result.upbitAvailable=markets_->marketListAvailable();}
    if(chartInstruments_){result.binance=chartInstruments_->instruments("binance");result.bithumb=chartInstruments_->instruments("bithumb");}
    for(const auto& identity:stocks)if(identity.market=="stock")result.verifiedStocks.append(identity);
    return result;
}
HoldingMarketPreferences LocalWindow::holdingMarketPreferences() const{
    HoldingMarketPreferences result;result.displayCurrency=moneyCurrency_==MoneyCurrency::Usd?QStringLiteral("USD"):QStringLiteral("KRW");
    result.sessionSelections=holdingMarketSession_;
    if(chartLists_){auto order=chartLists_->recentLists();for(const auto& list:chartLists_->userLists())if(!order.contains(list.id))order.append(list.id);
        for(const auto& id:order)for(const auto& entry:chartLists_->userList(id).entries)
            if(!result.publicPriority.contains(entry.identity))result.publicPriority.append(entry.identity);
    }
    return result;
}
void LocalWindow::clearHoldingMarketSession(){
    clearAssetShare();
    holdingMarketSession_.clear();holdingRows_.clear();if(chartWorkspace_)chartWorkspace_->setHoldingRows({});
}
void LocalWindow::refreshChartWatchlists(){
    if(!chartLists_||chartListsRefreshing_)return;
    chartListsRefreshing_=true;
    const bool marketListAvailable = markets_ && markets_->marketListAvailable();
    const auto verifiedMarkets = markets_ ? markets_->verifiedMarkets() : QStringList{};
    if (marketListAvailable != chartMarketListAvailable_ || verifiedMarkets != chartVerifiedUpbitMarkets_) {
        chartMarketListAvailable_ = marketListAvailable;
        chartVerifiedUpbitMarkets_ = verifiedMarkets;
        if (chartStudyFeed_) chartStudyFeed_->setVerifiedUpbitMarkets(verifiedMarkets, marketListAvailable);
        if (chartFlowMinutes_) chartFlowMinutes_->setVerifiedUpbitMarkets(verifiedMarkets, marketListAvailable);
        if (chartAlertFeed_) chartAlertFeed_->setVerifiedUpbitMarkets(verifiedMarkets, marketListAvailable);
    }
    syncKiwoomMarketSource();
    syncStockChartSources();
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const bool readable=portfolio_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);
    if(chartWorkspace_&&chartInstruments_&&chartCandles_){
        const auto selected=WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity());
        if(chartWorkspace_->isVisible()&&selected.market=="coin"&&selected.source=="binance")chartInstruments_->ensure("binance");
        // An empty catalogue explicitly revokes expired/missing base/quote
        // evidence; a public symbol suffix is never currency metadata.
        chartCandles_->setInstruments("binance",chartInstruments_->instruments("binance",now));
    }
    QSet<QString> knownCoins(chartPublicCoinIdentities_.cbegin(), chartPublicCoinIdentities_.cend());
    const auto retainCoin=[this,&knownCoins](const WatchlistIdentity& identity){
        if(identity.market!="coin"||!WatchlistWorkspaceList::validIdentity(identity))return;
        const auto key=identity.key();
        if(!knownCoins.contains(key)){knownCoins.insert(key);chartPublicCoinIdentities_.append(key);}
    };
    if(markets_)for(const auto& row:markets_->rows())retainCoin({"coin","upbit",row.market});
    const bool stockSelected=chartWorkspace_&&chartWorkspace_->selectedChartIdentity().startsWith("stock:");
    const auto stockSnapshot=chartStockSource_?chartStockSource_->snapshot():StockChartSnapshot{};
    const auto series=stockSelected?(stockSnapshot.series.identity==chartWorkspace_->selectedChartIdentity()?stockSnapshot.series:MarketCandleSeries{}):
        (chartCandles_?chartCandles_->series():MarketCandleSeries{});
    if(series.available)retainCoin(WatchlistWorkspaceList::identityFromKey(series.identity));
    QList<WatchlistIdentity> verified,held;
    struct BrokerHolding {WatchlistIdentity identity;QString provider;qint64 receivedAtMs=0;};
    QList<BrokerHolding> brokerHoldings;
    qint64 holdingsReceived=0;
    int unavailableHoldings=0;
    for(const auto& key:chartPublicCoinIdentities_)verified.append(WatchlistWorkspaceList::identityFromKey(key));
    if(chartStockSource_){
        const auto identity=WatchlistWorkspaceList::identityFromKey(stockSnapshot.series.identity);
        if(WatchlistWorkspaceList::validIdentity(identity)&&identity.market=="stock"&&
            stockSnapshot.source==StockSourceKind::Toss&&stockSnapshot.series.available)verified.append(identity);
    }
    if(readable&&chartKiwoomStream_)for(const auto& instrument:chartKiwoomStream_->instruments(now))
        if(instrument.verified&&WatchlistWorkspaceList::validIdentity(instrument.identity))verified.append(instrument.identity);
    QMap<QString,QStringList> legacy;
    if(chartSettings_)for(const auto& section:{QString("coin"),QString("kr"),QString("us")})
        legacy.insert(section,chartSettings_->value("explore/watchlist/"+section).toStringList());
    QStringList candidates=legacy.value("kr")+legacy.value("us");
    for(const auto& list:chartLists_->userLists())for(const auto& entry:list.entries)
        if(entry.identity.market=="stock")candidates.append(entry.identity.code);
    if(readable){
        const auto states=portfolio_->connections();
        for(const auto& state:states){
            if(state.shared||state.state!="complete"||state.receivedAtMs<=0||state.receivedAtMs>now||now-state.receivedAtMs>180000)continue;
            if(state.account.market=="domestic_equity"){
                auto* broker=portfolio_->broker(state.account.id);if(!broker)continue;
                const auto holdings=broker->marketDataHoldings();auto symbols=candidates+holdings;
                for(const auto& instrument:broker->instruments())symbols.append(instrument.symbol);
                symbols.removeDuplicates();
                for(const auto& symbol:symbols){const auto country=broker->marketDataCountry(symbol);
                    const WatchlistIdentity identity{"stock",country,symbol};
                    if(!WatchlistWorkspaceList::validIdentity(identity))continue;
                    verified.append(identity);if(holdings.contains(symbol))brokerHoldings.append({identity,state.account.provider,state.receivedAtMs});
                }
            }
        }
        const auto snapshots=portfolio_->snapshots();
        if(chartInstruments_)for(const auto& snapshot:snapshots)for(const auto& component:snapshot.components)
            if(component.kind==AssetComponentKind::Holding&&decimal::positive(component.quantity)&&
               (assetIdentity(snapshot,component)=="solana:native"||snapshot.provider=="binance"))chartInstruments_->ensure("binance");
        const auto catalog = holdingMarketCatalog(verified);
        const auto preferences = holdingMarketPreferences();
        holdingRows_=resolveHoldingMarkets(snapshots,states,lastValuation_.valuation.rows,catalog,preferences,now);
        // Broker chart holdings are a verified, fresh instrument list separate
        // from the evaluator's domestic-only snapshot. Preserve that existing
        // boundary without fabricating US valuation rows or reading accounts again.
        for(const auto& holding:brokerHoldings){
            const auto key=QStringLiteral("stock:KR:")+holding.identity.code;
            const auto route=resolveHoldingMarket(key,{holding.provider},catalog,preferences);
            auto found=std::find_if(holdingRows_.begin(),holdingRows_.end(),[&](const auto& row){return row.assetKey==key;});
            if(found==holdingRows_.end()){holdingRows_.append(route);found=std::prev(holdingRows_.end());found->displayName=found->symbol;}
            if(route.chart&&*route.chart==holding.identity){
                found->chart=route.chart;found->candidates=route.candidates;found->selectionRule=route.selectionRule;found->chartReason.clear();
                found->alertsEligible=true;found->observedAtMs=found->observedAtMs?qMin(found->observedAtMs,holding.receivedAtMs):holding.receivedAtMs;
                auto custody=std::find_if(found->custodies.begin(),found->custodies.end(),[&](const auto& value){return value.provider==holding.provider;});
                const auto label=holding.provider=="toss"?tr("토스"):holding.provider=="kiwoom"?tr("키움"):tr("증권사");
                if(custody==found->custodies.end())found->custodies.append({holding.provider,label,HoldingCustodyState::Fresh,holding.receivedAtMs});
                else{custody->state=HoldingCustodyState::Fresh;custody->observedAtMs=holding.receivedAtMs;}
            }
        }
        QSet<QString> verifiedKeys, heldKeys;
        for (const auto& identity : std::as_const(verified)) verifiedKeys.insert(identity.key());
        for(const auto& row:holdingRows_){
            for (const auto& candidate : row.candidates) {
                const auto key = candidate.key();
                if (!verifiedKeys.contains(key)) { verifiedKeys.insert(key); verified.append(candidate); }
            }
            if(!row.chart)++unavailableHoldings;
            if(row.alertsEligible&&row.chart){
                const auto key = row.chart->key();
                if (!heldKeys.contains(key)) { heldKeys.insert(key); held.append(*row.chart); }
                holdingsReceived=holdingsReceived?qMin(holdingsReceived,row.observedAtMs):row.observedAtMs;
            }
        }
    }else holdingRows_.clear();
    chartLists_->setVerifiedInstruments(verified);
    if(chartWorkspace_)chartWorkspace_->setPrivateEnabled(readable);
    if(chartWorkspace_)chartWorkspace_->setHoldingRows(holdingRows_);
    if(!readable)chartLists_->clearPrivate();else chartLists_->setHoldings(held);
    if(chartAlerts_){chartAlerts_->setPrivateEnabled(readable);
        if(readable)chartAlerts_->setHoldings(chartLists_->holdings(),holdingsReceived,chartAlerts_->privateEpoch());}
    if(chartWorkspace_)chartWorkspace_->setHoldingsUnavailableReason(readable&&unavailableHoldings>0?
        tr("공개 종목 식별 자료나 차트 지원을 확인하지 못한 보유는 알림에서 제외해요. 보유 목록에서 이유를 확인할 수 있어요."):QString{});
    chartLists_->migrateLegacy(legacy);
    // Market sources, authorization, alert holdings and list migration above remain
    // current in the background. Build display rows from them on re-entry.
    if (windowShownOnce_ && (!chartWorkspace_ || !chartWorkspace_->isVisible() || isMinimized())) {
        releaseChartRowQuotes();
        chartListsRefreshing_ = false;
        syncChartSubscriptions();
        return;
    }
    QHash<QString,WatchlistRowState> rows;
    for(const auto& key:chartObservedIdentities_)rows.insert(key,{});
    if(markets_)for(const auto& row:markets_->rows()){
        const WatchlistIdentity identity{"coin","upbit",row.market};
        if(!WatchlistWorkspaceList::validIdentity(identity))continue;
        if(!row.priced){rows.insert(identity.key(),{});continue;}
        WatchlistRowState quote{row.price,changePercentText(row.signedChangeRate),row.quoteUrl,{},0,false};
        quote.quoteCurrency="KRW";
        if(row.sourceTimeMs<=0||row.sourceTimeMs>now||now-row.sourceTimeMs>90000||row.receivedAtMs<=0||row.receivedAtMs>now||now-row.receivedAtMs>90000)
            quote.quoteState=WatchlistQuoteState::Delayed;
        rows.insert(identity.key(),quote);
    }
    // An already observed Binance ranking price can paint the selected chart before its first socket tick.
    // It must belong to this tradable spot instrument. Never turn the REST snapshot into a live tick.
    QString seededMarket;qint64 seededAt=0;
    if(board_&&chartInstruments_&&chartWorkspace_&&chartWorkspace_->isVisible()&&board_->status("binance").isEmpty()){
        const auto id=WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity());
        const auto instruments=chartInstruments_->instruments("binance",now);
        if(id.market=="coin"&&id.source=="binance"&&std::any_of(instruments.cbegin(),instruments.cend(),[&](const auto& m){
            return m.symbol==id.code&&m.market=="spot"&&m.tradable&&m.status=="TRADING"&&m.quoteAsset=="USDT"&&m.symbol==m.baseAsset+m.quoteAsset;
        })){
            for(const auto& ticker:board_->items("binance"))if(ticker.id==id.code&&ticker.sourceTimeMs>0&&ticker.sourceTimeMs<=now&&now-ticker.sourceTimeMs<=60000&&
                ticker.receivedAtMs>0&&ticker.receivedAtMs<=now&&now-ticker.receivedAtMs<=60000){
                WatchlistRowState quote;quote.price=ticker.price;quote.quoteCurrency="USDT";quote.change=(ticker.changePercent.toDouble()>0?QStringLiteral("+"):QString{})+ticker.changePercent+"%";
                quote.evidence=tr("바이낸스 현물 · 24시간 시세 · %1").arg(QDateTime::fromMSecsSinceEpoch(ticker.sourceTimeMs).toString("HH:mm:ss"));
                rows.insert(id.key(),quote);seededMarket=id.code;seededAt=ticker.sourceTimeMs;break;
            }
        }
    }
    // Binance Spot rows: subscribe only to the rows the chart can show, then use fresh public tickers.
    if(chartRowQuotes_){
        const auto instruments=chartInstruments_?chartInstruments_->instruments("binance",now):QList<MarketInstrument>{};
        QSet<QString> supported;
        for(const auto& item:instruments)
            if(item.market=="spot"&&item.status=="TRADING"&&item.tradable&&item.quoteAsset=="USDT"&&
               !item.baseAsset.isEmpty()&&item.symbol==item.baseAsset+item.quoteAsset&&binanceUsdtTickerSymbolValid(item.symbol))
                supported.insert(item.symbol);
        QStringList wanted;const auto want=[&](const WatchlistIdentity& identity){
            if(identity.market!="coin"||identity.source!="binance"||!WatchlistWorkspaceList::validIdentity(identity))return;
            if(chartInstruments_)chartInstruments_->ensure("binance");
            if(supported.contains(identity.code)&&!wanted.contains(identity.code)&&wanted.size()<50)wanted.append(identity.code);};
        QStringList displayed;const auto display=[&](const QString& key){
            const auto identity=WatchlistWorkspaceList::identityFromKey(key);
            if(identity.market=="coin"&&identity.source=="binance"&&WatchlistWorkspaceList::validIdentity(identity)&&
               binanceUsdtTickerSymbolValid(identity.code)&&!displayed.contains(identity.code)&&displayed.size()<50)
                displayed.append(identity.code);
        };
        if(chartWorkspace_&&chartWorkspace_->isVisible()&&isVisible()&&!isMinimized()){
            display(chartWorkspace_->selectedChartIdentity());
            for(const auto& key:chartWorkspace_->visiblePublicIdentityKeysForDisplay())display(key);
            if(!displayed.isEmpty()&&chartInstruments_)chartInstruments_->ensure("binance");
            want(WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity()));
            // An Upbit coin chart's header compares it with the same coin's Binance spot
            // market, matched by base asset in verified Binance metadata (as 김프 does).
            const auto selectedCoin=WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity());
            if(selectedCoin.market=="coin"&&selectedCoin.source=="upbit"&&selectedCoin.code.startsWith("KRW-")){
                if(chartInstruments_)chartInstruments_->ensure("binance");
                const auto cross=selectedCoin.code.mid(4)+"USDT";
                if(supported.contains(cross)){display(WatchlistIdentity{"coin","binance",cross}.key());want(WatchlistIdentity{"coin","binance",cross});}
            }
            for(const auto& key:chartVisibleIdentities_)want(WatchlistWorkspaceList::identityFromKey(key));
            if(readable)for(const auto& identity:chartLists_->holdings())want(identity);
        }
        // Keep symbols that already stream while the 50-symbol cap allows. Scrolling a long list then
        // reconnects only when a newly shown row is not streaming yet, not on every viewport change.
        if(wanted.isEmpty()){if(!chartRowQuoteMarkets_.isEmpty()){chartRowQuoteMarkets_.clear();chartRowQuotes_->setMarkets({});}}
        else if(std::any_of(wanted.cbegin(),wanted.cend(),[this](const QString& symbol){return !chartRowQuoteMarkets_.contains(symbol);})||
                std::any_of(chartRowQuoteMarkets_.cbegin(),chartRowQuoteMarkets_.cend(),[&supported](const QString& symbol){return !supported.contains(symbol);})){
            auto next=wanted;for(const auto& symbol:chartRowQuoteMarkets_)if(next.size()<50&&!next.contains(symbol)&&supported.contains(symbol))next.append(symbol);
            next.sort();if(next!=chartRowQuoteMarkets_){chartRowQuoteMarkets_=next;chartRowQuotes_->setMarkets(next);}
        }
        // A metadata refresh stops new subscriptions, not the last admitted
        // observation of a still-visible row. Confirmed removal revokes both.
        // Raw visible identities authorize retention only, never new requests.
        for(auto it=chartRowTicks_.begin();it!=chartRowTicks_.end();)
            if(!displayed.contains(it.key())||(!instruments.isEmpty()&&!supported.contains(it.key())))it=chartRowTicks_.erase(it);else ++it;
        for(auto it=chartRowTicks_.cbegin();it!=chartRowTicks_.cend();++it){
            if(!displayed.contains(it.key())||(it.key()==seededMarket&&it.value().quote.evidence.sourceTimeMs<seededAt))continue;
            const WatchlistIdentity identity{"coin","binance",it.key()};
            const auto quote=projectRetainedBinanceWatchlistQuote(identity,it.value(),instruments,now,chartRowQuotes_->status().contains(tr("끊")),chartRowQuotes_->streamEpoch());
            if(quote.price.isEmpty())continue;
            const bool oldQuote=quote.quoteState==WatchlistQuoteState::Delayed||quote.quoteState==WatchlistQuoteState::Disconnected;
            const bool newerCompleted=oldQuote&&series.available&&series.identity==identity.key()&&series.currency==quote.quoteCurrency&&!series.candles.isEmpty()&&
                series.candles.last().startsAtMs+series.intervalMs>it.value().quote.evidence.sourceTimeMs;
            if(newerCompleted){
                if(it.key()==seededMarket)rows.remove(identity.key());
                continue; // The newer same-identity completed observation below owns the fallback.
            }
            if(it.key()==seededMarket){
                // Older ticks were filtered above. This stream observation now owns the displayed price.
                rows.insert(identity.key(),quote);continue;
            }
            auto row=rows.value(identity.key());
            if(row.price.isEmpty()){row.price=quote.price;row.evidence=quote.evidence;row.quoteState=quote.quoteState;row.quoteCurrency=quote.quoteCurrency;}
            if(row.change.isEmpty())row.change=quote.change;
            rows.insert(identity.key(),row);
        }
    }
    if(series.available&&!series.candles.isEmpty()){
        const auto identity=WatchlistWorkspaceList::identityFromKey(series.identity);
        auto row=rows.value(identity.key());
        if(row.price.isEmpty()){
            row.price=series.candles.last().close+(stockSelected?" "+series.currency:QString{});
            row.evidence=(stockSelected&&stockSnapshot.delayed?tr("지연 일봉 종가 · "):tr("완료 봉 종가 · "))+series.sourceUrl;
        }
        row.completedAtMs=stockSelected?(stockSnapshot.timeline.rows.isEmpty()?0:stockSnapshot.timeline.rows.last().completesAtMs):series.candles.last().startsAtMs+series.intervalMs;
        row.badges=stockSelected&&stockSnapshot.delayed?QStringList{usabilityText(UsabilityText::Delayed)}:QStringList{};
        QHash<int,WatchlistBadge> directionalBadges;
        const auto studies=stockSelected?stockSnapshot.studies:chartWorkspace_->chartStudies();
        const auto* preferences=chartWorkspace_->indicatorPreferences();
        if(preferences&&chartWorkspace_->selectedChartIdentity()==series.identity&&
           studies.available&&studies.intervalMs==series.intervalMs&&studies.lastCompletedAtMs==row.completedAtMs){
            if(preferences->enabled("pullback")&&studies.pullback.available)
                row.badges.append(chartStudyName("pullback")+tr(" %1/10").arg(studies.pullback.score));
            if(preferences->enabled("similarity")&&studies.similarity.available)
                row.badges.append(chartStudyName("similarity")+tr(" %1곳").arg(studies.similarity.samples));
            if(preferences->enabled("cloud")&&!studies.cloud.isEmpty()){
                const auto& cloud=studies.cloud.last();
                row.badges.append(chartStudyName("cloud")+tr(" %1").arg(cloud.fast>cloud.slow?QStringLiteral("▲"):
                    cloud.fast<cloud.slow?QStringLiteral("▼"):QStringLiteral("—")));
                directionalBadges.insert(row.badges.size()-1,{chartStudyName("cloud"),cloud.fast>cloud.slow?MovementDirection::Up:cloud.fast<cloud.slow?MovementDirection::Down:MovementDirection::Flat,false});
            }
            if(preferences->enabled("volume")&&!studies.volumes.isEmpty())row.badges.append(chartStudyName("volume"));
        }
        if(!stockSelected&&preferences&&preferences->enabled("whales")){
            const auto flow=chartWorkspace_->coinFlowStudies();
            if(flow.identity==series.identity&&flow.lastCompletedAtMs>0&&flow.lastCompletedAtMs<=now&&now-flow.lastCompletedAtMs<=90000){
                row.badges.append(chartStudyName("whales")+tr(" · 실제 완료 1분"));
                if(flow.volume.state.available&&flow.volume.multiple)
                    row.badges.append(tr("거래량 %1배 · 표본 %2").arg(QString::number(*flow.volume.multiple,'f',2)).arg(flow.volume.state.samples));
                if(flow.largeTrade.state.available&&flow.largeTrade.latestPercentile)
                    row.badges.append(tr("대량 체결 %1백분위 · 이전 %2개").arg(QString::number(*flow.largeTrade.latestPercentile,'f',1)).arg(flow.largeTrade.baselineSamples));
                if(flow.reversal.state.available){
                    if(flow.reversal.rebound){row.badges.append(tr("반등 관측 ▲"));directionalBadges.insert(row.badges.size()-1,{tr("반등 관측"),MovementDirection::Up,false});}
                    if(flow.reversal.turnDown){row.badges.append(tr("꺾임 관측 ▼"));directionalBadges.insert(row.badges.size()-1,{tr("꺾임 관측"),MovementDirection::Down,false});}
                }
                if(flow.openInterest.state.available&&flow.openInterest.quantityChangePercent){
                    row.badges.append(tr("OI 5분 %1%2%").arg(*flow.openInterest.quantityChangePercent>0?QStringLiteral("▲ "):
                        *flow.openInterest.quantityChangePercent<0?QStringLiteral("▼ "):QStringLiteral("— "),QString::number(*flow.openInterest.quantityChangePercent,'f',2)));
                    const auto value=*flow.openInterest.quantityChangePercent;directionalBadges.insert(row.badges.size()-1,{tr("OI 5분 %1%").arg(QString::number(value,'f',2)),value>0?MovementDirection::Up:value<0?MovementDirection::Down:MovementDirection::Flat,false});
                }
            }
        }
        if(stockSelected&&preferences&&preferences->enabled("stock_flow")){
            const auto& flow=stockSnapshot.marketStudies;
            if(flow.foreignerConsecutiveBuyDays)row.badges.append(tr("등록외국인 순매수 %1거래일").arg(*flow.foreignerConsecutiveBuyDays));
            if(flow.institutionConsecutiveBuyDays)row.badges.append(tr("기관 순매수 %1거래일").arg(*flow.institutionConsecutiveBuyDays));
            if(flow.volumeMultiple)row.badges.append(tr("거래량 %1배").arg(QString::number(*flow.volumeMultiple,'f',2)));
        }
        row.badgePresentations.clear();for(int i=0;i<row.badges.size();++i)row.badgePresentations.append(directionalBadges.value(i,{row.badges[i],MovementDirection::Unknown,false}));
        row.privateData=stockSelected&&stockSnapshot.source==StockSourceKind::Toss;rows.insert(identity.key(),row);
    }
    QSet<QString> tossQuoteKeys;
    if(readable&&chartTossStream_){
        for(const auto& list:chartLists_->userLists())for(const auto& entry:list.entries)
            if(entry.identity.market=="stock"&&(!series.available||entry.identity.key()!=series.identity))rows.insert(entry.identity.key(),{});
        for(const auto& identity:held)if(identity.market=="stock"&&(!series.available||identity.key()!=series.identity))rows.insert(identity.key(),{});
        for(const auto& quote:chartTossStream_->quotes()){
            const auto& e=quote.evidence;
            if(e.receivedAtMs<=0||e.receivedAtMs>now||now-e.receivedAtMs>90000)continue;
            const WatchlistIdentity identity{"stock",quote.marketCountry,quote.symbol};
            tossQuoteKeys.insert(identity.key());
            auto row=rows.value(identity.key());row.price=quote.price+" "+quote.currency;row.evidence=e.url;row.privateData=true;rows.insert(identity.key(),row);
        }
    }
    if(readable&&chartKiwoomStream_)for(const auto& quote:chartKiwoomStream_->quotes(now)){
        const auto& evidence=quote.evidence;const auto& identity=quote.identity;
        if(!WatchlistWorkspaceList::validIdentity(identity)||identity.market!="stock"||tossQuoteKeys.contains(identity.key())||
            !evidence.complete||evidence.receivedAtMs<=0||evidence.receivedAtMs>now||now-evidence.receivedAtMs>KiwoomMarketStream::QuoteFreshnessMs)continue;
        auto row=rows.value(identity.key());row.price=quote.price+" "+quote.currency;row.evidence=evidence.url;row.privateData=true;
        const auto label=quote.realtime?tr("키움 실시간 · 원천 UTC 미제공"):tr("키움 참고 가격 · 원천 시각 미제공");
        row.badges.append(label);row.badgePresentations.append({label,MovementDirection::Unknown,false});rows.insert(identity.key(),row);
    }
    const auto annotateMissing=[&](const WatchlistIdentity& identity){
        auto row=rows.value(identity.key());if(!row.price.isEmpty())return;
        if(identity.market=="stock"){
            if(stockChartConnectionState()!=StockChartConnectionState::Ready)row.quoteState=WatchlistQuoteState::ConnectionRequired;
        }else if(identity.source=="upbit"&&markets_){
            const auto known=markets_->rows();bool listed=false;for(const auto& item:known)if(item.market==identity.code){listed=true;break;}
            if(!known.isEmpty()&&!listed)row.quoteState=WatchlistQuoteState::Unsupported;
            else if(!markets_->busy()&&!markets_->status().isEmpty())row.quoteState=WatchlistQuoteState::Disconnected;
        }else if(identity.source=="binance"&&chartInstruments_){
            const auto known=chartInstruments_->instruments("binance");bool listed=false;for(const auto& item:known)if(item.symbol==identity.code&&item.tradable){listed=true;break;}
            if(!known.isEmpty()&&!listed)row.quoteState=WatchlistQuoteState::Unsupported;
            else if(chartRowQuotes_&&chartRowQuotes_->status().contains(tr("끊")))row.quoteState=WatchlistQuoteState::Disconnected;
        }
        rows.insert(identity.key(),row);
    };
    for(const auto& list:chartLists_->userLists())for(const auto& entry:list.entries)annotateMissing(entry.identity);
    for(const auto& identity:held)annotateMissing(identity);
    chartObservedIdentities_.clear();
    for(auto it=rows.cbegin();it!=rows.cend();++it)if(it.value()!=WatchlistRowState{})chartObservedIdentities_.append(it.key());
    chartLists_->setRowStates(rows);
    chartListsRefreshing_=false;syncChartSubscriptions();
    refreshChartMarketContext();
}
void LocalWindow::syncWorldMarketsDemand(){
    if(!worldMarkets_)return;
    const bool shown=isVisible()&&!isMinimized();
    // Today reads the whole board (13 quotes a minute at most); a chart reads only what its
    // header names; nothing is read while neither is on screen.
    if(shown&&todayDashboard_&&todayDashboard_->isVisibleTo(this))worldMarkets_->setDemand(WorldMarketsDemand::full());
    else if(shown&&chartWorkspace_&&chartWorkspace_->isVisibleTo(this))worldMarkets_->setDemand(WorldMarketsDemand::chartHeader());
    else worldMarkets_->setDemand({});
}
void LocalWindow::refreshChartMarketContext(){
    if(!chartWorkspace_)return;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto identity=WatchlistWorkspaceList::identityFromKey(chartWorkspace_->selectedChartIdentity());
    ChartMarketInputs in;in.market=identity.market;in.source=identity.source;in.code=identity.code;in.nowMs=now;
    if(worldMarkets_)in.world=worldMarkets_->snapshot();
    // The public price the chart shows; a private (holdings) price never feeds public context.
    const auto row=chartLists_?chartLists_->rowState(identity):WatchlistRowState{};
    if(!row.privateData){
        const auto parts=row.price.trimmed().split(' ',Qt::SkipEmptyParts);
        in.price=QString(parts.value(0)).remove(',');in.currency=parts.size()>1?parts.value(1):row.quoteCurrency;
    }
    if(identity.market=="coin"&&identity.source=="upbit"&&in.currency.isEmpty())in.currency=QStringLiteral("KRW");
    if(identity.market=="coin"&&WatchlistWorkspaceList::validIdentity(identity)){
        QString base,symbol;
        if(identity.source=="upbit"&&identity.code.startsWith("KRW-")){base=identity.code.mid(4);symbol=base+"USDT";}
        else if(identity.source=="binance"&&chartInstruments_)
            for(const auto& item:chartInstruments_->instruments("binance",now))
                if(item.symbol==identity.code&&item.market=="spot"&&item.quoteAsset=="USDT"&&!item.baseAsset.isEmpty()){base=item.baseAsset;symbol=item.symbol;break;}
        if(!base.isEmpty()&&markets_)for(const auto& market:markets_->rows())
            if(market.market=="KRW-"+base&&market.priced){in.upbitKrw=market.price;in.upbitAtMs=market.sourceTimeMs;break;}
        const auto tick=chartRowTicks_.constFind(symbol);
        if(!symbol.isEmpty()&&tick!=chartRowTicks_.cend()){in.binanceUsdt=tick->quote.price;in.binanceAtMs=tick->quote.evidence.sourceTimeMs;}
    }
    chartWorkspace_->setMarketContext(chartMarketContext(in));
}
void LocalWindow::clearChartAlertPrivate(){
    const QScopedValueRollback clearing(chartRiskSyncing_,true);
    chartRiskTokens_.clear();
    if(chartAlerts_)chartAlerts_->clearPrivate();
    syncCoinFlowDemand();
    if(chartWorkspace_)chartWorkspace_->setFuturesObservationActive(false);
    if(chartAlertsPanel_)chartAlertsPanel_->clearPrivate();
}
void LocalWindow::syncChartRisk(){
    if(!chartAlerts_||!chartWorkspace_||chartRiskSyncing_)return;
    const QScopedValueRollback syncing(chartRiskSyncing_,true);
    const bool readable=portfolio_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);
    const auto rule=chartAlerts_->rule("@holdings");
    const bool active=readable&&rule.enabled&&rule.liquidation;
    chartWorkspace_->setFuturesObservationActive(active);
    QList<ChartLiquidationObservation> observations;QHash<QString,QString> nextTokens;
    const auto epoch=chartAlerts_->privateEpoch();
    if(active)for(const auto& position:chartWorkspace_->futuresPositionObservations()){
        if(!position.available||!position.distanceAvailable||position.contractSymbol.isEmpty())continue;
        const auto key=position.accountScope+QChar(0x1f)+position.contractSymbol+QChar(0x1f)+position.positionSide;
        auto token=chartRiskTokens_.value(key);if(token.isEmpty())token=QUuid::createUuid().toString(QUuid::WithoutBraces);
        nextTokens.insert(key,token);
        ChartLiquidationObservation value;value.identity={"usdm_futures",position.provider,position.contractSymbol};
        value.positionToken=token;value.markPrice=position.markPrice;value.liquidationPrice=position.liquidationPrice;
        value.currency=position.currency;value.sourceUrl=position.sourceUrl;value.receivedAtMs=position.receivedAtMs;
        value.verified=true;value.positionSide=position.positionSide;observations.append(value);
    }
    chartRiskTokens_=nextTokens;chartAlerts_->setLiquidationObservations(observations,epoch);
}
void LocalWindow::syncChartAlerts(){
    if(!chartAlerts_||!chartLists_||chartAlertsSyncing_||chartListsRefreshing_)return;
    chartAlertsSyncing_=true;
    QList<ChartAlertList> lists;QMap<QString,QString> labels;
    for(const auto& list:chartLists_->userLists()){
        ChartAlertList value;value.listId=list.id;
        for(const auto& entry:list.entries)value.identities.append(entry.identity);
        lists.append(value);labels.insert(list.id,list.flag.isEmpty()?list.name:list.flag+" "+list.name);
    }
    chartAlerts_->setPublicLists(lists);
    if(chartWorkspace_&&chartWorkspace_->drawingModel())chartAlerts_->setHorizontalLines(chartWorkspace_->drawingModel()->horizontalLines());
    QSet<QString> enabledLines;for(const auto& list:lists){const auto rule=chartAlerts_->rule(list.listId);if(rule.enabled&&rule.horizontal)enabledLines.insert(list.listId);}
    if(chartWorkspace_)chartWorkspace_->setDrawingAlertEnabledLists(enabledLines);
    const bool readable=portfolio_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);
    chartAlerts_->setPrivateEnabled(readable);
    if(chartAlertsPanel_)chartAlertsPanel_->setListLabels(labels);
    if(chartAlertFeed_){
        QStringList priority;
        if(chartWorkspace_)priority.append(chartWorkspace_->selectedChartIdentity());
        for(const auto& identity:chartLists_->holdings())priority.append(identity.key());
        if(chartWorkspace_&&chartWorkspace_->isVisible())priority.append(chartVisibleIdentities_);
        for(const auto& list:lists)for(const auto& identity:list.identities)priority.append(identity.key());
        priority.removeDuplicates();chartAlertFeed_->setPriorityIdentities(priority);
    }
    chartAlertsSyncing_=false;
    syncChartRisk();syncCoinFlowDemand();
}
void LocalWindow::releaseChartRowQuotes() {
    chartRowTicks_.clear();
    if (!chartRowQuoteMarkets_.isEmpty()) {
        chartRowQuoteMarkets_.clear();
        if (chartRowQuotes_) chartRowQuotes_->setMarkets({});
    }
}
void LocalWindow::syncChartTab(){
    syncChartPresentation();
}
void LocalWindow::syncChartPresentation(){
    syncExplorePage();
    if(markets_)markets_->setPresentationActive((isVisible()&&!isMinimized())||(chartAlerts_&&chartAlerts_->observationEnabled("rapid_move")));
    refreshWatchlist(); // Visible selected-detail history shares the existing paced queue.
    if(chartWorkspace_&&chartLists_){
        if(chartWorkspace_->isVisible()&&!isMinimized())refreshChartWatchlists();
        else {releaseChartRowQuotes();syncChartSubscriptions();}
    }else{
        syncKiwoomMarketSource();
        syncCoinFlowDemand();
    }
    syncMarketFlowHistory();
    refreshPublicObservations();
}
void LocalWindow::syncChartSubscriptions(){
    if(!chartWorkspace_||!chartLists_)return;
    syncCoinFlowDemand();
    syncChartAlerts();
    syncKiwoomMarketSource();
    auto* view=findChild<TossMarketView*>();
    QStringList rest;for(const auto& list:chartLists_->userLists())for(const auto& entry:list.entries)rest.append(entry.identity.key());
    if(view)view->setChartContext(!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_)&&chartWorkspace_->isVisible(),
        chartVisibleIdentities_,chartWorkspace_->selectedChartIdentity(),rest);
    QSet<QString> allocated;
    if(chartTossStream_)for(const auto& target:chartTossStream_->targets())allocated.insert(WatchlistIdentity{"stock",target.marketCountry,target.symbol}.key());
    if(chartKiwoomStream_)for(const auto& target:chartKiwoomStream_->realtimeTargets())allocated.insert(target.identity.key());
    QStringList delayed;if(chartTossStream_)for(const auto& target:chartTossStream_->restOnlyTargets())
        delayed.append(WatchlistIdentity{"stock",target.marketCountry,target.symbol}.key());
    if(chartKiwoomStream_)for(const auto& target:chartKiwoomStream_->restOnlyTargets())delayed.append(target.identity.key());
    delayed.removeDuplicates();
    delayed.erase(std::remove_if(delayed.begin(),delayed.end(),[&](const auto& key){return allocated.contains(key);}),delayed.end());
    chartWorkspace_->setRealtimeDelayedIdentities(delayed);
}
void LocalWindow::setChartContractTransport(TradingTransport* transport){
    if(chartWorkspace_)chartWorkspace_->setContractTransport(transport);
}
void LocalWindow::selectMarketMetric(const QString& section, const QString& asset) {
    if(section=="kr"||section=="us"){kiwoomCountry_=section=="kr"?QString("KR"):QString("US");syncKiwoomMarketSource();}
    if (auto* stocks = findChild<TossMarketView*>()) stocks->setSelection(section=="kr"||section=="us"?asset:QString{});
    if (!marketMetricsView_ || section != "coin") return;
    marketMetricsView_->setAsset(asset);syncMarketFlowBackground();
    if(chartWorkspace_){const auto route=publicChartRoute("coin",marketMetricsView_->asset());
        if(route.chart)chartWorkspace_->setIdentity(route.chart->key(),false);
    }
    if (auto* input = findChild<QLineEdit*>("marketFlowAsset")) input->setText(marketMetricsView_->asset());
    syncMarketFlowHistory();
    refreshPublicObservations();
}

void LocalWindow::showMapSelection() {
    if (!marketMap_) return;
    showPublicDetail(mapDetail_, marketMap_->selectedSection(), marketMap_->selectedId());
}

void LocalWindow::showPublicDetail(AssetDetailPanel* panel, const QString& section, const QString& id) {
    if (!panel || !markets_) return;
    const bool readable = securityReady_ && accountReadsAvailable(vault_);
    if (readable) exploreHasPrivateProjection_ = true;
    auto boardItems = board_ ? board_->items(section) : QList<PublicBoardItem>{};
    if (board_) {
        const auto previous = board_->knownItem(section, id);
        if (!previous.id.isEmpty() && std::none_of(boardItems.cbegin(), boardItems.cend(), [&](const auto& item) { return item.symbol == id; })) boardItems.append(previous);
    }
    const bool hasBoardQuote = std::any_of(boardItems.cbegin(), boardItems.cend(), [&](const auto& item) { return item.symbol == id; });
    if (section == "coin" && !hasBoardQuote) {
        for (const auto& row : markets_->rows()) if (row.base == id && row.priced) {
            const PortfolioGroupView* holdings = nullptr;
            if (readable) for (const auto& group : portfolioView_.groups) if (group.identity == publicAssetIdentity("upbit",row.base)) { holdings = &group; break; }
            const bool incomplete = readable && (!portfolioView_.missing.isEmpty() || !portfolioView_.summary.readingAccounts.isEmpty());
            panel->showMarket(row, readable, holdings, incomplete);
            panel->setPublicSelection(section, id, watchlist_ && watchlist_->contains(section, id));
            refreshPublicDetailPresentation(panel,"upbit",id,"coin:upbit:"+row.market,"KRW","coin:upbit:"+row.market);
            queueLegacyExploreEvidence();
            return;
        }
    }
    if (board_) for (const auto& item : boardItems) if (item.symbol == id) {
        QString held = readable ? tr("없음") : tr("금고를 열면 표시");
        const auto holdingIdentity=section=="coin"?(verifiedCoinMarketCap(id,markets_->rows(),boardItems)?publicAssetIdentity("upbit",id):QString{}):section=="kr"?"stock:KR:"+id:"stock:US:"+id;
        // An unproven public join is not evidence of absence: never say "없음".
        if (readable && holdingIdentity.isEmpty()) held = tr("확인 불가 · 거래소 종목과 같은 코인인지 확인하지 못했어요");
        if (readable) for (const auto& group : portfolioView_.groups)
            if (group.identity == holdingIdentity) held = tr("보유 · 보관처 %1곳").arg(group.storages.size());
        const auto received = item.receivedAtMs;
        const QString source = section == "coin" ? tr("CoinGecko") : section == "kr" ? tr("네이버 증권") : tr("Nasdaq");
        const auto currentItems = board_->items(section);
        const bool stale = !board_->status(section).isEmpty() || QDateTime::currentMSecsSinceEpoch() - received > 120000 ||
            std::none_of(currentItems.cbegin(), currentItems.cend(), [&](const auto& current) { return current.symbol == id; });
        const auto sourceKey=section=="coin"?"coin:coingecko:"+item.sourceId:section=="kr"?"public:naver:"+id:"public:nasdaq:"+id;
        // Mockup fact rows: coins show 김프 and 24h traded value; stocks show their market-cap rank.
        QList<QPair<QString, QString>> facts;
        if (section == "coin") {
            const auto gap = flowKoreaGaps_.value(id);const auto now = QDateTime::currentMSecsSinceEpoch();
            if (!gap.first.isEmpty() && gap.second > 0 && gap.second <= now && now - gap.second <= MarketMetrics::FreshnessMs)
                facts.append({usabilityText(UsabilityText::FlowMetricKoreaGap), groupedAmount(marketFlowRounded(gap.first, 2)) + "% · " +
                    marketFlowStateLabel(marketFlowState(MarketFlowMetric::KoreaGap, gap.first))});
            if (item.tradedValue > 0) facts.append({tr("24시간 거래대금"), compactCap(item.tradedValue, item.currency)});
        } else {
            const auto rank = std::find_if(currentItems.cbegin(), currentItems.cend(), [&](const auto& current) { return current.symbol == id; });
            if (rank != currentItems.cend()) facts.append({tr("시가총액 순위"), tr("%1위 · %2").arg(rank - currentItems.cbegin() + 1).arg(section == "kr" ? tr("코스피") : tr("나스닥"))});
        }
        facts.append({tr("시가총액"), compactCap(item.marketCap, item.currency)});
        if (readable) for (const auto& group : portfolioView_.groups) if (group.identity == holdingIdentity) {
            QString quantity = "0";for (const auto& holding : group.holdings) if (decimal::valid(holding.quantity)) quantity = decimal::add(quantity, holding.quantity);
            QString line = groupedAmount(quantity) + " " + (group.displaySymbol.isEmpty() ? item.symbol : group.displaySymbol);
            if (decimal::positive(group.freshTotal)) line += " · " + groupedAmount(QString::number(std::round(group.freshTotal.toDouble()), 'f', 0)) + tr("원");
            facts.append({tr("내 보유"), line}); break;
        }
        panel->showQuote(item.name, item.symbol + " · " + item.group + (stale ? tr(" · 이전 시세") : QString{}),
            section == "us" ? "$" + priceAmount(item.price) : priceAmount(item.price) + tr(" KRW"), item.changePercent,
            facts,
            {tr("%1 · %2 수신").arg(source, received > 0 ? QDateTime::fromMSecsSinceEpoch(received).toString("MM-dd HH:mm:ss") : tr("시각 미확인")),
             tr("공개 시세 · 지연될 수 있고 자산 평가에는 쓰지 않습니다.")}, section == "coin" ? tr("최근 24시간") : tr("전일 대비"),item.currency,sourceKey,stale);
        panel->setPublicEvidence({{sourceKey,source,{},0,received,!stale,stale?tr("지연"):tr("유효")}});
        panel->setPrivateLinkage(readable,tr("내 보유 · %1").arg(held));
        panel->setPublicSelection(section, id, watchlist_ && watchlist_->contains(section, id));
        const auto identity=section=="kr"?"stock:KR:"+id:section=="us"?"stock:US:"+id:QString{};
        refreshPublicDetailPresentation(panel,section,id,sourceKey,item.currency,identity);
        queueLegacyExploreEvidence();
        return;
    }
    panel->clearDetail();
    queueLegacyExploreEvidence();
}
void LocalWindow::connectPublicChartLink(AssetDetailPanel* panel){
    auto* link=panel->findChild<QPushButton*>("assetDetailChart");if(!link)return;
    connect(link,&QPushButton::clicked,this,[this,panel]{
        if(!panel->hasSelection())return;
        // The quote's provider does not determine which exchange supplies its
        // chart. Share the ranking/watchlist route, including unsupported and
        // broker-connection guidance, instead of treating a missing link as data.
        openPublicChart(panel->property("exploreChartSection").toString(),panel->property("exploreChartId").toString());
    });
}
HoldingChartRow LocalWindow::publicChartRoute(const QString& section,const QString& id) const{
    HoldingChartRow result;
    if(section=="coin"||section=="upbit"||section=="binance"){
        auto catalog=holdingMarketCatalog();auto preferences=holdingMarketPreferences();
        preferences.sessionSelections.clear(); // Public navigation never adopts a private-derived choice.
        bool canonical=true;
        if(section=="coin"&&board_){auto items=board_->items("coin");
            // A retained CoinGecko quote keeps its identity requirements after
            // leaving the current board, just as in showPublicDetail.
            const auto previous=board_->knownItem("coin",id);
            if(!previous.id.isEmpty()&&std::none_of(items.cbegin(),items.cend(),[&](const auto& item){return item.symbol==id;}))items.append(previous);
            const bool fromBoard=std::any_of(items.cbegin(),items.cend(),[&](const auto& item){return item.symbol==id;});
            if(fromBoard){
                // An Upbit KRW market of this symbol must prove the same coin
                // (pinned id or Upbit's English name). When the verified Upbit list
                // has no such market, no exchange name contradicts the global quote:
                // navigation keeps the Binance spot route, private joins stay closed.
                const bool upbitListed=std::any_of(catalog.upbitRows.cbegin(),catalog.upbitRows.cend(),
                    [&](const auto& row){return row.base==id&&row.market=="KRW-"+id;});
                if(upbitListed)canonical=verifiedCoinMarketCap(id,catalog.upbitRows,items).has_value();
                else if(catalog.upbitAvailable&&std::none_of(items.cbegin(),items.cend(),[&](const auto& item){return item.symbol==id&&item.ambiguousSymbol;})){
                    catalog.upbitRows.clear();catalog.upbitAvailable=false;
                }
                else canonical=false;
            }
        }
        if(section=="upbit")catalog.binance.clear();
        if(section=="binance"){catalog.upbitRows.clear();catalog.upbitAvailable=false;}
        const auto route=resolveHoldingMarket(publicAssetIdentity("upbit",id),{},catalog,preferences);
        if(canonical&&route.chart)result=route;
        else if(!canonical)result.chartReason=tr("공개 시총 종목과 거래소 종목의 같은 자산 여부를 확인할 수 없어요.");
        else result.chartReason=tr("%1 종목의 검증된 차트 지원 시장을 확인할 수 없어요.").arg(id);
    }else if(section=="kr"||section=="us"){
        const auto identity=WatchlistIdentity{"stock",section=="kr"?QStringLiteral("KR"):QStringLiteral("US"),id};
        const auto verified=chartLists_&&chartLists_->isCurrentlyVerified(identity);
        const auto route=resolveHoldingMarket(identity.key(),{},holdingMarketCatalog(verified?QList<WatchlistIdentity>{identity}:QList<WatchlistIdentity>{}),{});
        if(route.chart)result=route;
        else{const auto message=stockChartConnectionMessage(stockChartConnectionState());
            result.chartReason=tr("주식 차트 · %1").arg(message.isEmpty()?tr("이 종목은 연결된 증권사에서 확인되지 않았어요"):message);}
    }
    return result;
}
void LocalWindow::openChartBrokerConnection(const QString& key, qint64 interval) {
    const auto identity = WatchlistWorkspaceList::identityFromKey(key);
    if (!chartWorkspace_ || identity.market != "stock" || !WatchlistWorkspaceList::validIdentity(identity) || interval <= 0)
        return;
    if (!vault_.isUnlocked()) {
        openVaultDialog(int(vault_.exists() ? VaultDialog::Mode::Unlock : VaultDialog::Mode::Create),
                        [this, key, interval] { openAccountDialog(false, key, interval); });
        return;
    }
    openAccountDialog(false, key, interval);
}

void LocalWindow::returnToBrokerChart(const QString& key, qint64 interval, const QString& connectionId) {
    auto* pages = findChild<QTabWidget*>("portfolioPages");
    if (!pages || !chartWorkspace_ || !securityReady_ || !vault_.isUnlocked()) return;
    pages->setCurrentWidget(chartWorkspace_);
    if (!chartWorkspace_->openStockFromNavigation(key)) {
        showTransientFeedback(feedback_, tr("종목을 다시 열 수 없어요. 차트에서 종목을 확인해 주세요."));
        return;
    }
    chartWorkspace_->setTimeframe(interval);
    if (portfolio_) for (const auto& entry : portfolio_->connections()) {
        if (entry.account.id != portfolio_->canonicalConnection(connectionId) || entry.state != "complete" || entry.shared) continue;
        const bool kiwoom = entry.account.provider == "kiwoom";
        if (kiwoom) {
            if (auto* view = findChild<KiwoomMarketView*>()) view->selectConnection(entry.account.id);
        } else if (entry.account.provider == "toss") {
            if (auto* view = findChild<TossMarketView*>()) view->selectConnection(entry.account.id);
        } else continue;
        if (chartStockSource_) chartStockSource_->setPreferKiwoomDaily(kiwoom);
        break;
    }
    syncStockChartSources();
    syncKiwoomMarketSource();
    syncChartSubscriptions();
}

void LocalWindow::openPublicChart(const QString& section,const QString& id){
    if(chartInstruments_&&(section=="coin"||section=="binance"))chartInstruments_->ensure("binance");
    const auto route=publicChartRoute(section,id);
    const auto key=route.chart?route.chart->key():QString{};const auto reason=route.chartReason;
    auto* pages=findChild<QTabWidget*>("portfolioPages");
    if(key.isEmpty()||!chartWorkspace_||!pages){
        const bool coin=section=="coin"||section=="upbit"||section=="binance";
        const auto message=coin?tr("%1 차트를 열 수 없어요").arg(id)+(reason.isEmpty()?QString{}:tr(" · ")+reason):
            reason.isEmpty()?tr("차트를 열 수 없어요"):reason;
        showTransientFeedback(feedback_,message);
        if((section=="kr"||section=="us")&&pages){
            if(auto* existing=findChild<QDialog*>("chartConnectionGuide")){existing->raise();return;}
            auto* guide=new QDialog(this);guide->setObjectName("chartConnectionGuide");guide->setAttribute(Qt::WA_DeleteOnClose);guide->setWindowTitle(tr("주식 차트 연결"));auto* layout=new QVBoxLayout(guide);
            auto* text=new QLabel(reason,guide);text->setTextFormat(Qt::PlainText);text->setWordWrap(true);layout->addWidget(text);
            if (chartWorkspace_ && stockChartConnectionState() != StockChartConnectionState::Ready) {
                auto* open = new QPushButton(tr("증권 연결하기"), guide);
                open->setObjectName("publicStockConnect");
                layout->addWidget(open);
                const auto key = WatchlistIdentity{"stock", section == "kr" ? QString("KR") : QString("US"), id}.key();
                connect(open, &QPushButton::clicked, this, [this, guide, key] {
                    guide->accept();
                    openChartBrokerConnection(key, chartWorkspace_->timeframeMs());
                });
            }
            auto* close=new QPushButton(tr("닫기"),guide);layout->addWidget(close);connect(close,&QPushButton::clicked,guide,&QDialog::close);guide->open();
        }
        return;
    }
    if (section == "kr" || section == "us") {
        if (!chartWorkspace_->openStockFromNavigation(key)) return;
    } else {
        chartWorkspace_->setIdentity(key, true);
    }
    pages->setCurrentWidget(chartWorkspace_);
}
void LocalWindow::refreshPublicDetailPresentation(AssetDetailPanel* panel,const QString& section,const QString& id,const QString& sourceKey,const QString& unit,const QString& chartIdentity){
    if(!panel)return;panel->setPublicHistoryContext(sourceKey,unit);QString verifiedChart;
    const auto identity=WatchlistWorkspaceList::identityFromKey(chartIdentity);
    if(chartWorkspace_&&WatchlistWorkspaceList::validIdentity(identity)){
        bool verified=chartLists_&&chartLists_->isCurrentlyVerified(identity);
        if(identity.market=="coin"&&identity.source=="upbit"&&markets_)for(const auto& row:markets_->rows())if(row.market==identity.code)verified=true;
        if(verified)verifiedChart=chartIdentity;
    }panel->setChartIdentity(verifiedChart);
    panel->setProperty("exploreChartSection",section);panel->setProperty("exploreChartId",id);
    if(auto* link=panel->findChild<QPushButton*>("assetDetailChart"))
        link->setVisible(chartWorkspace_&&!id.isEmpty()&&(section=="coin"||section=="upbit"||section=="binance"||section=="kr"||section=="us"));
    const bool stock=section=="kr"||section=="us";
    const auto connectionReason=stockChartConnectionMessage(stockChartConnectionState());
    ExploreMetricHistory plot;plot.sourceKey=sourceKey;plot.unit=unit;
    if(stock)plot.reason=!connectionReason.isEmpty()?connectionReason:tr("증권사 차트에서 확인하세요");
    else if(marketHistory_){const auto recorded=marketHistory_->history(section=="upbit"||section=="binance"?QStringLiteral("coin"):section,id);plot.reason=recorded.status;
        const bool upbit=sourceKey=="coin:upbit:KRW-"+id&&unit=="KRW"&&recorded.source==QStringLiteral("업비트 · 최근 24시간 · 10분 종가 · KRW");
        if(upbit){qint64 previous=0;for(const auto& point:recorded.points){const auto at=qint64(point.x());plot.points.append({at,point.y(),previous>0&&at-previous==600000});previous=at;}plot.available=!plot.points.isEmpty();}
        else if(!recorded.points.isEmpty())plot.reason=tr("같은 종목·원천·통화를 확인한 기록만 표시합니다.");
    }else plot.reason=tr("실제 기록이 없어 작은 차트를 접었습니다.");
    panel->setPublicHistory(plot);
    QList<PublicDetailIndicator> values;
    if(stock&&!connectionReason.isEmpty())values.append({tr("주식 차트"),connectionReason});
    else if(chartWorkspace_&&!verifiedChart.isEmpty()&&chartWorkspace_->selectedChartIdentity()==verifiedChart){const auto study=chartWorkspace_->chartStudies();if(study.available){
        values.append({chartStudyName("pullback"),study.pullback.available?tr("%1/10").arg(study.pullback.score):QString{}});
        values.append({chartStudyName("similarity"),study.similarity.available?tr("표본 %1개").arg(study.similarity.samples):QString{}});
        const auto direction=study.cloud.isEmpty()?MovementDirection::Unknown:study.cloud.last().fast>study.cloud.last().slow?MovementDirection::Up:study.cloud.last().fast<study.cloud.last().slow?MovementDirection::Down:MovementDirection::Flat;
        values.append({chartStudyName("cloud"),!study.cloud.isEmpty()?(direction==MovementDirection::Up?tr("상승"):direction==MovementDirection::Down?tr("하락"):tr("보합")):QString{},direction});
    }}panel->setPublicIndicators(values);
}

} // namespace keefi
