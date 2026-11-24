#pragma once

#include <QDate>
#include <QRegularExpression>
#include <QString>

namespace keefi {
// Pure grammar shared by the encrypted journal and broker codec. Never includes
// an account selector. Numeric identifiers retain their original leading zeros.
inline bool kiwoomOrderReferenceValid(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("\\AW1_([0-9]{8})_(KRX|NXT|SOR)_([0-9]{1,20})\\z"));
    const auto match = pattern.match(value);
    if (!match.hasMatch() || !QDate::fromString(match.captured(1), QStringLiteral("yyyyMMdd")).isValid()) return false;
    const auto number = match.captured(3);
    for (const QChar digit : number) if (digit != QLatin1Char('0')) return true;
    return false;
}
inline bool lsOrderReferenceValid(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("\\AL1_([0-9]{8})_(KRX|NXT|SOR)_([0-9]{1,10})\\z"));
    const auto match = pattern.match(value);
    if (!match.hasMatch() || !QDate::fromString(match.captured(1), QStringLiteral("yyyyMMdd")).isValid()) return false;
    const auto number = match.captured(3);
    for (const QChar digit : number) if (digit != QLatin1Char('0')) return true;
    return false;
}
} // namespace keefi
