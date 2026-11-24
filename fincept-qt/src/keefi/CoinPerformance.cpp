#include "keefi/CoinPerformance.h"
#include "keefi/PublicFormatMigration.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>
#include <algorithm>

namespace keefi {
namespace {
const QString Collecting = QStringLiteral("입출금 기록을 확인하고 있어요");
const QString Pending = QStringLiteral("처리 중인 입출금이 있어 계산을 기다리고 있어요");
const QString Boundary = QStringLiteral("잔고 기록 시각 근처의 입출금이 있어 다음 기록을 기다리고 있어요");
const QString CoinMove = QStringLiteral("코인 입출금·지갑 이동이 있어 이 기간은 아직 계산하지 않았어요");
const QString Unpaired = QStringLiteral("내 계좌 사이의 이동인지 확인하지 못한 내역이 있어요");
const QString KrwFee = QStringLiteral("수수료가 있는 원화 입출금은 아직 계산하지 않았어요");
const QString Changed = QStringLiteral("거래소 기록 내용이 바뀐 내역이 있어 확인이 필요해요");
const QString EarnMissing = QStringLiteral("바이낸스 Earn 기록을 한 번 조회해 주세요 (자산 기록 → Earn 원금·보상)");
const QString EarnMove = QStringLiteral("바이낸스 Earn 이동이 있어 이 기간은 계산하지 않았어요");
const QString UpbitConfirm = QStringLiteral("업비트 포켓·스테이킹을 쓰지 않는지 확인해 주세요");

struct Point {
    bool valid = false;
    qint64 timeMs = 0;
    AssetValuationResult valuation;
    QMap<QString, qint64> snapshotAt; // Account scope -> balance time used.
};
struct Gate {
    QStringList reasons;
    QList<AssetEvent> events;
    QList<CoinFlowLine> lines;
    void block(const QString& reason) { if (!reasons.contains(reason)) reasons.append(reason); }
};
bool earnCovers(const EarnAccountState& s, EarnSource source, qint64 from, qint64 through) {
    QList<EarnWindow> windows;
    for (const auto& w : s.windows) if (w.source == source) windows.append(w);
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
// "MAIN_UMFUTURE" -> {"MAIN", "UMFUTURE"}; wallet names may contain '_'.
QPair<QString, QString> wallets(const QString& type) {
    static const QStringList names{"PORTFOLIO_MARGIN", "ISOLATEDMARGIN", "UMFUTURE", "CMFUTURE", "FUNDING", "MARGIN", "OPTION", "MAIN"};
    for (const auto& from : names) if (type.startsWith(from + '_')) {
        const auto to = type.mid(from.size() + 1);
        if (names.contains(to)) return {from, to};
    }
    return {};
}
bool usable(const PortfolioCheckpoint& p) {
    return p.available && p.valuation.complete && p.valuation.reconciled && p.valuation.issues.isEmpty() && !p.valuation.netAssets.isEmpty();
}
Point pointOf(const PortfolioCheckpoint& p) {
    Point r; r.valid = usable(p); r.timeMs = p.timeMs; r.valuation = p.valuation;
    for (const auto& s : p.usedSnapshots) r.snapshotAt.insert(s.accountScope, s.timeMs);
    return r;
}
} // namespace

bool CoinPerformance::inScope(const AssetSnapshot& s) {
    return s.environment == "live" && (s.provider == "upbit" || (s.provider == "binance" && (s.market == "spot" || s.market == "usdm_futures")));
}

CoinPerformanceView CoinPerformance::evaluate(const CoinPerformanceInput& in) {
    CoinPerformanceView view; view.today.name = "오늘"; view.sinceStart.name = "기록 시작 이후";
    QList<AssetSnapshot> coin;
    for (const auto& s : in.current) if (inScope(s) && !s.connectionId.isEmpty() && !s.connectionBinding.isEmpty()) coin.append(s);
    std::sort(coin.begin(), coin.end(), [](const auto& a, const auto& b) { return a.accountScope < b.accountScope; });
    if (coin.isEmpty()) {
        view.today.reason = view.sinceStart.reason = "업비트·바이낸스 연결이 없어요";
        return view;
    }
    view.inScope = true;
    CheckpointSelection selection; QStringList scopes; QMap<QString, AssetSnapshot> byScope;
    QSet<QString> seenConnections;
    for (const auto& s : coin) {
        if (!seenConnections.contains(s.connectionId)) { selection.connections.append({s.connectionId, s.connectionBinding}); seenConnections.insert(s.connectionId); }
        scopes.append(s.accountScope); byScope.insert(s.accountScope, s);
    }
    // Collection state that belongs to the same connection and key.
    QMap<QString, FlowAccountState> flows;
    for (const auto& f : in.flows) if (byScope.contains(f.accountScope) && byScope[f.accountScope].connectionId == f.connectionId &&
        byScope[f.accountScope].connectionBinding == f.binding) flows.insert(f.accountScope, f);
    QMap<QString, QSet<QString>> successfulReferences;
    for (auto it = flows.cbegin(); it != flows.cend(); ++it) {
        auto& references = successfulReferences[it.key()];
        for (const auto& observation : it.value().observations)
            if (!observation.conflict && observation.record.success && !observation.record.reference.isEmpty())
                references.insert(observation.record.reference);
    }
    const EarnAccountState* earn = nullptr; QString spotScope, usdmScope;
    for (const auto& s : coin) {
        if (s.provider == "binance" && s.market == "spot") spotScope = s.accountScope;
        if (s.provider == "binance" && s.market == "usdm_futures") usdmScope = s.accountScope;
    }
    for (const auto& e : in.earn) if (!spotScope.isEmpty() && e.accountScope == spotScope && e.binding == byScope[spotScope].connectionBinding) earn = &e;
    for (const auto& s : coin) if (s.provider == "upbit" && (!flows.contains(s.accountScope) || flows[s.accountScope].upbitConfirmedAtMs <= 0))
        view.upbitScopesToConfirm.append(s.accountScope);

    // Earliest instant every source has been read from.
    qint64 coverageStart = 0; bool allTracked = true;
    for (const auto& s : coin) {
        if (!flows.contains(s.accountScope)) { allTracked = false; continue; }
        coverageStart = qMax(coverageStart, FundFlowTracking::coverageStart(flows[s.accountScope]));
    }
    if (earn) coverageStart = qMax(coverageStart, earn->trackingStartMs - 30LL * 86400000 + 1);

    // Candidate balance instants of the selected connections, both ledgers merged.
    QList<AssetSnapshot> history = in.history; QSet<QString> known;
    for (const auto& s : history) known.insert(s.accountScope + QChar(0) + s.id);
    for (const auto& s : in.today) if (!known.contains(s.accountScope + QChar(0) + s.id)) { known.insert(s.accountScope + QChar(0) + s.id); history.append(s); }
    QList<qint64> times;
    for (const auto& s : history) if (seenConnections.contains(s.connectionId) && s.timeMs <= in.nowMs) times.append(s.timeMs);
    std::sort(times.begin(), times.end()); times.erase(std::unique(times.begin(), times.end()), times.end());
    // Search the complete bounded input. Cache shared today/since-start
    // candidates instead of abandoning valid endpoints after 300 attempts.
    QMap<qint64, Point> points;
    const auto pointAt = [&](qint64 time) {
        const auto found = points.constFind(time);
        if (found != points.cend()) return found.value();
        const auto point = pointOf(checkpointAt(history, in.quotes, selection, time));
        points.insert(time, point); return point;
    };

    const auto gate = [&](const Point& o, const Point& c) {
        Gate g;
        const qint64 from = o.timeMs - PreOpeningMs, to = c.timeMs + BoundaryMs;
        if (!view.upbitScopesToConfirm.isEmpty()) g.block(UpbitConfirm);
        for (const auto& s : coin) {
            if (in.flowErrors.contains(s.accountScope)) g.block(in.flowErrors.value(s.accountScope));
            if (!flows.contains(s.accountScope)) { g.block(Collecting); continue; }
            const auto& state = flows[s.accountScope];
            for (const auto source : FundFlowTracking::sources(state)) {
                const auto types = source == FlowSource::BinanceTransfers ? FundFlowTracking::transferTypes(state) : QStringList{QString{}};
                for (const auto& type : types) if (!FundFlowTracking::covers(state, source, type, from, to)) g.block(Collecting);
            }
            const auto openAt = o.snapshotAt.value(s.accountScope, o.timeMs), closeAt = c.snapshotAt.value(s.accountScope, c.timeMs);
            for (const auto& observation : state.observations) {
                const auto& r = observation.record;
                if (r.createdMs > to) continue;
                if (observation.conflict) { g.block(Changed); continue; }
                if (!r.final) { g.block(Pending); continue; }
                if (!r.success || r.doneMs <= from || r.doneMs > to) continue;
                const auto t = r.doneMs;
                if ((t > openAt - BoundaryMs && t <= o.timeMs + BoundaryMs) || (t > closeAt - BoundaryMs && t <= c.timeMs + BoundaryMs)) { g.block(Boundary); continue; }
                if (t <= o.timeMs || t > c.timeMs) continue;
                CoinFlowLine line{t, FundFlowProtocol::label(r.source), r.asset, r.amount, {}, {}};
                if (flowUpbit(r.source)) {
                    const bool in = r.source == FlowSource::UpbitDeposits;
                    if (r.asset != "KRW") { g.block(CoinMove); line.direction = "보류"; line.note = "코인 입출금"; }
                    else if (decimal::positive(r.fee)) { g.block(KrwFee); line.direction = "보류"; line.note = "수수료 " + r.fee + "원"; }
                    else {
                        AssetEvent e; e.id = r.id; e.officialId = r.id; e.accountScope = s.accountScope; e.asset = "KRW"; e.quantity = r.amount;
                        e.timeMs = t; e.kind = in ? AssetEventKind::ExternalIn : AssetEventKind::ExternalOut;
                        e.evidence = {"업비트 입출금 기록", "https://docs.upbit.com/kr/reference", s.accountScope, t, r.receivedAtMs, true};
                        g.events.append(e); line.direction = in ? "넣은 돈" : "뺀 돈";
                    }
                } else if (r.source == FlowSource::BinanceTransfers) {
                    const auto [fromWallet, toWallet] = wallets(r.type);
                    const auto scopeOf = [&](const QString& w) { return w == "MAIN" ? spotScope : w == "UMFUTURE" ? usdmScope : QString{}; };
                    const auto a = scopeOf(fromWallet), b = scopeOf(toWallet);
                    if (!a.isEmpty() && !b.isEmpty()) {
                        // Both legs must be on record under the other connection as well.
                        const auto other = a == s.accountScope ? b : a;
                        const auto references = successfulReferences.constFind(other);
                        const bool paired = references != successfulReferences.cend() && references->contains(r.reference);
                        if (paired) { line.direction = "내 계좌 사이 이동"; line.note = r.type; }
                        else { g.block(Unpaired); line.direction = "보류"; line.note = r.type; }
                    } else { g.block(CoinMove); line.direction = "보류"; line.note = r.type; }
                } else if (r.wallet == 1) { line.direction = "제외"; line.note = "Funding 지갑 (계산 범위 밖)"; }
                else { g.block(CoinMove); line.direction = "보류"; line.note = "코인 입출금"; }
                g.lines.append(line);
            }
        }
        if (!spotScope.isEmpty()) {
            if (!earn) g.block(EarnMissing);
            else {
                for (const auto source : {EarnSource::FlexibleSubscriptions, EarnSource::LockedSubscriptions, EarnSource::FlexibleRedemptions, EarnSource::LockedRedemptions})
                    if (!earnCovers(*earn, source, from, to)) g.block(Collecting);
                for (const auto& x : EarnTracking::active(*earn)) {
                    const auto& r = x.record;
                    if (earnPositions(r.source) || earnReward(r.source)) continue;
                    const bool touches = (r.timeMs > from && r.timeMs <= to) || (r.deliveryMs > from && r.deliveryMs <= to) ||
                        (!r.complete && EarnTracking::recheck(r, in.nowMs) != EarnTracking::Recheck::NotNeeded && r.timeMs <= to);
                    if (!touches) continue;
                    if (r.source == EarnSource::FlexibleRedemptions && r.destinationWallet == "FUNDING" && r.complete && !x.conflict) continue;
                    if ((r.source == EarnSource::FlexibleSubscriptions || r.source == EarnSource::LockedSubscriptions) && r.complete && !x.conflict &&
                        !decimal::positive(r.spotAmount)) continue;
                    g.block(EarnMove);
                }
            }
        }
        return g;
    };
    const auto compute = [&](CoinPeriodResult* out, const Point& o, const Point& c, bool keepLines) {
        out->fromMs = o.timeMs; out->toMs = c.timeMs;
        auto g = gate(o, c);
        if (keepLines) { view.lines = g.lines; std::sort(view.lines.begin(), view.lines.end(), [](const auto& a, const auto& b) { return a.timeMs > b.timeMs; }); }
        if (!g.reasons.isEmpty()) { out->reasons = g.reasons; out->reason = g.reasons.first(); return; }
        const auto flowResult = AssetValuation::flows(g.events, scopes, {}, "KRW", o.timeMs, c.timeMs, true);
        AssetPerformanceCoverage coverage; coverage.fromMs = o.timeMs; coverage.toMs = c.timeMs; coverage.accountScopes = scopes;
        coverage.externalFlowsComplete = true; coverage.balancesReconciled = true; coverage.hasGaps = false;
        const auto result = AssetValuation::performance(o.valuation, c.valuation, flowResult, coverage);
        if (!result.complete) { out->reason = "계산 조건을 확인하지 못했어요"; out->reasons = QStringList{out->reason} + result.issues + flowResult.issues; return; }
        QStringList in, outflows;
        for (const auto& e : g.events) (e.kind == AssetEventKind::ExternalIn ? in : outflows).append(e.quantity);
        out->opening = result.openingNetAssets; out->closing = result.closingNetAssets; out->pnl = result.investmentPnl;
        out->inflow = sumAssetAmounts(in); out->outflow = sumAssetAmounts(outflows); out->available = true;
    };

    // Closing: the newest valid instant every source has been read past.
    Point closing;
    for (auto it = times.crbegin(); it != times.crend(); ++it) {
        if (!allTracked) break;
        bool read = true;
        for (const auto& s : coin) {
            const auto& state = flows[s.accountScope];
            for (const auto source : FundFlowTracking::sources(state)) {
                const auto types = source == FlowSource::BinanceTransfers ? FundFlowTracking::transferTypes(state) : QStringList{QString{}};
                for (const auto& type : types) if (!FundFlowTracking::covers(state, source, type, *it, *it + BoundaryMs)) read = false;
            }
        }
        if (!read) continue;
        const auto p = pointAt(*it);
        if (p.valid && QSet<QString>(p.valuation.accountScopes.cbegin(), p.valuation.accountScopes.cend()) == QSet<QString>(scopes.cbegin(), scopes.cend())) { closing = p; break; }
    }
    const auto noClosing = allTracked ? QStringLiteral("입출금 확인 뒤의 잔고 기록을 기다리고 있어요") : Collecting;
    const auto firstAfter = [&](qint64 earliest) {
        Point found;
        for (const auto t : times) {
            if (t < earliest) continue;
            if (!closing.valid || t >= closing.timeMs) break;
            const auto p = pointAt(t);
            if (p.valid && QSet<QString>(p.valuation.accountScopes.cbegin(), p.valuation.accountScopes.cend()) == QSet<QString>(scopes.cbegin(), scopes.cend())) { found = p; break; }
        }
        return found;
    };

    // Today: the first valid instant after 00:00 KST, inside the read span.
    const QTimeZone seoul("Asia/Seoul");
    const auto midnight = QDateTime(QDateTime::fromMSecsSinceEpoch(in.nowMs, seoul).date(), QTime(0, 0), seoul).toMSecsSinceEpoch();
    if (!closing.valid) { view.today.reason = noClosing; }
    else if (!allTracked) { view.today.reason = Collecting; }
    else {
        const auto opening = firstAfter(qMax(midnight, coverageStart + PreOpeningMs));
        if (!opening.valid) view.today.reason = "오늘 첫 코인 잔고 기록을 기다리고 있어요";
        else compute(&view.today, opening, closing, false);
    }

    // Since start: one fixed opening for this exact set of connections.
    Point start; const auto& b = in.baseline;
    bool baselineMatches = !b.isEmpty() && canonicalPublicFormat(b.value("policy").toString()) == ValuationPolicyVersion;
    if (baselineMatches) {
        QSet<QString> stored, wanted;
        for (const auto& v : b.value("connections").toArray()) stored.insert(v.toObject().value("id").toString() + ':' + v.toObject().value("binding").toString());
        for (const auto& c : selection.connections) wanted.insert(c.connectionId + ':' + c.binding);
        QStringList storedScopes; for (const auto& v : b.value("scopes").toArray()) storedScopes.append(v.toString());
        std::sort(storedScopes.begin(), storedScopes.end());
        baselineMatches = stored == wanted && storedScopes == scopes;
    }
    if (baselineMatches) {
        start.valid = true; start.timeMs = b.value("time").toString().toLongLong();
        start.valuation.currency = "KRW"; start.valuation.netAssets = b.value("netAssets").toString(); start.valuation.knownSubtotal = start.valuation.netAssets;
        start.valuation.timeMs = start.timeMs; start.valuation.complete = start.valuation.reconciled = true; start.valuation.accountScopes = scopes;
        for (const auto& v : b.value("connections").toArray()) {
            const auto id = v.toObject().value("id").toString(); const auto at = v.toObject().value("snapshot").toString().toLongLong();
            for (const auto& s : coin) if (s.connectionId == id) start.snapshotAt.insert(s.accountScope, at);
        }
    } else if (closing.valid && allTracked) {
        start = firstAfter(coverageStart + PreOpeningMs);
        if (start.valid) {
            QJsonArray fixed;
            for (const auto& c : selection.connections) {
                qint64 at = 0; for (const auto& s : coin) if (s.connectionId == c.connectionId) at = qMax(at, start.snapshotAt.value(s.accountScope));
                fixed.append(QJsonObject{{"id", c.connectionId}, {"binding", c.binding}, {"snapshot", QString::number(at)}});
            }
            QJsonArray scopeList; for (const auto& s : scopes) scopeList.append(s);
            view.proposedBaseline = {{"kind", "coin-baseline-v1"}, {"time", QString::number(start.timeMs)}, {"netAssets", start.valuation.netAssets},
                {"policy", ValuationPolicyVersion}, {"connections", fixed}, {"scopes", scopeList}};
        }
    }
    if (!closing.valid) view.sinceStart.reason = noClosing;
    else if (!start.valid) view.sinceStart.reason = allTracked ? QStringLiteral("첫 잔고와 입출금 기록을 확인하고 있어요") : Collecting;
    else if (start.timeMs >= closing.timeMs) { view.sinceStart.fromMs = start.timeMs; view.sinceStart.reason = "기록을 시작했어요. 다음 잔고 기록부터 계산해요"; }
    else compute(&view.sinceStart, start, closing, true);
    if (view.sinceStart.fromMs > 0 && !view.sinceStart.available && view.sinceStart.reasons.isEmpty()) view.sinceStart.reasons = {view.sinceStart.reason};
    return view;
}
} // namespace keefi
