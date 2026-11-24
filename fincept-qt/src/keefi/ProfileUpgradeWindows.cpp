#include "ProfileUpgradePrivate.h"
#include "LegacyStorageCodecPrivate.h"
#include "Vault.h"
#include "WindowsPrivateStorage.h"

#include <QTemporaryDir>
#include <qt_windows.h>
#include <io.h>

namespace keefi {
namespace {
using namespace profile_detail;
using namespace profile_upgrade_detail;

// Disallow deletion/renaming while the plan and the owner's ACL are verified.
class PinnedProfilePath {
public:
    ~PinnedProfilePath() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    PinnedProfilePath() = default;
    PinnedProfilePath(const PinnedProfilePath&) = delete;
    PinnedProfilePath& operator=(const PinnedProfilePath&) = delete;

    bool open(const QString& path, bool directory, QString* error) {
        const auto native = QDir::toNativeSeparators(path).toStdWString();
        handle_ = CreateFileW(native.c_str(), directory ? FILE_LIST_DIRECTORY : GENERIC_READ,
            directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle_, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory
            || (!directory && info.nNumberOfLinks != 1))
            return fail(error, QStringLiteral("프로필 경로의 파일 종류와 링크를 확인할 수 없습니다."));
        return storage_detail::privateStoragePath(path, directory, error);
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
}

ProfileReady existingProfileReady(const ProfileUpgradePlan& plan, QString* error) {
    clear(error);
    if (plan.action != ProfileUpgradeAction::UseCurrent) {
        fail(error, plan.error.isEmpty() ? QStringLiteral("완료된 새 프로필을 선택하지 않았습니다.") : plan.error);
        return {};
    }
    PinnedProfilePath root, marker;
    if (!root.open(plan.targetDirectory, true, error)
        || !marker.open(QDir(plan.targetDirectory).filePath(ActiveMarkerName), false, error)) return {};
    const auto current = revalidatedPlan(plan, error);
    if (current.action != ProfileUpgradeAction::UseCurrent) return {};
    return {current.targetDirectory, current.identityScheme, {}};
}

ProfileReady createFreshProfile(const ProfileUpgradePlan& plan, const QString& version, QString* error) {
    const auto current = revalidatedPlan(plan, error);
    if (current.action != ProfileUpgradeAction::CreateCurrent || version.trimmed().isEmpty()) {
        if (!error || error->isEmpty()) fail(error, QStringLiteral("새 빈 프로필을 만들 계획이 아닙니다."));
        return {};
    }
    if (!Vault::prepareProfileDirectory(current.targetDirectory, error)) return {};
    PinnedProfilePath root;
    if (!root.open(current.targetDirectory, true, error)) return {};

    // Keep incomplete writes outside the profile so an interrupted first launch
    // can retry. The final move on the same volume never replaces another marker.
    const QDir parent(QFileInfo(current.targetDirectory).absolutePath());
    QTemporaryDir scratch(parent.filePath(QStringLiteral(".keefi-init-XXXXXX")));
    if (!scratch.isValid()) {
        fail(error, QStringLiteral("프로필 초기화용 임시 폴더를 만들 수 없습니다."));
        return {};
    }
    const auto stage = scratch.filePath(QStringLiteral("private"));
    if (!Vault::prepareProfileDirectory(stage, error)) return {};
    const auto stagedMarker = QDir(stage).filePath(ActiveMarkerName);
    // A fresh profile's source manifest is the empty list of files.
    const auto digest = QCryptographicHash::hash("[]", QCryptographicHash::Sha256).toHex();
    const QJsonObject record{
        {"format", ActiveMarkerFormat}, {"identity_scheme", "keefi-v1"}, {"version", version},
        {"verified", true}, {"source_kind", "fresh"}, {"source_manifest_sha256", QString::fromLatin1(digest)},
        {"completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    QFile file(stagedMarker);
    if (!file.open(QIODevice::ReadWrite | QIODevice::NewOnly) || file.write(bytes) != bytes.size()
        || !file.flush() || ::_commit(file.handle()) != 0 || !file.seek(0) || file.readAll() != bytes) {
        fail(error, QStringLiteral("프로필 초기화 기록을 저장하고 확인할 수 없습니다."));
        return {};
    }
    file.close();
    if (!privateWindowsPath(stagedMarker, true, error)
        || revalidatedPlan(current, error).action != ProfileUpgradeAction::CreateCurrent) return {};
    const auto from = QDir::toNativeSeparators(stagedMarker).toStdWString();
    const auto to = QDir::toNativeSeparators(QDir(current.targetDirectory).filePath(ActiveMarkerName)).toStdWString();
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        fail(error, QStringLiteral("프로필 초기화 기록을 적용할 수 없습니다. 기존 파일을 보존했습니다."));
        return {};
    }
    return existingProfileReady(planProfileUpgrade(current.explicitDirectory, current.paths.baseDirectory), error);
}
} // namespace keefi
