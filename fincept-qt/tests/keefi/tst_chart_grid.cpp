#include "keefi/ChartGridView.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/KeefiFonts.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QComboBox>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace keefi;
namespace {
constexpr qint64 Minute=60000;
constexpr qint64 Now=1728000000000+250*Minute+10000;
const QString Upbit=QStringLiteral("coin:upbit:KRW-SYN");
class Http final:public TradingTransport {
public:
    quint64 token=0;QList<RequestSpec> sent;int aborts=0;
    void send(quint64 value,const RequestSpec& request)override{token=value;sent.append(request);}
    void abort()override{++aborts;}
    void answer(const HttpResult& response){emit finished(token,response);}
};
HttpResult candles(qint64 now,bool binance=false){
    QJsonArray rows;const auto end=now/Minute*Minute;
    for(int i=30;i>=0;--i){const auto open=end-i*Minute;
        if(binance)rows.append(QJsonArray{double(open),"100","110","90","101","2",double(open+Minute-1),"202",1,"1","101","0"});
        else rows.append(QJsonObject{{"market","KRW-SYN"},{"unit","1"},{"timestamp",QString::number(open+1000)},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(open,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","101"},{"candle_acc_trade_volume","2"}});
    }
    HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();return result;
}
QWidget* firstCanvas(ChartGridView& grid){
    auto* first=grid.findChild<QPushButton*>("chartGridSelect_0");if(!first)return nullptr;
    for(auto* widget:first->parentWidget()->findChildren<QWidget*>())
        if(widget->accessibleName()==QStringLiteral("분할 차트 완료 봉"))return widget;
    return nullptr;
}
QString firstStatus(ChartGridView& grid){
    auto* first=grid.findChild<QPushButton*>("chartGridSelect_0");QStringList result;
    if(first)for(auto* label:first->parentWidget()->findChildren<QLabel*>())if(label->isVisible())result.append(label->text());
    return result.join('\n');
}
// Observe the text sent to the public paint surface, including the price heading.
class TextEngine final:public QPaintEngine {
public:
    TextEngine():QPaintEngine(AllFeatures){}
    QStringList text;
    bool begin(QPaintDevice*)override{return true;}bool end()override{return true;}
    Type type()const override{return User;}void updateState(const QPaintEngineState&)override{}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&)override{}
    void drawPolygon(const QPointF*,int,PolygonDrawMode)override{}
    void drawPath(const QPainterPath&)override{}
    void drawTextItem(const QPointF&,const QTextItem& item)override{text.append(item.text());}
};
class TextDevice final:public QPaintDevice {
public:
    explicit TextDevice(QSize size):size_(size){}mutable TextEngine engine;
    QPaintEngine* paintEngine()const override{return &engine;}
    int metric(PaintDeviceMetric metric)const override{
        switch(metric){case PdmWidth:return size_.width();case PdmHeight:return size_.height();case PdmDepth:return 32;
        case PdmNumColors:return 0;case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;}
    }
private:QSize size_;
};
QString paintedText(QWidget* widget){TextDevice device(widget->size());widget->render(&device);return device.engine.text.join(QString{});}
HttpResult syncCandles(qint64 now,const QString& market,qint64 missing=0){
    QJsonArray rows;const qint64 end=now/Minute*Minute;
    for(int i=180;i>=1;--i){const auto at=end-i*Minute;if(at==missing)continue;
        rows.append(QJsonObject{{"market",market},{"unit","1"},{"timestamp",QString::number(at+1000)},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","103"},{"candle_acc_trade_volume","2"}});
    }
    HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();return result;
}
class MarkerPaintProbe final:public QObject {
public:
    QObject* canvas=nullptr;QObject* marker=nullptr;int parentPaints=0,overlayPaints=0;qint64 parentDirtyPixels=0,overlayDirtyPixels=0;
    bool eventFilter(QObject* watched,QEvent* event)override{
        if(event->type()==QEvent::Paint){qint64 area=0;for(const auto& rect:static_cast<QPaintEvent*>(event)->region())area+=qint64(rect.width())*rect.height();
            if(watched==canvas){++parentPaints;parentDirtyPixels+=area;}else if(watched==marker){++overlayPaints;overlayDirtyPixels+=area;}}
        return false;
    }
};
}

class ChartGridTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
private slots:
    void initTestCase(){
        QVERIFY(settingsRoot_.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsRoot_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settingsRoot_.filePath("system"));
        KeefiFonts::registerBundledFonts();
    }
    void passivePanelShowsObservedVolumeAndGapAwareSma(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);
        ChartGridView grid;grid.setSource(&feed);grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.resize(900,500);grid.show();
        feed.pump();http.answer(candles(now));auto* canvas=firstCanvas(grid);QVERIFY(canvas);
        QVERIFY(paintedText(canvas).contains("SMA20 101"));
        QVERIFY(paintedText(canvas).contains("거래량 2 SYN"));
        now+=Minute;feed.pump();auto gap=candles(now);auto rows=QJsonDocument::fromJson(gap.body).array();rows.removeAt(20);gap.body=QJsonDocument(rows).toJson();http.answer(gap);
        QVERIFY(paintedText(canvas).contains("SMA20 · 연속 20봉 대기"));
        QVERIFY(!paintedText(canvas).contains("SMA20 101"));
    }
    void syncSelectionSwitchesAreIndependentAndBidirectional(){
        ChartGridView grid;grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.seed({"coin:binance:SYNUSDT"});
        for(auto option:{ChartSyncOption::Symbol,ChartSyncOption::Interval,ChartSyncOption::Crosshair,ChartSyncOption::Time,ChartSyncOption::DateRange})
            QVERIFY(!grid.syncSettings().enabled(option));
        const auto secondInterval=grid.panelInterval(1);
        grid.setSelection("coin:upbit:KRW-AAA",5*Minute);
        QCOMPARE(grid.panelIdentity(1),QString("coin:binance:SYNUSDT"));QCOMPARE(grid.panelInterval(1),secondInterval);
        grid.setSyncOption(ChartSyncOption::Symbol,true);grid.setSelection("coin:binance:BBBUSDT",Minute);
        QCOMPARE(grid.panelIdentity(1),QString("coin:binance:BBBUSDT"));QCOMPARE(grid.panelInterval(1),secondInterval);
        grid.setSyncOption(ChartSyncOption::Symbol,false);grid.setSyncOption(ChartSyncOption::Interval,true);
        grid.setSelection("coin:upbit:KRW-CCC",5*Minute);
        QCOMPARE(grid.panelIdentity(1),QString("coin:binance:BBBUSDT"));QCOMPARE(grid.panelInterval(1),5*Minute);
        grid.findChild<QPushButton*>("chartGridSelect_1")->click();
        grid.setSelection("coin:upbit:KRW-DDD",Minute);
        QCOMPARE(grid.panelIdentity(0),QString("coin:upbit:KRW-CCC"));QCOMPARE(grid.panelInterval(0),Minute);
        grid.setSyncOption(ChartSyncOption::Symbol,true);grid.setSelection("coin:binance:EEEUSDT",15*Minute);
        QCOMPARE(grid.panelIdentity(0),QString("coin:binance:EEEUSDT"));QCOMPARE(grid.panelInterval(0),15*Minute);
        grid.setSyncOption(ChartSyncOption::Interval,false);
        QVERIFY(grid.syncSettings().symbol);QVERIFY(!grid.syncSettings().interval);
    }
    void viewportSyncUsesOnlyUserInputAndDateRangeWinsOnce(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});feed.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);
        ChartGridView grid;QWidget active(&grid);grid.setSource(&feed);grid.setPanelCount(2);
        grid.setSyncOption(ChartSyncOption::Interval,true);grid.setSelection(Upbit,Minute);grid.setSyncOption(ChartSyncOption::Interval,false);
        grid.seed({"coin:upbit:KRW-ZZZ"});grid.setActiveView(&active);grid.resize(1100,540);grid.show();
        feed.pump();QCOMPARE(http.sent.size(),1);http.answer(syncCandles(now,"KRW-ZZZ"));
        auto* passive=grid.findChild<QWidget*>("chartGridCanvas_1");QVERIFY(passive);QVERIFY(grid.panelViewport(1).valid());
        const auto initial=grid.panelViewport(1);const auto end=now/Minute*Minute;
        const ChartSyncViewport source{end-100*Minute,end-40*Minute};grid.observeActiveViewport(source);
        grid.activeViewportNavigated(source,false);QCOMPARE(grid.panelViewport(1),initial);
        grid.setSyncOption(ChartSyncOption::Time,true);grid.activeViewportNavigated(source,false);
        const auto timed=grid.panelViewport(1);QCOMPARE(timed.centreMs(),source.centreMs());QCOMPARE(timed.toMs-timed.fromMs,initial.toMs-initial.fromMs);
        const auto applied=grid.property("syncViewportApplyCount").toULongLong();
        grid.activeViewportNavigated({source.fromMs+Minute,source.toMs-Minute},true);QCOMPARE(grid.property("syncViewportApplyCount").toULongLong(),applied);
        grid.setSyncOption(ChartSyncOption::DateRange,true);grid.activeViewportNavigated(source,true);QCOMPARE(grid.panelViewport(1),source);
        QCOMPARE(grid.property("syncViewportApplyCount").toULongLong(),applied+1);
        const ChartSyncViewport moved{source.fromMs-Minute,source.toMs-Minute};grid.activeViewportNavigated(moved,false);
        QCOMPARE(grid.panelViewport(1),moved);QCOMPARE(grid.property("syncViewportApplyCount").toULongLong(),applied+2);
        grid.observeActiveViewport({moved.fromMs+100,moved.toMs+100});QCOMPARE(grid.property("syncViewportApplyCount").toULongLong(),applied+2);
        QSignalSpy requests(&grid,&ChartGridView::activeViewportSyncRequested);passive->setFocus();QTest::keyClick(passive,Qt::Key_Left);
        QCOMPARE(requests.size(),1);QCOMPARE(qvariant_cast<ChartSyncViewport>(requests.first().at(0)),grid.panelViewport(1));QVERIFY(requests.first().at(1).toBool());
        // A synchronized target observation is silent, preventing a ping-pong loop.
        grid.observeActiveViewport(qvariant_cast<ChartSyncViewport>(requests.first().at(0)));QCOMPARE(requests.size(),1);
        const int sent=http.sent.size();QCoreApplication::processEvents();QCOMPARE(http.sent.size(),sent);
    }
    void passiveWheelPreservesHorizontalPanAfterEmptyBegin_data(){
        QTest::addColumn<bool>("phased");
        QTest::newRow("unphased-control") << false;
        QTest::newRow("zero-delta-begin-horizontal-update") << true;
    }
    void passiveWheelPreservesHorizontalPanAfterEmptyBegin(){
        QFETCH(bool, phased);
        Http http;
        const qint64 now = Now;
        ChartStudyFeed feed(&http, nullptr, nullptr, [&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN", "KRW-ZZZ"}, true);
        ChartGridView grid;
        QWidget active(&grid);
        grid.setSource(&feed);
        grid.setPanelCount(2);
        grid.setSyncOption(ChartSyncOption::Interval, true);
        grid.setSelection(Upbit, Minute);
        grid.setSyncOption(ChartSyncOption::Interval, false);
        grid.seed({"coin:upbit:KRW-ZZZ"});
        grid.setActiveView(&active);
        grid.resize(1100, 540);
        grid.show();
        feed.pump();
        QCOMPARE(http.sent.size(), 1);
        http.answer(syncCandles(now, "KRW-ZZZ"));

        auto* passive = grid.findChild<QWidget*>("chartGridCanvas_1");
        QVERIFY(passive);
        const auto before = grid.panelViewport(1);
        QVERIFY(before.valid());
        const double width = passive->property("viewportPlotWidth").toDouble();
        QVERIFY(width > 0);
        const QPointF point(passive->property("viewportPlotLeft").toDouble() + width / 2,
                            passive->property("viewportPlotTop").toDouble() + 20);
        auto wheel = [&](QPoint pixels, Qt::ScrollPhase phase){
            QWheelEvent event(point, passive->mapToGlobal(point.toPoint()), pixels, {},
                              Qt::NoButton, Qt::NoModifier, phase, false);
            event.setAccepted(false);
            QApplication::sendEvent(passive, &event);
            return event.isAccepted();
        };

        if (phased)
            QVERIFY(wheel({}, Qt::ScrollBegin));
        QCOMPARE(grid.panelViewport(1), before);
        QVERIFY(wheel({40, 0}, phased ? Qt::ScrollUpdate : Qt::NoScrollPhase));
        if (phased)
            QVERIFY(wheel({}, Qt::ScrollEnd));
        const auto after = grid.panelViewport(1);
        QCOMPARE(http.sent.size(), 1);
        QVERIFY2(after.toMs < before.toMs, "positive horizontal wheel should pan toward older candles");
        QCOMPARE(after.toMs - after.fromMs, before.toMs - before.fromMs);
    }
    void crosshairRejectsGapsOutsideAndLeaveWithoutNetwork(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});feed.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);
        ChartGridView grid;QWidget active(&grid);grid.setSource(&feed);grid.setPanelCount(2);
        grid.setSyncOption(ChartSyncOption::Interval,true);grid.setSelection(Upbit,Minute);grid.setSyncOption(ChartSyncOption::Interval,false);
        grid.seed({"coin:upbit:KRW-ZZZ"});grid.setActiveView(&active);grid.resize(1100,540);grid.show();
        const qint64 end=now/Minute*Minute,gap=end-35*Minute;feed.pump();http.answer(syncCandles(now,"KRW-ZZZ",gap));
        auto* passive=grid.findChild<QWidget*>("chartGridCanvas_1");QVERIFY(passive);
        grid.activeCrosshairHovered(end-20*Minute);QCOMPARE(passive->property("syncCrosshairMs").toLongLong(),0);
        grid.setSyncOption(ChartSyncOption::Crosshair,true);const int sent=http.sent.size();
        grid.activeCrosshairHovered(end-20*Minute);QCOMPARE(passive->property("syncCrosshairMs").toLongLong(),end-20*Minute);
        QCOMPARE(passive->property("syncCrosshairPrice").toDouble(),103.0);
        grid.activeCrosshairHovered(gap+Minute/2);QCOMPARE(passive->property("syncCrosshairMs").toLongLong(),0);
        grid.activeCrosshairHovered(end-160*Minute);QCOMPARE(passive->property("syncCrosshairMs").toLongLong(),0);
        grid.activeCrosshairHovered(end-20*Minute);grid.activeCrosshairHovered(0);QCOMPARE(passive->property("syncCrosshairMs").toLongLong(),0);
        QSignalSpy received(&grid,&ChartGridView::activeCrosshairSyncRequested);
        const QPoint point(qRound(passive->property("viewportPlotLeft").toDouble()+passive->property("viewportPlotWidth").toDouble()*.7),
                           qRound(passive->property("viewportPlotTop").toDouble()+20));
        QMouseEvent move(QEvent::MouseMove,QPointF(point),passive->mapToGlobal(point),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(passive,&move);QVERIFY(!received.isEmpty());QVERIFY(received.last().first().toLongLong()>0);
        QEvent leave(QEvent::Leave);QApplication::sendEvent(passive,&leave);QCOMPARE(received.last().first().toLongLong(),0);
        grid.activeCrosshairHovered(end-20*Minute);grid.setSyncOption(ChartSyncOption::Crosshair,false);QCOMPARE(passive->property("syncCrosshairMs").toLongLong(),0);
        QCOMPARE(http.sent.size(),sent);
    }
    void passiveCrosshairUsesTheCandleUnderThePointer_data(){
        QTest::addColumn<qint64>("offsetFromGap");
        QTest::addColumn<qint64>("expectedCandleOffset");
        QTest::newRow("left-half-of-gap") << -Minute/4 << qint64(0);
        QTest::newRow("left-half-of-next-candle") << 3*Minute/4 << Minute;
    }
    void passiveCrosshairUsesTheCandleUnderThePointer(){
        QFETCH(qint64,offsetFromGap);
        QFETCH(qint64,expectedCandleOffset);
        Http http;
        qint64 now=Now;
        ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);
        ChartGridView grid;
        QWidget active(&grid);
        grid.setSource(&feed);
        grid.setPanelCount(2);
        grid.setSyncOption(ChartSyncOption::Interval,true);
        grid.setSelection(Upbit,Minute);
        grid.setSyncOption(ChartSyncOption::Interval,false);
        grid.seed({"coin:upbit:KRW-ZZZ"});
        grid.setActiveView(&active);
        grid.resize(1100,540);
        grid.show();
        const qint64 gap=now/Minute*Minute-35*Minute;
        feed.pump();
        QCOMPARE(http.sent.size(),1);
        http.answer(syncCandles(now,"KRW-ZZZ",gap));
        auto* passive=grid.findChild<QWidget*>("chartGridCanvas_1");
        QVERIFY(passive);
        const auto viewport=grid.panelViewport(1);
        QVERIFY(viewport.valid());
        grid.setSyncOption(ChartSyncOption::Crosshair,true);
        QSignalSpy marker(&grid,&ChartGridView::activeCrosshairSyncRequested);
        const double fraction=double(gap+offsetFromGap-viewport.fromMs)/(viewport.toMs-viewport.fromMs);
        const QPointF point(passive->property("viewportPlotLeft").toDouble()+
                            passive->property("viewportPlotWidth").toDouble()*fraction,
                            passive->property("viewportPlotTop").toDouble()+20);
        QMouseEvent move(QEvent::MouseMove,point,passive->mapToGlobal(point.toPoint()),
                         Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(passive,&move);
        QCOMPARE(marker.size(),1);
        QCOMPARE(marker.first().first().toLongLong(),expectedCandleOffset?gap+expectedCandleOffset:0);
    }
    void crosshairUsesContainingIntervalInsteadOfNearestGap(){
        QList<MarketCandle> rows{{10*Minute,0,"100","101","99","100","1"},{12*Minute,0,"200","201","199","200","1"}};
        QCOMPARE(chartSyncCandleAt(rows,Minute,10*Minute+Minute/2),&rows[0]);
        QVERIFY(!chartSyncCandleAt(rows,Minute,11*Minute+Minute/2));
        QVERIFY(!chartSyncCandleAt(rows,Minute,9*Minute));
        QCOMPARE(chartSyncCandleAt(rows,Minute,12*Minute+Minute/2),&rows[1]);
        const ChartSyncViewport source{100*Minute,140*Minute},target{20*Minute,30*Minute};
        const auto timed=chartSyncTargetViewport(source,target,false);
        const ChartSyncViewport expected{115*Minute,125*Minute};QCOMPARE(timed,expected);QCOMPARE(chartSyncTargetViewport(source,target,true),source);
    }
    void emptyTargetCanReceiveRangeWithoutPanOrZoomCrash(){
        ChartGridView grid;QWidget active(&grid);grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.seed({"coin:upbit:KRW-ZZZ"});
        grid.setActiveView(&active);grid.resize(1000,520);grid.show();grid.setSyncOption(ChartSyncOption::DateRange,true);
        const ChartSyncViewport source{Now-100*Minute,Now-40*Minute};grid.observeActiveViewport(source);grid.activeViewportNavigated(source,false);
        QCOMPARE(grid.panelViewport(1),source);auto* passive=grid.findChild<QWidget*>("chartGridCanvas_1");QVERIFY(passive);
        QSignalSpy requests(&grid,&ChartGridView::activeViewportSyncRequested);passive->setFocus();
        QTest::keyClick(passive,Qt::Key_Left);QTest::keyClick(passive,Qt::Key_Right);QTest::keyClick(passive,Qt::Key_Plus);QTest::keyClick(passive,Qt::Key_Minus);
        QCOMPARE(grid.panelViewport(1),source);QCOMPARE(requests.size(),0);
    }
    void remoteMarkerLimitsDirtyRegionsWithoutCalculatingOrRequesting(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});feed.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);
        ChartGridView grid;QWidget active(&grid);grid.setSource(&feed);grid.setPanelCount(2);
        grid.setSyncOption(ChartSyncOption::Interval,true);grid.setSelection(Upbit,Minute);grid.setSyncOption(ChartSyncOption::Interval,false);
        grid.seed({"coin:upbit:KRW-ZZZ"});grid.setActiveView(&active);grid.resize(1100,540);grid.show();
        feed.pump();http.answer(syncCandles(now,"KRW-ZZZ"));QTest::qWait(50);
        auto* passive=grid.findChild<QWidget*>("chartGridCanvas_1");QVERIFY(passive);QWidget* marker=nullptr;
        for(auto* child:passive->findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly))if(child->testAttribute(Qt::WA_TransparentForMouseEvents))marker=child;
        QVERIFY(marker);QVERIFY(passive->property("passiveStudyCalculationCount").isValid());
        MarkerPaintProbe paints;paints.canvas=passive;paints.marker=marker;passive->installEventFilter(&paints);marker->installEventFilter(&paints);
        grid.setSyncOption(ChartSyncOption::Crosshair,true);const auto calculations=passive->property("passiveStudyCalculationCount").toULongLong();const int sent=http.sent.size();
        const qint64 end=now/Minute*Minute;constexpr int hoverCount=20;
        for(int i=0;i<hoverCount;++i){const int previous=paints.overlayPaints;grid.activeCrosshairHovered(end-(20+i)*Minute);QTRY_VERIFY_WITH_TIMEOUT(paints.overlayPaints>previous,1000);}
        const auto plotArea=qRound64(passive->property("viewportPlotWidth").toDouble()*passive->property("viewportPlotHeight").toDouble());
        qInfo().noquote()<<QJsonDocument(QJsonObject{{"scope","Qt offscreen redraw regression; not native latency"},{"hover_inputs",hoverCount},
            {"parent_paints",paints.parentPaints},{"overlay_paints",paints.overlayPaints},{"parent_dirty_pixels",paints.parentDirtyPixels},
            {"overlay_dirty_pixels",paints.overlayDirtyPixels},{"plot_area_pixels",plotArea},{"overlay_width",marker->width()},
            {"study_calculations_during_hover",qint64(passive->property("passiveStudyCalculationCount").toULongLong()-calculations)},
            {"rest_requests_during_hover",http.sent.size()-sent}}).toJson(QJsonDocument::Compact);
        QCOMPARE(passive->property("passiveStudyCalculationCount").toULongLong(),calculations);QCOMPARE(http.sent.size(),sent);
        QVERIFY(paints.overlayPaints>=hoverCount);QVERIFY(marker->width()<=3);
        QVERIFY2(paints.parentDirtyPixels<hoverCount*plotArea/5,"Transparent remote marker invalidated too much of its candle parent");
    }
    void hiddenGridReleasesOnlyItsOwnSharedDemand(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);
        feed.setDemands("other-visible-owner",{{{"coin","upbit","KRW-SYN"},Minute,10}});
        ChartGridView grid;grid.setSource(&feed);grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.resize(900,420);grid.show();
        QTRY_COMPARE(feed.ownerCount(Upbit,Minute),2);feed.pump();QCOMPARE(http.sent.size(),1);
        http.answer(candles(now));QVERIFY(feed.observation(Upbit,Minute).series.available);
        grid.hide();QCOMPARE(feed.ownerCount(Upbit,Minute),1);QCOMPARE(http.aborts,0);
        now+=2*Minute;feed.pump();QCOMPARE(http.sent.size(),2);
        grid.show();QTRY_COMPARE(feed.ownerCount(Upbit,Minute),2);
        grid.hide();feed.clearOwner("other-visible-owner");QCOMPARE(feed.demandCount(),0);QCOMPARE(http.aborts,1);
        const int sent=http.sent.size();now+=2*Minute;feed.pump();QCOMPARE(http.sent.size(),sent);
    }
    void clockExpiryUpdatesVisibleStatusDuringSharedCooldown(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);
        ChartGridView grid;grid.setSource(&feed);grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.resize(900,420);grid.show();
        feed.pump();QCOMPARE(http.sent.size(),1);http.answer(candles(now));
        QTRY_VERIFY(firstStatus(grid).contains("공개 현물"));
        grid.seed({"coin:upbit:KRW-ZZZ"});now+=1000;feed.pump();QCOMPARE(http.sent.size(),2);
        HttpResult cooldown;cooldown.status=429;cooldown.headers.insert("Retry-After","600");http.answer(cooldown);
        QVERIFY(!feed.observation(Upbit,Minute).delayed);QVERIFY(!firstStatus(grid).contains("지연"));
        QSignalSpy changes(&feed,&ChartStudyFeed::changed);now+=2*Minute;feed.pump();
        QCOMPARE(changes.size(),0);QVERIFY(feed.observation(Upbit,Minute).delayed);
        QTRY_VERIFY_WITH_TIMEOUT(firstStatus(grid).contains("지연"),2000);
        QCOMPARE(http.sent.size(),2); // Refreshing presentation cannot bypass the shared cooldown.
    }
    void destroyedSourceClearsPaintedPrices(){
        Http http;qint64 now=Now;
        auto feed=std::make_unique<ChartStudyFeed>(&http,nullptr,nullptr,[&]{return now;});
        feed->setVerifiedUpbitMarkets({"KRW-SYN"},true);
        ChartGridView grid;grid.setSource(feed.get());grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.resize(900,420);grid.show();
        feed->pump();QCOMPARE(http.sent.size(),1);http.answer(candles(now));
        auto* canvas=firstCanvas(grid);QVERIFY(canvas);QTRY_VERIFY(canvas->isVisible());
        QVERIFY(paintedText(canvas).contains("종가 101 KRW"));
        feed.reset();QTRY_VERIFY_WITH_TIMEOUT(!paintedText(canvas).contains("종가 "),2000);
        QVERIFY(!firstStatus(grid).contains("공개 현물"));
    }
    void expiredMetadataKeepsCachedBarsExplicitlyDelayed(){
        Http metadata,http;ChartInstrumentCatalog catalog(&metadata);catalog.ensure("binance");
        HttpResult instruments;instruments.status=200;instruments.body=R"({"symbols":[{"symbol":"SYNUSDT","baseAsset":"SYN","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})";
        metadata.answer(instruments);QVERIFY(!catalog.instruments("binance").isEmpty());
        qint64 now=QDateTime::currentMSecsSinceEpoch();ChartStudyFeed feed(&http,&catalog,nullptr,[&]{return now;});
        ChartGridView grid;grid.setSource(&feed);grid.setPanelCount(2);grid.setSelection("coin:binance:SYNUSDT",Minute);grid.resize(900,420);grid.show();
        feed.pump();QCOMPARE(http.sent.size(),1);http.answer(candles(now,true));
        auto* canvas=firstCanvas(grid);QVERIFY(canvas);QTRY_VERIFY(canvas->isVisible());
        QVERIFY(paintedText(canvas).contains("종가 101 USDT"));
        now+=ChartInstrumentCatalog::FreshnessMs+1000;feed.pump();
        const auto observation=feed.observation("coin:binance:SYNUSDT",Minute);
        QVERIFY(observation.series.available);QVERIFY(observation.delayed);
        QTRY_VERIFY(firstStatus(grid).contains("지연"));
        QVERIFY(!firstStatus(grid).contains("공개 현물"));
        QVERIFY(paintedText(canvas).contains("종가 101 USDT"));
        QCOMPARE(http.sent.size(),1);
    }
    void upbitMetadataFailureRetainsDelayedBarsButRevocationClearsThem(){
        Http http;qint64 now=Now;ChartStudyFeed feed(&http,nullptr,nullptr,[&]{return now;});
        feed.setVerifiedUpbitMarkets({"KRW-SYN"},true);
        ChartGridView grid;grid.setSource(&feed);grid.setPanelCount(2);grid.setSelection(Upbit,Minute);grid.resize(900,420);grid.show();
        feed.pump();http.answer(candles(now));auto* canvas=firstCanvas(grid);QVERIFY(canvas);QTRY_VERIFY(canvas->isVisible());
        QVERIFY(paintedText(canvas).contains("종가 101 KRW"));
        feed.setVerifiedUpbitMarkets({},false);QTRY_VERIFY(firstStatus(grid).contains("지연"));
        QVERIFY(paintedText(canvas).contains("종가 101 KRW"));
        feed.setVerifiedUpbitMarkets({},true);QTRY_VERIFY(firstStatus(grid).contains("지원 없음"));
        QVERIFY(!paintedText(canvas).contains("종가 "));
    }
};
QTEST_MAIN(ChartGridTest)
#include "tst_chart_grid.moc"
