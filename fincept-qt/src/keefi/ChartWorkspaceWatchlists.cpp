#include "keefi/ChartWorkspace.h"
#include "keefi/ChartWorkspacePresentation_p.h"
#include "keefi/PlainToolTip.h"
#include "keefi/ChartGridView.h"
#include "keefi/HoldingChartPanel.h"
#include "keefi/ChartInstrumentPicker.h"
#include "keefi/InstrumentListTree.h"
#include "keefi/InstrumentListSelector.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/KeefiFonts.h"

#include <QStandardItemModel>
#include <QPainter>
#include <QBoxLayout>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QComboBox>
#include <QAction>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMenu>
#include <QSaveFile>
#include <QSettings>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QBrush>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPaintEvent>
#include <QPair>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <QAbstractItemModel>

#include <algorithm>
#include <limits>
#include <cmath>
#include <optional>

namespace keefi {
using namespace chart_workspace_detail;
namespace {
class InstrumentListMenu final : public QMenu {
public:
    using QMenu::QMenu;
protected:
    void paintEvent(QPaintEvent* event) override {
        QMenu::paintEvent(event);
        for(auto* action:actions())if(action->objectName()=="chartListRemoveList"&&action->isVisible()) {
            const auto area=actionGeometry(action);if(area.isEmpty())continue;
            QPainter painter(this);painter.setClipRect(area);
            painter.fillRect(area,activeAction()==action?themeColors().surface:themeColors().input);
            painter.setPen(action->isEnabled()?themeColors().danger:themeColors().text3);
            drawLedgerText(painter,QRectF(area).adjusted(12,0,-12,0),action->text(),FontRole::Caption12);
        }
    }
};
QString askListText(QWidget* parent, const QString& title, const QString& label, const QString& value = {}) {
    QInputDialog dialog(parent);
    dialog.setObjectName("chartWatchlistTextDialog");
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(value);
    dialog.setInputMode(QInputDialog::TextInput);
    return dialog.exec() == QDialog::Accepted ? dialog.textValue().trimmed() : QString{};
}
}
WatchlistWorkspaceList* ChartWorkspace::watchlistModel() const { return watchlists_; }
QString ChartWorkspace::currentListId() const { return listSelector_ ? listSelector_->currentData().toString() : QString{}; }
QString ChartWorkspace::selectedIdentity() const {
    const auto* item = watchlistRows_ ? watchlistRows_->currentItem() : nullptr;
    return item ? item->data(0, IdentityRole).toString() : QString{};
}
void ChartWorkspace::setWatchlistModel(WatchlistWorkspaceList* model) {
    if (!model || model == watchlists_) return;
    if(browserContextMenu_)browserContextMenu_->close();
    static_cast<InstrumentListTree*>(watchlistRows_)->cancelInteractions();
    auto* old = watchlists_.data();
    if (old) disconnect(old, nullptr, this, nullptr);
    watchlists_ = model;
    connect(model, &WatchlistWorkspaceList::changed, this, &ChartWorkspace::refreshWatchlists);
    connect(model, &WatchlistWorkspaceList::changed, this, &ChartWorkspace::refreshDrawingAlertLists);
    connect(model, &QObject::destroyed, this, [this] { static_cast<InstrumentListTree*>(watchlistRows_)->cancelInteractions(); watchlists_ = nullptr; refreshWatchlists(); refreshDrawingAlertLists(); });
    if (old && old->parent() == this) old->deleteLater();
    refreshRoutingSources();
    refreshWatchlists();
    refreshDrawingAlertLists();
}
void ChartWorkspace::setChartSettings(QSettings* settings) {
    if(chartSettings_==settings)return;
    chartSettings_=settings;
    sectionExpansion_.clear();
    renderedListId_.clear();
    refreshWatchlistRows();
}
void ChartWorkspace::setRealtimeDelayedIdentities(const QStringList& identities) {
    const QSet<QString> next(identities.cbegin(), identities.cend());
    if (next == delayedIdentities_) return;
    delayedIdentities_ = next;
    refreshWatchlistRows();
    refreshHoldingsPreview();
}
void ChartWorkspace::setHoldingsUnavailableReason(const QString& reason) {
    if (!privateEnabled_) return;
    if (reason == holdingsUnavailableReason_) return;
    holdingsUnavailableReason_ = reason;
    holdingPanel_->setCoverageReason(reason);
    refreshWatchlistRows();
}
void ChartWorkspace::selectWatchlist(const QString& id) {
    const auto index = listSelector_->findData(id);
    if (index >= 0) listSelector_->setCurrentIndex(index);
}
void ChartWorkspace::refreshWatchlists() {
    if (!listSelector_ || renderingLists_ || (mutatingLists_ && !clearingPrivate_) || (!isVisible() && !clearingPrivate_)) return;
    const auto previous = currentListId();
    renderingLists_ = true;
    const QSignalBlocker block(listSelector_);
    // Recent use and manual ordering are independent projections of the same
    // public lists. Quote-only updates must not reset an open selector or drag.
    auto ordered=watchlists_?watchlists_->userLists():QList<WatchlistUserList>{};
    if(watchlists_&&!static_cast<InstrumentListSelector*>(listSelector_)->manualOrder()) {
        QList<WatchlistUserList> recent;
        for(const auto& id:watchlists_->recentLists())for(int i=0;i<ordered.size();++i)if(ordered[i].id==id){recent.append(ordered.takeAt(i));break;}
        recent.append(ordered);ordered=recent;
    }
    if(ordered.isEmpty()){WatchlistUserList empty;empty.name=tr("리스트 없음");ordered.append(empty);}
    for(int i=0;i<ordered.size();++i) {
        const auto& list=ordered[i];
        int old=listSelector_->findData(list.id);
        if(old<0)listSelector_->insertItem(i,listFlagIcon(list.flag),list.name,list.id);
        else if(old!=i) {
            if(auto* model=qobject_cast<QStandardItemModel*>(listSelector_->model())){auto row=model->takeRow(old);model->insertRow(i,row);}
            else {listSelector_->removeItem(old);listSelector_->insertItem(i,listFlagIcon(list.flag),list.name,list.id);}
        }
        if(listSelector_->itemText(i)!=list.name)listSelector_->setItemText(i,list.name);
        if(listSelector_->itemData(i,Qt::UserRole+1).toString()!=list.flag){listSelector_->setItemIcon(i,listFlagIcon(list.flag));listSelector_->setItemData(i,list.flag,Qt::UserRole+1);}
    }
    while(listSelector_->count()>ordered.size())listSelector_->removeItem(listSelector_->count()-1);
    auto selected = listSelector_->findData(previous);
    if (selected < 0 && watchlists_) for (const auto& recent : watchlists_->recentLists()) {
        selected = listSelector_->findData(recent);
        if (selected >= 0) break;
    }
    if (selected < 0 && watchlists_ && !watchlists_->userLists().isEmpty()) selected = listSelector_->findData(watchlists_->userLists().first().id);
    listSelector_->setCurrentIndex(qMax(0, selected));
    listSelector_->setToolTip(plainToolTip(currentListId().isEmpty()?tr("오른쪽 메뉴에서 새 관심 리스트를 만드세요."):listSelector_->currentText()));
    renderingLists_ = false;
    const bool editable = watchlists_ && !watchlists_->userList(currentListId()).id.isEmpty();
    if (listMenu_) for (auto* value : listMenu_->findChildren<QAction*>()) if (value->property("requiresUserList").toBool()) value->setEnabled(editable);
    if(holdingPanel_)holdingPanel_->setWatchlistContext(watchlists_,currentListId());
    refreshWatchlistRows();
    refreshHoldingsPreview();
}
void ChartWorkspace::refreshWatchlistRows() {
    updateWatchlistRows(false);
}
void ChartWorkspace::updateWatchlistRows(bool neededForSplit) {
    if (!watchlistRows_ || (mutatingLists_ && !clearingPrivate_) || (!isVisible() && !clearingPrivate_)) return;
    if (!neededForSplit && watchlists_ && instrumentPanel_->isHidden() && !clearingPrivate_) {
        // The current-price headline reads the same model and stays live with the popup closed.
        refreshMarketLedger();
        return;
    }
    if(browserContextMenu_&&(browserScope_!="watch"||currentListId()!=renderedListId_))browserContextMenu_->close();
    watchlistRows_->setProperty("chartBrowserScope",browserScope_);
    const auto id = currentListId();
    const bool sameList = id == renderedListId_;
    const bool preserveViewport = sameList;
    const auto scroll = watchlistRows_->verticalScrollBar()->value();
    const auto horizontalScroll = watchlistRows_->horizontalScrollBar()->value();
    QHash<QString, bool> expanded;
    if (preserveViewport) for (int i = 0; i < watchlistRows_->topLevelItemCount(); ++i) {
        const auto* item = watchlistRows_->topLevelItem(i);
        const auto section = item->data(0, SectionRole).toString();
        if (!section.isEmpty() && item->data(0, IdentityRole).toString().isEmpty()) expanded.insert(section, item->isExpanded());
    }
    const auto selected = selectedIdentity();
    const auto sectionSelection = watchlistRows_->currentItem() ? watchlistRows_->currentItem()->data(0, SectionRole).toString() : QString{};
    const QSignalBlocker block(watchlistRows_);
    if (!watchlists_) {
        static_cast<InstrumentListTree*>(watchlistRows_)->setInteractionContext({},false,false);
        if (watchlistRows_->topLevelItemCount() > 0) watchlistRows_->clear();
        renderedListId_.clear(); scheduleVisibleRows(); return;
    }
    const auto list = watchlists_->userList(id);
    static_cast<InstrumentListTree*>(watchlistRows_)->setInteractionContext(id,browserScope_=="watch"&&!list.id.isEmpty(),browserSortMode()=="manual");
    QList<WatchlistEntry> entries = browserScope_=="holdings" ? QList<WatchlistEntry>{} : list.entries;
    QSet<QString> keys;for(const auto& entry:entries)keys.insert(entry.identity.key());
    const auto append=[&](const WatchlistIdentity& identity){if(WatchlistWorkspaceList::validIdentity(identity)&&!keys.contains(identity.key())){keys.insert(identity.key());entries.append({identity,{}});}};
    if(browserScope_!="watch"&&privateEnabled_)for(const auto& identity:watchlists_->holdings())append(identity);
    if(browserScope_=="all") {
        for(const auto& other:watchlists_->userLists())for(const auto& entry:other.entries)append(entry.identity);
        for(const auto& candidate:chartInstrumentCandidates(publicMarkets_,publicBoard_,publicInstruments_))append(WatchlistWorkspaceList::identityFromKey(candidate.key));
        // Retain the last public list for display after metadata expires/fails.
        // Routing still requires fresh verifiedMarkets(); old rows never become
        // verified identities or authorize new candle/study requests.
        if(publicMarkets_&&!publicMarkets_->marketListAvailable())
            for(const auto& row:publicMarkets_->rows())append({"coin","upbit",row.market});
    }
    QList<WatchlistIdentity> identities;identities.reserve(entries.size());for(const auto& entry:entries)identities.append(entry.identity);
    const auto labels=instrumentPresentation_->labels(identities);
    QSet<QString> members;for(const auto& entry:list.entries)members.insert(entry.identity.key());
    const auto sortMode=browserSortMode();
    if(sortMode=="name"||sortMode=="name-desc")std::stable_sort(entries.begin(),entries.end(),[&labels,&sortMode](const auto& a,const auto& b){
        const auto first=labels.value(a.identity.key()),second=labels.value(b.identity.key());
        const auto an=first.name.isEmpty()?first.code:first.name,bn=second.name.isEmpty()?second.code:second.name;
        return an==bn?a.identity.key()<b.identity.key():(sortMode=="name"?QString::localeAwareCompare(an,bn)<0:QString::localeAwareCompare(an,bn)>0);
    });
    else if(sortMode=="gainers"||sortMode=="losers"||sortMode.startsWith("cap")||sortMode.startsWith("price")||sortMode.startsWith("volume")){
        auto metrics=browserMetrics();
        if(sortMode.startsWith("price"))for(const auto& entry:entries){const auto state=watchlists_->rowState(entry.identity);if(state.privateData&&!privateEnabled_)continue;bool ok=false;const auto value=quotePriceParts(state.price).first.remove(',').toDouble(&ok);if(ok&&value>0&&std::isfinite(value))metrics[entry.identity.key()].price=value;}
        std::stable_sort(entries.begin(),entries.end(),[&](const auto& a,const auto& b){
            const auto x=metrics.value(a.identity.key()),y=metrics.value(b.identity.key());
            const auto group=[&](const WatchlistIdentity& identity,const BrowserMetric& metric){return sortMode.startsWith("cap")?(identity.market=="coin"?QStringLiteral("KRW"):metric.unit):identity.source;};
            const auto ag=group(a.identity,x),bg=group(b.identity,y);if(ag!=bg)return ag<bg;
            const auto value=[&](const BrowserMetric& metric)->std::optional<double>{if(sortMode.startsWith("price"))return metric.price;if(sortMode.startsWith("cap"))return metric.cap>0?std::optional<double>(metric.cap):std::nullopt;if(sortMode.startsWith("volume"))return metric.volume>0?std::optional<double>(metric.volume):std::nullopt;return metric.change;};
            const auto av=value(x),bv=value(y);
            if(av.has_value()!=bv.has_value())return av.has_value();
            if(av&&bv&&*av!=*bv)return sortMode=="losers"||sortMode.endsWith("-asc")?*av<*bv:*av>*bv;
            return a.identity.key()<b.identity.key();
        });
    }
    else if(sortMode=="value"||sortMode=="value-asc"){
        // Rank once per data arrival or mode change, not on every quote, so rows do not jump while read.
        // Rows rank by their own exchange's 24h value; each tab shows one exchange, so values compare in one currency.
        const auto values=browserTradeValues();const auto metrics=browserMetrics();const auto rankKey=browserScope_+'/'+sortMode;const auto now=QDateTime::currentMSecsSinceEpoch();
        const bool unranked=std::any_of(entries.cbegin(),entries.cend(),[&](const auto& entry){return values.value(entry.identity.key())>0&&!browserRank_.contains(entry.identity.key());});
        const bool stale=watchlistRows_->verticalScrollBar()->value()==0&&now-browserRankedAtMs_>300000;
        if(browserRankKey_!=rankKey||unranked||stale){
            struct Ranked{double value;QString key;};QList<Ranked> ordered;
            for(const auto& entry:entries){const auto value=values.value(entry.identity.key());if(value>0)ordered.append({value,entry.identity.key()});}
            std::stable_sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.value>b.value;});
            browserRank_.clear();for(int i=0;i<ordered.size();++i)browserRank_.insert(ordered[i].key,i);browserRankKey_=rankKey;browserRankedAtMs_=now;
        }
        std::stable_sort(entries.begin(),entries.end(),[this,&metrics,&sortMode](const auto& a,const auto& b){
            const auto ac=metrics.value(a.identity.key()).unit,bc=metrics.value(b.identity.key()).unit;if(ac!=bc)return ac<bc;
            const bool ar=browserRank_.contains(a.identity.key()),br=browserRank_.contains(b.identity.key());if(ar!=br)return ar;
            const auto x=browserRank_.value(a.identity.key(),std::numeric_limits<int>::max()),y=browserRank_.value(b.identity.key(),std::numeric_limits<int>::max());
            return sortMode=="value-asc"?x>y:x<y;});
    }
    const auto wantedSections = browserScope_=="holdings" ? QList<WatchlistSection>{} : list.sections;
    QHash<QString, QStringList> sectionKeys;
    for (const auto& section : wantedSections) sectionKeys.insert(section.id, {});
    QStringList rootKeys;
    for (const auto& entry : entries) {
        if (sectionKeys.contains(entry.sectionId)) sectionKeys[entry.sectionId].append(entry.identity.key());
        else rootKeys.append(entry.identity.key());
    }
    QHash<QString, QTreeWidgetItem*> existingRows;
    const auto matchesRow = [&](QTreeWidgetItem* item, const QString& key, const QString& section) {
        if (!item || item->childCount() || item->data(0, IdentityRole).toString() != key ||
            item->data(0, SectionRole).toString() != section) return false;
        existingRows.insert(key, item); return true;
    };
    // Quotes, badges and public names are data changes. Only membership,
    // parent sections or ordering require replacing the tree's model indexes.
    // An empty list still has native column elements; avoid resetting them on quotes.
    const bool reuseRows = sameList && [&] {
        if (watchlistRows_->topLevelItemCount() != wantedSections.size() + rootKeys.size()) return false;
        for (int i = 0; i < wantedSections.size(); ++i) {
            const auto& section = wantedSections[i];
            const auto* item = watchlistRows_->topLevelItem(i);
            const auto keys = sectionKeys.value(section.id);
            if (item->data(0, SectionRole).toString() != section.id || !item->data(0, IdentityRole).toString().isEmpty() ||
                item->childCount() != keys.size()) return false;
            for (int row = 0; row < keys.size(); ++row)
                if (!matchesRow(item->child(row), keys[row], section.id)) return false;
        }
        for (int i = 0; i < rootKeys.size(); ++i)
            if (!matchesRow(watchlistRows_->topLevelItem(wantedSections.size() + i), rootKeys[i], {})) return false;
        return true;
    }();
    if (!reuseRows) { if(browserContextMenu_)browserContextMenu_->close();static_cast<InstrumentListTree*>(watchlistRows_)->cancelInteractions();watchlistRows_->clear(); existingRows.clear(); }
    const auto setData = [](QTreeWidgetItem* item, int column, int role, const QVariant& value) {
        const auto displayed = role == Qt::ToolTipRole ? QVariant(plainToolTip(value.toString())) : value;
        if (item->data(column, role) != displayed) item->setData(column, role, displayed);
    };
    QHash<QString, QTreeWidgetItem*> sections;
    for (int i = 0; i < wantedSections.size(); ++i) {
        const auto& section = wantedSections[i];
        auto* item = reuseRows ? watchlistRows_->topLevelItem(i) : new QTreeWidgetItem(watchlistRows_);
        setData(item, 0, Qt::DisplayRole, section.name);
        setData(item, 0, SectionRole, section.id);
        setData(item, 0, Qt::SizeHintRole, QSize(0,26));
        if (!reuseRows) {
            item->setFirstColumnSpanned(true);
            const bool stored=sectionExpansion_.value(id).value(section.id,chartSettings_?chartSettings_->value("chart/watchlistSections/"+id+"/"+section.id+"/expanded",true).toBool():true);
            item->setExpanded(expanded.value(section.id,stored));
        }
        sections.insert(section.id, item);
        if (selected.isEmpty() && sectionSelection == section.id && watchlistRows_->currentItem() != item)
            watchlistRows_->setCurrentItem(item);
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto browserMetricRows=browserMetrics();
    // Upbit's public KRW tickers are already fetched for the catalogue; index them once per refresh.
    QHash<QString,PublicMarketRow> upbitQuotes;if(publicMarkets_)for(const auto& row:publicMarkets_->rows())if(row.priced)upbitQuotes.insert(row.market,row);
    for (const auto& entry : entries) {
        const auto& identity = entry.identity;
        const auto key = identity.key();
        auto state = watchlists_->rowState(identity);
        if (state.privateData && !privateEnabled_) state = {};
        QString publicQuoteInfo;
        if(identity.market=="coin"&&identity.source=="upbit"&&state.price.isEmpty()&&upbitQuotes.contains(identity.code)){
            // Until the row's own quote arrives, show the official public ticker the catalogue holds.
            const auto& row=upbitQuotes[identity.code];
            if(row.receivedAtMs>0&&row.receivedAtMs<=now&&positiveDisplayNumber(row.price)){
                state.price=row.price;state.change=changePercentText(row.signedChangeRate);
                state.quoteState=WatchlistQuoteState::Delayed;
                publicQuoteInfo=tr("업비트 공개 현재가 · 전일 대비 · 수신 %1").arg(QDateTime::fromMSecsSinceEpoch(row.receivedAtMs).toString("HH:mm:ss"));
            }
        }
        const bool binanceRow=identity.market=="coin"&&identity.source=="binance";
        if((identity.market=="stock"||binanceRow)&&publicBoard_&&state.price.isEmpty()) {
            // Before the first stream tick a Binance row shows its last 24h ticker, labelled as such.
            auto quote=publicBoard_->knownItem(binanceRow?QStringLiteral("binance"):identity.source.toLower(),identity.code);
            if(binanceRow&&!quote.changePercent.isEmpty()){bool ok=false;const auto rate=quote.changePercent.toDouble(&ok);
                // Round first so a tiny move reads 0.00, never -0.00.
                const auto rounded=ok&&std::isfinite(rate)?std::round(rate*100)/100:0.0;
                quote.changePercent=ok&&std::isfinite(rate)?(rounded>0?"+":"")+QString::number(rounded==0?0.0:rounded,'f',2):QString{};}
            if(quote.receivedAtMs>0&&quote.receivedAtMs<=now&&positiveDisplayNumber(quote.price)) {
                state.price=quote.price+(quote.currency.isEmpty()?QString{}:" "+quote.currency);
                state.change=quote.changePercent.isEmpty()?QString{}:quote.changePercent+"%";
                state.quoteState=WatchlistQuoteState::Delayed;
                publicQuoteInfo=tr("공개 참고 시세 · 원천 시각 미제공 · 수신 %1").arg(QDateTime::fromMSecsSinceEpoch(quote.receivedAtMs).toString("HH:mm:ss"));
            }
        }
        const bool verified = identity.market != "stock" || watchlists_->isCurrentlyVerified(identity);
        auto presentations = rowBadges(state,verified,delayedIdentities_.contains(key),now);
        const auto instrument=labels.value(key);
        auto rowInstrument=instrument;rowInstrument.market.clear();
        // Risk badges always stay visible; of the rest only the first one is shown so a
        // 260px row never ellipsizes. The full list is in the tooltip.
        const auto allPresentations = presentations;
        QList<WatchlistBadge> shownBadges; bool ordinaryShown = false;
        for (const auto& badge : presentations) {
            if (badge.fixedRisk) shownBadges.append(badge);
            else if (!ordinaryShown) { shownBadges.append(badge); ordinaryShown = true; }
        }
        presentations = shownBadges;
        QStringList shownTexts; for (const auto& badge : shownBadges) shownTexts.append(movementDecoratedText(badge.text, badge.direction));
        const auto label = rowInstrument.text() + (shownTexts.isEmpty() ? QString{} : "\n" + shownTexts.join(" · "));
        auto* item = reuseRows ? existingRows.value(key) : sections.contains(entry.sectionId) ?
            new QTreeWidgetItem(sections.value(entry.sectionId)) : new QTreeWidgetItem(watchlistRows_);
        setData(item, 0, Qt::DisplayRole, label);
        const auto quoteDisplay=rowQuoteDisplay(state);
        setData(item, 1, Qt::DisplayRole, quoteDisplay.text);
        setData(item, 0, BrowserQuoteStatusRole, quoteDisplay.status);
        setData(item, 6, Qt::DisplayRole, state.change.isEmpty()?QStringLiteral("—"):state.change);
        setData(item, 1, Qt::ToolTipRole, quoteDisplay.text+(publicQuoteInfo.isEmpty()?QString{}:"\n"+publicQuoteInfo));
        const auto metric=browserMetricRows.value(key);
        const auto number=[](double value){
            if(!(value>0)||!std::isfinite(value))return QStringLiteral("—");
            for(const auto& unit:QList<QPair<double,QString>>{{1e12,QStringLiteral("조")},{1e8,QStringLiteral("억")},{1e4,QStringLiteral("만")}})
                if(value>=unit.first)return QString::number(value/unit.first,'f',value/unit.first<10?2:1)+unit.second;
            return groupedAmount(QString::number(value,'f',value<1?6:0));};
        setData(item,3,Qt::DisplayRole,number(metric.turnover)+(metric.turnover>0?" "+metric.unit:QString{}));
        setData(item,4,Qt::DisplayRole,number(metric.volume)+(metric.volume>0?" "+metric.base:QString{}));
        setData(item,5,Qt::DisplayRole,number(metric.cap)+(metric.cap>0?" "+(identity.market=="coin"?QStringLiteral("KRW"):metric.unit):QString{}));
        const auto period=identity.market=="coin"?tr("최근 24시간"):tr("거래일 기준");
        setData(item,3,Qt::ToolTipRole,period+tr(" 거래대금 · ")+metric.unit+(identity.source=="US"?tr(" · 가격×거래량 근사"):QString{}));
        setData(item,4,Qt::ToolTipRole,period+tr(" 실제 거래량 · 값이 없으면 추정하지 않아요"));
        setData(item,5,Qt::ToolTipRole,metric.capBasis.isEmpty()?tr("시총 원천 연결 미확인"):metric.capBasis);
        for(int column=3;column<6;++column)setData(item,column,Qt::TextAlignmentRole,int(Qt::AlignRight|Qt::AlignVCenter));
        setData(item, 0, IdentityRole, key);
        setData(item, 0, ChartInstrumentLabelRole, QStringList{instrument.name, instrument.code, instrument.market});
        const bool member=members.contains(key);
        setData(item,0,BrowserMemberRole,member);
        setData(item,0,BrowserSearchRole,QStringList{key,instrument.name,instrument.code,instrument.market,chartInstrumentSearchAlias(identity.code),chartInstrumentSearchAlias(identity.code.section('-',1)),chartInstrumentSearchAlias(identity.code.endsWith("USDT")?identity.code.chopped(4):identity.code)}.join(' '));
        setData(item,2,Qt::CheckStateRole,list.id.isEmpty()?QVariant{}:QVariant(member?Qt::Checked:Qt::Unchecked));
        setData(item,2,Qt::AccessibleTextRole,list.id.isEmpty()?tr("관심 리스트를 먼저 선택하세요"):tr("%1에 %2").arg(list.name,member?tr("저장됨 · Space로 해제"):tr("Space로 추가")));
        setData(item,2,Qt::ToolTipRole,list.id.isEmpty()?tr("메뉴에서 관심 리스트를 만든 뒤 별표를 사용하세요."):tr("%1에 %2").arg(list.name,member?tr("저장됨"):tr("추가")));
        const auto flags=item->flags()|Qt::ItemIsUserCheckable;
        if(item->flags()!=flags)item->setFlags(flags);
        const auto unavailable = grid_->isVisible() && identity.market == "stock" ? splitStockChartMessage() : QString{};
        const auto routeDetail = unavailable.isEmpty() ? QString{} : "\n" + unavailable;
        setData(item, 0, BrowserUnavailableReasonRole, unavailable);
        setData(item, 0, Qt::AccessibleTextRole, label + routeDetail);
        static const QRegularExpression signedPercent("\\A[+-][0-9]+(?:\\.[0-9]+)?%\\z");
        int changeDirection = 0;
        if (signedPercent.match(state.change).hasMatch()) {
            bool valid = false; const double value = state.change.chopped(1).toDouble(&valid);
            if (valid && std::isfinite(value)) changeDirection = value > 0 ? 1 : value < 0 ? -1 : 0;
        }
        setData(item, 1, ChangeDirectionRole, changeDirection);
        setData(item, 0, WatchlistBadgesRole, QVariant::fromValue(presentations));
        setData(item, 1, WatchlistBadgesRole, QVariant::fromValue(presentations)); // Repaint both halves on badge-only changes.
        setData(item, 0, SectionRole, entry.sectionId);
        QStringList badgeDetails;
        for (const auto& badge : allPresentations) if (!badge.text.isEmpty()) badgeDetails.append(badge.text);
        setData(item, 0, Qt::ToolTipRole, instrument.text() + routeDetail + (badgeDetails.isEmpty() ? QString{} : "\n" + badgeDetails.join(" · ")) +
            (publicQuoteInfo.isEmpty()?QString{}:"\n"+publicQuoteInfo));
        // Quotes and warning badges share a fixed line and cannot move neighbouring rows.
        setData(item, 0, Qt::SizeHintRole, QSize(0, 32));
        setData(item, 0, Qt::ForegroundRole, verified ? QVariant{} : QVariant::fromValue(QBrush(themeColors().text3)));
        if (key == selected && watchlistRows_->currentItem() != item) watchlistRows_->setCurrentItem(item);
    }
    QStringList markets;for(const auto& entry:entries){const auto market=labels.value(entry.identity.key()).market;if(!market.isEmpty()&&!markets.contains(market))markets.append(market);}
    listMarkets_->setText(markets.join(" · "));listMarkets_->hide();
    resizeBrowserColumns();
    if (!reuseRows) {
        watchlistRows_->doItemsLayout();
        if (preserveViewport) {
            watchlistRows_->verticalScrollBar()->setValue(scroll);
            watchlistRows_->horizontalScrollBar()->setValue(horizontalScroll);
        }
    }
    renderedListId_ = id;
    if(id==HoldingsList)listStatus_->setText(usabilityText(UsabilityText::HoldingsSummary).arg(entries.size())+
        (chartRouteMessage_.isEmpty()?QString{}:" · "+chartRouteMessage_));
    else if(!chartRouteMessage_.isEmpty())listStatus_->setText(chartRouteMessage_);
    else listStatus_->setText(entries.isEmpty()?tr("종목 찾기에서 관심 목록을 채워 보세요."):QString{});
    filterBrowserRows();
    scheduleVisibleRows();
    refreshMarketLedger();refreshTrendDemands();
}
void ChartWorkspace::buildWatchlistMenu() {
    listMenu_ = new InstrumentListMenu(listMenuButton_);
    listMenu_->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(listMenu_,QStringLiteral("QMenu{background:${input};color:${text};border:1px solid ${field};border-radius:6px;padding:4px;} QMenu::item{min-height:20px;padding:4px 12px;margin:0 2px;border-radius:4px;} QMenu::item:selected{background:${surface};} QMenu::item:disabled{color:${text3};} QMenu::separator{height:1px;background:${rule};margin:4px 8px;}"));
    listMenuButton_->setMenu(listMenu_);
    auto* trend=new QAction(tr("관심 추세표"),listMenu_);trend->setObjectName("chartListShowTrend");trend->setCheckable(true);
    connect(trend,&QAction::toggled,trendToggle_,&QPushButton::setChecked);
    auto* sort=new QMenu(tr("목록 정렬"),listMenu_);
    for(const auto& option:QList<QPair<QString,QString>>{{"auto",tr("기본 · 전체·보유는 거래대금순, 관심은 저장한 순서")},{"gainers",tr("상승률순")},{"losers",tr("하락률순")},{"cap",tr("시가총액순")},{"value",tr("24시간 거래대금순")},{"manual",tr("저장한 순서")},{"name",tr("이름순")}}) {
        auto* value=sort->addAction(option.second);value->setObjectName("chartBrowserSort_"+option.first);
        connect(value,&QAction::triggered,this,[this,key=option.first]{browserSort_=key;browserStates_[browserMarket_].sort=key;if(browserSortSelector_){const QSignalBlocker block(browserSortSelector_);browserSortSelector_->setCurrentIndex(browserSortSelector_->findData(key));}refreshWatchlistRows();});
    }
    const auto action = [this](const QString& text, const QString& name, bool userList, auto callback) {
        auto* value = listMenu_->addAction(text);
        value->setObjectName(name);
        value->setProperty("requiresUserList", userList);
        connect(value, &QAction::triggered, this, callback);
        return value;
    };
    auto* create=action(tr("새 리스트"), "chartListCreate", false, [this] {
        if (!watchlists_) return;
        const auto name = askListText(this, tr("새 관심 리스트"), tr("리스트 이름"));
        if (!name.isEmpty()) selectWatchlist(watchlists_->createList(name));
    });
    listMenu_->removeAction(create);
    action(tr("이름 바꾸기"), "chartListRename", true, [this] {
        if (!watchlists_) return;
        const auto id = currentListId();
        const auto name = askListText(this, tr("리스트 이름 바꾸기"), tr("리스트 이름"), watchlists_->userList(id).name);
        if (!name.isEmpty()) watchlists_->renameList(id, name);
    });
    auto* flags = listMenu_->addMenu(tr("색 깃발"));
    flags->menuAction()->setProperty("requiresUserList", true);
    for (const auto& pair : {std::pair{"", "없음"}, {"red", "빨강"}, {"orange", "주황"}, {"yellow", "노랑"}, {"green", "초록"}, {"blue", "파랑"}, {"purple", "보라"}}) {
        auto* flag = flags->addAction(QString::fromUtf8(pair.second));
        flag->setObjectName("chartListFlag_" + QString::fromLatin1(pair.first));
        connect(flag, &QAction::triggered, this, [this, value = QString::fromLatin1(pair.first)] { if (watchlists_) watchlists_->setFlag(currentListId(), value); });
    }
    action(tr("섹션 추가"), "chartListAddSection", true, [this] {
        if (!watchlists_) return;
        const auto name = askListText(this, tr("새 섹션"), tr("섹션 이름"));
        if (!name.isEmpty()) watchlists_->addSection(currentListId(), name);
    });
    action(tr("선택 섹션 이름 바꾸기"), "chartListRenameSection", true, [this] {
        if (!watchlists_ || !watchlistRows_->currentItem()) return;
        const auto section = watchlistRows_->currentItem()->data(0, SectionRole).toString();
        if (section.isEmpty()) return;
        const auto name = askListText(this, tr("섹션 이름 바꾸기"), tr("섹션 이름"));
        if (!name.isEmpty()) watchlists_->renameSection(currentListId(), section, name);
    });
    action(tr("대표 종목으로 채우기…"), "chartListAddIdentity", false, [this] { addWatchlistIdentity(); });
    action(tr("선택 종목 섹션 이동"), "chartListMoveSection", true, [this] {
        if (!watchlists_ || selectedIdentity().isEmpty()) return;
        const auto list = watchlists_->userList(currentListId());
        const auto identity = WatchlistWorkspaceList::identityFromKey(selectedIdentity());
        QDialog dialog(this); dialog.setObjectName("chartWatchlistMoveSectionDialog"); dialog.setWindowTitle(tr("종목 섹션 이동"));
        auto* layout = new QVBoxLayout(&dialog);
        QComboBox target(&dialog); target.setObjectName("chartWatchlistTargetSection"); target.addItem(tr("섹션 없음"), QString{});
        for (const auto& section : list.sections) target.addItem(section.name, section.id);
        layout->addWidget(&target);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        for (qsizetype i = 0; i < list.entries.size(); ++i) if (list.entries[i].identity == identity) {
            watchlists_->moveIdentity(list.id, identity, int(i), target.currentData().toString()); break;
        }
    });
    action(tr("선택 종목·섹션 삭제"), "chartListRemoveIdentity", true, [this] {
        if (!watchlists_ || !watchlistRows_->currentItem()) return;
        const auto key = selectedIdentity();
        if (!key.isEmpty()) watchlists_->removeIdentity(currentListId(), WatchlistWorkspaceList::identityFromKey(key));
        else watchlists_->removeSection(currentListId(), watchlistRows_->currentItem()->data(0, SectionRole).toString());
    });
    for (const auto direction : {-1, 1}) action(direction < 0 ? tr("선택 종목·섹션 위로") : tr("선택 종목·섹션 아래로"),
        direction < 0 ? "chartListMoveUp" : "chartListMoveDown", true, [this, direction] {
            if (!watchlists_ || !watchlistRows_->currentItem()) return;
            const auto list = watchlists_->userList(currentListId());
            const auto key = selectedIdentity();
            const auto section = watchlistRows_->currentItem()->data(0, SectionRole).toString();
            if (!key.isEmpty()) {
                QList<int> positions;
                int selected = -1;
                for (qsizetype i = 0; i < list.entries.size(); ++i) if (list.entries[i].sectionId == section) {
                    if (list.entries[i].identity.key() == key) selected = positions.size();
                    positions.append(int(i));
                }
                if (selected >= 0) {
                    const auto target = positions[qBound(0, selected + direction, int(positions.size()) - 1)];
                    watchlists_->moveIdentity(list.id, WatchlistWorkspaceList::identityFromKey(key), target, section);
                }
            }
            if (key.isEmpty()) for (qsizetype i = 0; i < list.sections.size(); ++i) if (list.sections[i].id == section) {
                watchlists_->moveSection(list.id, section, qBound(0, int(i) + direction, int(list.sections.size()) - 1));
                break;
            }
        });
    for (const auto direction : {-1, 1}) action(direction < 0 ? tr("리스트 위로") : tr("리스트 아래로"),
        direction < 0 ? "chartListMoveListUp" : "chartListMoveListDown", true, [this, direction] {
            if (!watchlists_) return;
            const auto lists = watchlists_->userLists();
            for (qsizetype i = 0; i < lists.size(); ++i) if (lists[i].id == currentListId()) {
                watchlists_->moveList(lists[i].id, qBound(0, int(i) + direction, int(lists.size()) - 1));
                break;
            }
        });
    auto* removeList=action(tr("리스트 삭제"), "chartListRemoveList", true, [this] { if (watchlists_) watchlists_->removeList(currentListId()); });
    listMenu_->removeAction(removeList);
    listMenu_->addSeparator();
    action(tr("CSV 가져오기"), "chartListImportCsv", false, [this] {
        if (!watchlists_) return;
        QFileDialog dialog(this, tr("관심 리스트 CSV 가져오기"), QDir::currentPath(), tr("CSV (*.csv)"));
        dialog.setObjectName("chartWatchlistImportDialog");
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setFileMode(QFileDialog::ExistingFile);
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
        QFile file(dialog.selectedFiles().first());
        QString error;
        if (!file.open(QIODevice::ReadOnly)) { showListMessage(tr("CSV를 읽지 못했습니다."),true); return; }
        const auto id = watchlists_->importCsv(file, &error);
        if (id.isEmpty()) showListMessage(error,true); else selectWatchlist(id);
    });
    action(tr("CSV 내보내기"), "chartListExportCsv", true, [this] {
        if (!watchlists_) return;
        const auto bytes = watchlists_->exportCsv(currentListId());
        if (bytes.isEmpty()) return;
        QFileDialog dialog(this, tr("관심 리스트 CSV 내보내기"), QDir::current().filePath("KEEFI-watchlist.csv"), tr("CSV (*.csv)"));
        dialog.setObjectName("chartWatchlistExportDialog");
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("csv");
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
        QSaveFile file(dialog.selectedFiles().first());
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) showListMessage(tr("CSV를 저장하지 못했습니다."),true);
    });
    action(tr("리스트 알림 설정"), "chartListAlerts", false, [this] { if (!currentListId().isEmpty()) emit listAlertsRequested(currentListId()); });
    listMenu_->addSeparator();listMenu_->addAction(removeList);
    auto* recent = new QMenu(tr("최근 사용한 리스트"),listMenu_);
    recent->setObjectName("chartListRecent");
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        recent->clear();
        if (watchlists_) for (const auto& id : watchlists_->recentLists()) {
            const auto list = watchlists_->userList(id);
            if (list.id.isEmpty()) continue;
            connect(recent->addAction(list.name), &QAction::triggered, this, [this, id] { selectWatchlist(id); });
        }
    });
    connect(listMenu_, &QMenu::aboutToShow, this, [this] {
        const bool editable = watchlists_ && !watchlists_->userList(currentListId()).id.isEmpty();
        for (auto* value : listMenu_->findChildren<QAction*>()) if (value->property("requiresUserList").toBool()) value->setEnabled(editable);
    });
    for(const auto* name:{"chartListRenameSection","chartListMoveSection","chartListRemoveIdentity",
        "chartListMoveUp","chartListMoveDown","chartListMoveListUp","chartListMoveListDown"})
        if(auto* legacy=listMenu_->findChild<QAction*>(name))listMenu_->removeAction(legacy);
}

} // namespace keefi
