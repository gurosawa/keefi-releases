#pragma once
#include <QByteArray>
#include <QStringList>

namespace keefi {
// The caller authenticated the manifest, verified and closed the ZIP, and locked
// the vault. The detached platform helper waits for this process to exit.
bool startUpdateInstaller(const QString& archive, const QByteArray& sha256Hex, const QString& version,
                          int build, const QStringList& restartArgs, QString* error);
}
