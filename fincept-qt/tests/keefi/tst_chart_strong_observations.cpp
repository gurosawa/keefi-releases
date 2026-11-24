#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/SystemNotificationSink.h"
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Base=1728000000000,Minute=60000;
const WatchlistIdentity Identity{"coin","upbit","KRW-SYN"};
class Sink final:public SystemNotificationSink {public:QList<SystemNotice> posted;QStringList removed;SystemNoticePermission permission()const override{return SystemNoticePermission::Granted;}QString status()const override{return {};}void refreshPermission()override{}void requestAuthorization()override{}void post(const SystemNotice& n)override{posted.append(n);emit submissionFinished(n.identifier,n.privateEpoch,true);}void remove(const QStringList& ids)override{removed.append(ids);}void setPrivateContext(quint64,bool)override{}void stop()override{}};
CoinFlowSnapshot input(int count,qint64 now){
 CoinFlowSnapshot out;out.identity=Identity.key();auto& instrument=out.primary.instrument;
 instrument.venue=CoinFlowVenue::UpbitSpot;instrument.grain=CoinFlowGrain::UpbitTrade;instrument.identity=out.identity;instrument.symbol="KRW-SYN";instrument.baseAsset="SYN";instrument.quoteAsset="KRW";instrument.verified=true;instrument.evidence={"합성 공식형식 metadata","https://api.upbit.com/v1/market/all",0,now,now};out.primary.evidence=instrument.evidence;
 for(int i=0;i<count;++i){const auto close=i==17?"110":i==18?"100":i==19?"95":i==20?"98":"100";CoinFlowClosedMinute row;row.candle={Base+i*Minute,0,i==20?"95":"100","120","90",close,i==20?"40":"10"};row.completesAtMs=Base+(i+1)*Minute;row.knownAtMs=row.decisionAtMs=row.completesAtMs+1000;row.connectsPrevious=i>0;row.liveEligible=true;row.evidence={"합성 완료1분",MarketCandles::request(out.identity,Minute).url.toString(),0,row.knownAtMs,row.knownAtMs};out.minutes.append(row);}
 return out;
}
ExtendedStrongReversal strong(const CoinFlowStudySet& core){ExtendedStrongReversal r;r.state={true,IndicatorDirection::Up,{}};r.grade=ExtendedReversalGrade::Strong;r.throughMs=core.lastCompletedAtMs;r.knownAtMs=core.reversal.state.knownAtMs;return r;}
void rule(ChartAlertEngine& engine){QVERIFY(engine.setPublicLists({{"public-a",{Identity}}}));auto r=engine.rule("public-a");r.enabled=true;r.pullback=r.results=false;r.whales=true;r.cooldownMs=0;QVERIFY(engine.setRule(r));}
}
class ChartStrongObservationsTest:public QObject {
 Q_OBJECT
private slots:
 void preparedCoreRejectsMismatchedIdentityAndFutureCalculation(){
  for(const auto fault:{QString("identity"),QString("future")}){qint64 now=Base+21*Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});rule(engine);engine.beginLane(Identity,Minute,1,true);auto prior=input(20,now);engine.observeCoinFlowCalculated(prior,coinFlowStudies(prior,now),1);
   auto current=input(21,now);auto core=coinFlowStudies(current,now);QVERIFY(core.available);QVERIFY(core.reversal.rebound);if(fault=="identity")core.identity="coin:upbit:KRW-OTHER";else core.asOfMs=now+1;engine.observeCoinFlowCalculated(current,core,1);QVERIFY2(sink.posted.isEmpty(),qPrintable(fault));}
 }
 void strongOptInCanNotifyWithoutWhaleRule(){
  qint64 now=Base+20*Minute;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(engine.setObservationEnabled("strong_reversal",true));now=Base+21*Minute+1000;auto current=input(21,now);auto core=coinFlowStudies(current,now);QVERIFY(core.reversal.rebound);auto grade=strong(core);
  engine.observeStrongReversalCalculated(current,core,grade);QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().title.contains("강한 반전"));QVERIFY(sink.posted.first().body.contains("강함"));QVERIFY(!sink.posted.first().privateData);QVERIFY(sink.posted.first().playSound);engine.observeStrongReversalCalculated(current,core,grade);QCOMPARE(sink.posted.size(),1);
 }
 void sameReversalMergesWithOriginalEvent(){
  QString noticeId;for(bool strongFirst:{true,false}){qint64 now=Base+20*Minute;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);rule(engine);QVERIFY(engine.setObservationEnabled("strong_reversal",true));now=Base+21*Minute+1000;engine.beginLane(Identity,Minute,1,true);auto prior=input(20,now);engine.observeCoinFlowCalculated(prior,coinFlowStudies(prior,now),1);auto current=input(21,now);auto core=coinFlowStudies(current,now);auto grade=strong(core);
   if(strongFirst)engine.observeStrongReversalCalculated(current,core,grade);engine.observeCoinFlowCalculated(current,core,1);if(!strongFirst)engine.observeStrongReversalCalculated(current,core,grade);
   QCOMPARE(sink.posted.size(),2); // One volume burst plus exactly one reversal, never a third strong copy.
   int strongCount=0;for(const auto& notice:sink.posted)if(notice.title.contains("강한 반전")){++strongCount;QVERIFY(notice.body.contains("강함"));if(noticeId.isEmpty())noticeId=notice.identifier;else QCOMPARE(notice.identifier,noticeId);}QCOMPARE(strongCount,1);
  }
 }
 void basicAndIncompletePreserveOriginalReversalInEitherOrder(){
  for(const auto quality:{ExtendedReversalGrade::Basic,ExtendedReversalGrade::Incomplete})for(bool strongFirst:{true,false}){
   qint64 now=Base+20*Minute;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);rule(engine);engine.setObservationEnabled("strong_reversal",true);now=Base+21*Minute+1000;engine.beginLane(Identity,Minute,1,true);auto prior=input(20,now);engine.observeCoinFlowCalculated(prior,coinFlowStudies(prior,now),1);
   auto current=input(21,now);auto core=coinFlowStudies(current,now);auto grade=strong(core);grade.grade=quality;if(quality==ExtendedReversalGrade::Incomplete)grade.state.available=false;
   if(strongFirst)engine.observeStrongReversalCalculated(current,core,grade);engine.observeCoinFlowCalculated(current,core,1);if(!strongFirst)engine.observeStrongReversalCalculated(current,core,grade);QCOMPARE(sink.posted.size(),2);
   int originalReversals=0;for(const auto& notice:sink.posted){QVERIFY(!notice.title.contains("강한 반전"));if(notice.body.contains("반등 조건"))++originalReversals;}QCOMPARE(originalReversals,1);engine.observeStrongReversalCalculated(current,core,grade);QCOMPARE(sink.posted.size(),2);
  }
 }
 void invalidatedOrReplacedLaneDropsQueuedGrade(){
  for(bool replaced:{false,true}){qint64 now=Base+20*Minute;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});rule(engine);engine.setObservationEnabled("strong_reversal",true);now=Base+21*Minute+1000;engine.beginLane(Identity,Minute,1,true);auto prior=input(20,now);engine.observeCoinFlowCalculated(prior,coinFlowStudies(prior,now),1);auto current=input(21,now);auto core=coinFlowStudies(current,now);auto grade=strong(core);
   if(replaced)engine.beginLane(Identity,Minute,2,true);else engine.invalidateLane(Identity,Minute,1);
   engine.observeStrongReversalCalculated(current,core,grade,1);engine.observeCoinFlowCalculated(current,core,1);QVERIFY(sink.posted.isEmpty());
  }
 }
 void privateOnlyReversalKeepsEpochAndIsPurgedOnLock(){
  for(bool strongFirst:{true,false}){qint64 now=Base+20*Minute;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);engine.setPrivateEnabled(true);const auto epoch=engine.privateEpoch();engine.setHoldings({Identity},now,epoch);auto r=engine.rule("@holdings");r.enabled=true;r.pullback=r.results=false;r.whales=true;r.cooldownMs=0;QVERIFY(engine.setRule(r));engine.setObservationEnabled("strong_reversal",true);
   now=Base+21*Minute+1000;engine.beginLane(Identity,Minute,1,true);auto prior=input(20,now);engine.observeCoinFlowCalculated(prior,coinFlowStudies(prior,now),1);auto current=input(21,now);auto core=coinFlowStudies(current,now);auto grade=strong(core);
   if(strongFirst)engine.observeStrongReversalCalculated(current,core,grade,1);engine.observeCoinFlowCalculated(current,core,1);if(!strongFirst)engine.observeStrongReversalCalculated(current,core,grade,1);QCOMPARE(sink.posted.size(),2);
   for(const auto& notice:sink.posted){QVERIFY(notice.privateData);QCOMPARE(notice.privateEpoch,epoch);QVERIFY(!notice.title.contains("강한 반전"));QVERIFY(!notice.playSound);}
   engine.clearPrivate();for(const auto& notice:sink.posted)QVERIFY(sink.removed.contains(notice.identifier));engine.observeStrongReversalCalculated(current,core,grade,1);QCOMPARE(sink.posted.size(),2);
  }
 }
 void missingLateAndHistoryCannotNotify(){
  for(const auto fault:{QString("off"),QString("history"),QString("late"),QString("missing"),QString("basic"),QString("wrong-source")}){qint64 now=Base+20*Minute;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});if(fault!="off")engine.setObservationEnabled("strong_reversal",true);now=Base+21*Minute+1000;auto current=input(21,now);auto core=coinFlowStudies(current,now);auto grade=strong(core);
   if(fault=="history")current.minutes.last().liveEligible=false;if(fault=="late")grade.knownAtMs=current.minutes.last().decisionAtMs+1;if(fault=="missing")grade.state.available=false;if(fault=="basic")grade.grade=ExtendedReversalGrade::Basic;if(fault=="wrong-source")current.minutes.last().evidence.sourceUrl="https://example.invalid";
   engine.observeStrongReversalCalculated(current,core,grade);QVERIFY2(sink.posted.isEmpty(),qPrintable(fault));}
 }
};
QTEST_GUILESS_MAIN(ChartStrongObservationsTest)
#include "tst_chart_strong_observations.moc"
