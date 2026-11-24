#include "keefi/MarketCandlesView.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/KeefiTheme.h"
#include "keefi/TradingTransport.h"
#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEngine>
#include <QPainterPath>
#include <QPen>
#include <QTest>
#include <QUrlQuery>
#include <cmath>
using namespace keefi;
namespace {
class Http final:public TradingTransport {
public:
    QList<RequestSpec> requests;QList<quint64> tokens;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{}
};
class Socket final:public PublicCandleSocket {
public:
    quint64 generation=0;int opens=0;
    void open(quint64 value,const QUrl&)override{generation=value;++opens;}
    void send(quint64,const QByteArray&)override{}
    void abort()override{}
};
// Observe the renderer's real line commands. Calculation diagnostics alone can
// pass even if refreshIndicatorUi accidentally sends the legacy BB20 snapshot.
class StudyInkEngine final:public QPaintEngine {
public:
    struct Stroke {QColor color;QPainterPath path;};QList<Stroke> strokes;
    StudyInkEngine():QPaintEngine(AllFeatures){}
    bool begin(QPaintDevice*)override{return true;}bool end()override{return true;}
    Type type()const override{return User;}
    void updateState(const QPaintEngineState& state)override{if(state.state()&DirtyPen)pen_=state.pen();}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&)override{}
    void drawPolygon(const QPointF*,int,PolygonDrawMode)override{}
    void drawTextItem(const QPointF&,const QTextItem&)override{}
    void drawPath(const QPainterPath& path)override{
        if(path.elementCount()<10||pen_.style()!=Qt::SolidLine||std::abs(pen_.widthF()-1)>1e-9)return;
        const auto ink=pen_.color();const auto colors=themeColors();
        if(ink!=colors.studyPurple&&ink!=colors.studyBlue&&ink!=colors.text3&&ink!=QColor("#c018ec"))return;
        // A study polyline has one initial move followed by connected lines.
        // Ignore candle rectangles, text, grids and other non-series paths.
        for(int i=1;i<path.elementCount();++i)if(!path.elementAt(i).isLineTo())return;
        strokes.append({ink,path});
    }
private:QPen pen_;
};
class StudyInkDevice final:public QPaintDevice {
public:
    explicit StudyInkDevice(QSize size):size_(size){}mutable StudyInkEngine engine;
    QPaintEngine* paintEngine()const override{return &engine;}
    int metric(PaintDeviceMetric value)const override{
        switch(value){case PdmWidth:return size_.width();case PdmHeight:return size_.height();case PdmDepth:return 32;
        case PdmNumColors:return 0;case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;}
    }
private:QSize size_;
};
QList<StudyInkEngine::Stroke> paintedStudies(QWidget* chart){StudyInkDevice device(chart->size());chart->render(&device);return device.engine.strokes;}
double emaAtEnd(const QList<MarketCandle>& rows,int period){
    double value=0;for(int i=0;i<period;++i)value+=rows[i].close.toDouble();value/=period;
    const double alpha=2.0/(period+1);for(int i=period;i<rows.size();++i)value+=alpha*(rows[i].close.toDouble()-value);return value;
}
double paintedEndPrice(const StudyInkEngine::Stroke& stroke,QWidget* chart){
    const auto low=chart->property("viewportPriceLow").toDouble(),high=chart->property("viewportPriceHigh").toDouble();
    const auto top=chart->property("viewportPlotTop").toDouble(),height=chart->property("viewportPlotHeight").toDouble();
    return low+(top+height-stroke.path.currentPosition().y())/height*(high-low);
}
struct Fixture {
    qint64 clock=QDateTime::currentMSecsSinceEpoch(),interval=900000,end=clock/interval*interval;
    Http http;Socket* socket=nullptr;MarketCandles model;ChartStudyPreferences preferences;MarketCandlesView view;
    Fixture():model(&http,makeSocket(),[this]{return clock;}),view(&model){
        for(const auto& id:preferences.supportedIds("coin"))preferences.setEnabled(id,false);
        preferences.setEnabled("ma",true);preferences.setEnabled("bb",true);
        view.setIndicatorPreferences(&preferences);view.resize(1280,1000);view.show();
        view.setSelection("coin:upbit:KRW-BTC",interval);emit socket->connected(socket->generation);
    }
    std::unique_ptr<PublicCandleSocket> makeSocket(){auto value=std::make_unique<Socket>();socket=value.get();return value;}
    QWidget* chart(){return view.findChild<QWidget*>("marketCandlesChart");}
    HttpResult page(int pageIndex,bool gap=false)const{
        QJsonArray rows;
        for(int i=200;i>0;--i){
            if(gap&&i==101)continue;
            const auto at=end-(pageIndex*200+i)*interval;const int close=100+(i%17);
            rows.append(QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                {"timestamp",QString::number(at+1000)},{"unit","15"},{"opening_price","100"},{"high_price","120"},{"low_price","90"},
                {"trade_price",QString::number(close)},{"candle_acc_trade_volume","2"}});
        }
        HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();return result;
    }
    void deliver(int pageIndex,bool gap=false){clock+=1100;emit http.finished(http.tokens.last(),page(pageIndex,gap));}
    ChartIndicatorState state(const QString& id)const{for(const auto& state:view.indicatorStates())if(state.id==id)return state;return {};}
};
}
class ChartDisplayWarmupTest:public QObject {
    Q_OBJECT
private slots:
    void savedInputsReachWorkersAndStyleOnlyChangesRepaintWithoutCalculation() {
        Fixture f;
        auto settings=f.preferences.studySettings();
        settings.rsiPeriod=5;settings.macdFast=4;settings.macdSlow=8;settings.macdSignal=3;
        settings.displayBandPeriod=10;settings.displayBandDeviations=1.25;
        auto ma=f.preferences.movingAverages();ma[0].enabled=false;ma[1].period=9;ma[1].kind=ExtendedMaKind::Simple;
        QVERIFY(f.preferences.setStudySettings(settings,ma));
        QVERIFY(f.preferences.setEnabled("rsi",true));QVERIFY(f.preferences.setEnabled("macd",true));f.deliver(0);
        QTRY_COMPARE(f.chart()->property("displayBandPointCount").toInt(),191);
        QTRY_COMPARE(f.view.extendedStudies().rsi.points.size(),195);QTRY_COMPARE(f.view.extendedStudies().macd.histogram.size(),191);
        const auto rows=f.model.series().candles;
        double mean=0,variance=0;for(const auto& row:rows.last(10))mean+=row.close.toDouble()/10;
        for(const auto& row:rows.last(10))variance+=std::pow(row.close.toDouble()-mean,2)/10;
        auto paths=paintedStudies(f.chart());QVERIFY(paths.size()>=5);
        QVERIFY(std::abs(paintedEndPrice(paths.last(),f.chart())-mean-1.25*std::sqrt(variance))<1e-6);
        const int calculations=f.view.property("displayStudyCalculationCount").toInt();
        const auto revision=f.view.indicatorRevision();
        settings.colors.insert("ma2",QColor("#c018ec"));QVERIFY(f.preferences.setStudySettings(settings,ma));
        const auto coloredEnd=[&]{
            for(const auto& stroke:paintedStudies(f.chart()))if(stroke.color==QColor("#c018ec"))return paintedEndPrice(stroke,f.chart());
            return -1.0;
        };
        double sma=0;for(const auto& row:rows.last(9))sma+=row.close.toDouble()/9;
        QTRY_VERIFY(std::abs(coloredEnd()-sma)<1e-6);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),calculations);QCOMPARE(f.view.indicatorRevision(),revision);
        ma[1].kind=ExtendedMaKind::Exponential;
        settings.rsiPeriod=25;settings.macdSlow=30;settings.displayBandPeriod=20;
        QVERIFY(f.preferences.setStudySettings(settings,ma));
        QTRY_COMPARE(f.view.extendedStudies().rsi.points.size(),175);QTRY_COMPARE(f.view.extendedStudies().macd.histogram.size(),169);
        QTRY_COMPARE(f.chart()->property("displayBandPointCount").toInt(),181);
        QTRY_VERIFY(std::abs(coloredEnd()-emaAtEnd(rows,9))<1e-6);
        settings.macdSlow=120;settings.macdSignal=81;
        QVERIFY(f.preferences.setStudySettings(settings,ma));
        QTRY_COMPARE(f.view.extendedStudies().macd.histogram.size(),1);
        QCOMPARE(f.view.extendedStudies().macd.signal.size(),1);
        QVERIFY(f.state("macd").requiredReady);
    }
    void historyWarmsDrawingWithoutExpandingObservation(){
        Fixture f;QVERIFY(!f.http.tokens.isEmpty());f.deliver(0);
        QCOMPARE(f.socket->opens,1);
        QTRY_COMPARE(f.view.extendedStudies().bb.points.size(),181);
        QTRY_COMPARE(f.chart()->property("displayBandPointCount").toInt(),1);
        QVERIFY(!f.state("bb").requiredReady);
        QCOMPARE(paintedStudies(f.chart()).size(),3); // EMA21/50/100 only; one BB/EMA200 value is not a segment.
        const auto before=f.view.extendedStudies();const auto first=f.model.series().candles.first().startsAtMs;
        QTRY_COMPARE(f.http.requests.size(),2);
        QCOMPARE(f.http.requests.last().url,MarketCandles::historyRequest("coin:upbit:KRW-BTC",f.interval,f.end-200*f.interval).url);
        f.deliver(1);
        QTRY_COMPARE(f.chart()->property("displayBandPointCount").toInt(),201);
        QTRY_VERIFY(f.state("ma").requiredReady&&f.state("bb").requiredReady);
        QCOMPARE(f.chart()->property("displayMaPointCounts").toMap().value("ma4").toInt(),201);
        const auto strokes=paintedStudies(f.chart());QCOMPARE(strokes.size(),7); // Four MA polylines plus three BB boundaries.
        const auto history=f.model.displayObservation().candles;
        const QList<int> periods{21,50,100,200};
        for(int i=0;i<periods.size();++i){
            QCOMPARE(strokes[i].color,i%2?themeColors().studyBlue:themeColors().studyPurple);
            QVERIFY2(std::abs(paintedEndPrice(strokes[i],f.chart())-emaAtEnd(history,periods[i]))<1e-7,
                     qPrintable(QString("Rendered EMA%1 must use the display history").arg(periods[i])));
            QVERIFY(strokes[i].path.elementCount()>2);
        }
        double mean=0,variance=0;for(int i=history.size()-200;i<history.size();++i)mean+=history[i].close.toDouble();mean/=200;
        for(int i=history.size()-200;i<history.size();++i)variance+=std::pow(history[i].close.toDouble()-mean,2);variance/=200;
        const QList<double> band{mean-2*std::sqrt(variance),mean,mean+2*std::sqrt(variance)};
        for(int i=0;i<3;++i){
            QCOMPARE(strokes[4+i].color,i==1?themeColors().studyBlue:themeColors().text3);
            QVERIFY2(std::abs(paintedEndPrice(strokes[4+i],f.chart())-band[i])<1e-7,"Rendered BB must use Length 200 and population sigma 2");
        }
        QCOMPARE(f.model.series().candles.size(),200);
        QCOMPARE(f.model.series().candles.first().startsAtMs,first);
        QCOMPARE(f.model.displayObservation().candles.size(),400);
        QCOMPARE(f.http.requests.size(),2);QCOMPARE(f.socket->opens,1); // One history REST; no additional socket.
        QCOMPARE(f.view.extendedStudies().observations,before.observations);
        QCOMPARE(f.view.extendedStudies().bb.points.size(),before.bb.points.size());
        QCOMPARE(f.view.extendedStudies().studyEntries.value("bb").size(),before.studyEntries.value("bb").size());
        QCOMPARE(f.view.extendedStudies().scorecards.value("bb").samples,before.scorecards.value("bb").samples);
        QCOMPARE(f.view.extendedStudies().lastCompletedAtMs,before.lastCompletedAtMs);
        const int requests=f.http.requests.size(),calculations=f.view.property("displayStudyCalculationCount").toInt();
        for(int i=0;i<20;++i)emit f.model.changed();
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),requests);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),calculations);
    }
    void priceScaleFitsVisibleLongStudies(){
        Fixture f;
        const auto deliverFlatPage=[&](int pageIndex,int price){
            auto response=f.page(pageIndex);
            auto rows=QJsonDocument::fromJson(response.body).array();
            for(qsizetype i=0;i<rows.size();++i){
                auto row=rows[i].toObject();
                row["opening_price"]=QString::number(price);
                row["high_price"]=QString::number(price+2);
                row["low_price"]=QString::number(price-2);
                row["trade_price"]=QString::number(price);
                rows[i]=row;
            }
            response.body=QJsonDocument(rows).toJson();
            f.clock+=1100;
            emit f.http.finished(f.http.tokens.last(),response);
        };
        deliverFlatPage(0,100);
        QTRY_COMPARE(f.http.requests.size(),2);
        deliverFlatPage(1,60);
        QTRY_VERIFY(f.state("ma").requiredReady&&f.state("bb").requiredReady);
        const auto strokes=paintedStudies(f.chart());
        QCOMPARE(strokes.size(),7);
        const auto top=f.chart()->property("viewportPlotTop").toDouble();
        const auto bottom=top+f.chart()->property("viewportPlotHeight").toDouble();
        for(const auto& stroke:strokes){
            QVERIFY2(stroke.path.boundingRect().top()>=top,"Visible study exceeded the top of the automatic price range");
            QVERIFY2(stroke.path.boundingRect().bottom()<=bottom,"Visible study fell below the automatic price range");
        }
        f.preferences.setVisible("ma",false);
        f.preferences.setVisible("bb",false);
        QTRY_VERIFY(paintedStudies(f.chart()).isEmpty());
        QVERIFY(f.chart()->property("viewportPriceLow").toDouble()>95);
        QVERIFY(f.chart()->property("viewportPriceHigh").toDouble()<105);
    }
    void reopeningSameSelectionWarmsAgain(){
        Fixture f;f.deliver(0);QTRY_COMPARE(f.http.requests.size(),2);f.deliver(1);
        QTRY_VERIFY(f.state("ma").requiredReady&&f.state("bb").requiredReady);
        QCOMPARE(f.model.displayObservation().candles.size(),400);
        QCOMPARE(paintedStudies(f.chart()).size(),7);
        QTRY_COMPARE(f.view.extendedStudies().bb.points.size(),181);
        const auto first=f.model.series().candles.first().startsAtMs;
        const auto before=f.view.extendedStudies();
        const auto historyBefore=f.model.displayObservation();const auto officialBefore=f.model.restObservation();
        QList<double> paintedEndsBefore;for(const auto& stroke:paintedStudies(f.chart()))paintedEndsBefore.append(paintedEndPrice(stroke,f.chart()));
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),1);
        f.view.hide();
        QCOMPARE(f.model.historyStatus().completedCount,0);
        QVERIFY(f.model.series().identity.isEmpty());
        QVERIFY(f.model.series().candles.isEmpty());QVERIFY(f.model.displayObservation().candles.isEmpty());
        QVERIFY(f.model.restObservation().candles.isEmpty());
        f.view.show();emit f.socket->connected(f.socket->generation);
        QTRY_COMPARE(f.http.requests.size(),3);
        QCOMPARE(f.http.requests.last().url,MarketCandles::request("coin:upbit:KRW-BTC",f.interval).url);
        // Performance contract: the exact selection's bounded display history is
        // reusable after reopening, but it is not fresh REST evidence. Re-warm
        // the indicators only after a new native REST receipt; never refetch a
        // history page that is already available in the display-only cache.
        const auto cached=f.model.displayObservation();
        QCOMPARE(cached.identity,QString("coin:upbit:KRW-BTC"));QCOMPARE(cached.intervalMs,f.interval);
        QCOMPARE(cached.currency,QString("KRW"));QCOMPARE(cached.candles.size(),400);
        QVERIFY(!cached.available&&cached.delayed&&cached.fromMemoryCache&&!cached.fromArchive);
        QVERIFY(!cached.forming);QCOMPARE(cached.receivedAtMs,historyBefore.receivedAtMs);
        QCOMPARE(cached.sourceTimeMs,historyBefore.sourceTimeMs);
        QCOMPARE(cached.candles.first().startsAtMs,historyBefore.candles.first().startsAtMs);
        QCOMPARE(cached.candles.last().startsAtMs,historyBefore.candles.last().startsAtMs);
        QVERIFY(!f.model.series().available);QVERIFY(f.model.series().candles.isEmpty());
        QVERIFY(f.model.restObservation().candles.isEmpty());
        QVERIFY(!f.view.extendedStudies().available);QCOMPARE(f.view.extendedStudies().observations,0);
        QVERIFY(paintedStudies(f.chart()).isEmpty());
        QVERIFY(f.chart()->property("displayHistoricalOnly").toBool());
        QVERIFY(f.chart()->property("displayFromMemoryCache").toBool());
        const int pendingCalculations=f.view.property("displayStudyCalculationCount").toInt();
        for(int i=0;i<20;++i){emit f.model.changed();emit f.model.historyChanged();}
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),3);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),pendingCalculations);
        f.deliver(0); // Same 200 observations and oldest timestamp as before hiding.
        QCOMPARE(f.model.historyStatus().completedCount,400);
        QCOMPARE(f.model.historyStatus().oldestAtMs,historyBefore.candles.first().startsAtMs);
        QCOMPARE(f.model.historyStatus().state,CandleHistoryState::Ready);
        QVERIFY(f.model.series().available);QVERIFY(!f.model.series().delayed);
        const auto fresh=f.model.restObservation();QVERIFY(fresh.available);
        QCOMPARE(fresh.candles.size(),200);QCOMPARE(fresh.candles.first().startsAtMs,first);
        QVERIFY(fresh.restReceivedAtMs>officialBefore.restReceivedAtMs);
        QVERIFY(fresh.restRevision!=officialBefore.restRevision);
        QTRY_VERIFY(f.state("ma").requiredReady&&f.state("bb").requiredReady);
        QCOMPARE(f.model.displayObservation().candles.size(),400);
        QVERIFY(f.model.displayObservation().available);
        QVERIFY(!f.model.displayObservation().delayed&&!f.model.displayObservation().fromMemoryCache);
        QVERIFY(!f.chart()->property("displayHistoricalOnly").toBool());
        QVERIFY(!f.chart()->property("displayFromMemoryCache").toBool());
        QCOMPARE(f.chart()->property("displayBandPointCount").toInt(),201);
        QCOMPARE(f.chart()->property("displayMaPointCounts").toMap().value("ma4").toInt(),201);
        const auto strokes=paintedStudies(f.chart());QCOMPARE(strokes.size(),paintedEndsBefore.size());
        for(qsizetype i=0;i<strokes.size();++i)
            QVERIFY2(std::abs(paintedEndPrice(strokes[i],f.chart())-paintedEndsBefore[i])<1e-7,
                     "Reopened MA/BB lines must retain their values using the same display history");
        QCOMPARE(f.model.series().candles.size(),200);
        QCOMPARE(f.model.series().candles.first().startsAtMs,first);
        QTRY_COMPARE(f.view.extendedStudies().observations,before.observations);
        QCOMPARE(f.view.extendedStudies().bb.points.size(),before.bb.points.size());
        QCOMPARE(f.view.extendedStudies().studyEntries.value("bb").size(),before.studyEntries.value("bb").size());
        QCOMPARE(f.view.extendedStudies().scorecards.value("bb").samples,before.scorecards.value("bb").samples);
        const auto after=f.view.extendedStudies();QCOMPARE(after.lastCompletedAtMs,before.lastCompletedAtMs);
        for(qsizetype i=0;i<before.bb.points.size();++i){
            QCOMPARE(after.bb.points[i].atMs,before.bb.points[i].atMs);
            QCOMPARE(after.bb.points[i].lower,before.bb.points[i].lower);
            QCOMPARE(after.bb.points[i].middle,before.bb.points[i].middle);
            QCOMPARE(after.bb.points[i].upper,before.bb.points[i].upper);
        }
        QCOMPARE(after.maLines.size(),before.maLines.size());
        for(qsizetype i=0;i<before.maLines.size();++i){
            QCOMPARE(after.maLines[i].spec.id,before.maLines[i].spec.id);
            QCOMPARE(after.maLines[i].points.size(),before.maLines[i].points.size());
            for(qsizetype j=0;j<before.maLines[i].points.size();++j){
                QCOMPARE(after.maLines[i].points[j].atMs,before.maLines[i].points[j].atMs);
                QCOMPARE(after.maLines[i].points[j].price,before.maLines[i].points[j].price);
            }
        }
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),1);
        const int calculations=f.view.property("displayStudyCalculationCount").toInt();
        for(int i=0;i<20;++i){emit f.model.changed();emit f.model.historyChanged();}
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),3);QCOMPARE(f.socket->opens,2); // Fresh REST is required; cached history is not fetched twice.
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),calculations);
    }
    void readyTransitionWarmsWithoutRecalculation(){
        Fixture f;f.preferences.setEnabled("ma",false);f.preferences.setEnabled("bb",false);f.deliver(0);
        QTRY_VERIFY(f.view.extendedStudies().available);QCOMPARE(f.http.requests.size(),1);
        const auto first=f.model.series().candles.first().startsAtMs;
        f.model.clearDisplayCache(); // Retains fresh observations while history awaits the replacement REST.
        QCOMPARE(f.model.historyStatus().state,CandleHistoryState::Unavailable);
        QCOMPARE(f.model.historyStatus().completedCount,0);
        QVERIFY(f.model.series().available);QCOMPARE(f.model.series().candles.size(),200);
        QCOMPARE(f.http.requests.size(),2);
        f.preferences.setEnabled("ma",true);f.preferences.setEnabled("bb",true);
        QTRY_COMPARE(f.chart()->property("displayBandPointCount").toInt(),1);
        QTRY_COMPARE(f.view.extendedStudies().bb.points.size(),181);
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),0);
        const auto before=f.view.extendedStudies();
        const int calculations=f.view.property("displayStudyCalculationCount").toInt();
        f.deliver(0); // Identical completed bytes; only history eligibility changes to Ready.
        QCOMPARE(f.model.historyStatus().completedCount,200);
        QTRY_COMPARE(f.http.requests.size(),3);
        QCOMPARE(f.http.requests.last().url,MarketCandles::historyRequest("coin:upbit:KRW-BTC",f.interval,first).url);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),calculations);
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),1);
        for(int i=0;i<20;++i){emit f.model.changed();emit f.model.historyChanged();}
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),3);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),calculations);
        f.deliver(1);
        QTRY_VERIFY(f.state("ma").requiredReady&&f.state("bb").requiredReady);
        QCOMPARE(paintedStudies(f.chart()).size(),7);
        QCOMPARE(f.model.displayObservation().candles.size(),400);
        QCOMPARE(f.model.series().candles.size(),200);
        QCOMPARE(f.model.series().candles.first().startsAtMs,first);
        QCOMPARE(f.view.extendedStudies().observations,before.observations);
        QCOMPARE(f.view.extendedStudies().bb.points.size(),before.bb.points.size());
        QCOMPARE(f.view.extendedStudies().scorecards.value("bb").samples,before.scorecards.value("bb").samples);
        QCOMPARE(f.http.requests.size(),3);QCOMPARE(f.socket->opens,2);
    }
    void stoppedHistoryDoesNotRetryOnCachedRefresh_data(){
        QTest::addColumn<int>("status");QTest::addColumn<int>("expectedState");
        QTest::newRow("end")<<200<<int(CandleHistoryState::End);
        QTest::newRow("rate-limited")<<429<<int(CandleHistoryState::RateLimited);
        QTest::newRow("delayed")<<503<<int(CandleHistoryState::Delayed);
    }
    void stoppedHistoryDoesNotRetryOnCachedRefresh(){
        QFETCH(int,status);QFETCH(int,expectedState);
        Fixture f;f.deliver(0);QTRY_COMPARE(f.http.requests.size(),2);
        QTRY_COMPARE(f.chart()->property("displayBandPointCount").toInt(),1);
        HttpResult response;response.status=status;response.body="[]";f.clock+=1100;
        emit f.http.finished(f.http.tokens.last(),response);
        QCOMPARE(int(f.model.historyStatus().state),expectedState);
        const int calculations=f.view.property("displayStudyCalculationCount").toInt();
        f.clock+=2000; // Even after the minimum retry delay, refresh alone is not a user retry.
        for(int i=0;i<20;++i){emit f.model.changed();emit f.model.historyChanged();}
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),2);QCOMPARE(f.socket->opens,1);
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),1);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(),calculations);
        QCOMPARE(f.model.series().candles.size(),200);
        QCOMPARE(f.model.displayObservation().candles.size(),200);
        QVERIFY(!f.state("ma").requiredReady&&!f.state("bb").requiredReady);
    }
    void gapDoesNotTriggerFutileOlderReads(){
        Fixture f;f.deliver(0,true);
        QTRY_VERIFY(f.view.extendedStudies().available);
        QTRY_VERIFY(f.chart()->property("displayBandPointCount").isValid());
        QCOMPARE(f.chart()->property("displayBandPointCount").toInt(),0);
        QVERIFY(!f.state("bb").requiredReady);
        for(int i=0;i<20;++i)emit f.model.changed();
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),1);
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),0);
    }
    void hiddenAppliedStudiesStillWarmUp(){
        Fixture f;f.preferences.setVisible("ma",false);f.preferences.setVisible("bb",false);f.deliver(0);
        QTRY_COMPARE(f.http.requests.size(),2);f.deliver(1);
        QTRY_VERIFY(f.state("bb").requiredReady);
        QVERIFY(f.state("bb").enabled&&f.state("bb").hidden);
        QVERIFY(f.state("ma").enabled&&f.state("ma").hidden);
        QVERIFY(paintedStudies(f.chart()).isEmpty());
        f.preferences.setVisible("bb",true);QCoreApplication::processEvents();
        QCOMPARE(paintedStudies(f.chart()).size(),3); // Visibility exposes already calculated BB, without enabling MA.
        QCOMPARE(f.http.requests.size(),2);
    }
    void disabledStudiesDoNotFetchWarmup(){
        Fixture f;f.preferences.setEnabled("ma",false);f.preferences.setEnabled("bb",false);f.deliver(0);
        QTRY_VERIFY(f.view.extendedStudies().available);
        QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),1);
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),0);
    }
    void changedPreferencesCancelQueuedWarmup_data(){
        QTest::addColumn<bool>("disableApplied");
        QTest::newRow("both-studies-off")<<true;
        QTest::newRow("long-slot-off")<<false;
    }
    void changedPreferencesCancelQueuedWarmup(){
        QFETCH(bool,disableApplied);Fixture f;f.deliver(0);
        QCOMPARE(f.http.requests.size(),1); // Warmup is queued, not dispatched yet.
        f.preferences.setEnabled("bb",false);
        if(disableApplied)f.preferences.setEnabled("ma",false);
        else{auto averages=f.preferences.movingAverages();averages.last().enabled=false;QVERIFY(f.preferences.setMovingAverages(averages));}
        QTRY_VERIFY(f.view.extendedStudies().available);QCoreApplication::processEvents();
        QCOMPARE(f.http.requests.size(),1);
        QCOMPARE(f.view.property("displayWarmupRequests").toInt(),0);
    }
};
QTEST_MAIN(ChartDisplayWarmupTest)
#include "tst_chart_display_warmup.moc"
