#include "keefi/KeefiTheme.h"
#include "keefi/LoadProgressBar.h"

#include <QDir>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

using namespace keefi;

namespace {
bool save(QWidget& widget, const QString& name) {
    const auto dir = qEnvironmentVariable("KEEFI_MOTION_RENDER_DIR");
    if (dir.isEmpty()) return true;
    return QDir().mkpath(dir) && widget.grab().save(QDir(dir).filePath(name));
}
bool moving(const LoadProgressBar& bar) { return bar.shownShare() < bar.finishedShare(); }
}

class LoadProgressBarTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { KeefiTheme::instance()->setMode(ThemeMode::Light); }
    void stepsStayRealCountsWhileTheFillFollows();
    void fillCoversMostOfAStepEarly();
    void numberNeverRunsAheadOfFinishedSteps_data();
    void numberNeverRunsAheadOfFinishedSteps();
    void tailStaysInsideTheStepInFlight();
    void resetAndHiddenBarsJumpWithoutMoving();
    void drawingUsesTrackFillAndTailColours_data();
    void drawingUsesTrackFillAndTailColours();
};

void LoadProgressBarTest::stepsStayRealCountsWhileTheFillFollows() {
    LoadProgressBar bar; bar.resize(500, 8); bar.show(); QVERIFY(QTest::qWaitForWindowExposed(&bar));
    QSignalSpy values(&bar, &QProgressBar::valueChanged);
    bar.setSteps(0, 5, true);
    QCOMPARE(bar.maximum(), 5); QCOMPARE(bar.value(), 0); QCOMPARE(bar.shownShare(), 0.0); QCOMPARE(bar.shownPercent(), 0);
    bar.setSteps(1, 5, true);
    // The count is the truth at once; only the drawing takes its time.
    QCOMPARE(bar.value(), 1); QCOMPARE(bar.maximum(), 5); QCOMPARE(values.count(), 1); QCOMPARE(bar.finishedShare(), 0.2);
    QVERIFY2(bar.shownShare() < 0.2, "The fill eases toward a finished step; it does not jump.");
    qreal previous = bar.shownShare(); bool between = false; QElapsedTimer clock; clock.start();
    while (moving(bar) && clock.elapsed() < 3000) {
        QTest::qWait(16); const auto shown = bar.shownShare();
        QVERIFY(shown >= previous); QVERIFY(shown <= bar.finishedShare());
        between |= shown > 0 && shown < 0.2; previous = shown;
        QCOMPARE(bar.value(), 1); // waiting never changes the count
    }
    QVERIFY(between); QCOMPARE(bar.shownShare(), 0.2); QCOMPARE(bar.shownPercent(), 20);
    // The window re-reports the same counts on every refresh. That must not
    // hold the fill back: it still arrives well inside one ease.
    bar.setSteps(2, 5, true); QElapsedTimer repeated; repeated.start();
    while (moving(bar) && repeated.elapsed() < 3000) { bar.setSteps(2, 5, true); QTest::qWait(5); }
    QCOMPARE(bar.shownShare(), 0.4); QVERIFY2(repeated.elapsed() < 2 * LoadProgressBar::FollowMs, qPrintable(QString::number(repeated.elapsed())));
    // A second step arriving mid-motion continues from where the fill is.
    bar.setSteps(3, 5, true); QTest::qWait(40); const auto midway = bar.shownShare(); QVERIFY(midway > 0.4);
    bar.setSteps(4, 5, true); QVERIFY(bar.shownShare() >= midway); QCOMPARE(bar.value(), 4);
    QTRY_COMPARE(bar.shownShare(), 0.8); bar.setSteps(3, 5, true); // a lower count is drawn at once
    QTRY_COMPARE(bar.shownShare(), 0.6);
    bar.setSteps(5, 5, false); QCOMPARE(bar.value(), 5); QTRY_COMPARE(bar.shownShare(), 1.0); QCOMPARE(bar.shownPercent(), 100);
}

void LoadProgressBarTest::fillCoversMostOfAStepEarly() {
    LoadProgressBar bar; bar.resize(500, 8); bar.show(); QVERIFY(QTest::qWaitForWindowExposed(&bar));
    bar.setSteps(0, 5, true); bar.setSteps(1, 5, true);
    // A quarter of 480 ms in, a linear fill would be 25% of the way; OutExpo is
    // at 82%. The animation clock may start up to two frames late (about 70%),
    // and a late timer only moves the fill further, so 65% cannot flake.
    QTest::qWait(120);
    QVERIFY2(bar.shownShare() >= 0.2 * 0.65, qPrintable(QString::number(bar.shownShare())));
    QVERIFY(bar.shownShare() <= 0.2);
}

void LoadProgressBarTest::numberNeverRunsAheadOfFinishedSteps_data() {
    QTest::addColumn<int>("total");
    for (int total : {3, 4, 5, 7}) QTest::newRow(qPrintable(QString::number(total))) << total;
}
void LoadProgressBarTest::numberNeverRunsAheadOfFinishedSteps() {
    QFETCH(int, total);
    LoadProgressBar bar; bar.resize(500, 8); bar.show(); QVERIFY(QTest::qWaitForWindowExposed(&bar));
    int finished = 0, worst = 0, last = 0;
    connect(&bar, &LoadProgressBar::shownChanged, this, [&] {
        const int exact = finished * 100 / total, shown = bar.shownPercent();
        worst = qMax(worst, shown - exact); QVERIFY(shown >= 0); last = shown;
    });
    bar.setSteps(0, total, true);
    for (finished = 1; finished <= total; ++finished) {
        bar.setSteps(finished, total, finished < total);
        QTest::qWait(60); // the next step often lands while the fill still moves
        QVERIFY(bar.shownPercent() <= finished * 100 / total);
    }
    finished = total;
    QTRY_COMPARE(bar.shownPercent(), 100);
    QCOMPARE(worst, 0); QCOMPARE(last, 100);
}

void LoadProgressBarTest::tailStaysInsideTheStepInFlight() {
    LoadProgressBar bar; bar.resize(500, 8); bar.show(); QVERIFY(QTest::qWaitForWindowExposed(&bar));
    const qreal slot = 1.0 / 5, limit = LoadProgressBar::TailReach * slot;
    bar.setSteps(1, 5, true);
    QCOMPARE(bar.tailShare(), 0.0); // every step starts its tail from nothing
    qreal previous = 0; QElapsedTimer clock; clock.start();
    while (clock.elapsed() < 600) {
        QTest::qWait(30); const auto tail = bar.tailShare();
        QVERIFY(tail >= previous); QVERIFY(tail < limit);
        QVERIFY2(bar.finishedShare() + tail < 2 * slot, "The tail never reaches the next step's boundary.");
        QCOMPARE(bar.value(), 1); QVERIFY(bar.shownPercent() <= 20); // nor does it count as progress
        previous = tail;
    }
    // 1 - e^(-600/1800) is 28% of the way to the resting length.
    QVERIFY2(previous > 0.25 * limit, qPrintable(QString::number(previous / limit)));
    QVERIFY(save(bar, "bar-step-1-of-5-with-tail-light.png"));
    bar.setSteps(2, 5, true); QCOMPARE(bar.tailShare(), 0.0);
    QTest::qWait(60); QVERIFY(bar.tailShare() > 0);
    // Re-reporting the same step does not restart or lengthen the tail.
    const auto held = bar.tailShare(); bar.setSteps(2, 5, true); QVERIFY(bar.tailShare() >= held);
    bar.setSteps(2, 5, false); QCOMPARE(bar.tailShare(), 0.0);
    bar.setSteps(5, 5, true); QCOMPARE(bar.tailShare(), 0.0); QVERIFY(!bar.inFlight());
}

void LoadProgressBarTest::resetAndHiddenBarsJumpWithoutMoving() {
    LoadProgressBar bar; bar.resize(500, 8);
    bar.setSteps(2, 5, true); // not shown yet
    QCOMPARE(bar.shownShare(), 0.4); QCOMPARE(bar.shownPercent(), 40); QCOMPARE(bar.tailShare(), 0.0);
    bar.show(); QVERIFY(QTest::qWaitForWindowExposed(&bar));
    bar.setSteps(3, 5, true); QVERIFY(bar.shownShare() < 0.6);
    QSignalSpy shown(&bar, &LoadProgressBar::shownChanged);
    bar.resetSteps();
    QCOMPARE(bar.value(), 0); QCOMPARE(bar.maximum(), 1); QCOMPARE(bar.shownShare(), 0.0); QCOMPARE(bar.shownPercent(), 0);
    QCOMPARE(bar.tailShare(), 0.0); QVERIFY(shown.count() >= 1);
    QTest::qWait(60); QCOMPARE(bar.shownShare(), 0.0); // nothing keeps moving after a reset
    // A lower count (a new read with more accounts) is drawn at once, never eased backwards.
    bar.setSteps(3, 5, true); QTRY_COMPARE(bar.shownShare(), 0.6);
    bar.setSteps(1, 7, true); QCOMPARE(bar.shownShare(), 1.0 / 7);
    bar.setSteps(2, 7, true); bar.hide(); QCOMPARE(bar.shownShare(), 2.0 / 7);
}

void LoadProgressBarTest::drawingUsesTrackFillAndTailColours_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false; QTest::newRow("dark") << true;
}
void LoadProgressBarTest::drawingUsesTrackFillAndTailColours() {
    QFETCH(bool, dark); KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
    LoadProgressBar bar; bar.resize(500, 8); bar.show(); QVERIFY(QTest::qWaitForWindowExposed(&bar));
    const auto& colours = themeColors(); const auto tailColour = KeefiTheme::blend(colours.accent, colours.rule, 0.38);
    bar.setSteps(2, 5, true); QTRY_COMPARE(bar.shownShare(), 0.4);
    QTRY_VERIFY(bar.tailShare() * 500 > 12);
    const auto tail = bar.tailShare(); const auto image = bar.grab().toImage();
    QCOMPARE(image.pixelColor(100, 4).rgb(), colours.accent.rgb());              // finished steps
    QCOMPARE(image.pixelColor(int(500 * 0.4 + 4), 4).rgb(), tailColour.rgb());    // the step in flight
    QCOMPARE(image.pixelColor(int(500 * (0.4 + tail) + 12), 4).rgb(), colours.rule.rgb()); // not started
    QCOMPARE(image.pixelColor(495, 4).rgb(), colours.rule.rgb());
    QVERIFY(tailColour.rgb() != colours.accent.rgb()); QVERIFY(tailColour.rgb() != colours.rule.rgb());
    QVERIFY(save(bar, QString("bar-step-2-of-5-%1.png").arg(dark ? "dark" : "light")));
}

QTEST_MAIN(LoadProgressBarTest)
#include "tst_load_progress_bar.moc"
