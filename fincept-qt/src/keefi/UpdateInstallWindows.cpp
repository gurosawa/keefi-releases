#include "UpdateInstall.h"
#include "update/WindowsUpdateLaunch.h"
#include "WindowsPrivateStorage.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <qt_windows.h>

namespace keefi {
bool startWindowsUpdateForProcess(qint64 oldPid, const QString& installed, const QString& archive,
                                  const QByteArray& sha256Hex, const QString& version, int build,
                                  const QStringList& restartArgs, qint64* installerPid, QString* error) {
    auto reject = [&](const QString& message) { if (error) *error = message; return false; };
    if (!restartArgs.isEmpty() && (restartArgs.size() != 2 || restartArgs[0] != "--profile-dir" ||
        !QDir::isAbsolutePath(restartArgs[1]))) return reject(QStringLiteral("프로필 경로를 확인할 수 없습니다."));

    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto appDir = QFileInfo(installed).absolutePath();
    const auto defaultProfile = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                                    .filePath("KEEFI/Profiles/Local");
    const auto profile = restartArgs.isEmpty() ? defaultProfile : restartArgs.last();
    const auto comparable = [](const QString& path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded() + '/'; };
    if (comparable(appDir).startsWith(comparable(profile)) || comparable(profile).startsWith(comparable(appDir)))
        return reject(QStringLiteral("앱과 금고를 서로 다른 폴더에 두고 업데이트해 주세요."));

    // A content-addressed helper lives outside the replaceable application.
    // Retrying the same update reuses it; old unlocked helper versions are
    // removed without touching any files outside our protected cache.
    const QString scratch = QDir(QDir::tempPath()).filePath("KEEFI-updater-tools");
    if (!createPrivateWindowsDirectory(scratch, error)) return false;
    const DWORD attrs = GetFileAttributesW(QDir::toNativeSeparators(scratch).toStdWString().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !privateWindowsPath(scratch, false, error))
        return reject(QStringLiteral("업데이트 도구 폴더의 소유권을 확인하지 못했습니다."));
    const auto source = QDir(appDir).filePath("KEEFIUpdate.exe");
    auto fileHash = [](const QString& path) {
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result().toHex() : QByteArray{};
    };
    const QByteArray digest = fileHash(source);
    if (digest.size() != 64) return reject(QStringLiteral("설치된 업데이트 도구를 확인하지 못했습니다."));
    const auto helperDir = QDir(scratch);
    const QString helper = helperDir.filePath("KEEFIUpdate-" + QString::fromLatin1(digest) + ".exe");
    if (!QFileInfo::exists(helper)) QFile::copy(source, helper);
    if (fileHash(helper) != digest)
        return reject(QStringLiteral("업데이트 도구를 준비하지 못했습니다."));
    for (const auto& old : helperDir.entryInfoList({"KEEFIUpdate-*.exe"}, QDir::Files | QDir::NoSymLinks)) {
        if (old.absoluteFilePath() != helper && old.fileName().size() == QFileInfo(helper).fileName().size())
            QFile::remove(old.absoluteFilePath());
    }
    const QString eventName = QStringLiteral("Local\\KEEFI-Update-") + id;
    const auto eventWide = eventName.toStdWString();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventWide.c_str());
    HANDLE commit = CreateEventW(nullptr, TRUE, FALSE, (eventName + "-commit").toStdWString().c_str());
    QProcess child;
    child.setProgram(helper);
    child.setWorkingDirectory(scratch);
    QStringList args{QString::number(oldPid), eventName, installed, archive,
        QString::fromLatin1(sha256Hex), version, QString::number(build)};
    if (!restartArgs.isEmpty()) args.append(restartArgs.last());
    child.setArguments(args);
    qint64 pid = 0;
    const bool started = ready && commit && child.startDetached(&pid);
    if (!started) {
        if (ready) CloseHandle(ready);
        if (commit) CloseHandle(commit);
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
    if (accepted) { if (installerPid) *installerPid = pid; return true; }
    return reject(QStringLiteral("업데이트 파일 또는 앱 설치 폴더를 확인할 수 없습니다 (%1). "
                                 "현재 버전은 그대로 사용할 수 있습니다.").arg(result));
}

bool startUpdateInstaller(const QString& archive, const QByteArray& sha256Hex, const QString& version,
                          int build, const QStringList& restartArgs, QString* error) {
    return startWindowsUpdateForProcess(QCoreApplication::applicationPid(), QCoreApplication::applicationFilePath(),
                                        archive, sha256Hex, version, build, restartArgs, nullptr, error);
}
}
