#pragma once

#include "keefi/ProfilePaths.h"
#include <QSettings>
#include <optional>

namespace keefi {

void setProfileSettingsIdentity(const std::optional<ProfileSettingsIdentity>& identity);

class ProfileSettings final : public QSettings {
  public:
    explicit ProfileSettings(QObject* parent = nullptr);
  private:
    ProfileSettings(ProfileSettingsIdentity identity, QObject* parent);
};

} // namespace keefi
