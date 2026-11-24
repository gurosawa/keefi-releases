#include "keefi/LocalWindow.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/MarketFlowHistory.h"
#include "keefi/MarketFlowPresentation.h"
#include "keefi/InfoButton.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/Vault.h"
#include "keefi/PortfolioController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/LocalComboBox.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/AssetListFilter.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/WorldMarkets.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/MarketBreadthMonitor.h"
#include "keefi/RapidMoveMonitor.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/PublicCoinIdentity.h"
#include "keefi/RankingFuturesFeed.h"
#include "keefi/RankingStudyRunner.h"
#include "keefi/StockChartSource.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/TossMarketView.h"
#include "keefi/OfficialNoticesView.h"
#include "keefi/EarningsCalendarView.h"
#include "keefi/MarketOverviewView.h"
#include "keefi/OfficialCalendarView.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketMap.h"
#include "keefi/Spotlight.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/ExploreWatchlist.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHash>
#include <QLabel>
#include <QProgressBar>
#include <QLineEdit>
#include <QSettings>
#include "keefi/MovementColorPolicy.h"
#include "keefi/ExploreMetricCard.h"
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTimer>
#include <QStackedWidget>
#include <QTableWidget>
#include <QScrollArea>
#include <QSplitter>
#include <QTimeZone>
#include <QDateTime>
#include "keefi/AssetValuation.h"
#include "keefi/MarketOverview.h"
#include "keefi/EarningsCalendar.h"
#include "keefi/MovementDirection.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/OfficialNotices.h"
#include "keefi/PortfolioHistory.h"
#include "keefi/PortfolioDisplayFx.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/RankingStudies.h"
#include <utility>
#include <algorithm>
#include <cmath>
#include <functional>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QByteArray>
#include <QComboBox>
#include <QEvent>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

namespace keefi {
using local_window_detail::hasPortfolioFigure;
using local_window_detail::textLabel;
using local_window_detail::button;

namespace {
// Text tabs over a hidden combo box: the combo stays the single source of truth
// for the existing explore logic, the tabs are only its visible face (mockup).
QWidget* textTabsFor(QComboBox* combo, const QList<QPair<QString, UsabilityText>>& tabs, const QString& name, FontRole role, bool underline, QWidget* parent) {
    auto* bar = new QWidget(parent);
    bar->setObjectName(name);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(underline ? 22 : 18);
    auto* group = new QButtonGroup(bar);
    group->setExclusive(true);
    for (const auto& [data, label] : tabs) {
        auto* button = new QPushButton(usabilityText(label), bar);
        button->setObjectName(name + "_" + data);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFont(KeefiFonts::font(role));
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("tabData", data);
        setThemeStyle(button, underline
            ? "QPushButton{border:0;border-radius:0;background:transparent;color:${text3};padding:4px 0 6px 0;min-height:0;} QPushButton:checked{color:${text};border-bottom:2px solid ${accent};} QPushButton:hover,QPushButton:focus{color:${text};}"
            : "QPushButton{border:0;border-radius:0;background:transparent;color:${text3};padding:4px 0;min-height:0;} QPushButton:checked{color:${text};font-weight:700;} QPushButton:hover,QPushButton:focus{color:${text};}");
        group->addButton(button);
        row->addWidget(button);
        QObject::connect(button, &QPushButton::toggled, combo, [combo, data](bool selected) {
            if (!selected) return;
            const auto index = combo->findData(data);
            if (index >= 0) combo->setCurrentIndex(index);
        });
    }
    const auto sync = [bar, combo] {
        for (auto* button : bar->findChildren<QPushButton*>()) {
            const QSignalBlocker block(button);
            const auto data = button->property("tabData").toString();
            button->setVisible(combo->findData(data) >= 0);
            button->setChecked(combo->currentData().toString() == data);
        }
    };
    QObject::connect(combo, &QComboBox::currentIndexChanged, bar, sync);
    QObject::connect(combo->model(), &QAbstractItemModel::rowsInserted, bar, sync);
    sync();
    return bar;
}
} // namespace
namespace {
// A success line at the window foot clears itself; errors keep using setText and stay.
// Calls back once a watched widget has a new size; layout choices read the settled size.
class ResizeHook final : public QObject {
  public:
    ResizeHook(QWidget* watched, std::function<void()> callback) : QObject(watched), callback_(std::move(callback)) { watched->installEventFilter(this); }
  protected:
    bool eventFilter(QObject*, QEvent* event) override { if (event->type() == QEvent::Resize) QTimer::singleShot(0, this, callback_); return false; }
  private:
    std::function<void()> callback_;
};

} // namespace
void LocalWindow::setPublicMarketCatalog(PublicMarketCatalog* catalog, bool autoLoad) {
    auto* pages = findChild<QTabWidget*>("portfolioPages");
    if (markets_ || !catalog || !pages) return;
    markets_ = catalog;
    syncPublicMarketMetadataDemand();
    if(chartWorkspace_)chartWorkspace_->setPublicNameSources(markets_,board_,chartInstruments_);
    auto* page = new QWidget(pages);
    page->setObjectName("explorePage");
    // Content follows the window and is centred only beyond ExploreMaximumWidth.
    auto* shell = new QHBoxLayout(page);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    auto* content = new QWidget(page);
    content->setObjectName("exploreContent");
    content->setMaximumWidth(ExploreMaximumWidth);
    shell->addStretch(1);
    shell->addWidget(content, 100);
    shell->addStretch(1);
    auto* layout = new QVBoxLayout(content);
    layout->setProperty("originalExploreMargins",QVariant::fromValue(layout->contentsMargins()));
    layout->setProperty("originalExploreSpacing",layout->spacing());
    // One row of controls; prices refresh on their own, so there is no reload button.
    auto* filters = new QHBoxLayout;
    filters->setObjectName("exploreFilters");
    filters->setSpacing(10);
    auto* view = new SegmentedComboBox(page);
    view->setObjectName("exploreView");
    view->addItem(usabilityText(UsabilityText::ExploreViewMap), "map");
    view->addItem(usabilityText(UsabilityText::ExploreViewRank), "list");
    view->setMinimumWidth(view->sizeHint().width());
    view->hide(); // Model only; the text tabs below are its visible face.
    filters->addWidget(view, 0, Qt::AlignLeft | Qt::AlignVCenter);
    auto* viewTabs = textTabsFor(view, {{QStringLiteral("flow"), UsabilityText::ExploreViewFlow}, {QStringLiteral("list"), UsabilityText::ExploreViewRank}, {QStringLiteral("map"), UsabilityText::ExploreViewMap}}, QStringLiteral("exploreViewTabs"), FontRole::BodyBold15, true, page);
    viewTabs->setMinimumHeight(44);
    filters->addWidget(viewTabs, 0, Qt::AlignLeft | Qt::AlignVCenter);
    filters->addStretch(1);
    for(const auto& choice:QList<QPair<QString,QString>>{{"breadth",tr("강세 비율")},{"relative",tr("BTC 대비")}}){
        auto* toggle=new QCheckBox(choice.second,page);toggle->setObjectName("exploreObservation_"+choice.first);toggle->setMinimumHeight(44);
        toggle->setFont(KeefiFonts::font(FontRole::Caption12));toggle->setChecked(chartSettings_&&chartSettings_->value("explore/observations/"+choice.first,false).toBool());
        toggle->hide(); // State model; the visible controls live in 표시 설정.
        connect(toggle,&QCheckBox::toggled,this,[this,key=choice.first](bool enabled){if(chartSettings_)chartSettings_->setValue("explore/observations/"+key,enabled);refreshPublicObservations();});
    }
    rankingStudyRunner_=new RankingStudyRunner(this);connect(rankingStudyRunner_,&RankingStudyRunner::changed,this,&LocalWindow::refreshRankingStudies);
    auto* rankStudy=new LocalComboBox(page);rankStudy->setObjectName("exploreRankingStudy");rankStudy->setAccessibleName(tr("순위 상태 열"));
    setThemeStyle(rankStudy,rankStudy->property("keefiThemeTemplate").toString()+QStringLiteral("\nQComboBox { min-height: 28px; }"));
    rankStudy->setMinimumHeight(44);rankStudy->setMinimumWidth(130);rankStudy->setFont(KeefiFonts::font(FontRole::Caption12));
    for(auto field:{RankingStudy::None,RankingStudy::Compression,RankingStudy::OpenInterest,RankingStudy::SpotFutures,RankingStudy::Pullback,RankingStudy::Volume,RankingStudy::Drawdown})rankStudy->addItem(rankingStudyLabel(field),rankingStudyKey(field));
    if(chartSettings_)rankStudy->setCurrentIndex(qMax(0,rankStudy->findData(chartSettings_->value("explore/ranking/study","none"))));
    filters->addWidget(rankStudy);rankStudy->hide();
    auto* rankInfo=new InfoButton(page);rankInfo->setObjectName("exploreRankingInfo");rankInfo->setFixedSize(44,44);filters->addWidget(rankInfo);rankInfo->hide();
    auto* rankBasis=new QLabel(tr("상태 · 1시간봉 · 완료 후 갱신"),page);rankBasis->setObjectName("exploreRankingBasis");rankBasis->setFont(KeefiFonts::font(FontRole::Caption12));filters->addWidget(rankBasis);rankBasis->hide();
    connect(rankStudy,&QComboBox::currentIndexChanged,this,[this,rankStudy]{if(chartSettings_)chartSettings_->setValue("explore/ranking/study",rankStudy->currentData());if(rankingStudyRunner_)rankingStudyRunner_->clear();refreshRankingStudies();});
    auto* rankTimer=new QTimer(page);rankTimer->setInterval(1000);connect(rankTimer,&QTimer::timeout,this,&LocalWindow::refreshRankingStudies);rankTimer->start();
    auto* section = new SegmentedComboBox(page);
    section->setObjectName("exploreMarketSection");
    section->addItem(usabilityText(UsabilityText::ExploreMarketAll), "all");
    section->addItem(usabilityText(UsabilityText::ExploreMarketCoin), "coin");
    section->addItem(usabilityText(UsabilityText::ExploreMarketKorea), "kr");
    section->addItem(usabilityText(UsabilityText::ExploreMarketUs), "us");
    section->setMinimumWidth(section->sizeHint().width());
    section->hide();
    filters->addWidget(section, 0, Qt::AlignLeft | Qt::AlignVCenter);
    auto* marketTabs = textTabsFor(section, {{QStringLiteral("all"), UsabilityText::ExploreMarketAll}, {QStringLiteral("coin"), UsabilityText::ExploreMarketCoin},
        {QStringLiteral("kr"), UsabilityText::ExploreMarketKorea}, {QStringLiteral("us"), UsabilityText::ExploreMarketUs}}, QStringLiteral("exploreMarketTabs"), FontRole::Secondary14, false, page);
    filters->addWidget(marketTabs, 0, Qt::AlignVCenter);
    auto* search = new QLineEdit(page);
    search->setObjectName("exploreSearch");
    search->setFixedWidth(200);
    search->setPlaceholderText(usabilityText(UsabilityText::ExploreCoinSearch));
    search->setClearButtonEnabled(true);
    search->setMaxLength(80);
    search->setAccessibleName(tr("업비트 KRW 종목 검색"));
    auto searchPolicy = search->sizePolicy();
    searchPolicy.setRetainSizeWhenHidden(true);
    search->setSizePolicy(searchPolicy);
    filters->addWidget(search, 0);
    auto* status = textLabel({}, page, "exploreStatus");
    status->hide(); // Full status text (with the KST 09:00 basis) stays for tooltips and records.
    filters->addWidget(status);
    layout->addLayout(filters);
    auto* breadthRow=new QWidget(page);breadthRow->setObjectName("exploreBreadthRow");auto* breadthLayout=new QHBoxLayout(breadthRow);breadthLayout->setContentsMargins(0,0,0,0);
    auto* breadthText=textLabel({},breadthRow,"exploreBreadthText");breadthText->setFont(KeefiFonts::font(FontRole::Caption12));breadthText->setMinimumWidth(0);breadthLayout->addWidget(breadthText,1);
    auto* breadthInfo=new InfoButton(breadthRow);breadthInfo->setObjectName("exploreBreadthInfo");breadthLayout->addWidget(breadthInfo);breadthRow->hide(); // Results are projected into the fixed market-summary line, not above the first card.
    marketBreadth_=new MarketBreadthMonitor(this);connect(marketBreadth_,&MarketBreadthMonitor::changed,this,&LocalWindow::refreshPublicObservations);
    // Search results: the full Upbit KRW table, shown only while a query is typed.
    exploreTable_ = new QTableWidget(0, 5, page);
    exploreTable_->setObjectName("exploreMarkets");
    exploreTable_->setHorizontalHeaderLabels({tr("종목"), tr("코드"), tr("현재가 KRW"), tr("전일 대비"), tr("내 보유")});
    exploreTable_->setAccessibleName(tr("업비트 KRW 종목 목록"));
    exploreTable_->verticalHeader()->hide();
    exploreTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    exploreTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    exploreTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    exploreTable_->setSortingEnabled(false); // Name/code order only; no ranking by price movement.
    exploreTable_->setShowGrid(false);
    exploreTable_->setItemDelegate(new WorkbenchRowDelegate(exploreTable_));
    exploreTable_->setMouseTracking(true);
    exploreTable_->setColumnHidden(1, true); // The ticker is in the avatar and the detail card.
    exploreTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* headerView = exploreTable_->horizontalHeader();
    headerView->setSectionResizeMode(QHeaderView::ResizeToContents);
    headerView->setSectionResizeMode(0, QHeaderView::Stretch);
    exploreDetail_ = new AssetDetailPanel(page);
    exploreDetail_->setObjectName("exploreDetail");
    exploreListDetail_ = new ListDetailView(exploreTable_, exploreDetail_, page);
    exploreListDetail_->setObjectName("exploreListDetail");
    // 0 map · 1 Spotlight cards · 2 search results.
    exploreStack_ = new QStackedWidget(page);
    exploreStack_->setObjectName("exploreStack");
    auto* mapPage = new QWidget(exploreStack_);
    auto* mapLayout = new QVBoxLayout(mapPage);
    mapLayout->setContentsMargins(0, 0, 0, 0);
    mapLayout->setSpacing(0);
    auto* mapCard = new QFrame(mapPage);
    mapCard->setObjectName("marketMapCard");
    mapCard->setProperty("card", false);
    auto* mapCardLayout = new QVBoxLayout(mapCard);
    mapCardLayout->setContentsMargins(4, 4, 4, 4);
    marketMap_ = new MarketMapWidget(mapCard);
    mapCard->setFocusProxy(marketMap_);
    mapCardLayout->addWidget(marketMap_);
    mapDetail_ = new AssetDetailPanel(mapPage);
    mapDetail_->setObjectName("marketMapDetail");
    mapDetail_->setFixedWidth(340);
    mapListDetail_ = new ListDetailView(mapCard,mapDetail_,mapPage);
    mapListDetail_->setObjectName("marketMapListDetail");
    mapListDetail_->findChild<QSplitter*>("assetListDetailSplitter")->setHandleWidth(14);
    mapLayout->addWidget(mapListDetail_);
    exploreStack_->addWidget(mapPage);
    auto* spotPage = new QWidget(exploreStack_);
    spotPage->setObjectName("spotlightPage");
    auto* spotLayout = new QVBoxLayout(spotPage);
    spotLayout->setContentsMargins(0, 0, 0, 0);
    spotLayout->setSpacing(0);
    auto* spotLeft = new QWidget(spotPage);
    spotLeft->setObjectName("spotlightBody");
    spotlightColumns_ = new QBoxLayout(QBoxLayout::LeftToRight, spotLeft);
    spotlightColumns_->setContentsMargins(0, 0, 0, 0);
    spotlightColumns_->setSpacing(16);
    auto* spotScroll = new QScrollArea(spotPage);
    spotScroll->setObjectName("spotlightScroll");
    spotScroll->setWidgetResizable(true);
    spotScroll->setFrameShape(QFrame::NoFrame);
    spotScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Let the layout divide the actual remaining space; card contents scroll
    // independently instead of forcing the watchlist over their footer.
    spotScroll->setMinimumHeight(100);
    auto* spotCards = new QWidget(spotScroll);
    spotCards->setObjectName("spotlightCards");
    spotlightGrid_ = new QGridLayout(spotCards);
    spotlightGrid_->setContentsMargins(0, 0, 0, 0);
    // Columns breathe like the watchlist gap; stacked markets already part with their top rule.
    spotlightGrid_->setHorizontalSpacing(16);
    spotlightGrid_->setVerticalSpacing(12);
    for (const auto& [key, title] : {std::pair{QStringLiteral("coin"), usabilityText(UsabilityText::ExploreMarketCoin)}, std::pair{QStringLiteral("kr"), usabilityText(UsabilityText::ExploreMarketKorea)},
                                    std::pair{QStringLiteral("us"), usabilityText(UsabilityText::ExploreMarketUs)}}) {
        auto* card = new SpotlightCard(key, title, spotCards);
        spotlightCards_.append(card);
        if (key == "coin") connect(card, &SpotlightCard::modeChanged, card, [this, card] {
            card->setStatus(card->mode()=="cap" ? tr("CoinGecko") :
                card->coinVenue()=="binance" ? tr("최근 24시간") : tr("09시 KST 기준"));
        });
        connect(card,&SpotlightCard::coinVenueChanged,this,[this]{refreshSpotlight();refreshRankingStudies();});
        connect(card,&SpotlightCard::evidenceOrderChanged,this,&LocalWindow::queueLegacyExploreEvidence);
        connect(card,&SpotlightCard::evidenceOrderChanged,this,[this]{if(rankingQueued_)return;rankingQueued_=true;QTimer::singleShot(0,this,[this]{rankingQueued_=false;refreshRankingStudies();});});
        connect(card, &SpotlightCard::itemSelected, this, [this,card](const QString& sectionKey, const QString& id) {
            for (auto* other : spotlightCards_) if (other->key() != sectionKey) other->setSelected({});
            selectMarketMetric(sectionKey, id);
            openPublicChart(sectionKey=="coin"&&card->mode()!="cap"?card->coinVenue():sectionKey,id);
        });
        connect(card, &SpotlightCard::openRequested, this, [view, section](const QString& sectionKey) {
            section->setCurrentIndex(qMax(0, section->findData(sectionKey)));
            view->setCurrentIndex(0);
        });
    }
    spotScroll->setWidget(spotCards);
    spotlightColumns_->addWidget(spotScroll, 1);
    watchlist_ = new ExploreWatchlist(spotLeft);
    spotlightColumns_->addWidget(watchlist_);
    connect(watchlist_, &ExploreWatchlist::favouritesChanged, this, [this] {
        refreshSpotlight();
        refreshWatchlist();
    });
    connect(watchlist_, &ExploreWatchlist::sectionChanged, this, [this] { refreshWatchlist(); });
    // Reparenting the shared list can hide and show it during one tab switch.
    // The route handler can already have refreshed it before the event loop resumes.
    watchlistVisibilityTimer_ = new QTimer(watchlist_);
    watchlistVisibilityTimer_->setSingleShot(true);
    connect(watchlist_, &ExploreWatchlist::visibilityChanged, this, [this] { watchlistVisibilityTimer_->start(0); });
    connect(watchlistVisibilityTimer_, &QTimer::timeout, this, [this] { refreshWatchlist(); });
    connect(watchlist_, &ExploreWatchlist::evidenceOrderChanged, this, &LocalWindow::queueLegacyExploreEvidence);
    connect(watchlist_, &ExploreWatchlist::itemSelected, this, [this](const QString& key, const QString& id) {
        for (auto* card : spotlightCards_) card->setSelected(card->key() == key ? id : QString{});
        selectMarketMetric(key, id);
        openPublicChart(key, id);
    });
    for (auto* card : spotlightCards_) connect(card, &SpotlightCard::favouriteToggled, this, [this](const QString& key, const QString& id) {
        watchlist_->setSection(key); watchlist_->toggle(key, id);
    });
    // At the minimum window height the cards and saved list remain reachable
    // by scrolling, rather than forcing the entire window above its minimum.
    auto* spotListScroll = new QScrollArea(spotPage);
    spotListScroll->setObjectName("spotlightListScroll");
    spotListScroll->setWidgetResizable(true);
    spotListScroll->setFrameShape(QFrame::NoFrame);
    spotListScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    spotListScroll->setWidget(spotLeft);
    spotListScroll->setFocusProxy(watchlist_);
    spotLayout->addWidget(spotListScroll);
    // Returning from another tab or view settles the size later than the window resize.
    new ResizeHook(spotListScroll->viewport(), [this] { layoutSpotlight(); });
    exploreStack_->addWidget(spotPage);
    exploreStack_->addWidget(exploreListDetail_);
    auto* panels=new QWidget(content);panels->setObjectName("explorePanels");
    auto* panelsLayout=new QHBoxLayout(panels);panelsLayout->setContentsMargins(0,0,0,0);panelsLayout->setSpacing(16);
    panelsLayout->addWidget(exploreStack_,1);
    exploreCommonSidebar_=new QWidget(panels);exploreCommonSidebar_->setObjectName("exploreCommonSidebar");
    auto* common=new QVBoxLayout(exploreCommonSidebar_);common->setContentsMargins(0,0,0,0);common->setSpacing(16);
    exploreCoinSummary_=new ExploreCoinSummary(exploreCommonSidebar_);common->addWidget(exploreCoinSummary_,3);
    spotlightColumns_->removeWidget(watchlist_);common->addWidget(watchlist_,2);watchlist_->setCompact(true);
    panelsLayout->addWidget(exploreCommonSidebar_);exploreCommonSidebar_->setFixedWidth(400);
    connect(exploreCoinSummary_,&ExploreCoinSummary::coinSelected,this,[this](const QString& asset){selectMarketMetric("coin",asset);refreshDashboard();});
    connect(exploreCoinSummary_,&ExploreCoinSummary::chartRequested,this,[this]{if(marketMetricsView_)openPublicChart("coin",marketMetricsView_->asset());});
    connect(watchlist_,&ExploreWatchlist::favouritesChanged,this,&LocalWindow::refreshDashboard);
    layout->addWidget(panels, 1);
    auto* panelsScroll=new QScrollArea(content);panelsScroll->setObjectName("explorePanelsScroll");
    panelsScroll->setWidgetResizable(true);panelsScroll->setFrameShape(QFrame::NoFrame);
    panelsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(panelsScroll,1);panelsScroll->hide();
    exploreEvidenceScroll_=new QScrollArea(page);exploreEvidenceScroll_->setObjectName("exploreBoardEvidenceScroll");
    exploreEvidenceScroll_->setWidgetResizable(true);exploreEvidenceScroll_->setFrameShape(QFrame::NoFrame);
    exploreEvidenceScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    exploreBoardEvidence_=new ExploreEvidenceFooter;exploreBoardEvidence_->setObjectName("exploreBoardEvidenceFooter");
    exploreEvidenceScroll_->setWidget(exploreBoardEvidence_);layout->addWidget(exploreEvidenceScroll_);exploreEvidenceScroll_->hide();
    connect(exploreStack_,&QStackedWidget::currentChanged,this,&LocalWindow::queueLegacyExploreEvidence);
    const auto syncStack = [this, view, search, marketTabs] {
        const bool map = view->currentData() == "map", flow = view->currentData() == "flow";
        search->setVisible(!map && !flow); marketTabs->setVisible(map);
        const int route = flow ? 3 : map ? 0 : search->text().trimmed().isEmpty() ? 1 : 2;
        const bool routeChanged = route != exploreStack_->currentIndex();
        exploreStack_->setCurrentIndex(route);
        if(exploreCommonSidebar_)exploreCommonSidebar_->setVisible(flow||(!map&&search->text().trimmed().isEmpty()));
        if(exploreSummaryHeader_)exploreSummaryHeader_->setVisible(flow);
        if (!routeChanged) return;
        syncMarketFlowBackground();
        syncExplorePage();
        refreshWatchlist();
        refreshPublicObservations();
    };
    connect(view, &QComboBox::currentIndexChanged, this, [this, syncStack] {
        syncStack();
        queueLegacyExploreEvidence();
        // Reuse recent public boards as the user switches between Explore views.
        // The visible page timer owns their regular refresh.
        if (board_ && !board_->marketBoardBusy() &&
            QDateTime::currentMSecsSinceEpoch() - qMin(board_->receivedAtMs("coin"), board_->receivedAtMs("kr")) >= 30000)
            board_->refresh();
        QTimer::singleShot(0, this, [this] { layoutSpotlight(); });
    });
    search->hide();
    connect(section, &QComboBox::currentIndexChanged, this, [this, section] {
        marketMap_->setFilter(section->currentData().toString());
        queueLegacyExploreEvidence();
    });
    connect(marketMap_, &MarketMapWidget::tileSelected, this, &LocalWindow::showMapSelection);
    connect(marketMap_, &MarketMapWidget::tileSelected, this, &LocalWindow::selectMarketMetric);
    connect(marketMap_, &MarketMapWidget::tileActivated, this, [this]{if(mapListDetail_)mapListDetail_->showDetail();});
    connect(marketMap_, &MarketMapWidget::tileSelected, this, [this]{refreshWatchlist();});
    connect(marketMap_, &MarketMapWidget::actionRequested, this, [this](const QString&) {
        if (!vault_.exists()) createVault_->click();
        else if (!vault_.isUnlocked()) unlockVault_->click();
        else openAccountDialog(false);
    });
    for (auto* detail : {mapDetail_, exploreDetail_})
        connect(detail, &AssetDetailPanel::connectionsRequested, this, [this, pages] { pages->setCurrentWidget(connectionsPage_); });
    connect(mapDetail_,&AssetDetailPanel::backRequested,this,[this]{refreshWatchlist();queueLegacyExploreEvidence();});
    for (auto* detail : {mapDetail_, exploreDetail_}) {
        detail->enableExplorationControls();
        connectPublicChartLink(detail);
        connect(detail, &AssetDetailPanel::favouriteRequested, this, [this](const QString& key, const QString& id) {
            watchlist_->setSection(key); watchlist_->toggle(key, id);
            showMapSelection(); showExploreDetail();
        });
    }
    connect(marketMap_, &MarketMapWidget::sectionRequested, this, [section](const QString& key) {
        section->setCurrentIndex(qMax(0, section->findData(key)));
    });
    boardTimer_ = new QTimer(this);
    boardTimer_->setInterval(30000);
    connect(boardTimer_, &QTimer::timeout, this, [this] {
        if (board_ && exploreStack_->isVisible() && !isMinimized()) board_->refresh();
    });
    boardTimer_->start();
    exploreTable_->setToolTip(tr("공개 시세는 금고·계좌 없이 조회하며 잠가도 유지합니다. 내 보유는 금고를 연 경우에만 같은 티커 기준으로 표시합니다. 이 화면에서 주문하지 않습니다."));
    for (int column : {2, 3})
        if (auto* head = exploreTable_->horizontalHeaderItem(column)) head->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pages->insertTab(0, page, tr("탐색"));
    pages->setCurrentIndex(0);
    syncHeaderTabs();
    connect(pages,&QTabWidget::currentChanged,this,&LocalWindow::syncChartTab,Qt::UniqueConnection);
    connect(catalog, &PublicMarketCatalog::changed, this, [this] {
        refreshExplore();
        applyPortfolioFilter(true);
        refreshMarketMap();
        refreshSpotlight();
        refreshWatchlist();
        refreshChartWatchlists();
        refreshPublicObservations();
        refreshAssetShare();
    });
    connect(search, &QLineEdit::textChanged, this, [this, syncStack](const QString& text) {
        const auto query = text.trimmed();
        const bool rebuild = searchRowsDirty_;
        syncStack();
        if (!rebuild && exploreStack_->currentIndex() == 2) filterExploreRows(query);
        queueLegacyExploreEvidence();
    });
    connect(exploreTable_, &QTableWidget::currentCellChanged, this, &LocalWindow::showExploreDetail);
    connect(exploreTable_, &QTableWidget::currentCellChanged, this, [this]{refreshWatchlist();});
    connect(exploreTable_, &QTableWidget::itemClicked, this, [this] {
        const auto* cell=exploreTable_->item(exploreTable_->currentRow(),0);
        if (!cell || !markets_) return;
        for (const auto& row : markets_->rows()) if (row.market==cell->data(Qt::UserRole).toString()) {selectMarketMetric("coin",row.base);break;}
    });
    const auto showSearchDetail = [this] {
        exploreListDetail_->showDetail();
        // An already-current row emits no selection change on activation. The
        // detail's visibility now determines its own public history demand.
        if (marketHistory_) refreshWatchlist();
    };
    connect(exploreTable_, &QTableWidget::itemClicked, this, showSearchDetail);
    connect(exploreTable_, &QTableWidget::itemActivated, this, showSearchDetail);
    connect(exploreDetail_, &AssetDetailPanel::backRequested, this, [this] {
        if (marketHistory_) refreshWatchlist();
    });
    layoutSpotlight();
    refreshExplore();
    refreshMarketMap();
    refreshSpotlight();
    if (autoLoad) catalog->refresh();
}

void LocalWindow::setPublicEquityBoard(PublicEquityBoard* board) {
    if (board_ || !board) return;
    board_ = board;
    if(chartWorkspace_)chartWorkspace_->setPublicNameSources(markets_,board_,chartInstruments_);
    connect(board, &PublicEquityBoard::changed, this, &LocalWindow::refreshMarketMap);
    connect(board, &PublicEquityBoard::changed, this, &LocalWindow::refreshSpotlight);
    connect(board, &PublicEquityBoard::changed, this, [this] { refreshWatchlist(); });
    connect(board, &PublicEquityBoard::changed, this, &LocalWindow::refreshPublicObservations);
    refreshMarketMap();
    refreshSpotlight();
}

void LocalWindow::setPublicMarketHistory(PublicMarketHistory* history) {
    if (marketHistory_ || !history) return;
    marketHistory_ = history;
    connect(history, &PublicMarketHistory::changed, this, [this] {
        refreshWatchlist();
        if (mapDetail_ && mapDetail_->isVisible()) showMapSelection();
        if (exploreDetail_ && exploreDetail_->isVisible()) showExploreDetail();
        refreshDashboard();
    });
    refreshWatchlist();
}



void LocalWindow::syncExplorePage() {
    if (!exploreStack_) return;
    const bool visible = exploreStack_->isVisible() && !isMinimized();
    const int route = exploreStack_->currentIndex();
    if (marketMetricsView_) {
        const bool mirror = !windowShownOnce_ || (visible && (route == 1 || route == 3));
        marketMetricsView_->setMirroredPresentation(mirror);
        if (mirror) syncMarketFlowPrivateContext();
    }
    const bool mapVisible = visible && route == 0 && marketMap_;
    if (mapVisible && (mapSectionsDirty_ || !marketMapSubscribed_)) refreshMarketMap();
    if (!mapVisible && marketMapSubscribed_) {
        if (auto* stocks = findChild<TossMarketView*>()) stocks->setMapSymbols({});
        marketMapSubscribed_ = false;
    }
    if (!exploreStack_->isVisible() || isMinimized()) return;
    if (exploreLayoutPending_) layoutSpotlight();
    if (rankingCardsDirty_ && route == 1) refreshSpotlight();
    if (searchRowsDirty_ && exploreStack_->currentIndex() == 2) refreshExplore();
    queueLegacyExploreEvidence();
}

void LocalWindow::refreshMarketMap() {
    if (!marketMap_ || !markets_) return;
    if (windowShownOnce_ && (!exploreStack_->isVisible() || exploreStack_->currentIndex() != 0 || isMinimized())) {
        mapSectionsDirty_ = true;
        return;
    }
    mapSectionsDirty_ = false;
    QHash<QString, PublicMarketRow> upbit;
    for (const auto& row : markets_->rows()) upbit.insert(row.base, row);
    const auto age = [this](const QString& key) {
        const auto at = board_ ? board_->receivedAtMs(key) : 0;
        return at > 0 ? QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("HH:mm")+QStringLiteral(" KST (UTC+9)") : QString{};
    };
    QList<MarketMapSection> sections;
    // The global market-cap map uses CoinGecko; exchange rankings have their own prices and periods.
    MarketMapSection coins;
    coins.key = "coin"; coins.title = usabilityText(UsabilityText::ExploreMarketCoin); coins.status = tr("24시간");
    coins.source = tr("CoinGecko") + (age("coin").isEmpty() ? QString{} : tr(" · %1").arg(age("coin")));
    const auto coinItems = board_ ? board_->items("coin") : QList<PublicBoardItem>{};
    for (const auto& item : coinItems) {
        MarketMapTile tile;
        tile.id = item.symbol; tile.label = item.symbol; tile.name = item.name; tile.group = item.group; tile.weight = item.marketCap;
        const auto row = upbit.value(item.symbol);
        tile.name = samePublicCoin(row,item)&&!row.koreanName.isEmpty() ? row.koreanName : item.name;
        tile.price = priceAmount(item.price) + tr("원");
        tile.changePercent = item.changePercent;
        coins.tiles.append(tile);
    }
    coins.available = !coins.tiles.isEmpty();
    coins.lockedTitle = board_ && !board_->status("coin").isEmpty() ? board_->status("coin") : tr("시세를 받는 중이에요");
    coins.lockedText = tr("계좌 없이 공개 시세로 그려요.");
    sections.append(coins);
    const auto stockSection = [&](const QString& key, const QString& title, const QString& sourceName) {
        MarketMapSection section;
        section.key = key; section.title = title;
        const auto market = board_ ? board_->marketStatus(key) : QString{};
        section.status = !market.isEmpty() ? market : key == "kr" ? tr("정규장 09:00–15:30") : PublicEquityBoard::usSessionKst(QDateTime::currentDateTime());
        section.source = sourceName + (age(key).isEmpty() ? QString{} : tr(" · %1").arg(age(key)));
        const auto items = board_ ? board_->items(key) : QList<PublicBoardItem>{};
        for (const auto& item : items) {
            MarketMapTile tile;
            tile.id = item.symbol; tile.label = key == "kr" ? item.name : item.symbol; tile.name = item.name; tile.group = item.group;
            tile.price = key == "us" ? "$" + priceAmount(item.price) : priceAmount(item.price) + tr("원");
            tile.changePercent = item.changePercent; tile.weight = item.marketCap;
            section.tiles.append(tile);
        }
        section.available = !section.tiles.isEmpty();
        section.lockedTitle = board_ && !board_->status(key).isEmpty() ? board_->status(key) : tr("시세를 받는 중이에요");
        section.lockedText = tr("계좌 없이 공개 시세로 그려요. 응답이 늦으면 잠시 뒤 다시 시도해요.");
        return section;
    };
    sections.append(stockSection("kr", usabilityText(UsabilityText::ExploreMarketKorea), tr("네이버 증권")));
    sections.append(stockSection("us", usabilityText(UsabilityText::ExploreMarketUs), tr("Nasdaq")));
    marketMap_->setSections(sections);
    if (auto* stocks = findChild<TossMarketView*>()) {
        QStringList symbols;
        if (board_ && marketMap_->isVisible()) for (const auto& section : {QString("kr"), QString("us")})
            for (const auto& item : board_->items(section).mid(0,50)) symbols.append(item.symbol);
        stocks->setMapSymbols(symbols);
    }
    marketMapSubscribed_ = exploreStack_->isVisible() && exploreStack_->currentIndex() == 0 && !isMinimized();
    if (marketMap_->selectedId().isEmpty() && !coins.tiles.isEmpty()) marketMap_->select("coin", coins.tiles.first().id);
    showMapSelection();
}

void LocalWindow::setMarketMetrics(MarketMetrics* metrics,CoinbaseQuotes* coinbase,PublicQuoteStream* upbit,PublicQuoteStream* binance) {
    if (!metrics || marketMetricsView_ || !exploreStack_) return;
    if(coinbase){
        const auto now=QDateTime::currentMSecsSinceEpoch();const auto existing=coinbase->quotes(now).value("USDT");
        if(!existing.price.isEmpty())portfolioDisplayFx_.observeCoinbase(existing,now);
        connect(coinbase,&CoinbaseQuotes::quoteObserved,this,[this,coinbase](const QString& base){
            if(base!="USDT")return;
            const auto now=QDateTime::currentMSecsSinceEpoch();const auto quote=coinbase->quotes(now).value("USDT");
            if(quote.price.isEmpty())portfolioDisplayFx_.invalidate();else portfolioDisplayFx_.observeCoinbase(quote,now);
            refreshMoneyPresentation();
        });
    }
    auto* page = new QScrollArea(exploreStack_); page->setObjectName("marketFlowPage");
    page->setWidgetResizable(true);page->setFrameShape(QFrame::NoFrame);page->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body=new QWidget(page);auto* todayLayout=new QVBoxLayout(body);todayLayout->setContentsMargins(0,0,0,0);todayLayout->setSpacing(4);
    // Existing view models retain their source lifecycle and alert state, but no longer define the page layout.
    auto* context=new QWidget(page);context->setObjectName("marketFlowContext");context->hide();new QVBoxLayout(context);
    auto* selected=new QWidget(page);selected->setObjectName("marketFlowSelection");selected->hide();
    marketMetricsView_=new MarketMetricsView(metrics,upbit,binance,selected);marketMetricsView_->hide();marketMetricsView_->setMirroredPresentation(true);marketMetricsView_->setCoinbaseQuotes(coinbase);
    connect(marketMetricsView_,&MarketMetricsView::evidenceChanged,this,&LocalWindow::syncExploreEvidence);
    connect(marketMetricsView_,&MarketMetricsView::evidenceChanged,this,&LocalWindow::refreshDashboard);
    marketFlowProgress_=new QWidget(body);marketFlowProgress_->setObjectName("marketFlowProgress");
    marketFlowProgress_->hide(); // Keep diagnostic progress off the product layout.
    auto* progressLine=new QHBoxLayout(marketFlowProgress_);progressLine->setContentsMargins(0,0,0,0);progressLine->setSpacing(12);
    marketFlowProgressBar_=new QProgressBar(marketFlowProgress_);marketFlowProgressBar_->setObjectName("marketFlowProgressBar");marketFlowProgressBar_->setTextVisible(false);marketFlowProgressBar_->setFixedHeight(3);
    setThemeStyle(marketFlowProgressBar_,"QProgressBar{border:0;background:${rule};}QProgressBar::chunk{background:${accent};}");progressLine->addWidget(marketFlowProgressBar_,1);
    todayDashboard_=new ExploreTodayView(body);todayLayout->addWidget(todayDashboard_,1);
    exploreSummaryHeader_=todayDashboard_->takeSummaryHeader();
    if(auto* content=findChild<QWidget*>("exploreContent")){
        exploreSummaryHeader_->setParent(content);
        auto* contentLayout=qobject_cast<QVBoxLayout*>(content->layout());
        contentLayout->insertWidget(contentLayout->indexOf(findChild<QWidget*>("explorePanels")),exploreSummaryHeader_);
        exploreSummaryHeader_->show();
    }
    connect(todayDashboard_,&ExploreTodayView::vaultOpenRequested,this,[this]{if(vault_.exists())unlockVault_->click();else createVault_->click();});
    connect(todayDashboard_,&ExploreTodayView::alertSettingsRequested,this,&LocalWindow::openMarketAlertSettings);
    page->setWidget(body);exploreStack_->addWidget(page);
    connect(exploreStack_,&QStackedWidget::currentChanged,this,&LocalWindow::syncMarketFlowHistory);
    connect(marketMetricsView_,&MarketMetricsView::metricAlertToggled,this,[this](MarketFlowMetric metric,bool enabled){
        if(!chartAlerts_)return;chartAlerts_->setMarketMetricEnabled(metric,enabled);
        if(enabled&&marketFlowAlertSink_&&marketFlowAlertSink_->permission()==SystemNoticePermission::Unknown)marketFlowAlertSink_->requestAuthorization();
        syncMarketFlowAlerts();
    });
    connect(marketMetricsView_,&MarketMetricsView::snapshotReady,this,[this](const QString& asset,const ExchangePriceGap& gap,const ExchangePriceGap& coinbase,const PublicDerivativesMetrics& derivatives){
        // The selected coin panel reuses this public 김프 instead of computing another one.
        if(gap.available){qint64 newest=0;for(const auto& leg:gap.legs)newest=qMax(newest,leg.evidence.receivedAtMs);flowKoreaGaps_.insert(asset,{gap.percent,newest});}else flowKoreaGaps_.remove(asset);
        if(!chartAlerts_)return;const auto now=QDateTime::currentMSecsSinceEpoch();
        const auto observeGap=[&](MarketFlowMetric metric,const ExchangePriceGap& value){qint64 source=0,received=0;for(const auto& leg:value.legs){source=source?qMin(source,leg.evidence.sourceTimeMs):leg.evidence.sourceTimeMs;received=qMax(received,leg.evidence.receivedAtMs);}chartAlerts_->observeMarketMetric(asset,metric,value.available?marketFlowState(metric,value.percent):MarketFlowState::Unavailable,source,received);};
        observeGap(MarketFlowMetric::KoreaGap,gap);observeGap(MarketFlowMetric::CoinbaseGap,coinbase);
        chartAlerts_->observeMarketMetric(asset,MarketFlowMetric::Funding,marketFlowFundingState(derivatives,now),derivatives.fundingEvidence.sourceTimeMs,qMax(derivatives.fundingEvidence.receivedAtMs,derivatives.intervalEvidence.receivedAtMs));
        ContractFlow flow;flow.available=derivatives.flowAvailable;flow.priceDirection=derivatives.flowPriceDirection;flow.quantityDirection=derivatives.flowQuantityDirection;flow.fromMs=derivatives.flowFromMs;flow.toMs=derivatives.flowToMs;
        chartAlerts_->observeContractFlow(asset,flow,qMax(derivatives.openInterestEvidence.receivedAtMs,derivatives.fundingEvidence.receivedAtMs));
    });
    auto* flowTimer=new QTimer(page);flowTimer->setObjectName("marketFlowContextTimer");flowTimer->setInterval(1000);connect(flowTimer,&QTimer::timeout,this,[this]{syncMarketFlowBackground();syncMarketFlowHistory();syncMarketFlowPrivateContext();refreshDashboard();});flowTimer->start();
    syncMarketFlowAlerts();
    findChild<QComboBox*>("exploreView")->addItem(tr("오늘"), "flow");
    findChild<QComboBox*>("exploreView")->setCurrentIndex(findChild<QComboBox*>("exploreView")->findData("flow"));
    selectMarketMetric("coin", "BTC"); syncExploreEvidence();layoutSpotlight();refreshMoneyPresentation();
}
void LocalWindow::setWorldMarkets(WorldMarkets* model){
    if(worldMarkets_==model)return;
    if(worldMarkets_){disconnect(worldMarkets_,nullptr,this,nullptr);worldMarkets_->setActive(false);}
    worldMarkets_=model;
    // Display-only public reads. No alert, history or private sink is attached.
    if(model)connect(model,&WorldMarkets::changed,this,[this]{
        refreshDashboard();if(chartWorkspace_&&chartWorkspace_->isVisible())refreshChartMarketContext();});
    syncMarketFlowBackground();refreshDashboard();
}
void LocalWindow::setTodayBtcPremium(TodayBtcPremium* model){
    if(todayBtcPremium_==model)return;
    if(todayBtcPremium_){disconnect(todayBtcPremium_,nullptr,this,nullptr);todayBtcPremium_->setActive(false);}
    todayBtcPremium_=model;
    if(model){connect(model,&TodayBtcPremium::changed,this,&LocalWindow::refreshDashboard);model->setActive(isVisible()&&!isMinimized());}
    refreshDashboard();
}
void LocalWindow::setMarketFlowHistory(MarketFlowHistory* history){
    if(marketFlowHistory_==history)return;if(marketFlowHistory_){disconnect(marketFlowHistory_,nullptr,this,nullptr);marketFlowHistory_->setActive(false);}marketFlowHistory_=history;
    if(history)connect(history,&MarketFlowHistory::changed,this,[this]{if(marketMetricsView_&&marketFlowHistory_)marketMetricsView_->setPreviousGap(marketFlowHistory_->snapshot());});syncMarketFlowHistory();
}
void LocalWindow::syncMarketFlowBackground(){
    if(todayBtcPremium_)todayBtcPremium_->setActive(isVisible()&&!isMinimized());
    syncWorldMarketsDemand();
    if(!marketMetricsView_)return;const bool background=isVisible();const auto asset=marketMetricsView_->asset();
    auto* context=findChild<QWidget*>("marketFlowContext");
    auto* calendar=context?context->findChild<OfficialCalendarView*>():nullptr;auto* earnings=context?context->findChild<EarningsCalendarView*>():nullptr;
    auto* notices=context?context->findChild<OfficialNoticesView*>():nullptr;auto* overview=context?context->findChild<MarketOverviewView*>():nullptr;
    if(calendar)calendar->setBackgroundActive(background);if(earnings)earnings->setBackgroundActive(background);
    if(notices)notices->setBackgroundActive(background);if(overview)overview->setBackgroundActive(background);
    // Where 김프 is read: the Upbit KRW list and Binance spot list, once each has loaded.
    using Listing=MarketMetricsView::Listing;Listing upbitListing=Listing::Unknown,binanceListing=Listing::Unknown;
    if(markets_&&markets_->marketListAvailable()){upbitListing=markets_->verifiedMarkets().contains("KRW-"+asset)?Listing::Listed:Listing::Missing;}
    if(asset=="USDT")binanceListing=Listing::Listed; // The bridge itself; no USDT/USDT market is read.
    else if(chartInstruments_){if(background)chartInstruments_->ensure("binance");const auto list=chartInstruments_->instruments("binance");
        if(!list.isEmpty())binanceListing=std::any_of(list.cbegin(),list.cend(),[&](const auto& item){return item.symbol==asset+"USDT"&&item.tradable;})?Listing::Listed:Listing::Missing;}
    marketMetricsView_->setListing(upbitListing,binanceListing);marketMetricsView_->setBackgroundActive(background);
    int done=0,total=0;const auto progress=marketMetricsView_->progress();done+=progress.first;total+=progress.second;
    for(const bool settled:{calendar?calendar->settled():true,earnings?earnings->settled():true,notices?notices->settled():true,overview?overview->settled():true}){++total;done+=settled;}
    if(progress.second>0&&done>=total)marketFlowLoadedAsset_=asset;
    // Later periodic refreshes stay quiet; a new coin shows the line again.
    const bool loading=background&&progress.second>0&&done<total&&marketFlowLoadedAsset_!=asset;
    if(marketFlowProgressBar_){marketFlowProgressBar_->setRange(0,total);marketFlowProgressBar_->setValue(done);marketFlowProgressBar_->setAccessibleName(usabilityText(UsabilityText::FlowLoadingProgress).arg(done).arg(total));}
    Q_UNUSED(loading);
    if(marketFlowProgress_)marketFlowProgress_->hide();
}
void LocalWindow::syncMarketFlowHistory(){
    if(!marketFlowHistory_||!marketMetricsView_)return;const bool active=isVisible()&&!isMinimized();
    marketFlowHistory_->setAsset(marketMetricsView_->asset());
    if(chartInstruments_){if(active)chartInstruments_->ensure("binance");marketFlowHistory_->setInstruments(chartInstruments_->instruments("binance"));}
    marketFlowHistory_->setActive(active);marketMetricsView_->setPreviousGap(marketFlowHistory_->snapshot());
}
void LocalWindow::syncMarketFlowAlerts(){
    if(!chartAlerts_||!marketMetricsView_)return;bool active=false;
    for(const auto metric:{MarketFlowMetric::KoreaGap,MarketFlowMetric::Funding,MarketFlowMetric::CoinbaseGap,MarketFlowMetric::OpenInterest}){const bool enabled=chartAlerts_->marketMetricEnabled(metric);active|=enabled;marketMetricsView_->setMetricAlertEnabled(metric,enabled);}
    const auto state=chartAlerts_->marketMetricState();if(chartAlertSettings_&&state!=marketMetricPublicState_){chartAlertSettings_->setValue("market/flow-alerts/v1",state);marketMetricPublicState_=state;}
    marketMetricsView_->setAlertDemandActive(active);
}
void LocalWindow::syncMarketFlowPrivateContext(){
    const bool readable=portfolio_&&!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_);
    if (readable && windowShownOnce_ && (!exploreStack_ || !exploreStack_->isVisible() || isMinimized() ||
        (exploreStack_->currentIndex() != 1 && exploreStack_->currentIndex() != 3))) return;
    if(readable&&deferPortfolioLoadingProjection()){portfolioLoadingProjectionDirty_=true;return;}
    bool hasFutures=false;QString coinShare,usShare;const auto now=QDateTime::currentMSecsSinceEpoch();
    if(readable){
        const auto states=portfolio_->connections();
        for(const auto& snapshot:portfolio_->snapshots()){
            if(!snapshot.balanceComplete||snapshot.timeMs<=0||snapshot.timeMs>now||now-snapshot.timeMs>180000)continue;
            bool current=false;for(const auto& state:states)if(!state.shared&&state.state=="complete"&&state.accountScope==snapshot.accountScope&&state.receivedAtMs>0&&state.receivedAtMs<=now&&now-state.receivedAtMs<=180000){current=true;break;}if(!current)continue;
            if(snapshot.market!="usdm_futures"&&snapshot.market!="okx_unified"&&snapshot.market!="okx_futures"&&snapshot.market!="hyperliquid_perps")continue;
            for(const auto& component:snapshot.components){auto quantity=component.quantity;if(quantity.startsWith('-'))quantity.remove(0,1);if(component.kind==AssetComponentKind::PositionNotional&&decimal::positive(quantity)){hasFutures=true;break;}}
        }
        if(portfolioView_.summary.complete&&portfolioView_.asOfTimeMs==0){
            const auto total=portfolioAllocationTotal(portfolioView_.groups);QString coins="0",us="0";bool classified=true;
            for(const auto& group:portfolioView_.groups)for(const auto& holding:group.holdings){
                if(holding.state!=PortfolioValueState::Fresh||holding.kind==AssetComponentKind::Liability||!decimal::positive(holding.value))continue;
                if(portfolioAssetClass(holding)=="coin")coins=decimal::add(coins,holding.value);
                else if(portfolioAssetClass(holding)=="stock"){
                    auto* broker=portfolio_->broker(holding.connectionId);const auto country=broker?broker->marketDataCountry(holding.asset):QString{};
                    if(country=="US")us=decimal::add(us,holding.value);else if(country!="KR")classified=false;
                }
            }
            if(classified&&decimal::positive(total)){coinShare=portfolioShareText(coins,total);usShare=portfolioShareText(us,total);}
        }
    }
    if(marketMetricsView_)marketMetricsView_->setPrivateContext(readable,hasFutures);
    if(auto* calendar=findChild<OfficialCalendarView*>())calendar->setPrivateContext(readable,coinShare,usShare);
}
namespace {
// Market flow right column, top to bottom: this week's calendar, notices, reference metric.
void placeFlowSection(QWidget* context, QWidget* section, int rank) {
    auto* layout = qobject_cast<QVBoxLayout*>(context->layout());
    if (!layout) return;
    section->setProperty("marketFlowRank", rank);
    int index = 0;
    for (; index < layout->count(); ++index) {
        auto* widget = layout->itemAt(index)->widget();
        if (!widget || widget->property("marketFlowRank").toInt() > rank) break;
    }
    layout->insertWidget(index, section);
}
} // namespace
void LocalWindow::setMarketOverview(MarketOverview* overview) {
    auto* context = findChild<QWidget*>("marketFlowContext");
    if (!overview || !context || context->findChild<MarketOverviewView*>()) return;
    dashboardOverview_=overview;connect(overview,&MarketOverview::changed,this,&LocalWindow::refreshDashboard);
    auto* view = new MarketOverviewView(overview, context); view->setProperty("card", false); view->setAttribute(Qt::WA_StyledBackground,true);
    view->setMarketFlowMode(true);placeFlowSection(context,view,3);
    connect(view,&MarketOverviewView::evidenceChanged,this,&LocalWindow::syncExploreEvidence);syncExploreEvidence();
}
void LocalWindow::setOfficialCalendar(OfficialCalendar* calendar) {
    auto* context = findChild<QWidget*>("marketFlowContext");
    if (!calendar || !context || context->findChild<OfficialCalendarView*>()) return;
    if(todayDashboard_)todayDashboard_->calendar()->setCalendar(calendar);
    auto* view = new OfficialCalendarView(calendar, context);view->setMarketFlowMode(true); view->setProperty("card", false); view->setAttribute(Qt::WA_StyledBackground,true);
    placeFlowSection(context,view,0);
    connect(view,&OfficialCalendarView::publicSummaryChanged,this,[this](const QString& summary){if(marketMetricsView_)marketMetricsView_->setCalendarSummary(summary);});
    if(marketMetricsView_)marketMetricsView_->setCalendarSummary(view->publicSummary());syncMarketFlowPrivateContext();
}
void LocalWindow::setOfficialNotices(OfficialNotices* notices) {
    auto* context = findChild<QWidget*>("marketFlowContext");
    if (!notices || !context || context->findChild<OfficialNoticesView*>()) return;
    dashboardNotices_=notices;connect(notices,&OfficialNotices::changed,this,&LocalWindow::refreshDashboard);
    auto* view = new OfficialNoticesView(notices, context);view->setMarketFlowMode(true); view->setProperty("card", false); view->setAttribute(Qt::WA_StyledBackground,true);
    placeFlowSection(context,view,2);
}
void LocalWindow::setEarningsCalendar(EarningsCalendar* calendar) {
    auto* context = findChild<QWidget*>("marketFlowContext");
    if (!calendar || !context || context->findChild<EarningsCalendarView*>()) return;
    if(todayDashboard_)todayDashboard_->calendar()->setEarnings(calendar);
    auto* view = new EarningsCalendarView(calendar, context); view->setProperty("card", false); view->setAttribute(Qt::WA_StyledBackground,true);
    placeFlowSection(context,view,1);
    // Korean rows follow the KOSPI top-50 board and favourites; US favourites join the large-cap rule.
    const auto sync=[this,view]{
        QSet<QString> korea,us;
        if(board_)for(const auto& item:board_->items("kr"))korea.insert(item.symbol);
        if(watchlist_){for(const auto& id:watchlist_->ids("kr"))korea.insert(id);for(const auto& id:watchlist_->ids("us"))us.insert(id);}
        view->setInterest(korea,us);
        if(todayDashboard_)todayDashboard_->calendar()->setInterest(korea,us);
    };
    if(board_)connect(board_,&PublicEquityBoard::changed,view,sync);
    if(watchlist_)connect(watchlist_,&ExploreWatchlist::favouritesChanged,view,sync);
    sync();
}

namespace {

QString usCap(double value) { return value >= 1e12 ? QString("$%1T").arg(value / 1e12, 0, 'f', 2) : value >= 1e9 ? QString("$%1B").arg(value / 1e9, 0, 'f', 1) : QString("$%1M").arg(value / 1e6, 0, 'f', 0); }
QString krwCap(double value) {
    return value >= 1e12 ? QObject::tr("%1조원").arg(groupedAmount(QString::number(value / 1e12, 'f', 1))) :
        value >= 1e8 ? QObject::tr("%1억원").arg(groupedAmount(QString::number(std::round(value / 1e8), 'f', 0))) :
        QObject::tr("%1만원").arg(groupedAmount(QString::number(std::round(value / 1e4), 'f', 0)));
}
// Top 20 by a key (enough for four pages of five).
template <typename T, typename Key> QList<T> topBy(QList<T> items, Key key, bool descending) {
    std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) { return descending ? key(a) > key(b) : key(a) < key(b); });
    return items.mid(0, SpotlightCard::MaxRows);
}
} // namespace

void LocalWindow::refreshRankingStudies(){
    if(rankingRefreshing_)return;const QScopedValueRollback guard(rankingRefreshing_,true);
    auto* choice=findChild<QComboBox*>("exploreRankingStudy");if(!choice)return;
    const bool rankVisible=isVisible()&&!isMinimized()&&exploreStack_&&exploreStack_->isVisible()&&exploreStack_->currentIndex()==1;
    choice->setVisible(rankVisible);const auto field=rankingStudyFromKey(choice->currentData().toString());
    const bool enabled=rankVisible&&field!=RankingStudy::None;
    auto* info=findChild<InfoButton*>("exploreRankingInfo");if(info)info->setVisible(enabled);
    if(auto* basis=findChild<QLabel*>("exploreRankingBasis")){basis->setVisible(enabled);basis->setText(field==RankingStudy::OpenInterest?tr("24시간 · 완료 후 갱신"):tr("1시간봉 · 완료 후 갱신"));}
    if(!enabled){if(rankingStudyRunner_)rankingStudyRunner_->clear();if(chartStudyFeed_)chartStudyFeed_->clearOwner("explore-ranking");if(rankingFuturesFeed_)rankingFuturesFeed_->clear();for(auto* card:spotlightCards_)card->setStudyValues({});if(info)info->setInformation(tr("순위 상태"),{});return;}
    const bool futures=field==RankingStudy::OpenInterest||field==RankingStudy::SpotFutures;
    QStringList bases,information{tr("현재 보이는 행만 요청합니다. 같은 원천·종목·완료 1시간 기록을 사용하며 가격 열과 별도로 지연을 판단합니다. 상태 설명이며 매매 신호가 아닙니다.")};
    if(futures)for(auto* card:spotlightCards_)if(card->key()=="coin")for(const auto& item:card->viewportItems())bases.append(item.id);
    if(rankingFuturesFeed_)rankingFuturesFeed_->setTargets(bases,field);
    QList<ChartStudyRequest> demands;QList<RankingStudyCalculation> calculations;
    const auto stockState=stockChartConnectionState();
    for(auto* card:spotlightCards_){QHash<QString,QPair<QString,QString>> cells;
        for(const auto& item:card->viewportItems()){
            RankingStudyValue value;WatchlistIdentity identity;
            if(futures){value=card->key()!="coin"?RankingStudyValue{false,tr("코인 선물 전용"),tr("이 항목은 바이낸스 USDT 현물·무기한 선물에만 적용합니다.")}:rankingFuturesFeed_?rankingFuturesFeed_->observation(item.id):RankingStudyValue{false,tr("받는 중"),{}};}
            else if(card->key()=="coin"){
                if(item.venue!="upbit"&&chartInstruments_)chartInstruments_->ensure("binance");
                const auto route=publicChartRoute(item.venue=="upbit"||item.venue=="binance"?item.venue:QStringLiteral("coin"),item.id);
                if(route.chart)identity=*route.chart;
                else value={false,tr("지원 시장 없음"),route.chartReason};
            }else if(stockState!=StockChartConnectionState::Ready){value={false,stockState==StockChartConnectionState::VaultClosed?tr("금고 열기 필요"):tr("증권사 연결 필요"),stockChartConnectionMessage(stockState)};}
            else identity={"stock",card->key()=="kr"?"KR":"US",item.id};
            if(!identity.code.isEmpty()){
                demands.append({identity,3600000,20});
                if(chartStudyFeed_){const auto observation=chartStudyFeed_->observation(identity.key(),3600000);
                    const auto key=identity.key()+"@"+rankingStudyKey(field)+"@"+QString::number(observation.series.receivedAtMs)+"@"+QString::number(observation.delayed)+"@"+observation.status;
                    calculations.append({key,field,observation});value=rankingStudyRunner_?rankingStudyRunner_->value(key):RankingStudyValue{false,tr("받는 중"),{}};
                }else value={false,tr("받는 중"),{}};
            }
            cells.insert(item.id,{value.text,rankingStudyLabel(field)+" · "+value.text+"\n"+value.information});
            information<<item.id+" · "+value.text+"\n"+value.information;
        }card->setStudyValues(cells);
    }
    if(rankingStudyRunner_)rankingStudyRunner_->setRequests(calculations);
    if(chartStudyFeed_)chartStudyFeed_->setDemands("explore-ranking",demands);
    if(info)info->setInformation(rankingStudyLabel(field),information.join("\n\n"));
}
void LocalWindow::refreshPublicObservations(){
    refreshRankingStudies();
    if(publicObservationsRefreshing_||!marketBreadth_)return;
    const QScopedValueRollback guard(publicObservationsRefreshing_,true);
    const auto* view=findChild<QComboBox*>("exploreView");
    const bool visible=isVisible()&&!isMinimized()&&exploreStack_&&exploreStack_->isVisible()&&view&&view->currentData()!="map";
    auto* breadthToggle=findChild<QCheckBox*>("exploreObservation_breadth");
    auto* relativeToggle=findChild<QCheckBox*>("exploreObservation_relative");
    if(breadthToggle)breadthToggle->hide();
    if(relativeToggle)relativeToggle->hide();
    const bool breadth=visible&&breadthToggle&&breadthToggle->isChecked();
    marketBreadth_->setEnabled(breadth);
    if(breadth){
        if(markets_)marketBreadth_->observeCoins(markets_->rows());
        if(board_)for(const auto& key:{QString("kr"),QString("us")})marketBreadth_->observeStocks(key,board_->items(key));
    }
    auto* row=findChild<QWidget*>("exploreBreadthRow");if(row)row->hide();
    QStringList labels,details;
    for(const auto& market:QList<QPair<QString,QString>>{{"coin",tr("코인 · 전일 종가 대비")},{"kr",tr("KOSPI 상위 50")},{"us",tr("Nasdaq 상위 80")}}){
        const auto value=marketBreadth_->snapshot(market.first);const auto& b=value.breadth;
        const auto summary=b.state.available&&b.percent?tr("상승 %1% · 관측 %2/%3").arg(*b.percent,0,'f',0).arg(b.ready).arg(b.population):
            market.first!="coin"?tr("기준 거래일 없음"):value.reason.isEmpty()?tr("완료된 관측을 모으는 중"):value.reason;
        labels.append(market.second+" · "+summary);
        QString detail=market.second+"\n"+value.basis+"\n"+value.source+"\n"+value.reason;
        detail+=b.state.available?tr("\n상승 %1 · 하락 %2 · 보합 %3 · 관측 %4/%5").arg(b.up).arg(b.down).arg(b.flat).arg(b.ready).arg(b.population):tr("\n상승·하락·보합 자료 없음");
        if(value.completedAtMs>0)detail+="\n"+QDateTime::fromMSecsSinceEpoch(value.completedAtMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm KST");
        details.append(detail);
    }
    if(auto* label=findChild<QLabel*>("exploreBreadthText"))label->setText(labels.join("    |    "));
    if(auto* info=findChild<InfoButton*>("exploreBreadthInfo"))info->setInformation(tr("강세 비율"),details.join("\n\n"));
    setProperty("publicBreadthSummary",breadth?labels.join(" · "):QString{});
    setProperty("publicBreadthInformation",details.join("\n\n"));
    const bool relative=visible&&relativeToggle&&relativeToggle->isChecked();
    QString text,information=tr("같은 거래소·같은 통화의 완료 1시간봉 25개로 최근 24시간 가격 비율 변화를 계산합니다. 공식 알트 지수나 개별 가격의 상승 판단이 아닙니다.");
    QList<ChartStudyRequest> demands;
    if(relative){
        const auto asset=marketMetricsView_?marketMetricsView_->asset():QString("BTC");QString identity;
        if(markets_)for(const auto& item:markets_->rows())if(item.base==asset){identity="coin:upbit:"+item.market;break;}
        if(identity.isEmpty()&&chartInstruments_)for(const auto& item:chartInstruments_->instruments("binance"))if(item.baseAsset==asset&&item.quoteAsset=="USDT"&&item.tradable){identity="coin:binance:"+item.symbol;break;}
        if(asset=="BTC")text=tr("BTC 대비 · 기준 종목");
        else if(identity.isEmpty())text=tr("BTC 대비 · 같은 원천을 확인할 수 없어요");
        else if(!chartStudyFeed_)text=tr("BTC 대비 · 연결할 수 없어요");
        else{
            const auto id=WatchlistWorkspaceList::identityFromKey(identity);const WatchlistIdentity btc{"coin",id.source,id.source=="upbit"?"KRW-BTC":"BTCUSDT"};
            demands={{id,3600000,8},{btc,3600000,8}};
            const auto alt=chartStudyFeed_->observation(identity,3600000),base=chartStudyFeed_->observation(btc.key(),3600000);
            if(alt.series.available&&base.series.available&&!alt.delayed&&!base.delayed){
                const auto result=extendedRelativeStrength(alt.series.candles,base.series.candles);
                if(result.state.available&&result.percent)text=tr("BTC 대비 %1% · 24시간").arg(movementDecoratedText(QString::number(*result.percent,'f',2),*result.percent>0?MovementDirection::Up:*result.percent<0?MovementDirection::Down:MovementDirection::Flat));
                else text=tr("BTC 대비 · ")+result.state.reason;
                information+="\n"+alt.series.sourceUrl+"\n"+base.series.sourceUrl;
                if(result.throughMs>0)information+="\n"+QDateTime::fromMSecsSinceEpoch(result.throughMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm KST");
            }else text=tr("BTC 대비 · ")+(alt.delayed||base.delayed?tr("지연"):!alt.status.isEmpty()?alt.status:!base.status.isEmpty()?base.status:tr("받는 중"));
        }
    }
    const auto selected=relative&&marketMetricsView_?marketMetricsView_->asset():QString{};
    const bool changed=property("publicRelativeSymbol").toString()!=selected||property("publicRelativeText").toString()!=text;
    setProperty("publicRelativeSymbol",selected);setProperty("publicRelativeText",text);setProperty("publicRelativeInformation",information);
    if(chartStudyFeed_)chartStudyFeed_->setDemands("explore-relative",demands);
    if(exploreCoinSummary_)exploreCoinSummary_->setRelativeStrength(text,information);
    if(changed)refreshSpotlight();
    refreshDashboard();
}

void LocalWindow::refreshSpotlight() {
    if (spotlightCards_.isEmpty()) return;
    if (windowShownOnce_ && (!exploreStack_->isVisible() || isMinimized() || exploreStack_->currentIndex() != 1)) {
        rankingCardsDirty_ = true;
        return;
    }
    rankingCardsDirty_ = false;
    const auto upbitRows=markets_?markets_->rows():QList<PublicMarketRow>{};
    QHash<QString, PublicMarketRow> upbitByBase;
    for (const auto& row : upbitRows) upbitByBase.insert(row.base, row);
    for (auto* card : spotlightCards_) {
        const auto key = card->key();
        const auto capItems=board_?board_->items(key):QList<PublicBoardItem>{};
        const bool coin=key=="coin",us=key=="us",binance=coin&&card->coinVenue()=="binance";
        QList<PublicBoardItem> items;
        if(!coin)items=capItems;
        else if(!binance){
            for(const auto& quote:upbitRows){
                bool ok=false;const auto rate=quote.signedChangeRate.toDouble(&ok);
                if(!quote.priced||!ok||!std::isfinite(rate)||quote.sourceTimeMs<=0)continue;
                PublicBoardItem row;row.id=quote.market;row.symbol=quote.base;row.name=quote.koreanName;
                row.price=quote.price;row.changePercent=QString::number(rate*100,'g',16);row.currency="KRW";
                row.tradedValue=quote.tradeValue24h.toDouble();row.sourceTimeMs=quote.sourceTimeMs;row.receivedAtMs=quote.receivedAtMs;
                items.append(row);
            }
        }else if(board_&&chartInstruments_){
            if(isVisible()&&exploreStack_->isVisible()&&exploreStack_->currentIndex()==1){
                chartInstruments_->ensure("binance");board_->refreshBinanceRanking();
            }
            QHash<QString,QString> bases;
            for(const auto& instrument:chartInstruments_->instruments("binance"))
                if(instrument.market=="spot"&&instrument.status=="TRADING"&&instrument.tradable&&instrument.quoteAsset=="USDT")
                    bases.insert(instrument.symbol,instrument.baseAsset);
            for(auto row:board_->items("binance")){
                const auto base=bases.value(row.id);if(base.isEmpty())continue;
                row.symbol=base;row.currency="USDT";items.append(row);
            }
        }
        if(coin)std::sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.symbol<b.symbol;});
        const auto toItem = [&](const PublicBoardItem& item) {
            const bool binanceRow=coin&&item.currency=="USDT";
            const QString volume = binanceRow&&item.tradedValue>0?tr("24h Vol %1 USDT").arg(groupedAmount(QString::number(item.tradedValue,'f',0))):item.tradedValue > 0 ? (coin ? tr("24h Vol %1") : tr("Vol %1")).arg(us ? usCap(item.tradedValue) : krwCap(item.tradedValue))
                                                       : coin ? tr("24h Vol —") : tr("Vol —");
            QString koreanName;
            if (coin) {
                const auto row = upbitByBase.constFind(item.symbol);
                if (row != upbitByBase.cend() && ((item.id == row->market && item.sourceId.isEmpty()) || samePublicCoin(*row, item)))
                    koreanName = row->koreanName;
            }
            SpotlightItem result{item.symbol, key == "kr" ? item.name : item.symbol, volume,
                                 us ? "$" + priceAmount(item.price) : binanceRow?priceAmount(item.price)+" USDT":priceAmount(item.price) + tr("원"), item.changePercent,
                                 coin ? koreanName : QString{}, item.sparkline,{}, {}};
            if(coin&&item.sourceTimeMs>0){result.venue=binanceRow?"binance":"upbit";result.sourceTimeMs=item.sourceTimeMs;result.receivedAtMs=item.receivedAtMs;}
            if(coin&&!binanceRow&&rapidMoves_&&rapidMoves_->enabled()){
                const auto rapid=rapidMoves_->snapshot("KRW-"+item.symbol);
                if(rapid.available&&(rapid.direction==MovementDirection::Up||rapid.direction==MovementDirection::Down)){
                    result.observation=tr("급변 ")+(rapid.direction==MovementDirection::Up?QStringLiteral("▲"):QStringLiteral("▼"));
                    result.observationInformation=tr("업비트 원화 · 완료 1분 관측 6개 · %1% · 같은 구간 거래대금 %2 KRW").arg(rapid.changePercent).arg(rapid.quoteTurnover)+"\n"+
                        QDateTime::fromMSecsSinceEpoch(rapid.fromObservedAtMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm:ss KST")+" ~ "+
                        QDateTime::fromMSecsSinceEpoch(rapid.throughObservedAtMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm:ss KST");
                }
            }
            if(coin&&!binanceRow&&item.symbol==property("publicRelativeSymbol").toString()){
                const auto relative=property("publicRelativeText").toString();
                if(!relative.isEmpty()){result.observation+=(result.observation.isEmpty()?QString{}:QStringLiteral(" · "))+relative;result.observationInformation+="\n"+property("publicRelativeInformation").toString();}
            }
            return result;
        };
        SpotlightMode cap{"cap", coin?tr("글로벌 시총 · CoinGecko"):tr("시가총액"), {}}, gainers{"gainers", tr("상승률 ↓"), {}}, losers{"losers", tr("하락률 ↑"), {}}, volume{"volume", key=="us"?tr("거래대금(근사)"):tr("거래대금"), {}};
        for (const auto& item : capItems.mid(0, SpotlightCard::MaxRows)) cap.items.append(toItem(item));
        QList<PublicBoardItem> ranked;
        for (const auto& item : items) {
            bool valid = false; const double percent = item.changePercent.toDouble(&valid);
            if (valid && std::isfinite(percent)) ranked.append(item);
        }
        const auto change = [](const PublicBoardItem& item) { return item.changePercent.toDouble(); };
        for (const auto& item : topBy(ranked, change, true)) gainers.items.append(toItem(item));
        for (const auto& item : topBy(ranked, change, false)) losers.items.append(toItem(item));
        for (const auto& item : topBy(items, [](const PublicBoardItem& item) { return item.tradedValue; }, true)) volume.items.append(toItem(item));
        card->setModes({gainers, losers, cap, volume}); // Default: greatest signed change. Keep an explicit user mode on refresh.
        const auto marketStatus=board_?board_->marketStatus(key):QString{};
        card->setStatus(coin ? (card->mode()=="cap"?tr("CoinGecko"):binance?tr("최근 24시간"):tr("09시 KST 기준")) :
                        marketStatus==tr("장 마감")?QString{}:marketStatus); // Coins trade around the clock.
        const auto saved = watchlist_ ? watchlist_->ids(key) : QStringList{};
        card->setFavourites(QSet<QString>(saved.cbegin(), saved.cend()));
        card->setToolTip(coin ? (card->mode()=="cap"?tr("CoinGecko 글로벌 시가총액 · KRW"):binance?tr("바이낸스 현물 USDT · 최근 24시간"):tr("업비트 원화 · 전일 종가(한국시간 09:00) 대비"))
                             : key == "kr" ? tr("전일 대비 · KOSPI 시총 상위 50개 중 순위 · 네이버 증권")
                                           : tr("전일 대비 · NASDAQ 시총 상위 80개 중 순위 · Nasdaq"));
    }
}

void LocalWindow::refreshWatchlist(bool scheduleHistory) {
    if (!watchlist_) return;
    if (scheduleHistory && watchlistVisibilityTimer_) watchlistVisibilityTimer_->stop();
    const bool present = !windowShownOnce_ || (watchlist_->isVisible() && !isMinimized());
    QList<WatchlistQuote> quotes;
    QList<HistoryTarget> targets;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto section = watchlist_->section();
    const bool stock = section == "kr" || section == "us";
    const auto stockMessage = stock ? stockChartConnectionMessage(stockChartConnectionState()) : QString{};
    if (present) {
        const auto ids = watchlist_->ids(section);
        QSet<QString> boardSymbols;
        if (board_ && !ids.isEmpty())
            for (const auto& row : board_->items(section)) boardSymbols.insert(row.symbol);
        quotes.reserve(ids.size());
        for (const auto& id : ids) {
            WatchlistQuote quote; quote.section = section; quote.id = id;
            const bool coin = section == "coin", us = section == "us";
            const auto item = board_ ? board_->knownItem(section, id) : PublicBoardItem{};
            if (!item.id.isEmpty()) {
                quote.name = item.name; quote.priceValue = item.price.toDouble(); quote.capValue = item.marketCap;
                quote.price = (us ? "$" : "") + priceAmount(item.price) + (us ? "" : tr("원"));
                quote.change = item.changePercent;
                quote.cap = item.marketCap > 0 ? us ? usCap(item.marketCap) : krwCap(item.marketCap) : QString{};
                quote.volumeValue = coin ? item.tradedValue : item.tradedUnits;
                if (quote.volumeValue > 0) quote.volume = coin ? krwCap(quote.volumeValue) :
                    QLocale().toString(quote.volumeValue, 'f', 0) + tr("주");
                const auto source = coin ? "CoinGecko" : us ? "Nasdaq" : "네이버 증권";
                quote.evidenceProvider=QString::fromUtf8(source);
                quote.evidence = quote.name + " · " + id + "\n" + source + (coin ? tr(" · 최근 24시간 · KRW") : us ? tr(" · 전일 대비 · USD") : tr(" · 전일 대비 · KRW"));
                quote.evidence += tr("\nMC: 표시 통화 기준 시가총액 · %1").arg(coin ? tr("24h Vol: 24시간 거래대금") : tr("거래량: 최근 거래일 체결 주수"));
                const auto at = item.receivedAtMs;
                quote.evidence += tr("\n시세 수신 %1").arg(QDateTime::fromMSecsSinceEpoch(at).toString("MM-dd HH:mm:ss"));
                const auto status = board_->status(section);
                if (!status.isEmpty()) quote.evidence += "\n" + status;
                else if (now - at > 120000) quote.evidence += tr("\n이전 시세 · 갱신 대기");
                if (!boardSymbols.contains(id) || !status.isEmpty() || now - at > 120000) {
                    quote.evidenceState=usabilityText(UsabilityText::Delayed);
                    quote.evidence += tr("\n마지막 정상 시세 유지 · 보드 수집 범위 밖이거나 갱신 대기");
                }
            } else if (coin && markets_) {
                const auto row = markets_->rowForMarket("KRW-" + id);
                if (!row.market.isEmpty()) {
                    quote.name = row.koreanName;quote.evidenceProvider=QStringLiteral("업비트");quote.evidenceSourceTimeMs=row.sourceTimeMs;
                    quote.evidenceState=publicMarketQuoteEvidence(row,now).state;
                    if (row.priced) { quote.priceValue = row.price.toDouble(); quote.price = priceAmount(row.price) + tr("원"); }
                    quote.volumeValue = row.tradeValue24h.toDouble();
                    if (quote.volumeValue > 0) quote.volume = krwCap(quote.volumeValue);
                    quote.evidence = tr("%1 · 업비트 KRW\n24시간 등락률·시가총액 미제공 · 24h Vol: 거래대금").arg(row.koreanName);
                }
            }
            if (marketHistory_ && coin) {
                const auto history = marketHistory_->history(section, id);
                quote.history = history.points;
                if (!history.points.isEmpty()) {
                    quote.evidence += "\n" + history.source + tr("\n추이 %1 — %2").arg(
                        QDateTime::fromMSecsSinceEpoch(qint64(history.points.first().x())).toString("MM-dd HH:mm"),
                        QDateTime::fromMSecsSinceEpoch(qint64(history.points.last().x())).toString("MM-dd HH:mm"));
                }
                if (!history.status.isEmpty()) quote.evidence += "\n" + history.status;
                if (watchlist_->isVisible() && isVisible() && !isMinimized() && watchlist_->section() == section) targets.append({section, id, item.sourceId});
            }
            if (!stockMessage.isEmpty()) quote.evidence += tr("\n주식 차트 · %1").arg(stockMessage);
            quotes.append(quote);
        }
    }
    if (present) watchlist_->setQuotes(quotes);
    if(auto* basis=watchlist_->findChild<QLabel*>("watchlistBasis")){
        const auto original=basis->property("publicHistoryOriginalBasis");
        const auto text=original.isValid()?original.toString():basis->text();
        if(!original.isValid())basis->setProperty("publicHistoryOriginalBasis",text);
        basis->setText(text + (stockMessage.isEmpty() ? QString{} : tr("\n주식 차트 · %1").arg(stockMessage)));
    }
    if(marketHistory_&&isVisible()&&!isMinimized()&&exploreStack_&&exploreStack_->isVisible()){
        const auto appendSelected=[&](const QString& section,const QString& id){if(section!="coin"||id.isEmpty())return;
            // The legacy CoinGecko history DTO has no unique coin-id proof.
            // Retain existing watchlist collection, but never bind it to a different source's detail.
            if(section=="coin"&&board_&&!board_->knownItem(section,id).id.isEmpty())return;
            const HistoryTarget target{section,id,{}};for(const auto& old:targets)if(old.key()==target.key())return;targets.append(target);};
        if(mapDetail_&&mapDetail_->isVisible()&&marketMap_)appendSelected(marketMap_->selectedSection(),marketMap_->selectedId());
        if(exploreCoinSummary_&&exploreCoinSummary_->isVisible()&&marketMetricsView_){const auto id=marketMetricsView_->asset();bool supported=false;if(markets_)for(const auto& row:markets_->rows())if(row.base==id){supported=true;break;}if(supported){for(qsizetype i=targets.size();i>0;--i)if(targets[i-1].key()=="coin:"+id)targets.removeAt(i-1);targets.append({"coin",id,{}});}}
        if(exploreDetail_&&exploreDetail_->isVisible()&&exploreTable_){const auto* cell=exploreTable_->item(exploreTable_->currentRow(),0);if(cell)for(const auto& row:markets_->rows())if(row.market==cell->data(Qt::UserRole).toString())appendSelected("coin",row.base);}
    }
    if (marketHistory_&&scheduleHistory) marketHistory_->setTargets(targets);
}

void LocalWindow::openMarketAlertSettings() {
    openOptionsPage("optionsMarketAlerts");
}

void LocalWindow::refreshDashboard() {
    if(!todayDashboard_||!marketMetricsView_||dashboardRefreshing_)return;
    const bool todayVisible = !windowShownOnce_ || (todayDashboard_->isVisible() && !isMinimized());
    const bool sidebarVisible = exploreCoinSummary_ && (!windowShownOnce_ || (exploreCoinSummary_->isVisible() && !isMinimized()));
    if (!todayVisible && !sidebarVisible) return;
    const QScopedValueRollback guard(dashboardRefreshing_,true);
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto metric=[this](const char* name){auto* card=marketMetricsView_->findChild<ExploreMetricCard*>(name);return card?card->data():ExploreMetricCardData{};};
    auto gap=metric("marketMetricsGapCard"),funding=metric("marketMetricsFundingCard"),coinbase=metric("marketMetricsCoinbaseGapCard");
    gap.title=tr("테더 환산 가격차");coinbase.title=tr("코인베이스 가격차");
    if(auto* state=marketMetricsView_->findChild<QLabel*>("marketMetricsGapState"))if(!state->text().isEmpty())gap.changeText=state->text()+(gap.changeText.isEmpty()?QString{}:" · "+gap.changeText);
    const bool readable=securityReady_&&accountReadsAvailable(vault_)&&!chartPrivateBlocked_&&!chartSourceResetting_;
    const bool current=readable&&portfolio_&&!portfolioInitialRefresh_&&portfolioDisplayView_.asOfTimeMs==0;
    if (todayVisible) {
        // 김프·금리 list. BTC 김프 uses Hana Bank's KRW posting (the ECB daily reference until
        // 2026-11-06); the compact note shows the posting time, or the last good check while late.
        ExploreMetricCardData btcGap;btcGap.id="today:btc-usdkrw-premium";btcGap.sourceKey="public:btc-premium:usdkrw:hana";
        btcGap.title=tr("BTC 김프");btcGap.value=QStringLiteral("—");btcGap.changeText=tr("받는 중");
        qint64 currentBtcFxPostedAt=0;
        btcGap.publicInformation=tr("(업비트 BTC 원화가 ÷ (바이낸스 BTCUSDT 현물가 × 원/달러) − 1) × 100. 1 USDT ≈ 1 USD 전제입니다. 업비트 USDT 원화 가격을 환율로 쓰지 않습니다.\n원/달러는 하나은행이 고시 회차마다 바꾸는 매매기준율(네이버 증권)입니다. 은행 간 시장 환율과 조금 다를 수 있고, 고시가 없는 주말·휴일에는 마지막 고시값을 씁니다. 앱은 1분마다 다시 확인합니다.\n출처: 하나은행 고시 환율 · 네이버 증권 — https://m.stock.naver.com/marketindex/exchange/FX_USDKRW\n고시일이 7일을 넘었거나 3분 넘게 다시 확인하지 못하면 최근 확인값으로 남깁니다. 두 BTC 시세에만 180초 유효기간·30초 시각차를 적용합니다.\n2026-11-06부터 ECB 일별 참고환율 대신 하나은행 고시를 씁니다. 환율이 하루 안에 움직일 때 김프가 그만큼 어긋나던 문제를 줄이려는 변경입니다.\n기존 테더 환산 가격차의 기록·알림과 분리된 참고값이며 신규 알림을 만들지 않습니다.");
        if(todayBtcPremium_){const auto premium=todayBtcPremium_->snapshot();
            if(!premium.percent.isEmpty()){
                auto value=marketFlowRounded(premium.percent,2);btcGap.value=(!value.startsWith('-')&&value!="0.00"&&value!="0"?"+":"")+groupedAmount(value);btcGap.unit="%";
                const auto checkedAt=QDateTime::fromMSecsSinceEpoch(premium.checkedAtMs,QTimeZone("Asia/Seoul"));
                if(premium.state==TodayBtcPremiumSnapshot::State::Recent){
                    // A retained value can span days. Keep the date of the last good check visible.
                    btcGap.changeText=checkedAt.toString("MM/dd HH:mm");btcGap.freshnessText=tr("지연");
                    btcGap.publicInformation+=tr("\n상태: 최근 확인값 · %1").arg(premium.reason);
                }else{
                    currentBtcFxPostedAt=premium.fx.postedAtMs;
                    const auto posted=QDateTime::fromMSecsSinceEpoch(premium.fx.postedAtMs,QTimeZone("Asia/Seoul"));
                    btcGap.changeText=tr("하나 %1").arg(posted.toString(posted.date()==QDateTime::fromMSecsSinceEpoch(now,QTimeZone("Asia/Seoul")).date()?"HH:mm":"MM/dd"));
                }
                btcGap.publicInformation+=tr("\n사용 원/달러: %1 원 · 하나은행 %2 고시\nBTC 김프 확인: %3\n업비트 BTC: %4 KRW\n바이낸스 BTC: %5 USDT").arg(groupedAmount(premium.fx.krwPerUsd),QDateTime::fromMSecsSinceEpoch(premium.fx.postedAtMs,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm"),checkedAt.toString("yyyy-MM-dd HH:mm:ss KST"),premium.upbit.price,premium.binance.price);
                btcGap.evidence={{"today:btc:upbit","업비트",{},premium.upbit.evidence.sourceTimeMs,premium.upbit.evidence.receivedAtMs,true,{}},
                    {"today:btc:binance","Binance",{},premium.binance.evidence.sourceTimeMs,premium.binance.evidence.receivedAtMs,true,{}},
                    {"today:fx:hana",tr("하나은행 · 네이버 증권"),premium.fx.postedDate.toString(Qt::ISODate),premium.fx.postedAtMs,premium.fx.receivedAtMs,true,tr("고시")}};
            }else{btcGap.changeText=premium.reason.contains(tr("수신 전"))?tr("받는 중"):tr("확인 필요");btcGap.publicInformation+=tr("\n상태: %1").arg(premium.reason);}
        }
        const auto world=worldMarkets_?worldMarkets_->snapshot():WorldMarketsSnapshot{};
        const auto& usdKrw=world.quote(WorldMarketItem::UsdKrw);
        ExploreMetricCardData tether;tether.id="today:tether-premium";tether.title=tr("테더 김프");tether.sourceKey="public:tether-premium:usdkrw:hana";
        tether.value=QStringLiteral("—");tether.changeText=tr("받는 중");
        tether.publicInformation=tr("(업비트 USDT 원화가 ÷ 하나은행 원/달러 고시 환율 − 1) × 100. 1 USDT ≈ 1 USD 전제입니다.\n원/달러는 하나은행이 고시 회차마다 바꾸는 매매기준율(네이버 증권)이라 은행 간 시장 환율과 조금 다를 수 있어요. 고시가 없는 주말·휴일에는 마지막 고시값과 비교합니다.\n기존 '테더 환산 가격차'의 기록·알림과 분리된 참고값이며 알림을 만들지 않습니다.");
        PublicMarketRow usdtRow;bool usdtFound=false;
        if(markets_)for(const auto& row:markets_->rows())if(row.base=="USDT"){usdtRow=row;usdtFound=true;break;}
        const auto tetherPercent=usdtFound&&usdtRow.priced&&usdKrw.available()?WorldMarkets::premiumPercent(usdtRow.price,usdKrw.price):QString{};
        if(!tetherPercent.isEmpty()){
            tether.value=(tetherPercent.startsWith('-')||tetherPercent=="0.00"?QString{}:QStringLiteral("+"))+tetherPercent;tether.unit="%";
            const auto posted=QDateTime::fromMSecsSinceEpoch(usdKrw.tradedAtMs,QTimeZone("Asia/Seoul"));
            const bool postedToday=posted.date()==QDateTime::fromMSecsSinceEpoch(now,QTimeZone("Asia/Seoul")).date();
            tether.changeText=tr("하나 %1").arg(posted.toString(postedToday?"HH:mm":"MM/dd"));
            const auto usdtEvidence=publicMarketQuoteEvidence(usdtRow,now);
            const bool usdtLate=!usdtEvidence.complete||usdtRow.sourceTimeMs<=0||usdtRow.sourceTimeMs>now||now-usdtRow.sourceTimeMs>90000;
            if(usdtLate||now-usdKrw.receivedAtMs>3*WorldMarkets::RefreshMs)tether.freshnessText=tr("지연");
            tether.publicInformation+=tr("\nUSDT 원화: %1 KRW (업비트)\n원/달러: %2 원 (하나은행 %3 고시)").arg(priceAmount(usdtRow.price),groupedAmount(usdKrw.price),posted.toString("yyyy-MM-dd HH:mm"));
            tether.evidence={usdtEvidence,{"world:usdkrw",tr("하나은행 · 네이버 증권"),usdKrw.localDate.toString(Qt::ISODate),usdKrw.tradedAtMs,usdKrw.receivedAtMs,true,tr("고시")}};
        }else{
            const auto fxFailure=world.failure(WorldMarketItem::UsdKrw);
            if((!usdKrw.available()&&!fxFailure.isEmpty())||(markets_&&!markets_->busy()&&(!usdtFound||!usdtRow.priced)&&markets_->marketListAvailable()))tether.changeText=tr("확인 필요");
            if(!fxFailure.isEmpty())tether.publicInformation+="\n"+fxFailure;
            if(markets_&&markets_->marketListAvailable()&&!usdtFound)tether.publicInformation+=tr("\n업비트 원화마켓에서 USDT를 찾지 못했어요.");
        }
        // Independent reads of the same Hana endpoint can return different posting rounds,
        // even when both requests succeed. Apply Today's existing one-minute comparison
        // bound to source times; a recent receipt must not make an older round comparable.
        constexpr qint64 MaximumHanaPostingSkewMs=60000;
        if(currentBtcFxPostedAt>0&&usdKrw.available()&&btcGap.unit=="%"&&tether.unit=="%"&&
           qAbs(currentBtcFxPostedAt-usdKrw.tradedAtMs)>MaximumHanaPostingSkewMs){
            auto& older=currentBtcFxPostedAt<usdKrw.tradedAtMs?btcGap:tether;
            older.value=QStringLiteral("—");older.unit.clear();older.changeText=tr("확인 필요");
            older.freshnessText.clear();older.direction=MovementDirection::Unknown;
            older.publicInformation+="\n"+tr("원·달러 고시 시각 확인 필요");
        }
        todayDashboard_->setMetric("gap",btcGap);
        todayDashboard_->setMetric("tether",tether);
        ExploreMetricCardData treasury;treasury.id="today:treasury";treasury.title=tr("미국 10년물");treasury.value=QStringLiteral("—");treasury.changeText=tr("받는 중");treasury.sourceKey="treasury:10y";
        treasury.publicInformation=tr("미국 재무부 일별 금리입니다. 장중 시세가 아닙니다.");
        const auto twoDecimals=[](QString value){const auto dot=value.indexOf('.');if(dot<0)return value+".00";while(value.size()-dot-1<2)value+='0';return value;};
        DailyTreasuryYield usYield;
        if(dashboardOverview_){usYield=dashboardOverview_->treasury10Year(now);
            if(usYield.available){treasury.value=twoDecimals(usYield.percent);treasury.unit="%";treasury.changeText=usYield.observationDate.toString("MM/dd");
                treasury.publicInformation+=tr("\n기준일 %1 (미국 날짜)").arg(usYield.observationDate.toString(Qt::ISODate));
                treasury.evidence={{"treasury:10y",usYield.source,usYield.observationDate.toString(Qt::ISODate),0,usYield.receivedAtMs,true,{}}};}
            else if(!dashboardOverview_->busy()){treasury.changeText=tr("받지 못했어요");if(!usYield.reason.isEmpty())treasury.publicInformation+="\n"+usYield.reason;}}
        todayDashboard_->setMetric("treasury",treasury);
        // US − Japan 10-year spread: the carry behind yen funding. Both legs are daily
        // official tables; differing basis dates are both shown, never aligned by guess.
        const auto& japan=world.japanYield;
        ExploreMetricCardData spread;spread.id="today:us-japan-10y-spread";spread.title=tr("미·일 금리차");spread.sourceKey="public:us-japan-10y-spread";
        spread.value=QStringLiteral("—");spread.changeText=tr("받는 중");
        spread.publicInformation=tr("미국 10년물 금리 − 일본 10년물 금리(%p)입니다. 엔화를 싸게 빌려 다른 자산을 사는 '엔 캐리' 거래의 기본 수익원이라, 이 차이가 빠르게 줄면 청산 압력이 커질 수 있어요.\n두 값 모두 하루 한 번 공시돼요. 두 나라의 기준일이 다르면 '미국·일본' 순서로 둘 다 표시합니다.");
        const auto points=usYield.available&&japan.available()?WorldMarkets::spreadPoints(usYield.percent,japan.tenYear):QString{};
        if(!points.isEmpty()){
            spread.value=points;spread.unit="%p";
            // Different basis days show both, US first; the tooltip names each country.
            spread.changeText=usYield.observationDate==japan.date?japan.date.toString("MM/dd"):tr("%1·%2").arg(usYield.observationDate.toString("MM/dd"),japan.date.toString("MM/dd"));
            if(now-japan.receivedAtMs>3*WorldMarkets::DailyRefreshMs)spread.freshnessText=tr("지연");
            spread.publicInformation+=tr("\n미국 10년물 %1% (미국 재무부, %2)\n일본 10년물 %3% (일본 재무성, %4)").arg(twoDecimals(usYield.percent),usYield.observationDate.toString(Qt::ISODate),japan.tenYear,japan.date.toString(Qt::ISODate));
            spread.evidence={{"treasury:10y",usYield.source,usYield.observationDate.toString(Qt::ISODate),0,usYield.receivedAtMs,true,{}},{"world:jgb",tr("일본 재무성"),japan.date.toString(Qt::ISODate),0,japan.receivedAtMs,true,{}}};
        }else if((dashboardOverview_&&!dashboardOverview_->busy()&&!usYield.available)||(!japan.available()&&!world.japanYieldFailure.isEmpty()))spread.changeText=tr("확인 필요");
        todayDashboard_->setMetric("spread",spread);
        for(const auto& [key,data]:todayWorldMarketMetrics(world,now))todayDashboard_->setMetric(key,data);
        const auto breadth=property("publicBreadthSummary").toString();
        todayDashboard_->setHeadline(breadth.isEmpty()?tr("시장 요약 · 원천과 기준을 확인한 값만 표시합니다"):tr("시장 요약 · ")+breadth);
        const auto amount=current&&hasPortfolioFigure(portfolioDisplayView_)?(portfolioDisplayView_.summary.complete?portfolioDisplayView_.summary.netAssets:portfolioDisplayView_.summary.knownSubtotal):QString{};
        auto history=current&&!portfolio_->dayHistoryBusy()?portfolio_->dayHistoryResult():PortfolioDayHistory{};
        // The foreground projection and the asynchronous history can settle on
        // different ticks. A percentage must describe the total on this card.
        if(amount.isEmpty() || history.closingValuation.complete!=portfolioDisplayView_.summary.complete ||
           amount!=history.closingValuation.knownSubtotal) history={};
        todayDashboard_->setPrivateSummary(readable,amount.isEmpty()?QStringLiteral("—"):portfolioMoneyDisplay_.amount(amount),
            amount.isEmpty()?QString{}:portfolioMoneyDisplay_.unit()+(portfolioDisplayView_.summary.complete?QString{}:tr(" · 확인된 평가액")),
            history,portfolioMoneyDisplay_);
        if(dashboardNotices_){QList<OfficialNoticeFeed> feeds;for(const auto source:{OfficialNoticeSource::Okx,OfficialNoticeSource::Bithumb})feeds.append(dashboardNotices_->feed(source,now));if(readable)feeds.append(dashboardNotices_->feed(OfficialNoticeSource::Upbit,now));todayDashboard_->setNoticeFeeds(feeds);}
    }
    if (!sidebarVisible) return;
    const auto quote=[this,now](const QString& asset,const QString& title){
        ExploreMetricCardData d;d.id="today:"+asset;d.title=title;d.value=tr("받는 중");d.sourceKey="coin:upbit:KRW-"+asset;
        d.publicInformation=tr("업비트 원화 공개 시세입니다. USDT 원화 가격은 공식 원/달러 환율이 아닙니다.");
        if(markets_){const auto rows=markets_->rows();bool found=false;for(const auto& row:rows)if(row.base==asset){found=true;if(row.priced){d.value=priceAmount(row.price);d.unit="KRW";d.evidence={publicMarketQuoteEvidence(row,now)};
            if(!d.evidence.first().complete||row.sourceTimeMs<=0||row.sourceTimeMs>now||now-row.sourceTimeMs>90000)d.freshnessText=tr("지연");}else if(!markets_->busy()&&!markets_->status().isEmpty())d.value=tr("받지 못했어요");break;}
            if(!found&&markets_->marketListAvailable())d.value=tr("업비트 원화마켓 미지원");else if(!found&&!markets_->busy()&&!markets_->status().isEmpty())d.value=tr("받지 못했어요");}
        return d;
    };
    const auto asset=marketMetricsView_->asset();auto choices=watchlist_?watchlist_->ids("coin").mid(0,3):QStringList{};if(choices.isEmpty())choices={"BTC","ETH","SOL"};exploreCoinSummary_->setCoins(choices,asset);
    QString title=asset;if(markets_)for(const auto& row:markets_->rows())if(row.base==asset&&!row.koreanName.isEmpty()){title=row.koreanName+" · "+asset;break;}
    auto selected=quote(asset,title);
    if(selected.unit.isEmpty()&&board_){const auto item=board_->knownItem("coin",asset);if(!item.id.isEmpty()&&!item.price.isEmpty()){selected.value=priceAmount(item.price);selected.unit=item.currency;selected.sourceKey="coingecko:"+item.sourceId;selected.publicInformation=tr("CoinGecko 시총 상위 200의 6시간 갱신 가격입니다. 실시간 시세가 아닙니다.");selected.evidence={{selected.sourceKey,"CoinGecko",{},0,item.receivedAtMs,true,{}}};if(now-item.receivedAtMs>6*3600000)selected.freshnessText=tr("지연");}}
    if(board_){const auto known=verifiedCoinMarketCap(asset,markets_?markets_->rows():QList<PublicMarketRow>{},board_->items("coin"));const auto item=known.value_or(PublicBoardItem{});if(!item.id.isEmpty()&&!item.changePercent.isEmpty()){const auto change=item.changePercent.toDouble();selected.changeText=(change>0?"+":"")+item.changePercent+"% · 24시간";selected.direction=change<0?MovementDirection::Down:change>0?MovementDirection::Up:MovementDirection::Flat;selected.publicInformation+=tr("\n24시간 등락률은 CoinGecko의 6시간 갱신 자료입니다.");selected.evidence.append({"change:coingecko:"+item.sourceId,"CoinGecko",{},0,item.receivedAtMs,true,{}});if(now-item.receivedAtMs>6*3600000)selected.changeText+=tr(" · 지연");}}
    if(marketHistory_&&selected.sourceKey.startsWith("coin:upbit:")){const auto history=marketHistory_->history("coin",asset);if(history.source==QStringLiteral("업비트 · 최근 24시간 · 10분 종가 · KRW")&&history.points.size()>1){selected.history.available=true;selected.history.sourceKey=selected.sourceKey;selected.history.unit="KRW";qint64 previous=0;for(const auto& point:history.points){selected.history.points.append({qint64(point.x()),point.y(),previous>0&&point.x()-previous==600000});previous=qint64(point.x());}}}
    exploreCoinSummary_->setQuote(selected);QString flow;if(auto* text=marketMetricsView_->findChild<QLabel*>("marketMetricsOpenInterestFlow"))flow=text->text();
    exploreCoinSummary_->setMetrics(gap,coinbase,funding,flow);
    QString holding;if(current)for(const auto& group:portfolioView_.groups)if(group.identity==publicAssetIdentity("upbit",asset)){const bool fresh=std::any_of(group.holdings.cbegin(),group.holdings.cend(),[](const auto& item){return item.state==PortfolioValueState::Fresh;});holding=fresh?tr("내 보유 평가액 ")+portfolioMoneyDisplay_.amount(group.freshTotal)+" "+portfolioMoneyDisplay_.unit():tr("보유 중 · 평가액을 확인하고 있어요");if(fresh&&group.unvalued>0)holding+=tr(" · 일부 미평가");break;}
    exploreCoinSummary_->setPrivateHolding(readable,current?holding:tr("보유 내역을 확인하고 있어요"));
}

void LocalWindow::layoutExploreToday() {
    auto* view=findChild<QComboBox*>("exploreView");
    auto* content=findChild<QWidget*>("exploreContent");
    auto* panels=findChild<QWidget*>("explorePanels");
    auto* scroll=findChild<QScrollArea*>("explorePanelsScroll");
    if(!content||!panels||!scroll||!exploreCommonSidebar_)return;
    const bool today=view&&view->currentData()=="flow";
    const bool stacked=today&&width()<1100;
    auto* shell=qobject_cast<QHBoxLayout*>(content->parentWidget()->layout());
    auto* contentLayout=qobject_cast<QVBoxLayout*>(content->layout());
    content->setMaximumWidth(today?ExploreMaximumWidth:1880);
    shell->setContentsMargins(today?6:0,0,today?6:0,0);
    shell->setStretch(0,today?0:1);shell->setStretch(2,today?0:1);
    contentLayout->setContentsMargins(today?QMargins{}:contentLayout->property("originalExploreMargins").value<QMargins>());
    contentLayout->setSpacing(today?12:contentLayout->property("originalExploreSpacing").toInt());
    if(stacked&&scroll->widget()!=panels){
        contentLayout->removeWidget(panels);scroll->setWidget(panels);scroll->show();panels->show();
    }else if(!stacked&&scroll->widget()==panels){
        scroll->takeWidget();panels->setParent(content);
        contentLayout->insertWidget(contentLayout->indexOf(scroll),panels,1);scroll->hide();panels->show();
    }
    auto* columns=qobject_cast<QBoxLayout*>(panels->layout());
    columns->setDirection(stacked?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);columns->setSpacing(today?12:16);
    auto* rail=qobject_cast<QVBoxLayout*>(exploreCommonSidebar_->layout());
    rail->setSpacing(today?12:16);rail->setStretch(0,today?0:3);rail->setStretch(1,today?1:2);
    if(stacked){
        exploreCommonSidebar_->setMinimumWidth(0);exploreCommonSidebar_->setMaximumWidth(QWIDGETSIZE_MAX);
        exploreCommonSidebar_->setMinimumHeight(600);
        exploreStack_->setMinimumHeight(740);
    }else{
        exploreCommonSidebar_->setMinimumHeight(0);exploreStack_->setMinimumHeight(0);
        exploreCommonSidebar_->setFixedWidth(today?(width()>=1680?380:360):rankingWatchlistWidth(panels->width()));
    }
    if(todayDashboard_)todayDashboard_->setWideLayout(today&&width()>=1680);
    if(auto* flow=findChild<QScrollArea*>("marketFlowPage"))if(flow->widget())flow->widget()->setMinimumHeight(stacked?740:0);
    syncHeaderTabs();
}
void LocalWindow::layoutSpotlight() {
    if (windowShownOnce_ && exploreStack_ && (!exploreStack_->isVisible() || isMinimized())) {
        exploreLayoutPending_ = true;
        return;
    }
    exploreLayoutPending_ = false;
    if(watchlist_){auto* view=findChild<QComboBox*>("exploreView");watchlist_->setDashboardLayout(view&&view->currentData()=="flow");}
    layoutExploreToday();
    if (!spotlightGrid_) return;
    // Reuse the existing search/holdings list-detail mode at narrow widths.
    // Wide screens size the detail from the width and keep the fourteen-pixel gap.
    if(mapListDetail_&&mapDetail_){
        if(mapListDetail_->narrow()){mapDetail_->setMinimumWidth(0);mapDetail_->setMaximumWidth(QWIDGETSIZE_MAX);}
        else mapDetail_->setFixedWidth(ListDetailView::detailWidth(mapListDetail_->width()));
    }
    // The local theme changes font metrics after construction. Keep the segments
    // on one line at their final width, especially in the stacked narrow toolbar.
    for (const auto* name : {"exploreView", "exploreMarketSection"})
        if (auto* control = findChild<SegmentedComboBox*>(name)) control->setMinimumWidth(control->sizeHint().width());
    if (auto* filters = findChild<QBoxLayout*>("exploreFilters")) {
        const bool narrow = width() < 1050;
        filters->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        filters->setStretch(2, narrow ? 0 : 1); // The spacer between the view tabs and market tabs.
        filters->setAlignment(narrow ? Qt::AlignTop : Qt::Alignment{});
    }
    // The shared sidebar has already taken its width. Fit rows to the remaining
    // viewport height even when the three markets stack into one column.
    const auto* outer = findChild<QScrollArea*>("spotlightListScroll");
    const int pageWidth = outer ? outer->viewport()->width() : width(), pageHeight = outer ? outer->viewport()->height() : height();
    const int gap=spotlightGrid_->horizontalSpacing();
    if(spotlightColumns_)spotlightColumns_->setDirection(QBoxLayout::LeftToRight);
    if(watchlist_){watchlist_->setMinimumWidth(0);watchlist_->setMaximumWidth(QWIDGETSIZE_MAX);watchlist_->setMinimumHeight(0);watchlist_->setCompact(true);}
    if (windowShownOnce_ && exploreStack_ && exploreStack_->currentIndex() != 1) return;
    const int marketsWidth=pageWidth;
    const int columns = marketsWidth >= 3 * SpotlightCard::ColumnWidth + 2 * gap ? 3 : 1;
    int rows = SpotlightCard::PageSize;
    if (pageHeight > 0) {
        const int perCard = columns == 3 ? pageHeight : (pageHeight - 2 * spotlightGrid_->verticalSpacing()) / 3;
        int chrome=0;for(const auto* card:spotlightCards_)chrome=qMax(chrome,card->chromeHeight());
        rows = qBound(3, (perCard - chrome) / SpotlightCard::RowHeight, columns==3 ? SpotlightCard::LastPageRows : 5);
    }
    for (auto* card : spotlightCards_) card->setRowsPerPage(rows);
    if (columns != rankingGridColumns_) {
        rankingGridColumns_ = columns;
        for (auto* card : spotlightCards_) spotlightGrid_->removeWidget(card);
        for (int i = 0; i < spotlightCards_.size(); ++i) {
            spotlightGrid_->addWidget(spotlightCards_[i], i / columns, i % columns, Qt::AlignTop);
            spotlightCards_[i]->show();
        }
        for (int c = 0; c < 3; ++c) spotlightGrid_->setColumnStretch(c, c < columns ? 1 : 0);
        for (int row = 0; row < 4; ++row) spotlightGrid_->setRowStretch(row, 0);
        spotlightGrid_->setRowStretch((int(spotlightCards_.size()) + columns - 1) / columns, 1);
    }
    queueLegacyExploreEvidence();
}

void LocalWindow::refreshExplore() {
    if (!markets_ || !exploreTable_) return;
    if (windowShownOnce_ && (!exploreStack_->isVisible() || isMinimized() || exploreStack_->currentIndex() != 2)) {
        searchRowsDirty_ = true;
        return;
    }
    searchRowsDirty_ = false;
    const auto rows = markets_->rows();
    const auto* current = exploreTable_->item(exploreTable_->currentRow(), 0);
    const auto selected = current ? current->data(Qt::UserRole).toString() : QString{};
    const QSignalBlocker block(exploreTable_);
    if (exploreTable_->rowCount() != rows.size()) exploreTable_->setRowCount(int(rows.size()));
    const auto setText = [this](int row, int column, const QString& text, Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter) {
        auto* cell = exploreTable_->item(row, column);
        if (!cell) { cell = new QTableWidgetItem; cell->setTextAlignment(alignment); exploreTable_->setItem(row, column, cell); }
        if (cell->text() != text) cell->setText(text);
        return cell;
    };
    int reselect = -1;
    for (int i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        auto* name = setText(i, 0, row.koreanName + (row.warning ? tr(" · 유의") : QString{}));
        name->setData(Qt::UserRole, row.market);
        name->setData(Qt::UserRole + 1, row.koreanName + " " + row.englishName + " " + row.market + " " + row.base);
        name->setData(AvatarRole, row.base);
        setText(i, 1, row.market);
        setText(i, 2, row.priced ? priceAmount(row.price) : tr("—"), Qt::AlignRight | Qt::AlignVCenter);
        auto* change = setText(i, 3, row.priced ? changePercentText(row.signedChangeRate) : tr("—"), Qt::AlignRight | Qt::AlignVCenter);
        const auto changeText = change->text();
        const int direction = !row.priced || changeText.startsWith("0.00") || changeText.startsWith("+0.00") || changeText.startsWith("-0.00") ? 0
            : row.signedChangeRate.startsWith('-') ? -1 : 1;
        change->setData(ChangeDirectionRole, direction);
        change->setForeground(movementColors_->foreground(direction>0?MovementDirection::Up:direction<0?MovementDirection::Down:MovementDirection::Flat));
        if (row.market == selected) reselect = i;
    }
    if (reselect >= 0) exploreTable_->setCurrentCell(reselect, 0);
    if (auto* search = findChild<QLineEdit*>("exploreSearch")) filterExploreRows(search->text().trimmed());
    const auto completed = markets_->completedAtMs();
    findChild<QLabel*>("exploreStatus")->setText((markets_->status().isEmpty() ? tr("시세를 받는 중이에요") : markets_->status()) +
        (completed > 0 ? tr(" · 수신 %1").arg(QDateTime::fromMSecsSinceEpoch(completed).toString("HH:mm:ss")) : QString{}) +
        tr("\n전일 대비는 업비트 전일 종가(UTC 0시 = KST 09시) 기준입니다."));
    if (auto* view = findChild<QComboBox*>("exploreView")) view->setToolTip(findChild<QLabel*>("exploreStatus")->text());
    refreshExploreHoldings();
}

void LocalWindow::filterExploreRows(const QString& query) {
    for (int row = 0; row < exploreTable_->rowCount(); ++row) {
        const auto* cell = exploreTable_->item(row, 0);
        const bool hidden = cell && !query.isEmpty() && !cell->data(Qt::UserRole + 1).toString().contains(query, Qt::CaseInsensitive);
        if (hidden != exploreTable_->isRowHidden(row)) exploreTable_->setRowHidden(row, hidden);
    }
}

void LocalWindow::refreshExploreHoldings() {
    if (!exploreTable_) return;
    if(deferPortfolioLoadingProjection()){portfolioLoadingProjectionDirty_=true;return;}
    const bool readable = securityReady_ && accountReadsAvailable(vault_);
    const bool clearLegacy = !readable && exploreHasPrivateProjection_;
    if (clearLegacy) {
        exploreHasPrivateProjection_ = false;
        clearExplorePrivate();
        // Rebuild from the retained public quote without another response.
        // This also removes hidden legacy holding rows in the Upbit fallback.
        showMapSelection();
    }
    if (!clearLegacy && windowShownOnce_ && (!exploreStack_->isVisible() || isMinimized() || exploreStack_->currentIndex() != 2)) {
        searchRowsDirty_ = true;
        return;
    }
    if (readable) exploreHasPrivateProjection_ = true;
    QHash<QString, AssetPresence> held;
    if (readable) for (const auto& group : portfolioView_.groups) held.insert(group.identity, group.presence);
    for (int row = 0; row < exploreTable_->rowCount(); ++row) {
        const auto* name = exploreTable_->item(row, 1);
        const auto base = name ? name->text().section('-', 1) : QString{};
        QString marker;
        const auto key=publicAssetIdentity("upbit",base);
        if (held.contains(key)) marker = held.value(key) == AssetPresence::Held ? tr("보유") : held.value(key) == AssetPresence::NeedsCheck ? tr("확인 필요") : QString{};
        auto* cell = exploreTable_->item(row, 4);
        if (!cell) { cell = new QTableWidgetItem; exploreTable_->setItem(row, 4, cell); }
        if (cell->text() != marker) cell->setText(marker);
    }
    showExploreDetail();
    // The common source book must agree before lock returns, including a
    // selected KR/US card whose local reference starts at one on reprojection.
    if (clearLegacy) refreshLegacyExploreEvidence(true);
}

void LocalWindow::clearExplorePrivate() {
    if(todayDashboard_){todayDashboard_->clearPrivate();if(dashboardNotices_)todayDashboard_->setNoticeFeeds({dashboardNotices_->feed(OfficialNoticeSource::Okx),dashboardNotices_->feed(OfficialNoticeSource::Bithumb)});}
    if(exploreCoinSummary_)exploreCoinSummary_->clearPrivate();
    if(marketMetricsView_)marketMetricsView_->setPrivateContext(false,false);
    if(auto* calendar=findChild<OfficialCalendarView*>())calendar->setPrivateContext(false,{},{});
    // Revoke open information dialogs before the vault emits its locked state.
    for (auto* detail : {mapDetail_, exploreDetail_})
        if (detail) detail->setPrivateLinkage(false, {});
}

void LocalWindow::showExploreDetail() {
    if (!exploreDetail_ || !markets_) return;
    const auto* current = exploreTable_->item(exploreTable_->currentRow(), 0);
    if (!current) { exploreDetail_->clearDetail(); return; }
    const auto market = current->data(Qt::UserRole).toString();
    for (const auto& row : markets_->rows()) if (row.market == market) {
        const bool readable = securityReady_ && accountReadsAvailable(vault_);
        const PortfolioGroupView* holdings = nullptr;
        if (readable) for (const auto& group : portfolioView_.groups) if (group.identity == publicAssetIdentity("upbit",row.base)) { holdings = &group; break; }
        const bool incomplete = readable && (!portfolioView_.missing.isEmpty() || !portfolioView_.summary.readingAccounts.isEmpty());
        exploreDetail_->showMarket(row, readable, holdings, incomplete);
        exploreDetail_->setPublicSelection("coin", row.base, watchlist_ && watchlist_->contains("coin", row.base));
        refreshPublicDetailPresentation(exploreDetail_,"upbit",row.base,"coin:upbit:"+row.market,"KRW","coin:upbit:"+row.market);
        queueLegacyExploreEvidence();
        return;
    }
    exploreDetail_->clearDetail();
}


} // namespace keefi
