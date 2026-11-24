#include "keefi/BuildPolicy.h"
#include "keefi/TradingController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/TradingTransport.h"
#include "keefi/UpbitAdapter.h"
#include "keefi/BinanceAdapter.h"
#include "keefi/BinanceTiming.h"
#include "keefi/OkxAdapter.h"
#include "keefi/BithumbAdapter.h"
#include "keefi/OkxOrderEvents.h"
#include "keefi/OkxProtocol.h"
#include "keefi/Vault.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSet>
#include <QQueue>
#include <QRegularExpression>
#include <QTimer>
#include <QTimeZone>
#include <QWidget>
#include <sodium.h>

namespace keefi {
namespace {
bool terminal(const OrderRecord& order) {
    return QStringList{"filled", "canceled", "cancelled", "rejected", "expired", "done", "cancel"}.contains(order.status);
}
void mergeOrders(QList<OrderRecord>& into, const QList<OrderRecord>& incoming) {
    for (const auto& order : incoming) {
        auto it = std::find_if(into.begin(), into.end(), [&](const OrderRecord& current) {
            return current.symbol == order.symbol && current.orderId == order.orderId;
        });
        if (it == into.end()) into.append(order); else *it = order;
    }
}
bool foreground(QWidget* origin) {
    return origin && origin->isVisible() && origin->isEnabled() && origin->hasFocus() &&
        origin->window() == QApplication::activeWindow() && QApplication::applicationState() == Qt::ApplicationActive;
}
void wipe(QByteArray& bytes) { bytes.detach(); if (!bytes.isEmpty()) sodium_memzero(bytes.data(), bytes.size()); bytes.clear(); }
}
struct TradingController::Private {
    TradingController* q;
    Vault& vault;
    TradingTransport* transport;
    std::unique_ptr<ExchangeAdapter> adapter;
    AccountRecord account;
    QString fingerprint, market, message = "API 연결을 선택해 주세요.", permission = "주문 권한 미검증", coverage;
    MarketSnapshot snapshot;
    QMap<Operation, qint64> observedAt, receivedWall;
    QJsonObject checks;
    QList<OrderRecord> open, closed;
    QList<MarketInstrument> catalog;
    QString catalogFailure;
    qint64 catalogAt = -1, catalogReceivedAtMs = 0;
    bool catalogLoading = false;
    OrderLimits limits;
    QElapsedTimer clock;
    qint64 quoteAt = -1, accountAt = -1, rulesAt = -1, serverAt = -1, serverAnchor = 0, cooldownUntil = 0, pacingUntil = 0;
    bool manualReconnectRequired = false;
    bool active = false, connected = false, enabled = false, halted = false, tested = false, refreshFailed = false;
    bool assetsOnly = false,historyOnly=false;
    bool orderTimeReady = false;
    int assetsTimeRetries = 0;
    quint64 token = 0, generation = 0;
    qint64 sentAt = 0;
    std::function<qint64()> timingClock;
    qint64 timingNow() const { return timingClock ? timingClock() : clock.elapsed(); }
    int pageCount = 0;
    struct Job { Operation operation; OrderInput input; QString journalId; bool replacement = false; bool catalogOnly = false; };
    QQueue<Job> jobs;
    Job current{Operation::Time, {}, {}, false};
    QMap<QString, QMap<bool, OrderRecord>> reconciled;
    QMap<QString, OrderRecord> activity;
    QTimer automatic, readinessTimer;
    OkxOrderEvents* events = nullptr;
    bool permissionKnown = false, tradeAllowed = false, openComplete = false;
    bool preparingDraft = false, preparingCancellation = false;
    QString accountScope, readinessKey;
    OrderInput draft, preparedDraft;
    OrderRecord preparedTarget;
    quint64 draftRevision = 0, preparingRevision = 0, preparedRevision = 0, eventGeneration = 0;
    qint64 openAt = -1;
    QMap<Operation, QList<OrderRecord>> pageRows;
    QMap<Operation, QSet<QString>> pageCursors, pageIdentities;
    QMap<Operation, qint64> firstPageAt;
    QMap<Operation, QString> readQuality;
    QMap<QString, OkxOrderEvent> earlyEvents;
    QMap<QString, QString> lastOrderTime;
    QList<OrderRecord> batchReviewed, batchRemaining;
    bool isOkx() const { return account.provider == "okx"; }
    bool isBithumb() const { return account.provider == "bithumb"; }
    bool exactPreparation() const { return adapter && adapter->exchangePolicy().exactIntentPreflight; }
    qint64 serverNow() const { return serverAt < 0 ? 0 : serverAnchor + clock.elapsed() - serverAt; }
    bool fresh(Operation op, qint64 maxAge) const {
        return observedAt.contains(op) && clock.elapsed() - observedAt.value(op) <= maxAge;
    }
    void revokePrepared() { preparedRevision = 0; preparedDraft = {}; preparedTarget = {}; }
    TradingReadiness requestReady() const {
        if (!adapter) return {false,"no_account","연결을 선택해 주세요."};
        if (isOkx() && !okxRegionValid(account.region)) return {false,"region_missing","OKX 지역을 확인하고 인증정보를 교체해 주세요."};
        if (active || !jobs.isEmpty()) return {false,"busy","조회·요청 처리 중입니다."};
        if (manualReconnectRequired) return {false,"reconnect","공식 앱 확인 후 수동으로 다시 연결해 주세요."};
        if (clock.elapsed() < cooldownUntil || clock.elapsed() < pacingUntil) return {false,"pacing","요청 간격·제한 대기 중입니다."};
        return {true,"ready","요청 가능"};
    }
    bool sameOpen(const OrderRecord& reviewed) const {
        return std::count_if(open.cbegin(),open.cend(),[&](const OrderRecord& row){return row == reviewed;}) == 1;
    }
    bool pendingTarget(const OrderRecord& order) const {
        for (const auto& value : pending()) {
            const auto e=value.toObject();
            if((isBithumb() ? owns(e) : e.value("accountScope").toString()==accountScope) && e.value("symbol").toString()==order.symbol &&
               e.value("orderId").toString()==order.orderId && e.value("kind").toString()!="place") return true;
        }
        return false;
    }
    TradingReadiness cancelReady(const OrderRecord& order, bool sending=false) const {
        if (!sending) { const auto r=requestReady(); if(!r.ready)return r; }
        if(!connected || (isOkx() && (accountScope.isEmpty() || !permissionKnown || !tradeAllowed)))
            return {false,"permission","계정 연결과 주문 권한 확인이 필요합니다."};
        if(!vault.isUnlocked())return {false,"vault_locked","금고를 열어 주세요."};
        if(!openComplete || openAt<0 || clock.elapsed()-openAt>10000 || (isOkx() && (!fresh(Operation::AccountConfig,60000) ||
           serverAt<0 || clock.elapsed()-serverAt>60000))) return {false,"stale_orders","취소 조건을 다시 조회해 주세요."};
        if(!order.cancelable || (isOkx() && (order.tdMode!="cash" || order.instType!="SPOT" || order.linkedAlgo)) || !sameOpen(order))
            return {false,"changed_order","현재 조회된 일반 현물 주문을 다시 선택해 주세요."};
        if(pendingTarget(order))return {false,"pending_target","이 주문의 이전 취소·정정 결과가 미확정입니다."};
        return {true,"ready","취소 준비 완료"};
    }
    TradingReadiness draftReady(bool sending=false) const {
        if(!sending){const auto r=requestReady();if(!r.ready)return r;}
        if(!connected||(isOkx()&&(accountScope.isEmpty()||!permissionKnown||!tradeAllowed)))
            return {false,"permission","계정 연결과 주문 권한 확인이 필요합니다."};
        if(halted||!pending().isEmpty())return {false,"pending","신규 중지 또는 미확정 주문 대사가 필요합니다."};
        if(!vault.isUnlocked())return {false,"vault_locked","주문 기록을 저장하려면 금고를 열어 주세요."};
        if(!preparedRevision || preparedRevision!=draftRevision || !(draft==preparedDraft))
            return {false,"unprepared","현재 입력으로 주문 조건을 조회해 주세요."};
        if(isBithumb()) {
            if(!draft.orderId.isEmpty())return {false,"unsupported_amend","빗썸 정정은 지원하지 않습니다."};
            if(!fresh(Operation::Rules,10000)||!fresh(Operation::Account,15000))return {false,"stale_balance","현재 입력의 거래 조건·잔고를 다시 조회해 주세요."};
            if(!fresh(Operation::Quote,10000)||!fresh(Operation::Depth,10000))return {false,"stale_market","시세·호가가 만료되었습니다. 다시 준비해 주세요."};
            for(auto op:{Operation::Quote,Operation::Depth}){
                bool valid=false;const auto stamp=snapshot.data.value(op).value("timestamp").toString().toLongLong(&valid);
                const auto age=QDateTime::currentMSecsSinceEpoch()-stamp;
                if(!valid||stamp<=0||age>10000||age < -2000)return {false,"stale_market_source","거래소 원본 시세·호가 시각을 다시 확인해 주세요."};
            }
            const auto check=adapter->validate(draft,snapshot,limits);
            return {check.ok,check.ok?"ready":"validation",check.ok?"주문 준비 완료 · 거래 권한 미검증 · 새 버튼 클릭으로 전송":check.message};
        }
        if(serverAt<0||clock.elapsed()-serverAt>60000)return {false,"stale_time","서버 시간을 다시 확인해 주세요."};
        for(auto op:{Operation::AccountConfig,Operation::Rules,Operation::Commissions})
            if(!fresh(op,60000))return {false,"stale_rules","계정·종목·수수료 조건이 만료되었습니다."};
        for(auto op:{Operation::Account,Operation::BuyingPower})
            if(!fresh(op,15000))return {false,"stale_balance","주문 가능 금액을 다시 조회해 주세요."};
        for(auto op:{Operation::Quote,Operation::Depth,Operation::FxUsd,Operation::FxUsdt})
            if(!fresh(op,10000))return {false,"stale_market","호가·환산 지수가 만료되었습니다. 다시 준비해 주세요."};
        if(draft.type=="limit"&&!fresh(Operation::PriceLimits,10000))return {false,"stale_price_limits","가격 제한을 다시 조회해 주세요."};
        if(!draft.orderId.isEmpty()){
            if(!events||!events->ready()||events->generation()!=eventGeneration)return {false,"ws_unavailable","정정 결과 수신 연결이 필요합니다. 다시 연결·조회해 주세요."};
            if(!preparedTarget.amendable||!sameOpen(preparedTarget)||!openComplete||clock.elapsed()-openAt>10000)
                return {false,"changed_order","원주문이 변경되었거나 조회가 만료되었습니다. 다시 준비해 주세요."};
            if(!decimal::valid(preparedTarget.executedBaseAmount)||!decimal::positive(draft.quantity)||
               decimal::compare(draft.quantity,preparedTarget.executedBaseAmount)<=0)
                return {false,"quantity","정정 후 전체 수량은 현재 체결 수량보다 커야 합니다."};
        }
        auto currentSnapshot=snapshot;currentSnapshot.serverNowMs=serverNow();
        const auto check=adapter->validate(draft,currentSnapshot,limits);
        return {check.ok,check.ok?"ready":"validation",check.ok?"주문 준비 완료 · 새 버튼 클릭으로 전송":check.message};
    }
    OrderInput orderInput(const OrderRecord& order) const {
        OrderInput in;in.symbol=order.symbol;in.orderId=order.orderId;in.clientId=order.clientId;
        in.side=order.side;in.type=order.type;in.price=order.price;
        if(order.quantityUnit=="quote")in.quoteAmount=order.requestedQuoteAmount;
        else in.quantity=order.quantity;
        return in;
    }
    QString okxCoverage() const {
        QStringList lines;
        for(auto op:{Operation::AccountConfig,Operation::Account,Operation::OpenOrders,Operation::FundingBalance,Operation::History}){
            QString state=readQuality.value(op,"미조회");
            lines.append(operationName(op)+" · "+state);
        }
        lines.append("미체결은 SPOT 전체 조회 범위입니다. 별도 algo·전략 주문은 미조회·미포함이며 제외 수에도 넣지 않습니다.");
        lines.append("이력은 요청한 최근 7일 범위이며 미체결 없이 취소된 주문 등의 보존 예외가 있습니다.");
        return lines.join('\n');
    }
    void finishExactReads(){
        if(!jobs.isEmpty())return;
        if(preparingDraft){
            preparingDraft=false;
            if(!refreshFailed && preparingRevision==draftRevision){preparedRevision=draftRevision;preparedDraft=draft;}
        }
        preparingCancellation=false;
        if(!refreshFailed&&openComplete&&(isBithumb()?observedAt.contains(Operation::Account):!accountScope.isEmpty()&&fresh(Operation::AccountConfig,60000))){
            connected=true;verified("refresh_complete");
            change(isBithumb()?"계좌·자산 조회를 마쳤어요.":"계좌·자산 조회를 마쳤어요.");
        }
    }
    void applyOkxEvent(const OkxOrderEvent& event){
        if(!isOkx()||!events||!events->ready()||event.generation!=eventGeneration||events->generation()!=eventGeneration||
           accountScope.isEmpty()||event.accountScope!=accountScope)return;
        const QString key=event.instId+":"+event.ordId;
        if(lastOrderTime.contains(key)&&decimal::compare(event.uTime,lastOrderTime.value(key))<0)return;
        lastOrderTime[key]=event.uTime;
        // Events update known rows; they never claim a complete initial snapshot.
        const bool known=std::any_of(open.cbegin(),open.cend(),[&](const auto&o){return o.symbol==event.instId&&o.orderId==event.ordId;});
        if(known){
            mergeOrders(terminal(event.order)?closed:open,{event.order});
            if(terminal(event.order))open.erase(std::remove_if(open.begin(),open.end(),[&](const auto&o){return o.symbol==event.instId&&o.orderId==event.ordId;}),open.end());
            snapshot.openOrders=open;
        }
        if(event.reqId.isEmpty()||event.amendResult.isEmpty()){change();return;}
        for(const auto& value:pending()){
            auto entry=value.toObject();
            if(!owns(entry)||entry.value("kind")!="amend"||entry.value("symbol")!=event.instId||entry.value("orderId")!=event.ordId||
               entry.value("amendRequestId")!=event.reqId||decimal::compare(event.uTime,entry.value("beforeUpdatedAt").toString())<0)continue;
            const QString id=entry.value("id").toString();
            if(active&&current.journalId==id){ if(earlyEvents.size()<128)earlyEvents[id]=event; return; }
            bool applied=event.amendResult=="0"&&event.order.tdMode=="cash"&&event.order.instType=="SPOT"&&!event.order.linkedAlgo&&
                event.order.symbol==entry.value("symbol").toString()&&event.order.side==entry.value("side").toString()&&event.order.type=="limit"&&
                event.order.quantityUnit=="base"&&decimal::valid(event.order.price)&&decimal::valid(event.order.quantity)&&
                decimal::compare(event.order.price,entry.value("price").toString())==0&&decimal::compare(event.order.quantity,entry.value("quantity").toString())==0;
            if(!applied&&event.amendResult!="-1")continue;
            auto row=event.order;row.amendRequestId=event.reqId;row.requestState=entry.value("ackState").toString();
            row.actionOutcome=applied?"applied":"rejected";activity[id]=row;
            if(!saveJournal(entry,true))return;
            verified(applied?"amend_reconciled":"amend_rejected_observed");
            accountAt=-1;observedAt.remove(Operation::Account);revokePrepared();
            change(applied?"정정 결과 확인 완료 · 현재 주문 상태는 별도 표시합니다.":"정정 거절 확인 · 원 주문의 현재 상태를 확인하세요.");return;
        }
        change();
    }

    Private(TradingController* owner, Vault& v, TradingTransport* t) : q(owner), vault(v), transport(t) { clock.start(); }
    void change(const QString& text = {}) { if (!text.isEmpty()) message = text; emit q->changed(); }
    void verified(const QString& name) {
        checks[name] = QJsonObject{{"observed", true}, {"at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    }
    bool updateCatalog(const QJsonObject& data) {
        const auto rows = data.value((account.provider == "upbit" || isOkx() || isBithumb()) ? "instruments" : "symbols");
        if (!rows.isArray() || rows.toArray().isEmpty() || rows.toArray().size() > 20000) return false;
        QList<MarketInstrument> incoming;
        QSet<QString> seen;
        for (const auto& value : rows.toArray()) {
            if (!value.isObject()) return false;
            const auto row = value.toObject();
            MarketInstrument instrument;
            instrument.symbol = row.value("symbol").toString();
            instrument.baseAsset = row.value("baseAsset").toString();
            instrument.quoteAsset = row.value("quoteAsset").toString();
            if (seen.contains(instrument.symbol)) return false;
            seen.insert(instrument.symbol);
            if (account.provider == "upbit" || isBithumb()) {
                if (!QRegularExpression("^(KRW|BTC|USDT)-[A-Z0-9]{1,24}$").match(instrument.symbol).hasMatch() ||
                    instrument.symbol != instrument.quoteAsset + "-" + instrument.baseAsset) return false;
                instrument.name = row.value("name").toString();
                instrument.englishName = row.value("englishName").toString();
                instrument.warning = row.value("warning").toString();
                instrument.status = "목록 등록 · 상태 조회 필요";
                if (!instrument.warning.isEmpty()) instrument.status += " · " + instrument.warning;
                instrument.tradable = !isBithumb() || instrument.quoteAsset == "KRW" || instrument.quoteAsset == "BTC";
            } else if (isOkx()) {
                const auto state=row.value("status").toString();
                if(!okxInstrumentValid(instrument.symbol)||instrument.symbol!=instrument.baseAsset+"-"+instrument.quoteAsset ||
                   !row.value("tradable").isBool()||state.isEmpty())return false;
                instrument.status=state;instrument.tradable=row.value("tradable").toBool();instrument.warning=row.value("warning").toString();
            } else {
                const QString state = row.value("status").toString();
                if (!binanceIdentifierValid(instrument.symbol, 2) || !binanceIdentifierValid(instrument.baseAsset) ||
                    !binanceIdentifierValid(instrument.quoteAsset) || !QRegularExpression("^[A-Z_]{1,40}$").match(state).hasMatch() ||
                    !row.value("isSpotTradingAllowed").isBool()) return false;
                instrument.tradable = state == "TRADING" && row.value("isSpotTradingAllowed").toBool();
                instrument.status = state == "TRADING" ? "거래 가능" : state == "HALT" ? "거래 중단" :
                    state == "BREAK" ? "거래 휴지" : "상태: " + state;
                if (!row.value("isSpotTradingAllowed").toBool()) instrument.status += " · 현물 미지원";
                // exchangeInfo provides asset codes, not official long names.
            }
            incoming.append(instrument);
        }
        catalog = incoming;
        catalogAt = clock.elapsed(); catalogReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
        catalogFailure.clear();
        return true;
    }
    bool matching(const QJsonObject& entry) const {
        if (entry.value("market").toString() == "usdm_futures") return false;
        // Keys may be rotated or re-entered in session-only mode. Never silently
        // discard an unresolved operation merely because the key/connection id changed.
        return entry.value("provider").toString() == account.provider && entry.value("environment").toString() == account.environment &&
            (!isOkx() || (entry.value("region").toString()==account.region &&
                (accountScope.isEmpty()?entry.value("accountId").toString()==account.id:entry.value("accountScope").toString()==accountScope)));
    }
    bool owns(const QJsonObject& entry) const { return matching(entry) && entry.value("fingerprint").toString() == fingerprint; }
    QJsonArray pending() const {
        QJsonArray result;
        for (const auto& value : vault.pendingOperations()) if (matching(value.toObject())) result.append(value);
        return result;
    }
    QJsonObject journal(const QString& id) const {
        for (const auto& value : vault.pendingOperations()) if (value.toObject().value("id").toString() == id) return value.toObject();
        return {};
    }
    bool saveJournal(const QJsonObject& entry, bool remove = false) {
        auto all = vault.pendingOperations();
        int index = -1;
        for (int i = 0; i < all.size(); ++i) if (all[i].toObject().value("id") == entry.value("id")) { index = i; break; }
        if (index >= 0) { if (remove) all.removeAt(index); else all[index] = entry; }
        else if (!remove) all.append(entry);
        QString error;
        if (!vault.setPendingOperations(all, &error)) { change(error); return false; }
        return true;
    }

    bool ready(bool needsForeground, QWidget* origin, bool mutation, bool newRisk) {
        if (!adapter || !connected) { change("먼저 조회 연결을 확인해 주세요."); return false; }
        if (active || !jobs.isEmpty()) { change("요청 처리 중입니다. 중복 전송하지 않았습니다."); return false; }
        if (manualReconnectRequired || clock.elapsed() < cooldownUntil || clock.elapsed() < pacingUntil) { change("거래소 요청 제한 대기 중입니다. 자동 재전송하지 않습니다."); return false; }
        if (needsForeground && !foreground(origin)) { change("활성 주문 화면의 직접 버튼 입력만 허용합니다."); return false; }
        if (mutation && !vault.isUnlocked()) { change("주문 기록을 암호화해 저장하려면 금고를 열어 주세요."); return false; }
        if (newRisk && (halted || !pending().isEmpty())) { change("신규 주문 중지 또는 미확정 주문 대사가 필요합니다."); return false; }
        if (newRisk && (!q->quoteFresh() || accountAt < 0 || clock.elapsed() - accountAt > 15000 || rulesAt < 0 || clock.elapsed() - rulesAt > 60000)) {
            change("시세·잔고·주문 규칙이 오래되었습니다. 새로 조회해 주세요."); return false;
        }
        if ((account.provider == "binance" || isOkx()) && (serverAt < 0 || clock.elapsed() - serverAt > 60000 ||
            (account.provider == "binance" && !orderTimeReady))) {
            change("Binance 서버 시간을 다시 확인해 주세요."); return false;
        }
        return true;
    }
    void enqueueReconciliation(const QJsonObject& entry, bool first = false) {
        if (!owns(entry)) return;
        const QString id = entry.value("id").toString();
        OrderInput input; input.symbol = entry.value("symbol").toString();
        input.orderId = entry.value("orderId").toString(); input.clientId = entry.value("clientId").toString();
        input.amendRequestId=entry.value("amendRequestId").toString();
        input.orderListId=entry.value("orderListId").toString();
        QList<Job> queries{{input.orderListId.isEmpty()?Operation::QueryOrder:Operation::OrderList, input, id, false}};
        if (entry.value("kind") == "amend" && (account.provider == "upbit"||entry.value("replaceMode")=="cancel_replace")) {
            input.orderId.clear(); input.clientId = entry.value("replacementClientId").toString();
            queries.append({Operation::QueryOrder, input, id, true});
        }
        if (first) for (auto it = queries.crbegin(); it != queries.crend(); ++it) jobs.prepend(*it);
        else for (const auto& query : queries) jobs.enqueue(query);
    }
    void schedule(int delay = 300) {
        const auto epoch = generation;
        QTimer::singleShot(delay, q, [this, epoch] { if (epoch == generation) next(); });
    }
    void next() {
        if (active || !adapter) return;
        if (jobs.isEmpty()) { change(); return; }
        if (manualReconnectRequired) { jobs.clear(); change("차단 해제 시점을 확인하지 못했습니다. 공식 거래소에서 확인한 뒤 수동으로 다시 조회해 주세요."); return; }
        if (clock.elapsed() < pacingUntil) { schedule(int(pacingUntil - clock.elapsed())); return; }
        if (clock.elapsed() < cooldownUntil) { jobs.clear(); change("요청 제한으로 남은 조회를 중단했습니다. 잠시 후 다시 조회해 주세요."); return; }
        current = jobs.dequeue();

        QString error; RequestSpec request;
        try {
            const qint64 serverNow = serverAt < 0 ? QDateTime::currentMSecsSinceEpoch() : serverAnchor + clock.elapsed() - serverAt;
            bool built = true;
            if (current.operation == Operation::Markets || (isOkx() && QList<Operation>{Operation::Time,Operation::Quote,Operation::Depth,Operation::FxUsd,Operation::FxUsdt,Operation::PriceLimits}.contains(current.operation)) || (isBithumb() && QList<Operation>{Operation::Quote,Operation::Depth}.contains(current.operation))) {
                // A public catalogue must never access or attach financial credentials.
                request = adapter->build(current.operation, account, current.input, CredentialSet{}, serverNow, &error);
                built = !request.authenticated && !request.headers.contains("Authorization") && !request.headers.contains("X-MBX-APIKEY") && !request.headers.contains("OK-ACCESS-KEY");
            } else built = vault.withCredentials(account.id, [&](const CredentialSet& credentials) {
                request = adapter->build(current.operation, account, current.input, credentials, serverNow, &error);
            }, &error);
            if (!built || !error.isEmpty() || request.url.isEmpty()) {
                if (current.catalogOnly) {
                    catalogLoading = false; catalogFailure = "공개 목록 요청 구성을 거부했습니다.";
                    jobs.clear(); change(); return;
                }
                refreshFailed = true; connected = false; jobs.clear(); change(error.isEmpty() ? "요청 구성을 거부했습니다." : error); return;
            }
        } catch (const std::exception&) {
            jobs.clear();
            if (current.catalogOnly) { catalogLoading = false; catalogFailure = "공개 목록 요청 구성에 실패했습니다."; change(); return; }
            connected = false; change("인증 요청 구성에 실패했습니다."); return;
        }

        active = true; sentAt = timingNow(); ++token;

        transport->send(token, request);
        wipe(request.body); for (auto& header : request.headers) wipe(header);
        change(current.catalogOnly ? QString{} : operationName(current.operation) + " 요청 중");
    }
    void onResult(quint64 replyToken, const HttpResult& response) {
        if (!active || replyToken != token || !adapter) return;
        active = false;
        if(exactPreparation())pacingUntil=qMax(pacingUntil,clock.elapsed()+300);
        const Job completed = current;
        AdapterResult result = response.localRetryAfterSeconds>0 ? localRateLimitFailure(response) : adapter->parse(completed.operation, response, completed.input);
        for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it) {
            if (it.key().compare("Remaining-Req", Qt::CaseInsensitive) == 0) {
                const auto remaining = QRegularExpression("(?:^|;)\\s*sec=([0-9]+)(?:;|$)").match(QString::fromLatin1(it.value()));
                if (remaining.hasMatch() && remaining.captured(1) == "0") pacingUntil = clock.elapsed() + 1100;
            }
        }
        if (result.rateLimited || response.status == 429 || response.status == 418) {
            cooldownUntil = clock.elapsed() + (response.localRetryAfterSeconds>0 ? response.localRetryAfterSeconds : qMax(60, result.retryAfterSeconds)) * 1000LL;
            enabled = false; connected = false;
            bool explicitRetry = false;
            for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
                if (it.key().compare("Retry-After", Qt::CaseInsensitive) == 0) {
                    bool ok = false; const auto value = it.value().toLongLong(&ok); explicitRetry = ok && value > 0;
                }
            if ((response.status == 418 || (isBithumb() && (result.rateLimited || response.status==429))) && !explicitRetry) { manualReconnectRequired = true; cooldownUntil = 0; }
        }
        if(exactPreparation()&&result.authenticationFailed){
            enabled=connected=false;permissionKnown=tradeAllowed=false;manualReconnectRequired=true;
            revokePrepared();if(events)events->stop();
        }
        if (completed.operation == Operation::Markets) {
            if (result.ok && !updateCatalog(result.data)) {
                result.ok = false; result.message = "공개 종목 목록 형식을 확인하지 못했습니다.";
            }
            if (!result.ok) catalogFailure = result.message.isEmpty() ? "공개 종목 목록 조회에 실패했습니다." : result.message;
            if (completed.catalogOnly) {
                catalogLoading = false;
                // Public data must not produce authenticated verification receipts,
                // connected state, account coverage, or automatic account polling.
                change(); schedule(); return;
            }
        }
        if (completed.operation == Operation::Time && assetsOnly && account.provider == "binance") {
            const qint64 rtt = timingNow() - sentAt;
            serverAt = -1; orderTimeReady = false;
            if (result.ok && result.serverTimeMs > 0 && binanceReadTimeUsable(rtt)) {
                serverAt = clock.elapsed(); serverAnchor = result.serverTimeMs;
            } else {
                result.message = binanceAssetReadFailure(response, result, rtt);
                if (!result.authenticationFailed && !result.rateLimited && response.status != 429 && response.status != 418 && assetsTimeRetries++ == 0) {
                    jobs.prepend(completed);
                    change(QString("서버 시간 측정 재시도 · 왕복 %1 ms").arg(rtt)); schedule(); return;
                }
                result.ok = false;
            }
        } else if (completed.operation == Operation::Time && result.ok) {
            const qint64 rtt = timingNow() - sentAt;
            orderTimeReady = false; serverAt = -1;
            if (result.serverTimeMs <= 0 || rtt > BinanceOrderMaxRttMs) { result.ok = false; result.message = "서버 시간 왕복 지연이 커서 주문을 차단합니다."; }
            else { serverAt = clock.elapsed(); serverAnchor = result.serverTimeMs + rtt / 2; orderTimeReady = true; }
        }

        if(completed.operation==Operation::OrderList){
            auto entry=journal(completed.journalId);if(entry.isEmpty()){schedule();return;}
            if(result.ok&&result.data.value("orderListId")==entry.value("orderListId")&&result.data.value("symbol")==entry.value("symbol")&&result.data.value("listOrderStatus")=="ALL_DONE"){
                if(!saveJournal(entry,true))return;
                const auto list=entry.value("orderListId").toString(),symbol=entry.value("symbol").toString();
                open.erase(std::remove_if(open.begin(),open.end(),[&](const auto& row){return row.orderListId==list&&row.symbol==symbol;}),open.end());snapshot.openOrders=open;
                activity[completed.journalId].status="closed";activity[completed.journalId].requestState="confirmed";activity[completed.journalId].actionOutcome="list_closed";verified("order_list_closed");
                change("주문 묶음의 종료를 확인했습니다. 취소 전 체결 여부는 종목 이력과 공식 앱에서 확인하세요.");
            }else{jobs.clear();enabled=false;change("주문 묶음 종료가 미확정입니다. 자동 재전송 없이 묶음 ID로 다시 조회하세요.");}
            schedule();return;
        }
        if (completed.operation == Operation::QueryOrder) {
            auto entry = journal(completed.journalId);
            if (entry.isEmpty()) { schedule(); return; }
            bool resolved = false;
            if(entry.value("replaceMode")=="cancel_replace"){
                const auto state=entry.value("replacementState").toString();
                if(result.ok&&result.orders.size()==1){const auto o=result.orders.first();
                    if(o.symbol==completed.input.symbol&&(completed.replacement?o.clientId==completed.input.clientId:o.orderId==completed.input.orderId)){
                        reconciled[completed.journalId][completed.replacement]=o;mergeOrders(terminal(o)?closed:open,{o});
                        if(terminal(o))open.erase(std::remove_if(open.begin(),open.end(),[&](const auto& x){return x.symbol==o.symbol&&x.orderId==o.orderId;}),open.end());
                    }
                }
                const auto both=reconciled.value(completed.journalId);
                if(both.contains(false)&&both.contains(true)){
                    const auto old=both.value(false),created=both.value(true);
                    resolved=old.status=="canceled"&&created.status!="unknown"&&created.side==entry.value("side")&&created.type==entry.value("type")&&created.orderId!=old.orderId&&
                        decimal::valid(created.price)&&decimal::valid(created.quantity)&&decimal::compare(created.price,entry.value("price").toString())==0&&decimal::compare(created.quantity,entry.value("quantity").toString())==0;
                    if(resolved){auto row=created;row.originalOrderId=old.orderId;row.actionOutcome="applied";row.requestState="confirmed";activity[completed.journalId]=row;}
                }else if(completed.replacement&&result.notFound&&both.contains(false)){
                    const auto old=both.value(false);resolved=(state=="FAILURE"&&old.status=="canceled")||(state=="NOT_ATTEMPTED"&&old.status!="unknown");
                    if(resolved){auto row=old;row.actionOutcome=state=="FAILURE"?"canceled_new_rejected":"cancel_rejected_new_not_attempted";row.requestState="rejected";activity[completed.journalId]=row;}
                }
                if(resolved){if(!saveJournal(entry,true))return;verified("cancel_replace_reconciled");accountAt=-1;enabled=false;
                    change(state=="FAILURE"?"원주문 취소 확인 · 새 주문 거절. 추가 주문을 자동 전송하지 않습니다.":state=="NOT_ATTEMPTED"?"취소 거절 · 새 주문 미시도 확인. 원주문 상태를 확인하세요.":"원주문 취소와 새 주문을 각각 확인했습니다. 최신 잔고를 다시 조회해 주세요.");}
                else change("취소·새 주문 결과 미확정 · 자동 재전송 없이 두 주문을 조회합니다.");
                schedule();return;
            }
            if(exactPreparation()){
                const QString id=completed.journalId,kind=entry.value("kind").toString();
                if(result.ok&&result.orders.size()==1){
                    const auto row=result.orders.first();
                    const bool identity=row.symbol==entry.value("symbol").toString() &&
                        (!entry.value("orderId").toString().isEmpty()?row.orderId==entry.value("orderId").toString():row.clientId==entry.value("clientId").toString());
                    if(identity){
                        mergeOrders(terminal(row)?closed:open,{row});
                        if(terminal(row))open.erase(std::remove_if(open.begin(),open.end(),[&](const auto&o){return o.symbol==row.symbol&&o.orderId==row.orderId;}),open.end());
                        const auto unit=isOkx()?entry.value("quantityUnit").toString():!entry.value("quoteAmount").toString().isEmpty()?QString("quote"):QString("base");
                        const bool terms=(!isOkx()||(row.tdMode=="cash"&&row.instType=="SPOT"&&!row.linkedAlgo))&&row.side==entry.value("side").toString()&&row.type==entry.value("type").toString()&&
                            row.quantityUnit==unit&&
                            (row.quantityUnit=="quote"?decimal::valid(row.requestedQuoteAmount)&&decimal::compare(row.requestedQuoteAmount,entry.value("quoteAmount").toString())==0:
                             decimal::valid(row.quantity)&&decimal::compare(row.quantity,entry.value("quantity").toString())==0)&&
                            (row.type!="limit"||(decimal::valid(row.price)&&decimal::compare(row.price,entry.value("price").toString())==0));
                        resolved=kind=="place"?terms&&row.status!="unknown":kind=="cancel"?terms&&(row.status=="canceled"||(isBithumb()&&terminal(row))):false;
                        auto action=row;action.requestState=isOkx()?entry.value("ackState").toString():activity.value(id).requestState;action.amendRequestId=entry.value("amendRequestId").toString();
                        action.actionOutcome=resolved&&!(kind=="cancel"&&row.status!="canceled")?"applied":terminal(row)?"terminal_without_action_proof":"unconfirmed";activity[id]=action;
                    }
                }
                if(resolved){if(!saveJournal(entry,true))return;verified(kind+"_reconciled");}
                else {entry["state"]="unknown";if(!saveJournal(entry))return;enabled=false;batchRemaining.clear();}
                snapshot.openOrders=open;accountAt=-1;observedAt.remove(Operation::Account);revokePrepared();

                change(resolved?"요청 대상의 주문 상태를 확인했습니다.":"결과 미확정 — 재전송하지 않았습니다. 공식 앱과 대사해 주세요.");schedule();return;
            }
            if (result.ok && !result.orders.isEmpty()) {
                const auto order = result.orders.first();
                if (order.symbol != completed.input.symbol ||
                    (!completed.input.orderId.isEmpty() && order.orderId != completed.input.orderId) ||
                    (completed.input.orderId.isEmpty() && order.clientId != completed.input.clientId)) {
                    result.ok = false;
                }
            }
            if (result.ok && !result.orders.isEmpty()) {
                const auto order = result.orders.first();
                reconciled[completed.journalId][completed.replacement] = order;
                mergeOrders(terminal(order) ? closed : open, {order});
                if (terminal(order)) open.erase(std::remove_if(open.begin(), open.end(), [&](const OrderRecord& o) { return o.orderId == order.orderId && o.symbol == order.symbol; }), open.end());
                const QString kind = entry.value("kind").toString();
                if (kind == "place") resolved = order.status != "unknown";
                else if (kind == "cancel") resolved = terminal(order);
                else if (account.provider == "binance") resolved = decimal::valid(order.quantity) && decimal::valid(entry.value("quantity").toString()) && decimal::compare(order.quantity, entry.value("quantity").toString()) == 0;
                else {
                    const auto both = reconciled.value(completed.journalId);
                    resolved = both.contains(false) && both.contains(true) && terminal(both.value(false)) && both.value(true).status != "unknown";
                }
            }
            if (resolved) {
                verified(entry.value("kind").toString() + "_reconciled");
                if (result.ok && !result.orders.isEmpty()) {
                    const auto& observed = result.orders.first();
                    if (activity.contains(completed.journalId)) {
                        auto& action = activity[completed.journalId]; action.status = observed.status;
                        action.orderId = observed.orderId; action.executed = observed.executed; action.remaining = observed.remaining;
                    }
                    if (decimal::positive(observed.executed)) verified("fill_observed");
                    if (observed.status == "partial") verified("partial_fill_observed");
                    if (observed.status == "canceled") verified("cancel_observed");
                }
                if (!saveJournal(entry, true)) return;
                change("서버 주문 상태 확인 완료. 체결·취소 결과는 주문 목록에서 확인하세요.");
            } else {
                entry["state"] = "unknown";
                if (!saveJournal(entry)) return;
                enabled = false;
                change("결과 미확정 — 주문을 다시 보내지 않았습니다. 거래소 확인 후 ‘미확정 대사’를 실행하세요.");
            }
            snapshot.openOrders = open;
            accountAt = -1; // Balances must be refreshed before taking new risk.
            schedule(); return;
        }

        if(exactPreparation()){
            if(!result.ok){
                readQuality[completed.operation]="조회 실패 · 이전 값은 최신이 아님";
                const bool optional=!preparingDraft&&!preparingCancellation&&(QList<Operation>{Operation::FundingBalance,Operation::History}.contains(completed.operation)||(isBithumb()&&completed.operation==Operation::Rules));
                observedAt.remove(completed.operation);snapshot.data.remove(completed.operation);
                if(result.authenticationFailed||result.rateLimited||!optional){
                    refreshFailed=true;enabled=false;revokePrepared();jobs.clear();batchRemaining.clear();
                    if(result.authenticationFailed||result.rateLimited){connected=false;if(events)events->stop();}
                    if(completed.operation==Operation::OpenOrders)openComplete=false;
                    change(operationName(completed.operation)+" 실패 — "+result.message);return;
                }
                finishExactReads();schedule();return;
            }
            if(completed.operation==Operation::AccountConfig){
                if(result.scope.isEmpty()||(!accountScope.isEmpty()&&accountScope!=result.scope)){
                    jobs.clear();enabled=connected=false;if(events)events->stop();revokePrepared();change("인증 계정 범위가 변경되어 연결을 중지했습니다.");return;
                }
                accountScope=result.scope;permissionKnown=result.tradingPermissionKnown;tradeAllowed=result.tradingAllowed;
                permission=tradeAllowed?"API의 trade 권한 확인 · 실주문 미검증":"조회 권한만 확인 · 주문 권한 없음/미확인";
                if(!tradeAllowed)enabled=false;
                if(events && !events->ready() && !preparingDraft && !preparingCancellation){
                    QString error;vault.withCredentials(account.id,[&](const CredentialSet& creds){events->start(account,creds,accountScope,serverNow());},&error);
                    eventGeneration=events->generation();
                }
            }
            const auto op=completed.operation;
            if((isOkx()&&QList<Operation>{Operation::Quote,Operation::Depth,Operation::FxUsd,Operation::FxUsdt,Operation::PriceLimits}.contains(op))||(isBithumb()&&(op==Operation::Quote||op==Operation::Depth))){
                bool valid=false;const auto stamp=result.data.value("timestamp").toString().toLongLong(&valid);
                const auto referenceNow=isBithumb()?QDateTime::currentMSecsSinceEpoch():serverNow();
                if(!valid||stamp<=0||referenceNow<=0||referenceNow-stamp>10000||stamp-referenceNow>2000){
                    jobs.clear();refreshFailed=true;enabled=false;revokePrepared();observedAt.remove(op);
                    change("거래소 시세·지수 원본 시각이 만료되었거나 확인되지 않습니다.");return;
                }
                const auto previous=snapshot.data.value(op).value("timestamp").toString();
                if(!previous.isEmpty()&&decimal::compare(QString::number(stamp),previous)<0){jobs.clear();refreshFailed=true;revokePrepared();change("원본 시세 시각이 역행해 준비를 중단했습니다.");return;}
            }
            if(op==Operation::OpenOrders||op==Operation::History){
                if(!firstPageAt.contains(op))firstPageAt[op]=clock.elapsed();
                bool valid=true;
                for(const auto& row:result.orders){const QString key=row.symbol+":"+row.orderId;if(pageIdentities[op].contains(key)||(op==Operation::OpenOrders&&lastOrderTime.contains(key)&&decimal::compare(row.sourceUpdatedAt,lastOrderTime.value(key))<0)){valid=false;break;}pageIdentities[op].insert(key);pageRows[op].append(row);}
                if(!valid||pageRows[op].size()>20000||(++pageCount)>100||(result.hasMore&&(result.nextCursor.isEmpty()||pageCursors[op].contains(result.nextCursor)))){
                    jobs.clear();refreshFailed=true;enabled=false;revokePrepared();batchRemaining.clear();if(op==Operation::OpenOrders)openComplete=false;
                    readQuality[op]="불완전 · 반복/중복/상한";change("주문 목록의 전체 범위를 확인하지 못했습니다.");return;
                }
                if(result.hasMore){pageCursors[op].insert(result.nextCursor);auto continuation=completed;continuation.input.cursor=result.nextCursor;jobs.prepend(continuation);schedule();return;}
                if(op==Operation::OpenOrders){open=pageRows[op];snapshot.openOrders=open;openAt=firstPageAt.value(op);openComplete=true;
                    if(preparingDraft&&!draft.orderId.isEmpty()){
                        const auto it=std::find_if(open.cbegin(),open.cend(),[&](const auto&o){return o.orderId==draft.orderId&&o.symbol==draft.symbol;});
                        preparedTarget=it==open.cend()?OrderRecord{}:*it;
                    }
                }else closed=pageRows[op];
                pageCount=0;
            }
            snapshot.data[op]=result.data;observedAt[op]=clock.elapsed();receivedWall[op]=QDateTime::currentMSecsSinceEpoch();
            readQuality[op]="조회 완료 · "+QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss")+" KST";
            if(op==Operation::Account||op==Operation::FundingBalance){
                const QString section=op==Operation::Account?"trading":"funding";
                if(isBithumb())snapshot.balances.clear();
                snapshot.balances.erase(std::remove_if(snapshot.balances.begin(),snapshot.balances.end(),[&](const auto&b){return b.section==section;}),snapshot.balances.end());
                snapshot.balances.append(result.balances);
                if(isBithumb())permission="자산·주문 이력 조회 인증 확인";
                if(op==Operation::Account){accountAt=clock.elapsed();verified("authenticated_account_read");}
            }
            if(op==Operation::Quote){quoteAt=clock.elapsed();snapshot.quoteReceivedAtMs=QDateTime::currentMSecsSinceEpoch();}
            if(op==Operation::Rules)rulesAt=clock.elapsed();
            finishExactReads();

            schedule();return;
        }
        if (!result.ok) {
            if(assetsOnly&&account.provider=="binance"&&completed.operation!=Operation::Time)
                result.message=binanceAssetReadFailure(response,result,timingNow()-sentAt);
            if(historyOnly&&!result.authenticationFailed&&!result.rateLimited){historyOnly=false;jobs.clear();readQuality[Operation::History]="이력 조회 실패 · 잔고 조회와 별도";change("과거 이력 조회 실패 · 마지막 계좌 자산은 유지합니다.");return;}
            refreshFailed = true; connected = false; enabled = false; jobs.clear();
            change(operationName(completed.operation) + " 조회 실패 — " + result.message);
            return;
        }
        snapshot.data[completed.operation] = result.data;
        observedAt[completed.operation] = clock.elapsed();receivedWall[completed.operation]=QDateTime::currentMSecsSinceEpoch();
        if (completed.operation == Operation::Quote) {
            // A recently received old order book is still stale. Upbit supplies
            // an exchange timestamp; Binance REST ticker supplies receipt time only.
            if (account.provider == "upbit") {
                bool valid = false; const auto sourceTime = result.data.value("timestamp").toString().toLongLong(&valid);
                const qint64 age = QDateTime::currentMSecsSinceEpoch() - sourceTime;
                if (!valid || age < -2000 || age > 10000) {
                    quoteAt = -1; connected = false; enabled = false; jobs.clear();
                    change("거래소 호가 시각이 오래되었거나 PC 시각과 맞지 않습니다."); return;
                }
            }
            quoteAt = clock.elapsed(); snapshot.quoteReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
        }
        if (completed.operation == Operation::Account) {
            verified("authenticated_account_read");
            snapshot.balances = result.balances; accountAt = clock.elapsed();
            if (result.tradingPermissionKnown && !result.tradingAllowed) { tested = false; enabled = false; permission = "거래소가 주문 권한 없음으로 응답했습니다."; }
        }
        if (completed.operation == Operation::Rules) rulesAt = clock.elapsed();
        if (completed.operation == Operation::OpenOrders) { mergeOrders(open, result.orders); snapshot.openOrders = open; }
        if (completed.operation == Operation::History) mergeOrders(closed, result.orders);
        if (!result.scope.isEmpty() && !coverage.contains(result.scope)) coverage += (coverage.isEmpty() ? "" : "\n") + result.scope;
        if (result.hasMore) {
            if (++pageCount > 100 || result.nextCursor.isEmpty() || open.size() + closed.size() > 20000) {
                refreshFailed = true; enabled = false; jobs.clear(); connected = false;
                change("조회량 상한에 도달하여 범위가 불완전합니다. 주문을 활성화하지 않았습니다."); return;
            }
            Job continuation = completed; continuation.input.cursor = result.nextCursor;
            jobs.prepend(continuation);
        }
        if (jobs.isEmpty() && !refreshFailed) {
            if(historyOnly){verified("history_read_complete");change("선택 범위 주문 이력 조회 완료");}
            else if (assetsOnly) { verified("authenticated_assets_read_complete"); change("계좌 자산 조회 완료 · 종목 거래 조건은 별도 조회"); }
            else { connected = true; verified("refresh_complete"); change("계좌 조회를 마쳤어요."); }
        }
        schedule();
    }
    void refresh(const QString& symbol, bool background = false,bool includeHistory=false) {
        if (!adapter || active || !jobs.isEmpty() || clock.elapsed() < cooldownUntil) return;
        if (background && manualReconnectRequired) return;
        if (!background) manualReconnectRequired = false;
        const bool validSymbol = isOkx() ? okxInstrumentValid(symbol) : (account.provider == "upbit" || isBithumb()) ?
            QRegularExpression("^[A-Z0-9]{2,12}-[A-Z0-9]{1,24}$").match(symbol).hasMatch() : binanceIdentifierValid(symbol, 2);
        if (!validSymbol) { change("종목 형식을 확인해 주세요. 예: KRW-BTC / BTCUSDT"); return; }
        assetsOnly = historyOnly = false;
        if (market != symbol) { tested = false; enabled = false; snapshot = {}; quoteAt = accountAt = rulesAt = -1; }
        market = symbol; refreshFailed = false; pageCount = 0;
        if(exactPreparation()){
            if(isOkx()&&!okxRegionValid(account.region)){change("OKX 지역을 먼저 확인해 주세요.");return;}
            revokePrepared();preparingDraft=preparingCancellation=false;pageRows.clear();pageCursors.clear();pageIdentities.clear();firstPageAt.clear();openComplete=false;
            OrderInput in;in.symbol=symbol;in.endTimeMs=QDateTime::currentMSecsSinceEpoch();in.startTimeMs=in.endTimeMs-7LL*86400000;
            for(auto op:adapter->refreshPlan())if(includeHistory||op!=Operation::History)jobs.enqueue({op,in});next();return;
        }
        open.clear(); if (!background) { closed.clear(); coverage.clear(); }
        OrderInput input; input.symbol = symbol; input.endTimeMs = QDateTime::currentMSecsSinceEpoch(); input.startTimeMs = input.endTimeMs - 7LL * 86400000;
        for (auto op : adapter->refreshPlan()) {
            if(!includeHistory&&op==Operation::History)continue;
            if (background && (op == Operation::Markets || op == Operation::History)) continue;
            jobs.enqueue({op, input, {}, false});
        }
        // Account and quote must be fresh even when historical pagination was slow.
        if (includeHistory&&account.provider == "binance") {
            jobs.enqueue({Operation::Time, input, {}, false});
            for (auto op : {Operation::Reference, Operation::AveragePrice, Operation::Depth}) jobs.enqueue({op, input, {}, false});
        }
        if(includeHistory){jobs.enqueue({Operation::Account, input, {}, false});jobs.enqueue({Operation::Quote, input, {}, false});}
        next();
    }
};

TradingController::TradingController(Vault& vault, QObject* parent)
    : TradingController(vault, new NetworkTradingTransport, parent) { d_->transport->setParent(this); }
TradingController::TradingController(Vault& vault, TradingTransport* transport, QObject* parent, std::function<qint64()> timingClock)
    : TradingController(vault, transport, dynamic_cast<NetworkTradingTransport*>(transport) ? new NetworkOkxOrderEvents : nullptr, parent) {
    if(d_->events) d_->events->setParent(this);
    d_->timingClock=std::move(timingClock);
}
TradingController::TradingController(Vault& vault, TradingTransport* transport, OkxOrderEvents* events, QObject* parent)
    : QObject(parent), d_(std::make_unique<Private>(this, vault, transport)) {
    d_->events=events;
    if(events){
        connect(events,&OkxOrderEvents::stateChanged,this,[this]{emit changed();});
        connect(events,&OkxOrderEvents::orderEvent,this,[this](const OkxOrderEvent& event){d_->applyOkxEvent(event);});
    }
    d_->readinessTimer.setInterval(250);
    connect(&d_->readinessTimer,&QTimer::timeout,this,[this]{
        if(!d_->exactPreparation())return;
        const QString key=d_->requestReady().code+":"+d_->draftReady().code+":"+QString::number(d_->openAt>=0&&d_->clock.elapsed()-d_->openAt<=10000);
        if(key!=d_->readinessKey){d_->readinessKey=key;emit changed();}
    });
    d_->readinessTimer.start();
    connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) { d_->onResult(token, result); });
    connect(&vault, &Vault::aboutToLock, this, &TradingController::stop);
    connect(&vault, &Vault::accountsChanged, this, [this] { if (!d_->account.id.isEmpty()) stop(); });
    d_->automatic.setInterval(10000);
    connect(&d_->automatic, &QTimer::timeout, this, [this] {
        if (d_->connected && !busy() && !hasUnknown() && !d_->exactPreparation()) d_->refresh(d_->market, true);
        else emit changed();
    });
    d_->automatic.start();
}
TradingController::~TradingController() { stop(); }
void TradingController::stop() {
    ++d_->generation;
    if(d_->events)d_->events->stop();
    d_->accountScope.clear();d_->permissionKnown=d_->tradeAllowed=d_->openComplete=false;d_->openAt=-1;
    d_->preparingDraft=d_->preparingCancellation=false;d_->draft={};d_->revokePrepared();++d_->draftRevision;
    d_->pageRows.clear();d_->pageCursors.clear();d_->pageIdentities.clear();d_->firstPageAt.clear();d_->readQuality.clear();
    d_->earlyEvents.clear();d_->lastOrderTime.clear();d_->batchReviewed.clear();d_->batchRemaining.clear();
    emit intentRevoked(); ++d_->token; d_->transport->abort(); d_->active = false; d_->jobs.clear();
    d_->connected = d_->enabled = d_->tested = false; d_->snapshot = {}; d_->open.clear(); d_->closed.clear();
    d_->observedAt.clear(); d_->checks = {};
    d_->catalog.clear(); d_->catalogFailure.clear(); d_->catalogAt = -1; d_->catalogReceivedAtMs = 0; d_->catalogLoading = false;
    d_->quoteAt = d_->accountAt = d_->rulesAt = d_->serverAt = -1;
    d_->reconciled.clear(); d_->activity.clear(); d_->fingerprint.clear(); d_->account = {}; d_->adapter.reset();
    d_->market.clear(); d_->permission = "주문 권한 미검증"; d_->coverage.clear();
    d_->change("연결을 중지하고 인증 요청을 취소했습니다. 전송된 주문은 거래소에서 계속될 수 있습니다.");
}
bool TradingController::selectAccount(const QString& id) {
    if (id == d_->account.id && supported()) return true;
    if (busy()) { d_->change("진행 중인 요청을 마친 뒤 연결을 바꿔 주세요."); return false; }
    stop();
    for (const auto& account : d_->vault.accounts()) if (account.id == id) { d_->account = account; break; }
    if (AccountModel::retiredEnvironment(d_->account)) {
        d_->change("지원이 끝난 모의·테스트넷 연결입니다. 계좌·연결에서 삭제하세요."); return false;
    }
    if (d_->account.market != "spot" || !QStringList{"upbit", "binance", "okx", "bithumb"}.contains(d_->account.provider)) {
        d_->change("이 화면은 Upbit·Binance·OKX·빗썸 현물 연결을 지원합니다."); return false;
    }
    if (d_->account.provider == "upbit") d_->adapter = std::make_unique<UpbitAdapter>();
    else if(d_->isBithumb()){
        if(d_->account.environment!="live"){d_->change("빗썸은 실전 현물 연결만 지원합니다.");return false;}
        d_->adapter=std::make_unique<BithumbAdapter>();
    }else if(d_->isOkx()){
        d_->adapter=std::make_unique<OkxAdapter>(d_->account);
        if(!okxRegionValid(d_->account.region)){d_->change("OKX 지역 미확인 — 지역과 인증정보를 다시 입력해 주세요.");return false;}
    }else d_->adapter = std::make_unique<BinanceAdapter>();
    QString error;
    if (!d_->vault.withCredentials(id, [&](const CredentialSet& credentials) {
        const auto key = credentials.find(d_->account.provider == "upbit" ? "access_key" : "api_key");
        if(d_->isOkx()){
            QCryptographicHash hash(QCryptographicHash::Sha256);
            for(const auto& name:{QStringLiteral("api_key"),QStringLiteral("secret"),QStringLiteral("passphrase")}){
                const auto it=credentials.find(name);if(it==credentials.end())return;
                unsigned char length[4];qToBigEndian<quint32>(quint32(it->second.size()),length);
                hash.addData(QByteArrayView(reinterpret_cast<const char*>(length),4));
                hash.addData(QByteArrayView(reinterpret_cast<const char*>(it->second.data()),it->second.size()));
            }
            d_->fingerprint=QString::fromLatin1(hash.result().toHex());
        }else if (key != credentials.end()) d_->fingerprint = QString::fromLatin1(QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(key->second.data()), key->second.size()), QCryptographicHash::Sha256).toHex());
    }, &error) || d_->fingerprint.isEmpty()) { stop(); d_->change("인증정보를 사용할 수 없습니다."); return false; }
    d_->limits = {}; d_->halted = false;
    d_->change("연결을 선택했습니다. 종목을 입력하고 조회 연결을 확인하세요."); return true;
}
bool TradingController::refreshAssets() {
    if (!d_->adapter || busy() || !accountCredentialsAvailable(d_->vault,d_->account.id) || d_->manualReconnectRequired ||
        d_->clock.elapsed() < qMax(d_->cooldownUntil,d_->pacingUntil)) return false;
    if(d_->isOkx()&&!okxRegionValid(d_->account.region))return false;
    d_->assetsOnly=true;d_->historyOnly=false;d_->refreshFailed=false;d_->pageCount=0;
    d_->assetsTimeRetries=0;
    if(d_->account.provider=="binance"){d_->orderTimeReady=false;d_->enabled=d_->tested=false;}
    OrderInput input;
    if(d_->account.provider=="binance"||d_->isOkx())d_->jobs.enqueue({Operation::Time,input});
    if(d_->isOkx())d_->jobs.enqueue({Operation::AccountConfig,input});
    d_->jobs.enqueue({Operation::Account,input});
    if(d_->isOkx())d_->jobs.enqueue({Operation::FundingBalance,input});
    d_->next();return true;
}
bool TradingController::canReconnectAssets() const {
    return d_->adapter&&!busy()&&accountCredentialsAvailable(d_->vault,d_->account.id)&&
        d_->clock.elapsed()>=qMax(d_->cooldownUntil,d_->pacingUntil)&&(!d_->isOkx()||okxRegionValid(d_->account.region));
}
bool TradingController::reconnectAssets() {
    if(!canReconnectAssets())return false;
    d_->manualReconnectRequired=false;return refreshAssets();
}
void TradingController::setAutomaticRefresh(bool enabled) { if(enabled)d_->automatic.start();else d_->automatic.stop(); }
bool TradingController::canInterruptRead()const{
    return !((d_->active&&(isMutation(d_->current.operation)||!d_->current.journalId.isEmpty()))||
        std::any_of(d_->jobs.cbegin(),d_->jobs.cend(),[](const auto& j){return isMutation(j.operation)||!j.journalId.isEmpty();}));
}
bool TradingController::historyLoading()const{return d_->historyOnly&&busy();}
bool TradingController::interruptRead() {
    if(!canInterruptRead())return false;
    ++d_->generation;++d_->token;d_->transport->abort();d_->active=false;d_->jobs.clear();
    d_->assetsOnly=d_->historyOnly=false;d_->catalogLoading=false;d_->preparingDraft=d_->preparingCancellation=false;d_->revokePrepared();
    d_->change("조회 중단 · 이미 제출한 주문과 미확정 기록은 유지합니다.");return true;
}
AccountRecord TradingController::account() const { return d_->account; }
bool TradingController::supported() const { return bool(d_->adapter); }
bool TradingController::busy() const { return d_->active || !d_->jobs.isEmpty(); }
bool TradingController::connected() const { return d_->connected; }
bool TradingController::liveEnabled() const { return d_->enabled; }
bool TradingController::hasUnknown() const { return !d_->pending().isEmpty(); }
bool TradingController::quoteFresh() const {
    if (d_->quoteAt < 0 || d_->clock.elapsed() - d_->quoteAt > 10000) return false;
    if(d_->exactPreparation()&&!d_->fresh(Operation::Depth,10000))return false;
    if (d_->account.provider == "binance")
        for (auto op : {Operation::Reference, Operation::AveragePrice, Operation::Depth})
            if (!d_->observedAt.contains(op) || d_->clock.elapsed() - d_->observedAt.value(op) > 10000) return false;
    return true;
}
QString TradingController::status() const { return d_->message; }
QString TradingController::permissionStatus() const { return d_->permission; }
QString TradingController::coverage() const {
    if(d_->isOkx())return d_->okxCoverage();
    if(d_->isBithumb()){
        QStringList lines;for(auto op:{Operation::Account,Operation::OpenOrders,Operation::History,Operation::Rules})lines.append(operationName(op)+" · "+d_->readQuality.value(op,"미조회"));
        lines.append("미체결은 wait·watch 전체 페이지 조회이며 원자적 스냅샷이 아닙니다. watch·미지원 주문은 표시만 합니다.");
        lines.append("이력은 선택 종목 최근 7일 요청 범위입니다. TWAP·예약 취소 등 보존 예외는 포함하지 않습니다.");return lines.join('\n');
    }
    return d_->coverage;
}
QString TradingController::quoteSummary() const {
    if (d_->quoteAt < 0) return "시세 미조회";
    const auto quote = d_->snapshot.data.value(Operation::Quote);
    QString price = quote.value("lastPrice").toString();
    if (price.isEmpty()) price = quote.value("price").toString();
    if (price.isEmpty()) price = "매수 " + quote.value("bid").toString() + " / 매도 " + quote.value("ask").toString();
    return d_->market + " · " + price + " · 수신 " + QDateTime::fromMSecsSinceEpoch(d_->snapshot.quoteReceivedAtMs).toString("yyyy-MM-dd HH:mm:ss t") +
        (quoteFresh() ? " · 신선" : " · 지연") + " · " + d_->account.provider + " 공식 API";
}
QString TradingController::amendDescription() const { return d_->adapter ? d_->adapter->amendDescription() : QString{}; }
QString TradingController::symbol() const { return d_->market; }
QList<MarketInstrument> TradingController::instruments() const { return d_->catalog; }
bool TradingController::catalogFresh() const {
    return !d_->catalog.isEmpty() && d_->catalogFailure.isEmpty() && d_->catalogAt >= 0 &&
        d_->clock.elapsed() - d_->catalogAt < 5 * 60 * 1000;
}
QString TradingController::catalogStatus() const {
    if (!d_->adapter) return "연결을 선택한 뒤 공개 종목 목록을 조회하세요.";
    QString text = d_->isBithumb() ? "빗썸 공식 /v1/market/all" : d_->isOkx() ? "OKX 공식 SPOT instruments" : d_->account.provider == "upbit" ? "Upbit 공식 /v1/market/all" :
        QString("Binance Spot 공식 /api/v3/exchangeInfo");
    if (d_->catalogReceivedAtMs > 0)
        text += " · 수신 " + QDateTime::fromMSecsSinceEpoch(d_->catalogReceivedAtMs).toString("yyyy-MM-dd HH:mm:ss t") +
            " · " + QString::number(d_->catalog.size()) + "개";
    if (d_->catalogLoading) text += " · 목록 조회 중";
    if (!d_->catalogFailure.isEmpty()) text += " · 조회 실패: " + d_->catalogFailure;
    text += d_->catalog.isEmpty() ? " · 목록 없음" : catalogFresh() ? " · 유효 (5분)" : " · 만료 — 다시 조회 후 선택";
    return text + " · 계좌 인증·주문 권한은 별도 확인";
}
QList<Balance> TradingController::balances() const { return d_->snapshot.balances; }
QList<OrderRecord> TradingController::openOrders() const { return d_->open; }
QList<OrderRecord> TradingController::history() const { return d_->closed; }
QList<OrderRecord> TradingController::pendingOrders() const {
    QList<OrderRecord> result;
    for (const auto& value : d_->pending()) {
        const auto entry = value.toObject(); OrderRecord order;
        order.orderId = entry.value("orderId").toString(); order.clientId = entry.value("clientId").toString();
        order.symbol = entry.value("symbol").toString(); order.type = entry.value("kind").toString();
        order.status = entry.value("state").toString(); order.quantity = entry.value("quantity").toString();
        if(d_->exactPreparation()){order.journalId=entry.value("id").toString();order.quantityUnit=d_->isOkx()?entry.value("quantityUnit").toString():!entry.value("quoteAmount").toString().isEmpty()?QString("quote"):QString("base");
            order.requestedQuoteAmount=entry.value("quoteAmount").toString();order.amendRequestId=entry.value("amendRequestId").toString();
            order.requestState=entry.value("ackState").toString();order.actionOutcome="unconfirmed";}
        result.append(order);
    }
    return result;
}
OrderLimits TradingController::limits() const { return d_->limits; }
QJsonObject TradingController::verificationReceipt() const {
    return {{"schema_version", 1}, {"application", "KEEFI"}, {"stage", d_->isOkx()?"G3-OKX":d_->isBithumb()?"G3-BITHUMB":"G2"},
        {"version", QCoreApplication::applicationVersion()},
        {"transport", dynamic_cast<NetworkTradingTransport*>(d_->transport) ? "official_https" : "synthetic_test"},
        {"provider", d_->account.provider}, {"environment", d_->account.environment},
        {"exported_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"checks", d_->checks}, {"pending_operation_count", d_->pending().size()},
        {"contains_keys_account_ids_order_ids_or_amounts", false},
        {"scope_note", "현재 실행의 관측 기록입니다. 주문 테스트 성공은 체결 증거가 아닙니다."}};
}
OrderValidation TradingController::preview(const OrderInput& input) const {
    if (!d_->adapter || !d_->connected) return {false, "조회 연결이 필요합니다.", {}, {}};
    if (input.symbol != d_->market) return {false, "조회한 종목과 주문 종목이 다릅니다.", {}, {}};
    if (!quoteFresh()) return {false, "시세가 오래되었습니다. 다시 조회해 주세요.", {}, {}};
    auto snapshot=d_->snapshot;if(d_->isOkx())snapshot.serverNowMs=d_->serverNow();
    auto request=input;
    if(d_->account.provider=="binance"&&!request.orderId.isEmpty())for(const auto& o:d_->open)if(o.orderId==request.orderId&&o.symbol==request.symbol)
        request.cancelReplace=decimal::valid(request.price)&&decimal::valid(o.price)&&decimal::valid(request.quantity)&&decimal::valid(o.quantity)&&(decimal::compare(request.price,o.price)!=0||decimal::compare(request.quantity,o.quantity)>=0);
    return d_->adapter->validate(request, snapshot, d_->limits);
}
bool TradingController::setLimits(const OrderLimits& limits) {
    if (!decimal::positive(limits.maxNotional) || !decimal::positive(limits.maxDeviationPercent) ||
        decimal::compare(limits.maxDeviationPercent, "100") > 0 ||
        !(d_->account.provider == "binance" ? binanceIdentifierValid(limits.currency) :
            QRegularExpression("^[A-Z0-9]{2,12}$").match(limits.currency).hasMatch())) {
        d_->change("최대 주문 금액·결제 통화·0 초과 100 이하의 괴리율을 직접 설정해 주세요."); return false;
    }
    d_->limits = limits; d_->enabled = false;if(d_->exactPreparation())d_->revokePrepared(); d_->change("주문 한도를 적용했습니다. 실제 주문 활성화는 별도 선택입니다."); return true;
}

void TradingController::refresh(const QString& symbol,bool includeHistory) { if(busy()&&!interruptRead())return;d_->refresh(symbol,false,includeHistory); }
void TradingController::loadMarkets() {
    if (!d_->adapter || d_->catalogLoading) return;
    if(busy()&&!interruptRead())return;
    if (d_->manualReconnectRequired || d_->clock.elapsed() < d_->cooldownUntil) {
        d_->catalogFailure = "거래소 요청 제한 대기 중입니다. 차단 상태는 공식 거래소에서 확인하세요.";
        d_->change(); return;
    }
    d_->catalogLoading = true;
    d_->jobs.enqueue({Operation::Markets, {}, {}, false, true});
    d_->next();
}

void TradingController::reconcile(QWidget* origin) {
    if (!d_->ready(true, origin, true, false)) return;
    d_->reconciled.clear();
    for (const auto& value : d_->pending()) d_->enqueueReconciliation(value.toObject());
    if (d_->jobs.isEmpty()) { d_->change(hasUnknown() ? "다른 API 키의 미확정 기록입니다. 원래 키를 등록한 연결에서 대사해 주세요. 다른 계좌 키로 대신 확인하지 않습니다." : "대사할 미확정 주문이 없습니다."); return; }
    d_->next();
}
ExchangePolicy TradingController::policy() const{return d_->adapter?d_->adapter->exchangePolicy():ExchangePolicy{};}
void TradingController::setDraft(const OrderInput& input){
    if(d_->draft==input)return;
    d_->draft=input;++d_->draftRevision;d_->revokePrepared();
    if(d_->exactPreparation())emit changed();
}
TradingReadiness TradingController::requestReadiness() const{return d_->requestReady();}
TradingReadiness TradingController::draftReadiness() const{
    if(d_->exactPreparation())return d_->draftReady();
    const auto v=preview(d_->draft);return {v.ok,v.ok?"ready":"validation",v.message};
}
TradingReadiness TradingController::cancellationReadiness(const OrderRecord& order) const{
    if(d_->exactPreparation())return d_->cancelReady(order);
    return {connected()&&!busy()&&order.cancelable,"legacy",order.cancelable?"취소 가능":"이 유형은 취소를 지원하지 않습니다."};
}
bool TradingController::cancelBlocked(const OrderRecord& order) const{
    if(d_->exactPreparation())return d_->pendingTarget(order);
    for(const auto& value:d_->pending()){const auto entry=value.toObject();if(entry.value("kind")=="place"||entry.value("symbol")!=order.symbol)continue;
        if(entry.value("orderId")==order.orderId||(!order.orderListId.isEmpty()&&entry.value("orderListId")==order.orderListId))return true;
    }return false;
}
bool TradingController::prepareDraft(){
    if(!d_->exactPreparation())return false;
    const auto ready=d_->requestReady();if(!ready.ready){d_->change(ready.message);return false;}
    if(!d_->connected||d_->draft.symbol!=d_->market||(d_->isOkx()&&!okxInstrumentValid(d_->draft.symbol))){d_->change("종목과 조회 연결을 먼저 확인해 주세요.");return false;}
    d_->revokePrepared();d_->preparingDraft=true;d_->preparingCancellation=false;d_->preparingRevision=d_->draftRevision;d_->refreshFailed=false;d_->pageCount=0;
    d_->pageRows.clear();d_->pageCursors.clear();d_->pageIdentities.clear();d_->firstPageAt.clear();
    if(d_->isBithumb()){
        if(!d_->draft.orderId.isEmpty()){d_->preparingDraft=false;d_->change("빗썸 정정은 지원하지 않습니다.");return false;}
        for(auto op:{Operation::Rules,Operation::Account,Operation::Quote,Operation::Depth}){d_->observedAt.remove(op);d_->jobs.enqueue({op,d_->draft});}
        d_->next();return true;
    }
    QList<Operation> ops{Operation::Time,Operation::AccountConfig};
    if(!d_->draft.orderId.isEmpty())ops.append(Operation::OpenOrders);
    ops.append({Operation::Rules,Operation::Commissions,Operation::FxUsd,Operation::FxUsdt});
    if(d_->draft.type=="limit")ops.append(Operation::PriceLimits);
    ops.append({Operation::Account,Operation::BuyingPower,Operation::Quote,Operation::Depth});
    for(auto op:ops){d_->observedAt.remove(op);d_->jobs.enqueue({op,d_->draft});}
    d_->next();return true;
}
bool TradingController::prepareCancellation(){
    if(historyLoading()&&!interruptRead())return false;
    if(!d_->exactPreparation())return false;
    const auto ready=d_->requestReady();if(!ready.ready){d_->change(ready.message);return false;}
    if(!d_->connected){d_->change("먼저 연결·조회해 주세요.");return false;}
    d_->revokePrepared();d_->preparingDraft=false;d_->preparingCancellation=true;d_->refreshFailed=false;d_->pageCount=0;d_->openComplete=false;
    d_->pageRows.clear();d_->pageCursors.clear();d_->pageIdentities.clear();d_->firstPageAt.clear();
    OrderInput in;in.symbol=d_->market;
    for(auto op:d_->isBithumb()?QList<Operation>{Operation::OpenOrders}:QList<Operation>{Operation::Time,Operation::AccountConfig,Operation::OpenOrders})d_->jobs.enqueue({op,in});
    d_->next();return true;
}

}

namespace keefi {
bool TradingController::assetReadComplete() const {
    return !busy() && accountCredentialsAvailable(d_->vault,d_->account.id) && d_->accountAt>=0 &&
        d_->observedAt.contains(Operation::Account) && d_->receivedWall.value(Operation::Account)>0 &&
        (!d_->accountScope.isEmpty() || !d_->fingerprint.isEmpty()) &&
        (!d_->isOkx() || d_->observedAt.contains(Operation::FundingBalance));
}
AssetCapture TradingController::assetCapture() const {
    if(busy()||!accountCredentialsAvailable(d_->vault,d_->account.id)||d_->accountAt<0||!d_->observedAt.contains(Operation::Account))return {};
    const bool complete=d_->accountAt>=0&&(!d_->isOkx()||d_->observedAt.contains(Operation::FundingBalance));
    return captureExchangeAssets(d_->account,d_->accountScope.isEmpty()?d_->fingerprint:d_->accountScope,d_->snapshot,
        d_->receivedWall.value(Operation::Account),d_->receivedWall.value(Operation::History),complete,!hasUnknown(),coverage(),d_->closed);
}
}
