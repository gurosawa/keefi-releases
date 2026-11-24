#include "keefi/ChartWorkspace.h"
#include <QSplitter>
#include "keefi/ChartWorkspacePresentation_p.h"
#include "keefi/HoldingChartPanel.h"
#include "keefi/ChartMarketResolver.h"
#include "keefi/ChartInstrumentPicker.h"
#include "keefi/InstrumentListTree.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicCoinIdentity.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/HoldingMarketResolver.h"

#include <QStyle>
#include <QScopedValueRollback>
#include <QDateTime>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <QComboBox>
#include <QScrollBar>
#include <QScreen>
#include <QToolButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTreeWidgetItemIterator>
#include <QApplication>
#include <QByteArray>
#include <QEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTreeWidgetItem>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>

namespace keefi {
using namespace chart_workspace_detail;
bool ChartWorkspace::knownPublicStock(const QString& key) const {
    const auto identity=WatchlistWorkspaceList::identityFromKey(key);
    if(identity.market!="stock"||!publicBoard_)return false;
    for(const auto& item:publicBoard_->items(identity.source.toLower()))if(item.symbol==identity.code)return true;
    return false;
}
bool ChartWorkspace::browserPublicDrawingContext(const QString& key) const {
    // The all scope also lists holdings. A row known only from holdings keeps
    // drawings transient; only saved or public-catalogue rows persist publicly.
    if(browserScope_=="holdings"||!watchlists_)return false;
    const auto identity=WatchlistWorkspaceList::identityFromKey(key);
    for(const auto& list:watchlists_->userLists())for(const auto& entry:list.entries)if(entry.identity==identity)return true;
    for(const auto& candidate:chartInstrumentCandidates(publicMarkets_,publicBoard_,publicInstruments_))if(candidate.key==key)return true;
    return false;
}
QString ChartWorkspace::browserSortMode() const {
    // The default lists the most traded instruments first; a saved watch list keeps its own order.
    if(browserSort_=="auto")return browserScope_=="watch"?QStringLiteral("manual"):QStringLiteral("value");
    return browserSort_;
}
void ChartWorkspace::sortBrowserColumn(int column) {
    if(browserScope_=="holdings")return;
    const auto current=browserSortMode();
    switch(column) {
    case 0:browserSort_=current=="name"?QStringLiteral("name-desc"):QStringLiteral("name");break;
    case 1:browserSort_=current=="price"?QStringLiteral("price-asc"):QStringLiteral("price");break;
    case 6:browserSort_=current=="gainers"?QStringLiteral("losers"):QStringLiteral("gainers");break;
    case 3:browserSort_=current=="value"?QStringLiteral("value-asc"):QStringLiteral("value");break;
    case 4:browserSort_=current=="volume"?QStringLiteral("volume-asc"):QStringLiteral("volume");break;
    case 5:browserSort_=current=="cap"?QStringLiteral("cap-asc"):QStringLiteral("cap");break;
    default:return;
    }
    browserStates_[browserMarket_].sort=browserSort_;
    if(browserSortSelector_){const QSignalBlocker block(browserSortSelector_);browserSortSelector_->setCurrentIndex(browserSortSelector_->findData(browserSort_));}
    refreshWatchlistRows();
}
void ChartWorkspace::updateBrowserHeaders() {
    if(!watchlistRows_)return;
    auto* header=watchlistRows_->header();const auto mode=browserSortMode();
    const int active=mode.startsWith("name")?0:mode.startsWith("price")?1:mode=="gainers"||mode=="losers"?6:mode.startsWith("value")?3:mode.startsWith("volume")?4:mode.startsWith("cap")?5:-1;
    const bool ascending=mode=="name"||mode=="losers"||mode.endsWith("-asc");
    const QString price=browserScope_=="all"?(browserMarket_=="upbit"||browserMarket_=="kr"?tr("가격 원"):browserMarket_=="binance"?tr("가격 USDT"):tr("가격 USD")):tr("가격");
    const QMap<int,QString> names{{0,tr("종목")},{1,price},{6,tr("24시간")},{3,tr("거래대금")},{4,tr("거래량")},{5,tr("시가총액")}};
    for(auto it=browserSortButtons_.cbegin();it!=browserSortButtons_.cend();++it) {
        const int column=it.key();auto* button=it.value();const bool sorted=column==active;
        const auto text=names.value(column)+(sorted?(ascending?QStringLiteral(" ▴"):QStringLiteral(" ▾")):QString{});
        if(watchlistRows_->headerItem()->text(column)!=text)watchlistRows_->headerItem()->setText(column,text);
        if(button->text()!=text)button->setText(text);
        if(button->property("sortActive").toBool()!=sorted){button->setProperty("sortActive",sorted);button->style()->unpolish(button);button->style()->polish(button);}
        button->setToolTip(watchlistRows_->headerItem()->toolTip(column));
        button->setGeometry(header->sectionViewportPosition(column),0,header->sectionSize(column),26);
        button->setVisible(!watchlistRows_->isColumnHidden(column));
        button->raise();
    }
    if(browserManual_)browserManual_->setVisible(browserScope_=="watch"&&mode!="manual");
    if(browserHint_)browserHint_->setText(browserScope_=="watch"?(mode!="manual"?tr("내 순서로 돌아가면 옮길 수 있어요"):tr("끌기 순서·섹션 이동 · ⌥↑↓ 이동 · Delete 관심 해제")):tr("Enter 열기 · Space 관심"));
}
QHash<QString,ChartWorkspace::BrowserMetric> ChartWorkspace::browserMetrics() const {
    QHash<QString,BrowserMetric> result;
    const auto rate=[](const QString& value,double scale=1.0)->std::optional<double>{bool ok=false;const auto n=value.toDouble(&ok);return ok&&std::isfinite(n)?std::optional<double>(n*scale):std::nullopt;};
    if(publicMarkets_)for(const auto& row:publicMarkets_->rows()){
        auto& value=result["coin:upbit:"+row.market];value.unit="KRW";value.base=row.base;
        if(row.priced){value.change=rate(row.signedChangeRate,100);value.price=rate(row.price);value.turnover=row.tradeValue24h.toDouble();value.volume=row.tradeVolume24h.toDouble();}
    }
    if(publicBoard_){
        QHash<QString,QString> bases;
        if(publicInstruments_)for(const auto& item:publicInstruments_->instruments("binance"))if(item.tradable&&item.market=="spot"&&item.quoteAsset=="USDT")bases.insert(item.symbol,item.baseAsset);
        for(const auto& row:publicBoard_->items("binance"))if(bases.contains(row.symbol)){
            auto& value=result["coin:binance:"+row.symbol];value.unit="USDT";value.base=bases.value(row.symbol);value.change=rate(row.changePercent);value.price=rate(row.price);value.turnover=row.tradedValue;value.volume=row.tradedUnits;
        }
        for(const auto& section:{QStringLiteral("kr"),QStringLiteral("us")})for(const auto& row:publicBoard_->items(section)){
            auto& value=result["stock:"+section.toUpper()+":"+row.symbol];value.unit=row.currency;value.base=tr("주");value.change=rate(row.changePercent);value.price=rate(row.price);value.turnover=row.tradedValue;value.volume=row.tradedUnits;value.cap=row.marketCap;value.capBasis=section=="kr"?tr("네이버 증권 · 시가총액"):tr("Nasdaq · 시가총액");
        }
        // One public identity join shared with Explore; never a holdings equivalence.
        QHash<QString,PublicMarketRow> names;if(publicMarkets_)for(const auto& row:publicMarkets_->rows())names.insert(row.base,row);
        QHash<QString,PublicBoardItem> caps;QSet<QString> ambiguous;
        for(const auto& row:publicBoard_->items("coin"))if(names.contains(row.symbol)&&samePublicCoin(names.value(row.symbol),row)&&row.currency=="KRW"){
            if(caps.contains(row.symbol))ambiguous.insert(row.symbol);else caps.insert(row.symbol,row);
        }
        for(auto it=result.begin();it!=result.end();++it)if(it.key().startsWith("coin:")&&caps.contains(it->base)&&!ambiguous.contains(it->base)){
            const auto cap=caps.value(it->base);it->cap=cap.marketCap;it->capBasis=tr("CoinGecko 글로벌 시가총액 · KRW · ")+cap.sourceId;
        }
    }
    return result;
}
QHash<QString,double> ChartWorkspace::browserTradeValues() const {
    // Display ordering only: 24h traded value in each tab's own quote currency. Never a valuation input.
    QHash<QString,double> values;
    if(publicMarkets_)for(const auto& row:publicMarkets_->rows()){
        bool ok=false;const auto value=row.tradeValue24h.toDouble(&ok);if(ok&&std::isfinite(value)&&value>0)values.insert("coin:upbit:"+row.market,value);
    }
    if(publicBoard_){
        // Binance quote volume stays in USDT: its rows are ranked only against each other in the Binance tab.
        for(const auto& item:publicBoard_->items("binance"))if(std::isfinite(item.tradedValue)&&item.tradedValue>0)values.insert("coin:binance:"+item.symbol,item.tradedValue);
        for(const auto& section:{QStringLiteral("kr"),QStringLiteral("us")})for(const auto& item:publicBoard_->items(section))
            if(std::isfinite(item.tradedValue)&&item.tradedValue>0)values.insert("stock:"+section.toUpper()+":"+item.symbol,item.tradedValue);
    }
    return values;
}
void ChartWorkspace::ensureBrowserCatalog() {
    if(!isVisible())return;
    if(coinBrowserMarket(browserMarket_)) {
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(publicBoard_&&(browserSortMode()=="cap"||(browserExpand_&&browserExpand_->isChecked()))&&
           publicBoard_->receivedAtMs("coin")==0&&!publicBoard_->marketBoardBusy()&&now-coinMetricsRequestedAtMs_>=60000){
            coinMetricsRequestedAtMs_=now;publicBoard_->refresh();
        }
        // Binance 24h tickers order the Binance tab and fill its rows before the stream; the board paces them to once a minute.
        if(browserMarket_=="binance"&&publicBoard_)publicBoard_->refreshBinanceRanking();
        if(publicMarkets_&&!publicMarkets_->marketListAvailable()&&!publicMarkets_->busy()&&
           publicMarkets_->marketListState()!=PublicMarketCatalog::MarketListState::Failed)publicMarkets_->refresh();
        if(publicInstruments_)publicInstruments_->ensure("binance");
    } else if(publicBoard_&&!publicBoard_->marketBoardBusy()&&publicBoard_->items(browserMarket_).isEmpty())publicBoard_->refresh();
}
void ChartWorkspace::setBrowserMarket(const QString& market,bool restoreSelection) {
    if(!QStringList{"upbit","binance","kr","us"}.contains(market)||browserMarket_==market)return;
    auto& previous=browserStates_[browserMarket_];previous.query=input_?input_->text():QString{};previous.scroll=watchlistRows_->verticalScrollBar()->value();previous.sort=browserSort_;
    if(browserMarket(WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_))==browserMarket_)previous.selected=selectedChartIdentity_;
    browserMarket_=market;const auto next=browserStates_.value(market);browserSort_=next.sort;
    if(browserSortSelector_){const QSignalBlocker block(browserSortSelector_);browserSortSelector_->setCurrentIndex(browserSortSelector_->findData(browserSort_));}
    if(auto* button=findChild<QPushButton*>("chartBrowserMarket_"+market)){const QSignalBlocker block(button);button->setChecked(true);}
    browserRestoring_=true;{const QSignalBlocker block(input_);input_->setText(next.query);}refreshWatchlistRows();browserRestoring_=false;
    watchlistRows_->verticalScrollBar()->setValue(next.scroll);ensureBrowserCatalog();
    if(restoreSelection&&!next.selected.isEmpty())setIdentity(next.selected,browserPublicDrawingContext(next.selected));
}
void ChartWorkspace::setBrowserScope(const QString& scope) {
    if(!QStringList{"all","holdings","watch"}.contains(scope)||browserScope_==scope)return;
    browserScope_=scope;browserSort_="auto";if(trendEnabled_)trendToggle_->setChecked(false);
    refreshWatchlistRows();ensureBrowserCatalog();
}
bool ChartWorkspace::openBrowserRow(QTreeWidgetItem* item) {
    if (!isVisibleInstrumentRow(item)) return false;
    const auto key = item->data(0, IdentityRole).toString();
    if (!setIdentity(key, browserPublicDrawingContext(key))) return false;
    emit identitySelected(selectedChartIdentity_);
    return true;
}
bool ChartWorkspace::openTrendRow(QTreeWidgetItem* item, int column) {
    if (!isVisibleInstrumentRow(item)) return false;
    const auto key = item->data(0, IdentityRole).toString();
    if (!setIdentity(key, currentListId() != HoldingsList)) return false;
    if (column > 0) setTimeframe(column == 1 ? 3600000 : column == 2 ? 14400000 : 86400000);
    emit identitySelected(selectedChartIdentity_);
    return true;
}
void ChartWorkspace::toggleBrowserFavorite(QTreeWidgetItem* item) {
    if(!item||!watchlists_)return;
    const auto identity=WatchlistWorkspaceList::identityFromKey(item->data(0,IdentityRole).toString());
    const auto check=item->data(2,Qt::CheckStateRole);
    if(!check.isValid()||!WatchlistWorkspaceList::validIdentity(identity))return;
    const auto listId=currentListId();const bool checked=check.toInt()==Qt::Checked;
    QPointer<WatchlistWorkspaceList> source=watchlists_;
    // Changing membership rebuilds the rows. Let Qt finish writing the checkbox
    // before that happens; the captured identity and list never depend on the row.
    QTimer::singleShot(0,this,[this,source,listId,identity,checked]{
        if(!source||source!=watchlists_)return;
        const auto list=source->userList(listId);
        if(list.id.isEmpty()){showListMessage(tr("관심에 저장할 리스트를 먼저 만들어 주세요."));return;}
        const bool member=std::any_of(list.entries.cbegin(),list.entries.cend(),[&](const auto& value){return value.identity==identity;});
        if(member==checked)return;
        const bool accepted=checked?source->addIdentity(list.id,identity):source->removeIdentity(list.id,identity);
        if(!accepted){refreshWatchlistRows();showListMessage(tr("종목을 원천에서 확인한 뒤 관심에 추가할 수 있어요."));}
    });
}
void ChartWorkspace::showListMessage(const QString& message, bool restorePanel) {
    if(!listStatus_)return;
    // Opening a modal file dialog dismisses the popup. Put a failed action's
    // feedback back on screen after that dialog has finished.
    if(restorePanel&&isVisible()&&!instrumentPanel_->isVisible()) {
        positionInstrumentPanel();
        instrumentPanel_->show();
        listMenuButton_->setFocus(Qt::PopupFocusReason);
    }
    listStatus_->setText(message);
    listStatus_->setVisible(!message.trimmed().isEmpty());
}
void ChartWorkspace::filterBrowserRows() {
    if(!input_||!watchlistRows_)return;
    holdingPanel_->setQuery(input_->text());
    const QSignalBlocker block(watchlistRows_);const auto query=input_->text().trimmed();int shown=0;
    const bool queryChanged=browserStates_.value(browserMarket_).query!=query;
    bool layoutChanged=false;
    for(QTreeWidgetItemIterator it(watchlistRows_);*it;++it) {
        auto* item=*it;const auto key=item->data(0,IdentityRole).toString();if(key.isEmpty())continue;
        const auto identity=WatchlistWorkspaceList::identityFromKey(key);
        const bool match=browserMarket(identity)==browserMarket_&&(query.isEmpty()||item->data(0,BrowserSearchRole).toString().contains(query,Qt::CaseInsensitive));
        if(item->isHidden()!=!match){item->setHidden(!match);layoutChanged=true;}if(match)++shown;
    }
    for(int i=0;i<watchlistRows_->topLevelItemCount();++i){auto* section=watchlistRows_->topLevelItem(i);if(!section->data(0,IdentityRole).toString().isEmpty())continue;
        bool children=false;for(int j=0;j<section->childCount();++j)children=children||!section->child(j)->isHidden();
        const bool emptyEditable=browserScope_=="watch"&&query.isEmpty()&&section->childCount()==0;
        if(section->isHidden()!=!(children||emptyEditable)){section->setHidden(!(children||emptyEditable));layoutChanged=true;}
        if(children&&!query.isEmpty()&&(queryChanged||browserRestoring_)&&!section->isExpanded()){section->setExpanded(true);layoutChanged=true;}
    }
    // QTreeWidget normally defers row layout. Restore a market's scroll only
    // after its visible range is current, instead of clamping it to the old market.
    if(layoutChanged)watchlistRows_->doItemsLayout();
    const QString scope=browserMarket_=="kr"?tr("공개 목록: KOSPI 최대 50개 + 보유·관심"):browserMarket_=="us"?tr("공개 목록: Nasdaq 거래소 최대 80개 + 보유·관심"):QString{};
    watchlistRows_->headerItem()->setText(1,browserScope_=="all"?(browserMarket_=="upbit"||browserMarket_=="kr"?tr("가격 원"):browserMarket_=="binance"?tr("가격 USDT"):tr("가격 USD")):tr("가격"));
    watchlistRows_->headerItem()->setToolTip(1,browserMarket_=="upbit"?tr("전일 종가 · 09:00 KST (UTC+9) 기준"):browserMarket_=="binance"?tr("바이낸스 현물 · 최근 24시간"):tr("거래일 전일 대비"));
    watchlistRows_->headerItem()->setToolTip(6,watchlistRows_->headerItem()->toolTip(1));
    updateBrowserHeaders();
    // The Upbit catalogue matters on the Upbit tab, and on the Binance tab while an Upbit coin is being opened.
    const auto routeIdentity=WatchlistWorkspaceList::identityFromKey(pendingChartIdentity_.isEmpty()?selectedChartIdentity_:pendingChartIdentity_);
    const bool upbitScope=browserMarket_=="upbit"||(coinBrowserMarket(browserMarket_)&&routeIdentity.market=="coin"&&routeIdentity.source=="upbit");
    const bool upbitFailure=upbitScope&&publicMarkets_&&
        publicMarkets_->marketListState()==PublicMarketCatalog::MarketListState::Failed;
    const bool upbitLoading=upbitScope&&publicMarkets_&&
        publicMarkets_->marketListState()==PublicMarketCatalog::MarketListState::Loading;
    const bool emptyConfirmedUpbit=upbitScope&&publicMarkets_&&publicMarkets_->marketListAvailable()&&
        publicMarkets_->marketListState()==PublicMarketCatalog::MarketListState::Ready&&publicMarkets_->rows().isEmpty();
    // An Upbit coin missing from Upbit KRW opens on Binance Spot, so both coin tabs depend on the Binance catalogue.
    const auto binanceFailure=coinBrowserMarket(browserMarket_)&&publicInstruments_?publicInstruments_->failureReason("binance"):QString{};
    QString sourceStatus;
    if(upbitFailure){
        sourceStatus=tr("업비트 종목 목록 조회 실패 · %1").arg(publicMarkets_->marketListError());
        if(!publicMarkets_->rows().isEmpty())sourceStatus+="\n"+tr("이전 목록을 표시하고 있어요.");
    }else if(upbitLoading){
        sourceStatus=tr("업비트 종목 목록 조회 중");
        if(!publicMarkets_->rows().isEmpty())sourceStatus+="\n"+tr("이전 목록을 유지하며 갱신하고 있어요.");
    }else if(emptyConfirmedUpbit){
        sourceStatus=tr("업비트 원화 목록에 표시할 종목이 없습니다.");
    }else if(!coinBrowserMarket(browserMarket_)&&publicBoard_&&!publicBoard_->status(browserMarket_).isEmpty()){
        sourceStatus=tr("%1 공개 목록 갱신 실패 · %2").arg(browserMarket_=="kr"?tr("한국 주식"):tr("미국 주식"),publicBoard_->status(browserMarket_));
    }
    if(!binanceFailure.isEmpty()){
        if(!sourceStatus.isEmpty())sourceStatus+='\n';
        sourceStatus+=tr("바이낸스 종목 목록 조회 실패 · %1").arg(binanceFailure);
    }
    updateCatalogRetry(upbitFailure,upbitLoading,!binanceFailure.isEmpty());
    if(browserScope_=="holdings"&&privateEnabled_){listStatus_->setText(tr("%1개").arg(holdingRows_.size()));}
    else if(browserScope_=="holdings"&&!privateEnabled_)listStatus_->setText(tr("금고를 열면 보유 종목을 볼 수 있어요."));
    else {
        QString summary;
        if(shown)summary=tr("%1개").arg(shown)+(scope.isEmpty()?QString{}:QStringLiteral(" · ")+scope);
        else if(!sourceStatus.isEmpty())summary=scope;
        else if(query.isEmpty())summary=tr("현재 범위에 표시할 종목이 없습니다.\n%1").arg(scope);
        else {
            summary=tr("현재 검색 범위에서 찾지 못했어요.\n%1").arg(scope);
            if(browserScope_!="all")summary+="\n"+tr("전체 탭에서 공개 목록과 보유·관심 종목을 함께 검색할 수 있어요.");
            if(browserMarket_=="kr")summary+="\n"+tr("한국 전체 상장 종목 검색이 아니에요. KOSDAQ 종목은 기본 공개 목록에 포함되지 않아요.");
            else if(browserMarket_=="us")summary+="\n"+tr("미국 전체 상장 종목 검색이 아니에요. NYSE 종목은 기본 공개 목록에 포함되지 않아요.");
        }
        listStatus_->setText(sourceStatus.isEmpty()?summary:sourceStatus+"\n"+summary);
    }
    // A pending coin selection is independent of the stock browser tabs (both coin tabs show it). A
    // metadata error/loading message above also replaces its generic waiting text.
    const auto routeMarket=browserMarket(routeIdentity);
    if(!chartRouteMessage_.isEmpty()&&(routeMarket==browserMarket_||(coinBrowserMarket(routeMarket)&&coinBrowserMarket(browserMarket_)))&&
       !(routeIdentity.source=="upbit"&&(upbitFailure||upbitLoading||emptyConfirmedUpbit||!binanceFailure.isEmpty()))&&
       !(routeIdentity.source=="binance"&&!binanceFailure.isEmpty()))
        listStatus_->setText(chartRouteMessage_+"\n"+listStatus_->text());
    const bool holdingScope = browserScope_ == "holdings";
    const bool watchScope=browserScope_=="watch";
    listSelector_->setVisible(watchScope);listCreate_->setVisible(watchScope);listMenuButton_->setVisible(watchScope);
    browserViewModes_->setVisible(watchScope);
    holdingInformation_->setVisible(holdingScope&&privateEnabled_);
    browserCount_->setText(tr("%1개").arg(holdingScope?(privateEnabled_?holdingRows_.size():0):shown));
    if(watchScope&&browserMarkets_->parentWidget()!=browserWatchMarkets_) {
        browserHeaderLayout_->removeWidget(browserMarkets_);browserWatchMarketsLayout_->addWidget(browserMarkets_);browserWatchMarketsLayout_->addStretch();
    }else if(!watchScope&&browserMarkets_->parentWidget()==browserWatchMarkets_) {
        browserWatchMarketsLayout_->removeWidget(browserMarkets_);browserHeaderLayout_->insertWidget(1,browserMarkets_);
    }
    browserMarkets_->setVisible(!holdingScope);browserWatchMarkets_->setVisible(watchScope);
    listStatus_->setVisible(!listStatus_->text().trimmed().isEmpty()&&!QRegularExpression(QStringLiteral("^\\d+개$")).match(listStatus_->text()).hasMatch());
    const bool holdingSearchEmpty = privateEnabled_ && !query.isEmpty() && !holdingRows_.isEmpty() && !holdingPanel_->hasVisibleResults();
    bool emptySectionsVisible=false;
    if(watchScope&&query.isEmpty())for(int i=0;i<watchlistRows_->topLevelItemCount();++i){const auto* row=watchlistRows_->topLevelItem(i);if(!row->isHidden()&&row->childCount()==0&&row->data(0,IdentityRole).toString().isEmpty()&&!row->data(0,SectionRole).toString().isEmpty())emptySectionsVisible=true;}
    const bool showEmpty = !trendEnabled_ && (holdingScope ? holdingSearchEmpty :
        shown == 0 && !emptySectionsVisible && (!query.isEmpty() || sourceStatus.isEmpty() || emptyConfirmedUpbit));
    browserEmpty_->setVisible(showEmpty);
    watchlistRows_->setVisible(!trendEnabled_ && !holdingScope && !showEmpty);
    holdingPanel_->setVisible(!trendEnabled_ && holdingScope && !showEmpty);
    trendRows_->setVisible(trendEnabled_);
    if(!browserRestoring_)browserStates_[browserMarket_].query=query;
    scheduleVisibleRows();
}
void ChartWorkspace::updateCatalogRetry(bool upbitFailed,bool upbitLoading,bool binanceFailed) {
    if(!browserRetry_)return;
    browserRetry_->setVisible(upbitFailed||binanceFailed||(upbitLoading&&!browserRetry_->isHidden()));
    const auto upbitWait=upbitFailed?publicMarkets_->retryAfterMs():0;
    const auto binanceWait=binanceFailed?publicInstruments_->retryAfterMs("binance"):0;
    const auto wait=upbitWait&&binanceWait?qMin(upbitWait,binanceWait):qMax(upbitWait,binanceWait);
    QStringList ready;
    if(upbitFailed&&!publicMarkets_->busy()&&upbitWait==0)ready<<tr("업비트");
    if(binanceFailed&&publicInstruments_->canRetry("binance"))ready<<tr("바이낸스");
    browserRetry_->setEnabled(!ready.isEmpty());
    if(!ready.isEmpty())browserRetry_->setText(tr("%1 종목 목록 다시 조회").arg(ready.join(QStringLiteral("·"))));
    else if(wait>0)browserRetry_->setText(tr("%1초 후 다시 조회").arg((wait+999)/1000));
    else browserRetry_->setText(upbitLoading?tr("다시 조회 중"):tr("재조회 대기"));
    if(instrumentPanel_->isVisible()&&wait>0){if(!browserRetryTimer_->isActive())browserRetryTimer_->start();}
    else browserRetryTimer_->stop();
}
bool ChartWorkspace::eventFilter(QObject* object, QEvent* event) {
    if((object==toolbar_||object==instrumentTools_||object==frequentTools_||object==secondaryTools_)&&
       (event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest))layoutToolbar();
    if(object==instrumentButton_&&event->type()==QEvent::LayoutRequest)layoutToolbar();
    if(object==sideSplitter_&&event->type()==QEvent::Resize)layoutSidePanel();
    if(object==relatedMarkets_&&event->type()==QEvent::Resize){renderMarketContext();return false;}
    if (object == instrumentTitle_ && event->type() == QEvent::ToolTipChange)
        instrumentButton_->setToolTip(instrumentTitle_->toolTip());
    if (object == instrumentPanel_ && event->type() == QEvent::Show)
        refreshWatchlists();
    if (object == instrumentPanel_ && event->type() == QEvent::Hide) {
        browserRetryTimer_->stop();
        ++trendGeneration_;
        trendKey_.clear();
        refreshTrendDemands();
        scheduleVisibleRows();
    }
    if (event->type() == QEvent::KeyPress && instrumentPanel_->isVisible()) {
        if(auto* listTree=qobject_cast<InstrumentListTree*>(object);listTree&&listTree->isEditingSection())return false;
        const auto key = static_cast<QKeyEvent*>(event)->key();
        auto* tree = instrumentResultTree();
        if (key == Qt::Key_Escape && (object == instrumentPanel_ || object == input_ || object == tree)) {
            if (input_->text().isEmpty()) closeInstrumentPanel();
            else { input_->clear(); input_->setFocus(); }
            return true;
        }
        if (object == watchlistRows_ && key == Qt::Key_Space) {
            auto* item = watchlistRows_->currentItem();
            if (item && item->data(2, Qt::CheckStateRole).isValid())
                item->setCheckState(2, item->checkState(2) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            return true;
        }
        if (object == tree && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
            activateInstrumentResult();
            return true;
        }
        if (object == input_ && (key == Qt::Key_Up || key == Qt::Key_Down)) {
            const bool down = key == Qt::Key_Down;
            auto* current = tree->currentItem();
            QTreeWidgetItem* candidate = nullptr;
            if (isVisibleInstrumentRow(current))
                candidate = down ? tree->itemBelow(current) : tree->itemAbove(current);
            if (!isVisibleInstrumentRow(current)) {
                for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                    if (!isVisibleInstrumentRow(*it)) continue;
                    candidate = *it;
                    if (down) break;
                }
            } else {
                while (candidate && !isVisibleInstrumentRow(candidate))
                    candidate = down ? tree->itemBelow(candidate) : tree->itemAbove(candidate);
            }
            if (candidate) {
                tree->setCurrentItem(candidate);
                tree->scrollToItem(candidate);
            }
            return true;
        }
    }
    for (auto* tree : {watchlistRows_, holdingsRows_}) if (tree && object == tree->viewport()) {
        if (event->type() == QEvent::Resize) {
            if (tree == watchlistRows_) resizeBrowserColumns();
            else {
                const int priceWidth = qMin(144, qMax(120, tree->viewport()->width() / 2));
                tree->setColumnWidth(0, qMax(0, tree->viewport()->width() - priceWidth));
                tree->setColumnWidth(1, priceWidth);
            }
        }
        if (event->type()==QEvent::Paint || event->type()==QEvent::Resize || event->type()==QEvent::Show || event->type()==QEvent::Hide) scheduleVisibleRows();
    }
    return QWidget::eventFilter(object, event);
}
void ChartWorkspace::resizeBrowserColumns() {
    const bool expanded=browserExpand_->isChecked();
    auto* header=watchlistRows_->header();
    header->moveSection(header->visualIndex(6),2);
    // Keep the final visible column before hidden metric sections. Moving it
    // behind hidden sections can retain their old geometry until the next layout.
    header->moveSection(header->visualIndex(2),expanded?6:browserScope_=="all"?4:3);
    int metricsWidth=0;
    for(int column=3;column<6;++column) {
        const bool visible=expanded||(column==3&&browserScope_=="all");
        watchlistRows_->setColumnHidden(column,!visible);
        if(visible){watchlistRows_->setColumnWidth(column,64);metricsWidth+=64;}
    }
    const int width = watchlistRows_->viewport()->width();
    watchlistRows_->setColumnWidth(0,qMax(expanded?160:0,width-96-78-24-metricsWidth));
    watchlistRows_->setColumnWidth(1,96);
    watchlistRows_->setColumnWidth(6,78);
    watchlistRows_->setColumnWidth(2,24);
    updateBrowserHeaders();
}
void ChartWorkspace::scheduleVisibleRows() {
    if (visibleRowsPending_) return;
    visibleRowsPending_ = true;
    QTimer::singleShot(0, this, [this] { visibleRowsPending_ = false; publishVisibleRows(); });
}
QStringList ChartWorkspace::visiblePublicIdentityKeysForDisplay() const {
    QStringList visible;
    if (instrumentPanel_->isVisible() && watchlists_) for (auto* tree : {holdingsRows_, watchlistRows_}) {
        if (!tree || !tree->isVisible() || (tree==holdingsRows_ && !privateEnabled_)) continue;
        const auto region = tree->viewport()->visibleRegion();
        for (QTreeWidgetItemIterator items(tree); *items; ++items) {
            const auto key = (*items)->data(0, IdentityRole).toString();
            if (!WatchlistWorkspaceList::validIdentity(WatchlistWorkspaceList::identityFromKey(key))) continue;
            const auto bounds = tree->visualItemRect(*items);
            if (!bounds.isEmpty() && region.intersects(bounds) && !visible.contains(key)) visible.append(key);
        }
    }
    return visible; // Display membership is not permission to issue a request.
}
void ChartWorkspace::publishVisibleRows() {
    QStringList visible;
    for(const auto& key:visiblePublicIdentityKeysForDisplay()) {
        const auto identity=WatchlistWorkspaceList::identityFromKey(key);
        if(identity.market=="stock") {
            if(!watchlists_->isCurrentlyVerified(identity))continue;
            bool existing=privateEnabled_&&watchlists_->holdings().contains(identity);
            for(const auto& list:watchlists_->userLists())for(const auto& entry:list.entries)if(entry.identity==identity)existing=true;
            if(!existing)continue; // Public catalogue rows are display-only, never a new stock feed demand.
        }
        if(routingEnabled_&&identity.market=="coin") {
            const auto route=resolveChartMarket(identity,publicMarkets_,publicInstruments_);
            if(!route.available()||route.identity!=identity)continue;
        }
        visible.append(key);
    }
    if (visible == publishedVisibleRows_) return;
    publishedVisibleRows_ = visible;
    emit visibleIdentitiesChanged(visible);
}
void ChartWorkspace::addWatchlistIdentity() { openInstrumentPicker(); }
void ChartWorkspace::openInstrumentPicker(const QString& query){
    if(instrumentPicker_){instrumentPicker_->setQuery(query);instrumentPicker_->raise();return;}
    if(publicMarkets_&&!publicMarkets_->marketListAvailable()&&!publicMarkets_->busy())publicMarkets_->refresh();
    if(publicInstruments_)publicInstruments_->ensure("binance");
    if(publicBoard_&&!publicBoard_->marketBoardBusy()&&(publicBoard_->items("kr").isEmpty()||publicBoard_->items("us").isEmpty()))publicBoard_->refresh();
    auto* picker=new ChartInstrumentPicker(publicMarkets_,publicBoard_,publicInstruments_,this);
    instrumentPicker_=picker;picker->setAttribute(Qt::WA_DeleteOnClose);picker->setQuery(query);
    connect(picker,&ChartInstrumentPicker::openRequested,this,[this,picker](const QString& key){
        if(setIdentity(key)){emit identitySelected(selectedChartIdentity_);picker->close();}
        else picker->setFeedback(listStatus_->text());
    });
    connect(picker,&ChartInstrumentPicker::addRequested,this,[this](const QString& key){addPublicInstruments({key},false);});
    connect(picker,&ChartInstrumentPicker::addRepresentativesRequested,this,[this](const QStringList& keys){addPublicInstruments(keys,true);});
    picker->open();
}
void ChartWorkspace::addPublicInstruments(const QStringList& keys,bool representative){
    if(!watchlists_)return;
    refreshRoutingSources();
    QList<WatchlistIdentity> accepted;
    for(const auto& key:keys){const auto identity=WatchlistWorkspaceList::identityFromKey(key);bool valid=false;
        if(identity.market=="coin"){const auto route=resolveChartMarket(identity,publicMarkets_,publicInstruments_);valid=route.available()&&route.identity==identity;}
        else if(identity.market=="stock"&&publicBoard_){for(const auto& row:publicBoard_->items(identity.source=="KR"?"kr":"us"))if(row.symbol==identity.code)valid=true;}
        if(valid&&!accepted.contains(identity))accepted.append(identity);
    }
    if(accepted.isEmpty()){if(instrumentPicker_)instrumentPicker_->setFeedback(tr("공개 목록에서 종목을 확인한 뒤 추가할 수 있어요."));return;}
    QString listId=currentListId();
    if(representative||watchlists_->userList(listId).id.isEmpty())listId=watchlists_->createList(representative?tr("대표 종목"):tr("내 관심 종목"));
    if(listId.isEmpty())return;
    QMap<QString,QString> sections;
    if(representative)for(const auto& name:{tr("코인"),tr("한국"),tr("미국")})sections.insert(name,watchlists_->addSection(listId,name));
    int added=0;
    for(const auto& identity:accepted){const auto name=identity.market=="coin"?tr("코인"):identity.source=="KR"?tr("한국"):tr("미국");
        if(watchlists_->addIdentity(listId,identity,sections.value(name)))++added;
    }
    selectWatchlist(listId);
    if(instrumentPicker_)instrumentPicker_->setFeedback(added?tr("관심 목록에 추가했어요. 관리 메뉴에서 순서를 바꿀 수 있어요."):tr("이미 관심 목록에 있어요."));
}
void ChartWorkspace::positionInstrumentPanel() {
    if (!instrumentButton_) return;
    const QRect windowRect(window()->mapToGlobal(QPoint{}), window()->size());
    const auto* screen = window()->screen();
    const QRect available = screen ? windowRect.intersected(screen->availableGeometry()) : windowRect;
    const QRect bounds = available.width() >= 64 && available.height() >= 64 ? available : windowRect;
    const int width = qMin(browserExpand_->isChecked() ? 760 : 560, windowRect.width() - 32);
    const QPoint anchor = instrumentButton_->mapToGlobal(QPoint(0, instrumentButton_->height() + 4));
    const int x = qBound(bounds.left() + 16, anchor.x(), qMax(bounds.left() + 16, bounds.right() - width - 15));
    const int height = qBound(1, bounds.bottom() - anchor.y() - 15, 520);
    instrumentPanel_->setGeometry(x, anchor.y(), qMax(1, width), height);
}

void ChartWorkspace::closeInstrumentPanel() {
    if (!instrumentPanel_->isVisible()) return;
    instrumentPanel_->hide();
    if (!instrumentButton_->isVisible()) return;
    instrumentButton_->setFocus(Qt::PopupFocusReason);
    // A native popup can deliver its final focus transition after Hide.
    QTimer::singleShot(0, this, [this] {
        if (!instrumentButton_->isVisible() || instrumentPanel_->isVisible() ||
            QApplication::activePopupWidget() || QApplication::activeModalWidget()) return;
        auto* activeWindow = QApplication::activeWindow();
        if (activeWindow && activeWindow != window() && activeWindow != instrumentPanel_) return;
        if (activeWindow != window() && QApplication::applicationState() == Qt::ApplicationActive)
            window()->activateWindow();
        instrumentButton_->setFocus(Qt::PopupFocusReason);
    });
}

QTreeWidget* ChartWorkspace::instrumentResultTree() const {
    if (trendEnabled_) return trendRows_;
    if (browserScope_ == "holdings") return holdingPanel_->resultTree();
    return watchlistRows_;
}

void ChartWorkspace::activateInstrumentResult() {
    auto* tree = instrumentResultTree();
    if(tree==watchlistRows_&&!static_cast<InstrumentListTree*>(tree)->rowActivationAllowed())return;
    auto* item = tree->currentItem();
    if (!isVisibleInstrumentRow(item)) {
        item = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if (isVisibleInstrumentRow(*it)) { item = *it; break; }
    }
    if (!item) return;
    if (tree == holdingPanel_->resultTree()) {
        const QScopedValueRollback activation(activatingHoldingResult_, true);
        holdingPanel_->activateResult(item);
        return;
    }
    if (tree == trendRows_) {
        if (openTrendRow(item, tree->currentColumn())) closeInstrumentPanel();
    } else if (openBrowserRow(item)) {
        closeInstrumentPanel();
    }
}


} // namespace keefi
