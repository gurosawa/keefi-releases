#pragma once
#include "keefi/CompactControls.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include <QAbstractItemModel>
#include <QComboBox>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QToolTip>
#include <QVector>

namespace keefi {
// G6 option C: a short choice (2-4 items) drawn as always-visible segments.
// It stays a QComboBox, so item data, enabled flags, currentIndexChanged and
// every caller keep their meaning; only presentation and input change.
// Items whose data is an empty string are placeholders ("선택하세요") and are
// never drawn, so a required choice shows nothing selected until picked.
// Arrow keys move focus only; Space/Enter selects. The wheel never changes it.
class SegmentedComboBox final : public QComboBox {
    Q_OBJECT
  public:
    explicit SegmentedComboBox(QWidget* parent = nullptr) : QComboBox(parent) {
        setFont(KeefiFonts::font(FontRole::Body15));
        bindThemePresentation(this,[this]{update();});
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        // As wide as its segments, never stretched; narrower space stacks them.
        QSizePolicy policy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setAccessibleDescription(tr("모든 선택지가 펼쳐진 단추 묶음입니다. 화살표로 이동하고 Space로 선택합니다."));
        for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
            connect(model(), signal, this, [this] { relayout(); });
        connect(model(), &QAbstractItemModel::dataChanged, this, [this] { relayout(); });
        connect(model(), &QAbstractItemModel::modelReset, this, [this] { relayout(); });
        connect(this, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (isSegment(index)) cursor_ = index;
            update();
        });
    }

    // The popup never opens: every choice is already visible.
    void showPopup() override {}
    void hidePopup() override {}
    void setCompact() {
        setProperty("keefiCompactSegment", true);
        setFont(KeefiFonts::font(FontRole::Secondary14));
        setMinimumTargetHeight(CompactButtonHeight);
    }
    void setMinimumTargetHeight(int height) {
        const int next = qMax(compact() ? CompactButtonHeight : 30, height);
        if (segmentHeight_ == next) return;
        segmentHeight_ = next;
        relayout();
    }
    bool isSegment(int row) const {
        if (row < 0 || row >= count()) return false;
        const auto value = itemData(row);
        return value.isValid() && !(value.typeId() == QMetaType::QString && value.toString().isEmpty());
    }
    bool isItemEnabled(int row) const {
        const auto* items = qobject_cast<const QStandardItemModel*>(model());
        return !items || !items->item(row) || items->item(row)->isEnabled();
    }
    // Where each drawn segment sits; empty rects for placeholders.
    QVector<QRect> segmentRects(int forWidth = -1) const {
        const int available = forWidth < 0 ? width() : forWidth;
        QVector<QRect> rects(count());
        const bool stack = horizontalWidth() > available;
        const int inset = padding();
        int x = inset, y = inset;
        for (int row = 0; row < count(); ++row) {
            if (!isSegment(row)) continue;
            if (stack) {
                rects[row] = QRect(inset, y, available - 2 * inset, segmentHeight_);
                y += segmentHeight_ + gap();
            } else {
                rects[row] = QRect(x, inset, segmentWidth(row), segmentHeight_);
                x += segmentWidth(row) + gap();
            }
        }
        return rects;
    }
    int segmentAt(const QPoint& point) const {
        const auto rects = segmentRects();
        for (int row = 0; row < rects.size(); ++row)
            if (!rects[row].isNull() && rects[row].contains(point)) return row;
        return -1;
    }
    int keyboardCursor() const { return cursor_; }

    QSize sizeHint() const override { return {horizontalWidth(), heightForWidth(horizontalWidth())}; }
    QSize minimumSizeHint() const override {
        int widest = 0;
        for (int row = 0; row < count(); ++row)
            if (isSegment(row)) widest = qMax(widest, segmentWidth(row));
        const int width = widest + 2 * padding();
        return {width, heightForWidth(width)};
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        const int rows = segmentCount();
        if (rows == 0 || horizontalWidth() <= width) return segmentHeight_ + 2 * padding();
        return rows * segmentHeight_ + (rows - 1) * gap() + 2 * padding();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool enabled = isEnabled();
        const bool error = property("error").toBool();
        const auto& colors = themeColors();
        const bool useCompactStyle = compact();
        const int pressedRow = property("keefiPressedSegment").toInt() - 1;
        const auto edge = useCompactStyle || !enabled ? colors.rule : colors.field;
        painter.setPen(QPen(error ? colors.danger : edge, 1));
        painter.setBrush(useCompactStyle ? colors.bg : enabled ? colors.input : colors.surface);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        const auto rects = segmentRects();
        const bool single = segmentCount() == 1;
        for (int row = 0; row < rects.size(); ++row) {
            if (rects[row].isNull()) continue;
            const QRectF box = QRectF(rects[row]).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool selected = row == currentIndex();
            const bool rowEnabled = enabled && isItemEnabled(row) && !single;
            QColor fill(Qt::transparent), border(Qt::transparent), text(rowEnabled ? themeColors().text2 : themeColors().text3);
            if (selected && (rowEnabled || (enabled && single))) {
                fill = themeColors().surface; border = single ? themeColors().field : themeColors().accent;
                text = (single ? themeColors().text2 : themeColors().text);
            } else if (selected) {
                fill = themeColors().surface; border = themeColors().rule; text = themeColors().text3;
            } else if (row == hover_ && rowEnabled) {
                fill = themeColors().surface; border = themeColors().field; text = themeColors().text;
            }
            const bool disabled = !enabled || !isItemEnabled(row);
            if (useCompactStyle) {
                fill = selected && !disabled ? colors.text : Qt::transparent;
                border = disabled ? colors.field : Qt::transparent;
                text = selected && !disabled ? colors.bg : colors.text3;
                if (rowEnabled && row == hover_ && !selected) {
                    fill = colors.surface;
                    text = colors.text;
                }
                if (rowEnabled && row == pressedRow) {
                    fill = colors.rule;
                    text = colors.text;
                }
            }
            painter.setPen(border.alpha() ? QPen(border, 1, useCompactStyle && disabled ? Qt::DotLine : Qt::SolidLine) : Qt::NoPen);
            painter.setBrush(fill);
            const int radius = useCompactStyle ? 6 : 4;
            painter.drawRoundedRect(box, radius, radius);
            if (hasFocus() && keyboardRing_ && row == cursor_) {
                auto focus = colors.accent;
                if (!useCompactStyle) focus.setAlpha(150);
                painter.setPen(QPen(focus, 2));
                painter.setBrush(Qt::NoBrush);
                const auto ring = useCompactStyle ? box.adjusted(0.5, 0.5, -0.5, -0.5) : box.adjusted(-1, -1, 1, 1);
                painter.drawRoundedRect(ring, 5, 5);
            }
            const QFont font = segmentFont(selected && (useCompactStyle ? !disabled : rowEnabled));
            painter.setFont(font);
            // Check and label are centred together so unselected labels do not shift.
            const qreal labelWidth = QFontMetricsF(font).horizontalAdvance(itemText(row)) + (selected ? CheckWidth : 0);
            const qreal left = qMax(box.left() + textPadding(), box.center().x() - labelWidth / 2);
            QRectF content(left, box.top(), box.right() - textPadding() - left, box.height());
            if (selected) {
                const auto check = useCompactStyle ? text : single || !rowEnabled ? colors.text3 : colors.accent;
                painter.setPen(QPen(check, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                const qreal cx = content.left(), cy = content.center().y();
                painter.drawPolyline(QPolygonF{{cx + 1, cy}, {cx + 4.5, cy + 3.5}, {cx + 11, cy - 3.5}});
                content.setLeft(content.left() + CheckWidth);
            }
            painter.setPen(text);
            painter.drawText(content, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, itemText(row));
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        event->accept();
        if (event->button() != Qt::LeftButton) return;
        setFocus(Qt::MouseFocusReason);
        keyboardRing_ = false;
        const auto row = segmentAt(event->position().toPoint());
        if (compact() && choosable(row)) setProperty("keefiPressedSegment", row + 1);
        choose(row);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (property("keefiPressedSegment").toInt() > 0) {
            setProperty("keefiPressedSegment", 0);
            update();
        }
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const int row = segmentAt(event->position().toPoint());
        if (row != hover_) { hover_ = row; update(); }
    }
    void leaveEvent(QEvent* event) override {
        hover_ = -1; update();
        QComboBox::leaveEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    void keyPressEvent(QKeyEvent* event) override {
        keyboardRing_ = true;
        switch (event->key()) {
        case Qt::Key_Left: case Qt::Key_Up: moveCursor(-1); break;
        case Qt::Key_Right: case Qt::Key_Down: moveCursor(1); break;
        case Qt::Key_Home: cursor_ = -1; moveCursor(1); break;
        case Qt::Key_End: cursor_ = count(); moveCursor(-1); break;
        case Qt::Key_Space: case Qt::Key_Return: case Qt::Key_Enter:
            if (!event->isAutoRepeat()) choose(cursor_);
            break;
        default: event->ignore(); return; // Tab, Escape and shortcuts belong to the dialog.
        }
        event->accept();
    }
    void keyReleaseEvent(QKeyEvent* event) override { event->ignore(); }
    void focusInEvent(QFocusEvent* event) override {
        cursor_ = isSegment(currentIndex()) ? currentIndex() : -1;
        if (cursor_ < 0) moveCursor(1);
        // The focus ring marks keyboard position only, never a mouse click.
        keyboardRing_ = event->reason() != Qt::MouseFocusReason;
        QComboBox::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent* event) override { QComboBox::focusOutEvent(event); update(); }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            const auto* help = static_cast<QHelpEvent*>(event);
            const int row = segmentAt(help->pos());
            QString tip = toolTip();
            if (segmentCount() == 1) tip = tr("이 항목은 선택지가 하나뿐입니다.");
            else if (row >= 0 && !isItemEnabled(row)) tip = itemData(row, Qt::ToolTipRole).toString();
            if (tip.isEmpty()) QToolTip::hideText(); else QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QComboBox::event(event);
    }

  private:
    static constexpr int CheckWidth = 18;
    int segmentHeight_ = 30;
    bool compact() const { return property("keefiCompactSegment").toBool(); }
    int padding() const { return compact() ? 2 : 3; }
    int gap() const { return compact() ? 2 : 3; }
    int textPadding() const { return compact() ? 10 : 14; }
    QFont segmentFont(bool strong) const {
        if (compact()) return KeefiFonts::font(strong ? FontRole::SecondaryStrong14 : FontRole::Secondary14);
        return KeefiFonts::font(strong ? FontRole::BodyStrong15 : FontRole::Body15);
    }
    void relayout() { updateGeometry(); update(); }
    int segmentCount() const {
        int total = 0;
        for (int row = 0; row < count(); ++row) total += isSegment(row);
        return total;
    }
    int segmentWidth(int row) const {
        return QFontMetrics(segmentFont(true)).horizontalAdvance(itemText(row)) + 2 * textPadding() + CheckWidth;
    }
    int horizontalWidth() const {
        int total = 2 * padding(), rows = 0;
        for (int row = 0; row < count(); ++row)
            if (isSegment(row)) { total += segmentWidth(row); ++rows; }
        return total + qMax(0, rows - 1) * gap();
    }
    bool choosable(int row) const { return isEnabled() && isSegment(row) && isItemEnabled(row) && segmentCount() > 1; }
    void moveCursor(int step) {
        for (int row = cursor_ + step; row >= 0 && row < count(); row += step)
            if (choosable(row)) { cursor_ = row; update(); return; }
    }
    void choose(int row) {
        if (!choosable(row)) return;
        cursor_ = row;
        if (row != currentIndex()) {
            setCurrentIndex(row);
            emit activated(row);
            emit textActivated(itemText(row));
        }
        update();
    }
    int hover_ = -1;
    int cursor_ = -1;
    bool keyboardRing_ = false;
};
} // namespace keefi
