#include "keefi/LocalWindow.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/PlainToolTip.h"
#include "keefi/ProfileSettings.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/MarketFlowPresentation.h"
#include "keefi/InfoButton.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/CompactControls.h"
#include "keefi/Vault.h"
#include "keefi/AssetsWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/ValuationPreferences.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/LoadProgressBar.h"
#include "keefi/LocalComboBox.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/AssetListFilter.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PortfolioAllocation.h"
#include "keefi/PortfolioAssetModel.h"
#include "keefi/PortfolioDayView.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/AssetShareDialog.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/MarketOverviewView.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketMap.h"
#include "keefi/Spotlight.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/ExploreWatchlist.h"
#include <QCryptographicHash>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHash>
#include <QFontMetrics>
#include <QLabel>
#include <QProgressBar>
#include "keefi/MovementColorPolicy.h"
#include "keefi/ExploreMetricCard.h"
#include <QTreeView>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QScopeGuard>
#include <QTimer>
#include <QStackedWidget>
#include <QTableWidget>
#include <QScrollArea>
#include <QDateTime>
#include "keefi/AssetValuation.h"
#include "keefi/AssetIdentity.h"
#include "keefi/AssetShareCard.h"
#include "keefi/ChartShareCard.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/PortfolioContribution.h"
#include "keefi/PortfolioDisplayFx.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/ValuationBundle.h"
#include <QTabBar>
#include <QScrollBar>
#include <QStyle>
#include <utility>
#include <algorithm>
#include <optional>
#include <cmath>
#include <limits>
#include <QAbstractItemView>
#include <QComboBox>
#include <QLayout>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextLayout>
#include <QVariant>
#include <QWidget>

namespace keefi {
using local_window_detail::hasPortfolioFigure;
using local_window_detail::textLabel;
using local_window_detail::ledgerTextLabel;
using local_window_detail::button;
using local_window_detail::displayChoice;
using local_window_detail::problemConnectionStates;

namespace {
bool portfolioLoadTerminal(const PortfolioConnection& connection){
    static const QSet<QString> states{"complete","failed","authentication_required","account_required","unsupported","stopped"};
    return !connection.active&&states.contains(connection.state);
}


// Qt asks a delegate for a QSize while measuring both heights and widths.
// Only a width request needs formatted cell text. The row layouts have
// known minimum heights and the tree header already owns the column widths.
class PortfolioAssetTree final : public QTreeView {
  public:
    using QTreeView::QTreeView;
    bool measuringColumn() const { return measuringColumn_; }
  protected:
    int sizeHintForColumn(int column) const override {
        const QScopedValueRollback guard(measuringColumn_, true);
        return QTreeView::sizeHintForColumn(column);
    }
  private:
    mutable bool measuringColumn_ = false;
};

// Asset rows show where a holding is kept under its name instead of in a
// column of its own, and the share column carries a small bar drawn to the
// percentage it prints. Holding rows name their account and market in place
// of the repeated asset. Model columns and texts stay as they were.
class PortfolioAssetDelegate final : public QStyledItemDelegate {
  public:
    static constexpr int WhereColumn = 1, ShareColumn = 5, BarWidth = 36;
    explicit PortfolioAssetDelegate(PortfolioAssetTree* tree) : QStyledItemDelegate(tree), tree_(tree), rows_(new WorkbenchRowDelegate(this)) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const int minimum = index.parent().isValid() ? 34 : twoLines(index) ? 54 : 46;
        const int height = qMax(minimum, option.fontMetrics.height() + 6);
        if (!tree_->measuringColumn()) return {tree_->columnWidth(index.column()), height};
        auto size = rows_->sizeHint(option, index);
        size.setHeight(qMax(size.height(), height));
        if (index.column() == ShareColumn) size.setWidth(size.width() + BarWidth + 10);
        return size;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool highlighted = option.state & (QStyle::State_Selected | QStyle::State_MouseOver);
        if (highlighted) painter->fillRect(option.rect, themeColors().surface);
        if (index.parent().isValid() && index.column() == 0) {
            // A holding row: its account and market, or a pending read's state and account.
            const auto where = index.siblingAtColumn(WhereColumn).data().toString();
            const bool pending = index.parent().data(Qt::UserRole + 1).toString() == QStringLiteral("missing");
            const auto text = pending ? index.data().toString() + QStringLiteral(" · ") + where : where;
            painter->save();
            painter->setPen(themeColors().text2);
            drawLedgerText(*painter, QRectF(option.rect.adjusted(12, 0, -12, 0)), text, FontRole::Secondary14, Qt::AlignLeft | Qt::AlignVCenter);
            painter->restore();
            return;
        }
        QStyleOptionViewItem main(option);
        if (twoLines(index)) main.rect.setHeight(option.rect.height() - 20); // the first line; the place goes under it
        rows_->paint(painter, main, index);
        // After the row paint, so a hovered or selected row does not cover the bar.
        if (index.column() == ShareColumn) drawShareBar(painter, main.rect, index.data().toString());
        if (index.column() == 0 && twoLines(index)) {
            painter->save();
            painter->setPen(themeColors().text3);
            const QRectF place(option.rect.left() + 12, option.rect.bottom() - 25, option.rect.width() - 24, 18);
            const QFontMetrics caption(KeefiFonts::font(FontRole::Caption12));
            drawLedgerText(*painter, place, caption.elidedText(index.siblingAtColumn(WhereColumn).data().toString(), Qt::ElideRight, int(place.width())),
                FontRole::Caption12, Qt::AlignLeft | Qt::AlignVCenter);
            painter->restore();
        }
    }

  private:
    static bool twoLines(const QModelIndex& index) {
        return index.data(PortfolioAssetModel::StorageLineRole).toBool();
    }
    static void drawShareBar(QPainter* painter, const QRect& cell, const QString& text) {
        auto number = text;
        bool ok = false;
        const double share = number.remove(QLatin1Char('%')).trimmed().toDouble(&ok);
        if (!ok || share <= 0) return;
        const QRectF track(cell.left() + 12, cell.center().y() - 1.5, BarWidth, 4);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(themeColors().rule);
        painter->drawRoundedRect(track, 2, 2);
        painter->setBrush(themeColors().text3);
        painter->drawRoundedRect(QRectF(track.left(), track.top(), qMax(1.0, track.width() * qMin(share, 100.0) / 100.0), track.height()), 2, 2);
        painter->restore();
    }
    PortfolioAssetTree* tree_;
    WorkbenchRowDelegate* rows_;
};

int publicLedgerWidth(const QString& text) {
    QTextLayout layout(text,KeefiFonts::font(FontRole::Secondary14));layout.setFormats(KeefiFonts::numberRuns(text));
    layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();
    return line.isValid()?int(std::ceil(line.naturalTextWidth())):0;
}
} // namespace

void LocalWindow::setPortfolioController(PortfolioController* controller) {
    if(portfolio_||!controller)return;portfolio_=controller;
    delete assetsWindow_;assetsWindow_=new AssetsWindow(vault_,this,nullptr,portfolio_->historyLedger());assetsWindow_->setCurrentPortfolio(portfolio_);
    auto* root=qobject_cast<QVBoxLayout*>(centralWidget()->layout());const int position=root->indexOf(accountPanel_);root->removeWidget(accountPanel_);
    auto* pages=new QTabWidget(centralWidget());pages->setObjectName("portfolioPages");
    pages->tabBar()->hide(); // The header's segmented tabs drive the pages.
    pages->setDocumentMode(true);
    auto* home=new QWidget(pages);home->setObjectName("portfolioHome");auto* homeOuter=new QVBoxLayout(home);homeOuter->setContentsMargins(0,0,0,0);
    homeBody_=new QWidget(home);auto* bodyOuter=new QVBoxLayout(homeBody_);bodyOuter->setContentsMargins(0,0,0,0);
    auto* homeScroll=new QScrollArea(homeBody_);homeScroll->setObjectName("portfolioHomeScroll");homeScroll->setWidgetResizable(true);homeScroll->setFrameShape(QFrame::NoFrame);homeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);bodyOuter->addWidget(homeScroll);
    auto* bodyContent=new QWidget;auto* bodyLayout=new QVBoxLayout(bodyContent);bodyLayout->setContentsMargins(0,0,0,0);bodyLayout->setSizeConstraint(QLayout::SetMinimumSize);homeScroll->setWidget(bodyContent);homeOuter->addWidget(homeBody_,1);
    portfolioWorkbench_=new QWidget(homeBody_);portfolioWorkbench_->setObjectName("portfolioWorkbench");
    auto* homeLayout=new QVBoxLayout(portfolioWorkbench_);homeLayout->setContentsMargins(0,8,0,0);homeLayout->setSpacing(12);bodyLayout->addWidget(portfolioWorkbench_,1);
    // Locked or not ready: one clear card and its single action instead of an empty table.
    lockedState_=new QFrame(home);lockedState_->setObjectName("portfolioLockedState");lockedState_->setProperty("card",false);
    {auto* lockedLayout=new QVBoxLayout(lockedState_);lockedLayout->setContentsMargins(36,36,36,36);lockedLayout->setSpacing(12);lockedLayout->addStretch();
     auto* title=textLabel({},lockedState_,"portfolioLockedTitle");title->setProperty("type","section");title->setAlignment(Qt::AlignCenter);
     auto* text=textLabel({},lockedState_,"portfolioLockedText");text->setProperty("type","muted");text->setAlignment(Qt::AlignCenter);
     auto* action=button(QString(),"portfolioLockedAction",lockedState_);action->setProperty("primary",true);
     connect(action,&QPushButton::clicked,this,[this]{(vault_.exists()?unlockVault_:createVault_)->click();});
     lockedLayout->addWidget(title);lockedLayout->addWidget(text);lockedLayout->addWidget(action,0,Qt::AlignHCenter);lockedLayout->addStretch();}
    lockedState_->hide();homeOuter->addWidget(lockedState_,1);
    // Workbench head: the confirmed total first, then what is left out of it.
    auto* head=new QHBoxLayout;head->setSpacing(24);portfolioHead_=head;
    auto* totalColumn=new QVBoxLayout;totalColumn->setSpacing(2);
    auto* totalLabel=textLabel(tr("확인된 자산"),home,"portfolioTotalLabel");totalLabel->setProperty("type","muted");totalLabel->setWordWrap(false);
    portfolioTotal_=textLabel(QStringLiteral("—"),home,"portfolioTotal");portfolioTotal_->setProperty("type","hero");portfolioTotal_->setWordWrap(false);
    portfolioTotalUnit_=textLabel(QStringLiteral("KRW"),home,"portfolioTotalUnit");portfolioTotalUnit_->setProperty("type","heroUnit");portfolioTotalUnit_->setWordWrap(false);portfolioTotalUnit_->hide();
    portfolioTotal_->setToolTip(tr("미평가·참고 평가·확인되지 않은 계좌는 이 합계에 넣지 않고 옆에 따로 표시합니다."));
    auto* figure=new QHBoxLayout;figure->setSpacing(8);figure->addWidget(portfolioTotal_);figure->addWidget(portfolioTotalUnit_,0,Qt::AlignBottom);
    portfolioCurrencyStatus_=ledgerTextLabel({},FontRole::Caption12,home,"portfolioCurrencyStatus");portfolioCurrencyStatus_->setProperty("type","muted");portfolioCurrencyStatus_->hide();figure->addWidget(portfolioCurrencyStatus_,0,Qt::AlignBottom);
    portfolioCurrencyInfo_=new InfoButton(home);portfolioCurrencyInfo_->setObjectName("portfolioCurrencyInfo");portfolioCurrencyInfo_->hide();figure->addWidget(portfolioCurrencyInfo_);
    portfolioTotalEvidence_=textLabel({},home,"portfolioTotalEvidence");portfolioTotalEvidence_->setFont(KeefiFonts::font(FontRole::Number11));
    setThemeStyle(portfolioTotalEvidence_,"color:${accentText};");figure->addWidget(portfolioTotalEvidence_,0,Qt::AlignBottom);figure->addStretch();
    portfolioGaps_=new QWidget(home);portfolioGaps_->setObjectName("portfolioGaps");auto* gapsLayout=new QHBoxLayout(portfolioGaps_);gapsLayout->setContentsMargins(0,4,0,0);gapsLayout->setSpacing(6);
    // What the total leaves out sits directly under the total, not across the screen.
    totalColumn->addWidget(totalLabel);totalColumn->addLayout(figure);totalColumn->addWidget(portfolioGaps_,0,Qt::AlignLeft);
    portfolioRefreshNote_=textLabel({},home,"portfolioRefreshNote");portfolioRefreshNote_->setProperty("type","muted");portfolioRefreshNote_->hide();totalColumn->addWidget(portfolioRefreshNote_);
    portfolioDayView_=new PortfolioDayView(home);totalColumn->addWidget(portfolioDayView_);
    // The flow chart stays closed until asked for; the choice is remembered.
    portfolioDayView_->setDetailsExpanded(ProfileSettings().value(QStringLiteral("display/dayFlowExpanded"),false).toBool());
    connect(portfolioDayView_,&PortfolioDayView::detailsExpandedChanged,this,[](bool open){ProfileSettings().setValue(QStringLiteral("display/dayFlowExpanded"),open);});
    portfolioLoadingPane_=new QWidget(homeBody_);portfolioLoadingPane_->setObjectName("portfolioLoadingPane");
    auto* loadingLayout=new QVBoxLayout(portfolioLoadingPane_);loadingLayout->setContentsMargins(24,24,24,24);loadingLayout->addStretch();
    auto* loadingCard=new QFrame(portfolioLoadingPane_);loadingCard->setObjectName("portfolioLoadingCard");loadingCard->setProperty("card",false);
    loadingCard->setMinimumWidth(520);loadingCard->setMaximumWidth(520);
    auto* cardLayout=new QVBoxLayout(loadingCard);cardLayout->setContentsMargins(32,32,32,32);cardLayout->setSpacing(12);
    portfolioLoading_=textLabel(usabilityText(UsabilityText::PortfolioLoadTitle),loadingCard,"portfolioLoading");portfolioLoading_->setFont(KeefiFonts::font(FontRole::Title17));portfolioLoading_->setAlignment(Qt::AlignCenter);cardLayout->addWidget(portfolioLoading_);
    portfolioLoadStage_=textLabel(usabilityText(UsabilityText::PortfolioLoadAccounts),loadingCard,"portfolioLoadStage");portfolioLoadStage_->setProperty("type","muted");portfolioLoadStage_->setWordWrap(false);
    auto* progressRow=new QHBoxLayout;progressRow->setSpacing(12);
    portfolioLoadProgress_=new LoadProgressBar(loadingCard);portfolioLoadProgress_->setObjectName("portfolioLoadProgress");portfolioLoadProgress_->setFixedHeight(8);
    portfolioLoadProgress_->setToolTip(usabilityText(UsabilityText::PortfolioLoadUnitInfo));progressRow->addWidget(portfolioLoadProgress_,1);
    portfolioLoadPercent_=textLabel("0%",loadingCard,"portfolioLoadPercent");portfolioLoadPercent_->setFont(KeefiFonts::font(FontRole::Number11));portfolioLoadPercent_->setMinimumWidth(42);portfolioLoadPercent_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);progressRow->addWidget(portfolioLoadPercent_);cardLayout->addLayout(progressRow);
    // The number follows the solid fill, so it can trail the finished steps for a moment but never run ahead of them.
    connect(portfolioLoadProgress_,&LoadProgressBar::shownChanged,this,[this]{portfolioLoadPercent_->setText(QString::number(portfolioLoadProgress_->shownPercent())+"%");});
    portfolioLoadTaskCount_=textLabel(usabilityText(UsabilityText::PortfolioLoadChecking),loadingCard,"portfolioLoadTaskCount");portfolioLoadTaskCount_->setProperty("type","muted");portfolioLoadTaskCount_->setWordWrap(false);
    // One caption under the bar: the step being checked, then how many are done.
    // How the steps are counted lives in the bar's tooltip and the user guide.
    auto* captionRow=new QHBoxLayout;captionRow->setSpacing(6);captionRow->addStretch();captionRow->addWidget(portfolioLoadStage_);
    auto* captionDot=textLabel(QStringLiteral("·"),loadingCard,"portfolioLoadCaptionDot");captionDot->setProperty("type","muted");captionDot->setWordWrap(false);captionRow->addWidget(captionDot);
    captionRow->addWidget(portfolioLoadTaskCount_);captionRow->addStretch();cardLayout->addLayout(captionRow);
    portfolioPendingDetails_=button(usabilityText(UsabilityText::PortfolioLoadAvailable),"portfolioPendingDetails",loadingCard);portfolioPendingDetails_->hide();cardLayout->addWidget(portfolioPendingDetails_,0,Qt::AlignHCenter);
    loadingLayout->addWidget(loadingCard,0,Qt::AlignHCenter);loadingLayout->addStretch();
    connect(portfolioPendingDetails_,&QPushButton::clicked,this,[this]{portfolioInitialRefresh_=false;portfolioRevision_.clear();refreshPortfolio();});
    portfolioLoading_->hide();portfolioLoadingPane_->hide();
    head->addLayout(totalColumn,1);
    portfolioAllocation_=new PortfolioAllocation(home);head->addWidget(portfolioAllocation_,1,Qt::AlignTop);
    auto* environment=new SegmentedComboBox(home);environment->setObjectName("portfolioEnvironment");environment->addItem(tr("실전·메인넷"),"live");
    environment->setMinimumWidth(environment->sizeHint().width());
    environment->hide(); // Live (Solana mainnet) only; paper/testnet support was removed on 2026-10-21.
    head->addWidget(environment,0,Qt::AlignBottom);homeLayout->addLayout(head);
    portfolioExpiryTimer_=new QTimer(this);
    portfolioExpiryTimer_->setSingleShot(true);
    portfolioExpiryTimer_->setTimerType(Qt::PreciseTimer);
    connect(portfolioExpiryTimer_,&QTimer::timeout,this,&LocalWindow::refreshPortfolioPresentation);
    auto* historyTimer=new QTimer(this);historyTimer->setInterval(1000);
    connect(historyTimer,&QTimer::timeout,this,[this]{
        if (smallThresholdTransitionMs_ && QDateTime::currentMSecsSinceEpoch() >= smallThresholdTransitionMs_) applyPortfolioFilter(true);
        if(securityReady_&&accountReadsAvailable(vault_)){
            // Also catch wall-clock jumps while the monotonic deadline timer was asleep.
            if(portfolioNextExpiryMs_>0&&QDateTime::currentMSecsSinceEpoch()>=portfolioNextExpiryMs_)refreshPortfolioPresentation();
            if(trustPill_&&trustPill_->isVisible())refreshTrust();
        }
        if(moneyCurrency_==MoneyCurrency::Usd)refreshMoneyPresentation();
        if(portfolioDayView_->isVisible()||(todayDashboard_&&todayDashboard_->isVisible()))refreshDayHistory();
    });historyTimer->start();
    connect(controller,&PortfolioController::dayHistoryChanged,this,[this]{refreshDayHistory(false);refreshDashboard();});
    connect(controller,&PortfolioController::coinPerformanceChanged,this,[this]{refreshDayHistory(false);});
    connect(environment,&QComboBox::currentIndexChanged,this,[this]{
        resetPortfolioLoadProgress();
        portfolioCachedView_={};portfolioDisplayView_={};portfolioRestoreCaptured_=false;portfolioInitialRefresh_=false;portfolioInitialStartedMs_=0;
        portfolioRevision_.clear();refreshPortfolio();});
    // Detailed status lines stay available to the trust view; the home shows the pill instead.
    portfolioSummary_=textLabel({},home,"portfolioSummary");portfolioProgress_=textLabel({},home,"portfolioProgress");
    portfolioSummary_->hide();portfolioProgress_->hide();homeLayout->addWidget(portfolioSummary_);homeLayout->addWidget(portfolioProgress_);
    // Keep search and the compact balance action on one row at the minimum width.
    auto* tools=new QBoxLayout(QBoxLayout::LeftToRight);tools->setSpacing(8);homeTools_=tools;
    portfolioFilter_ = new AssetListFilter("portfolioAssetFilter", home);
    tools->addWidget(portfolioFilter_,1);auto* toolButtons=new QHBoxLayout;toolButtons->setSpacing(8);tools->addLayout(toolButtons);
    connect(portfolioFilter_, &AssetListFilter::changed, this, [this] { applyPortfolioFilter(); });
    if(auto* counts=portfolioFilter_->findChild<QLabel*>("portfolioAssetFilterStatus"))counts->hide();
    hideSmall_=new QPushButton(tr("소액 숨기기"),home);hideSmall_->setObjectName("hideSmallBalances");hideSmall_->setCheckable(true);hideSmall_->setProperty("compact",true);
    hideSmall_->hide(); // Display choices live in 옵션 · 표시; the button retains the shared state.
    hideSmall_->setChecked(ProfileSettings().value(QStringLiteral("display/hideSmallBalances"),true).toBool());
    connect(hideSmall_,&QPushButton::toggled,this,[this](bool checked){
        ProfileSettings().setValue("display/hideSmallBalances",checked);applyPortfolioFilter();
    });
    hideUnvalued_=new QPushButton(tr("미평가 숨기기"),home);
    hideUnvalued_->setObjectName("hideUnvaluedBalances");hideUnvalued_->setCheckable(true);hideUnvalued_->setProperty("compact",true);
    hideUnvalued_->setFixedWidth(hideUnvalued_->fontMetrics().horizontalAdvance(tr("미평가 숨김 · 9999"))+30);
    hideUnvalued_->setChecked(ProfileSettings().value("display/hideUnvaluedBalances",true).toBool());
    hideUnvalued_->setToolTip(tr("평가가격이 없는 자산만 목록에서 숨깁니다. 0원·참고 평가·계좌 조회 실패는 유지합니다. 총 평가액은 변하지 않습니다."));
    hideUnvalued_->hide();
    connect(hideUnvalued_,&QPushButton::toggled,this,[this](bool checked){
        ProfileSettings().setValue("display/hideUnvaluedBalances",checked);portfolioRevision_.clear();refreshPortfolio();
    });
    // Reads refresh on their own (G6 refresh policy); only a failed account offers a retry.
    auto* retry=button(tr("다시 조회"),"retryPortfolioConnection",home);
    auto* priceDetails=button(tr("원화 평가 상세"),"portfolioPriceDetails",home);
    priceDetails->hide(); // Reached from the trust pill's popup.
    retry->setProperty("compact",true);toolButtons->addWidget(retry);
    toolButtons->addStretch();
    homeLayout->addLayout(tools);
    portfolioScopeControls_=new QWidget(home);
    auto* scopeRow=new QHBoxLayout(portfolioScopeControls_);scopeRow->setContentsMargins(0,0,0,0);scopeRow->setSpacing(12);
    portfolioClass_=new SegmentedComboBox(home);portfolioClass_->setObjectName("portfolioClassFilter");
    portfolioClass_->addItem(tr("전체"),"all");portfolioClass_->addItem(tr("코인"),"coin");
    portfolioClass_->addItem(tr("주식"),"stock");portfolioClass_->addItem(tr("현금"),"cash");
    portfolioClass_->setAccessibleName(tr("자산 종류"));
    portfolioAccount_=new LocalComboBox(home);portfolioAccount_->setObjectName("portfolioAccountFilter");
    portfolioAccount_->addItem(tr("전체 계좌"),QString{});portfolioAccount_->setAccessibleName(tr("계좌 필터"));
    portfolioAccount_->setMinimumWidth(190);portfolioAccount_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    portfolioAccount_->setMinimumContentsLength(12);
    for(auto* filter:{portfolioClass_,portfolioAccount_}){
        filter->setToolTip(tr("목록과 선택 상세에 적용합니다. 상단 평가액과 비중은 전체 계좌 기준입니다."));
        connect(filter,&QComboBox::currentIndexChanged,this,&LocalWindow::renderPortfolioAssets);
    }
    // Kind, account and search share one row; syncPortfolioLayout stacks them in narrow windows.
    scopeRow->addWidget(portfolioClass_);scopeRow->addWidget(portfolioAccount_);tools->insertWidget(0,portfolioScopeControls_);
    connect(priceDetails,&QPushButton::clicked,this,[this]{
        if(!accountReadsAvailable(vault_)||!securityReady_||portfolio_->pricingStatus().isEmpty())return;
        auto* box=new QMessageBox(QMessageBox::Information,tr("원화 평가 상태"),portfolio_->pricingStatus(),QMessageBox::Ok,this);
        box->setObjectName("publicPriceDetails");box->setTextFormat(Qt::PlainText);box->setInformativeText(portfolio_->pricingDetails());box->setAttribute(Qt::WA_DeleteOnClose);
        auto* retryPrice=box->addButton(tr("가격 다시 조회"),QMessageBox::ActionRole);retryPrice->setEnabled(!portfolio_->pricingBusy());
        connect(retryPrice,&QPushButton::clicked,box,[this,box]{portfolio_->refreshPrices();box->accept();});
        connect(&vault_,&Vault::aboutToLock,box,[box]{box->setText({});box->setInformativeText({});box->reject();});box->show();
    });
    auto* tree = new PortfolioAssetTree(home);
    portfolioAssets_=tree;portfolioAssets_->setObjectName("portfolioAssets");
    portfolioModel_=new PortfolioAssetModel(portfolioAssets_);
    portfolioAssets_->setModel(portfolioModel_);
    portfolioAssets_->header()->setSortIndicator(3,Qt::DescendingOrder);portfolioAssets_->header()->setSortIndicatorShown(true);
    portfolioAssets_->setRootIsDecorated(true);
    setThemeStyle(portfolioAssets_,"QTreeView{background:${bg};border:0;border-radius:0;} QTreeView::item{border-bottom:1px solid ${rule};}");portfolioAssets_->setAlternatingRowColors(false);
    portfolioAssets_->setItemDelegate(new PortfolioAssetDelegate(tree));portfolioAssets_->setMouseTracking(true);
    portfolioAssets_->setColumnHidden(4,true);portfolioAssets_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Where a holding is kept is drawn under its name; the column stays in the model for search and tests.
    portfolioAssets_->setColumnHidden(1,true);
    portfolioAssets_->header()->setStretchLastSection(false);portfolioAssets_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    portfolioAssets_->header()->setSectionResizeMode(5,QHeaderView::Fixed);
    portfolioAssets_->setColumnWidth(5,112);
    portfolioAssets_->setToolTip(tr("보유 자산과 보관처를 확인해요."));
    auto* listEmpty=textLabel(tr("조건에 맞는 자산이 없습니다."),home,"portfolioListEmpty");listEmpty->setProperty("type","muted");listEmpty->hide();homeLayout->addWidget(listEmpty);
    portfolioDetail_=new AssetDetailPanel(home);
    portfolioDetail_->enableAssetShareControl();
    connect(portfolioDetail_,&AssetDetailPanel::assetShareRequested,this,&LocalWindow::openAssetShare);
    portfolioListDetail_=new ListDetailView(portfolioAssets_,portfolioDetail_,home);homeLayout->addWidget(portfolioListDetail_,1);
    // An empty detail pane only said "select an asset": the list keeps the width until a row is chosen.
    portfolioListDetail_->setDetailOnlyWithSelection(true);
    const int minimumListHeight=200+portfolioAssets_->header()->sizeHint().height()+2*portfolioAssets_->frameWidth();
    portfolioAssets_->setMinimumHeight(minimumListHeight);portfolioListDetail_->setMinimumHeight(minimumListHeight);
    auto* evidenceScroll=new QScrollArea(home);evidenceScroll->setObjectName("portfolioEvidenceScroll");evidenceScroll->setWidgetResizable(true);evidenceScroll->setFrameShape(QFrame::NoFrame);evidenceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);evidenceScroll->setMaximumHeight(144);
    portfolioEvidence_=new ExploreEvidenceFooter;portfolioEvidence_->setObjectName("portfolioEvidenceFooter");evidenceScroll->setWidget(portfolioEvidence_);homeLayout->addWidget(evidenceScroll);
    bodyLayout->addWidget(portfolioLoadingPane_,1);
    connect(portfolioDetail_, &AssetDetailPanel::valuationExclusionRequested, this, [this](const QString& asset, bool excluded) {
        if (!accountReadsAvailable(vault_)) return;
        QString error;
        if (!valuationPreferences_->setExcluded(asset, excluded, &error))
            QMessageBox::warning(this, tr("평가 범위"), error);
    });
    // Scroll instead of squeezing the account form below its minimum height in short windows.
    auto* connectionsPage=new QScrollArea(pages);connectionsPage_=connectionsPage;connectionsPage->setObjectName("connectionsPage");connectionsPage->setWidgetResizable(true);connectionsPage->setFrameShape(QFrame::NoFrame);
    connectionsPage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* connectionsContent=new QWidget(connectionsPage);auto* connectionsLayout=new QVBoxLayout(connectionsContent);connectionsLayout->setContentsMargins(0,6,6,0);
    problemBanner_=new QPushButton(connectionsPage);problemBanner_->setObjectName("connectionProblemBanner");problemBanner_->setProperty("chip","warn");problemBanner_->hide();
    connectionsLayout->addWidget(problemBanner_,0,Qt::AlignLeft);
    auto* statusToggle=new QPushButton(tr("전체 연결 진단 ▾"),connectionsPage);statusToggle->setObjectName("connectionStatusToggle");statusToggle->setCheckable(true);statusToggle->setProperty("disclose",true);applyCompactButton(statusToggle);

    auto* statusTitle=textLabel(tr("계좌별 조회 상태"),connectionsPage);statusTitle->setProperty("type","section");statusTitle->hide();connectionsLayout->addWidget(statusTitle);
    connectionStatus_=new QTableWidget(0,5,connectionsPage);connectionStatus_->setObjectName("connectionStatusTable");
    connectionStatus_->setHorizontalHeaderLabels({tr("연결"),tr("상태 · 원인"),tr("마지막 자료"),tr("저장"),tr("조치")});
    connectionStatus_->setAccessibleName(tr("계좌별 조회 상태와 조치"));connectionStatus_->verticalHeader()->hide();
    connectionStatus_->setEditTriggers(QAbstractItemView::NoEditTriggers);connectionStatus_->setSelectionMode(QAbstractItemView::NoSelection);connectionStatus_->setFocusPolicy(Qt::NoFocus);
    connectionStatus_->setWordWrap(false);connectionStatus_->setTextElideMode(Qt::ElideRight);connectionStatus_->setShowGrid(false);connectionStatus_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);connectionStatus_->verticalHeader()->setDefaultSectionSize(44);connectionStatus_->setMaximumHeight(240);connectionStatus_->setMinimumHeight(110);
    auto* statusHeader=connectionStatus_->horizontalHeader();statusHeader->setSectionResizeMode(QHeaderView::ResizeToContents);statusHeader->setSectionResizeMode(1,QHeaderView::Stretch);
    connectionStatus_->hide();
    connect(statusToggle,&QPushButton::toggled,this,[this,statusToggle](bool open){connectionStatus_->setVisible(open);statusToggle->setText(open?tr("전체 연결 진단 접기 ▴"):tr("전체 연결 진단 ▾"));});
    connectionsLayout->addWidget(accountPanel_,1);
    connectionsLayout->addWidget(statusToggle,0,Qt::AlignLeft);
    connectionsLayout->addWidget(connectionStatus_);
    connectionsPage->setWidget(connectionsContent);
    pages->addTab(home,tr("내 자산"));pages->addTab(connectionsPage,tr("계좌·연결"));root->insertWidget(position,pages,1);
    connect(pages,&QTabWidget::currentChanged,this,[this]{flushPortfolioLoadingProjection();if(portfolioAssetsDirty_)renderPortfolioAssets();syncHeaderTabs();refreshTrust();syncConnectionLayout();if(portfolioDayView_->isVisible()||(todayDashboard_&&todayDashboard_->isVisible()))refreshDayHistory();});syncHeaderTabs();
    auto* stage = findChild<QLabel*>("stageBanner");
    stage->hide(); // Its sentence is now the page's empty state and tooltips.
    connect(retry,&QPushButton::clicked,this,[this]{const auto item=portfolioAssets_->currentIndex().siblingAtColumn(0);if(item.isValid())portfolio_->refreshConnection(item.data(Qt::UserRole).toString());});
    connect(portfolioAssets_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this]{refreshPortfolio();showPortfolioDetail();});
    // Narrow windows open the detail only on an explicit click or Enter, not on focus or arrow keys.
    // A click or Enter always fills the detail, also for a row that is already
    // current: a row can become current without a change signal.
    const auto openDetail=[this]{if(!portfolioDetail_->hasSelection())showPortfolioDetail();portfolioListDetail_->showDetail();syncPortfolioLayout();};
    connect(portfolioAssets_,&QTreeView::clicked,this,openDetail);
    connect(portfolioAssets_,&QTreeView::activated,this,openDetail);
    connect(portfolioDetail_,&AssetDetailPanel::backRequested,this,&LocalWindow::syncPortfolioLayout);
    connect(portfolioDetail_,&AssetDetailPanel::connectionsRequested,this,[this,pages]{pages->setCurrentWidget(connectionsPage_);});
    connect(portfolioAssets_,&QTreeView::doubleClicked,this,[this](const QModelIndex& index){const auto id=index.siblingAtColumn(0).data(Qt::UserRole).toString();
        if(connectionListModel_->rowForAccount(id)>=0)selectConnection(id);});
    connect(controller,&PortfolioController::changed,this,&LocalWindow::queuePortfolioPresentation);
    connect(controller,&PortfolioController::aboutToReset,this,[this]{
        const bool bootstrap=securityReady_&&accountReadsAvailable(vault_)&&portfolioInitialRefresh_&&!portfolioRestoreCaptured_&&hasSelectedPortfolioReadRegistration();
        const auto bootstrapStarted=portfolioInitialStartedMs_;
        resetPortfolioLoadProgress();
        portfolioAssetsDirty_=false;
        chartSourceResetting_=true;
        clearHoldingMarketSession();
        revokeKiwoomMarketAuthorization();
        revokeStockChartAuthorization();
        clearChartAlertPrivate();
        // Public chart metadata does not depend on this account generation.
        if(chartWorkspace_)chartWorkspace_->clearPrivate();
        portfolioDayView_->clear(tr("연결 정보를 확인하고 있습니다."));
        portfolioCachedView_={};portfolioDisplayView_={};portfolioRestoreCaptured_=false;portfolioInitialRefresh_=false;portfolioInitialStartedMs_=0;portfolioRevision_.clear();
        portfolioView_={};lastValuation_={};portfolioComplete_=false;
        portfolioTotal_->setText(QStringLiteral("—"));portfolioTotalUnit_->hide();portfolioTotalEvidence_->clear();portfolioEvidence_->setEntries({});portfolioEvidenceHasComparison_=false;
        portfolioCurrencyStatus_->clear();portfolioCurrencyStatus_->hide();portfolioCurrencyInfo_->hide();portfolioAllocation_->setGroups({});
        portfolioRefreshNote_->clear();portfolioRefreshNote_->hide();portfolioLoading_->hide();portfolioLoadingPane_->hide();portfolioPendingDetails_->hide();

        // Revocation cannot wait for a hidden page to become visible again.
        portfolioScopeGroups_.clear();
        {const QSignalBlocker block(portfolioAssets_->selectionModel());portfolioModel_->clearPrivate();}
        portfolioDetail_->clearDetail();portfolioListDetail_->showList();
        if(bootstrap){
            portfolioInitialRefresh_=true;portfolioInitialStartedMs_=bootstrapStarted;
            portfolioWorkbench_->hide();lockedState_->hide();homeBody_->show();
            portfolioLoading_->show();portfolioLoadingPane_->show();
        }
    });
    refreshSelection();refreshPortfolio();controller->setAutomaticRefresh(securityReady_);refreshDayHistory();
}

void LocalWindow::refreshDayHistory(bool request) {
    if (!portfolioDayView_ || !portfolio_) return;
    if (!securityReady_ || !accountReadsAvailable(vault_)) {
        portfolioDayView_->clear(tr("금고를 열면 표시")); return;
    }
    if (portfolioInitialRefresh_ || portfolioDisplayView_.asOfTimeMs > 0) {
        portfolioDayView_->clear(tr("현재 자산을 확인한 뒤 비교합니다.")); return;
    }
    const auto connections=portfolio_->connections();
    const bool reading=std::any_of(connections.cbegin(),connections.cend(),[](const auto& c){return !c.shared&&c.active;});
    // Coin and ledger notifications can arrive between the balance reply and
    // the one-second history tick. Recompose at both edges of the read so a
    // cached unavailable result does not collapse an already-settled flow.
    if (request || (!portfolio_->dayHistoryBusy() && reading!=portfolioDayView_->refreshPending()))
        portfolio_->requestDayHistory(valuationPreferences_ ? valuationPreferences_->excludedAssets() : QSet<QString>{});
    auto history = portfolio_->dayHistoryResult();
    const bool busy = portfolio_->dayHistoryBusy();
    const bool refreshing=busy||reading;
    if (busy) history.comparison.reason=tr("오늘 기록을 확인하고 있습니다…");
    else if (reading && !history.comparison.available) history.comparison.reason=tr("계좌 잔고를 다시 확인하고 있습니다…");
    portfolioDayView_->setHistory(history,portfolioMoneyDisplay_,refreshing);
    portfolioDayView_->setCoinPerformance(portfolio_->coinPerformance(),portfolioMoneyDisplay_);
    // Row projections publish their own evidence. A history-only refresh needs
    // to renumber it when comparison evidence is added, replaced or removed.
    if (portfolioEvidenceHasComparison_ || (!busy && history.comparison.available && history.baseline.available))
        refreshPortfolioEvidence();
}

void LocalWindow::syncExploreEvidence() {
    auto* overview=findChild<MarketOverviewView*>();
    if(overview)overview->setEvidenceStart(1);
    if(marketMetricsView_)marketMetricsView_->setEvidenceStart(overview?overview->evidenceCount()+1:1);
}
void LocalWindow::queueLegacyExploreEvidence() {
    if (exploreEvidenceQueued_) return;
    exploreEvidenceQueued_ = true;
    QTimer::singleShot(0,this,[this]{exploreEvidenceQueued_=false;refreshLegacyExploreEvidence();});
}
void LocalWindow::refreshLegacyExploreEvidence(bool clearingPrivate) {
    if (!clearingPrivate && windowShownOnce_ && (!exploreStack_ || !exploreStack_->isVisible() || isMinimized())) return;
    if (!exploreBoardEvidence_ || !exploreStack_ || !markets_) return;
    const int route = exploreStack_->currentIndex();
    // Market flow already owns its ordered overview/metric evidence book.
    if (route == 3) {exploreBoardEvidence_->setEntries({});exploreEvidenceScroll_->hide();return;}
    QList<ExploreMetricEvidence> entries;QHash<QString,int> assigned;
    QHash<QString,QList<int>> mapNumbers,watchNumbers,historyNumbers;QHash<QString,QString> mapStates;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto add = [&](ExploreMetricEvidence evidence) {
        if (evidence.receivedAtMs <= 0) return 0;
        // A board response is one actual observation, shared by its prices,
        // market caps, volumes and weighted change. No asset scope is invented.
        const auto key = evidence.id + QChar(0x1f) + evidence.provider + QChar(0x1f) +
            QString::number(evidence.sourceTimeMs) + QChar(0x1f) + QString::number(evidence.receivedAtMs);
        if (assigned.contains(key)) {
            const int number=assigned.value(key);
            if (entries[number-1].state!=evidence.state) entries[number-1].state=tr("미확인");
            return number;
        }
        const int number=entries.size()+1;assigned.insert(key,number);entries.append(evidence);return number;
    };
    QHash<QString, QSet<QString>> currentSymbols;
    const auto quoteEvidence = [&](const QString& section,const QString& id) -> ExploreMetricEvidence {
        const auto item=board_?board_->knownItem(section,id):PublicBoardItem{};
        if (!item.id.isEmpty()) {
            auto symbols = currentSymbols.constFind(section);
            if (symbols == currentSymbols.cend()) {
                QSet<QString> current;
                for (const auto& row : board_->items(section)) current.insert(row.symbol);
                symbols = currentSymbols.insert(section, std::move(current));
            }
            const bool stale = !board_->status(section).isEmpty() || now-item.receivedAtMs>120000 || !symbols->contains(id);
            const QString provider=section=="coin"?tr("CoinGecko"):section=="kr"?tr("네이버 증권"):tr("Nasdaq");
            return {"public:board:"+section,provider,{},0,item.receivedAtMs,!stale,stale?tr("지연"):tr("유효")};
        }
        if (section=="coin") for (const auto& row:markets_->rows()) if (row.base==id && row.priced)
            return publicMarketQuoteEvidence(row,now);
        return {};
    };
    const auto quoteRefs = [&](const QString& section,const QString& id) {
        const int number=add(quoteEvidence(section,id));return number>0?QList<int>{number}:QList<int>{};
    };
    const auto historyRefs = [&](const QString& section,const QString& id) {
        if (!marketHistory_) return QList<int>{};
        const auto history=marketHistory_->history(section,id);
        if (history.points.size()<2 || history.receivedAtMs<=0) return QList<int>{};
        QString provider;
        for (const auto& known:{QStringLiteral("CoinGecko"),QStringLiteral("업비트"),QStringLiteral("네이버 증권"),QStringLiteral("Nasdaq")})
            if (history.source==known || history.source.startsWith(known+" · ")) {provider=known;break;}
        // PublicHistory has sample times, but no response sourceTime field.
        // Keep that fact unavailable; the existing tooltip retains sample range.
        const int number=add({"public:history:"+section+":"+id,provider,{},0,history.receivedAtMs,true,history.status.isEmpty()?tr("유효"):tr("지연")});
        return number>0?QList<int>{number}:QList<int>{};
    };
    const auto append=[](QList<int>& values,const QList<int>& extra){for(int n:extra)if(!values.contains(n))values.append(n);};
    AssetDetailPanel* selected=nullptr;QString selectedSection,selectedId;
    if (route==0 && marketMap_) {
        // Section weighted changes appear before their constituent tile prices.
        for (const auto& section:{QStringLiteral("coin"),QStringLiteral("kr"),QStringLiteral("us")})
            if (marketMap_->filter()=="all" || marketMap_->filter()==section)
                for (const auto& item : board_ ? board_->items(section) : QList<PublicBoardItem>{}) {
                    const auto proof = quoteEvidence(section, item.symbol);
                    const int number = add(proof);
                    const auto key = section + ":" + item.symbol;
                    mapNumbers[key] = number > 0 ? QList<int>{number} : QList<int>{};
                    mapStates[key] = proof.state;
                }
        marketMap_->setEvidenceNumbers(mapNumbers);marketMap_->setEvidenceStates(mapStates);
        selected=mapDetail_;selectedSection=marketMap_->selectedSection();selectedId=marketMap_->selectedId();
    } else if (route==1) {
        auto shown=spotlightCards_;
        std::stable_sort(shown.begin(),shown.end(),[](const auto* a,const auto* b){return a->y()==b->y()?a->x()<b->x():a->y()<b->y();});
        for (auto* card:shown) if (!card->isHidden()) {
            QHash<QString,QList<int>> numbers;QHash<QString,QString> states,sources;
            for (const auto& item:card->viewportItems()){
                ExploreMetricEvidence proof;
                if (item.venue.isEmpty()) proof=quoteEvidence(card->key(),item.id);
                else {
                    const bool fresh=item.sourceTimeMs>0&&item.sourceTimeMs<=now&&now-item.sourceTimeMs<=120000&&
                        item.receivedAtMs>0&&item.receivedAtMs<=now&&now-item.receivedAtMs<=120000&&
                        (item.venue!="binance"||(board_&&board_->status("binance").isEmpty()));
                    proof={"ranking:"+item.venue+":"+item.id,item.venue=="upbit"?tr("업비트"):tr("바이낸스"),{},item.sourceTimeMs,item.receivedAtMs,fresh,fresh?tr("유효"):tr("지연")};
                }
                const auto ref=add(proof);if(ref>0)numbers[item.id]={ref};
                states[item.id]=proof.state;sources[item.id]=usabilitySourceTime(proof.provider,proof.sourceTimeMs);
            }
            card->setEvidenceNumbers(numbers);card->setEvidenceStates(states);card->setEvidenceSources(sources);
        }
        if (watchlist_) {
            for (const auto& id:watchlist_->visibleIds()) {
                watchNumbers[id]=quoteRefs(watchlist_->section(),id);
                historyNumbers[id]=historyRefs(watchlist_->section(),id);
            }
            watchlist_->setEvidenceNumbers(watchNumbers,historyNumbers);
        }
    } else if (route==2 && exploreTable_) {
        for (int i=0;i<exploreTable_->rowCount();++i) if (!exploreTable_->isRowHidden(i)) {
            const auto market=exploreTable_->item(i,0)->data(Qt::UserRole).toString();
            // Search is specifically the Upbit catalogue, even when a public
            // board also happens to contain the same ticker.
            QList<int> refs;
            for (const auto& row:markets_->rows()) if (row.market==market && row.priced) {
                const int n=add(publicMarketQuoteEvidence(row,now));if(n>0)refs.append(n);break;
            }
            QStringList text;for(int n:refs)text.append(QString("[%1]").arg(n));
            for (int column:{2,3}) if (auto* item=exploreTable_->item(i,column)) {
                item->setData(EvidenceNumbersRole,QVariant::fromValue(refs));item->setData(Qt::AccessibleDescriptionRole,item->text());
                const auto displayed=movementDecoratedText(item->text(),movementForSign(item->data(ChangeDirectionRole).toInt()));
                const int referenceWidth=0;
                item->setSizeHint({publicLedgerWidth(displayed)+referenceWidth,46});
            }
            if(i==exploreTable_->currentRow()){selected=exploreDetail_;selectedSection="coin";selectedId=market.mid(4);}
        }
        exploreTable_->viewport()->update();
    }
    if (selected && !selectedId.isEmpty()) {
        QList<int> refs;
        // The selected panel already has the exact typed quote provenance.
        // Its public card may use Upbit while another screen uses a board quote.
        if(auto* card=selected->findChild<ExploreMetricCard*>("assetDetailMetric")) {
            const auto evidence=card->data().evidence;
            for(auto entry:evidence) {
                if(entry.provider=="CoinGecko"||entry.provider=="네이버 증권"||entry.provider=="Nasdaq")entry.id="public:board:"+selectedSection;
                const int n=add(entry);if(n>0&&!refs.contains(n))refs.append(n);
            }
        }
        append(refs,historyRefs(selectedSection,selectedId));
        selected->setValuationEvidenceNumbers(refs);
    }
    exploreBoardEvidence_->setEntries(entries);
    exploreEvidenceScroll_->hide();
    exploreEvidenceScroll_->setFixedHeight(0);
}
void LocalWindow::refreshPortfolioEvidence() {
    if(!portfolioEvidence_||!portfolioTotalEvidence_)return;
    if(!securityReady_||!accountReadsAvailable(vault_)||!portfolio_||
        (portfolioInitialRefresh_&&portfolioCachedView_.groups.isEmpty())) {
        portfolioEvidenceHasComparison_=false;portfolioEvidence_->setEntries({});portfolioTotalEvidence_->clear();portfolioModel_->setEvidence({}, {});return;
    }
    QList<ExploreMetricEvidence> entries;QHash<QString,int> numbers;
    QHash<QString,QList<int>> groups,holdings;QList<int> comparison;
    const auto provider=[](const AssetEvidence& e,const QString& fallback) {
        const auto host=QUrl(e.url).host().toLower();
        static const QList<QPair<QString,QString>> hosts{{"upbit.com","업비트"},{"bithumb.com","빗썸"},{"binance.com","바이낸스"},{"binancefuture.com","바이낸스"},{"okx.com","OKX"},{"tossinvest.com","토스증권"},{"kiwoom.com","키움증권"},{"ls-sec.co.kr","LS증권"},{"koreainvestment.com","한국투자증권"},{"coinbase.com","Coinbase"},{"solana.com","Solana"}};
        for(const auto& pair:hosts)if(host==pair.first||host.endsWith('.'+pair.first))return pair.second;
        static const QHash<QString,QString> names{{"upbit","업비트"},{"bithumb","빗썸"},{"binance","바이낸스"},{"binance_usdm","바이낸스"},{"okx","OKX"},{"toss","토스증권"},{"kiwoom","키움증권"},{"ls","LS증권"},{"kis","한국투자증권"},{"phantom","Solana"}};
        return names.value(fallback,tr("원천 미제공"));
    };
    const auto add=[&](const AssetEvidence& evidence,const QString& fallback,bool reference=false) {
        if(evidence.source.isEmpty()&&evidence.sourceTimeMs==0&&evidence.receivedAtMs==0)return 0;
        const auto state=reference?tr("지연"):evidence.complete?tr("유효"):tr("불완전");
        const auto key=evidence.source+QChar(0x1f)+evidence.url+QChar(0x1f)+evidence.scope+QChar(0x1f)+QString::number(evidence.sourceTimeMs)+QChar(0x1f)+QString::number(evidence.receivedAtMs)+QString::number(evidence.complete);
        if(numbers.contains(key)){const int number=numbers.value(key);if(reference)entries[number-1].state=tr("지연");return number;}
        const int number=entries.size()+1;numbers.insert(key,number);
        entries.append({QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex()),provider(evidence,fallback),{},evidence.sourceTimeMs,evidence.receivedAtMs,evidence.complete,state});return number;
    };
    const auto append=[](QList<int>& target,int number){if(number>0&&!target.contains(number))target.append(number);};
    const auto addHolding=[&](const PortfolioGroupView& group,const PortfolioHoldingView& holding) {
        QList<int> refs;
        append(refs,add(holding.balanceEvidence,holding.provider));
        append(refs,add(holding.priceEvidence,{},holding.state==PortfolioValueState::Reference));
        for(const auto& leg:holding.conversionEvidence)append(refs,add(leg,{},holding.state==PortfolioValueState::Reference));
        for(int n:refs){append(groups[group.identity],n);append(holdings[portfolioHoldingKey(holding)],n);}
    };
    for(const auto& group:std::as_const(portfolioDisplayView_.groups))for(const auto& holding:group.holdings)
        if(holding.state==PortfolioValueState::Fresh&&!holding.excludedByUser)addHolding(group,holding);
    const auto history=portfolio_->dayHistoryResult();
    portfolioEvidenceHasComparison_=!portfolio_->dayHistoryBusy()&&history.comparison.available&&history.baseline.available;
    if(portfolioEvidenceHasComparison_) {
        const auto endpoint=[&](const AssetValuationResult& valuation,const QList<AssetSnapshot>& snapshots){
            for(const auto& snapshot:snapshots)append(comparison,add(snapshot.evidence,snapshot.provider));
            for(const auto& row:valuation.rows)if(row.included){
                for(const auto& snapshot:snapshots)if(snapshot.accountScope==row.accountScope)
                    for(const auto& component:snapshot.components)if(component.id==row.componentId)
                        append(comparison,add(component.evidence==AssetEvidence{}?snapshot.evidence:component.evidence,snapshot.provider));
                append(comparison,add(row.priceEvidence,{}));for(const auto& leg:row.conversionEvidence)append(comparison,add(leg,{}));
            }
        };
        endpoint(history.closingValuation,history.closingSnapshots);endpoint(history.baseline.valuation,history.baseline.usedSnapshots);
    } else comparison.clear();
    for(const auto& group:std::as_const(portfolioDisplayView_.groups))for(const auto& holding:group.holdings)
        if(holding.state!=PortfolioValueState::Fresh||holding.excludedByUser)addHolding(group,holding);
    const auto text=[](const QList<int>& refs){QStringList values;for(int n:refs)values.append(QString("[%1]").arg(n));return values.join(' ');};
    portfolioTotalEvidence_->clear();portfolioTotalEvidence_->hide();
    if(portfolioDayView_)portfolioDayView_->setEvidenceReferences(text(comparison));
    portfolioEvidence_->setEntries(entries);
    if(auto* scroll=findChild<QScrollArea*>("portfolioEvidenceScroll")){
        scroll->setFixedHeight(0);scroll->hide();
    }
    portfolioModel_->setEvidence(std::move(groups), std::move(holdings));
}

void LocalWindow::refreshPortfolioPresentation(){
    if(chartSourceResetting_&&accountReadsAvailable(vault_))
        for(const auto& connection:portfolio_->connections())if(connection.state=="complete"){chartSourceResetting_=false;break;}
    refreshPortfolio();
    if(!deferPortfolioLoadingProjection())refreshSelection();
    refreshChartWatchlists();refreshStockConnectionPresentation();
}
void LocalWindow::queuePortfolioPresentation(){
    if(!windowShownOnce_||!securityReady_||!accountReadsAvailable(vault_)||(portfolio_&&portfolio_->connections().isEmpty())){
        if(!securityReady_||!accountReadsAvailable(vault_)||(portfolio_&&portfolio_->connections().isEmpty()))resetPortfolioLoadProgress();
        refreshPortfolioPresentation();return;
    }
    if(portfolioPresentationQueued_)return;
    portfolioPresentationQueued_=true;const auto generation=portfolioPresentationGeneration_;
    QTimer::singleShot(0,this,[this,generation]{
        if(generation!=portfolioPresentationGeneration_)return;
        portfolioPresentationQueued_=false;refreshPortfolioPresentation();
    });
}
bool LocalWindow::hasSelectedPortfolioReadRegistration() const {
    if(!portfolio_)return false;
    const auto* environment=findChild<QComboBox*>("portfolioEnvironment");
    if(!environment)return false;
    const bool live=environment->currentData()=="live";const auto records=vault_.accounts();
    return std::any_of(records.cbegin(),records.cend(),[live](const auto& record){
        return !AccountModel::isProviderSuspended(record.provider)&&record.provider!="jupiter"&&((record.environment=="live"||record.environment=="mainnet")==live);
    });
}
bool LocalWindow::deferPortfolioLoadingProjection() const {
    if(!windowShownOnce_||!securityReady_||!accountReadsAvailable(vault_)||!portfolioInitialRefresh_||!portfolioCachedView_.groups.isEmpty())return false;
    const auto* pages=findChild<QTabWidget*>("portfolioPages");
    return pages&&pages->currentWidget()&&pages->currentWidget()->objectName()=="portfolioHome";
}
void LocalWindow::flushPortfolioLoadingProjection(){
    if (smallThresholdFilterPending_ && !portfolioAssetsDirty_ && isVisible() && !isMinimized()) {
        const auto* pages = findChild<QTabWidget*>("portfolioPages");
        if (pages && pages->currentWidget() && pages->currentWidget()->objectName() == "portfolioHome") applyPortfolioFilter();
    }
    if(!portfolioLoadingProjectionDirty_||deferPortfolioLoadingProjection())return;
    portfolioLoadingProjectionDirty_=false;
    refreshMoneyPresentation(true);refreshSelection();refreshExploreHoldings();refreshMarketMap();syncMarketFlowPrivateContext();
}
void LocalWindow::resetPortfolioLoadProgress(){
    if(portfolioExpiryTimer_)portfolioExpiryTimer_->stop();
    portfolioLoadingProjectionDirty_=false;
    ++portfolioPresentationGeneration_;portfolioPresentationQueued_=false;portfolioSettleQueued_=false;
    portfolioLoadConnections_.clear();portfolioLoadCompleted_.clear();
    portfolioLoadCaptured_=portfolioLoadPricesComplete_=portfolioLoadProjectionComplete_=false;
    portfolioStreamGraceStartedMs_=0;
    if(!portfolioLoadProgress_)return;
    portfolioLoadProgress_->resetSteps();
    portfolioLoadProgress_->setAccessibleName(usabilityText(UsabilityText::PortfolioLoadAccounts));portfolioLoadProgress_->setAccessibleDescription({});
    portfolioLoadStage_->setText(usabilityText(UsabilityText::PortfolioLoadAccounts));
    portfolioLoadTaskCount_->setText(usabilityText(UsabilityText::PortfolioLoadChecking));
}
void LocalWindow::updatePortfolioLoadProgress(const QList<PortfolioConnection>& states){
    if(!portfolioLoadProgress_)return;
    if(!portfolioLoadCaptured_&&portfolioInitialRefresh_&&!states.isEmpty()){
        for(const auto& state:states)portfolioLoadConnections_.insert(state.account.id);
        portfolioLoadCaptured_=true;
    }
    if(!portfolioLoadCaptured_)return;
    for(const auto& state:states)if(portfolioLoadConnections_.contains(state.account.id)&&portfolioLoadTerminal(state))
        portfolioLoadCompleted_.insert(state.account.id);
    const int total=int(portfolioLoadConnections_.size())+2;
    const int done=int(portfolioLoadCompleted_.size())+int(portfolioLoadPricesComplete_)+int(portfolioLoadProjectionComplete_);
    const auto stage=portfolioLoadCompleted_.size()<portfolioLoadConnections_.size()?UsabilityText::PortfolioLoadAccounts:
        !portfolioLoadPricesComplete_?UsabilityText::PortfolioLoadPrices:UsabilityText::PortfolioLoadPresentation;
    portfolioLoadStage_->setText(usabilityText(stage));portfolioLoadTaskCount_->setText(usabilityText(UsabilityText::PortfolioLoadTasks).arg(done).arg(total));
    portfolioLoadProgress_->setAccessibleName(usabilityText(stage));portfolioLoadProgress_->setAccessibleDescription(portfolioLoadTaskCount_->text());
    // Steps stay whole numbers of real tasks; the bar only eases its drawing between them.
    portfolioLoadProgress_->setSteps(done,total,done<total);
}

void LocalWindow::syncPortfolioAccountFilter(const QList<PortfolioConnection>& connections) {
    const bool live = findChild<QComboBox*>("portfolioEnvironment")->currentData() == "live";
    const QSignalBlocker guard(portfolioAccount_);
    const auto selected = portfolioAccount_->currentData().toString();
    int index = 1; // Index zero is the permanent "all accounts" option.
    for (const auto& connection : connections) {
        const auto& account = connection.account;
        if (AccountModel::isProviderSuspended(account.provider) || connection.shared || ((account.environment == "live" || account.environment == "mainnet") != live)) continue;
        const auto market = displayChoice(account.market);
        const auto label = account.alias + " · " + market;
        if (index == portfolioAccount_->count()) portfolioAccount_->addItem(label, account.id);
        else {
            if (portfolioAccount_->itemData(index) != account.id) portfolioAccount_->setItemData(index, account.id);
            if (portfolioAccount_->itemText(index) != label) portfolioAccount_->setItemText(index, label);
        }
        const auto tooltip = plainToolTip(account.alias + " · " + account.provider + " · " + market);
        if (portfolioAccount_->itemData(index, Qt::ToolTipRole) != tooltip)
            portfolioAccount_->setItemData(index, tooltip, Qt::ToolTipRole);
        ++index;
    }
    while (portfolioAccount_->count() > index) portfolioAccount_->removeItem(portfolioAccount_->count() - 1);
    portfolioAccount_->setCurrentIndex(qMax(0, portfolioAccount_->findData(selected)));
}

void LocalWindow::refreshPortfolio() {
    if(chartWorkspace_)chartWorkspace_->setPrivateEnabled(!chartPrivateBlocked_&&!chartSourceResetting_&&securityReady_&&accountReadsAvailable(vault_));
    if(!portfolio_||!portfolioSummary_)return;
    const bool readable=securityReady_&&accountReadsAvailable(vault_);
    findChild<QPushButton*>("portfolioPriceDetails")->setEnabled(readable&&!portfolio_->pricingStatus().isEmpty());
    const auto allStates=portfolio_->connections();
    auto* retry=findChild<QPushButton*>("retryPortfolioConnection");
    const auto selectedId=portfolioAssets_->currentIndex().siblingAtColumn(0).data(Qt::UserRole).toString();
    // Automatic reads skip failed accounts, so only those need a manual retry.
    bool canRetry=false;for(const auto& state:allStates)if(state.account.id==selectedId&&!state.active&&problemConnectionStates().contains(state.state))canRetry=true;
    retry->setEnabled(readable&&canRetry);retry->setVisible(retry->isEnabled());
    retry->setToolTip(tr("조회에 실패한 선택 계좌를 다시 조회합니다."));
    homeBody_->setVisible(readable);lockedState_->setVisible(!readable);
    if(!readable){
        resetPortfolioLoadProgress();
        portfolioAssetsDirty_=false;
        const bool exists=vault_.exists();
        findChild<QLabel*>("portfolioLockedTitle")->setText(!securityReady_?tr("PC 잠금 보호를 준비하지 못했어요"):exists?tr("금고가 잠겨 있어요"):tr("금고를 만들면 시작해요"));
        findChild<QLabel*>("portfolioLockedText")->setText(!securityReady_?tr("보호가 준비되기 전에는 금고를 열 수 없어요."):exists?tr("잠긴 동안에는 계좌·보유 정보를 화면에 두지 않아요.\n탐색의 공개 시세는 계속 볼 수 있어요."):tr("계좌 키는 이 PC의 암호화 금고에만 저장해요."));
        auto* action=findChild<QPushButton*>("portfolioLockedAction");action->setText(exists?tr("잠금 해제"):tr("금고 만들기"));action->setVisible(securityReady_);
        portfolioTotal_->setText(QStringLiteral("—"));portfolioTotalUnit_->hide();portfolioTotalEvidence_->clear();portfolioEvidence_->setEntries({});portfolioMoneyDisplay_={};portfolioCurrencyStatus_->clear();portfolioCurrencyStatus_->hide();portfolioCurrencyInfo_->hide();{auto* chips=portfolioGaps_->layout();while(auto* old=chips->takeAt(0)){delete old->widget();delete old;}}
        portfolioSummary_->setText(securityReady_?tr("금고를 열면 연결 계좌의 자산 조회를 시작합니다."):tr("PC 잠금 보호 준비 상태를 먼저 확인하세요."));portfolioProgress_->clear();{const QSignalBlocker block(portfolioAssets_->selectionModel());portfolioModel_->clearPrivate();}portfolioView_={};portfolioDisplayView_={};portfolioCachedView_={};portfolioRestoreCaptured_=false;portfolioInitialRefresh_=false;portfolioInitialStartedMs_=0;portfolioRefreshNote_->clear();portfolioRefreshNote_->hide();portfolioLoading_->hide();portfolioLoadingPane_->hide();portfolioPendingDetails_->hide();portfolioScopeGroups_.clear();portfolioAllocation_->setGroups({});
        {const QSignalBlocker guard(portfolioAccount_);portfolioAccount_->clear();portfolioAccount_->addItem(tr("전체 계좌"),QString{});}
        portfolioDetail_->clearDetail();portfolioListDetail_->showList();portfolioRevision_.clear();portfolioNextExpiryMs_=0;portfolioComplete_=false;lastValuation_={};applyPortfolioFilter();refreshConnectionStatus();refreshExploreHoldings();return;}
    const bool live=findChild<QComboBox*>("portfolioEnvironment")->currentData()=="live";
    const auto selected=[live](const QString& env){return (env=="live"||env=="mainnet")==live;};
    QList<PortfolioConnection> states;
    int suspendedAccounts = 0;
    for (const auto& connection : allStates) {
        if (!selected(connection.account.environment)) continue;
        if (AccountModel::isProviderSuspended(connection.account.provider)) ++suspendedAccounts;
        else states.append(connection);
    }
    QSet<QString> currentConnections;for(const auto& state:states)currentConnections.insert(state.account.id);
    auto snapshots=portfolio_->snapshots();snapshots.erase(std::remove_if(snapshots.begin(),snapshots.end(),[&](const auto& s){return !selected(s.environment)||!currentConnections.contains(s.connectionId);}),snapshots.end());
    QSet<QString> selectedScopes;for(const auto& s:snapshots)selectedScopes.insert(s.accountScope);
    const auto outsideScope=[&](const auto& p){return !selectedScopes.contains(p.evidence.scope)&&!p.evidence.scope.startsWith("public:");};
    auto prices=portfolio_->quotes();prices.erase(std::remove_if(prices.begin(),prices.end(),outsideScope),prices.end());
    auto references=portfolio_->referenceQuotes();references.erase(std::remove_if(references.begin(),references.end(),outsideScope),references.end());
    int completed=0,active=0;
    for(const auto& state:states){completed+=state.state=="complete";active+=state.active;}
    // Finish the visible day projection with the account reply. Waiting for
    // the one-second clock here also leaves a newly opened flow incomplete.
    if (!portfolioInitialRefresh_ && portfolioDayView_->isVisible() && !portfolio_->dayHistoryBusy() &&
        std::any_of(states.cbegin(),states.cend(),[](const auto& c){return !c.shared && c.active;}) != portfolioDayView_->refreshPending())
        refreshDayHistory();
    portfolioComplete_ = !states.isEmpty() && completed == states.size() &&
        std::all_of(snapshots.cbegin(), snapshots.cend(), [](const auto& s) { return s.balanceComplete; });
    // Per-account detail lives in 계좌·연결; the home keeps one status line plus pricing.
    const int problems=refreshConnectionStatus();
    portfolioProgress_->setText(tr("계좌 %1 / %2 갱신 완료 · %3개 조회 중%4\n%5").arg(completed).arg(states.size()).arg(active)
        .arg(problems?tr(" · 확인 필요 %1 · 계좌·연결에서 원인과 조치를 확인하세요").arg(problems):QString{},portfolio_->pricingStatus()));
    portfolioProgress_->setToolTip(tr("조회 결과는 이 PC의 금고와 암호화 원장에 보관합니다."));
    QString revision;for(const auto& s:snapshots)revision+=s.id+":"+s.connectionId+";";for(const auto& p:prices)revision+=p.baseAsset+p.quoteAsset+QString::number(p.evidence.receivedAtMs);
    for(const auto& p:references)revision+="reference:"+p.baseAsset+p.quoteAsset+QString::number(p.evidence.sourceTimeMs)+p.price;
    for(const auto& state:states)revision+=state.account.id+state.state;
    // Valuation changes only with new inputs or when a piece of evidence ages past the
    // freshness window; the summary and rows below always come from the same valuation.
    constexpr qint64 freshnessMs=DisplayFreshnessMs;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto excludedAssets = valuationPreferences_ ? valuationPreferences_->excludedAssets() : QSet<QString>{};
    auto excludedList = excludedAssets.values(); excludedList.sort();
    revision += ":excluded:" + excludedList.join(',') + ":suspended:" + QString::number(suspendedAccounts);
    QStringList scopeParts;
    for(const auto& state:states)scopeParts.append(state.account.id+":"+state.account.environment+":"+state.account.market);
    for(const auto& snapshot:snapshots)scopeParts.append(snapshot.connectionId+":"+snapshot.connectionBinding+":"+snapshot.accountScope);
    scopeParts.append("excluded:" + excludedList.join(','));
    scopeParts.sort();
    const auto scopeReceipt=QString::fromLatin1(QCryptographicHash::hash(scopeParts.join(QChar(0)).toUtf8(),QCryptographicHash::Sha256).toHex());
    if(!portfolioRestoreCaptured_&&!states.isEmpty()){
        portfolioRestoreCaptured_=true;portfolioInitialRefresh_=true;portfolioInitialStartedMs_=now;
        // Capture the encrypted restored inputs before a warm public cache or new
        // account response can change them. Current valuation stays independent.
        portfolioCachedView_={};
        // A disk cache is not proof of a successful first read in this run.
        // Only a previously completed, unchanged scope may use a complete saved view.
        if(!portfolioVerifiedScope_.isEmpty()&&portfolioVerifiedScope_==scopeReceipt){
            auto candidate=recordedPortfolio(snapshots,prices,states,now,excludedAssets);
            if(candidate.summary.complete)portfolioCachedView_=std::move(candidate);
        }
    }
    updatePortfolioLoadProgress(states);
    const bool readsPending=std::any_of(states.cbegin(),states.cend(),[](const auto& c){return !c.shared&&!portfolioLoadTerminal(c);});
    // REST prices are done and only first stream quotes are missing: wait a bounded
    // time, then open with the prices already received. A silent or halted market
    // must not hold the first screen forever; its asset stays unpriced until a quote arrives.
    const bool streamOnly=portfolioLoadCaptured_&&(portfolioInitialRefresh_||!portfolioLoadPricesComplete_)&&!readsPending&&!portfolio_->pricingBusy()&&portfolio_->pricingPending();
    if(!streamOnly)portfolioStreamGraceStartedMs_=0;
    else if(!portfolioStreamGraceStartedMs_){
        portfolioStreamGraceStartedMs_=QDateTime::currentMSecsSinceEpoch();
        const auto generation=portfolioPresentationGeneration_;
        QTimer::singleShot(portfolioStreamGraceMs_+1,this,[this,generation]{
            if(generation!=portfolioPresentationGeneration_||!portfolioStreamGraceStartedMs_)return;
            if(!securityReady_||!accountReadsAvailable(vault_))return;
            portfolioRevision_.clear();refreshPortfolio();
        });
    }
    const auto streamGraceOver=[this]{return portfolioStreamGraceStartedMs_>0&&QDateTime::currentMSecsSinceEpoch()-portfolioStreamGraceStartedMs_>=portfolioStreamGraceMs_;};
    if(portfolioLoadCaptured_&&(portfolioInitialRefresh_||!portfolioLoadPricesComplete_)&&!readsPending&&(!portfolio_->pricingPending()||streamGraceOver())&&!portfolioSettleQueued_){
        // A completed price batch may synchronously start a queued batch for
        // another account's newly discovered holdings. Check after that handoff.
        portfolioSettleQueued_=true;
        const auto generation=portfolioPresentationGeneration_;
        QTimer::singleShot(0,this,[this,generation]{
            if(generation!=portfolioPresentationGeneration_)return;
            portfolioSettleQueued_=false;
            if(!securityReady_||!accountReadsAvailable(vault_))return;
            const bool live=findChild<QComboBox*>("portfolioEnvironment")->currentData()=="live";
            const auto states=portfolio_->connections();
            const bool pending=std::any_of(states.cbegin(),states.cend(),[live](const auto& c){
                return ((c.account.environment=="live"||c.account.environment=="mainnet")==live)&&
                    !c.shared&&!portfolioLoadTerminal(c);
            });
            const bool graceOver=portfolioStreamGraceStartedMs_>0&&QDateTime::currentMSecsSinceEpoch()-portfolioStreamGraceStartedMs_>=portfolioStreamGraceMs_;
            if(pending||portfolio_->pricingBusy()||(portfolio_->pricingPending()&&!graceOver))return;
            portfolioStreamGraceStartedMs_=0;
            portfolioLoadPricesComplete_=true;updatePortfolioLoadProgress(states);
            portfolioInitialRefresh_=false;portfolioRevision_.clear();refreshPortfolioPresentation();
        });
    }
    const bool waitingLong=portfolioInitialRefresh_&&now-portfolioInitialStartedMs_>=15000;
    revision+=portfolioInitialRefresh_?":initial":":settled";
    if(waitingLong)revision+=":delayed";
    if(revision==portfolioRevision_&&now<portfolioNextExpiryMs_){refreshMoneyPresentation();return;}
    portfolioRevision_=revision;
    qint64 nextExpiry=std::numeric_limits<qint64>::max();
    const auto expiresAt=[&](qint64 at,qint64 maxAge){
        if(at<=0)return;
        // Future evidence is ineligible now, but can become usable at its
        // original timestamp without a new response or revision.
        if(at>now){nextExpiry=qMin(nextExpiry,at);return;}
        if(at>std::numeric_limits<qint64>::max()-maxAge-1)return;
        const auto expiry=at+maxAge+1;
        if(expiry>now)nextExpiry=qMin(nextExpiry,expiry);
    };
    const auto evidenceExpiry=[&](const AssetEvidence& e,qint64 maxAge){expiresAt(e.receivedAtMs,maxAge);expiresAt(e.sourceTimeMs,maxAge);};
    for(const auto& s:snapshots){expiresAt(s.timeMs,freshnessMs);evidenceExpiry(s.evidence,freshnessMs);for(const auto& c:s.components)evidenceExpiry(c.evidence,freshnessMs);}
    // Both cached references and current quotes can supply reference conversion legs.
    for(const auto& p:prices){evidenceExpiry(p.evidence,freshnessMs);evidenceExpiry(p.evidence,ReferenceMaxAgeMs);}
    for(const auto& p:references)evidenceExpiry(p.evidence,ReferenceMaxAgeMs);
    if(portfolioInitialRefresh_&&!waitingLong)nextExpiry=qMin(nextExpiry,portfolioInitialStartedMs_+15000);
    portfolioNextExpiryMs_=nextExpiry;
    if(nextExpiry==std::numeric_limits<qint64>::max())portfolioExpiryTimer_->stop();
    else portfolioExpiryTimer_->start(int(std::clamp<qint64>(nextExpiry - now, 1, std::numeric_limits<int>::max())));
    const auto valuation=AssetValuation::value(snapshots,prices,"KRW",now,freshnessMs,references,excludedAssets);
    portfolioView_=presentPortfolio(snapshots,valuation,states);
    if(!portfolioInitialRefresh_&&!portfolio_->pricingPending()&&portfolioView_.summary.complete)portfolioVerifiedScope_=scopeReceipt;
    portfolioDisplayView_=portfolioInitialRefresh_&&!portfolioCachedView_.groups.isEmpty()?portfolioCachedView_:portfolioView_;
    const bool loading=portfolioInitialRefresh_&&portfolioCachedView_.groups.isEmpty();
    const bool recorded=portfolioDisplayView_.asOfTimeMs>0;
    portfolioLoading_->setText(usabilityText(problems?UsabilityText::PortfolioLoadProblem:waitingLong?UsabilityText::PortfolioLoadDelayed:UsabilityText::PortfolioLoadTitle));
    portfolioLoading_->setVisible(loading);portfolioLoadingPane_->setVisible(loading);portfolioWorkbench_->setVisible(!loading);
    portfolioPendingDetails_->setVisible(loading&&(waitingLong||problems));
    portfolioListDetail_->setVisible(!loading);portfolioAllocation_->setVisible(!loading);
    portfolioRefreshNote_->setVisible(recorded);
    portfolioRefreshNote_->setText(recorded?tr("마지막 확인 · %1 기준 · 업데이트 중").arg(QDateTime::fromMSecsSinceEpoch(portfolioDisplayView_.asOfTimeMs).toString("yyyy-MM-dd HH:mm:ss")):
        QString{});
    const auto& summary=portfolioDisplayView_.summary;
    QStringList gaps;
    if(summary.unvaluedGroups&&!(hideUnvalued_&&hideUnvalued_->isChecked()))gaps.append(tr("가격 미확인 %1종").arg(summary.unvaluedGroups));
    if(summary.referenceGroups)gaps.append(summary.referenceTotal.isEmpty()
        ? tr("참고 평가 %1종 · 합계 자료 부족 · 합계 제외").arg(summary.referenceGroups)
        : tr("참고 평가 %1종 · 참고 %2 KRW는 합계 제외").arg(summary.referenceGroups).arg(groupedAmount(summary.referenceTotal)));
    if(!summary.missingAccounts.isEmpty())gaps.append(tr("미확인 계좌 %1 · 포함 자산 수 알 수 없음").arg(summary.missingAccounts.join(", ")));
    if(!summary.readingAccounts.isEmpty())gaps.append(tr("조회 중 %1").arg(summary.readingAccounts.join(", ")));
    // Never show 0 as a total when nothing could be valued yet: that reads as "no money".
    const bool hasFigure=hasPortfolioFigure(portfolioDisplayView_);
    const auto exactFigure = summary.complete ? summary.netAssets : summary.knownSubtotal;
    const bool validFigure = decimal::valid(exactFigure.startsWith('-') ? exactFigure.mid(1) : exactFigure);
    const auto krwFigure=loading?QStringLiteral("—"):hasFigure&&!validFigure?tr("자료 부족"):hasFigure?krwAmount(exactFigure):QStringLiteral("—");
    if(portfolioTotal_->property("quiet")!=QVariant(!hasFigure)){
        portfolioTotal_->setProperty("quiet",!hasFigure);
        portfolioTotal_->style()->unpolish(portfolioTotal_);portfolioTotal_->style()->polish(portfolioTotal_);
    }
    findChild<QLabel*>("portfolioTotalLabel")->setText(summary.excludedGroups
        ? (recorded ? tr("마지막 확인 평가 대상 합계") : summary.complete ? tr("평가 대상 합계") : tr("평가 대상 부분합"))
        : recorded?(summary.complete?tr("마지막 확인 자산"):tr("마지막 확인 평가액")):summary.complete?tr("순자산"):tr("확인된 자산 부분합"));
    lastValuation_={now,states,snapshots,prices,references,valuation,portfolioView_,
        findChild<QLabel*>("portfolioTotalLabel")->text(),krwFigure,recorded,excludedAssets,{}};
    for (const auto& comparison : portfolio_->providerReconciliations(now))
        if (std::any_of(states.cbegin(), states.cend(), [&](const auto& state) { return state.account.id == comparison.official.connectionId; }))
            lastValuation_.providerReconciliations.append(comparison);
    findChild<QLabel*>("portfolioTotalLabel")->setToolTip(tr("전체 연결 계좌 기준입니다. 목록 필터와 무관하며 미평가·참고 평가·미확인 계좌는 합계에서 제외합니다."));
    if(!loading){
        auto* chips=portfolioGaps_->layout();
        while(auto* old=chips->takeAt(0)){delete old->widget();delete old;}
        const auto chip=[this,chips](const QString& text,const QString& tone,bool toAccounts){
            auto* item=new QPushButton(text,portfolioGaps_);item->setObjectName("portfolioGapChip");item->setProperty("disclose",true);Q_UNUSED(tone);
            item->setToolTip(toAccounts?tr("계좌·연결에서 원인과 조치를 확인합니다."):tr("이 항목은 확인된 합계에 넣지 않았습니다."));
            if(toAccounts)connect(item,&QPushButton::clicked,this,[this]{if(auto* p=findChild<QTabWidget*>("portfolioPages"))p->setCurrentWidget(connectionsPage_);});
            else item->setFocusPolicy(Qt::TabFocus);
            chips->addWidget(item);};
        if (suspendedAccounts) chip(tr("비활성 계좌 %1개 · 평가 제외").arg(suspendedAccounts), "muted", true);
        if(!summary.missingAccounts.isEmpty())chip(tr("미확인 계좌 %1").arg(summary.missingAccounts.size()),"warn",true);
        // Background re-reads every 20 s are silent; only accounts still waiting for their first data show.
        int firstReads=0;for(const auto& m:portfolioDisplayView_.missing)firstReads+=m.active;
        if(firstReads)chip(tr("조회 중 %1").arg(firstReads),"info",true);
        if(summary.referenceGroups)chip(tr("참고 평가 %1종").arg(summary.referenceGroups),"muted",false);
        if(summary.unvaluedGroups&&!(hideUnvalued_&&hideUnvalued_->isChecked()))chip(tr("가격 미확인 %1종").arg(summary.unvaluedGroups),"muted",false);
        if(summary.excludedGroups)chip(tr("평가 제외 %1종").arg(summary.excludedGroups),"warn",false);
    }
    const auto subtotalText = !hasFigure||summary.knownSubtotal.isEmpty() ? QStringLiteral("—") : groupedAmount(summary.knownSubtotal) + " KRW";
    portfolioSummary_->setText(summary.complete?tr("조회 범위 순자산  %1 KRW").arg(groupedAmount(summary.netAssets)):
        tr("확인된 평가액 부분합  %1 · 순자산 확인 미완료").arg(subtotalText)+
        (gaps.isEmpty()?tr("\n빠진 범위 · 잔고·평가 시각 또는 수량 근거를 확인하세요"):tr("\n빠진 범위 · %1").arg(gaps.join(" · "))));
    portfolioGaps_->setVisible(!portfolioInitialRefresh_||recorded);
    if(!loading)syncPortfolioAccountFilter(allStates);
    refreshMoneyPresentation(true);
    refreshTrust();
    if(deferPortfolioLoadingProjection()){portfolioLoadingProjectionDirty_=true;return;}
    portfolioLoadingProjectionDirty_=false;
    refreshExploreHoldings();refreshMarketMap();syncMarketFlowPrivateContext();
}

PortfolioMoneyDisplay LocalWindow::currentMoneyDisplay(qint64 asOfMs) const {
    if(moneyCurrency_==MoneyCurrency::Krw)return {};
    auto quotes=portfolio_?portfolio_->quotes():QList<AssetQuote>{};
    if(markets_)quotes.append(markets_->quotes());
    quotes.append(portfolioDisplayFx_.quotes(asOfMs));
    return PortfolioMoneyDisplay::fromQuotes(moneyCurrency_,quotes,asOfMs);
}

void LocalWindow::refreshMoneyPresentation(bool force) {
    if(!portfolio_||!portfolioTotal_||!portfolioCurrencyStatus_)return;
    if(!securityReady_||!accountReadsAvailable(vault_)){
        portfolioMoneyDisplay_={};portfolioCurrencyStatus_->clear();portfolioCurrencyStatus_->hide();portfolioCurrencyInfo_->hide();return;
    }
    const auto display=currentMoneyDisplay(QDateTime::currentMSecsSinceEpoch());
    const bool changed=display!=portfolioMoneyDisplay_;
    if(!force&&!changed)return;
    portfolioMoneyDisplay_=display;
    const auto& summary=portfolioDisplayView_.summary;
    const bool hasFigure=hasPortfolioFigure(portfolioDisplayView_);
    const bool loading=portfolioInitialRefresh_&&portfolioCachedView_.groups.isEmpty();
    const auto value=summary.complete?summary.netAssets:summary.knownSubtotal;
    const bool valid=decimal::valid(value.startsWith('-')?value.mid(1):value);
    portfolioTotal_->setText(loading?QStringLiteral("—"):hasFigure&&!valid?tr("자료 부족"):hasFigure?display.amount(value):QStringLiteral("—"));
    portfolioTotalUnit_->setText(display.unit());portfolioTotalUnit_->setVisible(hasFigure&&valid&&!loading);
    portfolioCurrencyStatus_->setText(display.status());portfolioCurrencyStatus_->setVisible(hasFigure&&valid&&!loading&&!display.status().isEmpty());
    portfolioCurrencyStatus_->setToolTip(display.information());
    portfolioCurrencyInfo_->setInformation(usabilityText(UsabilityText::MoneyCurrencyLabel),display.information());portfolioCurrencyInfo_->setVisible(hasFigure&&valid&&!loading&&moneyCurrency_==MoneyCurrency::Usd);
    portfolioModel_->setHeaderData(3,Qt::Horizontal,usabilityText(UsabilityText::MoneyValuationColumn).arg(display.unit()));
    if(deferPortfolioLoadingProjection()){
        portfolioLoadingProjectionDirty_=true;portfolioAssetsDirty_=true;return;
    }
    renderPortfolioAssets();if(changed)refreshDayHistory(false);
}

void LocalWindow::renderPortfolioAssets() {
    if(!portfolioAssets_||!portfolioAccount_||!portfolioClass_)return;
    // Keep the first pre-show projection eager. Once shown, hidden pages consume
    // current source/valuation DTOs but defer their row/layout work until shown.
    if(windowShownOnce_&&securityReady_&&accountReadsAvailable(vault_)){
        const auto* pages=findChild<QTabWidget*>("portfolioPages");
        const bool home=pages&&pages->currentWidget()&&pages->currentWidget()->objectName()=="portfolioHome";
        if(!isVisible()||isMinimized()||!home||(portfolioInitialRefresh_&&portfolioCachedView_.groups.isEmpty())){portfolioAssetsDirty_=true;return;}
    }
    portfolioAssetsDirty_=false;
    portfolioAllocation_->setGroups(portfolioDisplayView_.groups,portfolioMoneyDisplay_);
    const int scrollPosition=portfolioAssets_->verticalScrollBar()->value();
    const auto assetClass=portfolioClass_->currentData().toString()=="all"?QString{}:portfolioClass_->currentData().toString();
    const auto connection=portfolioAccount_->currentData().toString();
    portfolioScopeGroups_ = assetClass.isEmpty() && connection.isEmpty()
        ? portfolioDisplayView_.groups : scopedPortfolioGroups(portfolioDisplayView_.groups,assetClass,connection);
    QList<PortfolioMissingView> missing;
    for (const auto& row : portfolioDisplayView_.missing) {
        if (!connection.isEmpty() && row.connectionId != connection) continue;
        const auto kind = row.market == "domestic_equity" ? QStringLiteral("stock") : QStringLiteral("coin");
        if (!assetClass.isEmpty() && assetClass != "cash" && assetClass != kind) continue;
        missing.append(row);
    }
    const QSignalBlocker selectionSignals(portfolioAssets_->selectionModel());
    portfolioModel_->setSnapshot(portfolioScopeGroups_, missing, portfolioMoneyDisplay_, portfolioAllocation_->total(),
                                 hideUnvalued_ && hideUnvalued_->isChecked());
    applyPortfolioFilter();
    if (!missing.isEmpty()) portfolioAssets_->expand(portfolioModel_->index(portfolioModel_->rowCount()-1,0));
    refreshPortfolioEvidence();
    showPortfolioDetail();
    if (!portfolioAssets_->currentIndex().isValid()) portfolioListDetail_->showList();
    syncPortfolioLayout();

    // Bound sizing work to a small sample. Other rows still provide their full
    // text on selection, expansion, scrolling and accessibility queries.
    const auto font = portfolioAssets_->fontMetrics();
    for (int col : {2, 3}) {
        int width = font.horizontalAdvance(portfolioModel_->headerData(col, Qt::Horizontal).toString()) + 32;
        for (int i = 0; i < qMin(12, portfolioModel_->rowCount()); ++i) {
            auto group = portfolioModel_->index(i, 0);
            width = qMax(width, font.horizontalAdvance(group.siblingAtColumn(col).data().toString()) + 32);
            if (portfolioAssets_->isExpanded(group) && portfolioModel_->rowCount(group))
                width = qMax(width, font.horizontalAdvance(portfolioModel_->index(0,col,group).data().toString()) + 32);
        }
        width = qBound(92, width, col == 2 ? 170 : 300);
        if (width != portfolioAssets_->columnWidth(col)) portfolioAssets_->setColumnWidth(col,width);
    }
    if (scrollPosition > 0 && portfolioAssets_->verticalScrollBar()->value() != scrollPosition)
        portfolioAssets_->verticalScrollBar()->setValue(scrollPosition);
    if(portfolioLoadCaptured_&&portfolioLoadPricesComplete_&&!portfolioLoadProjectionComplete_){portfolioLoadProjectionComplete_=true;updatePortfolioLoadProgress({});}
}

void LocalWindow::applyPortfolioFilter(bool thresholdOnly) {
    if (!portfolioFilter_ || !portfolioAssets_) return;
    // Compare the catalogue and portfolio copies of the verified KRW-USDT quote.
    const auto now = QDateTime::currentMSecsSinceEpoch();
    QString price;
    qint64 latest = 0;
    bool conflict = false;
    smallThresholdTransitionMs_ = 0;
    const auto checkAt = [&](qint64 at) {
        if (!smallThresholdTransitionMs_ || at < smallThresholdTransitionMs_) smallThresholdTransitionMs_ = at;
    };
    const auto accept = [&](const AssetQuote& quote) {
        const auto& e = quote.evidence;
        if (quote.baseAsset != "USDT" || quote.quoteAsset != "KRW" || e.scope != "public:upbit:KRW-USDT" ||
            !e.complete || !decimal::positive(quote.price) || e.sourceTimeMs <= 0 || e.receivedAtMs <= 0) return;
        const auto oldest = qMin(e.sourceTimeMs, e.receivedAtMs);
        const auto newest = qMax(e.sourceTimeMs, e.receivedAtMs);
        if (now - oldest > DisplayFreshnessMs) return;
        if (newest > now) { checkAt(newest); return; }
        checkAt(oldest + DisplayFreshnessMs + 1);
        if (e.sourceTimeMs > latest) {
            latest = e.sourceTimeMs;
            price = quote.price;
            conflict = false;
        } else if (e.sourceTimeMs == latest && decimal::normalize(price) != decimal::normalize(quote.price)) {
            conflict = true;
        }
    };
    if (portfolio_) for (const auto& quote : portfolio_->quotesForPair("USDT", "KRW")) accept(quote);
    if (markets_) accept(markets_->quoteForMarket("KRW-USDT"));
    if (conflict) price.clear();
    const bool changed = smallThresholdKrw_ != price;
    smallThresholdKrw_ = price;
    if (thresholdOnly && !changed) return;
    if (hideSmall_) {
        hideSmall_->setEnabled(!smallThresholdKrw_.isEmpty());
        hideSmall_->setToolTip(!smallThresholdKrw_.isEmpty()
            ? tr("1 USDT(현재 약 %1원) 미만인 자산을 목록에서 숨깁니다. 미평가 자산은 유지하며 총 평가액은 변하지 않습니다.")
                  .arg(krwAmount(smallThresholdKrw_))
            : tr("1 USDT의 원화 가격을 아직 받지 못해 소액 기준을 정할 수 없어요."));
    }
    if (thresholdOnly) {
        if (!hideSmall_ || !hideSmall_->isChecked()) { smallThresholdFilterPending_ = false; return; }
        const auto* pages = findChild<QTabWidget*>("portfolioPages");
        if (windowShownOnce_ && (!isVisible() || isMinimized() || !pages || !pages->currentWidget() || pages->currentWidget()->objectName() != "portfolioHome")) {
            smallThresholdFilterPending_ = true;
            return;
        }
    }
    smallThresholdFilterPending_ = false;
    const bool hideSmall = hideSmall_ && hideSmall_->isChecked() && !smallThresholdKrw_.isEmpty();
    PortfolioPresentation scoped;scoped.groups=portfolioScopeGroups_;scoped.summary=portfolioDisplayView_.summary;
    const auto dust=portfolioDust(scoped,smallThresholdKrw_,hideSmall);
    int visible = 0, held = 0, unknown = 0, hiddenUnvalued = 0;
    const auto criteria=portfolioFilter_->criteria();
    const QSet<QString> hiddenDust(dust.hiddenIdentities.cbegin(), dust.hiddenIdentities.cend());
    for (int i = 0; i < portfolioModel_->rowCount(); ++i) {
        const auto group = portfolioModel_->index(i, 0);
        const bool small = hiddenDust.contains(group.data(Qt::UserRole + 1).toString());
        bool anyShown = false, hasHiddenPrice = false;
        for (int child = 0; child < portfolioModel_->rowCount(group); ++child) {
            const auto row = portfolioModel_->index(child,0,group);
            const auto presence = AssetPresence(row.data(PortfolioAssetModel::PresenceRole).toInt());
            held += presence == AssetPresence::Held;
            unknown += presence == AssetPresence::NeedsCheck;
            const auto state = row.data(PortfolioAssetModel::ValueRole);
            const bool priceHidden = hideUnvalued_ && hideUnvalued_->isChecked() && state.isValid() && state.toInt() == int(PortfolioValueState::Unvalued);
            const auto label = criteria.search.isEmpty() ? QString{} : row.data(PortfolioAssetModel::SearchRole).toString();
            const bool matches = criteria.matches(label, presence);
            const bool show = matches && !priceHidden;
            if (portfolioAssets_->isRowHidden(child,group) == show) portfolioAssets_->setRowHidden(child,group,!show);
            anyShown |= show; hasHiddenPrice |= matches && priceHidden;
            visible += show && !small;
        }
        hiddenUnvalued += hasHiddenPrice;
        const bool hidden = !anyShown || small;
        if (portfolioAssets_->isRowHidden(i,{}) != hidden) portfolioAssets_->setRowHidden(i,{},hidden);
    }
    if(hideUnvalued_)hideUnvalued_->setText(hideUnvalued_->isChecked()?tr("미평가 숨김 · %1").arg(hiddenUnvalued):tr("미평가 숨기기"));
    if (hideSmall_) {
        hideSmall_->setText(hideSmall ? tr("소액 숨김") : tr("소액 숨기기"));
        if (hideSmall) hideSmall_->setToolTip(hideSmall_->toolTip()+tr("\n현재 목록에서 숨긴 소액 %1원\n소액 제외 후 합계 %2원").arg(krwAmount(dust.hidden),krwAmount(dust.displayed)));
    }
    const auto current=portfolioAssets_->currentIndex();
    if(current.isValid() && (portfolioAssets_->isRowHidden(current.row(),current.parent()) ||
        (current.parent().isValid() && portfolioAssets_->isRowHidden(current.parent().row(),{})))) {
        portfolioAssets_->setCurrentIndex({});portfolioDetail_->clearDetail();portfolioListDetail_->showList();
    }
    if(auto* empty=findChild<QLabel*>("portfolioListEmpty"))
        empty->setVisible(visible==0&&!(portfolioInitialRefresh_&&portfolioCachedView_.groups.isEmpty()));
    syncPortfolioLayout();
    portfolioFilter_->setCounts(visible, held, unknown, portfolioComplete_);
}

void LocalWindow::showPortfolioDetail() {
    if (!portfolioDetail_) return;
    const auto syncDetail = qScopeGuard([this] { portfolioListDetail_->refreshMode(); });
    const auto item = portfolioAssets_->currentIndex().siblingAtColumn(0);
    if (!item.isValid()) { portfolioDetail_->clearDetail(); return; }
    const auto group = item.parent().isValid() ? item.parent() : item;
    const auto identity = group.data(Qt::UserRole + 1).toString();
    if (identity == "missing") {
        const auto id = item.data(Qt::UserRole).toString();
        for (const auto& missing : portfolioDisplayView_.missing) if (missing.connectionId == id) { portfolioDetail_->showMissing(missing); return; }
        portfolioDetail_->clearDetail(); return;
    }
    if(const auto selected=selectedPortfolioGroup()){
        portfolioDetail_->showGroup(*selected,portfolioAllocationTotal(portfolioDisplayView_.groups),portfolioMoneyDisplay_,!(hideUnvalued_&&hideUnvalued_->isChecked()));
        if(selected->identity=="solana:native"&&std::any_of(selected->holdings.cbegin(),selected->holdings.cend(),[](const auto& holding){return holding.provider=="phantom";}))
            if(auto* note=portfolioDetail_->findChild<QLabel*>("assetDetailHeadlineSub")){
                const auto scope=tr("Phantom은 지갑에 바로 있는 SOL만 읽어요. 스테이킹한 SOL은 아직 포함하지 않아요. jitoSOL 같은 스테이킹 토큰은 별도 토큰으로 보여요.");
                note->setText(note->text().isEmpty()?scope:note->text()+"\n"+scope);note->show();
            }
        portfolioDetail_->setValuationEvidenceNumbers(item.siblingAtColumn(3).data(EvidenceNumbersRole).value<QList<int>>());return;
    }
    portfolioDetail_->clearDetail();
}

std::optional<PortfolioGroupView> LocalWindow::selectedPortfolioGroup() const {
    if(!portfolioAssets_)return {};
    const auto item=portfolioAssets_->currentIndex().siblingAtColumn(0);if(!item.isValid())return {};
    const auto root=item.parent().isValid()?item.parent():item;
    const auto key=root.data(Qt::UserRole+1).toString();
    for(const auto& group:portfolioScopeGroups_)if(group.identity==key){
        if(!item.parent().isValid())return group;
        const auto selected=item.data(PortfolioAssetModel::RowKeyRole).toString();
        for(const auto& holding:group.holdings)if(portfolioHoldingKey(holding)==selected){
            auto single=group;single.holdings={holding};const auto scoped=scopedPortfolioGroups({single},{},{});
            if(!scoped.isEmpty())return scoped.first();
        }
    }
    return {};
}
void LocalWindow::clearAssetShare(){
    ++assetShareGeneration_;
    const auto previous=assetShareDialog_;assetShareDialog_=nullptr;
    if(chartStudyFeed_)chartStudyFeed_->clearOwner("asset-share");
    assetShareValue_={};assetShareKey_.clear();assetShareProviders_.clear();
    if(previous){previous->clearPersonalValues();previous->reject();previous->deleteLater();}
}
void LocalWindow::openAssetShare(){
    if(!portfolio_||chartPrivateBlocked_||chartSourceResetting_||!securityReady_||!accountReadsAvailable(vault_))return;
    const auto selected=selectedPortfolioGroup();if(!selected||selected->holdings.isEmpty())return;
    clearAssetShare();assetShareKey_=selected->identity;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto current=[now](qint64 at){return at>0&&at<=now&&now-at<=DisplayFreshnessMs;};
    const auto evidenceCurrent=[&](const AssetEvidence& evidence){return evidence.complete&&current(evidence.receivedAtMs)&&
        (evidence.sourceTimeMs==0||current(evidence.sourceTimeMs));};
    const auto connections=portfolio_->connections();
    bool balanceComplete=!portfolioInitialRefresh_&&current(lastValuation_.asOfMs),valuationComplete=balanceComplete;
    AssetSharePersonalValues personal;personal.quantity="0";personal.currentValuation="0";personal.currency="KRW";
    personal.observedAtMs=lastValuation_.asOfMs;personal.todayUnavailableReason=tr("같은 자산·보관처의 오늘 관측 증감을 확인할 수 없어요.");
    QSet<QString> selectedRows;
    for(const auto& holding:selected->holdings){
        if(!assetShareProviders_.contains(holding.provider))assetShareProviders_.append(holding.provider);
        const auto connection=std::find_if(connections.cbegin(),connections.cend(),[&](const auto& item){return !item.shared&&
            item.account.id==holding.connectionId&&item.accountScope==holding.accountScope;});
        const bool fresh=connection!=connections.cend()&&connection->state=="complete"&&current(connection->receivedAtMs)&&
            holding.kind==AssetComponentKind::Holding&&holding.asOfTimeMs==0&&current(holding.balanceTimeMs)&&
            evidenceCurrent(holding.balanceEvidence)&&decimal::positive(holding.quantity);
        balanceComplete&=fresh;
        valuationComplete&=fresh&&holding.state==PortfolioValueState::Fresh&&!holding.excludedByUser&&
            decimal::valid(holding.value)&&evidenceCurrent(holding.priceEvidence)&&
            std::all_of(holding.conversionEvidence.cbegin(),holding.conversionEvidence.cend(),evidenceCurrent);
        if(decimal::valid(holding.quantity))personal.quantity=decimal::add(personal.quantity,holding.quantity);
        if(decimal::valid(holding.value))personal.currentValuation=decimal::add(personal.currentValuation,holding.value);
        selectedRows.insert(holding.accountScope+QChar(0)+holding.componentId);
    }
    personal.custodyProviders=assetShareProviders_;personal.valuationComplete=valuationComplete;
    if(!valuationComplete)personal.currentValuation.clear();
    // The cached comparison is an observation, never a holding return. It must
    // prove this exact selected row set and the original two endpoint identities.
    if(balanceComplete&&valuationComplete){
        const auto history=portfolio_->dayHistoryResult();const auto& comparison=history.contributions.comparison;
        if(history.contributions.available&&comparison.available&&comparison.complete&&comparison.currency==personal.currency&&
           comparison.fromMs>=portfolioHistoryDayStart(now)&&comparison.fromMs<comparison.toMs&&
           comparison.toMs<=personal.observedAtMs&&current(comparison.toMs)){
            QString positive="0",negative="0",closing="0";QSet<QString> matched;bool proven=true;
            const auto hasKey=[&](const QList<AssetSnapshot>& snapshots,const PortfolioContributionRow& row){
                int matches=0;for(const auto& snapshot:snapshots)if(snapshot.accountScope==row.accountScope)
                    for(const auto& component:snapshot.components)if(component.id==row.componentId&&component.asset==row.asset&&
                        assetIdentity(snapshot,component)==assetShareKey_)++matches;
                return matches==1;
            };
            for(const auto& row:history.contributions.rows){const auto key=row.accountScope+QChar(0)+row.componentId;
                if(!selectedRows.contains(key))continue;
                const auto magnitude=row.difference.startsWith('-')?row.difference.mid(1):row.difference;
                if(matched.contains(key)||!hasKey(history.baseline.usedSnapshots,row)||!hasKey(history.closingSnapshots,row)||
                   !decimal::valid(magnitude)||!decimal::valid(row.closingValue)){proven=false;break;}
                matched.insert(key);
                if(row.difference.startsWith('-'))negative=decimal::add(negative,magnitude);else positive=decimal::add(positive,magnitude);
                closing=decimal::add(closing,row.closingValue);
            }
            if(proven&&matched==selectedRows&&closing==personal.currentValuation){
                const auto direction=decimal::compare(positive,negative);
                const auto delta=direction<0?"-"+decimal::subtract(negative,positive):decimal::subtract(positive,negative);
                personal.today=AssetShareTodayObservation{delta,comparison.currency,comparison.fromMs,comparison.toMs};
            }
        }
    }
    if(balanceComplete)assetShareValue_.personal=personal;
    assetShareValue_.assetSymbol=assetShareKey_=="solana:native"?QStringLiteral("SOL"):assetShareKey_.section(':',-1);
    assetShareValue_.assetName=assetShareValue_.assetSymbol;
    const auto route=resolveHoldingMarket(assetShareKey_,assetShareProviders_,holdingMarketCatalog(),holdingMarketPreferences());
    if(route.chart)assetShareValue_.market.identity=*route.chart;
    auto* dialog=new AssetShareDialog(assetShareValue_,{},this);assetShareDialog_=dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);const auto generation=assetShareGeneration_;
    connect(dialog,&QDialog::finished,this,[this,dialog,generation]{if(assetShareDialog_==dialog&&generation==assetShareGeneration_)clearAssetShare();});
    connect(dialog,&AssetShareDialog::marketRequested,this,[this,dialog,generation](const WatchlistIdentity& identity){
        if(assetShareDialog_!=dialog||generation!=assetShareGeneration_)return;
        const auto route=resolveHoldingMarket(assetShareKey_,assetShareProviders_,holdingMarketCatalog(),holdingMarketPreferences());
        if(!route.candidates.contains(identity))return;
        assetShareValue_.market={};assetShareValue_.market.identity=identity;assetShareValue_.requestedFromMs=assetShareValue_.requestedToMs=0;
        refreshAssetShare();
    });
    connect(dialog,&AssetShareDialog::periodRequested,this,[this,dialog,generation](AssetSharePeriod period){
        if(assetShareDialog_!=dialog||generation!=assetShareGeneration_)return;
        const auto identity=assetShareValue_.market.identity;assetShareValue_.market={};assetShareValue_.market.identity=identity;
        assetShareValue_.period=period;assetShareValue_.requestedFromMs=assetShareValue_.requestedToMs=0;refreshAssetShare();
    });
    dialog->open();refreshAssetShare();
}
void LocalWindow::refreshAssetShare(){
    if(!assetShareDialog_||assetShareRefreshing_)return;
    const QScopedValueRollback guard(assetShareRefreshing_,true);
    if(chartPrivateBlocked_||chartSourceResetting_||!securityReady_||!accountReadsAvailable(vault_)){clearAssetShare();return;}
    if(!assetShareDialog_->isVisible()){if(chartStudyFeed_)chartStudyFeed_->clearOwner("asset-share");return;}
    auto preferences=holdingMarketPreferences();
    preferences.sessionSelections.insert(assetShareKey_,assetShareValue_.market.identity);
    const auto route=resolveHoldingMarket(assetShareKey_,assetShareProviders_,holdingMarketCatalog(),preferences);
    const auto identity=route.chart.value_or(WatchlistIdentity{});const auto interval=AssetShareCard::intervalMs(assetShareValue_.period);
    assetShareDialog_->setMarkets(route.candidates,identity);
    assetShareValue_.market={};assetShareValue_.market.identity=identity;
    assetShareValue_.requestedFromMs=assetShareValue_.requestedToMs=0;
    const auto unavailable=[&](const QString& reason){
        if(chartStudyFeed_)chartStudyFeed_->clearOwner("asset-share");
        assetShareDialog_->setSnapshot(assetShareValue_);assetShareDialog_->setPublicStatus(reason);
    };
    if(!route.chart){unavailable(route.chartReason);return;}
    if(identity.market=="stock"){
        unavailable(tr("증권사 차트의 같은 기간·수정주가 출처를 확인한 자산 카드는 아직 지원하지 않아요."));return;
    }
    if(!chartStudyFeed_){unavailable(tr("공개 완료봉 자료를 확인할 수 없어요."));return;}
    chartStudyFeed_->setVerifiedUpbitMarkets(markets_?markets_->verifiedMarkets():QStringList{},markets_&&markets_->marketListAvailable());
    chartStudyFeed_->setDemands("asset-share",{{identity,interval,0}});
    const auto observation=chartStudyFeed_->observation(identity.key(),interval);const auto& series=observation.series;
    auto& value=assetShareValue_.market;value.identity=identity;value.intervalMs=interval;value.asOfMs=QDateTime::currentMSecsSinceEpoch();
    value.displayTimeZoneId="Asia/Seoul";value.colors=MovementColorPolicy::schemeFor(this);
    value.currency=series.currency;value.source.canonicalUrl=series.sourceUrl;value.source.sourceTimeMs=series.sourceTimeMs;
    value.source.receivedAtMs=series.receivedAtMs;value.source.provider=identity.source=="binance"?ChartShareSource::BinanceSpot:ChartShareSource::Upbit;
    value.availability=ChartShareAvailability::Waiting;
    if(series.available&&series.identity==identity.key()&&series.intervalMs==interval&&series.candles.size()<=200&&
       !series.candles.isEmpty()&&series.asset==assetShareValue_.assetSymbol){
        value.closed=series.candles;value.studies=keefiChartStudies(value.closed,interval);
        value.calculatedThroughMs=value.closed.last().startsAtMs+interval;
        value.viewport={value.closed.last().startsAtMs-AssetShareCard::durationMs(assetShareValue_.period),value.closed.last().startsAtMs};
        assetShareValue_.requestedFromMs=value.viewport.fromMs+interval;assetShareValue_.requestedToMs=value.viewport.toMs+interval;
        value.availability=observation.delayed||series.delayed?ChartShareAvailability::Stale:ChartShareAvailability::Ready;
    }
    if(markets_)for(const auto& row:markets_->rows())if(row.base==assetShareValue_.assetSymbol&&row.market=="KRW-"+row.base){
        assetShareValue_.assetName=row.koreanName.isEmpty()?assetShareValue_.assetSymbol:row.koreanName;break;
    }
    // A frozen private value does not become fresh again when public prices refresh.
    if(assetShareValue_.personal&&value.asOfMs-assetShareValue_.personal->observedAtMs>DisplayFreshnessMs)
        assetShareValue_.personal.reset();
    assetShareDialog_->setSnapshot(assetShareValue_);
    if(value.availability!=ChartShareAvailability::Ready&&value.availability!=ChartShareAvailability::Stale)assetShareDialog_->setPublicStatus(tr("선택한 시장·기간의 완료봉을 확인하고 있어요."));
}


void LocalWindow::syncPortfolioLayout() {
    if(!portfolioListDetail_)return;
    // Keep the holdings list usable at 720×640 with the approved larger fonts.
    // Compact the gaps, not the text or controls, in the existing narrow mode.
    if(portfolioWorkbench_)portfolioWorkbench_->layout()->setSpacing(portfolioListDetail_->narrow()?8:12);
    const bool detailOnly=portfolioListDetail_->narrow()&&!portfolioDetail_->isHidden();
    portfolioAllocation_->setVisible(!detailOnly&&!(portfolioInitialRefresh_&&portfolioCachedView_.groups.isEmpty()));
    portfolioScopeControls_->setVisible(!detailOnly);
    const bool oneRow=portfolioScopeControls_->sizeHint().width()+portfolioFilter_->minimumSizeHint().width()+24<=portfolioWorkbench_->width();
    homeTools_->setDirection(oneRow?QBoxLayout::LeftToRight:QBoxLayout::TopToBottom);
    const int headWidth=portfolioHead_->itemAt(0)->minimumSize().width()+portfolioAllocation_->minimumWidth()+24;
    const bool stack=!detailOnly&&headWidth>portfolioWorkbench_->width();
    portfolioHead_->setDirection(stack?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);
    portfolioHead_->setSpacing(stack?12:24);
}


} // namespace keefi
