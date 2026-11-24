#include "ProfileUpgrade.h"
#include "ProfileUpgradePrivate.h"
#include <QDir>
#include <QFileInfo>
#ifdef Q_OS_UNIX
#include <dirent.h>
#endif

namespace keefi {
namespace {
using namespace profile_detail;
using namespace profile_upgrade_detail;

ProfileUpgradePlan unavailable(ProfileUpgradePlan plan, const QString& message) {
    plan.action = ProfileUpgradeAction::Unavailable;
    plan.error = message;
    return plan;
}
bool directoryContents(const QString& path, QStringList* names, QString* error) {
    names->clear();
#ifdef Q_OS_UNIX
    auto directory = openDirectory(path, nullptr, error);
    if (!directory) return false;
    Fd copy(::dup(directory.get()));
    if (!copy) return fail(error, QStringLiteral("프로필 파일 목록을 확인할 수 없습니다."));
    DIR* stream = ::fdopendir(copy.get());
    if (!stream) return fail(error, QStringLiteral("프로필 파일 목록을 열 수 없습니다."));
    copy.release();
    int readError = 0;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(stream);
        if (!entry) { readError = errno; break; }
        const QByteArray name(entry->d_name);
        if (name != "." && name != "..") names->append(QFile::decodeName(name));
    }
    const int closeError = ::closedir(stream);
    return (!readError && closeError == 0)
        || fail(error, QStringLiteral("프로필 파일 목록을 읽거나 닫을 수 없습니다."));
#else
    *names = QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    return true;
#endif
}
bool directoryEmpty(const QString& path, bool* empty, QString* error) {
    QStringList names;
    if (!directoryContents(path, &names, error)) return false;
    *empty = names.isEmpty();
    return true;
}
bool pendingFreshInitialization(const QString& path, const QString& control) {
#ifdef Q_OS_UNIX
    QString error;
    QStringList names;
    if (!directoryContents(path, &names, &error) || names != QStringList{QStringLiteral("instance.lock")}
        || !safeDirectory(control, false, true, nullptr, &error)) return false;
    auto directory = openDirectory(path, nullptr, &error);
    struct stat lock{};
    if (!directory || ::fstatat(directory.get(), "instance.lock", &lock, AT_SYMLINK_NOFOLLOW)
        || !S_ISREG(lock.st_mode) || lock.st_uid != ::geteuid() || lock.st_nlink != 2
        || (lock.st_mode & (S_ISUID | S_ISGID | S_ISVTX))) return false;
    QJsonObject owner;
    if (!readJson(QDir(control).filePath(QStringLiteral("owner.json")), &owner, &error)) return false;
    // This only permits entry into the existing recovery transaction. No file is
    // removed here; recoverControl still checks dead owner, inode/hash, native
    // flock, the stage proof and both hardlink paths before any mutation.
    return owner.value("format").toString() == OwnerFormat
        && owner.value("phase").toString() == QStringLiteral("locked")
        && owner.value("source").toString() == path && owner.value("target").toString() == path
        && !owner.value("version").toString().isEmpty() && !owner.value("nonce").toString().isEmpty()
        && owner.value("pid").toInteger(-1) > 0 && !owner.value("host").toString().isEmpty()
        && !owner.value("app").toString().isEmpty() && !owner.value("stage").toObject().isEmpty()
        && !owner.value("lock").toObject().isEmpty();
#else
    Q_UNUSED(path); Q_UNUSED(control);
    return false;
#endif
}
bool readActive(const QString& path, QString* scheme, QString* error) {
    QJsonObject marker;
    if (!readJson(QDir(path).filePath(ActiveMarkerName), &marker, error)
        || !activeMarker(marker, error)) return false;
    // A completion marker cannot make an unfinished mixed-format tree writable.
    for (const auto& name : legacy::StorageFiles)
        if (pathExists(QDir(path).filePath(name)))
            return fail(error, QStringLiteral("새 프로필에 변환되지 않은 옛 저장 파일이 남아 있습니다."));
    for (const auto& name : {legacy::SettingsDirectory, legacy::OrganizationSettings, MarkerName})
        if (pathExists(QDir(path).filePath(name)))
            return fail(error, QStringLiteral("새 프로필에 이전 단계의 설정 또는 표식이 남아 있습니다."));
    *scheme = marker.value("identity_scheme").toString();
    return true;
}
ProfileUpgradeSource source(ProfileUpgradeSourceKind kind, const QString& directory) {
    return {kind, directory,
            pathExists(QDir(directory).filePath(legacy::VaultFile))
            || pathExists(QDir(directory).filePath(CurrentVaultName))};
}
void choose(ProfileUpgradePlan& plan, const ProfileUpgradeSource& candidate) {
    plan.action = ProfileUpgradeAction::OfferUpgrade;
    plan.sourceKind = candidate.kind;
    plan.sourceDirectory = candidate.directory;
    plan.requiresPassword = candidate.requiresPassword;
    plan.identityScheme.clear();
    plan.error.clear();
}
QString explicitControl(const QString& target) {
    const auto digest = QCryptographicHash::hash(target.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(QFileInfo(target).absolutePath()).filePath(
        QStringLiteral(".keefi-profile-upgrade-") + QString::fromLatin1(digest.left(24)));
}
bool recognizedLegacy(const QString& directory) {
    for (const auto& name : legacy::StorageFiles)
        if (pathExists(QDir(directory).filePath(name))) return true;
    return pathExists(QDir(directory).filePath(legacy::AppSettings))
        || pathExists(QDir(directory).filePath(legacy::ProductionAppSettings))
        || pathExists(QDir(directory).filePath(legacy::OrganizationSettings))
        || pathExists(QDir(directory).filePath(QStringLiteral("KEEFI/KEEFI Dev.ini")))
        || pathExists(QDir(directory).filePath(QStringLiteral("KEEFI.ini")))
        || pathExists(QDir(directory).filePath(MarkerName));
}
} // namespace

ProfileUpgradePaths profileUpgradePaths(const QString& base) {
    const auto root = profile_detail::normalized(base);
    if (root.isEmpty()) return {};
    return {root, QDir(root).filePath(legacy::ProfileRelativePath),
            QDir(root).filePath(QStringLiteral("KEEFI/KEEFI Dev")),
            QDir(root).filePath(QStringLiteral("KEEFI/Profiles/Dev")),
            QDir(root).filePath(QStringLiteral("KEEFI/Profiles/.keefi-profile-upgrade-dev"))};
}

ProfileUpgradePlan planProfileUpgrade(const std::optional<QString>& explicitDirectory,
                                      const QString& base) {
    using namespace profile_detail;
    using namespace profile_upgrade_detail;
    ProfileUpgradePlan plan;
    plan.paths = profileUpgradePaths(base);
    plan.explicitDirectory = explicitDirectory;
    QString error;
    if (explicitDirectory) {
        // Explicit paths are authoritative. Do not inspect either default input,
        // even if the default base is inaccessible or is not a directory.
        const auto requested = normalized(*explicitDirectory);
        if (requested.isEmpty()) return unavailable(plan, QStringLiteral("명시한 프로필 경로가 비어 있습니다."));
        plan.targetDirectory = requested;
        plan.transactionDirectory = explicitControl(requested);
        bool exists = false;
        if (!safeDirectory(requested, true, true, &exists, &error)) return unavailable(plan, error);
        if (!exists) { plan.action = ProfileUpgradeAction::CreateCurrent; return plan; }
        if (pathExists(QDir(requested).filePath(ActiveMarkerName))) {
            if (!readActive(requested, &plan.identityScheme, &error)) return unavailable(plan, error);
            plan.action = ProfileUpgradeAction::UseCurrent;
            return plan;
        }
        bool empty = false;
        if (!directoryEmpty(requested, &empty, &error)) return unavailable(plan, error);
        if (empty || pendingFreshInitialization(requested, plan.transactionDirectory)) {
            plan.action = ProfileUpgradeAction::CreateCurrent;
            return plan;
        }
        if (!recognizedLegacy(requested))
            return unavailable(plan, QStringLiteral("명시한 폴더의 프로필 형식을 확인할 수 없습니다."));
        plan.targetDirectory = requested + QStringLiteral(".keefi");
        plan.transactionDirectory = explicitControl(plan.targetDirectory);
        bool targetExists = false;
        if (!safeDirectory(plan.targetDirectory, true, true, &targetExists, &error))
            return unavailable(plan, error);
        if (targetExists)
            return unavailable(plan, QStringLiteral("별도 전환 경로가 이미 있습니다. 그 경로를 직접 선택하거나 원본을 보존한 채 확인해 주세요."));
        plan.sources.append(source(ProfileUpgradeSourceKind::ExplicitLegacy, requested));
        choose(plan, plan.sources.front());
        return plan;
    }
    if (plan.paths.baseDirectory.isEmpty())
        return unavailable(plan, QStringLiteral("기본 프로필의 기반 경로가 비어 있습니다."));
    plan.targetDirectory = plan.paths.activeDirectory;
    plan.transactionDirectory = plan.paths.transactionDirectory;
    bool baseExists = false, activeExists = false;
    if (!safeDirectory(plan.paths.baseDirectory, true, false, &baseExists, &error)
        || !safeDirectory(plan.targetDirectory, true, true, &activeExists, &error))
        return unavailable(plan, error);
    if (activeExists) {
        if (!pathExists(QDir(plan.targetDirectory).filePath(ActiveMarkerName))) {
            bool empty = false;
            if (!directoryEmpty(plan.targetDirectory, &empty, &error)) return unavailable(plan, error);
            if (empty || pendingFreshInitialization(plan.targetDirectory, plan.transactionDirectory)) {
                plan.action = ProfileUpgradeAction::CreateCurrent;
                return plan;
            }
        }
        if (!readActive(plan.targetDirectory, &plan.identityScheme, &error))
            return unavailable(plan, error);
        plan.action = ProfileUpgradeAction::UseCurrent;
        return plan;
    }

    bool renamedExists = false;
    if (!safeDirectory(plan.paths.dev100Directory, true, true, &renamedExists, &error))
        return unavailable(plan, error);
    if (renamedExists) {
        const auto candidate = source(ProfileUpgradeSourceKind::Dev100Keefi, plan.paths.dev100Directory);
        const auto priorMarker = QDir(candidate.directory).filePath(MarkerName);
        if (pathExists(priorMarker)) {
            QJsonObject marker;
            const DevProfilePaths previous{plan.paths.baseDirectory, plan.paths.legacyOriginalDirectory,
                                           plan.paths.dev100Directory, {}};
            if (!readJson(priorMarker, &marker, &error) || !validMarker(marker, previous, &error))
                return unavailable(plan, error);
            // This marker proves the renamed location is the published copy.
            // Do not inspect the older input to keep using this later copy.
            plan.sources.append(candidate);
            choose(plan, candidate);
            return plan;
        }
        plan.sources.append(candidate);
    }
    bool legacyExists = false;
    if (!safeDirectory(plan.paths.legacyOriginalDirectory, true, true, &legacyExists, &error))
        return unavailable(plan, error);
    if (legacyExists)
        plan.sources.append(source(ProfileUpgradeSourceKind::LegacyOriginal, plan.paths.legacyOriginalDirectory));
    if (plan.sources.isEmpty()) {
        plan.action = ProfileUpgradeAction::CreateCurrent;
    } else if (plan.sources.size() == 1) {
        choose(plan, plan.sources.front());
    } else {
        plan.action = ProfileUpgradeAction::ChooseSource;
    }
    return plan;
}

namespace profile_upgrade_detail {
ProfileUpgradePlan revalidatedPlan(const ProfileUpgradePlan& plan, QString* error) {
    profile_detail::clear(error);
    auto current = planProfileUpgrade(plan.explicitDirectory, plan.paths.baseDirectory);
    if (!sameUpgradePaths(plan.paths, current.paths)
        || plan.targetDirectory != current.targetDirectory
        || plan.transactionDirectory != current.transactionDirectory
        || current.action == ProfileUpgradeAction::Unavailable) {
        profile_detail::fail(error, current.error.isEmpty()
            ? QStringLiteral("선택한 프로필 경로 또는 상태가 바뀌었습니다.") : current.error);
        return {};
    }
    if (plan.action == ProfileUpgradeAction::OfferUpgrade) {
        bool found = false;
        for (const auto& candidate : current.sources) {
            if (candidate.kind == plan.sourceKind && candidate.directory == plan.sourceDirectory) {
                choose(current, candidate); found = true; break;
            }
        }
        if (!found) {
            profile_detail::fail(error, QStringLiteral("선택한 원본 프로필을 다시 확인할 수 없습니다."));
            return {};
        }
    } else if (plan.action != current.action) {
        profile_detail::fail(error, QStringLiteral("선택한 프로필 상태가 바뀌었습니다."));
        return {};
    }
    return current;
}
} // namespace profile_upgrade_detail

ProfileUpgradePlan selectProfileUpgradeSource(const ProfileUpgradePlan& plan,
                                              ProfileUpgradeSourceKind kind, QString* error) {
    auto current = profile_upgrade_detail::revalidatedPlan(plan, error);
    if (current.action != ProfileUpgradeAction::ChooseSource
        && current.action != ProfileUpgradeAction::OfferUpgrade) return {};
    for (const auto& candidate : current.sources) {
        if (candidate.kind == kind) { choose(current, candidate); return current; }
    }
    profile_detail::fail(error, QStringLiteral("선택한 원본이 현재 후보에 없습니다."));
    return {};
}

// Session and fresh-profile mutation live beside the shared descriptor-based
// migration engine in ProfileMigration.cpp. No ordinary vault is opened here.
} // namespace keefi
