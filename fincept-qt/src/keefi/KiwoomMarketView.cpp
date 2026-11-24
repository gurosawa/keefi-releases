#include "keefi/KiwoomMarketView.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/InfoButton.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/PortfolioController.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/UsabilityPresentation.h"
#include <QComboBox>
#include <QDateTime>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QTimeZone>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace keefi {
namespace {
constexpr int MaxDisplayedRows=1000;
QString receiptText(qint64 at){
    return at>0?QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("MM-dd HH:mm:ss.zzz")+" KST":KiwoomMarketView::tr("수신 시각 없음");
}
QString publicInformation(){
    return KiwoomMarketView::tr("원천: 키움증권 REST API · 표시 전용 주식 시세\n"
        "미국 실시간 범위: Nasdaq TotalView. 전체 미국 시장 통합 시세나 NBBO로 보지 않습니다.\n"
        "한 연결·한 국가의 공개 시세 소켓을 공유하며 실시간 등록 예산은 200종목입니다. 나머지 종목과 비활성 국가는 실제 받은 REST 현재가가 있을 때만 표시합니다.\n"
        "원천 UTC 시각 미제공: 원문 날짜·시각·현지 시각은 시간대를 추정하지 않고 정보창에 나누어 표시합니다. 수신 시각은 별도로 표시합니다.\n"
        "실시간 수신은 완료 봉·수익률·알림 판정용 시각의 확인을 뜻하지 않습니다. 이 시세로 OHLCV를 만들거나 완료 봉 또는 실시간 알림 입력으로 사용하지 않습니다.\n"
        "새 인증과 계좌 조회를 시작하지 않습니다. 계좌·연결에서 이미 준비한 키움 연결을 선택합니다.");
}
QString evidenceText(const AssetEvidence& evidence){
    // Scope can bind an authenticated connection. It is deliberately excluded.
    auto text=usabilitySourceTime(usabilityProviderName("kiwoom"),evidence.sourceTimeMs);
    if(evidence.sourceTimeMs<=0)text+=" · "+KiwoomMarketView::tr("원천 UTC 시각 미제공");
    return text;
}
QString rowKey(const KiwoomMarketQuote& quote){return quote.identity.key()+"|"+quote.exchange;}
}

struct KiwoomMarketView::Private {
    KiwoomMarketView* q=nullptr;
    QPointer<PortfolioController> portfolio;
    QPointer<KiwoomMarketStream> source;
    QPointer<BrokerTradingController> selectedBroker;
    QPointer<MovementColorPolicy> policy;
    QComboBox *connections=nullptr,*country=nullptr;
    QLabel* status=nullptr;
    InfoButton* information=nullptr;
    QTreeWidget* rows=nullptr;
    QTimer* timer=nullptr;
    QString selectedId,activeCountry;
    bool allowed=false,synchronizing=false;
    QHash<QString,KiwoomMarketQuote> currentQuotes;

    void bindPolicy(){
        auto* next=MovementColorPolicy::forContext(q);
        if(next==policy)return;
        if(policy)QObject::disconnect(policy,nullptr,q,nullptr);
        policy=next;
        if(policy)QObject::connect(policy,&MovementColorPolicy::changed,q,[this]{paintChanges();});
    }
    void paintChanges(){
        for(int index=0;index<rows->topLevelItemCount();++index){
            auto* item=rows->topLevelItem(index);const auto key=item->data(0,Qt::UserRole).toString();
            const auto found=currentQuotes.constFind(key);if(found==currentQuotes.cend())continue;
            const auto& quote=*found;
            item->setText(2,quote.changeText.isEmpty()?QString{}:MovementColorPolicy::marker(quote.direction)+quote.changeText);
            item->setForeground(2,MovementColorPolicy::colorFor(q,quote.direction));
        }
    }
    void eraseItem(QTreeWidgetItem* item){
        // A public-looking identity can have originated only from holdings.
        // Destroy its open public dialog along with the private-origin row.
        if(auto* widget=rows->itemWidget(item,5)){
            if(auto* info=qobject_cast<InfoButton*>(widget))info->clearPrivate();
            rows->removeItemWidget(item,5);delete widget;
        }
        delete item;
    }
    void clearRows(){
        currentQuotes.clear();
        while(rows->topLevelItemCount()>0)eraseItem(rows->topLevelItem(0));
    }
    void clearSelection(){
        const bool changed=!selectedId.isEmpty();
        selectedId.clear();selectedBroker=nullptr;clearRows();information->clearPrivate();
        {const QSignalBlocker blocker(connections);connections->clear();}
        connections->hide();
        if(changed)emit q->selectedConnectionChanged();
    }
    void synchronize(){
        if(synchronizing)return;
        const QScopedValueRollback<bool> guard(synchronizing,true);
        if(!allowed){clearSelection();refresh();return;}
        const auto previous=selectedId;
        QList<PortfolioConnection> eligible;
        if(portfolio)for(const auto& connection:portfolio->connections()){
            if(connection.account.provider=="kiwoom"&&connection.account.environment=="live"&&!connection.shared&&connection.state!="unsupported")
                eligible.append(connection);
        }
        bool retained=false;for(const auto& connection:eligible)if(connection.account.id==selectedId)retained=true;
        if(!retained)selectedId=eligible.isEmpty()?QString{}:eligible.first().account.id;
        auto* next=portfolio&&!selectedId.isEmpty()?portfolio->broker(selectedId):nullptr;
        if(previous!=selectedId||selectedBroker!=next){clearRows();information->clearPrivate();}
        selectedBroker=next;
        bool identical=connections->count()==eligible.size();
        for(qsizetype i=0;identical&&i<eligible.size();++i)
            identical=connections->itemData(i).toString()==eligible[i].account.id&&connections->itemText(i)==eligible[i].account.alias;
        {const QSignalBlocker blocker(connections);
            if(!identical){connections->clear();for(const auto& connection:eligible)connections->addItem(connection.account.alias,connection.account.id);}
            connections->setCurrentIndex(connections->findData(selectedId));}
        connections->setVisible(!eligible.isEmpty());
        if(!selectedId.isEmpty()){
            information->setPrivateAccess(true);
            information->setPrivateInformation(q->tr("선택한 키움 연결: %1\n인증·계좌 조회는 계좌·연결에서 관리합니다.").arg(connections->currentText()));
        }else information->clearPrivate();
        refresh();
        // Root owns the one source binding. Rows were removed before its signal.
        if(previous!=selectedId)emit q->selectedConnectionChanged();
    }
    void renderQuotes(const QList<KiwoomMarketQuote>& quotes){
        const int oldScroll=rows->verticalScrollBar()->value();
        QString selectedKey;if(auto* current=rows->currentItem())selectedKey=current->data(0,Qt::UserRole).toString();
        QHash<QString,QTreeWidgetItem*> existing;
        for(int i=0;i<rows->topLevelItemCount();++i){auto* item=rows->topLevelItem(i);existing.insert(item->data(0,Qt::UserRole).toString(),item);}
        currentQuotes.clear();QSet<QString> retained;
        int index=0;
        for(const auto& quote:quotes){
            if(index>=MaxDisplayedRows)break;
            const auto key=rowKey(quote);if(retained.contains(key))continue;retained.insert(key);currentQuotes.insert(key,quote);
            auto* item=existing.value(key,nullptr);
            if(!item){item=new QTreeWidgetItem;item->setData(0,Qt::UserRole,key);rows->addTopLevelItem(item);}
            const auto shownIdentity=ChartInstrumentPresentation::fallback(quote.identity).text();
            item->setText(0,shownIdentity+(quote.exchange.isEmpty()?QString{}:" · "+quote.exchange));
            item->setText(1,priceAmount(quote.price)+" "+quote.currency);
            const auto sourceLabel=quote.identity.source=="US"?(quote.realtime?q->tr("키움 Nasdaq TotalView"):q->tr("키움 미국")):q->tr("키움");
            item->setText(3,sourceLabel+(quote.realtime?q->tr(" 실시간"):q->tr(" REST 현재가")));
            item->setText(4,receiptText(quote.evidence.receivedAtMs));
            for(int column=0;column<5;++column)item->setToolTip(column,usabilitySourceTime(usabilityProviderName("kiwoom"),quote.evidence.sourceTimeMs));
            auto* info=qobject_cast<InfoButton*>(rows->itemWidget(item,5));
            if(!info){info=new InfoButton(rows);info->setObjectName("kiwoomQuoteInfo");rows->setItemWidget(item,5,info);}
            QString detail=shownIdentity+"\n"+q->tr("거래소: %1 · 통화: %2").arg(quote.exchange,quote.currency)+"\n"+evidenceText(quote.evidence);
            if(!quote.rawDate.isEmpty())detail+="\n"+q->tr("원문 날짜: %1").arg(quote.rawDate);
            if(!quote.rawTime.isEmpty())detail+="\n"+q->tr("원문 시각: %1").arg(quote.rawTime);
            if(!quote.rawLocalTime.isEmpty())detail+="\n"+q->tr("원문 현지 시각: %1").arg(quote.rawLocalTime);
            detail+="\n"+q->tr("원문 시각의 시간대는 추정하지 않습니다. 완료 봉·알림 입력으로 사용하지 않습니다.");
            if(!quote.changeText.isEmpty())detail+="\n\n"+q->tr("등락률 원천 · 가격과 별도 시각")+"\n"+evidenceText(quote.changeEvidence);
            info->setInformation(shownIdentity+q->tr(" 시세"),detail);
            ++index;
        }
        for(auto iterator=existing.cbegin();iterator!=existing.cend();++iterator)if(!retained.contains(iterator.key()))eraseItem(iterator.value());
        if(!selectedKey.isEmpty())for(int i=0;i<rows->topLevelItemCount();++i){auto* item=rows->topLevelItem(i);if(item->data(0,Qt::UserRole).toString()==selectedKey){rows->setCurrentItem(item);break;}}
        rows->verticalScrollBar()->setValue(oldScroll);paintChanges();
    }
    void refresh(){
        bindPolicy();country->setEnabled(allowed&&!selectedId.isEmpty());
        if(!allowed){clearRows();status->setText(q->tr("금고를 열고 계좌·연결에서 키움 연결을 준비하면 표시합니다."));return;}
        if(selectedId.isEmpty()||!selectedBroker){clearRows();status->setText(q->tr("계좌·연결에서 키움 연결을 준비해 주세요."));return;}
        if(!source){clearRows();status->setText(q->tr("표시용 시세 연결 없음 · 새 인증이나 계좌 조회는 시작하지 않습니다."));return;}
        if(source->broker()!=selectedBroker){clearRows();status->setText(q->tr("선택한 연결의 표시용 시세 준비 중"));return;}
        const auto quotes=source->quotes();renderQuotes(quotes);
        QString summary=source->status();const auto realtime=source->realtimeTargets().size(),rest=source->restOnlyTargets().size();
        summary+="\n"+q->tr("실시간 등록 %1 / 200 · REST 대상 %2 · 원천 UTC 시각 미제공은 정보에서 확인").arg(realtime).arg(rest);
        if(quotes.size()>MaxDisplayedRows)summary+="\n"+q->tr("화면에는 받은 시세 중 앞 %1행을 표시합니다.").arg(MaxDisplayedRows);
        status->setText(summary);
    }
};

KiwoomMarketView::KiwoomMarketView(PortfolioController* portfolio,KiwoomMarketStream* source,QWidget* parent)
    :QWidget(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->portfolio=portfolio;d_->source=source;setObjectName("kiwoomMarketView");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,12,16,12);layout->setSpacing(8);
    auto* heading=new QHBoxLayout;auto* title=new QLabel(tr("키움 주식 시세"),this);title->setObjectName("kiwoomMarketTitle");title->setTextFormat(Qt::PlainText);title->setProperty("type","sectionHeading");heading->addWidget(title,1);
    d_->information=new InfoButton(this);d_->information->setObjectName("kiwoomMarketInfo");d_->information->setInformation(tr("키움 주식 시세"),publicInformation());heading->addWidget(d_->information);layout->addLayout(heading);
    d_->connections=new QComboBox(this);d_->connections->setObjectName("kiwoomMarketConnections");d_->connections->setAccessibleName(tr("이미 준비한 키움 연결 선택"));d_->connections->setMinimumHeight(40);d_->connections->hide();layout->addWidget(d_->connections);
    auto* countryRow=new QHBoxLayout;auto* countryLabel=new QLabel(tr("실시간 국가"),this);countryLabel->setTextFormat(Qt::PlainText);countryRow->addWidget(countryLabel);
    d_->country=new QComboBox(this);d_->country->setObjectName("kiwoomMarketCountry");d_->country->setAccessibleName(tr("공유 실시간 시세 국가"));d_->country->addItem(tr("실시간 중지"),QString{});d_->country->addItem(tr("국내 (KR)"),"KR");d_->country->addItem(tr("미국 (US)"),"US");d_->country->setMinimumHeight(40);countryRow->addWidget(d_->country,1);layout->addLayout(countryRow);
    d_->status=new QLabel(this);d_->status->setObjectName("kiwoomMarketStatus");d_->status->setTextFormat(Qt::PlainText);d_->status->setWordWrap(true);layout->addWidget(d_->status);
    d_->rows=new QTreeWidget(this);d_->rows->setObjectName("kiwoomMarketQuotes");d_->rows->setAccessibleName(tr("키움 표시용 주식 시세"));d_->rows->setColumnCount(6);d_->rows->setHeaderLabels({tr("종목"),tr("현재가"),tr("등락률"),tr("원천 상태"),tr("수신 시각"),tr("정보")});
    d_->rows->setRootIsDecorated(false);d_->rows->setAlternatingRowColors(true);d_->rows->setUniformRowHeights(true);d_->rows->setSelectionMode(QAbstractItemView::SingleSelection);d_->rows->setMinimumHeight(160);d_->rows->setTextElideMode(Qt::ElideRight);d_->rows->header()->setStretchLastSection(false);
    for(int i=0;i<5;++i)d_->rows->header()->setSectionResizeMode(i,QHeaderView::ResizeToContents);d_->rows->header()->setSectionResizeMode(0,QHeaderView::Stretch);d_->rows->header()->setSectionResizeMode(5,QHeaderView::Fixed);d_->rows->setColumnWidth(5,44);layout->addWidget(d_->rows,1);
    connect(d_->connections,&QComboBox::currentIndexChanged,this,[this]{
        if(!d_->allowed)return;const auto next=d_->connections->currentData().toString();if(next==d_->selectedId)return;
        d_->clearRows();d_->information->clearPrivate();d_->selectedId=next;d_->selectedBroker=d_->portfolio&&!next.isEmpty()?d_->portfolio->broker(next):nullptr;
        d_->information->setPrivateAccess(true);d_->information->setPrivateInformation(tr("선택한 키움 연결: %1\n인증·계좌 조회는 계좌·연결에서 관리합니다.").arg(d_->connections->currentText()));
        d_->refresh();emit selectedConnectionChanged();
    });
    connect(d_->country,&QComboBox::activated,this,[this]{if(d_->allowed)emit activeCountryRequested(d_->country->currentData().toString());});
    if(portfolio){connect(portfolio,&PortfolioController::changed,this,[this]{d_->synchronize();});connect(portfolio,&PortfolioController::aboutToReset,this,&KiwoomMarketView::clearPrivate);
        connect(portfolio,&QObject::destroyed,this,[this]{d_->portfolio=nullptr;clearPrivate();});}
    if(source){connect(source,&KiwoomMarketStream::changed,this,[this]{d_->refresh();});connect(source,&QObject::destroyed,this,[this]{d_->source=nullptr;d_->refresh();});}
    d_->timer=new QTimer(this);d_->timer->setInterval(1000);connect(d_->timer,&QTimer::timeout,this,[this]{if(isVisible())d_->refresh();});d_->timer->start();d_->refresh();
}
KiwoomMarketView::~KiwoomMarketView(){
    d_->timer->stop();if(d_->portfolio)disconnect(d_->portfolio,nullptr,this,nullptr);if(d_->source)disconnect(d_->source,nullptr,this,nullptr);if(d_->policy)disconnect(d_->policy,nullptr,this,nullptr);
    for(auto* child:findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);
    d_->clearRows();d_->information->clearPrivate();
}
void KiwoomMarketView::setPrivateEnabled(bool allowed){if(d_->allowed==allowed)return;if(!allowed){clearPrivate();return;}d_->allowed=true;d_->synchronize();}
bool KiwoomMarketView::privateEnabled()const{return d_->allowed;}
void KiwoomMarketView::clearPrivate(){d_->allowed=false;d_->clearSelection();d_->refresh();}
void KiwoomMarketView::selectConnection(const QString& id) {
    if (!d_->allowed || id.isEmpty()) return;
    d_->synchronize();
    const auto index = d_->connections->findData(id);
    if (index >= 0) d_->connections->setCurrentIndex(index);
}
QString KiwoomMarketView::selectedConnectionId()const{return d_->selectedId;}
void KiwoomMarketView::setActiveCountryDisplay(const QString& country){
    const auto next=country=="KR"||country=="US"?country:QString{};if(d_->activeCountry==next&&d_->country->currentData().toString()==next)return;d_->activeCountry=next;const QSignalBlocker blocker(d_->country);d_->country->setCurrentIndex(d_->country->findData(next));
}
void KiwoomMarketView::showEvent(QShowEvent* event){QWidget::showEvent(event);d_->synchronize();}
}
