#include "keefi/MarketStudies.h"
#include "keefi/CandleAggregation.h"
#include "keefi/StockChartTypes.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <array>
#include <limits>
#include <QDateTime>
#include <QTimeZone>
namespace keefi {
namespace {
const QList<qint64> intervals{60000,300000,900000,3600000,14400000,86400000};
struct Bar { qint64 at; double open,high,low,close,volume; };
double trueRange(const Bar& b,const Bar& previous){return std::max({b.high-b.low,std::abs(b.high-previous.close),std::abs(b.low-previous.close)});}
QList<Bar> observedRun(const QList<MarketCandle>& rows,qint64 interval,const ChartStudyTimeline* timeline=nullptr) {
    if(rows.size()>200)return {};
    if(timeline&&timeline->rows.size()!=rows.size())return {};
    static const QRegularExpression canonical(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));
    QList<Bar> bars;
    qint64 sourcePrior=0;
    for(qsizetype sourceIndex=0;sourceIndex<rows.size();++sourceIndex){
        const auto& r=rows[sourceIndex];
        Bar b{r.startsAtMs,0,0,0,0,0};
        const QString values[]{r.open,r.high,r.low,r.close,r.volume};
        double* fields[]{&b.open,&b.high,&b.low,&b.close,&b.volume};
        for(int i=0;i<5;++i){bool ok=false;*fields[i]=values[i].toDouble(&ok);
            if(timeline&&!canonical.match(values[i]).hasMatch())return {};
            if(decimal::normalize(values[i]).isEmpty()||!ok||!std::isfinite(*fields[i])||*fields[i]<0||*fields[i]>1e100||(i<4&&*fields[i]<1e-100))return {};}
        if(b.at<=0||b.high<std::max(b.open,b.close)||b.low>std::min(b.open,b.close)||b.high<b.low)return {};
        if(timeline){const auto& state=timeline->rows[sourceIndex];
            if(state.startsAtMs!=b.at||state.completesAtMs<=b.at||b.at<=sourcePrior)return {};
            sourcePrior=b.at;
            if(!state.eligible){bars.clear();continue;}
            if(!state.connectsPrevious)bars.clear();
        }
        if(!bars.isEmpty()){
            if(b.at<=bars.last().at)return {};
            if(!timeline&&b.at-bars.last().at!=interval)bars.clear();
        }
        bars.append(b);
    }
    return bars;
}
void setAdx(CandleStudySet& out,const QList<Bar>& bars){
    if(bars.size()>=28){
        double tr=0,plus=0,minus=0,adx=0;int dxCount=0;
        for(qsizetype i=1;i<bars.size();++i){
            const auto& b=bars[i];const auto& p=bars[i-1];
            const double up=b.high-p.high,down=p.low-b.low;
            const double t=trueRange(b,p);
            const double positive=up>down&&up>0?up:0,negative=down>up&&down>0?down:0;
            if(i<=14){tr+=t;plus+=positive;minus+=negative;}
            else{tr=tr-tr/14+t;plus=plus-plus/14+positive;minus=minus-minus/14+negative;}
            if(i<14)continue;
            out.plusDi=tr>0?100*plus/tr:0;out.minusDi=tr>0?100*minus/tr:0;
            const double sum=out.plusDi+out.minusDi;
            const double dx=sum>0?100*std::abs(out.plusDi-out.minusDi)/sum:0;
            if(dxCount<14){adx+=dx;++dxCount;if(dxCount==14)adx/=14;}
            else adx=(13*adx+dx)/14;
        }
        out.adxAvailable=true;out.adx=adx;
        out.regime=adx>=25?QStringLiteral("방향성 강함"):adx<20?QStringLiteral("방향성 약함"):QStringLiteral("방향성 중간");
    }
}
}
static CandleStudySet marketStudiesImpl(const QList<MarketCandle>& rows, qint64 intervalMs,const ChartStudyTimeline* timeline) {
    CandleStudySet out;out.reason=QStringLiteral("연속 완료 %1분봉 21개 이상 필요").arg(intervalMs/60000);
    if(!intervals.contains(intervalMs))return out;
    const auto bars=observedRun(rows,intervalMs,timeline);out.observations=int(bars.size());
    if(bars.size()<21)return out;
    const auto& last=bars.last();out.previousHigh=bars[bars.size()-21].high;out.previousLow=bars[bars.size()-21].low;
    for(qsizetype i=bars.size()-20;i<bars.size()-1;++i){out.previousHigh=std::max(out.previousHigh,bars[i].high);out.previousLow=std::min(out.previousLow,bars[i].low);}
    out.sweptHigh=last.high>out.previousHigh&&last.close<=out.previousHigh;
    out.sweptLow=last.low<out.previousLow&&last.close>=out.previousLow;
    const auto profile=bars.mid(qMax(qsizetype(0),bars.size()-96));
    double low=profile.first().low,high=profile.first().high;
    for(const auto& b:profile){low=std::min(low,b.low);high=std::max(high,b.high);out.profileVolume+=b.volume;}
    if(out.profileVolume>0){
        std::array<double,20> bins{};const double step=(high-low)/20;
        for(const auto& b:profile){const double typical=(b.high+b.low+b.close)/3;
            const int index=step>0?std::clamp(int((typical-low)/step),0,19):0;bins[index]+=b.volume;}
        const int poc=int(std::distance(bins.begin(),std::max_element(bins.begin(),bins.end())));
        out.profileAvailable=true;out.profileBars=int(profile.size());out.profileLow=low+poc*step;out.profileHigh=out.profileLow+step;
        out.profileShare=100*bins[poc]/out.profileVolume;
    }
    double mean=0,variance=0,atr=0,priorRange=0,levelSlopeNumerator=0;
    QList<double> volumes;
    for(qsizetype i=0;i<bars.size();++i){
        const auto& b=bars[i];const double tr=i?trueRange(b,bars[i-1]):0;
        // Twenty true ranges require 21 closes, matching candleCompression's seed.
        if(i>0&&i<=20)atr+=tr/20;else if(i>20)atr=(19*atr+tr)/20;
        if(i>=bars.size()-20)mean+=b.close/20;
        if(i>=bars.size()-21&&i<bars.size()-1){volumes.append(b.volume);priorRange+=tr/20;}
    }
    for(qsizetype i=bars.size()-20;i<bars.size();++i)variance+=(bars[i].close-mean)*(bars[i].close-mean)/20;
    std::sort(volumes.begin(),volumes.end());const double median=(volumes[9]+volumes[10])/2;
    const auto& prior=bars[bars.size()-2];
    const double lastRange=trueRange(last,prior);
    out.narrowHighVolume=bars.size()>=22&&median>0&&priorRange>0&&last.volume>=2*median&&lastRange<=.5*priorRange;
    // KEEFI proximity rule: within half of Wilder ATR20, not a trade score.
    const auto nearby=[&](double level,const QString& name){if(std::abs(last.close-level)<=atr*.5){++out.overlap;out.nearbyLevels.append(name);}};
    nearby(mean,QStringLiteral("이평20"));nearby(mean-2*std::sqrt(variance),QStringLiteral("볼린저 하단"));
    if(out.profileAvailable)nearby((out.profileLow+out.profileHigh)/2,QStringLiteral("거래량 대표가격 근사"));
    double lowMean=0;for(qsizetype i=bars.size()-20;i<bars.size();++i)lowMean+=bars[i].low/20;
    for(int i=0;i<20;++i)levelSlopeNumerator+=(i-9.5)*(bars[bars.size()-20+i].low-lowMean);
    nearby(lowMean+levelSlopeNumerator/665*9.5,QStringLiteral("저가 회귀선20"));
    const int selected=intervals.indexOf(intervalMs);
    const QList<qint64> trendIntervals=!timeline&&selected+1<intervals.size()?QList<qint64>{intervalMs,intervals[selected+1]}:QList<qint64>{intervalMs};
    const auto run=rows.mid(rows.size()-bars.size());
    for(const auto interval:trendIntervals){
        QList<double> closes;qint64 end=0;
        if(timeline){for(const auto& b:bars)closes.append(b.close);end=timeline->rows.last().completesAtMs;}
        else{const auto groups=aggregateCompletedCandles(run,intervalMs,interval,bars.last().at+intervalMs);
            for(const auto& c:groups.candles){closes.append(c.close.toDouble());end=c.startsAtMs+interval;}}
        CandleTrend trend;trend.minutes=int(interval/60000);trend.bars=int(closes.size());trend.endsAtMs=end;
        if(closes.size()>=21){
            double previous=0;for(qsizetype i=closes.size()-21;i<closes.size()-1;++i)previous+=closes[i]/20;
            for(qsizetype i=closes.size()-20;i<closes.size();++i)trend.mean+=closes[i]/20;
            trend.available=true;trend.direction=trend.mean>previous?1:trend.mean<previous?-1:0;
        }
        out.trends.append(trend);
    }
    if(!timeline)out.sessions={{QStringLiteral("아침 07–09시"),0,0,0},{QStringLiteral("국장 09–15:30"),0,0,0},
        {QStringLiteral("밤 21–07시"),0,0,0},{QStringLiteral("그 외 시간"),0,0,0}};
    for(qsizetype rowIndex=0;rowIndex<bars.size();++rowIndex){const auto& b=bars[rowIndex];
        if(timeline){const auto kind=timeline->rows[rows.size()-bars.size()+rowIndex].session;
            QString label;
            switch(kind){case StockSessionKind::Day:label=QStringLiteral("데이마켓");break;case StockSessionKind::Pre:label=QStringLiteral("프리마켓");break;
            case StockSessionKind::Regular:label=QStringLiteral("정규장");break;case StockSessionKind::After:label=QStringLiteral("애프터마켓");break;
            case StockSessionKind::Mixed:label=QStringLiteral("통합 경계 혼합");break;case StockSessionKind::Unknown:label=QStringLiteral("세션 미확인");break;}
            auto found=std::find_if(out.sessions.begin(),out.sessions.end(),[&](const auto& s){return s.label==label;});
            if(found==out.sessions.end()){out.sessions.append({label,0,0,0});found=out.sessions.end()-1;}
            ++found->bars;const auto percent=(b.close/b.open-1)*100;found->meanPercent+=percent;found->meanAbsolutePercent+=std::abs(percent);continue;
        }
        const auto time=QDateTime::fromMSecsSinceEpoch(b.at,QTimeZone(9*3600)).time();const int minute=time.hour()*60+time.minute();
        const int index=minute>=420&&minute<540?0:minute>=540&&minute<930?1:minute>=1260||minute<420?2:3;
        auto& session=out.sessions[index];++session.bars;const auto percent=(b.close/b.open-1)*100;
        session.meanPercent+=percent;session.meanAbsolutePercent+=std::abs(percent);
    }
    for(auto& session:out.sessions)if(session.bars){session.meanPercent/=session.bars;session.meanAbsolutePercent/=session.bars;}
    if(!timeline&&intervalMs>=3600000)out.sessions.clear();
    if(bars.size()>=40&&(!timeline||timeline->adjustedComparable)){
        const auto returns=[&](qsizetype start){QList<double> result;for(qsizetype i=start+1;i<start+20;++i)result.append(std::log(bars[i].close/bars[i-1].close));return result;};
        const auto query=returns(bars.size()-20);
        const auto correlation=[](const QList<double>& a,const QList<double>& b){
            double ma=0,mb=0,va=0,vb=0,cov=0;for(int i=0;i<19;++i){ma+=a[i]/19;mb+=b[i]/19;}
            for(int i=0;i<19;++i){const auto da=a[i]-ma,db=b[i]-mb;va+=da*da;vb+=db*db;cov+=da*db;}
            return va>1e-24&&vb>1e-24?std::clamp(cov/std::sqrt(va*vb),-1.0,1.0):std::numeric_limits<double>::quiet_NaN();
        };
        for(qsizetype start=0;start+20<=bars.size()-20;++start){
            const auto score=correlation(query,returns(start));if(!std::isfinite(score))continue;++out.similaritySamples;
            if(!out.similarityAvailable||score>out.similarity){out.similarityAvailable=true;out.similarity=score;out.similarFromMs=bars[start].at;
                out.similarToMs=timeline?timeline->rows[rows.size()-bars.size()+start+19].completesAtMs:bars[start+19].at+intervalMs;}
        }
    }
    setAdx(out,bars);
    out.available=true;out.reason.clear();return out;
}
CandleStudySet candleStudies(const QList<MarketCandle>& rows,qint64 intervalMs){return marketStudiesImpl(rows,intervalMs,nullptr);}
}

namespace keefi {
CandleStudySet candleStudies(const QList<MarketCandle>& rows,qint64 intervalMs,const ChartStudyTimeline& timeline){return marketStudiesImpl(rows,intervalMs,&timeline);}
CandleStudySet stockDisplayAdx(const QList<MarketCandle>& rows,qint64 intervalMs,const ChartStudyTimeline& timeline){
    CandleStudySet result;
    if(!intervals.contains(intervalMs))return result;
    const auto display=timeline.forCloseDisplay();
    const auto bars=observedRun(rows,intervalMs,&display);
    setAdx(result,bars);
    return result;
}
}
