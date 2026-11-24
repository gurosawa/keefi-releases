#pragma once

#include "ProfileUpgrade.h"
#include <functional>
#include <memory>

class QLockFile;

namespace keefi {
struct ProfileRestorePaths {
    QString currentDirectory;
    QString transactionDirectory;
};
ProfileRestorePaths profileRestorePaths(const QString& currentDirectory);

using ProfileBackupRestorer = std::function<bool(
    const QString& stageDirectory, const QString& backupPath, const QString& password,
    ProfileConversionReceipt* receipt, QString* error)>;

// The caller's ordinary instance lock must outlive this preparation. No active
// vault or backend is opened, and a prepared stage stays inactive until restart.
class ProfileRestoreSession {
public:
    static std::unique_ptr<ProfileRestoreSession> begin(
        const ProfileReady& current, QLockFile& ownedInstanceLock,
        const QString& version, QString* error);
    ~ProfileRestoreSession();
    bool copy(QString* error);
    bool restore(const QString& backupPath, const QString& password,
                 const ProfileBackupRestorer& restorer, QString* error);
    bool verify(QString* error);
    bool prepareForRestart(QString* error);
    QString stagingDirectory() const;
    QString archiveDirectory() const;
private:
    struct Private;
    std::unique_ptr<Private> d_;
    explicit ProfileRestoreSession(std::unique_ptr<Private> state);
};

// These are real startup stages, also used by completePendingProfileRestore.
// Successful publish is the commit; archive failure must not delete the new root.
class ProfileRestoreCompletion {
public:
    static std::unique_ptr<ProfileRestoreCompletion> begin(
        const ProfileReady& selected, const QString& version, QString* error);
    ~ProfileRestoreCompletion();
    bool refreshPublicSettings(QString* error);
    bool verify(QString* error);
    bool publish(QString* error);
    bool archive(QString* error);
    ProfileReady ready() const;
    QString archiveDirectory() const;
private:
    struct Private;
    std::unique_ptr<Private> d_;
    explicit ProfileRestoreCompletion(std::unique_ptr<Private> state);
};

// Returns the selected profile when no transaction exists. Any uncommitted
// recovery failure returns an empty directory, never a normal-backend fallback.
ProfileReady completePendingProfileRestore(const ProfileReady& selected,
                                           const QString& version, QString* error);
} // namespace keefi
