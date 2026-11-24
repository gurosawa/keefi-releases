#include "keefi/MarketCandlesView.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/KeefiFonts.h"
#include "keefi/TradingTransport.h"
#include <QDataStream>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimeZone>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000;

class PublicHttp final : public TradingTransport {
  public:
    quint64 token = 0;
    int sends = 0;
    void send(quint64 value, const RequestSpec& request) override {
        if (request.authenticated || !MarketCandles::isRequest(request))
            qFatal("Live chart test attempted a non-public candle request");
        token = value;
        ++sends;
    }
    void abort() override {}
};

class PublicSocket final : public PublicCandleSocket {
  public:
    quint64 generation = 0;
    void open(quint64 value, const QUrl&) override { generation = value; }
    void send(quint64, const QByteArray&) override {}
    void abort() override {}
};

MarketInstrument instrument(const QString& symbol = "SYNUSDT", const QString& asset = "SYN") {
    MarketInstrument value;
    value.symbol = symbol;
    value.baseAsset = asset;
    value.quoteAsset = "USDT";
    value.market = "spot";
    value.status = "TRADING";
    value.tradable = true;
    return value;
}

QByteArray evidence(const MarketCandleSeries& value) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << value.available << value.asset << value.sourceUrl << value.reason
        << value.receivedAtMs << value.identity << value.currency << value.intervalMs
        << value.sourceTimeMs << value.staleClose << value.delayed
        << value.restThroughMs << value.restReceivedAtMs << value.fromArchive
        << value.fromMemoryCache << value.restRevision << qint64(value.candles.size());
    const auto row = [&](const MarketCandle& candle) {
        out << candle.startsAtMs << candle.lastStoredTickMs << candle.open << candle.high
            << candle.low << candle.close << candle.volume;
    };
    for (const auto& candle : value.candles) row(candle);
    out << value.forming.has_value();
    if (value.forming) row(*value.forming);
    return bytes;
}

struct Fixture {
    bool binance;
    qint64 clock = QDateTime::currentMSecsSinceEpoch();
    qint64 boundary = clock / Minute * Minute;
    QString identity;
    PublicHttp http;
    PublicSocket* socket = nullptr;
    std::unique_ptr<MarketCandles> source;
    ChartDrawings drawings;
    ChartStudyPreferences preferences;
    MarketCandlesView view;

    explicit Fixture(bool useBinance, bool seedNow = true)
        : binance(useBinance),
          identity(binance ? "coin:binance:SYNUSDT" : "coin:upbit:KRW-BTC"),
          source(std::make_unique<MarketCandles>(&http, makeSocket(), [this] { return clock; })),
          view(source.get()) {
        if (binance) source->setInstruments("binance", {instrument(), instrument("ALTUSDT", "ALT")});
        else source->setVerifiedUpbitMarkets({"KRW-BTC", "KRW-ETH"}, true);
        drawings.setVerifiedInstruments({WatchlistWorkspaceList::identityFromKey(identity)});
        preferences.markIntroSeen();
        preferences.setEnabled("similarity", false);
        view.setIndicatorPreferences(&preferences);
        view.setDrawingModel(&drawings);
        view.setDrawingPublicContext(true);
        view.resize(1280, 1000);
        view.show();
        view.setSelection(identity, Minute);
        emit socket->connected(socket->generation);
        if (seedNow) seed();
    }

    std::unique_ptr<PublicCandleSocket> makeSocket() {
        auto value = std::make_unique<PublicSocket>();
        socket = value.get();
        return value;
    }

    QWidget* canvas() const { return view.findChild<QWidget*>("marketCandlesChart"); }

    void seed() {
        QJsonArray rows;
        for (int i = 200; i > 0; --i) {
            const auto at = boundary - i * Minute;
            const auto price = QString::number(100 + i % 20);
            if (binance) {
                rows.append(QJsonArray{QString::number(at), price,
                    QString::number(103 + i % 20), QString::number(97 + i % 20),
                    price, "2", QString::number(at + Minute - 1), "200", "2", "1", "100", "0"});
            } else {
                rows.append(QJsonObject{{"market", "KRW-BTC"},
                    {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, QTimeZone::utc()).toString("yyyy-MM-ddTHH:mm:ss")},
                    {"timestamp", QString::number(at + 1000)}, {"unit", "1"},
                    {"opening_price", price}, {"high_price", QString::number(103 + i % 20)},
                    {"low_price", QString::number(97 + i % 20)}, {"trade_price", price},
                    {"candle_acc_trade_volume", "2"}});
            }
        }
        HttpResult result;
        result.status = 200;
        result.body = QJsonDocument(rows).toJson(QJsonDocument::Compact);
        emit http.finished(http.token, result);
        anchor();
    }

    void anchor() {
        QJsonObject frame;
        if (binance) {
            const QJsonObject candle{{"t", QString::number(boundary)},
                {"T", QString::number(boundary + Minute - 1)}, {"s", "SYNUSDT"}, {"i", "1m"},
                {"o", "100"}, {"h", "110"}, {"l", "90"}, {"c", "104"},
                {"v", "7"}, {"n", "5"}, {"x", false}, {"q", "210"}, {"V", "1"},
                {"Q", "105"}, {"B", "0"}, {"L", "100"}};
            frame = {{"e", "kline"}, {"E", QString::number(clock)}, {"s", "SYNUSDT"}, {"k", candle}};
        } else {
            frame = {{"type", "candle.1m"}, {"code", "KRW-BTC"}, {"stream_type", "REALTIME"},
                {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(boundary, QTimeZone::utc()).toString("yyyy-MM-ddTHH:mm:ss")},
                {"timestamp", QString::number(clock)}, {"opening_price", "100"},
                {"high_price", "110"}, {"low_price", "90"}, {"trade_price", "104"},
                {"candle_acc_trade_volume", "7"}};
        }
        emit socket->received(socket->generation, QJsonDocument(frame).toJson(QJsonDocument::Compact));
    }

    void trade(int number, const QString& price, quint64 generation = 0) {
        ++clock;
        QJsonObject frame;
        if (binance) {
            frame = {{"e", "trade"}, {"s", "SYNUSDT"}, {"t", QString::number(number)},
                {"T", QString::number(clock)}, {"E", QString::number(clock)}, {"p", price}, {"q", "0.2"}};
        } else {
            frame = {{"type", "trade"}, {"code", "KRW-BTC"}, {"stream_type", "REALTIME"},
                {"sequential_id", QString::number(number)}, {"trade_timestamp", QString::number(clock)},
                {"timestamp", QString::number(clock)}, {"trade_price", price}, {"trade_volume", "0.2"}};
        }
        emit socket->received(generation ? generation : socket->generation,
                              QJsonDocument(frame).toJson(QJsonDocument::Compact));
    }

    QString addHorizontalDrawing() {
        const auto series = source->series();
        ChartDrawingFrame frame;
        frame.identity = WatchlistWorkspaceList::identityFromKey(identity);
        frame.currency = binance ? "USDT" : "KRW";
        frame.intervalMs = Minute;
        frame.asOfMs = clock;
        frame.contextGeneration = drawings.contextGeneration();
        for (const auto& row : series.candles)
            frame.bars.append({row, row.startsAtMs + Minute, true, true});
        return drawings.create(ChartDrawingKind::Horizontal,
                               {{series.candles.last().startsAtMs, "105"}}, frame);
    }
};

void providers() {
    QTest::addColumn<bool>("binance");
    QTest::newRow("upbit") << false;
    QTest::newRow("binance") << true;
}
}

class ChartLiveViewTest final : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir settings_;
  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("KEEFI-Synthetic-Chart-Live-View");
        QCoreApplication::setApplicationName("KEEFI Synthetic Chart Live View");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.filePath("settings"));
        KeefiFonts::registerBundledFonts();
    }

    void tradeUpdatesDisplayWithoutChangingClosedEvidence_data() { providers(); }
    void tradeUpdatesDisplayWithoutChangingClosedEvidence() {
        QFETCH(bool, binance);
        Fixture f(binance);
        QVERIFY(f.canvas());
        QVERIFY(f.source->series().available);
        QTRY_VERIFY_WITH_TIMEOUT(f.view.chartStudies().available, 3000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QTest::qWait(40);
        QVERIFY(!f.addHorizontalDrawing().isEmpty());
        QTest::qWait(40);
        const auto official = evidence(f.source->series());
        const auto rest = evidence(f.source->restObservation());
        const auto revision = f.view.indicatorRevision();
        const auto calculations = f.view.property("displayStudyCalculationCount").toInt();
        const auto drawingState = f.drawings.publicState();
        const auto drawingGeneration = f.drawings.contextGeneration();
        const int requests = f.http.sends;
        QSignalSpy officialChanges(f.source.get(), &MarketCandles::changed);
        QSignalSpy studyChanges(&f.view, &MarketCandlesView::studiesChanged);
        QSignalSpy drawingChanges(&f.drawings, &ChartDrawings::changed);
        QSignalSpy lineChanges(&f.drawings, &ChartDrawings::horizontalLinesChanged);
        // Several trades arrive before the paced notification; the final price must win.
        for (int i = 0; i < 8; ++i) f.trade(101 + i, QString::number(111 + i));
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("118"), 1000);
        QCOMPARE(f.canvas()->property("liveTradeAtMs").toLongLong(), f.clock);
        QCOMPARE(evidence(f.source->series()), official);
        QCOMPARE(evidence(f.source->restObservation()), rest);
        QCOMPARE(f.canvas()->property("displayCompletedCount").toInt(), 200);
        QCOMPARE(f.view.indicatorRevision(), revision);
        QCOMPARE(f.view.property("displayStudyCalculationCount").toInt(), calculations);
        QCOMPARE(f.drawings.publicState(), drawingState);
        QCOMPARE(f.drawings.contextGeneration(), drawingGeneration);
        QCOMPARE(officialChanges.size(), 0);
        QCOMPARE(studyChanges.size(), 0);
        QCOMPARE(drawingChanges.size(), 0);
        QCOMPARE(lineChanges.size(), 0);
        QCOMPARE(f.http.sends, requests);
    }

    void selectionAndTimeframeChangesClearPreviousTrade_data() {
        QTest::addColumn<bool>("binance");
        QTest::addColumn<bool>("changeTimeframe");
        QTest::newRow("upbit-symbol") << false << false;
        QTest::newRow("upbit-timeframe") << false << true;
        QTest::newRow("binance-symbol") << true << false;
        QTest::newRow("binance-timeframe") << true << true;
    }
    void selectionAndTimeframeChangesClearPreviousTrade() {
        QFETCH(bool, binance);
        QFETCH(bool, changeTimeframe);
        Fixture f(binance);
        f.trade(101, "111");
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("111"), 1000);
        const auto oldGeneration = f.socket->generation;
        f.view.setSelection(changeTimeframe ? f.identity :
            binance ? "coin:binance:ALTUSDT" : "coin:upbit:KRW-ETH",
            changeTimeframe ? 5 * Minute : Minute);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
        QCOMPARE(f.canvas()->property("liveTradeAtMs").toLongLong(), qint64(0));
        f.trade(102, "999", oldGeneration);
        QTest::qWait(60);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
    }

    void confirmedMetadataRevocationRemovesTrade_data() { providers(); }
    void confirmedMetadataRevocationRemovesTrade() {
        QFETCH(bool, binance);
        Fixture f(binance);
        f.trade(101, "111");
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("111"), 1000);
        const auto oldGeneration = f.socket->generation;
        if (binance) f.source->setInstruments("binance", {instrument("ALTUSDT", "ALT")});
        else f.source->setVerifiedUpbitMarkets({"KRW-ETH"}, true);
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString{}, 1000);
        f.trade(102, "999", oldGeneration);
        QTest::qWait(60);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
        QVERIFY(!f.source->liveTradeObservation().available);
    }

    void hiddenViewCannotRestoreAnOldStreamTrade_data() { providers(); }
    void hiddenViewCannotRestoreAnOldStreamTrade() {
        QFETCH(bool, binance);
        Fixture f(binance);
        f.trade(101, "111");
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("111"), 1000);
        const auto oldGeneration = f.socket->generation;
        f.view.hide();
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
        f.trade(102, "999", oldGeneration);
        QTest::qWait(60);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
        f.view.show();
        QTest::qWait(60);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
        // A fresh generation still needs its own official seed.
        emit f.socket->connected(f.socket->generation);
        f.trade(103, "998");
        QTest::qWait(60);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
    }

    void tradeBeforeOfficialSeedDoesNotCreateAPrice_data() { providers(); }
    void tradeBeforeOfficialSeedDoesNotCreateAPrice() {
        QFETCH(bool, binance);
        Fixture f(binance, false);
        f.trade(101, "111");
        QTest::qWait(60);
        QVERIFY(!f.source->liveTradeObservation().available);
        QCOMPARE(f.canvas()->property("liveTradePrice").toString(), QString{});
        QVERIFY(!f.source->series().available);
        f.seed();
        f.trade(102, "112");
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("112"), 1000);
    }

    void expiredOfficialEvidenceRemovesLivePrice_data() { providers(); }
    void expiredOfficialEvidenceRemovesLivePrice() {
        QFETCH(bool, binance);
        Fixture f(binance);
        f.trade(101, "111");
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("111"), 1000);
        f.clock += MarketCandles::FreshnessMs + 1;
        QVERIFY(!f.source->liveTradeObservation().available);
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString{}, 2000);
    }

    void destroyedSourceClearsLivePrice_data() { providers(); }
    void destroyedSourceClearsLivePrice() {
        QFETCH(bool, binance);
        Fixture f(binance);
        f.trade(101, "111");
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString("111"), 1000);
        f.source.reset();
        QTRY_COMPARE_WITH_TIMEOUT(f.canvas()->property("liveTradePrice").toString(), QString{}, 1000);
        QCOMPARE(f.canvas()->property("liveTradeAtMs").toLongLong(), qint64(0));
    }
};

QTEST_MAIN(ChartLiveViewTest)
#include "tst_chart_live_view.moc"
