#pragma once
#include "keefi/AssetsCapture.h"
#include "keefi/PortfolioHistory.h"
#include "keefi/CoinPerformance.h"
#include <QObject>
#include <functional>
#include <memory>
namespace keefi {
class Vault;class TradingTransport;class SwapTransport;class TradingController;class BrokerTradingController;
class FuturesTradingController;class WalletReadController;class AssetsLedger;
class PublicQuoteStream;
class PublicMarketCatalog;
class BinanceBalanceStream;
class BinanceEarnCollector;
class FundFlowCollector;
struct PortfolioConnection {
    AccountRecord account;
    QString state="not_read",stage,storageStatus,balanceStreamStatus;
    qint64 elapsedMs=0,receivedAtMs=0;
    bool active=false,shared=false;
    QString accountScope; // Verified successful read scope; multiple registrations may share one balance.
};
struct WalletReadSummary {
    bool balanceComplete = false, quantitiesConfirmed = false;
    int heldAssets = 0, openPositions = 0;
    QString usdcBalance;
};
class PortfolioController final : public QObject {
    Q_OBJECT
public:
    enum class RecordCheckpoint { Ready, Waiting, Failed };
    struct Factories {
        std::function<TradingTransport*(const AccountRecord&)> trading;
        std::function<SwapTransport*(const AccountRecord&)> wallet;
        std::function<TradingTransport*()> prices;
        // Monotonic scheduling clock; tests advance it without waiting real minutes.
        std::function<qint64()> monotonicMs;
        std::function<BinanceBalanceStream*(Vault&)> binanceBalances;
        std::function<TradingTransport*()> earn;
        std::function<TradingTransport*()> flow;
        std::function<qint64()> wallClockMs; // Coin performance "now"; tests pin it.
    };
    explicit PortfolioController(Vault&,QObject* = nullptr,Factories = {});
    ~PortfolioController() override;
    void setAutomaticRefresh(bool);
    void setBinanceBalancePilot(bool); // One live Binance Spot connection, after a successful REST read.
    void refreshAll();
    void refreshConnection(const QString&);
    bool canReconnectConnection(const QString&) const;
    bool reconnectConnection(const QString&); // Explicit user action; never called by automatic refresh.
    void stopReads();
    void suspend(const QString&,bool);
    QList<PortfolioConnection> connections() const;
    QList<AssetSnapshot> snapshots() const;
    std::optional<WalletReadSummary> completedWalletRead(const QString& connectionId) const;
    QList<AssetQuote> quotes() const;
    QList<AssetQuote> quotesForPair(const QString& base, const QString& currency) const;
    QList<AssetQuote> referenceQuotes() const;
    // Independent synchronous verification API: all current non-shared connections.
    // Pass the current valuation exclusions on every call. UI consumers use the
    // deferred API below and discard their copies on aboutToReset.
    PortfolioDayHistory dayHistory(const QSet<QString>& excludedAssets = {}, qint64 asOfMs = 0) const;
    // UI path: request before reading the result. Archive input is loaded once
    // per ledger revision/day; repeated requests re-evaluate only the current
    // endpoint. Completion/reset emits dayHistoryChanged. Pending results have
    // no private value; lock/account reset cancels outstanding work immediately.
    void requestDayHistory(const QSet<QString>& excludedAssets = {}, qint64 asOfMs = 0);
    PortfolioDayHistory dayHistoryResult() const;
    bool dayHistoryBusy() const;
    QList<ProviderReconciliationResult> providerReconciliations(qint64 asOfMs = 0) const;
    QString pricingStatus() const;
    QString pricingDiagnosticStatus() const;
    QString pricingDetails() const;
    bool pricingBusy() const;
    bool pricingPending() const; // Includes requested stream prices still awaiting a first quote.
    void refreshPrices();
    void setPublicPrices(const QList<AssetQuote>&,const QStringList& markets);
    void setPublicMarketCatalog(PublicMarketCatalog*);
    void setBinancePriceStream(PublicQuoteStream*);
    QString canonicalConnection(const QString&) const;
    TradingController* trading(const QString&);
    BrokerTradingController* broker(const QString&);
    FuturesTradingController* futures(const QString&);
    AssetsLedger* historyLedger();
    AssetsLedger* latestLedger();
    BinanceEarnCollector* earnCollector();
    // G9-C1-a coin investment PnL. Recomputed off the hot path; empty while locked.
    CoinPerformanceView coinPerformance() const;
    FundFlowCollector* flowCollector();
    void refreshCoinFlows();                 // Explicit: read every coin account again now.
    bool confirmUpbitPockets(bool confirmed); // User attestation, stored encrypted.
    // Owner thread only. Flush pending timers, then wait for durable writes;
    // never starts new reads or cancels/replays a financial operation.
    RecordCheckpoint checkpointRecords(QString* message=nullptr);
signals:
    void changed();
    void assetsChanged();
    void aboutToReset();
    void dayHistoryChanged();
    void coinPerformanceChanged();
private:
    struct Private;std::unique_ptr<Private> d_;
};
}
