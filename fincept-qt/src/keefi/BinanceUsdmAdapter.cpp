#include "keefi/BinanceUsdmAdapter.h"
#include "keefi/BinanceTiming.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <sodium.h>

namespace keefi {
namespace {
using Params = QList<QPair<QByteArray, QString>>;
constexpr qint64 Day = 86400000;
QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
bool num(const QString& s) { return s.size() <= 80 && decimal::valid(s); }
bool pos(const QString& s) { return num(s) && decimal::positive(s); }
QString absolute(QString s) { if (s.startsWith('-')) s.remove(0, 1); return s; }
bool signedNum(const QString& s) { return !s.isEmpty() && num(absolute(s)); }
bool integer(const QString& s) { static const QRegularExpression r(QStringLiteral("\\A[0-9]{1,19}\\z")); return r.match(s).hasMatch(); }
bool id(const QString& s) { return integer(s) && pos(s) && decimal::compare(s, "9223372036854775807") <= 0; }
bool stamp(const QString& s) { bool ok = false; const auto v = s.toLongLong(&ok); return integer(s) && ok && v > 0; }
bool client(const QString& s) { static const QRegularExpression r(QStringLiteral("\\A[.A-Za-z0-9:/_-]{1,36}\\z")); return r.match(s).hasMatch(); }
bool symbol(const QString& s) { return binanceUsdmSymbolValid(s); }
bool positionSide(const QString& s) { return s == "BOTH" || s == "LONG" || s == "SHORT"; }
bool side(const QString& s) { return s == "buy" || s == "sell"; }
bool secretValid(const SecureBuffer& s) {
    if (s.empty() || s.size() > 512) return false;
    for (std::size_t k = 0; k < s.size(); ++k) { auto c = s.data()[k]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false; }
    return true;
}
QByteArray encode(const Params& p) { QByteArray b; for (const auto& e : p) { if (!b.isEmpty()) b += '&'; b += e.first + '=' + QUrl::toPercentEncoding(e.second); } return b; }
bool same(const QString& a, const QString& b) { return num(a) && num(b) && decimal::compare(a, b) == 0; }
bool fresh(const QString& time, qint64 now, qint64 age = 10000) {
    if (!stamp(time) || now <= 0) return false;
    const qint64 t = time.toLongLong(); return t <= now + 2000 && now - t <= age;
}
QJsonObject only(const QJsonObject& in, std::initializer_list<const char*> keys) {
    QJsonObject out; for (const char* k : keys) if (in.contains(QLatin1String(k))) out.insert(QLatin1String(k), in.value(QLatin1String(k))); return out;
}
AdapterResult fail(Operation op, const QString& code, const QString& message, bool ambiguous = false) {
    AdapterResult r; r.code = code; r.message = message; r.unknown = isMutation(op) && ambiguous; r.needsReconciliation = r.unknown; return r;
}
bool ordinary(const QJsonObject& o, OrderRecord* r) {
    r->orderId = str(o, "orderId"); r->clientId = str(o, "clientOrderId"); r->symbol = str(o, "symbol");
    r->side = str(o, "side").toLower(); r->type = str(o, "type").toLower(); r->rawStatus = str(o, "status");
    r->quantity = str(o, "origQty"); r->executed = str(o, "executedQty"); r->price = str(o, "price");
    r->quoteAmount = str(o, "cumQuote"); r->sourceUpdatedAt = str(o, "updateTime");
    if (!id(r->orderId) || !client(r->clientId) || !symbol(r->symbol) || !side(r->side) || r->type.isEmpty() ||
        r->rawStatus.isEmpty() || !num(r->quantity) || !num(r->executed) || !num(r->price) ||
        (!r->quoteAmount.isEmpty() && !num(r->quoteAmount)) || !stamp(r->sourceUpdatedAt) ||
        !o.value("reduceOnly").isBool() || !o.value("closePosition").isBool()) return false;
    FuturesOrderTerms t; t.positionSide = str(o, "positionSide"); t.timeInForce = str(o, "timeInForce");
    t.reduceOnly = o.value("reduceOnly").toBool(); t.closePosition = o.value("closePosition").toBool();
    if (!positionSide(t.positionSide) || t.timeInForce.isEmpty()) return false;
    // Working/trigger defaults have no meaning for ordinary LIMIT/MARKET orders.
    r->futures = t; r->market = "usdm_futures"; r->quantityUnit = "base";
    r->remaining = decimal::subtract(r->quantity, r->executed); if (r->remaining.isEmpty()) return false;
    r->executedBaseAmount = r->executed; r->remainingBaseAmount = r->remaining;
    const QMap<QString, QString> statuses{{"NEW","open"},{"PARTIALLY_FILLED","partial"},{"FILLED","filled"},
        {"CANCELED","canceled"},{"REJECTED","rejected"},{"EXPIRED","expired"},{"EXPIRED_IN_MATCH","expired"}};
    r->status = statuses.value(r->rawStatus, "unknown");
    if (r->type == "market") r->price.clear();
    const bool normal = (r->type == "limit" || r->type == "market") && !t.closePosition && (t.positionSide == "BOTH" || !t.reduceOnly);
    r->cancelable = normal && (r->status == "open" || r->status == "partial");
    r->amendable = r->cancelable && r->type == "limit" && t.timeInForce == "GTC" && pos(r->price);
    return true;
}
bool conditional(const QJsonObject& o, OrderRecord* r) {
    r->algoId = str(o, "algoId"); r->clientAlgoId = str(o, "clientAlgoId"); r->actualOrderId = str(o, "actualOrderId");
    r->symbol = str(o, "symbol"); r->side = str(o, "side").toLower(); r->type = str(o, "orderType").toLower();
    r->rawStatus = str(o, "algoStatus"); r->quantity = str(o, "quantity"); r->price = str(o, "price"); r->sourceUpdatedAt = str(o, "updateTime");
    if (!id(r->algoId) || (!r->clientAlgoId.isEmpty() && !client(r->clientAlgoId)) || (!r->actualOrderId.isEmpty() && !id(r->actualOrderId)) ||
        !symbol(r->symbol) || !side(r->side) || r->type.isEmpty() || r->rawStatus.isEmpty() || !stamp(r->sourceUpdatedAt) ||
        !o.value("reduceOnly").isBool() || !o.value("closePosition").isBool() || str(o,"algoType") != "CONDITIONAL") return false;
    FuturesOrderTerms t; t.family = OrderFamily::Conditional; t.positionSide = str(o, "positionSide"); t.timeInForce = str(o, "timeInForce");
    t.reduceOnly = o.value("reduceOnly").toBool(); t.closePosition = o.value("closePosition").toBool();
    t.workingType = str(o, "workingType"); t.triggerPrice = str(o, "triggerPrice"); t.activationPrice = str(o, "activatePrice"); t.callbackRate = str(o, "callbackRate");
    if (o.contains("priceProtect")) { if (!o.value("priceProtect").isBool()) return false; t.priceProtect = o.value("priceProtect").toBool(); }
    if (!positionSide(t.positionSide) || (!r->quantity.isEmpty() && !num(r->quantity)) || (!r->price.isEmpty() && !num(r->price)) ||
        (!t.triggerPrice.isEmpty() && !num(t.triggerPrice)) || (!t.activationPrice.isEmpty() && !num(t.activationPrice)) ||
        (!t.callbackRate.isEmpty() && !num(t.callbackRate))) return false;
    r->futures = t; r->market = "usdm_futures"; r->quantityUnit = "base";
    const QMap<QString, QString> statuses{{"NEW","open"},{"CANCELED","canceled"},{"REJECTED","rejected"},{"EXPIRED","expired"}};
    // TRIGGERED/FINISHED describe the algo lifecycle, not fills of actualOrderId.
    r->status = statuses.value(r->rawStatus, "unknown");
    const bool stop = r->type == "stop" || r->type == "take_profit";
    const bool stopMarket = r->type == "stop_market" || r->type == "take_profit_market";
    const bool trailing = r->type == "trailing_stop_market";
    const bool quantityKnown = t.closePosition ? (r->quantity.isEmpty() || same(r->quantity, "0")) : pos(r->quantity);
    const bool termsKnown = (stop && pos(r->price) && pos(t.triggerPrice)) || (stopMarket && pos(t.triggerPrice)) ||
        (trailing && pos(t.activationPrice) && pos(t.callbackRate));
    r->cancelable = r->rawStatus == "NEW" && o.value("actualOrderId").isString() && r->actualOrderId.isEmpty() && quantityKnown && termsKnown &&
        (t.workingType == "MARK_PRICE" || t.workingType == "CONTRACT_PRICE") && !(t.closePosition && t.reduceOnly) &&
        (t.positionSide == "BOTH" || !t.reduceOnly);
    return true;
}
QJsonObject findSymbol(const QJsonArray& rows, const QString& s) {
    QJsonObject result; for (const auto& v : rows) if (v.isObject() && str(v.toObject(), "symbol") == s) {
        if (!result.isEmpty()) return {}; result = v.toObject();
    } return result;
}
QJsonObject filter(const QJsonObject& rules, const QString& type) {
    QJsonObject result; for (const auto& v : rules.value("filters").toArray()) if (str(v.toObject(), "filterType") == type) {
        if (!result.isEmpty()) return {}; result = v.toObject();
    } return result;
}
bool stepRange(const QString& value, const QJsonObject& f, const char* lowKey, const char* highKey, const char* stepKey) {
    const auto low = str(f, lowKey), high = str(f, highKey), step = str(f, stepKey);
    if (!num(value) || !num(low) || !num(high) || !num(step)) return false;
    if (pos(low) && decimal::compare(value, low) < 0) return false;
    if (pos(high) && decimal::compare(value, high) > 0) return false;
    // Futures filters explicitly define increments relative to the minimum.
    const auto offset = decimal::subtract(value, low); return !offset.isEmpty() && (!pos(step) || decimal::multipleOf(offset, step));
}
}

BinanceUsdmAdapter::BinanceUsdmAdapter(const AccountRecord& a) : environment_(a.environment) {}
ExchangePolicy BinanceUsdmAdapter::exchangePolicy() const { ExchangePolicy p; p.exactIntentPreflight = true; p.amendModel = ExchangeAmendModel::SameOrder; p.marketBuyUnit = "base"; return p; }
QList<Operation> BinanceUsdmAdapter::refreshPlan() const {
    return {Operation::Time,Operation::AccountConfig,Operation::Account,Operation::Positions,Operation::OpenOrders,Operation::AlgoOpenOrders,Operation::History,Operation::AlgoHistory};
}
QString BinanceUsdmAdapter::amendDescription() const { return QStringLiteral("일반 GTC 지정가만 정정합니다. STOP·TAKE_PROFIT 지정가는 신규·조회·취소를 지원하며 조건부 정정은 지원하지 않습니다."); }
RequestSpec BinanceUsdmAdapter::build(Operation op,const AccountRecord& a,const OrderInput& i,const CredentialSet& credentials,qint64 now,QString* error) const {
    if (error) error->clear(); RequestSpec r; r.operation = op;
    const auto reject = [&](const QString& m) { if (error) *error = m; return RequestSpec{}; };
    if (a.provider != "binance" || a.market != "usdm_futures" || a.environment != environment_ || environment_ != "live")
        return reject(QStringLiteral("Binance USDⓈ-M 실전 환경만 지원합니다."));
    QByteArray origin = "https://fapi.binance.com";
    QByteArray path; Params p;
    auto withSymbol = [&]() { if (!symbol(i.symbol)) return false; p.append(Params::value_type{"symbol",i.symbol}); return true; };
    auto reference = [&]() { if (!i.orderId.isEmpty()) { if (!id(i.orderId)) return false; p.append(Params::value_type{"orderId",i.orderId}); }
        else { if (!client(i.clientId)) return false; p.append(Params::value_type{"origClientOrderId",i.clientId}); } return true; };
    switch (op) {
    case Operation::Time: path = "/fapi/v1/time"; break;
    case Operation::Markets: case Operation::Rules: path = "/fapi/v1/exchangeInfo"; break;
    case Operation::Quote: path = "/fapi/v2/ticker/price"; if (!withSymbol()) return reject("종목을 확인하세요."); break;
    case Operation::Depth: path = "/fapi/v1/depth"; if (!withSymbol()) return reject("종목을 확인하세요."); p.append(Params::value_type{"limit","100"}); break;
    case Operation::MarkPrice: path = "/fapi/v1/premiumIndex"; if (!withSymbol()) return reject("종목을 확인하세요."); break;
    case Operation::AccountConfig: path = "/fapi/v1/accountConfig"; r.authenticated = true; break;
    case Operation::Account: path = "/fapi/v3/account"; r.authenticated = true; break;
    case Operation::Positions: path = "/fapi/v3/positionRisk"; r.authenticated = true; break;
    case Operation::SymbolConfig: path = "/fapi/v1/symbolConfig"; r.authenticated = true; if (!withSymbol()) return reject("종목을 확인하세요."); break;
    case Operation::LeverageBrackets: path = "/fapi/v1/leverageBracket"; r.authenticated = true; if (!withSymbol()) return reject("종목을 확인하세요."); break;
    case Operation::Commissions: path = "/fapi/v1/commissionRate"; r.authenticated = true; if (!withSymbol()) return reject("종목을 확인하세요."); break;
    case Operation::TradingStatus: path = "/fapi/v1/apiTradingStatus"; r.authenticated = true; break;
    case Operation::ApiRestrictions: origin = "https://api.binance.com"; path = "/sapi/v1/account/apiRestrictions"; r.authenticated = true; break;
    case Operation::OpenOrders: path = "/fapi/v1/openOrders"; r.authenticated = true; break;
    case Operation::AlgoOpenOrders: path = "/fapi/v1/openAlgoOrders"; r.authenticated = true; break;
    case Operation::History: case Operation::AlgoHistory: case Operation::Income: case Operation::AmendHistory: {
        if (!withSymbol() || !i.cursor.isEmpty() || i.startTimeMs <= 0 || i.endTimeMs < i.startTimeMs || i.endTimeMs - i.startTimeMs >= 7 * Day)
            return reject("이력은 고정된 7일 미만 범위로 조회하세요. 상한을 넘으면 범위를 좁혀야 합니다.");
        path = op == Operation::History ? "/fapi/v1/allOrders" : op == Operation::AlgoHistory ? "/fapi/v1/allAlgoOrders" : op == Operation::Income ? "/fapi/v1/income" : "/fapi/v1/orderAmendment";
        r.authenticated = true;
        if (op == Operation::AmendHistory && !reference()) return reject("정정 이력의 원주문 참조가 필요합니다.");
        p.append(Params::value_type{"startTime",QString::number(i.startTimeMs)}); p.append(Params::value_type{"endTime",QString::number(i.endTimeMs)});
        p.append(Params::value_type{"limit",op == Operation::AmendHistory ? "100" : "1000"}); break;
    }
    case Operation::QueryOrder:
        path = "/fapi/v1/order"; r.authenticated = true;
        if (!withSymbol() || !reference()) return reject("일반 주문의 정확한 참조가 필요합니다.");
        if (!i.algoId.isEmpty() || !i.clientAlgoId.isEmpty() || (i.futures && i.futures->family != OrderFamily::Ordinary)) return reject("조건부 주문 ID는 별도 경로를 사용해야 합니다.");

         break;
    case Operation::AlgoQueryOrder:
        if (!i.orderId.isEmpty() || !i.clientId.isEmpty() || (i.futures && i.futures->family != OrderFamily::Conditional)) return reject("조건부 주문과 일반 주문 참조를 섞을 수 없습니다.");
        path = "/fapi/v1/algoOrder"; r.authenticated = true;
        if (!i.algoId.isEmpty()) { if (!id(i.algoId)) return reject("조건부 주문 ID를 확인하세요."); p.append(Params::value_type{"algoId",i.algoId}); }
        else { if (!client(i.clientAlgoId)) return reject("조건부 주문 참조가 필요합니다."); p.append(Params::value_type{"clientAlgoId",i.clientAlgoId}); } break;
    default: return reject("지원하지 않는 USDⓈ-M 요청입니다.");
    }
    if (r.authenticated) {
        const auto key = credentials.find("api_key"), secret = credentials.find("secret");
        if (now <= 0 || key == credentials.end() || secret == credentials.end() || !secretValid(key->second) || !secretValid(secret->second)) return reject("확인된 서버 시각과 HMAC API 자격정보가 필요합니다.");
        p.append(Params::value_type{"recvWindow",QString::number(BinanceRecvWindowMs)}); p.append(Params::value_type{"timestamp",QString::number(now)}); QByteArray wire = encode(p);
        SecureBuffer stateBuffer(sizeof(crypto_auth_hmacsha256_state)), digest(crypto_auth_hmacsha256_BYTES);
        auto* state = reinterpret_cast<crypto_auth_hmacsha256_state*>(stateBuffer.data());
        if (crypto_auth_hmacsha256_init(state,secret->second.data(),secret->second.size()) || crypto_auth_hmacsha256_update(state,reinterpret_cast<const unsigned char*>(wire.constData()),wire.size()) || crypto_auth_hmacsha256_final(state,digest.data())) return reject("서명을 생성하지 못했습니다.");
        QByteArray signature = QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()),qsizetype(digest.size())).toHex();
        wire += "&signature=" + signature; sodium_memzero(signature.data(),std::size_t(signature.size()));
        r.headers.insert("X-MBX-APIKEY",QByteArray(reinterpret_cast<const char*>(key->second.data()),qsizetype(key->second.size())));
        if (r.method == "GET" || r.method == "DELETE") r.url = QUrl::fromEncoded(origin + path + '?' + wire,QUrl::StrictMode);
        else { r.url = QUrl::fromEncoded(origin + path,QUrl::StrictMode); r.body = wire; r.headers.insert("Content-Type","application/x-www-form-urlencoded"); }
    } else { const auto query = encode(p); r.url = QUrl::fromEncoded(origin + path + (query.isEmpty() ? QByteArray() : '?' + query),QUrl::StrictMode); }
    return r;
}

AdapterResult BinanceUsdmAdapter::parse(Operation op,const HttpResult& http,const OrderInput& i) const {
    const bool mutation = isMutation(op);
    if (http.timedOut || http.networkError) return fail(op,"NETWORK_UNKNOWN","응답을 확인하지 못했습니다. 같은 주문을 자동 재전송하지 않습니다.",true);
    if (http.status == 401 || http.status == 403) { auto r = fail(op,"AUTHENTICATION","API 인증 또는 허용 IP를 확인하세요.",true); r.authenticationFailed = true; return r; }
    QString error; const auto doc = parseExactJson(http.body,&error,op == Operation::Markets || op == Operation::Rules ? JsonResponseScope::BinanceMarketCatalog : JsonResponseScope::Standard);
    if (!error.isEmpty() || doc.isNull()) return fail(op,"INVALID_JSON","Binance 선물 응답을 안전하게 해석하지 못했습니다.",true);
    const auto o = doc.object(); const auto errorCode = str(o,"code");
    if (http.status == 429 || http.status == 418 || errorCode == "-1003" || errorCode == "-1015") {
        auto r = fail(op,"RATE_LIMIT","요청 한도에 도달했습니다. 주문 상태를 조회한 뒤 재시도하세요.",true); r.rateLimited = true;
        bool ok = false; const auto seconds = http.headers.value("retry-after").toInt(&ok); if (ok && seconds >= 0 && seconds <= 86400) r.retryAfterSeconds = seconds; return r;
    }
    if (errorCode.startsWith('-')) {
        auto r = fail(op,"BINANCE_" + errorCode,"Binance 선물이 요청을 처리하지 못했습니다. 오류 코드와 공식 앱에서 상태를 확인하세요.",mutation);
        r.authenticationFailed = errorCode == "-2014" || errorCode == "-2015" || errorCode == "-1022";
        r.notFound = errorCode == "-2013" || errorCode == "-2026"; // Retention/absence is never proof of no submission.
        const QSet<QString> rejected{"-1100","-1102","-1111","-1121","-2019","-2027","-2028","-5047"};
        if (mutation && rejected.contains(errorCode)) { r.definiteRejection = true; r.unknown = false; r.needsReconciliation = false; r.requestState = "rejected"; }
        return r;
    }
    if (!mutation && (http.status < 200 || http.status >= 300)) return fail(op,"HTTP_ERROR","Binance 선물 조회에 실패했습니다.");
    AdapterResult r;
    auto malformed = [&]() { return fail(op,"SCHEMA_UNKNOWN","Binance 선물 응답 필드 또는 완전성을 확인하지 못했습니다.",mutation); };
    auto arrayRows = [&]() -> QJsonArray { return doc.isArray() ? doc.array() : QJsonArray{o}; };
    switch (op) {
    case Operation::Time:
        if (!stamp(str(o,"serverTime"))) return malformed(); r.serverTimeMs = str(o,"serverTime").toLongLong(); r.data = only(o,{"serverTime"}); break;
    case Operation::Markets: case Operation::Rules: {
        if (!o.value("symbols").isArray() || o.value("symbols").toArray().size() > 20000) return malformed(); QSet<QString> seen;
        for (const auto& v : o.value("symbols").toArray()) { const auto row = v.toObject(); const auto s = str(row,"symbol");
            if (!v.isObject() || !symbol(s) || seen.contains(s) || !binanceIdentifierValid(str(row,"baseAsset")) || !binanceIdentifierValid(str(row,"quoteAsset")) ||
                !binanceIdentifierValid(str(row,"marginAsset")) || str(row,"status").isEmpty() || !row.value("filters").isArray()) return malformed(); seen.insert(s); }
        r.data = only(o,{"symbols","exchangeFilters","rateLimits"}); break;
    }
    case Operation::AccountConfig:
        if (!o.value("canTrade").isBool() || !o.value("dualSidePosition").isBool() || !o.value("multiAssetsMargin").isBool()) return malformed();
        r.data = only(o,{"canTrade","dualSidePosition","multiAssetsMargin","feeTier","updateTime"}); r.tradingPermissionKnown = true; r.tradingAllowed = o.value("canTrade").toBool(); break;
    case Operation::ApiRestrictions:
        r.data = only(o,{"enableReading","enableFutures","enablePortfolioMarginTrading","ipRestrict"});
        for (auto it = r.data.constBegin(); it != r.data.constEnd(); ++it) if (!it.value().isBool()) return malformed();
        r.tradingPermissionKnown = r.data.value("enableFutures").isBool(); r.tradingAllowed = r.data.value("enableFutures").toBool(); break;
    case Operation::Account: {
        if (!o.value("assets").isArray() || o.value("assets").toArray().size() > 2000) return malformed(); QSet<QString> seen; QJsonArray assets;
        r.data = only(o,{"totalWalletBalance","totalMarginBalance","totalInitialMargin","totalMaintMargin","totalUnrealizedProfit","availableBalance"});
        for (auto it = r.data.constBegin(); it != r.data.constEnd(); ++it) if (!signedNum(it.value().toString())) return malformed();
        for (const auto& v : o.value("assets").toArray()) { const auto row = v.toObject(); Balance b;
            b.asset = str(row,"asset"); b.section = "futures"; b.free = str(row,"availableBalance"); b.cashBalance = str(row,"walletBalance"); b.equity = str(row,"marginBalance");
            if (!v.isObject() || !binanceIdentifierValid(b.asset) || seen.contains(b.asset) || !signedNum(b.free) || !signedNum(b.cashBalance) || !signedNum(b.equity)) return malformed();
            seen.insert(b.asset); r.balances.append(b); assets.append(only(row,{"asset","walletBalance","marginBalance","availableBalance","initialMargin","maintMargin","unrealizedProfit","openOrderInitialMargin","positionInitialMargin","updateTime"}));
        } r.data.insert("assets",assets); break;
    }
    case Operation::Positions: {
        if (!doc.isArray() || doc.array().size() > 10000) return malformed(); QSet<QString> seen;
        for (const auto& v : doc.array()) { if (!v.isObject()) return malformed(); const auto row = v.toObject(); PositionRecord p;
            p.symbol = str(row,"symbol"); p.positionSide = str(row,"positionSide"); p.amount = str(row,"positionAmt"); p.entryPrice = str(row,"entryPrice");
            p.breakEvenPrice = str(row,"breakEvenPrice"); p.markPrice = str(row,"markPrice"); p.liquidationPrice = str(row,"liquidationPrice"); p.unrealizedPnl = str(row,"unRealizedProfit");
            p.notional = str(row,"notional"); p.marginAsset = str(row,"marginAsset"); p.isolatedWallet = str(row,"isolatedWallet"); p.initialMargin = str(row,"initialMargin"); p.maintMargin = str(row,"maintMargin"); p.updateTime = str(row,"updateTime");
            const auto key = p.symbol + '/' + p.positionSide;
            if (!symbol(p.symbol) || !positionSide(p.positionSide) || seen.contains(key) || !signedNum(p.amount) || !num(p.entryPrice) || !num(p.breakEvenPrice) || !num(p.markPrice) ||
                !num(p.liquidationPrice) || !signedNum(p.unrealizedPnl) || !signedNum(p.notional) || !binanceIdentifierValid(p.marginAsset) || !num(p.isolatedWallet) || !num(p.initialMargin) || !num(p.maintMargin) || !integer(p.updateTime)) return malformed();
            // V3 does not supply marginType/leverage. They stay unavailable here.
            seen.insert(key); r.positions.append(p);
        } r.data.insert("complete",true); break;
    }
    case Operation::SymbolConfig: {
        if (!doc.isArray()) return malformed(); QJsonArray rows; QSet<QString> seen;
        for (const auto& v : doc.array()) { const auto row = v.toObject(); if (!symbol(str(row,"symbol")) || seen.contains(str(row,"symbol")) ||
            (str(row,"marginType") != "CROSSED" && str(row,"marginType") != "ISOLATED") || !row.value("isAutoAddMargin").isBool() ||
            !integer(str(row,"leverage")) || !pos(str(row,"leverage")) || !num(str(row,"maxNotionalValue"))) return malformed();
            seen.insert(str(row,"symbol")); rows.append(only(row,{"symbol","marginType","isAutoAddMargin","leverage","maxNotionalValue"})); }
        r.data.insert("rows",rows); break;
    }
    case Operation::LeverageBrackets: {
        QJsonArray rows; for (const auto& v : arrayRows()) { const auto row = v.toObject();
            if (!symbol(str(row,"symbol")) || !row.value("brackets").isArray() || row.value("brackets").toArray().isEmpty() ||
                (row.contains("notionalCoef") && !pos(str(row,"notionalCoef")))) return malformed();
            for (const auto& bv : row.value("brackets").toArray()) { const auto b = bv.toObject(); if (!integer(str(b,"bracket")) || !pos(str(b,"initialLeverage")) ||
                !num(str(b,"notionalFloor")) || !pos(str(b,"notionalCap")) || !num(str(b,"maintMarginRatio")) || !num(str(b,"cum"))) return malformed(); }
            rows.append(only(row,{"symbol","notionalCoef","brackets"})); }
        r.data.insert("rows",rows); break;
    }
    case Operation::Commissions:
        if (str(o,"symbol") != i.symbol || !signedNum(str(o,"makerCommissionRate")) || !signedNum(str(o,"takerCommissionRate"))) return malformed();
        r.data = only(o,{"symbol","makerCommissionRate","takerCommissionRate"}); break;
    case Operation::TradingStatus: {
        if (!o.value("indicators").isObject()) return malformed(); const auto indicators = o.value("indicators").toObject(); QJsonObject safe;
        for (auto it = indicators.constBegin(); it != indicators.constEnd(); ++it) { if (!binanceIdentifierValid(it.key()) || !it.value().isArray()) return malformed(); QJsonArray rows;
            for (const auto& v : it.value().toArray()) { const auto row = v.toObject(); if (!row.value("isLocked").isBool()) return malformed(); rows.append(only(row,{"indicator","value","triggerValue","isLocked","plannedRecoverTime"})); }
            safe.insert(it.key(),rows); }
        r.data.insert("indicators",safe); if (o.contains("updateTime")) r.data.insert("updateTime",o.value("updateTime")); break;
    }
    case Operation::Quote:
        if (str(o,"symbol") != i.symbol || !pos(str(o,"price")) || !stamp(str(o,"time"))) return malformed();
        r.data = only(o,{"symbol","price","time"}); r.data.insert("lastPrice",str(o,"price")); r.data.insert("timestamp",str(o,"time")); break;
    case Operation::MarkPrice:
        if (str(o,"symbol") != i.symbol || !pos(str(o,"markPrice")) || !pos(str(o,"indexPrice")) || !signedNum(str(o,"lastFundingRate")) || !stamp(str(o,"nextFundingTime")) || !stamp(str(o,"time"))) return malformed();
        r.data = only(o,{"symbol","markPrice","indexPrice","lastFundingRate","nextFundingTime","time"}); break;
    case Operation::Depth: {
        if (!stamp(str(o,"E")) || !stamp(str(o,"T")) || !o.value("bids").isArray() || !o.value("asks").isArray()) return malformed();
        for (const char* key : {"bids","asks"}) { const auto rows = o.value(QLatin1String(key)).toArray(); if (rows.isEmpty() || rows.size() > 1000) return malformed(); QString previous;
            for (const auto& v : rows) { const auto row = v.toArray(); if (!v.isArray() || row.size() != 2 || !pos(row[0].toString()) || !pos(row[1].toString())) return malformed();
                if (!previous.isEmpty() && (qstrcmp(key,"bids") == 0 ? decimal::compare(previous,row[0].toString()) <= 0 : decimal::compare(previous,row[0].toString()) >= 0)) return malformed(); previous = row[0].toString(); } }
        r.data = only(o,{"bids","asks","lastUpdateId"}); r.data.insert("bid",o.value("bids").toArray()[0].toArray()[0]); r.data.insert("ask",o.value("asks").toArray()[0].toArray()[0]);
        if (decimal::compare(str(r.data,"bid"),str(r.data,"ask")) >= 0) return malformed(); r.data.insert("timestamp",str(o,"E")); r.data.insert("transactionTime",str(o,"T")); break;
    }
    case Operation::OpenOrders: case Operation::History: case Operation::AlgoOpenOrders: case Operation::AlgoHistory: {
        if (!doc.isArray() || doc.array().size() > 10000) return malformed(); const bool algo = op == Operation::AlgoOpenOrders || op == Operation::AlgoHistory; QSet<QString> seen;
        for (const auto& v : doc.array()) { OrderRecord order; if (!v.isObject() || !(algo ? conditional(v.toObject(),&order) : ordinary(v.toObject(),&order))) return malformed();
            const auto key = order.symbol + QLatin1Char('/') + (algo ? order.algoId : order.orderId); if (seen.contains(key)) return malformed(); seen.insert(key);
            if ((op == Operation::History || op == Operation::AlgoHistory) && order.symbol != i.symbol) return malformed(); r.orders.append(order); }
        if ((op == Operation::History || op == Operation::AlgoHistory) && doc.array().size() >= 1000) { r.code = "HISTORY_LIMIT"; r.message = "이력 응답이 1000행 상한에 도달했습니다. 조회 범위를 좁혀야 합니다."; r.data.insert("complete",false); return r; }
        r.data.insert("complete",true); break;
    }
    case Operation::QueryOrder: case Operation::AlgoQueryOrder: {
        OrderRecord order; const bool algo = op == Operation::AlgoQueryOrder;
        if (!doc.isObject() || !(algo ? conditional(o,&order) : ordinary(o,&order))) return malformed();
        if (algo ? ((!i.algoId.isEmpty() && order.algoId != i.algoId) || (!i.clientAlgoId.isEmpty() && order.clientAlgoId != i.clientAlgoId)) :
            ((!i.orderId.isEmpty() && order.orderId != i.orderId) || (!i.clientId.isEmpty() && order.clientId != i.clientId) || order.symbol != i.symbol)) return malformed();
        if (algo && !i.symbol.isEmpty() && order.symbol != i.symbol) return malformed(); r.orders.append(order); break;
    }
    case Operation::AmendHistory: {
        if (!doc.isArray() || doc.array().size() > 100) return malformed(); QJsonArray rows;
        for (const auto& v : doc.array()) { const auto row = v.toObject(), a = row.value("amendment").toObject();
            if (!id(str(row,"orderId")) || (!i.orderId.isEmpty() && str(row,"orderId") != i.orderId) || !stamp(str(row,"time")) || (a.contains("modifyId") && !id(str(a,"modifyId")))) return malformed();
            for (const char* key : {"price","origQty"}) { const auto part = a.value(QLatin1String(key)).toObject(); if (!num(str(part,"before")) || !num(str(part,"after"))) return malformed(); }
            rows.append(only(row,{"amendmentId","symbol","pair","orderId","clientOrderId","time","amendment"})); }
        r.data.insert("rows",rows); if (rows.size() >= 100) { r.code = "HISTORY_LIMIT"; r.message = "정정 이력 상한에 도달해 완전성을 확인하지 못했습니다."; return r; } break;
    }
    case Operation::Income: {
        if (!doc.isArray() || doc.array().size() > 1000) return malformed(); QJsonArray rows;
        for (const auto& v : doc.array()) { const auto row = v.toObject(); if (!signedNum(str(row,"income")) || !stamp(str(row,"time")) || !binanceIdentifierValid(str(row,"asset")) || str(row,"incomeType").isEmpty()) return malformed();
            rows.append(only(row,{"symbol","incomeType","income","asset","time","tranId","tradeId"})); }
        r.data.insert("rows",rows); if (rows.size() >= 1000) { r.code = "HISTORY_LIMIT"; r.message = "수입 이력 상한에 도달했습니다."; return r; } break;
    }
    default: return fail(op,"UNSUPPORTED","현재 구현하지 않은 선물 응답입니다.");
    }
    r.ok = true; return r;
}

OrderValidation BinanceUsdmAdapter::validate(const OrderInput& i,const MarketSnapshot& s,const OrderLimits& limits) const {
    OrderValidation v;
    const auto reject = [&](const QString& message) { v.message = message; return v; };
    const bool algo = i.futures && i.futures->family == OrderFamily::Conditional;
    const bool limit = i.type == "limit" || (algo && (i.type == "stop" || i.type == "take_profit"));
    if (!i.futures || !symbol(i.symbol) || !side(i.side) || (!limit && (algo || i.type != "market")) ||
        !pos(i.quantity) || !i.quoteAmount.isEmpty() || !i.algoId.isEmpty() || !i.clientAlgoId.isEmpty() || !i.actualOrderId.isEmpty() ||
        (algo && (!i.orderId.isEmpty() || !i.clientId.isEmpty() || (i.type != "stop" && i.type != "take_profit"))))
        return reject("일반 지정가·시장가 또는 STOP·TAKE_PROFIT 지정가의 기초자산 수량을 입력하세요.");
    const auto& terms = *i.futures;
    if (!positionSide(terms.positionSide) || terms.closePosition || (!algo && (!terms.workingType.isEmpty() || !terms.triggerPrice.isEmpty() || terms.priceProtect.has_value())) ||
        (algo && (!pos(terms.triggerPrice) || (terms.workingType != "MARK_PRICE" && terms.workingType != "CONTRACT_PRICE") ||
            terms.priceProtect != std::optional<bool>(false) || !terms.modifyId.isEmpty())) ||
        !terms.activationPrice.isEmpty() || !terms.callbackRate.isEmpty() ||
        (limit ? (!pos(i.price) || terms.timeInForce != "GTC") : (!i.price.isEmpty() || !terms.timeInForce.isEmpty())))
        return reject("주문 유형과 포지션 조건을 확인하세요. 설정은 자동 변경하지 않습니다.");
    if (s.serverNowMs <= 0) return reject("확인된 선물 서버 시각이 필요합니다.");
    const auto cfg = s.data.value(Operation::AccountConfig);
    if (!cfg.value("canTrade").isBool() || !cfg.value("canTrade").toBool() || !cfg.value("dualSidePosition").isBool() || !cfg.value("multiAssetsMargin").isBool())
        return reject("표준 FAPI 계정의 거래 권한과 포지션 모드를 확인하지 못했습니다.");
    if (cfg.value("multiAssetsMargin").toBool()) return reject("다중 자산 증거금 모드는 조회만 지원합니다. 단일 자산 모드를 자동 변경하지 않습니다.");
    if (environment_ == "live") {
        const auto permissions = s.data.value(Operation::ApiRestrictions);
        if (!permissions.value("enableFutures").isBool() || !permissions.value("enableFutures").toBool()) return reject("이 API 키의 선물 거래 권한이 확인되지 않았습니다. 계좌 조회는 계속 사용할 수 있습니다.");
    }
    const bool hedge = cfg.value("dualSidePosition").toBool();
    if ((hedge && (terms.positionSide == "BOTH" || terms.reduceOnly)) || (!hedge && terms.positionSide != "BOTH"))
        return reject("조회한 one-way/hedge 모드와 주문의 positionSide·reduceOnly가 다릅니다.");
    const auto rules = findSymbol(s.data.value(Operation::Rules).value("symbols").toArray(),i.symbol);
    if (rules.isEmpty() || str(rules,"status") != "TRADING" || str(rules,"underlyingType") != "COIN" ||
        (str(rules,"quoteAsset") != "USDT" && str(rules,"quoteAsset") != "USDC") || str(rules,"quoteAsset") != str(rules,"marginAsset"))
        return reject("거래 중인 COIN 기초자산의 USDT·USDC 선형 계약만 주문할 수 있습니다. 다른 상품은 조회만 지원합니다.");
    v.currency = str(rules,"quoteAsset");
    bool typeAllowed = false; for (const auto& t : rules.value("orderTypes").toArray()) if (t.toString() == i.type.toUpper()) typeAllowed = true;
    if (!typeAllowed) return reject("종목의 공식 주문 유형에 포함되지 않습니다.");
    const auto symbolCfg = findSymbol(s.data.value(Operation::SymbolConfig).value("rows").toArray(),i.symbol);
    const auto leverage = str(symbolCfg,"leverage");
    if (!integer(leverage) || !pos(leverage) || (str(symbolCfg,"marginType") != "CROSSED" && str(symbolCfg,"marginType") != "ISOLATED") || !pos(str(symbolCfg,"maxNotionalValue")))
        return reject("종목의 현재 증거금 방식·레버리지·최대 명목금액을 확인하지 못했습니다.");
    const auto mark = s.data.value(Operation::MarkPrice), quote = s.data.value(Operation::Quote), depth = s.data.value(Operation::Depth);
    if (str(mark,"symbol") != i.symbol || !pos(str(mark,"markPrice")) || !fresh(str(mark,"time"),s.serverNowMs) ||
        str(quote,"symbol") != i.symbol || !pos(str(quote,"price")) || !fresh(str(quote,"timestamp"),s.serverNowMs) ||
        !fresh(str(depth,"timestamp"),s.serverNowMs)) return reject("최신 mark·거래 가격·호가 증거가 필요합니다.");
    if (!s.data.value(Operation::Positions).value("complete").toBool() || !s.data.value(Operation::OpenOrders).value("complete").toBool() || !s.data.value(Operation::AlgoOpenOrders).value("complete").toBool())
        return reject("포지션과 일반·조건부 미체결 주문의 계좌 전체 조회가 필요합니다.");
    const auto indicators = s.data.value(Operation::TradingStatus).value("indicators");
    if (!indicators.isObject()) return reject("선물 거래 제한 상태를 확인하지 못했습니다.");
    const auto indicatorObject = indicators.toObject();
    for (auto it = indicatorObject.constBegin(); it != indicatorObject.constEnd(); ++it) if (it.key() == i.symbol || it.key() == "ACCOUNT") {
        for (const auto& row : it.value().toArray()) if (!row.toObject().value("isLocked").isBool() || row.toObject().value("isLocked").toBool()) return reject("계정 또는 종목의 API 거래가 제한되어 있습니다.");
    }
    const auto lot = filter(rules,"LOT_SIZE"), marketLot = filter(rules,"MARKET_LOT_SIZE");
    if (!stepRange(i.quantity,lot,"minQty","maxQty","stepSize") || (i.type == "market" && !stepRange(i.quantity,marketLot,"minQty","maxQty","stepSize")))
        return reject("수량이 종목의 최소·최대 수량 또는 수량 단위에 맞지 않습니다.");
    QString effectivePrice = i.price;
    if (limit) {
        if (!stepRange(i.price,filter(rules,"PRICE_FILTER"),"minPrice","maxPrice","tickSize")) return reject("가격이 종목의 최소·최대 가격 또는 호가 단위에 맞지 않습니다.");
        const auto percent = filter(rules,"PERCENT_PRICE"); const auto down = str(percent,"multiplierDown"), up = str(percent,"multiplierUp");
        if (!pos(down) || !pos(up)) return reject("mark 가격 대비 주문 가격 범위를 확인하지 못했습니다.");
        if ((i.side == "buy" && decimal::compare(i.price,decimal::multiply(str(mark,"markPrice"),up)) > 0) ||
            (i.side == "sell" && decimal::compare(i.price,decimal::multiply(str(mark,"markPrice"),down)) < 0)) return reject("가격이 mark 가격 기준의 거래소 허용 범위를 벗어났습니다.");
    } else {
        const auto book = depth.value(i.side == "buy" ? "asks" : "bids").toArray(); QString total = "0";
        for (const auto& entry : book) { const auto row = entry.toArray(); if (row.size() != 2 || !pos(row[0].toString()) || !pos(row[1].toString())) return reject("호가 수량을 확인하지 못했습니다.");
            total = decimal::add(total,row[1].toString()); effectivePrice = row[0].toString(); if (decimal::compare(total,i.quantity) >= 0) break; }
        if (!pos(effectivePrice) || decimal::compare(total,i.quantity) < 0) return reject("조회한 호가 범위에서 시장가 수량을 충당하지 못합니다.");
    }
    QString riskPrice = decimal::compare(effectivePrice,str(mark,"markPrice")) > 0 ? effectivePrice : str(mark,"markPrice");
    if (algo) {
        if (!stepRange(terms.triggerPrice,filter(rules,"PRICE_FILTER"),"minPrice","maxPrice","tickSize")) return reject("발동 가격이 종목 호가 단위·가격 범위에 맞지 않습니다.");
        const auto basis = terms.workingType == "MARK_PRICE" ? str(mark,"markPrice") : str(quote,"price");
        const bool rises = (i.type == "stop" && i.side == "buy") || (i.type == "take_profit" && i.side == "sell");
        const int relation = decimal::compare(terms.triggerPrice,basis);
        if (rises ? relation <= 0 : relation >= 0) return reject("현재 발동 기준 가격에서 이미 충족된 조건입니다. 발동 방향·가격을 확인하세요.");
        if (decimal::compare(terms.triggerPrice,riskPrice) > 0) riskPrice = terms.triggerPrice;
    }
    v.notional = decimal::multiply(i.quantity,riskPrice);
    if (!pos(limits.maxNotional) || limits.currency != v.currency || !num(limits.maxDeviationPercent) || decimal::compare(limits.maxDeviationPercent,"100") > 0)
        return reject("해당 결제 통화의 최대 주문 금액·가격 괴리율을 먼저 설정하세요.");
    if (decimal::compare(v.notional,limits.maxNotional) > 0) return reject("사용자가 정한 최대 주문 명목금액을 넘었습니다.");
    const QString last = str(quote,"price"), difference = decimal::compare(effectivePrice,last) >= 0 ? decimal::subtract(effectivePrice,last) : decimal::subtract(last,effectivePrice);
    if (decimal::compare(decimal::multiply(difference,"100"),decimal::multiply(last,limits.maxDeviationPercent)) > 0) return reject("사용자가 정한 가격 괴리율을 넘었습니다.");
    if (algo) {
        const auto basis = terms.workingType == "MARK_PRICE" ? str(mark,"markPrice") : last;
        const auto gap = decimal::compare(terms.triggerPrice,basis) >= 0 ? decimal::subtract(terms.triggerPrice,basis) : decimal::subtract(basis,terms.triggerPrice);
        if (decimal::compare(decimal::multiply(gap,"100"),decimal::multiply(basis,limits.maxDeviationPercent)) > 0) return reject("발동 가격이 사용자가 정한 괴리율을 넘었습니다.");
        int algos = 0; for (const auto& order : s.openOrders) if (order.futures && order.futures->family == OrderFamily::Conditional) ++algos;
        if (algos >= 200) return reject("계좌 전체 조건부 주문 수가 공식 200개 한도에 도달했습니다.");
    }
    const PositionRecord* selected = nullptr;
    for (const auto& p : s.positions) if (p.symbol == i.symbol && p.positionSide == terms.positionSide) { if (selected) return reject("포지션 행이 중복되어 주문을 검증할 수 없습니다."); selected = &p; }
    const QString current = selected ? selected->amount : "0";
    if (!signedNum(current)) return reject("포지션 수량을 확인하지 못했습니다.");
    const OrderRecord* old = nullptr;
    QString remainingIntent = i.quantity;
    if (!i.orderId.isEmpty()) {
        for (const auto& row : s.openOrders) if (row.futures && row.futures->family == OrderFamily::Ordinary && row.orderId == i.orderId) { if (old) return reject("원주문이 중복되었습니다."); old = &row; }
        if (!old || !old->amendable || old->symbol != i.symbol || old->side != i.side || old->type != "limit" || !old->futures ||
            old->futures->positionSide != terms.positionSide || old->futures->reduceOnly != terms.reduceOnly || old->futures->closePosition != terms.closePosition ||
            old->futures->timeInForce != terms.timeInForce || (!terms.modifyId.isEmpty() && !id(terms.modifyId)) || !num(old->executed) || decimal::compare(i.quantity,old->executed) <= 0)
            return reject("동일 일반 GTC 주문의 조건을 유지하고 이미 체결된 수량보다 큰 총 수량으로 정정해야 합니다.");
        if (same(old->price,i.price) && same(old->quantity,i.quantity)) return reject("기존 주문과 같은 가격·수량입니다.");
        remainingIntent = decimal::subtract(i.quantity,old->executed);
    }
    // External orders have no application exposure intent. An exact matched old
    // order may still be amended; server reduceOnly/hedge terms define reduction.
    const bool closingDirection = hedge ? (terms.positionSide == "LONG" ? i.side == "sell" : i.side == "buy") :
        (current.startsWith('-') ? i.side == "buy" : i.side == "sell");
    const bool reduces = terms.exposure == ExposureIntent::Reduce ||
        (old && terms.exposure == ExposureIntent::Unspecified && (terms.reduceOnly || (hedge && closingDirection)));
    if (!old && terms.exposure == ExposureIntent::Unspecified) return reject("진입 또는 축소 의도를 직접 선택하세요.");
    if (reduces) {
        if (!selected || !pos(absolute(current)) || !closingDirection || decimal::compare(remainingIntent,absolute(current)) > 0 || (!hedge && !terms.reduceOnly))
            return reject("축소 방향·잔량이 현재 포지션과 일치하지 않습니다. 반대 포지션으로 자동 전환하지 않습니다.");
    } else if (terms.reduceOnly || (hedge && closingDirection))
        return reject("진입 의도와 포지션 방향·reduceOnly 조건이 다릅니다.");
    const auto minimum = str(filter(rules,"MIN_NOTIONAL"),"notional");
    if (!num(minimum)) return reject("최소 주문 명목금액을 확인하지 못했습니다.");
    const auto minimumBasis = decimal::multiply(i.quantity,i.type == "market" ? str(mark,"markPrice") : i.price);
    if (!terms.reduceOnly && decimal::compare(minimumBasis,minimum) < 0) return reject("거래소 최소 주문 명목금액보다 작습니다.");
    const auto orderCap = str(filter(rules,"MAX_NUM_ORDERS"),"limit");
    if (!integer(orderCap)) return reject("최대 미체결 주문 수를 확인하지 못했습니다.");
    int active = 0; for (const auto& order : s.openOrders) if (order.symbol == i.symbol) ++active;
    if (i.orderId.isEmpty() && decimal::compare(QString::number(active),orderCap) >= 0) return reject("종목의 최대 미체결 주문 수에 도달했습니다.");
    if (!reduces) {
        const auto brackets = findSymbol(s.data.value(Operation::LeverageBrackets).value("rows").toArray(),i.symbol);
        if (brackets.isEmpty() || (brackets.contains("notionalCoef") && !same(str(brackets,"notionalCoef"),"1")))
            return reject("계정별 조정 증거금 구간의 적용 의미를 확인하지 못했습니다. 이 조건의 신규 위험 증가는 제한합니다.");
        const auto remainingNotional = decimal::multiply(remainingIntent,riskPrice);
        QString gross = remainingNotional;
        for (const auto& p : s.positions) if (p.symbol == i.symbol) { if (!signedNum(p.notional)) return reject("기존 포지션 명목금액이 필요합니다."); gross = decimal::add(gross,absolute(p.notional)); }
        for (const auto& order : s.openOrders) if (order.symbol == i.symbol && !(order.futures && order.futures->family == OrderFamily::Ordinary && order.orderId == i.orderId)) {
            if (!order.futures) return reject("기존 주문의 위험 조건을 확인하지 못했습니다."); if (order.futures->reduceOnly || order.futures->closePosition) continue;
            const auto quantity = order.futures->family == OrderFamily::Ordinary ? order.remaining : order.quantity;
            if (!num(quantity)) return reject("기존 미체결 수량이 확인되지 않았습니다.");
            QString price = str(mark,"markPrice"); if (pos(order.price) && decimal::compare(order.price,price) > 0) price = order.price;
            if (pos(order.futures->triggerPrice) && decimal::compare(order.futures->triggerPrice,price) > 0) price = order.futures->triggerPrice;
            gross = decimal::add(gross,decimal::multiply(quantity,price));
        }
        if (decimal::compare(gross,str(symbolCfg,"maxNotionalValue")) > 0) return reject("현재 레버리지의 최대 명목금액을 넘습니다.");
        bool bracketKnown = false;
        for (const auto& bv : brackets.value("brackets").toArray()) { const auto b = bv.toObject();
            if (num(str(b,"notionalFloor")) && pos(str(b,"notionalCap")) && pos(str(b,"initialLeverage")) && decimal::compare(gross,str(b,"notionalFloor")) >= 0 && decimal::compare(gross,str(b,"notionalCap")) < 0 && decimal::compare(leverage,str(b,"initialLeverage")) <= 0) bracketKnown = true; }
        if (!bracketKnown) return reject("주문 후 보수적으로 합산한 명목금액에 맞는 레버리지 구간이 없습니다.");
        const auto fees = s.data.value(Operation::Commissions); const QString taker = str(fees,"takerCommissionRate"), maker = str(fees,"makerCommissionRate");
        if (str(fees,"symbol") != i.symbol || !num(taker) || !num(maker)) return reject("신규 주문의 수수료를 확인하지 못했습니다. 미확인 값을 0으로 가정하지 않습니다.");
        const auto rate = decimal::compare(taker,maker) > 0 ? taker : maker;
        QString available; for (const auto& b : s.balances) if (b.asset == v.currency && b.section == "futures") available = b.free;
        if (!num(available)) return reject("해당 증거금 자산의 사용 가능한 잔고를 확인하지 못했습니다.");
        // No binary float/division/credit for an old order's unproven released margin.
        const QString fee = decimal::multiply(remainingNotional,rate);
        const QString requiredScaled = decimal::add(remainingNotional,decimal::multiply(fee,leverage));
        if (decimal::compare(decimal::multiply(available,leverage),requiredScaled) < 0) return reject("현재 레버리지와 수수료를 반영한 보수적 증거금 검사에서 사용 가능 잔고가 부족합니다.");
    }
    v.ok = true; v.message = "공식 필터·현재 모드·포지션과 주문 한도를 확인했습니다. 최종 접수 여부는 거래소가 결정합니다."; return v;
}
} // namespace keefi
