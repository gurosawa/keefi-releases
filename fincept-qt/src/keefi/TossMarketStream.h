#pragma once
#include "keefi/AssetValuation.h"
#include <QByteArrayView>
#include <QObject>
#include <memory>
#include <functional>

namespace keefi {
class BrokerTradingController;
class TradingTransport;
struct TossMarketTarget {
    QString symbol, marketCountry; // KR / US, verified against broker metadata.
    int priority = 3; // 0 selected chart, 1 holdings, 2 visible watchlist, 3 remaining watchlist.
    bool operator==(const TossMarketTarget&) const = default;
};
struct TossMarketQuote {
    QString symbol, marketCountry, price, currency;
    AssetEvidence evidence;
};
class TossMarketSocket : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    // Native Qt handshake tests only: the endpoint is restricted to an IPv4
    // loopback port, and callers provide synthetic credentials. No remote URL.
    static std::unique_ptr<TossMarketSocket> forLoopbackTest(quint16 port);
    // Fixed Toss endpoint, synchronous borrowed bytes. Never retain the view.
    virtual void open(QByteArrayView token) = 0;
    virtual void send(const QByteArray&) = 0;
    virtual void abort() = 0;
  signals:
    void connected();
    void received(const QByteArray&);
    void failed(int httpStatus = 0);
};
// Display-only market context: intentionally not an AssetSnapshot/valuation source.
class TossMarketStream final : public QObject {
    Q_OBJECT
  public:
    using MonotonicClock = std::function<qint64()>;
    explicit TossMarketStream(BrokerTradingController*, TradingTransport*, QObject* parent = nullptr);
    TossMarketStream(BrokerTradingController*, TradingTransport*, std::unique_ptr<TossMarketSocket>, QObject* parent = nullptr);
    TossMarketStream(BrokerTradingController*, TradingTransport*, std::unique_ptr<TossMarketSocket>,
                     MonotonicClock, QObject* parent = nullptr);
    ~TossMarketStream() override;
    void setBroker(BrokerTradingController*);
    void setTargets(const QList<TossMarketTarget>&);
    void stop();
    QList<TossMarketTarget> targets() const;
    QList<TossMarketTarget> restOnlyTargets() const; // All verified overflow; REST requests rotate in batches of at most 200.
    QList<TossMarketQuote> quotes() const;
    QStringList rejectedTopics() const;
    QString status() const;
    QString coverage() const;
    static QString limitations();
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
