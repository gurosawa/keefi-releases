#include "keefi/IdentityCompatibility.h"
#include "keefi/BinanceEarnCollector.h"
#include "keefi/AssetsLedger.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QTimer>
#include <chrono>

namespace keefi {
namespace {
qint64 sharedEarnDelay(int spacing) {
    // In-process pacing only; transport's shared Retry-After remains authoritative.
    static QMutex mutex; static qint64 next=0;
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    QMutexLocker guard(&mutex);const auto delay=qMax(qint64(0),next-now);next=now+delay+spacing;return delay;
}
QString binding(const AccountRecord& account,const CredentialSet& credentials){
    QCryptographicHash h(QCryptographicHash::Sha256);h.addData(identityDomain(IdentityDomain::CurrentAssetsCredentialBinding));
    for(const auto& part:{account.provider,account.environment,account.market,account.region}){const auto b=part.toUtf8();h.addData(QByteArray::number(b.size()));h.addData(":");h.addData(b);}
    for(const auto& [name,value]:credentials){h.addData(name.toUtf8());h.addData(":");h.addData(QByteArray::number(value.size()));h.addData(":");h.addData(QByteArrayView(reinterpret_cast<const char*>(value.data()),qsizetype(value.size())));}
    return QString::fromLatin1(h.result().toHex());
}
QString errorText(const QString& code){
    if(code=="AUTHENTICATION")return "Earn 읽기 권한·허용 IP를 확인해 주세요. 자동 재인증은 하지 않습니다.";
    if(code=="RATE_LIMIT")return "거래소 요청 제한으로 중단했습니다. 기존 기록은 유지합니다.";
    if(code=="NETWORK")return "연결 문제로 Earn 기록을 모두 받지 못했습니다. 기존 기록은 유지합니다.";
    if(code.startsWith("STORAGE"))return "Earn 기록의 저장 완료 상태를 확정하지 못했습니다. 원장을 다시 열어 확인해 주세요.";
    if(code=="BINDING")return "계좌 연결 정보를 다시 확인한 뒤 조회해 주세요.";
    if(code=="CLOCK_SKEW")return "기기와 거래소의 시간이 맞지 않아요. 기기의 날짜·시간을 자동으로 설정한 뒤 다시 조회해 주세요.";
    if(code=="SERVER_TIME")return "거래소 시간을 확인하지 못했어요. 연결 상태를 확인한 뒤 다시 조회해 주세요.";
    if(code=="QUERY_INVALID"||code=="REQUEST_INVALID")return "Earn 조회 기간이나 요청을 확인하지 못했어요. 다시 조회해 주세요.";
    return "Earn 응답의 형식·중복·완전성을 확인하지 못해 조회를 중단했습니다. 기존 기록은 유지합니다.";
}
}
struct BinanceEarnCollector::Private {
    BinanceEarnCollector* q;Vault& vault;AssetsLedger& ledger;TradingTransport* transport;
    TestHooks hooks;QTimer timer;QElapsedTimer serverAge,storageAge;
    bool active=false,waitingTime=false,waitingWrite=false,paced=false,planned=false,newTrackingState=false;
    quint64 token=0,awaiting=0,receipt=0; qint64 serverTime=0,runThrough=0,runStartedAt=0;
    AccountRecord account;EarnAccountState state;QList<EarnQuery> jobs;EarnPageAccumulator pages;EarnBatch pendingBatch;
    QString message,code;
    Private(BinanceEarnCollector* owner,Vault& v,AssetsLedger& l,TradingTransport* t,TestHooks h):q(owner),vault(v),ledger(l),transport(t),hooks(std::move(h)){
        if(!transport)transport=new NetworkTradingTransport(q);
        timer.setSingleShot(true);
    }
    qint64 now()const{return hooks.nowMs?hooks.nowMs():QDateTime::currentMSecsSinceEpoch();}
    void fail(const QString& why){
        active=false;waitingTime=waitingWrite=false;awaiting=receipt=0;timer.stop();++token;transport->abort();
        state={};jobs.clear();pages.clear();pendingBatch={};account={};code=why;message=errorText(why);
        emit q->changed();emit q->finished(false);
    }
    void clear(){
        active=false;timer.stop();++token;awaiting=receipt=0;waitingTime=waitingWrite=paced=false;transport->abort();
        jobs.clear();pages.clear();pendingBatch={};state={};account={};message.clear();code.clear();serverTime=runThrough=runStartedAt=0;
        planned=newTrackingState=false;serverAge.invalidate();
    }
    void clock(){
        if(!active||!vault.isUnlocked())return;
        RequestSpec r;r.operation=Operation::Time;r.url=QUrl("https://api.binance.com/api/v3/time");
        waitingTime=true;awaiting=++token;transport->send(awaiting,r);
    }
    void advance(){
        if(!active||!vault.isUnlocked())return;
        if(waitingWrite){save();return;}
        if(!planned||!serverTime||!serverAge.isValid()||serverAge.elapsed()>30000){clock();return;}
        if(jobs.isEmpty()){
            active=false;message="Earn 원금·회수·보상 조회와 암호화 저장 완료 · 손익 검증 완료를 뜻하지 않습니다.";
            account={};state={};emit q->changed();emit q->finished(true);return;
        }
        if(!paced){paced=true;const auto delay=hooks.pacingMs>0?sharedEarnDelay(qMax(2000,hooks.pacingMs)):0;if(delay>0){timer.start(int(qMin(delay,qint64(300000))));return;}}
        paced=false;
        const auto serverNow=serverTime+serverAge.elapsed();
        if(qAbs(serverNow-now())>120000||jobs.first().throughMs>now()||jobs.first().throughMs>serverNow){fail("CLOCK_SKEW");return;}
        if(!BinanceEarnProtocol::validQuery(jobs.first(),serverNow)){fail("QUERY_INVALID");return;}
        RequestSpec r;QString error;bool matching=false;
        const bool borrowed=vault.withCredentials(account.id,[&](const CredentialSet& c){
            matching=binding(account,c)==state.binding;
            if(matching)r=BinanceEarnProtocol::request(jobs.first(),c,serverNow,&error);
        },&error);
        if(!borrowed||!matching){fail("BINDING");return;}
        if(!allowedEarnRead(r)){fail("REQUEST_INVALID");return;}
        waitingTime=false;awaiting=++token;
        message=BinanceEarnProtocol::label(jobs.first().source)+QString(" 조회 중 · %1쪽").arg(jobs.first().page);
        emit q->changed();if(active&&vault.isUnlocked()&&awaiting==token)transport->send(awaiting,r);
    }
    void save(){
        if(!active||!vault.isUnlocked())return;
        // Once dispatched, only this exact receipt may complete the batch. A
        // timer must not merge it again, and a late receipt from a cancelled run
        // must not acknowledge a new run waiting behind another writer.
        if(receipt){
            if(storageAge.elapsed()>30000){fail("STORAGE_CONFIRMATION");return;}
            timer.start(50);return;
        }
        if(ledger.isWriting()){
            if(storageAge.elapsed()>30000){fail("STORAGE_BUSY");return;}
            timer.start(50);return;
        }
        if(!ledger.isOpen()){fail("STORAGE_LOCKED");return;}
        auto latest=state;
        for(const auto& s:ledger.earnStates())if(s.accountScope==state.accountScope)latest=s;
        if(latest.binding!=state.binding||latest.connectionId!=state.connectionId){fail("BINDING");return;}
        EarnAccountState candidate;QString error;bool merged=false;
        const bool borrowed=vault.withAssetsLedgerKey(false,[&](const SecureBuffer& key){merged=EarnTracking::merge(latest,pendingBatch,key,&candidate,&error);},&error);
        if(!borrowed||!merged){fail("STORAGE_MERGE");return;}
        static quint64 nextReceipt=0;receipt=++nextReceipt;if(!receipt)receipt=++nextReceipt;
        const auto result=ledger.saveEarnState(candidate,latest.revision,receipt,&error);
        if(result==AssetsLedger::CollectionWrite::Failed){fail("STORAGE_WRITE");return;}
        state=std::move(candidate);
        if(result==AssetsLedger::CollectionWrite::Unchanged)committed(true);
        else timer.start(100); // Bounded wait; never let a stalled writer keep UI busy forever.
        // The next cursor is not dispatched until our exact receipt succeeds.
    }
    void committed(bool ok){
        if(!active||!waitingWrite)return;
        if(!ok){fail("STORAGE_WRITE");return;}
        waitingWrite=false;receipt=0;pendingBatch={};pages.clear();jobs.removeFirst();
        message="Earn 조회 구간 저장됨";emit q->changed();if(active)timer.start(0);
    }
    void receive(quint64 id,const HttpResult& h){
        if(!active||!vault.isUnlocked()||id!=awaiting)return;
        awaiting=0;
        if(waitingTime){
            waitingTime=false;QString error;const auto doc=parseExactJson(h.body,&error);bool ok=false;
            const auto value=doc.object().value("serverTime").toString().toLongLong(&ok);
            if(h.status!=200||h.networkError||h.timedOut||!ok||value<=0||value>253402300799999LL||!error.isEmpty()){fail("SERVER_TIME");return;}
            if(qAbs(value-now())>120000){fail("CLOCK_SKEW");return;}
            serverTime=value;serverAge.start();
            if(!planned){
                // Freeze one end for the entire run, before page 1 is signed.
                // Never claim a local-clock future interval or change a window
                // halfway through its page chain after a server-time refresh.
                runThrough=qMin(runStartedAt,value);
                if(newTrackingState)state.trackingStartMs=runThrough;
                auto planState=state;
                if(planState.trackingStartMs>runThrough){
                    if(planState.trackingStartMs-runThrough>120000){fail("CLOCK_SKEW");return;}
                    // A prior version may have stored a slightly fast local
                    // start. Keep that recorded start; only widen discovery.
                    planState.trackingStartMs=runThrough;
                }
                jobs=EarnTracking::queries(planState,runThrough);
                if(jobs.isEmpty()){fail("QUERY_INVALID");return;}
                planned=true;
            }
            timer.start(0);return;
        }
        if(jobs.isEmpty())return;
        const auto received=now();
        if(received<jobs.first().throughMs||qAbs(serverTime+serverAge.elapsed()-received)>120000){fail("CLOCK_SKEW");return;}
        const auto page=BinanceEarnProtocol::parsePage(jobs.first(),h,received);QString error;
        if(!page.ok){fail(page.errorCode);return;}
        if(!pages.add(jobs.first(),page,received,&error)){fail(error);return;}
        if(!pages.complete()){++jobs.first().page;timer.start(0);return;}
        pendingBatch=pages.batch();waitingWrite=true;receipt=0;storageAge.start();save();
    }
};
BinanceEarnCollector::BinanceEarnCollector(Vault& v,AssetsLedger& l,TradingTransport* t,QObject* parent)
    :BinanceEarnCollector(v,l,t,TestHooks{},parent){}
BinanceEarnCollector::BinanceEarnCollector(Vault& v,AssetsLedger& l,TradingTransport* t,TestHooks h,QObject* parent)
    :QObject(parent),d_(std::make_unique<Private>(this,v,l,t,std::move(h))){
    connect(&d_->timer,&QTimer::timeout,this,[this]{d_->advance();});
    connect(d_->transport,&TradingTransport::finished,this,[this](quint64 id,const HttpResult& h){d_->receive(id,h);});
    connect(&v,&Vault::aboutToLock,this,&BinanceEarnCollector::stop);
    connect(&v,&Vault::accountsChanged,this,&BinanceEarnCollector::stop);
    connect(&l,&AssetsLedger::locked,this,&BinanceEarnCollector::stop);
    connect(&l,&AssetsLedger::collectionWriteFinished,this,[this](quint64 receipt,bool ok,const QString&){
        if(d_->active&&d_->waitingWrite&&receipt==d_->receipt)d_->committed(ok);
    });
}
BinanceEarnCollector::~BinanceEarnCollector(){disconnect(d_->transport,nullptr,this,nullptr);d_->clear();}
bool BinanceEarnCollector::start(const AssetSnapshot& snapshot){
    if(d_->active)return false;
    d_->clear();
    if(!d_->vault.isUnlocked()||!d_->ledger.isOpen()||snapshot.provider!="binance"||snapshot.environment!="live"||
        !snapshot.balanceComplete||!snapshot.evidence.complete||snapshot.connectionBinding.isEmpty()||snapshot.timeMs<=0||
        snapshot.timeMs>d_->now()||d_->now()-snapshot.timeMs>DisplayFreshnessMs){d_->code="BINDING";d_->message=errorText(d_->code);emit changed();return false;}
    for(const auto& account:d_->vault.accounts())if(account.id==snapshot.connectionId)d_->account=account;
    if(d_->account.provider!="binance"||d_->account.environment!="live"||
        (d_->account.market!="spot"&&d_->account.market!="usdm_futures")){d_->code="BINDING";d_->message=errorText(d_->code);emit changed();return false;}
    d_->state.accountScope=snapshot.accountScope;d_->state.connectionId=snapshot.connectionId;d_->state.binding=snapshot.connectionBinding;d_->state.trackingStartMs=d_->now();
    d_->newTrackingState=true;
    for(const auto& s:d_->ledger.earnStates())if(s.accountScope==snapshot.accountScope){
        if(s.binding!=snapshot.connectionBinding||s.connectionId!=snapshot.connectionId){d_->code="BINDING";d_->message=errorText(d_->code);emit changed();return false;}
        d_->state=s;
        d_->newTrackingState=false;
    }
    if(!EarnTracking::validAccount(d_->state)){d_->code="BINDING";d_->message=errorText(d_->code);emit changed();return false;}
    d_->runStartedAt=d_->now(); // Build the history plan only after public server time arrives.
    d_->active=true;d_->message="Earn 원금·회수·보상 기록 조회 준비";emit started(snapshot.accountScope);emit changed();
    if(d_->active)d_->timer.start(0);return d_->active;
}
void BinanceEarnCollector::stop(){const bool was=d_->active;d_->clear();emit changed();if(was)emit finished(false);}
bool BinanceEarnCollector::busy()const{return d_->active;}
QString BinanceEarnCollector::status()const{return d_->message;}
QString BinanceEarnCollector::errorCode()const{return d_->code;}
} // namespace keefi
