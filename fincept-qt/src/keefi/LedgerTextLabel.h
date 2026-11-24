#pragma once
#include "keefi/KeefiFonts.h"
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextLayout>
#include <memory>
#include <vector>

namespace keefi {
// Keep QLabel's plain text, sizing, accessibility and selection/copy contract.
// Only nonselectable ink is shaped here. Selectable content must use Qt's
// native rich text document so its glyph positions and hit testing agree.
class LedgerTextLabel final : public QLabel {
  public:
    explicit LedgerTextLabel(const QString& text, FontRole role, QWidget* parent = nullptr)
        : QLabel(text, parent) { setTextFormat(Qt::PlainText); setLedgerRole(role); }
    explicit LedgerTextLabel(FontRole role, QWidget* parent = nullptr)
        : LedgerTextLabel(QString(), role, parent) {}
    void setLedgerRole(FontRole role) { setFont(KeefiFonts::font(role)); update(); }
  protected:
    void paintEvent(QPaintEvent* event) override {
        // A custom paint layout cannot share QLabel's private hit-testing map.
        // Always defer selectable content, including before a selection starts.
        const bool elide = property("elideRight").toBool() && !wordWrap();
        const bool elideRichText = elide && textFormat() == Qt::RichText;
        if (textInteractionFlags().testFlag(Qt::TextSelectableByMouse) ||
            textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard) ||
            (textFormat() != Qt::PlainText && !elideRichText) || text().isEmpty()) {
            QLabel::paintEvent(event); return;
        }
        QPainter painter(this);QStyleOption option;option.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
        painter.end();QFrame::paintEvent(event);painter.begin(this);
        auto area=contentsRect().adjusted(margin(),margin(),-margin(),-margin());
        const int inset=indent()>=0?indent():(frameWidth()>0?fontMetrics().horizontalAdvance(QLatin1Char('x'))/2:0);
        if(alignment().testFlag(Qt::AlignLeft))area.adjust(inset,0,0,0);
        if(alignment().testFlag(Qt::AlignRight))area.adjust(0,0,-inset,0);
        painter.setClipRect(area);painter.setPen(palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::WindowText));
        std::vector<std::unique_ptr<QTextLayout>> paragraphs;
        qreal height=0;
        const auto appendParagraph = [&](const QString& paragraph, const QList<QTextLayout::FormatRange>& formats) {
            auto layout = std::make_unique<QTextLayout>(paragraph, font());
            layout->setFormats(formats);
            const auto positionLine = [&](QTextLine line) {
                const qreal x = alignment().testFlag(Qt::AlignRight) ? area.width() - line.naturalTextWidth()
                    : alignment().testFlag(Qt::AlignHCenter) ? (area.width() - line.naturalTextWidth()) / 2 : 0;
                line.setPosition(QPointF(x, height));
                height += line.height();
            };
            if (elide) {
                layoutElidedLine(*layout, paragraph, area.width());
                if (layout->lineCount() != 0) positionLine(layout->lineAt(0));
            } else {
                QTextOption option;
                option.setWrapMode(wordWrap() ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
                layout->setTextOption(option);
                layout->beginLayout();
                for (auto line = layout->createLine(); line.isValid(); line = layout->createLine()) {
                    line.setLineWidth(wordWrap() ? qMax(1, area.width()) : 1000000);
                    positionLine(line);
                }
                layout->endLayout();
            }
            if (layout->lineCount() == 0) height += fontMetrics().height();
            paragraphs.push_back(std::move(layout));
        };
        if (elideRichText) {
            QTextDocument document;
            document.setDefaultFont(font());
            document.setHtml(text());
            for (auto block = document.firstBlock(); block.isValid(); block = block.next()) {
                QList<QTextLayout::FormatRange> formats;
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (fragment.isValid())
                        formats.append({fragment.position() - block.position(), fragment.length(), fragment.charFormat()});
                }
                appendParagraph(block.text(), formats);
            }
        } else {
            for (const auto& paragraph : text().split(QLatin1Char('\n')))
                appendParagraph(paragraph, KeefiFonts::numberRuns(paragraph));
        }
        const qreal y=alignment().testFlag(Qt::AlignBottom)?area.bottom()+1-height:alignment().testFlag(Qt::AlignVCenter)?area.top()+(area.height()-height)/2:area.top();
        for(const auto& layout:paragraphs)layout->draw(&painter,QPointF(area.left(),y));
    }

  private:
    static QTextLine layoutFirstLine(QTextLayout& layout, qreal width, QTextOption::WrapMode wrapMode) {
        QTextOption option;
        option.setWrapMode(wrapMode);
        layout.setTextOption(option);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid()) line.setLineWidth(width);
        layout.endLayout();
        return line;
    }

    static void layoutElidedLine(QTextLayout& layout, const QString& paragraph, qreal width) {
        const auto fullLine = layoutFirstLine(layout, qMax(qreal(0), width), QTextOption::NoWrap);
        if (!fullLine.isValid() || fullLine.naturalTextWidth() <= width) return;

        // Find a shaped prefix with room for the marker, then recheck the
        // joined line because its shaping may differ at the new boundary.
        const auto marker = QStringLiteral("…");
        const qreal markerWidth = QFontMetricsF(layout.font()).horizontalAdvance(marker);
        const auto prefix = layoutFirstLine(layout, qMax(qreal(0), width - markerWidth), QTextOption::WrapAnywhere);
        QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, paragraph);
        boundaries.setPosition(prefix.textLength());
        if (!boundaries.isAtBoundary()) boundaries.toPreviousBoundary();
        const auto originalFormats = layout.formats();
        for (auto end = boundaries.position(); end >= 0; end = boundaries.toPreviousBoundary()) {
            QList<QTextLayout::FormatRange> formats;
            // Cutting a ticker must preserve its original font, even when the
            // remaining prefix alone would match a different formatting run.
            for (auto range : originalFormats) {
                if (range.start >= end) break;
                range.length = qMin(range.length, int(end) - range.start);
                formats.append(range);
            }
            layout.setText(paragraph.left(end) + marker);
            layout.setFormats(formats);
            const auto candidate = layoutFirstLine(layout, qMax(qreal(0), width), QTextOption::NoWrap);
            if (candidate.naturalTextWidth() <= width) return;
        }
        layout.setText(QString{});
        layout.setFormats({});
    }
};
} // namespace keefi
