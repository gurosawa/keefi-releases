#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>

// Synthetic values only. Never load account files, environment credentials or URLs.
namespace upbit_fixture {
inline keefi::AccountRecord account() {
    keefi::AccountRecord a;
    a.id = "fake-upbit-account"; a.provider = "upbit"; a.environment = "live";
    a.market = "spot"; a.currency = "KRW"; a.alias = "가짜 Upbit";
    return a;
}
inline keefi::OrderInput input() {
    keefi::OrderInput i;
    i.symbol = "KRW-BTC"; i.side = "buy"; i.type = "limit";
    i.price = "100000"; i.quantity = "0.1"; i.clientId = "fake-client-1";
    i.endTimeMs = QDateTime::currentMSecsSinceEpoch();
    i.startTimeMs = i.endTimeMs - 7LL * 24 * 60 * 60 * 1000;
    return i;
}
inline QJsonObject order(QString state = "wait", QString id = "fake-order-1", QString client = "fake-client-1") {
    return {{"uuid", id}, {"identifier", client}, {"market", "KRW-BTC"}, {"side", "bid"},
        {"ord_type", "limit"}, {"state", state}, {"price", "100000"}, {"volume", "0.1"},
        {"remaining_volume", state == "done" ? "0" : "0.1"}, {"executed_volume", state == "done" ? "0.1" : "0"},
        {"created_at", "2026-10-17T12:00:00+09:00"}, {"reserved_fee", "5"}, {"remaining_fee", "5"},
        {"paid_fee", "0"}, {"locked", "10005"}, {"trades_count", 0}};
}
inline keefi::HttpResult json(const QJsonObject& value, int status = 200) {
    keefi::HttpResult r; r.status = status; r.body = QJsonDocument(value).toJson(QJsonDocument::Compact); return r;
}
inline keefi::HttpResult json(const QJsonArray& value, int status = 200) {
    keefi::HttpResult r; r.status = status; r.body = QJsonDocument(value).toJson(QJsonDocument::Compact); return r;
}
inline keefi::HttpResult response(keefi::Operation op, const keefi::OrderInput& input = upbit_fixture::input()) {
    using keefi::Operation;
    switch (op) {
    case Operation::Markets:
        return json(QJsonArray{QJsonObject{{"market", "KRW-BTC"}, {"korean_name", "가짜 비트코인"},
            {"english_name", "Fake Bitcoin"}, {"market_event", QJsonObject{{"warning", false}}}}});
    case Operation::Reference:
        return json(QJsonArray{QJsonObject{{"market", "KRW-BTC"}, {"quote_currency", "KRW"},
            {"tick_size", "100"}, {"supported_levels", QJsonArray{"0"}}}});
    case Operation::Rules:
        return json(QJsonObject{{"bid_fee", "0.0005"}, {"ask_fee", "0.0005"}, {"market", QJsonObject{
            {"id", "KRW-BTC"}, {"state", "active"}, {"order_sides", QJsonArray{"bid", "ask"}},
            {"bid_types", QJsonArray{"limit", "price"}}, {"ask_types", QJsonArray{"limit", "market"}},
            {"bid", QJsonObject{{"currency", "KRW"}, {"min_total", "5000"}}},
            {"ask", QJsonObject{{"currency", "BTC"}, {"min_total", "5000"}}}, {"max_total", "1000000000"}}}});
    case Operation::Quote:
        return json(QJsonArray{QJsonObject{{"market", "KRW-BTC"}, {"timestamp", QDateTime::currentMSecsSinceEpoch()},
            {"orderbook_units", QJsonArray{
                QJsonObject{{"bid_price", 100000}, {"ask_price", 100000}, {"bid_size", 10}, {"ask_size", 10}},
                QJsonObject{{"bid_price", 99900}, {"ask_price", 100100}, {"bid_size", 10}, {"ask_size", 10}}}}}});
    case Operation::Account:
        return json(QJsonArray{QJsonObject{{"currency", "KRW"}, {"balance", "1000000"}, {"locked", "10005"},
                {"avg_buy_price", "0"}, {"avg_buy_price_modified", false}, {"unit_currency", "KRW"}},
            QJsonObject{{"currency", "BTC"}, {"balance", "10"}, {"locked", "0"},
                {"avg_buy_price", "90000"}, {"avg_buy_price_modified", false}, {"unit_currency", "KRW"}}});
    case Operation::OpenOrders: return json(QJsonArray{order()});
    case Operation::History: return json(QJsonArray{order("done", "fake-historical-order", "fake-history-client")});
    case Operation::QueryOrder: {
        auto o = order("wait", input.orderId.isEmpty() ? "fake-order-1" : input.orderId,
            input.clientId.isEmpty() ? "fake-client-1" : input.clientId); return json(o);
    }
    case Operation::PlaceOrder:
    case Operation::TestOrder: {
        auto o = order("wait", "fake-new-order", input.clientId);
        o["side"] = input.side == "buy" ? "bid" : "ask";
        o["ord_type"] = input.type == "limit" ? "limit" : input.side == "buy" ? "price" : "market";
        o["price"] = input.type == "limit" ? QJsonValue(input.price) : input.side == "buy" ? QJsonValue(input.quoteAmount) : QJsonValue();
        o["volume"] = input.quantity.isEmpty() ? QJsonValue() : QJsonValue(input.quantity);
        o["remaining_volume"] = input.quantity.isEmpty() ? "0" : input.quantity;
        return json(o, 201);
    }
    case Operation::CancelOrder: return json(order("cancel", input.orderId.isEmpty() ? "fake-order-1" : input.orderId,
        input.clientId.isEmpty() ? "fake-client-1" : input.clientId));
    case Operation::AmendOrder: {
        auto o = order("wait", input.orderId.isEmpty() ? "fake-order-1" : input.orderId,
            input.clientId.isEmpty() ? "fake-client-1" : input.clientId);
        o["new_order_uuid"] = "fake-replacement-order"; o["new_order_identifier"] = input.newClientId;
        return json(o);
    }
    default: return json(QJsonObject{{"error", QJsonObject{{"name", "validation_error"}, {"message", "FAKE_ONLY"}}}}, 400);
    }
}
} // namespace upbit_fixture
