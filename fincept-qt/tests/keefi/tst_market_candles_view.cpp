#include <QAccessible>
#include "keefi/KeefiTheme.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/InfoButton.h"
#include <QPlainTextEdit>
#include <QDialog>
#include "keefi/TradingTransport.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/StockChartSource.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/LocalWindow.h"
#include "keefi/AccountModel.h"
#include "keefi/BackgroundTray.h"
#include "keefi/Vault.h"
#include "keefi/ChartDrawings.h"
#include "keefi/AutoChartGeometry.h"
#include "keefi/CandleChart_p.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/ChartShareCard.h"
#include "keefi/KeefiFonts.h"
#include "keefi/PortfolioPresentation.h"
#include <QFile>
#include <QKeySequence>
#include <QDateTime>
#include <QApplication>
#include <QDir>
#include <QScreen>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <QElapsedTimer>
#include <QLocale>
#include <QTimeZone>
#include <QVariantAnimation>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSettings>
#include <QToolButton>
#include <QCheckBox>
#include <QComboBox>
#include <QTreeWidget>
#include <QScopeGuard>
#include <QTimer>
#include <QMenu>
#include <QContextMenuEvent>
#include <QStyleOptionButton>
#include <QStyle>
#include <QPushButton>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QPaintEngine>
#include <QPainter>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QToolTip>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QWheelEvent>
#include <QPinchGesture>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QGestureEvent>
#include <QUrlQuery>
#include <QThreadPool>
#include <QSemaphore>
#include <QtMath>
#include <limits>
#include <cmath>
using namespace keefi;
namespace {
void expandAppliedIndicators(MarketCandlesView& view) {
    auto* applied = view.findChild<QPushButton*>("chartStudyAppliedToggle");
    QVERIFY(applied);
    if (!applied->isChecked()) QTest::mouseClick(applied, Qt::LeftButton);
    QCoreApplication::processEvents();
}
class CandleTextEngine final:public QPaintEngine {
  public:
    struct Run {QString text;QFont font;QPointF position;};QList<Run> runs;
    struct Stroke {QLineF line;QColor ink;};QList<Stroke> strokes;
    bool begin(QPaintDevice*)override{return true;}bool end()override{return true;}
    Type type()const override{return User;}void updateState(const QPaintEngineState& state)override{ink=state.pen().color();}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&)override{}
    void drawPath(const QPainterPath&)override{}void drawPolygon(const QPointF*,int,PolygonDrawMode)override{}
    void drawTextItem(const QPointF& position,const QTextItem& item)override{runs.append({item.text(),item.font(),position});}
    void drawLines(const QLineF* lines,int count)override{
        for(int i=0;i<count;++i)strokes.append({lines[i],ink});
    }
  private:
    QColor ink;
};
class CandleTextDevice final:public QPaintDevice {
  public:
    mutable CandleTextEngine engine;QPaintEngine* paintEngine()const override{return &engine;}
    int metric(PaintDeviceMetric value)const override{
        switch(value){case PdmWidth:case PdmHeight:return 2000;case PdmDepth:return 32;case PdmNumColors:return 0;
        case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;}
    }
};
QString displayedText(const QLabel* label){
    if(label->textFormat()==Qt::RichText||(label->textFormat()==Qt::AutoText&&Qt::mightBeRichText(label->text()))){
        QTextDocument document;document.setHtml(label->text());return document.toPlainText();
    }
    return label->text();
}
int independentLabelHeight(const QLabel& label){
    // Do not call heightForWidth on the label under test: rich-text measurement
    // can update its internal text-layout cache before the screenshot is taken.
    QLabel witness;
    witness.setFont(label.font());witness.setStyleSheet(label.styleSheet());
    witness.setFrameStyle(label.frameStyle());witness.setLineWidth(label.lineWidth());witness.setMidLineWidth(label.midLineWidth());
    witness.setContentsMargins(label.contentsMargins());witness.setMargin(label.margin());witness.setIndent(label.indent());
    witness.setTextFormat(label.textFormat());witness.setTextInteractionFlags(label.textInteractionFlags());
    witness.setAlignment(label.alignment());witness.setLayoutDirection(label.layoutDirection());
    witness.setWordWrap(label.wordWrap());witness.setText(label.text());witness.ensurePolished();
    return witness.heightForWidth(label.width());
}
QString coreStudyWidthConstraints(const MarketCandlesView& view){
    const auto describe=[](const QWidget* widget){
        if(!widget)return QStringLiteral("missing widget");
        const auto minimum=widget->minimumSize(),hint=widget->minimumSizeHint();
        return QString("%1 minimum %2x%3, hint %4x%5").arg(widget->objectName())
            .arg(minimum.width()).arg(minimum.height()).arg(hint.width()).arg(hint.height());
    };
    return describe(&view)+QStringLiteral("; ")+describe(view.findChild<QWidget*>("chartStudyLegend"))+
        QStringLiteral("; ")+describe(view.findChild<QWidget*>("chartCoreStudySummary"));
}
QString coreStudyLayoutProblem(MarketCandlesView& view,int expectedWidth){
    if(view.width()!=expectedWidth)
        return QString("Chart fixture has width %1, expected %2; %3").arg(view.width()).arg(expectedWidth).arg(coreStudyWidthConstraints(view));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");
    auto* summary=view.findChild<QWidget*>("chartCoreStudySummary");
    if(!chart||!summary)return QStringLiteral("Core study summary or candle chart is missing");
    const QRect candleBounds(chart->mapTo(&view,QPoint()),chart->size());
    const QRect summaryBounds(summary->mapTo(&view,QPoint()),summary->size());
    QRect previousCard;
    for(const auto* id:{"pullback","similarity","whales"}){
        auto* card=view.findChild<QWidget*>("chartCoreStudyCard_"+QString(id));
        if(!card)return QStringLiteral("Core study card is missing: ")+id;
        const QRect cardBounds(card->mapTo(&view,QPoint()),card->size());
        if(expectedWidth==640){
            if(cardBounds.left()!=summaryBounds.left()||cardBounds.right()!=summaryBounds.right()||
                (!previousCard.isNull()&&previousCard.bottom()>=cardBounds.top()))
                return QStringLiteral("Narrow core study summaries have not settled into three full-width rows: ")+id;
        }else if(expectedWidth==1280){
            if(cardBounds.top()!=summaryBounds.top()||
                (!previousCard.isNull()&&previousCard.right()>=cardBounds.left()))
                return QStringLiteral("Wide core study summaries have not settled into three columns: ")+id;
        }else return QStringLiteral("Unsupported core study fixture width");
        previousCard=cardBounds;
        for(const auto* prefix:{"chartCoreStudyState_","chartCoreStudyDetail_"}){
            auto* label=card->findChild<QLabel*>(QString(prefix)+id);
            if(!label)return QStringLiteral("Core study label is missing: ")+prefix+id;
            if(!label->isVisible())continue;
            // QLabel's native text layout accounts for rich links, explicit
            // line breaks, fallback glyphs and wrapping at the actual width.
            // The old test checked only the heading's one-line font height.
            const int required=independentLabelHeight(*label);
            if(required<0||label->height()<required)
                return QString("%1 clips wrapped text at width %2: actual height %3, required %4")
                    .arg(label->objectName()).arg(label->width()).arg(label->height()).arg(required);
            const QRect bounds(label->mapTo(&view,QPoint()),label->size());
            if(!card->rect().contains(label->geometry())||!summaryBounds.contains(bounds)||!view.rect().contains(bounds))
                return label->objectName()+QStringLiteral(" extends beyond its card, summary or view");
            if(bounds.intersects(candleBounds))return label->objectName()+QStringLiteral(" overlaps the candles");
        }
    }
    return {};
}
constexpr qint64 StudyNow=1728000000000+250*60000+10000;
class StudyWorkerGate {
  public:
    StudyWorkerGate():pool(QThreadPool::globalInstance()),oldMaximum(pool->maxThreadCount()){
        pool->waitForDone();pool->setMaxThreadCount(1);
        pool->start([this]{entered.release();release.acquire();});
        started=entered.tryAcquire(1,3000);
    }
    ~StudyWorkerGate(){open();pool->waitForDone();pool->setMaxThreadCount(oldMaximum);}
    void open(){if(!opened){opened=true;release.release();}}
    QThreadPool* pool;int oldMaximum;bool started=false,opened=false;QSemaphore entered,release;
};
class StudyPaintObserver:public QObject {
  public:int paints=0;
    bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Paint)++paints;return false;}
};
class StudyTransport final:public TradingTransport {
  public:
    quint64 token=0;
    void send(quint64 t,const RequestSpec&)override{token=t;}
    void abort()override{}
};
class StudySocket final:public PublicCandleSocket {
  public:
    quint64 generation=0;
    void open(quint64 g,const QUrl&)override{generation=g;}
    void send(quint64,const QByteArray&)override{}
    void abort()override{}
};
QList<MarketCandle> syntheticStudyRows(int count,bool repeating=true){
    const int pattern[]{100,101,99,102,100,104,101,103,100,105,102,106,103,107,104,108,106,109,107,110};
    const auto end=StudyNow/60000*60000;QList<MarketCandle> rows;
    for(int i=0;i<count;++i){const int price=repeating?pattern[i%20]:100;const auto at=end-(count-i)*60000;
        rows.append({at,at+1000,QString::number(price),QString::number(price+1),QString::number(price-1),QString::number(price),"2"});}
    return rows;
}
QJsonObject syntheticStudyWire(const MarketCandle& row){
    return {{"market","KRW-SYN"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(row.startsAtMs,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(row.lastStoredTickMs)},{"unit","1"},{"opening_price",row.open},{"high_price",row.high},
        {"low_price",row.low},{"trade_price",row.close},{"candle_acc_trade_volume",row.volume}};
}
HttpResult syntheticStudyResponse(const QList<MarketCandle>& rows){
    QJsonArray array;for(const auto& row:rows)array.append(syntheticStudyWire(row));
    HttpResult result;result.status=200;result.body=QJsonDocument(array).toJson();return result;
}
bool saveDev86IndicatorCapture(MarketCandlesView& view,const QString& stem){
    const auto directory=qEnvironmentVariable("KEEFI_DEV86_CAPTURE_DIR");if(directory.isEmpty())return true;
    if(!QDir().mkpath(directory)||!view.grab().save(directory+"/"+stem+".png"))return false;
    QJsonObject receipt{{"kind","synthetic-fixture-qt-render"},{"scenario",stem},{"width",view.width()},{"height",view.height()}};
    for(const auto* id:{"pullback","similarity","whales"}){auto* label=view.findChild<QLabel*>("chartCoreStudyState_"+QString(id));auto* detail=view.findChild<QLabel*>("chartCoreStudyDetail_"+QString(id));if(label&&detail)receipt[id]=QJsonObject{{"state",label->text()},{"detail",displayedText(detail)},{"visible",label->isVisible()},
        {"heading_height",label->height()},{"heading_required_height",independentLabelHeight(*label)},
        {"detail_width",detail->width()},{"detail_height",detail->height()},{"detail_required_height",independentLabelHeight(*detail)}};}
    QFile file(directory+"/"+stem+".json");if(!file.open(QIODevice::WriteOnly))return false;const auto bytes=QJsonDocument(receipt).toJson();return file.write(bytes)==bytes.size();
}
bool saveD3CandleRender(const QString& name,const QImage& image){
    const auto directory=qEnvironmentVariable("KEEFI_D3_CANDLE_RENDER_DIR");return directory.isEmpty()||(QDir().mkpath(directory)&&image.save(directory+"/"+name+".png"));
}
QList<QPoint> visibleBodyPoints(QWidget* chart){
    QEvent leave(QEvent::Leave);QApplication::sendEvent(chart,&leave);const auto image=chart->grab().toImage();int best=0,left=0,right=0,row=0;
    const auto ink=MovementColorPolicy::colorFor(chart,MovementDirection::Flat);const auto scale=image.devicePixelRatio();
    for(int y=qCeil(12*scale);y<qFloor(image.height()-30*scale);++y){int count=0,first=-1,last=-1;for(int x=qCeil(8*scale);x<qFloor(image.width()-75*scale);++x)if(image.pixelColor(x,y)==ink){if(first<0)first=x;last=x;++count;}
        if(count>best){best=count;left=first;right=last;row=y;}}
    if(best<2||left==right)return {};const auto ratio=image.devicePixelRatio();return {{qRound(left/ratio),qRound(row/ratio)},{qRound(right/ratio),qRound(row/ratio)}};
}
QList<MarketCandle> flatDrawingRows(){auto rows=syntheticStudyRows(6);for(auto& row:rows){row.open=row.close="100";row.high="101";row.low="99";}return rows;}
QList<QPoint> visibleBodyCenters(QWidget* chart){
    QEvent leave(QEvent::Leave);QApplication::sendEvent(chart,&leave);const auto image=chart->grab().toImage();int best=0,row=-1;
    const auto ink=MovementColorPolicy::colorFor(chart,MovementDirection::Flat);const auto scale=image.devicePixelRatio();
    for(int y=qCeil(12*scale);y<qFloor(image.height()-30*scale);++y){int count=0;for(int x=qCeil(8*scale);x<qFloor(image.width()-75*scale);++x)if(image.pixelColor(x,y)==ink)++count;if(count>best){best=count;row=y;}}
    QList<QPoint> centers;if(row<0)return centers;int from=-1;const auto ratio=image.devicePixelRatio();
    for(int x=qCeil(8*scale);x<=qFloor(image.width()-75*scale);++x){const bool green=x<qFloor(image.width()-75*scale)&&image.pixelColor(x,row)==ink;if(green&&from<0)from=x;
        if(!green&&from>=0){if(x-from>=3)centers.append({qRound((from+x-1)/2.0/ratio),qRound(row/ratio)});from=-1;}}
    return centers;
}
bool saveDrawingPreview(MarketCandlesView& view,const QString& stem){
    const auto destination=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");if(destination.isEmpty())return true;
    QTemporaryDir profile;if(!profile.isValid()||!QDir().mkpath(destination))return false;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.filePath("settings"));
    class UnavailableTray final:public BackgroundTray{public:bool available()const override{return false;}bool visible()const override{return false;}
        void setMenu(QMenu*)override{}void show()override{}void hide()override{}} tray;
    Vault vault(profile.filePath("synthetic-c8-theme-vault"));AccountModel accounts(&vault);LocalWindow donor(vault,accounts,nullptr,&tray);view.setStyleSheet(donor.styleSheet());
    view.resize(1280,1000);QCoreApplication::processEvents();const bool wide=view.grab().save(QDir(destination).filePath(stem+"-wide.png"));
    view.resize(820,1100);QCoreApplication::processEvents();return view.grab().save(QDir(destination).filePath(stem+"-narrow.png"))&&wide;
}
}
namespace {
class NavigationHttp final:public TradingTransport {
public:
    QList<quint64> tokens;QList<RequestSpec> requests;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{}
};
struct NavigationFixture {
    qint64 clock=QDateTime::currentMSecsSinceEpoch(),interval=900000,end=clock/interval*interval;
    bool binance=false,repeating=false;QString identity;
    NavigationHttp http;StudySocket* socket=nullptr;MarketCandles model;ChartStudyPreferences preferences;MarketCandlesView view;
    explicit NavigationFixture(bool isBinance=false,bool repeat=false):binance(isBinance),repeating(repeat),identity(isBinance?"coin:binance:BTCUSDT":"coin:upbit:KRW-BTC"),
        model(&http,makeSocket(),[this]{return clock;}),view(&model){
        if(binance){MarketInstrument item;item.symbol="BTCUSDT";item.baseAsset="BTC";item.quoteAsset="USDT";item.market="spot";item.status="TRADING";item.tradable=true;model.setInstruments("binance",{item});}
        preferences.setEnabled("similarity",false);view.setIndicatorPreferences(&preferences);view.resize(1280,1000);view.show();view.setSelection(identity,interval);
        emit socket->connected(socket->generation);deliver(0);QCoreApplication::processEvents();
    }
    std::unique_ptr<PublicCandleSocket> makeSocket(){auto result=std::make_unique<StudySocket>();socket=result.get();return result;}
    QWidget* chart()const{return view.findChild<QWidget*>("marketCandlesChart");}
    HttpResult page(int pageIndex)const{
        QJsonArray rows;for(int i=200;i>0;--i){const auto at=end-(pageIndex*200+i)*interval;
            const int pattern[]{100,101,99,102,100,104,101,103,100,105,102,106,103,107,104,108,106,109,107,110};
            const int value=repeating?pattern[(200-i)%20]:100;
            if(binance)rows.append(QJsonArray{QString::number(at),QString::number(value),QString::number(value+2),QString::number(value-2),QString::number(value),"2",QString::number(at+interval-1),"200","2","1","100","0"});
            else rows.append(QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                {"timestamp",QString::number(at+1000)},{"unit","15"},{"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","100"},{"candle_acc_trade_volume","2"}});
        }HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();return result;
    }
    void deliver(int pageIndex){emit http.finished(http.tokens.last(),page(pageIndex));}
    bool loadPage(int pageIndex){
        clock+=1100;model.requestOlder();const auto expected=MarketCandles::historyRequest(identity,interval,end-pageIndex*200*interval).url;
        if(!QTest::qWaitFor([&]{return !http.requests.isEmpty()&&http.requests.last().url==expected;},2000))return false;
        deliver(pageIndex);QCoreApplication::processEvents();return true;
    }
    void tick(const QString& close=QStringLiteral("101")){
        clock+=10;QJsonObject wire;
        if(binance){const QJsonObject k{{"t",QString::number(end)},{"T",QString::number(end+interval-1)},{"s","BTCUSDT"},{"i","15m"},
            {"o","100"},{"h","110"},{"l","90"},{"c",close},{"v","2"},{"n","2"},{"x",false},{"q","202"},{"V","1"},{"Q","101"},{"B","0"}};
            wire={{"e","kline"},{"E",QString::number(clock)},{"s","BTCUSDT"},{"k",k}};
        }else wire={{"type","candle.15m"},{"code","KRW-BTC"},{"stream_type","REALTIME"},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(end,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},{"timestamp",QString::number(clock)},
            {"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price",close},{"candle_acc_trade_volume","2"}};
        emit socket->received(socket->generation,QJsonDocument(wire).toJson());QCoreApplication::processEvents();
    }
};
void navigationWheel(QWidget* chart,const QPoint& pixel,const QPoint& angle,Qt::ScrollPhase phase=Qt::NoScrollPhase,QPointF position={}){
    if(position.isNull())position=QPointF(chart->width()*.45,chart->height()*.4);
    QWheelEvent event(position,chart->mapToGlobal(position.toPoint()),pixel,angle,Qt::NoButton,Qt::NoModifier,phase,false);
    QApplication::sendEvent(chart,&event);
}
long double navigationPointerTime(QWidget* chart,qreal x,qint64 interval){
    return chart->property("viewportFirstMs").toLongLong()+
        (x-chart->property("viewportFirstBarX").toDouble())*chart->property("viewportSpanSlots").toDouble()/chart->property("viewportPlotWidth").toDouble()*interval;
}
}
class MarketCandlesViewTest:public QObject{
    Q_OBJECT
  private slots:
    void chartSyncNavigationSeparatesUserDataAndRemoteOrigins();
    void chartSyncCrosshairUsesTargetPricesAndClearsGaps();
    void dev86CoreStatesStayVisibleOutsideCandles_data();
    void dev86CoreStatesStayVisibleOutsideCandles();
    void dev86WhalesShowsAllObservationPhases_data();
    void dev86WhalesShowsAllObservationPhases();
    void horizontalWheelsCrossThousandBarHistoryWithoutZoom_data();
    void horizontalWheelsCrossThousandBarHistoryWithoutZoom();
    void drawingPlacesOnceThenMovesWholeShapes();
    void priceAxisDragChartStylesAndDrawingUndo();
    void chartContextMenuDeletesALine();
    void shiftWheelHomeEndAndDoubleClickNavigate();
    void axesShowRoundPricesCalendarTimesAndLatestPrice();
    void realPublicCandlesRenderForReview_data();
    void realPublicCandlesRenderForReview();
    void tinyPixelsAndMixedGesturesKeepTheirAxis();
    void longNoTradeGapRemainsHorizontallyNavigable();
    void historicalScaleSurvivesCompletedStudyInvalidation();
    void verticalWheelAndPinchKeepPointerTime();
    void prependingAndTicksKeepHistoricalAnchor_data();
    void prependingAndTicksKeepHistoricalAnchor();
    void wheelOutsidePricePlotIsNotConsumed();
    void cachedCandlesPaintWithoutBecomingFreshStudyOrShareInputs();
    void archiveHistoryShowsActualRefreshState_data();
    void archiveHistoryShowsActualRefreshState();
    void archiveStorageIssueHasAnIndependentRetryAction();
    void indicatorNamesDistinguishAppliedVisibilityAndReadiness();
    void chartStateIndicatorsPublishActualValuesAndHonorVisibility();
    void archiveAdvisoriesStayInInformation_data();
    void archiveAdvisoriesStayInInformation();
    void verifiedMarketStatesHaveSpecificShortMessages_data();
    void verifiedMarketStatesHaveSpecificShortMessages();
    void reconnectKeepsHistoricalCandlesButNotLiveStudies_data();
    void reconnectKeepsHistoricalCandlesButNotLiveStudies();
    void chartPricesUseGroupedDecimalsWithoutChangingInputs_data();
    void chartPricesUseGroupedDecimalsWithoutChangingInputs();
    void headerControlsStayBelowTheTitleAndFollowQtOwnership();
    void enabledLegendOverlaysChartWithoutWaitingOrDuplicateEvidence();
    void pointerOverlayMatchesFullRepaint();
    void pointerMoveCostWhenAsked();
    void overlayLegendConsumesNoCanvasHeight();
    void overlayLegendPassesWheelGesturesToChart();
    void compactLegendKeepsFourTargetsAndOverflowAccessible();
    void explicitLowerPanePreservesPriceAreaAndHiddenCalculation();
    void pickerUsesNameCellAndKeyboardWithFocusReturn();
    void indicatorControlsKeepFocusAcrossPublicRefresh();
    void formingUpdatesDoNotRelayoutUnchangedIndicatorControls();
    void accessibleToggleChangesTheDrawingOrStudyModel_data();
    void accessibleToggleChangesTheDrawingOrStudyModel();
    void indicatorHelpHasKeyboardLayersAndClearsAtContextBoundary_data();
    void indicatorHelpHasKeyboardLayersAndClearsAtContextBoundary();
    void pickerExplainsDisabledSimilarityWithoutEnablingIt();
    void historicalPathCarriesVisibleNonForecastLabel_data();
    void historicalPathCarriesVisibleNonForecastLabel();
    void chartNavigationUsesExistingGesturesWithoutFourButtons();
    void horizontalDrawingPricesGroupWithoutChangingSavedAnchors();
    void d3CandlesUseHollowRisesAndFilledFalls_data();
    void d3CandlesUseHollowRisesAndFilledFalls();
    void d3LegendAndToolsUseRowsAndLineIcons();
    void d3PullbackUsesAccentHatchingAndAverageUsesTextDashes();
    void d3EvidenceNumbersOnlyChangePresentation();
    void usabilityHidesSourceRowsAndKeepsTheirEvidenceInternal();
    void d3DetailsDisclosurePreservesTextAndShortStatus();
    void stockSelectionKeepsItsTypedContextVisibleWhileWaitingForSource();
    void stockSourceFailureDoesNotOfferAccountRegistration();
    void unconnectedStocksExplainBrokerRequirement_data();
    void unconnectedStocksExplainBrokerRequirement();
    void unconnectedStockClearsPreviouslyObservedCoinBodiesAndStudies_data();
    void unconnectedStockClearsPreviouslyObservedCoinBodiesAndStudies();
    void fullIdentitySelectionClearsPriorPrivateOverlayAndLabelsTimeframe();
    void nativeSeriesShowsVerifiedCurrencyAndSeparateSourceClock();
    void dailyCandlesKeepNativeBodiesFormingSeparationAndSourceClock();
    void completedStudySnapshotsIgnoreFormingOnlyUpdates();
    void completedStudiesYieldToCandlePaintAndKeepFormingLive();
    void lateCompletedStudiesCannotCrossContext_data();
    void lateCompletedStudiesCannotCrossContext();
    void destroyingViewDoesNotWaitForCompletedStudyWorker();
    void firstHiddenPublicStudyCompletionRemainsUsableOnShow();
    void completedStudyApplicationWaitsForItsFirstActualCandlePaint();
    void overlaysDrawObservedPathsCloudBoxAndVolume();
    void firstMeasuredCloudSampleIsVisible();
    void weakSimilarityKeepsReasonInternalAndNoFollowingPath();
    void historicalFollowingPathsDoNotFillWithTheLastCandleColor();
    void separateAccountsHaveDistinctVisibleLabelsAndExpire();
    void formingCandleDoesNotPreventPanningToOldestClosedBar();
    void providerPriceLinesStaySeparateAndRejectMissingOrForeignCurrency();
    void rendersOnlyVisibleSelectionAndClearsOnHide();
    void movementColorsRepaintObservedCandlesWithoutRefreshingSource_data();
    void movementColorsRepaintObservedCandlesWithoutRefreshingSource();
    void coinFlowRendersIndependentCompletedVolumeReversalMarkersAndPhysicalScorecard();
    void rawTradeBurstCoalescesOnlyTheChartPresentationAndCancelsOnSelection();
    void observedBinanceSourceRendersSeparatePopulationsCvdAndLossyLiquidations();
    void observedCvdWithoutSummaryFactsStillDrawsAndRespectsVisibility();
    void observedOiHistoryKeepsAdjacentEndsQuantityValueAndMissingSourceClock();
    void drawingToolbarUsesActualClosedSourceAndPublicModel();
    void drawingGeometryStaysAtOriginalUtcAndPriceAcrossZoomResizeAndTimeframes();
    void lateDrawingAnchorsStayVisibleAfterTimeframeChange_data();
    void lateDrawingAnchorsStayVisibleAfterTimeframeChange();
    void drawingToolsMagnetLockHideAndClearUseOnlyObservedOhlc();
    void privateDrawingLockRejectsLateSourceRefreshUntilExplicitSelection();
    void horizontalDrawingUiKeepsItsOwnerAndExplicitAlertInterval();
    void drawingGesturesRejectActualFormingAndUnobservedGapSlots();
    void drawingDraftAndHandleReleaseCannotCrossAContextGeneration();
    void hiddenShapesLeaveTheChartAndLockedShapesDoNotDrag();
    void unconnectedStockCannotCreateDrawingAnchors();
    void panningOlderDoesNotExtendTheViewportToOffscreenDrawingAnchors();
    void shareSnapshotUsesObservedViewportAndCompletedPrefix();
    void shareSnapshotDoesNotReadOrCaptureActualPrivatePriceLines();
    void shareSnapshotRejectsPriorSelectionAndKeepsItsCopiedImageImmutable();
    void sharedDrawingsRequirePublicOriginAndExplicitGeometryOptIn();
    void unconnectedStockCannotSharePreviouslyObservedCoinData();
    void autoDrawingButtonRequiresContextClosedUnlockedVisibleAndVerifiedStock();
    void autoDrawingButtonStartsPreviewWithoutChangingPublicState();
    void escapeDuringAutoPreviewFinishesAtOneThenCancelsOnSecondEscape();
    void autoPreviewApplyBatchesSingleUndoReplacesAndRespectsCheckboxes();
    void timeframeSwitchClearsAutoPreviewAndPreviewRowsImmediately();
    void closedCandleCorrectionClearsAutoPreviewWithoutNewBar();
    void objectsPanelEyeLockDeleteInPlaceUpdateAndFocusPreservation();
    void fibLabelsRespect18PxMinimumSpacingAndOffscreenAxisTags();
    void fibLabelsUsePriceAxisAndKeepPriceGrid_data();
    void fibLabelsUsePriceAxisAndKeepPriceGrid();
    void appliedDrawingsDoNotAlterAutoPriceScaleBounds();
    void lockedShapeHoverShowsArrowCursorInsteadOfOpenHand();
    void autoDrawingFailureShowsVisibleTransientFeedbackAndClearsOnSwitch();
    void lowTrendlineSweepMarkerDrawsBelowLowEvenWithLongerUpperWick();
    void similarityComparisonTablePaintsDirectlyHighlightsNavigatesAndCollapses();
    void controlModifierStrongMagnetSnapsWithin24PxWithoutFocusSteal();
    void strongMagnetUsesCurrentFrameAndCircularPixelRadius();
    void gappedAutoTrendUsesTheSameObservedPathForPreviewApplyAndHit();
    void autoPreviewHoverAndRenderProbeWhenAsked();
};
void MarketCandlesViewTest::dev86CoreStatesStayVisibleOutsideCandles_data(){
    QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");QTest::addColumn<QString>("scenario");
    for(bool dark:{false,true})for(int width:{640,1280})for(const auto* scenario:{"missing","none","signal"})
        QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark?"dark":"light").arg(width).arg(scenario)))<<dark<<width<<QString(scenario);
}
void MarketCandlesViewTest::dev86CoreStatesStayVisibleOutsideCandles(){
    QFETCH(bool,dark);QFETCH(int,width);QFETCH(QString,scenario);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    StudyTransport http;MarketCandles model(&http,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(&model);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(width,1100);view.show();expandAppliedIndicators(view);
    if(scenario!="missing"){
        auto rows=syntheticStudyRows(scenario=="signal"?200:60,scenario=="signal");
        if(scenario=="signal"){rows[190].low="90";rows[191].high="180";}
        else{
            const double pi=std::acos(-1.0);double past=100,query=100;
            for(int i=0;i<19;++i){past*=std::exp(.01*std::sin(2*pi*i/19));query*=std::exp(.01*std::cos(2*pi*i/19));rows[i+1].open=rows[i+1].close=QString::number(past,'f',12);rows[i+41].open=rows[i+41].close=QString::number(query,'f',12);}
            for(auto& row:rows){row.high=QString::number(row.close.toDouble()+50,'f',12);row.low=QString::number(row.close.toDouble()-50,'f',12);}
        }
        emit http.finished(http.token,syntheticStudyResponse(rows));QTRY_VERIFY(view.chartStudies().available);
    }
    auto* pullback=view.findChild<QLabel*>("chartCoreStudyState_pullback");auto* similarity=view.findChild<QLabel*>("chartCoreStudyState_similarity");auto* whales=view.findChild<QLabel*>("chartCoreStudyState_whales");
    QVERIFY(pullback&&similarity&&whales);QVERIFY(pullback->isVisible()&&similarity->isVisible()&&whales->isVisible());
    QVERIFY(pullback->text().startsWith(QStringLiteral("Pullback Zone")));QVERIFY(similarity->text().startsWith(QStringLiteral("Similarity")));QVERIFY(whales->text().startsWith(QStringLiteral("Whales & Reversal")));
    if(scenario=="missing"){QVERIFY(pullback->text().contains(QStringLiteral("자료 부족")));QVERIFY(similarity->text().contains(QStringLiteral("이력 부족")));}
    else if(scenario=="none"){QVERIFY2(pullback->text().contains(QStringLiteral("조건 없음")),qPrintable(pullback->text()));QVERIFY2(similarity->text().contains(QStringLiteral("일치 0건")),qPrintable(similarity->text()));}
    else{QVERIFY2(pullback->text().contains(QStringLiteral("유효 가격대")),qPrintable(pullback->text()));QVERIFY(similarity->text().contains(QStringLiteral("일치 3건")));}
    QVERIFY(whales->text().contains(QStringLiteral("체결 수집 중")));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");auto* panel=view.findChild<QWidget*>("chartCoreStudySummary");QVERIFY(chart&&panel);
    QCoreApplication::processEvents();
    const QRect candleRect(chart->mapTo(&view,QPoint()),chart->size());
    for(auto* label:{pullback,similarity,whales}){
        const QRect bounds(label->mapTo(&view,QPoint()),label->size());
        QVERIFY2(!bounds.intersects(candleRect),"Persistent study summaries must consume layout space, never cover the candles");
        QVERIFY(bounds.left()>=0);QVERIFY(bounds.right()<view.width());QVERIFY(label->height()>=label->fontMetrics().height());
    }
    QTRY_VERIFY2_WITH_TIMEOUT(coreStudyLayoutProblem(view,width).isEmpty(),qPrintable(coreStudyLayoutProblem(view,width)),1000);
    // Exercise the same labels after the responsive grid has changed columns;
    // a correct initial size must not conceal stale wrapped heights on resize.
    if(scenario=="signal"){
        const int otherWidth=width==640?1280:640;
        const auto beforeOtherWidth=coreStudyWidthConstraints(view);
        view.resize(otherWidth,1100);
        QTRY_VERIFY2_WITH_TIMEOUT(coreStudyLayoutProblem(view,otherWidth).isEmpty(),qPrintable(coreStudyLayoutProblem(view,otherWidth)+QStringLiteral("; before resize: ")+beforeOtherWidth),1000);
        const auto beforeOriginalWidth=coreStudyWidthConstraints(view);
        view.resize(width,1100);
        QTRY_VERIFY2_WITH_TIMEOUT(coreStudyLayoutProblem(view,width).isEmpty(),qPrintable(coreStudyLayoutProblem(view,width)+QStringLiteral("; before resize: ")+beforeOriginalWidth),1000);
    }
    QVERIFY2(saveDev86IndicatorCapture(view,QString("fixture-core-%1-%2-%3").arg(scenario,dark?"dark":"light").arg(width)),"Dev86 fixture capture must be saved when requested");
    if(scenario=="signal"){
        const auto comparison=view.chartStudies().similarity;
        auto* detail=view.findChild<QLabel*>("chartCoreStudyDetail_similarity");QVERIFY(detail);
        QVERIFY(detail->text().contains(QString("match:%1").arg(comparison.paths.first().fromMs)));
        const auto reference=comparison.referenceThroughMs;
        QVERIFY(QMetaObject::invokeMethod(detail,"linkActivated",Qt::DirectConnection,Q_ARG(QString,QString("match:%1").arg(comparison.paths.first().fromMs))));
        QCOMPARE(view.chartStudies().similarity.referenceThroughMs,reference);
        QVERIFY(view.visibleViewport().fromMs<=comparison.paths.first().fromMs&&view.visibleViewport().toMs>=comparison.paths.first().fromMs);
        QVERIFY(detail->text().contains(QStringLiteral("기준으로 돌아가기")));QVERIFY(detail->text().contains(QStringLiteral("화면 밖")));
        QTRY_VERIFY2_WITH_TIMEOUT(coreStudyLayoutProblem(view,width).isEmpty(),qPrintable(coreStudyLayoutProblem(view,width)),1000);
        QTextDocument links;links.setHtml(detail->text());QString returnLink;
        for(auto block=links.begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){const auto href=it.fragment().charFormat().anchorHref();if(href.startsWith("reference:"))returnLink=href;}
        QCOMPARE(returnLink,QString("reference:%1").arg(model.series().candles.last().startsAtMs));
        QVERIFY(QMetaObject::invokeMethod(detail,"linkActivated",Qt::DirectConnection,Q_ARG(QString,returnLink)));
        QCOMPARE(view.chartStudies().similarity.referenceThroughMs,reference);QVERIFY(view.visibleViewport().toMs>=model.series().candles.last().startsAtMs);
        QTRY_VERIFY2_WITH_TIMEOUT(coreStudyLayoutProblem(view,width).isEmpty(),qPrintable(coreStudyLayoutProblem(view,width)),1000);
    }
    preferences.setEnabled("pullback",false);preferences.setVisible("similarity",false);QCoreApplication::processEvents();
    QVERIFY(pullback->isVisible());QVERIFY(pullback->text().contains(QStringLiteral("꺼짐")));
    QVERIFY(similarity->isVisible());QVERIFY(similarity->text().contains(QStringLiteral("숨김")));
    ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(preferences.publicState()));view.setIndicatorPreferences(&restored);
    QVERIFY(!restored.enabled("pullback"));QVERIFY(restored.enabled("similarity"));QVERIFY(!restored.visible("similarity"));
}
void MarketCandlesViewTest::dev86WhalesShowsAllObservationPhases_data(){
    QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");QTest::addColumn<QString>("scenario");
    for(bool dark:{false,true})for(int width:{640,1280})for(const auto* scenario:{"collecting","none","signal"})QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark?"dark":"light").arg(width).arg(scenario)))<<dark<<width<<QString(scenario);
}
void MarketCandlesViewTest::dev86WhalesShowsAllObservationPhases(){
    QFETCH(bool,dark);QFETCH(int,width);QFETCH(QString,scenario);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    const auto end=QDateTime::currentMSecsSinceEpoch()/60000*60000;const auto minute=end-60000;qint64 now=end-180000;
    class FlowHttp final:public TradingTransport{public:QList<RequestSpec> requests;QList<quint64> tokens;void abort()override{}void send(quint64 token,const RequestSpec& request)override{requests.append(request);tokens.append(token);}} http;
    auto socket=std::make_unique<StudySocket>();auto* wire=socket.get();CoinFlowSource flow(&http,std::move(socket),{},nullptr,[&]{return now;});
    CoinFlowInstrument instrument;instrument.venue=CoinFlowVenue::BinanceSpot;instrument.grain=CoinFlowGrain::SpotAggregate;instrument.identity="coin:binance:SYNUSDT";instrument.symbol="SYNUSDT";instrument.baseAsset="SYN";instrument.quoteAsset="USDT";instrument.status="TRADING";instrument.verified=true;
    instrument.evidence={"합성 공개 목록","https://api.binance.com/api/v3/exchangeInfo",0,now,now};flow.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument});flow.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{});flow.setSelection(instrument.identity);flow.setActive(true);emit wire->connected(wire->generation);
    int handled=0;for(int step=0;step<12;++step){if(handled<http.requests.size()){HttpResult response;response.status=200;response.body="[]";emit http.finished(http.tokens[handled++],response);}now+=500;flow.pump();}
    now=minute;flow.pump();MarketCandleSeries seed;seed.available=true;seed.identity=instrument.identity;seed.asset="SYN";seed.currency="USDT";seed.intervalMs=60000;seed.sourceUrl=MarketCandles::request(seed.identity,60000).url.toString();seed.receivedAtMs=now;
    for(int i=21;i>0;--i)seed.candles.append({minute-i*60000,0,"100","101","99","100","10"});flow.setMinuteSeries(seed);
    if(scenario!="collecting")for(int i=0;i<202;++i){const auto at=minute+1000+i*200;now=at+2;const QJsonObject trade{{"e","aggTrade"},{"E",QString::number(at+1)},{"s","SYNUSDT"},{"a",QString::number(1000+i)},{"p","100"},{"q","1"},{"f",QString::number(2000+i)},{"l",QString::number(2000+i)},{"T",QString::number(at)},{"m",bool(i%2)}};emit wire->received(wire->generation,QJsonDocument(trade).toJson(QJsonDocument::Compact));}
    now=end;flow.pump();auto completed=seed;completed.receivedAtMs=now;completed.candles.append({minute,0,"100","101","99","100",scenario=="signal"?"40":"10"});flow.setMinuteSeries(completed);
    StudyTransport chartHttp;MarketCandles model(&chartHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});MarketInstrument chartInstrument;chartInstrument.symbol="SYNUSDT";chartInstrument.baseAsset="SYN";chartInstrument.quoteAsset="USDT";chartInstrument.market="spot";chartInstrument.status="TRADING";chartInstrument.tradable=true;model.setInstruments("binance",{chartInstrument});
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(&model);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setIndicatorPreferences(&preferences);view.setCoinFlowSource(&flow);view.setSelection(instrument.identity,60000);view.resize(width,1100);view.show();expandAppliedIndicators(view);
    QJsonArray candles;for(const auto& row:completed.candles)candles.append(QJsonArray{row.startsAtMs,row.open,row.high,row.low,row.close,row.volume,row.startsAtMs+59999,"1000",10,"5","500","0"});HttpResult response;response.status=200;response.body=QJsonDocument(candles).toJson();emit chartHttp.finished(chartHttp.token,response);
    QTRY_VERIFY(view.coinFlowStudies().volume.state.available);auto* label=view.findChild<QLabel*>("chartCoreStudyState_whales");QVERIFY(label);
    const auto expected=scenario=="collecting"?QStringLiteral("체결 수집 중"):scenario=="none"?QStringLiteral("관측 완료 · 신호 없음"):QStringLiteral("신호 발생");
    QTRY_VERIFY2(label->text().contains(expected),qPrintable(label->text()+" / "+coinFlowDisplayState(view.coinFlowStudies()).reason));
    auto* detail=view.findChild<QLabel*>("chartCoreStudyDetail_whales");QVERIFY(detail);QVERIFY(detail->text().contains(QStringLiteral("체결 미관측")));
    if(scenario=="signal"){const auto state=coinFlowDisplayState(view.coinFlowStudies());QCOMPARE(state.latestSignalAtMs,end);QVERIFY(detail->text().contains(QStringLiteral("최근 신호로 이동")));QVERIFY(!view.coinFlowStudies().groups.isEmpty());}
    else QVERIFY(!detail->text().contains(QStringLiteral("최근 신호로 이동")));
    QTRY_VERIFY2_WITH_TIMEOUT(coreStudyLayoutProblem(view,width).isEmpty(),qPrintable(coreStudyLayoutProblem(view,width)),1000);
    QVERIFY(saveDev86IndicatorCapture(view,QString("fixture-whales-%1-%2-%3").arg(scenario,dark?"dark":"light").arg(width)));
    if(scenario=="signal"){
        auto* chart=view.findChild<QWidget*>("marketCandlesChart");const auto before=chart->grab().toImage();preferences.setVisible("whales",false);QCoreApplication::processEvents();QVERIFY(before!=chart->grab().toImage());
    }
}
void MarketCandlesViewTest::horizontalWheelsCrossThousandBarHistoryWithoutZoom_data(){
    QTest::addColumn<bool>("binance");QTest::newRow("upbit-btc-15m-1000")<<false;QTest::newRow("binance-btc-15m-1000")<<true;
}
void MarketCandlesViewTest::horizontalWheelsCrossThousandBarHistoryWithoutZoom(){
    QFETCH(bool,binance);NavigationFixture f(binance);QVERIFY(f.chart());
    for(int page=1;page<5;++page)QVERIFY(f.loadPage(page));
    QCOMPARE(f.model.displayObservation().candles.size(),1000);QVERIFY(f.view.navigateToTime(f.end-500*f.interval));
    auto* chart=f.chart();const auto original=f.view.visibleViewport();const auto zoom=chart->property("viewportZoomBars").toInt();
    const auto scale=chart->property("viewportSpanSlots").toDouble();
    for(int i=0;i<10;++i){const auto before=f.view.visibleViewport();navigationWheel(chart,{},QPoint(120,0));
        QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);QCOMPARE(chart->property("viewportSpanSlots").toDouble(),scale);
        QCOMPARE(f.view.visibleViewport().fromMs,before.fromMs-10*f.interval);QCOMPARE(f.view.visibleViewport().toMs,before.toMs-10*f.interval);}
    for(int i=0;i<10;++i){const auto before=f.view.visibleViewport();navigationWheel(chart,{},QPoint(-120,0));
        QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);QCOMPARE(chart->property("viewportSpanSlots").toDouble(),scale);
        QCOMPARE(f.view.visibleViewport().fromMs,before.fromMs+10*f.interval);QCOMPARE(f.view.visibleViewport().toMs,before.toMs+10*f.interval);}
    QCOMPARE(f.view.visibleViewport().fromMs,original.fromMs);QCOMPARE(f.view.visibleViewport().toMs,original.toMs);
    // Repeat across the 200/400/600/800 boundaries, keeping one pixel scale.
    for(const int distance:{100,300,500,700,900}){QVERIFY(f.view.navigateToTime(f.end-distance*f.interval));
        QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);QCOMPARE(chart->property("viewportSpanSlots").toDouble(),scale);}
    const auto shared=f.view.shareSnapshot({},f.clock);QCOMPARE(shared.availability,ChartShareAvailability::Ready);
    QVERIFY(shared.closed.size()<=200);QCOMPARE(shared.viewport.toMs,f.view.visibleViewport().toMs);
    QCOMPARE(shared.calculatedThroughMs,shared.closed.last().startsAtMs+f.interval);QVERIFY(shared.calculatedThroughMs<=f.clock);
    const auto capture=qEnvironmentVariable("KEEFI_DEV86_CAPTURE_DIR");if(!capture.isEmpty()){QVERIFY(QDir().mkpath(capture));QVERIFY(f.view.grab().save(capture+"/fixture-navigation-"+(binance?"binance":"upbit")+"-1000.png"));}
}
namespace {
struct DrawingFixture {
    StudyTransport transport;MarketCandles source{&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;}};ChartDrawings drawings;ChartStudyPreferences preferences;MarketCandlesView view{&source};
    DrawingFixture(){
        preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
        view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,1000);view.show();
        emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));
    }
    QWidget* chart()const{return view.findChild<QWidget*>("marketCandlesChart");}
    QToolButton* tool(const char* name)const{return view.findChild<QToolButton*>(name);}
    // Selecting a horizontal line opens its panel and resizes the chart; read the live price mapping.
    int priceY(const QString& price)const{auto* c=chart();QCoreApplication::processEvents();
        return qRound(c->property("viewportPlotTop").toDouble()+(c->property("viewportPriceHigh").toDouble()-price.toDouble())/
            (c->property("viewportPriceHigh").toDouble()-c->property("viewportPriceLow").toDouble())*c->property("viewportPlotHeight").toDouble());}
};
}
void MarketCandlesViewTest::drawingPlacesOnceThenMovesWholeShapes(){
    DrawingFixture f;QVERIFY(f.source.series().available);auto* chart=f.chart();QVERIFY(chart);const auto centers=visibleBodyCenters(chart);QCOMPARE(centers.size(),6);
    // One placement per tool choice: the chart returns to the cursor so the next press selects or pans.
    QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[0]);
    QCOMPARE(f.drawings.drawings().size(),1);QVERIFY(f.tool("chartDrawingTool_crosshair")->isChecked());QVERIFY(!f.tool("chartDrawingTool_horizontal")->isChecked());
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[5]);QCOMPARE(f.drawings.drawings().size(),1);
    // Shift keeps the tool for repeated placements, as in common chart tools.
    QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::ShiftModifier,centers[5]+QPoint(0,-30));
    QCOMPARE(f.drawings.drawings().size(),2);QVERIFY(f.tool("chartDrawingTool_horizontal")->isChecked());
    QTest::keyClick(chart,Qt::Key_Escape);QVERIFY(f.tool("chartDrawingTool_crosshair")->isChecked());QVERIFY(f.drawings.clear());
    // A trend line placed with two clicks, then dragged by its body: both anchors move together.
    QTest::mouseClick(f.tool("chartDrawingTool_trend"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[1]);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[3]);
    QCOMPARE(f.drawings.drawings().size(),1);QVERIFY(f.tool("chartDrawingTool_crosshair")->isChecked());
    const auto trend=f.drawings.drawings().first();const auto bar=centers[3].x()-centers[2].x();QVERIFY(bar>4);
    const QPoint grab((centers[1].x()+centers[3].x())/2,centers[1].y());const QPoint drop=grab+QPoint(bar,-40);
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,grab);QTest::mouseMove(chart,grab+QPoint(bar/2,-20));QTest::mouseMove(chart,drop);QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,drop);
    auto moved=f.drawings.drawings().first();QCOMPARE(moved.id,trend.id);QCOMPARE(moved.anchors.size(),2);
    QCOMPARE(moved.anchors[0].atMs,trend.anchors[0].atMs+60000);QCOMPARE(moved.anchors[1].atMs,trend.anchors[1].atMs+60000);
    QVERIFY(moved.anchors[0].price.toDouble()>trend.anchors[0].price.toDouble());QCOMPARE(moved.anchors[0].price,moved.anchors[1].price);
    // A horizontal line dragged anywhere along its width moves its price only, rounded to the pixel.
    QVERIFY(f.drawings.clear());QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[2]);
    const auto line=f.drawings.drawings().first();const QPoint between((centers[4].x()+centers[5].x())/2,f.priceY(line.anchors.first().price));
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,between);QTest::mouseMove(chart,between+QPoint(0,30));QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,between+QPoint(0,30));
    const auto lowered=f.drawings.drawings().first();QCOMPARE(lowered.anchors.first().atMs,line.anchors.first().atMs);
    QVERIFY(lowered.anchors.first().price.toDouble()<line.anchors.first().price.toDouble());QVERIFY(lowered.anchors.first().price.size()<=8);
    // A plain drag on empty space still pans; it never moves a line.
    const auto before=f.drawings.publicState();const QPoint empty(centers[3].x(),centers[3].y()+150);
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,empty);QTest::mouseMove(chart,empty+QPoint(-40,0));QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,empty+QPoint(-40,0));
    QCOMPARE(f.drawings.publicState(),before);
}
// User requests (2026-11-05): drag the price axis to change the vertical scale, switch between
// candle, bar and line charts, and undo drawing.
void MarketCandlesViewTest::priceAxisDragChartStylesAndDrawingUndo(){
    DrawingFixture f;auto* chart=f.chart();QVERIFY(chart);QCoreApplication::processEvents();
    const auto range=[&]{return chart->property("viewportPriceHigh").toDouble()-chart->property("viewportPriceLow").toDouble();};
    const auto automatic=range();QVERIFY(automatic>0);QCOMPARE(f.view.priceScale(),1.0);
    const int top=qRound(chart->property("viewportPlotTop").toDouble()),height=qRound(chart->property("viewportPlotHeight").toDouble());
    const QPoint axis(chart->width()-20,top+height/2);
    // Even with a drawing tool armed, a press on the axis scales; it never draws or pans.
    QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,axis);QTest::mouseMove(chart,axis+QPoint(0,60));QTest::mouseMove(chart,axis+QPoint(0,120));
    QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,axis+QPoint(0,120));
    QVERIFY2(f.view.priceScale()>1.5,qPrintable(QString::number(f.view.priceScale())));QVERIFY(range()>automatic*1.5);
    QVERIFY(f.drawings.drawings().isEmpty());QCOMPARE(chart->property("viewportOffsetSlots").toDouble(),0.0);
    QTest::keyClick(chart,Qt::Key_Escape);
    // Dragging up narrows the range below the automatic one.
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,axis);QTest::mouseMove(chart,axis+QPoint(0,-120));QTest::mouseMove(chart,axis+QPoint(0,-240));
    QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,axis+QPoint(0,-240));
    QVERIFY(f.view.priceScale()<1);QVERIFY(range()<automatic);
    // Double-click on the axis returns to the automatic range.
    QTest::mouseDClick(chart,Qt::LeftButton,Qt::NoModifier,axis);QCOMPARE(f.view.priceScale(),1.0);QVERIFY(qAbs(range()-automatic)<1e-9);
    // Chart styles: candles (default), OHLC bars, close line. The choice is kept in the chart preferences.
    auto* style=f.view.findChild<QToolButton*>("chartStyleButton");QVERIFY(style);QVERIFY(style->isVisible());QVERIFY(style->accessibleName().contains(QStringLiteral("캔들")));
    QCOMPARE(visibleBodyCenters(chart).size(),6);const auto candles=chart->grab().toImage();
    f.view.setChartStyle("bars");QCOMPARE(f.preferences.chartStyle(),QString("bars"));const auto bars=chart->grab().toImage();QVERIFY(bars!=candles);
    QVERIFY(style->accessibleName().contains(QStringLiteral("바")));
    f.view.setChartStyle("line");QCOMPARE(f.view.chartStyle(),QString("line"));const auto line=chart->grab().toImage();QVERIFY(line!=bars&&line!=candles);
    QVERIFY(visibleBodyCenters(chart).isEmpty()); // No candle bodies in the line chart.
    f.preferences.setChartStyle("candles");QCoreApplication::processEvents();QCOMPARE(visibleBodyCenters(chart).size(),6);QVERIFY(style->accessibleName().contains(QStringLiteral("캔들")));
    // Drawing undo: toolbar button and ⌘Z / ⇧⌘Z.
    const auto centers=visibleBodyCenters(chart);auto* undo=f.tool("chartDrawingUndo");QVERIFY(undo);QVERIFY(undo->isVisible());QVERIFY(!undo->isEnabled());
    QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[2]);QCOMPARE(f.drawings.drawings().size(),1);
    QVERIFY(undo->isEnabled());chart->setFocus();
    QTest::keyClick(chart,Qt::Key_Z,Qt::ControlModifier);QTRY_VERIFY(f.drawings.drawings().isEmpty());QVERIFY(!undo->isEnabled());
    QTest::keyClick(chart,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);QTRY_COMPARE(f.drawings.drawings().size(),1);
    QTest::mouseClick(undo,Qt::LeftButton);QVERIFY(f.drawings.drawings().isEmpty());
    // An unfinished two-point line is cancelled first, without touching earlier edits.
    QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[1]);QCOMPARE(f.drawings.drawings().size(),1);
    QTest::mouseClick(f.tool("chartDrawingTool_trend"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[2]);chart->setFocus();
    QTest::keyClick(chart,Qt::Key_Z,Qt::ControlModifier);QCOMPARE(f.drawings.drawings().size(),1);
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[4]);QCOMPARE(f.drawings.drawings().size(),1); // A new first point, not a finished line.
}
void MarketCandlesViewTest::chartContextMenuDeletesALine(){
    DrawingFixture f;auto* chart=f.chart();const auto centers=visibleBodyCenters(chart);QCOMPARE(centers.size(),6);
    QTest::mouseClick(f.tool("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[2]);QCOMPARE(f.drawings.drawings().size(),1);
    QStringList shown;
    // Poll until the menu is up: selecting the line may run queued events before the menu opens.
    QTimer poll;int attempts=0;poll.setInterval(10);
    QObject::connect(&poll,&QTimer::timeout,chart,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu){if(++attempts>300)poll.stop();return;}
        poll.stop();for(auto* action:menu->actions())shown<<action->text();if(auto* remove=menu->findChild<QAction*>("marketCandlesChartMenuDelete"))remove->trigger();menu->close();});
    poll.start();
    const QPoint onLine(centers[4].x(),f.priceY(f.drawings.drawings().first().anchors.first().price));
    QContextMenuEvent event(QContextMenuEvent::Mouse,onLine,chart->mapToGlobal(onLine));QApplication::sendEvent(chart,&event);
    QVERIFY2(shown.contains(QStringLiteral("이 선 삭제")),qPrintable(shown.join('|')));QVERIFY(shown.contains(QStringLiteral("실시간으로")));QVERIFY(f.drawings.drawings().isEmpty());
}
void MarketCandlesViewTest::shiftWheelHomeEndAndDoubleClickNavigate(){
    NavigationFixture f;auto* chart=f.chart();QVERIFY(f.loadPage(1));QVERIFY(f.view.navigateToTime(f.end-100*f.interval));
    const auto zoom=chart->property("viewportZoomBars").toInt();const auto offset=chart->property("viewportOffsetSlots").toDouble();
    // A mouse without a horizontal wheel: Shift + vertical wheel moves through time without zooming.
    const QPointF at(chart->width()*.45,chart->height()*.4);
    QWheelEvent shifted(at,chart->mapToGlobal(at.toPoint()),{},QPoint(0,120),Qt::NoButton,Qt::ShiftModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(chart,&shifted);
    QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);QVERIFY(chart->property("viewportOffsetSlots").toDouble()>offset);
    chart->setFocus();QTest::keyClick(chart,Qt::Key_End);QCOMPARE(chart->property("viewportOffsetSlots").toDouble(),0.0);QVERIFY(chart->property("viewportLive").toBool());
    QTest::keyClick(chart,Qt::Key_Home);QVERIFY(chart->property("viewportOffsetSlots").toDouble()>300);
    QCOMPARE(f.view.visibleViewport().fromMs,f.end-400*f.interval);
    // Double-click on empty chart space restores the default zoom at the latest bar.
    QTest::keyClick(chart,Qt::Key_Plus);QTest::keyClick(chart,Qt::Key_Plus);QVERIFY(chart->property("viewportZoomBars").toInt()<60);
    QTest::mouseDClick(chart,Qt::LeftButton,Qt::NoModifier,at.toPoint());
    QCOMPARE(chart->property("viewportZoomBars").toInt(),60);QCOMPARE(chart->property("viewportOffsetSlots").toDouble(),0.0);
}
void MarketCandlesViewTest::axesShowRoundPricesCalendarTimesAndLatestPrice(){
    NavigationFixture f(false,true);auto* chart=f.chart();CandleTextDevice device;chart->render(&device);QStringList painted;for(const auto& run:device.engine.runs)painted<<run.text;
    // Several round-number price labels (pattern 99..110 → steps of 1 or 2) and KST clock labels.
    int prices=0,times=0;static const QRegularExpression clock("\\A[0-2][0-9]:[0-5][0-9]\\z"),date("\\A[01][0-9]/[0-3][0-9]\\z");
    for(const auto& text:painted){bool ok=false;const auto value=QString(text).remove(',').toDouble(&ok);if(ok&&value>=90&&value<=120&&!text.contains('.'))++prices;if(clock.match(text).hasMatch()||date.match(text).hasMatch())++times;}
    QVERIFY2(prices>=4,qPrintable(painted.join('|')));QVERIFY2(times>=3,qPrintable(painted.join('|')));QVERIFY(painted.contains("KST"));
    // The OHLC legend reads in plain Korean with the latest candle when nothing is hovered.
    for(const auto* label:{"시","고","저","종","거래량"})QVERIFY2(painted.contains(QString::fromUtf8(label)),label);
    // The latest price owns a filled axis label at its exact price coordinate.
    const auto image=chart->grab().toImage();const auto scale=image.devicePixelRatio();
    const auto top=chart->property("viewportPlotTop").toDouble(),plotHeight=chart->property("viewportPlotHeight").toDouble();
    const auto high=chart->property("viewportPriceHigh").toDouble(),low=chart->property("viewportPriceLow").toDouble();
    const auto latestY=top+(high-f.model.displayObservation().candles.last().close.toDouble())/(high-low)*plotHeight;
    const auto right=chart->property("viewportPlotLeft").toDouble()+chart->property("viewportPlotWidth").toDouble();
    int filled=0,total=0;for(int y=qCeil((latestY-5)*scale);y<=qFloor((latestY+5)*scale);++y)for(int x=qCeil((right+4)*scale);x<qFloor((right+40)*scale);++x){++total;if(image.pixelColor(x,y)!=themeColors().bg)++filled;}
    QVERIFY2(total>0&&filled>total*.6,"The latest price must be shown in a filled axis box");
}
void MarketCandlesViewTest::realPublicCandlesRenderForReview_data(){
    QTest::addColumn<QString>("file");QTest::addColumn<bool>("binance");QTest::addColumn<bool>("dark");
    QTest::newRow("upbit-btc-dark")<<"upbit-btc-15m.json"<<false<<true;QTest::newRow("upbit-btc-light")<<"upbit-btc-15m.json"<<false<<false;
    QTest::newRow("binance-btc-dark")<<"binance-btc-15m.json"<<true<<true;
}
void MarketCandlesViewTest::realPublicCandlesRenderForReview(){
    // Opt-in review capture: actual public responses saved beforehand, never fetched here.
    const auto directory=qEnvironmentVariable("KEEFI_CHART_REAL_DIR");if(directory.isEmpty())QSKIP("Opt-in: KEEFI_CHART_REAL_DIR holds saved public candle responses");
    QFETCH(QString,file);QFETCH(bool,binance);QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    QFile input(QDir(directory).filePath(file));QVERIFY(input.open(QIODevice::ReadOnly));HttpResult response;response.status=200;response.body=input.readAll();
    NavigationHttp http;MarketCandles model(&http);
    if(binance){MarketInstrument item;item.symbol="BTCUSDT";item.baseAsset="BTC";item.quoteAsset="USDT";item.market="spot";item.status="TRADING";item.tradable=true;model.setInstruments("binance",{item});}
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(&model);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setIndicatorPreferences(&preferences);
    ChartDrawings drawings;view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);
    view.setSelection(binance?"coin:binance:BTCUSDT":"coin:upbit:KRW-BTC",900000);view.resize(1280,1000);view.show();
    QTRY_VERIFY(!http.tokens.isEmpty());emit http.finished(http.tokens.last(),response);
    QTRY_VERIFY(model.series().available);QTRY_VERIFY(view.chartStudies().available);QTest::qWait(400);
    const auto& studies=view.chartStudies();qDebug()<<"real"<<QTest::currentDataTag()<<"cloud"<<studies.cloud.size()<<"pullback"<<studies.pullback.available<<studies.pullback.reason<<"similarity"<<studies.similarity.available;
    const auto out=qEnvironmentVariable("KEEFI_CHART_REAL_OUT",directory);QVERIFY(QDir().mkpath(out));
    QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+".png")));
    if(binance||!dark)return;
    // Crosshair: price and time boxes on the axes, OHLC legend for the hovered candle.
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const QPoint hover(int(chart->width()*.55),int(chart->height()*.45));
    QMouseEvent hoverMove(QEvent::MouseMove,hover,chart->mapToGlobal(hover),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(chart,&hoverMove);QTRY_VERIFY(!chart->property("hoverSummary").toString().isEmpty());
    QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-hover.png")));
    // One horizontal line and one trend line; the tool returns to the cursor after each.
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,QPoint(int(chart->width()*.5),int(chart->height()*.3)));
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_trend"),Qt::LeftButton);
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,QPoint(int(chart->width()*.25),int(chart->height()*.75)));QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,QPoint(int(chart->width()*.6),int(chart->height()*.4)));
    QCOMPARE(drawings.drawings().size(),2);QVERIFY(view.findChild<QToolButton*>("chartDrawingTool_crosshair")->isChecked());
    QEvent leave(QEvent::Leave);QApplication::sendEvent(chart,&leave);
    QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-drawn.png")));
    const auto before=drawings.drawings().last().anchors;const QPoint grab(int(chart->width()*.425),int(chart->height()*.575));const QPoint drop=grab+QPoint(90,-80);
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,grab);QTest::mouseMove(chart,grab+QPoint(45,-40));QTest::mouseMove(chart,drop);QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,drop);
    QVERIFY2(drawings.drawings().last().anchors!=before,"Dragging the trend line body must move the line");QApplication::sendEvent(chart,&leave);
    qDebug()<<"moved trend"<<before.first().atMs<<before.first().price<<"->"<<drawings.drawings().last().anchors.first().atMs<<drawings.drawings().last().anchors.first().price;
    QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-moved.png")));
    // ⌘Z undoes the move; the line returns to where it was drawn.
    chart->setFocus();QTest::keyClick(chart,Qt::Key_Z,Qt::ControlModifier);QTRY_COMPARE(drawings.drawings().last().anchors,before);QApplication::sendEvent(chart,&leave);
    QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-undo.png")));
    QVERIFY(drawings.clear());
    // Price axis dragged down: a wider range, so the same candles are shorter.
    const int top=qRound(chart->property("viewportPlotTop").toDouble()),height=qRound(chart->property("viewportPlotHeight").toDouble());const QPoint axis(chart->width()-20,top+height/2);
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,axis);QTest::mouseMove(chart,axis+QPoint(0,70));QTest::mouseMove(chart,axis+QPoint(0,140));QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,axis+QPoint(0,140));
    QVERIFY(view.priceScale()>1.5);QApplication::sendEvent(chart,&leave);
    QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-axis.png")));
    QTest::mouseDClick(chart,Qt::LeftButton,Qt::NoModifier,axis);QCOMPARE(view.priceScale(),1.0);
    for(const auto* style:{"bars","line"}){view.setChartStyle(style);QApplication::sendEvent(chart,&leave);
        QVERIFY(view.grab().save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-"+style+".png")));}
    view.setChartStyle("candles");
}
void MarketCandlesViewTest::historicalScaleSurvivesCompletedStudyInvalidation(){
    NavigationFixture f(true,true);f.preferences.setEnabled("similarity",true);
    QTRY_VERIFY(f.view.chartStudies().similarity.available);auto* chart=f.chart();
    QVERIFY(f.view.navigateToTime(f.end-100*f.interval));const auto range=f.view.visibleViewport();
    const auto span=chart->property("viewportSpanSlots").toDouble(),x=chart->property("viewportFirstBarX").toDouble();QVERIFY(span>60);
    f.preferences.setVisible("similarity",false);QCOMPARE(chart->property("viewportSpanSlots").toDouble(),span);
    QVERIFY(std::abs(chart->property("viewportFirstBarX").toDouble()-x)<.01);f.preferences.setVisible("similarity",true);
    // An authoritative Binance completed candle starts an asynchronous study
    // generation (old projections clear then reappear). Historical pixels stay.
    f.clock=f.end+f.interval+10000;
    const QJsonObject k{{"t",QString::number(f.end)},{"T",QString::number(f.end+f.interval-1)},{"s","BTCUSDT"},{"i","15m"},
        {"o","100"},{"h","102"},{"l","98"},{"c","100"},{"v","2"},{"n","2"},{"x",true},{"q","200"},{"V","1"},{"Q","100"},{"B","0"}};
    emit f.socket->received(f.socket->generation,QJsonDocument(QJsonObject{{"e","kline"},{"E",QString::number(f.clock)},{"s","BTCUSDT"},{"k",k}}).toJson());
    QCOMPARE(f.model.series().candles.last().startsAtMs,f.end);
    QCOMPARE(chart->property("viewportSpanSlots").toDouble(),span);QCOMPARE(f.view.visibleViewport().fromMs,range.fromMs);
    QVERIFY(std::abs(chart->property("viewportFirstBarX").toDouble()-x)<.01);
    QTRY_VERIFY(f.view.chartStudies().similarity.available);
    QCOMPARE(chart->property("viewportSpanSlots").toDouble(),span);QCOMPARE(f.view.visibleViewport().fromMs,range.fromMs);
    QVERIFY(std::abs(chart->property("viewportFirstBarX").toDouble()-x)<.01);
}
void MarketCandlesViewTest::longNoTradeGapRemainsHorizontallyNavigable(){
    NavigationFixture f;auto* chart=f.chart();f.clock+=1100;f.model.requestOlder();
    QVERIFY(QTest::qWaitFor([&]{return f.http.requests.size()>=2;},2000));
    // Actual older observations have a 400-interval no-trade gap. No zero bars.
    emit f.http.finished(f.http.tokens.last(),f.page(3));QCOMPARE(f.model.displayObservation().candles.size(),400);
    const auto zoom=chart->property("viewportZoomBars").toInt();
    for(int i=0;i<40;++i)navigationWheel(chart,{},QPoint(120,0));
    QCOMPARE(f.view.visibleViewport().fromMs,qint64(0));QCOMPARE(chart->property("viewportOffsetSlots").toDouble(),400.0);
    QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);
    for(int i=0;i<30;++i)navigationWheel(chart,{},QPoint(120,0));
    QVERIFY(f.view.visibleViewport().fromMs>0);QVERIFY(f.view.visibleViewport().toMs<f.end-600*f.interval);
    QCOMPARE(chart->property("viewportOffsetSlots").toDouble(),700.0);
    QCOMPARE(f.model.displayObservation().candles.size(),400);
    for(int i=0;i<70;++i)navigationWheel(chart,{},QPoint(-120,0));
    QCOMPARE(chart->property("viewportOffsetSlots").toDouble(),0.0);QCOMPARE(f.view.visibleViewport().toMs,f.end-f.interval);
}
void MarketCandlesViewTest::tinyPixelsAndMixedGesturesKeepTheirAxis(){
    NavigationFixture f;auto* chart=f.chart();QVERIFY(f.view.navigateToTime(f.end-100*f.interval));
    const auto zoom=chart->property("viewportZoomBars").toInt();auto offset=chart->property("viewportOffsetSlots").toDouble();
    navigationWheel(chart,QPoint(1,0),{});QVERIFY(chart->property("viewportOffsetSlots").toDouble()>offset);QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);
    offset=chart->property("viewportOffsetSlots").toDouble();
    navigationWheel(chart,QPoint(4,1),{},Qt::ScrollBegin);navigationWheel(chart,QPoint(1,7),{},Qt::ScrollUpdate);navigationWheel(chart,{}, {},Qt::ScrollEnd);
    QVERIFY(chart->property("viewportOffsetSlots").toDouble()>offset);QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);
    // A discrete diagonal event selects the dominant direction; vertical does zoom.
    navigationWheel(chart,{},QPoint(120,40));QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);
    navigationWheel(chart,{},QPoint(30,120));QVERIFY(chart->property("viewportZoomBars").toInt()<zoom);
}
void MarketCandlesViewTest::verticalWheelAndPinchKeepPointerTime(){
    NavigationFixture f;auto* chart=f.chart();QVERIFY(f.view.navigateToTime(f.end-100*f.interval));
    // A native sequence provides one consistent pixel in widget, window and
    // screen coordinates. macOS recognizes its pinch centre at Begin.
    const QPointF pointer(qRound(chart->width()*.35),qRound(chart->height()*.35));const auto before=navigationPointerTime(chart,pointer.x(),f.interval);
    const auto zoom=chart->property("viewportZoomBars").toInt();navigationWheel(chart,{},QPoint(0,120),Qt::NoScrollPhase,pointer);
    QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom-10);
    QVERIFY(std::abs(navigationPointerTime(chart,pointer.x(),f.interval)-before)<1.0L);
    QPinchGesture pinch;pinch.setCenterPoint(pointer);pinch.setChangeFlags(QPinchGesture::ScaleFactorChanged);pinch.setLastScaleFactor(1);pinch.setScaleFactor(1.2);
    QGestureEvent gesture({&pinch});QApplication::sendEvent(chart,&gesture);
    QVERIFY(chart->property("viewportZoomBars").toInt()<zoom-10);QVERIFY(std::abs(navigationPointerTime(chart,pointer.x(),f.interval)-before)<1.0L);
    const auto beforeNative=chart->property("viewportZoomBars").toInt();
    const auto nativeGesture=[&](Qt::NativeGestureType type,qreal value){
        QNativeGestureEvent event(type,QPointingDevice::primaryPointingDevice(),2,
            pointer,chart->mapTo(chart->window(),pointer.toPoint()),chart->mapToGlobal(pointer.toPoint()),value,{});
        QApplication::sendEvent(chart,&event);
    };
    nativeGesture(Qt::BeginNativeGesture,0);
    nativeGesture(Qt::ZoomNativeGesture,.10);
    const auto afterNativeZoom=chart->property("viewportZoomBars").toInt();
    nativeGesture(Qt::EndNativeGesture,0);
    QCOMPARE(chart->property("viewportZoomBars").toInt(),afterNativeZoom);
    QVERIFY(afterNativeZoom<beforeNative);
    // Center/rotation-only gesture updates must not reapply a previous scale.
    QPinchGesture unchanged;unchanged.setCenterPoint(pointer);unchanged.setLastScaleFactor(1);unchanged.setScaleFactor(1.2);
    unchanged.setChangeFlags(QPinchGesture::CenterPointChanged);
    QGestureEvent noScale({&unchanged});QApplication::sendEvent(chart,&noScale);
    QCOMPARE(chart->property("viewportZoomBars").toInt(),afterNativeZoom);
    QVERIFY(std::abs(navigationPointerTime(chart,pointer.x(),f.interval)-before)<1.0L);
    f.view.returnToLive();const auto liveAnchor=navigationPointerTime(chart,pointer.x(),f.interval);
    navigationWheel(chart,{},QPoint(0,-120),Qt::NoScrollPhase,pointer);
    QVERIFY(std::abs(navigationPointerTime(chart,pointer.x(),f.interval)-liveAnchor)<1.0L);
}
void MarketCandlesViewTest::prependingAndTicksKeepHistoricalAnchor_data(){
    QTest::addColumn<bool>("binance");QTest::newRow("upbit")<<false;QTest::newRow("binance")<<true;
}
void MarketCandlesViewTest::prependingAndTicksKeepHistoricalAnchor(){
    QFETCH(bool,binance);NavigationFixture f(binance);auto* chart=f.chart();
    QVERIFY(f.view.navigateToTime(f.end-150*f.interval));const auto before=f.view.visibleViewport();
    const auto x=chart->property("viewportFirstBarX").toDouble(),span=chart->property("viewportSpanSlots").toDouble();
    QVERIFY(f.loadPage(1));QCOMPARE(f.model.displayObservation().candles.size(),400);
    QCOMPARE(f.view.visibleViewport().fromMs,before.fromMs);QCOMPARE(f.view.visibleViewport().toMs,before.toMs);
    QVERIFY(std::abs(chart->property("viewportFirstBarX").toDouble()-x)<.01);QCOMPARE(chart->property("viewportSpanSlots").toDouble(),span);
    f.tick();QVERIFY(f.model.series().forming);QCOMPARE(f.view.visibleViewport().fromMs,before.fromMs);QCOMPARE(f.view.visibleViewport().toMs,before.toMs);
    QVERIFY(std::abs(chart->property("viewportFirstBarX").toDouble()-x)<.01);QVERIFY(!chart->property("viewportLive").toBool());
    auto* live=f.view.findChild<QPushButton*>("chartReturnToLive");QVERIFY(live&&live->isEnabled());QTest::mouseClick(live,Qt::LeftButton);
    QVERIFY(chart->property("viewportLive").toBool());QCOMPARE(f.view.visibleViewport().toMs,f.end-f.interval);QVERIFY(!live->isEnabled());
}
void MarketCandlesViewTest::wheelOutsidePricePlotIsNotConsumed(){
    NavigationFixture f;auto* chart=f.chart();const auto zoom=chart->property("viewportZoomBars").toInt();
    const QPointF outside(chart->width()-2,chart->height()-2);
    QWheelEvent event(outside,chart->mapToGlobal(outside.toPoint()),{},QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(chart,&event);QVERIFY(!event.isAccepted());QCOMPARE(chart->property("viewportZoomBars").toInt(),zoom);
    QScrollArea page;auto* contents=new QWidget;contents->setMinimumHeight(2000);page.setWidget(contents);page.resize(400,300);page.show();QCoreApplication::processEvents();
    const auto before=page.verticalScrollBar()->value();const QPointF position(50,50);
    QWheelEvent scroll(position,page.viewport()->mapToGlobal(position.toPoint()),{},QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(page.viewport(),&scroll);QVERIFY(page.verticalScrollBar()->value()>before);
}
void MarketCandlesViewTest::archiveHistoryShowsActualRefreshState_data(){
    QTest::addColumn<QString>("state");QTest::addColumn<QString>("expected");
    QTest::newRow("waiting")<<QString("waiting")<<QString("저장된 캔들");
    QTest::newRow("http-failure")<<QString("http-failure")<<QString("저장된 기록 · 최신 자료 받지 못함");
    QTest::newRow("limited")<<QString("limited")<<QString("저장된 기록 · 재조회 대기");
    QTest::newRow("disconnected")<<QString("disconnected")<<QString("저장된 기록 · 연결 끊김");
}
void MarketCandlesViewTest::archiveHistoryShowsActualRefreshState(){
    QFETCH(QString,state);QFETCH(QString,expected);
    StudyTransport transport;auto socket=std::make_unique<StudySocket>();auto* stream=socket.get();
    MarketCandles source(&transport,std::move(socket),[]{return StudyNow;});MarketCandlesView view(&source);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.show();
    MarketCandleSeries stored;stored.identity="coin:upbit:KRW-SYN";stored.asset="SYN";stored.currency="KRW";
    stored.intervalMs=60000;stored.receivedAtMs=StudyNow;stored.candles=syntheticStudyRows(5);stored.fromArchive=true;
    QVERIFY(source.setStoredCompleted(stored)==StoredCandlesOffer::Accepted);
    if(state=="disconnected")emit stream->failed(stream->generation);
    else if(state!="waiting"){HttpResult failed;failed.status=state=="limited"?429:503;emit transport.finished(transport.token,failed);}
    QCOMPARE(displayedText(view.findChild<QLabel*>("marketCandlesStatus")),expected);
    QVERIFY(source.displayObservation().fromArchive);QVERIFY(!source.series().available);
}
void MarketCandlesViewTest::archiveStorageIssueHasAnIndependentRetryAction(){
    StudyTransport transport;MarketCandles source(&transport);MarketCandlesView view(&source);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.show();
    QSignalSpy retries(&source,&MarketCandles::archiveRetryRequested);
    source.setArchiveStatus({"기록 저장 중단","합성 기록 저장 실패",true});
    auto* notice=view.findChild<QWidget*>("marketCandlesArchiveNotice");auto* retry=view.findChild<QPushButton*>("marketCandlesArchiveRetry");
    QVERIFY(notice&&retry);QVERIFY(!notice->isHidden());QVERIFY(!retry->isHidden());
    QCOMPARE(view.findChild<QLabel*>("marketCandlesArchiveStatus")->text(),QString("기록 저장 중단"));
    retry->click();QCOMPARE(retries.size(),1);
    source.setArchiveStatus({});QVERIFY(notice->isHidden());QVERIFY(!source.series().available);
}
void MarketCandlesViewTest::chartStateIndicatorsPublishActualValuesAndHonorVisibility(){
    StudyTransport http;
    MarketCandles model(&http,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;
    preferences.markIntroSeen();
    const QStringList ids={"contract","absorption","sweep","squeeze","adx","breakout","strong","trend","relative_strength"};
    for(const auto& id:ids)preferences.setEnabled(id,true);
    preferences.setMovingAverages({{"ma1",3,ExtendedMaKind::Exponential},{"ma2",200,ExtendedMaKind::Simple}});
    preferences.setEnabled("ma",true);
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(640,800);view.show();
    auto* strip=view.findChild<QScrollArea*>("chartStudyReadouts");QVERIFY(strip);
    auto* applied=view.findChild<QPushButton*>("chartStudyAppliedToggle");QVERIFY(applied);
    if(applied->isChecked())applied->click();
    const auto readout=[&](const QString& id){return strip->findChild<QLabel*>("chartStudyReadout_"+id);};
    for(const auto& id:ids){QVERIFY(readout(id));QVERIFY(readout(id)->isVisibleTo(&view));}
    const int stripHeight=strip->height();
    emit http.finished(http.token,syntheticStudyResponse(syntheticStudyRows(80)));
    const auto state=[&](const QString& id){
        for(const auto& item:view.indicatorStates())if(item.id==id)return item;
        return ChartIndicatorState{};
    };
    QTRY_VERIFY(state("adx").requiredReady);
    QTRY_COMPARE(state("ma").dataDetail,IndicatorDataDetail::Partial);
    QVERIFY(state("ma").status.contains("1/2"));
    QVERIFY(state("adx").status.contains(QRegularExpression("[0-9]+\\.[0-9]+")));
    QVERIFY(readout("adx")->text().contains(state("adx").status));
    QCOMPARE(state("absorption").dataDetail,IndicatorDataDetail::NoSignal);
    QVERIFY(readout("absorption")->text().contains(QStringLiteral("현재 조건 미충족")));
    view.setSupplementalStudies({{StudyNow,StudyNow,3600000,{true,IndicatorDirection::Up,{}}},
                                 {0,0,14400000,{false,IndicatorDirection::None,QStringLiteral("기간 기록 부족")}},
                                 {0,0,86400000,{false,IndicatorDirection::None,QStringLiteral("기간 기록 부족")}}},{},{});
    QCOMPARE(state("trend").dataDetail,IndicatorDataDetail::Partial);
    QVERIFY(!state("trend").requiredReady);
    QVERIFY(readout("trend")->text().contains(QStringLiteral("자료 없음")));
    QVERIFY(readout("trend")->toolTip().contains(QStringLiteral("기간 기록 부족")));
    QVERIFY(!state("contract").requiredReady);
    QVERIFY(readout("contract")->toolTip().contains(QStringLiteral("OI")));
    QTRY_VERIFY(state("adx").requiredReady);
    preferences.setVisible("adx",false);
    QVERIFY(!readout("adx")->isVisible());QVERIFY(readout("adx")->text().isEmpty());
    QVERIFY(state("adx").requiredReady);
    preferences.setVisible("adx",true);
    QVERIFY(readout("adx")->isVisible());QVERIFY(readout("adx")->text().contains(state("adx").status));
    QTRY_VERIFY(strip->horizontalScrollBar()->maximum()>0);
    strip->ensureWidgetVisible(readout("adx"),0,0);
    QVERIFY(saveDev86IndicatorCapture(view,"chart-state-readouts"));
    strip->ensureWidgetVisible(readout("relative_strength"),0,0);
    QTRY_VERIFY(strip->viewport()->rect().intersects(QRect(readout("relative_strength")->mapTo(strip->viewport(),QPoint{}),readout("relative_strength")->size())));
    QCOMPARE(strip->height(),stripHeight);QVERIFY(view.pricePlotHeight()>100);
    for(const auto& id:ids)preferences.setVisible(id,false);
    QVERIFY(!strip->isVisible());QVERIFY(preferences.enabled("adx"));
}
void MarketCandlesViewTest::indicatorNamesDistinguishAppliedVisibilityAndReadiness() {
    QVERIFY(KeefiFonts::registerBundledFonts());
    StudyTransport transport;
    MarketCandles source(&transport, std::unique_ptr<PublicCandleSocket>{}, [] { return StudyNow; });
    ChartStudyPreferences preferences;
    preferences.markIntroSeen();
    preferences.setEnabled("volume", true);
    preferences.setVisible("volume", true);
    preferences.setEnabled("bb", true);
    preferences.setVisible("bb", true);
    preferences.setEnabled("rsi", false);
    MarketCandlesView view(&source);
    view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN", 60000);
    view.resize(1024, 700);
    view.show();
    emit transport.finished(transport.token, syntheticStudyResponse(syntheticStudyRows(30)));
    QTRY_VERIFY(view.chartStudies().available);
    bool checked = false;
    QTimer::singleShot(0, &view, [&] {
        auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
        QVERIFY(dialog);
        const auto close = qScopeGuard([&] { dialog->reject(); });
        auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
        QVERIFY(tree);
        const auto row = [tree](const QString& id) -> QTreeWidgetItem* {
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toString() == id) return *it;
            return nullptr;
        };
        auto* volume = row("volume");
        auto* bb = row("bb");
        auto* rsi = row("rsi");
        QVERIFY(volume && bb && rsi);
        QCOMPARE(volume->text(5), QStringLiteral("Ready"));
        QCOMPARE(bb->text(5), QStringLiteral("Needs data"));
        QCOMPARE(rsi->text(5), QStringLiteral("Not calculated"));
        QCOMPARE(volume->foreground(0).color(), themeColors().text);
        QCOMPARE(volume->font(0), KeefiFonts::font(FontRole::BodyStrong15));
        QCOMPARE(bb->foreground(0).color(), themeColors().text);
        QCOMPARE(bb->font(0), KeefiFonts::font(FontRole::BodyStrong15));
        QCOMPARE(bb->foreground(5).color(), themeColors().accentText);
        QCOMPARE(rsi->foreground(0).color(), themeColors().text3);
        QCOMPARE(rsi->font(0), KeefiFonts::font(FontRole::Body15));

        auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
        QVERIFY(search);
        const auto verifySearchGeometry = [&](QTreeWidgetItem* item, const QString& query) {
            search->clear();
            tree->scrollToItem(item);
            QCoreApplication::processEvents();
            const int compactHeight = tree->visualItemRect(item).height();
            QVERIFY(compactHeight > 0);
            search->setText(query);
            tree->scrollToItem(item);
            QCoreApplication::processEvents();
            QVERIFY(!item->data(0, Qt::UserRole + 1).toString().isEmpty());
            const auto rowRect = tree->visualItemRect(item);
            const int twoTextLines = QFontMetrics(item->font(0)).height()
                + QFontMetrics(KeefiFonts::font(FontRole::Caption12)).height();
            QVERIFY2(rowRect.height() >= twoTextLines && rowRect.height() > compactHeight,
                     qPrintable(QStringLiteral("%1 has %2 px for two text lines totaling %3 px")
                                    .arg(item->text(0)).arg(rowRect.height()).arg(twoTextLines)));
            for (int column : {1, 3, 4}) {
                auto* action = tree->itemWidget(item, column);
                QVERIFY(action && action->isVisible());
                QCOMPARE(action->height(), compactHeight);
                const int actionCenter = action->mapTo(tree->viewport(), action->rect().center()).y();
                QVERIFY(qAbs(actionCenter - rowRect.center().y()) <= 1);
            }
            search->setText(item->text(0));
            tree->scrollToItem(item);
            QCoreApplication::processEvents();
            QVERIFY(item->data(0, Qt::UserRole + 1).toString().isEmpty());
            QCOMPARE(tree->visualItemRect(item).height(), compactHeight);
        };
        verifySearchGeometry(volume, QStringLiteral("거래량"));
        verifySearchGeometry(bb, QStringLiteral("ㅂㄹㅈ"));
        verifySearchGeometry(rsi, QStringLiteral("많이 올랐나"));
        search->clear();

        preferences.setVisible("volume", false);
        QCOMPARE(volume->foreground(0).color(), themeColors().text2);
        QCOMPARE(volume->font(0), KeefiFonts::font(FontRole::BodyStrong15));
        auto* help = tree->findChild<QPushButton*>("chartStudyInfo_bb");
        QVERIFY(help);
        QCOMPARE(help->accessibleName(), QStringLiteral("Bollinger Bands Information"));
        checked = true;
    });
    auto* picker = view.findChild<QPushButton*>("chartStudyPicker");
    QVERIFY(picker);
    picker->click();
    QVERIFY(checked);
}

void MarketCandlesViewTest::archiveAdvisoriesStayInInformation_data(){
    QTest::addColumn<QString>("message");QTest::addColumn<QString>("detail");
    QTest::newRow("key-missing")<<QString("차트 기록 저장 꺼짐")<<QString("자산 기록이 키를 만든 뒤부터 저장합니다.");
    QTest::newRow("partial")<<QString("일부 기록 확인 필요")<<QString("검증하지 못한 시세 기록은 계산에 쓰지 않아요.");
    QTest::newRow("previous-exit")<<QString()<<QString("이전 종료에서 시세 기록 저장이 3초 안에 끝나지 않았어요.");
}
void MarketCandlesViewTest::archiveAdvisoriesStayInInformation(){
    QFETCH(QString,message);QFETCH(QString,detail);StudyTransport transport;MarketCandles source(&transport);MarketCandlesView view(&source);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.show();source.setArchiveStatus({message,detail,false});
    auto* notice=view.findChild<QWidget*>("marketCandlesArchiveNotice");QVERIFY(notice);QVERIFY(notice->isHidden());
    auto* info=view.findChild<InfoButton*>("marketCandlesInformation");QVERIFY(info);info->openInformation();
    auto* body=info->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(body);QVERIFY(body->toPlainText().contains(detail));
    source.setArchiveStatus({});QVERIFY(!body->toPlainText().contains(detail));
}
void MarketCandlesViewTest::verifiedMarketStatesHaveSpecificShortMessages_data(){
    QTest::addColumn<QString>("state");
    for(const auto* state:{"checking","unsupported","removed"})QTest::newRow(state)<<QString(state);
}
void MarketCandlesViewTest::verifiedMarketStatesHaveSpecificShortMessages(){
    QFETCH(QString,state);StudyTransport transport;MarketCandles model(&transport);
    model.setVerifiedUpbitMarkets(state=="removed"?QStringList{"KRW-HYPE"}:QStringList{"KRW-BTC"},state!="checking");
    MarketCandlesView view(&model);view.setSelection("coin:upbit:KRW-HYPE",60000);view.show();
    if(state=="removed"){HttpResult response;response.status=404;response.body=R"({"error":{"name":404,"message":"Code not found"}})";emit transport.finished(transport.token,response);}
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");QVERIFY(status);
    QCOMPARE(displayedText(status),state=="checking"?QString("종목 확인 중"):QString("지원 없음"));
}
void MarketCandlesViewTest::accessibleToggleChangesTheDrawingOrStudyModel_data(){
    QTest::addColumn<QString>("control");
    for(const auto* key:{"chartStudyEye_cloud","chartDrawingMagnet","chartDrawingVisible","chartDrawingLocked","chartDrawingTool_horizontal"})QTest::newRow(key)<<QString(key);
}
void MarketCandlesViewTest::accessibleToggleChangesTheDrawingOrStudyModel(){
    QFETCH(QString,control);StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1200,1000);view.show();expandAppliedIndicators(view);
    emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));QVERIFY(source.series().available);
    auto* button=view.findChild<QToolButton*>(control);QVERIFY(button&&button->isEnabled());auto* ax=QAccessible::queryAccessibleInterface(button);QVERIFY(ax&&ax->actionInterface());
    const bool before=button->isChecked();ax->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    if(control=="chartStudyEye_cloud")QCOMPARE(preferences.visible("cloud"),!before);
    else if(control=="chartDrawingMagnet")QCOMPARE(drawings.magnetEnabled(),!before);
    else if(control=="chartDrawingVisible")QCOMPARE(drawings.visible(),!before);
    else if(control=="chartDrawingLocked")QCOMPARE(drawings.locked(),!before);
    else{
        auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);auto points=visibleBodyPoints(chart);QCOMPARE(points.size(),2);
        QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,points.first());QCOMPARE(drawings.drawings().size(),1);QCOMPARE(drawings.drawings().first().kind,ChartDrawingKind::Horizontal);
    }
}
void MarketCandlesViewTest::indicatorControlsKeepFocusAcrossPublicRefresh(){
    MarketCandles model(nullptr);MarketCandlesView view(&model);ChartStudyPreferences preferences;
    preferences.markIntroSeen();view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);
    view.resize(1000,800);view.show();expandAppliedIndicators(view);view.activateWindow();QTest::qWait(20);
    for(const auto& name:{"chartStudyHelp_cloud","chartStudyEye_cloud","chartStudyOff_cloud"}){
        auto* control=view.findChild<QAbstractButton*>(name);QVERIFY(control);QVERIFY(control->isVisible());
        control->setFocus();QTRY_VERIFY(control->hasFocus());
        for(int tick=0;tick<3;++tick){emit model.changed();QCoreApplication::processEvents();QVERIFY2(control->hasFocus(),"A public update must not hide an unchanged chip or move keyboard focus");}
        view.resize(view.width()+10,800);QCoreApplication::processEvents();QVERIFY(control->hasFocus());
    }
    auto* help=view.findChild<QPushButton*>("chartStudyHelp_cloud");help->setFocus();bool opened=false;
    QTimer::singleShot(0,&view,[&]{auto* dialog=view.findChild<QDialog*>("chartStudyHelpDialog");if(!dialog)return;opened=true;emit model.changed();QTest::keyClick(dialog,Qt::Key_Escape);});
    QTest::keyClick(help,Qt::Key_Space);QVERIFY(opened);QTRY_VERIFY(help->hasFocus());
    emit model.changed();QCoreApplication::processEvents();QVERIFY(help->hasFocus());
    preferences.setEnabled("cloud",false);QVERIFY(!help->isVisible());
}
void MarketCandlesViewTest::formingUpdatesDoNotRelayoutUnchangedIndicatorControls(){
    KeefiTheme::instance()->setMode(ThemeMode::Light);
    NavigationFixture f;f.preferences.markIntroSeen();expandAppliedIndicators(f.view);
    QTRY_VERIFY_WITH_TIMEOUT(f.view.chartStudies().available,3000);
    auto* contents=f.view.findChild<QWidget*>("chartStudyLegendContents");QVERIFY(contents);
    auto* legend=f.view.findChild<QWidget*>("chartStudyLegend");QVERIFY(legend&&legend->isVisible());
    auto* eye=f.view.findChild<QToolButton*>("chartStudyEye_cloud");QVERIFY(eye&&eye->isVisible());
    auto* off=f.view.findChild<QToolButton*>("chartStudyOff_cloud");QVERIFY(off);
    f.view.activateWindow();eye->setFocus();QTRY_VERIFY(eye->hasFocus());QTest::qWait(50);
    const auto icon=[](QToolButton* button){return button->icon().pixmap(QSize(24,24)).toImage();};
    const auto shownIcon=icon(eye),offIcon=icon(off),beforeChart=f.chart()->grab().toImage();
    const auto settings=f.preferences.publicState();const auto geometry=eye->geometry();
    class LayoutObserver final:public QObject {
    public:int requests=0;
        bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::LayoutRequest)++requests;return false;}
    } observer;
    // grab() can queue initial layout work for hidden sibling chips. Drain
    // that setup work before observing requests caused by the new quotes.
    QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
    contents->installEventFilter(&observer);
    for(auto* chip:contents->findChildren<QWidget*>())
        if(chip->objectName().startsWith("chartStudyChip_"))chip->installEventFilter(&observer);
    const auto overlayGeometry=legend->geometry();
    for(int i=0;i<100;++i){
        f.tick(i%2?QStringLiteral("101"):QStringLiteral("102"));
        QCOMPARE(legend->geometry(),overlayGeometry);
        QVERIFY(eye->hasFocus());
    }
    QVERIFY(f.model.series().forming.has_value());QCOMPARE(f.model.series().forming->close,QString("101"));
    QVERIFY(beforeChart!=f.chart()->grab().toImage());
    QCOMPARE(icon(eye),shownIcon);QCOMPARE(icon(off),offIcon);QCOMPARE(eye->geometry(),geometry);
    QCOMPARE(f.preferences.publicState(),settings);
    // Observe Qt's public layout boundary, not a private refresh/cache counter.
    // New quote pixels must not invalidate a structurally unchanged legend.
    QCOMPARE(observer.requests,0);
    f.preferences.setVisible("cloud",false);QVERIFY(icon(eye)!=shownIcon);
    QCOMPARE(eye->accessibleName(),QStringLiteral("Trend Cloud Hidden; show on chart"));
    f.preferences.setVisible("cloud",true);QCOMPARE(icon(eye),shownIcon);
    KeefiTheme::instance()->setMode(ThemeMode::Dark);QVERIFY(icon(eye)!=shownIcon);QVERIFY(icon(off)!=offIcon);
    KeefiTheme::instance()->setMode(ThemeMode::Light);QCOMPARE(icon(eye),shownIcon);QCOMPARE(icon(off),offIcon);
    f.preferences.setEnabled("cloud",false);QVERIFY(!eye->isVisible());
    f.preferences.setEnabled("cloud",true);QVERIFY(eye->isVisible());
    f.view.resize(640,1000);QCoreApplication::processEvents();
    int narrow=0;for(auto* child:contents->findChildren<QWidget*>())if(child->objectName().startsWith("chartStudyChip_")&&child->isVisible())++narrow;
    QCOMPARE(narrow,3);
    f.view.resize(1280,1000);QCoreApplication::processEvents();
    int wide=0;for(auto* child:contents->findChildren<QWidget*>())if(child->objectName().startsWith("chartStudyChip_")&&child->isVisible())++wide;
    QCOMPARE(wide,4);
}
void MarketCandlesViewTest::indicatorHelpHasKeyboardLayersAndClearsAtContextBoundary_data(){QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;}
void MarketCandlesViewTest::indicatorHelpHasKeyboardLayersAndClearsAtContextBoundary(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    MarketCandlesView view(nullptr);ChartStudyPreferences preferences;preferences.markIntroSeen();view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,800);view.show();expandAppliedIndicators(view);
    auto* help=view.findChild<QPushButton*>("chartStudyHelp_cloud");QVERIFY(help);QVERIFY(help->isVisible());QVERIFY(help->width()>=28&&help->height()>=28);
    const auto saved=preferences.publicState();bool read=false,details=false,closedOnBoundary=false,captured=false;
    help->setFocus();
    QTimer::singleShot(0,&view,[&]{auto* dialog=view.findChild<QDialog*>("chartStudyHelpDialog");if(!dialog)return;
        auto* brief=dialog->findChild<QLabel*>("chartStudyHelp_brief");auto* inputs=dialog->findChild<QLabel*>("chartStudyHelp_inputs");
        auto* reading=dialog->findChild<QLabel*>("chartStudyHelp_reading");auto* limits=dialog->findChild<QLabel*>("chartStudyHelp_limits");
        read=brief&&inputs&&reading&&limits&&!brief->text().isEmpty()&&!inputs->text().isEmpty()&&!reading->text().isEmpty()&&!limits->text().isEmpty();
        captured=saveD3CandleRender(QString("indicator-help-%1").arg(dark?"dark":"light"),dialog->grab().toImage());
        if(auto* expand=dialog->findChild<QPushButton*>("chartStudyHelpDetails")){QTest::mouseClick(expand,Qt::LeftButton);auto* detail=dialog->findChild<QLabel*>("chartStudyHelp_calculation");details=detail&&detail->isVisible()&&detail->text().contains("EMA 12")&&detail->text().contains("EMA 26");}
        QTest::keyClick(dialog,Qt::Key_Escape);
    });QTest::keyClick(help,Qt::Key_Return);QVERIFY(read);QVERIFY(details);QVERIFY(captured);QTRY_VERIFY(help->hasFocus());QCOMPARE(preferences.publicState(),saved);
    QTimer::singleShot(0,&view,[&]{auto* dialog=view.findChild<QDialog*>("chartStudyHelpDialog");if(!dialog)return;view.setSelection("coin:upbit:KRW-ETH",60000);closedOnBoundary=!dialog->isVisible();dialog->reject();});
    QTest::mouseClick(help,Qt::LeftButton);QVERIFY(closedOnBoundary);QCOMPARE(preferences.publicState(),saved);
    closedOnBoundary=false;QTimer::singleShot(0,&view,[&]{auto* dialog=view.findChild<QDialog*>("chartStudyHelpDialog");if(!dialog)return;view.clearPrivateDrawingContext();closedOnBoundary=!dialog->isVisible();dialog->reject();});
    QTest::mouseClick(help,Qt::LeftButton);QVERIFY(closedOnBoundary);QCOMPARE(preferences.publicState(),saved);
}
void MarketCandlesViewTest::pickerExplainsDisabledSimilarityWithoutEnablingIt(){
    MarketCandlesView view(nullptr);ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("similarity",false);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(900,800);view.show();
    const auto saved=preferences.publicState();bool explained=false,found=false;
    QTimer::singleShot(0,&view,[&]{auto* picker=view.findChild<QDialog*>("chartStudyPickerDialog");if(!picker)return;
        auto* tree=picker->findChild<QTreeWidget*>("chartStudyPickerTree");auto* help=picker->findChild<QPushButton*>("chartStudyInfo_similarity");
        if(tree&&help){for(int g=0;g<tree->topLevelItemCount();++g)for(int i=0;i<tree->topLevelItem(g)->childCount();++i){auto* item=tree->topLevelItem(g)->child(i);if(item->data(0,Qt::UserRole)=="similarity"){tree->setCurrentItem(item);found=item->text(0).contains(QStringLiteral("Historical Comparison"));}}
            QTimer::singleShot(0,&view,[&]{auto* dialog=picker->findChild<QDialog*>("chartStudyHelpDialog");if(!dialog)return;QString all;for(auto* label:dialog->findChildren<QLabel*>())all+=label->text();explained=all.contains(QStringLiteral("예측"))&&all.contains(QStringLiteral("모든 종목에 적용"));dialog->reject();});QTest::mouseClick(help,Qt::LeftButton);}
        picker->reject();
    });QTest::mouseClick(view.findChild<QPushButton*>("chartStudyPicker"),Qt::LeftButton);
    QVERIFY(found);QVERIFY(explained);QVERIFY(!preferences.enabled("similarity"));QCOMPARE(preferences.publicState(),saved);
}
void MarketCandlesViewTest::historicalPathCarriesVisibleNonForecastLabel_data(){QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;}
void MarketCandlesViewTest::historicalPathCarriesVisibleNonForecastLabel(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("similarity",true);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QTRY_VERIFY(view.chartStudies().similarity.available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);CandleTextDevice device;chart->render(&device);QString painted;for(const auto& run:device.engine.runs)painted+=run.text;
    QVERIFY(painted.contains(QStringLiteral("과거 경로")));QVERIFY(painted.contains(QStringLiteral("예측 아님")));
    QVERIFY(saveD3CandleRender(QString("historical-reference-%1").arg(dark?"dark":"light"),view.grab().toImage()));
    auto* label=view.findChild<QLabel*>("chartStudyStatus_similarity");QVERIFY(label);QVERIFY(label->text().contains(QStringLiteral("Historical Comparison")));
    for(const auto& state:view.indicatorStates())if(state.id=="similarity")QVERIFY(!state.stateBearing);
    preferences.setEnabled("similarity",false);CandleTextDevice disabled;chart->render(&disabled);for(const auto& run:disabled.engine.runs)QVERIFY(!run.text.contains(QStringLiteral("과거 경로")));
}
void MarketCandlesViewTest::chartPricesUseGroupedDecimalsWithoutChangingInputs_data(){
    QTest::addColumn<QString>("high");QTest::addColumn<QString>("low");QTest::addColumn<QString>("close");
    QTest::newRow("krw-large")<<QString("108810000")<<QString("106770000")<<QString("107780000.123456789");
    QTest::newRow("small-decimal")<<QString("0.00000131")<<QString("0.00000101")<<QString("0.00000123456789");
}
void MarketCandlesViewTest::chartPricesUseGroupedDecimalsWithoutChangingInputs(){
    QFETCH(QString,high);QFETCH(QString,low);QFETCH(QString,close);
    QVERIFY(KeefiFonts::registerBundledFonts());
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    MarketCandlesView view(&source);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,800);view.show();
    auto rows=syntheticStudyRows(30);for(auto& row:rows){row.open=row.close=close;row.high=high;row.low=low;row.volume="10000.012300";}
    emit transport.finished(transport.token,syntheticStudyResponse(rows));QVERIFY(source.series().available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QCoreApplication::processEvents();
    const auto before=view.shareSnapshot({},StudyNow);QVERIFY(ChartShareCard::render(before).available);
    const auto original=syntheticStudyResponse(source.series().candles).body;const auto token=transport.token;
    view.setPriceLines({{"upbit","SYNTHETIC_PRICE_SCOPE","SYN","KRW",close,"https://api.upbit.com/v1/accounts",QDateTime::currentMSecsSinceEpoch()}});
    CandleTextDevice device;chart->render(&device);QStringList painted;
    for(const auto& run:device.engine.runs){painted<<run.text;QVERIFY2(!QRegularExpression("[0-9][eE][+-]?[0-9]").match(run.text).hasMatch(),qPrintable(run.text));}
    const auto text=painted.join('\n');
    QVERIFY2(text.contains(priceAmount(high)),qPrintable(text));QVERIFY2(text.contains(priceAmount(low)),qPrintable(text));
    QVERIFY2(text.contains(priceAmount(close)+" KRW"),qPrintable(text));QVERIFY2(painted.contains(priceAmount(close)),qPrintable(text));
    for(const auto& run:device.engine.runs)if(run.text==priceAmount(high)||run.text==priceAmount(low)||run.text==priceAmount(close)){
        QCOMPARE(run.font.family(),KeefiFonts::font(FontRole::Number11).family());QCOMPARE(run.font.pixelSize(),11);
        QVERIFY2(run.position.x()+QFontMetricsF(run.font).horizontalAdvance(run.text)<=chart->width()+1,qPrintable(run.text));
    }
    // Hover OHLC moved from a covering tooltip to the chart legend; its text stays inspectable.
    const auto hovered=[chart]{return chart->property("hoverSummary").toString();};
    for(int x=20;x<chart->width()-100&&!hovered().contains(priceAmount(close));x+=12)
        QTest::mouseMove(chart,{x,chart->height()/2});
    QVERIFY2(hovered().contains(priceAmount(close)),qPrintable(hovered()));
    QVERIFY(hovered().contains(priceAmount(high)));QVERIFY(hovered().contains(priceAmount(low)));
    QVERIFY(hovered().contains(priceAmount("10000.012300")));QVERIFY(QToolTip::text().isEmpty()||!QToolTip::isVisible());
    const auto after=view.shareSnapshot({},StudyNow);
    QCOMPARE(syntheticStudyResponse(source.series().candles).body,original);QCOMPARE(view.priceLines().first().price,close);QCOMPARE(transport.token,token);
    QCOMPARE(after.identity,before.identity);QCOMPARE(after.viewport.fromMs,before.viewport.fromMs);QCOMPARE(after.viewport.toMs,before.viewport.toMs);
    QCOMPARE(syntheticStudyResponse(after.closed).body,syntheticStudyResponse(before.closed).body);
    QCOMPARE(ChartShareCard::render(after).image,ChartShareCard::render(before).image);
}
void MarketCandlesViewTest::headerControlsStayBelowTheTitleAndFollowQtOwnership(){
    auto* view=new MarketCandlesView(nullptr);view->setSelection("coin:upbit:KRW-SYN",60000);
    auto* controls=new QWidget;QPointer<QWidget> owned(controls);controls->setObjectName("syntheticHeaderControls");
    view->setHeaderControls(controls);auto* layout=qobject_cast<QVBoxLayout*>(view->layout());QVERIFY(layout);
    QCOMPARE(layout->indexOf(controls),1);QCOMPARE(controls->parentWidget(),view);
    auto* title=view->findChild<QLabel*>("marketCandlesTitle");QVERIFY(title);QVERIFY(displayedText(title).contains("SYN/KRW"));
    view->setHeaderControls(controls);view->setHeaderControls(nullptr);QCOMPARE(layout->indexOf(controls),1);
    int count=0;for(int i=0;i<layout->count();++i)count+=layout->itemAt(i)->widget()==controls;QCOMPARE(count,1);
    delete view;QVERIFY(owned.isNull());
}
void MarketCandlesViewTest::explicitLowerPanePreservesPriceAreaAndHiddenCalculation(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(828,680);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QTRY_VERIFY(view.chartStudies().available);
    auto* pane=view.findChild<QWidget*>("chartLowerPane");QVERIFY(pane);QVERIFY(!pane->isVisible());
    auto* rsi=view.findChild<QWidget*>("chartLowerPanePlot_rsi");
    auto* macd=view.findChild<QWidget*>("chartLowerPanePlot_macd");QVERIFY(rsi&&macd);
    QVERIFY(preferences.setEnabled("rsi",true));QTRY_VERIFY(view.extendedStudies().rsi.state.available);
    QVERIFY(preferences.setEnabled("macd",true));QTRY_VERIFY(view.extendedStudies().macd.state.available);
    QTRY_VERIFY(rsi->isVisible()&&macd->isVisible());
    const auto top=[&](QWidget* w){return w->mapTo(&view,QPoint(0,0)).y();};
    QVERIFY(top(rsi)+rsi->height()<=top(macd));QVERIFY(view.pricePlotHeight()>=200);
    const auto hasInk=[](QWidget* plot,QColor color){
        const auto image=plot->grab().toImage().convertToFormat(QImage::Format_RGB32);
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){
            const QColor ink=image.pixelColor(x,y);
            if(qAbs(ink.red()-color.red())+qAbs(ink.green()-color.green())+qAbs(ink.blue()-color.blue())<24)return true;
        }
        return false;
    };
    QVERIFY(hasInk(rsi,themeColors().studyPurple));QVERIFY(hasInk(macd,themeColors().studyBlue));
    QVERIFY(saveDev86IndicatorCapture(view,"rsi-macd-both-visible"));
    const auto atLive=view.visibleViewport();
    const auto liveRsi=rsi->grab().toImage(),liveMacd=macd->grab().toImage();
    QVERIFY(view.navigateToTime(source.series().candles.at(72).startsAtMs));
    QTRY_VERIFY(view.visibleViewport().fromMs<atLive.fromMs);
    QVERIFY(rsi->grab().toImage()!=liveRsi);QVERIFY(macd->grab().toImage()!=liveMacd);
    view.returnToLive();
    const auto revision=view.indicatorRevision();
    const auto value=view.extendedStudies().rsi.points.last().price;
    auto* hide=view.findChild<QPushButton*>("chartLowerPaneHide_rsi");QVERIFY(hide);
    QTest::mouseClick(hide,Qt::LeftButton);QVERIFY(!rsi->isVisible());QVERIFY(macd->isVisible());
    QVERIFY(preferences.enabled("rsi"));QCOMPARE(view.indicatorRevision(),revision);
    QCOMPARE(view.extendedStudies().rsi.points.last().price,value);
    QVERIFY(preferences.setVisible("rsi",true));view.resize(828,420);QCoreApplication::processEvents();
    QVERIFY(rsi->isVisible()&&macd->isVisible());QVERIFY(view.pricePlotHeight()>=200);
    QVERIFY(preferences.setEnabled("cvd",true));QVERIFY(preferences.setEnabled("oi",true));
    QVERIFY(view.findChild<QWidget*>("chartLowerPanePlot_cvd")->isVisible());
    QVERIFY(view.findChild<QWidget*>("chartLowerPanePlot_oi")->isVisible());
    view.setSelection("stock:KR:005930",60000);QCoreApplication::processEvents();
    QVERIFY(!view.findChild<QWidget*>("chartLowerPanePlot_cvd")->isVisible());
    QVERIFY(!view.findChild<QWidget*>("chartLowerPanePlot_oi")->isVisible());
    QVERIFY(rsi->isVisible()&&macd->isVisible());
}
void MarketCandlesViewTest::pickerUsesNameCellAndKeyboardWithFocusReturn(){
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",3600000);view.resize(828,620);view.show();
    auto* picker=view.findChild<QPushButton*>("chartStudyPicker");QVERIFY(picker);bool mouseWorked=false,enterWorked=false,spaceWorked=false;
    QTimer::singleShot(0,&view,[&]{auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");if(!dialog)return;auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");if(!tree){dialog->reject();return;}
        QTreeWidgetItem* target=nullptr;for(int g=0;g<tree->topLevelItemCount();++g)for(int i=0;i<tree->topLevelItem(g)->childCount();++i){auto* item=tree->topLevelItem(g)->child(i);if(item->data(0,Qt::UserRole)=="bb")target=item;}
        if(target){tree->scrollToItem(target);tree->setCurrentItem(target);tree->setFocus();QCoreApplication::processEvents();const auto row=tree->visualItemRect(target);
            QTest::mouseClick(tree->viewport(),Qt::LeftButton,{},QPoint(row.left()+tree->columnWidth(0)/2,row.center().y()));mouseWorked=preferences.enabled("bb");
            QTest::keyClick(tree,Qt::Key_Return);enterWorked=!preferences.enabled("bb");QTest::keyClick(tree,Qt::Key_Space);spaceWorked=preferences.enabled("bb");}
        QTest::keyClick(dialog,Qt::Key_Escape);});picker->click();QVERIFY(mouseWorked);QVERIFY(enterWorked);QVERIFY(spaceWorked);QTRY_VERIFY(picker->hasFocus());
}
void MarketCandlesViewTest::compactLegendKeepsFourTargetsAndOverflowAccessible(){
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"ma","rsi","levels","macd"})preferences.setEnabled(id,true);
    MarketCandlesView view(nullptr);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",3600000);view.resize(828,620);view.show();expandAppliedIndicators(view);
    QCoreApplication::processEvents();
    auto* more=view.findChild<QPushButton*>("chartStudyMore");QVERIFY2(more,"Enabled studies beyond four need an accessible +N control");
    QVERIFY(more->isVisible());QVERIFY(more->width()>=28);QVERIFY(more->height()>=28);
    QStyleOptionButton contents;contents.initFrom(more);contents.text=more->text();const auto contentRect=more->style()->subElementRect(QStyle::SE_PushButtonContents,&contents,more);QVERIFY2(more->fontMetrics().horizontalAdvance(more->text())<=contentRect.width(),"The +N label must fit inside the styled 28px target, after its actual padding");
    int shown=0;QSet<int> tops;
    for(auto* chip:view.findChildren<QWidget*>(QRegularExpression("^chartStudyChip_"))){
        if(!chip->isVisible())continue;++shown;tops.insert(chip->mapTo(&view,QPoint()).y());
        QCOMPARE(chip->height(), 28);
        auto* eye=chip->findChild<QToolButton*>("chartStudyEye_"+chip->objectName().mid(15));
        auto* off=chip->findChild<QToolButton*>("chartStudyOff_"+chip->objectName().mid(15));QVERIFY(eye);QVERIFY(off);
        QVERIFY(eye->width()>=28&&eye->height()>=28);QVERIFY(off->width()>=28&&off->height()>=28);
        QVERIFY(!QRect(eye->mapTo(chip,QPoint()),eye->size()).intersects(QRect(off->mapTo(chip,QPoint()),off->size())));
    }
    QVERIFY(shown>=1);QVERIFY(shown<=4);QCOMPARE(tops.size(),1);
    const auto on=preferences.enabledOrder("coin");preferences.setVisible("macd",false);QCOMPARE(preferences.enabledOrder("coin"),on);
    QVERIFY(preferences.enabled("macd"));
}
void MarketCandlesViewTest::enabledLegendOverlaysChartWithoutWaitingOrDuplicateEvidence(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(720,1100);view.show();expandAppliedIndicators(view);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");auto* legend=view.findChild<QWidget*>("chartStudyLegend");QVERIFY(chart);QVERIFY(legend);
    for(const auto* id:{"pullback","whales","similarity","cloud","volume"}){
        auto* text=view.findChild<QLabel*>(QString("chartStudyStatus_")+id);QVERIFY(text);
        QVERIFY2(!text->text().contains("<Keefi>"),qPrintable(text->text()));
        QVERIFY2(!text->text().contains(QStringLiteral("대기")),qPrintable(text->text()));
        QVERIFY(!text->property("accent").toBool());
    }
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QCoreApplication::processEvents();
    const QRect overlay(chart->mapFromGlobal(legend->mapToGlobal(QPoint())),legend->size());
    QVERIFY(chart->rect().contains(overlay));
    QCOMPARE(overlay.left(),8);
    // The painted OHLC box starts 2px into the price plot at y=12 and is 18px tall.
    QVERIFY(overlay.top()>=32);
    QVERIFY(overlay.width()<chart->width());
    // Usability defect 5: a shown study without a value folds away; every chip with a value stays inside the view.
    // Study values arrive after the deferred calculation; chips without a value are folded until then.
    QTRY_VERIFY_WITH_TIMEOUT(view.findChild<QWidget*>("chartStudyChip_volume")->property("legendHasValue").toBool(),3000);
    QSet<int> rowTops;int shown=0;for(const auto* id:{"pullback","whales","similarity","cloud","volume"}){
        auto* chip=view.findChild<QWidget*>(QString("chartStudyChip_")+id);QVERIFY(chip);
        if(!chip->isVisible()){QVERIFY(view.findChild<QPushButton*>("chartStudyMore")->isVisible());continue;}
        ++shown;
        const auto box=QRect(chip->mapTo(&view,QPoint(0,0)),chip->size());QVERIFY2(view.rect().contains(box),qPrintable(QString(id)+" "+QString::number(box.right())));
        rowTops.insert(box.top());
    }
    QVERIFY(shown>=3);QVERIFY(shown<=4);QCOMPARE(rowTops.size(),1);
    for(auto* scroll:legend->findChildren<QScrollArea*>())QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
    QVERIFY(view.findChild<QLabel*>("chartStudyPullbackEvidence")->isHidden());
    QTRY_VERIFY(view.chartStudies().available);const auto raw=source.series();const auto studies=view.chartStudies();const auto token=transport.token;
    auto* pullback=view.findChild<QLabel*>("chartStudyStatus_pullback");
    QVERIFY(!pullback->property("accent").toBool()); // Current conditions live in the right status panel; chips only control layers.
    QVERIFY(!pullback->text().contains(QStringLiteral("겹침")));QVERIFY(!view.findChild<QLabel*>("chartStudyStatus_cloud")->text().contains("EMA"));
    auto* eye=view.findChild<QToolButton*>("chartStudyEye_cloud");QVERIFY(eye);eye->click();QVERIFY(!preferences.visible("cloud"));eye->click();QVERIFY(preferences.visible("cloud"));
    QCOMPARE(syntheticStudyResponse(source.series().candles).body,syntheticStudyResponse(raw.candles).body);QCOMPARE(view.chartStudies().pullback.score,studies.pullback.score);QCOMPARE(transport.token,token);
}
void MarketCandlesViewTest::overlayLegendConsumesNoCanvasHeight() {
    NavigationFixture f;
    auto* chart=f.chart();
    auto* toggle=f.view.findChild<QPushButton*>("chartStudyAppliedToggle");
    auto* summary=f.view.findChild<QWidget*>("chartCoreStudySummary");
    auto* legend=f.view.findChild<QWidget*>("chartStudyLegend");
    QVERIFY(chart&&chart->isVisible()&&toggle&&summary&&legend);
    QVERIFY(!toggle->isChecked());
    const int collapsed=chart->height();
    toggle->click();
    QTRY_VERIFY(summary->isVisible()&&legend->isVisible());
    QCoreApplication::processEvents();
    QCOMPARE(collapsed-chart->height(),summary->height()+f.view.layout()->spacing());

    // Keep the summary out of the geometry measurement. The shared Enabled
    // control continues toggling its visibility.
    f.view.layout()->removeWidget(summary);
    toggle->click();
    QCoreApplication::processEvents();
    const int isolatedHeight=chart->height();
    toggle->click();
    QCoreApplication::processEvents();
    QVERIFY(legend->isVisible());
    QCOMPARE(chart->height(),isolatedHeight);
}

void MarketCandlesViewTest::overlayLegendPassesWheelGesturesToChart() {
    NavigationFixture f;
    expandAppliedIndicators(f.view);
    auto* legend=f.view.findChild<QWidget*>("chartStudyLegend");
    auto* eye=f.view.findChild<QToolButton*>("chartStudyEye_cloud");
    QVERIFY(legend&&legend->isVisible()&&eye&&eye->isVisible());
    const auto settings=f.preferences.publicState();
    const QPointF at=eye->rect().center();

    const int originalZoom=f.view.nominalZoomBars();
    navigationWheel(eye,{},QPoint(0,120),Qt::NoScrollPhase,at);
    QVERIFY(f.view.nominalZoomBars()<originalZoom);

    const auto before=f.view.syncViewport();
    QVERIFY(before.valid());
    const int zoom=f.view.nominalZoomBars();
    navigationWheel(eye,{},{},Qt::ScrollBegin,at);
    navigationWheel(eye,QPoint(80,2),{},Qt::ScrollUpdate,at);
    navigationWheel(eye,{},{},Qt::ScrollEnd,at);
    QVERIFY(f.view.syncViewport().fromMs<before.fromMs);
    QCOMPARE(f.view.nominalZoomBars(),zoom);
    QCOMPARE(f.preferences.publicState(),settings);
}
// Dev 97: the cached chart layer plus the pointer overlay must be the same picture as a full repaint.
void MarketCandlesViewTest::pointerOverlayMatchesFullRepaint(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"ma","bb"})preferences.setEnabled(id,true);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1180,720);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QCoreApplication::processEvents();
    QTRY_VERIFY(view.chartStudies().available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    const auto both=[&](const QString& state){
        chart->setProperty("keefiNoStaticCache",false);chart->repaint();const auto cached=chart->grab().toImage();
        chart->repaint();const auto cachedAgain=chart->grab().toImage(); // Second paint reuses the layer.
        chart->setProperty("keefiNoStaticCache",true);chart->repaint();const auto full=chart->grab().toImage();
        chart->setProperty("keefiNoStaticCache",false);
        if(cached!=full||cachedAgain!=full){const auto dir=qEnvironmentVariable("KEEFI_POINTER_RENDER_DIR");
            if(!dir.isEmpty()){QDir().mkpath(dir);cached.save(dir+"/"+state+"-cached.png");full.save(dir+"/"+state+"-full.png");}}
        QVERIFY2(cached==full,qPrintable(state));QVERIFY2(cachedAgain==full,qPrintable(state+" reused"));
    };
    int states=0;
    for(auto theme:{ThemeMode::Light,ThemeMode::Dark}){
        KeefiTheme::instance()->setMode(theme);QCoreApplication::processEvents();
        const auto name=QString(theme==ThemeMode::Light?"light":"dark");
        both(name+"-no-pointer");++states;
        for(const auto& at:{QPoint(chart->width()/2,chart->height()/2),QPoint(chart->width()/3,60),QPoint(chart->width()-120,chart->height()-90),QPoint(40,chart->height()/3)}){
            QTest::mouseMove(chart,at);QCoreApplication::processEvents();
            both(QString("%1-pointer-%2-%3").arg(name).arg(at.x()).arg(at.y()));++states;
        }
        QTest::mouseMove(chart,QPoint(chart->width()-8,chart->height()/2));QCoreApplication::processEvents();both(name+"-on-price-axis");++states;
        QTest::keyClick(chart,Qt::Key_Left);QTest::keyClick(chart,Qt::Key_Minus);QCoreApplication::processEvents();both(name+"-after-pan-zoom");++states;
        QTest::mouseMove(chart,QPoint(chart->width()/2,chart->height()/2));QCoreApplication::processEvents();both(name+"-pointer-after-pan");++states;
    }
    KeefiTheme::instance()->setMode(ThemeMode::Light);QCoreApplication::processEvents();
    QVERIFY(states>=12);
    // Pointer moves repaint strips only. What the window shows after a run of moves must equal a full repaint.
    const auto shown=[&]{QCoreApplication::processEvents();return view.screen()->grabWindow(view.winId(),chart->mapTo(&view,QPoint()).x(),chart->mapTo(&view,QPoint()).y(),chart->width(),chart->height()).toImage();};
    for(int i=0;i<40;++i){const QPoint at(30+(i*53)%(chart->width()-60),20+(i*37)%(chart->height()-40));
        QMouseEvent move(QEvent::MouseMove,at,chart->mapToGlobal(at),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(chart,&move);
        if(i%7==6){const auto strips=shown();chart->setProperty("keefiNoStaticCache",true);chart->repaint();const auto full=shown();chart->setProperty("keefiNoStaticCache",false);
            if(strips.isNull())QSKIP("This platform cannot read the window surface");
            QVERIFY2(strips==full,qPrintable(QString("strip repaint differs after move %1").arg(i)));chart->repaint();}}
}
// Opt-in measurement (KEEFI_POINTER_BENCH=1): pointer move to finished paint, cached layer against full repaint.
void MarketCandlesViewTest::pointerMoveCostWhenAsked(){
    if(!qEnvironmentVariableIsSet("KEEFI_POINTER_BENCH"))QSKIP("Set KEEFI_POINTER_BENCH=1 to measure");
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"ma","bb"})preferences.setEnabled(id,true);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QCoreApplication::processEvents();
    QTRY_VERIFY(view.chartStudies().available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    for(const auto& size:{QSize(1408,700),QSize(2000,1000),QSize(2500,1300)}){
        view.resize(size);QCoreApplication::processEvents();
        for(const bool cached:{false,true}){
            chart->setProperty("keefiNoStaticCache",!cached);chart->repaint();
            QList<qint64> micros;QElapsedTimer timer;
            for(int i=0;i<300;++i){const QPoint at(60+(i*7)%(chart->width()-200),40+(i*5)%(chart->height()-120));
                timer.start();QMouseEvent move(QEvent::MouseMove,at,chart->mapToGlobal(at),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QCoreApplication::sendEvent(chart,&move);QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();micros.append(timer.nsecsElapsed()/1000);}
            std::sort(micros.begin(),micros.end());
            qInfo().noquote()<<QString("POINTER %1x%2 chart %3x%4 %5 p50 %6us p95 %7us max %8us").arg(size.width()).arg(size.height()).arg(chart->width()).arg(chart->height())
                .arg(cached?"cached":"full").arg(micros[150]).arg(micros[285]).arg(micros.last());
        }
        chart->setProperty("keefiNoStaticCache",false);
    }
}
void MarketCandlesViewTest::chartNavigationUsesExistingGesturesWithoutFourButtons(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    MarketCandlesView view(&source);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,800);view.show();
    const auto rows=syntheticStudyRows(200);emit transport.finished(transport.token,syntheticStudyResponse(rows));
    QVERIFY2(!view.findChild<QWidget*>("marketCandlesControls"),"The redundant four navigation buttons must not reserve a layout row");
    const QStringList removedNames{QStringLiteral("이전 캔들로 이동"),QStringLiteral("최근 캔들로 이동"),QStringLiteral("캔들 확대"),QStringLiteral("캔들 축소")};
    for(auto* button:view.findChildren<QPushButton*>())QVERIFY(!removedNames.contains(button->accessibleName()));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);chart->setFocus();
    const auto original=syntheticStudyResponse(source.series().candles).body;const auto token=transport.token;
    QTest::keyClick(chart,Qt::Key_Left);QTest::keyClick(chart,Qt::Key_Left);
    auto snapshot=view.shareSnapshot({},StudyNow);QCOMPARE(snapshot.viewport.fromMs,rows[120].startsAtMs);QCOMPARE(snapshot.viewport.toMs,rows[179].startsAtMs);
    QTest::keyClick(chart,Qt::Key_Right);QTest::keyClick(chart,Qt::Key_Right);QTest::keyClick(chart,Qt::Key_Plus);
    snapshot=view.shareSnapshot({},StudyNow);QCOMPARE(snapshot.viewport.fromMs,rows[150].startsAtMs);QCOMPARE(snapshot.viewport.toMs,rows[199].startsAtMs);
    QTest::keyClick(chart,Qt::Key_Minus);snapshot=view.shareSnapshot({},StudyNow);QCOMPARE(snapshot.viewport.fromMs,rows[140].startsAtMs);
    const QPoint center(chart->width()/2,chart->height()/2);const auto pointerTime=navigationPointerTime(chart,center.x(),60000);
    QWheelEvent wheel(center,chart->mapToGlobal(center),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(chart,&wheel);snapshot=view.shareSnapshot({},StudyNow);
    QCOMPARE(chart->property("viewportZoomBars").toInt(),50);
    QVERIFY(std::abs(navigationPointerTime(chart,center.x(),60000)-pointerTime)<1.0L);
    QCOMPARE(snapshot.viewport.toMs-snapshot.viewport.fromMs,qint64(49*60000));
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,center);QTest::mouseMove(chart,center+QPoint(120,0));QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,center+QPoint(120,0));
    snapshot=view.shareSnapshot({},StudyNow);QVERIFY(snapshot.viewport.toMs<rows.last().startsAtMs);
    QCOMPARE(syntheticStudyResponse(source.series().candles).body,original);QCOMPARE(transport.token,token);
    QVERIFY(view.findChild<InfoButton*>("marketCandlesInformation"));
}
void MarketCandlesViewTest::horizontalDrawingPricesGroupWithoutChangingSavedAnchors(){
    QVERIFY(KeefiFonts::registerBundledFonts());
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;
    MarketCandlesView view(&source);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,800);view.show();
    const QString price="107780000.123456789";auto rows=syntheticStudyRows(30);
    for(auto& row:rows){row.open=row.close=price;row.high="108810000";row.low="106770000";}
    emit transport.finished(transport.token,syntheticStudyResponse(rows));QVERIFY(source.series().available);
    ChartDrawingFrame frame;frame.identity=drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;frame.asOfMs=StudyNow;frame.contextGeneration=drawings.contextGeneration();
    for(const auto& row:source.series().candles)frame.bars.append({row,row.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const QList<ChartDrawingPoint> anchors{{rows.last().startsAtMs,price}};QVERIFY(!drawings.create(ChartDrawingKind::Horizontal,anchors,frame).isEmpty());
    const auto saved=drawings.publicState();const auto token=transport.token;
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QCoreApplication::processEvents();
    CandleTextDevice device;chart->render(&device);bool grouped=false;
    for(const auto& run:device.engine.runs)if(run.text==priceAmount(price)+" KRW"){
        grouped=true;QCOMPARE(run.font.family(),KeefiFonts::font(FontRole::Number11).family());QCOMPARE(run.font.pixelSize(),11);
    }
    QVERIFY2(grouped,"The public horizontal line must paint its exact grouped price");
    const auto y=qRound(12+(108810000-price.toDouble())/(108810000-106770000)*(chart->height()-42));
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,{chart->width()/2,y});
    auto* status=view.findChild<QLabel*>("chartDrawingLineStatus");QVERIFY(status);QVERIFY(displayedText(status).contains(priceAmount(price)));
    QCOMPARE(drawings.publicState(),saved);QCOMPARE(drawings.drawings().first().anchors,anchors);QCOMPARE(transport.token,token);
}
void MarketCandlesViewTest::d3CandlesUseHollowRisesAndFilledFalls_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void MarketCandlesViewTest::d3CandlesUseHollowRisesAndFilledFalls(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,800);view.show();
    auto rows=syntheticStudyRows(2);for(int i=0;i<2;++i){rows[i].open=i?"105":"95";rows[i].close=i?"95":"105";rows[i].high="110";rows[i].low="90";}
    emit transport.finished(transport.token,syntheticStudyResponse(rows));auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    const auto image=chart->grab().toImage();
    QVERIFY(saveD3CandleRender(dark?"candles-dark":"candles-light",image));
    const auto inkBounds=[&](const QColor& ink){QRect bounds;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==ink)bounds=bounds.isNull()?QRect(x,y,1,1):bounds.united(QRect(x,y,1,1));return bounds;};
    const auto up=inkBounds(themeColors().up),down=inkBounds(themeColors().down);QVERIFY(up.width()>8);QVERIFY(down.width()>8);
    QCOMPARE(image.pixelColor(up.center()),themeColors().bg);
    QCOMPARE(image.pixelColor(down.center()),themeColors().down);
    QCOMPARE(image.pixelColor(QPoint(up.left(),up.center().y())),themeColors().up);
    QCOMPARE(source.series().candles.size(),2);QCOMPARE(source.series().candles.first().open,QString("95"));QCOMPARE(source.series().candles.last().close,QString("95"));
}
void MarketCandlesViewTest::d3LegendAndToolsUseRowsAndLineIcons(){
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();expandAppliedIndicators(view);
    for(const auto* name:{"chartDrawingTool_crosshair","chartDrawingTool_trend","chartDrawingTool_horizontal","chartDrawingTool_measure","chartDrawingMagnet","chartDrawingVisible","chartDrawingLocked","chartDrawingClear"}){
        auto* button=view.findChild<QToolButton*>(name);QVERIFY(button);QCOMPARE(button->size(),QSize(44,44));QVERIFY(!button->icon().isNull());QVERIFY(!button->accessibleName().isEmpty());
    }
    for(const auto* id:{"pullback","whales","similarity","cloud","volume"}){
        auto* row=view.findChild<QWidget*>(QString("chartStudyChip_")+id);QVERIFY(row);QVERIFY(!row->property("card").toBool());
        auto* label=row->findChild<QLabel*>(QString("chartStudyStatus_")+id);QVERIFY(label);QVERIFY(!label->wordWrap());
        auto* eye=row->findChild<QToolButton*>(QString("chartStudyEye_")+id);QVERIFY(!eye->icon().isNull());
        eye->click();QVERIFY2(!preferences.visible(id),id);eye->click();QVERIFY2(preferences.visible(id),id);
        row->findChild<QToolButton*>(QString("chartStudyOff_")+id)->click();QVERIFY(!preferences.enabled(id));
    }
    QVERIFY(saveD3CandleRender("legend-tools",view.grab().toImage()));
}
void MarketCandlesViewTest::d3PullbackUsesAccentHatchingAndAverageUsesTextDashes(){
    KeefiTheme::instance()->setMode(ThemeMode::Light);
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();
    auto rows=syntheticStudyRows(200);rows[190].low="90";rows[191].high="180";emit transport.finished(transport.token,syntheticStudyResponse(rows));QTRY_VERIFY(view.chartStudies().pullback.available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto hatched=chart->grab().toImage();
    QVERIFY(saveD3CandleRender("pullback",hatched));
    const auto hatchInk=QColor::fromRgb(KeefiTheme::blend(themeColors().accent,themeColors().bg,.6).rgb());int hatchPixels=0;
    for(int y=0;y<hatched.height();++y)for(int x=0;x<hatched.width();++x)hatchPixels+=hatched.pixelColor(x,y)==hatchInk;
    QVERIFY2(hatchPixels>10,"The observed pullback box must contain 60% accent hatch strokes, not a flat translucent fill");
    const int hatchScale=qRound(hatched.devicePixelRatio());const double rowInterval=6*std::sqrt(2.0)*hatchScale;int expectedIntervals=0,shortIntervals=0,diagonalPairs=0;
    for(int y=0;y<hatched.height();++y){int previousStart=-1;bool previousInk=false;
        for(int x=0;x<hatched.width();++x){const bool ink=hatched.pixelColor(x,y)==hatchInk;
            if(ink&&!previousInk){if(previousStart>=0){const int distance=x-previousStart;if(std::abs(distance-rowInterval)<=1)++expectedIntervals;else if(distance<rowInterval-1)++shortIntervals;}previousStart=x;}
            if(ink&&x+hatchScale<hatched.width()&&y>=hatchScale&&hatched.pixelColor(x+hatchScale,y-hatchScale)==hatchInk)++diagonalPairs;previousInk=ink;}}
    QVERIFY2(expectedIntervals>20&&expectedIntervals>4*shortIntervals,"Actual hatch strokes must be six logical pixels apart perpendicular to their 45-degree direction");
    QVERIFY2(diagonalPairs>hatchPixels*.7,"The hatch pixel runs must follow a 45-degree diagonal");
    preferences.setVisible("pullback",false);
    const auto now=QDateTime::currentMSecsSinceEpoch();view.setPriceLines({{"upbit","SYNTHETIC_ACCOUNT_SCOPE","SYN","KRW","130","https://api.upbit.com/v1/accounts",now}});
    const auto average=chart->grab().toImage();int best=0,bestY=-1;
    QVERIFY(saveD3CandleRender("average",average));
    for(int y=0;y<average.height();++y){int hits=0;for(int x=0;x<average.width()-80*average.devicePixelRatio();++x)hits+=average.pixelColor(x,y)==themeColors().text;if(hits>best){best=hits;bestY=y;}}
    QVERIFY2(best>average.width()/3,"Average-price reference must use the text token across the plot");
    const int scale=qRound(average.devicePixelRatio());int matchedDashes=0,matchedGaps=0,run=0;bool ink=false;
    for(int x=8*scale;x<average.width()-80*scale;++x){const bool next=average.pixelColor(x,bestY)==themeColors().text;
        if(next==ink)++run;else{if(ink&&run==5*scale)++matchedDashes;if(!ink&&run==4*scale)++matchedGaps;ink=next;run=1;}}
    QVERIFY2(matchedDashes>10&&matchedGaps>10,"The actual average-price pixels must retain five-pixel dashes and four-pixel gaps");
}
void MarketCandlesViewTest::d3EvidenceNumbersOnlyChangePresentation(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,800);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(6)));auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    const auto token=transport.token;const auto candles=source.series().candles;const auto before=chart->grab().toImage();
    QSignalSpy studies(&view,&MarketCandlesView::studiesChanged),flow(&view,&MarketCandlesView::coinFlowStudiesChanged);
    view.setCandleEvidenceNumber(7);QCOMPARE(before,chart->grab().toImage());QCOMPARE(studies.size(),0);QCOMPARE(flow.size(),0);QCOMPARE(transport.token,token);QCOMPARE(syntheticStudyResponse(source.series().candles).body,syntheticStudyResponse(candles).body);
    view.setCandleEvidenceNumber(0);QCOMPARE(chart->grab().toImage(),before);
    const auto now=QDateTime::currentMSecsSinceEpoch();view.setPriceLines({{"upbit","SYNTHETIC_ACCOUNT_SCOPE","SYN","KRW","101","https://api.upbit.com/v1/accounts",now,ChartPriceKind::Average,8}});
    QCOMPARE(view.priceLines().size(),1);QCOMPARE(view.priceLines().first().evidenceNumber,8);QVERIFY(!view.findChild<QLabel*>("marketCandlesPositionLines")->text().contains("[8]"));
    QVERIFY(!view.findChild<QLabel*>("marketCandlesPositionLines")->text().contains("범위"));
    QVERIFY(!view.findChild<QLabel*>("marketCandlesPositionLines")->toolTip().contains("https://"));
    view.clearPrivate();QVERIFY(view.priceLines().isEmpty());QVERIFY(view.findChild<QLabel*>("marketCandlesPositionLines")->text().isEmpty());QCOMPARE(studies.size(),0);QCOMPARE(flow.size(),0);
}
void MarketCandlesViewTest::usabilityHidesSourceRowsAndKeepsTheirEvidenceInternal(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));
    const auto original=source.series();QVERIFY(original.available);QVERIFY(!original.sourceUrl.isEmpty());QVERIFY(original.receivedAtMs>0);
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");QVERIFY(status);QVERIFY(status->isHidden());
    QVERIFY(view.findChild<QLabel*>("chartDrawingStatus")->isHidden());
    QVERIFY(view.findChild<QLabel*>("marketCandlesStudy")->isHidden());
    QVERIFY(view.findChild<QLabel*>("marketCandlesStudyState")->isHidden());
    for(auto* widget:view.findChildren<QWidget*>())QVERIFY2(!widget->toolTip().contains("https://"),qPrintable(widget->objectName()+" "+widget->toolTip()));
    QCOMPARE(source.series().sourceUrl,original.sourceUrl);QCOMPARE(source.series().sourceTimeMs,original.sourceTimeMs);
    QCOMPARE(source.series().receivedAtMs,original.receivedAtMs);QCOMPARE(syntheticStudyResponse(source.series().candles).body,syntheticStudyResponse(original.candles).body);
}
void MarketCandlesViewTest::d3DetailsDisclosurePreservesTextAndShortStatus(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,900);view.show();
    expandAppliedIndicators(view);
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QTRY_VERIFY(view.chartStudies().similarity.available);
    auto* information=view.findChild<InfoButton*>("marketCandlesInformation");QVERIFY(information);
    auto* similarity=view.findChild<QLabel*>("chartStudySimilarityEvidence");QVERIFY(!similarity->text().isEmpty());const auto original=similarity->text();
    auto* legend=view.findChild<QWidget*>("chartStudyLegend");QVERIFY(legend->isVisible());QVERIFY(similarity->isHidden());
    const auto originalStudies=view.chartStudies();information->click();QVERIFY(information->informationOpen());
    auto* body=information->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(body);QVERIFY(body->toPlainText().contains("ADX(14)"));QVERIFY(body->toPlainText().contains(QStringLiteral("상관")));
    QVERIFY(!body->toPlainText().contains("https://"));QCOMPARE(view.chartStudies().similarity.samples,originalStudies.similarity.samples);
    preferences.setVisible("volume",false);QCOMPARE(similarity->text(),original);QVERIFY(similarity->isHidden());
    information->findChild<QDialog*>("infoDialog")->close();QVERIFY(!information->informationOpen());QVERIFY(legend->isVisible());

}
void MarketCandlesViewTest::movementColorsRepaintObservedCandlesWithoutRefreshingSource_data(){
    QTest::addColumn<bool>("dark"); QTest::newRow("light")<<false; QTest::newRow("dark")<<true;
}
void MarketCandlesViewTest::movementColorsRepaintObservedCandlesWithoutRefreshingSource(){
    QFETCH(bool,dark);KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    QWidget owner;MovementColorPolicy colors(&owner);StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source,&owner);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);owner.resize(1000,750);view.resize(1000,750);owner.show();view.show();QVERIFY(transport.token);
    auto rows=syntheticStudyRows(6);for(int i=0;i<rows.size();++i){rows[i].open="100";rows[i].close=i%2?"99":"101";rows[i].high="102";rows[i].low="98";}
    emit transport.finished(transport.token,syntheticStudyResponse(rows));QVERIFY(source.series().available);const auto token=transport.token;const auto receipt=source.series().receivedAtMs;const auto sourceBody=source.series().candles;
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto count=[](const QImage& image,const QColor& color){int n=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)n+=image.pixelColor(x,y)==color;return n;};
    const auto before=chart->grab().toImage();QVERIFY(count(before,themeColors().up)>50);QVERIFY(count(before,themeColors().down)>50);
    colors.setScheme(MovementScheme::KoreanRedBlue);QCoreApplication::processEvents();const auto after=chart->grab().toImage();QVERIFY(count(after,themeColors().koreanDown)>50);QCOMPARE(count(after,themeColors().up),0);
    colors.setScheme(MovementScheme::GreenRed);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Light:ThemeMode::Dark);QCoreApplication::processEvents();
    const auto opposite=chart->grab().toImage();QVERIFY(count(opposite,themeColors().up)>50);QVERIFY(count(opposite,themeColors().down)>50);
    QCOMPARE(opposite.pixelColor(0,0),themeColors().bg);
    QCOMPARE(transport.token,token);QCOMPARE(source.series().receivedAtMs,receipt);const auto afterRows=source.series().candles;QCOMPARE(afterRows.size(),sourceBody.size());
    for(int i=0;i<sourceBody.size();++i){QCOMPARE(afterRows[i].startsAtMs,sourceBody[i].startsAtMs);QCOMPARE(afterRows[i].open,sourceBody[i].open);QCOMPARE(afterRows[i].close,sourceBody[i].close);QCOMPARE(afterRows[i].volume,sourceBody[i].volume);}
}
void MarketCandlesViewTest::panningOlderDoesNotExtendTheViewportToOffscreenDrawingAnchors(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();
    auto rows=syntheticStudyRows(200);for(auto& row:rows){row.open=row.close="100";row.high="101";row.low="99";}
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));QCOMPARE(source.series().candles.size(),200);
    ChartDrawingFrame frame;frame.identity=drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;frame.asOfMs=StudyNow;frame.contextGeneration=drawings.contextGeneration();
    for(const auto& row:source.series().candles)frame.bars.append({row,row.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const QList<ChartDrawingPoint> anchors{{rows[190].startsAtMs,"100"},{rows[198].startsAtMs,"100"}};QVERIFY(!drawings.create(ChartDrawingKind::Trend,anchors,frame).isEmpty());
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto saved=drawings.publicState();
    const auto bluePixels=[](const QImage& image){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==themeColors().studyBlue)++count;return count;};
    const auto latest=chart->grab().toImage();QVERIFY2(bluePixels(latest)>0,"The actual latest source viewport must first show the saved public trend");
    chart->setFocus();QTest::keyClick(chart,Qt::Key_Left);QTest::keyClick(chart,Qt::Key_Left);
    const auto past=chart->grab().toImage();
    if(const auto destination=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!destination.isEmpty()){QVERIFY(QDir().mkpath(destination));QVERIFY(latest.save(QDir(destination).filePath("c8-pan-latest.png")));QVERIFY(past.save(QDir(destination).filePath("c8-pan-older.png")));}
    QCOMPARE(drawings.publicState(),saved);QCOMPARE(drawings.drawings().first().anchors,anchors);QCOMPARE(source.series().candles.last().startsAtMs,rows.last().startsAtMs);QCOMPARE(source.series().candles.last().close,QString("100"));
    QCOMPARE(bluePixels(past),0); // Older20 means actual bars120..179; anchors190/198 are outside that viewport.
    QCOMPARE(visibleBodyCenters(chart).size(),60);
    const auto greenColumns=[](const QImage& image){QList<bool> columns;for(int x=0;x<image.width();++x){bool green=false;for(int y=0;y<image.height();++y)green|=image.pixelColor(x,y)==themeColors().text3;columns.append(green);}return columns;};
    const auto candlePositions=greenColumns(past);drawings.setVisible(false);QCOMPARE(greenColumns(chart->grab().toImage()),candlePositions);drawings.setVisible(true);QCOMPARE(drawings.publicState(),saved);
}
void MarketCandlesViewTest::drawingGesturesRejectActualFormingAndUnobservedGapSlots(){
    qint64 now=StudyNow;StudyTransport transport;auto socket=std::make_unique<StudySocket>();auto* wire=socket.get();MarketCandles source(&transport,std::move(socket),[&]{return now;});ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();
    QVERIFY(transport.token);QVERIFY(wire->generation);emit wire->connected(wire->generation);auto rows=flatDrawingRows();rows.removeAt(2);emit transport.finished(transport.token,syntheticStudyResponse(rows));QCOMPARE(source.series().candles.size(),5);
    MarketCandle forming{now/60000*60000,now,"100","101","99","100","2"};auto frame=syntheticStudyWire(forming);frame.remove("market");frame.remove("unit");frame["code"]="KRW-SYN";frame["type"]="candle.1m";frame["stream_type"]="REALTIME";
    emit wire->received(wire->generation,QJsonDocument(frame).toJson());QVERIFY(source.series().forming.has_value());
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");const auto centers=visibleBodyCenters(chart);QCOMPARE(centers.size(),5);
    int largest=0;double step=std::numeric_limits<double>::infinity();for(int i=1;i<centers.size();++i){const auto distance=centers[i].x()-centers[i-1].x();if(distance>centers[largest+1].x()-centers[largest].x())largest=i-1;step=qMin(step,double(distance));}
    const QPoint gap((centers[largest].x()+centers[largest+1].x())/2,centers.first().y()),formingPixel(centers.last().x()+qRound(step),centers.last().y());QVERIFY(chart->rect().contains(formingPixel));
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,gap);QVERIFY(drawings.drawings().isEmpty());
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,formingPixel);QVERIFY(drawings.drawings().isEmpty());QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers.first());QCOMPARE(drawings.drawings().size(),1);
    QCOMPARE(drawings.drawings().first().anchors.first().atMs,rows.first().startsAtMs);QCOMPARE(source.series().candles.size(),5);QCOMPARE(source.series().forming->startsAtMs,forming.startsAtMs);
}
void MarketCandlesViewTest::drawingDraftAndHandleReleaseCannotCrossAContextGeneration(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));auto* chart=view.findChild<QWidget*>("marketCandlesChart");auto centers=visibleBodyCenters(chart);QCOMPARE(centers.size(),6);
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingMagnet"),Qt::LeftButton);QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_trend"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers.first());QVERIFY(drawings.drawings().isEmpty());
    view.setSelection("coin:upbit:KRW-SYN",300000);QJsonArray target;const auto end=StudyNow/300000*300000;
    for(int i=2;i>0;--i){auto wire=syntheticStudyWire({end-i*300000,end-i*300000+1000,"100","101","99","100","5"});wire["unit"]="5";target.append(wire);}HttpResult response;response.status=200;response.body=QJsonDocument(target).toJson();emit transport.finished(transport.token,response);QVERIFY(source.series().available);
    centers=visibleBodyCenters(chart);QCOMPARE(centers.size(),2);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers.last());QVERIFY(drawings.drawings().isEmpty());QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers.first());QCOMPARE(drawings.drawings().size(),1);
    const auto anchors=drawings.drawings().first().anchors;const auto state=drawings.publicState();QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_crosshair"),Qt::LeftButton);
    const auto blueInk=[](const QImage& image){QImage mask(image.size(),QImage::Format_Grayscale8);mask.fill(0);
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==themeColors().studyBlue)mask.setPixelColor(x,y,Qt::white);return mask;};
    const auto beforeDrag=blueInk(chart->grab().toImage());const auto beforeDragState=drawings.publicState();
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,centers.last());QTest::mouseMove(chart,centers.last()+QPoint(0,18));
    QVERIFY2(blueInk(chart->grab().toImage())!=beforeDrag,"The actual selected handle must enter a visible preview drag before context revocation");QCOMPARE(drawings.publicState(),beforeDragState);
    QVERIFY(drawings.setContext({{"coin","upbit","KRW-ALT"},"KRW",true},300000));QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,centers.last()+QPoint(0,18));
    QVERIFY(drawings.setContext({{"coin","upbit","KRW-SYN"},"KRW",true},300000));QCOMPARE(drawings.drawings().first().anchors,anchors);QCOMPARE(drawings.publicState(),state);
    auto* replacement=new ChartDrawings;view.setDrawingModel(replacement);QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_trend"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers.first());delete replacement;
    QVERIFY(!view.findChild<QToolButton*>("chartDrawingTool_trend")->isEnabled());view.setSelection("coin:upbit:KRW-SYN",60000);emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));QVERIFY(source.series().available);QCOMPARE(drawings.publicState(),state);
}
void MarketCandlesViewTest::hiddenShapesLeaveTheChartAndLockedShapesDoNotDrag(){
    DrawingFixture f;QVERIFY(f.source.series().available);auto* chart=f.chart();QVERIFY(chart);const auto centers=visibleBodyCenters(chart);QCOMPARE(centers.size(),6);
    QTest::mouseClick(f.tool("chartDrawingTool_trend"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[1]);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,centers[3]);
    QCOMPARE(f.drawings.drawings().size(),1);const auto trend=f.drawings.drawings().first();
    const auto blueInk=[chart]{const auto image=chart->grab().toImage();QImage mask(image.size(),QImage::Format_Grayscale8);mask.fill(0);
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==themeColors().studyBlue)mask.setPixelColor(x,y,Qt::white);return mask;};
    const auto inked=[](const QImage& mask){for(int y=0;y<mask.height();++y)for(int x=0;x<mask.width();++x)if(mask.pixelColor(x,y)!=Qt::black)return true;return false;};
    QVERIFY2(inked(blueInk()),"The placed trend must first be drawn");
    const auto bar=centers[3].x()-centers[2].x();QVERIFY(bar>4);
    const QPoint grab((centers[1].x()+centers[3].x())/2,centers[1].y());const QPoint drop=grab+QPoint(bar,-40);
    // A hidden shape draws nothing and cannot be grabbed: dragging where it lies leaves it in place.
    QVERIFY(f.drawings.setDrawingVisible(trend.id,false));QVERIFY2(!inked(blueInk()),"A hidden shape must not be drawn");
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,grab);QTest::mouseMove(chart,grab+QPoint(bar/2,-20));QTest::mouseMove(chart,drop);QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,drop);
    QCOMPARE(f.drawings.drawings().first().anchors,trend.anchors);
    // A locked shape is drawn and selectable, but pressing it starts no drag preview and the release moves nothing.
    QVERIFY(f.drawings.setDrawingVisible(trend.id,true));QVERIFY(f.drawings.setDrawingLocked(trend.id,true));
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,grab);const auto pressed=blueInk();QVERIFY(inked(pressed));
    QTest::mouseMove(chart,grab+QPoint(bar/2,-20));QTest::mouseMove(chart,drop);
    QVERIFY2(blueInk()==pressed,"A locked shape must not show a drag preview");
    QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,drop);
    QCOMPARE(f.drawings.drawings().first().anchors,trend.anchors);QVERIFY(f.drawings.drawings().first().locked);
}
void MarketCandlesViewTest::unconnectedStockCannotCreateDrawingAnchors(){
    ChartDrawings drawings;drawings.setVerifiedInstruments({{"stock","KR","005930"}});
    MarketCandlesView view(nullptr);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("stock:KR:005930",86400000);view.resize(1050,950);view.show();
    QVERIFY(view.findChild<QWidget*>("marketCandlesChart")->isHidden());
    for(const auto* name:{"chartDrawingTool_horizontal","chartDrawingTool_trend","chartDrawingTool_measure"})QVERIFY(!view.findChild<QToolButton*>(name)->isEnabled());QVERIFY(drawings.drawings().isEmpty());
}
void MarketCandlesViewTest::drawingToolsMagnetLockHideAndClearUseOnlyObservedOhlc(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,1000);view.show();
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));QVERIFY(source.series().available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);auto points=visibleBodyPoints(chart);QCOMPARE(points.size(),2);
    view.setPriceLines({{"upbit","synthetic-only-ohlc-exclusion","SYN","KRW","100.001","https://api.upbit.com/v1/accounts",QDateTime::currentMSecsSinceEpoch()}});
    QCOMPARE(view.priceLines().size(),1);QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingMagnet"),Qt::LeftButton);
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,points.first()+QPoint(0,1));
    QCOMPARE(drawings.drawings().size(),1);QCOMPARE(drawings.drawings().first().anchors.first().price,QString("100"));
    auto* lock=view.findChild<QToolButton*>("chartDrawingLocked");auto* clear=view.findChild<QToolButton*>("chartDrawingClear");QTest::mouseClick(lock,Qt::LeftButton);QVERIFY(drawings.locked());QVERIFY(!clear->isEnabled());
    const auto locked=drawings.publicState();QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,chart->rect().center());QCOMPARE(drawings.publicState(),locked);
    const auto shown=chart->grab().toImage();QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingVisible"),Qt::LeftButton);QVERIFY(!drawings.visible());QVERIFY(shown!=chart->grab().toImage());
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingVisible"),Qt::LeftButton);QTest::mouseClick(lock,Qt::LeftButton);QTest::mouseClick(clear,Qt::LeftButton);QVERIFY(drawings.drawings().isEmpty());view.clearPrivate();
    points=visibleBodyPoints(chart);QCOMPARE(points.size(),2);
    for(const auto pair:{qMakePair("chartDrawingTool_trend",ChartDrawingKind::Trend),qMakePair("chartDrawingTool_measure",ChartDrawingKind::Measure)}){
        QTest::mouseClick(view.findChild<QToolButton*>(pair.first),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,points.first());QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,points.last());
        QVERIFY(!drawings.drawings().isEmpty());QCOMPARE(drawings.drawings().last().kind,pair.second);QCOMPARE(drawings.drawings().last().anchors.size(),2);}
    QCOMPARE(drawings.drawings().size(),2);QTest::keyClick(chart,Qt::Key_Escape);QVERIFY(view.findChild<QToolButton*>("chartDrawingTool_crosshair")->isChecked());
}
void MarketCandlesViewTest::privateDrawingLockRejectsLateSourceRefreshUntilExplicitSelection(){
    qint64 now=StudyNow;StudyTransport transport;auto socket=std::make_unique<StudySocket>();auto* wire=socket.get();MarketCandles source(&transport,std::move(socket),[&]{return now;});
    ChartDrawings drawings;const auto empty=drawings.publicState();ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(false);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();
    QVERIFY(transport.token);QVERIFY(wire->generation);emit wire->connected(wire->generation);emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");const auto points=visibleBodyPoints(chart);QCOMPARE(points.size(),2);
    auto* horizontal=view.findChild<QToolButton*>("chartDrawingTool_horizontal");QTest::mouseClick(horizontal,Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,points.first());
    QCOMPARE(drawings.drawings().size(),1);QCOMPARE(drawings.publicState(),empty);QVERIFY(!drawings.context().publicPersistenceAllowed);
    drawings.clearPrivate();view.clearPrivateDrawingContext();QVERIFY(drawings.drawings().isEmpty());QCOMPARE(drawings.publicState(),empty);
    MarketCandle forming{now/60000*60000,now,"100","101","99","100","2"};auto frame=syntheticStudyWire(forming);frame.remove("market");frame.remove("unit");frame["code"]="KRW-SYN";frame["type"]="candle.1m";frame["stream_type"]="REALTIME";
    emit wire->received(wire->generation,QJsonDocument(frame).toJson());QVERIFY(source.series().forming.has_value());QVERIFY(drawings.context().identity.key()=="::");QVERIFY(!horizontal->isEnabled());
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,chart->rect().center());QVERIFY(drawings.drawings().isEmpty());QCOMPARE(drawings.publicState(),empty);
    view.setDrawingPublicContext(true);QVERIFY(horizontal->isEnabled());QTest::mouseClick(horizontal,Qt::LeftButton);const auto publicPoints=visibleBodyPoints(chart);QCOMPARE(publicPoints.size(),2);
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,publicPoints.first());QCOMPARE(drawings.drawings().size(),1);QVERIFY(drawings.context().publicPersistenceAllowed);QVERIFY(drawings.publicState()!=empty);
}
void MarketCandlesViewTest::horizontalDrawingUiKeepsItsOwnerAndExplicitAlertInterval(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};drawings.setPublicLists({{"a",{identity}},{"b",{identity}}});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setDrawingAlertLists({{"a","합성 A"},{"b","합성 B"}},{});view.setSelection(identity.key(),60000);view.resize(1050,1100);view.show();
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(flatDrawingRows()));auto* chart=view.findChild<QWidget*>("marketCandlesChart");const auto points=visibleBodyPoints(chart);QCOMPARE(points.size(),2);
    QTest::mouseClick(view.findChild<QToolButton*>("chartDrawingTool_horizontal"),Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,points.first());QCOMPARE(drawings.drawings().size(),1);
    auto* owner=view.findChild<QComboBox*>("chartDrawingLineOwner");auto* enabled=view.findChild<QCheckBox*>("chartDrawingLineEnabled");auto* status=view.findChild<QLabel*>("chartDrawingLineStatus");QVERIFY(owner&&enabled&&status);QCOMPARE(owner->count(),2);QVERIFY(status->text().contains(QStringLiteral("리스트 수평선 알림 꺼짐")));
    owner->setCurrentIndex(owner->findData("b"));QSignalSpy settings(&view,&MarketCandlesView::drawingAlertSettingsRequested);QTest::mouseClick(view.findChild<QPushButton*>("chartDrawingLineSettings"),Qt::LeftButton);QCOMPARE(settings.size(),1);QCOMPARE(settings.first().first().toString(),QString("b"));
    enabled->setFocus();QTest::keyClick(enabled,Qt::Key_Space);QVERIFY(drawings.drawings().first().alert.enabled);QCOMPARE(drawings.drawings().first().alert.listId,QString("b"));QCOMPARE(drawings.drawings().first().alert.intervalMs,qint64(60000));
    view.setDrawingAlertLists({{"a","합성 A"},{"b","합성 B"}},{"b"});QVERIFY(!status->text().contains(QStringLiteral("리스트 수평선 알림 꺼짐")));
    QVERIFY(saveDrawingPreview(view,"c8-line-alert"));
    view.setSelection(identity.key(),300000);QCOMPARE(drawings.drawings().first().alert.intervalMs,qint64(60000));QCOMPARE(drawings.drawings().first().alert.listId,QString("b"));
}
void MarketCandlesViewTest::drawingToolbarUsesActualClosedSourceAndPublicModel(){
    qint64 now=StudyNow;StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,900);view.show();QVERIFY(transport.token);
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(6)));QVERIFY(source.series().available);
    auto* toolbar=view.findChild<QWidget*>("chartDrawingToolbar");QVERIFY2(toolbar,"Drawing tools must operate on the actual displayed closed-source series");
    for(const auto* name:{"chartDrawingTool_crosshair","chartDrawingTool_trend","chartDrawingTool_horizontal","chartDrawingTool_measure", "chartDrawingMagnet","chartDrawingVisible","chartDrawingLocked","chartDrawingClear"}){
        auto* button=view.findChild<QToolButton*>(name);QVERIFY2(button,name);QVERIFY(button->isVisible()||view.findChild<QToolButton*>("chartDrawingMore")->isVisible());}
    QCOMPARE(drawings.context().identity.key(),QString("coin:upbit:KRW-SYN"));QCOMPARE(drawings.context().currency,QString("KRW"));QVERIFY(drawings.context().publicPersistenceAllowed);
    auto* horizontal=view.findChild<QToolButton*>("chartDrawingTool_horizontal");QTest::mouseClick(horizontal,Qt::LeftButton);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart&&chart->isVisible());QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,chart->rect().center());
    QCOMPARE(drawings.drawings().size(),1);const auto drawing=drawings.drawings().first();QCOMPARE(drawing.kind,ChartDrawingKind::Horizontal);QCOMPARE(drawing.anchors.size(),1);
    bool actualClosed=false;for(const auto& row:source.series().candles)actualClosed|=row.startsAtMs==drawing.anchors.first().atMs;QVERIFY(actualClosed);
    QVERIFY(!drawings.publicState().isEmpty());QVERIFY(!drawings.publicState().contains("pixel"));QVERIFY(!drawings.publicState().contains("account"));
}
void MarketCandlesViewTest::drawingGeometryStaysAtOriginalUtcAndPriceAcrossZoomResizeAndTimeframes(){
    qint64 now=StudyNow;StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,900);view.show();QVERIFY(transport.token);
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(6)));QVERIFY(source.series().available);
    QCOMPARE(drawings.context().identity.key(),QString("coin:upbit:KRW-SYN"));
    ChartDrawingFrame frame;frame.identity=drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;frame.asOfMs=now;frame.contextGeneration=drawings.contextGeneration();
    const auto series=source.series();for(const auto& row:series.candles)frame.bars.append({row,row.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const QList<ChartDrawingPoint> anchors{{series.candles[0].startsAtMs,series.candles[0].close},{series.candles[3].startsAtMs,series.candles[3].close}};
    const auto id=drawings.create(ChartDrawingKind::Trend,anchors,frame);QVERIFY(!id.isEmpty());const auto saved=drawings.publicState();QVERIFY(!saved.isEmpty());
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QTest::keyClick(chart,Qt::Key_Plus);view.resize(820,1050);QCoreApplication::processEvents();QCOMPARE(drawings.publicState(),saved);
    view.setSelection("coin:upbit:KRW-SYN",300000);QVERIFY(transport.token);QJsonArray target;
    const auto end=now/300000*300000;
    for(int i=2;i>0;--i){MarketCandle row{end-i*300000,end-i*300000+1000,"99","120","90","100","50"};auto wire=syntheticStudyWire(row);wire["unit"]="5";target.append(wire);}
    HttpResult response;response.status=200;response.body=QJsonDocument(target).toJson();emit transport.finished(transport.token,response);QVERIFY(source.series().available);
    QCOMPARE(drawings.intervalMs(),qint64(300000));QCOMPARE(drawings.drawings().size(),1);QCOMPARE(drawings.drawings().first().id,id);QCOMPARE(drawings.drawings().first().anchors,anchors);QCOMPARE(drawings.publicState(),saved);
    QCoreApplication::processEvents();const auto shown=chart->grab().toImage();drawings.setVisible(false);QVERIFY2(shown!=chart->grab().toImage(),"Saved original UTC/price anchors must actually render inside the observed larger-timeframe bars");
    drawings.setVisible(true);QCOMPARE(drawings.drawings().first().anchors,anchors);QCOMPARE(drawings.publicState(),saved);
    QVERIFY(saveDrawingPreview(view,"c8-drawings"));
}
void MarketCandlesViewTest::lateDrawingAnchorsStayVisibleAfterTimeframeChange_data(){
    QTest::addColumn<qint64>("interval");QTest::addColumn<bool>("dark");
    QTest::newRow("15m-light")<<qint64(900000)<<false;
    QTest::newRow("15m-dark")<<qint64(900000)<<true;
    QTest::newRow("1h-light")<<qint64(3600000)<<false;
    QTest::newRow("1h-dark")<<qint64(3600000)<<true;
}
void MarketCandlesViewTest::lateDrawingAnchorsStayVisibleAfterTimeframeChange(){
    QFETCH(qint64,interval);QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    const auto end=StudyNow/interval*interval,now=end+10000;
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);
    view.resize(1050,950);view.show();view.setSelection("coin:upbit:KRW-SYN",60000);
    QList<MarketCandle> minutes;for(int i=60;i>0;--i){const auto at=end-i*60000;minutes.append({at,at+1000,"100","110","90","100","2"});}
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(minutes));QVERIFY(source.series().available);
    ChartDrawingFrame frame;frame.identity=drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;frame.asOfMs=now;frame.contextGeneration=drawings.contextGeneration();
    for(const auto& row:minutes)frame.bars.append({row,row.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const QList<ChartDrawingPoint> anchors{{end-5*60000,"99"},{end-60000,"101"}};
    QVERIFY(!drawings.create(ChartDrawingKind::Trend,anchors,frame).isEmpty());const auto saved=drawings.publicState();
    view.setSelection("coin:upbit:KRW-SYN",interval);QJsonArray larger;
    for(int i=200;i>0;--i){const auto at=end-i*interval;auto row=syntheticStudyWire({at,at+1000,"100","110","90","100","2"});row["unit"]=QString::number(interval/60000);larger.append(row);}
    HttpResult response;response.status=200;response.body=QJsonDocument(larger).toJson();emit transport.finished(transport.token,response);
    QVERIFY(source.series().available);QVERIFY(!source.series().forming);QCOMPARE(drawings.drawings().first().anchors,anchors);QCOMPARE(drawings.publicState(),saved);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QCoreApplication::processEvents();
    const auto bluePixels=[](const QImage& image){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)count+=image.pixelColor(x,y)==themeColors().studyBlue;return count;};
    QVERIFY2(bluePixels(chart->grab().toImage())>0,"Both saved anchors lie in the second half of the last completed larger bar and must remain visible without a forming bar or projection");
    drawings.setVisible(false);QCOMPARE(bluePixels(chart->grab().toImage()),0);drawings.setVisible(true);
    QTest::keyClick(chart,Qt::Key_Left);QTest::keyClick(chart,Qt::Key_Left);
    const auto past=chart->grab().toImage();QCOMPARE(bluePixels(past),0);
    const auto first=view.visibleViewport().fromMs;const auto x=chart->property("viewportFirstBarX");const auto span=chart->property("viewportSpanSlots");
    drawings.setVisible(false);drawings.setVisible(true);
    QCOMPARE(view.visibleViewport().fromMs,first);QCOMPARE(chart->property("viewportFirstBarX"),x);QCOMPARE(chart->property("viewportSpanSlots"),span);QCOMPARE(chart->grab().toImage(),past);
    view.returnToLive();QVERIFY(bluePixels(chart->grab().toImage())>0);
    QCOMPARE(drawings.publicState(),saved);
}
void MarketCandlesViewTest::observedOiHistoryKeepsAdjacentEndsQuantityValueAndMissingSourceClock(){
    qint64 now=QDateTime::currentMSecsSinceEpoch()-5000;
    class Http final:public TradingTransport{public:QList<RequestSpec> requests;QList<quint64> tokens;void abort()override{}
        void send(quint64 token,const RequestSpec& request)override{QVERIFY(CoinFlowSource::isRequest(request));QVERIFY(!request.authenticated);requests.append(request);tokens.append(token);}} http;
    CoinFlowSource source(&http,{},{},nullptr,[&]{return now;});
    const auto instrument=[&](bool derivative){CoinFlowInstrument i;i.venue=derivative?CoinFlowVenue::BinanceUsdM:CoinFlowVenue::BinanceSpot;i.grain=derivative?CoinFlowGrain::Futures100msAggregate:CoinFlowGrain::SpotAggregate;
        i.identity=derivative?"usdm_futures:binance:SYNUSDT":"coin:binance:SYNUSDT";i.symbol="SYNUSDT";i.baseAsset="SYN";i.quoteAsset="USDT";i.status="TRADING";i.verified=true;
        if(derivative){i.contractType="PERPETUAL";i.marginAsset="USDT";i.underlyingType="COIN";}i.evidence={"합성 검증된 공개 metadata",derivative?"https://fapi.binance.com/fapi/v1/exchangeInfo":"https://api.binance.com/api/v3/exchangeInfo",0,now,now};return i;};
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument(false)});source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{instrument(true)});source.setSelection("coin:binance:SYNUSDT");source.setActive(true);
    int handled=0;qint64 previousEnd=0,currentEnd=0,oiReceived=0;
    for(int step=0;step<6;++step){if(handled<http.requests.size()){HttpResult response;response.status=200;
            if(http.requests[handled].url.path().endsWith("openInterestHist")){currentEnd=now/300000*300000;previousEnd=currentEnd-300000;oiReceived=now;
                response.body=QJsonDocument(QJsonArray{QJsonObject{{"symbol","SYNUSDT"},{"sumOpenInterest","100"},{"sumOpenInterestValue","1000"},{"timestamp",QString::number(previousEnd)}},
                    QJsonObject{{"symbol","SYNUSDT"},{"sumOpenInterest","102"},{"sumOpenInterestValue","1100"},{"timestamp",QString::number(currentEnd)}}}).toJson(QJsonDocument::Compact);
            }else response.body="[]";emit http.finished(http.tokens[handled++],response);}
        now+=500;source.pump();}
    QCOMPARE(handled,3);QVERIFY(oiReceived>0);
    MarketCandleSeries series;series.available=true;series.identity="coin:binance:SYNUSDT";series.asset="SYN";series.currency="USDT";series.intervalMs=60000;series.receivedAtMs=now;
    series.sourceUrl=MarketCandles::request(series.identity,60000).url.toString();series.candles.append({now/60000*60000-60000,0,"100","101","99","100","10"});source.setMinuteSeries(series);
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setCoinFlowSource(&source);view.setSelection(series.identity,60000);view.resize(1000,900);view.show();
    QTRY_VERIFY(view.coinFlowStudies().openInterest.state.available);const auto result=view.coinFlowStudies();QVERIFY(result.openInterest.state.available);QCOMPARE(result.openInterest.previousEndMs,previousEnd);QCOMPARE(result.openInterest.currentEndMs,currentEnd);
    QCOMPARE(result.openInterest.previousQuantity,QString("100"));QCOMPARE(result.openInterest.currentQuantity,QString("102"));QCOMPARE(result.openInterest.previousValue,QString("1000"));QCOMPARE(result.openInterest.currentValue,QString("1100"));
    QCOMPARE(result.openInterest.quantityChangePercent.value_or(-1),2.0);QCOMPARE(result.openInterest.valueChangePercent.value_or(-1),10.0);
    QVERIFY(!result.openInterest.state.evidence.isEmpty());QCOMPARE(result.openInterest.state.evidence.last().sourceTimeMs,qint64(0));QCOMPARE(result.openInterest.state.evidence.last().receivedAtMs,oiReceived);
    auto* details=view.findChild<QLabel*>("chartCoinFlowDetails");QVERIFY(displayedText(details).contains(QStringLiteral("OI 수량 변화 2.00%")));QVERIFY(displayedText(details).contains(QStringLiteral("원문 value 변화 10.00%")));
    QVERIFY(displayedText(details).contains(QStringLiteral("통화 추정 없음")));QVERIFY(displayedText(details).contains(QStringLiteral("원천 시각 미제공")));
    const auto originalDetails=displayedText(details);details->setSelection(0,originalDetails.size());
    QCOMPARE(QString(details->selectedText()).replace(QChar::LineSeparator,'\n').replace(QChar::ParagraphSeparator,'\n'),originalDetails);details->setSelection(0,0);
    auto* chip=view.findChild<QLabel*>("chartStudyStatus_whales");QCOMPARE(chip->text(),QStringLiteral("Whales & Reversal"));
    view.clearPrivate();QCOMPARE(view.coinFlowStudies().openInterest.currentQuantity,QString("102"));QCOMPARE(view.coinFlowStudies().openInterest.state.evidence.last().receivedAtMs,oiReceived);
}
void MarketCandlesViewTest::observedBinanceSourceRendersSeparatePopulationsCvdAndLossyLiquidations(){
    QTemporaryDir profileDirectory;QVERIFY(profileDirectory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profileDirectory.filePath("settings"));
    class UnavailableTray final:public BackgroundTray{public:bool available()const override{return false;}bool visible()const override{return false;}
        void setMenu(QMenu*)override{}void show()override{}void hide()override{}} tray;
    Vault themeVault(profileDirectory.filePath("synthetic-coin-flow-theme-vault"));AccountModel themeAccounts(&themeVault);
    LocalWindow themeWindow(themeVault,themeAccounts,nullptr,&tray);
    const auto end=QDateTime::currentMSecsSinceEpoch()/60000*60000;const auto base=end-180000;qint64 now=base;
    class Http final:public TradingTransport{public:QList<RequestSpec> requests;QList<quint64> tokens;
        void abort()override{}void send(quint64 token,const RequestSpec& request)override{QVERIFY(CoinFlowSource::isRequest(request));QVERIFY(!request.authenticated);QVERIFY(!request.headers.contains("Authorization"));requests.append(request);tokens.append(token);}} http;
    auto spotSocket=std::make_unique<StudySocket>(),futuresSocket=std::make_unique<StudySocket>();auto* spot=spotSocket.get();auto* futures=futuresSocket.get();
    CoinFlowSource source(&http,std::move(spotSocket),std::move(futuresSocket),nullptr,[&]{return now;});
    const auto instrument=[&](bool derivative){CoinFlowInstrument value;value.venue=derivative?CoinFlowVenue::BinanceUsdM:CoinFlowVenue::BinanceSpot;
        value.grain=derivative?CoinFlowGrain::Futures100msAggregate:CoinFlowGrain::SpotAggregate;value.identity=derivative?"usdm_futures:binance:SYNUSDT":"coin:binance:SYNUSDT";
        value.symbol="SYNUSDT";value.baseAsset="SYN";value.quoteAsset="USDT";value.status="TRADING";value.verified=true;
        if(derivative){value.marginAsset="USDT";value.contractType="PERPETUAL";value.underlyingType="COIN";}
        value.evidence={"합성 공식 공개 메타데이터",derivative?"https://fapi.binance.com/fapi/v1/exchangeInfo":"https://api.binance.com/api/v3/exchangeInfo",0,now,now};return value;};
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument(false)});source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{instrument(true)});
    source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QVERIFY(spot->generation);QVERIFY(futures->generation);
    emit spot->connected(spot->generation);emit futures->connected(futures->generation);
    int handled=0;for(int step=0;step<10;++step){if(handled<http.requests.size()){
            const auto& request=http.requests[handled];QVERIFY(request.url.path().endsWith("aggTrades")||request.url.path().endsWith("openInterestHist"));
            HttpResult empty;empty.status=200;empty.body="[]";emit http.finished(http.tokens[handled++],empty);}
        now+=500;source.pump();}
    QCOMPARE(handled,3); // Spot seed, futures seed and public OI; no account/controller jobs.
    MarketCandleSeries seed;seed.available=true;seed.identity="coin:binance:SYNUSDT";seed.asset="SYN";seed.currency="USDT";seed.intervalMs=60000;
    seed.sourceUrl=MarketCandles::request(seed.identity,60000).url.toString();seed.receivedAtMs=now;
    for(int i=21;i>0;--i)seed.candles.append({base-i*60000,0,"99","101","99","100","10"});source.setMinuteSeries(seed);
    const auto minute=end-60000;now=minute;source.pump();
    const auto trade=[&](int i,qint64 at,bool derivative){const bool last=i==200;
        QJsonObject value{{"e","aggTrade"},{"E",QString::number(at+1)},{"s","SYNUSDT"},{"a",QString::number(9007199254741000LL+i)},
            {"p","100"},{"q",last?(derivative?"3":"100"):(derivative?"2":"1")},{"f",QString::number(9007199254742000LL+i)},{"l",QString::number(9007199254742000LL+i)},
            {"T",QString::number(at)},{"m",derivative?(i>=100):(i>=150&&!last)}};
        if(derivative){value["nq"]=last?"1":"2";return QJsonDocument(QJsonObject{{"stream","synusdt@aggTrade"},{"data",value}}).toJson(QJsonDocument::Compact);}
        return QJsonDocument(value).toJson(QJsonDocument::Compact);};
    for(int i=0;i<=200;++i){const auto at=minute+(i==200?59000:1000+i*200);now=at+2;
        emit spot->received(spot->generation,trade(i,at,false));emit futures->received(futures->generation,trade(i,at,true));}
    const auto liquidationAt=end-500;now=liquidationAt+2;
    const QJsonObject order{{"s","SYNUSDT"},{"S","SELL"},{"p","100"},{"ap","99"},{"q","5"},{"l","1"},{"z","2"},{"X","PARTIALLY_FILLED"},{"T",QString::number(liquidationAt)}};
    emit futures->received(futures->generation,QJsonDocument(QJsonObject{{"stream","synusdt@forceOrder"},{"data",QJsonObject{{"e","forceOrder"},{"E",QString::number(liquidationAt+1)},{"o",order}}}}).toJson(QJsonDocument::Compact));
    now=end;source.pump();auto completed=seed;completed.receivedAtMs=now;for(int i=0;i<3;++i)completed.candles.append({base+i*60000,0,"99","101","99","100","10"});source.setMinuteSeries(completed);
    const auto raw=source.snapshot();QCOMPARE(raw.primary.trades.size(),201);QVERIFY(raw.companion.has_value());QCOMPARE(raw.companion->trades.size(),201);
    QVERIFY(raw.companion->trades.last().normalQuantity.has_value());QCOMPARE(*raw.companion->trades.last().normalQuantity,QString("1"));QCOMPARE(raw.liquidations.size(),1);
    StudyTransport chartHttp;MarketCandles model(&chartHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});MarketInstrument chartInstrument;
    chartInstrument.symbol="SYNUSDT";chartInstrument.baseAsset="SYN";chartInstrument.quoteAsset="USDT";chartInstrument.market="spot";chartInstrument.status="TRADING";chartInstrument.tradable=true;model.setInstruments("binance",{chartInstrument});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume"})preferences.setEnabled(id,false);
    MarketCandlesView view(&model);view.setStyleSheet(themeWindow.styleSheet());view.setIndicatorPreferences(&preferences);view.setCoinFlowSource(&source);view.setSelection(completed.identity,60000);view.resize(1100,1100);view.show();QVERIFY(chartHttp.token);
    QJsonArray candles;for(const auto& row:completed.candles)candles.append(QJsonArray{row.startsAtMs,row.open,row.high,row.low,row.close,row.volume,row.startsAtMs+59999,"1000",10,"5","500","0"});
    HttpResult response;response.status=200;response.body=QJsonDocument(candles).toJson(QJsonDocument::Compact);emit chartHttp.finished(chartHttp.token,response);QVERIFY(model.series().available);
    QTRY_VERIFY(view.coinFlowStudies().largeTrade.state.available);const auto study=view.coinFlowStudies();QVERIFY(study.largeTrade.state.available);QCOMPARE(study.largeTrade.latestPercentile.value_or(-1),100.0);QCOMPARE(study.largeTrade.baselineSamples,200);
    QVERIFY(study.cvd.state.available);QCOMPARE(study.cvd.points.last().signedBaseQuantity,QString("200"));QVERIFY(study.comparison.state.available);
    QCOMPARE(study.comparison.spotSamples,201);QCOMPARE(study.comparison.futuresSamples,201);QCOMPARE(study.comparison.futuresSignedBaseQuantity,QString("-3"));QVERIFY(!study.comparison.directionsAgree);
    QVERIFY(study.liquidation.state.available);QCOMPARE(study.liquidation.state.coverage,CoinFlowCoverage::Partial);QCOMPARE(study.liquidation.observations.first().side,QString("SELL"));
    auto* details=view.findChild<QLabel*>("chartCoinFlowDetails");QVERIFY(details);QVERIFY(displayedText(details).contains(QStringLiteral("현물 기준 수량 200 / 선물 기준 수량 -3")));
    QVERIFY(displayedText(details).contains("RPI"));QVERIFY(displayedText(details).contains(QStringLiteral("원문 주문 side SELL")));QVERIFY(displayedText(details).contains(QStringLiteral("전체 청산 아님")));
    QVERIFY(!displayedText(details).contains("LONG"));QVERIFY(!displayedText(details).contains("SHORT"));
    auto* chip=view.findChild<QLabel*>("chartStudyStatus_whales");QVERIFY(chip);
    QVERIFY(study.volume.state.available);QCOMPARE(study.volume.multiple.value_or(-1),1.0);
    QCOMPARE(chip->text(),QStringLiteral("Whales & Reversal"));QVERIFY(!chip->text().contains(QStringLiteral("백분위")));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto image=chart->grab().toImage();int curvePixels=0;
    for(int y=qMax(0,image.height()-58);y<image.height()-32;++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==themeColors().studyPurple)++curvePixels;
    QCOMPARE(curvePixels,0); // CVD is now an explicitly selected lower pane, never a hidden side effect of whales.
    view.clearPrivate();QCOMPARE(view.coinFlowStudies().cvd.points.last().signedBaseQuantity,QString("200"));QCOMPARE(source.snapshot().liquidations.size(),1);
    preferences.setVisible("whales",false);QCOMPARE(image,chart->grab().toImage()); // This fixture has no qualified reversal marker; hidden CVD no longer changes price pixels.
    preferences.setVisible("whales",true);QCoreApplication::processEvents();
    const auto beforeLate=chart->grab().toImage();const auto fixedDecisions=view.coinFlowStudies();
    // This source observation arrives after the immutable minute decision. Its
    // original T is in the final half of the last real candle, never shifted.
    const auto lateAt=minute+59500;now=end+1000;
    auto late=QJsonDocument::fromJson(trade(201,lateAt,false)).object();late["q"]="1000";late["m"]=false;
    emit spot->received(spot->generation,QJsonDocument(late).toJson(QJsonDocument::Compact));
    QTRY_VERIFY(!view.coinFlowStudies().cvd.points.isEmpty()&&view.coinFlowStudies().cvd.points.last().atMs==lateAt);const auto afterStudy=view.coinFlowStudies();QCOMPARE(source.snapshot().primary.trades.size(),202);
    QCOMPARE(afterStudy.cvd.points.last().atMs,lateAt);QCOMPARE(afterStudy.cvd.points.last().signedBaseQuantity,QString("1200"));
    QCOMPARE(afterStudy.entries.size(),fixedDecisions.entries.size());QCOMPARE(afterStudy.groups.size(),fixedDecisions.groups.size());
    for(int i=0;i<fixedDecisions.entries.size();++i){const auto& before=fixedDecisions.entries[i];const auto& after=afterStudy.entries[i];
        QCOMPARE(after.kind,before.kind);QCOMPARE(after.direction,before.direction);QCOMPARE(after.atMs,before.atMs);QCOMPARE(after.knownAtMs,before.knownAtMs);QCOMPARE(after.entryClose,before.entryClose);}
    for(int i=0;i<fixedDecisions.groups.size();++i){const auto& before=fixedDecisions.groups[i];const auto& after=afterStudy.groups[i];
        QCOMPARE(after.kind,before.kind);QCOMPARE(after.direction,before.direction);QCOMPARE(after.fromMs,before.fromMs);QCOMPARE(after.throughMs,before.throughMs);QCOMPARE(after.knownAtMs,before.knownAtMs);}
    const auto afterLate=chart->grab().toImage();QCOMPARE(afterLate.size(),beforeLate.size());
    // The lower strip excludes the CVD caption, price bodies and time labels.
    // A changed source point must change the actual curve, even when its T is
    // past the candle center. No indicator labels can satisfy this assertion.
    const QRect cvdPlot(8,qMax(0,beforeLate.height()-58),qMax(1,beforeLate.width()-83),26);
    if(const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!preview.isEmpty()){
        QVERIFY(QDir().mkpath(preview));QVERIFY(beforeLate.save(QDir(preview).filePath("c6-cvd-before-late.png")));QVERIFY(afterLate.save(QDir(preview).filePath("c6-cvd-after-late.png")));
        QVERIFY(view.grab().save(QDir(preview).filePath("c6-observed-flow.png")));view.resize(820,1100);QCoreApplication::processEvents();QVERIFY(view.grab().save(QDir(preview).filePath("c6-observed-flow-narrow.png")));}
    QVERIFY2(beforeLate.copy(cvdPlot)==afterLate.copy(cvdPlot),"An unselected CVD must not resize or repaint the price field; completed decisions stay unchanged");
}
void MarketCandlesViewTest::observedCvdWithoutSummaryFactsStillDrawsAndRespectsVisibility(){
    QTemporaryDir profile;QVERIFY(profile.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.filePath("settings"));
    const auto end=QDateTime::currentMSecsSinceEpoch()/60000*60000;const auto base=end-180000;qint64 now=base;
    class Http final:public TradingTransport{public:QList<RequestSpec> requests;QList<quint64> tokens;
        void abort()override{}void send(quint64 token,const RequestSpec& request)override{QVERIFY(CoinFlowSource::isRequest(request));QVERIFY(!request.authenticated);QVERIFY(!request.headers.contains("Authorization"));requests.append(request);tokens.append(token);}} http;
    auto spotSocket=std::make_unique<StudySocket>(),futuresSocket=std::make_unique<StudySocket>();auto* spot=spotSocket.get();auto* futures=futuresSocket.get();
    CoinFlowSource source(&http,std::move(spotSocket),std::move(futuresSocket),nullptr,[&]{return now;});
    const auto instrument=[&](bool derivative){CoinFlowInstrument value;value.venue=derivative?CoinFlowVenue::BinanceUsdM:CoinFlowVenue::BinanceSpot;
        value.grain=derivative?CoinFlowGrain::Futures100msAggregate:CoinFlowGrain::SpotAggregate;value.identity=derivative?"usdm_futures:binance:SYNUSDT":"coin:binance:SYNUSDT";
        value.symbol="SYNUSDT";value.baseAsset="SYN";value.quoteAsset="USDT";value.status="TRADING";value.verified=true;
        if(derivative){value.marginAsset="USDT";value.contractType="PERPETUAL";value.underlyingType="COIN";}
        value.evidence={"합성 공개 메타데이터",derivative?"https://fapi.binance.com/fapi/v1/exchangeInfo":"https://api.binance.com/api/v3/exchangeInfo",0,now,now};return value;};
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument(false)});source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{instrument(true)});
    source.setSelection("coin:binance:SYNUSDT");source.setActive(true);QVERIFY(spot->generation);QVERIFY(futures->generation);
    emit spot->connected(spot->generation);emit futures->connected(futures->generation);
    int handled=0;for(int step=0;step<10;++step){if(handled<http.requests.size()){
            const auto& request=http.requests[handled];QVERIFY(request.url.path().endsWith("aggTrades")||request.url.path().endsWith("openInterestHist"));
            HttpResult empty;empty.status=200;empty.body="[]";emit http.finished(http.tokens[handled++],empty);}
        now+=500;source.pump();}
    QCOMPARE(handled,3);
    MarketCandleSeries series;series.available=true;series.identity="coin:binance:SYNUSDT";series.asset="SYN";series.currency="USDT";series.intervalMs=60000;
    series.sourceUrl=MarketCandles::request(series.identity,60000).url.toString();series.receivedAtMs=now;
    series.candles.append({end-120000,0,"99","101","99","100","10"});source.setMinuteSeries(series);
    const auto minute=end-60000;now=minute;source.pump();
    for(int i=0;i<3;++i){const auto at=minute+10000+i*15000;now=at+2;
        const QJsonObject trade{{"e","aggTrade"},{"E",QString::number(at+1)},{"s","SYNUSDT"},{"a",QString::number(9007199254741000LL+i)},
            {"p","100"},{"q",i==1?"3":"2"},{"f",QString::number(9007199254742000LL+i)},{"l",QString::number(9007199254742000LL+i)},
            {"T",QString::number(at)},{"m",i==1}};
        emit spot->received(spot->generation,QJsonDocument(trade).toJson(QJsonDocument::Compact));
        auto derivative=trade;derivative["nq"]="2";
        emit futures->received(futures->generation,QJsonDocument(QJsonObject{{"stream","synusdt@aggTrade"},{"data",derivative}}).toJson(QJsonDocument::Compact));}
    now=end;source.pump();series.receivedAtMs=now;series.candles.append({minute,0,"99","101","99","100","10"});source.setMinuteSeries(series);
    QCOMPARE(source.snapshot().primary.trades.size(),3);QVERIFY(source.snapshot().companion.has_value());QCOMPARE(source.snapshot().companion->trades.size(),3);
    StudyTransport chartHttp;MarketCandles model(&chartHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});MarketInstrument chartInstrument;
    chartInstrument.symbol="SYNUSDT";chartInstrument.baseAsset="SYN";chartInstrument.quoteAsset="USDT";chartInstrument.market="spot";chartInstrument.status="TRADING";chartInstrument.tradable=true;model.setInstruments("binance",{chartInstrument});
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume"})preferences.setEnabled(id,false);
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);view.setCoinFlowSource(&source);view.setSelection(series.identity,60000);view.resize(1100,1100);view.show();expandAppliedIndicators(view);QVERIFY(chartHttp.token);
    QJsonArray candles;for(const auto& row:series.candles)candles.append(QJsonArray{row.startsAtMs,row.open,row.high,row.low,row.close,row.volume,row.startsAtMs+59999,"1000",10,"5","500","0"});
    HttpResult response;response.status=200;response.body=QJsonDocument(candles).toJson(QJsonDocument::Compact);emit chartHttp.finished(chartHttp.token,response);QVERIFY(model.series().available);
    QThreadPool::globalInstance()->waitForDone();QTest::qWait(40);
    const auto study=view.coinFlowStudies();QVERIFY(study.cvd.state.available);QCOMPARE(study.cvd.points.size(),3);QCOMPARE(study.cvd.points.last().signedBaseQuantity,QString("1"));
    QVERIFY(!study.volume.state.available);QVERIFY(!study.volume.multiple);QVERIFY(!study.largeTrade.state.available);QVERIFY(!study.largeTrade.latestPercentile);
    QVERIFY(!study.openInterest.state.available);QVERIFY(!study.openInterest.quantityChangePercent);QVERIFY(!study.reversal.state.available);
    QVERIFY(preferences.enabled("whales"));QVERIFY(preferences.visible("whales"));
    auto* chip=view.findChild<QWidget*>("chartStudyChip_whales");QVERIFY(chip);QVERIFY(chip->isVisible()); // Enabled controls remain reachable even when inputs are missing.
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    const auto curvePixels=[&]{const auto image=chart->grab().toImage();int count=0;
        // This strip excludes the CVD caption, candle bodies and time labels.
        for(int y=qMax(0,image.height()-58);y<image.height()-32;++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==themeColors().studyPurple)++count;
        return count;};
    QCOMPARE(curvePixels(),0);QVERIFY(view.findChild<QWidget*>("chartLowerPane")->isHidden());
    preferences.setVisible("whales",false);QCOMPARE(curvePixels(),0);
    preferences.setVisible("whales",true);QVERIFY(chip->isVisible());QCOMPARE(curvePixels(),0);
    preferences.setEnabled("whales",false);QCOMPARE(curvePixels(),0);
    preferences.setEnabled("whales",true);QVERIFY(chip->isVisible());QCOMPARE(curvePixels(),0);
}
void MarketCandlesViewTest::rawTradeBurstCoalescesOnlyTheChartPresentationAndCancelsOnSelection(){
    qint64 now=(QDateTime::currentMSecsSinceEpoch()/60000-3)*60000;
    class Http final:public TradingTransport{public:QList<RequestSpec> requests;QList<quint64> tokens;
        void abort()override{}void send(quint64 token,const RequestSpec& request)override{requests.append(request);tokens.append(token);}} http;
    auto first=std::make_unique<StudySocket>(),second=std::make_unique<StudySocket>();auto* socket=first.get();auto* companion=second.get();
    CoinFlowSource source(&http,std::move(first),std::move(second),nullptr,[&]{return now;});
    const auto instrument=[&](bool derivative){CoinFlowInstrument value;value.venue=derivative?CoinFlowVenue::BinanceUsdM:CoinFlowVenue::BinanceSpot;
        value.grain=derivative?CoinFlowGrain::Futures100msAggregate:CoinFlowGrain::SpotAggregate;
        value.identity=derivative?"usdm_futures:binance:SYNUSDT":"coin:binance:SYNUSDT";value.symbol="SYNUSDT";value.baseAsset="SYN";value.quoteAsset="USDT";value.status="TRADING";value.verified=true;
        if(derivative){value.marginAsset="USDT";value.contractType="PERPETUAL";value.underlyingType="COIN";}
        value.evidence={"합성 공개 목록",derivative?"https://fapi.binance.com/fapi/v1/exchangeInfo":"https://api.binance.com/api/v3/exchangeInfo",0,now,now};return value;};
    source.setPublicCatalog(CoinFlowVenue::BinanceSpot,{instrument(false)});source.setPublicCatalog(CoinFlowVenue::BinanceUsdM,{instrument(true)});
    source.setSelection("coin:binance:SYNUSDT");source.setActive(true);emit socket->connected(socket->generation);emit companion->connected(companion->generation);
    int handled=0;for(int step=0;step<10;++step){if(handled<http.requests.size()){HttpResult empty;empty.status=200;empty.body="[]";emit http.finished(http.tokens[handled++],empty);}now+=500;source.pump();}
    QCOMPARE(handled,3);
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);
    view.setCoinFlowSource(&source);view.setSelection("coin:binance:SYNUSDT",60000);view.resize(1000,900);view.show();
    QTRY_VERIFY(view.coinFlowStudies().asOfMs>0);
    // Begin a full minute after seed establishment. Only completed observed
    // minutes are eligible for CVD; an in-flight minute must remain unavailable.
    now=(now/60000+1)*60000;
    QSignalSpy presentation(&view,&MarketCandlesView::coinFlowStudiesChanged),observations(&source,&CoinFlowSource::changed);
    const auto append=[&](int index){++now;
        const QJsonObject trade{{"e","aggTrade"},{"E",QString::number(now)},{"s","SYNUSDT"},{"a",QString::number(1000+index)},
            {"p","100"},{"q","1"},{"f",QString::number(2000+index)},{"l",QString::number(2000+index)},{"T",QString::number(now-1)},{"m",false}};
        emit socket->received(socket->generation,QJsonDocument(trade).toJson(QJsonDocument::Compact));};
    for(int i=0;i<100;++i)append(i);
    QCOMPARE(source.snapshot().primary.trades.size(),100);QCOMPARE(observations.count(),100);
    QCOMPARE(presentation.count(),0); // The raw source remains lossless; UI work waits for one bounded drain.
    now=(now/60000+1)*60000;source.pump();
    const auto expected=coinFlowStudies(source.snapshot(),QDateTime::currentMSecsSinceEpoch());
    QVERIFY(expected.cvd.state.available);QCOMPARE(expected.cvd.points.last().signedBaseQuantity,QString("100"));
    QTRY_VERIFY_WITH_TIMEOUT(presentation.count()>0,1000);QVERIFY(presentation.count()<=3);
    QTRY_VERIFY_WITH_TIMEOUT(!view.coinFlowStudies().cvd.points.isEmpty(),2500);
    QCOMPARE(view.coinFlowStudies().cvd.points.last().signedBaseQuantity,QString("100"));
    // New trades in the same chart keep the shown result until the next calculation replaces it.
    // Before, every 250 ms refresh blanked the markers and panels first (user report: chart felt slow).
    {QElapsedTimer watch;watch.start();bool blanked=false;for(int i=100;i<110;++i)append(i);
        while(watch.elapsed()<1500){QTest::qWait(20);if(view.coinFlowStudies().cvd.points.isEmpty())blanked=true;}
        QVERIFY2(!blanked,"New trades must not blank the shown flow result before the next calculation");}
    for(int i=110;i<120;++i)append(i);
    view.setSelection("stock:US:SYN",86400000);QVERIFY(view.coinFlowStudies().identity.isEmpty());
    const auto boundary=presentation.count();QTest::qWait(350);
    QVERIFY(view.coinFlowStudies().identity.isEmpty());QCOMPARE(presentation.count(),boundary);
    QCOMPARE(source.snapshot().primary.trades.size(),120);QVERIFY(source.active());
    view.setSelection("coin:binance:SYNUSDT",60000);
    QTRY_VERIFY(!view.coinFlowStudies().cvd.points.isEmpty());
    append(120);view.setCoinFlowSource(nullptr);
    QVERIFY(view.coinFlowStudies().cvd.points.isEmpty());
    const auto detached=presentation.count();QTest::qWait(350);
    QVERIFY(view.coinFlowStudies().cvd.points.isEmpty());QCOMPARE(presentation.count(),detached);
    QCOMPARE(source.snapshot().primary.trades.size(),121);QVERIFY(source.active());
}
void MarketCandlesViewTest::coinFlowRendersIndependentCompletedVolumeReversalMarkersAndPhysicalScorecard(){
    qint64 now=QDateTime::currentMSecsSinceEpoch();const auto end=now/60000*60000;
    StudyTransport chartHttp;MarketCandles model(&chartHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    class FlowHttp final:public TradingTransport{public:int requests=0;void abort()override{}void send(quint64,const RequestSpec& request)override{
        QVERIFY(CoinFlowSource::isRequest(request));QVERIFY(!request.authenticated);++requests;}} flowHttp;
    auto socket=std::make_unique<StudySocket>();CoinFlowSource source(&flowHttp,std::move(socket),{},nullptr,[&]{return now;});
    CoinFlowInstrument instrument;instrument.venue=CoinFlowVenue::UpbitSpot;instrument.grain=CoinFlowGrain::UpbitTrade;
    instrument.identity="coin:upbit:KRW-SYN";instrument.symbol="KRW-SYN";instrument.baseAsset="SYN";instrument.quoteAsset="KRW";instrument.verified=true;
    instrument.evidence={"업비트 합성 공개 메타데이터","https://api.upbit.com/v1/market/all",0,now,now};
    source.setPublicCatalog(CoinFlowVenue::UpbitSpot,{instrument});source.setSelection(instrument.identity);source.setActive(true);
    ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto* id:{"pullback","similarity","cloud","volume"})preferences.setEnabled(id,false);
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);view.setCoinFlowSource(&source);view.setSelection(instrument.identity,60000);view.resize(1100,1050);view.show();
    QList<MarketCandle> rows;for(int i=0;i<24;++i){const auto at=end-(24-i)*60000;const QString close=i==20?"100":i==21?"96":i==22?"94":i==23?"95":"100";
        rows.append({at,at+1000,i==23?"94":close,close,i==23?"94":close,close,i==23?"40":"10"});}
    QVERIFY(chartHttp.token);emit chartHttp.finished(chartHttp.token,syntheticStudyResponse(rows));QVERIFY(model.series().available);
    auto seed=model.series();seed.candles.removeLast();seed.receivedAtMs=now-60000;seed.sourceTimeMs=seed.candles.last().lastStoredTickMs;QVERIFY(seed.sourceTimeMs<=seed.receivedAtMs);source.setMinuteSeries(seed);source.setMinuteSeries(model.series());
    auto* details=view.findChild<QLabel*>("chartCoinFlowDetails");QVERIFY2(details,"Coin-flow facts need typed per-part availability, sample and source coverage details");
    QTRY_VERIFY(view.coinFlowStudies().volume.state.available);const auto studies=view.coinFlowStudies();QVERIFY(studies.volume.state.available);QVERIFY(studies.volume.multiple.has_value());QCOMPARE(*studies.volume.multiple,4.0);
    QVERIFY(studies.reversal.state.available);QVERIFY(studies.reversal.rebound);QVERIFY(!studies.openInterest.state.available);QVERIFY(!studies.taker.state.available);
    auto* chip=view.findChild<QLabel*>("chartStudyStatus_whales");QVERIFY(chip);QCOMPARE(chip->text(),QStringLiteral("Whales & Reversal"));QVERIFY(displayedText(details).contains(QStringLiteral("4.00배")));
    QVERIFY(!chip->text().contains(QStringLiteral("반등")));QVERIFY(!chip->property("accent").toBool());
    QVERIFY(displayedText(details).contains(QStringLiteral("완료 1분")));QVERIFY(displayedText(details).contains(QStringLiteral("표본")));QVERIFY(displayedText(details).contains(QStringLiteral("OI")));
    auto* score=view.findChild<QLabel*>("chartCoinFlowScorecard");QVERIFY(score);QVERIFY(score->text().contains(QStringLiteral("1시간")));
    QVERIFY(score->text().contains("no-lookahead"));QVERIFY(score->text().contains(QStringLiteral("비용")));QVERIFY(score->text().contains(QStringLiteral("표본")));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto shown=chart->grab().toImage();preferences.setVisible("whales",false);
    QVERIFY2(shown!=chart->grab().toImage(),"The completed public reversal/volume group must draw a real chart marker and obey the legend eye");
    const auto requests=flowHttp.requests;view.hide();QVERIFY(source.active());QCOMPARE(flowHttp.requests,requests); // Shared source lifecycle belongs to the root demand arbiter.
    view.setSelection("stock:US:SYN",86400000);QVERIFY(view.coinFlowStudies().identity.isEmpty());QVERIFY(details->isHidden());
}
void MarketCandlesViewTest::unconnectedStockClearsPreviouslyObservedCoinBodiesAndStudies_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void MarketCandlesViewTest::unconnectedStockClearsPreviouslyObservedCoinBodiesAndStudies(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);
    view.setStyleSheet(KeefiTheme::localStyleSheet(&view));
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();
    // The public source requests and accepts at most 200 native candles.
    // A larger synthetic page is deliberately rejected before any UI update.
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));
    QVERIFY(model.series().available);QCOMPARE(model.series().candles.size(),200);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QVERIFY(chart->isVisible());QTRY_VERIFY(view.chartStudies().available);
    QCOMPARE(view.chartStudies().observations,200);
    if(const auto directory=qEnvironmentVariable("KEEFI_BROKER_ONLY_RENDER_DIR");!directory.isEmpty()){
        QVERIFY(QDir().mkpath(directory));QCoreApplication::processEvents();
        QVERIFY(view.grab().save(QDir(directory).filePath(QString("guest-coin-%1.png").arg(dark?"dark":"light"))));
    }
    view.setSelection("stock:US:AAPL",86400000);
    QVERIFY(chart->isHidden());QVERIFY(!model.series().available);QVERIFY(model.series().identity.isEmpty());
    QVERIFY(!view.chartStudies().available);QVERIFY(!view.chartStudies().scorecard.available);QVERIFY(!view.chartStudies().similarity.available);
    QVERIFY(displayedText(view.findChild<QLabel*>("marketCandlesStatus")).contains(QStringLiteral("미국 주식 차트는 토스증권")));
}
void MarketCandlesViewTest::stockSelectionKeepsItsTypedContextVisibleWhileWaitingForSource(){
    MarketCandlesView view(nullptr);view.setStockSource(nullptr);
    view.setSelection("stock:US:SYN",86400000);view.resize(1000,800);view.show();
    auto* title=view.findChild<QLabel*>("marketCandlesTitle");QVERIFY(title);
    QVERIFY(displayedText(title).contains(QStringLiteral("SYN · 미국 주식")));
    auto* context=view.findChild<QLabel*>("marketCandlesStockContext");
    QVERIFY2(context,"Stock source, session and adjustment facts need a persistent plain-text context label");
    QVERIFY(context->isHidden());
    QVERIFY(context->text().contains(QStringLiteral("정규장")));
    QVERIFY(context->text().contains(QStringLiteral("원천")));
    QVERIFY(context->text().contains(QStringLiteral("수정주가")));
    QVERIFY(view.priceLines().isEmpty());
    view.setAsset("SYN");QVERIFY(context->isHidden());
}
void MarketCandlesViewTest::stockSourceFailureDoesNotOfferAccountRegistration(){
    StudyTransport http;StockChartSource source(&http);source.setConnectionState(StockChartConnectionState::Ready);
    source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_CHART",60000,1);return true;});
    MarketCandlesView view(nullptr);view.setStockSource(&source);view.setSelection("stock:KR:005930",60000);
    view.resize(900,760);view.show();QTRY_VERIFY(http.token);
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");
    auto* connectButton=view.findChild<QPushButton*>("chartStockConnect");
    QVERIFY(status&&connectButton);QVERIFY(!connectButton->isVisible());
    HttpResult outage;outage.status=503;emit http.finished(http.token,outage);
    QTRY_VERIFY(displayedText(status).contains(QStringLiteral("원천 응답 실패")));
    QVERIFY(!connectButton->isVisible());
    source.setConnectionState(StockChartConnectionState::AuthenticationRequired);
    QTRY_VERIFY(connectButton->isVisible());QVERIFY(displayedText(status).contains(QStringLiteral("토스증권 인증")));
    view.setSelection("coin:upbit:KRW-BTC",60000);QVERIFY(!connectButton->isVisible());
}
void MarketCandlesViewTest::unconnectedStocksExplainBrokerRequirement_data(){
    QTest::addColumn<QString>("identity");
    QTest::addColumn<bool>("dark");
    QTest::newRow("KR-light")<<QStringLiteral("stock:KR:005930")<<false;
    QTest::newRow("KR-dark")<<QStringLiteral("stock:KR:005930")<<true;
    QTest::newRow("US-light")<<QStringLiteral("stock:US:AAPL")<<false;
    QTest::newRow("US-dark")<<QStringLiteral("stock:US:AAPL")<<true;
}
void MarketCandlesViewTest::unconnectedStocksExplainBrokerRequirement(){
    QFETCH(QString,identity);QFETCH(bool,dark);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    MarketCandlesView view(nullptr);view.setSelection(identity,86400000);view.resize(1000,800);view.show();
    view.setStyleSheet(KeefiTheme::localStyleSheet(&view));QCoreApplication::processEvents();
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");QVERIFY(status);QVERIFY(status->isVisible());
    QVERIFY(displayedText(status).contains(QStringLiteral("연결된 증권사가 없어요")));
    QVERIFY(displayedText(status).contains(identity.contains(":KR:")?QStringLiteral("토스증권 또는 키움증권"):QStringLiteral("미국 주식 차트는 토스증권")));
    auto* connectButton=view.findChild<QPushButton*>("chartStockConnect");QVERIFY(connectButton&&connectButton->isVisible());
    QSignalSpy request(&view,&MarketCandlesView::stockConnectionRequested);
    QTest::mouseClick(connectButton,Qt::LeftButton);QCOMPARE(request.count(),1);
    QCOMPARE(request.first().at(0).toString(),identity);QCOMPARE(request.first().at(1).toLongLong(),86400000LL);
    QVERIFY(view.findChild<QWidget*>("marketCandlesChart")->isHidden());
    for(const auto* label:view.findChildren<QLabel*>()){
        QVERIFY(!label->text().contains("Yahoo"));QVERIFY(!label->text().contains(QStringLiteral("지연 일봉")));
    }
    const auto snapshot=view.shareSnapshot({},StudyNow);
    QVERIFY(snapshot.availability!=ChartShareAvailability::Ready);QVERIFY(snapshot.closed.isEmpty());
    if(const auto directory=qEnvironmentVariable("KEEFI_BROKER_ONLY_RENDER_DIR");!directory.isEmpty()){
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(view.grab().save(QDir(directory).filePath(QString("stock-%1-%2.png").arg(identity.contains(":KR:")?"kr":"us",dark?"dark":"light"))));
    }
}
void MarketCandlesViewTest::cachedCandlesPaintWithoutBecomingFreshStudyOrShareInputs(){
    StudyTransport http;MarketCandles model(&http,{},[]{return StudyNow;});MarketCandlesView view(&model);
    const QString identity="coin:upbit:KRW-SYN";
    view.setSelection(identity,60000);view.resize(1000,900);view.show();QCoreApplication::processEvents();
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    StudyPaintObserver observer;chart->installEventFilter(&observer);
    StudyWorkerGate gate;QVERIFY(gate.started);
    emit http.finished(http.token,syntheticStudyResponse(syntheticStudyRows(200)));
    QVERIFY(model.series().available);QVERIFY(!view.chartStudies().available);
    view.setSelection("coin:upbit:KRW-ALT",60000);
    QVERIFY(chart->property("displayIdentity").toString().isEmpty());
    QCOMPARE(chart->property("displayCompletedCount").toInt(),0);
    view.setSelection(identity,60000);
    QCOMPARE(chart->property("displayIdentity").toString(),identity);
    QCOMPARE(chart->property("displayIntervalMs").toLongLong(),qint64(60000));
    QCOMPARE(chart->property("displayCompletedCount").toInt(),200);
    QVERIFY(chart->property("displayHistoricalOnly").toBool());QVERIFY(chart->property("displayFromMemoryCache").toBool());
    QVERIFY(!model.series().available);QVERIFY(!model.series().forming);
    QVERIFY(!view.chartStudies().available);
    QVERIFY(view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
    const auto paints=observer.paints;QTRY_VERIFY(observer.paints>paints);
    QVERIFY(chart->isVisible());QVERIFY(chart->accessibleDescription().contains(QStringLiteral("지표 계산과 알림 제외")));
    QVERIFY(displayedText(view.findChild<QLabel*>("marketCandlesStatus")).contains(QStringLiteral("이전 캔들")));
    const auto share=view.shareSnapshot({},StudyNow);QVERIFY(share.availability!=ChartShareAvailability::Ready);QVERIFY(share.closed.isEmpty());
    gate.open();QVERIFY(gate.pool->waitForDone(5000));QCoreApplication::processEvents();
    QVERIFY(!view.chartStudies().available); // the pre-selection study worker cannot publish over historical-only candles
    view.hide();QVERIFY(model.series().candles.isEmpty());view.show();
    QVERIFY(chart->property("displayFromMemoryCache").toBool());QVERIFY(!view.chartStudies().available);
    const auto beforeLock=http.token;model.clearDisplayCache();
    QVERIFY(chart->property("displayIdentity").toString().isEmpty());QCOMPARE(chart->property("displayCompletedCount").toInt(),0);
    emit http.finished(beforeLock,syntheticStudyResponse(syntheticStudyRows(200)));
    QCOMPARE(chart->property("displayCompletedCount").toInt(),0);
    emit http.finished(http.token,syntheticStudyResponse(syntheticStudyRows(200)));
    QTRY_VERIFY(view.chartStudies().available);
    QVERIFY(!chart->property("displayFromMemoryCache").toBool());QVERIFY(!chart->property("displayHistoricalOnly").toBool());
    QCOMPARE(view.chartStudies().observations,200);
}
void MarketCandlesViewTest::completedStudiesYieldToCandlePaintAndKeepFormingLive(){
    StudyTransport transport;auto socket=std::make_unique<StudySocket>();auto* wire=socket.get();qint64 now=StudyNow;
    MarketCandles model(&transport,std::move(socket),[&]{return now;});MarketCandlesView view(&model);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,900);view.show();QCoreApplication::processEvents();
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);StudyPaintObserver observer;chart->installEventFilter(&observer);
    StudyWorkerGate gate;QVERIFY(gate.started);QSignalSpy changes(&view,&MarketCandlesView::studiesChanged);
    const auto rows=syntheticStudyRows(200);emit transport.finished(transport.token,syntheticStudyResponse(rows));
    QVERIFY(model.series().available);QVERIFY2(!view.chartStudies().available,"Completed public studies must not block the first candles Paint");
    QVERIFY(view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
    QTRY_VERIFY(observer.paints>0);QVERIFY(chart->accessibleDescription().contains("200"));
    emit wire->connected(wire->generation);now+=1000;
    MarketCandle forming{now/60000*60000,now,"100","120","80","115","8888"};auto frame=syntheticStudyWire(forming);
    frame.remove("market");frame.remove("unit");frame["code"]="KRW-SYN";frame["type"]="candle.1m";frame["stream_type"]="REALTIME";
    const auto pendingChanges=changes.size();emit wire->received(wire->generation,QJsonDocument(frame).toJson());
    QVERIFY(model.series().forming.has_value());QCOMPARE(model.series().forming->close,QString("115"));QCOMPARE(changes.size(),pendingChanges);
    gate.open();QTRY_VERIFY(view.chartStudies().available);
    const auto expected=keefiChartStudies(rows,60000),actual=view.chartStudies();
    QCOMPARE(actual.observations,expected.observations);QCOMPARE(actual.lastCompletedAtMs,expected.lastCompletedAtMs);
    QCOMPARE(actual.pullback.score,expected.pullback.score);QCOMPARE(actual.pullback.boxLow,expected.pullback.boxLow);
    QCOMPARE(actual.similarity.samples,expected.similarity.samples);QCOMPARE(actual.cloud.size(),expected.cloud.size());QCOMPARE(actual.volumes.size(),200);
    QVERIFY(!view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
    const auto completedChanges=changes.size();now+=1000;forming.lastStoredTickMs=now;forming.close="116";frame["timestamp"]=QString::number(now);frame["trade_price"]="116";
    emit wire->received(wire->generation,QJsonDocument(frame).toJson());QCOMPARE(changes.size(),completedChanges);QCOMPARE(view.chartStudies().observations,200);
}
void MarketCandlesViewTest::lateCompletedStudiesCannotCrossContext_data(){
    QTest::addColumn<QString>("boundary");for(const auto* boundary:{"selection","interval","hide","revocation"})QTest::newRow(boundary)<<QString(boundary);
}
void MarketCandlesViewTest::lateCompletedStudiesCannotCrossContext(){
    QFETCH(QString,boundary);StudyTransport transport;qint64 now=StudyNow;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    MarketCandlesView view(&model);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,900);view.show();
    StudyWorkerGate gate;QVERIFY(gate.started);emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));
    QVERIFY2(!view.chartStudies().available,"The public study result must remain pending while its worker is queued");
    if(boundary=="selection")view.setSelection("coin:upbit:KRW-ALT",60000);
    else if(boundary=="interval")view.setSelection("coin:upbit:KRW-SYN",300000);
    else if(boundary=="hide")view.hide();
    else{now+=MarketCandles::FreshnessMs+1;emit model.changed();}
    QVERIFY(!view.chartStudies().available);QVERIFY(view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
    QSignalSpy changed(&view,&MarketCandlesView::studiesChanged);gate.open();QVERIFY(gate.pool->waitForDone(5000));QCoreApplication::processEvents();
    QVERIFY(!view.chartStudies().available);QVERIFY(!view.chartStudies().similarity.available);QVERIFY(view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());QCOMPARE(changed.size(),0);
}
void MarketCandlesViewTest::destroyingViewDoesNotWaitForCompletedStudyWorker(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    auto* view=new MarketCandlesView(&model);view->setSelection("coin:upbit:KRW-SYN",60000);view->resize(1000,900);view->show();
    StudyWorkerGate gate;QVERIFY(gate.started);emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));
    QVERIFY2(!view->chartStudies().available,"The worker must hold only copied public rows, independently of the widget lifetime");
    QPointer<MarketCandlesView> alive=view;delete view;QVERIFY(alive.isNull());gate.open();QVERIFY(gate.pool->waitForDone(5000));QCoreApplication::processEvents();
    QVERIFY(!model.series().available);
}
void MarketCandlesViewTest::firstHiddenPublicStudyCompletionRemainsUsableOnShow(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    MarketCandlesView view(&model);view.setSelection("coin:upbit:KRW-SYN",60000);
    model.setSelection("coin:upbit:KRW-SYN",60000);StudyWorkerGate gate;QVERIFY(gate.started);
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));QVERIFY(!view.chartStudies().available);
    gate.open();QVERIFY(gate.pool->waitForDone(5000));QCoreApplication::processEvents();
    QVERIFY2(view.chartStudies().available,"A never-shown public view must not poison the identical completed-study cache key");
    view.resize(1000,900);view.show();QVERIFY(view.chartStudies().available);QCOMPARE(view.chartStudies().observations,200);
    QVERIFY(!view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
}
void MarketCandlesViewTest::completedStudyApplicationWaitsForItsFirstActualCandlePaint(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("similarity",true);MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,900);view.show();QCoreApplication::processEvents();
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);StudyPaintObserver observer;chart->installEventFilter(&observer);chart->setUpdatesEnabled(false);
    StudyWorkerGate gate;QVERIFY(gate.started);const auto rows=syntheticStudyRows(200);emit transport.finished(transport.token,syntheticStudyResponse(rows));
    QVERIFY(model.series().available);QVERIFY(!view.chartStudies().available);gate.open();QVERIFY(gate.pool->waitForDone(5000));QCoreApplication::processEvents();
    QCOMPARE(observer.paints,0);QVERIFY2(!view.chartStudies().available,"Ready auxiliary studies must wait until their new public candles have actually painted");
    QVERIFY(view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
    chart->setUpdatesEnabled(true);chart->update();QTRY_VERIFY(observer.paints>0);QTRY_VERIFY(view.chartStudies().available);
    const auto canonical=keefiChartStudies(rows,60000);QCOMPARE(view.chartStudies().observations,canonical.observations);
    QCOMPARE(view.chartStudies().pullback.score,canonical.pullback.score);QCOMPARE(view.chartStudies().similarity.samples,canonical.similarity.samples);
    QCOMPARE(view.chartStudies().scorecard.meanReturnPercent,canonical.scorecard.meanReturnPercent);
    QVERIFY(!view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());QVERIFY(chart->accessibleDescription().contains(QStringLiteral("이후 경로")));
    QCOMPARE(syntheticStudyResponse(model.series().candles).body,syntheticStudyResponse(rows).body);QVERIFY(!model.series().sourceUrl.isEmpty());QVERIFY(model.series().receivedAtMs>0);
}
void MarketCandlesViewTest::completedStudySnapshotsIgnoreFormingOnlyUpdates(){
    StudyTransport transport;auto socket=std::make_unique<StudySocket>();auto* wire=socket.get();qint64 now=StudyNow;
    MarketCandles model(&transport,std::move(socket),[&]{return now;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1000,900);view.show();
    QVERIFY(transport.token);QVERIFY(wire->generation);emit wire->connected(wire->generation);
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(200)));
    QVERIFY(model.series().available);
    QTRY_VERIFY(view.chartStudies().available);
    const auto snapshot=view.chartStudies();
    QVERIFY2(snapshot.available,"A typed study snapshot must be calculated from completed public candles");
    QCOMPARE(snapshot.observations,200);QCOMPARE(snapshot.lastCompletedAtMs,StudyNow/60000*60000);QCOMPARE(snapshot.similarity.samples,3);
    auto* details=view.findChild<QLabel*>("marketCandlesStudyDetails");
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");
    QVERIFY(details);QVERIFY(status);
    const auto completedDetails=details->text();const auto previousStatus=status->text();
    QVERIFY(!completedDetails.isEmpty());
    QSignalSpy changes(&view,&MarketCandlesView::studiesChanged);
    const auto formingAt=StudyNow/60000*60000;
    now+=1000;
    MarketCandle forming{formingAt,now,"100","120","80","115","8888"};
    auto frame=syntheticStudyWire(forming);frame.remove("market");frame.remove("unit");frame["code"]="KRW-SYN";frame["type"]="candle.1m";frame["stream_type"]="REALTIME";
    emit wire->received(wire->generation,QJsonDocument(frame).toJson());
    QVERIFY(model.series().forming.has_value());QCOMPARE(model.series().forming->close,QString("115"));
    QCOMPARE(changes.size(),0);
    const auto retained=view.chartStudies();QCOMPARE(retained.lastCompletedAtMs,snapshot.lastCompletedAtMs);QCOMPARE(retained.observations,snapshot.observations);
    QCOMPARE(retained.similarity.samples,snapshot.similarity.samples);QCOMPARE(retained.volumes.size(),200);QCOMPARE(retained.cloud.size(),snapshot.cloud.size());
    QVERIFY(!retained.volumes.isEmpty());QVERIFY(retained.volumes.last().volume!="8888");
    QCOMPARE(details->text(),completedDetails);
    QCOMPARE(status->text(),previousStatus);QVERIFY(status->isHidden());
    QCOMPARE(model.series().sourceTimeMs,now);QCOMPARE(model.series().forming->lastStoredTickMs,now); // Typed timing remains live without a status line.
    now+=MarketCandles::FreshnessMs+1;emit model.changed();
    QVERIFY(!model.series().available);QVERIFY(details->text().isEmpty());
    now=StudyNow+1000;emit model.changed();
    QVERIFY(model.series().available);QTRY_COMPARE(details->text(),completedDetails);
}
void MarketCandlesViewTest::firstMeasuredCloudSampleIsVisible(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto& id:preferences.supportedIds("coin"))preferences.setEnabled(id,id=="cloud");
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(900,750);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(26)));
    QTRY_COMPARE(view.chartStudies().cloud.size(),1);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    const auto shown=chart->grab().toImage();
    preferences.setVisible("cloud",false);QVERIFY(chart->grab().toImage()!=shown);
    preferences.setVisible("cloud",true);QCOMPARE(chart->grab().toImage(),shown);
}
void MarketCandlesViewTest::overlaysDrawObservedPathsCloudBoxAndVolume(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("similarity",true);
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,1000);view.show();
    auto rows=syntheticStudyRows(200);rows[190].low="90";rows[191].high="180";
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));
    QTRY_VERIFY(view.chartStudies().available);const auto studies=view.chartStudies();QVERIFY(studies.available);QVERIFY(studies.pullback.available);QVERIFY(studies.similarity.available);QVERIFY(!studies.cloud.isEmpty());
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto all=chart->grab().toImage();
    preferences.setVisible("cloud",false);const auto noCloud=chart->grab().toImage();QVERIFY2(noCloud!=all,"Cloud points must draw a real price layer");
    preferences.setVisible("pullback",false);const auto noBox=chart->grab().toImage();QVERIFY2(noBox!=noCloud,"The observed support box must draw on the chart");
    preferences.setVisible("similarity",false);const auto noPaths=chart->grab().toImage();QVERIFY2(noPaths!=noBox,"Historical following paths must use chart space to the right of observed candles");
    preferences.setVisible("volume",false);const auto noVolume=chart->grab().toImage();QVERIFY2(noVolume!=noPaths,"Source volume bars must be rendered");
    preferences.setVisible("similarity",true);
    const auto text=view.findChild<QLabel*>("chartStudySimilarityEvidence");QVERIFY(text);
    QVERIFY(text->text().contains(QStringLiteral("3개")));QVERIFY(text->text().contains(QStringLiteral("겹침")));QVERIFY(text->text().contains(QStringLiteral("범위")));
    QVERIFY(text->text().contains(QStringLiteral("정규화 없음")));QVERIFY(text->text().contains(QStringLiteral("예측")));
    QVERIFY(chart->accessibleDescription().contains(QStringLiteral("이후 경로")));
    const auto* chip=view.findChild<QWidget*>("chartStudyChip_similarity");QVERIFY(chip);
    for(int i=0;i<studies.similarity.paths.size();++i)
        QVERIFY(chip->toolTip().contains(QStringLiteral("경로 %1 · 유사도(상관) %2").arg(i+1).arg(QString::number(studies.similarity.paths[i].correlation,'f',3))));
}
void MarketCandlesViewTest::weakSimilarityKeepsReasonInternalAndNoFollowingPath(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("similarity",true);MarketCandlesView view(&model);
    view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,1000);view.show();
    auto rows=syntheticStudyRows(60);
    for(int i=40;i<60;++i){const auto value=10000.0/rows[i].close.toDouble();rows[i].open=rows[i].close=QString::number(value,'f',12);rows[i].high=QString::number(value+1,'f',12);rows[i].low=QString::number(value-1,'f',12);}
    QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));
    QTRY_VERIFY(view.chartStudies().available);const auto studies=view.chartStudies();QVERIFY(studies.available);QVERIFY(!studies.similarity.available);QVERIFY(studies.similarity.paths.isEmpty());
    const auto* chip=view.findChild<QWidget*>("chartStudyChip_similarity");QVERIFY(chip);
    QCOMPARE(studies.similarity.reason,QStringLiteral("충분히 닮은 과거 구간이 없어요"));
    auto* summary=chip->findChild<QLabel*>("chartStudyStatus_similarity");QVERIFY(summary);QCOMPARE(summary->text(),QStringLiteral("Similarity · Historical Comparison"));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QVERIFY(!chart->accessibleDescription().contains(QStringLiteral("이후 경로")));
    const auto* details=view.findChild<QLabel*>("marketCandlesStudyDetails");QVERIFY(details);QVERIFY(details->text().contains(studies.similarity.reason));QVERIFY(details->isHidden());
    const auto withLayer=chart->grab().toImage();preferences.setVisible("similarity",false);QCOMPARE(chart->grab().toImage(),withLayer);
}
void MarketCandlesViewTest::historicalFollowingPathsDoNotFillWithTheLastCandleColor(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("similarity",true);
    preferences.setVisible("cloud",false);preferences.setVisible("pullback",false);preferences.setVisible("volume",false);
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,1000);view.show();
    const auto rows=syntheticStudyRows(200);QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));
    QTRY_VERIFY(view.chartStudies().similarity.available);const auto studies=view.chartStudies();QVERIFY(studies.similarity.available);QCOMPARE(studies.similarity.samples,3);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto image=chart->grab().toImage();
    // The public chart initially shows the latest 60 observations plus the
    // following paths. Exclude the final observed body and its stroke margin.
    const auto start=rows[140].startsAtMs-30000;
    const auto end=studies.similarity.maximum.last().atMs+30000;
    const double lastBodyCenter=8+double(rows.last().startsAtMs-start)/double(end-start)*(chart->width()-83);
    const double bodyWidth=qBound(1.0,double(chart->width()-83)*60000/double(end-start)*.65,18.0);
    const double scale=image.devicePixelRatio();
    const int futureLeft=qCeil((lastBodyCenter+bodyWidth/2+3)*scale);
    const int futureRight=qFloor((chart->width()-75)*scale);
    QVERIFY(futureRight-futureLeft>100);
    int greenFuturePixels=0,pathPixels=0,observedGreenPixels=0;
    const QColor candleGreen=MovementColorPolicy::colorFor(chart,MovementDirection::Flat); // The actual fixture rows are doji.
    const QList<QColor> pathColors{themeColors().studyPurple,themeColors().studyBlue,themeColors().studyOrange,themeColors().studyBlue};
    for(int y=qCeil(12*scale);y<qFloor((chart->height()-30)*scale);++y){
        for(int x=qCeil(8*scale);x<futureRight;++x){const auto color=image.pixelColor(x,y);
            if(x<futureLeft){if(color==candleGreen)++observedGreenPixels;continue;}
            if(color==candleGreen)++greenFuturePixels;
            if(pathColors.contains(color))++pathPixels;
        }
    }
    QVERIFY(observedGreenPixels>0);QVERIFY(pathPixels>0);
    QCOMPARE(greenFuturePixels,0);
}
void MarketCandlesViewTest::fullIdentitySelectionClearsPriorPrivateOverlayAndLabelsTimeframe(){
    MarketCandlesView view(nullptr);view.setAsset("BTC");
    view.setPriceLines({{"upbit","synthetic-source-switch","BTC","KRW","103","https://api.upbit.com/v1/accounts",QDateTime::currentMSecsSinceEpoch()}});
    QCOMPARE(view.priceLines().size(),1);
    view.setSelection("coin:binance:SYNUSDT",60000);
    QVERIFY2(view.priceLines().isEmpty(),"Switching source identity clears the previous chart's private overlay");
    const auto title=displayedText(view.findChild<QLabel*>("marketCandlesTitle"));
    QVERIFY(title.contains("SYNUSDT · 바이낸스"));QVERIFY(title.contains(QStringLiteral("1분")));
    QVERIFY(!title.contains(QStringLiteral("업비트")));QVERIFY(!title.contains(QStringLiteral("15분")));
    view.setSelection("coin:upbit:KRW-BTC",14400000);
    QVERIFY(displayedText(view.findChild<QLabel*>("marketCandlesTitle")).contains(QStringLiteral("4시간")));
}
void MarketCandlesViewTest::nativeSeriesShowsVerifiedCurrencyAndSeparateSourceClock(){
    class Fake final:public TradingTransport{public:quint64 token=0;RequestSpec request;
        void send(quint64 t,const RequestSpec& r)override{token=t;request=r;}void abort()override{}} transport;
    MarketCandles model(&transport);
    MarketInstrument instrument;instrument.symbol="SYNUSDT";instrument.baseAsset="SYN";instrument.quoteAsset="USDT";instrument.market="spot";instrument.status="TRADING";instrument.tradable=true;
    model.setInstruments("binance",{instrument});
    MarketCandlesView view(&model);view.setSelection("coin:binance:SYNUSDT",60000);view.resize(900,680);view.show();
    QTRY_VERIFY(transport.token);
    QCOMPARE(transport.request.url.host(),QString("api.binance.com"));
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto end=now/60000*60000;QJsonArray rows;
    for(int i=30;i>0;--i){const auto at=end-i*60000;
        rows.append(QJsonArray{at,"100","110","90","101","2",at+59999,"202",10,"1","101","0"});}
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
    QTRY_VERIFY(model.series().available);
    auto* title=view.findChild<QLabel*>("marketCandlesTitle");
    QVERIFY(displayedText(title).contains("SYN/USDT · 바이낸스"));QVERIFY(displayedText(title).contains("USDT"));QVERIFY(displayedText(title).contains(QStringLiteral("1분")));
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");
    QVERIFY(status->isHidden());QVERIFY(status->text().isEmpty());
    const auto series=model.series();
    QCOMPARE(series.sourceTimeMs,qint64(0));
    QCOMPARE(title->toolTip(),QString("바이낸스"));
    QVERIFY(series.receivedAtMs>0);
    QVERIFY(status->toolTip().isEmpty());QVERIFY(series.sourceUrl.startsWith("https://api.binance.com/"));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    QVERIFY(chart->accessibleDescription().contains("USDT"));QVERIFY(chart->accessibleName().contains(QStringLiteral("1분")));
    view.setPriceLines({{"upbit","synthetic-wrong-unit","SYN","KRW","103","https://api.upbit.com/v1/accounts",now},
        {"binance","synthetic-matching-unit","SYN","USDT","103","https://api.binance.com/api/v3/account",now}});
    QCOMPARE(view.priceLines().size(),1);QCOMPARE(view.priceLines().first().currency,QString("USDT"));
    const auto* details=view.findChild<QLabel*>("marketCandlesStudyDetails");
    QVERIFY(!details->text().contains(" KRW"));
    QTRY_VERIFY(details->text().contains(QStringLiteral("1분봉")));
}
void MarketCandlesViewTest::dailyCandlesKeepNativeBodiesFormingSeparationAndSourceClock(){
    class Fake final:public TradingTransport{public:quint64 token=0;RequestSpec request;
        void send(quint64 t,const RequestSpec& r)override{token=t;request=r;}void abort()override{}} transport;
    MarketCandles model(&transport);MarketCandlesView view(&model);
    constexpr qint64 day=86400000;
    view.setSelection("coin:upbit:KRW-SYN",day);view.resize(900,760);view.show();QTRY_VERIFY(transport.token);
    QCOMPARE(transport.request.url.path(),QString("/v1/candles/days"));
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto end=now/day*day;
    const auto tick=qMax(end,now-1000);QJsonArray rows;
    for(int i=0;i<=30;++i){const auto at=end-i*day;
        rows.append(QJsonObject{{"market","KRW-SYN"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(i?at+1000:tick)},{"opening_price","100"},{"high_price","110"},{"low_price","90"},
            {"trade_price",i?QString("101"):QString("109")},{"candle_acc_trade_volume","2"}});}
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
    const auto series=model.series();QVERIFY(series.available);QCOMPARE(series.candles.size(),30);QVERIFY(series.forming.has_value());
    QCOMPARE(series.sourceTimeMs,tick);QVERIFY(series.sourceTimeMs<series.receivedAtMs);
    auto* title=view.findChild<QLabel*>("marketCandlesTitle");QVERIFY(displayedText(title).contains(QStringLiteral("1일")));QVERIFY(displayedText(title).contains("KRW"));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QVERIFY(chart->accessibleName().contains(QStringLiteral("1일")));
    QVERIFY(chart->accessibleDescription().contains(QStringLiteral("형성 중 1개")));
    const auto image=chart->grab().toImage();
    const auto scale=image.devicePixelRatio();const auto ink=MovementColorPolicy::fillFor(chart,MovementDirection::Up);
    const auto strokesAt=[&](int y){QList<QPair<int,int>> strokes;int from=-1;
        for(int x=qCeil(8*scale);x<=qFloor((chart->width()-75)*scale);++x){const bool painted=x<qFloor((chart->width()-75)*scale)&&image.pixelColor(x,y)==ink;
            if(painted&&from<0)from=x;if(!painted&&from>=0){strokes.append({from,x-1});from=-1;}}return strokes;};
    const int bodyY=qRound((12+qMax(1,chart->height()-42)*.475)*scale);const auto edges=strokesAt(bodyY);
    QCOMPARE(edges.size(),60); // Two outlines for each of 30 completed bars; the forming bar is translucent.
    const double expectedWidth=qBound(1.0,double(chart->width()-83)/31*.65,18.0);
    for(int i=0;i<edges.size();i+=2){const auto left=(edges[i].first+edges[i].second)/2.0,right=(edges[i+1].first+edges[i+1].second)/2.0;
        QVERIFY2(std::abs((right-left)/scale-expectedWidth)<=1.0/scale,"Each hollow daily body must retain its native day width within one raster pixel");
        QCOMPARE(image.pixelColor(qRound((left+right)/2),bodyY),themeColors().bg);}
    const auto wicks=strokesAt(qRound((12+qMax(1,chart->height()-42)*.25)*scale));QCOMPARE(wicks.size(),30);
    for(const auto& wick:wicks)QCOMPARE(wick.second-wick.first+1,qRound(scale));
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");
    QVERIFY(status->isHidden());QVERIFY(title->toolTip().contains(QDateTime::fromMSecsSinceEpoch(tick,QTimeZone("Asia/Seoul")).toString("HH:mm")));QCOMPARE(model.series().sourceTimeMs,tick);
    QVERIFY(series.receivedAtMs>series.sourceTimeMs);QVERIFY(status->toolTip().isEmpty());
    QTRY_VERIFY(!view.findChild<QLabel*>("marketCandlesStudyDetails")->text().isEmpty());
    const auto details=view.findChild<QLabel*>("marketCandlesStudyDetails")->text();
    QVERIFY(details.contains(QStringLiteral("최근 연속 봉 30개")));QVERIFY(details.contains(QStringLiteral("1일 추세")));
    QVERIFY(!details.contains(QStringLiteral("아침 07")));QVERIFY(!details.contains(QStringLiteral("국장 09")));
}
void MarketCandlesViewTest::separateAccountsHaveDistinctVisibleLabelsAndExpire(){
    MarketCandlesView view(nullptr);view.setAsset("BTC");view.resize(700,500);view.show();
    const auto now=QDateTime::currentMSecsSinceEpoch();
    ChartPriceLine a{"upbit","same-prefix-account-a","BTC","KRW","103","https://api.upbit.com/v1/accounts",now-179500};
    auto b=a;b.accountScope="same-prefix-account-b";b.price="107";view.setPriceLines({a,b});
    const auto text=view.findChild<QLabel*>("marketCandlesPositionLines")->text();
    QVERIFY2(text.contains(QStringLiteral("계좌 1"))&&text.contains(QStringLiteral("계좌 2")),"Accounts with the same short scope prefix must have distinct visible labels");
    QTRY_VERIFY_WITH_TIMEOUT(view.priceLines().isEmpty(),2200);
    QVERIFY(view.findChild<QLabel*>("marketCandlesPositionLines")->text().isEmpty());
    QVERIFY(view.findChild<QLabel*>("marketCandlesPositionLines")->toolTip().isEmpty());
}
void MarketCandlesViewTest::formingCandleDoesNotPreventPanningToOldestClosedBar(){
    class Fake final:public TradingTransport{public:quint64 token=0;void send(quint64 t,const RequestSpec&)override{token=t;}void abort()override{}} transport;
    MarketCandles model(&transport);MarketCandlesView view(&model);view.setAsset("BTC");view.resize(900,650);view.show();QTRY_VERIFY(transport.token);
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto end=now/MarketCandles::IntervalMs*MarketCandles::IntervalMs;QJsonArray rows;
    for(int i=0;i<=60;++i){const auto at=end-i*MarketCandles::IntervalMs;
        rows.append(QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(i?at+1000:now)},{"unit","15"},{"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","101"},{"candle_acc_trade_volume","2"}});}
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QCOMPARE(model.series().candles.size(),60);
    QVERIFY(chart->accessibleDescription().contains(QStringLiteral("형성 중")));
    const auto latest=chart->grab().toImage();QTest::keyClick(chart,Qt::Key_Left);
    QVERIFY2(chart->grab().toImage()!=latest,"The oldest completed candle remains reachable when the forming candle takes the 61st slot");
}
void MarketCandlesViewTest::providerPriceLinesStaySeparateAndRejectMissingOrForeignCurrency(){
    MarketCandlesView view(nullptr);view.setAsset("BTC");const auto now=QDateTime::currentMSecsSinceEpoch();
    const ChartPriceLine first{"upbit","synthetic-a","BTC","KRW","103","https://api.upbit.com/v1/accounts",now};
    auto second=first;second.accountScope="synthetic-b";second.price="107";
    auto absent=first;absent.provider="binance";absent.price.clear();
    auto wrong=first;wrong.currency="USDT";wrong.price="8";
    auto stale=first;stale.receivedAtMs=now-180001;
    auto future=first;future.receivedAtMs=now+60000;
    view.setPriceLines({first,second,absent,wrong,stale,future});
    QCOMPARE(view.priceLines().size(),2);
    QCOMPARE(view.priceLines()[0].price,QString("103"));QCOMPARE(view.priceLines()[1].price,QString("107"));
    QVERIFY(view.priceLines()[0].accountScope!=view.priceLines()[1].accountScope);
    view.clearPrivate();QVERIFY(view.priceLines().isEmpty());
    view.setPriceLines({first});view.setAsset("ETH");QVERIFY(view.priceLines().isEmpty());
}
void MarketCandlesViewTest::reconnectKeepsHistoricalCandlesButNotLiveStudies_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void MarketCandlesViewTest::reconnectKeepsHistoricalCandlesButNotLiveStudies(){
    QFETCH(bool,dark);KeefiTheme::instance()->setSettings(nullptr);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);QVERIFY(KeefiFonts::registerBundledFonts());
    StudyTransport transport;auto socket=std::make_unique<StudySocket>();auto* wire=socket.get();
    qint64 now=StudyNow;MarketCandles source(&transport,std::move(socket),[&]{return now;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,850);view.show();
    const auto rows=syntheticStudyRows(80);QVERIFY(transport.token);
    emit transport.finished(transport.token,syntheticStudyResponse(rows));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    QTRY_VERIFY(view.chartStudies().available);QVERIFY(chart->isVisible());
    emit wire->failed(wire->generation);
    QVERIFY(!source.series().available);QVERIFY(source.series().candles.isEmpty());
    QVERIFY2(chart->isVisible(),"A reconnect must not erase already observed historical candle bodies");
    QVERIFY(chart->accessibleDescription().contains(QStringLiteral("지연")));
    QVERIFY(!view.chartStudies().available);QVERIFY(view.shareSnapshot({},now).closed.isEmpty());
    auto* status=view.findChild<QLabel*>("marketCandlesStatus");
    QVERIFY(displayedText(status).contains(QStringLiteral("지연")));
    if(const auto dir=qEnvironmentVariable("KEEFI_G8_RELIABILITY_RENDER_DIR");!dir.isEmpty()){
        QVERIFY(QDir().mkpath(dir));QVERIFY(view.grab().save(dir+"/historical-candles-"+(dark?"dark":"light")+".png"));
    }
    now+=1200;QTest::qWait(250); // reconnect and a new REST request, without a new observation
    QVERIFY(chart->isVisible());QVERIFY(!source.series().available);
    view.setSelection("coin:upbit:KRW-OTHER",60000);
    QVERIFY(!chart->isVisible());QVERIFY(chart->accessibleDescription().isEmpty());
}
void MarketCandlesViewTest::rendersOnlyVisibleSelectionAndClearsOnHide(){
    class Fake:public TradingTransport{public:QList<quint64> tokens;int aborts=0;
        void send(quint64 token,const RequestSpec&) override{tokens.append(token);}void abort() override{++aborts;}
    } transport;
    MarketCandles model(&transport);MarketCandlesView view(&model);view.setAsset("BTC");
    QCOMPARE(transport.tokens.size(),0);view.resize(600,320);view.show();QTRY_COMPARE(transport.tokens.size(),1);
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto end=now/MarketCandles::IntervalMs*MarketCandles::IntervalMs;
    QJsonArray rows;
    for(int i=1;i<=30;++i){const auto at=end-i*MarketCandles::IntervalMs;
        rows.append(QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(at+1000)},{"unit","15"},{"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","100"},{"candle_acc_trade_volume","2"}});}
    HttpResult r;r.status=200;r.body=QJsonDocument(rows).toJson();emit transport.finished(transport.tokens.last(),r);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    QVERIFY(chart->accessibleDescription().contains("30"));
    view.setPriceLines({{"upbit","synthetic-render-scope","BTC","KRW","103","https://api.upbit.com/v1/accounts",now}});
    QTRY_VERIFY(view.findChild<QLabel*>("marketCandlesStudy")->text().contains(QStringLiteral("압축 상태")));
    QVERIFY(view.findChild<QLabel*>("marketCandlesStatus")->isHidden());QVERIFY(model.series().receivedAtMs>0);
    const auto image=chart->grab().toImage();bool hasCandle=false;
    const auto scale=image.devicePixelRatio();const auto doji=MovementColorPolicy::colorFor(chart,MovementDirection::Flat);
    // Inspect every row: a one-pixel dashed line may fall entirely between 2px samples.
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const auto c=image.pixelColor(x,y);if(x>=8*scale&&x<(chart->width()-75)*scale&&y>=12*scale&&y<(chart->height()-30)*scale&&c==doji)hasCandle=true;}
    // The plot keeps headroom above and below the candles; use its published price range.
    const auto priceY=[chart](double price){return chart->property("viewportPlotTop").toDouble()+(chart->property("viewportPriceHigh").toDouble()-price)/
        (chart->property("viewportPriceHigh").toDouble()-chart->property("viewportPriceLow").toDouble())*chart->property("viewportPlotHeight").toDouble();};
    const auto averageY=priceY(103)*scale;int averagePixels=0;
    for(int y=qFloor(averageY)-1;y<=qCeil(averageY)+1;++y){int count=0;for(int x=qCeil(8*scale);x<qFloor((chart->width()-75)*scale);++x)count+=image.pixelColor(x,y)==themeColors().text;averagePixels=qMax(averagePixels,count);}
    QVERIFY(hasCandle);QVERIFY2(averagePixels>(chart->width()-83)*scale/2,"The provider average must remain visible at its exact price coordinate using the text-token 5/4 dash line");
    if(const auto preview=qEnvironmentVariable("KEEFI_G8_PREVIEW");!preview.isEmpty()){
        view.resize(700,520);QTest::qWait(30);QVERIFY(view.grab().save(QDir(preview).filePath("g8-candles.png")));
    }
    view.hide();QVERIFY(model.series().candles.isEmpty());QVERIFY(chart->accessibleDescription().isEmpty());
    QVERIFY(!view.findChild<QLabel*>("marketCandlesStudy")->text().contains(QStringLiteral("압축 상태")));
}
void MarketCandlesViewTest::shareSnapshotUsesObservedViewportAndCompletedPrefix(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();
    const auto rows=syntheticStudyRows(200);QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));QCOMPARE(source.series().candles.size(),200);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    chart->setFocus();QTest::keyClick(chart,Qt::Key_Left);QTest::keyClick(chart,Qt::Key_Left);
    const auto snapshot=view.shareSnapshot({},StudyNow);
    QCOMPARE(snapshot.availability,ChartShareAvailability::Ready);
    QCOMPARE(snapshot.identity.key(),QString("coin:upbit:KRW-SYN"));QCOMPARE(snapshot.currency,QString("KRW"));QCOMPARE(snapshot.intervalMs,qint64(60000));QCOMPARE(snapshot.asOfMs,StudyNow);
    QCOMPARE(snapshot.viewport.fromMs,rows[120].startsAtMs);QCOMPARE(snapshot.viewport.toMs,rows[179].startsAtMs);QCOMPARE(snapshot.closed.size(),180);
    QCOMPARE(snapshot.closed.last().startsAtMs,rows[179].startsAtMs);QCOMPARE(snapshot.calculatedThroughMs,rows[179].startsAtMs+60000);
    const auto canonical=keefiChartStudies(rows.mid(0,180),60000);QCOMPARE(snapshot.studies.observations,canonical.observations);QCOMPARE(snapshot.studies.lastCompletedAtMs,canonical.lastCompletedAtMs);
    QCOMPARE(snapshot.studies.pullback.score,canonical.pullback.score);QCOMPARE(snapshot.studies.similarity.samples,canonical.similarity.samples);QCOMPARE(snapshot.studies.scorecard.samples,canonical.scorecard.samples);
    QCOMPARE(snapshot.source.canonicalUrl,source.series().sourceUrl);QCOMPARE(snapshot.source.sourceTimeMs,source.series().sourceTimeMs);QCOMPARE(snapshot.source.receivedAtMs,source.series().receivedAtMs);
    QCOMPARE(source.series().candles.size(),200);QCOMPARE(source.series().candles.last().startsAtMs,rows.last().startsAtMs);QCOMPARE(source.series().candles.last().close,rows.last().close);
    // Pure render/save positives belong to renderer tests; this tests the actual
    // source/gesture/viewport assembler and fails at Ready with the no-op stub.
}

void MarketCandlesViewTest::shareSnapshotDoesNotReadOrCaptureActualPrivatePriceLines(){
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto delta=now/60000*60000-StudyNow/60000*60000;
    auto rows=syntheticStudyRows(60);for(auto& row:rows){row.startsAtMs+=delta;row.lastStoredTickMs+=delta;row.open=row.close="100";row.low="99";row.high="101";}
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,800);view.show();QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));QVERIFY(source.series(now).available);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto plain=chart->grab().toImage();
    const ChartPriceLine first{"SYNTHETIC_PRIVATE_PROVIDER_A","PRIVATE_ACCOUNT_CANARY_A","SYN","KRW","100.25","https://api.upbit.com/v1/accounts",now};
    view.setPriceLines({first});QCOMPARE(view.priceLines().size(),1);const auto privateA=chart->grab().toImage();QVERIFY(privateA!=plain);
    const auto snapshotA=view.shareSnapshot({},now);QCOMPARE(snapshotA.availability,ChartShareAvailability::Ready);
    auto second=first;second.provider="SYNTHETIC_PRIVATE_PROVIDER_B";second.accountScope="PRIVATE_ACCOUNT_CANARY_B";second.price="100.75";view.setPriceLines({second});QCOMPARE(view.priceLines().size(),1);const auto privateB=chart->grab().toImage();QVERIFY(privateA!=privateB);
    const auto snapshotB=view.shareSnapshot({},now);QCOMPARE(snapshotB.availability,ChartShareAvailability::Ready);
    const auto renderedA=ChartShareCard::render(snapshotA),renderedB=ChartShareCard::render(snapshotB);QVERIFY(renderedA.available);QVERIFY(renderedB.available);QCOMPARE(renderedA.image,renderedB.image);QCOMPARE(renderedA.publicCaptions,renderedB.publicCaptions);
    for(const auto& text:renderedB.publicCaptions){QVERIFY(!text.contains("PRIVATE_ACCOUNT_CANARY"));QVERIFY(!text.contains("SYNTHETIC_PRIVATE_PROVIDER"));}QVERIFY(renderedB.image.textKeys().isEmpty());
    QTemporaryDir files;QVERIFY(files.isValid());QString error;const auto pathA=files.filePath("public-a.png"),pathB=files.filePath("public-b.png");QVERIFY2(ChartShareCard::savePng(snapshotA,{},pathA,&error),qPrintable(error));QVERIFY2(ChartShareCard::savePng(snapshotB,{},pathB,&error),qPrintable(error));
    QFile a(pathA),b(pathB);QVERIFY(a.open(QIODevice::ReadOnly));QVERIFY(b.open(QIODevice::ReadOnly));const auto aBytes=a.readAll(),bBytes=b.readAll();QCOMPARE(aBytes,bBytes);QVERIFY(!bBytes.contains("PRIVATE_ACCOUNT_CANARY"));QVERIFY(!bBytes.contains("SYNTHETIC_PRIVATE_PROVIDER"));
    view.clearPrivate();QVERIFY(view.priceLines().isEmpty());const auto afterLock=view.shareSnapshot({},now);QCOMPARE(ChartShareCard::render(afterLock).image,renderedA.image);
}

void MarketCandlesViewTest::shareSnapshotRejectsPriorSelectionAndKeepsItsCopiedImageImmutable(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();const auto oldToken=transport.token;QVERIFY(oldToken);emit transport.finished(oldToken,syntheticStudyResponse(syntheticStudyRows(60)));
    const auto snapshot=view.shareSnapshot({},StudyNow);QCOMPARE(snapshot.availability,ChartShareAvailability::Ready);const auto frozen=ChartShareCard::render(snapshot);QVERIFY(frozen.available);
    view.setSelection("coin:upbit:KRW-ALT",60000);QVERIFY(transport.token!=oldToken);emit transport.finished(oldToken,syntheticStudyResponse(syntheticStudyRows(60)));
    const auto waiting=view.shareSnapshot({},StudyNow);QVERIFY(waiting.availability!=ChartShareAvailability::Ready);QVERIFY(waiting.closed.isEmpty());
    const auto sameOld=ChartShareCard::render(snapshot);QVERIFY(sameOld.available);QCOMPARE(sameOld.image,frozen.image);QCOMPARE(sameOld.publicCaptions,frozen.publicCaptions);QCOMPARE(snapshot.identity.key(),QString("coin:upbit:KRW-SYN"));
}

void MarketCandlesViewTest::sharedDrawingsRequirePublicOriginAndExplicitGeometryOptIn(){
    StudyTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1050,950);view.show();
    const auto rows=flatDrawingRows();QVERIFY(transport.token);emit transport.finished(transport.token,syntheticStudyResponse(rows));QCOMPARE(source.series().candles.size(),6);QVERIFY(drawings.context().publicPersistenceAllowed);
    const auto frame=[&]{ChartDrawingFrame out;out.identity=drawings.context().identity;out.currency="KRW";out.intervalMs=60000;out.asOfMs=StudyNow;out.contextGeneration=drawings.contextGeneration();for(const auto& row:source.series().candles)out.bars.append({row,row.startsAtMs+60000,true,!out.bars.isEmpty()});return out;};
    const QList<ChartDrawingPoint> publicAnchors{{rows.first().startsAtMs,"100"}};QVERIFY(!drawings.create(ChartDrawingKind::Horizontal,publicAnchors,frame()).isEmpty());QCOMPARE(drawings.drawings().size(),1);
    QVERIFY(view.shareSnapshot({},StudyNow).drawings.isEmpty());ChartShareOptions include;include.includePublicDrawings=true;
    const auto publicShare=view.shareSnapshot(include,StudyNow);QCOMPARE(publicShare.drawings.size(),1);QCOMPARE(publicShare.drawings.first().kind,ChartDrawingKind::Horizontal);QCOMPARE(publicShare.drawings.first().anchors,publicAnchors);
    view.setDrawingPublicContext(false);QVERIFY(!drawings.context().publicPersistenceAllowed);QVERIFY(drawings.drawings().isEmpty());
    const QList<ChartDrawingPoint> privateAnchors{{rows.first().startsAtMs,"100.5"}};QVERIFY(!drawings.create(ChartDrawingKind::Horizontal,privateAnchors,frame()).isEmpty());QCOMPARE(drawings.drawings().size(),1);
    const auto holdingShare=view.shareSnapshot(include,StudyNow);QCOMPARE(holdingShare.availability,ChartShareAvailability::Ready);QCOMPARE(holdingShare.closed.size(),6);QVERIFY(holdingShare.drawings.isEmpty());
    drawings.clearPrivate();view.clearPrivateDrawingContext();const auto lockedShare=view.shareSnapshot(include,StudyNow);QCOMPARE(lockedShare.availability,ChartShareAvailability::Ready);QVERIFY(lockedShare.drawings.isEmpty());QCOMPARE(lockedShare.closed.size(),6);
}
void MarketCandlesViewTest::unconnectedStockCannotSharePreviouslyObservedCoinData(){
    StudyTransport transport;MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    MarketCandlesView view(&model);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();
    emit transport.finished(transport.token,syntheticStudyResponse(syntheticStudyRows(40)));
    QCOMPARE(view.shareSnapshot({},StudyNow).availability,ChartShareAvailability::Ready);
    view.setSelection("stock:KR:005930",86400000);
    const auto snapshot=view.shareSnapshot({},StudyNow);QCOMPARE(snapshot.availability,ChartShareAvailability::Unavailable);
    QVERIFY(snapshot.closed.isEmpty());QVERIFY(!snapshot.stockTimeline.has_value());QCOMPARE(snapshot.calculatedThroughMs,qint64(0));
    QVERIFY(!snapshot.studies.available);QVERIFY(!snapshot.studies.scorecard.available);QVERIFY(!snapshot.studies.similarity.available);
    const auto rendered=ChartShareCard::render(snapshot);QVERIFY(!rendered.available);QVERIFY(rendered.image.isNull());
}
void MarketCandlesViewTest::chartSyncNavigationSeparatesUserDataAndRemoteOrigins(){
    NavigationFixture f;auto* chart=f.chart();QVERIFY(chart);QTRY_VERIFY(f.view.syncViewport().valid());
    QSignalSpy user(&f.view,&MarketCandlesView::userViewportNavigated),hover(&f.view,&MarketCandlesView::crosshairHovered);
    const auto original=f.view.syncViewport();f.tick();QCOMPARE(user.size(),0);
    const ChartSyncViewport other{original.fromMs-10*f.interval,original.toMs-20*f.interval};
    const auto before=f.view.syncViewport();f.view.applySyncedViewport(other,false);
    QCOMPARE(user.size(),0);QCOMPARE(f.view.syncViewport().centreMs(),other.centreMs());
    QCOMPARE(f.view.syncViewport().toMs-f.view.syncViewport().fromMs,before.toMs-before.fromMs);
    f.view.applySyncedViewport(other,true);QCOMPARE(f.view.syncViewport(),other);QCOMPARE(user.size(),0);
    chart->setFocus();QTest::keyClick(chart,Qt::Key_Left);QCOMPARE(user.size(),1);QVERIFY(!user.last().at(1).toBool());
    QTest::keyClick(chart,Qt::Key_Plus);QCOMPARE(user.size(),2);QVERIFY(user.last().at(1).toBool());
    const auto after=f.view.syncViewport();f.tick();QCOMPARE(user.size(),2);QCOMPARE(f.view.syncViewport(),after);
    const QPoint at(qRound(chart->property("viewportPlotLeft").toDouble()+chart->property("viewportPlotWidth").toDouble()*.5),
                    qRound(chart->property("viewportPlotTop").toDouble()+40));
    QTest::mouseMove(chart,at);QVERIFY(!hover.isEmpty());QVERIFY(hover.last().first().toLongLong()>0);
    QEvent leave(QEvent::Leave);QApplication::sendEvent(chart,&leave);QCOMPARE(hover.last().first().toLongLong(),0);
    QCOMPARE(user.size(),2);
    QVERIFY(f.view.navigateToTime(f.end-100*f.interval));QCOMPARE(user.size(),2);
    auto* detail=f.view.findChild<QLabel*>("chartCoreStudyDetail_similarity");QVERIFY(detail);
    QVERIFY(QMetaObject::invokeMethod(detail,"linkActivated",Q_ARG(QString,QString("match:%1").arg(f.end-80*f.interval))));
    QCOMPARE(user.size(),3);QVERIFY(!user.last().at(1).toBool());
    QVERIFY(!chart->property("viewportLive").toBool());QVERIFY(chart->property("viewportOffsetSlots").toDouble()>0);
    auto* live=f.view.findChild<QPushButton*>("chartReturnToLive");QVERIFY(live);QVERIFY(live->isEnabled());
    QTest::mouseClick(live,Qt::LeftButton);QVERIFY(chart->property("viewportLive").toBool());QCOMPARE(user.size(),4);
}
void MarketCandlesViewTest::chartSyncCrosshairUsesTargetPricesAndClearsGaps(){
    NavigationHttp http;MarketCandles model(&http,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;for(const auto& id:preferences.supportedIds("coin"))preferences.setEnabled(id,false);
    MarketCandlesView view(&model);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",60000);view.resize(1100,900);view.show();
    auto rows=syntheticStudyRows(120,false);const qint64 gap=rows[80].startsAtMs;rows.removeAt(80);
    QVERIFY(!http.tokens.isEmpty());emit http.finished(http.tokens.last(),syntheticStudyResponse(rows));
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);QTRY_VERIFY(view.syncViewport().valid());
    QSignalSpy user(&view,&MarketCandlesView::userViewportNavigated),hover(&view,&MarketCandlesView::crosshairHovered);
    const int requests=http.requests.size();const qint64 valid=rows[100].startsAtMs+15000;
    view.setSyncedCrosshair(valid);QCOMPARE(chart->property("syncCrosshairMs").toLongLong(),valid);
    QCOMPARE(chart->property("syncCrosshairPrice").toDouble(),100.0);
    view.setSyncedCrosshair(gap+30000);QCOMPARE(chart->property("syncCrosshairMs").toLongLong(),0);
    view.setSyncedCrosshair(rows.first().startsAtMs);QCOMPARE(chart->property("syncCrosshairMs").toLongLong(),0);
    view.setSyncedCrosshair(valid);view.setSyncedCrosshair(0);QCOMPARE(chart->property("syncCrosshairMs").toLongLong(),0);
    for(int i=0;i<40;++i)view.setSyncedCrosshair(valid+i);
    QCOMPARE(http.requests.size(),requests);QCOMPARE(user.size(),0);QCOMPARE(hover.size(),0);
    view.applySyncedViewport({rows.first().startsAtMs,rows.first().startsAtMs+20*60000},true);
    QCOMPARE(chart->property("syncCrosshairMs").toLongLong(),0);QCOMPARE(user.size(),0);
}
namespace {
QList<MarketCandle> autoGeometryStudyRows(){
    const qint64 end=StudyNow/60000*60000;
    const QList<int> anchors{1000,1010,1025,1045,1070,1100,1135,1170,1200,1180,1150,1120,1095,1075,1090,1115,1145,1185,1230,1270,1300,1275,1240,1210,1190,1215,1245,1275,1295,1265,1235,1210,1195,1210,1220,1215,1218,1212,1216,1214,1217,1213,1215,1216,1214,1215};
    QList<MarketCandle> rows;
    for(int i=0;i<anchors.size();++i){
        const int c=anchors[i];
        const int prev=i>0?anchors[i-1]:c;
        const int o=(prev+c)/2;
        const int hi=qMax(o,c)+6;
        const int lo=qMin(o,c)-6;
        const qint64 at=end-(anchors.size()-i)*60000;
        rows.append({at,at+1000,QString::number(o),QString::number(hi),QString::number(lo),QString::number(c),"5"});
    }
    return rows;
}
struct AutoDrawingFixture {
    StudyTransport transport;
    MarketCandles source{&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;}};
    ChartDrawings drawings;
    ChartStudyPreferences preferences;
    MarketCandlesView view{&source};
    explicit AutoDrawingFixture(const QList<MarketCandle>& rows=autoGeometryStudyRows()){
        preferences.markIntroSeen();
        for(const auto* id:{"pullback","similarity","cloud","volume","whales"})preferences.setEnabled(id,false);
        view.setIndicatorPreferences(&preferences);
        view.setDrawingModel(&drawings);
        view.setDrawingPublicContext(true);
        view.setSelection("coin:upbit:KRW-SYN",60000);
        view.resize(1200,900);
        view.show();
        emit transport.finished(transport.token,syntheticStudyResponse(rows));
        QCoreApplication::processEvents();
    }
    QWidget* chart()const{return view.findChild<QWidget*>("marketCandlesChart");}
    QToolButton* autoBtn()const{return view.findChild<QToolButton*>("chartDrawingAuto");}
    QToolButton* objectsBtn()const{return view.findChild<QToolButton*>("chartDrawingObjects");}
    QWidget* objectsPanel()const{return view.findChild<QWidget*>("chartDrawingObjectsPanel");}
    QWidget* previewSection()const{return view.findChild<QWidget*>("chartDrawingPreviewSection");}
    QList<QCheckBox*> previewChecks()const{
        QList<QCheckBox*> out;
        if(auto* sec=previewSection())for(auto* cb:sec->findChildren<QCheckBox*>())if(cb->objectName().startsWith("chartDrawingPreviewCheck_"))out.append(cb);
        return out;
    }
    QPushButton* applyBtn()const{return view.findChild<QPushButton*>("chartDrawingAutoApply");}
};
int countColorPixels(const QImage& image,const QColor& ink){
    int count=0;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==ink)++count;
    return count;
}
}
void MarketCandlesViewTest::autoDrawingButtonRequiresContextClosedUnlockedVisibleAndVerifiedStock(){
    AutoDrawingFixture f;
    auto* btn=f.autoBtn();
    QVERIFY(btn);QVERIFY(btn->isVisible());QVERIFY(btn->isEnabled());
    // Locked disables
    f.drawings.setLocked(true);QCoreApplication::processEvents();QVERIFY(!btn->isEnabled());QVERIFY(!btn->toolTip().isEmpty());
    f.drawings.setLocked(false);QCoreApplication::processEvents();QVERIFY(btn->isEnabled());
    // Hidden disables
    f.drawings.setVisible(false);QCoreApplication::processEvents();QVERIFY(!btn->isEnabled());
    f.drawings.setVisible(true);QCoreApplication::processEvents();QVERIFY(btn->isEnabled());
    // No completed bars disables
    f.view.setSelection("coin:upbit:KRW-SYN",300000);QCoreApplication::processEvents();QVERIFY(!btn->isEnabled());
    // Unverified vs verified stock with real completed stock candles
    qint64 stockNow=QDateTime::fromString("2026-10-23T13:35:00Z",Qt::ISODate).toMSecsSinceEpoch();
    class StockHttp final:public TradingTransport{
      public:QList<RequestSpec> sent;QList<quint64> tokens;
        void send(quint64 t,const RequestSpec& r)override{sent.append(r);tokens.append(t);}void abort()override{}
    } stockHttp;
    StockChartSource stockSource(&stockHttp,nullptr,[&]{return stockNow;});
    stockSource.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_AUTO",3600000,1);return true;});
    ChartDrawings stockDrawings;MarketCandlesView stockView(nullptr);
    stockView.setStockSource(&stockSource);stockView.setDrawingModel(&stockDrawings);stockView.setDrawingPublicContext(true);
    stockView.setSelection("stock:US:SYN",60000);stockView.resize(1000,800);stockView.show();
    const auto stockJson=[&](const QJsonValue& val){HttpResult r;r.status=200;r.body=QJsonDocument(QJsonObject{{"result",val}}).toJson(QJsonDocument::Compact);return r;};
    int handled=0;
    for(int step=0;step<8&&!stockSource.snapshot().series.available;++step){
        if(stockHttp.sent.size()>handled){
            const auto req=stockHttp.sent[handled];const auto tok=stockHttp.tokens[handled++];
            if(req.url.path()=="/api/v1/stocks"){
                emit stockHttp.finished(tok,stockJson(QJsonArray{QJsonObject{{"symbol","SYN"},{"name","합성"},{"englishName","Synthetic"},{"isinCode","US0000000000"},{"market","NASDAQ"},{"securityType","STOCK"},{"isCommonShare",true},{"status","ACTIVE"},{"currency","USD"},{"sharesOutstanding","1000"},{"listDate",QJsonValue::Null},{"delistDate",QJsonValue::Null},{"leverageFactor",QJsonValue::Null},{"koreanMarketDetail",QJsonValue::Null}}}));
            }else if(req.url.path().startsWith("/api/v1/market-calendar/")){
                const auto usDay=[](const QString& date){
                    return QJsonObject{{"date",date},{"dayMarket",QJsonValue::Null},{"preMarket",QJsonValue::Null},{"regularMarket",QJsonObject{{"startTime",date+"T13:30:00Z"},{"endTime",date+"T20:00:00Z"}}},{"afterMarket",QJsonValue::Null}};
                };
                emit stockHttp.finished(tok,stockJson(QJsonObject{{"today",usDay("2026-10-23")},{"previousBusinessDay",usDay("2026-10-22")},{"nextBusinessDay",usDay("2026-10-26")}}));
            }else if(req.url.path()=="/api/v1/candles"){
                const QJsonArray bars{QJsonObject{{"timestamp","2026-10-23T13:34:00Z"},{"openPrice","100"},{"highPrice","102"},{"lowPrice","99"},{"closePrice","101"},{"volume","10"},{"currency","USD"}}};
                emit stockHttp.finished(tok,stockJson(QJsonObject{{"candles",bars},{"nextBefore",QJsonValue::Null}}));
            }
        }
        stockNow+=1000;stockSource.refresh();QCoreApplication::processEvents();
    }
    QVERIFY2(stockSource.snapshot().series.available,qPrintable(stockSource.snapshot().series.reason));
    auto* stockAutoBtn=stockView.findChild<QToolButton*>("chartDrawingAuto");QVERIFY(stockAutoBtn);
    QVERIFY2(!stockAutoBtn->isEnabled(),"Unverified stock must disable auto drawing even with completed candles");
    stockDrawings.setVerifiedInstruments({{"stock","US","SYN"}});QCoreApplication::processEvents();
    QVERIFY(stockAutoBtn->isEnabled());
}
void MarketCandlesViewTest::autoDrawingButtonStartsPreviewWithoutChangingPublicState(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    const auto beforeImg=chart->grab().toImage();
    const auto beforeState=f.drawings.publicState();
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QVERIFY(f.objectsPanel()->isVisible());
    QVERIFY(f.previewSection()->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(chart->property("autoPreviewProgress").toDouble(),1.0,2000);
    QCoreApplication::processEvents();
    QVERIFY2(chart->grab().toImage()!=beforeImg,"Auto preview must paint overlay ink on chart");
    const auto checks=f.previewChecks();
    QVERIFY(!checks.isEmpty());
    for(auto* cb:checks)QVERIFY(cb->isChecked());
    QCOMPARE(f.drawings.publicState(),beforeState);
    QVERIFY(f.drawings.drawings().isEmpty());
}
void MarketCandlesViewTest::escapeDuringAutoPreviewFinishesAtOneThenCancelsOnSecondEscape(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    auto* anim=chart->findChild<QVariantAnimation*>("chartAutoPreviewMotion");
    QVERIFY(anim);
    QCOMPARE(anim->state(),QAbstractAnimation::Running);
    QVERIFY(chart->property("autoPreviewProgress").toDouble()<1.0);
    QTest::keyClick(chart,Qt::Key_Escape);
    QCOMPARE(anim->state(),QAbstractAnimation::Stopped);
    QCOMPARE(chart->property("autoPreviewProgress").toDouble(),1.0);
    QVERIFY(chart->property("autoPreviewActive").toBool());
    QVERIFY(f.previewSection()->isVisible());
    QTest::keyClick(chart,Qt::Key_Escape);
    QVERIFY(!chart->property("autoPreviewActive").toBool());
    QVERIFY(f.previewSection()->isHidden());
}
void MarketCandlesViewTest::autoPreviewApplyBatchesSingleUndoReplacesAndRespectsCheckboxes(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QTest::keyClick(chart,Qt::Key_Escape);
    const int n=int(f.previewChecks().size());QVERIFY(n>=2);
    // Compare preview ink before an identical applied drawing can cover the same pixels.
    auto* retracement=f.previewSection()->findChild<QCheckBox*>("chartDrawingPreviewCheck_fib-retracement");
    QVERIFY(retracement);
    const auto previewFull=chart->grab().toImage();
    retracement->setChecked(false);QCoreApplication::processEvents();
    QVERIFY(chart->grab().toImage()!=previewFull);
    retracement->setChecked(true);
    QTest::mouseClick(f.applyBtn(),Qt::LeftButton);QCoreApplication::processEvents();
    QCOMPARE(f.drawings.drawings().size(),n);
    const auto batch1=f.drawings.drawings().first().autoBatchId;QVERIFY(!batch1.isEmpty());
    for(const auto& d:f.drawings.drawings())QCOMPARE(d.autoBatchId,batch1);
    QVERIFY(f.previewSection()->isHidden());
    // Single undo removes entire batch
    QTest::mouseClick(f.view.findChild<QToolButton*>("chartDrawingUndo"),Qt::LeftButton);
    QVERIFY(f.drawings.drawings().isEmpty());
    // Apply again, then re-apply replaces without growing count
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);QTest::keyClick(chart,Qt::Key_Escape);
    QTest::mouseClick(f.applyBtn(),Qt::LeftButton);QCOMPARE(f.drawings.drawings().size(),n);
    const auto batch2=f.drawings.drawings().first().autoBatchId;
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);QTest::keyClick(chart,Qt::Key_Escape);
    QTest::mouseClick(f.applyBtn(),Qt::LeftButton);QCOMPARE(f.drawings.drawings().size(),n);
    QVERIFY(f.drawings.drawings().first().autoBatchId!=batch2);
    // Applying after unchecking one candidate writes N-1 in the replacement batch.
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);QTest::keyClick(chart,Qt::Key_Escape);
    auto checks=f.previewChecks();QVERIFY(checks.size()==n);
    checks.first()->setChecked(false);QCoreApplication::processEvents();
    QTest::mouseClick(f.applyBtn(),Qt::LeftButton);
    QCOMPARE(f.drawings.drawings().size(),n-1);
}
void MarketCandlesViewTest::timeframeSwitchClearsAutoPreviewAndPreviewRowsImmediately(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QVERIFY(chart->property("autoPreviewActive").toBool());
    QVERIFY(f.previewSection()->isVisible());QVERIFY(!f.previewChecks().isEmpty());
    f.view.setSelection("coin:upbit:KRW-SYN",300000);QCoreApplication::processEvents();
    QVERIFY(!chart->property("autoPreviewActive").toBool());
    QVERIFY(f.previewSection()->isHidden());
    QVERIFY(f.previewChecks().isEmpty());
}
void MarketCandlesViewTest::closedCandleCorrectionClearsAutoPreviewWithoutNewBar(){
    AutoDrawingFixture f;
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QVERIFY(f.previewSection()->isVisible());
    QVERIFY(!f.previewChecks().isEmpty());

    const auto original=f.source.series().candles;
    QVERIFY(original.size()>2);
    auto token=f.transport.token;
    f.source.clearDisplayCache();
    QVERIFY(f.transport.token!=token);
    emit f.transport.finished(f.transport.token,syntheticStudyResponse(original));
    QVERIFY(f.previewSection()->isVisible());

    auto corrected=original;
    const auto changedIndex=corrected.size()/2;
    corrected[changedIndex].high=QStringLiteral("1350");
    QVERIFY(corrected.at(changedIndex).high!=original.at(changedIndex).high);
    token=f.transport.token;
    f.source.clearDisplayCache();
    QVERIFY(f.transport.token!=token);
    QVERIFY(f.previewSection()->isVisible());
    emit f.transport.finished(f.transport.token,syntheticStudyResponse(corrected));

    const auto current=f.source.series().candles;
    QCOMPARE(current.size(),original.size());
    QCOMPARE(current.last().startsAtMs,original.last().startsAtMs);
    QCOMPARE(current.at(changedIndex).high,corrected.at(changedIndex).high);
    QVERIFY(f.previewSection()->isHidden());
    QVERIFY(f.previewChecks().isEmpty());
    QVERIFY(!f.chart()->property("autoPreviewActive").toBool());
    QVERIFY(f.drawings.drawings().isEmpty());
}
void MarketCandlesViewTest::objectsPanelEyeLockDeleteInPlaceUpdateAndFocusPreservation(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    const auto rows=f.source.series().candles;
    ChartDrawingFrame frame;frame.identity=f.drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;
    frame.asOfMs=StudyNow;frame.contextGeneration=f.drawings.contextGeneration();
    for(const auto& r:rows)frame.bars.append({r,r.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const auto id1=f.drawings.create(ChartDrawingKind::Trend,{{rows[5].startsAtMs,rows[5].low},{rows[15].startsAtMs,rows[15].high}},frame);
    const auto id2=f.drawings.create(ChartDrawingKind::Measure,{{rows[10].startsAtMs,"1220"},{rows[14].startsAtMs,"1250"}},frame);
    QVERIFY(!id1.isEmpty()&&!id2.isEmpty());
    f.objectsBtn()->click();QCoreApplication::processEvents();QVERIFY(f.objectsPanel()->isVisible());
    auto* row1=f.view.findChild<QWidget*>("chartDrawingObjectRow_"+id1);
    auto* row2=f.view.findChild<QWidget*>("chartDrawingObjectRow_"+id2);
    QVERIFY(row1&&row2);
    // Focus stays in unrelated input across model changes
    QLineEdit unrelated(&f.view);unrelated.show();unrelated.setFocus();QCoreApplication::processEvents();
    QCOMPARE(QApplication::focusWidget(),&unrelated);
    // Eye toggle off removes ink, keeps widget identity and unrelated focus
    const auto shownImg=chart->grab().toImage();
    QVERIFY(countColorPixels(shownImg,themeColors().studyBlue)>0);
    auto* eye1=f.view.findChild<QToolButton*>("chartDrawingObjectEye_"+id1);QVERIFY(eye1);
    eye1->click();QCoreApplication::processEvents();
    QVERIFY(!f.drawings.drawings().first().visible);
    QCOMPARE(f.view.findChild<QWidget*>("chartDrawingObjectRow_"+id1),row1);
    QCOMPARE(f.view.findChild<QWidget*>("chartDrawingObjectRow_"+id2),row2);
    QCOMPARE(QApplication::focusWidget(),&unrelated);
    QCOMPARE(countColorPixels(chart->grab().toImage(),themeColors().studyBlue),0);
    eye1->click();QCoreApplication::processEvents();QVERIFY(countColorPixels(chart->grab().toImage(),themeColors().studyBlue)>0);
    // Lock prevents drag & disables delete
    auto* lock1=f.view.findChild<QToolButton*>("chartDrawingObjectLock_"+id1);
    auto* del1=f.view.findChild<QToolButton*>("chartDrawingObjectDelete_"+id1);
    QVERIFY(lock1&&del1);lock1->click();QCoreApplication::processEvents();
    QVERIFY(f.drawings.drawings().first().locked);QVERIFY(!del1->isEnabled());
    const auto anchorsBefore=f.drawings.drawings().first().anchors;
    QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,chart->rect().center());
    QTest::mouseMove(chart,chart->rect().center()+QPoint(25,-25));
    QTest::mouseRelease(chart,Qt::LeftButton,Qt::NoModifier,chart->rect().center()+QPoint(25,-25));
    QCOMPARE(f.drawings.drawings().first().anchors,anchorsBefore);
    // Delete removes id2 while row1 pointer remains identical in-place
    auto* del2=f.view.findChild<QToolButton*>("chartDrawingObjectDelete_"+id2);QVERIFY(del2&&del2->isEnabled());
    del2->click();QCoreApplication::processEvents();
    QCOMPARE(f.drawings.drawings().size(),1);
    QCOMPARE(f.view.findChild<QWidget*>("chartDrawingObjectRow_"+id1),row1);
    QVERIFY(!f.view.findChild<QWidget*>("chartDrawingObjectRow_"+id2));
    // Esc inside panel closes panel and returns focus to chart
    eye1->setFocus();QCoreApplication::processEvents();QVERIFY(f.objectsPanel()->isAncestorOf(QApplication::focusWidget()));
    QTest::keyClick(eye1,Qt::Key_Escape);QCoreApplication::processEvents();
    QVERIFY(f.objectsPanel()->isHidden());QCOMPARE(QApplication::focusWidget(),chart);
}
void MarketCandlesViewTest::fibLabelsRespect18PxMinimumSpacingAndOffscreenAxisTags(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    const auto rows=f.source.series().candles;
    ChartDrawingFrame frame;frame.identity=f.drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;
    frame.asOfMs=StudyNow;frame.contextGeneration=f.drawings.contextGeneration();
    for(const auto& r:rows)frame.bars.append({r,r.startsAtMs+60000,true,!frame.bars.isEmpty()});
    // Wide FibExtension whose 1.272/1.414/1.618/2.0/2.618 go far above viewport, plus a narrow FibRetracement whose 7 levels are clustered within ~27px (<18px apart)
    const auto fibWide=f.drawings.create(ChartDrawingKind::FibExtension,{{rows[2].startsAtMs,"900"},{rows[10].startsAtMs,"1250"},{rows[15].startsAtMs,"1150"}},frame);
    const auto fibTight=f.drawings.create(ChartDrawingKind::FibRetracement,{{rows[18].startsAtMs,"1100"},{rows[25].startsAtMs,"1110"}},frame);
    QVERIFY(!fibWide.isEmpty()&&!fibTight.isEmpty());
    chart->grab();QCoreApplication::processEvents();
    const QStringList labels=chart->property("drawingLabelTexts").toStringList();
    int tightCount=0;for(const auto& lbl:labels)if(lbl.contains("1,10"))++tightCount;
    QVERIFY2(tightCount>=1&&tightCount<=2,qPrintable(labels.join(", ")));
    const QString topTag=chart->property("drawingOffscreenTopLabel").toString();
    QVERIFY2(topTag.startsWith("▲ "),qPrintable(topTag));
    QVERIFY2(topTag.contains("1.272"),qPrintable("Closest offscreen high level expected: "+topTag));
    // Downward FibExtension gives offscreen bottom levels
    const auto fibDown=f.drawings.create(ChartDrawingKind::FibExtension,{{rows[3].startsAtMs,"1300"},{rows[11].startsAtMs,"1050"},{rows[16].startsAtMs,"1120"}},frame);
    QVERIFY(!fibDown.isEmpty());chart->grab();QCoreApplication::processEvents();
    const QString bottomTag=chart->property("drawingOffscreenBottomLabel").toString();
    QVERIFY2(bottomTag.startsWith("▼ "),qPrintable(bottomTag));
    QVERIFY2(bottomTag.contains("1.272"),qPrintable("Closest offscreen low level expected: "+bottomTag));
}
void MarketCandlesViewTest::appliedDrawingsDoNotAlterAutoPriceScaleBounds(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    QCoreApplication::processEvents();
    const double lowBefore=chart->property("viewportPriceLow").toDouble();
    const double highBefore=chart->property("viewportPriceHigh").toDouble();
    QVERIFY(highBefore>lowBefore);
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);QTest::keyClick(chart,Qt::Key_Escape);
    QCOMPARE(chart->property("viewportPriceLow").toDouble(),lowBefore);
    QCOMPARE(chart->property("viewportPriceHigh").toDouble(),highBefore);
    QTest::mouseClick(f.applyBtn(),Qt::LeftButton);QCoreApplication::processEvents();
    QCOMPARE(chart->property("viewportPriceLow").toDouble(),lowBefore);
    QCOMPARE(chart->property("viewportPriceHigh").toDouble(),highBefore);
    const auto rows=f.source.series().candles;
    ChartDrawingFrame frame;frame.identity=f.drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;
    frame.asOfMs=StudyNow;frame.contextGeneration=f.drawings.contextGeneration();
    for(const auto& r:rows)frame.bars.append({r,r.startsAtMs+60000,true,!frame.bars.isEmpty()});
    QVERIFY(!f.drawings.create(ChartDrawingKind::Horizontal,{{rows[10].startsAtMs,"5000"}},frame).isEmpty());
    f.view.resize(1202,900);QCoreApplication::processEvents();
    QCOMPARE(chart->property("viewportPriceLow").toDouble(),lowBefore);
    QCOMPARE(chart->property("viewportPriceHigh").toDouble(),highBefore);
}
void MarketCandlesViewTest::lockedShapeHoverShowsArrowCursorInsteadOfOpenHand(){
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    const auto rows=f.source.series().candles;
    ChartDrawingFrame frame;frame.identity=f.drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;
    frame.asOfMs=StudyNow;frame.contextGeneration=f.drawings.contextGeneration();
    for(const auto& r:rows)frame.bars.append({r,r.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const auto id=f.drawings.create(ChartDrawingKind::Horizontal,{{rows[10].startsAtMs,"1150"}},frame);
    QVERIFY(!id.isEmpty());QCoreApplication::processEvents();
    const int y=qRound(chart->property("viewportPlotTop").toDouble()+(chart->property("viewportPriceHigh").toDouble()-1150.0)/
        (chart->property("viewportPriceHigh").toDouble()-chart->property("viewportPriceLow").toDouble())*chart->property("viewportPlotHeight").toDouble());
    const QPoint onLine(qRound(chart->property("viewportPlotLeft").toDouble()+chart->property("viewportPlotWidth").toDouble()*.5),y);
    QTest::mouseMove(chart,onLine);QCOMPARE(chart->cursor().shape(),Qt::OpenHandCursor);
    QVERIFY(f.drawings.setDrawingLocked(id,true));QCoreApplication::processEvents();
    QTest::mouseMove(chart,onLine+QPoint(1,0));
    QVERIFY(chart->cursor().shape()!=Qt::OpenHandCursor);
    QCOMPARE(chart->cursor().shape(),Qt::ArrowCursor);
}
void MarketCandlesViewTest::autoDrawingFailureShowsVisibleTransientFeedbackAndClearsOnSwitch(){
    AutoDrawingFixture f(syntheticStudyRows(10));
    auto* status=f.view.findChild<QLabel*>("chartDrawingStatus");
    QVERIFY(status);
    QVERIFY(status->isHidden());
    QVERIFY(f.autoBtn()->isEnabled());
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QCoreApplication::processEvents();
    // Must stay visible across a normal refresh pass, not get hidden or overwritten.
    f.view.setSelection("coin:upbit:KRW-SYN",60000);
    QCoreApplication::processEvents();
    QVERIFY2(status->isVisible(),"Auto drawing failure reason must be visibly presented to the user");
    QVERIFY2(status->text().contains(QStringLiteral("완료 봉이 21개 이상 필요해요")),qPrintable(status->text()));
    // Switching timeframe clears the transient failure message.
    f.view.setSelection("coin:upbit:KRW-SYN",300000);
    QCoreApplication::processEvents();
    QVERIFY(status->isHidden());

    // applyBatch failure also surfaces visibly instead of failing silently.
    AutoDrawingFixture f2;
    auto* status2=f2.view.findChild<QLabel*>("chartDrawingStatus");
    QVERIFY(status2 && status2->isHidden());
    QTest::mouseClick(f2.autoBtn(),Qt::LeftButton);
    QTest::keyClick(f2.chart(),Qt::Key_Escape);
    QCoreApplication::processEvents();
    f2.drawings.setLocked(true);
    QCoreApplication::processEvents();
    QTest::mouseClick(f2.applyBtn(),Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY2(status2->isVisible(),"applyBatch failure must surface visibly");
    QVERIFY(!status2->text().isEmpty());
}
void MarketCandlesViewTest::lowTrendlineSweepMarkerDrawsBelowLowEvenWithLongerUpperWick(){
    KeefiTheme::instance()->setMode(ThemeMode::Light);
    const qint64 end=StudyNow/60000*60000;
    struct Spec{double o,h,l,c;};
    QList<Spec> specs(40,{110,111.0,109.0,110.0});
    specs[0] ={110,111.0,109.0,110.0};
    specs[1] ={110,110.5,105.0,106.0};
    specs[2] ={106,107.0,100.0,102.0}; // Low pivot 1 @ bar 2 = 100
    specs[3] ={102,110.0,102.0,109.0};
    specs[4] ={109,118.0,109.0,116.0};
    specs[5] ={116,116.5,109.0,110.0};
    specs[6] ={110,110.5,104.0,105.0}; // Low pivot 2 @ bar 6 = 104
    specs[7] ={105,118.0,105.0,117.0};
    specs[8] ={117,131.0,117.0,130.0};
    specs[9] ={130,130.5,118.0,119.0};
    specs[10]={119,120.0,108.0,110.0}; // Low pivot 3 @ bar 10 = 108
    specs[11]={110,117.0,110.0,116.0};
    specs[12]={116,124.0,116.0,123.0};
    for(int i=13;i<=17;++i)specs[i]={122,124.0,121.0,122.0};
    // Bar 18: line(18)=116, low=112 (4 below), high=122.5 (6.5 above — longer upper wick!), close=118 -> lower sweep!
    specs[18]={122,122.5,112.0,118.0};
    specs[19]={118,125.0,118.0,124.0};
    for(int i=20;i<40;++i)specs[i]={150,151.0,149.0,150.0};
    QList<MarketCandle> rows;
    for(int i=0;i<specs.size();++i){
        const qint64 at=end-(specs.size()-i)*60000;
        rows.append({at,at+1000,QString::number(specs[i].o,'f',2),QString::number(specs[i].h,'f',2),QString::number(specs[i].l,'f',2),QString::number(specs[i].c,'f',2),"5"});
    }
    AutoDrawingFixture f(rows);
    auto* chart=f.chart();QVERIFY(chart);
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QTest::keyClick(chart,Qt::Key_Escape);
    QCoreApplication::processEvents();
    auto* trendCb=f.view.findChild<QCheckBox*>("chartDrawingPreviewCheck_trend");
    QVERIFY(trendCb);
    for(auto* cb:f.previewChecks())if(cb!=trendCb)cb->setChecked(false);
    QCoreApplication::processEvents();
    const double plotTop=chart->property("viewportPlotTop").toDouble();
    const double plotW=chart->property("viewportPlotWidth").toDouble();
    const double plotH=chart->property("viewportPlotHeight").toDouble();
    const double pLow=chart->property("viewportPriceLow").toDouble();
    const double pHigh=chart->property("viewportPriceHigh").toDouble();
    const double firstBarX=chart->property("viewportFirstBarX").toDouble();
    const double spanSlots=chart->property("viewportSpanSlots").toDouble();
    QVERIFY(plotW>100&&plotH>100&&pHigh>pLow&&spanSlots>0);
    const double sx=firstBarX+18.0/spanSlots*plotW;
    const double lowY=plotTop+(pHigh-112.0)/(pHigh-pLow)*plotH;
    const double highY=plotTop+(pHigh-122.5)/(pHigh-pLow)*plotH;
    const QImage img=chart->grab().toImage();
    const double dpr=img.devicePixelRatio();
    const QColor accent=themeColors().accent;
    const auto accentInBox=[&](double x0,double x1,double y0,double y1){
        int count=0;
        const int ix0=qMax(0,qFloor(x0*dpr)),ix1=qMin(img.width()-1,qCeil(x1*dpr));
        const int iy0=qMax(0,qFloor(y0*dpr)),iy1=qMin(img.height()-1,qCeil(y1*dpr));
        for(int y=iy0;y<=iy1;++y)for(int x=ix0;x<=ix1;++x){
            const QColor c=img.pixelColor(x,y);
            if(qAbs(c.red()-accent.red())<=12&&qAbs(c.green()-accent.green())<=12&&qAbs(c.blue()-accent.blue())<=12)++count;
        }
        return count;
    };
    const int belowLow=accentInBox(sx-5,sx+5,lowY+1,lowY+12);
    const int aboveHigh=accentInBox(sx-5,sx+5,highY-12,highY-1);
    QVERIFY2(belowLow>=4,qPrintable(QStringLiteral("Expected lower-wick sweep marker below lowY=%1, found %2 px").arg(lowY).arg(belowLow)));
    QCOMPARE(aboveHigh,0);
}
void MarketCandlesViewTest::similarityComparisonTablePaintsDirectlyHighlightsNavigatesAndCollapses(){
    KeefiTheme::instance()->setMode(ThemeMode::Light);
    StudyTransport transport;
    MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return StudyNow;});
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    for(const auto* id:{"pullback","cloud","volume","whales"})preferences.setEnabled(id,false);
    preferences.setEnabled("similarity",true);preferences.setVisible("similarity",true);
    MarketCandlesView view(&source);
    view.setStyleSheet(KeefiTheme::localStyleSheet(&view));
    view.setIndicatorPreferences(&preferences);
    view.setSelection("coin:upbit:KRW-SYN",60000);
    view.resize(1280,900);view.show();
    auto history=syntheticStudyRows(200);
    for(int i=0;i<history.size();++i){
        const double scale=std::pow(1.025,i/20);
        auto& row=history[i];
        for(auto* value:{&row.open,&row.high,&row.low,&row.close})
            *value=QString::number(value->toDouble()*scale,'f',7);
    }
    emit transport.finished(transport.token,syntheticStudyResponse(history));
    QTRY_VERIFY(view.chartStudies().similarity.available&&view.chartStudies().similarity.paths.size()==3);
    auto* chart=view.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);
    // Ensure direct painting (no visible child widget or table widget created inside CandleChart).
    for(auto* child:chart->findChildren<QWidget*>())QVERIFY2(child->isHidden(),qPrintable(child->metaObject()->className()));
    const QImage baseImg=chart->grab().toImage();
    QCoreApplication::processEvents();
    QVERIFY(chart->property("similarityTableVisible").toBool());
    QVERIFY(chart->property("similarityTableExpanded").toBool());
    QCOMPARE(chart->property("similarityTitle").toString(),QStringLiteral("닮은꼴 · 과거 비교"));
    QCOMPARE(chart->property("similarityDisclaimer").toString(),QStringLiteral("과거 경로 · 예측 아님"));
    const QStringList tableRows=chart->property("similarityTableRows").toStringList();
    const auto& paths=view.chartStudies().similarity.paths;
    QCOMPARE(tableRows.size(),paths.size());
    const QTimeZone kst("Asia/Seoul");
    for(int i=0;i<paths.size();++i){
        const auto parts=tableRows[i].split('|');
        QCOMPARE(parts.size(),4);
        QCOMPARE(parts[0],QString::number(i+1));
        QCOMPARE(parts[1],QDateTime::fromMSecsSinceEpoch(paths[i].toMs,kst).toString(QStringLiteral("yy.MM.dd HH:mm")));
        QCOMPARE(parts[2],QString::number(paths[i].correlation,'f',2));
        QVERIFY(!parts[2].contains('%'));
        // Every group of 20 bars contains the same path at 1.025 times the prior price.
        QCOMPARE(parts[3],QStringLiteral("+2.5%"));
        QVERIFY(!tableRows[i].contains("Echo")&&!tableRows[i].contains(QChar(0x2460)));
    }
    const QVariantList rowRects=chart->property("similarityRowRects").toList();
    QCOMPARE(rowRects.size(),paths.size());
    // Hovering a table row must highlight the path, never emit leaveEvent, and never steal focus.
    class LeaveSpy final:public QObject{
      public:int leaves=0;
        bool eventFilter(QObject*,QEvent* ev)override{if(ev->type()==QEvent::Leave)++leaves;return false;}
    } leaveSpy;
    chart->installEventFilter(&leaveSpy);
    auto* focusKeeper=view.findChild<QToolButton*>("chartDrawingTool_crosshair");QVERIFY(focusKeeper);
    focusKeeper->setFocus();QCoreApplication::processEvents();
    QCOMPARE(QApplication::focusWidget(),focusKeeper);
    const QPoint row1Center=rowRects[1].toRectF().center().toPoint();
    QMouseEvent hoverEv(QEvent::MouseMove,row1Center,chart->mapToGlobal(row1Center),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(chart,&hoverEv);QCoreApplication::processEvents();
    const QImage hoverImg=chart->grab().toImage();
    QCOMPARE(chart->property("similarityHoverRow").toInt(),1);
    QCOMPARE(chart->cursor().shape(),Qt::PointingHandCursor);
    QCOMPARE(leaveSpy.leaves,0);
    QCOMPARE(QApplication::focusWidget(),focusKeeper);
    const auto table=chart->property("similarityTableRect").toRectF().adjusted(-6,-6,6,6);
    const auto plot=static_cast<market_candles_detail::CandleChart*>(chart)->studyViewport();
    const auto latest=plot.axis.coordinateAt(paths[1].points.first().atMs);
    QVERIFY(latest.has_value());
    const double projectionLeft=plot.priceArea.left()+(*latest-plot.slotsRange.first)/
        (plot.slotsRange.second-plot.slotsRange.first)*plot.priceArea.width();
    const auto ink=themeColors().studyBlue;
    const auto matches=[&](QColor pixel){
        return std::abs(pixel.red()-ink.red())<24&&std::abs(pixel.green()-ink.green())<24&&std::abs(pixel.blue()-ink.blue())<24;
    };
    const double dpr=hoverImg.devicePixelRatio();
    int newPathPixels=0;
    for(int y=qCeil(plot.priceArea.top()*dpr);y<qFloor(plot.priceArea.bottom()*dpr);++y){
        for(int x=qCeil(projectionLeft*dpr);x<qFloor(plot.priceArea.right()*dpr);++x){
            if(table.contains(QPointF(x/dpr,y/dpr)))continue;
            if(matches(hoverImg.pixelColor(x,y))&&!matches(baseImg.pixelColor(x,y)))++newPathPixels;
        }
    }
    QVERIFY2(newPathPixels>8,"Expected a highlighted trajectory in the projection outside the table");

    const auto renderDir=qEnvironmentVariable("KEEFI_STEP2_COMPLETE_RENDER_DIR");
    if(!renderDir.isEmpty()){
        QVERIFY(QDir().mkpath(renderDir));
        QVERIFY(view.grab().save(QDir(renderDir).filePath("step2-4-similarity-table-hover-light.png")));
    }

    // Clicking row 1 navigates to path[1].fromMs -> leaves right edge -> table auto-hides.
    QTest::mouseClick(chart,Qt::LeftButton,Qt::NoModifier,row1Center);
    QCoreApplication::processEvents();
    chart->grab();
    QVERIFY(view.visibleViewport().fromMs<=paths[1].fromMs&&view.visibleViewport().toMs>=paths[1].fromMs);
    QVERIFY(!chart->property("similarityTableVisible").toBool());

    // Pressing End returns to live right edge and restores the table.
    QTest::keyClick(chart,Qt::Key_End);
    QCoreApplication::processEvents();
    chart->grab();
    QVERIFY(chart->property("similarityTableVisible").toBool());

    // Narrow chart width collapses to 1-line summary and expands on hover.
    view.resize(420,700);QCoreApplication::processEvents();
    chart->grab();
    QVERIFY(chart->property("similarityTableVisible").toBool());
    QVERIFY(!chart->property("similarityTableExpanded").toBool());
    QCOMPARE(chart->property("similarityTableSummary").toString(),QStringLiteral("닮은꼴 3 · 이후 20봉 평균 +2.5%"));
    const QPoint pillCenter=chart->property("similarityTableRect").toRectF().center().toPoint();
    QMouseEvent expandEv(QEvent::MouseMove,pillCenter,chart->mapToGlobal(pillCenter),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(chart,&expandEv);QCoreApplication::processEvents();
    chart->grab();
    QVERIFY(chart->property("similarityTableExpanded").toBool());
}
void MarketCandlesViewTest::controlModifierStrongMagnetSnapsWithin24PxWithoutFocusSteal(){
    KeefiTheme::instance()->setMode(ThemeMode::Light);
    const qint64 end=StudyNow/60000*60000;
    QList<MarketCandle> rows;
    for(int i=0;i<6;++i){
        const qint64 at=end-(6-i)*60000;
        if(i==4)rows.append({at,at+1000,"50","90","10","50","5"}); // sets wide vertical range ~10..90
        else rows.append({at,at+1000,"50","52","48","50","5"});    // bar 2 has clean high=52 with plenty of vertical clearance above
    }
    AutoDrawingFixture f(rows);
    auto* chart=f.chart();QVERIFY(chart);
    QVERIFY(!f.drawings.magnetEnabled());
    auto* magnetBtn=f.view.findChild<QToolButton*>("chartDrawingMagnet");QVERIFY(magnetBtn);
    const QString nativeCtrl=QKeySequence(Qt::CTRL).toString(QKeySequence::NativeText);
    QVERIFY2(magnetBtn->toolTip().contains(nativeCtrl),qPrintable(magnetBtn->toolTip()));

    auto* focusKeeper=f.autoBtn();QVERIFY(focusKeeper);
    focusKeeper->setFocus();QCoreApplication::processEvents();
    QCOMPARE(QApplication::focusWidget(),focusKeeper);

    const double plotTop=chart->property("viewportPlotTop").toDouble();
    const double plotW=chart->property("viewportPlotWidth").toDouble();
    const double plotH=chart->property("viewportPlotHeight").toDouble();
    const double pLow=chart->property("viewportPriceLow").toDouble();
    const double pHigh=chart->property("viewportPriceHigh").toDouble();
    const double firstBarX=chart->property("viewportFirstBarX").toDouble();
    const double spanSlots=chart->property("viewportSpanSlots").toDouble();
    const int cx=qRound(firstBarX+2.0/spanSlots*plotW);
    const int highY=qRound(plotTop+(pHigh-52.0)/(pHigh-pLow)*plotH);
    const QPoint within24(cx,highY-16);  // 16px away: > default 8px, <= strong 24px
    const QPoint outside24(cx,highY-32); // 32px away: > strong 24px
    QVERIFY(outside24.y()>qCeil(plotTop)+10);

    // 1) Qt::MetaModifier must NOT trigger strong magnet.
    QMouseEvent metaMove(QEvent::MouseMove,within24,chart->mapToGlobal(within24),Qt::NoButton,Qt::NoButton,Qt::MetaModifier);
    QCoreApplication::sendEvent(chart,&metaMove);QCoreApplication::processEvents();
    QVERIFY(!chart->property("magnetStrongActive").toBool());
    QVERIFY(!chart->property("magnetSnapActive").toBool());

    // 2) Baseline image with NoModifier at within24 (no snap ring).
    QMouseEvent freeMove(QEvent::MouseMove,within24,chart->mapToGlobal(within24),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(chart,&freeMove);QCoreApplication::processEvents();
    const QImage beforeImg=chart->grab().toImage();
    QVERIFY(!chart->property("magnetSnapActive").toBool());

    // 3) Qt::ControlModifier within 24px snaps to high=52, paints snap ring, steals no focus, shows no bottom caption.
    QMouseEvent ctrlMove(QEvent::MouseMove,within24,chart->mapToGlobal(within24),Qt::NoButton,Qt::NoButton,Qt::ControlModifier);
    QCoreApplication::sendEvent(chart,&ctrlMove);QCoreApplication::processEvents();
    const QImage snappedImg=chart->grab().toImage();
    QVERIFY(chart->property("magnetStrongActive").toBool());
    QVERIFY(chart->property("magnetSnapActive").toBool());
    QCOMPARE(chart->property("magnetSnapField").toString(),QStringLiteral("high"));
    QCOMPARE(chart->property("magnetSnapPrice").toString(),QStringLiteral("52"));
    QCOMPARE(QApplication::focusWidget(),focusKeeper);
    QVERIFY(f.view.findChild<QLabel*>("chartDrawingStatus")->isHidden());
    QVERIFY2(snappedImg!=beforeImg,"Strong magnet must paint OHLC snap ring");

    const auto renderDir=qEnvironmentVariable("KEEFI_STEP2_COMPLETE_RENDER_DIR");
    if(!renderDir.isEmpty()){
        QVERIFY(QDir().mkpath(renderDir));
        QVERIFY(f.view.grab().save(QDir(renderDir).filePath("step2-5-magnet-snap-ring-light.png")));
    }

    // 4) Outside 24px with ControlModifier stays free (does not snap to "52").
    QMouseEvent ctrlFar(QEvent::MouseMove,outside24,chart->mapToGlobal(outside24),Qt::NoButton,Qt::NoButton,Qt::ControlModifier);
    QCoreApplication::sendEvent(chart,&ctrlFar);QCoreApplication::processEvents();
    QVERIFY(chart->property("magnetStrongActive").toBool());
    QVERIFY(!chart->property("magnetSnapActive").toBool());

    // 5) Placing Horizontal line with ControlModifier within 24px saves exact OHLC "52".
    QTest::mouseClick(f.view.findChild<QToolButton*>("chartDrawingTool_horizontal"),Qt::LeftButton);
    QTest::mouseClick(chart,Qt::LeftButton,Qt::ControlModifier,within24);
    QCoreApplication::processEvents();
    QCOMPARE(f.drawings.drawings().size(),1);
    QCOMPARE(f.drawings.drawings().first().anchors.first().price,QStringLiteral("52"));

    // Recompute coordinates after horizontal line selection opens drawingLinePanel_.
    const double plotTop2=chart->property("viewportPlotTop").toDouble();
    const double plotH2=chart->property("viewportPlotHeight").toDouble();
    const double pLow2=chart->property("viewportPriceLow").toDouble();
    const double pHigh2=chart->property("viewportPriceHigh").toDouble();
    const int highY2=qRound(plotTop2+(pHigh2-52.0)/(pHigh2-pLow2)*plotH2);
    const QPoint outside24After(cx,highY2-32);
    QTest::mouseClick(f.view.findChild<QToolButton*>("chartDrawingTool_horizontal"),Qt::LeftButton);
    QTest::mouseClick(chart,Qt::LeftButton,Qt::ControlModifier,outside24After);
    QCoreApplication::processEvents();
    QCOMPARE(f.drawings.drawings().size(),2);
    QVERIFY(f.drawings.drawings().last().anchors.first().price!=QStringLiteral("52"));
}
namespace {
struct DrawingCanvas {
    ChartDrawings drawings;
    market_candles_detail::CandleChart chart{nullptr};
    ChartDrawingFrame frame;
    DrawingCanvas(){
        chart.resize(1440,900);
        chart.show();
    }
    void setRows(const QList<MarketCandle>& rows,const QString& symbol){
        const ChartDrawingContext context{{"coin","upbit",symbol},"KRW",true};
        QVERIFY(drawings.setContext(context,60000));
        frame={context.identity,context.currency,60000,rows.last().startsAtMs+60000,drawings.contextGeneration(),{}};
        for(const auto& row:rows){
            const bool adjacent=!frame.bars.isEmpty()&&row.startsAtMs==frame.bars.last().completesAtMs;
            frame.bars.append({row,row.startsAtMs+60000,true,adjacent});
        }
        QVERIFY(ChartDrawings::validFrame(frame));
        chart.setContext(60000,"KRW",symbol);
        chart.setObservation(rows,{});
        chart.setDrawingFrame(&drawings,frame);
        QCoreApplication::processEvents();
        chart.grab();
    }
    QPointF pixel(qint64 at,double price)const{
        const auto viewport=chart.studyViewport();
        const auto slot=viewport.axis.coordinateAt(at).value();
        const auto range=viewport.slotsRange;
        const auto low=chart.property("viewportPriceLow").toDouble();
        const auto high=chart.property("viewportPriceHigh").toDouble();
        return {viewport.priceArea.left()+(slot-range.first)/(range.second-range.first)*viewport.priceArea.width(),
                viewport.priceArea.top()+(high-price)/(high-low)*viewport.priceArea.height()};
    }
    void move(const QPointF& position,Qt::KeyboardModifiers modifiers){
        QMouseEvent event(QEvent::MouseMove,position,chart.mapToGlobal(position),Qt::NoButton,Qt::NoButton,modifiers);
        QCoreApplication::sendEvent(&chart,&event);
        chart.grab();
    }
};
}

void MarketCandlesViewTest::strongMagnetUsesCurrentFrameAndCircularPixelRadius(){
    DrawingCanvas canvas;
    QList<MarketCandle> rows;
    const qint64 start=StudyNow/60000*60000-60*60000;
    for(int i=0;i<60;++i){
        const qint64 at=start+i*60000;
        rows.append({at,at+1000,"50",i==55?"90":"52",i==55?"10":"48","50","5"});
    }
    canvas.setRows(rows,"KRW-SYN");
    auto& chart=canvas.chart;
    auto high=canvas.pixel(rows[50].startsAtMs,52);
    const auto pointer=high+QPointF(0,-16);
    canvas.move(pointer,Qt::ControlModifier);
    QVERIFY(chart.property("magnetSnapActive").toBool());
    QVERIFY(QLineF(chart.property("magnetSnapPos").toPointF(),high).length()<.01);
    auto corrected=rows;
    corrected[50].high="70";
    // Adjacent bars must not provide a second candidate within the radius.
    corrected[49].high=corrected[51].high="70";
    canvas.setRows(corrected,"KRW-SYNB");
    QVERIFY(!chart.property("magnetSnapActive").toBool());
    canvas.setRows(rows,"KRW-SYN");
    canvas.move(pointer,Qt::ControlModifier);
    QVERIFY(chart.property("magnetSnapActive").toBool());
    chart.zoom(-40);
    chart.grab();
    if(chart.property("magnetSnapActive").toBool()){
        const auto current=chart.property("magnetSnapPos").toPointF();
        QVERIFY(QLineF(pointer,current).length()<=24.000001);
        QVERIFY(QLineF(high,current).length()>1);
    }
    high=canvas.pixel(rows[50].startsAtMs,52);
    for(const QPointF offset:{QPointF(24,0),QPointF(0,-24),QPointF(14.4,-19.2)}){
        canvas.move(high+offset,Qt::ControlModifier);
        QVERIFY(chart.property("magnetSnapActive").toBool());
        QVERIFY(QLineF(chart.property("magnetSnapPos").toPointF(),high).length()<.01);
    }
    for(const QPointF offset:{QPointF(30,0),QPointF(18,-18),QPointF(0,-24.1)}){
        canvas.move(high+offset,Qt::ControlModifier);
        QVERIFY(!chart.property("magnetSnapActive").toBool());
    }
}

void MarketCandlesViewTest::gappedAutoTrendUsesTheSameObservedPathForPreviewApplyAndHit(){
    DrawingCanvas canvas;
    QList<MarketCandle> rows;
    const qint64 start=StudyNow/60000*60000-40*60000;
    for(int i=0;i<18;++i){
        const auto at=start+(i+(i>=8?12:0))*60000;
        rows.append({at,at+1000,"104","112","96","104","5"});
    }
    canvas.setRows(rows,"KRW-SYN");
    auto& chart=canvas.chart;
    AutoChartCandidate candidate;
    candidate.kind=ChartDrawingKind::Trend;
    candidate.key="trend";
    candidate.anchors={{rows[2].startsAtMs,"100"},{rows[10].startsAtMs,"108"}};
    const auto expected=canvas.pixel(rows[6].startsAtMs,104);
    const auto chord=canvas.pixel(rows[6].startsAtMs,101.6);
    const auto nearBlue=[&](const QPointF& point){
        const auto image=chart.grab().toImage();
        const auto dpr=image.devicePixelRatio();
        const auto ink=themeColors().studyBlue;
        for(int y=qFloor((point.y()-2)*dpr);y<=qCeil((point.y()+2)*dpr);++y){
            for(int x=qFloor((point.x()-2)*dpr);x<=qCeil((point.x()+2)*dpr);++x){
                if(!image.rect().contains(x,y))continue;
                const auto pixel=image.pixelColor(x,y);
                if(std::abs(pixel.red()-ink.red())<20&&std::abs(pixel.green()-ink.green())<20&&std::abs(pixel.blue()-ink.blue())<20)return true;
            }
        }
        return false;
    };
    QVERIFY(!nearBlue(expected));
    chart.setAutoPreview({{candidate,true}},canvas.frame);
    chart.finishAutoPreview();
    QVERIFY(nearBlue(expected));
    QVERIFY(!nearBlue(chord));
    chart.clearAutoPreview();
    ChartDrawing drawing;drawing.kind=candidate.kind;drawing.anchors=candidate.anchors;
    QVERIFY(canvas.drawings.applyBatch({drawing},canvas.frame));
    chart.setDrawingFrame(&canvas.drawings,canvas.frame);
    QVERIFY(nearBlue(expected));
    QVERIFY(!nearBlue(chord));
    canvas.move(expected,Qt::NoModifier);
    QCOMPARE(chart.cursor().shape(),Qt::OpenHandCursor);
    canvas.move(chord,Qt::NoModifier);
    QVERIFY(chart.cursor().shape()!=Qt::OpenHandCursor);
}
void MarketCandlesViewTest::autoPreviewHoverAndRenderProbeWhenAsked(){
    const auto renderDir=qEnvironmentVariable("KEEFI_STEP23_RENDER_DIR");
    const bool bench=qEnvironmentVariableIsSet("KEEFI_AUTO_HOVER_BENCH");
    AutoDrawingFixture f;auto* chart=f.chart();QVERIFY(chart);
    f.view.resize(1440,900);QCoreApplication::processEvents();
    QTest::mouseClick(f.autoBtn(),Qt::LeftButton);
    QTest::keyClick(chart,Qt::Key_Escape);QCoreApplication::processEvents();
    const int iterations=bench?200:20;
    QList<qint64> micros;QElapsedTimer timer;
    for(int i=0;i<iterations;++i){const QPoint at(100+(i*13)%(chart->width()-240),40+(i*9)%(chart->height()-120));
        timer.start();QMouseEvent move(QEvent::MouseMove,at,chart->mapToGlobal(at),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QCoreApplication::sendEvent(chart,&move);QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();micros.append(timer.nsecsElapsed()/1000);}
    std::sort(micros.begin(),micros.end());
    QVERIFY(!micros.isEmpty());
    if(bench){
        qInfo().noquote()<<QString("AUTO_PREVIEW_HOVER 1440x900 chart %1x%2 p50 %3us p95 %4us max %5us").arg(chart->width()).arg(chart->height()).arg(micros[iterations/2]).arg(micros[int(iterations*.95)]).arg(micros.last());
    }
    if(!renderDir.isEmpty()){
        QVERIFY(QDir().mkpath(renderDir));
        for(const bool dark:{false,true}){
            f.drawings.clear();
            KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
            f.view.setStyleSheet(KeefiTheme::localStyleSheet(&f.view));
            QTest::mouseClick(f.autoBtn(),Qt::LeftButton);QTest::keyClick(chart,Qt::Key_Escape);
            QCoreApplication::processEvents();
            QVERIFY(f.view.grab().save(QDir(renderDir).filePath(QString("step2-23-gemini-preview-%1-1440.png").arg(dark?"dark":"light"))));
            QTest::mouseClick(f.applyBtn(),Qt::LeftButton);QCoreApplication::processEvents();
            QVERIFY(f.view.grab().save(QDir(renderDir).filePath(QString("step2-23-gemini-applied-%1-1440.png").arg(dark?"dark":"light"))));
        }
    }
}
void MarketCandlesViewTest::fibLabelsUsePriceAxisAndKeepPriceGrid_data(){
    QTest::addColumn<bool>("dark");
    QTest::newRow("light")<<false;
    QTest::newRow("dark")<<true;
}

void MarketCandlesViewTest::fibLabelsUsePriceAxisAndKeepPriceGrid(){
    QFETCH(bool,dark);
    const auto previousTheme=KeefiTheme::instance()->mode();
    const auto restoreTheme=qScopeGuard([&]{KeefiTheme::instance()->setMode(previousTheme);});
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    AutoDrawingFixture f;
    f.view.setStyleSheet(KeefiTheme::localStyleSheet(&f.view));
    f.view.setFixedSize(1440,900);
    auto* chart=f.chart();QVERIFY(chart);
    QEvent leave(QEvent::Leave);QApplication::sendEvent(chart,&leave);
    QCoreApplication::processEvents();

    const auto right=[&]{
        return chart->property("viewportPlotLeft").toDouble()+chart->property("viewportPlotWidth").toDouble();
    };
    const auto axisTexts=[](const CandleTextDevice& device,qreal plotRight){
        QMap<QString,qreal> result;
        for(const auto& run:device.engine.runs)if(run.position.x()>=plotRight)result.insert(run.text,run.position.y());
        return result;
    };
    const auto priceGrid=[](const CandleTextDevice& device){
        QList<qreal> result;
        for(const auto& stroke:device.engine.strokes){
            if(stroke.ink==themeColors().grid&&stroke.line.dx()>200&&qFuzzyIsNull(stroke.line.dy()))
                result.append(stroke.line.y1());
        }
        return result;
    };
    CandleTextDevice before;chart->render(&before);
    const auto beforeAxis=axisTexts(before,right());
    const auto beforeGrid=priceGrid(before);
    QVERIFY(beforeAxis.contains("1,175"));
    QVERIFY(beforeAxis.contains("1,215"));
    QVERIFY(!beforeGrid.isEmpty());

    const auto rows=f.source.series().candles;
    QVERIFY(rows.size()>20);
    ChartDrawingFrame frame;
    frame.identity=f.drawings.context().identity;frame.currency="KRW";frame.intervalMs=60000;
    frame.asOfMs=StudyNow;frame.contextGeneration=f.drawings.contextGeneration();
    for(const auto& row:rows)frame.bars.append({row,row.startsAtMs+60000,true,!frame.bars.isEmpty()});
    const auto fib=f.drawings.create(ChartDrawingKind::FibRetracement,
        {{rows[4].startsAtMs,"1040"},{rows[20].startsAtMs,"1255"}},frame);
    QVERIFY(!fib.isEmpty());
    QCoreApplication::processEvents();

    CandleTextDevice after;chart->render(&after);
    const auto afterAxis=axisTexts(after,right());
    const QStringList labels{"0 1,255","0.236 1,204","0.382 1,173","0.5 1,148","0.618 1,122","0.786 1,086","1 1,040"};
    for(const auto& label:labels)QVERIFY2(afterAxis.contains(label),qPrintable(label));
    QVERIFY(!afterAxis.contains("1,175"));
    QVERIFY(afterAxis.contains("1,100"));
    QVERIFY(afterAxis.contains("1,215"));
    QCOMPARE(priceGrid(after),beforeGrid);
    for(auto it=beforeAxis.cbegin();it!=beforeAxis.cend();++it){
        if(afterAxis.contains(it.key()))continue;
        const auto overlaps=std::any_of(labels.cbegin(),labels.cend(),[&](const QString& label){
            return std::abs(afterAxis.value(label)-it.value())<16.0;
        });
        QVERIFY2(overlaps,qPrintable(it.key()));
    }
    const auto captureDir=qEnvironmentVariable("KEEFI_FIB_AXIS_CAPTURE_DIR");
    if(!captureDir.isEmpty()){
        QVERIFY(QDir().mkpath(captureDir));
        const auto capture=f.view.grab().toImage();
        QCOMPARE(capture.size(),QSize(1440,900));
        QVERIFY(capture.save(QDir(captureDir).filePath(dark?"keefi-fib-axis-dark.png":"keefi-fib-axis-light.png")));
    }

    // A crosshair at a Fib level moves the label, and moving away clears the connector.
    const auto top=chart->property("viewportPlotTop").toDouble();
    const auto height=chart->property("viewportPlotHeight").toDouble();
    const auto high=chart->property("viewportPriceHigh").toDouble();
    const auto low=chart->property("viewportPriceLow").toDouble();
    const auto fibY=top+(high-1172.87)/(high-low)*height;
    QTest::mouseMove(chart,QPoint(chart->width()/2,qRound(fibY)));
    QCoreApplication::processEvents();
    CandleTextDevice hovered;chart->render(&hovered);
    const auto hoverAxis=axisTexts(hovered,right());
    QVERIFY(hoverAxis.contains("0.382 1,173"));
    QVERIFY(std::abs(hoverAxis.value("0.382 1,173")-afterAxis.value("0.382 1,173"))>=16);
    const auto windowChart=[&]{
        const auto at=chart->mapTo(&f.view,QPoint());
        return f.view.screen()->grabWindow(f.view.winId(),at.x(),at.y(),chart->width(),chart->height()).toImage();
    };
    chart->repaint();
    const auto collision=windowChart();
    QTest::mouseMove(chart,QPoint(chart->width()/2,qRound(top+35)));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr,QEvent::UpdateRequest);
    const auto strips=windowChart();
    // Isolate damage-region correctness. Both screen captures must use the
    // same backing-store rasterizer, including the Windows font engine.
    chart->repaint(chart->rect());
    const auto full=windowChart();
    if(!strips.isNull()){
        QVERIFY(!collision.isNull()&&!full.isNull());
        QVERIFY(collision!=strips);
        if(strips!=full&&!captureDir.isEmpty()){
            const auto prefix=captureDir+(dark?"/fib-dark-":"/fib-light-");
            strips.save(prefix+"strips.png");
            full.save(prefix+"full.png");
        }
        QCOMPARE(strips,full);
    }
    CandleTextDevice away;chart->render(&away);
    const auto awayAxis=axisTexts(away,right());
    QVERIFY(awayAxis.contains("0.382 1,173"));
    QCOMPARE(awayAxis.value("0.382 1,173"),afterAxis.value("0.382 1,173"));

    // A Fibonacci endpoint at the current price must leave both values visible.
    QApplication::sendEvent(chart,&leave);
    QVERIFY(f.drawings.remove(fib));
    QVERIFY(!f.drawings.create(ChartDrawingKind::FibRetracement,
        {{rows[4].startsAtMs,"1040"},{rows[20].startsAtMs,"1215"}},frame).isEmpty());
    CandleTextDevice latestCollision;chart->render(&latestCollision);
    const auto latestAxis=axisTexts(latestCollision,right());
    QVERIFY(latestAxis.contains("1,215"));
    QVERIFY(latestAxis.contains("0 1,215"));
    QVERIFY(std::abs(latestAxis.value("1,215")-latestAxis.value("0 1,215"))>=16);
}
QTEST_MAIN(MarketCandlesViewTest)
#include "tst_market_candles_view.moc"
