#include "keefi/MarketFlowHistory.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QTimeZone>
#include <QTimer>
#include <limits>
namespace keefi {
namespace {
qint64 after(qint64 at,qint64 delay){return delay>std::numeric_limits<qint64>::max()-at?std::numeric_limits<qint64>::max():at+delay;}
qint64 targetClose(qint64 at){return at>MarketFlowPolicy::DayMs+MarketFlowPolicy::HourMs?at/MarketFlowPolicy::HourMs*MarketFlowPolicy::HourMs-MarketFlowPolicy::DayMs:0;}
}
struct MarketFlowHistory::Private {
    MarketFlowHistory* q=nullptr;QPointer<TradingTransport> transport;Clock clock;QTimer timer;
    bool active=false,cycle=false;QString asset;QList<MarketInstrument> instruments;MarketInstrument selected;
    MarketFlowHistoricalGap cached;QList<MarketCandleSeries> rows;
    int index=0,failures=0;quint64 token=0,nextToken=1;
    qint64 startedAt=0,nextSendAt=0,retryAt=0,cycleCloseMs=0;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    MarketInstrument instrument()const {
        MarketInstrument found;int matches=0;
        for(const auto& item:instruments)if(item.symbol==asset+"USDT"){found=item;++matches;}
        return matches==1&&found.baseAsset==asset&&found.quoteAsset=="USDT"&&found.market=="spot"&&found.status=="TRADING"&&found.tradable?found:MarketInstrument{};
    }
    static bool sameInstrument(const MarketInstrument& a,const MarketInstrument& b){return a.symbol==b.symbol&&a.baseAsset==b.baseAsset&&
        a.quoteAsset==b.quoteAsset&&a.market==b.market&&a.status==b.status&&a.tradable==b.tradable;}
    bool cacheValid(qint64 at)const{return cached.available&&cached.asset==asset&&cached.comparisonCloseMs==targetClose(at)&&
        cached.receivedAtMs>0&&cached.receivedAtMs<=at&&at-cached.receivedAtMs<=MarketFlowPolicy::HistoryFreshnessMs;}
    void invalidate(){const bool wasAvailable=cached.available;cached={};if(wasAvailable)emit q->changed();}
    void cancel(){const bool pending=token!=0;token=0;cycle=false;index=0;rows.clear();if(pending&&transport)transport->abort();}
    QString identity()const{return index==0?"coin:upbit:KRW-"+asset:index==1?"coin:binance:"+asset+"USDT":QString("coin:upbit:KRW-USDT");}
    qint64 retryDelay(const HttpResult& response,qint64 at)const {
        const bool limited=response.status==429||response.localRetryAfterSeconds>0;
        qint64 delay=limited?1000:qMin<qint64>(300000,30000LL*(1<<qMin(qMax(0,failures-1),4)));
        delay=qMax(delay,qint64(qMax(0,response.localRetryAfterSeconds))*1000);
        for(auto it=response.headers.cbegin();it!=response.headers.cend();++it){
            if(it.key().compare("Retry-After",Qt::CaseInsensitive)!=0)continue;
            bool ok=false;const auto seconds=it.value().trimmed().toLongLong(&ok);
            if(ok&&seconds>0){delay=qMax(delay,seconds>std::numeric_limits<qint64>::max()/1000?std::numeric_limits<qint64>::max():seconds*1000);continue;}
            const auto text=QString::fromLatin1(it.value().trimmed());
            static const QRegularExpression date("\\A(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT\\z");
            if(date.match(text).hasMatch()){
                const auto parsed=QLocale::c().toDateTime(text,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");
                const auto deadline=QDateTime(parsed.date(),parsed.time(),QTimeZone("UTC"));
                if(parsed.isValid()&&deadline.toMSecsSinceEpoch()>at)delay=qMax(delay,deadline.toMSecsSinceEpoch()-at);
            }
        }
        return delay;
    }
    void fail(const HttpResult& response){
        cycle=false;rows.clear();index=0;invalidate();failures=qMin(failures+1,5);
        retryAt=after(now(),retryDelay(response,now()));nextSendAt=qMax(nextSendAt,retryAt);
    }
    void next(){
        if(!active||!cycle||!transport||token||index>=3||now()<nextSendAt)return;
        token=nextToken++;startedAt=now();nextSendAt=after(startedAt,1000);
        transport->send(token,MarketCandles::request(identity(),MarketFlowPolicy::HourMs));
    }
    void pump(){
        if(!active||!transport)return;const auto at=now();
        if(!targetClose(at)){cancel();invalidate();return;}
        if(cached.available&&!cacheValid(at))invalidate();
        if(cycle&&cycleCloseMs!=targetClose(at)){cancel();invalidate();}
        if(token&&(at<startedAt||at-startedAt>MarketCandles::FreshnessMs)){
            cancel();HttpResult timeout;timeout.timedOut=true;fail(timeout);
        }
        if(!cycle&&!cached.available){
            if(at<retryAt)return;
            static const QRegularExpression valid("\\A[A-Z0-9]{1,20}\\z");selected=instrument();
            if(!valid.match(asset).hasMatch()||selected.symbol.isEmpty())return;
            cycle=true;cycleCloseMs=targetClose(at);rows.clear();index=0;
        }
        next();
    }
};
MarketFlowHistory::MarketFlowHistory(TradingTransport* transport,QObject* parent):MarketFlowHistory(transport,Clock{},parent){}
MarketFlowHistory::MarketFlowHistory(TradingTransport* transport,Clock clock,QObject* parent):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->clock=std::move(clock);d_->timer.setInterval(1000);
    connect(&d_->timer,&QTimer::timeout,this,&MarketFlowHistory::tick);
    if(transport)connect(transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& response){
        if(!token||token!=d_->token)return;d_->token=0;
        if(!d_->active||!d_->cycle)return;const auto at=d_->now();
        if(d_->cycleCloseMs!=targetClose(at)){d_->cancel();d_->invalidate();d_->pump();return;}
        if(at<d_->startedAt||at-d_->startedAt>MarketCandles::FreshnessMs){d_->fail(response);return;}
        const auto parsed=MarketCandles::parse(d_->identity(),MarketFlowPolicy::HourMs,response,at,d_->startedAt,d_->selected);
        if(!parsed.available){d_->fail(response);return;}
        d_->rows.append(parsed);++d_->index;
        if(d_->index==3){
            const auto result=marketFlowHistoricalGap(d_->rows[0],d_->rows[1],d_->rows[2],at);
            if(!result.available){d_->fail(response);return;}
            d_->cycle=false;d_->cached=result;d_->failures=0;d_->retryAt=0;emit changed();return;
        }
        d_->next();
    });
}
MarketFlowHistory::~MarketFlowHistory(){d_->cancel();}
void MarketFlowHistory::setActive(bool active){if(d_->active==active)return;d_->active=active;if(active){d_->timer.start();tick();}else {d_->timer.stop();d_->cancel();}}
void MarketFlowHistory::setAsset(const QString& asset){if(d_->asset==asset)return;d_->cancel();d_->invalidate();d_->asset=asset;tick();}
void MarketFlowHistory::setInstruments(const QList<MarketInstrument>& instruments){
    const auto before=d_->instrument();d_->instruments=instruments;
    if(!Private::sameInstrument(before,d_->instrument())){d_->cancel();d_->invalidate();}tick();
}
MarketFlowHistoricalGap MarketFlowHistory::snapshot(qint64 asOfMs)const{return d_->cacheValid(asOfMs?asOfMs:d_->now())?d_->cached:MarketFlowHistoricalGap{};}
void MarketFlowHistory::refresh(){tick();}
void MarketFlowHistory::tick(){d_->pump();}
}
