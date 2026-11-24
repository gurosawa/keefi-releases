#pragma once
#include "keefi/BuildPolicy.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/Vault.h"
#include <QTest>

namespace keefi::test {
// Query-only account assertions. Execution APIs no longer exist.
template<class MutationCount>
void verifyReadOnlyBrokerActions(BrokerTradingController& controller, Vault& vault, QWidget& origin,
                                const OrderInput& input, MutationCount mutationCount) {
    QVERIFY(readOnlyBuild());
    QVERIFY(controller.connected());
    QVERIFY(!controller.busy());
    if (!controller.policy().exactIntentPreflight)
        QVERIFY(controller.setLimits({"10000000", "KRW", "5"}));
    const auto before = mutationCount();
    const auto journal = vault.pendingOperations();
    const auto orders = controller.openOrders();
    Q_UNUSED(origin); Q_UNUSED(input);
    QCOMPARE(mutationCount(), before);
    QCOMPARE(vault.pendingOperations(), journal);
    QVERIFY(!controller.liveEnabled());
    QVERIFY(!controller.busy());
}
}
