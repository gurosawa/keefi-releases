#include "keefi/PortfolioDayView.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/TradingTypes.h"
#include "keefi/InfoButton.h"
#include "keefi/MovementColorPolicy.h"
#include <QHBoxLayout>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QTimeZone>
#include <QStyle>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
// The flow toggle's chevron is drawn: a font's ▴/▾ differ in size between
// fonts, and a triangle beside the change would read like its ▲/▼ mark.
QIcon chevronIcon(bool up, const QColor& colour, qreal dpr) {
    QPixmap pixmap(QSize(10, 10) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colour, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal outer = up ? 6.5 : 3.5, tip = up ? 3.5 : 6.5;
    painter.drawPolyline(QPolygonF{QPointF(2, outer), QPointF(5, tip), QPointF(8, outer)});
    return QIcon(pixmap);
}
QString clockText(qint64 at) {
    return QDateTime::fromMSecsSinceEpoch(at, QTimeZone(9 * 3600)).toString("HH:mm:ss");
}
QString signedNumber(const QString& value, const PortfolioMoneyDisplay& display = {}) {
    const int sign = decimal::compare(value, "0");
    return (sign > 0 ? QStringLiteral("▲ +") : sign < 0 ? QStringLiteral("▼ ") : QString{}) + display.amount(value);
}
QString signedAmount(const QString& value, const PortfolioMoneyDisplay& display = {}) { return signedNumber(value, display) + " " + display.unit(); }
const QString DefaultLabel = QStringLiteral("관측 평가액 증감 · 입출금 포함 가능");
void changeTone(QLabel* label,bool available,int direction=0){
    // Unavailable explanations contain Korean, so never send them through the numeric face.
    label->setFont(KeefiFonts::font(available ? FontRole::Value22 : FontRole::Body15));
    label->setProperty("type",available?"observedAmount":"muted");
    label->setProperty("tone",available?(direction>0?"up":direction<0?"down":"neutral"):"");
    label->style()->unpolish(label);label->style()->polish(label);
}
}
class PortfolioDayChart final : public QWidget {
  public:
    explicit PortfolioDayChart(QWidget* p) : QWidget(p) {
        setObjectName("portfolioDayChart");
        setFixedHeight(80); setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(tr("오늘 관측 평가액 흐름"));
    }
    void setPoints(const QList<PortfolioHistoryPoint>& points) {
        points_ = points;
        int valid = 0, breaks = 0;
        for (qsizetype i = 0; i < points_.size(); ++i) {
            const auto& point = points_[i];
            if (point.available && !point.value.isEmpty()) ++valid;
            if (i > 0 && (!point.available || !point.connectToPrevious)) ++breaks;
        }
        setAccessibleDescription(points_.isEmpty() ? QString{} : tr("유효 기록 %1개 · 이어지지 않는 구간 %2개").arg(valid).arg(breaks));
        // A lone dot carries no change information and consumes the holdings area.
        setVisible(valid > 1 && points_.first().timeMs < points_.last().timeMs); update();
    }
    MovementDirection direction = MovementDirection::Unknown;
  protected:
    void paintEvent(QPaintEvent*) override {
        if (points_.isEmpty()) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area(5, 5, qMax(1, width() - 10), 48);
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (const auto& point : points_) {
            bool ok = false; const auto value = point.value.toDouble(&ok);
            if (point.available && ok && std::isfinite(value)) { low = qMin(low, value); high = qMax(high, value); }
        }
        if (!std::isfinite(low) || !std::isfinite(high)) return;
        const auto start = points_.first().timeMs, end = points_.last().timeMs;
        const auto location = [&](const PortfolioHistoryPoint& point, double value) {
            // Money is calculated before reaching this view; doubles are only coordinates.
            const auto x = end > start ? double(point.timeMs - start) / double(end - start) : 0.5;
            const auto y = high > low ? (value - low) / (high - low) : 0.5;
            return QPointF(area.left() + area.width() * x, area.bottom() - area.height() * y);
        };
        painter.setPen(QPen(themeColors().grid, 1)); painter.drawLine(area.bottomLeft(), area.bottomRight());
        const auto color=direction==MovementDirection::Unknown?themeColors().accent:direction==MovementDirection::Flat?themeColors().text3:MovementColorPolicy::fillFor(this,direction);
        painter.setPen(QPen(color, 2)); painter.setBrush(color);
        bool previousValid = false; QPointF previous;
        for (const auto& point : points_) {
            bool ok = false; const double value = point.value.toDouble(&ok);
            const bool valid = point.available && ok && std::isfinite(value);
            if (!valid) { previousValid = false; continue; }
            const auto position = location(point, value);
            if (previousValid && point.connectToPrevious) painter.drawLine(previous, position);
            painter.drawEllipse(position, 2.5, 2.5); previous = position; previousValid = true;
        }
        painter.setFont(KeefiFonts::font(FontRole::Number11));
        painter.setPen(themeColors().text3);
        painter.drawText(QRect(0, 58, width() / 2, 20), Qt::AlignLeft | Qt::AlignVCenter, clockText(start));
        painter.drawText(QRect(width() / 2, 58, width() / 2, 20), Qt::AlignRight | Qt::AlignVCenter, clockText(end) + " KST");
    }
  private:
    QList<PortfolioHistoryPoint> points_;
};
class PortfolioContributionBars final:public QWidget {
  public:
    explicit PortfolioContributionBars(QWidget* parent):QWidget(parent) {
        // Keep the existing object name for the view's accessibility/locking contract.
        setObjectName("portfolioContributionBars"); setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        auto* rows = new QVBoxLayout(this); rows->setContentsMargins(0,0,0,0); rows->setSpacing(0);
        refreshColors();
    }
    void refreshColors() {
        const bool korean = MovementColorPolicy::schemeFor(this) == MovementScheme::KoreanRedBlue;
        setThemeStyle(this, QStringLiteral(
            "QWidget[portfolioContributionRow=true]{border-top:1px solid ${rule};background:transparent;}"
            "QLabel{border:0;background:transparent;}"
            "QLabel[contributionDirection=up]{color:%1;}"
            "QLabel[contributionDirection=down]{color:%2;}"
            "QLabel[contributionDirection=flat]{color:${text3};}"
            "QLabel[contributionReference=true]{color:${accentText};}")
            .arg(korean ? "${downText}" : "${upText}", korean ? "${koreanDownText}" : "${downText}"));
    }
    void setGroups(const QList<PortfolioContributionGroup>& groups, const PortfolioMoneyDisplay& display = {}) {
        while (auto* item = layout()->takeAt(0)) { delete item->widget(); delete item; }
        QStringList descriptions;
        for (const auto& group : groups) {
            auto* row = new QWidget(this); row->setProperty("portfolioContributionRow",true);
            auto* columns = new QHBoxLayout(row); columns->setContentsMargins(0,12,0,12); columns->setSpacing(8);
            const auto id = QString::number(int(group.assetClass));
            const auto makeLabel = [&](const QString& name, const QString& text, FontRole role) {
                auto* label = new QLabel(text,row); label->setObjectName(name + id);
                label->setTextFormat(Qt::PlainText); label->setFont(KeefiFonts::font(role)); return label;
            };
            auto* name = makeLabel("portfolioContributionName_",portfolioContributionClassLabel(group.assetClass),FontRole::Body15);
            name->setMinimumWidth(0); name->setWordWrap(true); columns->addWidget(name,1);
            auto* value = makeLabel("portfolioContributionValue_",signedNumber(group.difference, display),FontRole::Value22);
            const int sign = decimal::compare(group.difference,"0");
            value->setProperty("contributionDirection", sign > 0 ? "up" : sign < 0 ? "down" : "flat");
            columns->addWidget(value);
            auto* unit = makeLabel("portfolioContributionUnit_",display.unit(),FontRole::Number11);
            columns->addWidget(unit);
            auto* reference = makeLabel("portfolioContributionReference_",references_,FontRole::Number11);
            reference->setProperty("contributionReference",true); reference->setVisible(!references_.isEmpty());
            columns->addWidget(reference);
            layout()->addWidget(row);
            descriptions.append(name->text() + ' ' + signedAmount(group.difference, display));
        }
        setAccessibleDescription(descriptions.join(" · ")); setVisible(!groups.isEmpty());
    }
    void setReferences(const QString& references) {
        references_ = references;
        for (auto* label : findChildren<QLabel*>(QRegularExpression("portfolioContributionReference_.*"))) {
            label->setText(references); label->setVisible(!references.isEmpty());
        }
    }
  private:
    QString references_;
};
PortfolioDayView::PortfolioDayView(QWidget* parent) : QWidget(parent) {
    setObjectName("portfolioDayView"); setMinimumWidth(0);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 10, 0, 0); layout->setSpacing(5);
    title_ = new QLabel(DefaultLabel, this); title_->setObjectName("portfolioDayTitle");
    title_->setProperty("type", "muted"); title_->setWordWrap(true);
    title_->setProperty("type","caption");
    change_ = new QLabel(this); change_->setObjectName("portfolioDayChange"); change_->setWordWrap(true);
    reference_ = new QLabel(this); reference_->setObjectName("portfolioDayReference");
    reference_->setTextFormat(Qt::PlainText); reference_->setFont(KeefiFonts::font(FontRole::Number11));
    setThemeStyle(reference_,"QLabel{color:${accentText};background:transparent;border:0;}");
    currencyStatus_ = new QLabel(this);currencyStatus_->setObjectName("portfolioDayCurrencyStatus");
    currencyStatus_->setTextFormat(Qt::PlainText);currencyStatus_->setProperty("type","caption");
    currencyStatus_->setFont(KeefiFonts::font(FontRole::Caption12));setThemeStyle(currencyStatus_,"color:${text3};");currencyStatus_->hide();
    baseline_ = new QLabel(this); baseline_->setObjectName("portfolioDayBaseline");
    baseline_->setProperty("type", "muted"); baseline_->setWordWrap(true);
    contributions_ = new QLabel(this); contributions_->setObjectName("portfolioDayContributions");
    contributions_->setWordWrap(true); contributions_->setProperty("type", "muted");
    chart_ = new PortfolioDayChart(this);
    bars_=new PortfolioContributionBars(this);info_=new InfoButton(this);info_->setObjectName("portfolioDayInfo");
    info_->setInformation(tr("오늘 내 자산"),tr("연결 계좌에서 실제로 관측한 평가액의 변화입니다. 입출금이 포함될 수 있으며 투자 수익률이 아닙니다. 기여도는 같은 비교 범위의 기존 평가액 증감입니다.\n\n코인 투자 손익은 업비트·바이낸스 기준으로 입출금을 뺀 손익이에요. 조건을 모두 확인한 뒤에만 보이고, 현금화한 돈이나 세금 신고 금액이 아니에요. 주식은 포함하지 않아요."));
    // One line: the caption, today's observed change, the flow on request, and what it means behind ⓘ.
    toggle_=new QPushButton(this);toggle_->setObjectName("portfolioDayToggle");toggle_->setFont(KeefiFonts::font(FontRole::Caption12));
    toggle_->setFixedHeight(28);toggle_->setCursor(Qt::PointingHandCursor);toggle_->setFocusPolicy(Qt::StrongFocus);
    toggle_->setIconSize(QSize(10,10));toggle_->setLayoutDirection(Qt::RightToLeft); // the chevron follows the words
    setThemeStyle(toggle_,"QPushButton{background:transparent;border:2px solid transparent;border-radius:6px;color:${text2};padding:0 6px;}"
                          "QPushButton:hover{color:${text};}QPushButton:focus{border-color:${accent};}");
    connect(toggle_,&QPushButton::clicked,this,[this]{setDetailsExpanded(!expanded_);});
    auto* head=new QHBoxLayout;head->setSpacing(8);auto* caption=new QLabel(tr("오늘 내 자산"),this);caption->setProperty("type","caption");
    head->addWidget(caption);head->addWidget(change_,1);head->addWidget(reference_);head->addWidget(currencyStatus_);head->addWidget(toggle_);head->addWidget(info_);layout->addLayout(head);
    details_=new QWidget(this);details_->setObjectName("portfolioDayDetails");
    auto* detailsLayout=new QVBoxLayout(details_);detailsLayout->setContentsMargins(0,0,0,0);detailsLayout->setSpacing(5);
    detailsLayout->addWidget(chart_);detailsLayout->addWidget(bars_);
    layout->addWidget(title_); layout->addWidget(baseline_); layout->addWidget(details_);layout->addWidget(contributions_);
    coinTitle_ = new QLabel(tr("코인 투자 손익 · 업비트·바이낸스 기준"), this); coinTitle_->setObjectName("coinPerformanceTitle");
    coinTitle_->setProperty("type", "caption"); coinTitle_->setWordWrap(true);
    coinValue_ = new QLabel(this); coinValue_->setObjectName("coinPerformanceValue"); coinValue_->setWordWrap(true); coinValue_->setTextFormat(Qt::PlainText);
    coinNote_ = new QLabel(this); coinNote_->setObjectName("coinPerformanceNote"); coinNote_->setWordWrap(true); coinNote_->setTextFormat(Qt::PlainText);
    coinNote_->setProperty("type", "caption"); coinNote_->setFont(KeefiFonts::font(FontRole::Caption12)); setThemeStyle(coinNote_, "color:${text3};");
    layout->addSpacing(6); layout->addWidget(coinTitle_); layout->addWidget(coinValue_); layout->addWidget(coinNote_);
    coinTitle_->hide(); coinValue_->hide(); coinNote_->hide();
    title_->hide();baseline_->hide();contributions_->hide();
    if(auto* policy=MovementColorPolicy::forContext(this))connect(policy,&MovementColorPolicy::changed,this,[this]{chart_->update();bars_->refreshColors();});
    bindThemePresentation(this,[this]{chart_->update();bars_->refreshColors();
        toggle_->setIcon(chevronIcon(expanded_,themeColors().text2,toggle_->devicePixelRatioF()));});
    setDetailsExpanded(false);
    clear(tr("오늘 비교 기록이 없습니다."));
}
void PortfolioDayView::setDetailsExpanded(bool expanded) {
    const bool changed = expanded_ != expanded;
    expanded_ = expanded;
    if (!expanded_) setMinimumHeight(0);
    details_->setVisible(expanded_);
    toggle_->setText(expanded_ ? tr("흐름 접기") : tr("흐름 보기"));
    toggle_->setIcon(chevronIcon(expanded_, themeColors().text2, toggle_->devicePixelRatioF()));
    toggle_->setAccessibleName(expanded_ ? tr("오늘 흐름 그래프 접기") : tr("오늘 흐름 그래프 보기"));
    refreshDetailsToggle();
    if (changed) emit detailsExpandedChanged(expanded_);
}
void PortfolioDayView::refreshDetailsToggle() {
    // Offer the flow only when there is a chart or contributions to open.
    toggle_->setVisible((expanded_ && minimumHeight() > 0) || chart_->isVisibleTo(details_) || bars_->isVisibleTo(details_));
}
void PortfolioDayView::setHistory(const PortfolioDayHistory& history, const PortfolioMoneyDisplay& display, bool refreshing) {
    // During a reread, the valuation cannot yet compare all accounts. Reserve
    // only the last measured height, including a wrapped summary in a narrow
    // window. The replacement content below carries no old contribution rows.
    if (!refreshing) {
        setMinimumHeight(0);
        details_->setMinimumHeight(0);
        hasSettledHistory_ = true;
    } else if (!refreshPending_ && hasSettledHistory_ && isVisible()) {
        if (!bars_->isHidden() || !chart_->isHidden())
            details_->setMinimumHeight(details_->sizeHint().height());
        setMinimumHeight(height());
    }
    refreshPending_ = refreshing;
    // A newly delivered history cannot keep numbers allocated for the old evidence set.
    setEvidenceReferences({});
    info_->setPrivateAccess(true);
    const auto& comparison = history.comparison;
    comparisonAvailable_ = comparison.available;
    title_->setText(comparison.label.isEmpty() ? DefaultLabel : comparison.label);
    if (comparison.available) {
        const int direction = decimal::compare(comparison.difference, "0");
        chart_->direction=direction>0?MovementDirection::Up:direction<0?MovementDirection::Down:MovementDirection::Flat;
        const auto prefix = direction > 0 ? QStringLiteral("▲ +") : direction < 0 ? QStringLiteral("▼ ") : QString{};
        change_->setText(prefix + display.amount(comparison.difference) + " " + display.unit());
        changeTone(change_,true,direction);
    } else {
        chart_->direction=MovementDirection::Unknown;
        change_->setText(comparison.reason.isEmpty() ? tr("오늘 비교 기록이 없습니다.") : comparison.reason);
        changeTone(change_,false);
    }
    currencyStatus_->setText(comparison.available ? display.status() : QString{});currencyStatus_->setVisible(!currencyStatus_->text().isEmpty());
    const auto at = comparison.fromMs > 0 ? comparison.fromMs : history.baseline.available ? history.baseline.timeMs : 0;
    baseline_->setText(at > 0 ? tr("%1 기준 · KST").arg(clockText(at)) : QString{});
    baseline_->hide();
    QStringList contributions;
    if (history.contributions.available && comparison.available)
        for (const auto& group : history.contributions.groups)
            contributions.append(portfolioContributionClassLabel(group.assetClass) + ' ' + signedAmount(group.difference, display));
    contributions_->setText(contributions.join(QStringLiteral("  ·  "))); contributions_->hide();
    bars_->setGroups(history.contributions.available&&comparison.available?history.contributions.groups:QList<PortfolioContributionGroup>{}, display);
    chart_->setPoints(history.points);
    info_->setPrivateInformation(title_->text()+"\n"+baseline_->text()+"\n"+contributions_->text()+(display.information().isEmpty()?QString{}:"\n"+display.information()));
    refreshDetailsToggle();
    setAccessibleDescription(title_->text() + " · " + change_->text() + " · " + baseline_->text()+(currencyStatus_->text().isEmpty()?QString{}:" · "+currencyStatus_->text()));
}
void PortfolioDayView::setEvidenceReferences(const QString& references) {
    // Only screen-allocated reference marks enter this view. Reject the entire
    // string if a caller accidentally supplies an account alias/identity with it.
    static const QRegularExpression assignedReferences(QStringLiteral(
        R"(^\[[1-9][0-9]*\](?:(?:\s+|\s*·\s*)\[[1-9][0-9]*\])*$)"));
    const auto trimmed = references.trimmed();
    const auto accepted = comparisonAvailable_ && assignedReferences.match(trimmed).hasMatch() ? trimmed : QString{};
    reference_->clear(); reference_->hide(); bars_->setReferences({});
    setProperty("internalEvidenceReferences",accepted);
    setAccessibleDescription(title_->text() + " · " + change_->text() + " · " + baseline_->text()+(currencyStatus_->text().isEmpty()?QString{}:" · "+currencyStatus_->text())
        );
}
void PortfolioDayView::setCoinPerformance(const CoinPerformanceView& view, const PortfolioMoneyDisplay& display) {
    const bool shown = view.inScope;
    coinTitle_->setVisible(shown); coinValue_->setVisible(shown);
    coinNote_->setVisible(shown && view.today.available);
    if (!shown) { coinValue_->clear(); coinValue_->setAccessibleName({}); coinNote_->clear(); return; }
    const auto& today = view.today;
    if (today.available) {
        // decimal::compare is unsigned; the sign is read from the exact PnL string.
        const bool negative = today.pnl.startsWith('-');
        const int direction = negative ? -1 : decimal::normalize(today.pnl) == "0" ? 0 : 1;
        coinValue_->setText((direction > 0 ? QStringLiteral("▲ +") : direction < 0 ? QStringLiteral("▼ ") : QString{}) +
            display.amount(today.pnl) + " " + display.unit());
        changeTone(coinValue_, true, direction);
        // The start time is data; what the figure means is behind ⓘ.
        coinNote_->setText(tr("오늘 %1부터").arg(QDateTime::fromMSecsSinceEpoch(today.fromMs, QTimeZone(9 * 3600)).toString("HH:mm")));
    } else {
        coinValue_->setText(today.reason); changeTone(coinValue_, false);
        coinNote_->clear();
    }
    coinValue_->setAccessibleName(coinTitle_->text() + " · " + coinValue_->text());
}
void PortfolioDayView::clear(const QString& reason) {
    refreshPending_ = false;
    hasSettledHistory_ = false;
    setMinimumHeight(0);
    details_->setMinimumHeight(0);
    comparisonAvailable_ = false; setEvidenceReferences({});
    coinTitle_->hide(); coinValue_->hide(); coinValue_->clear(); coinValue_->setAccessibleName({}); coinNote_->hide(); coinNote_->clear();
    currencyStatus_->clear();currencyStatus_->hide();
    info_->clearPrivate();bars_->setGroups({});chart_->direction=MovementDirection::Unknown;
    title_->setText(DefaultLabel); change_->setText(reason); changeTone(change_,false);
    baseline_->clear(); baseline_->hide(); contributions_->clear(); contributions_->hide(); chart_->setPoints({}); setAccessibleDescription(reason);
    refreshDetailsToggle();
}
} // namespace keefi
