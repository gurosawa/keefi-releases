#include "keefi/TradingTransport.h"
#include "keefi/ResponseJournal.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/MarketCandles.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/EarningsCalendar.h"
#include "keefi/OfficialNotices.h"
#include "keefi/MarketOverview.h"
#include "keefi/WorldMarkets.h"
#include "keefi/MarketCompare.h"
#include "keefi/FxHistory.h"
#include "keefi/BuildPolicy.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/BinanceEarnRequests.h"
#include "keefi/FundFlowRequests.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/KiwoomMarketProtocol.h"
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <QMutexLocker>
#include <chrono>
#include <limits>
#include "keefi/OkxProtocol.h"
#include <QUrlQuery>
#include <QSet>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QRegularExpression>

namespace keefi {
namespace {
// Standalone transport authority for the bounded public flow collector. Keep
// this independent of its QObject implementation (transport-only test/tools).
bool allowedCoinFlowRead(const RequestSpec& r){
    if(r.operation!=Operation::Reference||r.authenticated||r.method!="GET"||
       !r.body.isEmpty()||!r.headers.isEmpty()||r.url.port(443)!=443)return false;
    const auto path=r.url.path(QUrl::FullyEncoded),host=r.url.host();
    const bool upbit=host=="api.upbit.com"&&path=="/v1/trades/ticks";
    const bool spot=host=="api.binance.com"&&path=="/api/v3/aggTrades";
    const bool futures=host=="fapi.binance.com"&&path=="/fapi/v1/aggTrades";
    const bool oi=host=="fapi.binance.com"&&path=="/futures/data/openInterestHist";
    if(!upbit&&!spot&&!futures&&!oi)return false;
    const QSet<QString> fields=upbit?QSet<QString>{"market","count","cursor","days_ago"}:
        oi?QSet<QString>{"symbol","period","limit","startTime","endTime"}:
           QSet<QString>{"symbol","limit","fromId","startTime","endTime"};
    QMap<QString,QString> values;
    for(const auto& [key,value]:QUrlQuery(r.url).queryItems(QUrl::FullyDecoded)){
        if(!fields.contains(key)||values.contains(key)||value.isEmpty()||value.size()>100)return false;
        values.insert(key,value);
    }
    const auto integer=[](const QString& value,bool wide=false){
        return QRegularExpression(wide?"\\A(?:0|[1-9][0-9]{0,39})\\z":"\\A(?:0|[1-9][0-9]{0,18})\\z").match(value).hasMatch();
    };
    if(upbit){
        if(values.value("count")!="500"||!QRegularExpression("\\A[A-Z0-9]{1,20}-[A-Z0-9]{1,40}\\z").match(values.value("market")).hasMatch())return false;
        if(values.contains("cursor")&&!integer(values["cursor"],true))return false;
        if(values.contains("days_ago")&&!QRegularExpression("\\A[1-7]\\z").match(values["days_ago"]).hasMatch())return false;
        return true;
    }
    const auto symbol=values.value("symbol");
    if(spot?!binanceIdentifierValid(symbol,2):!binanceUsdmSymbolValid(symbol))return false;
    if(values.value("limit")!=(oi?"30":"500")||(oi&&values.value("period")!="5m"))return false;
    if(values.contains("fromId")&&(!integer(values["fromId"],true)||values.contains("startTime")||values.contains("endTime")))return false;
    if(values.contains("startTime")!=values.contains("endTime"))return false;
    if(values.contains("startTime")){
        if(!integer(values["startTime"])||!integer(values["endTime"]))return false;
        bool firstOk=false,lastOk=false;const auto first=values["startTime"].toLongLong(&firstOk),last=values["endTime"].toLongLong(&lastOk);
        if(!firstOk||!lastOk||first<=0||last<first||(futures&&last-first>=3600000))return false;
    }
    return true;
}
bool stockHeaders(const RequestSpec& r){
    QSet<QByteArray> seen;
    for(auto i=r.headers.cbegin();i!=r.headers.cend();++i){const auto name=i.key().toLower();
        if(seen.contains(name)||i.value().isEmpty()||i.value().size()>16384)return false;seen.insert(name);
        for(const auto c:i.value())if(uchar(c)<32||uchar(c)==127)return false;
        if(name=="accept"){if(i.value()!="application/json")return false;}
        else if(name=="authorization"){if(!i.value().startsWith("Bearer ")||i.value().size()<=7)return false;}
        else return false;
    }return true;
}
bool allowedTossStockChart(const RequestSpec& r){
    if(r.operation!=Operation::Reference||r.method!="GET"||!r.authenticated||!r.body.isEmpty()||!stockHeaders(r))return false;
    const auto path=r.url.path(QUrl::FullyEncoded);QSet<QString> fields,required;
    if(path=="/api/v1/stocks")fields=required={"symbols"};
    else if(path=="/api/v1/candles"){fields={"symbol","interval","count","adjusted","before"};required={"symbol","interval","count","adjusted"};}
    else if(path=="/api/v1/price-limits")fields=required={"symbol"};
    else if(path=="/api/v1/market-calendar/KR"||path=="/api/v1/market-calendar/US")fields={"date"};
    else if(QRegularExpression("\\A/api/v1/stocks/[A-Z0-9]{6}/warnings\\z").match(path).hasMatch()){}
    else if(QRegularExpression("\\A/api/v1/stocks/[A-Z0-9]{6}/(?:investor-trading|program-trades)\\z").match(path).hasMatch())fields={"count","until"};
    else return false;
    QMap<QString,QString> values;
    for(const auto& [name,value]:QUrlQuery(r.url).queryItems(QUrl::FullyDecoded)){
        if(!fields.contains(name)||values.contains(name)||value.isEmpty()||value.size()>128)return false;
        for(const auto c:value)if(c.category()==QChar::Other_Control||c.category()==QChar::Other_Format)return false;
        values.insert(name,value);
    }
    for(const auto& name:required)if(!values.contains(name))return false;
    for(const auto* name:{"symbol","symbols"})if(values.contains(name)&&!QRegularExpression("\\A[A-Z0-9][A-Z0-9.\\-]{0,39}\\z").match(values[name]).hasMatch())return false;
    if(values.contains("interval")&&!QStringList{"1m","1d"}.contains(values["interval"]))return false;
    if(values.contains("adjusted")&&values["adjusted"]!="true")return false;
    if(values.contains("count")){bool ok=false;const auto count=values["count"].toInt(&ok);if(!ok||count<1||count>(path=="/api/v1/candles"?200:100))return false;}
    for(const auto* name:{"date","until"})if(values.contains(name)){const auto date=QDate::fromString(values[name],"yyyy-MM-dd");if(!date.isValid()||date.toString("yyyy-MM-dd")!=values[name])return false;}
    if(values.contains("before")&&(!QRegularExpression("(?:Z|[+-][0-9]{2}:[0-9]{2})\\z").match(values["before"]).hasMatch()||!QDateTime::fromString(values["before"],Qt::ISODateWithMs).isValid()))return false;
    return true;
}
class NoCookies final : public QNetworkCookieJar {
  public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookiesForUrl(const QUrl&) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie>&, const QUrl&) override { return false; }
};
bool allowedHyperliquidReadRequest(const RequestSpec& request) {
    if (request.method != "POST" || request.authenticated || request.url.path(QUrl::FullyEncoded) != "/info" ||
        request.url.hasQuery() || request.body.size() > 16384 || request.headers.size() != 1) return false;
    const auto header = request.headers.cbegin();
    if (header.key().toLower() != "content-type" || header.value() != "application/json") return false;
    QString type;
    bool needsUser = true;
    switch (request.operation) {
    case Operation::AccountConfig: type = "userAbstraction"; break;
    case Operation::Rules: type = "metaAndAssetCtxs"; needsUser = false; break;
    case Operation::Account: type = "clearinghouseState"; break;
    case Operation::FundingBalance: type = "spotClearinghouseState"; break;
    default: return false;
    }
    const auto document = parseExactJson(request.body);
    if (!document.isObject()) return false;
    const auto object = document.object();
    // This producer sends Qt Compact JSON only. Canonical bytes reject duplicate
    // keys, escaped key aliases and other ambiguous bodies for this API alone.
    if (QJsonDocument(object).toJson(QJsonDocument::Compact) != request.body ||
        object.value("type") != QJsonValue(type) || object.size() != (needsUser ? 2 : 1)) return false;
    if (!needsUser) return true;
    const auto address = object.value("user").toString();
    // Keep this standalone transport boundary equivalent to the controller's validator.
    static const QRegularExpression addressPattern("\\A0x[0-9a-fA-F]{40}\\z");
    return addressPattern.match(address).hasMatch() && address.mid(2) != QString(40, '0');
}
// This is a second authority check after the adapter's order validation. Only
// routes and fields implemented by KEEFI may reach a spot exchange.
bool allowedSpotRequest(const RequestSpec& r) {
    const bool upbit=r.url.host()=="api.upbit.com";
    if(!upbit&&r.url.host()!="api.binance.com")return false;
    QString path;QByteArray method="GET";bool authenticated=false;
    QSet<QString> fields,required;
    const auto route=[&](const char* p,std::initializer_list<QString> names,std::initializer_list<QString> mandatory){path=p;fields=QSet<QString>(names);required=QSet<QString>(mandatory);};
    if(upbit) {
        switch(r.operation) {
        case Operation::Markets: route("/v1/market/all",{"is_details"},{});break;
        case Operation::Reference: route("/v1/orderbook/instruments",{"markets"},{"markets"});break;
        case Operation::Quote:
            route(r.url.path()=="/v1/ticker"?"/v1/ticker":"/v1/orderbook",{"markets"},{"markets"});break;
        case Operation::Account: route("/v1/accounts",{},{});authenticated=true;break;
        case Operation::Rules: route("/v1/orders/chance",{"market"},{"market"});authenticated=true;break;
        case Operation::OpenOrders: route("/v1/orders/open",{"states[]","page","limit","order_by"},{"states[]","page","limit","order_by"});authenticated=true;break;
        case Operation::History: route("/v1/orders/closed",{"states[]","start_time","end_time","limit","order_by"},{"states[]","start_time","end_time","limit","order_by"});authenticated=true;break;
        case Operation::QueryOrder:
            route("/v1/order",{"uuid","identifier"},{});authenticated=true;break;
        default:return false;
        }
    } else {
        switch(r.operation) {
        case Operation::Time: route("/api/v3/time",{},{});break;
        case Operation::Markets: route("/api/v3/exchangeInfo",{"permissions","showPermissionSets"},{});break;
        case Operation::Rules: route("/api/v3/exchangeInfo",{"symbol"},{"symbol"});break;
        case Operation::Quote: route("/api/v3/ticker/price",{"symbol"},{"symbol"});break;
        case Operation::Depth: route("/api/v3/depth",{"symbol","limit"},{"symbol","limit"});break;
        case Operation::Reference: route("/api/v3/referencePrice",{"symbol"},{"symbol"});break;
        case Operation::AveragePrice: route("/api/v3/avgPrice",{"symbol"},{"symbol"});break;
        case Operation::AssetFilters: route("/api/v3/myFilters",{"symbol"},{"symbol"});authenticated=true;break;
        case Operation::Account: route("/api/v3/account",{},{});authenticated=true;break;
        case Operation::OpenOrders: route("/api/v3/openOrders",{},{});authenticated=true;break;
        case Operation::History: route("/api/v3/allOrders",{"symbol","limit","startTime","endTime","orderId"},{"symbol","limit"});authenticated=true;break;
        case Operation::OrderList: route("/api/v3/orderList",{"orderListId"},{"orderListId"});authenticated=true;break;
        case Operation::QueryOrder:
             route("/api/v3/order",{"symbol","orderId","origClientOrderId"},{"symbol"});
            authenticated=true;break;
        default:return false;
        }
    }
    if(r.url.path(QUrl::FullyEncoded)!=path||r.method!=method||r.authenticated!=authenticated||r.body.size()>16384||r.url.query(QUrl::FullyEncoded).size()>16384)return false;
    QSet<QByteArray> headers;
    for(auto it=r.headers.cbegin();it!=r.headers.cend();++it) {
        const auto name=it.key().toLower();
        const auto authHeader=upbit?QByteArray("authorization"):QByteArray("x-mbx-apikey");
        if(headers.contains(name)||!QSet<QByteArray>{"accept","content-type",authHeader}.contains(name)||it.value().isEmpty()||it.value().size()>16384)return false;
        for(const auto ch:it.value())if(uchar(ch)<32||uchar(ch)==127)return false;
        if(name==authHeader&&(!authenticated||(upbit&&!it.value().startsWith("Bearer "))))return false;
        headers.insert(name);
    }
    if(authenticated&&!headers.contains(upbit?"authorization":"x-mbx-apikey"))return false;
    const bool bodyParameters=method=="POST"||method=="PUT";
    if((bodyParameters&&r.url.hasQuery())||(!bodyParameters&&!r.body.isEmpty()))return false;
    QList<QPair<QString,QString>> parameters;
    if(bodyParameters&&upbit) {
        const auto document=parseExactJson(r.body);if(!document.isObject())return false;
        const auto object=document.object();for(auto it=object.begin();it!=object.end();++it){if(!it.value().isString())return false;parameters.append({it.key(),it.value().toString()});}
    } else parameters=(bodyParameters?QUrlQuery(QString::fromUtf8(r.body)):QUrlQuery(r.url)).queryItems(QUrl::FullyDecoded);
    if(!upbit&&authenticated){fields.unite({"timestamp","recvWindow","signature"});required.unite({"timestamp","recvWindow","signature"});}
    QMap<QString,QString> values;QSet<QString> states;
    for(const auto& [name,value]:parameters) {
        if(!fields.contains(name)||value.isEmpty()||value.size()>(upbit&&name=="markets"?4096:2048))return false;
        for(const auto ch:value)if(ch.isNull()||ch.category()==QChar::Other_Control)return false;
        if(upbit&&name=="states[]") {
            const auto allowed=r.operation==Operation::OpenOrders?QStringList{"wait","watch"}:QStringList{"done","cancel"};
            if(!allowed.contains(value)||states.contains(value))return false;states.insert(value);
        } else if(values.contains(name))return false;
        values[name]=value;
    }
    for(const auto& name:required)if(!values.contains(name))return false;
    if(upbit) {
        if(values.contains("is_details")&&values["is_details"]!="true")return false;
        if(r.operation==Operation::QueryOrder)
            return values.contains("uuid")!=values.contains("identifier");

    } else {
        if(values.contains("symbol")&&!binanceIdentifierValid(values["symbol"]))return false;
        if(values.contains("permissions")&&values["permissions"]!="SPOT")return false;
        if(values.contains("showPermissionSets")&&values["showPermissionSets"]!="false")return false;
        if(values.contains("cancelReplaceMode")&&values["cancelReplaceMode"]!="STOP_ON_FAILURE")return false;
        if(path=="/api/v3/order/cancelReplace"&&values["type"]!="LIMIT")return false;
        if(values.contains("orderRateLimitExceededMode")&&values["orderRateLimitExceededMode"]!="DO_NOTHING")return false;
        if(values.contains("type")&&!QStringList{"LIMIT","MARKET"}.contains(values["type"]))return false;
        if(values.contains("side")&&!QStringList{"BUY","SELL"}.contains(values["side"]))return false;
        if(values.contains("timeInForce")&&values["timeInForce"]!="GTC")return false;
        if(values.contains("newOrderRespType")&&values["newOrderRespType"]!="RESULT")return false;
        if((r.operation==Operation::QueryOrder)&&!values.contains("orderListId")&&!values.contains("cancelOrderId")&&
            (values.contains("orderId")==values.contains("origClientOrderId")))return false;
    }
    return true;
}
}
struct NetworkTradingTransport::Private {
    QNetworkAccessManager manager;
    QPointer<QNetworkReply> reply;
    std::shared_ptr<TradingRequestLimits> limits;
};
TradingRequestLimits::TradingRequestLimits(Clock clock) : clock_(std::move(clock)) {
    if(!clock_)clock_=[] { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); };
}
QString TradingRequestLimits::service(const QUrl& url) {
    const auto host=url.host();
    // Conservatively share live Binance limits across the spot, permissions and
    // futures hosts; test/demo environments never inherit live cooldowns.
    if(host=="api.binance.com"||host=="fapi.binance.com"||host=="ws-api.binance.com")return "binance-live";
    return host+":"+QString::number(url.port(443));
}
qint64 TradingRequestLimits::remainingMs(const QUrl& url) const {
    QMutexLocker lock(&mutex_);
    return qMax<qint64>(0,deadlines_.value(service(url))-clock_());
}
qint64 TradingRequestLimits::retryAfterMs(const HttpResult& response) {
    if(response.localRetryAfterSeconds>0)return qint64(response.localRetryAfterSeconds)*1000;
    if(response.status!=429&&response.status!=418)return 0;
    qint64 wait=60000;
    for(auto it=response.headers.cbegin();it!=response.headers.cend();++it){
        if(it.key().compare("Retry-After",Qt::CaseInsensitive)!=0)continue;
        bool ok=false;const auto seconds=it.value().trimmed().toLongLong(&ok);
        if(ok&&seconds>0)wait=qMax(wait,qMin<qint64>(seconds,std::numeric_limits<int>::max())*1000);
        else {
            // HTTP-date uses a literal GMT suffix, which Qt's RFC2822 parser
            // does not accept on every supported Qt runtime.
            const auto header=QString::fromLatin1(it.value()).trimmed();
            const auto http=QLocale::c().toDateTime(header,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");
            const auto until=http.isValid()?QDateTime(http.date(),http.time(),QTimeZone("UTC"))
                :QDateTime::fromString(header,Qt::RFC2822Date);
            if(until.isValid())wait=qMax(wait,QDateTime::currentDateTimeUtc().msecsTo(until));
        }
    }
    return wait;
}
void TradingRequestLimits::observe(const QUrl& url,const HttpResult& response) {
    if(response.localRetryAfterSeconds>0)return; // Never extend a server deadline with our own rejection.
    qint64 wait=retryAfterMs(response);
    if(url.host()=="api.upbit.com")for(auto it=response.headers.cbegin();it!=response.headers.cend();++it)
        if(it.key().compare("Remaining-Req",Qt::CaseInsensitive)==0){
            const auto match=QRegularExpression("(?:^|;)\\s*sec=([0-9]+)(?:;|$)").match(QString::fromLatin1(it.value()));
            if(match.hasMatch()&&match.captured(1).toLongLong()==0)wait=qMax<qint64>(wait,1100);
        }
    if(wait<=0)return;
    QMutexLocker lock(&mutex_);const auto now=clock_();
    for(auto it=deadlines_.begin();it!=deadlines_.end();)if(it.value()<=now)it=deadlines_.erase(it);else ++it;
    const auto key=service(url);deadlines_[key]=qMax(deadlines_.value(key),now+wait);
}
std::shared_ptr<TradingRequestLimits> TradingRequestLimits::shared() {
    static const auto shared=std::make_shared<TradingRequestLimits>();
    return shared;
}
NetworkTradingTransport::NetworkTradingTransport(QObject* parent,std::shared_ptr<TradingRequestLimits> limits)
    : TradingTransport(parent), d_(std::make_unique<Private>()) {
    d_->limits=limits?std::move(limits):TradingRequestLimits::shared();
    d_->manager.setProxy(QNetworkProxy::NoProxy);
    d_->manager.setCookieJar(new NoCookies(&d_->manager));
    d_->manager.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    d_->manager.setStrictTransportSecurityEnabled(true);
}
NetworkTradingTransport::~NetworkTradingTransport() { abort(); }
bool NetworkTradingTransport::allowed(const RequestSpec& request) {
    {
        // Permit known reads only. Existing route/verb/body validation below
        // still prevents a mutation disguised as a read operation.
        switch (request.operation) {
        case Operation::Time: case Operation::Markets: case Operation::Rules:
        case Operation::Quote: case Operation::Depth: case Operation::Reference:
        case Operation::AveragePrice: case Operation::AssetFilters: case Operation::Account:
        case Operation::OpenOrders: case Operation::History: case Operation::QueryOrder:
        case Operation::Authenticate: case Operation::Accounts: case Operation::Holdings:
        case Operation::BuyingPower: case Operation::SellableQuantity: case Operation::Commissions:
        case Operation::PriceLimits: case Operation::TradingSchedule: case Operation::AccountConfig:
        case Operation::FundingBalance: case Operation::FxUsd: case Operation::FxUsdt:
        case Operation::Positions: case Operation::SymbolConfig: case Operation::LeverageBrackets:
        case Operation::MarkPrice: case Operation::ApiRestrictions: case Operation::TradingStatus:
        case Operation::AlgoOpenOrders: case Operation::AlgoHistory: case Operation::AlgoQueryOrder:
        case Operation::AmendHistory: case Operation::OrderList: case Operation::Income: case Operation::EarnRead: case Operation::FundFlowRead:
            break;
        default: return false;
        }
    }
    const QUrl& u = request.url;
    if (!u.isValid() || u.scheme() != "https" || !u.userInfo().isEmpty() || u.hasFragment()) return false;
    if (isEarnRead(request.operation)) return allowedEarnRead(request);
    if (isFundFlowRead(request.operation)) return allowedFundFlowRead(request);
    if (u.host() == "api.hyperliquid.xyz")
        return u.port(443) == 443 && allowedHyperliquidReadRequest(request);
    if(allowedCoinFlowRead(request))return true;
    if(ChartPublicContextSource::isRequest(request))return true;
    // Keyless public market-map boards: only their exact unauthenticated GETs.
    if (PublicEquityBoard::isBoardRequest(request)) return true;
    if (PublicMarketHistory::isRequest(request)) return true;
    if (MarketMetrics::isRequest(request)) return true;
    if (TodayBtcPremium::isRequest(request)) return true;
    if (CoinbaseQuotes::isRequest(request)) return true;
    if (MarketCandles::isRequest(request)) return true;
    if (OfficialCalendar::isRequest(request)) return true;
    if (EarningsCalendar::isRequest(request)) return true; // KIND's fixed IR search form is the only POST read here.
    if (OfficialNotices::isRequest(request)) return true;
    if (MarketOverview::isRequest(request)) return true;
    if (WorldMarkets::isRequest(request)) return true; // Exact keyless Today world-market reads.
    if (MarketCompare::isRequest(request)) return true; // Exact keyless 시장 비교 klines and USD-M catalog.
    if (FxHistory::isRequest(request)) return true; // ECB full reference-rate history.
    // Kiwoom also uses these keyless KRX listing files. Neither suspended broker API is allowed.
    const bool kisCatalog = u.host() == "new.real.download.dws.co.kr" && u.port(443) == 443;
    const bool kiwoom = u.host() == "api.kiwoom.com" && u.port(443) == 443;
    const bool bithumb = u.host() == "api.bithumb.com" && u.port(443) == 443;
    const bool okx = QStringList{"openapi.okx.com", "us.okx.com", "eea.okx.com"}.contains(u.host()) && u.port(443) == 443;
    const bool usdm = u.host() == "fapi.binance.com" && u.port(443) == 443;
    const bool futuresPermissions = request.operation == Operation::ApiRestrictions && u.host() == "api.binance.com" && u.port(443) == 443;
    if (!kisCatalog && !kiwoom && !okx && !bithumb && !usdm && (u.port(443) != 443 ||
        !QStringList{"api.upbit.com", "api.binance.com", "openapi.tossinvest.com"}.contains(u.host()))) return false;
    if (!QList<QByteArray>{"GET", "POST"}.contains(request.method)) return false;
    if (usdm || futuresPermissions) {
        const bool publicRead = QList<Operation>{Operation::Time, Operation::Markets, Operation::Rules, Operation::Quote, Operation::Depth, Operation::MarkPrice}.contains(request.operation);
        if (request.authenticated == publicRead) return false;
        QSet<QByteArray> headers;
        for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
            const auto name = it.key().toLower();
            if (headers.contains(name) || !QSet<QByteArray>{"accept", "content-type", "x-mbx-apikey"}.contains(name) ||
                it.value().contains('\r') || it.value().contains('\n') || it.value().isEmpty()) return false;
            if (publicRead && name == "x-mbx-apikey") return false;
            headers.insert(name);
        }
        if (!publicRead && !headers.contains("x-mbx-apikey")) return false;
        QString path; QByteArray method = "GET"; QSet<QString> fields;
        switch(request.operation) {
        case Operation::Time: path="/fapi/v1/time"; break;
        case Operation::Markets: case Operation::Rules: path="/fapi/v1/exchangeInfo"; break;
        case Operation::Quote: path="/fapi/v2/ticker/price"; fields={"symbol"}; break;
        case Operation::Depth: path="/fapi/v1/depth"; fields={"symbol","limit"}; break;
        case Operation::MarkPrice: path="/fapi/v1/premiumIndex"; fields={"symbol"}; break;
        case Operation::AccountConfig: path="/fapi/v1/accountConfig"; break;
        case Operation::Account: path="/fapi/v3/account"; break;
        case Operation::Positions: path="/fapi/v3/positionRisk"; break;
        case Operation::SymbolConfig: path="/fapi/v1/symbolConfig"; fields={"symbol"}; break;
        case Operation::LeverageBrackets: path="/fapi/v1/leverageBracket"; fields={"symbol"}; break;
        case Operation::Commissions: path="/fapi/v1/commissionRate"; fields={"symbol"}; break;
        case Operation::TradingStatus: path="/fapi/v1/apiTradingStatus"; break;
        case Operation::ApiRestrictions: if(!futuresPermissions) return false; path="/sapi/v1/account/apiRestrictions"; break;
        case Operation::OpenOrders: path="/fapi/v1/openOrders"; break;
        case Operation::AlgoOpenOrders: path="/fapi/v1/openAlgoOrders"; break;
        case Operation::History: path="/fapi/v1/allOrders"; fields={"symbol","startTime","endTime","limit","orderId"}; break;
        case Operation::AlgoHistory: path="/fapi/v1/allAlgoOrders"; fields={"symbol","startTime","endTime","limit","algoId"}; break;
        case Operation::Income: path="/fapi/v1/income"; fields={"symbol","startTime","endTime","limit","page"}; break;
        case Operation::AmendHistory: path="/fapi/v1/orderAmendment"; fields={"symbol","orderId","origClientOrderId","startTime","endTime","limit"}; break;
        case Operation::QueryOrder: path="/fapi/v1/order"; fields={"symbol","orderId","origClientOrderId"}; break;
        case Operation::AlgoQueryOrder: path="/fapi/v1/algoOrder"; fields={"algoId","clientAlgoId"}; break;
        default: return false;
        }
        if(futuresPermissions && request.operation!=Operation::ApiRestrictions) return false;
        if(u.path(QUrl::FullyEncoded)!=path || request.method!=method || request.body.size()>16384) return false;
        const bool bodyParameters=method=="POST"||method=="PUT";
        if((bodyParameters&&u.hasQuery())||(!bodyParameters&&!request.body.isEmpty())) return false;
        if(!publicRead) fields.unite({"timestamp","recvWindow","signature"});
        QSet<QString> seen; const QUrlQuery query=bodyParameters?QUrlQuery(QString::fromUtf8(request.body)):QUrlQuery(u);
        for(const auto& item:query.queryItems(QUrl::FullyDecoded)) {
            if(!fields.contains(item.first)||seen.contains(item.first)||item.second.isEmpty()||item.second.size()>256||item.second.contains('\r')||item.second.contains('\n'))return false;
            if(item.first=="symbol"&&!binanceIdentifierValid(item.second))return false;
            seen.insert(item.first);
        }
        if(!publicRead&&(!seen.contains("timestamp")||!seen.contains("recvWindow")||!seen.contains("signature")))return false;

        return true;
    }
    if (bithumb) {
        const bool publicRead = QList<Operation>{Operation::Markets, Operation::Quote, Operation::Depth}.contains(request.operation);
        if (request.authenticated == publicRead) return false;
        QSet<QByteArray> headers;
        for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
            const auto name = it.key().toLower();
            if (headers.contains(name) || !QSet<QByteArray>{"accept", "content-type", "authorization"}.contains(name) ||
                it.value().contains('\r') || it.value().contains('\n')) return false;
            if (name == "authorization" && (publicRead || !it.value().startsWith("Bearer "))) return false;
            headers.insert(name);
        }
        if (!publicRead && !headers.contains("authorization")) return false;
        QString path;
        QByteArray method = "GET";
        QSet<QString> allowed;
        switch (request.operation) {
        case Operation::Markets: path = "/v1/market/all"; allowed = {"isDetails"}; break;
        case Operation::Quote: path = "/v1/ticker"; allowed = {"markets"}; break;
        case Operation::Depth: path = "/v1/orderbook"; allowed = {"markets"}; break;
        case Operation::Account: path = "/v1/accounts"; break;
        case Operation::Rules: path = "/v1/orders/chance"; allowed = {"market"}; break;
        case Operation::OpenOrders: path = "/v2/orders/pending"; allowed = {"state", "limit", "order_by", "next_key"}; break;
        case Operation::History: path = "/v2/orders/history"; allowed = {"market", "start_time", "end_time", "limit", "order_by", "next_key"}; break;
        case Operation::QueryOrder: path = "/v1/order"; allowed = {"uuid", "client_order_id"}; break;
        default: return false; // No transfer, withdrawal, invented test or amendment endpoint.
        }
        if (u.path(QUrl::FullyEncoded) != path || request.method != method) return false;

        if (!request.body.isEmpty()) return false;
        QSet<QString> seen;
        const QUrlQuery query(u);
        for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
            if (!allowed.contains(item.first) || seen.contains(item.first) || item.second.isEmpty() || item.second.size() > 4096) return false;
            if (item.first == "state" && item.second != "wait" && item.second != "watch") return false;
            if (item.first == "order_by" && item.second != "asc" && item.second != "desc") return false;
            if (item.first == "isDetails" && item.second != "true") return false;
            if ((item.first == "market" || item.first == "markets") &&
                !QRegularExpression("^(KRW|BTC)-[A-Z0-9]{1,24}$").match(item.second).hasMatch()) return false;
            if (item.first == "limit") {
                bool ok = false; const int limit = item.second.toInt(&ok);
                if (!ok || limit < 1 || limit > (request.operation == Operation::OpenOrders ? 100 : 1000)) return false;
            }
            seen.insert(item.first);
        }
        if (request.operation == Operation::QueryOrder)
            return seen.size() == 1 && (seen.contains("uuid") || seen.contains("client_order_id"));

        if (request.operation == Operation::OpenOrders) return seen.contains("state");
        if (request.operation == Operation::History) return seen.contains("market") && seen.contains("start_time") && seen.contains("end_time");
        return true;
    }
    if(okx){
        const auto path=u.path(QUrl::FullyEncoded);
        const bool publicRead=QList<Operation>{Operation::Time,Operation::Markets,Operation::Quote,Operation::Depth,Operation::PriceLimits,Operation::FxUsd,Operation::FxUsdt}.contains(request.operation);
        if(request.authenticated==publicRead)return false;
        QSet<QByteArray> seenHeaders;
        for(auto it=request.headers.cbegin();it!=request.headers.cend();++it){
            const auto key=it.key().toLower();if(seenHeaders.contains(key))return false;seenHeaders.insert(key);
            if(!QSet<QByteArray>{"accept","content-type","ok-access-key","ok-access-sign","ok-access-timestamp","ok-access-passphrase"}.contains(key))return false;
            if(publicRead&&key.startsWith("ok-access-"))return false;
            if(it.value().contains('\r')||it.value().contains('\n'))return false;
        }
        if(!publicRead)for(const auto& key:{QByteArray("ok-access-key"),QByteArray("ok-access-sign"),QByteArray("ok-access-timestamp"),QByteArray("ok-access-passphrase")})if(!seenHeaders.contains(key))return false;
        QString expected;
        switch(request.operation){
        case Operation::Time:expected="/api/v5/public/time";break;
        case Operation::Markets:expected="/api/v5/public/instruments";break;
        case Operation::AccountConfig:expected="/api/v5/account/config";break;
        case Operation::Account:expected="/api/v5/account/balance";break;
        case Operation::Positions:expected="/api/v5/account/positions";break;
        case Operation::FundingBalance:expected="/api/v5/asset/balances";break;
        case Operation::Rules:expected="/api/v5/account/instruments";break;
        case Operation::Commissions:expected="/api/v5/account/trade-fee";break;
        case Operation::BuyingPower:expected="/api/v5/account/max-avail-size";break;
        case Operation::Quote:expected="/api/v5/market/ticker";break;
        case Operation::Depth:expected="/api/v5/market/books";break;
        case Operation::PriceLimits:expected="/api/v5/public/price-limit";break;
        case Operation::FxUsd:case Operation::FxUsdt:
            expected=path=="/api/v5/public/time"?"/api/v5/public/time":"/api/v5/market/index-tickers";break;
        case Operation::OpenOrders:expected="/api/v5/trade/orders-pending";break;
        case Operation::History:expected="/api/v5/trade/orders-history";break;
        case Operation::QueryOrder:expected="/api/v5/trade/order";break;
        default:return false;
        }
        if(path!=expected)return false;

        if(request.method!="GET"||!request.body.isEmpty())return false;
        QSet<QString> allowed;
        switch(request.operation){
        case Operation::Positions:allowed={"instType"};break;
        case Operation::Markets:allowed={"instType"};break;
        case Operation::Rules:case Operation::Commissions:allowed={"instType","instId"};break;
        case Operation::Quote:case Operation::PriceLimits:allowed={"instId"};break;
        case Operation::Depth:allowed={"instId","sz"};break;
        case Operation::BuyingPower:allowed={"instId","tdMode","tradeQuoteCcy"};break;
        case Operation::OpenOrders:allowed={"instType","after","limit"};break;
        case Operation::History:allowed={"instType","instId","after","limit","begin","end"};break;
        case Operation::QueryOrder:allowed={"instId","ordId","clOrdId"};break;
        case Operation::FxUsd:case Operation::FxUsdt:if(path!="/api/v5/public/time")allowed={"instId"};break;
        default:break;
        }
        QSet<QString> seen;
        for(const auto& item:QUrlQuery(u).queryItems(QUrl::FullyDecoded)){
            if(!allowed.contains(item.first)||seen.contains(item.first)||item.second.isEmpty())return false;seen.insert(item.first);
            if(item.first=="instType") {
                if(request.operation==Operation::Positions) {
                    if(item.second!="SWAP"&&item.second!="FUTURES")return false;
                } else if(item.second!="SPOT")return false;
            }
            if(item.first=="tdMode"&&item.second!="cash")return false;
        }
        return request.operation!=Operation::Positions||seen.contains("instType");
    }
    if (kiwoom) {
        if (!request.authenticated || request.method != "POST" || u.hasQuery()) return false;
        QByteArray apiId;
        bool foundApiId = false;
        for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
            if (it.key().toLower() != "api-id") continue;
            if (foundApiId) return false;
            foundApiId = true; apiId = it.value();
        }
        // New display reads have an exact independent Reference contract. Existing
        // Broker operations below retain their original routing authority.
        if (request.operation == Operation::Reference && QSet<QByteArray>{"ka10099","ka10100","ka10001","ka10081","usa10099","usa10098","usa20100"}.contains(apiId)) {
            bool bearer = false;
            for (auto it=request.headers.cbegin();it!=request.headers.cend();++it)
                if (it.key().compare("authorization",Qt::CaseInsensitive)==0) bearer=!it.value().isEmpty();
            return bearer && KiwoomMarketProtocol::isRequest(request);
        }
        const QString path = u.path(QUrl::FullyEncoded), root = "/api/dostk/";
        if (request.operation == Operation::Authenticate)
            return path == "/oauth2/token" && (!foundApiId || apiId == "au10001");
        // Kiwoom uses POST for reads too. The operation and exact TR determine
        // authority; the HTTP method alone cannot distinguish a financial write.
        switch (request.operation) {
        case Operation::Accounts: return path == root + "acnt" && apiId == "ka00001";
        case Operation::Markets: return path == root + "stkinfo" && apiId == "ka10099";
        case Operation::Rules: return path == root + "stkinfo" && apiId == "ka10100";
        case Operation::Quote: return path == root + "stkinfo" && apiId == "ka10001";
        case Operation::Depth: return path == root + "mrkcond" && apiId == "ka10004";
        case Operation::Reference: return path == root + "chart" && apiId == "ka10080";
        case Operation::Holdings: return path == root + "acnt" && apiId == "kt00018";
        case Operation::Account: return path == root + "acnt" && apiId == "kt00001";
        case Operation::BuyingPower: return path == root + "acnt" && apiId == "kt00011";
        case Operation::Commissions: return path == root + "acnt" && apiId == "kt00010";
        case Operation::OpenOrders: return path == root + "acnt" && apiId == "ka10075";
        case Operation::History: case Operation::QueryOrder:
            return path == root + "acnt" && apiId == "kt00007";
        default: return false;
        }
    }
    if (kisCatalog) {
        if (request.operation != Operation::Markets || request.method != "GET" || request.authenticated ||
            !request.body.isEmpty() || u.hasQuery() ||
            !QStringList{"/common/master/kospi_code.mst.zip", "/common/master/kosdaq_code.mst.zip", "/common/master/konex_code.mst.zip"}.contains(u.path(QUrl::FullyEncoded))) return false;
        // No private headers may cross from the financial API to the public CDN.
        for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it)
            if (it.key().toLower() != "accept") return false;
        return true;
    }
    if (u.host() == "openapi.tossinvest.com") {
        const auto path = u.path(QUrl::FullyEncoded);
        if(request.operation==Operation::Reference)return allowedTossStockChart(request);
        if (request.operation == Operation::Authenticate)
            return request.method == "POST" && path == "/oauth2/token" && !u.hasQuery();
        if(request.operation==Operation::AlgoOpenOrders)return request.method=="GET"&&path=="/api/v1/conditional-orders";
        if(request.operation==Operation::AlgoQueryOrder)
            return request.method=="GET"&&!u.hasQuery()&&QRegularExpression("^/api/v1/conditional-orders/[A-Za-z0-9_-]{1,128}$").match(path).hasMatch();

        const QMap<Operation, QString> paths{{Operation::Accounts, "/api/v1/accounts"},
            {Operation::Markets, "/api/v1/stocks/all"}, {Operation::Rules, "/api/v1/stocks"},
            {Operation::Quote, "/api/v1/prices"}, {Operation::Depth, "/api/v1/orderbook"},
            {Operation::Holdings, "/api/v1/holdings"}, {Operation::BuyingPower, "/api/v1/buying-power"},
            {Operation::SellableQuantity, "/api/v1/sellable-quantity"}, {Operation::Commissions, "/api/v1/commissions"},
            {Operation::PriceLimits, "/api/v1/price-limits"}, {Operation::TradingSchedule, "/api/v1/market-calendar/KR"},
            {Operation::OpenOrders, "/api/v1/orders"}, {Operation::History, "/api/v1/orders"}};
        if (request.method != "GET") return false;
        if (request.operation == Operation::QueryOrder)
            return QRegularExpression("^/api/v1/orders/[A-Za-z0-9_-]{1,128}$").match(path).hasMatch();
        return paths.contains(request.operation) && path == paths.value(request.operation);
    }
    return allowedSpotRequest(request);
}
qsizetype NetworkTradingTransport::responseBodyLimit(const RequestSpec& request) {
    // ECB's full reference-rate history is about 8 MB once decoded; only that exact keyless read gets more room.
    if (FxHistory::isRequest(request) && allowed(request)) return FxHistory::MaximumEcbBytes;
    const bool spotCatalog = request.operation == Operation::Markets &&
        request.url.host() == "api.binance.com" && request.url.path(QUrl::FullyEncoded) == "/api/v3/exchangeInfo";
    const bool futuresCatalog = (request.operation == Operation::Markets || request.operation == Operation::Rules) &&
        request.url.host() == "fapi.binance.com" && request.url.path(QUrl::FullyEncoded) == "/fapi/v1/exchangeInfo";
    if (!allowed(request) || (!spotCatalog && !futuresCatalog) || request.method != "GET" ||
        request.authenticated || !request.body.isEmpty()) return MaxStandardResponseBytes;
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        const auto name = it.key().toLower();
        if (name == "authorization" || name == "proxy-authorization" || name == "x-mbx-apikey" || name == "cookie")
            return MaxStandardResponseBytes;
    }
    const auto parameters = QUrlQuery(request.url).queryItems(QUrl::FullyDecoded);
    for (const auto& parameter : parameters) {
        const auto name = parameter.first.toLower();
        if (name == "signature" || name == "timestamp" || name == "recvwindow") return MaxStandardResponseBytes;
    }
    return MaxBinanceMarketCatalogResponseBytes;
}
void NetworkTradingTransport::abort() {
    if (d_->reply) {
        auto reply = d_->reply;
        d_->reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort(); reply->deleteLater();
    }
    d_->manager.clearAccessCache();
    d_->manager.clearConnectionCache();
}
void NetworkTradingTransport::send(quint64 token, const RequestSpec& spec) {
    if (d_->reply || !allowed(spec)) {
        QTimer::singleShot(0, this, [this, token] { HttpResult result; result.networkError = true; emit finished(token, result); });
        return;
    }
    const auto wait=d_->limits->remainingMs(spec.url);
    if(wait>0){
        HttpResult result;result.status=429;
        result.localRetryAfterSeconds=int(qMin<qint64>((wait+999)/1000,std::numeric_limits<int>::max()));
        result.headers["Retry-After"]=QByteArray::number(result.localRetryAfterSeconds);
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});return;
    }
    QNetworkRequest request(spec.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(10000);
    for (auto it = spec.headers.cbegin(); it != spec.headers.cend(); ++it) request.setRawHeader(it.key(), it.value());
    request.setRawHeader("User-Agent", "KEEFI/0.2-local");
    auto* reply = d_->manager.sendCustomRequest(request, spec.method, spec.body);
    d_->reply = reply;
    struct Pending { QByteArray bytes; bool exceeded = false, timedOut = false; };
    auto pending = std::make_shared<Pending>();
    const qsizetype maximum = responseBodyLimit(spec);
    auto* timer = new QTimer(reply); timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, [reply, pending] { pending->timedOut = true; reply->abort(); });
    timer->start(10000);
    connect(reply, &QIODevice::readyRead, this, [reply, pending, maximum] {
        pending->bytes += reply->read(maximum - pending->bytes.size() + 1);
        if (pending->bytes.size() > maximum) { pending->exceeded = true; reply->abort(); }
    });
    QUrl origin;origin.setScheme(spec.url.scheme());origin.setHost(spec.url.host());origin.setPort(spec.url.port());
    connect(reply, &QNetworkReply::finished, this, [this, reply, pending, timer, token, origin,
                                                     operation = spec.operation, method = spec.method, url = spec.url, authenticated = spec.authenticated] {
        timer->stop();
        HttpResult result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.timedOut = pending->timedOut || reply->error() == QNetworkReply::TimeoutError;
        result.networkError = pending->exceeded || (reply->error() != QNetworkReply::NoError && result.status == 0);
        if (!pending->exceeded) result.body = pending->bytes;
        for (const auto& header : reply->rawHeaderPairs()) {
            const auto name = header.first.toLower();
            if (QList<QByteArray>{"cont-yn", "next-key", "api-id", "tr_cd", "tr_cont", "tr_cont_key"}.contains(name) &&
                result.headers.contains(name) && result.headers.value(name) != header.second) {
                // Ambiguous continuation metadata must not become a complete
                // account result through QMap's last-value overwrite.
                result.networkError = true;
            }
            result.headers.insert(name, header.second);
        }
        if (d_->reply == reply) d_->reply = nullptr;
        reply->deleteLater();
        d_->limits->observe(origin,result);
        ResponseJournal::recordTrading(operation, method, url, authenticated, result);
        emit finished(token, result);
    });
}
}
