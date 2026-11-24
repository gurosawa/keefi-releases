#include "keefi/PublicCoinIdentity.h"
#include <cmath>
namespace keefi {
namespace {
// Pinned CoinGecko ids: widely copied symbols, plus Upbit KRW coins whose
// English name differs from CoinGecko's (checked on public lists 2026-11-07:
// Upbit "Ada"/"Lumen"/"Ondo Finance"/"Cosmos"… for cardano/stellar/ondo-finance/cosmos…).
const QHash<QString,QString> CanonicalIds{{"BTC","bitcoin"},{"ETH","ethereum"},{"SOL","solana"},{"XRP","ripple"},{"ADA","cardano"},{"DOGE","dogecoin"},{"AVAX","avalanche-2"},{"DOT","polkadot"},{"LINK","chainlink"},{"BCH","bitcoin-cash"},{"LTC","litecoin"},{"XLM","stellar"},{"SUI","sui"},{"APT","aptos"},{"NEAR","near"},{"TRX","tron"},{"ZEC","zcash"},{"HYPE","hyperliquid"},
    {"ONDO","ondo-finance"},{"SKY","sky"},{"POL","polygon-ecosystem-token"},{"RENDER","render-token"},{"ATOM","cosmos"},{"CRV","curve-dao-token"},{"SUN","sun-token"},{"KMNO","kamino"},{"BAT","basic-attention-token"},{"THETA","theta-token"}};
QString normalizedName(const QString& value){return value.normalized(QString::NormalizationForm_KC).simplified().toCaseFolded();}
}
bool samePublicCoin(const PublicMarketRow& market,const PublicBoardItem& coin){
    const auto exchangeName=normalizedName(market.englishName),publicName=normalizedName(coin.name);
    if(exchangeName.isEmpty()||publicName.isEmpty()||market.market!="KRW-"+market.base||market.base!=coin.symbol)return false;
    // A pinned id is CoinGecko's own identity: it alone decides, so a copy that
    // reuses the symbol and name is rejected and exchange naming cannot split it.
    const auto pinned=CanonicalIds.constFind(market.base);
    if(pinned!=CanonicalIds.cend())return pinned.value()==coin.sourceId;
    // Every other coin needs the exchange's own English name; a symbol never joins alone.
    return !coin.ambiguousSymbol&&exchangeName==publicName;
}
std::optional<PublicBoardItem> verifiedCoinMarketCap(const QString& base,const QList<PublicMarketRow>& markets,const QList<PublicBoardItem>& coins){
    std::optional<PublicBoardItem> result;
    for(const auto& market:markets)if(market.base==base)for(const auto& coin:coins)if(samePublicCoin(market,coin)&&coin.currency=="KRW"&&std::isfinite(coin.marketCap)&&coin.marketCap>0){
        if(result)return {}; // Ambiguous duplicates never select an arbitrary source.
        result=coin;
    }
    return result;
}
}
