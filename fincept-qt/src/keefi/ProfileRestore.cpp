#include "ProfileRestore.h"
#include "ProfileMigrationPrivate.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace keefi {
ProfileRestorePaths profileRestorePaths(const QString& currentDirectory) {
    const auto current = profile_detail::normalized(currentDirectory);
    if (current.isEmpty()) return {};
    const auto digest = QCryptographicHash::hash(current.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
    return {current, QDir(QFileInfo(current).absolutePath()).filePath(
        QStringLiteral(".keefi-profile-restore-") + QString::fromLatin1(digest))};
}

ProfileReady completePendingProfileRestore(const ProfileReady& selected,
                                           const QString& version, QString* error) {
    profile_detail::clear(error);
    const auto paths = profileRestorePaths(selected.directory);
    if (paths.currentDirectory.isEmpty()) {
        profile_detail::fail(error, QStringLiteral("복원을 마칠 프로필 경로가 비어 있습니다."));
        return {};
    }
    auto checked = existingProfileReady(planProfileUpgrade(paths.currentDirectory, {}), error);
    if (checked.directory != selected.directory || checked.identityScheme != selected.identityScheme) {
        if (!error || error->isEmpty())
            profile_detail::fail(error, QStringLiteral("선택한 프로필의 식별 방식이 바뀌었습니다."));
        return {};
    }
    if (!selected.warning.isEmpty() && checked.warning != selected.warning) {
        if (!checked.warning.isEmpty()) checked.warning += '\n';
        checked.warning += selected.warning;
    }
    if (!profile_detail::pathExists(paths.transactionDirectory)) return checked;
    auto completion = ProfileRestoreCompletion::begin(checked, version, error);
    if (!completion) return {};
    if (!completion->refreshPublicSettings(error) || !completion->verify(error) || !completion->publish(error)) {
        const auto committed = completion->ready();
        if (!committed.directory.isEmpty()) profile_detail::clear(error);
        return committed;
    }
    completion->archive(error);
    const auto ready = completion->ready();
    if (!ready.directory.isEmpty()) profile_detail::clear(error);
    return ready;
}
// Descriptor-bound preparation and completion share the private filesystem
// implementation in ProfileMigration.cpp.
} // namespace keefi
