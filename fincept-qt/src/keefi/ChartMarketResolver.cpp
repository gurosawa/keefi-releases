#include "keefi/ChartMarketResolver.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QObject>
namespace keefi {
namespace {
bool validSpot(const MarketInstrument& item) {
    return item.market == "spot" && item.status == "TRADING" && item.tradable &&
        binanceIdentifierValid(item.baseAsset) && binanceIdentifierValid(item.quoteAsset) &&
        binanceIdentifierValid(item.symbol,2) && item.symbol == item.baseAsset + item.quoteAsset;
}
ChartMarketResolution found(const WatchlistIdentity& identity,bool fallback=false) {
    return {identity,ChartMarketAvailability::Available,
        fallback ? QObject::tr("업비트 원화 시장 대신 확인된 바이낸스 현물 차트를 열었어요.") : QString{},fallback};
}
}
ChartMarketResolution resolveChartMarket(const WatchlistIdentity& requested,
    const QList<PublicMarketRow>& upbitRows,bool upbitAvailable,
    const QList<MarketInstrument>& binanceInstruments) {
    const auto unsupported=[] {return ChartMarketResolution{{},ChartMarketAvailability::Unsupported,
        QObject::tr("업비트 원화·바이낸스 현물에서 지원하는 종목을 찾지 못했어요."),false};};
    const auto checking=[] {return ChartMarketResolution{{},ChartMarketAvailability::Checking,
        QObject::tr("공개 종목 목록을 확인하고 있어요."),false};};
    if(requested.market!="coin" || !WatchlistWorkspaceList::validIdentity(requested))return unsupported();
    if(requested.source=="binance") {
        int count=0;MarketInstrument match;
        for(const auto& item:binanceInstruments)if(item.symbol==requested.code){++count;match=item;}
        if(count==1 && validSpot(match))return found(requested);
        return binanceInstruments.isEmpty()?checking():unsupported();
    }
    // This public catalogue contains Upbit KRW markets only. Do not silently
    // reinterpret saved BTC/USDT pairs or another exchange's instrument.
    if(requested.source!="upbit" || !requested.code.startsWith("KRW-"))return unsupported();
    // An unavailable catalogue cannot establish that an Upbit market is absent.
    // Explicit Binance selections above remain usable independently.
    if(!upbitAvailable)return checking();
    const auto base=requested.code.mid(4);
    int upbitMatches=0;
    if(upbitAvailable)for(const auto& row:upbitRows)
        if(row.market==requested.code && row.base==base)++upbitMatches;
    if(upbitMatches==1)return found(requested);
    if(upbitMatches>1)return unsupported();
    int matches=0;MarketInstrument match;
    for(const auto& item:binanceInstruments)
        if(item.baseAsset==base && item.quoteAsset=="USDT") {++matches;match=item;}
    if(matches==1 && validSpot(match))return found({"coin","binance",match.symbol},true);
    if(binanceInstruments.isEmpty())return checking();
    return unsupported();
}
ChartMarketResolution resolveChartMarket(const WatchlistIdentity& requested,
    const PublicMarketCatalog* upbit,const ChartInstrumentCatalog* binance) {
    return resolveChartMarket(requested,upbit?upbit->rows():QList<PublicMarketRow>{},
        upbit && upbit->marketListAvailable(),binance?binance->instruments("binance"):QList<MarketInstrument>{});
}
}
