#include "keefi/FundFlowProtocol.h"
#include "keefi/BinanceEarnProtocol.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/UpbitAdapter.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>
#include <sodium.h>

namespace keefi {
namespace {
constexpr qint64 LastTime = 253402300799999LL;
QString text(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
QString amount(const QJsonObject& o, const char* key) {
    static const QRegularExpression pattern("\\A[0-9]{1,40}(?:\\.[0-9]{1,40})?\\z");
    const auto s = text(o, key);
    return pattern.match(s).hasMatch() ? decimal::normalize(s) : QString{};
}
qint64 millis(const QJsonObject& o, const char* key) {
    const auto s = text(o, key); bool ok = false; const auto n = s.toLongLong(&ok);
    return ok && n > 0 && n <= LastTime && QString::number(n) == s ? n : 0;
}
int small(const QJsonObject& o, const char* key, int fallback = -1) {
    const auto s = text(o, key); bool ok = false; const auto n = s.toInt(&ok);
    return ok && QString::number(n) == s && n >= 0 && n <= 99 ? n : fallback;
}
// Upbit lists carry an explicit offset (KST). A timestamp without one is
// ambiguous and is rejected rather than guessed.
qint64 upbitTime(const QJsonValue& value) {
    static const QRegularExpression pattern("\\A\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(?:\\.\\d{1,9})?(?:Z|[+-]\\d{2}:\\d{2})\\z");
    if (!value.isString() || !pattern.match(value.toString()).hasMatch()) return 0;
    const auto at = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    return at.isValid() ? at.toMSecsSinceEpoch() : 0;
}
// Binance withdrawal times are documented as UTC text.
qint64 utcText(const QJsonObject& o, const char* key) {
    static const QRegularExpression pattern("\\A\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\z");
    const auto s = text(o, key);
    if (!pattern.match(s).hasMatch()) return 0;
    auto at = QDateTime::fromString(s, "yyyy-MM-dd HH:mm:ss"); at.setTimeZone(QTimeZone::UTC);
    return at.isValid() ? at.toMSecsSinceEpoch() : 0;
}
bool token(const QString& s, int max = 128) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9_.:/-]{1,128}\\z");
    return s.size() <= max && pattern.match(s).hasMatch();
}
bool credential(const SecureBuffer& value, bool binance) {
    if (value.empty() || value.size() > (binance ? 512u : 8192u)) return false;
    if (!binance) return true;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = value.data()[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}
bool parseUpbit(const FlowQuery& q, const QJsonObject& o, qint64 received, FlowRecord* r) {
    const bool deposit = q.source == FlowSource::UpbitDeposits;
    static const QRegularExpression uuid("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");
    if (text(o, "type") != (deposit ? "deposit" : "withdraw") || !uuid.match(text(o, "uuid")).hasMatch()) return false;
    r->id = text(o, "uuid"); r->asset = text(o, "currency"); r->state = text(o, "state");
    r->amount = amount(o, "amount"); r->fee = amount(o, "fee"); r->type = text(o, "transaction_type");
    if (!binanceIdentifierValid(r->asset) || r->amount.isEmpty() || r->fee.isEmpty() || !token(r->state, 40) ||
        (r->type != "default" && r->type != "internal")) return false;
    r->internal = r->type == "internal";
    r->createdMs = upbitTime(o.value("created_at"));
    if (!r->createdMs || r->createdMs > received + 2000) return false;
    const auto done = o.value("done_at");
    if (!done.isNull() && !done.isUndefined()) { r->doneMs = upbitTime(done); if (!r->doneMs || r->doneMs > received + 2000) return false; }
    const QStringList success = deposit ? QStringList{"ACCEPTED"} : QStringList{"DONE"};
    const QStringList closed = deposit ? QStringList{"CANCELLED", "REJECTED", "REFUNDED"} : QStringList{"FAILED", "CANCELLED", "REJECTED"};
    // A completed row must say when it completed; otherwise it stays pending.
    r->success = success.contains(r->state) && r->doneMs > 0;
    r->final = r->success || closed.contains(r->state);
    if (!r->final) r->pendingRef = r->id;
    return true;
}
bool parseBinance(const FlowQuery& q, const QJsonObject& o, qint64 received, FlowRecord* r) {
    if (q.source == FlowSource::BinanceTransfers) {
        r->id = text(o, "tranId"); r->asset = text(o, "asset"); r->amount = amount(o, "amount");
        r->state = text(o, "status"); r->type = text(o, "type"); r->createdMs = millis(o, "timestamp");
        if (!token(r->id, 40) || !binanceIdentifierValid(r->asset) || r->amount.isEmpty() || r->type != q.transferType ||
            !QStringList{"CONFIRMED", "FAILED", "PENDING"}.contains(r->state) || !r->createdMs || r->createdMs > received + 2000) return false;
        r->success = r->state == "CONFIRMED"; r->final = r->state != "PENDING";
        if (r->success) r->doneMs = r->createdMs;
        r->fee = "0"; return true;
    }
    const bool deposit = q.source == FlowSource::BinanceDeposits;
    r->id = text(o, "id"); r->asset = text(o, "coin"); r->amount = amount(o, "amount");
    r->fee = deposit ? QStringLiteral("0") : amount(o, "transactionFee");
    const int status = small(o, "status"), wallet = small(o, "walletType", 0), transfer = small(o, "transferType", 0);
    if (!token(r->id, 80) || !binanceIdentifierValid(r->asset) || r->amount.isEmpty() || r->fee.isEmpty() ||
        status < 0 || wallet < 0 || wallet > 1 || transfer < 0 || transfer > 1) return false;
    r->state = QString::number(status); r->wallet = wallet; r->internal = transfer == 1;
    if (deposit) {
        r->createdMs = millis(o, "insertTime");
        if (o.contains("completeTime")) r->doneMs = millis(o, "completeTime");
        // 1 credited and withdrawable; 2 rejected and 7 wrong deposit never credit
        // the account. 0/8 wait, and 6 (credited, not yet withdrawable) is rechecked.
        r->success = status == 1; r->final = status == 1 || status == 2 || status == 7;
    } else {
        r->createdMs = utcText(o, "applyTime");
        if (o.contains("completeTime") && !text(o, "completeTime").isEmpty()) r->doneMs = utcText(o, "completeTime");
        r->success = status == 6; r->final = status == 1 || status == 3 || status == 5 || status == 6;
    }
    // A row outside the requested window is kept: the provider's window field is
    // not documented, and dropping it could hide a real movement.
    if (!r->createdMs || r->createdMs > received + 2000) return false;
    if (r->success && !r->doneMs) r->doneMs = r->createdMs;
    return r->doneMs <= received + 2000;
}
} // namespace

QString FundFlowProtocol::label(FlowSource s) {
    switch (s) {
    case FlowSource::UpbitDeposits: return "업비트 입금";
    case FlowSource::UpbitWithdrawals: return "업비트 출금";
    case FlowSource::BinanceDeposits: return "바이낸스 입금";
    case FlowSource::BinanceWithdrawals: return "바이낸스 출금";
    case FlowSource::BinanceTransfers: return "바이낸스 지갑 이동";
    }
    return {};
}
bool FundFlowProtocol::validQuery(const FlowQuery& q, qint64 now) {
    if (now <= 0 || now > LastTime || q.page < 1 || q.page > 10000) return false;
    if (flowUpbit(q.source)) {
        if (q.fromMs || q.throughMs || q.offset || !q.transferType.isEmpty() || q.uuids.size() > 100) return false;
        return q.uuids.isEmpty() || q.page == 1;
    }
    if (!q.uuids.isEmpty() || q.fromMs <= 0 || q.throughMs < q.fromMs || q.throughMs > now) return false;
    if (q.source == FlowSource::BinanceTransfers)
        return q.offset == 0 && (flowSpotTransferTypes() + flowUsdmTransferTypes()).contains(q.transferType) &&
            q.throughMs - q.fromMs <= TransferWindowMs;
    return q.page == 1 && q.transferType.isEmpty() && q.offset >= 0 && q.offset <= 1000000 &&
        q.throughMs - q.fromMs <= BinanceWindowMs;
}
RequestSpec FundFlowProtocol::request(const FlowQuery& q, const CredentialSet& credentials, qint64 now, QString* error) {
    if (error) error->clear();
    const auto failed = [&] { if (error) *error = "입출금 조회 범위 또는 읽기 인증정보를 확인해 주세요."; return RequestSpec{}; };
    if (!validQuery(q, now)) return failed();
    RequestSpec r; r.operation = Operation::FundFlowRead; r.authenticated = true;
    if (flowUpbit(q.source)) {
        const auto access = credentials.find("access_key"), secret = credentials.find("secret");
        if (access == credentials.end() || secret == credentials.end() || !credential(access->second, false) || !credential(secret->second, false)) return failed();
        QByteArray query;
        if (q.uuids.isEmpty()) query = "limit=100&order_by=desc&page=" + QByteArray::number(q.page);
        else { for (const auto& id : q.uuids) query += "uuids[]=" + id.toLatin1() + '&'; query += "limit=100"; }
        r.url = QUrl::fromEncoded("https://api.upbit.com" + flowPath(q.source).toUtf8() + '?' + query, QUrl::StrictMode);
        r.headers.insert("Accept", "application/json");
        try { r.headers.insert("Authorization", upbitQueryAuthorization(credentials, query)); } catch (const std::exception&) { return failed(); }
        return allowedFundFlowRead(r) ? r : failed();
    }
    const auto key = credentials.find("api_key"), secret = credentials.find("secret");
    if (key == credentials.end() || secret == credentials.end() || !credential(key->second, true) || !credential(secret->second, true)) return failed();
    QByteArray wire;
    const auto add = [&](const QByteArray& k, const QString& v) { if (!wire.isEmpty()) wire += '&'; wire += k + '=' + QUrl::toPercentEncoding(v); };
    if (q.source == FlowSource::BinanceTransfers) add("type", q.transferType);
    add("startTime", QString::number(q.fromMs)); add("endTime", QString::number(q.throughMs));
    if (q.source == FlowSource::BinanceTransfers) { add("current", QString::number(q.page)); add("size", "100"); }
    else { add("offset", QString::number(q.offset)); add("limit", "1000"); }
    add("recvWindow", "5000"); add("timestamp", QString::number(now));
    SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)), digest(crypto_auth_hmacsha256_BYTES);
    auto* hmac = reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
    if (crypto_auth_hmacsha256_init(hmac, secret->second.data(), secret->second.size()) ||
        crypto_auth_hmacsha256_update(hmac, reinterpret_cast<const unsigned char*>(wire.constData()), wire.size()) ||
        crypto_auth_hmacsha256_final(hmac, digest.data())) return failed();
    auto signature = QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()), qsizetype(digest.size())).toHex();
    wire += "&signature=" + signature; sodium_memzero(signature.data(), std::size_t(signature.size()));
    r.url = QUrl::fromEncoded("https://api.binance.com" + flowPath(q.source).toUtf8() + '?' + wire, QUrl::StrictMode);
    r.headers.insert("X-MBX-APIKEY", QByteArray(reinterpret_cast<const char*>(key->second.data()), qsizetype(key->second.size())));
    return allowedFundFlowRead(r) ? r : failed();
}
FlowPage FundFlowProtocol::parsePage(const FlowQuery& q, const HttpResult& h, qint64 received) {
    const auto fail = [](const QString& code) { FlowPage out; out.errorCode = code; return out; };
    if (!validQuery(q, received)) return fail("QUERY_INVALID");
    if (h.networkError || h.timedOut) return fail("NETWORK");
    if (h.status == 429 || h.status == 418) return fail("RATE_LIMIT");
    QString error; const auto doc = parseExactJson(h.body, &error);
    if (h.status == 401 || h.status == 403) {
        // Upbit names a key without the deposit/withdrawal inquiry scope.
        const auto name = doc.object().value("error").toObject().value("name").toString();
        return fail(name == "out_of_scope" ? "PERMISSION" : "AUTHENTICATION");
    }
    if (h.status < 200 || h.status >= 300) return fail("HTTP_ERROR");
    if (doc.isNull() || !error.isEmpty() || !jsonObjectKeysUnique(h.body)) return fail("INVALID_JSON");
    FlowPage page; QJsonArray rows;
    if (q.source == FlowSource::BinanceTransfers) {
        const auto object = doc.object(); bool ok = false; const auto total = text(object, "total").toLongLong(&ok);
        if (!doc.isObject() || object.contains("code") || !ok || total < 0 || total > 1000000 ||
            (object.contains("rows") && !object.value("rows").isArray()) || (!object.contains("rows") && total != 0)) return fail("PAGE_INVALID");
        rows = object.value("rows").toArray(); page.total = int(total);
        if (rows.size() > TransferPageSize) return fail("PAGE_INVALID");
    } else {
        if (!doc.isArray()) return fail("PROVIDER_ERROR");
        rows = doc.array();
        if (rows.size() > (flowUpbit(q.source) ? UpbitPageSize : BinanceLimit)) return fail("PAGE_INVALID");
    }
    QSet<QString> seen;
    for (const auto& value : rows) {
        FlowRecord r; r.source = q.source; r.receivedAtMs = received;
        if (!value.isObject() || !(flowUpbit(q.source) ? parseUpbit(q, value.toObject(), received, &r) : parseBinance(q, value.toObject(), received, &r)))
            return fail("ROW_INVALID");
        if (seen.contains(r.id)) return fail("DUPLICATE_ID");
        seen.insert(r.id); page.rows.append(r);
    }
    page.ok = true; return page;
}
} // namespace keefi
