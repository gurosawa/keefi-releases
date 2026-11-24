#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace keefi {

enum class ProfileUpgradeSourceKind { None, LegacyOriginal, Dev100Keefi, ExplicitLegacy };
enum class ProfileUpgradeAction { UseCurrent, CreateCurrent, OfferUpgrade, ChooseSource, Unavailable };

struct ProfileUpgradePaths {
    QString baseDirectory;
    QString legacyOriginalDirectory;
    QString dev100Directory;
    QString activeDirectory;
    QString transactionDirectory;
};

struct ProfileUpgradeSource {
    ProfileUpgradeSourceKind kind = ProfileUpgradeSourceKind::None;
    QString directory;
    bool requiresPassword = false;
};

struct ProfileUpgradePlan {
    ProfileUpgradeAction action = ProfileUpgradeAction::Unavailable;
    ProfileUpgradePaths paths;
    std::optional<QString> explicitDirectory;
    QList<ProfileUpgradeSource> sources;
    ProfileUpgradeSourceKind sourceKind = ProfileUpgradeSourceKind::None;
    QString sourceDirectory;
    QString targetDirectory;
    QString transactionDirectory;
    bool requiresPassword = false;
    QString identityScheme;
    QString error;
};

struct ProfileConversionReceipt {
    bool verified = false;
    QString identityScheme;
    QJsonObject evidence;
};

using ProfileStageConverter = std::function<bool(
    const QString& stageDirectory, const QString& password,
    ProfileConversionReceipt* receipt, QString* error)>;

struct ProfileReady {
    QString directory;
    QString identityScheme;
    QString warning;
};

ProfileUpgradePaths profileUpgradePaths(const QString& base);
ProfileUpgradePlan planProfileUpgrade(const std::optional<QString>& explicitDirectory,
                                      const QString& base);
ProfileUpgradePlan selectProfileUpgradeSource(const ProfileUpgradePlan& plan,
                                              ProfileUpgradeSourceKind source,
                                              QString* error);
ProfileReady existingProfileReady(const ProfileUpgradePlan& plan, QString* error);
ProfileReady createFreshProfile(const ProfileUpgradePlan& plan, const QString& version,
                                QString* error);

class ProfileUpgradeSession {
public:
    static std::unique_ptr<ProfileUpgradeSession> begin(
        const ProfileUpgradePlan& plan, const QString& version, QString* error);
    ~ProfileUpgradeSession();
    bool copy(QString* error);
    bool convert(const QString& password, const ProfileStageConverter& converter, QString* error);
    bool verify(QString* error);
    bool publish(QString* error);
    QString stagingDirectory() const;
    ProfileReady ready() const;

private:
    struct Private;
    std::unique_ptr<Private> d_;
    explicit ProfileUpgradeSession(std::unique_ptr<Private> state);
};

} // namespace keefi
