#pragma once

#include <QJsonObject>
#include <QString>

namespace keefi {

struct StorageConversionReceipt {
    bool verified = false;
    QString identityScheme;
    QJsonObject evidence;
};

// Converts only an owned, inactive staging copy supplied by the profile transaction.
// The caller preserves/locks the original and publishes the stage only after success.
// No normal Vault, account backend, UI, network, or automatic writer is started here.
class StorageMigration final {
  public:
    static bool convertStagedCopy(const QString& stageDirectory, const QString& password,
                                  StorageConversionReceipt* receipt, QString* error = nullptr);
    // Restores only into an owned inactive stage without any encrypted storage.
    // The source backup is read-only; the profile transaction publishes on restart.
    static bool restoreBackupStagedCopy(const QString& stageDirectory, const QString& sourceBackup,
                                        const QString& password, StorageConversionReceipt* receipt,
                                        QString* error = nullptr);
};

} // namespace keefi
