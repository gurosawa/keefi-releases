#pragma once
#include "keefi/PublicMarketCatalog.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/TradingTypes.h"
namespace keefi {
class ChartInstrumentCatalog;
enum class ChartMarketAvailability { Available, Checking, Unsupported };
struct ChartMarketResolution {
    WatchlistIdentity identity;
    ChartMarketAvailability availability = ChartMarketAvailability::Checking;
    QString reason;
    bool fallbackUsed = false;
    bool available() const { return availability == ChartMarketAvailability::Available; }
};
// Only official catalogue entries can authorize a route. No symbol suffix is
// used to infer a Binance instrument or to reinterpret its quote currency.
ChartMarketResolution resolveChartMarket(const WatchlistIdentity& requested,
    const QList<PublicMarketRow>& upbitRows, bool upbitAvailable,
    const QList<MarketInstrument>& binanceInstruments);
ChartMarketResolution resolveChartMarket(const WatchlistIdentity& requested,
    const PublicMarketCatalog* upbit, const ChartInstrumentCatalog* binance);
}
