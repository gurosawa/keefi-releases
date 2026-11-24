#include "keefi/IdentityCompatibility.h"
#include "keefi/FundFlowCollector.h"
#include "keefi/AssetsLedger.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTimer>

namespace keefi {
namespace {
QString binding(const AccountRecord& account, const CredentialSet& credentials) {
    QCryptographicHash h(QCryptographicHash::Sha256); h.addData(identityDomain(IdentityDomain::CurrentAssetsCredentialBinding));
    for (const auto& part : {account.provider, account.environment, account.market, account.region}) {
        const auto b = part.toUtf8(); h.addData(QByteArray::number(b.size())); h.addData(":"); h.addData(b);
    }
    for (const auto& [name, value] : credentials) {
        h.addData(name.toUtf8()); h.addData(":"); h.addData(QByteArray::number(value.size())); h.addData(":");
        h.addData(QByteArrayView(reinterpret_cast<const char*>(value.data()), qsizetype(value.size())));
    }
    return QString::fromLatin1(h.result().toHex());
}
QString errorText(const QString& code, bool upbit) {
    if (code == "PERMISSION") return upbit ? "업비트 키에 입금·출금 조회 권한이 없어요. 업비트 API 키에 [입금조회]·[출금조회]를 추가해 주세요."
                                           : "바이낸스 키의 읽기 권한을 확인해 주세요.";
    if (code == "AUTHENTICATION") return upbit ? "업비트 키 인증 또는 허용 IP를 확인해 주세요." : "바이낸스 읽기 권한·허용 IP를 확인해 주세요.";
    if (code == "RATE_LIMIT") return "거래소 요청 제한으로 잠시 멈췄어요. 저장된 기록은 그대로예요.";
    if (code == "NETWORK") return "연결 문제로 입출금 기록을 모두 받지 못했어요. 저장된 기록은 그대로예요.";
    if (code == "CLOCK_SKEW") return "기기와 거래소의 시간이 맞지 않아요. 기기의 날짜·시간을 자동으로 설정한 뒤 다시 확인해 주세요.";
    if (code.startsWith("STORAGE")) return "입출금 기록의 저장 완료를 확인하지 못했어요. 잠시 뒤 다시 확인해요.";
    if (code == "BINDING") return "계좌 연결 정보를 다시 확인한 뒤 조회해 주세요.";
    if (code == "TOO_MANY_PAGES") return "입출금 기록이 너무 많아 한 번에 확인하지 못했어요.";
    return "입출금 응답의 형식·중복을 확인하지 못해 조회를 멈췄어요. 저장된 기록은 그대로예요.";
}
} // namespace
struct FundFlowCollector::Private {
    FundFlowCollector* q; Vault& vault; AssetsLedger& ledger; TradingTransport* transport; TestHooks hooks;
    QTimer timer; QElapsedTimer serverAge, storageAge, lastSent;
    bool active = false, waitingTime = false, waitingWrite = false, planned = false, confirming = false;
    quint64 token = 0, awaiting = 0, receipt = 0;
    qint64 serverTime = 0, runThrough = 0, runStartedAt = 0;
    int jobTotal = -1;
    AccountRecord account; FlowAccountState state; QList<FlowJob> jobs; FlowBatch batch;
    QMap<QString, FlowRecord> rows; // (source, raw ID) -> latest row in this run.
    FlowAccountState candidate;
    QString message, code, scope;
    Private(FundFlowCollector* owner, Vault& v, AssetsLedger& l, TradingTransport* t, TestHooks h)
        : q(owner), vault(v), ledger(l), transport(t), hooks(std::move(h)) {
        if (!transport) transport = new NetworkTradingTransport(q);
        timer.setSingleShot(true);
    }
    qint64 now() const { return hooks.nowMs ? hooks.nowMs() : QDateTime::currentMSecsSinceEpoch(); }
    bool upbit() const { return state.provider == "upbit"; }
    void reset() {
        active = waitingTime = waitingWrite = planned = confirming = false; timer.stop(); ++token; awaiting = receipt = 0; transport->abort();
        jobs.clear(); batch = {}; rows.clear(); candidate = {}; account = {}; state = {}; serverTime = runThrough = runStartedAt = 0; jobTotal = -1;
    }
    void fail(const QString& why) {
        const bool wasUpbit = upbit(); reset(); code = why; message = errorText(why, wasUpbit);
        emit q->changed(); emit q->finished(false);
    }
    void clock() {
        RequestSpec r; r.operation = Operation::Time; r.url = QUrl("https://api.binance.com/api/v3/time");
        waitingTime = true; awaiting = ++token; transport->send(awaiting, r);
    }
    void plan() {
        if (state.revision == 0) state.trackingStartMs = runThrough;
        auto planState = state;
        if (planState.trackingStartMs > runThrough) {
            if (planState.trackingStartMs - runThrough > 120000) { fail("CLOCK_SKEW"); return; }
            planState.trackingStartMs = runThrough;
        }
        jobs = FundFlowTracking::plan(planState, runThrough); planned = true;
        if (jobs.isEmpty()) fail("QUERY_INVALID");
    }
    void advance() {
        if (!active || !vault.isUnlocked()) return;
        if (waitingWrite) { save(); return; }
        if (!planned) { if (upbit()) { runThrough = runStartedAt; plan(); if (!active) return; } else { clock(); return; } }
        if (jobs.isEmpty()) { finish(); return; }
        if (!upbit() && (!serverAge.isValid() || serverAge.elapsed() > 30000)) { clock(); return; }
        const int pacing = upbit() ? hooks.upbitPacingMs : hooks.binancePacingMs;
        if (const auto wait = pacing > 0 && lastSent.isValid() ? pacing - lastSent.elapsed() : 0; wait > 0) { timer.start(int(wait)); return; } // One clock read.
        const auto serverNow = upbit() ? now() : serverTime + serverAge.elapsed();
        if (!upbit() && (qAbs(serverNow - now()) > 120000 || jobs.first().query.throughMs > serverNow)) { fail("CLOCK_SKEW"); return; }
        RequestSpec r; QString error; bool matching = false;
        const bool borrowed = vault.withCredentials(account.id, [&](const CredentialSet& c) {
            matching = binding(account, c) == state.binding;
            if (matching) r = FundFlowProtocol::request(jobs.first().query, c, serverNow, &error);
        }, &error);
        if (!borrowed || !matching) { fail("BINDING"); return; }
        if (!allowedFundFlowRead(r)) { fail("QUERY_INVALID"); return; }
        awaiting = ++token; lastSent.start();
        message = FundFlowProtocol::label(jobs.first().query.source) + " 기록 확인 중"; emit q->changed();
        transport->send(awaiting, r);
    }
    void complete(const FlowJob& job, qint64 received) {
        if (job.coverThroughMs >= job.coverFromMs && job.coverFromMs > 0)
            batch.windows.append({job.query.source, job.query.transferType, job.coverFromMs, job.coverThroughMs, received});
        jobs.removeFirst(); jobTotal = -1; timer.start(0);
    }
    void receive(quint64 id, const HttpResult& h) {
        if (!active || !vault.isUnlocked() || id != awaiting) return;
        awaiting = 0;
        if (waitingTime) {
            waitingTime = false; QString error; const auto doc = parseExactJson(h.body, &error); bool ok = false;
            const auto value = doc.object().value("serverTime").toString().toLongLong(&ok);
            if (h.status != 200 || h.networkError || h.timedOut || !ok || value <= 0 || value > 253402300799999LL || !error.isEmpty()) { fail("SERVER_TIME"); return; }
            if (qAbs(value - now()) > 120000) { fail("CLOCK_SKEW"); return; }
            serverTime = value; serverAge.start();
            if (!planned) { runThrough = qMin(runStartedAt, value); plan(); if (!active) return; }
            timer.start(0); return;
        }
        if (jobs.isEmpty()) return;
        const auto received = now();
        auto& job = jobs.first();
        const auto page = FundFlowProtocol::parsePage(job.query, h, received);
        if (!page.ok) { fail(page.errorCode); return; }
        for (const auto& row : page.rows) rows.insert(QString::number(int(row.source)) + ':' + row.id, row);
        batch.receivedAtMs = received;
        const auto source = job.query.source;
        if (flowUpbit(source)) {
            if (!job.query.uuids.isEmpty()) { complete(job, received); return; }
            qint64 oldest = 0; for (const auto& row : page.rows) oldest = oldest ? qMin(oldest, row.createdMs) : row.createdMs;
            if (page.rows.size() == FundFlowProtocol::UpbitPageSize && oldest >= job.stopAtMs) {
                if (job.query.page >= FundFlowTracking::MaxUpbitPages) { fail("TOO_MANY_PAGES"); return; }
                ++job.query.page; timer.start(0); return;
            }
            complete(job, received); return;
        }
        if (source == FlowSource::BinanceTransfers) {
            if (jobTotal >= 0 && page.total != jobTotal) { fail("PAGE_CHANGED"); return; }
            jobTotal = page.total;
            if (qint64(job.query.page) * FundFlowProtocol::TransferPageSize < page.total) {
                if (page.rows.size() != FundFlowProtocol::TransferPageSize) { fail("PAGE_SHORT"); return; }
                ++job.query.page; timer.start(0); return;
            }
            complete(job, received); return;
        }
        if (page.rows.size() == FundFlowProtocol::BinanceLimit) { job.query.offset += FundFlowProtocol::BinanceLimit; timer.start(0); return; }
        complete(job, received);
    }
    void finish() {
        batch.rows = rows.values();
        if (!batch.receivedAtMs) batch.receivedAtMs = now();
        waitingWrite = true; receipt = 0; storageAge.start(); save();
    }
    FlowAccountState latest() const {
        for (const auto& s : ledger.flowStates()) if (s.accountScope == state.accountScope) return s;
        return state.revision == 0 ? state : FlowAccountState{};
    }
    void save() {
        if (!active || !vault.isUnlocked()) return;
        if (receipt) { if (storageAge.elapsed() > 30000) fail("STORAGE_CONFIRMATION"); else timer.start(50); return; }
        if (ledger.isWriting()) { if (storageAge.elapsed() > 30000) fail("STORAGE_BUSY"); else timer.start(50); return; }
        if (!ledger.isOpen()) { fail("STORAGE_LOCKED"); return; }
        auto base = latest();
        if (base.accountScope.isEmpty() || base.binding != state.binding || base.connectionId != state.connectionId) { fail("BINDING"); return; }
        QString error; bool merged = false;
        if (confirming) {
            const auto confirmedAt = candidate.upbitConfirmedAtMs;
            candidate = base; candidate.upbitConfirmedAtMs = confirmedAt; candidate.revision = base.revision + 1; merged = true;
        }
        else {
            const bool borrowed = vault.withAssetsLedgerKey(false, [&](const SecureBuffer& key) { merged = FundFlowTracking::merge(base, batch, key, &candidate, &error); }, &error);
            if (!borrowed) merged = false;
        }
        if (!merged) { fail("STORAGE_MERGE"); return; }
        static quint64 nextReceipt = 0; receipt = ++nextReceipt; if (!receipt) receipt = ++nextReceipt;
        const auto result = ledger.saveFlowState(candidate, base.revision, receipt, &error);
        if (result == AssetsLedger::CollectionWrite::Failed) { fail("STORAGE_WRITE"); return; }
        if (result == AssetsLedger::CollectionWrite::Unchanged) committed(true);
        else timer.start(100);
    }
    void committed(bool ok) {
        if (!active || !waitingWrite) return;
        if (!ok) { fail("STORAGE_WRITE"); return; }
        const bool confirmation = confirming, wasUpbit = upbit();
        const auto confirmedAt = candidate.upbitConfirmedAtMs;
        reset(); code.clear();
        message = confirmation ? (confirmedAt ? "업비트 포켓·스테이킹 미사용 확인을 저장했어요." : "업비트 포켓·스테이킹 확인을 취소했어요.")
                               : QString("%1 입출금 기록 확인을 마쳤어요.").arg(wasUpbit ? "업비트" : "바이낸스");
        emit q->changed(); emit q->finished(true);
    }
};
FundFlowCollector::FundFlowCollector(Vault& v, AssetsLedger& l, TradingTransport* t, QObject* parent)
    : FundFlowCollector(v, l, t, TestHooks{}, parent) {}
FundFlowCollector::FundFlowCollector(Vault& v, AssetsLedger& l, TradingTransport* t, TestHooks h, QObject* parent)
    : QObject(parent), d_(std::make_unique<Private>(this, v, l, t, std::move(h))) {
    connect(&d_->timer, &QTimer::timeout, this, [this] { d_->advance(); });
    connect(d_->transport, &TradingTransport::finished, this, [this](quint64 id, const HttpResult& h) { d_->receive(id, h); });
    connect(&v, &Vault::aboutToLock, this, &FundFlowCollector::stop);
    connect(&v, &Vault::accountsChanged, this, &FundFlowCollector::stop);
    connect(&l, &AssetsLedger::locked, this, &FundFlowCollector::stop);
    connect(&l, &AssetsLedger::collectionWriteFinished, this, [this](quint64 receipt, bool ok, const QString&) {
        if (d_->active && d_->waitingWrite && receipt == d_->receipt) d_->committed(ok);
    });
}
FundFlowCollector::~FundFlowCollector() { disconnect(d_->transport, nullptr, this, nullptr); d_->reset(); }
bool FundFlowCollector::supports(const AssetSnapshot& s) {
    return s.environment == "live" && (s.provider == "upbit" || (s.provider == "binance" && (s.market == "spot" || s.market == "usdm_futures")));
}
bool FundFlowCollector::start(const AssetSnapshot& snapshot) {
    if (d_->active) return false;
    d_->reset(); d_->code.clear();
    const auto refuse = [&](const QString& why) { d_->code = why; d_->message = errorText(why, snapshot.provider == "upbit"); emit changed(); return false; };
    if (!d_->vault.isUnlocked() || !d_->ledger.isOpen() || !supports(snapshot) || !snapshot.balanceComplete || !snapshot.evidence.complete ||
        snapshot.connectionBinding.isEmpty() || snapshot.timeMs <= 0 || snapshot.timeMs > d_->now() + 2000 ||
        d_->now() - snapshot.timeMs > DisplayFreshnessMs) return refuse("BINDING");
    for (const auto& a : d_->vault.accounts()) if (a.id == snapshot.connectionId) d_->account = a;
    if (d_->account.provider != snapshot.provider || d_->account.environment != "live" || d_->account.market != snapshot.market) return refuse("BINDING");
    d_->state = {snapshot.accountScope, snapshot.connectionId, snapshot.connectionBinding, snapshot.provider, snapshot.market};
    d_->state.trackingStartMs = d_->now();
    for (const auto& s : d_->ledger.flowStates()) if (s.accountScope == snapshot.accountScope) {
        if (s.binding != snapshot.connectionBinding || s.connectionId != snapshot.connectionId || s.provider != snapshot.provider || s.market != snapshot.market)
            return refuse("BINDING");
        d_->state = s;
    }
    if (!FundFlowTracking::validAccount(d_->state)) return refuse("BINDING");
    d_->active = true; d_->scope = snapshot.accountScope; d_->runStartedAt = d_->now();
    d_->message = QString("%1 입출금 기록 확인 준비").arg(d_->upbit() ? "업비트" : "바이낸스");
    emit started(snapshot.accountScope); emit changed();
    if (d_->active) d_->timer.start(0);
    return d_->active;
}
bool FundFlowCollector::setUpbitConfirmation(const QString& accountScope, bool confirmed) {
    if (d_->active || !d_->vault.isUnlocked() || !d_->ledger.isOpen()) return false;
    for (const auto& s : d_->ledger.flowStates()) if (s.accountScope == accountScope && s.provider == "upbit") {
        d_->reset(); d_->code.clear();
        d_->state = s; d_->candidate = s; d_->candidate.upbitConfirmedAtMs = confirmed ? d_->now() : 0;
        if (d_->candidate.upbitConfirmedAtMs > 0 && s.upbitConfirmedAtMs > 0) return true;
        d_->active = d_->confirming = d_->waitingWrite = true; d_->scope = accountScope; d_->storageAge.start();
        d_->message = "업비트 확인 내용을 저장하고 있어요."; emit changed();
        d_->save(); return d_->active;
    }
    return false;
}
void FundFlowCollector::stop() { const bool was = d_->active; d_->reset(); emit changed(); if (was) emit finished(false); }
bool FundFlowCollector::busy() const { return d_->active; }
QString FundFlowCollector::status() const { return d_->message; }
QString FundFlowCollector::errorCode() const { return d_->code; }
QString FundFlowCollector::accountScope() const { return d_->scope; }
} // namespace keefi
