#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <optional>

namespace keefi {

// Resolve only: do not open profile contents or create, copy, or move anything.
// The caller still validates the selected path before opening the vault.
inline QString resolveProfileDirectory(const std::optional<QString>& explicitDirectory,
                                       const QString& appLocalDataDirectory,
                                       const QString& legacyG2Directory = {},
                                       bool useLegacyG2Directory = true) {
    if (explicitDirectory.has_value())
        return QDir::cleanPath(QFileInfo(*explicitDirectory).absoluteFilePath());
    if (useLegacyG2Directory && !legacyG2Directory.isEmpty() && QFileInfo::exists(legacyG2Directory))
        return legacyG2Directory;
    return appLocalDataDirectory;
}

struct ProfileSettingsIdentity {
    QString organization;
    QString application;
};

struct DevProfilePaths {
    QString baseDirectory;
    QString legacyDirectory;
    QString currentDirectory;
    QString transactionDirectory;
};

enum class DevProfileAction {
    UseExplicit, UseCurrent, CreateCurrent, OfferCopy, ChooseExisting, Unavailable
};

struct ProfileMigrationPlan {
    DevProfileAction action = DevProfileAction::Unavailable;
    DevProfilePaths paths;
    QString selectedDirectory;
    ProfileSettingsIdentity settings;
    QString error;
};

enum class ProfileMigrationChoice { Cancel, Copy, Later, UseLegacy, UseCurrent };

DevProfilePaths devProfilePaths(const QString& base);
ProfileMigrationPlan planDevProfile(const std::optional<QString>& explicitDirectory,
                                   const QString& base);
ProfileSettingsIdentity profileSettingsIdentity(const QString& directory);

} // namespace keefi
