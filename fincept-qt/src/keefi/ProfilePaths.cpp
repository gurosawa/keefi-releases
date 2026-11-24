#include "ProfilePaths.h"
#include "ProfileMigrationPrivate.h"

namespace keefi {
using namespace profile_detail;

DevProfilePaths devProfilePaths(const QString& base) {
    const auto directory = normalized(base);
    if (directory.isEmpty()) return {};
    return {directory, QDir(directory).filePath(legacy::ProfileRelativePath),
            QDir(directory).filePath(QStringLiteral("KEEFI/KEEFI Dev")),
            QDir(directory).filePath(QStringLiteral("KEEFI/.keefi-dev-profile-copy"))};
}

ProfileSettingsIdentity profileSettingsIdentity(const QString& directory) {
    const auto path = normalized(directory);
    QString error;
    if (!safeDirectory(path, true, false, nullptr, &error)) return {};
    const QStringList names{QStringLiteral("KEEFI/KEEFI Dev.ini"), legacy::AppSettings,
                            QStringLiteral("KEEFI.ini"), legacy::OrganizationSettings};
    bool found[4]{};
    for (int i = 0; i < names.size(); ++i) {
        const auto file = QDir(path).filePath(names.at(i));
#ifdef Q_OS_UNIX
        bool missing = false;
        auto parent = openDirectory(QFileInfo(file).absolutePath(), &missing, &error);
        if (!parent) { if (missing) continue; return {}; }
        struct stat st{};
        if (::fstatat(parent.get(), QFile::encodeName(QFileInfo(file).fileName()).constData(), &st,
                      AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) continue;
            return {};
        }
        if (!safeNode(st, false)) return {};
#else
        const QFileInfo info(file);
        if (!info.exists() && !info.isSymLink()) continue;
        if (!info.isFile() || info.isSymLink()) return {};
#endif
        found[i] = true;
    }
    if (found[0]) return currentSettings();
    if (found[1]) return legacySettings();
    if (found[2]) return currentSettings();
    if (found[3]) return legacySettings();
    return currentSettings();
}

ProfileMigrationPlan planDevProfile(const std::optional<QString>& explicitDirectory,
                                    const QString& base) {
    ProfileMigrationPlan result;
    // This branch must not inspect any default profile or transaction directory.
    if (explicitDirectory.has_value()) {
        result.selectedDirectory = normalized(*explicitDirectory);
        if (!safeDirectory(result.selectedDirectory, true, false, nullptr, &result.error)) return result;
        result.settings = profileSettingsIdentity(result.selectedDirectory);
        if (result.settings.organization.isEmpty()) {
            result.error = QStringLiteral("지정한 프로필의 설정 경로가 안전하지 않습니다.");
            return result;
        }
        result.action = DevProfileAction::UseExplicit;
        return result;
    }
    result.paths = devProfilePaths(base);
    if (result.paths.baseDirectory.isEmpty()) {
        result.error = QStringLiteral("프로필의 기반 폴더를 정할 수 없습니다.");
        return result;
    }
    bool current = false;
    if (!safeDirectory(result.paths.currentDirectory, true, false, &current, &result.error)) return result;
    const auto markerPath = QDir(result.paths.currentDirectory).filePath(MarkerName);
    if (current && pathExists(markerPath)) {
        QJsonObject marker;
        if (!readJson(markerPath, &marker, &result.error) || !validMarker(marker, result.paths, &result.error))
            return result;
        // A completed copy is independent of the continued presence of the legacy folder.
        result.action = DevProfileAction::UseCurrent;
        result.selectedDirectory = result.paths.currentDirectory;
        result.settings = currentSettings();
        return result;
    }
    bool legacy = false;
    if (!safeDirectory(result.paths.legacyDirectory, true, false, &legacy, &result.error)) return result;
    if (legacy) {
        result.action = current ? DevProfileAction::ChooseExisting : DevProfileAction::OfferCopy;
        result.selectedDirectory = result.paths.legacyDirectory;
        result.settings = legacySettings();
    } else {
        result.action = current ? DevProfileAction::UseCurrent : DevProfileAction::CreateCurrent;
        result.selectedDirectory = result.paths.currentDirectory;
        result.settings = currentSettings();
    }
    return result;
}
} // namespace keefi
