#include "keefi/SystemNotificationSink.h"
#include "keefi/SystemNotificationSink_p.h"
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <thread>
using namespace keefi;
namespace {
struct FakeNativeState {
    NativeNoticeCallbacks callbacks;
    std::shared_ptr<NativeNoticeAdmission> admission;
    QList<SystemNotice> added;
    QList<QStringList> removed;
    int binds=0, refreshes=0, authorizations=0, stops=0;
};
class FakeNativePort final:public NativeNotificationPort {
  public:
    explicit FakeNativePort(std::shared_ptr<FakeNativeState> state):state_(std::move(state)){}
    void bind(NativeNoticeCallbacks callbacks,std::shared_ptr<NativeNoticeAdmission> admission) override {
        ++state_->binds;state_->callbacks=std::move(callbacks);state_->admission=std::move(admission);
    }
    void refreshPermission() override {++state_->refreshes;}
    void requestAuthorization() override {++state_->authorizations;}
    void add(const SystemNotice& notice) override {state_->added.append(notice);}
    void remove(const QStringList& identifiers) override {state_->removed.append(identifiers);}
    void stop() override {++state_->stops;}
  private:
    std::shared_ptr<FakeNativeState> state_;
};
SystemNotice publicNotice(){return {"keefi.g8.public.synthetic-1","합성 KEEFI 알림","합성 완료 봉 신호",false,0};}
SystemNotice privateNotice(quint64 epoch){return {"keefi.g8.private.synthetic-1","합성 보유 알림","합성 표시 전용 신호",true,epoch};}
}
class SystemNotificationSinkTest:public QObject {
    Q_OBJECT
  private slots:
    void unavailableRejectsSubmissionWithoutNativeEffects();
    void nativeSetupDoesNotRequestPermissionOrPost();
    void soundIntentReachesNativePortWithoutPermissionPrompt();
    void admissionRejectsLockedOldEpochAndStoppedWork();
    void privateLockPurgesAndLateAcceptanceCannotRestoreIt();
    void callbacksReturnToOwnerThreadAndGlobalActionsSurviveLock();
    void callbacksAfterDestructionAreHarmless();
    void bodylessCompletionStillPurgesAfterOwnerHasGone();
    void unpermittedOrMalformedNoticesNeverReachNativePort();
    void explicitRemovalRejectsLateSameEpochAcceptance();
    void startupScanPreservesCurrentSessionAndPurgesOnlyPreviousPrivateIds();
    void revocationRegistryHasAFailClosedSessionLimit();
};
void SystemNotificationSinkTest::unavailableRejectsSubmissionWithoutNativeEffects(){
    std::unique_ptr<SystemNotificationSink> sink(makeUnavailableSystemNotificationSink());
    QCOMPARE(sink->permission(),SystemNoticePermission::Unavailable);QVERIFY(!sink->status().isEmpty());
    QSignalSpy submissions(sink.get(),&SystemNotificationSink::submissionFinished);
    sink->refreshPermission();sink->requestAuthorization();sink->post(publicNotice());
    QCOMPARE(submissions.size(),1);QCOMPARE(submissions.first()[0].toString(),publicNotice().identifier);QCOMPARE(submissions.first()[2].toBool(),false);
    sink->remove({publicNotice().identifier});sink->setPrivateContext(1,false);sink->stop();
    QCOMPARE(sink->permission(),SystemNoticePermission::Unavailable);
}
void SystemNotificationSinkTest::nativeSetupDoesNotRequestPermissionOrPost(){
    const auto native=std::make_shared<FakeNativeState>();
    std::unique_ptr<SystemNotificationSink> sink(makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native)));
    QCOMPARE(native->binds,1);QCOMPARE(native->refreshes,0);QCOMPARE(native->authorizations,0);QVERIFY(native->added.isEmpty());
    QCOMPARE(sink->permission(),SystemNoticePermission::Unknown);
    sink->refreshPermission();QCOMPARE(native->refreshes,1);QCOMPARE(native->authorizations,0);
    sink->requestAuthorization();QCOMPARE(native->authorizations,1);QVERIFY(native->added.isEmpty());
}
void SystemNotificationSinkTest::soundIntentReachesNativePortWithoutPermissionPrompt(){
    const auto native=std::make_shared<FakeNativeState>();std::unique_ptr<SystemNotificationSink> sink(makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native)));
    native->callbacks.permission(SystemNoticePermission::Granted,"합성 허용");QTRY_COMPARE(sink->permission(),SystemNoticePermission::Granted);
    auto silent=publicNotice();QVERIFY(!silent.playSound);sink->post(silent);QCOMPARE(native->added.size(),1);QVERIFY(!native->added.last().playSound);
    auto audible=publicNotice();audible.identifier="keefi.g8.public.synthetic-sound";audible.playSound=true;sink->post(audible);QCOMPARE(native->added.size(),2);QVERIFY(native->added.last().playSound);QCOMPARE(native->authorizations,0);
}
void SystemNotificationSinkTest::admissionRejectsLockedOldEpochAndStoppedWork(){
    NativeNoticeAdmission gate;QVERIFY(gate.permits(publicNotice()));QVERIFY(!gate.permits(privateNotice(1)));
    gate.setPrivateContext(7,true);QVERIFY(gate.permits(privateNotice(7)));QVERIFY(!gate.permits(privateNotice(6)));
    gate.setPrivateContext(8,false);QVERIFY(!gate.permits(privateNotice(7)));QVERIFY(!gate.permits(privateNotice(8)));QVERIFY(gate.permits(publicNotice()));
    gate.setPrivateContext(7,true);QVERIFY(!gate.permits(privateNotice(7)));QVERIFY(!gate.permits(privateNotice(8)));
    gate.stop();QVERIFY(!gate.permits(publicNotice()));gate.setPrivateContext(9,true);QVERIFY(!gate.permits(privateNotice(9)));
}
void SystemNotificationSinkTest::privateLockPurgesAndLateAcceptanceCannotRestoreIt(){
    const auto native=std::make_shared<FakeNativeState>();
    std::unique_ptr<SystemNotificationSink> sink(makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native)));
    QVERIFY(native->callbacks.permission);native->callbacks.permission(SystemNoticePermission::Granted,"합성 허용");
    QTRY_COMPARE(sink->permission(),SystemNoticePermission::Granted);
    QSignalSpy submissions(sink.get(),&SystemNotificationSink::submissionFinished);
    sink->setPrivateContext(7,true);const auto notice=privateNotice(7);sink->post(notice);QCOMPARE(native->added.size(),1);
    sink->setPrivateContext(8,false);QVERIFY(!native->removed.isEmpty());QVERIFY(native->removed.last().contains(notice.identifier));
    const int removals=native->removed.size();QVERIFY(native->callbacks.submitted);
    native->callbacks.submitted({notice.identifier,7,true,true});
    QTRY_VERIFY(native->removed.size()>removals);QCOMPARE(submissions.size(),1);QCOMPARE(submissions.last()[2].toBool(),false);
    sink->post(privateNotice(8));QCOMPARE(native->added.size(),1);QCOMPARE(submissions.last()[2].toBool(),false);
    sink->post(publicNotice());QCOMPARE(native->added.size(),2);
}
void SystemNotificationSinkTest::callbacksReturnToOwnerThreadAndGlobalActionsSurviveLock(){
    const auto native=std::make_shared<FakeNativeState>();
    std::unique_ptr<SystemNotificationSink> sink(makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native)));
    QVERIFY(native->callbacks.permission);bool ownerThread=false;
    connect(sink.get(),&SystemNotificationSink::permissionChanged,sink.get(),[&]{ownerThread=QThread::currentThread()==sink->thread();});
    const auto permissionCallback=native->callbacks.permission;
    std::thread worker([permissionCallback]{permissionCallback(SystemNoticePermission::Granted,"합성 worker 응답");});worker.join();
    QTRY_COMPARE(sink->permission(),SystemNoticePermission::Granted);QVERIFY(ownerThread);
    sink->setPrivateContext(2,false);QSignalSpy actions(sink.get(),&SystemNotificationSink::actionRequested);QVERIFY(native->callbacks.action);
    native->callbacks.action(SystemNoticeAction::DisableDrip);native->callbacks.action(SystemNoticeAction::OpenWindow);
    QTRY_COMPARE(actions.size(),2);QCOMPARE(qvariant_cast<SystemNoticeAction>(actions[0][0]),SystemNoticeAction::DisableDrip);
    sink->stop();native->callbacks.action(SystemNoticeAction::DisableDrip);QCoreApplication::processEvents();QCOMPARE(actions.size(),2);
}
void SystemNotificationSinkTest::callbacksAfterDestructionAreHarmless(){
    const auto native=std::make_shared<FakeNativeState>();
    auto* sink=makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native));
    QVERIFY(native->callbacks.permission);QVERIFY(native->callbacks.submitted);QVERIFY(native->callbacks.action);
    const auto callbacks=native->callbacks;delete sink;QCOMPARE(native->stops,1);
    std::thread worker([callbacks]{callbacks.permission(SystemNoticePermission::Granted,"합성 늦은 응답");
        callbacks.submitted({"keefi.g8.private.synthetic-late",1,true,true});callbacks.action(SystemNoticeAction::OpenWindow);});worker.join();
    QCoreApplication::processEvents();QVERIFY(native->admission);QVERIFY(!native->admission->permits(publicNotice()));
}
void SystemNotificationSinkTest::bodylessCompletionStillPurgesAfterOwnerHasGone(){
    const auto admission=std::make_shared<NativeNoticeAdmission>();admission->setPrivateContext(7,true);
    const NativeNoticeReceipt late{"keefi.g8.private.synthetic-owner-gone",7,true,true};
    QStringList removed;admission->stop();
    const auto result=reconcileNativeNoticeReceipt(late,admission,[&](const QStringList& ids){removed+=ids;});
    QCOMPARE(removed,QStringList{late.identifier});QVERIFY(!result.accepted);
    const auto publicResult=reconcileNativeNoticeReceipt({publicNotice().identifier,0,false,true},admission,[&](const QStringList& ids){removed+=ids;});
    QCOMPARE(removed.size(),1);QVERIFY(!publicResult.accepted);
}
void SystemNotificationSinkTest::unpermittedOrMalformedNoticesNeverReachNativePort(){
    const auto native=std::make_shared<FakeNativeState>();
    std::unique_ptr<SystemNotificationSink> sink(makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native)));
    QSignalSpy submissions(sink.get(),&SystemNotificationSink::submissionFinished);
    sink->post(publicNotice());QCOMPARE(native->authorizations,0);QVERIFY(native->added.isEmpty());QCOMPARE(submissions.size(),1);
    native->callbacks.permission(SystemNoticePermission::Denied,"합성 거부");QTRY_COMPARE(sink->permission(),SystemNoticePermission::Denied);
    sink->post(publicNotice());QVERIFY(native->added.isEmpty());
    native->callbacks.permission(SystemNoticePermission::Granted,"합성 허용");QTRY_COMPARE(sink->permission(),SystemNoticePermission::Granted);
    auto invalid=publicNotice();invalid.identifier="synthetic-not-keefi-owned";sink->post(invalid);
    invalid=publicNotice();invalid.identifier="keefi.g8.private.synthetic-wrong-class";sink->post(invalid);
    invalid=publicNotice();invalid.body.clear();sink->post(invalid);
    invalid=publicNotice();invalid.body=QString(1001,'x');sink->post(invalid);
    invalid=publicNotice();invalid.body.append(QChar(0));sink->post(invalid);
    QVERIFY(native->added.isEmpty());QCOMPARE(native->authorizations,0);
    sink->post(publicNotice());QCOMPARE(native->added.size(),1);
}
void SystemNotificationSinkTest::explicitRemovalRejectsLateSameEpochAcceptance(){
    const auto native=std::make_shared<FakeNativeState>();
    std::unique_ptr<SystemNotificationSink> sink(makeNativeSystemNotificationSink(std::make_unique<FakeNativePort>(native)));
    native->callbacks.permission(SystemNoticePermission::Granted,"합성 허용");QTRY_COMPARE(sink->permission(),SystemNoticePermission::Granted);
    sink->setPrivateContext(7,true);const auto notice=privateNotice(7);
    sink->post(notice);QCOMPARE(native->added.size(),1);
    // An ordinary failed submission can retry the same opaque ID.
    native->callbacks.submitted({notice.identifier,7,true,false});QCoreApplication::processEvents();
    sink->post(notice);QCOMPARE(native->added.size(),2);
    sink->remove({notice.identifier});const int removals=native->removed.size();QVERIFY(removals>0);
    // This is the bodyless helper used by the actual macOS add completion.
    const auto late=reconcileNativeNoticeReceipt({notice.identifier,7,true,true},native->admission,[&](const QStringList& ids){native->removed.append(ids);});
    QVERIFY2(!late.accepted,"Explicit cancellation must reject a late native acceptance even while the private epoch stays enabled");
    QCOMPARE(native->removed.size(),removals+1);QVERIFY(!native->admission->permits(notice));
    native->callbacks.submitted(late);QCoreApplication::processEvents();
    sink->post(notice);QCOMPARE(native->added.size(),2);
}
void SystemNotificationSinkTest::startupScanPreservesCurrentSessionAndPurgesOnlyPreviousPrivateIds(){
    const auto id=privateNotice(7).identifier;
    // The startup scan can complete after this session has submitted a new
    // private notice. The actual macOS scan uses this routing-only predicate.
    QVERIFY(!previousPrivateNoticeSession(id,"synthetic-current-session","synthetic-current-session"));
    QVERIFY(previousPrivateNoticeSession(id,"synthetic-previous-session","synthetic-current-session"));
    QVERIFY(previousPrivateNoticeSession(id,{},"synthetic-current-session"));
    QVERIFY(!previousPrivateNoticeSession(publicNotice().identifier,"synthetic-previous-session","synthetic-current-session"));
    QVERIFY(!previousPrivateNoticeSession("other.notification",{},"synthetic-current-session"));
}
void SystemNotificationSinkTest::revocationRegistryHasAFailClosedSessionLimit(){
    const auto admission=std::make_shared<NativeNoticeAdmission>();QStringList ids;
    for(qsizetype i=0;i<NativeNoticeAdmission::RevocationLimit;++i)ids.append("keefi.g8.public.synthetic-revoked-"+QString::number(i));
    QVERIFY(admission->revoke(ids));QVERIFY(!admission->permits({ids.first(),{},{},false,0}));
    QVERIFY(admission->revoke({ids.first()})); // Repeated cancellation uses no new slot.
    QVERIFY(!admission->revoke({"keefi.g8.public.synthetic-over-limit"}));
    QVERIFY(!admission->permits(publicNotice()));admission->setPrivateContext(7,true);QVERIFY(!admission->permits(privateNotice(7)));
    QStringList purged;
    const auto receipt=reconcileNativeNoticeReceipt({publicNotice().identifier,0,false,true},admission,[&](const QStringList& values){purged+=values;});
    QVERIFY(!receipt.accepted);QCOMPARE(purged,QStringList{publicNotice().identifier});
}
QTEST_GUILESS_MAIN(SystemNotificationSinkTest)
#include "tst_system_notification_sink.moc"
