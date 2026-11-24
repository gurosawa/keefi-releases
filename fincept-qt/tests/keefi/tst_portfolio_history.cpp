#include "keefi/PortfolioHistory.h"
#include "keefi/AssetsLedger.h"
#include "keefi/Vault.h"
#include "fixtures/G8HistoryFixture.h"
#include <QDateTime>
#include <QTest>
#include <QTemporaryDir>
#include <QTimeZone>

using namespace keefi;
namespace {
const qint64 Midnight = QDateTime(QDate(2026, 10, 22), QTime(0, 0), QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();
const qint64 At = Midnight + 3600000;
AssetEvidence evidence(const QString& scope, qint64 time) {
    return {"FAKE_OFFICIAL_SOURCE", "https://example.invalid/official", scope, time, time, true};
}
AssetSnapshot snap(const QString& connection, qint64 time, const QString& krw, const QString& binding = "BIND") {
    AssetSnapshot s; s.id = connection + ":" + QString::number(time); s.accountScope = "SCOPE_" + connection;
    s.provider = "fake"; s.environment = "live"; s.market = "spot"; s.timeMs = time; s.balanceComplete = true;
    s.evidence = evidence(s.accountScope, time); s.connectionId = connection; s.connectionBinding = binding;
    AssetComponent c; c.id = "cash"; c.asset = "KRW"; c.quantity = krw; c.kind = AssetComponentKind::Cash; c.evidence = s.evidence;
    s.components = {c}; return s;
}
CheckpointSelection select(const QStringList& connections) {
    CheckpointSelection selection;
    for (const auto& id : connections) selection.connections.append({id, "BIND"});
    return selection;
}
AssetValuationResult current(const QList<AssetSnapshot>& snapshots, qint64 time,
    const QList<AssetQuote>& quotes = {}, const QSet<QString>& excluded = {}) {
    return AssetValuation::value(snapshots, quotes, "KRW", time, DisplayFreshnessMs, {}, excluded);
}
void addUnpriced(AssetSnapshot& s) {
    auto missing = s.components.first(); missing.id = "missing"; missing.asset = "UNKNOWN"; missing.quantity = "10";
    s.components.append(missing);
}
} // namespace

class PortfolioHistoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void midnightGapUsesOnlyTheRequestedKstDay_data() {
        QTest::addColumn<qint64>("at");
        QTest::addColumn<bool>("includeGap");
        QTest::addColumn<QString>("difference");
        QTest::newRow("00-04-previous-day-gap") << Midnight + 4 * 60000 << false << QString("50");
        QTest::newRow("00-09-same-day-gap") << Midnight + 9 * 60000 << true << QString("75");
    }
    void midnightGapUsesOnlyTheRequestedKstDay() {
        QFETCH(qint64, at);
        QFETCH(bool, includeGap);
        QFETCH(QString, difference);
        const QString binding(64, 'a');
        const auto storedSnapshot = [&](qint64 time, const QString& value) {
            auto row = snap("A", time, value, binding);
            row.components[0].valuationGroup = "synthetic-cash";
            return row;
        };
        const CheckpointSelection selection{{{"A", binding}}, {}};
        const auto older = storedSnapshot(at - 1000, "50");
        const auto gap = storedSnapshot(older.timeMs - JointHistoryIntervalMs - DisplayFreshnessMs - 1, "25");
        const auto latest = storedSnapshot(at, "100");
        const QList<AssetSnapshot> history{gap, older, latest};
        QTemporaryDir dir;
        Vault vault(dir.filePath("synthetic.vault"));
        QVERIFY(vault.create(QStringLiteral("Fake Midnight History! 2026")));
        AssetsLedger ledger(&vault, dir.filePath("synthetic-history.keefi"));
        QVERIFY(ledger.open(true));
        QVERIFY(ledger.append(history, {}, {}));
        ledger.lock(); // Exercise the deferred encrypted-file path, not an open cache.
        bool finished = false, readOk = false;
        QString error;
        AssetsLedger::HistoryWindow window;
        ledger.readWindowAsync(Midnight, at, [&](bool ok, AssetsLedger::HistoryWindow rows, QString message) {
            readOk = ok; window = std::move(rows); error = std::move(message); finished = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 2000);
        QVERIFY2(readOk, qPrintable(error));
        QCOMPARE(window.snapshots.size(), includeGap ? 3 : 2);
        bool storedGap = false;
        for (const auto& row : window.snapshots) {
            QVERIFY(row.timeMs >= Midnight);
            QVERIFY(row.timeMs <= at);
            storedGap = storedGap || row.timeMs == gap.timeMs;
        }
        QCOMPARE(storedGap, includeGap);
        const auto live = current({latest}, at);
        const auto filtered = portfolioDayHistory(window.snapshots, window.quotes, selection, live, at);
        const auto full = portfolioDayHistory(history, {}, selection, live, at);
        for (const auto* result : {&filtered, &full}) {
            QVERIFY2(result->comparison.available, qPrintable(result->comparison.reason));
            QCOMPARE(result->comparison.difference, difference);
            QCOMPARE(result->comparison.openingValue, includeGap ? QString("25") : QString("50"));
            QCOMPARE(result->comparison.closingValue, QString("100"));
            QCOMPARE(result->baseline.timeMs, includeGap ? gap.timeMs : older.timeMs);
            QCOMPARE(result->points.size(), includeGap ? 3 : 2);
            bool foundOlder = false, foundGap = false;
            for (const auto& point : result->points) {
                QVERIFY(point.timeMs >= Midnight);
                QVERIFY(point.timeMs <= at);
                foundGap = foundGap || point.timeMs == gap.timeMs;
                if (point.timeMs == older.timeMs) {
                    foundOlder = true;
                    QCOMPARE(point.value, QString("50"));
                    QVERIFY(!point.connectToPrevious);
                    if (includeGap) QVERIFY(point.reason.contains("자료가 없어"));
                    else QVERIFY(point.reason.isEmpty());
                }
            }
            QVERIFY(foundOlder);
            QCOMPARE(foundGap, includeGap);
            QCOMPARE(result->points.last().value, QString("100"));
            QVERIFY(result->points.last().connectToPrevious);
        }
    }

    void baselineIsActualFirstObservationAndCurrentIsCompared() {
        const qint64 now = At + 600000;
        const QList<AssetSnapshot> history{snap("A", At, "100"), snap("A", At + 300000, "125")};
        const auto day = portfolioDayHistory(history, {}, select({"A"}), current({snap("A", now, "150")}, now), now);
        QVERIFY2(day.comparison.available, qPrintable(day.comparison.reason));
        QCOMPARE(day.baseline.timeMs, At); QCOMPARE(day.baseline.policyVersion, ValuationPolicyVersion);
        QCOMPARE(day.comparison.difference, QString("50")); QCOMPARE(day.comparison.fromMs, At); QCOMPARE(day.comparison.toMs, now);
        QCOMPARE(day.points.size(), 3); // Two durable observations plus current, without a synthetic midnight point.
        QCOMPARE(day.points[0].timeMs, At); QVERIFY(!day.points[0].connectToPrevious);
        QVERIFY(day.points[1].connectToPrevious); QVERIFY(day.points[2].connectToPrevious);
        QCOMPARE(day.points[2].value, QString("150"));
        QCOMPARE(day.closingValuation.timeMs, now);
        QCOMPARE(day.closingValuation.netAssets, QString("150"));
        QCOMPARE(day.closingValuation.rows.first().priceEvidence, snap("A", now, "150").components.first().evidence);
    }
    void noTodayRecordHasAReasonAndNoFabricatedMidnightValue() {
        const auto day = portfolioDayHistory({snap("A", Midnight - 1, "100")}, {}, select({"A"}), current({snap("A", At, "150")}, At), At);
        QVERIFY(!day.comparison.available); QVERIFY(!day.comparison.reason.isEmpty());
        QVERIFY(!day.baseline.available); QVERIFY(day.baseline.valuation.rows.isEmpty());
        QCOMPARE(day.points.size(), 1); QCOMPARE(day.points[0].timeMs, At); QVERIFY(!day.points[0].connectToPrevious);
    }
    void oldAndFutureRecordsNeverEnterTheDay() {
        auto later = snap("B", At + 1, "999");
        const auto day = portfolioDayHistory({snap("B", Midnight - 1, "888"), snap("A", At - 60000, "100"), later}, {},
            select({"A"}), current({snap("A", At, "125")}, At), At);
        QVERIFY2(day.comparison.available, qPrintable(day.comparison.reason)); QCOMPARE(day.comparison.difference, QString("25"));
        QCOMPARE(day.points.size(), 2); QCOMPARE(day.points.first().timeMs, At - 60000);
    }
    void observedDeletionOrKeyReplacementRefusesRebasing_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"deleted", "binding"}) QTest::newRow(fault) << QString(fault);
    }
    void observedDeletionOrKeyReplacementRefusesRebasing() {
        QFETCH(QString, fault);
        QList<AssetSnapshot> history{snap("A", At - 600000, "100"), snap("A", At - 300000, "125")};
        if (fault == "deleted") history.prepend(snap("B", At - 600000, "900"));
        if (fault == "binding") history[0].connectionBinding = "OLD_BINDING";
        const auto day = portfolioDayHistory(history, {}, select({"A"}), current({snap("A", At, "150")}, At), At);
        QVERIFY(!day.comparison.available); QVERIFY(!day.comparison.reason.isEmpty()); QVERIFY(day.comparison.difference.isEmpty());
        QVERIFY(!day.baseline.available); QVERIFY(day.baseline.valuation.rows.isEmpty());
        for (const auto& point : day.points) { QVERIFY(!point.available); QVERIFY(!point.connectToPrevious); QVERIFY(point.value.isEmpty()); }
    }
    void newlySelectedConnectionWithoutJointHistoryCannotCompare() {
        const auto day = portfolioDayHistory({snap("A", At - 60000, "100")}, {}, select({"A", "B"}),
            current({snap("A", At, "150"), snap("B", At, "10")}, At), At);
        QVERIFY(!day.comparison.available); QVERIFY(!day.comparison.reason.isEmpty()); QVERIFY(day.comparison.difference.isEmpty());
    }
    void laterConnectionDoesNotSilentlyStartANewDailyBaseline() {
        const QList<AssetSnapshot> history{snap("A", At - 600000, "100"),
            snap("A", At - 300000, "110"), snap("B", At - 300000, "50")};
        const auto day = portfolioDayHistory(history, {}, select({"A", "B"}),
            current({snap("A", At, "125"), snap("B", At, "75")}, At), At);
        QVERIFY(!day.comparison.available); QVERIFY(!day.comparison.reason.isEmpty());
        QVERIFY(!day.baseline.available); QVERIFY(day.comparison.difference.isEmpty());
    }
    void todayReceivedQuoteMayHaveAFreshSourceTimeBeforeMidnight() {
        auto before = snap("A", Midnight + 60000, "1"), after = snap("A", Midnight + 120000, "2");
        before.components[0].asset = after.components[0].asset = "USDT";
        AssetQuote quote{"USDT", "KRW", "1400", evidence("public:upbit:KRW-USDT", Midnight - 1000)};
        quote.evidence.receivedAtMs = Midnight + 1000;
        const auto day = portfolioDayHistory({before}, {quote}, select({"A"}), current({after}, after.timeMs, {quote}), after.timeMs);
        QVERIFY2(day.comparison.available, qPrintable(day.comparison.reason));
        QCOMPARE(day.baseline.timeMs, before.timeMs); QCOMPARE(day.comparison.difference, QString("1400"));
    }
    void asynchronousRowsWithinTheJointFreshnessWindowAreValid() {
        const auto day = portfolioDayHistory({snap("A", At - 90000, "100"), snap("B", At - 60000, "50")}, {}, select({"A", "B"}),
            current({snap("A", At, "125"), snap("B", At, "75")}, At), At);
        QVERIFY2(day.comparison.available, qPrintable(day.comparison.reason)); QCOMPARE(day.baseline.timeMs, At - 60000);
        QCOMPARE(day.comparison.difference, QString("50"));
    }
    void samePartialCoverageRemainsPartial() {
        auto before = snap("A", At - 60000, "100"), after = snap("A", At, "125"); addUnpriced(before); addUnpriced(after);
        const auto day = portfolioDayHistory({before}, {}, select({"A"}), current({after}, At), At);
        QVERIFY2(day.comparison.available, qPrintable(day.comparison.reason)); QVERIFY(!day.comparison.complete);
        QCOMPARE(day.comparison.difference, QString("25")); QVERIFY(day.comparison.label.contains("부분합"));
        for (const auto& point : day.points) { QVERIFY(point.available); QVERIFY(!point.complete); }
    }
    void changedPriceCoverageBreaksTheLine() {
        auto partial = snap("A", At - 300000, "110"); addUnpriced(partial);
        const auto day = portfolioDayHistory({snap("A", At - 600000, "100"), partial}, {}, select({"A"}), current({snap("A", At, "125")}, At), At);
        QVERIFY(day.comparison.available); QCOMPARE(day.points.size(), 3);
        QVERIFY(day.points[1].available); QVERIFY(!day.points[1].connectToPrevious); QVERIFY(!day.points[1].reason.isEmpty());
        QVERIFY(!day.points[2].connectToPrevious); QVERIFY(!day.points[2].reason.isEmpty());
    }
    void failedAndMissingObservationsBreakTheLine() {
        auto failed = snap("A", At - 300000, "110"); failed.balanceComplete = false;
        const auto day = portfolioDayHistory({snap("A", At - 600000, "100"), failed}, {}, select({"A"}), current({snap("A", At, "125")}, At), At);
        QVERIFY(day.comparison.available); QCOMPARE(day.points.size(), 3); QVERIFY(!day.points[1].available);
        QVERIFY(!day.points[1].reason.isEmpty()); QVERIFY(!day.points[1].connectToPrevious); QVERIFY(!day.points[2].connectToPrevious);
        const auto gap = portfolioDayHistory({snap("A", At - JointHistoryIntervalMs - DisplayFreshnessMs - 1, "100")}, {},
            select({"A"}), current({snap("A", At, "125")}, At), At);
        QVERIFY(gap.comparison.available); QVERIFY(!gap.points.last().connectToPrevious); QVERIFY(!gap.points.last().reason.isEmpty());
    }
    void aFutureReceivedQuoteCannotRevalueAnEarlierCheckpoint() {
        auto before = snap("A", At - 60000, "100"), after = snap("A", At, "125");
        auto beforeCoin = before.components.first(); beforeCoin.id = "coin"; beforeCoin.asset = "USDT"; beforeCoin.quantity = "1";
        auto afterCoin = beforeCoin; afterCoin.evidence = after.evidence; before.components.append(beforeCoin); after.components.append(afterCoin);
        AssetQuote late{"USDT", "KRW", "1400", evidence("public:upbit:KRW-USDT", At - 60000)};
        late.evidence.receivedAtMs = At;
        const auto day = portfolioDayHistory({before}, {late}, select({"A"}), current({after}, At, {late}), At);
        QVERIFY(day.baseline.available); QVERIFY(!day.baseline.valuation.complete); QCOMPARE(day.baseline.valuation.knownSubtotal, QString("100"));
        QVERIFY(!day.comparison.available); QVERIFY(day.comparison.difference.isEmpty()); QVERIFY(!day.points.last().connectToPrevious);
    }
    void baselineSkipsFullyUnvaluedRecordsButPreservesPartialOnes() {
        auto missing = snap("A", At - 600000, "1"); missing.components[0].asset = "USDT";
        auto priced = snap("A", At - 300000, "1"); priced.components[0].asset = "USDT";
        AssetQuote quote{"USDT", "KRW", "1400", evidence("public:upbit:KRW-USDT", At - 300000)};
        const auto baseline = dailyBaseline({missing, priced}, {quote}, select({"A"}), At);
        QVERIFY2(baseline.available, qPrintable(baseline.reason)); QCOMPARE(baseline.timeMs, At - 300000);
        QCOMPARE(baseline.valuation.netAssets, QString("1400"));
    }
    void currentAsOfMustMatchTheRequestedDayTime() {
        const auto day = portfolioDayHistory({snap("A", At - 60000, "100")}, {}, select({"A"}), current({snap("A", At + 1, "125")}, At + 1), At);
        QVERIFY(!day.comparison.available); QVERIFY(!day.comparison.reason.isEmpty());
        QVERIFY(day.comparison.closingValue.isEmpty()); QVERIFY(day.comparison.difference.isEmpty());
    }
    void kstMidnightRollsOverIndependentlyOfUtcDate() {
        QCOMPARE(portfolioHistoryDayStart(Midnight + 3600000), Midnight);
        QCOMPARE(portfolioHistoryDayStart(Midnight + 24 * 3600000 + 1), Midnight + 24 * 3600000);
        QCOMPARE(portfolioHistoryDayStart(Midnight - 1), Midnight - 24 * 3600000);
    }
    void futureComponentEvidenceDoesNotEnterTheCheckpointSubtotal() {
        auto holding = snap("A", At, "100");
        auto future = holding.components.first(); future.id = "future-cash"; future.quantity = "999";
        future.evidence.sourceTimeMs = At + 1; holding.components.append(future);
        const auto checkpoint = checkpointAt({holding}, {}, select({"A"}), At);
        QVERIFY(checkpoint.available); QVERIFY(!checkpoint.valuation.complete);
        QCOMPARE(checkpoint.valuation.knownSubtotal, QString("100"));
        QVERIFY(checkpoint.valuation.rows.last().value.isEmpty());
        QVERIFY(!checkpoint.valuation.rows.last().reason.isEmpty());
    }
    void matchesSharedPythonRecalculationFixtures() {
        const auto fixturePath = QFINDTESTDATA("../../../scripts/keefi-recon/g8-history-fixture.json");
        QVERIFY(!fixturePath.isEmpty()); QString error;
        const auto fixtures = keefi::test::loadG8HistoryFixtures(fixturePath, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(fixtures.size(), 4);
        QJsonObject actualCases;
        for (const auto& fixture : fixtures) {
            const auto currentValue = current(fixture.currentSnapshots, fixture.nowMs, fixture.quotes, fixture.selection.excludedAssets);
            const auto day = portfolioDayHistory(fixture.history, fixture.quotes, fixture.selection, currentValue, fixture.nowMs);
            const auto& compared = day.comparison;
            const QJsonObject actual{{"available", compared.available}, {"complete", compared.complete},
                {"baseline_time_ms", day.baseline.timeMs}, {"opening_value", compared.openingValue},
                {"closing_value", compared.closingValue}, {"difference", compared.difference}};
            QVERIFY2(actual == fixture.expected, qPrintable(fixture.name + ": " + QString::fromUtf8(QJsonDocument(actual).toJson(QJsonDocument::Compact))));
            actualCases.insert(fixture.name, actual);
        }
        const auto exportPath = qEnvironmentVariable("KEEFI_G8_CROSSCHECK_OUTPUT");
        if (!exportPath.isEmpty()) {
            QFile output(exportPath); QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QVERIFY(output.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
            const auto bytes = QJsonDocument(QJsonObject{{"format", "keefi-g8-history-crosscheck/1"}, {"cases", actualCases}}).toJson();
            QCOMPARE(output.write(bytes), bytes.size());
        }
    }
};
QTEST_GUILESS_MAIN(PortfolioHistoryTest)
#include "tst_portfolio_history.moc"
