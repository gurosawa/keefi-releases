#pragma once
#include "keefi/AssetsCapture.h"
#include <QObject>
#include <memory>

namespace keefi {
class Vault;
class TradingTransport;
// Unsigned mainnet account reads only. No signing, transfers or order API.
class HyperliquidReadController final : public QObject {
    Q_OBJECT
  public:
    explicit HyperliquidReadController(Vault&, TradingTransport* = nullptr, QObject* = nullptr);
    ~HyperliquidReadController() override;
    static bool validAddress(const QString&);
    bool selectAccount(const QString&);
    AccountRecord account() const;
    bool refreshAssets();
    bool canReconnectAssets() const;
    void stop();
    bool interruptRead();
    bool busy() const;
    QString status() const;
    QString coverage() const;
    AssetCapture assetCapture() const;
    QList<PositionRecord> positions() const;
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
