#include "keefi/PortfolioComparison.h"
#include <QTest>
#include <algorithm>

using namespace keefi;
namespace {
constexpr qint64 At = 1800000000000LL;
AssetValuationResult valued(qint64 at, const QString& amount) {
    AssetValuationResult result;
    result.currency = "KRW"; result.timeMs = at; result.complete = true;
    result.accountScopes = {"FAKE_ACCOUNT"}; result.knownSubtotal = result.netAssets = amount;
    AssetValueRow row;
    row.accountScope = "FAKE_ACCOUNT"; row.componentId = "holding:BTC"; row.asset = "BTC";
    row.quantity = "1"; row.value = amount; row.currency = "KRW"; row.included = true;
    result.rows = {row};
    return result;
}
PortfolioComparison compare(const AssetValuationResult& a, const AssetValuationResult& b) {
    return comparePortfolioValues(a, "test-policy-v2", b, "test-policy-v2");
}
} // namespace

class PortfolioComparisonTest final : public QObject {
    Q_OBJECT
  private slots:
    void comparesObservedValuesWithoutCallingQuantityChangesProfit() {
        const auto opening = valued(At, "100.000000000000000001");
        auto closing = valued(At + 300000, "150.000000000000000003");
        closing.rows[0].quantity = "2";
        const auto result = compare(opening, closing);
        QVERIFY(result.available); QVERIFY(result.complete);
        QCOMPARE(result.difference, QString("50.000000000000000002"));
        QCOMPARE(result.openingValue, opening.knownSubtotal); QCOMPARE(result.closingValue, closing.knownSubtotal);
        QCOMPARE(result.fromMs, At); QCOMPARE(result.toMs, At + 300000); QCOMPARE(result.currency, QString("KRW"));
        QCOMPARE(result.label,QString("관측 평가액 증감 · 입출금 포함 가능")); QVERIFY(!result.label.contains("수익"));
        QVERIFY(result.reason.isEmpty());
    }
    void preservesSameMissingSubsetWithoutInventingZeroPrices() {
        auto opening = valued(At, "100"); auto closing = valued(At + 1, "125");
        AssetValueRow missing = opening.rows[0]; missing.componentId = "holding:MISSING"; missing.asset = "MISSING";
        missing.value.clear(); missing.referenceValue = "99"; missing.reason = "평가가격 자료 부족";
        for (auto* point : {&opening, &closing}) {
            point->complete = false; point->netAssets.clear(); point->rows.append(missing);
        }
        closing.rows[1].quantity = "200"; closing.rows[1].referenceValue = "99999";
        const auto result = compare(opening, closing);
        QVERIFY(result.available); QVERIFY(!result.complete); QCOMPARE(result.difference, QString("25"));
        QVERIFY(result.label.contains("동일 범위 부분합"));
        QVERIFY(opening.rows[1].value.isEmpty()); QCOMPARE(opening.rows[1].referenceValue, QString("99"));
    }
    void incompatibleOrMalformedInputsStayUnavailable_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& fault : QStringList{"same-time", "reversed-time", "missing-time", "currency", "non-krw", "scope-change",
             "duplicate-scope", "empty-scope", "empty-scopes", "row-account", "row-id", "row-asset", "row-currency",
             "duplicate-row", "duplicate-component-other-asset", "missing-row", "new-row", "included-change", "excluded-change",
             "contradictory-included", "invalid-amount", "missing-value", "sum-mismatch", "net-mismatch", "partial-net",
             "complete-change", "scope-issues", "empty-policy", "different-policy", "whitespace-policy", "amount-overflow"})
            QTest::newRow(qPrintable(fault)) << fault;
    }
    void incompatibleOrMalformedInputsStayUnavailable() {
        QFETCH(QString, fault);
        auto opening = valued(At, "100"), closing = valued(At + 1, "125");
        QString fromPolicy = "v2", toPolicy = "v2";
        if (fault == "same-time") closing.timeMs = At;
        if (fault == "reversed-time") closing.timeMs = At - 1;
        if (fault == "missing-time") opening.timeMs = 0;
        if (fault == "currency") closing.currency = "USD";
        if (fault == "non-krw") { opening.currency = closing.currency = "USD"; opening.rows[0].currency = closing.rows[0].currency = "USD"; }
        if (fault == "scope-change") { closing.accountScopes = {"OTHER_ACCOUNT"}; closing.rows[0].accountScope = "OTHER_ACCOUNT"; }
        if (fault == "duplicate-scope") closing.accountScopes.append("FAKE_ACCOUNT");
        if (fault == "empty-scope") closing.accountScopes.append("");
        if (fault == "empty-scopes") closing.accountScopes.clear();
        if (fault == "row-account") closing.rows[0].accountScope = "OTHER_ACCOUNT";
        if (fault == "row-id") closing.rows[0].componentId.clear();
        if (fault == "row-asset") closing.rows[0].asset = "OTHER_MINT";
        if (fault == "row-currency") closing.rows[0].currency = "USD";
        if (fault == "duplicate-row") closing.rows.append(closing.rows[0]);
        if (fault == "duplicate-component-other-asset") { auto duplicate = closing.rows[0]; duplicate.asset = "OTHER"; closing.rows.append(duplicate); }
        if (fault == "missing-row") { closing.rows.clear(); closing.knownSubtotal = closing.netAssets = "0"; }
        if (fault == "new-row") { auto extra = closing.rows[0]; extra.componentId = "holding:OTHER"; extra.asset = "OTHER"; extra.value = "0"; closing.rows.append(extra); }
        if (fault == "included-change") { closing.rows[0].included = false; closing.rows[0].value.clear(); closing.knownSubtotal = closing.netAssets = "0"; }
        if (fault == "excluded-change") { closing.rows[0].included = false; closing.rows[0].excludedByUser = true; closing.rows[0].value.clear(); closing.knownSubtotal = closing.netAssets = "0"; }
        if (fault == "contradictory-included") closing.rows[0].excludedByUser = true;
        if (fault == "invalid-amount") closing.rows[0].value = "NaN";
        if (fault == "missing-value") closing.rows[0].value.clear();
        if (fault == "sum-mismatch") closing.knownSubtotal = closing.netAssets = "126";
        if (fault == "net-mismatch") closing.netAssets = "0";
        if (fault == "partial-net") { opening.complete = closing.complete = false; }
        if (fault == "complete-change") { closing.complete = false; closing.netAssets.clear(); }
        if (fault == "scope-issues") { opening.issues = closing.issues = {"잔고 조회 범위 또는 평가 시각 미완"}; opening.complete = closing.complete = false; opening.netAssets.clear(); closing.netAssets.clear(); }
        if (fault == "empty-policy") fromPolicy.clear();
        if (fault == "different-policy") toPolicy = "v3";
        if (fault == "whitespace-policy") fromPolicy = toPolicy = "  ";
        if (fault == "amount-overflow") closing.rows[0].value = closing.knownSubtotal = closing.netAssets = QString(81, '9');
        const auto result = comparePortfolioValues(opening, fromPolicy, closing, toPolicy);
        QVERIFY(!result.available); QVERIFY(!result.complete); QVERIFY(!result.reason.isEmpty());
        QVERIFY(result.openingValue.isEmpty()); QVERIFY(result.closingValue.isEmpty()); QVERIFY(result.difference.isEmpty());
    }
    void noValuedRowsIsNotAnObservedPartialZero() {
        auto opening = valued(At, "0"), closing = valued(At + 1, "0");
        for (auto* point : {&opening, &closing}) {
            point->complete = false; point->netAssets.clear(); point->rows[0].value.clear();
            point->rows[0].reason = "가격 없음";
        }
        const auto result = compare(opening, closing);
        QVERIFY(!result.available); QVERIFY(result.difference.isEmpty()); QVERIFY(!result.reason.isEmpty());
        opening = valued(At, "0"); closing = valued(At + 1, "0");
        opening.rows.clear(); closing.rows.clear();
        const auto emptyAccount = compare(opening, closing);
        QVERIFY(emptyAccount.available); QVERIFY(emptyAccount.complete); QCOMPARE(emptyAccount.difference, QString("0"));
    }
    void coverageChangesDoNotLookLikeMarketMovement() {
        auto opening = valued(At, "100"), closing = valued(At + 1, "100");
        AssetValueRow missing = opening.rows[0]; missing.componentId = "unknown"; missing.asset = "UNKNOWN";
        missing.value.clear(); missing.reason = "가격 없음";
        for (auto* point : {&opening, &closing}) { point->complete = false; point->netAssets.clear(); point->rows.append(missing); }
        closing.rows[1].value = "0";
        QVERIFY(!compare(opening, closing).available);
        closing.rows[1].value.clear(); closing.rows[1].included = false; closing.rows[1].excludedByUser = true;
        QVERIFY(!compare(opening, closing).available);
    }
    void negativeAndZeroValuesRemainExactAndOrderIndependent() {
        auto opening = valued(At, "-5.000"), closing = valued(At + 1, "-8.125");
        auto extra = opening.rows[0]; extra.componentId = "zero"; extra.asset = "ZERO"; extra.value = "-0.000";
        opening.rows.append(extra); closing.rows.prepend(extra);
        const auto result = compare(opening, closing);
        QVERIFY(result.available); QCOMPARE(result.openingValue, QString("-5")); QCOMPARE(result.difference, QString("-3.125"));
        const auto zero = compare(valued(At, "-0.000"), valued(At + 1, "0.000"));
        QVERIFY(zero.available); QCOMPARE(zero.difference, QString("0"));
    }
    void differenceOverflowStaysUnavailable() {
        const auto result = compare(valued(At, "-" + QString(80, '9')), valued(At + 1, QString(80, '9')));
        QVERIFY(!result.available); QVERIFY(result.difference.isEmpty()); QVERIFY(!result.reason.isEmpty());
    }
    void identicalExplicitExclusionsAndAccountOrderDoNotChangeTheComparedSubset() {
        auto opening = valued(At, "100"), closing = valued(At + 1, "125");
        auto excluded = opening.rows[0]; excluded.componentId = "holding:EXCLUDED"; excluded.asset = "solana:FAKE_MINT";
        excluded.included = false; excluded.excludedByUser = true; excluded.value.clear(); excluded.quantity.clear();
        for (auto* point : {&opening, &closing}) { point->accountScopes.append("OTHER_EMPTY_ACCOUNT"); point->rows.append(excluded); }
        std::reverse(closing.accountScopes.begin(), closing.accountScopes.end());
        std::reverse(closing.rows.begin(), closing.rows.end());
        const auto result = compare(opening, closing);
        QVERIFY(result.available); QCOMPARE(result.difference, QString("25"));
        QVERIFY(result.label.contains("평가 대상 합계 증감"));
        QCOMPARE(result.excludedRowCount, qsizetype(1)); QVERIFY(result.label.contains("1항목 제외"));
        QCOMPARE(opening.rows[1].excludedByUser, true); QVERIFY(opening.rows[1].value.isEmpty());

        auto missing = opening.rows[0]; missing.componentId = "missing"; missing.asset = "MISSING";
        missing.value.clear(); missing.reason = "가격 없음";
        for (auto* point : {&opening, &closing}) { point->rows.append(missing); point->complete = false; point->netAssets.clear(); }
        const auto partial = compare(opening, closing);
        QVERIFY(partial.available); QVERIFY(!partial.complete); QCOMPARE(partial.difference, QString("25"));
        QCOMPARE(partial.excludedRowCount, qsizetype(1)); QVERIFY(partial.label.contains("동일 평가 대상 부분합 증감"));
    }
    void consumesActualEvaluatorOutputIncludingEquityDisplayRows() {
        AssetSnapshot snapshot; snapshot.id = "FAKE_SNAPSHOT"; snapshot.accountScope = "FAKE_ACCOUNT";
        snapshot.timeMs = At; snapshot.balanceComplete = true;
        snapshot.evidence = {"Synthetic official observation", "https://example.test/account", "FAKE_ACCOUNT", At, At, true};
        AssetComponent equity; equity.id = "futures:KRW"; equity.asset = "KRW"; equity.quantity = "100";
        equity.kind = AssetComponentKind::Equity; equity.evidence = snapshot.evidence;
        auto position = equity; position.id = "position:BTC"; position.asset = "BTC"; position.quantity.clear();
        position.kind = AssetComponentKind::PositionNotional;
        snapshot.components = {equity, position};
        const auto opening = AssetValuation::value({snapshot}, {}, "KRW", At, DisplayFreshnessMs);
        snapshot.timeMs += 1; snapshot.evidence.sourceTimeMs += 1; snapshot.evidence.receivedAtMs += 1;
        snapshot.components[0].quantity = "120"; snapshot.components[0].evidence = snapshot.evidence;
        const auto closing = AssetValuation::value({snapshot}, {}, "KRW", At + 1, DisplayFreshnessMs);
        QVERIFY(opening.complete); QVERIFY(closing.complete); QVERIFY(!opening.rows[1].included);
        const auto result = compare(opening, closing);
        QVERIFY(result.available); QVERIFY(result.complete); QCOMPARE(result.difference, QString("20"));
        QVERIFY(result.label.contains("입출금 포함 가능"));

        snapshot.components[0].kind = AssetComponentKind::Holding;
        snapshot.components[1].kind = AssetComponentKind::Holding;
        snapshot.components[1].quantity = "1"; snapshot.components[1].evidence = snapshot.evidence;
        const auto partialOpening = AssetValuation::value({snapshot}, {}, "KRW", At + 1, DisplayFreshnessMs);
        snapshot.components[0].quantity = "130";
        const auto partialClosing = AssetValuation::value({snapshot}, {}, "KRW", At + 2, DisplayFreshnessMs);
        QVERIFY(!partialOpening.complete); QVERIFY(partialOpening.issues.isEmpty());
        const auto partialResult = compare(partialOpening, partialClosing);
        QVERIFY(partialResult.available); QVERIFY(!partialResult.complete); QCOMPARE(partialResult.difference, QString("10"));
    }
};
QTEST_APPLESS_MAIN(PortfolioComparisonTest)
#include "tst_portfolio_comparison.moc"
