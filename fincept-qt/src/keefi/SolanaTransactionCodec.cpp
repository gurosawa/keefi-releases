#include "keefi/SolanaTransactionCodec.h"
#include <array>
#include <vector>
namespace keefi {
namespace { constexpr char Alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"; }
QByteArray SolanaTransactionCodec::decodeBase58(const QString& value, qsizetype expectedBytes) {
    if (expectedBytes < 1 || expectedBytes > 64 || value.isEmpty() || value.size() > 88) return {};
    std::array<int, 128> inverse;
    inverse.fill(-1);
    for (int i = 0; i < 58; ++i) inverse[static_cast<unsigned char>(Alphabet[i])] = i;
    qsizetype zeros = 0;
    while (zeros < value.size() && value.at(zeros) == QLatin1Char('1')) ++zeros;
    if (zeros > expectedBytes) return {};
    std::vector<unsigned char> buffer(static_cast<std::size_t>(expectedBytes), 0);
    qsizetype length = 0;
    for (qsizetype i = zeros; i < value.size(); ++i) {
        const auto cp = value.at(i).unicode();
        if (cp >= inverse.size() || inverse[cp] < 0) return {};
        int carry = inverse[cp];
        qsizetype used = 0;
        for (auto it = buffer.rbegin(); (carry || used < length) && it != buffer.rend(); ++it, ++used) {
            carry += 58 * *it;
            *it = static_cast<unsigned char>(carry & 255);
            carry >>= 8;
        }
        if (carry) return {};
        length = used;
    }
    if (zeros + length != expectedBytes) return {};
    QByteArray result(zeros, '\0');
    result.append(reinterpret_cast<const char*>(buffer.data() + buffer.size() - length), length);
    // One canonical spelling; never trim, case-fold, or normalize addresses.
    return encodeBase58(result) == value ? result : QByteArray{};
}

QString SolanaTransactionCodec::encodeBase58(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > 64) return {};
    qsizetype zeros = 0;
    while (zeros < bytes.size() && bytes.at(zeros) == '\0') ++zeros;
    std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes.size() * 138 / 100 + 1), 0);
    qsizetype length = 0;
    for (qsizetype i = zeros; i < bytes.size(); ++i) {
        int carry = static_cast<unsigned char>(bytes.at(i));
        qsizetype used = 0;
        for (auto it = buffer.rbegin(); (carry || used < length) && it != buffer.rend(); ++it, ++used) {
            carry += 256 * *it;
            *it = static_cast<unsigned char>(carry % 58);
            carry /= 58;
        }
        if (carry) return {};
        length = used;
    }
    QString result(zeros, QLatin1Char('1'));
    for (auto i = buffer.size() - length; i < buffer.size(); ++i)
        result.append(QLatin1Char(Alphabet[buffer[i]]));
    return result;
}

bool SolanaTransactionCodec::publicKeyValid(const QString& value) { return decodeBase58(value, 32).size() == 32; }

}
