#include "keefi/ProviderReconciliation.h"
#include "keefi/PortfolioController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/Vault.h"
#include "fixtures/ProviderReadFixture.h"
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;
namespace {
constexpr qint64 At = 1800000000000LL;
AssetEvidence evidence(const QString& source, const QString& url, const QString& scope, qint64 received = At - 1000) {
    return {source, url, scope, 0, received, true};
}
AssetSnapshot snapshot(const QString& value = "100") {
    AssetSnapshot result; result.id = "FAKE_SNAPSHOT"; result.connectionId = "FAKE_CONNECTION";
    result.accountScope = "FAKE_SCOPE"; result.provider = "binance"; result.environment = "live"; result.market = "usdm_futures";
    result.balanceComplete = true; result.timeMs = At - 1000;
    result.evidence = evidence("Binance official account", "https://fapi.binance.com/fapi/v3/account", result.accountScope);
    AssetComponent component; component.id = "futures:USDT"; component.asset = "USDT"; component.quantity = value;
    component.kind = AssetComponentKind::Equity; component.valuationGroup = "futures"; component.evidence = result.evidence;
    result.components.append(component);
    return result;
}
ProviderTotalObservation official(const AssetSnapshot& source, const QString& amount = "100") {
    const auto configEvidence = evidence("Binance official configuration", "https://fapi.binance.com/fapi/v1/accountConfig", source.accountScope);
    return captureBinanceUsdmProviderTotal(source, {{"totalMarginBalance", amount}, {"totalWalletBalance", "80"}},
                                          {{"multiAssetsMargin", false}}, configEvidence);
}
AssetQuote fx(const QString& price = "1000", qint64 sourceTime = At - 1000) {
    return {"USDT", "KRW", price, {"Upbit public ticker", "https://api.upbit.com/v1/ticker", "public:upbit:KRW-USDT", sourceTime, At - 500, true}};
}
} // namespace

class ProviderReconciliationTest final : public QObject {
    Q_OBJECT
  private slots:
    void publicFxClockSkewUsesTheSameBoundaryAsValuation_data() {
        QTest::addColumn<qint64>("leadMs");QTest::addColumn<bool>("accepted");
        for(qint64 lead:{0,3,19,35,1999,2000,2001})QTest::newRow(qPrintable(QString::number(lead)))<<lead<<(lead<=2000);
    }
    void publicFxClockSkewUsesTheSameBoundaryAsValuation() {
        QFETCH(qint64,leadMs);QFETCH(bool,accepted);
        const auto source=snapshot("101");auto rate=fx();rate.evidence.receivedAtMs=rate.evidence.sourceTimeMs-leadMs;
        const auto original=rate.evidence;
        const auto result=reconcileProviderTotal(official(source),&source,{rate},At);
        QCOMPARE(result.state,accepted?ProviderReconciliationState::WithinTolerance:ProviderReconciliationState::Unavailable);
        if(accepted){QCOMPARE(result.krwDifference,QString("1000"));QCOMPARE(result.fxEvidence,original);}
        rate.evidence.sourceTimeMs=At+1;rate.evidence.receivedAtMs=At-34;
        QCOMPARE(reconcileProviderTotal(official(source),&source,{rate},At).state,ProviderReconciliationState::Unavailable);
        auto unconfirmed=official(source);unconfirmed.configurationEvidence.sourceTimeMs=At-1000;unconfirmed.configurationEvidence.receivedAtMs=At-1019;
        QCOMPARE(reconcileProviderTotal(unconfirmed,&source,{fx()},At).state,ProviderReconciliationState::Unavailable);
    }
    void usesMarginBalanceAndKeepsTheOfficialScope() {
        const auto source = snapshot(); const auto total = official(source);
        QCOMPARE(total.availability, ProviderTotalAvailability::Available);
        QCOMPARE(total.amount, QString("100")); QCOMPARE(total.currency, QString("USDT"));
        QCOMPARE(total.metric, QString("totalMarginBalance")); QCOMPARE(total.snapshotId, source.id);
        const auto result = reconcileProviderTotal(total, &source, {}, At);
        QCOMPARE(result.state, ProviderReconciliationState::Exact); QCOMPARE(result.keefiAmount, QString("100"));
        QCOMPARE(result.difference, QString("0")); QVERIFY(result.krwDifference.isEmpty());
        QVERIFY(result.official.coverage.contains("USDT"));
    }
    void spotWalletAndMultiAssetAreNeverMarkedAsMatched() {
        AccountRecord account; account.id = "FAKE_SPOT"; account.provider = "binance"; account.market = "spot";
        auto total = unsupportedProviderTotal(account);
        QCOMPARE(reconcileProviderTotal(total, nullptr, {}, At).state, ProviderReconciliationState::Unsupported);
        account.provider = "phantom"; account.market = "solana";
        QCOMPARE(reconcileProviderTotal(unsupportedProviderTotal(account), nullptr, {}, At).state, ProviderReconciliationState::Unsupported);
        const auto source = snapshot(); const auto configEvidence = official(source).configurationEvidence;
        total = captureBinanceUsdmProviderTotal(source, {{"totalMarginBalance", "100"}}, {{"multiAssetsMargin", true}}, configEvidence);
        QCOMPARE(total.currency, QString("USD")); QCOMPARE(total.availability, ProviderTotalAvailability::Unsupported);
        QCOMPARE(reconcileProviderTotal(total, &source, {}, At).state, ProviderReconciliationState::Unsupported);
    }
    void missingModeAndMissingAmountRemainUnavailable() {
        const auto source = snapshot(); const auto configEvidence = official(source).configurationEvidence;
        for (const auto& config : {QJsonObject{}, QJsonObject{{"multiAssetsMargin", "false"}}}) {
            const auto total = captureBinanceUsdmProviderTotal(source, {{"totalMarginBalance", "100"}}, config, configEvidence);
            QCOMPARE(reconcileProviderTotal(total, &source, {}, At).state, ProviderReconciliationState::Unavailable);
        }
        for (const auto& account : {QJsonObject{}, QJsonObject{{"totalWalletBalance", "100"}},
                                   QJsonObject{{"totalMarginBalance", QJsonValue::Null}}, QJsonObject{{"totalMarginBalance", "bad"}}}) {
            const auto total = captureBinanceUsdmProviderTotal(source, account, {{"multiAssetsMargin", false}}, configEvidence);
            QVERIFY(total.amount.isEmpty());
            QCOMPARE(reconcileProviderTotal(total, &source, {}, At).state, ProviderReconciliationState::Unavailable);
        }
        auto incomplete = configEvidence; incomplete.complete = false;
        const auto unconfirmed = captureBinanceUsdmProviderTotal(source, {{"totalMarginBalance", "100"}},
            {{"multiAssetsMargin", false}}, incomplete);
        QVERIFY(unconfirmed.currency.isEmpty()); QVERIFY(unconfirmed.amount.isEmpty());
        QCOMPARE(unconfirmed.availability, ProviderTotalAvailability::Unavailable);
    }
    void actualZeroAndNegativeEquityUseExactDecimals() {
        for (const auto& value : QStringList{"0", "-2.000000000000000001", "100.000000000000000001"}) {
            const auto source = snapshot(value); const auto result = reconcileProviderTotal(official(source, value), &source, {}, At);
            QCOMPARE(result.state, ProviderReconciliationState::Exact); QCOMPARE(result.difference, QString("0"));
        }
        const auto source = snapshot("-5"); const auto result = reconcileProviderTotal(official(source, "-10"), &source, {fx()}, At);
        QCOMPARE(result.difference, QString("5")); QCOMPARE(result.absoluteDifference, QString("5"));
        QCOMPARE(result.state, ProviderReconciliationState::Mismatch);
    }
    void relativeToleranceCanBeProvedWithoutAConversionRate() {
        const auto source = snapshot("100.5"); const auto result = reconcileProviderTotal(official(source), &source, {}, At);
        QCOMPARE(result.state, ProviderReconciliationState::WithinTolerance); QCOMPARE(result.relativeTolerance, QString("0.5"));
        QVERIFY(result.krwDifference.isEmpty());
    }
    void thousandWonToleranceUsesOnlyAnExplicitFreshRate() {
        auto source = snapshot("101"); auto result = reconcileProviderTotal(official(source), &source, {fx()}, At);
        QCOMPARE(result.state, ProviderReconciliationState::WithinTolerance); QCOMPARE(result.krwDifference, QString("1000"));
        source = snapshot("101.0001"); result = reconcileProviderTotal(official(source), &source, {fx()}, At);
        QCOMPARE(result.state, ProviderReconciliationState::Mismatch); QCOMPARE(result.krwDifference, QString("1000.1"));
        result = reconcileProviderTotal(official(source), &source, {}, At);
        QCOMPARE(result.state, ProviderReconciliationState::Unavailable); QVERIFY(result.reason.contains("1,000"));
    }
    void otherAssetsAndPositionNotionalStayOutsideTheUsdtScope() {
        auto source = snapshot(); auto other = source.components.first(); other.id = "futures:USDC"; other.asset = "USDC"; other.quantity = "900";
        source.components.append(other); other.id = "position:BTCUSDT"; other.asset = "BTCUSDT"; other.kind = AssetComponentKind::PositionNotional;
        other.quantity = "99999"; source.components.append(other);
        const auto result = reconcileProviderTotal(official(source), &source, {}, At);
        QCOMPARE(result.state, ProviderReconciliationState::Exact); QCOMPARE(result.keefiAmount, QString("100"));
        source.components.append(source.components.first());
        QCOMPARE(reconcileProviderTotal(official(source), &source, {}, At).state, ProviderReconciliationState::Unavailable);
        source.components = {other};
        QCOMPARE(reconcileProviderTotal(official(source, "0"), &source, {}, At).state, ProviderReconciliationState::Unavailable);
    }
    void staleIncompleteAndMismatchedEvidenceNeverBecomesAComparison() {
        const auto original = snapshot();
        for (const auto& fault : QStringList{"old-account", "old-config", "future-config", "config-scope", "row-scope", "new-snapshot", "incomplete"}) {
            auto source = original; auto total = official(source);
            if (fault == "old-account") total.evidence.receivedAtMs = At - DisplayFreshnessMs - 1;
            if (fault == "old-config") total.configurationEvidence.receivedAtMs = At - DisplayFreshnessMs - 1;
            if (fault == "future-config") total.configurationEvidence.receivedAtMs = At + 1;
            if (fault == "config-scope") total.configurationEvidence.scope = "OTHER_SCOPE";
            if (fault == "row-scope") source.components[0].evidence.scope = "OTHER_SCOPE";
            if (fault == "new-snapshot") source.id = "NEW_SNAPSHOT";
            if (fault == "incomplete") source.balanceComplete = false;
            QCOMPARE(reconcileProviderTotal(total, &source, {fx()}, At).state, ProviderReconciliationState::Unavailable);
        }
        auto source = original; source.components[0].evidence.receivedAtMs = At - DisplayFreshnessMs - 1;
        QCOMPARE(reconcileProviderTotal(official(source), &source, {}, At).state, ProviderReconciliationState::Unavailable);
    }
    void conversionUsesSourceTimeConflictsAndFreshness() {
        const auto source = snapshot("102"); const auto total = official(source);
        for (const auto& fault : QStringList{"old", "no-source-time", "future", "wrong-scope", "no-url"}) {
            auto rate = fx();
            if (fault == "old") rate.evidence.sourceTimeMs = At - DisplayFreshnessMs - 1;
            if (fault == "no-source-time") rate.evidence.sourceTimeMs = 0;
            if (fault == "future") rate.evidence.sourceTimeMs = At + 1;
            if (fault == "wrong-scope") rate.evidence.scope = "public:upbit:KRW-USDC";
            if (fault == "no-url") rate.evidence.url.clear();
            QCOMPARE(reconcileProviderTotal(total, &source, {rate}, At).state, ProviderReconciliationState::Unavailable);
        }
        auto old = fx("100"); old.evidence.sourceTimeMs = At - 2000; old.evidence.receivedAtMs = At;
        const auto recent = fx("1000");
        QCOMPARE(reconcileProviderTotal(total, &source, {old, recent}, At).state, ProviderReconciliationState::Mismatch);
        auto conflict = recent; conflict.price = "100";
        QCOMPARE(reconcileProviderTotal(total, &source, {recent, conflict}, At).state, ProviderReconciliationState::Unavailable);
    }
    void failedRefreshAndVaultLockDoNotReuseAConfirmedProviderTotal() {
        QTemporaryDir directory; Vault vault(directory.filePath("fake-provider-totals-vault"));
        const QString password = "Fake provider totals test! 2026";
        QVERIFY(vault.create(password));
        keefi::test::ReadFixture fixture("binance-usdm");
        QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
        keefi::test::ProviderReadTransport transport; transport.provider = "binance-usdm";
        PortfolioController::Factories factories;
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        factories.trading = [&](const AccountRecord&) { return &transport; };
        PortfolioController portfolio(vault, nullptr, factories); portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QString("complete"), 5000);
        QCOMPARE(portfolio.providerReconciliations().size(), 1);
        QCOMPARE(portfolio.providerReconciliations().first().state, ProviderReconciliationState::Exact);
        QCOMPARE(portfolio.providerReconciliations().first().official.amount, QString("9999"));
        const auto snapshotId = portfolio.snapshots().first().id;
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(), PortfolioController::RecordCheckpoint::Ready, 4000);
        auto* futures = portfolio.futures(portfolio.connections().first().account.id);
        QVERIFY(futures);
        QTRY_VERIFY_WITH_TIMEOUT(futures->canReconnectAssets(), 2000);
        transport.transform = [](const RequestSpec& request, HttpResult& response) {
            if (request.operation == Operation::Account) { response.status = 503; response.body = "FAKE_ACCOUNT_READ_FAILURE"; }
        };
        portfolio.refreshAll();
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QString("failed"), 5000);
        QCOMPARE(portfolio.snapshots().first().id, snapshotId);
        QCOMPARE(portfolio.providerReconciliations().first().state, ProviderReconciliationState::Unavailable);
        QVERIFY(portfolio.providerReconciliations().first().reason.contains("실패"));
        portfolio.setAutomaticRefresh(false);
        vault.lock(); QVERIFY(portfolio.providerReconciliations().isEmpty());
        QVERIFY(vault.unlock(password));
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.providerReconciliations().isEmpty(), 2000);
        QCOMPARE(portfolio.providerReconciliations().first().state, ProviderReconciliationState::Unavailable);
        QVERIFY(portfolio.providerReconciliations().first().official.amount.isEmpty());
    }
    void configurationFailureCannotReuseAPreviousCycleMode() {
        QTemporaryDir directory; Vault vault(directory.filePath("fake-provider-mode-vault"));
        QVERIFY(vault.create("Fake provider mode test! 2026"));
        keefi::test::ReadFixture fixture("binance-usdm");
        QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
        keefi::test::ProviderReadTransport transport; transport.provider = "binance-usdm";
        FuturesTradingController controller(vault, &transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto first = controller.assetCapture();
        QVERIFY(first.providerTotal.has_value());
        QCOMPARE(reconcileProviderTotal(*first.providerTotal, &first.snapshot, {}, QDateTime::currentMSecsSinceEpoch()).state,
                 ProviderReconciliationState::Exact);
        QTRY_VERIFY_WITH_TIMEOUT(controller.canReconnectAssets(), 2000);
        transport.transform = [](const RequestSpec& request, HttpResult& response) {
            if (request.operation == Operation::AccountConfig) { response.status = 503; response.body = "FAKE_CONFIGURATION_FAILURE"; }
        };
        QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto next = controller.assetCapture();
        QVERIFY(next.snapshot.id != first.snapshot.id);
        QVERIFY(next.providerTotal.has_value());
        QVERIFY(!next.providerTotal->configurationEvidence.complete);
        QVERIFY(next.providerTotal->currency.isEmpty()); QVERIFY(next.providerTotal->amount.isEmpty());
        // Balances/positions can finish, but the old single-asset mode is not proof of this cycle's currency.
        QCOMPARE(reconcileProviderTotal(*next.providerTotal, &next.snapshot, {}, QDateTime::currentMSecsSinceEpoch()).state,
                 ProviderReconciliationState::Unavailable);
        vault.lock();
    }
};
QTEST_MAIN(ProviderReconciliationTest)
#include "tst_provider_reconciliation.moc"
