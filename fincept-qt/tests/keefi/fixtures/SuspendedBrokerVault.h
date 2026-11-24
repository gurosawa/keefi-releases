#pragma once

#include "keefi/Vault.h"
#include <QFile>
#include <QFileInfo>
#include <QTest>
#ifdef Q_OS_WIN
#include "keefi/WindowsPrivateStorage.h"
#endif

namespace keefi::test {
// The fixture was written by Dev 110 using synthetic credentials for two live brokers.
inline bool openSuspendedBrokerFixture(Vault& vault) {
    QFile source(QFINDTESTDATA("suspended-brokers/credentials.keefi-vault"));
    if (!source.open(QIODevice::ReadOnly)
        || !Vault::prepareProfileDirectory(QFileInfo(vault.filePath()).absolutePath())) return false;
    // Create under the new profile so the fixture inherits its private Windows ACL.
    // A native copy can also copy the checked-out fixture's security descriptor.
    const auto bytes = source.readAll();
    QFile copy(vault.filePath());
    if (source.error() != QFileDevice::NoError || bytes.isEmpty()
        || !copy.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !copy.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || copy.write(bytes) != bytes.size() || !copy.flush()) return false;
    copy.close();
#ifdef Q_OS_WIN
    if (!privateWindowsPath(vault.filePath(), true)) return false;
#endif
    return vault.unlock(QStringLiteral("Synthetic suspended brokers 2026!"));
}
inline bool keepSingleSuspendedBroker(Vault& vault, const QString& provider, const QString& password) {
    if (!openSuspendedBrokerFixture(vault)) return false;
    for (const auto& account : vault.accounts())
        if (account.provider != provider && !vault.removeAccount(account.id)) return false;
    return vault.accounts().size() == 1 && vault.accounts().first().provider == provider &&
           vault.changePassword(QStringLiteral("Synthetic suspended brokers 2026!"), password);
}
} // namespace keefi::test
