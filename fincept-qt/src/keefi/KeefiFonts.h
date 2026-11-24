#pragma once
#include <QFont>
#include <QString>
#include <QTextLayout>

namespace keefi {
// Pixel sizes and real faces belong here; Departure Mono has no bold face.
enum class FontRole {
    Price44, Value22, Number11, Hero44, Title17, Body15, Secondary14, Caption12,
    BodyStrong15, SecondaryStrong14, CaptionStrong12, Logo22, BodyBold15, Lead28
};

class KeefiFonts final {
  public:
    // Call on the GUI thread after QGuiApplication exists. Repeated calls are safe.
    static bool registerBundledFonts();
    static QFont font(FontRole role);
    static QString bodyFamily();
    static QString numberFamily();
    // Display-only runs: never parse/recalculate values or change the model text.
    static QList<QTextLayout::FormatRange> numberRuns(const QString&);
    // Escape untrusted/plain content before native rich-text layout/selection.
    static QString mixedTextHtml(const QString&, FontRole bodyRole = FontRole::Body15);
};
} // namespace keefi
