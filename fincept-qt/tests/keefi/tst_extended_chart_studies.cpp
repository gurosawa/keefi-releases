#include "keefi/ExtendedChartStudies.h"
#include "keefi/StockChartTypes.h"
#include <QTest>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
using namespace keefi;
namespace {
constexpr qint64 Base=1728000000000;
QList<MarketCandle> rows(const QList<double>& closes,qint64 interval=60000) {
    QList<MarketCandle> result;
    for(int i=0;i<closes.size();++i){const auto value=QString::number(closes[i],'g',16);
        result.append({Base+i*interval,Base+i*interval+1000,value,value,value,value,"1"});}
    return result;
}
QList<MarketCandle> flat(int n,double close=10,qint64 interval=60000){return rows(QList<double>(n,close),interval);}
ExtendedChartOptions enabled(std::initializer_list<QString> ids){ExtendedChartOptions o;for(const auto& id:ids)o.enabled.insert(id);return o;}
bool near(double a,double b){return std::abs(a-b)<=1e-9+1e-10*std::abs(b);}
ExtendedLevelCandidate level(const QString& price,const QString& id,ExtendedLevelPeriod p=ExtendedLevelPeriod::Week){return {id,id,price,"fixture",p,Base,Base+60000,Base+60000,true};}
}
class ExtendedChartStudiesTest final:public QObject {
    Q_OBJECT
private slots:
    void exportSavedRecordResult();
    void defaultsAreOff();
    void movingAverageFixedVectors();
    void movingAverageDirections();
    void bollingerPopulationVariance();
    void rsiFlatAndBoundaries();
    void macdSmaSeeds();
    void invalidAndGapInputs();
    void displayWarmupBoundaries_data();
    void displayWarmupBoundaries();
    void displayFourLinesRespectDisabledSlots();
    void displayGapsRestartWarmupWithoutJoining();
    void displayStockTimelineHonorsVerifiedContinuity();
    void stockCloseDisplayKeepsRestrictionSensitiveSignalsSeparate();
    void displayHistoryNeverCreatesEventsOrBroadensObservationInput();
    void levelsAreCompletedBoundedAndExact();
    void amountDeltaBoundariesAndEpochs();
    void compressionIndependentAvailability();
    void breadthRelativeAndTrendVectors();
    void stockTimelineAndExactTarget();
    void strongReversalRequiresFiveMinuteData();
    void strongReversalHistoryPreservesGradesAndExactOutcomes();
    void strongReversalHistoryRejectsAmbiguousAndFutureEvidence();
    void strongReversalFirstGradeSurvivesRawRetentionWithoutResurrectingEvents();
    void lowVolumeZonesUseOnlyObservedData();
    void breakoutUsesPrefixesAndExactPriceBoundaries();
    void basicScorecardsRespectOptInAndTransitions();
    void basicScorecardVectorsAndPrefixInvariance();
    void basicScorecardsSeparateDirectionMinimumAndGaps();
    void basicScorecardsKeepExactStockTarget();
};
void ExtendedChartStudiesTest::defaultsAreOff(){const auto r=extendedChartStudies(flat(80),60000);QVERIFY(r.available);QVERIFY(r.maLines.isEmpty());QVERIFY(r.rsi.points.isEmpty());QVERIFY(r.macd.line.isEmpty());QVERIFY(r.bb.points.isEmpty());}
void ExtendedChartStudiesTest::movingAverageFixedVectors(){auto o=enabled({"ma"});o.movingAverages={{"s",3,ExtendedMaKind::Simple},{"e",3,ExtendedMaKind::Exponential},{"w",3,ExtendedMaKind::Weighted}};const auto r=extendedChartStudies(rows({1,2,3,4}),60000,o);QVERIFY(r.available);QCOMPARE(r.maLines.size(),3);QVERIFY(near(r.maLines[0].points.last().price,3));QVERIFY(near(r.maLines[1].points.first().price,2));QVERIFY(near(r.maLines[1].points.last().price,3));QVERIFY(near(r.maLines[2].points.last().price,20.0/6));QVERIFY(!extendedChartStudies(rows({1,2}),60000,o).movingAverageState.available);}
void ExtendedChartStudiesTest::movingAverageDirections(){QCOMPARE(extendedMovingAverageDirection(11,{9,10}),IndicatorDirection::Up);QCOMPARE(extendedMovingAverageDirection(8,{9,10}),IndicatorDirection::Down);QCOMPARE(extendedMovingAverageDirection(10,{9,11}),IndicatorDirection::Mix);QCOMPARE(extendedMovingAverageDirection(10,{10,10}),IndicatorDirection::Info);QCOMPARE(extendedMovingAverageDirection(11,{10,std::nullopt}),IndicatorDirection::None);QCOMPARE(extendedMovingAverageDirection(10,{9,10}),IndicatorDirection::Info);QCOMPARE(extendedMovingAverageDirection(10,{}),IndicatorDirection::None);}
void ExtendedChartStudiesTest::bollingerPopulationVariance(){auto o=enabled({"bb"});o.bandPeriod=3;const auto r=extendedChartStudies(rows({1,2,3}),60000,o);QVERIFY(r.bb.state.available);const auto b=r.bb.points.last();QVERIFY(near(b.middle,2));QVERIFY(near(b.lower,2-2*std::sqrt(2.0/3)));QVERIFY(near(b.upper,2+2*std::sqrt(2.0/3)));QCOMPARE(r.bb.state.direction,IndicatorDirection::Info);const auto f=extendedChartStudies(flat(20),60000,enabled({"bb"}));QCOMPARE(f.bb.points.last().upper,10.0);}
void ExtendedChartStudiesTest::rsiFlatAndBoundaries(){auto o=enabled({"rsi"});auto r=extendedChartStudies(flat(15),60000,o);QVERIFY(r.rsi.state.available);QCOMPARE(r.rsi.points.last().price,50.0);QCOMPARE(r.rsi.state.direction,IndicatorDirection::Info);QList<double> up,down;for(int i=0;i<15;++i){up.append(i+1);down.append(16-i);}QCOMPARE(extendedChartStudies(rows(up),60000,o).rsi.points.last().price,100.0);QCOMPARE(extendedChartStudies(rows(down),60000,o).rsi.points.last().price,0.0);QVERIFY(extendedChartStudies(flat(14),60000,o).rsi.state.reason.contains("14/15"));}
void ExtendedChartStudiesTest::macdSmaSeeds(){auto o=enabled({"macd"});o.macdFast=2;o.macdSlow=3;o.macdSignal=2;const auto r=extendedChartStudies(rows({1,2,3,5}),60000,o);QVERIFY(r.macd.state.available);QVERIFY(near(r.macd.line.last().price,2.0/3));QVERIFY(near(r.macd.signal.last().price,7.0/12));QVERIFY(near(r.macd.histogram.last().price,1.0/12));QCOMPARE(r.macd.state.direction,IndicatorDirection::Up);QVERIFY(extendedChartStudies(flat(33),60000,enabled({"macd"})).macd.state.reason.contains("33/34"));QCOMPARE(extendedChartStudies(flat(34),60000,enabled({"macd"})).macd.state.direction,IndicatorDirection::Info);}
void ExtendedChartStudiesTest::invalidAndGapInputs(){auto r=flat(20);r[10].close="nan";QVERIFY(!extendedChartStudies(r,60000,enabled({"rsi"})).available);QVERIFY(!extendedChartStudies(flat(201),60000,enabled({"rsi"})).available);r=flat(30);r.removeAt(20);const auto g=extendedChartStudies(r,60000,enabled({"rsi"}));QVERIFY(g.available);QCOMPARE(g.currentRunBars,9);QVERIFY(!g.rsi.state.available);QVERIFY(g.rsi.points.isEmpty());}
void ExtendedChartStudiesTest::levelsAreCompletedBoundedAndExact(){QList<ExtendedLevelCandidate> c;for(const auto& x:{"80","90","95","105","110","120"})c.append(level(x,x));auto r=chartLevels(c,"100","0.01",60000,Base+120000);QVERIFY(r.state.available);QCOMPARE(r.visible.size(),4);QCOMPARE(r.visible[0].price,QString("95"));QCOMPARE(r.visible[1].price,QString("90"));QCOMPARE(r.visible[2].price,QString("105"));QCOMPARE(r.visible[3].price,QString("110"));r=chartLevels({level("90","a"),level("90.05","b"),level("90.10","c")},"100","0.01",60000,Base+120000);QCOMPARE(r.all.size(),2);QCOMPARE(r.all[0].ids,QStringList({"a","b"}));QCOMPARE(r.all[1].price,QString("90.10"));auto unfinished=level("115","u");unfinished.complete=false;r=chartLevels({level("95","day",ExtendedLevelPeriod::Day),level("110","week"),unfinished},"100","0.01",86400000,Base+120000);QCOMPARE(r.visible.size(),1);QCOMPARE(r.visible.first().price,QString("110"));QVERIFY(chartLevels({level("100","same")},"100","0.01",60000,Base+120000).visible.isEmpty());}

namespace {
CoinFlowWindow flowWindow(int minutes=5){CoinFlowWindow w;w.instrument.verified=true;w.instrument.venue=CoinFlowVenue::BinanceSpot;w.instrument.grain=CoinFlowGrain::SpotAggregate;w.instrument.identity="coin:binance:SYNUSDT";w.instrument.symbol="SYNUSDT";w.instrument.baseAsset="SYN";w.instrument.quoteAsset="USDT";w.instrument.status="TRADING";w.periods.append({Base,Base+minutes*60000,Base+minutes*60000+1,1,CoinFlowCoverage::ObservedContinuous,{}});return w;}
CoinTradeObservation observedTrade(const QString& id,const QString& quantity,CoinFlowSide side=CoinFlowSide::Buy,qint64 at=Base+1000,const QString& price="1"){CoinTradeObservation t;t.id=id;t.price=price;t.quantity=quantity;t.side=side;t.tradeAtMs=at;t.eventAtMs=at+1;t.receivedAtMs=t.knownAtMs=at+2;t.epoch=1;t.liveEligible=true;return t;}
QJsonArray pricePoints(const QList<ChartPricePoint>& p){QJsonArray a;for(const auto& v:p)a.append(QJsonArray{v.atMs,v.price});return a;}
}
void ExtendedChartStudiesTest::exportSavedRecordResult(){
    const auto in=qEnvironmentVariable("KEEFI_EXTENDED_STUDIES_INPUT"),out=qEnvironmentVariable("KEEFI_EXTENDED_STUDIES_OUTPUT");
    if(in.isEmpty()&&out.isEmpty())QSKIP("Optional saved-public/synthetic cross-calculation seam");
    QVERIFY(!in.isEmpty());QVERIFY(!out.isEmpty());QFile file(in);QVERIFY(file.open(QIODevice::ReadOnly));QJsonParseError error;const auto document=QJsonDocument::fromJson(file.readAll(),&error);QCOMPARE(error.error,QJsonParseError::NoError);QVERIFY(document.isObject());const auto object=document.object();QList<MarketCandle> input;
    for(const auto& value:object.value("candles").toArray()){const auto r=value.toObject();input.append({r.value("startsAtMs").toInteger(),0,r.value("open").toString(),r.value("high").toString(),r.value("low").toString(),r.value("close").toString(),r.value("volume").toString()});}
    const auto result=extendedChartStudies(input,object.value("intervalMs").toInteger(),enabled({"ma","bb","rsi","macd","breakout","trend","low_volume"}));QVERIFY(result.available);QJsonArray ma,bb,events;
    for(const auto& m:result.maLines)ma.append(QJsonObject{{"id",m.spec.id},{"points",pricePoints(m.points)}});
    for(const auto& b:result.bb.points)bb.append(QJsonArray{b.atMs,b.lower,b.middle,b.upper});
    for(const auto& e:result.breakout.entries)events.append(QJsonObject{{"atMs",e.atMs},{"entryClose",e.entryClose},{"direction",int(e.direction)}});
    const QJsonObject canonical{{"observations",result.observations},{"currentRunBars",result.currentRunBars},{"lastCompletedAtMs",result.lastCompletedAtMs},{"ma",ma},{"bb",bb},{"rsi",pricePoints(result.rsi.points)},{"macdLine",pricePoints(result.macd.line)},{"macdSignal",pricePoints(result.macd.signal)},{"macdHistogram",pricePoints(result.macd.histogram)},{"breakoutEvents",events}};
    const auto bytes=QJsonDocument(canonical).toJson(QJsonDocument::Indented);QSaveFile target(out);QVERIFY(target.open(QIODevice::WriteOnly));QCOMPARE(target.write(bytes),qint64(bytes.size()));QVERIFY(target.commit());
}
void ExtendedChartStudiesTest::amountDeltaBoundariesAndEpochs(){
    auto w=flowWindow();w.trades={observedTrade("a","9999"),observedTrade("b","10000",CoinFlowSide::Sell),observedTrade("c","100000")};auto r=extendedAmountDelta(w,Base+300001);QVERIFY(r.state.available);QCOMPARE(r.buckets.size(),3);QCOMPARE(r.buckets[0].signedQuote,QString("9999"));QCOMPARE(r.buckets[1].signedQuote,QString("-10000"));QCOMPARE(r.buckets[2].signedQuote,QString("100000"));QCOMPARE(r.state.direction,IndicatorDirection::Mix);QCOMPARE(r.buckets[2].points.size(),5);
    w.trades.append(w.trades[1]);r=extendedAmountDelta(w,Base+300001);QCOMPARE(r.buckets[1].signedQuote,QString("-10000"));w.trades.last().quantity="10001";QVERIFY(!extendedAmountDelta(w,Base+300001).state.available);
    w=flowWindow();w.trades={observedTrade("boundary","100000",CoinFlowSide::Buy,Base+1000,"0.1")};r=extendedAmountDelta(w,Base+300001);QVERIFY(r.state.available);QCOMPARE(r.buckets[1].signedQuote,QString("10000"));QCOMPARE(r.buckets[0].signedQuote,QString("0"));
    w.periods[0].coverage=CoinFlowCoverage::Disconnected;QVERIFY(!extendedAmountDelta(w,Base+300001).state.available);w=flowWindow();w.trades={observedTrade("a","10000")};w.trades[0].knownAtMs=Base+400000;QVERIFY(!extendedAmountDelta(w,Base+300001).state.available);
    w=flowWindow();w.instrument.venue=CoinFlowVenue::BinanceUsdM;w.instrument.grain=CoinFlowGrain::Futures100msAggregate;w.instrument.marginAsset="BTC";QVERIFY(!extendedAmountDelta(w,Base+300001).state.available);
}
void ExtendedChartStudiesTest::compressionIndependentAvailability(){CandleCompression c;c.available=true;c.compressed=true;CoinOiStudy oi;auto r=extendedCompressionGauge(c,oi);QVERIFY(r.compressed.has_value());QVERIFY(*r.compressed);QVERIFY(!r.state.available);oi.state.available=true;oi.quantityChangePercent=0;r=extendedCompressionGauge(c,oi);QVERIFY(r.state.available);QCOMPARE(r.state.direction,IndicatorDirection::Info);QCOMPARE(*r.oiChangePercent,0.0);oi.quantityChangePercent=2;QCOMPARE(*extendedCompressionGauge(c,oi).oiChangePercent,2.0);}
void ExtendedChartStudiesTest::breadthRelativeAndTrendVectors(){auto b=extendedBreadth({{"a",1,true},{"b",-1,true},{"c",0,true},{"d",{},false},{"e",2,true}});QVERIFY(b.state.available);QCOMPARE(b.ready,4);QCOMPARE(b.population,5);QCOMPARE(*b.percent,50.0);QVERIFY(!extendedBreadth({{"a",1,true},{"b",-1,true},{"c",{},false}}).state.available);QVERIFY(!extendedBreadth({{"a",1,true},{"a",2,true}}).state.available);QVERIFY(!extendedBreadth({}).state.available);
    auto alt=flat(25,10,3600000),btc=flat(25,100,3600000);alt.last().open=alt.last().high=alt.last().low=alt.last().close="11";btc.last().open=btc.last().high=btc.last().low=btc.last().close="120";auto r=extendedRelativeStrength(alt,btc);QVERIFY(r.state.available);QVERIFY(near(*r.percent,-100.0/12));btc.removeAt(12);QVERIFY(!extendedRelativeStrength(alt,btc).state.available);
    QList<double> v;for(int i=1;i<=21;++i)v.append(i);QCOMPARE(extendedTrend(rows(v),60000).state.direction,IndicatorDirection::Up);QCOMPARE(extendedTrend(flat(21),60000).state.direction,IndicatorDirection::Info);QVERIFY(!extendedTrend(flat(20),60000).state.available);
}
void ExtendedChartStudiesTest::stockTimelineAndExactTarget(){auto r=flat(21,10,60000);ChartStudyTimeline t;t.adjustedComparable=true;for(int i=0;i<r.size();++i){if(i>=10)r[i].startsAtMs+=2*86400000;ChartStudyTimelineRow p;p.startsAtMs=r[i].startsAtMs;p.completesAtMs=p.startsAtMs+60000;p.eligible=true;p.connectsPrevious=i>0;t.rows.append(p);}auto result=extendedChartStudies(r,60000,enabled({"rsi","trend"}),t);QVERIFY(result.rsi.state.available);QCOMPARE(result.currentRunBars,21);QCOMPARE(result.trend.state.direction,IndicatorDirection::Info);t.rows[10].connectsPrevious=false;result=extendedChartStudies(r,60000,enabled({"rsi"}),t);QCOMPARE(result.currentRunBars,11);QVERIFY(!result.rsi.state.available);t.rows[10].connectsPrevious=true;t.rows[10].missingSlotsBefore=1;QVERIFY(!extendedChartStudies(r,60000,enabled({"rsi"}),t).available);}
void ExtendedChartStudiesTest::strongReversalRequiresFiveMinuteData(){
    CoinReversalStudy r;r.state.available=true;r.state.throughMs=Base+300000;r.state.knownAtMs=Base+300001;r.rebound=true;
    CoinVolumeStudy v;v.state.available=true;v.state.throughMs=r.state.throughMs;v.state.knownAtMs=r.state.knownAtMs;v.multiple=2;
    auto w=flowWindow();w.trades={observedTrade("buy","6"),observedTrade("sell","4",CoinFlowSide::Sell)};
    auto d=extendedAmountDelta(w,Base+300001);auto result=extendedStrongReversal(r,v,d);
    QVERIFY(result.state.available);QCOMPARE(result.grade,ExtendedReversalGrade::Strong);
    QCOMPARE(result.throughMs,r.state.throughMs);QCOMPARE(result.knownAtMs,r.state.knownAtMs);
    ++d.knownAtMs;QVERIFY(!extendedStrongReversal(r,v,d).state.available); // future corroboration cannot upgrade the past
    result=extendedStrongReversal(r,v,d,d.knownAtMs);QVERIFY(result.state.available);QCOMPARE(result.knownAtMs,d.knownAtMs);
    --d.knownAtMs;++v.state.knownAtMs;QVERIFY(!extendedStrongReversal(r,v,d).state.available);--v.state.knownAtMs;
    d.throughMs-=60000;QVERIFY(!extendedStrongReversal(r,v,d).state.available);
    d=extendedAmountDelta(w,Base+300001);v.multiple=1;QCOMPARE(extendedStrongReversal(r,v,d).grade,ExtendedReversalGrade::Reinforced);
    d.state.available=false;QCOMPARE(extendedStrongReversal(r,v,d).grade,ExtendedReversalGrade::Incomplete);
    r.rebound=false;QCOMPARE(extendedStrongReversal(r,v,d).grade,ExtendedReversalGrade::None);
}
namespace {
CoinFlowStudySet reversalHistory(){
    CoinFlowStudySet core;core.available=true;core.identity="coin:binance:SYNUSDT";core.asOfMs=core.lastCompletedAtMs=Base+20*3600000;
    for(int grade=1;grade<=4;++grade)for(bool up:{true,false}){
        const qint64 at=Base+(grade*2+(up?0:1))*3600000,decision=at+1000;
        CoinFlowSignal entry;entry.kind=up?CoinFlowSignalKind::Rebound:CoinFlowSignalKind::TurnDown;
        entry.direction=up?CoinFlowDirection::Up:CoinFlowDirection::Down;entry.atMs=at;entry.knownAtMs=decision;entry.entryClose="100";
        entry.state.available=true;entry.state.fromMs=at-4*60000;entry.state.throughMs=at;entry.state.knownAtMs=decision;
        CoinReversalCorroboration evidence;evidence.volume.state=entry.state;evidence.volume.multiple=grade==1?1:2;
        evidence.spot.state=entry.state;evidence.spot.state.fromMs=at-5*60000;evidence.spot.signedFraction=grade==3?(up?.2:-.2):0;
        evidence.spot.state.available=grade!=4;entry.reversalCorroboration=evidence;core.entries.append(entry);
        core.groups.append({entry.kind,entry.direction,at,at,decision,"100",1,true});
        core.scorecard.outcomes.append({entry.kind,at,at+3600000,at+3600001,up?2.0:-3.0});
    }
    return core;
}
}
void ExtendedChartStudiesTest::strongReversalHistoryPreservesGradesAndExactOutcomes(){
    auto core=reversalHistory();auto result=extendedStrongReversal(core);QCOMPARE(result.horizonMs,3600000);
    QCOMPARE(result.events.size(),8);
    for(int i=0;i<8;++i){const auto& event=result.events[i];QCOMPARE(int(event.grade),i/2+1);QVERIFY(event.outcome);
        QCOMPARE(event.outcome->evaluationAtMs,event.signalAtMs+3600000);QCOMPARE(event.decisionAtMs,event.signalAtMs+1000);}
    // The latest volume/five-minute flow is deliberately unrelated to old events.
    core.volume.multiple=999;core.taker.signedFraction=-1;
    core.entries.last().atMs+=3600000; // Missing first entry remains Incomplete, not Basic.
    result=extendedStrongReversal(core);QCOMPARE(result.events.first().grade,ExtendedReversalGrade::Basic);
    QCOMPARE(result.events[4].grade,ExtendedReversalGrade::Strong);QCOMPARE(result.events.last().grade,ExtendedReversalGrade::Incomplete);
    core=reversalHistory();core.scorecard.outcomes[0].evaluationAtMs+=60000;
    core.scorecard.outcomes[1].knownAtMs=core.asOfMs+1;core.scorecard.outcomes.removeAt(2);
    result=extendedStrongReversal(core);QVERIFY(!result.events[0].outcome);QVERIFY(!result.events[1].outcome);QVERIFY(!result.events[2].outcome);
    QVERIFY(result.events[3].outcome);
    core.scorecard.horizonMs=60000;result=extendedStrongReversal(core);for(const auto& event:result.events)QVERIFY(!event.outcome);
}
void ExtendedChartStudiesTest::strongReversalHistoryRejectsAmbiguousAndFutureEvidence(){
    auto core=reversalHistory();
    auto& strong=core.entries[4];strong.reversalCorroboration->spot.state.knownAtMs=strong.knownAtMs+1;
    core.entries[5].reversalCorroboration->volume.state.knownAtMs=core.entries[5].knownAtMs+1;
    core.entries.append(core.entries.first());core.scorecard.outcomes.append(core.scorecard.outcomes[2]);
    core.groups.append(core.groups.last());
    const auto result=extendedStrongReversal(core);QCOMPARE(result.events.size(),8);
    QCOMPARE(result.events[0].grade,ExtendedReversalGrade::Incomplete);
    QCOMPARE(result.events[4].grade,ExtendedReversalGrade::Incomplete);QCOMPARE(result.events[5].grade,ExtendedReversalGrade::Incomplete);
    QVERIFY(!result.events[2].outcome);QVERIFY(result.events[3].outcome);
    core.asOfMs=1;QVERIFY(extendedStrongReversal(core).events.isEmpty());
}
void ExtendedChartStudiesTest::strongReversalFirstGradeSurvivesRawRetentionWithoutResurrectingEvents(){
    auto core=reversalHistory();const auto first=extendedStrongReversal(core);
    core.entries[4].reversalCorroboration.reset(); // Original strong event's raw trade ring was pruned.
    core.entries[6].reversalCorroboration->spot.state.available=true;
    core.entries[6].reversalCorroboration->spot.signedFraction=1; // Originally incomplete remains incomplete.
    core.scorecard.outcomes[4].returnPercent=7;core.scorecard.outcomes.removeAt(3);
    auto next=extendedStrongReversal(core,&first);QCOMPARE(next.events[4].grade,ExtendedReversalGrade::Strong);
    QCOMPARE(next.events[6].grade,ExtendedReversalGrade::Incomplete);QCOMPARE(next.events[4].outcome->returnPercent,7.0);
    QVERIFY(!next.events[3].outcome); // A previous outcome cannot replace missing current proof.
    core.groups.removeLast();next=extendedStrongReversal(core,&first);QCOMPARE(next.events.size(),7);
    core.identity="coin:binance:OTHERUSDT";next=extendedStrongReversal(core,&first);
    QCOMPARE(next.events[4].grade,ExtendedReversalGrade::Incomplete);QCOMPARE(next.events[6].grade,ExtendedReversalGrade::Strong);
}
void ExtendedChartStudiesTest::lowVolumeZonesUseOnlyObservedData(){auto candles=flat(21);for(auto& c:candles){c.low="100";c.high="120";c.open=c.close="110";}const auto r=extendedChartStudies(candles,60000,enabled({"low_volume"}));QVERIFY(r.lowVolumeZones.state.available);QCOMPARE(r.lowVolumeZones.zones.size(),2);QVERIFY(near(r.lowVolumeZones.zones[0].low,101));QVERIFY(near(r.lowVolumeZones.zones[0].high,110));QVERIFY(!extendedChartStudies(candles,3600000,enabled({"low_volume"})).available);
    auto w=flowWindow(60);w.trades={observedTrade("low","10",CoinFlowSide::Buy,Base+1000,"100"),observedTrade("high","10",CoinFlowSide::Buy,Base+2000,"120")};auto p=extendedTradeLowVolumeZones(w,Base+3600001);QVERIFY(p.state.available);QVERIFY(!p.candleApproximation);QCOMPARE(p.zones.size(),1);QVERIFY(near(p.zones[0].low,101));QVERIFY(near(p.zones[0].high,119));w.periods[0].coverage=CoinFlowCoverage::Partial;QVERIFY(!extendedTradeLowVolumeZones(w,Base+3600001).state.available);QVERIFY(!extendedTradeLowVolumeZones(flowWindow(5),Base+300001).state.available);}
void ExtendedChartStudiesTest::breakoutUsesPrefixesAndExactPriceBoundaries(){auto r=flat(100,100);for(auto& b:r){b.high="101";b.low="99";}r[21].open="100";r[21].high="103";r[21].low="99";r[21].close="102";r[21].volume="2";auto o=enabled({"breakout"});auto full=extendedChartStudies(r,60000,o);QVERIFY(full.breakout.entries.size()>=1);const auto first=full.breakout.entries.first();QCOMPARE(first.atMs,Base+22*60000);QCOMPARE(first.direction,IndicatorDirection::Up);QVERIFY(!first.quality.has_value());const auto prefix=extendedChartStudies(r.mid(0,22),60000,o);QCOMPARE(prefix.breakout.entries.size(),1);QCOMPARE(prefix.breakout.entries.first().atMs,first.atMs);QCOMPARE(full.breakout.scorecard.samples,1);QVERIFY(!full.breakout.scorecard.available);QVERIFY(!full.breakout.scorecard.meanReturnPercent.has_value());auto missingTarget=r;missingTarget.removeAt(81);QCOMPARE(extendedChartStudies(missingTarget,60000,o).breakout.scorecard.samples,0);
    auto tiny=flat(21,100);for(auto& b:tiny){b.high="100";b.low="99";}tiny.last().open=tiny.last().close=tiny.last().high="100.000000000000000001";QCOMPARE(extendedChartStudies(tiny,60000,o).breakout.state.direction,IndicatorDirection::Up);
}

void ExtendedChartStudiesTest::basicScorecardsRespectOptInAndTransitions(){
    auto o=enabled({"ma"});o.movingAverages={{"s",2,ExtendedMaKind::Simple}};
    const auto input=rows({100,101,102,102,103,101,101});
    QVERIFY(extendedChartStudies(input,60000,o).scorecards.isEmpty());
    o.eventStudies.insert("ma");const auto result=extendedChartStudies(input,60000,o);
    QVERIFY(result.scorecards.contains("ma"));const auto events=result.studyEntries.value("ma");
    QCOMPARE(events.size(),3);QCOMPARE(events[0].atMs,Base+2*60000);QCOMPARE(events[0].direction,IndicatorDirection::Up);
    QCOMPARE(events[1].atMs,Base+5*60000);QCOMPARE(events[1].direction,IndicatorDirection::Up);
    QCOMPARE(events[2].atMs,Base+6*60000);QCOMPARE(events[2].direction,IndicatorDirection::Down);
    QCOMPARE(result.scorecards["ma"].unresolvedSignals,3);QVERIFY(!result.scorecards["ma"].available);
    o.enabled.clear();QVERIFY(extendedChartStudies(input,60000,o).scorecards.isEmpty());
}
void ExtendedChartStudiesTest::basicScorecardVectorsAndPrefixInvariance(){
    struct Vector{QString id;QList<double> input;QList<int> eventIndices;QList<IndicatorDirection> directions;};
    const QList<Vector> vectors{{"bb",{100,100,101,102,101,100},{2,5},{IndicatorDirection::Up,IndicatorDirection::Down}},
        {"rsi",{100,101,102,101,100},{2,4},{IndicatorDirection::Up,IndicatorDirection::Down}},
        {"macd",{1,2,3,5,4},{3,4},{IndicatorDirection::Up,IndicatorDirection::Down}}};
    for(const auto& v:vectors){
        auto o=enabled({v.id});o.bandPeriod=3;o.bandDeviations=1;o.rsiPeriod=2;o.macdFast=2;o.macdSlow=3;o.macdSignal=2;
        const auto input=rows(v.input);const auto full=extendedChartStudies(input,60000,o);QVERIFY(full.scorecards.contains(v.id));
        const auto events=full.studyEntries.value(v.id);QCOMPARE(events.size(),v.eventIndices.size());
        for(int k=0;k<events.size();++k){QCOMPARE(events[k].atMs,Base+(v.eventIndices[k]+1)*60000);QCOMPARE(events[k].direction,v.directions[k]);
            const auto prefix=extendedChartStudies(input.mid(0,v.eventIndices[k]+1),60000,o).studyEntries.value(v.id);
            QVERIFY(!prefix.isEmpty());QCOMPARE(prefix.last().atMs,events[k].atMs);QCOMPARE(prefix.last().entryClose,events[k].entryClose);QCOMPARE(prefix.last().direction,events[k].direction);}
    }
}
void ExtendedChartStudiesTest::basicScorecardsSeparateDirectionMinimumAndGaps(){
    QList<double> values;for(int i=0;i<180;++i)values.append(i%2?101:100);const auto input=rows(values);
    auto o=enabled({"ma"});o.movingAverages={{"s",2,ExtendedMaKind::Simple}};o.eventStudies.insert("ma");
    auto score=extendedChartStudies(input.mid(0,99),60000,o).scorecards.value("ma");
    QCOMPARE(score.samples,38);QVERIFY(!score.available);QVERIFY(!score.meanReturnPercent);
    for(const auto& slice:score.slices){QCOMPARE(slice.samples,19);QVERIFY(!slice.available);QVERIFY(!slice.meanDirectionalReturnPercent);}
    score=extendedChartStudies(input.mid(0,101),60000,o).scorecards.value("ma");
    QCOMPARE(score.samples,40);QVERIFY(score.available);QCOMPARE(score.slices.size(),2);QVERIFY(!score.meanReturnPercent);
    for(const auto& slice:score.slices){QCOMPARE(slice.samples,20);QVERIFY(slice.available);QCOMPARE(*slice.meanDirectionalReturnPercent,0.0);}
    QCOMPARE(score.unresolvedSignals,60);QCOMPARE(score.overlappingSignals,99);QVERIFY(score.costsExcluded);
    auto gap=input;gap.removeAt(75);score=extendedChartStudies(gap,60000,o).scorecards.value("ma");QCOMPARE(score.samples,57);
    for(const auto& outcome:score.outcomes)QVERIFY(!(outcome.signalAtMs<=Base+75*60000&&outcome.evaluationAtMs>=Base+76*60000));
    auto exact=input;exact.removeAt(61);score=extendedChartStudies(exact,60000,o).scorecards.value("ma");
    QVERIFY(std::none_of(score.outcomes.cbegin(),score.outcomes.cend(),[](const auto& outcome){return outcome.signalAtMs==Base+2*60000;}));
}
void ExtendedChartStudiesTest::basicScorecardsKeepExactStockTarget(){
    QList<double> values;for(int i=0;i<40;++i)values.append(i%2?101:100);auto input=rows(values);
    ChartStudyTimeline timeline;timeline.adjustedComparable=true;
    for(int i=0;i<input.size();++i){if(i>=10)input[i].startsAtMs+=2*86400000;ChartStudyTimelineRow row;row.startsAtMs=input[i].startsAtMs;row.completesAtMs=row.startsAtMs+60000;row.eligible=true;row.connectsPrevious=i>0;timeline.rows.append(row);}
    auto o=enabled({"ma"});o.movingAverages={{"s",2,ExtendedMaKind::Simple}};o.eventStudies.insert("ma");
    const auto result=extendedChartStudies(input,60000,o,timeline);QCOMPARE(result.studyEntries.value("ma").size(),39);
    const auto score=result.scorecards.value("ma");QCOMPARE(score.samples,0);QCOMPARE(score.unresolvedSignals,39);QCOMPARE(score.horizonMs,60000*60);
    timeline.adjustedComparable=false;QCOMPARE(extendedChartStudies(input,60000,o,timeline).scorecards.value("ma").samples,0);
}

void ExtendedChartStudiesTest::displayWarmupBoundaries_data(){
    QTest::addColumn<int>("count");QTest::addColumn<int>("validPoints");QTest::addColumn<bool>("drawable");
    QTest::newRow("199-completed")<<199<<0<<false;
    QTest::newRow("200-completed")<<200<<1<<false;
    QTest::newRow("201-completed")<<201<<2<<true;
}
void ExtendedChartStudiesTest::displayWarmupBoundaries(){
    QFETCH(int,count);QFETCH(int,validPoints);QFETCH(bool,drawable);
    auto o=enabled({"ma","bb"});o.movingAverages={{"ema200",200,ExtendedMaKind::Exponential}};
    QList<double> values;for(int i=1;i<=count;++i)values.append(i);
    const auto result=extendedChartDisplayStudies(rows(values),60000,o);
    QVERIFY(result.available);QCOMPARE(result.observations,count);QCOMPARE(result.currentRunBars,count);
    QCOMPARE(result.maLines.size(),1);QCOMPARE(result.maLines[0].points.size(),validPoints);QCOMPARE(result.bb.points.size(),validPoints);
    QCOMPARE(result.maLines[0].state.available,drawable);QCOMPARE(result.movingAverageState.available,drawable);QCOMPARE(result.bb.state.available,drawable);
    if(!drawable){QVERIFY(!result.maLines[0].state.reason.isEmpty());QVERIFY(!result.bb.state.reason.isEmpty());}
    if(validPoints){
        const auto& ma=result.maLines[0].points;QVERIFY(near(ma.first().price,100.5));QCOMPARE(ma.first().atMs,Base+199*60000);
        const auto& first=result.bb.points.first();QVERIFY(near(first.middle,100.5));
        const double sigma=std::sqrt((200.0*200.0-1)/12);
        QVERIFY(near(first.upper,100.5+2*sigma));QVERIFY(near(first.lower,100.5-2*sigma));
        if(drawable){QVERIFY(near(ma.last().price,101.5));QVERIFY(near(result.bb.points.last().middle,101.5));QCOMPARE(ma.last().atMs-ma.first().atMs,qint64(60000));}
    }
}
void ExtendedChartStudiesTest::displayFourLinesRespectDisabledSlots(){
    auto o=enabled({"ma"});o.movingAverages=defaultChartMovingAverages();
    auto result=extendedChartDisplayStudies(flat(201),60000,o);QVERIFY(result.available);QCOMPARE(result.maLines.size(),4);QVERIFY(result.movingAverageState.available);
    const QList<int> counts{181,152,102,2};for(int i=0;i<4;++i){QCOMPARE(result.maLines[i].points.size(),counts[i]);QCOMPARE(result.maLines[i].points.last().price,10.0);}
    o.movingAverages[3].enabled=false;
    result=extendedChartDisplayStudies(flat(121),60000,o);QCOMPARE(result.maLines.size(),3);QVERIFY(result.movingAverageState.available);
    for(auto& spec:o.movingAverages)spec.enabled=false;
    result=extendedChartDisplayStudies(flat(201),60000,o);QVERIFY(result.available);QVERIFY(result.maLines.isEmpty());QVERIFY(!result.movingAverageState.available);QVERIFY(result.studyEntries.isEmpty());
    o.movingAverages[0]={"sma",200,ExtendedMaKind::Simple,true};o.movingAverages[1]={"wma",200,ExtendedMaKind::Weighted,true};
    QList<double> values;for(int i=1;i<=201;++i)values.append(i);
    result=extendedChartDisplayStudies(rows(values),60000,o);QCOMPARE(result.maLines.size(),2);
    QVERIFY(near(result.maLines[0].points.last().price,101.5));QVERIFY(near(result.maLines[1].points.first().price,401.0/3));
    QVERIFY(near(result.maLines[1].points.last().price,404.0/3));
    o.movingAverages[0].period=201;QVERIFY(!extendedChartDisplayStudies(flat(201),60000,o).available);
}
void ExtendedChartStudiesTest::displayGapsRestartWarmupWithoutJoining(){
    auto o=enabled({"ma","bb"});o.movingAverages={{"ema200",200,ExtendedMaKind::Exponential}};
    for(int suffix:{199,200,201}){
        auto input=flat(202+suffix);input.removeAt(201);
        const auto result=extendedChartDisplayStudies(input,60000,o);QVERIFY(result.available);QCOMPARE(result.currentRunBars,suffix);
        const auto expected=qMax(0,suffix-199);QCOMPARE(result.maLines[0].points.size(),expected);QCOMPARE(result.bb.points.size(),expected);
        QCOMPARE(result.bb.state.available,suffix==201);QCOMPARE(result.maLines[0].state.available,suffix==201);
        for(const auto& point:result.maLines[0].points)QVERIFY(point.atMs>=Base+(202+199)*60000);
        for(const auto& point:result.bb.points)QVERIFY(point.atMs>=Base+(202+199)*60000);
    }
    auto duplicate=flat(201);duplicate.insert(100,duplicate[100]);QVERIFY(!extendedChartDisplayStudies(duplicate,60000,o).available);
    auto reversed=flat(201);std::swap(reversed[100],reversed[101]);QVERIFY(!extendedChartDisplayStudies(reversed,60000,o).available);
    QVERIFY(extendedChartDisplayStudies(flat(MarketCandles::HistoryLimit),60000,o).available);
    QVERIFY(!extendedChartDisplayStudies(flat(MarketCandles::HistoryLimit+1),60000,o).available);
}
void ExtendedChartStudiesTest::stockCloseDisplayKeepsRestrictionSensitiveSignalsSeparate(){
    const auto input=flat(120);
    ChartStudyTimeline timeline;
    timeline.adjustedComparable=true;
    for(int i=0;i<input.size();++i){
        ChartStudyTimelineRow row;
        row.startsAtMs=input[i].startsAtMs;
        row.completesAtMs=row.startsAtMs+60000;
        row.connectsPrevious=i>0;
        row.exclusions={StockBarExclusion::RestrictionCoverageUnknown};
        timeline.rows.append(row);
    }
    auto options=enabled({"ma","bb","rsi","macd","breakout","trend"});
    options.eventStudies.insert("ma");
    options.displayBandPeriod=20;
    const auto display=extendedChartDisplayStudies(input,60000,options,timeline);
    QVERIFY(display.movingAverageState.available && display.bb.state.available);
    QVERIFY(display.rsi.state.available && display.macd.state.available);
    QCOMPARE(display.rsi.points.last().atMs,input.last().startsAtMs);
    QCOMPARE(display.macd.signal.last().atMs,input.last().startsAtMs);
    QVERIFY(display.studyEntries.isEmpty() && display.scorecards.isEmpty());
    QVERIFY(!display.breakout.state.available && !display.trend.state.available);
    const auto assessed=extendedChartStudies(input,60000,options,timeline);
    QVERIFY(!assessed.movingAverageState.available && !assessed.bb.state.available);
    QVERIFY(!assessed.rsi.state.available && !assessed.macd.state.available);
    QVERIFY(!assessed.breakout.state.available && !assessed.trend.state.available);
    for(const auto& entries:assessed.studyEntries) QVERIFY(entries.isEmpty());
    for(const auto& score:assessed.scorecards) QCOMPARE(score.samples,0);
    for(auto reason:{StockBarExclusion::VolatilityInterruption,StockBarExclusion::PriceLimit,
                     StockBarExclusion::MixedSession,StockBarExclusion::AdjustmentUnknown,StockBarExclusion::InvalidPriceLimit}){
        auto blocked=timeline;
        blocked.rows[115].exclusions.append(reason);
        const auto recent=extendedChartDisplayStudies(input,60000,options,blocked);
        QCOMPARE(recent.currentRunBars,4);
        QVERIFY(!recent.movingAverageState.available && !recent.bb.state.available);
        QVERIFY(!recent.rsi.state.available && !recent.macd.state.available);
    }
    timeline.rows[115].connectsPrevious=false;
    QCOMPARE(extendedChartDisplayStudies(input,60000,options,timeline).currentRunBars,5);
}
void ExtendedChartStudiesTest::displayStockTimelineHonorsVerifiedContinuity(){
    auto input=flat(201);ChartStudyTimeline timeline;
    for(int i=0;i<input.size();++i){
        if(i>=100)input[i].startsAtMs+=2*86400000;
        ChartStudyTimelineRow row;row.startsAtMs=input[i].startsAtMs;row.completesAtMs=row.startsAtMs+60000;row.eligible=true;row.connectsPrevious=i>0;timeline.rows.append(row);
    }
    auto o=enabled({"ma","bb"});o.movingAverages={{"ema200",200,ExtendedMaKind::Exponential}};
    auto result=extendedChartDisplayStudies(input,60000,o,timeline);QVERIFY(result.bb.state.available);QCOMPARE(result.bb.points.size(),2);
    QVERIFY(!extendedChartDisplayStudies(input,60000,o).bb.state.available);
    timeline.rows[100].connectsPrevious=false;
    result=extendedChartDisplayStudies(input,60000,o,timeline);QCOMPARE(result.currentRunBars,101);QVERIFY(result.bb.points.isEmpty());
    timeline.rows[100].eligible=false;result=extendedChartDisplayStudies(input,60000,o,timeline);QCOMPARE(result.currentRunBars,100);QVERIFY(result.maLines[0].points.isEmpty());
    timeline.rows.removeLast();QVERIFY(!extendedChartDisplayStudies(input,60000,o,timeline).available);
}
void ExtendedChartStudiesTest::displayHistoryNeverCreatesEventsOrBroadensObservationInput(){
    QList<double> values;for(int i=1;i<=400;++i)values.append(i);
    const auto history=rows(values),observation=history.last(200);
    auto o=enabled({"ma","bb","rsi","macd","breakout"});o.eventStudies.insert("ma");o.movingAverages=defaultChartMovingAverages();
    const auto before=extendedChartStudies(observation,60000,o);
    QVERIFY(before.available);QCOMPARE(before.observations,200);QCOMPARE(o.bandPeriod,20);
    const auto display=extendedChartDisplayStudies(history,60000,o);
    QVERIFY(display.available);QCOMPARE(display.observations,400);QVERIFY(display.bb.state.available);
    QVERIFY(display.studyEntries.isEmpty());QVERIFY(display.scorecards.isEmpty());QVERIFY(display.breakout.entries.isEmpty());
    QVERIFY(display.rsi.points.isEmpty());QVERIFY(display.macd.line.isEmpty());
    QVERIFY(near(before.bb.points.last().middle,390.5));QVERIFY(near(display.bb.points.last().middle,300.5));
    QVERIFY(!extendedChartStudies(history,60000,o).available);
    const auto after=extendedChartStudies(observation,60000,o);
    QCOMPARE(after.observations,before.observations);QCOMPARE(after.lastCompletedAtMs,before.lastCompletedAtMs);QCOMPARE(after.studyEntries.keys(),before.studyEntries.keys());
    for(const auto& id:before.studyEntries.keys()){
        const auto a=before.studyEntries.value(id),b=after.studyEntries.value(id);QCOMPARE(a.size(),b.size());
        for(int i=0;i<a.size();++i){QCOMPARE(a[i].atMs,b[i].atMs);QCOMPARE(a[i].direction,b[i].direction);QCOMPARE(a[i].entryClose,b[i].entryClose);}
        QCOMPARE(after.scorecards.value(id).samples,before.scorecards.value(id).samples);QCOMPARE(after.scorecards.value(id).unresolvedSignals,before.scorecards.value(id).unresolvedSignals);
    }
    const auto internal=candleCompression(observation,60000);QVERIFY(internal.available);
    const auto current=observation.last().close.toDouble(),center=390.5,sigma=std::sqrt((20.0*20.0-1)/12);
    Q_UNUSED(current);QVERIFY(near(internal.bandHigh,center+2*sigma));QVERIFY(near(internal.bandLow,center-2*sigma));
}

QTEST_APPLESS_MAIN(ExtendedChartStudiesTest)
#include "tst_extended_chart_studies.moc"
