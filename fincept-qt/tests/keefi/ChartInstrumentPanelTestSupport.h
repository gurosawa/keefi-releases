#pragma once

#include <QAbstractButton>
#include <QCoreApplication>
#include <QWidget>

namespace keefi::test {

inline QWidget* openInstrumentPanel(QWidget& workspace) {
    auto* button = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    if (!button || !button->isVisible()) return nullptr;
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    if (panel && panel->isVisible()) return panel;

    button->click();
    QCoreApplication::processEvents();
    panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    return panel && panel->isVisible() ? panel : nullptr;
}

} // namespace keefi::test
