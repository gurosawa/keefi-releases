#include "keefi/CandleChart_p.h"
#include "keefi/MarketCandlesViewFormatting_p.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/Motion.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LocalMenuControls.h"
#include "keefi/ChartTimeAxis.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartStudies.h"
#include "keefi/ChartShareCard.h"
#include "keefi/StockChartTypes.h"
#include "keefi/ChartSync.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/MarketCandles.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/MovementDirection.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPaintEngine>
#include <QPixmap>
#include <QtMath>
#include <QMenu>
#include <QAction>
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QLineF>
#include <QPainterPath>
#include <QPolygonF>
#include <QSet>
#include <QTimeZone>
#include <QToolTip>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QDate>
#include <QLocale>
#include <QVariantAnimation>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPaintEvent>
#include <QPair>
#include <QPen>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QSize>
#include <QString>
#include <QTime>
#include <QTransform>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <iterator>

namespace keefi::market_candles_detail {
namespace {
bool sameCandle(const MarketCandle& a,const MarketCandle& b){
    return a.startsAtMs==b.startsAtMs&&a.lastStoredTickMs==b.lastStoredTickMs&&
        a.open==b.open&&a.high==b.high&&a.low==b.low&&a.close==b.close&&a.volume==b.volume;
}
bool sameForming(const std::optional<MarketCandle>& a,const std::optional<MarketCandle>& b){
    return a.has_value()==b.has_value()&&(!a||sameCandle(*a,*b));
}
bool sameRows(const QList<MarketCandle>& a,const QList<MarketCandle>& b){
    return a.isSharedWith(b)||(a.size()==b.size()&&std::equal(a.cbegin(),a.cend(),b.cbegin(),sameCandle));
}
bool sameAutoPreviewBars(const QList<ChartDrawingBar>& previous,const QList<ChartDrawingBar>& current){
    return previous.isSharedWith(current)||
        std::equal(previous.cbegin(),previous.cend(),current.cbegin(),current.cend(),
            [](const ChartDrawingBar& a,const ChartDrawingBar& b){
                return a.candle.startsAtMs==b.candle.startsAtMs&&a.completesAtMs==b.completesAtMs&&
                    a.sourceCompleted==b.sourceCompleted&&a.connectsPrevious==b.connectsPrevious&&
                    a.candle.open==b.candle.open&&a.candle.high==b.candle.high&&
                    a.candle.low==b.candle.low&&a.candle.close==b.candle.close;
            });
}
bool sameRowTimes(const QList<MarketCandle>& a,const QList<MarketCandle>& b){
    return a.isSharedWith(b)||(a.size()==b.size()&&std::equal(a.cbegin(),a.cend(),b.cbegin(),
        [](const auto& left,const auto& right){return left.startsAtMs==right.startsAtMs;}));
}
bool sameTimeline(const ChartStudyTimeline& a,const ChartStudyTimeline& b){
    if(a.projectionCovered!=b.projectionCovered||a.adjustedComparable!=b.adjustedComparable||
        a.projectionReason!=b.projectionReason||a.comparisonReason!=b.comparisonReason||
        a.nextStartsAtMs!=b.nextStartsAtMs)return false;
    return a.rows.isSharedWith(b.rows)||(a.rows.size()==b.rows.size()&&
        std::equal(a.rows.cbegin(),a.rows.cend(),b.rows.cbegin(),[](const auto& left,const auto& right){
            return left.startsAtMs==right.startsAtMs&&left.completesAtMs==right.completesAtMs&&
                left.tradingDate==right.tradingDate&&left.session==right.session&&
                left.connectsPrevious==right.connectsPrevious&&left.eligible==right.eligible&&
                left.missingSlotsBefore==right.missingSlotsBefore&&left.exclusions==right.exclusions&&left.reason==right.reason;
        }));
}
bool samePriceLines(const QList<ChartPriceLine>& a,const QList<ChartPriceLine>& b){
    return a.isSharedWith(b)||(a.size()==b.size()&&std::equal(a.cbegin(),a.cend(),b.cbegin(),
        [](const auto& left,const auto& right){
            return left.provider==right.provider&&left.accountScope==right.accountScope&&left.asset==right.asset&&
                left.currency==right.currency&&left.price==right.price&&left.sourceUrl==right.sourceUrl&&
                left.receivedAtMs==right.receivedAtMs&&left.kind==right.kind&&left.evidenceNumber==right.evidenceNumber;
        }));
}
// A pointer can only express a price to within one pixel. Round to that unit so
// a placed line reads 105,204,000 rather than 105,204,123.456789.
QString pixelPrice(double price,double pricePerPixel){
    if(!std::isfinite(price)||price<=0||!std::isfinite(pricePerPixel)||pricePerPixel<=0)return drawnPrice(price);
    const auto exponent=int(std::floor(std::log10(pricePerPixel)));const auto unit=std::pow(10.0,exponent);
    const auto rounded=std::round(price/unit)*unit;if(!(rounded>0))return drawnPrice(price);
    auto text=QString::number(rounded,'f',qBound(0,-exponent,110));
    if(text.contains('.')){while(text.endsWith('0'))text.chop(1);if(text.endsWith('.'))text.chop(1);}
    return text;
}
// 1, 2, 2.5 and 5 × 10^n steps: the price axis reads in round numbers.
double niceStep(double raw){
    if(!std::isfinite(raw)||raw<=0)return 0;
    const auto exponent=std::floor(std::log10(raw));const auto unit=std::pow(10.0,exponent);const auto fraction=raw/unit;
    return (fraction<=1?1:fraction<=2?2:fraction<=2.5?2.5:fraction<=5?5:10)*unit;
}
QString tickPrice(double value,double step){
    const auto decimals=qBound(0,int(-std::floor(std::log10(step)))+(std::abs(step/std::pow(10.0,std::floor(std::log10(step)))-2.5)<1e-9?1:0),12);
    auto text=QString::number(value,'f',decimals);
    if(text.contains('.')){while(text.endsWith('0'))text.chop(1);if(text.endsWith('.'))text.chop(1);}
    return priceAmount(text);
}
QString formatFibRatio(double ratio){return QLocale::c().toString(ratio,'g',4);}
QString formatFibPrice(double value,const QString& currency){
    if(!std::isfinite(value)||value<=0)return {};
    const int decimals=(value>=10000.0||(currency==QStringLiteral("KRW")&&value>=1000.0))?0
        :value>=1000.0?2:value>=1.0?2:qBound(2,3-int(std::floor(std::log10(value))),8);
    return priceAmount(QString::number(value,'f',decimals));
}
double priceToY(double price,double low,double high,const QRectF& area){
    return area.bottom()-(price-low)/(high-low)*area.height();
}
constexpr qreal FibLabelHeight=16.0;
constexpr qreal FibLabelGap=18.0;
constexpr int FibLabelConnectorWidth=3;
std::optional<qreal> nearestFibLabelCentre(qreal preferred,qreal top,qreal bottom,const QList<qreal>& occupied){
    std::optional<qreal> result;
    const auto consider=[&](qreal end){
        if(top>end)return;
        const auto y=qBound(top,preferred,end);
        if(!result||std::abs(y-preferred)<std::abs(*result-preferred))result=y;
    };
    for(const auto y:occupied){
        consider(qMin(bottom,y-FibLabelGap));
        top=qMax(top,y+FibLabelGap);
    }
    consider(bottom);
    return result;
}
void paintTextBadge(QPainter& p,const QPointF& anchorRightBottom,const QString& text,const QColor& ink,const QColor& bg,const QRectF& bounds,bool alignRight){
    if(text.isEmpty()||bounds.width()<=4||bounds.height()<=4)return;
    const QFontMetricsF fm(p.font());
    const qreal w=fm.horizontalAdvance(text)+8.0,h=qMax(15.0,fm.height()+2.0);
    const qreal x=qBound(bounds.left(),alignRight?anchorRightBottom.x()-w:anchorRightBottom.x(),qMax(bounds.left(),bounds.right()-w));
    const qreal y=qBound(bounds.top(),anchorRightBottom.y()-h,qMax(bounds.top(),bounds.bottom()-h));
    const QRectF box(x,y,qMin(w,bounds.width()),h);
    auto fill=bg;fill.setAlphaF(.92);p.fillRect(box,fill);
    p.setPen(ink);p.drawText(box.adjusted(4,0,-4,0),(alignRight?Qt::AlignRight:Qt::AlignLeft)|Qt::AlignVCenter,text);
}
// Time axis labels at whole KST clock or calendar boundaries, about every 110px.
qint64 timeTickStep(double msPerPixel,qint64 interval){
    constexpr qint64 Minute=60000,Hour=60*Minute,Day=24*Hour;
    for(const qint64 step:{5*Minute,15*Minute,30*Minute,Hour,2*Hour,3*Hour,4*Hour,6*Hour,12*Hour,Day,2*Day,3*Day,7*Day,14*Day,30*Day,91*Day,182*Day,364*Day})
        if(step>=interval&&step>=msPerPixel*110)return step;
    return 364*Day;
}
QString timeTickText(qint64 at,qint64 step){
    const auto local=QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul"));
    if(step>=30LL*86400000)return local.date().month()==1?local.toString("yyyy"):local.toString("yy/MM");
    if(local.time()==QTime(0,0)||step>=86400000)return local.toString("MM/dd");
    return local.toString("HH:mm");
}
// previousAt is the preceding visible observation (0 for none). Daily candles open at 09:00 KST,
// and stock sessions skip days, so calendar steps compare dates rather than exact midnights.
bool timeTickAt(qint64 at,qint64 previousAt,qint64 step,qint64 interval){
    constexpr qint64 Seoul=9LL*3600000,Day=86400000;const auto local=at+Seoul;
    if(step<Day)return local%step==0;
    if(interval<Day&&local%Day!=0)return false;
    const auto date=QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).date();
    const auto previous=previousAt>0?QDateTime::fromMSecsSinceEpoch(previousAt,QTimeZone("Asia/Seoul")).date():QDate{};
    if(step>=30*Day){const int months=step>=364*Day?12:step>=182*Day?6:step>=91*Day?3:1;
        return (!previous.isValid()||previous.month()!=date.month()||previous.year()!=date.year())&&(date.month()-1)%months==0&&(previous.isValid()||date.day()==1);}
    if(step>=7*Day)return previous.isValid()?(date.dayOfWeek()<previous.dayOfWeek()||previous.daysTo(date)>=7)&&(step==7*Day||(date.weekNumber()%2)==0):date.dayOfWeek()==1;
    return date.toJulianDay()%(step/Day)==0;
}
}

CandleChart::CandleChart(QWidget* p):QWidget(p){
        setObjectName("marketCandlesChart");setMinimumHeight(270);setMinimumWidth(0);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
        setProperty("liveTradePrice",QString{});setProperty("liveTradeAtMs",qint64(0));
        setAccessibleName(tr("완료된 15분 캔들 · 가로 휠과 방향키로 이동, 세로 휠로 확대·축소"));grabGesture(Qt::PinchGesture);
        bindMovementPresentation(this,[this]{refresh();});
        bindThemePresentation(this,[this]{refresh();});
        setProperty("autoPreviewActive",false);setProperty("autoPreviewProgress",1.0);
        setProperty("drawingLabelTexts",QStringList{});
        setProperty("drawingOffscreenTopLabel",QString{});
        setProperty("drawingOffscreenBottomLabel",QString{});
        syncMarker_=new ChartSyncMarker(this);
        autoPreviewAnim_=expoMotion(this,600);
        autoPreviewAnim_->setObjectName("chartAutoPreviewMotion");
        connect(autoPreviewAnim_,&QVariantAnimation::valueChanged,this,[this](const QVariant& value){
            autoPreviewProgress_=qBound(0.0,value.toDouble(),1.0);
            setProperty("autoPreviewProgress",autoPreviewProgress_);
            hoverRegion_={};QWidget::update();
        });
        connect(autoPreviewAnim_,&QAbstractAnimation::finished,this,[this]{
            autoPreviewProgress_=1.0;
            setProperty("autoPreviewProgress",1.0);
            hoverRegion_={};QWidget::update();
        });
    }

void CandleChart::setObservation(const QList<MarketCandle>& rows,const std::optional<MarketCandle>& forming){
        if(rows.isEmpty()&&!forming)setLiveTradePrice({},0);
        if(!observationContextDirty_&&sameRows(rows_,rows)&&sameForming(forming_,forming)){
            // Keep the existing paced history-retry opportunity without rebuilding
            // geometry, republishing an unchanged viewport, or scheduling a paint.
            requestOlderIfNeeded(shareViewport());return;
        }
        observationContextDirty_=false;
        // The viewport is anchored to an actual observed timestamp, not an index.
        // Prepending history, replacing a forming bar or appending a live close
        // therefore cannot move a historical bar's horizontal screen coordinate.
        const auto oldAxis=timeAxis();const auto oldBounds=navigationBounds(oldAxis);
        const auto oldVisible=visible();const bool preserve=offset_>.000001&&!rows_.isEmpty()&&oldAxis.available;
        const auto anchor=preserve?(oldVisible.isEmpty()?rows_.first().startsAtMs:oldVisible.first().startsAtMs):qint64(0);
        const auto displacement=preserve?oldAxis.coordinateAt(anchor).value_or(0)-oldBounds.first:0;
        const bool sameAxis=sameRowTimes(rows_,rows)&&
            (forming_?forming_->startsAtMs:0)==(forming?forming->startsAtMs:0);
        rows_=rows;forming_=forming;visibleRowsCache_.reset();geometryCache_.reset();
        if(!sameAxis)timeAxisCache_.reset();
        if(rows.isEmpty()&&!forming){offset_=0;hover_=-1;setAccessibleDescription({});}
        else{
            if(preserve){const auto axis=timeAxis();if(const auto at=axis.coordinateAt(anchor))
                offset_=latestRight(axis)-(*at-displacement+viewSpan(axis));}
            clampOffset();setAccessibleDescription(tr("실제 완료 캔들 %1개 · 빈 시간은 채우지 않음").arg(rows.size()));
        }
        publishViewport();refresh();
    }

void CandleChart::setLiveTradePrice(const QString& price,qint64 tradeAtMs){
        bool ok=false;const auto value=price.toDouble(&ok);
        const auto next=ok&&std::isfinite(value)&&value>0&&tradeAtMs>0?price:QString{};
        const auto nextAt=next.isEmpty()?qint64(0):tradeAtMs;
        if(liveTradePrice_==next&&liveTradeAtMs_==nextAt)return;
        setProperty("liveTradePrice",next);setProperty("liveTradeAtMs",nextAt);
        if(liveTradePrice_==next){liveTradeAtMs_=nextAt;return;}
        const auto plot=plotGeometry();const auto before=liveTradeRegion(plot,liveTradePrice_);
        liveTradePrice_=next;liveTradeAtMs_=nextAt;
        const auto region=before.united(liveTradeRegion(plot,next));
        if(staticDirty_||staticLayer_.isNull())QWidget::update();
        else if(!region.isEmpty())QWidget::update(region.toAlignedRect());
    }

void CandleChart::resetViewport(){syncRange_={};setSyncedCrosshair(0);offset_=0;projectionSlots_=drawingPadding_=edgePadding_=0;wheelAxis_=0;wheelTravel_={};zoomRemainder_=0;hover_=-1;similarityTableHovered_=false;similarityHoverRow_=-1;priceScale_=1;axisDragging_=false;publishViewport();refresh();}

void CandleChart::returnToLive(bool user){syncRange_={};offset_=0;edgePadding_=0;const auto axis=timeAxis();projectionSlots_=computedProjectionSlots(axis);drawingPadding_=computedDrawingPadding(axis);publishViewport();refresh();if(user)publishUserNavigation(false);}

bool CandleChart::navigateToTime(qint64 at,bool user){
        const auto axis=timeAxis();if(!axis.available||at<=0)return false;
        const auto found=std::upper_bound(rows_.cbegin(),rows_.cend(),at,[](qint64 time,const auto& row){return time<row.startsAtMs;});
        if(found==rows_.cbegin())return false;const auto& row=*std::prev(found);
        if(at>=row.startsAtMs+intervalMs_&&at!=row.startsAtMs)return false;
        const auto coordinate=axis.coordinateAt(row.startsAtMs);if(!coordinate)return false;
        syncRange_={};offset_=latestRight(axis)-*coordinate-viewSpan(axis)/2;clampOffset();publishViewport();refresh();if(user)publishUserNavigation(false);return true;
    }

void CandleChart::setPriceLines(const QList<ChartPriceLine>& rows){if(samePriceLines(lines_,rows))return;lines_=rows;refresh();}

void CandleChart::setCandleEvidenceNumber(int number){const auto next=qMax(0,number);if(candleEvidenceNumber_==next)return;candleEvidenceNumber_=next;refresh();}

void CandleChart::setContext(qint64 interval,const QString& currency,const QString& instrument){
        const auto name=instrument+" · "+tr("완료된 %1 캔들 · 방향키로 이동, 더하기와 빼기로 확대·축소").arg(timeframeLabel(interval));
        if(intervalMs_==interval&&currency_==currency&&accessibleName()==name)return;
        if(intervalMs_!=interval){syncRange_={};setSyncedCrosshair(0);timeAxisCache_.reset();visibleRowsCache_.reset();}
        intervalMs_=interval;currency_=currency;setLiveTradePrice({},0);observationContextDirty_=true;
        setAccessibleName(name);
        refresh();
    }

void CandleChart::setStudies(const ChartStudySet& studies,const QSet<QString>& layers){
        if(layers_.contains("ma")!=layers.contains("ma")||layers_.contains("bb")!=layers.contains("bb"))
            priceStudyBounds_.reset();
        studies_=studies;layers_=layers;
        if(similarityHoverRow_>=studies_.similarity.paths.size())similarityHoverRow_=-1;
        // A historical viewport owns its scale. A worker clearing/reapplying
        // latest-reference projections must not resize or move that viewport.
        if(!syncRange_.valid()&&offset_<.000001)projectionSlots_=computedProjectionSlots(timeAxis());
        clampOffset();publishViewport();refresh();
    }

void CandleChart::setCoinFlowStudies(const CoinFlowStudySet& studies){coinFlow_=studies;refresh();}

void CandleChart::setStudyColors(const QMap<QString,QColor>& colors) {
    if (colors==studyColors_) return;
    studyColors_=colors;
    refresh();
}
void CandleChart::setExtendedStudies(const ExtendedChartStudySet& studies){
        if(!extended_.maLines.isSharedWith(studies.maLines)
            ||!extended_.bb.points.isSharedWith(studies.bb.points)
            ||extended_.bb.state.available!=studies.bb.state.available)
            priceStudyBounds_.reset();
        extended_=studies;refresh();
    }

void CandleChart::setLevels(const ExtendedLevelSet& levels){levels_=levels;refresh();}

int CandleChart::pricePlotHeight()const{return int(plotGeometry().area.height());}
CandleChart::StudyViewport CandleChart::studyViewport() const {
    const auto plot = plotGeometry();
    return {plot.axis, plot.bounds, plot.area};
}

void CandleChart::setStockTimeline(bool stock,const ChartStudyTimeline& timeline){
        if(stock_==stock&&sameTimeline(timeline_,timeline))return;
        stock_=stock;timeline_=timeline;timeAxisCache_.reset();visibleRowsCache_.reset();observationContextDirty_=true;
        if(stock){syncRange_={};setSyncedCrosshair(0);}refresh();
    }

void CandleChart::setDrawingFrame(ChartDrawings* model,const ChartDrawingFrame& frame){
        const bool contextChanged=drawingModel_!=model||drawingFrame_.identity!=frame.identity||drawingFrame_.currency!=frame.currency||drawingFrame_.intervalMs!=frame.intervalMs||drawingFrame_.contextGeneration!=frame.contextGeneration;
        if(contextChanged){cancelDrawingGesture();selectedDrawing_.clear();}
        if(!autoPreview_.isEmpty()&&(contextChanged||!model||model->locked()||!model->visible()
            ||autoPreviewGeneration_!=frame.contextGeneration||!sameAutoPreviewBars(autoPreviewBars_,frame.bars))){
            clearAutoPreview();
            if(autoPreviewCancelled)autoPreviewCancelled();
        }
        drawingModel_=model;drawingFrame_=frame;validSnapFrame_.reset();
        const bool hadVisibleFib=hasVisibleFib_;
        hasVisibleFib_=false;
        if(drawingModel_){
            const auto shapes=drawingModel_->drawings();
            if(std::none_of(shapes.cbegin(),shapes.cend(),[this](const auto& d){return d.id==selectedDrawing_;}))selectedDrawing_.clear();
            hasVisibleFib_=drawingModel_->visible()&&std::any_of(shapes.cbegin(),shapes.cend(),[](const ChartDrawing& shape){
                return shape.visible&&(shape.kind==ChartDrawingKind::FibRetracement||shape.kind==ChartDrawingKind::FibExtension);
            });
        }
        bool publish=hadVisibleFib!=hasVisibleFib_;
        // As with study projections, freeze the right-edge allowance while
        // browsing history so toggling/editing drawings cannot move that view.
        if(!syncRange_.valid()&&offset_<.000001){
            const auto padding=computedDrawingPadding(timeAxis());
            if(padding!=drawingPadding_){drawingPadding_=padding;clampOffset();publish=true;}
        }
        if(publish)publishViewport();
        refresh();
    }

void CandleChart::setDrawingTool(ChartDrawingTool tool){if(tool_!=tool){cancelDrawingGesture();tool_=tool;if(drawingUiChanged)drawingUiChanged();}refresh();}

ChartDrawingTool CandleChart::drawingTool()const{return tool_;}

bool CandleChart::drawingInProgress()const{return !draft_.isEmpty()||!dragDrawing_.isEmpty()||!moveDrawing_.isEmpty();}

QString CandleChart::selectedDrawing()const{return selectedDrawing_;}

void CandleChart::cancelDrawingGesture(){draft_.clear();preview_.reset();dragDrawing_.clear();dragAnchor_=-1;moveDrawing_.clear();moveOriginal_.clear();movePreview_.clear();dragging_=false;unsetCursor();refresh();}

void CandleChart::setAutoPreview(const QList<DrawingPreviewItem>& items,const ChartDrawingFrame& frame){
        if(items.isEmpty()){clearAutoPreview();return;}
        cancelDrawingGesture();
        autoPreview_=items;
        autoPreviewGeneration_=frame.contextGeneration;
        autoPreviewBars_=frame.bars;
        if(autoPreviewAnim_)autoPreviewAnim_->stop();
        if(!motionEnabled()){
            autoPreviewProgress_=1.0;
        }else{
            autoPreviewProgress_=0.0;
            if(autoPreviewAnim_)autoPreviewAnim_->start();
        }
        setProperty("autoPreviewActive",true);
        setProperty("autoPreviewProgress",autoPreviewProgress_);
        hoverRegion_={};QWidget::update();
    }

void CandleChart::setAutoPreviewItemVisible(const QString& key,bool visible){
        bool changed=false;
        for(auto& item:autoPreview_)if(item.candidate.key==key&&item.visible!=visible){item.visible=visible;changed=true;}
        if(changed){hoverRegion_={};QWidget::update();}
    }

void CandleChart::clearAutoPreview(){
        const bool wasActive=!autoPreview_.isEmpty()||(autoPreviewAnim_&&autoPreviewAnim_->state()!=QAbstractAnimation::Stopped);
        if(autoPreviewAnim_)autoPreviewAnim_->stop();
        autoPreview_.clear();autoPreviewGeneration_=0;autoPreviewBars_.clear();autoPreviewProgress_=1.0;
        setProperty("autoPreviewActive",false);setProperty("autoPreviewProgress",1.0);
        if(wasActive){hoverRegion_={};QWidget::update();}
    }

void CandleChart::finishAutoPreview(){
        if(autoPreview_.isEmpty())return;
        if(autoPreviewAnim_)autoPreviewAnim_->stop();
        autoPreviewProgress_=1.0;
        setProperty("autoPreviewProgress",1.0);
        hoverRegion_={};QWidget::update();
    }

bool CandleChart::autoPreviewActive()const{return !autoPreview_.isEmpty();}

bool CandleChart::autoPreviewRunning()const{return !autoPreview_.isEmpty()&&autoPreviewAnim_&&autoPreviewAnim_->state()==QAbstractAnimation::Running;}

QList<DrawingPreviewItem> CandleChart::autoPreviewItems()const{return autoPreview_;}

ChartShareViewport CandleChart::shareViewport()const{
        ChartShareViewport result;
        // visible() consists only of completed rows plus the optional forming row.
        for(const auto& row:visible())if(!forming_||row.startsAtMs!=forming_->startsAtMs){
            if(!result.fromMs)result.fromMs=row.startsAtMs;result.toMs=row.startsAtMs;
        }
        return result;
    }

ChartSyncViewport CandleChart::syncViewport()const{
        return stock_?ChartSyncViewport{}:syncPublishedViewport_;
    }

bool CandleChart::followsLive()const{return !syncRange_.valid()&&offset_<.000001;}

int CandleChart::nominalZoomBars()const{return count_;}

void CandleChart::restoreLiveViewport(int zoomBars){
        const auto next=qBound(20,zoomBars,200);
        if(followsLive()&&count_==next)return;
        count_=next;
        returnToLive(false);
    }

void CandleChart::applySyncedViewport(const ChartSyncViewport& range,bool matchRange){
        if(stock_)return;const auto next=chartSyncTargetViewport(range,syncViewport(),matchRange);if(!next.valid())return;
        syncRange_=next;if(matchRange&&intervalMs_>0)count_=int(qBound(20.0,double(next.toMs-next.fromMs)/intervalMs_,200.0));hover_=-1;publishViewport();refresh();
        setProperty("syncViewportApplyCount",property("syncViewportApplyCount").toULongLong()+1);
    }

void CandleChart::setSyncedCrosshair(qint64 utc){syncCrosshairMs_=utc;refreshSyncedMarker();}

void CandleChart::zoom(int delta){if(syncRange_.valid()){zoomSynced(delta,1);return;}const auto before=syncViewport();count_=std::clamp(count_+delta,20,200);clampOffset();publishViewport();refresh();if(syncViewport()!=before)publishUserNavigation(true);}

void CandleChart::resetZoom(){count_=60;priceScale_=1;returnToLive(false);publishUserNavigation(true);}

void CandleChart::zoomAt(int delta,const QPointF& position){
        const auto before=plotGeometry();if(!before.available||!delta)return;
        const auto fraction=qBound(0.0,(position.x()-before.area.left())/before.area.width(),1.0);
        if(syncRange_.valid()){zoomSynced(delta,fraction);return;}
        const auto anchor=before.bounds.first+fraction*(before.bounds.second-before.bounds.first);
        count_=std::clamp(count_+delta,20,200);
        setRightEdge(anchor+(1-fraction)*viewSpan(before.axis),before.axis);
        // Price gutter width can change with the newly visible extrema. Correct
        // the same pointer pixel once more using that actual new plot width.
        const auto after=plotGeometry();if(after.available){
            const auto ratio=qBound(0.0,(position.x()-after.area.left())/after.area.width(),1.0);
            setRightEdge(anchor+(1-ratio)*viewSpan(after.axis),after.axis);}
        publishViewport();refresh();publishUserNavigation(true);
    }

void CandleChart::setRightEdge(double right,const ChartTimeAxis& axis){
        const auto delta=latestRight(axis)-right;
        if(delta<0){edgePadding_-=delta;offset_=0;}else offset_=delta;
        clampOffset();
    }

void CandleChart::pan(double delta){
        if(syncRange_.valid()&&(rows_.isEmpty()||intervalMs_<=0))return;
        const auto before=syncViewport();
        if(syncRange_.valid()&&!rows_.isEmpty()){
            const qint64 span=syncRange_.toMs-syncRange_.fromMs,first=rows_.first().startsAtMs-intervalMs_/2;
            const auto axis=timeAxis();const qint64 last=rows_.first().startsAtMs+qint64(std::llround(latestRight(axis)*intervalMs_));
            const auto move=qint64(qBound(-5000.0,delta,5000.0)*intervalMs_);
            const qint64 right=qBound(qMin(first+span,last),syncRange_.toMs-move,last);syncRange_={right-span,right};
        }else{offset_+=delta;clampOffset();}
        publishViewport();refresh();if(syncViewport()!=before)publishUserNavigation(false);
    }

void CandleChart::setChartStyle(const QString& style){const auto next=style=="bars"||style=="line"?style:QStringLiteral("candles");if(style_!=next){style_=next;refresh();}}

QString CandleChart::chartStyle()const{return style_;}

double CandleChart::priceScale()const{return priceScale_;}

void CandleChart::setPriceScale(double scale){const auto next=std::clamp(scale,MinPriceScale,MaxPriceScale);if(std::abs(next-priceScale_)>1e-9){priceScale_=next;publishViewport();refresh();}}

void CandleChart::resetPriceScale(){setPriceScale(1);}

bool CandleChart::onPriceAxis(const QPointF& pos)const{const auto plot=plotGeometry();
        return plot.available&&!plot.rows.isEmpty()&&pos.x()>plot.area.right()&&pos.x()<=width()&&pos.y()>=plot.area.top()&&pos.y()<=plot.area.bottom();}

bool CandleChart::event(QEvent* event){
        if(event->type()==QEvent::FontChange||event->type()==QEvent::ApplicationFontChange||event->type()==QEvent::StyleChange)refresh();
        if(event->type()==QEvent::NativeGesture){auto* gesture=static_cast<QNativeGestureEvent*>(event);
            if(gesture->gestureType()==Qt::ZoomNativeGesture&&plotGeometry().area.contains(gesture->position())){
                zoomRemainder_-=gesture->value()*count_;const auto steps=int(zoomRemainder_);zoomRemainder_-=steps;
                zoomAt(steps,gesture->position());event->accept();return true;}}
        if(event->type()==QEvent::Gesture){auto* gestures=static_cast<QGestureEvent*>(event);
            if(auto* pinch=static_cast<QPinchGesture*>(gestures->gesture(Qt::PinchGesture))){
                // Native recognizers may retain the previous scale ratio on
                // their final or centre-only event. Apply each scale update once.
                if(pinch->state()!=Qt::GestureFinished&&pinch->state()!=Qt::GestureCanceled&&
                    (pinch->changeFlags()&QPinchGesture::ScaleFactorChanged)&&
                    pinch->lastScaleFactor()>0&&pinch->scaleFactor()>0){
                    zoomRemainder_+=count_*(pinch->lastScaleFactor()/pinch->scaleFactor()-1);
                    const auto steps=int(zoomRemainder_);zoomRemainder_-=steps;zoomAt(steps,pinch->centerPoint());}
                gestures->accept(pinch);return true;}}
        return QWidget::event(event);
    }

void CandleChart::wheelEvent(QWheelEvent* event){
        const auto plot=plotGeometry();
        if(!plot.available||!plot.area.contains(event->position())){event->ignore();return;}
        const auto pixel=event->pixelDelta();const bool pixels=!pixel.isNull();
        QPointF delta=pixels?QPointF(pixel):QPointF(event->angleDelta());
        // A mouse without a horizontal wheel: Shift + wheel moves through time, as in common charts.
        if((event->modifiers()&Qt::ShiftModifier)&&delta.x()==0&&delta.y()!=0)delta=QPointF(delta.y(),0);
        if(event->phase()==Qt::ScrollBegin){wheelAxis_=0;wheelTravel_={};}
        if(delta.isNull()){if(event->phase()==Qt::ScrollEnd){wheelAxis_=0;wheelTravel_={};}event->accept();return;}
        wheelTravel_+=delta;
        // A trackpad gesture keeps its initially dominant axis. A diagonal
        // horizontal gesture can never accidentally zoom on a later event.
        const bool phased=event->phase()!=Qt::NoScrollPhase;
        if(!phased||!wheelAxis_){const auto direction=phased?wheelTravel_:delta;
            wheelAxis_=std::abs(direction.x())>=std::abs(direction.y())&&direction.x()!=0?1:2;}
        if(wheelAxis_==1){
            const auto slotDelta=pixels?delta.x()/plot.area.width()*(plot.bounds.second-plot.bounds.first):delta.x()/120.0*10.0;
            pan(slotDelta);
        }else{
            zoomRemainder_-=pixels?delta.y()/8.0:delta.y()/12.0;
            const auto steps=int(zoomRemainder_);zoomRemainder_-=steps;zoomAt(steps,event->position());
        }
        if(!phased||event->phase()==Qt::ScrollEnd){wheelAxis_=0;wheelTravel_={};}
        event->accept();
    }

void CandleChart::keyPressEvent(QKeyEvent* e){
        if(e->key()==Qt::Key_Control){
            lastModifiers_|=Qt::ControlModifier;
            if(hoverInside_){
                const auto plot=plotGeometry();
                updateMagnetSnap(plot);
                if(plot.available)hoverUpdate(plot.area,std::numeric_limits<double>::quiet_NaN());
            }
            QWidget::keyPressEvent(e);return;
        }
        if(e->key()==Qt::Key_Escape){
            if(autoPreviewActive()){
                if(autoPreviewRunning()||autoPreviewProgress_<1.0)finishAutoPreview();
                else{clearAutoPreview();if(autoPreviewCancelled)autoPreviewCancelled();}
                e->accept();return;
            }
            cancelDrawingGesture();setDrawingTool(ChartDrawingTool::Crosshair);e->accept();
        }
        else if((e->key()==Qt::Key_Delete||e->key()==Qt::Key_Backspace)&&drawingModel_&&!selectedDrawing_.isEmpty()){
            drawingModel_->remove(selectedDrawing_);if(drawingUiChanged)drawingUiChanged();e->accept();}
        else if(e->key()==Qt::Key_Left)pan(10);else if(e->key()==Qt::Key_Right)pan(-10);
        else if(e->key()==Qt::Key_End)returnToLive();else if(e->key()==Qt::Key_Home)pan(std::numeric_limits<double>::max()/4);
        else if(e->key()==Qt::Key_Plus||e->key()==Qt::Key_Equal)zoom(-10);else if(e->key()==Qt::Key_Minus)zoom(10);else QWidget::keyPressEvent(e);
    }

void CandleChart::keyReleaseEvent(QKeyEvent* e){
        if(e->key()==Qt::Key_Control){
            lastModifiers_&=~Qt::ControlModifier;
            if(hoverInside_){
                const auto plot=plotGeometry();
                updateMagnetSnap(plot);
                if(plot.available)hoverUpdate(plot.area,std::numeric_limits<double>::quiet_NaN());
            }
        }
        QWidget::keyReleaseEvent(e);
    }

void CandleChart::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);magnetGeometryDirty_=true;publishViewport();}

void CandleChart::leaveEvent(QEvent*){
        hoverRegion_={};hover_=-1;hoverInside_=false;
        similarityTableHovered_=false;similarityHoverRow_=-1;
        lastModifiers_=Qt::NoModifier;
        magnetSnapPos_.reset();magnetSnapField_.clear();magnetSnapPrice_.clear();magnetStrongActive_=false;
        setProperty("magnetSnapActive",false);setProperty("magnetStrongActive",false);
        setProperty("magnetSnapField",QString{});setProperty("magnetSnapPrice",QString{});setProperty("magnetSnapPos",QPointF{});
        setProperty("similarityHoverRow",-1);
        unsetCursor();
        if(crosshairHovered)crosshairHovered(0);QToolTip::hideText();update();
    }

void CandleChart::mousePressEvent(QMouseEvent* e){
        if(e->button()!=Qt::LeftButton)return;
        lastModifiers_=e->modifiers();
        if(autoPreviewRunning()||(autoPreviewActive()&&autoPreviewProgress_<1.0)){
            finishAutoPreview();e->accept();return;
        }
        const auto plot=plotGeometry();
        // Dragging the price axis changes the vertical scale; double-click on it restores automatic.
        if(onPriceAxis(e->position())){axisDragging_=true;axisPressY_=e->position().y();axisPressScale_=priceScale_;setCursor(Qt::SizeVerCursor);e->accept();return;}
        if(tool_==ChartDrawingTool::Crosshair&&plot.available){
            const auto sim=similarityTableLayout(plot);
            if(sim.visible&&sim.expanded){
                for(int i=0;i<sim.rowRects.size()&&i<studies_.similarity.paths.size();++i){
                    if(sim.rowRects[i].contains(e->position())){
                        const qint64 targetMs=studies_.similarity.paths[i].fromMs;
                        similarityHoverRow_=-1;similarityTableHovered_=false;
                        setProperty("similarityHoverRow",-1);
                        unsetCursor();
                        navigateToTime(targetMs,true);
                        e->accept();return;
                    }
                }
                if(sim.box.contains(e->position())){e->accept();return;}
            }else if(sim.visible&&sim.collapsedRect.contains(e->position())){
                similarityTableHovered_=true;
                hoverUpdate(plot.area,std::numeric_limits<double>::quiet_NaN());
                e->accept();return;
            }
        }
        if(drawingModel_&&drawingModel_->visible()&&plot.available){
            if(tool_!=ChartDrawingTool::Crosshair&&!drawingModel_->locked()){
                const auto point=pointerAnchor(e->position(),plot,e->modifiers());if(!point){feedback(tr("실제로 완료된 봉 위에서 가격·시각을 선택하세요."));return;}
                const auto kind=tool_==ChartDrawingTool::Horizontal?ChartDrawingKind::Horizontal:tool_==ChartDrawingTool::Trend?ChartDrawingKind::Trend:ChartDrawingKind::Measure;
                QString created;
                if(kind==ChartDrawingKind::Horizontal){QString error;created=drawingModel_->create(kind,{*point},drawingFrame_,&error);if(created.isEmpty())feedback(error);}
                else if(draft_.isEmpty()){draft_.append(*point);gestureGeneration_=drawingFrame_.contextGeneration;preview_=point;feedback(tr("두 번째 완료 봉을 선택하세요. Esc로 취소할 수 있어요."));}
                else {const auto points=QList<ChartDrawingPoint>{draft_.first(),*point};const bool current=gestureGeneration_==drawingFrame_.contextGeneration;draft_.clear();preview_.reset();
                    QString error;created=current?drawingModel_->create(kind,points,drawingFrame_,&error):QString{};if(created.isEmpty())feedback(error.isEmpty()?tr("선택이 바뀌어 그림을 취소했어요."):error);}
                if(!created.isEmpty()){
                    // One placement per tool choice, as in common chart tools. Shift keeps the tool.
                    selectDrawing(created);if(!(e->modifiers()&Qt::ShiftModifier))setDrawingTool(ChartDrawingTool::Crosshair);
                }
                refresh();e->accept();return;
            }
            const auto hit=hitDrawing(e->position(),plot);if(!hit.first.isEmpty()){
                selectDrawing(hit.first);
                if(!drawingModel_->locked()){
                    ChartDrawing shape;for(const auto& value:drawingModel_->drawings())if(value.id==hit.first)shape=value;
                    if(!shape.locked){
                        gestureGeneration_=drawingFrame_.contextGeneration;
                        if(hit.second>=0&&shape.kind!=ChartDrawingKind::Horizontal){dragDrawing_=hit.first;dragAnchor_=hit.second;setCursor(Qt::CrossCursor);}
                        else if(!shape.anchors.isEmpty()){
                            // Grabbing the line body moves the whole shape, keeping its slope.
                            moveDrawing_=shape.id;moveOriginal_=shape.anchors;movePreview_=shape.anchors;movePressPos_=e->position();
                            setCursor(shape.kind==ChartDrawingKind::Horizontal?Qt::SizeVerCursor:Qt::SizeAllCursor);
                        }
                    }
                }
                e->accept();return;
            }
            selectDrawing({});
        }
        dragging_=true;dragX_=e->position().x();setCursor(Qt::ClosedHandCursor);e->accept();
    }

void CandleChart::mouseReleaseEvent(QMouseEvent* e){
        lastModifiers_=e->modifiers();
        if(axisDragging_){axisDragging_=false;axisHover_=onPriceAxis(e->position());if(!axisHover_)unsetCursor();e->accept();refresh();return;}
        if(!dragDrawing_.isEmpty()){
            const auto id=dragDrawing_;const auto anchor=dragAnchor_;
            const auto released=pointerAnchor(e->position(),plotGeometry(),e->modifiers());
            const auto point=released?released:preview_;
            const bool current=gestureGeneration_==drawingFrame_.contextGeneration;
            dragDrawing_.clear();dragAnchor_=-1;preview_.reset();
            if(point&&current&&drawingModel_&&!drawingModel_->locked()){QString error;if(!drawingModel_->moveAnchor(id,anchor,*point,drawingFrame_,&error))feedback(error);}
        }
        if(!moveDrawing_.isEmpty()){
            const auto id=moveDrawing_;const auto points=movePreview_;const bool changed=points!=moveOriginal_;const bool current=gestureGeneration_==drawingFrame_.contextGeneration;
            moveDrawing_.clear();moveOriginal_.clear();movePreview_.clear();
            if(changed&&current&&drawingModel_&&!drawingModel_->locked()){QString error;if(!drawingModel_->moveDrawing(id,points,drawingFrame_,&error))feedback(error);}
        }
        dragging_=false;unsetCursor();refresh();e->accept();
    }

void CandleChart::mouseDoubleClickEvent(QMouseEvent* e){
        if(e->button()!=Qt::LeftButton){QWidget::mouseDoubleClickEvent(e);return;}
        const auto plot=plotGeometry();
        if(onPriceAxis(e->position())){resetPriceScale();e->accept();return;}
        if(tool_==ChartDrawingTool::Crosshair&&plot.available){
            const auto sim=similarityTableLayout(plot);
            if(sim.visible&&(sim.expanded?sim.box:sim.collapsedRect).contains(e->position())){e->accept();return;}
        }
        if(tool_!=ChartDrawingTool::Crosshair||!hitDrawing(e->position(),plot).first.isEmpty()){e->accept();return;}
        // Double-click on empty chart space restores the default view at the latest bar.
        resetZoom();e->accept();
    }

void CandleChart::contextMenuEvent(QContextMenuEvent* e){
        const auto plot=plotGeometry();QMenu menu(this);menu.setObjectName("marketCandlesChartMenu");applyLocalMenuStyle(&menu);
        const auto hit=drawingModel_&&drawingModel_->visible()?hitDrawing(e->pos(),plot):QPair<QString,int>{};
        if(!hit.first.isEmpty()){
            selectDrawing(hit.first);
            bool shapeLocked=false;
            for(const auto& value:drawingModel_->drawings())if(value.id==hit.first){shapeLocked=value.locked;break;}
            auto* remove=menu.addAction(tr("이 선 삭제"));remove->setObjectName("marketCandlesChartMenuDelete");remove->setEnabled(!drawingModel_->locked()&&!shapeLocked);
            connect(remove,&QAction::triggered,this,[this,id=hit.first]{if(drawingModel_&&!drawingModel_->locked()){drawingModel_->remove(id);selectedDrawing_.clear();if(drawingUiChanged)drawingUiChanged();refresh();}});
            menu.addSeparator();
        }
        if(tool_!=ChartDrawingTool::Crosshair){auto* cursor=menu.addAction(tr("그리기 끝내기"));connect(cursor,&QAction::triggered,this,[this]{setDrawingTool(ChartDrawingTool::Crosshair);});}
        auto* live=menu.addAction(tr("실시간으로"));live->setObjectName("marketCandlesChartMenuLive");live->setEnabled(syncRange_.valid()||offset_>.000001);connect(live,&QAction::triggered,this,[this]{returnToLive();});
        auto* reset=menu.addAction(tr("기본 확대로"));reset->setObjectName("marketCandlesChartMenuReset");connect(reset,&QAction::triggered,this,[this]{resetZoom();});
        auto* autoScale=menu.addAction(tr("가격 축 자동 맞춤"));autoScale->setObjectName("marketCandlesChartMenuAutoScale");autoScale->setEnabled(std::abs(priceScale_-1)>1e-9);
        connect(autoScale,&QAction::triggered,this,[this]{resetPriceScale();});
        menu.exec(e->globalPos());e->accept();
    }

void CandleChart::mouseMoveEvent(QMouseEvent* e){
        hoverPos_=e->position();hoverInside_=true;lastModifiers_=e->modifiers();
        if(axisDragging_){if(crosshairHovered)crosshairHovered(0);setPriceScale(axisPressScale_*std::exp((e->position().y()-axisPressY_)/200.0));e->accept();return;}
        {const bool axis=!dragging_&&moveDrawing_.isEmpty()&&dragDrawing_.isEmpty()&&onPriceAxis(e->position());
            if(axis!=axisHover_){axisHover_=axis;if(axis)setCursor(Qt::SizeVerCursor);else unsetCursor();}}
        if(drawingModel_&&(!draft_.isEmpty()||!dragDrawing_.isEmpty())){
            if(crosshairHovered)crosshairHovered(0);
            const auto plot=plotGeometry();
            preview_=pointerAnchor(e->position(),plot,e->modifiers());
            updateMagnetSnap(plot);
            refresh();e->accept();return;
        }
        if(!moveDrawing_.isEmpty()){if(crosshairHovered)crosshairHovered(0);if(const auto moved=movedAnchors(e->position(),plotGeometry()))movePreview_=*moved;refresh();e->accept();return;}
        const auto plot=plotGeometry();
        updateMagnetSnap(plot);
        bool nextSimHover=false;int nextSimRow=-1;
        if(!dragging_&&!axisHover_&&tool_==ChartDrawingTool::Crosshair&&plot.available){
            auto sim=similarityTableLayout(plot);
            if(sim.visible){
                if(sim.alwaysExpanded){
                    if(sim.box.contains(e->position())){
                        nextSimHover=true;
                        for(int i=0;i<sim.rowRects.size();++i)if(sim.rowRects[i].contains(e->position())){nextSimRow=i;break;}
                    }
                }else{
                    const QRectF hitRect=similarityTableHovered_?sim.box:sim.collapsedRect;
                    if(hitRect.contains(e->position())){
                        nextSimHover=true;
                        if(!similarityTableHovered_){
                            similarityTableHovered_=true;
                            sim=similarityTableLayout(plot);
                        }
                        for(int i=0;i<sim.rowRects.size();++i)if(sim.rowRects[i].contains(e->position())){nextSimRow=i;break;}
                    }
                }
            }
        }
        similarityTableHovered_=nextSimHover;similarityHoverRow_=nextSimRow;
        if(dragging_){const auto delta=(e->position().x()-dragX_)/qMax(1.0,plot.area.width())*(plot.bounds.second-plot.bounds.first);
            if(delta){pan(delta);dragX_=e->position().x();}}
        else if(!axisHover_&&similarityHoverRow_>=0){setCursor(Qt::PointingHandCursor);}
        else if(!axisHover_&&similarityTableHovered_){unsetCursor();}
        else if(!axisHover_&&tool_==ChartDrawingTool::Crosshair&&drawingModel_&&drawingModel_->visible()&&!drawingModel_->locked()){
            // Show that a drawn line can be grabbed before the press; locked shapes stay arrow.
            const auto hit=hitDrawing(e->position(),plot);
            bool movable=false;
            if(!hit.first.isEmpty())for(const auto& shape:drawingModel_->drawings())if(shape.id==hit.first){movable=!shape.locked;break;}
            if(movable)setCursor(Qt::OpenHandCursor);else unsetCursor();
        }
        else if(!axisHover_&&!dragging_&&moveDrawing_.isEmpty()&&dragDrawing_.isEmpty())unsetCursor();
        hoverY_=e->position().y();
        const auto& rows=plot.rows;const auto& axis=plot.axis;if(rows.isEmpty()||!axis.available){if(crosshairHovered)crosshairHovered(0);refresh();return;}
        const auto bounds=plot.bounds;const auto slot=bounds.first+(e->position().x()-plot.area.left())/qMax(1.0,plot.area.width())*(bounds.second-bounds.first);hover_=-1;
        double distance=.49,hoverCoordinate=std::numeric_limits<double>::quiet_NaN();
        for(qsizetype i=0;i<rows.size();++i){const auto coordinate=axis.coordinateAt(rows[i].startsAtMs);if(!coordinate)continue;
            const auto d=std::abs(*coordinate-slot);if(d<distance){distance=d;hover_=int(i);hoverCoordinate=*coordinate;}}
        // OHLC values are drawn in the chart legend and on the axes; a popup no longer covers candles.
        // The same summary stays readable as a property for assistive and test inspection.
        if(hover_>=0){const auto& c=rows[hover_];setProperty("hoverSummary",tr("%1 KST · %2\n시가 %3 · 고가 %4\n저가 %5 · 종가 %6\n거래량 %7").arg(localTime(c.startsAtMs),currency_,priceAmount(c.open),priceAmount(c.high),priceAmount(c.low),priceAmount(c.close),priceAmount(c.volume))+stockRowDescription(c.startsAtMs));}
        else setProperty("hoverSummary",QString{});
        if(crosshairHovered)crosshairHovered(!dragging_&&plot.area.contains(e->position())&&hover_>=0?rows[hover_].startsAtMs:0);
        if(hover_<0&&stock_&&plot.area.contains(e->position()))QToolTip::showText(e->globalPosition().toPoint(),tr("무체결 또는 관측 없음 · 가격·거래량을 채우지 않음"),this);
        else QToolTip::hideText();
        hoverUpdate(plot.area,plot.area.left()+(hoverCoordinate-bounds.first)/(bounds.second-bounds.first)*plot.area.width());
    }

void CandleChart::refresh(){magnetGeometryDirty_=true;geometryCache_.reset();staticDirty_=true;QWidget::update();}

void CandleChart::hoverUpdate(const QRectF& area,double crosshairX){
        if(staticDirty_||staticLayer_.isNull()||property("keefiNoStaticCache").toBool()){hoverRegion_={};QWidget::update();return;}
        QRegion next;
        const int axisLeft=qFloor(area.right())-(fibAxisLabels_.isEmpty()?0:FibLabelConnectorWidth);
        next+=QRect(axisLeft,0,width()-axisLeft,height());
        next+=QRect(0,height()-24,width(),24);
        next+=QRectF(area.left(),area.top(),area.width(),CandleOhlcInset+CandleOhlcHeight+2).toAlignedRect();
        if(std::isfinite(crosshairX))next+=QRect(qFloor(crosshairX)-3,qFloor(area.top())-1,7,qCeil(area.height())+2);
        next+=QRect(qFloor(area.left())-1,qFloor(qBound(area.top(),hoverY_,area.bottom()))-3,qCeil(area.width())+2,7);
        if(magnetSnapPos_)next+=QRect(qFloor(magnetSnapPos_->x())-12,qFloor(magnetSnapPos_->y())-12,24,24);
        const auto plot=plotGeometry();
        if(plot.available&&plot.following&&!studies_.similarity.paths.isEmpty()){
            const auto sim=similarityTableLayout(plot);
            if(sim.visible){
                next+=(sim.expanded?sim.box:sim.collapsedRect).adjusted(-4,-4,4,4).toAlignedRect();
                if(!sim.projectionRect.isEmpty())next+=sim.projectionRect.adjusted(-4,-4,4,4).toAlignedRect();
            }
        }
        const auto region=hoverRegion_.isEmpty()?QRegion(rect()):hoverRegion_+next;hoverRegion_=next;QWidget::update(region);
    }

void CandleChart::paintEvent(QPaintEvent* event){
        // A redirected render into a non-raster device (a recording or vector target) gets every draw call.
        {QPainter probe(this);const auto* engine=probe.paintEngine();
            if(property("keefiNoStaticCache").toBool()||!engine||engine->type()!=QPaintEngine::Raster){paintChart(probe,true,true);return;}}
        const auto dpr=devicePixelRatioF();const auto& c=themeColors();
        const QList<quint64> key{quint64(width()),quint64(height()),quint64(qRound64(dpr*1000)),c.bg.rgba(),c.text.rgba(),c.grid.rgba(),c.text3.rgba()};
        if(staticDirty_||staticLayer_.isNull()||key!=staticKey_||staticAge_.elapsed()>StaticLayerMaxAgeMs){
            QPixmap layer(QSize(qCeil(width()*dpr),qCeil(height()*dpr)));layer.setDevicePixelRatio(dpr);
            {QPainter painter(&layer);paintChart(painter,true,false);}
            staticLayer_=layer;staticKey_=key;staticDirty_=false;staticAge_.restart();
            // A rebuilt layer must reach the whole widget, also when this paint covers only pointer strips.
            if(event->region()!=QRegion(rect())){hoverRegion_={};QWidget::update();}
        }
        QPainter p(this);p.drawPixmap(0,0,staticLayer_);paintChart(p,false,true);
    }

void CandleChart::paintChart(QPainter& p,const bool S,const bool D){
        p.setFont(KeefiFonts::font(FontRole::Caption12));if(S)p.fillRect(rect(),themeColors().bg);const auto plot=plotGeometry();
        if(D&&magnetGeometryDirty_)updateMagnetSnap(plot);
        if(!plot.available)return;
        if(plot.rows.isEmpty()){
            offscreenTopTag_.reset();offscreenBottomTag_.reset();
            fibAxisLabels_.clear();
            setProperty("drawingLabelTexts",QStringList{});
            setProperty("drawingOffscreenTopLabel",QString{});
            setProperty("drawingOffscreenBottomLabel",QString{});
            setProperty("similarityTableVisible",false);
            setProperty("similarityTableExpanded",false);
            setProperty("similarityTableRect",QRectF{});
            setProperty("similarityTableSummary",QString{});
            setProperty("similarityTableRows",QStringList{});
            setProperty("similarityRowRects",QVariantList{});
            setProperty("similarityHoverRow",-1);
            if(S){p.setPen(themeColors().text3);p.drawText(rect(),Qt::AlignCenter,tr("이 구간에 관측된 봉이 없어요\n가로로 계속 이동할 수 있어요"));}return;
        }
        const auto& rows=plot.rows;const auto& axis=plot.axis;const auto& area=plot.area;const auto bounds=plot.bounds;
        const bool volumeLayer=plot.volumeLayer,flowLayer=plot.flowLayer,cvdLayer=plot.cvdLayer,following=plot.following;
        const auto low=plot.low,high=plot.high,volumeHeight=plot.volumeHeight,cvdHeight=plot.cvdHeight;const auto start=plot.start,end=plot.end;
        const auto x=[&](qint64 at){const auto slot=axis.coordinateAt(at);return slot?area.left()+(*slot-bounds.first)/(bounds.second-bounds.first)*area.width():std::numeric_limits<double>::quiet_NaN();};
        const auto y=[&](double value){return area.bottom()-(value-low)/(high-low)*area.height();};
        const auto bodyWidth=qBound(1.0,area.width()/(bounds.second-bounds.first)*.65,18.0);
        // Round-number price grid and calendar-aligned time grid, as in common charts.
        const auto priceStep=niceStep((high-low)/qMax(2.0,std::floor(area.height()/56)));QList<double> priceTicks;
        if(priceStep>0)for(double value=std::ceil(low/priceStep)*priceStep;value<=high&&priceTicks.size()<40;value+=priceStep)priceTicks.append(value);
        const auto spanMs=(bounds.second-bounds.first)*double(intervalMs_);const auto timeStep=timeTickStep(spanMs/qMax(1.0,area.width()),intervalMs_);
        QList<QPair<double,QString>> timeTicks;if(S){double lastRight=-1e9;p.setFont(KeefiFonts::font(FontRole::Number11));
            qint64 previousAt=0;for(const auto& row:rows){const auto before=previousAt;previousAt=row.startsAtMs;if(forming_&&row.startsAtMs==forming_->startsAtMs)continue;
                if(!timeTickAt(row.startsAtMs,before,timeStep,intervalMs_))continue;const auto xx=x(row.startsAtMs);if(!std::isfinite(xx))continue;
                const auto text=timeTickText(row.startsAtMs,timeStep);const auto half=p.fontMetrics().horizontalAdvance(text)/2.0;
                if(xx-half<lastRight+10||xx-half<area.left()||xx+half>area.right())continue;timeTicks.append({xx,text});lastRight=xx+half;}
            p.setFont(KeefiFonts::font(FontRole::Caption12));}
        p.setPen(themeColors().grid);
        if(S){for(const auto value:priceTicks){const auto yy=y(value);p.drawLine(QPointF(area.left(),yy),QPointF(area.right(),yy));}
        for(const auto& tick:timeTicks)p.drawLine(QPointF(tick.first,area.top()),QPointF(tick.first,area.bottom()));}
        p.save();p.setClipRect(area);
        if(S){
        if(layers_.contains("cloud"))for(qsizetype i=0;i<studies_.cloud.size();++i){
            const auto& b=studies_.cloud[i];
            const auto connected=[&](qint64 from,qint64 to){return stock_?axis.connects(from,to):to-from==intervalMs_;};
            const auto automatic=MovementColorPolicy::fillFor(this,movementForSign(b.fast>b.slow?1:b.fast<b.slow?-1:0));
            const auto color=b.fast==b.slow?automatic:studyColors_.value(b.fast>b.slow?"cloudUp":"cloudDown",automatic);
            if(i==0||!connected(studies_.cloud[i-1].atMs,b.atMs)){
                if(b.atMs>=start&&b.atMs<=end&&std::isfinite(x(b.atMs))&&
                    (i+1==studies_.cloud.size()||!connected(b.atMs,studies_.cloud[i+1].atMs))){
                    p.setPen(QPen(color,3,Qt::SolidLine,Qt::RoundCap));
                    p.drawPoint(QPointF(x(b.atMs),y(b.fast)));p.drawPoint(QPointF(x(b.atMs),y(b.slow)));
                }
                continue;
            }
            const auto& a=studies_.cloud[i-1];
            if(b.atMs<start||a.atMs>end||!std::isfinite(x(a.atMs))||!std::isfinite(x(b.atMs)))continue;
            QPainterPath band;band.moveTo(x(a.atMs),y(a.fast));band.lineTo(x(b.atMs),y(b.fast));band.lineTo(x(b.atMs),y(b.slow));band.lineTo(x(a.atMs),y(a.slow));band.closeSubpath();
            QColor fill=color;fill.setAlpha(45);p.fillPath(band,fill);p.setPen(QPen(color,1));
            p.drawLine(QPointF(x(a.atMs),y(a.fast)),QPointF(x(b.atMs),y(b.fast)));p.drawLine(QPointF(x(a.atMs),y(a.slow)),QPointF(x(b.atMs),y(b.slow)));
        }
        const auto drawSeries=[&](const QList<ChartPricePoint>& points,const QColor& ink){
            p.setPen(QPen(ink,1));QPainterPath path;qint64 previous=0;
            for(const auto& point:points){const auto xx=x(point.atMs);if(!std::isfinite(xx)||point.atMs<start||point.atMs>end){previous=0;continue;}
                const bool connected=previous&&(stock_?axis.connects(previous,point.atMs):point.atMs-previous==intervalMs_);
                if(connected)path.lineTo(xx,y(point.price));else path.moveTo(xx,y(point.price));previous=point.atMs;
            }p.drawPath(path);
        };
        const auto studyInk=[&](const QString& key) {
            const auto i=studyColors_.constFind(key);
            return i==studyColors_.cend()?market_candles_detail::defaultStudyColor(key,this):i.value();
        };
        if(layers_.contains("ma")){
            for(const auto& line:extended_.maLines)if(line.state.available)drawSeries(line.points,studyInk(line.spec.id));
        }
        if(layers_.contains("bb")&&extended_.bb.state.available){QList<ChartPricePoint> lower,middle,upper;for(const auto& b:extended_.bb.points){lower.append({b.atMs,b.lower});middle.append({b.atMs,b.middle});upper.append({b.atMs,b.upper});}drawSeries(lower,studyInk("bbLower"));drawSeries(middle,studyInk("bbMiddle"));drawSeries(upper,studyInk("bbUpper"));}
        if(layers_.contains("low_volume")&&extended_.lowVolumeZones.state.available){p.setPen(QPen(studyInk("low_volume"),1,Qt::DotLine));for(const auto& zone:extended_.lowVolumeZones.zones){p.drawLine(QPointF(area.left(),y(zone.low)),QPointF(area.right(),y(zone.low)));p.drawLine(QPointF(area.left(),y(zone.high)),QPointF(area.right(),y(zone.high)));}}
        if(layers_.contains("pullback")&&studies_.pullback.available){
            const auto& box=studies_.pullback;
            const auto right=stock_?x(rows_.last().startsAtMs)+bodyWidth/.65/2:x(studies_.lastCompletedAtMs-intervalMs_/2);
            if(std::isfinite(x(box.waveHighAtMs))&&std::isfinite(right)){
                const QRectF support(QPointF(x(box.waveHighAtMs),y(box.boxHigh)),QPointF(right,y(box.boxLow)));
                // A six-pixel tile, rotated 45°, keeps the interval perpendicular
                // to the strokes. Rebuilt from the current theme on each paint.
                QPixmap tile(12,12);tile.setDevicePixelRatio(2);tile.fill(Qt::transparent);
                {QPainter pattern(&tile);auto hatch=studyInk("pullback");hatch.setAlphaF(.6);pattern.setPen(QPen(hatch,1.2));pattern.drawLine(QPointF(3,0),QPointF(3,6));}
                QBrush hatch(tile);QTransform rotation;rotation.rotate(45);hatch.setTransform(rotation);p.fillRect(support.normalized(),hatch);
                p.setPen(QPen(studyInk("pullback"),1));p.setBrush(Qt::NoBrush);p.drawRect(support.normalized());
                if(std::isfinite(x(box.waveLowAtMs)))p.drawLine(QPointF(x(box.waveLowAtMs),y(box.waveLow)),QPointF(x(box.waveHighAtMs),y(box.waveHigh)));
            }
        }
        if(style_=="line"){
            // Closes of completed bars, joined only across connected bars; the forming bar is a faint dashed tail.
            QPainterPath path;qint64 previous=0;double lastX=0,lastY=0;
            for(const auto& c:rows){if(forming_&&c.startsAtMs==forming_->startsAtMs)continue;const auto xx=x(c.startsAtMs),yy=y(c.close.toDouble());if(!std::isfinite(xx))continue;
                const bool connected=previous&&(stock_?axis.connects(previous,c.startsAtMs):c.startsAtMs-previous==intervalMs_);
                if(connected)path.lineTo(xx,yy);else path.moveTo(xx,yy);previous=c.startsAtMs;lastX=xx;lastY=yy;}
            p.setBrush(Qt::NoBrush);p.setPen(QPen(themeColors().text,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(path);
            if(forming_&&previous&&forming_->startsAtMs-previous==intervalMs_){const auto xx=x(forming_->startsAtMs);if(std::isfinite(xx)){
                p.setOpacity(.5);p.setPen(QPen(themeColors().text,1.2,Qt::DashLine));p.drawLine(QPointF(lastX,lastY),QPointF(xx,y(forming_->close.toDouble())));p.setOpacity(1);}}
        }
        for(const auto& c:rows){const double open=c.open.toDouble(),close=c.close.toDouble();
            p.setOpacity(forming_&&c.startsAtMs==forming_->startsAtMs?.35:1.0);
            const auto color=MovementColorPolicy::fillFor(this,movementForSign(close>open?1:close<open?-1:0));
            const auto xx=x(c.startsAtMs);
            if(style_=="bars"){
                // OHLC bar: high-low stem, open tick to the left, close tick to the right.
                const auto tick=qMax(2.0,bodyWidth/2);p.setPen(QPen(color,qBound(1.0,bodyWidth/6,2.0)));p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(xx,y(c.low.toDouble())),QPointF(xx,y(c.high.toDouble())));
                p.drawLine(QPointF(xx-tick,y(open)),QPointF(xx,y(open)));p.drawLine(QPointF(xx,y(close)),QPointF(xx+tick,y(close)));
            }else if(style_!="line"){
                p.setPen(QPen(color,1));p.setBrush(close>open?themeColors().bg:color);
                p.drawLine(QPointF(xx,y(c.low.toDouble())),QPointF(xx,y(c.high.toDouble())));
                p.drawRect(QRectF(xx-bodyWidth/2,qMin(y(open),y(close)),bodyWidth,qMax(1.0,std::abs(y(open)-y(close)))));
            }
            if(stock_)for(const auto& context:timeline_.rows)if(context.startsAtMs==c.startsAtMs){
                const auto sessionColor=context.session==StockSessionKind::Pre?themeColors().studyBlue:context.session==StockSessionKind::After?themeColors().studyPurple:
                    context.session==StockSessionKind::Day?themeColors().sessionDay:context.session==StockSessionKind::Mixed?themeColors().accent:themeColors().text3;
                p.fillRect(QRectF(xx-bodyWidth/2,area.top(),bodyWidth,3),sessionColor);
                if(context.exclusions.contains(StockBarExclusion::VolatilityInterruption)||context.exclusions.contains(StockBarExclusion::PriceLimit)){
                    p.setPen(QPen(themeColors().danger,1,Qt::DashLine));p.setBrush(Qt::NoBrush);
                    p.drawRect(QRectF(xx-bodyWidth/2-2,y(c.high.toDouble())-2,bodyWidth+4,qMax(4.0,y(c.low.toDouble())-y(c.high.toDouble())+4)));
                }
                break;
            }
        }
        p.setOpacity(1);
        if(flowLayer){
            p.setBrush(Qt::NoBrush);
            for(const auto& group:coinFlow_.groups){bool ok=false;const auto value=group.entryClose.toDouble(&ok);
                const auto markerAt=coinFlowMarkerCandleAt(group.fromMs,rows,intervalMs_);if(!markerAt)continue;const auto at=*markerAt;
                if(!ok||!std::isfinite(value)||value<low||value>high)continue;
                const auto xx=x(at),yy=y(value);if(!std::isfinite(xx))continue;
                const auto automatic=MovementColorPolicy::fillFor(this,group.direction==CoinFlowDirection::Down?MovementDirection::Down:group.direction==CoinFlowDirection::Up?MovementDirection::Up:MovementDirection::Unknown);
                const auto color=group.direction==CoinFlowDirection::Down?studyColors_.value("whalesDown",automatic):group.direction==CoinFlowDirection::Up?studyColors_.value("whalesUp",automatic):automatic;
                p.setPen(QPen(themeColors().text,1));p.setBrush(color);QPolygonF marker;
                if(group.direction==CoinFlowDirection::Down)marker<<QPointF(xx-5,yy-10)<<QPointF(xx+5,yy-10)<<QPointF(xx,yy-3);
                else marker<<QPointF(xx-5,yy+10)<<QPointF(xx+5,yy+10)<<QPointF(xx,yy+3);
                p.drawPolygon(marker);p.setBrush(Qt::NoBrush);p.setPen(QPen(color,2));
                const auto through=coinFlowMarkerCandleAt(group.throughMs,rows,intervalMs_);if(through&&*through>at&&std::isfinite(x(*through)))p.drawLine(QPointF(xx,yy),QPointF(x(*through),yy));
            }
        }
        if(following){
            p.setBrush(Qt::NoBrush);
            const auto& similar=studies_.similarity;
            if(!similar.minimum.isEmpty()&&similar.minimum.size()==similar.maximum.size()){
                QPainterPath band;band.moveTo(x(similar.minimum.first().atMs),y(similar.minimum.first().price));
                for(const auto& point:similar.minimum)band.lineTo(x(point.atMs),y(point.price));
                for(auto it=similar.maximum.crbegin();it!=similar.maximum.crend();++it)band.lineTo(x(it->atMs),y(it->price));band.closeSubpath();
                auto bandInk=studyInk("similarity");bandInk.setAlpha(45);p.fillPath(band,bandInk);
            }
            const QList<QColor> colors{studyInk("similarity1"),studyInk("similarity2"),studyInk("similarity3")};
            for(int i=0;i<similar.paths.size();++i){QPainterPath path;bool first=true;for(const auto& point:similar.paths[i].points){if(first){path.moveTo(x(point.atMs),y(point.price));first=false;}else path.lineTo(x(point.atMs),y(point.price));}
                p.setPen(QPen(colors[i%colors.size()],1,Qt::DotLine));p.drawPath(path);}
            QPainterPath mean;bool first=true;for(const auto& point:similar.mean){if(first){mean.moveTo(x(point.atMs),y(point.price));first=false;}else mean.lineTo(x(point.atMs),y(point.price));}
            p.setPen(QPen(studyInk("similarity"),2));p.drawPath(mean);
            // The following coordinates contain actual historical paths, not
            // a forecast. When paths exist, the similarity comparison table in D pass
            // carries the single disclaimer line so it never duplicates.
            if(similar.paths.isEmpty()){
                p.setFont(KeefiFonts::font(FontRole::Caption12));p.setPen(themeColors().text2);
                const auto note=tr("과거 경로 · 예측 아님");const auto noteWidth=p.fontMetrics().horizontalAdvance(note);
                const auto left=qBound(area.left(),x(similar.maximum.first().atMs)+6,qMax(area.left(),area.right()-noteWidth-4));
                const QRectF noteBox(left,area.top()+4,qMin(qreal(noteWidth+4),area.right()-left),20);
                p.fillRect(noteBox,themeColors().bg);p.drawText(noteBox,Qt::AlignLeft|Qt::AlignVCenter,note);
            }
        }
        if(layers_.contains("levels")&&levels_.state.available){
            const QColor ink=studyInk("levels");QList<qreal> labels;
            if(!rows_.isEmpty())labels.append(y(rows_.last().close.toDouble()));p.setPen(QPen(ink,1,Qt::DashLine));
            for(const auto& level:levels_.visible){const auto price=level.price.toDouble();if(price<low||price>high)continue;const auto yy=y(price);p.drawLine(QPointF(area.left(),yy),QPointF(area.right(),yy));
                bool collision=false;for(auto at:labels)collision|=std::abs(at-yy)<18;if(collision||yy<area.top()+18)continue;labels.append(yy);
                p.setFont(KeefiFonts::font(FontRole::Caption12));const auto label=level.labels.join("·")+" ";const int nameWidth=p.fontMetrics().horizontalAdvance(label);
                p.fillRect(QRectF(area.left()+4,yy-17,qMin(qreal(nameWidth+100),area.width()-8),16),themeColors().bg);p.setPen(ink);p.drawText(QRectF(area.left()+4,yy-17,nameWidth,16),Qt::AlignVCenter,label);
                p.setFont(KeefiFonts::font(FontRole::Number11));p.drawText(QRectF(area.left()+4+nameWidth,yy-17,100,16),Qt::AlignVCenter,priceAmount(level.price));
            }
        }
        for(const auto& line:lines_){const auto price=line.price.toDouble();if(price<low||price>high)continue;
            const auto color=(line.kind==ChartPriceKind::Liquidation?themeColors().danger:line.kind==ChartPriceKind::Entry?themeColors().studyBlue:themeColors().text);
            QPen pricePen(color,1,Qt::DashLine);if(line.kind==ChartPriceKind::Average){pricePen.setDashPattern({5,4});pricePen.setCapStyle(Qt::FlatCap);}
            p.setPen(pricePen);const auto yy=y(price);p.drawLine(QPointF(area.left(),yy),QPointF(area.right(),yy));
            const auto kind=line.kind==ChartPriceKind::Average?tr("평균"):line.kind==ChartPriceKind::Entry?tr("진입"):tr("청산");
            p.setPen(color);p.setFont(KeefiFonts::font(FontRole::Caption12));
            const auto label=line.provider+" · "+accountLabel(lines_,line.accountScope)+" · "+kind+" ";
            const auto labelWidth=p.fontMetrics().horizontalAdvance(label);p.drawText(QRectF(area.left()+5,yy-20,labelWidth,20),Qt::AlignLeft|Qt::AlignVCenter,label);
            p.setFont(KeefiFonts::font(FontRole::Number11));
            const auto numeric=priceAmount(line.price)+" "+line.currency;const auto numericWidth=p.fontMetrics().horizontalAdvance(numeric);
            p.drawText(QRectF(area.left()+5+labelWidth,yy-20,numericWidth,20),Qt::AlignLeft|Qt::AlignVCenter,numeric);
        }
        p.setFont(KeefiFonts::font(FontRole::Caption12));paintDrawings(p,plot);
        } // cached layers inside the plot clip
        // Latest price: a dashed line across the plot in that candle's direction colour.
        const auto latest=forming_?std::optional<MarketCandle>(*forming_):rows_.isEmpty()?std::optional<MarketCandle>{}:std::optional<MarketCandle>(rows_.last());
        QColor latestColor=themeColors().text;double latestY=std::numeric_limits<double>::quiet_NaN();
        if(latest){const auto close=latest->close.toDouble(),open=latest->open.toDouble();latestColor=MovementColorPolicy::fillFor(this,movementForSign(close>open?1:close<open?-1:0));
            // Lighter than the candle ink so the line never reads as a candle body.
            if(close>=low&&close<=high){latestY=y(close);if(S){auto line=latestColor;line.setAlphaF(.7);p.setPen(QPen(line,1,Qt::DashLine));p.drawLine(QPointF(area.left(),latestY),QPointF(area.right(),latestY));}}}
        if(D){paintAutoPreview(p,plot);paintLiveTrade(p,plot);paintSimilarityOverlay(p,plot);paintMagnetSnapRing(p);}
        const bool pointerInPlot=hoverInside_&&area.contains(hoverPos_)&&moveDrawing_.isEmpty()&&!dragging_;
        if(D&&hover_>=0&&hover_<rows.size()){p.setPen(QPen(themeColors().text3,1,Qt::DashLine));const auto xx=x(rows[hover_].startsAtMs);p.drawLine(QPointF(xx,area.top()),QPointF(xx,area.bottom()));
            const auto yy=qBound(area.top(),hoverY_,area.bottom());p.drawLine(QPointF(area.left(),yy),QPointF(area.right(),yy));}
        p.restore();
        if(S)paintOffscreenFibTags(p,plot);
        if(S&&volumeLayer){
            const QRectF volumeArea(area.left(),area.bottom()+8,area.width(),qMax(1.0,volumeHeight-10));double maximum=0;
            for(const auto& point:studies_.volumes)if(point.atMs>=start&&point.atMs<=rows.last().startsAtMs)maximum=qMax(maximum,point.volume.toDouble());
            p.save();p.setClipRect(volumeArea);p.setPen(Qt::NoPen);
            // Volume bars take their candle's direction colour, as in common charts.
            QHash<qint64,int> directions;for(const auto& row:rows){const auto close=row.close.toDouble(),open=row.open.toDouble();directions.insert(row.startsAtMs,close>open?1:close<open?-1:0);}
            if(maximum>0)for(const auto& point:studies_.volumes)if(point.atMs>=start&&point.atMs<=rows.last().startsAtMs){const auto height=point.volume.toDouble()/maximum*volumeArea.height();
                auto ink=directions.contains(point.atMs)?MovementColorPolicy::fillFor(this,movementForSign(directions.value(point.atMs))):themeColors().text3;const int direction=directions.value(point.atMs);
                if(direction)ink=studyColors_.value(direction>0?"volumeUp":"volumeDown",ink);
                ink.setAlphaF(.55);p.setBrush(ink);
                p.drawRect(QRectF(x(point.atMs)-bodyWidth/2,volumeArea.bottom()-height,bodyWidth,height));}
            p.restore();
        }
        if(S&&cvdLayer){
            const QRectF cvdArea(area.left(),area.bottom()+volumeHeight+8,area.width(),qMax(1.0,cvdHeight-10));double minimum=0,maximum=0;
            for(const auto& point:coinFlow_.cvd.points)if(point.atMs>=start&&point.atMs<=end){bool ok=false;const auto value=point.signedBaseQuantity.toDouble(&ok);if(ok&&std::isfinite(value)){minimum=qMin(minimum,value);maximum=qMax(maximum,value);}}
            if(maximum==minimum)maximum=minimum+1;
            p.save();p.setClipRect(cvdArea);p.setBrush(Qt::NoBrush);p.setPen(QPen(studyColors_.value("cvd",themeColors().studyPurple),1));QPainterPath path;quint64 epoch=0;
            for(const auto& point:coinFlow_.cvd.points){bool ok=false;const auto value=point.signedBaseQuantity.toDouble(&ok);const auto xx=x(point.atMs);
                if(!ok||!std::isfinite(value)||!std::isfinite(xx)||point.atMs<start||point.atMs>end){epoch=0;continue;}
                const QPointF next(xx,cvdArea.bottom()-(value-minimum)/(maximum-minimum)*cvdArea.height());if(epoch!=point.epoch)path.moveTo(next);else path.lineTo(next);epoch=point.epoch;}
            p.drawPath(path);p.restore();p.setPen(studyColors_.value("cvd",themeColors().studyPurple));p.drawText(QRectF(cvdArea.left()+3,cvdArea.top(),cvdArea.width()-6,18),Qt::AlignLeft,tr("관측 CVD · 기준 수량 · epoch별 별도 기준"));
        }
        const auto axisWidth=width()-area.right()-10;
        p.setFont(KeefiFonts::font(FontRole::Number11));p.setPen(themeColors().text3);
        // Never present a truncated amount as a complete axis price. Extreme
        // strings that cannot fit beside 200px of price ink remain in tooltips.
        const auto axisText=[&](qreal top,const QString& amount,qreal height=20){
            if(p.fontMetrics().horizontalAdvance(amount)<=axisWidth)
                p.drawText(QRectF(area.right()+5,top,axisWidth,height),Qt::AlignLeft,amount);
        };
        if(S)axisText(area.top()-6,currency_,16);
        // Round-number price labels, kept clear of the currency, latest price and crosshair boxes.
        QList<qreal> occupied{area.top()+2};if(offscreenTopTag_)occupied.append(offscreenTopTag_->occupiedY);if(offscreenBottomTag_)occupied.append(offscreenBottomTag_->occupiedY);if(std::isfinite(latestY))occupied.append(latestY);if(pointerInPlot)occupied.append(hoverPos_.y());
        // The round-number labels move out of the pointer's way, so they belong to the overlay.
        if(D){paintFibAxisLabels(p,area,axisWidth,occupied);p.setPen(themeColors().text3);for(const auto value:priceTicks){const auto yy=y(value);if(yy<area.top()+10||yy>area.bottom()+2)continue;
            if(std::any_of(occupied.cbegin(),occupied.cend(),[yy](qreal at){return std::abs(at-yy)<16;}))continue;axisText(yy-8,tickPrice(value,priceStep),16);}}
        const auto axisBox=[&](qreal centre,const QString& text,const QColor& fill,const QColor& ink){
            if(p.fontMetrics().horizontalAdvance(text)>axisWidth)return;
            const QRectF box(area.right()+1,qBound(area.top()-8,centre-9,area.bottom()-9),axisWidth+8,18);p.fillRect(box,fill);p.setPen(ink);p.drawText(box.adjusted(4,0,0,0),Qt::AlignLeft|Qt::AlignVCenter,text);};
        if(S&&latest&&std::isfinite(latestY)){auto fill=latestColor;fill.setAlphaF(.92);axisBox(latestY,priceAmount(latest->close),fill,themeColors().bg);}
        if(D&&pointerInPlot){const auto price=low+(area.bottom()-hoverPos_.y())/area.height()*(high-low);axisBox(hoverPos_.y(),priceAmount(pixelPrice(price,(high-low)/area.height())),themeColors().text,themeColors().bg);}
        // Calendar-aligned time labels; the crosshair time replaces the label under the pointer.
        p.setPen(themeColors().text3);if(S){for(const auto& tick:timeTicks)p.drawText(QRectF(tick.first-60,height()-22,120,20),Qt::AlignHCenter|Qt::AlignVCenter,tick.second);
        p.drawText(QRectF(area.right()+5,height()-22,qMax(0.0,width()-area.right()-5),20),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("KST"));}
        if(D&&pointerInPlot&&hover_>=0&&hover_<rows.size()){const auto at=rows[hover_].startsAtMs;const auto text=intervalMs_>=86400000?QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("yy/MM/dd"):localTime(at);
            const auto widthText=p.fontMetrics().horizontalAdvance(text)+10.0;const auto left=qBound(area.left(),x(at)-widthText/2,qMax(area.left(),area.right()-widthText));
            const QRectF box(left,height()-22,widthText,20);p.fillRect(box,themeColors().text);p.setPen(themeColors().bg);p.drawText(box,Qt::AlignCenter,text);}
        if(D)paintLegend(p,area,hover_>=0&&hover_<rows.size()?std::optional<MarketCandle>(rows[hover_]):latest);
    }

QRectF CandleChart::liveTradeRegion(const Geometry& plot,const QString& price)const{
        bool ok=false;const auto value=price.toDouble(&ok);
        if(!ok||!std::isfinite(value)||value<=0||!plot.available||plot.rows.isEmpty()||plot.high<=plot.low)return {};
        const auto& area=plot.area;const auto y=area.bottom()-(value-plot.low)/(plot.high-plot.low)*area.height();
        const auto labelTop=qBound(area.top()+CandleOhlcInset+CandleOhlcHeight+2,y-20,area.bottom()-20);
        const auto top=qMin(labelTop,qBound(area.top(),y,area.bottom()))-2;
        const auto bottom=qMax(labelTop+20,qBound(area.top(),y,area.bottom()))+2;
        return {area.left(),top,area.width(),bottom-top};
    }

void CandleChart::paintLiveTrade(QPainter& painter,const Geometry& plot)const{
        if(liveTradePrice_.isEmpty()||liveTradeAtMs_<=0||!plot.available||plot.rows.isEmpty()||plot.high<=plot.low)return;
        const auto value=liveTradePrice_.toDouble();const auto& area=plot.area;
        const auto y=area.bottom()-(value-plot.low)/(plot.high-plot.low)*area.height();
        painter.save();painter.setClipRect(area);painter.setFont(KeefiFonts::font(FontRole::Caption12));
        auto ink=themeColors().text2;painter.setPen(QPen(ink,1,Qt::DotLine));
        if(y>=area.top()&&y<=area.bottom())painter.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));
        auto text=tr("최근 체결 %1 %2").arg(priceAmount(liveTradePrice_),currency_);
        const auto maximum=qMax(0.0,area.width()-12);
        if(painter.fontMetrics().horizontalAdvance(text)+10>maximum)text=tr("최근 체결가 표시 공간 부족");
        const auto width=qMin(maximum,double(painter.fontMetrics().horizontalAdvance(text)+10));
        const auto top=qBound(area.top()+CandleOhlcInset+CandleOhlcHeight+2,y-20,area.bottom()-20);
        const QRectF badge(area.right()-width-4,top,width,18);
        auto fill=themeColors().bg;fill.setAlphaF(.94);painter.fillRect(badge,fill);painter.setPen(ink);
        painter.drawText(badge.adjusted(5,0,-5,0),Qt::AlignRight|Qt::AlignVCenter,text);painter.restore();
    }

void CandleChart::paintLegend(QPainter& p,const QRectF& area,const std::optional<MarketCandle>& candle)const{
        if(!candle)return;bool ok=false;const auto close=candle->close.toDouble(&ok),open=candle->open.toDouble();if(!ok)return;
        std::optional<double> previous;
        const auto found=std::lower_bound(rows_.cbegin(),rows_.cend(),candle->startsAtMs,[](const auto& row,qint64 at){return row.startsAtMs<at;});
        if(found!=rows_.cend()&&found!=rows_.cbegin())previous=std::prev(found)->close.toDouble();
        if(!previous&&!rows_.isEmpty()&&forming_&&candle->startsAtMs==forming_->startsAtMs)previous=rows_.last().close.toDouble();
        // Values in the body text colour; only the change is coloured, slightly lighter than candle ink.
        auto change=MovementColorPolicy::colorFor(this,movementForSign(close>open?1:close<open?-1:0));change.setAlphaF(.95);
        QList<QPair<QString,QString>> parts{{tr("시"),priceAmount(candle->open)},{tr("고"),priceAmount(candle->high)},{tr("저"),priceAmount(candle->low)},{tr("종"),priceAmount(candle->close)}};
        if(previous&&*previous>0){const auto percent=(close/ *previous-1)*100;parts.append({QString{},(percent>0?"+":"")+QString::number(percent,'f',2)+"%"});}
        bool volumeOk=false;const auto volume=candle->volume.toDouble(&volumeOk);
        if(volumeOk&&std::isfinite(volume))parts.append({tr("거래량"),priceAmount(QString::number(volume,'f',volume>=100?0:volume>=1?2:4))});
        const QFont label=KeefiFonts::font(FontRole::Caption12),number=KeefiFonts::font(FontRole::Number11);const QFontMetrics labelMetrics(label),numberMetrics(number);
        qreal widthTotal=8;for(const auto& part:parts)widthTotal+=(part.first.isEmpty()?0:labelMetrics.horizontalAdvance(part.first)+3)+numberMetrics.horizontalAdvance(part.second)+10;
        auto fill=themeColors().bg;fill.setAlphaF(.82);const QRectF box(area.left()+CandleOhlcInset,area.top()+CandleOhlcInset,qMin(widthTotal,area.width()-2*CandleOhlcInset),CandleOhlcHeight);p.fillRect(box,fill);
        qreal left=box.left()+4;
        for(const auto& part:parts){if(left>box.right())break;
            if(!part.first.isEmpty()){p.setFont(label);p.setPen(themeColors().text3);const auto w=labelMetrics.horizontalAdvance(part.first);p.drawText(QRectF(left,box.top(),w+2,box.height()),Qt::AlignLeft|Qt::AlignVCenter,part.first);left+=w+3;}
            p.setFont(number);p.setPen(part.first.isEmpty()?change:themeColors().text);const auto w=numberMetrics.horizontalAdvance(part.second);p.drawText(QRectF(left,box.top(),w+2,box.height()),Qt::AlignLeft|Qt::AlignVCenter,part.second);left+=w+10;}
    }

int CandleChart::axisGutter(double low,double high)const{
        const QFontMetrics metrics(KeefiFonts::font(FontRole::Number11));int needed=CandlePriceAxisMinimum;
        if(!rows_.isEmpty())needed=qMax(needed,metrics.horizontalAdvance(priceAmount(rows_.last().close))+10);
        // Bounds include the actual enabled study paths, not only raw OHLC.
        needed=qMax(needed,metrics.horizontalAdvance(priceAmount(drawnPrice(high)))+10);
        needed=qMax(needed,metrics.horizontalAdvance(priceAmount(drawnPrice(low)))+10);
        needed=qMax(needed,metrics.horizontalAdvance(currency_)+10);
        if(hasVisibleFib_){
            const auto prefix=QStringLiteral("▲ 2.618 ");
            needed=qMax(needed,105);
            needed=qMax(needed,metrics.horizontalAdvance(prefix+formatFibPrice(low,currency_))+10);
            needed=qMax(needed,metrics.horizontalAdvance(prefix+formatFibPrice(high,currency_))+10);
        }
        return qMin(needed,qMax(CandlePriceAxisMinimum,width()-208));
    }

void CandleChart::includeVisiblePriceStudies(Geometry& plot)const{
        const bool showAverages=layers_.contains("ma")&&!extended_.maLines.isEmpty();
        const bool showBands=layers_.contains("bb")&&extended_.bb.state.available;
        if(!showAverages&&!showBands)return;
        // Hover and paint share the same price range until the viewport or
        // study data changes. Look only at points in the displayed interval.
        if(!priceStudyBounds_||priceStudyBounds_->fromMs!=plot.start||priceStudyBounds_->toMs!=plot.end){
            StudyPriceBounds bounds{plot.start,plot.end};
            const auto includePrice=[&bounds](double price){
                if(!std::isfinite(price))return;
                bounds.low=qMin(bounds.low,price);
                bounds.high=qMax(bounds.high,price);
            };
            const auto includePoints=[&plot](const auto& points,const auto& include){
                const auto begin=std::lower_bound(points.cbegin(),points.cend(),plot.start,
                    [](const auto& point,qint64 from){return point.atMs<from;});
                for(auto it=begin;it!=points.cend()&&it->atMs<=plot.end;++it)include(*it);
            };
            if(showAverages){
                for(const auto& line:extended_.maLines)
                    if(line.state.available)includePoints(line.points,[&](const auto& point){includePrice(point.price);});
            }
            if(showBands)includePoints(extended_.bb.points,[&](const auto& point){
                includePrice(point.lower);
                includePrice(point.upper);
            });
            priceStudyBounds_=bounds;
        }
        plot.low=qMin(plot.low,priceStudyBounds_->low);
        plot.high=qMax(plot.high,priceStudyBounds_->high);
    }

CandleChart::Geometry CandleChart::plotGeometry()const{
        const auto axis=timeAxis();
        const GeometryState state{size(),devicePixelRatioF(),navigationBounds(axis),priceScale_,followsLive()};
        if(!geometryCache_||geometryState_!=state){geometryState_=state;geometryCache_=calculatePlotGeometry();}
        return *geometryCache_;
    }

CandleChart::Geometry CandleChart::calculatePlotGeometry()const{
        Geometry plot;plot.rows=visible();plot.axis=timeAxis();if(!plot.axis.available)return plot;
        if(plot.rows.isEmpty()){
            // A no-trade interval is empty space, not an unavailable input
            // surface. Keep its time bounds operable without inventing OHLCV.
            plot.area={CandlePlotLeft,CandlePlotTop,double(qMax(1,width()-CandlePlotLeft-CandlePriceAxisMinimum)),double(qMax(1,height()-42))};
            plot.bounds=navigationBounds(plot.axis);plot.available=true;return plot;
        }
        plot.volumeLayer=layers_.contains("volume")&&!studies_.volumes.isEmpty();plot.flowLayer=!stock_&&layers_.contains("whales");
        plot.cvdLayer=false; // CVD has its own lower pane.
        plot.volumeHeight=plot.volumeLayer?qMin(qMax(0.0,double(height()-242)),qMin(55.0,qMax(20.0,(height()-42)*.17))):0; // Preserve 200px of price ink before allocating compact volume height.
        plot.following=showsFollowing(plot.axis);plot.start=plot.rows.first().startsAtMs-intervalMs_/2;plot.end=observedFlowEnd(plot.rows);
        if(plot.following)plot.end=qMax(plot.end,studies_.similarity.maximum.last().atMs+intervalMs_/2);
        plot.low=std::numeric_limits<double>::infinity();plot.high=-plot.low;
        for(const auto& row:plot.rows){plot.low=qMin(plot.low,row.low.toDouble());plot.high=qMax(plot.high,row.high.toDouble());}
        if(plot.following){for(const auto& point:studies_.similarity.minimum)plot.low=qMin(plot.low,point.price);for(const auto& point:studies_.similarity.maximum)plot.high=qMax(plot.high,point.price);}
        if(!std::isfinite(plot.low)||!std::isfinite(plot.high)||plot.low<=0)return plot;
        includeVisiblePriceStudies(plot);
        if(plot.high==plot.low){plot.high+=plot.low*.01;plot.low-=plot.low*.01;}
        // A Bollinger lower band can be negative even when every candle price is positive.
        {const auto pad=(plot.high-plot.low)*.06;plot.high+=pad;
         plot.low=plot.low>0?qMax(plot.low-pad,plot.low*.5):plot.low-pad;}
        if(std::abs(priceScale_-1)>1e-9){const auto centre=(plot.high+plot.low)/2,half=(plot.high-plot.low)/2*priceScale_;
            plot.high=centre+half;plot.low=qMax(centre-half,centre*1e-3);}
        plot.area={CandlePlotLeft,CandlePlotTop,double(qMax(1,width()-CandlePlotLeft-axisGutter(plot.low,plot.high))),qMax(1.0,height()-42-plot.volumeHeight-plot.cvdHeight)};
        plot.bounds=slotBounds(plot.rows,plot.axis);plot.available=true;return plot;
    }

std::optional<QPointF> CandleChart::drawingPosition(const ChartDrawingPoint& point,const Geometry& plot)const{
        const auto slot=plot.axis.annotationCoordinateAt(point.atMs,drawingFrame_.bars);bool ok=false;const auto price=point.price.toDouble(&ok);
        if(!slot||!ok||!std::isfinite(price)||price<=0)return {};
        return QPointF(plot.area.left()+(*slot-plot.bounds.first)/(plot.bounds.second-plot.bounds.first)*plot.area.width(),
            plot.area.bottom()-(price-plot.low)/(plot.high-plot.low)*plot.area.height());
    }

QPolygonF CandleChart::autoTrendPixels(const QList<ChartDrawingPoint>& anchors,const Geometry& plot)const{
        QPolygonF pixels;
        const auto path=AutoChartGeometry::trendDisplayPath(anchors,drawingFrame_,plot.axis);
        for(const QPointF& point:path){
            const auto x=plot.area.left()+(point.x()-plot.bounds.first)/(plot.bounds.second-plot.bounds.first)*plot.area.width();
            pixels.append({x,priceToY(point.y(),plot.low,plot.high,plot.area)});
        }
        return pixels;
    }

bool CandleChart::strongMagnetActive(Qt::KeyboardModifiers eventModifiers){
        const auto mods=eventModifiers|QGuiApplication::queryKeyboardModifiers();
        return mods.testFlag(Qt::ControlModifier);
    }

void CandleChart::updateMagnetSnap(const Geometry& plot){
        magnetGeometryDirty_=false;
        magnetSnapPos_.reset();magnetSnapField_.clear();magnetSnapPrice_.clear();
        const bool strong=strongMagnetActive(lastModifiers_);
        magnetStrongActive_=strong;
        const bool active=hoverInside_&&!dragging_&&!axisDragging_&&moveDrawing_.isEmpty()&&plot.available&&plot.area.contains(hoverPos_)&&
            drawingModel_&&drawingModel_->visible()&&!drawingModel_->locked()&&
            (strong||(drawingModel_->magnetEnabled()&&(tool_!=ChartDrawingTool::Crosshair||!dragDrawing_.isEmpty())));
        if(active){
            ChartDrawingSnap snap;
            const auto anchor=pointerAnchor(hoverPos_,plot,lastModifiers_,&snap);
            if(anchor&&snap.available&&snap.snapped){
                if(const auto pos=drawingPosition(snap.point,plot)){
                    magnetSnapPos_=pos;
                    magnetSnapField_=snap.field;
                    magnetSnapPrice_=snap.point.price;
                }
            }
        }
        setProperty("magnetSnapActive",magnetSnapPos_.has_value());
        setProperty("magnetStrongActive",strong);
        setProperty("magnetSnapField",magnetSnapField_);
        setProperty("magnetSnapPrice",magnetSnapPrice_);
        setProperty("magnetSnapPos",magnetSnapPos_.value_or(QPointF{}));
    }

void CandleChart::paintMagnetSnapRing(QPainter& painter)const{
        if(!magnetSnapPos_)return;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing,true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(themeColors().bg,2.0));
        painter.drawEllipse(*magnetSnapPos_,7.0,7.0);
        painter.setPen(QPen(themeColors().accent,2.0));
        painter.drawEllipse(*magnetSnapPos_,6.0,6.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(themeColors().accent);
        painter.drawEllipse(*magnetSnapPos_,2.2,2.2);
        painter.restore();
    }

CandleChart::SimilarityTableLayout CandleChart::similarityTableLayout(const Geometry& plot)const{
        SimilarityTableLayout out;
        if(!plot.available||!plot.following||studies_.similarity.paths.isEmpty()||plot.area.width()<140.0||plot.area.height()<80.0)return out;
        const auto& similar=studies_.similarity;
        const QFont captionFont=KeefiFonts::font(FontRole::Caption12);
        const QFont numFont=KeefiFonts::font(FontRole::Number11);
        const QFontMetricsF captionFm(captionFont),numFm(numFont);
        const auto formatSignedPct=[](double pct){
            if(!std::isfinite(pct))return QStringLiteral("-");
            if(std::abs(pct)<0.05)return QStringLiteral("0.0%");
            return (pct>0.0?QStringLiteral("+"):QString{})+QString::number(pct,'f',1)+QStringLiteral("%");
        };
        const QTimeZone kst("Asia/Seoul");
        double maxAfterW=captionFm.horizontalAdvance(tr("이후 20봉"));
        double sumPct=0.0;int pctCount=0;
        for(int i=0;i<similar.paths.size();++i){
            const auto& path=similar.paths[i];
            SimilarityTableLayout::Row row;
            row.rank=QString::number(i+1);
            row.endText=QDateTime::fromMSecsSinceEpoch(path.toMs,kst).toString(QStringLiteral("yy.MM.dd HH:mm"));
            row.corrText=QString::number(path.correlation,'f',2);
            if(path.points.size()>=2&&path.points.first().price>0.0){
                const double pct=(path.points.last().price/path.points.first().price-1.0)*100.0;
                row.afterText=formatSignedPct(pct);
                row.direction=pct>0.05?1:pct<-0.05?-1:0;
                sumPct+=pct;++pctCount;
            }else{
                row.afterText=QStringLiteral("-");
            }
            maxAfterW=qMax(maxAfterW,numFm.horizontalAdvance(row.afterText));
            out.rows.append(row);
        }
        const double meanPct=pctCount>0?(sumPct/pctCount):0.0;
        out.title=tr("닮은꼴 · 과거 비교");
        out.disclaimer=tr("과거 경로 · 예측 아님");
        out.summary=tr("닮은꼴 %1 · 이후 20봉 평균 %2").arg(similar.paths.size()).arg(formatSignedPct(meanPct));

        const double col0=qMax(captionFm.horizontalAdvance(QStringLiteral("#")),numFm.horizontalAdvance(QString::number(similar.paths.size())))+8.0;
        const double col1=qMax(captionFm.horizontalAdvance(tr("구간 끝")),numFm.horizontalAdvance(QStringLiteral("88.88.88 88:88")))+8.0;
        const double col2=qMax(captionFm.horizontalAdvance(tr("닮음")),numFm.horizontalAdvance(QStringLiteral("0.99")))+8.0;
        const double col3=maxAfterW+8.0;
        out.colWidths={col0,col1,col2,col3};
        const double padX=6.0,padY=6.0,titleH=18.0,headerH=18.0,rowH=22.0,footH=16.0;
        const double expandedW=qMin(plot.area.width()-8.0,padX*2.0+col0+col1+col2+col3);
        const double expandedH=padY*2.0+titleH+headerH+rowH*out.rows.size()+footH;

        const qint64 lastActualAt=forming_?forming_->startsAtMs:(rows_.isEmpty()?qint64(0):rows_.last().startsAtMs);
        const auto lastSlot=plot.axis.coordinateAt(lastActualAt);
        const double bodyWidth=qBound(1.0,plot.area.width()/(plot.bounds.second-plot.bounds.first)*.65,18.0);
        const double lastCandleRight=lastSlot
            ?(plot.area.left()+(*lastSlot-plot.bounds.first)/(plot.bounds.second-plot.bounds.first)*plot.area.width()+bodyWidth/2.0+6.0)
            :plot.area.left();
        const double projRight=plot.area.right()-4.0;
        const double projAvailW=projRight-lastCandleRight;
        out.projectionRect=QRectF(qBound(plot.area.left(),lastCandleRight-6.0,plot.area.right()),plot.area.top(),
            qMax(0.0,plot.area.right()-qBound(plot.area.left(),lastCandleRight-6.0,plot.area.right())),plot.area.height());

        const bool fitsProjection=(projAvailW>=expandedW)&&(plot.area.height()>=expandedH+8.0);
        out.alwaysExpanded=fitsProjection;
        out.expanded=fitsProjection||similarityTableHovered_;

        const double pillW=qMin(plot.area.width()-8.0,qMax(captionFm.horizontalAdvance(out.summary),captionFm.horizontalAdvance(out.disclaimer))+16.0);
        const double pillH=38.0;
        const double desiredLeft=out.expanded?qMax(plot.area.left()+4.0,projRight-expandedW):qMax(plot.area.left()+4.0,projRight-pillW);
        const bool clearsLegend=(desiredLeft>=plot.area.left()+340.0);
        const double top=(!clearsLegend&&plot.area.height()>=(out.expanded?expandedH:pillH)+CandleOhlcInset+CandleOhlcHeight+12.0)
            ?(plot.area.top()+CandleOhlcInset+CandleOhlcHeight+6.0)
            :(plot.area.top()+4.0);
        out.collapsedRect=QRectF(qMax(plot.area.left()+4.0,projRight-pillW),top,pillW,pillH);
        out.box=QRectF(qMax(plot.area.left()+4.0,projRight-expandedW),top,expandedW,qMin(expandedH,plot.area.bottom()-top-4.0));
        double yCur=out.box.top()+padY+titleH+headerH;
        for(int i=0;i<out.rows.size();++i){
            out.rowRects.append(QRectF(out.box.left()+2.0,yCur,out.box.width()-4.0,rowH));
            yCur+=rowH;
        }
        out.visible=true;
        return out;
    }

void CandleChart::paintSimilarityOverlay(QPainter& painter,const Geometry& plot){
        const auto clearProps=[&]{
            similarityTableHovered_=false;similarityHoverRow_=-1;
            setProperty("similarityTableVisible",false);
            setProperty("similarityTableExpanded",false);
            setProperty("similarityTableRect",QRectF{});
            setProperty("similarityTableSummary",QString{});
            setProperty("similarityTableRows",QStringList{});
            setProperty("similarityRowRects",QVariantList{});
            setProperty("similarityHoverRow",-1);
            setProperty("similarityTitle",QString{});
            setProperty("similarityDisclaimer",QString{});
        };
        const auto sim=similarityTableLayout(plot);
        if(!sim.visible){clearProps();return;}

        const QList<QColor> pathColors{themeColors().studyPurple,themeColors().studyBlue,themeColors().studyOrange};
        const auto& similar=studies_.similarity;
        const auto x=[&](qint64 at){
            const auto slot=plot.axis.coordinateAt(at);
            return slot?plot.area.left()+(*slot-plot.bounds.first)/(plot.bounds.second-plot.bounds.first)*plot.area.width():std::numeric_limits<double>::quiet_NaN();
        };
        const auto y=[&](double value){return plot.area.bottom()-(value-plot.low)/(plot.high-plot.low)*plot.area.height();};

        // Highlight the hovered historical path in D pass so hovering a table row immediately emphasizes its trajectory.
        if(similarityHoverRow_>=0&&similarityHoverRow_<similar.paths.size()){
            const auto& path=similar.paths[similarityHoverRow_];
            const QColor ink=pathColors[similarityHoverRow_%pathColors.size()];
            QPainterPath curve;bool first=true;QPointF lastPt;
            for(const auto& pt:path.points){
                const double xx=x(pt.atMs),yy=y(pt.price);
                if(!std::isfinite(xx)||!std::isfinite(yy))continue;
                lastPt=QPointF(xx,yy);
                if(first){curve.moveTo(lastPt);first=false;}else curve.lineTo(lastPt);
            }
            if(!first){
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing,true);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(ink,2.4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
                painter.drawPath(curve);
                painter.setBrush(ink);
                painter.setPen(QPen(themeColors().bg,1.2));
                painter.drawEllipse(lastPt,3.2,3.2);
                painter.restore();
            }
        }

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing,true);
        const QFont captionFont=KeefiFonts::font(FontRole::Caption12);
        const QFont numFont=KeefiFonts::font(FontRole::Number11);
        QColor cardBg=themeColors().bg;cardBg.setAlphaF(.94);
        painter.setBrush(cardBg);
        painter.setPen(QPen(themeColors().rule,1.0));

        const QRectF activeRect=sim.expanded?sim.box:sim.collapsedRect;
        painter.drawRoundedRect(activeRect,4.0,4.0);

        if(!sim.expanded){
            painter.setFont(captionFont);
            painter.setPen(themeColors().text);
            painter.drawText(QRectF(sim.collapsedRect.left()+8.0,sim.collapsedRect.top()+3.0,sim.collapsedRect.width()-16.0,16.0),Qt::AlignLeft|Qt::AlignVCenter,sim.summary);
            painter.setPen(themeColors().text2);
            painter.drawText(QRectF(sim.collapsedRect.left()+8.0,sim.collapsedRect.top()+19.0,sim.collapsedRect.width()-16.0,16.0),Qt::AlignLeft|Qt::AlignVCenter,sim.disclaimer);
        }else{
            const double left=sim.box.left()+6.0,right=sim.box.right()-6.0;
            const double w0=sim.colWidths[0],w1=sim.colWidths[1],w2=sim.colWidths[2],w3=qMax(24.0,right-left-w0-w1-w2);
            double yy=sim.box.top()+6.0;
            painter.setFont(captionFont);
            painter.setPen(themeColors().text);
            painter.drawText(QRectF(left,yy,right-left,18.0),Qt::AlignLeft|Qt::AlignVCenter,sim.title);
            yy+=18.0;

            painter.setPen(themeColors().text2);
            painter.drawText(QRectF(left,yy,w0,18.0),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("#"));
            painter.drawText(QRectF(left+w0,yy,w1,18.0),Qt::AlignLeft|Qt::AlignVCenter,tr("구간 끝"));
            painter.drawText(QRectF(left+w0+w1,yy,w2,18.0),Qt::AlignRight|Qt::AlignVCenter,tr("닮음"));
            painter.drawText(QRectF(left+w0+w1+w2,yy,w3,18.0),Qt::AlignRight|Qt::AlignVCenter,tr("이후 20봉"));
            painter.setPen(QPen(themeColors().rule,1.0));
            painter.drawLine(QPointF(left,yy+17.5),QPointF(right,yy+17.5));
            yy+=18.0;

            for(int i=0;i<sim.rows.size()&&i<sim.rowRects.size();++i){
                const auto& row=sim.rows[i];
                const auto& r=sim.rowRects[i];
                const QColor swatch=pathColors[i%pathColors.size()];
                if(i==similarityHoverRow_){
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(themeColors().surface);
                    painter.drawRoundedRect(r.adjusted(2.0,1.0,-2.0,-1.0),3.0,3.0);
                    painter.setBrush(swatch);
                    painter.drawRect(QRectF(r.left()+2.0,r.top()+3.0,2.5,r.height()-6.0));
                }
                painter.setFont(numFont);
                painter.setPen(swatch);
                painter.drawText(QRectF(left,r.top(),w0,r.height()),Qt::AlignLeft|Qt::AlignVCenter,row.rank);
                painter.setPen(themeColors().text);
                painter.drawText(QRectF(left+w0,r.top(),w1,r.height()),Qt::AlignLeft|Qt::AlignVCenter,row.endText);
                painter.setPen(themeColors().text2);
                painter.drawText(QRectF(left+w0+w1,r.top(),w2,r.height()),Qt::AlignRight|Qt::AlignVCenter,row.corrText);
                painter.setPen(row.direction==0?themeColors().text2:MovementColorPolicy::colorFor(this,movementForSign(row.direction)));
                painter.drawText(QRectF(left+w0+w1+w2,r.top(),w3,r.height()),Qt::AlignRight|Qt::AlignVCenter,row.afterText);
                yy=r.bottom();
            }
            painter.setFont(captionFont);
            painter.setPen(themeColors().text2);
            painter.drawText(QRectF(left,yy,right-left,16.0),Qt::AlignLeft|Qt::AlignVCenter,sim.disclaimer);
        }
        painter.restore();

        QStringList rowTexts;QVariantList rowRects;
        for(int i=0;i<sim.rows.size();++i){
            const auto& r=sim.rows[i];
            rowTexts.append(QStringLiteral("%1|%2|%3|%4").arg(r.rank,r.endText,r.corrText,r.afterText));
            if(i<sim.rowRects.size())rowRects.append(sim.rowRects[i]);
        }
        setProperty("similarityTableVisible",true);
        setProperty("similarityTableExpanded",sim.expanded);
        setProperty("similarityTableRect",activeRect);
        setProperty("similarityTableSummary",sim.summary);
        setProperty("similarityTableRows",rowTexts);
        setProperty("similarityRowRects",rowRects);
        setProperty("similarityHoverRow",similarityHoverRow_);
        setProperty("similarityTitle",sim.title);
        setProperty("similarityDisclaimer",sim.disclaimer);
    }

std::optional<ChartDrawingPoint> CandleChart::pointerAnchor(const QPointF& pos,const Geometry& plot,Qt::KeyboardModifiers modifiers,ChartDrawingSnap* snapOut)const{
        if(!plot.available||!plot.area.contains(pos)||!drawingModel_||drawingModel_->locked()||drawingFrame_.contextGeneration!=drawingModel_->contextGeneration())return {};
        const auto slot=plot.bounds.first+(pos.x()-plot.area.left())/plot.area.width()*(plot.bounds.second-plot.bounds.first);
        const auto observed=plot.axis.observedTimeAt(slot);
        const bool strong=strongMagnetActive(modifiers);
        if(strong){
            // Compare actual OHLC pixel positions, including neighbouring observed candles
            // when the plot is zoomed out. Preserve the source's exact decimal on selection.
            if(!validSnapFrame_)validSnapFrame_=ChartDrawings::validFrame(drawingFrame_);
            if(!*validSnapFrame_)return {};
            double best=StrongMagnetRadiusPx+1e-8;
            ChartDrawingSnap closest;
            const double slotWidth=plot.area.width()/(plot.bounds.second-plot.bounds.first);
            const double reach=StrongMagnetRadiusPx/slotWidth;
            const auto& bars=drawingFrame_.bars;
            const auto first=std::lower_bound(bars.cbegin(),bars.cend(),slot-reach-1e-8,
                [&](const auto& bar,double edge){return plot.axis.coordinateAt(bar.candle.startsAtMs).value_or(-std::numeric_limits<double>::infinity())<edge;});
            for(auto it=first;it!=bars.cend();++it){
                const auto& c=it->candle;
                const auto column=plot.axis.coordinateAt(c.startsAtMs);
                if(!column)continue;
                if(*column>slot+reach+1e-8)break;
                const double dx=(*column-slot)*slotWidth;
                for(const auto& [field,raw]:{qMakePair("close",c.close),qMakePair("open",c.open),
                                             qMakePair("high",c.high),qMakePair("low",c.low)}){
                    const double dy=priceToY(raw.toDouble(),plot.low,plot.high,plot.area)-pos.y();
                    const double distance=std::hypot(dx,dy);
                    if(distance<best){
                        best=distance;
                        closest.available=closest.snapped=true;
                        closest.field=QString::fromLatin1(field);
                        closest.point={c.startsAtMs,raw};
                    }
                }
            }
            if(closest.snapped){
                if(snapOut)*snapOut=closest;
                return closest.point;
            }
        }
        if(!observed||!ChartDrawings::readOhlc(drawingFrame_,*observed))return {};
        ChartDrawingPoint point{*observed,pixelPrice(plot.high-(pos.y()-plot.area.top())/plot.area.height()*(plot.high-plot.low),(plot.high-plot.low)/plot.area.height())};
        if(point.price.isEmpty())return {};
        if(!strong&&drawingModel_->magnetEnabled()){
            const auto snap=ChartDrawings::snapOhlc(drawingFrame_,point,plot.area.height()/(plot.high-plot.low),DefaultMagnetRadiusPx);
            if(snapOut)*snapOut=snap;
            if(snap.available&&snap.snapped)point=snap.point;
        }
        return point;
    }

QPair<QString,int> CandleChart::hitDrawing(const QPointF& pos,const Geometry& plot)const{
        if(!plot.area.contains(pos)||!drawingModel_||!drawingModel_->visible()||plot.high<=plot.low)return {};
        const auto shapes=drawingModel_->drawings();
        for(auto i=shapes.crbegin();i!=shapes.crend();++i){const auto& shape=*i;
            if(!shape.visible)continue;
            if(shape.kind==ChartDrawingKind::Horizontal&&!shape.anchors.isEmpty()){
                bool ok=false;const auto price=shape.anchors.first().price.toDouble(&ok);const auto yy=priceToY(price,plot.low,plot.high,plot.area);
                if(ok&&std::abs(yy-pos.y())<=7)return {shape.id,0};continue;
            }
            if(shape.kind==ChartDrawingKind::FibRetracement||shape.kind==ChartDrawingKind::FibExtension){
                if(shape.anchors.size()<2)continue;const auto a=drawingPosition(shape.anchors[0],plot);if(!a)continue;
                if(shape.id==selectedDrawing_)for(int idx=0;idx<shape.anchors.size();++idx)
                    if(const auto anchor=drawingPosition(shape.anchors[idx],plot);anchor&&QLineF(pos,*anchor).length()<=9)return {shape.id,idx};
                if(pos.x()>=a->x()&&pos.x()<=plot.area.right()){
                    for(const auto& level:ChartDrawings::fibLevels(shape)){
                        if(level.price<plot.low||level.price>plot.high)continue;
                        if(std::abs(priceToY(level.price,plot.low,plot.high,plot.area)-pos.y())<=7)return {shape.id,-1};
                    }
                }
                continue;
            }
            if(shape.kind==ChartDrawingKind::Zone){
                if(shape.anchors.size()!=2)continue;const auto a=drawingPosition(shape.anchors[0],plot),b=drawingPosition(shape.anchors[1],plot);if(!a||!b)continue;
                if(shape.id==selectedDrawing_){if(QLineF(pos,*a).length()<=9)return {shape.id,0};if(QLineF(pos,*b).length()<=9)return {shape.id,1};}
                if(QRectF(*a,*b).normalized().contains(pos))return {shape.id,-1};continue;
            }
            if(shape.anchors.size()!=2)continue;const auto a=drawingPosition(shape.anchors[0],plot),b=drawingPosition(shape.anchors[1],plot);if(!a||!b)continue;
            if(shape.id==selectedDrawing_){if(QLineF(pos,*a).length()<=9)return {shape.id,0};if(QLineF(pos,*b).length()<=9)return {shape.id,1};}
            const QPolygonF path=shape.kind==ChartDrawingKind::Trend&&!shape.autoBatchId.isEmpty()
                ?autoTrendPixels(shape.anchors,plot):QPolygonF{*a,*b};
            for(qsizetype j=1;j<path.size();++j){
                const auto start=path[j-1],delta=path[j]-start;
                const double lengthSquared=QPointF::dotProduct(delta,delta);
                const double t=lengthSquared>0?qBound(0.0,QPointF::dotProduct(pos-start,delta)/lengthSquared,1.0):0;
                if(QLineF(pos,start+delta*t).length()<=7)return {shape.id,-1};
            }
        }
        return {};
    }

std::optional<QList<ChartDrawingPoint>> CandleChart::movedAnchors(const QPointF& pos,const Geometry& plot)const{
        if(!plot.available||moveOriginal_.isEmpty()||!drawingModel_||drawingFrame_.contextGeneration!=drawingModel_->contextGeneration())return {};
        const auto span=plot.bounds.second-plot.bounds.first,range=plot.high-plot.low;if(span<=0||range<=0||plot.area.width()<=0||plot.area.height()<=0)return {};
        bool horizontal=false;for(const auto& shape:drawingModel_->drawings())if(shape.id==moveDrawing_)horizontal=shape.kind==ChartDrawingKind::Horizontal;
        const auto slotDelta=horizontal?0.0:std::round((pos.x()-movePressPos_.x())/plot.area.width()*span);
        const auto pricePerPixel=range/plot.area.height();const auto priceDelta=-(pos.y()-movePressPos_.y())*pricePerPixel;
        QList<ChartDrawingPoint> result;
        for(const auto& anchor:moveOriginal_){
            bool ok=false;const auto price=anchor.price.toDouble(&ok);if(!ok)return {};
            const auto moved=priceDelta==0?anchor.price:pixelPrice(price+priceDelta,pricePerPixel);if(moved.isEmpty())return {};
            auto at=anchor.atMs;
            if(slotDelta!=0){const auto slot=plot.axis.annotationCoordinateAt(anchor.atMs,drawingFrame_.bars);if(!slot)return {};
                const auto observed=plot.axis.observedTimeAt(*slot+slotDelta);if(!observed||!ChartDrawings::readOhlc(drawingFrame_,*observed))return {};at=*observed;}
            result.append({at,moved});
        }
        return result;
    }

void CandleChart::selectDrawing(const QString& id){if(selectedDrawing_==id)return;selectedDrawing_=id;if(drawingUiChanged)drawingUiChanged();refresh();}

void CandleChart::feedback(const QString& text){if(drawingFeedback)drawingFeedback(text);}

void CandleChart::paintDrawings(QPainter& painter,const Geometry& plot){
        offscreenTopTag_.reset();offscreenBottomTag_.reset();
        fibAxisLabels_.clear();
        const auto clearProps=[&]{
            setProperty("drawingOffscreenTopLabel",offscreenTopTag_?offscreenTopTag_->text:QString{});
            setProperty("drawingOffscreenBottomLabel",offscreenBottomTag_?offscreenBottomTag_->text:QString{});
        };
        if(!drawingModel_||!drawingModel_->visible()||plot.high<=plot.low||drawingFrame_.identity!=drawingModel_->context().identity||drawingFrame_.contextGeneration!=drawingModel_->contextGeneration()){clearProps();return;}
        QList<FibAxisLabel> inRangeFibLabels;
        std::optional<double> closestAbovePrice,closestBelowPrice;
        FibAxisLabel aboveCandidate,belowCandidate;
        painter.save();painter.setBrush(Qt::NoBrush);
        const auto applyDragPreview=[&](ChartDrawing shape){
            if(!dragDrawing_.isEmpty()&&shape.id==dragDrawing_&&preview_&&dragAnchor_>=0&&dragAnchor_<shape.anchors.size())shape.anchors[dragAnchor_]=*preview_;
            if(!moveDrawing_.isEmpty()&&shape.id==moveDrawing_&&movePreview_.size()==shape.anchors.size())shape.anchors=movePreview_;
            return shape;
        };
        const auto drawHandles=[&](const ChartDrawing& shape,const QColor& ink){
            painter.save();painter.setPen(QPen(ink,2));painter.setBrush(themeColors().bg);
            for(const auto& anchor:shape.anchors)if(const auto pos=drawingPosition(anchor,plot))painter.drawEllipse(*pos,4,4);
            painter.restore();
        };
        const auto drawSimple=[&](const ChartDrawing& shape,bool draft){
            const QColor color=shape.kind==ChartDrawingKind::Measure?themeColors().accent:themeColors().studyBlue;painter.setPen(QPen(color,draft?1:2,draft?Qt::DashLine:Qt::SolidLine));
            if(shape.kind==ChartDrawingKind::Horizontal&&!shape.anchors.isEmpty()){
                bool ok=false;const auto price=shape.anchors.first().price.toDouble(&ok);if(!ok||price<plot.low||price>plot.high)return;
                const auto yy=priceToY(price,plot.low,plot.high,plot.area);painter.drawLine(QPointF(plot.area.left(),yy),QPointF(plot.area.right(),yy));
                painter.save();painter.setFont(KeefiFonts::font(FontRole::Number11));
                painter.drawText(QRectF(plot.area.left()+6,yy-20,plot.area.width()-12,18),Qt::AlignLeft,priceAmount(shape.anchors.first().price)+" "+currency_);painter.restore();
            }else if(shape.anchors.size()==2){
                const auto a=drawingPosition(shape.anchors[0],plot),b=drawingPosition(shape.anchors[1],plot);if(!a||!b)return;
                if(shape.kind==ChartDrawingKind::Trend&&!shape.autoBatchId.isEmpty())
                    painter.drawPolyline(autoTrendPixels(shape.anchors,plot));
                else painter.drawLine(*a,*b);
                if(shape.kind==ChartDrawingKind::Measure){const auto measure=ChartDrawings::measure(shape.anchors,drawingFrame_);QString text;
                    if(measure.returnPercent)text=QString::number(*measure.returnPercent,'f',2)+"%";
                    text+=measure.barIntervals?tr(" · %1봉 간격").arg(*measure.barIntervals):tr(" · 봉 간격 미확인");
                    painter.save();painter.setPen(themeColors().accentText);
                    painter.drawText(QRectF(qMin(a->x(),b->x())+4,qMin(a->y(),b->y())-22,qMax(180.0,std::abs(a->x()-b->x())),20),Qt::AlignLeft,text);painter.restore();}
            }
            if(shape.id==selectedDrawing_&&!draft)drawHandles(shape,color);
        };
        for(const auto& raw:drawingModel_->drawings()){
            if(!raw.visible)continue;
            const auto shape=applyDragPreview(raw);
            if(shape.kind==ChartDrawingKind::FibRetracement||shape.kind==ChartDrawingKind::FibExtension){
                if(shape.anchors.size()<2)continue;const auto a=drawingPosition(shape.anchors[0],plot);if(!a||a->x()>plot.area.right())continue;
                const bool extension=(shape.kind==ChartDrawingKind::FibExtension);
                const QColor color=extension?themeColors().studyBlue:themeColors().accent;
                painter.setPen(QPen(color,1,extension?Qt::DashLine:Qt::SolidLine));
                const double x0=qMax(plot.area.left(),a->x());
                for(const auto& level:ChartDrawings::fibLevels(shape)){
                    const QString text=QStringLiteral("%1 %2").arg(formatFibRatio(level.ratio),formatFibPrice(level.price,currency_));
                    if(level.price>=plot.low&&level.price<=plot.high){
                        const double yy=priceToY(level.price,plot.low,plot.high,plot.area);
                        painter.drawLine(QPointF(x0,yy),QPointF(plot.area.right(),yy));
                        inRangeFibLabels.append({text,color,yy});
                    }else if(level.price>plot.high){
                        if(!closestAbovePrice||level.price<*closestAbovePrice){closestAbovePrice=level.price;aboveCandidate={QStringLiteral("▲ ")+text,color,0};}
                    }else{
                        if(!closestBelowPrice||level.price>*closestBelowPrice){closestBelowPrice=level.price;belowCandidate={QStringLiteral("▼ ")+text,color,0};}
                    }
                }
                if(shape.id==selectedDrawing_)drawHandles(shape,color);
            }else if(shape.kind==ChartDrawingKind::Zone){
                if(shape.anchors.size()!=2)continue;const auto a=drawingPosition(shape.anchors[0],plot),b=drawingPosition(shape.anchors[1],plot);if(!a||!b)continue;
                QColor fill=themeColors().accent;fill.setAlpha(40);
                painter.fillRect(QRectF(*a,*b).normalized(),fill);
                if(shape.id==selectedDrawing_)drawHandles(shape,themeColors().accent);
            }else{
                drawSimple(shape,false);
            }
        }
        std::stable_sort(inRangeFibLabels.begin(),inRangeFibLabels.end(),[](const FibAxisLabel& a,const FibAxisLabel& b){
            return a.occupiedY<b.occupiedY;
        });
        for(const auto& label:inRangeFibLabels){
            if(!fibAxisLabels_.isEmpty()&&label.occupiedY-fibAxisLabels_.last().occupiedY<FibLabelGap)continue;
            fibAxisLabels_.append(label);
        }
        if(closestAbovePrice){aboveCandidate.occupiedY=currency_.isEmpty()?plot.area.top()+8.0:plot.area.top()+19.0;offscreenTopTag_=aboveCandidate;}
        if(closestBelowPrice){belowCandidate.occupiedY=plot.area.bottom()-8.0;offscreenBottomTag_=belowCandidate;}
        clearProps();
        if(draft_.size()==1&&preview_)drawSimple({{},tool_==ChartDrawingTool::Measure?ChartDrawingKind::Measure:ChartDrawingKind::Trend,{draft_.first(),*preview_},{},true,false,{}},true);
        painter.restore();
    }

void CandleChart::paintOffscreenFibTags(QPainter& painter,const Geometry& plot)const{
        if(!offscreenTopTag_&&!offscreenBottomTag_)return;
        painter.save();painter.setFont(KeefiFonts::font(FontRole::Number11));
        const QFontMetricsF fm(painter.font());
    const auto drawTag=[&](const FibAxisLabel& tag,qreal top){
            const qreal w=fm.horizontalAdvance(tag.text)+6.0;
        const qreal axisLeft=plot.area.right()+4.0;
        const qreal x=(axisLeft+w<=width()-2.0)?axisLeft:qMax(plot.area.left(),width()-w-2.0);
            const QRectF box(x,top,w,15.0);
            painter.fillRect(box,themeColors().bg);painter.setPen(tag.color);
            painter.drawText(box.adjusted(3,0,-3,0),Qt::AlignLeft|Qt::AlignVCenter,tag.text);
        };
        if(offscreenTopTag_)drawTag(*offscreenTopTag_,currency_.isEmpty()?plot.area.top():plot.area.top()+11.0);
        if(offscreenBottomTag_)drawTag(*offscreenBottomTag_,plot.area.bottom()-15.0);
        painter.restore();
    }

void CandleChart::paintFibAxisLabels(QPainter& painter,const QRectF& area,qreal axisWidth,QList<qreal>& occupied){
        QStringList drawnFibLabels;
        if(!fibAxisLabels_.isEmpty())std::sort(occupied.begin(),occupied.end());
        for(const auto& label:fibAxisLabels_){
            if(painter.fontMetrics().horizontalAdvance(label.text)>axisWidth)continue;
            const auto centre=nearestFibLabelCentre(label.occupiedY,area.top()+FibLabelHeight/2,
                area.bottom()-FibLabelHeight/2,occupied);
            if(!centre)continue;
            painter.setPen(label.color);
            // Keep the anchor at its actual price when a price marker occupies the label's row.
            if(std::abs(*centre-label.occupiedY)>.5)
                painter.drawLine(QPointF(area.right()-FibLabelConnectorWidth,label.occupiedY),QPointF(area.right(),*centre));
            painter.drawText(QRectF(area.right()+5,*centre-FibLabelHeight/2,axisWidth,FibLabelHeight),
                Qt::AlignLeft|Qt::AlignVCenter,label.text);
            occupied.insert(std::lower_bound(occupied.cbegin(),occupied.cend(),*centre),*centre);
            drawnFibLabels.append(label.text);
        }
        setProperty("drawingLabelTexts",drawnFibLabels);
    }

void CandleChart::paintAutoPreview(QPainter& painter,const Geometry& plot)const{
        if(autoPreview_.isEmpty()||!plot.available||plot.high<=plot.low)return;
        const double progress=qBound(0.0,autoPreviewProgress_,1.0);
        painter.save();painter.setBrush(Qt::NoBrush);painter.setFont(KeefiFonts::font(FontRole::Number11));
        for(const auto& item:autoPreview_){
            if(!item.visible)continue;
            const auto& c=item.candidate;
            if(c.kind==ChartDrawingKind::FibRetracement||c.kind==ChartDrawingKind::FibExtension){
                if(c.anchors.size()<2)continue;const auto a=drawingPosition(c.anchors[0],plot);if(!a||a->x()>plot.area.right())continue;
                const bool extension=(c.kind==ChartDrawingKind::FibExtension);
                const QColor color=extension?themeColors().studyBlue:themeColors().accent;
                painter.setPen(QPen(color,1,extension?Qt::DashLine:Qt::SolidLine));
                ChartDrawing shape;shape.kind=c.kind;shape.anchors=c.anchors;
                const double x0=qMax(plot.area.left(),a->x()),x1=x0+(plot.area.right()-x0)*progress;
                for(const auto& level:ChartDrawings::fibLevels(shape)){
                    if(level.price<plot.low||level.price>plot.high)continue;
                    const double yy=priceToY(level.price,plot.low,plot.high,plot.area);
                    painter.drawLine(QPointF(x0,yy),QPointF(x1,yy));
                }
            }else if(c.kind==ChartDrawingKind::Zone){
                if(c.anchors.size()!=2)continue;const auto a=drawingPosition(c.anchors[0],plot),b=drawingPosition(c.anchors[1],plot);if(!a||!b)continue;
                QColor fill=themeColors().accent;fill.setAlpha(qRound(40.0*progress));
                painter.fillRect(QRectF(*a,*b).normalized(),fill);
            }else if(c.kind==ChartDrawingKind::Horizontal&&!c.anchors.isEmpty()){
                bool ok=false;const double price=c.anchors[0].price.toDouble(&ok);if(!ok||price<plot.low||price>plot.high)continue;
                const double yy=priceToY(price,plot.low,plot.high,plot.area);
                const auto a=drawingPosition(c.anchors[0],plot);
                const double anchorX=a?qMax(plot.area.left(),a->x()):plot.area.left();
                const double x1=plot.area.left()+plot.area.width()*progress;
                painter.setPen(QPen(themeColors().accent,1.5,Qt::DashLine));
                painter.drawLine(QPointF(plot.area.left(),yy),QPointF(x1,yy));
                if(progress>=1.0&&c.touches>0)
                    paintTextBadge(painter,QPointF(qBound(plot.area.left()+80.0,anchorX,plot.area.right()-4.0),yy-2.0),tr("%1번 닿음").arg(c.touches),themeColors().text2,themeColors().bg,plot.area,true);
            }else if(c.kind==ChartDrawingKind::Trend&&c.anchors.size()==2){
                const auto a=drawingPosition(c.anchors[0],plot),b=drawingPosition(c.anchors[1],plot);if(!a||!b)continue;
                painter.setPen(QPen(themeColors().studyBlue,1.5,Qt::SolidLine));
                const auto path=autoTrendPixels(c.anchors,plot);
                if(path.isEmpty())continue;
                painter.save();
                if(progress<1.0&&a->x()!=b->x()){
                    const auto limit=a->x()+(b->x()-a->x())*progress;
                    painter.setClipRect(QRectF(qMin(a->x(),limit),plot.area.top(),std::abs(limit-a->x()),plot.area.height()),Qt::IntersectClip);
                }
                painter.drawPolyline(path);
                painter.restore();
                if(progress>=1.0){
                    if(c.touches>0)
                        paintTextBadge(painter,QPointF(b->x(),b->y()-4.0),tr("%1번 닿음").arg(c.touches),themeColors().text2,themeColors().bg,plot.area,true);
                    for(const qint64 sweepAt:c.sweepAtMs){
                        const auto slot=plot.axis.coordinateAt(sweepAt);if(!slot)continue;
                        const double sx=plot.area.left()+(*slot-plot.bounds.first)/(plot.bounds.second-plot.bounds.first)*plot.area.width();
                        const MarketCandle* match=nullptr;for(const auto& row:plot.rows)if(row.startsAtMs==sweepAt){match=&row;break;}
                        if(!match)continue;
                        const double lowY=priceToY(match->low.toDouble(),plot.low,plot.high,plot.area);
                        const double highY=priceToY(match->high.toDouble(),plot.low,plot.high,plot.area);
                        const bool lowerSweep=!c.trendHighSide;
                        const double tipY=lowerSweep?qMin(plot.area.bottom()-2.0,lowY+3.0):qMax(plot.area.top()+2.0,highY-3.0);
                        const double dir=lowerSweep?1.0:-1.0;
                        painter.setPen(QPen(themeColors().accent,1.5));
                        painter.drawLine(QPointF(sx,tipY),QPointF(sx,tipY+dir*6.0));
                        painter.drawLine(QPointF(sx-2.5,tipY+dir*2.0),QPointF(sx+2.5,tipY+dir*2.0));
                    }
                }
            }
        }
        painter.restore();
    }

ChartTimeAxis CandleChart::timeAxis()const{
        if(!timeAxisCache_){
            if(stock_)timeAxisCache_=ChartTimeAxis::stock(rows_,intervalMs_,timeline_.forCloseDisplay(),forming_?forming_->startsAtMs:0);
            else{auto all=rows_;if(forming_)all.append(*forming_);timeAxisCache_=ChartTimeAxis::utc(all,intervalMs_);}
        }
        return *timeAxisCache_;
    }

bool CandleChart::showsFollowing(const ChartTimeAxis& axis)const{
        if(syncRange_.valid()||offset_||!layers_.contains("similarity")||!studies_.similarity.available||studies_.similarity.maximum.isEmpty())return false;
        for(const auto& points:{studies_.similarity.mean,studies_.similarity.minimum,studies_.similarity.maximum})
            for(const auto& point:points)if(!axis.coordinateAt(point.atMs))return false;
        for(const auto& path:studies_.similarity.paths)for(const auto& point:path.points)if(!axis.coordinateAt(point.atMs))return false;
        return true;
    }

double CandleChart::computedProjectionSlots(const ChartTimeAxis& axis)const{
        if(!layers_.contains("similarity")||!studies_.similarity.available||studies_.similarity.maximum.isEmpty()||!axis.available)return 0;
        const auto actual=forming_?forming_->startsAtMs:rows_.isEmpty()?qint64(0):rows_.last().startsAtMs;
        const auto last=axis.coordinateAt(actual),future=axis.coordinateAt(studies_.similarity.maximum.last().atMs);
        return last&&future?qMax(0.0,*future-*last):0;
    }

double CandleChart::viewSpan(const ChartTimeAxis& axis)const{
        if(!stock_&&syncRange_.valid())return double(syncRange_.toMs-syncRange_.fromMs)/intervalMs_;
        const auto first=rows_.isEmpty()?(forming_?forming_->startsAtMs:0):rows_.first().startsAtMs;
        const auto available=latestRight(axis)-(axis.coordinateAt(first).value_or(0)-.5);
        return qMax(1.0,qMin(qMin(200.0,count_+projectionSlots_),available));
    }

double CandleChart::computedDrawingPadding(const ChartTimeAxis& axis)const{
        if(!drawingModel_||!drawingModel_->visible()||rows_.isEmpty()||!axis.available)return 0;
        // A saved smaller-timeframe anchor can fall beyond the centre + .5
        // edge of the last completed larger bar. Admit only anchors proven to
        // belong to that actual closed bar, never arbitrary future timestamps.
        QList<ChartDrawingBar> lastClosed;
        for(const auto& bar:drawingFrame_.bars)if(bar.sourceCompleted&&bar.candle.startsAtMs==rows_.last().startsAtMs){lastClosed.append(bar);break;}
        const auto last=forming_?forming_->startsAtMs:rows_.last().startsAtMs;
        const auto base=axis.coordinateAt(last).value_or(0)+.5;double padding=0;
        for(const auto& shape:drawingModel_->drawings())for(const auto& anchor:shape.anchors)
            if(const auto slot=axis.annotationCoordinateAt(anchor.atMs,lastClosed))padding=qMax(padding,*slot+.1-base);
        return padding;
    }

double CandleChart::latestRight(const ChartTimeAxis& axis)const{
        const auto last=forming_?forming_->startsAtMs:rows_.isEmpty()?qint64(0):rows_.last().startsAtMs;
        return axis.coordinateAt(last).value_or(0)+.5+qMax(projectionSlots_,drawingPadding_)+edgePadding_;
    }

QPair<double,double> CandleChart::navigationBounds(const ChartTimeAxis& axis)const{
        if(!stock_&&syncRange_.valid()){
            const auto left=axis.coordinateAt(syncRange_.fromMs),right=axis.coordinateAt(syncRange_.toMs);
            if(left&&right)return {*left,*right};
        }
        const auto right=latestRight(axis)-offset_;
        return {right-viewSpan(axis),right};
    }

QPair<double,double> CandleChart::slotBounds(const QList<MarketCandle>&,const ChartTimeAxis& axis)const{return navigationBounds(axis);}

qint64 CandleChart::observedFlowEnd(const QList<MarketCandle>& rows)const{return rows.last().startsAtMs+intervalMs_/2;}

QString CandleChart::stockRowDescription(qint64 at)const{
        if(!stock_)return {};
        for(const auto& context:timeline_.rows)if(context.startsAtMs==at){
            const QMap<StockSessionKind,QString> sessions{{StockSessionKind::Day,tr("데이마켓")},{StockSessionKind::Pre,tr("프리마켓")},
                {StockSessionKind::Regular,tr("정규장")},{StockSessionKind::After,tr("애프터마켓")},{StockSessionKind::Mixed,tr("혼합 경계")}};
            return tr("\n거래일 %1 · %2 · 완료 경계 %3%4").arg(context.tradingDate.toString(Qt::ISODate),sessions.value(context.session,tr("세션 미확인")),sourceTime(context.completesAtMs),
                context.eligible?QString{}:tr("\n판정 제외 · %1").arg(context.reason));
        }
        return tr("\n형성 중 · 완료 봉 판정 제외");
    }

void CandleChart::clampOffset(){
        const auto axis=timeAxis();if(!axis.available){offset_=0;return;}
        const auto first=rows_.isEmpty()?(forming_?forming_->startsAtMs:0):rows_.first().startsAtMs;
        const auto maximum=qMax(0.0,latestRight(axis)-viewSpan(axis)-(axis.coordinateAt(first).value_or(0)-.5));
        offset_=qBound(0.0,offset_,maximum);
    }

QList<MarketCandle> CandleChart::visible()const{
        const auto axis=timeAxis();if(!axis.available)return {};
        const auto bounds=navigationBounds(axis);
        if(visibleRowsCache_&&visibleRowsCache_->bounds==bounds)return visibleRowsCache_->rows;
        QList<MarketCandle> result;
        // Valid time axes preserve the strictly increasing order of observed coordinates,
        // including compressed stock closures and explicit gaps.
        const auto first=std::upper_bound(rows_.cbegin(),rows_.cend(),bounds.first,[&](double slot,const auto& row){return slot<*axis.coordinateAt(row.startsAtMs);});
        const auto last=std::lower_bound(first,rows_.cend(),bounds.second,[&](const auto& row,double slot){return *axis.coordinateAt(row.startsAtMs)<slot;});
        result.reserve(std::distance(first,last)+1);
        for(auto it=first;it!=last;++it)result.append(*it);
        if(forming_){const auto at=axis.coordinateAt(forming_->startsAtMs);if(at&&*at>bounds.first&&*at<bounds.second)result.append(*forming_);}
        visibleRowsCache_=VisibleRows{bounds,result};
        return result;
    }

void CandleChart::publishViewport(){
        geometryCache_.reset();
        const auto range=shareViewport();const auto plot=plotGeometry();
        setProperty("viewportFirstMs",range.fromMs);setProperty("viewportLastMs",range.toMs);
        setProperty("viewportZoomBars",count_);setProperty("viewportSpanSlots",plot.bounds.second-plot.bounds.first);
        setProperty("viewportLive",followsLive());
        setProperty("viewportOffsetSlots",syncRange_.valid()?latestRight(plot.axis)-plot.bounds.second:offset_);
        setProperty("viewportPlotLeft",plot.area.left());setProperty("viewportPlotWidth",plot.area.width());
        setProperty("viewportPlotTop",plot.area.top());setProperty("viewportPlotHeight",plot.area.height());setProperty("viewportPriceLow",plot.low);setProperty("viewportPriceHigh",plot.high);
        const auto coordinate=plot.axis.coordinateAt(range.fromMs);
        setProperty("viewportFirstBarX",coordinate&&plot.available?plot.area.left()+(*coordinate-plot.bounds.first)/(plot.bounds.second-plot.bounds.first)*plot.area.width():0);
        ChartSyncViewport sync;
        if(!stock_){
            if(syncRange_.valid())sync=syncRange_;
            else if(!rows_.isEmpty()&&plot.axis.available){const auto origin=rows_.first().startsAtMs;
                sync={origin+qint64(std::llround(plot.bounds.first*intervalMs_)),origin+qint64(std::llround(plot.bounds.second*intervalMs_))};}
        }
        syncPublishedViewport_=sync;syncMarkerPlot_=plot.area;setProperty("syncViewportFromMs",sync.fromMs);setProperty("syncViewportToMs",sync.toMs);refreshSyncedMarker();
        if(viewportChanged)viewportChanged();
        requestOlderIfNeeded(range);
    }

void CandleChart::requestOlderIfNeeded(const ChartShareViewport& range){
        if(olderRequested&&!stock_&&!rows_.isEmpty()&&range.fromMs>0){
            const auto first=std::lower_bound(rows_.cbegin(),rows_.cend(),range.fromMs,[](const auto& row,qint64 at){return row.startsAtMs<at;});
            if(std::distance(rows_.cbegin(),first)<=qMax(30,count_/2))olderRequested();
        }
    }

void CandleChart::publishUserNavigation(bool zoom){
        const auto range=syncViewport();if(!range.valid())return;
        setProperty("userViewportEventCount",property("userViewportEventCount").toULongLong()+1);
        if(userNavigated)userNavigated(range,zoom);
    }

void CandleChart::zoomSynced(int delta,double fraction){
        if(!delta||!syncRange_.valid()||rows_.isEmpty()||intervalMs_<=0)return;const auto before=syncRange_;const qint64 span=before.toMs-before.fromMs;
        const qint64 nextSpan=qint64(qBound(2.0,double(span)/intervalMs_+delta,5000.0)*intervalMs_);
        const qint64 anchor=before.fromMs+qint64(fraction*span);
        syncRange_={anchor-qint64(fraction*nextSpan),anchor+qint64((1-fraction)*nextSpan)};
        count_=qBound(20,int(nextSpan/intervalMs_),200);publishViewport();refresh();if(syncRange_!=before)publishUserNavigation(true);
    }

void CandleChart::refreshSyncedMarker(){
        if(!syncMarker_)return;const auto range=syncPublishedViewport_;const auto* row=chartSyncCandleAt(rows_,intervalMs_,syncCrosshairMs_);
        const bool valid=!stock_&&range.valid()&&row&&syncCrosshairMs_>=range.fromMs&&syncCrosshairMs_<=range.toMs;
        if(!valid)syncCrosshairMs_=0;
        setProperty("syncCrosshairMs",syncCrosshairMs_);setProperty("syncCrosshairPrice",valid?row->close.toDouble():0);
        const auto& area=syncMarkerPlot_;const auto x=valid?std::optional<double>(area.left()+double(syncCrosshairMs_-range.fromMs)/(range.toMs-range.fromMs)*area.width()):std::nullopt;
        syncMarker_->setMarker(area,x);
    }

} // namespace keefi::market_candles_detail
