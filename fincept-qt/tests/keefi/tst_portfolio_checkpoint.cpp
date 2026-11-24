#include "keefi/PortfolioCheckpoint.h"
#include <QDateTime>
#include <QTest>
#include <QTimeZone>

using namespace keefi;
namespace {
const qint64 Midnight = QDateTime(QDate(2026, 10, 22), QTime(0, 0), QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();
const qint64 At = Midnight + 3 * 3600000LL;
AssetEvidence evidence(const QString& scope, qint64 time) {
    return {"FAKE_OFFICIAL_SOURCE", "https://example.invalid/official", scope, time, time, true};
}
AssetSnapshot snap(const QString& connection, qint64 time, const QString& krw, const QString& binding = "BIND") {
    AssetSnapshot s; s.id = connection + ":" + QString::number(time) + ":" + krw; s.accountScope = "SCOPE_" + connection;
    s.provider = "fake"; s.environment = "live"; s.market = "spot"; s.timeMs = time; s.balanceComplete = true;
    s.evidence = evidence(s.accountScope, time); s.connectionId = connection; s.connectionBinding = binding;
    AssetComponent c; c.id = "cash"; c.asset = "KRW"; c.quantity = krw; c.kind = AssetComponentKind::Cash; c.evidence = s.evidence;
    s.components = {c};
    return s;
}
CheckpointSelection select(const QStringList& connections) {
    CheckpointSelection selection;
    for (const auto& id : connections) selection.connections.append({id, "BIND"});
    return selection;
}
} // namespace

class PortfolioCheckpointTest final : public QObject {
    Q_OBJECT
  private slots:
    void usesLatestRecordAtOrBeforeTimeWithoutLookahead() {
        const QList<AssetSnapshot> history{snap("A", At - 400000, "1"), snap("A", At - 60000, "100"), snap("A", At + 10000, "999"),
                                           snap("B", At - 30000, "50")};
        const auto point = checkpointAt(history, {}, select({"A", "B"}), At);
        QVERIFY2(point.available, qPrintable(point.reason));
        QCOMPARE(point.timeMs, At); QCOMPARE(point.policyVersion, ValuationPolicyVersion);
        QVERIFY(point.valuation.complete); QCOMPARE(point.valuation.netAssets, QString("150"));
        QCOMPARE(point.valuation.timeMs, At);
    }
    void staleRecordBreaksCheckpointAtTheFreshnessBoundary() {
        const auto exact = checkpointAt({snap("A", At, "1"), snap("B", At - DisplayFreshnessMs, "2")}, {}, select({"A", "B"}), At);
        QVERIFY2(exact.available, qPrintable(exact.reason));
        const auto stale = checkpointAt({snap("A", At, "1"), snap("B", At - DisplayFreshnessMs - 1, "2")}, {}, select({"A", "B"}), At);
        QVERIFY(!stale.available); QVERIFY(!stale.reason.isEmpty()); QVERIFY(stale.valuation.rows.isEmpty());
    }
    void missingIncompleteOrRebindedConnectionsBreakCheckpoint_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"missing", "incomplete", "binding", "empty-selection", "other-only"}) QTest::newRow(fault) << QString(fault);
    }
    void missingIncompleteOrRebindedConnectionsBreakCheckpoint() {
        QFETCH(QString, fault);
        QList<AssetSnapshot> history{snap("A", At - 1000, "1"), snap("B", At - 1000, "2")};
        auto selection = select({"A", "B"});
        if (fault == "missing") selection = select({"A", "B", "C"});
        if (fault == "incomplete") history[1].balanceComplete = false;
        if (fault == "binding") history[1].connectionBinding = "OLD";
        if (fault == "empty-selection") selection = {};
        if (fault == "other-only") selection = select({"C"});
        const auto point = checkpointAt(history, {}, selection, At);
        QVERIFY(!point.available); QVERIFY(!point.reason.isEmpty());
    }
    void unselectedConnectionsAreNotValued() {
        const auto point = checkpointAt({snap("A", At, "1"), snap("Z", At, "1000")}, {}, select({"A"}), At);
        QVERIFY(point.available); QCOMPARE(point.valuation.netAssets, QString("1"));
        QCOMPARE(point.valuation.accountScopes, QStringList{"SCOPE_A"});
    }
    void quotesReceivedAfterTheCheckpointAreNotUsed() {
        auto holding = snap("A", At - 1000, "10"); holding.components[0].asset = "USDT";
        AssetQuote late{"USDT", "KRW", "1400", evidence("public:upbit:KRW-USDT", At - 2000)}; late.evidence.receivedAtMs = At + 1;
        const auto withoutPrice = checkpointAt({holding}, {late}, select({"A"}), At);
        QVERIFY(withoutPrice.available); QVERIFY(!withoutPrice.valuation.complete);
        auto onTime = late; onTime.evidence.receivedAtMs = At - 1000;
        const auto priced = checkpointAt({holding}, {onTime}, select({"A"}), At);
        QVERIFY(priced.valuation.complete); QCOMPARE(priced.valuation.netAssets, QString("14000"));
        AssetQuote privateOther{"USDT", "KRW", "9999", evidence("SCOPE_OTHER", At - 1000)};
        QCOMPARE(checkpointAt({holding}, {privateOther}, select({"A"}), At).valuation.complete, false);
    }
    void excludedAssetsArePassedToTheEvaluator() {
        auto holding = snap("A", At, "10"); holding.components[0].asset = "solana:So11111111111111111111111111111111111111112";
        CheckpointSelection selection = select({"A"}); selection.excludedAssets = {"solana:So11111111111111111111111111111111111111112"};
        const auto point = checkpointAt({holding}, {}, selection, At);
        QVERIFY(point.available); QCOMPARE(point.valuation.rows.size(), 1); QVERIFY(point.valuation.rows[0].excludedByUser);
    }
    void baselineIsFirstJointRecordAfterKstMidnight() {
        const QList<AssetSnapshot> history{
            snap("A", Midnight - 200000, "1"), snap("B", Midnight - 200000, "2"),   // yesterday, too old for any time today
            snap("A", Midnight + 60000, "3"),                                        // B not yet read today
            snap("A", Midnight + 600000, "5"), snap("B", Midnight + 600000 + 500, "6"),
            snap("A", Midnight + 900000, "7"), snap("B", Midnight + 900000, "8")};
        const auto baseline = dailyBaseline(history, {}, select({"A", "B"}), At);
        QVERIFY2(baseline.available, qPrintable(baseline.reason));
        QCOMPARE(baseline.timeMs, Midnight + 600000 + 500); QCOMPARE(baseline.valuation.netAssets, QString("11"));
        QCOMPARE(baseline.policyVersion, ValuationPolicyVersion);
        // Same freshness rule as the live screen: a balance read 2 minutes before
        // midnight still describes 00:01, but the baseline time stays 00:01.
        auto withRecentB = history; withRecentB[1] = snap("B", Midnight - 60000, "2");
        const auto early = dailyBaseline(withRecentB, {}, select({"A", "B"}), At);
        QCOMPARE(early.timeMs, Midnight + 60000); QCOMPARE(early.valuation.netAssets, QString("5"));
    }
    void baselineNeverUsesYesterdayOrTheFuture() {
        const QList<AssetSnapshot> yesterday{snap("A", Midnight - 1, "1"), snap("B", Midnight - 1, "2")};
        const auto none = dailyBaseline(yesterday, {}, select({"A", "B"}), At);
        QVERIFY(!none.available); QVERIFY(!none.reason.isEmpty());
        const QList<AssetSnapshot> future{snap("A", At + 1000, "1"), snap("B", At + 1000, "2")};
        QVERIFY(!dailyBaseline(future, {}, select({"A", "B"}), At).available);
        // A record exactly at midnight belongs to the new day.
        const auto exact = dailyBaseline({snap("A", Midnight, "4"), snap("B", Midnight, "5")}, {}, select({"A", "B"}), At);
        QVERIFY(exact.available); QCOMPARE(exact.timeMs, Midnight);
    }
    void baselineUsesTheKstDateEvenLateInTheUtcDay() {
        // 23:30 KST on 10-22 is 14:30 UTC the same day; 00:30 KST on 10-23 is still 10-22 in UTC.
        const qint64 nextMidnight = Midnight + 24 * 3600000LL;
        const QList<AssetSnapshot> history{snap("A", Midnight + 1000, "1"), snap("A", nextMidnight + 60000, "2")};
        QCOMPARE(dailyBaseline(history, {}, select({"A"}), nextMidnight + 1800000).timeMs, nextMidnight + 60000);
        QCOMPARE(dailyBaseline(history, {}, select({"A"}), nextMidnight - 1).timeMs, Midnight + 1000);
    }
    void jointHistoryNeedsEveryLiveAccountFreshAndComplete() {
        const QList<AssetSnapshot> fresh{snap("A", At - 1000, "1"), snap("B", At - DisplayFreshnessMs, "2")};
        QVERIFY(jointHistoryDue(fresh, At, 0));
        QVERIFY(!jointHistoryDue(fresh, At, At - JointHistoryIntervalMs + 1));
        QVERIFY(jointHistoryDue(fresh, At, At - JointHistoryIntervalMs));
        QVERIFY(!jointHistoryDue({}, At, 0));
        auto stale = fresh; stale[1].timeMs = At - DisplayFreshnessMs - 1;
        QVERIFY(!jointHistoryDue(stale, At, 0));
        auto incomplete = fresh; incomplete[1].balanceComplete = false;
        QVERIFY(!jointHistoryDue(incomplete, At, 0));
        auto future = fresh; future[0].timeMs = At + 2001;
        QVERIFY(!jointHistoryDue(future, At, 0));
    }
};
QTEST_APPLESS_MAIN(PortfolioCheckpointTest)
#include "tst_portfolio_checkpoint.moc"
