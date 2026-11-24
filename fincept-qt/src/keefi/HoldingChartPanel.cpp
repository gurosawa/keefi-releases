#include "keefi/PlainToolTip.h"
#include "keefi/HoldingChartPanel.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/InfoButton.h"
#include "keefi/LocalComboBox.h"
#include "keefi/LocalMenuControls.h"
#include "keefi/PortfolioPresentation.h"
#include <QBoxLayout>
#include <QComboBox>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimeZone>
#include <QTimer>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
namespace keefi {
namespace {
constexpr int HoldingLabelRole=Qt::UserRole+60;
constexpr int HoldingChartAvailableRole=Qt::UserRole+61;
constexpr int HoldingQuoteStatusRole=Qt::UserRole+62;
class HoldingRowDelegate final:public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override {
        auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(32);return size;
    }
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override {
        QStyleOptionViewItem background(option);initStyleOption(&background,index);background.text.clear();
        background.features&=~QStyleOptionViewItem::HasCheckIndicator;
        const auto* style=option.widget?option.widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&background,painter,option.widget);
        painter->save();painter->setClipRect(option.rect,Qt::IntersectClip);
        const bool available=index.siblingAtColumn(0).data(HoldingChartAvailableRole).toBool();
        const QRectF area=QRectF(option.rect).adjusted(6,0,-6,0);
        if(index.column()==0){
            const auto label=index.data(HoldingLabelRole).toStringList();
            const auto status=index.data(HoldingQuoteStatusRole).toString();
            const auto captionFont=KeefiFonts::font(FontRole::Caption12);
            const qreal statusWidth=status.isEmpty()?0:qMin(qMax(0.0,area.width()),QFontMetricsF(captionFont).horizontalAdvance(status)+8);
            const qreal contentRight=area.right()-statusWidth-(statusWidth>0?6:0);
            painter->setPen(available?themeColors().text:themeColors().text3);
            const auto codeFont=KeefiFonts::font(FontRole::BodyStrong15);
            const qreal codeWidth=qMin(qMax(0.0,contentRight-area.left()),QFontMetricsF(codeFont).horizontalAdvance(label.value(0))+1);
            const qreal drawn=drawLedgerText(*painter,QRectF(area.left(),area.top(),codeWidth,area.height()),label.value(0),FontRole::BodyStrong15,Qt::AlignLeft,&codeFont,false);
            qreal x=area.left()+drawn+(drawn>0?6:0);
            const auto tag=label.value(2);
            const qreal tagWidth=tag.isEmpty()?0:qMin(qMax(0.0,contentRight-x),QFontMetricsF(captionFont).horizontalAdvance(tag)+8);
            const qreal nameBudget=qMax(0.0,contentRight-x-tagWidth-(tagWidth>0?6:0));
            painter->setPen(themeColors().text3);
            const auto nameWidth=drawLedgerText(*painter,QRectF(x,area.top(),nameBudget,area.height()),label.value(1),FontRole::Caption12);
            x+=nameWidth+(nameWidth>0?6:0);
            if(tagWidth>0){
                const QRectF badge(x,area.center().y()-9,tagWidth,18);
                painter->setPen(themeColors().rule);painter->setBrush(Qt::NoBrush);painter->drawRoundedRect(badge,3,3);
                painter->setPen(themeColors().text3);drawLedgerText(*painter,badge.adjusted(4,0,-4,0),tag,FontRole::Caption12);
            }
            if(statusWidth>0){
                const QRectF badge(area.right()-statusWidth,area.center().y()-9,statusWidth,18);
                painter->setPen(themeColors().rule);painter->setBrush(Qt::NoBrush);painter->drawRoundedRect(badge,3,3);
                painter->setPen(themeColors().text3);drawLedgerText(*painter,badge.adjusted(4,0,-4,0),status,FontRole::Caption12);
            }
        }else if(index.column()==2){
            const auto state=index.data(Qt::CheckStateRole);
            if(available){painter->setPen(state.isValid()?themeColors().text2:themeColors().text3);
                drawLedgerText(*painter,QRectF(option.rect),state.toInt()==Qt::Checked?QStringLiteral("★"):QStringLiteral("☆"),FontRole::Title17,Qt::AlignCenter);}
        }else if(index.column()==1&&!available){
            painter->setPen(themeColors().text3);
            drawLedgerText(*painter,QRectF(option.rect).adjusted(1,0,-1,0),index.data().toString(),FontRole::Caption12,Qt::AlignRight);
        }else{
            const auto direction=movementForSign(index.data(ChangeDirectionRole).toInt());
            painter->setPen(available?(index.column()==3?MovementColorPolicy::colorFor(option.widget,direction):themeColors().text):themeColors().text3);
            const auto value=index.data().toString();
            drawLedgerText(*painter,area,index.column()==3?movementDecoratedText(value,direction):value,FontRole::Number11,Qt::AlignRight);
        }
        painter->restore();
    }
    bool editorEvent(QEvent* event,QAbstractItemModel* model,const QStyleOptionViewItem& option,const QModelIndex& index)override {
        if(index.column()==2&&index.data(Qt::CheckStateRole).isValid()&&event->type()==QEvent::MouseButtonRelease){
            const auto* mouse=static_cast<QMouseEvent*>(event);
            if(mouse->button()==Qt::LeftButton&&option.rect.contains(mouse->position().toPoint()))
                return model->setData(index,index.data(Qt::CheckStateRole).toInt()==Qt::Checked?Qt::Unchecked:Qt::Checked,Qt::CheckStateRole);
        }
        if(index.column()==2&&event->type()==QEvent::MouseButtonPress)return false;
        return QStyledItemDelegate::editorEvent(event,model,option,index);
    }
};
QString custodyTags(const HoldingChartRow& row){
    if(row.chart&&row.chart->market=="stock")return row.chart->source=="KR"?HoldingChartPanel::tr("한국"):HoldingChartPanel::tr("미국");
    QStringList labels;
    for(const auto& custody:row.custodies){
        const auto label=custody.provider=="binance"?HoldingChartPanel::tr("바이낸스"):custody.label;
        if(!label.isEmpty()&&!labels.contains(label))labels.append(label);
    }
    return labels.join(QStringLiteral(" · "));
}
QString holdingPrice(const WatchlistRowState& state,QString& status){
    status.clear();
    static const QRegularExpression number(QStringLiteral("\\A([0-9]+(?:\\.[0-9]+)?)(?: [A-Z]{2,10})?\\z"));
    const auto match=number.match(state.price);
    if(match.hasMatch()){const double value=match.captured(1).toDouble();if(value>0&&std::isfinite(value)){
        if(state.quoteState==WatchlistQuoteState::Delayed)status=HoldingChartPanel::tr("지연");
        else if(state.quoteState==WatchlistQuoteState::Disconnected)status=HoldingChartPanel::tr("끊김");
        return priceAmount(match.captured(1));
    }}
    switch(state.quoteState){
    case WatchlistQuoteState::Disconnected:return HoldingChartPanel::tr("끊김");
    case WatchlistQuoteState::Unsupported:return HoldingChartPanel::tr("지원 없음");
    case WatchlistQuoteState::ConnectionRequired:return HoldingChartPanel::tr("연결 필요");
    case WatchlistQuoteState::Delayed:return HoldingChartPanel::tr("지연");
    case WatchlistQuoteState::Receiving:return QStringLiteral("—");
    }
    return QStringLiteral("—");
}
QString marketText(const WatchlistIdentity& market){
    const auto name=market.source=="upbit"?QStringLiteral("업비트"):market.source=="binance"?QStringLiteral("바이낸스"):market.source;
    return name+" "+market.code;
}
QString custodyText(const HoldingChartRow& row){
    QStringList labels;
    for(const auto& custody:row.custodies){
        auto value=custody.label;
        if(custody.state!=HoldingCustodyState::Fresh){
            value+=custody.state==HoldingCustodyState::Failed?QStringLiteral(" · 확인 실패"):QStringLiteral(" · 지연");
            if(custody.observedAtMs>0)value+=QStringLiteral(" · 마지막 확인 ")+QDateTime::fromMSecsSinceEpoch(custody.observedAtMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm KST");
        }
        if(!labels.contains(value))labels.append(value);
    }
    return labels.join(QStringLiteral(" / "));
}
QString phantomStakingScope(const HoldingChartRow& row){
    if(row.assetKey=="solana:native")for(const auto& custody:row.custodies)if(custody.provider=="phantom")
        return HoldingChartPanel::tr("Phantom은 지갑에 바로 있는 SOL만 읽어요. 스테이킹한 SOL은 아직 포함하지 않아요. jitoSOL 같은 스테이킹 토큰은 별도 토큰으로 보여요.");
    return {};
}
QString sessionMarketGuidance(){
    return HoldingChartPanel::tr("여기서 고른 시장은 이번 실행에만 기억해요. 계속 쓰려면 그 시장을 관심 목록에 추가하세요.");
}
}
HoldingChartPanel::HoldingChartPanel(QWidget* parent):QWidget(parent){
    setObjectName("chartHoldingPanel");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
    information_=new InfoButton(this);information_->setObjectName("chartHoldingInformation");
    information_->setInformation(tr("보유 자산"),sessionMarketGuidance());applyCompactButton(information_,true);information_->setEnabled(false);
    informationRow_=new QHBoxLayout;informationRow_->setContentsMargins(0,0,0,0);informationRow_->addStretch();informationRow_->addWidget(information_);layout->addLayout(informationRow_);
    empty_=new QLabel(tr("금고를 열면 보유 자산을 보여요."),this);empty_->setWordWrap(true);layout->addWidget(empty_);
    tree_=new QTreeWidget(this);tree_->setObjectName("chartHoldingAssets");tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("종목"),tr("가격"),QStringLiteral("★"),tr("24시간")});
    tree_->headerItem()->setData(2,Qt::AccessibleTextRole,tr("관심"));tree_->headerItem()->setTextAlignment(2,Qt::AlignCenter);
    tree_->setHeaderHidden(false);tree_->setRootIsDecorated(false);tree_->setMinimumHeight(128);
    tree_->setItemDelegate(new HoldingRowDelegate(tree_));tree_->setUniformRowHeights(true);
    tree_->header()->setFixedHeight(26);tree_->header()->setFont(KeefiFonts::font(FontRole::Caption12));
    tree_->header()->setStretchLastSection(false);tree_->header()->setMinimumSectionSize(0);tree_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    for(int column=1;column<4;++column)tree_->header()->setSectionResizeMode(column,QHeaderView::Fixed);
    tree_->setColumnWidth(1,96);tree_->setColumnWidth(2,24);tree_->setColumnWidth(3,78);tree_->header()->moveSection(2,3);
    tree_->setAccessibleName(tr("보유 자산 · 보관처와 차트 시장"));tree_->setFont(KeefiFonts::font(FontRole::Caption12));
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);tree_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);layout->addWidget(tree_,1);
    setThemeStyle(tree_,"QTreeWidget{background:${bg};border:0;padding:0;} QTreeWidget::item:selected,QTreeWidget::item:hover{background:${surface};color:${text};border-radius:6px;} QHeaderView::section{padding:0 4px;}");
    bindMovementPresentation(tree_,[this]{tree_->viewport()->update();});
    connect(tree_,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem* item,int column){
        if(column==2)return;const bool unsupported=column==1&&!item->data(0,HoldingChartAvailableRole).toBool();
        selectRow(item);if(unsupported)information_->openInformation();
    });
    connect(tree_,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* item,int column){if(column!=2)selectRow(item);});
    connect(tree_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* item,int column){if(column==2)toggleFavorite(item);});
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_,&QTreeWidget::customContextMenuRequested,this,&HoldingChartPanel::openContextMenu);
    auto* markets = new LocalComboBox(this);
    markets_ = markets;
    markets->setObjectName("chartHoldingMarketSelector");
    markets->setAccessibleName(tr("보유 자산의 차트 시장 · 이번 세션에만 적용"));
    markets->setCompact();
    markets_->hide();
    markets_->setToolTip(sessionMarketGuidance());markets_->setAccessibleDescription(sessionMarketGuidance());
    connect(markets_,&QComboBox::activated,this,[this](int){selectMarket(markets_->currentData().toString());});
    detail_=new QLabel(this);detail_->setObjectName("chartHoldingReason");detail_->setTextFormat(Qt::PlainText);detail_->setWordWrap(true);detail_->setFont(KeefiFonts::font(FontRole::Caption12));detail_->hide();
    setThemeStyle(detail_,"color:${text3};border:0;");updateDetails();
}
HoldingChartPanel::~HoldingChartPanel(){
    // The workspace can reparent the control into its header; its private copy still belongs to this panel.
    if(information_){information_->clearPrivate();if(information_->parentWidget()!=this)delete information_.data();}
}
QWidget* HoldingChartPanel::informationButton(){
    // The workspace places this next to its count; a standalone panel keeps it above the list.
    if(informationRow_){layout()->removeItem(informationRow_);delete informationRow_;informationRow_=nullptr;}
    return information_.data();
}
void HoldingChartPanel::setWatchlistContext(WatchlistWorkspaceList* model,const QString& listId){
    if(watchlists_==model&&listId_==listId)return;
    if(watchlists_!=model){
        disconnect(quoteConnection_);disconnect(modelDestroyedConnection_);watchlists_=model;
        if(model){
            quoteConnection_=connect(model,&WatchlistWorkspaceList::changed,this,&HoldingChartPanel::refreshQuotes);
            modelDestroyedConnection_=connect(model,&QObject::destroyed,this,[this]{watchlists_=nullptr;refreshQuotes();});
        }
    }
    listId_=listId;refreshQuotes();
}
void HoldingChartPanel::setCoverageReason(const QString& reason){
    if(coverageReason_==reason)return;
    coverageReason_=reason;syncInformation();
}
void HoldingChartPanel::refreshQuotes(){
    if(rows_.isEmpty())return;
    const QSignalBlocker block(tree_);
    const auto list=watchlists_?watchlists_->userList(listId_):WatchlistUserList{};
    const auto setData=[](QTreeWidgetItem* item,int column,int role,const QVariant& value){
        const auto displayed=role==Qt::ToolTipRole?QVariant(plainToolTip(value.toString())):value;
        if(item->data(column,role)!=displayed)item->setData(column,role,displayed);
    };
    for(int i=0;i<rows_.size()&&i<tree_->topLevelItemCount();++i){
        const auto& row=rows_[i];auto* item=tree_->topLevelItem(i);
        if(item->data(0,Qt::UserRole).toString()!=row.assetKey)continue;
        const auto state=row.chart&&watchlists_?watchlists_->rowState(*row.chart):WatchlistRowState{};
        QString status;
        setData(item,1,Qt::DisplayRole,row.chart?holdingPrice(state,status):tr("차트 자료 없음 ⓘ"));
        setData(item,0,HoldingQuoteStatusRole,status);
        setData(item,3,Qt::DisplayRole,row.chart?(state.change.isEmpty()?QStringLiteral("—"):state.change):QString{});
        int direction=0;
        static const QRegularExpression percent(QStringLiteral("\\A[+-][0-9]+(?:\\.[0-9]+)?%\\z"));
        if(percent.match(state.change).hasMatch()){bool valid=false;const auto value=state.change.chopped(1).toDouble(&valid);if(valid&&std::isfinite(value))direction=value>0?1:value<0?-1:0;}
        setData(item,3,ChangeDirectionRole,direction);
        const bool member=row.chart&&std::any_of(list.entries.cbegin(),list.entries.cend(),[&](const auto& entry){return entry.identity==*row.chart;});
        setData(item,2,Qt::CheckStateRole,row.chart&&!list.id.isEmpty()?QVariant(member?Qt::Checked:Qt::Unchecked):QVariant{});
        setData(item,2,Qt::AccessibleTextRole,list.id.isEmpty()?tr("관심 리스트를 먼저 선택하세요"):tr("%1에 %2").arg(list.name,member?tr("저장됨 · Space로 해제"):tr("Space로 추가")));
        setData(item,2,Qt::ToolTipRole,list.id.isEmpty()?tr("메뉴에서 관심 리스트를 만든 뒤 별표를 사용하세요."):tr("%1에 %2").arg(list.name,member?tr("저장됨"):tr("추가")));
        const auto scope=phantomStakingScope(row);
        const auto description=item->text(0)+(row.chartReason.isEmpty()?QString{}:"\n"+row.chartReason)+(scope.isEmpty()?QString{}:"\n"+scope);
        const auto priceTip=description+(state.price.isEmpty()?QString{}:"\n"+state.price)+(state.change.isEmpty()?QString{}:" · "+state.change);
        setData(item,1,Qt::ToolTipRole,priceTip);setData(item,3,Qt::ToolTipRole,priceTip);
        for(const int column:{1,3})setData(item,column,Qt::TextAlignmentRole,int(Qt::AlignRight|Qt::AlignVCenter));
    }
}
void HoldingChartPanel::toggleFavorite(QTreeWidgetItem* item){
    if(!item||!watchlists_||!item->data(2,Qt::CheckStateRole).isValid())return;
    const auto asset=item->data(0,Qt::UserRole).toString();
    const auto row=std::find_if(rows_.cbegin(),rows_.cend(),[&](const auto& value){return value.assetKey==asset;});
    if(row==rows_.cend()||!row->chart)return;
    const auto identity=*row->chart;const auto listId=listId_;const bool checked=item->checkState(2)==Qt::Checked;
    const QPointer<WatchlistWorkspaceList> source=watchlists_;
    // Model notifications can synchronously replace rows; finish the checkbox event first.
    QTimer::singleShot(0,this,[this,source,listId,identity,checked]{
        if(!source||source!=watchlists_||!std::any_of(rows_.cbegin(),rows_.cend(),[&](const auto& row){return row.chart&&*row.chart==identity;}))return;
        const auto list=source->userList(listId);if(list.id.isEmpty()){refreshQuotes();return;}
        const bool member=std::any_of(list.entries.cbegin(),list.entries.cend(),[&](const auto& entry){return entry.identity==identity;});
        if(member==checked)return;
        const bool changed=checked?source->addIdentity(list.id,identity):source->removeIdentity(list.id,identity);
        if(!changed)refreshQuotes();
    });
}
void HoldingChartPanel::openContextMenu(const QPoint& position){
    if(contextMenu_){contextMenu_->close();delete contextMenu_.data();}
    auto* item=tree_->itemAt(position);if(!item)return;
    const auto asset=item->data(0,Qt::UserRole).toString();
    const auto row=std::find_if(rows_.cbegin(),rows_.cend(),[&](const auto& value){return value.assetKey==asset;});
    if(row==rows_.cend()||row->candidates.size()<2)return;
    const auto candidates=row->candidates;const auto current=row->chart?row->chart->key():QString{};
    selectedAsset_=asset;tree_->setCurrentItem(item);updateDetails();
    auto* menu=new QMenu(this);contextMenu_=menu;menu->setObjectName("chartHoldingContextMenu");menu->setProperty("holdingAssetKey",asset);applyLocalMenuStyle(menu);
    auto* sources=menu->addMenu(tr("차트 출처"));sources->setObjectName("chartHoldingChartSources");applyLocalMenuStyle(sources);
    for(const auto& candidate:candidates){
        auto* action=sources->addAction(marketText(candidate));action->setData(candidate.key());action->setCheckable(true);action->setChecked(candidate.key()==current);
        connect(action,&QAction::triggered,this,[this,asset,key=candidate.key()]{
            if(selectedAsset_!=asset)return;
            selectMarket(key);
        });
    }
    menu->popup(tree_->viewport()->mapToGlobal(position));
}
void HoldingChartPanel::selectMarket(const QString& market){
    const auto asset=selectedAsset_;if(asset.isEmpty()||market.isEmpty())return;
    const auto row=std::find_if(rows_.cbegin(),rows_.cend(),[&](const auto& value){return value.assetKey==asset;});
    if(row==rows_.cend()||!std::any_of(row->candidates.cbegin(),row->candidates.cend(),[&](const auto& candidate){return candidate.key()==market;}))return;
    const bool changed=!row->chart||row->chart->key()!=market;
    const auto defaultMarket=defaultMarkets_.value(asset);const bool explain=changed&&!defaultMarket.isEmpty()&&market!=defaultMarket;
    const auto choice=markets_->findData(market);if(choice<0)return;markets_->setCurrentIndex(choice);
    const QPointer<HoldingChartPanel> alive(this);emit marketSelected(asset,market);
    // The owner refreshes rows and chart selection synchronously from this signal.
    if(!alive||selectedAsset_!=asset)return;
    auto lines=detail_->text().split('\n',Qt::SkipEmptyParts);lines.removeAll(sessionMarketGuidance());
    if(explain&&markets_->currentData().toString()==market)lines.append(sessionMarketGuidance());
    detail_->setText(lines.join('\n'));syncInformation();
}
void HoldingChartPanel::syncInformation(){
    const bool available=!rows_.isEmpty()||!coverageReason_.isEmpty();
    information_->setEnabled(available);
    if(!available){information_->clearPrivate();return;}
    QStringList paragraphs;
    if(!coverageReason_.isEmpty())paragraphs.append(coverageReason_);
    if(!detail_->text().isEmpty())paragraphs.append(detail_->text());
    information_->setPrivateAccess(true);information_->setPrivateInformation(paragraphs.join(QStringLiteral("\n\n")));
}
void HoldingChartPanel::setRows(const QList<HoldingChartRow>& rows){
    if(rows_==rows)return;
    if(contextMenu_){
        const auto asset=contextMenu_->property("holdingAssetKey").toString();
        const auto previous=std::find_if(rows_.cbegin(),rows_.cend(),[&](const auto& row){return row.assetKey==asset;});
        const auto next=std::find_if(rows.cbegin(),rows.cend(),[&](const auto& row){return row.assetKey==asset;});
        if(previous==rows_.cend()||next==rows.cend()||previous->candidates!=next->candidates){contextMenu_->close();delete contextMenu_.data();}
    }
    QHash<QString,QString> defaults;
    for(const auto& row:rows)if(row.chart){
        const auto market=row.selectionRule>0?row.chart->key():defaultMarkets_.value(row.assetKey);
        if(!market.isEmpty())defaults.insert(row.assetKey,market);
    }
    defaultMarkets_=std::move(defaults);
    bool sameOrder=tree_->topLevelItemCount()==rows.size();
    for(int i=0;sameOrder&&i<rows.size();++i)sameOrder=tree_->topLevelItem(i)->data(0,Qt::UserRole).toString()==rows[i].assetKey;
    const auto currentAsset=tree_->currentItem()?tree_->currentItem()->data(0,Qt::UserRole).toString():QString{};
    rows_=rows;const auto scroll=tree_->verticalScrollBar()->value();const QSignalBlocker blocker(tree_);
    if(!sameOrder)tree_->clear();
    QTreeWidgetItem *current=nullptr,*selected=nullptr;int index=0;
    for(const auto& row:rows_){
        auto* item=sameOrder?tree_->topLevelItem(index):new QTreeWidgetItem(tree_);++index;item->setData(0,Qt::UserRole,row.assetKey);
        const auto title=row.displayName.isEmpty()||row.displayName==row.symbol?row.symbol:row.displayName+" "+row.symbol;
        const auto chart=row.chart?marketText(*row.chart):tr("차트 지원 없음");
        item->setText(0,title+"\n"+tr("보유: ")+custodyText(row)+"\n"+tr("차트: ")+chart);
        item->setData(0,HoldingLabelRole,QStringList{row.symbol,row.displayName==row.symbol?QString{}:row.displayName,custodyTags(row)});
        item->setData(0,HoldingChartAvailableRole,row.chart.has_value());
        item->setSizeHint(0,QSize(0,32));item->setFlags(item->flags()|Qt::ItemIsUserCheckable);
        item->setData(0,Qt::AccessibleTextRole,item->text(0));
        const auto scope=phantomStakingScope(row);
        item->setToolTip(0,plainToolTip(item->text(0)+(row.chartReason.isEmpty()?QString{}:"\n"+row.chartReason)+(scope.isEmpty()?QString{}:"\n"+scope)));
        if(row.assetKey==currentAsset)current=item;
        if(row.assetKey==selectedAsset_)selected=item;
    }
    if(current)tree_->setCurrentItem(current);
    else if(selected)tree_->setCurrentItem(selected);
    if(!selected){selectedAsset_.clear();chartIdentity_.clear();}
    empty_->setVisible(rows_.isEmpty());empty_->setText(tr("표시할 보유 자산이 없어요."));
    refreshQuotes();setQuery(query_);tree_->verticalScrollBar()->setValue(scroll);updateDetails();
}
void HoldingChartPanel::setQuery(const QString& query){
    query_=query.trimmed();for(int i=0;i<tree_->topLevelItemCount();++i){auto* item=tree_->topLevelItem(i);item->setHidden(!query_.isEmpty()&&!item->text(0).contains(query_,Qt::CaseInsensitive));}
}
bool HoldingChartPanel::hasVisibleResults() const {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (!tree_->topLevelItem(i)->isHidden()) return true;
    return false;
}
void HoldingChartPanel::activateResult(QTreeWidgetItem* item) {
    if (item && !item->isHidden()) selectRow(item);
}
void HoldingChartPanel::selectRow(QTreeWidgetItem* item){
    if(!item)return;selectedAsset_=item->data(0,Qt::UserRole).toString();updateDetails();
    for(const auto& row:rows_)if(row.assetKey==selectedAsset_&&row.chart){emit chartActivated(row.assetKey,row.chart->key());break;}
}
void HoldingChartPanel::setChartIdentity(const QString& key){
    // Quote and position refreshes repeat this identity. They must not move
    // the user's keyboard selection or rebuild an open market selector.
    if(key==chartIdentity_)return;
    chartIdentity_.clear();
    if(key.isEmpty())return;
    for(const auto& row:rows_)if(row.chart&&row.chart->key()==key){
        chartIdentity_=key;selectedAsset_=row.assetKey;const QSignalBlocker block(tree_);
        for(int i=0;i<tree_->topLevelItemCount();++i)if(tree_->topLevelItem(i)->data(0,Qt::UserRole).toString()==selectedAsset_)tree_->setCurrentItem(tree_->topLevelItem(i));
        updateDetails();return;
    }
}
void HoldingChartPanel::updateDetails(){
    const QSignalBlocker blocker(markets_);detail_->clear();
    for(const auto& row:rows_)if(row.assetKey==selectedAsset_){
        bool sameMarkets=markets_->count()==row.candidates.size();
        for(int i=0;sameMarkets&&i<row.candidates.size();++i)
            sameMarkets=markets_->itemData(i).toString()==row.candidates[i].key()&&markets_->itemText(i)==marketText(row.candidates[i]);
        if(!sameMarkets){
            markets_->clear();
            for(const auto& market:row.candidates)markets_->addItem(marketText(market),market.key());
        }
        const auto market=row.chart?row.chart->key():QString{};
        if(!sameMarkets||market!=displayedMarket_)
            markets_->setCurrentIndex(row.chart?markets_->findData(market):row.candidates.isEmpty()?-1:0);
        displayedMarket_=market;
        QStringList lines;
        if(!row.chartReason.isEmpty())lines.append(row.chartReason);
        const auto scope=phantomStakingScope(row);if(!scope.isEmpty())lines.append(scope);
        if(!row.valuationMarket.isEmpty())lines.append(tr("평가 출처: ")+row.valuationMarket);
        if(!row.alertsEligible)lines.append(row.chart?tr("현재 보유 알림 범위에서 제외 · 최신 잔고 확인 필요"):tr("현재 보유 알림 범위에서 제외 · 지원 차트 없음"));
        detail_->setText(lines.join('\n'));syncInformation();return;
    }
    markets_->clear();markets_->hide();displayedMarket_.clear();detail_->hide();syncInformation();
}
void HoldingChartPanel::clearPrivate(){
    coverageReason_.clear();
    if(contextMenu_){contextMenu_->close();delete contextMenu_.data();}
    information_->clearPrivate();information_->setEnabled(false);
    rows_.clear();defaultMarkets_.clear();selectedAsset_.clear();query_.clear();chartIdentity_.clear();displayedMarket_.clear();tree_->clear();markets_->clear();markets_->hide();detail_->clear();detail_->hide();
    empty_->setText(tr("금고를 열면 보유 자산을 보여요."));empty_->show();
}
}
