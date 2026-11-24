#include "keefi/MovementColorPolicy.h"
#include "keefi/KeefiTheme.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
using namespace keefi;
class MovementColorPolicyTest final : public QObject {
    Q_OBJECT
  private slots:
    void defaultAndKoreanDirectionsKeepTheSameArrow();
    void neutralMarksRemainVisibleInBothThemes();
    void identicalSchemeDoesNotNotify();
    void ancestorLookupIsPerWindowAndClosestOwnerWins();
    void policyDestructionFallsBackWithoutBorrowingAnotherWindow();
    void publicStateContainsOnlySchemaAndScheme();
    void invalidStateIsAtomic_data();
    void invalidStateIsAtomic();
};
void MovementColorPolicyTest::defaultAndKoreanDirectionsKeepTheSameArrow() {
    MovementColorPolicy policy;
    QCOMPARE(policy.scheme(),MovementScheme::GreenRed);
    QCOMPARE(policy.foreground(MovementDirection::Up),themeColors().upText);
    QCOMPARE(policy.foreground(MovementDirection::Down),themeColors().downText);
    QCOMPARE(MovementColorPolicy::marker(MovementDirection::Up),QString("▲"));
    QCOMPARE(MovementColorPolicy::marker(MovementDirection::Down),QString("▼"));
    QVERIFY(MovementColorPolicy::marker(MovementDirection::Unknown).isEmpty());
    QVERIFY(MovementColorPolicy::marker(MovementDirection::Flat).isEmpty());
    const auto neutral=policy.foreground(MovementDirection::Flat),unknown=policy.foreground(MovementDirection::Unknown);
    const auto redHeat=policy.heat(MovementDirection::Down,2);
    policy.setScheme(MovementScheme::KoreanRedBlue);
    QCOMPARE(policy.foreground(MovementDirection::Up),themeColors().downText);
    QCOMPARE(policy.foreground(MovementDirection::Down),themeColors().koreanDownText);
    QCOMPARE(policy.foreground(MovementDirection::Flat),neutral);QCOMPARE(policy.foreground(MovementDirection::Unknown),unknown);
    QCOMPARE(policy.heat(MovementDirection::Up,2),redHeat);
    QVERIFY(policy.heat(MovementDirection::Down,2)!=redHeat);
    QCOMPARE(MovementColorPolicy::marker(MovementDirection::Up),QString("▲"));
}
void MovementColorPolicyTest::neutralMarksRemainVisibleInBothThemes() {
    MovementColorPolicy policy;
    for (auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
        KeefiTheme::instance()->setMode(mode);
        for (auto direction : {MovementDirection::Flat, MovementDirection::Unknown}) {
            QCOMPARE(policy.fill(direction), themeColors().text3);
            QVERIFY(KeefiTheme::contrast(policy.fill(direction), themeColors().bg) >= 4.5);
            QCOMPARE(policy.heat(direction, 2), themeColors().surface);
        }
    }
    KeefiTheme::instance()->setMode(ThemeMode::System);
}
void MovementColorPolicyTest::identicalSchemeDoesNotNotify() {
    MovementColorPolicy policy;QSignalSpy changed(&policy,&MovementColorPolicy::changed);
    policy.setScheme(MovementScheme::GreenRed);QCOMPARE(changed.count(),0);
    policy.setScheme(MovementScheme::KoreanRedBlue);QCOMPARE(changed.count(),1);
    const auto bytes=policy.publicState();policy.setScheme(MovementScheme::KoreanRedBlue);QCOMPARE(changed.count(),1);QCOMPARE(policy.publicState(),bytes);
    policy.setScheme(static_cast<MovementScheme>(99));QCOMPARE(changed.count(),1);QCOMPARE(policy.publicState(),bytes);
}
void MovementColorPolicyTest::ancestorLookupIsPerWindowAndClosestOwnerWins() {
    QWidget first,second;MovementColorPolicy firstPolicy(&first),secondPolicy(&second);
    QWidget branch(&first);QLabel leaf(&branch),other(&second);firstPolicy.setScheme(MovementScheme::KoreanRedBlue);
    QCOMPARE(MovementColorPolicy::forContext(&leaf),&firstPolicy);QCOMPARE(MovementColorPolicy::forContext(&other),&secondPolicy);
    QCOMPARE(MovementColorPolicy::colorFor(&leaf,MovementDirection::Down),themeColors().koreanDownText);
    QCOMPARE(MovementColorPolicy::colorFor(&other,MovementDirection::Down),themeColors().downText);
    QLabel late(&branch);QCOMPARE(MovementColorPolicy::schemeFor(&late),MovementScheme::KoreanRedBlue);
    MovementColorPolicy nestedPolicy(&branch);QCOMPARE(MovementColorPolicy::forContext(&late),&nestedPolicy);
    QCOMPARE(MovementColorPolicy::schemeFor(&late),MovementScheme::GreenRed);
    QWidget unrelated;QVERIFY(!MovementColorPolicy::forContext(&unrelated));
    QCOMPARE(MovementColorPolicy::colorFor(&unrelated,MovementDirection::Up),themeColors().upText);
}
void MovementColorPolicyTest::policyDestructionFallsBackWithoutBorrowingAnotherWindow() {
    QWidget host,other;QLabel leaf(&host);auto* policy=new MovementColorPolicy(&host);MovementColorPolicy otherPolicy(&other);
    policy->setScheme(MovementScheme::KoreanRedBlue);otherPolicy.setScheme(MovementScheme::KoreanRedBlue);
    QPointer<MovementColorPolicy> observed=MovementColorPolicy::forContext(&leaf);QVERIFY(observed);
    delete policy;QVERIFY(observed.isNull());QVERIFY(!MovementColorPolicy::forContext(&leaf));
    QCOMPARE(MovementColorPolicy::colorFor(&leaf,MovementDirection::Down),themeColors().downText);
}
void MovementColorPolicyTest::publicStateContainsOnlySchemaAndScheme() {
    MovementColorPolicy policy;policy.setScheme(MovementScheme::KoreanRedBlue);const auto bytes=policy.publicState();
    const auto root=QJsonDocument::fromJson(bytes).object();QCOMPARE(root.size(),2);
    QCOMPARE(root["schema"].toString(),QString("keefi-movement-colors/1"));QCOMPARE(root["scheme"].toString(),QString("korean-red-blue"));
    MovementColorPolicy restored;QVERIFY(restored.restorePublicState(bytes));QCOMPARE(restored.scheme(),MovementScheme::KoreanRedBlue);
    QCOMPARE(restored.publicState(),bytes);QSignalSpy changed(&restored,&MovementColorPolicy::changed);QVERIFY(restored.restorePublicState(bytes));QCOMPARE(changed.count(),0);
}
void MovementColorPolicyTest::invalidStateIsAtomic_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("extra-private")<<QByteArray(R"({"schema":"keefi-movement-colors/1","scheme":"green-red","account":"FAKE_PRIVATE_CANARY"})");
    QTest::newRow("unknown-scheme")<<QByteArray(R"({"schema":"keefi-movement-colors/1","scheme":"red-green"})");
    QTest::newRow("wrong-version")<<QByteArray(R"({"schema":"keefi-movement-colors/2","scheme":"green-red"})");
    QTest::newRow("wrong-type")<<QByteArray(R"({"schema":"keefi-movement-colors/1","scheme":false})");
    QTest::newRow("duplicate-key")<<QByteArray(R"({"schema":"keefi-movement-colors/1","scheme":"korean-red-blue","scheme":"green-red"})");
    QTest::newRow("invalid-utf8")<<(QByteArray(R"({"schema":"keefi-movement-colors/1","scheme":")")+char(0xff)+QByteArray("\"}"));
    QTest::newRow("oversize")<<QByteArray(MovementColorPolicy::MaxStateBytes+1,' ');
}
void MovementColorPolicyTest::invalidStateIsAtomic() {
    QFETCH(QByteArray,bytes);MovementColorPolicy policy;policy.setScheme(MovementScheme::KoreanRedBlue);const auto before=policy.publicState();
    QCOMPARE(policy.scheme(),MovementScheme::KoreanRedBlue);QSignalSpy changed(&policy,&MovementColorPolicy::changed);QString error;
    QVERIFY(!policy.restorePublicState(bytes,&error));QVERIFY(!error.isEmpty());QCOMPARE(policy.scheme(),MovementScheme::KoreanRedBlue);QCOMPARE(policy.publicState(),before);QCOMPARE(changed.count(),0);
}
QTEST_MAIN(MovementColorPolicyTest)
#include "tst_movement_color_policy.moc"
