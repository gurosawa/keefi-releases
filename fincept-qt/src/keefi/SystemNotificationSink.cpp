#include "keefi/SystemNotificationSink.h"
#include "keefi/SystemNotificationSink_p.h"
#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>

namespace keefi {
namespace {
class UnavailableSink final : public SystemNotificationSink {
  public:
    using SystemNotificationSink::SystemNotificationSink;
    SystemNoticePermission permission() const override { return SystemNoticePermission::Unavailable; }
    QString status() const override { return tr("이 운영체제의 시스템 알림 연결 없음"); }
    void refreshPermission() override {}
    void requestAuthorization() override {}
    void post(const SystemNotice& notice) override { emit submissionFinished(notice.identifier,notice.privateEpoch,false); }
    void remove(const QStringList&) override {}
    void setPrivateContext(quint64, bool) override {}
    void stop() override {}
};
class NativeSink;
struct NativeDispatchState {
    std::mutex mutex;
    QObject* context=nullptr;
    NativeSink* owner=nullptr;
    bool closed=false;
};
void enqueueNative(const std::shared_ptr<NativeDispatchState>&,std::function<void(NativeSink*)>);
bool validIdentifier(const QString& id){
    static const QRegularExpression grammar("\\Akeefi\\.g8\\.(public|private)\\.[A-Za-z0-9_.-]{1,120}\\z");
    return grammar.match(id).hasMatch();
}
class NativeSink final : public SystemNotificationSink {
  public:
    explicit NativeSink(std::unique_ptr<NativeNotificationPort> port,QObject* parent)
        :SystemNotificationSink(parent),port_(std::move(port)),admission_(std::make_shared<NativeNoticeAdmission>()),dispatch_(std::make_shared<NativeDispatchState>()){
        dispatch_->context=new QObject(this);dispatch_->owner=this;
        const auto state=dispatch_;
        NativeNoticeCallbacks callbacks;
        callbacks.permission=[state](SystemNoticePermission permission,QString status){
            enqueueNative(state,[permission,status=std::move(status)](NativeSink* owner){owner->permissionResult(permission,status);});
        };
        callbacks.submitted=[state](NativeNoticeReceipt receipt){
            enqueueNative(state,[receipt=std::move(receipt)](NativeSink* owner){owner->submissionResult(receipt);});
        };
        callbacks.action=[state](SystemNoticeAction action){
            enqueueNative(state,[action](NativeSink* owner){emit owner->actionRequested(action);});
        };
        port_->bind(std::move(callbacks),admission_);
    }
    ~NativeSink() override {stop();}
    SystemNoticePermission permission() const override {return permission_;}
    QString status() const override {return status_;}
    void refreshPermission() override {if(!stopped_)port_->refreshPermission();}
    void requestAuthorization() override {if(!stopped_)port_->requestAuthorization();}
    void post(const SystemNotice& notice) override {
        const auto prefix=notice.privateData?QStringLiteral("keefi.g8.private."):QStringLiteral("keefi.g8.public.");
        if(stopped_||permission_!=SystemNoticePermission::Granted||!admission_->permits(notice)||
           !validIdentifier(notice.identifier)||!notice.identifier.startsWith(prefix)||
           notice.title.isEmpty()||notice.title.size()>120||notice.body.isEmpty()||notice.body.size()>1000||
           notice.title.contains(QChar(0))||notice.body.contains(QChar(0))){
            emit submissionFinished(notice.identifier,notice.privateEpoch,false);return;
        }
        pending_.insert(notice.identifier,{notice.privateEpoch,notice.privateData});
        if(notice.privateData)privateIds_.insert(notice.identifier,notice.privateEpoch);
        port_->add(notice);
    }
    void remove(const QStringList& identifiers) override {
        QStringList accepted;for(const auto& id:identifiers)if(validIdentifier(id)&&!accepted.contains(id)){
            accepted.append(id);privateIds_.remove(id);pending_.remove(id);
        }
        if(!stopped_&&!accepted.isEmpty()){
            const bool withinLimit=admission_->revoke(accepted);port_->remove(accepted);
            if(!withinLimit){stop();status_=tr("취소 기록 한도에 도달해 알림을 멈췄어요. 앱을 다시 실행하면 재개할 수 있어요.");emit permissionChanged();}
        }
    }
    void setPrivateContext(quint64 epoch,bool enabled) override {
        if(stopped_)return;admission_->setPrivateContext(epoch,enabled);
        QStringList revoked;
        for(auto it=privateIds_.cbegin();it!=privateIds_.cend();++it)
            if(!admission_->permits({it.key(),{},{},true,it.value()}))revoked.append(it.key());
        for(const auto& id:revoked){privateIds_.remove(id);pending_.remove(id);}
        if(!revoked.isEmpty())port_->remove(revoked);
    }
    void stop() override {
        if(stopped_)return;stopped_=true;admission_->stop();
        {std::lock_guard lock(dispatch_->mutex);dispatch_->closed=true;dispatch_->owner=nullptr;dispatch_->context=nullptr;}
        const auto ids=privateIds_.keys();privateIds_.clear();pending_.clear();
        if(!ids.isEmpty())port_->remove(ids);port_->stop();
        permission_=SystemNoticePermission::Unavailable;status_=tr("시스템 알림 전달 중지");
    }
    void permissionResult(SystemNoticePermission permission,const QString& status){
        if(stopped_||(permission_==permission&&status_==status))return;
        permission_=permission;status_=status;emit permissionChanged();
    }
    void submissionResult(NativeNoticeReceipt receipt){
        if(stopped_)return;
        const auto original=receipt;
        receipt=reconcileNativeNoticeReceipt(std::move(receipt),admission_,[this](const QStringList& ids){port_->remove(ids);});
        if(original.privateData&&!admission_->permits({original.identifier,{},{},true,original.privateEpoch})){
            pending_.remove(original.identifier);privateIds_.remove(original.identifier);
            emit submissionFinished(original.identifier,original.privateEpoch,false);return;
        }
        const auto pending=pending_.find(receipt.identifier);
        if(pending==pending_.end()||pending->epoch!=receipt.privateEpoch||pending->privateData!=receipt.privateData)return;
        pending_.erase(pending);
        if(!receipt.accepted)privateIds_.remove(receipt.identifier);
        emit submissionFinished(receipt.identifier,receipt.privateEpoch,receipt.accepted);
    }
  private:
    struct Pending {quint64 epoch=0;bool privateData=false;};
    std::unique_ptr<NativeNotificationPort> port_;
    std::shared_ptr<NativeNoticeAdmission> admission_;
    std::shared_ptr<NativeDispatchState> dispatch_;
    QHash<QString,Pending> pending_;
    QHash<QString,quint64> privateIds_;
    SystemNoticePermission permission_=SystemNoticePermission::Unknown;
    QString status_=tr("시스템 알림 권한 확인 전");
    bool stopped_=false;
};
void enqueueNative(const std::shared_ptr<NativeDispatchState>& state,std::function<void(NativeSink*)> job){
    std::lock_guard lock(state->mutex);
    if(state->closed||!state->context)return;
    QMetaObject::invokeMethod(state->context,[state,job=std::move(job)]{
        NativeSink* owner=nullptr;
        {std::lock_guard guard(state->mutex);if(state->closed)return;owner=state->owner;}
        if(owner)job(owner);
    },Qt::QueuedConnection);
}
}
SystemNotificationSink* makeUnavailableSystemNotificationSink(QObject* parent) { return new UnavailableSink(parent); }
SystemNotificationSink* makeSystemNotificationSink(QObject* parent) {
#ifdef Q_OS_MACOS
    return makeNativeSystemNotificationSink(makeMacSystemNotificationPort(),parent);
#else
    return makeUnavailableSystemNotificationSink(parent);
#endif
}
bool NativeNoticeAdmission::permits(const SystemNotice& notice) const {
    std::lock_guard lock(mutex_);return !stopped_&&!revocationOverflow_&&!revoked_.contains(notice.identifier)&&
        (!notice.privateData||(enabled_&&notice.privateEpoch==epoch_));
}
bool NativeNoticeAdmission::revoke(const QStringList& identifiers){
    std::lock_guard lock(mutex_);
    if(revocationOverflow_)return false;
    for(const auto& id:identifiers)if(!revoked_.contains(id)){
        if(revoked_.size()>=RevocationLimit){revocationOverflow_=true;return false;}
        revoked_.insert(id);
    }
    return true;
}
bool NativeNoticeAdmission::isRevoked(const QString& identifier) const {
    std::lock_guard lock(mutex_);return revocationOverflow_||revoked_.contains(identifier);
}
void NativeNoticeAdmission::setPrivateContext(quint64 epoch,bool enabled){
    std::lock_guard lock(mutex_);if(stopped_||epoch<epoch_)return;epoch_=epoch;enabled_=enabled;
}
void NativeNoticeAdmission::stop(){std::lock_guard lock(mutex_);stopped_=true;enabled_=false;}
NativeNoticeReceipt reconcileNativeNoticeReceipt(NativeNoticeReceipt receipt,const std::shared_ptr<NativeNoticeAdmission>& admission,
    const std::function<void(const QStringList&)>& purge){
    if(!admission||!admission->permits({receipt.identifier,{},{},receipt.privateData,receipt.privateEpoch})){
        if((receipt.privateData||(admission&&admission->isRevoked(receipt.identifier)))&&purge)purge({receipt.identifier});receipt.accepted=false;
    }
    return receipt;
}
bool previousPrivateNoticeSession(const QString& identifier,const QString& noticeSession,const QString& currentSession){
    return identifier.startsWith("keefi.g8.private.")&&(currentSession.isEmpty()||noticeSession!=currentSession);
}
SystemNotificationSink* makeNativeSystemNotificationSink(std::unique_ptr<NativeNotificationPort> port,QObject* parent) {
    return port?new NativeSink(std::move(port),parent):makeUnavailableSystemNotificationSink(parent);
}
} // namespace keefi
