#include "keefi/CoinFlowProtocol.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>

namespace keefi {
namespace {
constexpr qint64 CatalogFreshness=300000;
// C6 accepts bounded source lexemes wider than the shared trading decimal
// grammar. Compare their canonical non-negative values without narrowing them.
int compareDecimal(const QString& first,const QString& second){
    const auto firstPoint=first.indexOf('.'),secondPoint=second.indexOf('.');
    const auto firstWhole=firstPoint<0?first:first.left(firstPoint);
    const auto secondWhole=secondPoint<0?second:second.left(secondPoint);
    if(firstWhole.size()!=secondWhole.size())return firstWhole.size()<secondWhole.size()?-1:1;
    const auto whole=QString::compare(firstWhole,secondWhole);if(whole)return whole<0?-1:1;
    const auto firstFraction=firstPoint<0?QString{}:first.mid(firstPoint+1);
    const auto secondFraction=secondPoint<0?QString{}:second.mid(secondPoint+1);
    for(qsizetype i=0;i<qMax(firstFraction.size(),secondFraction.size());++i){
        const auto a=i<firstFraction.size()?firstFraction.at(i):QChar('0');
        const auto b=i<secondFraction.size()?secondFraction.at(i):QChar('0');
        if(a!=b)return a<b?-1:1;
    }return 0;
}
bool decimalNumber(const QString& value,bool positive=false){
    static const QRegularExpression pattern("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z");
    static const QString maximum="1"+QString(100,'0');
    if(value.size()>512||!pattern.match(value).hasMatch()||compareDecimal(value,maximum)>0)return false;
    if(!positive)return true;for(const auto ch:value)if(ch>='1'&&ch<='9')return true;return false;
}
bool id(const QString& value){static const QRegularExpression pattern("\\A(?:0|[1-9][0-9]{0,39})\\z");return pattern.match(value).hasMatch();}
bool timestamp(const QJsonValue& raw,qint64 now,qint64* result){
    const auto value=raw.toString();bool ok=false;if(!id(value)||value.size()>19)return false;const auto number=value.toLongLong(&ok);
    if(!ok||number<=0||number>now)return false;*result=number;return true;
}
bool upbitSymbol(const QString& value){static const QRegularExpression pattern("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,40}\\z");return pattern.match(value).hasMatch();}
QString identity(CoinFlowVenue venue,const QString& symbol){return (venue==CoinFlowVenue::UpbitSpot?"coin:upbit:":venue==CoinFlowVenue::BinanceSpot?"coin:binance:":"usdm_futures:binance:")+symbol;}
bool safeText(const QString& value,int limit=80){if(value.isEmpty()||value.size()>limit)return false;for(const auto ch:value)if(ch.isSpace()||ch.category()==QChar::Other_Control||ch.category()==QChar::Other_Format||ch.isSurrogate())return false;return true;}
bool instrumentValid(const CoinFlowInstrument& item,qint64 at){
    if(!item.verified||item.identity!=identity(item.venue,item.symbol)||item.evidence.receivedAtMs<=0||item.evidence.receivedAtMs>at||at-item.evidence.receivedAtMs>CatalogFreshness||item.evidence.source.isEmpty()||item.evidence.sourceUrl.isEmpty())return false;
    if(item.venue==CoinFlowVenue::UpbitSpot)return item.grain==CoinFlowGrain::UpbitTrade&&upbitSymbol(item.symbol)&&item.quoteAsset==item.symbol.section('-',0,0)&&item.baseAsset==item.symbol.section('-',1,1);
    if(!binanceIdentifierValid(item.baseAsset)||!binanceIdentifierValid(item.quoteAsset)||item.status!="TRADING")return false;
    if(item.venue==CoinFlowVenue::BinanceSpot)return item.grain==CoinFlowGrain::SpotAggregate&&binanceIdentifierValid(item.symbol,2)&&item.symbol==item.baseAsset+item.quoteAsset;
    return item.grain==CoinFlowGrain::Futures100msAggregate&&binanceUsdmSymbolValid(item.symbol)&&item.contractType=="PERPETUAL"&&item.quoteAsset=="USDT"&&item.marginAsset=="USDT"&&item.underlyingType=="COIN";
}
bool httpValid(const HttpResult& response,int bound=512*1024){return response.status==200&&!response.timedOut&&!response.networkError&&response.localRetryAfterSeconds<=0&&response.body.size()<=bound&&response.body.isValidUtf8();}
CoinFlowEvidence evidence(CoinFlowVenue venue,CoinFlowRead read,qint64 at){return {venue==CoinFlowVenue::UpbitSpot?"업비트 공개 체결":venue==CoinFlowVenue::BinanceSpot?"Binance Spot 단일 taker 집계":"Binance USD-M 100ms 집계 · q RPI 포함",CoinFlowProtocol::request(read,venue).url.toString(),0,at,at};}
QJsonObject unwrap(const CoinFlowInstrument& item,const QByteArray& raw,const QString& stream){
    if(raw.size()>65536||!raw.isValidUtf8())return {};const auto parsed=parseExactJson(raw);if(!parsed.isObject())return {};auto object=parsed.object();
    if(object.contains("stream")){if(item.venue!=CoinFlowVenue::BinanceUsdM||object.value("stream").toString()!=item.symbol.toLower()+stream||!object.value("data").isObject())return {};object=object.value("data").toObject();}
    return object;
}
bool trade(const CoinFlowInstrument& item,const QJsonObject& object,qint64 received,quint64 epoch,bool websocket,CoinTradeObservation* result){
    if(!epoch||!instrumentValid(item,received))return false;CoinTradeObservation value;value.epoch=epoch;value.receivedAtMs=value.knownAtMs=received;
    const bool upbit=item.venue==CoinFlowVenue::UpbitSpot;
    if(upbit){
        if(object.value(websocket?"code":"market").toString()!=item.symbol||(websocket&&object.value("type").toString()!="trade"))return false;
        value.id=object.value("sequential_id").toString();value.price=object.value("trade_price").toString();value.quantity=object.value("trade_volume").toString();
        const auto side=object.value("ask_bid").toString();if(side!="ASK"&&side!="BID")return false;value.side=side=="BID"?CoinFlowSide::Buy:CoinFlowSide::Sell;
        if(!timestamp(object.value(websocket?"trade_timestamp":"timestamp"),received,&value.tradeAtMs))return false;
        if(websocket){const auto type=object.value("stream_type").toString();if(type!="SNAPSHOT"&&type!="REALTIME")return false;value.snapshot=type=="SNAPSHOT";
            if(!timestamp(object.value("timestamp"),received,&value.eventAtMs))return false;value.liveEligible=!value.snapshot;}
    }else{
        if(websocket&&(object.value("e").toString()!="aggTrade"||object.value("s").toString()!=item.symbol))return false;
        if(object.contains("s")&&object.value("s").toString()!=item.symbol)return false;
        if(item.venue==CoinFlowVenue::BinanceUsdM&&object.contains("st")&&object.value("st").toString()!="1")return false;
        value.id=object.value("a").toString();value.price=object.value("p").toString();value.quantity=object.value("q").toString();
        if(!object.value("m").isBool()||!id(object.value("f").toString())||!id(object.value("l").toString()))return false;
        value.side=object.value("m").toBool()?CoinFlowSide::Sell:CoinFlowSide::Buy;if(!timestamp(object.value("T"),received,&value.tradeAtMs))return false;
        if(websocket){if(!timestamp(object.value("E"),received,&value.eventAtMs))return false;value.liveEligible=true;}
        if(item.venue==CoinFlowVenue::BinanceUsdM&&object.contains("nq")){const auto normal=object.value("nq").toString();if(!decimalNumber(normal)||!decimalNumber(value.quantity,true)||compareDecimal(normal,value.quantity)>0)return false;value.normalQuantity=normal;}
    }
    if(!id(value.id)||!decimalNumber(value.price,true)||!decimalNumber(value.quantity,true))return false;*result=value;return true;
}
}
RequestSpec CoinFlowProtocol::request(CoinFlowRead read,CoinFlowVenue venue,const QString& symbol,const CoinFlowPageCursor& cursor){
    RequestSpec result;result.operation=read==CoinFlowRead::Catalog?Operation::Markets:Operation::Reference;QUrlQuery query;QString host,path;
    const bool upbit=venue==CoinFlowVenue::UpbitSpot,spot=venue==CoinFlowVenue::BinanceSpot;
    host=upbit?"api.upbit.com":spot?"api.binance.com":"fapi.binance.com";
    if(read==CoinFlowRead::Catalog){
        if(!cursor.tradeId.isEmpty()||cursor.fromMs||cursor.throughMs||cursor.daysAgo!=-1)return {};
        path=upbit?"/v1/market/all":spot?"/api/v3/exchangeInfo":"/fapi/v1/exchangeInfo";
        if(upbit){query.addQueryItem("is_details","true");result.headers["Accept"]="application/json";}
        else if(spot){query.addQueryItem("permissions","SPOT");query.addQueryItem("showPermissionSets","false");}else result.headers["Accept"]="application/json";
    }else{
        if(upbit?!upbitSymbol(symbol):!binanceUsdmSymbolValid(symbol))return {};
        if(read==CoinFlowRead::OpenInterest&&venue!=CoinFlowVenue::BinanceUsdM)return {};
        query.addQueryItem(upbit?"market":"symbol",symbol);
        if(read==CoinFlowRead::OpenInterest){if(!cursor.tradeId.isEmpty()||cursor.daysAgo!=-1)return {};path="/futures/data/openInterestHist";query.addQueryItem("period","5m");query.addQueryItem("limit","30");}
        else{path=upbit?"/v1/trades/ticks":spot?"/api/v3/aggTrades":"/fapi/v1/aggTrades";query.addQueryItem(upbit?"count":"limit","500");}
        if(upbit){
            if(cursor.fromMs||cursor.throughMs||cursor.daysAgo==0||cursor.daysAgo>7||cursor.daysAgo< -1)return {};
            if(!cursor.tradeId.isEmpty()){if(!id(cursor.tradeId))return {};query.addQueryItem("cursor",cursor.tradeId);}if(cursor.daysAgo>0)query.addQueryItem("days_ago",QString::number(cursor.daysAgo));
        }else{
            if(cursor.daysAgo!=-1||((cursor.fromMs!=0)!=(cursor.throughMs!=0)))return {};
            if(!cursor.tradeId.isEmpty()){if(!id(cursor.tradeId)||cursor.fromMs||cursor.throughMs)return {};query.addQueryItem("fromId",cursor.tradeId);}
            if(cursor.fromMs||cursor.throughMs){if(cursor.fromMs<=0||cursor.throughMs<cursor.fromMs||(venue==CoinFlowVenue::BinanceUsdM&&read==CoinFlowRead::Trades&&cursor.throughMs-cursor.fromMs>=3600000))return {};query.addQueryItem("startTime",QString::number(cursor.fromMs));query.addQueryItem("endTime",QString::number(cursor.throughMs));}
        }
    }
    result.url=QUrl("https://"+host+path);if(!query.isEmpty())result.url.setQuery(query);return result;
}
bool CoinFlowProtocol::isRequest(const RequestSpec& value){
    if(!value.url.isValid()||value.url.scheme()!="https"||value.url.port(443)!=443||!value.url.userInfo().isEmpty()||value.url.hasFragment()||value.method!="GET"||value.authenticated||!value.body.isEmpty())return false;
    for(const auto venue:{CoinFlowVenue::UpbitSpot,CoinFlowVenue::BinanceSpot,CoinFlowVenue::BinanceUsdM}){
        auto expected=request(CoinFlowRead::Catalog,venue);if(value.operation==Operation::Markets&&value.url==expected.url&&value.headers==expected.headers)return true;
    }
    if(value.operation!=Operation::Reference||!value.headers.isEmpty())return false;const auto host=value.url.host(),path=value.url.path();CoinFlowVenue venue;
    if(host=="api.upbit.com"&&path=="/v1/trades/ticks")venue=CoinFlowVenue::UpbitSpot;
    else if(host=="api.binance.com"&&path=="/api/v3/aggTrades")venue=CoinFlowVenue::BinanceSpot;
    else if(host=="fapi.binance.com"&&(path=="/fapi/v1/aggTrades"||path=="/futures/data/openInterestHist"))venue=CoinFlowVenue::BinanceUsdM;else return false;
    const auto read=path.endsWith("openInterestHist")?CoinFlowRead::OpenInterest:CoinFlowRead::Trades;QMap<QString,QString> parameters;
    for(const auto& [key,item]:QUrlQuery(value.url).queryItems(QUrl::FullyDecoded)){if(parameters.contains(key)||item.isEmpty())return false;parameters.insert(key,item);}
    CoinFlowPageCursor cursor;cursor.tradeId=parameters.value(venue==CoinFlowVenue::UpbitSpot?"cursor":"fromId");bool ok=true;
    if(parameters.contains("days_ago")){cursor.daysAgo=parameters["days_ago"].toInt(&ok);if(!ok)return false;}
    for(const auto& key:{QString("startTime"),QString("endTime")})if(parameters.contains(key)){bool valid=false;if(!id(parameters[key]))return false;const auto at=parameters[key].toLongLong(&valid);if(!valid)return false;(key=="startTime"?cursor.fromMs:cursor.throughMs)=at;}
    const auto expected=request(read,venue,parameters.value(venue==CoinFlowVenue::UpbitSpot?"market":"symbol"),cursor);
    return expected.url.isValid()&&QUrlQuery(expected.url).queryItems(QUrl::FullyDecoded)==QUrlQuery(value.url).queryItems(QUrl::FullyDecoded);
}
QUrl CoinFlowProtocol::streamEndpoint(const CoinFlowInstrument& item){
    if(!item.verified)return {};if(item.venue==CoinFlowVenue::UpbitSpot)return upbitSymbol(item.symbol)?QUrl("wss://api.upbit.com/websocket/v1"):QUrl{};
    if(!binanceUsdmSymbolValid(item.symbol))return {};if(item.venue==CoinFlowVenue::BinanceSpot)return QUrl("wss://stream.binance.com:9443/ws/"+item.symbol.toLower()+"@aggTrade");
    QUrl url("wss://fstream.binance.com/market/stream");QUrlQuery query;query.addQueryItem("streams",item.symbol.toLower()+"@aggTrade/"+item.symbol.toLower()+"@forceOrder");url.setQuery(query);return url;
}
CoinFlowCatalogResult CoinFlowProtocol::catalog(CoinFlowVenue venue,const HttpResult& response,qint64 received){
    CoinFlowCatalogResult result;result.reason="공개 종목 메타데이터 검증 실패";if(received<=0||!httpValid(response,16*1024*1024))return result;
    const auto document=parseExactJson(response.body,nullptr,venue==CoinFlowVenue::UpbitSpot?JsonResponseScope::Standard:JsonResponseScope::BinanceMarketCatalog);
    QJsonArray rows;if(venue==CoinFlowVenue::UpbitSpot){if(!document.isArray())return result;rows=document.array();}else{if(!document.isObject()||!document.object().value("symbols").isArray())return result;rows=document.object().value("symbols").toArray();}
    if(rows.isEmpty()||rows.size()>20000)return result;QSet<QString> seen;QList<CoinFlowInstrument> instruments;
    for(const auto& raw:rows){if(!raw.isObject())return result;const auto object=raw.toObject();CoinFlowInstrument item;item.venue=venue;item.symbol=object.value(venue==CoinFlowVenue::UpbitSpot?"market":"symbol").toString();if(seen.contains(item.symbol))return result;seen.insert(item.symbol);
        item.identity=identity(venue,item.symbol);item.evidence=evidence(venue,CoinFlowRead::Catalog,received);
        if(venue==CoinFlowVenue::UpbitSpot){if(!upbitSymbol(item.symbol))return result;item.baseAsset=item.symbol.section('-',1,1);item.quoteAsset=item.symbol.section('-',0,0);item.grain=CoinFlowGrain::UpbitTrade;item.verified=true;}
        else{item.baseAsset=object.value("baseAsset").toString();item.quoteAsset=object.value("quoteAsset").toString();item.status=object.value("status").toString();if(!binanceUsdmSymbolValid(item.symbol)||!binanceIdentifierValid(item.baseAsset)||!binanceIdentifierValid(item.quoteAsset)||!safeText(item.status,40))return result;
            if(venue==CoinFlowVenue::BinanceSpot){if(item.symbol!=item.baseAsset+item.quoteAsset||!object.value("isSpotTradingAllowed").isBool())return result;item.grain=CoinFlowGrain::SpotAggregate;item.verified=item.status=="TRADING"&&object.value("isSpotTradingAllowed").toBool();}
            else{item.marginAsset=object.value("marginAsset").toString();item.contractType=object.value("contractType").toString();item.underlyingType=object.value("underlyingType").toString();if(!binanceIdentifierValid(item.marginAsset)||!safeText(item.contractType,40)||!safeText(item.underlyingType,40))return result;item.grain=CoinFlowGrain::Futures100msAggregate;item.verified=item.status=="TRADING"&&item.contractType=="PERPETUAL"&&item.quoteAsset=="USDT"&&item.marginAsset=="USDT"&&item.underlyingType=="COIN";}
        }instruments.append(item);
    }result.available=true;result.reason.clear();result.instruments=instruments;return result;
}
CoinTradeBatch CoinFlowProtocol::trades(const CoinFlowInstrument& item,const HttpResult& response,qint64 received,quint64 epoch){
    CoinTradeBatch result;result.reason="公開 체결 이력 검증 실패";if(!httpValid(response)||!instrumentValid(item,received))return result;const auto document=parseExactJson(response.body);if(!document.isArray()||document.array().size()>500)return result;QSet<QString> seen;QList<CoinTradeObservation> rows;
    for(const auto& raw:document.array()){CoinTradeObservation value;if(!raw.isObject()||!trade(item,raw.toObject(),received,epoch,false,&value)||seen.contains(value.id))return result;seen.insert(value.id);rows.append(value);}
    if(!rows.isEmpty())result.nextCursor=item.venue==CoinFlowVenue::UpbitSpot?rows.last().id:rows.last().id;
    std::stable_sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.tradeAtMs<b.tradeAtMs;});result.available=true;result.reason.clear();result.trades=rows;return result;
}
CoinTradeBatch CoinFlowProtocol::tradeFrame(const CoinFlowInstrument& item,const QByteArray& raw,qint64 received,quint64 epoch){CoinTradeBatch result;const auto object=unwrap(item,raw,"@aggTrade");CoinTradeObservation value;if(object.isEmpty()||!trade(item,object,received,epoch,true,&value)){result.reason="공개 실시간 체결 검증 실패";return result;}result.available=true;result.trades.append(value);return result;}
CoinOiBatch CoinFlowProtocol::openInterest(const CoinFlowInstrument& item,const HttpResult& response,qint64 received){
    CoinOiBatch result;result.reason="원천 5분 OI 이력 검증 실패";if(item.venue!=CoinFlowVenue::BinanceUsdM||!instrumentValid(item,received)||!httpValid(response))return result;const auto document=parseExactJson(response.body);if(!document.isArray()||document.array().size()>500)return result;QList<CoinOiPoint> rows;qint64 previous=0;
    for(const auto& raw:document.array()){if(!raw.isObject())return result;const auto object=raw.toObject();CoinOiPoint point;point.symbol=object.value("symbol").toString();point.quantity=object.value("sumOpenInterest").toString();point.value=object.value("sumOpenInterestValue").toString();
        if(point.symbol!=item.symbol||!decimalNumber(point.quantity)||!decimalNumber(point.value)||!timestamp(object.value("timestamp"),received,&point.periodEndMs)||point.periodEndMs%300000||point.periodEndMs<=previous)return result;previous=point.periodEndMs;point.knownAtMs=received;point.evidence={"Binance 원천 5분 OI · period END",request(CoinFlowRead::OpenInterest,item.venue,item.symbol).url.toString(),0,received,received};rows.append(point);
    }result.available=true;result.reason.clear();result.points=rows;return result;
}
CoinLiquidationBatch CoinFlowProtocol::liquidationFrame(const CoinFlowInstrument& item,const QByteArray& raw,qint64 received,quint64 epoch){
    CoinLiquidationBatch result;result.reason="원천 청산 snapshot 검증 실패";if(item.venue!=CoinFlowVenue::BinanceUsdM||!instrumentValid(item,received)||!epoch)return result;const auto object=unwrap(item,raw,"@forceOrder");if(object.value("e").toString()!="forceOrder"||!object.value("o").isObject()||(object.contains("st")&&object.value("st").toString()!="1"))return result;
    const auto order=object.value("o").toObject();CoinLiquidationObservation value;value.symbol=order.value("s").toString();value.side=order.value("S").toString();value.price=order.value("p").toString();value.averagePrice=order.value("ap").toString();value.originalQuantity=order.value("q").toString();value.lastFilledQuantity=order.value("l").toString();value.cumulativeFilledQuantity=order.value("z").toString();value.orderStatus=order.value("X").toString();
    if(value.symbol!=item.symbol||(value.side!="BUY"&&value.side!="SELL")||!decimalNumber(value.price)||!decimalNumber(value.averagePrice)||!decimalNumber(value.originalQuantity,true)||!decimalNumber(value.lastFilledQuantity)||!decimalNumber(value.cumulativeFilledQuantity)||compareDecimal(value.cumulativeFilledQuantity,value.originalQuantity)>0||compareDecimal(value.lastFilledQuantity,value.cumulativeFilledQuantity)>0||!safeText(value.orderStatus,40)||!timestamp(order.value("T"),received,&value.tradeAtMs)||!timestamp(object.value("E"),received,&value.eventAtMs))return result;
    value.epoch=epoch;value.receivedAtMs=value.knownAtMs=received;value.evidence={"Binance 관측 청산 · 종목별 1초 마지막 snapshot · 전체 아님",streamEndpoint(item).toString(),value.eventAtMs,received,received};result.available=true;result.reason.clear();result.observations.append(value);return result;
}
} // namespace keefi
