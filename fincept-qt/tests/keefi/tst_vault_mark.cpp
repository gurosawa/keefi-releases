#include "keefi/KeefiTheme.h"
#include "keefi/PressRevealButton.h"
#include "keefi/VaultDialog.h"
#include "keefi/VaultGate.h"
#include "keefi/VaultMark.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMetaMethod>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using namespace keefi;
using Letter = VaultMark::Letter;

namespace {
constexpr int AllRules = 0b11111;
int distance(const QColor& a, const QColor& b) {
    return qMax(qAbs(a.red() - b.red()), qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue())));
}
int pixels(const QImage& image, const QColor& colour) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) count += image.pixelColor(x, y).rgb() == colour.rgb();
    return count;
}
bool save(QWidget& widget, const QString& name) {
    const auto dir = qEnvironmentVariable("KEEFI_MOTION_RENDER_DIR");
    if (dir.isEmpty()) return true;
    return QDir().mkpath(dir) && widget.grab().save(QDir(dir).filePath(name));
}
struct PaintObserver final : QObject {
    int painted = 0;
    bool eventFilter(QObject*, QEvent* event) override { painted += event->type() == QEvent::Paint; return false; }
};
QLineEdit* edit(QWidget* owner, const char* name) { return owner->findChild<QLineEdit*>(name); }
QPushButton* submit(QWidget* owner) { return owner->findChild<QPushButton*>("submitVault"); }
}

class VaultMarkTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { KeefiTheme::instance()->setMode(ThemeMode::Light); }
    void lettersFollowTheFiveRules();
    void unmetLettersRiseOnAnExponentialCurve();
    void readinessLightsTheWordLeftToRight();
    void holdSettlesAndPaintsOneStillFrame();
    void rejectionShakesAndLastsUntilNewInput();
    void hiddenMarkJumpsStraightToItsState();
    void captionsChangeHeightAndDescription();
    void eachStateUsesItsThemeColour_data();
    void eachStateUsesItsThemeColour();
    void formReportsRuleFlagsOnly();
    void createFormFlagsMatchTheRules_data();
    void createFormFlagsMatchTheRules();
    void unlockFormLightsAllOrNothing();
    void embeddedFormIsQuiet();
    void gateWordmarkFollowsTheVisibleForm();
    void gateHoldsAStillFrameThenRejects();
    void gateFollowsTheVaultFile();
    void gateRendersForReview_data();
    void gateRendersForReview();
};

void VaultMarkTest::lettersFollowTheFiveRules() {
    VaultMark mark; mark.show(); QVERIFY(QTest::qWaitForWindowExposed(&mark));
    QCOMPARE(mark.text(), QString("KEEFI"));
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark.letter(i), Letter::Off);
    // Lower case, digit: E (third letter) and F.
    mark.setProgress(0b01100, false);
    const QList<Letter> expected{Letter::Off, Letter::Off, Letter::On, Letter::On, Letter::Off};
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark.letter(i), expected[i]);
    QTRY_VERIFY(mark.settled());
    QCOMPARE(mark.shownDrop(2), 0.0); QCOMPARE(mark.shownDrop(0), VaultMark::OffDrop);
    mark.setProgress(AllRules, true);
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark.letter(i), Letter::Ready);
    mark.setProgress(AllRules, false); // the confirmation stopped matching
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark.letter(i), Letter::On);
    mark.reset();
    for (int i = 0; i < VaultMark::Letters; ++i) { QCOMPARE(mark.letter(i), Letter::Off); QCOMPARE(mark.shownDrop(i), VaultMark::OffDrop); }
}

void VaultMarkTest::unmetLettersRiseOnAnExponentialCurve() {
    VaultMark mark; mark.show(); QVERIFY(QTest::qWaitForWindowExposed(&mark));
    mark.setProgress(0b00001, false);
    QVERIFY2(mark.shownDrop(0) > 0, "The letter moves up; it does not jump.");
    // A quarter of 450 ms in, a linear rise would be 25% of the way; OutExpo is
    // at 82%. A late timer only moves it further, so 65% cannot flake.
    QTest::qWait(113);
    QVERIFY2(mark.shownDrop(0) <= VaultMark::OffDrop * 0.35, qPrintable(QString::number(mark.shownDrop(0))));
    for (int i = 1; i < VaultMark::Letters; ++i) QCOMPARE(mark.shownDrop(i), VaultMark::OffDrop);
    qreal previous = mark.shownDrop(0); QElapsedTimer clock; clock.start();
    while (!mark.settled() && clock.elapsed() < 3000) { QTest::qWait(16); QVERIFY(mark.shownDrop(0) <= previous); previous = mark.shownDrop(0); }
    QCOMPARE(mark.shownDrop(0), 0.0);
    // Losing the rule drops it back the same way.
    mark.setProgress(0, false); QVERIFY(mark.shownDrop(0) < VaultMark::OffDrop); QTRY_COMPARE(mark.shownDrop(0), VaultMark::OffDrop);
}

void VaultMarkTest::readinessLightsTheWordLeftToRight() {
    VaultMark mark; mark.show(); QVERIFY(QTest::qWaitForWindowExposed(&mark));
    const auto& colours = themeColors();
    mark.setProgress(AllRules, false); QTRY_VERIFY(mark.settled());
    mark.setProgress(AllRules, true);
    // The last letter waits 4 x 40 ms before it starts.
    QVERIFY(distance(mark.shownColour(4), colours.text) <= 2);
    QTest::qWait(100);
    QVERIFY(distance(mark.shownColour(0), colours.accent) < distance(mark.shownColour(4), colours.accent));
    QTRY_VERIFY(mark.settled());
    for (int i = 0; i < VaultMark::Letters; ++i) QVERIFY(distance(mark.shownColour(i), colours.accent) <= 2);
}

void VaultMarkTest::holdSettlesAndPaintsOneStillFrame() {
    VaultMark mark; mark.show(); QVERIFY(QTest::qWaitForWindowExposed(&mark));
    mark.setProgress(AllRules, true); QVERIFY(!mark.settled());
    PaintObserver paint; mark.installEventFilter(&paint);
    // The password check blocks the thread right after hold() returns: the
    // frame it leaves on screen must already be the finished one.
    mark.hold();
    QVERIFY2(paint.painted > 0, "hold() paints without returning to the event loop.");
    QVERIFY(mark.settled());
    for (int i = 0; i < VaultMark::Letters; ++i) {
        QCOMPARE(mark.letter(i), Letter::Ready); QCOMPARE(mark.shownDrop(i), 0.0);
        QVERIFY(distance(mark.shownColour(i), themeColors().accent) <= 2);
    }
}

void VaultMarkTest::rejectionShakesAndLastsUntilNewInput() {
    VaultMark mark; mark.show(); QVERIFY(QTest::qWaitForWindowExposed(&mark));
    mark.setProgress(AllRules, true); mark.hold(); mark.reject();
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark.letter(i), Letter::Rejected);
    QVERIFY(!mark.settled());
    bool moved = false; QElapsedTimer clock; clock.start();
    while (!mark.settled() && clock.elapsed() < 3000) { QTest::qWait(10); moved |= qAbs(mark.shake()) > 0.5; }
    QVERIFY(moved); QCOMPARE(mark.shake(), 0.0);
    // The form clearing itself is not new input.
    mark.setProgress(0, false);
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark.letter(i), Letter::Rejected);
    mark.setProgress(0b00001, false);
    QCOMPARE(mark.letter(0), Letter::On); QCOMPARE(mark.letter(4), Letter::Off);
    mark.reject(); mark.setProgress(AllRules, true); // typing again during the shake stops it
    QCOMPARE(mark.shake(), 0.0); QCOMPARE(mark.letter(2), Letter::Ready);
}

void VaultMarkTest::hiddenMarkJumpsStraightToItsState() {
    VaultMark mark;
    mark.setProgress(0b10000, false);
    QVERIFY(mark.settled()); QCOMPARE(mark.shownDrop(4), 0.0); QCOMPARE(mark.shownDrop(0), VaultMark::OffDrop);
    mark.reject(); QVERIFY(mark.settled()); QCOMPARE(mark.shake(), 0.0);
}

void VaultMarkTest::captionsChangeHeightAndDescription() {
    VaultMark mark;
    QCOMPARE(mark.sizeHint().width(), 5 * 52 + 4 * 14);
    const int withCaptions = mark.sizeHint().height();
    mark.setProgress(0b00101, false);
    QCOMPARE(mark.accessibleDescription(), QString("암호 조건 2/5 충족"));
    mark.setCaptionsVisible(false);
    QVERIFY(mark.sizeHint().height() < withCaptions);
    QVERIFY(mark.accessibleDescription().isEmpty());
    QCOMPARE(mark.accessibleName(), QString("KEEFI"));
    QCOMPARE(mark.focusPolicy(), Qt::NoFocus);
}

void VaultMarkTest::eachStateUsesItsThemeColour_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false; QTest::newRow("dark") << true;
}
void VaultMarkTest::eachStateUsesItsThemeColour() {
    QFETCH(bool, dark); KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
    QWidget host; host.setAutoFillBackground(true); auto palette = host.palette(); palette.setColor(QPalette::Window, themeColors().bg); host.setPalette(palette);
    auto* mark = new VaultMark(&host); host.resize(mark->sizeHint() + QSize(24, 24)); mark->move(12, 12); mark->resize(mark->sizeHint());
    host.show(); QVERIFY(QTest::qWaitForWindowExposed(&host));
    const auto& colours = themeColors(); const QString theme = dark ? "dark" : "light";
    auto image = mark->grab().toImage();
    QVERIFY(pixels(image, colours.field) > 0); QCOMPARE(pixels(image, colours.accent), 0); QCOMPARE(pixels(image, colours.danger), 0);
    QVERIFY(save(host, QString("mark-none-%1.png").arg(theme)));
    mark->setProgress(0b01110, false); QTRY_VERIFY(mark->settled()); image = mark->grab().toImage();
    QVERIFY(pixels(image, colours.text) > 0); QVERIFY(pixels(image, colours.field) > 0);
    QVERIFY(save(host, QString("mark-three-%1.png").arg(theme)));
    mark->setProgress(AllRules, true); QTRY_VERIFY(mark->settled()); image = mark->grab().toImage();
    QVERIFY(pixels(image, colours.accent) > 0); QCOMPARE(pixels(image, colours.field), 0);
    QVERIFY(save(host, QString("mark-ready-%1.png").arg(theme)));
    mark->reject(); QTRY_VERIFY(mark->settled()); image = mark->grab().toImage();
    QVERIFY(pixels(image, colours.danger) > 0); QCOMPARE(pixels(image, colours.accent), 0);
    QVERIFY(save(host, QString("mark-rejected-%1.png").arg(theme)));
    // A form that cannot be used shows every letter unlit.
    mark->setProgress(AllRules, true); mark->setEnabled(false); QTRY_VERIFY(mark->settled()); image = mark->grab().toImage();
    QCOMPARE(pixels(image, colours.accent), 0);
}

void VaultMarkTest::formReportsRuleFlagsOnly() {
    // The gate may learn which rules are met and whether the form can be sent.
    // A text parameter here would let password bytes leave the form.
    const auto& meta = VaultDialog::staticMetaObject; int found = 0;
    for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
        const auto method = meta.method(index);
        if (method.methodType() != QMetaMethod::Signal || method.name() != "ruleProgressChanged") continue;
        ++found;
        QCOMPARE(method.parameterCount(), 2);
        QCOMPARE(method.parameterMetaType(0).id(), int(QMetaType::Int));
        QCOMPARE(method.parameterMetaType(1).id(), int(QMetaType::Bool));
    }
    QCOMPARE(found, 1);
    const auto& mark = VaultMark::staticMetaObject;
    QCOMPARE(mark.methodCount(), mark.methodOffset()); // no slot of its own to feed text into
}

void VaultMarkTest::createFormFlagsMatchTheRules_data() {
    QTest::addColumn<QString>("password"); QTest::addColumn<QString>("confirmation");
    QTest::addColumn<int>("rules"); QTest::addColumn<bool>("ready");
    QTest::newRow("empty") << "" << "" << 0 << false;
    QTest::newRow("lower") << "a" << "" << 0b00100 << false;
    QTest::newRow("upper-lower-digit") << "aA1" << "" << 0b01110 << false;
    QTest::newRow("four-classes") << "aA1!" << "" << 0b11110 << false;
    QTest::newRow("valid-unconfirmed") << "aA1!aaaaaaaa" << "" << AllRules << false;
    QTest::newRow("valid-mismatch") << "aA1!aaaaaaaa" << "aA1!aaaaaaab" << AllRules << false;
    QTest::newRow("valid-confirmed") << "aA1!aaaaaaaa" << "aA1!aaaaaaaa" << AllRules << true;
    QTest::newRow("space-is-not-a-symbol") << "aA1 aaaaaaaa" << "aA1 aaaaaaaa" << 0b01111 << false;
    QTest::newRow("long-without-digit") << "aA!aaaaaaaaaa" << "aA!aaaaaaaaaa" << 0b10111 << false;
}
void VaultMarkTest::createFormFlagsMatchTheRules() {
    QFETCH(QString, password); QFETCH(QString, confirmation); QFETCH(int, rules); QFETCH(bool, ready);
    VaultDialog form(VaultDialog::Mode::Create, nullptr, VaultDialog::Presentation::Embedded);
    int lastRules = -1; bool lastReady = false;
    connect(&form, &VaultDialog::ruleProgressChanged, this, [&](int met, bool canSubmit) { lastRules = met; lastReady = canSubmit; });
    edit(&form, "newPassword")->setText("seed"); // leave the initial empty state so every row reports
    edit(&form, "newPassword")->setText(password); edit(&form, "confirmPassword")->setText(confirmation);
    QCOMPARE(lastRules, rules); QCOMPARE(lastReady, ready);
    QCOMPARE(lastReady, submit(&form)->isEnabled());
}

void VaultMarkTest::unlockFormLightsAllOrNothing() {
    VaultDialog form(VaultDialog::Mode::Unlock, nullptr, VaultDialog::Presentation::Embedded); form.show();
    QSignalSpy progress(&form, &VaultDialog::ruleProgressChanged);
    edit(&form, "currentPassword")->setText("x");
    QCOMPARE(progress.count(), 1); QCOMPARE(progress.last()[0].toInt(), AllRules); QVERIFY(progress.last()[1].toBool());
    edit(&form, "currentPassword")->setText("xy"); QCOMPARE(progress.count(), 1); // nothing new to report
    edit(&form, "currentPassword")->clear();
    QCOMPARE(progress.last()[0].toInt(), 0); QVERIFY(!progress.last()[1].toBool());
    // Starting work clears the field but keeps the submitted pose until the result.
    edit(&form, "currentPassword")->setText("synthetic"); const int before = progress.count(); int duringWork = -1;
    connect(&form, &VaultDialog::submitted, this, [&] { duringWork = progress.count(); });
    submit(&form)->click();
    QVERIFY(edit(&form, "currentPassword")->text().isEmpty()); QCOMPARE(duringWork, before);
    form.setError("합성 오류"); QCOMPARE(progress.last()[0].toInt(), 0);
}

void VaultMarkTest::embeddedFormIsQuiet() {
    VaultDialog form(VaultDialog::Mode::Create, nullptr, VaultDialog::Presentation::Embedded);
    form.resize(340, 300); form.show(); QVERIFY(QTest::qWaitForWindowExposed(&form));
    QVERIFY(!form.findChild<QLabel*>("vaultExplanation")->isVisibleTo(&form));
    QVERIFY(!form.findChild<QLabel*>("passwordRequirements")->isVisibleTo(&form));
    auto* password = edit(&form, "newPassword"); auto* confirmation = edit(&form, "confirmPassword");
    QCOMPARE(password->placeholderText(), QString("새 암호")); QCOMPARE(confirmation->placeholderText(), QString("암호 확인"));
    QCOMPARE(password->accessibleName(), QString("새 암호"));
    QVERIFY(password->accessibleDescription().contains("12자"));
    // The only words under the button: what happens if the password is lost.
    auto* loss = form.findChild<QLabel*>("vaultLossWarning"); QVERIFY(loss && loss->isVisibleTo(&form));
    QCOMPARE(loss->text(), QString("암호를 잊으면 저장한 키를 되찾을 수 없어요."));
    for (auto* field : {password, confirmation}) {
        auto* reveal = form.findChild<PressRevealButton*>(field->objectName() + "Reveal"); QVERIFY(reveal);
        QCOMPARE(reveal->parentWidget(), field->parentWidget()); // a sibling drawn over the field
        QCOMPARE(reveal->size(), QSize(28, 28));
        QVERIFY2(field->geometry().contains(reveal->geometry()), "The reveal control sits inside the field's rectangle.");
        QVERIFY(reveal->text().isEmpty()); QVERIFY(!reveal->icon().isNull());
        QTest::mousePress(reveal, Qt::LeftButton); QCOMPARE(field->echoMode(), QLineEdit::Normal);
        QTest::mouseRelease(reveal, Qt::LeftButton); QCOMPARE(field->echoMode(), QLineEdit::Password);
    }
    // A mismatch is the one rule message in words; the wordmark shows the rest.
    auto* error = form.findChild<QLabel*>("vaultError");
    password->setText("short"); QVERIFY(error->text().isEmpty());
    password->setText("aA1!aaaaaaaa"); confirmation->setText("aA1!aaaaaaab"); QCOMPARE(error->text(), QString("두 암호가 달라요."));
    // The error line keeps its place, so the button does not move.
    const auto buttonBefore = submit(&form)->geometry(); confirmation->setText("aA1!aaaaaaaa"); QCoreApplication::processEvents();
    QVERIFY(error->text().isEmpty()); QCOMPARE(submit(&form)->geometry(), buttonBefore);
}

void VaultMarkTest::gateWordmarkFollowsTheVisibleForm() {
    VaultGate gate(false, {}); gate.setSecurityReady(true); gate.resize(1280, 820); gate.show();
    QVERIFY(QTest::qWaitForWindowExposed(&gate));
    auto* mark = gate.findChild<VaultMark*>("vaultGateBrand"); QVERIFY(mark); QVERIFY(mark->isVisibleTo(&gate));
    QVERIFY(mark->captionsVisible()); QVERIFY(mark->isEnabled());
    auto* create = gate.findChild<VaultDialog*>("vaultGateCreateForm");
    auto* password = edit(create, "newPassword"); auto* confirmation = edit(create, "confirmPassword");
    password->setFocus(); QTest::keyClicks(password, "aA1");
    const QList<Letter> expected{Letter::Off, Letter::On, Letter::On, Letter::On, Letter::Off};
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), expected[i]);
    QTest::keyClicks(password, "!aaaaaaaa");
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::On);
    QVERIFY(!submit(create)->isEnabled());
    confirmation->setFocus(); QTest::keyClicks(confirmation, "aA1!aaaaaaaa");
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::Ready);
    QVERIFY(submit(create)->isEnabled());
    confirmation->backspace();
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::On);
    // Leaving the page clears the form and the mark with it.
    gate.hide(); QVERIFY(password->text().isEmpty());
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::Off);
    gate.setSecurityReady(false); QVERIFY(!mark->isEnabled());
}

void VaultMarkTest::gateHoldsAStillFrameThenRejects() {
    VaultGate gate(true, QDateTime::currentDateTime()); gate.setSecurityReady(true); gate.resize(1280, 820); gate.show();
    QVERIFY(QTest::qWaitForWindowExposed(&gate));
    auto* mark = gate.findChild<VaultMark*>("vaultGateBrand"); QVERIFY(!mark->captionsVisible());
    auto* unlock = gate.findChild<VaultDialog*>("vaultGateUnlockForm"); auto* field = edit(unlock, "currentPassword");
    field->setFocus(); QTest::keyClicks(field, "ab");
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::Ready);
    // What the blocking password check would find on screen, then its failure.
    bool still = false; QString received;
    connect(&gate, &VaultGate::submitted, this, [&](int, const QString& current) {
        still = mark->settled();
        for (int i = 0; i < VaultMark::Letters; ++i) still = still && mark->letter(i) == Letter::Ready && mark->shownDrop(i) == 0;
        received = current;
        gate.setOperationError("합성 오류");
    });
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(received, QString("ab")); QVERIFY(still);
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::Rejected);
    QCOMPARE(unlock->findChild<QLabel*>("vaultError")->text(), QString("합성 오류"));
    QVERIFY(field->text().isEmpty()); QVERIFY(field->property("error").toBool()); QTRY_VERIFY(mark->settled());
    QTest::keyClicks(field, "c");
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::Ready);
    QVERIFY(!field->property("error").toBool()); // typing again clears the outline
    gate.setOperationError("합성 오류"); QVERIFY(field->property("error").toBool());
    gate.resetPresentation(); QVERIFY(!field->property("error").toBool()); // so does leaving the page
    for (int i = 0; i < VaultMark::Letters; ++i) QCOMPARE(mark->letter(i), Letter::Off);
}

void VaultMarkTest::gateFollowsTheVaultFile() {
    VaultGate gate(false, {}); gate.setSecurityReady(true); gate.resize(1280, 820); gate.show();
    QVERIFY(QTest::qWaitForWindowExposed(&gate));
    auto* create = gate.findChild<VaultDialog*>("vaultGateCreateForm"); auto* unlock = gate.findChild<VaultDialog*>("vaultGateUnlockForm");
    auto* restore = gate.findChild<VaultDialog*>("vaultGateRestoreForm"); auto* title = gate.findChild<QLabel*>("vaultGateTitle");
    auto* backup = gate.findChild<QPushButton*>("vaultGateBackup"); auto* cancel = gate.findChild<QPushButton*>("vaultGateRestoreCancel");
    QVERIFY(create->isVisibleTo(&gate)); QVERIFY(backup->isVisibleTo(&gate)); QVERIFY(!cancel->isVisibleTo(&gate));
    gate.refreshMetadata(true, {});
    QVERIFY(unlock->isVisibleTo(&gate)); QCOMPARE(title->text(), QString("금고 열기")); QVERIFY(!backup->isVisibleTo(&gate));
    gate.refreshMetadata(false, {});
    QVERIFY(create->isVisibleTo(&gate)); QCOMPARE(title->text(), QString("새 금고 만들기"));
    gate.selectBackup("/synthetic/backup.bin");
    QVERIFY(restore->isVisibleTo(&gate)); QCOMPARE(title->text(), QString("백업에서 복원")); QVERIFY(cancel->isVisibleTo(&gate)); QVERIFY(!backup->isVisibleTo(&gate));
    QVERIFY(!gate.findChild<VaultMark*>("vaultGateBrand")->captionsVisible());
    for (auto* text : gate.findChildren<QLabel*>()) QVERIFY(!text->text().contains("/synthetic/"));
    // A restore keeps its page while a vault appears, so its outcome stays readable.
    gate.refreshMetadata(true, {}); QVERIFY(restore->isVisibleTo(&gate));
    cancel->click(); QVERIFY(unlock->isVisibleTo(&gate));
    // The stack is sized to the visible form, so the shorter unlock form is not
    // padded to the create form's height.
    QTRY_VERIFY(gate.findChild<QWidget*>("vaultGateForms")->height() < create->sizeHint().height());
}

void VaultMarkTest::gateRendersForReview_data() {
    QTest::addColumn<bool>("dark"); QTest::addColumn<bool>("existing"); QTest::addColumn<QSize>("size");
    for (bool dark : {false, true}) for (bool existing : {false, true}) for (const auto& size : {QSize(1280, 820), QSize(720, 640)})
        QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark ? "dark" : "light", existing ? "unlock" : "create").arg(size.width()))) << dark << existing << size;
}
void VaultMarkTest::gateRendersForReview() {
    QFETCH(bool, dark); QFETCH(bool, existing); QFETCH(QSize, size);
    KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
    VaultGate gate(existing, QDateTime::currentDateTime()); gate.setSecurityReady(true); gate.resize(size); gate.show();
    QVERIFY(QTest::qWaitForWindowExposed(&gate));
    const QString tag = QString("%1-%2-%3").arg(dark ? "dark" : "light", existing ? "unlock" : "create").arg(size.width());
    auto* mark = gate.findChild<VaultMark*>("vaultGateBrand");
    auto* browse = gate.findChild<QPushButton*>("vaultGateBrowse");
    QVERIFY(gate.rect().contains(QRect(browse->mapTo(&gate, QPoint()), browse->size())));
    QVERIFY(save(gate, "gate-" + tag + "-empty.png"));
    if (existing) {
        auto* field = edit(gate.findChild<VaultDialog*>("vaultGateUnlockForm"), "currentPassword");
        field->setFocus(); QTest::keyClicks(field, "wrong");
        connect(&gate, &VaultGate::submitted, this, [&] { gate.setOperationError("금고 암호가 올바르지 않습니다."); });
        QTest::keyClick(field, Qt::Key_Return); QTRY_VERIFY(mark->settled());
        QVERIFY(save(gate, "gate-" + tag + "-rejected.png"));
    } else {
        auto* create = gate.findChild<VaultDialog*>("vaultGateCreateForm");
        auto* field = edit(create, "newPassword"); field->setFocus(); QTest::keyClicks(field, "keefi2026");
        QTRY_VERIFY(mark->settled()); QVERIFY(save(gate, "gate-" + tag + "-typing.png"));
        QTest::keyClicks(field, "!Kr"); auto* confirmation = edit(create, "confirmPassword"); confirmation->setFocus();
        QTest::keyClicks(confirmation, "keefi2026!Kr"); QTRY_VERIFY(mark->settled());
        QVERIFY(submit(create)->isEnabled()); QVERIFY(save(gate, "gate-" + tag + "-ready.png"));
    }
}

QTEST_MAIN(VaultMarkTest)
#include "tst_vault_mark.moc"
