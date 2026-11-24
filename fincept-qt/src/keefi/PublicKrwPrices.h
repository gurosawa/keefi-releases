#pragma once
#include "keefi/AssetsCapture.h"
#include <QObject>
#include <memory>
namespace keefi {
class TradingTransport;
class PublicQuoteStream;
class PublicMarketCatalog;
enum class PublicPriceStage { None, Catalogue, Quotes, BinanceCatalogue };
enum class PublicPriceFailure { None, RateLimited, Timeout, Network, Http, Json, Catalogue, Quote, Missing };
struct PublicPriceReport {
    PublicPriceStage stage=PublicPriceStage::None;
    PublicPriceFailure failure=PublicPriceFailure::None;
    int requested=0,updated=0,unsupported=0,unmapped=0,stale=0,failed=0,httpStatus=0,streamPending=0;
    bool catalogueKnown=false,localThrottle=false;
    qint64 retryAtMs=0;
};
// Public-only pricing. A caller supplies snapshots, never credentials; callers
// sharing a portfolio also share this request queue, catalogue and cooldown.
class PublicKrwPrices final : public QObject {
    Q_OBJECT
public:
    explicit PublicKrwPrices(TradingTransport*,QObject* parent=nullptr);
    ~PublicKrwPrices() override;
    void refresh(const QList<AssetSnapshot>&);
    void setPublicQuotes(const QList<AssetQuote>&,const QStringList& markets);
    void setPublicMarketCatalog(PublicMarketCatalog*);
    void setBinanceStream(PublicQuoteStream*);
    void clear();
    bool busy()const;
    QString status()const;
    QString details()const;
    PublicPriceReport report()const;
    // Display-only last official prices; never emitted as current-price updates.
    QList<AssetQuote> referenceQuotes()const;
signals:
    void changed();
    void completed(const QList<keefi::AssetQuote>& updates);
private:
    struct Private;std::unique_ptr<Private> d_;
};
}
