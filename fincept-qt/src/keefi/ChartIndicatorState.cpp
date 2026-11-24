#include "keefi/ChartIndicatorState.h"
#include "keefi/ChartStudies.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include <QMap>
#include <QObject>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
namespace keefi {
bool chartIndicatorContributesCurrentState(const QString& id) {
    return !id.isEmpty()&&id!="similarity"&&id!="volume"&&id!="sentiment"&&id!="strong";
}
ChartIndicatorSummary summarizeChartIndicators(const QList<ChartIndicatorState>& input,const QString& context,quint64 revision) {
    QMap<QString,ChartIndicatorState> active;
    for(const auto& state:input) {
        if(!state.enabled||!state.stateBearing||!chartIndicatorContributesCurrentState(state.id)||state.context!=context||state.revision!=revision)continue;
        if(!active.contains(state.id))active.insert(state.id,state);
        else {
            // Identical deliveries are harmless. Conflicting duplicate parts
            // are combined conservatively, independent of delivery order.
            auto& old=active[state.id];old.requiredReady&=state.requiredReady;
            old.atoms.append(state.atoms);
        }
    }
    struct Material {bool up=false,down=false,valid=false,missing=false;QSet<QString> ids,reasons;};
    QMap<QString,Material> materials;
    ChartIndicatorSummary result;result.enabled=active.size();
    for(const auto& state:active) {
        bool anyValid=false;
        for(const auto& atom:state.atoms) {
            if(atom.group.isEmpty())continue;
            auto& material=materials[atom.group];material.ids.insert(state.id);
            const bool valid=atom.ready&&atom.direction!=IndicatorDirection::None;
            anyValid|=valid;material.valid|=valid;material.missing|=!valid;
            if(!valid) {if(!atom.reason.isEmpty())material.reasons.insert(atom.reason);continue;}
            material.up|=atom.direction==IndicatorDirection::Up||atom.direction==IndicatorDirection::Mix;
            material.down|=atom.direction==IndicatorDirection::Down||atom.direction==IndicatorDirection::Mix;
        }
        if(state.requiredReady&&anyValid)++result.ready;
    }
    for(auto it=materials.cbegin();it!=materials.cend();++it) {
        IndicatorGroupState group;group.group=it.key();group.partial=it->missing;
        group.indicatorIds=it->ids.values();group.indicatorIds.sort();
        group.reasons=it->reasons.values();group.reasons.sort();
        if(!it->valid){group.direction=IndicatorDirection::None;++result.none;}
        else if(it->up&&it->down){group.direction=IndicatorDirection::Mix;++result.mix;}
        else if(it->up){group.direction=IndicatorDirection::Up;++result.up;}
        else if(it->down){group.direction=IndicatorDirection::Down;++result.down;}
        else {group.direction=IndicatorDirection::Info;++result.info;}
        result.groups.append(group);
    }
    if(result.enabled)result.completenessPercent=int((qint64(result.ready)*200+result.enabled)/(2*result.enabled));
    return result;
}
QString indicatorDirectionText(IndicatorDirection direction) {
    switch(direction) {
    case IndicatorDirection::Up:return QObject::tr("▲ 위쪽");
    case IndicatorDirection::Down:return QObject::tr("▼ 아래쪽");
    case IndicatorDirection::Mix:return QObject::tr("섞임");
    case IndicatorDirection::Info:return QObject::tr("방향 없음");
    case IndicatorDirection::None:return QObject::tr("자료 없음");
    }
    return {};
}
bool chartScorecardStatisticsVisible(bool available,int samples) {
    return available&&samples>=ChartScorecardDisplayMinimumSamples;
}
namespace {
struct HistoricalAccumulator {
    ChartHistoricalSlice row;double sum=0,signedSum=0;qint64 previous=0;bool directional=false;
    void event(qint64 at,qint64 horizon){++row.events;if(previous&&at<previous+horizon)++row.overlapping;previous=at;}
    void result(double value,int direction){++row.results;sum+=value;signedSum+=value*direction;directional=direction!=0;if(value>0)++row.up;else if(value<0)++row.down;else ++row.flat;}
    ChartHistoricalSlice finish(){row.pending=row.events-row.results;if(row.results>=ChartScorecardDisplayMinimumSamples){row.meanReturnPercent=sum/row.results;if(directional)row.meanDirectionalReturnPercent=signedSum/row.results;}return row;}
};
bool targetFits(qint64 at,qint64 horizon,qint64 last){return at>0&&horizon>0&&at<=std::numeric_limits<qint64>::max()-horizon&&at+horizon<=last;}
}
QList<ChartHistoricalSlice> chartPullbackScoreSlices(const ChartStudySet& core){
    if(!core.available||core.entries.size()>4096||core.scorecard.outcomes.size()>4096)return {};
    auto entries=core.entries;std::stable_sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.atMs<b.atMs;});
    QMap<int,HistoricalAccumulator> rows;QSet<qint64> seen;
    for(const auto& entry:entries){
        if(entry.atMs<=0||entry.atMs>core.lastCompletedAtMs||entry.score<0||entry.score>10||seen.contains(entry.atMs))continue;seen.insert(entry.atMs);
        const int band=entry.score<5?0:entry.score<8?1:2;auto& acc=rows[band];acc.row.key=QString("pullback:%1").arg(band);
        acc.row.label=band==0?QObject::tr("점수 0–4"):band==1?QObject::tr("점수 5–7"):QObject::tr("점수 8–10");acc.event(entry.atMs,core.scorecard.horizonMs);
        if(!targetFits(entry.atMs,core.scorecard.horizonMs,core.lastCompletedAtMs))continue;
        const ChartOutcome* match=nullptr;int count=0;
        for(const auto& out:core.scorecard.outcomes)if(out.signalAtMs==entry.atMs){match=&out;++count;}
        if(count==1&&match->evaluationAtMs==entry.atMs+core.scorecard.horizonMs&&std::isfinite(match->returnPercent))acc.result(match->returnPercent,0);
    }
    QList<ChartHistoricalSlice> result;for(auto& acc:rows)result.append(acc.finish());return result;
}
QList<ChartHistoricalSlice> chartFlowScoreSlices(const CoinFlowStudySet& core){
    if(!core.available||core.groups.size()>4096||core.scorecard.outcomes.size()>4096)return {};
    auto groups=core.groups;std::stable_sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.fromMs<b.fromMs;});
    const QStringList labels{QObject::tr("대량 체결"),QObject::tr("체결 방향"),QObject::tr("미결제약정 급변"),QObject::tr("거래량 증가"),QObject::tr("반등"),QObject::tr("하락 반전")};
    QMap<QString,HistoricalAccumulator> rows;QSet<QString> seen;
    for(const auto& group:groups){
        const int kind=int(group.kind),direction=int(group.direction);
        if(kind<0||kind>=labels.size()||direction<0||direction>2||group.fromMs<=0||group.fromMs>core.lastCompletedAtMs||group.knownAtMs>core.asOfMs)continue;
        const QString key=QString("flow:%1:%2").arg(kind).arg(direction),eventKey=key+":"+QString::number(group.fromMs);
        if(seen.contains(eventKey))continue;seen.insert(eventKey);auto& acc=rows[key];acc.row.key=key;
        acc.row.label=labels[kind]+" · "+(direction==1?QObject::tr("위쪽"):direction==2?QObject::tr("아래쪽"):QObject::tr("방향 없음"));acc.event(group.fromMs,core.scorecard.horizonMs);
        if(!targetFits(group.fromMs,core.scorecard.horizonMs,core.lastCompletedAtMs))continue;
        QSet<int> directions;for(const auto& other:groups)if(other.kind==group.kind&&other.fromMs==group.fromMs)directions.insert(int(other.direction));
        if(directions.size()!=1)continue; // Legacy outcome has no direction: ambiguous joins cannot be guessed.
        const CoinFlowOutcome* match=nullptr;int count=0;
        for(const auto& out:core.scorecard.outcomes)if(out.kind==group.kind&&out.signalAtMs==group.fromMs){match=&out;++count;}
        if(count==1&&match->evaluationAtMs==group.fromMs+core.scorecard.horizonMs&&match->knownAtMs<=core.asOfMs&&std::isfinite(match->returnPercent))acc.result(match->returnPercent,direction==1?1:direction==2?-1:0);
    }
    QList<ChartHistoricalSlice> result;for(auto& acc:rows)result.append(acc.finish());return result;
}
QList<ChartHistoricalSlice> chartStrongReversalScoreSlices(const ExtendedStrongReversal& core){
    constexpr qint64 hour=60*60000;
    if(core.asOfMs<=0||core.horizonMs!=hour||core.events.size()>4096)return {};
    const QStringList labels{QObject::tr("기본"),QObject::tr("보강"),QObject::tr("강함"),QObject::tr("미확정")};
    QMap<QString,HistoricalAccumulator> rows;
    for(int grade=1;grade<=4;++grade)for(int direction=1;direction<=2;++direction){
        const auto key=QString("strong:%1:%2").arg(grade).arg(direction);auto& acc=rows[key];acc.row.key=key;
        acc.row.label=labels[grade-1]+" · "+(direction==1?QObject::tr("위쪽"):QObject::tr("아래쪽"));
    }
    auto events=core.events;
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.signalAtMs<b.signalAtMs;});
    QSet<QString> seen;
    for(const auto& event:events){
        const int grade=int(event.grade),direction=event.direction==IndicatorDirection::Up?1:event.direction==IndicatorDirection::Down?2:0;
        if(grade<1||grade>4||!direction||event.signalAtMs<=0||event.signalAtMs>core.asOfMs
            ||event.decisionAtMs<event.signalAtMs||event.decisionAtMs>core.asOfMs)continue;
        const auto eventKey=QString::number(int(event.kind))+":"+QString::number(event.signalAtMs);
        if(seen.contains(eventKey))continue;
        seen.insert(eventKey);
        auto& acc=rows[QString("strong:%1:%2").arg(grade).arg(direction)];acc.event(event.signalAtMs,hour);
        if(!event.outcome||!targetFits(event.signalAtMs,hour,core.asOfMs))continue;
        const auto& value=*event.outcome;
        if(value.kind==event.kind&&value.signalAtMs==event.signalAtMs&&value.evaluationAtMs==event.signalAtMs+hour
            &&value.knownAtMs>=value.evaluationAtMs&&value.knownAtMs<=core.asOfMs&&std::isfinite(value.returnPercent))
            acc.result(value.returnPercent,direction==1?1:-1);
    }
    QList<ChartHistoricalSlice> result;for(auto& acc:rows)result.append(acc.finish());return result;
}
}
