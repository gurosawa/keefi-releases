#pragma once
#include <QByteArray>
#include <QString>
namespace keefi {
// Compatibility name for read-only address validation. Transaction decoding,
// signing preparation and signed-transaction verification have been removed.
class SolanaTransactionCodec final {
public:
    static QByteArray decodeBase58(const QString& value, qsizetype expectedBytes);
    static QString encodeBase58(const QByteArray& bytes);
    static bool publicKeyValid(const QString& value);
};
}
