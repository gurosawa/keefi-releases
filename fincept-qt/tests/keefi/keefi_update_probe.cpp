#include "keefi/ReleaseUpdater.h"
#include <QCoreApplication>
#include <QFile>
#include <QNetworkAccessManager>
#include <QTimer>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    bool number = false;
    const auto build = args.value(2).toInt(&number);
    if ((args.size() != 3 && args.size() != 4) || !number || build < 1) return 2;
    QNetworkAccessManager network;
    keefi::ReleaseUpdater update(args[1], build, keefi::updatePlatform(), &network, keefi::releaseUpdatePublicKey());
    QObject::connect(&update, &keefi::ReleaseUpdater::changed, &app, [&] {
        using State = keefi::ReleaseUpdater::State;
        if (update.state() == State::Available) update.download();
        else if (update.state() == State::Failed || update.state() == State::Current) {
            std::fprintf(stderr, "%s\n", update.status().toUtf8().constData());
            app.exit(1);
        }
    });
    QObject::connect(&update, &keefi::ReleaseUpdater::archiveReady, &app, [&] {
        const auto asset = update.asset();
        QString error;
        if (!keefi::verifyUpdateArchive(update.archivePath(), asset, &error)) { app.exit(1); return; }
        if (args.size() == 4 && !QFile::copy(update.archivePath(), args[3])) { app.exit(4); return; }
        std::printf("%s %s %lld %s\n", asset.tag.toUtf8().constData(), asset.name.toUtf8().constData(),
                    static_cast<long long>(asset.size), asset.sha256Hex.constData());
        app.exit(0);
    });
    QTimer::singleShot(0, &update, &keefi::ReleaseUpdater::check);
    QTimer::singleShot(420000, &app, [&app] { app.exit(3); });
    return app.exec();
}
