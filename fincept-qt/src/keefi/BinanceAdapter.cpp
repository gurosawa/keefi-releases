#include "keefi/BinanceAdapter.h"
#include "keefi/BinanceTiming.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <sodium.h>
#include <limits>

namespace keefi {
namespace {
using Params = QList<QPair<QByteArray, QString>>;
constexpr qint64 DayMs = 24LL * 60 * 60 * 1000;
QString text(const QJsonObject& object, const char* key) { return object.value(QLatin1String(key)).toString(); }
bool integer(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,20}$"));
    return pattern.match(value).hasMatch();
}
bool symbolValid(const QString& value) {
    return binanceIdentifierValid(value, 2);
}
bool assetValid(const QString& value) {
    return binanceIdentifierValid(value);
}
bool clientValid(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._:/-]{1,36}$"));
    return pattern.match(value).hasMatch();
}
bool credentialValid(const SecureBuffer& value) {
    if (value.empty() || value.size() > 512) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = value.data()[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}
QByteArray encoded(const Params& params) {
    QByteArray result;
    for (const auto& pair : params) {
        if (!result.isEmpty()) result += '&';
        result += pair.first + '=' + QUrl::toPercentEncoding(pair.second);
    }
    return result;
}
bool finiteDecimal(const QString& value) { return value.size() <= 80 && decimal::valid(value); }
bool amount(const QString& value) { return finiteDecimal(value) && decimal::positive(value); }
bool zero(const QString& value) { return finiteDecimal(value) && decimal::compare(value, QStringLiteral("0")) == 0; }
struct HistoryWindow { qint64 start = 0, end = 0; QString id; bool valid = false; };
HistoryWindow historyWindow(const OrderInput& input) {
    HistoryWindow window;
    if (input.startTimeMs < 0 || input.endTimeMs < 0 ||
        (input.startTimeMs && input.endTimeMs && input.startTimeMs > input.endTimeMs)) return window;
    window.start = input.startTimeMs;
    if (!input.cursor.isEmpty()) {
        const auto parts = input.cursor.split(QLatin1Char('|'));
        if (parts.size() != 2 || !integer(parts[0]) || (!parts[1].isEmpty() && !integer(parts[1]))) return window;
        bool ok = false;
        window.start = parts[0].toLongLong(&ok);
        if (!ok || window.start < input.startTimeMs || (input.endTimeMs && window.start > input.endTimeMs)) return window;
        window.id = parts[1];
    }
    window.end = input.endTimeMs;
    if (window.start && window.end && window.end - window.start >= DayMs) window.end = window.start + DayMs - 1;
    window.valid = true;
    return window;
}
QString nextInteger(const QString& value) {
    if (!integer(value)) return {};
    QString result = value;
    for (qsizetype i = result.size() - 1; i >= 0; --i) {
        if (result[i] != QLatin1Char('9')) { result[i] = QChar(result[i].unicode() + 1); return result; }
        result[i] = QLatin1Char('0');
    }
    return QLatin1Char('1') + result;
}
bool decodeOrder(const QJsonObject& object, OrderRecord* order) {
    order->orderId = text(object, "orderId");
    order->clientId = text(object, "clientOrderId");
    order->symbol = text(object, "symbol");
    order->side = text(object, "side").toLower();
    order->type = text(object, "type").toLower();
    const QString exchangeStatus = text(object, "status");
    order->price = text(object, "price");
    order->quantity = object.contains(QStringLiteral("origQty")) ? text(object, "origQty") : text(object, "qty");
    order->executed = text(object, "executedQty");
    order->quoteAmount = object.contains(QStringLiteral("cummulativeQuoteQty")) ? text(object, "cummulativeQuoteQty") : text(object, "cumulativeQuoteQty");
    if (order->quoteAmount.startsWith(QLatin1Char('-'))) order->quoteAmount.clear(); // Historical unavailable value.
    if (!integer(order->orderId) || !clientValid(order->clientId) || !symbolValid(order->symbol) ||
        (order->side != QStringLiteral("buy") && order->side != QStringLiteral("sell")) ||
        !finiteDecimal(order->price) || !finiteDecimal(order->quantity) || !finiteDecimal(order->executed) ||
        (!order->quoteAmount.isEmpty() && !finiteDecimal(order->quoteAmount))) return false;
    static const QMap<QString, QString> statuses = {{QStringLiteral("NEW"), QStringLiteral("open")},
        {QStringLiteral("PARTIALLY_FILLED"), QStringLiteral("partial")}, {QStringLiteral("FILLED"), QStringLiteral("filled")},
        {QStringLiteral("CANCELED"), QStringLiteral("canceled")}, {QStringLiteral("REJECTED"), QStringLiteral("rejected")},
        {QStringLiteral("EXPIRED"), QStringLiteral("expired")}, {QStringLiteral("EXPIRED_IN_MATCH"), QStringLiteral("expired")}};
    order->status = statuses.value(exchangeStatus, QStringLiteral("unknown"));
    if (exchangeStatus.isEmpty() || order->type.isEmpty()) return false;
    order->remaining = decimal::subtract(order->quantity, order->executed);
    if (order->remaining.isEmpty()) return false;
    const bool active = order->status == QStringLiteral("open") || order->status == QStringLiteral("partial");
    const auto list=text(object,"orderListId");const bool standalone=list=="-1";
    if(!standalone&&integer(list))order->orderListId=list;
    order->cancelable = active && (standalone||!order->orderListId.isEmpty());
    order->amendable = active && standalone && order->type == QStringLiteral("limit");
    return true;
}
AdapterResult failure(Operation op, QString code, QString message, bool ambiguous = false) {
    AdapterResult result;
    result.code = std::move(code); result.message = std::move(message);
    result.unknown = isMutation(op) && ambiguous;
    result.needsReconciliation = result.unknown;
    return result;
}
QString balanceOf(const MarketSnapshot& snapshot, const QString& asset, bool includeLocked = false) {
    for (const auto& balance : snapshot.balances) {
        if (balance.asset == asset && finiteDecimal(balance.free) && finiteDecimal(balance.locked))
            return includeLocked ? decimal::add(balance.free, balance.locked) : balance.free;
    }
    return {};
}
QString basisPrice(const QJsonObject& filter, const MarketSnapshot& snapshot) {
    const auto reference = snapshot.data.value(Operation::Reference);
    // Missing/failed reference lookup must not be mistaken for a known-null reference.
    if (!reference.contains(QStringLiteral("referencePrice"))) return {};
    const auto referenceValue = reference.value(QStringLiteral("referencePrice"));
    if (!referenceValue.isNull()) return amount(referenceValue.toString()) ? referenceValue.toString() : QString();
    const QString minutes = text(filter, "avgPriceMins");
    if (!integer(minutes)) return {};
    if (zero(minutes)) return text(snapshot.data.value(Operation::Quote), "price");
    const auto average = snapshot.data.value(Operation::AveragePrice);
    if (!integer(text(average, "mins")) || decimal::compare(minutes, text(average, "mins")) != 0 || !amount(text(average, "price"))) return {};
    return text(average, "price");
}
}

QList<Operation> BinanceAdapter::refreshPlan() const {
    return {Operation::Time, Operation::Rules, Operation::Quote, Operation::Depth, Operation::Reference, Operation::AveragePrice,
        Operation::AssetFilters, Operation::Account, Operation::OpenOrders, Operation::History};
}
QString BinanceAdapter::amendDescription() const {
    return QStringLiteral("같은 가격의 총 수량 감소는 우선순위를 유지합니다. 가격 변경·수량 증가는 취소 후 새 지정가 주문이며 우선순위를 잃습니다. 새 수량은 추가 주문 수량입니다. 취소 성공 뒤 신규 실패도 가능합니다. 묶음 정정은 미지원입니다.");
}

RequestSpec BinanceAdapter::build(Operation operation, const AccountRecord& account, const OrderInput& input,
    const CredentialSet& credentials, qint64 serverNowMs, QString* error) const {
    if (error) error->clear();
    RequestSpec request; request.operation = operation;
    const auto reject = [&](const QString& message) { if (error) *error = message; return RequestSpec{}; };
    if (account.provider != QStringLiteral("binance") || account.market != QStringLiteral("spot") ||
        account.environment != QStringLiteral("live"))
        return reject(QStringLiteral("Binance 현물 실전 계정만 지원합니다."));
    const QByteArray origin("https://api.binance.com");
    const bool noSymbol = operation == Operation::Time || operation == Operation::Markets || operation == Operation::Account || operation == Operation::OpenOrders;
    if (!noSymbol && !symbolValid(input.symbol)) return reject(QStringLiteral("Binance 종목 형식을 확인하세요."));
    Params params;
    QByteArray path;
    switch (operation) {
    case Operation::Time: path = "/api/v3/time"; break;
    case Operation::Markets:
        path = "/api/v3/exchangeInfo";
        params.append({"permissions", QStringLiteral("SPOT")});
        params.append({"showPermissionSets", QStringLiteral("false")});
        break;
    case Operation::Rules: path = "/api/v3/exchangeInfo"; params.append({"symbol", input.symbol}); break;
    case Operation::Quote: path = "/api/v3/ticker/price"; params.append({"symbol", input.symbol}); break;
    case Operation::Depth: path = "/api/v3/depth"; params.append({"symbol", input.symbol}); params.append({"limit", QStringLiteral("100")}); break;
    case Operation::Reference: path = "/api/v3/referencePrice"; params.append({"symbol", input.symbol}); break;
    case Operation::AveragePrice: path = "/api/v3/avgPrice"; params.append({"symbol", input.symbol}); break;
    case Operation::AssetFilters: path = "/api/v3/myFilters"; params.append({"symbol", input.symbol}); request.authenticated = true; break;
    case Operation::Account: path = "/api/v3/account"; request.authenticated = true; break;
    case Operation::OpenOrders: path = "/api/v3/openOrders"; request.authenticated = true; break;
    case Operation::History: {
        const auto window = historyWindow(input);
        if (!window.valid) return reject(QStringLiteral("주문 이력 조회 범위 또는 cursor가 올바르지 않습니다."));
        path = "/api/v3/allOrders"; request.authenticated = true;
        params.append({"symbol", input.symbol}); params.append({"limit", QStringLiteral("1000")});
        if (window.start) params.append({"startTime", QString::number(window.start)});
        if (window.end) params.append({"endTime", QString::number(window.end)});
        if (!window.id.isEmpty()) params.append({"orderId", window.id});
        break;
    }
    case Operation::OrderList:
        if(!integer(input.orderListId))return reject("주문 묶음 ID를 확인하세요.");
        path="/api/v3/orderList";request.authenticated=true;params.append({"orderListId",input.orderListId});break;
    case Operation::QueryOrder:

     {

        path = QByteArray("/api/v3/order");
        request.authenticated = true;

        params.append({"symbol", input.symbol});
        if (!input.orderId.isEmpty()) {
            if (!integer(input.orderId)) return reject(QStringLiteral("주문 ID 형식이 올바르지 않습니다."));
            params.append({"orderId", input.orderId});
        } else {
            if (!clientValid(input.clientId)) return reject(QStringLiteral("조회할 주문 식별자가 필요합니다."));
            params.append({"origClientOrderId", input.clientId});
        }

        break;
    }
    default: return reject(QStringLiteral("지원하지 않는 Binance 요청입니다."));
    }
    if (path.isEmpty()) return reject(QStringLiteral("지원하지 않는 Binance 요청입니다."));
    if (request.authenticated) {
        if (serverNowMs <= 0) return reject(QStringLiteral("확인된 Binance 서버 시각이 필요합니다."));
        const auto key = credentials.find(QStringLiteral("api_key")), secret = credentials.find(QStringLiteral("secret"));
        if (key == credentials.end() || secret == credentials.end() || !credentialValid(key->second) || !credentialValid(secret->second))
            return reject(QStringLiteral("HMAC API key·secret만 지원합니다. RSA·Ed25519 키는 사용할 수 없습니다."));
        params.append({"recvWindow", QString::number(BinanceRecvWindowMs)}); params.append({"timestamp", QString::number(serverNowMs)});
        QByteArray wire = encoded(params);
        // Keep secret-derived key pads/state and the binary digest in guarded,
        // locked storage. SecureBuffer explicitly wipes both on every exit.
        SecureBuffer stateStorage(sizeof(crypto_auth_hmacsha256_state));
        SecureBuffer digest(crypto_auth_hmacsha256_BYTES);
        auto* state = reinterpret_cast<crypto_auth_hmacsha256_state*>(stateStorage.data());
        if (crypto_auth_hmacsha256_init(state, secret->second.data(), secret->second.size()) ||
            crypto_auth_hmacsha256_update(state, reinterpret_cast<const unsigned char*>(wire.constData()),
                static_cast<unsigned long long>(wire.size())) ||
            crypto_auth_hmacsha256_final(state, digest.data()))
            return reject(QStringLiteral("Binance 서명을 생성하지 못했습니다."));
        QByteArray signature = QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()),
            qsizetype(digest.size())).toHex();
        stateStorage.clear();
        digest.clear();
        wire.append("&signature=");
        wire.append(signature);
        sodium_memzero(signature.data(), std::size_t(signature.size()));
        request.headers.insert("X-MBX-APIKEY", QByteArray(reinterpret_cast<const char*>(key->second.data()), qsizetype(key->second.size())));
        if (request.method == "GET" || request.method == "DELETE") request.url = QUrl::fromEncoded(origin + path + '?' + wire, QUrl::StrictMode);
        else { request.url = QUrl::fromEncoded(origin + path); request.body = wire; request.headers.insert("Content-Type", "application/x-www-form-urlencoded"); }
    } else {
        const auto query = encoded(params);
        request.url = QUrl::fromEncoded(origin + path + (query.isEmpty() ? QByteArray() : '?' + query), QUrl::StrictMode);
    }
    return request;
}

AdapterResult BinanceAdapter::parse(Operation operation, const HttpResult& response, const OrderInput& input) const {
    if (response.timedOut || response.networkError || response.status == 0)
        return failure(operation, QStringLiteral("transport_unknown"), QStringLiteral("요청 결과를 확인하지 못했습니다. 주문은 다시 보내지 말고 조회로 확인하세요."), true);
    QString parseError;
    const auto document = parseExactJson(response.body, &parseError,
        operation == Operation::Markets ? JsonResponseScope::BinanceMarketCatalog : JsonResponseScope::Standard);
    const auto object = document.object();
    const auto exchangeCode = text(object, "code");
    int retryAfter = 0;
    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it) {
        if (it.key().compare("Retry-After", Qt::CaseInsensitive) == 0) {
            bool ok = false; const auto seconds = it.value().toLongLong(&ok);
            if (ok && seconds >= 0) retryAfter = int(qMin(seconds, qint64(std::numeric_limits<int>::max())));
        }
    }
    if (response.status == 429 || response.status == 418) {
        auto result = failure(operation, QStringLiteral("rate_limited"), QStringLiteral("Binance 요청 제한으로 대기 중입니다."), true);
        result.rateLimited = true; result.retryAfterSeconds = qMax(1, retryAfter); return result;
    }
    if (operation == Operation::Reference && exchangeCode == QStringLiteral("-2043") && response.status >= 400 && response.status < 500) {
        AdapterResult result; result.ok = true; result.code = QStringLiteral("reference_unset");
        result.data.insert(QStringLiteral("symbol"), input.symbol); result.data.insert(QStringLiteral("referencePrice"), QJsonValue::Null);
        return result;
    }

    if (response.status >= 500 || response.status == 408 || response.status == 409 ||
        (response.status >= 300 && response.status < 400) || exchangeCode == QStringLiteral("-1006") ||
        exchangeCode == QStringLiteral("-1007") || exchangeCode == QStringLiteral("-1008"))
        return failure(operation, QStringLiteral("exchange_result_unknown"), QStringLiteral("거래소 처리 결과가 불명확합니다. 자동 재주문 없이 원주문을 조회해야 합니다."), true);
    if (response.status < 200 || response.status >= 300 || exchangeCode.startsWith(QLatin1Char('-'))) {
        auto result = failure(operation, QStringLiteral("exchange_rejected"), QStringLiteral("Binance가 요청을 거부했습니다. 권한·입력·계좌 상태를 확인하세요."));
        result.authenticationFailed = response.status == 401 || response.status == 403 ||
            exchangeCode == QStringLiteral("-2014") || exchangeCode == QStringLiteral("-2015") || exchangeCode == QStringLiteral("-1022");
        if (result.authenticationFailed) { result.code = QStringLiteral("authentication_failed"); result.message = QStringLiteral("Binance 키·서명·접근 권한을 확인하세요."); }
        if (exchangeCode == QStringLiteral("-1021")) { result.code = QStringLiteral("timestamp_rejected"); result.message = QStringLiteral("Binance 서버 시각을 다시 확인해야 합니다."); }
        if (exchangeCode == QStringLiteral("-2038")) { result.code = QStringLiteral("amend_limit_rejected"); result.message = QStringLiteral("거래소가 이 주문의 정정 횟수 한도 초과로 정정을 거부했습니다."); }
        if (exchangeCode == QStringLiteral("-2013") || exchangeCode == QStringLiteral("-2026")) {
            result.notFound = true; result.code = exchangeCode == QStringLiteral("-2026") ? QStringLiteral("order_archived") : QStringLiteral("order_not_found");
            result.message = QStringLiteral("조회에서 주문을 찾지 못했습니다. 미접수 증거가 아니므로 자동 재주문하지 않습니다.");
        }
        return result;
    }
    const auto malformed = [&]() { return failure(operation, QStringLiteral("invalid_response"), QStringLiteral("Binance 응답을 안전하게 해석하지 못했습니다."), true); };
    if (!parseError.isEmpty() || document.isNull()) return malformed();
    AdapterResult result; result.ok = true; result.code = QStringLiteral("ok");

    if(operation==Operation::OrderList){
        if(!document.isObject()||text(object,"orderListId")!=input.orderListId||text(object,"symbol")!=input.symbol||
            !QStringList{"EXECUTING","ALL_DONE","REJECT"}.contains(text(object,"listOrderStatus"))||!object.value("orders").isArray()||object.value("orders").toArray().isEmpty()||object.value("orders").toArray().size()>10)return malformed();
        QSet<QString> ids;for(const auto& value:object.value("orders").toArray()){const auto row=value.toObject();const auto id=text(row,"orderId");
            if(!value.isObject()||!integer(id)||ids.contains(id)||text(row,"symbol")!=input.symbol)return malformed();ids.insert(id);
        }
        result.data={{"orderListId",input.orderListId},{"symbol",input.symbol},{"listOrderStatus",object.value("listOrderStatus")},{"listClientOrderId",object.value("listClientOrderId")}};
        return result;
    }
    switch (operation) {
    case Operation::Time: {
        bool ok = false; result.serverTimeMs = text(object, "serverTime").toLongLong(&ok);
        if (!ok || result.serverTimeMs <= 0) return malformed();
        result.data = object; break;
    }
    case Operation::Markets:
        if (!object.value(QStringLiteral("symbols")).isArray()) return malformed();
        result.data = object; break;
    case Operation::Rules: {
        if (!object.value(QStringLiteral("symbols")).isArray() || !object.value(QStringLiteral("exchangeFilters")).isArray()) return malformed();
        bool found = false;
        for (const auto& value : object.value(QStringLiteral("symbols")).toArray()) {
            const auto symbol = value.toObject();
            if (text(symbol, "symbol") != input.symbol) continue;
            if (found || !symbol.value(QStringLiteral("filters")).isArray() || !symbol.value(QStringLiteral("orderTypes")).isArray()) return malformed();
            result.data = symbol; found = true;
        }
        if (!found) return malformed();
        result.data.insert(QStringLiteral("exchangeFilters"), object.value(QStringLiteral("exchangeFilters")));
        result.data.insert(QStringLiteral("rateLimits"), object.value(QStringLiteral("rateLimits"))); break;
    }
    case Operation::Quote:
        if (text(object, "symbol") != input.symbol || !amount(text(object, "price"))) return malformed();
        result.data = object; result.data.insert(QStringLiteral("lastPrice"), text(object, "price")); break;
    case Operation::Depth: {
        if (!integer(text(object, "lastUpdateId"))) return malformed();
        for (const auto* side : {"bids", "asks"}) {
            if (!object.value(QLatin1String(side)).isArray()) return malformed();
            const auto levels = object.value(QLatin1String(side)).toArray();
            if (levels.size() > 100) return malformed();
            QString prior;
            for (const auto& value : levels) {
                const auto level = value.toArray();
                if (level.size() != 2 || !amount(level[0].toString()) || !amount(level[1].toString())) return malformed();
                const auto price = level[0].toString();
                if (!prior.isEmpty() && ((QByteArray(side) == "asks" && decimal::compare(price, prior) <= 0) ||
                    (QByteArray(side) == "bids" && decimal::compare(price, prior) >= 0))) return malformed();
                prior = price;
            }
        }
        const auto bids = object.value(QStringLiteral("bids")).toArray(), asks = object.value(QStringLiteral("asks")).toArray();
        if (!bids.isEmpty() && !asks.isEmpty() && decimal::compare(bids.first().toArray()[0].toString(), asks.first().toArray()[0].toString()) >= 0) return malformed();
        result.data = object; result.data.insert(QStringLiteral("symbol"), input.symbol); break;
    }
    case Operation::Reference:
        if (text(object, "symbol") != input.symbol || !object.contains(QStringLiteral("referencePrice")) ||
            (!object.value(QStringLiteral("referencePrice")).isNull() && !amount(text(object, "referencePrice")))) return malformed();
        result.data = object; break;
    case Operation::AveragePrice:
        if (!amount(text(object, "price")) || !integer(text(object, "mins")) || !integer(text(object, "closeTime"))) return malformed();
        result.data = object; break;
    case Operation::AssetFilters:
        if (!object.value(QStringLiteral("exchangeFilters")).isArray() || !object.value(QStringLiteral("symbolFilters")).isArray() ||
            !object.value(QStringLiteral("assetFilters")).isArray()) return malformed();
        result.data = object; break;
    case Operation::Account:
        if (!object.value(QStringLiteral("canTrade")).isBool() || !object.value(QStringLiteral("balances")).isArray() ||
            text(object, "accountType") != QStringLiteral("SPOT")) return malformed();
        result.tradingPermissionKnown = true; result.tradingAllowed = object.value(QStringLiteral("canTrade")).toBool();
        for (const auto& value : object.value(QStringLiteral("balances")).toArray()) {
            const auto balance = value.toObject();
            const Balance row{text(balance, "asset"), text(balance, "free"), text(balance, "locked")};
            if (!assetValid(row.asset) || !finiteDecimal(row.free) || !finiteDecimal(row.locked)) return malformed();
            result.balances.append(row);
        }
        result.data = object; break;
    case Operation::OpenOrders:
    case Operation::History: {
        if (!document.isArray()) return malformed();
        const auto rows = document.array();
        QSet<QString> seen;
        QString largestId;
        for (const auto& row : rows) {
            OrderRecord order;
            if (!row.isObject() || !decodeOrder(row.toObject(), &order) ||
                (operation == Operation::History && order.symbol != input.symbol)) return malformed();
            const auto key = order.symbol + QLatin1Char(':') + order.orderId;
            if (seen.contains(key)) return malformed();
            seen.insert(key); result.orders.append(order);
            if (largestId.isEmpty() || decimal::compare(order.orderId, largestId) > 0) largestId = order.orderId;
        }
        if (operation == Operation::OpenOrders) result.scope = QStringLiteral("연결한 Binance 현물 계좌의 모든 종목 미체결 주문. 외부에서 생성한 주문 포함.");
        else {
            const auto window = historyWindow(input);
            if (!window.valid) return malformed();
            result.scope = QStringLiteral("선택 종목의 주문·누적 체결량/금액 이력. 개별 체결·전 종목 전체 기록이 아닙니다.");
            if (rows.size() >= 1000) {
                if (largestId.isEmpty()) return malformed();
                result.hasMore = true; result.nextCursor = QString::number(window.start) + QLatin1Char('|') + nextInteger(largestId);
            } else if (window.end && input.endTimeMs && window.end < input.endTimeMs) {
                result.hasMore = true; result.nextCursor = QString::number(window.end + 1) + QLatin1Char('|');
            }
            if (!window.start || !input.endTimeMs) result.scope += QStringLiteral(" 기간 미지정 시 최근 조회 범위이며 완전한 과거 기록을 보장하지 않습니다.");
        }
        break;
    }
    case Operation::QueryOrder:

     {
        const auto payload = object;
        OrderRecord order;
        if (!decodeOrder(payload, &order) || order.symbol != input.symbol) return malformed();

        const QString identifiedClient = order.clientId;
        if (operation == Operation::QueryOrder &&
            ((!input.orderId.isEmpty() && order.orderId != input.orderId) || (input.orderId.isEmpty() && identifiedClient != input.clientId))) return malformed();

        result.orders.append(order);
        result.needsReconciliation = isMutation(operation);
        break;
    }
    default: return malformed();
    }
    return result;
}

OrderValidation BinanceAdapter::validate(const OrderInput& input, const MarketSnapshot& snapshot, const OrderLimits& limits) const {
    OrderValidation result;
    const auto reject = [&](const QString& message) { result.message = message; return result; };
    const auto rules = snapshot.data.value(Operation::Rules);
    const auto account = snapshot.data.value(Operation::Account);
    const auto assetRules = snapshot.data.value(Operation::AssetFilters);
    const bool amend = !input.orderId.isEmpty()&&!input.cancelReplace;
    if(input.cancelReplace){const auto it=std::find_if(snapshot.openOrders.cbegin(),snapshot.openOrders.cend(),[&](const auto& o){return o.orderId==input.orderId&&o.symbol==input.symbol;});
        if(it==snapshot.openOrders.cend()||!it->amendable||it->clientId!=input.clientId||it->side!=input.side||it->type!="limit"||input.type!="limit")return reject("현재 독립 지정가 주문의 방향·종목을 유지한 취소·신규만 가능합니다.");
    }
    const bool market = input.type == QStringLiteral("market");
    const bool quoteQuantity = !input.quoteAmount.isEmpty();
    if (!symbolValid(input.symbol) || text(rules, "symbol") != input.symbol || text(rules, "status") != QStringLiteral("TRADING") ||
        !rules.value(QStringLiteral("isSpotTradingAllowed")).toBool() || !account.value(QStringLiteral("canTrade")).toBool())
        return reject(QStringLiteral("종목 거래 상태·현물 허용·계좌 거래 가능 정보를 확인해야 합니다."));
    if ((input.side != QStringLiteral("buy") && input.side != QStringLiteral("sell")) ||
        (input.type != QStringLiteral("limit") && !market) || !rules.value(QStringLiteral("orderTypes")).toArray().contains(input.type.toUpper()))
        return reject(QStringLiteral("이 종목에서 지원하는 지정가·시장가 주문만 입력하세요."));
    if (!market && (!amount(input.price) || !amount(input.quantity) || quoteQuantity)) return reject(QStringLiteral("지정가 가격·수량이 올바르지 않습니다."));
    if (market && (!input.price.isEmpty() || (input.quantity.isEmpty() == input.quoteAmount.isEmpty()) ||
        (!input.quantity.isEmpty() && !amount(input.quantity)) || (quoteQuantity && !amount(input.quoteAmount))))
        return reject(QStringLiteral("시장가는 수량 또는 호가 자산 금액 하나만 입력하세요."));
    if (quoteQuantity && !rules.value(QStringLiteral("quoteOrderQtyMarketAllowed")).toBool()) return reject(QStringLiteral("이 종목은 시장가 금액 입력을 허용하지 않습니다."));
    const auto last = text(snapshot.data.value(Operation::Quote), "price");
    if (!amount(last) || text(snapshot.data.value(Operation::Quote), "symbol") != input.symbol)
        return reject(QStringLiteral("현재 종목의 유효한 시세가 필요합니다."));
    const QString base = text(rules, "baseAsset"), quote = text(rules, "quoteAsset");
    if (base.isEmpty() || quote.isEmpty()) return reject(QStringLiteral("거래 자산 단위를 확인하지 못했습니다."));
    result.currency = quote;
    result.notional = quoteQuantity ? input.quoteAmount : decimal::multiply(input.quantity, market ? last : input.price);
    if (!amount(result.notional)) return reject(QStringLiteral("주문 예상 금액을 계산할 수 없습니다."));
    if (market) {
        if (quoteQuantity) return reject(QStringLiteral("시장가 금액 입력은 수량·호가 필터를 정확히 검증할 수 없어 미지원입니다. base 수량을 입력하세요."));
        const auto depth = snapshot.data.value(Operation::Depth);
        if (text(depth, "symbol") != input.symbol || !finiteDecimal(limits.maxDeviationPercent))
            return reject(QStringLiteral("시장가 주문에는 최신 종목 호가와 괴리율 한도가 필요합니다."));
        const auto levels = depth.value(input.side == QStringLiteral("buy") ? QStringLiteral("asks") : QStringLiteral("bids"));
        if (!levels.isArray() || levels.toArray().isEmpty()) return reject(QStringLiteral("시장가 주문을 소화할 호가 잔량이 없습니다."));
        QString remaining = input.quantity, cost = QStringLiteral("0"), worstPrice, previousPrice;
        for (const auto& value : levels.toArray()) {
            const auto level = value.toArray();
            if (level.size() != 2 || !amount(level[0].toString()) || !amount(level[1].toString()))
                return reject(QStringLiteral("호가 가격·잔량을 확인할 수 없습니다."));
            const QString levelPrice = level[0].toString(), levelQuantity = level[1].toString();
            if (!previousPrice.isEmpty() && ((input.side == QStringLiteral("buy") && decimal::compare(levelPrice, previousPrice) <= 0) ||
                (input.side == QStringLiteral("sell") && decimal::compare(levelPrice, previousPrice) >= 0)))
                return reject(QStringLiteral("호가 순서가 올바르지 않습니다."));
            previousPrice = levelPrice;
            const auto take = decimal::compare(remaining, levelQuantity) <= 0 ? remaining : levelQuantity;
            cost = decimal::add(cost, decimal::multiply(take, levelPrice));
            remaining = decimal::subtract(remaining, take); worstPrice = levelPrice;
            if (remaining.isEmpty() || cost.isEmpty()) return reject(QStringLiteral("호가 잔량 계산 범위를 초과했습니다."));
            if (zero(remaining)) break;
        }
        if (!zero(remaining)) return reject(QStringLiteral("조회한 100단계 호가 잔량으로 전체 시장가 수량을 충족하지 못합니다."));
        const auto deviation = decimal::compare(worstPrice, last) >= 0 ? decimal::subtract(worstPrice, last) : decimal::subtract(last, worstPrice);
        if (decimal::compare(decimal::multiply(deviation, QStringLiteral("100")), decimal::multiply(last, limits.maxDeviationPercent)) > 0)
            return reject(QStringLiteral("시장가가 소비할 호가 가격이 설정한 괴리율을 초과합니다."));
        result.notional = cost;
    }
    if (amend) {
        if (market || !rules.value(QStringLiteral("amendAllowed")).toBool()) return reject(QStringLiteral("이 주문의 수량 감소 정정을 지원하지 않습니다."));
        const OrderRecord* existing = nullptr;
        for (const auto& row : snapshot.openOrders) if (row.orderId == input.orderId && row.symbol == input.symbol) { existing = &row; break; }
        if (!existing || !existing->amendable || existing->clientId != input.clientId || existing->side != input.side ||
            existing->type != input.type || !finiteDecimal(existing->quantity) || !finiteDecimal(existing->executed) ||
            !finiteDecimal(existing->price) || decimal::compare(existing->price, input.price) != 0 ||
            decimal::compare(input.quantity, existing->quantity) >= 0 || decimal::compare(input.quantity, existing->executed) <= 0)
            return reject(QStringLiteral("정정은 조회된 주문의 가격·방향을 유지하며 체결량보다 큰 총 수량으로 감소할 때만 가능합니다."));
    }
    if (!rules.value(QStringLiteral("filters")).isArray() || !rules.value(QStringLiteral("exchangeFilters")).isArray() ||
        !assetRules.value(QStringLiteral("symbolFilters")).isArray() || !assetRules.value(QStringLiteral("exchangeFilters")).isArray() ||
        !assetRules.value(QStringLiteral("assetFilters")).isArray())
        return reject(QStringLiteral("현재 종목·계좌의 전체 적용 필터를 확인해야 합니다."));
    QJsonArray filters = rules.value(QStringLiteral("filters")).toArray();
    for (const auto& filter : rules.value(QStringLiteral("exchangeFilters")).toArray()) filters.append(filter);
    for (const auto& filter : assetRules.value(QStringLiteral("symbolFilters")).toArray()) filters.append(filter);
    for (const auto& filter : assetRules.value(QStringLiteral("exchangeFilters")).toArray()) filters.append(filter);
    for (const auto& filter : assetRules.value(QStringLiteral("assetFilters")).toArray()) filters.append(filter);
    bool priceFilterSeen = false, lotFilterSeen = false;
    const auto range = [&](const QString& value, const QString& minimum, const QString& maximum, const QString& step) {
        if (!finiteDecimal(value) || !finiteDecimal(minimum) || !finiteDecimal(maximum) || !finiteDecimal(step)) return false;
        return (zero(minimum) || decimal::compare(value, minimum) >= 0) && (zero(maximum) || decimal::compare(value, maximum) <= 0) &&
            (zero(step) || decimal::multipleOf(value, step));
    };
    for (const auto& value : filters) {
        if (!value.isObject()) return reject(QStringLiteral("거래 필터 형식을 확인할 수 없습니다."));
        const auto filter = value.toObject(); const auto type = text(filter, "filterType");
        if (type == QStringLiteral("PRICE_FILTER")) {
            priceFilterSeen = true;
            if (!market && !range(input.price, text(filter, "minPrice"), text(filter, "maxPrice"), text(filter, "tickSize")))
                return reject(QStringLiteral("가격 최소·최대 또는 호가 단위를 만족하지 않습니다."));
        } else if (type == QStringLiteral("LOT_SIZE") || type == QStringLiteral("MARKET_LOT_SIZE")) {
            if (type == QStringLiteral("LOT_SIZE")) lotFilterSeen = true;
            if (!market && type == QStringLiteral("MARKET_LOT_SIZE")) continue;
            if (quoteQuantity) {
                // quoteOrderQty is converted to LOT_SIZE-compliant base quantity by Binance.
                // Other base-quantity limits cannot be proved locally from a quote budget.
                if (type == QStringLiteral("MARKET_LOT_SIZE") &&
                    !(zero(text(filter, "minQty")) && zero(text(filter, "maxQty")) && zero(text(filter, "stepSize"))))
                    return reject(QStringLiteral("시장가 수량 필터 검증을 위해 base 자산 수량으로 입력하세요."));
            } else if (!range(input.quantity, text(filter, "minQty"), text(filter, "maxQty"), text(filter, "stepSize")))
                return reject(QStringLiteral("수량 최소·최대 또는 수량 단위를 만족하지 않습니다."));
        } else if (type == QStringLiteral("MIN_NOTIONAL") || type == QStringLiteral("NOTIONAL")) {
            const bool minOnly = type == QStringLiteral("MIN_NOTIONAL");
            if (!filter.value(minOnly ? QStringLiteral("applyToMarket") : QStringLiteral("applyMinToMarket")).isBool() ||
                (!minOnly && !filter.value(QStringLiteral("applyMaxToMarket")).isBool()))
                return reject(QStringLiteral("금액 필터의 시장가 적용 범위를 확인하지 못했습니다."));
            const bool applyMin = !market || filter.value(minOnly ? QStringLiteral("applyToMarket") : QStringLiteral("applyMinToMarket")).toBool();
            const bool applyMax = !minOnly && (!market || filter.value(QStringLiteral("applyMaxToMarket")).toBool());
            if (!applyMin && !applyMax) continue;
            QString notional = result.notional;
            if (market && !quoteQuantity) {
                const auto basis = basisPrice(filter, snapshot);
                if (!amount(basis)) return reject(QStringLiteral("시장가 금액 필터에 필요한 기준가·동일 구간 평균가가 없습니다."));
                notional = decimal::multiply(input.quantity, basis);
            }
            if (!amount(notional) || (applyMin && (!finiteDecimal(text(filter, "minNotional")) || decimal::compare(notional, text(filter, "minNotional")) < 0)) ||
                (applyMax && (!finiteDecimal(text(filter, "maxNotional")) || decimal::compare(notional, text(filter, "maxNotional")) > 0)))
                return reject(QStringLiteral("주문 금액이 거래소 최소·최대 금액 필터를 만족하지 않습니다."));
        } else if (type == QStringLiteral("PERCENT_PRICE") || type == QStringLiteral("PERCENT_PRICE_BY_SIDE")) {
            if (market) continue;
            const auto basis = basisPrice(filter, snapshot);
            const bool bySide = type == QStringLiteral("PERCENT_PRICE_BY_SIDE");
            const auto up = text(filter, bySide ? (input.side == QStringLiteral("buy") ? "bidMultiplierUp" : "askMultiplierUp") : "multiplierUp");
            const auto down = text(filter, bySide ? (input.side == QStringLiteral("buy") ? "bidMultiplierDown" : "askMultiplierDown") : "multiplierDown");
            if (!amount(basis) || !amount(up) || !amount(down)) return reject(QStringLiteral("가격 비율 필터의 기준가·동일 구간 평균가를 확인해야 합니다."));
            if (decimal::compare(input.price, decimal::multiply(basis, up)) > 0 || decimal::compare(input.price, decimal::multiply(basis, down)) < 0)
                return reject(QStringLiteral("가격이 거래소 방향별 가격 비율 범위를 벗어났습니다."));
        } else if (type == QStringLiteral("MAX_NUM_ORDERS") || type == QStringLiteral("EXCHANGE_MAX_NUM_ORDERS")) {
            if (amend) continue; // Keep-priority consumes no new order slot.
            if (!integer(text(filter, "maxNumOrders")) || !snapshot.data.contains(Operation::OpenOrders))
                return reject(QStringLiteral("미체결 개수 제한을 확인해야 합니다."));
            int count = 0;
            for (const auto& row : snapshot.openOrders) if (type.startsWith(QStringLiteral("EXCHANGE_")) || row.symbol == input.symbol) ++count;
            if (decimal::compare(QString::number(count), text(filter, "maxNumOrders")) >= 0) return reject(QStringLiteral("계좌의 미체결 주문 개수 한도에 도달했습니다."));
        } else if (type == QStringLiteral("MAX_POSITION")) {
            if (input.side != QStringLiteral("buy") || amend) continue;
            if (quoteQuantity || !amount(text(filter, "maxPosition")) || !snapshot.data.contains(Operation::OpenOrders))
                return reject(QStringLiteral("보유 한도 검증을 위해 현재 보유량·미체결·base 수량이 필요합니다."));
            QString position = balanceOf(snapshot, base, true);
            if (position.isEmpty()) return reject(QStringLiteral("보유 한도 계산에 필요한 자산 잔고가 없습니다."));
            for (const auto& row : snapshot.openOrders) {
                if (row.side == QStringLiteral("buy") && row.symbol != input.symbol)
                    return reject(QStringLiteral("다른 종목 미체결 매수의 base 자산을 확인하지 못해 보유 한도 검증을 중단했습니다."));
                if (row.symbol == input.symbol && row.side == QStringLiteral("buy")) {
                    if (!finiteDecimal(row.remaining)) return reject(QStringLiteral("미체결 매수 잔량을 확인하지 못했습니다."));
                    position = decimal::add(position, row.remaining);
                }
            }
            if (decimal::compare(decimal::add(position, input.quantity), text(filter, "maxPosition")) > 0)
                return reject(QStringLiteral("주문 후 예상 보유량이 거래소 한도를 초과합니다."));
        } else if (type == QStringLiteral("MAX_ASSET")) {
            const auto asset = text(filter, "asset"), maximum = text(filter, "limit");
            if (!amount(maximum) || (asset != base && asset != quote)) return reject(QStringLiteral("계좌 자산별 거래 한도를 확인하지 못했습니다."));
            if (asset == base && quoteQuantity) return reject(QStringLiteral("자산별 수량 제한 검증을 위해 base 수량으로 입력하세요."));
            if (asset == quote && market && !quoteQuantity) return reject(QStringLiteral("시장가의 실제 호가 자산 금액 상한을 확정할 수 없어 주문을 차단했습니다."));
            if (decimal::compare(asset == base ? input.quantity : result.notional, maximum) > 0)
                return reject(QStringLiteral("계좌 자산별 최대 거래 수량·금액을 초과했습니다."));
        } else if (type == QStringLiteral("MAX_NUM_ORDER_AMENDS")) {
            // The atomic per-order counter (including concurrent external amendments) is
            // server-owned. A known -2038 rejection is handled explicitly; no quota is invented.
            if (!integer(text(filter, "maxNumOrderAmends"))) return reject(QStringLiteral("정정 횟수 제한 형식이 올바르지 않습니다."));
        } else {
            // These filters cannot apply: G2 creates neither conditional/list/iceberg nor trailing orders.
            static const QSet<QString> inapplicable = {QStringLiteral("ICEBERG_PARTS"), QStringLiteral("TRAILING_DELTA"),
                QStringLiteral("MAX_NUM_ALGO_ORDERS"), QStringLiteral("MAX_NUM_ICEBERG_ORDERS"), QStringLiteral("MAX_NUM_ORDER_LISTS"),
                QStringLiteral("EXCHANGE_MAX_NUM_ALGO_ORDERS"), QStringLiteral("EXCHANGE_MAX_NUM_ICEBERG_ORDERS"), QStringLiteral("EXCHANGE_MAX_NUM_ORDER_LISTS")};
            if (!inapplicable.contains(type)) return reject(QStringLiteral("지원하지 않는 적용 필터가 있어 주문을 차단했습니다."));
        }
    }
    if (!priceFilterSeen || !lotFilterSeen) return reject(QStringLiteral("필수 가격·수량 필터가 없습니다."));
    if (!amend) {
        if (!amount(limits.maxNotional) || limits.currency != quote) return reject(QStringLiteral("이 종목의 호가 자산과 같은 통화로 최대 주문 금액을 설정하세요."));
        if (decimal::compare(result.notional, limits.maxNotional) > 0) return reject(QStringLiteral("설정한 최대 주문 예상 금액을 초과했습니다."));
        if (!finiteDecimal(limits.maxDeviationPercent)) return reject(QStringLiteral("가격 괴리율 한도를 설정하세요."));
        if (!market) {
            const QString difference = decimal::compare(input.price, last) >= 0 ? decimal::subtract(input.price, last) : decimal::subtract(last, input.price);
            if (decimal::compare(decimal::multiply(difference, QStringLiteral("100")), decimal::multiply(last, limits.maxDeviationPercent)) > 0)
                return reject(QStringLiteral("현재가 대비 지정가 괴리율이 설정 한도를 초과했습니다."));
        }
        const QString available = balanceOf(snapshot, input.side == QStringLiteral("buy") ? quote : base);
        if (available.isEmpty()) return reject(QStringLiteral("사용 가능한 주문 자산 잔고가 없습니다."));
        QString required = input.side == QStringLiteral("buy") ? result.notional : input.quantity;
        if (input.side == QStringLiteral("sell") && quoteQuantity) return reject(QStringLiteral("매도 잔고 검증을 위해 base 수량을 입력하세요."));
        if (input.side == QStringLiteral("buy")) {
            const auto commissions = account.value(QStringLiteral("commissionRates")).toObject();
            const auto maker = text(commissions, "maker"), taker = text(commissions, "taker");
            if (!finiteDecimal(maker) || !finiteDecimal(taker)) return reject(QStringLiteral("주문 잔고 검증에 필요한 수수료율을 확인하지 못했습니다."));
            const auto fee = decimal::compare(maker, taker) >= 0 ? maker : taker;
            required = decimal::multiply(required, decimal::add(QStringLiteral("1"), fee));
        }
        if (required.isEmpty() || decimal::compare(required, available) > 0) return reject(QStringLiteral("사용 가능한 잔고가 주문 예상 금액·수수료에 부족합니다."));
    }
    result.ok = true;
    result.message = market ? QStringLiteral("현 시세 기준 로컬 검증 완료. 시장가 체결 가격·금액은 달라질 수 있습니다.") : QStringLiteral("로컬 주문 검증 완료. 거래소 접수·체결 결과는 별도로 확인합니다.");
    if (amend) result.message += QStringLiteral(" 주문별 정정 횟수는 거래소가 최종 검증합니다.");
    return result;
}
} // namespace keefi
