#pragma once
// Display-only Kiwoom public prices. Reuses an existing Broker session.
#include "KiwoomMarketTypes.h"
#include "KiwoomMarketProtocol.h"
#include <QByteArrayView>
#include <QObject>
#include <functional>
#include <memory>
#include <optional>
namespace keefi {
class BrokerTradingController;
class TradingTransport;
class KiwoomRequestBudget;
class KiwoomMarketSocket : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    ~KiwoomMarketSocket() override=default;
    virtual void open(quint64 generation,const QString& country)=0; // Fixed KR/US URL only.
    virtual void sendLogin(quint64 generation,QByteArrayView borrowedToken)=0;
    virtual void sendControl(quint64 generation,const QByteArray& generatedPayload)=0;
    virtual void close(quint64 generation)=0;
    virtual void abort()=0;
  signals:
    void connected(quint64 generation);
    void received(quint64 generation,const QByteArray& payload);
    void closed(quint64 generation);
    void failed(quint64 generation,int httpStatus=0);
};
class KiwoomMarketStream final : public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    static constexpr int MaxRealtimeSymbols=200;
    static constexpr qint64 MetadataFreshnessMs=300000,QuoteFreshnessMs=180000; // KEEFI local display bounds, not provider freshness guarantees.
    explicit KiwoomMarketStream(BrokerTradingController*,TradingTransport*,QObject* parent=nullptr);
    KiwoomMarketStream(BrokerTradingController*,TradingTransport*,
        std::unique_ptr<KiwoomMarketSocket>,Clock monotonic={},Clock utc={},QObject* parent=nullptr,
        std::shared_ptr<KiwoomRequestBudget> budget={});
    ~KiwoomMarketStream() override;
    void setBroker(BrokerTradingController*);
    BrokerTradingController* broker() const; // Borrowed binding comparison only; no authentication/read.
    void setActiveCountry(const QString&); // KR,US,empty. Never two endpoints concurrently.
    void setTargets(const QList<KiwoomMarketTarget>&);
    // One KR stock whose ka10081 daily bars the chart needs; empty clears. Same borrowed session and budget.
    void setDailyTarget(const WatchlistIdentity&);
    KiwoomDailySeries daily(const WatchlistIdentity&) const;
    bool setVerifiedInstruments(const QList<KiwoomVerifiedInstrument>&,QString* error=nullptr);
    void stop();
    void tick(); // Same behavior as timer; fake clock tests, no hidden reconnect side channel.
    QList<KiwoomMarketTarget> realtimeTargets() const;
    QList<KiwoomMarketTarget> restOnlyTargets() const;
    QList<KiwoomMarketQuote> quotes(qint64 asOfMs=0) const;
    // Production path: exact public fetch -> atomic decode/currency proof -> current verified copies.
    // No read, authentication, freshness renewal or profile write is triggered by either getter.
    QList<KiwoomVerifiedInstrument> instruments() const;
    QList<KiwoomVerifiedInstrument> instruments(qint64 asOfMs) const;
    QString status() const;
    // False after a rejected/expired borrowed authorization until the user re-authenticates.
    bool authorizationUsable() const;
    static QString limitations();
    // Builders are unsigned prototypes. Dispatch obtains current borrowed auth
    // synchronously and revalidates the same revision at response receipt.
    static RequestSpec request(KiwoomDisplayRequest,const QString& country,
        const QString& symbol={},const QString& exchange={},const QByteArray& cursor={});
    static bool isRequest(const RequestSpec&);
    static QByteArray subscription(const QString& country,const QList<KiwoomMarketTarget>&,bool remove=false);
    static std::optional<KiwoomMarketQuote> parseQuote(const QByteArray& frame,
        const KiwoomVerifiedInstrument&,qint64 receivedAtMs);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
// Main-only factory. Constructor/default stream path does not secretly create real network.
std::unique_ptr<KiwoomMarketSocket> makeNetworkKiwoomMarketSocket();
}
