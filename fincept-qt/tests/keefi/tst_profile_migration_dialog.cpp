#include "keefi/KeefiFonts.h"
#include "keefi/ProfileMigration.h"
#include "keefi/ProfileMigrationDialog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace keefi;

namespace {
bool put(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush();
}
QByteArray get(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class ProfileMigrationDialogTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); }

    void buttonsReturnOnlyTheExplicitChoice_data() {
        QTest::addColumn<int>("action");
        QTest::addColumn<QString>("buttonName");
        QTest::addColumn<QString>("caption");
        QTest::addColumn<int>("choice");
        QTest::addColumn<bool>("keyboard");
        for (bool keys : {false, true}) {
            const auto add = [&](const char* name, DevProfileAction action, const char* object,
                                 const QString& caption, ProfileMigrationChoice choice) {
                QTest::newRow((QByteArray(name) + (keys ? "-keyboard" : "-mouse")).constData())
                    << int(action) << QString::fromLatin1(object) << caption << int(choice) << keys;
            };
            add("copy", DevProfileAction::OfferCopy, "profileMigrationCopy", "옮기기", ProfileMigrationChoice::Copy);
            add("later", DevProfileAction::OfferCopy, "profileMigrationLater", "나중에", ProfileMigrationChoice::Later);
            add("legacy", DevProfileAction::ChooseExisting, "profileMigrationUseLegacy", "옛 프로필 사용", ProfileMigrationChoice::UseLegacy);
            add("current", DevProfileAction::ChooseExisting, "profileMigrationUseCurrent", "새 프로필 사용", ProfileMigrationChoice::UseCurrent);
        }
    }
    void buttonsReturnOnlyTheExplicitChoice() {
        QFETCH(int, action);
        QFETCH(QString, buttonName);
        QFETCH(QString, caption);
        QFETCH(int, choice);
        QFETCH(bool, keyboard);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        ProfileMigrationPlan plan;
        plan.action = DevProfileAction(action);
        plan.paths.baseDirectory = base.path();
        plan.paths.legacyDirectory = base.filePath("FINIX/FINIX Dev <old>");
        plan.paths.currentDirectory = base.filePath("KEEFI/KEEFI Dev & new");
        ProfileMigrationDialog dialog(plan);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        auto* button = dialog.findChild<QPushButton*>(buttonName);
        QVERIFY2(button, qPrintable(buttonName + " button is missing"));
        QCOMPARE(dialog.objectName(), QString("profileMigrationDialog"));
        QCOMPARE(dialog.choice(), ProfileMigrationChoice::Cancel);
        auto* source = dialog.findChild<QLabel*>("profileMigrationSourcePath");
        auto* destination = dialog.findChild<QLabel*>("profileMigrationDestinationPath");
        QVERIFY(source && destination);
        QCOMPARE(source->text(), plan.paths.legacyDirectory);
        QCOMPARE(destination->text(), plan.paths.currentDirectory);
        QCOMPARE(source->textFormat(), Qt::PlainText);
        QCOMPARE(destination->textFormat(), Qt::PlainText);
        QString allText;
        for (const auto* label : dialog.findChildren<QLabel*>()) allText += label->text();
        QVERIFY(allText.contains("복사"));
        QVERIFY(allText.contains("지우지"));
        QVERIFY(button && button->isVisibleTo(&dialog) && button->isEnabled());
        QCOMPARE(button->text(), caption);
        if (auto* copy = dialog.findChild<QPushButton*>("profileMigrationCopy")) QVERIFY(!copy->isDefault());
        QSignalSpy finished(&dialog, &QDialog::finished);
        if (keyboard) {
            button->setFocus();
            QTRY_VERIFY(button->hasFocus());
            QTest::keyClick(button, Qt::Key_Space);
        } else QTest::mouseClick(button, Qt::LeftButton);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(dialog.choice(), ProfileMigrationChoice(choice));
        QVERIFY(!QFileInfo::exists(plan.paths.legacyDirectory));
        QVERIFY(!QFileInfo::exists(plan.paths.currentDirectory));
        QVERIFY(QDir(base.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    }

    void escapeCancelsWithoutSelectingOrCopying() {
        ProfileMigrationPlan plan;
        plan.action = DevProfileAction::OfferCopy;
        plan.paths.legacyDirectory = "/synthetic/FINIX/FINIX Dev";
        plan.paths.currentDirectory = "/synthetic/KEEFI/KEEFI Dev";
        ProfileMigrationDialog dialog(plan);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QTRY_COMPARE(rejected.size(), 1);
        QCOMPARE(dialog.choice(), ProfileMigrationChoice::Cancel);
    }

    void explicitButtonDrivesTheRealCopyOrFallback_data() {
        QTest::addColumn<bool>("copy");
        QTest::newRow("copy") << true;
        QTest::newRow("later") << false;
    }
    void explicitButtonDrivesTheRealCopyOrFallback() {
        QFETCH(bool, copy);
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto canonicalBase = QFileInfo(base.path()).canonicalFilePath();
        const DevProfilePaths paths{canonicalBase, QDir(canonicalBase).filePath("FINIX/FINIX Dev"),
            QDir(canonicalBase).filePath("KEEFI/KEEFI Dev"),
            QDir(canonicalBase).filePath("KEEFI/.keefi-dev-profile-copy")};
        const auto oldFile = QDir(paths.legacyDirectory).filePath("synthetic.dat");
        const QByteArray original("synthetic bytes\0FINIX", 21);
        QVERIFY(put(oldFile, original));
        QVERIFY(QFile::setPermissions(paths.legacyDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto plan = planDevProfile(std::nullopt, base.path());
        QCOMPARE(plan.action, DevProfileAction::OfferCopy);
        ProfileMigrationDialog dialog(plan);
        ProfileMigrationResult result;
        int applications = 0;
        connect(&dialog, &QDialog::accepted, &dialog, [&] {
            ++applications;
            result = applyDevProfileChoice(plan, dialog.choice(), "0.18.0-dev100-synthetic");
        });
        dialog.show();
        auto* button = dialog.findChild<QPushButton*>(copy ? "profileMigrationCopy" : "profileMigrationLater");
        QVERIFY(button);
        QTRY_VERIFY(button->isVisibleTo(&dialog));
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(applications, 1);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.copied, copy);
        QVERIFY(!result.cancelled);
        QCOMPARE(result.directory, copy ? paths.currentDirectory : paths.legacyDirectory);
        QCOMPARE(get(oldFile), original);
        if (copy) QCOMPARE(get(QDir(paths.currentDirectory).filePath("synthetic.dat")), original);
        else {
            QVERIFY(!QFileInfo::exists(paths.currentDirectory));
            QCOMPARE(planDevProfile(std::nullopt, base.path()).action, DevProfileAction::OfferCopy);
        }
    }
};

QTEST_MAIN(ProfileMigrationDialogTest)
#include "tst_profile_migration_dialog.moc"
