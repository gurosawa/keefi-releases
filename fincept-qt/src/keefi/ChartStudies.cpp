#include "keefi/ChartStudies.h"
#include "keefi/StockChartTypes.h"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
// Profile membership and volume ranking are discrete decisions: retain exact
// source decimals there, instead of moving boundaries with binary rounding.
struct ExactDecimal { QString digits=QStringLiteral("0");qsizetype scale=0; };
ExactDecimal compact(QString digits,qsizetype scale){
    qsizetype first=0;while(first+1<digits.size()&&digits[first]=='0')++first;
    if(first)digits.remove(0,first);
    if(digits=="0")return {};
    while(scale>0&&digits.endsWith('0')){digits.chop(1);--scale;}
    return {digits,scale};
}
ExactDecimal exact(const QString& text){
    QString digits=text;const auto point=digits.indexOf('.');
    const qsizetype scale=point<0?0:digits.size()-point-1;
    if(point>=0)digits.remove(point,1);return compact(digits,scale);
}
QString aligned(const ExactDecimal& value,qsizetype scale){
    return value.digits=="0"?value.digits:value.digits+QString(scale-value.scale,'0');
}
int integerCompare(const QString& a,const QString& b){
    return a.size()!=b.size()?(a.size()<b.size()?-1:1):QString::compare(a,b);
}
int exactCompare(const ExactDecimal& a,const ExactDecimal& b){
    const auto scale=std::max(a.scale,b.scale);return integerCompare(aligned(a,scale),aligned(b,scale));
}
ExactDecimal exactAdd(const ExactDecimal& a,const ExactDecimal& b){
    const auto scale=std::max(a.scale,b.scale);auto left=aligned(a,scale),right=aligned(b,scale);
    const auto size=std::max(left.size(),right.size());left=left.rightJustified(size,'0');right=right.rightJustified(size,'0');
    QString digits(size,'0');int carry=0;
    for(qsizetype i=size-1;i>=0;--i){const int sum=left[i].digitValue()+right[i].digitValue()+carry;digits[i]=QChar('0'+sum%10);carry=sum/10;}
    if(carry)digits.prepend('1');return compact(digits,scale);
}
ExactDecimal exactMultiply(const ExactDecimal& value,int factor){
    if(factor==0||value.digits=="0")return {};
    QString digits=value.digits;int carry=0;
    for(qsizetype i=digits.size()-1;i>=0;--i){const int product=digits[i].digitValue()*factor+carry;digits[i]=QChar('0'+product%10);carry=product/10;}
    while(carry){digits.prepend(QChar('0'+carry%10));carry/=10;}return compact(digits,value.scale);
}
struct Row {
    qint64 at;double open,high,low,close,volume;
    ExactDecimal sourceHigh,sourceLow,sourceClose,sourceVolume,profileNumerator;
};
int profileBin(const Row& row,const std::array<ExactDecimal,20>& boundaries){
    const auto& left=row.profileNumerator;
    int bin=0,upper=20;
    while(bin+1<upper){const int boundary=(bin+upper)/2;
        if(exactCompare(left,boundaries[boundary])>=0)bin=boundary;else upper=boundary;
    }
    return bin;
}
bool supported(qint64 interval){
    return interval==60000||interval==300000||interval==900000||interval==3600000||interval==14400000||interval==86400000;
}
bool number(const QString& text,bool price,double& value){
    static const QRegularExpression decimal(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));
    if(!decimal.match(text).hasMatch())return false;
    const auto source=exact(text);
    static const ExactDecimal maximum{QString("1")+QString(100,'0'),0},minimum{QString("1"),100};
    if(exactCompare(source,maximum)>0||(price&&exactCompare(source,minimum)<0))return false;
    bool ok=false;value=text.toDouble(&ok);
    return (ok||(!price&&value==0))&&std::isfinite(value)&&value<=1e100&&(price?value>=1e-100:value>=0);
}
double trueRange(const QList<Row>& rows,int index){
    const auto& r=rows[index];const auto prior=rows[index-1].close;
    return std::max({r.high-r.low,std::abs(r.high-prior),std::abs(r.low-prior)});
}
ChartPullback pullback(const QList<Row>& rows,int begin,int end){
    ChartPullback result;
    if(end-begin<21){result.reason=QStringLiteral("완료 봉 21개가 필요합니다");return result;}
    result.status=ChartStudyStatus::NoSignal;
    double atr=0;for(int i=begin+1;i<=begin+20;++i)atr+=trueRange(rows,i);atr/=20;
    for(int i=begin+21;i<end;++i)atr=(19*atr+trueRange(rows,i))/20;
    result.atr=atr;
    const auto reference=rows[end-20].close,lowReference=rows[end-20].low;
    double closeDeltas=0,lowDeltas=0;
    for(int i=end-20;i<end;++i){closeDeltas+=rows[i].close-reference;lowDeltas+=rows[i].low-lowReference;}
    const auto mean=reference+closeDeltas/20,meanLow=lowReference+lowDeltas/20;
    double variance=0,slope=0;
    for(int i=0;i<20;++i){const auto& row=rows[end-20+i];const auto delta=row.close-mean;variance+=delta*delta;slope+=(i-9.5)*(row.low-meanLow);}
    variance/=20;slope/=665;
    QList<ChartEvidence> levels{{QStringLiteral("sma20"),QStringLiteral("이평20"),mean},
                               {QStringLiteral("bbLower20"),QStringLiteral("볼린저 하단"),mean-2*std::sqrt(variance)}};
    const int profileBegin=std::max(begin,end-96);int profileLowIndex=profileBegin,profileHighIndex=profileBegin;
    bool hasVolume=false;
    for(int i=profileBegin;i<end;++i){
        if(exactCompare(rows[i].sourceLow,rows[profileLowIndex].sourceLow)<0)profileLowIndex=i;
        if(exactCompare(rows[i].sourceHigh,rows[profileHighIndex].sourceHigh)>0)profileHighIndex=i;
        hasVolume=hasVolume||rows[i].sourceVolume.digits!="0";
    }
    if(hasVolume){
        const auto& minimum=rows[profileLowIndex].sourceLow;const auto& maximum=rows[profileHighIndex].sourceHigh;
        double level=rows[profileLowIndex].low;
        if(exactCompare(maximum,minimum)>0){
            // Each exact boundary belongs to the window, and each numerator belongs to a row.
            // Reuse the unchanged integer-decimal expressions instead of allocating them per comparison.
            std::array<ExactDecimal,20> boundaries;
            for(int boundary=1;boundary<20;++boundary)
                boundaries[boundary]=exactAdd(exactMultiply(minimum,3*(20-boundary)),exactMultiply(maximum,3*boundary));
            ExactDecimal bins[20];
            for(int i=profileBegin;i<end;++i){const int bin=profileBin(rows[i],boundaries);bins[bin]=exactAdd(bins[bin],rows[i].sourceVolume);}
            int largest=0;for(int i=1;i<20;++i)if(exactCompare(bins[i],bins[largest])>0)largest=i;
            level=rows[profileLowIndex].low+(largest+0.5)*(rows[profileHighIndex].high-rows[profileLowIndex].low)/20;
        }
        levels.append({QStringLiteral("profilePoc96"),QStringLiteral("거래량 대표가격 근사"),level});
    }
    levels.append({QStringLiteral("lowRegression20"),QStringLiteral("저가 회귀선20"),meanLow+9.5*slope});
    const auto close=rows[end-1].close;
    for(const auto& level:levels)if(std::abs(close-level.price)<=0.5*atr)result.evidence.append(level);
    result.overlap=int(result.evidence.size());result.score=std::clamp((result.overlap*5+1)/2,0,10);
    const int waveBegin=std::max(begin,end-60);int lowIndex=waveBegin;
    for(int i=waveBegin+1;i<end-1;++i)if(rows[i].low<rows[lowIndex].low)lowIndex=i;
    int highIndex=lowIndex+1;
    if(highIndex>=end-1){result.reason=QStringLiteral("완료된 저점 이후 고점이 부족합니다");return result;}
    for(int i=highIndex+1;i<end-1;++i)if(rows[i].high>rows[highIndex].high)highIndex=i;
    const auto low=rows[lowIndex].low,high=rows[highIndex].high;
    if(!(atr>0&&high>low&&high-low>=2*atr)){result.reason=QStringLiteral("관측 파동 폭이 부족합니다");return result;}
    result.waveLowAtMs=rows[lowIndex].at;result.waveHighAtMs=rows[highIndex].at;result.waveLow=low;result.waveHigh=high;
    if(result.evidence.isEmpty()){result.reason=QStringLiteral("현재 가격과 겹치는 지지 근거가 없습니다");return result;}
    double minimum=result.evidence.first().price,maximum=minimum;
    for(const auto& evidence:result.evidence){minimum=std::min(minimum,evidence.price);maximum=std::max(maximum,evidence.price);}
    const auto boxLow=std::max(low,minimum-0.5*atr),boxHigh=std::min(high,maximum+0.5*atr);
    if(boxLow>boxHigh){result.reason=QStringLiteral("관측 파동 안의 겹침 구간이 없습니다");return result;}
    result.available=true;result.status=ChartStudyStatus::Signal;result.boxLow=boxLow;result.boxHigh=boxHigh;result.inBox=close>=boxLow&&close<=boxHigh;return result;
}
ChartSimilarity similarity(const QList<Row>& rows,int begin,qint64 interval,const ChartStudyTimeline* timeline=nullptr){
    ChartSimilarity result;const int count=int(rows.size())-begin;
    if(count<20){result.reason=QStringLiteral("비교할 완료 봉 20개가 필요합니다");return result;}
    const int query=int(rows.size())-20;
    result.referenceFromMs=rows[query].at;
    result.referenceThroughMs=timeline?timeline->rows.last().completesAtMs:rows.last().at+interval;
    result.referenceLastStartMs=rows.last().at;
    result.searchBars=query-begin;result.searchFromMs=rows[begin].at;result.searchToMs=rows[query].at;
    if(count<60){result.reason=QStringLiteral("후속 20봉까지 완료된 비교 구간이 없습니다");return result;}
    double queryReturns[19],queryMean=0;
    for(int i=0;i<19;++i){queryReturns[i]=std::log(rows[query+i+1].close/rows[query+i].close);queryMean+=queryReturns[i];}queryMean/=19;
    double queryVariance=0;for(double value:queryReturns){const auto d=value-queryMean;queryVariance+=d*d;}
    if(queryVariance<=1e-24){result.reason=QStringLiteral("현재 구간의 수익률 분산이 부족합니다");return result;}
    struct Candidate {int start;double correlation,rank;};QList<Candidate> candidates;
    for(int start=begin;start+40<=query;++start){
        double returns[19],mean=0;
        for(int i=0;i<19;++i){returns[i]=std::log(rows[start+i+1].close/rows[start+i].close);mean+=returns[i];}mean/=19;
        double variance=0,covariance=0;
        for(int i=0;i<19;++i){const auto d=returns[i]-mean;variance+=d*d;covariance+=d*(queryReturns[i]-queryMean);}
        if(variance<=1e-24)continue;
        const auto raw=covariance/std::sqrt(variance*queryVariance);if(!std::isfinite(raw))continue;
        const auto correlation=std::clamp(raw,-1.0,1.0);
        candidates.append({start,correlation,std::round(correlation*1e12)/1e12});
    }
    result.eligibleCandidates=int(candidates.size());
    if(candidates.isEmpty()){result.reason=QStringLiteral("분산을 비교할 수 있는 과거 구간이 부족합니다");return result;}
    result.status=ChartStudyStatus::NoSignal;
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.rank!=b.rank?a.rank>b.rank:a.start<b.start;});
    QList<Candidate> selected;
    for(const auto& candidate:candidates){
        // Filter the raw correlation; the rounded ordering key must not admit a
        // below-cutoff candidate. Keep eligibleCandidates as the finite pool size.
        if(candidate.correlation<ChartSimilarityMinimumCorrelation)continue;
        bool overlaps=false;for(const auto& prior:selected)if(candidate.start<prior.start+40&&prior.start<candidate.start+40){overlaps=true;break;}
        if(overlaps){++result.overlapDropped;continue;}
        if(selected.size()<3)selected.append(candidate);
    }
    const auto& latest=rows.last();
    for(const auto& candidate:selected){
        const int start=candidate.start;ChartSimilarPath path;
        path.fromMs=rows[start].at;path.toMs=timeline?timeline->rows[start+19].completesAtMs:rows[start+19].at+interval;
        path.followingToMs=timeline?timeline->rows[start+39].completesAtMs:rows[start+39].at+interval;path.correlation=candidate.correlation;
        path.points.append({latest.at,latest.close});
        const auto origin=rows[start+19].close;
        for(int step=1;step<=20;++step)path.points.append({timeline?timeline->nextStartsAtMs[step-1]:latest.at+step*interval,latest.close*(rows[start+19+step].close/origin)});
        const auto end=rows[start+39].close;
        if(end>origin)++result.upPaths;else if(end<origin)++result.downPaths;else ++result.flatPaths;
        result.paths.append(path);
    }
    result.samples=int(result.paths.size());
    if(result.samples==0){result.reason=QStringLiteral("충분히 닮은 과거 구간이 없어요");return result;}
    for(int step=0;step<=20;++step){
        double sum=0,minimum=result.paths.first().points[step].price,maximum=minimum;
        for(const auto& path:result.paths){const auto value=path.points[step].price;sum+=value;minimum=std::min(minimum,value);maximum=std::max(maximum,value);}
        const auto at=timeline?(step==0?latest.at:timeline->nextStartsAtMs[step-1]):latest.at+step*interval;
        result.mean.append({at,sum/result.samples});result.minimum.append({at,minimum});result.maximum.append({at,maximum});
    }
    result.available=true;result.status=ChartStudyStatus::Signal;return result;
}
}
ChartStudySet keefiChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs){
    ChartStudySet result;result.intervalMs=intervalMs;
    const auto invalid=[&](){result.reason=QStringLiteral("완료 봉 입력 형식 또는 범위가 올바르지 않습니다");return result;};
    if(!supported(intervalMs)||closedRows.isEmpty()||closedRows.size()>200)return invalid();
    const qint64 horizon=intervalMs<=300000?3600000:(intervalMs==900000?14400000:86400000);
    const qint64 coordinateRoom=std::max(20*intervalMs,intervalMs+horizon);
    QList<Row> rows;rows.reserve(closedRows.size());qint64 prior=0;
    for(const auto& input:closedRows){
        Row r{input.startsAtMs,0,0,0,0,0};
        if(r.at<=0||r.at%intervalMs!=0||r.at<=prior||r.at>std::numeric_limits<qint64>::max()-coordinateRoom
           ||!number(input.open,true,r.open)||!number(input.high,true,r.high)||!number(input.low,true,r.low)
           ||!number(input.close,true,r.close)||!number(input.volume,false,r.volume)
           ||r.low>r.open||r.low>r.close||r.high<r.open||r.high<r.close||r.low>r.high)return invalid();
        r.sourceHigh=exact(input.high);r.sourceLow=exact(input.low);r.sourceClose=exact(input.close);r.sourceVolume=exact(input.volume);
        const auto sourceOpen=exact(input.open);
        if(exactCompare(r.sourceLow,sourceOpen)>0||exactCompare(r.sourceLow,r.sourceClose)>0
           ||exactCompare(r.sourceHigh,sourceOpen)<0||exactCompare(r.sourceHigh,r.sourceClose)<0
           ||exactCompare(r.sourceLow,r.sourceHigh)>0)return invalid();
        r.profileNumerator=exactMultiply(exactAdd(exactAdd(r.sourceHigh,r.sourceLow),r.sourceClose),20);
        rows.append(r);prior=r.at;
    }
    result.available=true;result.observations=int(rows.size());result.lastCompletedAtMs=rows.last().at+intervalMs;
    result.scorecard.horizonMs=horizon;
    int runBegin=0;double fast=0,slow=0;ChartPullback previous;QSet<qint64> emittedWaves;
    QList<int> entryIndices;
    for(int i=0;i<rows.size();++i){
        const auto& row=rows[i];
        if(i>0&&row.at-rows[i-1].at!=intervalMs){runBegin=i;fast=slow=0;previous={};}
        const int runCount=i-runBegin+1;
        if(runCount==12){fast=0;for(int j=runBegin;j<=i;++j)fast+=rows[j].close;fast/=12;}
        else if(runCount>12)fast+=(row.close-fast)*(2.0/13);
        if(runCount==26){slow=0;for(int j=runBegin;j<=i;++j)slow+=rows[j].close;slow/=26;}
        else if(runCount>26)slow+=(row.close-slow)*(2.0/27);
        if(runCount>=26)result.cloud.append({row.at,fast,slow});
        result.volumes.append({row.at,closedRows[i].volume});
        auto current=pullback(rows,runBegin,i+1);
        const bool previouslyInside=previous.available&&previous.inBox&&previous.waveLowAtMs==current.waveLowAtMs;
        if(current.available&&current.inBox&&current.overlap>=2&&!previouslyInside&&!emittedWaves.contains(current.waveLowAtMs)){
            result.entries.append({row.at+intervalMs,current.waveLowAtMs,row.close,current.score});entryIndices.append(i);emittedWaves.insert(current.waveLowAtMs);
        }
        previous=current;
    }
    result.currentRunBars=int(rows.size())-runBegin;result.pullback=previous;result.similarity=similarity(rows,runBegin,intervalMs);
    auto& score=result.scorecard;qint64 previousEnd=0;double totalReturn=0;
    for(int k=0;k<result.entries.size();++k){
        const auto& signal=result.entries[k];const auto horizon=score.horizonMs;
        if(signal.atMs<previousEnd)++score.overlappingSignals;previousEnd=std::max(previousEnd,signal.atMs+horizon);
        const int start=entryIndices[k],distance=int(horizon/intervalMs),target=start+distance;bool complete=target<rows.size();
        if(complete)for(int i=start+1;i<=target;++i)if(rows[i].at-rows[i-1].at!=intervalMs){complete=false;break;}
        if(!complete||rows[target].at+intervalMs!=signal.atMs+horizon){++score.unresolvedSignals;continue;}
        const double value=100*(rows[target].close/signal.entryClose-1);score.outcomes.append({signal.atMs,signal.atMs+horizon,value});
        ++score.samples;totalReturn+=value;if(value>0)++score.up;else if(value<0)++score.down;else ++score.flat;
    }
    score.available=score.samples>0;
    if(score.available)score.meanReturnPercent=totalReturn/score.samples;else score.reason=QStringLiteral("평가 시각까지 연속해서 완료된 표본이 없습니다");
    return result;
}
}

namespace keefi {
ChartStudySet keefiChartStudies(const QList<MarketCandle>& closedRows,qint64 intervalMs,const ChartStudyTimeline& timeline,QList<ChartCloudPoint>* displayCloud) {
    if(displayCloud)displayCloud->clear();
    ChartStudySet result;result.intervalMs=intervalMs;
    const auto invalid=[&](){result.reason=QStringLiteral("주식 완료 봉·실제 거래 timeline 입력이 올바르지 않습니다");return result;};
    if(!supported(intervalMs)||closedRows.isEmpty()||closedRows.size()>200||closedRows.size()!=timeline.rows.size())return invalid();
    const qint64 horizon=intervalMs<=300000?3600000:(intervalMs==900000?14400000:86400000);
    QList<Row> rows;qint64 prior=0;
    for(int i=0;i<closedRows.size();++i){
        const auto& input=closedRows[i];const auto& state=timeline.rows[i];Row row{input.startsAtMs,0,0,0,0,0};
        if(row.at<=prior||row.at<=0||state.startsAtMs!=row.at||state.completesAtMs<=row.at||state.completesAtMs>std::numeric_limits<qint64>::max()-horizon||
           (state.missingSlotsBefore&&(*state.missingSlotsBefore<0||(state.connectsPrevious&&*state.missingSlotsBefore>0)))||
           !number(input.open,true,row.open)||!number(input.high,true,row.high)||!number(input.low,true,row.low)||!number(input.close,true,row.close)||!number(input.volume,false,row.volume))return invalid();
        row.sourceHigh=exact(input.high);row.sourceLow=exact(input.low);row.sourceClose=exact(input.close);row.sourceVolume=exact(input.volume);
        const auto open=exact(input.open);
        if(exactCompare(row.sourceLow,open)>0||exactCompare(row.sourceLow,row.sourceClose)>0||exactCompare(row.sourceHigh,open)<0||exactCompare(row.sourceHigh,row.sourceClose)<0||exactCompare(row.sourceLow,row.sourceHigh)>0)return invalid();
        row.profileNumerator=exactMultiply(exactAdd(exactAdd(row.sourceHigh,row.sourceLow),row.sourceClose),20);
        rows.append(row);prior=row.at;
    }
    if(timeline.projectionCovered){
        if(timeline.nextStartsAtMs.size()!=20)return invalid();prior=rows.last().at;
        for(const auto at:timeline.nextStartsAtMs){if(at<=prior)return invalid();prior=at;}
    }else if(!timeline.nextStartsAtMs.isEmpty())return invalid();
    result.available=true;result.observations=int(rows.size());result.lastCompletedAtMs=timeline.rows.last().completesAtMs;result.scorecard.horizonMs=horizon;
    int runBegin=0,displayBegin=0;double fast=0,slow=0,displayFast=0,displaySlow=0;ChartPullback previous;QSet<qint64> emittedWaves;QList<int> entryIndices;
    for(int i=0;i<rows.size();++i){
        const auto& row=rows[i];const auto& state=timeline.rows[i];result.volumes.append({row.at,closedRows[i].volume});
        const auto appendCloud=[&](int begin,double& ema12,double& ema26,QList<ChartCloudPoint>& output){
            const int count=i-begin+1;
            if(count==12){ema12=0;for(int j=begin;j<=i;++j)ema12+=rows[j].close;ema12/=12;}
            else if(count>12)ema12+=(row.close-ema12)*(2.0/13);
            if(count==26){ema26=0;for(int j=begin;j<=i;++j)ema26+=rows[j].close;ema26/=26;}
            else if(count>26)ema26+=(row.close-ema26)*(2.0/27);
            if(count>=26)output.append({row.at,ema12,ema26});
        };
        if(displayCloud){
            if(!state.closeDisplayEligible())displayBegin=i+1;
            else{
                if(!state.connectsPrevious)displayBegin=i;
                appendCloud(displayBegin,displayFast,displaySlow,*displayCloud);
            }
        }
        if(!state.eligible){runBegin=i+1;fast=slow=0;previous={};previous.reason=state.reason.isEmpty()?QStringLiteral("주식 판정 제외 구간"):state.reason;continue;}
        if(i==0||!state.connectsPrevious||!timeline.rows[i-1].eligible){runBegin=i;fast=slow=0;previous={};}
        appendCloud(runBegin,fast,slow,result.cloud);
        auto current=pullback(rows,runBegin,i+1);const bool inside=previous.available&&previous.inBox&&previous.waveLowAtMs==current.waveLowAtMs;
        if(current.available&&current.inBox&&current.overlap>=2&&!inside&&!emittedWaves.contains(current.waveLowAtMs)){
            result.entries.append({state.completesAtMs,current.waveLowAtMs,row.close,current.score});entryIndices.append(i);emittedWaves.insert(current.waveLowAtMs);
        }
        previous=current;
    }
    result.currentRunBars=int(rows.size())-runBegin;result.pullback=previous;
    if(!timeline.adjustedComparable)result.similarity.reason=timeline.comparisonReason.isEmpty()?QStringLiteral("수정주가 비교 범위 미확인"):timeline.comparisonReason;
    else if(!timeline.projectionCovered)result.similarity.reason=timeline.projectionReason.isEmpty()?QStringLiteral("미래 거래 좌표 미확인"):timeline.projectionReason;
    else if(runBegin>=rows.size())result.similarity.reason=QStringLiteral("판정 가능한 연속 완료 봉이 없습니다");
    else result.similarity=similarity(rows,runBegin,intervalMs,&timeline);
    auto& score=result.scorecard;qint64 previousEnd=0;double total=0;
    for(int k=0;k<result.entries.size();++k){
        const auto& signal=result.entries[k];const auto targetAt=signal.atMs+horizon;
        if(signal.atMs<previousEnd)++score.overlappingSignals;previousEnd=std::max(previousEnd,targetAt);
        int target=-1;
        if(timeline.adjustedComparable)for(int i=entryIndices[k]+1;i<rows.size();++i){
            if(!timeline.rows[i].eligible||!timeline.rows[i].connectsPrevious)break;
            if(timeline.rows[i].completesAtMs==targetAt){target=i;break;}if(timeline.rows[i].completesAtMs>targetAt)break;
        }
        if(target<0){++score.unresolvedSignals;continue;}
        const double value=100*(rows[target].close/signal.entryClose-1);score.outcomes.append({signal.atMs,targetAt,value});
        ++score.samples;total+=value;if(value>0)++score.up;else if(value<0)++score.down;else ++score.flat;
    }
    score.available=score.samples>0;
    if(score.available)score.meanReturnPercent=total/score.samples;
    else score.reason=timeline.adjustedComparable?QStringLiteral("실제 평가 시각까지 확인된 연속 완료 관측 없음"):QStringLiteral("수정주가 비교 범위 미확인");
    return result;
}
ChartStudySet keefiChartStudies(const QList<MarketCandle>& rows,qint64 interval,const ChartStudyTimeline& timeline){
    return keefiChartStudies(rows,interval,timeline,nullptr);
}
}
