#include "keefi/ChartMarketLedger.h"
#include <QRegularExpression>
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QBoxLayout>
#include <QLabel>
#include <QFontMetricsF>
#include <cmath>
#include <QPainter>
#include <limits>

namespace keefi {
namespace {
// Display only: thousands separators everywhere; values of 1,000 or more keep two decimals.
QString ledgerNumber(const QString& value) {
    static const QRegularExpression plain(QStringLiteral("\\A(-?)([0-9]+)(?:\\.([0-9]+))?\\z"));
    const auto match = plain.match(value);
    if (!match.hasMatch()) return value;
    auto whole = match.captured(2), fraction = match.captured(3);
    if (whole.size() >= 4 && fraction.size() > 2) {
        bool ok = false; const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number)) return value;
        const auto rounded = QString::number(std::abs(number), 'f', 2);
        whole = rounded.section('.', 0, 0); fraction = rounded.section('.', 1, 1);
    }
    for (int i = int(whole.size()) - 3; i > 0; i -= 3) whole.insert(i, ',');
    return match.captured(1) + whole + (fraction.isEmpty() ? QString{} : "." + fraction);
}
class LedgerSpark final : public QWidget {
  public:
    explicit LedgerSpark(QWidget* parent) : QWidget(parent) {
        setFixedSize(110, 24);
        bindThemePresentation(this, [this] { update(); });
    }
    void setSegments(const QList<QPolygonF>& segments) {
        segments_ = segments;
        int observations = 0, paths = 0;
        for (const auto& segment : segments_) { observations += segment.size(); if (segment.size() > 1) ++paths; }
        setProperty("observationCount", observations);
        setProperty("segmentCount", paths);
        setAccessibleName(tr("실제 관측 %1개 · 연속 구간 %2개").arg(observations).arg(paths));
        setVisible(paths > 0);
        update();
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        double low = std::numeric_limits<double>::max(), high = -low, first = low, last = high;
        for (const auto& segment : segments_) for (const auto& point : segment) {
            low = qMin(low, point.y()); high = qMax(high, point.y()); first = qMin(first, point.x()); last = qMax(last, point.x());
        }
        if (last <= first) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(themeColors().text2, 1.2));
        for (const auto& segment : segments_) {
            if (segment.size() < 2) continue;
            QPolygonF path;
            for (const auto& point : segment) path.append(QPointF(1 + (point.x() - first) / (last - first) * (width() - 2),
                high > low ? height() - 2 - (point.y() - low) / (high - low) * (height() - 4) : height() / 2.0));
            painter.drawPolyline(path);
        }
    }
  private:
    QList<QPolygonF> segments_;
};
QLabel* label(QWidget* parent, const QString& name, FontRole role) {
    auto* result = new QLabel(parent); result->setObjectName(name); result->setTextFormat(Qt::PlainText);
    result->setFont(KeefiFonts::font(role)); result->setMinimumWidth(0); return result;
}
}
ChartMarketLedger::ChartMarketLedger(QWidget* parent) : QWidget(parent) {
    setObjectName("chartMarketLedger");
    auto* rows = new QVBoxLayout(this); rows->setContentsMargins(0, 0, 0, 0); rows->setSpacing(0);
    setThemeStyle(this, "QWidget[ledgerRow=true]{border-top:1px solid ${rule};background:transparent;} QLabel{border:0;color:${text};}");
}
void ChartMarketLedger::setRows(const QList<ChartLedgerRow>& rows) {
    QStringList ids; for (const auto& row : rows) ids.append(row.id);
    if (ids != rowIds_) {
        while (auto* item = layout()->takeAt(0)) { delete item->widget(); delete item; }
        rowIds_ = ids;
        for (const auto& row : rows) {
            auto* line = new QWidget(this); line->setProperty("ledgerRow", true);
            auto* horizontal = new QHBoxLayout(line); horizontal->setContentsMargins(0, 12, 0, 12); horizontal->setSpacing(8);
            auto* title = label(line, "chartLedgerLabel_" + row.id, FontRole::Body15); title->setWordWrap(true);
            horizontal->addWidget(title, 1);
            horizontal->addWidget(label(line, "chartLedgerValue_" + row.id, FontRole::Value22));
            horizontal->addWidget(label(line, "chartLedgerUnit_" + row.id, FontRole::Number11));
            auto* spark = new LedgerSpark(line); spark->setObjectName("chartLedgerSpark_" + row.id); horizontal->addWidget(spark);
            layout()->addWidget(line);
        }
    }
    qreal unitWidth = 0;
    const QFontMetricsF unitMetrics(KeefiFonts::font(FontRole::Number11));
    for (const auto& row : rows) unitWidth = qMax(unitWidth, unitMetrics.horizontalAdvance(row.unit));
    for (const auto& row : rows) {
        findChild<QLabel*>("chartLedgerLabel_" + row.id)->setText(row.label);
        findChild<QLabel*>("chartLedgerValue_" + row.id)->setText(row.value.isEmpty() ? QStringLiteral("—") : ledgerNumber(row.value));
        auto* unit = findChild<QLabel*>("chartLedgerUnit_" + row.id); unit->setText(row.unit);
        unit->setFixedWidth(int(std::ceil(unitWidth))); unit->setVisible(unitWidth > 0);
        static_cast<LedgerSpark*>(findChild<QWidget*>("chartLedgerSpark_" + row.id))->setSegments(row.segments);
    }
}
}
