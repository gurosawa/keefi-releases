#include "keefi/ChartDrawings.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <algorithm>
#include <cmath>
using namespace keefi;
namespace {
constexpr qint64 Minute=60000,Day=86400000,Start=1792713600000;
const WatchlistIdentity Coin{"coin","upbit","KRW-SYN"};
ChartDrawingContext context(bool publicAllowed=true) { return {Coin,"KRW",publicAllowed}; }
ChartDrawingFrame frame(const ChartDrawings& model,int count=6) {
    ChartDrawingFrame result; result.identity=model.context().identity; result.currency=model.context().currency;
    result.intervalMs=model.intervalMs(); result.contextGeneration=model.contextGeneration();
    for(int i=0;i<count;++i) {
        ChartDrawingBar bar; bar.candle={Start+i*result.intervalMs,0,"100","105","95","102","1"};
        bar.completesAtMs=bar.candle.startsAtMs+result.intervalMs; bar.sourceCompleted=true; bar.connectsPrevious=i>0;
        result.bars.append(bar);
    }
    result.asOfMs=result.bars.last().completesAtMs+1000; return result;
}
QByteArray encoded(const QJsonObject& root) { return QJsonDocument(root).toJson(QJsonDocument::Compact); }
QJsonObject firstContext(const QJsonObject& root) { return root["contexts"].toArray().first().toObject(); }
QJsonObject firstDrawing(const QJsonObject& root) { return firstContext(root)["drawings"].toArray().first().toObject(); }
}
class ChartDrawingsTest final:public QObject {
    Q_OBJECT
  private slots:
    void actualContextSelectionDoesNotWriteAnEmptyIdentity();
    void thousandBarHistoryPreservesAnchorsAndBoundedFrame();
    void publicGeometryRoundTripsExactUtcAndPriceAcrossIntradayIntervals();
    void independentMarketsSourcesAndDailyBundlesDoNotMerge();
    void shapeCardinalityPriceAndActualClosedMembershipAreStrict_data();
    void shapeCardinalityPriceAndActualClosedMembershipAreStrict();
    void hideLockAndClearOperateOnlyOnTheActiveContext();
    void wholeShapeMovesKeepStoredTimesAndRequireClosedBars();
    void undoAndRedoReplayShapeEditsPerChart();
    void privateOriginGeometryNeverPromotesAndLateGenerationCannotRecreateIt();
    void ohlcReadAndMagnetNeverUseFutureGapOrFormingCoordinates();
    void measureUsesObservedIntervalsAndKeepsPricePercentAcrossGap();
    void normalStockClosureCountsOneObservedInterval();
    void strictPublicRestoreRejectsUnknownPrivateFieldsAtomically_data();
    void strictPublicRestoreRejectsUnknownPrivateFieldsAtomically();
    void timestampStringsBeyondBinary64RemainExact();
    void stockCreationNeedsCurrentVerifiedPublicMetadata();
    void horizontalRulesHaveOneOwnerAndVisibilityDoesNotDisableThem();
    void transientContextsCannotEnableOrSerializeLineRules();
    void identicalSettersDoNotEmitOrDirtyPublicState();
    void perContextAndJsonBoundsRejectRatherThanSilentlyEvict();
    void fullCountryAndProviderIdentitiesNeverMerge();
    void decimalBoundaryPricesRemainExactPublicStrings();
    void contextAndTotalRestoreBoundsRejectAtomically_data();
    void contextAndTotalRestoreBoundsRejectAtomically();
    void publicMutationMustRemainRestorableWithinSerializedByteBudget();
    void everyGrowingMutationRespectsSerializedBudgetWithoutSignals_data();
    void everyGrowingMutationRespectsSerializedBudgetWithoutSignals();
    void v1BytesMigrateToV2SchemaWithDefaultDrawingFlags();
    void v2RoundTripPreservesFlagsAndRejectsUnknownKeysOrKinds();
    void newKindAnchorRulesRejectInvalidCardinalityPricesAndTimes();
    void applyBatchReplacesAtomicallyPreservesLockedAndRespectsLimits();
    void perDrawingVisibilityAndLockGovernEditsUndoAndAlerts();
    void transientBatchDoesNotLeakIntoPublicStateAndClearsWithPrivate();
    void fibLevelsMatchWaveEquationsAndDropNonPositivePrices();
    void alertLineRevisionIgnoresVisibilityAndLock();
    void undoKeepsVisibilityAndLockSetAfterTheRecordedEdit();
};
void ChartDrawingsTest::thousandBarHistoryPreservesAnchorsAndBoundedFrame(){
    ChartDrawings model;QVERIFY(model.setContext(context(),15*Minute));
    const auto f=frame(model,1000);const auto first=f.bars[10].candle.startsAtMs,last=f.bars[990].candle.startsAtMs;
    const auto id=model.create(ChartDrawingKind::Trend,{{first,"100.25"},{last,"103.50"}},f);QVERIFY(!id.isEmpty());
    const auto saved=model.publicState();QCOMPARE(model.drawings().first().anchors[0].atMs,first);
    QCOMPARE(model.drawings().first().anchors[1].atMs,last);
    const auto measure=ChartDrawings::measure(model.drawings().first().anchors,f);QVERIFY(measure.barIntervals);QCOMPARE(*measure.barIntervals,qint64(980));
    ChartDrawings restored;QVERIFY(restored.restorePublicState(saved));QVERIFY(restored.setContext(context(),15*Minute));
    QCOMPARE(restored.drawings(),model.drawings());
    auto wrong=f;wrong.identity={"coin","binance","SYNUSDT"};QVERIFY(model.create(ChartDrawingKind::Horizontal,{{first,"100"}},wrong).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::Horizontal,{{first,"100"}},frame(model,ChartDrawings::MaxFrameBars+1)).isEmpty());
}
void ChartDrawingsTest::actualContextSelectionDoesNotWriteAnEmptyIdentity() {
    ChartDrawings model; const auto initial=model.publicState();
    QVERIFY(model.setContext(context(),Minute)); QCOMPARE(model.publicState(),initial);
    QCOMPARE(model.context().identity,Coin); QVERIFY(model.contextGeneration()>0);
    const auto f=frame(model); const auto id=model.create(ChartDrawingKind::Horizontal,{{Start,"100.1250"}},f);
    QVERIFY(!id.isEmpty()); QCOMPARE(model.drawings().size(),1);
    const auto root=QJsonDocument::fromJson(model.publicState()).object();
    QCOMPARE(root["schema"].toString(),QString("keefi-chart-drawings/2"));
    QCOMPARE(root["contexts"].toArray().size(),1);
    QCOMPARE(firstDrawing(root)["anchors"].toArray().first().toObject()["price"].toString(),QString("100.1250"));
}
void ChartDrawingsTest::publicGeometryRoundTripsExactUtcAndPriceAcrossIntradayIntervals() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); const auto f=frame(model);
    const auto id=model.create(ChartDrawingKind::Trend,{{Start+Minute,"100.1250"},{Start+3*Minute,"102.50"}},f);
    QVERIFY(!id.isEmpty()); const auto bytes=model.publicState(); const auto anchors=model.drawings().first().anchors;
    for(const auto interval:{5*Minute,15*Minute,60*Minute,240*Minute}) {
        QVERIFY(model.setContext(context(),interval)); QCOMPARE(model.drawings().first().id,id);
        QCOMPARE(model.drawings().first().anchors,anchors); QCOMPARE(model.publicState(),bytes);
    }
    ChartDrawings restored; QVERIFY(restored.restorePublicState(bytes)); QVERIFY(restored.setContext(context(),Minute));
    QCOMPARE(restored.drawings(),model.drawings()); QCOMPARE(restored.publicState(),bytes);
}
void ChartDrawingsTest::independentMarketsSourcesAndDailyBundlesDoNotMerge() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute));
    const auto first=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)); QVERIFY(!first.isEmpty());
    QVERIFY(model.setContext({{"coin","binance","SYNUSDT"},"USDT",true},Minute)); QVERIFY(model.drawings().isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"2"}},frame(model)).isEmpty());
    QVERIFY(model.setContext(context(),Day)); QVERIFY(model.drawings().isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"3"}},frame(model)).isEmpty());
    QVERIFY(model.setContext(context(),Minute)); QCOMPARE(model.drawings().first().id,first);
    QCOMPARE(ChartDrawings::bucketForInterval(Minute),QString("intraday"));
    QCOMPARE(ChartDrawings::bucketForInterval(Day),QString("daily"));
    QVERIFY(ChartDrawings::bucketForInterval(2*Minute).isEmpty());
}
void ChartDrawingsTest::shapeCardinalityPriceAndActualClosedMembershipAreStrict_data() {
    QTest::addColumn<QString>("fault");
    for(const auto* value:{"horizontal-two","trend-one","trend-zero-length","price-LF","price-zero","price-exponent",
        "price-leadingzero","price-negative","price-nan","price-overbound","price-underbound","price-toolong",
        "missing-half-slot","future-slot","forming","wrong-identity","wrong-currency","stale-generation","frame-overbound"}) QTest::newRow(value)<<QString(value);
}
void ChartDrawingsTest::shapeCardinalityPriceAndActualClosedMembershipAreStrict() {
    QFETCH(QString,fault); ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); auto f=frame(model);
    auto kind=ChartDrawingKind::Horizontal; QList<ChartDrawingPoint> points{{Start,"100"}};
    if(fault=="horizontal-two") points.append({Start+Minute,"101"});
    if(fault=="trend-one") kind=ChartDrawingKind::Trend;
    if(fault=="trend-zero-length") { kind=ChartDrawingKind::Trend; points.append(points.first()); }
    if(fault=="price-LF") points[0].price="100\n";
    if(fault=="price-zero") points[0].price="0";
    if(fault=="price-exponent") points[0].price="1e2";
    if(fault=="price-leadingzero") points[0].price="0100";
    if(fault=="price-negative") points[0].price="-1";
    if(fault=="price-nan") points[0].price="NaN";
    if(fault=="price-overbound") points[0].price="1"+QString(100,'0')+".00000000000000000001";
    if(fault=="price-underbound") points[0].price="0."+QString(100,'0')+"1";
    if(fault=="price-toolong") points[0].price="100."+QString(ChartDrawings::MaxPriceChars,'0');
    if(fault=="missing-half-slot") points[0].atMs+=Minute/2;
    if(fault=="future-slot") points[0].atMs=Start+7*Minute;
    if(fault=="forming") f.bars[0].sourceCompleted=false;
    if(fault=="wrong-identity") f.identity={"coin","binance","SYNUSDT"};
    if(fault=="wrong-currency") f.currency="USDT";
    if(fault=="stale-generation") ++f.contextGeneration;
    if(fault=="frame-overbound") f=frame(model,ChartDrawings::MaxFrameBars+1);
    const auto before=model.publicState(); QString error;
    QVERIFY(model.create(kind,points,f,&error).isEmpty()); QVERIFY(!error.isEmpty()); QCOMPARE(model.publicState(),before);
}
void ChartDrawingsTest::wholeShapeMovesKeepStoredTimesAndRequireClosedBars() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); const auto f=frame(model);
    const auto trend=model.create(ChartDrawingKind::Trend,{{Start+Minute,"100"},{Start+3*Minute,"104"}},f); QVERIFY(!trend.isEmpty());
    QSignalSpy publicChanged(&model,&ChartDrawings::publicStateChanged);QString error;
    // Both anchors move one bar and up by 2 in a single change, keeping the slope.
    QVERIFY2(model.moveDrawing(trend,{{Start+2*Minute,"102"},{Start+4*Minute,"106"}},f,&error),qPrintable(error));
    QCOMPARE(model.drawings().first().anchors,(QList<ChartDrawingPoint>{{Start+2*Minute,"102"},{Start+4*Minute,"106"}}));QCOMPARE(publicChanged.size(),1);
    // A new time must be an actual closed bar; nothing changes on rejection.
    const auto before=model.publicState();
    QVERIFY(!model.moveDrawing(trend,{{Start+2*Minute+1,"102"},{Start+4*Minute,"106"}},f,&error));QVERIFY(!error.isEmpty());
    QVERIFY(!model.moveDrawing(trend,{{Start+2*Minute,"102"},{Start+40*Minute,"106"}},f,&error));
    QVERIFY(!model.moveDrawing(trend,{{Start+2*Minute,"102"}},f,&error));
    QVERIFY(!model.moveDrawing(trend,{{Start+2*Minute,"102"},{Start+2*Minute,"102"}},f,&error)); // zero length
    QCOMPARE(model.publicState(),before);
    // A price-only move keeps a stored time even when it is not a bar of the current frame
    // (for example a line drawn on another timeframe).
    QVERIFY(model.clear());const auto line=model.create(ChartDrawingKind::Horizontal,{{Start+Minute,"100"}},f);QVERIFY(!line.isEmpty());
    QVERIFY(model.setContext(context(),5*Minute));QCOMPARE(model.drawings().first().id,line);const auto five=frame(model);
    QVERIFY2(model.moveDrawing(line,{{Start+Minute,"99"}},five,&error),qPrintable(error));QCOMPARE(model.drawings().first().anchors.first(),(ChartDrawingPoint{Start+Minute,"99"}));
    QVERIFY(!model.moveDrawing(line,{{Start+2*Minute,"99"}},five,&error));
    QVERIFY2(model.moveDrawing(line,{{Start+5*Minute,"99"}},five,&error),qPrintable(error));
    model.setLocked(true);QVERIFY(!model.moveDrawing(line,{{Start+5*Minute,"98"}},five,&error));QCOMPARE(model.drawings().first().anchors.first().price,QString("99"));
}
void ChartDrawingsTest::hideLockAndClearOperateOnlyOnTheActiveContext() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); auto f=frame(model);
    const auto id=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},f); QVERIFY(!id.isEmpty());
    model.setVisible(false); QVERIFY(!model.visible()); model.setLocked(true); QVERIFY(model.locked());
    QVERIFY(!model.remove(id)); QVERIFY(!model.clear());
    QVERIFY(!model.moveAnchor(id,0,{Start+Minute,"101"},f));
    QVERIFY(model.create(ChartDrawingKind::Horizontal,{{Start,"2"}},f).isEmpty());
    QVERIFY(model.setContext(context(),Day)); QVERIFY(model.drawings().isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"3"}},frame(model)).isEmpty()); QVERIFY(model.clear());
    QVERIFY(model.setContext(context(),Minute)); QCOMPARE(model.drawings().first().id,id); QVERIFY(model.locked());
    model.setLocked(false); QVERIFY(model.clear()); QVERIFY(model.drawings().isEmpty());
}
void ChartDrawingsTest::privateOriginGeometryNeverPromotesAndLateGenerationCannotRecreateIt() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute));
    const auto publicId=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)); QVERIFY(!publicId.isEmpty());
    const auto publicBytes=model.publicState();
    QVERIFY(model.setContext(context(false),Minute)); QVERIFY(model.drawings().isEmpty()); auto late=frame(model);
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"987.654321"}},late).isEmpty());
    QCOMPARE(model.publicState(),publicBytes);
    QVERIFY(model.setContext(context(),Minute)); QCOMPARE(model.drawings().first().id,publicId); QCOMPARE(model.publicState(),publicBytes);
    model.clearPrivate(); QCOMPARE(model.publicState(),publicBytes);
    QVERIFY(model.create(ChartDrawingKind::Horizontal,{{Start,"987.654321"}},late).isEmpty());
    QVERIFY(!model.publicState().contains("987.654321"));
}
void ChartDrawingsTest::ohlcReadAndMagnetNeverUseFutureGapOrFormingCoordinates() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); auto f=frame(model);
    const auto bar=ChartDrawings::readOhlc(f,Start); QVERIFY(bar.has_value()); QCOMPARE(bar->close,QString("102"));
    const auto snap=ChartDrawings::snapOhlc(f,{Start,"101"},1,8);
    QVERIFY(snap.available); QVERIFY(snap.snapped); QCOMPARE(snap.field,QString("close")); QCOMPARE(snap.point.price,QString("102"));
    const auto far=ChartDrawings::snapOhlc(f,{Start,"300"},1,8);
    QVERIFY(far.available); QVERIFY(!far.snapped); QCOMPARE(far.point.price,QString("300"));
    QVERIFY(!ChartDrawings::readOhlc(f,Start+Minute/2).has_value());
    QVERIFY(!ChartDrawings::readOhlc(f,Start+7*Minute).has_value());
    f.bars[0].sourceCompleted=false;
    QVERIFY(!ChartDrawings::readOhlc(f,Start).has_value()); QVERIFY(!ChartDrawings::snapOhlc(f,{Start,"101"},1,8).available);
}
void ChartDrawingsTest::measureUsesObservedIntervalsAndKeepsPricePercentAcrossGap() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); auto f=frame(model);
    const QList<ChartDrawingPoint> points{{Start,"100"},{Start+2*Minute,"110"}};
    const auto continuous=ChartDrawings::measure(points,f); QVERIFY(continuous.available); QVERIFY(continuous.returnPercent.has_value());
    QVERIFY(std::abs(*continuous.returnPercent-10)<1e-10); QCOMPARE(continuous.barIntervals,std::optional<int>(2));
    f.bars.removeAt(1); f.bars[1].connectsPrevious=false;
    const auto missing=ChartDrawings::measure(points,f); QVERIFY(missing.available); QVERIFY(missing.returnPercent.has_value());
    QVERIFY(std::abs(*missing.returnPercent-10)<1e-10); QVERIFY(!missing.barIntervals.has_value()); QVERIFY(!missing.reason.isEmpty());
}
void ChartDrawingsTest::normalStockClosureCountsOneObservedInterval() {
    ChartDrawings model; QVERIFY(model.setContext({{"stock","KR","0101N0"},"KRW",true},Day)); auto f=frame(model,2);
    f.bars[1].candle.startsAtMs=Start+3*Day; f.bars[1].completesAtMs=Start+4*Day; f.asOfMs=Start+4*Day+1000;
    const auto result=ChartDrawings::measure({{Start,"100"},{Start+3*Day,"110"}},f);
    QVERIFY(result.available); QCOMPARE(result.barIntervals,std::optional<int>(1));
}
void ChartDrawingsTest::strictPublicRestoreRejectsUnknownPrivateFieldsAtomically_data() {
    QTest::addColumn<QString>("fault");
    for(const auto* value:{"root-private","context-account","shape-average","anchor-quantity","duplicate-context","duplicate-id","cross-context-id",
        "bad-utf8","numeric-time","time-leadingzero","time-overflow","price-LF","unknown-identity","holdings-alert-owner","visible-string"})
        QTest::newRow(value)<<QString(value);
}
void ChartDrawingsTest::strictPublicRestoreRejectsUnknownPrivateFieldsAtomically() {
    QFETCH(QString,fault); ChartDrawings model; QVERIFY(model.setContext(context(),Minute));
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)).isEmpty());
    const auto original=model.publicState(); auto root=QJsonDocument::fromJson(original).object();
    auto contexts=root["contexts"].toArray(); auto ctx=contexts.first().toObject(); auto drawings=ctx["drawings"].toArray();
    auto drawing=drawings.first().toObject(); auto anchors=drawing["anchors"].toArray(); auto anchor=anchors.first().toObject();
    if(fault=="root-private") root["privateData"]="FAKE_PRIVATE_CANARY";
    if(fault=="context-account") ctx["accountScope"]="FAKE_PRIVATE_CANARY";
    if(fault=="shape-average") drawing["providerAveragePrice"]="FAKE_PRIVATE_CANARY";
    if(fault=="anchor-quantity") anchor["quantity"]="FAKE_PRIVATE_CANARY";
    if(fault=="numeric-time") anchor["atMs"]=double(Start);
    if(fault=="time-leadingzero") anchor["atMs"]="0"+QString::number(Start);
    if(fault=="time-overflow") anchor["atMs"]="9223372036854775808";
    if(fault=="price-LF") anchor["price"]="100\n";
    if(fault=="unknown-identity") { auto identity=ctx["identity"].toObject(); identity["source"]="FAKE_PRIVATE_CANARY"; ctx["identity"]=identity; }
    if(fault=="holdings-alert-owner") drawing["alert"]=QJsonObject{{"enabled",true},{"listId","holdings"},{"intervalMs",Minute}};
    if(fault=="visible-string") ctx["visible"]="false";
    anchors[0]=anchor; drawing["anchors"]=anchors; drawings[0]=drawing;
    if(fault=="duplicate-id") drawings.append(drawing);
    ctx["drawings"]=drawings; contexts[0]=ctx;
    if(fault=="duplicate-context") contexts.append(ctx);
    if(fault=="cross-context-id") { auto other=ctx; auto identity=other["identity"].toObject(); identity["source"]="bithumb"; other["identity"]=identity; contexts.append(other); }
    root["contexts"]=contexts; auto bytes=encoded(root); if(fault=="bad-utf8") bytes.append(char(0xff)); QString error;
    QVERIFY(!model.restorePublicState(bytes,&error)); QVERIFY(!error.isEmpty()); QCOMPARE(model.publicState(),original);
    QVERIFY(!model.publicState().contains("FAKE_PRIVATE_CANARY"));
}
void ChartDrawingsTest::timestampStringsBeyondBinary64RemainExact() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute));
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)).isEmpty());
    auto root=QJsonDocument::fromJson(model.publicState()).object(); auto contexts=root["contexts"].toArray(); auto ctx=contexts.first().toObject();
    auto drawings=ctx["drawings"].toArray(); auto drawing=drawings.first().toObject(); auto anchors=drawing["anchors"].toArray(); auto anchor=anchors.first().toObject();
    anchor["atMs"]="9007199254740993"; anchors[0]=anchor; drawing["anchors"]=anchors; drawings[0]=drawing; ctx["drawings"]=drawings; contexts[0]=ctx; root["contexts"]=contexts;
    QVERIFY(model.restorePublicState(encoded(root))); QCOMPARE(model.drawings().first().anchors.first().atMs,qint64(9007199254740993LL));
    QCOMPARE(firstDrawing(QJsonDocument::fromJson(model.publicState()).object())["anchors"].toArray().first().toObject()["atMs"].toString(),QString("9007199254740993"));
}
void ChartDrawingsTest::stockCreationNeedsCurrentVerifiedPublicMetadata() {
    ChartDrawings model; const WatchlistIdentity stock{"stock","KR","0101N0"};
    QVERIFY(model.setContext({stock,"KRW",true},Minute)); const auto f=frame(model);
    QVERIFY(model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},f).isEmpty());
    model.setVerifiedInstruments({stock}); QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},f).isEmpty());
    const auto bytes=model.publicState(); model.setVerifiedInstruments({}); QCOMPARE(model.publicState(),bytes);
    QVERIFY(model.create(ChartDrawingKind::Horizontal,{{Start+Minute,"101"}},f).isEmpty());
}
void ChartDrawingsTest::horizontalRulesHaveOneOwnerAndVisibilityDoesNotDisableThem() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); model.setPublicLists({{"public-list",{Coin}},{"second-public-list",{Coin}}});
    const auto id=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)); QVERIFY(!id.isEmpty());
    QVERIFY(model.setAlert(id,{true,"public-list",5*Minute})); const auto rules=model.horizontalLines(); QCOMPARE(rules.size(),1);
    QCOMPARE(rules.first().drawingId,id); QCOMPARE(rules.first().identity,Coin); QCOMPARE(rules.first().intervalMs,5*Minute);
    QVERIFY(!rules.first().revision.isEmpty()); const auto revision=rules.first().revision;
    model.setVisible(false); model.setLocked(true); QCOMPARE(model.horizontalLines().size(),1); QCOMPARE(model.horizontalLines().first().revision,revision);
    const auto lockedBytes=model.publicState();
    QVERIFY(!model.setAlert(id,{false,{},0})); QCOMPARE(model.publicState(),lockedBytes);
    QVERIFY(!model.setAlert(id,{true,"public-list",15*Minute})); QCOMPARE(model.publicState(),lockedBytes);
    QVERIFY(!model.setAlert(id,{true,"second-public-list",5*Minute})); QCOMPARE(model.publicState(),lockedBytes);
    QCOMPARE(model.horizontalLines().size(),1); QCOMPARE(model.horizontalLines().first().revision,revision);
    QVERIFY(model.setContext(context(),60*Minute)); QCOMPARE(model.horizontalLines().first().intervalMs,5*Minute); QCOMPARE(model.horizontalLines().first().revision,revision);
    model.setPublicLists({}); QVERIFY(model.horizontalLines().isEmpty());
    model.setPublicLists({{"public-list",{Coin}}}); QCOMPARE(model.horizontalLines().size(),1);
}
void ChartDrawingsTest::transientContextsCannotEnableOrSerializeLineRules() {
    ChartDrawings model; QVERIFY(model.setContext(context(false),Minute)); model.setPublicLists({{"public-list",{Coin}}});
    const auto before=model.publicState(); const auto id=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)); QVERIFY(!id.isEmpty());
    QVERIFY(!model.setAlert(id,{true,"public-list",Minute})); QVERIFY(model.horizontalLines().isEmpty()); QCOMPARE(model.publicState(),before);
}
void ChartDrawingsTest::identicalSettersDoNotEmitOrDirtyPublicState() {
    ChartDrawings model; QSignalSpy writes(&model,&ChartDrawings::publicStateChanged);
    QVERIFY(model.setContext(context(),Minute)); QCOMPARE(writes.size(),0);
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)).isEmpty()); QCOMPARE(writes.size(),1);
    model.setVisible(false); const int after=writes.size(); const auto state=model.publicState();
    model.setVisible(false); model.setLocked(false); model.setMagnetEnabled(false);
    QCOMPARE(writes.size(),after); QCOMPARE(model.publicState(),state);
}
void ChartDrawingsTest::perContextAndJsonBoundsRejectRatherThanSilentlyEvict() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); const auto f=frame(model);
    for(int i=0;i<ChartDrawings::MaxPerContext;++i) QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,QString::number(100+i)}},f).isEmpty());
    const auto before=model.publicState(); QVERIFY(model.create(ChartDrawingKind::Horizontal,{{Start,"1000"}},f).isEmpty()); QCOMPARE(model.publicState(),before);
    QString error; QVERIFY(!model.restorePublicState(QByteArray(ChartDrawings::MaxJsonBytes+1,' '),&error)); QCOMPARE(model.publicState(),before);
}
void ChartDrawingsTest::fullCountryAndProviderIdentitiesNeverMerge() {
    ChartDrawings model;
    const QList<ChartDrawingContext> contexts{{{"stock","KR","ABCDEF"},"KRW",true},{{"stock","US","ABCDEF"},"USD",true},
        {{"coin","upbit","KRW-SYN"},"KRW",true},{{"coin","bithumb","KRW-SYN"},"KRW",true}};
    model.setVerifiedInstruments({contexts[0].identity,contexts[1].identity}); QStringList ids;
    for(int i=0;i<contexts.size();++i) {
        QVERIFY(model.setContext(contexts[i],Minute)); QVERIFY(model.drawings().isEmpty());
        const auto id=model.create(ChartDrawingKind::Horizontal,{{Start,QString::number(100+i)}},frame(model)); QVERIFY(!id.isEmpty()); ids.append(id);
    }
    for(int i=0;i<contexts.size();++i) { QVERIFY(model.setContext(contexts[i],Minute)); QCOMPARE(model.drawings().size(),1); QCOMPARE(model.drawings().first().id,ids[i]); }
    const auto before=model.publicState();
    QVERIFY(!model.setContext({{"coin","unknown","KRW-SYN"},"KRW",true},Minute)); QCOMPARE(model.publicState(),before);
    QVERIFY(!model.setContext({{"stock","kr","ABCDEF"},"KRW",true},Minute)); QCOMPARE(model.publicState(),before);
}
void ChartDrawingsTest::decimalBoundaryPricesRemainExactPublicStrings() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); const auto f=frame(model);
    const QString minimum="0."+QString(99,'0')+"1",maximum="1"+QString(100,'0');
    const auto low=model.create(ChartDrawingKind::Horizontal,{{Start,minimum}},f); QVERIFY(!low.isEmpty());
    const auto high=model.create(ChartDrawingKind::Horizontal,{{Start+Minute,maximum}},f); QVERIFY(!high.isEmpty());
    QCOMPARE(model.drawings()[0].anchors[0].price,minimum); QCOMPARE(model.drawings()[1].anchors[0].price,maximum);
    const auto bytes=model.publicState(); ChartDrawings restored; QVERIFY(restored.restorePublicState(bytes)); QVERIFY(restored.setContext(context(),Minute));
    QCOMPARE(restored.drawings()[0].anchors[0].price,minimum); QCOMPARE(restored.drawings()[1].anchors[0].price,maximum); QCOMPARE(restored.publicState(),bytes);
}
void ChartDrawingsTest::contextAndTotalRestoreBoundsRejectAtomically_data() {
    QTest::addColumn<QString>("bound"); QTest::newRow("contexts")<<QString("contexts"); QTest::newRow("total")<<QString("total");
}
void ChartDrawingsTest::contextAndTotalRestoreBoundsRejectAtomically() {
    QFETCH(QString,bound); ChartDrawings model; QVERIFY(model.setContext(context(),Minute));
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)).isEmpty());
    const auto original=model.publicState(); auto root=QJsonDocument::fromJson(original).object();
    const auto templateContext=firstContext(root),templateDrawing=firstDrawing(root); QJsonArray contexts;
    const int count=bound=="contexts"?ChartDrawings::MaxContexts:ChartDrawings::MaxTotal/ChartDrawings::MaxPerContext;
    for(int i=0;i<count;++i) {
        auto ctx=templateContext; auto identity=ctx["identity"].toObject(); identity["code"]="KRW-SYN"+QString::number(i); ctx["identity"]=identity;
        QJsonArray drawings;
        for(int j=0;j<(bound=="contexts"?1:ChartDrawings::MaxPerContext);++j) {
            auto drawing=templateDrawing; drawing["id"]=QUuid::createUuid().toString(QUuid::WithoutBraces); drawings.append(drawing);
        }
        ctx["drawings"]=drawings; contexts.append(ctx);
    }
    root["contexts"]=contexts; QVERIFY(model.restorePublicState(encoded(root))); const auto atLimit=model.publicState();
    auto extra=templateContext; auto identity=extra["identity"].toObject(); identity["code"]="KRW-EXTRASYN"; extra["identity"]=identity;
    auto drawing=templateDrawing; drawing["id"]=QUuid::createUuid().toString(QUuid::WithoutBraces); extra["drawings"]=QJsonArray{drawing}; contexts.append(extra); root["contexts"]=contexts;
    QString error; QVERIFY(!model.restorePublicState(encoded(root),&error)); QVERIFY(!error.isEmpty()); QCOMPARE(model.publicState(),atLimit);
}
void ChartDrawingsTest::publicMutationMustRemainRestorableWithinSerializedByteBudget() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute));
    QVERIFY(!model.create(ChartDrawingKind::Trend,{{Start,"100"},{Start+Minute,"100"}},frame(model)).isEmpty());
    const auto baseRoot=QJsonDocument::fromJson(model.publicState()).object();
    const auto templateContext=firstContext(baseRoot),templateDrawing=firstDrawing(baseRoot);
    const QString longPrice="100."+QString(ChartDrawings::MaxPriceChars-4,'0');
    const auto build=[&](int count){
        auto root=baseRoot; QJsonArray contexts; int remainder=count-20,identifier=1;
        for(int index=0;index<20;++index){
            auto ctx=templateContext;auto identity=ctx["identity"].toObject();
            identity["code"]=index==0?QString("KRW-SYN"):QString("KRW-SYN")+QString::number(index);ctx["identity"]=identity;
            const int extra=index==0?0:std::min(99,remainder);remainder-=extra;QJsonArray drawings;
            for(int item=0;item<1+extra;++item){auto drawing=templateDrawing;
                drawing["id"]=QString("%1").arg(identifier++,8,16,QLatin1Char('0'))+"-0000-4000-8000-000000000000";
                drawing["anchors"]=QJsonArray{QJsonObject{{"atMs",QString::number(Start)},{"price",longPrice}},
                    QJsonObject{{"atMs",QString::number(Start+Minute)},{"price",longPrice}}};drawings.append(drawing);}
            ctx["drawings"]=drawings;contexts.append(ctx);
        }
        root["contexts"]=contexts;return encoded(root);
    };
    int lower=20,upper=1901;
    QVERIFY(build(upper).size()>ChartDrawings::MaxJsonBytes);
    while(lower<upper){const int middle=(lower+upper+1)/2;if(build(middle).size()<=ChartDrawings::MaxJsonBytes)lower=middle;else upper=middle-1;}
    QVERIFY(lower<ChartDrawings::MaxTotal); QVERIFY(model.restorePublicState(build(lower)));
    const auto before=model.publicState(); QVERIFY(before.size()<=ChartDrawings::MaxJsonBytes); QCOMPARE(model.drawings().size(),1);
    QSignalSpy changed(&model,&ChartDrawings::changed),persisted(&model,&ChartDrawings::publicStateChanged),lines(&model,&ChartDrawings::horizontalLinesChanged);
    QString error;const auto id=model.create(ChartDrawingKind::Trend,{{Start,longPrice},{Start+Minute,longPrice}},frame(model),&error);
    QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); QCOMPARE(model.publicState(),before);
    QVERIFY(model.publicState().size()<=ChartDrawings::MaxJsonBytes);
    QCOMPARE(changed.count(),0);QCOMPARE(persisted.count(),0);QCOMPARE(lines.count(),0);
}
void ChartDrawingsTest::everyGrowingMutationRespectsSerializedBudgetWithoutSignals_data() {
    QTest::addColumn<QString>("mutation");
    for(const auto* value:{"new-context","move","alert","visible","unlock","magnet"})QTest::newRow(value)<<QString(value);
}
void ChartDrawingsTest::everyGrowingMutationRespectsSerializedBudgetWithoutSignals() {
    QFETCH(QString,mutation);ChartDrawings model;QVERIFY(model.setContext(context(),Minute));
    QVERIFY(!model.create(ChartDrawingKind::Trend,{{Start,"100"},{Start+Minute,"100"}},frame(model)).isEmpty());
    auto root=QJsonDocument::fromJson(model.publicState()).object();const auto templateContext=firstContext(root),templateDrawing=firstDrawing(root);
    const QString longPrice="100."+QString(ChartDrawings::MaxPriceChars-4,'0');QJsonArray contexts;int identifier=1;
    for(int index=0;index<20;++index){auto ctx=templateContext;auto identity=ctx["identity"].toObject();
        identity["code"]=index==0?QString("KRW-SYN"):QString("KRW-SYN")+QString::number(index);ctx["identity"]=identity;QJsonArray drawings;
        for(int item=0;item<(index==0?1:100);++item){auto drawing=templateDrawing;
            drawing["id"]=QString("%1").arg(identifier++,8,16,QLatin1Char('0'))+"-0000-4000-8000-000000000000";
            drawing["anchors"]=QJsonArray{QJsonObject{{"atMs",QString::number(Start)},{"price",longPrice}},QJsonObject{{"atMs",QString::number(Start+Minute)},{"price",longPrice}}};
            if(index==0&&mutation=="move"){auto anchors=drawing["anchors"].toArray();auto point=anchors[0].toObject();point["price"]="100";anchors[0]=point;drawing["anchors"]=anchors;}
            if(index==0&&mutation=="alert"){drawing["kind"]="horizontal";drawing["anchors"]=QJsonArray{drawing["anchors"].toArray().first()};drawing["alert"]=QJsonObject{{"enabled",true},{"listId","a"},{"intervalMs",Minute}};}
            drawings.append(drawing);}
        ctx["drawings"]=drawings;if(index==0&&mutation=="unlock")ctx["locked"]=true;contexts.append(ctx);}
    root["contexts"]=contexts;if(mutation=="magnet")root["magnet"]=true;
    int excess=int(encoded(root).size())-ChartDrawings::MaxJsonBytes;QVERIFY(excess>0);
    // Shrink only synthetic filler anchors, preserving the active mutation's boundary.
    for(int index=1;index<contexts.size()&&excess>0;++index){auto ctx=contexts[index].toObject();auto drawings=ctx["drawings"].toArray();
        for(int item=0;item<drawings.size()&&excess>0;++item){auto drawing=drawings[item].toObject();auto anchors=drawing["anchors"].toArray();
            for(int pointIndex=0;pointIndex<anchors.size()&&excess>0;++pointIndex){auto point=anchors[pointIndex].toObject();auto price=point["price"].toString();const int trim=std::min(excess,int(price.size())-5);price.chop(trim);excess-=trim;point["price"]=price;anchors[pointIndex]=point;}
            drawing["anchors"]=anchors;drawings[item]=drawing;}
        ctx["drawings"]=drawings;contexts[index]=ctx;}
    QCOMPARE(excess,0);root["contexts"]=contexts;QCOMPARE(encoded(root).size(),qsizetype(ChartDrawings::MaxJsonBytes));QVERIFY(model.restorePublicState(encoded(root)));
    const QString longList(64,'a');model.setPublicLists({{"a",{Coin}},{longList,{Coin}}});
    if(mutation=="new-context")QVERIFY(model.setContext({{"coin","upbit","KRW-NEWSYN"},"KRW",true},Minute));
    const auto before=model.publicState();const auto drawings=model.drawings();const auto generation=model.contextGeneration();
    QSignalSpy changed(&model,&ChartDrawings::changed),persisted(&model,&ChartDrawings::publicStateChanged),lines(&model,&ChartDrawings::horizontalLinesChanged);QString error;
    if(mutation=="new-context")QVERIFY(model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model),&error).isEmpty());
    if(mutation=="move")QVERIFY(!model.moveAnchor(drawings.first().id,0,{Start,longPrice},frame(model),&error));
    if(mutation=="alert")QVERIFY(!model.setAlert(drawings.first().id,{true,longList,240*Minute},&error));
    if(mutation=="visible"){model.setVisible(false);QVERIFY(model.visible());}
    if(mutation=="unlock"){model.setLocked(false);QVERIFY(model.locked());}
    if(mutation=="magnet"){model.setMagnetEnabled(false);QVERIFY(model.magnetEnabled());}
    if(mutation=="new-context"||mutation=="move"||mutation=="alert")QVERIFY(!error.isEmpty());
    QCOMPARE(model.publicState(),before);QCOMPARE(model.drawings(),drawings);QCOMPARE(model.contextGeneration(),generation);
    QCOMPARE(changed.count(),0);QCOMPARE(persisted.count(),0);QCOMPARE(lines.count(),0);
    ChartDrawings restored;QVERIFY(restored.restorePublicState(model.publicState()));QCOMPARE(restored.publicState(),before);
}
// User request (2026-11-05): undo for drawing. Create, move, delete and clear are undone in
// reverse order and redone; a new edit drops the redo branch; alerts of surviving lines are
// never rolled back; each chart keeps its own history; private (holdings) history is dropped.
void ChartDrawingsTest::undoAndRedoReplayShapeEditsPerChart() {
    ChartDrawings model; QVERIFY(model.setContext(context(),Minute)); model.setPublicLists({{"public-list",{Coin}}}); const auto f=frame(model);
    QVERIFY(!model.canUndo()); QVERIFY(!model.undo()); QVERIFY(!model.canRedo());
    const auto empty=model.publicState();
    const auto line=model.create(ChartDrawingKind::Horizontal,{{Start+Minute,"100"}},f); QVERIFY(!line.isEmpty());
    QVERIFY(model.moveDrawing(line,{{Start+Minute,"103"}},f)); const auto afterMove=model.publicState();
    const auto trend=model.create(ChartDrawingKind::Trend,{{Start+Minute,"100"},{Start+3*Minute,"104"}},f); QVERIFY(!trend.isEmpty());
    QVERIFY(model.remove(trend)); QCOMPARE(model.publicState(),afterMove);
    // The alert on the line is a setting, not an edit: undoing the move keeps it on.
    QVERIFY(model.setAlert(line,{true,"public-list",5*Minute})); QCOMPARE(model.horizontalLines().size(),1);
    QSignalSpy changed(&model,&ChartDrawings::changed);
    QVERIFY(model.canUndo()); QVERIFY(model.undo()); QCOMPARE(model.drawings().size(),2); QCOMPARE(model.drawings().last().id,trend); QVERIFY(changed.count()>=1);
    QVERIFY(model.undo()); QCOMPARE(model.drawings().size(),1);
    QVERIFY(model.undo()); QCOMPARE(model.drawings().first().anchors.first().price,QString("100"));
    QVERIFY(model.drawings().first().alert.enabled); QCOMPARE(model.horizontalLines().size(),1);
    QVERIFY(model.undo()); QVERIFY(model.drawings().isEmpty()); QCOMPARE(model.publicState(),empty); QVERIFY(!model.canUndo());
    // Redo walks forward again and brings back each list as it was just before it was undone.
    QVERIFY(model.canRedo()); QVERIFY(model.redo()); QCOMPARE(model.drawings().size(),1);
    QCOMPARE(model.drawings().first().anchors.first().price,QString("100")); QVERIFY(model.drawings().first().alert.enabled);
    QVERIFY(model.redo()); QCOMPARE(model.drawings().first().anchors.first().price,QString("103"));
    // A new edit drops the remaining redo steps.
    QVERIFY(model.moveDrawing(line,{{Start+Minute,"101"}},f)); QVERIFY(!model.canRedo()); QVERIFY(!model.redo());
    // A locked chart neither undoes nor redoes.
    model.setLocked(true); QVERIFY(!model.canUndo()); QVERIFY(!model.undo()); model.setLocked(false); QVERIFY(model.canUndo());
    // Each chart (identity and interval bucket) has its own history.
    QVERIFY(model.setContext(context(),Day)); QVERIFY(!model.canUndo());
    const auto daily=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)); QVERIFY(!daily.isEmpty());
    QVERIFY(model.undo()); QVERIFY(model.drawings().isEmpty());
    QVERIFY(model.setContext(context(),Minute)); QVERIFY(model.canUndo()); QVERIFY(model.undo()); QCOMPARE(model.drawings().first().anchors.first().price,QString("103"));
    // Undo history is bounded per chart.
    for(int i=0;i<ChartDrawings::MaxUndoSteps+10;++i)QVERIFY(model.moveDrawing(line,{{Start+Minute,QString::number(110+i)}},frame(model)));
    int steps=0;while(model.undo())++steps; QCOMPARE(steps,ChartDrawings::MaxUndoSteps);
    // A chart opened from holdings is transient; its history disappears with the private data.
    QVERIFY(model.setContext(context(false),Minute)); QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start+Minute,"100"}},frame(model)).isEmpty());
    QVERIFY(model.canUndo()); model.clearPrivate(); QVERIFY(model.setContext(context(false),Minute)); QVERIFY(!model.canUndo());
    // Restoring saved public state starts a fresh public history.
    QVERIFY(model.setContext(context(),Minute)); QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start+Minute,"99"}},frame(model)).isEmpty());
    QVERIFY(model.canUndo()); QVERIFY(model.restorePublicState(empty)); QVERIFY(!model.canUndo());
}
void ChartDrawingsTest::v1BytesMigrateToV2SchemaWithDefaultDrawingFlags() {
    const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto v1Json=[&](const QString& schema,const QString& kind,const QJsonArray& anchors){
        return encoded(QJsonObject{
            {"schema",schema},
            {"magnet",true},
            {"contexts",QJsonArray{QJsonObject{
                {"identity",QJsonObject{{"market","coin"},{"source","upbit"},{"code","KRW-SYN"}}},
                {"currency","KRW"},
                {"bucket","intraday"},
                {"visible",true},
                {"locked",false},
                {"drawings",QJsonArray{QJsonObject{
                    {"id",id},
                    {"kind",kind},
                    {"anchors",anchors},
                    {"alert",QJsonObject{{"enabled",false},{"listId",""},{"intervalMs",0}}}
                }}}
            }}}
        });
    };
    const QJsonArray trendAnchors{
        QJsonObject{{"atMs",QString::number(Start)},{"price","100"}},
        QJsonObject{{"atMs",QString::number(Start+Minute)},{"price","104"}}
    };
    for(const QString& schema:{QString("keefi-chart-drawings/1"),QString("finix-chart-drawings/1")}){
        ChartDrawings model;QVERIFY(model.restorePublicState(v1Json(schema,"trend",trendAnchors)));
        QVERIFY(model.setContext(context(),Minute));
        QCOMPARE(model.drawings().size(),1);
        QCOMPARE(model.drawings().first().id,id);
        QCOMPARE(model.drawings().first().visible,true);
        QCOMPARE(model.drawings().first().locked,false);
        QVERIFY(model.drawings().first().autoBatchId.isEmpty());
        QVERIFY(model.magnetEnabled());
        const auto root=QJsonDocument::fromJson(model.publicState()).object();
        QCOMPARE(root["schema"].toString(),QString("keefi-chart-drawings/2"));
        const auto obj=firstDrawing(root);
        QCOMPARE(obj.size(),7);
        QCOMPARE(obj["visible"].toBool(),true);
        QCOMPARE(obj["locked"].toBool(),false);
        QCOMPARE(obj["autoBatchId"].toString(),QString(""));
    }
    ChartDrawings strictV1;
    QVERIFY(!strictV1.restorePublicState(v1Json("keefi-chart-drawings/1","fib-retracement",trendAnchors)));
}
void ChartDrawingsTest::v2RoundTripPreservesFlagsAndRejectsUnknownKeysOrKinds() {
    ChartDrawings model;QVERIFY(model.setContext(context(),Minute));const auto f=frame(model);
    const auto manual=model.create(ChartDrawingKind::FibRetracement,{{Start,"63900"},{Start+2*Minute,"64400"}},f);
    QVERIFY(!manual.isEmpty());
    QVERIFY(model.setDrawingVisible(manual,false));
    QVERIFY(model.setDrawingLocked(manual,true));
    QVERIFY(model.applyBatch({ChartDrawing{{},ChartDrawingKind::Zone,{{Start,"100"},{Start+3*Minute,"105"}}}},f));
    const auto bytes=model.publicState();
    ChartDrawings restored;QVERIFY(restored.restorePublicState(bytes));
    QCOMPARE(restored.publicState(),bytes);
    QVERIFY(restored.setContext(context(),Minute));
    QCOMPARE(restored.drawings(),model.drawings());
    QCOMPARE(restored.drawings().first().visible,false);
    QCOMPARE(restored.drawings().first().locked,true);
    QVERIFY(restored.drawings().first().autoBatchId.isEmpty());
    QVERIFY(!restored.drawings().last().autoBatchId.isEmpty());

    const auto checkRejected=[&](const QJsonObject& mutatedDrawing){
        auto root=QJsonDocument::fromJson(bytes).object();
        auto contexts=root["contexts"].toArray();auto ctx=contexts.first().toObject();
        ctx["drawings"]=QJsonArray{mutatedDrawing};contexts[0]=ctx;root["contexts"]=contexts;
        QString error;
        QVERIFY(!restored.restorePublicState(encoded(root),&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(restored.publicState(),bytes);
    };
    const auto base=firstDrawing(QJsonDocument::fromJson(bytes).object());
    {auto extra=base;extra["unknownKey"]=123;checkRejected(extra);}
    {auto missing=base;missing.remove("autoBatchId");checkRejected(missing);}
    {auto badKind=base;badKind["kind"]="bad-kind";checkRejected(badKind);}
    {auto badBatch=base;badBatch["autoBatchId"]="NOT-A-LOWERCASE-UUID";checkRejected(badBatch);}
    {auto upperBatch=base;upperBatch["autoBatchId"]=base["id"].toString().toUpper();checkRejected(upperBatch);}
}
void ChartDrawingsTest::newKindAnchorRulesRejectInvalidCardinalityPricesAndTimes() {
    ChartDrawings model;QVERIFY(model.setContext(context(),Minute));const auto f=frame(model);
    QVERIFY(ChartDrawings::validFrame(f));
    auto badFrame=f;badFrame.bars.first().sourceCompleted=false;
    QVERIFY(!ChartDrawings::validFrame(badFrame));
    QString err;
    QVERIFY(model.create(ChartDrawingKind::FibRetracement,{{Start,"100"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::FibRetracement,{{Start,"100"},{Start+Minute,"100.00"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::FibRetracement,{{Start,"100"},{Start+Minute,"101"},{Start+2*Minute,"102"}},f,&err).isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::FibRetracement,{{Start,"100"},{Start,"105"}},f,&err).isEmpty());

    QVERIFY(model.create(ChartDrawingKind::FibExtension,{{Start,"100"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::FibExtension,{{Start,"100"},{Start+Minute,"100.0"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::FibExtension,{{Start,"100"},{Start+Minute,"105"},{Start+2*Minute,"102"},{Start+3*Minute,"108"}},f,&err).isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::FibExtension,{{Start,"100"},{Start+Minute,"105"}},f,&err).isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::FibExtension,{{Start,"100"},{Start+Minute,"105"},{Start+2*Minute,"102"}},f,&err).isEmpty());

    QVERIFY(model.create(ChartDrawingKind::Zone,{{Start,"100"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::Zone,{{Start+Minute,"100"},{Start+Minute,"105"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::Zone,{{Start,"100"},{Start+Minute,"100.00"}},f,&err).isEmpty());
    QVERIFY(model.create(ChartDrawingKind::Zone,{{Start,"100"},{Start+Minute,"105"},{Start+2*Minute,"108"}},f,&err).isEmpty());
    QVERIFY(!model.create(ChartDrawingKind::Zone,{{Start,"100"},{Start+Minute,"105"}},f,&err).isEmpty());
}
void ChartDrawingsTest::applyBatchReplacesAtomicallyPreservesLockedAndRespectsLimits() {
    ChartDrawings model;QVERIFY(model.setContext(context(),Minute));const auto f=frame(model);
    const auto manual=model.create(ChartDrawingKind::Horizontal,{{Start,"99"}},f);QVERIFY(!manual.isEmpty());
    const QList<ChartDrawing> firstBatch{
        {QStringLiteral("ignored-id"),ChartDrawingKind::FibRetracement,{{Start,"100"},{Start+2*Minute,"105"}},{true,"a",Minute},false,true,QStringLiteral("ignored-batch")},
        {{},ChartDrawingKind::Zone,{{Start,"101"},{Start+3*Minute,"104"}}}
    };
    QString err;QVERIFY2(model.applyBatch(firstBatch,f,&err),qPrintable(err));
    QCOMPARE(model.drawings().size(),3);
    const QString batch1=model.drawings()[1].autoBatchId;
    QVERIFY(!batch1.isEmpty());
    QCOMPARE(model.drawings()[2].autoBatchId,batch1);
    QVERIFY(model.drawings()[1].id!=QStringLiteral("ignored-id")&&model.drawings()[1].id!=batch1);
    QCOMPARE(model.drawings()[1].visible,true);QCOMPARE(model.drawings()[1].locked,false);
    QVERIFY(!model.drawings()[1].alert.enabled);

    // 1 undo removes the entire batch; 1 redo restores it.
    QVERIFY(model.undo());QCOMPARE(model.drawings().size(),1);QCOMPARE(model.drawings().first().id,manual);
    QVERIFY(model.redo());QCOMPARE(model.drawings().size(),3);QCOMPARE(model.drawings()[1].autoBatchId,batch1);

    // Lock one auto drawing, then apply a second batch: unlocked auto shape is replaced, locked stays.
    const QString keptAuto=model.drawings()[1].id;
    QVERIFY(model.setDrawingLocked(keptAuto,true));
    QVERIFY2(model.applyBatch({{{},ChartDrawingKind::FibExtension,{{Start,"100"},{Start+Minute,"104"},{Start+2*Minute,"101"}}}},f,&err),qPrintable(err));
    QCOMPARE(model.drawings().size(),3);
    QCOMPARE(model.drawings()[0].id,manual);
    QCOMPARE(model.drawings()[1].id,keptAuto);
    const QString batch2=model.drawings()[2].autoBatchId;
    QVERIFY(!batch2.isEmpty()&&batch2!=batch1);
    // 1 undo reverts the second batch back to the first batch.
    QVERIFY(model.undo());QCOMPARE(model.drawings().size(),3);
    QCOMPARE(model.drawings()[1].id,keptAuto);
    QCOMPARE(model.drawings()[2].autoBatchId,batch1);
    QVERIFY(model.setDrawingLocked(keptAuto,false));
    QVERIFY(model.applyBatch({{{},ChartDrawingKind::Zone,{{Start,"100"},{Start+Minute,"105"}}}},f,&err));
    QCOMPARE(model.drawings().size(),2);

    // Reject non-closed bar, context lock, and overflow atomically without touching bytes or signals.
    const auto stableBytes=model.publicState();const auto stableDrawings=model.drawings();
    QSignalSpy changed(&model,&ChartDrawings::changed),persisted(&model,&ChartDrawings::publicStateChanged);
    QVERIFY(!model.applyBatch({{{},ChartDrawingKind::Zone,{{Start+Minute/2,"100"},{Start+Minute,"105"}}}},f,&err));
    QVERIFY(!err.isEmpty());QCOMPARE(model.publicState(),stableBytes);QCOMPARE(model.drawings(),stableDrawings);
    model.setLocked(true);
    const auto lockedBytes=model.publicState();
    QVERIFY(!model.applyBatch({{{},ChartDrawingKind::Zone,{{Start,"100"},{Start+2*Minute,"106"}}}},f,&err));
    QCOMPARE(model.publicState(),lockedBytes);
    model.setLocked(false);
    changed.clear();persisted.clear();
    QList<ChartDrawing> tooMany;
    for(int i=0;i<ChartDrawings::MaxPerContext;++i)
        tooMany.append({{},ChartDrawingKind::Horizontal,{{Start,QString::number(100+i)}}});
    QVERIFY(!model.applyBatch(tooMany,f,&err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(model.publicState(),stableBytes);
    QCOMPARE(model.drawings(),stableDrawings);
    QCOMPARE(changed.count(),0);QCOMPARE(persisted.count(),0);

    // MaxTotal (>2000 across contexts) rejects atomically even when per-context and byte limits are respected.
    {
        auto totalRoot=QJsonDocument::fromJson(stableBytes).object();
        auto totalCtxs=totalRoot["contexts"].toArray();auto tmplCtx=totalCtxs.first().toObject();
        auto tmplDrw=tmplCtx["drawings"].toArray().first().toObject();
        tmplDrw["kind"]="horizontal";tmplDrw["autoBatchId"]="";tmplDrw["locked"]=false;
        tmplDrw["anchors"]=QJsonArray{QJsonObject{{"atMs",QString::number(Start)},{"price","100"}}};
        int sid=1;
        for(int c=1;c<=20;++c){
            auto ctx=tmplCtx;auto ident=ctx["identity"].toObject();ident["code"]="KRW-TOT"+QString::number(c);ctx["identity"]=ident;
            QJsonArray drws;
            for(int j=0;j<98;++j){
                auto d=tmplDrw;d["id"]=QString("%1").arg(sid++,8,16,QLatin1Char('0'))+"-0000-4000-8000-000000000000";drws.append(d);
            }
            ctx["drawings"]=drws;totalCtxs.append(ctx);
        }
        totalRoot["contexts"]=totalCtxs;
        ChartDrawings totalBound;QVERIFY(totalBound.restorePublicState(encoded(totalRoot)));QVERIFY(totalBound.setContext(context(),Minute));
        const auto tbBefore=totalBound.publicState();
        QList<ChartDrawing> forty;for(int i=0;i<40;++i)forty.append({{},ChartDrawingKind::Horizontal,{{Start,QString::number(110+i)}}});
        QVERIFY(!totalBound.applyBatch(forty,frame(totalBound),&err));
        QVERIFY(!err.isEmpty());QCOMPARE(totalBound.publicState(),tbBefore);
    }

    // Serialized 2MB budget overflow via applyBatch also rejects atomically.
    const QString longPrice="100."+QString(ChartDrawings::MaxPriceChars-4,'0');
    const QString longPrice2="101."+QString(ChartDrawings::MaxPriceChars-4,'0');
    auto root=QJsonDocument::fromJson(stableBytes).object();
    auto ctxs=root["contexts"].toArray();auto tmplCtx=ctxs.first().toObject();auto tmplDrw=tmplCtx["drawings"].toArray().first().toObject();
    tmplDrw["kind"]="trend";tmplDrw["autoBatchId"]="";tmplDrw["locked"]=false;
    int synthId=1;
    for(int c=1;c<20;++c){
        auto ctx=tmplCtx;auto ident=ctx["identity"].toObject();ident["code"]="KRW-SYN"+QString::number(c);ctx["identity"]=ident;
        QJsonArray drws;
        for(int j=0;j<ChartDrawings::MaxPerContext;++j){
            auto d=tmplDrw;d["id"]=QString("%1").arg(synthId++,8,16,QLatin1Char('0'))+"-0000-4000-8000-000000000000";
            d["anchors"]=QJsonArray{QJsonObject{{"atMs",QString::number(Start)},{"price",longPrice}},QJsonObject{{"atMs",QString::number(Start+Minute)},{"price",longPrice}}};
            drws.append(d);
        }
        ctx["drawings"]=drws;ctxs.append(ctx);
    }
    int excess=int(encoded(QJsonObject{{"schema","keefi-chart-drawings/2"},{"contexts",ctxs},{"magnet",false}}).size())-(ChartDrawings::MaxJsonBytes-200);
    QVERIFY(excess>0);
    for(int c=1;c<ctxs.size()&&excess>0;++c){
        auto ctx=ctxs[c].toObject();auto drws=ctx["drawings"].toArray();
        for(int j=0;j<drws.size()&&excess>0;++j){
            auto d=drws[j].toObject();auto an=d["anchors"].toArray();
            for(int k=0;k<an.size()&&excess>0;++k){
                auto p=an[k].toObject();auto pr=p["price"].toString();
                const int trim=std::min(excess,int(pr.size())-5);pr.chop(trim);excess-=trim;p["price"]=pr;an[k]=p;
            }
            d["anchors"]=an;drws[j]=d;
        }
        ctx["drawings"]=drws;ctxs[c]=ctx;
    }
    QCOMPARE(excess,0);root["contexts"]=ctxs;
    ChartDrawings tight;QVERIFY(tight.restorePublicState(encoded(root)));QVERIFY(tight.setContext(context(),Minute));
    const auto tightBefore=tight.publicState();
    QVERIFY(!tight.applyBatch({{{},ChartDrawingKind::Zone,{{Start,longPrice},{Start+Minute,longPrice2}}}},frame(tight),&err));
    QVERIFY(err.contains(QStringLiteral("크기 한도")));QCOMPARE(tight.publicState(),tightBefore);
}
void ChartDrawingsTest::perDrawingVisibilityAndLockGovernEditsUndoAndAlerts() {
    ChartDrawings model;QVERIFY(model.setContext(context(),Minute));model.setPublicLists({{"public-list",{Coin}}});
    auto f=frame(model);
    const auto emptyBytes=model.publicState();
    const auto line=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},f);QVERIFY(!line.isEmpty());
    QVERIFY(model.setAlert(line,{true,"public-list",Minute}));
    // Replace history cleanly via non-identical restore roundtrip so canUndo starts false before toggle test.
    const auto saved=model.publicState();
    QVERIFY(model.restorePublicState(emptyBytes));
    QVERIFY(model.restorePublicState(saved));
    QVERIFY(model.setContext(context(),Minute));
    f=frame(model);
    QVERIFY(!model.canUndo());
    QSignalSpy changed(&model,&ChartDrawings::changed),persisted(&model,&ChartDrawings::publicStateChanged);
    QVERIFY(model.setDrawingVisible(line,false));
    QCOMPARE(changed.count(),1);QCOMPARE(persisted.count(),1);
    QVERIFY(!model.canUndo());
    QCOMPARE(model.horizontalLines().size(),1);
    QCOMPARE(model.horizontalLines().first().drawingId,line);

    const auto trend=model.create(ChartDrawingKind::Trend,{{Start,"100"},{Start+2*Minute,"105"}},f);QVERIFY(!trend.isEmpty());
    QVERIFY(model.moveAnchor(trend,1,{Start+2*Minute,"108"},f));
    QVERIFY(model.setDrawingVisible(trend,false));
    QVERIFY(model.setDrawingLocked(trend,true));

    // Locked drawing rejects moveAnchor, moveDrawing, remove with "잠긴 도형이에요".
    QString err;
    QVERIFY(!model.moveAnchor(trend,1,{Start+2*Minute,"110"},f,&err));QCOMPARE(err,QString("잠긴 도형이에요"));
    QVERIFY(!model.moveDrawing(trend,{{Start,"101"},{Start+2*Minute,"109"}},f,&err));QCOMPARE(err,QString("잠긴 도형이에요"));
    QVERIFY(!model.remove(trend,&err));QCOMPARE(err,QString("잠긴 도형이에요"));

    // clear() keeps locked trend and removes unlocked line; calling clear() again returns false.
    QVERIFY(model.clear());QCOMPARE(model.drawings().size(),1);QCOMPARE(model.drawings().first().id,trend);
    QVERIFY(!model.clear());
    // Undo brings back the line while keeping current visible=false, locked=true on trend.
    QVERIFY(model.undo());QCOMPARE(model.drawings().size(),2);
    QCOMPARE(model.drawings()[1].visible,false);QCOMPARE(model.drawings()[1].locked,true);

    // Undo that would change locked trend's anchors (108 -> 105) is rejected and keeps history intact.
    QVERIFY(!model.undo());
    QCOMPARE(model.drawings()[1].anchors[1].price,QString("108"));
    QVERIFY(model.canUndo());

    // Unlock allows undo of moveAnchor, then locking on original anchor still blocks undo of create.
    QVERIFY(model.setDrawingLocked(trend,false));
    QVERIFY(model.undo());QCOMPARE(model.drawings()[1].anchors[1].price,QString("105"));
    QVERIFY(model.setDrawingLocked(trend,true));
    QVERIFY(!model.undo());QCOMPARE(model.drawings().size(),2);
}
void ChartDrawingsTest::transientBatchDoesNotLeakIntoPublicStateAndClearsWithPrivate() {
    ChartDrawings model;QVERIFY(model.setContext(context(true),Minute));
    QVERIFY(!model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},frame(model)).isEmpty());
    const auto publicBytes=model.publicState();
    QVERIFY(model.setContext(context(false),Minute));
    QVERIFY(model.applyBatch({{{},ChartDrawingKind::Zone,{{Start,"100"},{Start+2*Minute,"105"}}}},frame(model)));
    QCOMPARE(model.drawings().size(),1);
    QCOMPARE(model.publicState(),publicBytes);
    model.clearPrivate();
    QVERIFY(model.setContext(context(false),Minute));
    QVERIFY(model.drawings().isEmpty());
    QCOMPARE(model.publicState(),publicBytes);
}
void ChartDrawingsTest::fibLevelsMatchWaveEquationsAndDropNonPositivePrices() {
    const auto findRatio=[](const QList<ChartFibLevel>& levels,double ratio,double* outPrice){
        for(const auto& lvl:levels)if(std::abs(lvl.ratio-ratio)<=1e-9){*outPrice=lvl.price;return true;}
        return false;
    };
    const ChartDrawing ret{{},ChartDrawingKind::FibRetracement,{{Start,"63900"},{Start+Minute,"64400"}}};
    const auto retLevels=ChartDrawings::fibLevels(ret);
    QCOMPARE(retLevels.size(),7);
    double price=0;
    QVERIFY(findRatio(retLevels,0.618,&price));QVERIFY2(std::abs(price-64091.0)<=1e-6,qPrintable(QString::number(price)));
    QVERIFY(findRatio(retLevels,1.0,&price));QVERIFY2(std::abs(price-63900.0)<=1e-6,qPrintable(QString::number(price)));
    QVERIFY(findRatio(retLevels,0.0,&price));QVERIFY2(std::abs(price-64400.0)<=1e-6,qPrintable(QString::number(price)));

    const ChartDrawing ext2{{},ChartDrawingKind::FibExtension,{{Start,"63900"},{Start+Minute,"64400"}}};
    const auto ext2Levels=ChartDrawings::fibLevels(ext2);
    QCOMPARE(ext2Levels.size(),5);
    QVERIFY(findRatio(ext2Levels,2.618,&price));QVERIFY2(std::abs(price-65209.0)<=1e-6,qPrintable(QString::number(price)));

    const ChartDrawing ext3{{},ChartDrawingKind::FibExtension,{{Start,"63900"},{Start+Minute,"64400"},{Start+2*Minute,"64080"}}};
    const auto ext3Levels=ChartDrawings::fibLevels(ext3);
    QCOMPARE(ext3Levels.size(),5);
    QVERIFY(findRatio(ext3Levels,1.618,&price));QVERIFY2(std::abs(price-64889.0)<=1e-6,qPrintable(QString::number(price)));

    const ChartDrawing down{{},ChartDrawingKind::FibExtension,{{Start,"1000"},{Start+Minute,"400"}}};
    const auto downLevels=ChartDrawings::fibLevels(down);
    QCOMPARE(downLevels.size(),3);
    for(const auto& lvl:downLevels)QVERIFY(lvl.price>0.0);

    QVERIFY(ChartDrawings::fibLevels(ChartDrawing{{},ChartDrawingKind::Zone,{{Start,"100"},{Start+Minute,"200"}}}).isEmpty());
}
void ChartDrawingsTest::alertLineRevisionIgnoresVisibilityAndLock() {
    ChartDrawings model;QVERIFY(model.setContext(context(),Minute));model.setPublicLists({{"public-list",{Coin}}});const auto f=frame(model);
    const auto line=model.create(ChartDrawingKind::Horizontal,{{Start,"100"}},f);QVERIFY(!line.isEmpty());
    QVERIFY(model.setAlert(line,{true,"public-list",Minute}));
    // Dev103 hashed the v1 shape object, so an upgrade keeps every saved alert line's revision.
    const QJsonObject v1Shape{{"id",line},{"kind","horizontal"},{"anchors",QJsonArray{QJsonObject{{"atMs",QString::number(Start)},{"price","100"}}}},
        {"alert",QJsonObject{{"enabled",true},{"listId","public-list"},{"intervalMs",Minute}}}};
    const QJsonObject proof{{"identity",Coin.key()},{"currency","KRW"},{"bucket","intraday"},{"drawing",v1Shape}};
    const auto dev103=QString::fromLatin1(QCryptographicHash::hash(encoded(proof),QCryptographicHash::Sha256).toHex());
    const auto revision=[&]{const auto lines=model.horizontalLines();return lines.size()==1?lines.first().revision:QString{};};
    QCOMPARE(revision(),dev103);
    QSignalSpy lines(&model,&ChartDrawings::horizontalLinesChanged);
    QVERIFY(model.setDrawingVisible(line,false));QCOMPARE(revision(),dev103);
    QVERIFY(model.setDrawingLocked(line,true));QCOMPARE(revision(),dev103);
    QVERIFY(model.setDrawingLocked(line,false));QVERIFY(model.setDrawingVisible(line,true));QCOMPARE(revision(),dev103);
    QCOMPARE(lines.count(),0);
    // Moving the line is an alert change: the revision follows the new price.
    QVERIFY(model.moveDrawing(line,{{Start,"101"}},f));QVERIFY(revision()!=dev103);QCOMPARE(lines.count(),1);
}
void ChartDrawingsTest::undoKeepsVisibilityAndLockSetAfterTheRecordedEdit() {
    // The flags change after the step is recorded, so only the restore rule can keep them.
    ChartDrawings model;QVERIFY(model.setContext(context(),Minute));const auto f=frame(model);
    const auto moved=model.create(ChartDrawingKind::Trend,{{Start,"100"},{Start+2*Minute,"105"}},f);
    const auto other=model.create(ChartDrawingKind::Horizontal,{{Start,"99"}},f);QVERIFY(!moved.isEmpty()&&!other.isEmpty());
    QVERIFY(model.moveAnchor(moved,1,{Start+2*Minute,"108"},f));
    QVERIFY(model.setDrawingVisible(moved,false));QVERIFY(model.setDrawingLocked(other,true));
    QVERIFY(model.undo());
    auto shapes=model.drawings();QCOMPARE(shapes.size(),2);
    QCOMPARE(shapes[0].id,moved);QCOMPARE(shapes[0].anchors[1].price,QString("105"));QVERIFY(!shapes[0].visible);QVERIFY(!shapes[0].locked);
    QCOMPARE(shapes[1].id,other);QVERIFY(shapes[1].visible);QVERIFY(shapes[1].locked);
    // The redo step was recorded with the old flags; the current ones still win.
    QVERIFY(model.setDrawingVisible(moved,true));QVERIFY(model.setDrawingLocked(other,false));
    QVERIFY(model.redo());
    shapes=model.drawings();QCOMPARE(shapes[0].anchors[1].price,QString("108"));QVERIFY(shapes[0].visible);QVERIFY(!shapes[1].locked);
}
QTEST_GUILESS_MAIN(ChartDrawingsTest)
#include "tst_chart_drawings.moc"
