#pragma once

#include "ProfilePaths.h"
#include <QIODevice>
#include <memory>

namespace keefi {

struct ProfileMigrationResult {
    QString directory;
    ProfileSettingsIdentity settings;
    bool copied = false;
    bool cancelled = false;
    QString error;
    QString warning;
};

class ProfileCopySession {
public:
    static std::unique_ptr<ProfileCopySession> begin(const DevProfilePaths& paths,
                                                   const QString& version,
                                                   QString* error);
    ~ProfileCopySession();
    bool copy(QString* error);
    bool verify(QString* error);
    bool publish(QString* error);
    QString stagingDirectory() const;
    QString warning() const;

private:
    friend ProfileMigrationResult applyDevProfileChoice(const ProfileMigrationPlan& plan,
                                                         ProfileMigrationChoice choice,
                                                         const QString& version);
    struct Private;
    std::unique_ptr<Private> d_;
    explicit ProfileCopySession(std::unique_ptr<Private> state);
};

bool copyProfileStream(QIODevice& input, QIODevice& output, QString* error);
ProfileMigrationResult applyDevProfileChoice(const ProfileMigrationPlan& plan,
                                             ProfileMigrationChoice choice,
                                             const QString& version);

} // namespace keefi
