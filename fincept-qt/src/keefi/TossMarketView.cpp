#include "keefi/TossMarketView.h"
#include "keefi/KeefiTheme.h"
#include "keefi/PortfolioController.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/TossMarketStream.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QComboBox>
#include <QDateTime>
#include <QHideEvent>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
namespace keefi {
namespace {
QString timeText(qint64 at){
    return at>0?QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("MM-dd HH:mm:ss")+" KST":QStringLiteral("원천 미제공");
}
QStringList uniqueSymbols(const QStringList& input){
    QStringList result;for(const auto& value:input){const auto symbol=value.trimmed();
        if(!symbol.isEmpty()&&symbol.size()<=80&&!result.contains(symbol))result.append(symbol);
    }return result;
}
QStringList stockIdentities(const QStringList& input){
    QStringList result;
    for(const auto& key:input){
        const auto identity=WatchlistWorkspaceList::identityFromKey(key);
        if(identity.market=="stock"&&!result.contains(identity.key()))result.append(identity.key());
    }
    return result;
}
}
TossMarketView::TossMarketView(PortfolioController* p,TossMarketStream* s,QWidget* parent):QWidget(parent),portfolio_(p),stream_(s){
    setObjectName("tossMarketView");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,12,16,12);layout->setSpacing(8);
    const auto label=[&](const char* name){auto* value=new QLabel(this);value->setObjectName(name);value->setWordWrap(true);
        value->setTextFormat(Qt::PlainText);value->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(value);return value;};
    auto* title=label("tossMarketTitle");title->setText(tr("토스 주식 실시간"));title->setProperty("type","sectionHeading");
    connections_=new QComboBox(this);connections_->setObjectName("tossMarketConnections");connections_->setAccessibleName(tr("실시간 시세에 사용할 토스 연결"));
    connections_->setMinimumHeight(44);layout->addWidget(connections_);connections_->hide();
    account_=label("tossMarketAccount");status_=label("tossMarketStatus");
    quotes_=new QLabel(this);quotes_->setObjectName("tossMarketQuotes");quotes_->setWordWrap(true);quotes_->setTextFormat(Qt::PlainText);
    quotes_->setTextInteractionFlags(Qt::TextSelectableByMouse);quotes_->setAlignment(Qt::AlignLeft|Qt::AlignTop);
    auto* scroll=new QScrollArea(this);scroll->setObjectName("tossMarketQuoteScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(quotes_);scroll->setMinimumHeight(80);scroll->setMaximumHeight(260);layout->addWidget(scroll);
    excluded_=label("tossMarketExcluded");setThemeStyle(excluded_,"color:${text3};");
    auto* limitations=label("tossMarketLimitations");limitations->setText(TossMarketStream::limitations());setThemeStyle(limitations,"color:${text3};");
    label("tossMarketSource")->setText(tr("원천: 토스증권 Open API · 표시 전용 시장 시세"));
    auto* toggle=new QPushButton(tr("원천·시각 보기"),this);toggle->setCheckable(true);toggle->setMinimumHeight(44);layout->addWidget(toggle);
    evidence_=label("tossMarketEvidence");evidence_->hide();connect(toggle,&QPushButton::toggled,evidence_,&QLabel::setVisible);
    toggle->setFlat(true);toggle->setProperty("compact",true);
    connect(connections_,&QComboBox::currentIndexChanged,this,[this]{
        const auto next=connections_->currentData().toString();const bool changed=selectedConnection_!=next;
        selectedConnection_=next;synchronize();if(changed)emit selectedConnectionChanged();
    });
    if(portfolio_){connect(portfolio_,&PortfolioController::changed,this,&TossMarketView::synchronize);
        connect(portfolio_,&PortfolioController::aboutToReset,this,&TossMarketView::clearPrivate);
        connect(portfolio_,&QObject::destroyed,this,[this]{portfolio_=nullptr;clearPrivate();});}
    if(stream_){connect(stream_,&TossMarketStream::changed,this,&TossMarketView::refresh);
        connect(stream_,&QObject::destroyed,this,[this]{stream_=nullptr;refresh();});}
    auto* timer=new QTimer(this);timer->setInterval(1000);connect(timer,&QTimer::timeout,this,[this]{if(isVisible())refresh();});timer->start();refresh();
}
TossMarketView::~TossMarketView(){
    if(stream_){disconnect(stream_,nullptr,this,nullptr);stream_->stop();stream_->setBroker(nullptr);}
}
void TossMarketView::setSelection(const QString& s){const auto values=uniqueSymbols({s});const auto next=values.isEmpty()?QString{}:values.first();if(next==selection_)return;selection_=next;synchronize();}
void TossMarketView::selectConnection(const QString& id) {
    if (!portfolio_ || id.isEmpty() || id == selectedConnection_) return;
    for (const auto& connection : portfolio_->connections()) {
        if (connection.account.id == id && connection.account.provider == "toss" &&
            connection.account.environment == "live" && !connection.shared) {
            selectedConnection_ = id;
            synchronize();
            emit selectedConnectionChanged();
            return;
        }
    }
}
QString TossMarketView::selectedConnectionId()const{return selectedConnection_;}
void TossMarketView::setWatchlist(const QStringList& s){const auto next=uniqueSymbols(s);if(next==watchlist_)return;watchlist_=next;synchronize();}
void TossMarketView::setMapSymbols(const QStringList& s){const auto next=uniqueSymbols(s);if(next==mapSymbols_)return;mapSymbols_=next;synchronize();}
void TossMarketView::setChartContext(bool active,const QStringList& visible,const QString& selected,const QStringList& remaining){
    const auto rows=stockIdentities(visible), rest=stockIdentities(remaining), choice=stockIdentities({selected});
    const auto next=choice.isEmpty()?QString{}:choice.first();
    if(chartActive_==active&&chartVisible_==rows&&chartSelection_==next&&chartRemaining_==rest)return;
    chartActive_=active;chartVisible_=rows;chartSelection_=next;chartRemaining_=rest;
    if(!active&&!isVisible()){broker_=nullptr;if(stream_){stream_->stop();stream_->setBroker(nullptr);}refresh();return;}
    synchronize();
}
void TossMarketView::showEvent(QShowEvent* e){QWidget::showEvent(e);synchronize();}
void TossMarketView::hideEvent(QHideEvent* e){
    QWidget::hideEvent(e);if(!chartActive_){broker_=nullptr;if(stream_){stream_->stop();stream_->setBroker(nullptr);}}refresh();
}
void TossMarketView::synchronize(){
    if(synchronizing_||(!isVisible()&&!chartActive_))return;synchronizing_=true;
    const auto previousSelection=selectedConnection_;
    if(!portfolio_){synchronizing_=false;clearPrivate();return;}
    QList<PortfolioConnection> eligible;
    for(const auto& c:portfolio_->connections())if(c.account.provider=="toss"&&c.account.environment=="live"&&!c.shared&&c.state!="unsupported")eligible.append(c);
    if(eligible.isEmpty()){synchronizing_=false;clearPrivate();return;}
    bool found=false;for(const auto& c:eligible)if(c.account.id==selectedConnection_)found=true;
    if(!found)selectedConnection_=eligible.first().account.id;
    bool same=connections_->count()==eligible.size();
    for(qsizetype i=0;same&&i<eligible.size();++i)same=connections_->itemData(i).toString()==eligible[i].account.id&&connections_->itemText(i)==eligible[i].account.alias;
    {const QSignalBlocker block(connections_);if(!same){connections_->clear();for(const auto& c:eligible)connections_->addItem(c.account.alias,c.account.id);}
        connections_->setCurrentIndex(connections_->findData(selectedConnection_));}
    connections_->setVisible(eligible.size()>1);account_->setText(connections_->currentText());
    auto* selected=portfolio_->broker(selectedConnection_);if(selected&&!selected->authenticated())selected=nullptr;
    if(selected!=broker_){if(stream_)stream_->stop();broker_=selected;if(stream_)stream_->setBroker(selected);}
    excludedReason_.clear();
    if(broker_&&stream_){
        QList<TossMarketTarget> targets;QMap<QString,int> indexes;QStringList unverified;
        const auto add=[&](const QString& symbol,int priority){
            const auto country=broker_->marketDataCountry(symbol);
            if(country!="KR"&&country!="US"){if(!unverified.contains(symbol))unverified.append(symbol);return;}
            if(!indexes.contains(symbol)){indexes.insert(symbol,targets.size());targets.append({symbol,country,priority});}
            else if(priority<targets[indexes.value(symbol)].priority)targets[indexes.value(symbol)].priority=priority;
        };
        const auto addChart=[&](const QString& key,int priority){
            const auto identity=WatchlistWorkspaceList::identityFromKey(key);
            if(identity.market!="stock"||broker_->marketDataCountry(identity.code)!=identity.source){
                if(!unverified.contains(key))unverified.append(key);
                return;
            }
            add(identity.code,priority);
        };
        if(chartActive_){
            if(!chartSelection_.isEmpty())addChart(chartSelection_,0);
        }else if(!selection_.isEmpty())add(selection_,0);
        // Borrow verified KR/US holdings without changing the G7 capture.
        for(const auto& symbol:broker_->marketDataHoldings())add(symbol,1);
        if(chartActive_){
            // Public catalog visibility does not authorize a new subscription.
            // Only saved watchlist rows receive this viewport promotion;
            // selected instruments and holdings were already added above.
            for(const auto& identity:chartVisible_)if(chartRemaining_.contains(identity))addChart(identity,2);
            for(const auto& identity:chartRemaining_)addChart(identity,3);
        }else{
            for(const auto& symbol:watchlist_)add(symbol,2);
            for(const auto& symbol:mapSymbols_)add(symbol,3);
        }
        if(!unverified.isEmpty()){
            QStringList shown;for(const auto& value:unverified){const auto identity=WatchlistWorkspaceList::identityFromKey(value);
                shown.append(identity.market=="stock"?ChartInstrumentPresentation::fallback(identity).text():usabilityPublicText(value));}
            excludedReason_=tr("시장 국가 메타데이터 미확인으로 제외: %1").arg(shown.join(", "));
        }
        stream_->setTargets(targets);
    }
    synchronizing_=false;refresh();if(previousSelection!=selectedConnection_)emit selectedConnectionChanged();
}
void TossMarketView::clearPrivate(){
    const bool changed=!selectedConnection_.isEmpty();
    broker_=nullptr;selectedConnection_.clear();excludedReason_.clear();
    if(stream_){stream_->stop();stream_->setBroker(nullptr);}
    const QSignalBlocker block(connections_);connections_->clear();connections_->hide();account_->clear();quotes_->clear();evidence_->clear();excluded_->clear();
    status_->setText(tr("금고를 열고 토스 계좌를 연결하면 표시합니다."));
    findChild<QScrollArea*>("tossMarketQuoteScroll")->hide();
    if(changed)emit selectedConnectionChanged();
}
void TossMarketView::refresh(){
    quotes_->clear();evidence_->clear();excluded_->setText(excludedReason_);
    findChild<QScrollArea*>("tossMarketQuoteScroll")->hide();
    if(!isVisible()){status_->setText(tr("화면을 열면 실시간 시세를 구독합니다."));return;}
    if(!stream_){status_->setText(tr("실시간 시세 연결 없음"));return;}
    if(!broker_||!broker_->authenticated()){
        status_->setText(selectedConnection_.isEmpty()?tr("금고를 열고 토스 계좌를 연결하면 표시합니다."):tr("계좌·연결에서 토스 연결·재인증을 완료해 주세요."));return;
    }
    status_->setText(stream_->status()+"\n"+stream_->coverage());QStringList lines,evidence;
    for(const auto& quote:stream_->quotes()){
        const auto& e=quote.evidence;
        lines.append(quote.symbol+" · "+priceAmount(quote.price)+" "+quote.currency+"\n"+tr("관측 %1 · 수신 %2").arg(timeText(e.sourceTimeMs),timeText(e.receivedAtMs)));
        evidence.append(usabilitySourceTime(usabilityProviderName("toss"),e.sourceTimeMs));
    }
    quotes_->setText(lines.join("\n\n"));evidence_->setText(evidence.join("\n\n"));
    findChild<QScrollArea*>("tossMarketQuoteScroll")->setVisible(!lines.isEmpty());
    const auto rejected=stream_->rejectedTopics();
    if(!rejected.isEmpty())excluded_->setText(excludedReason_+(excludedReason_.isEmpty()?QString{}:"\n")+tr("원천에서 거절한 구독: %1").arg(rejected.join(", ")));
    QStringList restOnly;for(const auto& target:stream_->restOnlyTargets())restOnly.append(target.symbol);
    if(!restOnly.isEmpty())excluded_->setText(excluded_->text()+(excluded_->text().isEmpty()?QString{}:"\n")+
        tr("실시간 대신 REST 현재가로 확인: %1").arg(restOnly.join(", ")));
}
}
