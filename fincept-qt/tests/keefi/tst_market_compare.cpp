#include "keefi/MarketCompare.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QSet>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest>
#include <cmath>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000, Day = 86400000;
const qint64 Monday = QDateTime(QDate(2026, 11, 2), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();

CompareSpec find(const QString& symbol) {
    for (const auto& spec : MarketCompare::defaultSpecs() + MarketCompare::optionalSpecs())
        if (spec.symbol == symbol) return spec;
    return {};
}

QByteArray tuple(qint64 open, qint64 interval, const QString& close, const QString& high = {}, const QString& low = {},
                 qint64 closeTime = -1) {
    const auto h = high.isEmpty() ? close : high;
    const auto l = low.isEmpty() ? close : low;
    return QStringLiteral("[%1,\"%2\",\"%3\",\"%4\",\"%2\",\"10.5\",%5,\"100\",42,\"5\",\"50\",\"0\"]")
        .arg(open)
        .arg(close, h, l)
        .arg(closeTime < 0 ? open + interval - 1 : closeTime)
        .toUtf8();
}

HttpResult ok(const QList<QByteArray>& rows) {
    HttpResult result;
    result.status = 200;
    result.body = '[' + rows.join(',') + ']';
    return result;
}

CompareSeries series(const CompareSpec& spec, qint64 interval, const QList<double>& closes, qint64 start = Monday) {
    CompareSeries out;
    out.spec = spec;
    out.intervalMs = interval;
    out.available = true;
    for (int i = 0; i < closes.size(); ++i)
        out.bars.append({start + i * interval, start + (i + 1) * interval - 1, QString::number(closes[i]), closes[i]});
    return out;
}

QList<double> walk(int count, double scale) {
    QList<double> out;
    double price = 100;
    for (int i = 0; i < count; ++i) {
        out.append(price);
        price *= std::exp(scale * std::sin(i * 1.7) * 0.01);
    }
    return out;
}

CorrelationPoint point(int index, std::optional<double> r20, std::optional<double> r60) {
    CorrelationPoint out;
    out.openMs = Monday + index * Minute;
    out.r20 = r20;
    out.r60 = r60;
    return out;
}
} // namespace

class MarketCompareTests final : public QObject {
    Q_OBJECT
  private slots:
    void defaultsAreTheFourDecidedMarkets() {
        const auto specs = MarketCompare::defaultSpecs();
        QCOMPARE(specs.size(), 4);
        QCOMPARE(specs[0].identity, QStringLiteral("perp:binance-usdm:XAUUSDT"));
        QCOMPARE(specs[0].underlyingType, QStringLiteral("COMMODITY"));
        QCOMPARE(specs[1].identity, QStringLiteral("perp:binance-usdm:CLUSDT"));
        QCOMPARE(specs[2].identity, QStringLiteral("perp:binance-usdm:QQQUSDT"));
        QCOMPARE(specs[2].underlyingType, QStringLiteral("EQUITY"));
        QCOMPARE(specs[3].identity, QStringLiteral("coin:binance:BTCUSDT"));
        QVERIFY(specs[3].underlyingType.isEmpty());
        QCOMPARE(find("KODEX200USDT").underlyingType, QStringLiteral("KR_EQUITY"));
        for (const auto& spec : specs) QCOMPARE(spec.currency, QStringLiteral("USDT"));
    }

    void requestsAreExactAndAllowListed() {
        const auto gold = find("XAUUSDT");
        const auto request = MarketCompare::klinesRequest(gold, 15 * Minute);
        QCOMPARE(request.url.toString(), QStringLiteral("https://fapi.binance.com/fapi/v1/klines?symbol=XAUUSDT&interval=15m&limit=200"));
        QVERIFY(MarketCompare::isRequest(request));
        QVERIFY(NetworkTradingTransport::allowed(request));
        const auto btc = MarketCompare::klinesRequest(find("BTCUSDT"), Day);
        QCOMPARE(btc.url.toString(), QStringLiteral("https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1d&limit=200"));
        QVERIFY(MarketCompare::isRequest(btc));
        QVERIFY(MarketCompare::isRequest(MarketCompare::perpCatalogRequest()));
        QVERIFY(NetworkTradingTransport::allowed(MarketCompare::perpCatalogRequest()));

        auto other = request;
        other.url = QUrl(QStringLiteral("https://fapi.binance.com/fapi/v1/klines?symbol=SPXUSDT&interval=15m&limit=200"));
        QVERIFY2(!MarketCompare::isRequest(other), "A coin named like an index is not on the allow list");
        other.url = QUrl(QStringLiteral("https://fapi.binance.com/fapi/v1/klines?interval=15m&symbol=XAUUSDT&limit=200"));
        QVERIFY(!MarketCompare::isRequest(other));
        other.url = QUrl(QStringLiteral("https://fapi.binance.com/fapi/v1/klines?symbol=XAUUSDT&interval=15m&limit=200&x=1"));
        QVERIFY(!MarketCompare::isRequest(other));
        other.url = QUrl(QStringLiteral("https://api.binance.com/fapi/v1/klines?symbol=XAUUSDT&interval=15m&limit=200"));
        QVERIFY(!MarketCompare::isRequest(other));
        other.url = QUrl(QStringLiteral("https://fapi.binance.com/fapi/v1/klines?symbol=XAUUSDT&interval=2h&limit=200"));
        QVERIFY(!MarketCompare::isRequest(other));
        QVERIFY(MarketCompare::klinesRequest(gold, 15 * Minute, 1001).url.isEmpty());
        auto posted = request;
        posted.method = "POST";
        QVERIFY(!MarketCompare::isRequest(posted));
        QVERIFY(MarketCompare::klinesRequest(gold, 2 * 3600000).url.isEmpty());
    }

    void klinesKeepOnlyCompletedValidBars() {
        const auto gold = find("XAUUSDT");
        const qint64 interval = 15 * Minute;
        const auto received = Monday + 3 * interval + 5000; // Third bar is forming.
        const auto parsed = MarketCompare::parseKlines(
            gold, interval, ok({tuple(Monday, interval, "4070.5"), tuple(Monday + interval, interval, "4080"), tuple(Monday + 3 * interval, interval, "4090")}),
            received);
        QVERIFY(parsed.available);
        QCOMPARE(parsed.bars.size(), 2);
        QCOMPARE(parsed.bars[0].closeText, QStringLiteral("4070.5"));
        QCOMPARE(parsed.bars[1].closeMs, Monday + 2 * interval - 1);

        const auto misaligned = MarketCompare::parseKlines(gold, interval, ok({tuple(Monday, interval, "1", {}, {}, Monday + interval)}), received);
        QVERIFY(!misaligned.available);
        const auto crossed = MarketCompare::parseKlines(gold, interval, ok({tuple(Monday, interval, "10", "9", "8")}), received);
        QVERIFY(!crossed.available);
        const auto backwards = MarketCompare::parseKlines(gold, interval, ok({tuple(Monday + interval, interval, "1"), tuple(Monday, interval, "1")}), received);
        QVERIFY(!backwards.available);
        HttpResult limited;
        limited.status = 429;
        QCOMPARE(MarketCompare::parseKlines(gold, interval, limited, received).reason, QStringLiteral("원천 요청 제한"));
    }

    void perpetualsNeedTheSameClassificationAsTheAllowList() {
        HttpResult catalog;
        catalog.status = 200;
        catalog.body = R"({"symbols":[
            {"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},
            {"symbol":"SPXUSDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"},
            {"symbol":"KODEX200USDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"KR_EQUITY"},
            {"symbol":"QQQUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},
            {"symbol":"CLUSDT","contractType":"PERPETUAL","status":"BREAK","underlyingType":"COMMODITY"},
            {"symbol":"BTCUSDT_261225","contractType":"CURRENT_QUARTER","status":"TRADING","underlyingType":"COIN"},
            {"symbol":"BTCUSDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})";
        QString reason;
        const auto perps = MarketCompare::parsePerpCatalog(catalog, &reason);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(perps.value("XAUUSDT"), QStringLiteral("COMMODITY"));
        QVERIFY(!perps.contains("CLUSDT"));
        QVERIFY(!perps.contains("BTCUSDT_261225"));
        QCOMPARE(perps.value("BTCUSDT"), QStringLiteral("COIN"));
        QCOMPARE(perps.value("KODEX200USDT"), QStringLiteral("KR_EQUITY"));
        QVERIFY(MarketCompare::perpAllowed(find("XAUUSDT"), perps, &reason));
        QVERIFY(MarketCompare::perpAllowed(find("KODEX200USDT"), perps, &reason));
        QVERIFY(!MarketCompare::perpAllowed(find("QQQUSDT"), perps, &reason));
        QVERIFY(reason.contains("COMMODITY"));
        QVERIFY(!MarketCompare::perpAllowed(find("CLUSDT"), perps, &reason));
        auto mislabeled = find("KODEX200USDT");
        mislabeled.underlyingType = QStringLiteral("EQUITY");
        QVERIFY(!MarketCompare::perpAllowed(mislabeled, perps, &reason));
        QVERIFY(MarketCompare::perpAllowed(find("BTCUSDT"), {}, &reason));
    }

    void percentStartsAtTheBaseBar() {
        const auto values = MarketCompare::percentFromBase(series(find("BTCUSDT"), Minute, {90, 100, 110, 99}), Monday + Minute);
        QCOMPARE(values.size(), 3);
        QCOMPARE(values[0].percent, 0.0);
        QVERIFY(std::abs(values[1].percent - 10.0) < 1e-9);
        QVERIFY(std::abs(values[2].percent + 1.0) < 1e-9);
        QVERIFY(MarketCompare::percentFromBase(series(find("BTCUSDT"), Minute, {1}), Monday + 7 * Minute).isEmpty());
    }

    void correlationPairsOnlyMatchingConsecutiveBars() {
        const auto closes = walk(70, 1.0);
        const auto base = series(find("BTCUSDT"), Minute, closes);
        auto same = series(find("XAUUSDT"), Minute, closes);
        auto points = MarketCompare::rollingCorrelation(base, same);
        QCOMPARE(points.size(), 70);
        QVERIFY(!points[19].r20);
        QVERIFY(points[20].r20);
        QVERIFY(std::abs(*points[20].r20 - 1.0) < 1e-9);
        QVERIFY(points[60].r60);

        QList<double> mirrored;
        for (const auto close : closes) mirrored.append(10000.0 / close);
        const auto inverse = MarketCompare::rollingCorrelation(base, series(find("XAUUSDT"), Minute, mirrored));
        QVERIFY(std::abs(*inverse[30].r20 + 1.0) < 1e-9);

        same.bars.removeAt(30); // A missing bar restarts the run on both sides.
        points = MarketCompare::rollingCorrelation(base, same);
        QVERIFY(!points[30].r20);
        QVERIFY(!points[50].r20);
        QCOMPARE(points[50].pairs20, 19);
        QVERIFY(points[51].r20);

        auto shifted = series(find("XAUUSDT"), Minute, closes);
        for (auto& bar : shifted.bars) bar.closeMs += 1; // Same start, different end: never paired.
        points = MarketCompare::rollingCorrelation(base, shifted);
        QVERIFY(!points[40].r20);
        QVERIFY(MarketCompare::rollingCorrelation(base, series(find("XAUUSDT"), 5 * Minute, closes)).isEmpty());
    }

    void relationChangeNeedsSizePersistenceAndAPriorClearRun() {
        QList<CorrelationPoint> flips;
        for (int i = 0; i < 10; ++i) flips.append(point(i, 0.01, -0.01));
        QVERIFY2(MarketCompare::relationChanges(flips).isEmpty(), "Tiny opposite correlations are noise");

        QList<CorrelationPoint> points;
        int i = 0;
        for (; i < 3; ++i) points << point(i, -0.4, 0.5);  // No known clear run before: never marked.
        for (; i < 6; ++i) points << point(i, -0.5, 0.6);
        for (; i < 8; ++i) points << point(i, 0.4, 0.5);   // Two clear bars are not enough.
        for (; i < 11; ++i) points << point(i, -0.4, 0.5);
        for (; i < 14; ++i) points << point(i, 0.4, 0.5);  // Three clear bars...
        for (; i < 17; ++i) points << point(i, 0.35, -0.6); // ...then three condition bars: one mark, on the third.
        for (; i < 20; ++i) points << point(i, 0.35, -0.6); // The same state continuing adds nothing.
        const auto changes = MarketCompare::relationChanges(points);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0].openMs, Monday + 16 * Minute);
        QCOMPARE(changes[0].r60, -0.6);

        QList<CorrelationPoint> missing;
        for (int j = 0; j < 3; ++j) missing << point(j, 0.4, std::nullopt);
        for (int j = 3; j < 6; ++j) missing << point(j, -0.5, 0.5);
        QVERIFY2(MarketCompare::relationChanges(missing).isEmpty(), "Unknown bars in the lookback never count as clear");
    }

    void original62To63WindowShiftDoesNotCreateAnotherMark() {
        // The old armed flag restarted at the first available r60. Moving a
        // 200-bar window by one candle moved the apparent mark from 62 to 63.
        QList<double> left{100.0}, right{100.0};
        for (int i = 1; i <= 200; ++i) {
            const double change = (i % 2 ? 1.0 : -1.0) * 0.01;
            left.append(left.last() * std::exp(change));
            right.append(right.last() * std::exp((i <= 40 ? 2.0 : -1.0) * change));
        }
        const auto original = MarketCompare::rollingCorrelation(
            series(find("BTCUSDT"), Minute, left.mid(0, 200)),
            series(find("ETHUSDT"), Minute, right.mid(0, 200)));
        const auto shifted = MarketCompare::rollingCorrelation(
            series(find("BTCUSDT"), Minute, left.mid(1, 200), Monday + Minute),
            series(find("ETHUSDT"), Minute, right.mid(1, 200), Monday + Minute));

        QCOMPARE(original.size(), 200);
        QCOMPARE(shifted.size(), 200);
        QCOMPARE(original[62].openMs, Monday + 62 * Minute);
        QCOMPARE(shifted[62].openMs, Monday + 63 * Minute);
        QCOMPARE(original[62].pairs60, 60);
        QCOMPARE(shifted[62].pairs60, 60);
        QVERIFY(original[62].r20 && original[62].r60);
        QVERIFY(shifted[62].r20 && shifted[62].r60);
        QVERIFY(std::abs(*original[62].r20 + 1.0) < 1e-9);
        QVERIFY(std::abs(*shifted[62].r20 + 1.0) < 1e-9);
        QVERIFY(std::abs(*original[62].r60 - 0.5284981975632369) < 1e-9);
        QVERIFY(std::abs(*shifted[62].r60 - 0.5037175233040704) < 1e-9);
        QVERIFY(original[63].r60);
        QVERIFY(std::abs(*original[63].r60 - *shifted[62].r60) < 1e-12);

        // Warm-up is unknown, so neither window has three known clear bars
        // before the first three qualifying bars. Neither may invent a mark.
        QVERIFY(MarketCompare::relationChanges(original).isEmpty());
        QVERIFY(MarketCompare::relationChanges(shifted).isEmpty());
    }

    void relationMarksStayPutWhenTheWindowSlides() {
        // Same prices, a 200-bar page sliding one bar at a time: shared bars keep the same marks.
        constexpr int total = 300, page = 200;
        QList<double> base, target;
        double b = 100, t = 50;
        for (int i = 0; i < total; ++i) {
            base.append(b);
            target.append(t);
            const double move = 0.01 * std::sin(i * 1.3) + 0.005 * std::cos(i * 0.7);
            const double noise = 0.003 * std::sin(i * 2.9);
            const bool opposite = (i >= 120 && i < 150) || (i >= 190 && i < 215) || (i >= 250 && i < 270);
            b *= std::exp(move);
            t *= std::exp((opposite ? -move : move) + noise);
        }
        const auto window = [&](const QList<double>& values, int from, const CompareSpec& spec) {
            return series(spec, Minute, values.mid(from, page), Monday + from * Minute);
        };
        QList<QList<RelationChange>> pages;
        int marked = 0;
        for (int shift = 0; shift <= total - page; ++shift) {
            const auto changes = MarketCompare::relationChanges(
                MarketCompare::rollingCorrelation(window(base, shift, find("BTCUSDT")), window(target, shift, find("XAUUSDT"))));
            pages.append(changes);
            marked += changes.size();
        }
        QVERIFY2(marked > 0, "The data must produce marks for this test to mean anything");
        for (int shift = 0; shift + 1 < pages.size(); ++shift) {
            // Bars both pages share and where the later page has a full 60-bar lookback plus the 6-bar rule.
            const qint64 from = Monday + qint64(shift + 1 + 60 + 6) * Minute, to = Monday + qint64(shift + page - 1) * Minute;
            QList<qint64> a, b2;
            for (const auto& change : pages[shift]) if (change.openMs >= from && change.openMs <= to) a.append(change.openMs);
            for (const auto& change : pages[shift + 1]) if (change.openMs >= from && change.openMs <= to) b2.append(change.openMs);
            QCOMPARE(b2, a);
            // Anywhere in the overlap, the later page never invents or moves a mark.
            QSet<qint64> earlier;
            for (const auto& change : pages[shift]) earlier.insert(change.openMs);
            for (const auto& change : pages[shift + 1])
                if (change.openMs <= to) QVERIFY2(earlier.contains(change.openMs), qPrintable(QStringLiteral("moved mark at %1").arg(change.openMs)));
        }
    }

    void labelsAndWeekend() {
        QCOMPARE(MarketCompare::relationLabel(0.6), QStringLiteral("같이 움직임"));
        QCOMPARE(MarketCompare::relationLabel(-0.5), QStringLiteral("반대로 움직임"));
        QCOMPARE(MarketCompare::relationLabel(0.2), QStringLiteral("따로 움직임"));
        QCOMPARE(MarketCompare::relationLabel(std::nullopt), QStringLiteral("표본 부족"));
        const auto saturday = QDateTime(QDate(2026, 11, 7), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        QVERIFY(MarketCompare::underlyingWeekend(find("XAUUSDT"), saturday));
        QVERIFY(MarketCompare::underlyingWeekend(find("QQQUSDT"), saturday + Day));
        QVERIFY(!MarketCompare::underlyingWeekend(find("QQQUSDT"), Monday));
        QVERIFY(!MarketCompare::underlyingWeekend(find("BTCUSDT"), saturday));
    }
};

QTEST_GUILESS_MAIN(MarketCompareTests)
#include "tst_market_compare.moc"
