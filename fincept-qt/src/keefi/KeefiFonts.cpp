#include "keefi/KeefiFonts.h"
#include <QFontDatabase>
#include <QGuiApplication>
#include <QResource>
#include <QStringList>
#include <QThread>
#include <QRegularExpression>

// Keep the resource reference outside the namespace and retain it when the
// KEEFI library is linked statically.
static void initializeKeefiFontResources() {
    Q_INIT_RESOURCE(keefi_fonts);
    Q_INIT_RESOURCE(keefi_notices);
}

namespace keefi {
QString KeefiFonts::bodyFamily() { return QStringLiteral("Pretendard"); }
QString KeefiFonts::numberFamily() { return QStringLiteral("Departure Mono"); }

QList<QTextLayout::FormatRange> KeefiFonts::numberRuns(const QString& text) {
    static const QRegularExpression numeric(QStringLiteral("[+\\-]?[0-9][0-9,.:/%+\\-]*(?![A-Za-z0-9_])|(?<![A-Za-z0-9_])[A-Z0-9][A-Z0-9_./\\-]*(?![A-Za-z0-9_])"));
    QList<QTextLayout::FormatRange> result;auto matches=numeric.globalMatch(text);
    while(matches.hasNext()){const auto match=matches.next();QTextLayout::FormatRange run;
        run.start=match.capturedStart();run.length=match.capturedLength();run.format.setFont(font(FontRole::Number11));result.append(run);}
    return result;
}

QString KeefiFonts::mixedTextHtml(const QString& text, FontRole bodyRole) {
    if(text.isEmpty())return {};
    const auto css=[](const QFont& value){return QString("font-family:'%1';font-size:%2px;font-weight:%3;")
        .arg(value.family()).arg(value.pixelSize()).arg(int(value.weight()));};
    const auto escaped=[](const QString& value){return value.toHtmlEscaped().replace('\n',"<br>");};
    QString html;qsizetype position=0;
    for(const auto& run:numberRuns(text)){
        html+=escaped(text.mid(position,run.start-position));
        html+="<span style=\""+css(run.format.font())+"\">"+escaped(text.mid(run.start,run.length))+"</span>";
        position=run.start+run.length;
    }
    html+=escaped(text.mid(position));
    return "<span style=\"white-space:pre-wrap;"+css(font(bodyRole))+"\">"+html+"</span>";
}

bool KeefiFonts::registerBundledFonts() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ||
        QThread::currentThread() != QCoreApplication::instance()->thread()) return false;
    static const bool registered = [] {
        initializeKeefiFontResources();
        bool ok = true;
        for (const auto* name : {"Pretendard-Regular.otf", "Pretendard-Medium.otf", "Pretendard-SemiBold.otf",
                                 "Pretendard-Bold.otf", "Pretendard-ExtraBold.otf", "DepartureMono-Regular.otf"}) {
            const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/keefi/fonts/") + QString::fromLatin1(name));
            const QString expected = QString::fromLatin1(name).startsWith(QStringLiteral("Departure")) ? numberFamily() : bodyFamily();
            ok = id >= 0 && QFontDatabase::applicationFontFamilies(id).contains(expected) && ok;
        }
        if (ok) QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Hangul, {bodyFamily()});
        return ok;
    }();
    return registered;
}

QFont KeefiFonts::font(FontRole role) {
    registerBundledFonts();
    const bool number = role == FontRole::Price44 || role == FontRole::Value22 ||
                        role == FontRole::Number11 || role == FontRole::Logo22;
    QFont result;
    // Explicit fallback keeps Korean fragments in the bundled face even when a
    // caller renders a mixed number/label run. Separately laid-out runs may use
    // Body/Caption and Number roles to preserve their different pixel sizes.
    result.setFamilies(number ? QStringList{numberFamily(), bodyFamily()} : QStringList{bodyFamily()});
    int pixels = 15;
    QFont::Weight weight = QFont::Normal;
    QString style = QStringLiteral("Regular");
    switch (role) {
    case FontRole::Price44: pixels = 44; break;
    case FontRole::Value22: pixels = 22; break;
    case FontRole::Logo22: pixels = 22; break;
    case FontRole::Number11: pixels = 11; break;
    case FontRole::Hero44: pixels = 44; weight = QFont::ExtraBold; style = QStringLiteral("ExtraBold"); break;
    case FontRole::Title17: pixels = 17; weight = QFont::Bold; style = QStringLiteral("Bold"); break;
    case FontRole::Lead28: pixels = 28; weight = QFont::ExtraBold; style = QStringLiteral("ExtraBold"); break;
    case FontRole::Body15: break;
    case FontRole::BodyBold15: weight = QFont::Bold; style = QStringLiteral("Bold"); break;
    case FontRole::Secondary14: pixels = 14; break;
    case FontRole::Caption12: pixels = 12; break;
    case FontRole::BodyStrong15: weight = QFont::DemiBold; style = QStringLiteral("SemiBold"); break;
    case FontRole::SecondaryStrong14: pixels = 14; weight = QFont::DemiBold; style = QStringLiteral("SemiBold"); break;
    case FontRole::CaptionStrong12: pixels = 12; weight = QFont::DemiBold; style = QStringLiteral("SemiBold"); break;
    }
    result.setPixelSize(pixels);
    // The logo alone uses the specified 0.08em tracking. Prices keep the
    // bundled mono face's original spacing and alignment.
    if (role == FontRole::Logo22) result.setLetterSpacing(QFont::AbsoluteSpacing, 22.0 * 0.08);
    result.setWeight(weight);
    // CoreText's weight matching may choose Bold for an 800 request. Naming the
    // bundled face selects its real OS/2 weight 800 without synthesizing bold.
    // Do not pin the inherited body face to Regular: Qt stylesheets change
    // weight without clearing styleName, which would silently keep headings
    // regular. 400/500/600/700 resolve correctly by weight on CoreText.
    if (number || role == FontRole::Hero44 || role == FontRole::Lead28) result.setStyleName(style);
    result.setHintingPreference(QFont::PreferDefaultHinting);
    result.setStyleStrategy(number ? QFont::NoAntialias : QFont::PreferAntialias);
    return result;
}
} // namespace keefi
