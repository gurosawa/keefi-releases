#include <QtTest>
#include "keefi/ProfileUpgrade.h"
#include "keefi/ProfileRestore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QProcess>
#include <QUuid>
#include <cstdlib>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <QTemporaryDir>

using namespace keefi;

namespace {
const QString Password = QStringLiteral("SYNTHETIC migration password 2026!");
const QString Version = QStringLiteral("synthetic-dev101");
const QString Marker = QStringLiteral(".keefi-profile.json");
const QString CopyMarker = QStringLiteral(".keefi-profile-copy.json");

QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool privateDirectory(const QString& path) {
    return QDir().mkpath(path)
        && QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
bool write(const QString& path, const QByteArray& contents) {
    if (!privateDirectory(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(contents) != contents.size() || !file.flush()) return false;
    file.close();
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
QMap<QString, QByteArray> snapshot(const QString& root) {
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        const auto relative = QDir(root).relativeFilePath(path);
        if (relative == "instance.lock") continue;
        result.insert(relative, QCryptographicHash::hash(bytes(path), QCryptographicHash::Sha256).toHex()
            + ':' + QByteArray::number(int(QFileInfo(path).permissions())));
    }
    return result;
}
QJsonObject currentMarker(const QString& scheme = QStringLiteral("legacy-v1")) {
    return {{"format", "keefi-profile/2"}, {"identity_scheme", scheme}, {"verified", true},
            {"version", Version}, {"source_kind", "renamed-dev"},
            {"source_manifest_sha256", QString(64, QLatin1Char('0'))},
            {"completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}

struct Fixture {
    QTemporaryDir temporary;
    QString base, old, dev100, active;
    Fixture() {
        base = temporary.filePath("data");
        old = QDir(base).filePath("FINIX/FINIX Dev");
        dev100 = QDir(base).filePath("KEEFI/KEEFI Dev");
        active = QDir(base).filePath("KEEFI/Profiles/Dev");
    }
    bool makeSource(const QString& source) {
        return temporary.isValid()
            && source.startsWith(temporary.path() + '/')
            && write(QDir(source).filePath("credentials.finix-vault"), "FINIX_SYNTHETIC_CIPHERTEXT_ONLY")
            && write(QDir(source).filePath("FINIX/FINIX Dev.ini"),
                     "; preserve user text\r\n[General]\r\nunknown=FINIX\r\nnumber=17\r\n")
            && write(QDir(source).filePath("FINIX.ini"), "[General]\norganizationFallback=41\n")
            && write(QDir(source).filePath("notes.tmp"), "SYNTHETIC_UNKNOWN_FILE_MUST_SURVIVE")
            && write(QDir(source).filePath("notes.lock"), "SYNTHETIC_ORDINARY_FILE")
            && privateDirectory(QDir(source).filePath("empty-directory"));
    }
    bool addDev100Marker() {
        const QJsonObject marker{{"format", "keefi-dev-profile-copy/1"},
            {"source", old}, {"target", dev100}, {"version", "dev100"},
            {"verified", true}, {"completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"file_count", 5}, {"total_bytes", 123},
            {"source_manifest_sha256", QString(64, QLatin1Char('a'))}};
        return write(QDir(dev100).filePath(CopyMarker), QJsonDocument(marker).toJson(QJsonDocument::Compact));
    }
};

ProfileStageConverter syntheticConverter(int* calls = nullptr, QString* seenStage = nullptr) {
    return [calls, seenStage](const QString& stage, const QString& password,
                             ProfileConversionReceipt* receipt, QString* error) {
        if (receipt) *receipt = {};
        if (calls) ++*calls;
        if (seenStage) *seenStage = stage;
        if (password != Password) {
            if (error) *error = QStringLiteral("합성 암호가 올바르지 않습니다.");
            return false;
        }
        const auto old = QDir(stage).filePath("credentials.finix-vault");
        const auto current = QDir(stage).filePath("credentials.keefi-vault");
        if (bytes(old) != QByteArray("FINIX_SYNTHETIC_CIPHERTEXT_ONLY")
            || !write(current, "KEEFI_SYNTHETIC_CIPHERTEXT_ONLY") || !QFile::remove(old)) {
            if (error) *error = QStringLiteral("합성 저장소 변환 실패");
            return false;
        }
        if (receipt) {
            receipt->verified = true;
            receipt->identityScheme = QStringLiteral("legacy-v1");
            receipt->evidence = {{"adapter", "synthetic-storage-adapter"}};
        }
        return true;
    };
}
}


#ifdef Q_OS_UNIX
int leaveFreshInitializationInterrupted(const QString& base) {
    const auto scope = QDir(base).filePath(".synthetic-upgrade-scope");
    if (!QFileInfo(base).absoluteFilePath().startsWith(QDir::cleanPath(QDir::tempPath()) + '/')
        || bytes(scope) != QByteArray("SYNTHETIC_PROFILE_UPGRADE_ONLY")) return 70;
    const auto paths = profileUpgradePaths(base);
    if (!privateDirectory(paths.activeDirectory) || !privateDirectory(paths.transactionDirectory)) return 71;
    const auto stage = QDir(paths.transactionDirectory).filePath("profile");
    const auto ownedLock = QDir(paths.transactionDirectory).filePath("source-instance.lock");
    const auto instance = QDir(paths.activeDirectory).filePath("instance.lock");
    if (!privateDirectory(stage)) return 72;
    QLockFile lock(ownedLock);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return 73;
    struct stat lockIdentity{}, stageIdentity{};
    if (::lstat(QFile::encodeName(ownedLock).constData(), &lockIdentity)
        || ::lstat(QFile::encodeName(stage).constData(), &stageIdentity)) return 74;
    qint64 pid = 0; QString host, app;
    if (!lock.getLockInfo(&pid, &host, &app)) return 75;
    const auto proof = [](const struct stat& st) {
        return QJsonObject{{"device", QString::number(qulonglong(st.st_dev))},
            {"inode", QString::number(qulonglong(st.st_ino))}, {"uid", qint64(st.st_uid)},
            {"gid", qint64(st.st_gid)}, {"mode", qint64(st.st_mode & 07777)}};
    };
    auto lockProof = proof(lockIdentity);
    lockProof["sha256"] = QString::fromLatin1(QCryptographicHash::hash(bytes(ownedLock), QCryptographicHash::Sha256).toHex());
    lockProof["pid"] = pid; lockProof["host"] = host; lockProof["app"] = app;
    const QJsonObject owner{{"format", "keefi-dev-profile-copy-owner/1"}, {"phase", "locked"},
        {"source", paths.activeDirectory}, {"target", paths.activeDirectory}, {"version", Version},
        {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"pid", pid}, {"host", host}, {"app", app}, {"stage", proof(stageIdentity)}, {"lock", lockProof}};
    if (!write(QDir(paths.transactionDirectory).filePath("owner.json"),
               QJsonDocument(owner).toJson(QJsonDocument::Compact))) return 76;
    if (::link(QFile::encodeName(ownedLock).constData(), QFile::encodeName(instance).constData())) return 77;
    // A synthetic child exits without destructors, just after the same owner
    // proof + hardlink boundary used by fresh-profile initialization.
    std::_Exit(0);
}
#endif


ProfileBackupRestorer syntheticBackupRestorer() {
    return [](const QString& stage, const QString& backup, const QString& password,
              ProfileConversionReceipt* receipt, QString* error) {
        *receipt = {};
        const auto original = bytes(backup);
        if (password != Password || original != QByteArray("SYNTHETIC_LEGACY_BACKUP")) {
            if (error) *error = QStringLiteral("합성 백업 인증 실패");
            return false;
        }
        const QByteArray current("SYNTHETIC_RESTORED_CURRENT_VAULT");
        if (!write(QDir(stage).filePath("credentials.keefi-vault"), current)) return false;
        receipt->verified = true;
        receipt->identityScheme = QStringLiteral("legacy-v1");
        receipt->evidence = {{"restoredBackup", QJsonObject{
            {"name", "credentials.keefi-vault"},
            {"sourceDigest", QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex())},
            {"sourceBytes", qint64(original.size())},
            {"cipherDigest", QString::fromLatin1(QCryptographicHash::hash(current, QCryptographicHash::Sha256).toHex())},
            {"cipherBytes", qint64(current.size())}}}};
        return true;
    };
}
struct RestoreFixture {
    Fixture f;
    ProfileReady current;
    QString backup = f.temporary.filePath("synthetic-backup");
    QString ini = QDir(f.active).filePath("KEEFI/KEEFI Dev.ini");
    bool make() {
        if (!write(QDir(f.base).filePath(".synthetic-upgrade-scope"), "SYNTHETIC_PROFILE_UPGRADE_ONLY")) return false;
        QString error;
        current = createFreshProfile(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        return current.directory == f.active
            && write(ini, "[General]\nwindowSize=@Size(1280 800)\nlabel=SYNTHETIC_KEEP\n")
            && write(backup, "SYNTHETIC_LEGACY_BACKUP");
    }
    bool child(const QString& command, const QString& phase) const {
        QProcess process;
        QStringList arguments{command, f.base};
        if (command == "--synthetic-restore-preparation") arguments << backup;
        arguments << phase;
        process.start(QCoreApplication::applicationFilePath(), arguments);
        return process.waitForStarted(5000) && process.waitForFinished(15000)
            && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }
};
#ifdef Q_OS_UNIX
bool syntheticRestoreScope(const QString& base) {
    return QFileInfo(base).absoluteFilePath().startsWith(QDir::cleanPath(QDir::tempPath()) + '/')
        && bytes(QDir(base).filePath(".synthetic-upgrade-scope")) == QByteArray("SYNTHETIC_PROFILE_UPGRADE_ONLY");
}
int leaveRestorePreparationInterrupted(const QString& base, const QString& backup, const QString& phase) {
    if (!syntheticRestoreScope(base) || !QFileInfo(backup).absoluteFilePath().startsWith(QFileInfo(base).absolutePath() + '/'))
        return 80;
    QString error;
    const auto current = existingProfileReady(planProfileUpgrade(std::nullopt, base), &error);
    if (current.directory.isEmpty()) return 81;
    QLockFile instance(QDir(current.directory).filePath("instance.lock"));
    instance.setStaleLockTime(0);
    if (!instance.tryLock(0)) return 82;
    auto session = ProfileRestoreSession::begin(current, instance, Version, &error);
    if (!session) return 83;
    if (phase == "begin") std::_Exit(0);
    if (!session->copy(&error)) return 84;
    if (phase == "copy") std::_Exit(0);
    if (!session->restore(backup, Password, syntheticBackupRestorer(), &error)) return 85;
    if (phase == "restore") std::_Exit(0);
    if (!session->verify(&error)) return 86;
    if (phase == "verify") std::_Exit(0);
    if (!session->prepareForRestart(&error)) return 87;
    if (phase == "prepared") std::_Exit(0);
    if (phase == "prepared-graceful") {
        session.reset();
        instance.unlock();
        return 0;
    }
    return 88;
}
int leaveRestoreCompletionInterrupted(const QString& base, const QString& phase) {
    if (!syntheticRestoreScope(base)) return 90;
    QString error;
    const auto current = existingProfileReady(planProfileUpgrade(std::nullopt, base), &error);
    if (current.directory.isEmpty()) return 91;
    auto completion = ProfileRestoreCompletion::begin(current, Version, &error);
    if (!completion) return 92;
    if (!completion->refreshPublicSettings(&error)) return 93;
    if (phase == "refreshed") std::_Exit(0);
    if (!completion->verify(&error)) return 94;
    if (phase == "verified") std::_Exit(0);
    if (!completion->publish(&error)) return 95;
    if (phase == "published") std::_Exit(0);
    if (!completion->archive(&error)) return 96;
    if (phase == "archived") std::_Exit(0);
    return 97;
}
#endif

class ProfileUpgradeTest : public QObject {
    Q_OBJECT
private slots:
    void newActiveRootIsOutsideBothLegacySources() {
        Fixture f;
        const auto paths = profileUpgradePaths(f.base);
        QCOMPARE(paths.baseDirectory, f.base);
        QCOMPARE(paths.legacyOriginalDirectory, f.old);
        QCOMPARE(paths.dev100Directory, f.dev100);
        QCOMPARE(paths.activeDirectory, f.active);
        QVERIFY(!paths.transactionDirectory.startsWith(f.old + '/'));
        QVERIFY(!paths.transactionDirectory.startsWith(f.dev100 + '/'));
        QVERIFY(paths.transactionDirectory.startsWith(QDir(f.base).filePath("KEEFI") + '/'));
    }

    void emptyStateCreatesOnlyTheNewIdentity() {
        Fixture f;
        QVERIFY(privateDirectory(f.base));
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::CreateCurrent);
        QCOMPARE(plan.targetDirectory, f.active);
        QString error;
        const auto ready = createFreshProfile(plan, Version, &error);
        QVERIFY2(!ready.directory.isEmpty(), qPrintable(error));
        QCOMPARE(ready.directory, f.active);
        QCOMPARE(ready.identityScheme, QStringLiteral("keefi-v1"));
        const auto marker = QJsonDocument::fromJson(bytes(QDir(f.active).filePath(Marker))).object();
        QCOMPARE(marker.value("identity_scheme").toString(), QStringLiteral("keefi-v1"));
        const auto reopened = completePendingProfileRestore(ready, Version, &error);
        QVERIFY2(!reopened.directory.isEmpty(), qPrintable(error));
        QCOMPARE(reopened.directory, ready.directory);
        QCOMPARE(reopened.identityScheme, ready.identityScheme);
        QVERIFY(!QFileInfo::exists(f.old));
        QVERIFY(!QFileInfo::exists(f.dev100));
        QVERIFY(!snapshot(f.active).contains("credentials.finix-vault"));
    }

    void legacyOnlyOffersConversion() {
        Fixture f; QVERIFY(f.makeSource(f.old));
        const auto before = snapshot(f.old);
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        QCOMPARE(plan.sourceKind, ProfileUpgradeSourceKind::LegacyOriginal);
        QCOMPARE(plan.sourceDirectory, f.old);
        QCOMPARE(plan.targetDirectory, f.active);
        QVERIFY(plan.requiresPassword);
        QCOMPARE(snapshot(f.old), before);
        QVERIFY(!QFileInfo::exists(f.active));
    }

    void dev100FolderIsAnInputEvenThoughItsNameIsAlreadyKeefi() {
        Fixture f; QVERIFY(f.makeSource(f.dev100));
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        QCOMPARE(plan.sourceKind, ProfileUpgradeSourceKind::Dev100Keefi);
        QCOMPARE(plan.sourceDirectory, f.dev100);
        QCOMPARE(plan.targetDirectory, f.active);
    }

    void validDev100CopyWinsOverTheOlderOriginal() {
        Fixture f; QVERIFY(f.makeSource(f.old)); QVERIFY(f.makeSource(f.dev100)); QVERIFY(f.addDev100Marker());
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        QCOMPARE(plan.sourceKind, ProfileUpgradeSourceKind::Dev100Keefi);
        QCOMPARE(plan.sourceDirectory, f.dev100);
    }

    void ambiguousSourcesRequireAnExplicitSelection() {
        Fixture f; QVERIFY(f.makeSource(f.old)); QVERIFY(f.makeSource(f.dev100));
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::ChooseSource);
        QCOMPARE(plan.sources.size(), 2);
        QVERIFY(plan.sourceDirectory.isEmpty());
        QString error;
        const auto chosen = selectProfileUpgradeSource(plan, ProfileUpgradeSourceKind::Dev100Keefi, &error);
        QCOMPARE(chosen.action, ProfileUpgradeAction::OfferUpgrade);
        QCOMPARE(chosen.sourceDirectory, f.dev100);
        QCOMPARE(chosen.targetDirectory, f.active);
    }

    void currentProfileWinsWithoutInspectingBrokenLegacyLocations() {
        Fixture f;
        QVERIFY(write(QDir(f.active).filePath(Marker), QJsonDocument(currentMarker()).toJson()));
        QVERIFY(write(f.old, "SYNTHETIC_NON_DIRECTORY_CANARY"));
        QVERIFY(write(f.dev100, "SYNTHETIC_NON_DIRECTORY_CANARY_2"));
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::UseCurrent);
        QString error;
        const auto ready = existingProfileReady(plan, &error);
        QCOMPARE(ready.directory, f.active);
        QCOMPARE(ready.identityScheme, QStringLiteral("legacy-v1"));
        QCOMPARE(bytes(f.old), QByteArray("SYNTHETIC_NON_DIRECTORY_CANARY"));
    }

    void currentProfileChangesDoNotInvalidateHistoricalImportEvidence() {
        Fixture f;
        QVERIFY(write(QDir(f.active).filePath(Marker), QJsonDocument(currentMarker()).toJson()));
        QVERIFY(write(QDir(f.active).filePath("credentials.keefi-vault"), "SYNTHETIC_NEW_USER_WRITE"));
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::UseCurrent);
        QString error;
        QCOMPARE(existingProfileReady(plan, &error).directory, f.active);
    }

    void explicitEmptyDirectoryNeverInspectsDefaults() {
        Fixture f;
        const auto explicitRoot = f.temporary.filePath("explicit-profile");
        QVERIFY(privateDirectory(explicitRoot));
        QVERIFY(write(f.base, "SYNTHETIC_BROKEN_DEFAULT_BASE"));
        const auto plan = planProfileUpgrade(explicitRoot, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::CreateCurrent);
        QCOMPARE(plan.targetDirectory, explicitRoot);
        QString error;
        const auto ready = createFreshProfile(plan, Version, &error);
        QCOMPARE(ready.directory, explicitRoot);
        QCOMPARE(ready.identityScheme, QStringLiteral("keefi-v1"));
        QCOMPARE(bytes(f.base), QByteArray("SYNTHETIC_BROKEN_DEFAULT_BASE"));
    }

    void explicitLegacyUsesASeparateSiblingAfterSelection() {
        Fixture f;
        const auto source = f.temporary.filePath("explicit-old");
        QVERIFY(f.makeSource(source));
        QVERIFY(write(f.base, "SYNTHETIC_BROKEN_DEFAULT_BASE"));
        const auto before = snapshot(source);
        const auto plan = planProfileUpgrade(source, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        QCOMPARE(plan.sourceKind, ProfileUpgradeSourceKind::ExplicitLegacy);
        QCOMPARE(plan.sourceDirectory, source);
        QCOMPARE(plan.targetDirectory, source + ".keefi");
        QVERIFY(!QFileInfo::exists(plan.targetDirectory));
        QCOMPARE(snapshot(source), before);
        QCOMPARE(bytes(f.base), QByteArray("SYNTHETIC_BROKEN_DEFAULT_BASE"));
    }

    void conversionPublishesOnlyVerifiedNewFilesAndPreservesBothOriginals() {
        Fixture f; QVERIFY(f.makeSource(f.old)); QVERIFY(f.makeSource(f.dev100)); QVERIFY(f.addDev100Marker());
        const auto oldBefore = snapshot(f.old), devBefore = snapshot(f.dev100);
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QString error, observedStage;
        int calls = 0;
        auto session = ProfileUpgradeSession::begin(plan, Version, &error);
        QVERIFY2(session, qPrintable(error));
        QVERIFY(!session->stagingDirectory().startsWith(f.old + '/'));
        QVERIFY(!session->stagingDirectory().startsWith(f.dev100 + '/'));
        QVERIFY2(session->copy(&error), qPrintable(error));
        QCOMPARE(calls, 0);
        QVERIFY(!QFileInfo::exists(QDir(session->stagingDirectory()).filePath(CopyMarker)));
        QVERIFY2(session->convert(Password, syntheticConverter(&calls, &observedStage), &error), qPrintable(error));
        QCOMPARE(calls, 1);
        QCOMPARE(observedStage, session->stagingDirectory());
        QVERIFY(!QFileInfo::exists(f.active));
        QVERIFY2(session->verify(&error), qPrintable(error));
        QVERIFY2(session->publish(&error), qPrintable(error));
        QCOMPARE(session->ready().directory, f.active);
        QCOMPARE(session->ready().identityScheme, QStringLiteral("legacy-v1"));
        QCOMPARE(bytes(QDir(f.active).filePath("credentials.keefi-vault")), QByteArray("KEEFI_SYNTHETIC_CIPHERTEXT_ONLY"));
        QVERIFY(!QFileInfo::exists(QDir(f.active).filePath("credentials.finix-vault")));
        QVERIFY(!QFileInfo::exists(QDir(f.active).filePath("FINIX")));
        QVERIFY(!QFileInfo::exists(QDir(f.active).filePath(CopyMarker)));
        QCOMPARE(bytes(QDir(f.active).filePath("KEEFI/KEEFI Dev.ini")), bytes(QDir(f.dev100).filePath("FINIX/FINIX Dev.ini")));
        QCOMPARE(bytes(QDir(f.active).filePath("KEEFI.ini")), bytes(QDir(f.dev100).filePath("FINIX.ini")));
        QCOMPARE(bytes(QDir(f.active).filePath("notes.tmp")), QByteArray("SYNTHETIC_UNKNOWN_FILE_MUST_SURVIVE"));
        const auto markerBytes = bytes(QDir(f.active).filePath(Marker));
        const auto marker = QJsonDocument::fromJson(markerBytes).object();
        QCOMPARE(marker.value("format").toString(), QStringLiteral("keefi-profile/2"));
        QCOMPARE(marker.value("identity_scheme").toString(), QStringLiteral("legacy-v1"));
        QVERIFY(marker.value("verified").toBool());
        QVERIFY(!markerBytes.contains(f.old.toUtf8()));
        QVERIFY(!markerBytes.contains(f.dev100.toUtf8()));
        QVERIFY(!markerBytes.contains(Password.toUtf8()));
        session.reset();
        QCOMPARE(snapshot(f.old), oldBefore);
        QCOMPARE(snapshot(f.dev100), devBefore);
        QVERIFY(!QFileInfo::exists(QDir(f.dev100).filePath("instance.lock")));
    }

    void passwordFailureNeverSelectsTheOriginalForNormalWriting() {
        Fixture f; QVERIFY(f.makeSource(f.dev100));
        const auto before = snapshot(f.dev100);
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QString error;
        auto session = ProfileUpgradeSession::begin(plan, Version, &error);
        QVERIFY2(session, qPrintable(error));
        QVERIFY2(session->copy(&error), qPrintable(error));
        QVERIFY(!session->convert("SYNTHETIC wrong password", syntheticConverter(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(session->ready().directory.isEmpty());
        QVERIFY(!session->publish(&error));
        session.reset();
        QCOMPARE(snapshot(f.dev100), before);
        QVERIFY(!QFileInfo::exists(f.active));
        auto retry = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY2(retry, qPrintable(error));
        QVERIFY2(retry->copy(&error), qPrintable(error));
        QVERIFY2(retry->convert(Password, syntheticConverter(), &error), qPrintable(error));
        QVERIFY2(retry->verify(&error), qPrintable(error));
        QVERIFY2(retry->publish(&error), qPrintable(error));
    }

    void converterMustReturnVerifiedAndSupportedIdentity_data() {
        QTest::addColumn<bool>("verified");
        QTest::addColumn<QString>("scheme");
        QTest::newRow("not-verified") << false << QString("legacy-v1");
        QTest::newRow("unknown-identity") << true << QString("future-v99");
        QTest::newRow("missing-identity") << true << QString();
    }
    void converterMustReturnVerifiedAndSupportedIdentity() {
        QFETCH(bool, verified); QFETCH(QString, scheme);
        Fixture f; QVERIFY(f.makeSource(f.old));
        const auto before = snapshot(f.old);
        QString error;
        auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY2(session, qPrintable(error)); QVERIFY2(session->copy(&error), qPrintable(error));
        QVERIFY(!session->convert(Password, [verified, scheme](const QString&, const QString&, ProfileConversionReceipt* receipt, QString*) {
            *receipt = {verified, scheme, {}}; return true;
        }, &error));
        QVERIFY(!session->ready().directory.size());
        session.reset();
        QCOMPARE(snapshot(f.old), before);
        QVERIFY(!QFileInfo::exists(f.active));
    }


    void legacyInputCannotSelectFreshIdentitySemantics() {
        Fixture f; QVERIFY(f.makeSource(f.old));
        const auto before = snapshot(f.old);
        QString error;
        auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY2(session, qPrintable(error)); QVERIFY2(session->copy(&error), qPrintable(error));
        const auto convert = syntheticConverter();
        QVERIFY(!session->convert(Password, [convert](const QString& stage, const QString& password,
                                                     ProfileConversionReceipt* receipt, QString* message) {
            if (!convert(stage, password, receipt, message)) return false;
            receipt->identityScheme = QStringLiteral("keefi-v1");
            return true;
        }, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(session->ready().directory.isEmpty());
        session.reset();
        QCOMPARE(snapshot(f.old), before);
        QVERIFY(!QFileInfo::exists(f.active));
    }

    void conversionReceiptMustBindTheProducedCiphertext() {
        Fixture f; QVERIFY(f.makeSource(f.old));
        const auto before = snapshot(f.old);
        QString error;
        auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY2(session, qPrintable(error)); QVERIFY2(session->copy(&error), qPrintable(error));
        const auto convert = syntheticConverter();
        QVERIFY(!session->convert(Password, [convert](const QString& stage, const QString& password,
                                                     ProfileConversionReceipt* receipt, QString* message) {
            if (!convert(stage, password, receipt, message)) return false;
            const QByteArray source("FINIX_SYNTHETIC_CIPHERTEXT_ONLY");
            const QByteArray current("KEEFI_SYNTHETIC_CIPHERTEXT_ONLY");
            receipt->evidence["convertedFiles"] = QJsonArray{QJsonObject{
                {"source", "credentials.finix-vault"}, {"name", "credentials.keefi-vault"},
                {"sourceDigest", QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())},
                {"sourceBytes", qint64(source.size())}, {"cipherBytes", qint64(current.size())},
                {"cipherDigest", QString(64, QLatin1Char('0'))}}};
            return true;
        }, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(session->ready().directory.isEmpty());
        session.reset();
        QCOMPARE(snapshot(f.old), before);
        QVERIFY(!QFileInfo::exists(f.active));
    }

    void sourceMutationCannotPassFinalVerification() {
        Fixture f; QVERIFY(f.makeSource(f.old));
        QString error;
        auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY2(session, qPrintable(error));
        QVERIFY2(session->copy(&error), qPrintable(error));
        QVERIFY2(session->convert(Password, syntheticConverter(), &error), qPrintable(error));
        QVERIFY(write(QDir(f.old).filePath("notes.tmp"), "SYNTHETIC_EXTERNAL_CHANGE"));
        QVERIFY(!session->verify(&error));
        QVERIFY(!session->publish(&error));
        session.reset();
        QCOMPARE(bytes(QDir(f.old).filePath("notes.tmp")), QByteArray("SYNTHETIC_EXTERNAL_CHANGE"));
        QVERIFY(!QFileInfo::exists(f.active));
    }

    void destinationClaimIsNeverOverwritten() {
        Fixture f; QVERIFY(f.makeSource(f.old));
        QString error;
        auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY2(session, qPrintable(error));
        QVERIFY2(session->copy(&error), qPrintable(error));
        QVERIFY2(session->convert(Password, syntheticConverter(), &error), qPrintable(error));
        QVERIFY2(session->verify(&error), qPrintable(error));
        QVERIFY(write(QDir(f.active).filePath("foreign-canary"), "SYNTHETIC_FOREIGN_TARGET"));
        QVERIFY(!session->publish(&error));
        session.reset();
        QCOMPARE(bytes(QDir(f.active).filePath("foreign-canary")), QByteArray("SYNTHETIC_FOREIGN_TARGET"));
    }

    void liveOriginalInstanceBlocksMigrationAndRemainsIntact() {
        Fixture f; QVERIFY(f.makeSource(f.dev100));
        QLockFile lock(QDir(f.dev100).filePath("instance.lock"));
        lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
        const auto lockBytes = bytes(QDir(f.dev100).filePath("instance.lock"));
        const auto before = snapshot(f.dev100);
        QString error;
        auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
        QVERIFY(!session);
        QVERIFY(!error.isEmpty());
        QCOMPARE(bytes(QDir(f.dev100).filePath("instance.lock")), lockBytes);
        QCOMPARE(snapshot(f.dev100), before);
        QVERIFY(!QFileInfo::exists(f.active));
    }

    void cancellationDiscardsOnlyOwnedStage() {
        Fixture f; QVERIFY(f.makeSource(f.dev100));
        const auto before = snapshot(f.dev100);
        const auto foreign = f.temporary.filePath("foreign-stage/canary");
        QVERIFY(write(foreign, "SYNTHETIC_KEEP"));
        QString error, stage;
        {
            auto session = ProfileUpgradeSession::begin(planProfileUpgrade(std::nullopt, f.base), Version, &error);
            QVERIFY2(session, qPrintable(error));
            QVERIFY2(session->copy(&error), qPrintable(error));
            stage = session->stagingDirectory();
            QVERIFY(QFileInfo(stage).isDir());
        }
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!QFileInfo::exists(f.active));
        QCOMPARE(snapshot(f.dev100), before);
        QCOMPARE(bytes(foreign), QByteArray("SYNTHETIC_KEEP"));
    }


    void explicitProductionSettingsMoveToCurrentNames_data() {
        QTest::addColumn<bool>("withVault");
        QTest::newRow("settings-only") << false;
        QTest::newRow("with-vault") << true;
    }
    void explicitProductionSettingsMoveToCurrentNames() {
        QFETCH(bool, withVault);
        Fixture f;
        const auto original = f.temporary.filePath("explicit-production");
        QVERIFY(privateDirectory(original));
        const QByteArray ini("; synthetic user data\r\n[General]\r\nlabel=FINIX\r\nvalue=17\r\n");
        const auto sourceIni = QDir(original).filePath("FINIX/FINIX.ini");
        QVERIFY(write(sourceIni, ini));
        QVERIFY(QFile::setPermissions(sourceIni, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                   | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        if (withVault)
            QVERIFY(write(QDir(original).filePath("credentials.finix-vault"), "FINIX_SYNTHETIC_CIPHERTEXT_ONLY"));
        const auto before = snapshot(original);
        const auto plan = planProfileUpgrade(original, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        QCOMPARE(plan.requiresPassword, withVault);
        QString error;
        auto session = ProfileUpgradeSession::begin(plan, Version, &error);
        QVERIFY2(session, qPrintable(error)); QVERIFY2(session->copy(&error), qPrintable(error));
        const ProfileStageConverter publicOnly = [](const QString&, const QString&, ProfileConversionReceipt* receipt, QString*) {
            *receipt = {true, QStringLiteral("legacy-v1"), {{"convertedFiles", QJsonArray{}}}};
            return true;
        };
        QVERIFY2(session->convert(withVault ? Password : QString{},
                                  withVault ? syntheticConverter() : publicOnly, &error), qPrintable(error));
        QVERIFY2(session->verify(&error), qPrintable(error));
        QVERIFY2(session->publish(&error), qPrintable(error));
        const auto target = original + ".keefi";
        QCOMPARE(session->ready().directory, target);
        const auto targetIni = QDir(target).filePath("KEEFI/KEEFI.ini");
        QCOMPARE(bytes(targetIni), ini);
        QCOMPARE(QFileInfo(targetIni).permissions(), QFileInfo(sourceIni).permissions());
        QVERIFY(!QFileInfo::exists(QDir(target).filePath("KEEFI/FINIX.ini")));
        QVERIFY(!QFileInfo::exists(QDir(target).filePath("FINIX")));
        QCOMPARE(snapshot(original), before);
    }

    void interruptedEmptyInitializationIsRetryableWithoutActivatingOldPaths() {
#ifndef Q_OS_UNIX
        QSKIP("The descriptor-based profile transaction is supported on Unix.");
#else
        Fixture f;
        QVERIFY(write(QDir(f.base).filePath(".synthetic-upgrade-scope"), "SYNTHETIC_PROFILE_UPGRADE_ONLY"));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--synthetic-fresh-profile-interruption"), f.base});
        QVERIFY(child.waitForStarted(5000));
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        const auto paths = profileUpgradePaths(f.base);
        QVERIFY(QFileInfo::exists(QDir(paths.activeDirectory).filePath("instance.lock")));
        QVERIFY(!QFileInfo::exists(QDir(paths.activeDirectory).filePath(Marker)));
        const auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::CreateCurrent);
        QString error;
        const auto ready = createFreshProfile(plan, Version, &error);
        QVERIFY2(!ready.directory.isEmpty(), qPrintable(error));
        QCOMPARE(ready.directory, paths.activeDirectory);
        QCOMPARE(ready.identityScheme, QStringLiteral("keefi-v1"));
        QVERIFY(!QFileInfo::exists(QDir(paths.activeDirectory).filePath("instance.lock")));
        QVERIFY(!QFileInfo::exists(paths.transactionDirectory));
        QVERIFY(!QFileInfo::exists(paths.legacyOriginalDirectory));
        QVERIFY(!QFileInfo::exists(paths.dev100Directory));
        QCOMPARE(planProfileUpgrade(std::nullopt, f.base).action, ProfileUpgradeAction::UseCurrent);
#endif
    }


    void restorePathsAreOutsideTheActiveProfile() {
        RestoreFixture r; QVERIFY(r.make());
        const auto paths = profileRestorePaths(r.current.directory);
        QCOMPARE(paths.currentDirectory, r.f.active);
        QVERIFY(paths.transactionDirectory.startsWith(QFileInfo(r.f.active).absolutePath() + '/'));
        QVERIFY(!paths.transactionDirectory.startsWith(r.f.active + '/'));
        QVERIFY(paths.transactionDirectory != r.f.active);
    }
    void restorePreparationBorrowsTheOwnedLockAndPreservesSource() {
        RestoreFixture r; QVERIFY(r.make());
        const auto before = snapshot(r.f.active);
        QLockFile lock(QDir(r.f.active).filePath("instance.lock"));
        lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
        const auto lockBytes = bytes(QDir(r.f.active).filePath("instance.lock"));
        QString error, stage;
        {
            auto session = ProfileRestoreSession::begin(r.current, lock, Version, &error);
            QVERIFY2(session, qPrintable(error));
            QVERIFY2(session->copy(&error), qPrintable(error));
            stage = session->stagingDirectory();
            QVERIFY(stage.startsWith(r.f.temporary.path() + '/'));
            QVERIFY(!stage.startsWith(r.f.active + '/'));
            QCOMPARE(bytes(QDir(stage).filePath("KEEFI/KEEFI Dev.ini")), bytes(r.ini));
            QVERIFY(lock.isLocked());
            QCOMPARE(bytes(QDir(r.f.active).filePath("instance.lock")), lockBytes);
        }
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(lock.isLocked());
        QCOMPARE(bytes(QDir(r.f.active).filePath("instance.lock")), lockBytes);
        QCOMPARE(snapshot(r.f.active), before);
    }
    void preparedRestoreDoesNotActivateWhileTheAppOwnsItsProfile() {
        RestoreFixture r; QVERIFY(r.make());
        const auto before = snapshot(r.f.active);
        QLockFile lock(QDir(r.f.active).filePath("instance.lock"));
        lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
        QString error, stage;
        {
            auto session = ProfileRestoreSession::begin(r.current, lock, Version, &error);
            QVERIFY2(session, qPrintable(error)); QVERIFY2(session->copy(&error), qPrintable(error));
            QVERIFY2(session->restore(r.backup, Password, syntheticBackupRestorer(), &error), qPrintable(error));
            QVERIFY2(session->verify(&error), qPrintable(error));
            QVERIFY2(session->prepareForRestart(&error), qPrintable(error));
            stage = session->stagingDirectory();
        }
        QVERIFY(QFileInfo::exists(QDir(stage).filePath("credentials.keefi-vault")));
        QVERIFY(!QFileInfo::exists(QDir(r.f.active).filePath("credentials.keefi-vault")));
        QCOMPARE(snapshot(r.f.active), before);
        QVERIFY(!ProfileRestoreCompletion::begin(r.current, Version, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot(r.f.active), before);
        QCOMPARE(bytes(r.backup), QByteArray("SYNTHETIC_LEGACY_BACKUP"));
    }
    void restoreRequiresAnOwnedMatchingInstanceLock() {
        RestoreFixture r; QVERIFY(r.make());
        const auto before = snapshot(r.f.active);
        QLockFile unheld(QDir(r.f.active).filePath("instance.lock"));
        QString error;
        QVERIFY(!ProfileRestoreSession::begin(r.current, unheld, Version, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot(r.f.active), before);
        auto forged = r.current; forged.identityScheme = QStringLiteral("legacy-v1");
        QLockFile held(QDir(r.f.active).filePath("instance.lock"));
        held.setStaleLockTime(0); QVERIFY(held.tryLock(0));
        QVERIFY(!ProfileRestoreSession::begin(forged, held, Version, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(held.isLocked());
        QCOMPARE(snapshot(r.f.active), before);
    }
    void restoreRejectsAnyExistingEncryptedArtifact_data() {
        QTest::addColumn<QString>("name");
        const QStringList names{
            "credentials.keefi-vault", "assets.keefi", "assets-current.keefi", "swaps.keefi",
            "prices.keefi", "imported-records.keefi", "prices.keefi.exit",
            "prices.keefi.unreadable-20261114T000000Z", "user-backup.keefi-vault", "export.keefirtx",
            "credentials.finix-vault", "assets.finix", "user-backup.finix-vault", "export.finixrtx",
            ".keefi-prices-Ab12Cd", ".keefi-vault-Ab12Cd", ".finix-prices-Ab12Cd", ".finix-vault-Ab12Cd",
            "assets.keefi.archive-0123456789abcdef0123456789abcdef.keefi",
            "assets.finix.pre-schema3-" + QString(64, 'a') + ".finix"};
        for (const auto& name : names) QTest::newRow(qPrintable(name)) << name;
    }
    void restoreRejectsAnyExistingEncryptedArtifact() {
        QFETCH(QString, name);
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(write(QDir(r.f.active).filePath(name), "SYNTHETIC_EXISTING_ENCRYPTED_ARTIFACT"));
        const auto before = snapshot(r.f.active);
        QLockFile lock(QDir(r.f.active).filePath("instance.lock"));
        lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
        QString error;
        QVERIFY(!ProfileRestoreSession::begin(r.current, lock, Version, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot(r.f.active), before); QVERIFY(lock.isLocked());
    }
    void restoreRestartKeepsLatestPublicSettingsAndArchivesOriginal_data() {
        QTest::addColumn<QString>("phase");
        QTest::newRow("interrupted-owner") << QStringLiteral("prepared");
        QTest::newRow("normal-quit") << QStringLiteral("prepared-graceful");
    }
    void restoreRestartKeepsLatestPublicSettingsAndArchivesOriginal() {
        QFETCH(QString, phase);
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(r.child("--synthetic-restore-preparation", phase));
        const QByteArray latest("[General]\nwindowSize=@Size(1440 900)\nlabel=SYNTHETIC_LATEST\n");
        QVERIFY(write(r.ini, latest));
        const auto original = snapshot(r.f.active);
        QString error;
        auto completion = ProfileRestoreCompletion::begin(r.current, Version, &error);
        QVERIFY2(completion, qPrintable(error));
        QVERIFY2(completion->refreshPublicSettings(&error), qPrintable(error));
        QVERIFY2(completion->verify(&error), qPrintable(error));
        QVERIFY2(completion->publish(&error), qPrintable(error));
        QCOMPARE(completion->ready().directory, r.f.active);
        QCOMPARE(completion->ready().identityScheme, QStringLiteral("legacy-v1"));
        QVERIFY2(completion->archive(&error), qPrintable(error));
        const auto archive = completion->archiveDirectory();
        QVERIFY(archive.startsWith(r.f.temporary.path() + '/'));
        QCOMPARE(snapshot(archive), original);
        QCOMPARE(bytes(r.ini), latest);
        QCOMPARE(bytes(QDir(r.f.active).filePath("credentials.keefi-vault")), QByteArray("SYNTHETIC_RESTORED_CURRENT_VAULT"));
        QCOMPARE(bytes(r.backup), QByteArray("SYNTHETIC_LEGACY_BACKUP"));
        QVERIFY(!QFileInfo::exists(profileRestorePaths(r.f.active).transactionDirectory));
        const auto nextPlan = planProfileUpgrade(std::nullopt, r.f.base);
        QCOMPARE(nextPlan.action, ProfileUpgradeAction::UseCurrent);
        QCOMPARE(nextPlan.identityScheme, QStringLiteral("legacy-v1"));
    }
    void restoredCiphertextMutationCannotPassStartupVerification() {
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(r.child("--synthetic-restore-preparation", "prepared"));
        const auto before = snapshot(r.f.active);
        const auto paths = profileRestorePaths(r.f.active);
        const auto altered = QDir(paths.transactionDirectory).filePath("profile/credentials.keefi-vault");
        QVERIFY(write(altered, "SYNTHETIC_REPLACEMENT_CIPHERTEXT"));
        QString error;
        const auto ready = completePendingProfileRestore(r.current, Version, &error);
        QVERIFY(ready.directory.isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot(r.f.active), before);
        QCOMPARE(bytes(altered), QByteArray("SYNTHETIC_REPLACEMENT_CIPHERTEXT"));
    }
    void newlyCreatedPrivateDataPreventsRestartReplacement_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("encrypted-store") << QStringLiteral("assets.keefi");
        QTest::newRow("unrelated-new-file") << QStringLiteral("new-user-note");
    }
    void newlyCreatedPrivateDataPreventsRestartReplacement() {
        QFETCH(QString, name);
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(r.child("--synthetic-restore-preparation", "prepared"));
        QVERIFY(write(QDir(r.f.active).filePath(name), "SYNTHETIC_NEW_USER_DATA"));
        const auto before = snapshot(r.f.active);
        QString error;
        const auto ready = completePendingProfileRestore(r.current, Version, &error);
        QVERIFY(ready.directory.isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot(r.f.active), before);
        QCOMPARE(bytes(r.backup), QByteArray("SYNTHETIC_LEGACY_BACKUP"));
    }
    void unfinishedRestorePreparationIsDiscardedWithoutActivation_data() {
        QTest::addColumn<QString>("phase");
        for (const auto& phase : {"begin", "copy", "restore", "verify"}) QTest::newRow(phase) << QString(phase);
    }
    void unfinishedRestorePreparationIsDiscardedWithoutActivation() {
        QFETCH(QString, phase);
        RestoreFixture r; QVERIFY(r.make());
        const auto before = snapshot(r.f.active);
        QVERIFY(r.child("--synthetic-restore-preparation", phase));
        QString error;
        const auto ready = completePendingProfileRestore(r.current, Version, &error);
        QVERIFY2(!ready.directory.isEmpty(), qPrintable(error));
        QCOMPARE(ready.directory, r.f.active); QCOMPARE(ready.identityScheme, QStringLiteral("keefi-v1"));
        QCOMPARE(snapshot(r.f.active), before);
        QVERIFY(!QFileInfo::exists(QDir(r.f.active).filePath("credentials.keefi-vault")));
        QVERIFY(!QFileInfo::exists(profileRestorePaths(r.f.active).transactionDirectory));
    }
    void interruptedRestoreCompletionDoesNotSwapTwice_data() {
        QTest::addColumn<QString>("phase");
        for (const auto& phase : {"refreshed", "verified", "published", "archived"}) QTest::newRow(phase) << QString(phase);
    }
    void interruptedRestoreCompletionDoesNotSwapTwice() {
        QFETCH(QString, phase);
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(r.child("--synthetic-restore-preparation", "prepared"));
        QVERIFY(r.child("--synthetic-restore-completion", phase));
        QString error;
        const auto selected = existingProfileReady(planProfileUpgrade(std::nullopt, r.f.base), &error);
        QVERIFY2(!selected.directory.isEmpty(), qPrintable(error));
        const auto ready = completePendingProfileRestore(selected, Version, &error);
        QVERIFY2(!ready.directory.isEmpty(), qPrintable(error));
        QCOMPARE(ready.directory, r.f.active); QCOMPARE(ready.identityScheme, QStringLiteral("legacy-v1"));
        const auto nextPlan = planProfileUpgrade(std::nullopt, r.f.base);
        QCOMPARE(nextPlan.action, ProfileUpgradeAction::UseCurrent);
        QCOMPARE(nextPlan.identityScheme, QStringLiteral("legacy-v1"));
        QCOMPARE(bytes(QDir(r.f.active).filePath("credentials.keefi-vault")), QByteArray("SYNTHETIC_RESTORED_CURRENT_VAULT"));
        QCOMPARE(bytes(r.backup), QByteArray("SYNTHETIC_LEGACY_BACKUP"));
        const auto archives = QDir(QFileInfo(r.f.active).absolutePath()).entryList(
            {QStringLiteral(".keefi-profile-before-restore-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(archives.size(), 1);
        const auto archive = QDir(QFileInfo(r.f.active).absolutePath()).filePath(archives.front());
        QVERIFY(!QFileInfo::exists(QDir(archive).filePath("credentials.keefi-vault")));
        QCOMPARE(bytes(QDir(archive).filePath("KEEFI/KEEFI Dev.ini")), bytes(r.ini));
    }
    void foreignRestoreOwnerIsPreservedAndBlocksActivation() {
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(r.child("--synthetic-restore-preparation", "prepared"));
        const auto before = snapshot(r.f.active);
        const auto owner = QDir(profileRestorePaths(r.f.active).transactionDirectory).filePath("owner.json");
        QVERIFY(write(owner, "{\"foreign\":true}"));
        QString error;
        QVERIFY(completePendingProfileRestore(r.current, Version, &error).directory.isEmpty());
        QVERIFY(!error.isEmpty()); QCOMPARE(bytes(owner), QByteArray("{\"foreign\":true}"));
        QCOMPARE(snapshot(r.f.active), before);
    }
    void cancelPreservesChangedRestoreControl_data() {
        QTest::addColumn<bool>("replaceOwner");
        QTest::newRow("owner-replaced") << true;
        QTest::newRow("unknown-control-file") << false;
    }
    void cancelPreservesChangedRestoreControl() {
        QFETCH(bool, replaceOwner);
        RestoreFixture r; QVERIFY(r.make());
        QLockFile lock(QDir(r.f.active).filePath("instance.lock"));
        lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
        const auto sourceBefore = snapshot(r.f.active);
        const auto lockBefore = bytes(QDir(r.f.active).filePath("instance.lock"));
        QString error;
        auto session = ProfileRestoreSession::begin(r.current, lock, Version, &error);
        QVERIFY2(session, qPrintable(error)); QVERIFY2(session->copy(&error), qPrintable(error));
        const auto control = profileRestorePaths(r.f.active).transactionDirectory;
        QVERIFY(write(QDir(control).filePath(replaceOwner ? "owner.json" : "foreign-canary"),
                      "SYNTHETIC_FOREIGN_CONTROL"));
        const auto foreignBefore = snapshot(control);
        session.reset();
        QCOMPARE(snapshot(control), foreignBefore);
        QCOMPARE(snapshot(r.f.active), sourceBefore);
        QCOMPARE(bytes(QDir(r.f.active).filePath("instance.lock")), lockBefore);
        QVERIFY(lock.isLocked());
    }
    void archiveConflictAfterCommitNeverRollsBackTheRestoredProfile() {
        RestoreFixture r; QVERIFY(r.make());
        QVERIFY(r.child("--synthetic-restore-preparation", "prepared"));
        QString error;
        auto completion = ProfileRestoreCompletion::begin(r.current, Version, &error);
        QVERIFY2(completion, qPrintable(error));
        QVERIFY2(completion->refreshPublicSettings(&error), qPrintable(error));
        QVERIFY2(completion->verify(&error), qPrintable(error));
        QVERIFY2(completion->publish(&error), qPrintable(error));
        const auto archive = completion->archiveDirectory();
        QVERIFY(archive.startsWith(r.f.temporary.path() + '/'));
        QVERIFY(write(QDir(archive).filePath("foreign-canary"), "SYNTHETIC_FOREIGN_ARCHIVE"));
        QVERIFY(!completion->archive(&error));
        const auto restored = completion->ready();
        QCOMPARE(restored.directory, r.f.active);
        QCOMPARE(restored.identityScheme, QStringLiteral("legacy-v1"));
        QVERIFY(!restored.warning.isEmpty());
        completion.reset();
        QVERIFY(write(QDir(r.f.active).filePath("new-user-note"), "SYNTHETIC_AFTER_PUBLICATION"));
        QVERIFY(write(QDir(r.f.active).filePath("credentials.keefi-vault"), "SYNTHETIC_NORMAL_POSTCOMMIT_VAULT_WRITE"));
        const auto ready = completePendingProfileRestore(restored, Version, &error);
        QCOMPARE(ready.directory, r.f.active);
        QVERIFY(!ready.warning.isEmpty());
        QCOMPARE(bytes(QDir(r.f.active).filePath("new-user-note")), QByteArray("SYNTHETIC_AFTER_PUBLICATION"));
        QCOMPARE(bytes(QDir(archive).filePath("foreign-canary")), QByteArray("SYNTHETIC_FOREIGN_ARCHIVE"));
        QCOMPARE(bytes(QDir(r.f.active).filePath("credentials.keefi-vault")), QByteArray("SYNTHETIC_NORMAL_POSTCOMMIT_VAULT_WRITE"));
    }

    void forgedDestinationPlanIsRejected() {
        Fixture f; QVERIFY(f.makeSource(f.old));
        auto plan = planProfileUpgrade(std::nullopt, f.base);
        QCOMPARE(plan.action, ProfileUpgradeAction::OfferUpgrade);
        plan.targetDirectory = f.temporary.filePath("not-the-approved-target");
        QString error;
        QVERIFY(!ProfileUpgradeSession::begin(plan, Version, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(plan.targetDirectory));
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
#ifdef Q_OS_UNIX
    if (argc == 5 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--synthetic-restore-preparation"))
        return leaveRestorePreparationInterrupted(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]));
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--synthetic-restore-completion"))
        return leaveRestoreCompletionInterrupted(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--synthetic-fresh-profile-interruption"))
        return leaveFreshInitializationInterrupted(QString::fromLocal8Bit(argv[2]));
#endif
    ProfileUpgradeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_profile_upgrade.moc"
