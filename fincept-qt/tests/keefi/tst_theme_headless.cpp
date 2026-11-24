#include "keefi/KeefiTheme.h"
#include "keefi/MovementColorPolicy.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
using namespace keefi;
class HeadlessThemeTest final : public QObject {
    Q_OBJECT
  private slots:
    void movementStateAndThemeTokensDoNotRequireGuiApplication() {
        QCOMPARE(QString(QCoreApplication::instance()->metaObject()->className()),QString("QCoreApplication"));
        MovementColorPolicy policy;
        policy.setScheme(MovementScheme::KoreanRedBlue);
        const auto before=policy.publicState();
        QCOMPARE(QJsonDocument::fromJson(before).object()["scheme"].toString(),QString("korean-red-blue"));
        MovementColorPolicy restored; QVERIFY(restored.restorePublicState(before)); QCOMPARE(restored.publicState(),before);
        KeefiTheme::instance()->setMode(ThemeMode::Dark);
        QCOMPARE(restored.foreground(MovementDirection::Down),KeefiTheme::colorsFor(true).koreanDownText);
        KeefiTheme::instance()->setMode(ThemeMode::System);
        QVERIFY(!KeefiTheme::instance()->isDark()); QCOMPARE(restored.publicState(),before);
    }
};
QTEST_GUILESS_MAIN(HeadlessThemeTest)
#include "tst_theme_headless.moc"
