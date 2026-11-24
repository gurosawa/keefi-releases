#include "keefi/MarketBreadthMonitor.h"
#include <QDateTime>
#include <QTest>
using namespace keefi;
namespace {
const qint64 Base = QDateTime::fromString("2026-10-29T01:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
QList<PublicMarketRow> rows(qint64 at, const QStringList& rates = {"0.1", "-0.1", "0", "", "0.2"}) {
    QList<PublicMarketRow> out;
    for (int i = 0; i < rates.size(); ++i) {
        PublicMarketRow r; r.market = "KRW-T" + QString::number(i); r.base = r.market.mid(4);
        r.priced = !rates[i].isEmpty(); r.price = "100"; r.previousClose = "100"; r.signedChangeRate = rates[i];
        r.sourceTimeMs = at; r.receivedAtMs = at; r.quoteUrl = "https://api.upbit.com/v1/ticker"; out.append(r);
    }
    return out;
}
}
class MarketBreadthMonitorTest final : public QObject {
    Q_OBJECT
private slots:
    void defaultOffAndCompletedOnly() {
        qint64 now = Base + 59000; MarketBreadthMonitor m(nullptr, [&] { return now; });
        m.observeCoins(rows(now)); m.tick(); QVERIFY(!m.enabled()); QVERIFY(!m.snapshot("coin").breadth.state.available);
        m.setEnabled(true); QVERIFY(m.enabled()); m.observeCoins(rows(now)); m.tick();
        QVERIFY(!m.snapshot("coin").breadth.state.available);
        now = Base + 60000; m.tick(); const auto s = m.snapshot("coin");
        QVERIFY(s.breadth.state.available); QCOMPARE(*s.breadth.percent, 50.0);
        QCOMPARE(s.breadth.population, 5); QCOMPARE(s.breadth.ready, 4); QCOMPARE(s.breadth.flat, 1);
        QCOMPARE(s.breadth.state.direction, IndicatorDirection::Info); QCOMPARE(s.completedAtMs, now);
        QCOMPARE(s.earliestSourceTimeMs, Base + 59000); QVERIFY(s.basis.contains("전일 종가"));
        m.setEnabled(false); QVERIFY(!m.snapshot("coin").breadth.state.available);
    }
    void thresholdMissingAndStableFrozenPopulation() {
        qint64 now = Base + 59000; MarketBreadthMonitor m(nullptr, [&] { return now; }); m.setEnabled(true);
        m.observeCoins(rows(now, {"0.1", "-0.1", ""})); now = Base + 60000; m.tick();
        auto s = m.snapshot("coin"); QVERIFY(!s.breadth.state.available); QCOMPARE(s.breadth.population, 3); QCOMPARE(s.breadth.ready, 2);
        now = Base + 61000; m.observeCoins(rows(now, {"0.1", "0.2", "0.3", "0.4"}));
        QCOMPARE(m.snapshot("coin").populationIds.size(), 3); // additions never rewrite the preceding minute
        now = Base + 120000; m.tick(); s = m.snapshot("coin");
        QVERIFY(s.breadth.state.available); QCOMPARE(s.populationIds.size(), 4); QCOMPARE(*s.breadth.percent, 100.0);
        QCOMPARE(s.breadth.state.direction, IndicatorDirection::Up);
    }
    void staleFutureDuplicateAndUtcBasisAreMissing() {
        for (int scenario = 0; scenario < 4; ++scenario) {
            qint64 now = Base + 59000; MarketBreadthMonitor m(nullptr, [&] { return now; }); m.setEnabled(true);
            auto input = rows(now, {"0.1", "0.2"});
            if (scenario == 0) for (auto& r : input) r.sourceTimeMs -= 180001;
            if (scenario == 1) for (auto& r : input) r.sourceTimeMs += 2000;
            if (scenario == 2) input.last().market = input.first().market;
            if (scenario == 3) for (auto& r : input) r.sourceTimeMs -= 24 * 60 * 60000;
            m.observeCoins(input); now = Base + 60000; m.tick();
            QVERIFY(!m.snapshot("coin").breadth.state.available);
        }
    }
    void stockCommonDateIsNotInferredAndMarketsNeverMix() {
        qint64 now = Base + 59000; MarketBreadthMonitor m(nullptr, [&] { return now; }); m.setEnabled(true);
        PublicBoardItem stock; stock.id = "005930"; stock.symbol = "005930"; stock.changePercent = "12.5"; stock.receivedAtMs = now;
        m.observeStocks("kr", {stock}); m.observeStocks("us", {stock}); m.observeStocks("coin", {stock});
        m.observeCoins(rows(now)); now = Base + 60000; m.tick();
        for (const auto& market : {"kr", "us"}) {
            const auto s = m.snapshot(market); QVERIFY(!s.breadth.state.available);
            QVERIFY(!s.breadth.percent); QCOMPARE(s.breadth.ready, 0); QVERIFY(s.reason.contains("거래일"));
        }
        QCOMPARE(m.snapshot("coin").breadth.population, 5); QCOMPARE(*m.snapshot("coin").breadth.percent, 50.0);
    }
    void noNewObservationDoesNotAdvanceSnapshotAndEventuallyExpires() {
        qint64 now = Base + 59000; MarketBreadthMonitor m(nullptr, [&] { return now; }); m.setEnabled(true);
        m.observeCoins(rows(now)); now = Base + 60000; m.tick();
        const auto closed = m.snapshot("coin").completedAtMs;
        now = Base + 120000; m.tick(); QCOMPARE(m.snapshot("coin").completedAtMs, closed);
        now = Base + 300000; m.tick(); QVERIFY(!m.snapshot("coin").breadth.state.available);
        QVERIFY(!m.snapshot("coin").reason.isEmpty());
    }
};
QTEST_GUILESS_MAIN(MarketBreadthMonitorTest)
#include "tst_market_breadth_monitor.moc"
