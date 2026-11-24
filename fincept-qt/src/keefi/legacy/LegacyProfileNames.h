#pragma once

// Import boundary: these are historical product-owned names. New profiles never
// write them; keep them exact so existing copies can be authenticated and moved.
#include <QString>
#include <QStringList>

namespace keefi::legacy {
inline const QString Organization = QStringLiteral("FINIX");
inline const QString Application = QStringLiteral("FINIX Dev");
inline const QString ProfileRelativePath = QStringLiteral("FINIX/FINIX Dev");
inline const QString SettingsDirectory = QStringLiteral("FINIX");
inline const QString AppSettings = QStringLiteral("FINIX/FINIX Dev.ini");
inline const QString ProductionAppSettings = QStringLiteral("FINIX/FINIX.ini");
inline const QString OrganizationSettings = QStringLiteral("FINIX.ini");
inline const QString VaultFile = QStringLiteral("credentials.finix-vault");
inline const QStringList StorageFiles{
    VaultFile, QStringLiteral("assets.finix"), QStringLiteral("assets-current.finix"),
    QStringLiteral("swaps.finix"), QStringLiteral("prices.finix"),
    QStringLiteral("imported-records.finix"), QStringLiteral("prices.finix.exit")};
inline const QStringList WriterLocks{
    QStringLiteral("credentials.finix-vault.lock"), QStringLiteral("assets.finix.lock"),
    QStringLiteral("assets-current.finix.lock"), QStringLiteral("swaps.finix.lock"),
    QStringLiteral("prices.finix.lock"), QStringLiteral("imported-records.finix.lock"),
    QStringLiteral("FINIX/FINIX Dev.ini.lock"), QStringLiteral("FINIX/FINIX.ini.lock"),
    QStringLiteral("FINIX.ini.lock")};
inline const QStringList TemporaryPrefixes{
    QStringLiteral(".finix-vault-"), QStringLiteral(".finix-restore-"),
    QStringLiteral(".finix-assets-"), QStringLiteral(".finix-prices-"),
    QStringLiteral(".finix-record-transfer-")};
} // namespace finix::legacy
