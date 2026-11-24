#pragma once
#include "keefi/MovementDirection.h"
#include <QObject>
#include <QStringList>

namespace keefi {
enum class SystemNoticePermission { Unknown, Denied, Granted, Unavailable };
enum class SystemNoticeAction { OpenWindow, DisableDrip };
struct SystemNotice {
    QString identifier, title, body;
    bool privateData = false;
    quint64 privateEpoch = 0;
    MovementDirection direction = MovementDirection::Unknown;
    bool fixedRisk = false;
    bool playSound = false; // OS default sound only, explicit intent; no custom asset.
};

// Local system delivery only. Construction never requests permission or posts.
// submissionFinished means accepted for scheduling, not displayed or read.
// Public methods run on the QObject owner's thread; native callbacks are queued
// there. setPrivateContext uses a monotonic session epoch, including on locks.
class SystemNotificationSink : public QObject {
    Q_OBJECT
  public:
    explicit SystemNotificationSink(QObject* parent = nullptr) : QObject(parent) {}
    ~SystemNotificationSink() override = default;
    virtual SystemNoticePermission permission() const = 0;
    virtual QString status() const = 0;
    virtual void refreshPermission() = 0;
    virtual void requestAuthorization() = 0;
    virtual void post(const SystemNotice&) = 0;
    virtual void remove(const QStringList& identifiers) = 0;
    virtual void setPrivateContext(quint64 epoch, bool enabled) = 0;
    virtual void stop() = 0;
  signals:
    void permissionChanged();
    void submissionFinished(QString identifier, quint64 privateEpoch, bool accepted);
    void actionRequested(keefi::SystemNoticeAction);
};

// QObject parent owns the returned sink. Tests and smoke use unavailable/fake.
SystemNotificationSink* makeSystemNotificationSink(QObject* parent = nullptr);
SystemNotificationSink* makeUnavailableSystemNotificationSink(QObject* parent = nullptr);
} // namespace keefi
Q_DECLARE_METATYPE(keefi::SystemNoticePermission)
Q_DECLARE_METATYPE(keefi::SystemNoticeAction)
