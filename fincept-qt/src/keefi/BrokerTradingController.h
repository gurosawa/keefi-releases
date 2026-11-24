#pragma once

#include "keefi/BrokerAdapter.h"
#include "keefi/AssetsCapture.h"
#include <QMap>
#include <QObject>
#include <QJsonArray>
#include <memory>
#include <functional>

class QWidget;

namespace keefi {
class Vault;
class TradingTransport;
class KiwoomRequestBudget;

class BrokerTradingController final : public QObject {
    Q_OBJECT
  public:
    using AdapterFactory = std::function<std::unique_ptr<BrokerAdapter>(const AccountRecord&)>;
    explicit BrokerTradingController(Vault& vault, TradingTransport* transport = nullptr, QObject* parent = nullptr,
                                     AdapterFactory adapterFactory = {}, std::shared_ptr<KiwoomRequestBudget> kiwoomBudget = {});
    ~BrokerTradingController() override;
    bool selectConnection(const QString& id);
    void stop();
    AccountRecord account() const;
    QString credentialSessionIdentity() const; // Opaque internal fingerprint; never display or upload.
    AssetCapture assetCapture() const;
    bool supported() const;
    bool authenticated() const;
    bool withMarketAuthorization(const BrokerAdapter::MarketAuthorizationBorrow&) const;
    QString marketDataCountry(const QString& symbol) const;
    QStringList marketDataHoldings() const; // Fresh official positive holdings, KR and US, display-only.
    bool connected() const;
    bool busy() const;
    // Common connection/queue/throttle gate. Foreground, vault and per-order
    // safety checks are still enforced at the direct action and send boundaries.
    BrokerReadiness requestReadiness() const;
    bool liveEnabled() const;
    bool halted() const;
    bool quoteFresh() const;
    QString status() const;
    QString coverage() const;
    QString permissionSummary() const;
    QString symbol() const;
    QString selectedAccountLabel() const;
    QList<BrokerAccountChoice> accountChoices() const;
    bool selectBrokerAccount(const QString& handle);
    bool canAuthenticate() const;
    void authenticate();
    void refresh(const QString& symbol,bool includeHistory=false);
    void loadHistory();
    bool refreshAssets();
    void setAutomaticRefresh(bool enabled);
    bool interruptRead();
    bool canInterruptRead() const;
    bool historyLoading() const;
    BrokerPolicy policy() const;
    // Input changes schedule only read-only preflight. Completion never submits.
    void setDraft(const OrderInput& input);
    BrokerReadiness draftReadiness() const;
    void prepareCancellation(); // Read-only; completion never queues a cancel.
    BrokerReadiness cancellationReadiness(const OrderRecord& order) const;
    void loadMarkets();
    QList<MarketInstrument> instruments() const;
    QString catalogStatus() const;
    bool catalogFresh() const;
    QJsonObject holdings() const;
    QJsonObject accountSummary() const;
    QJsonArray catalogSources() const;
    QJsonObject buyingPower() const;
    QString quoteSummary() const;
    QList<OrderRecord> openOrders() const;
    QList<OrderRecord> history() const;
    QList<OrderRecord> unresolvedOrders() const;
    bool cancelBlocked(const OrderRecord& order) const;
    QList<OrderRecord> actionResults() const;
    OrderLimits limits() const;
    bool setLimits(const OrderLimits& limits);
    OrderValidation preview(const OrderInput& input) const;
    void reconcile();
    QJsonObject verificationReceipt() const;

  signals:
    void changed();
    void intentRevoked();

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
