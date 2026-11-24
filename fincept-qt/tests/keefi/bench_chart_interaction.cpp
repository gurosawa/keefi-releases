// Opt-in synthetic canvas probe. No account, vault, collector or network is created.
#include "keefi/CandleChart_p.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace keefi;
using keefi::market_candles_detail::CandleChart;

namespace {
constexpr qint64 Minute = 60000;
constexpr qint64 End = 1794700800000LL; // Fixed synthetic UTC minute boundary.
constexpr int Warmups = 3;

class ProbeApp final : public QApplication {
  public:
    using QApplication::QApplication;
    QWidget* target = nullptr;
    QElapsedTimer* clock = nullptr;
    double firstPaintEndMs = -1, firstPaintCpuMs = -1;
    int paints = 0;
    qint64 dirtyPixels = 0;
    bool notify(QObject* receiver, QEvent* event) override {
        if (clock && receiver == target && event->type() == QEvent::Paint) {
            auto* paint = static_cast<QPaintEvent*>(event);
            qint64 pixels = 0;
            for (const auto& rect : paint->region())
                pixels += qint64(rect.width()) * rect.height();
            const auto started = clock->nsecsElapsed();
            const bool result = QApplication::notify(receiver, event);
            const auto ended = clock->nsecsElapsed();
            ++paints;
            dirtyPixels += pixels;
            if (firstPaintEndMs < 0) {
                firstPaintEndMs = ended / 1e6;
                firstPaintCpuMs = (ended - started) / 1e6;
            }
            return result;
        }
        return QApplication::notify(receiver, event);
    }
    void arm(QWidget* widget, QElapsedTimer* timer) {
        target = widget;
        clock = timer;
        firstPaintEndMs = firstPaintCpuMs = -1;
        paints = 0;
        dirtyPixels = 0;
    }
    void disarm() { target = nullptr; clock = nullptr; }
};

QList<MarketCandle> candles(int count, qint64 end = End) {
    QList<MarketCandle> rows;
    rows.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto at = end - (count - i) * Minute;
        const int price = 10000 + int((at / Minute) % 29) - 14;
        rows.append({at, 0, QString::number(price),
                     QString::number(price + 3), QString::number(price - 3),
                     QString::number(price + ((at / Minute) % 2 ? 1 : -1)), "10"});
    }
    return rows;
}

double percentile(QList<double> values, double fraction) {
    if (values.isEmpty()) return -1;
    std::sort(values.begin(), values.end());
    return values[qBound(0, int(std::ceil(fraction * values.size())) - 1, int(values.size()) - 1)];
}

QJsonObject statistics(QList<double> values) {
    std::sort(values.begin(), values.end());
    if (values.isEmpty()) return {{"n", 0}};
    const int middle = int(values.size()) / 2;
    return {{"n", int(values.size())},
            {"median", values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2},
            {"p95", percentile(values, .95)}, {"maximum", values.last()}};
}

QPointF pointInPlot(const CandleChart& chart, double fraction = .5) {
    return {chart.property("viewportPlotLeft").toDouble() +
                chart.property("viewportPlotWidth").toDouble() * fraction,
            chart.property("viewportPlotTop").toDouble() +
                chart.property("viewportPlotHeight").toDouble() * .5};
}

void mouse(CandleChart& chart, QEvent::Type kind, QPointF point,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(kind, point, chart.mapToGlobal(point.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&chart, &event);
}

void wheel(CandleChart& chart, int steps) {
    const auto point = pointInPlot(chart);
    QWheelEvent event(point, chart.mapToGlobal(point.toPoint()), {}, QPoint(0, steps),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&chart, &event);
}

void settle(ProbeApp& app, CandleChart& chart) {
    app.disarm();
    chart.repaint();
    QTest::qWait(20);
}

struct Samples {
    QJsonArray raw;
    QMap<QString, QMap<QString, QList<double>>> values;
    QMap<QString, int> noPaints;
    bool measure(ProbeApp& app, CandleChart& chart, int count, const QString& operation,
                 int round, bool requirePaint, const std::function<void()>& action) {
        QObject callbackScope;
        QElapsedTimer clock;
        double eventLoopMs = -1;
        app.arm(&chart, &clock);
        clock.start();
        QTimer::singleShot(0, &callbackScope, [&] { eventLoopMs = clock.nsecsElapsed() / 1e6; });
        action();
        const double syncMs = clock.nsecsElapsed() / 1e6;
        // No-op setters are allowed to avoid painting. Observe for 25 ms instead of
        // requesting a paint ourselves (which would hide that optimization).
        const qint64 observationMs = requirePaint ? 3000 : 25;
        while ((eventLoopMs < 0 || (requirePaint && app.firstPaintEndMs < 0) ||
                (!requirePaint && clock.elapsed() < observationMs)) &&
               clock.elapsed() < observationMs)
            QTest::qWait(1);
        const bool ok = eventLoopMs >= 0 && (!requirePaint || app.firstPaintEndMs >= 0);
        const bool warmup = round < Warmups;
        QJsonObject result{{"completed_bars", count}, {"operation", operation},
                           {"round", round}, {"warmup", warmup},
                           {"sync_ms", syncMs}, {"event_loop_ms", eventLoopMs},
                           {"paint_events", app.paints}, {"dirty_logical_pixels", app.dirtyPixels},
                           {"paint_observed", app.firstPaintEndMs >= 0}, {"completed", ok}};
        if (operation == "history_prepend") {
            result.insert("completed_bars_before", count - qMin(200, count / 2));
            result.insert("completed_bars_after", count);
        }
        if (app.firstPaintEndMs >= 0) {
            result.insert("first_paint_end_ms", app.firstPaintEndMs);
            result.insert("first_paint_cpu_ms", app.firstPaintCpuMs);
        } else {
            result.insert("first_paint_end_ms", QJsonValue::Null);
            result.insert("first_paint_cpu_ms", QJsonValue::Null);
            result.insert("no_paint_observation_ms", int(observationMs));
        }
        raw.append(result);
        const auto key = QString::number(count) + "/" + operation;
        if (!warmup) {
            values[key]["sync_ms"].append(syncMs);
            values[key]["event_loop_ms"].append(eventLoopMs);
            if (app.firstPaintEndMs >= 0) {
                values[key]["first_paint_end_ms"].append(app.firstPaintEndMs);
                values[key]["first_paint_cpu_ms"].append(app.firstPaintCpuMs);
            } else ++noPaints[key];
            values[key]["paint_events"].append(app.paints);
            values[key]["dirty_logical_pixels"].append(double(app.dirtyPixels));
        }
        app.disarm();
        return ok;
    }
    QJsonObject summary() const {
        QJsonObject result;
        for (auto group = values.cbegin(); group != values.cend(); ++group) {
            QJsonObject metrics;
            for (auto metric = group->cbegin(); metric != group->cend(); ++metric)
                metrics.insert(metric.key(), statistics(*metric));
            metrics.insert("no_paint_samples", noPaints.value(group.key()));
            result.insert(group.key(), metrics);
        }
        return result;
    }
};
}

int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir profile;
    if (!profile.isValid()) return 3;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath("settings"));
    ProbeApp app(argc, argv);
    app.setOrganizationName("KEEFI-Synthetic-Chart");
    app.setApplicationName("KEEFI Synthetic Chart Interaction");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {"rows", "Comma-separated completed-bar counts; values above 5000 are optional canvas stress cases", "counts", "200,2000,5000"},
        {"rounds", "Measured samples per operation, after three warmups", "count", "30"},
        {"output", "New result JSON path (existing files are refused)", "path"},
        {"theme", "light or dark", "mode", "dark"},
        {"label", "Caller-supplied source/build label; not independently verified", "text", "unspecified"}
    });
    parser.process(app);
    bool roundsOk = false;
    const int rounds = parser.value("rounds").toInt(&roundsOk);
    QList<int> counts;
    for (const auto& value : parser.value("rows").split(',')) {
        bool ok = false;
        const int count = value.toInt(&ok);
        if (!ok || count < 200 || count > 20000 || counts.contains(count)) return 2;
        counts.append(count);
    }
    if (!roundsOk || rounds < 5 || rounds > 100 || counts.isEmpty() ||
        parser.value("output").isEmpty() || QFileInfo::exists(parser.value("output")) ||
        !QStringList{"light", "dark"}.contains(parser.value("theme"))) return 2;
    KeefiFonts::registerBundledFonts();
    app.setFont(KeefiFonts::font(FontRole::Body15));
    KeefiTheme::instance()->setMode(parser.value("theme") == "dark" ? ThemeMode::Dark : ThemeMode::Light);
    CandleChart chart(nullptr);
    chart.setWindowTitle("KEEFI 합성 차트 성능 측정");
    chart.resize(1440, 900);
    chart.setContext(Minute, "KRW", "합성 종목");
    chart.setStyleSheet(KeefiTheme::localStyleSheet(&chart));
    chart.show();
    Samples samples;
    bool completed = true;
    for (const int count : counts) {
        const auto rows = candles(count);
        std::optional<MarketCandle> forming = MarketCandle{End, 0, "10000", "10003", "9997", "10000", "1"};
        const auto prepare = [&] {
            chart.setObservation(rows, forming);
            chart.resetViewport();
            settle(app, chart);
        };
        prepare();
        if (!chart.isVisible() || chart.property("viewportPlotWidth").toDouble() <= 0) return 4;
        for (const auto& operation : QStringList{"hover", "pan_drag", "zoom_wheel", "live_update", "noop_refresh", "history_prepend"}) {
            prepare();
            const auto historyBase = rows.mid(qMin(200, count / 2));
            for (int round = 0; round < rounds + Warmups; ++round) {
                const auto point = pointInPlot(chart, .25 + (round % 13) / 26.0);
                if (operation == "history_prepend") {
                    chart.setObservation(historyBase, forming);
                    chart.navigateToTime(historyBase[historyBase.size() / 2].startsAtMs);
                    settle(app, chart);
                }
                if (operation == "pan_drag")
                    mouse(chart, QEvent::MouseButtonPress, pointInPlot(chart), Qt::LeftButton, Qt::LeftButton);
                const bool ok = samples.measure(app, chart, count, operation, round,
                    operation != "noop_refresh", [&] {
                        if (operation == "hover") mouse(chart, QEvent::MouseMove, point);
                        else if (operation == "pan_drag") {
                            const auto from = pointInPlot(chart);
                            mouse(chart, QEvent::MouseMove, from + QPointF(round % 2 ? -24 : 24, 0), Qt::NoButton, Qt::LeftButton);
                        } else if (operation == "zoom_wheel") wheel(chart, round % 2 ? -120 : 120);
                        else if (operation == "live_update") {
                            forming->close = QString::number(10000 + (round % 2));
                            forming->volume = QString::number(round + 2);
                            chart.setObservation(rows, forming);
                        } else if (operation == "noop_refresh") {
                            chart.setStockTimeline(false, {});
                            chart.setContext(Minute, "KRW", "합성 종목");
                            chart.setPriceLines({});
                            chart.setObservation(rows, forming);
                        } else chart.setObservation(rows, forming);
                    });
                if (operation == "pan_drag") {
                    mouse(chart, QEvent::MouseButtonRelease, pointInPlot(chart), Qt::LeftButton);
                    settle(app, chart);
                }
                if (!ok) { completed = false; break; }
            }
            if (!completed) break;
        }
        if (!completed) break;
    }
    const QJsonObject result{
        {"format", "keefi-synthetic-chart-interaction-probe/1"},
        {"completed", completed}, {"synthetic_only", true}, {"real_network_requests", 0},
        {"qt_platform", QGuiApplication::platformName()}, {"qt_version", QString(QT_VERSION_STR)},
        {"label", parser.value("label")}, {"theme", parser.value("theme")},
        {"window_width", chart.width()}, {"window_height", chart.height()},
        {"device_pixel_ratio", chart.devicePixelRatioF()}, {"measured_rounds", rounds},
        {"discarded_warmup_rounds", Warmups}, {"model_history_limit", MarketCandles::HistoryLimit},
        {"rows_above_model_limit_are_stress_only", true},
        {"percentile_method", "nearest-rank p95; even-sample median averages the middle two values"},
        {"limits", "Direct CandleChart only, with synthetic contiguous crypto candles and no indicators. "
                   "Excludes MarketCandlesView, network, data parsing, indicator workers, event injection delivery "
                   "and OS compositor/display latency. first_paint_end_ms ends at QWidget paintEvent completion. "
                   "No-paint samples are observed for 25 ms, not forced to repaint. Counts above model_history_limit "
                   "are canvas stress cases, not a claim that the live model stores that many candles."},
        {"summary", samples.summary()}, {"raw", samples.raw}
    };
    QSaveFile output(parser.value("output"));
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(result).toJson(QJsonDocument::Indented)) < 0 ||
        !output.commit()) return 3;
    return completed ? 0 : 4;
}
