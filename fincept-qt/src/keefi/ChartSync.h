#pragma once
#include "keefi/MarketCandles.h"
#include "keefi/KeefiTheme.h"
#include <QMetaType>
#include <QPainter>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <optional>

namespace keefi {
enum class ChartSyncOption { Symbol, Interval, Crosshair, Time, DateRange };
// Continuous UTC plot edges, including empty time slots. Neither endpoint is
// rounded to an observed candle; Time aligns the centre and preserves the span.
struct ChartSyncViewport {
    qint64 fromMs=0,toMs=0;
    bool valid()const{return fromMs>0&&toMs>fromMs;}
    qint64 centreMs()const{return fromMs+(toMs-fromMs)/2;}
    bool operator==(const ChartSyncViewport&)const=default;
};
struct ChartSyncSettings {
    bool symbol=false,interval=false,crosshair=false,time=false,dateRange=false;
    bool enabled(ChartSyncOption option)const{
        switch(option){case ChartSyncOption::Symbol:return symbol;case ChartSyncOption::Interval:return interval;
        case ChartSyncOption::Crosshair:return crosshair;case ChartSyncOption::Time:return time;case ChartSyncOption::DateRange:return dateRange;}
        return false;
    }
    void set(ChartSyncOption option,bool value){
        switch(option){case ChartSyncOption::Symbol:symbol=value;break;case ChartSyncOption::Interval:interval=value;break;
        case ChartSyncOption::Crosshair:crosshair=value;break;case ChartSyncOption::Time:time=value;break;case ChartSyncOption::DateRange:dateRange=value;break;}
    }
};
// A target interval may differ from the source interval. Match the actual
// containing observed candle, never a nearest row across a missing interval.
inline const MarketCandle* chartSyncCandleAt(const QList<MarketCandle>& rows,qint64 interval,qint64 utc){
    if(utc<=0||interval<=0||rows.isEmpty())return nullptr;
    const auto found=std::upper_bound(rows.cbegin(),rows.cend(),utc,[](qint64 at,const MarketCandle& row){return at<row.startsAtMs;});
    if(found==rows.cbegin())return nullptr;
    const auto& row=*std::prev(found);
    return utc-row.startsAtMs<interval?&row:nullptr;
}
inline ChartSyncViewport chartSyncTargetViewport(const ChartSyncViewport& source,const ChartSyncViewport& target,bool matchRange){
    if(!source.valid()||(!matchRange&&!target.valid()))return {};
    if(matchRange)return source;
    const qint64 span=target.toMs-target.fromMs,centre=source.centreMs();
    return {centre-span/2,centre+(span-span/2)};
}
// A transparent child owns the remote marker without changing local cursor data.
// Qt may repaint its parent underneath, so keep old/new dirty areas to narrow
// vertical strips. Data-driven study calculation is independent of this overlay.
class ChartSyncMarker final:public QWidget {
  public:
    explicit ChartSyncMarker(QWidget* parent):QWidget(parent){
        setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);setFocusPolicy(Qt::NoFocus);hide();
    }
    void setMarker(const QRectF& plot,std::optional<double> x){
        if(!x||!std::isfinite(*x)||!plot.contains(QPointF(*x,plot.center().y()))){hide();return;}
        const QRect plotBounds=plot.toAlignedRect();
        const QRect next=QRect(int(std::floor(*x))-1,plotBounds.top(),3,plotBounds.height()).intersected(plotBounds);
        const auto local=*x-next.left();
        if(geometry()!=next)setGeometry(next);
        if(x_!=local){x_=local;update();}
        if(!isVisible()){show();raise();}
    }
  protected:
    void paintEvent(QPaintEvent*)override{QPainter p(this);p.setPen(QPen(themeColors().text2,1,Qt::DashLine));p.drawLine(QPointF(x_,0),QPointF(x_,height()));}
  private:
    double x_=0;
};
}
Q_DECLARE_METATYPE(keefi::ChartSyncViewport)
