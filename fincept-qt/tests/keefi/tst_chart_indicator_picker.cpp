#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MarketCandlesView.h"
#include <QApplication>
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QColorDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QHeaderView>
#include <QHelpEvent>
#include <QImage>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QSet>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyleOptionComboBox>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <private/qguiapplication_p.h>
#include <qpa/qplatformaccessibility.h>
#include <qpa/qplatformintegration.h>

using namespace keefi;

namespace {
QStringList visibleStudies(QTreeWidget* tree) {
    QStringList ids;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const auto id = (*it)->data(0, Qt::UserRole).toString();
        bool visible = !id.isEmpty();
        for (auto* row = *it; row; row = row->parent()) visible &= !row->isHidden();
        if (visible) ids.append(id);
    }
    ids.sort();
    return ids;
}

QList<QAccessibleInterface*> accessibleNamedDescendants(QAccessibleInterface* root, const QString& name) {
    QList<QAccessibleInterface*> matches;
    QList<QAccessibleInterface*> pending{root};
    QSet<QAccessible::Id> visited;
    while (!pending.isEmpty() && visited.size() < 4096) {
        auto* current = pending.takeLast();
        if (!current || !current->isValid()) continue;
        const auto id = QAccessible::uniqueId(current);
        if (visited.contains(id)) continue;
        visited.insert(id);
        if (current->text(QAccessible::Name) == name) matches.append(current);
        for (int i = 0; i < current->childCount(); ++i) pending.append(current->child(i));
    }
    return pending.isEmpty() ? matches : QList<QAccessibleInterface*>{};
}
}

class ChartIndicatorPickerTest final : public QObject {
    Q_OBJECT
  private slots:
    void eachSupportedIndicatorCanBeAppliedHiddenShownAndRemoved_data() {
        QTest::addColumn<QString>("market");
        QTest::newRow("coin")<<QStringLiteral("coin");
        QTest::newRow("stock")<<QStringLiteral("stock");
    }
    void eachSupportedIndicatorCanBeAppliedHiddenShownAndRemoved() {
        QFETCH(QString,market);
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        const auto supported=preferences.supportedIds(market);
        for(const auto& id:supported)preferences.setEnabled(id,false);
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection(market=="coin"?"coin:binance:BTCUSDT":"stock:KR:005930",60000);
        view.resize(800,680);view.show();
        bool inspected=false;
        QTimer::singleShot(0,&view,[&] {
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);
            const auto close=qScopeGuard([&] { dialog->reject(); });
            auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");QVERIFY(tree);
            auto ids=supported;ids.sort();
            QCOMPARE(visibleStudies(tree),ids);
            for(QTreeWidgetItemIterator it(tree);*it;++it){
                auto* item=*it;const auto id=item->data(0,Qt::UserRole).toString();if(id.isEmpty())continue;
                tree->scrollToItem(item);tree->setCurrentItem(item);tree->setFocus();
                auto* eye=dialog->findChild<QPushButton*>("chartStudyVisible_"+id);QVERIFY(eye);
                QTest::keyClick(tree,Qt::Key_Space);
                QVERIFY2(preferences.enabled(id)&&preferences.visible(id),qPrintable(id));
                QVERIFY2(item->text(5)!="Not calculated",qPrintable(id));
                QTest::mouseClick(eye,Qt::LeftButton);
                QVERIFY2(preferences.enabled(id)&&!preferences.visible(id)&&!eye->isChecked(),qPrintable(id));
                QTest::mouseClick(eye,Qt::LeftButton);
                QVERIFY2(preferences.enabled(id)&&preferences.visible(id)&&eye->isChecked(),qPrintable(id));
                tree->setFocus();QTest::keyClick(tree,Qt::Key_Space);
                QVERIFY2(!preferences.enabled(id)&&item->text(5)=="Not calculated",qPrintable(id));
            }
            inspected=true;
        });
        auto* picker=view.findChild<QPushButton*>("chartStudyPicker");QVERIFY(picker);
        QTest::mouseClick(picker,Qt::LeftButton);QVERIFY(inspected);
    }
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("KeefiSyntheticIndicatorPicker");
        QCoreApplication::setApplicationName("KeefiSyntheticIndicatorPicker");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QVERIFY(KeefiFonts::registerBundledFonts());
        reviewDirectory_=qEnvironmentVariable("KEEFI_UI_REVIEW_DIR");
        if(!reviewDirectory_.isEmpty()){
            const QFileInfo destination(reviewDirectory_),parent(destination.absolutePath());
            QVERIFY2(destination.isAbsolute()&&!destination.exists()&&!destination.isSymLink(),"KEEFI_UI_REVIEW_DIR must be a new absolute output directory");
            QVERIFY(parent.isDir());QCOMPARE(parent.canonicalFilePath(),parent.absoluteFilePath());
            QVERIFY(QDir(parent.absoluteFilePath()).mkdir(destination.fileName()));
        }
    }

    void appliedIndicatorsStartCollapsedWithoutChangingPreferences() {
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        for (const auto& id : preferences.supportedIds("coin")) preferences.setEnabled(id, false);
        preferences.setEnabled("cloud", true);
        preferences.setEnabled("pullback", true);
        preferences.setVisible("pullback", false);
        const auto saved = preferences.publicState();

        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:binance:BTCUSDT", 3600000);
        view.resize(1080, 780);
        view.show();
        QCoreApplication::processEvents();

        auto* applied = view.findChild<QPushButton*>("chartStudyAppliedToggle");
        QVERIFY2(applied, "Applied studies need a compact, collapsed entry point on the chart");
        QVERIFY(applied->isVisible());
        QVERIFY(applied->text().contains(QStringLiteral("Enabled 2")));
        QVERIFY(!applied->isChecked());
        auto* cloud = view.findChild<QWidget*>("chartStudyChip_cloud");
        auto* pullback = view.findChild<QWidget*>("chartStudyChip_pullback");
        auto* summary = view.findChild<QWidget*>("chartCoreStudySummary");
        QVERIFY(cloud); QVERIFY(pullback); QVERIFY(summary);
        QVERIFY(!cloud->isVisible()); QVERIFY(!pullback->isVisible());
        QVERIFY(!summary->isVisible());

        QTest::mouseClick(applied, Qt::LeftButton);
        QVERIFY(cloud->isVisible()); QVERIFY(pullback->isVisible());
        QVERIFY(summary->isVisible());
        QCOMPARE(preferences.publicState(), saved);
        QTest::mouseClick(applied, Qt::LeftButton);
        QVERIFY(!cloud->isVisible()); QVERIFY(!pullback->isVisible());
        QVERIFY(!summary->isVisible());
        QCOMPARE(preferences.publicState(), saved);
    }

    void conceptSearchAndFavoritesRoundTripWithoutEnablingAnything() {
        ChartStudyPreferences preferences;preferences.markIntroSeen();
        for(const auto& id:preferences.supportedIds("coin"))preferences.setEnabled(id,false);
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-BTC",900000);view.resize(1080,780);view.show();
        auto* picker=view.findChild<QPushButton*>("chartStudyPicker");QVERIFY(picker);bool checked=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);const auto close=qScopeGuard([&]{dialog->reject();});
            auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            auto* scope=dialog->findChild<QTabBar*>("chartStudyScope");QVERIFY(search&&tree&&scope);
            search->setText("변동성");QVERIFY(visibleStudies(tree).contains("bb"));search->setText("Bollinger");QCOMPARE(visibleStudies(tree),QStringList{"bb"});
            auto* favorite=dialog->findChild<QPushButton*>("chartStudyFavorite_bb");QVERIFY(favorite);favorite->setChecked(true);
            QVERIFY(!preferences.enabled("bb"));QCOMPARE(scope->count(),3);search->clear();scope->setCurrentIndex(2);QCOMPARE(visibleStudies(tree),QStringList{"bb"});
            checked=true;
        });picker->click();QVERIFY(checked);
        ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(preferences.publicState()));view.setIndicatorPreferences(&restored);checked=false;
        QTimer::singleShot(0,&view,[&]{auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);const auto close=qScopeGuard([&]{dialog->reject();});
            auto* scope=dialog->findChild<QTabBar*>("chartStudyScope");auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");QVERIFY(scope&&tree);scope->setCurrentIndex(2);QCOMPARE(visibleStudies(tree),QStringList{"bb"});QVERIFY(!restored.enabled("bb"));checked=true;});
        picker->click();QVERIFY(checked);
    }

    void initialsQuestionWordsAndReasonsStayDisplayOnly_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void initialsQuestionWordsAndReasonsStayDisplayOnly() {
        QFETCH(bool, dark);
        const auto previousTheme = KeefiTheme::instance()->mode();
        const auto restoreTheme = qScopeGuard([&] { KeefiTheme::instance()->setMode(previousTheme); });
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        for (const auto& id : preferences.supportedIds("coin")) preferences.setEnabled(id, false);
        const auto saved = preferences.publicState();
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:upbit:KRW-SYN", 3600000);
        view.resize(1080, 780);
        view.show();
        auto* picker = view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        const auto renderDir = qEnvironmentVariable("KEEFI_DEV92_RENDER_DIR");
        const auto capture = [&](QWidget* widget, const QString& name) {
            // Synthetic empty chart fixture only; offscreen images are not native-window evidence.
            QCoreApplication::processEvents();
            return renderDir.isEmpty() || (QDir().mkpath(renderDir) && widget->grab().save(renderDir + "/" + name + ".png"));
        };
        bool inspected = false;
        QTimer::singleShot(0, &view, [&] {
            auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto close = qScopeGuard([&] { dialog->reject(); });
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            QVERIFY(search && tree);
            QVERIFY(!dialog->findChild<QPlainTextEdit*>("chartStudyDetails"));
            QCOMPARE(search->accessibleName(), QStringLiteral("Search indicators and concepts"));
            search->setText(QStringLiteral("ㅂㄹㅈ"));
            QCOMPARE(visibleStudies(tree), QStringList{"bb"});
            QCOMPARE(search->placeholderText(), QStringLiteral("Search indicators, concepts or Korean initials"));
            QVERIFY(tree->currentItem());
            QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("bb"));
            QVERIFY(tree->currentItem()->data(0, Qt::UserRole + 1).toString().contains(QStringLiteral("초성")));
            QVERIFY(tree->currentItem()->data(0, Qt::AccessibleTextRole).toString().contains(QStringLiteral("찾은 이유")));
            QVERIFY(tree->currentItem()->toolTip(0).contains(QStringLiteral("찾은 이유")));
            QCOMPARE(tree->currentItem()->text(0), QStringLiteral("Bollinger Bands"));
            auto* bbInfo=dialog->findChild<QPushButton*>("chartStudyInfo_bb");QVERIFY(bbInfo);
            QCOMPARE(bbInfo->toolTip(),chartStudyHelp("bb")->brief);
            QCOMPARE(preferences.publicState(), saved);
            QVERIFY(capture(dialog, QString("initials-%1").arg(dark ? "dark" : "light")));
            if (!dark) {
                dialog->resize(720, 650);
                QVERIFY(capture(dialog, "initials-720-light"));
                QCOMPARE(dialog->width(), 720);
                dialog->resize(820, 650);
            }
            search->setText(QStringLiteral("많이 올랐나"));
            QCOMPARE(visibleStudies(tree), QStringList{"rsi"});
            QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("rsi"));
            QVERIFY(tree->currentItem()->data(0,Qt::UserRole+1).toString().contains(QStringLiteral("많이 올랐")));
            if (!dark) QVERIFY(capture(dialog, "question-light"));
            auto* help = dialog->findChild<QPushButton*>("chartStudyInfo_rsi");
            QVERIFY(help);
            bool explained = false;
            QTimer::singleShot(0, dialog, [&] {
                auto* helpDialog = dialog->findChild<QDialog*>("chartStudyHelpDialog");
                QVERIFY(helpDialog);
                const auto closeHelp = qScopeGuard([&] { helpDialog->reject(); });
                auto* expand = helpDialog->findChild<QPushButton*>("chartStudyHelpDetails");
                auto* calculation = helpDialog->findChild<QLabel*>("chartStudyHelp_calculation");
                QVERIFY(expand && calculation);
                expand->click();
                QVERIFY(calculation->isVisible());
                QVERIFY(calculation->text().contains("14"));
                QVERIFY(capture(helpDialog, QString("rsi-help-expanded-%1").arg(dark ? "dark" : "light")));
                explained = true;
            });
            help->click();
            QVERIFY(explained);
            search->setText(QStringLiteral("상대 강도"));
            QCOMPARE(visibleStudies(tree), (QStringList{"relative_strength", "rsi"}));
            QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("rsi"));
            search->setText(QStringLiteral("거래량"));
            QVERIFY(visibleStudies(tree).contains("low_volume"));
            QVERIFY(visibleStudies(tree).contains("volume"));
            QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("volume"));
            QVERIFY(tree->currentItem()->data(0, Qt::UserRole + 1).toString().contains(QStringLiteral("다른 이름")));
            search->clear();
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                QVERIFY((*it)->data(0, Qt::UserRole + 1).toString().isEmpty());
                QVERIFY(!(*it)->data(0, Qt::AccessibleTextRole).toString().contains(QStringLiteral("찾은 이유")));
                QVERIFY(!(*it)->toolTip(0).contains(QStringLiteral("찾은 이유")));
            }
            QVERIFY(!dialog->findChild<QPlainTextEdit*>("chartStudyDetails"));
            QCOMPARE(preferences.publicState(), saved);
            inspected = true;
        });
        picker->click();
        QVERIFY(inspected);
        QCOMPARE(preferences.publicState(), saved);
        view.setSelection("stock:KR:SYN", 86400000);
        inspected = false;
        QTimer::singleShot(0, &view, [&] {
            auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto close = qScopeGuard([&] { dialog->reject(); });
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            auto* empty = dialog->findChild<QLabel*>("chartStudyEmpty");
            QVERIFY(search && tree && empty);
            search->setText(QStringLiteral("펀딩"));
            QVERIFY(visibleStudies(tree).isEmpty());
            QVERIFY(empty->isVisible());
            QCOMPARE(empty->text(), QStringLiteral("No indicators match these filters."));
            QCOMPARE(preferences.publicState(), saved);
            inspected = true;
        });
        picker->click();
        QVERIFY(inspected);
    }

    void searchSelectionSurvivesToggleAndLiveRefresh() {
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        for (const auto& id : preferences.supportedIds("coin")) preferences.setEnabled(id, false);
        const auto initial = preferences.publicState();
        ChartStudyPreferences expected;
        QVERIFY(expected.restorePublicState(initial));
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:upbit:KRW-SYN", 3600000);
        view.resize(1080, 780);
        view.show();
        auto* picker = view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        bool inspected = false;
        QTimer::singleShot(0, &view, [&] {
            auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto close = qScopeGuard([&] { dialog->reject(); });
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            auto* favorite = dialog->findChild<QPushButton*>("chartStudyFavorite_macd");
            QVERIFY(search && tree && favorite);
            const auto current = [&] {
                return tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString() : QString{};
            };
            search->setText(QStringLiteral("추세"));
            QCOMPARE(current(), QString("cloud"));
            tree->setFocus();
            QTest::keyClick(tree, Qt::Key_Down);
            QCOMPARE(current(), QString("ma"));
            QTest::keyClick(tree, Qt::Key_Space);
            QVERIFY(preferences.enabled("ma"));
            QVERIFY(!preferences.enabled("cloud"));
            QCOMPARE(current(), QString("ma"));
            expected.setEnabled("ma", true);
            QCOMPARE(preferences.publicState(), expected.publicState());
            QTest::keyClick(tree, Qt::Key_Space);
            QVERIFY(!preferences.enabled("ma"));
            QVERIFY(!preferences.enabled("cloud"));
            QCOMPARE(current(), QString("ma"));
            expected.setEnabled("ma", false);
            QCOMPARE(preferences.publicState(), expected.publicState());

            // The corrected toggle remains on ma; return to cloud before the
            // reported two-Down navigation to macd.
            QTest::keyClick(tree, Qt::Key_Up);
            QCOMPARE(current(), QString("cloud"));
            QTest::keyClick(tree, Qt::Key_Down);
            QTest::keyClick(tree, Qt::Key_Down);
            QCOMPARE(current(), QString("macd"));
            emit view.studiesChanged();
            QCOMPARE(current(), QString("macd"));
            emit view.coinFlowStudiesChanged();
            QCOMPARE(current(), QString("macd"));
            QCOMPARE(preferences.publicState(), expected.publicState());
            favorite->setChecked(true);
            QCOMPARE(current(), QString("macd"));
            QVERIFY(preferences.favorite("macd"));
            favorite->setChecked(false);
            QCOMPARE(current(), QString("macd"));
            QCOMPARE(preferences.publicState(), expected.publicState());

            QTreeWidgetItem* second = nullptr;
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toString() == "ma") second = *it;
            QVERIFY(second && !second->isHidden());
            tree->scrollToItem(second);
            const auto target = tree->visualItemRect(second);
            QVERIFY(!target.isEmpty());
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(tree->columnViewportPosition(0)+tree->columnWidth(0)/2,target.center().y()));
            QVERIFY(preferences.enabled("ma"));
            QVERIFY(!preferences.enabled("cloud"));
            QCOMPARE(current(), QString("ma"));
            expected.setEnabled("ma", true);
            QCOMPARE(preferences.publicState(), expected.publicState());

            search->setText(QStringLiteral("흔들림"));
            QCOMPARE(visibleStudies(tree), (QStringList{"bb", "squeeze"}));
            QCOMPARE(current(), QString("bb"));
            QCOMPARE(preferences.publicState(), expected.publicState());
            inspected = true;
        });
        picker->click();
        QVERIFY(inspected);
        QCOMPARE(preferences.publicState(), expected.publicState());
    }

    void accessibilityCellsSurviveRefreshAndSearch_data() {
        QTest::addColumn<QString>("change");
        QTest::newRow("live-all") << QString("live-all");
        QTest::newRow("live-filtered") << QString("live-filtered");
        QTest::newRow("toggle-filtered") << QString("toggle-filtered");
        QTest::newRow("search-changed") << QString("search-changed");
        QTest::newRow("search-narrowed") << QString("search-narrowed");
        QTest::newRow("category-changed") << QString("category-changed");
    }

    void accessibilityCellsSurviveRefreshAndSearch() {
        QFETCH(QString, change);
        auto* platform = QGuiApplicationPrivate::platformIntegration()->accessibility();
        QVERIFY(platform);
        const bool previouslyActive = platform->isActive();
        platform->setActive(true);
        const auto previousHandler = QAccessible::installUpdateHandler([](QAccessibleEvent*) {});
        const auto restoreAccessibility = qScopeGuard([&] {
            QAccessible::installUpdateHandler(previousHandler);
            platform->setActive(previouslyActive);
        });
        QVERIFY(QAccessible::isActive());
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        for (const auto& id : preferences.supportedIds("coin")) preferences.setEnabled(id, false);
        const auto initial = preferences.publicState();
        ChartStudyPreferences expected;
        QVERIFY(expected.restorePublicState(initial));
        if (change == "toggle-filtered") expected.setEnabled("ma", true);
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:upbit:KRW-SYN", 3600000);
        view.resize(1080, 780);
        view.show();
        auto* picker = view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        bool inspected = false;
        QTimer::singleShot(0, &view, [&] {
            auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto close = qScopeGuard([&] { dialog->reject(); });
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* category = dialog->findChild<QComboBox*>("chartStudyCategory");
            QVERIFY(tree && search && category);
            if (change != "live-all") search->setText(QStringLiteral("추세"));
            QCoreApplication::processEvents();
            auto* accessible = QAccessible::queryAccessibleInterface(tree);
            QVERIFY(accessible && accessible->tableInterface());
            auto* table = accessible->tableInterface();
            QVERIFY(table->rowCount() > 1);
            // Seed the cell cache as a previous native AX snapshot would.
            QVERIFY(table->cellAt(1, 0));
            if (change == "search-changed") search->setText(QStringLiteral("흔들림"));
            else if (change == "search-narrowed") search->setText(QStringLiteral("구름"));
            else if (change == "category-changed") category->setCurrentIndex(category->findData("trend"));
            else if (change == "toggle-filtered") preferences.setEnabled("ma", true);
            else emit view.studiesChanged();
            // Native AX can arrive before the next event-loop/layout pass.
            // Obtain a fresh interface and read state/rect without processing
            // events or asking the view for geometry first (either masks it).
            for (int row = 0; row < table->rowCount(); ++row) {
                for (int column = 0; column < table->columnCount(); ++column) {
                    auto* cell = table->cellAt(row, column);
                    QVERIFY(cell && cell->isValid());
                    const auto id = QAccessible::uniqueId(cell);
                    (void)cell->state();
                    QCOMPARE(QAccessible::accessibleInterface(id), cell);
                    (void)cell->rect();
                    QCOMPARE(QAccessible::accessibleInterface(id), cell);
                }
            }
            QCOMPARE(preferences.publicState(), expected.publicState());
            inspected = true;
        });
        picker->click();
        QVERIFY(inspected);
    }

    void pickerSearchCategoriesAndAppliedListShareLiveSettings() {
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        for (const auto& id : preferences.supportedIds("coin")) preferences.setEnabled(id, false);
        preferences.setEnabled("cloud", true);
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:binance:BTCUSDT", 3600000);
        view.resize(1080, 780);
        view.show();
        auto* picker = view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        bool inspected = false;
        QTimer::singleShot(0, &view, [&] {
            auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto close = qScopeGuard([&] { dialog->reject(); });
            auto* scope = dialog->findChild<QTabBar*>("chartStudyScope");
            auto* category = dialog->findChild<QComboBox*>("chartStudyCategory");
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            auto* visible = dialog->findChild<QPushButton*>("chartStudyVisible_rsi");
            QVERIFY2(scope && category && search && tree && visible,
                     "The picker needs all/enabled filtering, categories, and independent row controls");
            QVERIFY(!dialog->findChild<QPlainTextEdit*>("chartStudyDetails"));
            QCOMPARE(scope->tabText(0), QStringLiteral("All"));
            QCOMPARE(scope->tabText(1), QStringLiteral("Enabled"));
            QTRY_VERIFY(search->hasFocus());
            QTest::keyClicks(search, "RSI");
            QCOMPARE(visibleStudies(tree), QStringList{"rsi"});
            const auto trend = category->findData("trend"), momentum = category->findData("momentum");
            QVERIFY(trend >= 0); QVERIFY(momentum >= 0);
            category->setCurrentIndex(trend);
            QVERIFY(visibleStudies(tree).isEmpty());
            category->setCurrentIndex(momentum);
            QCOMPARE(visibleStudies(tree), QStringList{"rsi"});
            scope->setCurrentIndex(1);
            QVERIFY(visibleStudies(tree).isEmpty());
            scope->setCurrentIndex(0);
            search->setFocus();
            QTest::keyClick(search, Qt::Key_Down);
            QTRY_VERIFY(tree->hasFocus());
            QTest::keyClick(tree, Qt::Key_Space);
            QVERIFY(preferences.enabled("rsi"));
            QCOMPARE(tree->currentItem()->checkState(0),Qt::Checked); QVERIFY(visible->isChecked());
            QCOMPARE(tree->currentItem()->text(0),QStringLiteral("RSI"));
            QCOMPARE(tree->currentItem()->text(2),QStringLiteral("✓ On"));
            QCOMPARE(tree->currentItem()->text(5),QStringLiteral("Needs data"));
            scope->setCurrentIndex(1);
            QCOMPARE(visibleStudies(tree), QStringList{"rsi"});
            QTest::mouseClick(visible, Qt::LeftButton);
            QVERIFY(preferences.enabled("rsi")); QVERIFY(!preferences.visible("rsi"));
            QCOMPARE(visibleStudies(tree), QStringList{"rsi"}); // Hidden is still applied.
            preferences.setVisible("rsi", true);
            QVERIFY(visible->isChecked());
            inspected = true;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        picker->click();
        QVERIFY(inspected);
        QTRY_VERIFY(picker->hasFocus());
        QVERIFY(preferences.enabled("cloud")); QVERIFY(preferences.enabled("rsi"));
        ChartStudyPreferences restored;
        QVERIFY(restored.restorePublicState(preferences.publicState()));
        QVERIFY(restored.enabled("cloud")); QVERIFY(restored.enabled("rsi"));
        QVERIFY(restored.visible("rsi"));
    }


    void exactEnglishNamesWinEqualRankSubstrings() {
        ChartStudyPreferences preferences;preferences.markIntroSeen();for(const auto& id:preferences.supportedIds("coin"))preferences.setEnabled(id,false);
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",900000);view.resize(900,700);view.show();
        bool inspected=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);const auto close=qScopeGuard([&]{dialog->reject();});
            auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");QVERIFY(search&&tree);
            for(const auto& query:{QStringLiteral("Volume"),QStringLiteral("volume"),QStringLiteral(" V O L U M E ")}){
                search->setText(query);QVERIFY(visibleStudies(tree).contains("low_volume"));QVERIFY(visibleStudies(tree).contains("volume"));
                QVERIFY(tree->currentItem());QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),QStringLiteral("volume"));
                QTest::keyClick(search,Qt::Key_Return);QVERIFY(preferences.enabled("volume"));QVERIFY(!preferences.enabled("low_volume"));
                QTest::keyClick(search,Qt::Key_Return);QVERIFY(!preferences.enabled("volume"));
            }
            inspected=true;
        });view.findChild<QPushButton*>("chartStudyPicker")->click();QVERIFY(inspected);
    }

    void rowActionsStayIndependentAndHelpReturnsFocus_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void rowActionsStayIndependentAndHelpReturnsFocus() {
        QFETCH(bool,dark);
        const auto previous=KeefiTheme::instance()->mode();
        const auto restore=qScopeGuard([&]{KeefiTheme::instance()->setMode(previous);});
        KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        ChartStudyPreferences preferences;preferences.markIntroSeen();
        preferences.setEnabled("bb",false);preferences.setVisible("bb",false);
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",900000);view.resize(1080,780);view.show();
        auto* picker=view.findChild<QPushButton*>("chartStudyPicker");QVERIFY(picker);bool inspected=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);const auto close=qScopeGuard([&]{dialog->reject();});
            auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");QVERIFY(search&&tree);
            search->setText(QStringLiteral("ㅂㄹㅈ"));QCOMPARE(visibleStudies(tree),QStringList{"bb"});
            auto* row=tree->currentItem();QVERIFY(row);QCOMPARE(row->text(0),QStringLiteral("Bollinger Bands"));QCOMPARE(row->text(2),QStringLiteral("− Off"));
            auto* info=dialog->findChild<QPushButton*>("chartStudyInfo_bb");auto* visible=dialog->findChild<QPushButton*>("chartStudyVisible_bb");auto* favorite=dialog->findChild<QPushButton*>("chartStudyFavorite_bb");QVERIFY(info&&visible&&favorite);
            QCOMPARE(visible->text(),QStringLiteral("Hidden"));QVERIFY(!visible->isChecked());QVERIFY(!visible->icon().isNull());
            QCOMPARE(row->text(5),QStringLiteral("Not calculated"));
            const auto saved=preferences.publicState();const auto ids=visibleStudies(tree);
            QHelpEvent hover(QEvent::ToolTip,info->rect().center(),info->mapToGlobal(info->rect().center()));
            QApplication::sendEvent(info,&hover);QCOMPARE(QToolTip::text(),chartStudyHelp("bb")->brief);QToolTip::hideText();
            QCOMPARE(preferences.publicState(),saved);QCOMPARE(tree->currentItem(),row);QCOMPARE(visibleStudies(tree),ids);
            for(const auto key:{Qt::Key_Return,Qt::Key_Space,Qt::Key_unknown}){
                bool explained=false;info->setFocus();
                QTimer::singleShot(0,dialog,[&]{
                    auto* help=dialog->findChild<QDialog*>("chartStudyHelpDialog");QVERIFY(help);const auto closeHelp=qScopeGuard([&]{help->reject();});
                    QVERIFY(help->windowTitle().startsWith(QStringLiteral("Bollinger Bands")));
                    QCOMPARE(help->width(), 520);
                    QVERIFY(help->height() <= 480);
                    auto* scroll = help->findChild<QScrollArea*>("chartStudyHelpScroll");
                    auto* expand = help->findChild<QPushButton*>("chartStudyHelpDetails");
                    QVERIFY(scroll && expand);
                    QCOMPARE(expand->height(), 28);
                    QVERIFY(expand->text().startsWith(QStringLiteral("▸")));
                    QCOMPARE(expand->text(),QStringLiteral("▸ Sources && Calculation"));
                    QCOMPARE(expand->accessibleName(),QStringLiteral("Sources & Calculation"));
                    expand->click();
                    QVERIFY(expand->text().startsWith(QStringLiteral("▾")));
                    QCOMPARE(expand->text(),QStringLiteral("▾ Sources && Calculation"));
                    QCOMPARE(expand->accessibleName(),QStringLiteral("Sources & Calculation"));
                    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
                    auto* brief=help->findChild<QLabel*>("chartStudyHelp_brief");QVERIFY(brief);QCOMPARE(brief->text(),chartStudyHelp("bb")->brief);
                    QTest::keyClick(help,Qt::Key_Escape);explained=true;
                });
                if(key==Qt::Key_unknown)QTest::mouseClick(info,Qt::LeftButton);else QTest::keyClick(info,key);
                QVERIFY(explained);QTRY_VERIFY(info->hasFocus());
                QCOMPARE(preferences.publicState(),saved);QCOMPARE(search->text(),QStringLiteral("ㅂㄹㅈ"));QCOMPARE(tree->currentItem(),row);QCOMPARE(visibleStudies(tree),ids);
            }
            // Visibility can change while Apply is Off, without enabling the row.
            const auto hiddenIcon=visible->icon().cacheKey();QTest::mouseClick(visible,Qt::LeftButton);
            QVERIFY(preferences.visible("bb"));QVERIFY(!preferences.enabled("bb"));QCOMPARE(row->checkState(0),Qt::Unchecked);QCOMPARE(row->text(2),QStringLiteral("− Off"));
            QCOMPARE(visible->text(),QStringLiteral("Visible"));QVERIFY(hiddenIcon!=visible->icon().cacheKey());
            auto* visibilityAx=QAccessible::queryAccessibleInterface(visible);QVERIFY(visibilityAx);QVERIFY(visibilityAx->state().checkable);QVERIFY(visibilityAx->state().checked);
            QTest::keyClick(visible,Qt::Key_Space);QVERIFY(!preferences.visible("bb"));QVERIFY(!preferences.enabled("bb"));
            QTest::keyClick(favorite,Qt::Key_Return);QVERIFY(preferences.favorite("bb"));QVERIFY(!preferences.enabled("bb"));QVERIFY(!preferences.visible("bb"));
            auto* favoriteAx=QAccessible::queryAccessibleInterface(favorite);QVERIFY(favoriteAx);QVERIFY(favoriteAx->state().checkable);QVERIFY(favoriteAx->state().checked);QVERIFY(!visibilityAx->state().checked);
            tree->setFocus();QTest::keyClick(tree,Qt::Key_Return);
            QVERIFY(preferences.enabled("bb"));QVERIFY(!preferences.visible("bb"));QCOMPARE(row->text(2),QStringLiteral("✓ On"));QCOMPARE(row->text(5),QStringLiteral("Needs data"));
            QVERIFY(row->data(0,Qt::AccessibleTextRole).toString().contains("On"));QVERIFY(row->data(0,Qt::AccessibleTextRole).toString().contains("Hidden"));
            inspected=true;
        });picker->click();QVERIFY(inspected);
    }

    void pickerRowsKeepDataOnOneLineAndAlignTheirActions_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void pickerRowsKeepDataOnOneLineAndAlignTheirActions() {
        QFETCH(bool,dark);
        auto* theme=KeefiTheme::instance();
        const auto previous=theme->mode();
        const auto restore=qScopeGuard([&]{theme->setMode(previous);});
        theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);

        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        preferences.setEnabled("bb",false);
        preferences.setVisible("bb",true);
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:upbit:KRW-SYN",900000);
        view.resize(1024,700);
        view.show();
        bool inspected=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto close=qScopeGuard([&]{dialog->reject();});
            auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");
            auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
            QVERIFY(search&&tree);
            dialog->resize(640,520);
            search->setText("Bollinger Bands");
            QCoreApplication::processEvents();
            auto* row=tree->currentItem();
            QVERIFY(row);
            QCOMPARE(row->data(0,Qt::UserRole).toString(),QStringLiteral("bb"));
            auto* visible=qobject_cast<QPushButton*>(tree->itemWidget(row,3));
            QVERIFY(visible);
            QVERIFY(visible->isEnabled()&&visible->isChecked());
            QCOMPARE(visible->accessibleName(),QStringLiteral("Bollinger Bands Visible"));

            const auto caption=KeefiFonts::font(FontRole::Caption12);
            QCOMPARE(row->text(5),QStringLiteral("Not calculated"));
            const int dataWidth=tree->columnWidth(5);
            const auto verifyData=[&] {
                QCOMPARE(row->font(5),caption);
                QVERIFY(dataWidth>=QFontMetrics(caption).horizontalAdvance(row->text(5))+4);
            };
            verifyData();

            const auto colorInsideButton=[&] {
                const auto image=visible->grab().toImage();
                const auto scale=image.devicePixelRatio();
                return image.pixelColor(qRound(5*scale),qRound(visible->height()*scale/2));
            };
            const auto unappliedFill=colorInsideButton();
            const auto unappliedInk=visible->palette().color(QPalette::ButtonText);

            preferences.setEnabled("bb",true);
            QCoreApplication::processEvents();
            QCOMPARE(row->text(5),QStringLiteral("Needs data"));
            const auto waitingFill=colorInsideButton();
            QVERIFY(visible->isEnabled()&&visible->isChecked());
            QCOMPARE(visible->accessibleName(),QStringLiteral("Bollinger Bands Visible"));
            const auto verifyCenters=[&] {
                const auto rowRect=tree->visualItemRect(row);
                const auto image=tree->viewport()->grab().toImage();
                const auto scale=image.devicePixelRatio();
                const int applyX=tree->columnViewportPosition(2)+10;
                int firstY=-1,lastY=-1;
                for(int y=rowRect.top();y<=rowRect.bottom();++y){
                    if(image.pixelColor(qRound(applyX*scale),qRound(y*scale))!=themeColors().text)continue;
                    if(firstY<0)firstY=y;
                    lastY=y;
                }
                QVERIFY(firstY>=0&&lastY>=firstY);
                const double applyCenter=(firstY+lastY)/2.0;
                for(int column:{1,3,4}){
                    auto* button=tree->itemWidget(row,column);
                    QVERIFY(button);
                    const double center=button->mapTo(tree->viewport(),QPoint()).y()+(button->height()-1)/2.0;
                    QVERIFY2(qAbs(center-applyCenter)<=1,qPrintable(QString("%1 center %2 vs Apply %3")
                        .arg(button->objectName()).arg(center).arg(applyCenter)));
                }
            };
            verifyCenters();
            const auto verifyAppearance=[&] {
                QVERIFY2(unappliedFill!=themeColors().text,qPrintable(unappliedFill.name()));
                QCOMPARE(unappliedInk,themeColors().text3);
                QVERIFY2(waitingFill!=themeColors().text,qPrintable(waitingFill.name()));
            };
            verifyAppearance();
            QCOMPARE(tree->columnWidth(5),dataWidth);
            preferences.setEnabled("bb",false);
            QCoreApplication::processEvents();
            QVERIFY(colorInsideButton()!=themeColors().text);
            QTest::mouseClick(visible,Qt::LeftButton);
            QVERIFY(!preferences.visible("bb")&&!preferences.enabled("bb"));
            QCOMPARE(visible->accessibleName(),QStringLiteral("Bollinger Bands Hidden"));
            inspected=true;
        });
        auto* picker=view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        picker->click();
        QVERIFY(inspected);
    }

    void pickerRowActionsAreReachableFromAccessibleDialog_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void pickerRowActionsAreReachableFromAccessibleDialog() {
        QFETCH(bool, dark);
        auto* platform = QGuiApplicationPrivate::platformIntegration()->accessibility();
        QVERIFY(platform);
        const bool previouslyActive = platform->isActive();
        platform->setActive(true);
        const auto previousHandler = QAccessible::installUpdateHandler([](QAccessibleEvent*) {});
        const auto restoreAccessibility = qScopeGuard([&] {
            QAccessible::installUpdateHandler(previousHandler);
            platform->setActive(previouslyActive);
        });
        QVERIFY(QAccessible::isActive());
        const auto previous = KeefiTheme::instance()->mode();
        const auto restore = qScopeGuard([&] { KeefiTheme::instance()->setMode(previous); });
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        preferences.setEnabled("bb", false);
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:upbit:KRW-SYN", 900000);
        view.resize(1080, 780);
        view.show();
        QVERIFY(QTest::qWaitForWindowActive(&view));
        auto* opener = view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(opener);
        // Reopening must not retain an interface belonging to the old dialog.
        for (int cycle = 0; cycle < 2; ++cycle) {
            preferences.setVisible("bb", true);
            preferences.setFavorite("bb", false);
            QPointer<QDialog> observedDialog;
            QAccessible::Id dialogId = 0, infoId = 0;
            bool inspected = false;
            QTimer::singleShot(0, &view, [&] {
                auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
                QVERIFY(dialog);
                observedDialog = dialog;
                const auto close = qScopeGuard([&] { dialog->reject(); });
                QTRY_VERIFY(dialog->isActiveWindow());
                auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
                auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
                QVERIFY(search && tree);
                search->setText(QStringLiteral("ㅂㄹㅈ"));
                QCoreApplication::processEvents();
                auto* root = QAccessible::queryAccessibleInterface(dialog);
                QVERIFY(root);
                QCOMPARE(root->role(), QAccessible::Dialog);
                dialogId = QAccessible::uniqueId(root);
                // Start at the dialog, as native AX does. Querying a button's
                // interface directly would hide a disconnected item-widget tree.
                const auto information = accessibleNamedDescendants(root, QStringLiteral("Bollinger Bands Information"));
                const auto visibility = accessibleNamedDescendants(root, QStringLiteral("Bollinger Bands Visible"));
                const auto favorites = accessibleNamedDescendants(root, QStringLiteral("Bollinger Bands Add to Favorites"));
                QCOMPARE(information.size(), 1);
                QCOMPARE(visibility.size(), 1);
                QCOMPARE(favorites.size(), 1);
                auto* info = information.front();
                auto* visible = visibility.front();
                auto* favorite = favorites.front();
                QCOMPARE(info->role(), QAccessible::Button);
                QCOMPARE(visible->role(), QAccessible::CheckBox);
                QCOMPARE(favorite->role(), QAccessible::CheckBox);
                QVERIFY(visible->state().checkable && visible->state().checked);
                QVERIFY(favorite->state().checkable && !favorite->state().checked);
                QVERIFY(!info->state().invisible);
                QVERIFY(!info->text(QAccessible::Description).isEmpty());
                for (auto* action : {info, visible, favorite}) {
                    QCOMPARE(action->parent(), root);
                    const int index = root->indexOfChild(action);
                    QVERIFY(index >= 0 && index < root->childCount());
                    QCOMPARE(root->child(index), action);
                    QVERIFY(action->actionInterface());
                    QVERIFY(action->actionInterface()->actionNames().contains(QAccessibleActionInterface::pressAction()));
                    const auto center = action->rect().center();
                    QCOMPARE(root->childAt(center.x(), center.y()), action);
                }
                infoId = QAccessible::uniqueId(info);
                emit view.studiesChanged();
                const auto refreshedInformation = accessibleNamedDescendants(root, QStringLiteral("Bollinger Bands Information"));
                QCOMPARE(refreshedInformation.size(), 1);
                QCOMPARE(QAccessible::uniqueId(refreshedInformation.front()), infoId);
                visible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
                QTRY_VERIFY(!preferences.visible("bb"));
                QVERIFY(!preferences.enabled("bb"));
                QVERIFY(!visible->state().checked);
                QCOMPARE(visible->text(QAccessible::Name), QStringLiteral("Bollinger Bands Hidden"));
                favorite->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
                QTRY_VERIFY(preferences.favorite("bb"));
                QVERIFY(!preferences.enabled("bb"));
                QVERIFY(favorite->state().checked);
                const auto saved = preferences.publicState();
                info->actionInterface()->doAction(QAccessibleActionInterface::setFocusAction());
                auto* infoButton = qobject_cast<QPushButton*>(info->object());
                QVERIFY(infoButton);
                const auto focusDiagnostic = [&] {
                    auto* active = QApplication::activeWindow();
                    auto* focus = QApplication::focusWidget();
                    return QStringLiteral("cycle=%1 active=%2 dialogActive=%3 focus=%4 dialogFocus=%5 visible=%6 enabled=%7 policy=%8 proxy=%9")
                        .arg(cycle).arg(active ? active->objectName() : QStringLiteral("null"))
                        .arg(dialog->isActiveWindow())
                        .arg(focus ? focus->objectName() : QStringLiteral("null"))
                        .arg(dialog->focusWidget() ? dialog->focusWidget()->objectName() : QStringLiteral("null"))
                        .arg(infoButton->isVisible()).arg(infoButton->isEnabled()).arg(int(infoButton->focusPolicy()))
                        .arg(infoButton->focusProxy() ? infoButton->focusProxy()->objectName() : QStringLiteral("null"));
                };
                QTRY_VERIFY2(infoButton->hasFocus(), qPrintable(focusDiagnostic()));
                bool explained = false, actionReturned = false, returnedBeforeHelp = false;
                QTimer helpWatcher;
                helpWatcher.setInterval(5);
                connect(&helpWatcher, &QTimer::timeout, dialog, [&] {
                    auto* help = dialog->findChild<QDialog*>("chartStudyHelpDialog");
                    if (!help) return;
                    helpWatcher.stop();
                    const auto closeHelp = qScopeGuard([&] { help->reject(); });
                    returnedBeforeHelp = actionReturned;
                    QCOMPARE(help->windowTitle(), QStringLiteral("Bollinger Bands · Information"));
                    QTest::keyClick(help, Qt::Key_Escape);
                    explained = true;
                });
                helpWatcher.start();
                info->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
                actionReturned = true;
                QTRY_VERIFY(explained);
                QVERIFY2(returnedBeforeHelp, "AXPress must return before the modal information window opens");
                QTRY_VERIFY(infoButton->hasFocus());
                auto* focused = root->focusChild();
                QVERIFY(focused);
                QCOMPARE(focused->object(), infoButton);
                QCOMPARE(preferences.publicState(), saved);
                QCOMPARE(search->text(), QStringLiteral("ㅂㄹㅈ"));
                QCOMPARE(visibleStudies(tree), QStringList{"bb"});
                search->setText(QStringLiteral("많이 올랐나"));
                QCoreApplication::processEvents();
                QVERIFY(accessibleNamedDescendants(root, QStringLiteral("Bollinger Bands Information")).isEmpty());
                QCOMPARE(accessibleNamedDescendants(root, QStringLiteral("RSI Information")).size(), 1);
                QCOMPARE(preferences.publicState(), saved);
                search->clear();
                tree->scrollToBottom();
                QCoreApplication::processEvents();
                QVERIFY(tree->verticalScrollBar()->maximum() > 0);
                QVERIFY(info->rect().isEmpty());
                QVERIFY(info->state().offscreen);
                QVERIFY(info->state().invisible);
                search->setText(QStringLiteral("ㅂㄹㅈ"));
                QCoreApplication::processEvents();
                QVERIFY(!info->rect().isEmpty());
                QVERIFY(!info->state().offscreen);
                QVERIFY(!info->state().invisible);
                inspected = true;
            });
            opener->click();
            QVERIFY(inspected);
            QVERIFY(observedDialog.isNull());
            QVERIFY(!QAccessible::accessibleInterface(dialogId));
            QVERIFY(!QAccessible::accessibleInterface(infoId));
            // Closing the picker deliberately queues focus back to its opener.
            // Observe that transition before attempting another user activation.
            QTRY_VERIFY(opener->hasFocus());
        }
    }

    void categoryFitsLongestEntryAtMinimumWidthAndLargeFont_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void categoryFitsLongestEntryAtMinimumWidthAndLargeFont() {
        QFETCH(bool,dark);const auto previous=KeefiTheme::instance()->mode();const auto restore=qScopeGuard([&]{KeefiTheme::instance()->setMode(previous);});
        KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("bb",true);preferences.setVisible("bb",true);preferences.setEnabled("rsi",true);preferences.setVisible("rsi",false);
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",900000);view.resize(900,780);view.show();
        bool inspected=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);const auto close=qScopeGuard([&]{dialog->reject();});
            auto* category=dialog->findChild<QComboBox*>("chartStudyCategory");auto* tree=dialog->findChild<QTreeWidget*>("chartStudyPickerTree");auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");QVERIFY(category&&tree&&search);
            const auto theme=dark?QStringLiteral("dark"):QStringLiteral("light");QCoreApplication::processEvents();
            QCOMPARE(search->palette().color(QPalette::Text),themeColors().text);QCOMPARE(search->palette().color(QPalette::PlaceholderText),themeColors().text3);
            saveReview(dialog,"offscreen-picker-"+theme+"-900px.png");
            QCOMPARE(search->height(), 28);
            auto* scope = dialog->findChild<QTabBar*>("chartStudyScope");
            QVERIFY(scope);
            QCOMPARE(scope->height(), 28);
            QCOMPARE(category->height(), 28);
            QVERIFY(category->width() >= 200 && category->width() <= 260);
            for(int pixels:{15,24}){
                auto font=dialog->font();font.setPixelSize(pixels);dialog->setFont(font);category->setFont(font);tree->setFont(font);
                for(auto* button:tree->findChildren<QPushButton*>())button->setFont(font);
                dialog->resize(640,520);QCoreApplication::processEvents();QCOMPARE(dialog->width(),640);QCOMPARE(tree->font().pixelSize(),pixels);
                int longest=0;for(int i=1;i<category->count();++i)if(category->fontMetrics().horizontalAdvance(category->itemText(i))>category->fontMetrics().horizontalAdvance(category->itemText(longest)))longest=i;
                category->setCurrentIndex(longest);QCoreApplication::processEvents();
                QStyleOptionComboBox option;option.initFrom(category);option.currentText=category->currentText();option.editable=false;option.frame=true;
                const auto textRect=category->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,category);
                const auto arrowRect=category->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,category);
                const int labelWidth=category->fontMetrics().horizontalAdvance(category->currentText());
                QVERIFY2(labelWidth<=textRect.width(),qPrintable(QString("%1 at %2px: needs %3px, has %4px in a %5px control")
                    .arg(category->currentText()).arg(pixels).arg(labelWidth).arg(textRect.width()).arg(category->width())));
                QVERIFY(textRect.right()<arrowRect.left());QVERIFY(category->geometry().right()<dialog->width());
                QVERIFY(textRect.right()<category->width()-30);
                if(pixels==24)saveReview(dialog,"offscreen-picker-"+theme+"-640px-24px-category.png");
                category->setCurrentIndex(0);search->setText("Bollinger");QCoreApplication::processEvents();tree->doItemsLayout();
                for(int column=1;column<6;++column)tree->resizeColumnToContents(column);
                auto* row=tree->currentItem();QVERIFY(row);QCOMPARE(row->data(0,Qt::UserRole).toString(),QStringLiteral("bb"));
                for(const auto* action:{"Info","Visible","Favorite"}){
                    auto* button=tree->findChild<QPushButton*>(QString("chartStudy%1_bb").arg(action));QVERIFY(button);
                    const QRect bounds(button->mapTo(tree->viewport(),QPoint{}),button->size());
                    QVERIFY2(tree->viewport()->rect().contains(bounds),qPrintable(button->objectName()+" "+QString::number(pixels)));
                    QCOMPARE(button->height(), 28);
                    QVERIFY(button->width() >= 28);
                }
                auto* visible=tree->findChild<QPushButton*>("chartStudyVisible_bb");QVERIFY(visible);
                QVERIFY(visible->fontMetrics().horizontalAdvance(visible->text())+visible->iconSize().width()+12<=visible->width());
                QVERIFY(tree->header()->sectionViewportPosition(5)+tree->columnWidth(5)<=tree->viewport()->width());
                QCOMPARE(row->text(2),QStringLiteral("✓ On"));QCOMPARE(row->text(5),QStringLiteral("Needs data"));
                search->clear();QCoreApplication::processEvents();
                if(pixels==24)saveReview(dialog,"offscreen-picker-"+theme+"-640px-24px-list.png");
            }
            inspected=true;
        });view.findChild<QPushButton*>("chartStudyPicker")->click();QVERIFY(inspected);
    }

    void maEditorFieldsRemainReadableAndTypesHaveUniqueDescriptions_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void maEditorFieldsRemainReadableAndTypesHaveUniqueDescriptions() {
        QFETCH(bool,dark);
        const auto previous=KeefiTheme::instance()->mode();
        const auto restore=qScopeGuard([&]{KeefiTheme::instance()->setMode(previous);});
        KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        ChartStudyPreferences preferences;preferences.markIntroSeen();
        QVERIFY(preferences.setMovingAverages(defaultChartMovingAverages()));
        const auto saved=preferences.publicState();
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",900000);
        // Use the containing window's real stylesheet, including its QLineEdit rule.
        bindThemePresentation(&view,[&view]{view.setStyleSheet(KeefiTheme::localStyleSheet());});
        view.resize(1000,780);view.show();
        bool inspected=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);
            const auto close=qScopeGuard([&]{dialog->reject();});
            auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");QVERIFY(search);search->setText("Moving Averages");
            auto* edit=dialog->findChild<QPushButton*>("chartStudyMaSettings");QVERIFY(edit&&edit->isVisible());
            bool fieldsInspected=false;
            QTimer::singleShot(0,dialog,[&]{
                auto* settings=dialog->findChild<QDialog*>("chartStudyMaSettingsDialog");QVERIFY(settings);
                const auto closeSettings=qScopeGuard([&]{settings->reject();});
                auto* accessibleSettings=QAccessible::queryAccessibleInterface(settings);QVERIFY(accessibleSettings);
                const auto distance=[](const QColor& a,const QColor& b){return qAbs(a.red()-b.red())+qAbs(a.green()-b.green())+qAbs(a.blue()-b.blue());};
                const auto pixelCount=[&](const QImage& image,const QRect& area,const QColor& color,int tolerance,const QColor* background=nullptr){
                    int count=0;for(int y=area.top();y<=area.bottom();++y)for(int x=area.left();x<=area.right();++x){const auto pixel=image.pixelColor(x,y);
                        if(distance(pixel,color)<=tolerance&&(!background||KeefiTheme::contrast(pixel,*background)>=4.5))++count;}
                    return count;
                };
                for(int pass=0;pass<2;++pass){
                    const bool currentDark=pass==0?dark:!dark;
                    KeefiTheme::instance()->setMode(currentDark?ThemeMode::Dark:ThemeMode::Light);
                    QCoreApplication::processEvents();
                    const auto colors=themeColors();
                    for(int i=0;i<ChartMovingAverageMaximumLines;++i){
                        auto* period=settings->findChild<QSpinBox*>(QString("chartMaPeriod_%1").arg(i));
                        auto* kind=settings->findChild<QComboBox*>(QString("chartMaKind_%1").arg(i));QVERIFY(period&&kind);
                        auto* editor=period->findChild<QLineEdit*>();QVERIFY(editor);
                        QCOMPARE(kind->accessibleName(),QString("Moving Average %1 Type").arg(i+1));
                        const auto description=QString("Moving Average %1 Type; SMA, EMA or WMA").arg(i+1);
                        QCOMPARE(kind->accessibleDescription(),description);QCOMPARE(kind->currentText(),QStringLiteral("EMA"));
                        int matches=0;QSet<QAccessible::Id> visited;QList<QAccessibleInterface*> pending{accessibleSettings};
                        while(!pending.isEmpty()&&visited.size()<4096){auto* node=pending.takeLast();if(!node||!node->isValid())continue;
                            const auto id=QAccessible::uniqueId(node);if(visited.contains(id))continue;visited.insert(id);
                            if(node->role()==QAccessible::ComboBox&&node->text(QAccessible::Description)==description){++matches;QCOMPARE(node->object(),static_cast<QObject*>(kind));}
                            for(int child=0;child<node->childCount();++child)pending.append(node->child(child));}
                        QVERIFY(pending.isEmpty());QCOMPARE(matches,1);
                        for(auto* field:{static_cast<QWidget*>(period),static_cast<QWidget*>(editor)})for(const auto group:{QPalette::Active,QPalette::Inactive}){
                            const auto palette=field->palette();QCOMPARE(palette.color(group,QPalette::Base),colors.input);QCOMPARE(palette.color(group,QPalette::Text),colors.text);
                            QVERIFY(KeefiTheme::contrast(palette.color(group,QPalette::Text),palette.color(group,QPalette::Base))>=4.5);
                            QVERIFY(KeefiTheme::contrast(palette.color(group,QPalette::HighlightedText),palette.color(group,QPalette::Highlight))>=4.5);
                        }
                        editor->deselect();kind->setFocus(Qt::OtherFocusReason);QCoreApplication::processEvents();
                        const auto normal=period->grab().toImage();const qreal scale=normal.devicePixelRatio();
                        const QRect field(editor->mapTo(period,QPoint{}),editor->size());
                        const QRect area(qRound((field.x()+2)*scale),qRound((field.y()+2)*scale),qRound((field.width()-4)*scale),qRound((field.height()-4)*scale));
                        QVERIFY(normal.rect().contains(area));
                        const auto background=normal.pixelColor(qRound((field.right()-4)*scale),qRound(field.center().y()*scale));
                        QVERIFY2(distance(background,colors.input)<=6,"Rendered Length background must use the active theme, including the native spinbox surface");
                        QVERIFY2(pixelCount(normal,area,colors.text,72,&background)>=3,"Rendered Length digits must contrast with their actual background");
                        period->setFocus(Qt::OtherFocusReason);editor->selectAll();QCoreApplication::processEvents();
                        QCOMPARE(editor->selectedText(),QString::number(period->value()));
                        const auto selected=period->grab().toImage();
                        QVERIFY2(pixelCount(selected,area,colors.surface,6)>=8,"Selected Length must render its themed selection background");
                        QVERIFY2(pixelCount(selected,area,colors.text,72,&colors.surface)>=3,"Selected Length digits must remain readable");
                    }
                    if(pass==0)saveReview(settings,QString("offscreen-ma-editor-%1.png").arg(dark?"dark":"light"));
                }
                fieldsInspected=true;
            });edit->click();QVERIFY(fieldsInspected);QCOMPARE(preferences.publicState(),saved);inspected=true;
        });view.findChild<QPushButton*>("chartStudyPicker")->click();QVERIFY(inspected);
    }

    void rowSettingsChangeInputsAndColorsWithoutTogglingTheStudy() {
        ChartStudyPreferences prefs;prefs.markIntroSeen();
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&prefs);
        view.setSelection("coin:upbit:KRW-SYN",900000);view.resize(1000,780);view.show();
        bool completed=false;
        QTimer::singleShot(0,&view,[&]{
            auto* picker=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(picker);
            const auto closePicker=qScopeGuard([&]{picker->reject();});
            auto* search=picker->findChild<QLineEdit*>("chartStudySearch");QVERIFY(search);
            search->setText("MACD");auto* open=picker->findChild<QPushButton*>("chartStudySettings_macd");QVERIFY(open&&open->isVisible());
            QTimer::singleShot(0,picker,[&]{
                auto* edit=picker->findChild<QDialog*>("chartStudySettingsDialog_macd");QVERIFY(edit);
                const auto closeEdit=qScopeGuard([&]{if(edit->isVisible())edit->reject();});
                auto* slow=edit->findChild<QSpinBox*>("chartStudyInput_macdSlow");
                auto* signal=edit->findChild<QSpinBox*>("chartStudyInput_macdSignal");
                auto* save=edit->findChild<QPushButton*>("chartStudySave");QVERIFY(slow&&signal&&save);
                slow->setValue(10);QVERIFY(!save->isEnabled());slow->setValue(120);
                signal->setValue(82);QVERIFY(!save->isEnabled());signal->setValue(81);QVERIFY(save->isEnabled());
                auto* color=edit->findChild<QPushButton*>("chartStudyColor_macdSignal");QVERIFY(color&&color->isVisible());
                QTimer::singleShot(0,edit,[&]{auto* chooser=edit->findChild<QColorDialog*>("chartStudyColorDialog");
                    QVERIFY(chooser);chooser->setCurrentColor(QColor("#123456"));chooser->accept();});
                color->click();QCOMPARE(color->text(),QStringLiteral("#123456"));save->click();
            });
            open->click();QCOMPARE(prefs.studySettings().macdSignal,81);QCOMPARE(prefs.lineColor("macdSignal",{}),QColor("#123456"));
            QVERIFY(!prefs.enabled("macd"));
            QTimer::singleShot(0,picker,[&]{
                auto* edit=picker->findChild<QDialog*>("chartStudySettingsDialog_macd");QVERIFY(edit);
                auto* reset=edit->findChild<QPushButton*>("chartStudyReset");QVERIFY(reset);reset->click();
                QCOMPARE(edit->findChild<QSpinBox*>("chartStudyInput_macdSignal")->value(),9);
                edit->reject();
            });
            open->click();QCOMPARE(prefs.studySettings().macdSignal,81);completed=true;
        });
        view.findChild<QPushButton*>("chartStudyPicker")->click();QVERIFY(completed);
    }
    void emptyMovingAverageLengthBlocksSaveUntilCorrected() {
        ChartStudyPreferences preferences;
        preferences.markIntroSeen();
        MarketCandlesView view(nullptr);
        view.setIndicatorPreferences(&preferences);
        view.setSelection("coin:upbit:KRW-SYN", 900000);
        view.resize(1000, 780);
        view.show();
        bool checked = false;
        QTimer::singleShot(0, &view, [&] {
            auto* dialog = view.findChild<QDialog*>("chartStudyPickerDialog");
            QVERIFY(dialog);
            const auto closePicker = qScopeGuard([&] { dialog->reject(); });
            auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
            QVERIFY(search);
            search->setText("Moving Averages");
            auto* settingsButton = dialog->findChild<QPushButton*>("chartStudyMaSettings");
            QVERIFY(settingsButton && settingsButton->isVisible());
            QTimer::singleShot(0, dialog, [&] {
                auto* settings = dialog->findChild<QDialog*>("chartStudyMaSettingsDialog");
                QVERIFY(settings);
                const auto closeSettings = qScopeGuard([&] { settings->reject(); });
                auto* save = settings->findChild<QPushButton*>("chartMaApply");
                auto* first = settings->findChild<QSpinBox*>("chartMaPeriod_0");
                auto* second = settings->findChild<QSpinBox*>("chartMaPeriod_1");
                QVERIFY(save && first && second);
                auto* editor = first->findChild<QLineEdit*>();
                auto* otherEditor = second->findChild<QLineEdit*>();
                QVERIFY(editor && otherEditor);
                editor->setFocus();
                QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
                editor->selectAll();
                QTest::keyClick(editor, Qt::Key_Backspace);
                QCOMPARE(editor->text(), QString());
                QVERIFY(!save->isEnabled());
                auto* message = settings->findChild<QLabel*>("chartMaLengthError_0");
                QVERIFY(message && message->isVisible());
                QCOMPARE(message->text(), QStringLiteral("2~200 사이 숫자를 넣어 주세요"));
                otherEditor->setFocus();
                QCOMPARE(editor->text(), QString());
                otherEditor->selectAll();
                QTest::keyClick(otherEditor, Qt::Key_Backspace);
                QVERIFY(!save->isEnabled());
                editor->setFocus();
                QTest::keyClicks(editor, "50");
                QVERIFY(!save->isEnabled());
                QVERIFY(message->isHidden());
                otherEditor->setFocus();
                QTest::keyClicks(otherEditor, "100");
                QVERIFY(save->isEnabled());
                checked = true;
            });
            settingsButton->click();
        });
        auto* picker = view.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker);
        picker->click();
        QVERIFY(checked);
    }

    void fourMaEditorPreservesLegacyValuesAndResetsExplicitly() {
        ChartStudyPreferences preferences;preferences.markIntroSeen();preferences.setEnabled("ma",false);preferences.setVisible("ma",false);preferences.setFavorite("ma",true);
        QVERIFY(preferences.setMovingAverages({{"ma1",20,ExtendedMaKind::Simple},{"ma2",60,ExtendedMaKind::Simple}}));
        MarketCandlesView view(nullptr);view.setIndicatorPreferences(&preferences);view.setSelection("coin:upbit:KRW-SYN",900000);view.resize(1000,780);view.show();
        bool inspected=false;
        QTimer::singleShot(0,&view,[&]{
            auto* dialog=view.findChild<QDialog*>("chartStudyPickerDialog");QVERIFY(dialog);const auto close=qScopeGuard([&]{dialog->reject();});
            auto* search=dialog->findChild<QLineEdit*>("chartStudySearch");QVERIFY(search);search->setText("Moving Averages");
            auto* edit=dialog->findChild<QPushButton*>("chartStudyMaSettings");QVERIFY(edit&&edit->isVisible());
            bool edited=false;
            QTimer::singleShot(0,dialog,[&]{
                auto* settings=dialog->findChild<QDialog*>("chartStudyMaSettingsDialog");QVERIFY(settings);const auto closeSettings=qScopeGuard([&]{settings->reject();});
                const QList<int> old{20,60,100,200};
                for(int i=0;i<4;++i){auto* period=settings->findChild<QSpinBox*>(QString("chartMaPeriod_%1").arg(i));auto* check=settings->findChild<QCheckBox*>(QString("chartMaEnabled_%1").arg(i));auto* kind=settings->findChild<QComboBox*>(QString("chartMaKind_%1").arg(i));QVERIFY(period&&check&&kind);QCOMPARE(period->value(),old[i]);QCOMPARE(check->isChecked(),i<2);QVERIFY(period->maximum()>=200);if(i<2)QCOMPARE(kind->currentText(),QStringLiteral("SMA"));}
                settings->findChild<QSpinBox*>("chartMaPeriod_0")->setValue(121);settings->findChild<QComboBox*>("chartMaKind_0")->setCurrentIndex(2);settings->findChild<QCheckBox*>("chartMaEnabled_0")->setChecked(false);
                settings->findChild<QCheckBox*>("chartMaEnabled_3")->setChecked(true);
                settings->findChild<QPushButton*>("chartMaApply")->click();edited=true;
            });edit->click();QVERIFY(edited);QCOMPARE(preferences.movingAverages().size(),4);
            QCOMPARE(preferences.movingAverages()[0].period,121);QCOMPARE(preferences.movingAverages()[0].kind,ExtendedMaKind::Weighted);QVERIFY(!preferences.movingAverages()[0].enabled);QVERIFY(preferences.movingAverages()[3].enabled);
            ChartStudyPreferences restored;QVERIFY(restored.restorePublicState(preferences.publicState()));QCOMPARE(restored.publicState(),preferences.publicState());
            const auto custom=preferences.publicState();
            for(bool save:{false,true}){
                bool resetSeen=false;
                QTimer::singleShot(0,dialog,[&]{
                    auto* settings=dialog->findChild<QDialog*>("chartStudyMaSettingsDialog");QVERIFY(settings);const auto closeSettings=qScopeGuard([&]{settings->reject();});
                    QCOMPARE(settings->findChild<QSpinBox*>("chartMaPeriod_0")->value(),121);QVERIFY(!settings->findChild<QCheckBox*>("chartMaEnabled_0")->isChecked());
                    settings->findChild<QPushButton*>("chartMaReset")->click();const QList<int> defaults{21,50,100,200};
                    for(int i=0;i<4;++i){QCOMPARE(settings->findChild<QSpinBox*>(QString("chartMaPeriod_%1").arg(i))->value(),defaults[i]);QVERIFY(settings->findChild<QCheckBox*>(QString("chartMaEnabled_%1").arg(i))->isChecked());QCOMPARE(settings->findChild<QComboBox*>(QString("chartMaKind_%1").arg(i))->currentText(),QStringLiteral("EMA"));}
                    if(save)settings->findChild<QPushButton*>("chartMaApply")->click();else settings->reject();resetSeen=true;
                });edit->click();QVERIFY(resetSeen);if(!save)QCOMPARE(preferences.publicState(),custom);
            }
            const QList<int> defaults{21,50,100,200};for(int i=0;i<4;++i){QCOMPARE(preferences.movingAverages()[i].period,defaults[i]);QCOMPARE(preferences.movingAverages()[i].kind,ExtendedMaKind::Exponential);QVERIFY(preferences.movingAverages()[i].enabled);}
            QVERIFY(!preferences.enabled("ma"));QVERIFY(!preferences.visible("ma"));QVERIFY(preferences.favorite("ma"));inspected=true;
        });view.findChild<QPushButton*>("chartStudyPicker")->click();QVERIFY(inspected);
    }

  private:
    void saveReview(QWidget* widget,const QString& filename){
        if(reviewDirectory_.isEmpty())return;
        QFile output(QDir(reviewDirectory_).filePath(filename));QVERIFY(output.open(QIODevice::WriteOnly|QIODevice::NewOnly));
        QVERIFY(widget->grab().save(&output,"PNG"));
    }
    QString reviewDirectory_;
    QTemporaryDir settings_;
};

QTEST_MAIN(ChartIndicatorPickerTest)
#include "tst_chart_indicator_picker.moc"
