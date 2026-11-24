#include "UpdateInstall.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <unistd.h>

static void registerUpdateResources() { Q_INIT_RESOURCE(keefi_update_mac); }

namespace keefi {

bool startUpdateInstaller(const QString& archive, const QByteArray& sha256Hex, const QString& version,
                          int build, const QStringList& restartArgs, QString* error) {
    auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    const auto binary = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString suffix = QStringLiteral("/KEEFI.app/Contents/MacOS/KEEFI");
    if (!binary.endsWith(suffix))
        return fail(QStringLiteral("설치된 KEEFI.app에서 업데이트를 실행해 주세요."));
    const auto bundle = binary.left(binary.size() - QStringLiteral("/Contents/MacOS/KEEFI").size());
    const auto parent = QFileInfo(bundle).absolutePath();
    if (parent.contains(QStringLiteral("/AppTranslocation/")))
        return fail(QStringLiteral("KEEFI.app을 응용 프로그램 폴더로 옮긴 후 다시 실행해 주세요."));
    const bool writable = ::access(QFile::encodeName(parent).constData(), W_OK) == 0;
    if (!writable && bundle != QStringLiteral("/Applications/KEEFI.app"))
        return fail(QStringLiteral("설치 폴더의 쓰기 권한을 확인해 주세요."));
    if ((!restartArgs.isEmpty() && (restartArgs.size() != 2 || restartArgs.first() != "--profile-dir"
                                     || !QDir::isAbsolutePath(restartArgs.last())))
        || build <= 0 || !QRegularExpression("^[0-9]+\\.[0-9]+\\.[0-9]+$").match(version).hasMatch()
        || !QRegularExpression("^[a-f0-9]{64}$").match(QString::fromLatin1(sha256Hex)).hasMatch())
        return fail(QStringLiteral("업데이트 버전 또는 재시작 정보를 확인할 수 없습니다."));
    if (!restartArgs.isEmpty()) {
        const QFileInfo profile(restartArgs.last());
        const auto resolved = profile.canonicalFilePath().normalized(QString::NormalizationForm_C);
        const auto bundlePath = bundle.normalized(QString::NormalizationForm_C);
        if (!profile.isDir() || resolved.isEmpty() || resolved.compare(bundlePath, Qt::CaseInsensitive) == 0
            || resolved.startsWith(bundlePath + '/', Qt::CaseInsensitive))
            return fail(QStringLiteral("금고는 앱 번들 밖의 프로필 폴더에 보관한 뒤 업데이트해 주세요."));
    }
    const QFileInfo zip(archive);
    if (!zip.isFile() || zip.isSymLink() || zip.canonicalFilePath().isEmpty())
        return fail(QStringLiteral("검증된 업데이트 파일을 열 수 없습니다."));

    const QFileInfo downloadDirectory(zip.absolutePath());
    if (!QRegularExpression("^KEEFI-update-[A-Za-z0-9]{6}$").match(downloadDirectory.fileName()).hasMatch()
        || downloadDirectory.dir().canonicalPath() != QDir(QDir::tempPath()).canonicalPath())
        return fail(QStringLiteral("업데이트 파일의 임시 폴더를 확인할 수 없습니다."));
    registerUpdateResources();
    QFile driver(":/keefi/update/restart.applescript"), transaction(":/keefi/update/replace.sh");
    if (!driver.open(QIODevice::ReadOnly) || !transaction.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("업데이트 설치 도구를 열 수 없습니다."));
    QStringList args{QStringLiteral("-e"), QString::fromUtf8(driver.readAll()),
                     QString::fromUtf8(transaction.readAll()), QString::number(QCoreApplication::applicationPid()),
                     zip.canonicalFilePath(), QString::fromLatin1(sha256Hex), version, QString::number(build), bundle,
                     writable ? QStringLiteral("user") : QStringLiteral("admin"), QString::number(getuid())};
    args.append(restartArgs);
    if (!QProcess::startDetached(QStringLiteral("/usr/bin/osascript"), args, QStringLiteral("/")))
        return fail(QStringLiteral("macOS 업데이트 도구를 시작할 수 없습니다."));
    return true;
}
} // namespace keefi
