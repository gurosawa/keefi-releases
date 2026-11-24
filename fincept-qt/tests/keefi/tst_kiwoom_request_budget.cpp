#include "keefi/KiwoomRequestBudget.h"
#include "keefi/TradingRequestLimits.h"
#include <QtTest>
#include <limits>

using namespace keefi;
namespace {
constexpr auto Broker = KiwoomBudgetLane::ExistingBroker;
constexpr auto Display = KiwoomBudgetLane::Display;
constexpr auto Catalogue = KiwoomBudgetRequest::UsCatalogue;
}
class KiwoomRequestBudgetTest final : public QObject {
    Q_OBJECT
  private slots:
    void defaultsShareOneProcessGate() {
        const auto first=KiwoomRequestBudget::shared(), second=KiwoomRequestBudget::shared();
        QVERIFY(first); QCOMPARE(first,second);
    }
    void allExistingBrokerAndDisplayOwnersShareFourHundredMilliseconds() {
        qint64 now=0; KiwoomRequestBudget gate([&]{return now;}); QObject broker,display,otherBroker;
        const auto first=gate.tryAcquire(&broker,Broker); QVERIFY(first.lease); QCOMPARE(first.waitMs,0);
        QVERIFY(gate.finish(first.lease));
        for(const auto instant:{0,100,399}) { now=instant;const auto blocked=gate.tryAcquire(&display,Display);QCOMPARE(blocked.lease,quint64(0));QCOMPARE(blocked.waitMs,400-now); }
        now=400;const auto second=gate.tryAcquire(&display,Display);QVERIFY(second.lease);QVERIFY(second.lease!=first.lease);QVERIFY(gate.finish(second.lease));
        now=799;QCOMPARE(gate.tryAcquire(&otherBroker,Broker).lease,quint64(0));
        now=800;QVERIFY(gate.tryAcquire(&otherBroker,Broker).lease);
    }
    void unfinishedPeerBlocksEvenAfterSpacingExpires() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject a,b;
        const auto first=gate.tryAcquire(&a,Display);QVERIFY(first.lease);now=10000;
        const auto waiting=gate.tryAcquire(&b,Broker);QCOMPARE(waiting.lease,quint64(0));QVERIFY(waiting.waitMs>0);
        QVERIFY(gate.remainingMs()>0);QVERIFY(gate.finish(first.lease));QVERIFY(gate.tryAcquire(&b,Broker).lease);
    }
    void pendingExistingBrokerWinsTheNextGrant() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject broker,display;
        gate.setBrokerPending(&broker,true);const auto waiting=gate.tryAcquire(&display,Display);QCOMPARE(waiting.lease,quint64(0));QVERIFY(waiting.waitMs>0);
        const auto granted=gate.tryAcquire(&broker,Broker);QVERIFY(granted.lease);QVERIFY(gate.finish(granted.lease));
        now=400;QCOMPARE(gate.tryAcquire(&display,Display).lease,quint64(0));
        gate.setBrokerPending(&broker,false);QVERIFY(gate.tryAcquire(&display,Display).lease);
    }
    void destroyedOwnerCannotLeaveAnUnboundedPendingOrInflightLease() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});auto owner=std::make_unique<QObject>();QObject peer;
        gate.setBrokerPending(owner.get(),true);const auto first=gate.tryAcquire(owner.get(),Broker);QVERIFY(first.lease);owner.reset();
        now=399;QCOMPARE(gate.tryAcquire(&peer,Display).lease,quint64(0));
        now=400;const auto next=gate.tryAcquire(&peer,Display);QVERIFY(next.lease);QVERIFY(next.lease!=first.lease);QVERIFY(!gate.finish(first.lease));
        now=800;QCOMPARE(gate.tryAcquire(&peer,Display).lease,quint64(0));QVERIFY(gate.finish(next.lease));
    }
    void usaCatalogueHasTwelveSecondSpacingAcrossAllOwners() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject a,b;
        for(int index=0;index<5;++index) {
            now=index*12000;const auto accepted=gate.tryAcquire(index%2?&a:&b,Display,Catalogue);QVERIFY(accepted.lease);QVERIFY(gate.finish(accepted.lease));
            now=index*12000+400;const auto ordinary=gate.tryAcquire(&a,Broker);QVERIFY(ordinary.lease);QVERIFY(gate.finish(ordinary.lease));
            now=index*12000+11999;const auto early=gate.tryAcquire(&b,Display,Catalogue);QCOMPARE(early.lease,quint64(0));QCOMPARE(early.waitMs,1);
        }
        now=60000;QVERIFY(gate.tryAcquire(&b,Display,Catalogue).lease);
    }
    void finishingOrAbortingNeverRefundsAlreadyChargedTime() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject owner;
        const auto first=gate.tryAcquire(&owner,Display,Catalogue);QVERIFY(first.lease);QVERIFY(gate.finish(first.lease));
        QVERIFY(!gate.finish(first.lease));QCOMPARE(gate.remainingMs(),qint64(400));QCOMPARE(gate.remainingMs(Catalogue),qint64(12000));
        now=400;QVERIFY(gate.tryAcquire(&owner,Broker).lease);QCOMPARE(gate.tryAcquire(&owner,Display,Catalogue).lease,quint64(0));
    }
    void oldLeaseCannotReleaseOrPauseANewRequest() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject a,b;
        const auto old=gate.tryAcquire(&a,Display);QVERIFY(old.lease);QVERIFY(gate.finish(old.lease));now=400;
        const auto current=gate.tryAcquire(&b,Broker);QVERIFY(current.lease);QVERIFY(!gate.finish(old.lease,60000));now=800;
        QCOMPARE(gate.tryAcquire(&a,Display).lease,quint64(0));QVERIFY(gate.finish(current.lease));QCOMPARE(gate.remainingMs(),qint64(0));QVERIFY(gate.tryAcquire(&a,Display).lease);
    }
    void businessLimitPauseAppliesToBothLanesWithoutShortening() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject broker,display;
        const auto first=gate.tryAcquire(&broker,Broker);QVERIFY(first.lease);QVERIFY(gate.finish(first.lease,1700));
        now=1000;QCOMPARE(gate.tryAcquire(&display,Display).waitMs,qint64(700));
        now=1700;const auto second=gate.tryAcquire(&display,Display);QVERIFY(second.lease);QVERIFY(gate.finish(second.lease,2000));
        now=3699;QCOMPARE(gate.tryAcquire(&broker,Broker).waitMs,qint64(1));now=3700;QVERIFY(gate.tryAcquire(&broker,Broker).lease);
    }
    void existingGlobalHttpLimitWinsAndLocalRejectionDoesNotExtendIt() {
        qint64 now=0;auto limits=std::make_shared<TradingRequestLimits>([&]{return now;});KiwoomRequestBudget gate([&]{return now;},limits);QObject owner;
        HttpResult remote;remote.status=429;remote.headers["retry-after"]="120";limits->observe(QUrl("https://api.kiwoom.com"),remote);
        QCOMPARE(gate.remainingMs(),qint64(120000));QCOMPARE(gate.tryAcquire(&owner,Broker).waitMs,qint64(120000));
        now=500;HttpResult local;local.status=429;local.localRetryAfterSeconds=120;limits->observe(QUrl("https://api.kiwoom.com"),local);
        QCOMPARE(gate.remainingMs(),qint64(119500));now=120000;QVERIFY(gate.tryAcquire(&owner,Display).lease);
    }
    void backwardsClockCannotRenewAnEarlierAllowance() {
        qint64 now=1000;KiwoomRequestBudget gate([&]{return now;});QObject owner;
        const auto first=gate.tryAcquire(&owner,Display);QVERIFY(first.lease);QVERIFY(gate.finish(first.lease));now=900;
        QCOMPARE(gate.tryAcquire(&owner,Broker).lease,quint64(0));QVERIFY(gate.remainingMs()>0);
        now=1399;QCOMPARE(gate.tryAcquire(&owner,Broker).lease,quint64(0));now=1400;QVERIFY(gate.tryAcquire(&owner,Broker).lease);
    }
    void overflowingAChargedDeadlineCannotBecomeAnImmediateAllowance() {
        qint64 now=std::numeric_limits<qint64>::max()-400;KiwoomRequestBudget gate([&]{return now;});QObject owner;
        const auto first=gate.tryAcquire(&owner,Broker);QVERIFY(first.lease);QVERIFY(gate.finish(first.lease));now=std::numeric_limits<qint64>::max()-1;
        QCOMPARE(gate.tryAcquire(&owner,Display).lease,quint64(0));now=std::numeric_limits<qint64>::max();QCOMPARE(gate.tryAcquire(&owner,Display).lease,quint64(0));
    }
    void invalidOwnerAndEnumsNeverAcquire() {
        qint64 now=0;KiwoomRequestBudget gate([&]{return now;});QObject owner;
        QCOMPARE(gate.tryAcquire(nullptr,Display).lease,quint64(0));
        QCOMPARE(gate.tryAcquire(&owner,static_cast<KiwoomBudgetLane>(99)).lease,quint64(0));
        QCOMPARE(gate.tryAcquire(&owner,Broker,static_cast<KiwoomBudgetRequest>(99)).lease,quint64(0));
        QVERIFY(!gate.finish(0));QVERIFY(!gate.finish(999));
    }
};
QTEST_GUILESS_MAIN(KiwoomRequestBudgetTest)
#include "tst_kiwoom_request_budget.moc"
