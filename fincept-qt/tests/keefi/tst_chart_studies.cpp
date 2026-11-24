#include "keefi/ChartStudies.h"
#include "keefi/MarketStudies.h"
#include "keefi/StockChartTypes.h"
#include <QTest>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <cmath>
using namespace keefi;
namespace {
constexpr qint64 Base=1728000000000;
MarketCandle bar(int index,qint64 interval,QString close="100",QString volume="0"){
    bool ok=false;const auto price=close.toDouble(&ok);Q_ASSERT(ok);
    const auto at=Base+index*interval;
    return {at,at+1000,close,QString::number(price+1,'f',6),QString::number(price-1,'f',6),close,volume};
}
QList<MarketCandle> flat(int count,qint64 interval=60000){
    QList<MarketCandle> rows;for(int i=0;i<count;++i)rows.append(bar(i,interval));return rows;
}
QList<MarketCandle> entry(int count,qint64 interval){
    auto rows=flat(count,interval);if(count>10)rows[10].low="90";if(count>11)rows[11].high="120";return rows;
}
QList<MarketCandle> repeating(int count,qint64 interval=60000){
    const int pattern[]{100,101,99,102,100,104,101,103,100,105,102,106,103,107,104,108,106,109,107,110};
    QList<MarketCandle> rows;
    for(int i=0;i<count;++i)rows.append(bar(i,interval,QString::number(pattern[i%20]),"2.00"));return rows;
}

QJsonValue nullable(double value,bool available){return available?QJsonValue(value):QJsonValue(QJsonValue::Null);}
QJsonArray pointsJson(const QList<ChartPricePoint>& points){
    QJsonArray out;for(const auto& point:points)out.append(QJsonObject{{"atMs",point.atMs},{"price",point.price}});return out;
}
QJsonObject canonicalJson(const ChartStudySet& r){
    const auto& p=r.pullback;QJsonArray evidence;
    for(const auto& e:p.evidence)evidence.append(QJsonObject{{"id",e.id},{"label",e.label},{"price",e.price}});
    QJsonObject pull{{"available",p.available},{"inBox",p.inBox},{"reason",p.reason},
        {"waveLowAtMs",p.waveLowAtMs},{"waveHighAtMs",p.waveHighAtMs},
        {"waveLow",nullable(p.waveLow,p.waveLowAtMs!=0)},{"waveHigh",nullable(p.waveHigh,p.waveLowAtMs!=0)},
        {"boxLow",nullable(p.boxLow,p.available)},{"boxHigh",nullable(p.boxHigh,p.available)},
        {"atr",nullable(p.atr,r.currentRunBars>=21)},{"overlap",p.overlap},{"score",p.score},{"evidence",evidence}};
    const auto& s=r.similarity;QJsonArray paths;
    for(const auto& path:s.paths)paths.append(QJsonObject{{"fromMs",path.fromMs},{"toMs",path.toMs},
        {"followingToMs",path.followingToMs},{"correlation",path.correlation},{"points",pointsJson(path.points)}});
    QJsonObject similar{{"available",s.available},{"volatilityNormalized",s.volatilityNormalized},{"reason",s.reason},
        {"queryBars",s.queryBars},{"followingBars",s.followingBars},{"searchBars",s.searchBars},
        {"eligibleCandidates",s.eligibleCandidates},{"overlapDropped",s.overlapDropped},{"samples",s.samples},
        {"upPaths",s.upPaths},{"downPaths",s.downPaths},{"flatPaths",s.flatPaths},
        {"searchFromMs",s.searchFromMs},{"searchToMs",s.searchToMs},{"paths",paths},
        {"mean",pointsJson(s.mean)},{"minimum",pointsJson(s.minimum)},{"maximum",pointsJson(s.maximum)}};
    QJsonArray cloud,volumes,signalsJson,outcomes;
    for(const auto& point:r.cloud)cloud.append(QJsonObject{{"atMs",point.atMs},{"fast",point.fast},{"slow",point.slow}});
    for(const auto& point:r.volumes)volumes.append(QJsonObject{{"atMs",point.atMs},{"volume",point.volume}});
    for(const auto& signal:r.entries)signalsJson.append(QJsonObject{{"atMs",signal.atMs},{"waveLowAtMs",signal.waveLowAtMs},
        {"entryClose",signal.entryClose},{"score",signal.score}});
    const auto& card=r.scorecard;
    for(const auto& outcome:card.outcomes)outcomes.append(QJsonObject{{"signalAtMs",outcome.signalAtMs},
        {"evaluationAtMs",outcome.evaluationAtMs},{"returnPercent",outcome.returnPercent}});
    QJsonObject scorecard{{"available",card.available},{"costsExcluded",card.costsExcluded},{"reason",card.reason},
        {"horizonMs",card.horizonMs},{"samples",card.samples},{"unresolvedSignals",card.unresolvedSignals},
        {"overlappingSignals",card.overlappingSignals},{"up",card.up},{"down",card.down},{"flat",card.flat},
        {"meanReturnPercent",nullable(card.meanReturnPercent,card.available)},{"outcomes",outcomes}};
    return {{"available",r.available},{"reason",r.reason},{"intervalMs",r.intervalMs},{"lastCompletedAtMs",r.lastCompletedAtMs},
        {"observations",r.observations},{"currentRunBars",r.currentRunBars},{"pullback",pull},{"similarity",similar},
        {"cloud",cloud},{"volumes",volumes},{"signals",signalsJson},{"scorecard",scorecard}};
}

}
class ChartStudiesTest final:public QObject {
    Q_OBJECT
  private slots:
    void dev86DistinguishesMissingInputsFromNoCondition();
    void dev86SimilarityRecordsOnlyItsCompletedReference();
    void similarityReferenceUsesObservedStockStart_data();
    void similarityReferenceUsesObservedStockStart();
    void exportSyntheticResult();
    void profileBoundaryUsesExactDecimalMembership_data();
    void profileBoundaryUsesExactDecimalMembership();
    void profileVolumeTotalsUseExactSourceDecimals();
    void tinyPositiveProfileVolumeRetainsItsSourceMeaning();
    void observedSupportCloudAndExactVolume();
    void observedStockCloudAndAdxWithoutRestrictionHistoryPreserveStrictStudies();
    void invalidInputsRejectAtomically_data();
    void invalidInputsRejectAtomically();
    void waveBoxUsesOnlyObservedPriorExtremes();
    void similaritySuppressesEntireWindowsAndUsesActualFollowingCloses();
    void minimumSimilarityRejectsWeakAndOppositePaths_data();
    void minimumSimilarityRejectsWeakAndOppositePaths();
    void incompleteFollowingAndGapsNeverCreateSamples();
    void scorecardUsesExactTimeframeHorizon_data();
    void scorecardUsesExactTimeframeHorizon();
    void futureChangesCannotChangeEarlierSignals();
    void missingIntermediateBarLeavesOutcomeUnresolved();
    void overlappingSignalWindowsAreDisclosedSeparately();
};
void ChartStudiesTest::dev86DistinguishesMissingInputsFromNoCondition(){
    const auto missing=keefiChartStudies(flat(20),60000);
    QCOMPARE(missing.pullback.status,ChartStudyStatus::Insufficient);
    QCOMPARE(missing.similarity.status,ChartStudyStatus::Insufficient);
    auto noWave=flat(40);for(auto& row:noWave){row.high="110";row.low="90";}
    const auto noCondition=keefiChartStudies(noWave,60000);
    QVERIFY(noCondition.available);QVERIFY(!noCondition.pullback.available);
    QCOMPARE(noCondition.pullback.status,ChartStudyStatus::NoSignal);
    const auto valid=keefiChartStudies(entry(40,60000),60000);
    QVERIFY(valid.pullback.available);QCOMPARE(valid.pullback.status,ChartStudyStatus::Signal);
    QVERIFY(valid.pullback.boxHigh>=valid.pullback.boxLow);
    // Adequate finite candidates with correlation zero really means 0 matches.
    auto weak=flat(60);const double pi=std::acos(-1.0);double past=100,query=100;
    for(int i=0;i<19;++i){const auto angle=2*pi*i/19;past*=std::exp(.01*std::sin(angle));query*=std::exp(.01*std::cos(angle));weak[i+1]=bar(i+1,60000,QString::number(past,'f',12));weak[i+41]=bar(i+41,60000,QString::number(query,'f',12));}
    const auto none=keefiChartStudies(weak,60000).similarity;
    QCOMPARE(none.eligibleCandidates,1);QCOMPARE(none.samples,0);QCOMPARE(none.status,ChartStudyStatus::NoSignal);
    // A constant query is non-comparable, not a confirmed empty match set.
    QCOMPARE(keefiChartStudies(flat(60),60000).similarity.status,ChartStudyStatus::Insufficient);
}
void ChartStudiesTest::dev86SimilarityRecordsOnlyItsCompletedReference(){
    auto rows=repeating(200);const auto prefix=rows.first(120);
    const auto original=keefiChartStudies(prefix,60000).similarity;
    QCOMPARE(original.status,ChartStudyStatus::Signal);
    QCOMPARE(original.referenceFromMs,prefix[100].startsAtMs);
    QCOMPARE(original.referenceThroughMs,prefix.last().startsAtMs+60000);
    QCOMPARE(original.referenceLastStartMs,prefix.last().startsAtMs);
    for(const auto& path:original.paths){QVERIFY(path.followingToMs<=original.referenceFromMs);QVERIFY(path.fromMs<path.toMs);}
    // Historical reference evaluation is the observed prefix. Later values do
    // not join the old query or its following outcomes.
    for(int i=120;i<rows.size();++i)rows[i]=bar(i,60000,"5000","100000");
    const auto again=keefiChartStudies(rows.first(120),60000).similarity;
    QCOMPARE(again.referenceThroughMs,original.referenceThroughMs);QCOMPARE(again.samples,original.samples);
    QCOMPARE(again.referenceLastStartMs,original.referenceLastStartMs);
    for(int i=0;i<again.paths.size();++i){QCOMPARE(again.paths[i].fromMs,original.paths[i].fromMs);QCOMPARE(again.paths[i].followingToMs,original.paths[i].followingToMs);QCOMPARE(again.paths[i].correlation,original.paths[i].correlation);}
}
void ChartStudiesTest::similarityReferenceUsesObservedStockStart_data(){
    QTest::addColumn<QByteArray>("zoneId");QTest::addColumn<QDate>("lastDay");
    QTest::newRow("KR-Monday")<<QByteArray("Asia/Seoul")<<QDate(2026,11,2);
    QTest::newRow("US-Monday-after-DST")<<QByteArray("America/New_York")<<QDate(2026,11,2);
    QTest::newRow("US-Friday-before-DST")<<QByteArray("America/New_York")<<QDate(2026,10,30);
}
void ChartStudiesTest::similarityReferenceUsesObservedStockStart(){
    QFETCH(QByteArray,zoneId);QFETCH(QDate,lastDay);const QTimeZone zone(zoneId);
    // Explicit synthetic trading calendar: this fixture is not a live broker/calendar check.
    QList<QDate> days;for(auto day=lastDay;days.size()<100;day=day.addDays(-1))if(day.dayOfWeek()<=5)days.prepend(day);
    auto rows=repeating(days.size());ChartStudyTimeline timeline;timeline.adjustedComparable=true;timeline.projectionCovered=true;
    for(int i=0;i<rows.size();++i){auto& candle=rows[i];candle.startsAtMs=QDateTime(days[i],QTime(0,0),zone).toMSecsSinceEpoch();candle.lastStoredTickMs=0;
        ChartStudyTimelineRow state;state.startsAtMs=candle.startsAtMs;state.completesAtMs=QDateTime(days[i],QTime(20,0),zone).toMSecsSinceEpoch();
        state.tradingDate=days[i];state.session=StockSessionKind::Regular;state.eligible=true;state.connectsPrevious=i>0;timeline.rows.append(state);}
    for(auto day=lastDay.addDays(1);timeline.nextStartsAtMs.size()<20;day=day.addDays(1))if(day.dayOfWeek()<=5)timeline.nextStartsAtMs.append(QDateTime(day,QTime(0,0),zone).toMSecsSinceEpoch());
    const auto result=keefiChartStudies(rows,86400000,timeline);QVERIFY(result.available);QVERIFY(result.similarity.available);
    QCOMPARE(result.similarity.referenceThroughMs,timeline.rows.last().completesAtMs);
    QVERIFY(result.similarity.referenceThroughMs-86400000!=rows.last().startsAtMs);
    QCOMPARE(result.similarity.referenceLastStartMs,rows.last().startsAtMs);
    QCOMPARE(result.similarity.referenceFromMs,rows[rows.size()-20].startsAtMs);
}
void ChartStudiesTest::exportSyntheticResult(){
    const auto inputPath=qEnvironmentVariable("KEEFI_G8_STUDIES_INPUT"),outputPath=qEnvironmentVariable("KEEFI_G8_STUDIES_OUTPUT");
    if(inputPath.isEmpty()&&outputPath.isEmpty())QSKIP("Optional synthetic JSON cross-calculation seam");
    QVERIFY(!inputPath.isEmpty());QVERIFY(!outputPath.isEmpty());QFile input(inputPath);QVERIFY(input.open(QIODevice::ReadOnly));
    QJsonParseError error;const auto document=QJsonDocument::fromJson(input.readAll(),&error);
    QCOMPARE(error.error,QJsonParseError::NoError);QVERIFY(document.isObject());const auto object=document.object();
    QVERIFY(object.value("intervalMs").isDouble());QVERIFY(object.value("candles").isArray());
    QList<MarketCandle> rows;
    for(const auto& value:object.value("candles").toArray()){
        QVERIFY(value.isObject());const auto row=value.toObject();
        QVERIFY(row.value("startsAtMs").isDouble());
        for(const auto* key:{"open","high","low","close","volume"})QVERIFY(row.value(key).isString());
        rows.append({row.value("startsAtMs").toInteger(),row.value("lastStoredTickMs").toInteger(),
            row.value("open").toString(),row.value("high").toString(),row.value("low").toString(),
            row.value("close").toString(),row.value("volume").toString()});
    }
    const auto encoded=QJsonDocument(canonicalJson(keefiChartStudies(rows,object.value("intervalMs").toInteger()))).toJson(QJsonDocument::Indented);
    QSaveFile output(outputPath);QVERIFY(output.open(QIODevice::WriteOnly));QCOMPARE(output.write(encoded),qint64(encoded.size()));QVERIFY(output.commit());
}
void ChartStudiesTest::profileBoundaryUsesExactDecimalMembership_data(){
    QTest::addColumn<QString>("close");QTest::addColumn<QString>("high");QTest::addColumn<QString>("low");QTest::addColumn<double>("expected");
    QTest::newRow("below")<<QString("100.799999999999999999")<<QString("101.799999999999999999")<<QString("99.799999999999999999")<<100.5;
    QTest::newRow("exact")<<QString("100.8")<<QString("101.8")<<QString("99.8")<<101.1;
    QTest::newRow("above")<<QString("100.800000000000000001")<<QString("101.800000000000000001")<<QString("99.800000000000000001")<<101.1;
}
void ChartStudiesTest::profileBoundaryUsesExactDecimalMembership(){
    QFETCH(QString,close);QFETCH(QString,high);QFETCH(QString,low);QFETCH(double,expected);
    auto rows=flat(21);rows[0].high="111";rows[0].low="99";rows[0].volume="0";
    for(int i=1;i<rows.size();++i){rows[i].open=close;rows[i].close=close;rows[i].high=high;rows[i].low=low;rows[i].volume="3";}
    const auto result=keefiChartStudies(rows,60000);QVERIFY(result.available);
    // Exact global span12: typical100.8 is boundary3 of twenty bins.
    // Its adjacent source decimals must remain on their respective sides.
    bool found=false;for(const auto& level:result.pullback.evidence)if(level.id=="profilePoc96"){
        found=true;QVERIFY2(std::abs(level.price-expected)<1e-10,qPrintable(QString::number(level.price,'g',17)));
    }
    QVERIFY(found);
    bool regressionFound=false;for(const auto& level:result.pullback.evidence)if(level.id=="lowRegression20")regressionFound=true;
    QVERIFY2(regressionFound,"Constant last20 low must remain exactly within inclusive half-ATR proximity");
}
void ChartStudiesTest::profileVolumeTotalsUseExactSourceDecimals(){
    auto rows=flat(21);rows[0].high="111";rows[0].low="99";
    for(int i=1;i<rows.size();++i){rows[i].open="100.8";rows[i].close="100.8";rows[i].high="101.8";rows[i].low="99.8";}
    rows[1].open=rows[1].close="100.5";rows[1].high="101.5";rows[1].low="99.5";rows[1].volume="100000000000000000000";
    rows[2].open=rows[2].close="101.1";rows[2].high="102.1";rows[2].low="100.1";rows[2].volume="100000000000000000001";
    const auto result=keefiChartStudies(rows,60000);QVERIFY(result.available);
    // Bin3 exceeds bin2 by exactly one source volume unit, below binary64's ULP.
    bool found=false;for(const auto& level:result.pullback.evidence)if(level.id=="profilePoc96"){
        found=true;QVERIFY2(std::abs(level.price-101.1)<1e-10,qPrintable(QString::number(level.price,'g',17)));
    }
    QVERIFY(found);
}
void ChartStudiesTest::tinyPositiveProfileVolumeRetainsItsSourceMeaning(){
    auto rows=flat(21);const auto tiny=QString("0.")+QString(999,'0')+"1";
    for(auto& row:rows){row.high="110";row.low="90";row.volume=tiny;}
    const auto result=keefiChartStudies(rows,60000);QVERIFY(result.available);QCOMPARE(result.volumes.last().volume,tiny);
    bool found=false;for(const auto& level:result.pullback.evidence)if(level.id=="profilePoc96"){
        found=true;QVERIFY(std::abs(level.price-100.5)<1e-10);
    }
    QVERIFY(found);
}
void ChartStudiesTest::observedStockCloudAndAdxWithoutRestrictionHistoryPreserveStrictStudies(){
    const auto rows=repeating(120);
    ChartStudyTimeline timeline;
    for(const auto& bar:rows){
        ChartStudyTimelineRow row;
        row.startsAtMs=bar.startsAtMs;row.completesAtMs=bar.startsAtMs+60000;
        row.connectsPrevious=!timeline.rows.isEmpty();
        row.exclusions={StockBarExclusion::RestrictionCoverageUnknown};
        row.reason=QStringLiteral("과거 VI·가격 제한 제외 근거 미제공");
        timeline.rows.append(row);
    }
    QList<ChartCloudPoint> cloud;
    const auto strict=keefiChartStudies(rows,60000,timeline,&cloud);
    const auto strictWithoutDisplay=keefiChartStudies(rows,60000,timeline);
    QCOMPARE(canonicalJson(strict),canonicalJson(strictWithoutDisplay));
    QVERIFY(strict.cloud.isEmpty());QVERIFY(strict.entries.isEmpty());QVERIFY(!strict.scorecard.available);
    QVERIFY(!candleStudies(rows,60000,timeline).adxAvailable);
    const auto native=keefiChartStudies(rows,60000);
    QCOMPARE(cloud.size(),95);QCOMPARE(cloud.size(),native.cloud.size());
    for(int i=0;i<cloud.size();++i){
        QCOMPARE(cloud[i].atMs,native.cloud[i].atMs);
        QCOMPARE(cloud[i].fast,native.cloud[i].fast);QCOMPARE(cloud[i].slow,native.cloud[i].slow);
    }
    auto adx=stockDisplayAdx(rows,60000,timeline);
    QVERIFY(adx.adxAvailable);QCOMPARE(adx.adx,candleStudies(rows,60000).adx);
    QVERIFY(timeline.recentExclusionReason(28).contains("VI"));QVERIFY(timeline.recentExclusionReason(28,true).isEmpty());
    // A confirmed exclusion ends the display run. Only future confirmed observations reseed it.
    timeline.rows[90].exclusions.append(StockBarExclusion::VolatilityInterruption);
    keefiChartStudies(rows,60000,timeline,&cloud);
    for(const auto& p:cloud)QVERIFY(p.atMs<rows[90].startsAtMs||p.atMs>=rows[116].startsAtMs);
    QCOMPARE(cloud.last().atMs,rows.last().startsAtMs);
    QCOMPARE(stockDisplayAdx(rows,60000,timeline).adx,candleStudies(rows.mid(91),60000).adx);
    timeline.rows.last().exclusions.append(StockBarExclusion::AdjustmentUnknown);
    timeline.rows.last().reason=QStringLiteral("수정주가 확인 필요");
    keefiChartStudies(rows,60000,timeline,&cloud);
    QVERIFY(cloud.last().atMs<rows.last().startsAtMs);QVERIFY(!stockDisplayAdx(rows,60000,timeline).adxAvailable);
    QCOMPARE(timeline.recentExclusionReason(28,true),QStringLiteral("수정주가 확인 필요"));
}
void ChartStudiesTest::observedSupportCloudAndExactVolume(){
    auto rows=flat(40);for(auto& row:rows){row.high="110";row.low="90";row.volume="2.00";}
    const auto result=keefiChartStudies(rows,60000);
    QVERIFY2(result.available,qPrintable(result.reason));QCOMPARE(result.observations,40);QCOMPARE(result.currentRunBars,40);
    QCOMPARE(result.lastCompletedAtMs,Base+40*60000);QCOMPARE(result.pullback.atr,20.0);
    QCOMPARE(result.pullback.overlap,4);QCOMPARE(result.pullback.score,10);QCOMPARE(result.pullback.evidence.size(),4);
    QCOMPARE(result.pullback.evidence[0].id,QString("sma20"));QCOMPARE(result.pullback.evidence[0].price,100.0);
    QCOMPARE(result.pullback.evidence[1].price,100.0);QCOMPARE(result.pullback.evidence[2].price,100.5);
    QCOMPARE(result.pullback.evidence[3].price,90.0);QVERIFY(!result.pullback.available);
    QCOMPARE(result.cloud.size(),15);QCOMPARE(result.cloud.first().atMs,Base+25*60000);
    QCOMPARE(result.cloud.last().fast,100.0);QCOMPARE(result.cloud.last().slow,100.0);
    QCOMPARE(result.volumes.size(),40);QCOMPARE(result.volumes.last().volume,QString("2.00"));
}
void ChartStudiesTest::invalidInputsRejectAtomically_data(){
    QTest::addColumn<QString>("mutation");
    for(const auto* value:{"empty","duplicate","unsorted","misaligned","decimal-LF","negative","ohlc","201rows","interval",
                           "ohlc-sub-ulp","upperbound-sub-ulp","lowerbound-sub-ulp","volume-upperbound-sub-ulp"})
        QTest::newRow(value)<<QString(value);
}
void ChartStudiesTest::invalidInputsRejectAtomically(){
    QFETCH(QString,mutation);auto rows=flat(40);qint64 interval=60000;
    if(mutation=="empty")rows.clear();
    if(mutation=="duplicate")rows[10].startsAtMs=rows[9].startsAtMs;
    if(mutation=="unsorted")std::swap(rows[10],rows[11]);
    if(mutation=="misaligned")++rows[10].startsAtMs;
    if(mutation=="decimal-LF")rows[10].close="100\n";
    if(mutation=="negative")rows[10].volume="-1";
    if(mutation=="ohlc")rows[10].low="105";
    if(mutation=="ohlc-sub-ulp")rows[10].high="99.999999999999999999";
    if(mutation=="upperbound-sub-ulp")rows[10].high=QString("1")+QString(100,'0')+".1";
    if(mutation=="lowerbound-sub-ulp")rows[10].low=QString("0.")+QString(100,'0')+QString(20,'9');
    if(mutation=="volume-upperbound-sub-ulp")rows[10].volume=QString("1")+QString(100,'0')+".1";
    if(mutation=="201rows")rows=flat(201);
    if(mutation=="interval")interval=120000;
    const auto result=keefiChartStudies(rows,interval);
    QVERIFY(!result.available);QVERIFY(!result.reason.isEmpty());QCOMPARE(result.observations,0);
    QVERIFY(result.volumes.isEmpty());QVERIFY(result.cloud.isEmpty());QVERIFY(result.entries.isEmpty());
    QVERIFY(result.similarity.paths.isEmpty());QVERIFY(result.scorecard.outcomes.isEmpty());
}
void ChartStudiesTest::waveBoxUsesOnlyObservedPriorExtremes(){
    const auto result=keefiChartStudies(entry(21,60000),60000);
    QVERIFY(result.pullback.available);QVERIFY(result.pullback.inBox);
    QCOMPARE(result.pullback.waveLowAtMs,Base+10*60000);QCOMPARE(result.pullback.waveHighAtMs,Base+11*60000);
    QCOMPARE(result.pullback.waveLow,90.0);QCOMPARE(result.pullback.waveHigh,120.0);
    QVERIFY(std::abs(result.pullback.atr-3.4)<1e-12);QCOMPARE(result.pullback.overlap,3);QCOMPARE(result.pullback.score,8);
    QVERIFY(std::abs(result.pullback.boxLow-96.91428571428571)<1e-10);
    QVERIFY(std::abs(result.pullback.boxHigh-101.7)<1e-10);
    QCOMPARE(result.entries.size(),1);QCOMPARE(result.entries.first().atMs,Base+21*60000);
    QCOMPARE(result.entries.first().waveLowAtMs,Base+10*60000);QCOMPARE(result.entries.first().score,8);
    QCOMPARE(result.entries.first().entryClose,100.0);
}
void ChartStudiesTest::similaritySuppressesEntireWindowsAndUsesActualFollowingCloses(){
    const auto result=keefiChartStudies(repeating(200),60000);const auto& similar=result.similarity;
    QVERIFY2(similar.available,qPrintable(similar.reason));QCOMPARE(similar.searchBars,180);
    QCOMPARE(similar.eligibleCandidates,141);QCOMPARE(similar.samples,3);QCOMPARE(similar.overlapDropped,57); // Only r >= 0.80 candidates enter overlap suppression.
    QCOMPARE(similar.searchFromMs,Base);QCOMPARE(similar.searchToMs,Base+180*60000);
    QVERIFY(!similar.volatilityNormalized);QCOMPARE(similar.flatPaths,3);QCOMPARE(similar.upPaths,0);QCOMPARE(similar.downPaths,0);
    for(int i=0;i<3;++i){const auto& path=similar.paths[i];
        QCOMPARE(path.fromMs,Base+i*40*60000);QCOMPARE(path.toMs,path.fromMs+20*60000);
        QCOMPARE(path.followingToMs,path.fromMs+40*60000);QVERIFY(path.followingToMs<=similar.searchToMs);
        QVERIFY(std::abs(path.correlation-1)<1e-12);QCOMPARE(path.points.size(),21);
        QCOMPARE(path.points[0].price,110.0);QCOMPARE(path.points[1].price,100.0);QCOMPARE(path.points[20].price,110.0);
        QCOMPARE(path.points[1].atMs,Base+200*60000);
    }
    QCOMPARE(similar.mean.size(),21);QCOMPARE(similar.mean[1].price,100.0);
    QCOMPARE(similar.minimum[1].price,100.0);QCOMPARE(similar.maximum[1].price,100.0);
}
void ChartStudiesTest::minimumSimilarityRejectsWeakAndOppositePaths_data(){
    QTest::addColumn<double>("correlation"); QTest::addColumn<bool>("expected");
    QTest::newRow("opposite") << -1.0 << false;
    QTest::newRow("uncorrelated") << 0.0 << false;
    QTest::newRow("weak") << 0.5 << false;
    QTest::newRow("just-below") << 0.799999 << false;
    QTest::newRow("just-above") << 0.800001 << true;
    QTest::newRow("identical") << 1.0 << true;
}
void ChartStudiesTest::minimumSimilarityRejectsWeakAndOppositePaths(){
    QFETCH(double, correlation); QFETCH(bool, expected);
    // Two orthogonal, zero-mean return vectors produce one candidate with a
    // controlled correlation; its following 20 bars are already completed.
    auto rows=flat(60); const double pi=std::acos(-1.0); double past=100,query=100;
    for(int i=0;i<19;++i){
        const double angle=2*pi*i/19, a=std::sin(angle), b=std::cos(angle);
        past*=std::exp(0.01*a);
        query*=std::exp(0.01*(correlation*a+std::sqrt(1-correlation*correlation)*b));
        rows[i+1]=bar(i+1,60000,QString::number(past,'f',12));
        rows[i+41]=bar(i+41,60000,QString::number(query,'f',12));
    }
    const auto& similar=keefiChartStudies(rows,60000).similarity;
    QCOMPARE(similar.eligibleCandidates,1); QCOMPARE(similar.available,expected);
    if(expected){QCOMPARE(similar.samples,1);QVERIFY(std::abs(similar.paths.first().correlation-correlation)<1e-9);}
    else {QCOMPARE(similar.reason,QStringLiteral("충분히 닮은 과거 구간이 없어요"));
        QVERIFY(similar.paths.isEmpty());QVERIFY(similar.mean.isEmpty());QVERIFY(similar.minimum.isEmpty());QVERIFY(similar.maximum.isEmpty());}
}
void ChartStudiesTest::incompleteFollowingAndGapsNeverCreateSamples(){
    QVERIFY(!keefiChartStudies(repeating(59),60000).similarity.available);
    QCOMPARE(keefiChartStudies(repeating(60),60000).similarity.samples,1);
    auto rows=flat(40);for(int i=20;i<rows.size();++i)rows[i].startsAtMs+=60000;
    const auto result=keefiChartStudies(rows,60000);QVERIFY(result.available);QCOMPARE(result.currentRunBars,20);
    QVERIFY(result.cloud.isEmpty());QVERIFY(!result.pullback.available);QVERIFY(!result.similarity.available);
    QCOMPARE(result.volumes.size(),40);
}
void ChartStudiesTest::scorecardUsesExactTimeframeHorizon_data(){
    QTest::addColumn<qint64>("interval");QTest::addColumn<qint64>("horizon");
    QTest::newRow("1m")<<qint64(60000)<<qint64(3600000);
    QTest::newRow("5m")<<qint64(300000)<<qint64(3600000);
    QTest::newRow("15m")<<qint64(900000)<<qint64(14400000);
    QTest::newRow("1h")<<qint64(3600000)<<qint64(86400000);
    QTest::newRow("4h")<<qint64(14400000)<<qint64(86400000);
    QTest::newRow("1d")<<qint64(86400000)<<qint64(86400000);
}
void ChartStudiesTest::scorecardUsesExactTimeframeHorizon(){
    QFETCH(qint64,interval);QFETCH(qint64,horizon);const int target=20+int(horizon/interval);
    auto rows=entry(target+1,interval);rows[target]=bar(target,interval,"110");
    const auto result=keefiChartStudies(rows,interval);QCOMPARE(result.entries.size(),1);
    const auto& score=result.scorecard;QVERIFY(score.available);QCOMPARE(score.horizonMs,horizon);
    QCOMPARE(score.samples,1);QCOMPARE(score.unresolvedSignals,0);QCOMPARE(score.outcomes.size(),1);
    QCOMPARE(score.outcomes.first().signalAtMs,Base+21*interval);
    QCOMPARE(score.outcomes.first().evaluationAtMs,Base+21*interval+horizon);
    QVERIFY(std::abs(score.meanReturnPercent-10)<1e-12);QCOMPARE(score.up,1);QCOMPARE(score.down,0);QVERIFY(score.costsExcluded);
}
void ChartStudiesTest::futureChangesCannotChangeEarlierSignals(){
    auto rows=entry(81,60000);rows[80]=bar(80,60000,"110");const auto original=keefiChartStudies(rows,60000);
    for(int i=21;i<rows.size();++i)rows[i]=bar(i,60000,"500");
    const auto changed=keefiChartStudies(rows,60000),prefix=keefiChartStudies(rows.mid(0,21),60000);
    QCOMPARE(prefix.entries.size(),1);QVERIFY(!original.entries.isEmpty());QVERIFY(!changed.entries.isEmpty());
    for(const auto* result:{&original,&changed}){
        QCOMPARE(result->entries.first().atMs,prefix.entries.first().atMs);
        QCOMPARE(result->entries.first().waveLowAtMs,prefix.entries.first().waveLowAtMs);
        QCOMPARE(result->entries.first().entryClose,prefix.entries.first().entryClose);
        QCOMPARE(result->entries.first().score,prefix.entries.first().score);
    }
}
void ChartStudiesTest::missingIntermediateBarLeavesOutcomeUnresolved(){
    auto rows=entry(33,300000);rows.last()=bar(32,300000,"110");
    QVERIFY(keefiChartStudies(rows,300000).scorecard.available);rows.removeAt(25);
    const auto result=keefiChartStudies(rows,300000);QCOMPARE(result.entries.size(),1);
    QVERIFY(!result.scorecard.available);QCOMPARE(result.scorecard.samples,0);QCOMPARE(result.scorecard.unresolvedSignals,1);
    QVERIFY(result.scorecard.outcomes.isEmpty());
}
void ChartStudiesTest::overlappingSignalWindowsAreDisclosedSeparately(){
    auto rows=entry(27,60000);rows[24].low="80";rows[25].high="130";
    const auto result=keefiChartStudies(rows,60000);QCOMPARE(result.entries.size(),2);
    QCOMPARE(result.entries[0].atMs,Base+21*60000);QCOMPARE(result.entries[1].atMs,Base+27*60000);
    QCOMPARE(result.scorecard.overlappingSignals,1);QCOMPARE(result.scorecard.unresolvedSignals,2);
}
QTEST_GUILESS_MAIN(ChartStudiesTest)
#include "tst_chart_studies.moc"
