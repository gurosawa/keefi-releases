#include "keefi/RankingStudyRunner.h"
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QTest>
#include <atomic>
using namespace keefi;
namespace {
RankingStudyCalculation request(QString key,QString value="original",qint64 revision=1){
    RankingStudyCalculation r;r.key=key;r.study=RankingStudy::Volume;r.observation.series.receivedAtMs=revision;r.observation.status=value;return r;
}
RankingStudyValue computed(const ChartStudyObservation& o){return {true,o.status,{},o.series.receivedAtMs};}
}
class RankingStudyRunnerTest:public QObject {
    Q_OBJECT
private slots:
    void calculationRunsOutsideUiAndCopiesItsInput();
    void latestSnapshotCoalescesAndRejectsSameKeyOldRevision();
    void clearRevokesInFlightEvenWhenSameKeyReturns();
    void boundsCacheReuseAndRemovedKeys();
    void destructionWhileRunningIsSafe();
};
void RankingStudyRunnerTest::calculationRunsOutsideUiAndCopiesItsInput(){
    auto heartbeat=std::make_shared<bool>(false);std::atomic_bool worker=false;auto* ui=QThread::currentThread();
    const auto calculate=[&](RankingStudy,const ChartStudyObservation& o){worker=QThread::currentThread()!=ui;QThread::msleep(25);return computed(o);};
    QTimer::singleShot(0,this,[heartbeat]{*heartbeat=true;});
    if(qEnvironmentVariableIsSet("KEEFI_RANKING_RUNNER_LEGACY")){calculate(RankingStudy::Volume,{});QVERIFY2(*heartbeat,"RED: the previous synchronous ranking calculation blocks the queued UI heartbeat");return;}
    RankingStudyRunner runner(nullptr,calculate);QSignalSpy ready(&runner,&RankingStudyRunner::changed);
    auto input=request("copy");runner.setRequests({input});input.observation.status="mutated";
    QVERIFY(!runner.value("copy").available);QTRY_COMPARE(ready.size(),1);QVERIFY(*heartbeat);QVERIFY(worker.load());QCOMPARE(runner.value("copy").text,QString("original"));
}
void RankingStudyRunnerTest::latestSnapshotCoalescesAndRejectsSameKeyOldRevision(){
    auto gate=std::make_shared<QSemaphore>();std::atomic_int calls=0;std::atomic_bool firstStarted=false;
    RankingStudyRunner runner(nullptr,[&](RankingStudy,const ChartStudyObservation& o){if(++calls==1){firstStarted=true;gate->tryAcquire(1,2000);}return computed(o);});
    QSignalSpy ready(&runner,&RankingStudyRunner::changed);runner.setRequests({request("same","first",1)});QTRY_VERIFY(firstStarted.load());
    runner.setRequests({request("same","second",2)});runner.setRequests({request("same","latest",3)});gate->release();
    QTRY_VERIFY(!runner.pending());QCOMPARE(calls.load(),2);QCOMPARE(ready.size(),1);QCOMPARE(runner.value("same").text,QString("latest"));
}
void RankingStudyRunnerTest::clearRevokesInFlightEvenWhenSameKeyReturns(){
    auto gate=std::make_shared<QSemaphore>();std::atomic_int calls=0;
    RankingStudyRunner runner(nullptr,[&](RankingStudy,const ChartStudyObservation& o){if(++calls==1)gate->tryAcquire(1,2000);return computed(o);});
    QSignalSpy ready(&runner,&RankingStudyRunner::changed);runner.setRequests({request("same")});QTRY_COMPARE(calls.load(),1);
    runner.clear();runner.setRequests({request("same")});gate->release();QTRY_VERIFY(!runner.pending());
    QCOMPARE(calls.load(),2);QCOMPARE(ready.size(),1);QVERIFY(runner.value("same").available);runner.clear();QCOMPARE(runner.cacheSize(),0);QCOMPARE(runner.requestCount(),0);
}
void RankingStudyRunnerTest::boundsCacheReuseAndRemovedKeys(){
    std::atomic_int calls=0;RankingStudyRunner runner(nullptr,[&](RankingStudy,const ChartStudyObservation& o){++calls;return computed(o);});
    QList<RankingStudyCalculation> rows;for(int i=0;i<70;++i)rows.append(request(QString::number(i)));
    runner.setRequests(rows);QCOMPARE(runner.requestCount(),60);QTRY_VERIFY(!runner.pending());QCOMPARE(calls.load(),60);QCOMPARE(runner.cacheSize(),60);
    runner.setRequests(rows);QVERIFY(!runner.pending());QCOMPARE(calls.load(),60);
    runner.setRequests({});QVERIFY(!runner.value("0").available);runner.setRequests({rows.first()});QVERIFY(runner.value("0").available);QCOMPARE(calls.load(),60);
    for(int batch=1;batch<3;++batch){rows.clear();for(int i=0;i<60;++i)rows.append(request(QString::number(batch*100+i)));runner.setRequests(rows);QTRY_VERIFY(!runner.pending());}
    QCOMPARE(calls.load(),180);QCOMPARE(runner.cacheSize(),128);QVERIFY(!runner.value("0").available);
}
void RankingStudyRunnerTest::destructionWhileRunningIsSafe(){
    auto gate=std::make_shared<QSemaphore>();auto completed=std::make_shared<std::atomic_bool>(false);
    auto* runner=new RankingStudyRunner(nullptr,[gate,completed](RankingStudy,const ChartStudyObservation& o){gate->tryAcquire(1,2000);*completed=true;return computed(o);});
    runner->setRequests({request("removed")});delete runner;gate->release();QTRY_VERIFY(completed->load());
}
QTEST_GUILESS_MAIN(RankingStudyRunnerTest)
#include "tst_ranking_study_runner.moc"
