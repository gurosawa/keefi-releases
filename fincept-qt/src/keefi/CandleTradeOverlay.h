#pragma once
#include "keefi/MarketCandles.h"
#include <QJsonObject>
#include <QSet>

namespace keefi {
// The official candle stream remains authoritative. Binance's last-trade ID
// provides an exact fence for replaying a bounded, contiguous raw-trade tail.
// Upbit has no corresponding fence in its candle payload: only its individual
// latest trade is exposed, never a reconstructed OHLCV value.
class CandleTradeOverlay {
  public:
    static constexpr int BufferLimit=4096;
    void reset(const QString& identity={}, const QString& provider={},
               const QString& symbol={}, qint64 intervalMs=0);
    bool receive(const QJsonObject&, qint64 receivedAtMs);
    void setBinanceAnchor(const MarketCandle&, qint64 lastTradeId, qint64 sourceAtMs);
    void clearBinanceAnchor();
    MarketCandleLiveTrade observation(qint64 asOfMs) const;
    std::optional<MarketCandle> forming(qint64 asOfMs) const;

  private:
    struct Trade {
        qint64 id=-1, atMs=0, eventMs=0, receivedAtMs=0;
        QString price, quantity;
    };
    bool appendToOverlay(const Trade&);
    void rebuild();
    QString identity_, provider_, symbol_;
    qint64 intervalMs_=0, lastSeenId_=-1, lastSeenAtMs_=0;
    qint64 anchorId_=-1, anchorAtMs_=0, appliedId_=-1, minimumAnchorId_=-1;
    QList<Trade> buffered_;
    QSet<qint64> upbitIds_;
    std::optional<Trade> latest_;
    std::optional<MarketCandle> anchor_, overlay_;
};
} // namespace keefi
