#include "TooltipTestSupport.h"
#include "keefi/AssetsLedger.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/Vault.h"
#include <QDateTime>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>

using namespace keefi;

namespace {
// Synthetic encrypted-cache fixture only: no registered account, credentials,
// network transport or personal profile is opened by this test.
struct CachedDerivative {
    AssetSnapshot source;
    QList<AssetSnapshot> restored;
    AssetValuationResult valuation;
    PortfolioPresentation view;

    CachedDerivative(const QString& provider, const QString& market) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        source.id = "synthetic-derivative-snapshot";
        source.accountScope = "synthetic-derivative-scope";
        source.connectionId = "00000000-0000-4000-8000-000000000042";
        source.connectionBinding = QString(64, 'a');
        source.provider = provider;
        source.environment = provider == "phantom" ? "mainnet" : "live";
        source.market = market;
        source.timeMs = now;
        source.balanceComplete = true;
        source.coverage = "SYNTHETIC equity and separate position; PnL already included in equity";
        source.evidence = {"SYNTHETIC account response", "https://example.invalid/account", source.accountScope, now, now, true};
        AssetComponent equity;
        equity.id = "synthetic-equity";
        equity.asset = provider == "phantom" ? "USDC" : "USDT";
        equity.quantity = "100";
        equity.kind = AssetComponentKind::Equity;
        equity.providerUnrealizedPnl = "10";
        equity.reportedValueCurrency = equity.asset;
        equity.valuationGroup = "synthetic-equity";
        equity.evidence = source.evidence;
        AssetComponent position;
        position.id = "synthetic-position";
        position.asset = provider == "okx" ? "BTC-USDT-SWAP" : provider == "phantom" ? "BTC" : "BTCUSDT";
        position.quantity = provider == "okx" ? "-2" : "-0.25";
        position.kind = AssetComponentKind::PositionNotional;
        position.providerUnrealizedPnl = "5";
        position.reportedValueCurrency = equity.asset;
        position.valuationGroup = "synthetic-positions";
        position.evidence = source.evidence;
        position.displayName = "SYNTHETIC volatile controller-only display hint";
        position.displaySymbol = position.asset;
        source.components = {equity, position};
    }

    bool roundTrip(QString* error) {
        QTemporaryDir directory;
        if (!directory.isValid()) return false;
        Vault vault(directory.filePath("synthetic-vault.bin"));
        if (!vault.create("Fake Derivative Presentation! 2026", error)) return false;
        const auto cash = source.components.first().asset;
        const QList<AssetQuote> quotes{{cash, "KRW", "1000",
            {"SYNTHETIC public quote", "https://example.invalid/quote", "public:upbit:KRW-" + cash,
                source.timeMs, source.timeMs, true}}};
        const auto path = directory.filePath("synthetic-current.keefi");
        {
            AssetsLedger ledger(&vault, path);
            if (!ledger.open(true, error) || !ledger.replaceLatest({source}, quotes, error)) return false;
        }
        AssetsLedger reopened(&vault, path);
        if (!reopened.open(false, error)) return false;
        restored = reopened.snapshots();
        valuation = AssetValuation::value(restored, reopened.quotes(), "KRW", source.timeMs, DisplayFreshnessMs);
        PortfolioConnection connection;
        connection.account.id = source.connectionId;
        connection.account.alias = "가짜 선물 연결";
        connection.account.provider = source.provider;
        connection.account.environment = source.environment;
        connection.account.market = source.market;
        connection.state = "complete";
        view = presentPortfolio(restored, valuation, {connection});
        return true;
    }

    PortfolioGroupView position() const {
        for (const auto& group : view.groups)
            if (!group.holdings.isEmpty() && group.holdings.first().kind == AssetComponentKind::PositionNotional) return group;
        return {};
    }
};

void derivativeCases() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("market");
    QTest::addColumn<QString>("label");
    QTest::newRow("okx-unified-cache") << QString("okx") << QString("okx_unified") << QString("OKX 통합계정");
    QTest::newRow("okx-futures-cache") << QString("okx") << QString("okx_futures") << QString("선물 · SWAP/FUTURES");
    QTest::newRow("hyperliquid-cache") << QString("phantom") << QString("hyperliquid_perps") << QString("무기한 선물(Hyperliquid)");
    QTest::newRow("binance-existing") << QString("binance") << QString("usdm_futures") << QString("USDⓈ-M 선물");
}
}

class DerivativePresentationTest final : public QObject {
    Q_OBJECT
private slots:
    void connectionTooltipPreservesAliasAndAccessibility() {
        AccountRecord account;
        account.id = QStringLiteral("synthetic-tooltip-account");
        account.alias = QStringLiteral("<b>합성 연결</b> & literal");
        account.provider = QStringLiteral("binance");
        account.environment = QStringLiteral("live");
        account.market = QStringLiteral("spot");
        ConnectionListModel model;
        model.refresh({account}, {});
        const auto row = model.index(0);
        QCOMPARE(row.data(Qt::DisplayRole).toString(), account.alias);
        const auto accessible = row.data(Qt::AccessibleTextRole).toString();
        QVERIFY(accessible.startsWith(account.alias + QLatin1Char('\n')));
        const auto shown = keefi::test::projectTooltip(row.data(Qt::ToolTipRole).toString());
        QCOMPARE(shown.text, accessible);
        QVERIFY(!shown.image);
        QVERIFY(!shown.anchor);
        QCOMPARE(shown.resourceRequests, 0);
    }
    void marketNamesAreHumanReadable_data() { derivativeCases(); }
    void marketNamesAreHumanReadable() {
        QFETCH(QString, market); QFETCH(QString, label);
        QCOMPARE(connectionMarketName(market), label);
    }

    void restoredMarketNamesReachTheDetail_data() { derivativeCases(); }
    void restoredMarketNamesReachTheDetail() {
        QFETCH(QString, provider); QFETCH(QString, market); QFETCH(QString, label);
        CachedDerivative fixture(provider, market); QString error;
        QVERIFY2(fixture.roundTrip(&error), qPrintable(error));
        QCOMPARE(fixture.restored.size(), 1);
        const auto group = fixture.position();
        QCOMPARE(group.holdings.size(), 1);
        // Non-Solana display hints intentionally do not survive encryption.
        QVERIFY(group.holdings.first().displayName.isEmpty());
        AssetDetailPanel detail; detail.showGroup(group);
        const auto* place = detail.findChild<QLabel*>("assetDetailPlaceName0");
        const auto* block = detail.findChild<QLabel*>("assetDetailHolding0");
        QVERIFY(place && block);
        QCOMPARE(place->text(), QString("가짜 선물 연결 · ") + label);
        QVERIFY(block->text().contains(label));
        QVERIFY(!place->text().contains(market));
        QVERIFY(!block->text().contains(market));
    }

    void restoredOkxPositionsShowContractCounts_data() {
        QTest::addColumn<QString>("market");
        QTest::newRow("unified") << QString("okx_unified");
        QTest::newRow("futures") << QString("okx_futures");
    }
    void restoredOkxPositionsShowContractCounts() {
        QFETCH(QString, market);
        CachedDerivative fixture("okx", market); QString error;
        QVERIFY2(fixture.roundTrip(&error), qPrintable(error));
        const auto group = fixture.position();
        QCOMPARE(group.holdings.size(), 1);
        QCOMPARE(group.holdings.first().quantity, QString("-2"));
        QVERIFY(group.holdings.first().displayName.isEmpty());
        AssetDetailPanel detail; detail.showGroup(group);
        const auto* place = detail.findChild<QLabel*>("assetDetailPlaceValue0");
        const auto* block = detail.findChild<QLabel*>("assetDetailHolding0");
        QVERIFY(place && block);
        QVERIFY2(place->text().contains("계약 수"), qPrintable(place->text()));
        QVERIFY2(block->text().contains("계약 수"), qPrintable(block->text()));
        QVERIFY(place->text().contains("-2"));
        QVERIFY(!place->text().contains("-2 BTC-USDT-SWAP"));
        QVERIFY(!block->text().contains("-2 BTC-USDT-SWAP"));
    }

    void cachedPnlAndPositionsStayOutsideEquityTotals_data() { derivativeCases(); }
    void cachedPnlAndPositionsStayOutsideEquityTotals() {
        QFETCH(QString, provider); QFETCH(QString, market);
        CachedDerivative fixture(provider, market); QString error;
        QVERIFY2(fixture.roundTrip(&error), qPrintable(error));
        QVERIFY(fixture.valuation.complete);
        QCOMPARE(fixture.valuation.netAssets, QString("100000"));
        QCOMPARE(fixture.view.summary.knownSubtotal, QString("100000"));
        const auto group = fixture.position();
        QCOMPARE(group.holdings.size(), 1);
        const auto& holding = group.holdings.first();
        QCOMPARE(holding.state, PortfolioValueState::NotCounted);
        QCOMPARE(holding.providerPnl, QString("5"));
        QCOMPARE(group.notCounted, 1);
        QCOMPARE(group.freshTotal, QString("0"));
        QVERIFY(portfolioQuantity(group).isEmpty());
        AssetDetailPanel detail; detail.showGroup(group);
        const auto* place = detail.findChild<QLabel*>("assetDetailPlaceValue0");
        QVERIFY(place);
        QVERIFY(place->text().contains("합계 미포함"));
        QVERIFY(place->text().contains("업체 평가손익 +5 "));
        if (provider != "okx") QVERIFY(!place->text().contains("계약"));
        for (const auto& equity : fixture.view.groups) {
            if (equity.holdings.isEmpty() || equity.holdings.first().kind != AssetComponentKind::Equity) continue;
            detail.showGroup(equity);
            const auto* cash = detail.findChild<QLabel*>("assetDetailPlaceValue0");
            QVERIFY(cash);
            QVERIFY(!cash->text().contains("계약"));
            QVERIFY(cash->text().contains("100 " + equity.asset));
        }
    }
};
QTEST_MAIN(DerivativePresentationTest)
#include "tst_derivative_presentation.moc"
