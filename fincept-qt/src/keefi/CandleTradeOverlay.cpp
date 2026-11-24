#include "keefi/CandleTradeOverlay.h"
#include <QRegularExpression>
#include <algorithm>
#include <limits>

namespace keefi {
namespace {
bool integer(const QJsonValue& value,qint64* result,int maximumDigits=16){
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9]{1,19}\\z"));
    const auto text=value.toString();bool ok=false;
    if(text.size()>maximumDigits||!pattern.match(text).hasMatch())return false;
    *result=text.toLongLong(&ok);return ok;
}
bool amount(const QString& value,bool positive=true){
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z"));
    return pattern.match(value).hasMatch()&&(!positive||decimal::positive(value));
}
bool validAnchor(const MarketCandle& row,qint64 interval){
    return interval>0&&row.startsAtMs>0&&row.startsAtMs%interval==0&&
        row.startsAtMs<=std::numeric_limits<qint64>::max()-interval&&
        amount(row.open)&&amount(row.high)&&amount(row.low)&&amount(row.close)&&amount(row.volume,false)&&
        decimal::compare(row.high,row.open)>=0&&decimal::compare(row.high,row.close)>=0&&
        decimal::compare(row.low,row.open)<=0&&decimal::compare(row.low,row.close)<=0;
}
}
void CandleTradeOverlay::reset(const QString& identity,const QString& provider,const QString& symbol,qint64 interval){
    identity_=identity;provider_=provider;symbol_=symbol;intervalMs_=interval;
    lastSeenId_=anchorId_=appliedId_=minimumAnchorId_=-1;lastSeenAtMs_=anchorAtMs_=0;
    buffered_.clear();upbitIds_.clear();latest_.reset();anchor_.reset();overlay_.reset();
}
bool CandleTradeOverlay::receive(const QJsonObject& object,qint64 received){
    if(identity_.isEmpty()||intervalMs_<=0||received<=0)return false;
    Trade trade;trade.receivedAtMs=received;
    if(provider_=="binance"){
        if(object.value("e").toString()!="trade"||object.value("s").toString()!=symbol_||
            !integer(object.value("t"),&trade.id,19)||!integer(object.value("T"),&trade.atMs)||
            !integer(object.value("E"),&trade.eventMs))return false;
        trade.price=object.value("p").toString();trade.quantity=object.value("q").toString();
    }else if(provider_=="upbit"){
        if(object.value("type").toString()!="trade"||object.value("code").toString()!=symbol_||
            object.value("stream_type").toString()!="REALTIME"||
            !integer(object.value("sequential_id"),&trade.id,19)||!integer(object.value("trade_timestamp"),&trade.atMs)||
            !integer(object.value("timestamp"),&trade.eventMs))return false;
        trade.price=object.value("trade_price").toString();trade.quantity=object.value("trade_volume").toString();
    }else return false;
    if(trade.atMs<=0||trade.eventMs<trade.atMs||trade.eventMs>received||
        received-trade.atMs>MarketCandles::FreshnessMs||!amount(trade.price)||!amount(trade.quantity))return false;
    trade.price=decimal::normalize(trade.price);trade.quantity=decimal::normalize(trade.quantity);
    if(provider_=="upbit"){
        if(trade.atMs<lastSeenAtMs_)return false;
        // sequential_id is a unique identifier, not a documented gap-free counter.
        if(upbitIds_.contains(trade.id))return false;
        upbitIds_.insert(trade.id);buffered_.append(trade);
        if(buffered_.size()>BufferLimit){upbitIds_.remove(buffered_.first().id);buffered_.removeFirst();}
        lastSeenAtMs_=trade.atMs;latest_=trade;return true;
    }
    if(trade.id<=anchorId_)return false; // already included in an official candle
    if(trade.id<=lastSeenId_){
        const auto found=std::lower_bound(buffered_.cbegin(),buffered_.cend(),trade.id,
            [](const Trade& value,qint64 id){return value.id<id;});
        if(found!=buffered_.cend()&&found->id==trade.id&&found->atMs==trade.atMs&&
            found->price==trade.price&&found->quantity==trade.quantity)return false;
        // An inconsistent duplicate or out-of-order unseen ID cannot repair an
        // already published tail. Wait for an authoritative snapshot covering it.
        minimumAnchorId_=qMax(minimumAnchorId_,lastSeenId_);
        const bool changed=overlay_.has_value();overlay_.reset();return changed;
    }
    if(trade.atMs<lastSeenAtMs_){
        lastSeenId_=trade.id;minimumAnchorId_=qMax(minimumAnchorId_,trade.id);
        const bool changed=overlay_.has_value();overlay_.reset();return changed;
    }
    lastSeenId_=trade.id;lastSeenAtMs_=trade.atMs;
    buffered_.append(trade);
    if(buffered_.size()>BufferLimit){
        minimumAnchorId_=qMax(minimumAnchorId_,buffered_.first().id);
        buffered_.removeFirst();overlay_.reset();
    }
    latest_=trade;
    if(overlay_&&!appendToOverlay(trade))overlay_.reset();
    return true;
}
bool CandleTradeOverlay::appendToOverlay(const Trade& trade){
    if(!overlay_||appliedId_==std::numeric_limits<qint64>::max()||trade.id!=appliedId_+1)return false;
    const auto start=trade.atMs/intervalMs_*intervalMs_;
    if(start<overlay_->startsAtMs||trade.atMs<overlay_->lastStoredTickMs)return false;
    if(start>overlay_->startsAtMs){
        // A contiguous tail may start a display-only new bar. The previous bar
        // is never promoted into the model's completed candle evidence here.
        *overlay_={start,trade.atMs,trade.price,trade.price,trade.price,trade.price,trade.quantity};
    }else{
        const auto volume=decimal::add(overlay_->volume,trade.quantity);
        if(!amount(volume,false))return false;
        if(decimal::compare(trade.price,overlay_->high)>0)overlay_->high=trade.price;
        if(decimal::compare(trade.price,overlay_->low)<0)overlay_->low=trade.price;
        overlay_->close=trade.price;overlay_->volume=volume;overlay_->lastStoredTickMs=trade.atMs;
    }
    appliedId_=trade.id;return true;
}
void CandleTradeOverlay::rebuild(){
    overlay_.reset();appliedId_=anchorId_;
    if(!anchor_||anchorId_<minimumAnchorId_)return;
    overlay_=anchor_;
    for(const auto& trade:buffered_){
        if(trade.id<=anchorId_)continue;
        if(!appendToOverlay(trade)){overlay_.reset();return;}
    }
}
void CandleTradeOverlay::setBinanceAnchor(const MarketCandle& row,qint64 lastTradeId,qint64 source){
    if(provider_!="binance"||lastTradeId<0||source<row.startsAtMs||!validAnchor(row,intervalMs_)){
        clearBinanceAnchor();return;
    }
    if(source<anchorAtMs_||lastTradeId<anchorId_)return;
    anchor_=row;anchorId_=lastTradeId;anchorAtMs_=source;
    while(!buffered_.isEmpty()&&buffered_.first().id<=lastTradeId)buffered_.removeFirst();
    if(latest_&&(latest_->id<lastTradeId||(latest_->id==lastTradeId&&latest_->price!=row.close)))latest_.reset();
    rebuild();
}
void CandleTradeOverlay::clearBinanceAnchor(){anchor_.reset();overlay_.reset();anchorId_=appliedId_=-1;anchorAtMs_=0;}
MarketCandleLiveTrade CandleTradeOverlay::observation(qint64 at)const{
    MarketCandleLiveTrade result;
    if(!latest_||at<latest_->receivedAtMs||at<latest_->eventMs||at-latest_->atMs>MarketCandles::FreshnessMs)return result;
    result.available=true;result.identity=identity_;result.intervalMs=intervalMs_;result.price=latest_->price;
    result.tradeAtMs=latest_->atMs;result.receivedAtMs=latest_->receivedAtMs;return result;
}
std::optional<MarketCandle> CandleTradeOverlay::forming(qint64 at)const{
    if(!overlay_||!observation(at).available||overlay_->startsAtMs>at||
        at-overlay_->startsAtMs>=intervalMs_||anchorAtMs_>at)return {};
    return overlay_;
}
} // namespace keefi
