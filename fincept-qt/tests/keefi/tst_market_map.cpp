#include "TooltipTestSupport.h"
#include "keefi/MarketMap.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/TradingTransport.h"
#include <QDir>
#include <QFontMetricsF>
#include <QPaintEngine>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QHelpEvent>
#include <QToolTip>
#include <memory>

using namespace keefi;

namespace {
class BoardProbeTransport final : public TradingTransport {
  public:
    struct Call { quint64 token; RequestSpec request; };
    QList<Call> calls;
    quint64 pending = 0;
    int aborts = 0;
    bool overlapped = false;
    void send(quint64 token, const RequestSpec& request) override {
        overlapped |= pending != 0;
        pending = token;
        calls.append({token, request});
    }
    void abort() override {
        ++aborts;
        if (!pending) return;
        const auto token = pending;
        pending = 0;
        HttpResult failure; failure.timedOut = true;
        emit finished(token, failure);
    }
    void complete(const HttpResult& response) {
        const auto token = pending;
        pending = 0;
        emit finished(token, response);
    }
    void deliver(quint64 token, const HttpResult& response) { emit finished(token, response); }
};
HttpResult boardResponse(int status, const QByteArray& body = {}) {
    HttpResult response; response.status = status; response.body = body; return response;
}
QByteArray rankingBody(const QByteArray& price = "105") {
    return "[{\"symbol\":\"BTCUSDT\",\"lastPrice\":\"" + price +
        "\",\"priceChangePercent\":\"1.5\",\"quoteVolume\":\"1000\",\"closeTime\":1700000000000,\"count\":1}]";
}
QString compact(QString text) { return text.remove(QRegularExpression(QStringLiteral("\\s+"))); }
struct PaintedText { QString text; QRectF ink, clip; };
struct PaintedFill { QColor color; QRectF rect, clip; };

// Observe glyphs emitted by the public widget's real paintEvent, including its
// active header clip. Accessible text alone cannot establish visible captions.
class MapPaintEngine final : public QPaintEngine {
  public:
    MapPaintEngine() : QPaintEngine(AllFeatures) {}
    QList<PaintedText> texts;
    QList<PaintedFill> fills;
    bool begin(QPaintDevice*) override { return true; }
    bool end() override { return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&) override {}
    void drawPath(const QPainterPath&) override {}
    void drawPolygon(const QPointF*,int,PolygonDrawMode) override {}
    QRectF visibleClip() const {
        return painter()->hasClipping() ? painter()->clipBoundingRect() : QRectF{};
    }
    void drawTextItem(const QPointF& baseline,const QTextItem& item) override {
        auto ink=QFontMetricsF(item.font()).tightBoundingRect(item.text());ink.translate(baseline);
        texts.append({item.text(),ink,visibleClip()});
    }
    void drawRects(const QRectF* rects,int count) override {
        if(state->brush().style()==Qt::NoBrush)return;
        for(int i=0;i<count;++i)fills.append({state->brush().color(),rects[i],visibleClip()});
    }
};
class MapPaintDevice final : public QPaintDevice {
  public:
    explicit MapPaintDevice(QSize size):size_(size){}
    mutable MapPaintEngine engine;
    QPaintEngine* paintEngine() const override { return &engine; }
    int metric(PaintDeviceMetric value) const override {
        switch(value){
        case PdmWidth:return size_.width();case PdmHeight:return size_.height();case PdmDepth:return 32;case PdmNumColors:return 0;
        case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;
        }
    }
  private:
    QSize size_;
};
QList<MarketMapSection> syntheticSections() {
    MarketMapSection coin;coin.key="crypto";coin.title="CRYPTO";coin.available=true;coin.status=QStringLiteral("24시간");coin.source="CoinGecko · 13:06";
    coin.tiles={{"BTC","BTC","Synthetic Bitcoin","109,771,433원","0.47",{},100}};
    MarketMapSection kr;kr.key="kospi";kr.title="KOSPI";kr.available=true;kr.status=QStringLiteral("장 마감");kr.source=QStringLiteral("네이버 증권 · 13:06");
    kr.tiles={{"005930",QStringLiteral("합성 국내"),"Synthetic KR","223,500원","3.71",{},100}};
    MarketMapSection us;us.key="nasdaq";us.title="NASDAQ";us.available=true;us.status="22:30–05:00 KST";us.source="Nasdaq · 13:06";
    us.tiles={{"NVDA","NVDA","Synthetic US","$214.13","-1.43",{},100}};
    return {coin,kr,us};
}
void prepareMap(MarketMapWidget& map,int width,bool dark) {
    KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    map.setStyleSheet(KeefiTheme::localStyleSheet(&map));map.resize(width,760);map.setSections(syntheticSections());
    map.setEvidenceNumbers({{"crypto:BTC",{1}},{"kospi:005930",{2}},{"nasdaq:NVDA",{3}}});map.show();QCoreApplication::processEvents();
}
QList<PaintedText> captionRuns(const QList<PaintedText>& runs,const QString& required) {
    QString all;for(const auto& run:runs)all+=compact(run.text);
    const auto needle=compact(required);const int first=all.indexOf(needle);if(first<0)return {};
    QList<PaintedText> result;int offset=0;
    for(const auto& run:runs){const int length=compact(run.text).size();if(offset<first+needle.size()&&offset+length>first)result.append(run);offset+=length;}
    return result;
}
bool fullyVisible(const QRectF& ink,const QRectF& clip,const QRectF& viewport) {
    // Half a logical pixel allows antialiasing at an edge, not missing glyphs.
    return viewport.adjusted(-.5,-.5,.5,.5).contains(ink)&&
        (clip.isEmpty()||clip.adjusted(-.5,-.5,.5,.5).contains(ink));
}
}

class MarketMapTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase(){QVERIFY(KeefiFonts::registerBundledFonts());}
    void cleanup(){KeefiTheme::instance()->setSettings(nullptr);}
    void binanceRankingStartsWhileAnotherProviderIsHeld(){
        BoardProbeTransport primary, ranking;
        PublicEquityBoard board(&primary, nullptr, &ranking);
        board.refresh();
        QCOMPARE(primary.calls.size(), 1);
        QCOMPARE(primary.calls.first().request.url.host(), QString("api.coingecko.com"));
        board.refreshBinanceRanking();
        QCOMPARE(ranking.calls.size(), 1);
        QCOMPARE(ranking.calls.first().request.url, PublicEquityBoard::request("binance").url);
        QCOMPARE(primary.calls.size(), 1); // The first provider is still deliberately held.
        QCOMPARE(primary.aborts, 0);
        ranking.complete(boardResponse(200, rankingBody()));
        QCOMPARE(board.items("binance").size(), 1);
        QCOMPARE(board.items("binance").first().price, QString("105"));
        QVERIFY(board.items("coin").isEmpty());
        QVERIFY(board.busy()); // The independent primary request remains active.
        QCOMPARE(primary.calls.size(), 1);
        QVERIFY(!primary.overlapped); QVERIFY(!ranking.overlapped);
    }
    void rankingResponsesCannotCompleteAnotherTransportOrReplayOldValues(){
        BoardProbeTransport primary, ranking;
        PublicEquityBoard board(&primary, nullptr, &ranking);
        board.refresh(); board.refreshBinanceRanking();
        const auto primaryToken = primary.pending, rankingToken = ranking.pending;
        QVERIFY(primaryToken != rankingToken);
        primary.deliver(rankingToken, boardResponse(200, rankingBody()));
        ranking.deliver(primaryToken, boardResponse(200, rankingBody()));
        QVERIFY(board.items("binance").isEmpty());
        QCOMPARE(primary.calls.size(), 1); QCOMPARE(ranking.calls.size(), 1);
        ranking.complete(boardResponse(200, rankingBody()));
        QCOMPARE(board.items("binance").first().price, QString("105"));
        ranking.deliver(rankingToken, boardResponse(200, rankingBody("1")));
        QCOMPARE(board.items("binance").first().price, QString("105"));
        primary.complete(boardResponse(200, R"([{"symbol":"btc","name":"Bitcoin","market_cap":2000,"current_price":100,"price_change_percentage_24h":1}])"));
        QCOMPARE(board.items("coin").size(), 1);
        QCOMPARE(board.items("coin").first().price, QString("100"));
        QCOMPARE(board.items("binance").first().price, QString("105"));
        QCOMPARE(primary.calls.size(), 2); // Only the normal board queue advances.
        QVERIFY(!primary.overlapped); QVERIFY(!ranking.overlapped);
    }
    void rankingRefreshCoalescesAndKeepsCooldown_data(){
        QTest::addColumn<int>("status"); QTest::addColumn<int>("localRetry");
        QTest::newRow("success-minute-limit") << 200 << 0;
        QTest::newRow("server-429-retry-after") << 429 << 0;
        QTest::newRow("server-418-retry-after") << 418 << 0;
        QTest::newRow("shared-local-cooldown") << 429 << 180;
    }
    void rankingRefreshCoalescesAndKeepsCooldown(){
        QFETCH(int, status); QFETCH(int, localRetry);
        BoardProbeTransport primary, ranking;
        PublicEquityBoard board(&primary, nullptr, &ranking);
        for (int i = 0; i < 25; ++i) board.refreshBinanceRanking();
        QCOMPARE(ranking.calls.size(), 1); QVERIFY(board.busy());
        auto response = boardResponse(status, status == 200 ? rankingBody() : QByteArray{});
        response.localRetryAfterSeconds = localRetry;
        if (status != 200) response.headers.insert("Retry-After", "180");
        ranking.complete(response);
        QVERIFY(!board.busy());
        if (status == 200) QCOMPARE(board.items("binance").size(), 1);
        else QVERIFY(!board.status("binance").isEmpty());
        for (int i = 0; i < 25; ++i) board.refreshBinanceRanking();
        QCOMPARE(ranking.calls.size(), 1);
        QCOMPARE(primary.calls.size(), 0);
        board.refresh();
        QCOMPARE(primary.calls.size(), 1); // A Binance cooldown does not block other providers.
        QVERIFY(!primary.overlapped); QVERIFY(!ranking.overlapped);
    }
    void omittedOrIdenticalRankingTransportStaysSerial_data(){
        QTest::addColumn<bool>("sameTransport");
        QTest::newRow("legacy-constructor") << false;
        QTest::newRow("same-transport") << true;
    }
    void omittedOrIdenticalRankingTransportStaysSerial(){
        QFETCH(bool, sameTransport);
        BoardProbeTransport primary;
        PublicEquityBoard board(&primary, nullptr, sameTransport ? &primary : nullptr);
        board.refresh(); board.refreshBinanceRanking(); board.refreshBinanceRanking();
        QCOMPARE(primary.calls.size(), 1);
        primary.complete(boardResponse(503));
        QCOMPARE(primary.calls.size(), 2);
        QCOMPARE(primary.calls.last().request.url, PublicEquityBoard::request("binance").url);
        primary.complete(boardResponse(200, rankingBody()));
        QCOMPARE(board.items("binance").size(), 1);
        for (int i = 0; i < 3; ++i) primary.complete(boardResponse(503));
        QVERIFY(!board.busy());
        QCOMPARE(primary.calls.size(), 5); // coin, Binance, kr, us, us-status
        QVERIFY(!primary.overlapped); QCOMPARE(primary.aborts, 0);
    }
    void destroyingActiveTransportsClearsBusyWithoutFallbackOrDanglingAccess(){
        auto primary = std::make_unique<BoardProbeTransport>();
        auto ranking = std::make_unique<BoardProbeTransport>();
        PublicEquityBoard board(primary.get(), nullptr, ranking.get());
        board.refresh(); board.refreshBinanceRanking();
        ranking.reset();
        QVERIFY(board.status("binance").contains(QStringLiteral("연결 실패")));
        QVERIFY(board.busy());
        board.refreshBinanceRanking();
        QCOMPARE(primary->calls.size(), 1);
        primary.reset();
        QVERIFY(!board.busy());
        board.refresh(); board.refreshBinanceRanking();
        QVERIFY(!board.busy());
    }
    void destroyingBoardCancelsBothActiveRequestsWithoutHandlingAbortReplies(){
        BoardProbeTransport primary, ranking;
        auto board = std::make_unique<PublicEquityBoard>(&primary, nullptr, &ranking);
        QSignalSpy changes(board.get(), &PublicEquityBoard::changed);
        board->refresh(); board->refreshBinanceRanking();
        board.reset();
        QCOMPARE(primary.aborts, 1); QCOMPARE(ranking.aborts, 1);
        QCOMPARE(primary.pending, quint64(0)); QCOMPARE(ranking.pending, quint64(0));
        QCOMPARE(primary.calls.size(), 1); QCOMPARE(ranking.calls.size(), 1);
        QCOMPARE(changes.count(), 0);
        primary.deliver(1, boardResponse(200, rankingBody()));
        ranking.deliver(2, boardResponse(200, rankingBody()));
        { PublicEquityBoard idle(&primary, nullptr, &ranking); }
        QCOMPARE(primary.aborts, 1); QCOMPARE(ranking.aborts, 1);
    }
    void marketNameInTheActualTooltipStaysLiteral() {
        MarketMapWidget map;
        MarketMapSection section;
        section.key = QStringLiteral("synthetic");
        section.title = QStringLiteral("합성 시장");
        section.available = true;
        MarketMapTile tile;
        tile.id = QStringLiteral("SYN");
        tile.label = QStringLiteral("SYN");
        tile.name = QStringLiteral("<b>합성 원천 이름</b>");
        tile.price = QStringLiteral("100");
        tile.changePercent = QStringLiteral("1");
        tile.weight = 1;
        section.tiles = {tile};
        map.resize(820, 560);
        map.setSections({section});
        map.show();
        QVERIFY(!map.tileRects().isEmpty());
        const auto point = map.tileRects().first().second.center().toPoint();
        QHelpEvent event(QEvent::ToolTip, point, map.mapToGlobal(point));
        QCoreApplication::sendEvent(&map, &event);
        const auto shown = keefi::test::projectTooltip(QToolTip::text());
        QToolTip::hideText();
        QVERIFY(shown.text.startsWith(tile.name + QLatin1Char('\n')));
        QVERIFY(!shown.image);
        QVERIFY(!shown.anchor);
        QCOMPARE(shown.resourceRequests, 0);
    }
    void accessibleExplanationUsesMarketCapAndSelectionContext(){
        MarketMapWidget map; prepareMap(map,1040,false);
        const auto explanation=map.accessibleDescription();
        QVERIFY(explanation.contains(QStringLiteral("시가총액")));
        QVERIFY(!explanation.contains(QStringLiteral("크기는 24시간 거래대금")));
        QVERIFY(explanation.contains(QStringLiteral("요약")));
        map.setFilter("crypto");
        QVERIFY(map.accessibleDescription().contains(QStringLiteral("시가총액")));
    }
    void referencesAndSourceFooterAreAbsentFromPaintAndAccessibility(){
        MarketMapWidget map;prepareMap(map,1040,false);map.setEvidenceStates({{"crypto:BTC",QStringLiteral("지연")}});
        MapPaintDevice device(map.size());map.render(&device);QString painted;
        for(const auto& run:device.engine.texts)painted+=run.text;
        QVERIFY2(!QRegularExpression(QStringLiteral("\\[[0-9]+\\]")).match(painted).hasMatch(),"The real map paint path must not render evidence references");
        QVERIFY(!QRegularExpression(QStringLiteral("\\[[0-9]+\\]")).match(map.accessibleDescription()).hasMatch());
        QVERIFY(!painted.contains(QStringLiteral("CoinGecko")));QVERIFY(painted.contains(QStringLiteral("지연")));
        const auto tile=map.tileRects().first();const auto point=tile.second.center().toPoint();
        QHelpEvent tooltip(QEvent::ToolTip,point,map.mapToGlobal(point));QCoreApplication::sendEvent(&map,&tooltip);
        QVERIFY(QToolTip::text().contains(QStringLiteral("CoinGecko · 13:06")));QVERIFY(!QToolTip::text().contains("[1]"));QToolTip::hideText();
        map.select("crypto","BTC");QCOMPARE(map.selectedSection(),QString("crypto"));QCOMPARE(map.selectedId(),QString("BTC"));
    }
    void tileChangeIsCompleteOrOmitted_data(){
        QTest::addColumn<bool>("dark");QTest::addColumn<bool>("small");QTest::addColumn<QString>("percent");QTest::addColumn<QString>("expected");
        for(const bool dark:{false,true})for(const bool small:{true,false}) {
            const auto row=QString("%1-%2-").arg(dark?"dark":"light",small?"small":"wide");
            QTest::newRow(qPrintable(row+"positive"))<<dark<<small<<QString("123456789.12")<<QStringLiteral("▲ +123456789.12%");
            QTest::newRow(qPrintable(row+"negative"))<<dark<<small<<QString("-123456789.12")<<QStringLiteral("▼ -123456789.12%");
            QTest::newRow(qPrintable(row+"reported-jpm"))<<dark<<small<<QString("1.25")<<QStringLiteral("▲ +1.25%");
        }
    }
    void tileChangeIsCompleteOrOmitted(){
        QFETCH(bool,dark);QFETCH(bool,small);QFETCH(QString,percent);QFETCH(QString,expected);
        KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        const auto textWidth=QFontMetricsF(KeefiFonts::font(FontRole::Number11)).horizontalAdvance(expected);
        // The small fixture enters the existing >46px detail policy, but has
        // less inner room than the complete original string actually needs.
        MarketMapWidget map;map.setStyleSheet(KeefiTheme::localStyleSheet(&map));map.resize(small?19+qMax(47,int(textWidth)-1):420,760);
        MarketMapSection section;section.key="fit";section.title="FIT";section.available=true;
        section.tiles={{"TARGET","TARGET","Synthetic target",{},percent,{},100}};map.setSections({section});map.show();QCoreApplication::processEvents();
        const auto rects=map.tileRects();QCOMPARE(rects.size(),1);const auto tile=rects.first().second;
        QVERIFY(small?tile.width()-16>46&&textWidth>tile.width()-16:textWidth<tile.width()-16);
        MapPaintDevice device(map.size());map.render(&device);QList<PaintedText> changeGlyphs;
        const QRegularExpression numeric(QStringLiteral("[0-9%▲▼+\\-]"));
        for(const auto& run:device.engine.texts)if(run.ink.center().y()>=tile.top()&&run.ink.center().y()<=tile.bottom()&&numeric.match(run.text).hasMatch())changeGlyphs.append(run);
        if(small)QVERIFY2(changeGlyphs.isEmpty(),"A tile without room for the complete original change must omit that line, including every partial glyph");
        else {
            QString actual;for(const auto& run:changeGlyphs)actual+=run.text;
            QCOMPARE(compact(actual),compact(expected));
            for(const auto& run:changeGlyphs)QVERIFY2(fullyVisible(run.ink,run.clip,tile),"Every glyph of a visible change must stay inside its own tile and active paint clip");
        }
    }
    void completeHeadersStayInsideTheirSection_data(){
        QTest::addColumn<int>("width");QTest::addColumn<bool>("dark");
        for(const int width:{322,1040})for(const bool dark:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(width).arg(dark?"dark":"light")))<<width<<dark;
    }
    void completeHeadersStayInsideTheirSection(){
        QFETCH(int,width);QFETCH(bool,dark);MarketMapWidget map;prepareMap(map,width,dark);
        const auto out=qEnvironmentVariable("KEEFI_FOLLOWUP_MAP_RENDER_DIR");if(!out.isEmpty()){
            QVERIFY(QDir().mkpath(out));QVERIFY(map.grab().save(out+QString("/map-%1-%2.png").arg(width).arg(dark?"dark":"light")));
        }
        MapPaintDevice device(map.size());map.render(&device);
        for(const auto& section:syntheticSections())for(const auto& caption:{section.status}){
            const auto runs=captionRuns(device.engine.texts,caption);QVERIFY2(!runs.isEmpty(),qPrintable(QString("Full caption must be painted: %1").arg(caption)));
            qreal tileTop=map.height();for(const auto& tile:map.tileRects())if(tile.first.startsWith(section.key+":"))tileTop=qMin(tileTop,tile.second.top());
            for(const auto& run:runs){
                QVERIFY2(fullyVisible(run.ink,run.clip,map.rect()),qPrintable(QString("Caption '%1' ink %2,%3 %4x%5 exceeds its viewport/header clip").arg(caption).arg(run.ink.x()).arg(run.ink.y()).arg(run.ink.width()).arg(run.ink.height())));
                QVERIFY2(run.ink.bottom()<=tileTop+.5,qPrintable(QString("Caption '%1' overlaps a market tile").arg(caption)));
            }
        }
        QVERIFY(!QRegularExpression(QStringLiteral("\\[[0-9]+\\]")).match(map.accessibleDescription()).hasMatch());
    }
    void completeLegendStaysInsideViewport_data(){completeHeadersStayInsideTheirSection_data();}
    void completeLegendStaysInsideViewport(){
        QFETCH(int,width);QFETCH(bool,dark);MarketMapWidget map;prepareMap(map,width,dark);MapPaintDevice device(map.size());map.render(&device);
        qreal tileBottom=0;for(const auto& tile:map.tileRects())tileBottom=qMax(tileBottom,tile.second.bottom());
        for(const auto& caption:{QStringLiteral("크기 · 시가총액"),QStringLiteral("색 · 전일 대비(코인 24시간)"),QStringLiteral("▼ -3%"),QStringLiteral("+3% ▲")}){
            const auto runs=captionRuns(device.engine.texts,caption);QVERIFY2(!runs.isEmpty(),qPrintable(QString("Full legend must be painted: %1").arg(caption)));
            for(const auto& run:runs){QVERIFY2(fullyVisible(run.ink,run.clip,map.rect()),qPrintable(QString("Legend '%1' exceeds its viewport").arg(caption)));QVERIFY(run.ink.top()>=tileBottom-.5);}
        }
        const QList<QColor> colors{MovementColorPolicy::heatFor(&map,MovementDirection::Down,2),MovementColorPolicy::heatFor(&map,MovementDirection::Down,1),MovementColorPolicy::heatFor(&map,MovementDirection::Down,0),themeColors().surface,
            MovementColorPolicy::heatFor(&map,MovementDirection::Up,0),MovementColorPolicy::heatFor(&map,MovementDirection::Up,1),MovementColorPolicy::heatFor(&map,MovementDirection::Up,2)};
        int swatches=0;for(const auto& fill:device.engine.fills)if(fill.rect.top()>=tileBottom&&colors.contains(fill.color)){
            ++swatches;QVERIFY2(fullyVisible(fill.rect,fill.clip,map.rect()),"Every heat legend swatch must remain inside the viewport");
        }
        QCOMPARE(swatches,7);
    }
};
QTEST_MAIN(MarketMapTest)
#include "tst_market_map.moc"
