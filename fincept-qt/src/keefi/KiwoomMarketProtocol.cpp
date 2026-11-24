#include "keefi/KiwoomMarketProtocol.h"
#include <QJsonArray>
#include <QDate>
#include <QTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
namespace keefi {
namespace {
bool matches(const QString& s,const char* p){return QRegularExpression(QString::fromLatin1(p)).match(s).hasMatch();}
bool country(const QString& s){return s=="KR"||s=="US";}
bool code(const QString& c,const QString& s){return c=="KR"?matches(s,"\\A[A-Z0-9]{6}\\z"):c=="US"&&matches(s,"\\A[A-Z]{1,8}(?:[.-][A-Z])?\\z");}
bool exchange(const QString& c,const QString& s){return c=="KR"?s=="KRX":c=="US"&&QStringList{"ND","NY","NA"}.contains(s);}
QString identityKey(const WatchlistIdentity& i){return i.market+':'+i.source+':'+i.code;}
QString binding(const WatchlistIdentity& i,const QString& e){return identityKey(i)+':'+e;}
bool cursorValid(const QByteArray& s){if(s.isEmpty()||s.size()>50)return false;for(const char ch:s)if(ch<0x21||ch>0x7e)return false;return true;}
QByteArray header(const HttpResult& r,const QByteArray& key,bool* unique=nullptr){QByteArray value;bool seen=false;for(auto it=r.headers.cbegin();it!=r.headers.cend();++it)if(it.key().compare(key,Qt::CaseInsensitive)==0){if(seen){if(unique)*unique=false;return {};}seen=true;value=it.value();}if(unique)*unique=true;return value;}
QString api(const KiwoomReadIntent& i){switch(i.read){case KiwoomDisplayRequest::KrCatalog:return "ka10099";case KiwoomDisplayRequest::KrMetadata:return "ka10100";case KiwoomDisplayRequest::UsCatalog:return "usa10099";case KiwoomDisplayRequest::UsMetadata:return "usa10098";case KiwoomDisplayRequest::KrPrice:return "ka10001";case KiwoomDisplayRequest::UsPrice:return "usa20100";case KiwoomDisplayRequest::KrDaily:return "ka10081";}return {};}
bool priceRead(const KiwoomReadIntent& i){return i.read==KiwoomDisplayRequest::KrPrice||i.read==KiwoomDisplayRequest::UsPrice;}
bool validIntent(const KiwoomReadIntent& i){
    if(!country(i.country)||(!i.cursor.isEmpty()&&(!cursorValid(i.cursor)||priceRead(i)||i.read==KiwoomDisplayRequest::KrDaily)))return false;
    if(i.read!=KiwoomDisplayRequest::KrDaily&&!i.baseDate.isEmpty())return false;
    switch(i.read){
    case KiwoomDisplayRequest::KrCatalog:return i.country=="KR"&&i.symbol.isEmpty()&&i.exchange.isEmpty()&&QStringList{"0","10","30","50","60","70","80","90","2","3","4","5","6","7","8","9"}.contains(i.catalogMarket);
    case KiwoomDisplayRequest::KrMetadata:case KiwoomDisplayRequest::KrPrice:return i.country=="KR"&&code("KR",i.symbol)&&(i.exchange.isEmpty()||i.exchange=="KRX")&&i.catalogMarket.isEmpty();
    case KiwoomDisplayRequest::UsCatalog:return i.country=="US"&&i.symbol.isEmpty()&&(i.exchange=="%"||exchange("US",i.exchange))&&i.catalogMarket.isEmpty();
    case KiwoomDisplayRequest::UsMetadata:return i.country=="US"&&code("US",i.symbol)&&(i.exchange.isEmpty()||exchange("US",i.exchange))&&i.catalogMarket.isEmpty();
    case KiwoomDisplayRequest::UsPrice:return i.country=="US"&&code("US",i.symbol)&&exchange("US",i.exchange)&&i.catalogMarket.isEmpty();
    case KiwoomDisplayRequest::KrDaily:return i.country=="KR"&&code("KR",i.symbol)&&i.exchange.isEmpty()&&i.catalogMarket.isEmpty()&&
        matches(i.baseDate,"\\A[0-9]{8}\\z")&&QDate::fromString(i.baseDate,"yyyyMMdd").isValid();
    }return false;
}
QString path(const KiwoomReadIntent& i){return i.read==KiwoomDisplayRequest::KrDaily?QStringLiteral("/api/dostk/chart"):i.country=="KR"?QStringLiteral("/api/dostk/stkinfo"):i.read==KiwoomDisplayRequest::UsPrice?QStringLiteral("/api/us/mrkcond"):QStringLiteral("/api/us/stkinfo");}
QString normalized(QString s){
    const auto point=s.indexOf('.');QString whole=point<0?s:s.left(point),fraction=point<0?QString():s.mid(point+1);
    while(whole.size()>1&&whole.front()=='0')whole.remove(0,1);while(fraction.endsWith('0'))fraction.chop(1);return whole+(fraction.isEmpty()?QString():'.'+fraction);
}
bool number(const QString& raw,QString* out,bool allowSign,bool allowNegative,int decimals=-1){
    if(raw.isEmpty()||raw.size()>512||!matches(raw,"\\A[+-]?[0-9]+(?:\\.[0-9]+)?\\z"))return false;
    QString s=raw;if(s.front()=='+'||s.front()=='-'){if(!allowSign||(!allowNegative&&s.front()=='-'))return false;s.remove(0,1);}
    if(decimals>=0){const auto dot=s.indexOf('.');if(dot>=0&&s.size()-dot-1>decimals)return false;}
    s=normalized(s);const auto dot=s.indexOf('.');const auto whole=dot<0?s:s.left(dot);
    if(whole.size()>101||(whole.size()==101&&(whole!=QString('1')+QString(100,'0')||(dot>=0))))return false;
    if(out)*out=s;return true;
}
bool price(const QString& raw,QString* out,const QString& c,bool realtime=false){
    // FE has no documented minus-sign convention. Do not borrow the KR sign meaning.
    if(!number(raw,out,true,c=="KR"||!realtime,c=="KR"?0:4)||*out=="0")return false;
    const auto dot=out->indexOf('.');if(out->startsWith("0.")&&dot>=0){const auto first=out->indexOf(QRegularExpression("[1-9]"),2);if(first<0||first>101)return false;}return true;
}
void change(const QString& raw,KiwoomMarketQuote& q,const AssetEvidence& evidence){
    QString value;if(!number(raw,&value,true,true,2))return;const bool negative=raw.startsWith('-');q.direction=value=="0"?MovementDirection::Flat:negative?MovementDirection::Down:MovementDirection::Up;
    q.changeText=(value=="0"?QString():negative?QStringLiteral("-"):QStringLiteral("+"))+value+'%';q.changeEvidence=evidence;
}
QString returnCode(const QJsonObject& o){const auto v=o.value("return_code");if(v.isString()&&matches(v.toString(),"\\A-?[0-9]{1,8}\\z"))return v.toString();if(v.isDouble()&&v.toDouble()==0)return "0";return {};}
bool envelope(const KiwoomReadIntent& i,const HttpResult& r,QJsonObject* out,bool* auth,bool* rate){
    *auth=r.status==401||r.status==403;*rate=r.status==429||r.status==418||r.localRetryAfterSeconds>0;
    if(*auth||*rate||r.status!=200||r.timedOut||r.networkError||r.body.size()>KiwoomMarketProtocol::MaxRestBytes||!validIntent(i))return false;
    bool unique=false;const auto id=header(r,"api-id",&unique);if(!unique||id!=api(i).toLatin1())return false;
    const auto document=parseExactJson(r.body);if(!document.isObject())return false;const auto o=document.object();const auto result=returnCode(o);
    *rate=result=="1700"||result=="1701"||result=="1702";if(result!="0")return false;*out=o;return true;
}
AssetEvidence evidence(const KiwoomReadIntent& i,const QString& symbol,const QString& venue,qint64 at){return {"키움 공식 공개 시세",QStringLiteral("https://api.kiwoom.com")+path(i),"public:kiwoom:"+i.country+':'+symbol+':'+venue,0,at,true};}
bool proofUrl(const QString& text,const QString& path){const QUrl u(text);return u.isValid()&&u.scheme()=="https"&&u.host()=="api.kiwoom.com"&&u.port(443)==443&&u.userInfo().isEmpty()&&!u.hasQuery()&&!u.hasFragment()&&u.path()==path;}
bool proof(const KiwoomVerifiedInstrument& i,qint64 at){const bool us=i.identity.source=="US";return i.verified&&i.identity.market=="stock"&&code(i.identity.source,i.identity.code)&&exchange(i.identity.source,i.exchange)&&i.currency==(us?"USD":"KRW")&&i.metadataEvidence.complete&&i.currencyEvidence.complete&&i.metadataEvidence.sourceTimeMs==0&&i.currencyEvidence.sourceTimeMs==0&&i.metadataEvidence.receivedAtMs>0&&i.currencyEvidence.receivedAtMs>0&&i.metadataEvidence.receivedAtMs<=at&&i.currencyEvidence.receivedAtMs<=at&&at-i.metadataEvidence.receivedAtMs<300000&&at-i.currencyEvidence.receivedAtMs<300000&&
    (us?QStringList{"usa10098","usa10099"}:QStringList{"ka10100","ka10099"}).contains(i.metadataApiId)&&i.currencyApiId==(us?"usa20100":"ka10001")&&proofUrl(i.metadataEvidence.url,us?"/api/us/stkinfo":"/api/dostk/stkinfo")&&proofUrl(i.currencyEvidence.url,us?"/api/us/mrkcond":"/api/dostk/stkinfo");}
}
RequestSpec KiwoomMarketProtocol::request(const KiwoomReadIntent& i){
    if(!validIntent(i))return {};RequestSpec r;r.operation=Operation::Reference;r.method="POST";r.authenticated=true;r.url=QUrl("https://api.kiwoom.com"+path(i));r.headers={{"Content-Type","application/json;charset=UTF-8"},{"api-id",api(i).toLatin1()}};QJsonObject body;
    if(i.read==KiwoomDisplayRequest::KrCatalog)body["mrkt_tp"]=i.catalogMarket;
    else if(i.read==KiwoomDisplayRequest::UsCatalog)body["stex_tp"]=i.exchange;
    else{body["stk_cd"]=i.symbol;if(i.read==KiwoomDisplayRequest::UsPrice)body["stex_tp"]=i.exchange;
        if(i.read==KiwoomDisplayRequest::KrDaily){body["base_dt"]=i.baseDate;body["upd_stkpc_tp"]="1";}} // Source-adjusted prices only.
    if(!i.cursor.isEmpty()){r.headers["cont-yn"]="Y";r.headers["next-key"]=i.cursor;}r.body=QJsonDocument(body).toJson(QJsonDocument::Compact);return r;
}
bool KiwoomMarketProtocol::isRequest(const RequestSpec& r){
    if(r.operation!=Operation::Reference||!r.authenticated||r.method!="POST"||r.url.scheme()!="https"||r.url.host()!="api.kiwoom.com"||r.url.port(443)!=443||!r.url.userInfo().isEmpty()||r.url.hasQuery()||r.url.hasFragment()||r.body.size()>1024)return false;
    QMap<QByteArray,QByteArray> h;for(auto it=r.headers.cbegin();it!=r.headers.cend();++it){const auto name=it.key().toLower();if(h.contains(name)||!QSet<QByteArray>{"content-type","api-id","authorization","cont-yn","next-key"}.contains(name))return false;h[name]=it.value();}
    if(h.value("content-type")!="application/json;charset=UTF-8"||(!h.value("authorization").isEmpty()&&!matches(QString::fromLatin1(h.value("authorization")),"\\ABearer [A-Za-z0-9._~+/=-]{1,1000}\\z"))||(h.contains("authorization")&&h.value("authorization").isEmpty()))return false;
    const auto document=parseExactJson(r.body);if(!document.isObject())return false;const auto body=document.object();KiwoomReadIntent i;const auto id=h.value("api-id");
    if(id=="ka10099"){i.read=KiwoomDisplayRequest::KrCatalog;i.country="KR";i.catalogMarket=body.value("mrkt_tp").toString();}
    else if(id=="ka10100"||id=="ka10001"){i.read=id=="ka10100"?KiwoomDisplayRequest::KrMetadata:KiwoomDisplayRequest::KrPrice;i.country="KR";i.symbol=body.value("stk_cd").toString();}
    else if(id=="ka10081"){i.read=KiwoomDisplayRequest::KrDaily;i.country="KR";i.symbol=body.value("stk_cd").toString();i.baseDate=body.value("base_dt").toString();}
    else if(id=="usa10099"){i.read=KiwoomDisplayRequest::UsCatalog;i.country="US";i.exchange=body.value("stex_tp").toString();}
    else if(id=="usa10098"){i.read=KiwoomDisplayRequest::UsMetadata;i.country="US";i.symbol=body.value("stk_cd").toString();}
    else if(id=="usa20100"){i.read=KiwoomDisplayRequest::UsPrice;i.country="US";i.symbol=body.value("stk_cd").toString();i.exchange=body.value("stex_tp").toString();}else return false;
    if(h.contains("cont-yn")||h.contains("next-key")){if(h.value("cont-yn")!="Y"||!cursorValid(h.value("next-key")))return false;i.cursor=h.value("next-key");}
    const auto expected=request(i);return !expected.url.isEmpty()&&expected.url==r.url&&QJsonDocument::fromJson(expected.body).object()==body;
}
KiwoomMetadataPage KiwoomMarketProtocol::parseMetadata(const KiwoomReadIntent& i,const HttpResult& r,qint64 at){
    KiwoomMetadataPage result;result.reason="키움 공개 종목 정보 확인 실패";QJsonObject body;if(at<=0||priceRead(i)||!envelope(i,r,&body,&result.authenticationFailed,&result.rateLimited))return result;
    bool unique=false;const auto continued=header(r,"cont-yn",&unique);if(!unique||(!continued.isEmpty()&&continued!="Y"&&continued!="N"))return result;
    result.continued=continued=="Y";if(result.continued){result.nextCursor=header(r,"next-key",&unique);if(!unique||!cursorValid(result.nextCursor))return result;}
    QJsonArray rows;if(i.read==KiwoomDisplayRequest::KrMetadata)rows.append(body);else{if(!body.value("list").isArray())return result;rows=body.value("list").toArray();}if(rows.size()>10000)return result;QSet<QString> seen;
    for(const auto& value:rows){if(!value.isObject())return result;const auto row=value.toObject();KiwoomVerifiedInstrument instrument;const auto symbol=row.value(i.country=="KR"?"code":"stk_cd").toString(),venue=i.country=="KR"?QStringLiteral("KRX"):row.value("stex_tp").toString();
        if(!code(i.country,symbol)||!exchange(i.country,venue))return result;if((i.read==KiwoomDisplayRequest::KrMetadata||i.read==KiwoomDisplayRequest::UsMetadata)&&symbol!=i.symbol)return result;
        if(i.country=="US"&&!i.exchange.isEmpty()&&i.exchange!="%"&&venue!=i.exchange)continue;
        instrument.identity={"stock",i.country,symbol};instrument.exchange=venue;instrument.metadataApiId=api(i);instrument.metadataEvidence=evidence(i,symbol,venue,at);instrument.metadataEvidence.complete=!result.continued;instrument.nxtEligible=i.country=="KR"&&row.value("nxtEnable").toString()=="Y";
        const auto key=binding(instrument.identity,venue);if(seen.contains(key))return result;seen.insert(key);result.instruments.append(instrument);
    }result.available=true;result.reason.clear();return result;
}
KiwoomDailyPage KiwoomMarketProtocol::parseDaily(const KiwoomReadIntent& i,const HttpResult& r,qint64 at){
    KiwoomDailyPage result;result.reason="키움 일봉 확인 실패";QJsonObject body;
    if(at<=0||i.read!=KiwoomDisplayRequest::KrDaily||!envelope(i,r,&body,&result.authenticationFailed,&result.rateLimited))return result;
    if(body.contains("stk_cd")&&body.value("stk_cd").toString()!=i.symbol)return result;
    const auto list=body.value("stk_dt_pole_chart_qry");if(!list.isArray()||list.toArray().size()>1000)return result;
    const auto base=QDate::fromString(i.baseDate,"yyyyMMdd");QSet<QDate> seen;QList<KiwoomDailyRow> rows;
    for(const auto& value:list.toArray()){
        if(!value.isObject())return result;const auto o=value.toObject();KiwoomDailyRow row;const auto day=o.value("dt").toString();row.date=QDate::fromString(day,"yyyyMMdd");
        if(!matches(day,"\\A[0-9]{8}\\z")||!row.date.isValid()||row.date>base||seen.contains(row.date))return result;seen.insert(row.date);
        // KR price fields carry a direction sign; the magnitude is the price.
        if(!price(o.value("open_pric").toString(),&row.open,"KR")||!price(o.value("high_pric").toString(),&row.high,"KR")||!price(o.value("low_pric").toString(),&row.low,"KR")||
           !price(o.value("cur_prc").toString(),&row.close,"KR")||!number(o.value("trde_qty").toString(),&row.volume,false,false,0))return result;
        if(decimal::compare(row.low,row.open)>0||decimal::compare(row.low,row.close)>0||decimal::compare(row.high,row.open)<0||decimal::compare(row.high,row.close)<0)return result;
        rows.append(row);
    }
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.date<b.date;});
    result.rows=rows;result.available=true;result.reason.clear();return result;
}
KiwoomRestPrice KiwoomMarketProtocol::parseRestPrice(const KiwoomReadIntent& i,const HttpResult& r,qint64 at){
    KiwoomRestPrice result;result.reason="키움 공개 현재가 확인 실패";QJsonObject body;if(at<=0||!priceRead(i)||!envelope(i,r,&body,&result.authenticationFailed,&result.rateLimited))return result;
    if(body.value("stk_cd").toString()!=i.symbol||(i.country=="US"&&(body.value("stex_tp").toString()!=i.exchange||body.value("curr_unit").toString()!="USD")))return result;
    KiwoomMarketQuote q;q.identity={"stock",i.country,i.symbol};q.exchange=i.country=="KR"?"KRX":i.exchange;q.currency=i.country=="US"?"USD":"KRW";q.rawSignedPrice=body.value("cur_prc").toString();if(!price(q.rawSignedPrice,&q.price,i.country))return result;q.evidence=evidence(i,i.symbol,q.exchange,at);change(body.value("flu_rt").toString(),q,q.evidence);
    result.available=true;result.reason.clear();result.quote=q;result.currencyEvidence=q.evidence;result.currencyApiId=api(i);return result;
}
QByteArray KiwoomMarketProtocol::subscription(const QString& c,const QList<KiwoomMarketTarget>& rows,bool remove){
    if(!country(c)||rows.isEmpty()||rows.size()>200)return {};QJsonArray items;QSet<QString> seen;for(const auto& row:rows){if(row.identity.market!="stock"||row.identity.source!=c||!code(c,row.identity.code)||!exchange(c,row.exchange)||row.priority<0||row.priority>3)return {};const auto key=binding(row.identity,row.exchange);if(seen.contains(key))return {};seen.insert(key);if(c=="US")items.append(QJsonObject{{"jmcode",row.identity.code},{"stex_tp",row.exchange}});else items.append(row.identity.code);}
    QJsonObject packet{{"trnm",remove?"REMOVE":"REG"},{"grp_no","1"},{"data",QJsonArray{QJsonObject{{"item",items},{"type",QJsonArray{c=="US"?"FE":"0B"}}}}}};if(!remove)packet["refresh"]="0";return QJsonDocument(packet).toJson(QJsonDocument::Compact);
}
KiwoomControlAck KiwoomMarketProtocol::controlAck(const QByteArray& bytes,const QString& expected){
    KiwoomControlAck a;if(bytes.size()>MaxFrameBytes||!QStringList{"LOGIN","REG","REMOVE"}.contains(expected))return a;const auto doc=parseExactJson(bytes);if(!doc.isObject())return a;const auto o=doc.object();if(o.value("trnm").toString()!=expected)return a;const auto code=returnCode(o);if(code.isEmpty())return a;a.available=true;a.command=expected;a.accepted=code=="0";a.authenticationFailed=expected=="LOGIN"&&!a.accepted;return a;
}
std::optional<QByteArray> KiwoomMarketProtocol::pingEcho(const QByteArray& bytes){if(bytes.size()>MaxFrameBytes)return {};if(bytes=="PING")return bytes;const auto d=parseExactJson(bytes);if(d.isObject()&&d.object().value("trnm").toString()=="PING")return bytes;return {};}
QList<KiwoomMarketQuote> KiwoomMarketProtocol::realtimeQuotes(const QByteArray& bytes,const QString& c,const QList<KiwoomVerifiedInstrument>& instruments,qint64 at){
    QList<KiwoomMarketQuote> result;if(!country(c)||at<=0||bytes.size()>MaxFrameBytes)return result;const auto doc=parseExactJson(bytes);if(!doc.isObject()||doc.object().value("trnm").toString()!="REAL"||!doc.object().value("data").isArray())return result;const auto data=doc.object().value("data").toArray();if(data.size()>200)return result;QSet<QString> seen;
    for(const auto& value:data){if(!value.isObject())continue;const auto row=value.toObject();if(row.value("type").toString()!=(c=="US"?"FE":"0B")||!row.value("values").isObject())continue;const auto symbol=row.value("item").toString(),venue=c=="US"?row.value("stexTp").toString():QStringLiteral("KRX");const KiwoomVerifiedInstrument* instrument=nullptr;for(const auto& candidate:instruments)if(candidate.identity.source==c&&candidate.identity.code==symbol&&candidate.exchange==venue&&proof(candidate,at)){if(instrument){instrument=nullptr;break;}instrument=&candidate;}if(!instrument)continue;
        const auto values=row.value("values").toObject();KiwoomMarketQuote q;q.identity=instrument->identity;q.exchange=venue;q.currency=instrument->currency;q.rawSignedPrice=values.value("10").toString();if(!price(q.rawSignedPrice,&q.price,c,true))continue;
        q.rawTime=values.value("20").toString();q.rawDate=values.value("22").toString();q.rawLocalTime=values.value("51020").toString();if((!q.rawTime.isEmpty()&&(!matches(q.rawTime,"\\A[0-9]{6}\\z")||!QTime::fromString(q.rawTime,"HHmmss").isValid()))||(!q.rawDate.isEmpty()&&(!matches(q.rawDate,"\\A[0-9]{8}\\z")||!QDate::fromString(q.rawDate,"yyyyMMdd").isValid()))||(!q.rawLocalTime.isEmpty()&&(!matches(q.rawLocalTime,"\\A[0-9]{6}\\z")||!QTime::fromString(q.rawLocalTime,"HHmmss").isValid())))continue;
        q.evidence={c=="US"?"키움 Nasdaq TotalView 실시간":"키움 KRX 실시간",c=="US"?"wss://api.kiwoom.com:10000/api/us/websocket":"wss://api.kiwoom.com:10000/api/dostk/websocket","public:kiwoom:"+c+':'+symbol+':'+venue,0,at,true};q.realtime=true;if(c=="KR")change(values.value("12").toString(),q,q.evidence);const auto key=binding(q.identity,venue);if(seen.contains(key))return {};seen.insert(key);result.append(q);
    }return result;
}
}
