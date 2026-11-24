#pragma once

#include "keefi/Vault.h"
#include <QFile>
#include <QTest>

namespace keefi::test {
// The fixture was written by Dev 110 using synthetic credentials for two live brokers.
inline bool openSuspendedBrokerFixture(Vault& vault) {
    const auto source = QFINDTESTDATA("suspended-brokers/credentials.keefi-vault");
    return !source.isEmpty() && QFile::copy(source, vault.filePath()) &&
           QFile::setPermissions(vault.filePath(), QFile::ReadOwner | QFile::WriteOwner) &&
           vault.unlock(QStringLiteral("Synthetic suspended brokers 2026!"));
}
inline bool keepSingleSuspendedBroker(Vault& vault, const QString& provider, const QString& password) {
    if (!openSuspendedBrokerFixture(vault)) return false;
    for (const auto& account : vault.accounts())
        if (account.provider != provider && !vault.removeAccount(account.id)) return false;
    return vault.accounts().size() == 1 && vault.accounts().first().provider == provider &&
           vault.changePassword(QStringLiteral("Synthetic suspended brokers 2026!"), password);
}
} // namespace keefi::test
