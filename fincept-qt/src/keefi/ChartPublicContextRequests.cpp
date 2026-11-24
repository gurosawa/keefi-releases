// Fixed unsigned request construction and validation, also linked by standalone transports.
#include "keefi/ChartPublicContextSource.h"
#include "keefi/MarketMetrics.h"
#include <QRegularExpression>
#include <QUrlQuery>

namespace keefi {
namespace {
bool coin(const QString& id){return QRegularExpression("\\Acoin:(?:upbit:KRW-[A-Z0-9]{1,20}|binance:[A-Z0-9]{2,40})\\z").match(id).hasMatch();}
bool symbol(const QString& s){return QRegularExpression("\\A[A-Z0-9]{1,20}USDT\\z").match(s).hasMatch();}
}
RequestSpec ChartPublicContextSource::request(Read read,const QString& id,const QString& future){
    if(read==Read::Contracts)return MarketMetrics::request(MarketMetrics::Read::Contracts);
    if(read==Read::Funding)return symbol(future)?MarketMetrics::request(MarketMetrics::Read::Funding,future):RequestSpec{};
    RequestSpec r;r.operation=Operation::Reference;if(!coin(id))return r;QUrlQuery q;
    if(read==Read::Day||read==Read::Week||read==Read::Month){
        const auto suffix=read==Read::Day?"days":read==Read::Week?"weeks":"months";
        if(id.startsWith("coin:upbit:")){r.url=QUrl(QString("https://api.upbit.com/v1/candles/")+suffix);q.addQueryItem("market",id.section(':',2));q.addQueryItem("count",read==Read::Day?"10":read==Read::Week?"4":"3");}
        else{r.url=QUrl("https://api.binance.com/api/v3/klines");q.addQueryItem("symbol",id.section(':',2));q.addQueryItem("interval",read==Read::Day?"1d":read==Read::Week?"1w":"1M");q.addQueryItem("limit",read==Read::Day?"10":read==Read::Week?"4":"3");}
    }else if(read==Read::FuturesPrice){
        if(!symbol(future))return r;r.url=QUrl("https://fapi.binance.com/fapi/v1/klines");q.addQueryItem("symbol",future);q.addQueryItem("interval","1h");q.addQueryItem("limit","3");
    }else{
        if(!symbol(future))return r;const auto path=read==Read::OpenInterest?"openInterestHist":read==Read::TopPosition?"topLongShortPositionRatio":read==Read::Taker?"takerlongshortRatio":"";if(!*path)return r;
        r.url=QUrl(QString("https://fapi.binance.com/futures/data/")+path);q.addQueryItem("symbol",future);q.addQueryItem("period","1h");q.addQueryItem("limit",read==Read::OpenInterest?"200":"2");
    }
    r.url.setQuery(q);r.headers.insert("Accept","application/json");return r;
}
bool ChartPublicContextSource::isRequest(const RequestSpec& r){
    if(r.method!="GET"||r.authenticated||!r.body.isEmpty()||r.url.scheme()!="https"||r.url.port(443)!=443||!r.url.userInfo().isEmpty()||r.url.hasFragment())return false;
    const QUrlQuery q(r.url);QString id;
    if(r.url.host()=="api.upbit.com")id="coin:upbit:"+q.queryItemValue("market");
    else if(r.url.host()=="api.binance.com")id="coin:binance:"+q.queryItemValue("symbol");
    else id="coin:binance:"+q.queryItemValue("symbol");
    for(const auto read:{Read::Day,Read::Week,Read::Month,Read::OpenInterest,Read::TopPosition,Read::Taker,Read::FuturesPrice}){const auto expected=request(read,id,q.queryItemValue("symbol"));if(!expected.url.isEmpty()&&r.url==expected.url&&r.operation==expected.operation&&r.headers==expected.headers)return true;}
    return false; // Existing MarketMetrics owns Contracts/Funding authority.
}
}
