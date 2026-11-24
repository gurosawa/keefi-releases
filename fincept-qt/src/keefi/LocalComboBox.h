#pragma once
#include "keefi/CompactControls.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include <QComboBox>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QListView>
#include <QPalette>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStylePainter>
#include <QToolTip>

namespace keefi {
// G6 option A popup rows: an orange check marks the combo's current item,
// hover/keyboard position is a grey fill, and a disabled item with a
// ToolTipRole reason shows that reason as a second line.
class LocalComboItemDelegate final : public QStyledItemDelegate {
  public:
    LocalComboItemDelegate(QComboBox* box, QObject* parent) : QStyledItemDelegate(parent), box_(box) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 36));
        if (!(index.flags() & Qt::ItemIsEnabled) && !index.data(Qt::ToolTipRole).toString().isEmpty())
            size.setHeight(size.height() + QFontMetrics(option.font).height());
        return size;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool enabled = index.flags() & Qt::ItemIsEnabled;
        const bool current = index.row() == box_->currentIndex();
        const QRectF row = QRectF(option.rect).adjusted(4, 1, -4, -1);
        if (enabled && (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(themeColors().surface);
            painter->drawRoundedRect(row, 4, 4);
        }
        if (current) {
            painter->setPen(QPen(themeColors().accent, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const qreal x = row.left() + 9, y = row.top() + 18;
            painter->drawPolyline(QPolygonF{{x, y}, {x + 3.5, y + 3.5}, {x + 10, y - 3.5}});
        }
        const QFont font = KeefiFonts::font(current ? FontRole::BodyStrong15 : FontRole::Body15);
        painter->setFont(font);
        painter->setPen((!enabled ? themeColors().text3 : current ? themeColors().text : themeColors().text2));
        const QRectF text = row.adjusted(30, 0, -10, 0);
        const QString reason = enabled ? QString() : index.data(Qt::ToolTipRole).toString();
        if (reason.isEmpty()) {
            painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
        } else {
            const int line = QFontMetrics(font).height();
            painter->drawText(text.adjusted(0, 6, 0, 0), Qt::AlignTop | Qt::AlignLeft, index.data().toString());
            painter->setPen(themeColors().text3);
            painter->drawText(text.adjusted(0, 6 + line, 0, 0), Qt::AlignTop | Qt::AlignLeft, reason);
        }
        painter->restore();
    }

  private:
    QComboBox* box_;
};

// A widget list on every platform: macOS menu mode reserves checkmark space,
// shrinks the popup and paints native menu caps outside the styled item view.
class LocalComboBox : public QComboBox {
  public:
    explicit LocalComboBox(QWidget* parent = nullptr) : QComboBox(parent) {
        setFont(KeefiFonts::font(FontRole::Body15));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Set the outer minimum on the widget so a caller can raise it.
        setMinimumHeight(36);
        setSizeAdjustPolicy(QComboBox::AdjustToContents);
        setMaxVisibleItems(12);
        setPalette(popupPalette());
        setThemeStyle(this, QStringLiteral(R"(
            QComboBox { combobox-popup: 0; background: ${input}; color: ${text}; border-radius: 4px;
                border: 1px solid ${field}; padding: 7px 34px 7px 12px; }
            QComboBox:hover { background: ${surface}; border-color: ${text3}; }
            QComboBox:focus, QComboBox:on { border-color: ${accent}; }
            QComboBox[error="true"] { border-color: ${danger}; }
            QComboBox:disabled { color: ${text3}; background: ${surface}; border-color: ${rule}; }
            QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right;
                width: 30px; border: 0; background: transparent; }
            QComboBox::down-arrow { image: none; width: 0; height: 0; }
        )"));
        auto* list = new QListView(this);
        list->setPalette(popupPalette());
        list->setTextElideMode(Qt::ElideNone);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setThemeStyle(list, QStringLiteral(R"(
            QListView { background: ${input}; color: ${text}; border: 1px solid ${field}; border-radius: 6px;
                padding: 4px; outline: 0; }
        )"));
        setView(list);
        list->setMouseTracking(true);
        setItemDelegate(new LocalComboItemDelegate(this, list));
        bindThemePresentation(this, [this] {
            setPalette(popupPalette());
            view()->setPalette(popupPalette());
            view()->window()->setPalette(popupPalette());
            view()->viewport()->update();
            update();
        });
    }

    void setCompact(int extraRightPadding = 0) {
        setProperty("keefiCompactCombo", true);
        setFont(KeefiFonts::font(FontRole::Secondary14));
        setFixedHeight(CompactButtonHeight);
        setMaximumWidth(CompactWidth);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
        setThemeStyle(this, QStringLiteral(R"(
            QComboBox { combobox-popup: 0; background: ${bg}; color: ${text}; border-radius: 6px;
                border: 1px solid ${rule}; padding: 1px %1px 1px 10px; }
            QComboBox:hover { background: ${surface}; }
            QComboBox:pressed { background: ${rule}; }
            QComboBox:focus, QComboBox:on { border: 2px solid ${accent}; padding: 0 %2px 0 9px; }
            QComboBox[error="true"] { border-color: ${danger}; }
            QComboBox:disabled { background: transparent; color: ${text3}; border: 1px dotted ${field}; }
            QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: top right;
                width: 24px; border: 0; background: transparent; }
            QComboBox::down-arrow { image: none; width: 0; height: 0; }
        )").arg(1 + extraRightPadding).arg(extraRightPadding));
    }

    QSize sizeHint() const override {
        const auto hint = QComboBox::sizeHint();
        return isCompact() ? QSize(qMin(hint.width(), CompactWidth), CompactButtonHeight) : hint;
    }

    QSize minimumSizeHint() const override {
        return isCompact() ? QSize(64, CompactButtonHeight) : QComboBox::minimumSizeHint();
    }

    void showPopup() override {
        ensurePolished();
        view()->setFont(font());
        updatePopupWidth();
        auto* popup = view()->window();
        popup->setPalette(popupPalette());
        popup->setAutoFillBackground(true);
        QComboBox::showPopup();
        updatePopupWidth();
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ToolTip && isCompact()) {
            QStyleOptionComboBox option;
            initStyleOption(&option);
            if (closedText(option) != option.currentText) {
                const auto tooltip = QStringLiteral("<qt>%1</qt>").arg(option.currentText.toHtmlEscaped());
                QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), tooltip, this);
                return true;
            }
        }
        return QComboBox::event(event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QComboBox::resizeEvent(event);
        // A first-show form layout can settle while its popup is already open.
        if (view()->isVisible()) updatePopupWidth();
    }
    void paintEvent(QPaintEvent* event) override {
        const bool compact = isCompact();
        if (compact) {
            QStylePainter stylePainter(this);
            QStyleOptionComboBox option;
            initStyleOption(&option);
            stylePainter.drawComplexControl(QStyle::CC_ComboBox, option);
            option.currentText = closedText(option);
            stylePainter.drawControl(QStyle::CE_ComboBoxLabel, option);
        } else {
            QComboBox::paintEvent(event);
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen((isEnabled() ? themeColors().accent : themeColors().text3), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        const qreal x = width() - (compact ? 13 : 18), y = height() / 2.0;
        // A line chevron that points up while the list is open.
        const qreal direction = view()->isVisible() ? -1 : 1;
        painter.drawPolyline(QPolygonF{{x - 4, y - 2 * direction}, {x, y + 2 * direction}, {x + 4, y - 2 * direction}});
    }

  private:
    static constexpr int CompactWidth = 260;
    bool isCompact() const { return property("keefiCompactCombo").toBool(); }

    QString closedText(const QStyleOptionComboBox& option) const {
        const auto area = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        const int iconWidth = option.currentIcon.isNull() ? 0 : option.iconSize.width() + 4;
        return fontMetrics().elidedText(option.currentText, Qt::ElideRight, qMax(0, area.width() - iconWidth));
    }

    void updatePopupWidth() {
        auto* list = view();
        int contentWidth = 0;
        const QFontMetrics metrics(KeefiFonts::font(FontRole::Body15));
        const QFontMetrics selectedMetrics(KeefiFonts::font(FontRole::BodyStrong15));
        for (int row = 0; row < count(); ++row)
            contentWidth = qMax(contentWidth, qMax(metrics.horizontalAdvance(itemText(row)), selectedMetrics.horizontalAdvance(itemText(row))));
        // Include the check column, item padding, popup borders and room for a vertical scrollbar.
        const int anchorWidth = isCompact() ? qMax(CompactWidth, width()) : width();
        const int popupWidth = qMax(anchorWidth, contentWidth + 56 + list->verticalScrollBar()->sizeHint().width());
        list->setMinimumWidth(popupWidth - 2);
        auto* popup = list->window();
        popup->setMinimumWidth(popupWidth);
        if (popup->isVisible()) popup->resize(popupWidth, popup->height());
    }
    static QPalette popupPalette() {
        QPalette result;
        for (auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::Button})
            result.setColor(role, themeColors().input);
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
            result.setColor(role, themeColors().text);
            result.setColor(QPalette::Disabled, role, themeColors().text3);
        }
        result.setColor(QPalette::Highlight, themeColors().surface);
        result.setColor(QPalette::HighlightedText, themeColors().text);
        return result;
    }
};
} // namespace keefi
