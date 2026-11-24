#include "keefi/InstrumentListTree.h"

#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QSet>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <utility>

namespace keefi {
namespace {
constexpr int IdentityRole = Qt::UserRole;
constexpr int SectionRole = Qt::UserRole + 1;
constexpr int SectionActionSize = 22;
constexpr int SectionActionGap = 2;
constexpr auto DragMimeType = "application/x-keefi-watchlist-order";

// Public IDs are the only serialized data. The live model and revision are
// in-process provenance, so copied text/Qt MIME and an old list cannot move rows.
class InstrumentDragMime final : public QMimeData {
public:
    InstrumentDragMime(QAbstractItemModel* model, QString list, quint64 generation,
                       QStringList keys, QString section)
        : sourceModel(model), listId(std::move(list)), revision(generation),
          identities(std::move(keys)), sectionId(std::move(section)) {
        setData(QString::fromLatin1(DragMimeType), QJsonDocument(QJsonObject{
            {QStringLiteral("list"), listId},
            {QStringLiteral("identities"), QJsonArray::fromStringList(identities)},
            {QStringLiteral("section"), sectionId}
        }).toJson(QJsonDocument::Compact));
    }

    const QPointer<QAbstractItemModel> sourceModel;
    const QString listId;
    const quint64 revision;
    const QStringList identities;
    const QString sectionId;
};
}

InstrumentListTree::InstrumentListTree(QWidget* parent) : QTreeWidget(parent) {
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    setExpandsOnDoubleClick(false);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(false);

    const auto makeAction = [this](const QString& text, const QString& name,
                                   const QString& accessible, bool destructive) {
        auto* button = new QToolButton(viewport());
        button->setObjectName(name);
        button->setText(text);
        button->setAccessibleName(accessible);
        button->setToolTip(accessible);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setFont(KeefiFonts::font(FontRole::CaptionStrong12));
        button->setFixedSize(SectionActionSize, SectionActionSize);
        setThemeStyle(button, QStringLiteral(
            "QToolButton{background:transparent;color:${text3};border:0;border-radius:3px;padding:0;}"
            "QToolButton:hover,QToolButton:focus{background:${surface};color:%1;}")
            .arg(destructive ? QStringLiteral("${danger}") : QStringLiteral("${text}")));
        button->installEventFilter(this);
        button->hide();
        return button;
    };
    addSection_ = makeAction(QStringLiteral("＋"), QStringLiteral("chartSectionAdd"),
                             tr("아래에 섹션 추가"), false);
    removeSection_ = makeAction(QStringLiteral("✕"), QStringLiteral("chartSectionRemove"),
                                tr("섹션 삭제 · 안의 종목은 섹션 없음으로 이동"), true);
    connect(addSection_, &QToolButton::clicked, this, [this] { runSectionAction(true); });
    connect(removeSection_, &QToolButton::clicked, this, [this] { runSectionAction(false); });

    // The workspace also cancels before rebuilding. Keep the internal view's
    // reset/removal boundary safe even when it is reached through another path.
    viewConnections_.append(connect(model(), &QAbstractItemModel::modelAboutToBeReset,
                                    this, &InstrumentListTree::cancelInteractions));
    viewConnections_.append(connect(model(), &QAbstractItemModel::rowsAboutToBeRemoved, this,
                                    [this] { cancelInteractions(); }));
    viewConnections_.append(connect(model(), &QAbstractItemModel::dataChanged, this,
                                    [this] { positionOverlays(); }));
    viewConnections_.append(connect(header(), &QHeaderView::sectionResized, this,
                                    [this] { positionOverlays(); }));
    viewConnections_.append(connect(header(), &QHeaderView::sectionMoved, this,
                                    [this] { positionOverlays(); }));
    bindThemePresentation(this, [this] { viewport()->update(); });
}

InstrumentListTree::~InstrumentListTree() {
    // QTreeWidget's internal model resets while the QWidget base destroys its
    // children, after our members have gone. End only our callbacks now;
    // leave Qt's own model/view connections and child destruction order intact.
    for (const auto& connection : viewConnections_) QObject::disconnect(connection);
    viewport()->removeEventFilter(this);
    if (addSection_) addSection_->removeEventFilter(this);
    if (removeSection_) removeSection_->removeEventFilter(this);
    cancelInteractions();
}

void InstrumentListTree::setInteractionContext(const QString& listId, bool enabled, bool manual) {
    const bool nextEnabled = enabled && !listId.isEmpty();
    const bool nextManual = nextEnabled && manual;
    if (listId_ == listId && sectionsEnabled_ == nextEnabled && manualOrder_ == nextManual) return;
    cancelInteractions();
    listId_ = listId;
    sectionsEnabled_ = nextEnabled;
    manualOrder_ = nextManual;
    setSelectionMode(nextEnabled ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
    setDragDropMode(nextManual ? QAbstractItemView::DragDrop : QAbstractItemView::NoDragDrop);
    setDragEnabled(nextManual);
    setAcceptDrops(nextManual);
    viewport()->setAcceptDrops(nextManual);
    viewport()->update();
}

bool InstrumentListTree::isEditingSection() const {
    return editor_ && editorIndex_.isValid();
}

bool InstrumentListTree::rowActivationAllowed() const {
    return !activationSuppressed_ && !dragExecuting_;
}

bool InstrumentListTree::dragHandlesVisible() const {
    return sectionsEnabled_ && manualOrder_;
}

void InstrumentListTree::scrollTo(const QModelIndex& index, ScrollHint hint) {
    auto visibleIndex = index;
    for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        if (isSection(parent) && !isExpanded(parent)) visibleIndex = parent;
    }
    // Qt may scroll to the current child after layout and expand its parent.
    // Respect a collapsed section without changing the current selection;
    // search explicitly expands matching sections before requesting a scroll.
    QTreeWidget::scrollTo(visibleIndex, hint);
}

void InstrumentListTree::cancelInteractions() {
    ++interactionRevision_;
    const bool releaseBelongsToDrag = dragExecuting_ || dragGesture_;
    if (dragExecuting_ && activeDrag_) QDrag::cancel();
    finishSectionEdit(false, false);
    pressedSection_ = QPersistentModelIndex{};
    pressedColumn_ = -1;
    ignoreNextRelease_ = releaseBelongsToDrag;
    suppressReleaseActivation_ = releaseBelongsToDrag;
    clearDragFeedback();
    clearHover();
}

bool InstrumentListTree::isSection(const QModelIndex& index) const {
    return index.isValid() && index.model() == model() && !index.parent().isValid() &&
        index.siblingAtColumn(0).data(IdentityRole).toString().isEmpty() &&
        !index.siblingAtColumn(0).data(SectionRole).toString().isEmpty();
}

QModelIndex InstrumentListTree::sectionAt(const QPoint& position) const {
    const auto index = indexAt(position).siblingAtColumn(0);
    return isSection(index) ? index : QModelIndex{};
}

QModelIndex InstrumentListTree::indexForIdentity(const QString& key) const {
    if (key.isEmpty()) return {};
    for (QTreeWidgetItemIterator it(const_cast<InstrumentListTree*>(this)); *it; ++it)
        if ((*it)->data(0, IdentityRole).toString() == key) return indexFromItem(*it, 0);
    return {};
}

QModelIndex InstrumentListTree::indexForSection(const QString& id) const {
    if (id.isEmpty()) return {};
    for (int row = 0; row < topLevelItemCount(); ++row) {
        const auto index = indexFromItem(topLevelItem(row), 0);
        if (isSection(index) && index.data(SectionRole).toString() == id) return index;
    }
    return {};
}

QStringList InstrumentListTree::orderedRowKeys(const QList<QTreeWidgetItem*>& items) const {
    const QSet<QTreeWidgetItem*> wanted(items.cbegin(), items.cend());
    QStringList result;
    for (QTreeWidgetItemIterator it(const_cast<InstrumentListTree*>(this)); *it; ++it) {
        if (!wanted.contains(*it)) continue;
        const auto key = (*it)->data(0, IdentityRole).toString();
        if (!key.isEmpty() && !result.contains(key)) result.append(key);
    }
    return result;
}

bool InstrumentListTree::onDragHandle(const QPoint& position) const {
    if (!dragHandlesVisible()) return false;
    const auto index = indexAt(position);
    if (index.column() != 0 || index.data(IdentityRole).toString().isEmpty()) return false;
    const auto rect = visualRect(index);
    return position.x() >= rect.left() && position.x() < rect.left() + DragHandleWidth;
}

QStringList InstrumentListTree::mimeTypes() const {
    return {QString::fromLatin1(DragMimeType)};
}

QMimeData* InstrumentListTree::mimeData(const QList<QTreeWidgetItem*>& items) const {
    if (!dragHandlesVisible() || editor_) return nullptr;
    const auto keys = orderedRowKeys(items);
    if (!keys.isEmpty()) return new InstrumentDragMime(model(), listId_, interactionRevision_, keys, {});
    for (auto* item : items) {
        if (!item || item->treeWidget() != this) continue;
        const auto index = indexFromItem(item, 0);
        if (isSection(index))
            return new InstrumentDragMime(model(), listId_, interactionRevision_, {}, index.data(SectionRole).toString());
    }
    return nullptr;
}

bool InstrumentListTree::dropMimeData(QTreeWidgetItem*, int, const QMimeData*, Qt::DropAction) {
    // dropEvent sends public IDs to the workspace. Never mutate QTreeWidget's
    // model through Qt's generic internal-move/deserialization path.
    return false;
}

Qt::DropActions InstrumentListTree::supportedDropActions() const {
    return Qt::MoveAction;
}

bool InstrumentListTree::acceptsOwnMime(const QMimeData* mime) const {
    const auto* payload = dynamic_cast<const InstrumentDragMime*>(mime);
    if (!dragHandlesVisible() || editor_ || !payload || payload->sourceModel != model() ||
        payload->listId != listId_ || payload->revision != interactionRevision_) return false;
    if (!payload->sectionId.isEmpty())
        return payload->identities.isEmpty() && indexForSection(payload->sectionId).isValid();
    if (payload->identities.isEmpty()) return false;
    for (const auto& key : payload->identities)
        if (!indexForIdentity(key).isValid()) return false;
    return true;
}

void InstrumentListTree::startDrag(Qt::DropActions supportedActions) {
    if (dragExecuting_ || !dragHandlesVisible() || editor_ ||
        !(supportedActions & Qt::MoveAction) || pressedColumn_ == 2) return;
    QList<QTreeWidgetItem*> items = selectedItems();
    if (pressedSection_.isValid()) items = {itemFromIndex(pressedSection_)};
    auto* mime = mimeData(items);
    if (!mime) return;
    const auto* payload = static_cast<const InstrumentDragMime*>(mime);
    clearHover();
    clearDragFeedback();
    if (payload->sectionId.isEmpty()) {
        for (const auto& key : payload->identities) draggingIndexes_.append(indexForIdentity(key));
    } else {
        draggingIndexes_.append(indexForSection(payload->sectionId));
    }
    const QString label = payload->sectionId.isEmpty()
        ? tr("종목 %1개").arg(payload->identities.size())
        : indexForSection(payload->sectionId).data(Qt::DisplayRole).toString();
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    QPixmap preview(144, 32);
    preview.fill(Qt::transparent);
    {
        QPainter painter(&preview);
        painter.fillRect(preview.rect(), themeColors().bg);
        painter.setPen(QPen(themeColors().accent, 1));
        painter.drawRect(QRectF(preview.rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        painter.setPen(themeColors().text);
        drawLedgerText(painter, QRectF(8, 0, 128, 32), label, FontRole::Caption12);
    }
    drag->setPixmap(preview);
    drag->setHotSpot(QPoint(12, 16));
    activeDrag_ = drag;
    dragExecuting_ = true;
    dragGesture_ = true;
    ignoreNextRelease_ = true;
    suppressReleaseActivation_ = true;
    pressedSection_ = QPersistentModelIndex{};
    setState(QAbstractItemView::DraggingState);
    viewport()->update();
    const QPointer<InstrumentListTree> alive(this);
    const QPointer<QDrag> dragAlive(drag);
    drag->exec(Qt::MoveAction, Qt::MoveAction);
    if (!alive) return;
    activeDrag_ = nullptr;
    dragExecuting_ = false;
    clearDragFeedback();
    setState(QAbstractItemView::NoState);
    if (dragAlive) dragAlive->deleteLater();
}

bool InstrumentListTree::updateDropTarget(const QMimeData* mime, const QPoint& position) {
    dropTarget_ = {};
    if (!acceptsOwnMime(mime) || !viewport()->rect().contains(position)) {
        viewport()->update();
        return false;
    }
    const auto* payload = static_cast<const InstrumentDragMime*>(mime);
    const auto index = indexAt(position).siblingAtColumn(0);
    if (!payload->sectionId.isEmpty()) {
        if (!isSection(index) || index.data(SectionRole).toString() == payload->sectionId) {
            viewport()->update();
            return false;
        }
        dropTarget_.section = index.data(SectionRole).toString();
        dropTarget_.after = position.y() > visualRect(index).center().y();
    } else if (isSection(index)) {
        dropTarget_.section = index.data(SectionRole).toString();
        dropTarget_.after = true;
        dropTarget_.intoSection = true;
    } else if (index.isValid()) {
        dropTarget_.identity = index.data(IdentityRole).toString();
        if (dropTarget_.identity.isEmpty() || payload->identities.contains(dropTarget_.identity)) {
            viewport()->update();
            return false;
        }
        dropTarget_.section = index.data(SectionRole).toString();
        dropTarget_.after = position.y() > visualRect(index).center().y();
    } else {
        int lastBottom = -1;
        for (QTreeWidgetItemIterator it(this); *it; ++it) {
            const auto rect = visualItemRect(*it);
            if (!rect.isEmpty()) lastBottom = qMax(lastBottom, rect.bottom());
        }
        if (position.y() <= lastBottom) {
            viewport()->update();
            return false;
        }
        dropTarget_.after = true;
    }
    dropTarget_.valid = true;
    dropTarget_.index = index;
    draggingIndexes_.clear();
    if (payload->sectionId.isEmpty()) {
        for (const auto& key : payload->identities) draggingIndexes_.append(indexForIdentity(key));
    } else {
        draggingIndexes_.append(indexForSection(payload->sectionId));
    }
    viewport()->update();
    return true;
}

void InstrumentListTree::dragEnterEvent(QDragEnterEvent* event) {
    if ((event->possibleActions() & Qt::MoveAction) &&
        acceptsOwnMime(event->mimeData())) {
        // A native drag first enters over its own source row. Accept the
        // session so later move events can reach another valid drop target.
        updateDropTarget(event->mimeData(), event->position().toPoint());
        dragGesture_ = true;
        ignoreNextRelease_ = true;
        clearHover();
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void InstrumentListTree::dragMoveEvent(QDragMoveEvent* event) {
    if ((event->possibleActions() & Qt::MoveAction) &&
        updateDropTarget(event->mimeData(), event->position().toPoint())) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void InstrumentListTree::dragLeaveEvent(QDragLeaveEvent* event) {
    dropTarget_ = {};
    if (!dragExecuting_) draggingIndexes_.clear();
    viewport()->update();
    event->accept();
}

void InstrumentListTree::dropEvent(QDropEvent* event) {
    if (!(event->possibleActions() & Qt::MoveAction) ||
        !updateDropTarget(event->mimeData(), event->position().toPoint())) {
        clearDragFeedback();
        event->ignore();
        return;
    }
    const auto* payload = static_cast<const InstrumentDragMime*>(event->mimeData());
    const QString list = payload->listId;
    const QStringList keys = payload->identities;
    const QString sourceSection = payload->sectionId;
    const QString targetIdentity = dropTarget_.identity;
    const QString targetSection = dropTarget_.section;
    const bool after = dropTarget_.after;
    dragGesture_ = true;
    ignoreNextRelease_ = true;
    suppressReleaseActivation_ = true;
    clearDragFeedback();
    event->setDropAction(Qt::MoveAction);
    event->accept();
    // The model may synchronously rebuild all items (and cancel the drag).
    // Do not access the MIME, item pointers or indexes after emitting.
    if (sourceSection.isEmpty()) emit rowsMoveRequested(list, keys, targetIdentity, targetSection, after);
    else emit sectionMoveRequested(list, sourceSection, targetSection, after);
}

void InstrumentListTree::clearDragFeedback() {
    draggingIndexes_.clear();
    dropTarget_ = {};
    viewport()->update();
}

QRect InstrumentListTree::dropLineRect() const {
    if (!dropTarget_.valid) return {};
    int y = 0;
    if (dropTarget_.index.isValid()) {
        auto rect = visualRect(dropTarget_.index);
        if (rect.isEmpty()) return {};
        if (dropTarget_.intoSection && isExpanded(dropTarget_.index)) {
            for (int row = 0; row < model()->rowCount(dropTarget_.index); ++row) {
                const auto childRect = visualRect(model()->index(row, 0, dropTarget_.index));
                if (!childRect.isEmpty()) rect.setBottom(qMax(rect.bottom(), childRect.bottom()));
            }
        }
        y = dropTarget_.after ? rect.bottom() + 1 : rect.top();
    } else {
        for (QTreeWidgetItemIterator it(const_cast<InstrumentListTree*>(this)); *it; ++it) {
            const auto rect = visualItemRect(*it);
            if (!rect.isEmpty()) y = qMax(y, rect.bottom() + 1);
        }
    }
    return QRect(0, qBound(1, y, qMax(1, viewport()->height() - 1)) - 1, viewport()->width(), 2);
}

void InstrumentListTree::paintEvent(QPaintEvent* event) {
    QTreeWidget::paintEvent(event);
    if (draggingIndexes_.isEmpty() && !dropTarget_.valid) return;
    QPainter painter(viewport());
    painter.setClipRegion(event->region());
    painter.setPen(QPen(themeColors().accent, 1));
    painter.setBrush(Qt::NoBrush);
    for (const auto& index : draggingIndexes_) {
        if (!index.isValid() || index.model() != model()) continue;
        const auto rect = visualRect(index);
        if (rect.isEmpty() || !rect.intersects(viewport()->rect())) continue;
        painter.drawRect(QRectF(0.5, rect.top() + 0.5, qMax(0, viewport()->width() - 1), qMax(0, rect.height() - 1)));
    }
    const auto line = dropLineRect();
    if (!line.isEmpty()) painter.fillRect(line, themeColors().accent);
}

void InstrumentListTree::drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    const auto section = index.siblingAtColumn(0);
    if (!isSection(section)) {
        QTreeWidget::drawRow(painter, option, index);
        if (dragHandlesVisible() && !section.data(IdentityRole).toString().isEmpty()) {
            const auto cell = visualRect(section);
            painter->save();
            painter->setClipRect(option.rect, Qt::IntersectClip);
            painter->setPen(hoveredRow_ == section ? themeColors().text2 : themeColors().text3);
            drawLedgerText(*painter, QRectF(cell.left() + 3, option.rect.top(), DragHandleWidth - 3, option.rect.height()),
                           QStringLiteral("⠿"), FontRole::Caption12);
            painter->restore();
        }
        return;
    }

    painter->save();
    painter->setClipRect(option.rect, Qt::IntersectClip);
    const bool hovered = hoveredSection_ == section;
    const bool selected = selectionModel()->isSelected(section);
    painter->fillRect(option.rect, hovered || selected ? themeColors().surface : themeColors().bg);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(themeColors().text3);
    const QPointF center(option.rect.left() + 10, option.rect.center().y() + 0.5);
    QPolygonF triangle;
    if (isExpanded(section)) triangle << center + QPointF(-3, -2) << center + QPointF(3, -2) << center + QPointF(0, 2);
    else triangle << center + QPointF(-2, -3) << center + QPointF(-2, 3) << center + QPointF(2, 0);
    painter->drawPolygon(triangle);

    const int actions = sectionsEnabled_ && hovered && !editor_ ? 2 * SectionActionSize + SectionActionGap + 6 : 6;
    const QRectF textRect = QRectF(option.rect).adjusted(22, 0, -actions, 0);
    const auto count = QStringLiteral(" · %1").arg(model()->rowCount(section));
    const qreal countWidth = qMax(QFontMetricsF(KeefiFonts::font(FontRole::Caption12)).horizontalAdvance(count),
                                  QFontMetricsF(KeefiFonts::font(FontRole::Number11)).horizontalAdvance(count)) + 4;
    painter->setPen(themeColors().text3);
    const qreal nameWidth = drawLedgerText(*painter,
        QRectF(textRect.left(), textRect.top(), qMax(0.0, textRect.width() - countWidth), textRect.height()),
        section.data(Qt::DisplayRole).toString(), FontRole::Caption12);
    drawLedgerText(*painter,
        QRectF(textRect.left() + nameWidth, textRect.top(), qMax(0.0, textRect.width() - nameWidth), textRect.height()),
        count, FontRole::Caption12);
    painter->restore();
}

void InstrumentListTree::updateHover(const QPoint& position) {
    const auto row = sectionsEnabled_ && !editor_ && !dragExecuting_
        ? indexAt(position).siblingAtColumn(0) : QModelIndex{};
    const auto next = isSection(row) ? row : QModelIndex{};
    const auto nextRow = !row.data(IdentityRole).toString().isEmpty() ? row : QModelIndex{};
    if (hoveredSection_ == next && hoveredRow_ == nextRow) return;
    hoveredSection_ = next;
    hoveredRow_ = nextRow;
    positionHoverActions();
    viewport()->update();
}

void InstrumentListTree::clearHover() {
    hoveredSection_ = QPersistentModelIndex{};
    hoveredRow_ = QPersistentModelIndex{};
    if (addSection_) addSection_->hide();
    if (removeSection_) removeSection_->hide();
    viewport()->update();
}

void InstrumentListTree::positionHoverActions() {
    if (!addSection_ || !removeSection_) return;
    if (!sectionsEnabled_ || editor_ || dragExecuting_ || !isVisible() || !isSection(hoveredSection_)) {
        addSection_->hide();
        removeSection_->hide();
        return;
    }
    const auto row = visualRect(hoveredSection_);
    if (row.isEmpty() || !row.intersects(viewport()->rect())) {
        addSection_->hide();
        removeSection_->hide();
        return;
    }
    const int right = qMin(row.right(), viewport()->rect().right()) - 4;
    const int top = row.top() + (row.height() - SectionActionSize) / 2;
    removeSection_->move(right - SectionActionSize + 1, top);
    addSection_->move(right - 2 * SectionActionSize - SectionActionGap + 1, top);
    addSection_->show();
    removeSection_->show();
    addSection_->raise();
    removeSection_->raise();
}

void InstrumentListTree::runSectionAction(bool add) {
    if (!sectionsEnabled_ || !isSection(hoveredSection_)) return;
    const QString list = listId_;
    const QString section = hoveredSection_.data(SectionRole).toString();
    clearHover();
    // A synchronous slot may replace every item. Only stable public IDs cross
    // that call, and no item or index is used after the signal.
    if (add) emit sectionAddBelowRequested(list, section);
    else emit sectionRemoveRequested(list, section);
}

void InstrumentListTree::mousePressEvent(QMouseEvent* event) {
    ignoreNextRelease_ = false;
    dragGesture_ = false;
    pressedSection_ = QPersistentModelIndex{};
    const auto position = event->position().toPoint();
    const auto index = indexAt(position);
    pressedColumn_ = index.column();
    pressPosition_ = position;
    suppressReleaseActivation_ = sectionsEnabled_ &&
        (event->button() != Qt::LeftButton || event->modifiers() != Qt::NoModifier || onDragHandle(position));
    const auto section = sectionAt(position);
    if (event->button() == Qt::LeftButton && sectionsEnabled_ && section.isValid()) {
        finishSectionEdit(false, false);
        pressedSection_ = section;
        setCurrentItem(itemFromIndex(section), 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        setFocus(Qt::MouseFocusReason);
        updateHover(pressPosition_);
        event->accept();
        return;
    }
    const QPointer<InstrumentListTree> alive(this);
    const bool previous = activationSuppressed_;
    activationSuppressed_ = suppressReleaseActivation_;
    QTreeWidget::mousePressEvent(event);
    if (alive) activationSuppressed_ = previous;
}

void InstrumentListTree::mouseMoveEvent(QMouseEvent* event) {
    updateHover(event->position().toPoint());
    if (pressedSection_.isValid()) {
        if ((event->buttons() & Qt::LeftButton) && dragHandlesVisible() &&
            (event->position().toPoint() - pressPosition_).manhattanLength() >= QApplication::startDragDistance())
            startDrag(Qt::MoveAction);
        event->accept();
        return;
    }
    const QPointer<InstrumentListTree> alive(this);
    const bool previous = activationSuppressed_;
    activationSuppressed_ = suppressReleaseActivation_ || dragGesture_;
    QTreeWidget::mouseMoveEvent(event);
    if (alive) activationSuppressed_ = previous;
}

void InstrumentListTree::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (ignoreNextRelease_ || dragGesture_)) {
        ignoreNextRelease_ = false;
        dragGesture_ = false;
        suppressReleaseActivation_ = false;
        pressedSection_ = QPersistentModelIndex{};
        pressedColumn_ = -1;
        setState(QAbstractItemView::NoState);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && pressedSection_.isValid()) {
        const auto section = pressedSection_;
        pressedSection_ = QPersistentModelIndex{};
        if (sectionsEnabled_ && section == sectionAt(event->position().toPoint()) &&
            (event->position().toPoint() - pressPosition_).manhattanLength() < QApplication::startDragDistance()) {
            const QString list = listId_;
            const QString id = section.data(SectionRole).toString();
            const bool expanded = !isExpanded(section);
            setExpanded(section, expanded);
            positionOverlays();
            viewport()->update();
            emit sectionExpandedByUser(list, id, expanded);
        }
        event->accept();
        return;
    }
    const QPointer<InstrumentListTree> alive(this);
    const bool previous = activationSuppressed_;
    activationSuppressed_ = suppressReleaseActivation_ ||
        (sectionsEnabled_ && (event->button() != Qt::LeftButton || event->modifiers() != Qt::NoModifier));
    suppressReleaseActivation_ = false;
    pressedColumn_ = -1;
    QTreeWidget::mouseReleaseEvent(event);
    if (alive) activationSuppressed_ = previous;
}

void InstrumentListTree::mouseDoubleClickEvent(QMouseEvent* event) {
    const auto section = sectionAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && sectionsEnabled_ && section.isValid()) {
        pressedSection_ = QPersistentModelIndex{};
        ignoreNextRelease_ = true;
        beginSectionEdit(section);
        event->accept();
        return;
    }
    const QPointer<InstrumentListTree> alive(this);
    const bool previous = activationSuppressed_;
    activationSuppressed_ = sectionsEnabled_ &&
        (event->button() != Qt::LeftButton || event->modifiers() != Qt::NoModifier ||
         onDragHandle(event->position().toPoint()));
    QTreeWidget::mouseDoubleClickEvent(event);
    if (alive) activationSuppressed_ = previous;
}

void InstrumentListTree::keyPressEvent(QKeyEvent* event) {
    if (sectionsEnabled_ && !editor_) {
        const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
        const bool move = modifiers == Qt::AltModifier &&
            (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down);
        const bool remove = modifiers == Qt::NoModifier &&
            (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace);
        if (move || remove) {
            const auto current = currentIndex().siblingAtColumn(0);
            const QString list = listId_;
            const QString section = isSection(current) ? current.data(SectionRole).toString() : QString{};
            QStringList keys = section.isEmpty() ? orderedRowKeys(selectedItems()) : QStringList{};
            if (keys.isEmpty() && section.isEmpty()) {
                const auto key = current.data(IdentityRole).toString();
                if (!key.isEmpty()) keys.append(key);
            }
            event->accept();
            if (keys.isEmpty() && section.isEmpty()) return;
            if (move) emit relativeMoveRequested(list, keys, section, event->key() == Qt::Key_Up ? -1 : 1);
            else emit removeRequested(list, keys, section);
            return;
        }
    }
    QTreeWidget::keyPressEvent(event);
}

void InstrumentListTree::contextMenuEvent(QContextMenuEvent* event) {
    const auto index = (event->reason() == QContextMenuEvent::Keyboard
        ? currentIndex() : indexAt(event->pos())).siblingAtColumn(0);
    if (!sectionsEnabled_ || !index.isValid() || index.data(IdentityRole).toString().isEmpty()) {
        QTreeWidget::contextMenuEvent(event);
        return;
    }
    finishSectionEdit(false, false);
    clearHover();
    const auto selection = selectionModel()->isSelected(index)
        ? QItemSelectionModel::NoUpdate : QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
    selectionModel()->setCurrentIndex(index, selection);
    const QString list = listId_;
    const QStringList keys = orderedRowKeys(selectedItems());
    const QPoint global = event->reason() == QContextMenuEvent::Keyboard
        ? viewport()->mapToGlobal(visualRect(index).center()) : event->globalPos();
    event->accept();
    if (!keys.isEmpty()) emit rowContextRequested(list, keys, global);
}

void InstrumentListTree::beginSectionEdit(const QModelIndex& index) {
    if (!sectionsEnabled_ || !isSection(index)) return;
    finishSectionEdit(false, false);
    clearHover();
    editorIndex_ = index;
    editorListId_ = listId_;
    editorSectionId_ = index.data(SectionRole).toString();
    auto* editor = new QLineEdit(viewport());
    editor_ = editor;
    editor->setObjectName(QStringLiteral("chartSectionNameEditor"));
    editor->setAccessibleName(tr("섹션 이름 바꾸기"));
    editor->setFont(KeefiFonts::font(FontRole::Caption12));
    editor->setText(index.data(Qt::DisplayRole).toString());
    setThemeStyle(editor, QStringLiteral(
        "QLineEdit{background:${bg};color:${text};border:1px solid ${accent};border-radius:3px;padding:0 3px;}"));
    editor->installEventFilter(this);
    positionEditor();
    editor->show();
    editor->raise();
    editor->setFocus(Qt::MouseFocusReason);
    editor->selectAll();
}

void InstrumentListTree::finishSectionEdit(bool commit, bool restoreFocus) {
    const auto editor = editor_;
    const bool valid = editor && sectionsEnabled_ && isSection(editorIndex_) &&
        editorListId_ == listId_ && editorIndex_.data(SectionRole).toString() == editorSectionId_;
    const QString list = editorListId_;
    const QString section = editorSectionId_;
    const QString text = editor ? editor->text() : QString{};
    editor_ = nullptr;
    editorIndex_ = QPersistentModelIndex{};
    editorListId_.clear();
    editorSectionId_.clear();
    if (editor) {
        editor->removeEventFilter(this);
        editor->hide();
        editor->deleteLater();
        if (restoreFocus && isVisible()) setFocus(Qt::OtherFocusReason);
    }
    // Never optimistically write DisplayRole. The existing list model decides
    // whether an empty, duplicate, long or otherwise invalid name is accepted.
    if (commit && valid) emit sectionRenameRequested(list, section, text);
}

void InstrumentListTree::positionEditor() {
    if (!editor_) return;
    if (!isSection(editorIndex_)) { finishSectionEdit(false, false); return; }
    const auto row = visualRect(editorIndex_);
    if (row.isEmpty()) return;
    const QRect geometry = row.adjusted(20, 1, -4, -1);
    if (editor_->geometry() != geometry) editor_->setGeometry(geometry);
    editor_->raise();
}

void InstrumentListTree::positionOverlays() {
    positionEditor();
    positionHoverActions();
}

void InstrumentListTree::scheduleHoverLeave() {
    QTimer::singleShot(0, this, [this] {
        const bool onAction = (addSection_ && (addSection_->underMouse() || addSection_->hasFocus())) ||
            (removeSection_ && (removeSection_->underMouse() || removeSection_->hasFocus()));
        if (!viewport()->underMouse() && !onAction) clearHover();
    });
}

bool InstrumentListTree::eventFilter(QObject* object, QEvent* event) {
    if (object == editor_) {
        if (event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                finishSectionEdit(key != Qt::Key_Escape, true);
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            finishSectionEdit(false, false);
        }
        return false;
    }
    if ((object == viewport() || object == addSection_ || object == removeSection_) && event->type() == QEvent::Leave)
        scheduleHoverLeave();
    return QTreeWidget::eventFilter(object, event);
}

void InstrumentListTree::resizeEvent(QResizeEvent* event) {
    QTreeWidget::resizeEvent(event);
    positionOverlays();
}

void InstrumentListTree::scrollContentsBy(int dx, int dy) {
    QTreeWidget::scrollContentsBy(dx, dy);
    positionOverlays();
}

void InstrumentListTree::hideEvent(QHideEvent* event) {
    cancelInteractions();
    QTreeWidget::hideEvent(event);
}

} // namespace keefi
