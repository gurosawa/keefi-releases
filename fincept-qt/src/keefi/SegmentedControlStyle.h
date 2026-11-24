#pragma once
#include "keefi/CompactControls.h"
#include "keefi/KeefiTheme.h"
#include <QPushButton>
#include <QTabBar>

namespace keefi {
// Selection keeps its own fill even when keyboard focus stays on another item.
inline void applySegmentedControlStyle(QPushButton* button,int minimumHeight=44){
    if(!button)return;
    if (minimumHeight == CompactButtonHeight) {
        applyCompactButton(button);
        setThemeStyle(button, QStringLiteral(R"(
            QPushButton { border: 2px solid transparent; border-radius: 6px;
                background: transparent; color: ${text3}; padding: 0 10px; }
            QPushButton:hover { color: ${text}; background: ${surface}; }
            QPushButton:pressed { color: ${text}; background: ${rule}; }
            QPushButton:checked { background: ${text}; color: ${bg}; font-weight: 600; }
            QPushButton:focus { border-color: ${accent}; }
            QPushButton:disabled { background: transparent; color: ${text3}; border: 2px dotted ${field}; }
        )"));
        return;
    }
    button->setFocusPolicy(Qt::StrongFocus);
    button->setMinimumSize(44,minimumHeight);
    button->setCursor(Qt::PointingHandCursor);
    setThemeStyle(button,
        "QPushButton{border:2px solid transparent;border-radius:6px;background:transparent;color:${text3};padding:0 8px;} "
        "QPushButton:hover{color:${text};background:${surface};} "
        "QPushButton:checked{background:${text};color:${bg};} "
        "QPushButton:focus{border-color:${accent};} "
        "QPushButton:disabled{background:transparent;color:${text3};}");
}
// Preserve QTabBar's tab roles and arrow-key selection while matching the
// selection/focus treatment of the existing segmented buttons.
inline void applySegmentedControlStyle(QTabBar* tabs,int minimumHeight=44){
    if(!tabs)return;
    tabs->setFocusPolicy(Qt::StrongFocus);
    tabs->setDrawBase(false);
    if (minimumHeight == CompactButtonHeight) {
        tabs->setFixedHeight(CompactButtonHeight);
        tabs->setFont(KeefiFonts::font(FontRole::Secondary14));
        setThemeStyle(tabs, QStringLiteral(R"(
            QTabBar::tab { border: 2px solid transparent; border-radius: 6px; background: transparent;
                color: ${text3}; padding: 0 10px; height: 24px; margin-right: 2px; }
            QTabBar::tab:hover { color: ${text}; background: ${surface}; }
            QTabBar::tab:pressed { color: ${text}; background: ${rule}; }
            QTabBar::tab:selected { background: ${text}; color: ${bg}; font-weight: 600; }
            QTabBar::tab:focus { border-color: ${accent}; }
            QTabBar::tab:disabled { background: transparent; color: ${text3}; border: 2px dotted ${field}; }
        )"));
        return;
    }
    tabs->setMinimumHeight(minimumHeight);
    setThemeStyle(tabs,QStringLiteral(
        "QTabBar::tab{border:2px solid transparent;border-radius:6px;background:transparent;color:${text3};padding:0 14px;min-height:%1px;margin-right:2px;} "
        "QTabBar::tab:hover{color:${text};background:${surface};} "
        "QTabBar::tab:selected{background:${text};color:${bg};} "
        "QTabBar::tab:focus{border-color:${accent};} "
        "QTabBar::tab:disabled{background:transparent;color:${text3};}").arg(qMax(0,minimumHeight-4)));
}
}
