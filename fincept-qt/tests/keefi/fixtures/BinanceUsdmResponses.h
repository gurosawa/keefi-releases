#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QUrlQuery>
namespace keefi::test {
inline AccountRecord usdmAccount(QString env="live") {AccountRecord a;a.id="fake-usdm";a.provider="binance";a.environment=env;a.market="usdm_futures";a.currency="USDT";a.alias="가짜 선물";return a;}
inline OrderInput usdmInput(){OrderInput i;i.symbol="BTCUSDT";i.side="buy";i.type="limit";i.price="100";i.quantity="1";i.clientId="fake-futures-client";
    FuturesOrderTerms f;f.positionSide="BOTH";f.timeInForce="GTC";f.exposure=ExposureIntent::Open;i.futures=f;
    i.endTimeMs=QDateTime::currentMSecsSinceEpoch();i.startTimeMs=i.endTimeMs-6LL*86400000;return i;}
inline HttpResult usdmJson(const QJsonObject& v,int status=200){HttpResult r;r.status=status;r.body=QJsonDocument(v).toJson(QJsonDocument::Compact);return r;}
inline HttpResult usdmJson(const QJsonArray& v,int status=200){HttpResult r;r.status=status;r.body=QJsonDocument(v).toJson(QJsonDocument::Compact);return r;}
inline QJsonObject usdmInstrument(QString symbol="BTCUSDT") {return {{"symbol",symbol},{"pair",symbol},{"status","TRADING"},{"contractType","PERPETUAL"},
    {"baseAsset","BTC"},{"quoteAsset","USDT"},{"marginAsset","USDT"},{"underlyingType","COIN"},{"onboardDate","1600000000000"},{"deliveryDate","4133404800000"},
    {"orderTypes",QJsonArray{"LIMIT","MARKET","STOP","TAKE_PROFIT"}},{"timeInForce",QJsonArray{"GTC","IOC","FOK","GTX","GTD"}},
    {"filters",QJsonArray{QJsonObject{{"filterType","PRICE_FILTER"},{"minPrice","0.1"},{"maxPrice","1000000"},{"tickSize","0.1"}},
        QJsonObject{{"filterType","LOT_SIZE"},{"minQty","0.001"},{"maxQty","1000"},{"stepSize","0.001"}},
        QJsonObject{{"filterType","MARKET_LOT_SIZE"},{"minQty","0.001"},{"maxQty","1000"},{"stepSize","0.001"}},
        QJsonObject{{"filterType","MIN_NOTIONAL"},{"notional","5"}},
        QJsonObject{{"filterType","PERCENT_PRICE"},{"multiplierUp","1.1"},{"multiplierDown","0.9"},{"multiplierDecimal","4"}},
        QJsonObject{{"filterType","MAX_NUM_ORDERS"},{"limit","200"}}}}};}
inline QJsonObject usdmOrder(QString id="100",QString state="NEW",OrderInput i=usdmInput(),QString executed="0",qint64 now=QDateTime::currentMSecsSinceEpoch()) {
    const auto f=i.futures.value_or(FuturesOrderTerms{});
    return {{"orderId",id},{"clientOrderId",i.clientId},{"symbol",i.symbol},{"side",i.side.toUpper()},{"type",i.type.toUpper()},
        {"origType",i.type.toUpper()},{"status",state},{"price",i.type=="market"?QString("0"):i.price},{"origQty",i.quantity},
        {"executedQty",executed},{"cumQuote","0"},{"timeInForce",f.timeInForce.isEmpty()?QString("GTC"):f.timeInForce},
        {"positionSide",f.positionSide},{"reduceOnly",f.reduceOnly},{"closePosition",false},{"workingType","CONTRACT_PRICE"},
        {"stopPrice","0"},{"updateTime",QString::number(now)},{"time",QString::number(now-1000)},
        {"priceMatch","NONE"},{"goodTillDate","0"}};
}
inline QJsonObject usdmAlgo(QString id="100",QString status="NEW",qint64 now=QDateTime::currentMSecsSinceEpoch()) {
    return {{"algoId",id},{"clientAlgoId","fake-conditional-client"},{"algoType","CONDITIONAL"},{"orderType","STOP_MARKET"},
        {"symbol","BTCUSDT"},{"side","SELL"},{"positionSide","BOTH"},{"timeInForce","GTC"},{"quantity","0"},
        {"algoStatus",status},{"actualOrderId",""},{"price","0"},{"triggerPrice","90"},{"workingType","MARK_PRICE"},
        {"closePosition",true},{"reduceOnly",false},{"createTime",QString::number(now-1000)},{"updateTime",QString::number(now)}};
}
inline OrderInput usdmConditionalInput() {
    auto i=usdmInput();i.clientId.clear();i.type="stop";i.price="101";
    i.futures->family=OrderFamily::Conditional;i.futures->triggerPrice="102";i.futures->workingType="MARK_PRICE";i.futures->priceProtect=false;return i;
}
inline QJsonObject usdmConditionalOrder(const OrderInput& i,QString id="300",QString status="NEW",QString actual={}) {
    auto j=usdmAlgo(id,status);const auto& t=*i.futures;
    j["clientAlgoId"]=i.clientAlgoId;j["orderType"]=i.type.toUpper();j["symbol"]=i.symbol;j["side"]=i.side.toUpper();
    j["positionSide"]=t.positionSide;j["quantity"]=i.quantity;j["price"]=i.price;j["triggerPrice"]=t.triggerPrice;j["workingType"]=t.workingType;
    j["timeInForce"]=t.timeInForce;j["closePosition"]=t.closePosition;j["reduceOnly"]=t.reduceOnly;j["priceProtect"]=t.priceProtect.value_or(false);j["actualOrderId"]=actual;return j;
}
inline QJsonObject usdmPosition(QString amount="2",QString side="BOTH",qint64 now=QDateTime::currentMSecsSinceEpoch()) {
    return {{"symbol","BTCUSDT"},{"positionSide",side},{"positionAmt",amount},{"entryPrice","100"},{"breakEvenPrice","100.01"},
        {"markPrice","100"},{"liquidationPrice","20"},{"unRealizedProfit","-1"},{"notional",amount.startsWith('-')?"-200":"200"},
        {"marginAsset","USDT"},{"isolatedWallet","0"},{"initialMargin","20"},{"maintMargin","1"},{"positionInitialMargin","20"},
        {"openOrderInitialMargin","0"},{"updateTime",QString::number(now)}};
}
inline HttpResult usdmResponse(const RequestSpec& r,qint64 now=QDateTime::currentMSecsSinceEpoch()) {
    QUrlQuery q(r.method=="GET"||r.method=="DELETE"?QUrlQuery(r.url):QUrlQuery(QString::fromUtf8(r.body)));
    auto symbol=q.queryItemValue("symbol");if(symbol.isEmpty())symbol="BTCUSDT";
    switch(r.operation){
    case Operation::Time:return usdmJson(QJsonObject{{"serverTime",QString::number(now)}});
    case Operation::Markets:case Operation::Rules:return usdmJson(QJsonObject{{"serverTime",QString::number(now)},{"symbols",QJsonArray{usdmInstrument()}},{"exchangeFilters",QJsonArray{}}});
    case Operation::AccountConfig:return usdmJson(QJsonObject{{"canTrade",true},{"dualSidePosition",false},{"multiAssetsMargin",false},{"feeTier","0"},{"updateTime","0"}});
    case Operation::Account:return usdmJson(QJsonObject{{"totalWalletBalance","10000"},{"totalMarginBalance","9999"},{"totalInitialMargin","20"},
        {"totalMaintMargin","1"},{"totalUnrealizedProfit","-1"},{"availableBalance","9979"},
        {"assets",QJsonArray{QJsonObject{{"asset","USDT"},{"walletBalance","10000"},{"marginBalance","9999"},{"availableBalance","9979"},
            {"initialMargin","20"},{"maintMargin","1"},{"unrealizedProfit","-1"},{"openOrderInitialMargin","0"},{"positionInitialMargin","20"},{"updateTime",QString::number(now)}}}},
        {"positions",QJsonArray{}}});
    case Operation::Positions:return usdmJson(QJsonArray{usdmPosition("2","BOTH",now)});
    case Operation::SymbolConfig:return usdmJson(QJsonArray{QJsonObject{{"symbol",symbol},{"marginType","CROSSED"},{"isAutoAddMargin",false},{"leverage","10"},{"maxNotionalValue","100000"}}});
    case Operation::LeverageBrackets:return usdmJson(QJsonArray{QJsonObject{{"symbol",symbol},{"brackets",QJsonArray{
        QJsonObject{{"bracket","1"},{"initialLeverage","20"},{"notionalFloor","0"},{"notionalCap","100000"},{"maintMarginRatio","0.005"},{"cum","0"}}}}}});
    case Operation::Commissions:return usdmJson(QJsonObject{{"symbol",symbol},{"makerCommissionRate","0.0002"},{"takerCommissionRate","0.0005"}});
    case Operation::TradingStatus: {
        const QJsonObject indicator{{"indicator","UFR"},{"value","0"},{"triggerValue","0.99"},{"isLocked",false},{"plannedRecoverTime","0"}};
        return usdmJson(QJsonObject{{"indicators",QJsonObject{{symbol,QJsonArray{indicator}}}},{"updateTime",QString::number(now)}});
    }
    case Operation::ApiRestrictions:return usdmJson(QJsonObject{{"enableReading",true},{"enableFutures",true},{"enablePortfolioMarginTrading",false},{"ipRestrict",true}});
    case Operation::Quote:return usdmJson(QJsonObject{{"symbol",symbol},{"price","100"},{"time",QString::number(now)}});
    case Operation::MarkPrice:return usdmJson(QJsonObject{{"symbol",symbol},{"markPrice","100"},{"indexPrice","100"},{"lastFundingRate","-0.0001"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}});
    case Operation::Depth:return usdmJson(QJsonObject{{"E",QString::number(now)},{"T",QString::number(now)},{"lastUpdateId","1"},
        {"bids",QJsonArray{QJsonValue(QJsonArray{"99.9","100"})}},{"asks",QJsonArray{QJsonValue(QJsonArray{"100.1","100"})}}});
    case Operation::OpenOrders:return usdmJson(QJsonArray{usdmOrder("100","NEW",usdmInput(),"0",now)});
    case Operation::AlgoOpenOrders:return usdmJson(QJsonArray{usdmAlgo("100","NEW",now)});
    case Operation::History:case Operation::AlgoHistory:case Operation::Income:case Operation::AmendHistory:return usdmJson(QJsonArray{});
    case Operation::TestOrder:return usdmJson(QJsonObject{});
    case Operation::QueryOrder:case Operation::PlaceOrder:case Operation::AmendOrder:case Operation::CancelOrder:{auto i=usdmInput();i.symbol=symbol;
        if(r.operation==Operation::PlaceOrder&&r.url.path()=="/fapi/v1/algoOrder"){
            i=usdmConditionalInput();i.clientAlgoId=q.queryItemValue("clientAlgoId");i.symbol=symbol;i.side=q.queryItemValue("side").toLower();i.type=q.queryItemValue("type").toLower();
            i.price=q.queryItemValue("price");i.quantity=q.queryItemValue("quantity");i.futures->triggerPrice=q.queryItemValue("triggerPrice");
            i.futures->workingType=q.queryItemValue("workingType");i.futures->positionSide=q.queryItemValue("positionSide");i.futures->reduceOnly=q.queryItemValue("reduceOnly")=="true";
            return usdmJson(usdmConditionalOrder(i));}
        if(q.hasQueryItem("side"))i.side=q.queryItemValue("side").toLower();if(q.hasQueryItem("type"))i.type=q.queryItemValue("type").toLower();
        if(q.hasQueryItem("price"))i.price=q.queryItemValue("price");if(q.hasQueryItem("quantity"))i.quantity=q.queryItemValue("quantity");
        if(q.hasQueryItem("newClientOrderId"))i.clientId=q.queryItemValue("newClientOrderId");if(q.hasQueryItem("origClientOrderId"))i.clientId=q.queryItemValue("origClientOrderId");
        if(q.hasQueryItem("positionSide"))i.futures->positionSide=q.queryItemValue("positionSide");i.futures->reduceOnly=q.queryItemValue("reduceOnly")=="true";
        auto o=usdmOrder(q.hasQueryItem("orderId")?q.queryItemValue("orderId"):"200",r.operation==Operation::CancelOrder?"CANCELED":"NEW",i,"0",now);
        if(q.hasQueryItem("modifyId"))o["modifyId"]=q.queryItemValue("modifyId");return usdmJson(o);}
    case Operation::AlgoQueryOrder:return usdmJson(usdmAlgo(q.queryItemValue("algoId"),"NEW",now));
    case Operation::AlgoCancelOrder:return usdmJson(QJsonObject{{"algoId",q.queryItemValue("algoId")},{"clientAlgoId","fake-conditional-client"},{"code","200"},{"msg","success"}});
    default:return usdmJson(QJsonObject{{"code","-1100"},{"msg","FAKE_ONLY"}},400);
    }
}
} // namespace keefi::test
