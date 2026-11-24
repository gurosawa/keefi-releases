#pragma once
#include "keefi/TradingTypes.h"
#include <QObject>
#include <memory>

namespace keefi {
struct OkxOrderEvent {
    quint64 generation = 0;
    QString accountScope, instId, ordId, reqId, amendResult, uTime;
    OrderRecord order;
};
class OkxOrderEvents : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start(const AccountRecord&, const CredentialSet&,
                       const QString& accountScope, qint64 serverNowMs) = 0;
    virtual void stop() = 0;
    virtual bool ready() const = 0;
    virtual QString status() const = 0;
    virtual quint64 generation() const = 0;
signals:
    void stateChanged();
    void orderEvent(const keefi::OkxOrderEvent& event);
};
// A narrow transport seam. Production owns QWebSocket; tests inject a fake
// socket. The receiver, not the socket caller, always selects the fixed URL.
class OkxEventSocket : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void open(const QUrl&) = 0;
    virtual void sendText(const QString&) = 0;
    virtual void abort() = 0;
signals:
    void connected();
    void textReceived(const QString&);
    void failed();
};
class NetworkOkxOrderEvents final : public OkxOrderEvents {
    Q_OBJECT
public:
    explicit NetworkOkxOrderEvents(QObject* parent = nullptr);
    // Dependency injection cannot override the receiver's endpoint/op allowlist.
    explicit NetworkOkxOrderEvents(std::unique_ptr<OkxEventSocket> socket, QObject* parent = nullptr);
    ~NetworkOkxOrderEvents() override;
    void start(const AccountRecord&, const CredentialSet&, const QString&, qint64) override;
    void stop() override;
    bool ready() const override;
    QString status() const override;
    quint64 generation() const override;
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
Q_DECLARE_METATYPE(keefi::OkxOrderEvent)
