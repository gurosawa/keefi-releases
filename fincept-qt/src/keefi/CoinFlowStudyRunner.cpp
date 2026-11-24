#include "keefi/CoinFlowStudyRunner.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPromise>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <optional>

namespace keefi {
struct CoinFlowStudyRunner::Private {
    struct Job {CoinFlowSnapshot input;qint64 asOf=0;QString key;quint64 generation=0;QSet<QString> enabled;};
    struct Result {CoinFlowStudySet studies;ExtendedAmountDelta delta;ExtendedLowVolumeZones lowVolume;ExtendedStrongReversal strong;qint64 ns=0;};
    CoinFlowStudyRunner* q=nullptr;Calculator calculate;QTimer timer;QElapsedTimer elapsed;
    bool enabled=true,running=false,builtinCalculator=false;quint64 generation=1;QString context;QSet<QString> options;std::optional<Job> latest;
    ExtendedStrongReversal deliveredStrong;
    void start(){
        if(!enabled||running||!latest)return;
        if(const auto wait=elapsed.isValid()?1000-elapsed.elapsed():0;wait>0){timer.start(int(wait));return;} // One clock read; never a negative wait.
        const auto job=std::move(*latest);latest.reset();running=true;elapsed.restart();
        auto promise=std::make_shared<QPromise<Result>>();auto* watcher=new QFutureWatcher<Result>(q);
        QObject::connect(watcher,&QFutureWatcher<Result>::finished,q,[this,watcher,job]{
            const auto result=watcher->result();watcher->deleteLater();running=false;
            QPointer<CoinFlowStudyRunner> alive(q);
            if(enabled&&generation==job.generation&&context==job.key){
                deliveredStrong=result.strong;
                emit q->resultReady(job.key,result.studies,result.ns);
                if(!alive)return;
                if(enabled&&generation==job.generation&&context==job.key)
                    emit q->extendedResultReady(job.key,result.delta,result.lowVolume,result.strong);
                if(!alive)return;
                if(enabled&&generation==job.generation&&context==job.key)
                    emit q->observationReady(job.input,job.key,result.studies,result.strong);
                if(!alive)return;
                if(enabled&&generation==job.generation&&context==job.key)
                    emit q->calculatedResultReady(job.input,job.key,result.studies);
                if(!alive)return;
            }
            start();
        });
        watcher->setFuture(promise->future());const auto calculator=calculate;const auto builtin=builtinCalculator;const auto previous=deliveredStrong;
        QThreadPool::globalInstance()->start([promise,calculator,builtin,previous,job]{
            promise->start();QElapsedTimer clock;clock.start();
            auto studies=builtin&&job.enabled.contains("strong")?coinFlowStudiesWithReversalEvidence(job.input,job.asOf):calculator(job.input,job.asOf);
            ExtendedAmountDelta delta;ExtendedLowVolumeZones lowVolume;ExtendedStrongReversal strong;
            if(job.enabled.contains("cvd"))delta=extendedAmountDelta(job.input.primary,job.asOf);
            if(job.enabled.contains("low_volume"))lowVolume=extendedTradeLowVolumeZones(job.input.primary,job.asOf);
            if(job.enabled.contains("strong"))strong=extendedStrongReversal(studies,&previous);
            const auto ns=clock.nsecsElapsed();
            promise->addResult(Result{std::move(studies),std::move(delta),std::move(lowVolume),std::move(strong),ns});promise->finish();
        });
    }
};
CoinFlowStudyRunner::CoinFlowStudyRunner(QObject* parent,Calculator calculate):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->builtinCalculator=!calculate;d_->calculate=calculate?std::move(calculate):Calculator(coinFlowStudies);d_->timer.setSingleShot(true);
    connect(&d_->timer,&QTimer::timeout,this,[this]{d_->start();});
}
CoinFlowStudyRunner::~CoinFlowStudyRunner(){d_->timer.stop();++d_->generation;}
void CoinFlowStudyRunner::request(const CoinFlowSnapshot& input,qint64 asOf,const QString& key,const QSet<QString>& options){
    if(!d_->enabled||key.isEmpty()||asOf<=0)return;
    if(key!=d_->context||options!=d_->options){invalidate();d_->context=key;d_->options=options;}
    d_->latest=Private::Job{input,asOf,key,d_->generation,options};d_->start();
}
void CoinFlowStudyRunner::invalidate(){++d_->generation;d_->latest.reset();d_->context.clear();d_->deliveredStrong={};d_->timer.stop();}
void CoinFlowStudyRunner::setEnabled(bool enabled){if(d_->enabled==enabled)return;d_->enabled=enabled;if(!enabled)invalidate();}
bool CoinFlowStudyRunner::pending()const{return d_->running||d_->latest.has_value();}
}
