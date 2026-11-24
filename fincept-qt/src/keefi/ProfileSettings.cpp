#include "keefi/ProfileSettings.h"

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>

namespace keefi {
namespace {
QMutex settingsIdentityMutex;
std::optional<ProfileSettingsIdentity> settingsIdentity;

ProfileSettingsIdentity effectiveSettingsIdentity() {
    const QMutexLocker lock(&settingsIdentityMutex);
    if (settingsIdentity) return *settingsIdentity;

    // Match QSettings' platform preference without changing application metadata.
#ifdef Q_OS_DARWIN
    auto organization = QCoreApplication::organizationDomain();
    if (organization.isEmpty()) organization = QCoreApplication::organizationName();
#else
    auto organization = QCoreApplication::organizationName();
    if (organization.isEmpty()) organization = QCoreApplication::organizationDomain();
#endif
    return {organization, QCoreApplication::applicationName()};
}
} // namespace

void setProfileSettingsIdentity(const std::optional<ProfileSettingsIdentity>& identity) {
    const QMutexLocker lock(&settingsIdentityMutex);
    settingsIdentity = identity;
}

ProfileSettings::ProfileSettings(QObject* parent)
    : ProfileSettings(effectiveSettingsIdentity(), parent) {}

ProfileSettings::ProfileSettings(ProfileSettingsIdentity identity, QObject* parent)
    : QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                identity.organization, identity.application, parent) {}

} // namespace keefi
