#include "keefi/KeefiTheme.h"
#include "keefi/ProductIdentity.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/CompactControls.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/InfoButton.h"
#include "keefi/CompactControls.h"
#include "keefi/LocalComboBox.h"
#include "keefi/LocalMenuControls.h"
#include "keefi/SegmentedControlStyle.h"
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QPaintEngine>
#include <QPixmap>
#include <QtMath>
#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/ChartStudySearch.h"
#include <QApplication>
#include <QAccessibleWidget>
#include <QScreen>
#include "keefi/MarketStudies.h"
#include "keefi/StockChartSource.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/CoinFlowStudyRunner.h"
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QSpinBox>
#include <QFocusEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include "keefi/ChartTimeAxis.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QLineF>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QShortcut>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QDataStream>
#include <QIODevice>
#include <QPainterPath>
#include <QPolygonF>
#include <QSet>
#include <QShowEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QTimeZone>
#include <QToolTip>
#include <QTextLayout>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <QGestureEvent>
#include <QPinchGesture>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <QUrl>
#include "keefi/MarketCandlesViewFormatting_p.h"
#include "keefi/CandleChart_p.h"

namespace keefi {
using namespace market_candles_detail;
namespace {
struct CompletedPublicStudies {
    ChartStudySet chart;
    CandleCompression compression;
    CandleStudySet explanation;
    ExtendedChartStudySet extended;
    QList<ChartCloudPoint> stockDisplayCloud;
    CandleStudySet stockDisplayAdx;
};
const StockFlowDay* latestStockFlowDay(const QString& identity,const StockChartSnapshot& snapshot){
    if(!identity.startsWith("stock:KR:")||snapshot.marketStudies.days.isEmpty())return nullptr;
    const auto& days=snapshot.marketStudies.days;
    const auto day=std::max_element(days.cbegin(),days.cend(),[](const auto& a,const auto& b){return a.date<b.date;});
    return day->foreignerNetBuyVolume||day->institutionNetBuyVolume||day->arbitrageNetBuyVolume||day->nonArbitrageNetBuyVolume?&*day:nullptr;
}
QByteArray completedStudyKey(const QString& identity,qint64 interval,const MarketCandleSeries& data){
    QByteArray key;QDataStream stream(&key,QIODevice::WriteOnly);stream<<identity<<interval<<data.asset<<data.currency<<data.available;
    if(data.available)for(const auto& row:data.candles)stream<<row.startsAtMs<<row.open<<row.high<<row.low<<row.close<<row.volume;
    else stream<<data.reason;
    return key;
}
void setNativeLedgerText(QLabel* label,const QString& plain,FontRole role){
    const auto rendered=KeefiFonts::mixedTextHtml(plain,role);
    if(label->text()!=rendered)label->setText(rendered);
}
QString selectablePlainText(const QLabel* label){
    QTextDocument document;document.setHtml(label->text());return document.toPlainText();
}
QString flowCoverage(CoinFlowCoverage value){switch(value){case CoinFlowCoverage::Unavailable:return QObject::tr("미확인");case CoinFlowCoverage::WarmingUp:return QObject::tr("표본 준비");
    case CoinFlowCoverage::ObservedContinuous:return QObject::tr("앱 관측 연속 · 시장 전체 무누락 보증 아님");case CoinFlowCoverage::RecoveredOverlap:return QObject::tr("중첩 관측 복구 · 시장 전체 무누락 보증 아님");
    case CoinFlowCoverage::Partial:return QObject::tr("부분 관측");case CoinFlowCoverage::Disconnected:return QObject::tr("연결 끊김");case CoinFlowCoverage::Overflow:return QObject::tr("관측 상한 초과");}return {};}
// Keep the original plain text for accessibility and tests, while applying the
// numeric face only to numeric runs in the wrapping legend.
class StudyLegendLabel final:public QLabel {
  public:
    explicit StudyLegendLabel(QWidget* parent):QLabel(parent){setFont(KeefiFonts::font(FontRole::Caption12));}
    void setLegendText(const QString& name,const QString& value){nameLength_=name.size();setText(name+(name.isEmpty()||value.isEmpty()?QString{}:QStringLiteral(" · "))+value);}
  protected:
    void paintEvent(QPaintEvent*)override{
        auto layoutText=text();layoutText.replace('\n',QChar::LineSeparator);
        QPainter painter(this);QTextLayout textLayout(layoutText,font());QList<QTextLayout::FormatRange> formats;
        QTextCharFormat base;base.setForeground(property("accent").toBool()?themeColors().accentText:themeColors().text);formats.append({0,int(text().size()),base});
        QTextCharFormat name;name.setForeground(property("accent").toBool()?themeColors().accentText:themeColors().text3);formats.append({0,nameLength_,name});
        auto numbers=QRegularExpression(QStringLiteral("[0-9][0-9,]*(?:\\.[0-9]+)?")).globalMatch(text());
        while(numbers.hasNext()){const auto match=numbers.next();QTextCharFormat number;number.setFont(KeefiFonts::font(FontRole::Number11));formats.append({int(match.capturedStart()),int(match.capturedLength()),number});}
        QTextOption option;option.setWrapMode(wordWrap()?QTextOption::WrapAtWordBoundaryOrAnywhere:QTextOption::NoWrap);textLayout.setTextOption(option);
        textLayout.setFormats(formats);textLayout.beginLayout();qreal heightUsed=0;
        for(;;){auto line=textLayout.createLine();if(!line.isValid())break;line.setLineWidth(width());line.setPosition(QPointF(0,heightUsed));heightUsed+=line.height();}textLayout.endLayout();
        textLayout.draw(&painter,QPointF(0,wordWrap()?0:(height()-heightUsed)/2));
    }
  private:int nameLength_=0;
};
QByteArray flowFingerprint(const CoinFlowSnapshot& data,const QString& status){
    QByteArray key;QDataStream out(&key,QIODevice::WriteOnly);out<<data.identity<<status;
    const auto evidence=[&](const CoinFlowEvidence& e){out<<e.source<<e.sourceUrl<<e.sourceTimeMs<<e.receivedAtMs<<e.knownAtMs;};
    const auto periods=[&](const QList<CoinFlowCoveragePeriod>& rows){out<<rows.size();for(const auto& r:rows)out<<r.fromMs<<r.throughMs<<r.knownAtMs<<r.epoch<<int(r.coverage)<<r.reason;};
    const auto window=[&](const CoinFlowWindow& w){const auto& i=w.instrument;out<<int(i.venue)<<int(i.grain)<<i.identity<<i.symbol<<i.baseAsset<<i.quoteAsset<<i.marginAsset<<i.contractType<<i.status<<i.underlyingType<<i.verified;evidence(i.evidence);
        // Held trades are never edited in place: the collector only inserts new IDs or drops the
        // whole window (overflow, conflict, reconnect). Count plus the two ends therefore marks every
        // change without serialising thousands of rows on each 250 ms refresh; the view also
        // recalculates at least once a second regardless of this key.
        const auto trade=[&](const CoinTradeObservation& t){out<<t.id<<t.price<<t.quantity<<t.normalQuantity.has_value();if(t.normalQuantity)out<<*t.normalQuantity;
            out<<int(t.side)<<t.tradeAtMs<<t.eventAtMs<<t.receivedAtMs<<t.knownAtMs<<t.epoch<<t.snapshot<<t.liveEligible;};
        out<<w.trades.size();if(!w.trades.isEmpty()){trade(w.trades.first());trade(w.trades.last());}
        qint64 latestKnown=0;for(const auto& t:w.trades)latestKnown=std::max(latestKnown,t.knownAtMs);out<<latestKnown;
        periods(w.periods);out<<w.duplicateCount<<w.reason;evidence(w.evidence);};
    window(data.primary);out<<data.companion.has_value();if(data.companion)window(*data.companion);
    out<<data.minutes.size();for(const auto& r:data.minutes){const auto& c=r.candle;out<<c.startsAtMs<<c.open<<c.high<<c.low<<c.close<<c.volume<<r.completesAtMs<<r.knownAtMs<<r.decisionAtMs<<r.connectsPrevious<<r.liveEligible;evidence(r.evidence);}
    out<<data.openInterest.size();for(const auto& p:data.openInterest){out<<p.symbol<<p.quantity<<p.value<<p.periodEndMs<<p.knownAtMs<<p.liveEligible;evidence(p.evidence);}
    out<<data.liquidations.size();for(const auto& p:data.liquidations){out<<p.symbol<<p.side<<p.price<<p.averagePrice<<p.originalQuantity<<p.lastFilledQuantity<<p.cumulativeFilledQuantity<<p.orderStatus<<p.tradeAtMs<<p.eventAtMs<<p.receivedAtMs<<p.knownAtMs<<p.epoch;evidence(p.evidence);}
    periods(data.liquidationCoverage);out<<data.candleSeries.has_value();if(data.candleSeries){const auto& s=*data.candleSeries;
        out<<s.available<<s.identity<<s.asset<<s.currency<<s.intervalMs<<s.sourceUrl<<s.receivedAtMs<<s.sourceTimeMs<<s.candles.size();
        for(const auto& c:s.candles)out<<c.startsAtMs<<c.open<<c.high<<c.low<<c.close<<c.volume;}
    return key;
}

class StudyLegendOverlay final : public QWidget {
public:
    explicit StudyLegendOverlay(QWidget* fallback):QWidget(fallback),fallback_(fallback) {
        setAttribute(Qt::WA_StyledBackground);
        setFixedHeight(CompactButtonHeight+2);
        fallback_->setFixedHeight(height());
        fallback_->hide();
    }

    int widthBudget(QWidget* chart,int viewWidth)const {
        return chart->isHidden()?viewWidth:qMax(0,chart->width()-CandlePlotLeft-CandlePriceAxisMinimum);
    }

    static int minimumWidthFor(int chips,bool overflow) {
        return extraWidth(chips,overflow)+chips*(StudyChipLeadingPadding+3*CompactButtonHeight);
    }

    void arrange(QWidget* chart,int budget,const QStringList& ids,bool overflow,bool visible) {
        // The chart is intentionally hidden before a usable observation. Keep
        // those same study controls usable in their former row while it is hidden.
        const bool overlaying=!chart->isHidden();
        auto* host=overlaying?chart:fallback_;
        const bool reparent=visible&&parentWidget()!=host;
        auto* focused=QApplication::focusWidget();
        const QPointer<QWidget> restoreFocus=reparent&&focused&&isAncestorOf(focused)?focused:nullptr;
        fallback_->setVisible(visible&&!overlaying);
        if(!visible){hide();return;}
        if(reparent)setParent(host);

        // Titles and the current chip selection determine the width. Quote
        // values and hover state never change the geometry or trigger layout.
        if(reparent||budget!=arrangedBudget_||ids!=arrangedIds_||overflow!=arrangedOverflow_){
            int preferred=extraWidth(int(ids.size()),overflow);
            const QFontMetrics metrics(KeefiFonts::font(FontRole::Caption12));
            for(const auto& id:ids)
                preferred+=StudyChipLeadingPadding+2*CompactButtonHeight
                    +qMax(CompactButtonHeight,StudyChipInfoMarkWidth+metrics.horizontalAdvance(studyName(id)));
            const int top=CandlePlotTop+CandleOhlcInset+CandleOhlcHeight+2;
            setGeometry(overlaying?CandlePlotLeft:0,overlaying?top:0,qMin(preferred,budget),height());
            arrangedBudget_=budget;arrangedIds_=ids;arrangedOverflow_=overflow;
        }
        if(isHidden()){show();raise();}
        if(restoreFocus&&restoreFocus->isVisible())restoreFocus->setFocus(Qt::OtherFocusReason);
    }

protected:
    bool eventFilter(QObject* watched,QEvent* event)override {
        if(event->type()==QEvent::Wheel&&forwardWheel(static_cast<QWheelEvent*>(event)))return true;
        return QWidget::eventFilter(watched,event);
    }

    void wheelEvent(QWheelEvent* event)override {
        if(!forwardWheel(event))QWidget::wheelEvent(event);
    }

private:
    bool forwardWheel(QWheelEvent* event) {
        auto* chart=parentWidget();
        if(chart==fallback_)return false;
        QWheelEvent forwarded(chart->mapFromGlobal(event->globalPosition()),event->globalPosition(),
            event->pixelDelta(),event->angleDelta(),event->buttons(),event->modifiers(),
            event->phase(),event->inverted(),event->source(),event->pointingDevice());
        forwarded.setTimestamp(event->timestamp());
        QCoreApplication::sendEvent(chart,&forwarded);
        event->setAccepted(forwarded.isAccepted());
        return event->isAccepted();
    }

    static int extraWidth(int chips,bool overflow) {
        return 2*StudyLegendPadding+qMax(0,chips-1)*StudyLegendChipGap
            +(overflow?CompactButtonHeight+(chips>0?StudyLegendMoreGap:0):0);
    }
    QWidget* fallback_;
    int arrangedBudget_=-1;
    QStringList arrangedIds_;
    bool arrangedOverflow_=false;
};

class StudyChip final:public QWidget {
  public:using QWidget::QWidget;
  protected:void paintEvent(QPaintEvent*)override{QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setPen(themeColors().rule);p.setBrush(themeColors().bg);p.drawRoundedRect(QRectF(0.5,7.5,width()-1,29),8,8);}
};
class StudyPanePlot final : public QWidget {
  public:
    StudyPanePlot(CandleChart* chart, QWidget* parent) : QWidget(parent), chart_(chart) {
        setMinimumHeight(64);
        setMinimumWidth(0);
    }
    QList<QList<ChartPricePoint>> lines;
    QList<QColor> colors;
    QList<ChartPricePoint> bars;
    QColor risingBar, fallingBar;
    QString reason;
    bool rsi = false, stock = false;
    qint64 interval = 0;
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), themeColors().bg);
        painter.setPen(themeColors().text3);
        painter.setFont(KeefiFonts::font(FontRole::Caption12));
        if (lines.isEmpty() && bars.isEmpty()) {
            painter.drawText(rect(), Qt::AlignCenter, reason.isEmpty() ? tr("완료 봉 관측 대기") : reason);
            return;
        }
        // The price chart owns the UTC/stock-session mapping, axis gutter, zoom
        // and right padding. A short MACD warm-up must not stretch its own x-axis.
        const auto view = chart_->studyViewport();
        const auto [from, to] = view.slotsRange;
        const QRectF area(view.priceArea.left(), 4, view.priceArea.width(), qMax(1, height()-12));
        if (!view.axis.available || from >= to || area.width() <= 0) {
            painter.drawText(rect(), Qt::AlignCenter, tr("봉 시간축을 확인하고 있습니다"));
            return;
        }
        const auto coordinate = [&](const ChartPricePoint& point) -> std::optional<double> {
            if (!std::isfinite(point.price)) return {};
            const auto slot = view.axis.coordinateAt(point.atMs);
            if (!slot || *slot < from || *slot > to) return {};
            return area.left()+(*slot-from)/(to-from)*area.width();
        };
        double low = std::numeric_limits<double>::infinity(), high = -low;
        const auto scan = [&](const QList<ChartPricePoint>& points) {
            for (const auto& point : points) if (coordinate(point)) {
                low=qMin(low,point.price); high=qMax(high,point.price);
            }
        };
        for (const auto& line : lines) scan(line);
        scan(bars);
        if (low > high) {
            painter.drawText(rect(), Qt::AlignCenter, tr("현재 구간에 관측값이 없습니다"));
            return;
        }
        if (rsi) { low=0; high=100; }
        if (!bars.isEmpty()) { low=qMin(0.0,low); high=qMax(0.0,high); }
        if (high==low) { low-=1; high+=1; }
        const auto y = [&](double v) {return area.bottom()-(v-low)/(high-low)*area.height();};
        painter.setPen(themeColors().rule);
        painter.drawLine(area.bottomLeft(), area.bottomRight());
        if (rsi) for (const int v : {30,70}) painter.drawLine(QPointF(area.left(),y(v)),QPointF(area.right(),y(v)));
        painter.save();
        painter.setClipRect(area.adjusted(-1,-1,1,1));
        for (const auto& point : bars) if (const auto x = coordinate(point)) {
            painter.setPen(QPen(point.price >= 0 ? risingBar : fallingBar, 2));
            painter.drawLine(QPointF(*x,y(0)), QPointF(*x,y(point.price)));
        }
        for (qsizetype i=0; i<lines.size(); ++i) {
            painter.setPen(QPen(colors.value(i,themeColors().studyPurple), 1.5));
            QPainterPath path;
            qint64 previous = 0;
            for (const auto& point : lines[i]) {
                const auto x=coordinate(point);
                if (!x) { previous=0; continue; }
                const QPointF here(*x,y(point.price));
                const bool connected=previous && (stock ? view.axis.connects(previous,point.atMs) : point.atMs-previous==interval);
                if (connected) path.lineTo(here);
                else { path.moveTo(here); painter.drawPoint(here); }
                previous=point.atMs;
            }
            painter.drawPath(path);
        }
        painter.restore();
        painter.setFont(KeefiFonts::font(FontRole::Number11));painter.setPen(themeColors().text3);
        const qreal labelLeft=area.right()+5, labelWidth=qMax(0.0,width()-labelLeft);
        painter.drawText(QRectF(labelLeft,0,labelWidth,14),QString::number(high,'g',5));
        painter.drawText(QRectF(labelLeft,area.bottom()-8,labelWidth,14),QString::number(low,'g',5));
    }
  private:
    CandleChart* chart_;
};

class LowerStudyPane final : public QWidget {
  public:
    LowerStudyPane(const QString& id, CandleChart* chart, QWidget* parent) : QWidget(parent) {
        setObjectName("chartLowerPane_"+id);
        auto* layout=new QVBoxLayout(this);
        layout->setContentsMargins(0,0,0,0);
        layout->setSpacing(0);
        auto* header=new QHBoxLayout;
        title=new QLabel(this);
        title->setObjectName("chartLowerPaneTitle_"+id);
        title->setFont(KeefiFonts::font(FontRole::Caption12));
        header->addWidget(title,1);
        hideButton=new QPushButton(tr("숨기기"),this);
        hideButton->setObjectName("chartLowerPaneHide_"+id);
        hideButton->setAccessibleName(studyName(id)+tr(" 숨기기"));
        hideButton->setMinimumSize(44,44);
        header->addWidget(hideButton);
        layout->addLayout(header);
        plot=new StudyPanePlot(chart,this);
        plot->setObjectName("chartLowerPanePlot_"+id);
        plot->setAccessibleName(studyName(id));
        layout->addWidget(plot,1);
    }
    QLabel* title;
    QPushButton* hideButton;
    StudyPanePlot* plot;
};
}
MarketCandlesView::MarketCandlesView(MarketCandles* model,QWidget* p):QWidget(p),model_(model){
    setObjectName("marketCandlesView");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(8);
    const auto label=[&](const char* name,FontRole role=FontRole::Body15){auto* value=new LedgerTextLabel(role,this);value->setObjectName(name);value->setWordWrap(true);value->setTextFormat(Qt::PlainText);layout->addWidget(value);return value;};
    auto* heading=new QHBoxLayout;headingLayout_=heading;
    title_=new LedgerTextLabel(FontRole::BodyStrong15,this);title_->setObjectName("marketCandlesTitle");title_->setWordWrap(false);title_->setMinimumWidth(0);title_->setTextFormat(Qt::PlainText);
    heading->addWidget(title_,1);
    auto* information=new InfoButton(this);information_=information;information->setObjectName("marketCandlesInformation");information->setFixedSize(CompactButtonHeight,CompactButtonHeight);heading->addWidget(information);
    indicatorPicker_=new QPushButton(tr("fx Indicators"),this);indicatorPicker_->setObjectName("chartStudyPicker");indicatorPicker_->setAccessibleName(tr("Find and enable indicators"));indicatorPicker_->setMinimumSize(60,44);
    applyCompactButton(indicatorPicker_);
    layout->addLayout(heading);
    connect(indicatorPicker_,&QPushButton::clicked,this,&MarketCandlesView::showIndicatorPicker);
    indicatorApplied_=new QPushButton(this);indicatorApplied_->setObjectName("chartStudyAppliedToggle");indicatorApplied_->setCheckable(true);indicatorApplied_->setFixedHeight(CompactButtonHeight);
    setThemeStyle(indicatorApplied_,"QPushButton { border:0; border-radius:4px; background:transparent; color:${text2}; padding:0 8px; } QPushButton:hover, QPushButton:focus { background:${surface}; }");
    heading->insertWidget(1,indicatorApplied_);
    connect(indicatorApplied_,&QPushButton::toggled,this,[this]{layoutIndicatorChips();});
    // Native rich-text layout owns both mixed glyph widths and wrapped height.
    status_=new QLabel(this);status_->setObjectName("marketCandlesStatus");status_->setFont(KeefiFonts::font(FontRole::Caption12));
    status_->setTextFormat(Qt::RichText);status_->setWordWrap(true);status_->setMinimumWidth(0);status_->setTextInteractionFlags(Qt::NoTextInteraction);layout->addWidget(status_);setThemeStyle(status_,"color:${text3};");
    auto* stockActions=new QWidget(this);
    auto* actionLayout=new QHBoxLayout(stockActions);actionLayout->setContentsMargins(0,0,0,0);
    stockConnect_=new QPushButton(tr("증권 연결하기"),stockActions);stockConnect_->setObjectName("chartStockConnect");
    stockDaily_=new QPushButton(tr("키움 국내 일봉 보기"),stockActions);stockDaily_->setObjectName("chartStockKiwoomDaily");
    for(auto* button:{stockConnect_,stockDaily_}){button->setMinimumHeight(36);actionLayout->addWidget(button);}
    actionLayout->addStretch();layout->addWidget(stockActions);stockActions->hide();
    connect(stockConnect_,&QPushButton::clicked,this,[this]{
        if(identity_.startsWith("stock:")&&stockSnapshot_.connectionIssue)emit stockConnectionRequested(identity_,intervalMs_);
    });
    connect(stockDaily_,&QPushButton::clicked,this,[this]{
        if(stockSnapshot_.kiwoomDailyAlternative)emit stockDailyRequested();
    });
    archiveNotice_=new QWidget(this);archiveNotice_->setObjectName("marketCandlesArchiveNotice");
    auto* archiveLayout=new QHBoxLayout(archiveNotice_);archiveLayout->setContentsMargins(0,0,0,0);
    archiveMessage_=new QLabel(archiveNotice_);archiveMessage_->setObjectName("marketCandlesArchiveStatus");
    archiveMessage_->setFont(KeefiFonts::font(FontRole::Caption12));setThemeStyle(archiveMessage_,"color:${text3};");
    archiveMessage_->setTextFormat(Qt::PlainText);archiveMessage_->setWordWrap(true);archiveLayout->addWidget(archiveMessage_,1);
    archiveRetry_=new QPushButton(tr("다시 시도"),archiveNotice_);archiveRetry_->setObjectName("marketCandlesArchiveRetry");
    archiveRetry_->setAccessibleName(tr("시세 기록 저장 다시 시도"));applyCompactButton(archiveRetry_);archiveLayout->addWidget(archiveRetry_);
    connect(archiveRetry_,&QPushButton::clicked,this,[this]{if(model_)model_->retryArchive();});
    archiveNotice_->hide();layout->addWidget(archiveNotice_);
    stockContext_=label("marketCandlesStockContext",FontRole::Caption12);stockContext_->hide();
    stockObservations_=label("marketCandlesStockObservations",FontRole::Caption12);stockObservations_->hide();
    indicatorIntro_=new QWidget(this);indicatorIntro_->setObjectName("chartStudyIntro");
    auto* introLayout=new QHBoxLayout(indicatorIntro_);introLayout->setContentsMargins(0,0,0,0);
    auto* introText=new QLabel(tr("%1 지표를 켜 뒀어요. 눈 버튼은 숨김, ✕는 끄기예요. 지표에서 다시 켤 수 있어요.").arg(productName()),indicatorIntro_);
    introText->setWordWrap(true);introText->setTextFormat(Qt::PlainText);introLayout->addWidget(introText,1);
    auto* introDismiss=new QPushButton(tr("확인"),indicatorIntro_);introDismiss->setObjectName("chartStudyIntroDismiss");applyCompactButton(introDismiss);introLayout->addWidget(introDismiss);
    connect(introDismiss,&QPushButton::clicked,this,[this]{introDismissed_=true;indicatorIntro_->hide();});
    indicatorIntro_->hide();layout->addWidget(indicatorIntro_);
    auto* legendFallback=new QWidget(this);
    indicatorLegend_=new StudyLegendOverlay(legendFallback);indicatorLegend_->setObjectName("chartStudyLegend");
    setThemeStyle(indicatorLegend_,"QWidget#chartStudyLegend { background:${bg}; border:1px solid ${rule}; border-radius:6px; }");
    auto* legendRow=new QHBoxLayout(indicatorLegend_);legendRow->setContentsMargins(StudyLegendPadding,1,StudyLegendPadding,1);legendRow->setSpacing(StudyLegendMoreGap);
    auto* legendContents=new QWidget(indicatorLegend_);legendContents->setObjectName("chartStudyLegendContents");legendContents->setMinimumWidth(0);
    indicatorLegendLayout_=new QGridLayout(legendContents);indicatorLegendLayout_->setContentsMargins(0,0,0,0);indicatorLegendLayout_->setHorizontalSpacing(StudyLegendChipGap);indicatorLegendLayout_->setVerticalSpacing(6);
    legendRow->addWidget(legendContents,1);
    indicatorMore_=new QPushButton(indicatorLegend_);indicatorMore_->setObjectName("chartStudyMore");
    applyCompactButton(indicatorMore_, true);
    indicatorMore_->setFont(KeefiFonts::font(FontRole::Caption12));
    indicatorMore_->setAccessibleName(tr("Show all enabled indicators"));
    connect(indicatorMore_,&QPushButton::clicked,this,&MarketCandlesView::showIndicatorOverflow);legendRow->addWidget(indicatorMore_,0,Qt::AlignTop);
    chartStyleButton_=new QToolButton(this);chartStyleButton_->setObjectName("chartStyleButton");
    chartStyleButton_->setIconSize(QSize(16,16));chartStyleButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    applyCompactButton(chartStyleButton_, true);
    connect(chartStyleButton_,&QToolButton::clicked,this,[this]{
        QMenu menu(this);menu.setObjectName("chartStyleMenu");const auto current=chartStyle();
        for(const auto& option:QList<QPair<QString,QString>>{{"candles",tr("캔들")},{"bars",tr("바 · 시가·고가·저가·종가")},{"line",tr("선 · 종가")}}){
            auto* action=menu.addAction(chartLineIcon("style-"+option.first,themeColors().text2),option.second);action->setObjectName("chartStyle_"+option.first);
            action->setCheckable(true);action->setChecked(current==option.first);connect(action,&QAction::triggered,this,[this,key=option.first]{setChartStyle(key);});}
        applyLocalMenuStyle(&menu);auto* anchor=chartStyleButton_->isVisible()||!toolbarMenuAnchor_?static_cast<QWidget*>(chartStyleButton_):toolbarMenuAnchor_.data();menu.exec(anchor->mapToGlobal(QPoint(0,anchor->height())));anchor->setFocus();});
    heading->insertWidget(2,chartStyleButton_);
    heading->insertWidget(3,indicatorPicker_);
    ChartStudyPreferences supported;for(const auto& id:supported.supportedIds("coin")+QStringList{"stock_flow"}){
        auto* chip=new StudyChip(legendContents);chip->setObjectName("chartStudyChip_"+id);chip->setMinimumWidth(0);chip->setFixedHeight(CompactButtonHeight);
        auto* row=new QHBoxLayout(chip);row->setContentsMargins(StudyChipLeadingPadding,0,0,0);row->setSpacing(0);
        auto* help=new QPushButton(chip);help->setObjectName("chartStudyHelp_"+id);help->setAccessibleName(studyName(id)+tr(" Information"));help->setMinimumWidth(CompactButtonHeight);help->setFixedHeight(CompactButtonHeight);help->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);help->setAutoDefault(true);
        auto* helpLayout=new QHBoxLayout(help);helpLayout->setContentsMargins(0,0,0,0);
        auto* text=new StudyLegendLabel(help);text->setObjectName("chartStudyStatus_"+id);text->setWordWrap(false);text->setMinimumWidth(0);text->setTextFormat(Qt::PlainText);text->setAttribute(Qt::WA_TransparentForMouseEvents);helpLayout->addWidget(text,1);
        auto* mark=new QLabel(tr("ⓘ"),help);mark->setFont(KeefiFonts::font(FontRole::Caption12));mark->setFixedWidth(StudyChipInfoMarkWidth);mark->setAttribute(Qt::WA_TransparentForMouseEvents);helpLayout->addWidget(mark);row->addWidget(help,1);
        setThemeStyle(help,"QPushButton { border:0; padding:0; background:transparent; text-align:left; } QPushButton:hover, QPushButton:focus { background:${surface}; }");connect(help,&QPushButton::clicked,this,[this,id,help]{showIndicatorHelp(id,help);});
        auto* eye=new QToolButton(chip);eye->setObjectName("chartStudyEye_"+id);eye->setCheckable(true);eye->setIconSize(QSize(16,16));applyCompactButton(eye, true);row->addWidget(eye);
        auto* off=new QToolButton(chip);off->setObjectName("chartStudyOff_"+id);off->setAccessibleName(studyName(id)+tr(" Turn Off"));off->setToolTip(tr("이 지표의 적용을 꺼요."));off->setIconSize(QSize(16,16));applyCompactButton(off, true);row->addWidget(off);
        connect(eye,&QToolButton::toggled,this,[this,id](bool visible){if(indicatorPreferences_)indicatorPreferences_->setVisible(id,visible);});
        connect(off,&QToolButton::clicked,this,[this,id]{if(indicatorPreferences_)indicatorPreferences_->setEnabled(id,false);});
        indicatorChips_.insert(id,chip);chip->hide();
    }
    // Descendant buttons keep normal mouse and keyboard activation. Only
    // wheel events over those controls belong to the underlying plot.
    for(auto* child:indicatorLegend_->findChildren<QWidget*>())child->installEventFilter(indicatorLegend_);
    drawingToolbar_=new QWidget(this);drawingToolbar_->setObjectName("chartDrawingToolbar");drawingToolbarLayout_=new QGridLayout(drawingToolbar_);drawingToolbarLayout_->setContentsMargins(0,0,0,0);drawingToolbarLayout_->setSpacing(4);
    setThemeStyle(drawingToolbar_,"QToolButton { border:0; border-radius:0; padding:0; background:transparent; } QToolButton:checked, QToolButton:hover { background:${surface}; }");
    const auto tool=[&](ChartDrawingTool value,const QString& text,const char* name){auto* button=new QToolButton(drawingToolbar_);button->setObjectName(name);button->setText(text);button->setToolTip(text);button->setAccessibleName(text);button->setCheckable(true);button->setAutoExclusive(true);button->setFixedSize(44,44);button->setIconSize(QSize(20,20));button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        drawingTools_.insert(value,button);connect(button,&QToolButton::toggled,this,[this,value](bool selected){if(selected){static_cast<CandleChart*>(chart_)->setDrawingTool(value);refreshDrawingUi();}});};
    tool(ChartDrawingTool::Crosshair,tr("십자선"),"chartDrawingTool_crosshair");tool(ChartDrawingTool::Trend,tr("추세선"),"chartDrawingTool_trend");
    tool(ChartDrawingTool::Horizontal,tr("수평선"),"chartDrawingTool_horizontal");tool(ChartDrawingTool::Measure,tr("측정"),"chartDrawingTool_measure");
    const auto control=[&](const QString& text,const char* name,bool checked){auto* button=new QToolButton(drawingToolbar_);button->setObjectName(name);button->setText(text);button->setToolTip(text);button->setAccessibleName(text);button->setCheckable(checked);button->setFixedSize(44,44);button->setIconSize(QSize(20,20));button->setToolButtonStyle(Qt::ToolButtonIconOnly);return button;};
    drawingAuto_=control(tr("자동 작도"),"chartDrawingAuto",false);
    drawingObjects_=control(tr("개체 목록"),"chartDrawingObjects",true);
    drawingMagnet_=control(tr("자석"),"chartDrawingMagnet",true);drawingVisible_=control(tr("그림 표시"),"chartDrawingVisible",true);
    drawingLocked_=control(tr("편집 잠금"),"chartDrawingLocked",true);drawingClear_=control(tr("모두 지우기"),"chartDrawingClear",false);
    drawingVisible_->setToolTip(tr("그림을 숨겨도 그림에 켠 알림은 계속됩니다."));drawingLocked_->setToolTip(tr("그림 편집만 잠급니다. 그림에 켠 알림은 계속됩니다."));
    drawingMagnet_->setToolTip(tr("실제 완료 봉의 시가·고가·저가·종가에만 붙입니다."));
    connect(drawingAuto_,&QToolButton::clicked,this,[this]{runAutoChartGeometry();});
    connect(drawingObjects_,&QToolButton::toggled,this,[this](bool on){setDrawingObjectsOpen(on);});
    connect(drawingMagnet_,&QToolButton::toggled,this,[this](bool on){if(drawingModel_)drawingModel_->setMagnetEnabled(on);});
    connect(drawingVisible_,&QToolButton::toggled,this,[this](bool on){if(drawingModel_)drawingModel_->setVisible(on);});
    connect(drawingLocked_,&QToolButton::toggled,this,[this](bool on){if(drawingModel_)drawingModel_->setLocked(on);});
    connect(drawingClear_,&QToolButton::clicked,this,[this]{if(drawingModel_)drawingModel_->clear();});
    // Undo sits in the toolbar; redo is in the "more" menu. Both also have the standard shortcuts
    // (⌘Z / ⇧⌘Z) anywhere inside the chart view, while a text field keeps its own undo.
    drawingUndo_=control(tr("그리기 실행취소"),"chartDrawingUndo",false);drawingRedo_=control(tr("그리기 다시 실행"),"chartDrawingRedo",false);
    connect(drawingUndo_,&QToolButton::clicked,this,[this]{undoDrawing(false);});connect(drawingRedo_,&QToolButton::clicked,this,[this]{undoDrawing(true);});
    for(const auto redo:{false,true}){auto* action=new QAction(this);action->setObjectName(redo?"chartDrawingRedoShortcut":"chartDrawingUndoShortcut");
        // Platform bindings plus ⇧⌘Z for redo, which some platform themes leave out.
        auto keys=QKeySequence::keyBindings(redo?QKeySequence::Redo:QKeySequence::Undo);const QKeySequence shiftZ(Qt::CTRL|Qt::SHIFT|Qt::Key_Z);
        if(redo&&!keys.contains(shiftZ))keys.append(shiftZ);action->setShortcuts(keys);action->setShortcutContext(Qt::WidgetWithChildrenShortcut);addAction(action);
        connect(action,&QAction::triggered,this,[this,redo]{undoDrawing(redo);});}
    drawingMore_=new QToolButton(drawingToolbar_);drawingMore_->setObjectName("chartDrawingMore");drawingMore_->setText("···");drawingMore_->setAccessibleName(tr("그리기 설정 더 보기"));drawingMore_->setFixedSize(44,44);
    connect(drawingMore_,&QToolButton::clicked,this,[this]{QMenu menu(this);for(auto* button:{drawingObjects_,drawingRedo_,drawingMagnet_,drawingVisible_,drawingLocked_,drawingClear_}){
        auto* action=menu.addAction(button->icon(),button->accessibleName());action->setEnabled(button->isEnabled());action->setCheckable(button->isCheckable());action->setChecked(button->isChecked());
        connect(action,&QAction::triggered,button,&QToolButton::click);
    }applyLocalMenuStyle(&menu);menu.exec(drawingMore_->mapToGlobal(QPoint(0,drawingMore_->height())));drawingMore_->setFocus();});
    layout->addWidget(legendFallback);
    // Applied-study details share the explicit expand/collapse control. Folding
    // presentation never changes saved enabled/visible bits or calculations.
    coreStudySummary_=new QWidget(this);coreStudySummary_->setObjectName("chartCoreStudySummary");
    coreStudySummaryLayout_=new QGridLayout(coreStudySummary_);coreStudySummaryLayout_->setContentsMargins(0,0,0,0);coreStudySummaryLayout_->setHorizontalSpacing(12);coreStudySummaryLayout_->setVerticalSpacing(4);
    for(const auto* id:{"pullback","similarity","whales"}){
        auto* card=new QWidget(coreStudySummary_);card->setObjectName("chartCoreStudyCard_"+QString(id));card->setMinimumWidth(0);
        auto* cells=new QVBoxLayout(card);cells->setContentsMargins(0,2,0,2);cells->setSpacing(1);
        auto* heading=new QLabel(card);heading->setObjectName("chartCoreStudyState_"+QString(id));heading->setTextFormat(Qt::PlainText);heading->setWordWrap(true);heading->setMinimumWidth(0);heading->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);heading->setFont(KeefiFonts::font(FontRole::Caption12));cells->addWidget(heading);
        auto* detail=new QLabel(card);detail->setObjectName("chartCoreStudyDetail_"+QString(id));detail->setTextFormat(Qt::RichText);detail->setTextInteractionFlags(Qt::TextBrowserInteraction);detail->setOpenExternalLinks(false);detail->setFocusPolicy(Qt::StrongFocus);detail->setWordWrap(true);detail->setMinimumWidth(0);detail->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);detail->setFont(KeefiFonts::font(FontRole::Caption12));cells->addWidget(detail);
        connect(detail,&QLabel::linkActivated,this,[this](const QString& link){bool ok=false;const auto at=link.section(':',1).toLongLong(&ok);if(ok&&at>0&&(link.startsWith("match:")||link.startsWith("reference:")||link.startsWith("signal:")))static_cast<CandleChart*>(chart_)->navigateToTime(at,true);});
        setThemeStyle(heading,"color:${text};");setThemeStyle(detail,"color:${text3};");coreStudyCards_.insert(id,card);
    }
    layout->addWidget(coreStudySummary_);layoutCoreStudySummary();
    connect(this,&MarketCandlesView::viewportChanged,this,&MarketCandlesView::refreshCoreStudySummary);
    auto* plotBody = new QWidget(this); plotBody->setObjectName("chartPlotWithTools");
    auto* plotLayout = new QHBoxLayout(plotBody); plotLayout->setContentsMargins(0,0,0,0); plotLayout->setSpacing(8);
    plotLayout->addWidget(drawingToolbar_,0,Qt::AlignTop);
    auto* chartColumn=new QWidget(plotBody);auto* chartColumnLayout=new QVBoxLayout(chartColumn);chartColumnLayout->setContentsMargins(0,0,0,0);chartColumnLayout->setSpacing(4);
    chart_=new CandleChart(chartColumn);chart_->installEventFilter(this);chartColumnLayout->addWidget(chart_,1);
    auto* navigation=new QWidget(chartColumn);navigation->setObjectName("chartHistoryNavigation");
    auto* navigationLayout=new QHBoxLayout(navigation);navigationLayout->setContentsMargins(0,0,0,0);navigationLayout->setSpacing(6);
    historyStatus_=new QLabel(navigation);historyStatus_->setObjectName("chartHistoryStatus");historyStatus_->setFont(KeefiFonts::font(FontRole::Caption12));
    historyStatus_->setTextFormat(Qt::PlainText);historyStatus_->setMinimumWidth(0);historyStatus_->setWordWrap(true);navigationLayout->addWidget(historyStatus_,1);
    historyRetry_=new QPushButton(tr("이전 봉 다시 시도"),navigation);historyRetry_->setObjectName("chartHistoryRetry");applyCompactButton(historyRetry_);navigationLayout->addWidget(historyRetry_);
    liveReturn_=new QPushButton(tr("실시간으로"),navigation);liveReturn_->setObjectName("chartReturnToLive");applyCompactButton(liveReturn_);navigationLayout->addWidget(liveReturn_);
    chartColumnLayout->addWidget(navigation);
    // These studies produce a current observation, rather than a price series.
    // Keep their actual values in the chart even when the editing legend is closed.
    studyReadoutStrip_=new QScrollArea(chartColumn);
    studyReadoutStrip_->setObjectName("chartStudyReadouts");
    studyReadoutStrip_->setAccessibleName(tr("지표 관측값"));
    studyReadoutStrip_->setFrameShape(QFrame::NoFrame);
    studyReadoutStrip_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    studyReadoutStrip_->setWidgetResizable(true);
    auto* readoutContent=new QWidget(studyReadoutStrip_);
    auto* readouts=new QHBoxLayout(readoutContent);
    readouts->setContentsMargins(2,0,2,0);readouts->setSpacing(16);
    readouts->setSizeConstraint(QLayout::SetMinAndMaxSize);
    for(const auto* id:{"contract","absorption","sweep","squeeze","adx","breakout","strong","trend","relative_strength"}){
        auto* value=new QLabel(readoutContent);value->setTextFormat(Qt::PlainText);
        value->setFont(KeefiFonts::font(FontRole::Caption12));value->setObjectName("chartStudyReadout_"+QString(id));
        value->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);
        readouts->addWidget(value);studyReadouts_.insert(id,value);value->hide();
    }
    readouts->addStretch();
    studyReadoutStrip_->setWidget(readoutContent);
    studyReadoutStrip_->setFixedHeight(QFontMetrics(KeefiFonts::font(FontRole::Caption12)).height()
        +studyReadoutStrip_->horizontalScrollBar()->sizeHint().height()+6);
    chartColumnLayout->addWidget(studyReadoutStrip_);studyReadoutStrip_->hide();
    connect(liveReturn_,&QPushButton::clicked,this,&MarketCandlesView::returnToLive);
    connect(historyRetry_,&QPushButton::clicked,this,[this]{if(model_&&isVisible()&&!identity_.startsWith("stock:"))model_->requestOlder();});
    static_cast<CandleChart*>(chart_)->viewportChanged=[this]{refreshHistoryNavigation();emit viewportChanged();};
    static_cast<CandleChart*>(chart_)->userNavigated=[this](ChartSyncViewport range,bool zoom){emit userViewportNavigated(range,zoom);};
    static_cast<CandleChart*>(chart_)->crosshairHovered=[this](qint64 utc){emit crosshairHovered(utc);};
    static_cast<CandleChart*>(chart_)->olderRequested=[this]{
        if(!historyPrefetchQueued_){historyPrefetchQueued_=true;QTimer::singleShot(0,this,[this]{historyPrefetchQueued_=false;
            if(model_&&isVisible()&&identity_.startsWith("coin:")&&chart_->property("displayIdentity").toString()==identity_&&
                model_->historyStatus().state==CandleHistoryState::Ready)model_->requestOlder();});}
    };
    lowerPane_=new QWidget(chartColumn);
    lowerPane_->setObjectName("chartLowerPane");
    auto* lowerLayout=new QVBoxLayout(lowerPane_);
    lowerLayout->setContentsMargins(0,0,0,0);
    lowerLayout->setSpacing(4);
    // The workspace scrolls vertically when the chosen panes no longer fit.
    // Keep each plot readable and let the price chart retain its minimum height.
    for (const auto& id : supported.supportedIds("coin")) {
        if (!ChartStudyPreferences::isPane(id)) continue;
        auto* pane=new LowerStudyPane(id,static_cast<CandleChart*>(chart_),lowerPane_);
        lowerStudies_.insert(id,pane);
        lowerLayout->addWidget(pane);
        pane->hide();
        connect(pane->hideButton,&QPushButton::clicked,this,[this,id]{
            if (indicatorPreferences_) indicatorPreferences_->setVisible(id,false);
        });
        connect(this,&MarketCandlesView::viewportChanged,pane->plot,qOverload<>(&QWidget::update));
    }
    chartColumnLayout->addWidget(lowerPane_);lowerPane_->hide();
    futuresLedger_=new QWidget(chartColumn);futuresLedger_->setObjectName("chartFuturesLedger");auto* futureLayout=new QHBoxLayout(futuresLedger_);futureLayout->setContentsMargins(0,0,0,0);futuresLedgerText_=new QLabel(futuresLedger_);futuresLedgerText_->setMinimumWidth(0);futuresLedgerText_->setTextFormat(Qt::RichText);futuresLedgerText_->setWordWrap(false);futureLayout->addWidget(futuresLedgerText_,1);auto* futureInfo=new InfoButton(futuresLedger_);futureInfo->setObjectName("chartFuturesInformation");futureInfo->setFixedSize(44,44);futureLayout->addWidget(futureInfo);liquidationRecords_=new QPushButton(tr("청산 관측"),futuresLedger_);liquidationRecords_->setObjectName("chartLiquidationRecordsButton");liquidationRecords_->setMinimumSize(44,44);futureLayout->addWidget(liquidationRecords_);connect(liquidationRecords_,&QPushButton::clicked,this,&MarketCandlesView::showLiquidationRecords);chartColumnLayout->addWidget(futuresLedger_);futuresLedger_->hide();plotLayout->addWidget(chartColumn,1);
    layout->addWidget(plotBody,1);
    drawingStatus_=label("chartDrawingStatus",FontRole::Caption12);drawingStatus_->setProperty("type","caption");
    drawingFeedbackTimer_=new QTimer(this);drawingFeedbackTimer_->setSingleShot(true);drawingFeedbackTimer_->setInterval(4000);
    connect(drawingFeedbackTimer_,&QTimer::timeout,this,&MarketCandlesView::clearDrawingFeedback);
    pullbackEvidence_=new StudyLegendLabel(this);pullbackEvidence_->setObjectName("chartStudyPullbackEvidence");pullbackEvidence_->setWordWrap(true);pullbackEvidence_->setTextFormat(Qt::PlainText);pullbackEvidence_->setProperty("accent",true);layout->addWidget(pullbackEvidence_);pullbackEvidence_->hide();
    static_cast<CandleChart*>(chart_)->drawingUiChanged=[this]{refreshDrawingUi();};
    static_cast<CandleChart*>(chart_)->drawingFeedback=[this](const QString& text){drawingStatus_->setText(text);};
    static_cast<CandleChart*>(chart_)->autoPreviewCancelled=[this]{clearAutoPreviewPanel();};
    drawingObjectsPanel_=new QFrame(this);drawingObjectsPanel_->setObjectName("chartDrawingObjectsPanel");drawingObjectsPanel_->setFocusPolicy(Qt::NoFocus);drawingObjectsPanel_->installEventFilter(this);
    setThemeStyle(drawingObjectsPanel_,"#chartDrawingObjectsPanel { background:${bg}; border:1px solid ${rule}; border-radius:6px; } #chartDrawingObjectsPanel QCheckBox { color:${text}; spacing:8px; } #chartDrawingObjectsPanel QCheckBox::indicator { width:14px; height:14px; border:1px solid ${text3}; border-radius:3px; background:${surface}; } #chartDrawingObjectsPanel QCheckBox::indicator:checked { background:${accent}; border:2px solid ${bg}; } QToolButton { border:0; border-radius:4px; padding:0; background:transparent; } QToolButton:hover, QToolButton:checked { background:${surface}; }");
    auto* objectsRoot=new QVBoxLayout(drawingObjectsPanel_);objectsRoot->setContentsMargins(8,8,8,8);objectsRoot->setSpacing(6);
    drawingObjectsScroll_=new QScrollArea(drawingObjectsPanel_);drawingObjectsScroll_->setObjectName("chartDrawingObjectsScroll");drawingObjectsScroll_->setWidgetResizable(true);drawingObjectsScroll_->setFrameShape(QFrame::NoFrame);drawingObjectsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);drawingObjectsScroll_->setFocusPolicy(Qt::NoFocus);
    auto* scrollContent=new QWidget(drawingObjectsScroll_);scrollContent->setFocusPolicy(Qt::NoFocus);
    auto* scrollLayout=new QVBoxLayout(scrollContent);scrollLayout->setContentsMargins(0,0,0,0);scrollLayout->setSpacing(6);
    drawingPreviewGroup_=new QWidget(scrollContent);drawingPreviewGroup_->setObjectName("chartDrawingPreviewSection");drawingPreviewGroup_->setFocusPolicy(Qt::NoFocus);
    auto* previewOuter=new QVBoxLayout(drawingPreviewGroup_);previewOuter->setContentsMargins(0,0,0,0);previewOuter->setSpacing(4);
    auto* previewHeading=new QLabel(tr("미리보기"),drawingPreviewGroup_);previewHeading->setFont(KeefiFonts::font(FontRole::Caption12));setThemeStyle(previewHeading,"color:${text3};");previewOuter->addWidget(previewHeading);
    auto* previewRowsHost=new QWidget(drawingPreviewGroup_);previewRowsHost->setFocusPolicy(Qt::NoFocus);
    drawingPreviewLayout_=new QVBoxLayout(previewRowsHost);drawingPreviewLayout_->setContentsMargins(0,0,0,0);drawingPreviewLayout_->setSpacing(2);previewOuter->addWidget(previewRowsHost);
    drawingPreviewApply_=new QPushButton(tr("적용"),drawingPreviewGroup_);drawingPreviewApply_->setObjectName("chartDrawingAutoApply");applyCompactButton(drawingPreviewApply_);drawingPreviewApply_->installEventFilter(this);
    connect(drawingPreviewApply_,&QPushButton::clicked,this,&MarketCandlesView::applyAutoPreview);
    previewOuter->addWidget(drawingPreviewApply_);drawingPreviewGroup_->hide();scrollLayout->addWidget(drawingPreviewGroup_);
    drawingSavedGroup_=new QWidget(scrollContent);drawingSavedGroup_->setObjectName("chartDrawingSavedSection");drawingSavedGroup_->setFocusPolicy(Qt::NoFocus);
    auto* savedOuter=new QVBoxLayout(drawingSavedGroup_);savedOuter->setContentsMargins(0,0,0,0);savedOuter->setSpacing(2);
    drawingSavedLayout_=new QVBoxLayout;drawingSavedLayout_->setContentsMargins(0,0,0,0);drawingSavedLayout_->setSpacing(2);savedOuter->addLayout(drawingSavedLayout_);
    drawingObjectsEmpty_=new QLabel(tr("저장된 도형 없음"),drawingSavedGroup_);drawingObjectsEmpty_->setFont(KeefiFonts::font(FontRole::Caption12));setThemeStyle(drawingObjectsEmpty_,"color:${text3};");savedOuter->addWidget(drawingObjectsEmpty_);
    scrollLayout->addWidget(drawingSavedGroup_);scrollLayout->addStretch(1);
    drawingObjectsScroll_->setWidget(scrollContent);objectsRoot->addWidget(drawingObjectsScroll_);drawingObjectsPanel_->hide();
    drawingLinePanel_=new QWidget(this);drawingLinePanel_->setObjectName("chartDrawingLineAlert");auto* lineLayout=new QGridLayout(drawingLinePanel_);lineLayout->setContentsMargins(0,0,0,0);lineLayout->setSpacing(6);
    drawingLineStatus_=new LedgerTextLabel(FontRole::Body15,drawingLinePanel_);drawingLineStatus_->setObjectName("chartDrawingLineStatus");drawingLineStatus_->setWordWrap(true);drawingLineStatus_->setTextFormat(Qt::PlainText);lineLayout->addWidget(drawingLineStatus_,0,0,1,2);
    drawingLineEnabled_=new QCheckBox(tr("이 선 닿으면 알림"),drawingLinePanel_);drawingLineEnabled_->setObjectName("chartDrawingLineEnabled");lineLayout->addWidget(drawingLineEnabled_,1,0);
    drawingLineInterval_=new LocalComboBox(drawingLinePanel_);drawingLineInterval_->setObjectName("chartDrawingLineInterval");drawingLineInterval_->setAccessibleName(tr("기준선 알림 시간봉"));
    for(const qint64 interval:{60000,300000,900000,3600000,14400000,86400000})drawingLineInterval_->addItem(timeframeLabel(interval),interval);lineLayout->addWidget(drawingLineInterval_,1,1);
    drawingLineOwner_=new LocalComboBox(drawingLinePanel_);drawingLineOwner_->setObjectName("chartDrawingLineOwner");drawingLineOwner_->setAccessibleName(tr("기준선 알림을 맡는 공개 리스트"));drawingLineOwner_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);drawingLineOwner_->setMinimumContentsLength(10);drawingLineOwner_->setMinimumWidth(0);lineLayout->addWidget(drawingLineOwner_,2,0,1,2);
    drawingLineSettings_=new QPushButton(tr("리스트 알림 설정"),drawingLinePanel_);drawingLineSettings_->setObjectName("chartDrawingLineSettings");lineLayout->addWidget(drawingLineSettings_,3,0);
    drawingLineDelete_=new QPushButton(tr("선 지우기"),drawingLinePanel_);drawingLineDelete_->setObjectName("chartDrawingLineDelete");lineLayout->addWidget(drawingLineDelete_,3,1);
    connect(drawingLineEnabled_,&QCheckBox::toggled,this,[this]{saveDrawingAlert();});connect(drawingLineOwner_,&QComboBox::currentIndexChanged,this,[this]{saveDrawingAlert();});connect(drawingLineInterval_,&QComboBox::currentIndexChanged,this,[this]{saveDrawingAlert();});
    connect(drawingLineSettings_,&QPushButton::clicked,this,[this]{const auto id=drawingLineOwner_->currentData().toString();if(!id.isEmpty())emit drawingAlertSettingsRequested(id);});
    connect(drawingLineDelete_,&QPushButton::clicked,this,[this]{if(drawingModel_)drawingModel_->remove(static_cast<CandleChart*>(chart_)->selectedDrawing());});
    layout->addWidget(drawingLinePanel_);drawingLinePanel_->hide();layoutDrawingTools();
    label("marketCandlesPositionLines");
    similarityEvidence_=label("chartStudySimilarityEvidence",FontRole::Caption12);similarityEvidence_->setProperty("type","caption");similarityEvidence_->hide();
    coinFlowDetails_=new QLabel(this);coinFlowDetails_->setObjectName("chartCoinFlowDetails");coinFlowDetails_->setFont(KeefiFonts::font(FontRole::Caption12));coinFlowDetails_->setTextFormat(Qt::RichText);coinFlowDetails_->setWordWrap(true);layout->addWidget(coinFlowDetails_);
    coinFlowDetails_->setProperty("type","caption");coinFlowDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);coinFlowDetails_->hide();
    layout->removeWidget(coinFlowDetails_);auto* flowScroll=new QScrollArea(this);flowScroll->setObjectName("chartCoinFlowDetailScroll");flowScroll->setWidgetResizable(true);flowScroll->setFrameShape(QFrame::NoFrame);
    flowScroll->setMinimumHeight(100);flowScroll->setMaximumHeight(220);flowScroll->setWidget(coinFlowDetails_);coinFlowDetailsPane_=flowScroll;layout->addWidget(flowScroll);flowScroll->hide();
    coinFlowScorecard_=label("chartCoinFlowScorecard",FontRole::Caption12);coinFlowScorecard_->setProperty("type","caption");coinFlowScorecard_->hide();
    study_=label("marketCandlesStudy");
    label("marketCandlesStudyState");
    auto* definition=label("marketCandlesDefinition",FontRole::Caption12);definition->setText(tr("변동성 압축: BB(20, 2σ)가 KC(EMA20, Wilder ATR20 × 1.5) 안에 있는 상태. 방향을 예측하지 않습니다. 초록은 종가≥시가, 빨강은 종가<시가입니다. 완료 봉과 형성 봉을 구분해 표시하고 지표는 완료 봉만 사용하며 빈 시간은 채우지 않습니다."));
    definition->hide();
    auto* calculations=label("marketCandlesStudyDetails");calculations->hide();

    definition->setProperty("type","caption");
    coinFlowRunner_=new CoinFlowStudyRunner(this);
    coinFlowRefresh_=new QTimer(this);coinFlowRefresh_->setSingleShot(true);coinFlowRefresh_->setInterval(250);
    connect(coinFlowRefresh_,&QTimer::timeout,this,&MarketCandlesView::refreshCoinFlow);
    connect(coinFlowRunner_,&CoinFlowStudyRunner::resultReady,this,[this](const QString& context,const CoinFlowStudySet& result,qint64 ns){
        if(context!=coinFlowContext_||result.identity!=identity_||!isVisible())return;
        QElapsedTimer applyTimer;applyTimer.start();
        coinFlowSnapshot_=result;setProperty("coinFlowWorkerNs",ns);static_cast<CandleChart*>(chart_)->setCoinFlowStudies(result);
        renderCoinFlow();emit coinFlowStudiesChanged();setProperty("coinFlowUiApplyNs",applyTimer.nsecsElapsed());
    });
    connect(coinFlowRunner_,&CoinFlowStudyRunner::observationReady,this,[this](const CoinFlowSnapshot& input,const QString& context,const CoinFlowStudySet& core,const ExtendedStrongReversal& strong){if(context==coinFlowContext_&&isVisible()&&input.identity==identity_)emit publicFlowObservationReady(input,core,strong);});
    connect(coinFlowRunner_,&CoinFlowStudyRunner::extendedResultReady,this,[this](const QString& context,const ExtendedAmountDelta& delta,const ExtendedLowVolumeZones& zones,const ExtendedStrongReversal& strong){
        if(context!=coinFlowContext_||!isVisible())return;deltaSnapshot_=delta;strongSnapshot_=strong;tradeZonesSnapshot_=zones;refreshIndicatorUi();emit coinFlowStudiesChanged();
    });
    if(model_){
        connect(model_,&MarketCandles::changed,this,&MarketCandlesView::refresh);
        connect(model_,&MarketCandles::liveChanged,this,&MarketCandlesView::refreshLiveObservation);
        connect(model_,&QObject::destroyed,this,[this]{model_=nullptr;refreshLiveObservation();});
        connect(model_,&MarketCandles::archiveStatusChanged,this,&MarketCandlesView::refreshInformation);
        connect(model_,&MarketCandles::historyChanged,this,&MarketCandlesView::refresh);
    }
    bindThemePresentation(this,[this]{refreshIndicatorUi();refreshDrawingUi();});
    auto* timer=new QTimer(this);timer->setInterval(1000);connect(timer,&QTimer::timeout,this,[this]{if(isVisible())refresh();});timer->start();refresh();
}
MarketCandlesView::~MarketCandlesView(){
    if(drawingModel_)disconnect(drawingModel_,nullptr,this,nullptr);
    if(coinFlowSource_)disconnect(coinFlowSource_,nullptr,this,nullptr);
    if(indicatorPreferences_)disconnect(indicatorPreferences_,nullptr,this,nullptr);
    if(stockSource_){disconnect(stockSource_,nullptr,this,nullptr);stockSource_->setActive(false);}
    if(model_){disconnect(model_,nullptr,this,nullptr);model_->setSelection({},intervalMs_);}
}
void MarketCandlesView::setHeaderControls(QWidget* controls){
    if(controls)static_cast<QVBoxLayout*>(layout())->insertWidget(1,controls);
}
void MarketCandlesView::setTitleHost(QWidget* host){
    if(!title_||!headingLayout_||!host||host==this||title_->parentWidget()!=this)return;
    auto* target=qobject_cast<QBoxLayout*>(host->layout());if(!target)return;
    headingLayout_->removeWidget(title_);headingLayout_->insertStretch(0,1);
    target->addWidget(title_);title_->show();
}
void MarketCandlesView::setToolbarHosts(QWidget* instrumentHost,QWidget* controlsHost,QWidget* menuAnchor){
    if(!instrumentHost||!controlsHost||!headingLayout_)return;
    auto* instrument=qobject_cast<QBoxLayout*>(instrumentHost->layout());
    auto* controls=qobject_cast<QBoxLayout*>(controlsHost->layout());
    if(!instrument||!controls)return;
    toolbarMenuAnchor_=menuAnchor;
    setTitleHost(instrumentHost);
    for(auto* widget:{static_cast<QWidget*>(information_.data()),static_cast<QWidget*>(indicatorPicker_),static_cast<QWidget*>(indicatorApplied_),static_cast<QWidget*>(chartStyleButton_)}){
        headingLayout_->removeWidget(widget);
        (widget==information_?instrument:controls)->addWidget(widget);widget->show();
    }
    // Remove the now empty heading, including the title's old stretch item.
    while(auto* item=headingLayout_->takeAt(0))delete item;
    layout()->removeItem(headingLayout_);delete headingLayout_;headingLayout_=nullptr;
    title_->setProperty("elideRight",true);title_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
    title_->setMinimumWidth(48);title_->setMaximumWidth(240);
}
void MarketCandlesView::setRelatedMarketInformation(const QString& text){
    if(relatedMarketInformation_==text)return;
    relatedMarketInformation_=text;refreshInformation();
}
bool MarketCandlesView::navigateToTime(qint64 at){return static_cast<CandleChart*>(chart_)->navigateToTime(at);}
void MarketCandlesView::returnToLive(){static_cast<CandleChart*>(chart_)->returnToLive();}
ChartShareViewport MarketCandlesView::visibleViewport()const{return static_cast<CandleChart*>(chart_)->shareViewport();}
ChartSyncViewport MarketCandlesView::syncViewport()const{return static_cast<CandleChart*>(chart_)->syncViewport();}
bool MarketCandlesView::followsLive()const{return static_cast<CandleChart*>(chart_)->followsLive();}
int MarketCandlesView::nominalZoomBars()const{return static_cast<CandleChart*>(chart_)->nominalZoomBars();}
void MarketCandlesView::restoreLiveViewport(int zoomBars){static_cast<CandleChart*>(chart_)->restoreLiveViewport(zoomBars);}
void MarketCandlesView::applySyncedViewport(const ChartSyncViewport& range,bool exact){static_cast<CandleChart*>(chart_)->applySyncedViewport(range,exact);}
void MarketCandlesView::setSyncedCrosshair(qint64 utc){static_cast<CandleChart*>(chart_)->setSyncedCrosshair(utc);}
void MarketCandlesView::refreshHistoryNavigation(){
    if(!historyStatus_||!chart_)return;
    const bool coin=identity_.startsWith("coin:");const auto status=coin&&model_?model_->historyStatus():CandleHistoryStatus{};
    QString text=status.message;
    if(coin&&status.completedCount>0&&text.isEmpty())text=tr("완료 %1봉 · 가로 이동으로 이전 구간 탐색").arg(status.completedCount);
    if(status.retryAtMs>QDateTime::currentMSecsSinceEpoch()&&status.state==CandleHistoryState::RateLimited)
        text+=tr(" · %1 이후").arg(sourceTime(status.retryAtMs));
    historyStatus_->setText(text);historyStatus_->parentWidget()->setVisible(coin||!chart_->property("viewportLive").toBool());
    const bool retry=coin&&(status.state==CandleHistoryState::Delayed||status.state==CandleHistoryState::RateLimited);
    historyRetry_->setVisible(retry);historyRetry_->setEnabled(retry&&status.retryAtMs<=QDateTime::currentMSecsSinceEpoch());
    liveReturn_->setEnabled(!chart_->property("viewportLive").toBool());
}
void MarketCandlesView::setAsset(const QString& asset){setSelection(asset.isEmpty()?QString{}:"coin:upbit:KRW-"+asset,MarketCandles::IntervalMs);}
void MarketCandlesView::setIndicatorPreferences(ChartStudyPreferences* preferences){
    if(indicatorPreferences_==preferences)return;
    if(indicatorPreferences_)disconnect(indicatorPreferences_,nullptr,this,nullptr);
    indicatorPreferences_=preferences;introDisplayed_=false;introDismissed_=false;indicatorIntro_->hide();
    if(preferences){
        connect(preferences,&ChartStudyPreferences::changed,this,[this]{refresh();emit studiesChanged();});
        connect(preferences,&QObject::destroyed,this,[this]{indicatorPreferences_=nullptr;refreshIndicatorUi();});
    }
    refreshIndicatorUi();maybeShowIndicatorIntro();
}
ChartStudySet MarketCandlesView::chartStudies()const{return studySnapshot_;}
ExtendedChartStudySet MarketCandlesView::extendedStudies()const{return extendedSnapshot_;}
QString MarketCandlesView::indicatorContext()const{return identity_+":"+QString::number(intervalMs_);}
quint64 MarketCandlesView::indicatorRevision()const{return studyGeneration_;}
int MarketCandlesView::pricePlotHeight()const{return static_cast<CandleChart*>(chart_)->pricePlotHeight();}
QList<ChartIndicatorState> MarketCandlesView::indicatorStates()const{
    QList<ChartIndicatorState> result;if(!indicatorPreferences_)return result;
    const bool stock=identity_.startsWith("stock:");const auto market=stock?QString("stock"):QString("coin");
    const auto& oscillators=stock?displayExtendedSnapshot_:extendedSnapshot_;
    const bool hasDisplayOnlyCloses=stock && std::any_of(stockSnapshot_.timeline.rows.cbegin(),stockSnapshot_.timeline.rows.cend(),
        [](const auto& row){return !row.eligible && row.closeDisplayEligible();});
    const auto& cloud=stock?stockDisplayCloud_:studySnapshot_.cloud;
    const auto cloudCurrent=[&](const auto& points){return !points.isEmpty()&&(!stock||(!stockSnapshot_.timeline.rows.isEmpty()&&points.last().atMs==stockSnapshot_.timeline.rows.last().startsAtMs));};
    const auto direction=[](double x){return x>0?IndicatorDirection::Up:x<0?IndicatorDirection::Down:IndicatorDirection::Info;};
    const auto priceGroup=QStringLiteral("가격·거래량");const auto tradeGroup=identity_.section(':',1,1)+tr(" 현물 체결");
    for(const auto& id:indicatorPreferences_->supportedIds(market)){
        ChartIndicatorState state;state.id=id;state.context=indicatorContext();state.revision=indicatorRevision();state.enabled=indicatorPreferences_->enabled(id);state.hidden=!indicatorPreferences_->visible(id);state.label=studyName(id);
        state.stateBearing=chartIndicatorContributesCurrentState(id);ExtendedStudyState value;QString text;QString group=priceGroup;
        const auto info=[&](bool ready,const QString& reason=QString{}){return ExtendedStudyState{ready,ready?IndicatorDirection::Info:IndicatorDirection::None,reason};};
        if(id=="cloud"){value=info(cloudCurrent(cloud),tr("연속 완료 봉 26개 필요"));if(value.available){const auto p=cloud.last();value.direction=direction(p.fast-p.slow);text=p.fast==p.slow?tr("두 평균 같음"):p.fast>p.slow?tr("빠른선 위"):tr("빠른선 아래");}}
        else if(id=="pullback"){const auto& p=studySnapshot_.pullback;value=info(p.status!=ChartStudyStatus::Insufficient,p.reason);if(p.available)text=p.inBox?tr("범위 안 · %1/10").arg(p.score):tr("범위 밖 · %1/10").arg(p.score);else if(p.status==ChartStudyStatus::NoSignal)text=tr("조건 없음");}
        else if(id=="similarity"){const auto& comparison=studySnapshot_.similarity;value=info(comparison.status!=ChartStudyStatus::Insufficient,comparison.reason);if(value.available)text=tr("과거 일치 %1건").arg(comparison.samples);}
        else if(id=="ma"){
            value=displayExtendedSnapshot_.movingAverageState;
            if(!value.available){
                const auto& lines=displayExtendedSnapshot_.maLines;
                const auto count=std::count_if(lines.cbegin(),lines.cend(),[](const auto& line){return line.state.available;});
                if(count){
                    state.dataDetail=IndicatorDataDetail::Partial;
                    text=tr("이동평균 %1/%2개 표시 가능").arg(count).arg(lines.size());
                }
            }
        }
        else if(id=="bb")value=displayExtendedSnapshot_.bb.state;
        else if(id=="rsi"){value=oscillators.rsi.state;if(value.available&&!oscillators.rsi.points.isEmpty())text=QString::number(oscillators.rsi.points.last().price,'f',2);}
        else if(id=="macd")value=oscillators.macd.state;
        else if(id=="breakout"){value=extendedSnapshot_.breakout.state;text=value.available&&value.direction==IndicatorDirection::Info?tr("현재 돌파 없음"):QString{};}
        else if(id=="low_volume"){const auto& zones=tradeZonesSnapshot_.state.available?tradeZonesSnapshot_:(minuteZonesSnapshot_.state.available||(intervalMs_!=60000&&!minuteZonesSnapshot_.state.reason.isEmpty()))?minuteZonesSnapshot_:extendedSnapshot_.lowVolumeZones;value=zones.state;text=value.available?(zones.candleApproximation?tr("캔들 근사"):tr("관측 체결"))+tr(" · %1구간").arg(zones.zones.size()):QString{};if(!zones.candleApproximation)group=tradeGroup;if(value.available&&zones.zones.isEmpty())state.dataDetail=IndicatorDataDetail::NoSignal;}
        else if(id=="trend"){
            QStringList facts,missing;
            int observed=0;
            for(const auto& h:higherTrends_){
                const auto interval=timeframeLabel(h.intervalMs);
                state.atoms.append({priceGroup,h.state.direction,h.state.available,h.state.reason});
                if(h.state.available)++observed;
                else if(!h.state.reason.isEmpty())missing<<interval+" "+usabilityPublicText(h.state.reason);
                facts<<interval+" "+(h.state.available?indicatorDirectionText(h.state.direction):tr("자료 없음"));
            }
            if(state.atoms.isEmpty()){
                state.atoms.append({priceGroup,IndicatorDirection::None,false,tr("완료 시간봉 기록 필요")});
                missing<<tr("완료 시간봉 기록 필요");
            }
            state.requiredReady=observed>0&&observed==higherTrends_.size();
            if(observed&&!state.requiredReady)state.dataDetail=IndicatorDataDetail::Partial;
            state.status=facts.isEmpty()?tr("자료 없음"):facts.join(" · ");
            state.reason=missing.join(" · ");
            result.append(state);continue;
        }
        else if(id=="relative_strength"){group=tr("BTC 상대강도");value=relativeSnapshot_.state;if(value.available&&relativeSnapshot_.percent)text=QString::number(*relativeSnapshot_.percent,'f',2)+tr("% · 동일 24시간");}
        else if(id=="levels"){value=levelsSnapshot_.state;if(value.available){text=tr("완료 기간 %1선").arg(levelsSnapshot_.visible.size());if(levelsSnapshot_.visible.isEmpty())state.dataDetail=IndicatorDataDetail::NoSignal;}}
        else if(id=="absorption"){value=info(explanationSnapshot_.available&&explanationSnapshot_.observations>=22,tr("연속 완료 봉 22개 필요"));if(value.available)text=explanationSnapshot_.narrowHighVolume?tr("좁은 변동·거래량 증가"):tr("현재 조건 미충족");}
        else if(id=="sweep"){value=info(explanationSnapshot_.available&&explanationSnapshot_.observations>=21,explanationSnapshot_.reason);if(value.available)value.direction=explanationSnapshot_.sweptHigh&&explanationSnapshot_.sweptLow?IndicatorDirection::Mix:explanationSnapshot_.sweptHigh?IndicatorDirection::Down:explanationSnapshot_.sweptLow?IndicatorDirection::Up:IndicatorDirection::Info;}
        else if(id=="squeeze"){value=info(compressionSnapshot_.available,compressionSnapshot_.reason);if(value.available)text=compressionSnapshot_.compressed?tr("압축 중"):tr("압축 아님");}
        else if(id=="adx"){const auto& adx=stock?stockDisplayAdx_:explanationSnapshot_;value=info(adx.adxAvailable,tr("연속 완료 봉 28개 필요"));if(value.available)text=QString::number(adx.adx,'f',2)+" · "+adx.regime;}
        else if(id=="whales"){
            const auto& flow=coinFlowSnapshot_;const auto display=coinFlowDisplayState(flow);const bool price=flow.reversal.state.available&&flow.volume.state.available,trade=flow.largeTrade.state.available&&flow.taker.state.available&&flow.taker.signedFraction.has_value();
            state.requiredReady=display.requiredReady;state.atoms={{priceGroup,price?(flow.reversal.rebound?IndicatorDirection::Up:flow.reversal.turnDown?IndicatorDirection::Down:IndicatorDirection::Info):IndicatorDirection::None,price,flow.reversal.state.reason},
                {tradeGroup,trade?direction(*flow.taker.signedFraction):IndicatorDirection::None,trade,flow.taker.state.reason}};
            if(display.phase==CoinFlowDisplayPhase::ObservedNoSignal)state.dataDetail=IndicatorDataDetail::NoSignal;
            else if(!display.requiredReady&&(price||trade||display.currentSignals))state.dataDetail=IndicatorDataDetail::Partial;
            state.status=display.phase==CoinFlowDisplayPhase::Signal?tr("신호 발생"):display.phase==CoinFlowDisplayPhase::ObservedNoSignal?tr("관측 완료 · 신호 없음"):tr("체결 수집 중");state.reason=display.reason;result.append(state);continue;
        }
        else if(id=="cvd"){group=tradeGroup;value=deltaSnapshot_.state;text=value.available?tr("금액 구간 3개 · %1").arg(deltaSnapshot_.currency):QString{};}
        else if(id=="strong"){group=priceGroup;value=strongSnapshot_.state;text=value.available?(strongSnapshot_.grade==ExtendedReversalGrade::Strong?tr("강함 · 원 반전 보강"):strongSnapshot_.grade==ExtendedReversalGrade::Reinforced?tr("보강 · 원 반전 설명"):strongSnapshot_.grade==ExtendedReversalGrade::Basic?tr("기본 · 원 반전 설명"):tr("현재 반전 없음")):QString{};}
        else if(id=="oi"){group=tr("계약수량");value=info(publicContextSnapshot_.oiChange.available,publicContextSnapshot_.oiChange.reason);if(value.available)text=publicContextSnapshot_.oiChange.value+"% · 1시간";}
        else if(id=="contract"){group=tr("계약수량");const auto& price=publicContextSnapshot_.priceChange;const auto& oi=publicContextSnapshot_.oiChange;
            value=info(price.available&&oi.available&&price.fromMs==oi.fromMs&&price.throughMs==oi.throughMs,tr("동일 계약·같은 시간의 가격과 OI 필요"));
            if(value.available){value.direction=oi.value.toDouble()>0?direction(price.value.toDouble()):IndicatorDirection::Info;text=tr("가격 %1% · OI %2% · 1시간").arg(price.value,oi.value);}}

        else if(id=="stock_flow"){
            group=tr("국내 일별 수급");
            const auto* day=latestStockFlowDay(identity_,stockSnapshot_);
            value=info(day,identity_.startsWith("stock:KR:")?tr("확인된 일별 수급 기록 없음"):tr("국내 주식의 일별 수급 자료 필요"));
            if(day){
                bool up=false,down=false;
                int reported=0;
                for(const auto& n:{day->foreignerNetBuyVolume,day->institutionNetBuyVolume,day->arbitrageNetBuyVolume,day->nonArbitrageNetBuyVolume}){
                    if(!n)continue;
                    ++reported;up|=n->toDouble()>0;down|=n->toDouble()<0;
                }
                value.direction=up&&down?IndicatorDirection::Mix:up?IndicatorDirection::Up:down?IndicatorDirection::Down:IndicatorDirection::Info;
                text=tr("일별 · 분류 합산 안 함");
                if(reported<4){state.dataDetail=IndicatorDataDetail::Partial;text+=tr(" · %1/4개 분류").arg(reported);}
            }
        }
        else if(id=="volume")value=info(!studySnapshot_.volumes.isEmpty());
        else if(id=="sentiment"){
            const auto& context=publicContextSnapshot_;
            int reported=0,expected=0;
            for(const auto* cell:{&context.funding,&context.topPosition,&context.takerBuy,&context.oiChange}){
                if(cell->hidden)continue;
                ++expected;
                if(cell->available)++reported;
            }
            value=info(context.futuresSupported&&expected>0&&reported==expected,
                       context.futuresReason.isEmpty()?tr("선물 공개 관측값 수신 대기"):context.futuresReason);
            if(context.futuresSupported&&reported){
                text=tr("공개 관측 %1/%2").arg(reported).arg(expected);
                if(!value.available)state.dataDetail=IndicatorDataDetail::Partial;
            }
        }
        if(stock&&!value.available&&group==priceGroup&&id!="levels"&&id!="volume"&&(id!="low_volume"||intervalMs_==60000)){
            const bool priceOnly=id=="cloud"||id=="adx"||id=="ma"||id=="bb"||id=="rsi"||id=="macd";
            int window=id=="similarity"?60:id=="cloud"?26:id=="adx"?28:22;
            const auto& settings=indicatorPreferences_->studySettings();
            if(id=="ma")for(const auto& line:indicatorPreferences_->movingAverages()){if(line.enabled)window=qMax(window,line.period+1);}
            if(id=="bb")window=settings.displayBandPeriod+1;
            if(id=="rsi")window=settings.rsiPeriod+1;
            if(id=="macd")window=settings.macdSlow+settings.macdSignal-1;
            const auto reason=stockSnapshot_.timeline.recentExclusionReason(window,priceOnly);
            if(!reason.isEmpty()&&!value.reason.contains(reason))value.reason=value.reason.isEmpty()?reason:reason+" · "+value.reason;
        }
        if(stock&&group==priceGroup&&!stockSnapshot_.series.available)value=info(false,stockUnavailableReason());
        if(stock&&stockSnapshot_.delayed&&group==priceGroup){value=info(false,tr("지연 · 최근 완료 기록 확인 필요"));state.dataDetail=IndicatorDataDetail::Normal;text.clear();}
        if(value.available){
            value.reason.clear();
            if((id=="pullback"&&studySnapshot_.pullback.status==ChartStudyStatus::NoSignal)
                ||(id=="similarity"&&studySnapshot_.similarity.status==ChartStudyStatus::NoSignal)
                ||(id=="absorption"&&!explanationSnapshot_.narrowHighVolume)
                ||(id=="sweep"&&!explanationSnapshot_.sweptHigh&&!explanationSnapshot_.sweptLow)
                ||(id=="squeeze"&&!compressionSnapshot_.compressed)
                ||(id=="breakout"&&value.direction==IndicatorDirection::Info)
                ||(id=="strong"&&strongSnapshot_.grade==ExtendedReversalGrade::None))
                state.dataDetail=IndicatorDataDetail::NoSignal;
        }
        auto assessment=value;
        if(hasDisplayOnlyCloses&&!stockSnapshot_.delayed){
            if(id=="cloud"){
                assessment=info(cloudCurrent(studySnapshot_.cloud),stockSnapshot_.timeline.recentExclusionReason(26));
                if(assessment.available){const auto& point=studySnapshot_.cloud.last();assessment.direction=direction(point.fast-point.slow);}
            }
            else if(id=="adx")assessment=info(explanationSnapshot_.adxAvailable,stockSnapshot_.timeline.recentExclusionReason(28));
            else if(id=="ma") assessment=extendedSnapshot_.movingAverageState;
            else if(id=="bb") assessment=extendedSnapshot_.bb.state;
            else if(id=="rsi") assessment=extendedSnapshot_.rsi.state;
            else if(id=="macd") assessment=extendedSnapshot_.macd.state;
        }
        state.requiredReady=value.available;
        state.atoms={{group,assessment.available?assessment.direction:IndicatorDirection::None,assessment.available,assessment.reason}};
        state.status=(value.available||state.dataDetail==IndicatorDataDetail::Partial)?(text.isEmpty()?indicatorDirectionText(value.direction):text):(id=="pullback"?tr("자료 부족"):id=="similarity"?tr("비교 이력 부족"):tr("자료 없음"));state.reason=value.available?QString{}:(value.reason.isEmpty()?tr("기록 부족"):usabilityPublicText(value.reason));result.append(state);
    }return result;
}
void MarketCandlesView::setSupplementalStudies(const QList<ExtendedTimedDirection>& higher,const ExtendedLowVolumeZones& minute,const ExtendedRelativeStrength& relative){
    QByteArray key;QDataStream stream(&key,QIODevice::WriteOnly);for(const auto& h:higher)stream<<h.completesAtMs<<h.knownAtMs<<h.intervalMs<<h.state.available<<int(h.state.direction)<<h.state.reason;
    stream<<minute.state.available<<minute.state.reason<<minute.observations;for(const auto& zone:minute.zones)stream<<zone.low<<zone.high;
    stream<<relative.state.available<<relative.fromMs<<relative.throughMs<<relative.percent.value_or(0)<<relative.state.reason;
    if(key==supplementalKey_)return;supplementalKey_=key;higherTrends_=higher;minuteZonesSnapshot_=minute;relativeSnapshot_=relative;refresh();emit studiesChanged();
}
void MarketCandlesView::setScorecardStudy(const QString& id){if(scorecardStudy_==id)return;scorecardStudy_=id;refresh();}
void MarketCandlesView::setObservationStudies(const QSet<QString>& ids){if(observationStudies_==ids)return;observationStudies_=ids;refreshCoinFlow();}
void MarketCandlesView::showLiquidationRecords(){
    if(!publicContextSource_||!publicContextSnapshot_.futuresSupported)return;
    QDialog dialog(this);dialog.setObjectName("chartLiquidationRecordsDialog");dialog.setWindowTitle(tr("청산 관측 · 부분 기록"));dialog.resize(680,440);auto* layout=new QVBoxLayout(&dialog);
    auto* note=new QLabel(&dialog);note->setWordWrap(true);note->setFont(KeefiFonts::font(FontRole::Caption12));layout->addWidget(note);
    auto* filter=new QHBoxLayout;filter->addWidget(new QLabel(tr("최소 관측 금액 · USDT"),&dialog));auto* minimum=new QLineEdit(publicContextSnapshot_.liquidation.minimumQuote,&dialog);minimum->setObjectName("chartLiquidationMinimum");minimum->setMinimumHeight(44);minimum->setMaxLength(40);minimum->setFont(KeefiFonts::font(FontRole::Number11));filter->addWidget(minimum);auto* apply=new QPushButton(tr("적용"),&dialog);apply->setMinimumSize(44,44);filter->addWidget(apply);layout->addLayout(filter);
    auto* rows=new QTreeWidget(&dialog);rows->setObjectName("chartLiquidationRecords");rows->setRootIsDecorated(false);rows->setHeaderLabels({tr("시각"),tr("원문 side"),tr("체결 가격"),tr("체결 수량"),tr("개별 금액")});rows->setFont(KeefiFonts::font(FontRole::Number11));rows->header()->setFont(KeefiFonts::font(FontRole::Caption12));layout->addWidget(rows,1);
    auto* more=new QPushButton(tr("전체 관측 보기"),&dialog);more->setCheckable(true);more->setMinimumHeight(44);layout->addWidget(more);
    const auto refresh=[this,rows,note,more]{if(!publicContextSource_)return;const auto data=publicContextSource_->snapshot();const auto& feed=data.liquidation;rows->clear();note->setText(tr("%1 · 최근 1시간 · 실제 연결 %2분 · 부분 기록\n시장 전체 청산·주문 수·포지션 방향을 뜻하지 않으며 합산하지 않습니다.").arg(data.futuresSymbol).arg(feed.coverageMinutes)+(feed.rows.isEmpty()?tr("\n관측 기록 없음"):QString{}));
        const int count=more->isChecked()?feed.rows.size():qMin(5,feed.rows.size());for(int i=0;i<count;++i){const auto& row=feed.rows[i];const auto& value=row.observation;auto* item=new QTreeWidgetItem(rows);item->setText(0,localTime(value.tradeAtMs));item->setText(1,value.side);item->setText(2,priceAmount(value.averagePrice));item->setText(3,priceAmount(value.lastFilledQuantity));item->setText(4,priceAmount(row.quoteNotional)+" USDT");item->setSizeHint(0,QSize(0,44));item->setToolTip(0,tr("관측 시각 %1 · 원문 주문 side이며 롱/숏 추정 없음").arg(sourceTime(value.tradeAtMs)));}for(int c=0;c<5;++c)rows->resizeColumnToContents(c);more->setVisible(feed.rows.size()>5);};
    connect(more,&QPushButton::toggled,&dialog,[refresh](bool){refresh();});connect(apply,&QPushButton::clicked,&dialog,[this,minimum,refresh]{if(publicContextSource_)publicContextSource_->setLiquidationMinimum(minimum->text().trimmed());refresh();});connect(publicContextSource_,&ChartPublicContextSource::changed,&dialog,refresh);refresh();
    QDialogButtonBox buttons(QDialogButtonBox::Close,&dialog);buttons.button(QDialogButtonBox::Close)->setText(tr("닫기"));buttons.button(QDialogButtonBox::Close)->setMinimumHeight(44);layout->addWidget(&buttons);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();QTimer::singleShot(0,this,[this]{window()->activateWindow();liquidationRecords_->setFocus(Qt::OtherFocusReason);});
}
void MarketCandlesView::setPublicContextSource(ChartPublicContextSource* source){
    if(publicContextSource_==source)return;if(publicContextSource_)disconnect(publicContextSource_,nullptr,this,nullptr);publicContextSource_=source;publicContextSnapshot_={};levelsSnapshot_={};
    if(source){connect(source,&ChartPublicContextSource::changed,this,[this]{refreshPublicContext();refreshIndicatorUi();emit studiesChanged();});connect(source,&QObject::destroyed,this,[this]{publicContextSource_=nullptr;publicContextSnapshot_={};levelsSnapshot_={};refreshPublicContext();refreshIndicatorUi();emit studiesChanged();});}refreshPublicContext();refreshIndicatorUi();
}
void MarketCandlesView::refreshPublicContext(){
    const bool coin=identity_.startsWith("coin:");const bool levels=indicatorPreferences_&&indicatorPreferences_->enabled("levels");bool futures=false;
    if(indicatorPreferences_)for(const auto* id:{"sentiment","oi","contract"})futures|=indicatorPreferences_->enabled(id);
    if(publicContextSource_){publicContextSource_->setSelection(identity_,asset_,currency_);publicContextSource_->setDemand(isVisible()&&coin&&levels,isVisible()&&coin&&futures);publicContextSnapshot_=publicContextSource_->snapshot();}
    if(stockSource_)stockSource_->setLevelDemand(isVisible()&&!coin&&levels);
    if(publicContextSnapshot_.identity!=identity_)publicContextSnapshot_={};
    const auto data=coin?(model_?model_->series():MarketCandleSeries{}):stockSnapshot_.series;
    levelsSnapshot_=levels&&data.available&&!data.candles.isEmpty()?chartLevels(coin?publicContextSnapshot_.levelCandidates:stockSnapshot_.levelCandidates,data.candles.last().close,{},intervalMs_,QDateTime::currentMSecsSinceEpoch()):ExtendedLevelSet{};
    const auto reason=coin?publicContextSnapshot_.levelsReason:stockSnapshot_.levelsReason;
    if(!reason.isEmpty())levelsSnapshot_.state.reason=reason;
    static_cast<CandleChart*>(chart_)->setLevels(levelsSnapshot_);
    if(futuresLedger_){
        const auto* stockFlow=indicatorPreferences_&&indicatorPreferences_->enabled("stock_flow")&&indicatorPreferences_->visible("stock_flow")?latestStockFlowDay(identity_,stockSnapshot_):nullptr;
        const bool shown=coin&&indicatorPreferences_&&indicatorPreferences_->enabled("sentiment")&&indicatorPreferences_->visible("sentiment")&&publicContextSnapshot_.futuresSupported;
        futuresLedger_->setVisible(shown||stockFlow);if(liquidationRecords_){liquidationRecords_->setVisible(shown);liquidationRecords_->setText(tr("청산 관측 %1").arg(publicContextSnapshot_.liquidation.rows.size()));liquidationRecords_->setToolTip(tr("실제 최근 관측 · 부분 기록 · 전체 합계가 아님"));}
        if(stockFlow){const auto& day=*stockFlow;QStringList values;
            const auto add=[&](const QString& label,const std::optional<QString>& value){if(value)values<<label+" "+priceAmount(*value)+tr("주");};
            add(tr("외국인"),day.foreignerNetBuyVolume);add(tr("기관"),day.institutionNetBuyVolume);add(tr("차익"),day.arbitrageNetBuyVolume);add(tr("비차익"),day.nonArbitrageNetBuyVolume);
            futuresLedgerText_->setText(KeefiFonts::mixedTextHtml(tr("일별 수급 · ")+values.join(" · "),FontRole::Caption12));
            findChild<InfoButton*>("chartFuturesInformation")->setInformation(tr("일별 수급"),tr("거래일 %1 · 단위 주 · 순매수 수량\n분류별 원천 수량이며 서로 합산하지 않습니다. %2\n외국인·기관: %3\n%4\n프로그램: %5\n%6\n프로그램은 KRX만이며 NXT는 제외합니다.").arg(day.date.toString(Qt::ISODate),day.provisional?tr("잠정 값"):QString{},day.investorEvidence.source,day.investorEvidence.url,day.programEvidence.source,day.programEvidence.url));
            return;
        }
        if(!shown){futuresLedgerText_->clear();return;}
        QStringList values;QStringList evidence;const auto cell=[&](const QString& label,const ChartPublicCell& c){if(c.hidden)return;values<<label+" "+(c.available?c.value+c.unit:tr("받지 못했어요"));evidence<<label+" · "+c.reason+"\n"+c.sourceUrl+"\n"+sourceTime(c.fromMs)+" ~ "+sourceTime(c.throughMs);};
        cell(tr("펀딩"),publicContextSnapshot_.funding);cell(tr("상위 보유 롱숏"),publicContextSnapshot_.topPosition);cell(tr("매수 비중"),publicContextSnapshot_.takerBuy);cell(tr("OI"),publicContextSnapshot_.oiChange);
        futuresLedgerText_->setText(KeefiFonts::mixedTextHtml(values.join("  ·  "),FontRole::Caption12));findChild<InfoButton*>("chartFuturesInformation")->setInformation(tr("선물 공개 관측"),publicContextSnapshot_.futuresSymbol+"\n"+evidence.join("\n\n")+tr("\n청산은 부분 관측이며 전체 합계로 계산하지 않습니다."));
    }
}
void MarketCandlesView::setStockSource(StockChartSource* source){
    if(stockSource_==source)return;
    if(stockSource_){disconnect(stockSource_,nullptr,this,nullptr);stockSource_->setActive(false);}
    stockSource_=source;stockSnapshot_={};
    if(source){
        connect(source,&StockChartSource::changed,this,[this]{if(identity_.startsWith("stock:"))refresh();});
        connect(source,&QObject::destroyed,this,[this]{stockSource_=nullptr;stockSnapshot_={};if(identity_.startsWith("stock:"))refresh();});
        if(identity_.startsWith("stock:")){source->setSelection(WatchlistWorkspaceList::identityFromKey(identity_),intervalMs_,includeExtended_);source->setActive(isVisible());}
    }
    if(identity_.startsWith("stock:"))refresh();
}
void MarketCandlesView::setCoinFlowSource(CoinFlowSource* source){
    if(coinFlowSource_==source)return;if(coinFlowSource_)disconnect(coinFlowSource_,nullptr,this,nullptr);
    coinFlowRefresh_->stop();++flowGeneration_;coinFlowRunner_->invalidate();coinFlowSource_=source;coinFlowKey_.clear();
    if(source){connect(source,&CoinFlowSource::changed,this,[this]{
            if(!isVisible()||!identity_.startsWith("coin:"))return;
            if(!coinFlowSource_||!coinFlowSource_->active()){
                coinFlowRefresh_->stop();++flowGeneration_;coinFlowRunner_->invalidate();refreshCoinFlow();return;
            }
            // Raw trades remain in the shared collector. Bound only this UI's
            // snapshot/fingerprint/render fan-out, taking the latest state once.
            // Do not restart the deadline when another trade arrives.
            if(!coinFlowRefresh_->isActive())coinFlowRefresh_->start();
        });
        connect(source,&QObject::destroyed,this,[this]{coinFlowRefresh_->stop();++flowGeneration_;coinFlowRunner_->invalidate();coinFlowSource_=nullptr;coinFlowKey_.clear();refreshCoinFlow();});}
    refreshCoinFlow();
}
void MarketCandlesView::setInstrumentPresentation(ChartInstrumentPresentation* presentation){
    if(instrumentPresentation_==presentation)return;
    if(instrumentPresentation_)disconnect(instrumentPresentation_,nullptr,this,nullptr);
    instrumentPresentation_=presentation;
    if(presentation){connect(presentation,&ChartInstrumentPresentation::changed,this,&MarketCandlesView::refresh);
        connect(presentation,&QObject::destroyed,this,[this]{instrumentPresentation_=nullptr;refresh();});}
    refresh();
}
ChartInstrumentLabel MarketCandlesView::instrumentLabel()const{
    const auto identity=WatchlistWorkspaceList::identityFromKey(identity_);
    auto label=instrumentPresentation_?instrumentPresentation_->label(identity):ChartInstrumentPresentation::fallback(identity);
    // Standalone views also have the selected series' existing verified base/quote.
    if(identity.market=="coin"&&identity.source=="binance"&&label.code==identity.code&&model_){
        const auto series=model_->series();if(series.identity==identity_&&!series.asset.isEmpty()&&!series.currency.isEmpty())
            label.code=series.asset+"/"+series.currency;
    }
    return label;
}
CoinFlowStudySet MarketCandlesView::coinFlowStudies()const{return identity_.startsWith("coin:")?coinFlowSnapshot_:CoinFlowStudySet{};}
ExtendedStrongReversal MarketCandlesView::strongReversalSnapshot()const{return identity_.startsWith("coin:")&&isVisible()&&indicatorPreferences_&&indicatorPreferences_->enabled("strong")?strongSnapshot_:ExtendedStrongReversal{};}
void MarketCandlesView::refreshCoinFlow(){
    if(!coinFlowRunner_||!coinFlowDetails_)return;
    if(coinFlowRefresh_)coinFlowRefresh_->stop();
    const bool coin=identity_.startsWith("coin:");
    bool requested=!indicatorPreferences_||!observationStudies_.isEmpty();QSet<QString> enabled=observationStudies_;
    if(indicatorPreferences_)for(const auto* id:{"whales","cvd","oi","contract","strong","sentiment","low_volume"})if(indicatorPreferences_->enabled(id)){requested=true;enabled.insert(id);}
    coinFlowRunner_->setEnabled(coin&&requested&&isVisible());
    CoinFlowSnapshot input;const auto previousStatus=coinFlowStatus_;coinFlowStatus_.clear();
    if(coin&&coinFlowSource_){input=coinFlowSource_->snapshot();if(input.identity==identity_)coinFlowStatus_=coinFlowSource_->status();else input={};}
    auto sorted=enabled.values();sorted.sort();const auto context=identity_+":"+QString::number(intervalMs_)+":"+QString::number(flowGeneration_)+":"+sorted.join(",");
    auto key=flowFingerprint(input,coinFlowStatus_);key.prepend(context.toUtf8());
    const auto now=QDateTime::currentMSecsSinceEpoch();
    bool inputChanged=false,tradesOnly=false;
    if(context!=coinFlowContext_||key!=coinFlowKey_){
        // In one chart context new trades extend the verified window, and a budget trim only drops its
        // oldest minutes (later results are unchanged). Keep the last calculated result (it carries its
        // own as-of time) until the newer one arrives instead of blanking the markers on every 250 ms
        // refresh. A new context, an emptied or replaced window (overflow, reconnect) or lost
        // verification (metadata expiry) still clears at once.
        const auto& held=input.primary.trades;const auto& shown=coinFlowInput_.primary.trades;
        const bool extends=!held.isEmpty()&&(shown.isEmpty()||(held.first().tradeAtMs>=shown.first().tradeAtMs&&
            held.last().tradeAtMs>=shown.last().tradeAtMs&&held.first().tradeAtMs<=shown.last().tradeAtMs));
        const bool continuation=context==coinFlowContext_&&input.identity==identity_&&coinFlowInput_.identity==identity_&&
            input.primary.instrument.verified&&coinFlowInput_.primary.instrument.verified&&
            input.primary.instrument.identity==coinFlowInput_.primary.instrument.identity&&extends;
        coinFlowKey_=key;coinFlowInput_=input;inputChanged=true;tradesOnly=continuation&&coinFlowStatus_==previousStatus;
        if(!continuation){
            coinFlowContext_=context;setProperty("coinFlowContext",context);coinFlowSnapshot_={};deltaSnapshot_={};strongSnapshot_={};tradeZonesSnapshot_={};
            coinFlowSnapshot_.identity=coin?identity_:QString{};
            static_cast<CandleChart*>(chart_)->setCoinFlowStudies(coinFlowSnapshot_);emit coinFlowStudiesChanged();
        }
    }
    // The runner keeps only the newest input and paces work to once a second.
    if(coin&&requested&&isVisible()&&input.identity==identity_&&(inputChanged||!coinFlowSnapshot_.asOfMs||now-coinFlowSnapshot_.asOfMs>=1000))coinFlowRunner_->request(input,now,context,enabled);
    // New trades alone change nothing on screen until their calculation arrives (resultReady renders it).
    if(!tradesOnly)renderCoinFlow();
}
void MarketCandlesView::renderCoinFlow(){
    const bool coin=identity_.startsWith("coin:");const auto status=coinFlowStatus_;
    if(!coin){coinFlowDetails_->clear();coinFlowScorecard_->clear();coinFlowDetails_->hide();coinFlowDetailsPane_->hide();coinFlowScorecard_->hide();refreshIndicatorUi();return;}
    const auto instrument=instrumentLabel();
    const auto& study=coinFlowSnapshot_;QStringList lines{studyName("whales")+tr(" · 실제 완료 1분 · 공개 원천 관측 자료 · 현재 선택 %1").arg(instrument.text())};
    if(!status.isEmpty())lines<<status;
    const auto part=[&](const QString& name,const CoinFlowAvailability& state,const QString& value){
        lines<<tr("%1 · %2 · 표본 %3개 · %4").arg(name,state.available?(value.isEmpty()?tr("관측 조건 충족"):value):tr("판정 없음"),QString::number(state.samples),flowCoverage(state.coverage));
        if(!state.reason.isEmpty())lines<<state.reason;
        if(state.fromMs||state.throughMs)lines<<tr("실제 범위 %1 ~ %2 · 당시 알려진 시각 %3").arg(sourceTime(state.fromMs),sourceTime(state.throughMs),sourceTime(state.knownAtMs));
        for(const auto& e:state.evidence)lines<<tr("원천 %1 · %2\n원천 시각 %3 · 수신 %4 · 알려진 시각 %5").arg(e.source,e.sourceUrl,e.sourceTimeMs?sourceTime(e.sourceTimeMs):tr("미제공"),sourceTime(e.receivedAtMs),sourceTime(e.knownAtMs));
    };
    QString large;if(study.largeTrade.latestPercentile)large=tr("%1백분위 · 이전 %2집계 · 현재 체결은 분모 제외").arg(QString::number(*study.largeTrade.latestPercentile,'f',2)).arg(study.largeTrade.baselineSamples);
    part(tr("대량 체결"),study.largeTrade.state,large);
    const auto& taker=study.taker;QString fraction;if(taker.signedFraction)fraction=tr("관측 순방향 비율 %1% · 매수 명목 %2 / 매도 명목 %3").arg(QString::number(*taker.signedFraction*100,'f',2),taker.buyQuoteNotional,taker.sellQuoteNotional);
    part(taker.sourceDirectionOnly?tr("원천 BID/ASK 관측 방향 · 엄밀 taker 아님"):tr("Binance taker 쏠림"),taker.state,fraction);
    const auto& oi=study.openInterest;QString oiText;if(oi.quantityChangePercent)oiText=tr("OI 수량 변화 %1% · %2→%3 · END %4→%5").arg(QString::number(*oi.quantityChangePercent,'f',2),oi.previousQuantity,oi.currentQuantity,sourceTime(oi.previousEndMs),sourceTime(oi.currentEndMs));
    if(oi.valueChangePercent)oiText+=tr(" · 원문 value 변화 %1% (%2→%3), 통화 추정 없음").arg(QString::number(*oi.valueChangePercent,'f',2),oi.previousValue,oi.currentValue);
    part(tr("OI 5분 이력"),oi.state,oiText);
    const auto& volume=study.volume;part(tr("완료 1분 거래량"),volume.state,volume.multiple?tr("%1배 · 현재 %2 / 이전20봉 중앙값 %3").arg(QString::number(*volume.multiple,'f',2),volume.currentVolume,volume.previousMedian):QString{});
    part(tr("완료 4봉 감속·반전"),study.reversal.state,study.reversal.rebound?tr("반등 조건 · 매매 지시 아님"):study.reversal.turnDown?tr("꺾임 조건 · 매매 지시 아님"):tr("반전 조건 없음"));
    QString cvdText=tr("기준 자산 수량 · epoch마다 새 관측 기준 · 시장 전체 역사 누계 아님");
    if(!study.cvd.points.isEmpty())cvdText+=tr(" · 마지막 %1 · epoch %2").arg(study.cvd.points.last().signedBaseQuantity).arg(study.cvd.points.last().epoch);
    part(study.cvd.sourceDirectionOnly?tr("BID/ASK 관측 CVD"):tr("taker 관측 CVD"),study.cvd.state,cvdText);
    const auto& comparison=study.comparison;QString compared;
    if(comparison.state.available){compared=tr("현물 단일 taker 주문 집계 %1개 / 선물 100ms 가격·방향 집계 %2개 · 모집단 분리").arg(comparison.spotSamples).arg(comparison.futuresSamples);
        compared+=tr(" · 현물 기준 수량 %1 / 선물 기준 수량 %2 · 선물 q는 RPI 포함, nq는 별도 원문 · 보험기금·ADL 제외").arg(comparison.spotSignedBaseQuantity,comparison.futuresSignedBaseQuantity);
        if(comparison.spotSignedFraction&&comparison.futuresSignedFraction)compared+=tr(" · 현물 %1% / 선물 %2% · %3").arg(QString::number(*comparison.spotSignedFraction*100,'f',2),QString::number(*comparison.futuresSignedFraction*100,'f',2),comparison.directionsAgree?tr("관측 방향 일치"):tr("관측 방향 다름"));}
    part(tr("같은 base·quote 현물/선물"),comparison.state,compared);
    part(tr("공개 관측 청산 snapshot · 종목당 1초 마지막 · 전체 청산 아님"),study.liquidation.state,tr("관측 %1건 · 주문수·사람수·포지션 방향·24시간 총액 추정 없음").arg(study.liquidation.observations.size()));
    const auto count=study.liquidation.observations.size();for(qsizetype i=qMax(qsizetype(0),count-5);i<count;++i){const auto& r=study.liquidation.observations[i];lines<<tr("%1 · 원문 주문 side %2 · p %3 / ap %4 · q %5 / l %6 / z %7 · T %8 · E %9 · 수신 %10").arg(r.symbol,r.side,r.price,r.averagePrice,r.originalQuantity,r.lastFilledQuantity,r.cumulativeFilledQuantity,sourceTime(r.tradeAtMs),sourceTime(r.eventAtMs)).arg(sourceTime(r.receivedAtMs));}
    lines<<tr("현물·선물 집계 단위와 표본을 섞지 않으며, 끊긴 epoch와 미확인 구간을 이어 그리지 않습니다.");
    const auto heading=lines.takeFirst();
    const auto rendered=KeefiFonts::mixedTextHtml(heading.left(heading.size()-instrument.text().size()),FontRole::Caption12)+
        instrument.titleHtml({},FontRole::Caption12)+KeefiFonts::mixedTextHtml("\n"+usabilityPublicText(lines.join('\n')),FontRole::Caption12);
    if(coinFlowDetails_->text()!=rendered)coinFlowDetails_->setText(rendered);
    const auto& score=study.scorecard;QString text=studyName("whales")+tr(" 관측 성적표 · 실제 1시간 · 표본 %1개\n겹침 %2개 · 미확정 %3개 · no-lookahead · 비용·슬리피지 미포함").arg(score.samples).arg(score.overlappingSignals).arg(score.unresolvedSignals);
    if(chartScorecardStatisticsVisible(score.available,score.samples)&&score.meanReturnPercent)text+=tr("\n관측 평균 %1% · 상승 %2 / 하락 %3 / 보합 %4").arg(QString::number(*score.meanReturnPercent,'f',2)).arg(score.up).arg(score.down).arg(score.flat);
    else text+="\n"+tr("결과가 확인된 과거 신호 %1건 · 같은 조건에서 20건 이상 필요").arg(score.samples);
    coinFlowScorecard_->setText(text);refreshIndicatorUi();
}
void MarketCandlesView::setIncludeExtendedSessions(bool include){
    static_cast<CandleChart*>(chart_)->cancelDrawingGesture();
    if(includeExtended_==include)return;includeExtended_=include;
    if(identity_.startsWith("stock:")){
        if(stockSource_)stockSource_->setSelection(WatchlistWorkspaceList::identityFromKey(identity_),intervalMs_,includeExtended_);
        refresh();
    }
}
ExtendedChartOptions MarketCandlesView::extendedOptions()const{
    ExtendedChartOptions options;if(indicatorPreferences_)for(const auto* id:{"ma","bb","rsi","macd","breakout","low_volume","trend"})if(indicatorPreferences_->enabled(id))options.enabled.insert(id);
    if(scorecardStudy_=="ma"&&options.enabled.contains("ma"))options.eventStudies.insert("ma");
    if (indicatorPreferences_) {
        options.movingAverages=indicatorPreferences_->movingAverages();
        const auto& settings=indicatorPreferences_->studySettings();
        options.rsiPeriod=settings.rsiPeriod;
        options.macdFast=settings.macdFast;
        options.macdSlow=settings.macdSlow;
        options.macdSignal=settings.macdSignal;
        options.displayBandPeriod=settings.displayBandPeriod;
        options.displayBandDeviations=settings.displayBandDeviations;
    }
    options.higherTrends=higherTrends_;if(intervalMs_!=60000)options.enabled.remove("low_volume");return options;
}
QByteArray MarketCandlesView::calculationKey(const MarketCandleSeries& data)const{
    auto key=completedStudyKey(identity_,intervalMs_,data);QDataStream stream(&key,QIODevice::Append);
    const auto options=extendedOptions();auto enabled=options.enabled.values();enabled.sort();auto events=options.eventStudies.values();events.sort();stream<<enabled<<events;if(options.enabled.contains("breakout"))for(const auto& h:options.higherTrends)stream<<h.completesAtMs<<h.knownAtMs<<h.intervalMs<<h.state.available<<int(h.state.direction);if(options.enabled.contains("ma"))for(const auto& a:options.movingAverages)stream<<a.id<<a.period<<int(a.kind)<<a.enabled;
    if(options.enabled.contains("rsi"))stream<<options.rsiPeriod;
    if(options.enabled.contains("macd"))stream<<options.macdFast<<options.macdSlow<<options.macdSignal;
    if(identity_.startsWith("stock:")){for(const auto& row:stockSnapshot_.timeline.rows)stream<<row.startsAtMs<<row.completesAtMs<<row.connectsPrevious<<row.eligible<<row.closeDisplayEligible()<<row.reason;stream<<stockSnapshot_.timeline.adjustedComparable;}
    return key;
}
void MarketCandlesView::refreshStudies(const MarketCandleSeries& data){
    const auto key=calculationKey(data);if(key==studyKey_)return;
    invalidateCompletedStudies();studyKey_=key;studySnapshot_.intervalMs=intervalMs_;if(!data.available)studySnapshot_.reason=data.reason;emit studiesChanged();if(!data.available)return;
    const auto generation=studyGeneration_;const auto identity=identity_;const auto interval=intervalMs_;const auto currency=currency_;const bool stock=identity.startsWith("stock:");
    auto promise=std::make_shared<QPromise<CompletedPublicStudies>>();auto* watcher=new QFutureWatcher<CompletedPublicStudies>(this);
    connect(watcher,&QFutureWatcher<CompletedPublicStudies>::finished,this,[this,watcher,generation,identity,interval,currency,key,stock]{
        watcher->deleteLater();if(generation!=studyGeneration_||identity!=identity_||interval!=intervalMs_||currency!=currency_)return;
        const auto result=watcher->result();
        pendingStudyApply_=[this,generation,identity,interval,currency,key,result,stock]{
            if(generation!=studyGeneration_||identity!=identity_||interval!=intervalMs_||currency!=currency_)return;
            const auto current=stock?(stockSource_?stockSource_->snapshot().series:MarketCandleSeries{}):(model_?model_->series():MarketCandleSeries{});
            if(!current.available||current.identity!=identity||current.intervalMs!=interval||calculationKey(current)!=key)return;
            studySnapshot_=result.chart;extendedSnapshot_=result.extended;compressionSnapshot_=result.compression;explanationSnapshot_=result.explanation;detailStudyKey_=key;detailStudyCurrency_=currency;
            stockDisplayCloud_=result.stockDisplayCloud;stockDisplayAdx_=result.stockDisplayAdx;
            applyCompletedStudyDetails(result.compression,result.explanation);refresh();emit studiesChanged();
        };
        if(!isVisible()||paintedCanvasGeneration_==generation)applyPendingCompletedStudies();
    });
    watcher->setFuture(promise->future());const auto rows=data.candles;const auto options=extendedOptions();const auto timeline=stockSnapshot_.timeline;
    QThreadPool::globalInstance()->start([promise,rows,interval,options,timeline,stock]{
        promise->start();CompletedPublicStudies result;
        result.chart=stock?keefiChartStudies(rows,interval,timeline,&result.stockDisplayCloud):keefiChartStudies(rows,interval);
        result.compression=stock?candleCompression(rows,interval,timeline):candleCompression(rows,interval);
        result.explanation=stock?candleStudies(rows,interval,timeline):candleStudies(rows,interval);
        if(stock){
            const bool displayOnly=std::any_of(timeline.rows.cbegin(),timeline.rows.cend(),[](const auto& row){return !row.eligible&&row.closeDisplayEligible();});
            result.stockDisplayAdx=displayOnly?stockDisplayAdx(rows,interval,timeline):result.explanation;
        }
        result.extended=stock?extendedChartStudies(rows,interval,options,timeline):extendedChartStudies(rows,interval,options);
        promise->addResult(std::move(result));promise->finish();
    });
}
void MarketCandlesView::refreshDisplayStudies(const MarketCandleSeries& observation,const MarketCandleSeries& history){
    const bool stock=identity_.startsWith("stock:");
    auto options=extendedOptions();
    options.enabled.intersect(stock ? QSet<QString>{"ma","bb","rsi","macd"} : QSet<QString>{"ma","bb"});
    options.eventStudies.clear();
    const bool fresh=observation.available&&observation.identity==identity_&&observation.intervalMs==intervalMs_;
    const bool usableHistory=!stock&&fresh&&history.available&&!history.delayed&&!history.fromArchive&&
        history.identity==identity_&&history.intervalMs==intervalMs_&&history.currency==observation.currency;
    const auto& input=usableHistory?history:observation;
    // Request eligibility can change while the completed-byte calculation key
    // stays identical (history reset, becoming visible, or a Ready transition).
    if(usableHistory)requestDisplayWarmup(history,options);
    // Reuse the completed-byte key already computed by refreshStudies. History
    // pages only prepend older observations; loading/status changes do not
    // invalidate unchanged display calculations or serialize the full history.
    auto key=studyKey_;QDataStream stream(&key,QIODevice::Append);
    stream<<int(input.candles.size())
          <<(input.candles.isEmpty()?qint64(0):input.candles.first().startsAtMs)
          <<options.enabled.contains("ma")<<options.enabled.contains("bb");
    if(options.enabled.contains("bb"))stream<<options.displayBandPeriod<<options.displayBandDeviations;
    for(const auto& spec:options.movingAverages)stream<<spec.id<<spec.period<<int(spec.kind)<<spec.enabled;
    if(stock)for(const auto& row:stockSnapshot_.timeline.rows)stream<<row.startsAtMs<<row.completesAtMs<<row.connectsPrevious<<row.closeDisplayEligible();
    if(key==displayStudiesKey_)return;
    displayStudiesKey_=key;const auto generation=++displayStudiesGeneration_;displayExtendedSnapshot_={};
    if(!fresh||options.enabled.isEmpty())return;
    const auto identity=identity_;const auto interval=intervalMs_;const auto timeline=stockSnapshot_.timeline;
    auto promise=std::make_shared<QPromise<ExtendedChartStudySet>>();auto* watcher=new QFutureWatcher<ExtendedChartStudySet>(this);
    connect(watcher,&QFutureWatcher<ExtendedChartStudySet>::finished,this,[this,watcher,generation,identity,interval,bandLength=options.displayBandPeriod]{
        watcher->deleteLater();if(generation!=displayStudiesGeneration_||identity!=identity_||interval!=intervalMs_)return;
        const auto current=identity.startsWith("stock:")?(stockSource_?stockSource_->snapshot().series:MarketCandleSeries{}):(model_?model_->series():MarketCandleSeries{});
        if(!current.available||current.identity!=identity||current.intervalMs!=interval)return;
        displayExtendedSnapshot_=watcher->result();
        QVariantMap counts;for(const auto& line:displayExtendedSnapshot_.maLines)counts.insert(line.spec.id,int(line.points.size()));
        chart_->setProperty("displayMaPointCounts",counts);chart_->setProperty("displayBandPointCount",int(displayExtendedSnapshot_.bb.points.size()));
        chart_->setProperty("displayBandLength",bandLength);
        refreshIndicatorUi();emit studiesChanged();
    });
    watcher->setFuture(promise->future());const auto rows=input.candles;
    setProperty("displayStudyCalculationCount",property("displayStudyCalculationCount").toInt()+1);
    QThreadPool::globalInstance()->start([promise,rows,interval,options,timeline,stock]{
        promise->start();promise->addResult(stock?extendedChartDisplayStudies(rows,interval,options,timeline):extendedChartDisplayStudies(rows,interval,options));promise->finish();
    });
}
void MarketCandlesView::requestDisplayWarmup(const MarketCandleSeries& input,const ExtendedChartOptions& options){
    if(!model_||!isVisible()||!identity_.startsWith("coin:")||historyPrefetchQueued_)return;
    const auto needsHistory=[](const MarketCandleSeries& data,const ExtendedChartOptions& settings){
        int needed=settings.enabled.contains("bb")?settings.displayBandPeriod+1:0;
        if(settings.enabled.contains("ma"))for(const auto& spec:settings.movingAverages)if(spec.enabled)needed=qMax(needed,spec.period+1);
        if(!data.available||data.delayed||data.fromArchive||data.candles.isEmpty()||data.candles.size()>=needed)return false;
        // Older pages cannot repair a gap inside the recent run. Only scan a
        // deficient input (at most 200 bars); sufficient histories need no scan.
        for(qsizetype i=1;i<data.candles.size();++i)if(data.candles[i].startsAtMs-data.candles[i-1].startsAtMs!=data.intervalMs)return false;
        return true;
    };
    if(input.identity!=identity_||input.intervalMs!=intervalMs_||input.currency!=currency_||
        model_->historyStatus().state!=CandleHistoryState::Ready||!needsHistory(input,options))return;
    const auto identity=identity_;const auto interval=intervalMs_;const auto currency=currency_;
    historyPrefetchQueued_=true;
    QTimer::singleShot(0,this,[this,identity,interval,currency,needsHistory]{
        historyPrefetchQueued_=false;
        if(!model_||!isVisible()||identity_!=identity||intervalMs_!=interval||currency_!=currency||
            model_->historyStatus().state!=CandleHistoryState::Ready)return;
        // Recheck current preferences and source data after the queued turn.
        // The shared queue and model's Loading/backoff/end states deduplicate
        // requests; no completed request key survives a history reset.
        const auto current=model_->displayObservation();
        if(current.identity!=identity||current.intervalMs!=interval||current.currency!=currency||
            !needsHistory(current,extendedOptions()))return;
        setProperty("displayWarmupRequests",property("displayWarmupRequests").toInt()+1);model_->requestOlder();
    });
}
void MarketCandlesView::invalidateCompletedStudies(){
    ++displayStudiesGeneration_;displayStudiesKey_.clear();displayExtendedSnapshot_={};
    ++studyGeneration_;canvasGeneration_=0;paintedCanvasGeneration_=0;pendingStudyApply_={};
    studyKey_.clear();detailStudyKey_.clear();detailStudyCurrency_.clear();studySnapshot_={};extendedSnapshot_={};compressionSnapshot_={};explanationSnapshot_={};stockDisplayCloud_.clear();stockDisplayAdx_={};
    study_->clear();findChild<QLabel*>("marketCandlesStudyState")->clear();findChild<QLabel*>("marketCandlesStudyDetails")->clear();
    static_cast<CandleChart*>(chart_)->setStudies({},{});
}
void MarketCandlesView::applyPendingCompletedStudies(){
    auto apply=std::move(pendingStudyApply_);pendingStudyApply_={};if(apply)apply();
}
bool MarketCandlesView::eventFilter(QObject* watched,QEvent* event){
    if(watched==chart_&&(event->type()==QEvent::Resize||event->type()==QEvent::Move||event->type()==QEvent::ShowToParent||event->type()==QEvent::HideToParent)){
        layoutIndicatorChips();layoutDrawingObjectsPanel();
    }
    if(drawingObjectsPanel_&&drawingObjectsPanel_->isVisible()&&event->type()==QEvent::KeyPress){
        auto* widget=qobject_cast<QWidget*>(watched);
        if(watched==drawingObjectsPanel_||(widget&&drawingObjectsPanel_->isAncestorOf(widget))){
            if(static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){setDrawingObjectsOpen(false);return true;}
        }
    }
    if(watched==chart_&&event->type()==QEvent::Paint&&canvasGeneration_&&paintedCanvasGeneration_!=canvasGeneration_){
        const auto generation=canvasGeneration_;
        // A queued continuation runs only after the observed Paint returns.
        // Capture its rows' generation so an older Paint never releases a
        // newer selection's ready auxiliary studies.
        QTimer::singleShot(0,this,[this,generation]{
            if(generation!=studyGeneration_||generation!=canvasGeneration_)return;
            paintedCanvasGeneration_=generation;applyPendingCompletedStudies();
        });
    }
    return QWidget::eventFilter(watched,event);
}
void MarketCandlesView::keyPressEvent(QKeyEvent* event){
    if(event->key()==Qt::Key_Escape){
        const QWidget* focus=QApplication::focusWidget();
        if(drawingObjectsPanel_&&drawingObjectsPanel_->isVisible()&&focus&&(focus==drawingObjectsPanel_||drawingObjectsPanel_->isAncestorOf(focus))){
            setDrawingObjectsOpen(false);event->accept();return;
        }
        auto* chart=static_cast<CandleChart*>(chart_);
        if(chart&&chart->autoPreviewActive()){
            if(chart->autoPreviewRunning())chart->finishAutoPreview();
            else{chart->clearAutoPreview();clearAutoPreviewPanel();}
            event->accept();return;
        }
    }
    QWidget::keyPressEvent(event);
}
void MarketCandlesView::refreshIndicatorUi(){
    const auto market=identity_.startsWith("stock:")?QStringLiteral("stock"):QStringLiteral("coin");
    const auto supported=indicatorPreferences_?indicatorPreferences_->supportedIds(market):QStringList{};
    const auto states=indicatorStates();QMap<QString,ChartIndicatorState> byId;for(const auto& state:states)byId.insert(state.id,state);
    bool showReadouts=false;
    for(auto it=studyReadouts_.cbegin();it!=studyReadouts_.cend();++it){
        const auto state=byId.constFind(it.key());
        const bool show=state!=byId.cend()&&state->enabled&&!state->hidden;
        auto* label=it.value();
        if(show){
            const auto text=state->label+" · "+state->status;
            if(label->text()!=text)label->setText(text);
            label->setToolTip(state->reason.isEmpty()?text:text+"\n"+state->reason);
            showReadouts=true;
        }else if(!label->text().isEmpty()){
            label->clear();label->setToolTip({});
        }
        label->setVisible(show);
    }
    studyReadoutStrip_->setVisible(showReadouts);
    // Quotes change the chart, not these three glyphs. Keep their QIcon identity
    // stable too: replacing an identical raster invalidates button geometry.
    const auto ink=themeColors().text3;
    if(indicatorIconInk_!=ink){
        indicatorIconInk_=ink;indicatorEyeShownIcon_=chartLineIcon("eye",ink,true);
        indicatorLegend_->setProperty("selectedEyeIcon", chartLineIcon("eye", themeColors().bg, true));
        indicatorEyeHiddenIcon_=chartLineIcon("eye",ink,false);indicatorOffIcon_=chartLineIcon("off",ink);
    }
    const auto legendEye = indicatorLegend_->property("selectedEyeIcon").value<QIcon>();
    for(auto i=indicatorChips_.begin();i!=indicatorChips_.end();++i){const auto id=i.key();auto* chip=i.value();const bool on=supported.contains(id)&&indicatorPreferences_->enabled(id),visible=on&&indicatorPreferences_->visible(id);
        auto* text=static_cast<StudyLegendLabel*>(chip->findChild<QLabel*>("chartStudyStatus_"+id));text->setLegendText(studyName(id),{});text->setProperty("accent",false);text->update();
        chip->setProperty("legendHasValue",on);const auto state=byId.value(id);chip->setToolTip(studyName(id)+" · "+state.status+(state.reason.isEmpty()?QString{}:"\n"+state.reason));if(id=="similarity"){QStringList paths;for(int i=0;i<studySnapshot_.similarity.paths.size();++i)paths<<tr("경로 %1 · 유사도(상관) %2").arg(i+1).arg(QString::number(studySnapshot_.similarity.paths[i].correlation,'f',3));chip->setToolTip(chip->toolTip()+"\n"+paths.join("\n"));}
        chip->findChild<QPushButton*>("chartStudyHelp_"+id)->setToolTip(chip->toolTip());
        auto* eye=chip->findChild<QToolButton*>("chartStudyEye_"+id);const QSignalBlocker block(eye);eye->setChecked(indicatorPreferences_&&indicatorPreferences_->visible(id));
        const auto& eyeIcon=visible?legendEye:indicatorEyeHiddenIcon_;
        if(eye->icon().cacheKey()!=eyeIcon.cacheKey())eye->setIcon(eyeIcon);
        eye->setAccessibleName(studyName(id)+(visible?tr(" Visible; hide on chart"):tr(" Hidden; show on chart")));eye->setToolTip(visible?tr("차트에서 이 지표를 숨겨요. 적용 상태는 유지돼요."):tr("차트에 이 지표를 표시해요."));
        auto* off=chip->findChild<QToolButton*>("chartStudyOff_"+id);if(off->icon().cacheKey()!=indicatorOffIcon_.cacheKey())off->setIcon(indicatorOffIcon_);
    }
    legendIds_=indicatorPreferences_?indicatorPreferences_->enabledOrder(market):QStringList{};
    indicatorPicker_->setEnabled(indicatorPreferences_);layoutIndicatorChips();QSet<QString> layers;
    for(const auto& id:legendIds_)if(indicatorPreferences_->visible(id))layers.insert(id);
    auto* chart=static_cast<CandleChart*>(chart_);auto displayed=extendedSnapshot_;
    displayed.maLines=displayExtendedSnapshot_.maLines;displayed.movingAverageState=displayExtendedSnapshot_.movingAverageState;displayed.bb=displayExtendedSnapshot_.bb;
    if(tradeZonesSnapshot_.state.available)displayed.lowVolumeZones=tradeZonesSnapshot_;else if(minuteZonesSnapshot_.state.available)displayed.lowVolumeZones=minuteZonesSnapshot_;
    const auto series=identity_.startsWith("stock:")?stockSnapshot_.series:model_?model_->series():MarketCandleSeries{};
    if(!series.candles.isEmpty()){const auto current=series.candles.last().close.toDouble();auto& zones=displayed.lowVolumeZones.zones;std::stable_sort(zones.begin(),zones.end(),[current](const auto& a,const auto& b){return qMin(std::abs(a.low-current),std::abs(a.high-current))<qMin(std::abs(b.low-current),std::abs(b.high-current));});if(zones.size()>2)zones=zones.first(2);}
    chart->setStudyColors(indicatorPreferences_ ? indicatorPreferences_->studySettings().colors : QMap<QString,QColor>{});
    chart->setExtendedStudies(displayed);
    auto plottedStudies=studySnapshot_;
    if(identity_.startsWith("stock:"))plottedStudies.cloud=stockDisplayCloud_;
    chart->setStudies(plottedStudies,layers);
    if(!indicatorPreferences_)indicatorIntro_->hide();pullbackEvidence_->hide();
    const auto& similar=studySnapshot_.similarity;similarityEvidence_->setText(similar.available?tr("과거 유사 구간 %1개 · 검색 %2봉 · 겹침 제외 %3개 · 실제 경로 범위 · 변동폭 정규화 없음 · 예측이 아닙니다.").arg(similar.samples).arg(similar.searchBars).arg(similar.overlapDropped):QString{});similarityEvidence_->hide();
    if(coinFlowDetails_)coinFlowDetails_->hide();if(coinFlowDetailsPane_)coinFlowDetailsPane_->hide();if(coinFlowScorecard_)coinFlowScorecard_->hide();
    refreshCoreStudySummary();refreshLowerPane();
}
void MarketCandlesView::layoutCoreStudySummary(){
    if(!coreStudySummaryLayout_)return;
    const int columns=width()>=780?3:1;
    if(coreStudyColumns_==columns)return;
    while(auto* item=coreStudySummaryLayout_->takeAt(0))delete item;
    const QStringList ids{"pullback","similarity","whales"};
    for(int i=0;i<ids.size();++i)coreStudySummaryLayout_->addWidget(coreStudyCards_.value(ids[i]),i/columns,i%columns,Qt::AlignTop);
    for(int i=0;i<3;++i)coreStudySummaryLayout_->setColumnStretch(i,i<columns?1:0);
    coreStudyColumns_=columns;
}
void MarketCandlesView::refreshCoreStudySummary(){
    if(!coreStudySummary_)return;
    const bool stock=identity_.startsWith("stock:");
    const auto series=stock?stockSnapshot_.series:model_?model_->series():MarketCandleSeries{};
    const auto viewport=visibleViewport();
    const auto linked=[&](const QString& kind,qint64 at,const QString& text){
        return QStringLiteral("<a href=\"%1:%2\" style=\"color:%3\">%4</a>").arg(kind,QString::number(at),themeColors().accentText.name(),text.toHtmlEscaped());
    };
    QMap<QString,ChartIndicatorState> states;for(const auto& state:indicatorStates())states.insert(state.id,state);
    for(const auto* key:{"pullback","similarity","whales"}){
        const QString id=key;auto* card=coreStudyCards_.value(id);if(!card)continue;
        auto* heading=card->findChild<QLabel*>("chartCoreStudyState_"+id);
        auto* detail=card->findChild<QLabel*>("chartCoreStudyDetail_"+id);
        const bool enabled=indicatorPreferences_&&indicatorPreferences_->enabled(id);
        const bool visible=indicatorPreferences_&&indicatorPreferences_->visible(id);
        auto state=states.value(id);QString status=state.status,body;
        if(!indicatorPreferences_)status=tr("설정 수신 전");
        else if(stock&&id=="whales")status=tr("코인 전용");
        else if(!enabled)status=tr("꺼짐");
        else{
            if(!visible)status=tr("숨김 · ")+status;
            if(id=="pullback"){
                const auto& box=studySnapshot_.pullback;
                if(box.available){
                    QString price;bool live=false;
                    if(series.identity==identity_&&series.available){
                        if(series.forming){price=series.forming->close;live=true;}
                        else if(!series.candles.isEmpty())price=series.candles.last().close;
                    }
                    bool ok=false;const double current=price.toDouble(&ok);
                    const auto position=ok?(current>=box.boxLow&&current<=box.boxHigh?tr("범위 안"):tr("범위 밖")):tr("현재가 수신 전");
                    status=(!visible?tr("숨김 · "):QString{})+tr("유효 가격대 · %1").arg(position);
                    body=tr("하단 %1 ~ 상단 %2 %3").arg(priceAmount(drawnPrice(box.boxLow)),priceAmount(drawnPrice(box.boxHigh)),currency_).toHtmlEscaped();
                    if(ok)body+="<br>"+tr("%1 %2 · 완료 봉 기준 %3/10").arg(live?tr("현재가"):tr("최근 완료가"),priceAmount(price)).arg(box.score).toHtmlEscaped();
                }else body=(box.reason.isEmpty()?(studySnapshot_.reason.isEmpty()?tr("완료 봉 21개 필요"):studySnapshot_.reason):box.reason).toHtmlEscaped();
            }else if(id=="similarity"){
                const auto& comparison=studySnapshot_.similarity;
                if(comparison.referenceThroughMs>0){
                    const bool outside=viewport.fromMs>0&&(comparison.referenceLastStartMs<viewport.fromMs||comparison.referenceLastStartMs>viewport.toMs);
                    body=tr("기준 %1 KST").arg(localTime(comparison.referenceThroughMs)).toHtmlEscaped();
                    if(outside)body+=" · "+tr("기준 경로가 화면 밖").toHtmlEscaped();
                    body+=" · "+linked("reference",comparison.referenceLastStartMs,tr("기준으로 돌아가기"));
                }
                if(!comparison.paths.isEmpty()){
                    QStringList matches;
                    for(int i=0;i<comparison.paths.size();++i){const auto& path=comparison.paths[i];matches<<linked("match",path.fromMs,tr("%1) %2 ~ %3").arg(i+1).arg(localTime(path.fromMs),localTime(path.toMs)));}
                    if(!body.isEmpty())body+="<br>";body+=matches.join(QStringLiteral(" · "));
                    body+="<br>"+tr("관측한 과거의 후속 경로 · 예측 아님").toHtmlEscaped();
                }else{
                    if(!body.isEmpty())body+="<br>";
                    body+=(comparison.reason.isEmpty()?(studySnapshot_.reason.isEmpty()?tr("비교·후속 완료 봉 60개 필요"):studySnapshot_.reason):comparison.reason).toHtmlEscaped();
                }
            }else{
                const auto display=coinFlowDisplayState(coinFlowSnapshot_);
                if(coinFlowSnapshot_.lastCompletedAtMs>0)body=tr("완료 1분 기준 %1 KST").arg(localTime(coinFlowSnapshot_.lastCompletedAtMs)).toHtmlEscaped();
                if(display.latestSignalAtMs>0){if(!body.isEmpty())body+=" · ";body+=linked("signal",display.latestSignalAtMs-1,tr("최근 신호로 이동"))+" · "+localTime(display.latestSignalAtMs).toHtmlEscaped();}
                if(!display.requiredReady&&!display.reason.isEmpty()){if(!body.isEmpty())body+="<br>";body+=display.reason.toHtmlEscaped();}
                // Explain the part of the viewed chart for which actual public
                // trade coverage is absent. A blank old range is never zero flow.
                if(viewport.fromMs>0&&viewport.toMs>=viewport.fromMs){
                    const qint64 end=viewport.toMs+intervalMs_;
                    QList<QPair<qint64,qint64>> covered;
                    for(const auto& period:coinFlowInput_.primary.periods){
                        if(period.fromMs<0||period.throughMs<=period.fromMs||period.knownAtMs<=0||period.knownAtMs>coinFlowSnapshot_.asOfMs||period.throughMs>period.knownAtMs)continue;
                        if(period.coverage!=CoinFlowCoverage::ObservedContinuous&&period.coverage!=CoinFlowCoverage::RecoveredOverlap)continue;
                        const auto from=qMax(period.fromMs,viewport.fromMs),through=qMin(period.throughMs,end);
                        if(through>from)covered.append({from,through});
                    }
                    std::sort(covered.begin(),covered.end(),[](const auto& a,const auto& b){return a.first<b.first;});
                    qint64 cursor=viewport.fromMs;QStringList missing;
                    for(const auto& range:covered){if(range.first>cursor)missing<<localTime(cursor)+" ~ "+localTime(range.first);cursor=qMax(cursor,range.second);}
                    if(cursor<end)missing<<localTime(cursor)+" ~ "+localTime(end);
                    if(!missing.isEmpty()){
                        if(!body.isEmpty())body+="<br>";
                        body+=tr("체결 미관측 · %1 KST").arg(missing.mid(0,2).join(" / ")).toHtmlEscaped();
                        if(missing.size()>2)body+=" · "+tr("외 %1구간").arg(missing.size()-2).toHtmlEscaped();
                    }
                }
                if(body.isEmpty())body=tr("공개 체결·완료 1분봉을 기다립니다").toHtmlEscaped();
            }
        }
        heading->setText(studyName(id)+" · "+status);
        heading->setAccessibleName(heading->text());
        detail->setText(body);detail->setVisible(!body.isEmpty());
        QTextDocument plain;plain.setHtml(body);detail->setAccessibleName(plain.toPlainText());
        card->setProperty("studyStatus",status);card->setProperty("studyEnabled",enabled);card->setProperty("studyVisible",visible);
    }
    layoutCoreStudySummary();
}
void MarketCandlesView::layoutIndicatorChips(){
    if(!indicatorMore_||!chart_)return;
    const bool expanded=indicatorApplied_&&indicatorApplied_->isChecked();
    indicatorApplied_->setText(tr("Enabled %1 %2").arg(legendIds_.size()).arg(expanded?QStringLiteral("▴"):QStringLiteral("▾")));
    indicatorApplied_->setAccessibleName(tr("%1 enabled indicators; %2").arg(legendIds_.size()).arg(expanded?tr("Collapse"):tr("Expand")));
    indicatorApplied_->setEnabled(indicatorPreferences_);
    if(coreStudySummary_)coreStudySummary_->setVisible(expanded);
    auto* overlay=static_cast<StudyLegendOverlay*>(indicatorLegend_);
    const int budget=overlay->widthBudget(chart_,width());
    const int available=qMax(0,width()-CompactButtonHeight-24);
    const int columns=qBound(1,available/166,4);int shown=qMin(columns,int(legendIds_.size()));
    while(shown>0&&StudyLegendOverlay::minimumWidthFor(shown,shown<legendIds_.size())>budget)--shown;
    // A live value refresh must not hide a still-visible control: doing so
    // drops keyboard/VoiceOver focus even when the same chip is shown again.
    QSet<QWidget*> visibleChips;
    if(expanded)for(int i=0;i<shown;++i)if(auto* chip=indicatorChips_.value(legendIds_[i]))visibleChips.insert(chip);
    for(auto* chip:indicatorChips_)chip->setVisible(visibleChips.contains(chip));
    const auto shownIds=legendIds_.first(shown);
    if(legendColumns_!=shown||laidOutLegendIds_!=shownIds){
        while(auto* item=indicatorLegendLayout_->takeAt(0))delete item;
        for(int i=0;i<shown;++i)if(auto* chip=indicatorChips_.value(shownIds[i]))indicatorLegendLayout_->addWidget(chip,0,i);
        for(int i=0;i<5;++i)indicatorLegendLayout_->setColumnStretch(i,i<shown?1:0);
        laidOutLegendIds_=shownIds;legendColumns_=shown;
    }
    const int count=legendIds_.size()-shown;indicatorMore_->setVisible(expanded&&count>0);indicatorMore_->setText("+"+QString::number(count));indicatorMore_->setAccessibleName(tr("Show %1 more enabled indicators").arg(count));
    overlay->arrange(chart_,budget,shownIds,count>0,expanded&&!legendIds_.isEmpty());
}
void MarketCandlesView::showIndicatorOverflow(){
    if(!indicatorPreferences_)return;QDialog dialog(this);dialog.setObjectName("chartStudyOverflow");dialog.setWindowTitle(tr("Enabled Indicators"));dialog.resize(520,440);
    auto* layout=new QVBoxLayout(&dialog);auto* scroll=new QScrollArea(&dialog);scroll->setWidgetResizable(true);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);auto* content=new QWidget;auto* rows=new QVBoxLayout(content);rows->setContentsMargins(0,0,0,0);rows->setSpacing(0);
    for(const auto& id:legendIds_){auto* row=new QWidget(content);auto* cells=new QHBoxLayout(row);cells->setContentsMargins(0,0,0,0);auto* text=new QPushButton(studyName(id)+tr(" ⓘ"),row);text->setObjectName("chartStudyOverflowHelp_"+id);text->setAccessibleName(studyName(id)+tr(" Information"));text->setMinimumSize(44,44);text->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);cells->addWidget(text,1);connect(text,&QPushButton::clicked,this,[this,id,text]{showIndicatorHelp(id,text);});
        auto* eye=new QPushButton(row);eye->setCheckable(true);eye->setChecked(indicatorPreferences_->visible(id));eye->setMinimumSize(100,44);eye->setAccessibleName(studyName(id)+tr(" Toggle Visibility"));cells->addWidget(eye);
        const auto showEye=[this,eye](bool visible){eye->setText(visible?tr("Visible"):tr("Hidden"));eye->setIcon(visible?indicatorEyeShownIcon_:indicatorEyeHiddenIcon_);};showEye(eye->isChecked());
        auto* off=new QPushButton("×",row);off->setFixedSize(44,44);off->setAccessibleName(studyName(id)+tr(" Turn Off"));cells->addWidget(off);
        connect(eye,&QPushButton::toggled,&dialog,[this,id,showEye](bool visible){indicatorPreferences_->setVisible(id,visible);showEye(visible);});
        connect(off,&QPushButton::clicked,&dialog,[this,id,text,eye,off]{indicatorPreferences_->setEnabled(id,false);text->setText(studyName(id)+tr(" · Off"));eye->setEnabled(false);off->setEnabled(false);});rows->addWidget(row);
    }rows->addStretch();scroll->setWidget(content);layout->addWidget(scroll);QDialogButtonBox buttons(QDialogButtonBox::Close,&dialog);buttons.button(QDialogButtonBox::Close)->setText(tr("Close"));buttons.button(QDialogButtonBox::Close)->setMinimumSize(44,44);layout->addWidget(&buttons);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();QTimer::singleShot(0,this,[this]{window()->activateWindow();(indicatorMore_->isVisible()?indicatorMore_:indicatorPicker_)->setFocus(Qt::OtherFocusReason);});
}
void MarketCandlesView::refreshLowerPane(){
    if (!lowerPane_) return;
    const bool stock=identity_.startsWith("stock:");
    const auto& oscillators=stock?displayExtendedSnapshot_:extendedSnapshot_;
    const auto visible=indicatorPreferences_ ? indicatorPreferences_->visiblePanes(stock?"stock":"coin") : QStringList{};
    for (auto it=lowerStudies_.cbegin(); it!=lowerStudies_.cend(); ++it) {
        const auto& id=it.key();
        auto* pane=static_cast<LowerStudyPane*>(it.value());
        const bool shown=visible.contains(id);
        pane->setVisible(shown);
        if (!shown) continue;
        auto* plot=pane->plot;
        plot->lines.clear(); plot->bars.clear(); plot->colors.clear(); plot->reason.clear();
        plot->rsi=id=="rsi"; plot->stock=stock; plot->interval=intervalMs_;
        plot->colors.append(indicatorPreferences_->lineColor(id,themeColors().studyPurple));
        QString source=timeframeLabel(intervalMs_);
        if (id=="rsi") {
            if (oscillators.rsi.state.available) plot->lines.append(oscillators.rsi.points);
            plot->reason=oscillators.rsi.state.reason;
        } else if (id=="macd") {
            const auto& macd=oscillators.macd;
            if (macd.state.available) {plot->lines<<macd.line<<macd.signal; plot->bars=macd.histogram;}
            plot->reason=macd.state.reason;
            plot->colors.append(indicatorPreferences_->lineColor("macdSignal",themeColors().studyBlue));
            plot->risingBar=indicatorPreferences_->lineColor("macdHistogramUp",MovementColorPolicy::colorFor(this,MovementDirection::Up));
            plot->fallingBar=indicatorPreferences_->lineColor("macdHistogramDown",MovementColorPolicy::colorFor(this,MovementDirection::Down));
        } else if (id=="cvd") {
            plot->interval=60000;
            if (deltaSnapshot_.state.available) for (const auto& bucket:deltaSnapshot_.buckets) if (!bucket.points.isEmpty()) {
                plot->lines.append(bucket.points);
                if (plot->colors.size()<plot->lines.size()) plot->colors.append(plot->colors.first());
            }
            plot->reason=deltaSnapshot_.state.reason;
            source=tr("%1 · 완료 1분").arg(deltaSnapshot_.currency);
        } else if (id=="oi") {
            plot->interval=3600000;
            if (publicContextSnapshot_.oiChange.available) plot->lines.append(publicContextSnapshot_.openInterest);
            plot->reason=tr("동일 계약의 완료 OI 기록 없음");
            source=tr("실제 1시간");
        }
        if((id=="rsi"||id=="macd")&&plot->lines.isEmpty()){
            if(stock&&!stockSnapshot_.series.available)plot->reason=stockUnavailableReason();
            else {
                if(plot->reason.isEmpty())plot->reason=oscillators.reason.isEmpty()?tr("완료 봉으로 지표 계산 중"):oscillators.reason;
                if(stock){
                    const auto& config=indicatorPreferences_->studySettings();
                    const int needed=id=="rsi"?config.rsiPeriod+1:config.macdSlow+config.macdSignal-1;
                    const auto exclusion=stockSnapshot_.timeline.recentExclusionReason(needed,true);
                    if(!exclusion.isEmpty()&&!plot->reason.contains(exclusion))plot->reason=exclusion+" · "+plot->reason;
                }
            }
        }
        pane->title->setText(studyName(id)+" · "+source);
        plot->setAccessibleDescription(plot->lines.isEmpty()&&plot->bars.isEmpty()?plot->reason:source);
        plot->update();
    }
    lowerPane_->setVisible(!visible.isEmpty());
}
void MarketCandlesView::resizeEvent(QResizeEvent* event){
    QWidget::resizeEvent(event);layoutIndicatorChips();layoutCoreStudySummary();layoutDrawingTools();layoutDrawingObjectsPanel();
    // Child layout geometry can still describe the old size during this event.
    QTimer::singleShot(0,this,[this]{layoutDrawingObjectsPanel();});
}
void MarketCandlesView::setMetadataFailure(const QString& identity,const QString& reason){
    const auto key=identity.startsWith("coin:")&&!reason.isEmpty()?identity:QString{};
    const auto text=key.isEmpty()?QString{}:reason;
    if(metadataFailureIdentity_==key&&metadataFailureReason_==text)return;
    metadataFailureIdentity_=key;metadataFailureReason_=text;
    refreshInformation();
}
void MarketCandlesView::setSelection(const QString& identity,qint64 interval){
    if(!supportedTimeframe(interval))return;
    if(identity!=identity_||interval!=intervalMs_){static_cast<CandleChart*>(chart_)->setLiveTradePrice({},0);static_cast<CandleChart*>(chart_)->resetViewport();}
    if((identity_!=identity||intervalMs_!=interval)&&indicatorHelpDialog_)indicatorHelpDialog_->reject();
    if(identity_!=identity||intervalMs_!=interval){++flowGeneration_;higherTrends_.clear();minuteZonesSnapshot_={};relativeSnapshot_={};supplementalKey_.clear();if(coinFlowRunner_)coinFlowRunner_->invalidate();coinFlowKey_.clear();static_cast<CandleChart*>(chart_)->cancelDrawingGesture();clearDrawingFeedback();setCandleEvidenceNumber(0);clearPrivate();invalidateCompletedStudies();}
    if(identity!=metadataFailureIdentity_){metadataFailureIdentity_.clear();metadataFailureReason_.clear();}
    identity_=identity;intervalMs_=interval;asset_.clear();currency_.clear();
    const auto upbit=QRegularExpression("\\Acoin:upbit:(KRW|BTC|USDT)-([A-Z0-9]{1,20})\\z").match(identity_);
    if(upbit.hasMatch()){currency_=upbit.captured(1);asset_=upbit.captured(2);}
    const bool stock=identity_.startsWith("stock:");
    if(stockSource_){
        if(stock)stockSource_->setSelection(WatchlistWorkspaceList::identityFromKey(identity_),intervalMs_,includeExtended_);
        stockSource_->setActive(stock&&isVisible());
    }
    const auto before = refreshRevision_;
    if(model_&&isVisible())model_->setSelection(stock?QString{}:identity_,intervalMs_);
    // A changed selection refreshes this view synchronously through the model's signal.
    if (refreshRevision_ == before) refresh();
}
void MarketCandlesView::showEvent(QShowEvent* e){QWidget::showEvent(e);setSelection(identity_,intervalMs_);maybeShowIndicatorIntro();}
void MarketCandlesView::hideEvent(QHideEvent* e){
    if(indicatorHelpDialog_)indicatorHelpDialog_->reject();
    ++flowGeneration_;
    if(coinFlowRunner_)coinFlowRunner_->setEnabled(false);
    if(publicContextSource_)publicContextSource_->setDemand(false,false);
    static_cast<CandleChart*>(chart_)->setLiveTradePrice({},0);
    static_cast<CandleChart*>(chart_)->cancelDrawingGesture();
    clearDrawingFeedback();
    if(!identity_.startsWith("stock:"))invalidateCompletedStudies();
    QWidget::hideEvent(e);
    if(stockSource_)stockSource_->setActive(false);
    const auto before = refreshRevision_;
    if(model_)model_->setSelection({},intervalMs_);
    if (refreshRevision_ == before) refresh();
}
QString MarketCandlesView::stockUnavailableReason() const {
    const auto& snapshot=stockSnapshot_;
    if(!identity_.startsWith("stock:")||snapshot.series.available)return {};
    if(!snapshot.connectionIssue){
        if(!snapshot.series.reason.isEmpty())return usabilityPublicText(snapshot.series.reason);
        return snapshot.loading?tr("주식 차트 조회 중"):tr("이 구간에 완료된 주식 봉이 아직 없어요");
    }
    const bool domesticDaily=identity_.startsWith("stock:KR:")&&intervalMs_==86400000;
    const auto supported=domesticDaily?tr("국내 일봉은 토스증권 또는 키움증권에서 볼 수 있어요"):
        identity_.startsWith("stock:KR:")?tr("이 시간봉은 토스증권에서 볼 수 있어요. 키움증권은 국내 일봉을 제공해요"):
        tr("미국 주식 차트는 토스증권에서 볼 수 있어요");
    switch(*snapshot.connectionIssue){
    case StockChartConnectionState::VaultClosed:return tr("금고가 열려 있지 않아요. 금고를 만들거나 열어 증권 연결을 확인해 주세요. ")+supported;
    case StockChartConnectionState::NoBroker:return tr("연결된 증권사가 없어요. ")+supported;
    case StockChartConnectionState::OtherBroker:return tr("현재 연결로는 이 차트를 조회할 수 없어요. ")+supported;
    case StockChartConnectionState::AuthenticationRequired:return tr("토스증권 인증이 필요해요. ")+supported;
    case StockChartConnectionState::KiwoomAuthenticationRequired:return domesticDaily?tr("키움증권 인증이 필요해요. ")+supported:supported;
    case StockChartConnectionState::Ready:break;
    }
    return usabilityPublicText(snapshot.series.reason);
}
void MarketCandlesView::refreshStock(){
    static_cast<CandleChart*>(chart_)->setLiveTradePrice({},0);
    detailStudyKey_.clear();
    auto snapshot=stockSource_?stockSource_->snapshot():StockChartSnapshot{};
    if(snapshot.series.identity!=identity_||snapshot.series.intervalMs!=intervalMs_){
        snapshot={};snapshot.series.identity=identity_;snapshot.series.intervalMs=intervalMs_;
        if(!stockSource_)snapshot.connectionIssue=StockChartConnectionState::NoBroker;
        else snapshot.loading=true;
    }
    stockSnapshot_=snapshot;
    auto data=snapshot.series;
    data.available=data.available&&!data.candles.isEmpty()&&(snapshot.source==StockSourceKind::Toss||snapshot.source==StockSourceKind::KiwoomDaily);
    asset_=data.available?data.asset:QString{};currency_=data.currency;
    const auto source=snapshot.source==StockSourceKind::Toss?tr("토스"):snapshot.source==StockSourceKind::KiwoomDaily?tr("키움 일봉"):tr("주식 원천 자료 대기");
    const auto mode=source;
    const auto instrument=instrumentLabel();
    const auto titleSuffix=tr(" · %1 · %2 캔들 · %3").arg(currency_.isEmpty()?tr("통화 확인 대기"):currency_,timeframeLabel(intervalMs_),mode);
    if(title_){title_->setTextFormat(Qt::RichText);title_->setText(instrument.titleHtml(titleSuffix));title_->setAccessibleName(instrument.text()+titleSuffix);}
    const auto adjustment=snapshot.adjustment==StockAdjustment::SourceAdjusted?tr("수정주가 적용"):snapshot.adjustment==StockAdjustment::SourceUnadjusted?tr("수정주가 미적용"):
        snapshot.adjustment==StockAdjustment::CloseSplitAdjustedOnly?tr("수정주가 · 종가 분할 조정만 확인"):tr("수정주가 미확인");
    QStringList facts{mode,includeExtended_?tr("시간외 포함"):tr("정규장"),adjustment};
    if(snapshot.loading)facts<<tr("보조 원천 자료 확인 중");
    if(snapshot.source==StockSourceKind::KiwoomDaily)facts<<snapshot.sessionNote;
    if(snapshot.source==StockSourceKind::Toss)facts<< (identity_.startsWith("stock:KR:")?tr("KRX+NXT 통합 · 개별 거래소 구분 미제공"):tr("일부 미국 거래소 · 전체 시장 최우선 호가가 아님"));
    facts<<(data.sourceTimeMs>0?tr("원천 갱신 시각 %1").arg(sourceTime(data.sourceTimeMs)):tr("원천 갱신 시각 미제공"));
    facts<<tr("수신 시각 %1").arg(sourceTime(data.receivedAtMs));
    if(!data.candles.isEmpty()){
        const auto date=!snapshot.timeline.rows.isEmpty()?snapshot.timeline.rows.last().tradingDate.toString(Qt::ISODate):localTime(data.candles.last().startsAtMs);
        facts<<tr("마지막 완료 %1").arg(date);
    }
    if(!snapshot.sessionNote.isEmpty())facts<<snapshot.sessionNote;
    if(!snapshot.adjustmentNote.isEmpty())facts<<snapshot.adjustmentNote;
    if(!snapshot.calendarReason.isEmpty())facts<<snapshot.calendarReason;
    stockContext_->setText(facts.join(" · "));stockContext_->setToolTip({});stockContext_->hide();
    QStringList observations;
    if(snapshot.realtimeAllowed&&!snapshot.livePrice.isEmpty()&&snapshot.liveEvidence.complete){
        observations<<tr("실시간 관측 가격 %1 %2 · LOSSY 참고 시세 · OHLCV 집계에 사용하지 않음\n원천 %3 · 수신 %4\n%5")
            .arg(snapshot.livePrice,currency_,sourceTime(snapshot.liveEvidence.sourceTimeMs),sourceTime(snapshot.liveEvidence.receivedAtMs),snapshot.liveEvidence.url);
    }
    const auto& restrictions=snapshot.restrictions;
    if(restrictions.currentVI&&*restrictions.currentVI)observations<<tr("VI 현재 관측 · 거래 제한 판정 제외 · 관측 %1").arg(sourceTime(restrictions.currentObservationAtMs));
    if(restrictions.upperLimit)observations<<tr("현재 상한가 %1 %2").arg(*restrictions.upperLimit,currency_);
    if(restrictions.lowerLimit)observations<<tr("현재 하한가 %1 %2").arg(*restrictions.lowerLimit,currency_);
    if(restrictions.historicalCoverage!=StockHistoryCoverage::VerifiedIntervals)observations<<tr("과거 VI·상하한가 제외 구간 검증 불가 · 해당 민감 판정 숨김");
    if(!snapshot.pending.isEmpty()){
        observations<<tr("미확정 원천 관측 %1개 · 목표 시간봉·완료 지표에 넣지 않음").arg(snapshot.pending.size());
        auto pendingRows=snapshot.pending;std::sort(pendingRows.begin(),pendingRows.end(),[](const auto& a,const auto& b){return a.observation.sourceLabelAtMs>b.observation.sourceLabelAtMs;});
        for(qsizetype i=0;i<qMin(qsizetype(5),pendingRows.size());++i){const auto& pending=pendingRows[i];observations<<tr("%1 · 완료 경계 %2 · %3").arg(sourceTime(pending.observation.sourceLabelAtMs),sourceTime(pending.completesAtMs),pending.reason);}
        if(pendingRows.size()>5)observations<<tr("최근 5개 관측의 미확정 사유만 표시합니다.");
    }
    stockObservations_->setText(usabilityPublicText(observations.join('\n')));stockObservations_->hide();
    setThemeStyle(stockObservations_,restrictions.currentVI&&*restrictions.currentVI?"color:${danger};border:0px solid transparent;border-left:2px solid ${danger};padding-left:6px;":QString{});

    refreshStudies(data);
    refreshDisplayStudies(data,data);
    refreshPublicContext();
    refreshIndicatorUi();clearPrivate();
    auto* chart=static_cast<CandleChart*>(chart_);chart->setContext(intervalMs_,currency_,instrument.text());chart->setStockTimeline(true,snapshot.timeline);
    chart->setObservation(data.available?data.candles:QList<MarketCandle>{},data.forming);canvasGeneration_=data.available?studyGeneration_:0;
    chart_->setProperty("displayIdentity",data.available?identity_:QString{});
    chart_->setProperty("displayIntervalMs",intervalMs_);
    chart_->setProperty("displayCompletedCount",data.available?int(data.candles.size()):0);
    chart_->setProperty("displayHistoricalOnly",false);
    chart_->setProperty("displayFromMemoryCache",false);
    syncDrawingContext(data);
    chart_->setVisible(data.available||data.forming.has_value());
    chart_->setAccessibleDescription(tr("주식 실제 완료 봉 %1개 · 정상 휴장 압축 · 장중 관측 공백 유지 · UTC 원본 시각 보존%2")
        .arg(data.candles.size()).arg(data.forming?tr(" · 형성 중 봉은 판정 제외"):QString{}));
    const auto completed=snapshot.timeline.rows.isEmpty()?qint64(0):snapshot.timeline.rows.last().completesAtMs;
    setNativeLedgerText(status_,data.available?tr("완료 구간 %1~%2 · 원천 %3").arg(sourceTime(data.candles.first().startsAtMs),sourceTime(completed),source):
        (data.reason.isEmpty()?tr("완료 주식 캔들 자료 없음"):data.reason),FontRole::Caption12);status_->setToolTip({});
    const auto& compression=snapshot.compression;
    study_->setText(compression.available?(compression.compressed?tr("마지막 완료 구간: 변동성 압축 상태 · 방향은 알 수 없음"):tr("마지막 완료 구간: 변동성 압축 상태 아님")):
        tr("변동성 압축 · %1").arg(compression.reason.isEmpty()?tr("완료 봉·세션 자료 대기"):compression.reason));
    const auto& context=snapshot.marketContextStudies;
    findChild<QLabel*>("marketCandlesStudyState")->setText(context.adxAvailable?tr("ADX(14) %1 · %2").arg(QString::number(context.adx,'f',2),context.regime):
        tr("ADX · %1").arg(context.reason.isEmpty()?tr("연속 완료 봉·세션 자료 대기"):context.reason));
    QStringList details{tr("실제 목표 시간봉의 완료 OHLCV만 사용합니다. 정상 휴장은 압축하고 장중 관측 공백·거래 제한 구간은 채우지 않습니다."),
        tr("분봉은 장 시작 경계의 완전한 그룹만, 일봉은 원천 일봉만 사용합니다. LOSSY 체결은 참고 가격이며 OHLCV를 재구성하지 않습니다.")};
    if(!snapshot.timeline.comparisonReason.isEmpty())details<<snapshot.timeline.comparisonReason;
    if(!snapshot.timeline.projectionReason.isEmpty())details<<snapshot.timeline.projectionReason;
    if(context.profileAvailable)details<<tr("거래량 대표가격 근사 %1~%2 %3 · 실제 가격별 체결 분포가 아님").arg(QString::number(context.profileLow,'g',8),QString::number(context.profileHigh,'g',8),currency_);
    for(const auto& trend:context.trends)details<<tr("%1 추세 · %2 · 완료 %3봉").arg(timeframeLabel(trend.minutes*60000LL),trend.available?(trend.direction>0?tr("▲ 상승"):trend.direction<0?tr("▼ 하락"):tr("보합")):tr("자료 부족")).arg(trend.bars);
    findChild<QLabel*>("marketCandlesStudyDetails")->setText(details.join("\n\n"));
}
void MarketCandlesView::refreshInformation(){
    const bool stock=identity_.startsWith("stock:");auto data=stock?stockSnapshot_.series:model_?model_->series():MarketCandleSeries{};
    const auto archiveStatus=!stock&&model_?model_->archiveStatus():CandleArchiveStatus{};
    archiveNotice_->setVisible(archiveStatus.canRetry&&!archiveStatus.message.isEmpty());
    archiveMessage_->setText(archiveStatus.message);archiveRetry_->setVisible(archiveStatus.canRetry);
    if(!stock&&model_&&!data.available){
        const auto observed=model_->displayObservation();
        if(observed.delayed&&observed.identity==identity_&&observed.intervalMs==intervalMs_)data=observed;
    }
    const bool matches=data.identity==identity_&&data.intervalMs==intervalMs_;
    if(!matches){data.available=false;data.delayed=false;data.sourceTimeMs=0;if(stock)data.reason=selectablePlainText(status_);else data.reason.clear();}
    const auto provider=stock?(stockSnapshot_.source==StockSourceKind::KiwoomDaily?tr("키움증권"):stockSnapshot_.source==StockSourceKind::Toss?tr("토스증권"):QString{}):identity_.startsWith("coin:upbit:")?QStringLiteral("업비트"):identity_.startsWith("coin:binance:")?QStringLiteral("바이낸스"):QString{};
    const auto hint=usabilitySourceTime(provider,data.sourceTimeMs);if(title_)title_->setToolTip(title_->property("elideRight").toBool()?title_->text()+QStringLiteral("\n")+hint:hint);
    QStringList information{findChild<QLabel*>("marketCandlesDefinition")->text(),findChild<QLabel*>("marketCandlesStudyDetails")->text(),hint};
    const bool metadataFailure=!stock&&metadataFailureIdentity_==identity_&&!metadataFailureReason_.isEmpty()&&!data.available;
    if(metadataFailure)information<<tr("종목 목록 조회 실패 · %1").arg(metadataFailureReason_);
    if(!archiveStatus.detail.isEmpty())information<<archiveStatus.detail;
    if(!relatedMarketInformation_.isEmpty())information<<relatedMarketInformation_;
    if(!stock&&!data.available){information<<data.reason;if(data.delayed)information<<(data.fromArchive?
        tr("금고에 저장해 둔 완료 캔들을 새 캔들이 올 때까지 보여 주고 있어요. 지표 계산·알림·공유에는 쓰지 않아요."):
        data.fromMemoryCache?tr("이번 실행에서 확인했던 완료 캔들을 먼저 보여 주고 있어요. 최신 가격·지표 계산·알림·공유에는 쓰지 않아요."):
        tr("마지막으로 확인한 완료 캔들을 유지하고 있어요. 지표 계산·알림·공유에는 쓰지 않아요."))+"\n"+tr("마지막 정상 수신 %1").arg(sourceTime(data.receivedAtMs));}
    if(stock){information<<stockSnapshot_.sessionNote<<stockSnapshot_.adjustmentNote<<stockSnapshot_.marketStudies.volumeScope;
        if(std::any_of(stockSnapshot_.marketStudies.days.cbegin(),stockSnapshot_.marketStudies.days.cend(),[](const auto& day){return day.programEvidence.complete;}))information<<tr("프로그램 KRX만 · NXT 제외");
        if(stockSnapshot_.restrictions.historicalCoverage!=StockHistoryCoverage::VerifiedIntervals)information<<tr("과거 VI·상하한가 제외 구간 검증 불가 · 해당 민감 판정 숨김");}
    for(const auto& state:indicatorStates())if(state.enabled)information<<state.label+" · "+state.status+"\n"+state.reason;
    if(levelsSnapshot_.state.available&&levelsSnapshot_.approximateTick)
        information<<tr("레벨선 · 호가 단위 미확인\n현재가의 0.05%를 가까운 레벨을 묶는 근사 기준으로 사용합니다.");
    for(const auto& level:levelsSnapshot_.all)for(const auto& member:level.members)information<<member.label+" "+level.price+"\n"+member.source+"\n"+sourceTime(member.fromMs)+" ~ "+sourceTime(member.throughMs);
    const auto summary=tr("완료 봉과 형성 봉을 구분해 표시하고 지표는 완료 봉만 사용하며 빈 시간은 채우지 않습니다.");
    const auto basis=title_?title_->accessibleName():QString{};
    const auto unavailable=tr("확인 필요");
    const auto delay=data.delayed&&!data.reason.isEmpty()?usabilityPublicText(data.reason):
        !stock&&data.available?tr("실시간"):unavailable;
    information_->setPublicSourceInformation(
        usabilityText(UsabilityText::ChartInformation),summary,
        {basis.isEmpty()?unavailable:basis+QStringLiteral(" · ")+timeframeLabel(intervalMs_),
         provider.isEmpty()?unavailable:provider,delay,currency_.isEmpty()?unavailable:currency_,summary},
        usabilityPublicText(information.join("\n\n")));
    study_->hide();findChild<QLabel*>("marketCandlesStudyState")->hide();if(drawingFeedbackText_.isEmpty())drawingStatus_->hide();stockContext_->hide();
    const bool vi=stock&&stockSnapshot_.restrictions.currentVI.value_or(false);stockObservations_->setVisible(vi);if(vi)stockObservations_->setText(usabilityText(UsabilityText::StockVi));
    // Source failures remain explicit; normal source timing leaves the main view.
    const bool canConnect=stock&&matches&&!data.available&&stockSnapshot_.connectionIssue.has_value();
    stockConnect_->parentWidget()->setVisible(canConnect);
    stockDaily_->setVisible(canConnect&&stockSnapshot_.kiwoomDailyAlternative);
    if(data.available){status_->clear();status_->hide();}
    else{QString message=stock?stockUnavailableReason():QString{};
        if(!stock){
            if(matches&&data.delayed&&data.fromArchive){
                if(data.reason==QStringLiteral("캔들 조회 대기"))message=tr("저장된 캔들");
                else if(data.reason.contains(QStringLiteral("연결 끊김")))message=tr("저장된 기록 · 연결 끊김");
                else if(data.reason==QStringLiteral("원천 요청 제한"))message=tr("저장된 기록 · 재조회 대기");
                else if(data.reason.contains(QStringLiteral("시간 초과")))message=tr("저장된 기록 · 응답 시간 초과");
                else if(data.reason.contains(QStringLiteral("메타데이터"))||data.reason.contains(QStringLiteral("종목 목록")))message=tr("저장된 기록 · 종목 확인 중");
                else message=tr("저장된 기록 · 최신 자료 받지 못함");
            }
            else if(matches&&data.delayed&&data.fromMemoryCache){
                if(data.reason==QStringLiteral("캔들 조회 대기"))message=tr("이전 캔들");
                else if(data.reason==QStringLiteral("원천 요청 제한"))message=tr("이전 캔들 · 재조회 대기");
                else if(data.reason.contains(QStringLiteral("메타데이터"))||data.reason.contains(QStringLiteral("종목 목록")))message=tr("이전 캔들 · 종목 확인 중");
                else message=tr("이전 캔들 · 최신 자료 받지 못함");
            }
            else if(matches&&data.delayed)message=tr("지연 · 마지막 확인 구간");
            else if(!matches||data.reason==QStringLiteral("캔들 조회 대기"))message=tr("첫 캔들 대기");
            else if(data.reason==QStringLiteral("공개 종목 메타데이터 확인 대기") ||
                    data.reason==QStringLiteral("업비트 종목 목록을 확인하고 있어요."))message=tr("종목 확인 중");
            else if(data.reason==QStringLiteral("지원하지 않는 캔들 identity 또는 시간봉") ||
                    data.reason==QStringLiteral("업비트에서 지원하지 않는 마켓이에요.") ||
                    data.reason==QStringLiteral("지원하지 않는 마켓이에요."))message=tr("지원 없음");
            else if(data.reason==QStringLiteral("완료된 캔들 없음"))message=tr("기록 부족");
            else if(data.reason==QStringLiteral("최신 캔들 자료 부족"))message=usabilityText(UsabilityText::NeedsCheck);
            else if(data.reason.startsWith(QStringLiteral("공개 캔들 연결 끊김")))message=tr("연결 끊김");
            else message=tr("받지 못했어요");
        }
        if(metadataFailure)message=(matches&&data.delayed?tr("이전 캔들 · "):QString{})+tr("종목 목록 조회 실패 · %1").arg(metadataFailureReason_);
        if(message.isEmpty())message=usabilityText(UsabilityText::NeedsCheck);
        setNativeLedgerText(status_,message,FontRole::Caption12);status_->show();}
    status_->setToolTip({});
}
void MarketCandlesView::refresh(){
    ++refreshRevision_;
    const auto presentation=qScopeGuard([this]{refreshInformation();refreshLowerPane();});
    static_cast<CandleChart*>(chart_)->setChartStyle(chartStyle());refreshChartStyleButton();
    refreshCoinFlow();
    if(identity_.startsWith("stock:")){refreshStock();return;}
    stockSnapshot_={};stockContext_->hide();stockObservations_->hide();
    static_cast<CandleChart*>(chart_)->setStockTimeline(false,{});
    auto data=model_?model_->series():MarketCandleSeries{};
    const auto observed=model_?model_->displayObservation():MarketCandleSeries{};
    const bool matches=data.identity==identity_&&data.intervalMs==intervalMs_;
    if(identity_.startsWith("coin:binance:")){
        asset_=matches?data.asset:QString{};currency_=matches?data.currency:QString{};
        if(matches&&currency_.isEmpty()&&observed.delayed&&observed.identity==identity_&&observed.intervalMs==intervalMs_){
            asset_=observed.asset;currency_=observed.currency;
        }
    }
    if(!matches){data.available=false;data.candles.clear();data.forming.reset();data.sourceTimeMs=0;data.receivedAtMs=0;data.sourceUrl.clear();data.reason=tr("선택 시간봉의 원천 자료 대기");}
    const auto instrument=instrumentLabel();
    const auto titleSuffix=tr(" · %1 · %2 캔들").arg(currency_.isEmpty()?tr("통화 확인 대기"):currency_,timeframeLabel(intervalMs_));
    if(title_){title_->setTextFormat(Qt::RichText);title_->setText(instrument.titleHtml(titleSuffix));title_->setAccessibleName(instrument.text()+titleSuffix);}
    refreshStudies(data);
    refreshDisplayStudies(data,observed);
    refreshPublicContext();
    refreshIndicatorUi();
    setPriceLines(priceLines_);
    static_cast<CandleChart*>(chart_)->setContext(intervalMs_,currency_,instrument.text());
    const bool historicalOnly=!data.available&&observed.delayed&&!observed.candles.isEmpty()&&
        observed.identity==identity_&&observed.intervalMs==intervalMs_&&observed.currency==currency_;
    const bool fullHistory=matches&&observed.identity==identity_&&observed.intervalMs==intervalMs_&&observed.currency==currency_;
    static_cast<CandleChart*>(chart_)->setObservation(data.available?(fullHistory?observed.candles:data.candles):historicalOnly?observed.candles:QList<MarketCandle>{},data.forming);
    chart_->setProperty("displayIdentity",data.available||historicalOnly?identity_:QString{});
    chart_->setProperty("displayIntervalMs",intervalMs_);
    chart_->setProperty("displayCompletedCount",data.available?(fullHistory?int(observed.candles.size()):int(data.candles.size())):historicalOnly?int(observed.candles.size()):0);
    chart_->setProperty("displayHistoricalOnly",historicalOnly);
    chart_->setProperty("displayFromMemoryCache",historicalOnly&&observed.fromMemoryCache);
    canvasGeneration_=data.available?studyGeneration_:0;
    // Faster trade overlays are for painting only. Drawing/alert evidence keeps
    // the official forming candle and the same completed observations as before.
    auto drawingObservation=observed;drawingObservation.forming=data.forming;
    syncDrawingContext(data.available&&fullHistory?drawingObservation:data);
    refreshHistoryNavigation();
    chart_->setVisible(data.available||data.forming.has_value()||historicalOnly);
    refreshLiveObservation();
    chart_->setAccessibleDescription((data.available?tr("실제 완료 캔들 %1개 · %2 · %3 · 빈 시간은 채우지 않음").arg(fullHistory?observed.candles.size():data.candles.size()).arg(timeframeLabel(intervalMs_),currency_):QString{})+
        (historicalOnly?tr("지연 · 마지막 확인 완료 캔들 %1개 · 지표 계산과 알림 제외").arg(observed.candles.size()):QString{})+
        (data.forming?tr(" · 형성 중 1개(흐림) · 지표 판정 제외"):QString{})+
        (indicatorPreferences_&&indicatorPreferences_->enabled("similarity")&&indicatorPreferences_->visible("similarity")&&studySnapshot_.similarity.available?tr(" · 과거 이후 경로 %1봉").arg(studySnapshot_.similarity.followingBars):QString{}));
    auto* state=findChild<QLabel*>("marketCandlesStudyState");auto* calculations=findChild<QLabel*>("marketCandlesStudyDetails");
    const auto timing=(data.sourceTimeMs>0?tr("원천 시각 %1").arg(sourceTime(data.sourceTimeMs)):tr("원천 시각 미제공"))+
        tr(" · 수신 시각 %1").arg(sourceTime(data.receivedAtMs));
    status_->setToolTip({});
    if(!data.available){setNativeLedgerText(status_,(data.reason.isEmpty()?tr("완료 캔들 자료 없음"):data.reason)+"\n"+timing,FontRole::Caption12);study_->clear();state->clear();calculations->clear();detailStudyKey_.clear();return;}
    setNativeLedgerText(status_,tr("완료 구간 %1~%2 KST · %3%4").arg(localTime(data.candles.first().startsAtMs),localTime(data.candles.last().startsAtMs+intervalMs_),timing,data.forming?tr(" · 형성 중 봉은 지표에서 제외"):QString{}),FontRole::Caption12);
}
void MarketCandlesView::applyCompletedStudyDetails(const CandleCompression& study,const CandleStudySet& studies){
    auto* state=findChild<QLabel*>("marketCandlesStudyState");auto* calculations=findChild<QLabel*>("marketCandlesStudyDetails");
    study_->setText(!study.available?tr("변동성 압축 · %1").arg(study.reason):study.compressed?tr("마지막 완료 구간: 변동성 압축 상태 · 방향은 알 수 없음"):tr("마지막 완료 구간: 변동성 압축 상태 아님"));
    if(!studies.available){state->setText(studies.reason);calculations->clear();return;}
    const auto number=[](double n){return QString::number(n,'f',2);};
    const auto sweep=studies.sweptHigh&&studies.sweptLow?tr("이전 고점·저점 돌파 후 복귀"):studies.sweptHigh?tr("이전 고점 돌파 후 복귀"):studies.sweptLow?tr("이전 저점 돌파 후 복귀"):tr("이전 범위 돌파 후 복귀 없음");
    state->setText(sweep+"\n"+(studies.adxAvailable?tr("ADX(14) %1 · %2").arg(number(studies.adx),studies.regime):tr("ADX · 연속 완료 봉 28개 필요")));
    QStringList lines;
    lines<<tr("최근 연속 봉 %1개 · 이전 20봉의 고가/저가를 마지막 봉이 넘은 뒤 종가가 복귀했는지 관측합니다.").arg(studies.observations);
    lines<<tr("현재가 주변 가격대 %1/4 · %2").arg(studies.overlap).arg(studies.nearbyLevels.isEmpty()?tr("겹침 없음"):studies.nearbyLevels.join(" · "));
    lines<<tr("근접 기준: Wilder ATR20의 0.5배 이내. SMA20·BB 하단·거래량 대표가격·저가 회귀선20. 투자 점수가 아닙니다.");
    if(studies.profileAvailable)lines<<tr("거래량 대표가격 근사 %1~%2 %3 · 최근 %4봉 · 거래량 %5% 집중").arg(number(studies.profileLow),number(studies.profileHigh),currency_).arg(studies.profileBars).arg(number(studies.profileShare));
    else lines<<tr("거래량 대표가격 · 거래량 자료 부족");
    lines<<tr("최근 최대 96봉의 (고가+저가+종가)/3에 해당 봉 거래량을 배정한 20구간 근사입니다. 실제 가격별 체결 분포·빈 가격대를 뜻하지 않습니다.");
    lines<<tr("거래량 대비 좁은 변동 · %1").arg(studies.observations<22?tr("연속 완료 봉 22개 필요"):studies.narrowHighVolume?tr("최근 봉에서 관측"):tr("조건 충족 없음"));
    lines<<tr("이전 20봉 거래량 중앙값의 2배 이상, true range는 이전 평균의 0.5배 이하. 실제 매수·매도 흡수를 확정하지 않습니다.");
    for(const auto& trend:studies.trends)lines<<tr("%1 추세 · %2 · 실제 완료 봉 %3개").arg(timeframeLabel(trend.minutes*60000LL))
        .arg(trend.available?(trend.direction>0?tr("이평20 상승"):trend.direction<0?tr("이평20 하락"):tr("이평20 보합")):tr("이평 비교에 완료 봉 21개 필요")).arg(trend.bars);
    lines<<tr("상위 시간봉은 UTC 경계에 맞는 완료 %1봉만 합칩니다. 진행 중인 시간봉과 공백은 채우지 않습니다.").arg(timeframeLabel(intervalMs_));
    for(const auto& session:studies.sessions)lines<<session.label+tr(" KST · 표본 %1봉 · ").arg(session.bars)+
        (session.bars>=8?tr("평균 %1% · 평균 절대변동 %2%").arg(number(session.meanPercent),number(session.meanAbsolutePercent)):tr("최소 8봉 필요"));
    if(!studies.sessions.isEmpty())lines<<tr("시간대 통계는 현재 수집 구간의 봉 시가→종가 변화이며 장기 계절성이나 예측이 아닙니다.");
    const auto& similar=studySnapshot_.similarity;
    lines<<(similar.available?tr("과거 닮은 구간 · 표본 %1개 · 최소 유사도(상관) %2 · 각 경로 상관은 닮은꼴 범례에서 확인").arg(similar.samples).arg(QString::number(ChartSimilarityMinimumCorrelation,'f',2)):
        (similar.reason.isEmpty()?tr("완료 봉과 비교 가능한 과거 구간을 기다립니다."):similar.reason));
    lines<<tr("최근 20봉과 겹치지 않고 후속 20봉도 완료된 과거 구간의 로그수익률 상관입니다. 최소 상관 기준을 통과한 경로만 표시하며 이후 수익을 예측하지 않습니다.");
    lines<<tr("ADX(14)는 Wilder 평활이며 20 미만/20~25/25 이상을 약함/중간/강함으로 표시합니다. 방향은 +DI/−DI와 별개이며 매매 신호가 아닙니다.");
    calculations->setText(lines.join("\n\n"));
}
QString MarketCandlesView::chartStyle()const{
    return indicatorPreferences_?indicatorPreferences_->chartStyle():static_cast<CandleChart*>(chart_)->chartStyle();
}
void MarketCandlesView::setChartStyle(const QString& style){
    if(!ChartStudyPreferences::isChartStyle(style))return;
    if(indicatorPreferences_)indicatorPreferences_->setChartStyle(style);
    static_cast<CandleChart*>(chart_)->setChartStyle(style);refreshChartStyleButton();
}
double MarketCandlesView::priceScale()const{return static_cast<CandleChart*>(chart_)->priceScale();}
void MarketCandlesView::refreshChartStyleButton(){
    if(!chartStyleButton_)return;const auto style=chartStyle();
    const auto name=style=="bars"?tr("바"):style=="line"?tr("선"):tr("캔들");
    // Refresh runs on every quote; rebuild the icon only when the style or theme ink changes.
    const auto key=style+themeColors().text2.name();if(key!=chartStyleIconKey_){chartStyleIconKey_=key;chartStyleButton_->setIcon(chartLineIcon("style-"+style,themeColors().text2));}
    chartStyleButton_->setAccessibleName(tr("차트 종류 %1 · 바꾸기").arg(name));chartStyleButton_->setToolTip(tr("차트 종류: %1\n캔들 · 바 · 선 중에서 고를 수 있어요").arg(name));
}
void MarketCandlesView::undoDrawing(bool redo){
    auto* chart=static_cast<CandleChart*>(chart_);
    // An unfinished two-point line or drag is cancelled first, as Esc does.
    if(!redo&&chart->drawingInProgress()){chart->cancelDrawingGesture();return;}
    if(!drawingModel_)return;
    const bool done=redo?drawingModel_->redo():drawingModel_->undo();
    if(done){drawingStatus_->setText(redo?tr("그리기를 다시 실행했어요."):tr("그리기를 되돌렸어요."));refreshDrawingUi();}
}

} // namespace keefi
