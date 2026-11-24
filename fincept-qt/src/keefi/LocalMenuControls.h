#pragma once
#include "keefi/CompactControls.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QMenu>
#include <QPainter>
#include <QToolButton>

namespace keefi {
// Keep QMenu actions, check states and keyboard navigation; share the same
// surface, hover and spacing as LocalComboBox's in-app popup.
inline void applyLocalMenuStyle(QMenu* menu) {
    if (!menu) return;
    menu->setFont(KeefiFonts::font(FontRole::Body15));
    setThemeStyle(menu, QStringLiteral(R"(
        QMenu { background: ${input}; color: ${text}; border: 1px solid ${field};
            border-radius: 6px; padding: 4px; }
        QMenu::item { min-height: 36px; padding: 4px 18px 4px 30px;
            margin: 1px 4px; border-radius: 4px; }
        QMenu::item:selected { background: ${surface}; color: ${text}; }
        QMenu::item:disabled { color: ${text3}; }
        QMenu::separator { height: 1px; background: ${rule}; margin: 6px 10px; }
    )"));
    bindThemePresentation(menu, [menu] { menu->setPalette(KeefiTheme::palette()); });
}

// A styled QToolButton, with the same line chevron as LocalComboBox. QToolButton
// still owns opening/closing the menu and its keyboard/accessibility actions.
class LocalMenuButton final : public QToolButton {
  public:
    explicit LocalMenuButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setFont(KeefiFonts::font(FontRole::Body15));
        setMinimumSize(44, 44);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setThemeStyle(this, QStringLiteral(R"(
            QToolButton { background: ${surface}; color: ${text}; border: 1px solid ${rule};
                border-radius: 10px; padding: 8px 32px 8px 16px; }
            QToolButton:hover { border-color: ${field}; }
            QToolButton:focus, QToolButton:pressed { border-color: ${accent}; }
            QToolButton:disabled { color: ${text3}; border-color: ${surface}; }
            QToolButton::menu-indicator { image: none; width: 0; height: 0; }
        )"));
        bindThemePresentation(this, [this] { update(); });
    }

    void setCompact() {
        setProperty("keefiCompactMenu", true);
        applyCompactButton(this);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);
        if (!menu()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(isEnabled() ? themeColors().accent : themeColors().text3,
                            1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        const qreal x = width() - (property("keefiCompactMenu").toBool() ? 13 : 18), y = height() / 2.0;
        const qreal direction = menu()->isVisible() ? -1 : 1;
        painter.drawPolyline(QPolygonF{{x - 4, y - 2 * direction}, {x, y + 2 * direction},
                                      {x + 4, y - 2 * direction}});
    }
};
} // namespace keefi
