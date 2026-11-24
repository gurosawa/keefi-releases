#pragma once
#include "keefi/SystemNotificationSink.h"
#include <functional>
#include <memory>
#include <mutex>
#include <QSet>

namespace keefi {
// Portable admission and native callback seam. The macOS adapter uses the same
// gate as fake-port tests; no Objective-C notification center is used by tests.
class NativeNoticeAdmission final {
  public:
    static constexpr qsizetype RevocationLimit = 2048;
    bool permits(const SystemNotice&) const;
    bool revoke(const QStringList& identifiers); // False ends admission for this session.
    bool isRevoked(const QString& identifier) const;
    void setPrivateContext(quint64 epoch, bool enabled);
    void stop();
  private:
    mutable std::mutex mutex_;
    quint64 epoch_ = 0;
    bool enabled_ = false, stopped_ = false;
    bool revocationOverflow_ = false;
    QSet<QString> revoked_;
};
struct NativeNoticeReceipt {
    QString identifier;
    quint64 privateEpoch = 0;
    bool privateData = false, accepted = false;
};
struct NativeNoticeCallbacks {
    std::function<void(SystemNoticePermission, QString)> permission;
    std::function<void(NativeNoticeReceipt)> submitted;
    std::function<void(SystemNoticeAction)> action;
};
// Bodyless completion reconciliation remains valid after the QObject/port is
// gone. Native add callbacks retain the gate and a center-only purge closure.
NativeNoticeReceipt reconcileNativeNoticeReceipt(NativeNoticeReceipt,
    const std::shared_ptr<NativeNoticeAdmission>&,
    const std::function<void(const QStringList&)>& purge);
bool previousPrivateNoticeSession(const QString& identifier,const QString& noticeSession,const QString& currentSession);
class NativeNotificationPort {
  public:
    virtual ~NativeNotificationPort() = default;
    virtual void bind(NativeNoticeCallbacks, std::shared_ptr<NativeNoticeAdmission>) = 0;
    virtual void refreshPermission() = 0;
    virtual void requestAuthorization() = 0;
    virtual void add(const SystemNotice&) = 0;
    virtual void remove(const QStringList& identifiers) = 0;
    virtual void stop() = 0;
};
SystemNotificationSink* makeNativeSystemNotificationSink(std::unique_ptr<NativeNotificationPort>, QObject* parent = nullptr);
#ifdef Q_OS_MACOS
std::unique_ptr<NativeNotificationPort> makeMacSystemNotificationPort();
#endif
} // namespace keefi
