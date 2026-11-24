#include "keefi/RapidMoveMonitor.h"
#include <QDateTime>
#include <QTest>
using namespace keefi;
namespace {
const qint64 Base = QDateTime::fromString("2026-10-29T00:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
PublicTick quote(int minute, const QString& price, const QString& cumulative, quint64 epoch = 1) {
    const auto at = Base + minute * 60000 + 59000;
    PublicTick t;
    t.market = "KRW-BTC";
    t.quote = {"BTC", "KRW", price, {"업비트 공식 KRW 현재가", "wss://api.upbit.com/websocket/v1", "public:upbit:KRW-BTC", at, at, true}};
    t.tradeValue24h = "999999999999";
    t.tradeValueUtcDay = cumulative;
    t.tradeDateUtc = QDateTime::fromMSecsSinceEpoch(at, Qt::UTC).toString("yyyyMMdd");
    t.tradeTimestampMs = at;
    t.realtime = true;
    t.streamEpoch = epoch;
    return t;
}
struct Fixture {
    qint64 now = Base;
    RapidMoveMonitor monitor{nullptr, [this] { return now; }};
    QList<ChartPublicStudyEvent> events;
    Fixture() { QObject::connect(&monitor, &RapidMoveMonitor::eventReady, &monitor, [this](const auto& e) { events.append(e); }); }
    void minute(int index, QString price = "100", QString counter = {}, quint64 epoch = 1) {
        if (counter.isEmpty()) counter = QString::number(qint64(index) * 20000000);
        auto t = quote(index, price, counter, epoch);
        now = t.quote.evidence.receivedAtMs;
        monitor.observe({t});
        now = Base + (index + 1) * 60000 + 2000;
        monitor.tick();
    }
};
}
class RapidMoveMonitorTest final : public QObject {
    Q_OBJECT
private slots:
    void defaultOffAndWarmup() {
        Fixture f;
        f.minute(0); QVERIFY(f.events.isEmpty()); QVERIFY(!f.monitor.enabled());
        f.monitor.setEnabled(true); QVERIFY(f.monitor.enabled());
        for (int i = 1; i < 6; ++i) f.minute(i);
        QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        f.minute(6, "103");
        QCOMPARE(f.events.size(), 1);
        const auto s = f.monitor.snapshot("KRW-BTC");
        QVERIFY(s.available); QCOMPARE(s.quoteTurnover, QString("100000000"));
        QCOMPARE(s.direction, MovementDirection::Up);
        QCOMPARE(s.fromObservedAtMs, Base + 119000); QCOMPARE(s.throughObservedAtMs, Base + 419000);
        QCOMPARE(f.events.first().identity, (WatchlistIdentity{"coin", "upbit", "KRW-BTC"}));
        QCOMPARE(f.events.first().kind, QString("rapid_move")); QVERIFY(f.events.first().liveEligible);
        f.monitor.setEnabled(false); QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
    }
    void exactThresholdAndQuoteTurnoverBoundary() {
        for (const auto& pair : QList<QPair<QString, int>>{{"103", 1}, {"102.999999999999999999", 0}, {"97", 1}, {"97.000000000000000001", 0}}) {
            Fixture f; f.monitor.setEnabled(true);
            for (int i = 0; i < 5; ++i) f.minute(i);
            f.minute(5, pair.first); QCOMPARE(f.events.size(), pair.second);
        }
        Fixture f; f.monitor.setEnabled(true);
        for (int i = 0; i < 5; ++i) f.minute(i);
        f.minute(5, "110", "99999999");
        QVERIFY(f.monitor.snapshot("KRW-BTC").available); QVERIFY(f.events.isEmpty());
        QCOMPARE(f.monitor.snapshot("KRW-BTC").direction, MovementDirection::Flat);
    }
    void missingMinuteAndNoRolling24hFallback() {
        Fixture f; f.monitor.setEnabled(true);
        for (int i : {0, 1, 3, 4, 5}) f.minute(i);
        QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        auto t = quote(6, "110", ""); f.now = t.quote.evidence.receivedAtMs; f.monitor.observe({t});
        f.now += 3000; f.monitor.tick();
        QVERIFY(f.events.isEmpty()); QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        QVERIFY(!f.monitor.snapshot("KRW-BTC").reason.isEmpty());
    }
    void reconnectCounterResetAndUtcRolloverInvalidate() {
        Fixture f; f.monitor.setEnabled(true);
        for (int i = 0; i < 5; ++i) f.minute(i);
        f.minute(5, "110", "100000000", 2); QVERIFY(f.events.isEmpty());
        QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        for (int i = 6; i < 10; ++i) f.minute(i, "110", {}, 2);
        f.minute(10, "120", "1", 2); QVERIFY(f.events.isEmpty());
        QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        Fixture g; g.monitor.setEnabled(true);
        for (int i = 1435; i < 1440; ++i) g.minute(i, "100", QString::number((i - 1435) * 20000000));
        g.minute(1440, "110", "100000000"); QVERIFY(g.events.isEmpty());
        QVERIFY(!g.monitor.snapshot("KRW-BTC").available);
    }
    void oneEpisodeCooldownAndOppositeDirection() {
        Fixture f; f.monitor.setEnabled(true);
        for (int i = 0; i < 5; ++i) f.minute(i);
        for (int i = 5; i <= 11; ++i) f.minute(i, "103");
        QCOMPARE(f.events.size(), 1);
        f.minute(12, "110"); QCOMPARE(f.events.size(), 1); // new episode, within ten minutes
        f.minute(13, "90"); QCOMPARE(f.events.size(), 2);
        QCOMPARE(f.events.last().direction, MovementDirection::Down);
        QVERIFY(f.events.first().eventId != f.events.last().eventId);
        for (int i = 14; i <= 16; ++i) f.minute(i, "103");
        f.minute(17, "110"); f.minute(18, "120");
        QCOMPARE(f.events.size(), 3); // prior up episode ended and its cooldown elapsed
        QCOMPARE(f.events.last().direction, MovementDirection::Up);
    }
    void formingMinutePoorEndCoverageAndSuspendNeverReplay() {
        Fixture f; f.monitor.setEnabled(true);
        for (int i = 0; i < 5; ++i) f.minute(i);
        auto t = quote(5, "110", "100000000"); f.now = t.quote.evidence.receivedAtMs;
        f.monitor.observe({t}); f.monitor.tick(); QVERIFY(f.events.isEmpty());
        f.now += 120000; f.monitor.tick(); QVERIFY(f.events.isEmpty());
        QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        Fixture g; g.monitor.setEnabled(true);
        for (int i = 0; i < 5; ++i) g.minute(i);
        t = quote(5, "110", "100000000");
        t.quote.evidence.sourceTimeMs -= 10000; t.quote.evidence.receivedAtMs -= 10000; t.tradeTimestampMs -= 10000;
        g.now = t.quote.evidence.receivedAtMs; g.monitor.observe({t});
        g.now = Base + 362000; g.monitor.tick(); QVERIFY(g.events.isEmpty());
        QVERIFY(!g.monitor.snapshot("KRW-BTC").available);
    }
    void snapshotsOldTicksAndForeignMarketsDoNotAlert() {
        Fixture f; f.monitor.setEnabled(true);
        for (int i = 0; i < 6; ++i) {
            auto t = quote(i, i == 5 ? "110" : "100", QString::number(i * 20000000));
            t.realtime = false; f.now = t.quote.evidence.receivedAtMs;
            f.monitor.observe({t}); f.now += 3000; f.monitor.tick();
        }
        QVERIFY(f.events.isEmpty()); QVERIFY(!f.monitor.snapshot("KRW-BTC").available);
        auto t = quote(7, "110", "140000000"); t.market = "BTCUSDT"; t.quote.quoteAsset = "USDT";
        f.now = t.quote.evidence.receivedAtMs; f.monitor.observe({t}); f.now += 3000; f.monitor.tick();
        QVERIFY(f.events.isEmpty());
        t = quote(8, "110", "160000000"); f.now = t.quote.evidence.receivedAtMs + 120000;
        f.monitor.observe({t}); f.monitor.tick(); QVERIFY(f.events.isEmpty());
    }
};
QTEST_GUILESS_MAIN(RapidMoveMonitorTest)
#include "tst_rapid_move_monitor.moc"
