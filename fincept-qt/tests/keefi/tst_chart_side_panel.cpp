#include "keefi/AccountModel.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartTimeframePins.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/CoinTapeView.h"
#include "keefi/InfoButton.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketCandles.h"
#include "keefi/Motion.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>

using namespace keefi;

namespace {
class FlowHttp final : public TradingTransport {
  public:
    QList<RequestSpec> requests;
    quint64 token = 0;
    void send(quint64 value, const RequestSpec& request) override {
        QVERIFY(!request.authenticated);
        QCOMPARE(request.method, QString("GET"));
        token = value;
        requests.append(request);
    }
    void abort() override {}
};
class QuietTransport final : public TradingTransport {
  public:
    int sent = 0;
    void send(quint64, const RequestSpec&) override { ++sent; }
    void abort() override {}
};
class FlowSocket final : public PublicCandleSocket {
  public:
    quint64 generation = 0;
    void open(quint64 value, const QUrl&) override { generation = value; }
    void send(quint64, const QByteArray&) override {}
    void abort() override {}
};
class MarketsTransport final : public TradingTransport {
  public:
    void abort() override {}
    void send(quint64 token, const RequestSpec& request) override {
        HttpResult response;
        response.status = 200;
        response.body = request.operation == Operation::Markets
            ? QByteArray(R"([{"market":"KRW-BTC","korean_name":"합성비트","english_name":"Synthetic Bit","market_event":{"warning":false,"caution":{}}}])")
            : QByteArray("[]");
        QTimer::singleShot(0, this, [this, token, response] { emit finished(token, response); });
    }
};

CoinFlowInstrument upbitInstrument(const QString& symbol, qint64 now) {
    CoinFlowInstrument item;
    item.venue = CoinFlowVenue::UpbitSpot;
    item.grain = CoinFlowGrain::UpbitTrade;
    item.identity = "coin:upbit:" + symbol;
    item.symbol = symbol;
    item.baseAsset = symbol.section('-', 1);
    item.quoteAsset = symbol.section('-', 0, 0);
    item.verified = true;
    item.evidence = {"합성 공개 종목 목록", "https://api.upbit.com/v1/market/all", 0, now, now};
    return item;
}
QByteArray upbitTrade(const QString& symbol, qint64 at, const QString& price, const QString& volume, bool bid, qint64 sequence) {
    return QJsonDocument(QJsonObject{{"type", "trade"}, {"code", symbol}, {"trade_price", price}, {"trade_volume", volume},
        {"ask_bid", bid ? "BID" : "ASK"}, {"sequential_id", QString::number(sequence)},
        {"trade_timestamp", QString::number(at)}, {"timestamp", QString::number(at)}, {"stream_type", "REALTIME"}})
        .toJson(QJsonDocument::Compact);
}
// A live Upbit lane: catalogue, empty REST seed and an open socket.
struct LiveFlow {
    qint64 now = 0;
    FlowHttp http;
    FlowSocket* wire = nullptr;
    std::unique_ptr<CoinFlowSource> flow;
    explicit LiveFlow(qint64 start, const QString& symbol = QStringLiteral("KRW-BTC")) : now(start) {
        auto socket = std::make_unique<FlowSocket>();
        wire = socket.get();
        flow = std::make_unique<CoinFlowSource>(&http, std::move(socket), std::unique_ptr<PublicCandleSocket>{}, nullptr, [this] { return now; });
        flow->setPublicCatalog(CoinFlowVenue::UpbitSpot, {upbitInstrument(symbol, now)});
        flow->setSelection("coin:upbit:" + symbol);
        flow->setActive(true);
    }
    bool open() {
        if (!wire->generation || !http.token) return false;
        HttpResult seeded;
        seeded.status = 200;
        seeded.body = "[]";
        emit http.finished(http.token, seeded);
        emit wire->connected(wire->generation);
        return true;
    }
    void trade(qint64 at, const QString& price, const QString& volume, bool bid, qint64 sequence, const QString& symbol = QStringLiteral("KRW-BTC")) {
        emit wire->received(wire->generation, upbitTrade(symbol, at, price, volume, bid, sequence));
    }
};

class CandleHttp final : public TradingTransport {
  public:
    quint64 token = 0;
    void send(quint64 value, const RequestSpec& request) override { QVERIFY(!request.authenticated); token = value; }
    void abort() override {}
};
// Synthetic public minute candles: a deterministic walk, never market data.
HttpResult syntheticCandles(qint64 now, int count = 200) {
    QJsonArray rows;
    const auto end = now / 60000 * 60000;
    qint64 price = 95000000;
    quint32 seed = 20261117;
    for (int i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const qint64 open = price, close = price + (qint64(seed % 9) - 4) * 40000;
        const qint64 high = qMax(open, close) + (seed >> 8) % 5 * 15000, low = qMin(open, close) - (seed >> 12) % 5 * 15000;
        price = close;
        const auto at = end - (count - i) * 60000;
        rows.append(QJsonObject{{"market", "KRW-BTC"},
            {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp", QString::number(at + 1000)}, {"unit", "1"},
            {"opening_price", QString::number(open)}, {"high_price", QString::number(high)},
            {"low_price", QString::number(low)}, {"trade_price", QString::number(close)},
            {"candle_acc_trade_volume", QString::number(2 + (seed >> 4) % 20)}});
    }
    HttpResult response;
    response.status = 200;
    response.body = QJsonDocument(rows).toJson();
    return response;
}

ChartWorkspace* workspaceAt(QWidget& host, int width, int height = 900) {
    auto* http = new CandleHttp;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto* candles = new MarketCandles(http, std::unique_ptr<PublicCandleSocket>{}, [now] { return now; });
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* workspace = new ChartWorkspace(candles, nullptr, &host);
    // Parent candles and http after workspace so Qt destroys workspace first.
    candles->setParent(&host);
    http->setParent(&host);
    workspace->indicatorPreferences()->markIntroSeen();
    workspace->setIdentity(QStringLiteral("coin:upbit:KRW-BTC"));
    workspace->setTimeframe(60000);
    layout->addWidget(workspace);
    host.resize(width, height);
    host.show();
    if (http->token) emit http->finished(http->token, syntheticCandles(now, 60));
    QCoreApplication::processEvents();
    return workspace;
}
QWidget* sidePanel(QWidget& workspace) { return workspace.findChild<QWidget*>("chartSidePanel"); }
QWidget* chartColumn(QWidget& workspace) { return workspace.findChild<QWidget*>("chartWorkspaceMain"); }
QWidget* chartCanvas(QWidget& workspace) {
    for (auto* widget : workspace.findChildren<QWidget*>("marketCandlesChart"))
        if (widget->isVisible()) return widget;
    return nullptr;
}
QSplitter* splitter(QWidget& workspace) { return workspace.findChild<QSplitter*>("chartWorkspaceSplitter"); }
QTabBar* tabs(QWidget& workspace) { return workspace.findChild<QTabBar*>("chartSidePanelTabs"); }
QScrollArea* scroll(QWidget& workspace) { return workspace.findChild<QScrollArea*>("chartWorkspaceScroll"); }
QComboBox* layoutSelector(QWidget& workspace) { return workspace.findChild<QComboBox*>("chartLayoutSelector"); }
CoinTapeView* tape(QWidget& workspace) { return workspace.findChild<CoinTapeView*>("chartCoinTape"); }
bool settled(QWidget& workspace) {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
    return true;
}
// Both sides keep their floors (including the actual CandleChart canvas >= 680px) while beside.
void verifyBeside(QWidget& workspace, int preferred = 0) {
    auto* panel = sidePanel(workspace);
    auto* chart = chartColumn(workspace);
    auto* canvas = chartCanvas(workspace);
    QVERIFY(panel && chart && canvas);
    QVERIFY(panel->isVisible());
    QVERIFY(splitter(workspace)->isAncestorOf(panel));
    QVERIFY2(chart->width() >= ChartWorkspace::ChartColumnMinimumWidth, qPrintable(QString::number(chart->width())));
    QVERIFY2(canvas->width() >= ChartWorkspace::ChartMinimumWidth, qPrintable(QStringLiteral("canvas %1 < %2").arg(canvas->width()).arg(ChartWorkspace::ChartMinimumWidth)));
    QVERIFY2(panel->width() >= ChartWorkspace::SidePanelMinimumWidth, qPrintable(QString::number(panel->width())));
    if (preferred) {
        const int extent = splitter(workspace)->width() - splitter(workspace)->handleWidth();
        const int expected = qMax(ChartWorkspace::SidePanelMinimumWidth, qMin(extent - ChartWorkspace::ChartColumnMinimumWidth, preferred));
        QCOMPARE(panel->width(), expected);
    }
    const QRect chartArea(chart->mapTo(&workspace, QPoint()), chart->size());
    const QRect panelArea(panel->mapTo(&workspace, QPoint()), panel->size());
    QVERIFY(panelArea.left() > chartArea.right());
    QVERIFY(workspace.rect().contains(panelArea));
    QCOMPARE(scroll(workspace)->horizontalScrollBar()->maximum(), 0);
}
void dragHandle(QSplitter* splitter, int dx) {
    auto* handle = splitter->handle(1);
    const QPoint start(handle->width() / 2, handle->height() / 2);
    QTest::mousePress(handle, Qt::LeftButton, {}, start);
    QTest::mouseMove(handle, start + QPoint(dx / 2, 0));
    QTest::mouseMove(handle, start + QPoint(dx, 0));
    QTest::mouseRelease(handle, Qt::LeftButton, {}, start + QPoint(dx, 0));
}
CoinFlowSnapshot binanceSnapshot(qint64 at) {
    CoinFlowSnapshot snapshot;
    snapshot.identity = "coin:binance:BTCUSDT";
    auto& spot = snapshot.primary.instrument;
    spot.venue = CoinFlowVenue::BinanceSpot; spot.grain = CoinFlowGrain::SpotAggregate;
    spot.identity = snapshot.identity; spot.symbol = "BTCUSDT"; spot.baseAsset = "BTC"; spot.quoteAsset = "USDT";
    spot.status = "TRADING"; spot.verified = true;
    spot.evidence = {"합성 공개 종목 목록", "https://api.binance.com/api/v3/exchangeInfo", 0, at, at};
    CoinTradeObservation buy;
    buy.id = "9001"; buy.price = "64386.5"; buy.quantity = "0.5"; buy.side = CoinFlowSide::Buy;
    buy.tradeAtMs = at - 2000; buy.eventAtMs = buy.tradeAtMs; buy.receivedAtMs = buy.knownAtMs = at - 1990; buy.epoch = 1;
    CoinTradeObservation small = buy;
    small.id = "9002"; small.price = "1.5"; small.quantity = "3"; small.side = CoinFlowSide::Sell; small.tradeAtMs = at - 1000;
    snapshot.primary.trades = {buy, small};
    CoinFlowWindow futures;
    auto& perpetual = futures.instrument;
    perpetual.venue = CoinFlowVenue::BinanceUsdM; perpetual.grain = CoinFlowGrain::Futures100msAggregate;
    perpetual.identity = "usdm_futures:binance:BTCUSDT"; perpetual.symbol = "BTCUSDT"; perpetual.baseAsset = "BTC";
    perpetual.quoteAsset = "USDT"; perpetual.marginAsset = "USDT"; perpetual.contractType = "PERPETUAL";
    perpetual.status = "TRADING"; perpetual.underlyingType = "COIN"; perpetual.verified = true;
    CoinTradeObservation companionTrade = buy;
    companionTrade.id = "7777"; companionTrade.price = "64390"; companionTrade.quantity = "9"; companionTrade.tradeAtMs = at - 500;
    futures.trades = {companionTrade};
    snapshot.companion = futures;
    CoinLiquidationObservation liquidation;
    liquidation.symbol = "BTCUSDT"; liquidation.side = "SELL"; liquidation.price = "64300"; liquidation.averagePrice = "64385.1";
    liquidation.originalQuantity = "2"; liquidation.lastFilledQuantity = "2"; liquidation.cumulativeFilledQuantity = "2";
    liquidation.orderStatus = "FILLED"; liquidation.tradeAtMs = at - 1500; liquidation.eventAtMs = liquidation.tradeAtMs;
    liquidation.receivedAtMs = liquidation.knownAtMs = at - 1490; liquidation.epoch = 1;
    snapshot.liquidations = {liquidation};
    return snapshot;
}
} // namespace

class ChartSidePanelTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); }

    void panelKeepsBothFloorsAtCommonWidths_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("beside");
        QTest::addColumn<int>("preferred");
        QTest::newRow("960") << 960 << false << 0;
        QTest::newRow("1079") << 1079 << false << 0;
        QTest::newRow("1080") << 1080 << true << ChartWorkspace::SidePanelDefaultWidth;
        QTest::newRow("1100") << 1100 << true << ChartWorkspace::SidePanelDefaultWidth;
        QTest::newRow("1280") << 1280 << true << ChartWorkspace::SidePanelDefaultWidth;
        QTest::newRow("1439") << 1439 << true << ChartWorkspace::SidePanelDefaultWidth;
        QTest::newRow("1440") << 1440 << true << ChartWorkspace::SidePanelWideWidth;
    }
    void panelKeepsBothFloorsAtCommonWidths() {
        QFETCH(int, width);
        QFETCH(bool, beside);
        QFETCH(int, preferred);
        QWidget host;
        auto* workspace = workspaceAt(host, width);
        settled(*workspace);
        QCOMPARE(workspace->sidePanelBesideChart(), beside);
        auto* panel = sidePanel(*workspace);
        auto* positions = workspace->findChild<QWidget*>("chartWorkspacePositions");
        auto* details = workspace->findChild<QPushButton*>("chartDetailsToggle");
        QVERIFY(panel && positions && details);
        if (beside) {
            verifyBeside(*workspace, preferred);
            QVERIFY(!positions->isAncestorOf(panel));
            QCOMPARE(details->text(), QStringLiteral("포지션 펼치기"));
            return;
        }
        // Narrow: the same panel waits in the details disclosure under the chart.
        QVERIFY(positions->isAncestorOf(panel));
        QVERIFY(!panel->isVisible());
        QCOMPARE(chartColumn(*workspace)->width(), splitter(*workspace)->width());
        QCOMPARE(scroll(*workspace)->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(details->text(), QStringLiteral("포지션 · 지금 상태 펼치기"));
        details->click();
        settled(*workspace);
        QVERIFY(panel->isVisible());
        QVERIFY(panel->width() > 0);
        auto* chart = workspace->findChild<QWidget*>("chartWorkspaceChartPane");
        QVERIFY(panel->mapTo(workspace, QPoint()).y() > chart->mapTo(workspace, QPoint()).y() + chart->height());
        QCOMPARE(scroll(*workspace)->horizontalScrollBar()->maximum(), 0);
    }

    void panelFoldsBelowTheWindowThresholdAndReturns() {
        QWidget host;
        auto* workspace = workspaceAt(host, 1440);
        settled(*workspace);
        verifyBeside(*workspace, ChartWorkspace::SidePanelWideWidth);
        for (const int width : {1079, 1080, 1100, 960, 1280, 1440}) {
            host.resize(width, 900);
            settled(*workspace);
            const bool beside = width >= ChartWorkspace::SidePanelCollapseWidth;
            QCOMPARE(workspace->sidePanelBesideChart(), beside);
            if (beside) verifyBeside(*workspace, width >= 1440 ? ChartWorkspace::SidePanelWideWidth : ChartWorkspace::SidePanelDefaultWidth);
            else {
                QVERIFY(!sidePanel(*workspace)->isVisible());
                QCOMPARE(scroll(*workspace)->horizontalScrollBar()->maximum(), 0);
            }
        }
    }

    void twoChartGridMovesThePanelAway() {
        QWidget host;
        auto* workspace = workspaceAt(host, 1440);
        settled(*workspace);
        verifyBeside(*workspace);
        auto* selector = layoutSelector(*workspace);
        QVERIFY(selector);
        selector->setCurrentIndex(selector->findData(2));
        settled(*workspace);
        QVERIFY(!workspace->sidePanelBesideChart());
        QVERIFY(!sidePanel(*workspace)->isVisible());
        QVERIFY(!workspace->coinTapeVisible());
        QCOMPARE(chartColumn(*workspace)->width(), splitter(*workspace)->width());
        QVERIFY(!workspace->findChild<QAction*>("chartSidePanelAction")->isEnabled());
        selector->setCurrentIndex(selector->findData(1));
        settled(*workspace);
        QVERIFY(workspace->sidePanelBesideChart());
        verifyBeside(*workspace, ChartWorkspace::SidePanelWideWidth);
    }

    void hiddenPanelComesBackWithWidthAfterTheWindowShrinks_data() {
        QTest::addColumn<QString>("hide");
        QTest::addColumn<bool>("dragged");
        for (const bool dragged : {false, true}) {
            QTest::newRow(dragged ? "menu-dragged" : "menu") << QString("menu") << dragged;
            QTest::newRow(dragged ? "grid-dragged" : "grid") << QString("grid") << dragged;
            QTest::newRow(dragged ? "narrow-dragged" : "narrow") << QString("narrow") << dragged;
        }
    }
    void hiddenPanelComesBackWithWidthAfterTheWindowShrinks() {
        QFETCH(QString, hide);
        QFETCH(bool, dragged);
        QWidget host;
        auto* workspace = workspaceAt(host, 1600);
        settled(*workspace);
        verifyBeside(*workspace, ChartWorkspace::SidePanelWideWidth);
        if (dragged) {
            // A share that stays strictly between the default (320) and canvas-floor clamp at 1200:
            // ~460 / 1572 → ~343 of 1172, distinct from both default 320 and clamped ceiling 408.
            dragHandle(splitter(*workspace), -100);
            settled(*workspace);
            QVERIFY2(sidePanel(*workspace)->width() > ChartWorkspace::SidePanelWideWidth + 60, qPrintable(QString::number(sidePanel(*workspace)->width())));
        }
        const auto before = splitter(*workspace)->sizes();
        QCOMPARE(before.size(), 2);
        const double share = double(before[1]) / (before[0] + before[1]);
        auto* action = workspace->findChild<QAction*>("chartSidePanelAction");
        auto* selector = layoutSelector(*workspace);
        QVERIFY(action && selector);
        if (hide == "menu") action->setChecked(false);
        else if (hide == "grid") selector->setCurrentIndex(selector->findData(2));
        else host.resize(960, 900);
        settled(*workspace);
        QVERIFY(!sidePanel(*workspace)->isVisible());
        host.resize(1200, 900);
        settled(*workspace);
        if (hide == "menu") action->setChecked(true);
        else if (hide == "grid") selector->setCurrentIndex(selector->findData(1));
        settled(*workspace);
        QTRY_VERIFY(sidePanel(*workspace)->isVisible());
        verifyBeside(*workspace);
        const auto sizes = splitter(*workspace)->sizes();
        QCOMPARE(sizes.size(), 2);
        QVERIFY(sizes[0] >= ChartWorkspace::ChartColumnMinimumWidth && sizes[1] >= ChartWorkspace::SidePanelMinimumWidth);
        auto* canvas = chartCanvas(*workspace);
        QVERIFY(canvas);
        QVERIFY(canvas->width() >= ChartWorkspace::ChartMinimumWidth);
        // A dragged width comes back as the same share of the narrower splitter,
        // within both floors; an untouched panel keeps its default width.
        const int extent = splitter(*workspace)->width() - splitter(*workspace)->handleWidth();
        const int preferred = dragged ? qRound(extent * share) : ChartWorkspace::SidePanelDefaultWidth;
        const int expected = qMax(ChartWorkspace::SidePanelMinimumWidth, qMin(extent - ChartWorkspace::ChartColumnMinimumWidth, preferred));
        QVERIFY2(qAbs(sizes[1] - expected) <= 1, qPrintable(QStringLiteral("panel %1, expected %2").arg(sizes[1]).arg(expected)));
    }

    void tapeVisibilityFollowsTheChosenTab() {
        QWidget host;
        auto* workspace = workspaceAt(host, 1440);
        settled(*workspace);
        QSignalSpy changes(workspace, &ChartWorkspace::coinTapeVisibilityChanged);
        QVERIFY2(!workspace->coinTapeVisible(), "The evidence tab opens first; no trade stream starts by itself");
        QCOMPARE(tabs(*workspace)->currentIndex(), 1);
        tabs(*workspace)->setCurrentIndex(0);
        QVERIFY(workspace->coinTapeVisible());
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.last().first().toBool(), true);
        tabs(*workspace)->setCurrentIndex(1);
        QVERIFY(!workspace->coinTapeVisible());
        QCOMPARE(changes.last().first().toBool(), false);
        tabs(*workspace)->setCurrentIndex(0);
        workspace->findChild<QAction*>("chartSidePanelAction")->setChecked(false);
        QVERIFY(!workspace->coinTapeVisible());
        workspace->findChild<QAction*>("chartSidePanelAction")->setChecked(true);
        QVERIFY(workspace->coinTapeVisible());
        host.resize(960, 900);
        settled(*workspace);
        QVERIFY2(!workspace->coinTapeVisible(), "Folded under a closed disclosure the tape is not on screen");
        workspace->findChild<QPushButton*>("chartDetailsToggle")->click();
        settled(*workspace);
        QVERIFY(workspace->coinTapeVisible());
        host.hide();
        QVERIFY(!workspace->coinTapeVisible());
        QCOMPARE(changes.last().first().toBool(), false);
        QVERIFY(changes.size() >= 6);
    }

    void windowCollectsTradesOnlyForAVisibleTape() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
        QSettings profile(directory.filePath("synthetic-tape.ini"), QSettings::IniFormat);
        Vault vault(directory.filePath("synthetic-tape-locked-vault"));
        AccountModel accounts(&vault);
        PortfolioController::Factories factories;
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        QuietTransport candleHttp, metadata;
        MarketsTransport publicHttp;
        PublicMarketCatalog markets(&publicHttp);
        FlowHttp flowHttp, minuteHttp;
        MarketCandles candles(&candleHttp), minutes(&minuteHttp);
        CoinFlowSource flow(&flowHttp);
        auto window = std::make_unique<LocalWindow>(vault, accounts);
        window->setPortfolioController(&portfolio);
        window->setChartSettings(&profile);
        window->setChartInstrumentTransport(&metadata);
        window->setPublicMarketCatalog(&markets, false);
        markets.refresh();
        QTRY_VERIFY(markets.marketListAvailable());
        window->setCoinFlowSource(&flow);
        window->setCoinFlowMinuteModel(&minutes);
        window->setMarketCandles(&candles);
        auto* workspace = window->findChild<ChartWorkspace*>("chartWorkspace");
        QVERIFY(workspace);
        for (const auto& id : workspace->indicatorPreferences()->supportedIds("coin")) workspace->indicatorPreferences()->setEnabled(id, false);
        window->resize(1440, 900);
        window->show();
        auto* pages = window->findChild<QTabWidget*>("portfolioPages");
        QVERIFY(pages);
        pages->setCurrentWidget(workspace);
        QVERIFY(workspace->setIdentity("coin:upbit:KRW-BTC"));
        QTest::qWait(50);
        QVERIFY(workspace->sidePanelBesideChart());
        QVERIFY2(!flow.active(), "With every flow study off and the evidence tab shown, no public trades are read");
        tabs(*workspace)->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(flow.active(), 1500);
        QCOMPARE(flow.snapshot().identity, QString("coin:upbit:KRW-BTC"));
        QTest::qWait(100);
        QVERIFY2(minuteHttp.requests.isEmpty(), "The tape alone reads no completed-minute candles");
        tabs(*workspace)->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!flow.active(), 1500);
        tabs(*workspace)->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(flow.active(), 1500);
        workspace->findChild<QAction*>("chartSidePanelAction")->setChecked(false);
        QTRY_VERIFY_WITH_TIMEOUT(!flow.active(), 1500);
        workspace->findChild<QAction*>("chartSidePanelAction")->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(flow.active(), 1500);
        pages->setCurrentIndex(pages->indexOf(workspace) == 0 ? 1 : 0);
        QTRY_VERIFY_WITH_TIMEOUT(!flow.active(), 1500);
        pages->setCurrentWidget(workspace);
        QTRY_VERIFY_WITH_TIMEOUT(flow.active(), 1500);
        window.reset();
        QVERIFY(!flow.active());
    }

    void incomingTradesLeaveFocusWhereItWas() {
        LiveFlow live(QDateTime::currentMSecsSinceEpoch());
        QVERIFY(live.open());
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        auto* field = new QLineEdit(&host);
        field->setObjectName("syntheticOtherField");
        layout->addWidget(field);
        auto* workspace = new ChartWorkspace(nullptr, nullptr, &host);
        workspace->indicatorPreferences()->markIntroSeen();
        layout->addWidget(workspace, 1);
        workspace->setCoinFlowSource(live.flow.get());
        QVERIFY(workspace->setIdentity("coin:upbit:KRW-BTC"));
        host.resize(1440, 960);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        host.activateWindow();
        field->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), field);
        // A mouse press chooses the tab; the tab bar itself takes no keyboard
        // focus (no focus ring after a click), as any non-input spot here.
        QTest::mouseClick(tabs(*workspace), Qt::LeftButton, {}, tabs(*workspace)->tabRect(0).center());
        QCOMPARE(tabs(*workspace)->currentIndex(), 0);
        QVERIFY(QApplication::focusWidget() != tabs(*workspace));
        field->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), field);
        auto* view = tape(*workspace);
        QVERIFY(view && view->isVisible());
        QCOMPARE(view->focusPolicy(), Qt::NoFocus);
        QCOMPARE(tabs(*workspace)->focusPolicy(), Qt::TabFocus);
        QVERIFY2(view->findChildren<QWidget*>().isEmpty(), "One painter: no child rows or item view");
        QVERIFY(view->findChildren<QAbstractItemView*>().isEmpty());
        for (int i = 0; i < 40; ++i) {
            live.trade(live.now - 1000 + i, "95000000", "0.01", i % 2, 9000000000000LL + i);
            if (i % 8 == 0) QTest::qWait(20);
            QCOMPARE(QApplication::focusWidget(), field);
        }
        QTRY_COMPARE(view->rows().size(), 40);
        QCOMPARE(QApplication::focusWidget(), field);
        QCOMPARE(field->cursorPosition(), field->text().size());
        QTest::keyClicks(field, "btc");
        QCOMPARE(field->text(), QString("btc"));
    }

    void tapeNamesTheVenueAndKeepsTheTradeCurrency() {
        constexpr qint64 at = 1780000020000;
        CoinTapeView view;
        view.resize(340, 400);
        view.show();
        view.setIdentity("coin:upbit:KRW-BTC");
        CoinFlowSnapshot upbit;
        upbit.identity = "coin:upbit:KRW-BTC";
        upbit.primary.instrument = upbitInstrument("KRW-BTC", at);
        CoinTradeObservation trade;
        trade.id = "1"; trade.price = "95000000"; trade.quantity = "0.0123"; trade.side = CoinFlowSide::Buy;
        trade.tradeAtMs = at - 1000; trade.eventAtMs = trade.tradeAtMs; trade.receivedAtMs = trade.knownAtMs = at - 990; trade.epoch = 1;
        CoinTradeObservation ask = trade;
        ask.id = "2"; ask.price = "94999000"; ask.quantity = "0.00000001"; ask.side = CoinFlowSide::Sell; ask.tradeAtMs = at - 500;
        upbit.primary.trades = {trade, ask};
        view.showSnapshot(upbit);
        QCOMPARE(view.rows().size(), 2);
        QCOMPARE(view.rows()[0].key, QString("t:2"));
        QCOMPARE(view.rows()[0].venue, QStringLiteral("업비트"));
        QCOMPARE(view.rows()[0].side, MovementDirection::Down);
        QCOMPARE(view.rows()[0].amount, QStringLiteral("1원"));
        QCOMPARE(view.rows()[1].amount, QStringLiteral("1,168,500원"));
        QCOMPARE(view.rows()[1].price, QStringLiteral("95,000,000"));
        QCOMPARE(view.rows()[1].side, MovementDirection::Up);

        view.setIdentity("coin:binance:BTCUSDT");
        view.showSnapshot(binanceSnapshot(at));
        const auto rows = view.rows();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].venue, QStringLiteral("바이낸스"));
        QCOMPARE(rows[0].amount, QStringLiteral("4.5 USDT"));
        QCOMPARE(rows[0].side, MovementDirection::Down);
        QVERIFY(rows[1].liquidation);
        QCOMPARE(rows[1].venue, QStringLiteral("바이낸스 .P"));
        QCOMPARE(rows[1].amount, QStringLiteral("128,770 USDT"));
        QCOMPARE(rows[1].price, QStringLiteral("64,385.1"));
        QCOMPARE(rows[1].side, MovementDirection::Down);
        QCOMPARE(rows[2].venue, QStringLiteral("바이낸스"));
        QCOMPARE(rows[2].amount, QStringLiteral("32,193 USDT"));
        for (const auto& row : rows) {
            QVERIFY2(row.key != "t:7777", "Companion futures trades never enter the tape");
            QVERIFY(!row.venue.contains(QStringLiteral("현물")));
            QVERIFY(!row.amount.contains(QStringLiteral("원")));
        }
        // A snapshot of another market shows nothing.
        auto other = binanceSnapshot(at);
        other.identity = "coin:binance:ETHUSDT";
        view.showSnapshot(other);
        QVERIFY(view.rows().isEmpty());
        view.setIdentity("stock:KR:005930");
        view.showSnapshot(binanceSnapshot(at));
        QVERIFY(view.rows().isEmpty());
        QCOMPARE(view.emptyText(), QStringLiteral("코인 차트에서 보여요"));
        QCOMPARE(CoinTapeView::venueName(CoinFlowVenue::UpbitSpot), QStringLiteral("업비트"));
        QCOMPARE(CoinTapeView::venueName(CoinFlowVenue::BinanceSpot), QStringLiteral("바이낸스"));
        QCOMPARE(CoinTapeView::venueName(CoinFlowVenue::BinanceUsdM), QStringLiteral("바이낸스 .P"));

        // Small positive notionals keep quote-appropriate precision and never display as unmarked zero.
        QCOMPARE(CoinTapeView::amountText("0.000123", "BTC"), QStringLiteral("0.000123 BTC"));
        QCOMPARE(CoinTapeView::amountText("0.0049", "BTC"), QStringLiteral("0.0049 BTC"));
        QCOMPARE(CoinTapeView::amountText("0.0000005", "BTC"), QStringLiteral("0.0000005 BTC"));
        QCOMPARE(CoinTapeView::amountText("0.000000001", "BTC"), QStringLiteral("<0.00000001 BTC"));
        QCOMPARE(CoinTapeView::amountText("0.001", "USDT"), QStringLiteral("0.001 USDT"));
        QCOMPARE(CoinTapeView::amountText("0.00001", "USDT"), QStringLiteral("<0.0001 USDT"));
        QCOMPARE(CoinTapeView::amountText("0.1", "KRW"), QStringLiteral("<1원"));
        QCOMPARE(CoinTapeView::amountText("0", "BTC"), QStringLiteral("0 BTC"));

        // End-to-end BTC-quoted market snapshot preserves sub-cent BTC notional.
        view.setIdentity("coin:upbit:BTC-ETH");
        CoinFlowSnapshot btcMarket;
        btcMarket.identity = "coin:upbit:BTC-ETH";
        btcMarket.primary.instrument = upbitInstrument("BTC-ETH", at);
        btcMarket.primary.instrument.identity = btcMarket.identity;
        btcMarket.primary.instrument.baseAsset = "ETH";
        btcMarket.primary.instrument.quoteAsset = "BTC";
        CoinTradeObservation tinyBtc = trade;
        tinyBtc.id = "301"; tinyBtc.price = "0.0123"; tinyBtc.quantity = "0.01"; // notional = 0.000123 BTC
        btcMarket.primary.trades = {tinyBtc};
        view.showSnapshot(btcMarket);
        QCOMPARE(view.rows().size(), 1);
        QCOMPARE(view.rows().first().amount, QStringLiteral("0.000123 BTC"));
    }

    void largeTradeMarksAreTheWhaleStudyCandidates() {
        // Start well in the past so the tape's own clock is later than every fact.
        const qint64 base = (QDateTime::currentMSecsSinceEpoch() - 20 * 60000) / 60000 * 60000;
        LiveFlow live(base + 5000);
        QVERIFY(live.open());
        CoinTapeView view;
        view.resize(340, 600);
        view.show();
        view.setSource(live.flow.get());
        view.setIdentity("coin:upbit:KRW-BTC");
        const qint64 first = base + 60000; // First whole observed minute after the seed.
        live.now = first + 40000;
        for (int i = 0; i < 200; ++i) live.trade(first + i * 100, "100", "1", true, 1000 + i);
        live.trade(first + 20100, "100", "50", false, 1200); // 5,000 against two hundred 100s.
        live.trade(first + 20200, "100", "1", true, 1201);   // An ordinary trade after it.
        live.now = first + 60000 + 1000;                     // The minute closes.
        live.trade(first + 60500, "100", "1", true, 1202);
        live.flow->pump();
        const auto snapshot = live.flow->snapshot();
        QVERIFY(!snapshot.primary.periods.isEmpty());
        const auto study = coinFlowStudies(snapshot, live.now);
        QSet<QString> expected;
        for (const auto& point : study.largeTrade.candidates) expected.insert(point.id);
        QCOMPARE(expected, QSet<QString>{"1200"});
        QSet<QString> pure;
        for (const auto& point : coinLargeTrades(snapshot.primary, live.now)) pure.insert(point.id);
        QCOMPARE(pure, expected);
        QTRY_COMPARE(view.rows().size(), CoinTapeView::MaxRows);
        QSet<QString> marked;
        for (const auto& row : view.rows())
            if (row.large) marked.insert(row.key.mid(2));
        QCOMPARE(marked, expected);
    }

    void newRowsFlashBrieflyAndOnlyThen() {
        constexpr qint64 at = 1780000020000;
        CoinTapeView view;
        view.resize(340, 400);
        view.show();
        view.setIdentity("coin:binance:BTCUSDT");
        auto snapshot = binanceSnapshot(at);
        view.showSnapshot(snapshot);
        QVERIFY2(!view.flashing(), "The first fill shows rows without flashing all of them");
        CoinTradeObservation next = snapshot.primary.trades.last();
        next.id = "9003"; next.tradeAtMs = at - 100;
        snapshot.primary.trades.append(next);
        QElapsedTimer clock;
        clock.start();
        view.showSnapshot(snapshot);
        QCOMPARE(view.rows().first().key, QString("t:9003"));
        if (motionEnabled()) {
            QVERIFY(view.flashing());
            QCOMPARE(view.flashingKeys(), QStringList{"t:9003"});
            QTRY_VERIFY_WITH_TIMEOUT(!view.flashing(), 1000);
            QVERIFY2(clock.elapsed() < CoinTapeView::FlashMs + 300, qPrintable(QString::number(clock.elapsed())));
        } else {
            QVERIFY(!view.flashing());
            QVERIFY(view.flashingKeys().isEmpty());
        }
        view.showSnapshot(snapshot);
        QVERIFY2(!view.flashing(), "An unchanged snapshot starts no frame timer");
    }

    // Rows that arrive while another tab is in front were never seen arriving:
    // coming back shows them in the first frame, with no flash and no stale list.
    void returningToTheTapeShowsHeldRowsWithoutFlashing() {
        LiveFlow live(QDateTime::currentMSecsSinceEpoch());
        QVERIFY(live.open());
        QWidget host;
        auto* workspace = workspaceAt(host, 1440);
        workspace->setCoinFlowSource(live.flow.get());
        QVERIFY(workspace->setIdentity("coin:upbit:KRW-BTC"));
        settled(*workspace);
        tabs(*workspace)->setCurrentIndex(0);
        auto* view = tape(*workspace);
        QVERIFY(view && view->isVisible());
        for (int i = 0; i < 5; ++i) live.trade(live.now - 2000 + i, "95000000", "0.01", true, 9100000000000LL + i);
        QTRY_COMPARE(view->rows().size(), 5);
        QVERIFY2(!view->flashing(), "The first fill shows rows without flashing");
        // The source keeps collecting (as with Whales on) while the evidence tab is in front.
        tabs(*workspace)->setCurrentIndex(1);
        QVERIFY(!view->isVisible());
        for (int i = 5; i < 25; ++i) live.trade(live.now - 2000 + i, "95000000", "0.01", i % 2, 9100000000000LL + i);
        tabs(*workspace)->setCurrentIndex(0);
        QCOMPARE(view->rows().size(), 25);
        QVERIFY(!view->flashing());
        QVERIFY(view->flashingKeys().isEmpty());
        QTest::qWait(200);
        QVERIFY2(!view->flashing(), "No delayed burst for the held rows either");
        // A trade that arrives while the tape is on screen still flashes.
        live.trade(live.now - 1000, "95010000", "0.02", true, 9100000000100LL);
        QTRY_COMPARE(view->rows().size(), 26);
        if (motionEnabled()) QCOMPARE(view->flashingKeys(), QStringList{QStringLiteral("t:9100000000100")});
        else QVERIFY(!view->flashing());
    }

    void evidenceTabIsTheMovedCardWithItsNames() {
        QWidget host;
        auto* workspace = workspaceAt(host, 1440);
        settled(*workspace);
        auto* card = workspace->findChild<QWidget*>("chartScorePanel");
        auto* stack = workspace->findChild<QStackedWidget*>("chartSidePanelStack");
        QVERIFY(card && stack);
        QVERIFY(stack->isAncestorOf(card));
        QCOMPARE(workspace->findChildren<QLabel*>("chartIndicatorSummary").size(), 1);
        QCOMPARE(workspace->findChildren<InfoButton*>("chartIndicatorSummaryInfo").size(), 1);
        auto* summary = workspace->findChild<QLabel*>("chartIndicatorSummary");
        auto* info = workspace->findChild<InfoButton*>("chartIndicatorSummaryInfo");
        QVERIFY(card->isAncestorOf(summary) && card->isAncestorOf(info));
        QVERIFY(summary->isVisible() && info->isVisible());
        QVERIFY(!workspace->findChild<QWidget*>("chartWorkspacePositions")->isAncestorOf(card));
        QCOMPARE(tabs(*workspace)->tabText(0), QStringLiteral("체결·청산"));
        QCOMPARE(tabs(*workspace)->tabText(1), QStringLiteral("근거"));
        for (const auto* forbidden : {"공격 매수", "매수 우위", "반등 확인"})
            for (auto* label : workspace->findChildren<QLabel*>())
                QVERIFY(!label->text().contains(QString::fromUtf8(forbidden)));
    }
    void captures_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("dark");
        for (const bool dark : {false, true})
            for (const int width : {960, 1280, 1440})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(dark ? "dark" : "light").arg(width))) << width << dark;
    }
    void captures() {
        QTemporaryDir fallbackDir;
        QVERIFY(fallbackDir.isValid());
        const auto envOut = qEnvironmentVariable("KEEFI_STEP267_RENDER_DIR");
        const auto out = envOut.isEmpty() ? fallbackDir.path() : envOut;
        QFETCH(int, width);
        QFETCH(bool, dark);
        QVERIFY(QDir().mkpath(out));
        auto* theme = KeefiTheme::instance();
        const auto previous = theme->mode();
        const auto restore = qScopeGuard([&] { theme->setMode(previous); });
        theme->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        const qint64 base = (QDateTime::currentMSecsSinceEpoch() - 3 * 60000) / 60000 * 60000;
        LiveFlow live(base + 5000);
        QVERIFY(live.open());
        CandleHttp http;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        MarketCandles candles(&http, std::unique_ptr<PublicCandleSocket>{}, [now] { return now; });
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-captures.ini"), QSettings::IniFormat);
        if (width != 1440) settings.setValue(ChartTimeframePins::SettingsKey, QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[300000,900000,3600000,86400000]})"));
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* workspace = new ChartWorkspace(&candles, nullptr, &host);
        workspace->setStyleSheet(KeefiTheme::localStyleSheet(workspace));
        workspace->indicatorPreferences()->markIntroSeen();
        workspace->setChartSettings(&settings);
        workspace->setCoinFlowSource(live.flow.get());
        layout->addWidget(workspace);
        QVERIFY(workspace->setIdentity("coin:upbit:KRW-BTC"));
        workspace->setTimeframe(60000);
        host.resize(width, 900);
        host.show();
        QTRY_VERIFY(http.token);
        emit http.finished(http.token, syntheticCandles(now));
        QTRY_VERIFY(candles.series().available);
        // Two hundred ordinary trades in one closed minute, one far larger, then the live minute.
        const qint64 first = base + 60000;
        live.now = first + 59000;
        quint32 seed = 7;
        for (int i = 0; i < 230; ++i) {
            seed = seed * 1103515245u + 12345u;
            const bool large = i == 226;
            const auto volume = large ? QStringLiteral("2.75") : QString::number(0.0005 + (seed >> 16) % 400 / 10000.0, 'f', 4);
            live.trade(first + i * 250, QString::number(95000000 + qint64((seed >> 8) % 41) * 5000 - 100000), volume, (seed >> 3) % 2, 5000 + i);
        }
        live.now = first + 60000 + 4000;
        for (int i = 0; i < 6; ++i) live.trade(first + 60000 + i * 500, QString::number(95010000 + i * 5000), "0.0123", i % 3 != 0, 6000 + i);
        live.flow->pump();
        tabs(*workspace)->setCurrentIndex(0);
        if (width < ChartWorkspace::SidePanelCollapseWidth) workspace->findChild<QPushButton*>("chartDetailsToggle")->click();
        QTRY_VERIFY(tape(*workspace)->rows().size() > 10);
        QTest::qWait(900);
        const auto name = QStringLiteral("%1/step267-%2-%3").arg(out, dark ? "dark" : "light").arg(width);
        QVERIFY(host.grab().save(name + "-tape.png"));
        auto* scrollArea = scroll(*workspace);
        if (width < ChartWorkspace::SidePanelCollapseWidth) {
            scrollArea->ensureWidgetVisible(sidePanel(*workspace), 0, 0);
            QCoreApplication::processEvents();
            QVERIFY(host.grab().save(name + "-tape-scrolled.png"));
            scrollArea->verticalScrollBar()->setValue(0);
            workspace->findChild<QPushButton*>("chartDetailsToggle")->click();
        }
        tabs(*workspace)->setCurrentIndex(1);
        QTest::qWait(1100);
        QVERIFY(host.grab().save(name + "-evidence.png"));
        auto* list = workspace->findChild<QToolButton*>("chartWorkspaceTimeframeList");
        auto* action = workspace->findChild<QAction*>("chartTimeframeFavorites");
        if (list && list->isVisible()) list->click();
        else action->trigger();
        QTRY_VERIFY(workspace->findChild<QMenu*>("chartTimeframeListMenu"));
        auto* menu = workspace->findChild<QMenu*>("chartTimeframeListMenu");
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(menu->grab().save(name + "-timeframe-list.png"));
        auto* toolbar = workspace->findChild<QWidget*>("chartToolbar");
        QVERIFY(toolbar->grab().save(name + "-toolbar.png"));
        qInfo() << "capture" << name << "toolbar" << toolbar->size() << "rows" << toolbar->property("rows") << "beside" << workspace->sidePanelBesideChart()
                << "panel" << sidePanel(*workspace)->size() << "chart" << chartColumn(*workspace)->size();
        menu->close();
    }
};

QTEST_MAIN(ChartSidePanelTest)
#include "tst_chart_side_panel.moc"
