#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <memory>

class QSettings;

namespace keefi {
class Vault;

// Explicit public-Mint choices for one vault in this app's settings channel.
// The settings contain Mint addresses and a one-way vault binding only: no
// balance, wallet-owner address, credentials, or copy of the ledger key.
class ValuationPreferences final : public QObject {
    Q_OBJECT
  public:
    explicit ValuationPreferences(Vault& vault, QObject* parent = nullptr);
    ~ValuationPreferences() override;
    // A missing ledger key is a normal empty preference; reading never creates it.
    bool load(QString* error = nullptr);
    QSet<QString> excludedAssets() const;
    bool setExcluded(const QString& canonicalAsset, bool excluded, QString* error = nullptr);
    // Clears this session's cache only; the vault-bound choice survives a lock.
    void clear();

  signals:
    void changed();

  private:
    QString binding(bool createIfMissing, QString* error);
    Vault& vault_;
    std::unique_ptr<QSettings> settings_;
    QString binding_;
    QSet<QString> excluded_;
    bool loaded_ = false;
};
} // namespace keefi
