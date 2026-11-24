#pragma once
#include "keefi/AssetsCapture.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class Vault;
class TradingTransport;

// Read-only OKX unified-account equity and SWAP/FUTURES positions. Contract
// exposure is informational; it is never added to the unified-account equity.
class OkxFuturesReadController final : public QObject {
    Q_OBJECT
public:
    explicit OkxFuturesReadController(Vault&, TradingTransport* = nullptr, QObject* = nullptr,
                                     std::function<qint64()> timingClock = {});
    ~OkxFuturesReadController() override;
    bool selectAccount(const QString&);
    bool refreshAssets();
    bool canReconnectAssets() const;
    bool reconnectAssets();
    bool interruptRead();
    void stop();
    bool busy() const;
    bool connected() const;
    QString status() const;
    AccountRecord account() const;
    AssetCapture assetCapture() const;
    QList<PositionRecord> positions() const;
signals:
    void changed();
    // Emitted on the first verified UID and every subsequent identity change.
    // Scopes are hashes; raw UIDs never cross this interface.
    void identityChanged(const QString& oldScope,const QString& newScope);
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
