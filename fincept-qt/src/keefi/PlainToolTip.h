#pragma once
#include <QTextDocument>

namespace keefi {
// Call only at the final tooltip sink, with plain display text. Do not pass
// already formatted tooltips or change the text used for accessibility/storage.
inline QString plainToolTip(const QString& text) {
    // QToolTip uses the same AutoText heuristic. Preserve ordinary strings
    // byte-for-byte; escape markup and entities only when Qt would parse them.
    if (!Qt::mightBeRichText(text)) return text;
    return QStringLiteral("<qt><span style=\"white-space: pre-wrap;\">%1</span></qt>")
        .arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
}
} // namespace keefi
