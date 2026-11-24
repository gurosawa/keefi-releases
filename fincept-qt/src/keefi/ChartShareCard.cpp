#include "keefi/KeefiTheme.h"
#include "keefi/ProductIdentity.h"
#include "keefi/KeefiFonts.h"
#include "keefi/ChartShareCard.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/ChartTimeAxis.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/StockChartSource.h"
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImageWriter>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <QTextLayout>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
constexpr qint64 Minute=60000,Day=86400000;
const QString InvalidInput=QStringLiteral("공개 차트 자료·좌표·계산 범위를 확인할 수 없습니다.");
struct Decimal {QString digits="0";int scale=0;};
Decimal exact(const QString& source) {
    QString digits=source;const int dot=int(digits.indexOf('.')),scale=dot<0?0:int(digits.size())-dot-1;if(dot>=0)digits.remove(dot,1);
    int first=0;while(first+1<digits.size()&&digits[first]=='0')++first;if(first)digits.remove(0,first);if(digits=="0")return {};
    int compactScale=scale;while(compactScale>0&&digits.endsWith('0')){digits.chop(1);--compactScale;}return {digits,compactScale};
}
int compare(const Decimal& left,const Decimal& right) {
    const int scale=std::max(left.scale,right.scale);const auto a=left.digits=="0"?QString("0"):left.digits+QString(scale-left.scale,'0');
    const auto b=right.digits=="0"?QString("0"):right.digits+QString(scale-right.scale,'0');if(a.size()!=b.size())return a.size()<b.size()?-1:1;return QString::compare(a,b);
}
bool number(const QString& text,bool price,double* value=nullptr) {
    static const QRegularExpression grammar("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z");
    if(text.isEmpty()||text.size()>ChartDrawings::MaxPriceChars||!grammar.match(text).hasMatch())return false;
    static const Decimal minimum{"1",100},maximum{QString("1")+QString(100,'0'),0};const auto decimal=exact(text);
    if(compare(decimal,maximum)>0||(price&&compare(decimal,minimum)<0))return false;
    if(value){bool ok=false;const double parsed=text.toDouble(&ok);if(!ok||!std::isfinite(parsed))return false;*value=parsed;}return true;
}
bool validEnum(ChartShareAvailability value) {
    return value==ChartShareAvailability::Ready||value==ChartShareAvailability::Waiting||value==ChartShareAvailability::Unavailable||value==ChartShareAvailability::Stale||value==ChartShareAvailability::Partial;
}
bool validAdjustment(StockAdjustment value) {
    return value==StockAdjustment::Unknown||value==StockAdjustment::SourceAdjusted||value==StockAdjustment::SourceUnadjusted||value==StockAdjustment::CloseSplitAdjustedOnly;
}
bool canonicalSource(const ChartShareSnapshot& value) {
    if(!WatchlistWorkspaceList::validIdentity(value.identity)||value.source.canonicalUrl.size()>2048)return false;
    const QUrl url(value.source.canonicalUrl,QUrl::StrictMode);
    if(!url.isValid()||url.scheme()!="https"||url.port()!=-1||!url.userInfo().isEmpty()||url.hasFragment()||url.toString()!=value.source.canonicalUrl)return false;
    const bool stock=value.identity.market=="stock";
    if(stock){if(value.currency!=(value.identity.source=="KR"?QString("KRW"):QString("USD")))return false;}
    else if(value.identity.source=="upbit") {if(value.currency!=value.identity.code.section('-',0,0))return false;}
    else if(value.identity.source=="binance") {
        if(!binanceIdentifierValid(value.currency)||!value.identity.code.endsWith(value.currency)||
           !binanceIdentifierValid(value.identity.code.left(value.identity.code.size()-value.currency.size())))return false;
    }else return false;
    switch(value.source.provider){
    case ChartShareSource::Upbit:case ChartShareSource::BinanceSpot:{
        const QString expectedProvider=value.source.provider==ChartShareSource::Upbit?QString("upbit"):QString("binance");
        if(stock||value.identity.source!=expectedProvider||value.source.delayed||value.source.adjustment!=StockAdjustment::Unknown)return false;
        const auto request=MarketCandles::request(value.identity.key(),value.intervalMs);return !request.url.isEmpty()&&value.source.canonicalUrl==request.url.toString();}
    case ChartShareSource::Toss:{
        if(!stock||value.source.delayed)return false;
        const auto request=StockChartSource::request(StockChartRead::Candles,value.identity,value.intervalMs==Day?Day:Minute);
        return !request.url.isEmpty()&&value.source.canonicalUrl==request.url.toString();}
    }return false;
}
bool studyBounds(const ChartStudySet& study) {
    if(study.cloud.size()>200||study.volumes.size()>200||study.entries.size()>200||study.scorecard.outcomes.size()>200||study.pullback.evidence.size()>4||
       study.similarity.paths.size()>3||study.similarity.mean.size()>21||study.similarity.minimum.size()>21||study.similarity.maximum.size()>21)return false;
    for(const auto& path:study.similarity.paths)if(path.points.size()>21)return false;
    for(const auto& evidence:study.pullback.evidence)if(evidence.id.size()>32)return false;
    for(const auto& volume:study.volumes)if(volume.volume.size()>ChartDrawings::MaxPriceChars)return false;return true;
}
// Compare only canonical public facts. Caller-supplied labels and reasons are
// neither interpreted nor painted, and cannot affect exported pixels/captions.
QByteArray studyFacts(const ChartStudySet& r) {
    QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);out.setFloatingPointPrecision(QDataStream::DoublePrecision);
    out<<r.available<<r.intervalMs<<r.lastCompletedAtMs<<r.observations<<r.currentRunBars;
    const auto& p=r.pullback;out<<p.available<<p.inBox<<p.waveLowAtMs<<p.waveHighAtMs<<p.waveLow<<p.waveHigh<<p.boxLow<<p.boxHigh<<p.atr<<p.overlap<<p.score<<p.evidence.size();
    for(const auto& evidence:p.evidence)out<<evidence.id<<evidence.price;
    const auto& s=r.similarity;out<<s.available<<s.volatilityNormalized<<s.queryBars<<s.followingBars<<s.searchBars<<s.eligibleCandidates<<s.overlapDropped<<s.samples<<s.upPaths<<s.downPaths<<s.flatPaths<<s.searchFromMs<<s.searchToMs<<s.paths.size();
    const auto points=[&](const QList<ChartPricePoint>& values){out<<values.size();for(const auto& point:values)out<<point.atMs<<point.price;};
    for(const auto& path:s.paths){out<<path.fromMs<<path.toMs<<path.followingToMs<<path.correlation;points(path.points);}points(s.mean);points(s.minimum);points(s.maximum);
    out<<r.cloud.size();for(const auto& point:r.cloud)out<<point.atMs<<point.fast<<point.slow;
    out<<r.volumes.size();for(const auto& point:r.volumes)out<<point.atMs<<point.volume;
    out<<r.entries.size();for(const auto& entry:r.entries)out<<entry.atMs<<entry.waveLowAtMs<<entry.entryClose<<entry.score;
    const auto& card=r.scorecard;out<<card.available<<card.costsExcluded<<card.horizonMs<<card.samples<<card.unresolvedSignals<<card.overlappingSignals<<card.up<<card.down<<card.flat<<card.meanReturnPercent<<card.outcomes.size();
    for(const auto& outcome:card.outcomes)out<<outcome.signalAtMs<<outcome.evaluationAtMs<<outcome.returnPercent;return bytes;
}
struct Prepared {
    ChartStudySet studies;ChartTimeAxis axis;QList<ChartDrawingBar> drawingBars;QList<MarketCandle> visible;int firstVisible=0;
};
bool prepare(const ChartShareSnapshot& value,const ChartShareOptions& options,Prepared* result) {
    if(!options.maskPersonalValues||options.outputSize.width()<ChartShareCard::MinWidth||options.outputSize.height()<ChartShareCard::MinHeight||
       options.outputSize.width()>ChartShareCard::MaxDimension||options.outputSize.height()>ChartShareCard::MaxDimension||
       ChartDrawings::bucketForInterval(value.intervalMs).isEmpty()||value.asOfMs<=0||!validEnum(value.availability)||!validAdjustment(value.source.adjustment)||
       (value.colors!=MovementScheme::GreenRed&&value.colors!=MovementScheme::KoreanRedBlue)||value.displayTimeZoneId.isEmpty()||value.displayTimeZoneId.size()>128||
       !value.displayTimeZoneId.isValidUtf8()||!QTimeZone(value.displayTimeZoneId).isValid()||value.closed.size()>ChartShareCard::MaxClosedBars||value.drawings.size()>ChartShareCard::MaxDrawings||!studyBounds(value.studies)||!canonicalSource(value))return false;
    if(value.source.receivedAtMs<0||value.source.receivedAtMs>value.asOfMs||value.source.sourceTimeMs<0||
       (value.source.sourceTimeMs>0&&(value.source.receivedAtMs==0||value.source.sourceTimeMs>value.source.receivedAtMs)))return false;
    static const QSet<QString> layers{"pullback","similarity","cloud","volume"};for(const auto& layer:value.visiblePublicLayers)if(!layers.contains(layer))return false;
    const bool stock=value.identity.market=="stock";
    if(stock!=value.stockTimeline.has_value())return false;
    if(value.closed.isEmpty()){
        if(value.viewport.fromMs!=0||value.viewport.toMs!=0||value.calculatedThroughMs!=0||studyFacts(value.studies)!=studyFacts({})||
           (value.stockTimeline&&(!value.stockTimeline->rows.isEmpty()||!value.stockTimeline->nextStartsAtMs.isEmpty())))return false;
        return !options.includePublicDrawings||value.drawings.isEmpty();
    }
    if(value.source.receivedAtMs<=0||value.viewport.toMs!=value.closed.last().startsAtMs||value.viewport.fromMs<=0||value.viewport.fromMs>value.viewport.toMs)return false;
    if(stock&&value.stockTimeline->rows.size()!=value.closed.size())return false;
    qint64 previous=0;bool foundStart=false;
    for(int i=0;i<value.closed.size();++i){const auto& row=value.closed[i];
        if(row.startsAtMs<=previous||row.startsAtMs>value.asOfMs||row.lastStoredTickMs<0||row.lastStoredTickMs>value.source.receivedAtMs||
           !number(row.open,true)||!number(row.high,true)||!number(row.low,true)||!number(row.close,true)||!number(row.volume,false)||
           compare(exact(row.low),exact(row.open))>0||compare(exact(row.low),exact(row.close))>0||compare(exact(row.low),exact(row.high))>0||
           compare(exact(row.high),exact(row.open))<0||compare(exact(row.high),exact(row.close))<0)return false;
        qint64 completion=0;bool connects=i>0&&row.startsAtMs-previous==value.intervalMs;
        if(stock){const auto& state=value.stockTimeline->rows[i];completion=state.completesAtMs;connects=state.connectsPrevious;
            if(state.startsAtMs!=row.startsAtMs||(state.missingSlotsBefore&&(*state.missingSlotsBefore<0||(state.connectsPrevious&&*state.missingSlotsBefore>0)))||
               int(state.session)<int(StockSessionKind::Unknown)||int(state.session)>int(StockSessionKind::Mixed))return false;
            for(const auto exclusion:state.exclusions)if(int(exclusion)<0||int(exclusion)>int(StockBarExclusion::AdjustmentUnknown))return false;
            if(completion<=row.startsAtMs||completion>value.source.receivedAtMs)return false;
        }else{
            if(row.startsAtMs>std::numeric_limits<qint64>::max()-value.intervalMs)return false;completion=row.startsAtMs+value.intervalMs;
            if(completion>value.source.receivedAtMs)return false;
        }
        result->drawingBars.append({row,completion,completion>0,connects});
        if(row.startsAtMs==value.viewport.fromMs){foundStart=true;result->firstVisible=i;}if(foundStart)result->visible.append(row);previous=row.startsAtMs;
    }
    if(!foundStart)return false;
    if(value.calculatedThroughMs!=result->drawingBars.last().completesAtMs||value.calculatedThroughMs>value.asOfMs)return false;
    result->studies=stock?keefiChartStudies(value.closed,value.intervalMs,*value.stockTimeline):keefiChartStudies(value.closed,value.intervalMs);
    if(!result->studies.available||studyFacts(result->studies)!=studyFacts(value.studies))return false;
    result->axis=stock?ChartTimeAxis::stock(value.closed,value.intervalMs,*value.stockTimeline):ChartTimeAxis::utc(value.closed,value.intervalMs);
    if(!result->axis.available)return false;
    if(options.includePublicDrawings)for(const auto& drawing:value.drawings){
        const int count=int(drawing.anchors.size());
        if(drawing.kind==ChartDrawingKind::Horizontal){if(count!=1)return false;}
        else if(drawing.kind==ChartDrawingKind::FibExtension){if(count!=2&&count!=3)return false;}
        else if(drawing.kind==ChartDrawingKind::Trend||drawing.kind==ChartDrawingKind::Measure
                ||drawing.kind==ChartDrawingKind::FibRetracement||drawing.kind==ChartDrawingKind::Zone){if(count!=2)return false;}
        else return false;
        for(const auto& anchor:drawing.anchors)if(!number(anchor.price,true)||!result->axis.annotationCoordinateAt(anchor.atMs,result->drawingBars))return false;
        if(drawing.kind==ChartDrawingKind::Trend&&drawing.anchors[0]==drawing.anchors[1])return false;
        if((drawing.kind==ChartDrawingKind::FibRetracement||drawing.kind==ChartDrawingKind::FibExtension)
            &&compare(exact(drawing.anchors[0].price),exact(drawing.anchors[1].price))==0)return false;
        if(drawing.kind==ChartDrawingKind::Zone
            &&(drawing.anchors[0].atMs==drawing.anchors[1].atMs||compare(exact(drawing.anchors[0].price),exact(drawing.anchors[1].price))==0))return false;
    }return true;
}
QString timeframe(qint64 interval) {return interval==Day?QStringLiteral("1일"):interval>=3600000?QString::number(interval/3600000)+QStringLiteral("시간"):QString::number(interval/Minute)+QStringLiteral("분");}
QString decimalText(double value,int precision=2) {return QLocale::c().toString(value,'f',precision);}
QString sourceName(ChartShareSource source) {
    switch(source){case ChartShareSource::Upbit:return QStringLiteral("업비트");case ChartShareSource::BinanceSpot:return QStringLiteral("Binance Spot");case ChartShareSource::Toss:return QStringLiteral("토스");}return {};
}
MovementDirection lastDirection(const ChartShareSnapshot& value,const Prepared& data) {
    if(value.closed.size()<2||!data.axis.connects(value.closed[value.closed.size()-2].startsAtMs,value.closed.last().startsAtMs))return MovementDirection::Unknown;
    const int sign=compare(exact(value.closed.last().close),exact(value.closed[value.closed.size()-2].close));return sign>0?MovementDirection::Up:sign<0?MovementDirection::Down:MovementDirection::Flat;
}
QString instrumentText(const ChartShareSnapshot& value) {
    auto label=ChartInstrumentPresentation::fallback(value.identity);
    if(value.identity.source=="binance")label.code=value.identity.code.left(value.identity.code.size()-value.currency.size())+"/"+value.currency;
    return label.text();
}
QString instrumentCaption(const ChartShareSnapshot& value) {
    return instrumentText(value)+" · "+timeframe(value.intervalMs)+
        (value.identity.market=="stock"?" · "+sourceName(value.source.provider):QString{});
}
QString availabilityText(ChartShareAvailability availability) {
    if(availability==ChartShareAvailability::Ready)return {};
    return usabilityText(availability==ChartShareAvailability::Stale?UsabilityText::Delayed:UsabilityText::NeedsCheck);
}
QStringList studyCaptions(const ChartShareSnapshot& value,const Prepared& data) {
    QStringList lines;const auto& study=data.studies;
    if(value.visiblePublicLayers.contains("pullback")&&study.pullback.available)
        lines<<usabilityText(study.pullback.inBox?UsabilityText::ChartPullbackInside:UsabilityText::ChartPullbackOutside).arg(study.pullback.score);
    if(value.visiblePublicLayers.contains("similarity")&&study.similarity.available)
        lines<<usabilityText(UsabilityText::SimilaritySummary).arg(study.similarity.samples);
    if(value.visiblePublicLayers.contains("cloud")&&!study.cloud.isEmpty()) {
        const auto& cloud=study.cloud.last();
        lines<<usabilityText(cloud.fast>cloud.slow?UsabilityText::ChartTrendUp:cloud.fast<cloud.slow?UsabilityText::ChartTrendDown:UsabilityText::ChartTrendFlat);
    }
    if(study.scorecard.available)lines<<usabilityText(UsabilityText::ChartScore).arg(study.scorecard.samples).arg(study.scorecard.up);
    return lines;
}
QStringList publicCaptions(const ChartShareSnapshot& value,const Prepared& data) {
    QStringList lines{QStringLiteral("%1 · 공개 차트").arg(productName()),instrumentCaption(value)};
    lines<< (value.closed.isEmpty()?usabilityText(UsabilityText::NeedsCheck):
        groupedAmount(value.closed.last().close)+" "+value.currency+" "+MovementColorPolicy::marker(lastDirection(value,data)));
    const auto status=availabilityText(value.availability);if(!status.isEmpty())lines<<status;
    lines<<studyCaptions(value,data);
    if(value.visiblePublicLayers.contains("volume"))lines<<QStringLiteral("시장 거래량");
    lines<<QStringLiteral("계좌 금액·수량 제외 · 로컬 저장");return lines;
}
void font(QPainter& painter,FontRole role) {painter.setFont(KeefiFonts::font(role));}
void line(QPainter& painter,const QRectF& box,const QString& text,const QColor& color,FontRole role=FontRole::Secondary14,const QString& smallUnit={}) {
    font(painter,role);painter.setPen(color);
    const bool numeric=role==FontRole::Price44||role==FontRole::Value22||role==FontRole::Number11||role==FontRole::Logo22;
    auto formats=numeric?QList<QTextLayout::FormatRange>{}:KeefiFonts::numberRuns(text);
    if(!smallUnit.isEmpty()&&text.endsWith(smallUnit)){QTextCharFormat unit;unit.setFont(KeefiFonts::font(FontRole::Number11));
        formats.append({int(text.size()-smallUnit.size()),int(smallUnit.size()),unit});}
    if(formats.isEmpty()){painter.drawText(box,Qt::AlignLeft|Qt::AlignVCenter|Qt::TextSingleLine,text);return;}
    QTextLayout layout(text,painter.font());layout.setFormats(formats);QTextOption option;option.setWrapMode(QTextOption::NoWrap);layout.setTextOption(option);
    layout.beginLayout();auto textLine=layout.createLine();if(textLine.isValid())textLine.setLineWidth(box.width());layout.endLayout();
    painter.save();painter.setClipRect(box,Qt::IntersectClip);
    layout.draw(&painter,QPointF(box.left(),box.top()+(box.height()-layout.boundingRect().height())/2));painter.restore();
}
void paintChart(QPainter& painter,const ChartShareSnapshot& value,const ChartShareOptions& options,const Prepared& data,const MovementColorPolicy& colors) {
    QRectF area(62,166,1076,330);painter.fillRect(area.adjusted(-12,-10,12,72),themeColors().surface);
    if(data.visible.isEmpty()){line(painter,area,usabilityText(UsabilityText::NeedsCheck),themeColors().text3,FontRole::Title17);return;}
    const auto first=data.axis.coordinateAt(data.visible.first().startsAtMs),last=data.axis.coordinateAt(data.visible.last().startsAtMs);if(!first||!last)return;
    double left=*first-.6,right=*last+.6,low=std::numeric_limits<double>::infinity(),high=-low;
    const bool similar=value.visiblePublicLayers.contains("similarity")&&data.studies.similarity.available;
    if(similar&&!data.studies.similarity.mean.isEmpty()){const auto following=data.axis.coordinateAt(data.studies.similarity.mean.last().atMs);if(following)right=std::max(right,*following+.6);}
    const auto include=[&](double price){if(std::isfinite(price)){low=std::min(low,price);high=std::max(high,price);}};
    for(const auto& row:data.visible){include(row.low.toDouble());include(row.high.toDouble());}
    if(value.visiblePublicLayers.contains("cloud"))for(const auto& point:data.studies.cloud)if(point.atMs>=value.viewport.fromMs){include(point.fast);include(point.slow);}
    if(similar){for(const auto& point:data.studies.similarity.minimum)include(point.price);for(const auto& point:data.studies.similarity.maximum)include(point.price);}
    if(!std::isfinite(low)||!std::isfinite(high))return;const double margin=high>low?(high-low)*.06:std::max(std::abs(low)*.01,1e-101);low-=margin;high+=margin;
    const auto x=[&](qint64 at){const auto coordinate=data.axis.coordinateAt(at);return coordinate?area.left()+(*coordinate-left)/(right-left)*area.width():std::numeric_limits<double>::quiet_NaN();};
    const auto y=[&](double price){return area.bottom()-(price-low)/(high-low)*area.height();};
    painter.setPen(themeColors().grid);for(int i=0;i<=3;++i){const double yy=area.top()+area.height()*i/3;painter.drawLine(QPointF(area.left(),yy),QPointF(area.right(),yy));}
    painter.save();painter.setClipRect(area);
    if(value.visiblePublicLayers.contains("cloud"))for(int i=1;i<data.studies.cloud.size();++i){const auto& a=data.studies.cloud[i-1];const auto& b=data.studies.cloud[i];
        if(!data.axis.connects(a.atMs,b.atMs)||b.atMs<value.viewport.fromMs)continue;QPainterPath path;path.moveTo(x(a.atMs),y(a.fast));path.lineTo(x(b.atMs),y(b.fast));path.lineTo(x(b.atMs),y(b.slow));path.lineTo(x(a.atMs),y(a.slow));path.closeSubpath();
        auto ink=colors.fill(b.fast>b.slow?MovementDirection::Up:b.fast<b.slow?MovementDirection::Down:MovementDirection::Flat);auto fill=ink;fill.setAlpha(38);painter.fillPath(path,fill);painter.setPen(QPen(ink,1));painter.drawLine(QPointF(x(a.atMs),y(a.fast)),QPointF(x(b.atMs),y(b.fast)));painter.drawLine(QPointF(x(a.atMs),y(a.slow)),QPointF(x(b.atMs),y(b.slow)));}
    if(value.visiblePublicLayers.contains("pullback")&&data.studies.pullback.available){const auto& box=data.studies.pullback;const auto start=x(box.waveHighAtMs);if(std::isfinite(start)){
        const QRectF rect(QPointF(start,y(box.boxHigh)),QPointF(x(value.closed.last().startsAtMs)+4,y(box.boxLow)));
        // Match the workspace's six-pixel, 45-degree support-zone strokes. A
        // fresh token-colored tile is made for each static themed PNG render.
        QImage tile(6,6,QImage::Format_ARGB32_Premultiplied);tile.fill(Qt::transparent);
        {QPainter pattern(&tile);auto ink=themeColors().accent;ink.setAlphaF(.6);pattern.setPen(QPen(ink,1.2));pattern.drawLine(QPointF(3,0),QPointF(3,6));}
        QBrush hatch(tile);QTransform rotation;rotation.rotate(45);hatch.setTransform(rotation);painter.fillRect(rect.normalized(),hatch);
        painter.setPen(QPen(themeColors().accent,1));painter.setBrush(Qt::NoBrush);painter.drawRect(rect.normalized());}}
    if(similar){const auto& s=data.studies.similarity;
        if(s.minimum.size()==s.maximum.size()&&!s.minimum.isEmpty()){QPainterPath band;band.moveTo(x(s.minimum[0].atMs),y(s.minimum[0].price));for(const auto& point:s.minimum)band.lineTo(x(point.atMs),y(point.price));for(auto it=s.maximum.crbegin();it!=s.maximum.crend();++it)band.lineTo(x(it->atMs),y(it->price));band.closeSubpath();auto bandInk=themeColors().studyBlue;bandInk.setAlpha(35);painter.fillPath(band,bandInk);}
        const QColor inks[]{themeColors().studyBlue,themeColors().studyPurple,themeColors().accent};for(int i=0;i<s.paths.size();++i){painter.setPen(QPen(inks[i%3],1,Qt::DashLine));for(int j=1;j<s.paths[i].points.size();++j){const auto& a=s.paths[i].points[j-1];const auto& b=s.paths[i].points[j];painter.drawLine(QPointF(x(a.atMs),y(a.price)),QPointF(x(b.atMs),y(b.price)));}}
        painter.setPen(QPen(themeColors().studyBlue,2));for(int i=1;i<s.mean.size();++i)painter.drawLine(QPointF(x(s.mean[i-1].atMs),y(s.mean[i-1].price)),QPointF(x(s.mean[i].atMs),y(s.mean[i].price)));}
    const double bodyWidth=std::clamp(area.width()/(right-left)*.65,1.0,14.0);
    for(int i=data.firstVisible;i<value.closed.size();++i){const auto& row=value.closed[i];const double open=row.open.toDouble(),close=row.close.toDouble(),xx=x(row.startsAtMs);const int sign=compare(exact(row.close),exact(row.open));
        const auto ink=colors.fill(sign>0?MovementDirection::Up:sign<0?MovementDirection::Down:MovementDirection::Flat);painter.setPen(QPen(ink,1));painter.setBrush(sign>0?themeColors().surface:ink);painter.drawLine(QPointF(xx,y(row.low.toDouble())),QPointF(xx,y(row.high.toDouble())));painter.drawRect(QRectF(xx-bodyWidth/2,std::min(y(open),y(close)),bodyWidth,std::max(1.0,std::abs(y(open)-y(close)))));
        if(value.stockTimeline){const auto& state=value.stockTimeline->rows[i];const auto session=state.session==StockSessionKind::Pre?themeColors().studyBlue:state.session==StockSessionKind::After?themeColors().studyPurple:state.session==StockSessionKind::Day?themeColors().sessionDay:themeColors().text3;painter.fillRect(QRectF(xx-bodyWidth/2,area.top(),bodyWidth,3),session);
            if(state.exclusions.contains(StockBarExclusion::VolatilityInterruption)||state.exclusions.contains(StockBarExclusion::PriceLimit)){painter.setPen(QPen(themeColors().danger,1,Qt::DashLine));painter.setBrush(Qt::NoBrush);painter.drawRect(QRectF(xx-bodyWidth/2-2,y(row.high.toDouble())-2,bodyWidth+4,std::max(4.0,y(row.low.toDouble())-y(row.high.toDouble())+4)));}}
    }
    if(options.includePublicDrawings)for(const auto& drawing:value.drawings){const auto coordinate=[&](const ChartDrawingPoint& point){const auto slot=data.axis.annotationCoordinateAt(point.atMs,data.drawingBars);return QPointF(area.left()+(*slot-left)/(right-left)*area.width(),y(point.price.toDouble()));};
        const auto a=coordinate(drawing.anchors[0]);
        if(drawing.kind==ChartDrawingKind::FibRetracement||drawing.kind==ChartDrawingKind::FibExtension){
            ChartDrawing modelDrawing;modelDrawing.kind=drawing.kind;modelDrawing.anchors=drawing.anchors;
            const bool extension=(drawing.kind==ChartDrawingKind::FibExtension);
            painter.setPen(QPen(extension?themeColors().studyBlue:themeColors().accent,1,extension?Qt::DashLine:Qt::SolidLine));
            for(const auto& level:ChartDrawings::fibLevels(modelDrawing)){
                const double lineY=y(level.price);
                if(lineY>=area.top()&&lineY<=area.bottom())painter.drawLine(QPointF(a.x(),lineY),QPointF(area.right(),lineY));
            }
        }else if(drawing.kind==ChartDrawingKind::Zone){
            const auto b=coordinate(drawing.anchors[1]);const QRectF rect=QRectF(a,b).normalized().intersected(area);
            if(!rect.isEmpty()){QColor fill=themeColors().accent;fill.setAlpha(40);painter.fillRect(rect,fill);}
        }else{
            painter.setPen(QPen(themeColors().accent,1.6));if(drawing.kind==ChartDrawingKind::Horizontal)painter.drawLine(QPointF(area.left(),a.y()),QPointF(area.right(),a.y()));
            else{const auto b=coordinate(drawing.anchors[1]);painter.drawLine(a,b);if(drawing.kind==ChartDrawingKind::Measure){ChartDrawingFrame frame;frame.identity=value.identity;frame.currency=value.currency;frame.intervalMs=value.intervalMs;frame.asOfMs=value.asOfMs;frame.bars=data.drawingBars;const auto measure=ChartDrawings::measure(drawing.anchors,frame);if(measure.available&&measure.returnPercent){font(painter,FontRole::Number11);painter.setPen(themeColors().accentText);painter.drawText((a+b)/2,decimalText(*measure.returnPercent)+"%"+(measure.barIntervals?QStringLiteral(" · %1봉").arg(*measure.barIntervals):QString{}));}}}
        }
    }
    painter.restore();
    if(value.visiblePublicLayers.contains("volume")){double maximum=0;for(const auto& row:data.visible)maximum=std::max(maximum,row.volume.toDouble());painter.setPen(Qt::NoPen);painter.setBrush(themeColors().text3);if(maximum>0)for(const auto& row:data.visible){const double height=row.volume.toDouble()/maximum*40;painter.drawRect(QRectF(x(row.startsAtMs)-bodyWidth/2,area.bottom()+48-height,bodyWidth,height));}line(painter,QRectF(area.left(),area.bottom()+52,area.width(),18),QStringLiteral("시장 거래량"),themeColors().text3,FontRole::Caption12);}
}
}
ChartShareRenderResult ChartShareCard::render(const ChartShareSnapshot& value,const ChartShareOptions& options) {
    Prepared data;ChartShareRenderResult result;if(!prepare(value,options,&data)){result.reason=InvalidInput;return result;}
    QImage image(options.outputSize,QImage::Format_ARGB32_Premultiplied);if(image.isNull()){result.reason=QStringLiteral("공개 차트 이미지 공간을 준비하지 못했습니다.");return result;}
    image.setDevicePixelRatio(1);image.setDotsPerMeterX(3780);image.setDotsPerMeterY(3780);image.fill(themeColors().bg);
    QPainter painter(&image);if(!painter.isActive()){result.reason=QStringLiteral("공개 차트 이미지를 만들지 못했습니다.");return result;}
    painter.setRenderHint(QPainter::Antialiasing);painter.scale(image.width()/1200.0,image.height()/900.0);MovementColorPolicy colors;colors.setScheme(value.colors);result.publicCaptions=publicCaptions(value,data);
    line(painter,QRectF(48,24,1104,36),productName(),themeColors().text,FontRole::Value22);
    line(painter,QRectF(48,65,1104,26),instrumentCaption(value),themeColors().text2,FontRole::Body15);
    const QString price=value.closed.isEmpty()?usabilityText(UsabilityText::NeedsCheck):MovementColorPolicy::marker(lastDirection(value,data))+" "+groupedAmount(value.closed.last().close)+" "+value.currency;
    line(painter,QRectF(48,98,1104,32),price,colors.foreground(lastDirection(value,data)),FontRole::Value22,value.closed.isEmpty()?QString{}:value.currency);
    line(painter,QRectF(48,134,1104,22),availabilityText(value.availability),themeColors().text3,FontRole::Caption12);
    paintChart(painter,value,options,data,colors);
    qreal yy=594;for(const auto& text:studyCaptions(value,data)){
        line(painter,QRectF(48,yy,1104,20),text,themeColors().text2,FontRole::Caption12);yy+=20;
    }
    line(painter,QRectF(48,870,1104,18),QStringLiteral("계좌 금액·수량 제외 · 로컬 저장"),themeColors().text2,FontRole::CaptionStrong12);painter.end();result.image=std::move(image);result.available=true;return result;
}
bool ChartShareCard::savePng(const ChartShareSnapshot& value,const ChartShareOptions& options,const QString& path,QString* error) {
    const auto reject=[&](){if(error)*error=QStringLiteral("PNG를 명시한 로컬 파일에 저장하지 못했습니다.");return false;};
    if(path.isEmpty()||path.contains(QChar(0))||!QDir::isAbsolutePath(path)||path.contains("://")||QFileInfo(path).suffix().toLower()!="png"||QFileInfo(path).isDir())return reject();
    const auto result=render(value,options);if(!result.available)return reject();QSaveFile file(path);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly))return reject();
    QImageWriter writer(&file,"png");if(!writer.write(result.image)){file.cancelWriting();return reject();}if(!file.commit())return reject();if(error)error->clear();return true;
}
}
