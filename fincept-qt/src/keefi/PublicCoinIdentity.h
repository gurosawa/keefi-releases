#pragma once
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include <optional>
namespace keefi {
// Public presentation join only. Never use this to merge holdings or value an account.
// An Upbit KRW market and a CoinGecko row are the same coin when the symbol
// matches and either the pinned CoinGecko id matches or, for unpinned symbols,
// Upbit's English name equals CoinGecko's name (NFKC, spacing and case folded).
bool samePublicCoin(const PublicMarketRow&, const PublicBoardItem&);
std::optional<PublicBoardItem> verifiedCoinMarketCap(const QString& base,
    const QList<PublicMarketRow>&, const QList<PublicBoardItem>&);
}
