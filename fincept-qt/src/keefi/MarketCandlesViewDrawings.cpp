#include "keefi/MarketCandlesView.h"
#include "keefi/MarketCandlesViewFormatting_p.h"
#include "keefi/KeefiTheme.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/StockChartSource.h"
#include "keefi/ChartTimeAxis.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/CandleChart_p.h"
#include "keefi/ChartDrawings.h"
#include "keefi/AutoChartGeometry.h"
#include "keefi/ChartShareCard.h"
#include "keefi/KeefiFonts.h"
#include "keefi/StockChartTypes.h"
#include "keefi/MarketCandles.h"
#include "keefi/MovementColorPolicy.h"

#include <QApplication>
#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QDialog>
#include <QGridLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QSet>
#include <QUrl>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>

namespace keefi {
using namespace market_candles_detail;
namespace {
QString drawingKindName(ChartDrawingKind kind) {
    switch (kind) {
    case ChartDrawingKind::Trend: return QObject::tr("추세선");
    case ChartDrawingKind::Horizontal: return QObject::tr("수평선");
    case ChartDrawingKind::Measure: return QObject::tr("측정");
    case ChartDrawingKind::FibRetracement: return QObject::tr("피보 되돌림");
    case ChartDrawingKind::FibExtension: return QObject::tr("피보 확장");
    case ChartDrawingKind::Zone: return QObject::tr("횡보 구간");
    }
    return {};
}
QString drawingRepresentativePrice(ChartDrawingKind kind, const QList<ChartDrawingPoint>& anchors) {
    if (anchors.isEmpty()) return {};
    if (kind == ChartDrawingKind::Horizontal) return priceAmount(anchors.first().price);
    if (kind == ChartDrawingKind::FibRetracement || kind == ChartDrawingKind::FibExtension)
        return priceAmount(anchors.size() > 1 ? anchors[1].price : anchors.first().price);
    if (kind == ChartDrawingKind::Zone && anchors.size() >= 2) {
        const double a = anchors[0].price.toDouble(), b = anchors[1].price.toDouble();
        return priceAmount(drawnPrice(qMin(a, b))) + QStringLiteral(" ~ ") + priceAmount(drawnPrice(qMax(a, b)));
    }
    return priceAmount(anchors.last().price);
}
QIcon drawingControlIcon(const QString& kind, const QColor& ink, bool active = true) {
    if (kind == QStringLiteral("chartDrawingAuto")) {
        QPixmap pixels(48, 48); pixels.setDevicePixelRatio(2); pixels.fill(Qt::transparent);
        QPainter p(&pixels); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(4, 17), QPointF(16, 7));
        p.drawLine(QPointF(4, 9), QPointF(10, 9));
        p.drawLine(QPointF(12, 17), QPointF(20, 17));
        p.drawLine(QPointF(18, 4), QPointF(18, 10));
        p.drawLine(QPointF(15, 7), QPointF(21, 7));
        return QIcon(pixels);
    }
    if (kind == QStringLiteral("chartDrawingObjects")) {
        QPixmap pixels(48, 48); pixels.setDevicePixelRatio(2); pixels.fill(Qt::transparent);
        QPainter p(&pixels); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const int y : {6, 12, 18}) {
            p.drawPoint(5, y);
            p.drawLine(9, y, 20, y);
        }
        return QIcon(pixels);
    }
    if (kind == QStringLiteral("objectLock")) {
        QPixmap pixels(48, 48); pixels.setDevicePixelRatio(2); pixels.fill(Qt::transparent);
        QPainter p(&pixels); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(5, 11, 14, 10), 1, 1);
        if (active) p.drawArc(QRectF(8, 4, 8, 12), 0, 180 * 16);
        else p.drawArc(QRectF(8, 3, 8, 10), 25 * 16, 145 * 16);
        p.drawLine(12, 15, 12, 17);
        return QIcon(pixels);
    }
    return chartLineIcon(kind, ink, active);
}
class DrawingObjectRowWidget final : public QFrame {
  public:
    explicit DrawingObjectRowWidget(std::function<void()> onClick, QWidget* parent = nullptr)
        : QFrame(parent), onClick_(std::move(onClick)) {
        setFocusPolicy(Qt::NoFocus);
    }
  protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && onClick_) {
            onClick_();
            event->accept();
            return;
        }
        QFrame::mouseReleaseEvent(event);
    }
  private:
    std::function<void()> onClick_;
};
} // namespace
ChartShareSnapshot MarketCandlesView::shareSnapshot(const ChartShareOptions& options,qint64 asOfMs)const{
    ChartShareSnapshot out;
    out.identity=WatchlistWorkspaceList::identityFromKey(identity_);out.currency=currency_;out.intervalMs=intervalMs_;
    out.asOfMs=asOfMs?asOfMs:QDateTime::currentMSecsSinceEpoch();out.displayTimeZoneId="Asia/Seoul";
    out.colors=MovementColorPolicy::schemeFor(this);
    const auto unavailable=[&]{ChartShareSnapshot rejected=out;rejected.closed.clear();rejected.stockTimeline.reset();rejected.studies={};rejected.drawings.clear();rejected.viewport={};rejected.calculatedThroughMs=0;rejected.availability=ChartShareAvailability::Unavailable;return rejected;};
    if(!options.maskPersonalValues||!WatchlistWorkspaceList::validIdentity(out.identity)||!supportedTimeframe(intervalMs_)||out.asOfMs<=0)return unavailable();
    const bool stock=out.identity.market=="stock";
    MarketCandleSeries data;StockChartSnapshot stockData;
    if(stock){
        if(stockSource_)stockData=stockSource_->snapshot(out.asOfMs);
        if(stockData.source!=StockSourceKind::Toss)return unavailable();
        data=stockData.series;
        out.source.provider=ChartShareSource::Toss;
        out.source.delayed=stockData.delayed;out.source.adjustment=stockData.adjustment;
    }else{
        if(out.identity.source!="upbit"&&out.identity.source!="binance")return unavailable();
        if(model_)data=model_->displayObservation(out.asOfMs);
        out.source.provider=out.identity.source=="upbit"?ChartShareSource::Upbit:ChartShareSource::BinanceSpot;
        // The canonical request factory is pure; this does not dispatch a read.
        out.source.canonicalUrl=MarketCandles::request(identity_,intervalMs_).url.toString();
    }
    if(data.identity!=identity_||data.intervalMs!=intervalMs_)return out;
    out.currency=data.currency;
    // Explicit public fields only. No AssetEvidence scope, account state,
    // private overlay, arbitrary error label or QWidget enters the snapshot.
    out.source.canonicalUrl=data.sourceUrl;out.source.sourceTimeMs=data.sourceTimeMs;out.source.receivedAtMs=data.receivedAtMs;
    if(!data.available||data.candles.isEmpty()){
        if(!stock&&data.receivedAtMs>0&&out.asOfMs>=data.receivedAtMs&&out.asOfMs-data.receivedAtMs>MarketCandles::FreshnessMs)out.availability=ChartShareAvailability::Stale;
        else if(data.receivedAtMs>0)out.availability=ChartShareAvailability::Unavailable;
        return out;
    }
    if(out.currency.isEmpty()||data.candles.size()>MarketCandles::HistoryLimit||data.receivedAtMs<=0||data.receivedAtMs>out.asOfMs)return unavailable();
    out.viewport=static_cast<const CandleChart*>(chart_)->shareViewport();
    if(out.viewport.fromMs<=0||out.viewport.toMs<out.viewport.fromMs)return unavailable();
    int first=-1,last=-1;
    for(int i=0;i<data.candles.size();++i){if(data.candles[i].startsAtMs==out.viewport.fromMs)first=i;if(data.candles[i].startsAtMs==out.viewport.toMs)last=i;}
    if(first<0||last<first)return unavailable();
    const auto prefixFirst=stock?0:qMax(0,last-ChartShareCard::MaxClosedBars+1);
    if(first<prefixFirst)return unavailable();
    for(int i=prefixFirst;i<=last;++i){const auto& row=data.candles[i];MarketCandle copy;
        copy.startsAtMs=row.startsAtMs;copy.lastStoredTickMs=row.lastStoredTickMs;copy.open=row.open;copy.high=row.high;copy.low=row.low;copy.close=row.close;copy.volume=row.volume;out.closed.append(copy);
    }
    if(stock){
        if(stockData.timeline.rows.size()!=data.candles.size())return unavailable();
        ChartStudyTimeline timeline;timeline.adjustedComparable=stockData.timeline.adjustedComparable;
        if(!timeline.adjustedComparable)timeline.comparisonReason=tr("공유 구간의 수정주가 비교 근거 미확인");
        for(int i=0;i<=last;++i){const auto& row=stockData.timeline.rows[i];if(row.startsAtMs!=out.closed[i].startsAtMs||row.completesAtMs>out.asOfMs)return unavailable();
            ChartStudyTimelineRow copy;copy.startsAtMs=row.startsAtMs;copy.completesAtMs=row.completesAtMs;copy.tradingDate=row.tradingDate;copy.session=row.session;
            copy.connectsPrevious=row.connectsPrevious;copy.eligible=row.eligible;copy.missingSlotsBefore=row.missingSlotsBefore;copy.exclusions=row.exclusions;
            if(!copy.eligible)copy.reason=tr("주식 판정 제외 구간");timeline.rows.append(copy);
        }
        // A historical viewport cannot reuse the global latest tail's future
        // coordinates. Without a prefix calendar proof, forward paths stay off.
        if(last==data.candles.size()-1&&stockData.timeline.projectionCovered){timeline.projectionCovered=true;timeline.nextStartsAtMs=stockData.timeline.nextStartsAtMs;}
        else timeline.projectionReason=tr("공유 계산 구간의 미래 거래 시각 미확인");
        out.calculatedThroughMs=timeline.rows.last().completesAtMs;out.stockTimeline=timeline;
        if(std::all_of(timeline.rows.cbegin(),timeline.rows.cend(),[](const auto& row){return row.completesAtMs>row.startsAtMs;}))out.studies=keefiChartStudies(out.closed,intervalMs_,timeline);
        else out.studies={}; // Unknown completion remains chart-only; no inferred study boundary.
    }else{
        if(out.closed.last().startsAtMs>std::numeric_limits<qint64>::max()-intervalMs_)return unavailable();
        out.calculatedThroughMs=out.closed.last().startsAtMs+intervalMs_;
        if(out.calculatedThroughMs>out.asOfMs)return unavailable();
        out.studies=keefiChartStudies(out.closed,intervalMs_);
    }
    for(const auto* id:{"pullback","similarity","cloud","volume"})if(indicatorPreferences_&&indicatorPreferences_->enabled(id)&&indicatorPreferences_->visible(id))out.visiblePublicLayers.insert(id);
    if(options.includePublicDrawings&&drawingPublicContext_&&!drawingPrivateCleared_&&drawingModel_&&drawingModel_->visible()){
        const auto context=drawingModel_->context();
        if(context.publicPersistenceAllowed&&context.identity==out.identity&&context.currency==out.currency&&drawingModel_->intervalMs()==out.intervalMs){
            const auto axis=stock?ChartTimeAxis::stock(out.closed,intervalMs_,*out.stockTimeline):ChartTimeAxis::utc(out.closed,intervalMs_);
            QList<ChartDrawingBar> actualClosed;
            for(int i=0;i<out.closed.size();++i){const auto& row=out.closed[i];const auto completion=stock?out.stockTimeline->rows[i].completesAtMs:row.startsAtMs+intervalMs_;
                const bool connects=stock?out.stockTimeline->rows[i].connectsPrevious:i>0&&row.startsAtMs-out.closed[i-1].startsAtMs==intervalMs_;
                actualClosed.append({row,completion,completion>row.startsAtMs&&completion<=out.asOfMs,connects});
            }
            for(const auto& drawing:drawingModel_->drawings()){
                if(!drawing.visible)continue;
                if(std::any_of(drawing.anchors.cbegin(),drawing.anchors.cend(),[&](const auto& point){return !axis.annotationCoordinateAt(point.atMs,actualClosed).has_value();}))continue;
                ChartShareDrawing copy;copy.kind=drawing.kind;for(const auto& point:drawing.anchors)copy.anchors.append({point.atMs,point.price});out.drawings.append(copy);
            }
        }
    }
    out.availability=stock&&stockData.loading?ChartShareAvailability::Partial:ChartShareAvailability::Ready;
    return out;
}
void MarketCandlesView::setPriceLines(const QList<ChartPriceLine>& rows){
    const auto input=rows;priceLines_.clear();const auto now=QDateTime::currentMSecsSinceEpoch();
    for(const auto& row:input){bool ok=false;const auto value=row.price.toDouble(&ok);const QUrl source(row.sourceUrl,QUrl::StrictMode);
        if(identity_.startsWith("stock:")||asset_.isEmpty()||currency_.isEmpty()||row.asset!=asset_||row.currency!=currency_||row.provider.isEmpty()||row.accountScope.isEmpty()||
           !decimal::positive(row.price)||!ok||!std::isfinite(value)||value>1e100||value<1e-100||
           row.receivedAtMs<=0||row.receivedAtMs>now||now-row.receivedAtMs>180000||source.scheme()!="https"||source.host().isEmpty())continue;
        priceLines_.append(row);
    }
    static_cast<CandleChart*>(chart_)->setPriceLines(priceLines_);
    QStringList descriptions;for(const auto& row:priceLines_){const auto kind=row.kind==ChartPriceKind::Average?tr("업체 제공 평균가"):row.kind==ChartPriceKind::Entry?tr("선물 진입가"):tr("선물 청산가");
        descriptions<<tr("%1 · %2 · %3 · %4 %5").arg(kind,usabilityPublicText(row.provider),accountLabel(priceLines_,row.accountScope),priceAmount(row.price),row.currency);}
    auto* label=findChild<QLabel*>("marketCandlesPositionLines");label->setText(descriptions.join('\n'));label->hide();label->setToolTip({}); // The position panel owns this repeated description.
}
QList<ChartPriceLine> MarketCandlesView::priceLines() const{return priceLines_;}
void MarketCandlesView::setCandleEvidenceNumber(int number){static_cast<CandleChart*>(chart_)->setCandleEvidenceNumber(number);}
void MarketCandlesView::clearPrivate(){setPriceLines({});}
void MarketCandlesView::setDrawingModel(ChartDrawings* model){
    if(drawingModel_==model)return;if(drawingModel_)disconnect(drawingModel_,nullptr,this,nullptr);
    static_cast<CandleChart*>(chart_)->cancelDrawingGesture();clearDrawingFeedback();drawingModel_=model;
    if(model){connect(model,&ChartDrawings::changed,this,&MarketCandlesView::refreshDrawingUi);
        connect(model,&QObject::destroyed,this,[this]{drawingModel_=nullptr;clearDrawingFeedback();static_cast<CandleChart*>(chart_)->setDrawingFrame(nullptr,{});refreshDrawingUi();});}
    syncDrawingContext(drawingSeries_);
}
void MarketCandlesView::setDrawingPublicContext(bool publicAllowed){
    const bool changed=drawingPublicContext_!=publicAllowed||drawingPrivateCleared_;drawingPublicContext_=publicAllowed;drawingPrivateCleared_=false;
    if(changed){static_cast<CandleChart*>(chart_)->cancelDrawingGesture();clearDrawingFeedback();syncDrawingContext(drawingSeries_);}
}
void MarketCandlesView::clearPrivateDrawingContext(){
    if(indicatorHelpDialog_)indicatorHelpDialog_->reject();
    static_cast<CandleChart*>(chart_)->cancelDrawingGesture();clearDrawingFeedback();if(!drawingPublicContext_)drawingPrivateCleared_=true;
    refreshDrawingUi();
}
void MarketCandlesView::showDrawingFeedback(const QString& text){
    if(text.isEmpty()){clearDrawingFeedback();return;}
    drawingFeedbackText_=text;
    drawingFeedbackGen_=drawingModel_?drawingModel_->contextGeneration():0;
    if(drawingStatus_){drawingStatus_->setText(text);drawingStatus_->show();}
    if(drawingFeedbackTimer_)drawingFeedbackTimer_->start();
}
void MarketCandlesView::clearDrawingFeedback(){
    drawingFeedbackText_.clear();
    drawingFeedbackGen_=0;
    if(drawingFeedbackTimer_)drawingFeedbackTimer_->stop();
    if(drawingStatus_)drawingStatus_->hide();
}
void MarketCandlesView::setDrawingAlertLists(const QMap<QString,QString>& names,const QSet<QString>& enabled){
    if(drawingAlertListNames_==names&&drawingEnabledLineLists_==enabled)return;drawingAlertListNames_=names;drawingEnabledLineLists_=enabled;refreshDrawingUi();
}
ChartDrawingFrame MarketCandlesView::drawingFrame()const{
    ChartDrawingFrame frame;
    if(!drawingModel_||(!drawingPublicContext_&&drawingPrivateCleared_)||!drawingSeries_.available||drawingSeries_.identity!=identity_||drawingSeries_.intervalMs!=intervalMs_||drawingSeries_.currency.isEmpty()||drawingSeries_.currency!=currency_)return frame;
    frame.identity=WatchlistWorkspaceList::identityFromKey(identity_);frame.currency=currency_;frame.intervalMs=intervalMs_;frame.asOfMs=QDateTime::currentMSecsSinceEpoch();frame.contextGeneration=drawingModel_->contextGeneration();
    if(frame.identity!=drawingModel_->context().identity||frame.currency!=drawingModel_->context().currency||intervalMs_!=drawingModel_->intervalMs())return {};
    const bool stock=identity_.startsWith("stock:");
    if(stock&&stockSnapshot_.timeline.rows.size()!=drawingSeries_.candles.size())return {};
    if(!stock&&(MarketCandles::request(identity_,intervalMs_).url.toString()!=drawingSeries_.sourceUrl||drawingSeries_.receivedAtMs<=0||drawingSeries_.receivedAtMs>frame.asOfMs))return {};
    for(qsizetype i=0;i<drawingSeries_.candles.size();++i){const auto& candle=drawingSeries_.candles[i];ChartDrawingBar row;row.candle=candle;
        if(stock){const auto& context=stockSnapshot_.timeline.rows[i];if(context.startsAtMs!=candle.startsAtMs)return {};row.completesAtMs=context.completesAtMs;row.connectsPrevious=context.connectsPrevious;}
        else{if(candle.startsAtMs>std::numeric_limits<qint64>::max()-intervalMs_)return {};row.completesAtMs=candle.startsAtMs+intervalMs_;row.connectsPrevious=i>0&&candle.startsAtMs-drawingSeries_.candles[i-1].startsAtMs==intervalMs_;}
        row.sourceCompleted=row.completesAtMs>candle.startsAtMs&&row.completesAtMs<=drawingSeries_.receivedAtMs&&row.completesAtMs<=frame.asOfMs;frame.bars.append(row);
    }
    return frame;
}
void MarketCandlesView::syncDrawingContext(const MarketCandleSeries& series){
    const auto copy=series;drawingSeries_=copy;
    if(drawingModel_){
        const auto id=WatchlistWorkspaceList::identityFromKey(identity_);const bool valid=series.available&&series.identity==identity_&&series.intervalMs==intervalMs_&&!currency_.isEmpty()&&series.currency==currency_&&(!drawingPrivateCleared_||drawingPublicContext_);
        if(valid)drawingModel_->setContext({id,currency_,drawingPublicContext_},intervalMs_);
        else if(drawingModel_->context().identity!=id||drawingModel_->context().currency!=currency_||(!drawingPublicContext_&&drawingPrivateCleared_))drawingModel_->clearContext();
    }
    refreshDrawingUi();
}
void MarketCandlesView::layoutDrawingTools(){
    if(!drawingToolbarLayout_)return;const int columns=1;if(columns==drawingToolbarColumns_)return;drawingToolbarColumns_=columns;
    while(auto* item=drawingToolbarLayout_->takeAt(0))delete item;
    QList<QToolButton*> buttons;for(const auto tool:{ChartDrawingTool::Crosshair,ChartDrawingTool::Trend,ChartDrawingTool::Horizontal,ChartDrawingTool::Measure})buttons.append(drawingTools_.value(tool));
    buttons<<drawingAuto_<<drawingUndo_<<drawingMore_;for(auto* auxiliary:{drawingObjects_,drawingRedo_,drawingMagnet_,drawingVisible_,drawingLocked_,drawingClear_})auxiliary->hide();
    drawingToolbarLayout_->setAlignment(Qt::AlignTop);
    for(qsizetype i=0;i<buttons.size();++i)drawingToolbarLayout_->addWidget(buttons[i],i/columns,i%columns);
}
bool MarketCandlesView::drawingContextEditable()const{
    if(!drawingModel_||!WatchlistWorkspaceList::validIdentity(drawingModel_->context().identity))return false;
    QString err;drawingModel_->create(ChartDrawingKind::Horizontal,{},ChartDrawingFrame{},&err);
    return err!=QStringLiteral("현재 문맥이 잠겼거나 공개 종목 검증이 없습니다");
}
void MarketCandlesView::refreshDrawingUi(){
    auto* chart=static_cast<CandleChart*>(chart_);const auto frame=drawingFrame();chart->setDrawingFrame(drawingModel_,frame);layoutDrawingTools();
    const bool context=drawingModel_&&WatchlistWorkspaceList::validIdentity(drawingModel_->context().identity)&&frame.identity==drawingModel_->context().identity;
    const bool closed=context&&std::any_of(frame.bars.begin(),frame.bars.end(),[](const auto& row){return row.sourceCompleted;});const bool locked=drawingModel_&&drawingModel_->locked();
    const bool visible=!drawingModel_||drawingModel_->visible();
    for(auto it=drawingTools_.begin();it!=drawingTools_.end();++it){const QSignalBlocker block(it.value());it.value()->setChecked(chart->drawingTool()==it.key());it.value()->setEnabled(it.key()==ChartDrawingTool::Crosshair||(closed&&!locked&&visible));}
    for(auto* button:{drawingMagnet_,drawingVisible_,drawingLocked_,drawingClear_,drawingObjects_})button->setEnabled(context);
    const bool editable=drawingContextEditable();
    const bool autoEnabled=context&&closed&&!locked&&visible&&editable;
    if(drawingAuto_)drawingAuto_->setEnabled(autoEnabled);
    if(drawingModel_){const QSignalBlocker magnet(drawingMagnet_),visibleBlock(drawingVisible_),lock(drawingLocked_);drawingMagnet_->setChecked(drawingModel_->magnetEnabled());drawingVisible_->setChecked(drawingModel_->visible());drawingLocked_->setChecked(locked);drawingClear_->setEnabled(context&&!locked&&!drawingModel_->drawings().isEmpty());}
    drawingMagnet_->setEnabled(context&&!locked);
    drawingUndo_->setEnabled(context&&drawingModel_->canUndo());drawingRedo_->setEnabled(context&&drawingModel_->canRedo());
    for(auto* button:drawingToolbar_->findChildren<QToolButton*>())if(button!=drawingMore_)button->setIcon(drawingControlIcon(button->objectName(),button->isChecked()?themeColors().accent:themeColors().text2,button!=drawingVisible_||button->isChecked()));
    QString state=!closed?tr("실제 완료 봉과 공개 통화를 확인하면 그림을 그릴 수 있어요."):
        tr("%1 · %2개 그림 · 가격·UTC 시각 저장 · 두 점 도구는 클릭 두 번, Esc로 취소").arg(ChartDrawings::bucketForInterval(intervalMs_)=="daily"?tr("일봉 그림"):tr("분·시간봉 그림")).arg(drawingModel_->drawings().size());
    if(context&&!drawingModel_->context().publicPersistenceAllowed)state+=tr(" · 보유에서 연 임시 그림: 프로필 저장·알림 사용 안 함");
    if(!drawingFeedbackText_.isEmpty()&&drawingModel_&&drawingModel_->contextGeneration()!=drawingFeedbackGen_)clearDrawingFeedback();
    if(locked)state+=tr(" · 편집 잠금");if(drawingModel_&&!drawingModel_->visible())state+=tr(" · 그림 숨김 · 켠 알림은 계속됨");
    if(!drawingFeedbackText_.isEmpty()){drawingStatus_->setText(drawingFeedbackText_);drawingStatus_->show();}
    else{drawingStatus_->setText(state);drawingStatus_->hide();}
    for(auto* button:drawingToolbar_->findChildren<QToolButton*>())button->setToolTip(button->accessibleName()+"\n"+state);
    if(drawingMagnet_){
        const auto mod=QKeySequence(Qt::CTRL).toString(QKeySequence::NativeText);
        drawingMagnet_->setToolTip(tr("자석 · %1 누른 채 24px 강한 자석\n실제 완료 봉의 시가·고가·저가·종가에 붙입니다.").arg(mod));
    }
    if(drawingAuto_){
        const QString reason=!context||!closed?tr("완료 봉이 준비되면 자동 작도를 쓸 수 있어요."):
            locked?tr("그리기 편집이 잠겨 있어 자동 작도를 켤 수 없어요."):
            !visible?tr("그림이 숨겨져 있어 자동 작도를 켤 수 없어요."):
            !editable?tr("검증된 종목에서만 자동 작도를 쓸 수 있어요."):
            tr("완료 봉 구조에서 피보·지지저항·횡보 구간·추세선 후보를 찾아요.");
        drawingAuto_->setToolTip(drawingAuto_->accessibleName()+QStringLiteral("\n")+reason);
    }
    if(drawingObjects_)drawingObjects_->setToolTip(drawingObjects_->accessibleName());
    drawingUndo_->setToolTip(tr("그리기 실행취소 · ⌘Z"));drawingRedo_->setToolTip(tr("그리기 다시 실행 · ⇧⌘Z"));
    syncDrawingObjectsPanel();
    ChartDrawing selected;bool selectedLine=false;if(drawingModel_)for(const auto& shape:drawingModel_->drawings())if(shape.id==chart->selectedDrawing()&&shape.kind==ChartDrawingKind::Horizontal){selected=shape;selectedLine=true;break;}
    drawingLinePanel_->setVisible(selectedLine);if(!selectedLine){drawingUiSelection_.clear();return;}
    const bool sameSelection=drawingUiSelection_==selected.id;drawingUiSelection_=selected.id;
    const auto pendingOwner=sameSelection?drawingLineOwner_->currentData().toString():QString{};
    const auto pendingInterval=sameSelection?drawingLineInterval_->currentData().toLongLong():intervalMs_;
    const QSignalBlocker enabledBlock(drawingLineEnabled_),ownerBlock(drawingLineOwner_),intervalBlock(drawingLineInterval_);
    drawingLineOwner_->clear();for(auto i=drawingAlertListNames_.cbegin();i!=drawingAlertListNames_.cend();++i)drawingLineOwner_->addItem(i.value(),i.key());
    auto owner=drawingLineOwner_->findData(selected.alert.enabled?selected.alert.listId:pendingOwner);drawingLineOwner_->setCurrentIndex(owner>=0?owner:(!selected.alert.enabled&&drawingLineOwner_->count()?0:-1));
    const auto alertInterval=selected.alert.intervalMs>0?selected.alert.intervalMs:pendingInterval;drawingLineInterval_->setCurrentIndex(drawingLineInterval_->findData(alertInterval));drawingLineEnabled_->setChecked(selected.alert.enabled);
    const bool permitted=context&&drawingModel_->context().publicPersistenceAllowed&&drawingLineOwner_->currentIndex()>=0;
    drawingLineEnabled_->setEnabled(permitted&&!locked);drawingLineOwner_->setEnabled(permitted&&!locked);drawingLineInterval_->setEnabled(permitted&&!locked);drawingLineDelete_->setEnabled(!locked);
    const auto list=drawingLineOwner_->currentData().toString();drawingLineSettings_->setEnabled(!list.isEmpty());
    auto lineState=tr("수평선 %1 %2 · 알림 %3 고정 · 화면 시간봉을 바꿔도 유지").arg(priceAmount(selected.anchors.first().price),currency_,timeframeLabel(alertInterval));
    if(!drawingModel_->context().publicPersistenceAllowed)lineState+=tr(" · 보유 임시 그림에는 공개 리스트 알림을 켤 수 없어요.");
    else if(!permitted)lineState+=tr(" · 이 종목을 담은 공개 리스트가 필요해요.");
    else if(!drawingEnabledLineLists_.contains(list))lineState+=tr(" · 리스트 수평선 알림 꺼짐");drawingLineStatus_->setText(lineState);
}
void MarketCandlesView::saveDrawingAlert(){
    if(!drawingModel_||drawingModel_->locked()||!drawingLinePanel_->isVisible())return;
    ChartDrawingAlert alert{drawingLineEnabled_->isChecked(),drawingLineOwner_->currentData().toString(),drawingLineInterval_->currentData().toLongLong()};
    if(!alert.enabled){bool wasEnabled=false;for(const auto& shape:drawingModel_->drawings())if(shape.id==static_cast<CandleChart*>(chart_)->selectedDrawing())wasEnabled=shape.alert.enabled;
        if(!wasEnabled){refreshDrawingUi();return;}alert={};}
    QString error;if(!drawingModel_->setAlert(static_cast<CandleChart*>(chart_)->selectedDrawing(),alert,&error))showDrawingFeedback(error);refreshDrawingUi();
}
void MarketCandlesView::runAutoChartGeometry(){
    if(!drawingModel_||!drawingContextEditable()||!drawingModel_->visible()||drawingModel_->locked())return;
    const auto frame=drawingFrame();
    const auto result=AutoChartGeometry::analyze(frame);
    if(!result.available||result.candidates.isEmpty()){
        showDrawingFeedback(result.reason.isEmpty()?tr("작도할 후보가 없습니다."):result.reason);
        return;
    }
    clearDrawingFeedback();
    clearAutoPreviewPanel();
    QList<DrawingPreviewItem> items;
    for(const auto& candidate:result.candidates){
        items.append({candidate,true});
        PreviewRow row;
        row.key=candidate.key;
        row.kind=candidate.kind;
        row.anchors=candidate.anchors;
        const auto rep=drawingRepresentativePrice(candidate.kind,candidate.anchors);
        const auto text=rep.isEmpty()?drawingKindName(candidate.kind):drawingKindName(candidate.kind)+QStringLiteral(" · ")+rep;
        row.check=new QCheckBox(text,drawingPreviewGroup_);
        row.check->setObjectName(QStringLiteral("chartDrawingPreviewCheck_")+candidate.key);
        row.check->setChecked(true);
        row.check->setFont(KeefiFonts::font(FontRole::Caption12));
        row.check->installEventFilter(this);
        connect(row.check,&QCheckBox::toggled,this,[this,key=candidate.key](bool checked){
            static_cast<CandleChart*>(chart_)->setAutoPreviewItemVisible(key,checked);
        });
        row.row=row.check;
        drawingPreviewLayout_->addWidget(row.check);
        drawingPreviewRows_.append(row);
    }
    if(drawingPreviewGroup_)drawingPreviewGroup_->show();
    static_cast<CandleChart*>(chart_)->setAutoPreview(items,frame);
    setDrawingObjectsOpen(true);
    syncDrawingObjectsPanel();
    layoutDrawingObjectsPanel();
}
void MarketCandlesView::applyAutoPreview(){
    if(!drawingModel_)return;
    const auto frame=drawingFrame();
    QList<ChartDrawing> shapes;
    for(const auto& row:drawingPreviewRows_){
        if(!row.check||!row.check->isChecked())continue;
        ChartDrawing shape;
        shape.kind=row.kind;
        shape.anchors=row.anchors;
        shapes.append(shape);
    }
    QString error;
    if(!drawingModel_->applyBatch(shapes,frame,&error)){
        showDrawingFeedback(error.isEmpty()?tr("자동 작도를 적용할 수 없어요."):error);
        return;
    }
    clearDrawingFeedback();
    static_cast<CandleChart*>(chart_)->clearAutoPreview();
    clearAutoPreviewPanel();
    syncDrawingObjectsPanel();
    layoutDrawingObjectsPanel();
}
void MarketCandlesView::clearAutoPreviewPanel(){
    for(auto& row:drawingPreviewRows_){
        if(drawingPreviewLayout_&&row.row)drawingPreviewLayout_->removeWidget(row.row);
        delete row.row;
    }
    drawingPreviewRows_.clear();
    if(drawingPreviewGroup_)drawingPreviewGroup_->hide();
    if(drawingObjectsEmpty_)drawingObjectsEmpty_->setVisible(drawingObjectRows_.isEmpty()&&drawingPreviewRows_.isEmpty());
    layoutDrawingObjectsPanel();
}
void MarketCandlesView::setDrawingObjectsOpen(bool open){
    if(drawingObjects_){
        const QSignalBlocker block(drawingObjects_);
        drawingObjects_->setChecked(open);
    }
    if(!drawingObjectsPanel_)return;
    const bool wasVisible=drawingObjectsPanel_->isVisible();
    if(wasVisible==open){
        if(open)layoutDrawingObjectsPanel();
        return;
    }
    const QWidget* focus=QApplication::focusWidget();
    const bool focusInside=focus&&(focus==drawingObjectsPanel_||drawingObjectsPanel_->isAncestorOf(focus));
    drawingObjectsPanel_->setVisible(open);
    if(open){
        syncDrawingObjectsPanel();
        layoutDrawingObjectsPanel();
        drawingObjectsPanel_->raise();
    }else if(focusInside&&chart_){
        chart_->setFocus();
    }
}
void MarketCandlesView::layoutDrawingObjectsPanel(){
    if(!drawingObjectsPanel_||!chart_||!drawingObjectsPanel_->isVisible())return;
    const int maxH=qMax(44,chart_->height()-16);
    const int w=qMin(280,qMax(120,chart_->width()-16));
    int desired=18;
    if(drawingObjectsScroll_&&drawingObjectsScroll_->widget()){
        auto* content=drawingObjectsScroll_->widget();
        for(auto* lay:std::initializer_list<QLayout*>{drawingPreviewLayout_,drawingSavedLayout_,drawingPreviewGroup_?drawingPreviewGroup_->layout():nullptr,drawingSavedGroup_?drawingSavedGroup_->layout():nullptr,content->layout()}){
            if(lay){lay->invalidate();lay->activate();}
        }
        int savedH=0;
        for(const auto& entry:drawingObjectRows_)if(entry.row)savedH+=qMax(32,entry.row->sizeHint().height())+2;
        if(drawingPreviewGroup_&&drawingPreviewGroup_->isVisible())savedH+=drawingPreviewGroup_->sizeHint().height()+6;
        if(drawingObjectsEmpty_&&drawingObjectsEmpty_->isVisible())savedH+=drawingObjectsEmpty_->sizeHint().height()+4;
        desired+=qMax(content->sizeHint().height(),savedH);
    }
    const int h=qBound(44,desired,maxH);
    const QPoint topLeft=chart_->mapTo(this,QPoint(8,8));
    drawingObjectsPanel_->setGeometry(topLeft.x(),topLeft.y(),w,h);
    if(drawingObjectsPanel_->layout()){drawingObjectsPanel_->layout()->invalidate();drawingObjectsPanel_->layout()->activate();}
    drawingObjectsPanel_->raise();
}
void MarketCandlesView::syncDrawingObjectsPanel(){
    if(!drawingObjectsPanel_||!drawingSavedLayout_)return;
    const auto ink=themeColors().text2,accent=themeColors().accent;
    if(drawingObjectIconInk_!=ink||drawingObjectAccentInk_!=accent){
        drawingObjectIconInk_=ink;drawingObjectAccentInk_=accent;
        drawingEyeShownIcon_=drawingControlIcon("eye",ink,true);
        drawingEyeHiddenIcon_=drawingControlIcon("eye",ink,false);
        drawingLockedIcon_=drawingControlIcon("objectLock",accent,true);
        drawingUnlockedIcon_=drawingControlIcon("objectLock",ink,false);
        drawingTrashIcon_=drawingControlIcon("chartDrawingClear",ink,false);
    }
    const auto shapes=drawingModel_?drawingModel_->drawings():QList<ChartDrawing>{};
    const bool modelLocked=drawingModel_&&drawingModel_->locked();
    const auto selectedId=static_cast<CandleChart*>(chart_)->selectedDrawing();
    QSet<QString> presentIds;
    for(const auto& shape:shapes)presentIds.insert(shape.id);
    for(auto it=drawingObjectRows_.begin();it!=drawingObjectRows_.end();){
        if(!presentIds.contains(it.key())){
            drawingSavedLayout_->removeWidget(it->row);
            delete it->row;
            it=drawingObjectRows_.erase(it);
        }else ++it;
    }
    for(int i=0;i<shapes.size();++i){
        const auto& shape=shapes[i];
        const auto rep=drawingRepresentativePrice(shape.kind,shape.anchors);
        const QString text=rep.isEmpty()?drawingKindName(shape.kind):drawingKindName(shape.kind)+QStringLiteral(" · ")+rep;
        auto it=drawingObjectRows_.find(shape.id);
        if(it==drawingObjectRows_.end()){
            SavedDrawingRow entry;
            entry.id=shape.id;
            auto* rowWidget=new DrawingObjectRowWidget([this,id=shape.id]{
                static_cast<CandleChart*>(chart_)->selectDrawing(id);
            },drawingSavedGroup_);
            rowWidget->setObjectName(QStringLiteral("chartDrawingObjectRow_")+shape.id);
            rowWidget->setMinimumHeight(32);
            auto* rowLayout=new QHBoxLayout(rowWidget);rowLayout->setContentsMargins(4,2,2,2);rowLayout->setSpacing(2);
            entry.label=new QLabel(rowWidget);entry.label->setFont(KeefiFonts::font(FontRole::Caption12));
            entry.label->setAttribute(Qt::WA_TransparentForMouseEvents);entry.label->setMinimumWidth(0);
            setThemeStyle(entry.label,"color:${text};");
            const auto makeButton=[&](const QString& name,const QString& accessible,bool checkable){
                auto* btn=new QToolButton(rowWidget);btn->setObjectName(name+shape.id);btn->setAccessibleName(accessible);btn->setToolTip(accessible);
                btn->setCheckable(checkable);btn->setFixedSize(28,28);btn->setIconSize(QSize(16,16));btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
                btn->installEventFilter(this);return btn;
            };
            entry.eye=makeButton(QStringLiteral("chartDrawingObjectEye_"),tr("개체 표시 전환"),true);
            entry.lock=makeButton(QStringLiteral("chartDrawingObjectLock_"),tr("개체 잠금 전환"),true);
            entry.remove=makeButton(QStringLiteral("chartDrawingObjectDelete_"),tr("개체 삭제"),false);
            connect(entry.eye,&QToolButton::toggled,this,[this,id=shape.id](bool visible){
                if(drawingModel_)drawingModel_->setDrawingVisible(id,visible);
            });
            connect(entry.lock,&QToolButton::toggled,this,[this,id=shape.id](bool locked){
                if(drawingModel_)drawingModel_->setDrawingLocked(id,locked);
            });
            connect(entry.remove,&QToolButton::clicked,this,[this,id=shape.id]{
                if(drawingModel_)drawingModel_->remove(id);
            });
            rowLayout->addWidget(entry.label,1);rowLayout->addWidget(entry.eye);rowLayout->addWidget(entry.lock);rowLayout->addWidget(entry.remove);
            entry.row=rowWidget;
            it=drawingObjectRows_.insert(shape.id,entry);
        }
        auto& entry=it.value();
        entry.label->setText(text);entry.label->setToolTip(text);
        entry.row->setProperty("selected",shape.id==selectedId);
        {
            const QSignalBlocker block(entry.eye);
            entry.eye->setChecked(shape.visible);
            entry.eye->setIcon(shape.visible?drawingEyeShownIcon_:drawingEyeHiddenIcon_);
            entry.eye->setEnabled(!modelLocked);
        }
        {
            const QSignalBlocker block(entry.lock);
            entry.lock->setChecked(shape.locked);
            entry.lock->setIcon(shape.locked?drawingLockedIcon_:drawingUnlockedIcon_);
            entry.lock->setEnabled(!modelLocked);
        }
        entry.remove->setIcon(drawingTrashIcon_);
        entry.remove->setEnabled(!modelLocked&&!shape.locked);
        if(drawingSavedLayout_->indexOf(entry.row)!=i){
            drawingSavedLayout_->removeWidget(entry.row);
            drawingSavedLayout_->insertWidget(i,entry.row);
        }
    }
    if(drawingObjectsEmpty_)drawingObjectsEmpty_->setVisible(shapes.isEmpty()&&drawingPreviewRows_.isEmpty());
    layoutDrawingObjectsPanel();
}

} // namespace keefi
