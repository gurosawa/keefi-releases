#pragma once
#include "keefi/MarketMetrics.h"
#include "keefi/MarketCandles.h"
#include "keefi/UsabilityPresentation.h"
#include <QMetaType>

namespace keefi {
enum class MarketFlowMetric { KoreaGap, Funding, CoinbaseGap, OpenInterest };
enum class MarketFlowState { Unavailable, Cool, Normal, Hot };
struct MarketFlowPolicy {
    static constexpr qint64 HourMs = 3600000;
    static constexpr qint64 DayMs = 24 * HourMs;
    static constexpr qint64 HistoryFreshnessMs = HourMs;
    static constexpr int AnnualPercentFactor = 365 * 24 * 100;
};
struct MarketFlowGapPoint {
    qint64 closeMs = 0;
    QString percent;
    bool operator==(const MarketFlowGapPoint&) const = default;
};
struct MarketFlowHistoricalGap {
    bool available = false;
    QString percent;
    qint64 comparisonCloseMs = 0, receivedAtMs = 0;
    QString asset;
    // Hourly gaps from the comparison window through the latest completed hour.
    // Only windows where all three completed closes validate are included.
    QList<MarketFlowGapPoint> series;
};
struct MarketFlowDayComparison {
    bool available = false;
    QString previousPercent, differencePercentagePoints;
    qint64 comparisonCloseMs = 0;
};
struct MarketFlowThresholds {
    QString cool, hot;
    bool coolInclusive = true;
};
// Display policy only. This module owns no transport, storage or portfolio input.
inline MarketFlowThresholds marketFlowThresholds(MarketFlowMetric metric) {
    // Temporary state boundaries. Change this table, never presentation copy, to tune policy.
    switch(metric) {
    case MarketFlowMetric::KoreaGap:return {"-0.5", "3", true};
    case MarketFlowMetric::Funding:return {"0", "30", false};
    case MarketFlowMetric::CoinbaseGap:return {"-0.1", "0.1", true};
    case MarketFlowMetric::OpenInterest:return {};
    }
    return {};
}
namespace market_flow_detail {
struct SignedValue { bool valid=false,negative=false;QString magnitude; };
inline SignedValue signedValue(QString value) {
    const bool negative=value.startsWith('-');if(negative)value.remove(0,1);
    for(const auto c:value)if((c<'0'||c>'9')&&c!='.')return {};
    const auto magnitude=decimal::normalize(value);
    return magnitude.isEmpty()?SignedValue{}:SignedValue{true,negative&&magnitude!="0",magnitude};
}
inline int compare(const SignedValue& a,const SignedValue& b) {
    if(a.negative!=b.negative)return a.negative?-1:1;
    const auto magnitudeCompare=[](const QString& left,const QString& right){
        const auto lp=left.indexOf('.'),rp=right.indexOf('.');
        auto lw=lp<0?left:left.left(lp),rw=rp<0?right:right.left(rp);
        while(lw.size()>1&&lw.startsWith('0'))lw.remove(0,1);while(rw.size()>1&&rw.startsWith('0'))rw.remove(0,1);
        if(lw.size()!=rw.size())return lw.size()>rw.size()?1:-1;
        const auto whole=QString::compare(lw,rw);if(whole)return whole;
        const auto lf=lp<0?QString{}:left.mid(lp+1),rf=rp<0?QString{}:right.mid(rp+1);const auto width=qMax(lf.size(),rf.size());
        return QString::compare(lf.leftJustified(width,'0'),rf.leftJustified(width,'0'));
    };
    const auto difference=magnitudeCompare(a.magnitude,b.magnitude);
    return a.negative?-difference:difference;
}
inline MarketFlowState classify(int coolComparison,int hotComparison,bool coolInclusive) {
    if(hotComparison>=0)return MarketFlowState::Hot;
    if(coolComparison<0||(coolInclusive&&coolComparison==0))return MarketFlowState::Cool;
    return MarketFlowState::Normal;
}
}
inline MarketFlowState marketFlowState(MarketFlowMetric metric, const QString& percent) {
    const auto value=market_flow_detail::signedValue(percent);const auto thresholds=marketFlowThresholds(metric);
    const auto cool=market_flow_detail::signedValue(thresholds.cool),hot=market_flow_detail::signedValue(thresholds.hot);
    if(!value.valid||!cool.valid||!hot.valid)return MarketFlowState::Unavailable;
    return market_flow_detail::classify(market_flow_detail::compare(value,cool),market_flow_detail::compare(value,hot),thresholds.coolInclusive);
}
inline QString marketFlowAnnualFundingPercent(const QString& rate, int intervalHours) {
    const auto value=market_flow_detail::signedValue(rate);
    if(!value.valid||intervalHours<1||intervalHours>24)return {};
    auto numerator=decimal::multiply(value.magnitude,QString::number(MarketFlowPolicy::AnnualPercentFactor));
    const auto point=numerator.indexOf('.');const auto scale=point<0?0:numerator.size()-point-1;numerator.remove('.');
    // Integer units of 0.000001 percent. Any truncation belongs to this final display only.
    const auto shift=6-scale;
    if(shift>=0)numerator+=QString(shift,'0');else numerator=numerator.left(qMax(qsizetype(0),numerator.size()+shift));
    if(numerator.isEmpty())numerator="0";
    QString units;int remainder=0;
    for(const auto digit:numerator){const auto amount=remainder*10+digit.digitValue();units+=QChar('0'+amount/intervalHours);remainder=amount%intervalHours;}
    while(units.size()>1&&units.startsWith('0'))units.remove(0,1);
    const bool nonzero=units!="0";units=units.rightJustified(7,'0');units.insert(units.size()-6,'.');
    while(units.endsWith('0'))units.chop(1);if(units.endsWith('.'))units.chop(1);
    return (value.negative&&nonzero?"-":"")+units;
}
inline MarketFlowState marketFlowFundingState(const PublicDerivativesMetrics& data, qint64 asOfMs) {
    if(asOfMs<=0||data.baseAsset.isEmpty()||data.symbol!=data.baseAsset+"USDT"||data.fundingIntervalHours<1||data.fundingIntervalHours>24||
        data.nextFundingTimeMs<=asOfMs||data.nextFundingTimeMs-asOfMs>MarketFlowPolicy::DayMs)return MarketFlowState::Unavailable;
    const auto scope="public:binance:usdm:"+data.symbol;
    const auto receivedFresh=[&](const AssetEvidence& e){return e.complete&&!e.source.isEmpty()&&!e.url.isEmpty()&&e.scope==scope&&
        e.receivedAtMs>0&&e.receivedAtMs<=asOfMs&&asOfMs-e.receivedAtMs<=MarketMetrics::FreshnessMs;};
    const auto& funding=data.fundingEvidence;const auto& interval=data.intervalEvidence;
    if(!receivedFresh(funding)||!receivedFresh(interval)||funding.sourceTimeMs<=0||funding.sourceTimeMs>funding.receivedAtMs||
        asOfMs-funding.sourceTimeMs>MarketMetrics::FreshnessMs||interval.sourceTimeMs<0||interval.sourceTimeMs>interval.receivedAtMs)return MarketFlowState::Unavailable;
    auto value=market_flow_detail::signedValue(data.fundingRate);const auto thresholds=marketFlowThresholds(MarketFlowMetric::Funding);
    auto cool=market_flow_detail::signedValue(thresholds.cool),hot=market_flow_detail::signedValue(thresholds.hot);
    if(!value.valid||!cool.valid||!hot.valid)return MarketFlowState::Unavailable;
    // Compare the exact annual ratio against both boundaries before any display truncation.
    value.magnitude=decimal::multiply(value.magnitude,QString::number(MarketFlowPolicy::AnnualPercentFactor));
    const auto hours=QString::number(data.fundingIntervalHours);
    cool.magnitude=decimal::multiply(cool.magnitude,hours);hot.magnitude=decimal::multiply(hot.magnitude,hours);
    return market_flow_detail::classify(market_flow_detail::compare(value,cool),market_flow_detail::compare(value,hot),thresholds.coolInclusive);
}
inline QString marketFlowContractLabel(const ContractFlow& flow) {
    if(!flow.available||flow.priceDirection < -1||flow.priceDirection > 1||flow.quantityDirection < -1||flow.quantityDirection > 1)return {};
    const auto direction=[](int sign){return usabilityText(sign>0?UsabilityText::FlowDirectionUp:sign<0?UsabilityText::FlowDirectionDown:UsabilityText::FlowDirectionFlat);};
    return usabilityText(UsabilityText::FlowContractLabel).arg(direction(flow.priceDirection),direction(flow.quantityDirection));
}
namespace market_flow_detail {
// One completed native hourly close inside [start, start + 1h), validated against
// its own request, freshness and (Upbit) stored tick. No interpolation.
inline const MarketCandle* completedClose(const MarketCandleSeries& series, const QString& asset, const QString& currency, const QString& identity, qint64 start, qint64 asOfMs) {
    const auto close=start+MarketFlowPolicy::HourMs;
    const auto request=MarketCandles::request(identity,MarketFlowPolicy::HourMs);
    if(!series.available||series.asset!=asset||series.currency!=currency||series.identity!=identity||series.intervalMs!=MarketFlowPolicy::HourMs||
        request.url.isEmpty()||QUrl(series.sourceUrl)!=request.url||series.receivedAtMs<=0||series.receivedAtMs>asOfMs||
        asOfMs-series.receivedAtMs>MarketFlowPolicy::HistoryFreshnessMs||series.sourceTimeMs<0||series.sourceTimeMs>series.receivedAtMs||close>series.receivedAtMs)return nullptr;
    const MarketCandle* found=nullptr;
    for(const auto& candle:series.candles)if(candle.startsAtMs==start){if(found)return nullptr;found=&candle;}
    if(!found||!decimal::positive(found->close))return nullptr;
    // The official completed window, not a fabricated quote clock, aligns all three closes.
    // Binance REST has no last-trade timestamp. Upbit keeps its actual observed tick.
    const auto tick=found->lastStoredTickMs;
    if((tick&&(tick<start||tick>=close))||(identity.startsWith("coin:upbit:")&&(tick<=0||series.sourceTimeMs<tick)))return nullptr;
    return found;
}
inline QString completedGap(const MarketCandleSeries& local, const MarketCandleSeries& remote, const MarketCandleSeries& bridge, qint64 start, qint64 asOfMs) {
    const auto a=completedClose(local,local.asset,"KRW","coin:upbit:KRW-"+local.asset,start,asOfMs);
    const auto b=completedClose(remote,local.asset,"USDT","coin:binance:"+local.asset+"USDT",start,asOfMs);
    const auto c=completedClose(bridge,"USDT","KRW","coin:upbit:KRW-USDT",start,asOfMs);
    return a&&b&&c?MarketMetrics::priceGapPercent(a->close,b->close,c->close):QString{};
}
}
inline MarketFlowHistoricalGap marketFlowHistoricalGap(const MarketCandleSeries& local, const MarketCandleSeries& remote, const MarketCandleSeries& bridge, qint64 asOfMs) {
    if(asOfMs<=MarketFlowPolicy::DayMs+MarketFlowPolicy::HourMs||local.asset.isEmpty())return {};
    const auto close=asOfMs/MarketFlowPolicy::HourMs*MarketFlowPolicy::HourMs-MarketFlowPolicy::DayMs;
    const auto percent=market_flow_detail::completedGap(local,remote,bridge,close-MarketFlowPolicy::HourMs,asOfMs);if(percent.isEmpty())return {};
    MarketFlowHistoricalGap result{true,percent,close,qMin(local.receivedAtMs,qMin(remote.receivedAtMs,bridge.receivedAtMs)),local.asset,{}};
    const auto lastStart=asOfMs/MarketFlowPolicy::HourMs*MarketFlowPolicy::HourMs-MarketFlowPolicy::HourMs;
    for(auto start=close-MarketFlowPolicy::HourMs;start<=lastStart;start+=MarketFlowPolicy::HourMs){
        const auto value=market_flow_detail::completedGap(local,remote,bridge,start,asOfMs);
        if(!value.isEmpty())result.series.append({start+MarketFlowPolicy::HourMs,value});
    }
    return result;
}
inline MarketFlowDayComparison marketFlowDayComparison(const ExchangePriceGap& current, const MarketFlowHistoricalGap& previous, qint64 asOfMs) {
    if(asOfMs<=MarketFlowPolicy::DayMs+MarketFlowPolicy::HourMs||!current.available||!previous.available||current.legs.size()!=3||
        current.asOfMs<=0||current.asOfMs>asOfMs||asOfMs-current.asOfMs>MarketMetrics::FreshnessMs||previous.asset.isEmpty()||
        current.legs[0].baseAsset!=previous.asset||current.legs[0].quoteAsset!="KRW"||previous.receivedAtMs<=0||previous.receivedAtMs>asOfMs||
        asOfMs-previous.receivedAtMs>MarketFlowPolicy::HistoryFreshnessMs||
        previous.comparisonCloseMs!=asOfMs/MarketFlowPolicy::HourMs*MarketFlowPolicy::HourMs-MarketFlowPolicy::DayMs)return {};
    const auto validated=MarketMetrics::exchangePriceGap(current.legs[0],current.legs[1],current.legs[2],asOfMs);
    const auto a=market_flow_detail::signedValue(current.percent),b=market_flow_detail::signedValue(previous.percent),fresh=market_flow_detail::signedValue(validated.percent);
    if(!validated.available||!a.valid||!b.valid||!fresh.valid||market_flow_detail::compare(a,fresh)!=0)return {};
    QString difference;bool negative=false;
    if(a.negative!=b.negative){difference=decimal::add(a.magnitude,b.magnitude);negative=a.negative;}
    else {
        const auto order=decimal::compare(a.magnitude,b.magnitude);
        difference=order<0?decimal::subtract(b.magnitude,a.magnitude):decimal::subtract(a.magnitude,b.magnitude);
        negative=a.negative?order>0:order<0;
    }
    difference=decimal::normalize(difference);if(difference.isEmpty())return {};
    if(negative&&difference!="0")difference.prepend('-');
    return {true,(b.negative?"-":"")+b.magnitude,difference,previous.comparisonCloseMs};
}
// Display-only rounding (half away from zero). Policy comparisons keep the exact value.
inline QString marketFlowRounded(const QString& exact, int decimals) {
    auto value=market_flow_detail::signedValue(exact);if(!value.valid||decimals<0||decimals>8)return {};
    const auto half=decimals?"0."+QString(decimals,'0')+"5":QStringLiteral("0.5");
    auto magnitude=decimal::add(value.magnitude,half);if(magnitude.isEmpty())return {};
    const auto point=magnitude.indexOf('.');auto whole=point<0?magnitude:magnitude.left(point);auto fraction=point<0?QString{}:magnitude.mid(point+1);
    fraction=fraction.left(decimals).leftJustified(decimals,'0');
    const auto text=decimals?whole+"."+fraction:whole;
    bool zero=true;for(const auto c:text)if(c!='0'&&c!='.'){zero=false;break;}
    return (value.negative&&!zero?"-":"")+text;
}
inline QString marketFlowMetricLabel(MarketFlowMetric metric) {
    switch(metric) {
    case MarketFlowMetric::KoreaGap:return usabilityText(UsabilityText::FlowMetricKoreaGap);
    case MarketFlowMetric::Funding:return usabilityText(UsabilityText::FlowMetricFunding);
    case MarketFlowMetric::CoinbaseGap:return usabilityText(UsabilityText::FlowMetricCoinbaseGap);
    case MarketFlowMetric::OpenInterest:return usabilityText(UsabilityText::FlowOiTitle);
    }
    return {};
}
inline QString marketFlowStateLabel(MarketFlowState state) {
    switch(state) {
    case MarketFlowState::Unavailable:return usabilityText(UsabilityText::FlowChecking);
    case MarketFlowState::Cool:return usabilityText(UsabilityText::FlowStateCool);
    case MarketFlowState::Normal:return usabilityText(UsabilityText::FlowStateNormal);
    case MarketFlowState::Hot:return usabilityText(UsabilityText::FlowStateHot);
    }
    return {};
}
}
Q_DECLARE_METATYPE(keefi::MarketFlowMetric)
Q_DECLARE_METATYPE(keefi::MarketFlowState)
