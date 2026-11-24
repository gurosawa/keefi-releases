#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <memory>
#include <functional>
#include "keefi/AssetValuation.h"

namespace keefi {
class TradingTransport;
class PublicQuoteStream;

// One Upbit KRW market. Prices are the exact decimal strings from the official
// public ticker; nothing here is derived from an account or a credential.
struct PublicMarketRow {
    QString market, base, koreanName, englishName;
    bool warning = false, caution = false;
    bool priced = false;
    QString price, previousClose, signedChangeRate; // previousClose: UTC 00:00 (KST 09:00) close.
    qint64 sourceTimeMs = 0, receivedAtMs = 0;       // ticker `timestamp` and local receipt time.
    QString quoteUrl;
    QString tradeValue24h; // acc_trade_price_24h (KRW); empty when not reported. Sizes market-map tiles.
    QString tradeVolume24h; // Actual base-asset units from the 24h REST ticker; never inferred from price.
};

// Public-only Upbit KRW market list for exploration. It never needs an open
// vault or an account and is not cleared by locking; callers own the transport.
class PublicMarketCatalog final : public QObject {
    Q_OBJECT
  public:
    enum class MarketListState { NotRequested, Loading, Ready, Failed };
    using Clock = std::function<qint64()>; // Monotonic milliseconds; omitted in production.
    explicit PublicMarketCatalog(TradingTransport* transport, QObject* parent = nullptr, Clock clock = {});
    ~PublicMarketCatalog() override;
    void refresh();
    void retryMarketList(); // Explicit failed-metadata retry; preserves pacing/cooldown and in-flight exclusion.
    void setStream(PublicQuoteStream*);
    void setPresentationActive(bool active); // Default true; pause request demand without clearing public evidence.
    void setMetadataDemand(bool active); // Background public alert identity checks; never starts hidden quotes/WS.
    bool presentationActive() const;
    QList<AssetQuote> quotes() const;
    // Start with revision 0. A missed change or a replaced market list returns the full snapshot.
    QList<AssetQuote> quotesSince(quint64& revision) const;
    QSet<QString> marketCodes() const; // Last accepted list, including while metadata is stale.
    AssetQuote quoteForMarket(const QString& market) const;
    PublicMarketRow rowForMarket(const QString& market) const;
    bool busy() const;
    QList<PublicMarketRow> rows() const;
    bool marketListAvailable() const; // Official identity list freshness, independent of ticker success.
    MarketListState marketListState() const; // Last metadata request, not the independent quote/stream state.
    QString marketListError() const;
    qint64 retryAfterMs() const; // Existing source cooldown; presentation must never bypass it.
    QStringList verifiedMarkets() const; // Empty while identity metadata is unavailable/expired.
    QString status() const;
    qint64 completedAtMs() const; // 0 until a full or partial price pass finished.

  signals:
    void changed();

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};

// signed_change_rate (0.015 = +1.5%) as a percentage rounded to two decimals.
QString changePercentText(const QString& signedRate);

} // namespace keefi
