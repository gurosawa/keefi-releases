#include "keefi/OkxFuturesReadController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/OkxAdapter.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <sodium.h>

namespace keefi {
namespace {
const QUrl Origin(QStringLiteral("https://openapi.okx.com"));
QString hash(const QString& value) {
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(),QCryptographicHash::Sha256).toHex());
}
bool number(const QString& value) { return value.size()<=80&&decimal::valid(value); }
bool signedNumber(QString value) { if(value.startsWith('-'))value.remove(0,1);return number(value); }
QString str(const QJsonObject& row,const char* field) { return row.value(QLatin1String(field)).toString(); }
bool optionalNumber(const QJsonObject& row,const char* field,bool signedValue=false) {
    const auto v=row.value(QLatin1String(field));
    return v.isUndefined()||v.isNull()||(v.isString()&&(v.toString().isEmpty()||
        (signedValue?signedNumber(v.toString()):number(v.toString()))));
}
bool updateTimeValid(const QJsonObject& row,qint64 serverNow,bool required=false) {
    const auto value=row.value("uTime");
    if(value.isUndefined()||value.isNull()||(value.isString()&&value.toString().isEmpty()))return !required;
    static const QRegularExpression millis("\\A[0-9]{1,16}\\z");
    bool valid=false;const auto time=value.toString().toLongLong(&valid);
    return value.isString()&&millis.match(value.toString()).hasMatch()&&valid&&time>0&&time<=serverNow+2000;
}
void wipe(RequestSpec& request) {
    for(auto it=request.headers.begin();it!=request.headers.end();++it)sodium_memzero(it.value().data(),it.value().size());
    sodium_memzero(request.body.data(),request.body.size());request.headers.clear();request.body.clear();
}
int retrySeconds(const HttpResult& response) {
    qint64 seconds=qMax(60,response.localRetryAfterSeconds);
    for(auto it=response.headers.cbegin();it!=response.headers.cend();++it)if(it.key().compare("Retry-After",Qt::CaseInsensitive)==0) {
        bool ok=false;const auto n=it.value().trimmed().toLongLong(&ok);
        if(ok&&n>0)seconds=qMax(seconds,qMin<qint64>(n,86400));
        else {const auto until=QDateTime::fromString(QString::fromLatin1(it.value()),Qt::RFC2822Date);
            if(until.isValid())seconds=qMax(seconds,qMin<qint64>((QDateTime::currentDateTimeUtc().msecsTo(until)+999)/1000,86400));}
    }
    return int(seconds);
}
// Position sizes in these two families are contract counts, not base-coin balances.
bool parsePositions(const HttpResult& http,const QString& family,qint64 serverNow,QList<PositionRecord>* positions) {
    const auto doc=parseExactJson(http.body);if(!doc.isObject())return false;
    const auto object=doc.object();if(str(object,"code")!="0"||!object.value("data").isArray())return false;
    const auto rows=object.value("data").toArray();if(rows.size()>10000)return false;
    static const QRegularExpression instrument("\\A[A-Z0-9_]{1,40}-[A-Z0-9_]{1,40}-(SWAP|[0-9]{6})\\z");
    QSet<QString> seen;
    for(const auto& value:rows) {
        if(!value.isObject())return false;const auto row=value.toObject();PositionRecord p;
        p.symbol=str(row,"instId");p.positionSide=str(row,"posSide");p.amount=str(row,"pos");
        p.marginType=str(row,"mgnMode");p.marginAsset=str(row,"ccy");p.unrealizedPnl=str(row,"upl");
        p.updateTime=str(row,"uTime");
        const auto identity=p.symbol+":"+p.positionSide+":"+p.marginType+":"+p.marginAsset;
        const auto rawCurrency=row.value("ccy");
        const bool currencyValid=rawCurrency.isUndefined()||rawCurrency.isNull()||
            (rawCurrency.isString()&&(p.marginAsset.isEmpty()||binanceIdentifierValid(p.marginAsset)));
        if(str(row,"instType")!=family||p.symbol.size()>80||!instrument.match(p.symbol).hasMatch()||
            (family=="SWAP")!=p.symbol.endsWith("-SWAP")||
            !QStringList{"net","long","short"}.contains(p.positionSide)||
            !QStringList{"cross","isolated"}.contains(p.marginType)||!currencyValid||
            !signedNumber(p.amount)||(p.positionSide!="net"&&p.amount.startsWith('-'))||
            !signedNumber(p.unrealizedPnl)||!updateTimeValid(row,serverNow,true)||seen.contains(identity))return false;
        seen.insert(identity);
        if(!optionalNumber(row,"notionalUsd",true))return false;
        for(const auto* key:{"avgPx","markPx","liqPx","lever","margin","imr","mmr"})
            if(!optionalNumber(row,key))return false;
        p.entryPrice=str(row,"avgPx");p.markPrice=str(row,"markPx");p.liquidationPrice=str(row,"liqPx");
        p.notional=str(row,"notionalUsd");p.leverage=str(row,"lever");p.isolatedWallet=str(row,"margin");
        p.initialMargin=str(row,"imr");p.maintMargin=str(row,"mmr");positions->append(p);
    }
    return true;
}
}

struct OkxFuturesReadController::Private {
    OkxFuturesReadController* q;
    Vault& vault;
    TradingTransport* transport;
    AccountRecord account;
    QElapsedTimer clock;
    std::function<qint64()> timingClock;
    quint64 token=0;
    bool running=false,active=false,isConnected=false,manualReconnectRequired=false;
    int step=0;
    qint64 cooldownUntil=0,timeSent=0,serverTime=0,serverAt=0;
    QString message,scope,accountMode;
    AssetCapture retained,working;
    QList<PositionRecord> retainedPositions,workingPositions;
    Operation current=Operation::Time;
    QString family;
    explicit Private(OkxFuturesReadController* owner,Vault& v,TradingTransport* t,std::function<qint64()> timing)
        : q(owner),vault(v),transport(t),timingClock(std::move(timing)) {clock.start();}
    qint64 timingNow() const { return timingClock?timingClock():clock.elapsed(); }
    qint64 serverNow() const { return serverTime>0?serverTime+clock.elapsed()-serverAt:0; }
    bool available() const { return !account.id.isEmpty()&&accountCredentialsAvailable(vault,account.id); }
    bool coolingDown() const { return clock.elapsed()<cooldownUntil||TradingRequestLimits::shared()->remainingMs(Origin)>0; }
    void change(const QString& text) {message=text;emit q->changed();}
    void fail(const QString& reason,bool auth=false,int retry=0) {
        running=active=isConnected=false;working={};workingPositions.clear();
        manualReconnectRequired=manualReconnectRequired||auth;
        if(retry>0)cooldownUntil=qMax(cooldownUntil,clock.elapsed()+qint64(retry)*1000);
        change(QStringLiteral("OKX 통합 잔고 조회 실패 · ")+reason+
            (auth?QStringLiteral(" · 인증정보를 확인하고 다시 연결하세요."):retry>0?QStringLiteral(" · 대기 후 다시 조회하세요."):QStringLiteral(" · 다시 조회하세요.")));
    }
    RequestSpec request(const CredentialSet& credentials,QString* error) const {
        auto codecAccount=account;codecAccount.market="spot";
        OkxAdapter codec(codecAccount);OrderInput input;
        auto r=codec.build(current==Operation::Positions?Operation::Account:current,codecAccount,input,credentials,serverNow(),error);
        if(current!=Operation::Positions||r.url.isEmpty())return r;
        // Reuse credential/header validation, then sign only this fixed read
        // route. No caller-supplied path, method or query is accepted.
        const QByteArray target="/api/v5/account/positions?instType="+family.toLatin1();
        r.operation=Operation::Positions;r.url=QUrl::fromEncoded(Origin.toEncoded()+target);
        const auto secret=credentials.find("secret");
        try {
            const auto preimage=r.headers.value("OK-ACCESS-TIMESTAMP")+r.method+target;
            SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)),digest(crypto_auth_hmacsha256_BYTES);
            auto* h=reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
            crypto_auth_hmacsha256_init(h,secret->second.data(),secret->second.size());
            crypto_auth_hmacsha256_update(h,reinterpret_cast<const unsigned char*>(preimage.constData()),preimage.size());
            crypto_auth_hmacsha256_final(h,digest.data());
            QByteArray raw(reinterpret_cast<const char*>(digest.data()),digest.size());
            r.headers["OK-ACCESS-SIGN"]=raw.toBase64();sodium_memzero(raw.data(),raw.size());
        } catch(...) {wipe(r);r={};if(error)*error="서명용 보안 메모리를 확보하지 못했습니다.";}
        return r;
    }
    void next() {
        if(!running)return;
        if(!available()){fail("금고 잠금 또는 인증정보 없음");return;}
        if(coolingDown()){fail("요청 제한",false,60);return;}
        if(step>=6||(step==4&&accountMode=="1")) {
            working.snapshot.balanceComplete=true;working.snapshot.evidence.complete=true;
            retained=std::move(working);retainedPositions=std::move(workingPositions);
            running=active=false;isConnected=true;change(accountMode=="1"?
                "OKX 현물 전용 계정의 통합 잔고를 조회했습니다.":"OKX 통합 잔고와 선물 포지션을 조회했습니다.");return;
        }
        static constexpr Operation operations[]={Operation::Time,Operation::AccountConfig,Operation::Account,
            Operation::FundingBalance,Operation::Positions,Operation::Positions};
        current=operations[step];family=step==4?"SWAP":step==5?"FUTURES":QString{};
        RequestSpec r;QString error;
        // The transport must run after the borrowed credential visitor returns.
        const bool borrowed=vault.withCredentials(account.id,[&](const CredentialSet& credentials){r=request(credentials,&error);});
        if(!borrowed||r.url.isEmpty()){wipe(r);fail("요청 인증정보를 사용할 수 없음");return;}
        active=true;const auto sentToken=++token;if(current==Operation::Time)timeSent=timingNow();
        emit q->changed();
        if(running&&active&&token==sentToken)transport->send(sentToken,r);
        wipe(r);
    }
    AssetEvidence evidence(const QString& source,const QString& path,qint64 received) const {
        // Account/position uTime is the last *change*, not the snapshot receipt.
        // An unchanged balance can be old while this authenticated read is new.
        // Use its original receipt for freshness; never invent a source timestamp.
        return {source,Origin.toString()+path,scope,0,received,true};
    }
    bool balances(const AdapterResult& parsed,const HttpResult& result,qint64 received) {
        const bool trading=current==Operation::Account;
        const auto doc=parseExactJson(result.body);const auto rows=doc.object().value("data").toArray();
        const auto detail=trading?rows.first().toObject().value("details").toArray():rows;
        if(trading) {
            if(!updateTimeValid(rows.first().toObject(),serverNow()))return false;
            auto& s=working.snapshot;s.provider="okx";s.environment=account.environment;
            s.market="okx_unified";s.accountScope=scope;s.id=hash(scope+":"+QString::number(received));s.timeMs=received;
            s.connectionId=account.id;s.coverage=accountMode=="1"?"통합 거래계정 equity + 펀딩 잔고; 현물 전용 모드":
                "통합 거래계정 equity + 펀딩 잔고; SWAP·FUTURES 포지션 별도 표시";
            s.evidence=evidence("OKX 공식 통합 잔고","/api/v5/account/balance",received);
        }
        int i=0;
        for(const auto& b:parsed.balances) {
            AssetComponent c;c.id=b.section+":"+b.asset;c.asset=b.asset;c.valuationGroup=b.section;
            c.evidence=evidence(trading?"OKX 공식 통합 잔고":"OKX 공식 펀딩 잔고",
                trading?"/api/v5/account/balance":"/api/v5/asset/balances",received);
            if(trading) {
                if(!signedNumber(b.equity)||!optionalNumber(detail[i].toObject(),"upl",true)||
                    !updateTimeValid(detail[i].toObject(),serverNow()))return false;
                c.kind=AssetComponentKind::Equity;c.quantity=b.equity;
                c.providerUnrealizedPnl=str(detail[i].toObject(),"upl");c.reportedValueCurrency=b.asset;
            } else {
                if(!number(b.cashBalance))return false;c.kind=AssetComponentKind::Holding;c.quantity=b.cashBalance;
            }
            working.snapshot.components.append(c);++i;
        }
        return true;
    }
    void onResult(quint64 replyToken,const HttpResult& result) {
        if(!running||!active||replyToken!=token)return;
        active=false;
        if(!available()){fail("금고 잠금 또는 인증정보 없음");return;}
        if(result.localRetryAfterSeconds>0||result.status==429||result.status==418) {
            fail("요청 제한",false,retrySeconds(result));return;
        }
        auto codecAccount=account;codecAccount.market="spot";OkxAdapter codec(codecAccount);OrderInput input;
        // The existing codec supplies auth/error handling for the custom route.
        // Its successful position payload is parsed separately below.
        AdapterResult parsed;
        if(current==Operation::Positions) {
            const auto doc=parseExactJson(result.body);const auto object=doc.object();
            if(result.status>=200&&result.status<300&&!result.networkError&&!result.timedOut&&doc.isObject()&&str(object,"code")=="0")parsed.ok=true;
            else parsed=codec.parse(Operation::AccountConfig,result,input);
        } else parsed=codec.parse(current,result,input);
        if(!parsed.ok) {
            fail(parsed.authenticationFailed?"인증 실패":parsed.rateLimited?"요청 제한":
                result.networkError||result.timedOut?"네트워크 오류":"응답 또는 조회 권한 확인 필요",
                parsed.authenticationFailed,parsed.rateLimited?qMax(60,parsed.retryAfterSeconds):0);return;
        }
        const auto received=QDateTime::currentMSecsSinceEpoch();
        if(current==Operation::Time) {
            const auto rtt=timingNow()-timeSent;
            if(rtt<0||rtt>2000){fail("서버 시각 응답 지연");return;}
            serverTime=parsed.serverTimeMs+rtt/2;serverAt=clock.elapsed();
        } else if(current==Operation::AccountConfig) {
            const auto newScope=hash("okx:"+account.environment+":"+parsed.scope);
            if(!retained.snapshot.accountScope.isEmpty()&&retained.snapshot.accountScope!=newScope) {
                retained={};retainedPositions.clear();
            }
            const auto oldScope=scope;scope=newScope;
            if(oldScope!=newScope) {
                emit q->identityChanged(oldScope,newScope);
                if(!running)return;
            }
            accountMode=parsed.data.value("acctLv").toString();
            if(!QStringList{"1","2","3","4"}.contains(accountMode)){fail("계정 모드를 확인할 수 없음");return;}
        } else if(current==Operation::Account||current==Operation::FundingBalance) {
            if(!balances(parsed,result,received)){fail("잔고 필수 값 누락 또는 형식 오류");return;}
        } else {
            QList<PositionRecord> rows;if(!parsePositions(result,family,serverNow(),&rows)){fail("선물 포지션 형식 오류");return;}
            for(const auto& p:rows) {
                AssetComponent c;c.id="position:"+family+":"+p.symbol+":"+p.positionSide+":"+p.marginType+":"+p.marginAsset;
                c.asset=p.symbol;c.quantity=p.amount;c.kind=AssetComponentKind::PositionNotional;c.valuationGroup="positions";
                // Do not attach a currency to a raw PnL whose settlement currency
                // is unavailable. positions() keeps both original values; the
                // persisted display amount stays unknown instead of guessing.
                if(!p.marginAsset.isEmpty())c.providerUnrealizedPnl=p.unrealizedPnl;
                c.reportedValueCurrency=p.marginAsset;
                c.displaySymbol=p.symbol;c.displayName=family=="SWAP"?"무기한 선물 · 계약 수":"만기 선물 · 계약 수";
                c.evidence=evidence("OKX 공식 선물 포지션","/api/v5/account/positions",received);
                working.snapshot.components.append(c);workingPositions.append(p);
            }
        }
        ++step;next();
    }
};

OkxFuturesReadController::OkxFuturesReadController(Vault& vault,TradingTransport* transport,QObject* parent,
    std::function<qint64()> timingClock) : QObject(parent),d_(std::make_unique<Private>(this,vault,
        transport?transport:new NetworkTradingTransport(this),std::move(timingClock))) {
    connect(d_->transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& result){d_->onResult(token,result);});
    connect(&vault,&Vault::aboutToLock,this,&OkxFuturesReadController::stop);
    connect(&vault,&Vault::accountsChanged,this,[this]{if(!d_->account.id.isEmpty())stop();});
}
OkxFuturesReadController::~OkxFuturesReadController() {stop();}
bool OkxFuturesReadController::selectAccount(const QString& id) {
    if(id==d_->account.id&&!id.isEmpty()&&d_->available())return true;
    if(busy())return false;
    for(const auto& account:d_->vault.accounts())if(account.id==id&&account.provider=="okx"&&
        QStringList{"spot","okx_futures"}.contains(account.market)&&account.environment=="live"&&account.region=="global"&&
        accountCredentialsAvailable(d_->vault,id)) {
        stop();d_->account=account;d_->change("OKX 통합 잔고 조회 연결을 선택했습니다.");return true;
    }
    return false;
}
bool OkxFuturesReadController::refreshAssets() {
    if(busy()||!d_->available()||d_->manualReconnectRequired||d_->coolingDown())return false;
    d_->running=true;d_->isConnected=false;d_->step=0;d_->working={};d_->workingPositions.clear();
    d_->serverTime=0;d_->accountMode.clear();d_->next();return true;
}
bool OkxFuturesReadController::canReconnectAssets() const {return !busy()&&d_->available()&&!d_->coolingDown();}
bool OkxFuturesReadController::reconnectAssets() {
    if(!canReconnectAssets())return false;d_->manualReconnectRequired=false;return refreshAssets();
}
bool OkxFuturesReadController::interruptRead() {
    ++d_->token;d_->transport->abort();d_->running=d_->active=d_->isConnected=false;
    d_->working={};d_->workingPositions.clear();d_->change("OKX 통합 잔고 조회를 중단했습니다.");return true;
}
void OkxFuturesReadController::stop() {
    ++d_->token;d_->transport->abort();d_->running=d_->active=d_->isConnected=d_->manualReconnectRequired=false;
    d_->working=d_->retained={};d_->workingPositions.clear();d_->retainedPositions.clear();d_->account={};d_->scope.clear();
    d_->serverTime=0;d_->change("OKX 조회 연결을 중지했습니다.");
}
bool OkxFuturesReadController::busy() const {return d_->running;}
bool OkxFuturesReadController::connected() const {return d_->isConnected;}
QString OkxFuturesReadController::status() const {return d_->message;}
AccountRecord OkxFuturesReadController::account() const {return d_->account;}
AssetCapture OkxFuturesReadController::assetCapture() const {return busy()||!d_->available()?AssetCapture{}:d_->retained;}
QList<PositionRecord> OkxFuturesReadController::positions() const {return busy()||!d_->available()?QList<PositionRecord>{}:d_->retainedPositions;}
}
