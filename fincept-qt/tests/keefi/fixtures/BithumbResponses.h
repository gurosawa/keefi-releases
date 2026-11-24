#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QUrlQuery>
namespace keefi::test {
inline AccountRecord bithumbAccount() {
    AccountRecord a;a.id="fake-bithumb";a.provider="bithumb";a.environment="live";
    a.market="spot";a.currency="KRW";a.alias="가짜 빗썸";return a;
}
inline OrderInput bithumbInput() {
    OrderInput i;i.symbol="KRW-BTC";i.side="buy";i.type="limit";i.price="100000";i.quantity="0.1";
    i.clientId="fake-bithumb-client-1";i.endTimeMs=QDateTime::currentMSecsSinceEpoch();
    i.startTimeMs=i.endTimeMs-7LL*86400000;return i;
}
inline HttpResult bithumbJson(const QJsonObject& o,int status=200) {
    HttpResult r;r.status=status;r.body=QJsonDocument(o).toJson(QJsonDocument::Compact);return r;
}
inline HttpResult bithumbJson(const QJsonArray& a,int status=200) {
    HttpResult r;r.status=status;r.body=QJsonDocument(a).toJson(QJsonDocument::Compact);return r;
}
inline QJsonObject bithumbOrder(QString id="fake-bithumb-order",QString state="wait",bool v2=false,
                               OrderInput input=bithumbInput()) {
    const bool completed=state=="done";
    QJsonObject o{{v2?"order_id":"uuid",id},{v2?"order_type":"ord_type",input.type=="limit"?"limit":input.side=="buy"?"price":"market"},
        {"client_order_id",input.clientId},{"market",input.symbol},{"side",input.side=="buy"?"bid":"ask"},
        {"state",state},{"created_at","2026-10-17T12:00:00+09:00"},{"price",input.price},{"volume",input.quantity},
        {"remaining_volume",completed?QString("0"):input.quantity},{"executed_volume",completed?input.quantity:QString("0")},
        {"reserved_fee","5"},{"remaining_fee","5"},{"paid_fee","0"},{"locked","10005"},
        {"executed_funds",completed?QString("10000"):QString("0")},{"trades_count",0}};
    if(!v2)o["trades"]=QJsonArray{};
    // Market null variants are explicitly constructed by negative/unknown tests,
    // never presented as documented successful market-order response examples.
    return o;
}
inline HttpResult bithumbPage(QJsonArray rows,bool hasNext=false,QJsonValue next=QJsonValue::Null) {
    return bithumbJson(QJsonObject{{"data",rows},{"has_next",hasNext},{"next_key",next}});
}
inline HttpResult bithumbResponse(const RequestSpec& req,qint64 now=QDateTime::currentMSecsSinceEpoch()) {
    QUrlQuery q(req.url);const auto symbol=q.queryItemValue("market").isEmpty()?QString("KRW-BTC"):q.queryItemValue("market");
    switch(req.operation){
    case Operation::Markets:return bithumbJson(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"korean_name","가짜 비트코인"},{"english_name","Fake Bitcoin"},{"market_warning","NONE"}}});
    case Operation::Account:return bithumbJson(QJsonArray{
        QJsonObject{{"currency","KRW"},{"balance","1000000"},{"locked","10005"},{"avg_buy_price","0"},{"unit_currency","KRW"}},
        QJsonObject{{"currency","BTC"},{"balance","10"},{"locked","0"},{"avg_buy_price","90000"},{"unit_currency","KRW"}}});
    case Operation::Rules:return bithumbJson(QJsonObject{{"bid_fee","0.0005"},{"ask_fee","0.0005"},{"maker_bid_fee","0.0005"},{"maker_ask_fee","0.0005"},
        {"market",QJsonObject{{"id",symbol},{"state","active"},{"order_sides",QJsonArray{"bid","ask"}},
            {"order_types",QJsonArray{"limit","price","market"}},{"bid_types",QJsonArray{"limit","price"}},{"ask_types",QJsonArray{"limit","market"}},
            {"bid",QJsonObject{{"currency","KRW"},{"price_unit",QJsonValue::Null},{"min_total","5000"}}},
            {"ask",QJsonObject{{"currency","BTC"},{"price_unit",QJsonValue::Null},{"min_total","5000"}}},{"max_total","1000000000"}}},
        {"bid_account",QJsonObject{{"currency","KRW"},{"balance","1000000"},{"locked","10005"}}},
        {"ask_account",QJsonObject{{"currency","BTC"},{"balance","10"},{"locked","0"}}}});
    case Operation::Quote:return bithumbJson(QJsonArray{QJsonObject{{"market",q.queryItemValue("markets")},{"trade_price",100000},{"timestamp",now},{"trade_timestamp",now-1000}}});
    case Operation::Depth:return bithumbJson(QJsonArray{QJsonObject{{"market",q.queryItemValue("markets")},{"timestamp",now},{"orderbook_units",QJsonArray{
        QJsonObject{{"bid_price",100000},{"ask_price",100100},{"bid_size",10},{"ask_size",10}},
        QJsonObject{{"bid_price",99900},{"ask_price",100200},{"bid_size",10},{"ask_size",10}}}}}});
    case Operation::OpenOrders:return q.queryItemValue("state")=="watch"?bithumbPage({}):bithumbPage({bithumbOrder("fake-bithumb-order","wait",true)});
    case Operation::History:return bithumbPage({});
    case Operation::QueryOrder:{auto i=bithumbInput();if(q.hasQueryItem("client_order_id"))i.clientId=q.queryItemValue("client_order_id");
        return bithumbJson(bithumbOrder(q.hasQueryItem("uuid")?q.queryItemValue("uuid"):"fake-bithumb-new","wait",false,i));}
    case Operation::PlaceOrder:{auto b=QJsonDocument::fromJson(req.body).object();return bithumbJson(QJsonObject{{"order_id","fake-bithumb-new"},
        {"market",b.value("market")},{"side",b.value("side")},{"order_type",b.value("order_type")},{"client_order_id",b.value("client_order_id")},
        {"created_at","2026-10-17T12:00:00+09:00"}},201);}
    default:return bithumbJson(QJsonObject{{"error",QJsonObject{{"name","order_not_ready"},{"message","FAKE_ONLY"}}}},422);
    }
}
} // namespace keefi::test
