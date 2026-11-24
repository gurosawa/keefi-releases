#include "keefi/MarketComparePeriod.h"
#include "keefi/MarketCompare.h"
#include <QSet>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000;
constexpr qint64 Hour = 60 * Minute;
constexpr qint64 Day = 24 * Hour;
constexpr qint64 Interval = 15 * Minute;
constexpr qint64 T0 = 20000LL * Day;

struct PeriodPlan {
    const char* name;
    qint64 duration;
    qint64 interval;
    qsizetype minimumBars;
    double startPrice;
    double expectedPercent;
};

// Dev99 periods are elapsed 24h / 7d / 30d / 90d, not calendar months.
// Both endpoint closes are required: N intervals need N + 1 completed bars.
constexpr PeriodPlan Plans[]{
    {"day", Day, 15 * Minute, 97, 100, 20},
    {"week", 7 * Day, Hour, 169, 80, 50},
    {"month", 30 * Day, 4 * Hour, 181, 60, 100},
    {"quarter", 90 * Day, Day, 91, 40, 200}
};

CompareSeries samples(const QList<double>& prices, qint64 interval = Interval,
                      qint64 start = T0) {
    CompareSeries out;
    out.spec = MarketCompare::defaultSpecs().last();
    out.intervalMs = interval;
    out.available = !prices.isEmpty();
    for (qsizetype i = 0; i < prices.size(); ++i) {
        const auto open = start + i * interval;
        out.bars.append({open, open + interval - 1,
                         QString::number(prices[i], 'g', 17), prices[i]});
    }
    out.receivedAtMs = start + prices.size() * interval;
    out.lastOkAtMs = out.receivedAtMs;
    return out;
}

CompareSeries ramp(qint64 interval, qint64 start, qsizetype count,
                   double first, double last) {
    QList<double> prices;
    prices.reserve(count);
    for (qsizetype i = 0; i < count; ++i)
        prices.append(i == 0 ? first : i == count - 1 ? last :
                      first + (last - first) * double(i) / double(count - 1));
    return samples(prices, interval, start);
}

CompareSeries convertedSamples() {
    auto out = samples({100000, 100000, 100000});
    out.spec = MarketCompare::upbitBitcoin();
    out.spec.currency = QStringLiteral("USD 환산");
    for (int i = 0; i < 3; ++i) {
        FxChoice choice;
        choice.available = true;
        choice.krwPerUsd = 1350;
        choice.postedAtMs = T0 - Interval;
        choice.knownAtMs = T0 - Interval + 1000;
        choice.source = i == 0 ? QStringLiteral("ecb") : QStringLiteral("hana");
        choice.label = i == 0 ? QStringLiteral("ECB 일별(근사)") : QStringLiteral("하나은행 고시");
        choice.approximate = i == 0;
        choice.previous = i == 0;
        out.fx.append(choice);
    }
    return out;
}
} // namespace

class MarketComparePeriodTests final : public QObject {
    Q_OBJECT
  private slots:
    void endpointReturnsUseTheFirstAndLastClose_data() {
        QTest::addColumn<QList<double>>("closes");
        QTest::addColumn<double>("expected");
        QTest::newRow("flat") << QList<double>{100, 100, 100, 100} << 0.0;
        QTest::newRow("rise") << QList<double>{100, 110, 121} << 21.0;
        QTest::newRow("fall") << QList<double>{100, 90, 81} << -19.0;
        QTest::newRow("intermediate-peak-and-fall") << QList<double>{100, 200, 50, 110} << 10.0;
        QTest::newRow("small-prices") << QList<double>{0.1, 0.11, 0.121} << 21.0;
        QTest::newRow("large-prices") << QList<double>{100000000, 110000000, 121000000} << 21.0;
    }

    void endpointReturnsUseTheFirstAndLastClose() {
        QFETCH(QList<double>, closes);
        QFETCH(double, expected);
        const auto series = samples(closes);
        const ComparePeriodWindow window{T0, T0 + (closes.size() - 1) * Interval,
                                         T0 + closes.size() * Interval};
        const auto result = marketComparePeriodReturn(series, window);
        QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(result.percent.has_value());
        QVERIFY(std::abs(*result.percent - expected) < 1e-9);
        QCOMPARE(result.expectedBars, closes.size());
        QCOMPARE(result.presentBars, closes.size());
    }

    void exactWindowIgnoresOutsideValuesAndGaps() {
        auto series = samples({100, 110, 121});
        series.bars.prepend({T0 - 3 * Interval, T0 - 2 * Interval - 1, "1", 1});
        series.bars.append({T0 + 5 * Interval, T0 + 6 * Interval - 1, "999999", 999999});
        series.receivedAtMs = T0 + 6 * Interval;
        const ComparePeriodWindow window{T0, T0 + 2 * Interval, T0 + 6 * Interval};
        const auto result = marketComparePeriodReturn(series, window);
        QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(result.percent.has_value());
        QVERIFY(std::abs(*result.percent - 21.0) < 1e-9);
        QCOMPARE(result.expectedBars, qsizetype(3));
        QCOMPARE(result.presentBars, qsizetype(3));
    }

    void missingBarsNeverShortenTheRequestedWindow_data() {
        QTest::addColumn<int>("removeIndex");
        QTest::addColumn<int>("coverage");
        QTest::newRow("start") << 0 << int(ComparePeriodCoverage::MissingStart);
        QTest::newRow("middle") << 2 << int(ComparePeriodCoverage::Gap);
        QTest::newRow("end") << 4 << int(ComparePeriodCoverage::MissingEnd);
    }

    void missingBarsNeverShortenTheRequestedWindow() {
        QFETCH(int, removeIndex);
        QFETCH(int, coverage);
        const ComparePeriodWindow requested{T0, T0 + 4 * Interval, T0 + 5 * Interval};
        auto series = samples({100, 101, 102, 103, 104});
        series.bars.removeAt(removeIndex);
        const auto result = marketComparePeriodReturn(series, requested);
        QCOMPARE(int(result.coverage), coverage);
        QVERIFY(!result.percent.has_value());
        QCOMPARE(result.expectedBars, qsizetype(5));
        QCOMPARE(result.presentBars, qsizetype(4));
    }

    void fourPeriodColumnsHaveHandCalculatedReturns() {
        constexpr qint64 EndExclusive = T0 + 200 * Day;
        for (const auto& spec : MarketCompare::defaultSpecs()) {
            for (const auto& plan : Plans) {
                const auto endOpen = EndExclusive - plan.interval;
                const auto startOpen = endOpen - plan.duration;
                auto series = ramp(plan.interval, startOpen, plan.minimumBars, plan.startPrice, 120);
                series.spec = spec;
                const auto result = marketComparePeriodReturn(series,
                    {startOpen, endOpen, EndExclusive});
                QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
                QVERIFY(result.percent.has_value());
                QVERIFY2(std::abs(*result.percent - plan.expectedPercent) < 1e-9,
                         qPrintable(spec.symbol + ":" + QString::fromLatin1(plan.name)));
                QCOMPARE(result.expectedBars, plan.minimumBars);
                QCOMPARE(result.presentBars, plan.minimumBars);
            }
        }
    }

    void eachPeriodNeedsBothEndpointCloses_data() {
        QTest::addColumn<qint64>("duration");
        QTest::addColumn<qint64>("interval");
        QTest::addColumn<int>("minimumBars");
        QTest::addColumn<int>("availableBars");
        for (const auto& plan : Plans) {
            for (const int extra : {-1, 0, 1}) {
                const auto name = QString("%1-minimum%2%3").arg(QString::fromLatin1(plan.name),
                    extra >= 0 ? "+" : "").arg(extra);
                QTest::newRow(qPrintable(name)) << plan.duration << plan.interval
                    << int(plan.minimumBars) << int(plan.minimumBars + extra);
            }
        }
    }

    void eachPeriodNeedsBothEndpointCloses() {
        QFETCH(qint64, duration);
        QFETCH(qint64, interval);
        QFETCH(int, minimumBars);
        QFETCH(int, availableBars);
        constexpr qint64 EndExclusive = T0 + 200 * Day;
        const auto endOpen = EndExclusive - interval;
        const auto requestedStart = endOpen - duration;
        const auto actualStart = endOpen - (availableBars - 1) * interval;
        const auto series = ramp(interval, actualStart, availableBars, 100, 100);
        const auto result = marketComparePeriodReturn(series,
            {requestedStart, endOpen, EndExclusive});
        QCOMPARE(result.expectedBars, qsizetype(minimumBars));
        QCOMPARE(result.presentBars, qsizetype(std::min(minimumBars, availableBars)));
        if (availableBars < minimumBars) {
            QCOMPARE(result.coverage, ComparePeriodCoverage::MissingStart);
            QVERIFY(!result.percent.has_value());
        } else {
            QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
            QVERIFY(result.percent.has_value());
            QCOMPARE(*result.percent, 0.0);
        }
    }

    void lastCloseMustBeStrictlyBeforeAsOf_data() {
        QTest::addColumn<qint64>("offsetFromClose");
        QTest::addColumn<bool>("ready");
        QTest::newRow("one-ms-before-close") << qint64(-1) << false;
        QTest::newRow("exact-close") << qint64(0) << false;
        QTest::newRow("one-ms-after-close") << qint64(1) << true;
    }

    void lastCloseMustBeStrictlyBeforeAsOf() {
        QFETCH(qint64, offsetFromClose);
        QFETCH(bool, ready);
        const auto series = samples({100, 110, 121});
        const auto endOpen = T0 + 2 * Interval;
        const auto close = endOpen + Interval - 1;
        const auto result = marketComparePeriodReturn(series,
            {T0, endOpen, close + offsetFromClose});
        QCOMPARE(result.coverage, ready ? ComparePeriodCoverage::Ready : ComparePeriodCoverage::NotClosed);
        QCOMPARE(result.percent.has_value(), ready);
        if (ready) QVERIFY(std::abs(*result.percent - 21.0) < 1e-9);
    }

    void formingBarOutsideTheWindowCannotChangeTheReturn() {
        auto series = samples({100, 110, 121, 1000000000});
        const ComparePeriodWindow window{T0, T0 + 2 * Interval, T0 + 3 * Interval};
        const auto result = marketComparePeriodReturn(series, window);
        QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(result.percent.has_value());
        QVERIFY(std::abs(*result.percent - 21.0) < 1e-9);
        QCOMPARE(result.presentBars, qsizetype(3));
        QCOMPARE(result.expectedBars, qsizetype(3));
    }

    void preparedFxValuesAndProvisionalCountsArePreserved() {
        const ComparePeriodWindow window{T0, T0 + 2 * Interval, T0 + 3 * Interval};
        const auto provisional = convertedSamples();
        const auto before = marketComparePeriodReturn(provisional, window,
            {T0 + 2 * Interval, T0 + 6 * Interval});
        QCOMPARE(before.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(before.percent.has_value());
        QCOMPARE(*before.percent, 0.0);
        QCOMPARE(before.provisionalFxBars, 1);
        QCOMPARE(before.approximateFxBars, 1);
        QCOMPARE(before.previousFxBars, 1);

        auto sealed = provisional;
        sealed.bars.last().close = 135000000.0 / 1400.0;
        // Source keeps the precise double beside its rounded display text.
        sealed.bars.last().closeText = QString::number(sealed.bars.last().close, 'f', 2);
        sealed.fx.last().krwPerUsd = 1400;
        sealed.fx.last().postedAtMs = T0 + 2 * Interval;
        sealed.fx.last().knownAtMs = T0 + 2 * Interval + 1000;
        const auto after = marketComparePeriodReturn(sealed, window);
        QCOMPARE(after.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(after.percent.has_value());
        QVERIFY(std::abs(*after.percent + 25.0 / 7.0) < 1e-9);
        QCOMPARE(after.provisionalFxBars, 0);
        QCOMPARE(after.approximateFxBars, 1);
        QCOMPARE(after.previousFxBars, 1);
        // A later call must not turn an earlier prepared snapshot into sealed data.
        const auto repeated = marketComparePeriodReturn(provisional, window, {T0 + 2 * Interval});
        QVERIFY(repeated.percent.has_value());
        QCOMPARE(*repeated.percent, *before.percent);
        QCOMPARE(repeated.provisionalFxBars, before.provisionalFxBars);
    }

    void fxCountsApplyOnlyInsideTheRequestedWindow() {
        const auto prepared = convertedSamples();
        const auto result = marketComparePeriodReturn(prepared,
            {T0 + Interval, T0 + 2 * Interval, T0 + 3 * Interval}, {T0, T0 + 2 * Interval});
        QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(result.percent.has_value());
        QCOMPARE(*result.percent, 0.0);
        QCOMPARE(result.expectedBars, qsizetype(2));
        QCOMPARE(result.presentBars, qsizetype(2));
        QCOMPARE(result.provisionalFxBars, 1);
        QCOMPARE(result.approximateFxBars, 0);
        QCOMPARE(result.previousFxBars, 0);
    }

    void missingFxBarsRemainMissingPeriodBars_data() {
        QTest::addColumn<int>("removeIndex");
        QTest::addColumn<int>("coverage");
        QTest::newRow("start") << 0 << int(ComparePeriodCoverage::MissingStart);
        QTest::newRow("middle") << 1 << int(ComparePeriodCoverage::Gap);
        QTest::newRow("end") << 2 << int(ComparePeriodCoverage::MissingEnd);
    }

    void missingFxBarsRemainMissingPeriodBars() {
        QFETCH(int, removeIndex);
        QFETCH(int, coverage);
        const ComparePeriodWindow window{T0, T0 + 2 * Interval, T0 + 3 * Interval};
        auto converted = convertedSamples();
        // MarketCompareSource already drops a bar when no past FX can price it.
        converted.bars.removeAt(removeIndex);
        converted.fx.removeAt(removeIndex);
        const auto result = marketComparePeriodReturn(converted, window);
        QCOMPARE(int(result.coverage), coverage);
        QVERIFY(!result.percent.has_value());
        QCOMPARE(result.expectedBars, qsizetype(3));
        QCOMPARE(result.presentBars, qsizetype(2));
    }

    void unconvertedSeriesIgnoreAnotherSeriesFxMarks_data() {
        QTest::addColumn<bool>("wonRaw");
        QTest::newRow("dollar") << false;
        QTest::newRow("won-raw") << true;
    }

    void unconvertedSeriesIgnoreAnotherSeriesFxMarks() {
        QFETCH(bool, wonRaw);
        auto series = samples(wonRaw ? QList<double>{135000000, 148500000} : QList<double>{100, 110});
        if (wonRaw) series.spec = MarketCompare::upbitBitcoin();
        QVERIFY(series.fx.isEmpty());
        const auto result = marketComparePeriodReturn(series,
            {T0, T0 + Interval, T0 + 2 * Interval}, {T0, T0 + Interval});
        QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(result.percent.has_value());
        QVERIFY(std::abs(*result.percent - 10.0) < 1e-9);
        QCOMPARE(result.provisionalFxBars, 0);
        QCOMPARE(result.approximateFxBars, 0);
        QCOMPARE(result.previousFxBars, 0);
    }

    void retainedPageStillNeedsTheRequestedCurrentEnd() {
        auto retained = samples({100, 110, 121});
        retained.reason = QStringLiteral("갱신 실패: 합성 원천 응답 없음");
        const auto before = marketComparePeriodReturn(retained,
            {T0, T0 + 2 * Interval, T0 + 3 * Interval});
        QCOMPARE(before.coverage, ComparePeriodCoverage::Ready);
        QVERIFY(before.percent.has_value());
        QVERIFY(std::abs(*before.percent - 21.0) < 1e-9);
        const auto current = marketComparePeriodReturn(retained,
            {T0 + Interval, T0 + 3 * Interval, T0 + 4 * Interval});
        QCOMPARE(current.coverage, ComparePeriodCoverage::MissingEnd);
        QVERIFY(!current.percent.has_value());
        QCOMPARE(current.expectedBars, qsizetype(3));
        QCOMPARE(current.presentBars, qsizetype(2));
        QCOMPARE(retained.bars.last().openMs, T0 + 2 * Interval);
        QCOMPARE(retained.bars.last().close, 121.0);
        QVERIFY(retained.reason.startsWith(QStringLiteral("갱신 실패:")));
    }

    void invalidWindowsHaveNoValue_data() {
        QTest::addColumn<qint64>("start");
        QTest::addColumn<qint64>("end");
        QTest::addColumn<qint64>("asOf");
        QTest::newRow("reversed") << T0 + 2 * Interval << T0 << T0 + 3 * Interval;
        QTest::newRow("same-endpoint") << T0 << T0 << T0 + Interval;
        QTest::newRow("unaligned-start") << T0 + 1 << T0 + 2 * Interval << T0 + 3 * Interval;
        QTest::newRow("unaligned-end") << T0 << T0 + 2 * Interval + 1 << T0 + 3 * Interval;
        QTest::newRow("no-asof") << T0 << T0 + 2 * Interval << qint64(0);
        QTest::newRow("before-epoch") << -Interval << T0 << T0 + 3 * Interval;
    }

    void invalidWindowsHaveNoValue() {
        QFETCH(qint64, start);
        QFETCH(qint64, end);
        QFETCH(qint64, asOf);
        const auto result = marketComparePeriodReturn(samples({100, 110, 121}), {start, end, asOf});
        QCOMPARE(result.coverage, ComparePeriodCoverage::InvalidWindow);
        QVERIFY(!result.percent.has_value());
    }

    void malformedSeriesAreNotRepaired_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"duplicate", "out-of-order", "zero-price", "negative-price", "nan-price",
                                  "infinite-price", "wrong-close-time", "unaligned-open", "zero-interval",
                                  "negative-interval", "misaligned-fx"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }

    void malformedSeriesAreNotRepaired() {
        QFETCH(QString, fault);
        auto series = samples({100, 110, 121});
        if (fault == "duplicate") series.bars[1] = series.bars[0];
        else if (fault == "out-of-order") std::swap(series.bars[1], series.bars[2]);
        else if (fault == "zero-price") series.bars[1].close = 0;
        else if (fault == "negative-price") series.bars[1].close = -1;
        else if (fault == "nan-price") series.bars[1].close = std::numeric_limits<double>::quiet_NaN();
        else if (fault == "infinite-price") series.bars[1].close = std::numeric_limits<double>::infinity();
        else if (fault == "wrong-close-time") ++series.bars[1].closeMs;
        else if (fault == "unaligned-open") { ++series.bars[1].openMs; ++series.bars[1].closeMs; }
        else if (fault == "zero-interval") series.intervalMs = 0;
        else if (fault == "negative-interval") series.intervalMs = -Interval;
        else if (fault == "misaligned-fx") series.fx.append(FxChoice{});
        else QFAIL("Unknown synthetic fault");
        const auto result = marketComparePeriodReturn(series,
            {T0, T0 + 2 * Interval, T0 + 3 * Interval});
        QCOMPARE(result.coverage, ComparePeriodCoverage::InvalidSeries);
        QVERIFY(!result.percent.has_value());
    }

    void unavailableSeriesDoesNotInventAZeroReturn() {
        auto series = samples({100, 100, 100});
        series.available = false;
        series.reason = QStringLiteral("합성 공개 원천 검증 보류");
        const auto result = marketComparePeriodReturn(series,
            {T0, T0 + 2 * Interval, T0 + 3 * Interval});
        QCOMPARE(result.coverage, ComparePeriodCoverage::Unavailable);
        QVERIFY(!result.percent.has_value());
    }

    void oneDailyPageSuppliesAllFourPeriodColumns() {
        // One unchanged page: daily closes are 100, 101, ... 299.
        // The four starting closes are 298, 292, 269 and 209.
        const auto page = ramp(Day, T0, 200, 100, 299);
        constexpr qint64 EndOpen = T0 + 199 * Day;
        constexpr qint64 AsOf = T0 + 200 * Day;
        struct ExpectedColumn {
            const char* name;
            qint64 duration;
            double percent;
            qsizetype bars;
        };
        const ExpectedColumn columns[]{
            {"day", Day, 100.0 / 298.0, 2},
            {"week", 7 * Day, 700.0 / 292.0, 8},
            {"month", 30 * Day, 3000.0 / 269.0, 31},
            {"quarter", 90 * Day, 9000.0 / 209.0, 91}
        };
        QCOMPARE(page.bars.size(), qsizetype(200));
        for (const auto& column : columns) {
            const auto result = marketComparePeriodReturn(page,
                {EndOpen - column.duration, EndOpen, AsOf});
            QCOMPARE(result.coverage, ComparePeriodCoverage::Ready);
            QVERIFY(result.percent.has_value());
            QVERIFY2(std::abs(*result.percent - column.percent) < 1e-9, column.name);
            QCOMPARE(result.expectedBars, column.bars);
            QCOMPARE(result.presentBars, column.bars);
        }
        QCOMPARE(page.bars.size(), qsizetype(200));
        QCOMPARE(page.bars.first().close, 100.0);
        QCOMPARE(page.bars.last().close, 299.0);
    }

    void finitePricesWithUnrepresentableReturnsHaveNoValue_data() {
        QTest::addColumn<double>("first");
        QTest::addColumn<double>("last");
        QTest::newRow("ratio-overflows")
            << std::numeric_limits<double>::min() << std::numeric_limits<double>::max();
        QTest::newRow("percent-overflows")
            << 1.0 << std::numeric_limits<double>::max() / 2.0;
    }

    void finitePricesWithUnrepresentableReturnsHaveNoValue() {
        QFETCH(double, first);
        QFETCH(double, last);
        QVERIFY(std::isfinite(first) && first > 0);
        QVERIFY(std::isfinite(last) && last > 0);
        const auto series = samples({first, last});
        const auto result = marketComparePeriodReturn(series,
            {T0, T0 + Interval, T0 + 2 * Interval});
        QCOMPARE(result.coverage, ComparePeriodCoverage::InvalidSeries);
        QVERIFY(!result.percent.has_value());
    }
};

QTEST_GUILESS_MAIN(MarketComparePeriodTests)
#include "tst_market_compare_period.moc"
