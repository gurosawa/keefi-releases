#include "keefi/VaultDialog.h"
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using namespace keefi;

namespace {
void passwordFields() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("field");
    QTest::newRow("create-new") << int(VaultDialog::Mode::Create) << QString("newPassword");
    QTest::newRow("create-confirm") << int(VaultDialog::Mode::Create) << QString("confirmPassword");
    QTest::newRow("unlock-current") << int(VaultDialog::Mode::Unlock) << QString("currentPassword");
    QTest::newRow("change-current") << int(VaultDialog::Mode::ChangePassword) << QString("currentPassword");
    QTest::newRow("change-new") << int(VaultDialog::Mode::ChangePassword) << QString("newPassword");
    QTest::newRow("change-confirm") << int(VaultDialog::Mode::ChangePassword) << QString("confirmPassword");
}
void commitTo(QLineEdit* edit, const QString& value, int start = 0, int length = 0) {
    QInputMethodEvent event;
    event.setCommitString(value, start, length);
    QCoreApplication::sendEvent(edit, &event);
}
void insertThrough(QLineEdit* edit, const QString& value, bool paste) {
    if (!paste) { edit->insert(value); return; }
    // The target runs on Qt's offscreen platform. Only synthetic text is put
    // into that process-local clipboard; no previous clipboard data is read.
    QGuiApplication::clipboard()->setText(value);
    edit->paste();
    QGuiApplication::clipboard()->clear();
}
}

class VaultKeyboardTest final : public QObject {
    Q_OBJECT
private slots:
    void legacyPasswordChoiceIsRemoved_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("create") << int(VaultDialog::Mode::Create);
        QTest::newRow("unlock") << int(VaultDialog::Mode::Unlock);
        QTest::newRow("change") << int(VaultDialog::Mode::ChangePassword);
    }
    void legacyPasswordChoiceIsRemoved() {
        QFETCH(int, mode);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        QVERIFY(!dialog.findChild<QCheckBox*>("legacyNonAsciiPassword"));
    }

    void committedHangulBecomesEnglishKeyboardKeys_data() { passwordFields(); }
    void committedHangulBecomesEnglishKeyboardKeys() {
        QFETCH(int, mode); QFETCH(QString, field);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        auto* edit = dialog.findChild<QLineEdit*>(field);
        QVERIFY(edit);
        // Synthetic input only. No Vault, account, clipboard or personal
        // profile is opened: exercise the public QLineEdit IME boundary.
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("안녕"));
        QCoreApplication::sendEvent(edit, &commit);
        QCOMPARE(edit->text(), QString("dkssud"));
        QCOMPARE(edit->echoMode(), QLineEdit::Password);
    }

    void pasteAndInsertMapHangulWithoutChangingAscii_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected"); QTest::addColumn<bool>("paste");
        const QList<QPair<QString, QString>> cases{
            {QStringLiteral("안녕"), "dkssud"}, {QStringLiteral("값"), "rkqt"},
            {QStringLiteral("왜"), "dho"}, {QStringLiteral("똠"), "Eha"},
            {QStringLiteral("ㄱㅏㄴㅏ"), "rksk"}, {QStringLiteral("ㄲㅘㄳ"), "Rhkrt"},
            {QStringLiteral("\u1100\u1161\u11a8"), "rkr"},
            {QStringLiteral(" FAKE안녕#Z9 "), " FAKEdkssud#Z9 "},
            {"  Fake-ASCII_2026!~`[]{}\\|;:'\",.<>/?  ", "  Fake-ASCII_2026!~`[]{}\\|;:'\",.<>/?  "}};
        for (int i = 0; i < cases.size(); ++i) for (bool paste : {false, true})
            QTest::newRow(qPrintable(QString("case-%1-%2").arg(i).arg(paste ? "paste" : "insert")))
                << cases[i].first << cases[i].second << paste;
    }
    void pasteAndInsertMapHangulWithoutChangingAscii() {
        QFETCH(QString, input); QFETCH(QString, expected); QFETCH(bool, paste);
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword"); QVERIFY(edit);
        insertThrough(edit, input, paste);
        QCOMPARE(edit->text().toUtf8(), expected.toUtf8());
        QCOMPARE(edit->cursorPosition(), expected.size());
    }

    void middleEditsKeepTheAsciiCursorAndSelectionSemantics_data() {
        QTest::addColumn<bool>("selection"); QTest::addColumn<bool>("paste");
        for (bool selection : {false, true}) for (bool paste : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(selection ? "selection" : "cursor", paste ? "paste" : "insert")))
                << selection << paste;
    }
    void middleEditsKeepTheAsciiCursorAndSelectionSemantics() {
        QFETCH(bool, selection); QFETCH(bool, paste);
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword"); QVERIFY(edit);
        QLineEdit reference;
        for (auto* field : {edit, &reference}) {
            field->setText("FAKE-old-tail");
            if (selection) field->setSelection(5, 3); else field->setCursorPosition(5);
        }
        insertThrough(&reference, "dkssud", paste);
        insertThrough(edit, QStringLiteral("안녕"), paste);
        QCOMPARE(edit->text(), reference.text());
        QCOMPARE(edit->cursorPosition(), reference.cursorPosition());
        QTest::keyClicks(edit, "X"); QTest::keyClicks(&reference, "X");
        QCOMPARE(edit->text(), reference.text());
    }

    void preeditIsTransientAndFinalCommitAppearsOnce_data() { passwordFields(); }
    void preeditIsTransientAndFinalCommitAppearsOnce() {
        QFETCH(int, mode); QFETCH(QString, field);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        auto* edit = dialog.findChild<QLineEdit*>(field); QVERIFY(edit);
        edit->insert("FAKE-");
        QInputMethodEvent preedit(QStringLiteral("안녕"), {});
        QCoreApplication::sendEvent(edit, &preedit);
        QCOMPARE(edit->text(), QString("FAKE-"));
        QVERIFY(!edit->inputMethodQuery(Qt::ImEnabled).toBool());
        commitTo(edit, QStringLiteral("안녕"));
        QCOMPARE(edit->text(), QString("FAKE-dkssud"));
        QCOMPARE(edit->displayText().size(), edit->text().size());
    }

    void imeReplacementAndDeletionMatchOrdinaryAsciiEditing_data() {
        QTest::addColumn<int>("start"); QTest::addColumn<int>("length");
        QTest::addColumn<bool>("selection"); QTest::addColumn<bool>("deletion");
        QTest::newRow("insert-at-cursor") << 0 << 0 << false << false;
        QTest::newRow("replace-before-cursor") << -3 << 3 << false << false;
        QTest::newRow("replace-after-cursor") << 0 << 2 << false << false;
        QTest::newRow("delete-before-cursor") << -3 << 3 << false << true;
        QTest::newRow("replace-selection") << 0 << 0 << true << false;
        QTest::newRow("selection-and-replacement") << -2 << 2 << true << false;
        QTest::newRow("delete-selection") << 0 << 0 << true << true;
    }
    void imeReplacementAndDeletionMatchOrdinaryAsciiEditing() {
        QFETCH(int, start); QFETCH(int, length); QFETCH(bool, selection); QFETCH(bool, deletion);
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword"); QVERIFY(edit);
        QLineEdit reference;
        for (auto* field : {edit, &reference}) {
            field->setText("FAKE-old-tail");
            if (selection) field->setSelection(5, 3); else field->setCursorPosition(8);
        }
        commitTo(&reference, deletion ? QString{} : QString("dkssud"), start, length);
        commitTo(edit, deletion ? QString{} : QStringLiteral("안녕"), start, length);
        QCOMPARE(edit->text(), reference.text());
        QCOMPARE(edit->cursorPosition(), reference.cursorPosition());
    }

    void physicalKeysKeepShiftCapsAndExistingAscii_data() {
        QTest::addColumn<int>("key"); QTest::addColumn<int>("modifiers");
        QTest::addColumn<quint32>("nativeKey"); QTest::addColumn<quint32>("nativeModifiers");
        QTest::addColumn<QString>("text"); QTest::addColumn<QString>("expected");
        const auto row = [](const char* name, int key, Qt::KeyboardModifiers modifiers, quint32 nativeKey,
                            quint32 nativeModifiers, const QString& text, const QString& expected) {
            QTest::newRow(name) << key << int(modifiers) << nativeKey << nativeModifiers << text << expected;
        };
        row("plain-a", Qt::Key_A, Qt::NoModifier, 0x00, 0, QStringLiteral("ㅁ"), "a");
        row("shift-a", Qt::Key_A, Qt::ShiftModifier, 0x00, 0, QStringLiteral("ㅁ"), "A");
        row("shift-q", Qt::Key_Q, Qt::ShiftModifier, 0x0c, 0, QStringLiteral("ㅂ"), "Q");
        row("already-ascii", Qt::Key_A, Qt::ShiftModifier, 0x00, 0, "z", "z");
#ifdef Q_OS_MACOS
        row("caps-a", Qt::Key_A, Qt::NoModifier, 0x00, 1u << 16, QStringLiteral("ㅁ"), "A");
        row("caps-shift-a", Qt::Key_A, Qt::ShiftModifier, 0x00, 1u << 16, QStringLiteral("ㅁ"), "a");
        row("shift-number", Qt::Key_1, Qt::ShiftModifier, 0x12, 0, QStringLiteral("！"), "!");
        row("native-key-without-qt-letter", Qt::Key_unknown, Qt::NoModifier, 0x2e, 0, QStringLiteral("ㅡ"), "m");
#endif
    }
    void physicalKeysKeepShiftCapsAndExistingAscii() {
        QFETCH(int, key); QFETCH(int, modifiers); QFETCH(quint32, nativeKey); QFETCH(quint32, nativeModifiers);
        QFETCH(QString, text); QFETCH(QString, expected);
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword"); QVERIFY(edit);
        QKeyEvent press(QEvent::KeyPress, key, Qt::KeyboardModifiers(modifiers), 0, nativeKey, nativeModifiers, text);
        QCoreApplication::sendEvent(edit, &press);
        QCOMPARE(edit->text(), expected);
    }

    void unsupportedMixedEditsPreserveTheWholePriorPassword_data() {
        QTest::addColumn<QString>("input");
        QTest::newRow("emoji") << QStringLiteral("안녕😀");
        QTest::newRow("accent") << QStringLiteral("안녕é");
        QTest::newRow("fullwidth-paste") << QStringLiteral("안녕！");
    }
    void unsupportedMixedEditsPreserveTheWholePriorPassword() {
        QFETCH(QString, input);
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword"); QVERIFY(edit);
        edit->insert("FAKE-Prior-2026!"); edit->setSelection(5, 5);
        insertThrough(edit, input, true);
        QCOMPARE(edit->text(), QString("FAKE-Prior-2026!"));
    }

    void expandedPasswordsCannotBecomeAValidTruncatedPrefix_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("insert") << QString("insert");
        QTest::newRow("paste") << QString("paste");
        QTest::newRow("ime") << QString("ime");
    }
    void expandedPasswordsCannotBecomeAValidTruncatedPrefix() {
        QFETCH(QString, path);
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        auto* submit = dialog.findChild<QPushButton*>("submitVault"); QVERIFY(edit && submit);
        const QString source(342, QChar(0xc548)); // 안 -> dks: 1026 ASCII bytes.
        if (path == "ime") commitTo(edit, source); else insertThrough(edit, source, path == "paste");
        QVERIFY(edit->text().toUtf8().size() > 1024);
        QVERIFY(!submit->isEnabled());
        QSignalSpy submitted(&dialog, &VaultDialog::submitted); submit->click(); QCOMPARE(submitted.size(), 0);
    }

    void submittedPasswordsStayExactClearAndSubmitOnlyOnce_data() {
        QTest::addColumn<int>("mode"); QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        for (const auto mode : {VaultDialog::Mode::Create, VaultDialog::Mode::Unlock, VaultDialog::Mode::ChangePassword}) {
            const auto name = QString::number(int(mode));
            QTest::newRow(qPrintable(name + "-ascii")) << int(mode) << QString("  Fake-ASCII-2026!  ") << QString("  Fake-ASCII-2026!  ");
            QTest::newRow(qPrintable(name + "-hangul")) << int(mode) << QStringLiteral("Fake-안녕-2026!") << QString("Fake-dkssud-2026!");
        }
    }
    void submittedPasswordsStayExactClearAndSubmitOnlyOnce() {
        QFETCH(int, mode); QFETCH(QString, input); QFETCH(QString, expected);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        for (const auto* name : {"currentPassword", "newPassword", "confirmPassword"})
            if (auto* edit = dialog.findChild<QLineEdit*>(name)) commitTo(edit, input);
        auto* submit = dialog.findChild<QPushButton*>("submitVault"); QVERIFY(submit && submit->isEnabled());
        QSignalSpy submitted(&dialog, &VaultDialog::submitted);
        submit->click(); submit->click(); QTest::keyClick(&dialog, Qt::Key_Return);
        QCOMPARE(submitted.size(), 1);
        QCOMPARE(submitted.first()[0].toString().toUtf8(), mode == int(VaultDialog::Mode::Create) ? QByteArray{} : expected.toUtf8());
        QCOMPARE(submitted.first()[1].toString().toUtf8(), mode == int(VaultDialog::Mode::Unlock) ? QByteArray{} : expected.toUtf8());
        for (auto* edit : dialog.findChildren<QLineEdit*>()) {
            QVERIFY(edit->text().isEmpty()); QCOMPARE(edit->echoMode(), QLineEdit::Password);
        }
        dialog.setError("FAKE operation failure");
        QVERIFY(!submit->isEnabled());
        dialog.reject(); QCOMPARE(submitted.size(), 1);
        for (auto* edit : dialog.findChildren<QLineEdit*>()) QVERIFY(edit->text().isEmpty());
    }
};

QTEST_MAIN(VaultKeyboardTest)
#include "tst_vault_keyboard.moc"
