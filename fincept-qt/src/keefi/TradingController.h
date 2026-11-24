#pragma once
#include "keefi/TradingTypes.h"
#include "keefi/AssetsCapture.h"
#include <QObject>
#include <memory>
#include <functional>
class QWidget;
namespace keefi {
class Vault;
class TradingTransport;
class OkxOrderEvents;
// Read-only account service. The historical class name is retained for
// portfolio/session compatibility; no financial execution API exists.
class TradingController final : public QObject {
    Q_OBJECT
  public:
    explicit TradingController(Vault& vault, QObject* parent = nullptr);
    TradingController(Vault& vault, TradingTransport* transport, QObject* parent = nullptr,
                      std::function<qint64()> timingClock = {});
    TradingController(Vault& vault, TradingTransport* transport, OkxOrderEvents* events, QObject* parent = nullptr);
    ~TradingController() override;
    bool selectAccount(const QString& id);
    ExchangePolicy policy() const;
    void setDraft(const OrderInput& input);
    bool prepareDraft();
    TradingReadiness requestReadiness() const;
    TradingReadiness draftReadiness() const;
    TradingReadiness cancellationReadiness(const OrderRecord& order) const;
    bool prepareCancellation();
    bool cancelBlocked(const OrderRecord& order) const;
    AccountRecord account() const;
    AssetCapture assetCapture() const;
    bool assetReadComplete() const; // Completeness of the retained read, not a freshness guarantee.
    bool supported() const;
    bool busy() const;
    bool connected() const;
    bool liveEnabled() const;
    bool hasUnknown() const;
    bool quoteFresh() const;
    QString status() const;
    QString permissionStatus() const;
    QString coverage() const;
    QString quoteSummary() const;
    QString amendDescription() const;
    QString symbol() const;
    QList<MarketInstrument> instruments() const;
    QString catalogStatus() const;
    bool catalogFresh() const;
    QList<Balance> balances() const;
    QList<OrderRecord> openOrders() const;
    QList<OrderRecord> history() const;
    QList<OrderRecord> pendingOrders() const;
    OrderLimits limits() const;
    QJsonObject verificationReceipt() const;
    OrderValidation preview(const OrderInput& input) const;
    bool setLimits(const OrderLimits& limits);
    void refresh(const QString& symbol,bool includeHistory=false);
    // Account-wide read, independent of an instrument, history and order preparation.
    bool refreshAssets();
    bool canReconnectAssets() const;
    bool reconnectAssets();
    void setAutomaticRefresh(bool enabled);
    bool interruptRead();
    bool canInterruptRead() const;
    bool historyLoading() const;
    // Public exchange catalogue only; never starts authenticated account reads.
    void loadMarkets();
    // origin must be a visible enabled widget in the active trading window,
    // with focus. UI additionally admits only a direct, non-repeated mouse
    // release; confirmation dialogs are owned by that same manual UI.
    void reconcile(QWidget* origin);
    void stop();
  signals:
    void changed();
    void intentRevoked();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
