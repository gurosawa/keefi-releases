#include "keefi/OkxAdapter.h"
#include "keefi/OkxProtocol.h"
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <sodium.h>

namespace keefi {
namespace {
using Params = QList<QPair<QByteArray, QString>>;
QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }
bool number(const QString& s) { return s.size() <= 80 && decimal::valid(s); }
bool positive(const QString& s) { return number(s) && decimal::positive(s); }
bool signedNumber(QString s) { if (s.startsWith('-')) s.remove(0,1); return number(s); }
bool integer(const QString& s) { static const QRegularExpression re("\\A[0-9]{1,16}\\z"); return re.match(s).hasMatch(); }
qint64 millis(const QString& s) { bool ok=false; auto n=s.toLongLong(&ok); return integer(s)&&ok&&n>0 ? n : 0; }
bool safeText(const QString& s, qsizetype maximum=128) {
    if (s.isEmpty() || s.size()>maximum) return false;
    for (QChar c:s) if (c.isNull() || c.category()==QChar::Other_Control || c.category()==QChar::Other_Format) return false;
    return true;
}
bool safeHeader(const SecureBuffer& s, bool asciiOnly) {
    if (s.empty() || s.size()>512) return false;
    for (size_t i=0;i<s.size();++i) if (s.data()[i]<32 || s.data()[i]==127 || (asciiOnly&&s.data()[i]>126)) return false;
    return true;
}
QByteArray query(const Params& params) {
    QByteArray out; for (const auto& p:params) { if(!out.isEmpty()) out+='&'; out+=p.first+'='+QUrl::toPercentEncoding(p.second); } return out;
}
QString origin(const QString& region) {
    if(region=="global") return "https://openapi.okx.com";
    if(region=="us") return "https://us.okx.com";
    if(region=="eea") return "https://eea.okx.com";
    return {};
}
AdapterResult failure(Operation op, const QString& code, const QString& message) {
    AdapterResult r; r.code=code; r.message=message; r.unknown=isMutation(op); r.needsReconciliation=r.unknown;
    if(r.unknown) r.requestState="unknown"; return r;
}
bool authCode(const QString& code) {
    static const QSet<QString> codes={"50103","50104","50105","50106","50107","50108","50109","50110","50111","50113","50114","50119","50120","50128"};
    return codes.contains(code);
}
QString available(const MarketSnapshot& s, const QString& currency) {
    QString result;
    for(const auto& b:s.balances) if(b.section=="trading"&&b.asset==currency) {
        if(!result.isEmpty() || !number(b.free)) return {};
        result=b.free;
    }
    return result;
}
QString less(const QString& a,const QString& b) { return decimal::compare(a,b)<0 ? a:b; }
QString feeCost(const QString& signedRate) { return signedRate.startsWith('-') ? signedRate.mid(1) : QString("0"); }
QJsonValue nullable(const QString& s) { return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s); }
}

OkxAdapter::OkxAdapter(const AccountRecord& account):region_(account.region),environment_(account.environment) {}
ExchangePolicy OkxAdapter::exchangePolicy() const { ExchangePolicy p; p.officialOrderTest=false; p.exactIntentPreflight=true;
    p.amendModel=ExchangeAmendModel::SameOrder; p.marketBuyUnit="quote"; return p; }
QList<Operation> OkxAdapter::refreshPlan() const { return {Operation::Time,Operation::AccountConfig,Operation::Account,
    Operation::OpenOrders,Operation::FundingBalance,Operation::History}; }
QString OkxAdapter::amendDescription() const {
    return QStringLiteral("동일 주문 ID의 지정가와 체결분 포함 전체 수량을 정정합니다. 접수 뒤 정정 결과를 별도로 확인합니다.");
}

RequestSpec OkxAdapter::build(Operation op,const AccountRecord& a,const OrderInput& in,
    const CredentialSet& credentials,qint64 now,QString* error) const {
    if(error) error->clear(); RequestSpec r; r.operation=op;
    const auto reject=[&](const QString& msg){if(error)*error=msg;return RequestSpec{};};
    if(a.provider!="okx"||a.market!="spot"||a.region!=region_||a.environment!=environment_||
        !okxRegionValid(region_)||!okxEnvironmentValid(environment_)) return reject("OKX 지역과 실전 현물 계정을 확인하세요.");
    const bool noSymbol=op==Operation::Time||op==Operation::Markets||op==Operation::AccountConfig||op==Operation::Account||
        op==Operation::FundingBalance||op==Operation::OpenOrders;
    if(!noSymbol&&!okxInstrumentValid(in.symbol)) return reject("OKX 현물 종목 형식이 올바르지 않습니다.");
    const auto quote=in.symbol.section('-',1,1); QByteArray path; Params p; QJsonObject b;
    switch(op) {
    case Operation::Time: path="/api/v5/public/time"; break;
    case Operation::Markets: path="/api/v5/public/instruments"; p={{"instType","SPOT"}}; break;
    case Operation::AccountConfig: path="/api/v5/account/config"; r.authenticated=true; break;
    case Operation::Account: path="/api/v5/account/balance"; r.authenticated=true; break;
    case Operation::FundingBalance: path="/api/v5/asset/balances"; r.authenticated=true; break;
    case Operation::Rules: path="/api/v5/account/instruments"; r.authenticated=true; p={{"instType","SPOT"},{"instId",in.symbol}}; break;
    case Operation::Quote: path="/api/v5/market/ticker"; p={{"instId",in.symbol}}; break;
    case Operation::Depth: path="/api/v5/market/books"; p={{"instId",in.symbol},{"sz","100"}}; break;
    case Operation::PriceLimits: path="/api/v5/public/price-limit"; p={{"instId",in.symbol}}; break;
    case Operation::FxUsd: case Operation::FxUsdt: {
        const QString target=op==Operation::FxUsd ? "USD":"USDT";
        if(quote==target) path="/api/v5/public/time";
        else {path="/api/v5/market/index-tickers";p={{"instId",quote+'-'+target}};} break;
    }
    case Operation::Commissions: path="/api/v5/account/trade-fee";r.authenticated=true;p={{"instType","SPOT"},{"instId",in.symbol}};break;
    case Operation::BuyingPower: path="/api/v5/account/max-avail-size";r.authenticated=true;
        p={{"instId",in.symbol},{"tdMode","cash"},{"tradeQuoteCcy",quote}};break;
    case Operation::OpenOrders: case Operation::History: {
        r.authenticated=true; path=op==Operation::OpenOrders ? "/api/v5/trade/orders-pending":"/api/v5/trade/orders-history";
        p={{"instType","SPOT"},{"limit","100"}};
        if(op==Operation::History) {
            if(in.startTimeMs<=0||in.endTimeMs<in.startTimeMs||in.endTimeMs-in.startTimeMs>7LL*86400000)
                return reject("OKX 이력 조회는 고정된 최대 7일 범위가 필요합니다.");
            p.append({"instId",in.symbol});p.append({"begin",QString::number(in.startTimeMs)});p.append({"end",QString::number(in.endTimeMs)});
        }
        if(!in.cursor.isEmpty()) {if(!okxOrderIdValid(in.cursor))return reject("OKX 페이지 식별자가 올바르지 않습니다.");p.append({"after",in.cursor});}
        break;
    }
    case Operation::QueryOrder:
        path="/api/v5/trade/order";r.authenticated=true;p={{"instId",in.symbol}};
        if(!in.orderId.isEmpty()){if(!okxOrderIdValid(in.orderId))return reject("주문 ID 형식이 올바르지 않습니다.");p.append({"ordId",in.orderId});}
        else {if(!okxClientIdValid(in.clientId,false))return reject("조회할 주문 식별자가 필요합니다.");p.append({"clOrdId",in.clientId});}break;
    default:return reject("이 OKX 현물 작업은 구현 범위 밖입니다.");
    }
    const auto q=query(p);const auto target=path+(q.isEmpty()?QByteArray{}:'?'+q);
    r.url=QUrl::fromEncoded(origin(region_).toUtf8()+target);r.headers.insert("Accept","application/json");
    if(r.authenticated) {
        const auto key=credentials.find("api_key"),secret=credentials.find("secret"),pass=credentials.find("passphrase");
        if(key==credentials.end()||secret==credentials.end()||pass==credentials.end()||!safeHeader(key->second,true)||
            !safeHeader(pass->second,false)||secret->second.empty()||secret->second.size()>512||now<=0)
            return reject("OKX API Key·Secret·Passphrase와 서버 시각을 확인하세요.");
        try {
            const auto stamp=QDateTime::fromMSecsSinceEpoch(now,Qt::UTC).toString(Qt::ISODateWithMs).toUtf8();
            const auto preimage=stamp+r.method+target+r.body;
            SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)),digest(crypto_auth_hmacsha256_BYTES);
            auto* h=reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
            crypto_auth_hmacsha256_init(h,secret->second.data(),secret->second.size());
            crypto_auth_hmacsha256_update(h,reinterpret_cast<const unsigned char*>(preimage.constData()),preimage.size());
            crypto_auth_hmacsha256_final(h,digest.data());
            QByteArray raw(reinterpret_cast<const char*>(digest.data()),digest.size());
            r.headers.insert("OK-ACCESS-SIGN",raw.toBase64());sodium_memzero(raw.data(),raw.size());
            r.headers.insert("OK-ACCESS-KEY",QByteArray(reinterpret_cast<const char*>(key->second.data()),key->second.size()));
            r.headers.insert("OK-ACCESS-PASSPHRASE",QByteArray(reinterpret_cast<const char*>(pass->second.data()),pass->second.size()));
            r.headers.insert("OK-ACCESS-TIMESTAMP",stamp);r.headers.insert("Content-Type","application/json");
        } catch(...) {return reject("서명용 보안 메모리를 확보하지 못했습니다.");}
    }
    return r;
}

AdapterResult OkxAdapter::parse(Operation op,const HttpResult& http,const OrderInput& in) const {
    auto bad=[&](){return failure(op,"MALFORMED",QStringLiteral("OKX 응답을 안전하게 해석하지 못했습니다."));};
    if(http.status==401) {auto r=failure(op,"AUTH","OKX 인증을 다시 확인하세요.");r.authenticationFailed=true;return r;}
    if(http.timedOut||http.networkError)return failure(op,"NETWORK","응답을 확인하지 못했습니다. 주문을 다시 전송하지 말고 조회하세요.");
    const auto doc=parseExactJson(http.body);if(!doc.isObject())return bad();const auto obj=doc.object();
    const auto code=str(obj,"code");if(code.isEmpty()||code.size()>16||!obj.value("data").isArray())return bad();
    const auto rows=obj.value("data").toArray();
    if(authCode(code)){auto r=failure(op,"AUTH","OKX 키·권한·환경을 다시 확인하세요.");r.authenticationFailed=true;return r;}
    if(http.status==429||code=="50011"||code=="50061") {auto r=failure(op,"RATE_LIMIT","OKX 요청 한도에 도달했습니다.");r.rateLimited=true;
        bool ok=false;int seconds=http.headers.value("retry-after").toInt(&ok);if(ok&&seconds>=0&&seconds<=3600)r.retryAfterSeconds=seconds;return r;}

    if(code!="0"||http.status<200||http.status>=300) {
        auto r=failure(op,"API_ERROR","OKX 조회가 완료되지 않았습니다.");
        r.notFound=op==Operation::QueryOrder&&code=="51603";return r;
    }
    AdapterResult r;r.ok=true;const auto one=[&](){return rows.size()==1&&rows[0].isObject();};
    const QJsonObject o=one()?rows[0].toObject():QJsonObject{};
    switch(op) {
    case Operation::Time:if(!one()||!millis(str(o,"ts")))return bad();r.serverTimeMs=millis(str(o,"ts"));r.data={{"timestamp",str(o,"ts")}};break;
    case Operation::AccountConfig: {
        if(!one())return bad();r.scope=okxAccountScope(region_,environment_,str(o,"uid"));if(r.scope.isEmpty())return bad();
        // UID establishes account identity. Unavailable configuration/permission
        // metadata disables new risk, not successful account reads.
        const auto rawPerm=o.value("perm");const auto permText=rawPerm.toString();
        const auto perms=permText.split(',');QSet<QString> seen;
        bool known=rawPerm.isString()&&!permText.isEmpty()&&permText.size()<=128;
        if(permText.size()<=128)for(const auto& p:perms){
            if(seen.contains(p)||p.isEmpty())known=false;
            seen.insert(p);if(p!="read_only"&&p!="trade"&&p!="withdraw")known=false;
        }
        r.tradingPermissionKnown=known;r.tradingAllowed=known&&seen.contains("trade");
        r.data={{"canTrade",r.tradingAllowed},{"hasWithdrawPermission",seen.contains("withdraw")}};
        for(const auto* k:{"acctLv","feeType","spotRoleType"})
            r.data.insert(QLatin1String(k),o.value(QLatin1String(k)).isString()&&safeText(str(o,k),16)?QJsonValue(str(o,k)):QJsonValue(QJsonValue::Null));
        for(const auto* k:{"enableSpotBorrow","autoLoan"}){
            const auto v=o.value(QLatin1String(k));
            r.data.insert(QLatin1String(k),v.isBool()?v:QJsonValue(QJsonValue::Null));
        }break;
    }
    case Operation::Account: case Operation::FundingBalance: {
        QJsonArray balances=rows;
        if(op==Operation::Account){if(!one()||!o.value("details").isArray())return bad();balances=o.value("details").toArray();}
        if(balances.size()>10000)return bad();QSet<QString> seen;
        for(const auto& v:balances){if(!v.isObject())return bad();const auto b=v.toObject();Balance x;x.asset=str(b,"ccy");
            if(!binanceIdentifierValid(x.asset)||seen.contains(x.asset))return bad();seen.insert(x.asset);
            x.section=op==Operation::Account?"trading":"funding";x.free=str(b,"availBal");x.locked=str(b,"frozenBal");
            x.cashBalance=str(b,op==Operation::Account?"cashBal":"bal");x.equity=str(b,"eq");x.liability=str(b,"liab");
            for(const auto* k:{"availBal","frozenBal"}) {auto z=b.value(QLatin1String(k));if(!z.isString()||(!z.toString().isEmpty()&&!number(z.toString())))return bad();}
            for(const auto* k:{"cashBal","bal","eq","liab"}){auto z=b.value(QLatin1String(k));if(!z.isUndefined()&&!z.isNull()&&(!z.isString()||(!z.toString().isEmpty()&&!signedNumber(z.toString()))))return bad();}
            r.balances.append(x);
        }
        r.data={{"section",op==Operation::Account?"trading":"funding"}};break;
    }
    case Operation::Markets: case Operation::Rules: {
        if(op==Operation::Rules&&!one())return bad();QJsonArray instruments;QSet<QString> ids;
        for(const auto& v:rows){if(!v.isObject())return bad();const auto item=v.toObject();const auto id=str(item,"instId"),base=str(item,"baseCcy"),quote=str(item,"quoteCcy");
            if(str(item,"instType")!="SPOT"||!okxInstrumentValid(id)||id!=base+'-'+quote||ids.contains(id)||!safeText(str(item,"state"),40))return bad();ids.insert(id);
            if(op==Operation::Markets){instruments.append(QJsonObject{{"symbol",id},{"baseAsset",base},{"quoteAsset",quote},{"name",id},
                {"status",str(item,"state")},{"tradable",str(item,"state")=="live"},{"warning",str(item,"state")=="live"?"":"현재 거래 상태를 확인해야 합니다."}});continue;}
            if(id!=in.symbol)return bad();
            for(const auto* k:{"tickSz","lotSz","minSz"})if(!positive(str(item,k)))return bad();
            for(const auto* k:{"maxLmtSz","maxMktSz","maxLmtAmt","maxMktAmt"}) {
                const auto z=item.value(QLatin1String(k));if(!z.isString()||(!z.toString().isEmpty()&&!positive(z.toString())))return bad();
            }
            if(!integer(str(item,"groupId"))||!item.value("tradeQuoteCcyList").isArray()||!item.value("upcChg").isArray())return bad();
            for(const auto* k:{"instType","instId","baseCcy","quoteCcy","state","groupId","tickSz","lotSz","minSz","maxLmtSz","maxMktSz","maxLmtAmt","maxMktAmt","listTime","contTdSwTime","openType","ruleType"})
                r.data.insert(QLatin1String(k),item.value(QLatin1String(k)));
            r.data.insert("tradeQuoteCcyList",item.value("tradeQuoteCcyList"));r.data.insert("upcChg",item.value("upcChg"));
            r.data.insert("symbol",id);r.data.insert("baseAsset",base);r.data.insert("quoteAsset",quote);
        }
        if(op==Operation::Markets)r.data={{"instruments",instruments}};break;
    }
    case Operation::Quote:
        if(!one()||str(o,"instId")!=in.symbol||!positive(str(o,"last"))||!positive(str(o,"bidPx"))||!positive(str(o,"askPx"))||!millis(str(o,"ts")))return bad();
        r.data={{"symbol",in.symbol},{"lastPrice",str(o,"last")},{"price",str(o,"last")},{"bid",str(o,"bidPx")},{"ask",str(o,"askPx")},{"ts",str(o,"ts")},{"timestamp",str(o,"ts")}};break;
    case Operation::Depth: {
        if(!one()||!millis(str(o,"ts")))return bad();r.data={{"symbol",in.symbol},{"ts",str(o,"ts")},{"timestamp",str(o,"ts")}};
        for(const auto* side:{"bids","asks"}){if(!o.value(QLatin1String(side)).isArray())return bad();QJsonArray normalized;QString previous;
            const auto levels=o.value(QLatin1String(side)).toArray();if(levels.size()>100)return bad();
            for(const auto& v:levels){const auto pair=v.toArray();if(!v.isArray()||pair.size()!=4||!positive(pair[0].toString())||!positive(pair[1].toString()))return bad();
                const auto price=pair[0].toString();if(!previous.isEmpty()&&((QByteArray(side)=="bids"&&decimal::compare(price,previous)>=0)||(QByteArray(side)=="asks"&&decimal::compare(price,previous)<=0)))return bad();
                previous=price;normalized.append(QJsonArray{pair[0],pair[1]});}
            r.data.insert(QLatin1String(side),normalized);
        }break;
    }
    case Operation::BuyingPower:
        if(!one()||str(o,"instId")!=in.symbol||!number(str(o,"availBuy"))||!number(str(o,"availSell")))return bad();
        r.data={{"symbol",in.symbol},{"availBuy",str(o,"availBuy")},{"availSell",str(o,"availSell")}};break;
    case Operation::Commissions: {
        if(!one()||str(o,"instType")!="SPOT"||!o.value("feeGroup").isArray())return bad();QJsonArray groups;QSet<QString> seen;
        for(const auto& v:o.value("feeGroup").toArray()){auto g=v.toObject();const auto id=str(g,"groupId");if(!v.isObject()||!integer(id)||seen.contains(id)||!signedNumber(str(g,"maker"))||!signedNumber(str(g,"taker")))return bad();
            seen.insert(id);groups.append(QJsonObject{{"groupId",id},{"maker",str(g,"maker")},{"taker",str(g,"taker")}});}
        r.data={{"instType","SPOT"},{"symbol",in.symbol},{"feeGroup",groups}};break;
    }
    case Operation::PriceLimits:
        if(!one()||str(o,"instId")!=in.symbol||!o.value("enabled").isBool()||!millis(str(o,"ts")))return bad();
        if(o.value("enabled").toBool()&&(!positive(str(o,"buyLmt"))||!positive(str(o,"sellLmt"))))return bad();
        r.data={{"symbol",in.symbol},{"buyLmt",nullable(str(o,"buyLmt"))},{"sellLmt",nullable(str(o,"sellLmt"))},{"enabled",o.value("enabled")},{"ts",str(o,"ts")},{"timestamp",str(o,"ts")}};break;
    case Operation::FxUsd: case Operation::FxUsdt: {
        const QString target=op==Operation::FxUsd?"USD":"USDT",quote=in.symbol.section('-',1,1);const bool identity=target==quote;
        if(!one()||!okxInstrumentValid(in.symbol)||!millis(str(o,"ts"))||(!identity&&(str(o,"instId")!=quote+'-'+target||!positive(str(o,"idxPx")))))return bad();
        r.data={{"indexId",identity?QString{}:str(o,"instId")},{"sourceCurrency",quote},{"targetCurrency",target},{"rate",identity?QString("1"):str(o,"idxPx")},
            {"identity",identity},{"ts",str(o,"ts")},{"timestamp",str(o,"ts")}};break;
    }
    case Operation::OpenOrders: case Operation::History: case Operation::QueryOrder: {
        if((op==Operation::QueryOrder&&!one())||rows.size()>100)return bad();QSet<QString> seen;QString last;
        for(const auto& v:rows){OrderRecord item;if(!v.isObject()||!parseOkxOrder(v.toObject(),OkxOrderSource::Rest,&item)||seen.contains(item.orderId)||item.instType!="SPOT")return bad();
            if(op!=Operation::OpenOrders&&item.symbol!=in.symbol)return bad();
            if(op==Operation::QueryOrder&&((!in.orderId.isEmpty()&&item.orderId!=in.orderId)||(in.orderId.isEmpty()&&item.clientId!=in.clientId)))return bad();
            if(op==Operation::OpenOrders&&item.status!="open"&&item.status!="partial")return bad();
            seen.insert(item.orderId);r.orders.append(item);last=item.orderId;
        }
        if(op!=Operation::QueryOrder&&rows.size()==100){if(last.isEmpty()||last==in.cursor)return bad();r.hasMore=true;r.nextCursor=last;}
        r.scope=op==Operation::OpenOrders?"연결한 OKX 현물의 일반 미체결 주문. 별도 algo 목록은 포함하지 않습니다.":
            "선택 종목의 최근 7일 조회 범위. 미체결 취소 주문의 짧은 보존기간 등으로 전체 이력을 보장하지 않습니다.";break;
    }
    default:return bad();
    }
    return r;
}

OrderValidation OkxAdapter::validate(const OrderInput& in,const MarketSnapshot& s,const OrderLimits& limits) const {
    OrderValidation v;auto no=[&](const QString& m){v.message=m;return v;};
    const auto config=s.data.value(Operation::AccountConfig),rules=s.data.value(Operation::Rules),power=s.data.value(Operation::BuyingPower);
    const QString mode=str(config,"acctLv"),base=str(rules,"baseAsset"),quote=str(rules,"quoteAsset");
    const bool buy=in.side=="buy",market=in.type=="market",amend=!in.orderId.isEmpty();
    if(!config.value("canTrade").toBool())return no("현재 키의 trade 권한을 확인해야 합니다.");
    if((mode!="1"&&mode!="2")||str(config,"spotRoleType")!="0")return no("일반 계정 모드 1/2의 cash 현물 범위만 신규·정정을 준비합니다.");
    if(mode=="1"&&(!config.value("enableSpotBorrow").isBool()||config.value("enableSpotBorrow").toBool()))return no("현물 자동 차입이 꺼진 상태를 확인해야 합니다.");
    if(!okxInstrumentValid(in.symbol)||str(rules,"symbol")!=in.symbol||base+'-'+quote!=in.symbol||str(rules,"state")!="live"||
        str(rules,"instType")!="SPOT"||str(power,"symbol")!=in.symbol)return no("해당 계정의 종목·cash 거래 가능 정보를 확인해야 합니다.");
    if(!rules.value("tradeQuoteCcyList").isArray()||!rules.value("tradeQuoteCcyList").toArray().contains(quote))return no("이 종목의 실제 결제 통화를 확인하지 못했습니다.");
    const auto now=s.serverNowMs;
    if(now<=0)return no("보정된 OKX 서버 시각을 확인해야 합니다.");
    const auto listing=str(rules,"listTime"),continuous=str(rules,"contTdSwTime");
    if(!millis(listing)||millis(listing)>now||(!continuous.isEmpty()&&(!millis(continuous)||millis(continuous)>now)))return no("상장·연속매매 전환 시각을 확인해야 합니다.");
    if(continuous.isEmpty()&&str(rules,"openType")!="fix_price")return no("연속매매 전환 여부가 미확인입니다.");
    if(!rules.value("upcChg").isArray())return no("예정된 종목 규칙 변경을 확인해야 합니다.");
    for(const auto& value:rules.value("upcChg").toArray()){const auto x=value.toObject();if(!value.isObject()||!millis(str(x,"effTime"))||millis(str(x,"effTime"))<=now)return no("종목 규칙 변경 시각이 지났거나 불명확합니다. 다시 조회하세요.");}
    if((!buy&&in.side!="sell")||(!market&&in.type!="limit"))return no("일반 지정가·시장가 매수·매도만 입력하세요.");
    if((!market&&(!positive(in.price)||!positive(in.quantity)||!in.quoteAmount.isEmpty()))||
        (market&&(!in.price.isEmpty()||(buy&&(!positive(in.quoteAmount)||!in.quantity.isEmpty()))||(!buy&&(!positive(in.quantity)||!in.quoteAmount.isEmpty())))))
        return no("지정가 가격·수량 또는 시장가 매수 총액·매도 수량을 확인하세요.");
    const auto tick=str(rules,"tickSz"),lot=str(rules,"lotSz"),minimum=str(rules,"minSz");
    if(!positive(tick)||!positive(lot)||!positive(minimum))return no("가격·수량 단위를 확인해야 합니다.");
    if(!market&&!decimal::multipleOf(in.price,tick))return no("지정가가 호가 단위에 맞지 않습니다.");
    if(!(market&&buy)&&(!decimal::multipleOf(in.quantity,lot)||decimal::compare(in.quantity,minimum)<0))return no("주문 수량이 단위·최소 수량에 맞지 않습니다.");
    const auto ticker=s.data.value(Operation::Quote),depth=s.data.value(Operation::Depth);const auto last=str(ticker,"price");
    if(str(ticker,"symbol")!=in.symbol||!positive(last)||!number(limits.maxDeviationPercent)||!positive(limits.maxNotional)||limits.currency!=quote)
        return no("현재 시세와 결제 통화에 맞는 사용자 금액·괴리율 한도가 필요합니다.");
    QString worst=in.price;v.currency=quote;v.notional=market&&buy?in.quoteAmount:decimal::multiply(in.quantity,market?last:in.price);
    if(market){
        if(str(depth,"symbol")!=in.symbol)return no("시장가 주문에는 해당 종목의 최신 호가가 필요합니다.");
        const auto values=depth.value(buy?"asks":"bids");if(!values.isArray()||values.toArray().isEmpty())return no("시장가 주문을 소화할 호가가 없습니다.");
        QString remaining=buy?in.quoteAmount:in.quantity,cost="0",previous;
        for(const auto& value:values.toArray()){
            const auto level=value.toArray();if(!value.isArray()||level.size()!=2||!positive(level[0].toString())||!positive(level[1].toString()))return no("호가 가격·잔량이 올바르지 않습니다.");
            const auto px=level[0].toString(),qty=level[1].toString();
            if(!previous.isEmpty()&&((buy&&decimal::compare(px,previous)<=0)||(!buy&&decimal::compare(px,previous)>=0)))return no("호가 순서가 올바르지 않습니다.");previous=px;
            const auto capacity=buy?decimal::multiply(px,qty):qty,take=less(remaining,capacity);
            remaining=decimal::subtract(remaining,take);cost=decimal::add(cost,buy?take:decimal::multiply(take,px));worst=px;
            if(remaining.isEmpty()||cost.isEmpty())return no("호가 계산 범위를 초과했습니다.");if(decimal::compare(remaining,"0")==0)break;
        }
        if(decimal::compare(remaining,"0")!=0)return no("조회한 호가 잔량으로 전체 시장가 주문을 충족하지 못합니다.");
        v.notional=cost;
        if(buy&&decimal::compare(in.quoteAmount,decimal::multiply(minimum,worst))<0)return no("시장가 총액으로 최소 base 수량을 충족하지 못합니다.");
    }
    if(!positive(v.notional)||decimal::compare(v.notional,limits.maxNotional)>0)return no("사용자 최대 주문 금액을 초과합니다.");
    const auto delta=decimal::compare(worst,last)>=0?decimal::subtract(worst,last):decimal::subtract(last,worst);
    if(!number(delta)||decimal::compare(decimal::multiply(delta,"100"),decimal::multiply(last,limits.maxDeviationPercent))>0)return no("주문 가격이 설정한 괴리율을 초과합니다.");
    auto convert=[&](Operation op,const QString& target)->QString{
        if(quote==target)return v.notional;
        const auto fx=s.data.value(op);if(str(fx,"sourceCurrency")!=quote||str(fx,"targetCurrency")!=target||str(fx,"indexId")!=quote+'-'+target||!positive(str(fx,"rate")))return {};
        const auto ts=millis(str(fx,"ts"));if(!ts||ts>now+2000||now-ts>10000)return {};
        return decimal::multiply(v.notional,str(fx,"rate"));
    };
    const auto usd=convert(Operation::FxUsd,"USD");const auto maxUsd=str(rules,market?"maxMktAmt":"maxLmtAmt");
    if(!positive(usd)||!positive(maxUsd))return no("USD 금액 상한 또는 정확한 공식 환산 지수가 미확인입니다.");
    if(decimal::compare(usd,maxUsd)>0)return no("공식 지수로 평가한 주문 금액이 USD 상한을 초과합니다.");
    if(market){auto usdt=convert(Operation::FxUsdt,"USDT"),max=str(rules,"maxMktSz");if(!positive(usdt)||!positive(max))return no("시장가 USDT 금액 상한 또는 공식 환산 지수가 미확인입니다.");
        if(decimal::compare(usdt,max)>0)return no("시장가의 USDT 금액 상한을 초과합니다.");
    }else{
        const auto max=str(rules,"maxLmtSz");if(!positive(max)||decimal::compare(in.quantity,max)>0)return no("지정가 최대 base 수량을 초과하거나 상한이 미확인입니다.");
        const auto bound=s.data.value(Operation::PriceLimits);if(str(bound,"symbol")!=in.symbol||!bound.value("enabled").isBool())return no("동적 가격 제한을 확인해야 합니다.");
        if(bound.value("enabled").toBool()){auto limit=str(bound,buy?"buyLmt":"sellLmt");if(!positive(limit)||(buy?decimal::compare(in.price,limit)>0:decimal::compare(in.price,limit)<0))return no("동적 주문 가격 제한을 벗어났습니다.");}
    }
    const auto fees=s.data.value(Operation::Commissions);QString maker,taker;int matches=0;
    if(str(fees,"instType")!="SPOT"||str(fees,"symbol")!=in.symbol)return no("종목별 적용 수수료를 확인해야 합니다.");
    for(const auto& x:fees.value("feeGroup").toArray()){const auto g=x.toObject();if(str(g,"groupId")==str(rules,"groupId")){++matches;maker=str(g,"maker");taker=str(g,"taker");}}
    if(matches!=1||!signedNumber(maker)||!signedNumber(taker)||(str(config,"feeType")!="0"&&str(config,"feeType")!="1"))return no("수수료 그룹·요율·차감 통화가 미확인입니다.");
    const auto mc=feeCost(maker),tc=feeCost(taker),fee=market?tc:(decimal::compare(mc,tc)>0?mc:tc);
    auto buyFunds=str(power,"availBuy"),sellFunds=str(power,"availSell"),freeQuote=available(s,quote),freeBase=available(s,base);
    if(!number(buyFunds)||!number(sellFunds))return no("cash 주문 가능 금액·수량을 확인해야 합니다.");
    const OrderRecord* existing=nullptr;
    if(amend){for(const auto& x:s.openOrders)if(x.orderId==in.orderId&&x.symbol==in.symbol){if(existing)return no("정정 대상이 중복됩니다.");existing=&x;}
        if(!existing||!existing->amendable||existing->tdMode!="cash"||existing->instType!="SPOT"||existing->quantityUnit!="base"||
            existing->side!=in.side||existing->type!="limit"||market||!number(existing->executed)||!positive(existing->price)||!number(existing->remaining)||
            decimal::compare(in.quantity,existing->executed)<=0||
            (decimal::compare(in.price,existing->price)==0&&decimal::compare(in.quantity,existing->quantity)==0))return no("정정 대상·전체 수량·체결분을 다시 확인하세요.");
        // Reuse only this exact order's reserved principal; never all account frozen balances.
        if(buy){const auto reserve=decimal::multiply(existing->remaining,existing->price);if(number(freeQuote)){freeQuote=decimal::add(freeQuote,reserve);buyFunds=decimal::add(buyFunds,reserve);}}
        else if(number(freeBase)){freeBase=decimal::add(freeBase,existing->remaining);sellFunds=decimal::add(sellFunds,existing->remaining);}
    }
    const auto newRemaining=existing?decimal::subtract(in.quantity,existing->executed):in.quantity;
    if(buy){QString required=existing?decimal::multiply(newRemaining,in.price):v.notional;
        if(str(config,"feeType")=="1")required=decimal::add(required,decimal::multiply(required,fee));
        if(!number(freeQuote)||!positive(required)||decimal::compare(required,less(freeQuote,buyFunds))>0)return no("수수료를 고려한 trading cash 주문 가능 금액이 부족합니다.");
    }else if(!number(freeBase)||!positive(newRemaining)||decimal::compare(newRemaining,less(freeBase,sellFunds))>0)return no("trading 계정의 매도 가능 base 수량이 부족합니다.");
    v.ok=true;v.message="cash 현물 사전 검사를 통과했습니다. 환산 금액은 공식 지수 기반 추정이며 서버가 최종 검증합니다.";return v;
}
} // namespace keefi
