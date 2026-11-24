#include "keefi/IdentityCompatibility.h"
#include "keefi/EarnTracking.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <limits>
#include <sodium.h>

namespace keefi {
namespace {
bool fail(QString* error, const QString& code) { if (error) *error = code; return false; }
bool hex(const QString& s) { return QRegularExpression("\\A[a-f0-9]{64}\\z").match(s).hasMatch(); }
bool name(const QString& s, int max = 160, bool empty = false) {
    if (s.isEmpty()) return empty;
    if (s.size() > max) return false;
    for (const auto c : s) if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format || c.isSurrogate()) return false;
    return true;
}
qint64 stamp(const QJsonObject& o, const char* key, bool zero = false) {
    if (!o.value(QLatin1String(key)).isString()) return -1;
    const auto s = o.value(QLatin1String(key)).toString(); bool ok = false; const auto n = s.toLongLong(&ok);
    return ok && QString::number(n) == s && n >= (zero ? 0 : 1) && n <= 253402300799999LL ? n : -1;
}
QString mac(const SecureBuffer& key, const QStringList& fields) {
    const auto data = QJsonDocument(QJsonArray::fromStringList(fields)).toJson(QJsonDocument::Compact);
    unsigned char digest[32];
    if (crypto_generichash(digest, sizeof digest, reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), key.data(), key.size())) return {};
    const auto result = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(digest), sizeof digest).toHex());
    sodium_memzero(digest, sizeof digest); return result;
}
bool mutableTransition(const EarnRecord& before, const EarnRecord& after) {
    if (before.asset != after.asset || before.amount != after.amount || before.productId != after.productId ||
        before.positionId != after.positionId || before.timeMs != after.timeMs || before.type != after.type ||
        before.sourceWallet != after.sourceWallet || before.spotAmount != after.spotAmount || before.fundingAmount != after.fundingAmount) return false;
    if (earnReward(before.source)) return false;
    if (before.status == "SUCCESS" || (before.status == "PAID" && !before.issues.contains("PAYOUT_PENDING"))) return false;
    // Only a known pending -> completed refinement is automatically selected.
    const QStringList pending{"PENDING", "PROCESSING", "WAITING", "CREATED"};
    return (pending.contains(before.status)||before.issues.contains("PAYOUT_PENDING")) && (after.status == "SUCCESS" || after.status == "PAID" || pending.contains(after.status));
}
QJsonObject recordJson(const EarnRecord& r) {
    return {{"source",int(r.source)},{"id",r.id},{"positionId",r.positionId},{"productId",r.productId},
        {"asset",r.asset},{"amount",r.amount},{"type",r.type},{"status",r.status},{"sourceWallet",r.sourceWallet},
        {"destinationWallet",r.destinationWallet},{"spotAmount",r.spotAmount},{"fundingAmount",r.fundingAmount},
        {"originalAmount",r.originalAmount},{"lossAmount",r.lossAmount},{"rewardAsset",r.rewardAsset},
        {"rewardAmount",r.rewardAmount},{"collateralAmount",r.collateralAmount},{"redeemingAmount",r.redeemingAmount},
        {"timeMs",QString::number(r.timeMs)},{"deliveryMs",QString::number(r.deliveryMs)},
        {"receivedAtMs",QString::number(r.receivedAtMs)},{"complete",r.complete},{"autoSubscribe",r.autoSubscribe},
        {"payoutComplete",r.payoutComplete.has_value()?QJsonValue(*r.payoutComplete):QJsonValue(QJsonValue::Null)},
        {"issues",QJsonArray::fromStringList(r.issues)}};
}
bool decodeRecord(const QJsonObject& o, EarnRecord* r) {
    if (o.size() != 25 || !o.value("source").isDouble() || o.value("source") != QJsonValue(o.value("source").toInt()) ||
        o.value("source").toInt(-1) < 0 || o.value("source").toInt() > 7 ||
        !o.value("complete").isBool() || !o.value("autoSubscribe").isBool() || !o.value("issues").isArray()) return false;
    r->source = EarnSource(o.value("source").toInt());
    if(!o.contains("payoutComplete")||(!o.value("payoutComplete").isNull()&&!o.value("payoutComplete").isBool()))return false;
    if(o.value("payoutComplete").isBool())r->payoutComplete=o.value("payoutComplete").toBool();
    const QList<QPair<const char*, QString*>> strings{{"id",&r->id},{"positionId",&r->positionId},{"productId",&r->productId},
        {"asset",&r->asset},{"amount",&r->amount},{"type",&r->type},{"status",&r->status},{"sourceWallet",&r->sourceWallet},
        {"destinationWallet",&r->destinationWallet},{"spotAmount",&r->spotAmount},{"fundingAmount",&r->fundingAmount},
        {"originalAmount",&r->originalAmount},{"lossAmount",&r->lossAmount},{"rewardAsset",&r->rewardAsset},
        {"rewardAmount",&r->rewardAmount},{"collateralAmount",&r->collateralAmount},{"redeemingAmount",&r->redeemingAmount}};
    for (const auto& [key, target] : strings) {
        if (!o.value(QLatin1String(key)).isString()) return false;
        *target = o.value(QLatin1String(key)).toString(); if (!name(*target, 161, true)) return false;
    }
    if (!hex(r->id) || (!r->positionId.isEmpty() && !hex(r->positionId)) || !name(r->asset,80) || !name(r->status,80)) return false;
    for (const auto* n : {&r->amount,&r->spotAmount,&r->fundingAmount,&r->originalAmount,&r->lossAmount,&r->rewardAmount,&r->collateralAmount,&r->redeemingAmount})
        if (!n->isEmpty() && (!decimal::valid(*n) || decimal::normalize(*n) != *n)) return false;
    if (r->amount.isEmpty()) return false;
    r->timeMs = stamp(o,"timeMs",true); r->deliveryMs = stamp(o,"deliveryMs",true); r->receivedAtMs = stamp(o,"receivedAtMs");
    if (r->timeMs < 0 || r->deliveryMs < 0 || r->receivedAtMs < 0 || r->timeMs > r->receivedAtMs) return false;
    r->complete = o.value("complete").toBool(); r->autoSubscribe = o.value("autoSubscribe").toBool();
    const QSet<QString> codes{"COLLATERAL","REDEMPTION_IN_POSITION","SOURCE_SPLIT_MISSING","SOURCE_SPLIT_CONFLICT",
        "DESTINATION_UNKNOWN","PAYOUT_PENDING","EARLY_REDEMPTION_LOSS","STATUS_UNCONFIRMED","REWARD_TYPE_UNKNOWN","SOURCE_CONFLICT"};
    QSet<QString> seen;
    for (const auto& v : o.value("issues").toArray()) {
        if (!v.isString() || !codes.contains(v.toString()) || seen.contains(v.toString())) return false;
        seen.insert(v.toString()); r->issues.append(v.toString());
    }
    if (r->complete && !r->issues.isEmpty()) return false;
    BinanceEarnProtocol::normalizeFlexibleDestination(*r);
    return true;
}
} // namespace

bool EarnPageAccumulator::add(const EarnQuery& q, const EarnPage& p, qint64 received, QString* error) {
    if (error) error->clear();
    const auto stop = [&](const QString& code) { failed_ = true; return fail(error,code); };
    if (failed_ || complete() || !p.ok || !BinanceEarnProtocol::validQuery(q,received) || q.page != expectedPage_) return stop("PAGE_SEQUENCE");
    if (total_ < 0) { total_ = p.total; size_ = q.size; batch_.query = q; }
    if (q.source != batch_.query.source || q.fromMs != batch_.query.fromMs || q.throughMs != batch_.query.throughMs ||
        q.size != size_ || total_ != p.total) return stop("PAGE_CHANGED");
    if (batch_.rows.size() + p.rows.size() > total_ || p.rows.size() > size_) return stop("PAGE_COUNT");
    for (const auto& r : p.rows) {
        if (r.source != q.source || r.receivedAtMs != received) return stop("PAGE_ROW_SCOPE");
        const auto id = BinanceEarnProtocol::identity(r);
        if (ids_.contains(id)) return stop(earnReward(q.source) ? "REWARD_ID_AMBIGUOUS" : "PAGE_DUPLICATE");
        ids_.insert(id);
    }
    batch_.rows.append(p.rows); batch_.receivedAtMs = received; ++expectedPage_;
    if (batch_.rows.size() < total_ && p.rows.size() != size_) return stop("PAGE_SHORT");
    return true;
}
bool EarnPageAccumulator::complete() const { return !failed_ && total_ >= 0 && batch_.rows.size() == total_; }
EarnBatch EarnPageAccumulator::batch() const { return complete() ? batch_ : EarnBatch{}; }
void EarnPageAccumulator::clear() { *this = EarnPageAccumulator{}; }

bool EarnTracking::validAccount(const EarnAccountState& s) {
    return hex(s.accountScope) && hex(s.binding) && name(s.connectionId) && s.trackingStartMs > 0 &&
        s.trackingStartMs <= 253402300799999LL && s.observations.size() <= MaxObservations &&
        s.positions.size() <= MaxPositions && s.windows.size() <= MaxWindows && s.revision <= quint64(std::numeric_limits<qint64>::max());
}
bool EarnTracking::sameMeaning(EarnRecord a, EarnRecord b) {
    BinanceEarnProtocol::normalizeFlexibleDestination(a);
    BinanceEarnProtocol::normalizeFlexibleDestination(b);
    a.receivedAtMs=b.receivedAtMs=0;
    // Conflict is our sticky audit marker, not a field the provider will echo.
    const bool aConflict=a.issues.removeAll("SOURCE_CONFLICT")>0;
    const bool bConflict=b.issues.removeAll("SOURCE_CONFLICT")>0;
    if(aConflict&&!bConflict)a.complete=b.complete;
    if(bConflict&&!aConflict)b.complete=a.complete;
    return a==b;
}
QList<EarnObservation> EarnTracking::active(const EarnAccountState& s) {
    QMap<QString,EarnObservation> active;
    for (const auto& r : s.observations) {
        auto old = active.constFind(r.key);
        if (old == active.cend() || old->version < r.version) active[r.key] = r;
    }
    return active.values();
}
bool EarnTracking::merge(const EarnAccountState& old, const EarnBatch& b, const SecureBuffer& rootKey, EarnAccountState* out, QString* error) {
    if (error) error->clear();
    if (!out || rootKey.size() != 32 || !validAccount(old) || old.revision >= quint64(std::numeric_limits<qint64>::max()) ||
        !BinanceEarnProtocol::validQuery(b.query,b.receivedAtMs) || b.rows.size() > BinanceEarnProtocol::MaxRows) return fail(error,"MERGE_INPUT");
    SecureBuffer derived(32);
    const auto domain = identityDomain(IdentityDomain::EventReference);
    if (crypto_generichash(derived.data(),derived.size(),reinterpret_cast<const unsigned char*>(domain.constData()),std::size_t(domain.size()),rootKey.data(),rootKey.size())) return fail(error,"REFERENCE_KEY");
    auto next = old; ++next.revision;
    for (auto& row : next.observations) BinanceEarnProtocol::normalizeFlexibleDestination(row.record);
    const auto source = b.query.source;
    if (earnPositions(source)) {
        const auto previous = next.positionsAt.value(int(source),0);
        if (b.receivedAtMs < previous) return fail(error,"STALE_POSITION_BATCH");
        next.positions.removeIf([source](const EarnRecord& r){return r.source == source;});
        next.positionsAt[int(source)] = b.receivedAtMs;
    }
    QMap<QString,EarnObservation> previous;
    for (const auto& row : active(next)) previous.insert(row.key,row);
    QSet<QString> within;
    for (auto row : b.rows) {
        BinanceEarnProtocol::normalizeFlexibleDestination(row);
        if (row.source != source || row.receivedAtMs > b.receivedAtMs || row.receivedAtMs <= 0) return fail(error,"ROW_SCOPE");
        const auto id = mac(derived,{"binance-earn-record",old.accountScope,BinanceEarnProtocol::identity(row)});
        if (id.isEmpty() || within.contains(id)) return fail(error,"IDENTITY_AMBIGUOUS");
        within.insert(id); row.id = id;
        if (!row.positionId.isEmpty()) row.positionId = mac(derived,{"binance-earn-position",old.accountScope,row.positionId});
        if (earnPositions(source)) { next.positions.append(row); continue; }
        const auto prior = previous.constFind(id);
        if (prior != previous.cend()) {
            if (sameMeaning(prior->record,row)) continue;
            if (row.receivedAtMs < prior->record.receivedAtMs) return fail(error,"STALE_OBSERVATION");
        }
        EarnObservation observation{id,next.revision,row,false};
        if (prior != previous.cend() && (prior->conflict || !mutableTransition(prior->record,row))) {
            observation.conflict = true;
            observation.record.complete = false;
            observation.record.issues << "SOURCE_CONFLICT";
            observation.record.issues.removeDuplicates();
        }
        next.observations.append(observation);
    }
    if (!earnPositions(source)) {
        EarnWindow joined{source,b.query.fromMs,b.query.throughMs,b.receivedAtMs};
        // Coalesce overlapping/adjacent completed QUERY spans. This is not a
        // promise that no delayed provider records can appear in those spans.
        bool changed = true;
        while (changed) {
            changed = false;
            for (qsizetype i = 0; i < next.windows.size(); ++i) {
                const auto w = next.windows[i];
                if (w.source == source && w.fromMs <= joined.throughMs + 1 && joined.fromMs <= w.throughMs + 1) {
                    joined.fromMs = qMin(joined.fromMs,w.fromMs); joined.throughMs = qMax(joined.throughMs,w.throughMs);
                    joined.receivedAtMs = qMax(joined.receivedAtMs,w.receivedAtMs); next.windows.removeAt(i); changed = true; break;
                }
            }
        }
        next.windows.append(joined);
    }
    if (!validAccount(next)) return fail(error,"COLLECTION_LIMIT");
    // Validate exactly the same fixed-field schema before it reaches a ledger.
    EarnAccountState checked;
    if (!decode(encode(next),&checked) || checked != next) return fail(error,"COLLECTION_SCHEMA");
    *out = std::move(next); return true;
}
EarnTracking::Recheck EarnTracking::recheck(const EarnRecord& r, qint64 now) {
    if (earnPositions(r.source) || earnReward(r.source) || r.complete || r.timeMs <= 0 ||
        now < r.timeMs || r.issues.contains("SOURCE_CONFLICT") ||
        r.issues.contains("SOURCE_SPLIT_MISSING") || r.issues.contains("SOURCE_SPLIT_CONFLICT") ||
        r.issues.contains("REWARD_TYPE_UNKNOWN")) return Recheck::NotNeeded;
    const bool pendingState = QStringList{"CREATED","PENDING","PROCESSING","WAITING"}.contains(r.status);
    const bool pendingPayout = r.source == EarnSource::LockedRedemptions && r.status == "PAID" &&
        r.issues.contains("PAYOUT_PENDING");
    if (!pendingState && !pendingPayout) return Recheck::NotNeeded;
    // Locked history omits the credited wallet even while payout is pending.
    // Recheck the payment state, not that permanent omission. Other structural
    // unknowns and terminal/unsupported statuses do not rewind history.
    if (r.issues.contains("DESTINATION_UNKNOWN") &&
        !(r.source == EarnSource::LockedRedemptions && r.issues.contains("PAYOUT_PENDING")))
        return Recheck::NotNeeded;
    const auto anchor = r.source == EarnSource::LockedRedemptions && r.issues.contains("PAYOUT_PENDING") &&
        r.deliveryMs >= r.timeMs ? r.deliveryMs : r.timeMs;
    return now > anchor && now - anchor > PendingRecheckGraceMs ? Recheck::Expired : Recheck::Pending;
}
QList<EarnQuery> EarnTracking::queries(const EarnAccountState& s, qint64 now) {
    QList<EarnQuery> result;
    if (!validAccount(s) || now < s.trackingStartMs) return result;
    constexpr qint64 Day = 86400000;
    const auto observations = active(s);
    for (auto source : earnSources()) {
        if (earnPositions(source)) { result.append({source,0,0,1,100}); continue; }
        // First discovery includes the preceding month to catch already pending
        // subscriptions/redemptions. Performance still starts at trackingStart.
        const auto discoveryFrom = qMax(qint64(1),s.trackingStartMs - 30 * Day + 1);
        qint64 through = discoveryFrom;
        QList<EarnWindow> fetched;
        for (const auto& w : s.windows) if (w.source == source) fetched.append(w);
        std::sort(fetched.begin(),fetched.end(),[](const auto& a,const auto& b){return a.fromMs < b.fromMs;});
        for (const auto& w : fetched) {
            if (w.fromMs > through + 1) break; // A targeted old-day read cannot close a real gap.
            if (w.throughMs >= through) through = w.throughMs;
        }
        const auto from = qMax(discoveryFrom,qMin(through,now) - 2 * Day);
        QList<QPair<qint64,qint64>> spans{{from,now}};
        // Revisit only the original UTC day of a mutable pending record. Do not
        // replay all intervening months. purchaseTime on a current Locked
        // position is NOT a redemption request time and is never a rewind cursor.
        for (const auto& observation : observations) {
            const auto& r = observation.record;
            if (r.source != source || observation.conflict || recheck(r,now) != Recheck::Pending) continue;
            const auto day = r.timeMs - r.timeMs % Day;
            spans.append({qMax(qint64(1),day),qMin(now,day + Day - 1)});
        }
        std::sort(spans.begin(),spans.end());
        QList<QPair<qint64,qint64>> merged;
        for (const auto& span : spans) {
            if (!merged.isEmpty() && span.first <= merged.last().second + 1)
                merged.last().second = qMax(merged.last().second,span.second);
            else merged.append(span);
        }
        for (const auto& span : merged) for (auto start = span.first; start <= span.second;) {
            const auto end = qMin(span.second,start + BinanceEarnProtocol::WindowMs - 1);
            result.append({source,start,end,1,100});
            if (result.size() > 4096) return {}; // Bounded work, never truncate silently.
            if (end == span.second) break; start = end + 1;
        }
    }
    return result;
}
QJsonObject EarnTracking::encode(const EarnAccountState& s) {
    QJsonArray observations,positions,windows,positionsAt;
    for (const auto& r : s.observations) observations.append(QJsonObject{{"key",r.key},{"version",QString::number(r.version)},{"conflict",r.conflict},{"record",recordJson(r.record)}});
    for (const auto& r : s.positions) positions.append(recordJson(r));
    for (const auto& w : s.windows) windows.append(QJsonObject{{"source",int(w.source)},{"from",QString::number(w.fromMs)},{"through",QString::number(w.throughMs)},{"received",QString::number(w.receivedAtMs)}});
    for (auto it = s.positionsAt.cbegin(); it != s.positionsAt.cend(); ++it) positionsAt.append(QJsonObject{{"source",it.key()},{"at",QString::number(it.value())}});
    return {{"kind","binance-simple-earn-v1"},{"scope",s.accountScope},{"connection",s.connectionId},{"binding",s.binding},
        {"revision",QString::number(s.revision)},{"start",QString::number(s.trackingStartMs)},
        {"observations",observations},{"positions",positions},{"positionsAt",positionsAt},{"windows",windows}};
}
bool EarnTracking::decode(const QJsonObject& o, EarnAccountState* output) {
    if (!output || o.size() != 10 || o.value("kind") != QJsonValue("binance-simple-earn-v1") ||
        !o.value("observations").isArray() || !o.value("positions").isArray() || !o.value("positionsAt").isArray() || !o.value("windows").isArray()) return false;
    EarnAccountState s;
    s.accountScope=o.value("scope").toString(); s.connectionId=o.value("connection").toString(); s.binding=o.value("binding").toString();
    const auto revision=stamp(o,"revision",true); s.trackingStartMs=stamp(o,"start");
    if (revision < 0 || s.trackingStartMs < 0) return false; s.revision=quint64(revision);
    if (o.value("observations").toArray().size()>MaxObservations || o.value("positions").toArray().size()>MaxPositions ||
        o.value("windows").toArray().size()>MaxWindows || o.value("positionsAt").toArray().size()>2) return false;
    QSet<QString> positionKeys;
    QMap<QString,quint64> lastVersions;
    for (const auto& v:o.value("observations").toArray()) {
        if (!v.isObject()) return false; const auto row=v.toObject(); EarnObservation r;
        if (row.size()!=4 || !row.value("conflict").isBool() || !row.value("record").isObject()) return false;
        r.key=row.value("key").toString(); const auto version=stamp(row,"version");
        if (!hex(r.key)||version<=0||quint64(version)>s.revision||!decodeRecord(row.value("record").toObject(),&r.record)||r.record.id!=r.key||earnPositions(r.record.source)) return false;
        r.version=quint64(version); r.conflict=row.value("conflict").toBool();
        if ((r.conflict&&!r.record.issues.contains("SOURCE_CONFLICT"))||lastVersions.value(r.key,0)>=r.version) return false;
        lastVersions[r.key]=r.version; s.observations.append(r);
    }
    for (const auto& v:o.value("positions").toArray()) {
        EarnRecord r; if (!v.isObject()||!decodeRecord(v.toObject(),&r)||!earnPositions(r.source)||positionKeys.contains(r.id)) return false;
        positionKeys.insert(r.id); s.positions.append(r);
    }
    for (const auto& v:o.value("positionsAt").toArray()) {
        if (!v.isObject()) return false; const auto row=v.toObject(); const auto source=row.value("source").toInt(-1); const auto at=stamp(row,"at");
        if (row.size()!=2||source<0||source>1||row.value("source")!=QJsonValue(source)||at<=0||s.positionsAt.contains(source)) return false;
        s.positionsAt[source]=at;
    }
    for (const auto& r:s.positions) if (!s.positionsAt.contains(int(r.source))||r.receivedAtMs>s.positionsAt.value(int(r.source))) return false;
    for (const auto& v:o.value("windows").toArray()) {
        if (!v.isObject()) return false; const auto row=v.toObject(); const auto source=row.value("source").toInt(-1);
        EarnWindow w{EarnSource(source),stamp(row,"from"),stamp(row,"through"),stamp(row,"received")};
        if (row.size()!=4||source<2||source>7||row.value("source")!=QJsonValue(source)||w.fromMs<=0||w.throughMs<w.fromMs||w.receivedAtMs<w.throughMs) return false;
        s.windows.append(w);
    }
    if (!validAccount(s)) return false; *output=std::move(s); return true;
}
} // namespace keefi
