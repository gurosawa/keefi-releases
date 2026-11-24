#include "keefi/CandleChart_p.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/StockChartTypes.h"
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <limits>

using namespace keefi;
using keefi::market_candles_detail::CandleChart;

namespace {
constexpr qint64 Minute=60000;
constexpr qint64 Start=1900000020000;
QList<MarketCandle> candles(int count){
    QList<MarketCandle> result;result.reserve(count);
    for(int i=0;i<count;++i){
        const auto price=100+i%11;
        result.append({Start+i*Minute,0,QString::number(price),QString::number(price+3),
                       QString::number(price-2),QString::number(price+1),"10"});
    }
    return result;
}
void configure(CandleChart& chart,const QList<MarketCandle>& rows,
               const std::optional<MarketCandle>& forming={}){
    chart.resize(1000,600);
    chart.setContext(Minute,"KRW","합성 차트");
    chart.setObservation(rows,forming);
    chart.show();
    QCoreApplication::processEvents();
}
QImage render(CandleChart& chart){
    QImage image(chart.size()*chart.devicePixelRatioF(),QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(chart.devicePixelRatioF());image.fill(Qt::transparent);
    chart.render(&image);return image;
}
void hoverAt(CandleChart& chart,qint64 at){
    const auto range=chart.syncViewport();
    const auto x=chart.property("viewportPlotLeft").toDouble()+
        double(at-range.fromMs)/(range.toMs-range.fromMs)*chart.property("viewportPlotWidth").toDouble();
    const QPointF point(x,chart.property("viewportPlotTop").toDouble()+80);
    QMouseEvent move(QEvent::MouseMove,point,chart.mapToGlobal(point.toPoint()),
                     Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&chart,&move);
}
class PaintCounter final:public QObject{
  public:
    int count=0;
    bool eventFilter(QObject*,QEvent* event)override{
        if(event->type()==QEvent::Paint)++count;
        return false;
    }
};
ChartStudyTimeline stockTimeline(const QList<MarketCandle>& rows){
    ChartStudyTimeline timeline;
    for(int i=0;i<rows.size();++i){
        ChartStudyTimelineRow row;row.startsAtMs=rows[i].startsAtMs;row.completesAtMs=row.startsAtMs+Minute;
        row.tradingDate=QDate(2030,3,17);row.session=StockSessionKind::Regular;
        row.eligible=true;row.connectsPrevious=i>0;
        if(i>0&&i%13==0){row.connectsPrevious=false;row.missingSlotsBefore=2;}
        timeline.rows.append(row);
    }
    return timeline;
}
}

class CandleChartPerformanceTest final:public QObject{
    Q_OBJECT
    QTemporaryDir profile_;
  private slots:
    void initTestCase(){
        QVERIFY(profile_.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("KEEFI-Synthetic-Chart-Performance");
        QCoreApplication::setApplicationName("KEEFI Synthetic Chart Performance");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile_.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());
        KeefiTheme::instance()->setMode(ThemeMode::Dark);
    }
    void hoverKeepsTheObservedCandleAcrossHistorySizes_data(){
        QTest::addColumn<int>("count");
        QTest::newRow("200")<<200;QTest::newRow("2000")<<2000;QTest::newRow("5000")<<5000;
    }
    void hoverKeepsTheObservedCandleAcrossHistorySizes(){
        QFETCH(int,count);
        const auto rows=candles(count);CandleChart chart(nullptr);configure(chart,rows);
        qint64 observed=0;chart.crosshairHovered=[&](qint64 at){observed=at;};
        const auto viewport=chart.syncViewport();QVERIFY(viewport.valid());
        for(int i=0;i<30;++i){
            const auto at=rows[count-10-i%20].startsAtMs;
            hoverAt(chart,at);QCOMPARE(observed,at);
            QCOMPARE(chart.syncViewport(),viewport);
        }
    }
    void unchangedPresentationDoesNotScheduleAnotherPaint(){
        const auto rows=candles(5000);CandleChart chart(nullptr);configure(chart,rows);
        PaintCounter paints;chart.installEventFilter(&paints);
        QTest::qWait(30);const auto before=paints.count;
        for(int i=0;i<25;++i){
            chart.setStockTimeline(false,{});chart.setContext(Minute,"KRW","합성 차트");
            chart.setPriceLines({});chart.setObservation(rows,{});
        }
        QTest::qWait(20);
        QCOMPARE(paints.count,before);
        QCOMPARE(chart.shareViewport().toMs,rows.last().startsAtMs);
    }
    void unchangedNearEdgeObservationKeepsHistoryRetryWithoutRepublishing(){
        const auto rows=candles(200);CandleChart chart(nullptr);configure(chart,rows);chart.pan(5000);
        const auto viewport=chart.syncViewport();int requests=0,published=0;
        chart.olderRequested=[&]{++requests;};chart.viewportChanged=[&]{++published;};
        for(int i=0;i<3;++i)chart.setObservation(rows,{});
        QCOMPARE(requests,3);QCOMPARE(published,0);QCOMPARE(chart.syncViewport(),viewport);
    }
    void historyPrependPreservesTheViewportAndMatchesFreshPainting(){
        const auto rows=candles(5000);CandleChart warm(nullptr);configure(warm,rows.mid(200));
        warm.pan(120);const auto before=warm.syncViewport();
        const auto previousImage=render(warm);
        warm.setObservation(rows,{});
        QCOMPARE(warm.syncViewport(),before);
        QCOMPARE(render(warm),previousImage);
        CandleChart fresh(nullptr);configure(fresh,rows);fresh.applySyncedViewport(before,true);
        QCOMPARE(render(warm),render(fresh));
        qint64 warmHover=0,freshHover=0;warm.crosshairHovered=[&](qint64 at){warmHover=at;};
        fresh.crosshairHovered=[&](qint64 at){freshHover=at;};
        const auto at=rows[rows.size()-140].startsAtMs;
        hoverAt(warm,at);hoverAt(fresh,at);
        QCOMPARE(warmHover,at);QCOMPARE(freshHover,at);
        QCOMPARE(render(warm),render(fresh));
    }
    void formingPriceChangeInvalidatesBoundsWithoutChangingCompletedHistory(){
        const auto rows=candles(5000);
        MarketCandle forming{rows.last().startsAtMs+Minute,0,"110","112","109","111","1"};
        CandleChart warm(nullptr);configure(warm,rows,forming);render(warm);
        const auto closed=warm.shareViewport();const auto oldHigh=warm.property("viewportPriceHigh").toDouble();
        forming.high="300";forming.close="299";forming.volume="8";
        warm.setObservation(rows,forming);
        QVERIFY(warm.property("viewportPriceHigh").toDouble()>oldHigh);
        QCOMPARE(warm.shareViewport().toMs,closed.toMs);
        CandleChart fresh(nullptr);configure(fresh,rows,forming);
        QCOMPARE(render(warm),render(fresh));
        warm.setObservation(rows,{});fresh.setObservation(rows,{});
        QCOMPARE(render(warm),render(fresh));
    }
    void stockTimelineResizeAndScaleInvalidateWarmGeometry(){
        const auto rows=candles(200);auto timeline=stockTimeline(rows);
        CandleChart warm(nullptr);configure(warm,rows);render(warm);
        warm.setStockTimeline(true,timeline);warm.setObservation(rows,{});render(warm);
        timeline.rows[timeline.rows.size()-15].missingSlotsBefore=4;
        timeline.rows[timeline.rows.size()-15].connectsPrevious=false;
        warm.setStockTimeline(true,timeline);warm.setObservation(rows,{});
        warm.resize(1200,650);warm.setPriceScale(1.8);
        CandleChart fresh(nullptr);configure(fresh,rows);fresh.setStockTimeline(true,timeline);
        fresh.setObservation(rows,{});fresh.resize(1200,650);fresh.setPriceScale(1.8);
        QCoreApplication::processEvents();
        QCOMPARE(warm.property("viewportFirstMs"),fresh.property("viewportFirstMs"));
        QCOMPARE(warm.property("viewportSpanSlots"),fresh.property("viewportSpanSlots"));
        QCOMPARE(render(warm),render(fresh));
        warm.setContext(5*Minute,"USD","다른 합성 차트");warm.setStockTimeline(false,{});
        warm.setObservation(rows,{});warm.resetViewport();
        fresh.setContext(5*Minute,"USD","다른 합성 차트");fresh.setStockTimeline(false,{});
        fresh.setObservation(rows,{});fresh.resetViewport();
        QCOMPARE(render(warm),render(fresh));
    }
    void observedLookupMatchesLinearReferenceAcrossGapsAndTies(){
        auto rows=candles(60);rows.removeAt(30);rows.removeAt(13);
        auto timeline=stockTimeline(rows);
        const auto formingAt=rows.last().startsAtMs+Minute;
        timeline.projectionCovered=true;
        for(int i=1;i<=20;++i)timeline.nextStartsAtMs.append(rows.last().startsAtMs+i*Minute);
        for(const bool stock:{false,true}){
            const auto axis=stock?ChartTimeAxis::stock(rows,Minute,timeline,formingAt):ChartTimeAxis::utc(rows,Minute);
            QVERIFY(axis.available);
            QList<qint64> observed;for(const auto& row:rows)observed.append(row.startsAtMs);
            if(stock)observed.append(formingAt);
            for(double tolerance:{0.0,.49,.5})for(int query=-4;query<350;++query){
                const auto slot=query/4.0;std::optional<qint64> expected;
                auto distance=std::numeric_limits<double>::infinity();
                for(const auto at:observed)if(const auto coordinate=axis.coordinateAt(at)){
                    const auto gap=std::abs(*coordinate-slot);
                    if(gap<=tolerance&&gap<distance){distance=gap;expected=at;}
                }
                QCOMPARE(axis.observedTimeAt(slot,tolerance).value_or(-1),expected.value_or(-1));
            }
        }
    }
};
QTEST_MAIN(CandleChartPerformanceTest)
#include "tst_candle_chart_performance.moc"
