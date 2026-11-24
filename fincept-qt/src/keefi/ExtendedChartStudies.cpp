#include "keefi/ExtendedChartStudies.h"
#include "keefi/StockChartTypes.h"
#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
using Dir=IndicatorDirection;
constexpr qint64 Minute=60000,Hour=3600000,Day=86400000;
ExtendedStudyState ready(Dir d=Dir::Info){return {true,d,{}};}
ExtendedStudyState missing(const QString& reason){return {false,Dir::None,reason};}
Dir sign(double v){return v>0?Dir::Up:v<0?Dir::Down:Dir::Info;}

// Exact non-negative decimals for discrete memberships, thresholds and totals.
// The display doubles below never enter valuation or account calculations.
struct Exact { QString digits="0";qsizetype scale=0; };
Exact compact(QString s,qsizetype scale){qsizetype first=0;while(first+1<s.size()&&s[first]=='0')++first;if(first)s.remove(0,first);if(s=="0")return {};while(scale>0&&s.endsWith('0')){s.chop(1);--scale;}return {s,scale};}
Exact exact(QString s){const auto p=s.indexOf('.');const auto scale=p<0?0:s.size()-p-1;if(p>=0)s.remove(p,1);return compact(s,scale);}
QString aligned(const Exact& n,qsizetype scale){return n.digits=="0"?QString("0"):n.digits+QString(scale-n.scale,'0');}
int integerCompare(const QString& a,const QString& b){if(a.size()!=b.size())return a.size()<b.size()?-1:1;const int c=QString::compare(a,b);return c<0?-1:c>0?1:0;}
int cmp(const Exact& a,const Exact& b){const auto scale=std::max(a.scale,b.scale);return integerCompare(aligned(a,scale),aligned(b,scale));}
Exact add(const Exact& a,const Exact& b){const auto scale=std::max(a.scale,b.scale);auto l=aligned(a,scale),r=aligned(b,scale);const auto n=std::max(l.size(),r.size());l=l.rightJustified(n,'0');r=r.rightJustified(n,'0');QString out(n,'0');int carry=0;for(qsizetype i=n-1;i>=0;--i){const int v=l[i].digitValue()+r[i].digitValue()+carry;out[i]=QChar('0'+v%10);carry=v/10;}if(carry)out.prepend('1');return compact(out,scale);}
Exact sub(const Exact& a,const Exact& b){Q_ASSERT(cmp(a,b)>=0);const auto scale=std::max(a.scale,b.scale);auto l=aligned(a,scale),r=aligned(b,scale);r=r.rightJustified(l.size(),'0');int borrow=0;for(qsizetype i=l.size()-1;i>=0;--i){int v=l[i].digitValue()-r[i].digitValue()-borrow;borrow=v<0;if(borrow)v+=10;l[i]=QChar('0'+v);}return compact(l,scale);}
Exact times(const Exact& a,int factor){if(!factor||a.digits=="0")return {};auto s=a.digits;int carry=0;for(qsizetype i=s.size()-1;i>=0;--i){const int n=s[i].digitValue()*factor+carry;s[i]=QChar('0'+n%10);carry=n/10;}while(carry){s.prepend(QChar('0'+carry%10));carry/=10;}return compact(s,a.scale);}
Exact product(const Exact& a,const Exact& b){if(a.digits=="0"||b.digits=="0")return {};constexpr quint64 Base=1000000000;const auto limbs=[](const QString& s){QList<quint64> x;for(qsizetype end=s.size();end>0;end-=9)x.append(s.mid(std::max<qsizetype>(0,end-9),std::min<qsizetype>(9,end)).toULongLong());return x;};const auto l=limbs(a.digits),r=limbs(b.digits);QList<quint64> x(l.size()+r.size(),0);for(qsizetype i=0;i<l.size();++i){quint64 carry=0;for(qsizetype j=0;j<r.size();++j){const quint64 v=x[i+j]+l[i]*r[j]+carry;x[i+j]=v%Base;carry=v/Base;}x[i+r.size()]+=carry;}while(x.size()>1&&x.last()==0)x.removeLast();QString s=QString::number(x.last());for(qsizetype i=x.size()-2;i>=0;--i)s+=QString::number(x[i]).rightJustified(9,'0');return compact(s,a.scale+b.scale);}
QString text(const Exact& n){auto s=n.digits;if(n.scale>0){s=s.rightJustified(n.scale+1,'0');s.insert(s.size()-n.scale,'.');}return s;}
QString signedDifference(const Exact& a,const Exact& b){const int c=cmp(a,b);return (c<0?QString("-"):QString())+text(c<0?sub(b,a):sub(a,b));}
bool number(const QString& s,bool price,double& v,Exact* e=nullptr){static const QRegularExpression re(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));if(s.size()>4096||!re.match(s).hasMatch())return false;const auto n=exact(s);static const Exact maximum{QString("1")+QString(100,'0'),0},minimum{"1",100};if(cmp(n,maximum)>0||(price&&cmp(n,minimum)<0))return false;bool ok=false;v=s.toDouble(&ok);if((!ok&&(price||v!=0))||!std::isfinite(v)||v<0)return false;if(e)*e=n;return true;}

struct Row {MarketCandle raw;qint64 end=0;double o=0,h=0,l=0,c=0,v=0;Exact exHigh,exLow,exClose,exVolume;bool eligible=true,connect=false;};
struct Parsed {bool valid=false;QList<Row> rows;int runStart=0;QString reason;};
bool supported(qint64 interval){return interval==Minute||interval==5*Minute||interval==15*Minute||interval==Hour||interval==4*Hour||interval==Day;}
qint64 horizon(qint64 interval){return interval<=5*Minute?Hour:interval==15*Minute?4*Hour:Day;}
Parsed parse(const QList<MarketCandle>& input,qint64 interval,const ChartStudyTimeline* timeline,int maximumRows=200){Parsed p;p.reason=QStringLiteral("완료 봉 입력 형식 또는 범위가 올바르지 않아요");if(!supported(interval)||input.isEmpty()||input.size()>maximumRows||(timeline&&timeline->rows.size()!=input.size()))return p;qint64 previous=0;for(int i=0;i<input.size();++i){Row r;r.raw=input[i];Exact open;if(r.raw.startsAtMs<=0||r.raw.startsAtMs<=previous||(!timeline&&r.raw.startsAtMs%interval!=0)||r.raw.startsAtMs>std::numeric_limits<qint64>::max()-std::max(20*interval,interval+horizon(interval))||!number(r.raw.open,true,r.o,&open)||!number(r.raw.high,true,r.h,&r.exHigh)||!number(r.raw.low,true,r.l,&r.exLow)||!number(r.raw.close,true,r.c,&r.exClose)||!number(r.raw.volume,false,r.v,&r.exVolume)||cmp(r.exLow,open)>0||cmp(r.exLow,r.exClose)>0||cmp(r.exHigh,open)<0||cmp(r.exHigh,r.exClose)<0||cmp(r.exLow,r.exHigh)>0)return p;r.end=r.raw.startsAtMs+interval;r.connect=i>0&&r.raw.startsAtMs-previous==interval;if(timeline){const auto& t=timeline->rows[i];if(t.startsAtMs!=r.raw.startsAtMs||t.completesAtMs<=t.startsAtMs||t.completesAtMs>std::numeric_limits<qint64>::max()-horizon(interval)||(t.missingSlotsBefore&&(*t.missingSlotsBefore<0||(t.connectsPrevious&&*t.missingSlotsBefore>0))))return p;r.end=t.completesAtMs;r.eligible=t.eligible;r.connect=i>0&&t.connectsPrevious&&timeline->rows[i-1].eligible;}if(!r.eligible)p.runStart=i+1;else if(i==0||!r.connect)p.runStart=i;p.rows.append(r);previous=r.raw.startsAtMs;}p.valid=true;p.reason.clear();return p;}
QList<double> closes(const QList<Row>& run){QList<double> result;for(const auto& r:run)result.append(r.c);return result;}
double mean(const QList<double>& values,int start,int count){const double reference=values[start];double sum=0;for(int i=start;i<start+count;++i)sum+=values[i]-reference;return reference+sum/count;}
QList<std::optional<double>> average(const QList<double>& values,int period,ExtendedMaKind kind){QList<std::optional<double>> out(values.size());double previous=0;for(int i=period-1;i<values.size();++i){if(kind==ExtendedMaKind::Exponential){if(i==period-1)previous=mean(values,0,period);else previous+=(values[i]-previous)*2/(period+1);out[i]=previous;}else if(kind==ExtendedMaKind::Simple)out[i]=mean(values,i-period+1,period);else {const auto reference=values[i-period+1];double total=0;for(int j=0;j<period;++j)total+=(j+1)*(values[i-period+1+j]-reference);out[i]=reference+total/(period*(period+1)/2.0);}}return out;}
QList<ChartPricePoint> points(const QList<Row>& run,const QList<std::optional<double>>& values){QList<ChartPricePoint> out;for(int i=0;i<values.size();++i)if(values[i])out.append({run[i].raw.startsAtMs,*values[i]});return out;}
ExtendedTimedDirection trendImpl(const Parsed& p,qint64 interval,int period){ExtendedTimedDirection out;out.intervalMs=interval;out.state=missing(QStringLiteral("완료 봉 %1개가 필요해요").arg(period+1));if(!p.valid){out.state=missing(p.reason);return out;}if(period<2||period>120){out.state=missing(QStringLiteral("이동평균 기간이 올바르지 않아요"));return out;}const auto run=p.rows.mid(p.runStart);if(run.size()<period+1)return out;const auto values=closes(run);out.completesAtMs=run.last().end;out.knownAtMs=out.completesAtMs;out.state=ready(sign(mean(values,values.size()-period,period)-mean(values,values.size()-period-1,period)));return out;}

ExtendedLowVolumeZones zonesFromBins(const std::array<Exact,20>& bins,const Exact& total,double low,double high,int observations,bool approximation){ExtendedLowVolumeZones out;out.candleApproximation=approximation;out.observations=observations;if(total.digits=="0"||!(high>low)){out.state=missing(QStringLiteral("가격 범위와 관측 거래량이 필요해요"));return out;}out.state=ready();const double step=(high-low)/20;int start=-1;for(int i=1;i<=19;++i){const bool cold=i<19&&cmp(times(bins[i],80),total)<0;if(cold&&start<0)start=i;if(!cold&&start>=0){if(i-start>=2)out.zones.append({low+start*step,low+i*step});start=-1;}}return out;}
ExtendedLowVolumeZones candleZones(const QList<Row>& all){ExtendedLowVolumeZones out;out.state=missing(QStringLiteral("완료 1분봉 21개가 필요해요"));if(all.size()<21)return out;const auto run=all.mid(std::max<qsizetype>(0,all.size()-96));int lo=0,hi=0;Exact total;for(int i=0;i<run.size();++i){if(cmp(run[i].exLow,run[lo].exLow)<0)lo=i;if(cmp(run[i].exHigh,run[hi].exHigh)>0)hi=i;total=add(total,run[i].exVolume);}std::array<Exact,20> bins;if(cmp(run[lo].exLow,run[hi].exHigh)==0)return zonesFromBins(bins,total,run[lo].l,run[hi].h,run.size(),true);std::array<Exact,20> boundary;for(int b=1;b<20;++b)boundary[b]=times(add(times(run[lo].exLow,20-b),times(run[hi].exHigh,b)),3);for(const auto& row:run){const auto numerator=times(add(add(row.exHigh,row.exLow),row.exClose),20);int bin=0;while(bin<19&&cmp(numerator,boundary[bin+1])>=0)++bin;bins[bin]=add(bins[bin],row.exVolume);}return zonesFromBins(bins,total,run[lo].l,run[hi].h,run.size(),true);}
qint64 higherInterval(qint64 x){return x==Minute?5*Minute:x==5*Minute?15*Minute:x==15*Minute?Hour:x==Hour?4*Hour:x==4*Hour?Day:7*Day;}
ExtendedBreakout breakoutAt(const QList<Row>& run,qint64 interval,const QList<ExtendedTimedDirection>& higher){ExtendedBreakout out;out.state=missing(QStringLiteral("완료 봉 21개가 필요해요"));if(run.size()<21)return out;const auto& last=run.last();int hi=run.size()-21,lo=hi;QList<double> volumes;for(int i=run.size()-21;i<run.size()-1;++i){if(cmp(run[i].exHigh,run[hi].exHigh)>0)hi=i;if(cmp(run[i].exLow,run[lo].exLow)<0)lo=i;volumes.append(run[i].v);}out.previousHigh=run[hi].h;out.previousLow=run[lo].l;const Dir direction=cmp(last.exClose,run[hi].exHigh)>0?Dir::Up:cmp(last.exClose,run[lo].exLow)<0?Dir::Down:Dir::Info;out.state=ready(direction);if(direction==Dir::Info)return out;out.qualityReason=QStringLiteral("거래량·압축·상위 시간봉 자료가 더 필요해요");std::sort(volumes.begin(),volumes.end());const double med=(volumes[9]+volumes[10])/2;if(med<=0||last.h<=last.l||run.size()<22)return out;QList<MarketCandle> raw;for(const auto& r:run)raw.append(r.raw);ChartStudyTimeline proof;for(int i=0;i<run.size();++i){ChartStudyTimelineRow t;t.startsAtMs=run[i].raw.startsAtMs;t.completesAtMs=run[i].end;t.eligible=true;t.connectsPrevious=i>0;proof.rows.append(t);}auto priorProof=proof;priorProof.rows.removeLast();const auto current=candleCompression(raw,interval,proof),previous=candleCompression(raw.mid(0,raw.size()-1),interval,priorProof);if(!current.available||!previous.available)return out;const ExtendedTimedDirection* selected=nullptr;for(const auto& h:higher)if(h.intervalMs==higherInterval(interval)&&h.state.available&&h.completesAtMs>0&&h.completesAtMs<=last.end&&h.knownAtMs>0&&h.knownAtMs<=last.end&&(!selected||h.completesAtMs>selected->completesAtMs))selected=&h;if(!selected)return out;const double ratio=last.v/med,location=(last.c-last.l)/(last.h-last.l);int quality=2+(ratio>=2?3:ratio>=1.5?2:ratio>=1?1:0);quality+=2*int(direction==Dir::Up?location>=.75:location<=.25);quality+=int(previous.compressed&&!current.compressed);quality+=2*int(selected->state.direction==direction);out.quality=quality;out.qualityReason.clear();return out;}
ExtendedScorecard scorecard(const Parsed& p,const QList<ExtendedSignal>& entries,qint64 interval,bool comparable,bool qualitySlices=true){ExtendedScorecard out;out.horizonMs=horizon(interval);qint64 previousEnd=0;for(const auto& event:entries){const auto target=event.atMs+out.horizonMs;if(event.atMs<previousEnd)++out.overlappingSignals;previousEnd=std::max(previousEnd,target);int signalIndex=-1,targetIndex=-1;for(int i=0;i<p.rows.size();++i)if(p.rows[i].end==event.atMs){signalIndex=i;break;}if(comparable&&signalIndex>=0)for(int i=signalIndex+1;i<p.rows.size();++i){if(!p.rows[i].eligible||!p.rows[i].connect)break;if(p.rows[i].end==target){targetIndex=i;break;}if(p.rows[i].end>target)break;}if(targetIndex<0){++out.unresolvedSignals;continue;}const double change=100*(p.rows[targetIndex].c/event.entryClose-1);out.outcomes.append({event.atMs,target,event.direction,event.quality,change,event.direction==Dir::Down?-change:change});++out.samples;}
    const QList<int> bands=qualitySlices?QList<int>{-1,-2,0,1,2}:QList<int>{-1};
    for(const auto direction:{Dir::Up,Dir::Down})for(const int band:bands){ExtendedScorecardSlice slice;slice.direction=direction;slice.qualityBand=band;double total=0,directionalTotal=0;for(const auto& outcome:out.outcomes){const int actualBand=!outcome.quality?-2:*outcome.quality<=4?0:*outcome.quality<=7?1:2;if(outcome.direction!=direction||(band!=-1&&actualBand!=band))continue;++slice.samples;total+=outcome.returnPercent;directionalTotal+=outcome.directionalReturnPercent;if(outcome.returnPercent>0)++slice.up;else if(outcome.returnPercent<0)++slice.down;else ++slice.flat;}slice.available=slice.samples>=ChartScorecardDisplayMinimumSamples;if(slice.available){slice.meanReturnPercent=total/slice.samples;slice.meanDirectionalReturnPercent=directionalTotal/slice.samples;}if(slice.samples)out.slices.append(slice);if(band==-1&&slice.available)out.available=true;}
    if(out.available){const ExtendedScorecardSlice* only=nullptr;bool multiple=false;for(const auto& slice:out.slices)if(slice.qualityBand==-1&&slice.samples){if(only)multiple=true;else only=&slice;}if(only&&!multiple&&only->available)out.meanReturnPercent=only->meanReturnPercent;}else out.reason=QStringLiteral("기록 부족 · 방향별 평가 %1개 이상 필요").arg(ChartScorecardDisplayMinimumSamples);return out;}

QMap<QString,QList<ExtendedSignal>> basicEntries(const Parsed& parsed,const ExtendedChartOptions& options){
    QMap<QString,QList<ExtendedSignal>> events;
    for(const auto& id:{QString("ma"),QString("bb"),QString("rsi"),QString("macd")})
        if(options.enabled.contains(id)&&(id!="ma"||options.eventStudies.contains(id)))events.insert(id,{});
    for(int begin=0;begin<parsed.rows.size();){
        if(!parsed.rows[begin].eligible){++begin;continue;}
        int end=begin+1;
        while(end<parsed.rows.size()&&parsed.rows[end].eligible&&parsed.rows[end].connect)++end;
        const auto run=parsed.rows.mid(begin,end-begin);const auto values=closes(run);
        QMap<QString,QList<Dir>> states;
        for(auto it=events.cbegin();it!=events.cend();++it)states.insert(it.key(),QList<Dir>(run.size(),Dir::None));
        if(states.contains("ma")){
            QList<QList<std::optional<double>>> lines;
            for(const auto& spec:options.movingAverages)if(spec.enabled)lines.append(average(values,spec.period,spec.kind));
            for(int i=0;i<run.size();++i){QList<std::optional<double>> configured;for(const auto& line:lines)configured.append(line[i]);
                states["ma"][i]=extendedMovingAverageDirection(values[i],configured);}
        }
        if(states.contains("bb"))for(int i=options.bandPeriod-1;i<run.size();++i){
            const auto m=mean(values,i-options.bandPeriod+1,options.bandPeriod);double variance=0;
            for(int j=i-options.bandPeriod+1;j<=i;++j)variance+=(values[j]-m)*(values[j]-m);
            const auto width=options.bandDeviations*std::sqrt(variance/options.bandPeriod);
            states["bb"][i]=values[i]>m+width?Dir::Up:values[i]<m-width?Dir::Down:Dir::Info;
        }
        if(states.contains("rsi")){
            double gain=0,loss=0;
            for(int i=1;i<run.size();++i){
                const auto delta=values[i]-values[i-1],g=std::max(delta,0.0),l=std::max(-delta,0.0);
                if(i<=options.rsiPeriod){gain+=g;loss+=l;if(i<options.rsiPeriod)continue;gain/=options.rsiPeriod;loss/=options.rsiPeriod;}
                else{gain=((options.rsiPeriod-1)*gain+g)/options.rsiPeriod;loss=((options.rsiPeriod-1)*loss+l)/options.rsiPeriod;}
                const auto value=gain==0&&loss==0?50:loss==0?100:100-100/(1+gain/loss);
                states["rsi"][i]=value>=55?Dir::Up:value<=45?Dir::Down:Dir::Info;
            }
        }
        if(states.contains("macd")){
            const auto fast=average(values,options.macdFast,ExtendedMaKind::Exponential),slow=average(values,options.macdSlow,ExtendedMaKind::Exponential);
            QList<double> macd;QList<int> indices;
            for(int i=0;i<run.size();++i)if(fast[i]&&slow[i]){macd.append(*fast[i]-*slow[i]);indices.append(i);}
            const auto signal=average(macd,options.macdSignal,ExtendedMaKind::Exponential);
            for(int i=0;i<macd.size();++i)if(signal[i])states["macd"][indices[i]]=sign(macd[i]-*signal[i]);
        }
        for(auto it=states.cbegin();it!=states.cend();++it){
            Dir previous=Dir::None;
            for(int i=0;i<run.size();++i){const auto direction=it.value()[i];
                if((direction==Dir::Up||direction==Dir::Down)&&direction!=previous)
                    events[it.key()].append({it.key(),run[i].end,run[i].c,direction,{}});
                previous=direction;
            }
        }
        begin=end;
    }
    return events;
}

ExtendedChartStudySet calculate(const QList<MarketCandle>& input,qint64 interval,const ExtendedChartOptions& o,const ChartStudyTimeline* timeline,bool displayOnly=false){ExtendedChartStudySet out;out.intervalMs=interval;const auto p=parse(input,interval,timeline,displayOnly?MarketCandles::HistoryLimit:200);if(!p.valid){out.reason=p.reason;if(o.enabled.contains("rsi"))out.rsi.state=missing(p.reason);if(o.enabled.contains("macd"))out.macd.state=missing(p.reason);if(displayOnly){out.movingAverageState=missing(p.reason);out.bb.state=missing(p.reason);}return out;}const auto periodValid=[](int x){return x>=2&&x<=120;};QSet<QString> ids;if(o.enabled.contains("ma")){if(o.movingAverages.isEmpty()||o.movingAverages.size()>ChartMovingAverageMaximumLines){out.reason=QStringLiteral("이동평균은 1~4개를 설정해요");return out;}for(const auto& m:o.movingAverages){if((m.period<2||m.period>ChartMovingAverageMaximumPeriod)||m.id.isEmpty()||ids.contains(m.id)||int(m.kind)<0||int(m.kind)>2){out.reason=QStringLiteral("이동평균 설정이 올바르지 않아요");return out;}ids.insert(m.id);}}if((o.enabled.contains("rsi")&&!periodValid(o.rsiPeriod))||(o.enabled.contains("bb")&&((o.bandPeriod<2||o.bandPeriod>(displayOnly?ChartDisplayBandPeriod:120))||!std::isfinite(o.bandDeviations)||o.bandDeviations<=0||o.bandDeviations>10))||(o.enabled.contains("macd")&&(!periodValid(o.macdFast)||!periodValid(o.macdSlow)||o.macdSlow<=o.macdFast||!periodValid(o.macdSignal)||o.macdSlow+o.macdSignal>201))){out.reason=QStringLiteral("지표 기간 설정이 올바르지 않아요");return out;}out.available=true;out.observations=p.rows.size();out.currentRunBars=p.rows.size()-p.runStart;out.lastCompletedAtMs=p.rows.last().end;const auto run=p.rows.mid(p.runStart);const auto values=closes(run);const auto shortReason=QStringLiteral("연속 완료 봉 기록이 더 필요해요");
    const auto warmup=[&](int needed){return QStringLiteral("연속 완료 봉 %1/%2개 · %3개 더 필요").arg(out.currentRunBars).arg(needed).arg(qMax(0,needed-out.currentRunBars));};
    if(o.enabled.contains("ma")){QList<std::optional<double>> latest;for(const auto& spec:o.movingAverages){if(!spec.enabled)continue;const auto v=average(values,spec.period,spec.kind);ExtendedMaLine line;line.spec=spec;line.points=points(run,v);line.state=line.points.size()<(displayOnly?2:1)?missing(QStringLiteral("선을 표시하려면 연속 완료 봉 %1개가 필요해요").arg(spec.period+(displayOnly?1:0))):ready();latest.append(v.isEmpty()||(displayOnly&&!line.state.available)?std::optional<double>{}:v.last());out.maLines.append(line);}const auto direction=run.isEmpty()?Dir::None:extendedMovingAverageDirection(run.last().c,latest);out.movingAverageState=direction==Dir::None?missing(latest.isEmpty()?QStringLiteral("사용 중인 이동평균선이 없어요"):shortReason):ready(direction);}
    if(o.enabled.contains("bb")){out.bb.state=missing(displayOnly?QStringLiteral("밴드를 표시하려면 연속 완료 봉 %1개가 필요해요").arg(o.bandPeriod+1):shortReason);for(int i=o.bandPeriod-1;i<values.size();++i){const auto m=mean(values,i-o.bandPeriod+1,o.bandPeriod);double variance=0;for(int j=i-o.bandPeriod+1;j<=i;++j)variance+=(values[j]-m)*(values[j]-m);const auto width=o.bandDeviations*std::sqrt(variance/o.bandPeriod);out.bb.points.append({run[i].raw.startsAtMs,m-width,m,m+width});}if(out.bb.points.size()>=(displayOnly?2:1)){const auto& b=out.bb.points.last();out.bb.state=ready(run.last().c>b.upper?Dir::Up:run.last().c<b.lower?Dir::Down:Dir::Info);}}
    if(o.enabled.contains("rsi")){out.rsi.state=missing(warmup(o.rsiPeriod+1));double gain=0,loss=0;for(int i=1;i<values.size();++i){const auto d=values[i]-values[i-1],g=std::max(d,0.0),l=std::max(-d,0.0);if(i<=o.rsiPeriod){gain+=g;loss+=l;if(i<o.rsiPeriod)continue;gain/=o.rsiPeriod;loss/=o.rsiPeriod;}else {gain=((o.rsiPeriod-1)*gain+g)/o.rsiPeriod;loss=((o.rsiPeriod-1)*loss+l)/o.rsiPeriod;}const auto r=gain==0&&loss==0?50:loss==0?100:100-100/(1+gain/loss);out.rsi.points.append({run[i].raw.startsAtMs,r});}if(!out.rsi.points.isEmpty()){const auto v=out.rsi.points.last().price;out.rsi.state=ready(v>=55?Dir::Up:v<=45?Dir::Down:Dir::Info);}}
    if(o.enabled.contains("macd")){out.macd.state=missing(warmup(o.macdSlow+o.macdSignal-1));const auto fast=average(values,o.macdFast,ExtendedMaKind::Exponential),slow=average(values,o.macdSlow,ExtendedMaKind::Exponential);QList<double> line;QList<Row> lineRows;for(int i=0;i<values.size();++i)if(fast[i]&&slow[i]){const auto v=*fast[i]-*slow[i];line.append(v);lineRows.append(run[i]);out.macd.line.append({run[i].raw.startsAtMs,v});}const auto signal=average(line,o.macdSignal,ExtendedMaKind::Exponential);out.macd.signal=points(lineRows,signal);for(int i=0;i<line.size();++i)if(signal[i])out.macd.histogram.append({lineRows[i].raw.startsAtMs,line[i]-*signal[i]});if(!out.macd.histogram.isEmpty())out.macd.state=ready(sign(out.macd.histogram.last().price));}
    if(displayOnly)return out;
    if(o.enabled.contains("low_volume")){out.lowVolumeZones=interval==Minute?candleZones(run):ExtendedLowVolumeZones{};if(interval!=Minute)out.lowVolumeZones.state=missing(QStringLiteral("거래 드문 가격대에는 완료 1분봉이 필요해요"));}
    if(o.enabled.contains("trend"))out.trend=trendImpl(p,interval,20);
    if(o.enabled.contains("breakout")){int begin=0;Dir previous=Dir::None;QList<ExtendedSignal> entries;for(int i=0;i<p.rows.size();++i){if(!p.rows[i].eligible){begin=i+1;previous=Dir::None;continue;}if(i==0||!p.rows[i].connect){begin=i;previous=Dir::None;}const auto b=breakoutAt(p.rows.mid(begin,i-begin+1),interval,o.higherTrends);if(b.state.available&&(b.state.direction==Dir::Up||b.state.direction==Dir::Down)&&previous!=b.state.direction)entries.append({"breakout",p.rows[i].end,p.rows[i].c,b.state.direction,b.quality});previous=b.state.direction;}out.breakout=breakoutAt(run,interval,o.higherTrends);out.breakout.entries=entries;out.breakout.scorecard=scorecard(p,entries,interval,!timeline||timeline->adjustedComparable);}
    out.studyEntries=basicEntries(p,o);
    for(auto it=out.studyEntries.cbegin();it!=out.studyEntries.cend();++it)
        out.scorecards.insert(it.key(),scorecard(p,it.value(),interval,!timeline||timeline->adjustedComparable,false));
    if(o.enabled.contains("breakout")){out.studyEntries.insert("breakout",out.breakout.entries);out.scorecards.insert("breakout",out.breakout.scorecard);}
    return out;
}
}
ExtendedChartStudySet extendedChartStudies(const QList<MarketCandle>& r,qint64 i,const ExtendedChartOptions& o){return calculate(r,i,o,nullptr);}
ExtendedChartStudySet extendedChartStudies(const QList<MarketCandle>& r,qint64 i,const ExtendedChartOptions& o,const ChartStudyTimeline& t){return calculate(r,i,o,&t);}
namespace {
ExtendedChartStudySet displayStudies(const QList<MarketCandle>& rows,qint64 interval,const ExtendedChartOptions& options,const ChartStudyTimeline* timeline){
    auto display=options;
    display.enabled.intersect(timeline ? QSet<QString>{"ma","bb","rsi","macd"} : QSet<QString>{"ma","bb"});
    display.eventStudies.clear();display.higherTrends.clear();
    display.bandPeriod=options.displayBandPeriod;display.bandDeviations=options.displayBandDeviations;
    if (!timeline) return calculate(rows,interval,display,nullptr,true);
    // Unknown historical VI/limit coverage alone does not invalidate an observed close.
    // Only this display copy relaxes that exclusion. Alerts and scorecards use the original timeline.
    const auto closeTimeline=timeline->forCloseDisplay();
    return calculate(rows,interval,display,&closeTimeline,true);
}
}
ExtendedChartStudySet extendedChartDisplayStudies(const QList<MarketCandle>& rows,qint64 interval,const ExtendedChartOptions& options){return displayStudies(rows,interval,options,nullptr);}
ExtendedChartStudySet extendedChartDisplayStudies(const QList<MarketCandle>& rows,qint64 interval,const ExtendedChartOptions& options,const ChartStudyTimeline& timeline){return displayStudies(rows,interval,options,&timeline);}
IndicatorDirection extendedMovingAverageDirection(double close,const QList<std::optional<double>>& values){if(!std::isfinite(close)||values.isEmpty())return Dir::None;bool above=false,below=false,equal=false;for(const auto& v:values){if(!v||!std::isfinite(*v))return Dir::None;above|=close>*v;below|=close<*v;equal|=close==*v;}return above&&below?Dir::Mix:equal?Dir::Info:above?Dir::Up:below?Dir::Down:Dir::Info;}
ExtendedTimedDirection extendedTrend(const QList<MarketCandle>& r,qint64 i,int p){return trendImpl(parse(r,i,nullptr),i,p);}
ExtendedTimedDirection extendedTrend(const QList<MarketCandle>& r,qint64 i,int p,const ChartStudyTimeline& t){return trendImpl(parse(r,i,&t),i,p);}
ExtendedLevelSet chartLevels(const QList<ExtendedLevelCandidate>& candidates,const QString& currentPrice,const QString& tick,qint64 interval,qint64 asOf){ExtendedLevelSet out;out.state=missing(QStringLiteral("완료 기간 가격 자료가 필요해요"));double value=0;Exact current,tickValue;if(asOf<=0||interval<=0||candidates.size()>64||!number(currentPrice,true,value,&current))return out;out.approximateTick=tick.isEmpty();if(!tick.isEmpty()&&!number(tick,true,value,&tickValue)){out.state=missing(QStringLiteral("호가 단위를 확인할 수 없어요"));return out;}auto tolerance=product(current,exact("0.0005"));if(cmp(tickValue,tolerance)>0)tolerance=tickValue;QList<ExtendedLevelCandidate> eligible;QSet<QString> seen;for(const auto& c:candidates){if(!c.complete||c.fromMs<=0||c.throughMs<=c.fromMs||c.throughMs>asOf||c.knownAtMs<=0||c.knownAtMs>asOf||(interval>=Day&&c.period==ExtendedLevelPeriod::Day))continue;if(c.id.isEmpty()||seen.contains(c.id)||!number(c.price,true,value)){out.state=missing(QStringLiteral("기간 가격 입력이 올바르지 않아요"));return out;}seen.insert(c.id);eligible.append(c);}std::stable_sort(eligible.begin(),eligible.end(),[](const auto& a,const auto& b){const int c=cmp(exact(a.price),exact(b.price));return c?c<0:a.id<b.id;});for(const auto& c:eligible){if(!out.all.isEmpty()&&cmp(sub(exact(c.price),exact(out.all.last().price)),tolerance)<=0){auto& l=out.all.last();l.ids.append(c.id);l.labels.append(c.label);l.members.append(c);}else out.all.append({c.price,{c.id},{c.label},{c}});}if(out.all.isEmpty())return out;out.state=ready();int below=0,above=0;for(qsizetype i=out.all.size()-1;i>=0&&below<2;--i)if(cmp(exact(out.all[i].price),current)<0){out.visible.append(out.all[i]);++below;}for(const auto& l:out.all)if(cmp(exact(l.price),current)>0&&above<2){out.visible.append(l);++above;}return out;}

// Remaining public flow extensions are implemented below; independent unavailable
// states do not stop the completed-candle studies above.
namespace {
struct ObservedTrade {const CoinTradeObservation* raw=nullptr;Exact price,quantity,notional;double priceValue=0;};
struct ObservedWindow {bool available=false;QString reason;qint64 from=0,through=0,knownAt=0;quint64 epoch=0;QList<ObservedTrade> trades;};
bool continuous(CoinFlowCoverage c){return c==CoinFlowCoverage::ObservedContinuous||c==CoinFlowCoverage::RecoveredOverlap;}
ObservedWindow observedWindow(const CoinFlowWindow& w,qint64 asOf,int minimumMinutes,int maximumMinutes){
    ObservedWindow out;out.reason=QStringLiteral("연속 관측한 완료 %1분 체결이 필요해요").arg(minimumMinutes);
    const auto& instrument=w.instrument;
    if(asOf<=0||asOf>std::numeric_limits<qint64>::max()-Day||w.trades.size()>10000||w.periods.size()>1000||!instrument.verified||instrument.identity.isEmpty()||instrument.symbol.isEmpty()||instrument.baseAsset.isEmpty())return out;
    if(instrument.venue==CoinFlowVenue::UpbitSpot){if(instrument.grain!=CoinFlowGrain::UpbitTrade||instrument.quoteAsset!="KRW")return out;}
    else if(instrument.venue==CoinFlowVenue::BinanceSpot){if(instrument.grain!=CoinFlowGrain::SpotAggregate||(instrument.quoteAsset!="USDT"&&instrument.quoteAsset!="USD"))return out;}
    else if(instrument.venue==CoinFlowVenue::BinanceUsdM){if(instrument.grain!=CoinFlowGrain::Futures100msAggregate||instrument.quoteAsset!="USDT"||instrument.marginAsset!="USDT"||instrument.contractType!="PERPETUAL"||instrument.status!="TRADING"||instrument.underlyingType!="COIN")return out;}
    else return out;
    out.through=asOf/Minute*Minute;
    if(out.through<minimumMinutes*Minute)return out;
    for(const auto& p:w.periods)if(p.knownAtMs>0&&p.knownAtMs<=asOf&&p.fromMs<out.through&&p.throughMs>=out.through&&continuous(p.coverage)&&p.epoch){if(out.epoch&&out.epoch!=p.epoch){out.reason=QStringLiteral("관측 구간 세대가 겹쳐요");return out;}out.epoch=p.epoch;}
    if(!out.epoch)return out;
    QList<CoinFlowCoveragePeriod> periods;
    for(const auto& p:w.periods)if(p.epoch==out.epoch&&p.knownAtMs>0&&p.knownAtMs<=asOf&&p.fromMs>0&&p.throughMs>p.fromMs&&p.throughMs<=out.through&&continuous(p.coverage))periods.append(p);
    std::sort(periods.begin(),periods.end(),[](const auto& a,const auto& b){return a.fromMs==b.fromMs?a.throughMs<b.throughMs:a.fromMs<b.fromMs;});
    qint64 cursor=out.through;
    for(qsizetype i=periods.size()-1;i>=0;--i)if(periods[i].throughMs>=cursor&&periods[i].fromMs<cursor){cursor=periods[i].fromMs;out.knownAt=std::max(out.knownAt,periods[i].knownAtMs);}
    out.from=std::max(cursor,out.through-maximumMinutes*Minute);
    out.from=((out.from+Minute-1)/Minute)*Minute;
    if(out.through-out.from<minimumMinutes*Minute)return out;
    for(const auto& p:w.periods)if(p.knownAtMs>0&&p.knownAtMs<=asOf&&p.fromMs<out.through&&p.throughMs>out.from&&(!continuous(p.coverage)||p.epoch!=out.epoch)){out.reason=QStringLiteral("체결 관측 구간에 끊김이 있어요");return out;}
    QHash<QString,const CoinTradeObservation*> ids;
    for(const auto& t:w.trades){
        if(t.epoch!=out.epoch||t.knownAtMs>asOf||t.tradeAtMs>=out.through||t.tradeAtMs<out.from)continue;
        double price=0,quantity=0;ObservedTrade item;item.raw=&t;
        if(t.id.isEmpty()||t.id.size()>256||t.receivedAtMs<=0||t.knownAtMs<t.receivedAtMs||t.tradeAtMs<0||t.tradeAtMs>t.knownAtMs||t.eventAtMs<0||t.eventAtMs>t.knownAtMs||!number(t.price,true,price,&item.price)||!number(t.quantity,true,quantity,&item.quantity)||(t.side!=CoinFlowSide::Buy&&t.side!=CoinFlowSide::Sell)){out.reason=QStringLiteral("체결 입력 형식 또는 시각이 올바르지 않아요");return out;}
        if(ids.contains(t.id)){const auto* prior=ids.value(t.id);if(prior->price!=t.price||prior->quantity!=t.quantity||prior->normalQuantity!=t.normalQuantity||prior->side!=t.side||prior->tradeAtMs!=t.tradeAtMs){out.reason=QStringLiteral("같은 체결 식별자의 내용이 달라요");return out;}continue;}
        ids.insert(t.id,&t);item.priceValue=price;item.notional=product(item.price,item.quantity);out.trades.append(item);out.knownAt=std::max(out.knownAt,t.knownAtMs);
    }
    std::sort(out.trades.begin(),out.trades.end(),[](const auto& a,const auto& b){return a.raw->tradeAtMs==b.raw->tradeAtMs?a.raw->id<b.raw->id:a.raw->tradeAtMs<b.raw->tradeAtMs;});
    out.available=true;out.reason.clear();return out;
}
std::optional<double> fraction(const Exact& buy,const Exact& sell){const auto total=add(buy,sell);if(total.digits=="0")return {};bool ok=false;const double sum=text(total).toDouble(&ok);if(!ok||!std::isfinite(sum)||sum<=0)return {};const double value=signedDifference(buy,sell).toDouble(&ok);if(!ok||!std::isfinite(value))return {};return value/sum;}
bool readTotal(const QString& source,Exact& out){static const QRegularExpression re(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));if(source.size()>4096||!re.match(source).hasMatch())return false;bool ok=false;const double v=source.toDouble(&ok);if((!ok&&v!=0)||!std::isfinite(v)||v<0)return false;out=exact(source);return true;}
}
ExtendedAmountDelta extendedAmountDelta(const CoinFlowWindow& window,qint64 asOf){
    ExtendedAmountDelta out;out.currency=window.instrument.quoteAsset;out.sourceDirectionOnly=window.instrument.venue==CoinFlowVenue::UpbitSpot;
    const auto observed=observedWindow(window,asOf,5,200);out.state=missing(observed.reason);if(!observed.available)return out;
    out.fromMs=observed.from;out.throughMs=observed.through;out.knownAtMs=observed.knownAt;out.epoch=observed.epoch;
    const Exact cut1=exact(out.currency=="KRW"?"10000000":"10000"),cut2=exact(out.currency=="KRW"?"100000000":"100000");
    for(const auto* id:{"small","medium","large"}){ExtendedDeltaBucket b;b.id=id;b.state=missing(QStringLiteral("이 금액 구간에 관측한 체결이 없어요"));out.buckets.append(b);}
    std::array<Exact,3> buy,sell,cumulativeBuy,cumulativeSell;
    int tradeIndex=0;
    for(qint64 end=observed.from+Minute;end<=observed.through;end+=Minute){
        while(tradeIndex<observed.trades.size()&&observed.trades[tradeIndex].raw->tradeAtMs<end){const auto& t=observed.trades[tradeIndex++];const int bin=cmp(t.notional,cut1)<0?0:cmp(t.notional,cut2)<0?1:2;auto& cumulative=t.raw->side==CoinFlowSide::Buy?cumulativeBuy[bin]:cumulativeSell[bin];cumulative=add(cumulative,t.notional);if(t.raw->tradeAtMs>=observed.through-5*Minute){auto& value=t.raw->side==CoinFlowSide::Buy?buy[bin]:sell[bin];value=add(value,t.notional);}}
        for(int i=0;i<3;++i){bool ok=false;const auto value=signedDifference(cumulativeBuy[i],cumulativeSell[i]).toDouble(&ok);if(!ok||!std::isfinite(value)){out.state=missing(QStringLiteral("체결 누적 금액을 그릴 수 없어요"));out.buckets.clear();return out;}out.buckets[i].points.append({end-Minute,value});}
    }
    bool up=false,down=false,any=false;
    for(int i=0;i<3;++i){auto& b=out.buckets[i];b.buyQuote=text(buy[i]);b.sellQuote=text(sell[i]);b.signedQuote=signedDifference(buy[i],sell[i]);b.signedFraction=fraction(buy[i],sell[i]);if(b.signedFraction){any=true;b.state=ready(*b.signedFraction>=.2?Dir::Up:*b.signedFraction<=-.2?Dir::Down:Dir::Info);up|=b.state.direction==Dir::Up;down|=b.state.direction==Dir::Down;}}
    out.state=any?ready(up&&down?Dir::Mix:up?Dir::Up:down?Dir::Down:Dir::Info):missing(QStringLiteral("최근 완료 5분에 관측한 체결이 없어요"));return out;
}
ExtendedCompressionGauge extendedCompressionGauge(const CandleCompression& c,const CoinOiStudy& oi){ExtendedCompressionGauge r;if(c.available)r.compressed=c.compressed;if(oi.state.available&&oi.quantityChangePercent&&std::isfinite(*oi.quantityChangePercent))r.oiChangePercent=oi.quantityChangePercent;r.state=c.available&&r.oiChangePercent?ready():missing(QStringLiteral("압축·계약 수량 일부 자료가 없어요"));return r;}
ExtendedBreadth extendedBreadth(const QList<ExtendedBreadthObservation>& observations,double minimum){ExtendedBreadth r;r.state=missing(QStringLiteral("같은 기준의 관측 표본이 더 필요해요"));if(!std::isfinite(minimum)||minimum<=0||minimum>1||observations.size()>10000)return r;QSet<QString> ids;for(const auto& o:observations){if(o.identity.isEmpty()||ids.contains(o.identity)){r.state=missing(QStringLiteral("표본 종목이 중복되거나 식별되지 않아요"));return r;}ids.insert(o.identity);++r.population;if(!o.fresh||!o.changePercent||!std::isfinite(*o.changePercent))continue;++r.ready;if(*o.changePercent>0)++r.up;else if(*o.changePercent<0)++r.down;else ++r.flat;}if(r.population&&r.ready&&double(r.ready)/r.population>=minimum){r.percent=100.0*r.up/r.ready;r.state=ready(sign(*r.percent-50));}return r;}
ExtendedRelativeStrength extendedRelativeStrength(const QList<MarketCandle>& alt,const QList<MarketCandle>& benchmark,qint64 interval,int lookback){ExtendedRelativeStrength out;out.state=missing(QStringLiteral("같은 시각의 비교 기록이 필요해요"));const auto a=parse(alt,interval,nullptr),b=parse(benchmark,interval,nullptr);if(!a.valid||!b.valid||lookback<1||lookback>199)return out;const auto ar=a.rows.mid(a.runStart),br=b.rows.mid(b.runStart);if(ar.size()<=lookback||br.size()<=lookback)return out;const int ai=ar.size()-1,bi=br.size()-1;for(int j=0;j<=lookback;++j)if(ar[ai-j].raw.startsAtMs!=br[bi-j].raw.startsAtMs)return out;const double start=ar[ai-lookback].c/br[bi-lookback].c,end=ar[ai].c/br[bi].c;const double change=100*(end/start-1);if(!std::isfinite(change))return out;out.percent=change;out.fromMs=ar[ai-lookback].end;out.throughMs=ar[ai].end;out.state=ready(sign(change));return out;}
ExtendedStrongReversal extendedStrongReversal(const CoinReversalStudy& reversal,const CoinVolumeStudy& volume,const CoinTakerStudy& spot,qint64 decisionAtMs){
    ExtendedStrongReversal out;
    const auto decision=decisionAtMs>0?decisionAtMs:reversal.state.knownAtMs;
    out.throughMs=reversal.state.throughMs;out.knownAtMs=reversal.state.knownAtMs;
    if(!reversal.state.available||out.throughMs<=0||out.knownAtMs<out.throughMs||out.knownAtMs>decision){
        out.state=missing(reversal.state.reason.isEmpty()?QStringLiteral("반전 결정 시각의 자료가 필요해요"):reversal.state.reason);
        out.grade=ExtendedReversalGrade::Incomplete;return out;
    }
    const Dir direction=reversal.rebound&&reversal.turnDown?Dir::Mix:reversal.rebound?Dir::Up:reversal.turnDown?Dir::Down:Dir::Info;
    out.state=ready(direction);
    if(direction==Dir::Info||direction==Dir::Mix)return out;
    out.knownAtMs=std::max({reversal.state.knownAtMs,volume.state.knownAtMs,spot.state.knownAtMs});
    if(!volume.state.available||!volume.multiple||!std::isfinite(*volume.multiple)||*volume.multiple<0||
       !spot.state.available||!spot.signedFraction||!std::isfinite(*spot.signedFraction)||std::abs(*spot.signedFraction)>1||
       spot.state.fromMs<0||spot.state.throughMs<5*Minute||spot.state.fromMs>spot.state.throughMs||
       spot.state.throughMs-spot.state.fromMs!=5*Minute||spot.state.throughMs!=out.throughMs||volume.state.throughMs!=out.throughMs||
       volume.state.knownAtMs<out.throughMs||spot.state.knownAtMs<out.throughMs||out.knownAtMs>decision){
        out.state=missing(QStringLiteral("반전 결정 시각의 보강 자료가 일부 없어요"));
        out.grade=ExtendedReversalGrade::Incomplete;return out;
    }
    const bool v=*volume.multiple>=2,aligned=direction==Dir::Up?*spot.signedFraction>=.2:*spot.signedFraction<=-.2;
    out.grade=v&&aligned?ExtendedReversalGrade::Strong:v||aligned?ExtendedReversalGrade::Reinforced:ExtendedReversalGrade::Basic;
    return out;
}
ExtendedStrongReversal extendedStrongReversal(const CoinReversalStudy& reversal,const CoinVolumeStudy& volume,const ExtendedAmountDelta& spot,qint64 decisionAtMs){
    CoinTakerStudy joined;joined.state.available=false;
    if(spot.state.available&&spot.fromMs<=spot.throughMs-5*Minute&&spot.throughMs>0&&spot.throughMs==reversal.state.throughMs&&spot.throughMs==volume.state.throughMs){
        Exact buy,sell;bool valid=spot.buckets.size()==3;
        for(const auto& b:spot.buckets){Exact a,z;if(!readTotal(b.buyQuote,a)||!readTotal(b.sellQuote,z)){valid=false;break;}buy=add(buy,a);sell=add(sell,z);}
        if(valid){joined.signedFraction=fraction(buy,sell);joined.state.available=joined.signedFraction.has_value();
            joined.state.fromMs=spot.throughMs-5*Minute;joined.state.throughMs=spot.throughMs;joined.state.knownAtMs=spot.knownAtMs;}
    }
    return extendedStrongReversal(reversal,volume,joined,decisionAtMs);
}
ExtendedStrongReversal extendedStrongReversal(const CoinFlowStudySet& core,const ExtendedStrongReversal* previous){
    ExtendedStrongReversal out;
    out.state=missing(QStringLiteral("반전 결정 시각의 기록이 필요해요"));
    out.grade=ExtendedReversalGrade::Incomplete;
    if(!core.available||core.asOfMs<=0||core.entries.size()>4096||core.groups.size()>4096||core.scorecard.outcomes.size()>4096)return out;
    const auto eventKey=[](CoinFlowSignalKind kind,qint64 at){return QString::number(int(kind))+":"+QString::number(at);};
    const auto direction=[](CoinFlowSignalKind kind){return kind==CoinFlowSignalKind::Rebound?Dir::Up:kind==CoinFlowSignalKind::TurnDown?Dir::Down:Dir::None;};
    QHash<QString,QList<const CoinFlowSignal*>> entries;
    QHash<QString,QList<const CoinFlowOutcome*>> outcomes;
    for(const auto& entry:core.entries)if(direction(entry.kind)!=Dir::None)entries[eventKey(entry.kind,entry.atMs)].append(&entry);
    for(const auto& value:core.scorecard.outcomes)if(direction(value.kind)!=Dir::None)outcomes[eventKey(value.kind,value.signalAtMs)].append(&value);
    const auto gradeAt=[&](CoinFlowSignalKind kind,qint64 at,qint64 decision){
        ExtendedStrongReversal grade;
        grade.state=missing(QStringLiteral("반전 결정 시각의 보강 자료가 일부 없어요"));
        grade.grade=ExtendedReversalGrade::Incomplete;grade.throughMs=at;grade.knownAtMs=decision;
        const auto found=entries.value(eventKey(kind,at));
        if(found.size()!=1)return grade;
        const auto& entry=*found.first();
        const bool up=kind==CoinFlowSignalKind::Rebound;
        if(entry.direction!=(up?CoinFlowDirection::Up:CoinFlowDirection::Down)||entry.knownAtMs!=decision
            ||decision<at||decision>core.asOfMs||!entry.state.available||entry.state.throughMs!=at
            ||!entry.reversalCorroboration)return grade;
        CoinReversalStudy original;original.state=entry.state;original.rebound=up;original.turnDown=!up;
        return extendedStrongReversal(original,entry.reversalCorroboration->volume,entry.reversalCorroboration->spot,decision);
    };
    // Current grade uses the same immutable decision as its historical event.
    // Merely advancing asOf must not slide the corroborating five-minute window.
    if(core.reversal.state.available&&!core.reversal.rebound&&!core.reversal.turnDown)
        out=extendedStrongReversal(core.reversal,{},CoinTakerStudy{});
    else if(core.reversal.rebound!=core.reversal.turnDown){
        const auto kind=core.reversal.rebound?CoinFlowSignalKind::Rebound:CoinFlowSignalKind::TurnDown;
        const auto found=entries.value(eventKey(kind,core.reversal.state.throughMs));
        if(found.size()==1)out=gradeAt(kind,core.reversal.state.throughMs,found.first()->knownAtMs);
    }
    out.asOfMs=core.asOfMs;out.identity=core.identity;
    QHash<QString,ExtendedReversalGrade> frozen;
    const auto canonical=[&](CoinFlowSignalKind kind,qint64 at,qint64 decision){return eventKey(kind,at)+":"+QString::number(decision);};
    if(previous&&!core.identity.isEmpty()&&previous->identity==core.identity&&previous->asOfMs<=core.asOfMs&&previous->events.size()<=4096)
        for(const auto& event:previous->events)if(int(event.grade)>=int(ExtendedReversalGrade::Basic)&&int(event.grade)<=int(ExtendedReversalGrade::Incomplete))
            frozen.insert(canonical(event.kind,event.signalAtMs,event.decisionAtMs),event.grade);
    auto groups=core.groups;
    std::stable_sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.fromMs<b.fromMs;});
    QSet<QString> seen;
    for(const auto& group:groups){
        const auto dir=direction(group.kind);
        if(dir==Dir::None||group.direction!=(dir==Dir::Up?CoinFlowDirection::Up:CoinFlowDirection::Down)
            ||group.fromMs<=0||group.fromMs>core.lastCompletedAtMs||group.fromMs>core.asOfMs
            ||group.throughMs<group.fromMs||group.knownAtMs<group.fromMs||group.knownAtMs>core.asOfMs)continue;
        const auto key=eventKey(group.kind,group.fromMs);
        if(seen.contains(key))continue;
        seen.insert(key);
        const auto original=gradeAt(group.kind,group.fromMs,group.knownAtMs);
        ExtendedReversalEvent event{group.kind,dir,original.grade,group.fromMs,group.knownAtMs,{}};
        // Raw trade rings can shrink after the first decision. Keep its already
        // delivered grade (including Incomplete); only new core outcomes change.
        // Events absent from the new core are deliberately not resurrected.
        const auto saved=frozen.constFind(canonical(group.kind,group.fromMs,group.knownAtMs));
        if(saved!=frozen.cend())event.grade=*saved;
        const auto found=outcomes.value(key);
        // The core has already checked every observed minute for gaps. Join only
        // that proof's exact physical one-hour endpoint, never a nearby candle.
        if(core.scorecard.horizonMs==Hour&&found.size()==1&&group.fromMs<=std::numeric_limits<qint64>::max()-Hour){
            const auto& value=*found.first();
            if(value.evaluationAtMs==group.fromMs+Hour&&value.evaluationAtMs<=core.lastCompletedAtMs
                &&value.knownAtMs>=value.evaluationAtMs&&value.knownAtMs<=core.asOfMs&&std::isfinite(value.returnPercent))event.outcome=value;
        }
        if(out.throughMs==event.signalAtMs){
            out.grade=event.grade;out.knownAtMs=event.decisionAtMs;
            if(event.grade==ExtendedReversalGrade::Incomplete)out.state=missing(QStringLiteral("반전 결정 시각의 보강 자료가 일부 없어요"));
            else out.state=ready(event.direction);
        }
        out.events.append(event);
    }
    return out;
}
ExtendedLowVolumeZones extendedTradeLowVolumeZones(const CoinFlowWindow& window,qint64 asOf){
    ExtendedLowVolumeZones out;out.candleApproximation=false;const auto observed=observedWindow(window,asOf,60,60);out.state=missing(observed.reason);if(!observed.available)return out;
    if(observed.trades.isEmpty()){out.state=missing(QStringLiteral("관측한 체결 가격이 없어요"));return out;}
    int lo=0,hi=0;Exact total;for(int i=0;i<observed.trades.size();++i){if(cmp(observed.trades[i].price,observed.trades[lo].price)<0)lo=i;if(cmp(observed.trades[i].price,observed.trades[hi].price)>0)hi=i;total=add(total,observed.trades[i].quantity);}
    std::array<Exact,20> bins;const auto& minimum=observed.trades[lo];const auto& maximum=observed.trades[hi];
    if(cmp(minimum.price,maximum.price)==0)return zonesFromBins(bins,total,minimum.priceValue,maximum.priceValue,observed.trades.size(),false);
    std::array<Exact,20> boundaries;for(int b=1;b<20;++b)boundaries[b]=add(times(minimum.price,20-b),times(maximum.price,b));
    for(const auto& t:observed.trades){const auto numerator=times(t.price,20);int bin=0;while(bin<19&&cmp(numerator,boundaries[bin+1])>=0)++bin;bins[bin]=add(bins[bin],t.quantity);}
    return zonesFromBins(bins,total,minimum.priceValue,maximum.priceValue,observed.trades.size(),false);
}
} // namespace keefi
