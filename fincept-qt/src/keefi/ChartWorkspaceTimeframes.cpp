#include "keefi/ChartWorkspace.h"
#include "keefi/CompactControls.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalMenuControls.h"
#include <QAbstractButton>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>
#include <QtMath>
#include <cmath>

namespace keefi {
namespace {
// The single-colour ★ of one interval row: filled while that interval keeps a
// toolbar button. Pressing it leaves the list open. It takes no keyboard focus:
// keys stay with the list and act on its highlighted row.
class TimeframePinToggle final : public QAbstractButton {
  public:
    explicit TimeframePinToggle(QWidget* parent) : QAbstractButton(parent) {
        setCheckable(true);
        setFixedSize(32, 32);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        bindThemePresentation(this, [this] { update(); });
    }
    QSize sizeHint() const override { return {32, 32}; }

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
        return QAbstractButton::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto& colors = themeColors();
        const QRectF box = QRectF(rect()).adjusted(2, 2, -2, -2);
        if (isEnabled() && underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.surface);
            painter.drawRoundedRect(box, 6, 6);
        }
        QPolygonF star;
        const QPointF centre = QRectF(rect()).center();
        for (int point = 0; point < 10; ++point) {
            const qreal angle = -M_PI / 2 + point * M_PI / 5, radius = point % 2 ? 3.3 : 7.6;
            star << QPointF(centre.x() + radius * std::cos(angle), centre.y() + radius * std::sin(angle));
        }
        const QColor ink = isChecked() && isEnabled() ? colors.text : colors.text3;
        painter.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(isChecked() ? QBrush(ink) : QBrush(Qt::NoBrush));
        painter.drawPolygon(star);
    }
};

// Keys and highlight of the open list. ↑↓ move the highlighted row (QMenu),
// Enter chooses it (the row action's trigger) and Space toggles that row's ★
// with the list still open. Space never reaches QMenu, where some styles would
// choose the row instead. QMenu paints no highlight behind widget rows, so the
// row draws its own; the pointer entering a row highlights it as well.
class TimeframeListInput final : public QObject {
  public:
    explicit TimeframeListInput(QMenu* menu) : QObject(menu), menu_(menu) {
        menu->installEventFilter(this);
        connect(menu, &QMenu::hovered, this, &TimeframeListInput::showHighlight);
    }
    void watch(QWidgetAction* action) {
        if (auto* row = action->defaultWidget()) row->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != menu_) {
            if (event->type() == QEvent::Enter)
                for (auto* action : menu_->actions())
                    if (rowOf(action) == watched && menu_->activeAction() != action) menu_->setActiveAction(action);
            return false;
        }
        // QMenu may drop its highlight when the pointer leaves; follow it afterwards.
        if (event->type() == QEvent::Leave) QTimer::singleShot(0, this, &TimeframeListInput::showHighlight);
        if (event->type() != QEvent::KeyPress) return false;
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() != Qt::Key_Space || key->modifiers() != Qt::NoModifier) return false;
        auto* action = menu_->activeAction();
        auto* row = rowOf(action);
        if (!row) return false;
        auto* pin = row->findChild<QAbstractButton*>(QStringLiteral("chartTimeframePin_%1").arg(action->data().toLongLong()));
        if (pin && pin->isEnabled()) pin->click();
        return true;
    }

  private:
    static QWidget* rowOf(QAction* action) {
        auto* row = qobject_cast<QWidgetAction*>(action);
        return row ? row->defaultWidget() : nullptr;
    }
    void showHighlight() {
        const auto* active = menu_->activeAction();
        for (auto* action : menu_->actions()) {
            auto* row = rowOf(action);
            const bool on = action == active;
            if (!row || row->property("active").toBool() == on) continue;
            row->setProperty("active", on);
            row->style()->unpolish(row);
            row->style()->polish(row);
            row->update();
        }
    }
    QMenu* menu_;
};
} // namespace

void ChartWorkspace::restoreTimeframePins() {
    timeframePins_ = ChartTimeframePins{};
    setProperty("timeframeStorageIssue", QVariant{});
    if (chartSettings_ && chartSettings_->contains(ChartTimeframePins::SettingsKey)) {
        // Unreadable bytes stay where they are until the user changes a pin.
        ChartTimeframePins restored;
        QString issue;
        if (restored.restorePublicState(chartSettings_->value(ChartTimeframePins::SettingsKey).toByteArray(), &issue))
            timeframePins_ = restored;
        else setProperty("timeframeStorageIssue", issue);
    }
    refreshTimeframeButtons();
}

void ChartWorkspace::setTimeframePinned(qint64 intervalMs, bool pinned) {
    if (timeframePins_.isPinned(intervalMs) != pinned && timeframePins_.setPinned(intervalMs, pinned)) {
        if (chartSettings_) chartSettings_->setValue(ChartTimeframePins::SettingsKey, timeframePins_.publicState());
        setProperty("timeframeStorageIssue", QVariant{});
    }
    refreshTimeframeButtons();
}

void ChartWorkspace::refreshTimeframeButtons() {
    if (!timeframeButtons_ || !timeframeSelector_) return;
    const qint64 current = timeframeSelector_->currentData().toLongLong();
    // A pinned interval keeps its button; the current one shows even unpinned.
    for (auto* button : timeframeButtons_->findChildren<QPushButton*>()) {
        const qint64 interval = button->objectName().section('_', 1).toLongLong();
        button->setVisible(timeframePins_.isPinned(interval) || interval == current);
    }
    if (timeframeList_) timeframeList_->setVisible(!timeframePins_.allPinned());
    if (timeframeMenu_) {
        const bool last = timeframePins_.pinned().size() == 1;
        for (auto* pin : timeframeMenu_->findChildren<QAbstractButton*>()) {
            const auto name = pin->objectName();
            const qint64 interval = name.section('_', 1).toLongLong();
            const QSignalBlocker block(pin);
            if (name.startsWith(QStringLiteral("chartTimeframePin_"))) {
                pin->setChecked(timeframePins_.isPinned(interval));
                pin->setEnabled(!(last && timeframePins_.isPinned(interval)));
            } else if (name.startsWith(QStringLiteral("chartTimeframeListSelect_"))) {
                pin->setChecked(interval == current);
            }
        }
    }
    layoutToolbar();
}

void ChartWorkspace::openTimeframeList(QWidget* anchor) {
    if (!timeframeSelector_ || !anchor) return;
    if (timeframeMenu_) timeframeMenu_->close();
    auto* menu = new QMenu(this);
    menu->setObjectName("chartTimeframeListMenu");
    menu->setAccessibleName(tr("시간봉 목록"));
    applyLocalMenuStyle(menu);
    timeframeMenu_ = menu;
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    auto* input = new TimeframeListInput(menu);
    QAction* currentRow = nullptr;
    for (int index = 0; index < timeframeSelector_->count(); ++index) {
        const qint64 interval = timeframeSelector_->itemData(index).toLongLong();
        const auto label = timeframeSelector_->itemText(index);
        auto* row = new QWidget(menu);
        row->setObjectName(QStringLiteral("chartTimeframeListRow_%1").arg(interval));
        row->setAttribute(Qt::WA_StyledBackground, true);
        setThemeStyle(row, QStringLiteral("QWidget#%1{background:transparent;border-radius:6px;} QWidget#%1[active=\"true\"]{background:${surface};}")
            .arg(row->objectName()));
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(4, 2, 4, 2);
        line->setSpacing(4);
        auto* select = new QPushButton(label, row);
        select->setObjectName(QStringLiteral("chartTimeframeListSelect_%1").arg(interval));
        select->setAccessibleName(tr("시간봉 %1").arg(label));
        select->setCheckable(true);
        select->setFocusPolicy(Qt::NoFocus);
        select->setMinimumSize(104, 32);
        select->setFont(KeefiFonts::font(FontRole::Body15));
        select->setCursor(Qt::PointingHandCursor);
        setThemeStyle(select, QStringLiteral(
            "QPushButton{background:transparent;color:${text};border:2px solid transparent;border-radius:4px;padding:0 10px;text-align:left;}"
            "QPushButton:hover{background:${surface};}"
            "QPushButton:checked{font-weight:600;}"));
        auto* pin = new TimeframePinToggle(row);
        pin->setObjectName(QStringLiteral("chartTimeframePin_%1").arg(interval));
        pin->setAccessibleName(tr("%1 툴바에 고정").arg(label));
        pin->setToolTip(tr("툴바에 고정"));
        line->addWidget(select, 1);
        line->addWidget(pin);
        auto* action = new QWidgetAction(menu);
        action->setDefaultWidget(row);
        action->setText(select->accessibleName());
        action->setData(interval);
        menu->addAction(action);
        input->watch(action);
        if (index == timeframeSelector_->currentIndex()) currentRow = action;
        connect(select, &QPushButton::clicked, this, [this, index, menu] {
            timeframeSelector_->setCurrentIndex(index);
            menu->close();
        });
        // Enter on the highlighted row (QMenu closes itself after a trigger).
        connect(action, &QAction::triggered, this, [this, index] { timeframeSelector_->setCurrentIndex(index); });
        connect(pin, &QAbstractButton::toggled, this, [this, interval](bool pinned) { setTimeframePinned(interval, pinned); });
    }
    refreshTimeframeButtons();
    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height() + 2)));
    // Arrow keys start from the interval on screen now.
    if (currentRow) menu->setActiveAction(currentRow);
}
} // namespace keefi
