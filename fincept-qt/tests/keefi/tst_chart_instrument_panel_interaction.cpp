#include "keefi/AccountModel.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketCandles.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <functional>
#include <memory>

using namespace keefi;

namespace {
constexpr int IdentityRole = Qt::UserRole;
constexpr int SectionRole = Qt::UserRole + 1;
const WatchlistIdentity Btc{"coin", "upbit", "KRW-BTC"};
const WatchlistIdentity Eth{"coin", "upbit", "KRW-ETH"};
const WatchlistIdentity Sol{"coin", "upbit", "KRW-SOL"};
const WatchlistIdentity Xrp{"coin", "upbit", "KRW-XRP"};

struct SectionPaintRecord { QStringList texts; };
class SectionTextEngine final : public QPaintEngine {
public:
    explicit SectionTextEngine(SectionPaintRecord& record)
        : QPaintEngine(QPaintEngine::AllFeatures), record_(record) {}
    bool begin(QPaintDevice*) override { setActive(true); return true; }
    bool end() override { setActive(false); return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawPath(const QPainterPath&) override {}
    // Qt's default polygon overloads call each other; this text recorder must
    // consume non-text primitives instead of entering that fallback recursion.
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawPolygon(const QPoint*, int, PolygonDrawMode) override {}
    void drawTextItem(const QPointF&, const QTextItem& text) override { record_.texts.append(text.text()); }
private:
    SectionPaintRecord& record_;
};
class SectionTextDevice final : public QPaintDevice {
public:
    explicit SectionTextDevice(SectionPaintRecord& record) : engine(record) {}
    mutable SectionTextEngine engine;
    QPaintEngine* paintEngine() const override { return &engine; }
protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth: return 1440;
        case PdmHeight: return 900;
        case PdmWidthMM: return 381;
        case PdmHeightMM: return 238;
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDepth: return 32;
        case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return QPaintDevice::metric(metric);
        }
    }
};
class SectionPaintAccess : public QTreeWidget {
public:
    static void paintActualRow(QTreeWidget* tree, QPainter* painter,
                               const QStyleOptionViewItem& option, const QModelIndex& index) {
        // A pointer to the inherited virtual member preserves dispatch to the
        // actual tree. No downcast or test-only production interface is used.
        const auto draw = &SectionPaintAccess::drawRow;
        (tree->*draw)(painter, option, index);
    }
};
QString paintedSectionText(QTreeWidget* tree, QTreeWidgetItem* section) {
    SectionPaintRecord record;
    SectionTextDevice device(record);
    QStyleOptionViewItem option;
    option.initFrom(tree);
    option.widget = tree;
    option.rect = tree->visualItemRect(section);
    option.state = QStyle::State_Enabled | QStyle::State_Active;
    QPainter painter(&device);
    SectionPaintAccess::paintActualRow(tree, &painter, option, tree->indexFromItem(section));
    painter.end();
    return record.texts.join(QString{});
}

// Every transport in this suite records locally and never opens a connection.
class InteractionTransport final : public TradingTransport {
public:
    QList<RequestSpec> requests;
    void send(quint64, const RequestSpec& request) override { requests.append(request); }
    void abort() override {}
};

QTreeWidgetItem* identityRow(QTreeWidget* tree, const QString& key) {
    if (tree) for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, IdentityRole).toString() == key) return *it;
    return nullptr;
}
QTreeWidgetItem* sectionRow(QTreeWidget* tree, const QString& id) {
    if (tree) for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* row = tree->topLevelItem(i);
        if (row->data(0, IdentityRole).toString().isEmpty() && row->data(0, SectionRole).toString() == id) return row;
    }
    return nullptr;
}
QStringList modelKeys(const WatchlistUserList& list, const QString& section) {
    QStringList result;
    for (const auto& entry : list.entries) if (entry.sectionId == section) result.append(entry.identity.key());
    return result;
}
QStringList selectedKeys(QTreeWidget* tree) {
    QStringList result;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->isSelected() && !(*it)->data(0, IdentityRole).toString().isEmpty())
            result.append((*it)->data(0, IdentityRole).toString());
    return result;
}
QPoint rowPoint(QTreeWidget* tree, QTreeWidgetItem* row, bool handle = false) {
    const auto rect = tree->visualItemRect(row);
    return QPoint(rect.left() + qMin(handle ? 12 : 110, qMax(1, rect.width() / 3)), rect.center().y());
}
QAbstractButton* sectionHoverAction(QTreeWidget* tree, QTreeWidgetItem* section, bool add) {
    const auto row = tree->visualItemRect(section);
    for (auto* button : tree->viewport()->findChildren<QAbstractButton*>()) {
        if (!button->isVisible() || !row.contains(button->mapTo(tree->viewport(), button->rect().center()))) continue;
        const auto text = QString(button->text()).remove('&');
        const auto accessible = button->accessibleName();
        if (add ? text == "＋" || text == "+" || accessible.contains("섹션 추가")
                : text == "✕" || text == "×" || accessible.contains("섹션 삭제")) return button;
    }
    return nullptr;
}
QLineEdit* visibleEditor(QTreeWidget* tree) {
    for (auto* editor : tree->findChildren<QLineEdit*>())
        if (editor->isVisible() && editor->hasFocus()) return editor;
    return nullptr;
}
QLineEdit* beginInlineEdit(QTreeWidget* tree, QTreeWidgetItem* section) {
    const auto point = rowPoint(tree, section);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    return visibleEditor(tree);
}
bool openWatch(ChartWorkspace& workspace) {
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    if (!open || !panel || !watch) return false;
    if (!panel->isVisible()) QTest::mouseClick(open, Qt::LeftButton);
    QCoreApplication::processEvents();
    QTest::mouseClick(watch, Qt::LeftButton);
    QCoreApplication::processEvents();
    return panel->isVisible();
}
bool clickHeading(QTreeWidget* tree, const QString& text) {
    auto* header = tree->header();
    for (int column = 0; column < tree->columnCount(); ++column) {
        if (tree->isColumnHidden(column) || !tree->headerItem()->text(column).startsWith(text)) continue;
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2, header->height() / 2));
        QCoreApplication::processEvents();
        return true;
    }
    return false;
}

struct InteractionFixture {
    InteractionTransport http;
    MarketCandles candles{&http, std::unique_ptr<PublicCandleSocket>{}};
    WatchlistWorkspaceList lists;
    ChartWorkspace workspace{&candles, nullptr};
    QString list, first, second;
    bool seeded = false;
    InteractionFixture() {
        list = lists.createList("합성 이동 목록");
        first = lists.addSection(list, "메이저");
        second = lists.addSection(list, "다른 섹션");
        seeded = !list.isEmpty() && !first.isEmpty() && !second.isEmpty() &&
            lists.addIdentity(list, Btc, first) && lists.addIdentity(list, Eth, first) &&
            lists.addIdentity(list, Sol, first) && lists.addIdentity(list, Xrp, second);
        lists.useList(list);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setWatchlistModel(&lists);
        workspace.resize(1440, 900);
        workspace.show();
        QCoreApplication::processEvents();
    }
    QTreeWidget* tree() { return workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); }
};

struct DragResult { bool enter = false, move = false, drop = false, during = false; };

// Exercise the real view MIME and viewport handlers. The timer also runs inside
// a native QDrag nested loop. Cancellation is only bounded test cleanup after
// delivery; the model must have changed in the viewport drop handler itself.
DragResult dragView(QAbstractItemView* view, const QPoint& from, const QPoint& to,
                    const std::function<void()>& during = {}, QMimeData* supplied = nullptr) {
    DragResult result;
    std::unique_ptr<QMimeData> owned;
    if (!supplied) owned.reset(view->model()->mimeData(view->selectionModel()->selectedIndexes()));
    const auto* mime = supplied ? supplied : owned.get();
    if (!mime) return result;
    auto* viewport = view->viewport();
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, from);
    QTimer deliver;
    deliver.setSingleShot(true);
    QObject::connect(&deliver, &QTimer::timeout, view, [&] {
        // Prefer the MIME from the actual in-progress drag when the platform
        // starts one; do not reconstruct or guess an application payload.
        const QMimeData* deliveryMime = mime;
        if (!supplied) {
            const auto drags = view->findChildren<QDrag*>();
            for (auto it = drags.crbegin(); it != drags.crend(); ++it)
                if ((*it)->mimeData()) { deliveryMime = (*it)->mimeData(); break; }
        }
        QDragEnterEvent enter(to, Qt::MoveAction, deliveryMime, Qt::LeftButton, Qt::NoModifier);
        enter.setAccepted(false);
        QApplication::sendEvent(viewport, &enter);
        result.enter = enter.isAccepted();
        QDragMoveEvent move(to, Qt::MoveAction, deliveryMime, Qt::LeftButton, Qt::NoModifier);
        move.setAccepted(false);
        QApplication::sendEvent(viewport, &move);
        result.move = move.isAccepted();
        if (during) during();
        result.during = true;
        QDropEvent drop(QPointF(to), Qt::MoveAction, deliveryMime, Qt::LeftButton, Qt::NoModifier);
        drop.setAccepted(false);
        QApplication::sendEvent(viewport, &drop);
        result.drop = drop.isAccepted();
        QDrag::cancel();
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, to);
    });
    deliver.start(0);
    const auto moved = from + QPoint(qMax(24, QApplication::startDragDistance() + 4), 0);
    QMouseEvent motion(QEvent::MouseMove, QPointF(moved), QPointF(viewport->mapToGlobal(moved)),
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &motion);
    QCoreApplication::processEvents();
    return result;
}

struct ContextResult { bool opened = false, invoked = false; QStringList actions; };
ContextResult contextAction(QTreeWidget* tree, const QPoint& at, const QString& requested) {
    ContextResult result;
    QTimer choose, stop;
    QEventLoop loop;
    choose.setInterval(1);
    stop.setSingleShot(true);
    QObject::connect(&stop, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&choose, &QTimer::timeout, tree, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu || !menu->isVisible()) return;
        result.opened = true;
        QAction* wanted = nullptr;
        for (auto* action : menu->actions()) {
            if (!action->isVisible() || action->isSeparator()) continue;
            result.actions.append(action->text());
            if (action->text() == requested) wanted = action;
        }
        choose.stop();
        if (wanted && wanted->isEnabled()) {
            result.invoked = true;
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(wanted).center());
        } else QTest::keyClick(menu, Qt::Key_Escape);
        loop.quit();
    });
    choose.start();
    stop.start(500);
    QContextMenuEvent event(QContextMenuEvent::Mouse, at, tree->viewport()->mapToGlobal(at));
    QApplication::sendEvent(tree->viewport(), &event);
    if (!result.opened) loop.exec();
    choose.stop();
    stop.stop();
    return result;
}
QStringList popupListIds(QAbstractItemView* view) {
    QStringList ids;
    for (int row = 0; row < view->model()->rowCount(); ++row)
        ids.append(view->model()->index(row, 0).data(Qt::UserRole).toString());
    return ids;
}
}

class ChartInstrumentPanelInteractionTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase();
    void listSelectorProjectsRecentAndManualOrderWithoutMutation();
    void sectionHeaderGeometryAndPaintedCountSurviveCollapse();
    void hoveringSectionAddsBelowWithoutMovingMembers();
    void hoveringSectionDeleteKeepsMembersUnsectioned();
    void sectionClickHidesChildrenWithoutChangingSavedMembers();
    void collapsePersistsPerListAcrossWindowRecreation();
    void inlineEditorSurvivesOneHundredQuotesAndOwnsEscape();
    void inlineEditorRejectsInvalidNames_data();
    void inlineEditorRejectsInvalidNames();
    void rowDropAppendsToCollapsedOrEmptySection_data();
    void rowDropAppendsToCollapsedOrEmptySection();
    void multiSelectionDragPreservesRelativeOrder();
    void multiSelectionDragKeepsVisibleOrderAcrossReorderedSections();
    void sectionDragUsesTheSavedSectionOrder();
    void listSelectorDragUpdatesTheSavedListOrder();
    void optionArrowsMoveRowsAndDeleteOnlySelectedFavorites();
    void optionArrowsMoveSectionsWithoutChangingEntries();
    void spaceStillTogglesFavoritesAndEditorOwnsDelete();
    void sortedViewRejectsDragUntilSavedOrderIsRestored();
    void draggingSurvivesOneHundredQuoteUpdates();
    void clearPrivateDuringListMutationImmediatelyRedactsRows_data();
    void clearPrivateDuringListMutationImmediatelyRedactsRows();
    void rowContextMenuMovesAlertsAndUnstarsWithoutReads();
    void foreignMimeDoesNotChangeTheList();
    void completedInteractionMenuHasEightActions();
};

void ChartInstrumentPanelInteractionTest::initTestCase() {
    QVERIFY(settings_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
}

void ChartInstrumentPanelInteractionTest::listSelectorProjectsRecentAndManualOrderWithoutMutation() {
    InteractionFixture f;
    const auto second = f.lists.createList("합성 둘째 목록");
    const auto third = f.lists.createList("합성 셋째 목록");
    QVERIFY(f.seeded && !second.isEmpty() && !third.isEmpty());
    f.lists.useList(second);
    f.lists.useList(third);
    QVERIFY(openWatch(f.workspace));
    auto* combo = f.workspace.findChild<QComboBox*>("chartWorkspaceListSelector");
    QVERIFY(combo && combo->count() == 3);
    const auto saved = f.lists.publicState();
    const auto lists = f.lists.userLists();
    const auto selected = f.workspace.currentListId();
    const auto requests = f.http.requests.size();
    const auto openPopup = [combo] {
        if (!combo->view()->isVisible())
            QTest::mouseClick(combo, Qt::LeftButton, Qt::NoModifier, QPoint(combo->width() - 8, combo->height() / 2));
        QCoreApplication::processEvents();
    };
    openPopup();
    auto* view = combo->view();
    QVERIFY(view->isVisible());
    QCOMPARE(view->model()->rowCount(), 3);
    QCOMPARE(popupListIds(view), (QStringList{third, second, f.list}));
    QCOMPARE(f.workspace.currentListId(), selected);
    QCOMPARE(f.lists.publicState(), saved);
    QPointer<QAbstractButton> manual = view->window()->findChild<QAbstractButton*>("chartListOrder_manual");
    QPointer<QAbstractButton> recent = view->window()->findChild<QAbstractButton*>("chartListOrder_recent");
    QVERIFY(manual && recent && manual->isVisible() && recent->isVisible());
    QTest::mouseClick(manual, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCOMPARE(view->model()->rowCount(), 3);
    QCOMPARE(popupListIds(view), (QStringList{f.list, second, third}));
    QCOMPARE(f.workspace.currentListId(), selected);
    QCOMPARE(f.lists.publicState(), saved);
    QCOMPARE(f.lists.userLists(), lists);
    openPopup();
    recent = view->window()->findChild<QAbstractButton*>("chartListOrder_recent");
    QVERIFY(recent && recent->isVisible());
    QTest::mouseClick(recent, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCOMPARE(view->model()->rowCount(), 3);
    QCOMPARE(popupListIds(view), (QStringList{third, second, f.list}));
    QCOMPARE(f.workspace.currentListId(), selected);
    QCOMPARE(f.lists.publicState(), saved);
    QCOMPARE(f.lists.userLists(), lists);
    QCOMPARE(f.http.requests.size(), requests);
    QTest::keyClick(view, Qt::Key_Escape);
}

void ChartInstrumentPanelInteractionTest::sectionHeaderGeometryAndPaintedCountSurviveCollapse() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    QVERIFY(tree);
    const auto saved = f.lists.publicState();
    const auto requests = f.http.requests.size();
    for (const auto& id : {f.first, f.second}) {
        auto* section = sectionRow(tree, id);
        QVERIFY(section && !section->isHidden());
        QCOMPARE(tree->visualItemRect(section).height(), 26);
        const auto painted = paintedSectionText(tree, section);
        QVERIFY2(painted.contains(section->text(0)), qPrintable(painted));
        const QRegularExpression count(QStringLiteral("(?:^|\\D)%1(?:\\D|$)").arg(section->childCount()));
        QVERIFY2(count.match(painted).hasMatch(), qPrintable(painted));
    }
    auto* first = sectionRow(tree, f.first);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, first));
    QVERIFY(!first->isExpanded());
    QCOMPARE(first->childCount(), 3);
    QCOMPARE(tree->visualItemRect(first).height(), 26);
    const auto collapsed = paintedSectionText(tree, first);
    QVERIFY2(collapsed.contains("메이저"), qPrintable(collapsed));
    QVERIFY2(QRegularExpression(QStringLiteral("(?:^|\\D)3(?:\\D|$)")).match(collapsed).hasMatch(), qPrintable(collapsed));
    QCOMPARE(f.lists.publicState(), saved);
    QCOMPARE(f.http.requests.size(), requests);
}

void ChartInstrumentPanelInteractionTest::hoveringSectionAddsBelowWithoutMovingMembers() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* first = sectionRow(tree, f.first);
    QVERIFY(first);
    const auto before = f.lists.userList(f.list);
    const auto requests = f.http.requests.size();
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    // Enter from a different point; consecutive fixtures can retain the same cursor position.
    QTest::mouseMove(tree->viewport(), tree->viewport()->rect().bottomLeft() + QPoint(1, -1));
    QTest::mouseMove(tree->viewport(), rowPoint(tree, first));
    QTRY_VERIFY_WITH_TIMEOUT(sectionHoverAction(tree, first, true), 500);
    QPointer<QAbstractButton> add = sectionHoverAction(tree, first, true);

    // Reuse the existing name dialog when present. An implementation that adds
    // an empty section first and then offers inline naming is also admissible.
    QTimer answer, timeout;
    answer.setInterval(1);
    timeout.setSingleShot(true);
    bool named = false;
    connect(&answer, &QTimer::timeout, tree, [&] {
        if (named) return;
        QLineEdit* editor = nullptr;
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
            editor = dialog->findChild<QLineEdit*>();
        if (!editor) editor = visibleEditor(tree);
        if (!editor) return;
        named = true;
        editor->selectAll();
        QTest::keyClicks(editor, "Added section");
        QTest::keyClick(editor, Qt::Key_Return);
    });
    connect(&timeout, &QTimer::timeout, tree, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            QTest::keyClick(dialog, Qt::Key_Escape);
    });
    answer.start();
    timeout.start(1500);
    QTest::mouseClick(add, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(f.lists.userList(f.list).sections.size(), before.sections.size() + 1, 1000);
    answer.stop();
    timeout.stop();
    if (auto* editor = visibleEditor(tree)) QTest::keyClick(editor, Qt::Key_Escape);
    const auto after = f.lists.userList(f.list);
    QCOMPARE(after.sections.size(), 3);
    QCOMPARE(after.sections.at(0).id, f.first);
    QCOMPARE(after.sections.at(2).id, f.second);
    const auto inserted = after.sections.at(1).id;
    QVERIFY(!inserted.isEmpty() && inserted != f.first && inserted != f.second);
    QVERIFY(!after.sections.at(1).name.trimmed().isEmpty());
    QCOMPARE(after.entries, before.entries);
    auto* row = sectionRow(tree, inserted);
    QVERIFY(row && !row->isHidden());
    QCOMPARE(row->childCount(), 0);
    QCOMPARE(opened.count(), 0);
    QCOMPARE(f.http.requests.size(), requests);
    WatchlistWorkspaceList restored;
    QVERIFY(restored.restorePublicState(f.lists.publicState()));
    QCOMPARE(restored.userList(f.list), after);
}

void ChartInstrumentPanelInteractionTest::hoveringSectionDeleteKeepsMembersUnsectioned() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* first = sectionRow(tree, f.first);
    QVERIFY(first);
    const auto before = f.lists.userList(f.list);
    auto expectedEntries = before.entries;
    for (auto& entry : expectedEntries) if (entry.sectionId == f.first) entry.sectionId.clear();
    const auto requests = f.http.requests.size();
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    // Enter from a different point; consecutive fixtures can retain the same cursor position.
    QTest::mouseMove(tree->viewport(), tree->viewport()->rect().bottomLeft() + QPoint(1, -1));
    QTest::mouseMove(tree->viewport(), rowPoint(tree, first));
    QTRY_VERIFY_WITH_TIMEOUT(sectionHoverAction(tree, first, false), 500);
    QPointer<QAbstractButton> remove = sectionHoverAction(tree, first, false);
    QTest::mouseClick(remove, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(f.lists.userList(f.list).sections.size(), before.sections.size() - 1, 1000);
    const auto after = f.lists.userList(f.list);
    QCOMPARE(after.sections.size(), 1);
    QCOMPARE(after.sections.first().id, f.second);
    QCOMPARE(after.entries, expectedEntries);
    QCOMPARE(after.entries.size(), before.entries.size());
    QVERIFY(!sectionRow(tree, f.first));
    QCOMPARE(modelKeys(after, QString{}), (QStringList{Btc.key(), Eth.key(), Sol.key()}));
    QCOMPARE(modelKeys(after, f.second), QStringList{Xrp.key()});
    for (const auto& identity : {Btc, Eth, Sol}) {
        auto* row = identityRow(tree, identity.key());
        QVERIFY(row && !row->parent());
        QCOMPARE(row->data(0, SectionRole).toString(), QString{});
    }
    QCOMPARE(opened.count(), 0);
    QCOMPARE(f.http.requests.size(), requests);
    WatchlistWorkspaceList restored;
    QVERIFY(restored.restorePublicState(f.lists.publicState()));
    QCOMPARE(restored.userList(f.list), after);
}

void ChartInstrumentPanelInteractionTest::sectionClickHidesChildrenWithoutChangingSavedMembers() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* section = sectionRow(tree, f.first);
    QVERIFY(tree && section && section->isExpanded());
    QCOMPARE(section->childCount(), 3);
    const auto before = f.lists.publicState();
    const auto requests = f.http.requests.size();
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, section));
    QVERIFY2(!section->isExpanded(), "A section header single click must collapse its children");
    QVERIFY(!tree->visualItemRect(section).isEmpty());
    QCOMPARE(section->childCount(), 3);
    QVERIFY(tree->visualItemRect(section->child(0)).isEmpty());
    const auto visible = f.workspace.visiblePublicIdentityKeysForDisplay();
    for (const auto& key : modelKeys(f.lists.userList(f.list), f.first)) QVERIFY(!visible.contains(key));
    QCOMPARE(f.lists.publicState(), before);
    QCOMPARE(opened.count(), 0);
    QCOMPARE(f.http.requests.size(), requests);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, section));
    QVERIFY(section->isExpanded());
    QCOMPARE(f.lists.publicState(), before);
}

void ChartInstrumentPanelInteractionTest::collapsePersistsPerListAcrossWindowRecreation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings profile(directory.filePath("synthetic-dev98-collapse.ini"), QSettings::IniFormat);
    WatchlistWorkspaceList seed;
    const auto firstList = seed.createList("합성 첫 목록"), secondList = seed.createList("합성 둘째 목록");
    const auto firstSection = seed.addSection(firstList, "같은 이름"), secondSection = seed.addSection(secondList, "같은 이름");
    QVERIFY(seed.addIdentity(firstList, Btc, firstSection));
    QVERIFY(seed.addIdentity(secondList, Eth, secondSection));
    seed.useList(firstList);
    profile.setValue("chart/watchlists/v2", seed.publicState());
    profile.sync();
    for (int pass = 0; pass < 2; ++pass) {
        Vault vault(directory.filePath("synthetic-unopened-vault"));
        AccountModel accounts(&vault);
        PortfolioController::Factories factories;
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        InteractionTransport http, metadata;
        MarketCandles candles(&http, std::unique_ptr<PublicCandleSocket>{});
        LocalWindow window(vault, accounts);
        window.setChartSettings(&profile);
        window.setPortfolioController(&portfolio);
        window.setChartInstrumentTransport(&metadata);
        window.setMarketCandles(&candles);
        auto* workspace = window.findChild<ChartWorkspace*>("chartWorkspace");
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        QVERIFY(workspace && pages);
        workspace->indicatorPreferences()->markIntroSeen();
        window.resize(1440, 900);
        window.show();
        pages->setCurrentWidget(workspace);
        QCoreApplication::processEvents();
        QVERIFY(openWatch(*workspace));
        auto* selector = workspace->findChild<QComboBox*>("chartWorkspaceListSelector");
        auto* tree = workspace->findChild<QTreeWidget*>("chartWorkspaceWatchlist");
        QVERIFY(selector && tree);
        selector->setCurrentIndex(selector->findData(firstList));
        auto* first = sectionRow(tree, firstSection);
        QVERIFY(first);
        if (pass == 0) {
            const auto before = workspace->watchlistModel()->publicState();
            QVERIFY(first->isExpanded());
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, first));
            QVERIFY(!first->isExpanded());
            QCOMPARE(workspace->watchlistModel()->publicState(), before);
        } else QVERIFY2(!first->isExpanded(), "Collapsed state must survive destruction and recreation using the same injected profile");
        selector->setCurrentIndex(selector->findData(secondList));
        auto* second = sectionRow(tree, secondSection);
        QVERIFY(second);
        QVERIFY2(second->isExpanded(), "One list's collapse state must not leak into another list with the same section name");
        selector->setCurrentIndex(selector->findData(firstList));
        first = sectionRow(tree, firstSection);
        QVERIFY(first && !first->isExpanded());
        profile.sync();
        QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(), workspace->watchlistModel()->publicState());
        QVERIFY(!vault.exists());
        QVERIFY(!vault.isUnlocked());
        window.hide();
    }
}

void ChartInstrumentPanelInteractionTest::inlineEditorSurvivesOneHundredQuotesAndOwnsEscape() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* section = sectionRow(tree, f.first);
    QVERIFY(section);
    const QPersistentModelIndex sectionIndex(tree->indexFromItem(section));
    const QPersistentModelIndex rowIndex(tree->indexFromItem(identityRow(tree, Btc.key())));
    const auto saved = f.lists.publicState();
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    QPointer<QLineEdit> editor = beginInlineEdit(tree, section);
    QVERIFY2(editor, "A section double click must open an inline editor in the tree");
    editor->selectAll();
    QTest::keyClicks(editor, "Draft section");
    for (int update = 0; update < 100; ++update) {
        WatchlistRowState quote; quote.price = QString::number(1000 + update); quote.change = "+1.00%";
        f.lists.setRowState(Btc, quote);
        QCoreApplication::processEvents();
        QVERIFY(editor && editor->isVisible());
        QCOMPARE(editor->text(), QString("Draft section"));
        QVERIFY(sectionIndex.isValid() && rowIndex.isValid());
    }
    QCOMPARE(resets.count(), 0);
    QCOMPARE(f.lists.publicState(), saved);
    QTest::keyClick(editor, Qt::Key_Escape);
    QTRY_VERIFY(!editor || !editor->isVisible());
    QCOMPARE(f.lists.publicState(), saved);
    QVERIFY(f.workspace.findChild<QWidget*>("chartInstrumentPanel")->isVisible());
    QCOMPARE(opened.count(), 0);
    section = sectionRow(tree, f.first);
    editor = beginInlineEdit(tree, section);
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClicks(editor, "Renamed section");
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_COMPARE(f.lists.userList(f.list).sections.first().name, QString("Renamed section"));
    QVERIFY(sectionIndex.isValid() && rowIndex.isValid());
    QCOMPARE(resets.count(), 0);
    QCOMPARE(opened.count(), 0);
}

void ChartInstrumentPanelInteractionTest::inlineEditorRejectsInvalidNames_data() {
    // Preserve renameSection's existing publicText and same-list uniqueness rules.
    QTest::addColumn<QString>("name");
    QTest::newRow("empty") << QString{};
    QTest::newRow("duplicate") << QString("Other");
}
void ChartInstrumentPanelInteractionTest::inlineEditorRejectsInvalidNames() {
    QFETCH(QString, name);
    InteractionFixture f;
    QVERIFY(f.seeded && f.lists.renameSection(f.list, f.second, "Other") && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* section = sectionRow(tree, f.first);
    const auto before = f.lists.publicState();
    QPointer<QLineEdit> editor = beginInlineEdit(tree, section);
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClick(editor, Qt::Key_Backspace);
    if (!name.isEmpty()) QTest::keyClicks(editor, name);
    QTest::keyClick(editor, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(f.lists.publicState(), before);
    if (editor && editor->isVisible()) QTest::keyClick(editor, Qt::Key_Escape);
    QCOMPARE(sectionRow(tree, f.first)->text(0), QString("메이저"));
}

void ChartInstrumentPanelInteractionTest::rowDropAppendsToCollapsedOrEmptySection_data() {
    QTest::addColumn<bool>("empty");
    QTest::newRow("collapsed-populated") << false;
    QTest::newRow("empty") << true;
}
void ChartInstrumentPanelInteractionTest::rowDropAppendsToCollapsedOrEmptySection() {
    QFETCH(bool, empty);
    InteractionFixture f;
    QVERIFY(f.seeded);
    if (empty) QVERIFY(f.lists.removeIdentity(f.list, Xrp));
    QVERIFY(openWatch(f.workspace));
    auto* tree = f.tree();
    auto* target = sectionRow(tree, f.second);
    QVERIFY2(target && !target->isHidden(), "Empty sections must remain reachable drop targets");
    target->setExpanded(false);
    auto* source = identityRow(tree, Btc.key());
    QVERIFY(source);
    tree->setCurrentItem(source);
    const auto count = f.lists.userList(f.list).entries.size();
    const auto result = dragView(tree, rowPoint(tree, source, true), rowPoint(tree, target));
    QVERIFY2(result.enter && result.move && result.drop, "The actual viewport must accept a saved-row move");
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.second), empty ? QStringList{Btc.key()} : QStringList({Xrp.key(), Btc.key()}));
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), (QStringList{Eth.key(), Sol.key()}));
    QCOMPARE(f.lists.userList(f.list).entries.size(), count);
    WatchlistWorkspaceList restored;
    QVERIFY(restored.restorePublicState(f.lists.publicState()));
    QCOMPARE(restored.userList(f.list), f.lists.userList(f.list));
}

void ChartInstrumentPanelInteractionTest::multiSelectionDragPreservesRelativeOrder() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* btc = identityRow(tree, Btc.key());
    auto* sol = identityRow(tree, Sol.key());
    QVERIFY(btc && sol);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, btc, true));
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ControlModifier, rowPoint(tree, sol, true));
    QCOMPARE(selectedKeys(tree), (QStringList{Btc.key(), Sol.key()}));
    auto* target = sectionRow(tree, f.second);
    QVERIFY(target);
    target->setExpanded(false); // The specified append position is a collapsed header.
    const auto result = dragView(tree, rowPoint(tree, identityRow(tree, Btc.key()), true), rowPoint(tree, target));
    QVERIFY(result.drop);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.second), (QStringList{Xrp.key(), Btc.key(), Sol.key()}));
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), QStringList{Eth.key()});
}

void ChartInstrumentPanelInteractionTest::multiSelectionDragKeepsVisibleOrderAcrossReorderedSections() {
    InteractionFixture f;
    QVERIFY(f.seeded);
    const auto destination=f.lists.addSection(f.list,"합성 대상");
    QVERIFY(!destination.isEmpty());
    QVERIFY(f.lists.moveSection(f.list,f.second,0));
    QVERIFY(openWatch(f.workspace));
    auto* tree=f.tree();
    auto* xrp=identityRow(tree,Xrp.key());
    auto* btc=identityRow(tree,Btc.key());
    QVERIFY(xrp&&btc);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,rowPoint(tree,xrp,true));
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::ControlModifier,rowPoint(tree,btc,true));
    QCOMPARE(selectedKeys(tree),(QStringList{Xrp.key(),Btc.key()}));
    auto* target=sectionRow(tree,destination);
    QVERIFY(target);target->setExpanded(false);
    const auto result=dragView(tree,rowPoint(tree,xrp,true),rowPoint(tree,target));
    QVERIFY(result.drop);
    QCOMPARE(modelKeys(f.lists.userList(f.list),destination),(QStringList{Xrp.key(),Btc.key()}));
    QCOMPARE(modelKeys(f.lists.userList(f.list),f.first),(QStringList{Eth.key(),Sol.key()}));
    QVERIFY(modelKeys(f.lists.userList(f.list),f.second).isEmpty());
}

void ChartInstrumentPanelInteractionTest::sectionDragUsesTheSavedSectionOrder() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* source = sectionRow(tree, f.second);
    auto* target = sectionRow(tree, f.first);
    QVERIFY(source && target);
    const auto entries = f.lists.userList(f.list).entries;
    tree->setCurrentItem(source);
    const auto targetRect = tree->visualItemRect(target);
    const auto result = dragView(tree, rowPoint(tree, source, true), QPoint(targetRect.left() + 20, targetRect.top() + 1));
    QVERIFY(result.drop);
    QCOMPARE(f.lists.userList(f.list).sections.first().id, f.second);
    QCOMPARE(f.lists.userList(f.list).entries, entries);
    QCOMPARE(tree->topLevelItem(0)->data(0, SectionRole).toString(), f.second);
}

void ChartInstrumentPanelInteractionTest::listSelectorDragUpdatesTheSavedListOrder() {
    InteractionFixture f;
    const auto other = f.lists.createList("합성 다른 목록");
    QVERIFY(f.seeded && !other.isEmpty() && openWatch(f.workspace));
    auto* combo = f.workspace.findChild<QComboBox*>("chartWorkspaceListSelector");
    QVERIFY(combo && combo->count() == 2);
    const auto current = f.workspace.currentListId();
    QTest::mouseClick(combo, Qt::LeftButton, Qt::NoModifier, QPoint(combo->width() - 8, combo->height() / 2));
    QCoreApplication::processEvents();
    auto* view = combo->view();
    QVERIFY(view->isVisible());
    const auto first = view->model()->index(combo->findData(f.list), 0);
    const auto last = view->model()->index(combo->findData(other), 0);
    view->selectionModel()->setCurrentIndex(first, QItemSelectionModel::ClearAndSelect);
    const auto destination = view->visualRect(last);
    const auto result = dragView(view, view->visualRect(first).center(), QPoint(destination.center().x(), destination.bottom() - 1));
    QVERIFY(result.drop);
    QCOMPARE(f.lists.userLists().last().id, f.list);
    QCOMPARE(f.workspace.currentListId(), current);
    WatchlistWorkspaceList restored;
    QVERIFY(restored.restorePublicState(f.lists.publicState()));
    QCOMPARE(restored.userLists(), f.lists.userLists());
    QTest::keyClick(view, Qt::Key_Escape);
}

void ChartInstrumentPanelInteractionTest::optionArrowsMoveRowsAndDeleteOnlySelectedFavorites() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* btc = identityRow(tree, Btc.key());
    auto* eth = identityRow(tree, Eth.key());
    QVERIFY(btc && eth);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, btc, true));
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ShiftModifier, rowPoint(tree, eth, true));
    QCOMPARE(selectedKeys(tree), (QStringList{Btc.key(), Eth.key()}));
    const auto selectedChart = f.workspace.selectedChartIdentity();
    const auto requests = f.http.requests.size();
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Down, Qt::AltModifier);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), (QStringList{Sol.key(), Btc.key(), Eth.key()}));
    QCOMPARE(selectedKeys(tree), (QStringList{Btc.key(), Eth.key()}));
    QVERIFY(tree->currentItem());
    const auto announcement = tree->accessibleDescription() + tree->currentItem()->data(0, Qt::AccessibleDescriptionRole).toString();
    QVERIFY2(announcement.contains("옮") && announcement.contains("메이저"), qPrintable(announcement));
    QTest::keyClick(tree, Qt::Key_Up, Qt::AltModifier);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), (QStringList{Btc.key(), Eth.key(), Sol.key()}));
    const auto top = f.lists.publicState();
    QTest::keyClick(tree, Qt::Key_Up, Qt::AltModifier);
    QCOMPARE(f.lists.publicState(), top);
    QTest::keyClick(tree, Qt::Key_Delete);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), QStringList{Sol.key()});
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.second), QStringList{Xrp.key()});
    QCOMPARE(f.workspace.selectedChartIdentity(), selectedChart);
    QCOMPARE(opened.count(), 0);
    QCOMPARE(f.http.requests.size(), requests);
}

void ChartInstrumentPanelInteractionTest::optionArrowsMoveSectionsWithoutChangingEntries() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* second = sectionRow(tree, f.second);
    const auto entries = f.lists.userList(f.list).entries;
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, second));
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Up, Qt::AltModifier);
    QCOMPARE(f.lists.userList(f.list).sections.first().id, f.second);
    QCOMPARE(f.lists.userList(f.list).entries, entries);
    QTest::keyClick(tree, Qt::Key_Down, Qt::AltModifier);
    QCOMPARE(f.lists.userList(f.list).sections.first().id, f.first);
    QCOMPARE(f.lists.userList(f.list).entries, entries);
}

void ChartInstrumentPanelInteractionTest::spaceStillTogglesFavoritesAndEditorOwnsDelete() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    tree->setCurrentItem(identityRow(tree, Btc.key()));
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Space);
    QTRY_COMPARE(modelKeys(f.lists.userList(f.list), f.first), (QStringList{Eth.key(), Sol.key()}));
    auto* section = sectionRow(tree, f.first);
    QPointer<QLineEdit> editor = beginInlineEdit(tree, section);
    QVERIFY(editor);
    const auto before = f.lists.publicState();
    editor->selectAll();
    QTest::keyClicks(editor, "Draft");
    editor->setCursorPosition(0);
    QTest::keyClick(editor, Qt::Key_Delete);
    editor->setCursorPosition(editor->text().size());
    QTest::keyClick(editor, Qt::Key_Space);
    QCOMPARE(editor->text(), QString("raft "));
    QCOMPARE(f.lists.publicState(), before);
    QTest::keyClick(editor, Qt::Key_Escape);
    QCOMPARE(f.lists.publicState(), before);
}

void ChartInstrumentPanelInteractionTest::sortedViewRejectsDragUntilSavedOrderIsRestored() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    const auto saved = f.lists.publicState();
    QVERIFY(clickHeading(tree, "종목"));
    QString explanation;
    for (auto* label : f.workspace.findChildren<QLabel*>()) if (label->isVisible()) explanation += label->text();
    QVERIFY2(explanation.contains("내 순서로 돌아가면 옮길 수 있어요"), qPrintable(explanation));
    tree->setCurrentItem(identityRow(tree, Btc.key()));
    auto* target = sectionRow(tree, f.second);
    QVERIFY(target);
    target->setExpanded(false);
    const auto rejected = dragView(tree, rowPoint(tree, identityRow(tree, Btc.key()), true), rowPoint(tree, target));
    QVERIFY(!rejected.drop);
    QCOMPARE(f.lists.publicState(), saved);
    QAbstractButton* restore = nullptr;
    for (auto* button : f.workspace.findChildren<QAbstractButton*>())
        if (button->isVisible() && button->text() == "내 순서로") restore = button;
    QVERIFY(restore);
    QTest::mouseClick(restore, Qt::LeftButton);
    tree->setCurrentItem(identityRow(tree, Btc.key()));
    target = sectionRow(tree, f.second);
    QVERIFY(target);
    target->setExpanded(false);
    const auto accepted = dragView(tree, rowPoint(tree, identityRow(tree, Btc.key()), true), rowPoint(tree, target));
    QVERIFY(accepted.drop);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.second), (QStringList{Xrp.key(), Btc.key()}));
}

void ChartInstrumentPanelInteractionTest::draggingSurvivesOneHundredQuoteUpdates() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* source = identityRow(tree, Btc.key());
    auto* neighbour = identityRow(tree, Eth.key());
    auto* target = sectionRow(tree, f.second);
    QVERIFY(source && neighbour && target);
    target->setExpanded(false);
    tree->setCurrentItem(source);
    const QPersistentModelIndex sourceIndex(tree->indexFromItem(source)), neighbourIndex(tree->indexFromItem(neighbour));
    const auto saved = f.lists.publicState();
    const auto requests = f.http.requests.size();
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    int observed = 0;
    bool stable = true;
    const auto result = dragView(tree, rowPoint(tree, source, true), rowPoint(tree, target), [&] {
        for (int update = 0; update < 100; ++update) {
            WatchlistRowState quote; quote.price = QString::number(2000 + update); quote.change = "+2.00%";
            f.lists.setRowState(Btc, quote);
            QCoreApplication::processEvents();
            ++observed;
            stable = stable && sourceIndex.isValid() && neighbourIndex.isValid() && resets.count() == 0 &&
                selectedKeys(tree) == QStringList{Btc.key()} && f.lists.publicState() == saved;
        }
    });
    QCOMPARE(observed, 100);
    QVERIFY2(stable, "Quote updates during drag must retain the selected rows, indexes and saved order until drop");
    QVERIFY(result.enter && result.move && result.drop);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.second), (QStringList{Xrp.key(), Btc.key()}));
    QCOMPARE(f.lists.rowState(Btc).price, QString("2099"));
    QCOMPARE(opened.count(), 0);
    QCOMPARE(f.http.requests.size(), requests);
}

void ChartInstrumentPanelInteractionTest::clearPrivateDuringListMutationImmediatelyRedactsRows_data() {
    QTest::addColumn<bool>("remove");
    QTest::newRow("move") << false;
    QTest::newRow("delete") << true;
}

void ChartInstrumentPanelInteractionTest::clearPrivateDuringListMutationImmediatelyRedactsRows() {
    QFETCH(bool, remove);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Vault vault(directory.filePath("synthetic-unopened-reentrant-vault"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    InteractionTransport http;
    MarketCandles candles(&http, std::unique_ptr<PublicCandleSocket>{});
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("합성 잠금 재진입 목록");
    const auto section = lists.addSection(list, "메이저");
    QVERIFY(!list.isEmpty() && !section.isEmpty());
    QVERIFY(lists.addIdentity(list, Btc, section));
    QVERIFY(lists.addIdentity(list, Eth, section));
    QVERIFY(lists.addIdentity(list, Sol, section));
    lists.useList(list);
    ChartWorkspace workspace(&candles, &portfolio);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    QCoreApplication::processEvents();
    QVERIFY(openWatch(workspace));
    workspace.setPrivateEnabled(true);
    const QString privatePriceToken = QStringLiteral("731");
    const QString privateBadgeToken = QStringLiteral("SYNTHETIC_PRIVATE_DEV98");
    WatchlistRowState observation;
    observation.price = privatePriceToken;
    observation.change = "+1.23%";
    observation.evidence = "SYNTHETIC_PRIVATE_EVIDENCE_DEV98";
    observation.badges = {privateBadgeToken};
    observation.completedAtMs = 1;
    observation.privateData = true;
    lists.setRowState(Sol, observation);
    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    auto* survivor = identityRow(tree, Sol.key());
    QVERIFY(tree && survivor);
    QVERIFY(survivor->text(1).contains(privatePriceToken));
    QVERIFY(survivor->toolTip(1).contains(privatePriceToken));
    QVERIFY(survivor->toolTip(0).contains(privateBadgeToken));
    auto* btc = identityRow(tree, Btc.key());
    auto* eth = identityRow(tree, Eth.key());
    QVERIFY(btc && eth);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint(tree, btc, true));
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ShiftModifier, rowPoint(tree, eth, true));
    QCOMPARE(selectedKeys(tree), (QStringList{Btc.key(), Eth.key()}));
    const auto before = lists.userList(list);
    const auto chart = workspace.selectedChartIdentity();
    const auto requests = http.requests.size();
    QSignalSpy opened(&workspace, &ChartWorkspace::identitySelected);
    bool inspected = false, rowSurvived = false, modelCleared = false, publicStatePreserved = false;
    QString priceImmediately, priceTipImmediately, nameTipImmediately;
    QMetaObject::Connection changed;
    changed = connect(&lists, &WatchlistWorkspaceList::changed, &workspace, [&] {
        if (lists.userList(list).entries == before.entries) return;
        // Disconnect before clearPrivate emits changed again. The observations
        // below are captured before the outer keyboard mutation can resume;
        // an eventual refresh at the end of that mutation cannot satisfy them.
        disconnect(changed);
        inspected = true;
        const auto publicAtLock = lists.publicState();
        workspace.clearPrivate();
        publicStatePreserved = lists.publicState() == publicAtLock;
        const auto cleared = lists.rowState(Sol);
        modelCleared = cleared.price.isEmpty() && cleared.evidence.isEmpty() &&
            cleared.badges.isEmpty() && !cleared.privateData;
        auto* row = identityRow(tree, Sol.key());
        rowSurvived = row != nullptr;
        if (row) {
            priceImmediately = row->text(1);
            priceTipImmediately = row->toolTip(1);
            nameTipImmediately = row->toolTip(0);
        }
    });
    tree->setFocus();
    if (remove) QTest::keyClick(tree, Qt::Key_Delete);
    else QTest::keyClick(tree, Qt::Key_Down, Qt::AltModifier);
    disconnect(changed);
    QVERIFY2(inspected, "The real keyboard mutation must reach the synchronous changed observer");
    QVERIFY(modelCleared);
    QVERIFY(publicStatePreserved);
    QVERIFY2(rowSurvived, "Locking must retain the surviving public list member");
    const auto immediate = priceImmediately + "\n" + priceTipImmediately + "\n" + nameTipImmediately;
    QVERIFY2(!priceImmediately.contains(privatePriceToken) &&
             !priceTipImmediately.contains(privatePriceToken) &&
             !nameTipImmediately.contains(privateBadgeToken),
             qPrintable("clearPrivate returned with private row presentation still present:\n" + immediate));
    QCOMPARE(modelKeys(lists.userList(list), section),
             (remove ? QStringList{Sol.key()} : QStringList{Sol.key(), Btc.key(), Eth.key()}));
    QCOMPARE(workspace.selectedChartIdentity(), chart);
    QCOMPARE(opened.count(), 0);
    QCOMPARE(http.requests.size(), requests);
    QVERIFY(!vault.exists());
    QVERIFY(!vault.isUnlocked());
}

void ChartInstrumentPanelInteractionTest::rowContextMenuMovesAlertsAndUnstarsWithoutReads() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* sol = identityRow(tree, Sol.key());
    QVERIFY(sol);
    tree->setCurrentItem(sol);
    const auto requests = f.http.requests.size();
    QSignalSpy opened(&f.workspace, &ChartWorkspace::identitySelected);
    QSignalSpy alerts(&f.workspace, &ChartWorkspace::listAlertsRequested);
    const auto moved = contextAction(tree, rowPoint(tree, sol), "맨 위로");
    QVERIFY2(moved.opened && moved.invoked, "A row context menu must provide a working move-to-top action");
    QCOMPARE(moved.actions, (QStringList{"섹션으로 이동", "맨 위로", "알림 설정", "관심 해제"}));
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), (QStringList{Sol.key(), Btc.key(), Eth.key()}));
    const auto alert = contextAction(tree, rowPoint(tree, identityRow(tree, Sol.key())), "알림 설정");
    QVERIFY(alert.invoked);
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(alerts.first().first().toString(), f.list);
    const auto removed = contextAction(tree, rowPoint(tree, identityRow(tree, Sol.key())), "관심 해제");
    QVERIFY(removed.invoked);
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.first), (QStringList{Btc.key(), Eth.key()}));
    QCOMPARE(modelKeys(f.lists.userList(f.list), f.second), QStringList{Xrp.key()});
    QCOMPARE(opened.count(), 0);
    QCOMPARE(f.http.requests.size(), requests);
}

void ChartInstrumentPanelInteractionTest::foreignMimeDoesNotChangeTheList() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* tree = f.tree();
    auto* source = identityRow(tree, Btc.key());
    tree->setCurrentItem(source);
    const auto before = f.lists.publicState();
    QMimeData foreign;
    foreign.setText("coin:upbit:KRW-BTC");
    dragView(tree, rowPoint(tree, source, true), rowPoint(tree, sectionRow(tree, f.second)), {}, &foreign);
    // Unsupported payloads must preserve the list; the Qt acceptance flag is not specified.
    QCOMPARE(f.lists.publicState(), before);
}

void ChartInstrumentPanelInteractionTest::completedInteractionMenuHasEightActions() {
    InteractionFixture f;
    QVERIFY(f.seeded && openWatch(f.workspace));
    auto* button=f.workspace.findChild<QToolButton*>("chartWorkspaceListMenu");
    QVERIFY(button&&button->menu());
    auto* menu=button->menu();
    QStringList visibleActions;
    bool inspected=false;
    QTimer inspect;
    inspect.setInterval(1);
    connect(&inspect,&QTimer::timeout,menu,[&]{
        if(!menu->isVisible())return;
        for(const auto* action:menu->actions())if(action->isVisible()&&!action->isSeparator())visibleActions.append(action->text());
        inspected=true;inspect.stop();QTest::keyClick(menu,Qt::Key_Escape);
    });
    QTimer::singleShot(1000,menu,&QMenu::close);
    inspect.start();
    QTest::mouseClick(button,Qt::LeftButton);
    QTRY_VERIFY(inspected);
    QCOMPARE(visibleActions,(QStringList{"이름 바꾸기","색 깃발","섹션 추가","대표 종목으로 채우기…",
        "CSV 가져오기","CSV 내보내기","리스트 알림 설정","리스트 삭제"}));
    for(const auto* name:{"chartListRenameSection","chartListMoveSection","chartListRemoveIdentity",
        "chartListMoveUp","chartListMoveDown","chartListMoveListUp","chartListMoveListDown"})
        QVERIFY(f.workspace.findChild<QAction*>(name));
}

QTEST_MAIN(ChartInstrumentPanelInteractionTest)
#include "tst_chart_instrument_panel_interaction.moc"
