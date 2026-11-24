#include "keefi/ValuationBundle.h"
#include "keefi/PortfolioController.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;

class ValuationBundleTest final : public QObject {
    Q_OBJECT
  private slots:
    void independentCalculationMatchesExport_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto& scenario : {"incomplete-account", "stale-row", "aged-account", "complete-zero", "unknown-token",
                                     "excluded-token", "reference-price", "public-price", "duplicate-row", "duplicate-equity",
                                     "unknown-only", "reference-only", "aggregate-cancellation", "aggregate-overflow",
                                     "reference-cancellation", "reference-overflow", "clock-small", "clock-fx-fanout",
                                     "clock-future", "clock-over-limit"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void independentCalculationMatchesExport() {
        QFETCH(QString, scenario);
        constexpr qint64 at = 1800000000000;
        AssetSnapshot snapshot;
        snapshot.id = "synthetic"; snapshot.accountScope = "synthetic-scope";
        snapshot.connectionId = "synthetic-connection"; snapshot.provider = "binance";
        snapshot.environment = "live"; snapshot.market = "spot";
        snapshot.timeMs = at - 1000; snapshot.balanceComplete = scenario != "incomplete-account";
        snapshot.evidence = {"Synthetic balance", "https://example.invalid/balance", snapshot.accountScope, 0, at - 1000, true};
        AssetComponent cash;
        cash.id = "cash"; cash.asset = "KRW"; cash.quantity = scenario == "complete-zero" ? "0" : "1000";
        cash.kind = AssetComponentKind::Cash; cash.evidence = snapshot.evidence;
        snapshot.components = {cash};
        if (scenario.startsWith("aggregate-") || (scenario.startsWith("reference-") &&
            (scenario.endsWith("cancellation") || scenario.endsWith("overflow")))) {
            snapshot.components[0].quantity = QString(80, '9');
            auto one = cash; one.id = "one"; one.quantity = "1"; snapshot.components.append(one);
            if (scenario.endsWith("cancellation")) {
                auto debt = one; debt.id = "debt"; debt.kind = AssetComponentKind::Liability;
                snapshot.components.append(debt);
            }
            if (scenario.startsWith("reference-"))
                for (auto& component : snapshot.components) component.asset = "BTC";
        }
        if (scenario == "unknown-only" || scenario == "reference-only") {
            snapshot.components[0].asset = "BTC";
            snapshot.components[0].kind = AssetComponentKind::Holding;
        }
        if (scenario == "duplicate-row") {
            auto duplicate = cash; duplicate.quantity = "50"; snapshot.components.append(duplicate);
        }
        if (scenario == "duplicate-equity") {
            snapshot.components[0].kind = AssetComponentKind::Equity;
            auto duplicate = snapshot.components[0]; duplicate.id = "another-equity"; duplicate.quantity = "50";
            snapshot.components.append(duplicate);
        }
        if (scenario == "stale-row") {
            snapshot.components[0].evidence.receivedAtMs = at - DisplayFreshnessMs - 1;
            auto zero = cash; zero.id = "fresh-zero"; zero.quantity = "0"; snapshot.components.append(zero);
        }
        if (scenario == "aged-account") {
            snapshot.timeMs = at - DisplayFreshnessMs - 1;
            snapshot.evidence.receivedAtMs = snapshot.timeMs;
        }
        if (scenario == "unknown-token") {
            auto unknown = cash; unknown.id = "unknown"; unknown.asset = "NOPRICE"; unknown.quantity = "0.001";
            unknown.kind = AssetComponentKind::Holding; snapshot.components.append(unknown);
        }
        const QString mint = "solana:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
        if (scenario == "excluded-token") {
            snapshot.market = "solana";
            auto unknown = cash; unknown.id = "token"; unknown.asset = mint; unknown.quantity = "1";
            unknown.kind = AssetComponentKind::Holding; snapshot.components.append(unknown);
        }
        if (scenario == "reference-price" || scenario == "public-price") {
            auto token = cash; token.id = "pump"; token.asset = "PUMP"; token.quantity = "2";
            token.kind = AssetComponentKind::Holding; snapshot.components.append(token);
        }
        PortfolioConnection connection;
        connection.account.id = snapshot.connectionId; connection.account.provider = snapshot.provider;
        connection.account.alias = "Synthetic account"; connection.account.environment = "live";
        connection.account.market = "spot"; connection.state = "complete";
        ValuationCapture capture;
        capture.asOfMs = at; capture.connections = {connection}; capture.snapshots = {snapshot};
        if (scenario == "reference-only" || scenario == "reference-cancellation" || scenario == "reference-overflow")
            capture.references.append({"BTC", "KRW", "1", {"Upbit public price", "https://api.upbit.com/v1/ticker",
                "public:upbit:KRW-BTC", at - DisplayFreshnessMs - 1, at - 1000, true}});
        if (scenario == "excluded-token") capture.excludedAssets.insert(mint);
        if (scenario == "reference-price" || scenario == "public-price") {
            AssetQuote price{"PUMP", "KRW", "2.75", {"Upbit public price", "https://api.upbit.com/v1/ticker",
                "public:upbit:KRW-PUMP", at - (scenario == "reference-price" ? 200000 : 1000), at - 1000, true}};
            (scenario == "reference-price" ? capture.references : capture.quotes).append(price);
        }
        if (scenario == "clock-small") {
            auto token = cash; token.id = "pump"; token.asset = "PUMP"; token.quantity = "2";
            token.kind = AssetComponentKind::Holding; capture.snapshots[0].components.append(token);
            capture.quotes.append({"PUMP", "KRW", "3", {"Synthetic public price", "https://example.invalid/price",
                "public:upbit:KRW-PUMP", at - 1000, at - 1035, true}});
        } else if (scenario.startsWith("clock-")) {
            capture.snapshots[0].components.clear();
            for (const auto& asset : {QString("USDT"), QString("USDC"), QString("PUMP")}) {
                auto holding = cash; holding.id = asset; holding.asset = asset; holding.kind = AssetComponentKind::Holding;
                holding.quantity = asset == "USDT" ? "2" : asset == "USDC" ? "3" : "4";
                capture.snapshots[0].components.append(holding);
            }
            const qint64 source = scenario == "clock-future" ? at + 1 : at - 1000;
            const qint64 receipt = scenario == "clock-over-limit" ? source - 2001 : source - 19;
            capture.quotes = {
                {"USDT", "KRW", "1000", {"Synthetic public price", "https://example.invalid/fx", "public:upbit:KRW-USDT", source, receipt, true}},
                {"USDC", "USDT", "0.5", {"Synthetic public price", "https://example.invalid/price", "public:binance:USDCUSDT", at - 1000, at - 1000, true}},
                {"PUMP", "USDT", "0.25", {"Synthetic public price", "https://example.invalid/price", "public:binance:PUMPUSDT", at - 1000, at - 1000, true}}
            };
            if (scenario == "clock-fx-fanout") {
                capture.snapshots[0].components.clear();capture.quotes={capture.quotes.first()};
                for(int i=0;i<13;++i){
                    auto holding=cash;holding.id=QString("synthetic-%1").arg(i);holding.kind=AssetComponentKind::Holding;
                    holding.asset=i==0?QString("USDT"):i<6?QString("FAKE%1").arg(i-1):QString("UNPRICED%1").arg(i-6);
                    holding.quantity=i==0?QString("2"):QString("1");capture.snapshots[0].components.append(holding);
                    if(i>0&&i<6)capture.quotes.append({holding.asset,"USDT","1",{"Synthetic public price","https://example.invalid/price",
                        "public:binance:"+holding.asset+"USDT",at-1000,at-1000,true}});
                }
                auto normal=capture.quotes;normal[0].evidence.receivedAtMs=normal[0].evidence.sourceTimeMs;
                const auto baseline=AssetValuation::value(capture.snapshots,normal,"KRW",at,DisplayFreshnessMs);
                QCOMPARE(presentPortfolio(capture.snapshots,baseline,capture.connections).summary.unvaluedGroups,7);
            }
        }
        capture.valuation = AssetValuation::value(capture.snapshots, capture.quotes, "KRW", at, DisplayFreshnessMs,
                                                  capture.references, capture.excludedAssets);
        if (scenario == "clock-small") QCOMPARE(capture.valuation.netAssets, QString("1006"));
        if (scenario == "clock-fx-fanout") QCOMPARE(capture.valuation.knownSubtotal, QString("7000"));
        if (scenario == "clock-future" || scenario == "clock-over-limit") {
            QVERIFY(capture.valuation.netAssets.isEmpty());
            for (const auto& row : capture.valuation.rows) QVERIFY(row.value.isEmpty());
        }
        capture.view = presentPortfolio(capture.snapshots, capture.valuation, capture.connections);
        if (scenario == "clock-fx-fanout") QCOMPARE(capture.view.summary.unvaluedGroups,7);
        capture.totalLabel = capture.view.summary.excludedGroups
            ? (capture.view.summary.complete ? QStringLiteral("평가 대상 합계") : QStringLiteral("평가 대상 부분합"))
            : capture.view.summary.complete ? QStringLiteral("순자산") : QStringLiteral("확인된 자산 부분합");
        bool anyFresh = false;
        for (const auto& row : capture.valuation.rows) anyFresh |= row.included && !row.value.isEmpty();
        const auto exactFigure = capture.view.summary.complete ? capture.valuation.netAssets : capture.valuation.knownSubtotal;
        capture.totalFigure = !capture.view.summary.complete && !anyFresh ? QStringLiteral("—")
            : exactFigure.isEmpty() ? QStringLiteral("자료 부족") : krwAmount(exactFigure);
        ResponseJournal::setEnabled(true);
        HttpResult authentication; authentication.status = 200; authentication.body = R"({"access_token":"FAKE_AUTH_R1_INTEGRATION"})";
        ResponseJournal::recordTrading(Operation::Authenticate, "POST", QUrl("https://example.invalid/oauth2/token"), true, authentication);
        const auto dustThreshold = scenario == "clock-fx-fanout" ? QString("1000") : QString{};
        const auto exported = valuationBundle(capture, dustThreshold, ResponseJournal::entries(), true, "KEEFI test", "synthetic");
        ResponseJournal::setEnabled(false);
        const auto bytes = QJsonDocument(exported).toJson();
        QVERIFY(!bytes.contains("FAKE_AUTH_R1_INTEGRATION"));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile bundle(directory.filePath("synthetic.json"));
        QVERIFY(bundle.open(QIODevice::WriteOnly)); QVERIFY(bundle.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(bundle.write(bytes), bytes.size()); bundle.close();
        QProcess recalculator;
        recalculator.start(QString::fromUtf8(KEEFI_TEST_PYTHON),
                           {QString::fromUtf8(KEEFI_RECON_COMPARE), bundle.fileName()});
        QVERIFY(recalculator.waitForFinished(10000));
        const auto result = recalculator.readAllStandardOutput() + recalculator.readAllStandardError();
        QCOMPARE(recalculator.exitStatus(), QProcess::NormalExit);
        QVERIFY2(recalculator.exitCode() == 0, result.constData());
        QVERIFY(result.startsWith("MATCH:"));
    }
    void officialComparisonExportsScopeAndMissingValuesWithoutPrivateIdentities() {
        ValuationCapture capture;
        AssetSnapshot snapshot;
        snapshot.connectionId = "PRIVATE_CONNECTION_SENTINEL";
        snapshot.accountScope = "PRIVATE_SCOPE_SENTINEL";
        capture.snapshots = {snapshot};
        ProviderReconciliationResult comparison;
        comparison.official.connectionId = snapshot.connectionId;
        comparison.official.accountScope = snapshot.accountScope;
        comparison.official.snapshotId = "PRIVATE_SNAPSHOT_SENTINEL";
        comparison.official.metric = "totalMarginBalance";
        comparison.official.currency = "USDT";
        comparison.official.coverage = "USDT equity only";
        comparison.official.amount = "0";
        comparison.official.evidence.scope = snapshot.accountScope;
        comparison.official.configurationEvidence.scope = snapshot.accountScope;
        comparison.state = ProviderReconciliationState::Unavailable;
        capture.providerReconciliations = {comparison};
        const auto bundle = valuationBundle(capture, {}, {}, false, "KEEFI test", "synthetic");
        const auto exported = bundle.value("provider_reconciliations").toArray().first().toObject();
        QCOMPARE(exported.value("official_amount").toString(), QString("0"));
        QVERIFY(exported.value("keefi_amount").isNull());
        QVERIFY(exported.value("difference").isNull());
        QCOMPARE(exported.value("currency").toString(), QString("USDT"));
        QCOMPARE(exported.value("coverage").toString(), QString("USDT equity only"));
        QCOMPARE(exported.value("state").toString(), QString::fromUtf8("확인 필요"));
        QCOMPARE(exported.value("account_scope").toString(), QString("scope-1"));
        QCOMPARE(exported.value("connection_id").toString(), QString("conn-1"));
        QCOMPARE(exported.value("configuration_evidence").toObject().value("scope").toString(), QString("scope-1"));
        const auto bytes = QJsonDocument(bundle).toJson();
        QVERIFY(!bytes.contains("PRIVATE_CONNECTION_SENTINEL"));
        QVERIFY(!bytes.contains("PRIVATE_SCOPE_SENTINEL"));
        QVERIFY(!bytes.contains("PRIVATE_SNAPSHOT_SENTINEL"));
    }
};
QTEST_GUILESS_MAIN(ValuationBundleTest)
#include "tst_valuation_bundle.moc"
