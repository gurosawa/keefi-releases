#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/FundFlowTracking.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class Vault;
class AssetsLedger;
class TradingTransport;
// Read-only collector of Upbit/Binance deposits, withdrawals and Binance
// wallet transfers for one verified account at a time. A finished run means
// the rows were read and stored; it is not a completeness verdict for PnL.
class FundFlowCollector final : public QObject {
    Q_OBJECT
public:
    struct TestHooks {
        std::function<qint64()> nowMs;
        int binancePacingMs = 300, upbitPacingMs = 150;
    };
    explicit FundFlowCollector(Vault&, AssetsLedger&, TradingTransport* = nullptr, QObject* = nullptr);
    FundFlowCollector(Vault&, AssetsLedger&, TradingTransport*, TestHooks, QObject* = nullptr);
    ~FundFlowCollector() override;
    static bool supports(const AssetSnapshot&);
    bool start(const AssetSnapshot& verifiedAccount);
    // Stores or withdraws the user's "no Upbit pocket/staking" confirmation.
    bool setUpbitConfirmation(const QString& accountScope, bool confirmed);
    void stop();
    bool busy() const;
    QString status() const;
    QString errorCode() const;
    QString accountScope() const;
signals:
    void started(const QString& accountScope);
    void changed();
    void finished(bool success);
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
