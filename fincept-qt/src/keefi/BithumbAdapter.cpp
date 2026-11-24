#include "keefi/BithumbAdapter.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <sodium.h>
#include <stdexcept>

namespace keefi {
namespace {
using Fields=QList<QPair<QString,QString>>;
constexpr qint64 SevenDays=7LL*86400000;
QString str(const QJsonObject& o,const char* k){return o.value(QLatin1String(k)).toString();}
bool regex(const QString& s,const char* pattern){return QRegularExpression(QLatin1String(pattern)).match(s).hasMatch();}
bool asset(const QString& s){return regex(s,"\\A[A-Z0-9]{1,40}\\z");}
bool symbol(const QString& s){const auto p=s.split('-');return p.size()==2&&asset(p[0])&&asset(p[1]);}
bool supported(const QString& s){return symbol(s)&&(s.section('-',0,0)=="KRW"||s.section('-',0,0)=="BTC");}
bool id(const QString& s){return regex(s,"\\A[A-Za-z0-9_-]{1,128}\\z");}
bool client(const QString& s){return regex(s,"\\A[A-Za-z0-9_-]{1,36}\\z");}
bool number(const QString& s){return s.size()<=80&&decimal::valid(s);}
bool positive(const QString& s){return number(s)&&decimal::positive(s);}
bool text(const QString& s,qsizetype max=128){
    if(s.isEmpty()||s.size()>max)return false;
    for(const auto c:s)if(c.isNull()||c.category()==QChar::Other_Control||c.category()==QChar::Other_Format)return false;
    return true;
}
bool amount(const QJsonObject& o,const char* key,QString* out,bool unknown=false){
    const auto v=o.value(QLatin1String(key));if(unknown&&(v.isNull()||v.isUndefined())){out->clear();return true;}
    if(!v.isString()||!number(v.toString()))return false;*out=v.toString();return true;
}
bool strings(const QJsonValue& v){if(!v.isArray()||v.toArray().size()>32)return false;QSet<QString> seen;
    for(const auto& x:v.toArray()){if(!x.isString()||!text(x.toString(),40)||seen.contains(x.toString()))return false;seen.insert(x.toString());}return true;}
qint64 millis(const QString& s){bool ok=false;auto n=s.toLongLong(&ok);return regex(s,"\\A[0-9]{1,16}\\z")&&ok&&n>0?n:0;}
bool volume(const QString& s){if(!positive(s))return false;const auto n=decimal::normalize(s);auto d=n.indexOf('.');return d<0||n.size()-d-1<=8;}
QString wireType(const OrderInput& i){return i.type=="limit"?QString("limit"):i.side=="buy"?QString("price"):QString("market");}
bool orderShape(const OrderInput& i){return supported(i.symbol)&&(i.side=="buy"||i.side=="sell")&&
    ((i.type=="limit"&&positive(i.price)&&volume(i.quantity)&&i.quoteAmount.isEmpty())||
     (i.type=="market"&&i.price.isEmpty()&&((i.side=="buy"&&positive(i.quoteAmount)&&i.quantity.isEmpty())||
       (i.side=="sell"&&volume(i.quantity)&&i.quoteAmount.isEmpty()))));}
QByteArray quoted(const QByteArray& s){QJsonArray a;a.append(QString::fromUtf8(s));auto b=QJsonDocument(a).toJson(QJsonDocument::Compact);return b.mid(1,b.size()-2);}
QByteArray body(const Fields& fields){QByteArray b="{";for(const auto& [k,v]:fields){if(b.size()>1)b+=',';b+=quoted(k.toUtf8())+':'+quoted(v.toUtf8());}return b+'}';}
QByteArray query(const Fields& fields,bool encoded){QByteArray b;for(const auto& [k,v]:fields){if(!b.isEmpty())b+='&';
    b+=(encoded?QUrl::toPercentEncoding(k,"[]"):k.toUtf8())+'='+(encoded?QUrl::toPercentEncoding(v):v.toUtf8());}return b;}
struct Wiped {QByteArray b;~Wiped(){if(!b.isEmpty())sodium_memzero(b.data(),size_t(b.size()));}};
QByteArray sign(const CredentialSet& keys,const QByteArray& raw,qint64 now,const QString& nonce){
    auto a=keys.find("api_key"),s=keys.find("secret");
    if(a==keys.end()||s==keys.end()||a->second.empty()||s->second.empty()||a->second.size()>8192||s->second.size()>8192||now<=0||
        !regex(nonce,"\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"))throw std::runtime_error("INVALID_SIGNING_INPUT");
    Wiped access{QByteArray(reinterpret_cast<const char*>(a->second.data()),qsizetype(a->second.size()))};
    if(QString::fromUtf8(access.b).toUtf8()!=access.b)throw std::runtime_error("INVALID_KEY_ENCODING");
    Wiped payload{"{\"access_key\":"+quoted(access.b)+",\"nonce\":"+quoted(nonce.toUtf8())+",\"timestamp\":"+QByteArray::number(now)};
    if(!raw.isEmpty())payload.b+=",\"query_hash\":\""+QCryptographicHash::hash(raw,QCryptographicHash::Sha512).toHex()+"\",\"query_hash_alg\":\"SHA512\"";
    payload.b+='}';const auto enc=QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals;
    Wiped signing{QByteArray("{\"alg\":\"HS256\",\"typ\":\"JWT\"}").toBase64(enc)+'.'+payload.b.toBase64(enc)};
    SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)),digest(crypto_auth_hmacsha256_BYTES);
    auto* h=reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
    if(crypto_auth_hmacsha256_init(h,s->second.data(),s->second.size())||
       crypto_auth_hmacsha256_update(h,reinterpret_cast<const unsigned char*>(signing.b.constData()),signing.b.size())||
       crypto_auth_hmacsha256_final(h,digest.data()))throw std::runtime_error("SIGN_FAILED");
    Wiped signature{QByteArray(reinterpret_cast<const char*>(digest.data()),qsizetype(digest.size())).toBase64(enc)};
    return "Bearer "+signing.b+'.'+signature.b;
}
struct Cursor{QString state,key;};
QJsonObject cursorContext(Operation op,const OrderInput& in){return {{"v","1"},{"provider","bithumb"},{"env","live"},
    {"op",op==Operation::OpenOrders?"pending":"history"},{"symbol",op==Operation::OpenOrders?QString{}:in.symbol},
    {"start",op==Operation::OpenOrders?QString{}:QString::number(in.startTimeMs)},
    {"end",op==Operation::OpenOrders?QString{}:QString::number(in.endTimeMs)}};}
bool cursor(Operation op,const OrderInput& in,Cursor* out){
    if(op==Operation::History&&(!supported(in.symbol)||in.startTimeMs<=0||in.endTimeMs<in.startTimeMs||in.endTimeMs-in.startTimeMs>SevenDays))return false;
    out->state=op==Operation::OpenOrders?"wait":"closed";out->key.clear();if(in.cursor.isEmpty())return true;
    if(in.cursor.size()>8192||!regex(in.cursor,"\\A[A-Za-z0-9_-]+\\z"))return false;
    const auto doc=QJsonDocument::fromJson(QByteArray::fromBase64(in.cursor.toLatin1(),QByteArray::Base64UrlEncoding));
    if(!doc.isObject())return false;auto o=doc.object();const auto expected=cursorContext(op,in);
    if(o.size()!=expected.size()+2)return false;
    for(auto it=expected.constBegin();it!=expected.constEnd();++it)if(o.value(it.key())!=it.value())return false;
    if(!o.value("state").isString()||!o.value("key").isString())return false;
    out->state=str(o,"state");out->key=str(o,"key");
    if((op==Operation::OpenOrders&&out->state!="wait"&&out->state!="watch")||(op==Operation::History&&out->state!="closed"))return false;
    return out->key.isEmpty()?(op==Operation::OpenOrders&&out->state=="watch"):text(out->key,2048);
}
QString nextCursor(Operation op,const OrderInput& in,const QString& state,const QString& key){auto o=cursorContext(op,in);o["state"]=state;o["key"]=key;
    return QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals));}
AdapterResult bad(Operation op,const QString& code="MALFORMED",const QString& message={}){
    AdapterResult r;r.code=code;r.message=message.isEmpty()?QStringLiteral("빗썸 응답을 안전하게 해석하지 못했습니다."):message;
    r.unknown=isMutation(op);r.needsReconciliation=r.unknown;if(r.unknown)r.requestState="unknown";return r;}
QString min(const QString& a,const QString& b){return decimal::compare(a,b)<0?a:b;}
QString krwTick(const QString& p){for(const auto& pair:QList<QPair<QString,QString>>{{"1000000","1000"},{"500000","500"},{"100000","100"},{"50000","50"},
    {"10000","10"},{"5000","5"},{"100","1"},{"10","0.01"},{"1","0.001"}})if(decimal::compare(p,pair.first)>=0)return pair.second;return "0.0001";}
bool orderRecord(const QJsonObject& o,bool v2,OrderRecord* r){
    r->orderId=str(o,v2?"order_id":"uuid");r->clientId=str(o,"client_order_id");r->symbol=str(o,"market");
    const auto side=str(o,"side"),type=str(o,v2?"order_type":"ord_type"),state=str(o,"state");
    if(!id(r->orderId)||(!r->clientId.isEmpty()&&!client(r->clientId))||!symbol(r->symbol)||
       (side!="bid"&&side!="ask")||!text(type,40)||!QStringList{"wait","watch","done","cancel"}.contains(state))return false;
    const auto clientValue=o.value("client_order_id");if(!clientValue.isUndefined()&&!clientValue.isNull()&&!clientValue.isString())return false;
    r->side=side=="bid"?"buy":"sell";r->type=(type=="price"||type=="market")?"market":type;r->rawStatus=state;
    r->market="spot";r->quoteAsset=r->symbol.section('-',0,0);r->baseAsset=r->symbol.section('-',1,1);r->currency=r->quoteAsset;
    QString px,qty,remain,executed;
    if(!amount(o,"price",&px,true)||!amount(o,"volume",&qty,true)||!amount(o,"remaining_volume",&remain,true)||!amount(o,"executed_volume",&executed))return false;
    if(!qty.isEmpty()&&!remain.isEmpty()&&(decimal::compare(executed,qty)>0||decimal::compare(remain,decimal::subtract(qty,executed))>0))return false;
    r->executed=r->executedBaseAmount=executed;r->remainingBaseAmount=remain;
    if(type=="price"){if(side!="bid")return false;r->quantityUnit="quote";r->quoteAmount=r->requestedQuoteAmount=px;}
    else{r->quantityUnit="base";r->quantity=qty;r->remaining=remain;if(type!="market")r->price=px;if(type=="market"&&side!="ask")return false;}
    r->status=state=="watch"?"watch":state=="cancel"?"canceled":state=="wait"?(decimal::positive(executed)?"partial":"open"):
        positive(qty)&&decimal::compare(executed,qty)==0&&number(remain)&&decimal::compare(remain,"0")==0?"filled":"done";
    const auto tif=o.value("time_in_force");const bool ordinary=tif.isUndefined()||tif.isNull()||(tif.isString()&&tif.toString().isEmpty());
    if(!ordinary){if(!tif.isString()||!text(tif.toString(),40))return false;r->type=type+"_"+tif.toString();}
    const bool terms=type=="limit"?positive(px)&&positive(qty)&&number(remain):type=="price"?positive(px):type=="market"?positive(qty)&&number(remain):false;
    r->cancelable=supported(r->symbol)&&ordinary&&terms&&state=="wait";r->amendable=false;
    return true;
}
}
BithumbAdapter::BithumbAdapter(std::function<QString()> nonce):nonce_(std::move(nonce)){
    if(!nonce_)nonce_=[](){return QUuid::createUuid().toString(QUuid::WithoutBraces);};}
ExchangePolicy BithumbAdapter::exchangePolicy()const{ExchangePolicy p;p.officialOrderTest=false;p.exactIntentPreflight=true;
    p.amendModel=ExchangeAmendModel::Unsupported;p.marketBuyUnit="quote";return p;}
QList<Operation> BithumbAdapter::refreshPlan()const{return {Operation::Account,Operation::OpenOrders,Operation::History};}
QString BithumbAdapter::amendDescription()const{return QStringLiteral("공개 명세에서 원자적 정정을 확인하지 못했습니다. 취소 뒤 새 주문을 자동 생성하지 않습니다.");}
RequestSpec BithumbAdapter::build(Operation op,const AccountRecord& a,const OrderInput& in,const CredentialSet& keys,qint64 now,QString* error)const{
    if(error)error->clear();RequestSpec r;r.operation=op;Fields f;QString path;
    auto reject=[&](const QString& m){if(error)*error=m;return RequestSpec{};};
    if(a.provider!="bithumb"||a.environment!="live"||a.market!="spot")return reject("빗썸 실전 현물 계좌만 연결할 수 있습니다.");
    switch(op){
    case Operation::Markets:path="/v1/market/all";f={{"isDetails","true"}};break;
    case Operation::Quote:case Operation::Depth:
        if(!symbol(in.symbol))return reject("종목을 확인하세요.");path=op==Operation::Quote?"/v1/ticker":"/v1/orderbook";f={{"markets",in.symbol}};break;
    case Operation::Rules:if(!supported(in.symbol))return reject("주문 조건을 조회할 KRW·BTC 종목을 선택하세요.");
        path="/v1/orders/chance";r.authenticated=true;f={{"market",in.symbol}};break;
    case Operation::Account:path="/v1/accounts";r.authenticated=true;break;
    case Operation::OpenOrders:case Operation::History:{Cursor c;if(!cursor(op,in,&c))return reject("주문 목록의 고정 조회 범위·페이지 위치를 확인하세요.");
        r.authenticated=true;if(op==Operation::OpenOrders){path="/v2/orders/pending";f={{"state",c.state},{"limit","100"},{"order_by","desc"}};}
        else{path="/v2/orders/history";f={{"market",in.symbol},{"start_time",QString::number(in.startTimeMs)},
            {"end_time",QString::number(in.endTimeMs)},{"limit","1000"},{"order_by","desc"}};}
        if(!c.key.isEmpty())f.append({"next_key",c.key});break;}
    case Operation::QueryOrder:
        if(in.orderId.isEmpty()?!client(in.clientId):!id(in.orderId))return reject("조회·취소할 주문 ID를 확인하세요.");
        r.authenticated=true;path=op==Operation::QueryOrder?"/v1/order":"/v2/order";
        f={{in.orderId.isEmpty()?"client_order_id":op==Operation::QueryOrder?"uuid":"order_id",in.orderId.isEmpty()?in.clientId:in.orderId}};
        break;
    default:return reject("이 작업의 빗썸 공개 API 계약은 이번 범위에서 지원하지 않습니다.");
    }
    QByteArray target=path.toLatin1();if(r.method=="POST")r.body=body(f);else if(!f.isEmpty())target+='?'+query(f,true);
    r.url=QUrl::fromEncoded("https://api.bithumb.com"+target,QUrl::StrictMode);r.headers.insert("Accept","application/json");
    if(r.method=="POST")r.headers.insert("Content-Type","application/json; charset=utf-8");
    if(r.authenticated){try{Wiped raw{query(f,false)};r.headers.insert("Authorization",sign(keys,raw.b,now,nonce_()));}
        catch(const std::exception&){return reject("빗썸 인증 요청을 안전하게 구성하지 못했습니다.");}}
    return r;
}
AdapterResult BithumbAdapter::parse(Operation op,const HttpResult& http,const OrderInput& in)const{
    if(http.networkError||http.timedOut)return bad(op,"NETWORK","응답이 불완전합니다. 주문은 재전송하지 않고 조회로 확인합니다.");
    QString error;const auto doc=parseExactJson(http.body,&error);const auto o=doc.object();
    const auto err=o.value("error").toObject();const auto code=str(err,"name");
    if(http.status==401){auto r=bad(op,code=="out_of_scope"?"PERMISSION":"AUTHENTICATION",code=="out_of_scope"?"이 조회·주문에 필요한 API 권한이 없습니다.":"키·허용 IP·만료 등 인증 상태를 확인하세요.");
        r.authenticationFailed=!(op==Operation::Rules&&code=="out_of_scope");return r;}
    if(http.status==429){auto r=bad(op,"RATE_LIMIT","요청 제한으로 조회·주문을 중지했습니다.");r.rateLimited=true;
        for(auto it=http.headers.cbegin();it!=http.headers.cend();++it)if(it.key().compare("Retry-After",Qt::CaseInsensitive)==0){bool ok=false;auto n=it.value().toInt(&ok);if(ok&&n>0&&n<=86400)r.retryAfterSeconds=n;}return r;}
    if(!error.isEmpty()||doc.isNull())return bad(op);

    if(!code.isEmpty()||http.status<200||http.status>=300){
        auto r=bad(op,"API_ERROR","빗썸 요청이 정상 완료되지 않았습니다. 확인되지 않은 주문은 보존합니다.");
        if(code=="order_not_found"){r.notFound=true;r.code="ORDER_NOT_FOUND";}
        else if(code=="order_not_ready")r.code="ORDER_NOT_READY";
        static const QSet<QString> rejected={"invalid_parameter","invalid_price","under_price_limit_ask","under_price_limit_bid","invalid_price_ask","invalid_price_bid"};
        if(http.status==400&&isMutation(op)&&rejected.contains(code)){r.definiteRejection=true;r.unknown=false;r.needsReconciliation=false;r.requestState="rejected";r.code="ORDER_REJECTED";r.message="주문 입력이 거절되었습니다. 조건을 확인하세요.";}
        return r;
    }
    // Exact successful DELETE response schema was not available in the retrieved
    // official docs. Preserve the already journaled target and reconcile by GET.

    AdapterResult r;r.ok=true;
    switch(op){
    case Operation::Markets:{if(!doc.isArray()||doc.array().size()>10000)return bad(op);QJsonArray rows,symbols;QSet<QString> seen;
        for(const auto& v:doc.array()){if(!v.isObject())return bad(op);auto m=v.toObject();const auto s=str(m,"market"),warning=str(m,"market_warning");
            if(!symbol(s)||seen.contains(s)||!text(str(m,"korean_name"),200)||!text(str(m,"english_name"),200)||!text(warning,40))return bad(op);seen.insert(s);symbols.append(s);
            rows.append(QJsonObject{{"symbol",s},{"baseAsset",s.section('-',1,1)},{"quoteAsset",s.section('-',0,0)},{"name",str(m,"korean_name")},
                {"englishName",str(m,"english_name")},{"status",warning},{"tradable",supported(s)},
                {"warning",!supported(s)?QString("조회 전용 결제 통화"):warning=="NONE"?QString{}:QString("거래 유의: ")+warning}});}
        r.data={{"instruments",rows},{"symbols",symbols}};break;}
    case Operation::Account:{if(!doc.isArray()||doc.array().size()>10000)return bad(op);QSet<QString> seen;
        for(const auto& v:doc.array()){if(!v.isObject())return bad(op);const auto b=v.toObject();Balance x;x.asset=str(b,"currency");x.section="spot";
            if(!asset(x.asset)||seen.contains(x.asset)||!amount(b,"balance",&x.free)||!amount(b,"locked",&x.locked))return bad(op);
            const auto average=str(b,"avg_buy_price"),currency=str(b,"unit_currency");
            if(average==average.trimmed()&&positive(average)&&asset(currency)){
                x.providerAveragePrice=decimal::normalize(average);x.providerAverageCurrency=currency;}
            seen.insert(x.asset);r.balances.append(x);}
        r.data={{"scope","api_key_pocket"}};break;}
    case Operation::Rules:{if(!doc.isObject())return bad(op);auto m=o.value("market").toObject(),b=m.value("bid").toObject(),a=m.value("ask").toObject();
        auto ba=o.value("bid_account").toObject(),aa=o.value("ask_account").toObject();QString bm,am,max,bf,af,bb,ab,bl,al,bp,ap;
        if(!supported(in.symbol)||str(m,"id")!=in.symbol||str(b,"currency")!=in.symbol.section('-',0,0)||str(a,"currency")!=in.symbol.section('-',1,1)||
           str(ba,"currency")!=str(b,"currency")||str(aa,"currency")!=str(a,"currency")||!text(str(m,"state"),40)||
           !amount(b,"min_total",&bm)||!amount(a,"min_total",&am)||!amount(m,"max_total",&max)||!amount(o,"bid_fee",&bf)||!amount(o,"ask_fee",&af)||
           !amount(ba,"balance",&bb)||!amount(aa,"balance",&ab)||!amount(ba,"locked",&bl)||!amount(aa,"locked",&al)||
           !amount(b,"price_unit",&bp,true)||!amount(a,"price_unit",&ap,true)||!strings(m.value("bid_types"))||!strings(m.value("ask_types"))||!strings(m.value("order_sides")))return bad(op);
        r.data={{"symbol",in.symbol},{"baseAsset",str(a,"currency")},{"quoteAsset",str(b,"currency")},{"state",str(m,"state")},
            {"minBuyNotional",bm},{"minSellNotional",am},{"maxNotional",max},{"buyFee",bf},{"sellFee",af},
            {"buyAvailable",bb},{"sellAvailable",ab},{"buyLocked",bl},{"sellLocked",al},
            {"buyPriceUnit",bp.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(bp)},{"sellPriceUnit",ap.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(ap)},
            {"buyTypes",m.value("bid_types")},{"sellTypes",m.value("ask_types")},{"sides",m.value("order_sides")}};break;}
    case Operation::Quote:case Operation::Depth:{if(!doc.isArray()||doc.array().size()!=1||!doc.array()[0].isObject())return bad(op);const auto q=doc.array()[0].toObject();
        if(str(q,"market")!=in.symbol||!millis(str(q,"timestamp")))return bad(op);
        r.data={{"symbol",in.symbol},{"timestamp",str(q,"timestamp")}};
        if(op==Operation::Quote){QString p;if(!amount(q,"trade_price",&p)||!positive(p)||!millis(str(q,"trade_timestamp")))return bad(op);
            r.data.insert("price",p);r.data.insert("lastPrice",p);r.data.insert("tradeTimestamp",str(q,"trade_timestamp"));break;}
        if(!q.value("orderbook_units").isArray()||q.value("orderbook_units").toArray().isEmpty()||q.value("orderbook_units").toArray().size()>100)return bad(op);
        QJsonArray bids,asks;QString prevBid,prevAsk;
        for(const auto& v:q.value("orderbook_units").toArray()){if(!v.isObject())return bad(op);const auto l=v.toObject();QString bp,ap,bs,as;
            if(!amount(l,"bid_price",&bp)||!positive(bp)||!amount(l,"ask_price",&ap)||!positive(ap)||!amount(l,"bid_size",&bs)||!amount(l,"ask_size",&as)||
               decimal::compare(bp,ap)>0||(!prevBid.isEmpty()&&(decimal::compare(bp,prevBid)>=0||decimal::compare(ap,prevAsk)<=0)))return bad(op);
            if(prevBid.isEmpty()){r.data.insert("bid",bp);r.data.insert("ask",ap);}prevBid=bp;prevAsk=ap;bids.append(QJsonArray{bp,bs});asks.append(QJsonArray{ap,as});}
        r.data.insert("bids",bids);r.data.insert("asks",asks);break;}
    case Operation::OpenOrders:case Operation::History:{Cursor c;if(!cursor(op,in,&c)||!doc.isObject()||!o.value("data").isArray()||!o.value("has_next").isBool()||!o.contains("next_key"))return bad(op);
        const auto rows=o.value("data").toArray();const bool more=o.value("has_next").toBool();const auto key=o.value("next_key");
        if(rows.size()>(op==Operation::OpenOrders?100:1000)||(more&&(!key.isString()||!text(key.toString(),2048)||key.toString()==c.key||rows.isEmpty()))||(!more&&!key.isNull()))return bad(op);
        QSet<QString> seen;for(const auto& v:rows){OrderRecord x;if(!v.isObject()||!orderRecord(v.toObject(),true,&x)||seen.contains(x.orderId)||
            (op==Operation::OpenOrders&&x.rawStatus!=c.state)||(op==Operation::History&&(x.symbol!=in.symbol||(x.rawStatus!="done"&&x.rawStatus!="cancel"))))return bad(op);
            seen.insert(x.orderId);r.orders.append(x);}
        if(more){r.hasMore=true;r.nextCursor=nextCursor(op,in,c.state,key.toString());}
        else if(op==Operation::OpenOrders&&c.state=="wait"){r.hasMore=true;r.nextCursor=nextCursor(op,in,"watch",{});}
        r.data={{"scopeComplete",!r.hasMore}};r.scope=op==Operation::OpenOrders?"API 계좌 전체 wait·watch 주문. 두 상태는 원자적 스냅샷이 아닙니다.":"선택 종목 최근 7일 종료 주문. 예약 취소·TWAP 등 전체 과거 기록을 보장하지 않습니다.";break;}
    case Operation::QueryOrder:{OrderRecord x;if(!doc.isObject()||!orderRecord(o,false,&x)||x.symbol!=in.symbol||
        (!in.orderId.isEmpty()?x.orderId!=in.orderId:x.clientId!=in.clientId))return bad(op);r.orders.append(x);break;}
    default:return bad(op);
    }return r;
}
OrderValidation BithumbAdapter::validate(const OrderInput& in,const MarketSnapshot& s,const OrderLimits& limits)const{
    OrderValidation v;auto no=[&](const QString& m){v.message=m;return v;};
    if(!orderShape(in)||!in.orderId.isEmpty())return no("빗썸 일반 지정가·시장가 신규 입력만 준비합니다. 정정은 지원하지 않습니다.");
    const auto rules=s.data.value(Operation::Rules),quote=s.data.value(Operation::Quote),depth=s.data.value(Operation::Depth);
    const auto base=in.symbol.section('-',1,1),ccy=in.symbol.section('-',0,0);const bool buy=in.side=="buy",market=in.type=="market";
    if(str(rules,"symbol")!=in.symbol||str(rules,"baseAsset")!=base||str(rules,"quoteAsset")!=ccy||str(rules,"state")!="active")return no("해당 종목의 최신 거래 조건을 확인해야 합니다.");
    if(!rules.value(buy?"buyTypes":"sellTypes").toArray().contains(wireType(in))||!rules.value("sides").toArray().contains(buy?"bid":"ask"))return no("거래소가 이 방향·주문 유형을 허용하는지 확인해야 합니다.");
    if(!market){const auto tick=ccy=="KRW"?krwTick(in.price):str(rules,buy?"buyPriceUnit":"sellPriceUnit");
        if(!positive(tick))return no("이 결제 통화의 지정가 호가 단위가 미확인입니다.");if(!decimal::multipleOf(in.price,tick))return no("지정가가 가격대별 호가 단위에 맞지 않습니다.");}
    const auto last=str(quote,"price");if(str(quote,"symbol")!=in.symbol||!positive(last)||!positive(limits.maxNotional)||!number(limits.maxDeviationPercent)||limits.currency!=ccy)return no("시세·사용자 금액 한도·결제 통화를 확인하세요.");
    v.currency=ccy;QString worst=in.price;
    if(!market)v.notional=decimal::multiply(in.price,in.quantity);
    else{if(str(depth,"symbol")!=in.symbol)return no("시장가 주문에 필요한 해당 종목 호가가 없습니다.");const auto rows=depth.value(buy?"asks":"bids");
        if(!rows.isArray()||rows.toArray().isEmpty())return no("시장가 주문을 소화할 호가가 없습니다.");QString remaining=buy?in.quoteAmount:in.quantity,cost="0",previous;
        for(const auto& raw:rows.toArray()){const auto row=raw.toArray();if(!raw.isArray()||row.size()!=2||!positive(row[0].toString())||!number(row[1].toString()))return no("시장가 호가 형식이 올바르지 않습니다.");
            const auto px=row[0].toString(),qty=row[1].toString();if(!previous.isEmpty()&&(buy?decimal::compare(px,previous)<=0:decimal::compare(px,previous)>=0))return no("호가 정렬을 확인해야 합니다.");previous=px;
            if(!decimal::positive(qty))continue;const auto cap=buy?decimal::multiply(px,qty):qty;if(!number(cap))return no("호가 계산 한도를 초과했습니다.");const auto take=min(remaining,cap);
            remaining=decimal::subtract(remaining,take);cost=decimal::add(cost,buy?take:decimal::multiply(take,px));worst=px;
            if(!number(remaining)||!number(cost))return no("호가 계산 한도를 초과했습니다.");if(decimal::compare(remaining,"0")==0)break;}
        if(!number(remaining)||decimal::compare(remaining,"0")!=0)return no("조회된 호가로 전체 시장가 주문을 충족하지 못합니다.");v.notional=cost;}
    const auto minimum=str(rules,buy?"minBuyNotional":"minSellNotional"),maximum=str(rules,"maxNotional"),fee=str(rules,buy?"buyFee":"sellFee");
    if(!positive(v.notional)||!positive(minimum)||!positive(maximum)||!number(fee))return no("최소·최대 주문 금액 또는 수수료가 미확인입니다.");
    if(decimal::compare(v.notional,minimum)<0||decimal::compare(v.notional,maximum)>0||decimal::compare(v.notional,limits.maxNotional)>0)return no("거래소 또는 사용자 주문 금액 한도를 벗어났습니다.");
    const auto delta=decimal::compare(worst,last)>=0?decimal::subtract(worst,last):decimal::subtract(last,worst);
    if(!number(delta)||decimal::compare(decimal::multiply(delta,"100"),decimal::multiply(last,limits.maxDeviationPercent))>0)return no("설정한 시세 괴리율을 초과합니다.");
    QString free;const auto assetNeeded=buy?ccy:base;for(const auto& b:s.balances)if(b.asset==assetNeeded){if(!free.isEmpty()||!number(b.free))return no("사용 가능 잔고를 확인해야 합니다.");free=b.free;}
    const auto chance=str(rules,buy?"buyAvailable":"sellAvailable");if(!number(free)||!number(chance))return no("자산 조회와 주문 조건의 가용 잔고가 모두 필요합니다.");
    const auto needed=buy?decimal::add(v.notional,decimal::multiply(v.notional,fee)):in.quantity;
    if(!positive(needed)||decimal::compare(needed,min(free,chance))>0)return no("수수료를 포함한 주문 가능 잔고가 부족합니다.");
    v.ok=true;v.message="현물 사전 검사를 통과했습니다. 실제 권한과 최종 주문 조건은 거래소가 검증합니다.";return v;
}
} // namespace keefi
