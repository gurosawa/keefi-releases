#pragma once
#include <QByteArray>
#include <QString>

namespace keefi {
struct ReleaseUpdateAsset {
    QString version;
    int build = 0;
    QString tag;
    QString name;
    QByteArray sha256Hex;
    qint64 size = 0;
};
enum class UpdateEligibility { Invalid, AlreadyCurrent, Available };

QString updatePlatform();
QByteArray releaseUpdatePublicKey(); // Raw Ed25519 verification key.
UpdateEligibility verifyReleaseManifest(const QByteArray& manifest, const QByteArray& signatureBase64,
    const QByteArray& publicKey, const QString& tag, const QString& platform, const QString& installedVersion,
    int installedBuild, ReleaseUpdateAsset* asset, QString* error);
bool verifyUpdateArchive(const QString& path, const ReleaseUpdateAsset& asset, QString* error);
}
