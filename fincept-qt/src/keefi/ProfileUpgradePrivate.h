#pragma once

#include "ProfileUpgrade.h"
#include "ProfileMigrationPrivate.h"
#include "legacy/LegacyProfileNames.h"

namespace keefi::profile_upgrade_detail {
inline const QString ActiveMarkerName = QStringLiteral(".keefi-profile.json");
inline const QString ActiveMarkerFormat = QStringLiteral("keefi-profile/2");
inline const QString CurrentVaultName = QStringLiteral("credentials.keefi-vault");
inline QString sourceName(ProfileUpgradeSourceKind kind) {
    switch (kind) {
    case ProfileUpgradeSourceKind::LegacyOriginal: return QStringLiteral("legacy-dev");
    case ProfileUpgradeSourceKind::Dev100Keefi: return QStringLiteral("renamed-dev");
    case ProfileUpgradeSourceKind::ExplicitLegacy: return QStringLiteral("explicit-import");
    case ProfileUpgradeSourceKind::None: return QStringLiteral("fresh");
    }
    return {};
}
inline bool supportedScheme(const QString& scheme) {
    return scheme == QStringLiteral("legacy-v1") || scheme == QStringLiteral("keefi-v1");
}
inline bool activeMarker(const QJsonObject& marker, QString* error) {
    const auto completed = QDateTime::fromString(marker.value("completed_at_utc").toString(), Qt::ISODateWithMs);
    const auto source = marker.value("source_kind").toString();
    if (marker.value("format").toString() != ActiveMarkerFormat
        || !supportedScheme(marker.value("identity_scheme").toString())
        || !marker.value("verified").isBool() || !marker.value("verified").toBool()
        || marker.value("version").toString().isEmpty()
        || (source != QStringLiteral("fresh") && source != QStringLiteral("legacy-dev")
            && source != QStringLiteral("renamed-dev") && source != QStringLiteral("explicit-import")
            && source != QStringLiteral("backup-restore"))
        || (source == QStringLiteral("fresh")
            && marker.value("identity_scheme").toString() != QStringLiteral("keefi-v1"))
        || !completed.isValid() || completed.offsetFromUtc() != 0
        || !profile_detail::validHash(marker.value("source_manifest_sha256").toString())
        || marker.contains("source") || marker.contains("target") || marker.contains("password"))
        return profile_detail::fail(error, QStringLiteral("새 프로필의 완료 기록을 확인할 수 없습니다."));
    return true;
}
inline bool sameUpgradePaths(const ProfileUpgradePaths& a, const ProfileUpgradePaths& b) {
    return a.baseDirectory == b.baseDirectory && a.legacyOriginalDirectory == b.legacyOriginalDirectory
        && a.dev100Directory == b.dev100Directory && a.activeDirectory == b.activeDirectory
        && a.transactionDirectory == b.transactionDirectory;
}
// Replanning is required at every operation that can select or create a profile.
// A dialog may remain open while another process changes source or destination.
ProfileUpgradePlan revalidatedPlan(const ProfileUpgradePlan& plan, QString* error);
} // namespace keefi::profile_upgrade_detail
