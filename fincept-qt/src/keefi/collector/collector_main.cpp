// keefi-collector: headless public-market collector for KEEFI.
// Usage: keefi-collector [--root DIR] [--duration SECONDS] [--coins BTC,ETH,...] [--upbit BTC,ETH,...]
// Without --duration it runs until SIGINT/SIGTERM (launchd keeps it alive).
#include "keefi/collector/CollectorRuntime.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <atomic>
#include <csignal>
#include <cstdio>

#ifndef KEEFI_COLLECTOR_REVISION
#define KEEFI_COLLECTOR_REVISION "unknown"
#endif

namespace {
std::atomic_bool stopRequested{false};
void requestStop(int) { stopRequested = true; }
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("keefi-collector"));
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption root({"r", "root"}, "Record folder", "dir", keefi::collectorDefaultRoot());
    QCommandLineOption duration({"d", "duration"}, "Stop after N seconds (0 = run until signalled)", "seconds", "0");
    QCommandLineOption coins("coins", "Binance spot+USD-M coins", "list", "BTC,ETH,SOL,HYPE,ZEC");
    QCommandLineOption upbit("upbit", "Upbit KRW coins", "list", "BTC,ETH,SOL");
    QCommandLineOption floor("disk-floor-gb", "Pause raw trades below this free space", "gb", "30");
    parser.addOptions({root, duration, coins, upbit, floor});
    parser.process(app);

    keefi::CollectorRuntimeOptions options;
    options.root = parser.value(root);
    options.binanceCoins = parser.value(coins).split(',', Qt::SkipEmptyParts);
    options.upbitCoins = parser.value(upbit).split(',', Qt::SkipEmptyParts);
    options.revision = QStringLiteral(KEEFI_COLLECTOR_REVISION);
    options.diskFloorBytes = qint64(parser.value(floor).toDouble() * 1024 * 1024 * 1024);

    keefi::CollectorRuntime runtime(options);
    QString error;
    if (!runtime.start(&error)) {
        std::fprintf(stderr, "keefi-collector: %s\n", error.toUtf8().constData());
        return 2;
    }
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &app, [&] { if (stopRequested) app.quit(); });
    watch.start(200);
    if (const auto seconds = parser.value(duration).toLongLong(); seconds > 0) QTimer::singleShot(int(seconds * 1000), &app, &QCoreApplication::quit);
    const int code = app.exec();
    const auto summary = runtime.summary();
    runtime.stop();
    std::printf("%s\n", QJsonDocument(summary).toJson(QJsonDocument::Indented).constData());
    return code;
}
