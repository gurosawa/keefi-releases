// Opt-in frame capture of the asset load bar, for reviewing motion outside the
// app. Not part of the test run.
//   keefi_motion_review_probe <output directory>
// Writes one sprite sheet per theme and reels.json with each frame's real time.
//   keefi_motion_review_probe --vault-timing
// Prints how long a password check blocks the UI thread.
#include "keefi/KeefiTheme.h"
#include "keefi/LoadProgressBar.h"
#include "keefi/Vault.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;

namespace {
constexpr int FrameMs = 40;

struct Reel {
    QList<QImage> images;
    QJsonArray frames;
    QElapsedTimer clock;
    void add(const QImage& image, QJsonObject mark = {}) {
        // A still stretch reuses its picture; the mark keeps its own time.
        if (images.isEmpty() || images.last() != image) images.append(image);
        mark["ms"] = clock.elapsed();
        mark["image"] = int(images.size() - 1);
        frames.append(mark);
    }
    QJsonObject save(const QDir& dir, const QString& name) const {
        const auto size = images.first().size();
        const int columns = 10, rows = int((images.size() + columns - 1) / columns);
        QImage sheet(size.width() * columns, size.height() * rows, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::transparent);
        QPainter painter(&sheet);
        for (qsizetype index = 0; index < images.size(); ++index) {
            auto image = images[index];
            image.setDevicePixelRatio(1);
            painter.drawImage(QPoint(int(index % columns) * size.width(), int(index / columns) * size.height()), image);
        }
        painter.end();
        sheet.save(dir.filePath(name + ".png"));
        return {{"sheet", name + ".png"}, {"columns", columns}, {"width", size.width()}, {"height", size.height()},
                {"scale", images.first().devicePixelRatio()}, {"frames", frames}};
    }
};

// Three accounts, prices, then the screen: the previous stepped bar and the
// new one are fed the same finished-step counts at the same moments.
QJsonObject barReel(const QDir& dir, const QString& theme) {
    constexpr int total = 5;
    const QList<QPair<int, int>> schedule{{700, 1}, {1300, 2}, {3500, 3}, {3800, 4}, {3900, 5}};
    QWidget host; host.resize(402, 40);
    auto* before = new QProgressBar(&host); before->setGeometry(0, 4, 402, 8); before->setTextVisible(false); before->setRange(0, total);
    setThemeStyle(before, "QProgressBar{border:0;background:${rule};border-radius:4px;}QProgressBar::chunk{background:${accent};border-radius:4px;}");
    auto* after = new LoadProgressBar(&host); after->setGeometry(0, 28, 402, 8);
    setThemeStyle(&host, "QWidget{background:${bg};}");
    host.show(); (void)QTest::qWaitForWindowExposed(&host);
    Reel reel; int done = 0; after->setSteps(0, total, true); reel.clock.start();
    const auto shoot = [&] {
        while (done < total && reel.clock.elapsed() >= schedule[done].first) {
            ++done; before->setValue(done); after->setSteps(done, total, done < total);
        }
        reel.add(host.grab().toImage(), {{"done", done}, {"total", total}, {"before", done * 100 / total}, {"after", after->shownPercent()}});
    };
    shoot();
    QElapsedTimer span; span.start();
    while (span.elapsed() < 4700) { QTest::qWait(FrameMs); shoot(); }
    return reel.save(dir, "bar-" + theme);
}

int timeVaultCheck() {
    QTemporaryDir dir; if (!dir.isValid()) return 1;
    const QString password = QStringLiteral("Synthetic Probe! 2026");
    Vault vault(dir.filePath("synthetic-probe.bin"));
    QElapsedTimer clock; clock.start();
    if (!vault.create(password)) return 1;
    qInfo("create %lld ms", clock.elapsed());
    for (int run = 0; run < 5; ++run) {
        vault.lock(); clock.restart();
        if (!vault.unlock(password)) return 1;
        qInfo("unlock %lld ms", clock.elapsed());
    }
    vault.lock(); clock.restart();
    if (vault.unlock(QStringLiteral("Wrong Synthetic! 2026"))) return 1;
    qInfo("rejected unlock %lld ms", clock.elapsed());
    return 0;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().size() < 2) { qWarning("usage: keefi_motion_review_probe <output directory> | --vault-timing"); return 2; }
    if (app.arguments().at(1) == "--vault-timing") return timeVaultCheck();
    const QDir dir(app.arguments().at(1));
    if (!QDir().mkpath(dir.path())) return 1;
    QJsonObject reels;
    for (const auto mode : {ThemeMode::Dark, ThemeMode::Light}) {
        KeefiTheme::instance()->setMode(mode);
        const QString theme = mode == ThemeMode::Dark ? "dark" : "light";
        reels["bar-" + theme] = barReel(dir, theme);
    }
    QFile manifest(dir.filePath("reels.json"));
    if (!manifest.open(QIODevice::WriteOnly)) return 1;
    manifest.write(QJsonDocument(reels).toJson(QJsonDocument::Compact));
    return 0;
}
