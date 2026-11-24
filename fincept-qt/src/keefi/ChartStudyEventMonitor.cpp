#include "keefi/ChartStudyEventMonitor.h"
#include <QObject>
namespace keefi {
QList<ChartPublicStudyEvent> ChartStudyEventMonitor::observe(const MarketCandleSeries& series,const ExtendedChartStudySet& studies,bool realtime){
    QList<ChartPublicStudyEvent> result;
    QString settings;for(const auto& line:studies.maLines)settings+=line.spec.id+":"+QString::number(line.spec.period)+":"+QString::number(int(line.spec.kind))+";";
    const auto context=series.identity+"|"+QString::number(series.intervalMs)+"|"+settings;
    if(context_!=context){clear();context_=context;}
    const auto completed=studies.lastCompletedAtMs;
    if(!realtime||!series.available||!studies.available||series.candles.isEmpty()||series.intervalMs<=0||studies.intervalMs!=series.intervalMs||
        completed!=series.candles.last().startsAtMs+series.intervalMs||completed<=0||series.receivedAtMs<completed||series.receivedAtMs-completed>90000){lanes_.clear();return result;}
    const QMap<QString,ExtendedStudyState> states{{"ma",studies.movingAverageState},{"bb",studies.bb.state},{"rsi",studies.rsi.state},{"macd",studies.macd.state},{"breakout",studies.breakout.state}};
    for(auto it=states.cbegin();it!=states.cend();++it){
        auto& lane=lanes_[it.key()];if(!it->available||it->direction==IndicatorDirection::None){lane={};continue;}
        if(completed<=lane.completed)continue;
        const bool continuous=lane.seeded&&completed==lane.completed+series.intervalMs;
        const auto previous=lane.direction;lane={completed,it->direction,true};
        if(!continuous||it->direction==previous||(it->direction!=IndicatorDirection::Up&&it->direction!=IndicatorDirection::Down))continue;
        const auto direction=it->direction==IndicatorDirection::Up?MovementDirection::Up:MovementDirection::Down;
        QString detail=QObject::tr("완료 봉 관측 · %1").arg(indicatorDirectionText(it->direction));
        if(it.key()=="breakout"&&studies.breakout.quality)detail+=QObject::tr(" · 조건 %1/10 (확률이 아닙니다)").arg(*studies.breakout.quality);
        result.append({WatchlistWorkspaceList::identityFromKey(series.identity),it.key(),
            QString::number(series.intervalMs)+"|"+QString::number(completed)+"|"+QString::number(int(direction)),detail,
            completed,series.receivedAtMs,direction,true});
    }
    return result;
}
void ChartStudyEventMonitor::clear(){lanes_.clear();context_.clear();}
}
