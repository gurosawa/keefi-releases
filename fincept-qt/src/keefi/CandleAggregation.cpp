#include "keefi/CandleAggregation.h"
#include <QRegularExpression>
#include <limits>
namespace keefi {
CandleAggregation aggregateCompletedCandles(const QList<MarketCandle>& rows,
    qint64 source, qint64 target, qint64 asOf) {
    CandleAggregation out{false,QStringLiteral("완전한 완료 봉 그룹 없음"),{}};
    const QList<qint64> supported{60000,300000,900000,3600000,14400000,86400000};
    if(!supported.contains(source)||!supported.contains(target)||target<source||target%source||asOf<=0||rows.size()>10000)return out;
    QList<MarketCandle> group;
    qint64 previous=0,bucket=-1;
    const auto invalid=[&](){return CandleAggregation{false,QStringLiteral("캔들 그룹 입력 검증 실패"),{}};};
    const auto flush=[&](){
        if(group.size()!=target/source||group.first().startsAtMs!=bucket||group.last().startsAtMs+source!=bucket+target||bucket+target>asOf)return true;
        MarketCandle result=group.first();result.volume="0";result.lastStoredTickMs=0;
        for(const auto& c:group){
            if(decimal::compare(c.high,result.high)>0)result.high=c.high;
            if(decimal::compare(c.low,result.low)<0)result.low=c.low;
            result.volume=decimal::add(result.volume,c.volume);if(!decimal::valid(result.volume))return false;
            result.lastStoredTickMs=qMax(result.lastStoredTickMs,c.lastStoredTickMs);
        }
        result.close=group.last().close;out.candles.append(result);return true;
    };
    for(const auto& raw:rows){
        static const QRegularExpression number(QStringLiteral("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z"));
        for(const auto& value:{raw.open,raw.high,raw.low,raw.close,raw.volume})
            if(value.size()>161||!number.match(value).hasMatch())return invalid();
        MarketCandle c=raw;
        c.open=decimal::normalize(c.open);c.high=decimal::normalize(c.high);c.low=decimal::normalize(c.low);c.close=decimal::normalize(c.close);c.volume=decimal::normalize(c.volume);
        if(c.startsAtMs<=0||c.startsAtMs%source||c.startsAtMs<=previous||c.startsAtMs>asOf||c.startsAtMs>std::numeric_limits<qint64>::max()-target||
           c.lastStoredTickMs<0||c.lastStoredTickMs>asOf||(c.lastStoredTickMs&&c.lastStoredTickMs<c.startsAtMs)||
           !decimal::positive(c.open)||!decimal::positive(c.high)||!decimal::positive(c.low)||!decimal::positive(c.close)||c.volume.isEmpty()||
           decimal::compare(c.high,c.open)<0||decimal::compare(c.high,c.close)<0||decimal::compare(c.low,c.open)>0||decimal::compare(c.low,c.close)>0)return invalid();
        previous=c.startsAtMs;
        if(c.startsAtMs+source>asOf)continue;
        const qint64 nextBucket=c.startsAtMs/target*target;
        if(nextBucket!=bucket){if(!group.isEmpty()&&!flush())return invalid();group.clear();bucket=nextBucket;}
        group.append(c);
    }
    if(!group.isEmpty()&&!flush())return invalid();
    out.available=!out.candles.isEmpty();if(out.available)out.reason.clear();return out;
}
}
