#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/EarnTracking.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class Vault;
class AssetsLedger;
class TradingTransport;
// Explicit read-only collector. It is not a PnL completeness oracle: page
// completion, official payout status and reconciled balance credit differ.
class BinanceEarnCollector final : public QObject {
    Q_OBJECT
public:
    struct TestHooks {
        std::function<qint64()> nowMs;
        int pacingMs = 2000;
    };
    explicit BinanceEarnCollector(Vault&,AssetsLedger&,TradingTransport* = nullptr,QObject* = nullptr);
    BinanceEarnCollector(Vault&,AssetsLedger&,TradingTransport*,TestHooks,QObject* = nullptr);
    ~BinanceEarnCollector() override;
    bool start(const AssetSnapshot& verifiedAccount);
    void stop();
    bool busy() const;
    QString status() const;
    QString errorCode() const;
signals:
    void started(const QString& accountScope);
    void changed();
    void finished(bool success);
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
