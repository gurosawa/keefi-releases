#pragma once
#include <QByteArray>
#include <QStringList>
namespace keefi {
// Separate the calling Qt process from the process handle the detached helper
// pins. The package test exercises this same handoff with a genuine older EXE.
bool startWindowsUpdateForProcess(qint64 oldPid, const QString& installed, const QString& archive,
                                  const QByteArray& sha256Hex, const QString& version, int build,
                                  const QStringList& restartArgs, qint64* installerPid, QString* error);
}
