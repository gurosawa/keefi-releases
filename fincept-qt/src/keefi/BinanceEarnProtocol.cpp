#include "keefi/BinanceEarnProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <sodium.h>

namespace keefi {
namespace {
bool uniqueObjectKeys(const QByteArray& bytes) {
    struct Frame { bool object=false, key=false; QSet<QString> keys; };
    QList<Frame> stack;
    for(qsizetype i=0;i<bytes.size();++i){
        const auto c=bytes[i];
        if(c=='"'){
            const auto start=i++;bool escaped=false;
            for(;i<bytes.size();++i){if(escaped){escaped=false;continue;}if(bytes[i]=='\\'){escaped=true;continue;}if(bytes[i]=='"')break;}
            if(i>=bytes.size())return false;
            if(!stack.isEmpty()&&stack.last().object&&stack.last().key){
                const auto parsed=QJsonDocument::fromJson('['+bytes.mid(start,i-start+1)+']');
                if(!parsed.isArray()||parsed.array().size()!=1)return false;
                const auto key=parsed.array().first().toString();if(stack.last().keys.contains(key))return false;
                stack.last().keys.insert(key);stack.last().key=false;
            }
        }else if(c=='{'||c=='['){if(stack.size()>=64)return false;stack.append({c=='{',c=='{',{}});}
        else if(c=='}'||c==']'){if(stack.isEmpty())return false;stack.removeLast();}
        else if(c==','&&!stack.isEmpty()&&stack.last().object)stack.last().key=true;
    }
    return stack.isEmpty();
}
QString text(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
bool token(const QString& s, bool empty = false) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9_.*:/ -]{1,160}\\z");
    return (empty && s.isEmpty()) || pattern.match(s).hasMatch();
}
QString number(const QJsonObject& o, const char* key) {
    const auto s = text(o, key);
    static const QRegularExpression pattern("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");
    return pattern.match(s).hasMatch() ? decimal::normalize(s) : QString{};
}
qint64 time(const QJsonObject& o, const char* key) {
    const auto s = text(o, key); bool ok = false; const auto n = s.toLongLong(&ok);
    return ok && n > 0 && n <= 253402300799999LL && QString::number(n) == s ? n : 0;
}
bool credential(const SecureBuffer& value) {
    if (value.empty() || value.size() > 512) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = value.data()[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}
bool optionalNumber(const QJsonObject& o, const char* key, QString* out) {
    if (!o.contains(QLatin1String(key))) return true;
    *out = number(o, key); return !out->isEmpty();
}
bool parse(const EarnQuery& query, const QJsonObject& o, qint64 received, EarnRecord* r) {
    r->source = query.source; r->receivedAtMs = received;
    r->asset = text(o, "asset");
    if (!binanceIdentifierValid(r->asset)) return false;
    const auto s = query.source;
    if (earnPositions(s)) {
        r->productId = text(o, s == EarnSource::FlexiblePositions ? "productId" : "projectId");
        r->positionId = s == EarnSource::LockedPositions ? text(o, "positionId") : QString{};
        r->id = s == EarnSource::FlexiblePositions ? r->productId : r->positionId;
        r->amount = number(o, s == EarnSource::FlexiblePositions ? "totalAmount" : "amount");
        if (!o.value("autoSubscribe").isBool()) return false;
        r->autoSubscribe = o.value("autoSubscribe").toBool(); r->status = "OBSERVED";
        // Cumulative/estimated rewards are deliberately not added to balances.
        if (!optionalNumber(o, "collateralAmount", &r->collateralAmount) ||
            !optionalNumber(o, "redeemingAmt", &r->redeemingAmount)) return false;
        if (decimal::positive(r->collateralAmount)) r->issues << "COLLATERAL";
        if (decimal::positive(r->redeemingAmount)) r->issues << "REDEMPTION_IN_POSITION";
        if (s == EarnSource::LockedPositions) {
            r->timeMs = time(o, "purchaseTime");
            r->destinationWallet = text(o, "redeemTo");
            r->deliveryMs = time(o, "deliverDate");
            if (!r->timeMs || !token(r->positionId)) return false;
        }
        r->complete = r->issues.isEmpty();
    } else {
        r->timeMs = time(o, "time");
        if (!r->timeMs || r->timeMs < query.fromMs || r->timeMs > query.throughMs || r->timeMs > received) return false;
        r->productId = text(o, "productId");
        if (r->productId.isEmpty()) r->productId = text(o, "projectId");
        r->positionId = text(o, "positionId");
        r->type = text(o, "type");
        r->status = text(o, "status");
        r->amount = number(o, s == EarnSource::FlexibleRewards ? "rewards" : "amount");
        if (s == EarnSource::FlexibleSubscriptions || s == EarnSource::LockedSubscriptions) {
            r->id = text(o, "purchaseId");
            r->sourceWallet = text(o, "sourceAccount");
            r->spotAmount = number(o, "amtFromSpot"); r->fundingAmount = number(o, "amtFromFunding");
            if (r->spotAmount.isEmpty() || r->fundingAmount.isEmpty()) {
                r->issues << "SOURCE_SPLIT_MISSING";
            } else if (decimal::add(r->spotAmount, r->fundingAmount) != r->amount) {
                r->issues << "SOURCE_SPLIT_CONFLICT";
            }
            r->complete = r->status == "SUCCESS" && r->issues.isEmpty();
            if (r->status != "SUCCESS") r->issues << "STATUS_UNCONFIRMED";
        } else if (s == EarnSource::FlexibleRedemptions || s == EarnSource::LockedRedemptions) {
            r->id = text(o, "redeemId"); r->destinationWallet = text(o, "destAccount");
            if (s == EarnSource::FlexibleRedemptions) {
                if (r->destinationWallet == "FUND") r->destinationWallet = "FUNDING";
                if (r->destinationWallet != "SPOT" && r->destinationWallet != "FUNDING") r->issues << "DESTINATION_UNKNOWN";
                r->complete = r->status == "PAID" && r->issues.isEmpty();
            } else {
                r->originalAmount = number(o, "originalAmount"); r->lossAmount = number(o, "lossAmount");
                r->deliveryMs = time(o, "deliverDate");
                if (!o.value("isComplete").isBool() || r->originalAmount.isEmpty() || r->lossAmount.isEmpty()) return false;
                r->payoutComplete = o.value("isComplete").toBool();
                // Locked history does not specify the payout wallet. A stored
                // position's redeemTo is a hint, not independent proof of receipt.
                r->issues << "DESTINATION_UNKNOWN";
                if (!o.value("isComplete").toBool() || !r->deliveryMs || r->deliveryMs > received)
                    r->issues << "PAYOUT_PENDING";
                if (decimal::positive(r->lossAmount)) r->issues << "EARLY_REDEMPTION_LOSS";
                r->rewardAsset = text(o, "rewardAsset");
                if (!optionalNumber(o, "rewardAmt", &r->rewardAmount)) return false;
                r->complete = false; // Track PAID without fabricating a Spot inflow.
            }
            if (r->status != "PAID") { r->complete = false; r->issues << "STATUS_UNCONFIRMED"; }
        } else {
            if (!token(r->type)) return false;
            if (s == EarnSource::LockedRewards && !token(r->positionId)) return false;
            if (s == EarnSource::FlexibleRewards && !token(r->productId)) return false;
            // No official per-reward ID is documented. Composite collisions
            // remain errors in the page accumulator; never silently deduplicate.
            r->id = "reward"; r->status = "REPORTED";
            r->complete = true;
            if (s == EarnSource::FlexibleRewards && !QStringList{"BONUS", "REALTIME", "REWARDS"}.contains(r->type)) {
                r->complete = false; r->issues << "REWARD_TYPE_UNKNOWN";
            }
            if(s == EarnSource::LockedRewards && r->type != "Locked Rewards") {
                r->complete = false; r->issues << "REWARD_TYPE_UNKNOWN";
            }
        }
    }
    if (r->amount.isEmpty() || !token(r->id) || !token(r->productId, true) ||
        !token(r->positionId, true) || !token(r->type, true) || !token(r->status) ||
        !token(r->sourceWallet, true) || !token(r->destinationWallet, true) ||
        (!r->rewardAsset.isEmpty()&&!binanceIdentifierValid(r->rewardAsset))) return false;
    if (r->timeMs > received) return false;
    r->issues.removeDuplicates(); return true;
}
} // namespace

bool jsonObjectKeysUnique(const QByteArray& bytes) { return uniqueObjectKeys(bytes); }
bool BinanceEarnProtocol::validQuery(const EarnQuery& q, qint64 now) {
    if (!earnSources().contains(q.source) || q.page < 1 || q.page > 100000 || q.size < 1 || q.size > 100 || now <= 0 || now > 253402300799999LL) return false;
    return earnPositions(q.source) ? q.fromMs == 0 && q.throughMs == 0 :
        q.fromMs > 0 && q.throughMs >= q.fromMs && q.throughMs <= now && q.throughMs - q.fromMs < WindowMs;
}
RequestSpec BinanceEarnProtocol::request(const EarnQuery& q, const CredentialSet& credentials, qint64 now, QString* error) {
    if (error) error->clear();
    const auto failed = [&] { if (error) *error = "Earn 조회 범위 또는 읽기 인증정보를 확인해 주세요."; return RequestSpec{}; };
    const auto key = credentials.find("api_key"), secret = credentials.find("secret");
    if (!validQuery(q, now) || key == credentials.end() || secret == credentials.end() || !credential(key->second) || !credential(secret->second)) return failed();
    QByteArray wire;
    const auto add = [&](const QByteArray& k, const QString& v) { if (!wire.isEmpty()) wire += '&'; wire += k + '=' + QUrl::toPercentEncoding(v); };
    add("current", QString::number(q.page)); add("size", QString::number(q.size));
    if (!earnPositions(q.source)) { add("startTime", QString::number(q.fromMs)); add("endTime", QString::number(q.throughMs)); }
    if (q.source == EarnSource::FlexibleRewards) add("type", "ALL");
    add("recvWindow", "5000"); add("timestamp", QString::number(now));
    SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)), digest(crypto_auth_hmacsha256_BYTES);
    auto* hmac = reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
    if (crypto_auth_hmacsha256_init(hmac, secret->second.data(), secret->second.size()) ||
        crypto_auth_hmacsha256_update(hmac, reinterpret_cast<const unsigned char*>(wire.constData()), wire.size()) ||
        crypto_auth_hmacsha256_final(hmac, digest.data())) return failed();
    auto signature = QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()), qsizetype(digest.size())).toHex();
    wire += "&signature=" + signature; sodium_memzero(signature.data(), std::size_t(signature.size()));
    RequestSpec r; r.operation = Operation::EarnRead; r.authenticated = true;
    r.url = QUrl::fromEncoded("https://api.binance.com" + earnPath(q.source).toUtf8() + '?' + wire, QUrl::StrictMode);
    r.headers.insert("X-MBX-APIKEY", QByteArray(reinterpret_cast<const char*>(key->second.data()), qsizetype(key->second.size())));
    return allowedEarnRead(r) ? r : failed();
}
EarnPage BinanceEarnProtocol::parsePage(const EarnQuery& q, const HttpResult& h, qint64 received) {
    EarnPage result;
    const auto fail = [&](const QString& code) { EarnPage out; out.errorCode = code; return out; };
    if (!validQuery(q, received)) return fail("QUERY_INVALID");
    if (h.networkError || h.timedOut) return fail("NETWORK");
    if (h.status == 429 || h.status == 418) return fail("RATE_LIMIT");
    if (h.status == 401 || h.status == 403) return fail("AUTHENTICATION");
    if (h.status < 200 || h.status >= 300) return fail("HTTP_ERROR");
    QString error; const auto doc = parseExactJson(h.body, &error);
    if (doc.isNull() || !doc.isObject() || !error.isEmpty() || !uniqueObjectKeys(h.body)) return fail("INVALID_JSON");
    const auto object = doc.object();
    if (object.contains("code") || !object.value("rows").isArray()) return fail("PROVIDER_ERROR");
    bool ok = false; const auto total = text(object, "total").toLongLong(&ok);
    const auto rows = object.value("rows").toArray();
    if (!ok || QString::number(total) != text(object, "total") || total < 0 || total > MaxRows ||
        rows.size() > q.size || rows.size() > total) return fail("PAGE_INVALID");
    QSet<QString> seen;
    for (const auto& value : rows) {
        EarnRecord row;
        if (!value.isObject() || !parse(q, value.toObject(), received, &row)) return fail("ROW_INVALID");
        const auto id = identity(row);
        if (seen.contains(id)) return fail(earnReward(q.source) ? "REWARD_ID_AMBIGUOUS" : "DUPLICATE_ID");
        seen.insert(id); result.rows.append(row);
    }
    result.total = int(total); result.ok = true; return result;
}
QString BinanceEarnProtocol::identity(const EarnRecord& r) {
    QJsonArray key{earnSourceToken(r.source), r.id};
    if (earnReward(r.source)) key = {earnSourceToken(r.source), r.productId, r.positionId, r.asset, r.type, QString::number(r.timeMs)};
    return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}
QString BinanceEarnProtocol::label(EarnSource s) {
    switch (s) {
    case EarnSource::FlexiblePositions: return "수시형 보유";
    case EarnSource::LockedPositions: return "기간형 보유";
    case EarnSource::FlexibleSubscriptions: return "수시형 맡긴 원금";
    case EarnSource::LockedSubscriptions: return "기간형 맡긴 원금";
    case EarnSource::FlexibleRedemptions: return "수시형 원금 회수";
    case EarnSource::LockedRedemptions: return "기간형 원금 회수";
    case EarnSource::FlexibleRewards: return "수시형 보상";
    case EarnSource::LockedRewards: return "기간형 보상";
    }
    return {};
}
QString BinanceEarnProtocol::issueLabel(const QString& code) {
    static const QMap<QString, QString> messages{
        {"COLLATERAL", "담보로 사용 중인 수량이 있어 순자산 확인 필요"},
        {"REDEMPTION_IN_POSITION", "상환 중 수량의 보유액 포함 여부 확인 필요"},
        {"SOURCE_SPLIT_MISSING", "출발 지갑별 원금 확인 필요"},
        {"SOURCE_SPLIT_CONFLICT", "출발 지갑별 원금 합계 불일치"},
        {"DESTINATION_UNKNOWN", "상환 도착 지갑·잔고 반영 확인 필요"},
        {"PAYOUT_PENDING", "상환 지급 완료 확인 전"},
        {"EARLY_REDEMPTION_LOSS", "중도 상환으로 줄어든 수익 확인 필요"},
        {"STATUS_UNCONFIRMED", "거래소 처리 결과 확인 필요"},
        {"REWARD_TYPE_UNKNOWN", "새 보상 유형 확인 필요"},
        {"SOURCE_CONFLICT", "같은 거래소 기록의 내용이 달라 확인 필요"}};
    return messages.value(code, "기록 확인 필요");
}
void BinanceEarnProtocol::normalizeFlexibleDestination(EarnRecord& r) {
    if (r.source != EarnSource::FlexibleRedemptions) return;
    if (r.destinationWallet == "FUND") r.destinationWallet = "FUNDING";
    if (r.destinationWallet != "SPOT" && r.destinationWallet != "FUNDING") return;
    // Older readers incorrectly marked FUNDING as unknown. Remove only that
    // derived issue; never erase a real source conflict or invent payout evidence.
    if (r.issues.removeAll("DESTINATION_UNKNOWN"))
        r.complete = r.status == "PAID" && r.issues.isEmpty();
}
} // namespace keefi
