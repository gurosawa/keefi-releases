#pragma once

#include "keefi/AccountModel.h"
#include "keefi/SecureBuffer.h"
#include <QMap>
#include <QJsonArray>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace keefi {

// Synchronous, single-owner Qt-thread API. Caller clears Qt input widgets/maps
// after each call. No network calls and no legacy credential migration.
class Vault final : public QObject {
    Q_OBJECT
  public:
    static constexpr quint64 DefaultMemLimit = 64ULL * 1024 * 1024;
    static constexpr quint32 DefaultOpsLimit = 3;
    static constexpr int MinPasswordCharacters = 12;
    static constexpr qsizetype MaxPasswordBytes = 1024;
    static constexpr qsizetype MaxFileBytes = 8 * 1024 * 1024;
    static constexpr int MaxAccounts = 64;
    static constexpr int HeaderBytes = 72;

    explicit Vault(QString filePath, QObject* parent = nullptr);
    // A normal profile binds authentication to its selected identity scheme.
    // Parent stays explicit so the existing Vault(path, nullptr) call is unambiguous.
    Vault(QString filePath, const QString& expectedIdentityScheme, QObject* parent);
    ~Vault() override;
    bool exists() const;
    bool isUnlocked() const;
    QString filePath() const;
    QList<AccountRecord> accounts() const;

    bool create(const QString& password, QString* error = nullptr);
    bool unlock(const QString& password, QString* error = nullptr);
    // Authenticates an owner-only backup without writing or locking its source.
    // Installs only at an absent destination. A legacy container is re-encrypted
    // with the same password-derived key and payload in the current format;
    // current containers retain their exact bytes. The source is never changed.
    bool restoreBackup(const QString& sourcePath, const QString& password,
                       QString* error = nullptr);
    static bool authenticatedBackupIdentity(const QString& sourcePath, const QString& password,
                                            QString* identityScheme, QString* error = nullptr);
    void lock();
    // Forgotten password: locks, then permanently deletes this vault and the
    // encrypted records keyed by it in the same folder (asset ledgers and
    // archives, swap journal, imported records, price archive). Needs no password. Financial
    // accounts and API keys at the providers are untouched.
    bool erase(QString* error = nullptr);
    static QStringList erasedFileNames(const QString& vaultPath); // existing files erase() would delete
    bool changePassword(const QString& currentPassword, const QString& newPassword,
                        QString* error = nullptr);

    // Empty id is replaced by a random UUID. Connections are persisted only in an
    // unlocked vault; SessionOnly is rejected (kept for record decoding only).
    bool addAccount(AccountRecord record, const QMap<QString, QString>& credentials,
                    StorageMode mode, QString* error = nullptr);
    bool addAccountUsingCredentials(AccountRecord record, const QString& sourceAccountId, QString* error = nullptr);
    QList<AccountRecord> credentialUsers(const QString& accountId) const;
    // Dedicated random data key, unrelated to API credentials and stable across password changes.
    // createIfMissing is an explicit persistent mutation; false never rewrites an older vault.
    bool hasAssetsLedgerKey() const; // Presence only; never creates a key or reads a file.
    bool withAssetsLedgerKey(bool createIfMissing, const std::function<void(const SecureBuffer&)>&,
                             QString* error = nullptr);
    bool removeAccount(const QString& id, QString* error = nullptr);
    bool replaceCredentials(const QString& id, const QMap<QString, QString>& credentials,
                            QString* error = nullptr);
    bool replaceCredentials(const QString& id, const QMap<QString, QString>& credentials,
                            const QString& region, QString* error = nullptr);
    // Encrypted write-ahead records, persisted before any trading mutation.
    // API secrets/private account identifiers are forbidden. Payload v7 preserves
    // legacy 17/20/23/27-field records and adds strict futures 25-field records.
    // Payload v1-v6 remain readable; opening alone never rewrites the file.
    QJsonArray pendingOperations() const;
    bool setPendingOperations(const QJsonArray& operations, QString* error = nullptr);

    // Borrowed references are valid only for this synchronous call. Do not keep
    // pointers, log, queue or forward them to unrelated workers/LLM/MCP paths.
    // The visitor must not re-enter Vault methods or process Qt events.
    bool withCredentials(const QString& id,
                         const std::function<void(const CredentialSet&)>& visitor,
                         QString* error = nullptr) const;

    // Creation/change only: printable ASCII (U+0020-U+007E), at least 12 characters,
    // ASCII A-Z/a-z/0-9 and punctuation (!-/, :-@, [-`, {-~) are all required. No
    // normalization or trimming. Existing passwords, including non-ASCII ones, are
    // authenticated by unlock and changePassword's current password unchanged.
    static bool validateNewPassword(const QString& password, QString* error = nullptr);
    // Creates only a missing profile directory with user-only permissions/ACL.
    // An existing directory is verified, never silently chmod'ed or re-owned.
    static bool prepareProfileDirectory(const QString& directory, QString* error = nullptr);

  signals:
    void aboutToLock();
    void locked();
    void stateChanged();
    void accountsChanged();

  private:
    bool replaceCredentialsImpl(const QString&, const QMap<QString, QString>&, const QString&, bool, QString*);
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace keefi
