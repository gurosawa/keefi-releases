#include "keefi/AssetIdentity.h"

namespace keefi {
namespace {
bool registeredSolLedger(const AssetSnapshot& snapshot,const AssetComponent& component){
    // Registry v1: these exchange SOL deposit/withdrawal ledgers represent
    // Solana mainnet native SOL. Display symbols never establish equivalence.
    if(component.asset!="SOL"||component.kind!=AssetComponentKind::Holding||
       !QStringList{"upbit","binance","bithumb","okx"}.contains(snapshot.provider))return false;
    if(snapshot.market=="spot"||snapshot.market=="earn")return true;
    // OKX /api/v5/asset/balances: funding 'bal', not trading equity or exposure.
    return snapshot.provider=="okx"&&snapshot.market=="okx_unified"&&component.valuationGroup=="funding";
}
}
AssetKey assetIdentity(const AssetSnapshot& snapshot,const AssetComponent& component){
    const auto& asset=component.asset;
    if(asset=="KRW")return QStringLiteral("cash:KRW");
    if(snapshot.market=="domestic_equity")return QStringLiteral("stock:KR:")+asset;
    if(snapshot.market=="solana"){
        if(snapshot.provider=="phantom"&&component.id=="solana:native"&&asset=="SOL"&&
           component.kind==AssetComponentKind::Holding)return QStringLiteral("solana:native");
        if(asset.startsWith("solana:")&&asset!="solana:native")return asset;
        return QStringLiteral("solana:unresolved:")+component.id;
    }
    if(registeredSolLedger(snapshot,component))return QStringLiteral("solana:native");
    // Preserve legacy exchange-code groups pending a broader reviewed registry.
    return QStringLiteral("exchange:")+asset;
}
AssetKey publicAssetIdentity(const QString& provider,const QString& asset){
    AssetSnapshot snapshot;snapshot.provider=provider;snapshot.market="spot";
    AssetComponent component;component.asset=asset;component.id=asset;
    return assetIdentity(snapshot,component);
}
}
