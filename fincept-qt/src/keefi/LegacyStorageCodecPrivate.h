#pragma once
// Explicit compatibility boundary. Historical wire bytes and identity names live here,
// never hidden by numeric encoding. This header is internal to storage/migration.
#include "keefi/SecureBuffer.h"
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>

namespace keefi::storage_detail {
enum class Kind { Vault, Assets, Swap, Price, Imported, Package };
enum class Format { Invalid, Legacy, Current };
QByteArray magic(Kind,Format);
Format formatOf(const QByteArray&,Kind);
QByteArray keyDomain(Kind,Format);
bool deriveStoreKey(const SecureBuffer& root,Kind,Format,SecureBuffer* key,QString* error);
bool decodeStore(const QByteArray&,Kind,const SecureBuffer& root,SecureBuffer* plain,QString* error);
QByteArray encodeStore(const SecureBuffer&,Kind,const SecureBuffer& root,QString* error);
QString legacyFileName(Kind);
QString currentFileName(Kind);
QString readableFileName(const QString& directory,Kind);
QString latestAssetsFileName(Format);
QString readableLatestAssetsFileName(const QString& directory);
QString rewrittenFileName(const QString&);
bool legacyStorageName(const QString&);
// Basename-only guard shared by the inactive profile/backup transactions.
bool encryptedStorageName(const QString&);
QStringList archivePatterns(const QString& base);
QStringList relatedFileNames(const QString& directory);
bool legacyAssetMigrationValid(const QJsonObject&);
bool rejectLegacyWrite(bool legacy,QString* error);

struct VaultMaterial {
    quint32 ops=0;
    quint64 mem=0;
    QByteArray salt;
    Format format=Format::Invalid;
    QString identityScheme;
    SecureBuffer key,payload,ledgerRoot;
};
bool authenticateVault(const QByteArray&,const QString& password,VaultMaterial*,QString* error);
QByteArray encodeCurrentVault(const VaultMaterial&,QString* error);
bool privateStoragePath(const QString&,bool directory,QString* error);
// These adapters are implemented beside the existing strict product decoders.
bool validateAssetsPayload(const SecureBuffer&,bool legacy,QString* error);
bool rewriteAssetsPayload(const SecureBuffer&,const QJsonObject& rewriteFiles,SecureBuffer*,QString* error);
bool validateSwapPayload(const SecureBuffer&,QString* error);
bool validatePricePayload(const SecureBuffer&,bool exitNotice,QString* error);
bool validateImportedPayload(const SecureBuffer&,QString* error);
} // namespace finix::storage_detail

