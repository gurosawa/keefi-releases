#pragma once

#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"

#include <QAbstractButton>

namespace keefi {
inline constexpr int CompactButtonHeight = 28;
inline constexpr int CompactInputHeight = 32;

inline void applyCompactButton(QAbstractButton* button, bool iconOnly = false) {
    button->setMinimumWidth(iconOnly ? CompactButtonHeight : 0);
    if (iconOnly) button->setMaximumWidth(CompactButtonHeight);
    button->setFixedHeight(CompactButtonHeight);
    button->setFont(KeefiFonts::font(FontRole::Secondary14));
    button->setFocusPolicy(Qt::StrongFocus);
    button->setCursor(Qt::PointingHandCursor);
    const int padding = iconOnly ? 0 : 10;
    setThemeStyle(button, QStringLiteral(R"(
        QAbstractButton { background: ${bg}; color: ${text}; border: 1px solid ${rule};
            border-radius: 6px; padding: 1px %1px; }
        QAbstractButton:hover { background: ${surface}; color: ${text}; }
        QAbstractButton:pressed { background: ${rule}; }
        QAbstractButton:checked, QAbstractButton[primary="true"] {
            background: ${text}; color: ${bg}; font-weight: 600; }
        QAbstractButton:focus { border: 2px solid ${accent}; padding: 0 %2px; }
        QAbstractButton[tone="error"]:enabled { color: ${danger}; }
        QAbstractButton:disabled { background: transparent; color: ${text3}; border: 1px dotted ${field}; }
        QToolButton[keefiCompactMenu="true"] { padding-right: 25px; }
        QToolButton[keefiCompactMenu="true"]:focus { padding-right: 24px; }
        QToolButton[keefiCompactMenu="true"]::menu-indicator { image: none; width: 0; height: 0; }
    )").arg(iconOnly ? 0 : padding + 1).arg(padding));
}

inline void applyCompactInput(QWidget* input, int height = CompactInputHeight) {
    input->setProperty("keefiCompactInput", true);
    input->setFixedHeight(height);
    input->setFont(KeefiFonts::font(FontRole::Body15));
    setThemeStyle(input, QStringLiteral(R"(
        QWidget[keefiCompactInput="true"] { background: ${input}; color: ${text};
            border: 2px solid ${field}; border-radius: 6px; padding: 0 10px;
            min-height: %1px; max-height: %1px; }
        QWidget[keefiCompactInput="true"]:focus { border-color: ${accent}; }
        QWidget[keefiCompactInput="true"][error="true"] { border-color: ${danger}; }
        QWidget[keefiCompactInput="true"]:disabled { color: ${text3}; border: 2px dotted ${field}; }
    )").arg(height - 4));
}
} // namespace keefi
