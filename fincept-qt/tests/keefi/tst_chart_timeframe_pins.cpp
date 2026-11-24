#include "keefi/ChartTimeframePins.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/KeefiFonts.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

using namespace keefi;

namespace {
const QList<qint64> All{60000, 300000, 900000, 3600000, 14400000, 86400000};
QByteArray state(const QList<qint64>& pinned) {
    QStringList values;
    for (const auto value : pinned) values.append(QString::number(value));
    return QStringLiteral(R"({"format":"keefi-chart-timeframes/1","pinned":[%1]})").arg(values.join(',')).toUtf8();
}
QPushButton* segment(ChartWorkspace& workspace, qint64 interval) {
    return workspace.findChild<QPushButton*>(QStringLiteral("chartWorkspaceTimeframe_%1").arg(interval));
}
QMenu* listMenu(ChartWorkspace& workspace) {
    auto* menu = workspace.findChild<QMenu*>("chartTimeframeListMenu");
    return menu && menu->isVisible() ? menu : nullptr;
}
QMenu* openFromMoreMenu(ChartWorkspace& workspace) {
    auto* action = workspace.findChild<QAction*>("chartTimeframeFavorites");
    if (!action) return nullptr;
    action->trigger();
    for (int i = 0; i < 100 && !listMenu(workspace); ++i) QTest::qWait(10);
    return listMenu(workspace);
}
QAbstractButton* pin(QMenu* menu, qint64 interval) {
    return menu ? menu->findChild<QAbstractButton*>(QStringLiteral("chartTimeframePin_%1").arg(interval)) : nullptr;
}
// Rows that draw the list highlight (QMenu paints none behind widget rows).
QStringList highlightedRows(QMenu* menu) {
    QStringList rows;
    for (auto* row : menu->findChildren<QWidget*>()) {
        const auto name = row->objectName();
        if (name.startsWith(QStringLiteral("chartTimeframeListRow_")) && row->property("active").toBool()) rows.append(name.section('_', 1));
    }
    return rows;
}
QStringList visibleSegments(ChartWorkspace& workspace) {
    QStringList shown;
    for (const auto interval : All)
        if (auto* button = segment(workspace, interval); button && !button->isHidden()) shown.append(QString::number(interval));
    return shown;
}
} // namespace

class ChartTimeframePinsTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); }

    void modelDefaultsToEveryIntervalAndWritesExactBytes() {
        ChartTimeframePins pins;
        QVERIFY(pins.allPinned());
        QCOMPARE(pins.pinned(), All);
        QCOMPARE(pins.publicState(), state(All));
        QVERIFY(pins.setPinned(60000, false));
        QCOMPARE(pins.publicState(), QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[300000,900000,3600000,14400000,86400000]})"));
        QVERIFY(!pins.allPinned());
        QVERIFY(!pins.setPinned(120000, false));
        QVERIFY(!pins.setPinned(120000, true));
        QVERIFY(pins.setPinned(60000, true));
        QCOMPARE(pins.pinned(), All); // Toolbar order is kept whatever order pins come back in.
        ChartTimeframePins read;
        QVERIFY(read.restorePublicState(R"({"pinned":[86400000,60000],"format":"keefi-chart-timeframes/1"})"));
        QCOMPARE(read.pinned(), (QList<qint64>{60000, 86400000}));
        QCOMPARE(read.publicState(), state({60000, 86400000}));
    }

    void modelRefusesAnythingButTheExactShape_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not-json") << QByteArray("pinned=60000");
        QTest::newRow("array-root") << QByteArray("[60000]");
        QTest::newRow("missing-format") << QByteArray(R"({"pinned":[60000]})");
        QTest::newRow("missing-pinned") << QByteArray(R"({"format":"keefi-chart-timeframes/1"})");
        QTest::newRow("extra-key") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[60000],"order":[]})");
        QTest::newRow("legacy-name") << QByteArray(R"({"format":"finix-chart-timeframes/1","pinned":[60000]})");
        QTest::newRow("next-version") << QByteArray(R"({"format":"keefi-chart-timeframes/2","pinned":[60000]})");
        QTest::newRow("format-not-string") << QByteArray(R"({"format":1,"pinned":[60000]})");
        QTest::newRow("pinned-object") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":{"60000":true}})");
        QTest::newRow("none-pinned") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[]})");
        QTest::newRow("duplicate") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[60000,60000]})");
        QTest::newRow("unsupported") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[120000]})");
        QTest::newRow("fraction") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[60000.5]})");
        QTest::newRow("string") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":["60000"]})");
        QTest::newRow("bool") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[true]})");
        QTest::newRow("null") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[null]})");
        QTest::newRow("nested") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[[60000]]})");
        QTest::newRow("seven") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[60000,300000,900000,3600000,14400000,86400000,60000]})");
        QTest::newRow("oversized") << state({60000}) + QByteArray(ChartTimeframePins::MaxStateBytes, ' ');
    }
    void modelRefusesAnythingButTheExactShape() {
        QFETCH(QByteArray, bytes);
        ChartTimeframePins pins;
        QVERIFY(pins.setPinned(86400000, false));
        const auto before = pins.publicState();
        QString reason;
        QVERIFY(!pins.restorePublicState(bytes, &reason));
        QVERIFY(!reason.isEmpty());
        QCOMPARE(pins.publicState(), before);
    }

    void modelKeepsTheLastPin() {
        ChartTimeframePins pins;
        for (int i = 1; i < All.size(); ++i) QVERIFY(pins.setPinned(All[i], false));
        QCOMPARE(pins.pinned(), QList<qint64>{60000});
        QVERIFY(!pins.setPinned(60000, false));
        QCOMPARE(pins.pinned(), QList<qint64>{60000});
        QCOMPARE(pins.publicState(), state({60000}));
    }

    void missingKeyKeepsTheToolbarAsItWas() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setChartSettings(&settings);
        workspace.resize(1440, 900);
        workspace.show();
        QCoreApplication::processEvents();
        QCOMPARE(visibleSegments(workspace).size(), 6);
        auto* list = workspace.findChild<QToolButton*>("chartWorkspaceTimeframeList");
        QVERIFY(list);
        QVERIFY(list->isHidden());
        QVERIFY(!workspace.property("timeframeStorageIssue").isValid());
        // The segment row is exactly the six buttons: the list button adds no width.
        auto* row = workspace.findChild<QWidget*>("chartWorkspaceTimeframeButtons");
        QVERIFY(row && row->isVisible());
        int buttons = 0;
        for (const auto interval : All) buttons += segment(workspace, interval)->sizeHint().width();
        QCOMPARE(row->sizeHint().width(), buttons + 5 * 2);
        workspace.setTimeframe(60000);
        QCoreApplication::processEvents();
        QVERIFY2(!settings.contains(ChartTimeframePins::SettingsKey), "Defaults are never written on their own");
    }

    void unpinPersistsExactBytesAndRestores() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        {
            ChartWorkspace workspace(nullptr, nullptr);
            workspace.indicatorPreferences()->markIntroSeen();
            workspace.setChartSettings(&settings);
            workspace.resize(1440, 900);
            workspace.show();
            auto* menu = openFromMoreMenu(workspace);
            QVERIFY(menu);
            auto* star = pin(menu, 60000);
            QVERIFY(star && star->isChecked());
            QTest::mouseClick(star, Qt::LeftButton);
            QVERIFY2(listMenu(workspace) == menu, "The star leaves the list open");
            QVERIFY(!star->isChecked());
            QTest::mouseClick(pin(menu, 14400000), Qt::LeftButton);
            QVERIFY(listMenu(workspace) == menu);
            QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), state({300000, 900000, 3600000, 86400000}));
            QCOMPARE(visibleSegments(workspace), (QStringList{"300000", "900000", "3600000", "86400000"}));
            QVERIFY(workspace.findChild<QToolButton*>("chartWorkspaceTimeframeList")->isVisibleTo(&workspace));
            menu->close();
        }
        settings.sync();
        QSettings reread(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        ChartWorkspace restored(nullptr, nullptr);
        restored.indicatorPreferences()->markIntroSeen();
        restored.setChartSettings(&reread);
        restored.resize(1440, 900);
        restored.show();
        QCoreApplication::processEvents();
        QCOMPARE(restored.timeframePins().pinned(), (QList<qint64>{300000, 900000, 3600000, 86400000}));
        QCOMPARE(visibleSegments(restored), (QStringList{"300000", "900000", "3600000", "86400000"}));
        QVERIFY(restored.findChild<QToolButton*>("chartWorkspaceTimeframeList")->isVisible());
    }

    void unreadableBytesStartFromDefaultsAndStayUntilAChange_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("broken-json") << QByteArray("{\"format\":\"keefi-chart-timeframes/1\",");
        QTest::newRow("empty-pins") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[]})");
        QTest::newRow("unknown-interval") << QByteArray(R"({"format":"keefi-chart-timeframes/1","pinned":[60000,120000]})");
    }
    void unreadableBytesStartFromDefaultsAndStayUntilAChange() {
        QFETCH(QByteArray, bytes);
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        settings.setValue(ChartTimeframePins::SettingsKey, bytes);
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setChartSettings(&settings);
        workspace.resize(1440, 900);
        workspace.show();
        QCoreApplication::processEvents();
        QVERIFY(workspace.timeframePins().allPinned());
        QCOMPARE(visibleSegments(workspace).size(), 6);
        QVERIFY(!workspace.property("timeframeStorageIssue").toString().isEmpty());
        workspace.setTimeframe(3600000);
        workspace.setTimeframe(900000);
        QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), bytes);
        auto* menu = openFromMoreMenu(workspace);
        QVERIFY(menu);
        QTest::mouseClick(pin(menu, 86400000), Qt::LeftButton);
        QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), state({60000, 300000, 900000, 3600000, 14400000}));
        QVERIFY(!workspace.property("timeframeStorageIssue").isValid());
        menu->close();
    }

    void theLastPinIsRefusedInTheList() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setChartSettings(&settings);
        workspace.resize(1440, 900);
        workspace.show();
        auto* menu = openFromMoreMenu(workspace);
        QVERIFY(menu);
        for (const auto interval : All)
            if (interval != 3600000) QTest::mouseClick(pin(menu, interval), Qt::LeftButton);
        QCOMPARE(workspace.timeframePins().pinned(), QList<qint64>{3600000});
        auto* last = pin(menu, 3600000);
        QVERIFY(last->isChecked());
        QVERIFY2(!last->isEnabled(), "The only pinned interval cannot be unpinned");
        QTest::mouseClick(last, Qt::LeftButton);
        QVERIFY(last->isChecked());
        QCOMPARE(workspace.timeframePins().pinned(), QList<qint64>{3600000});
        QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), state({3600000}));
        // The current interval (15분) still shows next to the one pin.
        QCOMPARE(visibleSegments(workspace), (QStringList{"900000", "3600000"}));
        menu->close();
    }

    void anUnpinnedIntervalIsChosenFromTheList() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        settings.setValue(ChartTimeframePins::SettingsKey, state({900000, 3600000, 86400000}));
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setChartSettings(&settings);
        workspace.resize(1440, 900);
        workspace.show();
        QCoreApplication::processEvents();
        auto* list = workspace.findChild<QToolButton*>("chartWorkspaceTimeframeList");
        QVERIFY(list && list->isVisible());
        QTest::mouseClick(list, Qt::LeftButton);
        QTRY_VERIFY(listMenu(workspace));
        auto* menu = listMenu(workspace);
        auto* minute = menu->findChild<QPushButton*>("chartWorkspaceTimeframe_60000");
        QVERIFY(!minute); // List rows are their own controls, not the toolbar segments.
        auto* choose = menu->findChild<QPushButton*>("chartTimeframeListSelect_60000");
        QVERIFY(choose);
        QTest::mouseClick(choose, Qt::LeftButton);
        QCOMPARE(workspace.timeframeMs(), 60000);
        QTRY_VERIFY(!listMenu(workspace));
        auto* shown = segment(workspace, 60000);
        QVERIFY(shown->isVisible());
        QVERIFY(shown->isChecked());
        QCOMPARE(workspace.timeframePins().pinned(), (QList<qint64>{900000, 3600000, 86400000}));
        QTest::mouseClick(segment(workspace, 3600000), Qt::LeftButton);
        QCOMPARE(workspace.timeframeMs(), 3600000);
        QVERIFY(segment(workspace, 60000)->isHidden());
        QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), state({900000, 3600000, 86400000}));
    }

    void toolbarStaysInsideAtCommonWidths_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<QByteArray>("bytes");
        for (const int width : {960, 1280, 1440}) {
            QTest::newRow(qPrintable(QStringLiteral("%1-default").arg(width))) << width << QByteArray();
            QTest::newRow(qPrintable(QStringLiteral("%1-three-pinned").arg(width))) << width << state({300000, 900000, 3600000});
            QTest::newRow(qPrintable(QStringLiteral("%1-one-pinned").arg(width))) << width << state({86400000});
        }
    }
    void toolbarStaysInsideAtCommonWidths() {
        QFETCH(int, width);
        QFETCH(QByteArray, bytes);
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        if (!bytes.isEmpty()) settings.setValue(ChartTimeframePins::SettingsKey, bytes);
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setChartSettings(&settings);
        workspace.resize(width, 900);
        workspace.show();
        QTest::qWait(20);
        auto* toolbar = workspace.findChild<QWidget*>("chartToolbar");
        auto* scroll = workspace.findChild<QScrollArea*>("chartWorkspaceScroll");
        QVERIFY(toolbar && scroll);
        QVERIFY(toolbar->height() <= 96);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QList<QRect> groups;
        for (const char* name : {"chartWorkspaceTitleHost", "chartToolbarFrequent", "chartToolbarSecondary"}) {
            auto* group = workspace.findChild<QWidget*>(name);
            QVERIFY(group);
            const QRect area(group->mapTo(toolbar, QPoint()), group->size());
            QVERIFY2(toolbar->rect().contains(area), name);
            for (const auto& other : groups) QVERIFY2(!other.intersects(area), name);
            groups.append(area);
        }
        auto* frequent = workspace.findChild<QWidget*>("chartToolbarFrequent");
        QList<QWidget*> controls;
        for (const auto interval : All) controls.append(segment(workspace, interval));
        controls.append(workspace.findChild<QToolButton*>("chartWorkspaceTimeframeList"));
        controls.append(workspace.findChild<QWidget*>("chartWorkspaceTimeframe"));
        for (auto* control : controls) {
            QVERIFY(control);
            if (!control->isVisible()) continue;
            const QRect area(control->mapTo(frequent, QPoint()), control->size());
            QVERIFY2(frequent->rect().contains(area), qPrintable(control->objectName()));
        }
        const bool folded = toolbar->property("timeframeFolded").toBool();
        QCOMPARE(workspace.findChild<QToolButton*>("chartWorkspaceTimeframeList")->isVisible(), !folded && !bytes.isEmpty());
    }

    // Keyboard only: Space on ▾ opens the list on the interval on screen, ↑↓
    // move the highlighted row, Space toggles that row's ★ with the list still
    // open, and Enter chooses the row.
    void keyboardChoosesAndPinsFromTheList() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        settings.setValue(ChartTimeframePins::SettingsKey, state({900000, 3600000}));
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* workspace = new ChartWorkspace(nullptr, nullptr, &host);
        workspace->indicatorPreferences()->markIntroSeen();
        workspace->setChartSettings(&settings);
        layout->addWidget(workspace);
        host.resize(1440, 900);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        host.activateWindow();
        auto* list = workspace->findChild<QToolButton*>("chartWorkspaceTimeframeList");
        QVERIFY(list && list->isVisible());
        list->setFocus(Qt::TabFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(list));
        QTest::keyClick(list, Qt::Key_Space);
        QTRY_VERIFY(listMenu(*workspace));
        auto* menu = listMenu(*workspace);
        const auto highlighted = [menu] { return menu->activeAction() ? menu->activeAction()->data().toLongLong() : 0; };
        // Keys go to the open popup, as the application delivers them.
        const auto press = [menu](Qt::Key key) {
            QWidget* popup = QApplication::activePopupWidget();
            QTest::keyClick(popup ? popup : menu, key);
        };
        QCOMPARE(highlighted(), 900000);
        QCOMPARE(highlightedRows(menu), QStringList{"900000"});
        press(Qt::Key_Down);
        QCOMPARE(highlighted(), 3600000);
        QCOMPARE(highlightedRows(menu), QStringList{"3600000"});
        press(Qt::Key_Space);
        QCOMPARE(listMenu(*workspace), menu);
        QCOMPARE(workspace->timeframePins().pinned(), QList<qint64>{900000});
        QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), state({900000}));
        press(Qt::Key_Space);
        QCOMPARE(workspace->timeframePins().pinned(), (QList<qint64>{900000, 3600000}));
        press(Qt::Key_Up);
        press(Qt::Key_Up);
        QCOMPARE(highlighted(), 300000);
        QCOMPARE(highlightedRows(menu), QStringList{"300000"});
        press(Qt::Key_Space);
        QCOMPARE(workspace->timeframePins().pinned(), (QList<qint64>{300000, 900000, 3600000}));
        QCOMPARE(workspace->timeframeMs(), 900000);
        press(Qt::Key_Return);
        QCOMPARE(workspace->timeframeMs(), 300000);
        QTRY_VERIFY(!listMenu(*workspace));
        QCOMPARE(settings.value(ChartTimeframePins::SettingsKey).toByteArray(), state({300000, 900000, 3600000}));
    }

    // Pointer and keys share one highlighted row. A mouse click on a ★ takes no
    // keyboard focus, so the next Space acts on the highlighted row, not on the
    // row clicked before.
    void pointerAndKeysShareTheHighlightedRow() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        settings.setValue(ChartTimeframePins::SettingsKey, state({60000, 900000, 3600000}));
        QWidget host;
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* workspace = new ChartWorkspace(nullptr, nullptr, &host);
        workspace->indicatorPreferences()->markIntroSeen();
        workspace->setChartSettings(&settings);
        layout->addWidget(workspace);
        host.resize(1440, 900);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        host.activateWindow();
        auto* list = workspace->findChild<QToolButton*>("chartWorkspaceTimeframeList");
        QVERIFY(list && list->isVisible());
        QTest::mouseClick(list, Qt::LeftButton);
        QTRY_VERIFY(listMenu(*workspace));
        auto* menu = listMenu(*workspace);
        QCOMPARE(highlightedRows(menu), QStringList{"900000"});
        auto* hourSelect = menu->findChild<QPushButton*>("chartTimeframeListSelect_3600000");
        QVERIFY(hourSelect);
        QTest::mouseMove(hourSelect, hourSelect->rect().center());
        QTRY_COMPARE(highlightedRows(menu), QStringList{"3600000"});
        QCOMPARE(menu->activeAction()->data().toLongLong(), 3600000);
        auto* minutePin = menu->findChild<QAbstractButton*>("chartTimeframePin_60000");
        QVERIFY(minutePin && minutePin->isChecked());
        QTest::mouseMove(minutePin, minutePin->rect().center());
        QTest::mouseClick(minutePin, Qt::LeftButton);
        QCOMPARE(workspace->timeframePins().pinned(), (QList<qint64>{900000, 3600000}));
        QVERIFY2(!menu->isAncestorOf(QApplication::focusWidget()), "A click in the list takes no keyboard focus");
        QTRY_COMPARE(highlightedRows(menu), QStringList{"60000"});
        QWidget* popup = QApplication::activePopupWidget();
        QTest::keyClick(popup ? popup : menu, Qt::Key_Down);
        QCOMPARE(highlightedRows(menu), QStringList{"300000"});
        QTest::keyClick(popup ? popup : menu, Qt::Key_Space);
        QCOMPARE(workspace->timeframePins().pinned(), (QList<qint64>{300000, 900000, 3600000}));
        QVERIFY(!minutePin->isChecked());
        menu->close();
    }

    // Every toolbar width near the fold thresholds: the ▾ button counts in the
    // width that decides one or two rows, so no segment is squeezed or cut.
    void listButtonCountsInEveryFoldDecision() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("synthetic-pins.ini"), QSettings::IniFormat);
        settings.setValue(ChartTimeframePins::SettingsKey, state({60000, 300000, 900000, 3600000, 14400000}));
        ChartWorkspace workspace(nullptr, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setChartSettings(&settings);
        workspace.resize(1800, 900);
        workspace.show();
        QTest::qWait(20);
        auto* toolbar = workspace.findChild<QWidget*>("chartToolbar");
        auto* frequent = workspace.findChild<QWidget*>("chartToolbarFrequent");
        auto* list = workspace.findChild<QToolButton*>("chartWorkspaceTimeframeList");
        QVERIFY(toolbar && frequent && list);
        int checked = 0;
        for (int width = 700; width <= 1700; width += 2) {
            toolbar->resize(width, toolbar->height());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            if (!list->isVisible()) continue;
            ++checked;
            const QRect area(list->mapTo(frequent, QPoint()), list->size());
            QVERIFY2(frequent->rect().contains(area), qPrintable(QStringLiteral("toolbar %1").arg(width)));
            for (const auto interval : All) {
                auto* button = segment(workspace, interval);
                if (!button->isVisible()) continue;
                QVERIFY2(button->width() >= button->sizeHint().width(),
                         qPrintable(QStringLiteral("toolbar %1, %2 is %3 of %4").arg(width).arg(interval).arg(button->width()).arg(button->sizeHint().width())));
            }
        }
        QVERIFY(checked > 100);
    }
};

QTEST_MAIN(ChartTimeframePinsTest)
#include "tst_chart_timeframe_pins.moc"
