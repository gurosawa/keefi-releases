#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QImage>
#include <QLabel>
#include <QWidget>
#include <QPainter>
#include <QRawFont>
#include <QTest>
#include <QTextLayout>
#include <QtEndian>

using namespace keefi;
namespace {
int sourceWeight(const QRawFont& font) {
    const auto table = font.fontTable("OS/2");
    return table.size() < 6 ? -1 : qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(table.constData() + 4));
}
}
class FontsTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(KeefiFonts::registerBundledFonts());
        QVERIFY(KeefiFonts::registerBundledFonts());
        QCOMPARE(KeefiFonts::bodyFamily(), QStringLiteral("Pretendard"));
        QCOMPARE(KeefiFonts::numberFamily(), QStringLiteral("Departure Mono"));
    }
    void officialFilesAndGlyphCoverage_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("sha256");
        QTest::addColumn<int>("weight");
        QTest::newRow("pretendard-regular") << QStringLiteral("Pretendard-Regular.otf") << QByteArray("3ffbacde6ab8411f1d2db54bb9b1f0b3ee2a738932033722cf0388c06aed1c93") << 400;
        QTest::newRow("pretendard-medium") << QStringLiteral("Pretendard-Medium.otf") << QByteArray("d39e50e4bb52b4993b6a4eeb821a171254745bd824446af01e1f616b89fface0") << 500;
        QTest::newRow("pretendard-semibold") << QStringLiteral("Pretendard-SemiBold.otf") << QByteArray("c89bc43027dc7cde5726e96223376f8eec09302b2fc1f8147fd5b57cfc376118") << 600;
        QTest::newRow("pretendard-bold") << QStringLiteral("Pretendard-Bold.otf") << QByteArray("2e91915fab54df71cc9598ebf608b2bdb54c6fe3c066ac61dff0bc44fca71cc7") << 700;
        QTest::newRow("pretendard-extrabold") << QStringLiteral("Pretendard-ExtraBold.otf") << QByteArray("c35fe941b7568d52a96010561540e47f9d3948dfde66ba25bc1908233e0a40cd") << 800;
        QTest::newRow("departure") << QStringLiteral("DepartureMono-Regular.otf") << QByteArray("4d53f663155cf8bf7ffc8e688776e719625f7bbb80a8d90073438b249261a2e0") << 400;
    }
    void officialFilesAndGlyphCoverage() {
        QFETCH(QString, file); QFETCH(QByteArray, sha256); QFETCH(int, weight);
        QFile input(QStringLiteral(":/keefi/fonts/") + file);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto bytes = input.readAll();
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), sha256);
        QRawFont raw(bytes, 22);
        QVERIFY(raw.isValid()); QCOMPARE(sourceWeight(raw), weight);
        for (uint ch = 32; ch <= 126; ++ch) QVERIFY(raw.supportsCharacter(ch));
        const bool korean = file.startsWith(QStringLiteral("Pretendard"));
        for (uint ch = 0xac00; ch <= 0xd7a3; ++ch) QCOMPARE(raw.supportsCharacter(ch), korean);
    }
    void rolesResolveTheRealBundledFaces_data() {
        QTest::addColumn<int>("role"); QTest::addColumn<int>("pixels"); QTest::addColumn<int>("weight"); QTest::addColumn<QString>("face");
        const auto row = [](const char* name, FontRole role, int pixels, int weight, const char* face) {
            QTest::newRow(name) << int(role) << pixels << weight << QString::fromLatin1(face);
        };
        row("price", FontRole::Price44, 44, 400, "Regular"); row("value", FontRole::Value22, 22, 400, "Regular"); row("number", FontRole::Number11, 11, 400, "Regular");
        row("logo", FontRole::Logo22, 22, 400, "Regular");
        row("hero", FontRole::Hero44, 44, 800, "ExtraBold"); row("title", FontRole::Title17, 17, 700, "Bold");
        row("lead", FontRole::Lead28, 28, 800, "ExtraBold");
        row("body-bold", FontRole::BodyBold15, 15, 700, "Bold");
        row("body", FontRole::Body15, 15, 400, "Regular"); row("secondary", FontRole::Secondary14, 14, 400, "Regular"); row("caption", FontRole::Caption12, 12, 400, "Regular");
        row("body-strong", FontRole::BodyStrong15, 15, 600, "SemiBold"); row("secondary-strong", FontRole::SecondaryStrong14, 14, 600, "SemiBold"); row("caption-strong", FontRole::CaptionStrong12, 12, 600, "SemiBold");
    }
    void rolesResolveTheRealBundledFaces() {
        QFETCH(int, role); QFETCH(int, pixels); QFETCH(int, weight); QFETCH(QString, face);
        const auto f = KeefiFonts::font(FontRole(role));
        QCOMPARE(f.pixelSize(), pixels); QCOMPARE(int(f.weight()), weight);
        const auto raw = QRawFont::fromFont(f);
        QVERIFY(raw.isValid()); QCOMPARE(sourceWeight(raw), weight); QCOMPARE(raw.styleName(), face);
        const bool number = role <= int(FontRole::Number11) || role == int(FontRole::Logo22);
        QCOMPARE(raw.familyName(), number ? KeefiFonts::numberFamily() : KeefiFonts::bodyFamily());
        if (number) { QCOMPARE(pixels % 11, 0); QVERIFY(!f.bold()); QCOMPARE(f.styleName(), QStringLiteral("Regular")); }
    }
    void logoTrackingPreservesNumericValueSpacing() {
        const auto logo = KeefiFonts::font(FontRole::Logo22);
        const auto value = KeefiFonts::font(FontRole::Value22);
        QCOMPARE(logo.letterSpacingType(), QFont::AbsoluteSpacing);
        QVERIFY(qAbs(logo.letterSpacing() - 1.76) < 0.02); // Qt stores spacing as fixed point.
        const QFont unchangedSpacing;
        QCOMPARE(value.letterSpacingType(), unchangedSpacing.letterSpacingType());
        QCOMPARE(value.letterSpacing(), unchangedSpacing.letterSpacing());
    }
    void mixedKoreanNumberLineUsesPretendardFallback() {
        const QString text = QStringLiteral("내 평균 84,672 [2]");
        QTextLayout layout(text, KeefiFonts::font(FontRole::Number11));
        layout.beginLayout(); layout.createLine(); layout.endLayout();
        // Qt 6.8.3/CoreText returns zero for every retrieved string index.
        // Query known source ranges instead, so the test checks the actual
        // fallback face and glyphs rather than misattributing another run.
        for (const auto& range : QList<QPair<int, int>>{{0, 1}, {2, 2}, {5, 6}, {13, 1}}) {
            const auto runs = layout.glyphRuns(range.first, range.second);
            QVERIFY(!runs.isEmpty());
            const QString expected = range.first < 5 ? KeefiFonts::bodyFamily() : KeefiFonts::numberFamily();
            for (const auto& run : runs) {
                QCOMPARE(run.rawFont().familyName(), expected);
                QVERIFY(!run.glyphIndexes().isEmpty());
                for (const auto glyph : run.glyphIndexes()) QVERIFY(glyph != 0);
            }
        }
    }
    void explicitRolesSurviveGlobalStyleSheet() {
        QWidget owner; owner.setStyleSheet(KeefiTheme::localStyleSheet());
        for (const auto role : {FontRole::Hero44, FontRole::Title17, FontRole::Body15, FontRole::Secondary14, FontRole::Caption12}) {
            QLabel label(QStringLiteral("합성 한글"), &owner);
            const auto expected = KeefiFonts::font(role); label.setFont(expected);
            owner.ensurePolished(); label.ensurePolished();
            QCOMPARE(label.font().pixelSize(), expected.pixelSize());
            QCOMPARE(QRawFont::fromFont(label.font()).styleName(), QRawFont::fromFont(expected).styleName());
        }
    }
    void inheritedBodyAllowsRealStylesheetHeadingWeights() {
        QWidget owner; owner.setFont(KeefiFonts::font(FontRole::Body15));
        for (const int weight : {400, 500, 600, 700}) {
            QLabel label(QStringLiteral("합성 제목"), &owner);
            label.setStyleSheet(QStringLiteral("font-weight:%1;").arg(weight));
            owner.ensurePolished(); label.ensurePolished();
            const auto raw = QRawFont::fromFont(label.font());
            QCOMPARE(raw.familyName(), KeefiFonts::bodyFamily());
            QCOMPARE(sourceWeight(raw), weight);
        }
    }
    void styledNumericLabelsKeepDepartureRegular_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("quiet");
        QTest::newRow("hero") << QStringLiteral("hero") << false;
        QTest::newRow("hero-quiet") << QStringLiteral("hero") << true;
        QTest::newRow("headline") << QStringLiteral("assetDetailHeadline") << false;
        QTest::newRow("headline-quiet") << QStringLiteral("assetDetailHeadline") << true;
        QTest::newRow("observed") << QStringLiteral("observedAmount") << false;
    }
    void styledNumericLabelsKeepDepartureRegular() {
        QFETCH(QString, kind); QFETCH(bool, quiet);
        QWidget owner; owner.setStyleSheet(KeefiTheme::localStyleSheet());
        QLabel label(QStringLiteral("84,672"), &owner);
        if (kind == QStringLiteral("assetDetailHeadline")) label.setObjectName(kind);
        else label.setProperty("type", kind);
        label.setProperty("quiet", quiet);
        label.setFont(KeefiFonts::font(FontRole::Price44));
        owner.ensurePolished(); label.ensurePolished();
        const auto resolved = label.font();
        const auto raw = QRawFont::fromFont(resolved);
        QCOMPARE(raw.familyName(), KeefiFonts::numberFamily());
        QCOMPARE(sourceWeight(raw), 400); QCOMPARE(int(resolved.weight()), 400);
        QVERIFY(!resolved.bold());
    }
    void licensesShipUnmodified() {
        for (const auto& item : QList<QPair<QString, QByteArray>>{
                 {QStringLiteral("pretendard-OFL.txt"), QByteArray("b04538c9abec39a3db75108cf0af0fd9c77032fe8aa2cf38345b4d250e98e38e")},
                 {QStringLiteral("departure-mono-OFL.txt"), QByteArray("b65e42750f3cb65437a97f4dfd781c58f199259949db2341012be60e32500e83")}}) {
            QFile file(QStringLiteral(":/keefi/fonts/") + item.first); QVERIFY(file.open(QIODevice::ReadOnly));
            const auto original = file.readAll();
            QVERIFY(original.contains("SIL OPEN FONT LICENSE Version 1.1"));
            QCOMPARE(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex(), item.second);
        }
    }
    void noticesShipThroughProductInitialization_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("sha256");
        QTest::addColumn<int>("byteCount");
        QTest::newRow("project-license") << QStringLiteral("LICENSE")
            << QByteArray("d8a6cc31abc16b6748c7a21f21611f5a1ec33f67d22ca23d7da1c19b95496bee") << 34020;
        QTest::newRow("project-notice") << QStringLiteral("NOTICE")
            << QByteArray("6b3b8106eee5d39fc6662eadbbdac0bc5debd296c01d3c885c3edc0dda692f37") << 1009;
        QTest::newRow("third-party-notices") << QStringLiteral("THIRD-PARTY-NOTICES.md")
            << QByteArray("367bf9bf13238cb3dc13120396f3384222e9f543845e5d8a667255b4116e7741") << 4042;
        QTest::newRow("font-sources") << QStringLiteral("font-sources.json")
            << QByteArray("1c9d4c8141f89037921276f6db4c218b4d57755e601f4f7487c32ec470bcae6f") << 4038;
    }
    void noticesShipThroughProductInitialization() {
        QFETCH(QString, file);
        QFETCH(QByteArray, sha256);
        QFETCH(int, byteCount);
        // initTestCase follows the product path. No test-only resource registration.
        QFile input(QStringLiteral(":/keefi/notices/") + file);
        QVERIFY2(input.open(QIODevice::ReadOnly), qPrintable(input.fileName()));
        const auto original = input.readAll();
        QCOMPARE(original.size(), byteCount);
        QCOMPARE(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex(), sha256);
    }
    void syntheticSpecimen() {
        const auto directory = qEnvironmentVariable("KEEFI_FONT_RENDER_DIR");
        if (directory.isEmpty()) return;
        QVERIFY(QDir().mkpath(directory));
        for (const bool dark : {false, true}) {
            const auto colors = KeefiTheme::colorsFor(dark);
            QImage image(2200, 1380, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(2); image.fill(colors.bg);
            QPainter p(&image); p.setPen(colors.text);
            p.setFont(KeefiFonts::font(FontRole::Value22)); p.drawText(40, 52, QStringLiteral("KEEFI"));
            p.setFont(KeefiFonts::font(FontRole::Hero44)); p.drawText(40, 136, QStringLiteral("내 자산의 첫 화면"));
            p.setFont(KeefiFonts::font(FontRole::Body15)); p.drawText(40, 176, QStringLiteral("합성 자료 · Pretendard 한글과 Departure Mono 숫자 · 사용자 자료 없음"));
            p.setFont(KeefiFonts::font(FontRole::Title17)); p.drawText(40, 230, QStringLiteral("글꼴 역할과 크기"));
            p.setPen(colors.rule); p.drawLine(40, 250, 1060, 250); p.setPen(colors.text);
            p.setFont(KeefiFonts::font(FontRole::Price44)); p.drawText(40, 316, QStringLiteral("84,672.50"));
            p.setFont(KeefiFonts::font(FontRole::Value22)); p.drawText(40, 364, QStringLiteral("+12.34%  KEEFI"));
            p.setFont(KeefiFonts::font(FontRole::Number11)); p.drawText(40, 400, QStringLiteral("BTC/USDT PERP  4H  23:49 KST  [1]"));
            p.setFont(KeefiFonts::font(FontRole::Secondary14)); p.drawText(40, 452, QStringLiteral("내 평균 ")); const auto labelWidth = p.fontMetrics().horizontalAdvance(QStringLiteral("내 평균 "));
            p.setFont(KeefiFonts::font(FontRole::Number11)); p.drawText(40 + labelWidth, 452, QStringLiteral("84,672 [2]"));
            p.setPen(colors.text3); p.setFont(KeefiFonts::font(FontRole::Caption12)); p.drawText(40, 504, QStringLiteral("Retina 비교용 2x 이미지 · 숫자는 11·22·44px, 한글은 역할별 크기"));
            p.setFont(KeefiFonts::font(FontRole::BodyStrong15)); p.drawText(40, 554, QStringLiteral("한글 강조는 실제 SemiBold, 큰 제목은 실제 ExtraBold"));
            p.end(); QVERIFY(image.save(QDir(directory).filePath(dark ? "fonts-dark@2x.png" : "fonts-light@2x.png")));
        }
    }
};
QTEST_MAIN(FontsTest)
#include "tst_fonts.moc"
