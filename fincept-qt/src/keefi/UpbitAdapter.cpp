#include "keefi/UpbitAdapter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace keefi {
namespace {
using Fields = QList<QPair<QString, QString>>;
constexpr qint64 SevenDays = 7LL * 24 * 60 * 60 * 1000;
constexpr qsizetype MaxBodyBytes = 8 * 1024 * 1024;
const QString Scope = QStringLiteral("연결한 API Key의 포켓 전체");

bool fail(QString* error, const QString& text) { if (error) *error = text; return false; }
bool symbolValid(const QString& s) {
    static const QRegularExpression pattern("^(KRW|BTC|USDT)-[A-Z0-9]{1,24}$");
    return pattern.match(s).hasMatch();
}
bool identifierValid(const QString& s) {
    static const QRegularExpression pattern("^[A-Za-z0-9_-]{1,64}$");
    return pattern.match(s).hasMatch();
}
// External orders need not use this application's restricted identifier alphabet.
bool externalIdentifierValid(const QString& s) {
    return !s.isEmpty() && s.size() <= 64 && !s.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
bool targetValid(const OrderInput& input) {
    return input.orderId.isEmpty() ? externalIdentifierValid(input.clientId) : identifierValid(input.orderId);
}
bool currencyValid(const QString& s) {
    static const QRegularExpression pattern("^[A-Z0-9]{1,24}$");
    return pattern.match(s).hasMatch();
}
QByteArray header(const HttpResult& response, const QByteArray& name) {
    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) return it.value();
    return {};
}
QString str(const QJsonObject& object, const char* key) { return object.value(QLatin1String(key)).toString(); }
bool amount(const QJsonObject& o, const char* k, QString* out, bool optional = false) {
    const auto value = o.value(QLatin1String(k));
    if (optional && (value.isUndefined() || value.isNull())) { out->clear(); return true; }
    if (!value.isString() || !decimal::valid(value.toString())) return false;
    *out = value.toString();
    return true;
}
bool strings(const QJsonValue& value, QStringList* out) {
    if (!value.isArray()) return false;
    for (const auto& v : value.toArray()) {
        if (!v.isString() || v.toString().isEmpty()) return false;
        out->append(v.toString());
    }
    return true;
}
bool volumeValid(const QString& value) {
    if (!decimal::positive(value)) return false;
    const QString normalized = decimal::normalize(value);
    const qsizetype dot = normalized.indexOf('.');
    return dot < 0 || normalized.size() - dot - 1 <= 8;
}
QString typeFor(const OrderInput& input) {
    return input.type == "limit" ? QStringLiteral("limit") :
        input.side == "buy" ? QStringLiteral("price") : QStringLiteral("market");
}
bool orderShape(const OrderInput& input, QString* error) {
    if (!symbolValid(input.symbol) || (input.side != "buy" && input.side != "sell") ||
        (input.type != "limit" && input.type != "market"))
        return fail(error, QStringLiteral("Upbit 현물 지정가·시장가 주문만 지원합니다."));
    if (input.type == "limit") {
        if (!decimal::positive(input.price) || !volumeValid(input.quantity) || !input.quoteAmount.isEmpty())
            return fail(error, QStringLiteral("지정가와 수량을 확인해 주세요. 수량은 소수점 8자리 이하여야 합니다."));
    } else if (input.side == "buy") {
        if (!decimal::positive(input.quoteAmount) || !input.quantity.isEmpty() || !input.price.isEmpty())
            return fail(error, QStringLiteral("시장가 매수는 수량·단가 대신 결제 통화 기준 총액을 입력해 주세요."));
    } else if (!volumeValid(input.quantity) || !input.price.isEmpty() || !input.quoteAmount.isEmpty()) {
        return fail(error, QStringLiteral("시장가 매도는 단가·총액 대신 수량만 입력해 주세요."));
    }
    return true;
}
QByteArray query(const Fields& fields, bool encoded) {
    QByteArray result;
    for (const auto& [key, value] : fields) {
        if (!result.isEmpty()) result += '&';
        result += encoded ? QUrl::toPercentEncoding(key, "[]") : key.toUtf8();
        result += '=';
        result += encoded ? QUrl::toPercentEncoding(value) : value.toUtf8();
    }
    return result;
}
QByteArray quoted(const QByteArray& utf8) {
    QByteArray result("\"");
    for (const unsigned char c : utf8) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 0x20) {
            result += "\\u00";
            result += "0123456789abcdef"[c >> 4];
            result += "0123456789abcdef"[c & 15];
        } else result += char(c);
    }
    result += '"';
    return result;
}
QByteArray body(const Fields& fields) {
    QByteArray result("{");
    for (const auto& [key, value] : fields) {
        if (result.size() != 1) result += ',';
        result += quoted(key.toUtf8()) + ':' + quoted(value.toUtf8());
    }
    return result + '}';
}
struct WipedBytes {
    QByteArray bytes;
    ~WipedBytes() { if (!bytes.isEmpty()) sodium_memzero(bytes.data(), static_cast<size_t>(bytes.size())); }
};
QByteArray authorization(const CredentialSet& credentials, const QByteArray& hashInput) {
    const auto access = credentials.find(QStringLiteral("access_key"));
    const auto secret = credentials.find(QStringLiteral("secret"));
    if (access == credentials.end() || secret == credentials.end() || access->second.empty() || secret->second.empty())
        throw std::runtime_error("MISSING_CREDENTIALS");
    if (access->second.size() > 8192 || secret->second.size() > 8192)
        throw std::runtime_error("INVALID_CREDENTIALS");
    WipedBytes accessBytes{QByteArray(reinterpret_cast<const char*>(access->second.data()), access->second.size())};
    WipedBytes escaped{quoted(accessBytes.bytes)};
    WipedBytes payload;
    payload.bytes = "{\"access_key\":" + escaped.bytes + ",\"nonce\":\"" +
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1() + '"';
    if (!hashInput.isEmpty())
        payload.bytes += ",\"query_hash\":\"" + QCryptographicHash::hash(hashInput, QCryptographicHash::Sha512).toHex() +
            "\",\"query_hash_alg\":\"SHA512\"";
    payload.bytes += '}';
    const auto encoding = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    WipedBytes signing;
    signing.bytes = QByteArray("{\"alg\":\"HS512\",\"typ\":\"JWT\"}").toBase64(encoding) + '.' + payload.bytes.toBase64(encoding);
    SecureBuffer stateStorage(sizeof(crypto_auth_hmacsha512_state)), digest(crypto_auth_hmacsha512_BYTES);
    auto* state = reinterpret_cast<crypto_auth_hmacsha512_state*>(stateStorage.data());
    if (crypto_auth_hmacsha512_init(state, secret->second.data(), secret->second.size()) ||
        crypto_auth_hmacsha512_update(state, reinterpret_cast<const unsigned char*>(signing.bytes.constData()), signing.bytes.size()) ||
        crypto_auth_hmacsha512_final(state, digest.data())) throw std::runtime_error("SIGNING_FAILED");
    WipedBytes signature{QByteArray(reinterpret_cast<const char*>(digest.data()), digest.size()).toBase64(encoding)};
    return "Bearer " + signing.bytes + '.' + signature.bytes;
}
struct Window { qint64 start = 0, end = 0; };
bool historyWindows(const OrderInput& input, QList<Window>* windows, QString* error) {
    if (input.startTimeMs <= 0 || input.endTimeMs < input.startTimeMs ||
        input.endTimeMs - input.startTimeMs > SevenDays)
        return fail(error, QStringLiteral("종료 주문 조회는 시작·종료 시각이 고정된 최대 7일 구간이어야 합니다."));
    if (input.cursor.isEmpty()) { windows->append({input.startTimeMs, input.endTimeMs}); return true; }
    if (input.cursor.size() > 8192) return fail(error, QStringLiteral("종료 주문 조회 위치가 올바르지 않습니다."));
    const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(input.cursor.toLatin1(), QByteArray::Base64UrlEncoding));
    if (!document.isArray() || document.array().isEmpty() || document.array().size() > 64)
        return fail(error, QStringLiteral("종료 주문 조회 위치가 올바르지 않습니다."));
    for (const auto& item : document.array()) {
        const auto pair = item.toArray();
        if (pair.size() != 2 || !pair[0].isString() || !pair[1].isString())
            return fail(error, QStringLiteral("종료 주문 조회 위치가 올바르지 않습니다."));
        bool first = false, second = false;
        Window w{pair[0].toString().toLongLong(&first), pair[1].toString().toLongLong(&second)};
        if (!first || !second || w.start < input.startTimeMs || w.end > input.endTimeMs || w.end < w.start)
            return fail(error, QStringLiteral("종료 주문 조회 위치가 올바르지 않습니다."));
        windows->append(w);
    }
    return true;
}
QString cursorFor(const QList<Window>& windows) {
    QJsonArray values;
    for (const auto& w : windows) values.append(QJsonArray{QString::number(w.start), QString::number(w.end)});
    return QString::fromLatin1(QJsonDocument(values).toJson(QJsonDocument::Compact)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
bool orderRecord(const QJsonObject& object, OrderRecord* out) {
    out->orderId = str(object, "uuid"); out->clientId = str(object, "identifier");
    out->symbol = str(object, "market");
    const auto side = str(object, "side"), type = str(object, "ord_type"), state = str(object, "state");
    if (!identifierValid(out->orderId) || (!out->clientId.isEmpty() && !externalIdentifierValid(out->clientId)) ||
        !symbolValid(out->symbol) || (side != "bid" && side != "ask") ||
        !QStringList{"limit", "price", "market", "best"}.contains(type) ||
        !QStringList{"wait", "watch", "done", "cancel"}.contains(state)) return false;
    out->side = side == "bid" ? "buy" : "sell";
    out->type = type == "limit" ? "limit" : type == "best" ? "best" : "market";
    if (!amount(object, "volume", &out->quantity, true) ||
        !amount(object, "executed_volume", &out->executed) || !amount(object, "remaining_volume", &out->remaining)) return false;
    QString price;
    if (!amount(object, "price", &price, true)) return false;
    if (type == "price") out->quoteAmount = price;
    else out->price = price;
    out->status = state == "done" ? "filled" : state == "cancel" ? "canceled" :
        decimal::positive(out->executed) ? "partial" : "open";
    out->cancelable = state == "wait" || state == "watch";
    out->amendable = state == "wait" && type == "limit";
    return true;
}
AdapterResult invalid(Operation operation, const QString& code = "INVALID_RESPONSE") {
    AdapterResult result;
    result.code = code;
    result.unknown = isMutation(operation);
    result.needsReconciliation = result.unknown;
    result.message = result.unknown ? QStringLiteral("주문 처리 결과를 확인할 수 없습니다. 재전송하지 않고 주문 조회로 확인합니다.") :
                                     QStringLiteral("Upbit 응답 형식을 확인할 수 없습니다.");
    result.scope = Scope;
    return result;
}
QString priceTick(const QString& quote, const QString& price) {
    if (quote == "BTC") return "0.00000001";
    const QList<QPair<QString, QString>> krw{{"1000000", "1000"}, {"500000", "500"}, {"100000", "100"},
        {"50000", "50"}, {"10000", "10"}, {"5000", "5"}, {"100", "1"}, {"10", "0.1"},
        {"1", "0.01"}, {"0.1", "0.001"}, {"0.01", "0.0001"}, {"0.001", "0.00001"},
        {"0.0001", "0.000001"}, {"0.00001", "0.0000001"}};
    const QList<QPair<QString, QString>> usdt{{"10", "0.01"}, {"1", "0.001"}, {"0.1", "0.0001"},
        {"0.01", "0.00001"}, {"0.001", "0.000001"}, {"0.0001", "0.0000001"}};
    for (const auto& [threshold, step] : quote == "KRW" ? krw : usdt)
        if (decimal::compare(price, threshold) >= 0) return step;
    return "0.00000001";
}
bool withinDeviation(const QString& price, const QString& best, const QString& percent) {
    const QString difference = decimal::compare(price, best) >= 0 ? decimal::subtract(price, best) : decimal::subtract(best, price);
    const QString actual = decimal::multiply(difference, "100"), allowed = decimal::multiply(best, percent);
    return decimal::valid(actual) && decimal::valid(allowed) && decimal::compare(actual, allowed) <= 0;
}
} // namespace

QString UpbitAdapter::provider() const { return QStringLiteral("upbit"); }
QByteArray upbitConnectionAuthorization(const CredentialSet& credentials) { return authorization(credentials, {}); }
QByteArray upbitQueryAuthorization(const CredentialSet& credentials, const QByteArray& unencodedQuery) {
    return authorization(credentials, unencodedQuery);
}
QString UpbitAdapter::amendDescription() const { return QStringLiteral("기존 주문 취소 후 재주문 — 주문 번호가 바뀌며 체결 경합 시 새 주문이 생성되지 않을 수 있습니다."); }
QList<Operation> UpbitAdapter::refreshPlan() const {
    return {Operation::Markets, Operation::Reference, Operation::Rules, Operation::Quote,
        Operation::Account, Operation::OpenOrders, Operation::History};
}

RequestSpec UpbitAdapter::build(Operation operation, const AccountRecord& account, const OrderInput& input,
    const CredentialSet& credentials, qint64 serverNowMs, QString* error) const {
    Q_UNUSED(serverNowMs)
    if (error) error->clear();
    RequestSpec result; result.operation = operation;
    const auto bad = [&](const QString& message) { fail(error, message); return RequestSpec{}; };
    if (account.provider != "upbit" || account.environment != "live" || account.market != "spot")
        return bad(QStringLiteral("Upbit 한국 서비스 현물 연결만 사용할 수 있습니다."));
    Fields fields;
    QString path;
    switch (operation) {
    case Operation::Markets: path = "/v1/market/all"; fields = {{"is_details", "true"}}; break;
    case Operation::Reference: path = "/v1/orderbook/instruments"; fields = {{"markets", input.symbol}}; break;
    case Operation::Quote: path = "/v1/orderbook"; fields = {{"markets", input.symbol}}; break;
    case Operation::Rules: path = "/v1/orders/chance"; fields = {{"market", input.symbol}}; result.authenticated = true; break;
    case Operation::Account: path = "/v1/accounts"; result.authenticated = true; break;
    case Operation::OpenOrders: {
        bool valid = input.cursor.isEmpty();
        const int page = input.cursor.isEmpty() ? 1 : input.cursor.toInt(&valid);
        if (!valid || page < 1 || page > 10000) return bad(QStringLiteral("미체결 주문 페이지가 올바르지 않습니다."));
        path = "/v1/orders/open"; result.authenticated = true;
        fields = {{"states[]", "wait"}, {"states[]", "watch"}, {"page", QString::number(page)}, {"limit", "100"}, {"order_by", "asc"}};
        break;
    }
    case Operation::History: {
        QList<Window> windows;
        if (!historyWindows(input, &windows, error)) return {};
        path = "/v1/orders/closed"; result.authenticated = true;
        fields = {{"states[]", "done"}, {"states[]", "cancel"}, {"start_time", QString::number(windows.first().start)},
            {"end_time", QString::number(windows.first().end)}, {"limit", "1000"}, {"order_by", "asc"}};
        break;
    }
    case Operation::QueryOrder:

        if (!targetValid(input))
            return bad(QStringLiteral("조회·취소 대상 주문 번호가 필요합니다."));
        path = "/v1/order"; result.authenticated = true;
        fields = !input.orderId.isEmpty() ? Fields{{"uuid", input.orderId}} : Fields{{"identifier", input.clientId}};

        break;
    default: return bad(QStringLiteral("이 Upbit API 동작은 지원하지 않습니다."));
    }
    if ((operation == Operation::Reference || operation == Operation::Quote || operation == Operation::Rules) &&
        !symbolValid(input.symbol)) return bad(QStringLiteral("Upbit 거래쌍 형식이 올바르지 않습니다."));
    result.url = QUrl("https://api.upbit.com" + path);
    result.headers.insert("Accept", "application/json");
    if (result.method == "POST") {
        result.body = body(fields);
        result.headers.insert("Content-Type", "application/json");
    } else if (!fields.isEmpty()) result.url.setQuery(QString::fromLatin1(query(fields, true)), QUrl::StrictMode);
    try {
        if (result.authenticated) result.headers.insert("Authorization", authorization(credentials, query(fields, false)));
    } catch (const std::exception&) {
        return bad(QStringLiteral("보호된 API 인증정보로 요청에 서명하지 못했습니다."));
    }
    return result;
}

AdapterResult UpbitAdapter::parse(Operation operation, const HttpResult& response, const OrderInput& input) const {
    if (response.timedOut || (response.networkError && response.status == 0)) return invalid(operation, "TRANSPORT_ERROR");
    if (response.body.size() > MaxBodyBytes) return invalid(operation, "RESPONSE_TOO_LARGE");
    QString parseError;
    const QJsonDocument document = parseExactJson(response.body, &parseError);
    const QJsonObject object = document.object();
    AdapterResult result;
    result.scope = Scope;
    result.serverTimeMs = QDateTime::fromString(QString::fromLatin1(header(response, "Date")), Qt::RFC2822Date).toMSecsSinceEpoch();
    if (response.status < 200 || response.status >= 300) {
        if (response.status >= 500 || response.status == 0) return invalid(operation, "SERVER_ERROR");
        const QString name = str(object.value("error").toObject(), "name");
        if ((!parseError.isEmpty() || name.isEmpty()) && response.status != 429 && response.status != 418)
            return invalid(operation);
        result.code = "EXCHANGE_REJECTED";
        result.message = QStringLiteral("Upbit가 요청을 거부했습니다. 주문 조건과 연결 상태를 확인해 주세요.");
        if (response.status == 429 || response.status == 418) {
            result.code = response.status == 418 ? "IP_OR_POCKET_BLOCKED" : "RATE_LIMITED";
            result.rateLimited = true;
            bool valid = false;
            const int seconds = header(response, "Retry-After").toInt(&valid);
            result.retryAfterSeconds = valid && seconds > 0 ? seconds : response.status == 429 ? 1 : 0;
            result.message = QStringLiteral("Upbit 요청 제한으로 중지했습니다. 주문을 자동 재전송하지 않습니다.");
        } else if (name == "out_of_scope") {
            result.code = "PERMISSION_DENIED";
            result.message = QStringLiteral("이 API 작업에 필요한 권한이 없습니다. 조회 성공은 거래 권한 검증이 아닙니다.");
            result.tradingPermissionKnown = false;
        } else if (name == "order_not_found" && operation == Operation::QueryOrder) {
            result.code = "ORDER_NOT_FOUND"; result.notFound = true;
            result.message = QStringLiteral("해당 주문이 아직 조회되지 않습니다. 미접수로 단정하지 않습니다.");
        } else if (response.status == 401 || name == "expired_access_key" || name == "no_authorization_ip") {
            result.authenticationFailed = true;
            result.code = name == "expired_access_key" ? "KEY_EXPIRED" : name == "no_authorization_ip" ? "IP_NOT_ALLOWED" : "AUTHENTICATION_FAILED";
            result.message = QStringLiteral("API 인증·키 만료·허용 IP 설정을 확인해 주세요.");
        } else if (name == "insufficient_funds_bid" || name == "insufficient_funds_ask") {
            result.code = "INSUFFICIENT_BALANCE"; result.message = QStringLiteral("수수료를 포함한 주문 가능 잔고가 부족합니다.");
        } else if (name == "under_min_total_bid" || name == "under_min_total_ask") {
            result.code = "MIN_NOTIONAL"; result.message = QStringLiteral("최소 주문 금액에 미달합니다.");
        } else if (name == "duplicated_identifier") {
            result.code = "DUPLICATE_IDENTIFIER"; result.needsReconciliation = true;
            result.message = QStringLiteral("이미 사용한 주문 식별자입니다. 새 주문을 만들지 않고 기존 주문을 조회합니다.");
        } else if (name == "market_offline") {
            result.code = "MARKET_OFFLINE"; result.message = QStringLiteral("해당 거래쌍은 현재 주문할 수 없습니다.");
        }
        // A replacement error does not by itself prove the original order stayed
        // open. Reconcile both identifiers before clearing the durable journal.

        return result;
    }
    if (!parseError.isEmpty() || document.isNull()) return invalid(operation);
    switch (operation) {
    case Operation::Markets: {
        if (!document.isArray() || document.array().isEmpty() || document.array().size() > 20000) return invalid(operation);
        QJsonArray markets, instruments;
        QSet<QString> seen;
        for (const auto& item : document.array()) {
            const auto market = item.toObject(); const QString symbol = str(market, "market");
            if (!symbolValid(symbol) || seen.contains(symbol)) return invalid(operation);
            seen.insert(symbol); markets.append(symbol);
            for (const auto& field : {"korean_name", "english_name"}) {
                if (market.contains(field) && (!market.value(field).isString() || str(market, field).size() > 256)) return invalid(operation);
                for (const auto c : str(market, field))
                    if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format) return invalid(operation);
            }
            QStringList warnings;
            if (market.contains("market_warning") && !market.value("market_warning").isString()) return invalid(operation);
            if (str(market, "market_warning") == "CAUTION") warnings.append("거래 유의");
            if (market.contains("market_event")) {
                if (!market.value("market_event").isObject()) return invalid(operation);
                const auto event = market.value("market_event").toObject();
                if (event.contains("warning") && !event.value("warning").isBool()) return invalid(operation);
                if (event.value("warning").toBool() && !warnings.contains("거래 유의")) warnings.append("거래 유의");
                if (event.contains("caution")) {
                    if (!event.value("caution").isObject()) return invalid(operation);
                    const auto cautions = event.value("caution").toObject();
                    bool caution = false;
                    for (auto it = cautions.constBegin(); it != cautions.constEnd(); ++it) {
                        if (!it.value().isBool()) return invalid(operation);
                        caution = caution || it.value().toBool();
                    }
                    if (caution) warnings.append("거래 주의");
                }
            }
            instruments.append(QJsonObject{{"symbol", symbol}, {"baseAsset", symbol.section('-', 1, 1)},
                {"quoteAsset", symbol.section('-', 0, 0)}, {"name", str(market, "korean_name")},
                {"englishName", str(market, "english_name")}, {"warning", warnings.join(" · ")}});
        }
        result.data.insert("symbols", markets);
        result.data.insert("instruments", instruments); break;
    }
    case Operation::Reference: {
        if (!document.isArray() || document.array().size() != 1) return invalid(operation);
        const auto value = document.array().first().toObject(); QString tick;
        if (str(value, "market") != input.symbol || str(value, "quote_currency") != input.symbol.section('-', 0, 0) ||
            !amount(value, "tick_size", &tick) || !decimal::positive(tick)) return invalid(operation);
        result.data = {{"symbol", input.symbol}, {"tickSize", tick}}; break;
    }
    case Operation::Rules: {
        const auto market = object.value("market").toObject();
        const auto bid = market.value("bid").toObject(), ask = market.value("ask").toObject();
        QString bidMin, askMin, maximum, bidFee, askFee;
        QStringList bidTypes, askTypes, sides;
        if (str(market, "id") != input.symbol || str(bid, "currency") != input.symbol.section('-', 0, 0) ||
            str(ask, "currency") != input.symbol.section('-', 1, 1) || !amount(bid, "min_total", &bidMin) ||
            !amount(ask, "min_total", &askMin) || !amount(market, "max_total", &maximum) ||
            !amount(object, "bid_fee", &bidFee) || !amount(object, "ask_fee", &askFee) ||
            !strings(market.value("bid_types"), &bidTypes) || !strings(market.value("ask_types"), &askTypes) ||
            !strings(market.value("order_sides"), &sides)) return invalid(operation);
        result.data = {{"symbol", input.symbol}, {"state", str(market, "state")}, {"quoteAsset", str(bid, "currency")},
            {"baseAsset", str(ask, "currency")}, {"minBuyNotional", bidMin}, {"minSellNotional", askMin},
            {"maxNotional", maximum}, {"buyFee", bidFee}, {"sellFee", askFee},
            {"buyTypes", QJsonArray::fromStringList(bidTypes)}, {"sellTypes", QJsonArray::fromStringList(askTypes)},
            {"sides", QJsonArray::fromStringList(sides)}};
        break;
    }
    case Operation::Quote: {
        if (!document.isArray() || document.array().size() != 1) return invalid(operation);
        const auto value = document.array().first().toObject();
        const auto units = value.value("orderbook_units").toArray();
        QString bid, ask;
        if (str(value, "market") != input.symbol || units.isEmpty() ||
            !amount(units.first().toObject(), "bid_price", &bid) || !decimal::positive(bid) ||
            !amount(units.first().toObject(), "ask_price", &ask) || !decimal::positive(ask) ||
            decimal::compare(ask, bid) < 0) return invalid(operation);
        QJsonArray levels;
        QString previousBid, previousAsk;
        for (const auto& unit : units) {
            QString bp, ap, bs, as;
            const auto level = unit.toObject();
            if (!amount(level, "bid_price", &bp) || !decimal::positive(bp) ||
                !amount(level, "ask_price", &ap) || !decimal::positive(ap) ||
                !amount(level, "bid_size", &bs) || !decimal::valid(bs) ||
                !amount(level, "ask_size", &as) || !decimal::valid(as) ||
                decimal::compare(ap, bp) < 0 ||
                (!previousBid.isEmpty() && (decimal::compare(bp, previousBid) > 0 || decimal::compare(ap, previousAsk) < 0)))
                return invalid(operation);
            levels.append(QJsonObject{{"bidPrice", bp}, {"askPrice", ap}, {"bidSize", bs}, {"askSize", as}});
            previousBid = bp; previousAsk = ap;
        }
        result.data = {{"symbol", input.symbol}, {"bid", bid}, {"ask", ask}, {"timestamp", str(value, "timestamp")}, {"levels", levels}};
        break;
    }
    case Operation::Account: {
        if (!document.isArray()) return invalid(operation);
        QSet<QString> assets;
        static const QRegularExpression averageCurrencyPattern("\\A[A-Z0-9]{1,24}\\z");
        for (const auto& item : document.array()) {
            const auto value = item.toObject(); Balance balance;
            balance.asset = str(value, "currency");
            if (!currencyValid(balance.asset) || assets.contains(balance.asset) ||
                !amount(value, "balance", &balance.free) || !amount(value, "locked", &balance.locked)) return invalid(operation);
            const auto average = str(value, "avg_buy_price"), currency = str(value, "unit_currency");
            if (average == average.trimmed() && decimal::positive(average) && averageCurrencyPattern.match(currency).hasMatch()) {
                balance.providerAveragePrice = decimal::normalize(average);
                balance.providerAverageCurrency = currency;
            }
            assets.insert(balance.asset); result.balances.append(balance);
        }
        result.data.insert("scope", "api_key_pocket"); break;
    }
    case Operation::OpenOrders:
    case Operation::History: {
        const int limit = operation == Operation::OpenOrders ? 100 : 1000;
        if (!document.isArray() || document.array().size() > limit) return invalid(operation);
        for (const auto& item : document.array()) {
            OrderRecord order;
            if (!item.isObject() || !orderRecord(item.toObject(), &order)) return invalid(operation);
            result.orders.append(order);
        }
        if (operation == Operation::OpenOrders && result.orders.size() == limit) {
            bool valid = input.cursor.isEmpty();
            const int page = input.cursor.isEmpty() ? 1 : input.cursor.toInt(&valid);
            if (!valid || page < 1 || page >= 10000) return invalid(operation, "PAGINATION_LIMIT");
            result.hasMore = true; result.nextCursor = QString::number(page + 1);
        } else if (operation == Operation::History) {
            QList<Window> windows;
            if (!historyWindows(input, &windows, nullptr)) return invalid(operation, "INVALID_HISTORY_WINDOW");
            const Window current = windows.takeFirst();
            if (result.orders.size() == limit) {
                if (current.end - current.start <= 1 || windows.size() > 61) {
                    result.code = "HISTORY_INCOMPLETE";
                    result.message = QStringLiteral("종료 주문 조회 한도로 일부 내역을 확인하지 못했습니다.");
                    result.data.insert("complete", false);
                    result.scope += QStringLiteral(" · 최근 7일 일부");
                } else {
                    const qint64 middle = current.start + (current.end - current.start) / 2;
                    windows.prepend({middle, current.end}); windows.prepend({current.start, middle});
                }
            }
            result.hasMore = !windows.isEmpty();
            if (result.hasMore) result.nextCursor = cursorFor(windows);
        }
        break;
    }
    case Operation::QueryOrder:

     {
        OrderRecord order;
        if (!document.isObject() || !orderRecord(object, &order)) return invalid(operation);
         if (isMutation(operation)) result.needsReconciliation = true;
        result.orders.append(order);
        break;
    }
    default: return invalid(operation, "UNSUPPORTED_OPERATION");
    }
    result.ok = true;
    return result;
}

OrderValidation UpbitAdapter::validate(const OrderInput& input, const MarketSnapshot& snapshot,
    const OrderLimits& limits) const {
    OrderValidation result;
    const auto bad = [&](const QString& message) { result.message = message; return result; };
    if (!orderShape(input, &result.message)) return result;
    const auto rules = snapshot.data.value(Operation::Rules), quote = snapshot.data.value(Operation::Quote),
               reference = snapshot.data.value(Operation::Reference), markets = snapshot.data.value(Operation::Markets);
    if (str(rules, "symbol") != input.symbol || str(quote, "symbol") != input.symbol ||
        str(reference, "symbol") != input.symbol || !markets.value("symbols").toArray().contains(input.symbol))
        return bad(QStringLiteral("선택한 거래쌍의 최신 시장·호가·주문 규칙이 필요합니다."));
    if (str(rules, "state") != "active") return bad(QStringLiteral("해당 거래쌍은 현재 거래 가능한 상태가 아닙니다."));
    const QString quoteAsset = input.symbol.section('-', 0, 0), baseAsset = input.symbol.section('-', 1, 1);
    if (str(rules, "quoteAsset") != quoteAsset || str(rules, "baseAsset") != baseAsset)
        return bad(QStringLiteral("주문 규칙의 기준 통화가 일치하지 않습니다."));
    const bool buy = input.side == "buy";
    if (!rules.value("sides").toArray().contains(buy ? "bid" : "ask") ||
        !rules.value(buy ? "buyTypes" : "sellTypes").toArray().contains(typeFor(input)))
        return bad(QStringLiteral("해당 거래쌍이 선택한 주문 유형·방향을 지원하지 않습니다."));
    const QString best = str(quote, buy ? "ask" : "bid"), tick = str(reference, "tickSize");
    if (!decimal::positive(best) || !decimal::positive(tick)) return bad(QStringLiteral("유효한 현재 호가와 가격 단위가 필요합니다."));
    if (!decimal::positive(limits.maxNotional) || !decimal::valid(limits.maxDeviationPercent) || limits.currency != quoteAsset)
        return bad(QStringLiteral("연결의 최대 주문 금액·가격 괴리율을 결제 통화 기준으로 먼저 설정해 주세요."));
    if (input.type == "limit") {
        const QString expectedTick = priceTick(quoteAsset, input.price);
        const QString currentTick = priceTick(quoteAsset, best);
        // A new exchange policy must not silently be replaced by a stale local table.
        if (decimal::compare(currentTick, tick) != 0)
            return bad(QStringLiteral("공식 호가 단위가 로컬 가격 구간 정책과 다릅니다. 정책 갱신이 필요합니다."));
        if (!decimal::multipleOf(input.price, expectedTick)) return bad(QStringLiteral("입력한 지정가가 해당 가격 구간의 호가 단위와 맞지 않습니다."));
        if (!withinDeviation(input.price, best, limits.maxDeviationPercent))
            return bad(QStringLiteral("지정가가 설정한 현재 호가 대비 가격 괴리율을 초과했습니다."));
    }
    result.currency = quoteAsset;
    result.notional = input.type == "market" && buy ? input.quoteAmount :
        decimal::multiply(input.type == "limit" ? input.price : best, input.quantity);
    if (input.type == "market") {
        const auto levels = quote.value("levels").toArray();
        QString remaining = buy ? input.quoteAmount : input.quantity;
        QString proceeds = "0";
        for (const auto& item : levels) {
            if (!decimal::positive(remaining)) break;
            const auto level = item.toObject();
            const QString price = str(level, buy ? "askPrice" : "bidPrice"), size = str(level, buy ? "askSize" : "bidSize");
            if (!decimal::positive(price) || !decimal::valid(size))
                return bad(QStringLiteral("시장가 주문의 호가 잔량을 확인하지 못했습니다."));
            if (!decimal::positive(size)) continue;
            if (!withinDeviation(price, best, limits.maxDeviationPercent))
                return bad(QStringLiteral("시장가 주문이 설정한 가격 괴리율 밖의 호가 잔량을 사용합니다."));
            const QString available = buy ? decimal::multiply(price, size) : size;
            if (!decimal::positive(available)) return bad(QStringLiteral("호가 잔량 금액의 정밀도 범위를 초과했습니다."));
            const QString consumed = decimal::compare(available, remaining) >= 0 ? remaining : available;
            if (!buy) proceeds = decimal::add(proceeds, decimal::multiply(consumed, price));
            remaining = decimal::subtract(remaining, consumed);
        }
        if (decimal::positive(remaining))
            return bad(QStringLiteral("조회한 호가 잔량으로 시장가 주문 전체의 가격 범위를 확인할 수 없습니다."));
        if (!buy) result.notional = proceeds;
    }
    const QString minimum = str(rules, buy ? "minBuyNotional" : "minSellNotional"), maximum = str(rules, "maxNotional"),
                  fee = str(rules, buy ? "buyFee" : "sellFee");
    if (!decimal::positive(result.notional) || !decimal::valid(minimum) || !decimal::positive(maximum) || !decimal::valid(fee))
        return bad(QStringLiteral("주문 금액·수수료·최소 및 최대 금액 조건을 확인하지 못했습니다."));
    if (decimal::compare(result.notional, minimum) < 0) return bad(QStringLiteral("최소 주문 금액에 미달합니다."));
    if (decimal::compare(result.notional, maximum) > 0 || decimal::compare(result.notional, limits.maxNotional) > 0)
        return bad(QStringLiteral("거래소 또는 사용자 최대 주문 금액을 초과했습니다."));
    const QString neededAsset = buy ? quoteAsset : baseAsset;
    const QString needed = buy ? decimal::multiply(result.notional, decimal::add("1", fee)) : input.quantity;
    if (!decimal::positive(needed)) return bad(QStringLiteral("수수료 포함 주문 금액의 정밀도 범위를 초과했습니다."));
    auto balance = std::find_if(snapshot.balances.cbegin(), snapshot.balances.cend(), [&](const Balance& b) { return b.asset == neededAsset; });
    if (balance == snapshot.balances.cend() || !decimal::valid(balance->free) || !decimal::valid(balance->locked))
        return bad(QStringLiteral("주문 통화의 잔고를 확인하지 못했습니다."));
    QString available = balance->free;
    if (!input.orderId.isEmpty() || !input.newClientId.isEmpty()) {
        const auto original = std::find_if(snapshot.openOrders.cbegin(), snapshot.openOrders.cend(), [&](const OrderRecord& order) {
            return !input.orderId.isEmpty() ? order.orderId == input.orderId : order.clientId == input.clientId;
        });
        if (original == snapshot.openOrders.cend() || !original->amendable || original->symbol != input.symbol ||
            original->side != input.side || original->type != "limit" || !decimal::positive(original->remaining) ||
            !decimal::positive(original->price))
            return bad(QStringLiteral("정정 대상의 최신 미체결 주문·거래쌍·방향을 다시 확인해 주세요."));
        // Only this order's unfilled principal may be released. Do not borrow
        // other locked funds or estimate the original order's refundable fee.
        const QString release = buy ? decimal::multiply(original->remaining, original->price) : original->remaining;
        if (!decimal::valid(release) || decimal::compare(release, balance->locked) > 0)
            return bad(QStringLiteral("정정 대상의 미체결 금액과 잠금 잔고가 일치하지 않습니다. 잔고를 갱신해 주세요."));
        available = decimal::add(available, release);
    }
    if (!decimal::valid(available) || decimal::compare(available, needed) < 0)
        return bad(QStringLiteral("수수료를 포함한 주문 가능 잔고가 부족합니다. 다른 주문의 잠금 잔고는 사용하지 않습니다."));
    result.ok = true;
    return result;
}
} // namespace keefi
