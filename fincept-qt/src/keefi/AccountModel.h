#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace keefi {

class Vault;

enum class StorageMode { EncryptedVault, SessionOnly };

struct CredentialField {
    QString key;
    QString label;
    bool required = true;
    bool secret = true;
};

struct ProviderSpec {
    QString id;
    QString label;
    QList<CredentialField> fields;
    QString guideUrl;
    QStringList markets;
    QStringList currencies;
    QStringList environments;
    bool keyless = false;
    QString issuanceUrl = {};
};

// No account number, wallet address or credential values in this public record.
// G1 cannot assert authentication/order authority or expose live trading.
struct AccountRecord {
    QString id;
    QString provider;
    QString alias;
    QString environment;
    QString market;
    QString currency;
    StorageMode storageMode = StorageMode::EncryptedVault;
    QString connectionState = QStringLiteral("not_checked");
    QString permissionState = QStringLiteral("unverified");
    bool readOnly = true;
    QString region; // OKX explicit region; empty only for legacy metadata/other providers.
    QString credentialRef; // Opaque local credential-set UUID; never a key or account identity.
};

class AccountModel final : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Role {
        IdRole = Qt::UserRole + 1, ProviderRole, AliasRole, EnvironmentRole,
        MarketRole, CurrencyRole, StorageModeRole, ConnectionStateRole,
        PermissionStateRole, ReadOnlyRole
    };
    explicit AccountModel(Vault* vault, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    AccountRecord recordAt(int row) const;
    QList<AccountRecord> accounts() const { return records_; }

    static const QList<ProviderSpec>& providers();
    static const ProviderSpec* provider(const QString& id);
    static bool isProviderSuspended(const QString& id);
    static bool allowsNewConnection(const QString& id);
    // New connections accept live (Solana mainnet) only. Reading an older vault also accepts
    // the retired paper/demo/testnet environments so the vault still opens; such records are never read.
    static bool validate(const AccountRecord& record, QString* error = nullptr, bool acceptRetired = false);
    static bool retiredEnvironment(const AccountRecord& record);
    static QJsonObject toJson(const AccountRecord& record);
    static bool fromJson(const QJsonObject& json, AccountRecord* record, QString* error = nullptr);

  private:
    void refresh();
    Vault* vault_;
    QList<AccountRecord> records_;
};

} // namespace keefi
