#include <QtTest>
#include "keefi/ProfileUpgradeDialog.h"
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace keefi;
namespace {
const QString Password = QStringLiteral("Synthetic-Only-2026!");
QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool write(const QString& path, const QByteArray& value) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())
        || !QFile::setPermissions(QFileInfo(path).absolutePath(),
             QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(value) != value.size() || !file.flush()) return false;
    file.close();
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
struct Fixture {
    QTemporaryDir temp;
    QString base = temp.filePath("data");
    QString source = QDir(base).filePath("KEEFI/KEEFI Dev");
    QString target = QDir(base).filePath("KEEFI/Profiles/Dev");
    QString vault = QDir(source).filePath("credentials.finix-vault");
    bool make() { return temp.isValid() && write(vault, "SYNTHETIC_OLD_CIPHERTEXT"); }
    ProfileUpgradePlan plan() const { return planProfileUpgrade(std::nullopt, base); }
};
ProfileStageConverter converter(int* calls) {
    return [calls](const QString& stage, const QString& password,
                   ProfileConversionReceipt* receipt, QString* error) {
        ++*calls;
        *receipt = {};
        if (password != Password) {
            if (error) *error = QStringLiteral("합성 암호 불일치");
            return false;
        }
        const auto old = QDir(stage).filePath("credentials.finix-vault");
        if (bytes(old) != "SYNTHETIC_OLD_CIPHERTEXT"
            || !write(QDir(stage).filePath("credentials.keefi-vault"), "SYNTHETIC_NEW_CIPHERTEXT")
            || !QFile::remove(old)) return false;
        receipt->verified = true;
        receipt->identityScheme = QStringLiteral("legacy-v1");
        return true;
    };
}
}

class ProfileUpgradeDialogTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
private slots:
    void initTestCase() {
        QVERIFY(settingsRoot_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("SyntheticProfileUpgrade"));
        QCoreApplication::setApplicationName(QStringLiteral("SyntheticUpgradeDialog"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot_.path());
    }
    void singleSourceRequiresActualConfirmationAndShowsBothPaths() {
        Fixture f; QVERIFY(f.make());
        const auto plan = f.plan();
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        ProfileUpgradeDialog dialog(plan);
        dialog.show();
        auto* from = dialog.findChild<QLabel*>("profileUpgradeSourcePath");
        auto* target = dialog.findChild<QLabel*>("profileUpgradeTargetPath");
        auto* proceed = dialog.findChild<QPushButton*>("profileUpgradeContinue");
        QVERIFY(from); QVERIFY(target); QVERIFY(proceed);
        QCOMPARE(from->text(), f.source);
        QCOMPARE(target->text(), f.target);
        QCOMPARE(from->textFormat(), Qt::PlainText);
        QCOMPARE(dialog.sourceKind(), ProfileUpgradeSourceKind::None);
        QVERIFY(!QFileInfo::exists(f.target));
        QTest::mouseClick(proceed, Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.sourceKind(), ProfileUpgradeSourceKind::Dev100Keefi);
        QVERIFY(!QFileInfo::exists(f.target));
        QCOMPARE(bytes(f.vault), QByteArray("SYNTHETIC_OLD_CIPHERTEXT"));
    }
    void ambiguousSourcesHaveNoImplicitChoice() {
        Fixture f; QVERIFY(f.make());
        const auto older = QDir(f.base).filePath("FINIX/FINIX Dev");
        QVERIFY(write(QDir(older).filePath("credentials.finix-vault"), "SYNTHETIC_OTHER_CIPHERTEXT"));
        const auto plan = f.plan();
        QCOMPARE(plan.action, ProfileUpgradeAction::ChooseSource);
        ProfileUpgradeDialog dialog(plan); dialog.show();
        auto* proceed = dialog.findChild<QPushButton*>("profileUpgradeContinue");
        auto* original = dialog.findChild<QRadioButton*>("profileUpgradeSource_legacy");
        auto* renamed = dialog.findChild<QRadioButton*>("profileUpgradeSource_renamed");
        QVERIFY(proceed); QVERIFY(original); QVERIFY(renamed);
        QVERIFY(!proceed->isEnabled());
        QVERIFY(!original->isChecked()); QVERIFY(!renamed->isChecked());
        QTest::mouseClick(original, Qt::LeftButton);
        QVERIFY(proceed->isEnabled());
        QTest::mouseClick(proceed, Qt::LeftButton);
        QCOMPARE(dialog.sourceKind(), ProfileUpgradeSourceKind::LegacyOriginal);
        QCOMPARE(bytes(f.vault), QByteArray("SYNTHETIC_OLD_CIPHERTEXT"));
    }
    void sourceEscapeDoesNotAuthorizeConversion() {
        Fixture f; QVERIFY(f.make());
        ProfileUpgradeDialog dialog(f.plan()); dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(dialog.sourceKind(), ProfileUpgradeSourceKind::None);
        QVERIFY(!QFileInfo::exists(f.target));
        QVERIFY(!QFileInfo::exists(QDir(f.source).filePath("instance.lock")));
    }
    void privateAccessMustBeEnabledBeforePasswordEntry() {
        Fixture f; QVERIFY(f.make()); int calls = 0;
        ProfileUpgradePasswordDialog dialog(f.plan(), "synthetic-dev101", converter(&calls));
        dialog.show();
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        auto* submit = dialog.findChild<QPushButton*>("submitVault");
        QVERIFY(edit); QVERIFY(submit);
        QVERIFY(!edit->isEnabled()); QVERIFY(!submit->isEnabled());
        QCOMPARE(calls, 0);
        dialog.setPrivateAccess(true);
        QVERIFY(edit->isEnabled());
        QTest::keyClicks(edit, Password);
        QVERIFY(submit->isEnabled());
        dialog.setPrivateAccess(false);
        QVERIFY(edit->text().isEmpty()); QVERIFY(!edit->isUndoAvailable());
        QVERIFY(!edit->isEnabled()); QVERIFY(!submit->isEnabled());
        QCOMPARE(calls, 0);
    }
    void actualPasswordSubmitPublishesOnlyNewProfileAndClearsBeforeFinished() {
        Fixture f; QVERIFY(f.make()); int calls = 0;
        ProfileUpgradePasswordDialog dialog(f.plan(), "synthetic-dev101", converter(&calls));
        QVERIFY2(dialog.error().isEmpty(), qPrintable(dialog.error()));
        dialog.setPrivateAccess(true); dialog.show();
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        auto* submit = dialog.findChild<QPushButton*>("submitVault");
        QVERIFY(edit); QVERIFY(submit);
        bool clearedAtFinish = false;
        connect(&dialog, &QDialog::finished, &dialog, [&] {
            clearedAtFinish = edit->text().isEmpty() && !edit->isUndoAvailable()
                && edit->echoMode() == QLineEdit::Password;
        });
        QTest::keyClicks(edit, Password);
        QTest::mouseClick(submit, Qt::LeftButton);
        QCOMPARE(calls, 1);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(clearedAtFinish);
        QCOMPARE(dialog.ready().directory, f.target);
        QCOMPARE(dialog.ready().identityScheme, QStringLiteral("legacy-v1"));
        QCOMPARE(bytes(f.vault), QByteArray("SYNTHETIC_OLD_CIPHERTEXT"));
        QCOMPARE(bytes(QDir(f.target).filePath("credentials.keefi-vault")), QByteArray("SYNTHETIC_NEW_CIPHERTEXT"));
        QVERIFY(!QFileInfo::exists(QDir(f.source).filePath("instance.lock")));
        QVERIFY(!bytes(QDir(f.target).filePath(".keefi-profile.json")).contains(Password.toUtf8()));
    }
    void wrongPasswordClearsAndFreshStageCanRetry() {
        Fixture f; QVERIFY(f.make()); int calls = 0;
        ProfileUpgradePasswordDialog dialog(f.plan(), "synthetic-dev101", converter(&calls));
        dialog.setPrivateAccess(true); dialog.show();
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        auto* submit = dialog.findChild<QPushButton*>("submitVault");
        auto* error = dialog.findChild<QLabel*>("vaultError");
        QVERIFY(edit); QVERIFY(submit); QVERIFY(error);
        QTest::keyClicks(edit, "Wrong-Synthetic-2026!");
        QTest::mouseClick(submit, Qt::LeftButton);
        QCOMPARE(calls, 1);
        QVERIFY(error->isVisible()); QVERIFY(error->text().contains("합성 암호 불일치"));
        QVERIFY(edit->text().isEmpty()); QVERIFY(!edit->isUndoAvailable());
        QVERIFY(dialog.ready().directory.isEmpty());
        QVERIFY(!QFileInfo::exists(f.target));
        QVERIFY(!QFileInfo::exists(QDir(f.source).filePath("instance.lock")));
        QTest::keyClicks(edit, Password);
        QTest::mouseClick(submit, Qt::LeftButton);
        QCOMPARE(calls, 2);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.ready().directory, f.target);
        QCOMPARE(bytes(f.vault), QByteArray("SYNTHETIC_OLD_CIPHERTEXT"));
    }
    void escapeDiscardsOnlyOwnedStageAndErasesPassword() {
        Fixture f; QVERIFY(f.make()); int calls = 0;
        const auto plan = f.plan();
        ProfileUpgradePasswordDialog dialog(plan, "synthetic-dev101", converter(&calls));
        dialog.setPrivateAccess(true); dialog.show();
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        QVERIFY(edit);
        QTest::keyClicks(edit, Password);
        QVERIFY(QFileInfo::exists(plan.transactionDirectory));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(edit->text().isEmpty()); QVERIFY(!edit->isUndoAvailable());
        QVERIFY(dialog.ready().directory.isEmpty());
        QCOMPARE(calls, 0);
        QVERIFY(!QFileInfo::exists(plan.transactionDirectory));
        QVERIFY(!QFileInfo::exists(f.target));
        QCOMPARE(bytes(f.vault), QByteArray("SYNTHETIC_OLD_CIPHERTEXT"));
    }
    void nativeLockSignalClearsPasswordAndStageBeforeFinished() {
        Fixture f; QVERIFY(f.make()); int calls = 0;
        const auto plan = f.plan();
        ProfileUpgradePasswordDialog dialog(plan, "synthetic-dev101", converter(&calls));
        dialog.setPrivateAccess(true); dialog.show();
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        QVERIFY(edit); QTest::keyClicks(edit, Password);
        bool cleared = false;
        connect(&dialog, &QDialog::finished, &dialog, [&] {
            cleared = edit->text().isEmpty() && !QFileInfo::exists(plan.transactionDirectory);
        });
        dialog.lockAndClear(QStringLiteral("synthetic-screen-lock"));
        QVERIFY(cleared);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(calls, 0);
        QVERIFY(dialog.ready().directory.isEmpty());
        QCOMPARE(bytes(f.vault), QByteArray("SYNTHETIC_OLD_CIPHERTEXT"));
    }
    void sourceLockFailureCannotOpenPasswordInputOrCallConverter() {
        Fixture f; QVERIFY(f.make()); int calls = 0;
        const auto lock = QDir(f.source).filePath("instance.lock");
        QVERIFY(write(lock, "SYNTHETIC_FOREIGN_LOCK"));
        ProfileUpgradePasswordDialog dialog(f.plan(), "synthetic-dev101", converter(&calls));
        dialog.setPrivateAccess(true); dialog.show();
        QVERIFY(!dialog.error().isEmpty());
        auto* edit = dialog.findChild<QLineEdit*>("currentPassword");
        QVERIFY(edit); QVERIFY(!edit->isEnabled());
        QCOMPARE(calls, 0);
        QCOMPARE(bytes(lock), QByteArray("SYNTHETIC_FOREIGN_LOCK"));
        QVERIFY(dialog.ready().directory.isEmpty());
    }
};
QTEST_MAIN(ProfileUpgradeDialogTest)
#include "tst_profile_upgrade_dialog.moc"
