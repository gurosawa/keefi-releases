#include "ChartInstrumentPanelTestSupport.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartMarketLedger.h"
#include "keefi/InfoButton.h"
#include "keefi/CoinFlowStudyRunner.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/PortfolioController.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/TradingTransport.h"
#include "fixtures/UpbitResponses.h"
#include "fixtures/BinanceUsdmResponses.h"
#include <QApplication>
#include <QComboBox>
#include <QColorDialog>
#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <sys/resource.h>
#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTimeZone>
#include "keefi/KeefiVisuals.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEngine>
#include <QPainter>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QTest>
#include <cmath>

using namespace keefi;
namespace {
constexpr qint64 Now = 1780000020000;
class PublicTransport final : public TradingTransport {
  public:
    quint64 token = 0;
    void send(quint64 id, const RequestSpec& request) override {
        QVERIFY(!request.authenticated); QVERIFY(!isMutation(request.operation)); token = id;
    }
    void abort() override {}
};
HttpResult observedRows(int count = 40) {
    QJsonArray rows;
    const auto end = Now / 60000 * 60000;
    for (int i = 0; i < count; ++i) {
        const auto at = end - (count - i) * 60000;
        const auto price = 100 + i;
        rows.append(QJsonObject{{"market", "KRW-SYN"},
            {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp", QString::number(at + 1000)}, {"unit", "1"},
            {"opening_price", QString::number(price - 1)}, {"high_price", QString::number(price + 2)},
            {"low_price", QString::number(price - 2)}, {"trade_price", QString::number(price)},
            {"candle_acc_trade_volume", QString::number(2 + i)}});
    }
    HttpResult response; response.status = 200; response.body = QJsonDocument(rows).toJson(); return response;
}
// Identical public fixture to CoinFlowStudyRunnerTest::maximumFixtureBenchmark.
// No account source or external network is used by the UI benchmark.
CoinFlowSnapshot maximumUiFixture(){
    constexpr qint64 start=1792713600000,minute=60000;constexpr int bars=200,trades=10000;
    const qint64 now=start+bars*minute+1000;CoinFlowSnapshot input;input.identity="coin:binance:SYNUSDT";
    auto& item=input.primary.instrument;item.identity=input.identity;item.symbol="SYNUSDT";item.baseAsset="SYN";item.quoteAsset="USDT";
    item.venue=CoinFlowVenue::BinanceSpot;item.grain=CoinFlowGrain::SpotAggregate;item.verified=true;item.status="TRADING";
    item.evidence={"synthetic","https://api.binance.com/api/v3/exchangeInfo?permissions=SPOT&showPermissionSets=false",0,now-1,now-1};input.primary.evidence=item.evidence;
    for(int i=0;i<bars;++i){CoinFlowClosedMinute row;const auto at=start+i*minute;row.candle={at,0,"100","101","99",i%2?"100.1":"100","10"};row.completesAtMs=at+minute;
        row.knownAtMs=row.decisionAtMs=at+minute+1000;row.connectsPrevious=i>0;row.liveEligible=i==bars-1;row.evidence={"synthetic","https://example.invalid/fixture",0,row.knownAtMs,row.knownAtMs};input.minutes.append(row);
        input.primary.periods.append({start,at+minute,at+minute+1,1,CoinFlowCoverage::ObservedContinuous,{}});}
    for(int i=0;i<trades;++i){CoinTradeObservation row;row.id=QString::number(i+1);row.price="100";row.quantity=QString::number(1+i%23);row.side=i%3?CoinFlowSide::Buy:CoinFlowSide::Sell;
        row.tradeAtMs=start+(qint64(i)*bars*minute/trades);row.eventAtMs=row.tradeAtMs+1;row.receivedAtMs=row.knownAtMs=row.tradeAtMs+2;row.epoch=1;row.liveEligible=true;input.primary.trades.append(row);}
    return input;
}
class ContextTransport final:public TradingTransport {
public:
    qint64* now=nullptr;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(!isMutation(request.operation));const auto path=request.url.path();const auto end=*now/3600000*3600000;QByteArray body="[]";
        if(path.endsWith("exchangeInfo"))body=R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})";
        else if(path.endsWith("premiumIndex"))body=QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0001"},{"interestRate","0.0001"},{"time",double(*now)},{"nextFundingTime",double(*now+3600000)}}).toJson();
        else if(path.endsWith("openInterestHist"))body=QJsonDocument(QJsonArray{QJsonObject{{"symbol","BTCUSDT"},{"timestamp",double(end-3600000)},{"sumOpenInterest","100"}},QJsonObject{{"symbol","BTCUSDT"},{"timestamp",double(end)},{"sumOpenInterest","110"}}}).toJson();
        else if(path.endsWith("topLongShortPositionRatio"))body=QJsonDocument(QJsonArray{QJsonObject{{"symbol","BTCUSDT"},{"timestamp",double(end-3600000)},{"longShortRatio","1.2"},{"longAccount","0.5454545"},{"shortAccount","0.4545455"}}}).toJson();
        else if(path.endsWith("takerlongshortRatio"))body=QJsonDocument(QJsonArray{QJsonObject{{"timestamp",double(end-3600000)},{"buyVol","60"},{"sellVol","40"},{"buySellRatio","1.5"}}}).toJson();
        else if(path=="/fapi/v1/klines"){QJsonArray a;for(int i=2;i>=1;--i)a.append(QJsonArray{double(end-i*3600000),"100","120","90",i==2?"100":"110","10",double(end-(i-1)*3600000-1),"1000",10,"5","500","0"});body=QJsonDocument(a).toJson();}
        HttpResult response;response.status=200;response.body=body;QTimer::singleShot(0,this,[this,token,response]{emit finished(token,response);});
    }
};
// Observe the fonts that reach actual text painting, including native rich
// text glyph runs. A QLabel base font alone cannot establish mixed typography.
class ChartTextPaintEngine final : public QPaintEngine {
  public:
    struct Run {QString text;QFont font;qreal right;};QList<Run> runs;
    bool begin(QPaintDevice*)override{return true;}bool end()override{return true;}
    Type type()const override{return User;}
    void updateState(const QPaintEngineState&)override{}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&)override{}
    void drawPath(const QPainterPath&)override{}
    void drawPolygon(const QPointF*,int,PolygonDrawMode)override{}
    void drawTextItem(const QPointF& position,const QTextItem& item)override{runs.append({item.text(),item.font(),position.x()+item.width()});}
};
class ChartTextPaintDevice final : public QPaintDevice {
  public:
    mutable ChartTextPaintEngine engine;
    QPaintEngine* paintEngine()const override{return &engine;}
    int metric(PaintDeviceMetric value)const override{
        switch(value){case PdmWidth:return 1600;case PdmHeight:return 1600;case PdmDepth:return 32;case PdmNumColors:return 0;
        case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;}
    }
};
QList<ChartTextPaintEngine::Run> paintedLabel(QLabel& label){
    const auto originalSize=label.size();label.resize(800,qMax(160,label.heightForWidth(800)));
    ChartTextPaintDevice device;label.render(&device);label.resize(originalSize);return device.engine.runs;
}
bool roleMatches(const QFont& actual,FontRole role){
    const auto expected=KeefiFonts::font(role);
    return actual.family()==expected.family()&&actual.pixelSize()==expected.pixelSize()&&actual.weight()==expected.weight();
}
bool paintsWith(const QList<ChartTextPaintEngine::Run>& runs,const QRegularExpression& text,FontRole role){
    for(const auto& run:runs)if(text.match(run.text).hasMatch()&&roleMatches(run.font,role))return true;
    return false;
}
QString plainLabelText(const QLabel& label){
    if(label.textFormat()==Qt::RichText||(label.textFormat()==Qt::AutoText&&Qt::mightBeRichText(label.text()))){
        QTextDocument document;document.setHtml(label.text());return document.toPlainText();
    }
    return label.text();
}
}
class ChartDesignTest final : public QObject {
    Q_OBJECT
  private slots:
    void sentimentNeedsMeasuredCellsAndVisibilityControlsItsActualLedger(){
        qint64 now=Now;ContextTransport transport;transport.now=&now;
        ChartPublicContextSource source(&transport,nullptr,[&]{return now;});
        ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("sentiment",true);
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);
        view.setPublicContextSource(&source);view.setSelection("coin:upbit:KRW-BTC",60000);
        view.resize(850,650);view.show();
        const auto state=[&]{
            for(const auto& indicator:view.indicatorStates())if(indicator.id=="sentiment")return indicator;
            return ChartIndicatorState{};
        };
        QTRY_VERIFY(source.snapshot().futuresSupported);
        QVERIFY(!state().requiredReady);QCOMPARE(state().dataDetail,IndicatorDataDetail::Normal);
        auto* ledger=view.findChild<QWidget*>("chartFuturesLedger");QVERIFY(ledger);QVERIFY(ledger->isVisible());
        now+=1000;source.pump();QTRY_VERIFY(source.snapshot().funding.available);
        QVERIFY(!state().requiredReady);QCOMPARE(state().dataDetail,IndicatorDataDetail::Partial);
        for(int i=0;i<7;++i){now+=1000;source.pump();QCoreApplication::processEvents();}
        QTRY_VERIFY(source.snapshot().takerBuy.available);QVERIFY(state().requiredReady);
        preferences.setVisible("sentiment",false);
        QVERIFY(!ledger->isVisible());QVERIFY(preferences.enabled("sentiment"));QVERIFY(state().requiredReady);
        preferences.setVisible("sentiment",true);QVERIFY(ledger->isVisible());
        view.setPublicContextSource(nullptr);
        QVERIFY(!ledger->isVisible());QVERIFY(!state().requiredReady);
    }
    void mixedChartTitleElidesToItsPaintedWidth_data() {
        QTest::addColumn<QString>("fullText");
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("expectsElision");
        QTest::newRow("mixed-overflow")
            << QStringLiteral("비트코인 IIIIIIIII/11111111 · 업비트 · KRW · 5분 캔들") << 120 << true;
        QTest::newRow("mixed-full-fits")
            << QStringLiteral("WWWWWWWWWWWWWWWWWWWW · 1일 캔들") << 210 << false;
        QTest::newRow("grapheme-prefix")
            << QStringLiteral("한국 e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301 · IIIIII1111") << 78 << true;
    }
    void mixedChartTitleElidesToItsPaintedWidth() {
        QFETCH(QString, fullText);
        QFETCH(int, width);
        QFETCH(bool, expectsElision);
        LedgerTextLabel title(fullText, FontRole::BodyStrong15);
        title.setObjectName("marketCandlesTitle");
        title.setProperty("elideRight", true);
        title.setAccessibleName(fullText);
        title.setToolTip(fullText);
        const auto paintAt = [&](int width) {
            title.setFixedSize(width, 28);
            ChartTextPaintDevice device;
            title.render(&device);
            return device.engine.runs;
        };
        const auto runs = paintAt(width);
        QString painted;
        qreal paintedRight = 0;
        for (const auto& run : runs) {
            painted += run.text;
            paintedRight = qMax(paintedRight, run.right);
        }
        if (expectsElision) {
            QVERIFY(painted.endsWith(QStringLiteral("…")));
            const auto prefix = painted.chopped(1);
            QVERIFY(fullText.startsWith(prefix));
            QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, fullText);
            boundaries.setPosition(prefix.size());
            QVERIFY(boundaries.isAtBoundary());
        } else {
            QCOMPARE(painted, fullText);
        }
        QVERIFY2(paintedRight <= title.contentsRect().width(),
                 qPrintable(QStringLiteral("Painted title ends at %1, but only %2px fit: %3")
                                .arg(paintedRight).arg(title.contentsRect().width()).arg(painted)));
        QCOMPARE(title.text(), fullText);
        QCOMPARE(title.accessibleName(), fullText);
        QCOMPARE(title.toolTip(), fullText);
    }

    void richChartTitleUsesActualInstrumentFormat() {
        const ChartInstrumentLabel instrument{QStringLiteral("비트코인"), QStringLiteral("BTC/KRW"), QStringLiteral("업비트")};
        const auto suffix = QStringLiteral(" · KRW · 15분 캔들");
        const auto html = instrument.titleHtml(suffix);
        const auto fullText = instrument.text() + suffix;
        const auto tooltip = html + QStringLiteral("\n원천 시각");
        LedgerTextLabel title(FontRole::BodyStrong15);
        title.setObjectName("marketCandlesTitle");
        title.setProperty("elideRight", true);
        title.setTextFormat(Qt::RichText);
        title.setText(html);
        title.setAccessibleName(fullText);
        title.setToolTip(tooltip);

        for (const int width : {240, 480}) {
            title.setFixedSize(width, 28);
            ChartTextPaintDevice device;
            title.render(&device);
            QString painted;
            qreal right = 0;
            for (const auto& run : device.engine.runs) {
                painted += run.text;
                right = qMax(right, run.right);
            }
            if (width == 240) {
                QVERIFY2(painted.endsWith(QStringLiteral("…")), qPrintable(painted));
                QVERIFY(fullText.startsWith(painted.chopped(1)));
            } else {
                QCOMPARE(painted, fullText);
            }
            QVERIFY2(right <= title.contentsRect().width(), qPrintable(QString::number(right)));
            QVERIFY(paintsWith(device.engine.runs, QRegularExpression(QStringLiteral("^BTC/KRW$")), FontRole::Number11));
            QCOMPARE(title.textFormat(), Qt::RichText);
            QCOMPARE(title.text(), html);
            QCOMPARE(title.accessibleName(), fullText);
            QCOMPARE(title.toolTip(), tooltip);
        }
    }

    void workspaceRichChartTitleElidesAfterRefresh() {
        class NamesTransport final : public TradingTransport {
          public:
            void abort() override {}
            void send(quint64 token, const RequestSpec& request) override {
                QVERIFY(!request.authenticated);
                HttpResult response;
                response.status = 200;
                response.body = request.operation == Operation::Markets
                    ? QByteArray(R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin","market_event":{"warning":false,"caution":{}}}])")
                    : QByteArray(R"([{"market":"KRW-BTC","trade_price":129104000,"prev_closing_price":130000000,"signed_change_rate":-0.01,"timestamp":1790000000000}])");
                QTimer::singleShot(0, this, [this, token, response] { emit finished(token, response); });
            }
        } transport;
        PublicMarketCatalog markets(&transport);
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setPublicNameSources(&markets, nullptr, nullptr);
        workspace.resize(1440, 900);
        workspace.show();
        markets.refresh();
        QTRY_VERIFY(!markets.busy());
        QVERIFY(workspace.setIdentity(QStringLiteral("coin:upbit:KRW-BTC")));
        workspace.setTimeframe(300000);
        workspace.setTimeframe(900000);

        auto* toolbar = workspace.findChild<QWidget*>("chartToolbar");
        auto* title = toolbar ? toolbar->findChild<QLabel*>("marketCandlesTitle") : nullptr;
        QVERIFY(title && title->isVisible());
        const auto instrument = ChartInstrumentLabel{QStringLiteral("비트코인"), QStringLiteral("BTC/KRW"), QStringLiteral("업비트")};
        const auto suffix = QStringLiteral(" · KRW · 15분 캔들");
        const auto fullText = instrument.text() + suffix;
        const auto html = instrument.titleHtml(suffix);
        QTRY_COMPARE(title->accessibleName(), fullText);
        QCOMPARE(title->textFormat(), Qt::RichText);
        QCOMPARE(title->text(), html);
        QVERIFY(title->toolTip().startsWith(html));
        const auto tooltip = title->toolTip();

        ChartTextPaintDevice device;
        title->render(&device);
        QString painted;
        qreal right = 0;
        for (const auto& run : device.engine.runs) {
            painted += run.text;
            right = qMax(right, run.right);
        }
        QVERIFY2(painted.endsWith(QStringLiteral("…")), qPrintable(painted));
        QVERIFY(fullText.startsWith(painted.chopped(1)));
        QVERIFY2(right <= title->contentsRect().width(), qPrintable(QString::number(right)));
        QCOMPARE(title->text(), html);
        QCOMPARE(title->accessibleName(), fullText);
        QCOMPARE(title->toolTip(), tooltip);
    }

    void idleToolbarDoesNotScheduleMoreLayouts(){
        struct LayoutRequests final:QObject{
            int count=0;
            bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::LayoutRequest)++count;return false;}
        } requests;
        ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1440,900);workspace.show();
        auto* toolbar=workspace.findChild<QWidget*>("chartToolbar");QVERIFY(toolbar);
        toolbar->installEventFilter(&requests);
        for(auto* child:toolbar->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))child->installEventFilter(&requests);
        QTest::qWait(40);requests.count=0;
        for(int i=0;i<10;++i)QCoreApplication::processEvents();
        QVERIFY2(requests.count<=1,qPrintable(QStringLiteral("An idle toolbar scheduled %1 new layouts").arg(requests.count)));
    }
    void compactToolbarRetainsControlsWithinTwoRows(){
        ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        workspace.show();
        for(const int width:{1800,1440,1280,1100,1024,900,720}){
            workspace.resize(width,900);QTest::qWait(20);
            auto* toolbar=workspace.findChild<QWidget*>("chartToolbar");QVERIFY(toolbar);
            QVERIFY(toolbar->height()<=96);
            for(const char* name:{"marketCandlesTitle","marketCandlesInformation","chartCurrentPrice","chartWorkspaceTimeframeButtons","chartStudyPicker","chartStudyAppliedToggle","chartStyleButton","chartLayout_1","chartLayout_2","chartSyncMenuButton","chartMoreTools"}){
                auto* control=workspace.findChild<QWidget*>(name);QVERIFY(control);QVERIFY(toolbar->isAncestorOf(control));
                if(control->isVisible())QVERIFY2(toolbar->rect().contains(QRect(control->mapTo(toolbar,QPoint()),control->size())),name);
            }
            if(toolbar->property("timeframeFolded").toBool())QVERIFY(toolbar->property("styleFolded").toBool());
            if(toolbar->property("styleFolded").toBool())QVERIFY(toolbar->property("gridFolded").toBool());
        }
    }

    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); }
    void dashboardChartKeepsLockedPositionBesideThePublicChart_data() {
        QTest::addColumn<QSize>("viewport");
        QTest::addColumn<bool>("showIntro");
        QTest::newRow("1440") << QSize(1440, 790) << false;
        QTest::newRow("1920") << QSize(1920, 970) << false;
        QTest::newRow("1440-first-visit") << QSize(1440, 790) << true;
    }
    void dashboardChartKeepsLockedPositionBesideThePublicChart() {
        QFETCH(QSize, viewport);QFETCH(bool,showIntro);
        PublicTransport transport;
        MarketCandles source(&transport, std::unique_ptr<PublicCandleSocket>{}, [] { return Now; });
        ChartAlertEngine alertsEngine(nullptr);
        ChartWorkspace workspace(&source, nullptr);
        // Alert controls now belong to settings and do not consume chart height.
        QWidget settingsOwner;
        auto* alerts = new ChartAlertsPanel(&alertsEngine, nullptr, nullptr, nullptr, &settingsOwner);
        alerts->hide();
        workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
        workspace.indicatorPreferences()->markIntroSeen();
        auto* lists = workspace.watchlistModel();
        const auto list = lists->createList(QStringLiteral("공개 관심"));
        QVERIFY(lists->addIdentity(list, {"coin", "upbit", "KRW-SYN"}));
        workspace.setIdentity("coin:upbit:KRW-SYN"); workspace.setTimeframe(60000);
        workspace.resize(viewport); workspace.show(); QTRY_VERIFY(transport.token);
        emit transport.finished(transport.token, observedRows(120));
        if(showIntro){workspace.findChild<QWidget*>("chartStudyIntro")->show();
            emit alertsEngine.noticeSubmitted("synthetic-first-visit","합성 공개 알림","합성 기록으로 화면 높이를 확인해요.",false);}
        QTest::qWait(50);
        const auto original=source.series();
        auto* instrumentPanel = workspace.findChild<QWidget*>("chartInstrumentPanel");
        auto* instrumentButton = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
        auto* chart = workspace.findChild<QWidget*>("chartWorkspaceChartPane");
        auto* details = workspace.findChild<QWidget*>("chartWorkspacePositions");
        auto* disclosure = workspace.findChild<QPushButton*>("chartDetailsToggle");
        auto* lock = workspace.findChild<QLabel*>("chartWorkspacePositionLocked");
        auto* scroll = workspace.findChild<QScrollArea*>("chartWorkspaceScroll");
        QVERIFY(instrumentPanel&&instrumentButton); QVERIFY(chart); QVERIFY(details); QVERIFY(disclosure); QVERIFY(scroll);
        QVERIFY(!instrumentPanel->isVisible());QVERIFY(instrumentButton->isVisible());QVERIFY(disclosure->isVisible());QVERIFY(!disclosure->isChecked());
        QVERIFY2(!details->isVisible(), "Position and indicator details start collapsed");
        QVERIFY(lock); QVERIFY(!lock->isVisible());
        QVERIFY(lock->text().contains(QStringLiteral("금고")));
        auto* toolbar = workspace.findChild<QWidget*>("chartToolbar");
        QVERIFY(toolbar&&toolbar->isAncestorOf(instrumentButton));
        auto* chartView = chart->findChild<MarketCandlesView*>();
        QVERIFY(chartView);
        QVERIFY(instrumentButton->mapToGlobal(QPoint(0,instrumentButton->height())).y()<=chartView->mapToGlobal(QPoint()).y());
        QVERIFY(!workspace.findChild<ChartAlertsPanel*>());QVERIFY(!alerts->isVisible());
        const auto captureDirectory=qEnvironmentVariable("KEEFI_REDESIGN_CHART_RENDER_DIR");
        if(showIntro&&!captureDirectory.isEmpty()){
            QVERIFY(QDir().mkpath(captureDirectory));
            QVERIFY(workspace.grab().save(captureDirectory+"/chart-first-visit-1440x790.png"));
        }
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QCOMPARE(workspace.findChild<QLabel*>("chartCurrentPrice")->font(), KeefiFonts::font(FontRole::Value22));
        QVERIFY(workspace.findChild<QLabel*>("chartWorkspacePositionText")->text().isEmpty());
        scroll->ensureWidgetVisible(disclosure,0,0);QCoreApplication::processEvents();
        QVERIFY(scroll->viewport()->rect().contains(QRect(disclosure->mapTo(scroll->viewport(),QPoint()),disclosure->size())));
        QTest::mouseClick(disclosure,Qt::LeftButton);QTRY_VERIFY(details->isVisible());QVERIFY(lock->isVisible());QCoreApplication::processEvents();
        QVERIFY(details->geometry().top()>chart->geometry().bottom());QCOMPARE(details->geometry().left(),chart->geometry().left());
        // Under load the opened details may finish a last relayout after the first scroll
        // (seen once in three runs on 2026-11-05). What matters is that each control is
        // reachable once the page settles, so scroll and check until it does.
        const auto reachable=[&](QWidget* widget){scroll->ensureWidgetVisible(widget,0,0);QCoreApplication::processEvents();
            return scroll->viewport()->rect().contains(QRect(widget->mapTo(scroll->viewport(),QPoint()),widget->size()));};
        QTRY_VERIFY(reachable(lock));
        auto* unlock=workspace.findChild<QPushButton*>("chartWorkspaceUnlock");QVERIFY(unlock);QVERIFY(unlock->isVisible());
        QTRY_VERIFY(reachable(unlock));
        QVERIFY(workspace.findChild<QLabel*>("chartWorkspacePositionText")->text().isEmpty());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        QCOMPARE(source.series().sourceUrl,original.sourceUrl);QCOMPARE(source.series().candles.size(),original.candles.size());QVERIFY(source.series().available);
    }
    void currentSelectionNameKeepsItsBodyFace(){
        class NamesTransport final:public TradingTransport {public:
            void abort()override{}
            void send(quint64 token,const RequestSpec& request)override{QVERIFY(!request.authenticated);HttpResult response;response.status=200;
                response.body=request.operation==Operation::Markets?QByteArray(R"([{"market":"KRW-SYN","korean_name":"UPPERCASE NAME","english_name":"Synthetic","market_event":{"warning":false,"caution":{}}}])"):
                    QByteArray(R"([{"market":"KRW-SYN","trade_price":100,"prev_closing_price":99,"signed_change_rate":0.01,"timestamp":1790000000000}])");
                QTimer::singleShot(0,this,[this,token,response]{emit finished(token,response);});}
        } transport;
        PublicMarketCatalog markets(&transport);ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        workspace.setPublicNameSources(&markets,nullptr,nullptr);workspace.setIdentity("coin:upbit:KRW-SYN");workspace.resize(1280,1200);workspace.show();
        markets.refresh();QTRY_VERIFY(!markets.busy());
        auto* detail=workspace.findChild<QLabel*>("chartCoinFlowDetails");QVERIFY(detail);
        // The calculation text stays available internally, while P1 exposes details through ⓘ.
        QVERIFY(detail->isHidden());
        QVERIFY(plainLabelText(*detail).contains(QStringLiteral("UPPERCASE NAME SYN/KRW · 업비트")));
        const auto runs=paintedLabel(*detail);
        QVERIFY2(paintsWith(runs,QRegularExpression("UPPERCASE"),FontRole::Caption12),"The display name in the current-selection body must retain Pretendard");
        QVERIFY(paintsWith(runs,QRegularExpression("SYN/KRW"),FontRole::Number11));
    }
    void instrumentNamesKeepTheirBodyFaceAndLeaveShareIdentityUntouched(){
        class NamesTransport final:public TradingTransport {public:
            int sent=0;void abort()override{}
            void send(quint64 token,const RequestSpec& request)override{++sent;QVERIFY(!request.authenticated);HttpResult response;response.status=200;
                response.body=request.url.host()=="api.nasdaq.com"?QByteArray(R"({"data":{"rows":[{"symbol":"NVDA","name":"NVIDIA Corporation Common Stock","lastsale":"$101","pctchange":"1%","marketCap":"100000","sector":"Technology"}]}})"):
                    request.url.host()=="m.stock.naver.com"?QByteArray(R"({"marketStatus":"CLOSE","stocks":[{"itemCode":"000660","stockName":"SK하이닉스","closePrice":"100","fluctuationsRatio":"1","compareToPreviousPrice":{"code":"2"},"marketValue":"1000"}]})"):QByteArray("[]");
                QTimer::singleShot(0,this,[this,token,response]{emit finished(token,response);});}
        } transport;
        PublicEquityBoard board(&transport);ChartInstrumentPresentation presentation;presentation.setSources(nullptr,&board,nullptr);
        MarketCandlesView view(nullptr);view.setInstrumentPresentation(&presentation);view.resize(1000,800);view.show();
        view.setSelection("stock:US:NVDA",86400000);const auto before=view.shareSnapshot({},Now);
        board.refresh();QTRY_VERIFY(!board.busy());
        for(const auto& pair:QList<QPair<QString,QString>>{{"stock:US:NVDA","NVIDIA Corporation"},{"stock:KR:000660",QStringLiteral("SK하이닉스")}}){
            view.setSelection(pair.first,86400000);auto* title=view.findChild<QLabel*>("marketCandlesTitle");QVERIFY(title);
            QVERIFY(plainLabelText(*title).startsWith(pair.second));
            const auto runs=paintedLabel(*title);bool namePainted=false,codePainted=false;
            const auto code=pair.first.section(':',2);
            for(const auto& run:runs){
                if(!run.text.trimmed().isEmpty()&&pair.second.contains(run.text.trimmed())){namePainted=true;QVERIFY(roleMatches(run.font,FontRole::BodyStrong15));}
                if(run.text==code){codePainted=true;QVERIFY(roleMatches(run.font,FontRole::Number11));}
            }
            QVERIFY(namePainted);QVERIFY(codePainted);QVERIFY(title->accessibleName().startsWith(pair.second));
        }
        view.setSelection("stock:US:NVDA",86400000);const auto after=view.shareSnapshot({},Now);
        QCOMPARE(after.identity,before.identity);QCOMPARE(after.availability,before.availability);QCOMPARE(after.currency,before.currency);
        const auto count=transport.sent;emit board.changed();QCoreApplication::processEvents();QCOMPARE(transport.sent,count);
    }
    void readySourceFactsMoveToInformationWithoutFooter_data(){
        QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
    }
    void readySourceFactsMoveToInformationWithoutFooter(){
        QFETCH(bool,dark);auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto restore=qScopeGuard([=]{theme->setMode(oldMode);});
        theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.indicatorPreferences()->markIntroSeen();
        workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.resize(1280,1450);workspace.show();QTRY_VERIFY(transport.token);
        emit transport.finished(transport.token,observedRows(120));QVERIFY(source.series().available);QTest::qWait(30);
        auto* label=workspace.findChild<QLabel*>("marketCandlesStatus");QVERIFY(label);const auto original=plainLabelText(*label);
        QVERIFY(original.isEmpty());QVERIFY(label->isHidden());
        const auto before=source.series();QVERIFY(before.sourceTimeMs>0);QVERIFY(before.receivedAtMs>=before.sourceTimeMs);
        auto* information=workspace.findChild<InfoButton*>("marketCandlesInformation");QVERIFY(information);information->openInformation();
        auto* body=workspace.findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(body);
        QVERIFY(body->toPlainText().contains(QDateTime::fromMSecsSinceEpoch(before.sourceTimeMs,QTimeZone("Asia/Seoul")).toString("HH:mm")));
        QVERIFY(!body->toPlainText().contains("https://"));QVERIFY(!body->toPlainText().contains("coin:"));
        QCOMPARE(source.series().sourceTimeMs,before.sourceTimeMs);QCOMPARE(source.series().receivedAtMs,before.receivedAtMs);
        QCOMPARE(source.series().sourceUrl,before.sourceUrl);
    }
    void unavailableCandleStatesHaveDistinctPublicPresentation_data(){
        QTest::addColumn<QString>("sourceState");
        QTest::addColumn<QString>("message");
        QTest::newRow("loading")<<QString("loading")<<QStringLiteral("첫 캔들 대기");
        QTest::newRow("failure")<<QString("failure")<<QStringLiteral("받지 못했어요");
        QTest::newRow("delayed")<<QString("delayed")<<QStringLiteral("지연 · 마지막 확인 구간");
    }
    void unavailableCandleStatesHaveDistinctPublicPresentation(){
        QFETCH(QString,sourceState);QFETCH(QString,message);
        qint64 now=Now;PublicTransport transport;
        MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
        ChartWorkspace workspace(&source,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);
        workspace.resize(1440,790);workspace.show();QTRY_VERIFY(transport.token);
        auto* status=workspace.findChild<QLabel*>("marketCandlesStatus");
        auto* price=workspace.findChild<QLabel*>("chartCurrentPrice");
        auto* reference=workspace.findChild<QLabel*>("chartCurrentPriceReference");
        auto* chart=workspace.findChild<QWidget*>("marketCandlesChart");
        MarketCandleSeries lastGood;
        QVERIFY(status&&price&&reference&&chart);
        if(sourceState=="failure"){
            HttpResult response;response.status=503;response.body="{}";
            emit transport.finished(transport.token,response);
        }else if(sourceState=="delayed"){
            emit transport.finished(transport.token,observedRows());QTRY_VERIFY(source.series().available);
            QTRY_COMPARE(plainLabelText(*price),QString("139"));
            QTRY_VERIFY(workspace.chartStudies().available);lastGood=source.series();
            now+=MarketCandles::FreshnessMs+1;emit source.changed();
        }
        // Last-good same-selection candles may remain visible, but they never
        // become current calculation inputs, even after a late worker result.
        QVERIFY(!source.series().available);QVERIFY(source.series().candles.isEmpty());
        QVERIFY(!source.series().forming.has_value());
        QTRY_VERIFY(!workspace.chartStudies().available);
        QVERIFY(status->isVisible());QCOMPARE(plainLabelText(*status),message);
        if(sourceState=="delayed"){
            const auto retained=source.displayObservation();QVERIFY(!retained.available);QVERIFY(retained.delayed);
            QVERIFY(!retained.forming);QCOMPARE(retained.identity,lastGood.identity);QCOMPARE(retained.intervalMs,lastGood.intervalMs);
            QCOMPARE(retained.receivedAtMs,lastGood.receivedAtMs);QCOMPARE(retained.sourceTimeMs,lastGood.sourceTimeMs);
            QCOMPARE(retained.candles.size(),lastGood.candles.size());
            for(int i=0;i<lastGood.candles.size();++i){const auto& before=lastGood.candles[i];const auto& after=retained.candles[i];
                QCOMPARE(after.startsAtMs,before.startsAtMs);QCOMPARE(after.lastStoredTickMs,before.lastStoredTickMs);
                QCOMPARE(after.open,before.open);QCOMPARE(after.high,before.high);QCOMPARE(after.low,before.low);QCOMPARE(after.close,before.close);QCOMPARE(after.volume,before.volume);}
            QTRY_VERIFY(chart->isVisible());
            QVERIFY(source.series().delayed);QCOMPARE(source.series().staleClose,QString("139"));
            QCOMPARE(plainLabelText(*price),QString("139"));
            QVERIFY(reference->isVisible());QCOMPARE(plainLabelText(*reference),QStringLiteral("지연"));
            QCOMPARE(reference->font(),KeefiFonts::font(FontRole::Caption12));
            workspace.setIdentity("coin:upbit:KRW-OTHER");
            QCOMPARE(plainLabelText(*price),QStringLiteral("—"));
            QVERIFY(!reference->isVisible());
            QTRY_COMPARE(plainLabelText(*status),QStringLiteral("첫 캔들 대기"));
            QTRY_VERIFY(!chart->isVisible());QVERIFY(source.displayObservation().candles.isEmpty());
        }else{
            QTRY_VERIFY(!chart->isVisible());
            QVERIFY(!source.series().delayed);QVERIFY(source.series().staleClose.isEmpty());
            QCOMPARE(plainLabelText(*price),QStringLiteral("—"));
            QVERIFY(!reference->isVisible());
        }
    }
    void futureCandleReceiptNeverBecomesADelayedQuote(){
        qint64 now=Now;PublicTransport transport;
        MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
        ChartWorkspace workspace(&source,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);
        workspace.resize(1440,790);workspace.show();QTRY_VERIFY(transport.token);
        emit transport.finished(transport.token,observedRows());QTRY_VERIFY(source.series().available);
        now=Now-1;emit source.changed();const auto invalid=source.series();
        QVERIFY(!invalid.available);QVERIFY(!invalid.delayed);QVERIFY(invalid.staleClose.isEmpty());
        QVERIFY(invalid.candles.isEmpty());
        QCOMPARE(plainLabelText(*workspace.findChild<QLabel*>("chartCurrentPrice")),QStringLiteral("—"));
        QVERIFY(workspace.findChild<QLabel*>("chartCurrentPriceReference")->isHidden());
    }
    void indicatorOverflowUsesEnglishCloseAndVerticalScrolling(){
        ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        workspace.setIdentity("coin:upbit:KRW-SYN");workspace.resize(1280,790);workspace.show();
        for(const auto& id:workspace.indicatorPreferences()->supportedIds("coin"))workspace.indicatorPreferences()->setEnabled(id,true);
        workspace.findChild<QPushButton*>("chartStudyAppliedToggle")->setChecked(true);
        auto* more=workspace.findChild<QPushButton*>("chartStudyMore");QVERIFY(more);QTRY_VERIFY(more->isVisible());bool inspected=false;
        QTimer::singleShot(0,&workspace,[&]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;
            const auto closeDialog=qScopeGuard([&]{dialog->reject();});
            auto* scroll=dialog->findChild<QScrollArea*>();auto* buttons=dialog->findChild<QDialogButtonBox*>();QVERIFY(scroll&&buttons);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);QVERIFY(scroll->verticalScrollBar()->maximum()>0);
            auto* close=buttons->button(QDialogButtonBox::Close);QVERIFY(close);QCOMPARE(close->text(),QStringLiteral("Close"));
            for(const auto* button:dialog->findChildren<QPushButton*>())QVERIFY(button->width()>=44&&button->height()>=44);
            inspected=true;
        });
        more->click();QVERIFY(inspected);
    }
    void priceAxisIncludesFullStudyBounds_data(){
        QTest::addColumn<QSize>("viewport");
        QTest::newRow("1280")<<QSize(1280,800);QTest::newRow("1440")<<QSize(1440,900);QTest::newRow("1920")<<QSize(1920,1080);
    }
    void priceAxisIncludesFullStudyBounds(){
        QFETCH(QSize,viewport);PublicTransport transport;
        MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        // Historical comparison is optional; this case explicitly tests the
        // complete bounds of its enabled, actually calculated path layer.
        QVERIFY(workspace.indicatorPreferences()->setEnabled("similarity",true));
        workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.resize(viewport);workspace.show();QTRY_VERIFY(transport.token);
        auto response=observedRows(120);auto rows=QJsonDocument::fromJson(response.body).array();
        for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();const auto base=100000000+i*15000;
            row["opening_price"]=QString::number(base-10000);row["trade_price"]=QString::number(base);
            row["high_price"]=QString::number(base+20000);row["low_price"]=QString::number(base-20000);rows[i]=row;}
        response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
        QTRY_VERIFY(workspace.chartStudies().similarity.available);
        const auto studies=workspace.chartStudies();double high=0;
        for(const auto& point:studies.similarity.maximum)high=qMax(high,point.price);
        QVERIFY(high>source.series().candles.last().high.toDouble());
        auto* chart=workspace.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);ChartTextPaintDevice device;chart->render(&device);
        // The axis now shows round-number ticks instead of the raw extremes. The plotted range must
        // still contain the complete calculated study path, and every axis number stays whole.
        int axisNumbers=0;
        for(const auto& run:device.engine.runs){
            QVERIFY2(!run.text.contains(QChar(0x2026)),qPrintable("Axis price was elided: "+run.text));
            auto numeric=run.text;numeric.remove(',');bool valid=false;numeric.toDouble(&valid);
            if(valid&&roleMatches(run.font,FontRole::Number11))++axisNumbers;
        }
        QVERIFY2(axisNumbers>=3,"Round-number price labels must be painted beside the chart");
        QVERIFY2(chart->property("viewportPriceHigh").toDouble()>=high,"The complete calculated upper bound must stay inside the plotted price range");
        auto* scroll=workspace.findChild<QScrollArea*>("chartWorkspaceScroll");QVERIFY(scroll);QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        QVERIFY(workspace.findChild<MarketCandlesView*>()->pricePlotHeight()>=200);
    }
    void metricChangeUsesNativeMixedTypographyWithoutChangingTheQuote(){
        auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto restore=qScopeGuard([=]{theme->setMode(oldMode);});
        ExploreMetricCardData data;data.id="synthetic-f06-change";data.title="합성 공개 가격";data.value="101.25";data.unit="KRW";
        data.sourceKey="coin:upbit:KRW-SYN";data.direction=MovementDirection::Up;
        data.changeText="전일 대비 +1.25% · 오늘 09:00 기준 <b>FAKE</b>";
        ExploreMetricCard card;card.setData(data);card.resize(900,160);card.show();auto* change=card.findChild<QLabel*>("metricChange");QVERIFY(change);
        const auto expected=QStringLiteral("▲ ")+data.changeText;
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
            theme->setMode(mode);card.setStyleSheet(KeefiTheme::localStyleSheet(&card));QCoreApplication::processEvents();
            QCOMPARE(plainLabelText(*change),expected);QCOMPARE(card.data(),data);QVERIFY(change->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
            change->setSelection(0,expected.size());QCOMPARE(change->selectedText(),expected);card.setData(data);QCOMPARE(change->selectedText(),expected);change->setSelection(0,0);
            const auto runs=paintedLabel(*change);
            QVERIFY2(paintsWith(runs,QRegularExpression(QStringLiteral("[가-힣]")),FontRole::Caption12),"Korean change captions must actually paint as Pretendard12 rather than the number font's 11px fallback");
            QVERIFY(paintsWith(runs,QRegularExpression(QStringLiteral("1.25")),FontRole::Number11));
            const auto output=qEnvironmentVariable("KEEFI_F06_RENDER_DIR");
            if(!output.isEmpty()){
                QVERIFY(QDir().mkpath(output));QCoreApplication::processEvents();
                QVERIFY(card.grab().save(QDir(output).filePath(QStringLiteral("metric-change-%1.png").arg(mode==ThemeMode::Light?QStringLiteral("light"):QStringLiteral("dark")))));
            }
        }
        card.clearPrivate();QCOMPARE(card.data(),data);QCOMPARE(plainLabelText(*change),expected);
    }
    void chartSummaryNumbersUseTheirPaintedRoles_data(){
        QTest::addColumn<QString>("labelName");QTest::addColumn<int>("koreanRole");
        QTest::newRow("scorecard")<<QStringLiteral("chartStudyScorecard")<<int(FontRole::Caption12);
        QTest::newRow("similarity-summary")<<QStringLiteral("chartStudySimilaritySummary")<<int(FontRole::Caption12);
        QTest::newRow("study-state")<<QStringLiteral("marketCandlesStudyState")<<int(FontRole::Body15);
        QTest::newRow("study-details")<<QStringLiteral("marketCandlesStudyDetails")<<int(FontRole::Body15);
        QTest::newRow("title")<<QStringLiteral("marketCandlesTitle")<<int(FontRole::BodyStrong15);
    }
    void chartSummaryNumbersUseTheirPaintedRoles(){
        QFETCH(QString,labelName);QFETCH(int,koreanRole);
        auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto restore=qScopeGuard([=]{theme->setMode(oldMode);});
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.resize(1440,1500);workspace.show();QTRY_VERIFY(transport.token);
        auto response=observedRows(120);auto rows=QJsonDocument::fromJson(response.body).array();
        const int pattern[]{100,101,99,102,100,104,101,103,100,105,102,106,103,107,104,108,106,109,107,110};
        for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();const auto price=pattern[i%20];
            row["opening_price"]=QString::number(price-1);row["trade_price"]=QString::number(price);
            row["high_price"]=QString::number(price+2);row["low_price"]=QString::number(price-2);rows[i]=row;}
        response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
        QVERIFY(source.series().available);QTRY_VERIFY2(workspace.chartStudies().similarity.available,"The existing repeated public candle fixture must produce a real similarity summary");
        auto* details=workspace.findChild<InfoButton*>("marketCandlesInformation");QVERIFY(details);details->openInformation();
        auto* infoBody=workspace.findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(infoBody);
        QVERIFY(infoBody->toPlainText().contains(workspace.findChild<QLabel*>("marketCandlesStudyDetails")->text()));
        auto* label=workspace.findChild<QLabel*>(labelName);QVERIFY(label);const auto original=plainLabelText(*label);
        QVERIFY(QRegularExpression(QStringLiteral("[0-9]")).match(original).hasMatch());QVERIFY(QRegularExpression(QStringLiteral("[가-힣]")).match(original).hasMatch());
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
            theme->setMode(mode);workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));QCoreApplication::processEvents();
            QCOMPARE(plainLabelText(*label),original);const auto runs=paintedLabel(*label);
            QVERIFY2(paintsWith(runs,QRegularExpression(QStringLiteral("[0-9]")),FontRole::Number11),qPrintable(labelName+QStringLiteral(" must actually paint its numbers in Departure Mono11")));
            QVERIFY2(paintsWith(runs,QRegularExpression(QStringLiteral("[가-힣]")),FontRole(koreanRole)),qPrintable(labelName+QStringLiteral(" must retain its specified Korean role")));
            const auto output=qEnvironmentVariable("KEEFI_F06_RENDER_DIR");
            if(!output.isEmpty()){
                QVERIFY(QDir().mkpath(output));
                workspace.findChild<QScrollArea*>("chartWorkspaceScroll")->ensureWidgetVisible(label);QCoreApplication::processEvents();
                QVERIFY(workspace.grab().save(QDir(output).filePath(QStringLiteral("chart-%1-%2.png").arg(QString::fromLatin1(QTest::currentDataTag()),mode==ThemeMode::Light?QStringLiteral("light"):QStringLiteral("dark")))));
            }
        }
        workspace.clearPrivate();QCOMPARE(plainLabelText(*label),original);QVERIFY(source.series().available);
    }
    void positionTypographyKeepsNativeSelectionAndClearsOnLock(){
        class AverageTransport final:public TradingTransport {public:
            void send(quint64 token,const RequestSpec& request)override{
                QVERIFY(!isMutation(request.operation));auto response=upbit_fixture::response(request.operation);
                if(request.operation==Operation::Account){auto rows=QJsonDocument::fromJson(response.body).array();
                    for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();if(row["currency"].toString()=="BTC")row["avg_buy_price"]="120";rows[i]=row;}response.body=QJsonDocument(rows).toJson();}
                QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
            }
            void abort()override{}
        } accountTransport;
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-f06-vault"));QVERIFY(vault.create("Fake Chart Design Password! 2026"));
        auto account=upbit_fixture::account();account.id.clear();account.alias="FAKE_PRIVATE_ALIAS_F06";
        QVERIFY(vault.addAccount(account,{{"access_key","FAKE_PRIVATE_KEY_F06"},{"secret","FAKE_PRIVATE_SECRET_F06"}},StorageMode::EncryptedVault));
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &accountTransport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,8000);
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,&portfolio);workspace.setTimeframe(60000);workspace.resize(1440,1500);workspace.show();workspace.setPrivateEnabled(true);
        QTRY_VERIFY(transport.token);auto response=observedRows();auto rows=QJsonDocument::fromJson(response.body).array();
        for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["market"]="KRW-BTC";rows[i]=row;}response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
        auto* label=workspace.findChild<QLabel*>("chartWorkspacePositionText");QVERIFY(label);
        QTRY_VERIFY(plainLabelText(*label).contains(QStringLiteral("내 평균단가 120 KRW")));const auto original=plainLabelText(*label);
        const auto suppliedSnapshots=portfolio.snapshots();
        auto* candles=workspace.findChild<MarketCandlesView*>();QVERIFY(candles);QCOMPARE(candles->priceLines().size(),1);
        QCOMPARE(candles->priceLines().first().price,QStringLiteral("120"));
        const auto scope=portfolio.connections().first().accountScope.left(12);QVERIFY(!scope.isEmpty());
        auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto restore=qScopeGuard([=]{theme->setMode(oldMode);});
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
            theme->setMode(mode);workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));QCoreApplication::processEvents();
            QCOMPARE(plainLabelText(*label),original);QVERIFY(label->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));label->setSelection(0,original.size());
            QCOMPARE(QString(label->selectedText()).replace(QChar::LineSeparator,'\n').replace(QChar::ParagraphSeparator,'\n'),original);
            emit portfolio.changed();QCoreApplication::processEvents();
            QCOMPARE(QString(label->selectedText()).replace(QChar::LineSeparator,'\n').replace(QChar::ParagraphSeparator,'\n'),original);label->setSelection(0,0);
            const auto runs=paintedLabel(*label);
            QVERIFY2(paintsWith(runs,QRegularExpression(QStringLiteral("120")),FontRole::Number11),"The selected position's displayed price must paint as Mono11 through native selection layout");
            QVERIFY(paintsWith(runs,QRegularExpression(QStringLiteral("[가-힣]")),FontRole::Body15));
            const auto output=qEnvironmentVariable("KEEFI_F06_RENDER_DIR");
            if(!output.isEmpty()){
                QVERIFY(QDir().mkpath(output));QCoreApplication::processEvents();
                QVERIFY(workspace.grab().save(QDir(output).filePath(QStringLiteral("chart-position-%1.png").arg(mode==ThemeMode::Light?QStringLiteral("light"):QStringLiteral("dark")))));
            }
        }
        QCOMPARE(portfolio.snapshots(),suppliedSnapshots);QCOMPARE(candles->priceLines().first().price,QStringLiteral("120"));
        vault.lock();const auto cleared=plainLabelText(*label);QVERIFY(!cleared.contains(QStringLiteral("120 KRW")));
        QVERIFY(!cleared.contains(scope));QVERIFY(!cleared.contains(QStringLiteral("FAKE_")));
        QVERIFY(cleared.isEmpty());QVERIFY(label->isHidden());
    }
    void ledgerOmitsReferenceLabelsWithoutChangingSuppliedObservations() {
        const QList<ChartLedgerRow> rows{{"volume", QStringLiteral("거래량"), "41", {}, 7,
            {QPolygonF{{0, 2}, {1, 3}}, QPolygonF{{3, 7}, {4, 41}}}}};
        ChartMarketLedger ledger; ledger.setRows(rows); ledger.show();
        auto* value=ledger.findChild<QLabel*>("chartLedgerValue_volume"); QVERIFY(value);
        QCOMPARE(value->text(),QStringLiteral("41"));
        for(const auto* label:ledger.findChildren<QLabel*>()) {
            QVERIFY2(!QRegularExpression(QStringLiteral("\\[\\d+\\]")).match(label->text()).hasMatch(),qPrintable(label->text()));
            QVERIFY(!label->toolTip().contains("https://"));
        }
        QCOMPARE(rows.first().evidenceNumber,7);
        QCOMPARE(rows.first().segments.size(),2);
        auto* spark=ledger.findChild<QWidget*>("chartLedgerSpark_volume"); QVERIFY(spark);
        QCOMPARE(spark->property("observationCount").toInt(),4);
        QCOMPARE(spark->property("segmentCount").toInt(),2);
        QCOMPARE(value->font(),KeefiFonts::font(FontRole::Value22));
        ledger.setRows(rows);
        QCOMPARE(ledger.findChild<QLabel*>("chartLedgerValue_volume"),value);
    }
    void completedChartValuesKeepEvidenceWithoutDuplicatePriceRows() {
        PublicTransport transport;
        MarketCandles source(&transport, std::unique_ptr<PublicCandleSocket>{}, [] { return Now; });
        ChartWorkspace workspace(&source, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setIdentity("coin:upbit:KRW-SYN"); workspace.setTimeframe(60000);
        workspace.resize(1440, 1000); workspace.show(); QTRY_VERIFY(transport.token);
        emit transport.finished(transport.token, observedRows());
        QVERIFY(source.series().available); const auto before=source.series();
        auto* ledger = workspace.findChild<QWidget*>("chartMarketLedger"); QVERIFY(ledger);
        for(const auto* removed:{"close","fast","slow"})
            QVERIFY(!ledger->findChild<QLabel*>(QString("chartLedgerValue_")+removed));
        auto* volume = ledger->findChild<QLabel*>("chartLedgerValue_volume"); QVERIFY(volume);
        QCOMPARE(volume->text(), QString("41"));
        QCOMPARE(volume->font(),KeefiFonts::font(FontRole::Value22));
        QVERIFY(!ledger->findChild<QLabel*>("chartLedgerReference_volume"));
        auto* note = workspace.findChild<QWidget*>("chartEvidenceNotes"); QVERIFY(note); QVERIFY(note->isHidden());
        QCOMPARE(before.candles.last().close,QString("139"));
        QCOMPARE(before.sourceUrl,MarketCandles::request("coin:upbit:KRW-SYN",60000).url.toString());
        QVERIFY(before.receivedAtMs>0); QVERIFY(before.sourceTimeMs>0);
        QTRY_VERIFY(!workspace.chartStudies().cloud.isEmpty());
        const auto fast=workspace.chartStudies().cloud.last().fast, slow=workspace.chartStudies().cloud.last().slow;
        workspace.clearPrivate();
        QCOMPARE(volume->text(), QString("41"));
        QCOMPARE(source.series().sourceUrl,before.sourceUrl);
        QCOMPARE(source.series().sourceTimeMs,before.sourceTimeMs);
        QCOMPARE(source.series().receivedAtMs,before.receivedAtMs);
        QCOMPARE(source.series().candles.last().close,before.candles.last().close);
        QCOMPARE(workspace.chartStudies().cloud.last().fast,fast);
        QCOMPARE(workspace.chartStudies().cloud.last().slow,slow);
    }
    void missingHistoryCollapsesSparklineWithoutFabricatingAValue() {
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.show();
        auto* ledger=workspace.findChild<QWidget*>("chartMarketLedger");QVERIFY(ledger);
        auto* volume=ledger->findChild<QLabel*>("chartLedgerValue_volume");QVERIFY(volume);QCOMPARE(volume->text(),QString("—"));
        auto* spark=ledger->findChild<QWidget*>("chartLedgerSpark_volume");QVERIFY(spark);QVERIFY(spark->isHidden());
        QTRY_VERIFY(transport.token);emit transport.finished(transport.token,observedRows(1));
        QCOMPARE(volume->text(),QString("2"));QVERIFY(spark->isHidden());
        QCOMPARE(source.series().candles.last().close,QString("100"));
    }
    void candleGapsDoNotBecomeInventedSparklineConnections() {
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.show();
        QTRY_VERIFY(transport.token);auto response=observedRows(5);auto rows=QJsonDocument::fromJson(response.body).array();rows.removeAt(2);
        response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);QVERIFY(source.series().available);
        auto* spark=workspace.findChild<QWidget*>("chartLedgerSpark_volume");QVERIFY(spark);
        QCOMPARE(spark->property("observationCount").toInt(),4);QCOMPARE(spark->property("segmentCount").toInt(),2);
        QCOMPARE(spark->size(),QSize(110,24));
    }
    void hiddenEvidenceKeepsSourceFactsAndVisibleLedgerFontRoles() {
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.show();
        QTRY_VERIFY(transport.token);emit transport.finished(transport.token,observedRows());QCoreApplication::processEvents();
        const auto series=source.series();QVERIFY(series.available);
        QCOMPARE(series.candles.size(),40);QVERIFY(series.sourceTimeMs>0);QVERIFY(series.receivedAtMs>=series.sourceTimeMs);
        QCOMPARE(series.sourceUrl,MarketCandles::request(series.identity,series.intervalMs).url.toString());
        auto* note=workspace.findChild<QWidget*>("chartEvidenceNotes");QVERIFY(note);QVERIFY(note->isHidden());
        QVERIFY(!workspace.findChild<QLabel*>("chartEvidenceReference_1"));
        auto* label=workspace.findChild<QLabel*>("chartLedgerLabel_volume");QVERIFY(label);
        QCOMPARE(label->font(),KeefiFonts::font(FontRole::Body15));
        QCOMPARE(workspace.findChild<QLabel*>("chartLedgerValue_volume")->font(),KeefiFonts::font(FontRole::Value22));
        for(const auto* shown:workspace.findChild<QWidget*>("chartMarketLedger")->findChildren<QLabel*>())
            QVERIFY(!QRegularExpression(QStringLiteral("\\[\\d+\\]")).match(shown->text()).hasMatch());
    }
    void selectionAndIntervalChangesClearOldPublicEvidence() {
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,nullptr);workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);workspace.show();
        QTRY_VERIFY(transport.token);emit transport.finished(transport.token,observedRows());
        auto* price=workspace.findChild<QLabel*>("chartCurrentPrice");QVERIFY(price);QCOMPARE(price->text(),QString("139"));
        QCOMPARE(price->font(),KeefiFonts::font(FontRole::Value22));
        workspace.setTimeframe(300000);QCOMPARE(price->text(),QString("—"));
        QVERIFY(!workspace.findChild<QLabel*>("chartEvidenceSource_1"));
        workspace.setTimeframe(60000);QTest::qWait(310);emit transport.finished(transport.token,observedRows());
        QCOMPARE(price->text(),QString("139"));
        workspace.setIdentity("coin:upbit:KRW-OTHER");QCOMPARE(price->text(),QString("—"));
        QVERIFY(!workspace.findChild<QLabel*>("chartEvidenceSource_1"));
        QVERIFY(!workspace.findChild<QLabel*>("chartLedgerReference_close"));
    }
    void watchlistMovementUsesOnlySignedPercentValues() {
        ChartWorkspace workspace(nullptr,nullptr);auto* lists=workspace.watchlistModel();
        const auto list=lists->createList("합성 방향");const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};
        QVERIFY(lists->addIdentity(list,identity));workspace.show();
        QVERIFY(test::openInstrumentPanel(workspace));
        auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);
        WatchlistRowState badges;badges.completedAtMs=QDateTime::currentMSecsSinceEpoch()-1;
        badges.badgePresentations={{"첫째 합성 상태"},{"둘째 합성 상태"},{"셋째 합성 상태"}};
        lists->setRowState(identity,badges);
        for(const auto& badge:badges.badgePresentations)QVERIFY(tree->topLevelItem(0)->toolTip(0).contains(badge.text));
        for(const auto& entry:QList<QPair<QString,int>>{{"+1.25%",1},{"-2.5%",-1},{"+0.0%",0},{"1.25%",0},{"오류 -2%",0},{"-nan%",0}}){
            WatchlistRowState state;state.price="100";state.change=entry.first;lists->setRowState(identity,state);
            QCOMPARE(tree->topLevelItem(0)->data(1,ChangeDirectionRole).toInt(),entry.second);
        }
    }
    void pickerAppliesTwoPanesAndStylesTheVisibleAverage() {
        PublicTransport transport;
        MarketCandles source(&transport, std::unique_ptr<PublicCandleSocket>{}, [] {return Now;});
        ChartWorkspace workspace(&source, nullptr);
        auto* prefs = workspace.indicatorPreferences();
        prefs->markIntroSeen();
        for (const auto& id : prefs->supportedIds("coin")) prefs->setEnabled(id, false);
        auto averages = prefs->movingAverages();
        averages.first().kind = ExtendedMaKind::Simple;
        prefs->setMovingAverages(averages);
        const auto oldTheme = KeefiTheme::instance()->mode();
        const auto restoreTheme = qScopeGuard([oldTheme]{KeefiTheme::instance()->setMode(oldTheme);});
        KeefiTheme::instance()->setMode(ThemeMode::Dark);
        workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
        workspace.resize(1440, 980);
        workspace.show();
        workspace.setIdentity("coin:upbit:KRW-BTC");
        workspace.setTimeframe(60000);
        QTRY_VERIFY(transport.token);
        auto response = observedRows(120);
        auto rows = QJsonDocument::fromJson(response.body).array();
        for (int i = 0; i < rows.size(); ++i) {
            auto row = rows[i].toObject();
            const int price = 180 + i/2 + qRound(12*std::sin(i*0.21));
            const int open = price + (i%3 == 0 ? 2 : -2);
            row["market"] = "KRW-BTC";
            row["opening_price"] = QString::number(open);
            row["trade_price"] = QString::number(price);
            row["high_price"] = QString::number(qMax(price, open) + 1);
            row["low_price"] = QString::number(qMin(price, open) - 1);
            rows[i] = row;
        }
        response.body = QJsonDocument(rows).toJson();
        emit transport.finished(transport.token, response);
        QTRY_VERIFY(source.series().available);
        auto* picker = workspace.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker && picker->isVisible());
        const auto directory = qEnvironmentVariable("KEEFI_INDICATOR_CAPTURE_DIR");
        const auto capture = [&](QWidget& widget, const QString& name) {
            return directory.isEmpty() || (QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name + ".png")));
        };
        bool saved = false;
        QTimer::singleShot(0, &workspace, [&] {
            auto* dialog = workspace.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto closePicker = qScopeGuard([&]{dialog->reject();});
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            QVERIFY(search && tree);
            for (const auto& id : {QString("rsi"), QString("macd"), QString("ma")}) {
                search->setText(id == "ma" ? "Moving Averages" : id.toUpper());
                QTreeWidgetItemIterator it(tree);
                while (*it && (*it)->data(0, Qt::UserRole).toString() != id) ++it;
                QVERIFY(*it && !(*it)->isHidden());
                tree->setCurrentItem(*it);
                tree->scrollToItem(*it);
                QTest::keyClick(tree, Qt::Key_Space);
                QVERIFY(prefs->enabled(id));
                QVERIFY(prefs->visible(id));
            }
            auto* options = dialog->findChild<QPushButton*>("chartStudyMaSettings");
            QVERIFY(options && options->isVisibleTo(dialog));
            QTimer::singleShot(0, dialog, [&] {
                auto* editor = dialog->findChild<QDialog*>("chartStudyMaSettingsDialog");
                QVERIFY(editor);
                const auto closeEditor = qScopeGuard([&]{if (editor->isVisible()) editor->reject();});
                auto* kind = editor->findChild<QComboBox*>("chartMaKind_0");
                auto* length = editor->findChild<QSpinBox*>("chartMaPeriod_0");
                auto* color = editor->findChild<QPushButton*>("chartStudyColor_ma1");
                auto* save = editor->findChild<QPushButton*>("chartMaApply");
                QVERIFY(kind && length && color && save);
                kind->setCurrentText("EMA");
                length->setValue(9);
                QTimer::singleShot(0, editor, [&]{
                    auto* palette = editor->findChild<QColorDialog*>("chartStudyColorDialog");
                    QVERIFY(palette);
                    palette->setCurrentColor(QColor("#ff4dc5"));
                    palette->accept();
                });
                QTest::mouseClick(color, Qt::LeftButton);
                QVERIFY(capture(*editor, "moving-average-settings"));
                QTest::mouseClick(save, Qt::LeftButton);
                saved = true;
            });
            QTest::mouseClick(options, Qt::LeftButton);
            search->clear();
            QTRY_VERIFY(workspace.extendedStudies().rsi.state.available && workspace.extendedStudies().macd.state.available);
            auto* priceChart=workspace.findChild<QWidget*>("marketCandlesChart");
            QVERIFY(priceChart);
            QTRY_COMPARE(priceChart->property("displayMaPointCounts").toMap().value("ma1").toInt(), int(rows.size())-9+1);
            QVERIFY(capture(*dialog, "indicator-picker"));
        });
        QTest::mouseClick(picker, Qt::LeftButton);
        QVERIFY(saved);
        QCOMPARE(prefs->movingAverages().first().kind, ExtendedMaKind::Exponential);
        QCOMPARE(prefs->movingAverages().first().period, 9);
        QCOMPARE(prefs->lineColor("ma1", QColor()), QColor("#ff4dc5"));
        QTRY_VERIFY(workspace.extendedStudies().rsi.state.available && workspace.extendedStudies().macd.state.available);
        auto* rsi = workspace.findChild<QWidget*>("chartLowerPanePlot_rsi");
        auto* macd = workspace.findChild<QWidget*>("chartLowerPanePlot_macd");
        auto* canvas = workspace.findChild<QWidget*>("marketCandlesChart");
        QVERIFY(rsi && macd && canvas);
        QTRY_VERIFY(rsi->isVisibleTo(&workspace) && macd->isVisibleTo(&workspace));
        const QRect a(rsi->mapTo(&workspace, QPoint()), rsi->size());
        const QRect b(macd->mapTo(&workspace, QPoint()), macd->size());
        QVERIFY(a.height() > 20 && b.height() > 20 && !a.intersects(b));
        const auto image = canvas->grab().toImage().convertToFormat(QImage::Format_RGB32);
        int colored = 0;
        for (int y = 0; y < image.height(); ++y) {
            const auto* pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (qRed(pixels[x]) > 225 && qAbs(qGreen(pixels[x])-77) < 30 && qAbs(qBlue(pixels[x])-197) < 30) ++colored;
        }
        QVERIFY2(colored > 10, "The selected moving average color must reach the price chart.");
        QVERIFY(capture(workspace, "rsi-macd-ema-workspace"));
    }
    void optionalIndicatorWorkspaceRenders() {
        const auto destination=qEnvironmentVariable("KEEFI_C12_CHART_RENDER_DIR");
        if(destination.isEmpty())QSKIP("Set KEEFI_C12_CHART_RENDER_DIR for synthetic C12 geometry and captures");
        QVERIFY(QDir().mkpath(destination));QJsonArray receipts;
        const auto previousTheme=KeefiTheme::instance()->mode();const auto restore=qScopeGuard([&]{KeefiTheme::instance()->setMode(previousTheme);});
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto state:{QString("rsi"),QString("macd"),QString("oi"),QString("all-off"),QString("loading"),QString("failure"),QString("delayed"),QString("korean")}){
            KeefiTheme::instance()->setMode(mode);qint64 now=Now;PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
            ContextTransport contextTransport;contextTransport.now=&now;ChartPublicContextSource context(&contextTransport,nullptr,[&]{return now;});
            QWidget shell;auto* policy=new MovementColorPolicy(&shell);if(state=="korean")policy->setScheme(MovementScheme::KoreanRedBlue);
            auto* layout=new QVBoxLayout(&shell);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
            auto* note=new QLabel(tr("KEEFI · C12 Qt 검증 · 모든 숫자·캔들·체결은 합성 자료 · %1").arg(state),&shell);note->setFont(KeefiFonts::font(FontRole::Body15));note->setContentsMargins(24,0,24,0);note->setFixedHeight(110);layout->addWidget(note);
            ChartAlertEngine engine(nullptr);auto* workspace=new ChartWorkspace(&source,nullptr,&shell);layout->addWidget(workspace,1);workspace->setPublicContextSource(&context);workspace->indicatorPreferences()->markIntroSeen();
            auto* alerts=new ChartAlertsPanel(&engine,nullptr,nullptr,nullptr,workspace);qobject_cast<QVBoxLayout*>(workspace->layout())->addWidget(alerts);
            auto* lists=workspace->watchlistModel();const auto list=lists->createList("합성 공개 관심");QVERIFY(lists->addIdentity(list,{"coin","upbit","KRW-BTC"}));lists->useList(list);
            workspace->setIdentity("coin:upbit:KRW-BTC");workspace->setTimeframe(60000);auto* preferences=workspace->indicatorPreferences();
            if(state=="all-off")for(const auto& id:preferences->supportedIds("coin"))preferences->setEnabled(id,false);
            else if(state=="rsi"||state=="macd"||state=="oi"||state=="korean"){
                for(const auto* id:{"ma","bb","levels","trend","relative_strength"})preferences->setEnabled(id,true);
                preferences->setEnabled(state=="korean"?"rsi":state,true);if(state=="oi")preferences->setEnabled("sentiment",true);
            }
            shell.setStyleSheet(KeefiTheme::localStyleSheet(&shell));shell.resize(1440,900);shell.show();QTRY_VERIFY(transport.token);
            if(state!="loading"){
                auto response=observedRows(200);auto rows=QJsonDocument::fromJson(response.body).array();for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["market"]="KRW-BTC";rows[i]=row;}response.body=QJsonDocument(rows).toJson();
                if(state=="failure"){response.status=503;response.body="{}";}emit transport.finished(transport.token,response);
                if(state!="failure")QTRY_VERIFY(source.series().available);
            }
            if(state=="oi"){for(int i=0;i<9;++i){now+=1000;context.pump();QCoreApplication::processEvents();}QTRY_VERIFY(context.snapshot().oiChange.available);}
            if(state=="delayed"){now+=MarketCandles::FreshnessMs+1;emit source.changed();}
            if(state=="rsi"||state=="macd"||state=="korean")QTRY_VERIFY(workspace->extendedStudies().rsi.state.available||workspace->extendedStudies().macd.state.available);
            for(const auto viewport:{QSize(1440,900),QSize(1920,1080),QSize(1280,800),QSize(1440,790)}){
                shell.resize(viewport);QTest::qWait(60);QCOMPARE(shell.size(),viewport);
                auto* view=workspace->findChild<MarketCandlesView*>();auto* scroll=workspace->findChild<QScrollArea*>("chartWorkspaceScroll");auto* pane=view->findChild<QWidget*>("chartLowerPane");QVERIFY(view&&scroll&&pane);
                const auto name=QString("c12-%1-%2-%3x%4").arg(mode==ThemeMode::Light?"light":"dark",state).arg(viewport.width()).arg(viewport.height());
                QVERIFY(shell.grab().save(QDir(destination).filePath(name+".png")));
                QJsonArray actions;QList<QAbstractButton*> buttons=view->findChildren<QAbstractButton*>();
                for(auto* button:workspace->findChild<QWidget*>("chartWorkspaceTimeframeButtons")->findChildren<QAbstractButton*>())buttons.append(button);
                buttons.append(workspace->findChild<QAbstractButton*>("chartIndicatorScoreToggle"));
                for(auto* button:buttons)if(button&&button->isVisible()){
                    const auto bounds=QRect(button->mapTo(&shell,QPoint{}),button->size());actions.append(QJsonObject{{"name",button->objectName()},{"x",bounds.x()},{"y",bounds.y()},{"width",bounds.width()},{"height",bounds.height()}});
                    QVERIFY2(bounds.width()>=44&&bounds.height()>=44,qPrintable(name+" "+button->objectName()+" target "+QString::number(bounds.width())+"x"+QString::number(bounds.height())));
                }
                receipts.append(QJsonObject{{"capture",name},{"viewportWidth",viewport.width()},{"viewportHeight",viewport.height()},{"pricePlotHeight",source.series().available?QJsonValue(view->pricePlotHeight()):QJsonValue(QJsonValue::Null)},{"chartVisible",view->findChild<QWidget*>("marketCandlesChart")->isVisible()},
                    {"panes",QJsonArray::fromStringList(preferences->visiblePanes("coin"))},{"paneVisible",pane->isVisible()},{"scrollX",scroll->horizontalScrollBar()->maximum()},{"scrollY",scroll->verticalScrollBar()->maximum()},{"actions",actions}});
                QVERIFY(shell.grab().save(QDir(destination).filePath(name+".png")));
                if(source.series().available)QVERIFY2(view->pricePlotHeight()>=200,qPrintable(name+" price field "+QString::number(view->pricePlotHeight())));else QVERIFY(!view->findChild<QWidget*>("marketCandlesChart")->isVisible());QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
                if(viewport.height()>=900)QCOMPARE(scroll->verticalScrollBar()->maximum(),0);
                QVERIFY(workspace->findChild<QLabel*>("chartWorkspacePositionText")->text().isEmpty());QVERIFY(view->priceLines().isEmpty());
            }
            auto* instrumentPanel=test::openInstrumentPanel(*workspace);QVERIFY(instrumentPanel);
            auto* watch=instrumentPanel->findChild<QAbstractButton*>("chartBrowserScope_watch");QVERIFY(watch);QTest::mouseClick(watch,Qt::LeftButton);QCoreApplication::processEvents();
            auto* trendToggle=instrumentPanel->findChild<QAbstractButton*>("chartWatchTrendToggle");QVERIFY(trendToggle);
            QVERIFY(trendToggle->isVisible());QCOMPARE(trendToggle->height(),24);
            QVERIFY(instrumentPanel->rect().contains(QRect(trendToggle->mapTo(instrumentPanel,QPoint{}),trendToggle->size())));
            instrumentPanel->close();
            if(state=="rsi"){
                shell.resize(1440,900);workspace->findChild<QPushButton*>("chartStudyAppliedToggle")->setChecked(true);QTest::qWait(30);auto* more=workspace->findChild<QPushButton*>("chartStudyMore");QVERIFY(more&&more->isVisible());bool captured=false;
                QTimer::singleShot(0,workspace,[&]{auto* dialog=workspace->findChild<QDialog*>("chartStudyOverflowDialog");if(!dialog)dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;
                    captured=dialog->grab().save(QDir(destination).filePath(QString("c12-%1-more-dialog.png").arg(mode==ThemeMode::Light?"light":"dark")));QTest::keyClick(dialog,Qt::Key_Escape);});
                more->click();QVERIFY(captured);QTRY_VERIFY(more->hasFocus());
            }
        }
        QFile receipt(QDir(destination).filePath("geometry.json"));QVERIFY(receipt.open(QIODevice::WriteOnly));receipt.write(QJsonDocument(receipts).toJson());
    }
    void optionalMaximumUiApplyAndRenderBenchmark(){
        const auto destination=qEnvironmentVariable("KEEFI_C12_UI_BENCHMARK");if(destination.isEmpty())QSKIP("Opt-in exclusive 30-run 200-bar/10000-trade UI timing");
        const auto input=maximumUiFixture();const qint64 asOf=input.minutes.last().knownAtMs;PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[&]{return asOf;});
        MarketInstrument instrument;instrument.symbol="SYNUSDT";instrument.baseAsset="SYN";instrument.quoteAsset="USDT";instrument.market="spot";instrument.status="TRADING";instrument.tradable=true;source.setInstruments("binance",{instrument});
        ChartWorkspace workspace(&source,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.setIdentity(input.identity);workspace.setTimeframe(60000);workspace.resize(1440,790);workspace.show();QTRY_VERIFY(transport.token);
        QJsonArray candles;for(const auto& value:input.minutes){const auto& c=value.candle;candles.append(QJsonArray{double(c.startsAtMs),c.open,c.high,c.low,c.close,c.volume,double(c.startsAtMs+59999),"1000",10,"5","500","0"});}
        HttpResult response;response.status=200;response.body=QJsonDocument(candles).toJson();emit transport.finished(transport.token,response);QTRY_VERIFY(source.series().available);QCOMPARE(source.series().candles.size(),200);QTRY_VERIFY(!workspace.chartStudies().cloud.isEmpty());
        const bool includeStrong=qEnvironmentVariableIsSet("KEEFI_C12_BENCHMARK_STRONG");
        if(includeStrong){workspace.indicatorPreferences()->setEnabled("strong",true);auto* selector=workspace.findChild<QComboBox*>("chartStudyScorecardSelector");QVERIFY(selector);selector->setCurrentIndex(selector->findData("strong"));QCoreApplication::processEvents();}
        auto* view=workspace.findChild<MarketCandlesView*>();auto* runner=view->findChild<CoinFlowStudyRunner*>();QVERIFY(runner);const auto context=view->property("coinFlowContext").toString();QVERIFY(!context.isEmpty());
        QList<qint64> workers,applies,renders;int results=0;connect(runner,&CoinFlowStudyRunner::resultReady,&workspace,[&](const QString& key,const CoinFlowStudySet& result,qint64 ns){if(key!=context)return;QVERIFY(result.available);workers.append(ns);applies.append(view->property("coinFlowUiApplyNs").toLongLong());++results;});
        QElapsedTimer total;total.start();QJsonArray samples;
        for(int i=0;i<30;++i){runner->request(input,asOf,context,includeStrong?QSet<QString>{"whales","strong"}:QSet<QString>{"whales"});QTRY_COMPARE_WITH_TIMEOUT(results,i+1,3500);QVERIFY(applies.last()>0);
            QElapsedTimer render;render.start();QCoreApplication::processEvents();const auto frame=workspace.grab();QVERIFY(!frame.isNull());const auto elapsed=render.nsecsElapsed();renders.append(elapsed);
            samples.append(QJsonObject{{"workerNs",double(workers.last())},{"widgetApplyNs",double(applies.last())},{"processAndRenderNs",double(elapsed)}});}
        const auto elapsed=total.elapsed();std::sort(workers.begin(),workers.end());std::sort(applies.begin(),applies.end());std::sort(renders.begin(),renders.end());struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
        const QJsonObject receipt{{"strongEnabled",includeStrong},{"bars",200},{"trades",10000},{"runs",30},{"fixtureStartMs",1792713600000.0},{"firstTradeId","1"},{"lastTradeId","10000"},{"elapsedMs",double(elapsed)},
            {"workerMedianNs",double((workers[14]+workers[15])/2)},{"workerP95Ns",double(workers[28])},{"widgetApplyMedianNs",double((applies[14]+applies[15])/2)},{"widgetApplyP95Ns",double(applies[28])},
            {"processAndRenderMedianNs",double((renders[14]+renders[15])/2)},{"processAndRenderP95Ns",double(renders[28])},{"peakRssPlatformUnits",double(usage.ru_maxrss)},{"peakRssUnit","bytes on macOS; KiB on Linux"},
            {"note","Same synthetic fixture as runner benchmark. Apply includes real MarketCandlesView projection and synchronous Workspace slots; render measures processEvents + QWidget::grab, separately. No live account or network."},{"samples",samples}};
        QFile file(destination);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(receipt).toJson());qInfo().noquote()<<QJsonDocument(receipt).toJson(QJsonDocument::Compact);
    }
    void optionalDashboardChartRenders() {
        const auto destination=qEnvironmentVariable("KEEFI_REDESIGN_CHART_RENDER_DIR");
        if(destination.isEmpty())QSKIP("Set KEEFI_REDESIGN_CHART_RENDER_DIR for synthetic Qt workspace captures");
        QVERIFY(QDir().mkpath(destination));
        const auto previousTheme=KeefiTheme::instance()->mode();
        const auto resetTheme=qScopeGuard([&]{KeefiTheme::instance()->setMode(previousTheme);});
        // Temporary synthetic vaults and injected transports only. The wrapper
        // reserves the real shell's 110 px, but is not an application screenshot.
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto* state:{"locked","open","futures"}) {
            KeefiTheme::instance()->setMode(mode);
            class SpotTransport final:public TradingTransport {public:
                void abort()override{}
                void send(quint64 token,const RequestSpec& request)override{
                    QVERIFY(!isMutation(request.operation));auto response=upbit_fixture::response(request.operation);
                    if(request.operation==Operation::Account){auto rows=QJsonDocument::fromJson(response.body).array();
                        for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();if(row["currency"].toString()=="BTC"){
                            row["balance"]="0.125";row["avg_buy_price"]="100500000";}rows[i]=row;}
                        response.body=QJsonDocument(rows).toJson();}
                    QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
                }
            } spot;
            class FuturesTransport final:public TradingTransport {public:
                void abort()override{}
                void send(quint64 token,const RequestSpec& request)override{
                    QVERIFY(!isMutation(request.operation));auto response=keefi::test::usdmResponse(request);
                    if(request.operation==Operation::Positions){auto row=keefi::test::usdmPosition("0.020");
                        row["entryPrice"]="75000";row["markPrice"]="78431.37";row["liquidationPrice"]="72000";
                        row["unRealizedProfit"]="68.6274";row["notional"]="1568.6274";
                        response=keefi::test::usdmJson(QJsonArray{row});}
                    QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
                }
            } futures;
            QTemporaryDir directory;QVERIFY(directory.isValid());
            Vault vault(directory.filePath("synthetic-redesign-chart-vault"));QVERIFY(vault.create("Fake redesign chart password 2026!"));
            auto spotAccount=upbit_fixture::account();spotAccount.id.clear();
            QVERIFY(vault.addAccount(spotAccount,{{"access_key","FAKE_CHART_REDESIGN_KEY"},{"secret","FAKE_CHART_REDESIGN_SECRET"}},StorageMode::EncryptedVault));
            const bool withFutures=QString::fromLatin1(state)=="futures";
            if(withFutures){auto account=keefi::test::usdmAccount();account.id.clear();
                QVERIFY(vault.addAccount(account,{{"api_key","FakeChartRedesignFuturesKey"},{"secret","FakeChartRedesignFuturesSecret"}},StorageMode::EncryptedVault));}
            PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
            factories.trading=[&](const AccountRecord& account)->TradingTransport*{return account.provider=="binance"?static_cast<TradingTransport*>(&futures):static_cast<TradingTransport*>(&spot);};
            PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),withFutures?2:1,8000);
            PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
            ChartAlertEngine alertsEngine(nullptr);
            QWidget shell;auto* shellLayout=new QVBoxLayout(&shell);shellLayout->setContentsMargins(0,0,0,0);shellLayout->setSpacing(0);
            auto* note=new QLabel(QStringLiteral("KEEFI · 차트   |   Qt 컴포넌트 검증 · 모든 가격·보유·캔들은 합성 자료 · %1").arg(QString::fromLatin1(state)),&shell);
            note->setFont(KeefiFonts::font(FontRole::Body15));note->setContentsMargins(24,0,24,0);note->setFixedHeight(110);shellLayout->addWidget(note);
            auto* workspace=new ChartWorkspace(&source,&portfolio,&shell);shellLayout->addWidget(workspace,1);
            workspace->setContractTransport(&futures);workspace->indicatorPreferences()->markIntroSeen();
            auto* alerts=new ChartAlertsPanel(&alertsEngine,nullptr,nullptr,nullptr,workspace);
            qobject_cast<QVBoxLayout*>(workspace->layout())->addWidget(alerts);
            shell.setStyleSheet(KeefiTheme::localStyleSheet(&shell));
            auto* lists=workspace->watchlistModel();const auto list=lists->createList(QStringLiteral("관심 코인"));
            const QStringList codes{"BTC","ETH","SOL","LINK"};
            const QStringList prices{"101785000","4352000","219400","22510"};
            for(int i=0;i<codes.size();++i){const WatchlistIdentity identity{"coin","upbit","KRW-"+codes[i]};
                QVERIFY(lists->addIdentity(list,identity));WatchlistRowState row;row.price=prices[i];row.change=i==2?"-0.82%":"+1.24%";lists->setRowState(identity,row);}
            lists->useList(list);workspace->setIdentity("coin:upbit:KRW-BTC");workspace->setTimeframe(60000);
            workspace->setPrivateEnabled(QString::fromLatin1(state)!="locked");
            if(QString::fromLatin1(state)!="locked")lists->setHoldings({{"coin","upbit","KRW-BTC"}});
            shell.resize(1440,900);shell.show();QTRY_VERIFY(transport.token);
            auto response=observedRows(120);auto rows=QJsonDocument::fromJson(response.body).array();
            for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["market"]="KRW-BTC";
                const auto base=100000000+i*15000;
                row["opening_price"]=QString::number(base-10000);row["trade_price"]=QString::number(base);
                row["high_price"]=QString::number(base+20000);row["low_price"]=QString::number(base-20000);rows[i]=row;}
            response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
            QTRY_VERIFY(source.series().available);
            // A source response precedes the worker result. Capture a stable
            // study frame rather than racing the first light-theme screenshot.
            QTRY_VERIFY(workspace->chartStudies().available);
            if(withFutures)QTRY_VERIFY(workspace->findChild<QLabel*>("chartWorkspaceLiquidationRibbon")->isVisible());
            emit alertsEngine.noticeSubmitted("synthetic-public",QStringLiteral("BTC · 눌림존 관측"),QStringLiteral("완료 봉의 합성 관측입니다."),false,MovementDirection::Unknown,false);
            for(const auto viewport:{QSize(1440,900),QSize(1920,1080)}){
                shell.resize(viewport);QTest::qWait(70);
                const auto name=QString("/chart-%1-%2-%3x%4.png").arg(mode==ThemeMode::Light?"light":"dark",QString::fromLatin1(state)).arg(viewport.width()).arg(viewport.height());
                QVERIFY(shell.grab().save(destination+name));
                QCOMPARE(shell.size(),viewport);
                auto* scroll=workspace->findChild<QScrollArea*>("chartWorkspaceScroll");QVERIFY(scroll);
                QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);QCOMPARE(scroll->verticalScrollBar()->maximum(),0);
                if(QString::fromLatin1(state)=="locked")QVERIFY(workspace->findChild<QLabel*>("chartWorkspacePositionText")->text().isEmpty());
            }
            workspace->clearPrivate();
            QVERIFY(workspace->findChild<QLabel*>("chartWorkspacePositionText")->text().isEmpty());
            QVERIFY(workspace->findChild<MarketCandlesView*>()->priceLines().isEmpty());
            QVERIFY(workspace->findChild<QTreeWidget*>("chartWorkspaceHoldings")->topLevelItemCount()==0);
        }
    }
    void optionalSyntheticLedgerRenders() {
        const auto destination=qEnvironmentVariable("KEEFI_D3_RENDER_DIR");if(destination.isEmpty())QSKIP("Set KEEFI_D3_RENDER_DIR to capture synthetic ledger states");
        QVERIFY(QDir().mkpath(destination));
        const auto previousTheme=KeefiTheme::instance()->mode();
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode);const auto theme=mode==ThemeMode::Light?"light":"dark";
            PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
            ChartWorkspace workspace(&source,nullptr);workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.indicatorPreferences()->markIntroSeen();
            workspace.setIdentity("coin:upbit:KRW-SYN");workspace.setTimeframe(60000);
            workspace.resize(1280,1450);workspace.show();QTRY_VERIFY(transport.token);
            auto response=observedRows(120);auto rows=QJsonDocument::fromJson(response.body).array();
            for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["opening_price"]="100";row["trade_price"]="100";
                row["high_price"]=i==110?"120":"101";row["low_price"]=i==109?"90":"99";rows[i]=row;}
            response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
            QTRY_VERIFY(workspace.chartStudies().pullback.available);
            auto* ledger=workspace.findChild<QWidget*>("chartMarketLedger");QVERIFY(ledger);
            QCOMPARE(workspace.findChild<QLabel*>("chartLedgerUnit_pullback")->text(),QString("/10"));
            QCOMPARE(workspace.findChild<QLabel*>("chartLedgerUnit_pullback")->font(),KeefiFonts::font(FontRole::Number11));
            QTest::qWait(50);QVERIFY(workspace.grab().save(destination+QString("/ledger-%1-wide.png").arg(theme)));
            workspace.resize(720,1000);QTest::qWait(50);
            workspace.findChild<QScrollArea*>("chartWorkspaceScroll")->ensureWidgetVisible(ledger);
            QTest::qWait(50);QVERIFY(workspace.grab().save(destination+QString("/ledger-%1-narrow.png").arg(theme)));
            workspace.setIdentity("coin:upbit:KRW-EMPTY");QTest::qWait(30);
            QVERIFY(workspace.grab().save(destination+QString("/ledger-%1-empty.png").arg(theme)));
        }
        KeefiTheme::instance()->setMode(previousTheme);
    }
    void privateEvidenceContainsNoIdentifiersAndLockRetainsPublicEvidence() {
        class AverageTransport final:public TradingTransport {public:
            void send(quint64 token,const RequestSpec& request)override{QVERIFY(!isMutation(request.operation));
                auto response=upbit_fixture::response(request.operation);
                if(request.operation==Operation::Account){auto rows=QJsonDocument::fromJson(response.body).array();
                    for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();if(row["currency"].toString()=="BTC")row["avg_buy_price"]="120";rows[i]=row;}
                    response.body=QJsonDocument(rows).toJson();}
                QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});}
            void abort()override{}
        } accountTransport;
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-vault"));QVERIFY(vault.create("Fake Chart Design Password! 2026"));
        auto account=upbit_fixture::account();account.id.clear();account.alias="FAKE_PRIVATE_ALIAS_D3";
        QVERIFY(vault.addAccount(account,{{"access_key","FAKE_PRIVATE_KEY_D3"},{"secret","FAKE_PRIVATE_SECRET_D3"}},StorageMode::EncryptedVault));
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &accountTransport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,8000);
        PublicTransport transport;MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return Now;});
        ChartWorkspace workspace(&source,&portfolio);workspace.setTimeframe(60000);workspace.show();workspace.setPrivateEnabled(true);
        auto* view=workspace.findChild<MarketCandlesView*>();QVERIFY(view);
        QTRY_COMPARE(view->priceLines().size(),1);
        QCOMPARE(view->priceLines().first().evidenceNumber,1);
        auto* note=workspace.findChild<QWidget*>("chartEvidenceNotes");QVERIFY(note);QVERIFY(note->isHidden());
        QVERIFY(!workspace.findChild<QLabel*>("chartEvidenceSource_1"));
        QVERIFY(!workspace.findChild<QLabel*>("chartLedgerReference_close"));
        QTRY_VERIFY(transport.token);auto response=observedRows();auto rows=QJsonDocument::fromJson(response.body).array();
        for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["market"]="KRW-BTC";rows[i]=row;}response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
        QTRY_COMPARE_WITH_TIMEOUT(view->priceLines().first().evidenceNumber,2,2500);
        QVERIFY(note->isHidden());QVERIFY(!note->findChild<QLabel*>("chartEvidenceSource_2"));
        QVERIFY(source.series().sourceTimeMs>0);QVERIFY(source.series().receivedAtMs>0);
        QString rendered;for(auto* label:note->findChildren<QLabel*>())rendered+=label->text()+label->toolTip()+label->accessibleName();
        for(const auto& forbidden:{QString("FAKE_PRIVATE_ALIAS_D3"),QString("FAKE_PRIVATE_KEY_D3"),QString("FAKE_PRIVATE_SECRET_D3"),portfolio.connections().first().accountScope,portfolio.connections().first().account.id}){
            QVERIFY(!forbidden.isEmpty());QVERIFY2(!rendered.contains(forbidden),qPrintable(forbidden));}
        const auto lines=workspace.findChild<MarketCandlesView*>()->priceLines();QCOMPARE(lines.size(),1);QCOMPARE(lines.first().evidenceNumber,2);
        const auto renderDirectory=qEnvironmentVariable("KEEFI_D3_RENDER_DIR");
        if(!renderDirectory.isEmpty()){workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.resize(1280,1500);QTest::qWait(40);QVERIFY(workspace.grab().save(renderDirectory+"/ledger-private-synthetic.png"));}
        const auto publicSource=source.series().sourceUrl;
        vault.lock();QVERIFY(view->priceLines().isEmpty());QVERIFY(note->isHidden());
        QCOMPARE(source.series().sourceUrl,publicSource);QCOMPARE(source.series().candles.last().close,QString("139"));
        QCOMPARE(workspace.findChild<QLabel*>("chartLedgerValue_volume")->text(),QString("41"));
    }
};
QTEST_MAIN(ChartDesignTest)
#include "tst_chart_design.moc"
