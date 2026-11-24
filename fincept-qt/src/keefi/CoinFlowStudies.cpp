#include "keefi/CoinFlowStudies.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
constexpr qint64 Minute=60000, Horizon=60*Minute;
constexpr int MaxTrades=10000, MaxOi=500, MaxLiquidations=1000, MaxMinutes=200;

// Exact public display arithmetic is local to C6. No valuation decimal helper
// or evaluator participates. Discrete thresholds never depend on binary64.
struct Number { QString digits="0"; int scale=0; bool negative=false; };
Number compact(QString digits,int scale,bool negative=false) {
    int first=0; while(first+1<digits.size()&&digits[first]=='0') ++first;
    if(first) digits.remove(0,first);
    if(digits=="0") return {};
    while(scale>0&&digits.endsWith('0')) { digits.chop(1); --scale; }
    return {digits,scale,negative};
}
Number exact(const QString& text) {
    QString digits=text; const int point=int(digits.indexOf('.'));
    const int scale=point<0?0:int(digits.size())-point-1;
    if(point>=0) digits.remove(point,1);
    return compact(digits,scale);
}
QString aligned(const Number& value,int scale) {
    return value.digits=="0"?QString("0"):value.digits+QString(scale-value.scale,'0');
}
int unsignedCompare(const QString& left,const QString& right) {
    if(left.size()!=right.size()) return left.size()<right.size()?-1:1;
    const int value=QString::compare(left,right); return value<0?-1:value>0?1:0;
}
int magnitude(const Number& left,const Number& right) {
    // Same result as unsignedCompare(aligned(left,scale),aligned(right,scale)) without building
    // the two padded strings: the padding is read as virtual trailing zeros.
    const int scale=std::max(left.scale,right.scale);
    const bool leftZero=left.digits==QLatin1String("0"),rightZero=right.digits==QLatin1String("0");
    const qsizetype leftSize=leftZero?1:left.digits.size()+scale-left.scale,rightSize=rightZero?1:right.digits.size()+scale-right.scale;
    if(leftSize!=rightSize) return leftSize<rightSize?-1:1;
    const auto digit=[](const Number& value,bool zero,qsizetype index){return zero||index>=value.digits.size()?QChar('0'):value.digits[index];};
    for(qsizetype index=0;index<leftSize;++index) {
        const QChar a=digit(left,leftZero,index),b=digit(right,rightZero,index);
        if(a!=b) return a<b?-1:1;
    }
    return 0;
}
int compare(const Number& left,const Number& right) {
    if(left.negative!=right.negative) return left.negative?-1:1;
    const int value=magnitude(left,right); return left.negative?-value:value;
}
Number addMagnitude(const Number& a,const Number& b) {
    const int scale=std::max(a.scale,b.scale);
    QString left=aligned(a,scale),right=aligned(b,scale);
    const int size=int(std::max(left.size(),right.size()));
    left=left.rightJustified(size,'0'); right=right.rightJustified(size,'0');
    QString digits(size,'0'); int carry=0;
    for(int i=size-1;i>=0;--i) {
        const int sum=left[i].digitValue()+right[i].digitValue()+carry;
        digits[i]=QChar('0'+sum%10); carry=sum/10;
    }
    if(carry) digits.prepend('1');
    return compact(digits,scale);
}
Number subtractMagnitude(const Number& a,const Number& b) { // a >= b
    const int scale=std::max(a.scale,b.scale);
    QString left=aligned(a,scale),right=aligned(b,scale);
    right=right.rightJustified(left.size(),'0'); int borrow=0;
    for(int i=int(left.size())-1;i>=0;--i) {
        int digit=left[i].digitValue()-right[i].digitValue()-borrow;
        borrow=digit<0; if(borrow) digit+=10; left[i]=QChar('0'+digit);
    }
    return compact(left,scale);
}
Number add(const Number& a,const Number& b) {
    if(a.negative==b.negative) {
        auto result=addMagnitude(a,b); result.negative=a.negative&&result.digits!="0"; return result;
    }
    const int order=magnitude(a,b);
    auto result=order>=0?subtractMagnitude(a,b):subtractMagnitude(b,a);
    result.negative=result.digits!="0"&&(order>=0?a.negative:b.negative); return result;
}
Number subtract(const Number& a,Number b) {
    if(b.digits!="0") b.negative=!b.negative;
    return add(a,b);
}
Number multiply(const Number& a,const Number& b) {
    if(a.digits=="0"||b.digits=="0") return {};
    constexpr quint64 Base=1000000000;
    const auto limbs=[](const QString& digits) {
        QList<quint64> values;
        for(int end=int(digits.size());end>0;end-=9)
            values.append(digits.mid(std::max(0,end-9),std::min(9,end)).toULongLong());
        return values;
    };
    const auto left=limbs(a.digits),right=limbs(b.digits);
    QList<quint64> values(left.size()+right.size(),0);
    for(int i=0;i<left.size();++i) {
        quint64 carry=0;
        for(int j=0;j<right.size();++j) {
            const quint64 value=values[i+j]+left[i]*right[j]+carry;
            values[i+j]=value%Base; carry=value/Base;
        }
        values[i+right.size()]+=carry;
    }
    while(values.size()>1&&values.last()==0) values.removeLast();
    QString digits=QString::number(values.last());
    for(int i=int(values.size())-2;i>=0;--i) digits+=QString::number(values[i]).rightJustified(9,'0');
    return compact(digits,a.scale+b.scale,a.negative!=b.negative);
}
Number times(const Number& value,int factor) { return multiply(value,exact(QString::number(factor))); }
QString textNumber(const Number& value) {
    QString digits=value.digits;
    if(value.scale>0) {
        digits=digits.rightJustified(value.scale+1,'0'); digits.insert(digits.size()-value.scale,'.');
    }
    return value.negative?'-'+digits:digits;
}
// Plain decimal grammar (?:0|[1-9][0-9]*)(?:\.[0-9]+)? checked by hand: it runs for every
// held trade on every calculation, where a regular expression match dominated the cost.
bool plainDecimal(const QString& text,bool fraction) {
    const auto digit=[](QChar c){return c>=QLatin1Char('0')&&c<=QLatin1Char('9');};
    qsizetype i=0; const qsizetype size=text.size();
    if(size==0||!digit(text[0])) return false;
    if(text[0]==QLatin1Char('0')) i=1; else while(i<size&&digit(text[i])) ++i;
    if(i==size) return true;
    if(!fraction||text[i]!=QLatin1Char('.')||++i==size) return false;
    while(i<size&&digit(text[i])) ++i;
    return i==size;
}
bool number(const QString& text,bool positive,Number* output=nullptr) {
    if(text.isEmpty()||text.size()>512||!plainDecimal(text,true)) return false;
    const auto value=exact(text);
    static const Number maximum{QString("1")+QString(100,'0'),0,false};
    if(magnitude(value,maximum)>0||(positive&&value.digits=="0")) return false;
    if(output) *output=value; return true;
}
std::optional<double> ratio(const Number& numerator,const Number& denominator) {
    if(denominator.digits=="0") return {};
    if(numerator.digits=="0") return 0.0;
    const auto mantissa=[](const Number& value) {
        const auto prefix=value.digits.left(18);
        return static_cast<long double>(prefix.toULongLong())/std::pow(10.0L,int(prefix.size())-1);
    };
    const int exponent=int(numerator.digits.size())-numerator.scale-int(denominator.digits.size())+denominator.scale;
    long double value=mantissa(numerator)/mantissa(denominator)*std::pow(10.0L,exponent);
    if(numerator.negative!=denominator.negative) value=-value;
    const double result=static_cast<double>(value);
    if(!std::isfinite(result)||result==0) return {};
    return result;
}
bool validId(const QString& value) {
    return value.size()<=128&&plainDecimal(value,false);
}
bool catalogUrl(const QString& text,CoinFlowVenue venue) {
    const QUrl url(text);
    if(!url.isValid()||url.scheme()!="https"||url.port(443)!=443||!url.userInfo().isEmpty()||url.hasFragment()) return false;
    const QString host=venue==CoinFlowVenue::UpbitSpot?"api.upbit.com":venue==CoinFlowVenue::BinanceSpot?"api.binance.com":"fapi.binance.com";
    const QString path=venue==CoinFlowVenue::UpbitSpot?"/v1/market/all":venue==CoinFlowVenue::BinanceSpot?"/api/v3/exchangeInfo":"/fapi/v1/exchangeInfo";
    if(url.host()!=host||url.path()!=path) return false;
    const auto query=QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if(query.isEmpty()) return !url.hasQuery();
    if(venue==CoinFlowVenue::UpbitSpot) return query==QList<QPair<QString,QString>>{{"is_details","true"}};
    if(venue==CoinFlowVenue::BinanceSpot)
        return query==QList<QPair<QString,QString>>{{"permissions","SPOT"},{"showPermissionSets","false"}};
    return false;
}
bool publicIdentity(const QString& identity) {
    const auto parts=identity.split(':');
    if(parts.size()!=3||parts[0]!="coin") return false;
    if(parts[1]=="binance") return binanceIdentifierValid(parts[2],2);
    static const QRegularExpression pair("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,40}\\z");
    return parts[1]=="upbit"&&pair.match(parts[2]).hasMatch();
}
bool instrumentValid(const CoinFlowInstrument& value,qint64 asOf) {
    if(!value.verified||value.symbol.isEmpty()||value.symbol.size()>128||value.baseAsset.isEmpty()
        ||value.quoteAsset.isEmpty()||value.baseAsset.size()>80||value.quoteAsset.size()>80) return false;
    const auto& proof=value.evidence;
    // Structural current proof, deliberately no asOf-age gate here: a newer
    // catalogue receipt must not erase already constructed historical entries.
    if(proof.source.isEmpty()||proof.source.size()>256||!catalogUrl(proof.sourceUrl,value.venue)
        ||proof.receivedAtMs<=0||proof.receivedAtMs>asOf||proof.knownAtMs<proof.receivedAtMs||proof.knownAtMs>asOf
        ||proof.sourceTimeMs<0||proof.sourceTimeMs>proof.receivedAtMs) return false;
    if(value.venue==CoinFlowVenue::UpbitSpot)
        return value.grain==CoinFlowGrain::UpbitTrade&&value.identity=="coin:upbit:"+value.symbol
            &&publicIdentity(value.identity)&&value.symbol==value.quoteAsset+'-'+value.baseAsset;
    if(value.status!="TRADING"||!binanceIdentifierValid(value.baseAsset)||!binanceIdentifierValid(value.quoteAsset)) return false;
    if(value.venue==CoinFlowVenue::BinanceSpot)
        return value.grain==CoinFlowGrain::SpotAggregate&&value.identity=="coin:binance:"+value.symbol
            &&binanceIdentifierValid(value.symbol,2)&&value.symbol==value.baseAsset+value.quoteAsset;
    return value.venue==CoinFlowVenue::BinanceUsdM&&value.grain==CoinFlowGrain::Futures100msAggregate
        &&value.identity=="usdm_futures:binance:"+value.symbol&&binanceUsdmSymbolValid(value.symbol)&&value.contractType=="PERPETUAL"
        &&value.quoteAsset=="USDT"&&value.marginAsset=="USDT"&&value.underlyingType=="COIN";
}
bool goodCoverage(CoinFlowCoverage value) {
    return value==CoinFlowCoverage::ObservedContinuous||value==CoinFlowCoverage::RecoveredOverlap;
}
CoinFlowAvailability unavailable(QString reason,CoinFlowCoverage coverage=CoinFlowCoverage::Unavailable,int samples=0) {
    CoinFlowAvailability value; value.reason=std::move(reason); value.coverage=coverage; value.samples=samples; return value;
}
struct Coverage {
    bool good=false; quint64 epoch=0; qint64 knownAtMs=0;
    CoinFlowCoverage coverage=CoinFlowCoverage::Unavailable;
};
Coverage coverageAt(const QList<CoinFlowCoveragePeriod>& periods,qint64 from,qint64 through,
    qint64 decision,quint64 epoch=0) {
    Coverage result;
    if(from<0||through<=from||periods.size()>2*MaxTrades) return result;
    // Window preparation sorts once. Filter knowledge in place for each past
    // decision; no per-trade sorting or retroactive coverage promotion.
    for(const auto& period:periods) {
        if(period.knownAtMs<=0||period.knownAtMs>decision||period.fromMs<0||period.throughMs<=period.fromMs||period.throughMs>period.knownAtMs
            ||period.epoch==0||period.throughMs<=from||period.fromMs>=through||(epoch&&period.epoch!=epoch)) continue;
        if(!goodCoverage(period.coverage)) { result.coverage=period.coverage; return result; }
    }
    qint64 cursor=from;
    for(const auto& value:periods) {
        const auto* period=&value;
        if(period->knownAtMs<=0||period->knownAtMs>decision||period->fromMs<0||period->throughMs<=period->fromMs||period->throughMs>period->knownAtMs
            ||period->epoch==0||period->throughMs<=from||period->fromMs>=through||(epoch&&period->epoch!=epoch)) continue;
        if(!goodCoverage(period->coverage)) continue;
        if(period->fromMs>cursor) break;
        if(period->throughMs<=cursor) continue;
        if(result.epoch&&result.epoch!=period->epoch) return result;
        result.epoch=period->epoch; cursor=std::max(cursor,period->throughMs);
        result.knownAtMs=std::max(result.knownAtMs,period->knownAtMs);
        if(result.coverage!=CoinFlowCoverage::RecoveredOverlap) result.coverage=period->coverage;
        if(cursor>=through) { result.good=true; return result; }
    }
    return result;
}
CoinFlowAvailability state(const Coverage& coverage,qint64 from,qint64 through,int samples,
    const CoinFlowEvidence& evidence,qint64 known=0) {
    CoinFlowAvailability result; result.available=coverage.good; result.coverage=coverage.coverage;
    result.fromMs=from; result.throughMs=through; result.samples=samples;
    result.knownAtMs=std::max(known,coverage.knownAtMs); result.evidence.append(evidence);
    if(!result.available) result.reason="해당 기간의 연속 관측 근거가 없습니다";
    return result;
}
struct Trade {
    const CoinTradeObservation* source=nullptr;
    Number price,quantity,notional;
    int notionalRank=-1; // Position of notional among valid rows; equal notionals share a rank.
    bool valid=false;
};
struct Window {
    const CoinFlowWindow* source=nullptr;
    QList<Trade> trades;
    QList<CoinFlowCoveragePeriod> periods;
    bool valid=false,overflow=false;
};
bool sameTrade(const CoinTradeObservation& a,const CoinTradeObservation& b) {
    return a.price==b.price&&a.quantity==b.quantity&&a.normalQuantity==b.normalQuantity
        &&a.side==b.side&&a.tradeAtMs==b.tradeAtMs&&a.eventAtMs==b.eventAtMs&&a.snapshot==b.snapshot;
}
Window prepare(const CoinFlowWindow& input,qint64 asOf) {
    Window result; result.source=&input;
    result.overflow=input.trades.size()>MaxTrades||input.periods.size()>2*MaxTrades;
    result.valid=instrumentValid(input.instrument,asOf)&&!result.overflow;
    if(!result.valid) return result;
    result.periods=input.periods;
    std::stable_sort(result.periods.begin(),result.periods.end(),[](const auto& a,const auto& b) {
        return a.fromMs!=b.fromMs?a.fromMs<b.fromMs:a.knownAtMs<b.knownAtMs;
    });
    for(const auto& source:input.trades) {
        Trade row; row.source=&source;
        row.valid=validId(source.id)&&source.epoch>0&&source.tradeAtMs>=0&&source.tradeAtMs<std::numeric_limits<qint64>::max()
            &&source.receivedAtMs>0&&source.knownAtMs>=source.receivedAtMs&&source.tradeAtMs<=source.knownAtMs
            &&source.eventAtMs>=0&&source.eventAtMs<=source.knownAtMs
            &&number(source.price,true,&row.price)&&number(source.quantity,true,&row.quantity)
            &&(source.side==CoinFlowSide::Buy||source.side==CoinFlowSide::Sell);
        if(source.normalQuantity) {
            Number normal;
            row.valid=row.valid&&input.instrument.venue==CoinFlowVenue::BinanceUsdM
                &&number(*source.normalQuantity,false,&normal)&&magnitude(normal,row.quantity)<=0;
        }
        if(row.valid) row.notional=multiply(row.price,row.quantity);
        result.trades.append(row);
    }
    std::stable_sort(result.trades.begin(),result.trades.end(),[](const Trade& a,const Trade& b) {
        const auto& left=*a.source; const auto& right=*b.source;
        if(left.tradeAtMs!=right.tradeAtMs) return left.tradeAtMs<right.tradeAtMs;
        if(left.knownAtMs!=right.knownAtMs) return left.knownAtMs<right.knownAtMs;
        return unsignedCompare(left.id,right.id)<0;
    });
    // Rank exact notionals once so percentile windows compare integers instead of re-comparing decimals.
    QList<Trade*> ranked;ranked.reserve(result.trades.size());
    for(auto& row:result.trades) if(row.valid) ranked.append(&row);
    std::sort(ranked.begin(),ranked.end(),[](const Trade* a,const Trade* b){return compare(a->notional,b->notional)<0;});
    for(int i=0,rank=-1;i<ranked.size();++i){
        if(i==0||compare(ranked[i-1]->notional,ranked[i]->notional)!=0) ++rank;
        ranked[i]->notionalRank=rank;
    }
    return result;
}
struct KnownTrades { bool valid=false; QList<const Trade*> rows; QString reason; };
KnownTrades knownTrades(const Window& window,qint64 decision,qint64 through) {
    KnownTrades result;
    if(!window.valid) { result.reason=window.overflow?"체결 보관 한도를 넘었습니다":"공개 종목 원천 검증이 없습니다"; return result; }
    QHash<QString,const CoinTradeObservation*> ids;
    for(const auto& row:window.trades) {
        const auto& source=*row.source;
        if(source.knownAtMs>decision||source.tradeAtMs>=through||source.snapshot) continue;
        if(!row.valid) { result.reason="체결 원문·시각·수량 검증에 실패했습니다"; return result; }
        const QString key=QString::number(source.epoch)+':'+source.id;
        const auto found=ids.constFind(key);
        if(found!=ids.cend()) {
            if(!sameTrade(**found,source)) { result.reason="같은 체결 ID의 원문이 충돌합니다"; return result; }
            continue;
        }
        ids.insert(key,&source); result.rows.append(&row);
    }
    result.valid=true; return result;
}
CoinFlowEvidence tradeEvidence(const Window& window,const CoinTradeObservation& row) {
    // The window URL names the public stream/population. Its latest receipt is
    // not the receipt of an older constituent observation.
    return {window.source->evidence.source,window.source->evidence.sourceUrl,
        row.eventAtMs,row.receivedAtMs,row.knownAtMs};
}
CoinLargeTradeStudy largeStudy(const Window& window,qint64 decision,qint64 through,qint64 candidateFrom=0) {
    CoinLargeTradeStudy result;
    const auto known=knownTrades(window,decision,through);
    if(!known.valid) { result.state=unavailable(known.reason,window.overflow?CoinFlowCoverage::Overflow:CoinFlowCoverage::Unavailable); return result; }
    result.state=unavailable("이전 집계 체결 200개가 필요합니다",CoinFlowCoverage::WarmingUp,int(known.rows.size()));
    result.baselineSamples=std::min(200,std::max(0,int(known.rows.size())-1));
    for(int index=200;index<known.rows.size();++index) {
        const auto* current=known.rows[index]; const auto& source=*current->source;
        if(source.tradeAtMs<candidateFrom&&index!=known.rows.size()-1) continue;
        const auto* first=known.rows[index-200];
        if(first->source->epoch!=source.epoch) continue;
        const auto coverage=coverageAt(window.periods,first->source->tradeAtMs,source.tradeAtMs+1,decision,source.epoch);
        if(!coverage.good) continue;
        int less=0,equal=0; bool sameEpoch=true;
        for(int before=index-200;before<index;++before) {
            if(known.rows[before]->source->epoch!=source.epoch) { sameEpoch=false; break; }
            const int left=known.rows[before]->notionalRank,right=current->notionalRank;
            const int order=left<right?-1:left>right?1:0;
            less+=order<0; equal+=order==0;
        }
        if(!sameEpoch) continue;
        const double percentile=(less+0.5*equal)/2.0;
        if(index==known.rows.size()-1) {
            result.state=state(coverage,first->source->tradeAtMs,source.tradeAtMs+1,200,tradeEvidence(window,source),source.knownAtMs);
            result.latestPercentile=percentile; result.baselineSamples=200;
        }
        if(source.tradeAtMs>=candidateFrom&&less*2+equal>=396) result.candidates.append({source.id,textNumber(current->notional),source.tradeAtMs,
            std::max(source.knownAtMs,coverage.knownAtMs),percentile,200,source.side});
    }
    return result;
}
struct FlowTotals {
    CoinTakerStudy study;
    Number buy,sell,signedBase,difference,total;
};
FlowTotals totals(const Window& window,qint64 from,qint64 through,qint64 decision,bool requireSamples=true) {
    FlowTotals result;
    result.study.sourceDirectionOnly=window.source->instrument.venue==CoinFlowVenue::UpbitSpot;
    const auto known=knownTrades(window,decision,through);
    if(!known.valid) { result.study.state=unavailable(known.reason,window.overflow?CoinFlowCoverage::Overflow:CoinFlowCoverage::Unavailable); return result; }
    const auto coverage=coverageAt(window.periods,from,through,decision);
    int count=0; qint64 latest=0; const CoinTradeObservation* newest=nullptr;
    for(const auto* row:known.rows) {
        const auto& source=*row->source;
        if(source.tradeAtMs<from||source.epoch!=coverage.epoch) continue;
        if(source.side==CoinFlowSide::Buy) { result.buy=add(result.buy,row->notional); result.signedBase=add(result.signedBase,row->quantity); }
        else { result.sell=add(result.sell,row->notional); result.signedBase=subtract(result.signedBase,row->quantity); }
        if(source.knownAtMs>=latest) { latest=source.knownAtMs; newest=&source; } ++count;
    }
    const CoinFlowEvidence evidence=newest?tradeEvidence(window,*newest)
        :CoinFlowEvidence{window.source->evidence.source,window.source->evidence.sourceUrl,0,0,0};
    result.study.state=state(coverage,from,through,count,evidence,latest);
    if(!coverage.good) return result;
    result.difference=subtract(result.buy,result.sell); result.total=add(result.buy,result.sell);
    result.study.buyQuoteNotional=textNumber(result.buy); result.study.sellQuoteNotional=textNumber(result.sell);
    if(result.total.digits=="0"||(requireSamples&&count<30)) {
        result.study.state.available=false; result.study.state.coverage=CoinFlowCoverage::WarmingUp;
        result.study.state.reason="완료 1분의 실제 체결 표본 30개와 양수 분모가 필요합니다"; return result;
    }
    result.study.signedFraction=ratio(result.difference,result.total);
    if(!result.study.signedFraction) {
        result.study.state.available=false; result.study.state.reason="비율을 유한한 표시 좌표로 나타낼 수 없습니다";
    }
    return result;
}
CoinCvdStudy cvdStudy(const Window& window,qint64 asOf) {
    CoinCvdStudy result; result.sourceDirectionOnly=window.source->instrument.venue==CoinFlowVenue::UpbitSpot;
    const auto known=knownTrades(window,asOf,asOf);
    if(!known.valid) { result.state=unavailable(known.reason,window.overflow?CoinFlowCoverage::Overflow:CoinFlowCoverage::Unavailable); return result; }
    quint64 epoch=0; qint64 previous=-1,begin=0; int count=0; Number cumulative;
    result.state=unavailable("관측한 체결과 연속 수집 구간이 없습니다");
    for(const auto* row:known.rows) {
        const auto& source=*row->source;
        auto coverage=coverageAt(window.periods,source.tradeAtMs,source.tradeAtMs+1,asOf,source.epoch);
        if(!coverage.good) { previous=-1; continue; }
        const bool broken=epoch!=source.epoch||previous<0
            ||!coverageAt(window.periods,previous,source.tradeAtMs+1,asOf,source.epoch).good;
        if(broken) {
            // An inconsistent caller may retain one epoch across a gap. Do not
            // invent an epoch or let the renderer join the earlier component.
            if(epoch==source.epoch) {
                for(int i=int(result.points.size())-1;i>=0;--i) if(result.points[i].epoch==epoch) result.points.removeAt(i);
            }
            cumulative={}; begin=source.tradeAtMs; count=0;
        }
        cumulative=source.side==CoinFlowSide::Buy?add(cumulative,row->quantity):subtract(cumulative,row->quantity);
        epoch=source.epoch; previous=source.tradeAtMs;
        result.points.append({source.tradeAtMs,std::max(source.knownAtMs,coverage.knownAtMs),source.epoch,textNumber(cumulative)});
        result.state=state(coverage,begin,source.tradeAtMs+1,++count,tradeEvidence(window,source),source.knownAtMs);
    }
    const CoinFlowCoveragePeriod* latest=nullptr;
    for(const auto& period:window.periods) if(period.knownAtMs>0&&period.knownAtMs<=asOf&&period.fromMs<=asOf
        &&(!latest||period.fromMs>latest->fromMs||(period.fromMs==latest->fromMs&&period.knownAtMs>latest->knownAtMs))) latest=&period;
    if(latest&&!goodCoverage(latest->coverage)) {
        result.state.available=false; result.state.coverage=latest->coverage;
        result.state.reason=latest->reason.isEmpty()?QString("현재 CVD 수집 구간이 끊겼습니다"):latest->reason;
    }
    return result;
}
struct MinuteRow {
    const CoinFlowClosedMinute* source=nullptr;
    Number open,high,low,close,volume;
    bool valid=false;
};
bool sameCandle(const MarketCandle& left,const MarketCandle& right) {
    return left.startsAtMs==right.startsAtMs&&left.lastStoredTickMs==right.lastStoredTickMs
        &&left.open==right.open&&left.high==right.high&&left.low==right.low&&left.close==right.close&&left.volume==right.volume;
}
bool independentCandles(const CoinFlowSnapshot& input,qint64 asOf) {
    if(!input.candleSeries) return false;
    const auto& proof=*input.candleSeries;
    if(!proof.available||proof.identity!=input.identity||!publicIdentity(proof.identity)||proof.intervalMs!=Minute
        ||proof.forming.has_value()||proof.receivedAtMs<=0||proof.receivedAtMs>asOf||proof.sourceTimeMs<0
        ||proof.sourceTimeMs>proof.receivedAtMs||proof.candles.isEmpty()||proof.candles.size()>MaxMinutes
        ||proof.candles.size()!=input.minutes.size()||proof.sourceUrl!=MarketCandles::request(input.identity,Minute).url.toString()) return false;
    const auto parts=input.identity.split(':');
    if(parts[1]=="upbit") {
        if(proof.asset!=parts[2].section('-',1,1)||proof.currency!=parts[2].section('-',0,0)) return false;
    } else if(!binanceIdentifierValid(proof.asset)||!binanceIdentifierValid(proof.currency)
        ||parts[2]!=proof.asset+proof.currency) return false;
    for(int index=0;index<input.minutes.size();++index) {
        const auto& minute=input.minutes[index]; const auto& evidence=minute.evidence;
        if(!sameCandle(proof.candles[index],minute.candle)||evidence.sourceUrl!=proof.sourceUrl
            ||evidence.source.isEmpty()||evidence.source.size()>256||minute.candle.lastStoredTickMs<0||minute.candle.lastStoredTickMs>minute.knownAtMs
            ||evidence.receivedAtMs<=0||evidence.receivedAtMs!=minute.knownAtMs||evidence.receivedAtMs>proof.receivedAtMs
            ||evidence.knownAtMs!=minute.knownAtMs||evidence.knownAtMs>minute.decisionAtMs
            ||evidence.sourceTimeMs<0||evidence.sourceTimeMs>evidence.receivedAtMs) return false;
    }
    return true;
}
QList<MinuteRow> minutes(const QList<CoinFlowClosedMinute>& input) {
    QList<MinuteRow> result;
    if(input.size()>MaxMinutes) return result;
    qint64 previous=-1; bool ordered=true;
    for(const auto& source:input) {
        MinuteRow row; row.source=&source; const auto& candle=source.candle;
        row.valid=candle.startsAtMs>=0&&candle.startsAtMs%Minute==0
            &&candle.startsAtMs<=std::numeric_limits<qint64>::max()-Horizon-Minute
            &&source.completesAtMs==candle.startsAtMs+Minute&&source.knownAtMs>=source.completesAtMs
            &&source.decisionAtMs>=source.knownAtMs&&source.evidence.receivedAtMs<=source.knownAtMs
            &&number(candle.open,true,&row.open)&&number(candle.high,true,&row.high)
            &&number(candle.low,true,&row.low)&&number(candle.close,true,&row.close)&&number(candle.volume,false,&row.volume);
        row.valid=row.valid&&compare(row.low,row.high)<=0&&compare(row.low,row.open)<=0&&compare(row.low,row.close)<=0
            &&compare(row.high,row.open)>=0&&compare(row.high,row.close)>=0;
        if(candle.startsAtMs<=previous) ordered=false;
        previous=candle.startsAtMs; result.append(row);
    }
    if(!ordered) for(auto& row:result) row.valid=false;
    return result;
}
int contiguousBegin(const QList<MinuteRow>& rows,int end,qint64 decision) {
    if(end<0||!rows[end].valid||rows[end].source->knownAtMs>decision) return end+1;
    int begin=end;
    while(begin>0&&rows[begin].source->connectsPrevious&&rows[begin-1].valid
        &&rows[begin-1].source->knownAtMs<=decision
        &&rows[begin].source->candle.startsAtMs-rows[begin-1].source->candle.startsAtMs==Minute) --begin;
    return begin;
}
CoinFlowAvailability candleState(const QList<MinuteRow>& rows,int from,int through) {
    CoinFlowAvailability result; result.available=true; result.coverage=CoinFlowCoverage::ObservedContinuous;
    result.fromMs=rows[from].source->candle.startsAtMs; result.throughMs=rows[through].source->completesAtMs;
    result.samples=through-from+1;
    for(int i=from;i<=through;++i) {
        result.knownAtMs=std::max(result.knownAtMs,rows[i].source->knownAtMs);
        result.evidence.append(rows[i].source->evidence);
    }
    return result;
}
CoinVolumeStudy volumeStudy(const QList<MinuteRow>& rows,int end,qint64 decision) {
    CoinVolumeStudy result; const int begin=contiguousBegin(rows,end,decision);
    result.state=unavailable("연속 완료 1분 봉 21개가 필요합니다",CoinFlowCoverage::WarmingUp,std::max(0,end-begin+1));
    if(end-begin+1<21) return result;
    QList<Number> previous; for(int i=end-20;i<end;++i) previous.append(rows[i].volume);
    std::sort(previous.begin(),previous.end(),[](const Number& a,const Number& b){return compare(a,b)<0;});
    auto median=times(add(previous[9],previous[10]),5); ++median.scale; median=compact(median.digits,median.scale);
    result.currentVolume=rows[end].source->candle.volume; result.previousMedian=textNumber(median);
    result.state=candleState(rows,end-20,end);
    result.multiple=ratio(rows[end].volume,median);
    if(!result.multiple) {
        result.state.available=false; result.state.reason="이전 20봉 중앙값이 0이거나 유한한 배수 표시가 불가능합니다";
    }
    return result;
}
CoinReversalStudy reversalStudy(const QList<MinuteRow>& rows,int end,qint64 decision) {
    CoinReversalStudy result; const int begin=contiguousBegin(rows,end,decision);
    result.state=unavailable("연속 완료 1분 봉 4개가 필요합니다",CoinFlowCoverage::WarmingUp,std::max(0,end-begin+1));
    if(end-begin+1<4) return result;
    result.state=candleState(rows,end-3,end);
    const auto& a=rows[end-3].close; const auto& b=rows[end-2].close;
    const auto& c=rows[end-1].close; const auto& d=rows[end].close;
    result.rebound=compare(a,b)>0&&compare(b,c)>0&&compare(subtract(b,c),subtract(a,b))<0
        &&compare(d,c)>0&&compare(d,rows[end].open)>0;
    result.turnDown=compare(a,b)<0&&compare(b,c)<0&&compare(subtract(c,b),subtract(b,a))<0
        &&compare(d,c)<0&&compare(d,rows[end].open)<0;
    return result;
}
CoinOiStudy oiStudy(const CoinFlowSnapshot& input,qint64 decision,qint64 through,qint64 metadataAsOf) {
    CoinOiStudy result;
    if(input.openInterest.size()>MaxOi) { result.state=unavailable("OI 보관 한도를 넘었습니다",CoinFlowCoverage::Overflow); return result; }
    const CoinFlowInstrument* contract=nullptr;
    if(input.primary.instrument.venue==CoinFlowVenue::BinanceUsdM&&instrumentValid(input.primary.instrument,metadataAsOf)) contract=&input.primary.instrument;
    else if(input.companion&&input.companion->instrument.venue==CoinFlowVenue::BinanceUsdM&&instrumentValid(input.companion->instrument,metadataAsOf)) contract=&input.companion->instrument;
    result.state=unavailable("검증된 같은 종목의 OI 5분 이력 2개가 필요합니다",CoinFlowCoverage::WarmingUp);
    if(!contract||input.primary.instrument.identity!=input.identity||!instrumentValid(input.primary.instrument,metadataAsOf)||contract->baseAsset!=input.primary.instrument.baseAsset
        ||contract->quoteAsset!=input.primary.instrument.quoteAsset) return result;
    QList<const CoinOiPoint*> known; QHash<qint64,const CoinOiPoint*> ends;
    for(const auto& point:input.openInterest) {
        if(point.knownAtMs>decision||point.periodEndMs>through) continue;
        Number quantity,value;
        if(point.symbol!=contract->symbol||point.periodEndMs<0||point.periodEndMs%(5*Minute)!=0
            ||point.knownAtMs<=0||point.knownAtMs<point.periodEndMs||point.evidence.receivedAtMs>point.knownAtMs
            ||!number(point.quantity,false,&quantity)||!number(point.value,false,&value)) {
            result.state=unavailable("OI 원문·종목·기간 끝·수신 시각 검증에 실패했습니다"); return result;
        }
        const auto duplicate=ends.constFind(point.periodEndMs);
        if(duplicate!=ends.cend()) {
            if((*duplicate)->quantity!=point.quantity||(*duplicate)->value!=point.value) {
                result.state=unavailable("같은 OI 기간 끝의 원문이 충돌합니다"); return result;
            }
            continue;
        }
        ends.insert(point.periodEndMs,&point); known.append(&point);
    }
    std::sort(known.begin(),known.end(),[](const auto* a,const auto* b){return a->periodEndMs<b->periodEndMs;});
    result.state.samples=int(known.size());
    if(known.size()<2) return result;
    const auto& previous=*known[known.size()-2]; const auto& current=*known.last();
    result.previousEndMs=previous.periodEndMs; result.currentEndMs=current.periodEndMs;
    result.previousQuantity=previous.quantity; result.currentQuantity=current.quantity;
    result.previousValue=previous.value; result.currentValue=current.value;
    if(current.periodEndMs-previous.periodEndMs!=5*Minute) {
        result.state=unavailable("실제 OI 5분 이력 사이에 누락이 있습니다",CoinFlowCoverage::Partial,2); return result;
    }
    const auto origin=exact(previous.quantity),latest=exact(current.quantity);
    result.quantityChangePercent=ratio(times(subtract(latest,origin),100),origin);
    const auto valueOrigin=exact(previous.value);
    result.valueChangePercent=ratio(times(subtract(exact(current.value),valueOrigin),100),valueOrigin);
    result.state.available=result.quantityChangePercent.has_value(); result.state.samples=2;
    result.state.coverage=CoinFlowCoverage::ObservedContinuous;
    result.state.fromMs=previous.periodEndMs; result.state.throughMs=current.periodEndMs;
    result.state.knownAtMs=std::max(previous.knownAtMs,current.knownAtMs);
    result.state.evidence={previous.evidence,current.evidence};
    result.state.reason=result.state.available?QString{}:QString("OI 원점이 0이거나 유한한 변화율 표시가 불가능합니다");
    return result;
}
CoinSpotFuturesStudy comparisonStudy(const Window& primary,const std::optional<Window>& companion,qint64 decision) {
    CoinSpotFuturesStudy result; result.state=unavailable("같은 base·quote의 검증된 현물·선물 공통 관측 구간이 없습니다");
    if(!companion||!primary.valid||!companion->valid) return result;
    const Window* spot=&primary; const Window* future=&*companion;
    if(spot->source->instrument.venue==CoinFlowVenue::BinanceUsdM) std::swap(spot,future);
    const auto& left=spot->source->instrument; const auto& right=future->source->instrument;
    if(left.venue!=CoinFlowVenue::BinanceSpot||right.venue!=CoinFlowVenue::BinanceUsdM
        ||left.baseAsset!=right.baseAsset||left.quoteAsset!=right.quoteAsset) return result;
    for(const Window* window:{spot,future}) {
        const CoinFlowCoveragePeriod* latest=nullptr;
        for(const auto& period:window->periods) {
            if(period.knownAtMs<=0||period.knownAtMs>decision||period.fromMs>decision||period.throughMs<=period.fromMs) continue;
            if(!latest||period.fromMs>latest->fromMs||(period.fromMs==latest->fromMs&&period.knownAtMs>latest->knownAtMs)) latest=&period;
        }
        if(latest&&!goodCoverage(latest->coverage)) {
            result.state.coverage=latest->coverage;
            result.state.reason=latest->reason.isEmpty()?QString("현물·선물의 최신 관측 구간이 일부 누락됐습니다"):latest->reason;
            return result;
        }
    }
    qint64 from=0,through=0;
    for(const auto& a:spot->periods) for(const auto& b:future->periods) {
        if(a.knownAtMs<=0||b.knownAtMs<=0||a.knownAtMs>decision||b.knownAtMs>decision
            ||!goodCoverage(a.coverage)||!goodCoverage(b.coverage)) continue;
        const auto begin=std::max(a.fromMs,b.fromMs),end=std::min(a.throughMs,b.throughMs);
        if(end>begin&&end<=decision&&(end>through||(end==through&&begin<from))) { from=begin; through=end; }
    }
    if(through<=from) return result;
    const auto spotTotals=totals(*spot,from,through,decision,false),futureTotals=totals(*future,from,through,decision,false);
    if(!spotTotals.study.state.available||!futureTotals.study.state.available) return result;
    result.state=spotTotals.study.state; result.state.samples=spotTotals.study.state.samples+futureTotals.study.state.samples;
    result.state.knownAtMs=std::max(result.state.knownAtMs,futureTotals.study.state.knownAtMs);
    result.state.evidence.append(futureTotals.study.state.evidence);
    if(futureTotals.study.state.coverage==CoinFlowCoverage::RecoveredOverlap) result.state.coverage=CoinFlowCoverage::RecoveredOverlap;
    result.spotSamples=spotTotals.study.state.samples; result.futuresSamples=futureTotals.study.state.samples;
    result.spotSignedFraction=spotTotals.study.signedFraction; result.futuresSignedFraction=futureTotals.study.signedFraction;
    result.spotSignedBaseQuantity=textNumber(spotTotals.signedBase); result.futuresSignedBaseQuantity=textNumber(futureTotals.signedBase);
    result.directionsAgree=compare(spotTotals.difference,Number{})==compare(futureTotals.difference,Number{});
    return result;
}
CoinLiquidationStudy liquidationStudy(const CoinFlowSnapshot& input,qint64 asOf) {
    CoinLiquidationStudy result;
    result.state=unavailable("관측한 공개 청산 snapshot이 없습니다");
    if(input.liquidations.size()>MaxLiquidations||input.liquidationCoverage.size()>2*MaxTrades) {
        result.state=unavailable("공개 청산 snapshot 보관 한도를 넘었습니다",CoinFlowCoverage::Overflow); return result;
    }
    const CoinFlowInstrument* contract=nullptr;
    if(input.primary.instrument.venue==CoinFlowVenue::BinanceUsdM&&instrumentValid(input.primary.instrument,asOf)) contract=&input.primary.instrument;
    else if(input.companion&&instrumentValid(input.companion->instrument,asOf)&&input.companion->instrument.venue==CoinFlowVenue::BinanceUsdM) contract=&input.companion->instrument;
    if(!contract||input.primary.instrument.identity!=input.identity||!instrumentValid(input.primary.instrument,asOf)||contract->baseAsset!=input.primary.instrument.baseAsset
        ||contract->quoteAsset!=input.primary.instrument.quoteAsset) return result;
    QSet<QString> duplicate;
    for(const auto& item:input.liquidations) {
        if(item.knownAtMs>asOf||item.tradeAtMs>asOf) continue;
        Number price,average,quantity,last,cumulative;
        if(item.symbol!=contract->symbol||(item.side!="BUY"&&item.side!="SELL")||item.epoch==0
            ||item.tradeAtMs<0||item.tradeAtMs==std::numeric_limits<qint64>::max()||item.eventAtMs<0||item.receivedAtMs<=0||item.knownAtMs<item.receivedAtMs
            ||item.tradeAtMs>item.knownAtMs||item.eventAtMs>item.knownAtMs
            ||!number(item.price,false,&price)||!number(item.averagePrice,false,&average)||!number(item.originalQuantity,true,&quantity)
            ||!number(item.lastFilledQuantity,false,&last)||!number(item.cumulativeFilledQuantity,false,&cumulative)
            ||magnitude(last,cumulative)>0||magnitude(cumulative,quantity)>0) {
            result.observations.clear(); result.state=unavailable("공개 청산 snapshot 원문 검증에 실패했습니다"); return result;
        }
        const QString key=QString::number(item.epoch)+'|'+item.symbol+'|'+item.side+'|'+item.price+'|'+item.averagePrice+'|'
            +item.originalQuantity+'|'+item.lastFilledQuantity+'|'+item.cumulativeFilledQuantity+'|'+item.orderStatus+'|'
            +QString::number(item.tradeAtMs)+'|'+QString::number(item.eventAtMs);
        if(duplicate.contains(key)) continue;
        duplicate.insert(key); result.observations.append(item);
    }
    if(result.observations.isEmpty()) return result;
    std::stable_sort(result.observations.begin(),result.observations.end(),[](const auto& a,const auto& b){return a.tradeAtMs<b.tradeAtMs;});
    result.state.available=true; result.state.samples=int(result.observations.size());
    result.state.coverage=CoinFlowCoverage::Partial;
    result.state.fromMs=result.observations.first().tradeAtMs; result.state.throughMs=result.observations.last().tradeAtMs+1;
    result.state.reason="종목별 1초 마지막 snapshot의 관측 목록입니다. 전체 청산 합계가 아닙니다";
    for(const auto& period:input.liquidationCoverage) {
        if(period.knownAtMs<=0||period.knownAtMs>asOf||period.throughMs<=period.fromMs) continue;
        if(period.fromMs<=result.state.fromMs&&period.throughMs>=result.state.throughMs) {
            result.state.fromMs=period.fromMs; result.state.throughMs=period.throughMs;
            result.state.coverage=period.coverage; result.state.knownAtMs=std::max(result.state.knownAtMs,period.knownAtMs);
            if(!period.reason.isEmpty()) result.state.reason+=" · "+period.reason;
        }
    }
    for(const auto& item:result.observations) {
        result.state.knownAtMs=std::max(result.state.knownAtMs,item.knownAtMs); result.state.evidence.append(item.evidence);
    }
    return result;
}
void appendEntry(CoinFlowStudySet& result,CoinFlowSignalKind kind,CoinFlowDirection direction,
    const MinuteRow& row,const CoinFlowAvailability& availability,std::optional<double> value={}) {
    result.entries.append({kind,direction,row.source->completesAtMs,row.source->decisionAtMs,
        row.source->candle.close,availability,value});
}
int groupKey(CoinFlowSignalKind kind,CoinFlowDirection direction) { return int(kind)*3+int(direction); }
void groupEntries(CoinFlowStudySet& result,const QList<MinuteRow>& rows,qint64 asOf) {
    QMap<int,int> active; int cursor=0; qint64 previous=0;
    for(const auto& row:rows) {
        const auto& source=*row.source;
        if(source.knownAtMs>asOf||source.decisionAtMs>asOf||source.completesAtMs>asOf) continue;
        const bool contiguous=previous>0&&source.connectsPrevious&&source.completesAtMs-previous==Minute;
        if(!contiguous) {
            for(auto it=active.cbegin();it!=active.cend();++it) result.groups[it.value()].ended=true;
            active.clear();
        }
        QSet<int> present;
        while(cursor<result.entries.size()&&result.entries[cursor].atMs<source.completesAtMs) ++cursor;
        int next=cursor;
        while(next<result.entries.size()&&result.entries[next].atMs==source.completesAtMs) {
            const auto& entry=result.entries[next++]; const int key=groupKey(entry.kind,entry.direction); present.insert(key);
            auto existing=active.find(key);
            if(existing!=active.end()) {
                auto& group=result.groups[existing.value()]; group.throughMs=entry.atMs; ++group.minutes;
            } else {
                active.insert(key,int(result.groups.size()));
                result.groups.append({entry.kind,entry.direction,entry.atMs,entry.atMs,entry.knownAtMs,entry.entryClose,1,false});
            }
        }
        cursor=next;
        for(auto it=active.begin();it!=active.end();) {
            if(!present.contains(it.key())) { result.groups[it.value()].ended=true; it=active.erase(it); }
            else ++it;
        }
        previous=source.completesAtMs;
    }
}
void evaluate(CoinFlowStudySet& result,const QList<MinuteRow>& rows,qint64 asOf) {
    auto& score=result.scorecard; qint64 previousEnd=0; long double total=0;
    for(const auto& group:result.groups) {
        const qint64 target=group.fromMs+Horizon;
        if(group.fromMs<previousEnd) ++score.overlappingSignals;
        previousEnd=std::max(previousEnd,target);
        int start=-1,end=-1;
        for(int i=0;i<rows.size();++i) {
            if(rows[i].source->completesAtMs==group.fromMs) start=i;
            if(rows[i].source->completesAtMs==target) end=i;
        }
        bool complete=start>=0&&end>start&&target<=asOf;
        if(complete) for(int i=start;i<=end;++i) {
            const auto& row=rows[i]; const auto& source=*row.source;
            if(!row.valid||!source.liveEligible||source.knownAtMs>asOf||source.decisionAtMs>asOf
                ||(i>start&&(!source.connectsPrevious||source.completesAtMs-rows[i-1].source->completesAtMs!=Minute))) {
                complete=false; break;
            }
        }
        if(!complete) { ++score.unresolvedSignals; continue; }
        const Number change=subtract(rows[end].close,exact(group.entryClose));
        const auto value=ratio(times(change,100),exact(group.entryClose));
        if(!value) { ++score.unresolvedSignals; continue; }
        score.outcomes.append({group.kind,group.fromMs,target,rows[end].source->knownAtMs,*value});
        ++score.samples; total+=*value;
        const int direction=compare(change,Number{}); score.up+=direction>0; score.down+=direction<0; score.flat+=direction==0;
    }
    score.available=score.samples>0;
    if(score.available) score.meanReturnPercent=static_cast<double>(total/score.samples);
    else score.reason="실제 1시간 뒤까지 연속 관측·완료된 평가 표본이 없습니다";
}
} // namespace

static CoinFlowStudySet calculateFlowStudies(const CoinFlowSnapshot& input,qint64 asOfMs,bool reversalEvidence) {
    CoinFlowStudySet result; result.identity=input.identity; result.asOfMs=asOfMs;
    const QString missing="해당 공개 원천의 관측 자료가 없습니다";
    result.largeTrade.state=result.taker.state=result.openInterest.state=result.volume.state=result.reversal.state
        =result.cvd.state=result.comparison.state=result.liquidation.state=unavailable(missing);
    if(asOfMs<=0||!publicIdentity(input.identity)) {
        result.reason="공개 종목 identity 또는 평가 시각이 유효하지 않습니다"; return result;
    }
    Window primary=prepare(input.primary,asOfMs);
    if(input.primary.instrument.identity!=input.identity) primary.valid=false;
    std::optional<Window> companion; if(input.companion) companion=prepare(*input.companion,asOfMs);
    const bool legacyInstrument=instrumentValid(input.primary.instrument,asOfMs)&&input.primary.instrument.identity==input.identity;
    const bool candleValid=input.candleSeries?independentCandles(input,asOfMs):legacyInstrument;
    const auto rows=candleValid?minutes(input.minutes):QList<MinuteRow>{};
    int latest=-1;
    for(int i=0;i<rows.size();++i) {
        const auto& source=*rows[i].source;
        if(source.knownAtMs<=asOfMs&&source.completesAtMs<=asOfMs) latest=i;
    }
    const bool sourceDirection=input.identity.startsWith("coin:upbit:");
    result.taker.sourceDirectionOnly=result.cvd.sourceDirectionOnly=sourceDirection;
    for(int i=0;i<rows.size();++i) {
        const auto& row=rows[i]; const auto& source=*row.source;
        if(!row.valid||source.knownAtMs>asOfMs||source.decisionAtMs>asOfMs||source.completesAtMs>asOfMs||!source.liveEligible) continue;
        const qint64 decision=source.decisionAtMs,through=source.completesAtMs,from=source.candle.startsAtMs;
        const auto volume=volumeStudy(rows,i,decision);
        const auto reversal=reversalStudy(rows,i,decision);
        if(volume.state.available&&compare(rows[i].volume,times(exact(volume.previousMedian),2))>=0)
            appendEntry(result,CoinFlowSignalKind::VolumeBurst,CoinFlowDirection::Neutral,row,volume.state,volume.multiple);
        if(reversal.state.available&&(reversal.rebound||reversal.turnDown)) {
            // These inputs are evaluated at the immutable first closed-minute
            // decision, including coverage/receipt times. A late trade cannot
            // upgrade this event when a later worker snapshot is calculated.
            CoinTakerStudy spot;
            if(reversalEvidence&&primary.source->instrument.venue!=CoinFlowVenue::BinanceUsdM
                &&instrumentValid(primary.source->instrument,decision))
                spot=totals(primary,through-5*Minute,through,decision,false).study;
            if(reversal.rebound) {
                appendEntry(result,CoinFlowSignalKind::Rebound,CoinFlowDirection::Up,row,reversal.state);
                if(reversalEvidence)result.entries.last().reversalCorroboration=CoinReversalCorroboration{volume,spot};
            }
            if(reversal.turnDown) {
                appendEntry(result,CoinFlowSignalKind::TurnDown,CoinFlowDirection::Down,row,reversal.state);
                if(reversalEvidence)result.entries.last().reversalCorroboration=CoinReversalCorroboration{volume,spot};
            }
        }
        const auto flow=totals(primary,from,through,decision);
        if(flow.study.state.available&&!sourceDirection&&magnitude(times(flow.difference,10),times(flow.total,3))>=0)
            appendEntry(result,CoinFlowSignalKind::TakerSkew,flow.difference.negative?CoinFlowDirection::Down:CoinFlowDirection::Up,
                row,flow.study.state,flow.study.signedFraction);
        const auto large=largeStudy(primary,decision,through,from);
        for(const auto side:{CoinFlowSide::Buy,CoinFlowSide::Sell}) {
            const CoinLargeTradePoint* candidate=nullptr;
            for(const auto& point:large.candidates) {
                if(point.side!=side||point.tradeAtMs<from||point.tradeAtMs>=through) continue;
                bool live=false;
                for(const auto& trade:input.primary.trades) if(trade.id==point.id&&trade.tradeAtMs==point.tradeAtMs
                    &&trade.liveEligible&&trade.knownAtMs<=decision) { live=true; break; }
                if(live&&(!candidate||point.percentile>candidate->percentile)) candidate=&point;
            }
            if(candidate&&large.state.available) appendEntry(result,CoinFlowSignalKind::LargeTrade,
                side==CoinFlowSide::Buy?CoinFlowDirection::Up:CoinFlowDirection::Down,row,large.state,candidate->percentile);
        }
        const auto oi=oiStudy(input,decision,through,asOfMs);
        if(oi.state.available) {
            const auto origin=exact(oi.previousQuantity),change=subtract(exact(oi.currentQuantity),origin);
            const auto previous=i>0?oiStudy(input,rows[i-1].source->decisionAtMs,rows[i-1].source->completesAtMs,asOfMs):CoinOiStudy{};
            bool live=false;
            for(const auto& point:input.openInterest) if(point.periodEndMs==oi.currentEndMs&&point.liveEligible&&point.knownAtMs<=decision) live=true;
            if(live&&(!previous.state.available||previous.currentEndMs!=oi.currentEndMs)&&magnitude(times(change,100),origin)>=0)
                appendEntry(result,CoinFlowSignalKind::OpenInterestJump,change.negative?CoinFlowDirection::Down:CoinFlowDirection::Up,
                    row,oi.state,oi.quantityChangePercent);
        }
    }
    std::stable_sort(result.entries.begin(),result.entries.end(),[](const auto& a,const auto& b) {
        if(a.atMs!=b.atMs) return a.atMs<b.atMs;
        return groupKey(a.kind,a.direction)<groupKey(b.kind,b.direction);
    });
    if(latest>=0) {
        const auto& row=rows[latest]; const qint64 through=row.source->completesAtMs;
        result.lastCompletedAtMs=through;
        result.volume=volumeStudy(rows,latest,asOfMs); result.reversal=reversalStudy(rows,latest,asOfMs);
        result.taker=totals(primary,row.source->candle.startsAtMs,through,asOfMs).study;
        result.largeTrade=largeStudy(primary,asOfMs,through);
    } else {
        result.largeTrade=largeStudy(primary,asOfMs,asOfMs);
    }
    if(input.minutes.size()>MaxMinutes) {
        result.volume.state=result.reversal.state=unavailable("완료 1분 봉 보관 한도를 넘었습니다",CoinFlowCoverage::Overflow);
    }
    result.cvd=cvdStudy(primary,asOfMs);
    result.taker.sourceDirectionOnly=result.cvd.sourceDirectionOnly=sourceDirection;
    result.openInterest=oiStudy(input,asOfMs,asOfMs,asOfMs);
    result.comparison=comparisonStudy(primary,companion,asOfMs);
    result.liquidation=liquidationStudy(input,asOfMs);
    groupEntries(result,rows,asOfMs); evaluate(result,rows,asOfMs);
    result.available=result.largeTrade.state.available||result.taker.state.available||result.openInterest.state.available
        ||result.volume.state.available||result.reversal.state.available||result.cvd.state.available
        ||result.comparison.state.available||result.liquidation.state.available;
    if(!result.available) result.reason="현재 사용할 수 있는 공개 관측 표본이 부족합니다";
    return result;
}
std::optional<qint64> coinFlowMarkerCandleAt(qint64 completedAtMs,const QList<MarketCandle>& rows,qint64 intervalMs) {
    if(completedAtMs<=0||intervalMs<=0)return std::nullopt;
    const auto at=completedAtMs-1;
    const auto after=std::upper_bound(rows.cbegin(),rows.cend(),at,[](qint64 time,const MarketCandle& row){return time<row.startsAtMs;});
    if(after==rows.cbegin())return std::nullopt;
    const auto& row=*(after-1);
    if(row.startsAtMs<=0||at-row.startsAtMs>=intervalMs)return std::nullopt;
    return row.startsAtMs;
}
CoinFlowDisplayState coinFlowDisplayState(const CoinFlowStudySet& study) {
    CoinFlowDisplayState result;
    const bool price=study.volume.state.available&&study.reversal.state.available;
    const bool trade=study.largeTrade.state.available&&study.taker.state.available&&study.taker.signedFraction.has_value();
    // Completed-minute observations remain attributable to that minute. A stale
    // retained snapshot must never be presented as today's confirmed no-signal.
    const bool recent=study.lastCompletedAtMs>0&&study.asOfMs>=study.lastCompletedAtMs
        &&study.asOfMs-study.lastCompletedAtMs<=2*Minute;
    result.requiredReady=price&&trade&&recent;
    for(const auto& entry:study.entries) {
        if(!entry.state.available||entry.atMs<=0||entry.knownAtMs<=0||entry.knownAtMs>study.asOfMs
            ||entry.atMs>study.lastCompletedAtMs)continue;
        result.latestSignalAtMs=std::max(result.latestSignalAtMs,entry.atMs);
        if(recent&&entry.atMs==study.lastCompletedAtMs)++result.currentSignals;
    }
    if(result.currentSignals>0)result.phase=CoinFlowDisplayPhase::Signal;
    else if(result.requiredReady)result.phase=CoinFlowDisplayPhase::ObservedNoSignal;
    QStringList missing;
    if(!study.largeTrade.state.available)missing<<study.largeTrade.state.reason;
    if(!study.taker.state.available||!study.taker.signedFraction)missing<<study.taker.state.reason;
    if(!study.volume.state.available)missing<<study.volume.state.reason;
    if(!study.reversal.state.available)missing<<study.reversal.state.reason;
    if(!recent)missing<<QStringLiteral("최근 완료 1분 수신 전");
    missing.removeAll(QString{});missing.removeDuplicates();result.reason=missing.join(QStringLiteral(" · "));
    return result;
}
CoinFlowStudySet coinFlowStudies(const CoinFlowSnapshot& input,qint64 asOfMs) {
    return calculateFlowStudies(input,asOfMs,false);
}
CoinFlowStudySet coinFlowStudiesWithReversalEvidence(const CoinFlowSnapshot& input,qint64 asOfMs) {
    return calculateFlowStudies(input,asOfMs,true);
}
} // namespace keefi
