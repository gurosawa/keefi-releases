#include "keefi/BuildPolicy.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/BinanceUsdmAdapter.h"
#include "keefi/BinanceTiming.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <algorithm>

namespace keefi {
namespace {
void wipe(QByteArray& bytes) { bytes.fill('\0'); bytes.clear(); }
bool direct(QWidget* origin) {
    return origin && origin->isEnabled() && origin->isVisible() && origin->hasFocus() &&
        QApplication::applicationState()==Qt::ApplicationActive && origin->window()==QApplication::activeWindow();
}
bool publicOperation(Operation op) {
    return QList<Operation>{Operation::Time,Operation::Markets,Operation::Rules,Operation::Quote,Operation::Depth,Operation::MarkPrice}.contains(op);
}
bool conditional(const OrderRecord& o) { return o.futures && o.futures->family==OrderFamily::Conditional; }
QString rowKey(const OrderRecord& o) { return (conditional(o)?"algo:":"order:")+o.symbol+":"+(conditional(o)?o.algoId:o.orderId); }
bool amountEqual(const QString& a,const QString& b) { return a.isEmpty()||b.isEmpty()?a==b:decimal::valid(a)&&decimal::valid(b)&&decimal::compare(a,b)==0; }
FuturesOrderTerms termsFrom(const QJsonObject& j,OrderFamily family) {
    FuturesOrderTerms t;t.family=family;t.positionSide=j.value("positionSide").toString();
    t.exposure=j.value("exposure")=="open"?ExposureIntent::Open:j.value("exposure")=="reduce"?ExposureIntent::Reduce:ExposureIntent::Unspecified;
    t.reduceOnly=j.value("reduceOnly").toBool();t.closePosition=j.value("closePosition").toBool();
    t.timeInForce=j.value("timeInForce").toString();t.workingType=j.value("workingType").toString();
    t.triggerPrice=j.value("triggerPrice").toString();t.activationPrice=j.value("activationPrice").toString();
    t.callbackRate=j.value("callbackRate").toString();t.modifyId=j.value("modifyId").toString();
    if(j.value("priceProtect").isBool())t.priceProtect=j.value("priceProtect").toBool();return t;
}
OrderInput inputFrom(const QJsonObject& j) {
    OrderInput i;i.symbol=j.value("symbol").toString();i.side=j.value("side").toString();i.type=j.value("type").toString();
    i.price=j.value("price").toString();i.quantity=j.value("quantity").toString();i.orderId=j.value("orderId").toString();i.clientId=j.value("clientId").toString();
    i.algoId=j.value("algoId").toString();i.clientAlgoId=j.value("clientAlgoId").toString();i.actualOrderId=j.value("actualOrderId").toString();
    i.futures=termsFrom(j.value("futures").toObject(),j.value("orderFamily")=="conditional"?OrderFamily::Conditional:OrderFamily::Ordinary);return i;
}
OrderInput inputFrom(const OrderRecord& o) {
    OrderInput i;i.symbol=o.symbol;i.side=o.side;i.type=o.type;i.price=o.price;i.quantity=o.quantity;i.orderId=o.orderId;i.clientId=o.clientId;
    i.futures=o.futures;i.algoId=o.algoId;i.clientAlgoId=o.clientAlgoId;i.actualOrderId=o.actualOrderId;return i;
}
bool observedTerms(const OrderInput& i,const OrderRecord& o) {
    if(!i.futures||!o.futures||i.futures->family!=o.futures->family||i.symbol!=o.symbol||i.side!=o.side||i.type!=o.type)return false;
    if(i.futures->family==OrderFamily::Conditional) {
        if((i.algoId.isEmpty()&&i.clientAlgoId.isEmpty())||(!i.algoId.isEmpty()&&i.algoId!=o.algoId)||(!i.clientAlgoId.isEmpty()&&i.clientAlgoId!=o.clientAlgoId))return false;
    }else if((!i.orderId.isEmpty()&&i.orderId!=o.orderId)||(!i.clientId.isEmpty()&&i.clientId!=o.clientId))return false;
    const auto& a=*i.futures;const auto& b=*o.futures;
    if(a.positionSide!=b.positionSide||a.reduceOnly!=b.reduceOnly||a.closePosition!=b.closePosition||a.timeInForce!=b.timeInForce)return false;
    if(!amountEqual(i.quantity,o.quantity)||(QStringList{"limit","stop","take_profit"}.contains(i.type)&&!amountEqual(i.price,o.price)))return false;
    if(a.family==OrderFamily::Conditional && (a.workingType!=b.workingType||!amountEqual(a.triggerPrice,b.triggerPrice)||
        !amountEqual(a.activationPrice,b.activationPrice)||!amountEqual(a.callbackRate,b.callbackRate)||
        (a.priceProtect.has_value()&&a.priceProtect!=b.priceProtect)))return false;
    return true;
}
}
struct FuturesTradingController::Private {
    enum class Phase { Idle, Catalog, Refresh, Assets, History, Prepare, CancelPrepare, Action, Reconcile, Test };
    struct Job { Operation op; OrderInput input; QString journalId; };
    FuturesTradingController* q;Vault& vault;TradingTransport* transport;std::unique_ptr<BinanceUsdmAdapter> adapter;
    AccountRecord account;QElapsedTimer clock;QTimer timer;QQueue<Job> jobs;Job current{Operation::Time,{},{}};Phase phase=Phase::Idle;
    bool active=false,connected=false,enabled=false,halted=false,reconnect=false,openComplete=false,prepareFailed=false;
    bool orderTimeReady=false;int assetsTimeRetries=0;QString assetsFailure;
    std::function<qint64()> timingClock;
    qint64 timingNow()const{return timingClock?timingClock():clock.elapsed();}
    quint64 token=0,revision=1,preparedRevision=0,testedRevision=0; qint64 pacingUntil=0,cooldownUntil=0,serverAt=-1,serverAnchor=0,sentAt=0,catalogAt=-1,openAt=-1;
    QString fingerprint,symbol,message="선물 연결을 선택해 주세요.",permission="선물 주문 권한 미검증",catalogMessage,readinessCode;
    OrderLimits limits;OrderInput draft,prepared;OrderRecord preparedTarget;MarketSnapshot snapshot;
    QList<MarketInstrument> catalog;QList<OrderRecord> open,closed,batchReviewed,batchRemaining;QMap<QString,OrderRecord> activity;
    QMap<Operation,qint64> observed,firstPage,receivedWall;QMap<Operation,QString> quality;QMap<Operation,QList<OrderRecord>> staging;
    QMap<Operation,QSet<QString>> identities,cursors;QSet<Operation> completedReads;int pages=0;
    Private(FuturesTradingController* owner,Vault& v,TradingTransport* t):q(owner),vault(v),transport(t){clock.start();}
    void change(const QString& m={}) { if(!m.isEmpty())message=m;emit q->changed(); }
    qint64 now() const { return serverAt<0?0:serverAnchor+clock.elapsed()-serverAt; }
    bool fresh(Operation op,qint64 age) const { return observed.contains(op)&&clock.elapsed()-observed.value(op)<=age; }
    bool matches(const QJsonObject& j) const { return j.value("provider")=="binance"&&j.value("market")=="usdm_futures"&&j.value("environment")==account.environment &&
        (j.value("accountId")==account.id || (!fingerprint.isEmpty()&&j.value("fingerprint")==fingerprint)); }
    bool owns(const QJsonObject& j) const { return matches(j)&&!fingerprint.isEmpty()&&j.value("fingerprint")==fingerprint; }
    QJsonArray pending() const { QJsonArray result;for(const auto& v:vault.pendingOperations())if(matches(v.toObject()))result.append(v);return result; }
    QJsonObject journal(const QString& id) const { for(const auto& v:vault.pendingOperations())if(v.toObject().value("id")==id)return v.toObject();return {}; }
    bool save(const QJsonObject& j,bool remove=false) {
        auto all=vault.pendingOperations();int index=-1;for(int n=0;n<all.size();++n)if(all[n].toObject().value("id")==j.value("id")){index=n;break;}
        if(index>=0){if(remove)all.removeAt(index);else all[index]=j;}else if(!remove)all.append(j);
        QString error;if(!vault.setPendingOperations(all,&error)){enabled=false;batchRemaining.clear();jobs.clear();change(error);return false;}return true;
    }
    void revoke(bool clearInput=false) { preparedRevision=testedRevision=0;prepared={};preparedTarget={};if(clearInput)emit q->intentRevoked(); }
    TradingReadiness requestReady() const {
        if(!adapter)return {false,"account","선물 연결을 선택해 주세요."};
        if(active||!jobs.isEmpty())return {false,"busy","조회·요청 처리 중입니다."};
        if(clock.elapsed()<pacingUntil||clock.elapsed()<cooldownUntil)return {false,"pacing","요청 간격·제한 대기 중입니다."};
        if(reconnect)return {false,"reconnect","공식 앱 확인 후 다시 연결해 주세요."};
        return {true,"ready","요청 가능"};
    }
    bool permitted() const {
        return snapshot.data.value(Operation::AccountConfig).value("canTrade")==QJsonValue(true) &&
            snapshot.data.value(Operation::ApiRestrictions).value("enableFutures")==QJsonValue(true);
    }
    TradingReadiness draftReady(bool sending=false) const {
        if(!sending){auto r=requestReady();if(!r.ready)return r;}
        if(!connected)return {false,"connection","선물 계좌를 연결·조회해 주세요."};
        if(!vault.isUnlocked())return {false,"vault","금고를 열어 주세요."};
        if(!draft.futures)return {false,"intent","선물 주문 의도를 선택해 주세요."};
        if(halted&&draft.futures->exposure!=ExposureIntent::Reduce)return {false,"halted","신규 진입이 중지되었습니다."};
        if(!pending().isEmpty())return {false,"pending","미확정 선물 주문을 먼저 대사해 주세요."};
        if(!preparedRevision||preparedRevision!=revision||!(draft==prepared))return {false,"prepare","현재 입력으로 선물 주문 조건을 조회해 주세요."};
        if(!permitted())return {false,"permission","현재 선물 거래 권한을 확인하지 못했습니다."};
        if(!orderTimeReady||serverAt<0||clock.elapsed()-serverAt>60000)return {false,"time","서버 시간을 다시 조회해 주세요."};
        for(auto op:{Operation::AccountConfig,Operation::Rules,Operation::SymbolConfig,Operation::LeverageBrackets,Operation::Commissions,Operation::TradingStatus})
            if(!fresh(op,60000))return {false,"rules","계정·거래 조건이 만료되었습니다."};
        if(account.environment=="live"&&!fresh(Operation::ApiRestrictions,60000))return {false,"permission_stale","API 선물 권한을 다시 확인해 주세요."};
        for(auto op:{Operation::Account,Operation::Positions})if(!fresh(op,15000))return {false,"positions","잔고·포지션을 다시 조회해 주세요."};
        for(auto op:{Operation::Quote,Operation::Depth,Operation::MarkPrice}){
            if(!fresh(op,10000))return {false,"market","가격·호가가 만료되었습니다."};
            const auto d=snapshot.data.value(op);bool ok=false;const auto stamp=d.value(op==Operation::MarkPrice?"time":"timestamp").toString().toLongLong(&ok);
            if(!ok||stamp<=0||now()-stamp>10000||stamp-now()>2000)return {false,"source_time","거래소 가격·호가 시각을 확인할 수 없습니다."};
        }
        if(!openComplete||openAt<0||clock.elapsed()-openAt>10000)return {false,"open_orders","일반·조건부 미체결을 다시 조회해 주세요."};
        if(!draft.orderId.isEmpty()) {
            if(!preparedTarget.amendable||!sameOpen(preparedTarget)||!preparedTarget.futures||
                !decimal::valid(preparedTarget.executed)||!decimal::positive(draft.quantity)||
                decimal::compare(draft.quantity,preparedTarget.executed)<=0)return {false,"target","원주문·체결 수량이 변경되었습니다."};
            auto before=*preparedTarget.futures,after=*draft.futures;before.modifyId.clear();after.modifyId.clear();
            if(!(before==after)||draft.side!=preparedTarget.side||draft.symbol!=preparedTarget.symbol||draft.type!="limit")return {false,"amend_terms","정정은 원주문 속성을 유지한 가격·전체 수량 변경만 가능합니다."};
        }
        auto current=snapshot;current.serverNowMs=now();const auto check=adapter->validate(draft,current,limits);
        return {check.ok,check.ok?"ready":"validation",check.ok?"입력 준비 완료":check.message};
    }
    bool sameOpen(const OrderRecord& order) const { return std::count(open.cbegin(),open.cend(),order)==1; }
    bool targetPending(const OrderRecord& o) const {
        for(const auto& v:pending()){const auto j=v.toObject();if(!owns(j)||j.value("kind")=="place")continue;
            const auto i=inputFrom(j);if(i.symbol==o.symbol&&i.futures&&o.futures&&i.futures->family==o.futures->family&&
                (conditional(o)?i.algoId==o.algoId:i.orderId==o.orderId))return true;}return false;
    }
    TradingReadiness cancelReady(const OrderRecord& o,bool sending=false) const {
        if(!sending){const auto r=requestReady();if(!r.ready)return r;}
        if(!connected||!vault.isUnlocked()||!permitted())return {false,"permission","금고·현재 선물 거래 권한 확인이 필요합니다."};
        if(!fresh(Operation::AccountConfig,60000)||(account.environment=="live"&&!fresh(Operation::ApiRestrictions,60000))||
            !orderTimeReady||serverAt<0||clock.elapsed()-serverAt>60000)return {false,"permission_stale","취소 조건을 다시 조회해 주세요."};
        if(!openComplete||openAt<0||clock.elapsed()-openAt>10000)return {false,"orders_stale","전체 미체결 범위를 다시 조회해 주세요."};
        if(!o.cancelable||!o.futures||!sameOpen(o))return {false,"target_changed","현재 취소 가능한 주문을 다시 선택해 주세요."};
        if(targetPending(o))return {false,"pending_target","이 주문의 이전 취소·정정 결과가 미확정입니다."};
        return {true,"ready","취소 준비 완료"};
    }
    OrderInput readInput() const { OrderInput i;i.symbol=symbol;i.endTimeMs=QDateTime::currentMSecsSinceEpoch();i.startTimeMs=i.endTimeMs-6LL*86400000;return i; }
    void enqueue(Operation op,OrderInput input={},QString id={}) {jobs.enqueue({op,std::move(input),std::move(id)});}
    void begin(Phase p) { phase=p;prepareFailed=false;staging.clear();identities.clear();cursors.clear();firstPage.clear();completedReads.clear();pages=0; }
    void addReadPlan(const QList<Operation>& plan) {const auto i=readInput();for(auto op:plan)enqueue(op,i);}
    void schedule() {
        if(jobs.isEmpty()){finish();return;}
        const auto delay=qMax<qint64>(0,qMax(pacingUntil,cooldownUntil)-clock.elapsed());
        QTimer::singleShot(int(qMin<qint64>(delay,60000)),q,[this]{next();});
    }

    void next() {
        if(active||jobs.isEmpty()||!adapter)return;
        if(clock.elapsed()<pacingUntil||clock.elapsed()<cooldownUntil){schedule();return;}
        if(reconnect){jobs.clear();batchRemaining.clear();return;}
        current=jobs.dequeue();

        RequestSpec request;QString error;
        try {
            if(publicOperation(current.op)){CredentialSet empty;request=adapter->build(current.op,account,current.input,empty,now(),&error);}
            else if(!vault.withCredentials(account.id,[&](const CredentialSet& keys){request=adapter->build(current.op,account,current.input,keys,now(),&error);},&error))error="인증정보를 사용할 수 없습니다.";
        } catch(const std::exception&) {error="요청 구성에 실패했습니다.";}
        if(!error.isEmpty()||request.url.isEmpty()){jobs.clear();prepareFailed=true;revoke();change(error.isEmpty()?"요청 구성을 거부했습니다.":error);return;}

        active=true;sentAt=timingNow();++token;transport->send(token,request);
        wipe(request.body);for(auto& h:request.headers)wipe(h);change(operationName(current.op)+" 요청 중");
    }
    bool catalogue(const QJsonObject& data) {
        const auto raw=data.value("symbols");if(!raw.isArray())return false;
        QList<MarketInstrument> next;QSet<QString> seen;
        for(const auto& value:raw.toArray()){
            if(!value.isObject())return false;const auto j=value.toObject();MarketInstrument m;
            m.symbol=j.value("symbol").toString();m.name=m.symbol;m.baseAsset=j.value("baseAsset").toString();m.quoteAsset=j.value("quoteAsset").toString();
            m.status=j.value("status").toString();m.market="usdm_futures";m.securityType=j.value("underlyingType").toString();
            if(!binanceUsdmSymbolValid(m.symbol)||seen.contains(m.symbol))return false;seen.insert(m.symbol);
            m.tradable=m.status=="TRADING"&&m.securityType=="COIN"&&QStringList{"USDT","USDC"}.contains(m.quoteAsset)&&j.value("marginAsset")==m.quoteAsset;
            if(!m.tradable)m.warning="조회할 수 있는 시장 정보를 확인해 주세요.";next.append(m);
        }
        catalog=next;catalogAt=clock.elapsed();catalogMessage=QString("공식 선물 종목 %1개 · 수신 %2").arg(catalog.size()).arg(QDateTime::currentDateTime().toString("HH:mm:ss"));return true;
    }
    bool page(const Job& job,const AdapterResult& r) {
        const auto op=job.op;if(!firstPage.contains(op))firstPage[op]=clock.elapsed();
        for(const auto& row:r.orders){const auto key=rowKey(row);if(identities[op].contains(key))return false;identities[op].insert(key);staging[op].append(row);}
        if(staging[op].size()>20000||++pages>100)return false;
        if(r.hasMore){if(r.nextCursor.isEmpty()||cursors[op].contains(r.nextCursor)||r.orders.isEmpty())return false;
            cursors[op].insert(r.nextCursor);auto continuation=job;continuation.input.cursor=r.nextCursor;jobs.prepend(continuation);return true;}
        completedReads.insert(op);
        if(op==Operation::OpenOrders||op==Operation::AlgoOpenOrders){
            if(completedReads.contains(Operation::OpenOrders)&&completedReads.contains(Operation::AlgoOpenOrders)){
                open=staging[Operation::OpenOrders]+staging[Operation::AlgoOpenOrders];snapshot.openOrders=open;openComplete=true;
                openAt=qMin(firstPage.value(Operation::OpenOrders),firstPage.value(Operation::AlgoOpenOrders));
                if(phase==Phase::Prepare&&!draft.orderId.isEmpty()){
                    const auto found=std::find_if(open.cbegin(),open.cend(),[&](const auto&o){return !conditional(o)&&o.orderId==draft.orderId&&o.symbol==draft.symbol;});
                    preparedTarget=found==open.cend()?OrderRecord{}:*found;
                }
            }
        }else if(completedReads.contains(Operation::History)&&completedReads.contains(Operation::AlgoHistory))closed=staging[Operation::History]+staging[Operation::AlgoHistory];
        return true;
    }
    void queryPending(const QJsonObject& j) {
        auto i=inputFrom(j);i.startTimeMs=j.value("createdAt").toString().toLongLong()-2000;i.endTimeMs=now();
        const auto op=j.value("kind")=="amend"?Operation::AmendHistory:i.futures->family==OrderFamily::Conditional?Operation::AlgoQueryOrder:Operation::QueryOrder;
        enqueue(op,i,j.value("id").toString());
    }
    void observedAction(const QString& id,const QJsonObject& j,const OrderRecord& row,const QString& outcome,bool remove) {
        auto r=row;r.journalId=id;r.requestState="acknowledged";r.actionOutcome=outcome;activity[id]=r;
        if(!save(j,remove))return;
        if(remove){open.erase(std::remove_if(open.begin(),open.end(),[&](const auto& o){return rowKey(o)==rowKey(row);}),open.end());
            if(row.status=="open"||row.status=="partial"||row.status=="watch")open.append(row);snapshot.openOrders=open;}
    }
    void reconcileResult(const Job& job,const AdapterResult& r) {
        const auto j=journal(job.journalId);if(j.isEmpty()||!owns(j))return;
        if(job.op==Operation::AmendHistory&&r.ok){
            const auto i=inputFrom(j);const auto before=j.value("before").toObject();int matchesId=0;bool exact=false;
            for(const auto& value:r.data.value("rows").toArray()){
                const auto row=value.toObject(),a=row.value("amendment").toObject();
                if(row.value("orderId")!=i.orderId||row.value("symbol")!=i.symbol||a.value("modifyId")!=i.futures->modifyId)continue;
                ++matchesId;const auto px=a.value("price").toObject(),qty=a.value("origQty").toObject();
                bool ok=false;const auto stamp=row.value("time").toString().toLongLong(&ok);
                exact=ok&&stamp>=j.value("createdAt").toString().toLongLong()-2000&&stamp>=before.value("updatedAt").toString().toLongLong()&&
                    amountEqual(px.value("before").toString(),before.value("price").toString())&&amountEqual(qty.value("before").toString(),before.value("quantity").toString())&&
                    amountEqual(px.value("after").toString(),i.price)&&amountEqual(qty.value("after").toString(),i.quantity);
            }
            if(matchesId==1&&exact){auto row=activity.value(job.journalId);row.orderId=i.orderId;row.symbol=i.symbol;row.price=i.price;row.quantity=i.quantity;row.futures=i.futures;
                row.actionOutcome="amended";row.requestState="confirmed";activity[job.journalId]=row;save(j,true);change("정정 이력의 요청 ID·전후 조건으로 적용을 확인했습니다.");return;}
        }else if(r.ok&&r.orders.size()==1){
            const auto row=r.orders.first();const auto i=inputFrom(j);
            if(observedTerms(i,row)){
                const auto kind=j.value("kind").toString();
                if(kind=="place"&&conditional(row)&&QStringList{"NEW","CANCELED","REJECTED","EXPIRED","TRIGGERED","FINISHED"}.contains(row.rawStatus)){
                    observedAction(job.journalId,j,row,"observed_conditional",true);change("조건부 주문 상태를 확인했습니다. 발동·종료는 일반 주문 체결 확인과 다릅니다.");return;}
                if(kind=="place"&&!conditional(row)&&QStringList{"open","partial","filled","canceled","rejected","expired"}.contains(row.status)){
                    observedAction(job.journalId,j,row,"observed_order",true);change("제출 주문의 실제 상태를 확인했습니다.");return;}
                if(kind=="cancel"&&row.status=="canceled"){
                    observedAction(job.journalId,j,row,"canceled",true);change("주문 취소를 확인했습니다.");return;}
                observedAction(job.journalId,j,row,"terminal_without_action_proof",false);
            }
        }
        batchRemaining.clear();change("요청 결과가 미확정입니다. 재전송하지 않고 기존 ID로 대사합니다.");
    }
    void finish() {
        if(active||!jobs.isEmpty())return;
        const auto done=phase;phase=Phase::Idle;
        if(done==Phase::Refresh){
            connected=completedReads.contains(Operation::AccountConfig)&&completedReads.contains(Operation::Account)&&completedReads.contains(Operation::Positions)&&openComplete;
            change(connected?"선물 잔고·포지션 조회를 마쳤어요.":"선물 조회가 일부 불완전합니다. 표시된 조회 범위를 확인해 주세요.");
        }else if(done==Phase::Assets){
            change(!assetsFailure.isEmpty()?assetsFailure:completedReads.contains(Operation::Account)&&completedReads.contains(Operation::Positions)?"선물 잔고·포지션 조회를 마쳤어요.":"선물 자산 조회 범위가 불완전합니다. 다시 조회해 주세요.");
        }else if(done==Phase::History){change("선택 종목의 일반·조건부 이력 조회 완료 · 계좌 자산과 별도 조회");
        }else if(done==Phase::Prepare){
            if(!prepareFailed){prepared=draft;preparedRevision=revision;}else revoke();
            change(prepareFailed?"선물 조건 조회가 불완전합니다.":q->orderTestRequired()?"현재 입력의 선물 조건을 조회했습니다. 일반 신규는 공식 주문 테스트 후 직접 전송합니다.":"조건부 지정가 조건을 조회했습니다. 공식 비체결 테스트 없이 직접 전송하며 발동·체결은 보장하지 않습니다.");
        }else if(done==Phase::CancelPrepare)change("일반·조건부 미체결과 취소 권한을 다시 조회했습니다.");

    }
    void result(quint64 reply,const HttpResult& response) {
        if(!active||reply!=token||!adapter)return;active=false;pacingUntil=qMax(pacingUntil,clock.elapsed()+300);
        const auto job=current;auto r=response.localRetryAfterSeconds>0?localRateLimitFailure(response):adapter->parse(job.op,response,job.input);
        if(r.rateLimited||response.status==429||response.status==418){enabled=false;connected=false;batchRemaining.clear();
            if(r.retryAfterSeconds>0)cooldownUntil=clock.elapsed()+qint64(r.retryAfterSeconds)*1000;else reconnect=true;}
        if(r.authenticationFailed){enabled=connected=false;reconnect=true;revoke(true);batchRemaining.clear();}

        if(!job.journalId.isEmpty()){reconcileResult(job,r);schedule();return;}

        if(job.op==Operation::Time&&phase==Phase::Assets){
            const auto rtt=timingNow()-sentAt;serverAt=-1;orderTimeReady=false;
            if(r.ok&&r.serverTimeMs>0&&binanceReadTimeUsable(rtt)){serverAt=clock.elapsed();serverAnchor=r.serverTimeMs;}
            else{
                r.message=binanceAssetReadFailure(response,r,rtt);
                if(!r.authenticationFailed&&!r.rateLimited&&response.status!=429&&response.status!=418&&assetsTimeRetries++==0){jobs.prepend(job);change(QString("서버 시간 측정 재시도 · 왕복 %1 ms").arg(rtt));schedule();return;}
                r.ok=false;
            }
        }else if(job.op==Operation::Time&&r.ok){const auto rtt=timingNow()-sentAt;orderTimeReady=false;serverAt=-1;
            if(r.serverTimeMs<=0||rtt>BinanceOrderMaxRttMs){r.ok=false;r.message="서버 시간 지연이 큽니다.";}
            else{serverAt=clock.elapsed();serverAnchor=r.serverTimeMs+rtt/2;orderTimeReady=true;}}
        if((job.op==Operation::Markets||job.op==Operation::Rules)&&r.ok&&!catalogue(r.data)){r.ok=false;r.message="공식 선물 종목 목록을 해석하지 못했습니다.";}
        if(!r.ok){quality[job.op]="조회 실패";
            if(phase==Phase::Assets&&job.op!=Operation::Time)r.message=binanceAssetReadFailure(response,r,timingNow()-sentAt);
            if(job.op==Operation::Markets)catalogMessage="공식 선물 종목 목록 조회 실패";
            if(job.op==Operation::OpenOrders||job.op==Operation::AlgoOpenOrders)openComplete=false;
            if(phase==Phase::Prepare){prepareFailed=true;revoke();jobs.clear();}
            if(job.op==Operation::Time||r.authenticationFailed||r.rateLimited){jobs.clear();batchRemaining.clear();enabled=false;}
            const auto failure=operationName(job.op)+" — "+(r.message.isEmpty()?"응답 확인 실패":r.message);
            if(phase==Phase::Assets)assetsFailure=failure+(failure.contains("다시 조회")?QString{}:QStringLiteral(" · 다시 조회해 주세요."));
            change(failure);schedule();return;
        }
        snapshot.data[job.op]=r.data;observed[job.op]=clock.elapsed();receivedWall[job.op]=QDateTime::currentMSecsSinceEpoch();quality[job.op]="조회 완료";completedReads.insert(job.op);
        if(job.op==Operation::Account)snapshot.balances=r.balances;
        if(job.op==Operation::Positions)snapshot.positions=r.positions;
        if(job.op==Operation::Quote)snapshot.quoteReceivedAtMs=QDateTime::currentMSecsSinceEpoch();
        if(job.op==Operation::ApiRestrictions||job.op==Operation::AccountConfig){
            const bool denied=snapshot.data.value(Operation::AccountConfig).value("canTrade")==QJsonValue(false)||
                (account.environment=="live"&&snapshot.data.value(Operation::ApiRestrictions).value("enableFutures")==QJsonValue(false));
            permission=denied?"현재 조회한 설정에서 선물 주문 불가":permitted()?"API 선물 권한·계좌 canTrade 확인 · 실체결 미검증":
                "선물 주문 권한 미검증 · 종목 선택 후 연결·조회 필요";
        }
        if(QList<Operation>{Operation::OpenOrders,Operation::AlgoOpenOrders,Operation::History,Operation::AlgoHistory}.contains(job.op)){
            completedReads.remove(job.op);
            if(!page(job,r)){quality[job.op]="불완전 · 중복/상한/페이지";if(job.op==Operation::OpenOrders||job.op==Operation::AlgoOpenOrders)openComplete=false;
                if(phase==Phase::Prepare){prepareFailed=true;revoke();jobs.clear();}batchRemaining.clear();change("목록의 전체 범위를 확인하지 못했습니다.");}
        }
        change();schedule();
    }
};

FuturesTradingController::FuturesTradingController(Vault& v,QObject* parent):FuturesTradingController(v,new NetworkTradingTransport,parent){d_->transport->setParent(this);}
FuturesTradingController::FuturesTradingController(Vault& v,TradingTransport* t,QObject* parent,std::function<qint64()> timingClock):QObject(parent),d_(std::make_unique<Private>(this,v,t)){
    d_->timingClock=std::move(timingClock);
    connect(t,&TradingTransport::finished,this,[this](quint64 n,const HttpResult& r){d_->result(n,r);});
    connect(&v,&Vault::aboutToLock,this,&FuturesTradingController::stop);
    connect(&v,&Vault::accountsChanged,this,&FuturesTradingController::stop);
    d_->timer.setInterval(250);connect(&d_->timer,&QTimer::timeout,this,[this]{const auto r=d_->requestReady();const auto draft=d_->draftReady();
        const QString state=r.code+":"+draft.code+":"+QString::number(d_->testedRevision==d_->revision);
        if(state!=d_->readinessCode){d_->readinessCode=state;emit changed();}});d_->timer.start();
}
FuturesTradingController::~FuturesTradingController(){stop();}
void FuturesTradingController::stop(){
    ++d_->token;d_->transport->abort();d_->active=false;d_->jobs.clear();d_->phase=Private::Phase::Idle;
    d_->connected=d_->enabled=d_->openComplete=false;d_->batchRemaining.clear();d_->batchReviewed.clear();d_->revoke(true);
    d_->snapshot={};d_->observed.clear();d_->quality.clear();d_->open.clear();d_->closed.clear();d_->activity.clear();d_->serverAt=-1;d_->openAt=-1;
    d_->fingerprint.clear();d_->change("선물 연결이 중지되었습니다. 다시 연결·조회해 주세요.");
}
bool FuturesTradingController::selectAccount(const QString& id){
    if(id==d_->account.id&&supported()&&!d_->fingerprint.isEmpty())return true;
    stop();d_->adapter.reset();d_->account={};d_->catalog.clear();d_->catalogAt=-1;d_->symbol.clear();d_->limits={};d_->draft={};d_->halted=false;d_->reconnect=false;
    for(const auto& a:d_->vault.accounts())if(a.id==id)d_->account=a;
    if(d_->account.provider!="binance"||d_->account.market!="usdm_futures"||d_->account.environment!="live"){
        d_->change("Binance USDⓈ-M 실전 연결만 지원합니다. 모의·테스트넷 연결은 지원이 끝났으니 계좌·연결에서 삭제해 주세요.");return false;}
    d_->adapter=std::make_unique<BinanceUsdmAdapter>(d_->account);QString error;
    if(!d_->vault.withCredentials(id,[&](const CredentialSet& keys){const auto key=keys.find("api_key");if(key!=keys.end())
        d_->fingerprint=QString::fromLatin1(QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(key->second.data()),key->second.size()),QCryptographicHash::Sha256).toHex());},&error)||d_->fingerprint.isEmpty()){
        d_->adapter.reset();d_->change("인증정보를 사용할 수 없습니다.");return false;}
    d_->change("선물 연결을 선택했습니다. 종목 목록과 계좌를 직접 조회해 주세요.");return true;
}
AccountRecord FuturesTradingController::account()const{return d_->account;}
bool FuturesTradingController::supported()const{return bool(d_->adapter);}
bool FuturesTradingController::busy()const{return d_->active||!d_->jobs.isEmpty();}
bool FuturesTradingController::connected()const{return d_->connected;}
bool FuturesTradingController::liveEnabled()const{return d_->enabled;}
bool FuturesTradingController::halted()const{return d_->halted;}
bool FuturesTradingController::hasUnknown()const{return !d_->pending().isEmpty();}
bool FuturesTradingController::quoteFresh()const{return d_->fresh(Operation::Quote,10000);}
bool FuturesTradingController::orderTestPassed()const{return d_->testedRevision&&d_->testedRevision==d_->revision;}
bool FuturesTradingController::orderTestRequired()const{return !d_->draft.futures||d_->draft.futures->family!=OrderFamily::Conditional;}
bool FuturesTradingController::catalogFresh()const{return d_->catalogAt>=0&&d_->clock.elapsed()-d_->catalogAt<=3600000;}
QString FuturesTradingController::status()const{return d_->message;}
QString FuturesTradingController::permissionStatus()const{return d_->permission;}
QString FuturesTradingController::coverage()const{QStringList r;for(auto op:{Operation::Account,Operation::Positions,Operation::OpenOrders,Operation::AlgoOpenOrders,Operation::History,Operation::AlgoHistory})r<<operationName(op)+" · "+d_->quality.value(op,"미조회");
    r<<"일반·조건부 미체결을 각각 조회합니다. 이력은 선택 종목 최근 6일 범위이며 영구·전체 이력이 아닙니다.";return r.join('\n');}
QString FuturesTradingController::quoteSummary()const{
    const auto price=d_->snapshot.data.value(Operation::Quote).value("price").toString();
    if(d_->snapshot.quoteReceivedAtMs<=0||price.isEmpty())return QStringLiteral("시세 미조회");
    return d_->symbol+" · "+price+" · 수신 "+QDateTime::fromMSecsSinceEpoch(d_->snapshot.quoteReceivedAtMs).toString("yyyy-MM-dd HH:mm:ss t");
}
QString FuturesTradingController::symbol()const{return d_->symbol;}
QList<MarketInstrument> FuturesTradingController::instruments()const{return d_->catalog;}
QString FuturesTradingController::catalogStatus()const{return d_->catalogMessage;}
QList<Balance> FuturesTradingController::balances()const{return d_->snapshot.balances;}
QList<PositionRecord> FuturesTradingController::positions()const{return d_->snapshot.positions;}
QList<OrderRecord> FuturesTradingController::openOrders()const{return d_->open;}
QList<OrderRecord> FuturesTradingController::history()const{return d_->closed;}
QList<OrderRecord> FuturesTradingController::actionResults()const{return d_->activity.values();}
QList<OrderRecord> FuturesTradingController::pendingOrders()const{QList<OrderRecord> rows;for(const auto& v:d_->pending()){const auto j=v.toObject();const auto i=inputFrom(j);OrderRecord r;
    r.journalId=j.value("id").toString();r.orderId=i.orderId;r.clientId=i.clientId;r.algoId=i.algoId;r.clientAlgoId=i.clientAlgoId;r.actualOrderId=i.actualOrderId;
    r.symbol=i.symbol;r.side=i.side;r.type=i.type;r.price=i.price;r.quantity=i.quantity;r.futures=i.futures;r.status="pending";r.requestState="unknown";r.actionOutcome="unconfirmed";rows.append(r);}return rows;}
QJsonObject FuturesTradingController::verificationReceipt()const{return {{"schema",1},{"provider","binance"},{"market","usdm_futures"},{"environment",d_->account.environment},
    {"captured_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"connected",d_->connected},{"open_scope_complete",d_->openComplete},
    {"pending_count",pendingOrders().size()},{"official_order_test",orderTestPassed()},{"official_order_test_required",orderTestRequired()},
    {"conditional_limit_local_checks_only",!orderTestRequired()},{"real_execution_verified",false}};}
OrderLimits FuturesTradingController::limits()const{return d_->limits;}
OrderValidation FuturesTradingController::preview(const OrderInput& in)const{if(!d_->adapter)return {false,"선물 연결이 필요합니다.",{}, {}};auto s=d_->snapshot;s.serverNowMs=d_->now();return d_->adapter->validate(in,s,d_->limits);}
bool FuturesTradingController::setLimits(const OrderLimits& l){if(!decimal::positive(l.maxNotional)||!decimal::valid(l.maxDeviationPercent)||decimal::compare(l.maxDeviationPercent,"100")>0||!QStringList{"USDT","USDC"}.contains(l.currency))return false;
    d_->limits=l;d_->revoke();d_->change();return true;}

void FuturesTradingController::setDraft(const OrderInput& in){if(d_->draft==in)return;d_->draft=in;++d_->revision;d_->revoke();if(d_->phase==Private::Phase::Prepare||d_->phase==Private::Phase::Test){
    ++d_->token;d_->transport->abort();d_->active=false;d_->jobs.clear();d_->phase=Private::Phase::Idle;}d_->change();}
TradingReadiness FuturesTradingController::requestReadiness()const{return d_->requestReady();}
TradingReadiness FuturesTradingController::draftReadiness()const{return d_->draftReady();}
TradingReadiness FuturesTradingController::cancellationReadiness(const OrderRecord& o)const{return d_->cancelReady(o);}
void FuturesTradingController::loadMarkets(){if(busy()&&!interruptRead())return;if(!d_->requestReady().ready)return;d_->begin(Private::Phase::Catalog);d_->enqueue(Operation::Markets);d_->schedule();}
void FuturesTradingController::refresh(const QString& symbol,bool includeHistory){
    if(busy()&&!interruptRead())return;
    if(!d_->adapter||busy()||d_->clock.elapsed()<d_->cooldownUntil||!binanceUsdmSymbolValid(symbol))return;
    if(d_->fingerprint.isEmpty()){const auto id=d_->account.id;if(!selectAccount(id))return;}
    d_->reconnect=false;d_->symbol=symbol;d_->enabled=d_->connected=d_->openComplete=false;d_->revoke(true);d_->begin(Private::Phase::Refresh);
    d_->snapshot={};d_->observed.clear();d_->quality.clear();d_->serverAt=-1;
    auto plan=d_->adapter->refreshPlan();if(d_->account.environment=="live"&&!plan.contains(Operation::ApiRestrictions))plan.insert(2,Operation::ApiRestrictions);
    if(!plan.contains(Operation::SymbolConfig))plan.append(Operation::SymbolConfig);
    if(!includeHistory)plan.removeIf([](auto op){return op==Operation::History||op==Operation::AlgoHistory||op==Operation::Income;});
    d_->addReadPlan(plan);d_->schedule();
}
void FuturesTradingController::loadHistory(){
    if(!d_->connected||d_->symbol.isEmpty())return;if(busy()&&!interruptRead())return;if(!d_->requestReady().ready)return;
    d_->begin(Private::Phase::History);d_->closed.clear();d_->addReadPlan({Operation::Time,Operation::History,Operation::AlgoHistory});d_->schedule();
}
bool FuturesTradingController::refreshAssets(){
    if(!d_->adapter||busy()||!accountCredentialsAvailable(d_->vault,d_->account.id)||d_->reconnect||!d_->requestReady().ready)return false;
    d_->begin(Private::Phase::Assets);
    d_->assetsTimeRetries=0;d_->assetsFailure.clear();d_->orderTimeReady=false;d_->enabled=false;d_->revoke();
    d_->addReadPlan({Operation::Time,Operation::AccountConfig,Operation::Account,Operation::Positions});
    d_->schedule();return true;
}
bool FuturesTradingController::canReconnectAssets()const{
    return d_->adapter&&!busy()&&accountCredentialsAvailable(d_->vault,d_->account.id)&&
        d_->clock.elapsed()>=qMax(d_->cooldownUntil,d_->pacingUntil);
}
bool FuturesTradingController::reconnectAssets(){
    if(!canReconnectAssets())return false;
    d_->reconnect=false;return refreshAssets();
}
bool FuturesTradingController::canInterruptRead()const{return d_->phase!=Private::Phase::Action&&d_->phase!=Private::Phase::Reconcile;}
bool FuturesTradingController::historyLoading()const{return d_->phase==Private::Phase::History&&busy();}
bool FuturesTradingController::interruptRead(){
    if(!canInterruptRead())return false;
    ++d_->token;d_->transport->abort();d_->active=false;d_->jobs.clear();d_->phase=Private::Phase::Idle;d_->revoke();
    d_->change("선물 조회 중단 · 이미 제출한 주문과 미확정 기록은 유지합니다.");return true;
}
bool FuturesTradingController::prepareDraft(){
    if(!d_->requestReady().ready||!d_->connected||!d_->draft.futures||d_->draft.symbol!=d_->symbol||!d_->draft.algoId.isEmpty())return false;
    d_->revoke();d_->begin(Private::Phase::Prepare);d_->openComplete=false;
    QList<Operation> plan{Operation::Time,Operation::AccountConfig};if(d_->account.environment=="live")plan<<Operation::ApiRestrictions;
    plan<<Operation::Rules<<Operation::SymbolConfig<<Operation::LeverageBrackets<<Operation::Commissions<<Operation::TradingStatus
        <<Operation::Account<<Operation::Positions<<Operation::OpenOrders<<Operation::AlgoOpenOrders<<Operation::MarkPrice<<Operation::Quote<<Operation::Depth;
    d_->addReadPlan(plan);d_->schedule();return true;
}
bool FuturesTradingController::prepareCancellation(){if(historyLoading()&&!interruptRead())return false;if(!d_->requestReady().ready||!d_->connected)return false;d_->begin(Private::Phase::CancelPrepare);d_->openComplete=false;
    d_->addReadPlan({Operation::Time,Operation::AccountConfig});if(d_->account.environment=="live")d_->enqueue(Operation::ApiRestrictions,d_->readInput());
    d_->addReadPlan({Operation::OpenOrders,Operation::AlgoOpenOrders});d_->schedule();return true;}

void FuturesTradingController::reconcile(QWidget* origin){if(!direct(origin)||!d_->connected||!d_->requestReady().ready)return;d_->begin(Private::Phase::Reconcile);
    for(const auto& v:d_->pending())if(d_->owns(v.toObject()))d_->queryPending(v.toObject());d_->schedule();}
} // namespace keefi

namespace keefi {
AssetCapture FuturesTradingController::assetCapture() const {
    if(busy()||!accountCredentialsAvailable(d_->vault,d_->account.id)||!d_->observed.contains(Operation::Account)||!d_->observed.contains(Operation::Positions))return {};
    auto capture = captureExchangeAssets(d_->account,d_->fingerprint,d_->snapshot,d_->receivedWall.value(Operation::Account),
        d_->receivedWall.value(Operation::History),d_->observed.contains(Operation::Account),!hasUnknown(),coverage(),d_->closed);
    if (!capture.snapshot.id.isEmpty()) {
        const AssetEvidence configurationEvidence{"Binance 공식 선물 계좌 설정",
            "https://fapi.binance.com/fapi/v1/accountConfig",
            capture.snapshot.accountScope, 0, d_->receivedWall.value(Operation::AccountConfig),
            d_->completedReads.contains(Operation::AccountConfig) && d_->completedReads.contains(Operation::Account)};
        capture.providerTotal = captureBinanceUsdmProviderTotal(capture.snapshot, d_->snapshot.data.value(Operation::Account),
            d_->snapshot.data.value(Operation::AccountConfig), configurationEvidence);
    }
    return capture;
}
}
