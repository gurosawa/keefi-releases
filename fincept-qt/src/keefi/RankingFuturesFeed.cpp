#include "keefi/RankingFuturesFeed.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartStudyRequestPacer.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <limits>

namespace keefi {
namespace {
constexpr qint64 Hour=3600000,RefreshMs=60000,TimeoutMs=10000;
bool accepted(const HttpResult& r){return r.status==200&&!r.networkError&&!r.timedOut&&r.localRetryAfterSeconds<=0;}
bool supported(RankingStudy s){return s==RankingStudy::OpenInterest||s==RankingStudy::SpotFutures;}
QString key(const QString& base,RankingStudy study){return QString::number(int(study))+":"+base;}
qint64 retryDelay(const HttpResult& response,qint64 now){
    qint64 delay=response.status==429||response.status==418?60000:5000;
    if(response.localRetryAfterSeconds>0)delay=qMax(delay,qint64(response.localRetryAfterSeconds)*1000);
    for(auto i=response.headers.cbegin();i!=response.headers.cend();++i)if(i.key().compare("Retry-After",Qt::CaseInsensitive)==0){
        bool ok=false;const auto seconds=i.value().toLongLong(&ok);
        if(ok&&seconds>=0&&seconds<=std::numeric_limits<qint64>::max()/1000)delay=qMax(delay,seconds*1000);
        else {const auto date=QDateTime::fromString(QString::fromLatin1(i.value()),Qt::RFC2822Date);if(date.isValid())delay=qMax(delay,date.toMSecsSinceEpoch()-now);}
    }
    return delay;
}
}
struct RankingFuturesFeed::Private {
    enum class Read {Contracts,OpenInterest,Spot,Futures};
    struct Job {QString base;Read read=Read::Contracts;quint64 epoch=0;};
    struct Cached {RankingStudyValue value; qint64 received=0;QString failure;};
    struct SpotPart {HttpResult response;qint64 decision=0,received=0;quint64 epoch=0;};
    RankingFuturesFeed* q=nullptr;QPointer<TradingTransport> transport;QPointer<ChartInstrumentCatalog> catalog;
    Clock clock;QTimer timer;QStringList targets;RankingStudy study=RankingStudy::None;
    std::shared_ptr<ChartStudyRequestPacer> pacer;ChartStudyRequestPacer::Client pacingClient=0;
    QList<Job> queue;Job running;quint64 epoch=1,token=0,nextToken=1;
    qint64 sentAt=0,nextSend=0,nextRefresh=0,contractsAt=0;HttpResult contracts;
    QHash<QString,Cached> cache;QHash<QString,SpotPart> spot;bool pumping=false;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    bool exactSpot(const QString& base,qint64 at)const{
        if(!catalog)return false;int matches=0;
        for(const auto& item:catalog->instruments("binance",at))if(item.symbol==base+"USDT"){
            if(item.baseAsset!=base||item.quoteAsset!="USDT"||item.status!="TRADING"||!item.tradable||item.market!="spot")return false;
            ++matches;
        }
        return matches==1;
    }
    bool exactContract(const QString& base,qint64 at)const{
        if(contractsAt<=0||at<contractsAt||at-contractsAt>=FreshnessMs)return false;
        PublicDerivativesMetrics verified;
        return MarketMetrics::applyResponse(MarketMetrics::Read::Contracts,base+"USDT",contracts,contractsAt,&verified)&&verified.baseAsset==base;
    }
    void abort(){++epoch;const bool active=token;token=0;queue.clear();spot.clear();if(pacer)pacer->cancel(pacingClient);if(active&&transport)transport->abort();}
    void failure(const QString& base,const QString& reason){auto& saved=cache[key(base,study)];saved.failure=reason;if(!saved.value.available){saved.value.text=reason;saved.value.information=reason;}}
    RequestSpec request(const Job& job)const{
        const auto identity="coin:binance:"+job.base+"USDT",symbol=job.base+"USDT";
        using PublicRead=ChartPublicContextSource::Read;
        switch(job.read){
        case Read::Contracts:return ChartPublicContextSource::request(PublicRead::Contracts,identity);
        case Read::OpenInterest:return ChartPublicContextSource::request(PublicRead::OpenInterest,identity,symbol);
        case Read::Spot:return MarketCandles::request(identity,Hour);
        case Read::Futures:return ChartPublicContextSource::request(PublicRead::FuturesPrice,identity,symbol);
        }
        return {};
    }
    void enqueue(qint64 at){
        if(!catalog)return;
        catalog->ensure("binance");QStringList eligible;
        for(const auto& base:targets){
            if(exactSpot(base,at))eligible.append(base);
            else failure(base,catalog->instruments("binance",at).isEmpty()?QStringLiteral("종목 확인 중"):QStringLiteral("지원 없음"));
        }
        if(eligible.isEmpty())return;
        if(contractsAt<=0||at<contractsAt||at-contractsAt>=FreshnessMs)queue.append({eligible.first(),Read::Contracts,epoch});
        for(const auto& base:eligible){
            const auto saved=cache.value(key(base,study));
            if(saved.value.available&&saved.failure.isEmpty()&&at>=saved.received&&at-saved.received<RefreshMs&&saved.value.throughMs==at/Hour*Hour)continue;
            if(study==RankingStudy::OpenInterest)queue.append({base,Read::OpenInterest,epoch});
            else {queue.append({base,Read::Spot,epoch});queue.append({base,Read::Futures,epoch});}
        }
        nextRefresh=at+RefreshMs;
    }
    void apply(const Job& job,const HttpResult& response,qint64 received,qint64 decision){
        if(job.epoch!=epoch||!targets.contains(job.base))return;
        if(job.read==Read::Contracts){
            contracts=accepted(response)?response:HttpResult{};contractsAt=accepted(response)?received:0;
            for(const auto& base:targets)if(!exactContract(base,received)){
                auto& saved=cache[key(base,study)];saved={};failure(base,accepted(response)?QStringLiteral("지원 없음"):QStringLiteral("받지 못했어요"));
            }
            return;
        }
        if(!exactSpot(job.base,received)||!exactContract(job.base,received)){failure(job.base,QStringLiteral("종목 확인 중"));return;}
        if(!accepted(response)){failure(job.base,QStringLiteral("받지 못했어요"));spot.remove(job.base);return;}
        if(job.read==Read::Spot){spot[job.base]={response,decision,received,epoch};return;}
        RankingStudyValue value;
        if(job.read==Read::OpenInterest)value=RankingFuturesFeed::parseOpenInterest24h(job.base,response,decision);
        else {
            const auto part=spot.take(job.base);
            if(part.epoch!=epoch||part.decision/Hour!=decision/Hour||received-part.received>RefreshMs){failure(job.base,QStringLiteral("같은 시각 자료 부족"));return;}
            value=RankingFuturesFeed::parseSpotFutures(job.base,part.response,response,decision);
        }
        if(value.available){
            auto& saved=cache[key(job.base,study)];saved={value,received,{}};
            const auto primary=request(job).url.toString();
            saved.value.information+="\n"+primary;
            if(job.read==Read::Futures)saved.value.information+="\n"+MarketCandles::request("coin:binance:"+job.base+"USDT",Hour).url.toString();
            saved.value.information+=QStringLiteral("\n완료 %1 · 확인 %2").arg(QDateTime::fromMSecsSinceEpoch(value.throughMs).toString(Qt::ISODate),QDateTime::fromMSecsSinceEpoch(received).toString(Qt::ISODate));
            while(cache.size()>100)cache.erase(cache.begin());
        }else failure(job.base,value.text);
    }
    void finished(quint64 incoming,const HttpResult& response){
        if(!token||incoming!=token)return;token=0;const auto job=running;const auto at=now();
        if(!accepted(response)){
            const auto delay=retryDelay(response,at);
            const auto until=at>std::numeric_limits<qint64>::max()-delay?std::numeric_limits<qint64>::max():at+delay;
            nextSend=qMax(nextSend,until);
        }
        apply(job,response,at,sentAt);emit q->changed();pump();
    }
    void pump(){
        if(pumping||!transport||targets.isEmpty()||!supported(study))return;
        QScopedValueRollback<bool> guard(pumping,true);const auto at=now();
        if(token){if(at-sentAt<TimeoutMs)return;const auto job=running;token=0;transport->abort();HttpResult result;result.timedOut=true;apply(job,result,at,sentAt);nextSend=qMax(nextSend,at+5000);emit q->changed();return;}
        if(queue.isEmpty()&&at>=nextRefresh)enqueue(at);
        while(!queue.isEmpty()){
            const auto job=queue.first();
            if(job.epoch!=epoch||!targets.contains(job.base)){queue.removeFirst();continue;}
            if(job.read!=Read::Contracts&&(!exactSpot(job.base,at)||!exactContract(job.base,at))){failure(job.base,QStringLiteral("지원 없음"));queue.removeFirst();emit q->changed();continue;}
            if(at<nextSend)return;
            const auto spec=request(job);if(spec.url.isEmpty()){failure(job.base,QStringLiteral("지원 없음"));queue.removeFirst();continue;}
            if(pacer&&!pacer->tryAcquire(pacingClient,spec.url))return;
            queue.removeFirst();running=job;token=nextToken++;sentAt=at;nextSend=at+1000;
            transport->send(token,spec);return;
        }
    }
};
RankingFuturesFeed::RankingFuturesFeed(TradingTransport* transport,QObject* parent,Clock clock):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->clock=std::move(clock);d_->timer.setInterval(200);
    connect(&d_->timer,&QTimer::timeout,this,[this]{d_->pump();});
    if(transport){connect(transport,&TradingTransport::finished,this,[this](quint64 t,const HttpResult& r){d_->finished(t,r);});
        connect(transport,&QObject::destroyed,this,[this]{d_->token=0;d_->transport=nullptr;d_->queue.clear();d_->timer.stop();emit changed();});}
}
RankingFuturesFeed::~RankingFuturesFeed(){d_->timer.stop();if(d_->transport)disconnect(d_->transport,nullptr,this,nullptr);d_->abort();if(d_->pacer)d_->pacer->release(d_->pacingClient);}
void RankingFuturesFeed::setCatalog(ChartInstrumentCatalog* catalog){
    if(d_->catalog==catalog)return;if(d_->catalog)disconnect(d_->catalog,nullptr,this,nullptr);d_->abort();d_->catalog=catalog;d_->nextRefresh=0;
    if(catalog)connect(catalog,&ChartInstrumentCatalog::changed,this,[this]{d_->nextRefresh=0;d_->pump();emit changed();});d_->pump();
}
void RankingFuturesFeed::setRequestPacer(std::shared_ptr<ChartStudyRequestPacer> pacer){
    if(d_->pacer==pacer)return;if(d_->pacer)d_->pacer->release(d_->pacingClient);d_->pacer=std::move(pacer);d_->pacingClient=d_->pacer?d_->pacer->createClient():0;
}
void RankingFuturesFeed::setTargets(const QStringList& bases,RankingStudy study){
    QStringList next;static const QRegularExpression valid("\\A[A-Z0-9]{1,20}\\z");
    if(supported(study))for(const auto& base:bases)if(valid.match(base).hasMatch()&&!next.contains(base)){next.append(base);if(next.size()==MaximumTargets)break;}
    if(next.isEmpty())study=RankingStudy::None;
    if(next==d_->targets&&study==d_->study)return;
    if(study==d_->study&&QSet<QString>(next.cbegin(),next.cend())==QSet<QString>(d_->targets.cbegin(),d_->targets.cend())){d_->targets=next;return;}
    d_->abort();d_->targets=next;d_->study=study;d_->nextRefresh=0;
    if(next.isEmpty())d_->timer.stop();else d_->timer.start();d_->pump();emit changed();
}
void RankingFuturesFeed::clear(){setTargets({},RankingStudy::None);}
void RankingFuturesFeed::pump(){d_->pump();}
RankingStudyValue RankingFuturesFeed::observation(const QString& base)const{
    if(!d_->targets.contains(base)||!supported(d_->study))return {false,QStringLiteral("지원 없음"),QStringLiteral("현재 보이는 코인 순위에서만 확인해요"),0};
    const auto saved=d_->cache.value(key(base,d_->study));
    if(!saved.value.available)return saved.value.text.isEmpty()?RankingStudyValue{false,QStringLiteral("받는 중"),QStringLiteral("공개 종목과 완료 관측 자료를 확인하고 있어요"),0}:saved.value;
    auto result=saved.value;const auto at=d_->now();
    if(at<saved.received||at-saved.received>=FreshnessMs||result.throughMs!=at/Hour*Hour||!saved.failure.isEmpty()){
        result.text+=QStringLiteral(" · 지연");result.information+=QStringLiteral("\n최근 공개 관측값이에요. ")+saved.failure;
    }
    return result;
}
}
