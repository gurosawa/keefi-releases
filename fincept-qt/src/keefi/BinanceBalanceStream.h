#pragma once
#include <QObject>
#include <QUrl>
#include <memory>

namespace keefi {
class Vault;
class TradingRequestLimits;
// Private account socket, deliberately separate from the public price interface.
class BinanceBalanceSocket : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void open(const QUrl&) = 0;
    virtual void send(const QByteArray&) = 0;
    virtual void ping() = 0;
    virtual void abort() = 0;
signals:
    void connected();
    void received(const QByteArray&);
    void alive();
    void failed();
};
// Read-only invalidation feed. It never publishes partial balances or orders.
// Secrets are borrowed from the unlocked vault only while signing a subscription.
class BinanceBalanceStream final : public QObject {
    Q_OBJECT
public:
    explicit BinanceBalanceStream(Vault&, QObject* parent = nullptr);
    BinanceBalanceStream(Vault&, std::unique_ptr<BinanceBalanceSocket>,
                         std::shared_ptr<TradingRequestLimits> = {}, QObject* parent = nullptr);
    ~BinanceBalanceStream() override;
    void start(const QString& connectionId); // Explicit start/retry; live Spot only.
    void stop();
    bool ready() const;
    QString status() const;
signals:
    void changed();
    void balancesChanged(); // Subscribe/reconnect or account event: reconcile with REST.
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
