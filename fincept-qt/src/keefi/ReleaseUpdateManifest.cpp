#include "ReleaseUpdateManifest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>
#include <sodium.h>

namespace keefi {
QString updatePlatform() {
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("macos-x86_64");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("macos-arm64");
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("windows-x64");
#else
    return {};
#endif
}

QByteArray releaseUpdatePublicKey() {
    return QByteArray::fromHex("a4075005d352c63e5c694ec44508e5e5f98dd60581094fd3d4ebf4a39774df85");
}

UpdateEligibility verifyReleaseManifest(const QByteArray& manifest, const QByteArray& signatureBase64,
    const QByteArray& publicKey, const QString& tag, const QString& platform, const QString& installedVersion,
    int installedBuild, ReleaseUpdateAsset* asset, QString* error) {
    *asset = {};
    error->clear();
    const auto invalid = [error](const QString& reason) {
        *error = reason;
        return UpdateEligibility::Invalid;
    };
    const auto signature = QByteArray::fromBase64Encoding(signatureBase64.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    if (manifest.isEmpty() || manifest.size() > 32768 || !signature || signature.decoded.size() != crypto_sign_BYTES
        || publicKey.size() != crypto_sign_PUBLICKEYBYTES || sodium_init() < 0
        || crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(signature.decoded.constData()),
              reinterpret_cast<const unsigned char*>(manifest.constData()), manifest.size(),
              reinterpret_cast<const unsigned char*>(publicKey.constData())) != 0) {
        return invalid(QStringLiteral("배포 서명을 확인할 수 없습니다."));
    }

    const auto root = QJsonDocument::fromJson(manifest).object();
    const QString version = root.value("version").toString();
    static const QRegularExpression versionPattern(QStringLiteral(R"(^(?:0|[1-9][0-9]{0,5})(?:\.(?:0|[1-9][0-9]{0,5})){2}$)"));
    const auto build = root.value("build").toInt(-1);
    if (root.value("schema").toInt(-1) != 1 || build < 1 || !versionPattern.match(version).hasMatch()
        || root.value("tag").toString() != tag
        || (tag != "v" + version && tag != QStringLiteral("v%1-rc.%2").arg(version).arg(build))) {
        return invalid(QStringLiteral("배포 버전과 서명된 릴리스 정보가 일치하지 않습니다."));
    }
    if (installedBuild < 1 || !versionPattern.match(installedVersion).hasMatch()) {
        return invalid(QStringLiteral("설치된 앱 버전을 확인할 수 없습니다."));
    }
    const int comparison = QVersionNumber::compare(QVersionNumber::fromString(version),
                                                  QVersionNumber::fromString(installedVersion));
    if (build <= installedBuild || comparison < 0) return UpdateEligibility::AlreadyCurrent;
    const auto entry = root.value("assets").toObject().value(platform).toObject();
    const auto name = entry.value("name").toString();
    const auto digest = entry.value("sha256").toString();
    const auto size = entry.value("size").toInteger(-1);
    static const QRegularExpression digestPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (platform.isEmpty() || name != QStringLiteral("KEEFI-%1-%2-%3.zip").arg(version).arg(build).arg(platform)
        || !digestPattern.match(digest).hasMatch() || size <= 0 || size > 512LL * 1024 * 1024) {
        return invalid(QStringLiteral("이 운영체제용 배포 파일 정보를 확인할 수 없습니다."));
    }
    *asset = {version, build, tag, name, digest.toLatin1(), size};
    return UpdateEligibility::Available;
}

bool verifyUpdateArchive(const QString& path, const ReleaseUpdateAsset& asset, QString* error) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || file.size() != asset.size || !hash.addData(&file)
        || hash.result().toHex() != asset.sha256Hex) {
        *error = QStringLiteral("다운로드한 파일이 서명된 배포본과 일치하지 않습니다.");
        return false;
    }
    return true;
}
}
