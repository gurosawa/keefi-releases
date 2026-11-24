#include "UpdateInstall.h"
#include "WindowsPrivateStorage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <qt_windows.h>

namespace keefi {
bool startUpdateInstaller(const QString& archive, const QByteArray& sha256Hex, const QString& version,
                          int build, const QStringList& restartArgs, QString* error) {
    auto reject = [&](const QString& message) { if (error) *error = message; return false; };
    if (!restartArgs.isEmpty() && (restartArgs.size() != 2 || restartArgs[0] != "--profile-dir" ||
        !QDir::isAbsolutePath(restartArgs[1]))) return reject(QStringLiteral("프로필 경로를 확인할 수 없습니다."));

    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // GetTempPath gives this user's normal temporary directory. The directory
    // containing the copied executable has a protected, current-user-only ACL.
    const QString scratch = QDir(QDir::tempPath()).filePath("keefi-updater-" + id);
    if (!createPrivateWindowsDirectory(scratch, error)) return false;
    const QString helper = QDir(scratch).filePath("KEEFIUpdate.exe");
    const QString installed = QCoreApplication::applicationFilePath();
    if (!QFile::copy(QDir(QFileInfo(installed).absolutePath()).filePath("KEEFIUpdate.exe"), helper)) {
        QDir().rmdir(scratch);
        return reject(QStringLiteral("업데이트 도구를 준비하지 못했습니다."));
    }
    const QString eventName = QStringLiteral("Local\\KEEFI-Update-") + id;
    const auto eventWide = eventName.toStdWString();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventWide.c_str());
    HANDLE commit = CreateEventW(nullptr, TRUE, FALSE, (eventName + "-commit").toStdWString().c_str());
    QProcess child;
    child.setProgram(helper);
    child.setWorkingDirectory(scratch);
    QStringList args{QString::number(QCoreApplication::applicationPid()), eventName, installed, archive,
        QString::fromLatin1(sha256Hex), version, QString::number(build)};
    if (!restartArgs.isEmpty()) args.append(restartArgs.last());
    child.setArguments(args);
    qint64 pid = 0;
    const bool started = ready && commit && child.startDetached(&pid);
    if (!started) {
        if (ready) CloseHandle(ready);
        if (commit) CloseHandle(commit);
        QFile::remove(helper); QDir().rmdir(scratch);
        return reject(QStringLiteral("Windows에서 업데이트 도구를 시작하지 못했습니다."));
    }
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    HANDLE handles[]{ready, process};
    const DWORD state = WaitForMultipleObjects(process ? 2 : 1, handles, FALSE, 4000);
    // A live child never installs until this exact app process has exited.
    DWORD result = 0;
    if (process) { GetExitCodeProcess(process, &result); CloseHandle(process); }
    const bool accepted = state == WAIT_OBJECT_0 && SetEvent(commit);
    CloseHandle(ready); CloseHandle(commit);
    if (accepted) return true;
    return reject(QStringLiteral("업데이트 파일 또는 앱 설치 폴더를 확인할 수 없습니다 (%1). "
                                 "현재 버전은 그대로 사용할 수 있습니다.").arg(result));
}
}
