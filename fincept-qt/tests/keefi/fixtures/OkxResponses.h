#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QUrlQuery>

namespace keefi::test {
inline AccountRecord okxAccount(QString environment = "live", QString region = "global") {
    AccountRecord a; a.id = "fake-okx"; a.provider = "okx"; a.market = "spot";
    a.environment = environment; a.region = region; a.currency = "USDT"; a.alias = "가짜 OKX"; return a;
}
inline OrderInput okxInput() {
    OrderInput i; i.symbol = "BTC-USDT"; i.side = "buy"; i.type = "limit";
    i.price = "100"; i.quantity = "2"; i.clientId = "00112233445566778899aabbccddeeff"; return i;
}
inline HttpResult okxJson(QJsonArray data, QString code = "0", int status = 200) {
    HttpResult r; r.status = status; r.body = QJsonDocument(QJsonObject{{"code",code},{"msg",""},{"data",data}}).toJson(QJsonDocument::Compact); return r;
}
inline QJsonObject okxInstrument(QString symbol = "BTC-USDT") {
    const auto parts = symbol.split('-');
    return {{"instType","SPOT"},{"instId",symbol},{"baseCcy",parts.value(0)},{"quoteCcy",parts.value(1)},
        {"state","live"},{"groupId","12"},{"tickSz","0.1"},{"lotSz","0.001"},{"minSz","0.001"},
        {"maxLmtSz","10000"},{"maxMktSz","1000000"},{"maxLmtAmt","1000000"},{"maxMktAmt","1000000"},
        {"listTime","1600000000000"},{"contTdSwTime","1600000000000"},{"openType","fix_price"},
        {"ruleType","normal"},{"tradeQuoteCcyList",QJsonArray{parts.value(1)}},{"upcChg",QJsonArray{}}};
}
inline QJsonObject okxOrder(QString id = "100", QString state = "live", QString side = "buy",
                           QString type = "limit", QString size = "2", QString filled = "0",
                           QString price = "100", qint64 now = QDateTime::currentMSecsSinceEpoch()) {
    return {{"instType","SPOT"},{"instId","BTC-USDT"},{"ordId",id},
        {"clOrdId","00112233445566778899aabbccddeeff"},{"side",side},{"ordType",type},{"state",state},
        {"tdMode","cash"},{"px",type == "market" ? "" : price},{"sz",size},{"accFillSz",filled},
        {"avgPx",filled == "0" ? "" : price},{"tgtCcy",type == "market" && side == "buy" ? "quote_ccy" : "base_ccy"},
        {"tradeQuoteCcy","USDT"},{"uTime",QString::number(now)},{"cTime",QString::number(now - 1000)},
        {"algoId",""},{"algoClOrdId",""},{"attachAlgoOrds",QJsonArray{}},{"linkedAlgoOrd",QJsonObject{}},
        {"fee","0"},{"feeCcy","USDT"},{"rebate","0"},{"rebateCcy","USDT"},
        {"fillPx",""},{"fillSz","0"},{"fillTime",""},{"tradeId",""},{"category","normal"},
        {"source",""},{"cancelSource",""},{"reqId",""},{"amendResult",""}};
}
inline HttpResult okxResponse(const RequestSpec& request, qint64 now = QDateTime::currentMSecsSinceEpoch()) {
    const QUrlQuery query(request.url); const auto ts = QString::number(now);
    const auto symbol = query.queryItemValue("instId").isEmpty() ? QString("BTC-USDT") : query.queryItemValue("instId");
    switch (request.operation) {
    case Operation::Time: return okxJson({QJsonObject{{"ts",ts}}});
    case Operation::AccountConfig: return okxJson({QJsonObject{{"uid","123456789"},{"acctLv","1"},{"feeType","0"},
        {"spotRoleType","0"},{"autoLoan",false},{"enableSpotBorrow",false},{"perm","read_only,trade"},
        {"ip","PRIVATE-IP-MARKER"},{"label","PRIVATE-LABEL-MARKER"}}});
    case Operation::Markets: return okxJson({okxInstrument()});
    case Operation::Rules: return okxJson({okxInstrument(symbol)});
    case Operation::Account: return okxJson({QJsonObject{{"uTime",ts},{"details",QJsonArray{
        QJsonObject{{"ccy","USDT"},{"availBal","10000"},{"frozenBal","200"},{"cashBal","10200"},{"eq","10200"},{"liab",""}},
        QJsonObject{{"ccy","BTC"},{"availBal","10"},{"frozenBal","0"},{"cashBal","10"},{"eq","10"},{"liab",""}}}}}});
    case Operation::FundingBalance: return okxJson({QJsonObject{{"ccy","USDT"},{"bal","50"},{"availBal","50"},{"frozenBal","0"}}});
    case Operation::BuyingPower: return okxJson({QJsonObject{{"instId",symbol},{"availBuy","10000"},{"availSell","10"}}});
    case Operation::Commissions: return okxJson({QJsonObject{{"instType","SPOT"},{"feeGroup",QJsonArray{
        QJsonObject{{"groupId","12"},{"maker","-0.001"},{"taker","-0.002"}}}}}});
    case Operation::Quote: return okxJson({QJsonObject{{"instId",symbol},{"last","100"},{"bidPx","99.9"},{"askPx","100.1"},{"ts",ts}}});
    case Operation::Depth: return okxJson({QJsonObject{{"ts",ts},{"bids",QJsonArray{QJsonValue(QJsonArray{"99.9","100","0","1"})}},
        {"asks",QJsonArray{QJsonValue(QJsonArray{"100.1","100","0","1"})}}}});
    case Operation::PriceLimits: return okxJson({QJsonObject{{"instId",symbol},{"buyLmt","110"},{"sellLmt","90"},{"enabled",true},{"ts",ts}}});
    case Operation::FxUsd: case Operation::FxUsdt:
        if (request.url.path().endsWith("/time")) return okxJson({QJsonObject{{"ts",ts}}});
        return okxJson({QJsonObject{{"instId",symbol},{"idxPx","0.99"},{"ts",ts}}});
    case Operation::OpenOrders: return okxJson({okxOrder("100","live","buy","limit","2","0","100",now)});
    case Operation::History: return okxJson({});
    case Operation::QueryOrder: return okxJson({okxOrder(query.queryItemValue("ordId").isEmpty() ? "100" : query.queryItemValue("ordId"),"live","buy","limit","2","0","100",now)});
    case Operation::PlaceOrder: case Operation::AmendOrder: case Operation::CancelOrder: {
        auto input = QJsonDocument::fromJson(request.body).object();
        return okxJson({QJsonObject{{"ordId",input.value("ordId").toString("200")},{"clOrdId",input.value("clOrdId").toString()},
            {"reqId",input.value("reqId").toString()},{"sCode","0"},{"sMsg",""}}});
    }
    default: return okxJson({},"51000",400);
    }
}
} // namespace keefi::test
