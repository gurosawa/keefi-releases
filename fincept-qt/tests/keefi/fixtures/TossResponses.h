#pragma once
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>

// Entirely synthetic examples. No keys, accounts, market or order API calls.
namespace keefi::test {
inline HttpResult tossEnvelope(const QJsonValue& value) {
    HttpResult r; r.status = 200; r.body = QJsonDocument(QJsonObject{{"result", value}}).toJson(QJsonDocument::Compact); return r;
}
inline QJsonObject tossOrder(const QString& id = "order-original-001", const QString& status = "PENDING",
                            const QString& symbol = "005930", const QString& quantity = "10", const QString& filled = "0") {
    const QString now = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Seoul")).toString(Qt::ISODateWithMs);
    const bool hasFill = decimal::positive(filled);
    return {{"orderId", id}, {"symbol", symbol}, {"side", "BUY"}, {"orderType", "LIMIT"}, {"timeInForce", "DAY"},
        {"status", status}, {"price", "70000"}, {"quantity", quantity}, {"orderAmount", QJsonValue::Null},
        {"currency", symbol == "AAPL" ? "USD" : "KRW"}, {"orderedAt", now},
        {"canceledAt", status == "CANCELED" ? QJsonValue(now) : QJsonValue(QJsonValue::Null)},
        {"execution", QJsonObject{{"filledQuantity", filled},
            {"averageFilledPrice", hasFill ? QJsonValue("70000") : QJsonValue(QJsonValue::Null)},
            {"filledAmount", hasFill ? QJsonValue(decimal::multiply(filled, "70000")) : QJsonValue(QJsonValue::Null)},
            {"commission", hasFill ? QJsonValue("0") : QJsonValue(QJsonValue::Null)},
            {"tax", hasFill ? QJsonValue("0") : QJsonValue(QJsonValue::Null)},
            {"filledAt", hasFill ? QJsonValue(now) : QJsonValue(QJsonValue::Null)}, {"settlementDate", QJsonValue::Null}}}};
}
inline HttpResult tossResponse(Operation op, const OrderInput& input = {}) {
    const QString s = input.symbol.isEmpty() ? QStringLiteral("005930") : input.symbol;
    const auto now = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Seoul"));
    const QString time = now.toString(Qt::ISODateWithMs);
    switch (op) {
    case Operation::Authenticate: {
        HttpResult r; r.status = 200;
        r.body = R"({"access_token":"fakeheader.fakepayload.fakesignature","token_type":"Bearer","expires_in":86400})"; return r;
    }
    case Operation::Accounts:
        return tossEnvelope(QJsonArray{QJsonObject{{"accountNo", "00000001234"}, {"accountSeq", 7391}, {"accountType", "BROKERAGE"}}});
    case Operation::Markets:
        if (input.cursor == "KR_ETC") return tossEnvelope(QJsonArray{});
        return tossEnvelope(QJsonArray{QJsonObject{{"symbol", input.cursor == "KOSDAQ" ? "035720" : "005930"},
            {"name", input.cursor == "KOSDAQ" ? "가상 KOSDAQ 종목" : "가상 국내 종목"},
            {"securityType", "STOCK"}, {"isCommonShare", true}, {"isinCode", "KR0000000000"}}});
    case Operation::Rules:
        return tossEnvelope(QJsonArray{QJsonObject{{"symbol", s}, {"name", "가상 국내 종목"}, {"englishName", "Synthetic Stock"},
            {"market", "KOSPI"}, {"securityType", "STOCK"}, {"isCommonShare", true}, {"isinCode", "KR0000000000"},
            {"status", "ACTIVE"}, {"currency", "KRW"}, {"sharesOutstanding", "100000"},
            {"koreanMarketDetail", QJsonObject{{"liquidationTrading", false}, {"nxtSupported", true},
                {"krxTradingSuspended", false}, {"nxtTradingSuspended", false}}}}});
    case Operation::Quote: return tossEnvelope(QJsonArray{QJsonObject{{"symbol", s}, {"lastPrice", "70000"}, {"currency", "KRW"}, {"timestamp", time}}});
    case Operation::Depth:
        return tossEnvelope(QJsonObject{{"currency", "KRW"}, {"timestamp", time},
            {"asks", QJsonArray{QJsonObject{{"price", "70100"}, {"volume", "1000"}}}},
            {"bids", QJsonArray{QJsonObject{{"price", "69900"}, {"volume", "1000"}}}}});
    case Operation::PriceLimits:
        return tossEnvelope(QJsonObject{{"currency", "KRW"}, {"timestamp", time}, {"upperLimitPrice", "91000"}, {"lowerLimitPrice", "49000"}});
    case Operation::TradingSchedule: {
        const QJsonObject regular{{"startTime", now.addSecs(-3600).toString(Qt::ISODateWithMs)},
            {"singlePriceAuctionStartTime", now.addSecs(3600).toString(Qt::ISODateWithMs)}, {"endTime", now.addSecs(4200).toString(Qt::ISODateWithMs)}};
        const QJsonObject day{{"date", now.date().toString(Qt::ISODate)},
            {"integrated", QJsonObject{{"preMarket", QJsonValue::Null}, {"regularMarket", regular}, {"afterMarket", QJsonValue::Null}}}};
        return tossEnvelope(QJsonObject{{"today", day}, {"previousBusinessDay", QJsonObject{{"date", now.date().addDays(-1).toString(Qt::ISODate)}, {"integrated", QJsonValue::Null}}},
            {"nextBusinessDay", QJsonObject{{"date", now.date().addDays(1).toString(Qt::ISODate)}, {"integrated", QJsonValue::Null}}}});
    }
    case Operation::Holdings:
        return tossEnvelope(QJsonObject{{"totalPurchaseAmount", QJsonObject{{"krw", "6500000"}, {"usd", QJsonValue::Null}}},
            {"marketValue", QJsonObject{}}, {"profitLoss", QJsonObject{}}, {"dailyProfitLoss", QJsonObject{}},
            {"items", QJsonArray{QJsonObject{{"symbol", s}, {"name", "가상 국내 종목"}, {"marketCountry", "KR"}, {"currency", "KRW"},
                {"quantity", "100"}, {"lastPrice", "70000"}, {"averagePurchasePrice", "65000"},
                {"marketValue", QJsonObject{{"purchaseAmount", "6500000"}, {"amount", "7000000"}, {"amountAfterCost", "6990000"}}},
                {"profitLoss", QJsonObject{{"amount", "500000"}, {"amountAfterCost", "490000"}, {"rate", "0.076923"}, {"rateAfterCost", "0.075"}}},
                {"dailyProfitLoss", QJsonObject{{"amount", "-1000"}, {"rate", "-0.000142"}}},
                {"cost", QJsonObject{{"commission", "1000"}, {"tax", "9000"}}}}}}});
    case Operation::BuyingPower: return tossEnvelope(QJsonObject{{"currency", "KRW"}, {"cashBuyingPower", "10000000"}});
    case Operation::SellableQuantity: return tossEnvelope(QJsonObject{{"sellableQuantity", "100"}});
    case Operation::Commissions: return tossEnvelope(QJsonArray{QJsonObject{{"marketCountry", "KR"}, {"commissionRate", "0.00015"}, {"startDate", QJsonValue::Null}, {"endDate", QJsonValue::Null}}});
    case Operation::OpenOrders: return tossEnvelope(QJsonObject{{"orders", QJsonArray{}}, {"nextCursor", QJsonValue::Null}, {"hasNext", false}});
    case Operation::History: return tossEnvelope(QJsonObject{{"orders", QJsonArray{tossOrder("history-order-001", "FILLED", s, "10", "10")}}, {"nextCursor", QJsonValue::Null}, {"hasNext", false}});
    case Operation::QueryOrder: return tossEnvelope(tossOrder(input.orderId, "PENDING", s, input.quantity.isEmpty() ? "10" : input.quantity));
    case Operation::PlaceOrder: return tossEnvelope(QJsonObject{{"orderId", "new-order-001"}, {"clientOrderId", input.clientId}});
    case Operation::CancelOrder: return tossEnvelope(QJsonObject{{"orderId", "cancel-operation-001"}});
    case Operation::AmendOrder: return tossEnvelope(QJsonObject{{"orderId", "amend-operation-001"}});
    default: { HttpResult r; r.status = 400; r.body = R"({"error":{"code":"unsupported-fixture","message":"synthetic"}})"; return r; }
    }
}
} // namespace keefi::test
