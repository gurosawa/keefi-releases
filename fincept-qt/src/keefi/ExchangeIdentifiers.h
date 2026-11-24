#pragma once

#include <QString>

namespace keefi {

// Binance Spot supports UTF-8 asset/symbol names. Preserve the exact identifier:
// no trimming, case folding or Unicode normalization. These local bounds admit
// letters, combining marks and numbers, but reject controls, format characters,
// whitespace, URL syntax and malformed surrogate pairs. Trading still requires
// the exact symbol and its current rules from the official exchange catalogue.
inline bool binanceIdentifierValid(const QString& value, qsizetype minScalars = 1) {
    if (minScalars < 1 || minScalars > 40 || value.isEmpty() || value.size() > 80)
        return false;
    qsizetype scalars = 0, bytes = 0;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar first = value.at(i);
        char32_t codepoint = first.unicode();
        if (first.isHighSurrogate()) {
            if (++i >= value.size() || !value.at(i).isLowSurrogate())
                return false;
            codepoint = QChar::surrogateToUcs4(first, value.at(i));
        } else if (first.isLowSurrogate()) {
            return false;
        }
        const auto category = QChar::category(codepoint);
        if (!QChar::isLetterOrNumber(codepoint) && category != QChar::Mark_NonSpacing &&
            category != QChar::Mark_SpacingCombining && category != QChar::Mark_Enclosing)
            return false;
        bytes += codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
        if (++scalars > 40 || bytes > 160)
            return false;
    }
    return scalars >= minScalars;
}

// The public mini-ticker lane supports only spot pairs quoted in USDT.
inline bool binanceUsdtTickerSymbolValid(const QString& symbol) {
    return symbol.endsWith(QStringLiteral("USDT")) && binanceIdentifierValid(symbol, 5);
}

// USDⓈ-M delivery contracts append _YYMMDD to the pair. Keep asset/Spot
// validation separate; exact catalogue membership and rules are still required.
inline bool binanceUsdmSymbolValid(const QString& value) {
    const auto separator = value.indexOf(u'_');
    if (separator < 0)
        return binanceIdentifierValid(value, 2);
    if (value.size() > 80 || value.toUtf8().size() > 160 || value.toUcs4().size() > 40 || value.size() - separator != 7 ||
        !binanceIdentifierValid(value.left(separator), 2))
        return false;
    for (const auto c : value.sliced(separator + 1))
        if (c < u'0' || c > u'9')
            return false;
    return true;
}

} // namespace keefi
