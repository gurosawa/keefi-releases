#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;

namespace {
struct Submission {
    int count = 0;
    bool success = false;
    QString current, next, error;
};

// The public UI signal authenticates a real temporary encrypted vault. No input
// conversion helper or private dialog/vault state is used by this fixture.
Submission submit(VaultDialog& dialog, Vault& vault, VaultDialog::Mode mode) {
    Submission result;
    const auto connection = QObject::connect(&dialog, &VaultDialog::submitted, &dialog,
        [&](const QString& current, const QString& next) {
            ++result.count;
            result.current = current;
            result.next = next;
            if (mode == VaultDialog::Mode::Create) result.success = vault.create(next, &result.error);
            else if (mode == VaultDialog::Mode::Unlock) result.success = vault.unlock(current, &result.error);
            else result.success = vault.changePassword(current, next, &result.error);
            if (result.success) dialog.accept();
            else dialog.setError(result.error);
        });
    dialog.findChild<QPushButton*>("submitVault")->click();
    QObject::disconnect(connection);
    return result;
}

void insert(VaultDialog& dialog, const char* name, const QString& text) {
    auto* field = dialog.findChild<QLineEdit*>(QString::fromLatin1(name));
    if (!field) {
        QTest::qFail("Expected password input is missing", __FILE__, __LINE__);
        return;
    }
    field->insert(text);
}
}

class VaultKeyboardRoundtripTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void convertedPasswordCreatesUnlocksChangesAndReopensTheEncryptedVault() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("synthetic-vault.bin");
        const QString originalUi = QStringLiteral(" Fake-안녕-2026! ");
        const QString originalEnglish = QStringLiteral(" Fake-dkssud-2026! ");
        const QString changedUi = QStringLiteral(" Next-값-Pass2026! ");
        const QString changedEnglish = QStringLiteral(" Next-rkqt-Pass2026! ");
        Vault vault(path);
        {
            VaultDialog dialog(VaultDialog::Mode::Create);
            QVERIFY(!dialog.findChild<QCheckBox*>("legacyNonAsciiPassword"));
            insert(dialog, "newPassword", originalUi);
            insert(dialog, "confirmPassword", originalUi);
            QVERIFY2(dialog.findChild<QPushButton*>("submitVault")->isEnabled(),
                "Two-set Hangul input must become the valid English password before creation.");
            const auto result = submit(dialog, vault, VaultDialog::Mode::Create);
            QCOMPARE(result.count, 1); QVERIFY2(result.success, qPrintable(result.error));
            QVERIFY(result.current.isEmpty()); QCOMPARE(result.next, originalEnglish);
            QVERIFY(vault.isUnlocked()); QVERIFY(vault.exists());
            QVERIFY(dialog.findChild<QLineEdit*>("newPassword")->text().isEmpty());
            QVERIFY(dialog.findChild<QLineEdit*>("confirmPassword")->text().isEmpty());
        }
        vault.lock();
        {
            VaultDialog dialog(VaultDialog::Mode::Unlock);
            QVERIFY(!dialog.findChild<QCheckBox*>("legacyNonAsciiPassword"));
            insert(dialog, "currentPassword", originalUi);
            const auto result = submit(dialog, vault, VaultDialog::Mode::Unlock);
            QCOMPARE(result.count, 1); QVERIFY2(result.success, qPrintable(result.error));
            QCOMPARE(result.current, originalEnglish); QVERIFY(result.next.isEmpty());
            QVERIFY(vault.isUnlocked());
            QVERIFY(dialog.findChild<QLineEdit*>("currentPassword")->text().isEmpty());
        }
        {
            VaultDialog dialog(VaultDialog::Mode::ChangePassword);
            QVERIFY(!dialog.findChild<QCheckBox*>("legacyNonAsciiPassword"));
            insert(dialog, "currentPassword", originalUi);
            insert(dialog, "newPassword", changedUi);
            insert(dialog, "confirmPassword", changedUi);
            const auto result = submit(dialog, vault, VaultDialog::Mode::ChangePassword);
            QCOMPARE(result.count, 1); QVERIFY2(result.success, qPrintable(result.error));
            QCOMPARE(result.current, originalEnglish); QCOMPARE(result.next, changedEnglish);
            for (const auto* name : {"currentPassword", "newPassword", "confirmPassword"})
                QVERIFY(dialog.findChild<QLineEdit*>(name)->text().isEmpty());
        }
        vault.lock();
        // A fresh owner reads the saved encrypted file, so an in-memory success
        // cannot hide a mismatch between the converted password and persistence.
        Vault reopened(path);
        QVERIFY(!reopened.unlock(originalEnglish));
        QVERIFY(!reopened.unlock(changedEnglish.trimmed()));
        QVERIFY(!reopened.unlock(changedEnglish.toLower()));
        {
            VaultDialog dialog(VaultDialog::Mode::Unlock);
            insert(dialog, "currentPassword", changedUi);
            const auto result = submit(dialog, reopened, VaultDialog::Mode::Unlock);
            QCOMPARE(result.count, 1); QVERIFY2(result.success, qPrintable(result.error));
            QCOMPARE(result.current, changedEnglish); QVERIFY(reopened.isUnlocked());
        }
        reopened.lock();
    }

    void anExistingAsciiPasswordKeepsCaseAndBothBoundarySpacesExactly() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("existing-ascii-vault.bin");
        const QString exact = QStringLiteral(" Existing-MiXeD-2026! ");
        Vault vault(path);
        QVERIFY(vault.create(exact));
        vault.lock();
        for (const auto& wrong : QStringList{exact.trimmed(), exact.toLower(), exact.mid(1), exact.chopped(1)}) {
            VaultDialog dialog(VaultDialog::Mode::Unlock);
            QVERIFY(!dialog.findChild<QCheckBox*>("legacyNonAsciiPassword"));
            insert(dialog, "currentPassword", wrong);
            const auto result = submit(dialog, vault, VaultDialog::Mode::Unlock);
            QCOMPARE(result.count, 1); QCOMPARE(result.current, wrong);
            QVERIFY(!result.success); QVERIFY(!vault.isUnlocked()); QVERIFY(!result.error.isEmpty());
            QVERIFY(dialog.findChild<QLineEdit*>("currentPassword")->text().isEmpty());
            QVERIFY(!dialog.findChild<QLabel*>("vaultError")->text().isEmpty());
        }
        {
            VaultDialog dialog(VaultDialog::Mode::Unlock);
            insert(dialog, "currentPassword", exact);
            const auto result = submit(dialog, vault, VaultDialog::Mode::Unlock);
            QCOMPARE(result.count, 1); QCOMPARE(result.current, exact);
            QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(vault.isUnlocked());
        }
        vault.lock();
        Vault reopened(path);
        QVERIFY(reopened.unlock(exact));
        reopened.lock();
    }
};

QTEST_MAIN(VaultKeyboardRoundtripTest)
#include "tst_vault_keyboard_roundtrip.moc"
