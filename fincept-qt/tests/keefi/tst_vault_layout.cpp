#include "keefi/VaultDialog.h"
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>

using namespace keefi;

namespace {
void modes() {
    QTest::addColumn<int>("mode");
    QTest::newRow("create") << int(VaultDialog::Mode::Create);
    QTest::newRow("unlock") << int(VaultDialog::Mode::Unlock);
    QTest::newRow("change") << int(VaultDialog::Mode::ChangePassword);
}
QString modeName(int mode) {
    return mode == int(VaultDialog::Mode::Create) ? QString("create") :
        mode == int(VaultDialog::Mode::Unlock) ? QString("unlock") : QString("change");
}
QLineEdit* firstPassword(VaultDialog& dialog) {
    auto* current = dialog.findChild<QLineEdit*>("currentPassword");
    return current ? current : dialog.findChild<QLineEdit*>("newPassword");
}
bool preview(VaultDialog& dialog, const QString& name) {
    // Explicit opt-in, synthetic masked inputs only. No Vault or user profile.
    const auto directory = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    return directory.isEmpty() || (QDir().mkpath(directory) &&
        dialog.grab().save(QDir(directory).filePath("vault-layout-" + name + ".png")));
}
bool conversionCopy(const QString& text) {
    return text.contains("한글") || text.contains("두벌식") || text.contains("dkssud") ||
        text.contains("영문 자판 글자");
}
}

class VaultLayoutTest final : public QObject {
    Q_OBJECT
private slots:
    void passwordHelpContainsOnlyTheNeededRequirements_data() { modes(); }
    void passwordHelpContainsOnlyTheNeededRequirements() {
        QFETCH(int, mode);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(!dialog.findChild<QLabel*>("currentPasswordHint"));
        for (const auto* label : dialog.findChildren<QLabel*>())
            if (label->isVisibleTo(&dialog)) QVERIFY2(!conversionCopy(label->text()), qPrintable(label->text()));
        if (mode != int(VaultDialog::Mode::Unlock)) {
            const auto* requirements = dialog.findChild<QLabel*>("passwordRequirements");
            QVERIFY(requirements);
            QVERIFY(requirements->text().contains("12자"));
            for (const auto* word : {"대문자", "소문자", "숫자", "특수문자"}) QVERIFY(requirements->text().contains(word));
        }
        QVERIFY(preview(dialog, modeName(mode) + "-initial"));
    }

    void convertedInputStaysQuietAndKeepsTheLayout_data() { modes(); }
    void convertedInputStaysQuietAndKeepsTheLayout() {
        QFETCH(int, mode);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        for (auto* edit : dialog.findChildren<QLineEdit*>()) edit->setText("Fake-dkssud-2026!");
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* edit = firstPassword(dialog); auto* notice = dialog.findChild<QLabel*>("passwordInputNotice");
        QVERIFY(edit && notice); QVERIFY(notice->isHidden());
        const auto before = dialog.size(); const auto fieldBefore = edit->geometry();
        edit->selectAll();
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("Fake-안녕-2026!"));
        QCoreApplication::sendEvent(edit, &commit);
        QCOMPARE(edit->text(), QString("Fake-dkssud-2026!"));
        QVERIFY(notice->isHidden());
        QCOMPARE(dialog.size(), before); QCOMPARE(edit->geometry(), fieldBefore);
        QVERIFY(preview(dialog, modeName(mode) + "-converted"));
        // Korean-layout physical keystrokes also stay quiet; conversion remains active.
        QKeyEvent shifted(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, 0, 0x00, 0, QStringLiteral("ㅁ"));
        QCoreApplication::sendEvent(edit, &shifted);
        QVERIFY(edit->text().endsWith('A')); QVERIFY(notice->isHidden());
    }

    void unlockUsesContentHeightWithoutReservedNoticeLines() {
        VaultDialog dialog(VaultDialog::Mode::Unlock);
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        auto* submit = dialog.findChild<QPushButton*>("submitVault");
        auto* notice = dialog.findChild<QLabel*>("passwordInputNotice");
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(edit && submit && notice && buttons);
        QVERIFY(!notice->sizePolicy().retainSizeWhenHidden());
        const auto line = QFontMetrics(dialog.font()).lineSpacing();
        const int gap = submit->mapTo(&dialog, QPoint(0, 0)).y() - edit->mapTo(&dialog, QPoint(0, edit->height())).y();
        QVERIFY2(gap <= line * 3, qPrintable(QString("password/button gap %1 exceeds %2").arg(gap).arg(line * 3)));
        QLabel* explanation = nullptr;
        for (auto* label : dialog.findChildren<QLabel*>(QString{}, Qt::FindDirectChildrenOnly))
            if (label->text().contains("이 PC의")) { explanation = label; break; }
        QVERIFY(explanation);
        const auto margins = dialog.layout()->contentsMargins();
        const int available = dialog.width() - margins.left() - margins.right();
        const int explanationHeight = qMax(explanation->sizeHint().height(), explanation->heightForWidth(available));
        // Budget the actual explanation, one password row and the actions, with
        // room for spacing and two normal text lines; no fixed three-line void.
        const int compactHeight = margins.top() + margins.bottom() + explanationHeight +
            edit->parentWidget()->sizeHint().height() + buttons->sizeHint().height() +
            3 * qMax(0, dialog.layout()->spacing()) + 2 * line;
        qInfo() << "unlock layout" << dialog.size() << "hint" << dialog.sizeHint() << "content limit" << compactHeight << "gap" << gap;
        QVERIFY(dialog.sizeHint().height() <= compactHeight);
        QVERIFY(dialog.height() <= compactHeight);
        QVERIFY(preview(dialog, "unlock-compact"));
    }

    void unsupportedInputKeepsAVisibleRealError_data() { modes(); }
    void unsupportedInputKeepsAVisibleRealError() {
        QFETCH(int, mode);
        VaultDialog dialog{VaultDialog::Mode(mode)};
        for (auto* edit : dialog.findChildren<QLineEdit*>()) edit->setText("Fake-Layout-2026!");
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* edit = firstPassword(dialog); auto* notice = dialog.findChild<QLabel*>("passwordInputNotice");
        QVERIFY(edit && notice);
        edit->insert(QStringLiteral("😀"));
        QCOMPARE(edit->text(), QString("Fake-Layout-2026!"));
        QVERIFY(notice->isVisibleTo(&dialog)); QVERIFY(!notice->text().isEmpty());
        QVERIFY(notice->text().contains("지원하지 않는 문자"));
        QVERIFY(!conversionCopy(notice->text()));
        QVERIFY(preview(dialog, modeName(mode) + "-unsupported"));
        edit->insert("X"); QTRY_VERIFY(notice->isHidden());
    }
};
QTEST_MAIN(VaultLayoutTest)
#include "tst_vault_layout.moc"
