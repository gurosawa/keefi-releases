#include "keefi/PortfolioContribution.h"
#include <QTest>
#include <algorithm>

using namespace keefi;
namespace {
constexpr qint64 At = 1800000000000LL;
struct Endpoint { AssetValuationResult value; QList<AssetSnapshot> snapshots; };
Endpoint endpoint(qint64 at, const QString& amount, const QString& provider = "binance", const QString& market = "spot",
    AssetComponentKind kind = AssetComponentKind::Holding) {
    Endpoint e;
    e.value.currency = "KRW"; e.value.timeMs = at; e.value.complete = true;
    e.value.accountScopes = {"FAKE_ACCOUNT"}; e.value.netAssets = e.value.knownSubtotal = amount;
    AssetValueRow row; row.accountScope = "FAKE_ACCOUNT"; row.componentId = "holding:BTC"; row.asset = "BTC";
    row.quantity = "1"; row.value = amount; row.currency = "KRW"; row.included = true;
    e.value.rows = {row};
    AssetSnapshot s; s.id = "FAKE_SNAPSHOT"; s.accountScope = row.accountScope; s.provider = provider;
    s.market = market; s.timeMs = at; s.balanceComplete = true;
    s.evidence = {"Synthetic source metadata", "https://example.test/account", s.accountScope, at, at, true};
    AssetComponent c; c.id = row.componentId; c.asset = row.asset; c.quantity = row.quantity; c.kind = kind;
    c.evidence = s.evidence; s.components = {c}; e.snapshots = {s};
    return e;
}
PortfolioContributions contributions(const Endpoint& a, const Endpoint& b) {
    return portfolioContributions(a.value, "test-v2", a.snapshots, b.value, "test-v2", b.snapshots);
}
void identify(Endpoint& e, const QString& scope, const QString& id, const QString& asset) {
    e.value.accountScopes = {scope};
    auto& row = e.value.rows[0]; row.accountScope = scope; row.componentId = id; row.asset = asset;
    auto& snapshot = e.snapshots[0]; snapshot.accountScope = scope; snapshot.evidence.scope = scope;
    auto& component = snapshot.components[0]; component.id = id; component.asset = asset; component.evidence.scope = scope;
}
void append(Endpoint& all, const Endpoint& extra, const QString& total) {
    all.value.accountScopes.append(extra.value.accountScopes); all.value.rows.append(extra.value.rows);
    all.value.netAssets = all.value.knownSubtotal = total; all.snapshots.append(extra.snapshots);
}
QString groupChange(const PortfolioContributions& result, PortfolioContributionClass assetClass) {
    for (const auto& group : result.groups) if (group.assetClass == assetClass) return group.difference;
    return {};
}
}
class PortfolioContributionTest final : public QObject {
    Q_OBJECT
  private slots:
    void explicitBithumbSpotHoldingBelongsToCoin() {
        const auto opening=endpoint(At,"100","bithumb","spot",AssetComponentKind::Holding);
        const auto closing=endpoint(At+1,"125","bithumb","spot",AssetComponentKind::Holding);
        const auto result=contributions(opening,closing);
        QVERIFY(result.available);QCOMPARE(result.comparison.difference,QString("25"));
        QCOMPARE(result.rows.size(),1);QCOMPARE(result.rows.first().assetClass,PortfolioContributionClass::Coin);
        QCOMPARE(groupChange(result,PortfolioContributionClass::Coin),QString("25"));
        QVERIFY(groupChange(result,PortfolioContributionClass::Unclassified).isEmpty());
    }
    void splitsObservedChangeWithoutRoundingOrCallingItProfit() {
        const auto a = endpoint(At, "100.000000000000000001");
        const auto b = endpoint(At + 1, "150.000000000000000003");
        const auto result = contributions(a, b);
        QVERIFY(result.available); QVERIFY(result.comparison.available); QVERIFY(result.comparison.complete);
        QCOMPARE(result.comparison.difference, QString("50.000000000000000002"));
        QCOMPARE(result.rows.size(), 1); QCOMPARE(result.groups.size(), 1);
        QCOMPARE(result.rows[0].assetClass, PortfolioContributionClass::Coin);
        QCOMPARE(result.rows[0].difference, QString("50.000000000000000002"));
        QCOMPARE(result.groups[0].difference, result.comparison.difference);
        QCOMPARE(result.groups[0].openingValue, a.value.knownSubtotal);
        QCOMPARE(result.groups[0].closingValue, b.value.knownSubtotal);
        QVERIFY(result.comparison.label.contains("입출금 포함 가능"));
        QVERIFY(!result.comparison.label.contains("수익"));
        QCOMPARE(portfolioContributionClassLabel(PortfolioContributionClass::Coin), QString("코인"));
    }
    void classifiesExplicitMarketsAndKindsWithFuturesInTheResidual() {
        auto a = endpoint(At, "100"), b = endpoint(At + 1, "120");
        auto add = [&](const QString& scope, const QString& asset, const QString& from, const QString& to,
                       const QString& provider, const QString& market, AssetComponentKind kind,
                       const QString& fromTotal, const QString& toTotal) {
            auto x = endpoint(At, from, provider, market, kind), y = endpoint(At + 1, to, provider, market, kind);
            identify(x, scope, "component", asset); identify(y, scope, "component", asset);
            append(a, x, fromTotal); append(b, y, toTotal);
        };
        add("STOCK", "005930", "200", "190", "toss", "domestic_equity", AssetComponentKind::Holding, "300", "310");
        add("CASH", "KRW", "50", "55", "toss", "domestic_equity", AssetComponentKind::Cash, "350", "365");
        add("FUTURES", "USDT", "300", "330", "binance", "usdm_futures", AssetComponentKind::Equity, "650", "695");
        add("UNKNOWN", "USD", "10", "8", "unknown", "unknown", AssetComponentKind::Holding, "660", "703");
        add("WALLET", "solana:FAKE_MINT", "100", "110", "phantom", "solana", AssetComponentKind::Holding, "760", "813");
        std::reverse(b.value.rows.begin(), b.value.rows.end());
        std::reverse(b.snapshots.begin(), b.snapshots.end());
        const auto result = contributions(a, b);
        QVERIFY(result.available); QCOMPARE(result.comparison.difference, QString("53"));
        QCOMPARE(groupChange(result, PortfolioContributionClass::Coin), QString("30"));
        QCOMPARE(groupChange(result, PortfolioContributionClass::DomesticStock), QString("-10"));
        QCOMPARE(groupChange(result, PortfolioContributionClass::Cash), QString("5"));
        QCOMPARE(groupChange(result, PortfolioContributionClass::Unclassified), QString("28"));
        QVERIFY(groupChange(result, PortfolioContributionClass::UsStock).isEmpty());
        QStringList differences;
        for (const auto& group : result.groups) differences.append(group.difference);
        QCOMPARE(sumAssetAmounts(differences), QString("53"));
        QCOMPARE(result.rows.size(), 6);
        for (const auto& row : result.rows) if (row.accountScope == "FUTURES") {
            QCOMPARE(row.assetClass, PortfolioContributionClass::Unclassified);
            QVERIFY(row.reason.contains("equity"));
        }
    }
    void untrustedSourceMetadataRemainsAnExactResidual_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& fault : QStringList{"missing-opening", "missing-closing", "wrong-asset", "wrong-scope", "wrong-id",
             "duplicate-snapshot", "duplicate-component", "duplicate-id-other-asset", "quantity-mismatch", "future-snapshot",
             "future-receipt", "future-component", "missing-evidence", "changed-provider", "fiat-holding"})
            QTest::newRow(qPrintable(fault)) << fault;
    }
    void untrustedSourceMetadataRemainsAnExactResidual() {
        QFETCH(QString, fault);
        auto a = endpoint(At, "100"), b = endpoint(At + 1, "125");
        if (fault == "missing-opening") a.snapshots.clear();
        if (fault == "missing-closing") b.snapshots.clear();
        if (fault == "wrong-asset") b.snapshots[0].components[0].asset = "OTHER_CANONICAL_ASSET";
        if (fault == "wrong-scope") b.snapshots[0].accountScope = "OTHER_ACCOUNT";
        if (fault == "wrong-id") b.snapshots[0].components[0].id = "OTHER_COMPONENT";
        if (fault == "duplicate-snapshot") b.snapshots.append(b.snapshots[0]);
        if (fault == "duplicate-component") b.snapshots[0].components.append(b.snapshots[0].components[0]);
        if (fault == "duplicate-id-other-asset") {
            auto duplicate = b.snapshots[0].components[0]; duplicate.asset = "OTHER_ASSET";
            b.snapshots[0].components.append(duplicate);
        }
        if (fault == "quantity-mismatch") b.snapshots[0].components[0].quantity = "2";
        if (fault == "future-snapshot") ++b.snapshots[0].timeMs;
        if (fault == "future-receipt") ++b.snapshots[0].evidence.receivedAtMs;
        if (fault == "future-component") ++b.snapshots[0].components[0].evidence.sourceTimeMs;
        if (fault == "missing-evidence") b.snapshots[0].components[0].evidence.source.clear();
        if (fault == "changed-provider") b.snapshots[0].provider = "upbit";
        if (fault == "fiat-holding") {
            identify(a, "FAKE_ACCOUNT", "holding:KRW", "KRW"); identify(b, "FAKE_ACCOUNT", "holding:KRW", "KRW");
        }
        const auto result = contributions(a, b);
        QVERIFY(result.available); QVERIFY(result.comparison.available);
        QCOMPARE(result.comparison.difference, QString("25"));
        QCOMPARE(result.rows.size(), 1); QCOMPARE(result.groups.size(), 1);
        QCOMPARE(result.rows[0].assetClass, PortfolioContributionClass::Unclassified);
        QVERIFY(!result.rows[0].reason.isEmpty());
        QCOMPARE(groupChange(result, PortfolioContributionClass::Unclassified), QString("25"));
    }
    void doesNotInferStockCountryFromTickerOrValuationCurrency() {
        for (const QString& asset : {QString("005930"), QString("AAPL"), QString("USD")}) {
            auto a = endpoint(At, "100", "toss", "unknown"), b = endpoint(At + 1, "125", "toss", "unknown");
            identify(a, "FAKE_ACCOUNT", "component", asset); identify(b, "FAKE_ACCOUNT", "component", asset);
            const auto result = contributions(a, b);
            QVERIFY(result.available); QCOMPARE(result.rows[0].assetClass, PortfolioContributionClass::Unclassified);
        }
    }
    void preservesPartialAndExcludedComparisonWithoutAddingReferenceOrPositionValues() {
        auto a = endpoint(At, "100"), b = endpoint(At + 1, "125");
        for (auto* point : {&a, &b}) {
            point->value.complete = false; point->value.netAssets.clear();
            auto missing = point->value.rows[0]; missing.componentId = "missing"; missing.asset = "MISSING";
            missing.value.clear(); missing.reason = "가격 없음"; missing.referenceValue = "99999";
            auto excluded = missing; excluded.componentId = "excluded"; excluded.asset = "EXCLUDED";
            excluded.included = false; excluded.excludedByUser = true;
            auto position = missing; position.componentId = "position"; position.asset = "BTCUSDT";
            position.included = false; position.providerUnrealizedPnl = "88888";
            point->value.rows.append({missing, excluded, position});
        }
        const auto result = contributions(a, b);
        QVERIFY(result.available); QVERIFY(!result.comparison.complete);
        QCOMPARE(result.comparison.excludedRowCount, qsizetype(1));
        QCOMPARE(result.comparison.label, QString("동일 평가 대상 부분합 증감 · 1항목 제외 · 입출금 포함 가능"));
        QCOMPARE(result.comparison.difference, QString("25"));
        QCOMPARE(result.rows.size(), 1); QCOMPARE(result.groups.size(), 1);
        QCOMPARE(result.groups[0].difference, QString("25"));
    }
    void comparisonFailureDoesNotProduceContributionNumbers() {
        auto a = endpoint(At, "100"), b = endpoint(At + 1, "125");
        identify(b, "CHANGED_ACCOUNT", "holding:BTC", "BTC");
        const auto result = contributions(a, b);
        QVERIFY(!result.available); QVERIFY(!result.comparison.available);
        QVERIFY(!result.reason.isEmpty()); QVERIFY(result.rows.isEmpty()); QVERIFY(result.groups.isEmpty());
        const auto policyChange = portfolioContributions(a.value, "v1", a.snapshots, a.value, "v2", a.snapshots);
        QVERIFY(!policyChange.available); QVERIFY(policyChange.rows.isEmpty());
    }
    void canonicalAssetsWithTheSameDisplayNameNeverMerge() {
        auto a = endpoint(At, "100", "phantom", "solana"), b = endpoint(At + 1, "120", "phantom", "solana");
        identify(a, "WALLET_A", "mint-one", "solana:FAKE_MINT_1"); identify(b, "WALLET_A", "mint-one", "solana:FAKE_MINT_1");
        auto x = endpoint(At, "200", "phantom", "solana"), y = endpoint(At + 1, "190", "phantom", "solana");
        identify(x, "WALLET_B", "mint-two", "solana:FAKE_MINT_2"); identify(y, "WALLET_B", "mint-two", "solana:FAKE_MINT_2");
        for (auto* e : {&a, &b, &x, &y}) e->snapshots[0].components[0].displaySymbol = "STONK";
        append(a, x, "300"); append(b, y, "310");
        const auto result = contributions(a, b);
        QVERIFY(result.available); QCOMPARE(result.rows.size(), 2);
        QCOMPARE(result.rows[0].difference, QString("20")); QCOMPARE(result.rows[1].difference, QString("-10"));
        QCOMPARE(groupChange(result, PortfolioContributionClass::Coin), QString("10"));
    }
    void refusesOverflowingContributionsEvenWhenTotalCancels_data() {
        QTest::addColumn<bool>("rowOverflow");
        QTest::newRow("row-change-overflow") << true;
        QTest::newRow("class-subtotal-overflow") << false;
    }
    void refusesOverflowingContributionsEvenWhenTotalCancels() {
        QFETCH(bool, rowOverflow);
        const QString large(80, '9');
        auto a = endpoint(At, rowOverflow ? "-" + large : large), b = endpoint(At + 1, large);
        auto x = endpoint(At, large), y = endpoint(At + 1, rowOverflow ? "-" + large : large);
        identify(x, "OTHER", "other", "ETH"); identify(y, "OTHER", "other", "ETH");
        append(a, x, "0"); append(b, y, "0");
        if (!rowOverflow) {
            auto liabilityA = endpoint(At, "-" + large, "binance", "spot", AssetComponentKind::Liability);
            auto liabilityB = endpoint(At + 1, "-" + large, "binance", "spot", AssetComponentKind::Liability);
            identify(liabilityA, "DEBT_1", "debt", "USDT"); identify(liabilityB, "DEBT_1", "debt", "USDT");
            append(a, liabilityA, "0"); append(b, liabilityB, "0");
            identify(liabilityA, "DEBT_2", "debt", "USDT"); identify(liabilityB, "DEBT_2", "debt", "USDT");
            append(a, liabilityA, "0"); append(b, liabilityB, "0");
        }
        const auto result = contributions(a, b);
        QVERIFY(result.comparison.available); QCOMPARE(result.comparison.difference, QString("0"));
        QVERIFY(!result.available); QVERIFY(result.rows.isEmpty()); QVERIFY(result.groups.isEmpty());
        QVERIFY(result.reason.contains("정밀도"));
    }
    void genuineEmptyAccountsRemainZeroWithoutInventingRows() {
        auto a = endpoint(At, "0"), b = endpoint(At + 1, "0");
        a.value.rows.clear(); b.value.rows.clear(); a.snapshots[0].components.clear(); b.snapshots[0].components.clear();
        const auto result = contributions(a, b);
        QVERIFY(result.available); QCOMPARE(result.comparison.difference, QString("0"));
        QVERIFY(result.rows.isEmpty()); QVERIFY(result.groups.isEmpty());
    }
};
QTEST_APPLESS_MAIN(PortfolioContributionTest)
#include "tst_portfolio_contribution.moc"
