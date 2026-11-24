#include "keefi/MarketCandles.h"
#include "keefi/CandleTradeOverlay.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/TradingTransport.h"
#include "keefi/StockChartTypes.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <limits>
namespace keefi {
namespace {
constexpr qint64 DayMs=86400000;
QString wireInterval(qint64 interval){
    switch(interval){case 60000:return "1m";case 300000:return "5m";case 900000:return "15m";
    case 3600000:return "1h";case 14400000:return "4h";case DayMs:return "1d";default:return {};}
}
struct Selection {
    QString identity,provider,code,base,quote;
    qint64 interval=0;
    bool valid=false;
};
Selection selection(const QString& identity,qint64 interval){
    Selection out;out.identity=identity;out.interval=interval;
    const auto pieces=identity.split(':');
    if(pieces.size()!=3||pieces[0]!="coin"||wireInterval(interval).isEmpty())return out;
    out.provider=pieces[1];out.code=pieces[2];
    if(out.provider=="upbit"){
        static const QRegularExpression market("\\A(KRW|BTC|USDT)-([A-Z0-9]{1,20})\\z");
        const auto match=market.match(out.code);if(!match.hasMatch())return out;
        out.quote=match.captured(1);out.base=match.captured(2);
    }else if(out.provider!="binance"||!binanceIdentifierValid(out.code,2))return out;
    out.valid=true;return out;
}
bool validInstrument(const Selection& selected,const MarketInstrument& item){
    return selected.provider=="binance"&&item.symbol==selected.code&&item.market=="spot"&&item.status=="TRADING"&&item.tradable&&
        binanceIdentifierValid(item.baseAsset)&&binanceIdentifierValid(item.quoteAsset)&&item.symbol==item.baseAsset+item.quoteAsset;
}
bool integer(const QJsonValue& value,qint64* output){
    static const QRegularExpression exact("\\A[0-9]{1,16}\\z");
    const auto text=value.toString();bool ok=false;
    if(!exact.match(text).hasMatch())return false;
    const auto number=text.toLongLong(&ok);if(!ok)return false;*output=number;return true;
}
bool tradeIdentifier(const QJsonValue& value,qint64* output){
    static const QRegularExpression exact("\\A[0-9]{1,19}\\z");
    const auto text=value.toString();bool ok=false;
    if(!exact.match(text).hasMatch())return false;
    *output=text.toLongLong(&ok);return ok;
}
bool number(const QString& value,bool positive=false){
    static const QRegularExpression exact("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");
    return exact.match(value).hasMatch()&&(!positive||decimal::positive(value));
}
bool validRow(MarketCandle* row){
    if(!number(row->open,true)||!number(row->high,true)||!number(row->low,true)||!number(row->close,true)||!number(row->volume))return false;
    row->open=decimal::normalize(row->open);row->high=decimal::normalize(row->high);row->low=decimal::normalize(row->low);
    row->close=decimal::normalize(row->close);row->volume=decimal::normalize(row->volume);
    return decimal::compare(row->high,row->open)>=0&&decimal::compare(row->high,row->close)>=0&&
        decimal::compare(row->low,row->open)<=0&&decimal::compare(row->low,row->close)<=0;
}
MarketCandleSeries emptySeries(const Selection& selected,const MarketInstrument& item={}){
    MarketCandleSeries out;out.identity=selected.identity;out.intervalMs=selected.interval;
    out.asset=selected.base;out.currency=selected.quote;
    if(validInstrument(selected,item)){out.asset=item.baseAsset;out.currency=item.quoteAsset;}
    out.sourceUrl=MarketCandles::request(selected.identity,selected.interval).url.toString();
    out.reason="캔들 원천 자료 미수신 또는 검증 실패";return out;
}
QList<MarketCandle> mergeCompletedRows(const QList<MarketCandle>& previous,const QList<MarketCandle>& incoming){
    const auto orderedUnique=[](const QList<MarketCandle>& rows){
        return std::adjacent_find(rows.cbegin(),rows.cend(),[](const auto& a,const auto& b){
            return a.startsAtMs>=b.startsAtMs;})==rows.cend();
    };
    if(!orderedUnique(previous)||!orderedUnique(incoming)){
        // Archive offers may not share the native parser's ordering contract.
        // Preserve the former QMap semantics: last row within each input wins,
        // then the incoming input wins over the previous history at that time.
        QMap<qint64,MarketCandle> normalized;
        for(const auto& row:previous)normalized.insert(row.startsAtMs,row);
        for(const auto& row:incoming)normalized.insert(row.startsAtMs,row);
        return normalized.values();
    }
    QList<MarketCandle> merged;merged.reserve(previous.size()+incoming.size());
    auto old=previous.cbegin(),next=incoming.cbegin();
    while(old!=previous.cend()||next!=incoming.cend()){
        if(next==incoming.cend()||(old!=previous.cend()&&old->startsAtMs<next->startsAtMs))merged.append(*old++);
        else{
            if(old!=previous.cend()&&old->startsAtMs==next->startsAtMs)++old;
            merged.append(*next++);
        }
    }
    return merged;
}
}
struct MarketCandles::Private {
    MarketCandles* q=nullptr;
    QPointer<TradingTransport> transport;
    std::unique_ptr<PublicCandleSocket> socket;
    Clock clock;
    QMap<QString,QList<MarketInstrument>> instruments;
    Selection selected;
    MarketCandleSeries data, lastCompleted, lastRest;
    QList<MarketCandle> historyRows;
    mutable QList<MarketCandle> archiveDisplayHistory,archiveDisplayCurrent,archiveDisplayMerged;
    CandleHistoryState historyState=CandleHistoryState::Unavailable;
    QString historyMessage;
    qint64 historyBefore=0, historyRetryAt=0;
    bool historyPending=false, requestIsHistory=false;
    QList<MarketCandleSeries> displayCache; // LRU, at most eight exact native selections.
    struct CachedHistory { QList<MarketCandle> rows; CandleHistoryState state=CandleHistoryState::Ready; QString message; };
    QMap<QString,CachedHistory> displayHistoryCache; // Across the LRU: at most HistoryLimit rows.
    CandleTradeOverlay liveTrades;
    QMap<QString,qint64> providerRetryAt;
    CandleArchiveStatus archiveStatus;
    QTimer refresh,pacing,health,futureTimer,liveDelivery;
    QByteArray futureMessage;
    quint64 futureGeneration=0;
    qint64 futureEvent=0,futureReceivedAt=0;
    QElapsedTimer futureAge;
    int futureRetries=0;
    quint64 token=0,nextToken=1,wsGeneration=0,wsArrival=0,requestWsArrival=0;
    qint64 requestStartedAt=0,nextRestAt=0,reconnectAt=0,wsSourceTime=0;
    int reconnectAttempts=0;
    bool pending=false,seeded=false,connected=false;
    bool guardUpbit=false,upbitAvailable=false;
    QSet<QString> upbitMarkets,unsupportedIdentities;
    std::optional<MarketCandle> wsForming;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    MarketInstrument instrument()const{
        MarketInstrument result;int matches=0;
        for(const auto& item:instruments.value(selected.provider))if(item.symbol==selected.code){result=item;++matches;}
        return matches==1&&validInstrument(selected,result)?result:MarketInstrument{};
    }
    bool eligible()const{return selected.valid&&!unsupportedIdentities.contains(selected.identity)&&
        (selected.provider=="upbit"?(!guardUpbit||(upbitAvailable&&upbitMarkets.contains(selected.code))):validInstrument(selected,instrument()));}
    bool canDisplayCompleted()const{return eligible() || (selected.valid&&selected.provider=="upbit"&&guardUpbit&&
        !upbitAvailable&&upbitMarkets.contains(selected.code)&&!unsupportedIdentities.contains(selected.identity)) ||
        (selected.valid&&selected.provider=="binance"&&instruments.value("binance").isEmpty());}
    QString ineligibleReason()const{
        if(unsupportedIdentities.contains(selected.identity))return QObject::tr("지원하지 않는 마켓이에요.");
        if(selected.provider=="upbit"&&guardUpbit)return upbitAvailable?
            QObject::tr("업비트에서 지원하지 않는 마켓이에요."):QObject::tr("업비트 종목 목록을 확인하고 있어요.");
        return selected.valid?QObject::tr("공개 종목 메타데이터 확인 대기"):QObject::tr("지원하지 않는 캔들 identity 또는 시간봉");
    }
    bool reusable(const MarketCandleSeries& value,qint64 at)const{
        if(value.candles.isEmpty()||value.candles.size()>200||value.intervalMs<=0||
            value.receivedAtMs<=0||value.receivedAtMs>at)return false;
        const auto key=selection(value.identity,value.intervalMs);
        if(!key.valid||value.sourceUrl!=MarketCandles::request(key.identity,key.interval).url.toString())return false;
        qint64 previous=0;
        for(const auto& row:value.candles){
            if(row.startsAtMs<=previous||row.startsAtMs%value.intervalMs||
                row.startsAtMs>at-value.intervalMs||row.lastStoredTickMs>at)return false;
            previous=row.startsAtMs;
        }
        const auto end=previous+value.intervalMs;
        return at-end<=qMax<qint64>(DayMs,3*value.intervalMs);
    }
    bool matchesContract(const MarketCandleSeries& value)const{
        if(value.identity!=selected.identity||value.intervalMs!=selected.interval||!canDisplayCompleted())return false;
        if(selected.provider=="upbit")return value.asset==selected.base&&value.currency==selected.quote;
        const auto item=instrument();
        return instruments.value("binance").isEmpty()||
            (validInstrument(selected,item)&&value.asset==item.baseAsset&&value.currency==item.quoteAsset);
    }
    static QString cacheKey(const QString& identity,qint64 interval){return identity+'\n'+QString::number(interval);}
    void pruneHistoryCache(){
        QSet<QString> valid;for(const auto& row:displayCache)valid.insert(cacheKey(row.identity,row.intervalMs));
        for(auto it=displayHistoryCache.begin();it!=displayHistoryCache.end();)
            if(!valid.contains(it.key()))it=displayHistoryCache.erase(it);else ++it;
        qsizetype remaining=MarketCandles::HistoryLimit;
        for(auto it=displayCache.crbegin();it!=displayCache.crend();++it){
            auto history=displayHistoryCache.find(cacheKey(it->identity,it->intervalMs));
            if(history==displayHistoryCache.end())continue;
            if(history->rows.size()>remaining){
                history->rows=history->rows.sliced(history->rows.size()-remaining);
                history->state=CandleHistoryState::Ready;history->message.clear();
            }
            remaining-=history->rows.size();
        }
    }
    void rememberHistory(){
        if(historyRows.isEmpty())return;
        const auto key=cacheKey(selected.identity,selected.interval);
        const bool cached=std::any_of(displayCache.cbegin(),displayCache.cend(),[&](const auto& row){
            return cacheKey(row.identity,row.intervalMs)==key;});
        if(!cached)return; // Archive-only and lock-retained observations never enter this cache.
        const bool terminal=historyState==CandleHistoryState::End||historyState==CandleHistoryState::Limit;
        displayHistoryCache.insert(key,{historyRows,terminal?historyState:CandleHistoryState::Ready,terminal?historyMessage:QString{}});
        pruneHistoryCache();
    }
    void forgetCached(const QString& identity){
        displayCache.removeIf([&](const auto& row){return row.identity==identity;});
        pruneHistoryCache();
    }
    void restoreCompleted(){
        const auto at=now();
        displayCache.removeIf([&](const auto& row){return !reusable(row,at);});
        pruneHistoryCache();
        for(qsizetype i=0;i<displayCache.size();++i){
            if(!matchesContract(displayCache[i]))continue;
            auto cached=displayCache.takeAt(i);cached.available=false;cached.delayed=true;
            cached.forming.reset();cached.fromMemoryCache=true;
            lastCompleted=cached;displayCache.append(std::move(cached));
            const auto found=displayHistoryCache.constFind(cacheKey(selected.identity,selected.interval));
            if(found!=displayHistoryCache.cend()&&!found->rows.isEmpty()){
                historyRows=found->rows;historyState=found->state;historyMessage=found->message;
                mergeHistory(lastCompleted.candles);
            }
            return;
        }
    }
    void rememberCompleted(){
        if(!data.available||data.candles.isEmpty())return;
        clearArchiveDisplay();
        lastCompleted=data;lastCompleted.forming.reset();
        if(!reusable(lastCompleted,now()))return;
        displayCache.removeIf([&](const auto& row){return row.identity==data.identity&&row.intervalMs==data.intervalMs;});
        displayCache.append(lastCompleted);
        while(displayCache.size()>8)displayCache.removeFirst();
        pruneHistoryCache();
    }
    void mergeHistory(const QList<MarketCandle>& rows){
        // The normal parser-produced inputs take the linear path. Rendering
        // shares this immutable result rather than repeating the merge.
        auto merged=mergeCompletedRows(historyRows,rows);
        historyRows=merged.size()>MarketCandles::HistoryLimit?merged.sliced(merged.size()-MarketCandles::HistoryLimit):std::move(merged);
        if(historyState==CandleHistoryState::Unavailable){historyState=CandleHistoryState::Ready;historyMessage.clear();}
    }
    void clearArchiveDisplay(){archiveDisplayHistory.clear();archiveDisplayCurrent.clear();archiveDisplayMerged.clear();}
    void clearHistory(){historyRows.clear();historyPending=false;requestIsHistory=false;
        historyBefore=historyRetryAt=0;historyState=CandleHistoryState::Unavailable;historyMessage.clear();clearArchiveDisplay();}
    void setHistoryState(CandleHistoryState state,const QString& message){historyState=state;historyMessage=message;emit q->historyChanged();}
    void clearFuture(){futureTimer.stop();futureMessage.clear();futureGeneration=0;futureEvent=futureReceivedAt=0;futureRetries=0;futureAge.invalidate();}
    void deferFuture(quint64 generation,const QByteArray& message,qint64 event,qint64 received){
        // A small exchange/client clock lead is not permission to accept future
        // evidence. Keep only the latest packet until the unchanged clock guard
        // succeeds. Both its exchange timestamp and original receipt are kept.
        if(!futureMessage.isEmpty()&&event<futureEvent)return;
        futureMessage=message;futureGeneration=generation;futureEvent=event;futureReceivedAt=received;
        futureRetries=0;futureAge.start();futureTimer.start(int(qBound<qint64>(qint64(1),event-now(),qint64(1000))));
    }
    void deliverFuture(){
        if(futureMessage.isEmpty())return;
        const auto at=now();
        if(futureGeneration!=wsGeneration||!connected||!eligible()||futureAge.elapsed()>1500||
            ++futureRetries>8||futureEvent-at>1000){clearFuture();return;}
        if(futureEvent>at){futureTimer.start(int(qBound<qint64>(qint64(1),futureEvent-at,qint64(1000))));return;}
        const auto generation=futureGeneration;const auto received=futureReceivedAt;const auto message=futureMessage;
        clearFuture();receive(generation,message,received);
    }
    void cancel(){
        liveDelivery.stop();liveTrades.reset(selected.identity,selected.provider,selected.code,selected.interval);
        clearFuture();const bool active=token!=0;token=0;++wsGeneration;pending=seeded=connected=false;
        lastRest={};historyPending=false;requestIsHistory=false;historyBefore=0;
        if(historyState==CandleHistoryState::Loading)historyState=CandleHistoryState::Ready;
        nextRestAt=reconnectAt=wsSourceTime=0;wsArrival=requestWsArrival=0;wsForming.reset();
        refresh.stop();pacing.stop();health.stop();
        if(active&&transport)transport->abort();if(socket)socket->abort();
    }
    void next(){
        if(!transport||token||(!pending&&!historyPending)||!eligible())return;
        const auto at=now();
        const auto deadline=qMax(nextRestAt,providerRetryAt.value(selected.provider));
        if(at<deadline){pacing.start(int(qMin<qint64>(1000,deadline-at)));return;}
        requestIsHistory=!pending&&historyPending;
        if(requestIsHistory){historyPending=false;historyBefore=historyRows.isEmpty()?0:historyRows.first().startsAtMs;
            if(!historyBefore)return;
        }else pending=false;
        token=nextToken++;requestStartedAt=at;requestWsArrival=wsArrival;
        nextRestAt=at+1000;pacing.start(1000);
        const auto request=requestIsHistory?MarketCandles::historyRequest(selected.identity,selected.interval,historyBefore):
            MarketCandles::request(selected.identity,selected.interval);
        if(request.url.isEmpty()){
            token=0;requestIsHistory=false;historyPending=false;
            setHistoryState(CandleHistoryState::Delayed,QObject::tr("과거 조회 기준 시각을 확인할 수 없어요"));return;
        }
        transport->send(token,request);
    }
    void start(bool retainFreshPublic=false){
        if(!retainFreshPublic)data=emptySeries(selected,instrument());
        if(!eligible()){data.reason=ineligibleReason();emit q->changed();return;}
        if(!retainFreshPublic)data.reason=providerRetryAt.value(selected.provider)>now()?"원천 요청 제한":"캔들 조회 대기";
        refresh.start(60000);health.start(200);pending=true;next();
        if(socket&&eligible()){
            const auto url=selected.provider=="upbit"?QUrl("wss://api.upbit.com/websocket/v1"):
                QUrl("wss://stream.binance.com:9443/ws/"+selected.code.toLower()+"@kline_"+wireInterval(selected.interval));
            socket->open(wsGeneration,url);
        }
        emit q->changed();
    }
    void fail(quint64 generation){
        if(generation!=wsGeneration||!eligible())return;
        cancel();data=emptySeries(selected,instrument());data.reason="공개 캔들 연결 끊김 · 재연결 후 REST 재확인 대기";
        const auto delay=qMin(60000,1000*(1<<qMin(reconnectAttempts++,6)));
        reconnectAt=now()+delay;health.start(200);emit q->changed();
    }
    void pulse(){
        if(!eligible())return;const auto at=now();
        if(reconnectAt){if(at>=reconnectAt){reconnectAt=0;start();}return;}
        if(token&&(at<requestStartedAt||at-requestStartedAt>=15000)){
            token=0;if(transport)transport->abort();
            if(requestIsHistory){requestIsHistory=false;historyRetryAt=at+1000;
                setHistoryState(CandleHistoryState::Delayed,QObject::tr("과거 봉 응답 지연 · 다시 시도할 수 있어요"));next();return;}
            seeded=false;wsForming.reset();
            lastRest={};
            data=emptySeries(selected,instrument());data.reason="캔들 REST 응답 시간 초과 · 재확인 대기";
            pending=true;nextRestAt=at+1000;emit q->changed();
        }
        next();
    }
    void ready(quint64 generation){
        if(generation!=wsGeneration||!eligible()||!socket)return;
        connected=true;
        if(selected.provider=="upbit"){
            const auto interval=selected.interval==DayMs?60000:selected.interval;
            const auto type="candle."+QString::number(interval/60000)+"m";
            socket->send(wsGeneration,QJsonDocument(QJsonArray{QJsonObject{{"ticket",QString::number(wsGeneration)}},
                QJsonObject{{"type",type},{"codes",QJsonArray{selected.code}}},
                QJsonObject{{"type","trade"},{"codes",QJsonArray{selected.code}},{"is_only_realtime",true}},
                QJsonObject{{"format","DEFAULT"}}}).toJson(QJsonDocument::Compact));
        }else if(selected.provider=="binance"){
            socket->send(wsGeneration,QJsonDocument(QJsonObject{{"method","SUBSCRIBE"},
                {"params",QJsonArray{selected.code.toLower()+"@trade"}},{"id",QString::number(wsGeneration)}}).toJson(QJsonDocument::Compact));
        }
    }
    void retainForming(const MarketCandle& row,qint64 source,qint64 received=0){
        if(!data.candles.isEmpty()&&row.startsAtMs<=data.candles.last().startsAtMs)return;
        if(wsForming&&row.startsAtMs<wsForming->startsAtMs)return;
        if(data.forming&&data.forming->startsAtMs!=row.startsAtMs)pending=true;
        wsForming=row;wsSourceTime=source;++wsArrival;
        if(!seeded)return;
        data.forming=row;data.receivedAtMs=received>0?received:now();data.sourceTimeMs=source;
        data.available=!data.candles.isEmpty();reconnectAttempts=0;emit q->changed();next();
    }
    void receive(quint64 generation,const QByteArray& message,qint64 originalReceipt=0){
        if(generation!=wsGeneration||!connected||!eligible()||message.size()>65536)return;
        const auto doc=parseExactJson(message);if(!doc.isObject())return;const auto object=doc.object();const auto at=now();
        const auto received=originalReceipt>0?originalReceipt:at;
        if(object.value("e").toString()=="trade"||object.value("type").toString()=="trade"){
            if(liveTrades.receive(object,received)&&!liveDelivery.isActive())liveDelivery.start(33);
            return;
        }
        if(selected.provider=="upbit"){
            const auto interval=selected.interval==DayMs?60000:selected.interval;
            if(object.value("code").toString()!=selected.code||object.value("type").toString()!=
                "candle."+QString::number(interval/60000)+"m"||
                (object.value("stream_type").toString()!="SNAPSHOT"&&object.value("stream_type").toString()!="REALTIME"))return;
            auto raw=object;raw["market"]=selected.code;raw["unit"]=QString::number(interval/60000);
            HttpResult result;result.status=200;result.body=QJsonDocument(QJsonArray{raw}).toJson(QJsonDocument::Compact);
            const auto parsed=MarketCandles::parse(selected.identity,interval,result,at,at);
            const auto row=parsed.forming?parsed.forming:parsed.candles.isEmpty()?std::optional<MarketCandle>{}:std::optional<MarketCandle>{parsed.candles.last()};
            if(!row)return;
            if(selected.interval==DayMs){pending=true;next();return;}
            retainForming(*row,row->lastStoredTickMs);return;
        }
        if(object.value("e").toString()!="kline"||object.value("s").toString()!=selected.code||!object.value("k").isObject())return;
        const auto kline=object.value("k").toObject();qint64 event=0;
        if(kline.value("s").toString()!=selected.code||kline.value("i").toString()!=wireInterval(selected.interval)||
            !kline.value("x").isBool()||!integer(object.value("E"),&event)||event<wsSourceTime)return;
        if(event>at){if(event-at<=1000)deferFuture(generation,message,event,received);return;}
        if(!futureMessage.isEmpty()&&event>=futureEvent)clearFuture();
        const QJsonArray tuple{kline.value("t"),kline.value("o"),kline.value("h"),kline.value("l"),kline.value("c"),
            kline.value("v"),kline.value("T"),kline.value("q"),kline.value("n"),kline.value("V"),kline.value("Q"),kline.value("B")};
        QJsonArray rows;rows.append(tuple);HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
        const auto parsed=MarketCandles::parse(selected.identity,selected.interval,response,at,at,instrument());
        const auto row=parsed.forming?parsed.forming:parsed.candles.isEmpty()?std::optional<MarketCandle>{}:std::optional<MarketCandle>{parsed.candles.last()};
        if(!row||event<row->startsAtMs)return;
        qint64 lastTradeId=0;
        if(tradeIdentifier(kline.value("L"),&lastTradeId))liveTrades.setBinanceAnchor(*row,lastTradeId,event);
        else liveTrades.clearBinanceAnchor();
        if(!kline.value("x").toBool()){retainForming(*row,event,received);return;}
        if(!seeded||event<row->startsAtMs+selected.interval-1)return;
        if(!data.candles.isEmpty()&&row->startsAtMs<data.candles.last().startsAtMs)return;
        if(!data.candles.isEmpty()&&row->startsAtMs==data.candles.last().startsAtMs)data.candles.last()=*row;
        else data.candles.append(*row);
        while(data.candles.size()>200)data.candles.removeFirst();
        if(data.forming&&data.forming->startsAtMs<=row->startsAtMs)data.forming.reset();
        if(wsForming&&wsForming->startsAtMs<=row->startsAtMs)wsForming.reset();
        ++wsArrival;wsSourceTime=event;data.sourceTimeMs=event;data.receivedAtMs=received;data.available=true;data.reason.clear();
        rememberCompleted();mergeHistory(data.candles);
        reconnectAttempts=0;emit q->changed();
    }
};
MarketCandles::MarketCandles(TradingTransport* transport,QObject* parent):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->pacing.setSingleShot(true);
    d_->futureTimer.setSingleShot(true);d_->futureTimer.setTimerType(Qt::PreciseTimer);
    d_->liveDelivery.setSingleShot(true);d_->liveDelivery.setTimerType(Qt::PreciseTimer);
    connect(&d_->liveDelivery,&QTimer::timeout,this,[this]{emit liveChanged();});
    connect(&d_->futureTimer,&QTimer::timeout,this,[this]{d_->deliverFuture();});
    connect(&d_->refresh,&QTimer::timeout,this,[this]{d_->pending=true;d_->next();});
    connect(&d_->pacing,&QTimer::timeout,this,[this]{d_->next();});
    connect(&d_->health,&QTimer::timeout,this,[this]{d_->pulse();});
    if(transport)connect(transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& response){
        if(!token||token!=d_->token)return;d_->token=0;
        const auto at=d_->now();
        const auto error=parseExactJson(response.body).object().value("error").toObject();
        if(d_->selected.provider=="upbit"&&response.status==404&&error.value("message").toString()=="Code not found"){
            d_->unsupportedIdentities.insert(d_->selected.identity);d_->cancel();d_->clearHistory();d_->lastCompleted={};
            d_->forgetCached(d_->selected.identity);
            d_->data=emptySeries(d_->selected,d_->instrument());d_->data.reason=d_->ineligibleReason();emit changed();return;
        }
        const bool history=d_->requestIsHistory;d_->requestIsHistory=false;
        auto parsed=parse(d_->selected.identity,d_->selected.interval,response,at,d_->requestStartedAt,d_->instrument());
        if(history){
            const auto json=parseExactJson(response.body);
            const bool responseOk=response.status==200&&!response.networkError&&!response.timedOut&&response.localRetryAfterSeconds<=0;
            const bool ended=responseOk&&json.isArray()&&json.array().isEmpty();
            const bool valid=parsed.available&&!parsed.forming&&std::all_of(parsed.candles.cbegin(),parsed.candles.cend(),
                [&](const auto& row){return row.startsAtMs<d_->historyBefore&&row.startsAtMs+parsed.intervalMs<=d_->historyBefore;});
            if(valid){
                const auto oldFirst=d_->historyRows.isEmpty()?qint64(0):d_->historyRows.first().startsAtMs;
                d_->mergeHistory(parsed.candles);d_->historyRetryAt=0;
                d_->historyState=d_->historyRows.size()>=HistoryLimit?CandleHistoryState::Limit:
                    (oldFirst&&d_->historyRows.first().startsAtMs>=oldFirst?CandleHistoryState::End:CandleHistoryState::Ready);
                d_->historyMessage=d_->historyState==CandleHistoryState::Limit?tr("이번 탐색의 과거 보관 한도 %1봉").arg(HistoryLimit):
                    d_->historyState==CandleHistoryState::End?tr("확인 가능한 과거 데이터 끝"):QString{};
            }else if(ended){d_->historyState=CandleHistoryState::End;d_->historyMessage=tr("확인 가능한 과거 데이터 끝");}
            else{d_->historyState=CandleHistoryState::Delayed;d_->historyRetryAt=at+1000;d_->historyMessage=tr("과거 봉을 받지 못했어요 · 다시 시도");}
        }else{
        // Snapshot before any stream merge. Qt's copy-on-write containers keep this
        // receipt unchanged when the live chart replaces the same completed candle.
        parsed.restRevision=token;
        d_->lastRest=parsed.available?parsed:MarketCandleSeries{};
        d_->lastRest.forming.reset();
        d_->seeded=parsed.available||parsed.forming.has_value();
        if(d_->seeded&&d_->wsForming&&d_->wsArrival>d_->requestWsArrival&&
            (parsed.candles.isEmpty()||d_->wsForming->startsAtMs>parsed.candles.last().startsAtMs)&&
            (!parsed.forming||d_->wsForming->startsAtMs>=parsed.forming->startsAtMs)){
            parsed.forming=d_->wsForming;parsed.sourceTimeMs=d_->wsSourceTime;
        }
        d_->data=parsed;
        d_->rememberCompleted();if(d_->data.available)d_->mergeHistory(d_->data.candles);
        }
        if(response.status==429||response.status==418||response.localRetryAfterSeconds>0){
            qint64 delay=qMax<qint64>(1000,qint64(response.localRetryAfterSeconds)*1000);
            for(auto it=response.headers.cbegin();it!=response.headers.cend();++it){
                if(it.key().compare("Retry-After",Qt::CaseInsensitive)!=0)continue;
                bool ok=false;const auto seconds=it.value().toLongLong(&ok);
                if(ok&&seconds>0)delay=qMax(delay,seconds>std::numeric_limits<qint64>::max()/1000?
                    std::numeric_limits<qint64>::max():seconds*1000);
                else{
                    const auto text=QString::fromLatin1(it.value());
                    auto date=QDateTime::fromString(text,Qt::RFC2822Date);
                    // Qt's RFC2822 parser rejects the literal GMT used by HTTP's IMF-fixdate.
                    static const QRegularExpression httpDate("\\A(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} "
                        "(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT\\z");
                    if(httpDate.match(text).hasMatch()){
                        const auto parsed=QLocale::c().toDateTime(text,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");
                        if(parsed.isValid())date=QDateTime(parsed.date(),parsed.time(),QTimeZone("UTC"));
                    }
                    if(date.isValid()&&date.toMSecsSinceEpoch()>at)delay=qMax(delay,date.toMSecsSinceEpoch()-at);
                }
            }
            d_->nextRestAt=delay>std::numeric_limits<qint64>::max()-at?std::numeric_limits<qint64>::max():at+delay;
            d_->providerRetryAt[d_->selected.provider]=qMax(d_->providerRetryAt.value(d_->selected.provider),d_->nextRestAt);
            if(history){d_->historyRetryAt=d_->nextRestAt;d_->historyState=CandleHistoryState::RateLimited;
                d_->historyMessage=tr("과거 봉 요청 제한 · 재시도 가능 시각을 기다려 주세요");}
            else d_->pending=true;
        }
        if(history)emit historyChanged();else emit changed();d_->next();
    });
}
MarketCandles::MarketCandles(TradingTransport* transport,std::unique_ptr<PublicCandleSocket> socket,QObject* parent)
    :MarketCandles(transport,std::move(socket),Clock{},parent){}
MarketCandles::MarketCandles(TradingTransport* transport,std::unique_ptr<PublicCandleSocket> socket,Clock clock,QObject* parent)
    :MarketCandles(transport,parent){
    d_->socket=std::move(socket);d_->clock=std::move(clock);
    if(d_->socket){
        connect(d_->socket.get(),&PublicCandleSocket::connected,this,[this](quint64 epoch){d_->ready(epoch);});
        connect(d_->socket.get(),&PublicCandleSocket::received,this,[this](quint64 epoch,const QByteArray& data){d_->receive(epoch,data);});
        connect(d_->socket.get(),&PublicCandleSocket::failed,this,[this](quint64 epoch,int){d_->fail(epoch);});
    }
}
MarketCandles::~MarketCandles(){d_->cancel();}
void MarketCandles::setAsset(const QString& asset){
    static const QRegularExpression valid("\\A[A-Z0-9]{1,20}\\z");
    setSelection(valid.match(asset).hasMatch()?"coin:upbit:KRW-"+asset:QString{},IntervalMs);
}
void MarketCandles::setSelection(const QString& identity,qint64 interval){
    if(identity==d_->selected.identity&&interval==d_->selected.interval)return;
    d_->rememberHistory();d_->cancel();d_->clearHistory();d_->lastCompleted={};d_->reconnectAttempts=0;d_->selected=selection(identity,interval);
    d_->liveTrades.reset(d_->selected.identity,d_->selected.provider,d_->selected.code,d_->selected.interval);
    if(identity.isEmpty()){d_->data={};d_->data.intervalMs=interval;emit changed();return;}
    d_->restoreCompleted();
    d_->start();
}
void MarketCandles::setVerifiedUpbitMarkets(const QStringList& values,bool available){
    QSet<QString> next;
    static const QRegularExpression pattern("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,20}\\z");
    for(const auto& code:values)if(pattern.match(code).hasMatch())next.insert(code);
    const bool wasEligible=d_->eligible();const bool priorReady=d_->upbitAvailable;
    if(available){
        // A 404 is terminal for this metadata membership. Only an official
        // removal followed by re-addition permits requesting that pair again.
        for(const auto& code:next)if(!d_->upbitMarkets.contains(code))d_->unsupportedIdentities.remove("coin:upbit:"+code);
        d_->upbitMarkets=next;
        d_->displayCache.removeIf([&](const auto& row){const auto key=selection(row.identity,row.intervalMs);
            return key.provider=="upbit"&&!next.contains(key.code);});
        d_->pruneHistoryCache();
    }
    d_->guardUpbit=true;d_->upbitAvailable=available;
    if(d_->selected.provider=="upbit" && (wasEligible!=d_->eligible() || priorReady!=available)){
        d_->cancel();if(!d_->canDisplayCompleted()){d_->lastCompleted={};d_->clearHistory();}d_->start();
    }
}
void MarketCandles::setInstruments(const QString& provider,const QList<MarketInstrument>& values){
    if(provider!="binance" || d_->instruments.value(provider)==values)return;
    const auto before=d_->instrument();const bool wasUnavailable=d_->instruments.value(provider).isEmpty();d_->instruments[provider]=values;
    if(!values.isEmpty())d_->displayCache.removeIf([&](const auto& row){
        const auto key=selection(row.identity,row.intervalMs);if(key.provider!=provider)return false;
        int matches=0;MarketInstrument match;
        for(const auto& item:values)if(item.symbol==key.code){match=item;++matches;}
        return matches!=1||!validInstrument(key,match)||row.asset!=match.baseAsset||row.currency!=match.quoteAsset;
    });
    d_->pruneHistoryCache();
    if(d_->selected.provider==provider&&(before!=d_->instrument()||wasUnavailable!=values.isEmpty())){
        d_->cancel();if(!d_->matchesContract(d_->lastCompleted)){d_->lastCompleted={};d_->clearHistory();}d_->start();
    }
}
MarketCandleSeries MarketCandles::series(qint64 at)const{
    if(!at)at=d_->now();auto result=d_->data;
    if((result.available||result.forming)&&(result.receivedAtMs>at||at-result.receivedAtMs>FreshnessMs)){
        // Future receipt times are invalid, not an older public observation.
        result.delayed=result.receivedAtMs>0&&result.receivedAtMs<=at;
        if(result.delayed&&!result.candles.isEmpty())result.staleClose=result.candles.last().close;
        result.available=false;result.candles.clear();result.forming.reset();result.reason="최신 캔들 자료 부족";
    }
    return result;
}
MarketCandleSeries MarketCandles::restObservation()const{
    if(!d_->eligible()||d_->lastRest.identity!=d_->selected.identity||
        d_->lastRest.intervalMs!=d_->selected.interval||d_->lastRest.restReceivedAtMs>d_->now())return {};
    return d_->lastRest;
}
CandleArchiveStatus MarketCandles::archiveStatus()const{return d_->archiveStatus;}
void MarketCandles::setArchiveStatus(const CandleArchiveStatus& status){
    if(d_->archiveStatus==status)return;
    d_->archiveStatus=status;emit archiveStatusChanged();
}
void MarketCandles::retryArchive(){if(d_->archiveStatus.canRetry)emit archiveRetryRequested();}
MarketCandleSeries MarketCandles::displayObservation(qint64 at)const{
    if(!at)at=d_->now();
    const auto current=series(at);
    const auto withHistory=[&](MarketCandleSeries value){
        if(!d_->historyRows.isEmpty()&&value.identity==d_->selected.identity&&value.intervalMs==d_->selected.interval){
            if(value.fromArchive){
                // A replacement archive offer has display precedence without
                // changing the history paging cursor/status. Retained COW keys
                // make repeated reads O(1) and invalidate on either input edit.
                const auto same=[](const auto& a,const auto& b){return a.size()==b.size()&&a.constData()==b.constData();};
                if(!same(d_->archiveDisplayHistory,d_->historyRows)||!same(d_->archiveDisplayCurrent,value.candles)){
                    d_->archiveDisplayMerged=mergeCompletedRows(d_->historyRows,value.candles);
                    d_->archiveDisplayHistory=d_->historyRows;d_->archiveDisplayCurrent=value.candles;
                }
                value.candles=d_->archiveDisplayMerged;
            }else value.candles=d_->historyRows;
        }
        return value;
    };
    if(current.available){
        auto displayed=withHistory(current);
        if(d_->connected&&d_->seeded){
            const auto forming=d_->liveTrades.forming(at);
            if(forming&&(displayed.candles.isEmpty()||forming->startsAtMs>displayed.candles.last().startsAtMs))displayed.forming=forming;
        }
        return displayed;
    }
    auto previous=d_->lastCompleted;
    if(!d_->canDisplayCompleted()||previous.candles.isEmpty()||previous.identity!=d_->selected.identity||
        previous.intervalMs!=d_->selected.interval||previous.receivedAtMs<=0||previous.receivedAtMs>at)
        return current;
    const auto end=previous.candles.last().startsAtMs+previous.intervalMs;
    if(end>at||at-end>qMax<qint64>(DayMs,3*previous.intervalMs))return current;
    if(previous.fromMemoryCache&&(!d_->reusable(previous,at)||!d_->matchesContract(previous)))return current;
    previous.available=false;previous.delayed=true;previous.forming.reset();
    previous.staleClose=previous.candles.last().close;previous.reason=current.reason;
    return withHistory(previous);
}
MarketCandleLiveTrade MarketCandles::liveTradeObservation(qint64 at)const{
    if(!at)at=d_->now();
    if(!d_->eligible()||!d_->connected||!d_->seeded)return {};
    const auto official=series(at);
    if(!official.available&&!official.forming)return {};
    auto trade=d_->liveTrades.observation(at);
    if(d_->selected.provider=="upbit"&&trade.tradeAtMs<=official.sourceTimeMs)return {};
    return trade;
}
void MarketCandles::requestOlder(){
    if(!d_->eligible()||d_->historyPending||d_->requestIsHistory||
        d_->historyState==CandleHistoryState::End||d_->historyState==CandleHistoryState::Limit)return;
    if(d_->historyRows.isEmpty()){
        const auto shown=displayObservation();if(shown.identity!=d_->selected.identity||shown.candles.isEmpty())return;
        d_->mergeHistory(shown.candles);
    }
    if(d_->historyRows.size()>=HistoryLimit){d_->setHistoryState(CandleHistoryState::Limit,tr("이번 탐색의 과거 보관 한도 %1봉").arg(HistoryLimit));return;}
    const auto at=d_->now();
    const auto retry=qMax(d_->historyRetryAt,d_->providerRetryAt.value(d_->selected.provider));
    if(at<retry){d_->setHistoryState(CandleHistoryState::RateLimited,tr("과거 봉 재시도 대기"));return;}
    d_->historyPending=true;d_->historyRetryAt=0;d_->setHistoryState(CandleHistoryState::Loading,tr("이전 봉을 불러오는 중"));d_->next();
}
CandleHistoryStatus MarketCandles::historyStatus()const{
    CandleHistoryStatus out;out.state=d_->historyState;out.message=d_->historyMessage;
    out.completedCount=int(d_->historyRows.size());out.retryAtMs=qMax(d_->historyRetryAt,d_->providerRetryAt.value(d_->selected.provider));
    if(!d_->historyRows.isEmpty()){out.oldestAtMs=d_->historyRows.first().startsAtMs;out.newestAtMs=d_->historyRows.last().startsAtMs;}
    return out;
}
StoredCandlesOffer MarketCandles::setStoredCompleted(const MarketCandleSeries& stored){
    const auto at=d_->now();
    if(stored.identity!=d_->selected.identity||stored.intervalMs!=d_->selected.interval||stored.intervalMs<=0||stored.candles.isEmpty()||
        stored.candles.size()>200||stored.receivedAtMs<=0||stored.receivedAtMs>at||!d_->lastCompleted.candles.isEmpty())
        return StoredCandlesOffer::Refused;
    // An old run must not pass for a recent delayed price: at most a day or three candles back.
    const auto end=stored.candles.last().startsAtMs+stored.intervalMs;
    if(end>at||at-end>qMax<qint64>(DayMs,3*stored.intervalMs))return StoredCandlesOffer::Refused;
    if(!d_->canDisplayCompleted())return StoredCandlesOffer::NotNow;
    d_->lastCompleted=stored;d_->lastCompleted.forming.reset();d_->lastCompleted.fromArchive=true;emit changed();
    return StoredCandlesOffer::Accepted;
}
void MarketCandles::clearStoredCompleted(){
    if(!d_->lastCompleted.fromArchive)return;
    d_->lastCompleted={};d_->clearArchiveDisplay();emit changed();
}
void MarketCandles::clearDisplayCache(){
    const auto fresh=series();
    d_->cancel();d_->clearHistory();d_->displayCache.clear();d_->displayHistoryCache.clear();d_->lastCompleted={};d_->lastRest={};d_->reconnectAttempts=0;
    const bool retain=d_->eligible()&&(fresh.available||fresh.forming.has_value());
    d_->data=retain?fresh:emptySeries(d_->selected,d_->instrument());
    if(d_->selected.identity.isEmpty()){d_->data={};emit changed();return;}
    d_->start(retain);
}
RequestSpec MarketCandles::request(const QString& asset){
    static const QRegularExpression valid("\\A[A-Z0-9]{1,20}\\z");
    return request(valid.match(asset).hasMatch()?"coin:upbit:KRW-"+asset:QString{},IntervalMs);
}
RequestSpec MarketCandles::request(const QString& identity,qint64 interval){
    RequestSpec result;result.operation=Operation::Markets;const auto selected=selection(identity,interval);if(!selected.valid)return result;
    QUrlQuery query;
    if(selected.provider=="upbit"){
        result.url=QUrl(interval==DayMs?QString("https://api.upbit.com/v1/candles/days"):
            QString("https://api.upbit.com/v1/candles/minutes/%1").arg(interval/60000));
        query.addQueryItem("market",selected.code);query.addQueryItem("count","200");
    }else{
        result.url=QUrl("https://api.binance.com/api/v3/klines");query.addQueryItem("symbol",selected.code);
        query.addQueryItem("interval",wireInterval(interval));query.addQueryItem("limit","200");
    }
    result.url.setQuery(query);result.headers.insert("Accept","application/json");return result;
}
RequestSpec MarketCandles::historyRequest(const QString& identity,qint64 interval,qint64 before){
    auto result=request(identity,interval);const auto selected=selection(identity,interval);
    if(result.url.isEmpty()||before<=0||before%interval){result.url=QUrl();return result;}
    QUrlQuery query(result.url);
    if(selected.provider=="upbit")query.addQueryItem("to",QDateTime::fromMSecsSinceEpoch(before,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss'Z'"));
    else query.addQueryItem("endTime",QString::number(before-1));
    result.url.setQuery(query);return result;
}
bool MarketCandles::isRequest(const RequestSpec& request){
    if(request.method!="GET"||request.authenticated||!request.body.isEmpty()||request.operation!=Operation::Markets)return false;
    const QUrlQuery query(request.url);QString identity;qint64 interval=0;
    if(request.url.host()=="api.upbit.com"){
        identity="coin:upbit:"+query.queryItemValue("market");
        if(request.url.path()=="/v1/candles/days")interval=DayMs;
        else if(request.url.path().startsWith("/v1/candles/minutes/"))interval=request.url.path().mid(20).toLongLong()*60000;
    }else if(request.url.host()=="api.binance.com"&&request.url.path()=="/api/v3/klines"){
        identity="coin:binance:"+query.queryItemValue("symbol");
        for(const auto candidate:{60000LL,300000LL,900000LL,3600000LL,14400000LL,86400000LL})
            if(query.queryItemValue("interval")==wireInterval(candidate))interval=candidate;
    }
    auto expected=MarketCandles::request(identity,interval);
    if(query.hasQueryItem("to")){
        const auto before=QDateTime::fromString(query.queryItemValue("to"),Qt::ISODate);
        if(!before.isValid()||before.offsetFromUtc()!=0)return false;
        expected=MarketCandles::historyRequest(identity,interval,before.toMSecsSinceEpoch());
    }else if(query.hasQueryItem("endTime")){
        bool ok=false;const auto end=query.queryItemValue("endTime").toLongLong(&ok);
        if(!ok||end<=0||end==std::numeric_limits<qint64>::max())return false;
        expected=MarketCandles::historyRequest(identity,interval,end+1);
    }
    return !expected.url.isEmpty()&&request.url==expected.url&&request.headers==expected.headers;
}
MarketCandleSeries MarketCandles::parse(const QString& asset,const HttpResult& response,qint64 received){
    return parse("coin:upbit:KRW-"+asset,IntervalMs,response,received,received);
}
MarketCandleSeries MarketCandles::parse(const QString& identity,qint64 interval,const HttpResult& response,
    qint64 received,qint64 started,const MarketInstrument& item){
    const auto selected=selection(identity,interval);auto out=emptySeries(selected,item);out.receivedAtMs=received;
    if(response.status==429||response.status==418)out.reason="원천 요청 제한";
    if(!selected.valid||received<=0||started<=0||started>received||response.status!=200||response.networkError||response.timedOut||
        response.localRetryAfterSeconds>0||(selected.provider=="binance"&&!validInstrument(selected,item)))return out;
    const auto doc=parseExactJson(response.body);const int maximum=200;
    if(!doc.isArray()||doc.array().isEmpty()||doc.array().size()>maximum)return out;
    QSet<qint64> seen;QList<MarketCandle> rows;std::optional<MarketCandle> forming;qint64 sourceTime=0;
    for(const auto& value:doc.array()){
        MarketCandle row;
        if(selected.provider=="upbit"){
            if(!value.isObject())return out;const auto object=value.toObject();
            const auto text=object.value("candle_date_time_utc").toString();const auto date=QDateTime::fromString(text+"Z",Qt::ISODate);
            if(!date.isValid()||date.toUTC().toString("yyyy-MM-ddTHH:mm:ss")!=text||object.value("market").toString()!=selected.code||
                (interval==DayMs?object.contains("unit"):object.value("unit").toString()!=QString::number(interval/60000)))return out;
            row.startsAtMs=date.toMSecsSinceEpoch();
            if(!integer(object.value("timestamp"),&row.lastStoredTickMs)||row.lastStoredTickMs<row.startsAtMs||row.lastStoredTickMs>received)return out;
            row.open=object.value("opening_price").toString();row.high=object.value("high_price").toString();row.low=object.value("low_price").toString();
            row.close=object.value("trade_price").toString();row.volume=object.value("candle_acc_trade_volume").toString();
            sourceTime=qMax(sourceTime,row.lastStoredTickMs);
        }else{
            if(!value.isArray())return out;const auto tuple=value.toArray();qint64 closeTime=0,count=0;
            if(tuple.size()!=12||!integer(tuple[0],&row.startsAtMs)||!integer(tuple[6],&closeTime)||!integer(tuple[8],&count)||
                row.startsAtMs>std::numeric_limits<qint64>::max()-interval||closeTime!=row.startsAtMs+interval-1||
                !number(tuple[7].toString())||!number(tuple[9].toString())||!number(tuple[10].toString())||!number(tuple[11].toString()))return out;
            row.open=tuple[1].toString();row.high=tuple[2].toString();row.low=tuple[3].toString();row.close=tuple[4].toString();row.volume=tuple[5].toString();
        }
        if(row.startsAtMs<=0||row.startsAtMs%interval||row.startsAtMs>received||seen.contains(row.startsAtMs)||!validRow(&row))return out;
        seen.insert(row.startsAtMs);
        if(row.startsAtMs+interval<=started)rows.append(row);else{if(forming)return out;forming=row;}
    }
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.startsAtMs<b.startsAtMs;});
    out.candles=rows;out.forming=forming;out.sourceTimeMs=sourceTime;out.available=!rows.isEmpty();
    if(!rows.isEmpty()){out.restThroughMs=rows.last().startsAtMs;out.restReceivedAtMs=received;}
    out.reason=out.available?QString{}:QString("완료된 캔들 없음");return out;
}
static CandleCompression compressionImpl(const QList<MarketCandle>& candles,qint64 intervalMs,const ChartStudyTimeline* timeline){
    CandleCompression out;out.reason="연속 완료 캔들 21개 이상 필요";
    if(candles.isEmpty()||intervalMs<=0)return out;
    // Retain only the final uninterrupted run. Missing bars remain missing.
    qsizetype first=0;
    if(timeline){
        if(timeline->rows.size()!=candles.size()||candles.size()>200){out.reason="주식 timeline 입력 불일치";return out;}
        static const QRegularExpression canonical(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));
        for(qsizetype i=0;i<candles.size();++i){const auto& c=candles[i];const auto& state=timeline->rows[i];
            if(state.startsAtMs!=c.startsAtMs||c.startsAtMs<=0||state.completesAtMs<=c.startsAtMs||(i&&c.startsAtMs<=candles[i-1].startsAtMs)){out.reason="주식 timeline 입력 불일치";return out;}
            for(const auto& value:{c.open,c.high,c.low,c.close,c.volume})if(!canonical.match(value).hasMatch()){out.reason="주식 캔들 숫자 입력 오류";return out;}
            if(!state.eligible)first=i+1;else if(i&&(!state.connectsPrevious||!timeline->rows[i-1].eligible))first=i;
        }
    }else for(qsizetype i=1;i<candles.size();++i)if(candles[i].startsAtMs-candles[i-1].startsAtMs!=intervalMs)first=i;
    if(candles.size()-first<21)return out;
    QList<double> closes,tr;
    for(qsizetype i=first;i<candles.size();++i){
        const auto& c=candles[i];bool a=false,b=false,d=false;
        const double high=c.high.toDouble(&a),low=c.low.toDouble(&b),close=c.close.toDouble(&d);
        if(!a||!b||!d||!std::isfinite(high)||!std::isfinite(low)||!std::isfinite(close)||low<=0||close<low||close>high){out.reason="지표 계산 범위 초과";return out;}
        if(!closes.isEmpty())tr.append(std::max({high-low,std::abs(high-closes.last()),std::abs(low-closes.last())}));
        closes.append(close);
    }
    double ema=0,atr=0,mean=0,variance=0;
    for(int i=0;i<20;++i){ema+=closes[i]/20;atr+=tr[i]/20;mean+=closes[closes.size()-20+i]/20;}
    for(qsizetype i=20;i<closes.size();++i)ema+=(closes[i]-ema)*2/21;
    for(qsizetype i=20;i<tr.size();++i)atr=(atr*19+tr[i])/20;
    for(qsizetype i=closes.size()-20;i<closes.size();++i)variance+=std::pow(closes[i]-mean,2)/20;
    const double sigma=std::sqrt(variance);out.bandLow=mean-2*sigma;out.bandHigh=mean+2*sigma;out.channelLow=ema-1.5*atr;out.channelHigh=ema+1.5*atr;
    if(!std::isfinite(out.bandLow)||!std::isfinite(out.bandHigh)||!std::isfinite(out.channelLow)||!std::isfinite(out.channelHigh)){out.reason="지표 계산 범위 초과";return out;}
    out.available=true;out.compressed=out.bandLow>=out.channelLow&&out.bandHigh<=out.channelHigh;out.reason.clear();return out;
}
CandleCompression candleCompression(const QList<MarketCandle>& candles,qint64 intervalMs){return compressionImpl(candles,intervalMs,nullptr);}
} // namespace keefi

namespace keefi {
CandleCompression candleCompression(const QList<MarketCandle>& candles,qint64 intervalMs,const ChartStudyTimeline& timeline){return compressionImpl(candles,intervalMs,&timeline);}
}
