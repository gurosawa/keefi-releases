#include "fixtures/PortfolioTestRow.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/TradingTransport.h"
#include "keefi/ValuationPreferences.h"
#include "keefi/Vault.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QUrlQuery>
#include <algorithm>

using keefi::test::PortfolioTestRow;
using keefi::test::portfolioRow;

using namespace keefi;
namespace {
const QString Mint = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
const QString Asset = "solana:" + Mint;
const QString Owner = "11111111111111111111111111111111";
const QString TokenProgram = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";

// A real WalletReadController consumes these RPC responses. The public Mint is
// held synthetically; metadata and price fail independently without losing it.
class UnpricedWalletTransport final : public SwapTransport {
  public:
    QList<SwapOperation> operations;
    void abort() override {}
    void send(quint64 token, const SwapRequest& request) override {
        QVERIFY(NetworkSwapTransport::allowed(request));
        QVERIFY(request.operation != SwapOperation::Order && request.operation != SwapOperation::Execute);
        operations.append(request.operation);
        HttpResult response; response.status = 200;
        if (request.operation == SwapOperation::TokenMetadata || request.operation == SwapOperation::TokenPrices) {
            response.status = 503; response.body = "{}";
        } else {
            const auto body = QJsonDocument::fromJson(request.body).object();
            const auto params = body.value("params").toArray();
            QJsonValue result;
            if (request.operation == SwapOperation::Genesis) result = "5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d";
            else if (request.operation == SwapOperation::Balance)
                result = QJsonObject{{"context", QJsonObject{{"slot", "100"}}}, {"value", "0"}};
            else if (request.operation == SwapOperation::TokenAccounts) {
                QJsonArray accounts;
                if (params[1].toObject().value("programId").toString() == TokenProgram) {
                    const QJsonObject info{{"owner", Owner}, {"mint", Mint}, {"state", "initialized"},
                                           {"tokenAmount", QJsonObject{{"amount", "1000000"}, {"decimals", "6"}}}};
                    accounts.append(QJsonObject{{"pubkey", "ComputeBudget111111111111111111111111111111"},
                        {"account", QJsonObject{{"owner", TokenProgram}, {"data", QJsonObject{{"parsed", QJsonObject{{"type", "account"}, {"info", info}}}}}}}});
                }
                result = QJsonObject{{"context", QJsonObject{{"slot", "101"}}}, {"value", accounts}};
            } else if (request.operation == SwapOperation::Accounts) {
                QJsonArray values;
                for (const auto& ignored : params[0].toArray()) {
                    Q_UNUSED(ignored);
                    values.append(QJsonObject{{"owner", TokenProgram}, {"executable", false},
                        {"data", QJsonObject{{"parsed", QJsonObject{{"type", "mint"}, {"info", QJsonObject{{"decimals", 6}, {"isInitialized", true}}}}}}}});
                }
                result = QJsonObject{{"context", QJsonObject{{"slot", "102"}}}, {"value", values}};
            } else QFAIL("Unexpected wallet request in an offline valuation-scope fixture");
            response.body = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", body.value("id")}, {"result", result}}).toJson();
        }
        QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
    }
};

class PublicPriceTransport final : public TradingTransport {
  public:
    void abort() override {}
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(!request.authenticated && request.headers.isEmpty());
        QVERIFY(NetworkTradingTransport::allowed(request));
        QJsonArray rows;
        if (request.operation == Operation::Markets) rows.append(QJsonObject{{"market", "KRW-USDC"}});
        else {
            QCOMPARE(request.operation, Operation::Quote);
            for (const auto& market : QUrlQuery(request.url).queryItemValue("markets").split(','))
                rows.append(QJsonObject{{"market", market}, {"trade_price", "1300"}, {"timestamp", QString::number(QDateTime::currentMSecsSinceEpoch())}});
        }
        HttpResult response; response.status = 200; response.body = QJsonDocument(rows).toJson();
        QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
    }
};

PortfolioTestRow mintGroup(QTreeView* tree) {
    for (int index = 0; index < tree->model()->rowCount(); ++index)
        if (portfolioRow(tree, index).data(0, Qt::UserRole + 1).toString() == Asset) return portfolioRow(tree, index);
    return {};
}
QJsonObject mintRow(const QJsonObject& bundle) {
    for (const auto& value : bundle.value("keefi_result").toObject().value("rows").toArray())
        if (value.toObject().value("asset").toString() == Asset) return value.toObject();
    return {};
}
QVariantMap storedExclusions() {
    QSettings settings; settings.sync(); QVariantMap result;
    for (const auto& key : settings.allKeys()) if (key.startsWith("valuation/excludedMints/")) result.insert(key, settings.value(key));
    return result;
}
} // namespace

class ValuationScopeUiTest final : public QObject {
    Q_OBJECT
  private slots:
    void signedGroupTotalsAndQuantitiesDoNotDependOnHoldingOrder() {
        const qint64 at=1800000000000LL;const QString maximum(80,'9');
        AssetSnapshot snapshot;snapshot.id="aggregate";snapshot.accountScope="synthetic:aggregate";
        snapshot.provider="fake";snapshot.market="spot";snapshot.timeMs=at;snapshot.balanceComplete=true;
        snapshot.evidence={"Synthetic","https://example.invalid/aggregate",snapshot.accountScope,at,at,true};
        AssetComponent large;large.id="large";large.asset="KRW";large.quantity=maximum;
        large.kind=AssetComponentKind::Cash;large.evidence=snapshot.evidence;
        auto one=large;one.id="one";one.quantity="1";
        auto debt=one;debt.id="debt";debt.kind=AssetComponentKind::Liability;
        const QList<AssetComponent> components{large,one,debt};QList<int> order{0,1,2};
        do {
            snapshot.components.clear();for(int i:order)snapshot.components.append(components[i]);
            const auto valuation=AssetValuation::value({snapshot},{},"KRW",at,DisplayFreshnessMs);
            const auto view=presentPortfolio({snapshot},valuation,{});QCOMPARE(view.groups.size(),1);
            QCOMPARE(view.groups.first().freshTotal,maximum);QCOMPARE(portfolioQuantity(view.groups.first()),maximum);
            const auto scoped=scopedPortfolioGroups(view.groups,"cash",{});QCOMPARE(scoped.size(),1);
            QCOMPARE(scoped.first().freshTotal,maximum);QCOMPARE(portfolioQuantity(scoped.first()),maximum);
            for(const auto& holding:view.groups.first().holdings)QCOMPARE(holding.state,PortfolioValueState::Fresh);
            for(auto& component:snapshot.components)component.asset="BTC";
            AssetQuote old{"BTC","KRW","1",{"Synthetic public price","https://example.invalid/BTC","public:upbit:KRW-BTC",at-DisplayFreshnessMs-1,at-DisplayFreshnessMs-1,true}};
            const auto reference=presentPortfolio({snapshot},AssetValuation::value({snapshot},{},"KRW",at,DisplayFreshnessMs,{old}),{});
            QCOMPARE(reference.summary.referenceTotal,maximum);
        }while(std::next_permutation(order.begin(),order.end()));
        snapshot.components={large,one};
        const auto view=presentPortfolio({snapshot},AssetValuation::value({snapshot},{},"KRW",at,DisplayFreshnessMs),{});
        QVERIFY(view.groups.first().freshTotal.isEmpty());QVERIFY(portfolioQuantity(view.groups.first()).isEmpty());
        for(const auto& holding:view.groups.first().holdings)QCOMPARE(holding.state,PortfolioValueState::Fresh);
        for(auto& component:snapshot.components)component.asset="BTC";
        AssetQuote old{"BTC","KRW","1",{"Synthetic public price","https://example.invalid/BTC","public:upbit:KRW-BTC",at-DisplayFreshnessMs-1,at-DisplayFreshnessMs-1,true}};
        const auto reference=presentPortfolio({snapshot},AssetValuation::value({snapshot},{},"KRW",at,DisplayFreshnessMs,{old}),{});
        QVERIFY(reference.summary.referenceTotal.isEmpty());
    }
    void mintExclusionAndRestoreUseTheRealAssetsScreen() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Valuation Scope Tests");
        QCoreApplication::setApplicationName("Valuation Scope UI Test");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
        Vault vault(dir.filePath("fake-scope-ui.bin")); QVERIFY(vault.create("Fake Scope UI Vault! 2026"));
        AccountRecord account; account.provider="phantom"; account.alias="FAKE SCOPE WALLET";
        account.environment="mainnet"; account.market="solana"; account.currency="SOL";
        QVERIFY(vault.addAccount(account,{{"address",Owner}},StorageMode::EncryptedVault));
        UnpricedWalletTransport wallet;
        PortfolioController::Factories factories;
        factories.wallet=[&](const AccountRecord&){return &wallet;};
        factories.prices=[] { return new PublicPriceTransport; };
        PortfolioController portfolio(vault,nullptr,factories); AccountModel accounts(&vault); LocalWindow window(vault,accounts);
        window.setPortfolioController(&portfolio); window.setSecurityStatus(true); window.resize(1440,900); window.show();
        auto* pages=window.findChild<QTabWidget*>("portfolioPages"); QVERIFY(pages);
        pages->setCurrentWidget(window.findChild<QWidget*>("portfolioHome"));
        auto* tree=window.findChild<QTreeView*>("portfolioAssets"); QVERIFY(tree);
        auto* title=window.findChild<QLabel*>("portfolioTotalLabel"); QVERIFY(title);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty() && portfolio.connections().first().state=="complete",4000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.valuationDiagnostics().isEmpty(),4000);
        portfolio.setAutomaticRefresh(false);
        QVERIFY(wallet.operations.contains(SwapOperation::TokenMetadata)); QVERIFY(wallet.operations.contains(SwapOperation::TokenPrices));
        QCOMPARE(wallet.operations.count(SwapOperation::TokenAccounts),2);
        auto* hideUnvalued=window.findChild<QPushButton*>("hideUnvaluedBalances"); QVERIFY(hideUnvalued);
        // The real screen defaults to hiding unvalued holdings. Exercise that
        // default, then expose the row before testing its exclusion action.
        QVERIFY(hideUnvalued->isChecked()); QVERIFY(mintGroup(tree)); QVERIFY(mintGroup(tree).isHidden());
        hideUnvalued->setChecked(false);
        QVERIFY(mintGroup(tree)); QCOMPARE(mintGroup(tree).text(2),QStringLiteral("1"));
        QCOMPARE(mintGroup(tree).text(3),QStringLiteral("미평가")); QVERIFY(!mintGroup(tree).isHidden());
        QCOMPARE(mintRow(window.valuationDiagnostics()).value("state").toString(),QStringLiteral("Unvalued"));

        tree->setCurrentIndex(mintGroup(tree).modelIndex());
        auto* detail=window.findChild<QWidget*>("portfolioHome")->findChild<AssetDetailPanel*>(); QVERIFY(detail);
        auto* action=detail->findChild<QPushButton*>("assetValuationExclusion"); QVERIFY(action);
        QTRY_VERIFY(!action->isHidden()); QCOMPARE(action->text(),QStringLiteral("평가 대상 제외"));
        action->click(); // Exercises AssetDetailPanel -> LocalWindow -> persisted preference -> valuation.
        QTRY_COMPARE_WITH_TIMEOUT(title->text(),QStringLiteral("평가 대상 합계"),2000);
        QVERIFY(mintGroup(tree)); QCOMPARE(mintGroup(tree).text(2),QStringLiteral("1"));
        QCOMPARE(mintGroup(tree).text(3),QStringLiteral("평가 대상 제외"));
        const auto excludedBundle=window.valuationDiagnostics(); const auto excludedRow=mintRow(excludedBundle);
        QCOMPARE(excludedRow.value("state").toString(),QStringLiteral("NotCounted"));
        QVERIFY(excludedRow.value("excluded_by_user").toBool()); QCOMPARE(excludedRow.value("quantity").toString(),QStringLiteral("1"));
        QVERIFY(excludedRow.value("value").isNull()); // User exclusion is not a price of zero.
        bool chipFound=false;
        for (auto* chip : window.findChildren<QPushButton*>("portfolioGapChip")) chipFound|=chip->text()==QStringLiteral("평가 제외 1종");
        QVERIFY(chipFound);
        hideUnvalued->setChecked(true); QVERIFY(mintGroup(tree)); QVERIFY(!mintGroup(tree).isHidden());

        tree->setCurrentIndex(mintGroup(tree).modelIndex()); QTRY_COMPARE(action->text(),QStringLiteral("평가 대상에 포함"));
        action->click();
        QTRY_COMPARE_WITH_TIMEOUT(title->text(),QStringLiteral("확인된 자산 부분합"),2000);
        QVERIFY(mintGroup(tree)); QCOMPARE(mintGroup(tree).text(2),QStringLiteral("1"));
        QCOMPARE(mintGroup(tree).text(3),QStringLiteral("미평가")); QVERIFY(mintGroup(tree).isHidden());
        const auto restored=mintRow(window.valuationDiagnostics());
        QCOMPARE(restored.value("state").toString(),QStringLiteral("Unvalued")); QVERIFY(!restored.value("excluded_by_user").toBool());

        hideUnvalued->setChecked(false); tree->setCurrentIndex(mintGroup(tree).modelIndex()); action->click();
        QTRY_COMPARE(title->text(),QStringLiteral("평가 대상 합계"));
        const auto savedChoices=storedExclusions(); QVERIFY(!savedChoices.isEmpty());
        QSignalSpy requested(detail,&AssetDetailPanel::valuationExclusionRequested);
        vault.lock();
        QCOMPARE(tree->model()->rowCount(),0); QVERIFY(window.valuationDiagnostics().isEmpty());
        QVERIFY(action->isHidden()); action->click(); QCOMPARE(requested.count(),0);
        auto* preferences=window.findChild<ValuationPreferences*>(); QVERIFY(preferences); QVERIFY(preferences->excludedAssets().isEmpty());
        // Even a delayed pre-lock selection signal must not alter the saved choice.
        QVERIFY(QMetaObject::invokeMethod(detail,"valuationExclusionRequested",Q_ARG(QString,Asset),Q_ARG(bool,false)));
        QCOMPARE(storedExclusions(),savedChoices);
    }
};

QTEST_MAIN(ValuationScopeUiTest)
#include "tst_valuation_scope_ui.moc"
