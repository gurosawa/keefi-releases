#pragma once
#include "keefi/MarketMetrics.h"
#include <QMap>
#include <memory>
namespace keefi {
class CoinbaseQuotes final:public QObject {
    Q_OBJECT
  public:
    explicit CoinbaseQuotes(TradingTransport*,QObject* parent=nullptr,TradingTransport* bridgeTransport=nullptr);
    ~CoinbaseQuotes() override;
    void setAsset(const QString&);
    QMap<QString,AssetQuote> quotes(qint64 asOfMs=0) const;
    bool unlisted(const QString& base) const;
    static RequestSpec request(const QString& base);
    static bool isRequest(const RequestSpec&);
    static AssetQuote parse(const QString& base,const HttpResult&,qint64 receivedAtMs);
    static ExchangePriceGap priceGap(const AssetQuote& coinbase,const AssetQuote& binance,const AssetQuote& usdUsdt,qint64 asOfMs);
  signals:
    void changed();
    void quoteObserved(const QString& base); // Finished response only; stopping demand keeps passive display evidence.
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
