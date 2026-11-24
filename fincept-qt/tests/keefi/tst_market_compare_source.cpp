#include "keefi/CollectorStore.h"
#include "keefi/FxHistory.h"
#include "keefi/MarketCompareSource.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest>
#include <cmath>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000, Hour = 3600000;
const qint64 T0 = QDateTime(QDate(2026, 11, 6), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
constexpr int Bars = 40;
constexpr qint64 Interval = 15 * Minute;

class Wire final : public TradingTransport {
  public:
    QList<QPair<quint64, RequestSpec>> sent;
    void send(quint64 token, const RequestSpec& request) override { sent.append({token, request}); }
    void abort() override {}
    void answer(int index, const QByteArray& body, int status = 200) {
        HttpResult result;
        result.status = status;
        result.body = body;
        emit finished(sent[index].first, result);
    }
};

QByteArray binanceBars(double price, qint64 start = T0, qint64 interval = Interval, int count = Bars) {
    QStringList rows;
    for (int i = 0; i < count; ++i) {
        const auto open = start + i * interval;
        rows.append(QStringLiteral("[%1,\"%2\",\"%2\",\"%2\",\"%2\",\"1\",%3,\"1\",1,\"1\",\"1\",\"0\"]")
                        .arg(open).arg(QString::number(price + i, 'f', 2)).arg(open + interval - 1));
    }
    return '[' + rows.join(',').toUtf8() + ']';
}

QByteArray upbitBars(qint64 start = T0, qint64 interval = Interval, int count = Bars) {
    QStringList rows;
    for (int i = count - 1; i >= 0; --i) { // Upbit lists newest first.
        const auto open = start + i * interval;
        const auto utc = QDateTime::fromMSecsSinceEpoch(open, QTimeZone::UTC).toString("yyyy-MM-ddTHH:mm:ss");
        rows.append(QStringLiteral("{\"market\":\"KRW-BTC\",\"candle_date_time_utc\":\"%1\",\"candle_date_time_kst\":\"%1\","
                                   "\"opening_price\":135000000,\"high_price\":135000000,\"low_price\":135000000,\"trade_price\":135000000,"
                                   "\"timestamp\":%2,\"candle_acc_trade_price\":1,\"candle_acc_trade_volume\":1,\"unit\":%3}")
                        .arg(utc).arg(open + 1000).arg(interval / Minute));
    }
    return '[' + rows.join(',').toUtf8() + ']';
}

QByteArray line(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QByteArray catalog() {
    return R"({"symbols":[{"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},{"symbol":"CLUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},{"symbol":"QQQUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"EQUITY"}]})";
}
} // namespace

class MarketCompareSourceTests final : public QObject {
    Q_OBJECT
  private slots:
    void tabReturnResumesPendingMarketsAndKeepsOriginalRefreshDeadline() {
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 now = T0 + (Bars + 1) * Interval;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setInterval(Interval);
        source.setActive(true);
        QTRY_COMPARE(wire.sent.size(), 1);
        wire.answer(0, catalog());
        QTRY_COMPARE(wire.sent.size(), 2);
        source.setActive(false);
        wire.answer(1, binanceBars(100000));
        QVERIFY(!source.snapshot().loading);

        source.setActive(true);
        for (int i = 2; i <= 4; ++i) {
            QTRY_COMPARE(wire.sent.size(), i + 1);
            wire.answer(i, binanceBars(100000));
        }
        for (const auto& series : source.snapshot().series) QVERIFY(series.available);
        source.setActive(false);
        source.setActive(true);
        QVERIFY(!source.snapshot().loading);
        QCOMPARE(wire.sent.size(), 5);

        source.setActive(false);
        now += MarketCompareSource::refreshPeriodMs(Interval);
        source.setActive(true);
        QTRY_COMPARE(wire.sent.size(), 6);
        QCOMPARE(QUrlQuery(wire.sent.last().second.url).queryItemValue("symbol"), QString("XAUUSDT"));
    }

    void tabReturnRetriesOnlyFailedCandlesUntilOriginalDeadline() {
        Wire wire;
        const auto start = T0 + (Bars + 1) * Interval;
        qint64 now = start;
        MarketCompareSource source(&wire, nullptr, [&] { return now; });
        source.setInterval(Interval);
        source.setActive(true);
        const auto answer = [&](int index, const QByteArray& body, int status = 200) {
            QCOMPARE(wire.sent.size(), index + 1);
            now += 250;
            wire.answer(index, body, status);
        };
        answer(0, catalog());
        for (int i = 1; i <= 4; ++i) answer(i, binanceBars(100));
        source.refresh();
        for (int i = 5; i <= 8; ++i) answer(i, binanceBars(200), i == 6 ? 503 : 200);
        const auto before = source.snapshot();
        QVERIFY(before.series[1].available); // The failed refresh retained its earlier bars.
        QVERIFY(!before.series[1].reason.isEmpty());
        const auto deadline = start + 5 * 250 + MarketCompareSource::refreshPeriodMs(Interval);
        now = deadline - 2000;
        source.setActive(false);
        source.setActive(true);
        QCOMPARE(wire.sent.size(), 10);
        QCOMPARE(wire.sent[9].second.url, wire.sent[6].second.url);
        answer(9, binanceBars(300));
        const auto fixed = source.snapshot();
        QVERIFY(!fixed.loading);
        for (int i = 0; i < 4; ++i) {
            QVERIFY(fixed.series[i].available);
            QVERIFY(fixed.series[i].reason.isEmpty());
            QCOMPARE(fixed.series[i].lastOkAtMs, i == 1 ? now : before.series[i].lastOkAtMs);
        }
        source.setActive(false);
        now = deadline;
        source.setActive(true);
        for (int i = 10; i <= 13; ++i) {
            QCOMPARE(wire.sent.size(), i + 1);
            QCOMPARE(wire.sent[i].second.url, wire.sent[i-9].second.url);
            answer(i, binanceBars(400));
        }
        QVERIFY(!source.snapshot().loading);
    }

    void tabReturnRetriesFxIndependently_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("failed-transport") << 503 << QByteArray{};
        QTest::newRow("no-currency-observations") << 200 << QByteArray("<Cube/>");
    }

    void tabReturnRetriesFxIndependently() {
        QFETCH(int, status);QFETCH(QByteArray, body);
        Wire wire;
        qint64 now = T0 + (Bars + 1) * Interval;
        MarketCompareSource source(&wire, nullptr, [&] { return now; });
        source.setInterval(Interval);
        source.setWonMode(CompareWonMode::WonToDollar);
        source.setActive(true);
        const auto answer = [&](int i, const QByteArray& data, int code = 200) {
            QCOMPARE(wire.sent.size(), i + 1);
            now += 250;
            wire.answer(i, data, code);
        };
        answer(0, catalog());
        for (int i = 1; i < 4; ++i) answer(i, binanceBars(1000));
        answer(4, upbitBars());
        QCOMPARE(wire.sent.size(), 6);
        QCOMPARE(wire.sent[5].second.url, FxHistory::ecbHistoryRequest().url);
        answer(5, body, status);
        const auto failed = source.snapshot();
        QVERIFY(!failed.loading);QVERIFY(!failed.fxReason.isEmpty());QVERIFY(failed.ecb.isEmpty());
        source.setActive(false);source.setActive(true);
        QCOMPARE(wire.sent.size(), 7);
        QCOMPARE(wire.sent.last().second.url, FxHistory::ecbHistoryRequest().url);
        answer(6, R"(<Cube time="2026-11-05"><Cube currency="USD" rate="1.1"/><Cube currency="KRW" rate="1500"/></Cube>)");
        const auto recovered = source.snapshot();
        QVERIFY(recovered.fxReason.isEmpty());QCOMPARE(recovered.ecb.size(), 1);
        QVERIFY(recovered.series.last().available);QCOMPARE(recovered.series.last().bars.size(), Bars);
        for (int i=0; i<4; ++i) QCOMPARE(recovered.series[i].lastOkAtMs, failed.series[i].lastOkAtMs);
        source.setActive(false);source.setActive(true);
        QVERIFY(!source.snapshot().loading);QCOMPARE(wire.sent.size(), 7);

        // A failed renewal may keep the old FX history, and must still retry.
        source.setActive(false);now += 13 * Hour;source.setActive(true);
        answer(7, catalog());
        for (int i=8; i<11; ++i) answer(i, binanceBars(1000));
        answer(11, upbitBars());
        QCOMPARE(wire.sent.size(), 13);
        QCOMPARE(wire.sent[12].second.url, FxHistory::ecbHistoryRequest().url);
        answer(12, body, status);
        QVERIFY(!source.snapshot().fxReason.isEmpty());QCOMPARE(source.snapshot().ecb.size(), 1);
        source.setActive(false);source.setActive(true);
        QCOMPARE(wire.sent.size(), 14);
        QCOMPARE(wire.sent[13].second.url, FxHistory::ecbHistoryRequest().url);
        answer(13, R"(<Cube time="2026-11-05"><Cube currency="USD" rate="1.1"/><Cube currency="KRW" rate="1450"/></Cube>)");
        QVERIFY(!source.snapshot().loading);QVERIFY(source.snapshot().fxReason.isEmpty());
    }

    void tabReturnRetriesCatalogAndOnlyItsBlockedFutures() {
        Wire wire;
        qint64 now = T0 + (Bars + 1) * Interval;
        MarketCompareSource source(&wire, nullptr, [&] { return now; });
        source.setInterval(Interval);
        source.setWonMode(CompareWonMode::WonRaw);
        source.setActive(true);
        QCOMPARE(wire.sent.size(), 1);
        now += 250;wire.answer(0, {}, 503);
        QCOMPARE(wire.sent.size(), 2);now += 250;wire.answer(1, upbitBars());
        const auto cached = source.snapshot().series.last().lastOkAtMs;
        QVERIFY(cached > 0);
        source.setActive(false);source.setActive(true);
        QCOMPARE(wire.sent.size(), 3);QCOMPARE(wire.sent.last().second.url, MarketCompare::perpCatalogRequest().url);
        now += 250;wire.answer(2, catalog());
        for (int i = 3; i <= 5; ++i) {
            QCOMPARE(wire.sent.size(), i + 1);
            QCOMPARE(wire.sent[i].second.url.host(), QStringLiteral("fapi.binance.com"));
            now += 250;wire.answer(i, binanceBars(4000));
        }
        const auto result = source.snapshot();
        QVERIFY(!result.loading);QVERIFY(result.catalogReason.isEmpty());
        for (const auto& series : result.series) QVERIFY(series.available);
        QCOMPARE(result.series.last().lastOkAtMs, cached);
        source.setActive(false);source.setActive(true);
        QCOMPARE(wire.sent.size(), 6);QVERIFY(!source.snapshot().loading);
    }

    void unavailableCatalogBlocksFuturesButKeepsBitcoin_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("http503") << 503 << QByteArray(R"({"msg":"unavailable"})");
        QTest::newRow("empty") << 200 << QByteArray(R"({"symbols":[]})");
        QTest::newRow("non-trading") << 200 << QByteArray(R"({"symbols":[{"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"BREAK","underlyingType":"COMMODITY"}]})");
    }

    void unavailableCatalogBlocksFuturesButKeepsBitcoin() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        MarketCompareSource source(&wire, nullptr, [&] { return T0 + (Bars + 1) * Interval + elapsed.elapsed(); });
        source.setInterval(Interval); // This fixture deliberately uses 15-minute bars.
        source.setActive(true);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, body, status);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 2; }, 2000));
        QCOMPARE(wire.sent[1].second.url.host(), QStringLiteral("api.binance.com"));
        wire.answer(1, binanceBars(100000));
        QTest::qWait(300);
        QCOMPARE(wire.sent.size(), 2);
        for (const auto& sent : wire.sent)
            QVERIFY(!(sent.second.url.host() == QLatin1String("fapi.binance.com") && sent.second.url.path().contains("klines")));
        const auto snapshot = source.snapshot();
        QCOMPARE(snapshot.series.size(), 4);
        QVERIFY(!snapshot.loading);
        QVERIFY(!snapshot.catalogReason.isEmpty());
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!snapshot.series[i].available);
            QVERIFY2(snapshot.series[i].reason.startsWith(QStringLiteral("선물 종목 목록을 확인하지 못해 보류")), qPrintable(snapshot.series[i].reason));
        }
        QVERIFY(snapshot.series.last().available);
        QCOMPARE(snapshot.series.last().bars.size(), Bars);
    }

    void expiredCatalogFailureDiscardsPreviouslyAllowedFutures_data() {
        unavailableCatalogBlocksFuturesButKeepsBitcoin_data();
    }

    void expiredCatalogFailureDiscardsPreviouslyAllowedFutures() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 now = T0 + (Bars + 1) * Interval;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setInterval(Interval);
        source.setActive(true);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            wire.answer(i, binanceBars(100000));
        }
        QVERIFY(source.snapshot().series.first().available);
        now += 7 * Hour;
        source.refresh();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 6; }, 2000));
        QCOMPARE(wire.sent[5].second.url.path(), QStringLiteral("/fapi/v1/exchangeInfo"));
        wire.answer(5, body, status);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 7; }, 2000));
        QCOMPARE(wire.sent[6].second.url.host(), QStringLiteral("api.binance.com"));
        wire.answer(6, binanceBars(100000));
        QTest::qWait(300);
        QCOMPARE(wire.sent.size(), 7);
        const auto snapshot = source.snapshot();
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!snapshot.series[i].available);
            QVERIFY2(snapshot.series[i].reason.startsWith(QStringLiteral("선물 종목 목록을 확인하지 못해 보류")), qPrintable(snapshot.series[i].reason));
        }
        QVERIFY(snapshot.series.last().available);
    }

    void failedRefreshRetainsBarsAndLastSuccessfulTime() {
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 now = T0 + (Bars + 1) * Interval;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setInterval(Interval);
        source.setActive(true);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            wire.answer(i, binanceBars(100000));
        }
        const auto before = source.snapshot();
        now += 5 * Minute;
        source.refresh();
        for (int i = 5; i <= 8; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            wire.answer(i, QByteArray(R"({"msg":"unavailable"})"), 503);
        }
        const auto after = source.snapshot();
        for (int i = 0; i < 4; ++i) {
            QVERIFY(after.series[i].available);
            QCOMPARE(after.series[i].lastOkAtMs, before.series[i].lastOkAtMs);
            QVERIFY(after.series[i].lastOkAtMs > 0);
            QVERIFY(after.series[i].reason.startsWith(QStringLiteral("갱신 실패:")));
            QCOMPARE(after.series[i].bars.size(), before.series[i].bars.size());
            for (int j = 0; j < before.series[i].bars.size(); ++j) {
                QCOMPARE(after.series[i].bars[j].openMs, before.series[i].bars[j].openMs);
                QCOMPARE(after.series[i].bars[j].close, before.series[i].bars[j].close);
            }
        }
    }

    void longestFourHourWindowKeepsOldHanaWhenClockAdvances_data() {
        QTest::addColumn<qint64>("advanceHours");
        QTest::addColumn<bool>("refreshFails");
        QTest::newRow("12-hours-same-bars") << qint64(12) << false;
        QTest::newRow("48-hours-refresh-fails") << qint64(48) << true;
    }

    void longestFourHourWindowKeepsOldHanaWhenClockAdvances() {
        QFETCH(qint64, advanceHours);
        QFETCH(bool, refreshFails);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto root = dir.path() + QStringLiteral("/v1");
        constexpr qint64 interval = 4 * Hour;
        constexpr int count = 200;
        const auto start = T0 - count * interval;
        qint64 storeNow = start - 3 * Hour;
        {
            CollectorStoreOptions options;
            options.root = root;
            options.collectorVersion = options.algorithmVersion = QStringLiteral("test");
            CollectorStore store(options, [&] { return storeNow; }, [] { return 500LL * 1024 * 1024 * 1024; });
            QVERIFY(store.open());
            for (int i = 0; i <= 40; ++i) {
                const auto posted = start - 2 * Hour + i * 20 * Hour;
                const auto known = posted + 10000;
                QVERIFY(store.append("fx", "naver-hana", "USDKRW", known, known,
                                     line({{"x", QString::number(1300 + i)}, {"p", posted},
                                           {"d", QDateTime::fromMSecsSinceEpoch(posted, QTimeZone::UTC).date().toString(Qt::ISODate)}, {"r", known}})));
            }
            const auto date = QDateTime::fromMSecsSinceEpoch(start, QTimeZone::UTC).date().addDays(-1);
            const auto known = FxHistory::ecbKnownAtMs(date);
            QVERIFY(store.append("fx", "ecb", "USDKRW", known, known,
                                 line({{"d", date.toString(Qt::ISODate)}, {"x", "1000.00"}, {"p", known - Hour}, {"k", known}, {"r", known}})));
            storeNow = T0 + 2 * Hour;
            QCOMPARE(store.sealDue(), 42);
        }
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 now = T0 + 4 * Hour;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setCollectorRoot(root);
        source.setInterval(interval);
        source.setWonMode(CompareWonMode::WonToDollar);
        source.setActive(true);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const auto& url = wire.sent[i].second.url;
            const QUrlQuery query(url);
            QCOMPARE(query.queryItemValue(url.host() == QLatin1String("api.upbit.com") ? "count" : "limit"), QStringLiteral("200"));
            if (url.host() == QLatin1String("api.upbit.com")) {
                QVERIFY(url.path().endsWith("/240"));
                wire.answer(i, upbitBars(start, interval, count));
            } else {
                QCOMPARE(query.queryItemValue("interval"), QStringLiteral("4h"));
                wire.answer(i, binanceBars(4000, start, interval, count));
            }
        }
        const auto before = source.snapshot();
        QCOMPARE(before.hana.size(), 41);
        const auto original = before.series.last();
        QCOMPARE(original.bars.size(), count);
        QCOMPARE(original.fx.size(), count);
        QVERIFY(now - original.fx.first().postedAtMs > 3 * 24 * Hour);
        for (const auto& fx : original.fx) {
            QCOMPARE(fx.source, QStringLiteral("hana"));
            QVERIFY(!fx.approximate);
        }
        now += advanceHours * Hour;
        source.refresh();
        // The six-hour catalog cache expires too; the historical bars stay the same.
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > 5; }, 3000));
        wire.answer(5, catalog());
        for (int i = 6; i <= 9; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const auto& url = wire.sent[i].second.url;
            if (refreshFails) wire.answer(i, QByteArray(R"({"msg":"unavailable"})"), 503);
            else wire.answer(i, url.host() == QLatin1String("api.upbit.com") ? upbitBars(start, interval, count) : binanceBars(4000, start, interval, count));
        }
        const auto after = source.snapshot().series.last();
        QCOMPARE(after.bars.size(), count);
        QCOMPARE(after.fx.size(), count);
        if (refreshFails) {
            QVERIFY(after.reason.startsWith(QStringLiteral("갱신 실패:")));
            QCOMPARE(after.lastOkAtMs, original.lastOkAtMs);
        }
        for (int i = 0; i < count; ++i) {
            QCOMPARE(after.bars[i].openMs, original.bars[i].openMs);
            QCOMPARE(after.bars[i].close, original.bars[i].close);
            QCOMPARE(after.fx[i].source, original.fx[i].source);
            QCOMPARE(after.fx[i].postedAtMs, original.fx[i].postedAtMs);
            QCOMPARE(after.fx[i].knownAtMs, original.fx[i].knownAtMs);
        }
    }

    void wonToDollarUsesCollectorHanaPostingsWithoutLookahead() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto root = dir.path() + QStringLiteral("/v1");
        qint64 storeNow = T0 - 3 * Hour;
        {
            CollectorStoreOptions options;
            options.root = root;
            options.collectorVersion = options.algorithmVersion = QStringLiteral("test");
            CollectorStore store(options, [&] { return storeNow; }, [] { return 500LL * 1024 * 1024 * 1024; });
            QVERIFY(store.open());
            const auto earlyKnown = T0 - 2 * Hour + 10000;
            QVERIFY(store.append("fx", "naver-hana", "USDKRW", earlyKnown, earlyKnown,
                                 line({{"x", "1350.00"}, {"p", T0 - 2 * Hour}, {"d", "2026-11-06"}, {"r", earlyKnown}})));
            const auto laterKnown = T0 + 5 * Hour + 10000;
            QVERIFY(store.append("fx", "naver-hana", "USDKRW", laterKnown, laterKnown,
                                 line({{"x", "1360.00"}, {"p", T0 + 5 * Hour}, {"d", "2026-11-06"}, {"r", laterKnown}})));
            QVERIFY(store.append("fx", "ecb", "USDKRW", T0 - 3 * Hour, T0 - 3 * Hour,
                                 line({{"d", "2026-11-05"}, {"x", "1344.6900"}, {"p", FxHistory::ecbKnownAtMs(QDate(2026, 11, 5)) - Hour},
                                       {"k", FxHistory::ecbKnownAtMs(QDate(2026, 11, 5))}, {"r", T0 - 3 * Hour}})));
            storeNow = T0 + 2 * 24 * Hour;
            QCOMPARE(store.sealDue(), 3);
        }

        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto now = T0 + (Bars + 1) * Interval + 30000;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setCollectorRoot(root);
        source.setInterval(Interval);
        source.setWonMode(CompareWonMode::WonToDollar);
        source.setActive(true);
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() >= 1; }, 2000));
        wire.answer(0, R"({"symbols":[{"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},{"symbol":"CLUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},{"symbol":"QQQUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"EQUITY"}]})");
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const auto& url = wire.sent[i].second.url;
            if (url.host() == QLatin1String("api.upbit.com")) wire.answer(i, upbitBars());
            else wire.answer(i, binanceBars(QUrlQuery(url).queryItemValue("symbol") == "XAUUSDT" ? 4000 : 100));
        }
        QTest::qWait(50);
        QCOMPARE(wire.sent.size(), 5); // Catalog + four klines; the collector's ECB days make a direct ECB read unnecessary.

        const auto snapshot = source.snapshot();
        QCOMPARE(snapshot.hana.size(), 2);
        QCOMPARE(snapshot.ecb.size(), 1);
        const auto& bitcoin = snapshot.series.last();
        QCOMPARE(bitcoin.spec.identity, QStringLiteral("coin:upbit:KRW-BTC"));
        QCOMPARE(bitcoin.spec.currency, QStringLiteral("USD 환산"));
        QVERIFY2(bitcoin.reason.isEmpty(), qPrintable(bitcoin.reason));
        QCOMPARE(bitcoin.bars.size(), Bars);
        QCOMPARE(bitcoin.fx.size(), Bars);
        const auto earlyKnown = T0 - 2 * Hour + 10000;
        QCOMPARE(bitcoin.fx.first().knownAtMs, earlyKnown);
        QCOMPARE(bitcoin.fx[19].knownAtMs, earlyKnown); // The bar ending 05:00 cannot use the 05:00:10 receipt.
        QCOMPARE(bitcoin.fx[20].knownAtMs, T0 + 5 * Hour + 10000);
        QCOMPARE(bitcoin.fx.last().knownAtMs, T0 + 5 * Hour + 10000);
        QVERIFY(std::abs(bitcoin.bars.first().close - 135000000.0 / 1350.0) < 1e-6);
        const auto laterKnown = T0 + 5 * Hour + 10000;
        for (const auto& bar : bitcoin.bars) {
            const double expected = bar.closeMs + 1 >= laterKnown ? 135000000.0 / 1360.0 : 135000000.0 / 1350.0;
            QVERIFY2(std::abs(bar.close - expected) < 1e-6, qPrintable(QStringLiteral("bar %1").arg(bar.openMs)));
        }
    }
};

QTEST_GUILESS_MAIN(MarketCompareSourceTests)
#include "tst_market_compare_source.moc"
