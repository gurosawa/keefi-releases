#include "keefi/PlainToolTip.h"
#include "TooltipTestSupport.h"
#include "keefi/InfoButton.h"
#include <QDialog>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
using namespace keefi;
class InfoButtonTest final : public QObject {
    Q_OBJECT
  private slots:
    void plainTooltipConversion_data();
    void plainTooltipConversion();
    void titleTooltipsKeepMarkupLiteral_data();
    void titleTooltipsKeepMarkupLiteral();
    void sourceTooltipsKeepMarkupLiteral_data();
    void sourceTooltipsKeepMarkupLiteral();
    void keyboardCanOpenAndCloseInformation_data();
    void keyboardCanOpenAndCloseInformation();
    void mouseUsesTheSamePlainInformation();
    void privateAccessDefaultsToDeniedAndLockPurgesOpenWidgets();
    void revokingAccessRejectsLatePrivateInformation();
    void deletingTheTriggerClosesTheOwnedDialogSafely();
};
void InfoButtonTest::keyboardCanOpenAndCloseInformation_data() {
    QTest::addColumn<int>("key");QTest::newRow("space")<<int(Qt::Key_Space);QTest::newRow("return")<<int(Qt::Key_Return);QTest::newRow("enter")<<int(Qt::Key_Enter);
}
void InfoButtonTest::keyboardCanOpenAndCloseInformation() {
    QFETCH(int,key);QWidget host;auto* layout=new QVBoxLayout(&host);auto* button=new InfoButton(&host);layout->addWidget(button);
    button->setInformation("시장 지표","공개 원천 설명");host.show();host.activateWindow();button->setFocus();
    QVERIFY(button->focusPolicy()&Qt::TabFocus);QCOMPARE(button->accessibleName(),QString("시장 지표 정보 보기"));
    QSignalSpy opened(button,&InfoButton::informationOpened);QTest::keyClick(button,Qt::Key(key));QTRY_VERIFY(button->informationOpen());QCOMPARE(opened.count(),1);
    auto* dialog=button->findChild<QDialog*>("infoDialog");QVERIFY(dialog);QVERIFY(dialog->isVisible());
    auto* text=dialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);QVERIFY(text->isReadOnly());QCOMPARE(text->toPlainText(),QString("공개 원천 설명"));
    QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!button->informationOpen());QTRY_VERIFY(button->hasFocus());
}
void InfoButtonTest::mouseUsesTheSamePlainInformation() {
    InfoButton button;button.setInformation("<b>합성 제목</b>","<script>synthetic</script> & https://example.invalid/public");button.show();
    QTest::mouseClick(&button,Qt::LeftButton);QTRY_VERIFY(button.informationOpen());
    auto* text=button.findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);QCOMPARE(text->toPlainText(),QString("<script>synthetic</script> & https://example.invalid/public"));
    button.setInformation("새 공개 제목","갱신된 공개 근거");QCOMPARE(text->toPlainText(),QString("갱신된 공개 근거"));
}
void InfoButtonTest::privateAccessDefaultsToDeniedAndLockPurgesOpenWidgets() {
    InfoButton button;button.setInformation("공개 지표","PUBLIC_SOURCE_REMAINS");
    QVERIFY(!button.setPrivateInformation("FAKE_PRIVATE_CANARY"));button.setPrivateAccess(true);QVERIFY(button.setPrivateInformation("FAKE_PRIVATE_CANARY"));
    button.show();button.openInformation();QTRY_VERIFY(button.informationOpen());auto* privateText=button.findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(privateText);QVERIFY(privateText->toPlainText().contains("FAKE_PRIVATE_CANARY"));
    button.clearPrivate();QVERIFY(!button.informationOpen());for(const auto* text:button.findChildren<QPlainTextEdit*>())QVERIFY(!text->toPlainText().contains("FAKE_PRIVATE_CANARY"));
    QVERIFY(!button.setPrivateInformation("FAKE_PRIVATE_LATE"));QVERIFY(!button.accessibleDescription().contains("FAKE_PRIVATE"));QVERIFY(!button.toolTip().contains("FAKE_PRIVATE"));
    button.openInformation();QTRY_VERIFY(button.informationOpen());
    bool publicFound=false;for(const auto* text:button.findChildren<QPlainTextEdit*>()){QVERIFY(!text->toPlainText().contains("FAKE_PRIVATE"));publicFound|=text->toPlainText().contains("PUBLIC_SOURCE_REMAINS");}QVERIFY(publicFound);
}
void InfoButtonTest::revokingAccessRejectsLatePrivateInformation() {
    InfoButton button;button.setPrivateAccess(true);QVERIFY(button.setPrivateInformation("FAKE_PRIVATE_LINK"));button.setPrivateAccess(false);
    QVERIFY(!button.setPrivateInformation("FAKE_PRIVATE_LATE"));button.setInformation("공개 제목","공개 본문");button.show();button.openInformation();QTRY_VERIFY(button.informationOpen());
    for(const auto* text:button.findChildren<QPlainTextEdit*>())QVERIFY(!text->toPlainText().contains("FAKE_PRIVATE"));
}
void InfoButtonTest::deletingTheTriggerClosesTheOwnedDialogSafely() {
    QWidget host;auto* button=new InfoButton(&host);button->setInformation("정보","공개 본문");host.show();button->openInformation();QTRY_VERIFY(button->informationOpen());
    QPointer<QDialog> dialog=button->findChild<QDialog*>("infoDialog");QVERIFY(dialog);delete button;QVERIFY(dialog.isNull());
}
void InfoButtonTest::titleTooltipsKeepMarkupLiteral_data() {
    QTest::addColumn<QString>("input");
    QTest::newRow("format") << QStringLiteral("<b>합성 제목</b>");
    QTest::newRow("image") << QStringLiteral("<img src='synthetic:pixel'>합성");
    QTest::newRow("anchor") << QStringLiteral("<a href='synthetic:link'>합성</a>");
}
void InfoButtonTest::titleTooltipsKeepMarkupLiteral() {
    QFETCH(QString, input);
    InfoButton button;
    button.setInformation(input, QStringLiteral("합성 공개 본문"));
    const auto expected = input + QStringLiteral(" 정보 보기");
    QCOMPARE(button.accessibleName(), expected);
    const auto shown = keefi::test::projectTooltip(button.toolTip());
    QCOMPARE(shown.text, expected);
    QVERIFY(!shown.image);
    QVERIFY(!shown.anchor);
    QCOMPARE(shown.resourceRequests, 0);
}
void InfoButtonTest::sourceTooltipsKeepMarkupLiteral_data() {
    titleTooltipsKeepMarkupLiteral_data();
}
void InfoButtonTest::sourceTooltipsKeepMarkupLiteral() {
    QFETCH(QString, input);
    InfoButton button;
    button.setPublicSourceInformation(QStringLiteral("합성 원천"), input, {});
    QCOMPARE(button.accessibleName(), QStringLiteral("합성 원천 정보 보기"));
    const auto shown = keefi::test::projectTooltip(button.toolTip());
    QCOMPARE(shown.text, input);
    QVERIFY(!shown.image);
    QVERIFY(!shown.anchor);
    QCOMPARE(shown.resourceRequests, 0);
}
void InfoButtonTest::plainTooltipConversion_data() {
    QTest::addColumn<QString>("input");
    QTest::newRow("empty") << QString{};
    QTest::newRow("plain") << QStringLiteral("S&P 500 · 100.00 USD");
    QTest::newRow("comparison") << QStringLiteral("A & B < 10");
    QTest::newRow("unicode") << QStringLiteral("합성 한국어 · € 1.25");
    QTest::newRow("unrecognized-tag") << QStringLiteral("<synthetic>literal</synthetic>");
    QTest::newRow("second-line-tag") << QStringLiteral("plain first\n<b>literal</b>");
    QTest::newRow("format") << QStringLiteral("<b>literal & text</b>");
    QTest::newRow("entities") << QStringLiteral("&lt;b&gt;literal&lt;/b&gt;");
    QTest::newRow("spaces") << QStringLiteral("<b>two  spaces</b>  retained");
    QTest::newRow("lines") << QStringLiteral("<b>one</b>\ntwo\n\nfour");
    QTest::newRow("image") << QStringLiteral("<img src='synthetic:pixel'>literal");
    QTest::newRow("anchor") << QStringLiteral("<a href='synthetic:link'>literal</a>");
}
void InfoButtonTest::plainTooltipConversion() {
    QFETCH(QString, input);
    const auto tooltip = plainToolTip(input);
    if (!Qt::mightBeRichText(input)) QCOMPARE(tooltip, input);
    const auto shown = keefi::test::projectTooltip(tooltip);
    QCOMPARE(shown.text, input);
    QVERIFY(!shown.image);
    QVERIFY(!shown.anchor);
    QCOMPARE(shown.resourceRequests, 0);
}
QTEST_MAIN(InfoButtonTest)
#include "tst_info_button.moc"
