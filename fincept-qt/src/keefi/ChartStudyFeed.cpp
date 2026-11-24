#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartStudyRequestPacer.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartMarketResolver.h"
#include <QRegularExpression>
#include "keefi/StockChartSource.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QMap>
#include <QPointer>
#include <QScopedValueRollback>
#include <QTimer>
#include <limits>
namespace keefi {
namespace {
QString key(const QString& identity,qint64 interval){return identity+"@"+QString::number(interval);}
QString key(const ChartStudyRequest& r){return key(r.identity.key(),r.intervalMs);}
qint64 after(qint64 now,qint64 delta){return now>std::numeric_limits<qint64>::max()-delta?std::numeric_limits<qint64>::max():now+delta;}
}
struct ChartStudyFeed::Private {
    struct Lane {ChartStudyRequest request;ChartStudyObservation value;qint64 due=0;};
    ChartStudyFeed* q=nullptr;QPointer<TradingTransport> http;QPointer<ChartInstrumentCatalog> catalog;
    QPointer<StockChartSource> stock;Clock clock;QTimer* timer=nullptr;
    QMap<QString,QList<ChartStudyRequest>> owners;QMap<QString,Lane> lanes;QSet<QString> active;
    QString current;bool stockPending=false,pumping=false;quint64 token=0,nextToken=1;
    qint64 sentAt=0,nextSend=0;
    bool guardUpbit=false,upbitAvailable=false;
    QSet<QString> upbitMarkets,unsupportedIdentities;
    bool upbitEligible(const WatchlistIdentity& identity)const{
        return identity.source!="upbit" || (!unsupportedIdentities.contains(identity.key()) &&
            (!guardUpbit || (upbitAvailable&&upbitMarkets.contains(identity.code))));
    }
    QString upbitStatus(const WatchlistIdentity& identity)const{
        return unsupportedIdentities.contains(identity.key()) || upbitAvailable ? QObject::tr("지원 없음") : QObject::tr("종목 확인 중");
    }
    std::shared_ptr<ChartStudyRequestPacer> pacer;ChartStudyRequestPacer::Client pacingClient=0;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    void cancel(){const bool pending=token!=0,wasStock=stockPending;token=0;current.clear();stockPending=false;
        if(pacer)pacer->cancel(pacingClient);if(pending&&http)http->abort();if(wasStock&&stock)stock->stop();}
    void rebuild(){
        QMap<QString,ChartStudyRequest> requests;
        for(const auto& rows:owners)for(const auto& r:rows){const auto k=key(r);
            if(!requests.contains(k)||r.priority<requests[k].priority)requests[k]=r;}
        active.clear();for(auto it=requests.cbegin();it!=requests.cend();++it){active.insert(it.key());lanes[it.key()].request=it.value();}
        if(!current.isEmpty()&&!active.contains(current))cancel();
        while(lanes.size()>128){auto it=lanes.begin();while(it!=lanes.end()&&active.contains(it.key()))++it;
            if(it==lanes.end())break;lanes.erase(it);}
        if(active.isEmpty()){timer->stop();if(pacer)pacer->cancel(pacingClient);}else timer->start();
    }
    void publish(const QString& k){if(lanes.contains(k)&&active.contains(k))emit q->changed(lanes[k].request.identity.key(),lanes[k].request.intervalMs);}
    void suppressUpbit(const QString& k){
        auto& lane=lanes[k];const auto identity=lane.request.identity;
        const bool retain=!upbitAvailable&&upbitMarkets.contains(identity.code)&&
            !unsupportedIdentities.contains(identity.key())&&lane.value.series.available;
        const auto status=retain?QObject::tr("지연"):upbitStatus(identity);
        const bool changed=lane.value.status!=status||lane.value.loading||(!retain&&lane.value.series.available);
        if(retain){lane.value.loading=false;lane.value.delayed=true;lane.value.series.forming.reset();}
        else lane.value={};
        lane.value.status=status;lane.due=std::numeric_limits<qint64>::max();if(changed)publish(k);
    }
    void completeStock(){
        if(!stockPending||!stock||!active.contains(current))return;
        const auto snap=stock->snapshot(now());const auto k=current;const auto request=lanes[k].request;
        if(snap.series.identity!=request.identity.key()||snap.series.intervalMs!=request.intervalMs)return;
        if(snap.loading)return;
        auto& lane=lanes[k];lane.value={snap.series,snap.timeline,snap.series.reason,true,false,snap.delayed};
        lane.value.series.forming.reset();
        if(!snap.series.available&&lane.value.status.isEmpty())lane.value.status=QObject::tr("증권사 연결 필요");
        lane.due=after(now(),snap.series.available?qMax<qint64>(60000,request.intervalMs):30000);
        current.clear();stockPending=false;stock->stop();publish(k);
    }
    void finished(quint64 received,const HttpResult& result){
        if(!token||token!=received||current.isEmpty()||!active.contains(current))return;
        const auto k=current;const auto request=lanes[k].request;const auto at=now();
        MarketInstrument instrument;
        if(request.identity.source=="binance"&&catalog)for(const auto& item:catalog->instruments("binance",at))if(item.symbol==request.identity.code){instrument=item;break;}
        const auto error=parseExactJson(result.body).object().value("error").toObject();
        if(request.identity.source=="upbit"&&result.status==404&&error.value("message").toString()=="Code not found"){
            token=0;current.clear();unsupportedIdentities.insert(request.identity.key());
            const auto keys=lanes.keys();
            for(const auto& other:keys)if(lanes.contains(other)&&lanes[other].request.identity==request.identity){
                auto& lane=lanes[other];lane.value={};lane.value.status=QObject::tr("지원 없음");lane.due=std::numeric_limits<qint64>::max();publish(other);
            }
            return;
        }
        auto series=MarketCandles::parse(request.identity.key(),request.intervalMs,result,at,sentAt,instrument);
        series.forming.reset();token=0;current.clear();auto& lane=lanes[k];lane.value.loading=false;
        if(series.available){lane.value={series,{},QString{},false,false,false};
            const auto completed=series.candles.isEmpty()?at:series.candles.last().startsAtMs+request.intervalMs;
            lane.due=qMax(after(at,1000),after(completed,request.intervalMs+1000));
        }else{lane.value.status=QObject::tr("받지 못했어요");lane.value.delayed=lane.value.series.available;lane.due=after(at,30000);}
        if(result.status==429||result.localRetryAfterSeconds>0){
            qint64 delay=qMax<qint64>(1000,qint64(result.localRetryAfterSeconds)*1000);
            for(auto it=result.headers.cbegin();it!=result.headers.cend();++it)if(it.key().compare("Retry-After",Qt::CaseInsensitive)==0){
                bool ok=false;const auto seconds=it.value().toLongLong(&ok);if(ok&&seconds>0&&seconds<86400)delay=qMax(delay,seconds*1000);}
            nextSend=qMax(nextSend,after(at,delay));lane.due=qMax(lane.due,nextSend);
        }
        publish(k);
    }
};
ChartStudyFeed::ChartStudyFeed(TradingTransport* transport,ChartInstrumentCatalog* catalog,QObject* parent,Clock clock)
    :QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->http=transport;d_->catalog=catalog;d_->clock=std::move(clock);d_->timer=new QTimer(this);d_->timer->setInterval(100);
    connect(d_->timer,&QTimer::timeout,this,&ChartStudyFeed::pump);
    if(transport){connect(transport,&TradingTransport::finished,this,[this](quint64 t,const HttpResult& r){d_->finished(t,r);});
        connect(transport,&QObject::destroyed,this,[this]{d_->http=nullptr;stop();});}
}
ChartStudyFeed::~ChartStudyFeed(){if(d_->http)disconnect(d_->http,nullptr,this,nullptr);if(d_->stock)disconnect(d_->stock,nullptr,this,nullptr);stop();if(d_->pacer)d_->pacer->release(d_->pacingClient);}
void ChartStudyFeed::setRequestPacer(std::shared_ptr<ChartStudyRequestPacer> value){if(d_->pacer==value)return;if(d_->pacer)d_->pacer->release(d_->pacingClient);d_->pacer=std::move(value);d_->pacingClient=d_->pacer?d_->pacer->createClient():0;}
void ChartStudyFeed::setVerifiedUpbitMarkets(const QStringList& values,bool available){
    QSet<QString> next;static const QRegularExpression pattern("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,20}\\z");
    for(const auto& code:values)if(pattern.match(code).hasMatch())next.insert(code);
    QSet<QString> previouslyEligible;const auto keys=d_->lanes.keys();
    for(const auto& k:keys)if(d_->upbitEligible(d_->lanes[k].request.identity))previouslyEligible.insert(k);
    if(available){for(const auto& code:next)if(!d_->upbitMarkets.contains(code))d_->unsupportedIdentities.remove("coin:upbit:"+code);d_->upbitMarkets=next;}
    d_->guardUpbit=true;d_->upbitAvailable=available;
    if(d_->lanes.contains(d_->current)&&!d_->upbitEligible(d_->lanes[d_->current].request.identity))d_->cancel();
    for(const auto& k:keys){
        if(!d_->lanes.contains(k)||d_->lanes[k].request.identity.source!="upbit")continue;
        auto& lane=d_->lanes[k];const bool eligible=d_->upbitEligible(lane.request.identity);
        if(!eligible)d_->suppressUpbit(k);
        else if(!previouslyEligible.contains(k)){lane.due=0;if(!lane.value.series.available)lane.value={};d_->publish(k);}
    }
}
void ChartStudyFeed::setDemands(const QString& owner,const QList<ChartStudyRequest>& input){
    if(owner.isEmpty()||owner.size()>80)return;QList<ChartStudyRequest> accepted;QSet<QString> seen;
    const QList<qint64> intervals{60000,300000,900000,3600000,14400000,86400000};
    // Ranking can show twenty rows in each of three markets. Other owners keep
    // their existing 32-row limit; all owners still share the bounded 128 cache.
    const int limit=owner=="explore-ranking"?60:32;
    for(const auto& request:input){if(accepted.size()>=limit)break;
        if(!WatchlistWorkspaceList::validIdentity(request.identity)||!intervals.contains(request.intervalMs))continue;
        const auto k=key(request);if(seen.contains(k))continue;seen.insert(k);accepted.append(request);}
    if(accepted.isEmpty())d_->owners.remove(owner);else if(d_->owners.contains(owner)||d_->owners.size()<4)d_->owners[owner]=accepted;
    d_->rebuild();
}
void ChartStudyFeed::clearOwner(const QString& owner) {
    if (d_->owners.remove(owner)) d_->rebuild();
}
void ChartStudyFeed::setStockSource(StockChartSource* source){
    if(d_->stock==source)return;if(d_->stock){disconnect(d_->stock,nullptr,this,nullptr);d_->stock->stop();}
    if(d_->stockPending){d_->current.clear();d_->stockPending=false;}d_->stock=source;
    if(source){connect(source,&StockChartSource::changed,this,[this]{d_->completeStock();});
        connect(source,&QObject::destroyed,this,[this]{d_->stock=nullptr;clearPrivate();});}
}
void ChartStudyFeed::clearPrivate(){
    for(auto& rows:d_->owners)rows.erase(std::remove_if(rows.begin(),rows.end(),[](const auto& r){return r.identity.market=="stock";}),rows.end());
    d_->rebuild();for(auto it=d_->lanes.begin();it!=d_->lanes.end();)if(it->request.identity.market=="stock")it=d_->lanes.erase(it);else ++it;
    if(d_->stock)d_->stock->stop();
}
void ChartStudyFeed::stop(){d_->owners.clear();d_->active.clear();d_->timer->stop();d_->cancel();d_->lanes.clear();}
void ChartStudyFeed::pump(){
    if(d_->pumping||d_->active.isEmpty())return;QScopedValueRollback guard(d_->pumping,true);const auto at=d_->now();
    if(!d_->current.isEmpty()){
        if(d_->stockPending)d_->completeStock();
        if(!d_->current.isEmpty()){
            if(at-d_->sentAt<15000)return;const auto k=d_->current;d_->cancel();
            if(d_->lanes.contains(k)){auto& lane=d_->lanes[k];lane.value.loading=false;lane.value.delayed=lane.value.series.available;lane.value.status=tr("받지 못했어요");lane.due=after(at,30000);d_->publish(k);}return;
        }
    }
    if(at<d_->nextSend)return;
    QString selected;int priority=std::numeric_limits<int>::max();qint64 due=std::numeric_limits<qint64>::max();
    for(const auto& k:d_->active.values()){
        if(!d_->lanes.contains(k)||!d_->active.contains(k))continue;
        auto& lane=d_->lanes[k];
        if(!d_->upbitEligible(lane.request.identity)){d_->suppressUpbit(k);continue;}
        if(lane.due>at)continue;
        if(selected.isEmpty()||lane.request.priority<priority||(lane.request.priority==priority&&(lane.due<due||(lane.due==due&&k<selected)))){selected=k;priority=lane.request.priority;due=lane.due;}}
    if(selected.isEmpty()||!d_->active.contains(selected)||!d_->lanes.contains(selected)){if(d_->pacer)d_->pacer->cancel(d_->pacingClient);return;}auto& lane=d_->lanes[selected];const auto request=lane.request;
    if(request.identity.market=="stock"){
        if(d_->pacer)d_->pacer->cancel(d_->pacingClient);d_->nextSend=after(at,1000);d_->sentAt=at;
        if(!d_->stock){lane.value.status=tr("증권사 연결 필요");lane.due=after(at,30000);d_->publish(selected);return;}
        d_->current=selected;d_->stockPending=true;lane.value.loading=true;
        d_->stock->setSelection(request.identity,request.intervalMs,false);d_->stock->setActive(true);d_->completeStock();return;
    }
    if(request.identity.source=="binance"){
        const auto rows=d_->catalog?d_->catalog->instruments("binance",at):QList<MarketInstrument>{};
        const auto route=resolveChartMarket(request.identity,{},false,rows);
        if(!route.available()){
            if(d_->catalog&&rows.isEmpty())d_->catalog->ensure("binance");
            lane.value.status=route.availability==ChartMarketAvailability::Checking?tr("종목 확인 중"):tr("지원 없음");
            lane.value.loading=false;lane.due=after(at,1000);d_->publish(selected);return;
        }
    }
    const auto spec=MarketCandles::request(request.identity.key(),request.intervalMs);
    if(!d_->http||!MarketCandles::isRequest(spec)){lane.value.status=tr("지원 없음");lane.due=after(at,60000);d_->publish(selected);return;}
    if(d_->pacer&&!d_->pacer->tryAcquire(d_->pacingClient,spec.url))return;
    d_->nextSend=after(at,1000);d_->sentAt=at;
    d_->current=selected;d_->token=d_->nextToken++;lane.value.loading=true;lane.value.status=tr("받는 중");
    d_->http->send(d_->token,spec);
}
ChartStudyObservation ChartStudyFeed::observation(const QString& identity,qint64 interval)const{
    const auto k=key(identity,interval);if(!d_->lanes.contains(k)){ChartStudyObservation missing;missing.status=tr("받는 중");return missing;}
    auto value=d_->lanes[k].value;if(value.status.isEmpty()&&!value.series.available)value.status=tr("받는 중");
    if(value.series.available&&d_->now()>d_->lanes[k].due&&d_->now()-d_->lanes[k].due>30000){value.delayed=true;value.status=tr("지연");}
    if(value.series.available&&!value.stock&&interval>0&&!value.series.candles.isEmpty()&&
       value.series.candles.last().startsAtMs+interval<d_->now()/interval*interval){value.delayed=true;value.status=tr("지연");}
    return value;
}
int ChartStudyFeed::demandCount()const{return d_->active.size();}
int ChartStudyFeed::ownerCount(const QString& identity,qint64 interval)const{
    int count=0;for(const auto& rows:d_->owners)for(const auto& r:rows)if(key(r)==key(identity,interval)){++count;break;}return count;
}
}
