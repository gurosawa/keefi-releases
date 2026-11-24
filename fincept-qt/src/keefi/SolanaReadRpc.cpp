#include "keefi/SolanaReadRpc.h"
#include "keefi/BuildPolicy.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/ResponseJournal.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QUrlQuery>
#include <iterator>
namespace keefi {
namespace {
constexpr qsizetype MaxReadResponseBytes=4*1024*1024;
QString method(SwapOperation op){switch(op){case SwapOperation::Genesis:return "getGenesisHash";case SwapOperation::Balance:return "getBalance";
case SwapOperation::TokenAccounts:return "getTokenAccountsByOwner";case SwapOperation::Accounts:return "getMultipleAccounts";
case SwapOperation::BlockHeight:return "getBlockHeight";case SwapOperation::Fee:return "getFeeForMessage";
case SwapOperation::SignatureStatus:return "getSignatureStatuses";case SwapOperation::Transaction:return "getTransaction";case SwapOperation::RentMinimum:return "getMinimumBalanceForRentExemption";default:return {};}}
bool keyValid(const QByteArray& key){if(key.isEmpty()||key.size()>512)return false;for(auto c:key)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;return true;}
bool headerAllowed(const SwapRequest& r){for(auto it=r.headers.constBegin();it!=r.headers.constEnd();++it){if(it.key()=="Content-Type"&&it.value()=="application/json")continue;
    if(it.key()=="x-api-key"&&r.url.host()=="api.jup.ag"&&keyValid(it.value()))continue;return false;}return true;}
}
SwapRequest SolanaReadRpc::request(SwapOperation op,const QString& id,const QJsonArray& params,QString* error){if(error)error->clear();
    SwapRequest r;r.operation=op;const auto m=method(op);if(m.isEmpty()||id.isEmpty()||id.size()>64){if(error)*error="허용되지 않은 RPC 조회입니다.";return {};}
    r.url=QUrl("https://api.mainnet-beta.solana.com");r.headers.insert("Content-Type","application/json");
    r.body=QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",m},{"params",params}}).toJson(QJsonDocument::Compact);return r;
}
SwapApiResult SolanaReadRpc::parse(SwapOperation op,const HttpResult& h,const QString& expected){SwapApiResult r;
    auto bad=[&](const QString& code,const QString& msg){r.code=code;r.message=msg;return r;};
    if(h.timedOut||h.networkError)return bad("RPC_NETWORK","Solana 읽기 응답을 확인하지 못했습니다.");
    if(h.status==429){r.rateLimited=true;return bad("RPC_RATE_LIMIT","Solana RPC 요청 한도에 도달했습니다.");}
    if(h.status<200||h.status>=300)return bad("RPC_HTTP","Solana RPC 조회에 실패했습니다.");
    QString e;const auto d=parseExactJson(h.body,&e);if(!e.isEmpty()||!d.isObject())return bad("RPC_JSON","RPC 응답 형식이 올바르지 않습니다.");const auto o=d.object();
    if(o.value("jsonrpc").toString()!="2.0"||o.value("id").toString()!=expected||o.contains("error")||!o.contains("result"))return bad("RPC_RESULT","요청과 일치하는 RPC 결과를 확인하지 못했습니다.");
    r.value=o.value("result");

    r.ok=true;return r;
}
struct NetworkSwapTransport::Private {QNetworkAccessManager manager;QPointer<QNetworkReply> reply;QByteArray bytes;QTimer timer;quint64 token=0;bool timedOut=false,oversized=false;};
NetworkSwapTransport::NetworkSwapTransport(QObject* p):SwapTransport(p),d(std::make_unique<Private>()){
    d->timer.setSingleShot(true);connect(&d->timer,&QTimer::timeout,this,[this]{if(d->reply){d->timedOut=true;d->reply->abort();}});
}
NetworkSwapTransport::~NetworkSwapTransport(){abort();}
bool NetworkSwapTransport::allowed(const SwapRequest& r){
    {
        switch (r.operation) {
        case SwapOperation::Genesis: case SwapOperation::Balance: case SwapOperation::TokenAccounts:
        case SwapOperation::TokenMetadata: case SwapOperation::TokenPrices: case SwapOperation::BlockTimes:
        case SwapOperation::Accounts: case SwapOperation::BlockHeight: case SwapOperation::Fee:
        case SwapOperation::SignatureStatus: case SwapOperation::Transaction: case SwapOperation::RentMinimum:
            break;
        default: return false;
        }
    }
    if(r.url.scheme()!="https"||r.url.port(443)!=443||!r.url.userInfo().isEmpty()||!r.url.fragment().isEmpty()||!headerAllowed(r)||r.body.size()>128*1024)return false;
    if(r.operation==SwapOperation::TokenMetadata||r.operation==SwapOperation::TokenPrices){
        const bool metadata=r.operation==SwapOperation::TokenMetadata;
        if(r.method!="GET"||r.url.host()!="api.jup.ag"||r.url.path()!=(metadata?"/tokens/v2/search":"/price/v3")||!r.body.isEmpty())return false;
        const auto items=QUrlQuery(r.url).queryItems(QUrl::FullyDecoded);
        if(items.size()!=1||items.first().first!=(metadata?"query":"ids"))return false;
        const auto mints=items.first().second.split(',');QSet<QString> seen;
        if(mints.isEmpty()||mints.size()>50)return false;
        for(const auto& mint:mints){if(!SolanaTransactionCodec::publicKeyValid(mint)||seen.contains(mint))return false;seen.insert(mint);}return true;
    }
    if(r.operation==SwapOperation::BlockTimes){
        if(r.method!="POST"||r.url.host()!="api.mainnet-beta.solana.com"||(r.url.path()!=""&&r.url.path()!="/")||
            !r.url.query().isEmpty()||r.headers.contains("x-api-key"))return false;
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(r.body,&error);
        if(error.error!=QJsonParseError::NoError||!doc.isArray()||doc.array().isEmpty()||doc.array().size()>50)return false;
        QSet<QString> ids;
        for(const auto& value:doc.array()){
            const auto o=value.toObject();const auto params=o.value("params").toArray();const auto id=o.value("id").toString();
            if(o.size()!=4||o.value("jsonrpc").toString()!="2.0"||o.value("method").toString()!="getBlockTime"||id.isEmpty()||id.size()>96||ids.contains(id)||
                params.size()!=1||!params[0].isDouble()||params[0].toDouble()<1||params[0].toDouble()>9007199254740991.0||params[0].toDouble()!=double(params[0].toInteger()))return false;
            ids.insert(id);
        }return true;
    }

    if(r.method!="POST"||!r.url.query().isEmpty())return false;
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(r.body,&error);if(error.error!=QJsonParseError::NoError||!doc.isObject())return false;const auto o=doc.object();

    if(r.url.host()!="api.mainnet-beta.solana.com"||(r.url.path()!=""&&r.url.path()!="/")||r.headers.contains("x-api-key")||o.size()!=4||o.value("jsonrpc").toString()!="2.0"||
        o.value("id").toString().isEmpty()||o.value("id").toString().size()>64||o.value("method").toString()!=method(r.operation)||method(r.operation).isEmpty()||!o.value("params").isArray())return false;
    const auto p=o.value("params").toArray();
    switch(r.operation){
    case SwapOperation::Genesis:return p.isEmpty();
    case SwapOperation::Balance:return p.size()==2&&SolanaTransactionCodec::publicKeyValid(p[0].toString())&&p[1].isObject();
    case SwapOperation::TokenAccounts:return p.size()==3&&SolanaTransactionCodec::publicKeyValid(p[0].toString())&&p[1].isObject()&&p[2].isObject();
    case SwapOperation::Accounts:return p.size()==2&&p[0].isArray()&&p[0].toArray().size()<=100&&p[1].isObject();
    case SwapOperation::BlockHeight:return p.size()==1&&p[0].isObject();
    case SwapOperation::Fee:return p.size()==2&&p[0].isString()&&p[0].toString().size()<=2048&&p[1].isObject();
    case SwapOperation::SignatureStatus:return p.size()==2&&p[0].isArray()&&p[0].toArray().size()==1&&SolanaTransactionCodec::decodeBase58(p[0].toArray()[0].toString(),64).size()==64&&p[1].toObject().value("searchTransactionHistory").toBool();
    case SwapOperation::Transaction:return p.size()==2&&SolanaTransactionCodec::decodeBase58(p[0].toString(),64).size()==64&&p[1].isObject();
    case SwapOperation::RentMinimum:return p.size()==2&&p[0].isDouble()&&p[0].toInt(-1)>=0&&p[0].toInt()>0&&p[0].toInt()<=10240&&p[1].isObject();
    default:return false;}
}
namespace {
QString swapOperationName(SwapOperation op){
    static const char* names[]={"Order","Execute","Genesis","Balance","TokenAccounts","Accounts","BlockHeight","Fee","Simulate",
        "SignatureStatus","Transaction","RentMinimum","TokenMetadata","TokenPrices","BlockTimes"};
    const auto index=int(op);return index>=0&&index<int(std::size(names))?QString::fromLatin1(names[index]):QString::number(index);
}
}
bool NetworkSwapTransport::readResponseChunk(QIODevice& input,QByteArray& bytes){
    if(bytes.size()>MaxReadResponseBytes)return false;
    while(input.bytesAvailable()>0&&bytes.size()<=MaxReadResponseBytes){
        const auto chunk=input.read(qMin<qint64>(input.bytesAvailable(),MaxReadResponseBytes-bytes.size()+1));
        if(chunk.isEmpty())break;
        bytes+=chunk;
    }
    return bytes.size()<=MaxReadResponseBytes;
}
QNetworkRequest NetworkSwapTransport::requestPolicy(const SwapRequest& r){
    QNetworkRequest req(r.url);req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute,false);
    req.setRawHeader("Accept","application/json");for(auto it=r.headers.constBegin();it!=r.headers.constEnd();++it)req.setRawHeader(it.key(),it.value());
    return req;
}
void NetworkSwapTransport::send(quint64 token,const SwapRequest& r){
    if(d->reply||!allowed(r)){QTimer::singleShot(0,this,[this,token]{HttpResult h;h.networkError=true;emit finished(token,h);});return;}
    d->token=token;d->bytes.clear();d->timedOut=false;d->oversized=false;auto req=requestPolicy(r);
    d->reply=r.method=="GET"?d->manager.get(req):d->manager.post(req,r.body);auto* reply=d->reply.data();
    reply->setReadBufferSize(MaxReadResponseBytes+1);
    connect(reply,&QNetworkReply::readyRead,this,[this,reply]{if(d->reply!=reply)return;if(!readResponseChunk(*reply,d->bytes)){d->oversized=true;reply->abort();}});
    connect(reply,&QNetworkReply::finished,this,[this,reply,operation=r.operation,method=r.method,url=r.url]{if(d->reply!=reply){reply->deleteLater();return;}d->timer.stop();if(!readResponseChunk(*reply,d->bytes))d->oversized=true;HttpResult h;
        h.status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();h.timedOut=d->timedOut;h.networkError=d->oversized||d->bytes.size()>MaxReadResponseBytes||(h.status==0&&reply->error()!=QNetworkReply::NoError);
        if(!d->oversized&&d->bytes.size()<=MaxReadResponseBytes)h.body=d->bytes;const auto token=d->token;d->reply=nullptr;d->bytes.clear();reply->deleteLater();
        ResponseJournal::record("solana",swapOperationName(operation),method,url,false,h);emit finished(token,h);});
    const bool tokenData=r.operation==SwapOperation::TokenMetadata||r.operation==SwapOperation::TokenPrices||r.operation==SwapOperation::BlockTimes;
    d->timer.start(tokenData?8000:20000);
}
void NetworkSwapTransport::abort(){d->timer.stop();if(d->reply){auto* reply=d->reply.data();d->reply=nullptr;reply->disconnect(this);reply->abort();reply->deleteLater();}d->bytes.clear();}
}
