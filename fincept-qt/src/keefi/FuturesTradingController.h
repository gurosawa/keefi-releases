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
// Read-only USD(S)-M balances, positions and order history service.
class FuturesTradingController final : public QObject {
    Q_OBJECT
public:
    explicit FuturesTradingController(Vault&, QObject* parent=nullptr);
    FuturesTradingController(Vault&, TradingTransport*, QObject* parent=nullptr,
                             std::function<qint64()> timingClock={});
    ~FuturesTradingController() override;
    bool selectAccount(const QString&);
    AccountRecord account() const;
    AssetCapture assetCapture() const;
    bool supported() const;
    bool busy() const;
    bool connected() const;
    bool liveEnabled() const;
    bool halted() const;
    bool hasUnknown() const;
    bool quoteFresh() const;
    bool orderTestPassed() const;
    bool orderTestRequired() const;
    QString status() const;
    QString permissionStatus() const;
    QString coverage() const;
    QString quoteSummary() const;
    QString symbol() const;
    QList<MarketInstrument> instruments() const;
    QString catalogStatus() const;
    bool catalogFresh() const;
    QList<Balance> balances() const;
    QList<PositionRecord> positions() const;
    QList<OrderRecord> openOrders() const;
    QList<OrderRecord> history() const;
    QList<OrderRecord> pendingOrders() const;
    QList<OrderRecord> actionResults() const;
    QJsonObject verificationReceipt() const;
    OrderLimits limits() const;
    OrderValidation preview(const OrderInput&) const;
    bool setLimits(const OrderLimits&);
    void setDraft(const OrderInput&);
    bool prepareDraft();
    bool prepareCancellation();
    TradingReadiness requestReadiness() const;
    TradingReadiness draftReadiness() const;
    TradingReadiness cancellationReadiness(const OrderRecord&) const;
    void loadMarkets();
    void refresh(const QString& symbol,bool includeHistory=false);
    void loadHistory();
    bool refreshAssets();
    bool canReconnectAssets() const;
    bool reconnectAssets();
    bool interruptRead();
    bool canInterruptRead()const;
    bool historyLoading()const;
    void reconcile(QWidget* origin);
    void stop();
signals:
    void changed();
    void intentRevoked();
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
