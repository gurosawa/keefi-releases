#include "keefi/IdentityCompatibility.h"
#include "keefi/FundFlowTracking.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <limits>
#include <sodium.h>

namespace keefi {
namespace {
constexpr qint64 LastTime = 253402300799999LL;
bool fail(QString* error, const QString& code) { if (error) *error = code; return false; }
bool hex(const QString& s) { static const QRegularExpression p("\\A[a-f0-9]{64}\\z"); return p.match(s).hasMatch(); }
bool plain(const QString& s, int max, bool empty = false) {
    if (s.isEmpty()) return empty;
    if (s.size() > max) return false;
    for (const auto c : s) if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format || c.isSurrogate()) return false;
    return true;
}
qint64 stamp(const QJsonObject& o, const char* key, bool zero = false) {
    if (!o.value(QLatin1String(key)).isString()) return -1;
    const auto s = o.value(QLatin1String(key)).toString(); bool ok = false; const auto n = s.toLongLong(&ok);
    return ok && QString::number(n) == s && n >= (zero ? 0 : 1) && n <= LastTime ? n : -1;
}
QString mac(const SecureBuffer& key, const QStringList& fields) {
    const auto data = QJsonDocument(QJsonArray::fromStringList(fields)).toJson(QJsonDocument::Compact);
    unsigned char digest[32];
    if (crypto_generichash(digest, sizeof digest, reinterpret_cast<const unsigned char*>(data.constData()),
            data.size(), key.data(), key.size())) return {};
    const auto result = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(digest), sizeof digest).toHex());
    sodium_memzero(digest, sizeof digest); return result;
}
QString sourceToken(FlowSource s) { return flowPath(s).mid(1); }
// Same meaning ignores when it was received and the raw recheck reference.
bool sameMeaning(FlowRecord a, FlowRecord b) { a.receivedAtMs = b.receivedAtMs = 0; a.pendingRef = b.pendingRef = QString{}; return a == b; }
// Only a still-pending row may change, and never its asset, amount or request time.
bool allowedChange(const FlowRecord& before, const FlowRecord& after) {
    return !before.final && before.asset == after.asset && before.amount == after.amount &&
        before.createdMs == after.createdMs && before.type == after.type && before.wallet == after.wallet;
}
QJsonObject recordJson(const FlowRecord& r) {
    return {{"source",int(r.source)},{"id",r.id},{"reference",r.reference},{"pendingRef",r.pendingRef},{"asset",r.asset},
        {"amount",r.amount},{"fee",r.fee},{"state",r.state},{"type",r.type},{"wallet",r.wallet},{"internal",r.internal},
        {"created",QString::number(r.createdMs)},{"done",QString::number(r.doneMs)},{"received",QString::number(r.receivedAtMs)},
        {"final",r.final},{"success",r.success}};
}
bool decodeRecord(const QJsonObject& o, FlowRecord* r) {
    if (o.size() != 16 || !o.value("source").isDouble() || o.value("source") != QJsonValue(o.value("source").toInt()) ||
        o.value("source").toInt(-1) < 0 || o.value("source").toInt() > int(FlowSource::BinanceTransfers) ||
        !o.value("wallet").isDouble() || o.value("wallet") != QJsonValue(o.value("wallet").toInt()) ||
        o.value("wallet").toInt() < -1 || o.value("wallet").toInt() > 1 ||
        !o.value("internal").isBool() || !o.value("final").isBool() || !o.value("success").isBool()) return false;
    r->source = FlowSource(o.value("source").toInt()); r->wallet = o.value("wallet").toInt();
    const QList<QPair<const char*, QString*>> strings{{"id",&r->id},{"reference",&r->reference},{"pendingRef",&r->pendingRef},
        {"asset",&r->asset},{"amount",&r->amount},{"fee",&r->fee},{"state",&r->state},{"type",&r->type}};
    for (const auto& [key, target] : strings) {
        if (!o.value(QLatin1String(key)).isString()) return false;
        *target = o.value(QLatin1String(key)).toString(); if (!plain(*target, 128, true)) return false;
    }
    if (!hex(r->id) || (!r->reference.isEmpty() && !hex(r->reference)) || r->asset.isEmpty() || r->state.isEmpty()) return false;
    for (const auto* n : {&r->amount, &r->fee}) if (!decimal::valid(*n) || decimal::normalize(*n) != *n) return false;
    r->createdMs = stamp(o,"created"); r->doneMs = stamp(o,"done",true); r->receivedAtMs = stamp(o,"received");
    r->internal = o.value("internal").toBool(); r->final = o.value("final").toBool(); r->success = o.value("success").toBool();
    if (r->createdMs < 0 || r->doneMs < 0 || r->receivedAtMs < 0 || (r->success && (!r->final || !r->doneMs))) return false;
    // The raw recheck reference lives only while the row is pending.
    return r->final ? r->pendingRef.isEmpty() : true;
}
// End of the merged run of windows that starts at or before `from`.
qint64 contiguousThrough(const FlowAccountState& s, FlowSource source, const QString& type, qint64 from) {
    QList<FlowWindow> windows;
    for (const auto& w : s.windows) if (w.source == source && w.transferType == type) windows.append(w);
    std::sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) { return a.fromMs < b.fromMs; });
    qint64 through = 0; bool started = false;
    for (const auto& w : windows) {
        if (!started) { if (w.fromMs > from) break; started = true; through = w.throughMs; continue; }
        if (w.fromMs > through + 1) break;
        through = qMax(through, w.throughMs);
    }
    return started ? through : 0;
}
} // namespace

QStringList FundFlowTracking::transferTypes(const FlowAccountState& s) {
    if (s.provider != "binance") return {};
    return s.market == "spot" ? flowSpotTransferTypes() : s.market == "usdm_futures" ? flowUsdmTransferTypes() : QStringList{};
}
QList<FlowSource> FundFlowTracking::sources(const FlowAccountState& s) {
    if (s.provider == "upbit") return {FlowSource::UpbitDeposits, FlowSource::UpbitWithdrawals};
    if (s.provider == "binance" && s.market == "spot") return {FlowSource::BinanceDeposits, FlowSource::BinanceWithdrawals, FlowSource::BinanceTransfers};
    if (s.provider == "binance" && s.market == "usdm_futures") return {FlowSource::BinanceTransfers};
    return {};
}
qint64 FundFlowTracking::coverageStart(const FlowAccountState& s) { return qMax(qint64(1), s.trackingStartMs - FirstLookbackMs); }
bool FundFlowTracking::validAccount(const FlowAccountState& s) {
    return hex(s.accountScope) && hex(s.binding) && plain(s.connectionId, 160) && !sources(s).isEmpty() &&
        s.trackingStartMs > 0 && s.trackingStartMs <= LastTime && s.upbitConfirmedAtMs >= 0 && s.upbitConfirmedAtMs <= LastTime &&
        (s.provider == "upbit" || s.upbitConfirmedAtMs == 0) && s.observations.size() <= MaxObservations &&
        s.windows.size() <= MaxWindows && s.revision <= quint64(std::numeric_limits<qint64>::max());
}
bool FundFlowTracking::covers(const FlowAccountState& s, FlowSource source, const QString& type, qint64 from, qint64 through) {
    QList<FlowWindow> windows;
    for (const auto& w : s.windows) if (w.source == source && w.transferType == type) windows.append(w);
    std::sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) { return a.fromMs < b.fromMs; });
    qint64 reached = 0; bool started = false;
    for (const auto& w : windows) {
        if (!started) { if (w.fromMs > from) return false; started = true; reached = w.throughMs; }
        else if (w.fromMs > reached + 1) break;
        else reached = qMax(reached, w.throughMs);
        if (reached >= through) return true;
    }
    return started && reached >= through;
}
QList<FlowJob> FundFlowTracking::plan(const FlowAccountState& s, qint64 runThrough) {
    QList<FlowJob> jobs;
    if (!validAccount(s) || runThrough < s.trackingStartMs) return jobs;
    const auto first = coverageStart(s), coverEnd = runThrough - ListingLagMs;
    for (const auto source : sources(s)) {
        const auto types = source == FlowSource::BinanceTransfers ? transferTypes(s) : QStringList{QString{}};
        for (const auto& type : types) {
            const auto contiguous = contiguousThrough(s, source, type, first);
            const auto from = contiguous ? qMax(first, qMin(contiguous, runThrough) - OverlapMs) : first;
            if (flowUpbit(source)) { jobs.append({{source}, from, from, coverEnd}); continue; }
            const auto span = source == FlowSource::BinanceTransfers ? FundFlowProtocol::TransferWindowMs : FundFlowProtocol::BinanceWindowMs;
            for (auto start = from; start <= runThrough;) {
                const auto end = qMin(runThrough, start + span);
                FlowQuery q; q.source = source; q.fromMs = start; q.throughMs = end; q.transferType = type;
                jobs.append({q, 0, start, qMin(end, coverEnd)});
                if (end == runThrough) break; start = end + 1;
            }
        }
        // Rows still pending are rechecked for a bounded time after their request.
        QStringList uuids; QSet<QString> days;
        for (const auto& o : s.observations) {
            const auto& r = o.record;
            if (r.source != source || r.final || o.conflict || runThrough - r.createdMs > PendingRecheckMs) continue;
            if (flowUpbit(source)) { if (!r.pendingRef.isEmpty() && !uuids.contains(r.pendingRef)) uuids.append(r.pendingRef); continue; }
            const auto day = r.createdMs - r.createdMs % Day;
            const auto key = r.type + ':' + QString::number(day);
            if (days.contains(key) || day + Day - 1 >= runThrough - OverlapMs) continue; // Inside the regular overlap.
            days.insert(key);
            FlowQuery q; q.source = source; q.fromMs = day; q.throughMs = qMin(runThrough, day + Day - 1);
            q.transferType = source == FlowSource::BinanceTransfers ? r.type : QString{};
            jobs.append({q, 0, q.fromMs, qMin(q.throughMs, coverEnd)});
        }
        for (qsizetype i = 0; i < uuids.size(); i += 100) { FlowQuery q; q.source = source; q.uuids = uuids.mid(i, 100); jobs.append({q}); }
    }
    return jobs;
}
bool FundFlowTracking::merge(const FlowAccountState& old, const FlowBatch& b, const SecureBuffer& rootKey, FlowAccountState* out, QString* error) {
    if (error) error->clear();
    if (!out || rootKey.size() != 32 || !validAccount(old) || old.revision >= quint64(std::numeric_limits<qint64>::max())) return fail(error, "MERGE_INPUT");
    SecureBuffer derived(32);
    const auto domain = identityDomain(IdentityDomain::EventReference);
    if (crypto_generichash(derived.data(), derived.size(), reinterpret_cast<const unsigned char*>(domain.constData()), std::size_t(domain.size()),
            rootKey.data(), rootKey.size())) return fail(error, "REFERENCE_KEY");
    auto next = old; ++next.revision;
    QMap<QString, qsizetype> index;
    for (qsizetype i = 0; i < next.observations.size(); ++i) index.insert(next.observations[i].key, i);
    const auto allowedSources = sources(old); const auto types = transferTypes(old);
    QSet<QString> within;
    for (auto row : b.rows) {
        if (!allowedSources.contains(row.source) || (row.source == FlowSource::BinanceTransfers && !types.contains(row.type)) ||
            row.receivedAtMs <= 0 || row.receivedAtMs > b.receivedAtMs || row.id.isEmpty()) return fail(error, "ROW_SCOPE");
        const auto key = mac(derived, {"fund-flow-record", old.accountScope, sourceToken(row.source), row.id});
        if (key.isEmpty() || within.contains(key)) return fail(error, "IDENTITY_AMBIGUOUS");
        within.insert(key);
        if (row.source == FlowSource::BinanceTransfers) row.reference = mac(derived, {"binance-universal-transfer", row.id});
        row.id = key; if (row.final) row.pendingRef.clear();
        const auto found = index.constFind(key);
        if (found == index.cend()) {
            index.insert(key, next.observations.size()); next.observations.append({key, next.revision, row, false});
            continue;
        }
        auto& prior = next.observations[*found];
        if (sameMeaning(prior.record, row)) continue;
        if (row.receivedAtMs < prior.record.receivedAtMs) return fail(error, "STALE_OBSERVATION");
        // A changed final row, or a pending row whose identity fields moved, stays visible as a conflict.
        prior.conflict = prior.conflict || !allowedChange(prior.record, row);
        prior.record = row; prior.version = next.revision;
        if (prior.conflict) prior.record.pendingRef.clear();
    }
    for (const auto& w : b.windows) {
        if (!allowedSources.contains(w.source) || (w.source == FlowSource::BinanceTransfers) != !w.transferType.isEmpty() ||
            (!w.transferType.isEmpty() && !types.contains(w.transferType)) || w.fromMs <= 0 || w.throughMs < w.fromMs ||
            w.receivedAtMs < w.throughMs) return fail(error, "WINDOW_SCOPE");
        FlowWindow joined = w; bool changed = true;
        while (changed) {
            changed = false;
            for (qsizetype i = 0; i < next.windows.size(); ++i) {
                const auto x = next.windows[i];
                if (x.source == joined.source && x.transferType == joined.transferType &&
                    x.fromMs <= joined.throughMs + 1 && joined.fromMs <= x.throughMs + 1) {
                    joined.fromMs = qMin(joined.fromMs, x.fromMs); joined.throughMs = qMax(joined.throughMs, x.throughMs);
                    joined.receivedAtMs = qMax(joined.receivedAtMs, x.receivedAtMs); next.windows.removeAt(i); changed = true; break;
                }
            }
        }
        next.windows.append(joined);
    }
    if (!validAccount(next)) return fail(error, "COLLECTION_LIMIT");
    FlowAccountState checked;
    if (!decode(encode(next), &checked) || checked != next) return fail(error, "COLLECTION_SCHEMA");
    *out = std::move(next); return true;
}
QJsonObject FundFlowTracking::encode(const FlowAccountState& s) {
    QJsonArray observations, windows;
    for (const auto& o : s.observations) observations.append(QJsonObject{{"key",o.key},{"version",QString::number(o.version)},{"conflict",o.conflict},{"record",recordJson(o.record)}});
    for (const auto& w : s.windows) windows.append(QJsonObject{{"source",int(w.source)},{"type",w.transferType},{"from",QString::number(w.fromMs)},
        {"through",QString::number(w.throughMs)},{"received",QString::number(w.receivedAtMs)}});
    return {{"kind","fund-flows-v1"},{"scope",s.accountScope},{"connection",s.connectionId},{"binding",s.binding},
        {"provider",s.provider},{"market",s.market},{"revision",QString::number(s.revision)},{"start",QString::number(s.trackingStartMs)},
        {"upbitConfirmed",QString::number(s.upbitConfirmedAtMs)},{"observations",observations},{"windows",windows}};
}
bool FundFlowTracking::decode(const QJsonObject& o, FlowAccountState* output) {
    if (!output || o.size() != 11 || o.value("kind") != QJsonValue("fund-flows-v1") || !o.value("observations").isArray() ||
        !o.value("windows").isArray() || o.value("observations").toArray().size() > MaxObservations ||
        o.value("windows").toArray().size() > MaxWindows) return false;
    FlowAccountState s;
    s.accountScope = o.value("scope").toString(); s.connectionId = o.value("connection").toString(); s.binding = o.value("binding").toString();
    s.provider = o.value("provider").toString(); s.market = o.value("market").toString();
    const auto revision = stamp(o, "revision", true); s.trackingStartMs = stamp(o, "start"); s.upbitConfirmedAtMs = stamp(o, "upbitConfirmed", true);
    if (revision < 0 || s.trackingStartMs < 0 || s.upbitConfirmedAtMs < 0) return false;
    s.revision = quint64(revision);
    QSet<QString> keys;
    for (const auto& v : o.value("observations").toArray()) {
        if (!v.isObject()) return false; const auto row = v.toObject(); FlowObservation r;
        if (row.size() != 4 || !row.value("conflict").isBool() || !row.value("record").isObject()) return false;
        r.key = row.value("key").toString(); const auto version = stamp(row, "version");
        if (!hex(r.key) || keys.contains(r.key) || version <= 0 || quint64(version) > s.revision ||
            !decodeRecord(row.value("record").toObject(), &r.record) || r.record.id != r.key) return false;
        r.version = quint64(version); r.conflict = row.value("conflict").toBool(); keys.insert(r.key); s.observations.append(r);
    }
    for (const auto& v : o.value("windows").toArray()) {
        if (!v.isObject()) return false; const auto row = v.toObject(); const auto source = row.value("source").toInt(-1);
        FlowWindow w{FlowSource(qMax(0, source)), row.value("type").toString(), stamp(row,"from"), stamp(row,"through"), stamp(row,"received")};
        if (row.size() != 5 || !row.value("type").isString() || source < 0 || source > int(FlowSource::BinanceTransfers) ||
            row.value("source") != QJsonValue(source) || w.fromMs <= 0 || w.throughMs < w.fromMs || w.receivedAtMs < w.throughMs ||
            (w.source == FlowSource::BinanceTransfers) == w.transferType.isEmpty()) return false;
        s.windows.append(w);
    }
    if (!validAccount(s)) return false;
    *output = std::move(s); return true;
}
} // namespace keefi
