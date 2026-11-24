#pragma once
#include "keefi/AssetValuation.h"
#include <QObject>
#include <QUrl>
#include <memory>

namespace keefi {
struct PublicTick {
    QString market, previousClose, signedChangeRate, tradeValue24h;
    AssetQuote quote;
    // Optional public observation fields. The UTC-day counter is distinct from
    // rolling 24h turnover and must never be substituted with it.
    QString tradeValueUtcDay, tradeDateUtc;
    qint64 tradeTimestampMs = 0;
    bool realtime = false;
    quint64 streamEpoch = 0;
};
// No credential, account, order or arbitrary endpoint enters this interface.
class PublicTickerSocket : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void open(const QUrl&)=0;
    virtual void send(const QByteArray&)=0;
    virtual void ping()=0;
    virtual void abort()=0;
signals:
    void connected();
    void received(const QByteArray&);
    void alive();
    void failed();
};
class PublicQuoteStream final : public QObject {
    Q_OBJECT
public:
    enum class Provider { Upbit, Binance };
    explicit PublicQuoteStream(Provider,QObject* parent=nullptr);
    PublicQuoteStream(Provider,std::unique_ptr<PublicTickerSocket>,QObject* parent=nullptr);
    ~PublicQuoteStream() override;
    void setMarkets(QStringList); // Official catalogue codes only; an empty list stops the stream.
    QString status() const;
    quint64 streamEpoch() const; // Changes when a new socket connection begins; per-market ticks keep their original epoch.
signals:
    void ticks(const QList<keefi::PublicTick>&); // At most once each second; original times are retained.
    void changed();
private:
    struct Private;std::unique_ptr<Private> d_;
};
}
