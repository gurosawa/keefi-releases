#include "keefi/ChartGridView.h"
#include "keefi/CompactControls.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LocalComboBox.h"
#include "keefi/MovementColorPolicy.h"
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QTimeZone>
#include <QDateTime>
#include <QTimer>
#include <functional>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
bool publicSpot(const QString& key) {
    const auto id=WatchlistWorkspaceList::identityFromKey(key);
    return WatchlistWorkspaceList::validIdentity(id)&&id.market=="coin"&&(id.source=="upbit"||id.source=="binance");
}
class GridCandles final : public QWidget {
  public:
    explicit GridCandles(QWidget* parent):QWidget(parent){
        setMinimumHeight(220);setAccessibleName(tr("분할 차트 완료 봉"));
        setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
        marker_=new ChartSyncMarker(this);
        bindThemePresentation(this,[this]{update();marker_->update();});
    }
    std::function<void(ChartSyncViewport,bool)> navigated;
    std::function<void(qint64)> hovered;
    ChartStudyObservation data;
    bool covered=false;
    void setObservation(const ChartStudyObservation& next){
        const bool context=data.series.identity!=next.series.identity||data.series.intervalMs!=next.series.intervalMs;
        const auto& a=data.series;const auto& b=next.series;
        const bool changed=context||a.available!=b.available||a.receivedAtMs!=b.receivedAtMs||
            a.restRevision!=b.restRevision||a.candles.size()!=b.candles.size()||
            (!a.candles.isEmpty()&&!b.candles.isEmpty()&&
             (a.candles.first().startsAtMs!=b.candles.first().startsAtMs||a.candles.last().startsAtMs!=b.candles.last().startsAtMs||
              a.candles.last().close!=b.candles.last().close||a.candles.last().volume!=b.candles.last().volume));
        data=next;
        if(context){range_={};setCrosshair(0);}
        if(changed){
            setProperty("passiveStudyCalculationCount",qulonglong(++studyCalculationCount_));
            averages_.clear();QList<double> window;double sum=0;qint64 previous=0;
            for(const auto& row:data.series.candles){
                if(previous&&row.startsAtMs-previous!=data.series.intervalMs){window.clear();sum=0;}
                previous=row.startsAtMs;const double close=row.close.toDouble();window.append(close);sum+=close;
                if(window.size()>20)sum-=window.takeFirst();
                averages_.append(window.size()==20?sum/20:std::numeric_limits<double>::quiet_NaN());
            }
            publish();update();
        }else refreshMarker();
    }
    ChartSyncViewport viewport()const{
        if(range_.valid())return range_;
        const auto& rows=data.series.candles;const auto interval=data.series.intervalMs;
        if(rows.isEmpty()||interval<=0)return {};
        const qint64 right=rows.last().startsAtMs+interval/2;
        return {qMax(rows.first().startsAtMs-interval/2,right-qint64(count_*interval)),right};
    }
    bool followsLive()const{return !range_.valid();}
    int nominalZoomBars()const{return qRound(count_);}
    void restoreLiveViewport(int zoomBars){
        if(followsLive()&&count_==zoomBars)return;
        count_=zoomBars;range_={};
        publish();update();
    }
    void applyViewport(const ChartSyncViewport& source,bool matchRange){
        const auto next=chartSyncTargetViewport(source,viewport(),matchRange);if(!next.valid())return;
        range_=next;publish();update();
    }
    void setCrosshair(qint64 at){crosshairMs_=at;refreshMarker();}
    void resetContext(){range_={};setCrosshair(0);publish();update();}
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.fillRect(rect(),themeColors().bg);p.setFont(KeefiFonts::font(FontRole::Caption12));p.setPen(themeColors().text3);
        if(covered)return;
        const auto indices=visibleIndices();
        if(!data.series.available||data.series.candles.isEmpty()){
            p.drawText(rect(),Qt::AlignCenter,data.status.isEmpty()?tr("완료 봉을 기다려요"):data.status);return;}
        if(indices.isEmpty()){p.drawText(rect(),Qt::AlignCenter,tr("이 구간에 관측된 봉이 없어요"));return;}
        double low=std::numeric_limits<double>::max(),high=0;
        for(int index:indices){const auto& row=data.series.candles[index];const auto l=row.low.toDouble(),h=row.high.toDouble();
            if(!std::isfinite(l)||!std::isfinite(h)||l<=0||h<l)return;low=qMin(low,l);high=qMax(high,h);
            if(index<averages_.size()&&std::isfinite(averages_[index])){low=qMin(low,averages_[index]);high=qMax(high,averages_[index]);}}
        if(high==low){high*=1.001;low*=.999;}
        const auto plot=plotArea();const auto contentHeight=qMax(1,height()-76);const auto volumeHeight=qMax(24,contentHeight/5);
        const QRectF volumePlot(plot.left(),plot.bottom()+8,plot.width(),volumeHeight);
        const auto bounds=viewport();const double span=bounds.toMs-bounds.fromMs;const auto interval=data.series.intervalMs;
        const auto x=[&](qint64 at){return plot.left()+double(at-bounds.fromMs)/span*plot.width();};
        const auto y=[&](double value){return plot.bottom()-(value-low)/(high-low)*plot.height();};
        double maxVolume=0;for(int i:indices){const double v=data.series.candles[i].volume.toDouble();if(std::isfinite(v)&&v>=0)maxVolume=qMax(maxVolume,v);}
        for(int n=0;n<3;++n){const auto value=low+(high-low)*n/2;const auto at=y(value);p.setPen(themeColors().rule);p.drawLine(QPointF(plot.left(),at),QPointF(plot.right(),at));p.setPen(themeColors().text3);p.drawText(QRectF(plot.right()+5,at-9,76,18),Qt::AlignLeft,QString::number(value,'g',7));}
        for(int i:indices){
            const auto& row=data.series.candles[i];const double o=row.open.toDouble(),c=row.close.toDouble();if(!std::isfinite(o)||!std::isfinite(c)||o<=0||c<=0)continue;
            const auto at=x(row.startsAtMs);const auto color=MovementColorPolicy::colorFor(this,c>=o?MovementDirection::Up:MovementDirection::Down);
            p.setPen(color);p.setBrush(color);p.drawLine(QPointF(at,y(row.high.toDouble())),QPointF(at,y(row.low.toDouble())));
            const auto w=qBound(1.0,plot.width()*interval/span*.65,10.0);p.drawRect(QRectF(at-w/2,qMin(y(o),y(c)),w,qMax(1.0,std::abs(y(o)-y(c)))));
            const auto volume=row.volume.toDouble();if(maxVolume>0&&std::isfinite(volume)&&volume>0){
                const auto h=volume/maxVolume*volumePlot.height();p.setOpacity(.55);p.drawRect(QRectF(at-w/2,volumePlot.bottom()-h,w,h));p.setOpacity(1);}
        }
        QPainterPath averageLine;bool connected=false;qint64 previous=0;
        for(int i:indices){const auto& row=data.series.candles[i];
            if(i>=averages_.size()||!std::isfinite(averages_[i])){connected=false;continue;}
            if(connected&&row.startsAtMs-previous==interval)averageLine.lineTo(x(row.startsAtMs),y(averages_[i]));
            else averageLine.moveTo(x(row.startsAtMs),y(averages_[i]));
            connected=true;previous=row.startsAtMs;
        }
        p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(themeColors().accent,1.5));p.setBrush(Qt::NoBrush);p.drawPath(averageLine);
        const int last=indices.last();const auto& row=data.series.candles[last];
        const auto averageText=last<averages_.size()&&std::isfinite(averages_[last])?tr("SMA20 %1").arg(QString::number(averages_[last],'g',7)):tr("SMA20 · 연속 20봉 대기");
        p.setPen(themeColors().text3);p.drawText(QRectF(8,22,width()-16,20),averageText+tr(" · 거래량 %1 %2").arg(row.volume,data.series.asset));
        p.setPen(themeColors().text2);p.drawText(QRectF(8,0,width()-16,20),tr("종가 %1 %2").arg(row.close,data.series.currency));
        p.setPen(themeColors().text3);p.drawText(QRectF(8,height()-22,width()-16,20),QDateTime::fromMSecsSinceEpoch(row.startsAtMs+interval,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm 'KST · UTC+9'"));
    }
    void resizeEvent(QResizeEvent* e)override{QWidget::resizeEvent(e);publish();}
    void leaveEvent(QEvent*)override{if(hovered)hovered(0);}
    void mousePressEvent(QMouseEvent* e)override{
        if(e->button()==Qt::LeftButton&&plotArea().contains(e->position())){dragging_=true;dragX_=e->position().x();setFocus();setCursor(Qt::ClosedHandCursor);e->accept();}
    }
    void mouseReleaseEvent(QMouseEvent* e)override{dragging_=false;unsetCursor();e->accept();}
    void mouseMoveEvent(QMouseEvent* e)override{
        const auto bounds=viewport();const auto plot=plotArea();
        if(!bounds.valid()||!plot.contains(e->position())){if(hovered)hovered(0);return;}
        if(dragging_){pan((e->position().x()-dragX_)/plot.width()*(bounds.toMs-bounds.fromMs));dragX_=e->position().x();if(hovered)hovered(0);return;}
        if(!hovered)return;
        const auto& series=data.series;
        const auto at=bounds.fromMs+qint64((e->position().x()-plot.left())/plot.width()*(bounds.toMs-bounds.fromMs));
        // The drawn candle is centred on its start, just as in the full chart.
        // Interval containment by itself would map half of the slot to the bar on its left.
        const auto* row=series.available?chartSyncCandleAt(series.candles,series.intervalMs,at+series.intervalMs/2):nullptr;
        const bool hit=row&&row->startsAtMs>bounds.fromMs&&row->startsAtMs<bounds.toMs;
        hovered(hit?row->startsAtMs:0);
    }
    void wheelEvent(QWheelEvent* e)override{
        if(!plotArea().contains(e->position())||!viewport().valid()){e->ignore();return;}
        const bool pixels=!e->pixelDelta().isNull();QPointF delta=pixels?QPointF(e->pixelDelta()):QPointF(e->angleDelta());
        if((e->modifiers()&Qt::ShiftModifier)&&delta.x()==0)delta=QPointF(delta.y(),0);
        if(e->phase()==Qt::ScrollBegin){wheelAxis_=0;wheelTravel_={};}
        const bool phased=e->phase()!=Qt::NoScrollPhase;
        if(!delta.isNull()){
            wheelTravel_+=delta;
            if(!phased||!wheelAxis_){const auto d=phased?wheelTravel_:delta;wheelAxis_=std::abs(d.x())>=std::abs(d.y())&&d.x()!=0?1:2;}
            if(wheelAxis_==1){const auto span=viewport().toMs-viewport().fromMs;pan(pixels?delta.x()/plotArea().width()*span:delta.x()/120.0*10*data.series.intervalMs);}
            else {zoomRemainder_-=pixels?delta.y()/8.0:delta.y()/12.0;const int steps=int(zoomRemainder_);zoomRemainder_-=steps;zoom(steps,e->position().x());}}
        if(!phased||e->phase()==Qt::ScrollEnd){wheelAxis_=0;wheelTravel_={};}e->accept();
    }
    void keyPressEvent(QKeyEvent* e)override{
        if(e->key()==Qt::Key_Left)pan(10.0*data.series.intervalMs);
        else if(e->key()==Qt::Key_Right)pan(-10.0*data.series.intervalMs);
        else if(e->key()==Qt::Key_Plus||e->key()==Qt::Key_Equal)zoom(-10,plotArea().right());
        else if(e->key()==Qt::Key_Minus)zoom(10,plotArea().right());
        else if(e->key()==Qt::Key_End){range_={};publishUser(false);}
        else{QWidget::keyPressEvent(e);return;}e->accept();
    }
  private:
    QRectF plotArea()const{const auto h=qMax(1,height()-76);return QRectF(8,44,qMax(1,width()-90),qMax(1,h-qMax(24,h/5)-8));}
    QList<int> visibleIndices()const{QList<int> indices;const auto range=viewport();if(!range.valid())return indices;
        for(int i=0;i<data.series.candles.size();++i){const auto at=data.series.candles[i].startsAtMs;if(at>range.fromMs&&at<range.toMs)indices.append(i);}return indices;}
    void pan(double deltaMs){const auto before=viewport();if(!before.valid()||!std::isfinite(deltaMs)||data.series.candles.isEmpty()||data.series.intervalMs<=0)return;
        const auto& rows=data.series.candles;const qint64 span=before.toMs-before.fromMs;
        const qint64 first=rows.first().startsAtMs-data.series.intervalMs/2,last=rows.last().startsAtMs+data.series.intervalMs/2;
        const qint64 right=qBound(qMin(first+span,last),before.toMs-qint64(deltaMs),last);
        range_={right-span,right};if(range_!=before)publishUser(false);}
    void zoom(int delta,double pixel){const auto before=viewport();if(!before.valid()||!delta||data.series.candles.isEmpty()||data.series.intervalMs<=0)return;
        const auto plot=plotArea();const double fraction=qBound(0.0,(pixel-plot.left())/plot.width(),1.0);
        const qint64 span=before.toMs-before.fromMs;
        const qint64 nextSpan=qint64(qBound(2.0,double(span)/data.series.intervalMs+delta,5000.0)*data.series.intervalMs);
        const qint64 anchor=before.fromMs+qint64(fraction*span);range_={anchor-qint64(fraction*nextSpan),anchor+qint64((1-fraction)*nextSpan)};
        count_=qBound(20.0,double(nextSpan)/data.series.intervalMs,200.0);if(range_!=before)publishUser(true);}
    void publishUser(bool zoom){publish();update();if(navigated)navigated(viewport(),zoom);}
    void publish(){const auto bounds=viewport();const auto plot=plotArea();
        setProperty("syncViewportFromMs",bounds.fromMs);setProperty("syncViewportToMs",bounds.toMs);
        setProperty("viewportPlotLeft",plot.left());setProperty("viewportPlotWidth",plot.width());
        setProperty("viewportPlotTop",plot.top());setProperty("viewportPlotHeight",plot.height());refreshMarker();}
    void refreshMarker(){const auto bounds=viewport();const auto* row=chartSyncCandleAt(data.series.candles,data.series.intervalMs,crosshairMs_);
        const bool valid=!covered&&bounds.valid()&&row&&crosshairMs_>=bounds.fromMs&&crosshairMs_<=bounds.toMs;
        if(!valid)crosshairMs_=0;
        setProperty("syncCrosshairMs",crosshairMs_);setProperty("syncCrosshairPrice",valid?row->close.toDouble():0);
        const auto plot=plotArea();marker_->setMarker(plot,valid?std::optional<double>(plot.left()+double(crosshairMs_-bounds.fromMs)/(bounds.toMs-bounds.fromMs)*plot.width()):std::nullopt);}
    ChartSyncMarker* marker_=nullptr;ChartSyncViewport range_;QList<double> averages_;quint64 studyCalculationCount_=0;
    qint64 crosshairMs_=0;double count_=100,dragX_=0,zoomRemainder_=0;bool dragging_=false;int wheelAxis_=0;QPointF wheelTravel_;
};
}
ChartGridView::ChartGridView(QWidget* parent):QWidget(parent){
    qRegisterMetaType<ChartSyncViewport>();setObjectName("chartGridView");freshness_=new QTimer(this);freshness_->setInterval(1000);
    connect(freshness_,&QTimer::timeout,this,&ChartGridView::refresh);layout_=new QGridLayout(this);layout_->setContentsMargins(0,0,0,0);layout_->setSpacing(8);
    for(int n=0;n<2;++n){
        Panel panel;panel.box=new QWidget(this);auto* column=new QVBoxLayout(panel.box);column->setContentsMargins(8,8,8,8);auto* header=new QHBoxLayout;
        panel.select=new QPushButton(panel.box);panel.select->setCheckable(true);panel.select->setFixedHeight(CompactButtonHeight);panel.select->setObjectName("chartGridSelect_"+QString::number(n));
        setThemeStyle(panel.select,"QPushButton{border:1px solid ${rule};color:${text};background:${bg};padding:4px;} QPushButton:checked{border:2px solid ${accent};}");
        connect(panel.select,&QPushButton::clicked,this,[this,n]{activate(n);});header->addWidget(panel.select,1);
        panel.period=new LocalComboBox(panel.box);
        panel.period->setObjectName("chartGridTimeframe_"+QString::number(n));
        panel.period->setAccessibleName(tr("%1번 차트 시간봉").arg(n+1));
        panel.period->setMinimumHeight(36);
        panel.period->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);
        for(const auto& entry:QList<QPair<QString,qint64>>{{tr("1분"),60000},{tr("5분"),300000},{tr("15분"),900000},{tr("1시간"),3600000},{tr("4시간"),14400000},{tr("1일"),86400000}})panel.period->addItem(entry.first,entry.second);
        panel.period->setCurrentIndex(2);connect(panel.period,&QComboBox::currentIndexChanged,this,[this,n]{
            const auto interval=panels_[n].period->currentData().toLongLong();if(panels_[n].interval==interval)return;
            panels_[n].interval=interval;panels_[n].viewport={};panels_[n].followsLive=true;
            static_cast<GridCandles*>(panels_[n].canvas)->resetContext();
            syncSelection(n,false,true);activate(n);syncDemand();});header->addWidget(panel.period);
        column->addLayout(header);auto* canvas=new GridCandles(panel.box);canvas->setObjectName("chartGridCanvas_"+QString::number(n));panel.canvas=canvas;
        canvas->navigated=[this,n,canvas](ChartSyncViewport range,bool zoom){
            auto& panel=panels_[n];
            panel.viewport=range;panel.followsLive=canvas->followsLive();panel.zoomBars=canvas->nominalZoomBars();
            syncViewport(n,range,zoom);
        };
        canvas->hovered=[this,n](qint64 utc){syncCrosshair(n,utc);};
        column->addWidget(panel.canvas,1);panel.status=new QLabel(panel.box);panel.status->setWordWrap(true);panel.status->setFont(KeefiFonts::font(FontRole::Caption12));column->addWidget(panel.status);
        panels_.append(panel);panel.box->hide();
    }
}
ChartGridView::~ChartGridView(){if(source_)source_->clearOwner("chart-grid");}
void ChartGridView::setSource(ChartStudyFeed* source){
    if(source_==source)return;if(source_){source_->clearOwner("chart-grid");disconnect(source_,nullptr,this,nullptr);}source_=source;
    if(source_){connect(source_,&ChartStudyFeed::changed,this,[this](const QString&,qint64){if(isVisible())refresh();});
        connect(source_,&QObject::destroyed,this,[this]{source_=nullptr;freshness_->stop();refresh();});}
    syncDemand();
}
void ChartGridView::setPanelCount(int count){
    if(count!=0&&count!=2)return;const int previous=active_;count_=count;clearSyncCrosshairs();
    if(count_>0)active_=qMin(active_,count_-1);
    for(int n=0;n<panels_.size();++n){layout_->removeWidget(panels_[n].box);panels_[n].box->setVisible(n<count_);if(n<count_)layout_->addWidget(panels_[n].box,n/2,n%2);}
    syncDemand();if(count_>0&&active_!=previous)activate(active_);
}
void ChartGridView::setActiveView(QWidget* view){
    if(activeView_==view)return;activeView_=view;clearSyncCrosshairs();
    for(auto& panel:panels_)panel.canvas->setMinimumHeight(view?qMax(220,view->minimumSizeHint().height()):220);syncDemand();
}
void ChartGridView::syncActiveGeometry(){
    if(!activeView_||!isVisible()||count_==0)return;layout_->activate();auto* slot=panels_[active_].canvas;
    activeView_->setGeometry(QRect(slot->mapTo(activeView_->parentWidget(),QPoint{}),slot->size()));activeView_->raise();
}
void ChartGridView::seed(const QStringList& identities){QStringList used;for(const auto& panel:panels_)if(!panel.identity.isEmpty())used.append(panel.identity);for(auto& panel:panels_)if(panel.identity.isEmpty())for(const auto& key:identities)if(publicSpot(key)&&!used.contains(key)){panel.identity=key;used.append(key);break;}syncDemand();}
void ChartGridView::setSelection(const QString& key,qint64 interval){
    if(!publicSpot(key))return;auto& panel=panels_[active_];if(panel.period->findData(interval)<0)return;
    const bool symbolChanged=panel.identity!=key,intervalChanged=panel.interval!=interval;
    panel.identity=key;panel.interval=interval;
    if(symbolChanged||intervalChanged){panel.viewport={};panel.followsLive=true;static_cast<GridCandles*>(panel.canvas)->resetContext();clearSyncCrosshairs();}
    {const QSignalBlocker block(panel.period);panel.period->setCurrentIndex(panel.period->findData(interval));}
    syncSelection(active_,symbolChanged,intervalChanged);syncDemand();
}
void ChartGridView::rollbackActivation(const QString& key,qint64 interval){
    if(count_<=0)return;active_=qBound(0,previousActive_,count_-1);auto& panel=panels_[active_];panel.identity=key;panel.interval=interval;
    {const QSignalBlocker block(panel.period);panel.period->setCurrentIndex(panel.period->findData(interval));}clearSyncCrosshairs();syncDemand();
}
void ChartGridView::activate(int n){
    if(n<0||n>=count_)return;
    const auto& panel=panels_[n];
    const auto saved=panel.viewport;
    const auto identity=panel.identity;
    const auto interval=panel.interval;
    const bool followsLive=panel.followsLive;
    const int zoomBars=panel.zoomBars;
    const bool switching=n!=active_;
    previousActive_=active_;active_=n;clearSyncCrosshairs();syncDemand();
    emit activated(identity,interval);
    if(!switching||active_!=n||panel.identity!=identity||panel.interval!=interval)return;
    if(followsLive)emit activeViewportFollowLiveRequested(zoomBars);
    else if(saved.valid()){panels_[n].viewport=saved;emit activeViewportSyncRequested(saved,true);}
}
void ChartGridView::setSyncOption(ChartSyncOption option,bool value){
    if(sync_.enabled(option)==value)return;sync_.set(option,value);
    if(option==ChartSyncOption::Crosshair)clearSyncCrosshairs();emit syncSettingsChanged();
}
QString ChartGridView::panelIdentity(int index)const{return index>=0&&index<panels_.size()?panels_[index].identity:QString{};}
qint64 ChartGridView::panelInterval(int index)const{return index>=0&&index<panels_.size()?panels_[index].interval:0;}
ChartSyncViewport ChartGridView::panelViewport(int index)const{
    if(index<0||index>=panels_.size())return {};
    return activeView_&&index==active_?panels_[index].viewport:static_cast<GridCandles*>(panels_[index].canvas)->viewport();
}
void ChartGridView::syncSelection(int source,bool symbolChanged,bool intervalChanged){
    if(count_!=2||!publicSpot(panels_[source].identity))return;
    for(int n=0;n<count_;++n)if(n!=source){
        auto& target=panels_[n];bool changed=false;
        if(sync_.symbol&&symbolChanged&&target.identity!=panels_[source].identity){target.identity=panels_[source].identity;changed=true;}
        if(sync_.interval&&intervalChanged&&target.interval!=panels_[source].interval){target.interval=panels_[source].interval;changed=true;
            const QSignalBlocker block(target.period);target.period->setCurrentIndex(target.period->findData(target.interval));}
        if(changed){target.viewport={};target.followsLive=true;static_cast<GridCandles*>(target.canvas)->resetContext();}
    }
}
void ChartGridView::observeActiveViewport(const ChartSyncViewport& range,bool followsLive,int zoomBars){
    if(count_!=2||!activeView_)return;
    auto& panel=panels_[active_];
    panel.viewport=range;panel.followsLive=followsLive;panel.zoomBars=zoomBars;
}
void ChartGridView::activeViewportNavigated(const ChartSyncViewport& range,bool zoom){
    if(activeView_)syncViewport(active_,range,zoom);
}
void ChartGridView::activeCrosshairHovered(qint64 utc){if(activeView_)syncCrosshair(active_,utc);}
void ChartGridView::syncViewport(int source,const ChartSyncViewport& range,bool zoom){
    if(count_!=2||!isVisible()||!range.valid()||!publicSpot(panels_[source].identity))return;
    if(!sync_.dateRange&&(!sync_.time||zoom))return;
    const int target=1-source;if(!publicSpot(panels_[target].identity))return;
    const bool exact=sync_.dateRange;const auto next=chartSyncTargetViewport(range,panelViewport(target),exact);if(!next.valid())return;
    panels_[target].viewport=next;panels_[target].followsLive=false;
    ++viewportApplyCount_;setProperty("syncViewportApplyCount",qulonglong(viewportApplyCount_));
    if(activeView_&&target==active_)emit activeViewportSyncRequested(range,exact);
    else static_cast<GridCandles*>(panels_[target].canvas)->applyViewport(range,exact);
}
void ChartGridView::syncCrosshair(int source,qint64 utc){
    if(count_!=2||!isVisible()||!sync_.crosshair||!publicSpot(panels_[source].identity))return;
    ++crosshairEventCount_;setProperty("syncCrosshairEventCount",qulonglong(crosshairEventCount_));
    for(int n=0;n<count_;++n){
        const qint64 value=n==source||!publicSpot(panels_[n].identity)?0:utc;
        if(activeView_&&n==active_)emit activeCrosshairSyncRequested(value);
        else static_cast<GridCandles*>(panels_[n].canvas)->setCrosshair(value);
    }
}
void ChartGridView::clearSyncCrosshairs(){emit activeCrosshairSyncRequested(0);for(auto& panel:panels_)static_cast<GridCandles*>(panel.canvas)->setCrosshair(0);}
void ChartGridView::syncDemand(){if(isVisible()&&count_>0&&source_)freshness_->start();else freshness_->stop();if(source_){QList<ChartStudyRequest> requests;if(isVisible())for(int n=0;n<count_;++n)if((!activeView_||n!=active_)&&publicSpot(panels_[n].identity))requests.append({WatchlistWorkspaceList::identityFromKey(panels_[n].identity),panels_[n].interval,10});source_->setDemands("chart-grid",requests);}refresh();}
void ChartGridView::refresh(){
    for(int n=0;n<panels_.size();++n){
        auto& panel=panels_[n];const auto id=WatchlistWorkspaceList::identityFromKey(panel.identity);const bool active=activeView_&&n==active_;
        const auto title=panel.identity.isEmpty()?tr("%1번 · 목록에서 종목 선택").arg(n+1):id.code+" · "+id.source;
        panel.select->setText((n==active_?tr("활성 · "):QString{})+title);panel.select->setProperty("chartIdentity",panel.identity);panel.select->setChecked(n==active_);
        auto data=source_?source_->observation(panel.identity,panel.interval):ChartStudyObservation{};
        if(data.status==tr("지원 없음")||data.status==tr("종목 확인 중"))data.series={};
        auto* canvas=static_cast<GridCandles*>(panel.canvas);const bool coverChanged=canvas->covered!=active;canvas->covered=active;canvas->setObservation(data);
        if(!active){
            if(panel.followsLive)canvas->restoreLiveViewport(panel.zoomBars);
            else if(panel.viewport.valid()&&canvas->viewport()!=panel.viewport)canvas->applyViewport(panel.viewport,true);
        }
        if(coverChanged)canvas->update();
        const auto state=panel.identity.isEmpty()?tr("칸을 선택한 뒤 종목 목록에서 골라 주세요."):
            active?tr("활성 차트 · 전체 지표"):
            data.delayed?tr("이전 완료 봉 · 지연"):data.loading?tr("완료 봉 갱신 중"):
            data.series.available?tr("공개 현물 · 완료 봉 · REST 갱신"):data.status.isEmpty()?tr("공개 봉 원천 대기"):data.status;
        panel.status->setText(state+(active?QString{}:tr(" · 기본 지표만")));
        canvas->setAccessibleDescription(panel.select->text()+" · "+panel.period->currentText()+" · "+panel.status->text());panel.status->setToolTip(data.series.sourceUrl);
    }
    syncActiveGeometry();QTimer::singleShot(0,this,&ChartGridView::syncActiveGeometry);
}
void ChartGridView::resizeEvent(QResizeEvent* e){QWidget::resizeEvent(e);syncActiveGeometry();}
void ChartGridView::showEvent(QShowEvent* e){QWidget::showEvent(e);syncDemand();}
void ChartGridView::hideEvent(QHideEvent* e){QWidget::hideEvent(e);clearSyncCrosshairs();freshness_->stop();if(source_)source_->clearOwner("chart-grid");}
}
