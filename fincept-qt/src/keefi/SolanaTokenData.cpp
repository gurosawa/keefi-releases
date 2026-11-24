#include "keefi/SolanaTokenData.h"
#include "keefi/SolanaTransactionCodec.h"
#include <QDateTime>
#include <QSet>
#include <QUrlQuery>

namespace keefi {
namespace {
QString failure(const HttpResult& h) {
    if(h.timedOut)return "응답 시간 초과";
    if(h.networkError)return "네트워크 연결 실패";
    if(h.status==429)return "요청 한도 초과";
    if(h.status==401||h.status==403)return "제공처 인증 필요";
    if(h.status<200||h.status>=300)return QString("HTTP %1").arg(h.status);
    return {};
}
QJsonDocument document(const HttpResult& h,QString* error) {
    *error=failure(h);if(!error->isEmpty())return {};
    return parseExactJson(h.body,error);
}
int precision(const QJsonValue& value) {
    bool ok=false;const auto s=value.toString();const auto n=s.toInt(&ok);
    return ok&&QString::number(n)==s&&n>=0&&n<=255?n:-1;
}
bool blockValid(const QString& s) {
    bool ok=false;const auto n=s.toULongLong(&ok);
    return ok&&n>0&&n<=9007199254740991ULL&&QString::number(n)==s;
}
}
bool SolanaTokenData::displayTextValid(const QString& s,int maximum) {
    if(s.isEmpty()||s.size()>maximum||s!=s.trimmed())return false;
    for(const auto c:s)if(c.isNull()||c.category()==QChar::Other_Control||c.category()==QChar::Other_Format||
                         c.category()==QChar::Separator_Line||c.category()==QChar::Separator_Paragraph)return false;
    return true;
}
bool SolanaTokenData::supportsBaseUnits(const QJsonValue& value,const QString& program,int decimals) {
    const auto account=value.toObject(),data=account.value("data").toObject().value("parsed").toObject();
    const auto info=data.value("info").toObject();
    if(account.value("owner").toString()!=program||account.value("executable").toBool()||data.value("type").toString()!="mint"||
        !info.value("isInitialized").toBool()||precision(info.value("decimals"))!=decimals)return false;
    const auto extensions=info.value("extensions");
    if(!extensions.isUndefined()&&!extensions.isArray())return false;
    for(const auto& extension:extensions.toArray()){
        const auto o=extension.toObject();const auto name=o.value("extension").toString();
        // Balances intentionally retain raw units / 10^decimals. Do not multiply
        // a display-unit price by a different raw-unit balance.
        if(name=="interestBearingConfig")return false;
        if(name=="scaledUiAmountConfig"){
            const auto state=o.value("state").toObject();
            if(!decimal::valid(state.value("multiplier").toString())||!decimal::valid(state.value("newMultiplier").toString())||
                decimal::normalize(state.value("multiplier").toString())!="1"||decimal::normalize(state.value("newMultiplier").toString())!="1")return false;
        }
    }
    return true;
}
SwapRequest SolanaTokenData::request(bool metadata,const QStringList& mints) {
    SwapRequest r;r.operation=metadata?SwapOperation::TokenMetadata:SwapOperation::TokenPrices;r.method="GET";
    r.url=QUrl(metadata?"https://api.jup.ag/tokens/v2/search":"https://api.jup.ag/price/v3");
    QUrlQuery query;query.addQueryItem(metadata?"query":"ids",mints.join(','));r.url.setQuery(query);return r;
}
SwapRequest SolanaTokenData::blockTimesRequest(const QString& id,const QStringList& blocks) {
    SwapRequest r;r.operation=SwapOperation::BlockTimes;r.url=QUrl("https://api.mainnet-beta.solana.com");
    r.headers.insert("Content-Type","application/json");QJsonArray batch;
    for(const auto& block:blocks)batch.append(QJsonObject{{"jsonrpc","2.0"},{"id",id+":"+block},{"method","getBlockTime"},
        {"params",QJsonArray{double(block.toULongLong())}}});
    r.body=QJsonDocument(batch).toJson(QJsonDocument::Compact);return r;
}
QList<SolanaTokenMetadata> SolanaTokenData::metadata(const HttpResult& h,const QStringList& mints,QString* error) {
    QList<SolanaTokenMetadata> out;const auto doc=document(h,error);if(!error->isEmpty())return out;
    if(!doc.isArray()||doc.array().size()>100){*error="메타데이터 응답 형식 오류";return out;}
    QSet<QString> seen;
    for(const auto& value:doc.array()){
        const auto o=value.toObject();const auto mint=o.value("id").toString();
        if(!mints.contains(mint))continue;
        if(seen.contains(mint)){*error="중복 Mint 메타데이터";return {};}seen.insert(mint);
        const auto symbol=o.value("symbol").toString(),name=o.value("name").toString(),program=o.value("tokenProgram").toString();
        const int decimals=precision(o.value("decimals"));
        if(!displayTextValid(symbol,32)||!displayTextValid(name,160)||decimals<0||
            (program!="TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"&&program!="TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"))continue;
        out.append({mint,symbol,name,program,decimals,{"Jupiter Tokens V2","https://api.jup.ag/tokens/v2/search",
            "public:jupiter:metadata:"+mint,0,QDateTime::currentMSecsSinceEpoch(),true}});
    }
    return out;
}
QList<SolanaTokenPrice> SolanaTokenData::prices(const HttpResult& h,const QStringList& mints,QString* error) {
    QList<SolanaTokenPrice> out;const auto doc=document(h,error);if(!error->isEmpty())return out;
    if(!doc.isObject()){*error="가격 응답 형식 오류";return out;}
    const auto object=doc.object();
    for(const auto& mint:mints){
        const auto o=object.value(mint).toObject();const auto price=o.value("usdPrice").toString(),block=o.value("blockId").toString();
        const int decimals=precision(o.value("decimals"));
        // null, zero and a missing mint are unknown, never a zero valuation.
        if(!decimal::positive(price)||!blockValid(block)||decimals<0)continue;
        out.append({mint,price,block,decimals});
    }
    return out;
}
QMap<QString,qint64> SolanaTokenData::blockTimes(const HttpResult& h,const QString& id,const QStringList& blocks,QString* error) {
    QMap<QString,qint64> out;const auto doc=document(h,error);if(!error->isEmpty())return out;
    if(!doc.isArray()||doc.array().size()>50){*error="가격 시각 응답 형식 오류";return out;}
    QSet<QString> seen;
    for(const auto& value:doc.array()){
        const auto o=value.toObject();const auto responseId=o.value("id").toString();
        if(!responseId.startsWith(id+":"))continue;
        const auto block=responseId.mid(id.size()+1);
        if(!blocks.contains(block))continue;
        if(seen.contains(block)){*error="중복 가격 시각 응답";return {};}seen.insert(block);
        bool ok=false;const auto seconds=o.value("result").toString().toLongLong(&ok);
        const auto now=QDateTime::currentSecsSinceEpoch();
        if(o.value("jsonrpc").toString()=="2.0"&&!o.contains("error")&&ok&&seconds>0&&seconds<=now)out.insert(block,seconds*1000);
    }
    return out;
}
QString solanaPriceRatio(const QString& numerator,const QString& denominator) {
    if(!decimal::valid(numerator)||!decimal::positive(denominator))return {};
    auto a=decimal::normalize(numerator),b=decimal::normalize(denominator);
    const int ap=a.contains('.')?a.size()-a.indexOf('.')-1:0,bp=b.contains('.')?b.size()-b.indexOf('.')-1:0;
    a.remove('.');b.remove('.');a+=QString(qMax(0,bp-ap),'0');b+=QString(qMax(0,ap-bp),'0');
    QString result,remainder="0";const int whole=a.size();a+=QString(24,'0');
    for(int i=0;i<a.size();++i){
        remainder=decimal::normalize(remainder+a[i]);int digit=0;
        while(decimal::compare(remainder,b)>=0){remainder=decimal::subtract(remainder,b);++digit;}
        if(i==whole)result+='.';result+=QChar('0'+digit);
    }
    return decimal::normalize(result);
}
}
