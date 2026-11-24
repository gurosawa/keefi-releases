#include "keefi/RankingStudyRunner.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QFutureWatcher>
#include <QIODevice>
#include <QMap>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QThreadPool>
#include <optional>

namespace keefi {
namespace {
QByteArray fingerprint(const RankingStudyCalculation& input){
    const auto& o=input.observation;const auto& s=o.series;QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);
    out<<int(input.study)<<o.stock<<o.loading<<o.delayed<<o.status<<s.available<<s.identity<<s.sourceUrl<<s.reason<<s.receivedAtMs<<s.sourceTimeMs<<s.intervalMs;
    out<<s.candles.size();for(const auto& row:s.candles)out<<row.startsAtMs<<row.open<<row.high<<row.low<<row.close<<row.volume;
    const auto& t=o.timeline;out<<t.adjustedComparable<<t.comparisonReason<<t.projectionCovered<<t.projectionReason<<t.nextStartsAtMs<<t.rows.size();
    for(const auto& row:t.rows){out<<row.startsAtMs<<row.completesAtMs<<row.tradingDate<<int(row.session)<<row.connectsPrevious<<row.eligible<<row.missingSlotsBefore.has_value();
        if(row.missingSlotsBefore)out<<*row.missingSlotsBefore;out<<row.reason<<row.exclusions.size();for(const auto e:row.exclusions)out<<int(e);}
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
}
struct RankingStudyRunner::Private {
    struct Request {RankingStudyCalculation input;QByteArray fingerprint;quint64 generation=0;};
    struct Cached {QByteArray fingerprint;RankingStudyValue value;};
    struct Job {Request request;quint64 epoch=0;};
    RankingStudyRunner* q=nullptr;Calculator calculate;
    QMap<QString,Request> active;QMap<QString,Cached> cache;QStringList order,cacheOrder;
    std::optional<Job> running;quint64 epoch=1,nextGeneration=1;
    bool cached(const QString& key,const QByteArray& digest)const {const auto found=cache.constFind(key);return found!=cache.cend()&&found->fingerprint==digest;}
    void start(){
        if(running)return;
        QString selected;for(const auto& key:order){const auto found=active.constFind(key);if(found!=active.cend()&&!cached(key,found->fingerprint)){selected=key;break;}}
        if(selected.isEmpty())return;
        const Job job{active.value(selected),epoch};running=job;
        auto promise=std::make_shared<QPromise<RankingStudyValue>>();auto* watcher=new QFutureWatcher<RankingStudyValue>(q);
        QObject::connect(watcher,&QFutureWatcher<RankingStudyValue>::finished,q,[this,watcher,job]{
            const auto result=watcher->result();watcher->deleteLater();running.reset();
            const auto current=active.constFind(job.request.input.key);QPointer<RankingStudyRunner> alive(q);
            if(job.epoch==epoch&&current!=active.cend()&&current->generation==job.request.generation&&current->fingerprint==job.request.fingerprint){
                const auto key=job.request.input.key;cache.insert(key,{job.request.fingerprint,result});cacheOrder.removeAll(key);cacheOrder.append(key);
                while(cache.size()>128){auto evict=cacheOrder.begin();while(evict!=cacheOrder.end()&&active.contains(*evict))++evict;
                    if(evict==cacheOrder.end())evict=cacheOrder.begin();cache.remove(*evict);cacheOrder.erase(evict);}
                emit q->changed();if(!alive)return;
            }
            start();
        });
        watcher->setFuture(promise->future());const auto calculator=calculate;
        QThreadPool::globalInstance()->start([promise,calculator,job]{promise->start();promise->addResult(calculator(job.request.input.study,job.request.input.observation));promise->finish();});
    }
};
RankingStudyRunner::RankingStudyRunner(QObject* parent,Calculator calculator):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->calculate=calculator?std::move(calculator):Calculator(rankingCandleStudy);
}
RankingStudyRunner::~RankingStudyRunner(){++d_->epoch;d_->active.clear();}
void RankingStudyRunner::setRequests(const QList<RankingStudyCalculation>& requests){
    QMap<QString,Private::Request> next;QStringList order;
    for(const auto& input:requests){
        if(next.size()>=60)break;if(input.key.isEmpty()||input.key.size()>1024||input.study==RankingStudy::None||next.contains(input.key))continue;
        // Reject an oversized DTO rather than truncate a price history into a
        // different result. The ordinary source contract is already <=200.
        if(input.observation.series.candles.size()>200||input.observation.timeline.rows.size()>200)continue;
        const auto digest=fingerprint(input);const auto previous=d_->active.constFind(input.key);
        if(previous!=d_->active.cend()&&previous->fingerprint==digest)next.insert(input.key,*previous);
        else {auto copy=input;copy.observation.series.forming.reset();next.insert(input.key,{std::move(copy),digest,d_->nextGeneration++});}
        order.append(input.key);
    }
    d_->active=std::move(next);d_->order=std::move(order);d_->start();
}
RankingStudyValue RankingStudyRunner::value(const QString& key)const{
    const auto request=d_->active.constFind(key);if(request!=d_->active.cend()&&d_->cached(key,request->fingerprint))return d_->cache.value(key).value;
    return {false,tr("받는 중"),tr("완료 기록으로 상태를 확인하고 있어요"),0};
}
void RankingStudyRunner::clear(){++d_->epoch;d_->active.clear();d_->order.clear();d_->cache.clear();d_->cacheOrder.clear();}
int RankingStudyRunner::requestCount()const{return d_->active.size();}
int RankingStudyRunner::cacheSize()const{return d_->cache.size();}
bool RankingStudyRunner::pending()const{if(d_->running)return true;for(auto i=d_->active.cbegin();i!=d_->active.cend();++i)if(!d_->cached(i.key(),i->fingerprint))return true;return false;}
}
