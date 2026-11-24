#pragma once
#include "keefi/AssetIdentity.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <optional>

namespace keefi {
enum class HoldingCustodyState { Fresh, Delayed, Failed };
struct HoldingChartCustody {
    QString provider, label;
    HoldingCustodyState state = HoldingCustodyState::Delayed;
    qint64 observedAtMs = 0;
    bool operator==(const HoldingChartCustody&) const = default;
};
// Private, transient display facts. No quantities, account aliases or addresses.
struct HoldingChartRow {
    AssetKey assetKey;
    QString displayName, symbol;
    QList<HoldingChartCustody> custodies;
    std::optional<WatchlistIdentity> chart;
    QList<WatchlistIdentity> candidates;
    QString chartReason, valuationMarket;
    int selectionRule = 0; // 1 public list, 2 custody, 3 display currency, 4 verified fallback; 0 session/no market.
    bool alertsEligible = false;
    qint64 observedAtMs = 0;
    bool operator==(const HoldingChartRow&) const = default;
};
struct HoldingMarketCatalog {
    QList<PublicMarketRow> upbitRows;
    bool upbitAvailable = false;
    QList<MarketInstrument> binance, bithumb;
    QList<WatchlistIdentity> verifiedStocks;
};
struct HoldingMarketPreferences {
    // Existing public lists flattened in recent-list order, without mutation.
    QList<WatchlistIdentity> publicPriority;
    QString displayCurrency = QStringLiteral("KRW");
    // Private session choice only. Never serialize this mapping.
    QHash<AssetKey, WatchlistIdentity> sessionSelections;
};
HoldingChartRow resolveHoldingMarket(const AssetKey&, const QStringList& custodyProviders,
    const HoldingMarketCatalog&, const HoldingMarketPreferences&);
QList<HoldingChartRow> resolveHoldingMarkets(const QList<AssetSnapshot>&,
    const QList<PortfolioConnection>&, const QList<AssetValueRow>&,
    const HoldingMarketCatalog&, const HoldingMarketPreferences&, qint64 nowMs);
}
