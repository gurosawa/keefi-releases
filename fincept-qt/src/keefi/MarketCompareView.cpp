#include "keefi/MarketCompareView.h"
#include "keefi/MarketComparePeriod.h"
#include "keefi/SegmentedControlStyle.h"
#include "keefi/LocalComboBox.h"
#include "keefi/InfoButton.h"
#include "keefi/PlainToolTip.h"

#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextDocumentFragment>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace keefi {
namespace {
constexpr qint64 Minute = 60000, Hour = 3600000, Day = 86400000;
constexpr auto IntervalPreference = "marketCompare/intervalMs/v1";
constexpr auto PeriodPreference = "marketCompare/period/v1";
constexpr auto AutomaticIntervalPreference = "marketCompare/automaticInterval/v1";
constexpr auto CorrelationPreference = "marketCompare/correlationExpanded/v1";
constexpr auto HiddenSeriesPreference = "marketCompare/hiddenSeries/v1";

struct PeriodPlan {
    const char* id;
    const char* label;
    qint64 duration, interval;
};
constexpr std::array<PeriodPlan,4> PeriodPlans{{
    {"1d", "1일", Day, 15*Minute}, {"1w", "1주", 7*Day, Hour},
    {"1m", "1달", 30*Day, 4*Hour}, {"3m", "3달", 90*Day, Day}
}};

const PeriodPlan* periodPlan(const QString& id) {
    for(const auto& plan:PeriodPlans)if(id==QLatin1String(plan.id))return &plan;
    return nullptr;
}

ComparePeriodWindow periodWindow(const MarketCompareSnapshot& snapshot,const PeriodPlan& plan) {
    if(snapshot.intervalMs<=0||snapshot.updatedAtMs<=snapshot.intervalMs)return {};
    // Always ask for the current completed slot. A stale last page must not
    // silently shorten the requested period or substitute its last available bar.
    const auto end=snapshot.updatedAtMs/snapshot.intervalMs*snapshot.intervalMs-snapshot.intervalMs;
    if(end<plan.duration)return {};
    return {end-plan.duration,end,snapshot.updatedAtMs};
}

struct PeriodColumn {
    ComparePeriodWindow window;
    QList<ComparePeriodReturn> values;
    bool ready = false;
};

PeriodColumn periodColumn(const MarketCompareSnapshot& snapshot, const PeriodPlan& plan) {
    PeriodColumn column;
    column.window = periodWindow(snapshot, plan);
    column.ready = snapshot.series.size() == 4;
    for (const auto& series : snapshot.series) {
        ComparePeriodReturn value;
        if (series.intervalMs == snapshot.intervalMs)
            value = marketComparePeriodReturn(series, column.window, snapshot.provisionalFxOpenMs);
        else
            value.coverage = ComparePeriodCoverage::InvalidSeries;
        column.ready = column.ready && value.coverage == ComparePeriodCoverage::Ready;
        column.values.append(value);
    }
    return column;
}

bool periodSelectable(const MarketCompareSnapshot& snapshot, const PeriodPlan& plan, bool ready) {
    if (snapshot.loading) return false;
    if (ready) return true;
    // A short page at another interval does not prove the target period is
    // unavailable. Permit one explicit interval change, then validate its real
    // completed bars. Leave room for a still-forming bar in the 200-bar page.
    return snapshot.intervalMs!=plan.interval&&MarketCompare::supportedInterval(plan.interval)&&
        plan.duration%plan.interval==0&&plan.duration/plan.interval+2<=MarketCompare::DefaultLimit;
}

QString periodDescription(const PeriodPlan& plan) {
    return QStringLiteral("최근 %1일의 완료 봉 · 1달=30일, 3달=90일").arg(plan.duration/Day);
}

QString barIntervalText(qint64 interval){
    if(!MarketCompare::supportedInterval(interval))return QStringLiteral("봉 간격 확인 필요");
    if(interval>=Day)return QStringLiteral("%1일봉").arg(interval/Day);
    if(interval>=Hour)return QStringLiteral("%1시간봉").arg(interval/Hour);
    return QStringLiteral("%1분봉").arg(interval/Minute);
}

struct CompareLine {
    QString identity, name;
    QColor color;
    Qt::PenStyle style = Qt::SolidLine;
    QList<ComparePercentPoint> points;
    QStringList notes; // Optional, aligned with points: the FX posting a converted bar used.
    bool base = false;
};

struct TimeSpan {
    qint64 fromMs = 0, toMs = 0;
};

bool isBitcoin(const QString& identity) {
    return identity == QLatin1String("coin:binance:BTCUSDT") || identity == QLatin1String("coin:upbit:KRW-BTC");
}

QString seriesKey(const QString& identity) {
    return isBitcoin(identity) ? QStringLiteral("coin:binance:BTCUSDT") : identity;
}

QString shortName(const QString& identity, const QString& fallback = {}) {
    if (isBitcoin(identity)) return QStringLiteral("BTC");
    if (identity == QLatin1String("perp:binance-usdm:XAUUSDT")) return QStringLiteral("금");
    if (identity == QLatin1String("perp:binance-usdm:CLUSDT")) return QStringLiteral("원유");
    if (identity == QLatin1String("perp:binance-usdm:QQQUSDT")) return QStringLiteral("나스닥");
    return fallback;
}

// Only the closed face is abbreviated. LocalComboBox keeps the original model,
// popup rows, keyboard selection and full-label tooltip behaviour.
class CompareSelector final : public LocalComboBox {
  public:
    enum class Kind { Base, Won };
    CompareSelector(Kind kind, QWidget* parent) : LocalComboBox(parent), kind_(kind) {
        setCompact();setFixedHeight(24);setFont(KeefiFonts::font(FontRole::Caption12));
        setMinimumWidth(0);setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    }
    QSize sizeHint() const override {
        return QSize(fontMetrics().horizontalAdvance(closedText())+42,24);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
  protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter stylePainter(this);QStyleOptionComboBox option;initStyleOption(&option);
        stylePainter.drawComplexControl(QStyle::CC_ComboBox,option);
        option.currentText=closedText();stylePainter.drawControl(QStyle::CE_ComboBoxLabel,option);
        stylePainter.setRenderHint(QPainter::Antialiasing);
        stylePainter.setPen(QPen(isEnabled()?themeColors().accent:themeColors().text3,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        stylePainter.setBrush(Qt::NoBrush);
        const qreal x=width()-13,y=height()/2.0,direction=view()->isVisible()?-1:1;
        stylePainter.drawPolyline(QPolygonF{{x-4,y-2*direction},{x,y+2*direction},{x+4,y-2*direction}});
    }
  private:
    QString closedText() const {
        if(kind_==Kind::Base)return QStringLiteral("기준 ")+shortName(currentData().toString(),currentText());
        switch(CompareWonMode(currentData().toInt())){
        case CompareWonMode::Dollar:return QStringLiteral("BTC 달러");
        case CompareWonMode::WonRaw:return QStringLiteral("BTC 원화");
        case CompareWonMode::WonToDollar:return QStringLiteral("BTC 달러 환산");
        }
        return currentText();
    }
    Kind kind_;
};

class CompareSingleLineLabel final : public QLabel {
  public:
    explicit CompareSingleLineLabel(QWidget* parent) : QLabel(parent) {
        setTextFormat(Qt::PlainText);setWordWrap(false);setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.setFont(font());painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(),alignment()|Qt::TextSingleLine,fontMetrics().elidedText(text(),Qt::ElideRight,contentsRect().width()));
    }
};

struct SummaryEntry {
    QString name, relation;
    double change = 0;
    bool base = false;
};

QColor colorFor(const CompareSpec& spec, int index) {
    const auto& c = themeColors();
    if (spec.symbol == QLatin1String("XAUUSDT")) return c.studyOrange;
    if (spec.symbol == QLatin1String("CLUSDT")) return c.studyPurple;
    if (spec.symbol == QLatin1String("QQQUSDT")) return c.studyBlue;
    if (spec.symbol.contains(QLatin1String("BTC"))) return c.text; // The base line reads strongest.
    const QList<QColor> others{c.level, c.up, c.down, c.text2};
    return others[index % others.size()];
}

Qt::PenStyle styleFor(int index) {
    static const QList<Qt::PenStyle> styles{Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine, Qt::DashDotDotLine};
    return styles[index % styles.size()];
}

QString percentText(double value) {
    return (value > 0 ? QStringLiteral("+") : QString{}) + QString::number(value, 'f', 2) + QStringLiteral("%");
}

QString timeText(qint64 openMs, qint64 intervalMs) {
    const auto at = QDateTime::fromMSecsSinceEpoch(openMs, QTimeZone("Asia/Seoul"));
    return intervalMs >= Day ? at.toString(QStringLiteral("yy.MM.dd")) : at.toString(QStringLiteral("MM.dd HH:mm"));
}

// Crosshair time: the weekday tells a weekend gap from a trading day at a glance.
QString hoverTimeText(qint64 openMs, qint64 intervalMs) {
    static const QStringList days{QStringLiteral("월"), QStringLiteral("화"), QStringLiteral("수"), QStringLiteral("목"),
                                  QStringLiteral("금"), QStringLiteral("토"), QStringLiteral("일")};
    const auto at = QDateTime::fromMSecsSinceEpoch(openMs, QTimeZone("Asia/Seoul"));
    const auto day = QStringLiteral(" (%1)").arg(days.value(at.date().dayOfWeek() - 1));
    return intervalMs >= Day ? at.toString(QStringLiteral("yy.MM.dd")) + day
                             : at.toString(QStringLiteral("MM.dd")) + day + at.toString(QStringLiteral(" HH:mm"));
}

QString signedTwo(std::optional<double> value) {
    if (!value) return QStringLiteral("—");
    return (*value > 0 ? QStringLiteral("+") : QString{}) + QString::number(*value, 'f', 2);
}

QString comparisonSummary(const QList<SummaryEntry>& entries) {
    if(entries.size()!=4)return {};
    const auto lowest=std::min_element(entries.cbegin(),entries.cend(),[](const auto& a,const auto& b){return a.change<b.change;});
    const auto highest=std::max_element(entries.cbegin(),entries.cend(),[](const auto& a,const auto& b){return a.change<b.change;});
    QString text;
    if(lowest->change==0&&highest->change==0)text=QStringLiteral("기준 시점과 비교해 넷 다 변동이 없어요.");
    else if(lowest->change>0)text=QStringLiteral("넷 다 올랐어요. 가장 큰 상승은 %1(%2), 가장 작은 상승은 %3(%4)이에요.")
        .arg(highest->name,percentText(highest->change),lowest->name,percentText(lowest->change));
    else if(highest->change<0)text=QStringLiteral("넷 다 내렸어요. 가장 큰 하락은 %1(%2), 가장 작은 하락은 %3(%4)이에요.")
        .arg(lowest->name,percentText(lowest->change),highest->name,percentText(highest->change));
    else if(highest->change>0&&lowest->change<0)text=QStringLiteral("기준 시점부터 %1이 가장 많이 올랐고(%2), %3이 가장 많이 내렸어요(%4).")
        .arg(highest->name,percentText(highest->change),lowest->name,percentText(lowest->change));
    else if(highest->change>0)text=QStringLiteral("기준 시점부터 %1이 가장 많이 올랐고(%2), %3은 변동이 없어요(%4).")
        .arg(highest->name,percentText(highest->change),lowest->name,percentText(lowest->change));
    else text=QStringLiteral("기준 시점부터 %1은 변동이 없고(%2), %3이 가장 많이 내렸어요(%4).")
        .arg(highest->name,percentText(highest->change),lowest->name,percentText(lowest->change));
    QString baseName;QStringList opposite;
    for(const auto& entry:entries){if(entry.base)baseName=entry.name;else if(entry.relation==QStringLiteral("반대로 움직임"))opposite.append(entry.name);}
    if(!baseName.isEmpty()&&!opposite.isEmpty())text+=QStringLiteral(" %1은 %2와 반대로 움직였어요.").arg(opposite.join(QStringLiteral("·")),baseName);
    return text;
}

QString periodCoverageText(ComparePeriodCoverage coverage) {
    switch(coverage){
    case ComparePeriodCoverage::Ready:return QStringLiteral("완료 봉으로 계산했어요");
    case ComparePeriodCoverage::Unavailable:return QStringLiteral("자료가 아직 모자라요");
    case ComparePeriodCoverage::MissingStart:return QStringLiteral("기간 시작 봉이 없어요");
    case ComparePeriodCoverage::MissingEnd:return QStringLiteral("기간 끝의 완료 봉이 없어요");
    case ComparePeriodCoverage::Gap:return QStringLiteral("기간 안에 빠진 봉이 있어요");
    case ComparePeriodCoverage::NotClosed:return QStringLiteral("아직 완료되지 않은 봉이 있어요");
    case ComparePeriodCoverage::InvalidWindow:return QStringLiteral("기간과 봉 간격을 확인할 수 없어요");
    case ComparePeriodCoverage::InvalidSeries:return QStringLiteral("봉 자료를 확인할 수 없어요");
    }
    return {};
}

QString periodReturnTooltip(const CompareSeries& series,const PeriodPlan& plan,
                            const ComparePeriodWindow& window,const ComparePeriodReturn& value) {
    QStringList rows{series.spec.name+QStringLiteral(" · ")+QString::fromUtf8(plan.label),periodDescription(plan)};
    if(window.endOpenMs>window.startOpenMs){
        const auto stamp=[](qint64 at){return QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString(QStringLiteral("yyyy.MM.dd HH:mm"));};
        rows.append(QStringLiteral("시작 봉 %1 → 끝 봉 %2 KST").arg(stamp(window.startOpenMs),stamp(window.endOpenMs)));
    }
    rows.append(periodCoverageText(value.coverage));
    if(value.percent)rows.append(QStringLiteral("기준 대비 %1% · %2").arg(QString::number(*value.percent,'g',17),series.spec.currency));
    rows.append((value.coverage==ComparePeriodCoverage::Ready?QStringLiteral("완료 봉 %1 / %2"):QStringLiteral("기간 안 봉 %1 / %2"))
        .arg(value.presentBars).arg(value.expectedBars));
    if(!series.fx.isEmpty())rows.append(QStringLiteral("환율: 잠정 %1봉 · 근사 %2봉 · 직전 고시 %3봉")
        .arg(value.provisionalFxBars).arg(value.approximateFxBars).arg(value.previousFxBars));
    if(!series.reason.isEmpty())rows.append(series.reason);
    return rows.join('\n');
}

class ComparePeriodHeader final : public QHeaderView {
  public:
    explicit ComparePeriodHeader(QWidget* parent):QHeaderView(Qt::Horizontal,parent){
        setSectionsClickable(true);setHighlightSections(false);setFixedHeight(26);
        setFont(KeefiFonts::font(FontRole::Caption12));
    }
    void setPeriodColumn(int column){if(column_!=column){column_=column;viewport()->update();}}
  protected:
    void paintSection(QPainter* painter,const QRect& rect,int section) const override {
        if(!rect.isValid())return;
        const auto& c=themeColors();
        painter->save();painter->fillRect(rect,section==column_?c.surface:c.bg);
        painter->setFont(font());painter->setPen(c.text3);
        const int alignment=(section>=1&&section<=4?Qt::AlignRight:Qt::AlignLeft)|Qt::AlignVCenter;
        painter->drawText(rect.adjusted(8,0,-8,0),alignment,
            model()->headerData(section,Qt::Horizontal,Qt::DisplayRole).toString());
        painter->setPen(c.rule);painter->drawLine(rect.bottomLeft(),rect.bottomRight());painter->restore();
    }
  private:
    int column_=-1;
};

// The period column background is part of the presentation, independent of
// item-box stylesheet painting. Draw the same model text and brush explicitly.
class CompareReturnDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        auto cell=option;initStyleOption(&cell,index);
        painter->save();
        painter->fillRect(option.rect,cell.backgroundBrush);
        painter->setFont(cell.font);painter->setPen(cell.palette.color(QPalette::Text));
        const auto textRect=option.rect.adjusted(8,0,-8,0);
        painter->drawText(textRect,int(cell.displayAlignment)|Qt::TextSingleLine,
                         cell.fontMetrics.elidedText(cell.text,cell.textElideMode,textRect.width()));
        painter->setPen(themeColors().rule);painter->drawLine(option.rect.bottomLeft(),option.rect.bottomRight());
        painter->restore();
    }
};

// The model keeps the visible relation wording. This role supplies its existing
// r20 only to the five-part drawing; it does not classify the relationship.
constexpr int RelationValueRole=Qt::UserRole+1;
class CompareRelationDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        if(index.data().toString()==QStringLiteral("기준")){
            QStyledItemDelegate::paint(painter,option,index);return;
        }
        painter->save();
        const auto& c=themeColors();
        painter->fillRect(option.rect,index.data(Qt::BackgroundRole).value<QBrush>());
        auto textOption=option;textOption.rect.adjust(0,0,-72,0);
        QStyledItemDelegate::paint(painter,textOption,index);
        const auto value=index.data(RelationValueRole);
        const double strength=value.isValid()?std::clamp(std::abs(value.toDouble()),0.0,1.0):0.0;
        constexpr qreal barWidth=8,gap=3,barHeight=10;
        const qreal left=option.rect.right()-8-(5*barWidth+4*gap),top=option.rect.center().y()-barHeight/2;
        painter->setPen(Qt::NoPen);
        for(int bar=0;bar<5;++bar){
            const QRectF cell(left+bar*(barWidth+gap),top,barWidth,barHeight);
            painter->fillRect(cell,c.rule);
            // Partial fills preserve the scalar instead of adding new thresholds.
            const double fraction=std::clamp(strength*5-bar,0.0,1.0);
            if(fraction>0)painter->fillRect(QRectF(cell.left(),cell.top(),cell.width()*fraction,cell.height()),c.text3);
        }
        painter->setPen(c.rule);painter->drawLine(option.rect.bottomLeft(),option.rect.bottomRight());
        painter->restore();
    }
};
} // namespace

// Percent lines on one shared axis. Click sets the base bar; double-click resets it.
class CompareChartWidget final : public QWidget {
  public:
    std::function<void(qint64)> baseChosen;
    explicit CompareChartWidget(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("marketCompareChart"));
        setMouseTracking(true);
        setMinimumHeight(260);
        setAccessibleName(QStringLiteral("시장 비교 차트"));
    }
    void setControls(QWidget* chips, QWidget* selectors) {
        chipsBar_=chips;selectorsBar_=selectors;layoutControls();
    }
    void layoutControls() {
        if(!chipsBar_||!selectorsBar_)return;
        const int available=std::max(0,width()-16);
        const auto chips=chipsBar_->sizeHint(),selectors=selectorsBar_->sizeHint();
        const bool sameRow=chips.width()+selectors.width()+12<=available;
        chipsBar_->setGeometry(8,8,std::min(chips.width(),available),28);
        selectorsBar_->setGeometry(std::max(8,width()-8-selectors.width()),sameRow?10:44,
                                  std::min(selectors.width(),available),24);
        controlsBottom_=sameRow?36:68;
        update();
    }
    void setData(QList<CompareLine> lines, QList<TimeSpan> weekends, qint64 intervalMs, const QSet<QString>& hidden) {
        lines_ = std::move(lines);
        weekends_ = std::move(weekends);
        intervalMs_ = intervalMs;
        hidden_=hidden;
        // A data refresh must not erase the crosshair under a resting pointer.
        const qint64 hovered = pointerInside_ && hover_ >= 0 && hover_ < times_.size() ? times_[hover_] : 0;
        times_.clear();
        QSet<qint64> seen;
        for (const auto& line : lines_)
            for (const auto& point : line.points)
                if (!seen.contains(point.openMs)) { seen.insert(point.openMs); times_.append(point.openMs); }
        std::sort(times_.begin(), times_.end());
        hover_ = -1;
        if (hovered) {
            const auto it = std::lower_bound(times_.cbegin(), times_.cend(), hovered);
            hover_ = it != times_.cend() && *it == hovered ? int(it - times_.cbegin()) : nearest(pointerX_);
        }
        setAccessibleDescription(hover_ >= 0 ? hoverRows(times_[hover_]).join('\n') : QString{});
        layoutControls();
        update();
    }
    qint64 firstMs() const { return times_.isEmpty() ? 0 : times_.first(); }
    qint64 lastMs() const { return times_.isEmpty() ? 0 : times_.last(); }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& c = themeColors();
        p.fillRect(rect(), c.surface);
        const auto area=plotArea();
        p.setFont(KeefiFonts::font(FontRole::Caption12));
        if (times_.size() < 2) {
            p.setPen(c.text2);
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("같은 시각의 완료 봉이 아직 없어요"));
            return;
        }
        double low = 0, high = 0;
        for (const auto& line : lines_)
            for (const auto& point : line.points) { low = std::min(low, point.percent); high = std::max(high, point.percent); }
        const double pad = std::max(0.2, (high - low) * 0.08);
        low -= pad;
        high += pad;
        const auto x = [&](qint64 at) { return area.left() + area.width() * double(at - times_.first()) / double(times_.last() - times_.first()); };
        const auto y = [&](double value) { return area.bottom() - area.height() * (value - low) / (high - low); };
        auto shade = c.text3;
        shade.setAlpha(26);
        for (const auto& span : weekends_) {
            const auto left = std::max(area.left(), x(span.fromMs)), right = std::min(area.right(), x(span.toMs));
            if (right > left) p.fillRect(QRectF(left, area.top(), right - left, area.height()), shade);
        }
        const double step = std::pow(10.0, std::floor(std::log10((high - low) / 4.0)));
        const double nice = (high - low) / step > 20 ? step * 5 : (high - low) / step > 8 ? step * 2 : step;
        p.setPen(QPen(c.grid, 1));
        for (double v = std::ceil(low / nice) * nice; v <= high; v += nice) {
            p.drawLine(QPointF(area.left(), y(v)), QPointF(area.right(), y(v)));
            p.save();
            p.setPen(c.text2);
            p.setFont(KeefiFonts::font(FontRole::Number11));
            p.drawText(QRectF(area.right() + 6, y(v) - 8, 60, 16), Qt::AlignLeft | Qt::AlignVCenter, percentText(v));
            p.restore();
        }
        p.setPen(QPen(c.text3, 1, Qt::DashLine));
        p.drawLine(QPointF(area.left(), y(0)), QPointF(area.right(), y(0)));
        p.setPen(c.text2);
        const int ticks = 4;
        for (int i = 0; i <= ticks; ++i) {
            const auto at = times_[std::min<int>(times_.size() - 1, i * (times_.size() - 1) / ticks)];
            const auto label = timeText(at, intervalMs_);
            const auto left = std::clamp(x(at) - 40, area.left(), area.right() - 80);
            p.drawText(QRectF(left, area.bottom() + 4, 80, 16), Qt::AlignCenter, label);
        }
        for (const auto& line : lines_) {
            // Visibility is a paint choice only. All lines still define the shared
            // time/value axes, base bar, hover values and correlation calculations.
            if (line.points.size() < 2 || (!line.base&&hidden_.contains(seriesKey(line.identity)))) continue;
            QPainterPath path;
            path.moveTo(x(line.points.first().openMs), y(line.points.first().percent));
            for (int i = 1; i < line.points.size(); ++i) {
                const auto& point = line.points[i];
                const QPointF at(x(point.openMs), y(point.percent));
                if (point.openMs - line.points[i - 1].openMs == intervalMs_)
                    path.lineTo(at);
                else
                    path.moveTo(at); // Missing prices or FX remain a gap in the displayed line.
            }
            p.setPen(QPen(line.color, line.base ? 2.6 : 1.8, line.style, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        if (hover_ >= 0 && hover_ < times_.size()) {
            const auto at = times_[hover_];
            p.setPen(QPen(c.text3, 1, Qt::DotLine));
            p.drawLine(QPointF(x(at), area.top()), QPointF(x(at), area.bottom()));
            const auto groups=hoverGroups(at);
            const auto rows=hoverRows(at);
            int textWidth=0;
            for(const auto& row:rows)textWidth=std::max(textWidth,p.fontMetrics().horizontalAdvance(row));
            const qreal boxWidth=std::min(area.width(),std::max(280,textWidth+46)*1.0);
            const auto box = QRectF(std::clamp(x(at)+10,area.left(),std::max(area.left(),area.right()-boxWidth)),
                                    area.top()+6,boxWidth,18.0*rows.size()+8);
            p.setPen(c.rule);
            p.setBrush(c.bg);
            p.drawRoundedRect(box, 6, 6);
            p.setPen(c.text);
            p.drawText(QRectF(box.left()+8,box.top()+4,box.width()-16,18),Qt::AlignLeft|Qt::AlignVCenter,rows.first());
            int row=1;
            for(const auto& group:groups){
                const qreal rowTop=box.top()+4+18*row++;
                p.setPen(QPen(group.color,1.8,group.style,Qt::RoundCap));
                p.drawLine(QPointF(box.left()+8,rowTop+9),QPointF(box.left()+28,rowTop+9));
                p.setPen(c.text);
                const auto label=group.name+QStringLiteral("  ")+percentText(group.value);
                p.drawText(QRectF(box.left()+36,rowTop,box.width()-44,18),Qt::AlignLeft|Qt::AlignVCenter,
                           p.fontMetrics().elidedText(label,Qt::ElideRight,int(box.width()-44)));
                if(!group.note.isEmpty()){
                    p.setPen(c.text2);
                    p.drawText(QRectF(box.left()+36,box.top()+4+18*row++,box.width()-44,18),Qt::AlignLeft|Qt::AlignVCenter,
                               p.fontMetrics().elidedText(group.note,Qt::ElideRight,int(box.width()-44)));
                }
            }
            // The same time on the axis, so the eye does not have to travel to the box.
            const auto label = rows.first();
            const auto labelWidth = p.fontMetrics().horizontalAdvance(label) + 12.0;
            const QRectF badge(std::clamp(x(at) - labelWidth / 2, area.left(), std::max(area.left(), area.right() - labelWidth)),
                               area.bottom() + 3, labelWidth, 18);
            p.fillRect(badge, c.text);
            p.setPen(c.bg);
            p.drawText(badge, Qt::AlignCenter, label);
        }
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);layoutControls();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        pointerInside_ = true;
        pointerX_ = event->position().x();
        hover_ = nearest(event->position().x());
        setAccessibleDescription(hover_ >= 0 ? hoverRows(times_[hover_]).join('\n') : QString{});
        update();
    }
    void leaveEvent(QEvent*) override { pointerInside_ = false; hover_ = -1; setAccessibleDescription({}); update(); }
    void mousePressEvent(QMouseEvent* event) override {
        const auto index = nearest(event->position().x());
        if (index >= 0 && baseChosen) baseChosen(times_[index]);
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        if (baseChosen) baseChosen(0);
    }

  private:
    struct HoverGroup {
        double value;
        QString name,note;
        QColor color;
        Qt::PenStyle style;
    };
    QList<HoverGroup> hoverGroups(qint64 at) const {
        QList<HoverGroup> groups;
        for(const auto& line:lines_)
            for(int i=0;i<line.points.size();++i)
                if(line.points[i].openMs==at){
                    groups.append({line.points[i].percent,line.name,line.notes.value(i),line.color,line.style});
                    break;
                }
        std::stable_sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.value>b.value;});
        return groups;
    }
    QStringList hoverRows(qint64 at) const {
        QStringList rows{hoverTimeText(at, intervalMs_)};
        for(const auto& group:hoverGroups(at)){
            rows.append(group.name+QStringLiteral("  ")+percentText(group.value));
            if(!group.note.isEmpty())rows.append(QStringLiteral("  ")+group.note);
        }
        return rows;
    }
    QRectF plotArea() const {
        const int top=controlsBottom_+8;
        return QRectF(8,top,std::max(1,width()-72),std::max(1,height()-24-top));
    }
    int nearest(double px) const {
        if (times_.size() < 2) return -1;
        const auto area=plotArea();
        const auto at = times_.first() + qint64((px - area.left()) / area.width() * double(times_.last() - times_.first()));
        const auto it = std::lower_bound(times_.cbegin(), times_.cend(), at);
        int index = int(it - times_.cbegin());
        if (index >= times_.size()) index = times_.size() - 1;
        if (index > 0 && std::abs(times_[index - 1] - at) < std::abs(times_[index] - at)) --index;
        return index;
    }
    QList<CompareLine> lines_;
    QList<TimeSpan> weekends_;
    QList<qint64> times_;
    QSet<QString> hidden_;
    QWidget* chipsBar_=nullptr;
    QWidget* selectorsBar_=nullptr;
    int controlsBottom_=36;
    qint64 intervalMs_ = Hour;
    int hover_ = -1;
    bool pointerInside_ = false;
    double pointerX_ = 0;
};

// Rolling 20/60-bar correlation of the base and the chosen target.
class CompareCorrelationWidget final : public QWidget {
  public:
    explicit CompareCorrelationWidget(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("marketCompareCorrelation"));
        setMinimumHeight(118);
        setAccessibleName(QStringLiteral("연동 정도"));
    }
    void setData(QList<CorrelationPoint> points, QList<RelationChange> changes, qint64 fromMs, qint64 toMs, QString title) {
        points_ = std::move(points);
        changes_ = std::move(changes);
        fromMs_ = fromMs;
        toMs_ = toMs;
        title_ = std::move(title);
        update();
    }
    QList<qint64> relationChangeOpens() const {
        QList<qint64> opens;
        for (const auto& change : changes_) opens.append(change.openMs);
        return opens;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& c = themeColors();
        p.fillRect(rect(), c.surface);
        p.setFont(KeefiFonts::font(FontRole::Caption12));
        p.setPen(c.text2);
        p.drawText(QRectF(10, 2, width() - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, title_);
        const QRectF area(8, 22, width() - 72, height() - 30);
        if (toMs_ <= fromMs_) return;
        const auto x = [&](qint64 at) { return area.left() + area.width() * double(at - fromMs_) / double(toMs_ - fromMs_); };
        const auto y = [&](double r) { return area.top() + area.height() * (1.0 - (r + 1.0) / 2.0); };
        for (const double guide : {1.0, 0.5, 0.0, -0.5, -1.0}) {
            p.setPen(QPen(guide == 0.0 ? c.text3 : c.grid, 1, guide == 0.0 ? Qt::SolidLine : Qt::DotLine));
            p.drawLine(QPointF(area.left(), y(guide)), QPointF(area.right(), y(guide)));
            p.setPen(c.text2);
            p.setFont(KeefiFonts::font(FontRole::Number11));
            p.drawText(QRectF(area.right() + 6, y(guide) - 8, 60, 16), Qt::AlignLeft | Qt::AlignVCenter, signedTwo(guide));
        }
        const auto draw = [&](bool sixty, const QColor& color, Qt::PenStyle style) {
            QPainterPath path;
            bool open = false;
            for (const auto& point : points_) {
                const auto value = sixty ? point.r60 : point.r20;
                if (!value || point.openMs < fromMs_) { open = false; continue; }
                const QPointF at(x(point.openMs), y(*value));
                if (!open) path.moveTo(at), open = true;
                else path.lineTo(at);
            }
            p.setPen(QPen(color, sixty ? 1.4 : 2.0, style));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        };
        draw(true, c.text2, Qt::DashLine);
        draw(false, c.accent, Qt::SolidLine);
        p.setPen(Qt::NoPen);
        p.setBrush(c.danger);
        for (const auto& change : changes_) {
            if (change.openMs < fromMs_) continue;
            const QPointF at(x(change.openMs), y(change.r20));
            QPolygonF diamond;
            diamond << at + QPointF(0, -6) << at + QPointF(6, 0) << at + QPointF(0, 6) << at + QPointF(-6, 0);
            p.drawPolygon(diamond);
        }
    }

  private:
    QList<CorrelationPoint> points_;
    QList<RelationChange> changes_;
    qint64 fromMs_ = 0, toMs_ = 0;
    QString title_;
};

MarketCompareView::MarketCompareView(MarketCompareSource* source, QWidget* parent) : QWidget(parent), source_(source) {
    setObjectName(QStringLiteral("marketCompareView"));
    setAccessibleName(QStringLiteral("시장 비교"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("marketCompareScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFocusPolicy(Qt::NoFocus);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll);

    // Keep each chart's minimum plot area in short windows.
    auto* content = new QWidget;
    auto* root = new QVBoxLayout(content);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);
    scroll->setWidget(content);

    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("시장 비교"), this);
    title->setFont(KeefiFonts::font(FontRole::Title17));
    information_=new InfoButton(this);
    information_->setObjectName(QStringLiteral("marketCompareInformation"));
    information_->setFixedSize(28,28);
    header->addWidget(title);
    header->addWidget(information_);
    header->addStretch();

    auto* periodLabel=new QLabel(QStringLiteral("기간"),this);
    periodLabel->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(periodLabel,QStringLiteral("color:${text3};"));
    header->addWidget(periodLabel);
    for(const auto& plan:PeriodPlans){
        auto* button=new QPushButton(QString::fromUtf8(plan.label),this);
        button->setObjectName(QStringLiteral("marketComparePeriod_")+QLatin1String(plan.id));
        button->setCheckable(true);
        applySegmentedControlStyle(button,CompactButtonHeight);
        button->setAccessibleName(QStringLiteral("비교 기간 ")+QString::fromUtf8(plan.label));
        button->setAccessibleDescription(periodDescription(plan));
        periodButtons_.insert(QString::fromLatin1(plan.id),button);
        header->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,id=QString::fromLatin1(plan.id)]{selectPeriod(id);});
    }
    intervalMenu_=new QPushButton(this);
    intervalMenu_->setObjectName(QStringLiteral("marketCompareIntervalMenu"));
    intervalMenu_->setAccessibleName(QStringLiteral("봉 간격 선택"));
    applyCompactButton(intervalMenu_);
    intervalMenu_->setFixedHeight(24);
    intervalMenu_->setFont(KeefiFonts::font(FontRole::Caption12));
    intervalMenu_->setToolTip(QStringLiteral("봉 간격을 직접 고르면 자동 선택이 풀려요. 1달은 30일, 3달은 90일이에요."));
    header->addWidget(intervalMenu_);
    auto* intervalPopup=new QMenu(this);
    intervalPopup->setObjectName(QStringLiteral("marketCompareIntervalPopup"));
    auto* controlsWidget=new QWidget(intervalPopup);
    auto* controls = new QHBoxLayout(controlsWidget);
    controls->setContentsMargins(8,6,8,6);
    controls->setSpacing(6);
    timeframes_ = new QButtonGroup(this);
    timeframes_->setExclusive(true);
    const QList<QPair<QString, qint64>> frames{{QStringLiteral("1분"), Minute}, {QStringLiteral("5분"), 5 * Minute},
                                               {QStringLiteral("15분"), 15 * Minute}, {QStringLiteral("1시간"), Hour},
                                               {QStringLiteral("4시간"), 4 * Hour}, {QStringLiteral("1일"), Day}};
    for (const auto& [label, interval] : frames) {
        auto* button = new QPushButton(label, controlsWidget);
        button->setCheckable(true);
        button->setObjectName(QStringLiteral("marketCompareTimeframe_%1").arg(interval));
        button->setProperty("intervalMs",interval);
        button->setAccessibleName(QStringLiteral("시간봉 %1").arg(label));
        applySegmentedControlStyle(button, CompactButtonHeight);
        button->setChecked(source_ && interval == source_->intervalMs());
        timeframes_->addButton(button);
        controls->addWidget(button);
        connect(button, &QPushButton::toggled, this, [this, interval](bool on) {
            if (!on || !source_) return;
            clearPeriod();
            automaticInterval_=false;
            chosenBaseOpenMs_ = 0;
            if(settings_)settings_->setValue(AutomaticIntervalPreference,false);
            source_->setInterval(interval);
            if(settings_)settings_->setValue(IntervalPreference,interval);
            rebuild();
        });
        connect(button,&QPushButton::clicked,this,[this,interval](bool on){
            // An already checked exclusive button does not emit toggled.
            // Choosing that same interval manually must still leave auto mode.
            if(!on||!source_||(!automaticInterval_&&selectedPeriod_.isEmpty()))return;
            clearPeriod();automaticInterval_=false;chosenBaseOpenMs_=0;
            if(settings_){
                settings_->setValue(AutomaticIntervalPreference,false);
                settings_->setValue(IntervalPreference,interval);
            }
            rebuild();
        });
    }
    auto* intervalAction=new QWidgetAction(intervalPopup);
    intervalAction->setDefaultWidget(controlsWidget);
    intervalPopup->addAction(intervalAction);
    connect(intervalMenu_,&QPushButton::clicked,this,[this,intervalPopup]{
        intervalPopup->popup(intervalMenu_->mapToGlobal(QPoint(0,intervalMenu_->height())));
    });
    root->addLayout(header);
    summary_=new CompareSingleLineLabel(this);
    summary_->setObjectName(QStringLiteral("marketCompareSummary"));
    summary_->setFont(KeefiFonts::font(FontRole::Body15));
    setThemeStyle(summary_,QStringLiteral("color:${text2};"));
    root->addWidget(summary_);

    chart_=new CompareChartWidget(this);
    auto* chipsBar=new QWidget(chart_);
    auto* chipLayout=new QHBoxLayout(chipsBar);
    chipLayout->setContentsMargins(0,0,0,0);
    chipLayout->setSpacing(4);
    for(const auto& spec:MarketCompare::defaultSpecs()){
        auto* chip=new QPushButton(shortName(spec.identity,spec.name),chipsBar);
        chip->setObjectName(QStringLiteral("marketCompareChip_")+spec.symbol);
        chip->setCheckable(true);
        chip->setChecked(true);
        applyCompactButton(chip);
        chip->setFont(KeefiFonts::font(FontRole::Caption12));
        chip->setIconSize(QSize(24,12));
        setThemeStyle(chip,QStringLiteral(R"(
            QPushButton { background:transparent; color:${text3}; border:1px solid ${rule}; border-radius:6px; padding:0 7px; }
            QPushButton:checked { background:${surface}; color:${text}; }
            QPushButton:hover { border-color:${text3}; }
            QPushButton:focus { border:2px solid ${accent}; padding:0 6px; }
        )"));
        const auto key=seriesKey(spec.identity);
        chips_.insert(key,chip);
        chipLayout->addWidget(chip);
        connect(chip,&QPushButton::toggled,this,[this,key,chip](bool shown){
            if(key==seriesKey(baseIdentity_)){
                const QSignalBlocker block(chip);chip->setChecked(true);return;
            }
            if(shown)hiddenSeries_.remove(key);else hiddenSeries_.insert(key);
            saveHiddenSeries();rebuild();
        });
    }
    auto* selectorsBar=new QWidget(chart_);
    auto* selectors=new QHBoxLayout(selectorsBar);
    selectors->setContentsMargins(0,0,0,0);
    selectors->setSpacing(8);
    baseSelector_=new CompareSelector(CompareSelector::Kind::Base,selectorsBar);
    baseSelector_->setObjectName(QStringLiteral("marketCompareBaseSelector"));
    baseSelector_->setAccessibleName(QStringLiteral("기준 종목"));
    wonSelector_=new CompareSelector(CompareSelector::Kind::Won,selectorsBar);
    wonSelector_->setObjectName(QStringLiteral("marketCompareWonSelector"));
    wonSelector_->setAccessibleName(QStringLiteral("비트코인 가격 기준"));
    wonSelector_->addItem(QStringLiteral("비트코인: 달러(바이낸스)"), int(CompareWonMode::Dollar));
    wonSelector_->addItem(QStringLiteral("비트코인: 원화(환율 효과 포함)"), int(CompareWonMode::WonRaw));
    wonSelector_->addItem(QStringLiteral("비트코인: 원화→달러 환산"), int(CompareWonMode::WonToDollar));
    selectors->addWidget(baseSelector_);
    selectors->addWidget(wonSelector_);
    chart_->setControls(chipsBar,selectorsBar);
    root->addWidget(chart_, 3);

    periodTable_=new QTableWidget(4,6,this);
    periodTable_->setObjectName(QStringLiteral("marketComparePeriodTable"));
    periodTable_->setAccessibleName(QStringLiteral("기간별 성적표"));
    periodTable_->setFrameShape(QFrame::NoFrame);
    periodTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    periodTable_->setSelectionMode(QAbstractItemView::NoSelection);
    periodTable_->setWordWrap(false);periodTable_->setShowGrid(false);
    periodTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    periodTable_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    periodTable_->setFont(KeefiFonts::font(FontRole::Secondary14));
    periodTable_->setIconSize(QSize(24,12));
    periodTable_->verticalHeader()->hide();
    periodTable_->verticalHeader()->setMinimumSectionSize(30);
    periodTable_->verticalHeader()->setDefaultSectionSize(30);
    periodTable_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* periodHeader=new ComparePeriodHeader(periodTable_);
    periodTable_->setHorizontalHeader(periodHeader);
    periodTable_->setHorizontalHeaderLabels({QStringLiteral("종목"),QStringLiteral("1일"),QStringLiteral("1주"),
        QStringLiteral("1달"),QStringLiteral("3달"),QStringLiteral("기준과 같이 움직였나")});
    periodHeader->setSectionResizeMode(0,QHeaderView::Fixed);periodTable_->setColumnWidth(0,124);
    for(int column=1;column<=4;++column)periodHeader->setSectionResizeMode(column,QHeaderView::Stretch);
    periodHeader->setSectionResizeMode(5,QHeaderView::Fixed);periodTable_->setColumnWidth(5,280);
    auto* returnDelegate=new CompareReturnDelegate(periodTable_);
    for(int column=1;column<=4;++column)periodTable_->setItemDelegateForColumn(column,returnDelegate);
    periodTable_->setItemDelegateForColumn(5,new CompareRelationDelegate(periodTable_));
    for(int row=0;row<4;++row){
        periodTable_->setRowHeight(row,30);
        for(int column=0;column<6;++column){
            auto* cell=new QTableWidgetItem(QStringLiteral("—"));
            cell->setFlags(Qt::ItemIsEnabled);
            cell->setTextAlignment((column>=1&&column<=4?Qt::AlignRight:Qt::AlignLeft)|Qt::AlignVCenter);
            cell->setFont(KeefiFonts::font(column==0?FontRole::BodyStrong15:column==5?FontRole::Caption12:FontRole::Number11));
            periodTable_->setItem(row,column,cell);
        }
    }
    setThemeStyle(periodTable_,QStringLiteral(R"(
        QTableWidget { background:${bg}; color:${text}; border:0; border-radius:0; padding:0; }
        QTableWidget::item { padding:0 6px; border-bottom:1px solid ${rule}; }
    )"));
    periodTable_->setFixedHeight(26+4*30);
    root->addWidget(periodTable_);
    connect(periodHeader,&QHeaderView::sectionClicked,this,[this](int column){
        if(column>=1&&column<=4)selectPeriod(QString::fromLatin1(PeriodPlans[size_t(column-1)].id));
    });

    auto* correlationHeader=new QHBoxLayout;
    correlationHeader->setSpacing(8);
    correlationToggle_=new QPushButton(this);
    correlationToggle_->setObjectName(QStringLiteral("marketCompareCorrelationToggle"));
    correlationToggle_->setCheckable(true);
    applyCompactButton(correlationToggle_);
    correlationHeader->addWidget(correlationToggle_);
    correlationControls_=new QWidget(this);
    auto* targetLayout=new QHBoxLayout(correlationControls_);
    targetLayout->setContentsMargins(0,0,0,0);
    targetLayout->setSpacing(8);
    auto* targetLabel=new QLabel(QStringLiteral("연동 비교"),correlationControls_);
    targetLabel->setFont(KeefiFonts::font(FontRole::Caption12));
    targetLayout->addWidget(targetLabel);
    auto* targetSelector=new LocalComboBox(correlationControls_);
    targetSelector->setCompact();
    targetSelector_=targetSelector;
    targetSelector_->setObjectName(QStringLiteral("marketCompareTargetSelector"));
    targetSelector_->setAccessibleName(QStringLiteral("연동 정도 비교 대상"));
    targetLayout->addWidget(targetSelector_);
    correlationHeader->addWidget(correlationControls_);
    correlationHeader->addStretch();
    root->addLayout(correlationHeader);
    correlation_ = new CompareCorrelationWidget(this);
    root->addWidget(correlation_, 1);
    setCorrelationExpanded(false,false);
    connect(correlationToggle_,&QPushButton::toggled,this,[this](bool expanded){setCorrelationExpanded(expanded,true);});

    collectorStatus_=new CompareSingleLineLabel(this);
    collectorStatus_->setObjectName(QStringLiteral("marketCompareCollectorStatus"));
    collectorStatus_->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(collectorStatus_,QStringLiteral("color:${text3};"));
    status_ = new CompareSingleLineLabel(this);
    status_->setObjectName(QStringLiteral("marketCompareStatus"));
    status_->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(status_,QStringLiteral("color:${text3};"));
    root->addWidget(collectorStatus_);
    root->addWidget(status_);

    chart_->baseChosen = [this](qint64 openMs) { clearPeriod();chosenBaseOpenMs_ = openMs; rebuild(); };
    connect(baseSelector_, &QComboBox::activated, this, [this](int index) {
        baseIdentity_ = baseSelector_->itemData(index).toString();
        if(hiddenSeries_.remove(seriesKey(baseIdentity_)))saveHiddenSeries();
        rebuild();
    });
    connect(targetSelector_, &QComboBox::activated, this, [this](int index) {
        targetIdentity_ = targetSelector_->itemData(index).toString();
        rebuild();
    });
    connect(wonSelector_, &QComboBox::activated, this, [this](int index) {
        if (source_) source_->setWonMode(CompareWonMode(wonSelector_->itemData(index).toInt()));
    });
    if (source_) {
        connect(source_, &MarketCompareSource::changed, this, [this] {
            if (observingSource_) refreshFromSource();
        });
    }
    connect(KeefiTheme::instance(), &KeefiTheme::changed, this, [this] { rebuild(); });
    rebuild();
}

MarketCompareView::~MarketCompareView() {
    if (source_) source_->setActive(false);
}

void MarketCompareView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (source_) source_->setActive(true);
    observingSource_ = true;
    refreshFromSource();
}

void MarketCompareView::hideEvent(QHideEvent* event) {
    observingSource_ = false;
    if (source_) source_->setActive(false);
    QWidget::hideEvent(event);
}

void MarketCompareView::refreshFromSource() { rebuild(); }

void MarketCompareView::setSettings(QSettings* settings){
    if(settings_==settings)return;
    settings_=settings;
    if(!settings_)return;
    hiddenSeries_.clear();
    QSet<QString> allowed;
    for(const auto& spec:MarketCompare::defaultSpecs())allowed.insert(seriesKey(spec.identity));
    for(const auto& identity:settings_->value(HiddenSeriesPreference).toStringList()){
        const auto key=seriesKey(identity);
        if(allowed.contains(key)&&key!=seriesKey(baseIdentity_))hiddenSeries_.insert(key);
    }
    setCorrelationExpanded(settings_->value(CorrelationPreference,false).toBool(),false);
    if(!source_)return;
    selectedPeriod_=settings_->value(PeriodPreference).toString();
    if(!periodPlan(selectedPeriod_))selectedPeriod_.clear();
    automaticInterval_=settings_->value(AutomaticIntervalPreference,true).toBool();
    bool valid=false;auto interval=settings_->value(IntervalPreference).toLongLong(&valid);
    if(const auto* plan=periodPlan(selectedPeriod_);plan&&automaticInterval_){interval=plan->interval;valid=true;}
    if(valid&&MarketCompare::supportedInterval(interval))source_->setInterval(interval);
    rebuild();
}

void MarketCompareView::clearPeriod(){
    selectedPeriod_.clear();
    if(settings_)settings_->setValue(PeriodPreference,QString{});
}

void MarketCompareView::selectPeriod(const QString& id){
    const auto* plan=periodPlan(id);
    if (!plan || !source_) { rebuild(); return; }
    const auto snapshot = source_->snapshot();
    if (!periodSelectable(snapshot, *plan, periodColumn(snapshot, *plan).ready)) { rebuild(); return; }
    selectedPeriod_=id;automaticInterval_=true;chosenBaseOpenMs_=0;
    if(settings_){
        settings_->setValue(PeriodPreference,id);
        settings_->setValue(AutomaticIntervalPreference,true);
        settings_->setValue(IntervalPreference,plan->interval);
    }
    source_->setInterval(plan->interval);
    rebuild();
}

void MarketCompareView::setCorrelationExpanded(bool expanded,bool persist){
    const QSignalBlocker block(correlationToggle_);
    correlationToggle_->setChecked(expanded);
    correlationToggle_->setText(expanded?QStringLiteral("▾ 연동 추이 접기"):QStringLiteral("▸ 연동 추이 보기"));
    correlationToggle_->setAccessibleName(expanded?QStringLiteral("연동 추이 접기"):QStringLiteral("연동 추이 보기"));
    correlationControls_->setVisible(expanded);
    correlation_->setVisible(expanded);
    if(persist&&settings_)settings_->setValue(CorrelationPreference,expanded);
}

void MarketCompareView::saveHiddenSeries(){
    if(!settings_)return;
    QStringList hidden(hiddenSeries_.cbegin(),hiddenSeries_.cend());
    hidden.sort();settings_->setValue(HiddenSeriesPreference,hidden);
}

QString MarketCompareView::legendText() const { return legendText_; }
QString MarketCompareView::notesText() const { return notesText_; }
QList<qint64> MarketCompareView::relationChangeOpens() const { return correlation_ ? correlation_->relationChangeOpens() : QList<qint64>{}; }

void MarketCompareView::rebuild() {
    const auto& c = themeColors();
    if (!source_) return;
    const auto snapshot = source_->snapshot();
    // The period buttons, selected chart base and table must agree on the same
    // completed bars. Evaluate each period for this snapshot once.
    std::array<PeriodColumn, PeriodPlans.size()> columns;
    int selectedPeriodColumn = -1;
    for (size_t period = 0; period < PeriodPlans.size(); ++period) {
        const auto& plan = PeriodPlans[period];
        const int tableColumn = int(period) + 1;
        columns[period] = periodColumn(snapshot, plan);
        auto* button=periodButtons_.value(QString::fromLatin1(plan.id));
        const QSignalBlocker block(button);
        const bool ready = columns[period].ready;
        const bool selectable = periodSelectable(snapshot, plan, ready);
        button->setEnabled(selectable);
        button->setChecked(selectedPeriod_==QLatin1String(plan.id));
        button->setToolTip(ready?periodDescription(plan):selectable?
            barIntervalText(plan.interval)+QStringLiteral(" 자료를 확인해요 · ")+periodDescription(plan):
            QStringLiteral("자료가 아직 모자라요"));
        periodTable_->horizontalHeaderItem(tableColumn)->setToolTip(button->toolTip());
        if (selectedPeriod_ == QLatin1String(plan.id)) selectedPeriodColumn = tableColumn;
    }
    static_cast<ComparePeriodHeader*>(periodTable_->horizontalHeader())->setPeriodColumn(selectedPeriodColumn);
    for(int row=0;row<periodTable_->rowCount();++row)
        for(int column=0;column<periodTable_->columnCount();++column)
            periodTable_->item(row,column)->setBackground(column==selectedPeriodColumn?c.surface:c.bg);
    intervalMenu_->setText(barIntervalText(snapshot.intervalMs)
        +(automaticInterval_?QStringLiteral(" · 자동 ▾"):QStringLiteral(" · 직접 ▾")));
    {
        const auto buttons=timeframes_->buttons();std::vector<QSignalBlocker> blockers;blockers.reserve(buttons.size());
        for(auto* button:buttons)blockers.emplace_back(button);
        for(auto* button:buttons)button->setChecked(button->property("intervalMs").toLongLong()==snapshot.intervalMs);
    }
    QList<CompareSeries> available;
    for (const auto& series : snapshot.series)
        if (series.available && series.bars.size() >= 2) available.append(series);

    // Selectors keep their identity choice; bitcoin keeps its slot across won modes.
    const auto fill = [&](QComboBox* box, QString* chosen, bool includeBase) {
        const QSignalBlocker block(box);
        int count = 0;
        for (const auto& series : snapshot.series) {
            if (!includeBase && (series.spec.identity == baseIdentity_ || (isBitcoin(series.spec.identity) && isBitcoin(baseIdentity_)))) continue;
            if (count == box->count()) {
                box->addItem(series.spec.name, series.spec.identity);
            } else {
                if (box->itemText(count) != series.spec.name) box->setItemText(count, series.spec.name);
                if (box->itemData(count) != series.spec.identity) box->setItemData(count, series.spec.identity);
            }
            ++count;
        }
        while (box->count() > count) box->removeItem(box->count() - 1);
        int index = box->findData(*chosen);
        if (index < 0 && isBitcoin(*chosen))
            for (int i = 0; i < box->count(); ++i)
                if (isBitcoin(box->itemData(i).toString())) index = i;
        if (index < 0) index = 0;
        box->setCurrentIndex(index);
        if (box->count()) *chosen = box->itemData(index).toString();
    };
    fill(baseSelector_, &baseIdentity_, true);
    fill(targetSelector_, &targetIdentity_, false);
    wonSelector_->setCurrentIndex(wonSelector_->findData(int(snapshot.wonMode)));

    // One base bar for every line: the chosen bar if all lines have it, else the
    // first bar all lines share.
    QSet<qint64> common;
    bool first = true;
    for (const auto& series : available) {
        QSet<qint64> opens;
        for (const auto& bar : series.bars) opens.insert(bar.openMs);
        common = first ? opens : common.intersect(opens);
        first = false;
    }
    QList<qint64> sharedOpens(common.cbegin(), common.cend());
    std::sort(sharedOpens.begin(), sharedOpens.end());
    baseOpenMs_ = sharedOpens.contains(chosenBaseOpenMs_) ? chosenBaseOpenMs_ : (sharedOpens.isEmpty() ? 0 : sharedOpens.first());
    if (selectedPeriodColumn > 0) {
        const auto& selected = columns[size_t(selectedPeriodColumn - 1)];
        baseOpenMs_ = selected.ready && common.contains(selected.window.startOpenMs) ? selected.window.startOpenMs : 0;
    }

    const CompareSeries* base = nullptr;
    const CompareSeries* target = nullptr;
    for (const auto& series : available) {
        if (series.spec.identity == baseIdentity_ || (isBitcoin(series.spec.identity) && isBitcoin(baseIdentity_))) base = &series;
        if (series.spec.identity == targetIdentity_ || (isBitcoin(series.spec.identity) && isBitcoin(targetIdentity_))) target = &series;
    }

    QList<CompareLine> lines;
    QList<CorrelationPoint> targetCorrelation;
    QList<SummaryEntry> summaryEntries;
    QStringList legend;
    int index = 0;
    for (const auto& series : snapshot.series) {
        CompareLine line;
        line.identity = series.spec.identity;
        line.name = series.spec.name;
        line.color = colorFor(series.spec, index);
        line.style = styleFor(index);
        line.base = base && base->spec.identity == series.spec.identity;
        ++index;
        if (series.available && baseOpenMs_) line.points = MarketCompare::percentFromBase(series, baseOpenMs_);
        QHash<QString, int> fxCounts;
        int provisionalCount = 0;
        if (!series.fx.isEmpty()) {
            QHash<qint64, QString> fxNote;
            for (int i = 0; i < series.bars.size() && i < series.fx.size(); ++i) {
                const auto& choice = series.fx[i];
                auto label = choice.label + (choice.previous ? QStringLiteral(" · 직전 고시") : QString{});
                ++fxCounts[label];
                if (snapshot.provisionalFxOpenMs.contains(series.bars[i].openMs)) {
                    label += QStringLiteral(" · 잠정");
                    ++provisionalCount;
                }
                fxNote.insert(series.bars[i].openMs, QStringLiteral("환율 %1 %2 %3")
                                                         .arg(label, timeText(choice.postedAtMs, snapshot.intervalMs), QString::number(choice.krwPerUsd, 'f', 2)));
            }
            for (const auto& point : line.points) line.notes.append(fxNote.value(point.openMs));
        }
        QString text = QStringLiteral("<span style='color:%1'>━━</span> <b>%2</b> ").arg(line.color.name(), line.name.toHtmlEscaped());
        if (!line.points.isEmpty()) {
            text += percentText(line.points.last().percent);
            if (!series.reason.isEmpty()) // The bars shown are the last good page; say why the refresh failed.
                text += QStringLiteral(" · <span style='color:%1'>%2%3</span>")
                            .arg(c.danger.name(), series.reason.toHtmlEscaped(),
                                 series.lastOkAtMs ? QStringLiteral(" · 마지막 정상 수신 %1").arg(timeText(series.lastOkAtMs, Minute)) : QString{});
        } else {
            text += QStringLiteral("<span style='color:%1'>%2</span>").arg(c.text2.name(), (series.reason.isEmpty() ? QStringLiteral("자료 없음") : series.reason).toHtmlEscaped());
        }
        if (!fxCounts.isEmpty()) {
            QStringList parts;
            for (auto it = fxCounts.cbegin(); it != fxCounts.cend(); ++it) parts.append(QStringLiteral("%1 %2봉").arg(it.key()).arg(it.value()));
            parts.sort();
            text += QStringLiteral("<br><span style='color:%1'>　환율: %2</span>").arg(c.text2.name(), parts.join(QStringLiteral(" · ")).toHtmlEscaped());
            if (provisionalCount) text += QStringLiteral(" · 잠정 %1봉").arg(provisionalCount);
        }
        QString relation;
        std::optional<double> relation20,relation60;
        if (line.base) text += QStringLiteral(" · 기준");
        else if (base && series.available) {
            const auto points = MarketCompare::rollingCorrelation(*base, series);
            if (!points.isEmpty()) {
                const auto& last = points.last();
                relation20=last.r20;relation60=last.r60;
                relation=MarketCompare::relationLabel(last.r20);
                text += QStringLiteral(" · %1 (20봉 %2 / 60봉 %3)").arg(relation, signedTwo(last.r20), signedTwo(last.r60));
            }
            if (target && series.spec.identity == target->spec.identity) targetCorrelation = points;
        }
        if (series.spec.currency != QLatin1String("USDT")) text += QStringLiteral(" · %1").arg(series.spec.currency);
        legend.append(text);
        const int row=index-1;
        if(row<periodTable_->rowCount()){
            auto* name=periodTable_->item(row,0);
            name->setText(shortName(series.spec.identity,series.spec.name));
            name->setToolTip(series.spec.name);name->setForeground(c.text);
            for(size_t period=0;period<PeriodPlans.size();++period){
                const auto& plan=PeriodPlans[period];
                const auto& window = columns[period].window;
                const auto& value = columns[period].values.at(row);
                auto* cell=periodTable_->item(row,int(period)+1);
                cell->setText(value.percent?percentText(*value.percent):QStringLiteral("—"));
                cell->setForeground(!value.percent||*value.percent==0?c.text3:*value.percent>0?c.up:c.down);
                cell->setToolTip(periodReturnTooltip(series,plan,window,value));
            }
            const bool isBase=seriesKey(series.spec.identity)==seriesKey(baseIdentity_);
            auto* relationship=periodTable_->item(row,5);
            relationship->setText(isBase?QStringLiteral("기준"):MarketCompare::relationLabel(relation20));
            relationship->setForeground(isBase?c.text3:c.text2);
            relationship->setData(RelationValueRole,isBase||!relation20?QVariant{}:QVariant(*relation20));
            relationship->setToolTip(isBase?QStringLiteral("선택한 기준 종목이에요"):
                QStringLiteral("기준: %1\n20봉 %2 · 60봉 %3\n눈금은 20봉 연동 정도의 크기예요. 방향은 낱말로 표시해요.")
                    .arg(baseSelector_->currentText(),signedTwo(relation20),signedTwo(relation60)));
        }
        if(auto* chip=chips_.value(seriesKey(series.spec.identity))){
            const bool isBase=seriesKey(series.spec.identity)==seriesKey(baseIdentity_);
            const QSignalBlocker block(chip);
            chip->setChecked(isBase||!hiddenSeries_.contains(seriesKey(series.spec.identity)));
            chip->setText(shortName(series.spec.identity,series.spec.name)
                          +(isBase?QStringLiteral(" [기준]"):QString{})+QStringLiteral("  ")
                          +(line.points.isEmpty()?QStringLiteral("—"):percentText(line.points.last().percent)));
            const auto detail=QTextDocumentFragment::fromHtml(text).toPlainText();
            chip->setToolTip(plainToolTip(detail));
            chip->setAccessibleDescription(detail);
            QPixmap sample(48,24);sample.setDevicePixelRatio(2);sample.fill(Qt::transparent);
            {QPainter painter(&sample);painter.setRenderHint(QPainter::Antialiasing);
             painter.setPen(QPen(line.color,isBase?2.6:1.8,line.style,Qt::RoundCap));
             painter.drawLine(QPointF(2,6),QPointF(22,6));}
            chip->setIcon(QIcon(sample));
            if(row<periodTable_->rowCount())periodTable_->item(row,0)->setIcon(QIcon(sample));
        }
        if (!line.points.isEmpty()) {
            summaryEntries.append({shortName(series.spec.identity,series.spec.name),relation,line.points.last().percent,line.base});
            lines.append(line);
        }
    }
    legendText_=QTextDocumentFragment::fromHtml(legend.join(QStringLiteral("<br>"))).toPlainText();
    const auto summary=available.size()==4?comparisonSummary(summaryEntries):QString{};
    summary_->setText(summary);
    summary_->setToolTip(summary);
    summary_->setVisible(!summary.isEmpty());
    baseSelector_->updateGeometry();wonSelector_->updateGeometry();
    baseSelector_->setToolTip(baseSelector_->currentText());
    wonSelector_->setToolTip(wonSelector_->currentText());

    QList<TimeSpan> weekends;
    for (const auto& series : available) {
        if (series.spec.venue != CompareVenue::BinanceUsdM) continue;
        for (const auto& bar : series.bars) {
            if (bar.openMs < baseOpenMs_ || !MarketCompare::underlyingWeekend(series.spec, bar.openMs)) continue;
            if (!weekends.isEmpty() && weekends.last().toMs + 1 >= bar.openMs) weekends.last().toMs = bar.closeMs;
            else weekends.append({bar.openMs, bar.closeMs});
        }
        break;
    }
    chart_->setData(lines, weekends, snapshot.intervalMs, hiddenSeries_);

    if (base && target && base != target) {
        const auto& points = targetCorrelation;
        const auto key = base->spec.identity + '|' + target->spec.identity + '|' + QString::number(snapshot.intervalMs) + '|' +
                         base->spec.currency + '|' + target->spec.currency;
        auto& cached = relationCache_[key];
        const bool usesConvertedFx = !base->fx.isEmpty() || !target->fx.isEmpty();
        QList<RelationChange> provisional;
        for (const auto& change : MarketCompare::relationChanges(points)) {
            if (usesConvertedFx && snapshot.provisionalFxOpenMs.contains(change.openMs))
                provisional.append(change); // Recompute after the open FX hour seals.
            else
                cached.insert(change.openMs, change);
        }
        QList<RelationChange> changes;
        for (const auto& change : std::as_const(cached))
            if (change.openMs >= chart_->firstMs() && change.openMs <= chart_->lastMs()) changes.append(change);
        for (const auto& change : provisional)
            if (change.openMs >= chart_->firstMs() && change.openMs <= chart_->lastMs()) changes.append(change);
        correlation_->setData(points, changes, chart_->firstMs(), chart_->lastMs(),
                              QStringLiteral("연동 정도: %1 ↔ %2 · 실선 20봉, 점선 60봉, ◆ 관계 바뀜").arg(base->spec.name, target->spec.name));
    } else {
        correlation_->setData({}, {}, 0, 0, QStringLiteral("연동 정도: 비교할 두 종목의 봉을 기다리는 중"));
    }

    QStringList notes{QStringLiteral("금·원유·나스닥은 바이낸스 무기한 계약 가격이에요. 실제 지수·현물 가격과 조금 달라요."),
                      QStringLiteral("회색 바탕은 기초 시장의 주말 휴장 시간이에요. 그 시간의 값은 계약 가격일 뿐이에요."),
                      QStringLiteral("상관은 함께 움직인 정도일 뿐 원인이 아니에요. 같은 기간이라도 시간봉마다 달라요.")};
    if (snapshot.wonMode == CompareWonMode::WonRaw) notes.append(QStringLiteral("비트코인은 업비트 원화 가격이라 원/달러 움직임(환율 효과)이 포함돼요."));
    if (snapshot.wonMode == CompareWonMode::WonToDollar) {
        notes.append(QStringLiteral("비트코인은 원/달러 이력으로 환산했어요. 일봉은 ECB, 짧은 봉은 하나은행 고시 → 없으면 ECB 근사 → 둘 다 없으면 환산 불가예요."));
        if (!snapshot.provisionalFxOpenMs.isEmpty())
            notes.append(QStringLiteral("잠정으로 표시한 봉은 하나은행 기록 확정 뒤 환율이 바뀔 수 있어요."));
        if (!snapshot.collectorReason.isEmpty()) notes.append(QStringLiteral("수집기: ") + snapshot.collectorReason);
        if (!snapshot.fxReason.isEmpty()) notes.append(QStringLiteral("환율: ") + snapshot.fxReason);
    }
    if (!snapshot.catalogReason.isEmpty()) notes.append(QStringLiteral("선물 종목 목록: ") + snapshot.catalogReason);
    auto informationNotes=notes;
    QStringList collectorNotes;
    if(snapshot.wonMode==CompareWonMode::WonToDollar&&!snapshot.collectorReason.isEmpty()){
        const auto reason=QStringLiteral("수집기: ")+snapshot.collectorReason;
        informationNotes.removeOne(reason);
        collectorNotes.append(reason);
    }
    if (snapshot.collectorWrittenAtMs > 0) {
        const auto age = std::max<qint64>(0, snapshot.updatedAtMs - snapshot.collectorWrittenAtMs);
        notes.append(age <= 3 * Minute ? QStringLiteral("수집기: 마지막 기록 %1초 전").arg(age / 1000)
                                       : QStringLiteral("수집기 마지막 기록 %1분 전 · 멈춘 것 같아요. 짧은 봉의 원화→달러 환산은 ECB 근사로 바뀌어요").arg(age / Minute));
    } else {
        notes.append(QStringLiteral("수집기 기록 없음 · 짧은 봉의 원화→달러 환산은 ECB 근사로 해요"));
    }
    collectorNotes.append(notes.last());
    notesText_=notes.join(QStringLiteral("\n"));
    const auto collectorText=collectorNotes.join(QStringLiteral(" · "));
    collectorStatus_->setText(collectorText);
    collectorStatus_->setToolTip(collectorText);
    information_->setInformation(QStringLiteral("시장 비교"),
        QStringLiteral("금·원유·나스닥·비트코인을 같은 기준 시점에서 겹쳐 보고, 같이·따로·반대로 움직이는지 봐요.\n\n")
            +informationNotes.join(QStringLiteral("\n")));
    QSet<qint64> observedIntervals;
    for(const auto& series:snapshot.series)if(series.available&&!series.bars.isEmpty())observedIntervals.insert(series.intervalMs);
    const auto actualInterval=observedIntervals.size()==1?*observedIntervals.cbegin():qint64(0);
    const auto intervalLabel=observedIntervals.isEmpty()?barIntervalText(snapshot.intervalMs)+QStringLiteral(" 자료 대기")
        :actualInterval>0?barIntervalText(actualInterval):QStringLiteral("봉 간격 확인 필요");
    const auto basis=baseOpenMs_?timeText(baseOpenMs_,actualInterval>0?actualInterval:snapshot.intervalMs)+QStringLiteral(" KST · UTC+9"):QStringLiteral("없음");
    status_->setText(intervalLabel+QStringLiteral(" · 기준 봉 %1 · 완료 봉만 계산해요").arg(basis)
        +(snapshot.loading?QStringLiteral(" · 받는 중…"):QString{})+QStringLiteral(" · 차트를 누르면 그 시점이 기준"));
    status_->setToolTip(status_->text());
}
} // namespace keefi
