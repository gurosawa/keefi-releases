#include "keefi/IdentityCompatibility.h"
#include "keefi/TossAdapter.h"
#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QScopeGuard>
#include <QTimeZone>
#include <QUuid>
#include <array>
#include <cstring>
#include <sodium.h>

namespace keefi {
namespace {
const QString Domestic = QStringLiteral("domestic_equity");
const QString Foreign = QStringLiteral("foreign_equity");
const QString Unknown = QStringLiteral("unknown");
QString string(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
bool match(const QString& s, const char* expression) { return QRegularExpression(QLatin1String(expression)).match(s).hasMatch(); }
bool code(const QString& s) { return match(s, "\\A[A-Za-z0-9_-]{1,64}\\z"); }
bool symbol(const QString& s) { return match(s, "\\A[A-Za-z0-9.\\-]{1,24}\\z"); }
bool krSymbol(const QString& s) { return match(s, "\\A[A-Za-z0-9]{6}\\z"); }
bool orderId(const QString& s) { return match(s, "\\A[A-Za-z0-9_-]{1,128}\\z"); }
bool clientId(const QString& s) { return match(s, "\\A[A-Za-z0-9_-]{1,36}\\z"); }
bool integer(const QString& s) { return match(s, "\\A[0-9]{1,30}\\z"); }
bool number(const QString& s) { return s.size() <= 30 && decimal::valid(s); }
bool positive(const QString& s) { return number(s) && decimal::positive(s); }
bool signedNumber(QString s) { if (s.startsWith('-')) s.remove(0, 1); return number(s); }
bool safeText(const QString& s, qsizetype maximum = 200) {
    if (s.isEmpty() || s.size() > maximum) return false;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const auto ch = s[i];
        if (ch.isHighSurrogate()) {
            if (++i >= s.size() || !s[i].isLowSurrogate()) return false;
        } else if (ch.isLowSurrogate() || ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format) return false;
    }
    return true;
}
bool nullableNumber(const QJsonValue& v, bool signedValue = false) {
    return v.isNull() || (v.isString() && (signedValue ? signedNumber(v.toString()) : number(v.toString())));
}
qint64 instant(const QJsonValue& v) {
    if (!v.isString()) return 0;
    const auto s = v.toString();
    if (s.size() > 48 || !QRegularExpression(QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})$")).match(s).hasMatch()) return 0;
    const auto d = QDateTime::fromString(s, Qt::ISODateWithMs);
    return d.isValid() ? d.toMSecsSinceEpoch() : 0;
}
bool nullableTime(const QJsonValue& v) { return v.isNull() || instant(v) > 0; }
bool credential(const SecureBuffer& b) {
    if (b.empty() || b.size() > 4096) return false;
    for (std::size_t i = 0; i < b.size(); ++i) if (b.data()[i] < 0x21 || b.data()[i] > 0x7e) return false;
    return true;
}
void wipe(QByteArray& b) { if (!b.isEmpty()) sodium_memzero(b.data(), std::size_t(b.size())); b.clear(); }
void wipe(QString& s) { if (!s.isEmpty()) sodium_memzero(s.data(), std::size_t(s.size()) * sizeof(QChar)); s.clear(); }
QString accountIdentityScope(const QString& accountNumber, const QString& accountType, const SecureBuffer& sequence) {
    // Versioned domain + uint32 big-endian byte lengths makes field boundaries
    // unambiguous. Client credentials are deliberately not part of account identity.
    const auto domain = identityDomain(IdentityDomain::BrokerAccountIdentity) + QByteArray(1, '\0');
    const auto numberBytes = SecureBuffer::fromUtf8(accountNumber);
    const auto typeBytes = SecureBuffer::fromUtf8(accountType);
    const std::array<QByteArrayView, 5> fields{
        QByteArrayView("toss"), QByteArrayView("live"),
        QByteArrayView(reinterpret_cast<const char*>(numberBytes.data()), qsizetype(numberBytes.size())),
        QByteArrayView(reinterpret_cast<const char*>(typeBytes.data()), qsizetype(typeBytes.size())),
        QByteArrayView(reinterpret_cast<const char*>(sequence.data()), qsizetype(sequence.size()))};
    std::size_t size = std::size_t(domain.size());
    for (const auto field : fields) size += 4 + std::size_t(field.size());
    SecureBuffer material(size);
    std::memcpy(material.data(), domain.constData(), std::size_t(domain.size())); std::size_t offset = std::size_t(domain.size());
    for (const auto field : fields) {
        const auto length = quint32(field.size());
        for (int shift : {24, 16, 8, 0}) material.data()[offset++] = static_cast<unsigned char>((length >> shift) & 0xff);
        std::memcpy(material.data() + offset, field.data(), std::size_t(field.size())); offset += std::size_t(field.size());
    }
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, material.data(), static_cast<unsigned long long>(material.size()));
    const QString scope = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(digest), sizeof(digest)).toHex());
    sodium_memzero(digest, sizeof(digest));
    return scope;
}
AdapterResult failure(Operation op, const QString& reason, const QString& message, bool ambiguous = false) {
    AdapterResult r; r.code = reason; r.message = message;
    r.unknown = isMutation(op) && ambiguous; r.needsReconciliation = r.unknown; return r;
}
bool accountOperation(Operation op) {
    return op == Operation::Holdings || op == Operation::BuyingPower || op == Operation::SellableQuantity ||
        op == Operation::Commissions || op == Operation::OpenOrders || op == Operation::History ||
        op == Operation::QueryOrder || op==Operation::AlgoOpenOrders||op==Operation::AlgoQueryOrder||isMutation(op);
}
QString marketOf(const QString& market) {
    if (market == "KOSPI" || market == "KOSDAQ" || market == "KR_ETC") return Domestic;
    if (market == "NYSE" || market == "NASDAQ" || market == "AMEX" || market == "US_ETC") return Foreign;
    return Unknown;
}
bool numericObject(const QJsonObject& source, QJsonObject* target, const QStringList& fields, bool signedValues = false) {
    for (const auto& key : fields) {
        const auto value = source.value(key);
        if (!value.isString() || !(signedValues ? signedNumber(value.toString()) : number(value.toString()))) return false;
        target->insert(key, value);
    }
    return true;
}
bool calendarDay(const QJsonObject& raw, QJsonObject* clean) {
    const QString date = string(raw, "date");
    if (!QDate::fromString(date, Qt::ISODate).isValid() || date.size() != 10) return false;
    clean->insert("date", date);
    const auto integrated = raw.value("integrated");
    if (integrated.isNull()) { clean->insert("integrated", QJsonValue::Null); return true; }
    if (!integrated.isObject()) return false;
    QJsonObject sessions;
    for (const auto* key : {"preMarket", "regularMarket", "afterMarket"}) {
        const auto v = integrated.toObject().value(QLatin1String(key));
        if (v.isNull()) { sessions.insert(QLatin1String(key), QJsonValue::Null); continue; }
        if (!v.isObject()) return false;
        const auto o = v.toObject();
        const auto start = o.value("startTime"), end = o.value("endTime");
        if (instant(start) <= 0 || instant(end) <= instant(start)) return false;
        QJsonObject session{{"startTime", start}, {"endTime", end}};
        const QString auction = QByteArray(key) == "afterMarket" ? "singlePriceAuctionEndTime" : "singlePriceAuctionStartTime";
        const auto a = o.value(auction);
        if (!a.isUndefined() && !nullableTime(a)) return false;
        session.insert(auction, a.isUndefined() ? QJsonValue(QJsonValue::Null) : a);
        sessions.insert(QLatin1String(key), session);
    }
    clean->insert("integrated", sessions); return true;
}
bool decodeOrder(const QJsonObject& raw, const QHash<QString, QString>& markets, bool closed, OrderRecord* out) {
    out->orderId = string(raw, "orderId"); out->symbol = string(raw, "symbol");
    out->side = string(raw, "side").toLower(); out->type = string(raw, "orderType").toLower();
    out->rawStatus = string(raw, "status"); out->currency = string(raw, "currency");
    out->market = markets.value(out->symbol, Unknown);
    out->quantity = string(raw, "quantity"); out->price = string(raw, "price");
    if (!orderId(out->orderId) || !symbol(out->symbol) || (out->side != "buy" && out->side != "sell") ||
        !code(out->type) || !code(out->rawStatus) || !code(string(raw, "timeInForce")) ||
        (out->currency != "KRW" && out->currency != "USD") || !number(out->quantity) ||
        (!raw.value("price").isUndefined() && !nullableNumber(raw.value("price"))) ||
        (!raw.value("orderAmount").isUndefined() && !nullableNumber(raw.value("orderAmount"))) ||
        instant(raw.value("orderedAt")) <= 0 || (!raw.value("canceledAt").isUndefined() && !nullableTime(raw.value("canceledAt"))) || !raw.value("execution").isObject()) return false;
    const auto fill = raw.value("execution").toObject(); out->executed = string(fill, "filledQuantity");
    if (!number(out->executed)) return false;
    for (const auto* f : {"averageFilledPrice", "filledAmount", "commission", "tax"}) if (!nullableNumber(fill.value(QLatin1String(f)))) return false;
    if (!nullableTime(fill.value("filledAt"))) return false;
    const auto settlement = fill.value("settlementDate");
    if (!settlement.isNull() && (!settlement.isString() || !QDate::fromString(settlement.toString(), Qt::ISODate).isValid())) return false;
    out->quoteAmount = string(fill, "filledAmount"); out->remaining = decimal::subtract(out->quantity, out->executed);
    if (out->remaining.isEmpty()) return false;
    static const QMap<QString, QString> states{{"PENDING", "open"}, {"PARTIAL_FILLED", "partial"},
        {"FILLED", "filled"}, {"CANCELED", "canceled"}, {"REJECTED", "rejected"},
        {"CANCEL_REJECTED", "rejected"}, {"REPLACE_REJECTED", "rejected"}, {"REPLACED", "canceled"}};
    out->status = states.value(out->rawStatus, Unknown);
    const bool active = out->rawStatus == "PENDING" || out->rawStatus == "PARTIAL_FILLED";
    out->cancelable = !closed && active && out->market == Domestic && positive(out->remaining) &&
        (out->type == "limit" || out->type == "market") && string(raw, "timeInForce") == "DAY";
    // Partial-fill replacement quantity semantics are not specified publicly.
    out->amendable = out->cancelable && out->type == "limit" && decimal::compare(out->executed, "0") == 0;
    return true;
}
QString tick(const QString& price, const QString& securityType) {
    if (!positive(price)) return {};
    if (securityType == "ETF" || securityType == "FOREIGN_ETF" || securityType == "ETN")
        return decimal::compare(price, "2000") < 0 ? "1" : "5";
    if (!QStringList{"STOCK", "FOREIGN_STOCK", "DEPOSITARY_RECEIPT", "INFRASTRUCTURE_FUND", "REIT"}.contains(securityType)) return {};
    for (const auto& pair : QList<QPair<QString, QString>>{{"2000", "1"}, {"5000", "5"}, {"20000", "10"},
             {"50000", "50"}, {"200000", "100"}, {"500000", "500"}})
        if (decimal::compare(price, pair.first) < 0) return pair.second;
    return "1000";
}
}

BrokerPolicy TossAdapter::policy() const {
    BrokerPolicy p; p.provider = "toss"; p.displayName = QStringLiteral("토스증권"); p.environment = "live";
    for (auto action : {BrokerAction::Read, BrokerAction::Place, BrokerAction::Cancel, BrokerAction::Amend})
        p.actions[action] = {ApiAvailability::Documented, true, {}};
    p.catalog = {ApiAvailability::Documented, true, {}};
    p.officialOrderTest.explanation = QStringLiteral("공개 명세상 비체결 주문 테스트 없음");
    p.paperTrading.explanation = QStringLiteral("공개 명세상 모의 환경 미확인");
    p.catalogSource = QStringLiteral("토스 공식 /api/v1/stocks/all · KOSPI·KOSDAQ·KR_ETC");
    p.holdingsSource = QStringLiteral("토스 공식 /api/v1/holdings");
    p.buyingPowerSource = QStringLiteral("토스 공식 /api/v1/buying-power");
    QList<BrokerFreshnessRule> rules{{Operation::Quote, 10000, SourceTimeQuality::FullTimestamp},
        {Operation::Depth, 10000, SourceTimeQuality::FullTimestamp}, {Operation::BuyingPower, 15000},
        {Operation::SellableQuantity, 15000}, {Operation::Rules, 60000},
        {Operation::TradingSchedule, 60000}, {Operation::PriceLimits, 60000}};
    p.freshness[BrokerAction::Place] = rules; p.freshness[BrokerAction::Amend] = rules;
    return p;
}
bool TossAdapter::symbolValid(const QString& value) const { return krSymbol(value); }
bool TossAdapter::orderReferenceValid(const QString& value) const { return orderId(value); }
QString TossAdapter::credentialIdentityFingerprint(const CredentialSet& credentials) const {
    const auto id = credentials.find("client_id");
    return id == credentials.end() ? QString{} : QString::fromLatin1(QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(id->second.data()), id->second.size()), QCryptographicHash::Sha256).toHex());
}
bool TossAdapter::configureSession(const AccountRecord& account, const CredentialSet& credentials, QString* error) {
    clearSessionState();
    const auto id = credentials.find("client_id"), secret = credentials.find("client_secret");
    if (account.provider != "toss" || account.environment != "live" || account.market != Domestic ||
        id == credentials.end() || secret == credentials.end() || !credential(id->second) || !credential(secret->second)) {
        if (error) *error = QStringLiteral("토스 연결과 Client ID·Secret의 형식을 확인하세요."); return false;
    }
    configured_ = account;
    bootstrapId_ = SecureBuffer(id->second.data(), id->second.size());
    bootstrapSecret_ = SecureBuffer(secret->second.data(), secret->second.size());
    return true;
}
QList<BrokerStep> TossAdapter::plan(BrokerPlanKind kind, const OrderInput& input) const {
    QList<BrokerStep> result;
    auto plannedInput = input;
    if (kind == BrokerPlanKind::AccountRead) {
        if (!plannedInput.endTimeMs) plannedInput.endTimeMs = QDateTime::currentMSecsSinceEpoch();
        if (!plannedInput.startTimeMs) plannedInput.startTimeMs = plannedInput.endTimeMs - 7LL * 86400000;
    }
    const auto append = [&](Operation op, BrokerAuthScope auth = BrokerAuthScope::SelectedAccount) {
        BrokerStep step; step.operation = op; step.authScope = auth; step.input = plannedInput; result.append(step);
    };
    if (kind == BrokerPlanKind::Authenticate) {
        append(Operation::Authenticate, BrokerAuthScope::CredentialBootstrap);
        append(Operation::Accounts, BrokerAuthScope::Session);
    } else if (kind == BrokerPlanKind::Catalog) {
        for (const auto* market : {"KOSPI", "KOSDAQ", "KR_ETC"}) {
            BrokerStep step; step.operation = Operation::Markets; step.authScope = BrokerAuthScope::Session;
            step.input.cursor = QString::fromLatin1(market); step.catalogMarket = step.input.cursor;
            step.minimumDelayMs = 1100; step.allowEmptyCatalog = step.input.cursor == "KR_ETC"; result.append(step);
        }
    } else if (kind == BrokerPlanKind::AccountRead) {
        for (const auto op : refreshPlan()) append(op);
        for (const auto op : {Operation::BuyingPower, Operation::SellableQuantity, Operation::PriceLimits,
             Operation::TradingSchedule, Operation::Quote, Operation::Depth}) append(op);
    }
    return result;
}
RequestSpec TossAdapter::buildStep(const BrokerStep& step, qint64 nowMs, QString* error) const {
    CredentialSet credentials;
    if (step.operation == Operation::Authenticate) {
        credentials.emplace("client_id", SecureBuffer(bootstrapId_.data(), bootstrapId_.size()));
        credentials.emplace("client_secret", SecureBuffer(bootstrapSecret_.data(), bootstrapSecret_.size()));
        bootstrapId_.clear(); bootstrapSecret_.clear();
    }
    auto input = step.input; if (!step.cursorHandle.isEmpty()) input.cursor = step.cursorHandle;
    return build(step.operation, configured_, input, credentials, nowMs, error);
}
BrokerStepResult TossAdapter::parseStep(const BrokerStep& step, const HttpResult& response) const {
    auto input = step.input; if (!step.cursorHandle.isEmpty()) input.cursor = step.cursorHandle;
    BrokerStepResult result; result.value = parse(step.operation, response, input);
    result.accountReadVerified = result.value.ok && step.operation == Operation::Holdings && !selectedAccountScope().isEmpty();
    const auto timestamp = result.value.data.value("timestamp");
    if (timestamp.isString() && instant(timestamp) > 0) {
        result.sourceTime.quality = SourceTimeQuality::FullTimestamp;
        result.sourceTime.rawText = timestamp.toString(); result.sourceTime.fieldName = "timestamp";
        result.sourceTime.fullTimestamp = QDateTime::fromMSecsSinceEpoch(instant(timestamp), QTimeZone::UTC);
    }
    return result;
}
BrokerReadiness TossAdapter::actionReadiness(BrokerAction action, const OrderInput&, const QList<BrokerEvidence>&, const OrderLimits&) const {
    if (!policy().actions.value(action).implemented) return {false, BrokerNotReady::NotImplemented, QStringLiteral("구현되지 않은 증권 동작입니다.")};
    return {true, BrokerNotReady::None, {}};
}

TossAdapter::~TossAdapter() { clearSessionState(); }
void TossAdapter::clearSessionState() const {
    ++marketSessionRevision_;
    bootstrapId_.clear(); bootstrapSecret_.clear(); token_.clear(); selectedSequence_.clear(); accounts_.clear(); sessionClock_.invalidate(); sessionLifetimeMs_ = 0;
    selectedScope_.clear(); selectedLabel_.clear(); symbolMarkets_.clear();
    marketHoldings_.clear(); marketHoldingsAt_ = -1;
}
void TossAdapter::clearSession() { clearSessionState(); }
bool TossAdapter::sessionReady() const {
    if (token_.empty() || !sessionClock_.isValid()) return false;
    if (sessionClock_.elapsed() >= sessionLifetimeMs_) { clearSessionState(); return false; }
    return true;
}
bool TossAdapter::withMarketAuthorization(const MarketAuthorizationBorrow& borrow) const {
    if (!borrow || !sessionReady()) return false;
    const auto remaining = sessionLifetimeMs_ - sessionClock_.elapsed();
    if (remaining <= 0) return false;
    borrow(QByteArrayView(reinterpret_cast<const char*>(token_.data()), qsizetype(token_.size())), remaining, marketSessionRevision_);
    return true;
}
QString TossAdapter::marketDataCountry(const QString& symbol) const {
    if (!sessionReady()) return {};
    const auto market = symbolMarkets_.value(symbol);
    return market == Domestic ? QStringLiteral("KR") : market == Foreign ? QStringLiteral("US") : QString();
}
QStringList TossAdapter::marketDataHoldings() const {
    if (!sessionReady() || marketHoldingsAt_ < 0 || sessionClock_.elapsed() - marketHoldingsAt_ > 180000) return {};
    return marketHoldings_;
}
QList<BrokerAccountChoice> TossAdapter::accountChoices() const {
    QList<BrokerAccountChoice> result;
    if (sessionReady()) for (const auto& entry : accounts_) result.append(entry.choice);
    return result;
}
bool TossAdapter::selectBrokerAccount(const QString& handle, QString* error) {
    if (error) error->clear();
    if (sessionReady()) for (const auto& entry : accounts_) if (entry.choice.handle == handle) {
        marketHoldings_.clear(); marketHoldingsAt_ = -1;
        selectedSequence_ = SecureBuffer(entry.sequence.data(), entry.sequence.size());
        selectedScope_ = entry.identityScope;
        selectedLabel_ = entry.choice.label; return true;
    }
    if (error) *error = QStringLiteral("이번 연결에서 조회한 계좌를 선택하세요.");
    return false;
}
QString TossAdapter::selectedAccountScope() const { return sessionReady() ? selectedScope_ : QString(); }
QString TossAdapter::selectedAccountLabel() const { return sessionReady() ? selectedLabel_ : QString(); }
QList<Operation> TossAdapter::refreshPlan() const {
    return {Operation::OpenOrders, Operation::History, Operation::Rules, Operation::TradingSchedule,
        Operation::PriceLimits, Operation::Holdings, Operation::BuyingPower, Operation::SellableQuantity,
        Operation::Commissions, Operation::OpenOrders, Operation::Quote, Operation::Depth};
}
QString TossAdapter::amendDescription() const {
    return QStringLiteral("국내 DAY 지정가 미체결 주문의 가격·수량을 정정합니다. 부분 체결 정정은 아직 지원하지 않습니다. 접수 후 원주문과 새 작업 ID를 함께 확인합니다.");
}

RequestSpec TossAdapter::build(Operation op, const AccountRecord& account, const OrderInput& input,
                               const CredentialSet& credentials, qint64, QString* error) const {
    if (error) error->clear();
    const auto reject = [&](const QString& reason) { if (error) *error = reason; return RequestSpec{}; };
    if (account.provider != "toss" || account.market != Domestic || account.environment != "live")
        return reject(QStringLiteral("토스증권 국내주식 실전 연결만 지원합니다. 공식 모의 환경은 확인되지 않았습니다."));
    RequestSpec request; request.operation = op; request.authenticated = true;
    QByteArray path, query;
    const auto parameter = [&](const QByteArray& name, const QString& value) {
        if (!query.isEmpty()) query.append('&'); query += name + '=' + QUrl::toPercentEncoding(value);
    };
    if (op == Operation::Authenticate) {
        clearSessionState();
        const auto id = credentials.find("client_id"), secret = credentials.find("client_secret");
        if (id == credentials.end() || secret == credentials.end() || !credential(id->second) || !credential(secret->second))
            return reject(QStringLiteral("Client ID·Secret의 형식과 앞뒤 공백을 확인하세요."));
        request.method = "POST"; path = "/oauth2/token";
        request.headers.insert("Content-Type", "application/x-www-form-urlencoded");
        QByteArray idBytes(reinterpret_cast<const char*>(id->second.data()), qsizetype(id->second.size()));
        QByteArray secretBytes(reinterpret_cast<const char*>(secret->second.data()), qsizetype(secret->second.size()));
        request.body = "grant_type=client_credentials&client_id=" + idBytes.toPercentEncoding() + "&client_secret=" + secretBytes.toPercentEncoding();
        wipe(idBytes); wipe(secretBytes);
        request.url = QUrl(QStringLiteral("https://openapi.tossinvest.com/oauth2/token")); return request;
    }
    if (!sessionReady()) return reject(QStringLiteral("토스 연결이 만료되었거나 잠겼습니다. 다시 연결하세요."));
    if (accountOperation(op) && selectedSequence_.empty()) return reject(QStringLiteral("조회할 토스 계좌를 먼저 선택하세요."));
    const bool requiresSymbol = op == Operation::Rules || op == Operation::Quote || op == Operation::Depth ||
        op == Operation::PriceLimits || op == Operation::SellableQuantity;
    if (requiresSymbol && !krSymbol(input.symbol)) return reject(QStringLiteral("국내 종목코드 6자리를 확인하세요."));
    QJsonObject body;
    switch (op) {
    case Operation::AlgoOpenOrders:
        path="/api/v1/conditional-orders";parameter("status","OPEN");parameter("limit","100");
        if(!input.cursor.isEmpty()){if(!match(input.cursor,"\\A[A-Za-z0-9_-]{1,256}\\z"))return reject("조건주문 cursor가 올바르지 않습니다.");parameter("cursor",input.cursor);}break;
    case Operation::AlgoQueryOrder:
        if(!input.brokerConditional||!orderId(input.orderId))return reject("조건주문의 전용 ID가 필요합니다.");
        path="/api/v1/conditional-orders/"+input.orderId.toLatin1();break;
    case Operation::Accounts: path = "/api/v1/accounts"; break;
    case Operation::Markets:
        if (marketOf(input.cursor) != Domestic) return reject(QStringLiteral("국내 종목 목록의 시장을 선택하세요."));
        path = "/api/v1/stocks/all"; parameter("market", input.cursor); break;
    case Operation::Rules: path = "/api/v1/stocks"; parameter("symbols", input.symbol); break;
    case Operation::Quote: path = "/api/v1/prices"; parameter("symbols", input.symbol); break;
    case Operation::Depth: path = "/api/v1/orderbook"; parameter("symbol", input.symbol); break;
    case Operation::PriceLimits: path = "/api/v1/price-limits"; parameter("symbol", input.symbol); break;
    case Operation::TradingSchedule: path = "/api/v1/market-calendar/KR"; break;
    case Operation::Holdings: path = "/api/v1/holdings"; break;
    case Operation::BuyingPower: path = "/api/v1/buying-power"; parameter("currency", "KRW"); break;
    case Operation::SellableQuantity: path = "/api/v1/sellable-quantity"; parameter("symbol", input.symbol); break;
    case Operation::Commissions: path = "/api/v1/commissions"; break;
    case Operation::OpenOrders: path = "/api/v1/orders"; parameter("status", "OPEN"); break;
    case Operation::History: {
        if (input.startTimeMs < 0 || input.endTimeMs < 0 || (input.startTimeMs && input.endTimeMs && input.startTimeMs > input.endTimeMs) ||
            input.cursor.size() > 2048 || (!input.cursor.isEmpty() && !safeText(input.cursor, 2048)))
            return reject(QStringLiteral("주문 이력의 조회 기간·페이지 정보가 올바르지 않습니다."));
        path = "/api/v1/orders"; parameter("status", "CLOSED"); parameter("limit", "100");
        const QTimeZone kst("Asia/Seoul");
        if (input.startTimeMs) parameter("from", QDateTime::fromMSecsSinceEpoch(input.startTimeMs, kst).date().toString(Qt::ISODate));
        if (input.endTimeMs) parameter("to", QDateTime::fromMSecsSinceEpoch(input.endTimeMs, kst).date().toString(Qt::ISODate));
        if (!input.cursor.isEmpty()) parameter("cursor", input.cursor);
        break;
    }
    case Operation::QueryOrder:

     {
        if (!orderId(input.orderId)) return reject(QStringLiteral("서버가 발급한 주문 ID가 필요합니다. clientOrderId 조회는 지원되지 않습니다."));
        path = "/api/v1/orders/" + QUrl::toPercentEncoding(input.orderId);

        break;
    }
    default: return reject(QStringLiteral("지원하지 않는 토스 요청입니다."));
    }

    if (request.method == "POST") { request.headers.insert("Content-Type", "application/json"); request.body = QJsonDocument(body).toJson(QJsonDocument::Compact); }
    QByteArray authorization("Bearer "); authorization.append(reinterpret_cast<const char*>(token_.data()), qsizetype(token_.size()));
    request.headers.insert("Authorization", authorization); authorization.clear();
    if (accountOperation(op)) request.headers.insert("X-Tossinvest-Account", QByteArray(reinterpret_cast<const char*>(selectedSequence_.data()), qsizetype(selectedSequence_.size())));
    request.url = QUrl::fromEncoded("https://openapi.tossinvest.com" + path + (query.isEmpty() ? QByteArray() : '?' + query));
    return request;
}

AdapterResult TossAdapter::parse(Operation op, const HttpResult& response, const OrderInput& input) const {
    if (op == Operation::Holdings) { marketHoldings_.clear(); marketHoldingsAt_ = -1; }
    const auto malformed = [&] { return failure(op, "invalid-response", QStringLiteral("토스 응답을 안전하게 해석하지 못했습니다."), true); };
    if (op == Operation::Authenticate) clearSessionState();
    // An intermediary may send HTML or no body on 401. HTTP authentication
    // failure must invalidate the session before attempting JSON decoding.
    if (response.status == 401) {
        clearSessionState();
        auto result = failure(op, "authentication-failed", QStringLiteral("토스 인증이 만료되었거나 무효화됐습니다. 다른 기기의 연결을 확인한 뒤 다시 연결하세요."), true);
        result.authenticationFailed = true; return result;
    }
    if (response.timedOut || response.networkError)
        return failure(op, "network-error", QStringLiteral("토스 응답을 받지 못했습니다. 주문은 재전송하지 않고 상태를 확인하세요."), true);

    QString parseError; const auto doc = parseExactJson(response.body, &parseError);
    if (!parseError.isEmpty() || !doc.isObject()) return malformed();
    const auto root = doc.object();
    if (response.status != 200 || root.contains("error")) {
        QString remoteCode = op == Operation::Authenticate ? root.value("error").toString() : string(root.value("error").toObject(), "code");
        if (!match(remoteCode, "\\A[a-z0-9_-]{1,80}\\z")) remoteCode = "api-error";
        auto r = failure(op, remoteCode, QStringLiteral("토스 요청이 처리되지 않았습니다. 연결 상태와 주문 상태를 확인하세요."),
            response.status < 400 || response.status >= 500 || remoteCode == "request-in-progress" || remoteCode == "already-processing");
        r.authenticationFailed = response.status == 401;
        if (r.authenticationFailed || op == Operation::Authenticate) clearSessionState();
        r.notFound = response.status == 404 && remoteCode == "order-not-found";
        r.rateLimited = response.status == 429;
        if (r.rateLimited) { bool ok = false; const int seconds = response.headers.value("retry-after", response.headers.value("Retry-After")).toInt(&ok); r.retryAfterSeconds = ok && seconds >= 1 && seconds <= 3600 ? seconds : 1; }
        if (remoteCode == "ip-not-allowed" || remoteCode == "access_denied") r.message = QStringLiteral("토스 공식 설정에서 현재 공인 IP의 허용 여부를 확인하세요.");
        if (r.authenticationFailed) r.message = QStringLiteral("토스 인증이 만료되었거나 무효화됐습니다. 다른 기기의 연결을 확인한 뒤 다시 연결하세요.");
        return r;
    }
    AdapterResult result;
    if (op == Operation::Authenticate) {
        clearSessionState();
        QString token = string(root, "access_token"); const auto expiry = string(root, "expires_in");
        bool ok = false; const qint64 seconds = expiry.toLongLong(&ok);
        // RFC 6750 b64token is header-safe; no JWT claim is trusted locally.
        if (token.size() > 16384 || !match(token, "\\A[A-Za-z0-9._~+/-]+=*\\z") ||
            string(root, "token_type") != "Bearer" || !integer(expiry) || !ok || seconds < 1 || seconds > 30 * 24 * 3600) { wipe(token); return malformed(); }
        token_ = SecureBuffer::fromUtf8(token); wipe(token);
        sessionLifetimeMs_ = seconds * 1000 - qMin<qint64>(5000, seconds * 100); sessionClock_.start();
        result.ok = true; return result;
    }
    if (!root.contains("result")) return malformed();
    const auto payload = root.value("result");
    if(op==Operation::AlgoOpenOrders||op==Operation::AlgoQueryOrder){
        QJsonArray rows;if(op==Operation::AlgoQueryOrder){if(!payload.isObject())return malformed();rows.append(payload);}
        else{
            const auto page=payload.toObject();if(!payload.isObject()||!page.value("conditionalOrders").isArray()||!page.value("hasNext").isBool())return malformed();rows=page.value("conditionalOrders").toArray();
            result.hasMore=page.value("hasNext").toBool();result.nextCursor=string(page,"nextCursor");
            if(result.hasMore? !match(result.nextCursor,"\\A[A-Za-z0-9_-]{1,256}\\z"):!page.value("nextCursor").isNull())return malformed();
        }
        if(rows.size()>100)return malformed();QSet<QString> ids;
        for(const auto& value:rows){if(!value.isObject())return malformed();const auto o=value.toObject();OrderRecord row;row.orderId=string(o,"conditionalOrderId");row.symbol=string(o,"symbol");row.quantity=string(o,"quantity");row.type=string(o,"orderType").toLower();row.rawStatus=string(o,"status");
            const auto group=string(o,"type"),market=string(o,"market");
            if(!orderId(row.orderId)||ids.contains(row.orderId)||!symbol(row.symbol)||!positive(row.quantity)||!QStringList{"limit","market"}.contains(row.type)||!QStringList{"SINGLE","OCO","OTO"}.contains(group)||!QStringList{"WATCHING","PAUSED","ORDERING","ORDERED","COMPLETED","EXPIRED"}.contains(row.rawStatus)||!QStringList{"KR","US"}.contains(market)||instant(o.value("createdAt"))<=0)return malformed();
            ids.insert(row.orderId);QJsonObject terms{{"type",group},{"expireDate",o.value("expireDate")}};bool safeCancel=true,allCanceled=true;int legs=group=="SINGLE"?1:2;
            for(int n=0;n<legs;++n){const auto name=n==0?"first":"second";const auto leg=o.value(name).toObject();const auto state=string(leg,"status"),side=string(leg,"orderSide"),type=string(leg,"type");
                if(!o.value(name).isObject()||!QStringList{"STOP","PROFIT_RATE"}.contains(type)||!QStringList{"BUY","SELL"}.contains(side)||!QStringList{"WATCHING","HOLDING","PAUSED","ORDERING","ORDERED","COMPLETED","EXPIRED","CANCELED"}.contains(state))return malformed();
                QJsonObject clean{{"type",type},{"status",state},{"orderSide",side}};
                for(const auto* field:{"triggerPrice","orderPrice","targetProfitRate","triggeredOrderId"}){const auto v=leg.value(field);if(!v.isUndefined()&&!v.isNull()){
                    if(!v.isString()||(QByteArray(field)=="triggeredOrderId"?!orderId(v.toString()):QByteArray(field)=="targetProfitRate"?!signedNumber(v.toString()):!positive(v.toString())))return malformed();clean[field]=v;}}
                if(type=="STOP"&&!positive(string(clean,"triggerPrice")))return malformed();if(row.type=="limit"&&!positive(string(clean,"orderPrice")))return malformed();
                safeCancel&=QStringList{"WATCHING","HOLDING","PAUSED"}.contains(state)&&!clean.contains("triggeredOrderId");allCanceled&=state=="CANCELED";terms[name]=clean;
                if(n==0){row.side=side.toLower();row.price=row.type=="limit"?string(clean,"orderPrice"):QString{};}
            }
            if(group=="SINGLE"&&!o.value("second").isUndefined()&&!o.value("second").isNull())return malformed();
            row.brokerConditional=true;row.conditions=terms;row.market=market=="KR"?Domestic:Foreign;row.currency=market=="KR"?"KRW":"USD";
            row.status=allCanceled?"canceled":row.rawStatus=="WATCHING"?"watch":row.rawStatus=="PAUSED"?"paused":row.rawStatus=="ORDERING"?"condition_ordering":row.rawStatus=="ORDERED"?"triggered":row.rawStatus=="EXPIRED"?"expired":row.rawStatus=="COMPLETED"?"condition_completed":"unknown";
            row.cancelable=market=="KR"&&safeCancel&&(row.rawStatus=="WATCHING"||row.rawStatus=="PAUSED");
            if(op==Operation::AlgoQueryOrder&&row.orderId!=input.orderId)return malformed();result.orders.append(row);
        }
        result.ok=true;result.data={{"complete",!result.hasMore},{"coverage","별도 조건 감시 · 발생한 일반 주문의 취소는 포함하지 않음"}};return result;
    }
    if (op == Operation::Accounts) {
        if (!sessionReady() || !payload.isArray() || payload.toArray().size() > 64) return malformed();
        std::vector<AccountEntry> fresh; QSet<qint64> seen;
        for (const auto& v : payload.toArray()) {
            if (!v.isObject()) return malformed(); const auto o = v.toObject();
            QString seq = string(o, "accountSeq"), numberValue = string(o, "accountNo"); const auto type = string(o, "accountType");
            const auto clearIdentityText = qScopeGuard([&] { wipe(seq); wipe(numberValue); });
            bool ok = false; const qint64 sequence = seq.toLongLong(&ok);
            if (!integer(seq) || !ok || sequence <= 0 || seen.contains(sequence) || !match(numberValue, "\\A[0-9-]{4,32}\\z") || !code(type)) return malformed();
            seen.insert(sequence);
            if (type == "BROKERAGE") {
                AccountEntry entry; entry.choice.handle = QUuid::createUuid().toString(QUuid::WithoutBraces);
                entry.choice.label = QStringLiteral("종합매매 · ••••%1").arg(numberValue.size() > 4 ? numberValue.right(4) : QString());
                QString canonicalSequence = QString::number(sequence);
                const auto clearCanonicalSequence = qScopeGuard([&] { wipe(canonicalSequence); });
                entry.sequence = SecureBuffer::fromUtf8(canonicalSequence);
                entry.identityScope = accountIdentityScope(numberValue, type, entry.sequence);
                fresh.push_back(std::move(entry));
            }
        }
        accounts_ = std::move(fresh); selectedSequence_.clear(); selectedScope_.clear(); selectedLabel_.clear();
        marketHoldings_.clear(); marketHoldingsAt_ = -1;
        result.ok = true; return result;
    }
    if (op == Operation::Markets) {
        if (marketOf(input.cursor) != Domestic || !payload.isArray() || payload.toArray().size() > 20000) return malformed();
        QJsonArray rows; QSet<QString> seen;
        for (const auto& v : payload.toArray()) {
            const auto o = v.toObject(); const auto s = string(o, "symbol"), name = string(o, "name"), securityType = string(o, "securityType");
            if (!v.isObject() || !krSymbol(s) || !safeText(name) || !code(securityType) || !o.value("isCommonShare").isBool() ||
                !safeText(string(o, "isinCode"), 32) || seen.contains(s)) return malformed();
            seen.insert(s);
            rows.append(QJsonObject{{"symbol", s}, {"name", name}, {"englishName", ""}, {"baseAsset", s},
                {"quoteAsset", "KRW"}, {"status", "ACTIVE"}, {"tradable", true}, {"securityType", securityType}});
        }
        for (const auto& s : seen) symbolMarkets_.insert(s, Domestic);
        result.data = {{"market", input.cursor}, {"instruments", rows}};
    } else if (op == Operation::Rules) {
        if (!payload.isArray() || payload.toArray().size() != 1) return malformed();
        const auto o = payload.toArray().first().toObject();
        if (string(o, "symbol") != input.symbol || !krSymbol(input.symbol) || marketOf(string(o, "market")) != Domestic ||
            string(o, "currency") != "KRW" || !safeText(string(o, "name")) || !o.value("englishName").isString() ||
            (!string(o, "englishName").isEmpty() && !safeText(string(o, "englishName"))) ||
            !code(string(o, "securityType")) || !code(string(o, "status")) || !o.value("koreanMarketDetail").isObject()) return malformed();
        const auto d = o.value("koreanMarketDetail").toObject();
        if (!d.value("liquidationTrading").isBool() || !d.value("nxtSupported").isBool() || !d.value("krxTradingSuspended").isBool() ||
            !(d.value("nxtTradingSuspended").isUndefined() || d.value("nxtTradingSuspended").isNull() || d.value("nxtTradingSuspended").isBool())) return malformed();
        for (const auto* key : {"symbol", "name", "englishName", "market", "currency", "securityType", "status"}) result.data.insert(QLatin1String(key), o.value(QLatin1String(key)));
        QJsonObject detail; for (const auto* key : {"liquidationTrading", "nxtSupported", "krxTradingSuspended", "nxtTradingSuspended"}) detail.insert(QLatin1String(key), d.value(QLatin1String(key)));
        result.data.insert("koreanMarketDetail", detail); symbolMarkets_.insert(input.symbol, Domestic);
    } else if (op == Operation::Quote) {
        if (!payload.isArray() || payload.toArray().size() != 1) return malformed();
        const auto o = payload.toArray().first().toObject();
        const auto timestamp = o.value("timestamp").isUndefined() ? QJsonValue(QJsonValue::Null) : o.value("timestamp");
        if (string(o, "symbol") != input.symbol || string(o, "currency") != "KRW" || !number(string(o, "lastPrice")) || !nullableTime(timestamp)) return malformed();
        result.data = {{"symbol", input.symbol}, {"currency", "KRW"}, {"price", o.value("lastPrice")}, {"lastPrice", o.value("lastPrice")},
            {"timestamp", timestamp}, {"timestampMs", QString::number(instant(timestamp))}};
    } else if (op == Operation::Depth) {
        if (!payload.isObject()) return malformed(); const auto o = payload.toObject();
        const auto timestamp = o.value("timestamp").isUndefined() ? QJsonValue(QJsonValue::Null) : o.value("timestamp");
        if (string(o, "currency") != "KRW" || !nullableTime(timestamp)) return malformed();
        result.data = {{"currency", "KRW"}, {"timestamp", timestamp}, {"timestampMs", QString::number(instant(timestamp))}};
        for (const auto* key : {"asks", "bids"}) {
            const auto v = o.value(QLatin1String(key)); if (!v.isArray() || v.toArray().size() > 1000) return malformed();
            QJsonArray rows; QString previous;
            for (const auto& row : v.toArray()) {
                const auto b = row.toObject(); const auto price = string(b, "price"), quantity = string(b, "volume");
                if (!row.isObject() || !positive(price) || !number(quantity) || (!previous.isEmpty() &&
                    (QByteArray(key) == "asks" ? decimal::compare(price, previous) < 0 : decimal::compare(price, previous) > 0))) return malformed();
                previous = price; rows.append(QJsonObject{{"price", price}, {"quantity", quantity}});
            }
            result.data.insert(QLatin1String(key), rows);
            result.data.insert(QByteArray(key) == "asks" ? "ask" : "bid", rows.isEmpty() ? QJsonValue(QJsonValue::Null) : rows.first().toObject().value("price"));
        }
        // Integrated KRX/NXT books may legitimately cross; do not reject that.
    } else if (op == Operation::TradingSchedule) {
        if (!payload.isObject()) return malformed();
        for (const auto* k : {"today", "previousBusinessDay", "nextBusinessDay"}) {
            QJsonObject clean; if (!payload.toObject().value(QLatin1String(k)).isObject() || !calendarDay(payload.toObject().value(QLatin1String(k)).toObject(), &clean)) return malformed();
            result.data.insert(QLatin1String(k), clean);
        }
    } else if (op == Operation::PriceLimits) {
        const auto o = payload.toObject();
        const auto high = o.value("upperLimitPrice").isUndefined() ? QJsonValue(QJsonValue::Null) : o.value("upperLimitPrice");
        const auto low = o.value("lowerLimitPrice").isUndefined() ? QJsonValue(QJsonValue::Null) : o.value("lowerLimitPrice");
        if (!payload.isObject() || string(o, "currency") != "KRW" || instant(o.value("timestamp")) <= 0 ||
            !nullableNumber(high) || !nullableNumber(low)) return malformed();
        result.data = {{"currency", "KRW"}, {"timestamp", o.value("timestamp")}, {"upperLimitPrice", high}, {"lowerLimitPrice", low}};
    } else if (op == Operation::BuyingPower) {
        const auto o = payload.toObject();
        if (!payload.isObject() || string(o, "currency") != "KRW" || !integer(string(o, "cashBuyingPower"))) return malformed();
        result.data = {{"currency", "KRW"}, {"cashBuyingPower", o.value("cashBuyingPower")}};
    } else if (op == Operation::SellableQuantity) {
        const auto o = payload.toObject(); if (!payload.isObject() || !integer(string(o, "sellableQuantity"))) return malformed();
        result.data = {{"symbol", input.symbol}, {"sellableQuantity", o.value("sellableQuantity")}};
    } else if (op == Operation::Commissions) {
        if (!payload.isArray()) return malformed(); bool found = false;
        for (const auto& v : payload.toArray()) {
            const auto o = v.toObject(); if (string(o, "marketCountry") != "KR") continue;
            const auto rate = string(o, "commissionRate"); if (found || !number(rate) || decimal::compare(rate, "1") > 0) return malformed();
            result.data = {{"marketCountry", "KR"}, {"currency", "KRW"}, {"commissionRate", rate}};
            for (const auto* k : {"startDate", "endDate"}) {
                const auto date = o.value(QLatin1String(k)); if (!date.isNull() && (!date.isString() || !QDate::fromString(date.toString(), Qt::ISODate).isValid())) return malformed();
                result.data.insert(QLatin1String(k), date);
            }
            found = true;
        }
        if (!found) return malformed();
    } else if (op == Operation::Holdings) {
        const auto o = payload.toObject(); if (!payload.isObject() || !o.value("items").isArray() || o.value("items").toArray().size() > 20000) return malformed();
        QJsonArray rows; QSet<QString> seen; QHash<QString, QString> markets; QStringList positiveHoldings;
        for (const auto& v : o.value("items").toArray()) {
            const auto h = v.toObject(); const auto s = string(h, "symbol"), country = string(h, "marketCountry"), currency = string(h, "currency");
            if (!v.isObject() || !symbol(s) || seen.contains(s) || !safeText(string(h, "name")) ||
                !((country == "KR" && currency == "KRW" && krSymbol(s)) || (country == "US" && currency == "USD"))) return malformed();
            seen.insert(s); markets.insert(s, country == "KR" ? Domestic : Foreign);
            QJsonObject item;
            for (const auto* k : {"symbol", "name", "marketCountry", "currency"}) item.insert(QLatin1String(k), h.value(QLatin1String(k)));
            if (!numericObject(h, &item, {"quantity", "lastPrice", "averagePurchasePrice"})) return malformed();
            for (const auto& spec : QList<QPair<QString, QStringList>>{{"marketValue", {"purchaseAmount", "amount", "amountAfterCost"}},
                    {"profitLoss", {"amount", "amountAfterCost", "rate", "rateAfterCost"}}, {"dailyProfitLoss", {"amount", "rate"}}, {"cost", {"commission"}}}) {
                QJsonObject clean; if (!h.value(spec.first).isObject() || !numericObject(h.value(spec.first).toObject(), &clean, spec.second, spec.first.contains("ProfitLoss") || spec.first == "profitLoss")) return malformed();
                if (spec.first == "cost") { auto tax = h.value("cost").toObject().value("tax"); if (tax.isUndefined()) tax = QJsonValue::Null; if (!nullableNumber(tax)) return malformed(); clean.insert("tax", tax); }
                item.insert(spec.first, clean);
            }
            if (country == "KR") rows.append(item);
            if (decimal::positive(item.value("quantity").toString())) positiveHoldings.append(s);
        }
        for (auto it = markets.begin(); it != markets.end(); ++it) symbolMarkets_.insert(it.key(), it.value());
        marketHoldings_ = positiveHoldings; marketHoldingsAt_ = sessionClock_.elapsed();
        result.data = {{"items", rows}, {"scope", "KR"}}; result.scope = "KR";
    } else if (op == Operation::OpenOrders || op == Operation::History || op == Operation::QueryOrder) {
        QJsonArray rows;
        if (op == Operation::QueryOrder) { if (!payload.isObject()) return malformed(); rows.append(payload); }
        else {
            const auto o = payload.toObject(); if (!payload.isObject() || !o.value("orders").isArray() || !o.value("hasNext").isBool()) return malformed();
            const auto cursor = o.value("nextCursor"); if (!cursor.isNull() && (!cursor.isString() || !safeText(cursor.toString(), 2048))) return malformed();
            result.hasMore = o.value("hasNext").toBool(); result.nextCursor = cursor.toString();
            if (result.hasMore != !result.nextCursor.isEmpty() || (op == Operation::OpenOrders && result.hasMore)) return malformed();
            rows = o.value("orders").toArray(); if (rows.size() > (op == Operation::History ? 100 : 20000)) return malformed();
        }
        QSet<QString> seen;
        for (const auto& v : rows) {
            OrderRecord record;
            if (!v.isObject() || !decodeOrder(v.toObject(), symbolMarkets_, op == Operation::History, &record) || seen.contains(record.orderId)) return malformed();
            if (op == Operation::QueryOrder && record.orderId != input.orderId) return malformed();
            seen.insert(record.orderId); result.orders.append(record);
        }
        result.scope = QStringLiteral("계좌 범위 · API 제공 호가 유형만 · 시장 미확인/해외 주문 변경 제외");
    } else  return failure(op, "unsupported-operation", QStringLiteral("이 토스 응답 유형은 지원하지 않습니다."));
    result.ok = true; return result;
}

OrderValidation TossAdapter::validate(const OrderInput& input, const MarketSnapshot& snapshot, const OrderLimits& limits) const {
    const auto deny = [](const QString& message) { OrderValidation v; v.message = message; return v; };
    if (!sessionReady() || selectedSequence_.empty()) return deny(QStringLiteral("유효한 토스 연결과 선택 계좌가 필요합니다."));
    if (!krSymbol(input.symbol) || (input.side != "buy" && input.side != "sell") || (input.type != "limit" && input.type != "market") ||
        !integer(input.quantity) || !positive(input.quantity) || !input.quoteAmount.isEmpty()) return deny(QStringLiteral("국내 수동 주문의 종목·방향·정수 수량을 확인하세요."));
    const auto rules = snapshot.data.value(Operation::Rules);
    const auto detail = rules.value("koreanMarketDetail").toObject();
    if (string(rules, "symbol") != input.symbol || marketOf(string(rules, "market")) != Domestic || string(rules, "currency") != "KRW" ||
        string(rules, "status") != "ACTIVE" || !detail.value("liquidationTrading").isBool() || !detail.value("krxTradingSuspended").isBool() ||
        !detail.value("nxtSupported").isBool() || (detail.value("nxtSupported").toBool() && !detail.value("nxtTradingSuspended").isBool()) ||
        detail.value("liquidationTrading").toBool() || detail.value("krxTradingSuspended").toBool() || detail.value("nxtTradingSuspended").toBool())
        return deny(QStringLiteral("국내 종목의 현재 거래 가능 상태를 확인할 수 없습니다."));
    const auto today = snapshot.data.value(Operation::TradingSchedule).value("today").toObject();
    const auto regular = today.value("integrated").toObject().value("regularMarket").toObject();
    const qint64 now = QDateTime::currentMSecsSinceEpoch(), start = instant(regular.value("startTime")), end = instant(regular.value("endTime"));
    const qint64 auction = instant(regular.value("singlePriceAuctionStartTime"));
    if (string(today, "date") != QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Seoul")).date().toString(Qt::ISODate) ||
        !start || !end || !auction || auction <= start || auction > end || now < start || now >= auction)
        return deny(QStringLiteral("현재는 확인된 국내 정규장 연속매매 시간에만 주문할 수 있습니다."));
    const auto quote = snapshot.data.value(Operation::Quote), depth = snapshot.data.value(Operation::Depth);
    const QString last = string(quote, "lastPrice");
    if (string(quote, "symbol") != input.symbol || string(quote, "currency") != "KRW" || string(depth, "currency") != "KRW" || !positive(last))
        return deny(QStringLiteral("국내 시세·호가를 다시 조회하세요."));
    const auto depthTime = instant(depth.value("timestamp"));
    if (!depthTime || depthTime > now + 5000 || now - depthTime > 30000)
        return deny(QStringLiteral("호가의 원본 시각이 없거나 오래됐습니다. 조회만 가능하며 주문은 중지합니다."));
    QString price = input.price, upperValuePrice = input.price;
    if (input.type == "limit") {
        const auto step = tick(price, string(rules, "securityType"));
        if (!integer(price) || !positive(price) || step.isEmpty() || !decimal::multipleOf(price, step)) return deny(QStringLiteral("지원 종목 유형과 원화 호가 단위를 확인하세요."));
    } else {
        if (!price.isEmpty()) return deny(QStringLiteral("시장가 주문에는 가격을 입력하지 마세요."));
        const auto book = depth.value(input.side == "buy" ? "asks" : "bids").toArray(); QString left = input.quantity;
        for (const auto& v : book) {
            const auto row = v.toObject(); const auto volume = string(row, "quantity"), p = string(row, "price");
            if (!positive(p) || !number(volume)) return deny(QStringLiteral("호가 수량을 확인할 수 없습니다."));
            if (!positive(volume)) continue;
            price = p;
            // Sell books descend: the final bid is adverse, but understates the value bound.
            if (upperValuePrice.isEmpty() || decimal::compare(p, upperValuePrice) > 0) upperValuePrice = p;
            if (decimal::compare(volume, left) >= 0) { left = "0"; break; }
            left = decimal::subtract(left, volume);
        }
        if (!positive(price) || decimal::compare(left, "0") != 0) return deny(QStringLiteral("시장가 수량을 검증할 호가 잔량이 부족합니다."));
        if (tick(price, string(rules, "securityType")).isEmpty()) return deny(QStringLiteral("이 종목 유형의 주문 검증은 아직 지원하지 않습니다."));
    }
    const auto range = snapshot.data.value(Operation::PriceLimits); const auto low = string(range, "lowerLimitPrice"), high = string(range, "upperLimitPrice");
    if (string(range, "currency") != "KRW" || !positive(low) || !positive(high) || decimal::compare(low, high) > 0 ||
        decimal::compare(price, low) < 0 || decimal::compare(price, high) > 0 || decimal::compare(upperValuePrice, high) > 0)
        return deny(QStringLiteral("당일 상·하한가를 확인하거나 주문 가격을 조정하세요."));
    const QString notional = decimal::multiply(upperValuePrice, input.quantity);
    if (decimal::compare(notional, "100000000") >= 0) return deny(QStringLiteral("1억원 이상 주문의 금액 확인 흐름은 아직 구현되지 않았습니다."));
    if (limits.currency != "KRW" || !positive(limits.maxNotional) || !number(limits.maxDeviationPercent) ||
        decimal::compare(notional, limits.maxNotional) > 0) return deny(QStringLiteral("원화 주문 한도를 먼저 설정하거나 주문 금액을 줄이세요."));
    const auto difference = decimal::compare(price, last) >= 0 ? decimal::subtract(price, last) : decimal::subtract(last, price);
    if (decimal::compare(decimal::multiply(difference, "100"), decimal::multiply(last, limits.maxDeviationPercent)) > 0)
        return deny(QStringLiteral("설정한 현재가 대비 가격 괴리율을 초과했습니다."));
    if (!input.orderId.isEmpty()) {
        bool found = false;
        for (const auto& original : snapshot.openOrders) if (original.orderId == input.orderId) {
            found = true;
            if (!original.amendable || original.market != Domestic || original.symbol != input.symbol || original.side != input.side ||
                original.type != "limit" || input.type != "limit" || !number(original.executed) || decimal::compare(original.executed, "0") != 0)
                return deny(QStringLiteral("부분 체결·시장 미확인·미지원 주문은 정정할 수 없습니다."));
        }
        if (!found) return deny(QStringLiteral("정정할 원주문을 다시 조회하세요."));
    } else for (const auto& order : snapshot.openOrders) {
        if (order.symbol == input.symbol && order.side != input.side &&
            (order.rawStatus == "PENDING" || order.rawStatus == "PARTIAL_FILLED" || order.rawStatus == "PENDING_CANCEL" || order.rawStatus == "PENDING_REPLACE"))
            return deny(QStringLiteral("같은 종목의 반대 방향 미체결 주문을 먼저 확인하세요."));
    }
    if (input.side == "buy") {
        const auto power = snapshot.data.value(Operation::BuyingPower), fees = snapshot.data.value(Operation::Commissions);
        const auto cash = string(power, "cashBuyingPower"), rate = string(fees, "commissionRate");
        if (string(power, "currency") != "KRW" || !number(cash) || !number(rate) || decimal::compare(rate, "1") > 0 ||
            decimal::compare(decimal::add(notional, decimal::multiply(notional, rate)), cash) > 0)
            return deny(QStringLiteral("수수료를 포함한 주문 가능 금액이 부족하거나 확인되지 않았습니다."));
    } else {
        const auto sellable = snapshot.data.value(Operation::SellableQuantity); const auto quantity = string(sellable, "sellableQuantity");
        if (string(sellable, "symbol") != input.symbol || !integer(quantity) || decimal::compare(input.quantity, quantity) > 0)
            return deny(QStringLiteral("매도 가능 수량이 부족하거나 확인되지 않았습니다."));
    }
    OrderValidation result; result.ok = true; result.currency = "KRW"; result.notional = notional; return result;
}

BrokerReconciliation TossAdapter::reconcile(const QString& kind, const OrderInput& intent, const QList<OrderRecord>& observed) const {
    BrokerReconciliation result; result.message = QStringLiteral("서버 주문 상태가 확정될 때까지 자동 재전송하지 않습니다.");
    if(intent.brokerConditional){
        if(kind!="cancel"||observed.size()!=1)return result;const auto& row=observed.first();
        if(row.brokerConditional&&row.orderId==intent.orderId&&row.market==Domestic&&row.symbol==intent.symbol&&row.status=="canceled"){
            result.resolved=result.applied=true;result.message="조건 감시의 취소 상태를 확인했습니다. 발생한 일반 주문은 별도로 확인하세요.";
        }return result;
    }
    const QString receipt = intent.operationOrderId.isEmpty() && kind == "place" ? intent.orderId : intent.operationOrderId;
    if (!orderId(receipt)) return result; // clientOrderId cannot be queried or inferred.
    const auto matchesIntent = [&](const OrderRecord& row, bool compareTerms) {
        if (row.market != Domestic || row.currency != "KRW" || row.symbol != intent.symbol || row.side != intent.side || row.type != intent.type) return false;
        if (!compareTerms) return true;
        if (!number(row.quantity) || !number(intent.quantity) || decimal::compare(row.quantity, intent.quantity) != 0) return false;
        if (intent.type == "market") return row.price.isEmpty() && intent.price.isEmpty();
        return intent.type == "limit" && positive(row.price) && positive(intent.price) && decimal::compare(row.price, intent.price) == 0;
    };
    const OrderRecord* operation = nullptr;
    const OrderRecord* original = nullptr;
    for (const auto& order : observed) if (order.orderId == receipt) {
        if (operation) return result; operation = &order;
    }
    if (!operation || !matchesIntent(*operation, true)) return result;
    if (kind == "cancel" || kind == "amend") {
        if (!orderId(intent.orderId) || receipt == intent.orderId) return result;
        for (const auto& row : observed) if (row.orderId == intent.orderId) {
            if (original) return result; original = &row;
        }
        if (!original || !matchesIntent(*original, kind == "cancel")) return result;
    }
    const auto state = operation->rawStatus;
    if (kind == "place") {
        if (QStringList{"PENDING", "PARTIAL_FILLED", "FILLED", "CANCELED", "REPLACED", "REJECTED"}.contains(state)) {
            result.resolved = true; result.applied = state != "REJECTED" || (number(operation->executed) && positive(operation->executed));
        }
    } else if (kind == "cancel") {
        if (state == "CANCELED" && original->rawStatus == "CANCELED") { result.resolved = true; result.applied = true; }
        else if ((state == "CANCEL_REJECTED" || state == "REJECTED") &&
                 original->rawStatus != "PENDING_CANCEL" && original->rawStatus != "PENDING_REPLACE" && original->status != "unknown") result.resolved = true;
    } else if (kind == "amend") {
        if ((state == "REPLACE_REJECTED" || state == "REJECTED") && original->rawStatus != "REPLACED" &&
            original->rawStatus != "PENDING_REPLACE" && original->rawStatus != "PENDING_CANCEL" && original->status != "unknown") result.resolved = true;
        else if (original->rawStatus == "REPLACED" && QStringList{"PENDING", "PARTIAL_FILLED", "FILLED", "CANCELED", "REPLACED"}.contains(state)) {
            result.resolved = true; result.applied = true;
        }
    }
    if (result.resolved) result.message = result.applied ? QStringLiteral("해당 서버 주문 ID로 처리 상태를 확인했습니다.") : QStringLiteral("해당 서버 주문 ID로 요청 거부를 확인했습니다.");
    return result;
}
} // namespace keefi
