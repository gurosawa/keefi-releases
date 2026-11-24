#include "keefi/BrokerOrderReferences.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/KisAdapter.h"
#include "keefi/KiwoomAdapter.h"
#include "keefi/LsAdapter.h"
#include "keefi/TossAdapter.h"
#include "keefi/Vault.h"
#include "fixtures/SuspendedBrokerVault.h"
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;

class LsTradingTests final : public QObject {
    Q_OBJECT
private slots:
    void sharedPoliciesPreserveExistingProviderSemantics(){
        TossAdapter toss;KisAdapter kis("live");KiwoomAdapter kiwoom("live");LsAdapter ls("live");
        for(const auto* adapter:QList<const BrokerAdapter*>{&toss,&kis}){
            QCOMPARE(adapter->policy().cancelExplicitQtyBasis,ExplicitQtyBasis::Total);
            QCOMPARE(adapter->policy().referenceModel,BrokerReferenceModel::TargetReceipt);QVERIFY(!adapter->policy().amendPriceOnly);
        }
        QCOMPARE(kiwoom.policy().cancelQuantityMode,OrderQuantityMode::AllRemaining);
        QCOMPARE(kiwoom.policy().referenceModel,BrokerReferenceModel::TargetReceiptRoot);QVERIFY(!kiwoom.policy().amendPriceOnly);
        QCOMPARE(ls.policy().cancelQuantityMode,OrderQuantityMode::Exact);QCOMPARE(ls.policy().cancelExplicitQtyBasis,ExplicitQtyBasis::Remaining);
        QVERIFY(ls.policy().amendPriceOnly);QCOMPARE(ls.policy().referenceModel,BrokerReferenceModel::TargetReceiptRoot);
        QVERIFY(lsOrderReferenceValid("L1_20261019_KRX_0000000100"));QVERIFY(!lsOrderReferenceValid("L1_20260230_KRX_100"));
    }
    void savedAccountCannotSelectEvenAnInjectedAdapter() {
        QTemporaryDir dir;
        const auto path = dir.filePath("credentials.keefi-vault");
        Vault vault(path);
        QVERIFY(test::openSuspendedBrokerFixture(vault));
        bool factoryCalled = false;
        BrokerTradingController controller(vault, nullptr, nullptr, [&](const AccountRecord& account) {
            factoryCalled = true;
            return std::make_unique<LsAdapter>(account.environment);
        });
        QCOMPARE(vault.accounts().size(), 2);
        for (const auto& account : vault.accounts()) {
            QVERIFY(!controller.selectConnection(account.id));
            QVERIFY(!controller.supported());
            QVERIFY(!controller.canAuthenticate());
            controller.authenticate();
            QVERIFY(!controller.refreshAssets());
            QVERIFY(!controller.busy());
        }
        QVERIFY(!factoryCalled);
    }
};
QTEST_MAIN(LsTradingTests)
#include "tst_ls_trading.moc"
