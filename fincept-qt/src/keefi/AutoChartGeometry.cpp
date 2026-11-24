#include "keefi/AutoChartGeometry.h"
#include "keefi/ChartTimeAxis.h"
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
struct BarNumbers {
    double high=0, low=0, close=0;
};

QString normalizedDecimal(QString text) {
    const int dot=int(text.indexOf('.'));
    if(dot>=0){
        while(text.endsWith('0'))text.chop(1);
        if(text.endsWith('.'))text.chop(1);
    }
    int zero=0;
    while(zero+1<text.size()&&text[zero]=='0'&&text[zero+1]!='.')++zero;
    if(zero>0)text.remove(0,zero);
    return text;
}

bool sameDecimalPrice(const QString& a,const QString& b) {
    return normalizedDecimal(a)==normalizedDecimal(b);
}

double pivotPriceValue(const AutoChartPivot& pivot,const QList<BarNumbers>& nums) {
    return pivot.high?nums[pivot.bar].high:nums[pivot.bar].low;
}
} // namespace

AutoChartResult AutoChartGeometry::analyze(const ChartDrawingFrame& frame) {
    AutoChartResult result;
    if(!ChartDrawings::validFrame(frame)){
        result.reason=QStringLiteral("완료 봉 자료를 확인할 수 없어요");
        return result;
    }
    const int n=int(frame.bars.size());
    if(n<MinBars){
        result.reason=QStringLiteral("완료 봉이 21개 이상 필요해요");
        return result;
    }

    QList<BarNumbers> nums;
    nums.reserve(n);
    for(const auto& bar:frame.bars){
        bool hOk=false,lOk=false,cOk=false;
        const double h=bar.candle.high.toDouble(&hOk);
        const double l=bar.candle.low.toDouble(&lOk);
        const double c=bar.candle.close.toDouble(&cOk);
        if(!hOk||!lOk||!cOk||!std::isfinite(h)||!std::isfinite(l)||!std::isfinite(c)){
            result.reason=QStringLiteral("완료 봉 자료를 확인할 수 없어요");
            return result;
        }
        nums.append({h,l,c});
    }

    double trSum=0;
    for(int i=n-AtrBars;i<n;++i){
        const double range=nums[i].high-nums[i].low;
        double tr=range;
        if(i>0&&frame.bars[i].connectsPrevious){
            const double prevClose=nums[i-1].close;
            tr=std::max({range,std::abs(nums[i].high-prevClose),std::abs(nums[i].low-prevClose)});
        }
        trSum+=tr;
    }
    const double atr=trSum/double(AtrBars);
    if(!std::isfinite(atr)||atr<=0.0){
        result.reason=QStringLiteral("가격 변동이 없어 계산할 수 없어요");
        return result;
    }

    result.available=true;
    result.atr=atr;

    // A-4. 지그재그 피벗 (theta = ZigzagAtr * ATR20)
    const double theta=ZigzagAtr*atr;
    int dir=0,highBar=0,lowBar=0,candBar=0;
    for(int i=1;i<n;++i){
        if(dir==0){
            const bool upHit=(nums[i].high-nums[lowBar].low)>=theta;
            const bool downHit=(nums[highBar].high-nums[i].low)>=theta;
            if(upHit&&downHit){
                if(lowBar<highBar){
                    result.pivots.append({lowBar,false,frame.bars[lowBar].candle.low});
                    dir=1;candBar=i;
                }else if(highBar<lowBar){
                    result.pivots.append({highBar,true,frame.bars[highBar].candle.high});
                    dir=-1;candBar=i;
                }else{
                    bool openOk=false;
                    const double open=frame.bars[lowBar].candle.open.toDouble(&openOk);
                    if(!openOk||nums[lowBar].close>=open){
                        result.pivots.append({lowBar,false,frame.bars[lowBar].candle.low});
                        dir=1;
                    }else{
                        result.pivots.append({highBar,true,frame.bars[highBar].candle.high});
                        dir=-1;
                    }
                    candBar=i;
                }
            }else if(upHit){
                result.pivots.append({lowBar,false,frame.bars[lowBar].candle.low});
                dir=1;candBar=i;
            }else if(downHit){
                result.pivots.append({highBar,true,frame.bars[highBar].candle.high});
                dir=-1;candBar=i;
            }else{
                if(nums[i].high>nums[highBar].high)highBar=i;
                if(nums[i].low<nums[lowBar].low)lowBar=i;
            }
        }else if(dir>0){
            if(nums[i].high>nums[candBar].high){
                candBar=i;
            }else if((nums[candBar].high-nums[i].low)>=theta){
                result.pivots.append({candBar,true,frame.bars[candBar].candle.high});
                dir=-1;candBar=i;
            }
        }else{
            if(nums[i].low<nums[candBar].low){
                candBar=i;
            }else if((nums[i].high-nums[candBar].low)>=theta){
                result.pivots.append({candBar,false,frame.bars[candBar].candle.low});
                dir=1;candBar=i;
            }
        }
    }

    const auto anchorFor=[&](const AutoChartPivot& pivot){
        return ChartDrawingPoint{frame.bars[pivot.bar].candle.startsAtMs,pivot.price};
    };

    // A-5. 피보나치 되돌림·확장 후보
    const int pivotCount=int(result.pivots.size());
    if(pivotCount>=2){
        const AutoChartPivot& a=pivotCount>=3?result.pivots[pivotCount-3]:result.pivots[0];
        const AutoChartPivot& b=pivotCount>=3?result.pivots[pivotCount-2]:result.pivots[1];
        if(!sameDecimalPrice(a.price,b.price)){
            const ChartDrawingPoint pointA=anchorFor(a),pointB=anchorFor(b);
            result.candidates.append({QStringLiteral("fib-retracement"),ChartDrawingKind::FibRetracement,{pointA,pointB},0,{}});
            QList<ChartDrawingPoint> extAnchors{pointA,pointB};
            if(pivotCount>=3)extAnchors.append(anchorFor(result.pivots[pivotCount-1]));
            result.candidates.append({QStringLiteral("fib-extension"),ChartDrawingKind::FibExtension,extAnchors,0,{}});
        }
    }

    // A-7. 횡보 구간 (`zone`, 0~1개) — 후보는 A-9 순서상 피보나치 다음, 수평선 앞
    std::optional<AutoChartCandidate> zoneCandidate;
    const double zoneLimit=ZoneRangeAtr*atr;
    for(int end=n-1;end>=ZoneMinBars-1;--end){
        double maxH=nums[end].high,minL=nums[end].low;
        QString maxStr=frame.bars[end].candle.high,minStr=frame.bars[end].candle.low;
        int bestStart=end;
        for(int start=end-1;start>=0;--start){
            const double nextMax=std::max(maxH,nums[start].high);
            const double nextMin=std::min(minL,nums[start].low);
            if((nextMax-nextMin)>zoneLimit)break;
            if(nums[start].high>maxH){maxH=nums[start].high;maxStr=frame.bars[start].candle.high;}
            if(nums[start].low<minL){minL=nums[start].low;minStr=frame.bars[start].candle.low;}
            bestStart=start;
        }
        if((end-bestStart+1)>=ZoneMinBars){
            if(maxH>minL&&!sameDecimalPrice(maxStr,minStr)){
                zoneCandidate=AutoChartCandidate{
                    QStringLiteral("zone"),ChartDrawingKind::Zone,
                    {{frame.bars[bestStart].candle.startsAtMs,maxStr},{frame.bars[end].candle.startsAtMs,minStr}},
                    0,{}
                };
            }
            break;
        }
    }
    if(zoneCandidate)result.candidates.append(*zoneCandidate);

    // A-6. 지지·저항 (`horizontal:1..3`)
    if(!result.pivots.isEmpty()){
        auto sorted=result.pivots;
        std::stable_sort(sorted.begin(),sorted.end(),[&](const AutoChartPivot& lhs,const AutoChartPivot& rhs){
            const double lp=pivotPriceValue(lhs,nums),rp=pivotPriceValue(rhs,nums);
            if(lp!=rp)return lp<rp;
            return lhs.bar<rhs.bar;
        });
        struct Cluster {
            double firstPrice=0;
            int touches=0;
            AutoChartPivot latest;
            double repPrice=0;
        };
        const double levelTol=LevelToleranceAtr*atr;
        QList<Cluster> clusters;
        for(const auto& pivot:sorted){
            const double price=pivotPriceValue(pivot,nums);
            if(clusters.isEmpty()||(price-clusters.last().firstPrice)>levelTol){
                clusters.append({price,1,pivot,price});
            }else{
                auto& back=clusters.last();
                ++back.touches;
                if(pivot.bar>back.latest.bar){
                    back.latest=pivot;
                    back.repPrice=price;
                }
            }
        }
        QList<Cluster> kept;
        for(const auto& cluster:clusters)if(cluster.touches>=MinLevelTouches)kept.append(cluster);
        const double lastClose=nums.last().close;
        std::stable_sort(kept.begin(),kept.end(),[&](const Cluster& lhs,const Cluster& rhs){
            if(lhs.touches!=rhs.touches)return lhs.touches>rhs.touches;
            const double ld=std::abs(lhs.repPrice-lastClose),rd=std::abs(rhs.repPrice-lastClose);
            if(ld!=rd)return ld<rd;
            if(lhs.repPrice!=rhs.repPrice)return lhs.repPrice>rhs.repPrice;
            return lhs.latest.bar>rhs.latest.bar;
        });
        if(kept.size()>MaxLevels)kept.resize(MaxLevels);
        std::stable_sort(kept.begin(),kept.end(),[](const Cluster& lhs,const Cluster& rhs){
            if(lhs.repPrice!=rhs.repPrice)return lhs.repPrice>rhs.repPrice;
            return lhs.latest.bar>rhs.latest.bar;
        });
        for(int i=0;i<kept.size();++i){
            result.candidates.append({
                QStringLiteral("horizontal:%1").arg(i+1),ChartDrawingKind::Horizontal,
                {anchorFor(kept[i].latest)},kept[i].touches,{}
            });
        }
    }

    // A-8. 추세선 (`trend`, 0~1개)
    struct TrendCandidate {
        bool isHigh=false;
        int x1=0,x2=0,touches=0,pierces=0;
        AutoChartCandidate candidate;
    };
    std::optional<TrendCandidate> bestTrend;
    const double trendTau=TrendToleranceAtr*atr;
    const auto evaluateSide=[&](bool highSide){
        QList<AutoChartPivot> allSide;
        for(const auto& pivot:result.pivots)if(pivot.high==highSide)allSide.append(pivot);
        const int startIdx=std::max(0,int(allSide.size())-TrendPivotWindow);
        const int m=int(allSide.size());
        for(int a=startIdx;a<m;++a){
            for(int b=a+1;b<m;++b){
                const int x1=allSide[a].bar,x2=allSide[b].bar;
                if(x2<=x1)continue;
                const double y1=pivotPriceValue(allSide[a],nums),y2=pivotPriceValue(allSide[b],nums);
                const auto lineAt=[&](int x){return y1+(y2-y1)*double(x-x1)/double(x2-x1);};

                int touches=0;
                for(int c=a;c<m;++c){
                    const double py=pivotPriceValue(allSide[c],nums);
                    if(std::abs(py-lineAt(allSide[c].bar))<=trendTau)++touches;
                }
                if(touches<MinTrendTouches)continue;

                int pierces=0;
                QList<qint64> sweeps;
                for(int barIdx=x1+1;barIdx<n;++barIdx){
                    const double y=lineAt(barIdx);
                    if(!highSide){
                        if(nums[barIdx].close<y-trendTau)++pierces;
                        else if(nums[barIdx].low<y-trendTau)sweeps.append(frame.bars[barIdx].candle.startsAtMs);
                    }else{
                        if(nums[barIdx].close>y+trendTau)++pierces;
                        else if(nums[barIdx].high>y+trendTau)sweeps.append(frame.bars[barIdx].candle.startsAtMs);
                    }
                    if(pierces>MaxTrendPierces)break;
                }
                if(pierces>MaxTrendPierces)continue;

                TrendCandidate cand{
                    highSide,x1,x2,touches,pierces,
                    {QStringLiteral("trend"),ChartDrawingKind::Trend,{anchorFor(allSide[a]),anchorFor(allSide[b])},touches,sweeps,highSide}
                };
                const auto better=[&](const TrendCandidate& lhs,const TrendCandidate& rhs){
                    if(lhs.touches!=rhs.touches)return lhs.touches>rhs.touches;
                    if(lhs.x2!=rhs.x2)return lhs.x2>rhs.x2;
                    if(lhs.pierces!=rhs.pierces)return lhs.pierces<rhs.pierces;
                    if(lhs.isHigh!=rhs.isHigh)return !lhs.isHigh;
                    return lhs.x1>rhs.x1;
                };
                if(!bestTrend||better(cand,*bestTrend))bestTrend=cand;
            }
        }
    };
    evaluateSide(false);
    evaluateSide(true);
    if(bestTrend)result.candidates.append(bestTrend->candidate);

    if(result.candidates.isEmpty())result.reason=QStringLiteral("찾은 도형이 없어요");
    return result;
}

QList<QPointF> AutoChartGeometry::trendDisplayPath(const QList<ChartDrawingPoint>& anchors,
                                                  const ChartDrawingFrame& frame, const ChartTimeAxis& axis) {
    if (anchors.size() != 2 || !axis.available) return {};
    const auto barIndex = [&](qint64 at) -> qsizetype {
        const auto it = std::lower_bound(frame.bars.cbegin(), frame.bars.cend(), at,
            [](const ChartDrawingBar& bar, qint64 time) { return bar.candle.startsAtMs < time; });
        return it != frame.bars.cend() && it->candle.startsAtMs == at ? it - frame.bars.cbegin() : -1;
    };
    const auto first = barIndex(anchors[0].atMs);
    const auto last = barIndex(anchors[1].atMs);
    bool validFirst = false, validLast = false;
    const double firstPrice = anchors[0].price.toDouble(&validFirst);
    const double lastPrice = anchors[1].price.toDouble(&validLast);
    if (first < 0 || last < 0 || !validFirst || !validLast || !std::isfinite(firstPrice) || !std::isfinite(lastPrice)) return {};
    const auto firstSlot = axis.coordinateAt(anchors[0].atMs);
    if (!firstSlot) return {};
    QList<QPointF> path{{*firstSlot, firstPrice}};
    if (first == last) {
        path.append({*firstSlot, lastPrice});
        return path;
    }
    const int direction = last > first ? 1 : -1;
    double previousStride = 0;
    QPointF previous = path.first();
    for (auto i = first + direction; ; i += direction) {
        const auto slot = axis.coordinateAt(frame.bars[i].candle.startsAtMs);
        if (!slot) return {};
        const double stride = *slot - previous.x();
        // Consecutive bars with equal display spacing share a single straight segment.
        if (i != first + direction && std::abs(stride - previousStride) > 1e-8)
            path.append(previous);
        if (i == last) {
            path.append({*slot, lastPrice});
            return path;
        }
        const double fraction = double(i - first) / double(last - first);
        previous = {*slot, firstPrice + (lastPrice - firstPrice) * fraction};
        previousStride = stride;
    }
}
}
