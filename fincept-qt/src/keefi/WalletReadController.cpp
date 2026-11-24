#include "keefi/IdentityCompatibility.h"
#include "keefi/WalletReadController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/SolanaTokenData.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <QTimer>
#include <QUuid>

namespace keefi {
namespace {
const QString TokenProgram="TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
const QString Token2022Program="TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb";
bool pubkey(const QString& s){return s.size()<=44&&SolanaTransactionCodec::publicKeyValid(s);}
bool natural(const QString& s){return !s.isEmpty()&&s.size()<=20&&std::all_of(s.cbegin(),s.cend(),[](QChar c){return c>=u'0'&&c<=u'9';})&&decimal::compare(s,"18446744073709551615")<=0;}
QString units(QString n,int decimals){if(!decimals)return decimal::normalize(n);while(n.size()<=decimals)n.prepend('0');n.insert(n.size()-decimals,'.');return decimal::normalize(n);}
QString hash(const QString& s){return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(),QCryptographicHash::Sha256).toHex());}
}
struct WalletReadController::Private {
    WalletReadController* q;Vault& vault;SwapTransport* transport;AccountRecord account;
    QString address,id,message="조회할 Solana 주소를 등록하세요.";int stage=0;bool busy=false;quint64 token=0;
    qint64 received=0;QMap<QString,AssetComponent> components;QSet<QString> tokenAccounts;AssetCapture latest;
    QMap<QString,int> precisions;
    QStringList remaining,batch,blocks;
    QList<SolanaTokenPrice> batchPrices;
    QList<AssetQuote> quotes;
    QSet<QString> priceEligible;
    qint64 priceReceived=0;QElapsedTimer enrichmentClock;
    Private(WalletReadController* owner,Vault& v,SwapTransport* t):q(owner),vault(v),transport(t){}
    void fail(const QString& text){busy=false;components.clear();tokenAccounts.clear();precisions.clear();priceEligible.clear();remaining.clear();batch.clear();blocks.clear();batchPrices.clear();quotes.clear();address.clear();message=text;emit q->changed();}
    void send(SwapOperation op,const QJsonArray& params){id=QUuid::createUuid().toString(QUuid::Id128);QString error;
        auto r=SolanaReadRpc::request(op,id,params,&error);if(!error.isEmpty()){fail(error);return;}
        message=stage==0?"Solana 메인넷 확인 중":stage==1?"SOL 잔고 조회 중":stage==2?"SPL Token 보유 조회 중":"Token-2022 보유 조회 중";
        const auto n=++token;emit q->changed();transport->send(n,r);
    }
    void tokens(){const auto program=stage==2?TokenProgram:Token2022Program;
        send(SwapOperation::TokenAccounts,{address,QJsonObject{{"programId",program}},QJsonObject{{"encoding","jsonParsed"},{"commitment","confirmed"}}});}
    void publish(){
        AssetCapture capture;auto& s=capture.snapshot;s.provider="phantom";s.environment="mainnet";s.market="solana";
        s.accountScope=hash(identityDomainText(IdentityDomain::SolanaMainnet)+address);s.connectionId=account.id;s.timeMs=received;s.id=hash(s.accountScope+":"+QString::number(received));
        s.balanceComplete=true;s.reconciled=false;s.coverage="SOL·SPL Token·Token-2022 기초 수량. 스테이킹·압축 자산·비공개 잔고·확장에 따른 표시 수량/이전 성과는 미포함";
        s.evidence={"Solana 메인넷 공식 RPC","https://api.mainnet-beta.solana.com","SOL/SPL/Token-2022",0,received,true};
        for(auto c:components){c.evidence=s.evidence;s.components.append(c);}capture.quotes=quotes;latest=std::move(capture);
        busy=false;components.clear();tokenAccounts.clear();precisions.clear();priceEligible.clear();address.clear();remaining.clear();batch.clear();blocks.clear();batchPrices.clear();quotes.clear();
        message="지갑 자산 조회 완료";emit q->changed();
    }
    void lookup(){
        if(remaining.isEmpty()){publish();return;}
        if(enrichmentClock.elapsed()>30000){
            for(auto& c:components)if(remaining.contains(c.asset.mid(7))){c.metadataIssue="조회 시간 초과";c.priceIssue="조회 시간 초과";}
            publish();return;
        }
        batch=remaining.mid(0,50);remaining=remaining.mid(batch.size());stage=7;priceEligible.clear();
        id=QUuid::createUuid().toString(QUuid::Id128);QString error;
        QJsonArray mints;for(const auto& mint:batch)mints.append(mint);
        const auto request=SolanaReadRpc::request(SwapOperation::Accounts,id,{mints,QJsonObject{{"encoding","jsonParsed"},{"commitment","confirmed"}}},&error);
        message="토큰 수량 단위 확인 중";emit q->changed();transport->send(++token,request);
    }
    void enrichment(const HttpResult& h){
        QString error;
        if(h.status==429){
            auto affected=remaining;affected.append(batch);
            for(auto& c:components)if(affected.contains(c.asset.mid(7))){
                if(stage==4||stage==7)c.metadataIssue="조회 보류 · 요청 한도 초과";
                c.priceIssue="요청 한도 초과";
            }
            publish();return;
        }
        if(stage==7){
            const auto result=SolanaReadRpc::parse(SwapOperation::Accounts,h,id);
            const auto object=result.value.toObject();const auto values=object.value("value").toArray();
            if(result.ok&&natural(object.value("context").toObject().value("slot").toString())&&values.size()==batch.size())
                for(int i=0;i<batch.size();++i){
                    const auto mint=batch[i];QString program=mint==UsdcMint?TokenProgram:QString{};
                    for(const auto& c:components)if(c.asset=="solana:"+mint){program=c.id.section(':',0,0);break;}
                    if(SolanaTokenData::supportsBaseUnits(values[i],program,precisions.value(mint,mint==UsdcMint?6:-1)))priceEligible.insert(mint);
                }
            stage=4;message="토큰 이름·심볼 조회 중";emit q->changed();
            transport->send(++token,SolanaTokenData::request(true,batch));return;
        }
        if(stage==4){
            const auto metadata=SolanaTokenData::metadata(h,batch,&error);
            for(auto& c:components)if(batch.contains(c.asset.mid(7))){
                c.metadataIssue=error.isEmpty()?QString("메타데이터 미제공"):error;
                for(const auto& m:metadata)if(c.asset=="solana:"+m.mint&&c.id==m.program+":"+m.mint&&precisions.value(m.mint,-1)==m.decimals){
                    c.displaySymbol=m.symbol;c.displayName=m.name;c.metadataEvidence=m.evidence;c.metadataIssue.clear();break;
                }
            }
            // A metadata error must not suppress an independent price request.
            stage=5;message="Mint별 가격 조회 중";emit q->changed();
            transport->send(++token,SolanaTokenData::request(false,batch));return;
        }
        if(stage==5){
            batchPrices=SolanaTokenData::prices(h,batch,&error);blocks.clear();
            priceReceived=QDateTime::currentMSecsSinceEpoch();
            for(auto it=batchPrices.begin();it!=batchPrices.end();){
                if(!priceEligible.contains(it->mint)||precisions.value(it->mint,it->mint==UsdcMint?6:-1)!=it->decimals)it=batchPrices.erase(it);
                else {if(!blocks.contains(it->block))blocks.append(it->block);++it;}
            }
            for(auto& c:components)if(batch.contains(c.asset.mid(7)))c.priceIssue=error.isEmpty()?QString("가격 미제공"):error;
            for(auto& c:components)if(batch.contains(c.asset.mid(7))&&!priceEligible.contains(c.asset.mid(7)))c.priceIssue="Mint 규격·수량 단위 미확인";
            if(blocks.isEmpty()){lookup();return;}
            stage=6;id=QUuid::createUuid().toString(QUuid::Id128);message="가격 기준 시각 확인 중";emit q->changed();
            transport->send(++token,SolanaTokenData::blockTimesRequest(id,blocks));return;
        }
        const auto times=SolanaTokenData::blockTimes(h,id,blocks,&error);
        for(const auto& p:batchPrices){
            const auto time=times.value(p.block);const auto now=QDateTime::currentMSecsSinceEpoch();
            QString issue=error.isEmpty()?QString("가격 기준 시각 미확인"):error;
            // Keep the block time as the original time. Older than 180 s but within 24 h is a
            // reference price (policy v2); AssetValuation decides that, never this reader.
            if(time>0&&time<=priceReceived&&now-time<=ReferenceMaxAgeMs&&now-priceReceived<=DisplayFreshnessMs){
                quotes.append({"solana:"+p.mint,"USD",p.usd,{"Jupiter Price V3","https://api.jup.ag/price/v3?ids="+p.mint,
                    "public:jupiter:price:"+p.mint,time,priceReceived,true}});
                issue=now-time<=DisplayFreshnessMs?QString{}:QStringLiteral("가격 기준 시각 180초 초과 · 참고 평가");
            }else if(time>0)issue="가격 기준 시각 만료";
            for(auto& c:components)if(c.asset=="solana:"+p.mint)c.priceIssue=issue;
        }
        lookup();
    }
    void result(quint64 n,const HttpResult& h){if(n!=token||!busy||!accountCredentialsAvailable(vault,account.id))return;
        if(stage>=4){enrichment(h);return;}
        const auto op=stage==0?SwapOperation::Genesis:stage==1?SwapOperation::Balance:SwapOperation::TokenAccounts;
        const auto r=SolanaReadRpc::parse(op,h,id);if(!r.ok){fail(r.message);return;}
        if(stage==0){if(r.value.toString()!="5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d"){fail("Solana 메인넷을 확인하지 못했습니다.");return;}
            stage=1;send(SwapOperation::Balance,{address,QJsonObject{{"commitment","confirmed"}}});return;}
        const auto object=r.value.toObject();const auto slot=object.value("context").toObject().value("slot").toString();
        if(!natural(slot)||!decimal::positive(slot)){fail("Solana 조회 슬롯을 확인하지 못했습니다.");return;}
        if(stage==1){const auto amount=object.value("value").toString();if(!natural(amount)){fail("SOL 원본 잔고를 확인하지 못했습니다.");return;}
            received=QDateTime::currentMSecsSinceEpoch();AssetComponent c;c.id="solana:native";c.asset="SOL";c.quantity=units(amount,9);components[c.id]=c;
            stage=2;tokens();return;}
        if(!object.value("value").isArray()||object.value("value").toArray().size()>2047){fail("토큰 보유 조회 형식 또는 수량 상한을 확인하세요.");return;}
        const auto program=stage==2?TokenProgram:Token2022Program;
        for(const auto& value:object.value("value").toArray()){
            const auto row=value.toObject(),accountData=row.value("account").toObject();const auto parsed=accountData.value("data").toObject().value("parsed").toObject();
            const auto info=parsed.value("info").toObject(),amount=info.value("tokenAmount").toObject();
            const auto key=row.value("pubkey").toString(),mint=info.value("mint").toString(),atomic=amount.value("amount").toString(),precision=amount.value("decimals").toString();
            bool ok=false;const int decimals=precision.toInt(&ok);
            if(!pubkey(key)||tokenAccounts.contains(key)||accountData.value("owner").toString()!=program||parsed.value("type").toString()!="account"||
                info.value("owner").toString()!=address||!pubkey(mint)||!natural(atomic)||!ok||QString::number(decimals)!=precision||decimals<0||decimals>255||
                !QStringList{"initialized","frozen"}.contains(info.value("state").toString())){fail("토큰의 소유자·프로그램·mint·기초 수량을 확인하지 못했습니다.");return;}
            tokenAccounts.insert(key);const auto componentId=program+":"+mint;const auto qty=units(atomic,decimals);
            if(precisions.contains(mint)&&precisions.value(mint)!=decimals){fail("같은 Mint의 소수 자릿수가 일치하지 않습니다.");return;}
            precisions.insert(mint,decimals);
            if(components.contains(componentId))components[componentId].quantity=decimal::add(components[componentId].quantity,qty);
            else{AssetComponent c;c.id=componentId;c.asset="solana:"+mint;c.quantity=qty;components[c.id]=c;}
            if(components.size()>2048){fail("자산 항목 상한을 초과했습니다. 완전한 잔고로 표시하지 않습니다.");return;}
        }
        if(stage==2){stage=3;const auto generation=token;QTimer::singleShot(300,q,[this,generation]{if(busy&&token==generation&&accountCredentialsAvailable(vault,account.id))tokens();});return;}
        remaining.clear();for(const auto& c:components)if(c.asset.startsWith("solana:")&&decimal::positive(c.quantity)&&!remaining.contains(c.asset.mid(7)))remaining.append(c.asset.mid(7));
        enrichmentClock.start();
        // The USDC USD quote is a separate conversion leg, never a $1 peg.
        if(!remaining.isEmpty()){remaining.removeAll(UsdcMint);remaining.prepend(UsdcMint);}
        lookup();
    }
};
WalletReadController::WalletReadController(Vault& v,SwapTransport* transport,QObject* parent):QObject(parent),d_(std::make_unique<Private>(this,v,transport?transport:new NetworkSwapTransport(this))){
    connect(d_->transport,&SwapTransport::finished,this,[this](quint64 n,const HttpResult& r){d_->result(n,r);});
    connect(&v,&Vault::aboutToLock,this,&WalletReadController::stop);
    connect(&v,&Vault::accountsChanged,this,&WalletReadController::stop);
}
WalletReadController::~WalletReadController(){stop();}
bool WalletReadController::selectAccount(const QString& id){if(id==d_->account.id&&!id.isEmpty())return true;stop();d_->account={};
    for(const auto& a:d_->vault.accounts())if(a.id==id&&a.provider=="phantom"&&a.market=="solana"&&a.environment=="mainnet")d_->account=a;
    return !d_->account.id.isEmpty();}
bool WalletReadController::refresh(){if(d_->busy)return true;if(!accountCredentialsAvailable(d_->vault,d_->account.id))return false;
    QString error;const bool found=d_->vault.withCredentials(d_->account.id,[this](const CredentialSet& c){const auto it=c.find("address");if(it!=c.end())d_->address=QString::fromUtf8(reinterpret_cast<const char*>(it->second.data()),it->second.size());},&error);
    if(!found||!pubkey(d_->address)){d_->fail("조회용 Solana 주소를 계좌 연결에서 등록하세요. 개인키·시드 문구는 입력하지 않습니다.");return false;}
    d_->components.clear();d_->tokenAccounts.clear();d_->precisions.clear();d_->quotes.clear();d_->stage=0;d_->busy=true;d_->send(SwapOperation::Genesis,{});return true;}
void WalletReadController::stop(){++d_->token;d_->transport->abort();d_->busy=false;d_->address.clear();d_->components.clear();d_->tokenAccounts.clear();d_->precisions.clear();d_->priceEligible.clear();d_->remaining.clear();d_->batch.clear();d_->blocks.clear();d_->batchPrices.clear();d_->quotes.clear();d_->latest={};d_->message="지갑 조회 중지";emit changed();}
bool WalletReadController::busy()const{return d_->busy;}QString WalletReadController::status()const{return d_->message;}
AccountRecord WalletReadController::account()const{return d_->account;}AssetCapture WalletReadController::assetCapture()const{return accountCredentialsAvailable(d_->vault,d_->account.id)?d_->latest:AssetCapture{};}
}
