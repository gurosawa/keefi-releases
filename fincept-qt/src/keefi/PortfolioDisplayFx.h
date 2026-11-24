#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/TradingTypes.h"

namespace keefi {
// Transient public Coinbase leg, independent of the visible market's demand.
class PortfolioDisplayFx final {
  public:
    void observeCoinbase(const AssetQuote& quote, qint64 asOfMs) {
        const auto sourceTime = quote.evidence.sourceTimeMs;
        if (sourceTime > 0 && sourceTime < sourceWatermark_) return;
        if (!usable(quote, asOfMs)) { invalidate(); return; }
        if (quote.evidence.sourceTimeMs == sourceWatermark_) {
            if (!quote_.price.isEmpty() && decimal::normalize(quote.price) != decimal::normalize(quote_.price)) invalidate();
            return;
        }
        sourceWatermark_ = quote.evidence.sourceTimeMs;
        quote_ = quote;
    }
    // Keep the source watermark: an old reply cannot revive a failed observation.
    void invalidate() { quote_ = {}; }
    QList<AssetQuote> quotes(qint64 asOfMs) const {
        return usable(quote_, asOfMs) ? QList<AssetQuote>{quote_} : QList<AssetQuote>{};
    }
  private:
    static bool usable(const AssetQuote& quote, qint64 asOfMs) {
        const auto& evidence = quote.evidence;
        return quote.baseAsset == "USDT" && quote.quoteAsset == "USD" && decimal::positive(quote.price) &&
            evidence.complete && !evidence.source.trimmed().isEmpty() &&
            evidence.scope == "public:coinbase:USDT-USD" &&
            evidence.url == "https://api.exchange.coinbase.com/products/USDT-USD/ticker" &&
            asOfMs > 0 && evidence.sourceTimeMs > 0 && evidence.receivedAtMs > 0 &&
            evidence.sourceTimeMs <= evidence.receivedAtMs && evidence.receivedAtMs <= asOfMs &&
            asOfMs - evidence.sourceTimeMs <= DisplayFreshnessMs &&
            asOfMs - evidence.receivedAtMs <= DisplayFreshnessMs;
    }
    AssetQuote quote_;
    qint64 sourceWatermark_ = 0;
};
}
