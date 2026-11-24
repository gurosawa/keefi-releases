#include "keefi/update/WindowsUpdateLaunch.h"
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList a=app.arguments();
    if(a.size()!=8) return 2;
    qint64 installer=0;
    QString error;
    if (!keefi::startWindowsUpdateForProcess(a[1].toLongLong(),a[2],a[3],a[4].toLatin1(),a[5],a[6].toInt(),
        {"--profile-dir",a[7]},&installer,&error)) {
        QTextStream(stderr)<<error<<Qt::endl;
        return 1;
    }
    QTextStream(stdout)<<installer<<Qt::endl;
    return 0;
}
