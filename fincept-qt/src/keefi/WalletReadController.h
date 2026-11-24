#pragma once
#include "keefi/AssetsCapture.h"
#include <QObject>
#include <memory>
namespace keefi {
class Vault;
class SwapTransport;
// Read-only Solana balances and public Jupiter mint metadata/prices.
// No browser, signing, swap quote or broadcast entry point.
class WalletReadController final : public QObject {
    Q_OBJECT
public:
    explicit WalletReadController(Vault&, SwapTransport* = nullptr, QObject* = nullptr);
    ~WalletReadController() override;
    bool selectAccount(const QString&);
    bool refresh();
    void stop();
    bool busy() const;
    QString status() const;
    AccountRecord account() const;
    AssetCapture assetCapture() const;
signals:
    void changed();
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
