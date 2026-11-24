#include "keefi/ChartAlertFeed.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartMarketResolver.h"
#include "keefi/TradingTransport.h"
#include "keefi/StockChartTypes.h"
#include "keefi/StockChartSource.h"
#include "keefi/CoinFlowSource.h"
#include <QDateTime>
#include <QLocale>
#include <QMap>
#include <QScopedValueRollback>
#include <QRegularExpression>
#include <QTimer>
#include <QTimeZone>
#include <limits>
namespace keefi {
namespace {
QString laneKey(const ChartAlertDemand& d){return d.identity.key()+"@"+QString::number(d.intervalMs);}
qint64 after(qint64 now,qint64 delay){return now>std::numeric_limits<qint64>::max()-delay?std::numeric_limits<qint64>::max():now+delay;}
qint64 retryDelay(const HttpResult& response,qint64 now){
    qint64 delay=qMax<qint64>(1000,qint64(response.localRetryAfterSeconds)*1000);
    for(auto i=response.headers.cbegin();i!=response.headers.cend();++i){
        if(i.key().compare("Retry-After",Qt::CaseInsensitive)!=0)continue;
        bool ok=false;const auto seconds=i.value().toLongLong(&ok);
        if(ok&&seconds>0){delay=qMax(delay,seconds>std::numeric_limits<qint64>::max()/1000?std::numeric_limits<qint64>::max():seconds*1000);continue;}
        auto date=QLocale::c().toDateTime(QString::fromLatin1(i.value()),"ddd, dd MMM yyyy HH:mm:ss 'GMT'");
        if(date.isValid()){date.setTimeZone(QTimeZone::UTC);if(date.toMSecsSinceEpoch()>now)delay=qMax(delay,date.toMSecsSinceEpoch()-now);}
    }
    return delay;
}
}
struct ChartAlertFeed::Private {
    struct Lane {ChartAlertDemand demand;quint64 generation=0;bool begun=false,marketBlocked=false;qint64 due=0;
        QMap<qint64,CoinFlowClosedMinute> flowMinutes;bool flowSeeded=false;};
    ChartAlertFeed* q=nullptr;QPointer<TradingTransport> transport;QPointer<ChartInstrumentCatalog> catalog;
    QPointer<StockChartSource> stock;QString stockCurrent;
    QPointer<CoinFlowSource> flow;
    Clock clock;QMap<QString,Lane> lanes;QStringList priorities;QString current,message;
    QTimer* timer=nullptr;bool running=false,pumping=false;quint64 token=0,nextToken=1,nextGeneration=1;
    qint64 sentAt=0,nextSend=0;
    bool guardUpbit=false,upbitAvailable=false,validatingMarkets=false;
    QSet<QString> upbitMarkets,unsupportedIdentities;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    ChartMarketAvailability marketAvailability(const WatchlistIdentity& identity,qint64 at)const{
        if(identity.market!="coin")return ChartMarketAvailability::Available;
        if(identity.source=="upbit"){
            if(unsupportedIdentities.contains(identity.key()))return ChartMarketAvailability::Unsupported;
            if(!guardUpbit)return ChartMarketAvailability::Available;
            if(!upbitAvailable)return ChartMarketAvailability::Checking;
            return upbitMarkets.contains(identity.code)?ChartMarketAvailability::Available:ChartMarketAvailability::Unsupported;
        }
        if(identity.source=="binance")return resolveChartMarket(identity,{},false,
            catalog?catalog->instruments("binance",at):QList<MarketInstrument>{}).availability;
        return ChartMarketAvailability::Unsupported;
    }
    void validateMarkets(bool requestMetadata=false){
        if(validatingMarkets)return;QScopedValueRollback guard(validatingMarkets,true);
        const auto at=now();const auto keys=lanes.keys();
        for(const auto& key:keys){
            if(!lanes.contains(key))continue;
            const auto identity=lanes[key].demand.identity;const auto available=marketAvailability(identity,at);
            if(available==ChartMarketAvailability::Available){
                if(lanes[key].marketBlocked){lanes[key].marketBlocked=false;lanes[key].due=0;}
                continue;
            }
            if(current==key)cancel();
            auto& lane=lanes[key];const auto previous=lane;
            lane.marketBlocked=true;lane.begun=false;lane.flowMinutes.clear();lane.flowSeeded=false;lane.due=std::numeric_limits<qint64>::max();
            message=available==ChartMarketAvailability::Checking?QObject::tr("알림 종목 목록을 확인하고 있어요."):QObject::tr("지원하지 않는 종목은 알림 자료를 받지 않아요.");
            if(previous.begun)emit q->laneInvalidated(previous.demand.identity.key(),previous.demand.intervalMs,previous.generation);
            if(requestMetadata&&running&&available==ChartMarketAvailability::Checking&&identity.source=="binance"&&catalog)catalog->ensure("binance");
        }
    }
    void flowChanged(){
        if(!running||!flow||!flow->active())return;
        validateMarkets();
        const auto value=flow->snapshot(now());
        for(const auto& lane:lanes)if(lane.begun&&lane.demand.whales&&lane.demand.intervalMs==60000&&lane.demand.identity.key()==value.identity){
            emit q->coinFlowObservationReady(value,lane.generation);return;
        }
    }
    CoinFlowSnapshot flowSnapshot(Lane& lane,const MarketCandleSeries& series){
        CoinFlowSnapshot value;value.identity=series.identity;
        value.candleSeries=series;
        value.candleSeries->forming.reset();
        const auto& identity=lane.demand.identity;auto& item=value.primary.instrument;
        item.identity=series.identity;item.symbol=identity.code;item.baseAsset=series.asset;item.quoteAsset=series.currency;
        item.venue=identity.source=="binance"?CoinFlowVenue::BinanceSpot:CoinFlowVenue::UpbitSpot;
        item.grain=identity.source=="binance"?CoinFlowGrain::SpotAggregate:CoinFlowGrain::UpbitTrade;
        const CoinFlowEvidence evidence{identity.source,series.sourceUrl,series.sourceTimeMs,series.receivedAtMs,series.receivedAtMs};
        value.primary.evidence=evidence;
        const qint64 previous=lane.flowMinutes.isEmpty()?0:lane.flowMinutes.lastKey();
        bool changedClosed=false;
        for(const auto& candle:series.candles)if(lane.flowMinutes.contains(candle.startsAtMs)){
            const auto& old=lane.flowMinutes[candle.startsAtMs].candle;
            if(old.open!=candle.open||old.high!=candle.high||old.low!=candle.low||old.close!=candle.close||old.volume!=candle.volume)changedClosed=true;
        }
        if(changedClosed){lane.flowMinutes.clear();lane.flowSeeded=false;}
        const bool oneNew=previous&&series.candles.last().startsAtMs==previous+60000;
        for(const auto& candle:series.candles){
            if(!lane.flowMinutes.contains(candle.startsAtMs)){
                CoinFlowClosedMinute minute;minute.candle=candle;minute.completesAtMs=candle.startsAtMs+60000;
                minute.knownAtMs=series.receivedAtMs;minute.decisionAtMs=series.receivedAtMs;
                minute.liveEligible=lane.flowSeeded&&oneNew&&candle.startsAtMs>previous;
                minute.connectsPrevious=lane.flowMinutes.contains(candle.startsAtMs-60000);minute.evidence=evidence;
                lane.flowMinutes.insert(candle.startsAtMs,minute);
            }
        }
        lane.flowSeeded=true;while(lane.flowMinutes.size()>200)lane.flowMinutes.erase(lane.flowMinutes.begin());
        for(const auto& candle:series.candles)value.minutes.append(lane.flowMinutes.value(candle.startsAtMs));
        if(flow&&flow->active()&&flow->snapshot(now()).identity==series.identity){
            flow->setMinuteSeries(series);auto observed=flow->snapshot(now());
            if(observed.identity==series.identity&&!observed.minutes.isEmpty()&&
               observed.minutes.last().completesAtMs==series.candles.last().startsAtMs+60000){
                if(!observed.candleSeries){observed.candleSeries=series;observed.candleSeries->forming.reset();}
                return observed;
            }
        }
        return value;
    }
    void stopStock(){stockCurrent.clear();if(stock)stock->stop();}
    void stockChanged(){
        if(!running||stockCurrent.isEmpty()||!stock||!lanes.contains(stockCurrent))return;
        const auto snapshot=stock->snapshot(now());if(snapshot.loading||!snapshot.series.available||snapshot.series.candles.isEmpty())return;
        const auto key=stockCurrent;auto& lane=lanes[key];
        if(snapshot.series.identity!=lane.demand.identity.key())return;
        const bool allowed=snapshot.realtimeAllowed&&!snapshot.delayed&&snapshot.calendarAvailable;
        if(!lane.begun){lane.begun=true;lane.generation=nextGeneration++;
            emit q->laneStarted(lane.demand.identity.key(),lane.demand.intervalMs,lane.generation,allowed);}
        if(!running||!lanes.contains(key)||stockCurrent!=key)return;
        const auto generation=lanes[key].generation;
        lanes[key].due=after(now(),qMax<qint64>(60000,lane.demand.intervalMs));
        message=allowed?QObject::tr("검증된 주식 완료 봉 확인 · 장 캘린더 적용"):
            snapshot.delayed?QObject::tr("지연 일봉 · 실시간 알림 차단 · 일봉 확정 근거 확인 필요"):
                QObject::tr("주식 실시간 알림 대기 · 원천·장 캘린더 확인 필요");
        stockCurrent.clear();stock->stop();
        emit q->stockObservationReady(snapshot,generation);emit q->changed();
    }
    void cancel(){const bool pending=token!=0;token=0;current.clear();if(pending&&transport)transport->abort();}
    void failCurrent(const QString& reason){
        const auto key=current;token=0;current.clear();message=reason;
        Lane failed;bool invalidate=false;
        if(lanes.contains(key)){auto& lane=lanes[key];failed=lane;invalidate=lane.begun;lane.begun=false;lane.flowMinutes.clear();lane.flowSeeded=false;lane.due=after(now(),5000);}
        if(invalidate)emit q->laneInvalidated(failed.demand.identity.key(),failed.demand.intervalMs,failed.generation);
        emit q->changed();
    }
    void finished(quint64 received,const HttpResult& response){
        if(!running||!token||token!=received||!lanes.contains(current))return;
        validateMarkets();if(!token||token!=received||!lanes.contains(current))return;
        const auto at=now();auto& lane=lanes[current];
        const auto error=parseExactJson(response.body).object().value("error").toObject();
        if(lane.demand.identity.source=="upbit"&&response.status==404&&error.value("message").toString()=="Code not found"){
            unsupportedIdentities.insert(lane.demand.identity.key());token=0;current.clear();validateMarkets();emit q->changed();return;
        }
        if(response.status==429||response.localRetryAfterSeconds>0){nextSend=qMax(nextSend,after(at,retryDelay(response,at)));failCurrent(QObject::tr("공개 알림 원천 요청 제한 · 재시도 대기"));return;}
        MarketInstrument instrument;
        if(lane.demand.identity.source=="binance"&&catalog)for(const auto& item:catalog->instruments("binance",at))if(item.symbol==lane.demand.identity.code){instrument=item;break;}
        auto series=MarketCandles::parse(lane.demand.identity.key(),lane.demand.intervalMs,response,at,sentAt,instrument);
        if(!series.available){failCurrent(series.reason.isEmpty()?QObject::tr("공개 완료 봉 자료 대기"):series.reason);return;}
        const auto studies=keefiChartStudies(series.candles,series.intervalMs);
        if(!studies.available&&!lane.demand.whales&&!lane.demand.horizontal){failCurrent(studies.reason);return;}
        const auto generation=lane.generation;
        const auto completed=series.candles.isEmpty()?0:series.candles.last().startsAtMs+series.intervalMs;
        lane.due=qMax(after(at,5000),after(completed,series.intervalMs+1000));
        std::optional<CoinFlowSnapshot> flowValue;
        if(lane.demand.whales&&series.intervalMs==60000&&!series.candles.isEmpty())flowValue=flowSnapshot(lane,series);
        if(!running||!lanes.contains(current))return;
        token=0;current.clear();message=QObject::tr("완료 봉 알림 수집 · %1종목").arg(lanes.size());
        if(studies.available||lane.demand.horizontal)emit q->observationReady(series,studies,generation);
        if(flowValue)emit q->coinFlowObservationReady(*flowValue,generation);emit q->changed();
    }
};
ChartAlertFeed::ChartAlertFeed(TradingTransport* transport,ChartInstrumentCatalog* catalog,QObject* parent,Clock clock):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->catalog=catalog;d_->clock=std::move(clock);
    d_->timer=new QTimer(this);d_->timer->setInterval(100);connect(d_->timer,&QTimer::timeout,this,&ChartAlertFeed::pump);
    if(transport){connect(transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& r){d_->finished(token,r);});
        connect(transport,&QObject::destroyed,this,[this]{d_->token=0;d_->current.clear();setRunning(false);});}
    if(catalog)connect(catalog,&ChartInstrumentCatalog::changed,this,&ChartAlertFeed::pump);
}
ChartAlertFeed::~ChartAlertFeed(){if(d_->transport)disconnect(d_->transport,nullptr,this,nullptr);if(d_->catalog)disconnect(d_->catalog,nullptr,this,nullptr);if(d_->stock)disconnect(d_->stock,nullptr,this,nullptr);if(d_->flow)disconnect(d_->flow,nullptr,this,nullptr);setRunning(false);}
void ChartAlertFeed::setVerifiedUpbitMarkets(const QStringList& values,bool available){
    QSet<QString> next;static const QRegularExpression pattern("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,20}\\z");
    for(const auto& code:values)if(pattern.match(code).hasMatch())next.insert(code);
    if(available){for(const auto& code:next)if(!d_->upbitMarkets.contains(code))d_->unsupportedIdentities.remove("coin:upbit:"+code);d_->upbitMarkets=next;}
    d_->guardUpbit=true;d_->upbitAvailable=available;d_->validateMarkets();
}
void ChartAlertFeed::setDemands(const QList<ChartAlertDemand>& demands){
    QMap<QString,Private::Lane> next;const QList<qint64> intervals{60000,300000,900000,3600000,14400000,86400000};
    if(demands.size()<=6400)for(const auto& demand:demands){
        if(!WatchlistWorkspaceList::validIdentity(demand.identity)||!intervals.contains(demand.intervalMs)||(!demand.pullback&&!demand.results&&!demand.whales&&!demand.horizontal))continue;
        if(demand.whales&&(demand.identity.market!="coin"||demand.intervalMs!=60000))continue;
        const auto key=laneKey(demand);
        if(next.contains(key)){auto& old=next[key].demand;old.pullback|=demand.pullback;old.results|=demand.results;old.whales|=demand.whales;old.horizontal|=demand.horizontal;old.privateOnly&=demand.privateOnly;continue;}
        auto lane=d_->lanes.value(key);lane.demand=demand;next.insert(key,lane);
    }
    const auto previous=d_->lanes;const bool cancel=d_->token&&!next.contains(d_->current);
    d_->lanes=next; // Install removal before abort/invalidation can call back.
    if(cancel)d_->cancel();
    if(!d_->stockCurrent.isEmpty()&&!next.contains(d_->stockCurrent))d_->stopStock();
    for(auto i=previous.cbegin();i!=previous.cend();++i)if(i->begun&&!next.contains(i.key()))
        emit laneInvalidated(i->demand.identity.key(),i->demand.intervalMs,i->generation);
    emit changed();
}
void ChartAlertFeed::setPriorityIdentities(const QStringList& priorities){d_->priorities=priorities;d_->priorities.removeDuplicates();}
void ChartAlertFeed::setCoinFlowSource(CoinFlowSource* source){
    if(d_->flow==source)return;if(d_->flow)disconnect(d_->flow,nullptr,this,nullptr);d_->flow=source;
    if(source)connect(source,&CoinFlowSource::changed,this,[this]{d_->flowChanged();});
}
void ChartAlertFeed::setStockSource(StockChartSource* source){
    if(d_->stock==source)return;d_->stopStock();if(d_->stock)disconnect(d_->stock,nullptr,this,nullptr);d_->stock=source;
    if(source){connect(source,&StockChartSource::changed,this,[this]{d_->stockChanged();});
        connect(source,&QObject::destroyed,this,[this]{const auto key=d_->stockCurrent;d_->stockCurrent.clear();d_->stock=nullptr;
            if(d_->lanes.contains(key)){auto& lane=d_->lanes[key];lane.begun=false;emit laneInvalidated(lane.demand.identity.key(),lane.demand.intervalMs,lane.generation);}});}
}
void ChartAlertFeed::setRunning(bool running){
    if(d_->running==running)return;d_->running=running;if(running){d_->timer->start();return;}
    d_->timer->stop();const auto previous=d_->lanes;
    for(auto i=d_->lanes.begin();i!=d_->lanes.end();++i){i->begun=false;i->flowMinutes.clear();i->flowSeeded=false;}
    d_->cancel();d_->stopStock();for(const auto& lane:previous)if(lane.begun)emit laneInvalidated(lane.demand.identity.key(),lane.demand.intervalMs,lane.generation);
    emit changed();
}
bool ChartAlertFeed::running()const{return d_->running;}
QString ChartAlertFeed::status()const{
    if(!d_->running)return tr("공개 알림 수집 중지");if(d_->lanes.isEmpty())return tr("켜진 공개 알림 대상 없음");
    return d_->message.isEmpty()?tr("완료 봉 확인 대기 · %1종목 · 늦은 이벤트는 보내지 않음").arg(d_->lanes.size()):d_->message;
}
QStringList ChartAlertFeed::scheduledIdentities()const{QStringList result;for(const auto& lane:d_->lanes)result.append(lane.demand.identity.key());result.removeDuplicates();return result;}
void ChartAlertFeed::pump(){
    if(!d_->running||!d_->transport||d_->pumping)return;QScopedValueRollback guard(d_->pumping,true);const auto now=d_->now();
    d_->validateMarkets(true);
    if(d_->token){if(now-d_->sentAt<15000)return;const auto key=d_->current;d_->token=0;d_->transport->abort();d_->current=key;d_->failCurrent(tr("공개 알림 원천 응답 시간 초과"));}
    if(!d_->stockCurrent.isEmpty()){
        d_->stockChanged();if(!d_->stockCurrent.isEmpty()){
            if(now-d_->sentAt<15*60000)return;
            const auto key=d_->stockCurrent;d_->stopStock();
            if(d_->lanes.contains(key)){auto& lane=d_->lanes[key];lane.due=after(now,60000);lane.begun=false;
                emit laneInvalidated(lane.demand.identity.key(),lane.demand.intervalMs,lane.generation);}return;
        }
    }
    if(!d_->running||now<d_->nextSend)return;
    QString selected;int bestPriority=std::numeric_limits<int>::max();qint64 earliest=std::numeric_limits<qint64>::max();
    for(auto i=d_->lanes.begin();i!=d_->lanes.end();++i){auto& lane=i.value();if(lane.due>now)continue;const auto& identity=lane.demand.identity;
        const bool stock=identity.market=="stock"&&d_->stock;
        if(!stock&&(identity.market!="coin"||(identity.source!="upbit"&&identity.source!="binance"))){d_->message=tr("이 원천의 실시간 알림 연결 대기");continue;}
        if(lane.marketBlocked)continue;
        int rank=d_->priorities.indexOf(identity.key());if(rank<0)rank=d_->priorities.size();
        if(selected.isEmpty()||rank<bestPriority||(rank==bestPriority&&lane.due<earliest)){selected=i.key();bestPriority=rank;earliest=lane.due;}
    }
    if(selected.isEmpty())return;auto& lane=d_->lanes[selected];
    if(lane.demand.identity.market=="stock"){
        d_->stockCurrent=selected;d_->sentAt=now;d_->nextSend=after(now,350);
        d_->stock->setSelection(lane.demand.identity,lane.demand.intervalMs,false);d_->stock->setActive(true);return;
    }
    const auto request=MarketCandles::request(lane.demand.identity.key(),lane.demand.intervalMs);
    if(!MarketCandles::isRequest(request)){d_->message=tr("공개 완료 봉 요청 범위 오류");return;}
    if(!lane.begun){lane.begun=true;lane.generation=d_->nextGeneration++;emit laneStarted(lane.demand.identity.key(),lane.demand.intervalMs,lane.generation,true);}
    if(!d_->running||!d_->lanes.contains(selected)||d_->marketAvailability(d_->lanes[selected].demand.identity,now)!=ChartMarketAvailability::Available)return;
    d_->current=selected;d_->sentAt=now;d_->nextSend=after(now,350);d_->token=d_->nextToken++;d_->transport->send(d_->token,request);
}
}
