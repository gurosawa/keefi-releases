#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MarketCompareSource.h"
#include "keefi/MarketCompareView.h"
#include "keefi/TradingTransport.h"
#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <memory>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000, Hour = 60 * Minute, Day = 24 * Hour;
constexpr auto IntervalKey = "marketCompare/intervalMs/v1";
constexpr auto PeriodKey = "marketCompare/period/v1";
constexpr auto AutomaticKey = "marketCompare/automaticInterval/v1";
const QStringList Periods{"1d", "1w", "1m", "3m"};

struct FixtureClock {
    QElapsedTimer elapsed;
    FixtureClock() { elapsed.start(); }
    qint64 now() const {
        // Safely inside every supported bar, while allowing Source's 250ms pacing.
        return 20000LL * Day + 5 * Minute + 10000 + elapsed.elapsed();
    }
};

qint64 wireInterval(const QString& value) {
    if (value == "1m") return Minute;
    if (value == "5m") return 5 * Minute;
    if (value == "15m") return 15 * Minute;
    if (value == "1h") return Hour;
    if (value == "4h") return 4 * Hour;
    if (value == "1d") return Day;
    return 0;
}

class PeriodWire final : public TradingTransport {
  public:
    enum class Fault { None, MiddleGold, EndGold, EmptyGold, ShortGold };
    explicit PeriodWire(const FixtureClock& clock) : clock_(clock) {}
    QList<RequestSpec> sent;
    Fault fault = Fault::None;
    void abort() override { ++generation_; }
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY2(!request.authenticated, "The period fixture accepts public requests only");
        QVERIFY2(MarketCompare::isRequest(request), qPrintable(request.url.toString()));
        sent.append(request);
        HttpResult reply;
        reply.status = 200;
        if (request.url.path().endsWith("/exchangeInfo")) {
            QJsonArray symbols;
            for (const auto& spec : MarketCompare::defaultSpecs())
                if (spec.venue == CompareVenue::BinanceUsdM)
                    symbols.append(QJsonObject{{"symbol", spec.symbol}, {"contractType", "TRADIFI_PERPETUAL"},
                        {"status", "TRADING"}, {"underlyingType", spec.underlyingType}});
            reply.body = QJsonDocument(QJsonObject{{"symbols", symbols}}).toJson(QJsonDocument::Compact);
        } else {
            const QUrlQuery query(request.url);
            const auto interval = wireInterval(query.queryItemValue("interval"));
            QVERIFY(interval > 0);
            QCOMPARE(query.queryItemValue("limit"), QStringLiteral("200"));
            const auto endExclusive = clock_.now() / interval * interval;
            const bool gold = query.queryItemValue("symbol") == QStringLiteral("XAUUSDT");
            QJsonArray bars;
            for (int i = 0; i < 200; ++i) {
                if (gold && (fault == Fault::EmptyGold || (fault == Fault::MiddleGold && i == 189) ||
                             (fault == Fault::EndGold && i == 199) ||
                             (fault == Fault::ShortGold && interval == Day && i < 120))) continue;
                const auto open = endExclusive - (200 - i) * interval;
                bars.append(QJsonArray{open, "100", "100", "100", "100", "1", open + interval - 1,
                                       "1", 1, "1", "1", "0"});
            }
            reply.body = QJsonDocument(bars).toJson(QJsonDocument::Compact);
        }
        const auto generation = generation_;
        QTimer::singleShot(0, this, [this, token, generation, reply] {
            if (generation == generation_) emit finished(token, reply);
        });
    }
  private:
    const FixtureClock& clock_;
    quint64 generation_ = 0;
};

struct Scenario {
    QTemporaryDir directory;
    QSettings settings;
    FixtureClock clock;
    PeriodWire wire;
    MarketCompareSource source;
    std::unique_ptr<MarketCompareView> view;

    explicit Scenario(qint64 interval = Hour, const QString& settingsFile = {})
        : settings(settingsFile.isEmpty() ? directory.filePath("synthetic-period.ini") : settingsFile,
                   QSettings::IniFormat), wire(clock),
          source(&wire, nullptr, [this] { return clock.now(); }) {
        source.setInterval(interval);
    }
    bool open() {
        if (!directory.isValid()) return false;
        view = std::make_unique<MarketCompareView>(&source);
        view->setSettings(&settings);
        view->resize(1440, 900);
        view->show();
        return QTest::qWaitForWindowExposed(view.get());
    }
    bool ready() const {
        const auto snapshot = source.snapshot();
        return !snapshot.loading && snapshot.series.size() == 4 &&
            std::ranges::all_of(snapshot.series, [](const auto& series) {
                return series.available && series.bars.size() == 200;
            });
    }
    QPushButton* period(const QString& id) const {
        return view->findChild<QPushButton*>("marketComparePeriod_" + id);
    }
    QTableWidget* table() const { return view->findChild<QTableWidget*>("marketComparePeriodTable"); }
};

bool noSelectedPeriod(const Scenario& scenario) {
    for (const auto& id : Periods) {
        const auto* button = scenario.period(id);
        if (!button || button->isChecked()) return false;
    }
    return true;
}

bool zeroPercent(const QTableWidgetItem* item) {
    if (!item) return false;
    auto text = item->text().trimmed();
    if (!text.endsWith('%')) return false;
    text.chop(1);
    text.replace(QChar(0x2212), '-');
    bool ok = false;
    const auto value = text.toDouble(&ok);
    return ok && std::abs(value) < 1e-12;
}

int rowNamed(const QTableWidget* table, const QString& name) {
    for (int row = 0; row < table->rowCount(); ++row)
        if (const auto* item = table->item(row, 0); item && item->text().contains(name)) return row;
    return -1;
}

QString periodBackgroundMismatch(QTableWidget& table, int selectedColumn) {
    if (table.rowCount() != 4 || table.columnCount() != 6)
        return QStringLiteral("Expected the real four-row period table");
    const auto picture = table.viewport()->grab().toImage();
    if (picture.isNull()) return QStringLiteral("Period viewport grab is empty");
    const auto ratio = picture.devicePixelRatio();
    for (int row = 0; row < 4; ++row) for (int column = 1; column <= 4; ++column) {
        const auto* item = table.item(row, column);
        if (!item) return QStringLiteral("Missing period cell %1,%2").arg(row).arg(column);
        const auto expected = column == selectedColumn ? themeColors().surface : themeColors().bg;
        if (item->background().color() != expected)
            return QStringLiteral("Cell %1,%2 background role is %3, expected %4")
                .arg(row).arg(column).arg(item->background().color().name(), expected.name());
        const auto cell = table.visualItemRect(item);
        // Three logical pixels inside the top edge avoid the divider; the
        // number baseline is lower, and this patch is beyond the left padding.
        const QRect patch(cell.left() + 8, cell.top() + 3, 3, 3);
        if (!cell.contains(patch) || !table.viewport()->rect().contains(patch))
            return QStringLiteral("Cell %1,%2 background patch is outside the visible viewport").arg(row).arg(column);
        for (int y = patch.top(); y <= patch.bottom(); ++y)
            for (int x = patch.left(); x <= patch.right(); ++x) {
                const QPoint pixel(int(std::floor((x + .5) * ratio)), int(std::floor((y + .5) * ratio)));
                if (!picture.rect().contains(pixel) || picture.pixelColor(pixel) != expected)
                    return QStringLiteral("Cell %1,%2 does not paint its %3 background at %4,%5")
                        .arg(row).arg(column).arg(expected.name()).arg(x).arg(y);
            }
    }
    return {};
}

// The real menu may use a nested popup event loop. Click its real existing
// timeframe button from a bounded timer and always dismiss the popup afterward.
bool chooseManualInterval(MarketCompareView& view, qint64 interval, int* buttonHeight) {
    auto* open = view.findChild<QPushButton*>("marketCompareIntervalMenu");
    if (!open || !open->isVisible()) return false;
    bool chosen = false, timedOut = false;
    QPointer<QWidget> popup;
    QTimer inspect, deadline;
    inspect.setInterval(1);
    deadline.setSingleShot(true);
    const auto closePopup = qScopeGuard([&] {
        inspect.stop();
        deadline.stop();
        if (popup && popup->isVisible()) QTest::keyClick(popup.data(), Qt::Key_Escape);
    });
    QObject::connect(&deadline, &QTimer::timeout, &view, [&] {
        timedOut = true;
        inspect.stop();
        if (auto* active = QApplication::activePopupWidget()) QTest::keyClick(active, Qt::Key_Escape);
    });
    QObject::connect(&inspect, &QTimer::timeout, &view, [&] {
        auto* active = QApplication::activePopupWidget();
        auto* button = view.findChild<QPushButton*>(QStringLiteral("marketCompareTimeframe_%1").arg(interval));
        if (!active || !button || !active->isAncestorOf(button) || !button->isVisible()) return;
        popup = active;
        inspect.stop();
        deadline.stop();
        *buttonHeight = button->height();
        QTest::mouseClick(button, Qt::LeftButton);
        chosen = true;
        if (popup && popup->isVisible()) QTest::keyClick(popup.data(), Qt::Key_Escape);
    });
    inspect.start();
    deadline.start(3000);
    QTest::mouseClick(open, Qt::LeftButton);
    if (!chosen && !timedOut && !QTest::qWaitFor([&] { return chosen || timedOut; }, 3500))
        timedOut = true;
    return chosen && !timedOut;
}
} // namespace

class MarketComparePeriodViewTests final : public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
  private slots:
    void initTestCase() {
        QVERIFY(settingsRoot_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot_.filePath("system"));
        QVERIFY(KeefiFonts::registerBundledFonts());
        QApplication::setFont(KeefiFonts::font(FontRole::Body15));
        KeefiTheme::instance()->setMode(ThemeMode::Light);
    }

    void defaultTableAndPresentationReuseTheCurrentSnapshot() {
        Scenario scenario;
        QVERIFY(scenario.open());
        QCOMPARE(scenario.source.intervalMs(), Hour);
        QVERIFY(noSelectedPeriod(scenario));
        QCOMPARE(scenario.settings.value(AutomaticKey, true).toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.wire.sent.size(), 5); // One catalogue and four price pages.
        auto* table = scenario.table();
        QVERIFY(table && table->isVisible());
        QCOMPARE(table->rowCount(), 4);
        QCOMPARE(table->columnCount(), 6);
        const QStringList headers{QStringLiteral("종목"), QStringLiteral("1일"), QStringLiteral("1주"),
            QStringLiteral("1달"), QStringLiteral("3달"), QStringLiteral("기준과 같이 움직였나")};
        for (int column = 0; column < headers.size(); ++column) {
            QVERIFY(table->horizontalHeaderItem(column));
            QCOMPARE(table->horizontalHeaderItem(column)->text(), headers[column]);
        }
        for (int row = 0; row < 4; ++row) {
            QVERIFY(zeroPercent(table->item(row, 1)));
            QVERIFY(zeroPercent(table->item(row, 2)));
            for (int column : {3, 4}) {
                QVERIFY(table->item(row, column));
                QCOMPARE(table->item(row, column)->text(), QStringLiteral("—"));
            }
        }
        QTRY_VERIFY(scenario.period("1d")->isEnabled());
        QTRY_VERIFY(scenario.period("1w")->isEnabled());
        for (const auto& id : {QString("1m"), QString("3m")})
            QVERIFY(scenario.period(id)->isEnabled()); // A click may fetch its declared coarser interval.
        QVERIFY(noSelectedPeriod(scenario));
        const int btc = rowNamed(table, QStringLiteral("BTC")) >= 0 ? rowNamed(table, QStringLiteral("BTC")) :
            rowNamed(table, QStringLiteral("비트코인"));
        QVERIFY(btc >= 0);
        for (int row = 0; row < 4; ++row) {
            QVERIFY(table->item(row, 5));
            QVERIFY(table->item(row, 5)->text().contains(row == btc ? QStringLiteral("기준") :
                MarketCompare::relationLabel(std::nullopt)));
        }

        const auto requests = scenario.wire.sent.size();
        QVERIFY(!table->grab().isNull());
        auto* chart = scenario.view->findChild<QWidget*>("marketCompareChart");
        auto* gold = scenario.view->findChild<QAbstractButton*>("marketCompareChip_XAUUSDT");
        QVERIFY(chart && chart->isVisible() && gold && gold->isVisible());
        QTest::mouseMove(chart, QPoint(chart->width() / 2, chart->height() / 2));
        QTRY_VERIFY(!chart->accessibleDescription().isEmpty());
        QVERIFY(gold->isChecked());
        QTest::mouseClick(gold, Qt::LeftButton);
        QVERIFY(!gold->isChecked());
        scenario.view->refreshFromSource();
        QCOMPARE(table->rowCount(), 4);
        for (int row = 0; row < 4; ++row) QVERIFY(zeroPercent(table->item(row, 1)));
        QTest::mouseClick(gold, Qt::LeftButton);
        QVERIFY(gold->isChecked());
        QTest::qWait(300);
        QCOMPARE(scenario.wire.sent.size(), requests);
    }

    void periodButtonsUseTheDeclaredIntervalAndExactWindow_data() {
        QTest::addColumn<QString>("period");
        QTest::addColumn<qint64>("seedInterval");
        QTest::addColumn<qint64>("targetInterval");
        QTest::addColumn<qint64>("duration");
        QTest::newRow("day") << QString("1d") << Hour << 15 * Minute << Day;
        QTest::newRow("week") << QString("1w") << 4 * Hour << Hour << 7 * Day;
        QTest::newRow("month-30-days") << QString("1m") << Day << 4 * Hour << 30 * Day;
        QTest::newRow("quarter-90-days") << QString("3m") << Day << Day << 90 * Day;
        QTest::newRow("month-from-default-hour") << QString("1m") << Hour << 4 * Hour << 30 * Day;
        QTest::newRow("quarter-from-default-hour") << QString("3m") << Hour << Day << 90 * Day;
    }

    void periodButtonsUseTheDeclaredIntervalAndExactWindow() {
        QFETCH(QString, period);
        QFETCH(qint64, seedInterval);
        QFETCH(qint64, targetInterval);
        QFETCH(qint64, duration);
        Scenario scenario(seedInterval);
        QVERIFY(scenario.open());
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.wire.sent.size(), 5);
        QVERIFY(noSelectedPeriod(scenario));
        auto* button = scenario.period(period);
        QVERIFY(button && button->isVisible());
        QTRY_VERIFY(button->isEnabled());
        const auto before = scenario.wire.sent.size();
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_COMPARE(scenario.source.intervalMs(), targetInterval);
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        const auto expectedRequests = before + (seedInterval == targetInterval ? 0 : 4);
        QCOMPARE(scenario.wire.sent.size(), expectedRequests);
        const auto end = scenario.clock.now() / targetInterval * targetInterval - targetInterval;
        QCOMPARE(scenario.view->baseOpenMs(), end - duration);
        QCOMPARE(end - scenario.view->baseOpenMs(), duration);
        for (const auto& id : Periods) QCOMPARE(scenario.period(id)->isChecked(), id == period);
        QCOMPARE(scenario.settings.value(IntervalKey).toLongLong(), targetInterval);
        QCOMPARE(scenario.settings.value(PeriodKey).toString(), period);
        QCOMPARE(scenario.settings.value(AutomaticKey).toBool(), true);
        for (qsizetype i = before; i < scenario.wire.sent.size(); ++i) {
            const auto& request = scenario.wire.sent[i];
            QVERIFY(request.url.path().endsWith("/klines"));
            QCOMPARE(QUrlQuery(request.url).queryItemValue("interval"), MarketCompare::wireInterval(targetInterval));
            QVERIFY(!request.authenticated);
        }
    }

    void periodSequenceAutomaticallyChangesIntervalsWithoutPresentationRequests() {
        Scenario scenario;
        QVERIFY(scenario.open());
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.source.intervalMs(), Hour);
        QCOMPARE(scenario.wire.sent.size(), 5);
        struct Step { const char* period; qint64 interval, duration; };
        const Step steps[] = {{"1d", 15 * Minute, Day}, {"1w", Hour, 7 * Day},
                              {"1m", 4 * Hour, 30 * Day}, {"3m", Day, 90 * Day}};
        int expectedRequests = 5;
        for (const auto& step : steps) {
            auto* button = scenario.period(step.period);
            QVERIFY2(button && button->isVisible() && button->isEnabled(), step.period);
            const auto before = scenario.wire.sent.size();
            QTest::mouseClick(button, Qt::LeftButton);
            QTRY_COMPARE(scenario.source.intervalMs(), step.interval);
            QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
            expectedRequests += 4;
            QCOMPARE(scenario.wire.sent.size(), expectedRequests);
            const auto end = scenario.clock.now() / step.interval * step.interval - step.interval;
            QCOMPARE(scenario.view->baseOpenMs(), end - step.duration);
            for (const auto& id : Periods) QCOMPARE(scenario.period(id)->isChecked(), id == step.period);
            QCOMPARE(scenario.settings.value(PeriodKey).toString(), QString::fromLatin1(step.period));
            QCOMPARE(scenario.settings.value(IntervalKey).toLongLong(), step.interval);
            QVERIFY(scenario.settings.value(AutomaticKey).toBool());
            for (qsizetype i = before; i < scenario.wire.sent.size(); ++i) {
                const auto& request = scenario.wire.sent[i];
                QVERIFY(request.url.path().endsWith("/klines"));
                QCOMPARE(QUrlQuery(request.url).queryItemValue("interval"), MarketCompare::wireInterval(step.interval));
            }
            auto* table = scenario.table();
            auto* chart = scenario.view->findChild<QWidget*>("marketCompareChart");
            QVERIFY(table && table->isVisible() && chart && chart->isVisible());
            scenario.view->refreshFromSource();
            QVERIFY(!table->viewport()->grab().isNull());
            QTest::mouseMove(chart, QPoint(chart->width() * 3 / 5, chart->height() / 2));
            QTRY_VERIFY(!chart->accessibleDescription().isEmpty());
            QTest::mouseMove(button, button->rect().center());
            QTest::qWait(300);
            QCOMPARE(scenario.wire.sent.size(), expectedRequests);
            QCOMPARE(scenario.view->baseOpenMs(), end - step.duration);
        }
        QCOMPARE(scenario.wire.sent.size(), 21);
    }

    void tableHeaderSelectsThePeriodAndChartClickClearsIt() {
        Scenario scenario(4 * Hour);
        scenario.settings.setValue("unrelated/public", "keep");
        QVERIFY(scenario.open());
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        auto* table = scenario.table();
        auto* week = scenario.period("1w");
        QVERIFY(table && week && week->isEnabled());
        const auto initialBackgroundError = periodBackgroundMismatch(*table, -1);
        QVERIFY2(initialBackgroundError.isEmpty(), qPrintable(initialBackgroundError));
        auto* header = table->horizontalHeader();
        const auto requests = scenario.wire.sent.size();
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(header->sectionViewportPosition(2) + header->sectionSize(2) / 2,
                   header->height() / 2));
        QTRY_COMPARE(scenario.source.intervalMs(), Hour);
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.wire.sent.size(), requests + 4);
        QVERIFY(week->isChecked());
        QCOMPARE(scenario.settings.value(PeriodKey).toString(), QStringLiteral("1w"));
        const auto selectedBackgroundError = periodBackgroundMismatch(*table, 2);
        QVERIFY2(selectedBackgroundError.isEmpty(), qPrintable(selectedBackgroundError));
        const auto end = scenario.clock.now() / Hour * Hour - Hour;
        QCOMPARE(scenario.view->baseOpenMs(), end - 7 * Day);
        const auto periodBase = scenario.view->baseOpenMs();
        auto* chart = scenario.view->findChild<QWidget*>("marketCompareChart");
        QVERIFY(chart && chart->isVisible());
        QTest::mouseClick(chart, Qt::LeftButton, Qt::NoModifier,
            QPoint(chart->width() * 3 / 5, chart->height() * 2 / 3));
        QTRY_VERIFY(noSelectedPeriod(scenario));
        QVERIFY(scenario.settings.value(PeriodKey).toString().isEmpty());
        const auto clearedBackgroundError = periodBackgroundMismatch(*table, -1);
        QVERIFY2(clearedBackgroundError.isEmpty(), qPrintable(clearedBackgroundError));
        QVERIFY(scenario.view->baseOpenMs() != periodBase);
        QCOMPARE(scenario.source.intervalMs(), Hour);
        QTest::qWait(300);
        QCOMPARE(scenario.wire.sent.size(), requests + 4);
        QCOMPARE(scenario.settings.value("unrelated/public").toString(), QStringLiteral("keep"));
    }

    void manualSelectingTheCurrentIntervalTurnsAutomaticOffWithoutRequests() {
        QSettings defaults;
        QVariantMap defaultsBefore;
        for (const auto& key : defaults.allKeys()) defaultsBefore.insert(key, defaults.value(key));
        Scenario scenario;
        scenario.settings.setValue("unrelated/public", "keep");
        QVERIFY(scenario.open());
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.source.intervalMs(), Hour);
        QCOMPARE(scenario.wire.sent.size(), 5);
        QVERIFY(noSelectedPeriod(scenario));
        QVERIFY(scenario.settings.value(AutomaticKey, true).toBool());
        int buttonHeight = 0;
        QVERIFY(chooseManualInterval(*scenario.view, Hour, &buttonHeight));
        QCOMPARE(buttonHeight, 28);
        QCOMPARE(scenario.settings.value(AutomaticKey, true).toBool(), false);
        QVERIFY(noSelectedPeriod(scenario));
        QVERIFY(scenario.settings.value(PeriodKey).toString().isEmpty());
        QCOMPARE(scenario.source.intervalMs(), Hour);
        QCOMPARE(scenario.settings.value("unrelated/public").toString(), QStringLiteral("keep"));
        QTest::qWait(300);
        QCOMPARE(scenario.wire.sent.size(), 5);
        QVariantMap defaultsAfter;
        for (const auto& key : defaults.allKeys()) defaultsAfter.insert(key, defaults.value(key));
        QCOMPARE(defaultsAfter, defaultsBefore);
    }

    void manualMenuUsesExistingButtonsAndPersistsManualMode() {
        Scenario scenario;
        scenario.settings.setValue(IntervalKey, 4 * Hour);
        scenario.settings.setValue("unrelated/public", "keep");
        QVERIFY(scenario.open());
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.source.intervalMs(), 4 * Hour);
        QCOMPARE(scenario.wire.sent.size(), 5);
        QVERIFY(noSelectedPeriod(scenario));
        int buttonHeight = 0;
        QVERIFY(chooseManualInterval(*scenario.view, Day, &buttonHeight));
        QCOMPARE(buttonHeight, 28);
        QTRY_COMPARE(scenario.source.intervalMs(), Day);
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.wire.sent.size(), 9);
        QCOMPARE(scenario.settings.value(IntervalKey).toLongLong(), Day);
        QVERIFY(scenario.settings.value(PeriodKey).toString().isEmpty());
        QVERIFY(!scenario.settings.value(AutomaticKey, true).toBool());
        QCOMPARE(scenario.settings.value("unrelated/public").toString(), QStringLiteral("keep"));
        scenario.settings.sync();
        const auto savedPath = scenario.settings.fileName();
        scenario.view.reset();

        Scenario restored(Hour, savedPath);
        QVERIFY(restored.open());
        QTRY_VERIFY_WITH_TIMEOUT(restored.ready(), 5000);
        QCOMPARE(restored.source.intervalMs(), Day);
        QVERIFY(noSelectedPeriod(restored));
        QVERIFY(!restored.settings.value(AutomaticKey, true).toBool());
        QCOMPARE(restored.settings.value("unrelated/public").toString(), QStringLiteral("keep"));
        QCOMPARE(restored.wire.sent.size(), 5);
        auto* quarter = restored.period("3m");
        QVERIFY(quarter && quarter->isEnabled());
        QTest::mouseClick(quarter, Qt::LeftButton);
        QTRY_VERIFY(quarter->isChecked());
        QCOMPARE(restored.settings.value(PeriodKey).toString(), QStringLiteral("3m"));
        QCOMPARE(restored.settings.value(AutomaticKey).toBool(), true);
        QTest::qWait(300);
        QCOMPARE(restored.wire.sent.size(), 5); // Same daily interval: no new source cycle.
    }

    void oneIncompleteSeriesDisablesTheSharedPeriod_data() {
        QTest::addColumn<qint64>("interval");
        QTest::addColumn<QString>("period");
        QTest::addColumn<int>("column");
        QTest::addColumn<int>("fault");
        QTest::newRow("day-middle-missing") << 15 * Minute << QString("1d") << 1 << int(PeriodWire::Fault::MiddleGold);
        QTest::newRow("day-end-missing") << 15 * Minute << QString("1d") << 1 << int(PeriodWire::Fault::EndGold);
        QTest::newRow("day-one-series-unavailable") << 15 * Minute << QString("1d") << 1 << int(PeriodWire::Fault::EmptyGold);
        QTest::newRow("week-middle-missing") << Hour << QString("1w") << 2 << int(PeriodWire::Fault::MiddleGold);
        QTest::newRow("week-end-missing") << Hour << QString("1w") << 2 << int(PeriodWire::Fault::EndGold);
        QTest::newRow("week-one-series-unavailable") << Hour << QString("1w") << 2 << int(PeriodWire::Fault::EmptyGold);
    }

    void oneIncompleteSeriesDisablesTheSharedPeriod() {
        QFETCH(qint64, interval);
        QFETCH(QString, period);
        QFETCH(int, column);
        QFETCH(int, fault);
        Scenario scenario(interval);
        scenario.wire.fault = PeriodWire::Fault(fault);
        QVERIFY(scenario.open());
        QTRY_COMPARE_WITH_TIMEOUT(scenario.wire.sent.size(), 5, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!scenario.source.snapshot().loading, 5000);
        auto* table = scenario.table();
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 4);
        const auto gold = rowNamed(table, QStringLiteral("금"));
        QVERIFY(gold >= 0);
        QVERIFY(table->item(gold, column));
        QCOMPARE(table->item(gold, column)->text(), QStringLiteral("—"));
        for (int row = 0; row < 4; ++row)
            if (row != gold) QVERIFY(zeroPercent(table->item(row, column)));
        const auto requests = scenario.wire.sent.size();
        auto* button = scenario.period(period);
        QVERIFY(button && !button->isEnabled());
        QCOMPARE(button->toolTip(), QStringLiteral("자료가 아직 모자라요"));
        QTest::mouseClick(button, Qt::LeftButton);
        auto* header = table->horizontalHeader();
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2, header->height() / 2));
        QTest::qWait(300);
        QVERIFY(noSelectedPeriod(scenario));
        QCOMPARE(scenario.source.intervalMs(), interval);
        QCOMPARE(scenario.wire.sent.size(), requests);
    }

    void shortDailySeriesKeepsQuarterUnavailableWithoutFetching() {
        Scenario scenario(Day);
        scenario.wire.fault = PeriodWire::Fault::ShortGold;
        QVERIFY(scenario.open());
        QTRY_COMPARE_WITH_TIMEOUT(scenario.wire.sent.size(), 5, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!scenario.source.snapshot().loading, 5000);
        const auto snapshot = scenario.source.snapshot();
        QCOMPARE(snapshot.series.size(), 4);
        for (const auto& series : snapshot.series) {
            QVERIFY(series.available);
            QCOMPARE(series.bars.size(), series.spec.symbol == "XAUUSDT" ? 80 : 200);
        }
        auto* quarter = scenario.period("3m");
        auto* month = scenario.period("1m");
        auto* table = scenario.table();
        QVERIFY(quarter && month && table);
        QVERIFY(!quarter->isEnabled());
        QCOMPARE(quarter->toolTip(), QStringLiteral("자료가 아직 모자라요"));
        QVERIFY(month->isEnabled());
        const auto gold = rowNamed(table, QStringLiteral("금"));
        QVERIFY(gold >= 0);
        QVERIFY(zeroPercent(table->item(gold, 3)));
        QVERIFY(table->item(gold, 4));
        QCOMPARE(table->item(gold, 4)->text(), QStringLiteral("—"));
        for (int row = 0; row < table->rowCount(); ++row)
            if (row != gold) QVERIFY(zeroPercent(table->item(row, 4)));
        QVERIFY(!table->viewport()->grab().isNull());
        QTest::qWait(300);
        QVERIFY(noSelectedPeriod(scenario));
        QCOMPARE(scenario.source.intervalMs(), Day);
        QCOMPARE(scenario.wire.sent.size(), 5);
    }

    void settingsRestoreLegacyIntervalAndAutomaticPeriod_data() {
        QTest::addColumn<QString>("period");
        QTest::addColumn<qint64>("interval");
        QTest::addColumn<qint64>("duration");
        QTest::newRow("legacy-interval-only") << QString{} << 4 * Hour << qint64(0);
        QTest::newRow("automatic-period") << QString("1m") << 4 * Hour << 30 * Day;
    }

    void settingsRestoreLegacyIntervalAndAutomaticPeriod() {
        QFETCH(QString, period);
        QFETCH(qint64, interval);
        QFETCH(qint64, duration);
        Scenario scenario;
        scenario.settings.setValue(IntervalKey, interval);
        scenario.settings.setValue("unrelated/public", "keep");
        if (!period.isEmpty()) {
            scenario.settings.setValue(PeriodKey, period);
            scenario.settings.setValue(AutomaticKey, true);
        }
        scenario.settings.sync();
        QVERIFY(scenario.open());
        QTRY_VERIFY_WITH_TIMEOUT(scenario.ready(), 5000);
        QCOMPARE(scenario.source.intervalMs(), interval);
        QCOMPARE(scenario.wire.sent.size(), 5);
        QCOMPARE(scenario.settings.value(AutomaticKey, true).toBool(), true);
        QCOMPARE(scenario.settings.value("unrelated/public").toString(), QStringLiteral("keep"));
        if (period.isEmpty()) {
            QVERIFY(noSelectedPeriod(scenario));
        } else {
            auto* selected = scenario.period(period);
            QVERIFY(selected && selected->isChecked());
            const auto end = scenario.clock.now() / interval * interval - interval;
            QCOMPARE(scenario.view->baseOpenMs(), end - duration);
            for (const auto& id : Periods) QCOMPARE(scenario.period(id)->isChecked(), id == period);
            QCOMPARE(scenario.settings.value(PeriodKey).toString(), period);
        }
    }
};

QTEST_MAIN(MarketComparePeriodViewTests)
#include "tst_market_compare_period_view.moc"
