#include "keefi/ChartWorkspace.h"
#include "keefi/CoinTapeView.h"
#include <QAction>
#include <QBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>

namespace keefi {
bool ChartWorkspace::coinTapeVisible() const { return tape_ && tape_->isVisible(); }

void ChartWorkspace::updateCoinTapeVisibility() {
    const bool visible = coinTapeVisible();
    if (visible == coinTapeShown_) return;
    coinTapeShown_ = visible;
    emit coinTapeVisibilityChanged(visible);
}

void ChartWorkspace::updateDetailsToggleText() {
    if (!detailsToggle_) return;
    const bool expanded = detailsToggle_->isChecked();
    // Beside the chart, the disclosure holds the position card alone.
    detailsToggle_->setText(sidePlaced_ ? (expanded ? tr("포지션 접기") : tr("포지션 펼치기"))
                                        : (expanded ? tr("포지션 · 지금 상태 접기") : tr("포지션 · 지금 상태 펼치기")));
}

void ChartWorkspace::layoutSidePanel() {
    if (!sideSplitter_ || !sidePanel_ || !chartColumn_ || !scroll_ || !positions_ || sideLayoutActive_) return;
    const QScopedValueRollback guard(sideLayoutActive_, true);
    const bool grid = gridSelector_ && gridSelector_->currentData().toInt() > 1;
    const int windowWidth = window()->width();
    const auto* content = scroll_->widget() ? scroll_->widget()->layout() : nullptr;
    const int inset = content ? content->contentsMargins().left() + content->contentsMargins().right() : 0;
    const int room = scroll_->viewport()->width() - inset;
    const bool side = !grid && windowWidth >= SidePanelCollapseWidth &&
        room >= ChartColumnMinimumWidth + SidePanelMinimumWidth + sideSplitter_->handleWidth();
    bool moved = false;
    if (side != sidePlaced_) {
        if (sidePlaced_ && !sidePanel_->isHidden()) sideShare_.remember(*sideSplitter_);
        if (side) sideSplitter_->addWidget(sidePanel_);
        else if (auto* stacked = qobject_cast<QBoxLayout*>(positions_->layout())) stacked->addWidget(sidePanel_, 1);
        moved = true;
        sidePlaced_ = side;
    }
    const bool open = side && sideOpen_;
    // Both floors hold only while the panel stands beside the chart; stacked
    // under it, neither may widen the page past the window.
    chartColumn_->setMinimumWidth(open ? ChartColumnMinimumWidth : 0);
    sidePanel_->setMinimumWidth(open ? SidePanelMinimumWidth : 0);
    const bool appearing = open && sidePanel_->isHidden();
    sidePanel_->setVisible(side ? sideOpen_ : true);
    if (open) {
        const int extent = SplitterShare::extent(*sideSplitter_);
        if (extent >= ChartColumnMinimumWidth + SidePanelMinimumWidth) {
            if (!sideUserSized_) {
                const int preferred = windowWidth >= SidePanelWideWindowWidth ? SidePanelWideWidth : SidePanelDefaultWidth;
                const int target = qMax(SidePanelMinimumWidth, qMin(extent - ChartColumnMinimumWidth, preferred));
                const auto sizes = sideSplitter_->sizes();
                if (sizes.size() != 2 || sizes[1] != target || sizes[0] != extent - target) sideSplitter_->setSizes({extent - target, target});
            } else if (appearing || moved) {
                sideShare_.restore(*sideSplitter_, ChartColumnMinimumWidth, SidePanelMinimumWidth);
            }
        }
    }
    if (sidePanelAction_) {
        const QSignalBlocker block(sidePanelAction_);
        sidePanelAction_->setEnabled(side);
        sidePanelAction_->setChecked(sideOpen_);
    }
    updateDetailsToggleText();
    updateCoinTapeVisibility();
}
} // namespace keefi
