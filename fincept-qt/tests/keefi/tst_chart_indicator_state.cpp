#include <QtTest>
#include "keefi/ChartIndicatorState.h"
#include "keefi/ChartStudies.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include <algorithm>
using namespace keefi;
namespace {
ChartIndicatorState state(QString id, IndicatorDirection direction, QString group="price-volume") {
    ChartIndicatorState s; s.id=id; s.context="coin:upbit:KRW-BTC/3600000"; s.revision=7;
    s.requiredReady=direction!=IndicatorDirection::None;
    s.atoms={{group,direction,s.requiredReady,{}}}; return s;
}
ChartIndicatorSummary summary(const QList<ChartIndicatorState>& states) {
    return summarizeChartIndicators(states,"coin:upbit:KRW-BTC/3600000",7);
}
}
class ChartIndicatorStateTest final:public QObject {
    Q_OBJECT
private slots:
    void sameMaterialVotesOnce() {
        auto s=summary({state("cloud",IndicatorDirection::Up),state("macd",IndicatorDirection::Down),
            state("ma",IndicatorDirection::Up),state("whales",IndicatorDirection::None,"spot:upbit")});
        QCOMPARE(s.mix,1); QCOMPARE(s.up,0); QCOMPARE(s.down,0); QCOMPARE(s.none,1);
        QCOMPARE(s.ready,3); QCOMPARE(s.enabled,4); QCOMPARE(s.completenessPercent,std::optional<int>(75));
    }
    void hiddenCalculatesWhileOffExcludes() {
        auto cloud=state("cloud",IndicatorDirection::Up), macd=state("macd",IndicatorDirection::Down);
        auto ma=state("ma",IndicatorDirection::Up), whales=state("whales",IndicatorDirection::None,"spot:upbit");
        macd.hidden=true; QCOMPARE(summary({cloud,macd,ma,whales}).mix,1);
        macd.enabled=false; auto s=summary({cloud,macd,ma,whales});
        QCOMPARE(s.up,1); QCOMPARE(s.mix,0); QCOMPARE(s.enabled,3);
        QCOMPARE(s.completenessPercent,std::optional<int>(67));
    }
    void informationIsValidAndDistinctFromMissing() {
        auto s=summary({state("bb",IndicatorDirection::Info),state("pullback",IndicatorDirection::Info),
            state("oi",IndicatorDirection::Info,"contract")});
        QCOMPARE(s.info,2); QCOMPARE(s.none,0); QCOMPARE(s.ready,3);
        QCOMPARE(s.completenessPercent,std::optional<int>(100));
    }
    void partialGroupDoesNotPretendCompositeIsComplete() {
        auto s=summary({state("cloud",IndicatorDirection::Up),state("bb",IndicatorDirection::None)});
        QCOMPARE(s.up,1); QCOMPARE(s.none,0); QVERIFY(s.groups.first().partial);
        QCOMPARE(s.completenessPercent,std::optional<int>(50));
        auto whales=state("whales",IndicatorDirection::None);
        whales.atoms={{"price-volume",IndicatorDirection::Up,true,{}},{"spot:upbit",IndicatorDirection::None,false,"받는 중"}};
        s=summary({whales}); QCOMPARE(s.up,1); QCOMPARE(s.none,1); QCOMPARE(s.ready,0); QCOMPARE(s.enabled,1);
        whales.atoms[1]={"spot:upbit",IndicatorDirection::Info,true,{}};whales.requiredReady=true;
        s=summary({whales}); QCOMPARE(s.up,1); QCOMPARE(s.info,1); QCOMPARE(s.ready,1);
        // OI is intentionally absent; it is not required for a spot composite.
        whales.enabled=false; QCOMPARE(summary({whales}).enabled,0);
    }
    void historicalSimilarityNeverChangesCurrentDirectionOrCompleteness(){
        const auto cloud=state("cloud",IndicatorDirection::Up);
        for(const auto direction:{IndicatorDirection::None,IndicatorDirection::Info,IndicatorDirection::Down})for(bool hidden:{false,true}){
            auto historical=state("similarity",direction);historical.hidden=hidden;
            const auto combined=summary({cloud,historical});QCOMPARE(combined.enabled,1);QCOMPARE(combined.ready,1);
            QCOMPARE(combined.up,1);QCOMPARE(combined.mix,0);QCOMPARE(combined.none,0);QCOMPARE(combined.completenessPercent,std::optional<int>(100));
            QCOMPARE(combined.groups.size(),1);QVERIFY(!combined.groups.first().partial);QCOMPARE(combined.groups.first().indicatorIds,QStringList{"cloud"});
            const auto alone=summary({historical});QCOMPARE(alone.enabled,0);QVERIFY(alone.groups.isEmpty());QVERIFY(!alone.completenessPercent);
        }
    }
    void emptyAndStaleAreNeverNeutralVotes() {
        auto s=summary({}); QVERIFY(!s.completenessPercent); QCOMPARE(s.info,0);
        auto cloud=state("cloud",IndicatorDirection::Up); cloud.requiredReady=false;
        cloud.atoms[0].ready=false; cloud.atoms[0].reason="지연";
        s=summary({cloud,state("rsi",IndicatorDirection::None)});
        QCOMPARE(s.none,1); QCOMPARE(s.mix,0); QCOMPARE(s.info,0); QCOMPARE(s.ready,0);
        QCOMPARE(s.completenessPercent,std::optional<int>(0));
    }
    void immutableContextAndOrder() {
        auto cloud=state("cloud",IndicatorDirection::Up), macd=state("macd",IndicatorDirection::Down);
        auto stale=cloud;stale.id="rsi";stale.revision=6;
        auto other=cloud;other.id="ma";other.context="coin:upbit:KRW-ETH/3600000";
        auto visual=cloud;visual.id="volume";visual.stateBearing=false;
        QList<ChartIndicatorState> rows{cloud,macd,cloud,stale,other,visual};
        auto s=summary(rows); QCOMPARE(s.enabled,2); QCOMPARE(s.mix,1);
        std::reverse(rows.begin(),rows.end());auto reversed=summary(rows);
        QCOMPARE(reversed.enabled,s.enabled); QCOMPARE(reversed.mix,s.mix);
        QCOMPARE(reversed.groups.first().indicatorIds,s.groups.first().indicatorIds);
    }
    void pullbackBandsNeverPoolSmallSamples(){
        ChartStudySet core;core.available=true;core.scorecard.available=true;core.scorecard.horizonMs=60000;core.lastCompletedAtMs=10000000;
        for(int i=0;i<40;++i){const qint64 at=60000*(i+1);core.entries.append({at,at-60000,100,i<20?3:8});if(i!=39)core.scorecard.outcomes.append({at,at+60000,i<20?2.0:-1.0});}
        const auto rows=chartPullbackScoreSlices(core);QCOMPARE(rows.size(),2);
        QCOMPARE(rows[0].events,20);QCOMPARE(rows[0].results,20);QCOMPARE(*rows[0].meanReturnPercent,2.0);
        QCOMPARE(rows[1].events,20);QCOMPARE(rows[1].results,19);QCOMPARE(rows[1].pending,1);QVERIFY(!rows[1].meanReturnPercent);
    }
    void flowSlicesSeparateKindDirectionAndRejectAmbiguousOutcome(){
        CoinFlowStudySet core;core.available=true;core.asOfMs=core.lastCompletedAtMs=10000000;core.scorecard.horizonMs=60000;
        for(int i=0;i<40;++i){const qint64 at=60000*(i+1);const auto direction=i<20?CoinFlowDirection::Up:CoinFlowDirection::Down;
            core.groups.append({CoinFlowSignalKind::Rebound,direction,at,at,at,"100",1,true});
            core.scorecard.outcomes.append({CoinFlowSignalKind::Rebound,at,at+60000,at+60000,i<20?2.0:-3.0});}
        auto rows=chartFlowScoreSlices(core);QCOMPARE(rows.size(),2);for(const auto& row:rows){QCOMPARE(row.results,20);QVERIFY(row.meanReturnPercent);QVERIFY(row.meanDirectionalReturnPercent);QVERIFY(*row.meanDirectionalReturnPercent>0);}
        auto conflict=core.groups.last();conflict.direction=CoinFlowDirection::Up;core.groups.append(conflict);
        rows=chartFlowScoreSlices(core);for(const auto& row:rows)if(row.key.endsWith(":2")){QCOMPARE(row.results,19);QVERIFY(!row.meanReturnPercent);}
        core.asOfMs=1;rows=chartFlowScoreSlices(core);QVERIFY(rows.isEmpty());
    }
    void scorecardDisplayKeepsNineteenPrivate() {
        QVERIFY(!chartScorecardStatisticsVisible(true,19));
        QVERIFY(chartScorecardStatisticsVisible(true,20));
        QVERIFY(!chartScorecardStatisticsVisible(false,25));
    }
    void strongReversalSlicesKeepEachGradeAndDirectionAtN20(){
        constexpr qint64 base=1728000000000,hour=3600000;
        ExtendedStrongReversal core;core.asOfMs=base+1000*hour;
        for(int grade=1;grade<=4;++grade)for(int i=0;i<40;++i){
            const bool up=i<20;const qint64 at=base+(grade*100+i)*60000;
            ExtendedReversalEvent event;event.kind=up?CoinFlowSignalKind::Rebound:CoinFlowSignalKind::TurnDown;
            event.direction=up?IndicatorDirection::Up:IndicatorDirection::Down;event.grade=ExtendedReversalGrade(grade);
            event.signalAtMs=at;event.decisionAtMs=at+1000;
            if(i!=39)event.outcome=CoinFlowOutcome{event.kind,at,at+hour,at+hour+1,up?2.0:-3.0};
            core.events.append(event);
        }
        const auto rows=chartStrongReversalScoreSlices(core);QCOMPARE(rows.size(),8);
        for(const auto& row:rows){QCOMPARE(row.events,20);QCOMPARE(row.overlapping,19);
            if(row.key.endsWith(":1")){QCOMPARE(row.results,20);QCOMPARE(row.pending,0);QCOMPARE(row.meanReturnPercent,std::optional<double>(2));QCOMPARE(row.meanDirectionalReturnPercent,std::optional<double>(2));}
            else{QCOMPARE(row.results,19);QCOMPARE(row.pending,1);QVERIFY(!row.meanReturnPercent);QVERIFY(!row.meanDirectionalReturnPercent);}}
        auto duplicate=core.events.first();core.events.append(duplicate);
        core.events.first().outcome->evaluationAtMs+=60000;core.events[1].outcome->knownAtMs=core.asOfMs+1;
        const auto invalid=chartStrongReversalScoreSlices(core);QCOMPARE(invalid.first().events,20);QCOMPARE(invalid.first().results,18);QCOMPARE(invalid.first().pending,2);QVERIFY(!invalid.first().meanReturnPercent);
        core.events.clear();const auto empty=chartStrongReversalScoreSlices(core);QCOMPARE(empty.size(),8);
        for(const auto& row:empty){QCOMPARE(row.events,0);QCOMPARE(row.results,0);QVERIFY(!row.meanReturnPercent);}
        core.horizonMs=60000;QVERIFY(chartStrongReversalScoreSlices(core).isEmpty());
    }
};
QTEST_GUILESS_MAIN(ChartIndicatorStateTest)
#include "tst_chart_indicator_state.moc"
