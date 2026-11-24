#pragma once

#include <QObject>
#include <QString>

namespace keefi {

// This controls app-owned secret handling, not administrator/debugger access or
// every OS diagnostic. A failed/unsupported check must prevent vault unlock.
class PlatformSecurity final : public QObject {
    Q_OBJECT
public:
    explicit PlatformSecurity(QObject* parent = nullptr);
    ~PlatformSecurity() override;

    static bool hardenCurrentProcess(QString* error = nullptr);
    bool initialize();
    bool ready() const { return ready_; }
    QString statusCode() const { return status_; }
    bool setKeepAwake(bool enabled);
    bool keepAwake() const { return keep_awake_; }

public slots:
    // Native adapters call this on the object's thread. It never unlocks.
    void requestLock(const QString& reason);

signals:
    void lockRequested(const QString& reason);
    void statusChanged();

private:
    bool ready_ = false;
    bool keep_awake_ = false;
    QString status_ = QStringLiteral("security_not_initialized");
    void* native_ = nullptr;
};

} // namespace keefi
