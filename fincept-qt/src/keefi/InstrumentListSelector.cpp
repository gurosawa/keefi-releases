#include "keefi/InstrumentListSelector.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QScreen>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <memory>
#include <utility>

namespace keefi {
namespace {
constexpr int OrderBarHeight = 24;
constexpr int ListIdRole = Qt::UserRole;

QString orderMimeType() { return QStringLiteral("application/x-keefi-list-order"); }

QModelIndex indexForId(const QAbstractItemModel* model, const QString& id) {
    if (!model || id.isEmpty()) return {};
    QModelIndex found;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        if (index.data(ListIdRole).toString() != id) continue;
        if (found.isValid()) return {}; // Ambiguous identities cannot be moved.
        found = index;
    }
    return found;
}

bool enabledList(const QModelIndex& index) {
    return index.isValid() && !index.parent().isValid() && index.column() == 0 &&
        !index.data(ListIdRole).toString().isEmpty() &&
        (index.flags() & Qt::ItemIsEnabled) && (index.flags() & Qt::ItemIsSelectable);
}

// Only this model's in-process MIME object is accepted. Copying its serialized
// public ID (or Qt's ordinary item MIME) does not copy the creator identity.
class ListOrderMime final : public QMimeData {
public:
    ListOrderMime(QAbstractItemModel* creator, QString id)
        : creator_(creator), sourceId_(std::move(id)) {
        setData(orderMimeType(), sourceId_.toUtf8());
    }
    QAbstractItemModel* creator() const { return creator_.data(); }
    const QString& sourceId() const { return sourceId_; }
private:
    QPointer<QAbstractItemModel> creator_;
    QString sourceId_;
};

class InstrumentListSelectorModel final : public QStandardItemModel {
public:
    explicit InstrumentListSelectorModel(QObject* parent) : QStandardItemModel(parent) {}

    QStringList mimeTypes() const override { return {orderMimeType()}; }
    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QString source;
        for (const auto& index : indexes) {
            if (index.model() != this || !enabledList(index)) return nullptr;
            const auto id = index.data(ListIdRole).toString();
            if (!source.isEmpty() && source != id) return nullptr;
            source = id;
        }
        if (!enabledList(indexForId(this, source))) return nullptr;
        return new ListOrderMime(const_cast<InstrumentListSelectorModel*>(this), source);
    }
    Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    bool canDropMimeData(const QMimeData*, Qt::DropAction, int, int,
                         const QModelIndex&) const override { return false; }
    bool dropMimeData(const QMimeData*, Qt::DropAction, int, int,
                      const QModelIndex&) override { return false; }
};
} // namespace

class InstrumentListSelectorView final : public QListView {
public:
    explicit InstrumentListSelectorView(InstrumentListSelector* owner)
        : QListView(owner), owner_(owner) {
        setObjectName(QStringLiteral("chartListOrderView"));
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setDragDropOverwriteMode(false);
        setDropIndicatorShown(false);
        setDragEnabled(true);
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);
        setViewportMargins(0, OrderBarHeight, 0, 0);

        orderBar_ = new QWidget(this);
        orderBar_->setObjectName(QStringLiteral("chartListOrderBar"));
        orderBar_->setFixedHeight(OrderBarHeight);
        setThemeStyle(orderBar_, QStringLiteral(
            "QWidget#chartListOrderBar{background:${input};border:0;border-bottom:1px solid ${rule};}"));
        auto* layout = new QHBoxLayout(orderBar_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        auto* group = new QButtonGroup(orderBar_);
        group->setExclusive(true);
        const auto makeOrderButton = [&](const QString& text, const QString& name) {
            auto* button = new QToolButton(orderBar_);
            button->setText(text);
            button->setObjectName(name);
            button->setAccessibleName(text);
            button->setCheckable(true);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::StrongFocus);
            button->setFont(KeefiFonts::font(FontRole::Caption12));
            button->setFixedHeight(OrderBarHeight);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            setThemeStyle(button, QStringLiteral(
                "QToolButton{background:transparent;color:${text3};border:1px solid transparent;"
                "border-radius:3px;padding:0 6px;}"
                "QToolButton:checked{background:${surface};color:${text};}"
                "QToolButton:hover{background:${surface};color:${text};}"
                "QToolButton:focus{border-color:${accent};}"));
            group->addButton(button);
            return button;
        };
        recent_ = makeOrderButton(tr("최근순"), QStringLiteral("chartListOrder_recent"));
        manual_ = makeOrderButton(tr("내 순서"), QStringLiteral("chartListOrder_manual"));
        auto* divider = new QFrame(orderBar_);
        divider->setFixedSize(1, 12);
        setThemeStyle(divider, QStringLiteral("QFrame{background:${rule};border:0;}"));
        layout->addWidget(recent_, 1);
        layout->addWidget(divider, 0, Qt::AlignVCenter);
        layout->addWidget(manual_, 1);
        recent_->setChecked(true);
        connect(recent_, &QToolButton::clicked, owner, [owner] { owner->setManualOrder(false); });
        connect(manual_, &QToolButton::clicked, owner, [owner] { owner->setManualOrder(true); });

        autoScroll_ = new QTimer(this);
        autoScroll_->setInterval(80);
        connect(autoScroll_, &QTimer::timeout, this, [this] {
            if (!isVisible() || !enabledList(indexForId(model(), dragSourceId_))) {
                clearDropState();
                return;
            }
            auto* bar = verticalScrollBar();
            bar->setValue(bar->value() + autoScrollDirection_ * qMax(1, bar->singleStep()));
            updateDropPreview(lastDragPosition_, dragSourceId_);
        });
        bindThemePresentation(this, [this] { viewport()->update(); });
    }

    void installInputFilters(QObject* filter) {
        // Install after QComboBox::setView: its release filter must not activate
        // the row under a completed drag. Ordinary clicks still reach Qt.
        viewport()->installEventFilter(filter);
        installEventFilter(filter);
        recent_->installEventFilter(filter);
        manual_->installEventFilter(filter);
    }

    void removeInputFilters(QObject* filter) {
        viewport()->removeEventFilter(filter);
        removeEventFilter(filter);
        recent_->removeEventFilter(filter);
        manual_->removeEventFilter(filter);
    }

    void setManual(bool manual) {
        recent_->setChecked(!manual);
        manual_->setChecked(manual);
    }

    void preparePopup() {
        cancelInteractions();
        positionOrderBar();
    }

    void cancelInteractions() {
        pressedId_.clear();
        suppressRelease_ = false;
        clearDropState();
        setState(QAbstractItemView::NoState);
    }

    bool filterInput(QObject* watched, QEvent* event) {
        if (watched == viewport()) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    suppressRelease_ = false;
                    const auto index = indexAt(mouse->position().toPoint());
                    pressedId_ = enabledList(index) ? index.data(ListIdRole).toString() : QString();
                    pressPosition_ = mouse->position().toPoint();
                }
            } else if (event->type() == QEvent::MouseMove) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if ((mouse->buttons() & Qt::LeftButton) && !pressedId_.isEmpty() &&
                    (mouse->position().toPoint() - pressPosition_).manhattanLength() >= QApplication::startDragDistance()) {
                    const auto source = pressedId_;
                    pressedId_.clear();
                    suppressRelease_ = true;
                    beginListDrag(source);
                    return true;
                }
            } else if (event->type() == QEvent::MouseButtonRelease) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    pressedId_.clear();
                    if (suppressRelease_) {
                        suppressRelease_ = false;
                        setState(QAbstractItemView::NoState);
                        mouse->accept();
                        return true;
                    }
                }
            }
        }
        if (event->type() != QEvent::KeyPress) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        const bool fromButton = watched == recent_ || watched == manual_;
        if (fromButton && key->key() == Qt::Key_Escape) {
            if (owner_) owner_->hidePopup();
            return true;
        }
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            const bool back = key->key() == Qt::Key_Backtab || (key->modifiers() & Qt::ShiftModifier);
            QWidget* next = nullptr;
            if (watched == this || watched == viewport()) next = back ? manual_ : recent_;
            else if (watched == recent_) next = back ? static_cast<QWidget*>(this) : manual_;
            else if (watched == manual_) next = back ? recent_ : static_cast<QWidget*>(this);
            if (next) {
                next->setFocus(back ? Qt::BacktabFocusReason : Qt::TabFocusReason);
                return true;
            }
        }
        if (fromButton && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            static_cast<QToolButton*>(watched)->click();
            return true;
        }
        if (fromButton && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            (key->key() == Qt::Key_Left ? recent_ : manual_)->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (fromButton && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
            setFocus(Qt::TabFocusReason);
            return true;
        }
        return false;
    }

protected:
    void startDrag(Qt::DropActions) override {
        suppressRelease_ = true;
        beginListDrag(currentIndex().data(ListIdRole).toString());
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (!trustedSource(event)) { event->ignore(); return; }
        dragSourceId_ = static_cast<const ListOrderMime*>(event->mimeData())->sourceId();
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (!trustedSource(event)) { clearDropState(); event->ignore(); return; }
        const auto source = static_cast<const ListOrderMime*>(event->mimeData())->sourceId();
        dragSourceId_ = source;
        lastDragPosition_ = event->position().toPoint();
        updateAutoScroll(lastDragPosition_);
        if (!updateDropPreview(lastDragPosition_, source)) { event->ignore(); return; }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        clearDropState();
        event->accept();
    }

    void dropEvent(QDropEvent* event) override {
        if (!trustedSource(event)) { clearDropState(); event->ignore(); return; }
        const QString source = static_cast<const ListOrderMime*>(event->mimeData())->sourceId();
        const auto targetIndex = indexAt(event->position().toPoint());
        const QString target = targetIndex.data(ListIdRole).toString();
        if (!enabledList(targetIndex) || source == target ||
            !enabledList(indexForId(model(), target))) {
            clearDropState();
            event->ignore();
            return;
        }
        const bool after = event->position().y() >= visualRect(targetIndex).center().y();
        QPointer<InstrumentListSelectorView> alive(this);
        QPointer<InstrumentListSelector> owner(owner_);
        QPointer<QAbstractItemModel> creator(model());
        clearDropState();
        pressedId_.clear();
        suppressRelease_ = true;

        // This signal can synchronously rebuild every row. From here on use
        // copied IDs only, never targetIndex or a row number from the old view.
        owner->setManualOrder(true);
        if (!alive || !owner || !creator || alive->model() != creator.data() ||
            !enabledList(indexForId(creator, source)) || !enabledList(indexForId(creator, target))) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        emit owner->listMoveRequested(source, target, after);
    }

    void resizeEvent(QResizeEvent* event) override {
        QListView::resizeEvent(event);
        positionOrderBar();
    }

    void updateGeometries() override {
        QListView::updateGeometries();
        positionOrderBar();
    }

    void hideEvent(QHideEvent* event) override {
        cancelInteractions();
        QListView::hideEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QListView::paintEvent(event);
        if (!dropTarget_.isValid()) return;
        const auto row = visualRect(dropTarget_);
        QPainter painter(viewport());
        painter.setPen(QPen(themeColors().accent, 2));
        const int y = dropAfter_ ? row.bottom() : row.top();
        painter.drawLine(row.left() + 3, y, row.right() - 3, y);
    }

private:
    bool trustedSource(const QDropEvent* event) const {
        if (!owner_ || !owner_->isEnabled() || !isVisible() ||
            !(event->possibleActions() & Qt::MoveAction)) return false;
        const auto* mime = dynamic_cast<const ListOrderMime*>(event->mimeData());
        if (!mime || mime->creator() != model() ||
            (event->source() && event->source() != this)) return false;
        return enabledList(indexForId(model(), mime->sourceId()));
    }

    void beginListDrag(const QString& source) {
        if (activeDrag_ || !owner_ || !owner_->isEnabled()) return;
        const auto index = indexForId(model(), source);
        if (!enabledList(index)) return;
        std::unique_ptr<QMimeData> mime(model()->mimeData({index}));
        if (!mime) return;
        auto* drag = new QDrag(this);
        drag->setMimeData(mime.release());
        const auto row = visualRect(index);
        if (!row.isEmpty()) {
            drag->setPixmap(viewport()->grab(row));
            drag->setHotSpot(pressPosition_ - row.topLeft());
        }
        QPointer<InstrumentListSelectorView> alive(this);
        QPointer<QDrag> guardedDrag(drag);
        activeDrag_ = drag;
        setState(QAbstractItemView::DraggingState);
        drag->exec(Qt::MoveAction, Qt::MoveAction);
        // Do not invoke the base startDrag: it can remove the source model row
        // after a MoveAction, while the workspace already owns the saved move.
        if (guardedDrag) guardedDrag->deleteLater();
        if (!alive) return;
        activeDrag_.clear();
        clearDropState();
        setState(QAbstractItemView::NoState);
    }

    bool updateDropPreview(const QPoint& position, const QString& source) {
        const auto index = indexAt(position);
        const auto target = index.data(ListIdRole).toString();
        const bool valid = enabledList(index) && target != source &&
            enabledList(indexForId(model(), target));
        dropTarget_ = valid ? QPersistentModelIndex(index) : QPersistentModelIndex{};
        dropAfter_ = valid && position.y() >= visualRect(index).center().y();
        viewport()->update();
        return valid;
    }

    void updateAutoScroll(const QPoint& position) {
        const int margin = qMin(autoScrollMargin(), viewport()->height() / 2);
        autoScrollDirection_ = position.y() < margin ? -1 :
            position.y() >= viewport()->height() - margin ? 1 : 0;
        if (autoScrollDirection_) autoScroll_->start();
        else autoScroll_->stop();
    }

    void clearDropState() {
        if (autoScroll_) autoScroll_->stop();
        autoScrollDirection_ = 0;
        dragSourceId_.clear();
        dropTarget_ = QPersistentModelIndex{};
        viewport()->update();
    }

    void positionOrderBar() {
        if (!orderBar_) return;
        const auto area = viewport()->geometry();
        orderBar_->setGeometry(area.x(), area.y() - OrderBarHeight, area.width(), OrderBarHeight);
        orderBar_->raise();
    }

    QPointer<InstrumentListSelector> owner_;
    QWidget* orderBar_ = nullptr;
    QToolButton* recent_ = nullptr;
    QToolButton* manual_ = nullptr;
    QTimer* autoScroll_ = nullptr;
    QPointer<QDrag> activeDrag_;
    QString pressedId_, dragSourceId_;
    QPoint pressPosition_, lastDragPosition_;
    QPersistentModelIndex dropTarget_;
    bool suppressRelease_ = false;
    bool dropAfter_ = false;
    int autoScrollDirection_ = 0;
};

InstrumentListSelector::InstrumentListSelector(QWidget* parent) : LocalComboBox(parent) {
    const auto originalPalette = view()->palette();
    const auto popupStyle = view()->property("keefiThemeTemplate").toString();
    setModel(new InstrumentListSelectorModel(this));
    auto* list = new InstrumentListSelectorView(this);
    list->setPalette(originalPalette);
    list->setTextElideMode(Qt::ElideNone);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list->setMouseTracking(true);
    setThemeStyle(list, popupStyle);
    setView(list);
    setItemDelegate(new LocalComboItemDelegate(this, list));
    popupView_ = list;
    list->installInputFilters(this);
}

InstrumentListSelector::~InstrumentListSelector() {
    if (popupView_) {
        popupView_->removeInputFilters(this);
        popupView_->cancelInteractions();
    }
}

bool InstrumentListSelector::manualOrder() const { return manualOrder_; }

void InstrumentListSelector::setManualOrder(bool manual) {
    if (manualOrder_ == manual) return;
    manualOrder_ = manual;
    if (popupView_) popupView_->setManual(manual);
    emit orderViewChanged();
}

void InstrumentListSelector::showPopup() {
    if (popupView_) popupView_->preparePopup();
    LocalComboBox::showPopup();
    if (!popupView_ || !popupView_->isVisible()) return;

    // Keep the same number of visible list rows after reserving the 24px bar.
    // The ordinary LocalComboBox still computes the popup width and styling.
    int rowsHeight = 0;
    for (int row = 0; row < qMin(count(), maxVisibleItems()); ++row)
        rowsHeight += qMax(0, popupView_->sizeHintForRow(row));
    const int missing = qMax(0, rowsHeight - popupView_->viewport()->height());
    auto* popup = popupView_->window();
    if (missing > 0 && screen()) {
        const auto available = screen()->availableGeometry();
        popup->resize(popup->width(), qMin(available.height(), popup->height() + missing));
        const int bottomAligned = available.bottom() - popup->height() + 1;
        popup->move(popup->x(), qMax(available.top(), qMin(popup->y(), bottomAligned)));
    }
}

void InstrumentListSelector::hidePopup() {
    if (popupView_) popupView_->cancelInteractions();
    LocalComboBox::hidePopup();
}

bool InstrumentListSelector::eventFilter(QObject* watched, QEvent* event) {
    if (popupView_ && popupView_->filterInput(watched, event)) return true;
    return LocalComboBox::eventFilter(watched, event);
}

} // namespace keefi
