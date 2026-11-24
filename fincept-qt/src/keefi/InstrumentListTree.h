#pragma once

#include <QList>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTreeWidget>

class QDrag;
class QLineEdit;
class QMimeData;
class QToolButton;

namespace keefi {

// List presentation and input only. The workspace owns list mutations and
// public settings; ordinary instrument rows keep the installed row delegate.
class InstrumentListTree final : public QTreeWidget {
    Q_OBJECT
public:
    static constexpr int DragHandleWidth = 18;

    explicit InstrumentListTree(QWidget* parent = nullptr);
    ~InstrumentListTree() override;

    void setInteractionContext(const QString& listId, bool enabled, bool manual);
    void cancelInteractions();
    bool isEditingSection() const;
    bool rowActivationAllowed() const;
    bool dragHandlesVisible() const;
    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override;

signals:
    void sectionExpandedByUser(const QString& listId, const QString& sectionId, bool expanded);
    void sectionRenameRequested(const QString& listId, const QString& sectionId, const QString& name);
    void sectionAddBelowRequested(const QString& listId, const QString& sectionId);
    void sectionRemoveRequested(const QString& listId, const QString& sectionId);
    void rowsMoveRequested(const QString& listId, const QStringList& identities,
                           const QString& targetIdentity, const QString& targetSection, bool after);
    void sectionMoveRequested(const QString& listId, const QString& sourceSection,
                              const QString& targetSection, bool after);
    void relativeMoveRequested(const QString& listId, const QStringList& identities,
                               const QString& sectionId, int delta);
    void removeRequested(const QString& listId, const QStringList& identities, const QString& sectionId);
    void rowContextRequested(const QString& listId, const QStringList& identities, const QPoint& globalPosition);

protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    bool dropMimeData(QTreeWidgetItem*, int, const QMimeData*, Qt::DropAction) override;
    Qt::DropActions supportedDropActions() const override;
    void startDrag(Qt::DropActions) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void drawRow(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void scrollContentsBy(int dx, int dy) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

private:
    struct DropTarget {
        bool valid = false;
        QPersistentModelIndex index;
        QString identity, section;
        bool after = false;
        bool intoSection = false;
    };

    bool isSection(const QModelIndex&) const;
    QModelIndex sectionAt(const QPoint&) const;
    QModelIndex indexForIdentity(const QString&) const;
    QModelIndex indexForSection(const QString&) const;
    QStringList orderedRowKeys(const QList<QTreeWidgetItem*>&) const;
    bool onDragHandle(const QPoint&) const;
    bool acceptsOwnMime(const QMimeData*) const;
    bool updateDropTarget(const QMimeData*, const QPoint&);
    void clearDragFeedback();
    QRect dropLineRect() const;
    void updateHover(const QPoint&);
    void clearHover();
    void positionHoverActions();
    void runSectionAction(bool add);
    void beginSectionEdit(const QModelIndex&);
    void finishSectionEdit(bool commit, bool restoreFocus);
    void positionEditor();
    void positionOverlays();
    void scheduleHoverLeave();

    QString listId_;
    bool sectionsEnabled_ = false;
    bool manualOrder_ = false;
    bool activationSuppressed_ = false;
    bool suppressReleaseActivation_ = false;
    bool dragGesture_ = false;
    bool dragExecuting_ = false;
    quint64 interactionRevision_ = 0;
    QPersistentModelIndex hoveredSection_, hoveredRow_, pressedSection_, editorIndex_;
    int pressedColumn_ = -1;
    QPointer<QDrag> activeDrag_;
    QList<QPersistentModelIndex> draggingIndexes_;
    DropTarget dropTarget_;
    QPoint pressPosition_;
    bool ignoreNextRelease_ = false;
    QPointer<QToolButton> addSection_, removeSection_;
    QPointer<QLineEdit> editor_;
    QString editorListId_, editorSectionId_;
    QList<QMetaObject::Connection> viewConnections_;
};

} // namespace keefi
