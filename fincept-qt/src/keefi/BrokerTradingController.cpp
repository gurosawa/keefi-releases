#include "keefi/BuildPolicy.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/TossAdapter.h"
#include "keefi/KiwoomAdapter.h"
#include "keefi/KiwoomRequestBudget.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QWidget>
#include <algorithm>
#include <exception>
#include <limits>
#include <utility>
#include <sodium.h>

namespace keefi {
namespace {
QString displayTime(const QString& iso) {
    const auto instant = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return instant.isValid() ? instant.toTimeZone(QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss 'KST'") : QStringLiteral("미확인");
}
bool cancelOperation(Operation op){return op==Operation::CancelOrder||op==Operation::AlgoCancelOrder;}
bool foreground(QWidget* origin) {
    return origin && origin->isVisible() && origin->isEnabled() && origin->hasFocus() &&
        origin->window() == QApplication::activeWindow() && QApplication::applicationState() == Qt::ApplicationActive;
}
bool bounded(const OrderInput& input) {
    for (const auto& value : {input.symbol, input.side, input.type, input.price, input.quantity,
         input.quoteAmount, input.orderId, input.clientId, input.newClientId, input.operationOrderId, input.baseOriginalOrderId})
        if (value.size() > 128) return false;
    return (input.quantityMode == OrderQuantityMode::Exact || input.quantityMode == OrderQuantityMode::AllRemaining) &&
        (input.baseOriginalProvenance == OrderReferenceProvenance::None || input.baseOriginalProvenance == OrderReferenceProvenance::BrokerReceipt);
}
void wipe(QByteArray& bytes) {
    bytes.detach(); if (!bytes.isEmpty()) sodium_memzero(bytes.data(), bytes.size()); bytes.clear();
}
QString orderKey(const OrderRecord& order) { return order.symbol + QChar(0x1f) + order.orderId; }
void mergeOrders(QList<OrderRecord>& target, const QList<OrderRecord>& incoming) {
    for (const auto& order : incoming) {
        const auto it = std::find_if(target.begin(), target.end(), [&](const OrderRecord& current) {
            return orderKey(current) == orderKey(order);
        });
        if (it == target.end()) target.append(order); else *it = order;
    }
}
OrderInput journalInput(const QJsonObject& entry) {
    OrderInput input;
    input.symbol = entry.value("symbol").toString(); input.side = entry.value("side").toString();
    input.type = entry.value("type").toString(); input.price = entry.value("price").toString();
    input.quantity = entry.value("quantity").toString(); input.quoteAmount = entry.value("quoteAmount").toString();
    input.orderId = entry.value("orderId").toString(); input.clientId = entry.value("clientId").toString();
    input.brokerConditional=entry.value("brokerConditional")=="true";
    input.newClientId = entry.value("replacementClientId").toString();
    input.operationOrderId = entry.value("operationOrderId").toString();
    input.quantityMode = entry.value("quantityMode") == "all_remaining" ? OrderQuantityMode::AllRemaining : OrderQuantityMode::Exact;
    input.baseOriginalOrderId = entry.value("baseOriginalOrderId").toString();
    input.baseOriginalProvenance = entry.value("baseOriginalProvenance") == "broker_receipt" ? OrderReferenceProvenance::BrokerReceipt : OrderReferenceProvenance::None;
    return input;
}
}

struct BrokerTradingController::Private {
    enum class Purpose { Authentication, Refresh, Catalog, Reconciliation, Mutation, BatchScope, Preflight, CancelPreflight };
    struct Job {
        Operation operation; OrderInput input; Purpose purpose; QString journalId;
        BrokerStep step;
        quint64 draftRevision = 0;
        QByteArray inputBinding;
        QPointer<QWidget> origin;
        qint64 firstPageReceived = -1;
        QList<BrokerEvidence> usedDependencies;
        QList<OrderRecord> stagedHistoryOrders;
    };
    struct Reconciliation {
        int remaining = 0;
        bool failed = false;
        QString associatedId;
        QList<OrderRecord> observed;
    };
    BrokerTradingController* q;
    Vault& vault;
    TradingTransport* transport;
    std::shared_ptr<KiwoomRequestBudget> kiwoomBudget;
    quint64 kiwoomLease = 0;
    std::unique_ptr<BrokerAdapter> adapter;
    AdapterFactory adapterFactory;
    AccountRecord connection;
    QString fingerprint, verifiedAccountScope, market, message = "증권 연결을 선택하세요.", coverage;
    MarketSnapshot snapshot;
    QMap<Operation, qint64> receivedAt;
    QMap<Operation, QString> receivedWallTime;
    QMap<QString, Reconciliation> reconciliations;
    QMap<QString, OrderRecord> activity;
    QList<OrderRecord> open, closed, batch, batchReviewed;
    QList<MarketInstrument> catalog, catalogStaging;
    QJsonObject checks;
    QJsonArray catalogSourceRecords;
    QSet<QString> failedCatalogMarkets;
    OrderLimits limits;
    QQueue<Job> jobs;
    Job current{Operation::Authenticate, {}, Purpose::Authentication, {}};
    QElapsedTimer clock;
    QTimer automatic, preflightTimer, readinessTimer;
    QList<BrokerEvidence> evidence;
    OrderInput draft;
    quint64 draftRevision = 0, completedDraftRevision = 0;
    QByteArray completedBinding;
    bool draftPending = false, accountReadVerified = false, historyIncomplete = false;
    bool assetsOnly = false;
    QPointer<QWidget> batchOrigin;
    quint64 generation = 0, token = 0, readinessGeneration = 0;
    qint64 pacingUntil = 0, cooldownUntil = 0, catalogAt = -1;
    QString catalogFailure, catalogWallTime;
    bool active = false, connected = false, enabled = false, halted = false;
    QList<OrderRecord> conditionalRows,conditionalStaging;bool conditionalComplete=false;qint64 conditionalAt=-1;
    bool historyOnly=false,historyRequested=false;
    bool refreshing = false, catalogLoading = false, manualReconnect = false, hadSession = false;
    int pages = 0;
    QSet<QString> cursors;

    Private(BrokerTradingController* owner, Vault& store, TradingTransport* wire, AdapterFactory factory,
            std::shared_ptr<KiwoomRequestBudget> budget)
        : q(owner), vault(store), transport(wire), kiwoomBudget(budget ? std::move(budget) : KiwoomRequestBudget::shared()),
          adapterFactory(std::move(factory)) { clock.start(); }
    bool kiwoom() const { return connection.provider == "kiwoom"; }
    void updateKiwoomPending() { kiwoomBudget->setBrokerPending(q, kiwoom() && (active || !jobs.isEmpty())); }
    void abortRequest() {
        const auto lease = std::exchange(kiwoomLease, 0);
        transport->abort(); active = false;
        if (lease) kiwoomBudget->finish(lease);
    }
    void armReadinessNotification() {
        readinessTimer.stop();
        if (!adapter) return;
        const qint64 now = clock.elapsed();
        qint64 deadline = 0;
        for (const auto until : {pacingUntil, cooldownUntil})
            if (until > now && (deadline == 0 || until < deadline)) deadline = until;
        if (kiwoom() && !active && jobs.isEmpty()) {
            const auto wait = kiwoomBudget->remainingMs();
            if (wait > 0) {
                const auto until = wait > std::numeric_limits<qint64>::max() - now ? std::numeric_limits<qint64>::max() : now + wait;
                if (!deadline || until < deadline) deadline = until;
            }
        }
        if (!deadline) return;
        readinessGeneration = generation;
        readinessTimer.start(int(qMin<qint64>(deadline - now, std::numeric_limits<int>::max())));
    }
    void change(const QString& text = {}) {
        if (!text.isEmpty()) message = text;
        updateKiwoomPending();
        armReadinessNotification(); emit q->changed();
    }
    void verified(const QString& name) { checks[name] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
    bool session() const { return adapter && adapter->sessionReady(); }
    QString scope() const { return adapter ? adapter->selectedAccountScope() : QString{}; }
    bool matching(const QJsonObject& entry) const {
        return !scope().isEmpty() && entry.value("provider").toString() == connection.provider && entry.value("environment").toString() == connection.environment &&
            entry.value("market") == "domestic_equity" && entry.value("accountScope").toString() == scope();
    }
    bool owns(const QJsonObject& entry) const {
        // A client credential can be replaced while the account remains the
        // same. Keep its original fingerprint in the encrypted audit record,
        // but reconcile only after this session has successfully read the exact
        // server-derived account identity selected for the request.
        return !verifiedAccountScope.isEmpty() && verifiedAccountScope == scope() && matching(entry);
    }
    QJsonArray pending() const {
        QJsonArray rows;
        for (const auto& value : vault.pendingOperations()) if (matching(value.toObject())) rows.append(value);
        return rows;
    }
    bool pendingForOrder(const QString& symbol, const QString& orderId) const {
        if (orderId.isEmpty()) return false;
        for (const auto& value : pending()) {
            const auto entry = value.toObject();
            if (entry.value("symbol").toString() == symbol &&
                (entry.value("orderId").toString() == orderId || entry.value("operationOrderId").toString() == orderId)) return true;
        }
        return false;
    }
    QJsonObject journal(const QString& id) const {
        for (const auto& value : vault.pendingOperations()) if (value.toObject().value("id") == id) return value.toObject();
        return {};
    }
    void revoke() {
        enabled = false; completedBinding.clear(); completedDraftRevision = 0;
        draft = {}; ++draftRevision; draftPending = false; preflightTimer.stop();
        evidence.erase(std::remove_if(evidence.begin(), evidence.end(), [](const auto& item) { return item.draftRevision != 0; }), evidence.end());
        emit q->intentRevoked();
    }
    void clearWork() {
        ++generation; ++token; abortRequest();
        readinessTimer.stop();
        current.usedDependencies.clear();
        jobs.clear(); batch.clear(); batchReviewed.clear(); batchOrigin.clear(); catalogStaging.clear();
        preflightTimer.stop(); draftPending = false; completedBinding.clear(); completedDraftRevision = 0; ++draftRevision;
        reconciliations.clear(); refreshing = catalogLoading = historyOnly = false;
        updateKiwoomPending();
    }
    void clearReadState() {
        connected = false; verifiedAccountScope.clear(); snapshot = {}; receivedAt.clear(); receivedWallTime.clear(); open.clear(); closed.clear();
        conditionalRows.clear();conditionalStaging.clear();conditionalComplete=false;conditionalAt=-1;
        activity.clear(); market.clear(); coverage.clear(); evidence.clear(); accountReadVerified = false; historyIncomplete = false; draft = {};
        preflightTimer.stop(); draftPending = false; completedBinding.clear(); completedDraftRevision = 0;
    }
    void loseSession(const QString& reason) {
        clearWork(); revoke(); clearReadState();
        if (adapter) adapter->clearSession();
        hadSession = false; catalog.clear(); catalogAt = -1; catalogFailure.clear(); catalogWallTime.clear();
        catalogSourceRecords = {}; failedCatalogMarkets.clear();
        checks = {}; change(reason);
    }
    bool saveJournal(const QJsonObject& entry, bool remove = false) {
        auto records = vault.pendingOperations();
        int found = -1;
        for (int i = 0; i < records.size(); ++i) if (records[i].toObject().value("id") == entry.value("id")) { found = i; break; }
        if (found >= 0) { if (remove) records.removeAt(found); else records[found] = entry; }
        else if (!remove) records.append(entry);
        QString error;
        if (vault.setPendingOperations(records, &error)) return true;
        clearWork(); revoke(); connected = false;
        change(error.isEmpty() ? "주문 기록 저장에 실패하여 남은 요청을 중단했습니다." : error);
        return false;
    }

    bool fresh(Operation op, qint64 age) const { return receivedAt.contains(op) && clock.elapsed() - receivedAt.value(op) <= age; }
    BrokerPolicy policy() const { return adapter ? adapter->policy() : BrokerPolicy{}; }
    BrokerStep stepFor(const Job& job) const {
        auto step = job.step;
        if (step.variant.isEmpty() && step.catalogMarket.isEmpty() && job.purpose != Purpose::Authentication && job.purpose != Purpose::Refresh)
            step = adapter->directStep(job.operation, job.input);
        step.operation = job.operation; step.input = job.input;
        if (!job.input.cursor.isEmpty() && step.catalogMarket.isEmpty()) step.cursorHandle = job.input.cursor;
        return step;
    }
    QByteArray binding(const OrderInput& input) const {
        const QJsonObject data{{"provider", connection.provider}, {"environment", connection.environment},
            {"market", connection.market}, {"scope", scope()}, {"generation", QString::number(generation)},
            {"symbol", input.symbol}, {"side", input.side}, {"type", input.type}, {"price", input.price},
            {"quantity", input.quantity}, {"quoteAmount", input.quoteAmount}, {"orderId", input.orderId},
            {"quantityMode", input.quantityMode == OrderQuantityMode::AllRemaining ? "all_remaining" : "exact"}};
        return QCryptographicHash::hash(QJsonDocument(data).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    }
    bool enqueuePlan(BrokerPlanKind kind, const OrderInput& input, Purpose purpose, bool background = false) {
        const auto plan = adapter->plan(kind, input);
        if (plan.isEmpty()) return false;
        for (const auto& step : plan) {
            if (isMutation(step.operation) || step.operation == Operation::TestOrder ||
                (kind != BrokerPlanKind::Authenticate && step.operation == Operation::Authenticate) ||
                (step.catalogAuxiliary && (kind != BrokerPlanKind::Catalog || step.authScope != BrokerAuthScope::Public)) ||
                (!step.dependencies.isEmpty() && purpose != Purpose::Preflight) ||
                (step.optionalAccountRead && (kind != BrokerPlanKind::AccountRead || purpose != Purpose::Refresh ||
                    step.operation != Operation::History || step.authScope != BrokerAuthScope::SelectedAccount))) return false;
        }
        for (const auto& step : plan) {
            if (background && step.operation == Operation::History) continue;
            Job job{step.operation, step.input, purpose, {}, step};
            if (purpose == Purpose::Preflight) {
                job.draftRevision = draftRevision; job.inputBinding = binding(input);
            }
            jobs.enqueue(job);
        }
        return !jobs.isEmpty();
    }
    const BrokerEvidence* latestEvidence(Operation op) const {
        for (auto it = evidence.crbegin(); it != evidence.crend(); ++it)
            if (it->operation == op && it->sessionGeneration == generation && it->accountScope == scope() && it->complete) return &*it;
        return nullptr;
    }
    bool evidenceFresh(const BrokerEvidence& item, const BrokerFreshnessRule& rule) const {
        const auto receiptAge = clock.elapsed() - item.receivedMonotonicMs;
        if (item.receivedMonotonicMs < 0 || receiptAge < 0 || receiptAge > rule.maxReceiptAgeMs) return false;
        if (rule.acceptedSourceQuality == SourceTimeQuality::NotProvided) return true;
        if (rule.acceptedSourceQuality == SourceTimeQuality::TimeOfDayOnly)
            return item.sourceTime.quality == SourceTimeQuality::TimeOfDayOnly || item.sourceTime.quality == SourceTimeQuality::FullTimestamp;
        if (!adapter || item.sourceTime.quality != SourceTimeQuality::FullTimestamp || !item.sourceTime.fullTimestamp.isValid()) return false;
        const auto now = adapter->utcNow(); if (!now.isValid()) return false;
        const qint64 age = item.sourceTime.fullTimestamp.msecsTo(now);
        return age >= -2000 && age <= rule.maxSourceAgeMs;
    }
    bool sourceFresh(Operation op) const {
        const auto* item = latestEvidence(op); if (!item) return false;
        BrokerFreshnessRule selected; selected.operation = op;
        for (const auto& rule : policy().freshness.value(BrokerAction::Place)) if (rule.operation == op) selected = rule;
        return evidenceFresh(*item, selected);
    }
    BrokerReadiness readiness(BrokerAction action, const OrderInput& input) const {
        if (!adapter) return {false, BrokerNotReady::NotImplemented, "증권 연결을 선택하세요."};
        const auto feature = policy().actions.value(action);
        if (!feature.implemented) return {false, feature.api == ApiAvailability::ExplicitlyUnsupported ? BrokerNotReady::UnsupportedApi : BrokerNotReady::NotImplemented,
            feature.explanation.isEmpty() ? QStringLiteral("이 환경의 동작은 아직 구현되지 않았습니다.") : feature.explanation};
        if (!session()) return {false, BrokerNotReady::NotAuthenticated, "인증이 필요합니다."};
        if (!connected || verifiedAccountScope.isEmpty() || verifiedAccountScope != scope()) return {false, BrokerNotReady::AccountNotVerified, "선택 계좌의 조회 검증이 필요합니다."};
        if(input.brokerConditional){
            if(connection.provider!="toss"||action!=BrokerAction::Cancel||!conditionalComplete||conditionalAt<0||clock.elapsed()-conditionalAt>10000)
                return {false,BrokerNotReady::MissingEvidence,"조건 감시 목록을 최신 정보로 전체 조회하세요."};
            const auto found=std::find_if(conditionalRows.cbegin(),conditionalRows.cend(),[&](const auto& row){return row.orderId==input.orderId&&row.symbol==input.symbol;});
            if(found==conditionalRows.cend()||!found->cancelable||!found->brokerConditional||found->market!="domestic_equity")return {false,BrokerNotReady::ChangedInput,"현재 취소 가능한 국내 조건 감시를 선택하세요."};
            return {true,BrokerNotReady::None,"조건 감시 취소 준비 완료 · 발생한 일반 주문은 별도 관리"};
        }
        if (action == BrokerAction::Place && !policy().placeOrderTypes.contains(input.type))
            return {false, BrokerNotReady::NotImplemented, input.type == "market"
                ? QStringLiteral("이 연결의 시장가 신규 주문은 아직 구현하지 않았습니다.")
                : QStringLiteral("이 연결에서 지원하지 않는 신규 주문 유형입니다.")};
        const auto expectedMode = action == BrokerAction::Cancel ? policy().cancelQuantityMode :
            action == BrokerAction::Amend ? policy().amendQuantityMode : OrderQuantityMode::Exact;
        if (action != BrokerAction::Read && input.quantityMode != expectedMode)
            return {false, BrokerNotReady::RejectedBySafetyRule, "이 동작에서 지원하는 수량 모드를 확인하세요."};
        if (input.quantityMode == OrderQuantityMode::AllRemaining ||
            (action == BrokerAction::Cancel && policy().cancelExplicitQtyBasis == ExplicitQtyBasis::Remaining)) {
            const auto found = std::find_if(open.cbegin(), open.cend(), [&](const auto& row) { return row.orderId == input.orderId && row.symbol == input.symbol; });
            if (found == open.cend() || !decimal::positive(input.quantity) || !decimal::valid(found->remaining) ||
                decimal::compare(input.quantity, found->remaining) != 0)
                return {false, BrokerNotReady::ChangedInput, "확인한 미체결 잔량이 바뀌었습니다. 원주문을 다시 불러오세요."};
        }
        if (action == BrokerAction::Amend && policy().amendPriceOnly) {
            const auto found = std::find_if(open.cbegin(), open.cend(), [&](const auto& row) { return row.orderId == input.orderId && row.symbol == input.symbol; });
            if (found == open.cend() || input.quantityMode != OrderQuantityMode::Exact ||
                !decimal::valid(found->executed) || decimal::compare(found->executed, "0") != 0 ||
                !decimal::positive(found->quantity) || !decimal::positive(found->remaining) || !decimal::positive(input.quantity) ||
                decimal::compare(found->quantity, found->remaining) != 0 || decimal::compare(input.quantity, found->remaining) != 0 ||
                input.side != found->side || input.type != "limit" || found->type != "limit" ||
                !decimal::positive(input.price) || !decimal::positive(found->price) || decimal::compare(input.price, found->price) == 0)
                return {false, BrokerNotReady::ChangedInput, "체결되지 않은 지정가 주문의 잔량 전부를 유지하고 가격만 변경할 수 있습니다. 원주문을 다시 확인하세요."};
        }
        if (action == BrokerAction::Place || action == BrokerAction::Amend) {
            for (const auto& instrument : catalog) if (instrument.symbol == input.symbol && failedCatalogMarkets.contains(instrument.market))
                return {false, BrokerNotReady::MissingEvidence, "이 종목의 공식 상품 분류를 확인하지 못했습니다. 목록을 다시 조회하세요."};
        }
        if (policy().exactIntentPreflight && (action == BrokerAction::Place || action == BrokerAction::Amend) &&
            (completedDraftRevision != draftRevision || completedBinding != binding(input)))
            return {false, BrokerNotReady::ChangedInput, "입력한 주문 조건의 사전 조회가 필요합니다. 완료 후 직접 주문하세요."};
        for (const auto& rule : policy().freshness.value(action)) {
            if (!rule.requiredForAction) continue;
            const BrokerEvidence* item = nullptr;
            for (auto it = evidence.crbegin(); it != evidence.crend(); ++it) {
                if (it->operation == rule.operation && it->sessionGeneration == generation && it->accountScope == scope() && it->complete &&
                    (it->draftRevision == 0 || (it->draftRevision == draftRevision && it->inputBinding == binding(input)))) { item = &*it; break; }
            }
            if (!item || !evidenceFresh(*item, rule)) return {false, BrokerNotReady::ExpiredEvidence, "주문 조건의 수신 또는 원본 시각이 미확인·만료되었습니다. 다시 조회하세요."};
        }
        QList<BrokerEvidence> relevant;
        const auto wanted = binding(input);
        for (const auto& item : evidence) if (item.sessionGeneration == generation && item.accountScope == scope() && item.complete &&
            (item.draftRevision == 0 || (item.draftRevision == draftRevision && item.inputBinding == wanted))) {
            bool freshEnough = true;
            for (const auto& rule : policy().freshness.value(action)) if (rule.operation == item.operation && !evidenceFresh(item, rule)) freshEnough = false;
            if (freshEnough) relevant.append(item);
        }
        return adapter->actionReadiness(action, input, relevant, limits);
    }
    bool prepareDependencies(Job& job) {
        job.usedDependencies.clear();
        const auto step = stepFor(job);
        if (step.dependencies.isEmpty()) return true;
        if (job.purpose != Purpose::Preflight || step.dependencies.size() > 8 ||
            job.draftRevision != draftRevision || job.inputBinding != binding(draft)) return false;
        QSet<QString> seen;
        const auto action = job.input.orderId.isEmpty() ? BrokerAction::Place : BrokerAction::Amend;
        for (const auto& dependency : step.dependencies) {
            const auto key = QString::number(int(dependency.operation)) + ':' + dependency.variant;
            if (dependency.variant.isEmpty() || seen.contains(key) || dependency.maxReceiptAgeMs <= 0) return false;
            seen.insert(key);
            const BrokerEvidence* selected = nullptr;
            for (auto it = evidence.crbegin(); it != evidence.crend(); ++it) {
                if (it->operation == dependency.operation && it->variant == dependency.variant && it->complete &&
                    it->sessionGeneration == generation && it->accountScope == scope() &&
                    it->draftRevision == job.draftRevision && it->inputBinding == job.inputBinding) { selected = &*it; break; }
            }
            BrokerFreshnessRule receipt; receipt.operation = dependency.operation; receipt.maxReceiptAgeMs = dependency.maxReceiptAgeMs;
            if (!selected || !evidenceFresh(*selected, receipt)) return false;
            for (const auto& rule : policy().freshness.value(action))
                if (rule.operation == dependency.operation && !evidenceFresh(*selected, rule)) return false;
            job.usedDependencies.append(*selected);
        }
        return true;
    }
    OrderInput cancelInput(const OrderRecord& order) const {
        OrderInput input; input.symbol = order.symbol; input.orderId = order.orderId; input.side = order.side;
        input.brokerConditional=order.brokerConditional;input.type = order.type; input.price = order.price; input.quantityMode = policy().cancelQuantityMode;
        input.quantity = (input.quantityMode == OrderQuantityMode::AllRemaining ||
            policy().cancelExplicitQtyBasis == ExplicitQtyBasis::Remaining) ? order.remaining : order.quantity;
        return input;
    }
    void startPreflight() {
        if (!draftPending || !adapter || !policy().exactIntentPreflight) return;
        if (active || !jobs.isEmpty()) { preflightTimer.start(300); return; }
        if (!session() || !connected || verifiedAccountScope != scope() || scope().isEmpty()) return;
        if (manualReconnect || clock.elapsed() < cooldownUntil) return;
        if (!bounded(draft) || !adapter->symbolValid(draft.symbol) || !decimal::positive(draft.quantity) ||
            (draft.type != "limit" && draft.type != "market") || (draft.side != "buy" && draft.side != "sell") ||
            (draft.type == "limit" && !decimal::positive(draft.price)) || (draft.type == "market" && !draft.price.isEmpty())) return;
        if (draft.orderId.isEmpty() && !policy().placeOrderTypes.contains(draft.type)) {
            draftPending = false;
            change(readiness(BrokerAction::Place, draft).message);
            return;
        }
        draftPending = false; pages = 0; cursors.clear(); market = draft.symbol;
        if (!enqueuePlan(BrokerPlanKind::OrderPreflight, draft, Purpose::Preflight)) {
            change("이 주문 조건의 사전 조회 계획이 준비되지 않았습니다."); return;
        }
        next();
    }
    BrokerReadiness requestReadiness() const {
        if (!session()) return {false, BrokerNotReady::NotAuthenticated, "인증을 먼저 완료하세요."};
        if (scope().isEmpty() || !connected) return {false, BrokerNotReady::AccountNotVerified, "계좌 선택·조회 연결을 먼저 완료하세요."};
        if (active || !jobs.isEmpty()) return {false, BrokerNotReady::Busy, "진행 중인 요청이 있습니다."};
        if (clock.elapsed() < cooldownUntil) return {false, BrokerNotReady::RateLimited, "서버 요청 제한 대기 중입니다. 자동 재전송하지 않습니다."};
        if (manualReconnect) return {false, BrokerNotReady::ReauthenticationRequired, "요청 제한 이후 명시적으로 다시 연결·인증해야 합니다."};
        if (clock.elapsed() < pacingUntil) return {false, BrokerNotReady::RequestSpacing, "다음 요청까지 잠시 대기 중입니다. 완료 후 직접 실행하세요."};
        if (kiwoom() && kiwoomBudget->remainingMs() > 0) return {false, BrokerNotReady::RequestSpacing, "공유 키움 요청 예산 대기 중입니다. 완료 후 직접 실행하세요."};
        return {true, BrokerNotReady::None, {}};
    }
    bool ready(QWidget* origin, bool mutation, bool newRisk, bool requireForeground = true) {
        const auto gate = requestReadiness();
        if (!gate.ready) { change(gate.message); return false; }
        if (requireForeground && !foreground(origin)) { change("활성 거래 창의 직접 버튼 입력만 허용합니다."); return false; }
        if (mutation && !vault.isUnlocked()) { change("주문 기록을 저장하려면 암호화 금고를 열어 주세요."); return false; }
        if (newRisk && (!enabled || halted || !pending().isEmpty())) { change("실제 주문 활성화 또는 미확정 주문 확인이 필요합니다."); return false; }
        return true;
    }
    void schedule(int delay = 0) {
        const auto epoch = generation;
        QTimer::singleShot(delay, q, [this, epoch] { if (epoch == generation) next(); });
    }
    bool enqueueReconciliation(const QJsonObject& entry, const QString& associatedId = {}) {
        if (!owns(entry)) return true;
        auto intent = journalInput(entry);
        const QString kind = entry.value("kind").toString();
        if (associatedId.isEmpty() && (kind == "cancel" || kind == "amend") && !intent.operationOrderId.isEmpty() &&
            !adapter->restoreReconciliationIntent(kind, intent)) {
            change("저장된 작업 주문과 원주문 관계가 충돌하여 대사를 중단했습니다. 미확정 기록을 보존했습니다.");
            return false;
        }
        if (!associatedId.isEmpty()) {
            if (kind == "place") intent.orderId = associatedId; else intent.operationOrderId = associatedId;
        }
        QStringList ids;
        if (!intent.operationOrderId.isEmpty()) ids.append(intent.operationOrderId);
        if (!intent.orderId.isEmpty() && !ids.contains(intent.orderId)) ids.append(intent.orderId);
        if (ids.isEmpty()) return true; // There is no client-ID lookup in the broker API.
        const auto id = entry.value("id").toString();
        reconciliations[id] = {int(ids.size()), false, associatedId, {}};
        for (const auto& queryId : ids) {
            auto query = intent; query.orderId = queryId;
            jobs.enqueue({intent.brokerConditional?Operation::AlgoQueryOrder:Operation::QueryOrder, query, Purpose::Reconciliation, id});
        }
        return true;
    }

    void next() {
        updateKiwoomPending();
        if (active || !adapter) return;
        if (jobs.isEmpty()) { change(); return; }
        if (manualReconnect || clock.elapsed() < cooldownUntil) { jobs.clear(); batch.clear(); refreshing = catalogLoading = false; change("요청 제한으로 남은 작업을 중단했습니다."); return; }
        if (clock.elapsed() < pacingUntil) { schedule(int(pacingUntil - clock.elapsed())); return; }
        current = jobs.dequeue();
        if (stepFor(current).authScope != BrokerAuthScope::Public && current.operation != Operation::Authenticate && !session()) { loseSession("토큰이 없거나 만료되었습니다. 명시적으로 다시 인증하세요."); return; }

        if (cancelOperation(current.operation) && pendingForOrder(current.input.symbol, current.input.orderId)) {
            jobs.clear(); batch.clear(); batchReviewed.clear();
            change("이 주문의 이전 요청이 미확정이므로 취소를 다시 전송하지 않았습니다. 먼저 주문 결과를 확인하세요."); return;
        }

        if (!prepareDependencies(current)) {
            jobs.clear(); completedBinding.clear(); completedDraftRevision = 0;
            change("이 주문 입력에 필요한 사전 조회 증거가 없거나 만료되었습니다. 다시 조회하세요."); return;
        }
        QString error; RequestSpec request;
        try {
            const bool built = true;
            request = adapter->buildPreparedStep(stepFor(current), current.usedDependencies, QDateTime::currentMSecsSinceEpoch(), &error);
            if (!built || !error.isEmpty() || request.url.isEmpty()) {
                wipe(request.body); for (auto& value : request.headers) wipe(value);
                if (optionalHistoryFailed(current)) return;
                jobs.clear(); batch.clear(); refreshing = catalogLoading = false; revoke();
                change(error.isEmpty() ? "요청 구성을 거부했습니다." : error); return;
            }
        } catch (const std::exception&) { loseSession("보호 메모리 또는 요청 구성에 실패하여 인증 세션을 폐기했습니다."); return; }
        if (kiwoom() && request.url.scheme() == "https" && request.url.host() == "api.kiwoom.com" && request.url.port(443) == 443) {
            const auto grant = kiwoomBudget->tryAcquire(q, KiwoomBudgetLane::ExistingBroker);
            if (!grant.lease) {
                wipe(request.body); for (auto& value : request.headers) wipe(value);
                current.usedDependencies.clear();
                if (grant.waitMs <= 0) {
                    jobs.clear(); batch.clear(); refreshing = catalogLoading = false; revoke();
                    change("공유 키움 요청 예산을 확인하지 못해 전송을 중단했습니다."); return;
                }
                jobs.prepend(current);
                schedule(int(qMin<qint64>(grant.waitMs, std::numeric_limits<int>::max())));
                change("공유 키움 요청 예산 대기 중 · 전송 전 조건을 다시 확인합니다."); return;
            }
            kiwoomLease = grant.lease;
        }

        active = true; ++token;

        transport->send(token, request);
        wipe(request.body); for (auto& value : request.headers) wipe(value);
        change(current.purpose == Purpose::Catalog ? QString{} : operationName(current.operation) + " 요청 중");
    }
    void completeReconciliation(const Job& job, const AdapterResult& result) {
        const auto entry = journal(job.journalId);
        if (entry.isEmpty() || !owns(entry) || !reconciliations.contains(job.journalId)) return;
        if(entry.value("brokerConditional")=="true"){
            if(result.ok&&result.orders.size()==1&&result.orders.first().brokerConditional&&result.orders.first().orderId==entry.value("orderId")&&result.orders.first().symbol==entry.value("symbol")&&result.orders.first().market=="domestic_equity"&&result.orders.first().status=="canceled"){
                if(!saveJournal(entry,true))return;activity[job.journalId]=result.orders.first();conditionalRows.erase(std::remove_if(conditionalRows.begin(),conditionalRows.end(),[&](const auto& row){return row.orderId==entry.value("orderId");}),conditionalRows.end());reconciliations.remove(job.journalId);change("조건 감시의 취소 상태 확인 · 발생한 일반 주문은 별도로 확인하세요.");
            }else{batch.clear();revoke();change("조건 감시 취소 결과 미확정 · 자동 재전송하지 않습니다. 공식 앱에서 확인하세요.");}return;
        }
        auto& state = reconciliations[job.journalId];
        if (!result.ok || result.orders.size() > 1) state.failed = true;
        for (const auto& order : result.orders) {
            if (order.orderId != job.input.orderId || order.symbol != job.input.symbol ||
                std::any_of(state.observed.begin(), state.observed.end(), [&](const auto& old) { return old.orderId == order.orderId; })) state.failed = true;
            else state.observed.append(order);
        }
        if (result.hasMore && !state.failed) {
            const auto cursorKey = job.journalId + ':' + job.input.orderId + ':' + result.nextCursor;
            if (++pages > 100 || result.nextCursor.isEmpty() || cursors.contains(cursorKey)) state.failed = true;
            else {
                cursors.insert(cursorKey); auto continuation = job; continuation.input.cursor = result.nextCursor;
                continuation.step.cursorHandle = result.nextCursor; jobs.prepend(continuation); return;
            }
        }
        if (!std::any_of(state.observed.begin(), state.observed.end(), [&](const auto& row) { return row.orderId == job.input.orderId; })) state.failed = true;
        if (--state.remaining > 0) return;
        const auto completed = state; reconciliations.remove(job.journalId);
        auto intent = journalInput(entry);
        const auto kind = entry.value("kind").toString();
        if (!completed.associatedId.isEmpty()) {
            if (kind == "place") intent.orderId = completed.associatedId;
            else intent.operationOrderId = completed.associatedId;
        }
        const auto outcome = completed.failed ? BrokerReconciliation{} : adapter->reconcile(kind, intent, completed.observed);
        if (!outcome.resolved) {
            auto unknown = entry; unknown["state"] = "unknown";
            if (!saveJournal(unknown)) return;
            batch.clear(); revoke();
            change("주문 결과가 미확정입니다. 재전송하지 않았습니다. 공식 앱의 주문 상태를 확인하세요.");
            return;
        }
        // Any explicit association is accepted only after the adapter compares
        // broker details with the encrypted intent and selected account scope.
        if (!saveJournal(entry, true)) return;
        verified(kind + "_reconciled");
        auto& activityRow = activity[job.journalId];
        if (!completed.observed.isEmpty()) activityRow = completed.observed.first();
        activityRow.journalId = job.journalId;
        if (kind != "place") {
            activityRow.orderId = intent.orderId;
            activityRow.replacementOrderId = intent.operationOrderId;
            activityRow.baseOriginalOrderId = intent.baseOriginalOrderId;
        }
        if (!outcome.applied) activityRow.status = "rejected";
        for (const auto& order : completed.observed) {
            if (order.status == "open" || order.status == "partial" || order.status == "pending_cancel" || order.status == "pending_replace") mergeOrders(open, {order});
            else { mergeOrders(closed, {order}); open.erase(std::remove_if(open.begin(), open.end(), [&](const auto& old) { return orderKey(old) == orderKey(order); }), open.end()); }
        }
        snapshot.openOrders = open; receivedAt.remove(Operation::BuyingPower); receivedAt.remove(Operation::SellableQuantity);
        evidence.erase(std::remove_if(evidence.begin(), evidence.end(), [&](const auto& item) {
            return item.operation == Operation::BuyingPower || item.operation == Operation::SellableQuantity ||
                (policy().exactIntentPreflight && item.draftRevision != 0);
        }), evidence.end());
        if (policy().exactIntentPreflight) {
            completedBinding.clear(); completedDraftRevision = 0; draftPending = true; preflightTimer.start(300);
        }
        change(outcome.message.isEmpty() ? "주문 상태 확인을 완료했습니다." : outcome.message);
        if (!batch.isEmpty()) {
            if (!outcome.applied) { batch.clear(); revoke(); }
            else {
                const auto removed = batch.first();
                batchReviewed.erase(std::remove_if(batchReviewed.begin(), batchReviewed.end(), [&](const auto& row) {
                    return orderKey(row) == orderKey(removed);
                }), batchReviewed.end());
                batch.removeFirst();
                if (!batch.isEmpty()) jobs.enqueue({Operation::OpenOrders, {}, Purpose::BatchScope, {}});
            }
        }
    }
    bool preserveCatalogSources(const QJsonObject& data) {
        if (!data.contains("catalogSources")) return true;
        if (!data.value("catalogSources").isArray() || data.value("catalogSources").toArray().size() > 32) return false;
        for (const auto& value : data.value("catalogSources").toArray()) {
            if (!value.isObject()) return false;
            const auto row = value.toObject(); QJsonObject clean;
            for (const auto* key : {"name", "url", "receivedAt", "status", "contentSha256", "httpDate", "lastModified"}) {
                if (!row.contains(key)) continue;
                if (!row.value(key).isString()) return false;
                const auto text = row.value(key).toString();
                if (text.size() > (QString::fromLatin1(key) == "url" ? 2048 : 160)) return false;
                for (const auto ch : text) if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format) return false;
                clean[key] = text;
            }
            const QUrl url(clean.value("url").toString());
            if (clean.value("name").toString().isEmpty() || clean.value("status").toString().isEmpty() ||
                !url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
            const auto digest = clean.value("contentSha256").toString();
            if (!digest.isEmpty() && !QRegularExpression("\\A[a-f0-9]{64}\\z").match(digest).hasMatch()) return false;
            const auto timestamp = clean.value("receivedAt").toString();
            if (!timestamp.isEmpty() && !QDateTime::fromString(timestamp, Qt::ISODateWithMs).isValid()) return false;
            bool duplicate = false;
            for (const auto& old : catalogSourceRecords) if (old.toObject() == clean) { duplicate = true; break; }
            if (!duplicate) { if (catalogSourceRecords.size() >= 32) return false; catalogSourceRecords.append(clean); }
        }
        return true;
    }
    bool updateCatalog(const QJsonObject& data, const BrokerStep& step, bool hasMore) {
        const auto requestedMarket = step.catalogMarket;
        if (!data.value("instruments").isArray() || data.value("market").toString() != requestedMarket) return false;
        const auto rows = data.value("instruments").toArray();
        const bool priorRows = std::any_of(catalogStaging.cbegin(), catalogStaging.cend(), [&](const auto& row) { return row.market == requestedMarket; });
        if ((rows.isEmpty() && !step.allowEmptyCatalog && !hasMore && !priorRows) || rows.size() + catalogStaging.size() > 10000) return false;
        QList<MarketInstrument> incoming; QSet<QString> seen;
        for (const auto& instrument : catalogStaging) seen.insert(instrument.symbol);
        for (const auto& value : rows) {
            if (!value.isObject()) return false;
            const auto row = value.toObject();
            MarketInstrument instrument;
            instrument.symbol = row.value("symbol").toString(); instrument.name = row.value("name").toString();
            instrument.englishName = row.value("englishName").toString();
            instrument.baseAsset = instrument.symbol; instrument.quoteAsset = "KRW";
            instrument.market = requestedMarket;
            instrument.securityType = row.value("securityType").toString();
            instrument.preferredClass = row.value("preferredClass").toString(); instrument.etpClass = row.value("etpClass").toString();
            instrument.warning = row.value("warning").toString();
            instrument.status = row.value("status").toString(); instrument.tradable = row.value("tradable").toBool();
            if (failedCatalogMarkets.contains(requestedMarket)) { instrument.tradable = false; instrument.warning = "공식 상품 분류 조회 실패 · 신규·정정 준비 미완"; }
            if (!adapter->catalogSymbolValid(instrument.symbol) || seen.contains(instrument.symbol)) return false;
            seen.insert(instrument.symbol); incoming.append(instrument);
        }
        catalogStaging.append(incoming); return true;
    }
    void finishAccountRead() {
        if (jobs.isEmpty() && refreshing) {
            refreshing = false; connected = accountReadVerified; verifiedAccountScope = connected ? scope() : QString{};
            if (connected) {
                verified("authenticated_basic_refresh_complete");
                if (historyRequested && !historyIncomplete && !assetsOnly) verified("authenticated_refresh_complete");
            }
            const auto completion = historyIncomplete ? QStringLiteral("기본 계좌 조회 완료 · 주문 상세 일부 미확인") : QStringLiteral("계좌 조회 완료");
            change(connected ? completion + " · " + policy().officialOrderTest.explanation + " · 실주문 미검증" : QStringLiteral("선택 계좌의 완전한 조회 증거를 확인하지 못했습니다."));
            if (connected && !assetsOnly && policy().exactIntentPreflight) { draftPending = true; preflightTimer.start(300); }
        }
    }
    bool optionalHistoryFailed(const Job& job) {
        if (job.purpose != Purpose::Refresh || job.operation != Operation::History || !job.step.optionalAccountRead) return false;
        historyIncomplete = true; checks.remove("authenticated_refresh_complete"); verified("optional_history_incomplete");
        evidence.erase(std::remove_if(evidence.begin(), evidence.end(), [&](const auto& item) {
            return item.operation == Operation::History && item.variant == job.step.variant;
        }), evidence.end());
        snapshot.data.remove(Operation::History); receivedAt.remove(Operation::History); receivedWallTime.remove(Operation::History);
        QString range = job.input.symbol;
        if (job.input.startTimeMs > 0) range += " " + QDateTime::fromMSecsSinceEpoch(job.input.startTimeMs, QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd");
        const auto note = range + " 주문 상세 일부 미확인 · 실패하거나 불완전한 조회 범위는 제외했습니다.";
        if (!coverage.contains(note)) coverage += (coverage.isEmpty() ? "" : "\n") + note;
        if (jobs.isEmpty()) finishAccountRead();
        else change("주문 상세 조회가 불완전합니다. 나머지 계좌 조회를 계속합니다.");
        schedule(); return true;
    }
    void onResult(quint64 replyToken, const HttpResult& response) {
        if (!active || replyToken != token || !adapter) return;
        const auto lease = std::exchange(kiwoomLease, 0);
        const auto budget = kiwoomBudget;
        qint64 sharedPause = 0;
        const auto release = qScopeGuard([budget, lease, &sharedPause] { if (lease) budget->finish(lease, sharedPause); });
        active = false;
        const auto completed = current;
        pacingUntil = clock.elapsed() + qBound(0, stepFor(completed).minimumDelayMs, 60000);
        if (completed.purpose == Purpose::Preflight && response.status != 401 &&
            (completed.draftRevision != draftRevision || completed.inputBinding != binding(draft))) {
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const auto& job) { return job.purpose == Purpose::Preflight; }), jobs.end());
            draftPending = true; preflightTimer.start(300); change("이전 입력의 사전 조회 응답을 폐기했습니다."); schedule(); return;
        }
        BrokerStepResult stepResult;
        AdapterResult result;
        try { if(response.localRetryAfterSeconds>0){result=localRateLimitFailure(response);stepResult.mutationRejected=true;}
            else {stepResult = adapter->parsePreparedStep(stepFor(completed), completed.usedDependencies, response); result = stepResult.value;} }
        catch (const std::exception&) { result.unknown = isMutation(completed.operation); result.message = "응답 처리에 실패했습니다."; }
        if (result.rateLimited || response.status == 429 || response.status == 418) {
            cooldownUntil = clock.elapsed() + qMax(1, result.retryAfterSeconds > 0 ? result.retryAfterSeconds : 60) * 1000LL;
            if (response.localRetryAfterSeconds <= 0)
                sharedPause = qMax(1, result.retryAfterSeconds > 0 ? result.retryAfterSeconds : 60) * 1000LL;
            if (response.status == 418 && result.retryAfterSeconds <= 0) manualReconnect = true;
            revoke();
        }
        if (result.authenticationFailed) { loseSession("인증이 만료되었거나 거부되었습니다. 명시적으로 다시 인증하세요."); return; }
        if (completed.purpose == Purpose::Preflight && (completed.draftRevision != draftRevision || completed.inputBinding != binding(draft))) {
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const auto& job) { return job.purpose == Purpose::Preflight; }), jobs.end());
            draftPending = true; preflightTimer.start(300); change("입력 변경 전 사전 조회 결과를 폐기했습니다."); schedule(); return;
        }
        if (completed.purpose == Purpose::Reconciliation) {
            completeReconciliation(completed, result); schedule(); return;
        }
        if(completed.operation==Operation::AlgoOpenOrders){
            if(!result.ok||++pages>100||conditionalStaging.size()+result.orders.size()>20000){conditionalComplete=false;conditionalStaging.clear();jobs.clear();change("조건 감시 조회 실패 · 이전 목록은 만료 상태로 유지합니다.");return;}
            QSet<QString> ids;for(const auto& row:conditionalStaging)ids.insert(row.orderId);for(const auto& row:result.orders){if(ids.contains(row.orderId)){conditionalComplete=false;conditionalStaging.clear();jobs.clear();change("조건 감시 페이지의 중복을 확인해 중단했습니다.");return;}ids.insert(row.orderId);conditionalStaging.append(row);}
            if(result.hasMore){if(result.nextCursor.isEmpty()||cursors.contains(result.nextCursor)){conditionalComplete=false;jobs.clear();return;}cursors.insert(result.nextCursor);auto next=completed;next.input.cursor=result.nextCursor;jobs.prepend(next);}
            else{conditionalRows=conditionalStaging;conditionalStaging.clear();conditionalAt=clock.elapsed();conditionalComplete=true;change("별도 조건 감시 조회 완료 · 발생한 일반 주문은 미체결에서 관리하세요.");}
            schedule();return;
        }
        if (completed.purpose == Purpose::Catalog) {
            const auto step = stepFor(completed);
            const bool sourceValid = preserveCatalogSources(result.data);
            const bool accepted = result.ok && sourceValid && updateCatalog(result.data, step, result.hasMore);
            if (!accepted && step.catalogAuxiliary && step.authScope == BrokerAuthScope::Public &&
                !result.authenticationFailed && !result.rateLimited && response.status != 429 && response.status != 418) {
                // Enrichment failure cannot certify a classification. The adapter
                // invalidates that source; its private catalog remains readable.
                failedCatalogMarkets.insert(step.catalogMarket);
                if (jobs.isEmpty()) {
                    catalog = catalogStaging; catalogStaging.clear(); catalogLoading = false;
                    catalogAt = catalog.isEmpty() ? -1 : clock.elapsed();
                    catalogWallTime = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                }
                change("보조 상품 분류를 확인하지 못했습니다. 증권사 종목 목록 조회를 계속합니다."); schedule(); return;
            }
            if (!accepted || ++pages > 100) {
                jobs.clear(); catalogStaging.clear(); catalogLoading = false; catalogAt = -1;
                catalogFailure = "종목 목록 조회 실패 · 이전 목록 만료";
            } else if (result.hasMore) {
                const auto key = step.catalogMarket + ':' + step.variant + ':' + result.nextCursor;
                if (result.nextCursor.isEmpty() || result.nextCursor.size() > 256 || cursors.contains(key)) {
                    jobs.clear(); catalogStaging.clear(); catalogLoading = false; catalogAt = -1;
                    catalogFailure = "종목 목록의 연속 조회 범위를 확인하지 못했습니다.";
                } else {
                    cursors.insert(key); auto continuation = completed;
                    continuation.step.cursorHandle = result.nextCursor;
                    jobs.prepend(continuation);
                }
            } else if (jobs.isEmpty()) {
                catalog = catalogStaging; catalogStaging.clear(); catalogLoading = false;
                catalogAt = clock.elapsed(); catalogWallTime = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                catalogFailure.clear();
            }
            change(); schedule(); return;
        }
        if (!result.ok) {
            if (!result.authenticationFailed && !result.rateLimited && response.status != 429 && response.status != 418 && optionalHistoryFailed(completed)) return;
            if (completed.purpose == Purpose::Preflight) {
                jobs.clear(); completedBinding.clear(); completedDraftRevision = 0; enabled = false;
                change("주문 조건 사전 조회 실패 — " + result.message); return;
            }
            jobs.clear(); batch.clear(); refreshing = false; connected = false; revoke();
            if (completed.purpose == Purpose::Authentication) adapter->clearSession();
            change(operationName(completed.operation) + " 실패 — " + result.message); return;
        }
        if (completed.operation == Operation::Authenticate) { hadSession = true; verified("authentication_succeeded");
            change(policy().accountMode == BrokerAccountMode::Configured ? QStringLiteral("인증 성공 · 등록 계좌를 선택하고 조회로 검증하세요.") : QStringLiteral("인증 성공 · 계좌 목록을 조회합니다.")); schedule(); return; }
        if (completed.operation == Operation::Accounts) { verified("account_choices_received"); change("인증된 계좌를 선택하세요. 주문은 비활성입니다."); schedule(); return; }
        if (completed.operation == Operation::OpenOrders) {
            // A broker's changeable-quantity lookup is evidence, not the whole
            // account order list. Never replace the reviewed scope with it.
            if (completed.purpose != Purpose::Preflight && completed.purpose != Purpose::CancelPreflight) {
                if (completed.input.cursor.isEmpty()) open = result.orders; else mergeOrders(open, result.orders);
                snapshot.openOrders = open;
            }
            auto data = result.data;
            if (!completed.input.cursor.isEmpty() && data.value("possibleOrders").isArray()) {
                auto rows = snapshot.data.value(completed.operation).value("possibleOrders").toArray();
                QSet<QString> seen; for (const auto& row : rows) seen.insert(row.toObject().value("orderId").toString());
                for (const auto& row : data.value("possibleOrders").toArray()) {
                    const auto id = row.toObject().value("orderId").toString();
                    if (id.isEmpty() || seen.contains(id) || rows.size() >= 20000) {
                        jobs.clear(); refreshing = false; connected = false; revoke(); change("변경 가능 주문의 연속 조회 범위를 확인하지 못했습니다."); return;
                    }
                    seen.insert(id); rows.append(row);
                }
                data["possibleOrders"] = rows;
            }
            snapshot.data[completed.operation] = data;
        }
        else if (completed.operation == Operation::History) {
            if (completed.step.optionalAccountRead) {
                QSet<QString> stagedKeys;
                for (const auto& row : completed.stagedHistoryOrders) stagedKeys.insert(orderKey(row));
                bool invalid = completed.stagedHistoryOrders.size() + result.orders.size() > 20000;
                for (const auto& row : result.orders) {
                    const auto key = orderKey(row);
                    if (row.orderId.isEmpty() || stagedKeys.contains(key)) invalid = true;
                    stagedKeys.insert(key);
                }
                if (invalid) { optionalHistoryFailed(completed); return; }
            }
            if (!completed.step.optionalAccountRead) mergeOrders(closed, result.orders);
            auto data = result.data;
            // Preserve every page of this one dated request as preflight evidence.
            // Different daily requests deliberately start a new evidence set.
            if (data.value("orders").isArray()) {
                auto rows = completed.input.cursor.isEmpty() ? QJsonArray{} : snapshot.data.value(Operation::History).value("orders").toArray();
                QSet<QString> seen; for (const auto& row : rows) seen.insert(row.toObject().value("orderId").toString());
                for (const auto& row : data.value("orders").toArray()) {
                    const auto id = row.toObject().value("orderId").toString();
                    if (id.isEmpty() || seen.contains(id) || rows.size() >= 20000) {
                        if (optionalHistoryFailed(completed)) return;
                        jobs.clear(); refreshing = false; connected = false; revoke(); change("주문 상세 연속 조회가 중복되거나 범위가 불완전합니다."); return;
                    }
                    seen.insert(id); rows.append(row);
                }
                data["orders"] = rows;
                if (data.contains("possibleOrders")) data["possibleOrders"] = rows;
            }
            snapshot.data[Operation::History] = data;
            if (completed.step.optionalAccountRead && !result.hasMore) {
                auto finished = completed.stagedHistoryOrders; mergeOrders(finished, result.orders); mergeOrders(closed, finished);
            }
        }
        else if (completed.operation == Operation::Holdings) {
            auto merged = result.data;
            auto rows = completed.input.cursor.isEmpty() ? QJsonArray{} : snapshot.data.value(Operation::Holdings).value("items").toArray();
            const auto rowKey = [](const QJsonValue& value) {
                const auto row = value.toObject();
                if (row.contains("positionKey")) {
                    const auto key = row.value("positionKey").toString();
                    return QRegularExpression(QStringLiteral("\\A[a-f0-9]{64}\\z")).match(key).hasMatch() ? "position:" + key : QString();
                }
                const auto symbol = row.value("symbol").toString();
                return symbol.isEmpty() ? QString() : "symbol:" + symbol;
            };
            QSet<QString> keys; for (const auto& row : rows) keys.insert(rowKey(row));
            for (const auto& row : result.data.value("items").toArray()) {
                const auto key = rowKey(row);
                if (key.isEmpty() || keys.contains(key)) { jobs.clear(); refreshing = false; connected = false; revoke(); change("보유 종목의 조회 행을 식별하지 못했거나 중복되어 중단했습니다."); return; }
                keys.insert(key); rows.append(row);
            }
            if (rows.size() > 20000) { jobs.clear(); refreshing = false; connected = false; revoke(); change("보유 종목 조회 상한을 초과했습니다."); return; }
            merged["items"] = rows; snapshot.data[completed.operation] = merged;
        } else { snapshot.data[completed.operation] = result.data; }
        if (stepResult.accountReadVerified && !result.hasMore && completed.purpose == Purpose::Refresh) accountReadVerified = true;
        BrokerEvidence item; item.operation = completed.operation; item.variant = completed.step.variant;
        item.sessionGeneration = generation; item.accountScope = scope();
        item.draftRevision = completed.draftRevision; item.inputBinding = completed.inputBinding;
        item.receivedMonotonicMs = completed.firstPageReceived >= 0 ? completed.firstPageReceived : clock.elapsed();
        item.receivedUtc = adapter->utcNow();
        item.sourceTime = stepResult.sourceTime; item.complete = !result.hasMore;
        item.data = completed.operation == Operation::Holdings || completed.operation == Operation::OpenOrders || completed.operation == Operation::History ?
            snapshot.data.value(completed.operation) : result.data;
        evidence.erase(std::remove_if(evidence.begin(), evidence.end(), [&](const auto& old) {
            return old.operation == item.operation && old.variant == item.variant;
        }), evidence.end()); evidence.append(item);
        receivedAt[completed.operation] = item.receivedMonotonicMs;
        receivedWallTime[completed.operation] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        if (completed.operation == Operation::Quote) snapshot.quoteReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
        if (!result.scope.isEmpty() && !coverage.contains(result.scope)) coverage += (coverage.isEmpty() ? "" : "\n") + result.scope;
        if (result.hasMore) {
            const auto cursorKey = QString::number(int(completed.operation)) + ':' + completed.step.variant + ':' + result.nextCursor;
            if (++pages > 100 || result.nextCursor.isEmpty() || cursors.contains(cursorKey) || open.size() + closed.size() > 20000) {
                if (optionalHistoryFailed(completed)) return;
                jobs.clear(); refreshing = false; connected = false; revoke(); change("조회 범위가 불완전하여 주문을 활성화하지 않았습니다."); return;
            }
            cursors.insert(cursorKey);
            auto continuation = completed; continuation.input.cursor = result.nextCursor; continuation.step.cursorHandle = result.nextCursor;
            if (completed.step.optionalAccountRead && completed.operation == Operation::History)
                mergeOrders(continuation.stagedHistoryOrders, result.orders);
            if (continuation.firstPageReceived < 0) continuation.firstPageReceived = clock.elapsed();
            jobs.prepend(continuation);
        }

        finishAccountRead();
        if(jobs.isEmpty()&&historyOnly){historyOnly=false;change("과거 주문 이력 조회 완료 · 계좌 잔고와 별도 기준 시각");}
        if (jobs.isEmpty() && completed.purpose == Purpose::CancelPreflight) change("취소 조건 조회 완료 · 대상 확인 후 직접 취소하세요.");
        if (jobs.isEmpty() && completed.purpose == Purpose::Preflight) {
            completedDraftRevision = completed.draftRevision; completedBinding = completed.inputBinding;
            change("입력한 주문 조건의 사전 조회 완료 · 주문은 별도 직접 클릭이 필요합니다.");
        }
        schedule();
    }
    void refresh(const QString& symbol, bool background = false,bool includeHistory=false) {
        if (!session() || scope().isEmpty() || active || !jobs.isEmpty()) { if (!background) change("인증과 계좌 선택을 완료한 뒤 조회하세요."); return; }
        if (manualReconnect || clock.elapsed() < cooldownUntil) { if (!background) change("요청 제한 대기 중입니다."); return; }
        if (!adapter->symbolValid(symbol)) { if (!background) change("이 증권사에서 지원하는 국내 종목코드를 확인하세요."); return; }
        assetsOnly = historyOnly = false;historyRequested=includeHistory;
        if (market != symbol) { revoke(); snapshot = {}; receivedAt.clear(); receivedWallTime.clear(); }
        market = symbol; cursors.clear(); refreshing = true; connected = false; verifiedAccountScope.clear(); pages = 0; open.clear();
        checks.remove("authenticated_refresh_complete"); checks.remove("authenticated_basic_refresh_complete");
        if (!background) { closed.clear(); coverage.clear(); historyIncomplete = false; }
        OrderInput input; input.symbol = symbol;
        accountReadVerified = false;
        if (!enqueuePlan(BrokerPlanKind::AccountRead, input, Purpose::Refresh, background||!includeHistory)) {
            refreshing = false; change("계좌 조회 계획이 준비되지 않았습니다."); return;
        }
        next();
    }
};

BrokerTradingController::BrokerTradingController(Vault& vault, TradingTransport* transport, QObject* parent, AdapterFactory adapterFactory,
                                               std::shared_ptr<KiwoomRequestBudget> kiwoomBudget)
    : QObject(parent), d_(std::make_unique<Private>(this, vault, transport ? transport : new NetworkTradingTransport(this), std::move(adapterFactory), std::move(kiwoomBudget))) {
    connect(d_->transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) { d_->onResult(token, result); });
    connect(&vault, &Vault::aboutToLock, this, &BrokerTradingController::stop);
    connect(&vault, &Vault::accountsChanged, this, [this] { if (!d_->connection.id.isEmpty()) stop(); });
    d_->preflightTimer.setSingleShot(true);
    connect(&d_->preflightTimer, &QTimer::timeout, this, [this] { d_->startPreflight(); });
    d_->readinessTimer.setSingleShot(true);
    d_->readinessTimer.setTimerType(Qt::PreciseTimer);
    connect(&d_->readinessTimer, &QTimer::timeout, this, [this] {
        if (d_->readinessGeneration != d_->generation) return;
        // Expiry only updates the UI. A waiting/manual action is never replayed.
        // change() re-arms if another deadline remains or a long timer was capped.
        d_->change();
    });
    d_->automatic.setInterval(10000);
    connect(&d_->automatic, &QTimer::timeout, this, [this] {
        if (d_->hadSession && !d_->session()) { d_->loseSession("토큰이 만료되었습니다. 명시적으로 다시 인증하세요."); return; }
        if (d_->connected && !busy() && d_->pending().isEmpty()) {
            if (d_->policy().exactIntentPreflight && !d_->draft.symbol.isEmpty()) { d_->draftPending = true; d_->preflightTimer.start(0); }
            else if (!d_->policy().exactIntentPreflight && !d_->market.isEmpty()) d_->refresh(d_->market, true);
        }
        emit changed();
    });
    d_->automatic.start();
}
BrokerTradingController::~BrokerTradingController() { stop(); }
void BrokerTradingController::stop() {
    d_->clearWork(); d_->revoke(); d_->clearReadState();
    if (d_->adapter) d_->adapter->clearSession();
    d_->adapter.reset(); d_->connection = {}; d_->fingerprint.clear(); d_->checks = {};
    d_->catalog.clear(); d_->catalogAt = -1; d_->catalogFailure.clear(); d_->catalogWallTime.clear();
    d_->catalogSourceRecords = {}; d_->failedCatalogMarkets.clear();
    d_->hadSession = false; d_->limits = {}; d_->halted = false;
    // Broker throttling may be tied to a client or IP. Switching a local connection
    // must not bypass an outstanding cooldown; explicit authentication clears only
    // the manual-reconnect latch after that cooldown has elapsed.
    d_->change("증권 연결을 중지했습니다. 접수된 주문은 공식 앱에서 확인하세요.");
}
bool BrokerTradingController::selectConnection(const QString& id) {
    if (id == d_->connection.id && supported()) return true;
    if (busy()) { d_->change("진행 중인 요청이 끝난 뒤 연결을 바꾸세요."); return false; }
    stop();
    for (const auto& connection : d_->vault.accounts()) if (connection.id == id) { d_->connection = connection; break; }
    if (AccountModel::isProviderSuspended(d_->connection.provider)) {
        d_->connection = {};
        d_->change("이 증권사의 연결은 비활성화되어 있습니다.");
        return false;
    }
    if (d_->adapterFactory) d_->adapter = d_->adapterFactory(d_->connection);
    else if (d_->connection.market == "domestic_equity" && d_->connection.provider == "toss" && d_->connection.environment == "live")
        d_->adapter = std::make_unique<TossAdapter>();
    else if (d_->connection.market == "domestic_equity" && d_->connection.provider == "kiwoom" &&
             d_->connection.environment == "live")
        d_->adapter = std::make_unique<KiwoomAdapter>(d_->connection.environment);
    if (d_->adapter && (d_->adapter->policy().provider != d_->connection.provider || d_->adapter->policy().environment != d_->connection.environment ||
        d_->adapter->policy().market != d_->connection.market)) d_->adapter.reset();
    if (!d_->adapter) { d_->connection = {}; d_->change("지원하는 증권사·환경·시장을 선택하세요."); return false; }
    QString error;
    if (!d_->vault.withCredentials(id, [&](const CredentialSet& credentials) {
        d_->fingerprint = d_->adapter->credentialIdentityFingerprint(credentials);
    }, &error) || d_->fingerprint.isEmpty()) { stop(); d_->change("보호된 인증정보의 연결 식별자를 사용할 수 없습니다."); return false; }
    d_->change(d_->policy().displayName + " 연결을 선택했습니다. ‘연결·인증’을 직접 눌러 연결하세요."); return true;
}
AccountRecord BrokerTradingController::account() const { return d_->connection; }
QString BrokerTradingController::credentialSessionIdentity() const { return d_->fingerprint; }
bool BrokerTradingController::supported() const { return bool(d_->adapter); }
bool BrokerTradingController::authenticated() const { return d_->session(); }
bool BrokerTradingController::withMarketAuthorization(const BrokerAdapter::MarketAuthorizationBorrow& borrow) const {
    if (!d_->vault.isUnlocked() || !d_->adapter || (d_->connection.provider != "toss" && d_->connection.provider != "kiwoom") ||
        !accountCredentialsAvailable(d_->vault, d_->connection.id)) return false;
    return d_->adapter->withMarketAuthorization(borrow);
}
QString BrokerTradingController::marketDataCountry(const QString& symbol) const {
    if (!d_->vault.isUnlocked() || !d_->adapter || !d_->session()) return {};
    return d_->adapter->marketDataCountry(symbol);
}
QStringList BrokerTradingController::marketDataHoldings() const {
    if (!d_->vault.isUnlocked() || !d_->adapter || !d_->session()) return {};
    return d_->adapter->marketDataHoldings();
}
bool BrokerTradingController::connected() const { return d_->connected && d_->session() && !d_->scope().isEmpty(); }
bool BrokerTradingController::busy() const { return d_->active || !d_->jobs.isEmpty(); }
BrokerReadiness BrokerTradingController::requestReadiness() const { return d_->requestReadiness(); }
bool BrokerTradingController::liveEnabled() const { return d_->enabled && d_->session(); }
bool BrokerTradingController::halted() const { return d_->halted; }
bool BrokerTradingController::quoteFresh() const { return d_->sourceFresh(d_->policy().quoteOperation) && d_->sourceFresh(Operation::Depth); }
QString BrokerTradingController::status() const { return d_->message; }
QString BrokerTradingController::coverage() const {
    QString result = d_->coverage;
    for (const auto& pair : {qMakePair(Operation::Holdings, QStringLiteral("보유 종목 · ") + d_->policy().holdingsSource),
                           qMakePair(Operation::BuyingPower, QStringLiteral("주문 가능 금액 · ") + d_->policy().buyingPowerSource),
                           qMakePair(Operation::Account, QStringLiteral("예수금 · ") + d_->policy().accountSource)}) {
        if (d_->receivedWallTime.contains(pair.first)) result += (result.isEmpty() ? "" : "\n") + pair.second + " · 수신 " + displayTime(d_->receivedWallTime.value(pair.first));
    }
    return result;
}
QString BrokerTradingController::permissionSummary() const { return connected() ? "인증 계좌 조회 완료" : "인증·계좌 조회 미완료"; }
QString BrokerTradingController::symbol() const { return d_->market; }
QString BrokerTradingController::selectedAccountLabel() const { return d_->adapter ? d_->adapter->selectedAccountLabel() : QString{}; }
QList<BrokerAccountChoice> BrokerTradingController::accountChoices() const { return d_->adapter && authenticated() ? d_->adapter->accountChoices() : QList<BrokerAccountChoice>{}; }
bool BrokerTradingController::selectBrokerAccount(const QString& handle) {
    if (!authenticated() || busy() || handle.size() > 128) return false;
    d_->clearWork(); d_->revoke(); d_->clearReadState(); d_->checks = {}; QString error;
    if (!d_->adapter->selectBrokerAccount(handle, &error)) { d_->change(error); return false; }
    d_->change("계좌를 선택했습니다. 조회를 완료하고 주문 한도를 설정하세요."); return true;
}
bool BrokerTradingController::canAuthenticate() const {
    return d_->adapter && !busy() && accountCredentialsAvailable(d_->vault, d_->connection.id) &&
        d_->clock.elapsed() >= d_->cooldownUntil;
}
void BrokerTradingController::authenticate() {
    if (!d_->adapter || busy() || !accountCredentialsAvailable(d_->vault, d_->connection.id)) return;
    if (d_->clock.elapsed() < d_->cooldownUntil) { d_->change("인증 요청 제한 대기 중입니다."); return; }
    d_->clearWork(); d_->revoke(); d_->clearReadState(); d_->adapter->clearSession();
    d_->catalog.clear(); d_->catalogAt = -1; d_->catalogFailure.clear(); d_->catalogWallTime.clear();
    d_->catalogSourceRecords = {}; d_->failedCatalogMarkets.clear();
    d_->hadSession = false; d_->checks = {}; d_->manualReconnect = false;
    QString error; bool configured = false;
    try {
        if (!d_->vault.withCredentials(d_->connection.id, [&](const CredentialSet& credentials) {
            configured = d_->adapter->configureSession(d_->connection, credentials, &error);
        }, &error) || !configured) { d_->loseSession(error.isEmpty() ? QStringLiteral("보호된 연결 설정을 준비하지 못했습니다.") : error); return; }
    } catch (const std::exception&) { d_->loseSession("보호 메모리에 인증 설정을 준비하지 못했습니다."); return; }
    if (!d_->enqueuePlan(BrokerPlanKind::Authenticate, {}, Private::Purpose::Authentication)) {
        d_->loseSession("인증 계획이 준비되지 않았습니다."); return;
    }
    d_->next();
}
void BrokerTradingController::refresh(const QString& symbol,bool includeHistory) { if(busy()&&!interruptRead())return;d_->refresh(symbol,false,includeHistory); }
void BrokerTradingController::loadHistory(){
    if(!d_->session()||d_->scope().isEmpty()||d_->market.isEmpty())return;
    if(busy()&&!interruptRead())return;
    if(d_->manualReconnect||d_->clock.elapsed()<d_->cooldownUntil)return;
    d_->historyOnly=true;d_->historyRequested=true;d_->historyIncomplete=false;d_->pages=0;d_->cursors.clear();d_->closed.clear();
    OrderInput in;in.symbol=d_->market;
    for(auto step:d_->adapter->plan(BrokerPlanKind::AccountRead,in))if(step.operation==Operation::History){
        step.optionalAccountRead=true;d_->jobs.enqueue({step.operation,step.input,Private::Purpose::Refresh,{},step});
    }
    if(d_->jobs.isEmpty()){d_->historyOnly=false;d_->change("이 연결은 선택 종목의 별도 이력 조회를 제공하지 않습니다.");return;}d_->next();
}
bool BrokerTradingController::refreshAssets() {
    if(!d_->session()||d_->scope().isEmpty()||busy()||d_->manualReconnect||d_->clock.elapsed()<qMax(d_->cooldownUntil,d_->pacingUntil))return false;
    d_->assetsOnly=true;d_->historyOnly=d_->historyRequested=false;d_->refreshing=true;d_->accountReadVerified=false;d_->pages=0;d_->cursors.clear();
    if(!d_->enqueuePlan(BrokerPlanKind::AccountRead,{},Private::Purpose::Refresh)) {d_->refreshing=false;return false;}
    d_->jobs.erase(std::remove_if(d_->jobs.begin(),d_->jobs.end(),[](const auto& j){return j.operation!=Operation::Account&&j.operation!=Operation::Holdings;}),d_->jobs.end());
    if(d_->jobs.isEmpty()){d_->refreshing=false;return false;}
    d_->next();return true;
}
void BrokerTradingController::setAutomaticRefresh(bool enabled) { if(enabled)d_->automatic.start();else d_->automatic.stop(); }
bool BrokerTradingController::canInterruptRead()const{
    return !((d_->active&&(isMutation(d_->current.operation)||!d_->current.journalId.isEmpty()||d_->current.purpose==Private::Purpose::Authentication))||
        std::any_of(d_->jobs.cbegin(),d_->jobs.cend(),[](const auto& j){return isMutation(j.operation)||!j.journalId.isEmpty()||j.purpose==Private::Purpose::Authentication;}));
}
bool BrokerTradingController::historyLoading()const{return d_->historyOnly&&busy();}
bool BrokerTradingController::interruptRead() {
    if(!canInterruptRead())return false;
    d_->clearWork();d_->enabled=false;d_->refreshing=false;d_->preflightTimer.stop();d_->draftPending=false;
    d_->change("조회 중단 · 인증 세션과 미확정 주문 기록은 유지합니다.");return true;
}
BrokerPolicy BrokerTradingController::policy() const { return d_->policy(); }
void BrokerTradingController::setDraft(const OrderInput& input) {
    if (d_->binding(d_->draft) == d_->binding(input)) return;
    d_->draft = input; ++d_->draftRevision; d_->completedDraftRevision = 0; d_->completedBinding.clear();
    if (!d_->adapter || !d_->policy().exactIntentPreflight) return;
    d_->draftPending = true;
    // Only a read-only preflight may be aborted. An already sent mutation and
    // its receipt/reconciliation remain attached to the session generation.
    if (d_->active && d_->current.purpose == Private::Purpose::Preflight) {
        d_->pacingUntil = qMax(d_->pacingUntil, d_->clock.elapsed() + qBound(0, d_->stepFor(d_->current).minimumDelayMs, 60000));
        ++d_->token; d_->abortRequest();
    }
    d_->jobs.erase(std::remove_if(d_->jobs.begin(), d_->jobs.end(), [](const auto& job) { return job.purpose == Private::Purpose::Preflight; }), d_->jobs.end());
    d_->preflightTimer.start(300); d_->change("주문 입력 변경 · 사전 조회 완료 후 직접 주문하세요.");
}
BrokerReadiness BrokerTradingController::draftReadiness() const {
    const auto gate = requestReadiness(); if (!gate.ready) return gate;
    if (!bounded(d_->draft)) return {false, BrokerNotReady::RejectedBySafetyRule, "주문 입력이 너무 깁니다."};
    return d_->readiness(d_->draft.orderId.isEmpty() ? BrokerAction::Place : BrokerAction::Amend, d_->draft);
}
void BrokerTradingController::prepareCancellation() {
    if(historyLoading()&&!interruptRead())return;
    if (!d_->ready(nullptr, false, false, false)) return;
    const auto plan = d_->adapter->plan(BrokerPlanKind::CancelPreflight, {});
    if (plan.isEmpty()) {
        d_->change(d_->policy().exactIntentPreflight ? QStringLiteral("이 환경에서 취소에 필요한 선조회를 제공하지 않습니다.") : QStringLiteral("선택 주문을 확인한 뒤 취소할 수 있습니다.")); return;
    }
    d_->pages = 0; d_->cursors.clear();
    if (d_->enqueuePlan(BrokerPlanKind::CancelPreflight, {}, Private::Purpose::CancelPreflight)) d_->next();
}
BrokerReadiness BrokerTradingController::cancellationReadiness(const OrderRecord& order) const {
    const auto gate = requestReadiness(); if (!gate.ready) return gate;
    if (!order.cancelable || cancelBlocked(order)) return {false, BrokerNotReady::UnresolvedOrder, "취소할 수 없거나 이전 요청이 미확정입니다."};
    const auto input = d_->cancelInput(order);
    return d_->readiness(BrokerAction::Cancel, input);
}
void BrokerTradingController::loadMarkets() {
    if(busy()&&!interruptRead())return;
    if (!d_->adapter || busy() || (!d_->policy().publicCatalog && !authenticated())) { d_->change("이 종목 목록 조회에 필요한 연결 상태를 확인하세요."); return; }
    if (d_->manualReconnect || d_->clock.elapsed() < d_->cooldownUntil) { d_->catalogFailure = "요청 제한 대기 중입니다."; d_->change(); return; }
    d_->catalogLoading = true; d_->catalogAt = -1; d_->catalogStaging.clear(); d_->catalogFailure.clear();
    d_->catalogSourceRecords = {}; d_->failedCatalogMarkets.clear(); d_->pages = 0; d_->cursors.clear();
    if (!d_->enqueuePlan(BrokerPlanKind::Catalog, {}, Private::Purpose::Catalog)) {
        d_->catalogLoading = false; d_->catalogFailure = "종목 목록 조회가 구현되지 않았습니다."; d_->change(); return;
    }
    d_->next();
}
QList<MarketInstrument> BrokerTradingController::instruments() const { return d_->catalog; }
bool BrokerTradingController::catalogFresh() const { return d_->adapter && (authenticated() || d_->policy().publicCatalog) && !d_->catalog.isEmpty() && d_->catalogFailure.isEmpty() && d_->catalogAt >= 0 && d_->clock.elapsed() - d_->catalogAt < 300000; }
QString BrokerTradingController::catalogStatus() const {
    QString result = d_->policy().catalogSource + " · " + (d_->catalogLoading ? "조회 중" : catalogFresh() ? "유효 (5분)" : "미조회 또는 만료") + (d_->catalogWallTime.isEmpty() ? "" : " · 수신 " + displayTime(d_->catalogWallTime)) + (d_->catalogFailure.isEmpty() ? "" : " · " + d_->catalogFailure);
    for (const auto& value : d_->catalogSourceRecords) {
        const auto row = value.toObject();
        result += "\n" + row.value("name").toString() + " · " + row.value("status").toString() +
            " · 원천 수신 " + displayTime(row.value("receivedAt").toString()) + " · " + row.value("url").toString();
    }
    return result;
}
QJsonObject BrokerTradingController::holdings() const { return d_->snapshot.data.value(Operation::Holdings); }
QJsonObject BrokerTradingController::accountSummary() const { return d_->snapshot.data.value(Operation::Account); }
QJsonArray BrokerTradingController::catalogSources() const { return d_->catalogSourceRecords; }
QJsonObject BrokerTradingController::buyingPower() const { return d_->snapshot.data.value(Operation::BuyingPower); }
QString BrokerTradingController::quoteSummary() const {
    const auto quote = d_->snapshot.data.value(d_->policy().quoteOperation); QString source = "미제공";
    const auto* item = d_->latestEvidence(Operation::Depth);
    if (item && item->sourceTime.quality == SourceTimeQuality::FullTimestamp) source = displayTime(item->sourceTime.fullTimestamp.toString(Qt::ISODateWithMs));
    else if (item && item->sourceTime.quality == SourceTimeQuality::TimeOfDayOnly) source = item->sourceTime.rawText + " (날짜 미제공)";
    return d_->market + " · " + quote.value("lastPrice").toString() + " KRW · " +
        (quoteFresh() ? "수신 기한 내 호가" : "호가 미확인 또는 지연") +
        " · 공식 시세 시각 (호가) " + source + " · 수신 " + displayTime(d_->receivedWallTime.value(Operation::Depth));
}
QList<OrderRecord> BrokerTradingController::openOrders() const { return d_->open; }
QList<OrderRecord> BrokerTradingController::history() const { return d_->closed; }
QList<OrderRecord> BrokerTradingController::unresolvedOrders() const {
    QList<OrderRecord> result;
    for (const auto& value : d_->pending()) {
        const auto record = value.toObject(); const auto input = journalInput(record);
        OrderRecord order; order.journalId = record.value("id").toString(); order.orderId = input.orderId;
        order.replacementOrderId = input.operationOrderId; order.clientId = input.clientId; order.symbol = input.symbol;
        order.baseOriginalOrderId = input.baseOriginalOrderId;
        order.side = input.side; order.type = record.value("kind").toString(); order.quantity = input.quantity;
        order.price = input.price; order.status = "unknown"; order.market = "domestic_equity"; order.currency = "KRW"; result.append(order);
    }
    return result;
}
QList<OrderRecord> BrokerTradingController::actionResults() const { return d_->activity.values(); }
bool BrokerTradingController::cancelBlocked(const OrderRecord& order) const { return d_->pendingForOrder(order.symbol, order.orderId); }
OrderLimits BrokerTradingController::limits() const { return d_->limits; }
bool BrokerTradingController::setLimits(const OrderLimits& limits) {
    if (busy() || limits.maxNotional.size() > 30 || limits.maxDeviationPercent.size() > 30 || limits.currency != "KRW" ||
        !decimal::positive(limits.maxNotional) || !decimal::positive(limits.maxDeviationPercent) || decimal::compare(limits.maxDeviationPercent, "100") > 0) {
        d_->change("KRW 주문 한도와 0 초과 100 이하의 괴리율을 설정하세요."); return false;
    }
    d_->limits = limits; d_->revoke(); d_->change("주문 한도를 적용했습니다. 실제 주문은 별도로 활성화하세요."); return true;
}

OrderValidation BrokerTradingController::preview(const OrderInput& input) const {
    if (!connected() || input.symbol != d_->market || !bounded(input)) return {false, "조회한 국내 종목과 입력을 확인하세요.", {}, {}};
    if (!quoteFresh()) return {false, "주문에 사용할 최신 호가 시각을 확인하지 못했습니다.", {}, {}};
    const auto readiness = d_->readiness(input.orderId.isEmpty() ? BrokerAction::Place : BrokerAction::Amend, input);
    if (!readiness.ready) return {false, readiness.message, {}, {}};
    return d_->policy().exactIntentPreflight ? readiness.validation : d_->adapter->validate(input, d_->snapshot, d_->limits);
}

void BrokerTradingController::reconcile() {
    if (!d_->ready(nullptr, true, false, false)) return;
    d_->pages = 0; d_->cursors.clear();
    for (const auto& value : d_->pending()) if (!d_->enqueueReconciliation(value.toObject())) {
        d_->jobs.clear(); d_->reconciliations.clear(); return;
    }
    if (d_->jobs.isEmpty()) { d_->change("검증된 선택 계좌에서 조회할 서버 주문 ID가 없습니다. 공식 앱에서 확인한 ID를 연결하세요."); return; }
    d_->next();
}
QJsonObject BrokerTradingController::verificationReceipt() const {
    return {{"schema_version", 1}, {"application", "KEEFI"}, {"stage", "G3-Broker"},
        {"version", QCoreApplication::applicationVersion()}, {"provider", d_->connection.provider}, {"environment", d_->connection.environment},
        {"transport", dynamic_cast<NetworkTradingTransport*>(d_->transport) ? "official_https" : "synthetic_test"},
        {"exported_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"checks", d_->checks},
        {"official_order_test_supported", false}, {"pending_operation_count", d_->pending().size()},
        {"account_history_requested",d_->historyRequested},{"account_history_incomplete", d_->historyIncomplete},
        {"contains_keys_account_ids_order_ids_or_amounts", false},
        {"scope_note", "현재 실행에서 관측한 단계입니다. 인증·조회 성공은 주문 권한 또는 실제 체결 성공의 증거가 아닙니다."}};
}
} // namespace keefi

namespace keefi {
AssetCapture BrokerTradingController::assetCapture() const {
    if(!connected()||busy()||!accountCredentialsAvailable(d_->vault,d_->connection.id))return {};
    const auto evidence=[this](Operation op,const QString& source){AssetEvidence e;e.source=source;e.scope=d_->scope();
        const auto* raw=d_->latestEvidence(op);if(!raw)return e;e.complete=raw->complete;e.receivedAtMs=raw->receivedUtc.toMSecsSinceEpoch();
        if(raw->sourceTime.quality==SourceTimeQuality::FullTimestamp)e.sourceTimeMs=raw->sourceTime.fullTimestamp.toMSecsSinceEpoch();return e;};
    const auto p=policy();return captureBrokerAssets(d_->connection,d_->scope(),holdings(),accountSummary(),buyingPower(),
        evidence(Operation::Holdings,p.holdingsSource),evidence(Operation::Account,p.accountSource),unresolvedOrders().isEmpty(),coverage(),
        history(),evidence(Operation::History,p.displayName+" 공식 주문 이력 관측"));
}
}
