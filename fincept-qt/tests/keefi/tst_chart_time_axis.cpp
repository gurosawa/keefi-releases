#include "keefi/ChartTimeAxis.h"
#include "keefi/StockChartTypes.h"
#include "keefi/ChartDrawings.h"
#include <QDateTime>
#include <QTest>
#include <limits>

using namespace keefi;
namespace {
qint64 instant(const char* value) { return QDateTime::fromString(QString::fromLatin1(value), Qt::ISODate).toMSecsSinceEpoch(); }
MarketCandle candle(qint64 at) { return {at, 0, "100", "102", "99", "101", "2"}; }
ChartStudyTimelineRow context(qint64 at, bool connected = true, bool eligible = true) {
    ChartStudyTimelineRow row; row.startsAtMs = at; row.completesAtMs = at + 60000;
    row.tradingDate = QDateTime::fromMSecsSinceEpoch(at, Qt::UTC).date();
    row.session = StockSessionKind::Regular; row.connectsPrevious = connected; row.eligible = eligible;
    return row;
}
}
class ChartTimeAxisTest final: public QObject {
    Q_OBJECT
  private slots:
    void utcAxisPreservesElapsedGapsAndActualTimeHits();
    void officialNormalClosureCompressesWithoutChangingUtcTimestamps();
    void intradayMissingSlotsStayEmptyAndCannotReturnAnObservedCandle();
    void unknownContinuityRemainsAVisibleBreak();
    void excludedObservationStaysVisibleWithoutJoiningStudyLayers();
    void futureCoordinatesRequireCompleteCalendarCoverageAndStayUnobserved();
    void malformedRowsAndTimelineAreRejected();
    void displayCoordinatesDoNotInventUnknownCompletionClocks();
    void savedAnnotationProjectsInsideActualLargerTimeframeWithoutChangingUtc();
    void savedAnnotationRejectsUnobservedOrIncompleteStockCoordinates();
};
void ChartTimeAxisTest::savedAnnotationProjectsInsideActualLargerTimeframeWithoutChangingUtc(){
    const auto at=instant("2030-01-07T09:00:00+09:00");const auto anchor=at+180000;
    const QList<MarketCandle> rows{candle(at),candle(at+3600000)};
    const QList<ChartDrawingBar> closed{{rows[0],at+3600000,true,false},{rows[1],at+7200000,true,true}};
    auto first=context(at,false),second=context(at+3600000);first.completesAtMs=at+3600000;second.completesAtMs=at+7200000;
    ChartStudyTimeline timeline;timeline.rows={first,second};
    for(const auto& axis:{ChartTimeAxis::utc(rows,3600000),ChartTimeAxis::stock(rows,3600000,timeline)}){
        QVERIFY(axis.available);QCOMPARE(axis.annotationCoordinateAt(at,closed).value_or(-1),0.0);
        QCOMPARE(axis.annotationCoordinateAt(anchor,closed).value_or(-1),.05);
        QCOMPARE(axis.annotationCoordinateAt(at+3600000+180000,closed).value_or(-1),1.05);
        QCOMPARE(axis.observedTimeAt(0).value_or(0),at);QVERIFY(!axis.observedTimeAt(.05,0));
    }
    QCOMPARE(anchor,instant("2030-01-07T09:03:00+09:00"));QCOMPARE(closed[0].candle.startsAtMs,at);
}
void ChartTimeAxisTest::savedAnnotationRejectsUnobservedOrIncompleteStockCoordinates(){
    const auto friday=instant("2030-01-04T15:00:00+09:00"),monday=instant("2030-01-07T09:00:00+09:00");
    const QList<MarketCandle> rows{candle(friday),candle(monday)};
    auto first=context(friday,false),second=context(monday);first.completesAtMs=friday+3600000;second.completesAtMs=monday+3600000;
    ChartStudyTimeline timeline;timeline.rows={first,second};timeline.projectionCovered=true;
    for(int i=1;i<=20;++i)timeline.nextStartsAtMs.append(monday+i*3600000);
    const auto axis=ChartTimeAxis::stock(rows,3600000,timeline,monday+3600000);
    const QList<ChartDrawingBar> closed{{rows[0],friday+3600000,true,false},{rows[1],monday+3600000,true,true}};
    QVERIFY(axis.available);QCOMPARE(axis.annotationCoordinateAt(monday+180000,closed).value_or(-1),1.05);
    QVERIFY(axis.coordinateAt(monday+3600000));QVERIFY(axis.observedTimeAt(2));
    QVERIFY(!axis.annotationCoordinateAt(monday+3600000,closed)); // Observed forming is not a completed anchor.
    QVERIFY(!axis.annotationCoordinateAt(monday+7200000,closed)); // Calendar projection is not an observation.
    QVERIFY(!axis.annotationCoordinateAt(instant("2030-01-05T12:00:00+09:00"),closed));
    QVERIFY(!axis.annotationCoordinateAt(friday-1,closed));
    auto incomplete=closed;incomplete[1].sourceCompleted=false;QVERIFY(!axis.annotationCoordinateAt(monday+180000,incomplete));
    incomplete=closed;incomplete[1].completesAtMs=0;QVERIFY(!axis.annotationCoordinateAt(monday,incomplete));
    incomplete=closed;incomplete[1].candle.startsAtMs+=60000;QVERIFY(!axis.annotationCoordinateAt(monday+180000,incomplete));
}
void ChartTimeAxisTest::displayCoordinatesDoNotInventUnknownCompletionClocks(){
    const auto at=instant("2030-01-04T00:00:00-05:00");
    auto row=context(at,false,false);row.completesAtMs=0;
    ChartStudyTimeline timeline;timeline.rows={row};
    const auto axis=ChartTimeAxis::stock({candle(at)},86400000,timeline);
    QVERIFY2(axis.available,"Display-only coordinates do not require an invented finalization timestamp");
    QCOMPARE(axis.coordinateAt(at).value_or(-1),0.0);QCOMPARE(timeline.rows.first().completesAtMs,qint64(0));
    QVERIFY(!axis.connects(at,at+86400000));
}
void ChartTimeAxisTest::utcAxisPreservesElapsedGapsAndActualTimeHits() {
    const auto at = instant("2030-01-04T00:00:00Z");
    const QList<MarketCandle> rows{candle(at), candle(at + 60000), candle(at + 180000)};
    const auto axis = ChartTimeAxis::utc(rows, 60000);
    QVERIFY(axis.available);
    QCOMPARE(axis.coordinateAt(at).value_or(-1), 0.0);
    QCOMPARE(axis.coordinateAt(at + 180000).value_or(-1), 3.0);
    QCOMPARE(axis.observedTimeAt(3.0).value_or(0), at + 180000);
    QVERIFY(!axis.observedTimeAt(2.0));
    QVERIFY(axis.connects(at, at + 60000)); QVERIFY(!axis.connects(at + 60000, at + 180000));
}
void ChartTimeAxisTest::officialNormalClosureCompressesWithoutChangingUtcTimestamps() {
    const auto friday = instant("2030-01-04T15:29:00+09:00");
    const auto monday = instant("2030-01-07T09:00:00+09:00");
    const QList<MarketCandle> rows{candle(friday), candle(monday), candle(monday + 60000)};
    ChartStudyTimeline timeline; timeline.rows = {context(friday, false), context(monday), context(monday + 60000)};
    const auto axis = ChartTimeAxis::stock(rows, 60000, timeline);
    QVERIFY(axis.available);
    QCOMPARE(axis.coordinateAt(friday).value_or(-1), 0.0);
    QCOMPARE(axis.coordinateAt(monday).value_or(-1), 1.0);
    QCOMPARE(axis.observedTimeAt(1.0).value_or(0), monday);
    QCOMPARE(rows.first().startsAtMs, friday); QCOMPARE(rows.last().startsAtMs, monday + 60000);
    QVERIFY(axis.connects(friday, monday));
    QVERIFY(!axis.coordinateAt(instant("2030-01-05T12:00:00+09:00")));
}
void ChartTimeAxisTest::intradayMissingSlotsStayEmptyAndCannotReturnAnObservedCandle() {
    const auto at = instant("2030-01-07T09:00:00+09:00");
    const QList<MarketCandle> rows{candle(at), candle(at + 180000)};
    auto afterGap = context(at + 180000, false); afterGap.missingSlotsBefore = 2;
    ChartStudyTimeline timeline; timeline.rows = {context(at, false), afterGap};
    const auto axis = ChartTimeAxis::stock(rows, 60000, timeline);
    QVERIFY(axis.available); QCOMPARE(axis.coordinateAt(at + 180000).value_or(-1), 3.0);
    QVERIFY(!axis.observedTimeAt(1.0)); QVERIFY(!axis.observedTimeAt(2.0));
    QVERIFY(!axis.coordinateAt(at + 60000)); QVERIFY(!axis.connects(at, at + 180000));
}
void ChartTimeAxisTest::unknownContinuityRemainsAVisibleBreak() {
    const auto at = instant("2030-01-04T15:29:00+09:00");
    const auto later = instant("2030-01-07T09:00:00+09:00");
    ChartStudyTimeline timeline; timeline.rows = {context(at, false), context(later, false)};
    const auto axis = ChartTimeAxis::stock({candle(at), candle(later)}, 60000, timeline);
    QVERIFY(axis.available); QCOMPARE(axis.coordinateAt(later).value_or(-1), 2.0);
    QVERIFY(!axis.observedTimeAt(1.0)); QVERIFY(!axis.connects(at, later));
}
void ChartTimeAxisTest::excludedObservationStaysVisibleWithoutJoiningStudyLayers() {
    const auto at = instant("2030-01-07T09:00:00+09:00");
    ChartStudyTimeline timeline; timeline.rows = {context(at, false), context(at + 60000, true, false), context(at + 120000)};
    const auto axis = ChartTimeAxis::stock({candle(at), candle(at + 60000), candle(at + 120000)}, 60000, timeline);
    QVERIFY(axis.available); QCOMPARE(axis.coordinateAt(at + 60000).value_or(-1), 1.0);
    QCOMPARE(axis.observedTimeAt(1.0).value_or(0), at + 60000);
    QVERIFY(!axis.connects(at, at + 60000)); QVERIFY(!axis.connects(at + 60000, at + 120000));
    timeline.rows[1].exclusions={StockBarExclusion::RestrictionCoverageUnknown};
    const QList<MarketCandle> prices{candle(at),candle(at+60000),candle(at+120000)};
    const auto strict=ChartTimeAxis::stock(prices,60000,timeline);
    QVERIFY(!strict.connects(at,at+60000));
    const auto closeAxis=ChartTimeAxis::stock(prices,60000,timeline.forCloseDisplay());
    QVERIFY(closeAxis.connects(at,at+60000) && closeAxis.connects(at+60000,at+120000));
    timeline.rows[1].exclusions.append(StockBarExclusion::VolatilityInterruption);
    const auto halted=ChartTimeAxis::stock(prices,60000,timeline.forCloseDisplay());
    QVERIFY(!halted.connects(at,at+60000) && !halted.connects(at+60000,at+120000));
}
void ChartTimeAxisTest::futureCoordinatesRequireCompleteCalendarCoverageAndStayUnobserved() {
    const auto at = instant("2030-01-04T15:29:00+09:00");
    const auto next = instant("2030-01-07T09:00:00+09:00");
    ChartStudyTimeline timeline; timeline.rows = {context(at, false)};
    for (int i = 0; i < 20; ++i) timeline.nextStartsAtMs.append(next + i * 60000);
    auto axis = ChartTimeAxis::stock({candle(at)}, 60000, timeline);
    QVERIFY(axis.available); QVERIFY(!axis.coordinateAt(next));
    timeline.projectionCovered = true; axis = ChartTimeAxis::stock({candle(at)}, 60000, timeline);
    QVERIFY(axis.available); QCOMPARE(axis.coordinateAt(next).value_or(-1), 1.0);
    QCOMPARE(axis.coordinateAt(next + 19 * 60000).value_or(-1), 20.0);
    QVERIFY(!axis.observedTimeAt(1.0)); QVERIFY(!axis.connects(at, next));
    const auto withForming = ChartTimeAxis::stock({candle(at)}, 60000, timeline, next);
    QVERIFY(withForming.available); QCOMPARE(withForming.observedTimeAt(1.0).value_or(0), next);
    QVERIFY(!withForming.connects(at, next)); // Displayed target forming never joins completed study layers.
    const auto formingOnly = ChartTimeAxis::stock({}, 60000, {}, next);
    QVERIFY(formingOnly.available); QCOMPARE(formingOnly.coordinateAt(next).value_or(-1), 0.0);
}
void ChartTimeAxisTest::malformedRowsAndTimelineAreRejected() {
    const auto at = instant("2030-01-07T09:00:00+09:00");
    QVERIFY(!ChartTimeAxis::utc({}, 60000).available);
    QVERIFY(!ChartTimeAxis::utc({candle(at)}, 0).available);
    QVERIFY(!ChartTimeAxis::utc({candle(at), candle(at)}, 60000).available);
    ChartStudyTimeline timeline; timeline.rows = {context(at, false)};
    QVERIFY(!ChartTimeAxis::stock({candle(at + 60000)}, 60000, timeline).available);
    timeline.rows.first().missingSlotsBefore = -1;
    QVERIFY(!ChartTimeAxis::stock({candle(at)}, 60000, timeline).available);
    const auto axis = ChartTimeAxis::utc({candle(at)}, 60000);
    QVERIFY(!axis.observedTimeAt(std::numeric_limits<double>::quiet_NaN()));
}
QTEST_GUILESS_MAIN(ChartTimeAxisTest)
#include "tst_chart_time_axis.moc"
