#pragma once
#include "keefi/IdentityCompatibility.h"
#include "keefi/TradingTypes.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QtEndian>

namespace keefi {
inline bool okxRegionValid(const QString& value) { return value == "global" || value == "us" || value == "eea"; }
inline bool okxEnvironmentValid(const QString& value) { return value == "live"; }
inline bool okxOrderIdValid(const QString& value) {
    static const QRegularExpression re(QStringLiteral("\\A[0-9]{1,128}\\z"));
    return re.match(value).hasMatch();
}
inline bool okxClientIdValid(const QString& value, bool emptyAllowed = true) {
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9]{1,32}\\z"));
    return (emptyAllowed && value.isEmpty()) || re.match(value).hasMatch();
}
inline bool okxInstrumentValid(const QString& value) {
    const auto parts = value.split('-');
    return parts.size() == 2 && binanceIdentifierValid(parts[0]) && binanceIdentifierValid(parts[1]);
}
inline QString okxAccountScope(const QString& region, const QString& environment, const QString& rawUid) {
    if (!okxRegionValid(region) || !okxEnvironmentValid(environment) || !okxOrderIdValid(rawUid)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& value : {identityDomainText(IdentityDomain::OkxAccount), region, environment, QStringLiteral("spot"), rawUid}) {
        const auto bytes = value.toUtf8();
        unsigned char length[4]; qToBigEndian<quint32>(quint32(bytes.size()), length);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(length), 4)); hash.addData(bytes);
    }
    return QString::fromLatin1(hash.result().toHex());
}
enum class OkxOrderSource { Rest, WebSocket };
bool parseOkxOrder(const QJsonObject& object, OkxOrderSource source,
                   OrderRecord* output, QString* safeError = nullptr);
} // namespace keefi
