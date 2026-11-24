#include "keefi/PlainToolTip.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartGridView.h"
#include "keefi/HoldingChartPanel.h"
#include "keefi/SegmentedControlStyle.h"
#include "keefi/CompactControls.h"
#include "keefi/ChartMarketResolver.h"
#include "keefi/ChartInstrumentPicker.h"
#include "keefi/InstrumentListTree.h"
#include "keefi/InstrumentListSelector.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicCoinIdentity.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartIndicatorState.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/CoinTapeView.h"
#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>
#include <QDataStream>
#include <QStyledItemDelegate>
#include <QStandardItemModel>
#include <QStyle>
#include <QPainter>
#include <QTextDocument>
#include <memory>
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/ChartMarketLedger.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LocalComboBox.h"
#include "keefi/LocalMenuControls.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/InfoButton.h"
#include <QScopeGuard>
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartDrawings.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/PortfolioController.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TradingController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include <QCheckBox>
#include <QScopedValueRollback>
#include <QBoxLayout>
#include <QDateTime>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QScrollArea>
#include <QTimer>
#include <QToolTip>
#include <QComboBox>
#include <QAction>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QMenu>
#include <QSaveFile>
#include <QSettings>
#include <QScrollBar>
#include <QScreen>
#include <QToolButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTreeWidgetItemIterator>
#include "keefi/ChartShareDialog.h"
#include <algorithm>
#include <limits>
#include <cmath>

#include "keefi/ChartWorkspacePresentation_p.h"

namespace keefi {
using namespace chart_workspace_detail;
namespace {
class IndicatorTextDelegate final:public QStyledItemDelegate {
public:using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override{
        QStyleOptionViewItem background(option);initStyleOption(&background,index);const auto text=index.data(Qt::DisplayRole).toString();background.text.clear();
        if(option.widget)option.widget->style()->drawControl(QStyle::CE_ItemViewItem,&background,painter,option.widget);
        painter->save();const auto area=option.rect.adjusted(5,4,-5,-4);painter->setClipRect(area);painter->setPen(themeColors().text);
        qreal y=area.top();for(const auto& paragraph:text.split('\n')){QTextLayout layout(paragraph,KeefiFonts::font(FontRole::Caption12));layout.setFormats(KeefiFonts::numberRuns(paragraph));layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(area.width());layout.endLayout();layout.draw(painter,QPointF(area.left(),y));y+=line.isValid()?line.height():14;}
        painter->restore();
    }
};
class InstrumentSortButton final : public QPushButton {
public:
    using QPushButton::QPushButton;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){click();event->accept();return;}
        QPushButton::keyPressEvent(event);
    }
};
class CompactInstrumentDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override {
        if(index.siblingAtColumn(0).data(IdentityRole).toString().isEmpty()) {
            QStyledItemDelegate::paint(painter,option,index);return;
        }
        QStyleOptionViewItem item(option);initStyleOption(&item,index);item.text.clear();
        if(index.column()==2)item.features&=~QStyleOptionViewItem::HasCheckIndicator;
        const auto* style=option.widget?option.widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&item,painter,option.widget);
        painter->save();painter->setClipRect(option.rect);
        if(index.column()==2) {
            const auto state=index.data(Qt::CheckStateRole);
            painter->setPen(state.isValid()?themeColors().text2:themeColors().text3);
            drawLedgerText(*painter,QRectF(option.rect),state.toInt()==Qt::Checked?QStringLiteral("★"):QStringLiteral("☆"),FontRole::Title17,Qt::AlignCenter);
            painter->restore();return;
        }
        QRectF line(option.rect.adjusted(6,0,-6,0));
        if(index.column()==0)if(const auto* tree=qobject_cast<const InstrumentListTree*>(option.widget);tree&&tree->dragHandlesVisible())
            line.adjust(InstrumentListTree::DragHandleWidth,0,0,0);
        const auto label=index.siblingAtColumn(0).data(ChartInstrumentLabelRole).toStringList();
        const auto unavailable = index.siblingAtColumn(0).data(BrowserUnavailableReasonRole).toString();
        const auto lines=index.data(Qt::DisplayRole).toString().split('\n');
        if(index.column()==0) {
            auto code=label.value(1);
            if(index.data(IdentityRole).toString().startsWith("coin:")) {
                code=code.section('/',0,0);
                if(!label.value(1).contains('/')&&code.endsWith("USDT")&&code.size()>4)code.chop(4);
            }
            const auto* tree=qobject_cast<const QTreeWidget*>(option.widget);
            const auto market=tree&&tree->property("chartBrowserScope").toString()=="watch"?label.value(2):QString{};
            const auto badges=index.data(WatchlistBadgesRole).value<QList<WatchlistBadge>>();
            const auto ordinary=std::find_if(badges.cbegin(),badges.cend(),[](const auto& badge){return !badge.fixedRisk&&!badge.text.isEmpty();});
            QList<QPair<QString,QColor>> runs;
            for(const auto& badge:badges)if(badge.fixedRisk&&!badge.text.isEmpty())runs.append({movementDecoratedText(badge.text,badge.direction),themeColors().danger});
            if(ordinary!=badges.cend())runs.append({movementDecoratedText(ordinary->text,ordinary->direction),MovementColorPolicy::colorFor(option.widget,ordinary->direction)});
            const auto quoteStatus=index.data(BrowserQuoteStatusRole).toString();
            if(!quoteStatus.isEmpty()&&(ordinary==badges.cend()||ordinary->text!=quoteStatus))runs.append({quoteStatus,themeColors().text3});
            const auto caption=KeefiFonts::font(FontRole::Caption12);
            qreal desiredBadges=0;for(const auto& run:runs)desiredBadges+=QFontMetricsF(caption).horizontalAdvance(run.first)+8;
            const qreal badgeWidth=qMin(qMax(0.0,line.width()*0.65),desiredBadges);
            const qreal marketWidth=market.isEmpty()?0:qMin(line.width()*0.26,qreal(QFontMetrics(caption).horizontalAdvance(market))+12);
            painter->setPen(unavailable.isEmpty()?themeColors().text:themeColors().text3);
            const qreal codeWidth=drawLedgerText(*painter,QRectF(line.left(),line.top(),qMax(0.0,line.width()-marketWidth-badgeWidth),line.height()),
                code,FontRole::SecondaryStrong14,Qt::AlignLeft,nullptr,false);
            const qreal nameLeft=line.left()+codeWidth+6;
            painter->setPen(themeColors().text3);
            drawLedgerText(*painter,QRectF(nameLeft,line.top(),qMax(0.0,line.right()-nameLeft-marketWidth-badgeWidth-6),line.height()),label.value(0),FontRole::Caption12);
            if(marketWidth>0) {
                const QRectF tag(line.right()-badgeWidth-marketWidth,line.center().y()-9,marketWidth-4,18);
                painter->setPen(themeColors().field);painter->setBrush(Qt::NoBrush);painter->drawRoundedRect(tag,3,3);
                painter->setPen(themeColors().text3);
                drawLedgerText(*painter,tag.adjusted(3,0,-3,0),market,FontRole::Caption12,Qt::AlignHCenter);
            }
            if(badgeWidth>0) {
                qreal x=line.right()-badgeWidth;
                for(const auto& run:runs) {
                    const qreal width=(QFontMetricsF(caption).horizontalAdvance(run.first)+8)*badgeWidth/desiredBadges;
                    painter->setPen(run.second);
                    drawLedgerText(*painter,QRectF(x+4,line.top(),qMax(0.0,width-4),line.height()),run.first,FontRole::Caption12,Qt::AlignRight);
                    x+=width;
                }
            }
        } else {
            painter->setPen(unavailable.isEmpty() ? themeColors().text : themeColors().text3);
            auto text=lines.value(0);
            if(index.column()==1||index.column()==3||index.column()==5)text.remove(QRegularExpression(QStringLiteral("\\s+(?:KRW|USDT|USD|원)$")));
            if(index.column()==3||index.column()==5)text.remove(QRegularExpression(QStringLiteral("\\.0(?=[만억조]$)")));
            if(index.column()==6) {
                const auto direction=movementForSign(index.siblingAtColumn(1).data(ChangeDirectionRole).toInt());
                painter->setPen(unavailable.isEmpty()?MovementColorPolicy::colorFor(option.widget,direction):themeColors().text3);
                text=movementDecoratedText(text,direction);
            }
            drawLedgerText(*painter,line,text,FontRole::Number11,Qt::AlignRight);
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override {
        auto size=QStyledItemDelegate::sizeHint(option,index);
        if(!index.siblingAtColumn(0).data(IdentityRole).toString().isEmpty())size.setHeight(32);
        return size;
    }
    bool editorEvent(QEvent* event,QAbstractItemModel* model,const QStyleOptionViewItem& option,const QModelIndex& index)override {
        if(index.column()==2&&index.data(Qt::CheckStateRole).isValid()&&event->type()==QEvent::MouseButtonRelease) {
            const auto* mouse=static_cast<QMouseEvent*>(event);
            if(mouse->button()==Qt::LeftButton&&option.rect.contains(mouse->position().toPoint()))
                return model->setData(index,index.data(Qt::CheckStateRole).toInt()==Qt::Checked?Qt::Unchecked:Qt::Checked,Qt::CheckStateRole);
        }
        if(index.column()==2&&event->type()==QEvent::MouseButtonPress)return false;
        return QStyledItemDelegate::editorEvent(event,model,option,index);
    }
};
void flatPanel(QWidget* panel) {
    panel->setAttribute(Qt::WA_StyledBackground, true);
    setThemeStyle(panel, QString("QWidget#%1{background:${bg};border:1px solid ${rule};border-radius:12px;}").arg(panel->objectName()));
}
// The ▾ that opens the full interval list; a painted single-colour mark.
class TimeframeListButton final : public QToolButton {
public:
    explicit TimeframeListButton(QWidget* parent) : QToolButton(parent) {
        applyCompactButton(this, true);
        bindThemePresentation(this, [this] { update(); });
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled() ? themeColors().text : themeColors().text3);
        const QPointF centre = QRectF(rect()).center();
        painter.drawPolygon(QPolygonF{{centre.x() - 4.5, centre.y() - 2}, {centre.x() + 4.5, centre.y() - 2}, {centre.x(), centre.y() + 3}});
    }
};
}
ChartWorkspace::ChartWorkspace(MarketCandles* model, PortfolioController* portfolio, QWidget* parent)
    : QWidget(parent), portfolio_(portfolio), candleModel_(model) {
    setObjectName("chartWorkspace");
    bindThemePresentation(this,[this]{
        if (listSelector_ && watchlists_) {
            for (int i=0;i<listSelector_->count();++i)
                listSelector_->setItemIcon(i,listFlagIcon(watchlists_->userList(listSelector_->itemData(i).toString()).flag));
        }
        if (!watchlistRows_) return;
        for (QTreeWidgetItemIterator item(watchlistRows_); *item; ++item)
            if (!((*item)->flags() & Qt::ItemIsEnabled)) (*item)->setForeground(0, themeColors().text3);
        watchlistRows_->viewport()->update();
    });
    indicatorPreferences_ = new ChartStudyPreferences(this);
    drawings_ = new ChartDrawings(this);
    setMinimumWidth(0);
    contractPacing_ = new QTimer(this);
    contractPacing_->setInterval(1000);
    contractPacing_->setSingleShot(true);
    connect(contractPacing_, &QTimer::timeout, this, &ChartWorkspace::requestContracts);
    contractRefresh_ = new QTimer(this);
    contractRefresh_->setInterval(60000);
    connect(contractRefresh_, &QTimer::timeout, this, [this] { contractPending_ = true; requestContracts(); });
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0,0,0,0);
    outer->setSpacing(16);
    auto* scroll = new QScrollArea(this);
    scroll_ = scroll;
    scroll->setObjectName("chartWorkspaceScroll");
    scroll->setAccessibleName(tr("차트 작업 공간 스크롤"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* workspaceBar = scroll->verticalScrollBar();
    // Nested disclosure layouts can publish a larger range in the next event
    // turn. Preserve an intentional end position, without jumping from the top.
    connect(workspaceBar, &QScrollBar::rangeChanged, this,
        [workspaceBar, previousMaximum = workspaceBar->maximum()](int, int maximum) mutable {
            const bool wasAtEnd = previousMaximum > 0 && workspaceBar->value() == previousMaximum;
            const bool grew = maximum > previousMaximum;
            previousMaximum = maximum;
            if (grew && wasAtEnd) workspaceBar->setValue(maximum);
        });
    auto* content = new QWidget(scroll);
    auto* panels = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer->addWidget(scroll,1);
    const int chartMargin = 8;
    panels->setContentsMargins(chartMargin, 8, chartMargin, 8);
    panels->setSpacing(12);

    instrumentPanel_ = new QWidget(this, Qt::Popup | Qt::FramelessWindowHint);
    instrumentPanel_->setObjectName("chartInstrumentPanel");
    instrumentPanel_->setWindowTitle(tr("종목 패널"));
    instrumentPanel_->setAccessibleName(tr("종목 패널"));
    instrumentPanel_->setMaximumHeight(520);
    instrumentPanel_->installEventFilter(this);
    auto* popupLayout = new QVBoxLayout(instrumentPanel_);
    popupLayout->setContentsMargins(0, 0, 0, 0);
    list_ = new QWidget(instrumentPanel_);
    popupLayout->addWidget(list_);
    list_->setObjectName("chartWorkspaceList");
    auto* listLayout = new QVBoxLayout(list_);
    listLayout->setContentsMargins(12, 12, 12, 12);
    listLayout->setSpacing(4);
    flatPanel(list_);
    auto* holdingsTitle = new QLabel(tr("보유 (자동)"), list_);
    holdingsTitle->setObjectName("chartWorkspaceHoldingsTitle");
    holdingsTitle->setFont(KeefiFonts::font(FontRole::BodyStrong15));
    holdingsTitle->hide();
    holdingsEmpty_ = new QLabel(tr("금고를 열면 보유 종목을 보여요."), list_);
    holdingsEmpty_->setObjectName("chartWorkspaceHoldingsLocked");
    holdingsEmpty_->setFont(KeefiFonts::font(FontRole::Caption12));
    holdingsEmpty_->setWordWrap(true);
    setThemeStyle(holdingsEmpty_, "color:${text3};border:0;padding:8px 0;");
    holdingsEmpty_->hide();
    holdingsRows_ = new QTreeWidget(list_);
    holdingsRows_->setObjectName("chartWorkspaceHoldings");
    holdingsRows_->setAccessibleName(tr("보유 종목 자동 목록"));
    holdingsRows_->setColumnCount(2);
    holdingsRows_->setHeaderHidden(true);
    holdingsRows_->setRootIsDecorated(false);
    holdingsRows_->setItemDelegate(new WatchlistBadgeDelegate(holdingsRows_));
    holdingsRows_->setMinimumHeight(0);
    holdingsRows_->setMaximumHeight(130);
    holdingsRows_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    holdingsRows_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    holdingsRows_->viewport()->installEventFilter(this);
    setThemeStyle(holdingsRows_, "QTreeWidget{background:${bg};border:0;padding:0;} QTreeWidget::item:selected{background:${surface};color:${text};}");
    bindMovementPresentation(holdingsRows_,[this]{holdingsRows_->viewport()->update();});
    holdingsRows_->hide();
    // The holdings tab uses the main instrument rows; retain this private projection for compatibility.
    const auto selectHolding = [this](QTreeWidgetItem* item) {
        const auto key = item->data(0, IdentityRole).toString();
        if (privateEnabled_ && setIdentity(key, false)) emit identitySelected(selectedChartIdentity_);
    };
    connect(holdingsRows_, &QTreeWidget::itemClicked, this, selectHolding);
    connect(holdingsRows_, &QTreeWidget::itemActivated, this, selectHolding);
    connect(holdingsRows_->verticalScrollBar(), &QScrollBar::valueChanged, this, &ChartWorkspace::scheduleVisibleRows);
    auto* listSelector = new InstrumentListSelector(list_);
    listSelector_ = listSelector;
    listSelector_->setObjectName("chartWorkspaceListSelector");
    listSelector_->setAccessibleName(tr("관심 리스트 전환"));
    listSelector->setCompact();
    listSelector_->setMinimumWidth(0);
    listSelector_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    listSelector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    listSelector_->setMinimumContentsLength(1);
    // Keep list flag decorations in the popup as well as the selected label.
    listSelector_->setItemDelegate(new QStyledItemDelegate(listSelector_));
    connect(listSelector,&InstrumentListSelector::orderViewChanged,this,&ChartWorkspace::refreshWatchlists);
    connect(listSelector,&InstrumentListSelector::listMoveRequested,this,[this](const QString& source,const QString& target,bool after){
        if(!watchlists_||source==target)return;
        QStringList order;for(const auto& list:watchlists_->userLists())order.append(list.id);
        if(!order.contains(source)||!order.contains(target))return;
        order.removeAll(source);const int destination=order.indexOf(target)+(after?1:0);
        watchlists_->moveList(source,destination);
    });
    listMenuButton_ = new QToolButton(list_);
    listMenuButton_->setObjectName("chartWorkspaceListMenu");
    listMenuButton_->setText("⋯");
    listMenuButton_->setAccessibleName(tr("관심 리스트 관리 메뉴"));
    listMenuButton_->setPopupMode(QToolButton::InstantPopup);
    applyCompactButton(listMenuButton_, true);
    setThemeStyle(listMenuButton_, listMenuButton_->property("keefiThemeTemplate").toString()
        + QStringLiteral("\nQToolButton::menu-indicator { image:none; width:0; height:0; }"));
    listMarkets_=new QLabel(list_);listMarkets_->setObjectName("chartWorkspaceMarkets");
    listMarkets_->setFont(KeefiFonts::font(FontRole::Caption12));listMarkets_->setWordWrap(true);listLayout->addWidget(listMarkets_);
    auto* instrumentTree = new InstrumentListTree(list_);
    watchlistRows_ = instrumentTree;
    connect(instrumentTree,&InstrumentListTree::rowsMoveRequested,this,&ChartWorkspace::moveBrowserRows);
    connect(instrumentTree,&InstrumentListTree::sectionMoveRequested,this,&ChartWorkspace::moveBrowserSection);
    connect(instrumentTree,&InstrumentListTree::relativeMoveRequested,this,&ChartWorkspace::moveBrowserSelection);
    connect(instrumentTree,&InstrumentListTree::removeRequested,this,&ChartWorkspace::removeBrowserSelection);
    connect(instrumentTree,&InstrumentListTree::rowContextRequested,this,&ChartWorkspace::openBrowserContextMenu);
    connect(instrumentTree,&InstrumentListTree::sectionExpandedByUser,this,[this](const QString& listId,const QString& sectionId,bool expanded){
        if(!watchlists_||listId!=currentListId()||browserScope_!="watch")return;
        sectionExpansion_[listId].insert(sectionId,expanded);
        if(chartSettings_)chartSettings_->setValue("chart/watchlistSections/"+listId+"/"+sectionId+"/expanded",expanded);
    });
    connect(instrumentTree,&InstrumentListTree::sectionRenameRequested,this,[this](const QString& listId,const QString& sectionId,const QString& name){
        if(watchlists_&&listId==currentListId()&&browserScope_=="watch")watchlists_->renameSection(listId,sectionId,name);
    });
    connect(instrumentTree,&InstrumentListTree::sectionRemoveRequested,this,[this](const QString& listId,const QString& sectionId){
        if(watchlists_&&listId==currentListId()&&browserScope_=="watch")watchlists_->removeSection(listId,sectionId);
    });
    connect(instrumentTree,&InstrumentListTree::sectionAddBelowRequested,this,[this](const QString& listId,const QString& sectionId){
        QPointer<WatchlistWorkspaceList> source=watchlists_;
        if(!source||listId!=currentListId()||browserScope_!="watch")return;
        bool accepted=false;
        const auto name=QInputDialog::getText(this,tr("섹션 추가"),tr("섹션 이름"),QLineEdit::Normal,{},&accepted);
        if(!accepted||!source||source!=watchlists_||listId!=currentListId()||browserScope_!="watch")return;
        const auto list=source->userList(listId);
        int after=-1;for(int i=0;i<list.sections.size();++i)if(list.sections[i].id==sectionId){after=i;break;}
        if(after<0)return;
        const auto added=source->addSection(listId,name);
        if(!added.isEmpty())source->moveSection(listId,added,after+1);
        if(isVisible()) { positionInstrumentPanel();instrumentPanel_->show();watchlistRows_->setFocus(Qt::PopupFocusReason); }
    });
    watchlistRows_->setObjectName("chartWorkspaceWatchlist");
    watchlistRows_->setColumnCount(7);
    watchlistRows_->setItemDelegate(new CompactInstrumentDelegate(watchlistRows_));
    bindMovementPresentation(watchlistRows_,[this]{watchlistRows_->viewport()->update();});
    watchlistRows_->setHeaderLabels({tr("종목"), tr("가격"), QStringLiteral("★"), tr("거래대금"), tr("거래량"), tr("시가총액"), tr("24시간")});
    watchlistRows_->headerItem()->setData(2,Qt::AccessibleTextRole,tr("관심"));
    watchlistRows_->headerItem()->setTextAlignment(2,Qt::AlignCenter);
    watchlistRows_->setRootIsDecorated(false);
    watchlistRows_->setHeaderHidden(false);
    watchlistRows_->header()->setStretchLastSection(false);
    watchlistRows_->header()->setMinimumSectionSize(0);
    watchlistRows_->header()->setSectionResizeMode(2,QHeaderView::Fixed);
    watchlistRows_->setColumnWidth(2,24);
    watchlistRows_->header()->setFixedHeight(26);
    watchlistRows_->header()->setFont(KeefiFonts::font(FontRole::Caption12));
    watchlistRows_->header()->moveSection(watchlistRows_->header()->visualIndex(6),2);
    watchlistRows_->header()->moveSection(watchlistRows_->header()->visualIndex(2),6);
    for(int column=3;column<6;++column)watchlistRows_->setColumnHidden(column,true);
    watchlistRows_->header()->setSectionsClickable(true);
    for(const auto& item:QList<QPair<int,QString>>{{0,tr("종목순 정렬")},{1,tr("가격순 정렬")},{6,tr("24시간 등락순 정렬")},{3,tr("거래대금순 정렬")},{4,tr("거래량순 정렬")},{5,tr("시가총액순 정렬")}}) {
        auto* button=new InstrumentSortButton(watchlistRows_->header()->viewport());
        button->setObjectName("chartBrowserHeader_"+QString::number(item.first));button->setAccessibleName(item.second);
        button->setFocusPolicy(Qt::StrongFocus);button->setFont(KeefiFonts::font(FontRole::Caption12));
        setThemeStyle(button,QStringLiteral("QPushButton{background:${bg};color:${text3};border:0;border-radius:0;border-bottom:1px solid ${rule};padding:0 4px;text-align:%1;} QPushButton[sortActive=true]{color:${text};font-weight:bold;} QPushButton:hover{background:${surface};} QPushButton:focus{border-bottom:2px solid ${accent};}").arg(item.first==0?"left":"right"));
        browserSortButtons_.insert(item.first,button);
        connect(button,&QPushButton::clicked,this,[this,column=item.first]{sortBrowserColumn(column);});
    }
    connect(watchlistRows_->header(),&QHeaderView::sectionClicked,this,&ChartWorkspace::sortBrowserColumn);
    connect(watchlistRows_->header(),&QHeaderView::sectionResized,this,[this]{updateBrowserHeaders();});
    connect(watchlistRows_->horizontalScrollBar(),&QScrollBar::valueChanged,this,[this]{updateBrowserHeaders();});
    watchlistRows_->setAccessibleName(tr("시장별 종목 목록 · Enter로 차트 열기 · Space로 관심 추가 또는 해제"));
    watchlistRows_->installEventFilter(this);
    watchlistRows_->setTabKeyNavigation(false);
    setThemeStyle(watchlistRows_, "QTreeWidget{background:${bg};border:0;padding:0;} QTreeWidget::item:selected{background:${surface};color:${text};} QHeaderView::section{padding:0 4px;}");
    watchlistRows_->setSelectionMode(QAbstractItemView::SingleSelection);
    watchlistRows_->setMinimumHeight(128);
    watchlistRows_->setMinimumWidth(0);
    watchlistRows_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    watchlistRows_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    watchlistRows_->viewport()->installEventFilter(this);
    auto* browserSort = new LocalComboBox(list_);
    browserSortSelector_ = browserSort;
    browserSort->setObjectName("chartBrowserSort");
    browserSort->setAccessibleName(tr("차트 목록 순위 기준"));
    browserSort->setCompact();
    for(const auto& option:QList<QPair<QString,QString>>{{"auto",tr("기본 순서")},{"gainers",tr("상승률순")},{"losers",tr("하락률순")},{"value",tr("거래대금순")},{"cap",tr("시가총액순")},{"manual",tr("저장한 순서")},{"name",tr("이름순")}})
        browserSortSelector_->addItem(option.second,option.first);
    connect(browserSortSelector_,&QComboBox::currentIndexChanged,this,[this]{browserSort_=browserSortSelector_->currentData().toString();browserStates_[browserMarket_].sort=browserSort_;refreshWatchlistRows();ensureBrowserCatalog();});
    browserSortSelector_->hide();
    browserExpand_=new QPushButton(QStringLiteral("↗"),list_);browserExpand_->setObjectName("chartBrowserExpand");browserExpand_->setCheckable(true);
    applyCompactButton(browserExpand_);
    browserExpand_->setFixedSize(28,28);
    browserExpand_->setAccessibleName(tr("차트 목록 넓게 · 거래대금·거래량·시가총액 보기"));
    connect(browserExpand_,&QPushButton::toggled,this,[this](bool expanded){
        watchlistRows_->setHorizontalScrollBarPolicy(expanded?Qt::ScrollBarAsNeeded:Qt::ScrollBarAlwaysOff);
        browserExpand_->setText(expanded?QStringLiteral("↙"):QStringLiteral("↗"));
        positionInstrumentPanel();refreshWatchlistRows();ensureBrowserCatalog();
    });
    listLayout->addWidget(watchlistRows_, 1);
    holdingPanel_=new HoldingChartPanel(list_);holdingPanel_->hide();listLayout->addWidget(holdingPanel_,1);
    connect(holdingPanel_, &HoldingChartPanel::chartActivated, this, [this](const QString&, const QString& key) {
        if (!setIdentity(key, false)) return;
        emit identitySelected(key);
        if (activatingHoldingResult_) closeInstrumentPanel();
    });
    holdingPanel_->resultTree()->installEventFilter(this);
    connect(holdingPanel_,&HoldingChartPanel::marketSelected,this,[this](const QString& assetKey,const QString& key){emit holdingMarketSelected(assetKey,key);if(setIdentity(key,false))emit identitySelected(key);});
    trendToggle_=new QPushButton(tr("추세표"),list_);trendToggle_->setObjectName("chartWatchTrendToggle");trendToggle_->setCheckable(true);applyCompactButton(trendToggle_);
    trendRows_=new QTreeWidget(list_);trendRows_->setObjectName("chartWatchTrendRows");trendRows_->setColumnCount(4);trendRows_->setHeaderLabels({tr("종목"),tr("1시간"),tr("4시간"),tr("1일")});trendRows_->setRootIsDecorated(false);trendRows_->setMinimumWidth(0);trendRows_->setMinimumHeight(128);trendRows_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);trendRows_->setFont(KeefiFonts::font(FontRole::Caption12));trendRows_->header()->setFont(KeefiFonts::font(FontRole::Caption12));listLayout->addWidget(trendRows_,1);trendRows_->hide();
    connect(trendToggle_, &QPushButton::toggled, this, [this](bool enabled) {
        trendEnabled_ = enabled;
        ++trendGeneration_;
        trendKey_.clear();
        trendResults_.clear();
        filterBrowserRows();
        refreshTrendDemands();
    });
    const auto selectTrend = [this](QTreeWidgetItem* item, int column) { openTrendRow(item, column); };
    connect(trendRows_, &QTreeWidget::itemClicked, this, selectTrend);
    connect(trendRows_, &QTreeWidget::itemActivated, this, selectTrend);
    trendRows_->installEventFilter(this);

    browserEmpty_ = new QWidget(list_);
    browserEmpty_->setObjectName("chartInstrumentEmpty");
    browserEmpty_->setMinimumHeight(128);
    auto* emptyLayout = new QVBoxLayout(browserEmpty_);
    emptyLayout->addStretch();
    auto* emptyLabel = new QLabel(tr("찾는 종목이 없어요"), browserEmpty_);
    emptyLabel->setFont(KeefiFonts::font(FontRole::Body15));
    emptyLayout->addWidget(emptyLabel, 0, Qt::AlignHCenter);
    auto* clearSearch = new QPushButton(tr("검색어 지우기"), browserEmpty_);
    clearSearch->setObjectName("chartInstrumentClearSearch");
    applyCompactButton(clearSearch);
    emptyLayout->addWidget(clearSearch, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    listLayout->addWidget(browserEmpty_, 1);
    browserEmpty_->hide();

    auto* findPublic = new QPushButton(tr("종목 찾기"), list_);
    findPublic->setObjectName("chartInstrumentFindPublic");
    applyCompactButton(findPublic);
    findPublic->hide();
    connect(findPublic, &QPushButton::clicked, this, [this] {
        const auto query = input_->text();
        closeInstrumentPanel();
        openInstrumentPicker(query);
    });

    listStatus_ = new LedgerTextLabel(FontRole::Caption12,list_);
    listStatus_->setObjectName("chartWorkspaceListStatus");
    listStatus_->setWordWrap(true);
    listStatus_->setTextFormat(Qt::PlainText);
    listLayout->addWidget(listStatus_);
    browserRetry_=new QPushButton(tr("업비트 종목 목록 다시 조회"),list_);
    browserRetry_->setObjectName("chartBrowserRetry");applyCompactButton(browserRetry_);browserRetry_->hide();
    listLayout->addWidget(browserRetry_);
    auto* browserFooter=new QHBoxLayout;browserFooter->setContentsMargins(0,0,0,0);browserFooter->setSpacing(6);
    browserHint_=new LedgerTextLabel(FontRole::Caption12,list_);browserHint_->setObjectName("chartBrowserHint");
    setThemeStyle(browserHint_,"color:${text3};border:0;");browserFooter->addWidget(browserHint_,1);
    browserManual_=new QPushButton(tr("내 순서로"),list_);browserManual_->setObjectName("chartBrowserManualOrder");applyCompactButton(browserManual_);browserManual_->hide();browserFooter->addWidget(browserManual_);
    connect(browserManual_,&QPushButton::clicked,this,[this]{browserSort_="manual";browserStates_[browserMarket_].sort=browserSort_;if(browserSortSelector_){const QSignalBlocker block(browserSortSelector_);browserSortSelector_->setCurrentIndex(browserSortSelector_->findData(browserSort_));}refreshWatchlistRows();});
    listLayout->addLayout(browserFooter);
    browserRetryTimer_=new QTimer(this);browserRetryTimer_->setInterval(1000);
    connect(browserRetryTimer_,&QTimer::timeout,this,[this]{
        if(!instrumentPanel_->isVisible()){browserRetryTimer_->stop();return;}
        filterBrowserRows();
    });
    connect(browserRetry_,&QPushButton::clicked,this,[this]{
        if(!coinBrowserMarket(browserMarket_))return;
        const auto binance=publicInstruments_;
        if(publicMarkets_&&!publicMarkets_->busy()&&publicMarkets_->retryAfterMs()==0&&
           publicMarkets_->marketListState()==PublicMarketCatalog::MarketListState::Failed)publicMarkets_->retryMarketList();
        if(binance&&binance->canRetry("binance"))binance->ensure("binance");
    });
    buildWatchlistMenu();
    connect(listSelector_, &QComboBox::currentIndexChanged, this, [this] {
        if (renderingLists_) return;
        if(browserContextMenu_)browserContextMenu_->close();
        listSelector_->setToolTip(plainToolTip(currentListId().isEmpty()?tr("오른쪽 메뉴에서 새 관심 리스트를 만드세요."):listSelector_->currentText()));
        if (watchlists_ && currentListId() != HoldingsList) watchlists_->useList(currentListId());
        refreshWatchlistRows();
    });
    const auto selectBrowser = [this,instrumentTree](QTreeWidgetItem* item, int column) {
        if (column != 2 && instrumentTree->rowActivationAllowed()) openBrowserRow(item);
    };
    connect(watchlistRows_, &QTreeWidget::itemActivated, this, selectBrowser);
    connect(watchlistRows_, &QTreeWidget::itemClicked, this, selectBrowser);
    connect(watchlistRows_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item,int column) { if(column==2)toggleBrowserFavorite(item); });
    connect(watchlistRows_->verticalScrollBar(), &QScrollBar::valueChanged, this, &ChartWorkspace::scheduleVisibleRows);
    connect(watchlistRows_, &QTreeWidget::itemExpanded, this, &ChartWorkspace::scheduleVisibleRows);
    connect(watchlistRows_, &QTreeWidget::itemCollapsed, this, &ChartWorkspace::scheduleVisibleRows);
    connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, &ChartWorkspace::scheduleVisibleRows);
    auto* browserControls = new QWidget(list_);
    browserControls->setObjectName("chartBrowserControls");
    auto* browserLayout = new QVBoxLayout(browserControls);
    browserLayout->setContentsMargins(0,0,0,0);
    browserLayout->setSpacing(4);
    input_ = new QLineEdit(browserControls);
    input_->setObjectName("chartWorkspaceAsset");
    input_->setAccessibleName(tr("종목 이름 또는 코드 검색"));
    input_->setPlaceholderText(tr("이름 또는 코드 · BTC, 삼성전자, AAPL"));
    input_->setMaxLength(80);
    applyCompactInput(input_);
    input_->setFixedHeight(32);
    input_->installEventFilter(this);
    browserLayout->addWidget(input_);
    connect(input_, &QLineEdit::textChanged, this, &ChartWorkspace::filterBrowserRows);
    connect(input_, &QLineEdit::returnPressed, this, &ChartWorkspace::activateInstrumentResult);
    connect(clearSearch, &QPushButton::clicked, this, [this] { input_->clear(); input_->setFocus(); });

    auto* filters = new QHBoxLayout;
    browserHeaderLayout_=filters;
    filters->setSpacing(6);
    auto* scopeBox=new QWidget(browserControls);
    scopeBox->setObjectName("chartBrowserScopes");
    auto* scopes = new QHBoxLayout(scopeBox);
    scopes->setContentsMargins(2,2,2,2);scopes->setSpacing(0);
    setThemeStyle(scopeBox,"QWidget#chartBrowserScopes{background:${surface};border:0;border-radius:6px;}");
    auto* scopeGroup = new QButtonGroup(browserControls);
    QList<QWidget*> tabOrder{input_};
    const auto configureFilterButton = [](QPushButton* button) {
        applyCompactButton(button);
        button->setFixedHeight(24);
        button->setFont(KeefiFonts::font(FontRole::Caption12));
        setThemeStyle(button, QStringLiteral(R"(
            QPushButton { background:${surface}; color:${text2}; border:1px solid transparent;
                border-radius:4px; padding:0 5px; }
            QPushButton:hover { background:${field}; }
            QPushButton:checked { background:${text}; color:${bg}; }
            QPushButton:focus, QPushButton:checked:focus { border:2px solid ${accent}; }
            QPushButton:disabled { background:transparent; color:${text3}; }
        )"));
    };
    for (const auto& option : QList<QPair<QString, QString>>{{"all",tr("전체")},{"holdings",tr("보유")},{"watch",tr("관심")}}) {
        auto* button = new QPushButton(option.second, browserControls);
        button->setObjectName("chartBrowserScope_" + option.first);
        button->setCheckable(true);
        button->setChecked(option.first == browserScope_);
        configureFilterButton(button);
        scopeGroup->addButton(button);
        scopes->addWidget(button);
        tabOrder.append(button);
        connect(button, &QPushButton::clicked, this, [this, key = option.first] { setBrowserScope(key); });
    }
    filters->addWidget(scopeBox);
    browserMarkets_=new QWidget(browserControls);browserMarkets_->setObjectName("chartBrowserMarkets");
    auto* markets = new QHBoxLayout(browserMarkets_);
    markets->setContentsMargins(2,2,2,2);markets->setSpacing(0);
    setThemeStyle(browserMarkets_,"QWidget#chartBrowserMarkets{background:${surface};border:0;border-radius:6px;}");
    auto* marketGroup = new QButtonGroup(browserControls);
    for (const auto& option : QList<QPair<QString, QString>>{{"upbit",tr("업비트")},{"binance",tr("바이낸스")},{"kr",tr("한국 주식")},{"us",tr("미국 주식")}}) {
        auto* button = new QPushButton(option.second, browserControls);
        button->setObjectName("chartBrowserMarket_" + option.first);
        button->setCheckable(true);
        button->setChecked(option.first == browserMarket_);
        configureFilterButton(button);
        marketGroup->addButton(button);
        markets->addWidget(button);
        tabOrder.append(button);
        connect(button, &QPushButton::clicked, this, [this, key = option.first] { setBrowserMarket(key); });
    }
    filters->addWidget(browserMarkets_);
    filters->addWidget(listSelector_,1);
    listSelector_->setMaximumWidth(150);listSelector_->setFixedHeight(28);
    listCreate_=new QPushButton(QStringLiteral("＋"),browserControls);
    listCreate_->setObjectName("chartListCreateButton");listCreate_->setAccessibleName(tr("새 관심 리스트"));
    applyCompactButton(listCreate_);listCreate_->setFixedSize(28,28);filters->addWidget(listCreate_);
    connect(listCreate_,&QPushButton::clicked,this,[this]{if(auto* action=findChild<QAction*>("chartListCreate"))action->trigger();});
    listMenuButton_->setFixedSize(28,28);filters->addWidget(listMenuButton_);
    filters->addStretch();
    browserCount_=new LedgerTextLabel(FontRole::Caption12,browserControls);browserCount_->setObjectName("chartBrowserCount");
    browserCount_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);filters->addWidget(browserCount_);
    holdingInformation_=holdingPanel_->informationButton();filters->addWidget(holdingInformation_);
    browserViewModes_=new QWidget(browserControls);browserViewModes_->setObjectName("chartBrowserViewModes");
    auto* modes=new QHBoxLayout(browserViewModes_);modes->setContentsMargins(2,2,2,2);modes->setSpacing(0);
    setThemeStyle(browserViewModes_,"QWidget#chartBrowserViewModes{background:${surface};border:0;border-radius:6px;}");
    browserListMode_=new QPushButton(tr("목록"),browserViewModes_);browserListMode_->setObjectName("chartWatchListMode");
    browserListMode_->setCheckable(true);browserListMode_->setChecked(true);
    configureFilterButton(browserListMode_);configureFilterButton(trendToggle_);
    auto* viewGroup=new QButtonGroup(browserViewModes_);viewGroup->setExclusive(false);viewGroup->addButton(browserListMode_);viewGroup->addButton(trendToggle_);
    modes->addWidget(browserListMode_);modes->addWidget(trendToggle_);
    connect(browserListMode_,&QPushButton::clicked,this,[this]{trendToggle_->setChecked(false);browserListMode_->setChecked(true);});
    connect(trendToggle_,&QPushButton::toggled,this,[this](bool enabled){browserListMode_->setChecked(!enabled);});
    filters->addWidget(browserViewModes_);filters->addWidget(browserExpand_);
    browserLayout->addLayout(filters);
    // The preserved market-filter contract needs a reachable control in watch scope.
    // Keep it on a compact extra line instead of squeezing the list controls away.
    browserWatchMarkets_=new QWidget(browserControls);
    browserWatchMarketsLayout_=new QHBoxLayout(browserWatchMarkets_);
    browserWatchMarketsLayout_->setContentsMargins(0,0,0,0);browserWatchMarketsLayout_->setSpacing(0);
    browserLayout->addWidget(browserWatchMarkets_);browserWatchMarkets_->hide();
    listLayout->insertWidget(0, browserControls);
    tabOrder.append(QList<QWidget*>{listSelector_,listCreate_,listMenuButton_,holdingInformation_,browserListMode_,trendToggle_,browserExpand_});
    for(const auto column:{0,1,6,3,4,5})tabOrder.append(browserSortButtons_.value(column));
    tabOrder.append(QList<QWidget*>{watchlistRows_,holdingPanel_->resultTree(),trendRows_,clearSearch,browserManual_,browserRetry_});
    for (qsizetype i = 1; i < tabOrder.size(); ++i) QWidget::setTabOrder(tabOrder[i - 1], tabOrder[i]);
    chartPane_ = new QWidget(this);
    chartPane_->setObjectName("chartWorkspaceChartPane");
    chartPane_->setMinimumWidth(0);
    auto* chartLayout = new QVBoxLayout(chartPane_);
    chartLayout->setContentsMargins(16, 16, 16, 16);
    chartLayout->setSpacing(8);
    flatPanel(chartPane_);
    toolbar_=new QWidget(chartPane_);toolbar_->setObjectName("chartToolbar");
    toolbar_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);toolbar_->setFixedHeight(96);toolbar_->installEventFilter(this);
    const auto group=[this](const char* name){
        auto* widget=new QWidget(toolbar_);widget->setObjectName(name);widget->installEventFilter(this);
        auto* row=new QHBoxLayout(widget);row->setContentsMargins(10,8,10,8);row->setSpacing(6);
        widget->setMinimumWidth(0);widget->setFixedHeight(44);
        setThemeStyle(widget,QStringLiteral("QWidget#%1{background:${bg};border:1px solid ${rule};border-radius:8px;}").arg(name));return widget;
    };
    instrumentTools_=group("chartWorkspaceTitleHost");frequentTools_=group("chartToolbarFrequent");secondaryTools_=group("chartToolbarSecondary");
    auto* priceLayout=static_cast<QHBoxLayout*>(instrumentTools_->layout());
    auto* timeframeLayout=static_cast<QHBoxLayout*>(frequentTools_->layout());
    auto* gridTools=static_cast<QHBoxLayout*>(secondaryTools_->layout());
    chartLayout->addWidget(toolbar_);
    currentPrice_ = new QLabel("—", chartPane_); currentPrice_->setObjectName("chartCurrentPrice");
    // Price, unit and freshness share one baseline row (mockup); the unit never wraps below.
    currentPrice_->setFont(KeefiFonts::font(FontRole::Value22));currentPrice_->setMinimumWidth(0);currentPrice_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred); priceLayout->addWidget(currentPrice_,0,Qt::AlignVCenter);
    currentPriceUnit_ = new QLabel(chartPane_); currentPriceUnit_->setObjectName("chartCurrentPriceUnit");
    currentPriceUnit_->setFont(KeefiFonts::font(FontRole::Number11)); priceLayout->addWidget(currentPriceUnit_,0,Qt::AlignVCenter);
    currentPriceReference_ = new QLabel(chartPane_); currentPriceReference_->setObjectName("chartCurrentPriceReference");
    currentPriceReference_->setFont(KeefiFonts::font(FontRole::Number11));
    priceLayout->addWidget(currentPriceReference_,0,Qt::AlignVCenter);
    // The won value and related world markets share the price row, so the header never
    // grows; the related list gives up trailing items before any text would clip.
    priceConversion_=new QLabel(chartPane_);priceConversion_->setObjectName("chartPriceConversion");priceConversion_->setTextFormat(Qt::RichText);
    priceConversion_->setFont(KeefiFonts::font(FontRole::Caption12));priceConversion_->hide();priceLayout->addWidget(priceConversion_,0,Qt::AlignVCenter);
    relatedMarkets_=new QLabel(chartPane_);relatedMarkets_->setObjectName("chartRelatedMarkets");relatedMarkets_->setTextFormat(Qt::RichText);
    relatedMarkets_->setFont(KeefiFonts::font(FontRole::Caption12));relatedMarkets_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);relatedMarkets_->setMinimumWidth(0);
    relatedMarkets_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);relatedMarkets_->hide();relatedMarkets_->installEventFilter(this);priceLayout->addWidget(relatedMarkets_,1);priceLayout->addStretch();
    bindMovementPresentation(relatedMarkets_,[this]{renderMarketContext();});bindThemePresentation(relatedMarkets_,[this]{renderMarketContext();});
    bindMovementPresentation(currentPriceReference_,[this]{if(candles_)refreshMarketLedger();});
    bindThemePresentation(currentPriceReference_,[this]{if(candles_)refreshMarketLedger();});
    timeframeSelector_ = new LocalComboBox(chartPane_);
    timeframeSelector_->setObjectName("chartWorkspaceTimeframe");
    timeframeSelector_->setAccessibleName(tr("차트 시간봉 선택"));
    static_cast<LocalComboBox*>(timeframeSelector_)->setCompact();
    const QList<QPair<QString, qint64>> timeframes{{tr("1분"), 60000}, {tr("5분"), 300000}, {tr("15분"), 900000},
        {tr("1시간"), 3600000}, {tr("4시간"), 14400000}, {tr("1일"), 86400000}};
    for (const auto& timeframe : timeframes) timeframeSelector_->addItem(timeframe.first, timeframe.second);
    timeframeSelector_->setCurrentIndex(2);
    timeframeSelector_->hide(); // Model only: a narrow native popup covered the price. Text buttons are its face.
    timeframeLayout->addWidget(timeframeSelector_);
    auto* timeframeButtons = new QWidget(chartPane_);timeframeButtons_=timeframeButtons; timeframeButtons->setObjectName("chartWorkspaceTimeframeButtons");
    auto* timeframeRow = new QHBoxLayout(timeframeButtons); timeframeRow->setContentsMargins(0, 0, 0, 0); timeframeRow->setSpacing(2);
    auto* timeframeGroup = new QButtonGroup(timeframeButtons); timeframeGroup->setExclusive(true);
    for (int index = 0; index < timeframeSelector_->count(); ++index) {
        auto* button = new QPushButton(timeframeSelector_->itemText(index), timeframeButtons);
        button->setObjectName(QStringLiteral("chartWorkspaceTimeframe_%1").arg(timeframeSelector_->itemData(index).toLongLong()));
        button->setCheckable(true); button->setAutoExclusive(true);
        button->setAccessibleName(tr("시간봉 %1").arg(timeframeSelector_->itemText(index)));
        applySegmentedControlStyle(button, CompactButtonHeight);
        timeframeGroup->addButton(button, index); timeframeRow->addWidget(button);
        connect(button, &QPushButton::toggled, timeframeSelector_, [this, index](bool selected) { if (selected) timeframeSelector_->setCurrentIndex(index); });
    }
    timeframeList_ = new TimeframeListButton(timeframeButtons);
    timeframeList_->setObjectName("chartWorkspaceTimeframeList");
    timeframeList_->setAccessibleName(tr("시간봉 목록"));
    timeframeList_->setToolTip(tr("시간봉 목록 · ★ 툴바에 고정"));
    timeframeList_->hide(); timeframeRow->addWidget(timeframeList_);
    connect(timeframeList_, &QToolButton::clicked, this, [this] { openTimeframeList(timeframeList_); });
    const auto syncTimeframeButtons = [this, timeframeButtons] {
        for (auto* button : timeframeButtons->findChildren<QPushButton*>()) {
            const QSignalBlocker block(button);
            const auto index = timeframeSelector_->findData(button->objectName().section('_', 1).toLongLong());
            button->setChecked(index == timeframeSelector_->currentIndex()); button->setEnabled(timeframeSelector_->isEnabled());
        }
        refreshTimeframeButtons();
    };
    connect(timeframeSelector_, &QComboBox::currentIndexChanged, timeframeButtons, syncTimeframeButtons);
    syncTimeframeButtons_ = syncTimeframeButtons; syncTimeframeButtons();
    timeframeLayout->addWidget(timeframeButtons);
    extendedSessions_ = new QCheckBox(tr("시간외 포함"), chartPane_);
    extendedSessions_->setObjectName("chartWorkspaceExtendedSessions");
    extendedSessions_->setAccessibleName(tr("주식 시간외 세션 포함 · 기본은 정규장"));
    extendedSessions_->hide(); timeframeLayout->addWidget(extendedSessions_);
    connect(extendedSessions_, &QCheckBox::toggled, this, &ChartWorkspace::setIncludeExtendedSessions);
    auto* more=new LocalMenuButton(chartPane_);more->setObjectName("chartMoreTools");more->setText(tr("더보기"));more->setAccessibleName(tr("차트 더보기"));more->setPopupMode(QToolButton::InstantPopup);
    auto* menu=new QMenu(more);applyLocalMenuStyle(menu);auto* publicShare=menu->addAction(tr("공개 차트 카드"));publicShare->setObjectName("chartPublicShareAction");
    connect(publicShare,&QAction::triggered,this,&ChartWorkspace::openShareCard);
    assetCardAction_=menu->addAction(tr("내 자산 카드 만들기"));assetCardAction_->setObjectName("chartAssetCardAction");assetCardAction_->setVisible(false);
    connect(assetCardAction_,&QAction::triggered,this,[this]{if(privateEnabled_&&!selectedHoldingAsset_.isEmpty())emit assetCardRequested(selectedHoldingAsset_);});
    // Pins are edited in the interval list itself; with every interval pinned
    // the toolbar has no list button, so the same list opens from here.
    timeframeFavorites_=menu->addAction(tr("시간봉 즐겨찾기"));timeframeFavorites_->setObjectName("chartTimeframeFavorites");
    connect(timeframeFavorites_,&QAction::triggered,this,[this]{QTimer::singleShot(0,this,[this]{
        QWidget* anchor=timeframeList_->isVisible()?static_cast<QWidget*>(timeframeList_):timeframeButtons_->isVisible()?timeframeButtons_:timeframeSelector_->isVisible()?static_cast<QWidget*>(timeframeSelector_):moreTools_;
        openTimeframeList(anchor);});});
    sidePanelAction_=menu->addAction(tr("체결 · 근거"));sidePanelAction_->setObjectName("chartSidePanelAction");
    sidePanelAction_->setCheckable(true);sidePanelAction_->setChecked(true);
    connect(sidePanelAction_,&QAction::toggled,this,[this](bool open){if(sideOpen_==open)return;if(!open&&sidePlaced_&&sidePanel_&&sidePanel_->isVisible())sideShare_.remember(*sideSplitter_);sideOpen_=open;layoutSidePanel();});
    more->setMenu(menu);more->setCompact();moreTools_=more;
    holdingContext_=new QLabel(chartPane_);holdingContext_->setObjectName("chartHoldingContext");holdingContext_->setTextFormat(Qt::PlainText);holdingContext_->setWordWrap(true);holdingContext_->setFont(KeefiFonts::font(FontRole::Caption12));holdingContext_->hide();chartLayout->addWidget(holdingContext_);
    gridSelector_=new LocalComboBox(chartPane_);gridSelector_->setObjectName("chartLayoutSelector");gridSelector_->setAccessibleName(tr("차트 분할 보기"));gridSelector_->setMinimumHeight(44);
    gridSelector_->addItem(tr("단일 차트"),1);gridSelector_->addItem(tr("2분할"),2);gridSelector_->hide();
    auto* layoutButtons=new QWidget(chartPane_);layoutButtons_=layoutButtons;auto* layoutRow=new QHBoxLayout(layoutButtons);layoutRow->setContentsMargins(0,0,0,0);layoutRow->setSpacing(2);
    auto* layoutGroup=new QButtonGroup(layoutButtons);layoutGroup->setExclusive(true);
    for(int index=0;index<gridSelector_->count();++index){
        auto* button=new QPushButton(gridSelector_->itemText(index),layoutButtons);button->setObjectName("chartLayout_"+gridSelector_->itemData(index).toString());
        button->setCheckable(true);button->setChecked(index==0);applySegmentedControlStyle(button, CompactButtonHeight);layoutGroup->addButton(button,index);layoutRow->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,index]{gridSelector_->setCurrentIndex(index);});
    }
    connect(gridSelector_,&QComboBox::currentIndexChanged,layoutButtons,[this,layoutGroup]{for(auto* button:layoutGroup->buttons()){const QSignalBlocker blocked(button);button->setChecked(layoutGroup->id(button)==gridSelector_->currentIndex());}});
    gridTools->addWidget(layoutButtons);
    auto* gridHelp=new QLabel(tr("칸 선택 → 목록에서 종목 변경 · 각 칸의 시간봉은 따로 유지해요"),chartPane_);gridHelp->setWordWrap(true);gridHelp->hide();chartLayout->addWidget(gridHelp);
    grid_=new ChartGridView(chartPane_);grid_->hide();
    auto* syncButton=new LocalMenuButton(chartPane_);syncButton->setObjectName("chartSyncMenuButton");syncButton->setText(QStringLiteral("Sync Charts"));
    syncButton->setAccessibleName(QStringLiteral("Sync Charts"));syncButton->setPopupMode(QToolButton::InstantPopup);syncButton->setEnabled(false);
    syncButton->setToolTip(tr("2분할에서 쓸 수 있어요"));
    auto* syncMenu=new QMenu(syncButton);syncMenu->setObjectName("chartSyncMenu");applyLocalMenuStyle(syncMenu);syncButton->setMenu(syncMenu);syncButton->setCompact();gridTools->insertWidget(1,syncButton);syncTools_=syncButton;gridTools->addWidget(more);
    const QList<QPair<ChartSyncOption,QString>> syncOptions{{ChartSyncOption::Symbol,QStringLiteral("Symbol")},{ChartSyncOption::Interval,QStringLiteral("Interval")},
        {ChartSyncOption::Crosshair,QStringLiteral("Crosshair")},{ChartSyncOption::Time,QStringLiteral("Time")},{ChartSyncOption::DateRange,QStringLiteral("Date Range")}};
    for(const auto& option:syncOptions){
        auto* action=syncMenu->addAction(option.second);action->setObjectName("chartSync_"+option.second.simplified().remove(' '));
        action->setCheckable(true);action->setChecked(false);
        if(option.first==ChartSyncOption::Time)action->setToolTip(tr("가로 이동 시 중심 UTC 시각을 맞추고 상대 확대율을 유지해요."));
        if(option.first==ChartSyncOption::DateRange)action->setToolTip(tr("보이는 시작·끝 UTC 시각을 맞춰요. Time보다 우선해요."));
        connect(action,&QAction::toggled,this,[this,kind=option.first](bool enabled){grid_->setSyncOption(kind,enabled);});
        connect(grid_, &ChartGridView::syncSettingsChanged, action, [this, action, kind = option.first] {
            const QSignalBlocker blocked(action);
            const auto settings = grid_->syncSettings();
            action->setChecked(settings.enabled(kind));
            if (kind == ChartSyncOption::Time) {
                action->setText(settings.enabled(ChartSyncOption::Time) && settings.enabled(ChartSyncOption::DateRange)
                    ? QStringLiteral("Time · Date Range 우선") : QStringLiteral("Time"));
            }
        });
    }
    connect(grid_,&ChartGridView::activated,this,[this](const QString& key,qint64 interval){
        const QScopedValueRollback guard(gridActivating_,true);
        const auto previousKey=selectedChartIdentity_;const auto previousInterval=timeframeMs_;
        // Commit identity and interval together: never start a request for the
        // new market at the previous panel's interval while switching panels.
        const auto syncInterval=[this](qint64 value){
            timeframeMs_=value;const QSignalBlocker block(timeframeSelector_);
            timeframeSelector_->setCurrentIndex(timeframeSelector_->findData(value));
            if(syncTimeframeButtons_)syncTimeframeButtons_();
        };
        syncInterval(interval);
        if(key.isEmpty()){
            selectedChartIdentity_.clear();asset_.clear();cancelContractRead();
            candles_->clearPrivate();candles_->setSelection({},interval);
            synchronizeChartSelection();refreshPositions();emit selectionChanged({},interval);
        }else if(setIdentity(key,browserPublicDrawingContext(key))){
            grid_->setSelection(selectedChartIdentity_,interval);
            if(previousKey==selectedChartIdentity_&&previousInterval!=interval)emit selectionChanged(selectedChartIdentity_,interval);
        }else{
            syncInterval(previousInterval);pendingChartIdentity_.clear();
            grid_->rollbackActivation(previousKey,previousInterval);
        }
    });
    connect(gridSelector_,&QComboBox::currentIndexChanged,this,[this,gridHelp,chartLayout,syncButton]{
        const auto count=gridSelector_->currentData().toInt();const bool multi=count>1;
        if(multi&&!selectedChartIdentity_.isEmpty()&&!(selectedChartIdentity_.startsWith("coin:upbit:")||selectedChartIdentity_.startsWith("coin:binance:"))){
            gridSelector_->setCurrentIndex(0);chartRouteMessage_=tr("2분할은 확인된 업비트·바이낸스 공개 현물부터 지원해요.");filterBrowserRows();return;
        }
        grid_->setSelection(selectedChartIdentity_,timeframeMs_);
        if(grid_->panelIdentity(0).isEmpty() || grid_->panelIdentity(1).isEmpty()){
            // A new split follows the current browser order, market and search even if its popup is closed.
            if(instrumentPanel_->isHidden())updateWatchlistRows(true);
            QStringList candidates;
            for(QTreeWidgetItemIterator it(watchlistRows_);*it;++it)
                if(!(*it)->isHidden())candidates.append((*it)->data(0,IdentityRole).toString());
            grid_->seed(candidates);
        }
        if(multi){
            chartLayout->removeWidget(candles_);grid_->setActiveView(candles_);
            grid_->setPanelCount(count);grid_->show();
        }else{
            grid_->setPanelCount(0);grid_->hide();grid_->setActiveView(nullptr);
            chartLayout->insertWidget(chartLayout->indexOf(grid_),candles_,1);
        }
        gridHelp->setVisible(multi);candles_->setVisible(chartSupported_);chartUnavailable_->setVisible(!chartSupported_);
        syncButton->setEnabled(multi);
        syncButton->setToolTip(multi ? QString{} : tr("2분할에서 쓸 수 있어요"));
        if(multi)grid_->observeActiveViewport(candles_->syncViewport(),candles_->followsLive(),candles_->nominalZoomBars());
        refreshWatchlistRows();
        layoutSidePanel();
    });
    candles_ = new MarketCandlesView(model, chartPane_);
    connect(candles_,&MarketCandlesView::viewportChanged,this,[this]{
        grid_->observeActiveViewport(candles_->syncViewport(),candles_->followsLive(),candles_->nominalZoomBars());
    });
    connect(candles_,&MarketCandlesView::userViewportNavigated,grid_,&ChartGridView::activeViewportNavigated);
    connect(candles_,&MarketCandlesView::crosshairHovered,grid_,&ChartGridView::activeCrosshairHovered);
    connect(grid_,&ChartGridView::activeViewportSyncRequested,candles_,&MarketCandlesView::applySyncedViewport);
    connect(grid_,&ChartGridView::activeViewportFollowLiveRequested,candles_,&MarketCandlesView::restoreLiveViewport);
    connect(grid_,&ChartGridView::activeCrosshairSyncRequested,candles_,&MarketCandlesView::setSyncedCrosshair);
    candles_->setToolbarHosts(instrumentTools_,frequentTools_,moreTools_);
    instrumentTitle_ = findChild<QLabel*>("marketCandlesTitle");
    auto* info = findChild<QWidget*>("marketCandlesInformation");
    priceLayout->removeWidget(instrumentTitle_);
    priceLayout->removeWidget(info);
    instrumentButton_ = new QPushButton(instrumentTools_);
    instrumentButton_->setObjectName("chartInstrumentButton");
    instrumentButton_->setAccessibleName(tr("종목 패널"));
    instrumentButton_->installEventFilter(this);
    applyCompactButton(instrumentButton_);
    auto* anchorLayout = new QHBoxLayout(instrumentButton_);
    anchorLayout->setContentsMargins(9, 0, 8, 0);
    anchorLayout->setSpacing(6);
    instrumentTitle_->setAttribute(Qt::WA_TransparentForMouseEvents);
    instrumentTitle_->installEventFilter(this);
    anchorLayout->addWidget(instrumentTitle_, 1);
    auto* chevron = new QLabel(QStringLiteral("⌄"), instrumentButton_);
    chevron->setFont(KeefiFonts::font(FontRole::Secondary14));
    chevron->setAttribute(Qt::WA_TransparentForMouseEvents);
    anchorLayout->addWidget(chevron);
    priceLayout->insertWidget(0, instrumentButton_);
    priceLayout->insertWidget(1, info);
    connect(instrumentButton_, &QPushButton::clicked, this, [this] {
        if (instrumentPanel_->isVisible()) {
            closeInstrumentPanel();
            return;
        }
        positionInstrumentPanel();
        instrumentPanel_->show();
        input_->setFocus(Qt::PopupFocusReason);
        filterBrowserRows();
        ensureBrowserCatalog();
        refreshTrendDemands();
    });
    styleTools_=findChild<QToolButton*>("chartStyleButton");
    foldSeparator_=menu->addSeparator();
    auto* layoutMenu=new QMenu(gridSelector_->accessibleName(),menu);applyLocalMenuStyle(layoutMenu);
    for(auto* button:layoutButtons_->findChildren<QPushButton*>()){
        auto* action=layoutMenu->addAction(button->text());action->setCheckable(true);
        connect(action,&QAction::triggered,button,&QPushButton::click);
        connect(layoutMenu,&QMenu::aboutToShow,action,[action,button]{action->setChecked(button->isChecked());action->setEnabled(button->isEnabled());});
    }
    foldLayout_=menu->addMenu(layoutMenu);foldLayout_->setObjectName("chartFoldedLayout");
    syncMenu->setTitle(syncButton->text());foldSync_=menu->addMenu(syncMenu);foldSync_->setObjectName("chartFoldedSync");
    foldStyle_=menu->addAction(styleTools_->toolTip().section('\n',0));foldStyle_->setObjectName("chartFoldedStyle");
    connect(foldStyle_,&QAction::triggered,styleTools_,&QToolButton::click);
    connect(menu,&QMenu::aboutToShow,this,[this]{foldSync_->setEnabled(syncTools_->isEnabled());foldStyle_->setText(styleTools_->toolTip().section('\n',0));});
    QWidget* previous=info;
    QList<QWidget*> focusOrder;
    for(auto* button:timeframeButtons_->findChildren<QPushButton*>())focusOrder<<button;
    focusOrder<<timeframeList_<<timeframeSelector_<<extendedSessions_<<findChild<QWidget*>("chartStudyPicker")<<findChild<QWidget*>("chartStudyAppliedToggle")<<styleTools_;
    for(auto* button:layoutButtons_->findChildren<QPushButton*>())focusOrder<<button;
    focusOrder<<syncTools_<<moreTools_;
    for(auto* widget:focusOrder){QWidget::setTabOrder(previous,widget);previous=widget;}
    QTimer::singleShot(0,this,[this]{layoutToolbar();});
    instrumentPresentation_ = new ChartInstrumentPresentation(this);
    candles_->setInstrumentPresentation(instrumentPresentation_);
    connect(instrumentPresentation_,&ChartInstrumentPresentation::changed,this,&ChartWorkspace::refreshWatchlistRows);
    candles_->setDrawingModel(drawings_);
    connect(candles_,&MarketCandlesView::stockConnectionRequested,this,&ChartWorkspace::stockConnectionRequested);
    connect(candles_,&MarketCandlesView::stockDailyRequested,this,[this]{setTimeframe(86400000);});
    connect(candles_,&MarketCandlesView::drawingAlertSettingsRequested,this,&ChartWorkspace::listAlertsRequested);
    refreshDrawingAlertLists();
    candles_->setIndicatorPreferences(indicatorPreferences_);
    connect(candles_, &MarketCandlesView::studiesChanged, this, [this] { refreshStudyPanels(); emit studiesChanged(); });
    connect(candles_, &MarketCandlesView::coinFlowStudiesChanged, this, [this]{refreshStudyPanels();emit coinFlowStudiesChanged();});
    connect(candles_,&MarketCandlesView::publicFlowObservationReady,this,&ChartWorkspace::publicFlowObservationReady);
    connect(timeframeSelector_, &QComboBox::currentIndexChanged, this, [this] { setTimeframe(timeframeSelector_->currentData().toLongLong()); });
    chartLayout->addWidget(candles_, 1);
    chartLayout->addWidget(grid_,1);
    chartUnavailable_ = new LedgerTextLabel(FontRole::Body15,chartPane_);
    chartUnavailable_->setObjectName("chartWorkspaceChartUnavailable");
    chartUnavailable_->setTextFormat(Qt::PlainText);
    chartUnavailable_->setWordWrap(true);
    chartUnavailable_->hide();
    chartLayout->addWidget(chartUnavailable_);
    liquidationRibbon_ = new LedgerTextLabel(FontRole::Body15,chartPane_);
    liquidationRibbon_->setObjectName("chartWorkspaceLiquidationRibbon");
    liquidationRibbon_->setAccessibleName(tr("선물 청산 거리 · 업체 mark 기준"));
    liquidationRibbon_->setTextFormat(Qt::PlainText);
    liquidationRibbon_->setWordWrap(true);
    setThemeStyle(liquidationRibbon_,"background:${bg};color:${danger};border:1px solid ${danger};padding:8px;");
    liquidationRibbon_->hide();chartLayout->addWidget(liquidationRibbon_);
    detailsToggle_=new QPushButton(tr("포지션 · 지금 상태 펼치기"),chartPane_);detailsToggle_->setObjectName("chartDetailsToggle");applyCompactButton(detailsToggle_);detailsToggle_->setCheckable(true);chartLayout->addWidget(detailsToggle_);
    connect(detailsToggle_,&QPushButton::toggled,this,[this](bool expanded){if(positions_)positions_->setVisible(expanded);updateDetailsToggleText();});

    auto* ledgerTitle = new QLabel(tr("시장 원장"), chartPane_); ledgerTitle->setObjectName("chartMarketLedgerTitle");
    ledgerTitle->setFont(KeefiFonts::font(FontRole::Title17)); ledgerTitle->hide();
    marketLedger_ = new ChartMarketLedger(chartPane_); marketLedger_->hide();
    evidenceNotes_ = new QWidget(chartPane_); evidenceNotes_->setObjectName("chartEvidenceNotes");
    auto* evidenceLayout = new QVBoxLayout(evidenceNotes_); evidenceLayout->setContentsMargins(0, 12, 0, 0); evidenceLayout->setSpacing(8);
    setThemeStyle(evidenceNotes_, "QWidget#chartEvidenceNotes{border-top:1px solid ${rule};} QLabel{color:${text3};border:0;}");
    chartLayout->addWidget(evidenceNotes_);
    positions_ = new QWidget(this);
    positions_->setObjectName("chartWorkspacePositions");
    auto* positionLayout = new QVBoxLayout(positions_);
    positionLayout->setContentsMargins(0, 0, 0, 0); positionLayout->setSpacing(16);
    positionCard_ = new QWidget(positions_); positionCard_->setObjectName("chartPositionPanel"); flatPanel(positionCard_);
    auto* privateLayout = new QVBoxLayout(positionCard_); privateLayout->setContentsMargins(16,16,16,16); privateLayout->setSpacing(10);
    auto* positionTitle = new QLabel(tr("내 포지션"), positionCard_);positionTitle->setObjectName("chartWorkspacePositionTitle");
    positionTitle->setProperty("type", "sectionHeading"); privateLayout->addWidget(positionTitle);
    positionLocked_ = new QLabel(positionCard_); positionLocked_->setObjectName("chartWorkspacePositionLocked");
    positionLocked_->setText(tr("금고를 열면 보유와 평균가를 볼 수 있어요."));
    positionLocked_->setFont(KeefiFonts::font(FontRole::Secondary14)); positionLocked_->setWordWrap(true);
    setThemeStyle(positionLocked_, "color:${text3};border:0;"); privateLayout->addWidget(positionLocked_, 1);
    positionUnlock_ = new QPushButton(tr("금고 열기"), positionCard_); positionUnlock_->setObjectName("chartWorkspaceUnlock");
    positionUnlock_->setMinimumSize(44,44); privateLayout->addWidget(positionUnlock_,0,Qt::AlignLeft);
    connect(positionUnlock_, &QPushButton::clicked, this, &ChartWorkspace::vaultOpenRequested);
    auto* privateScroll = new QScrollArea(positionCard_); privateScroll->setObjectName("chartPositionScroll");
    privateScroll->setWidgetResizable(true); privateScroll->setFrameShape(QFrame::NoFrame);
    privateScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); privateScroll->setMinimumHeight(0);
    positionText_ = new QLabel; positionText_->setObjectName("chartWorkspacePositionText");
    positionText_->setFont(KeefiFonts::font(FontRole::Body15)); positionText_->setTextFormat(Qt::RichText);
    positionText_->clear(); positionText_->hide(); positionText_->setWordWrap(true);
    positionText_->setTextInteractionFlags(Qt::TextSelectableByMouse); positionText_->setAlignment(Qt::AlignTop|Qt::AlignLeft);
    privateScroll->setWidget(positionText_); privateLayout->addWidget(privateScroll,1); privateScroll->hide();
    // Risk remains on the chart even while the details disclosure is closed.
    positionCard_->setMaximumHeight(220);positionLayout->addWidget(positionCard_, 0);
    scoreCard_=new QWidget(positions_);scoreCard_->setObjectName("chartScorePanel");flatPanel(scoreCard_);
    // Switching the two detail views must not move the user's chart/list scroll.
    scoreCard_->setMinimumHeight(340);
    auto* scoreLayout=new QVBoxLayout(scoreCard_);scoreLayout->setContentsMargins(16,12,16,12);scoreLayout->setSpacing(6);
    auto* stateHeading=new QHBoxLayout;auto* stateTitle=new QLabel(tr("지금 상태"),scoreCard_);stateTitle->setObjectName("chartIndicatorTitle");stateTitle->setFont(KeefiFonts::font(FontRole::BodyStrong15));stateHeading->addWidget(stateTitle,1);
    scoreToggle_=new QPushButton(tr("성적표"),scoreCard_);scoreToggle_->setObjectName("chartIndicatorScoreToggle");scoreToggle_->setMinimumSize(44,44);stateHeading->addWidget(scoreToggle_);
    auto* stateInfo=new InfoButton(scoreCard_);stateInfo->setObjectName("chartIndicatorSummaryInfo");stateInfo->setFixedSize(44,44);stateHeading->addWidget(stateInfo);scoreLayout->addLayout(stateHeading);
    statusBody_=new QWidget(scoreCard_);statusBody_->setObjectName("chartIndicatorStatusBody");auto* stateLayout=new QVBoxLayout(statusBody_);stateLayout->setContentsMargins(0,0,0,0);stateLayout->setSpacing(6);
    indicatorSummary_=new LedgerTextLabel(FontRole::Caption12,statusBody_);indicatorSummary_->setObjectName("chartIndicatorSummary");indicatorSummary_->setWordWrap(true);stateLayout->addWidget(indicatorSummary_);
    indicatorCompleteness_=new LedgerTextLabel(FontRole::Caption12,statusBody_);indicatorCompleteness_->setObjectName("chartIndicatorCompleteness");stateLayout->addWidget(indicatorCompleteness_);
    indicatorRows_=new QTreeWidget(statusBody_);indicatorRows_->setObjectName("chartIndicatorRows");indicatorRows_->setHeaderHidden(true);indicatorRows_->setRootIsDecorated(false);indicatorRows_->setColumnCount(1);indicatorRows_->setMinimumHeight(44);indicatorRows_->setMinimumWidth(0);indicatorRows_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);indicatorRows_->setFont(KeefiFonts::font(FontRole::Caption12));indicatorRows_->setItemDelegate(new IndicatorTextDelegate(indicatorRows_));
    setThemeStyle(indicatorRows_,"QTreeWidget{background:transparent;border:0;padding:0;} QTreeWidget::item{border-bottom:1px solid ${rule};} QTreeWidget::item:selected{background:${surface};color:${text};}");stateLayout->addWidget(indicatorRows_,1);scoreLayout->addWidget(statusBody_,1);
    const auto explainIndicator=[this](QTreeWidgetItem* item){if(item&&candles_)candles_->showIndicatorHelp(item->data(0,Qt::UserRole+1).toString(),indicatorRows_);};
    connect(indicatorRows_,&QTreeWidget::itemClicked,this,[explainIndicator](QTreeWidgetItem* item,int){explainIndicator(item);});connect(indicatorRows_,&QTreeWidget::itemActivated,this,[explainIndicator](QTreeWidgetItem* item,int){explainIndicator(item);});
    auto* scoreScroll=new QScrollArea(scoreCard_);scoreScroll->setFrameShape(QFrame::NoFrame);scoreScroll->setWidgetResizable(true);scoreScroll->setMinimumHeight(0);scoreScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scoreBody_=scoreScroll;scoreBody_->setObjectName("chartIndicatorScoreBody");auto* scoreContent=new QWidget(scoreScroll);auto* scoreBodyLayout=new QVBoxLayout(scoreContent);scoreBodyLayout->setContentsMargins(0,0,0,0);scoreBodyLayout->setSpacing(6);scoreScroll->setWidget(scoreContent);
    auto* scorecardTitle=new QLabel(chartStudyName("pullback"),scoreBody_);scorecardTitle->setObjectName("chartStudyScorecardTitle");scorecardTitle->setProperty("type","sectionHeading");
    scorecardTitle->hide();auto* scoreSelector=new LocalComboBox(scoreBody_);scoreSelector->setObjectName("chartStudyScorecardSelector");scoreSelector->setAccessibleName(tr("성적표 지표 선택"));scoreSelector->setMinimumHeight(44);scoreSelector->setMinimumWidth(0);scoreSelector->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for(const auto* id:{"pullback","whales","strong","breakout","ma","bb","rsi","macd"})scoreSelector->addItem(chartStudyName(id),QString::fromLatin1(id));
    auto* scoreHeading=new QHBoxLayout;scoreHeading->addWidget(scoreSelector,1);auto* scoreInformation=new InfoButton(scoreBody_);scoreInformation->setObjectName("chartStudyScorecardInfo");scoreInformation->setFixedSize(44,44);scoreHeading->addWidget(scoreInformation);scoreBodyLayout->addLayout(scoreHeading);
    scoreEmpty_=new LedgerTextLabel(FontRole::Caption12,scoreBody_);scoreEmpty_->setObjectName("chartStudyScorecardEmpty");scoreEmpty_->setFont(KeefiFonts::font(FontRole::Caption12));scoreEmpty_->setWordWrap(true);scoreBodyLayout->addWidget(scoreEmpty_);
    scorecardText_=new LedgerTextLabel(FontRole::Caption12,scoreBody_);scorecardText_->setObjectName("chartStudyScorecard");scorecardText_->setTextFormat(Qt::PlainText);scorecardText_->setWordWrap(true);scoreBodyLayout->addWidget(scorecardText_);
    auto* similarTitle=new QLabel(chartStudyName("similarity"),scoreBody_);similarTitle->setObjectName("chartStudySimilarityTitle");similarTitle->setWordWrap(true);auto* similarHeading=new QHBoxLayout;similarHeading->addWidget(similarTitle,1);auto* similarInformation=new InfoButton(scoreBody_);similarInformation->setObjectName("chartStudySimilarityInfo");similarInformation->setFixedSize(44,44);similarHeading->addWidget(similarInformation);scoreBodyLayout->addLayout(similarHeading);
    similaritySummary_=new LedgerTextLabel(FontRole::Caption12,scoreBody_);similaritySummary_->setObjectName("chartStudySimilaritySummary");similaritySummary_->setWordWrap(true);scoreBodyLayout->addWidget(similaritySummary_);scoreBodyLayout->addStretch();scoreBody_->hide();scoreLayout->addWidget(scoreBody_,1);
    connect(scoreSelector,&QComboBox::currentIndexChanged,this,[this,scoreSelector]{candles_->setScorecardStudy(scoreMode_?scoreSelector->currentData().toString():QString{});refreshStudyPanels();});
    connect(scoreToggle_,&QPushButton::clicked,this,[this,stateTitle,scoreSelector]{scoreMode_=!scoreMode_;statusBody_->setVisible(!scoreMode_);scoreBody_->setVisible(scoreMode_);stateTitle->setText(scoreMode_?tr("성적표"):tr("지금 상태"));scoreToggle_->setText(scoreMode_?tr("지금 상태"):tr("성적표"));candles_->setScorecardStudy(scoreMode_?scoreSelector->currentData().toString():QString{});refreshStudyPanels();});
    connect(indicatorPreferences_, &ChartStudyPreferences::changed, this, &ChartWorkspace::refreshStudyPanels);
    refreshStudyPanels(); refreshPositionPresentation();
    candles_->setAsset(asset_);
    buildSidePanel(panels);
    setWatchlistModel(new WatchlistWorkspaceList(this));
    if (model) connect(model, &MarketCandles::changed, this, [this] {
        if (chartSupported_ && selectedChartIdentity_.startsWith("coin:binance:")) {
            const auto data = candleModel_->series();
            const auto verifiedAsset = data.identity == selectedChartIdentity_ && data.intervalMs == timeframeMs_ && !data.currency.isEmpty() ? data.asset : QString{};
            if (asset_ != verifiedAsset) {
                cancelContractRead(); contract_ = {}; contractReceivedMs_ = 0; contractPending_ = true;
                contractReason_ = tr("선물 종목 identity 메타데이터 조회 대기"); asset_ = verifiedAsset;
                requestContracts();
            }
        }
        refreshPositions();
    });
    if (portfolio_) {
        connect(portfolio_, &PortfolioController::changed, this, [this] { refreshFuturesObservations(); if (isVisible()) refreshPositions(); });
        connect(portfolio_, &PortfolioController::aboutToReset, this, &ChartWorkspace::clearPrivate);
        connect(portfolio_, &QObject::destroyed, this, &ChartWorkspace::clearPrivate);
    }
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (futuresObservationActive_ && privateEnabled_) emit futuresPositionObservationsChanged();
        if (isVisible()) refreshPositions();
    });
    timer->start();
}
void ChartWorkspace::buildSidePanel(QBoxLayout* panels) {
    sideSplitter_ = new QSplitter(Qt::Horizontal, panels->parentWidget());
    sideSplitter_->setObjectName("chartWorkspaceSplitter");
    sideSplitter_->setChildrenCollapsible(false);
    sideSplitter_->setHandleWidth(12);
    setThemeStyle(sideSplitter_, "QSplitter#chartWorkspaceSplitter::handle{background:transparent;}");
    chartColumn_ = new QWidget(sideSplitter_);
    chartColumn_->setObjectName("chartWorkspaceMain");
    auto* column = new QVBoxLayout(chartColumn_);
    column->setContentsMargins(0, 0, 0, 0); column->setSpacing(12);
    column->addWidget(chartPane_, 1);
    column->addWidget(positions_);
    sidePanel_ = new QWidget(sideSplitter_);
    sidePanel_->setObjectName("chartSidePanel");
    auto* side = new QVBoxLayout(sidePanel_);
    side->setContentsMargins(0, 0, 0, 0); side->setSpacing(8);
    sideTabs_ = new QTabBar(sidePanel_);
    sideTabs_->setObjectName("chartSidePanelTabs");
    sideTabs_->setAccessibleName(tr("체결·청산과 근거"));
    applySegmentedControlStyle(sideTabs_, CompactButtonHeight);
    // Two halves of the panel width: a tab is never elided or scrolled away.
    sideTabs_->setExpanding(true);
    sideTabs_->setUsesScrollButtons(false);
    sideTabs_->setElideMode(Qt::ElideNone);
    // Reached with Tab; a mouse click chooses a tab without a focus ring on it.
    sideTabs_->setFocusPolicy(Qt::TabFocus);
    sideTabs_->addTab(tr("체결·청산"));
    sideTabs_->addTab(tr("근거"));
    sideStack_ = new QStackedWidget(sidePanel_);
    sideStack_->setObjectName("chartSidePanelStack");
    auto* tapeCard = new QWidget(sideStack_);
    tapeCard->setObjectName("chartTapePanel");
    flatPanel(tapeCard);
    auto* tapeLayout = new QVBoxLayout(tapeCard);
    tapeLayout->setContentsMargins(2, 6, 2, 6);
    tape_ = new CoinTapeView(tapeCard);
    tape_->setIdentity(selectedChartIdentity_);
    tapeLayout->addWidget(tape_, 1);
    sideStack_->addWidget(tapeCard);
    // The evidence tab is the existing current-state/scorecard card, moved whole.
    sideStack_->addWidget(scoreCard_);
    // Evidence first: it is what this card showed before, and a public trade
    // stream starts only once the tape tab is chosen.
    sideTabs_->setCurrentIndex(1);
    sideStack_->setCurrentIndex(1);
    connect(sideTabs_, &QTabBar::currentChanged, sideStack_, &QStackedWidget::setCurrentIndex);
    side->addWidget(sideTabs_);
    side->addWidget(sideStack_, 1);
    sideSplitter_->addWidget(chartColumn_);
    sideSplitter_->addWidget(sidePanel_);
    sideSplitter_->setStretchFactor(0, 1);
    sideSplitter_->setStretchFactor(1, 0);
    sidePlaced_ = true;
    panels->addWidget(sideSplitter_, 1);
    connect(sideSplitter_, &QSplitter::splitterMoved, this, [this] {
        sideUserSized_ = true;
        sideShare_.remember(*sideSplitter_);
    });
    connect(tape_, &CoinTapeView::shownChanged, this, &ChartWorkspace::updateCoinTapeVisibility);
    sideSplitter_->installEventFilter(this);
}
ChartDrawings* ChartWorkspace::drawingModel() const { return drawings_; }
void ChartWorkspace::setDrawingAlertEnabledLists(const QSet<QString>& enabled) {
    if(drawingAlertEnabledLists_==enabled)return;
    drawingAlertEnabledLists_=enabled;refreshDrawingAlertLists();
}
void ChartWorkspace::refreshDrawingAlertLists() {
    QMap<QString,QString> names;QMap<QString,QList<WatchlistIdentity>> lists;QList<WatchlistIdentity> verified;
    const auto selected=WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_);
    if(watchlists_){
        if(watchlists_->isCurrentlyVerified(selected))verified.append(selected);
        for(const auto& list:watchlists_->userLists()){
            QList<WatchlistIdentity> identities;for(const auto& entry:list.entries){identities.append(entry.identity);
                if(watchlists_->isCurrentlyVerified(entry.identity)&&!verified.contains(entry.identity))verified.append(entry.identity);}
            lists.insert(list.id,identities);if(identities.contains(selected))names.insert(list.id,list.name);
        }
    }
    if(drawings_){drawings_->setVerifiedInstruments(verified);drawings_->setPublicLists(lists);}
    if(candles_)candles_->setDrawingAlertLists(names,drawingAlertEnabledLists_);
}
ChartWorkspace::~ChartWorkspace() {
    if(studyFeed_){studyFeed_->clearOwner("chart-watch-trends");studyFeed_->clearOwner("chart-selected-studies");}
    if (stockSource_) disconnect(stockSource_, nullptr, this, nullptr);
    if (candles_) disconnect(candles_, nullptr, this, nullptr);
    if (drawings_) disconnect(drawings_, nullptr, this, nullptr);
    if (indicatorPreferences_) disconnect(indicatorPreferences_, nullptr, this, nullptr);
    if (candleModel_) disconnect(candleModel_, nullptr, this, nullptr);
    if (watchlists_) disconnect(watchlists_, nullptr, this, nullptr);
    cancelContractRead();
    if (contractTransport_) disconnect(contractTransport_, nullptr, this, nullptr);
}
void ChartWorkspace::setAsset(const QString& asset) {
    const auto value = asset.trimmed().toUpper();
    if (!QRegularExpression("\\A[A-Z0-9]{1,20}\\z").match(value).hasMatch()) return;
    setIdentity("coin:upbit:KRW-" + value);
}
QString ChartWorkspace::selectedChartIdentity() const { return selectedChartIdentity_; }
void ChartWorkspace::openShareCard(){
    if(sharePreview_){sharePreview_->raise();sharePreview_->activateWindow();return;}
    if(!candles_)return;ChartShareOptions options,capture;capture.includePublicDrawings=true;
    // Only eligible public geometry is captured; rendering remains opt-in in the dialog.
    auto snapshot=candles_->shareSnapshot(capture);
    auto* dialog=new ChartShareDialog(std::move(snapshot),options,this);sharePreview_=dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->open();
}
void ChartWorkspace::setStockSource(StockChartSource* source) {
    if (stockSource_ == source) return;
    if (stockSource_) disconnect(stockSource_, nullptr, this, nullptr);
    stockSource_ = source;
    candles_->setStockSource(source);
    if (source) {
        connect(source, &StockChartSource::changed, this, &ChartWorkspace::syncStockSourceState);
        connect(source, &QObject::destroyed, this, [this] { stockSource_ = nullptr; syncStockSourceState(); });
    }
    syncStockSourceState();
}
void ChartWorkspace::setIncludeExtendedSessions(bool include) {
    if (includeExtended_ == include) return;
    includeExtended_ = include;
    { const QSignalBlocker block(extendedSessions_); extendedSessions_->setChecked(include); }
    candles_->setIncludeExtendedSessions(include);
    emit sessionPolicyChanged(include);
}
void ChartWorkspace::setCoinFlowSource(CoinFlowSource* source){if(candles_)candles_->setCoinFlowSource(source);if(tape_)tape_->setSource(source);}
void ChartWorkspace::setPublicNameSources(PublicMarketCatalog* markets,PublicEquityBoard* board,ChartInstrumentCatalog* instruments){
    if(publicMarkets_)disconnect(publicMarkets_,&PublicMarketCatalog::changed,this,&ChartWorkspace::refreshRoutingSources);
    if(publicBoard_)disconnect(publicBoard_,&PublicEquityBoard::changed,this,&ChartWorkspace::refreshRoutingSources);
    if(publicInstruments_)disconnect(publicInstruments_,&ChartInstrumentCatalog::changed,this,&ChartWorkspace::refreshRoutingSources);
    publicMarkets_=markets;publicBoard_=board;publicInstruments_=instruments;
    routingEnabled_=routingEnabled_||markets||instruments;
    if(markets)connect(markets,&PublicMarketCatalog::changed,this,&ChartWorkspace::refreshRoutingSources);
    if(board)connect(board,&PublicEquityBoard::changed,this,&ChartWorkspace::refreshRoutingSources);
    if(instruments)connect(instruments,&ChartInstrumentCatalog::changed,this,&ChartWorkspace::refreshRoutingSources);
    instrumentPresentation_->setSources(markets,board,instruments);
    if(routingEnabled_&&pendingChartIdentity_.isEmpty())pendingChartIdentity_=selectedChartIdentity_;
    refreshRoutingSources();
}
void ChartWorkspace::refreshRoutingSources(){
    if(routingRefreshing_)return;
    const QScopedValueRollback busy(routingRefreshing_,true);
    if(routingEnabled_&&candleModel_){
        candleModel_->setVerifiedUpbitMarkets(publicMarkets_?publicMarkets_->verifiedMarkets():QStringList{},publicMarkets_&&publicMarkets_->marketListAvailable());
        candleModel_->setInstruments("binance",publicInstruments_?publicInstruments_->instruments("binance"):QList<MarketInstrument>{});
    }
    if(routingEnabled_&&studyFeed_)studyFeed_->setVerifiedUpbitMarkets(publicMarkets_?publicMarkets_->verifiedMarkets():QStringList{},publicMarkets_&&publicMarkets_->marketListAvailable());
    if(watchlists_){QList<WatchlistIdentity> stocks;
        if(publicBoard_)for(const auto& pair:{std::pair{"kr","KR"},std::pair{"us","US"}})
            for(const auto& row:publicBoard_->items(pair.first))stocks.append({"stock",pair.second,row.symbol});
        watchlists_->setPublicStockInstruments(stocks);
    }
    if(!pendingChartIdentity_.isEmpty()){const auto key=pendingChartIdentity_;setIdentity(key,pendingPublicDrawingContext_);}
    refreshMetadataFailurePresentation();
    refreshWatchlistRows();
}
QString ChartWorkspace::metadataFailureFor(const WatchlistIdentity& identity) const {
    if(identity.market!="coin")return {};
    if(identity.source=="upbit"){
        if(!publicMarkets_)return {};
        if(publicMarkets_->marketListState()==PublicMarketCatalog::MarketListState::Failed)
            return tr("업비트 · %1").arg(publicMarkets_->marketListError());
        if(!publicMarkets_->marketListAvailable()||publicMarkets_->verifiedMarkets().contains(identity.code))return {};
    }else if(identity.source!="binance")return {};
    const auto reason=publicInstruments_?publicInstruments_->failureReason("binance"):QString{};
    return reason.isEmpty()?QString{}:tr("바이낸스 · %1").arg(reason);
}
void ChartWorkspace::refreshMetadataFailurePresentation(){
    if(!candles_)return;
    const auto key=pendingChartIdentity_.isEmpty()?selectedChartIdentity_:pendingChartIdentity_;
    const auto failure=metadataFailureFor(WatchlistWorkspaceList::identityFromKey(key));
    candles_->setMetadataFailure(failure.isEmpty()?QString{}:key,failure);
}
CoinFlowStudySet ChartWorkspace::coinFlowStudies()const{return candles_?candles_->coinFlowStudies():CoinFlowStudySet{};}
bool ChartWorkspace::includeExtendedSessions() const { return includeExtended_; }
void ChartWorkspace::syncStockSourceState() {
    if (stockStateSyncing_) return;
    const QScopedValueRollback syncing(stockStateSyncing_, true);
    const bool stock = selectedChartIdentity_.startsWith("stock:");
    extendedSessions_->setVisible(stock);
    timeframeSelector_->setEnabled(true); if (syncTimeframeButtons_) syncTimeframeButtons_();
    extendedSessions_->setEnabled(true);
    timeframeSelector_->setToolTip({});
    refreshMarketLedger();
}
void ChartWorkspace::setTimeframe(qint64 intervalMs) {
    if (!timeframeSelector_ || timeframeSelector_->findData(intervalMs) < 0 || timeframeMs_ == intervalMs) return;
    timeframeMs_ = intervalMs;
    if(grid_&&grid_->isVisible()&&!gridActivating_)grid_->setSelection(selectedChartIdentity_,intervalMs);
    const QSignalBlocker block(timeframeSelector_);
    timeframeSelector_->setCurrentIndex(timeframeSelector_->findData(intervalMs));
    if (syncTimeframeButtons_) syncTimeframeButtons_();
    candles_->clearPrivate();
    if (chartSupported_) candles_->setSelection(selectedChartIdentity_, timeframeMs_);
    refreshPositions();
    emit selectionChanged(selectedChartIdentity_, timeframeMs_);
}
qint64 ChartWorkspace::timeframeMs() const { return timeframeMs_; }
bool ChartWorkspace::openStockFromNavigation(const QString& key) {
    const auto stock = WatchlistWorkspaceList::identityFromKey(key);
    if (stock.market != "stock" || !WatchlistWorkspaceList::validIdentity(stock)) return false;
    if (gridSelector_->currentData().toInt() > 1)
        gridSelector_->setCurrentIndex(gridSelector_->findData(1));
    return selectedChartIdentity_ == key || setIdentity(key);
}

bool ChartWorkspace::setIdentity(const QString& requestedKey, bool publicDrawingContext) {
    QString key=requestedKey;
    auto identity=WatchlistWorkspaceList::identityFromKey(key);
    if(!WatchlistWorkspaceList::validIdentity(identity))return false;
    if(grid_&&grid_->isVisible()&&identity.market!="coin"){chartRouteMessage_=splitStockChartMessage();filterBrowserRows();return false;}
    QString routeNotice;
    if(identity.market=="coin"&&routingEnabled_){
        const auto route=resolveChartMarket(identity,publicMarkets_,publicInstruments_);
        if(!route.available()){
            pendingChartIdentity_=route.availability==ChartMarketAvailability::Checking?requestedKey:QString{};
            pendingPublicDrawingContext_=publicDrawingContext;
            chartRouteMessage_=route.reason;
            if(const auto failure=metadataFailureFor(identity);!failure.isEmpty())
                chartRouteMessage_=tr("종목 목록 조회 실패 · %1").arg(failure);
            if(input_)filterBrowserRows();else showListMessage(chartRouteMessage_);
            if(instrumentPicker_)instrumentPicker_->setFeedback(chartRouteMessage_);
            refreshMetadataFailurePresentation();
            synchronizeChartSelection();
            // Missing Upbit evidence does not yet establish a fallback need.
            // Keep initial hidden chart setup free of an unrelated Binance read.
            if(publicInstruments_&&route.availability==ChartMarketAvailability::Checking&&
               (identity.source=="binance"||(publicMarkets_&&publicMarkets_->marketListAvailable())))
                publicInstruments_->ensure("binance");
            return false;
        }
        identity=route.identity;key=identity.key();
        if(route.fallbackUsed){
            routeNotice=tr("업비트 원화 시장에 없어 확인된 바이낸스 현물로 열었어요 · USDT");
            if(watchlists_)watchlists_->replaceIdentity(currentListId(),WatchlistWorkspaceList::identityFromKey(requestedKey),identity);
        }
    }
    pendingChartIdentity_.clear();
    // A well-formed symbol can open a connection prompt. Only saved or known
    // symbols may carry public drawings; candle evidence still comes from the broker.
    if(identity.market=="stock"&&!canSelectIdentity(watchlists_,identity)&&!knownPublicStock(key))publicDrawingContext=false;
    chartRouteMessage_=routeNotice;
    const bool identityChanged = selectedChartIdentity_ != key;
    selectedChartIdentity_ = key;
    if(grid_&&grid_->isVisible()&&!gridActivating_)grid_->setSelection(key,timeframeMs_);
    refreshMetadataFailurePresentation();
    if(input_&&browserMarket(identity)!=browserMarket_)setBrowserMarket(browserMarket(identity),false);
    browserStates_[browserMarket_].selected=key;
    candles_->setDrawingPublicContext(publicDrawingContext);
    refreshDrawingAlertLists();
    const bool stock = identity.market == "stock";
    chartSupported_ = stock || (identity.market == "coin" && (identity.source == "upbit" || identity.source == "binance"));
    QString nextAsset;
    if (chartSupported_ && identity.source == "upbit") nextAsset = identity.code.section('-', 1);
    else if (!stock && chartSupported_ && candleModel_) {
        const auto data = candleModel_->series();
        if (data.identity == key && data.intervalMs == timeframeMs_ && !data.currency.isEmpty()) nextAsset = data.asset;
    }
    if (asset_ != nextAsset || identityChanged) {
        cancelContractRead(); contract_ = {}; contractReceivedMs_ = 0;
        contractReason_ = tr("선물 종목 identity 메타데이터 조회 대기");
        contractPending_ = chartSupported_ && !stock; asset_ = nextAsset;
        candles_->clearPrivate();
    }
    if (chartSupported_) {
        chartUnavailable_->hide();
        candles_->setSelection(key, timeframeMs_);
        candles_->show();
        requestContracts();
    } else {
        candles_->setSelection({}, timeframeMs_); candles_->clearPrivate(); candles_->hide();
        chartUnavailable_->setText(tr("%1 · 선택 종목의 차트 원천 자료 없음").arg(identity.code));
        chartUnavailable_->show();
    }
    syncStockSourceState();
    refreshPositions();
    synchronizeChartSelection();
    if(!routeNotice.isEmpty())showListMessage(routeNotice);
    if (identityChanged) emit selectionChanged(selectedChartIdentity_, timeframeMs_);
    return true;
}
void ChartWorkspace::synchronizeChartSelection() {
    if (tape_) tape_->setIdentity(selectedChartIdentity_);
    for (auto* tree : {holdingsRows_, watchlistRows_, trendRows_}) {
        if (!tree) continue;
        QTreeWidgetItem* match = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0,IdentityRole).toString()==selectedChartIdentity_) { match=*it; break; }
        const QSignalBlocker blocker(tree);
        if (tree->currentItem()!=match) tree->setCurrentItem(match);
        tree->clearSelection();
        if (match) match->setSelected(true);
    }
}
void ChartWorkspace::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutSidePanel();
    if (instrumentPanel_ && instrumentPanel_->isVisible()) positionInstrumentPanel();
    scheduleVisibleRows();
}
void ChartWorkspace::showEvent(QShowEvent* event) {
    QTimer::singleShot(0,this,[this]{refreshTrendDemands();refreshSelectedStudies();});
    QWidget::showEvent(event);
    layoutSidePanel();
    refreshWatchlists();
    ensureBrowserCatalog();
    contractPending_ = true;
    contractRefresh_->start();
    requestContracts();
    refreshPositions();
    scheduleVisibleRows();
}
void ChartWorkspace::hideEvent(QHideEvent* event) {
    if (instrumentPanel_) instrumentPanel_->hide();
    if(browserRetryTimer_)browserRetryTimer_->stop();
    ++selectedStudyGeneration_;selectedStudyKey_.clear();selectedStudyAppliedKey_.clear();++trendGeneration_;trendKey_.clear();if(studyFeed_){studyFeed_->clearOwner("chart-watch-trends");studyFeed_->clearOwner("chart-selected-studies");}
    QWidget::hideEvent(event);
    if (observedFuturesSymbols().isEmpty()) { cancelContractRead(); contractRefresh_->stop(); }
    contractPending_ = true;
    contract_ = {};
    contractReceivedMs_ = 0;
    scheduleVisibleRows();
}
void ChartWorkspace::layoutToolbar(){
    if(!toolbar_||!styleTools_||toolbarLayoutActive_)return;
    const QScopedValueRollback guard(toolbarLayoutActive_,true);
    const int room=toolbar_->width(),gap=8;
    if (!instrumentTitle_) return;
    constexpr int anchorChrome = 38;
    constexpr int minimumTitle = 48;
    int titleWidth = qBound(minimumTitle, instrumentTitle_->sizeHint().width(), 240);
    const bool relatedAvailable = relatedMarkets_->property("contextAvailable").toBool();
    const bool conversionAvailable = priceConversion_->property("contextAvailable").toBool();
    bool showRelated = relatedAvailable && window()->width() > 1440;
    bool showConversion = conversionAvailable;
    bool foldGrid=false,foldStyle=false,foldTime=false;
    const auto natural=[&](QWidget* group){
        int width=20,count=0;auto* row=group->layout();
        for(int i=0;i<row->count();++i)if(auto* child=row->itemAt(i)->widget()){
            const bool visible=child==relatedMarkets_?showRelated:child==priceConversion_?showConversion:
                child==timeframeButtons_?!foldTime:child==timeframeSelector_?foldTime:
                child==layoutButtons_||child==syncTools_?!foldGrid:
                child==styleTools_?!foldStyle:!child->isHidden();
            if(!visible)continue;
            width+=child==relatedMarkets_?120:child==instrumentButton_?titleWidth+anchorChrome:qBound(child->minimumWidth(),child->sizeHint().width(),child->maximumWidth());++count;
        }
        return width+qMax(0,count-1)*6;
    };
    int instrument=natural(instrumentTools_),frequent=natural(frequentTools_),secondary=natural(secondaryTools_);
    if(instrument+frequent+secondary+2*gap>room){showRelated=false;instrument=natural(instrumentTools_);}
    // Public conversion/premium can arrive after the first layout. Fold this
    // secondary context before allowing it to displace the chart controls.
    if(instrument+frequent+secondary+2*gap>room){showConversion=false;instrument=natural(instrumentTools_);}
    const int deficit = instrument + frequent + secondary + 2 * gap - room;
    if (deficit > 0 && deficit <= titleWidth - minimumTitle) {
        titleWidth -= deficit;
        instrument = natural(instrumentTools_);
    }
    const bool twoRows=instrument+frequent+secondary+2*gap>room;
    if(twoRows){
        showConversion=conversionAvailable;
        instrument=natural(instrumentTools_);
        if(instrument>room){showConversion=false;instrument=natural(instrumentTools_);}
        showRelated=relatedAvailable&&instrument+120<room;
        if(frequent+secondary+gap>room){foldGrid=true;secondary=natural(secondaryTools_);}
        if(frequent+secondary+gap>room){foldStyle=true;frequent=natural(frequentTools_);}
        if(frequent+secondary+gap>room){foldTime=true;frequent=natural(frequentTools_);}
        titleWidth=qMax(minimumTitle,qMin(titleWidth,room-(natural(instrumentTools_)-titleWidth)));
    }
    instrumentButton_->setFixedWidth(titleWidth + anchorChrome);
    instrumentTitle_->setFixedWidth(titleWidth);
    relatedMarkets_->setVisible(showRelated);priceConversion_->setVisible(showConversion);
    timeframeButtons_->setVisible(!foldTime);timeframeSelector_->setVisible(foldTime);
    layoutButtons_->setVisible(!foldGrid);syncTools_->setVisible(!foldGrid);styleTools_->setVisible(!foldStyle);
    foldSeparator_->setVisible(foldGrid||foldStyle);foldLayout_->setVisible(foldGrid);foldSync_->setVisible(foldGrid);foldStyle_->setVisible(foldStyle);
    toolbar_->setProperty("gridFolded",foldGrid);toolbar_->setProperty("styleFolded",foldStyle);toolbar_->setProperty("timeframeFolded",foldTime);
    toolbar_->setProperty("rows",twoRows?2:1);toolbar_->setFixedHeight(twoRows?96:44);
    if(twoRows){
        instrumentTools_->setGeometry(0,0,room,44);
        frequentTools_->setGeometry(0,52,qMax(0,room-secondary-gap),44);
        secondaryTools_->setGeometry(qMax(0,room-secondary),52,secondary,44);
    }else{
        instrumentTools_->setGeometry(0,0,qMax(0,room-frequent-secondary-2*gap),44);
        frequentTools_->setGeometry(room-frequent-secondary-gap,0,frequent,44);
        secondaryTools_->setGeometry(room-secondary,0,secondary,44);
    }
    // Geometry/visibility changes already invalidate the affected Qt layout.
    // Explicit invalidation here would enqueue another LayoutRequest forever.
    for(auto* group:{instrumentTools_,frequentTools_,secondaryTools_})group->layout()->activate();
    if (instrumentPanel_->isVisible()) positionInstrumentPanel();
}


} // namespace keefi
