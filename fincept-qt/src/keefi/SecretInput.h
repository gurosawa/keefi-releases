#pragma once

#include <QString>

namespace keefi {

// Count the UI input without making another unprotected UTF-8 secret copy.
inline bool exceedsSecretByteLimit(const QString& text, qsizetype limit) {
    qsizetype bytes = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto character = text.at(i);
        if (character.isHighSurrogate()) {
            if (++i >= text.size() || !text.at(i).isLowSurrogate()) return true;
            bytes += 4;
        } else if (character.isLowSurrogate()) {
            return true;
        } else {
            bytes += character.unicode() < 0x80 ? 1 : character.unicode() < 0x800 ? 2 : 3;
        }
        if (bytes > limit) return true;
    }
    return false;
}

// New vault passwords accept printable ASCII U+0020-U+007E only. Existing
// passwords are never checked against this; unlock uses their exact bytes.
inline bool isNewVaultPasswordText(const QString& text) {
    for (const QChar character : text)
        if (character.unicode() < 0x20 || character.unicode() > 0x7e) return false;
    return true;
}

inline QString newVaultPasswordAsciiMessage() {
    return QStringLiteral("영문 입력 상태로 전환해 주세요. 한글 등 비ASCII 문자는 사용할 수 없습니다.");
}

// Fixed Unicode White_Space property set, independent of QChar::isSpace.
// Characters outside it (for example U+200B) are never removed.
inline bool isUnicodeWhiteSpace(char16_t value) {
    return (value >= 0x09 && value <= 0x0d) || value == 0x20 || value == 0x85 || value == 0xa0 ||
        value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 || value == 0x2029 ||
        value == 0x202f || value == 0x205f || value == 0x3000;
}

// Removes only leading and trailing White_Space; interior characters stay exact.
inline QString trimUnicodeWhiteSpace(const QString& text) {
    qsizetype begin = 0, end = text.size();
    while (begin < end && isUnicodeWhiteSpace(text.at(begin).unicode())) ++begin;
    while (end > begin && isUnicodeWhiteSpace(text.at(end - 1).unicode())) --end;
    return begin == 0 && end == text.size() ? text : text.mid(begin, end - begin);
}

// Issued key identifiers and secrets only. Passphrases, account numbers and
// wallet addresses keep their exact input.
inline bool trimsCredentialField(const QString& key) {
    return key == QLatin1String("client_id") || key == QLatin1String("client_secret") ||
        key == QLatin1String("app_key") || key == QLatin1String("app_secret") ||
        key == QLatin1String("api_key") || key == QLatin1String("access_key") || key == QLatin1String("secret");
}

} // namespace keefi
