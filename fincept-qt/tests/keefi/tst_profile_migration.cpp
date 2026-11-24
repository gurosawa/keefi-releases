#include "keefi/PlatformSecurity.h"
#include "keefi/ProfileMigration.h"
#include "keefi/ProfilePaths.h"
#include "keefi/Vault.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace keefi;

#ifndef KEEFI_DEV99_PROFILE_FIXTURE
#error KEEFI_DEV99_PROFILE_FIXTURE must identify the preserved synthetic Dev99 vault
#endif

namespace {
const QString Version = QStringLiteral("100");
const QString Marker = QStringLiteral(".keefi-profile-copy.json");
const QString FixturePassword = QStringLiteral("FINIX synthetic Dev99 password 2026!");
const QByteArray FixtureHash("2e136e637980c02d9e3499b32452344faa2b722494e44a6ebf68eb24d01e85e6");
const auto PrivateFile = QFile::ReadOwner | QFile::WriteOwner;
const auto PrivateDirectory = PrivateFile | QFile::ExeOwner;

// These are private temporary fixture paths, never the user's default profile.
// A not-yet-implemented devProfilePaths() must not direct fixture writes.
struct Fixture {
    QTemporaryDir root;
    DevProfilePaths paths;

    Fixture() {
        paths.baseDirectory = QFileInfo(root.path()).canonicalFilePath();
        paths.legacyDirectory = filePath("FINIX/FINIX Dev");
        paths.currentDirectory = filePath("KEEFI/KEEFI Dev");
        paths.transactionDirectory = filePath("KEEFI/.keefi-dev-profile-copy");
    }

    QString path() const { return paths.baseDirectory; }
    QString filePath(const QString& relative) const { return QDir(path()).filePath(relative); }

    bool contains(const QString& path) const {
        const auto clean = QDir::cleanPath(path);
        return root.isValid() && !paths.baseDirectory.isEmpty() &&
               (clean == paths.baseDirectory || clean.startsWith(paths.baseDirectory + '/'));
    }

    bool directory(const QString& path) const {
        if (!contains(path) || !QDir().mkpath(path)) return false;
        QString current = QDir::cleanPath(path);
        while (contains(current)) {
            if (!QFile::setPermissions(current, PrivateDirectory)) return false;
            if (current == paths.baseDirectory) return true;
            current = QFileInfo(current).absolutePath();
        }
        return false;
    }

    bool write(const QString& path, const QByteArray& bytes) const {
        if (!contains(path) || !directory(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !file.setPermissions(PrivateFile))
            return false;
        return file.write(bytes) == bytes.size() && file.flush();
    }

    bool legacy() const {
        const QDir old(paths.legacyDirectory);
        return directory(old.path()) && directory(old.filePath("empty-directory")) &&
               write(old.filePath("payload.bin"), QByteArray("FINIX_SYNTHETIC_PAYLOAD_ONLY") + QByteArray::fromHex("00ff")) &&
               write(old.filePath("zero.bin"), {}) &&
               write(old.filePath(".synthetic-hidden"), QByteArray("HIDDEN_SYNTHETIC_ONLY")) &&
               write(old.filePath(QStringLiteral("nested/한글 이름.bin")), QByteArray("NESTED_SYNTHETIC_ONLY")) &&
               write(old.filePath("notes.lock"), QByteArray("ORDINARY_DATA_NOT_A_WRITER_LOCK")) &&
               write(old.filePath("notes.tmp"), QByteArray("ORDINARY_DATA_NOT_A_TEMPORARY_WRITE")) &&
               write(old.filePath("FINIX/FINIX Dev.ini"),
                     QByteArray("; retain this FINIX comment\r\n[General]\r\nunknown=FINIX\r\nnumber=17\r\n") +
                         QStringLiteral("korean=가짜 설정\r\n[group]\r\nflag=true\r\n").toUtf8()) &&
               write(old.filePath("FINIX.ini"), QByteArray("[General]\r\norganizationFallback=41\r\n"));
    }

    ProfileMigrationPlan offer() const {
        ProfileMigrationPlan plan;
        plan.action = DevProfileAction::OfferCopy;
        plan.paths = paths;
        plan.selectedDirectory = paths.legacyDirectory;
        plan.settings = {QStringLiteral("FINIX"), QStringLiteral("FINIX Dev")};
        return plan;
    }
};

QByteArray readBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool replaceBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size() && file.flush();
}

struct FileState {
    int kind = 0;
    QByteArray bytes;
    quint64 inode = 0;
    quint64 device = 0;
    quint64 owner = 0;
    quint64 group = 0;
    quint32 mode = 0;
    qint64 modifiedSeconds = 0;
    qint64 modifiedNanoseconds = 0;
    bool operator==(const FileState&) const = default;
};
using TreeState = QMap<QString, FileState>;

TreeState treeState(const QString& directory) {
    TreeState state;
    QDirIterator it(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        FileState value;
#ifdef Q_OS_UNIX
        struct stat statValue {};
        if (::lstat(QFile::encodeName(path).constData(), &statValue) != 0) {
            value.kind = -1;
        } else {
            value.kind = S_ISREG(statValue.st_mode) ? 1 : S_ISDIR(statValue.st_mode) ? 2 :
                         S_ISLNK(statValue.st_mode) ? 3 : 4;
            value.inode = statValue.st_ino;
            value.device = statValue.st_dev;
            value.owner = statValue.st_uid;
            value.group = statValue.st_gid;
            value.mode = statValue.st_mode & 07777;
            if (value.kind == 1) {
#ifdef Q_OS_MACOS
                value.modifiedSeconds = statValue.st_mtimespec.tv_sec;
                value.modifiedNanoseconds = statValue.st_mtimespec.tv_nsec;
#else
                value.modifiedSeconds = statValue.st_mtim.tv_sec;
                value.modifiedNanoseconds = statValue.st_mtim.tv_nsec;
#endif
                value.bytes = readBytes(path);
            } else if (value.kind == 3) {
                value.bytes = QFileInfo(path).symLinkTarget().toUtf8();
            }
        }
#else
        const QFileInfo info(path);
        value.kind = info.isSymLink() ? 3 : info.isDir() ? 2 : info.isFile() ? 1 : 4;
        value.mode = quint32(info.permissions());
        if (value.kind == 1) value.bytes = readBytes(path);
#endif
        state.insert(QDir(directory).relativeFilePath(path), value);
    }
    return state;
}

QString treeDifference(const TreeState& expected, const TreeState& actual) {
    if (expected.keys() != actual.keys()) return QStringLiteral("relative path list changed");
    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        if (!(it.value() == actual.value(it.key()))) return QStringLiteral("changed: ") + it.key();
    return {};
}

QString mappedName(const QString& name) {
    if (name == "FINIX/FINIX Dev.ini") return QStringLiteral("KEEFI/KEEFI Dev.ini");
    if (name == "FINIX.ini") return QStringLiteral("KEEFI.ini");
    return name;
}

QString copiedDifference(const TreeState& source, const QString& destination) {
    const auto actual = treeState(destination);
    QStringList expectedFiles{Marker}, actualFiles;
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        if (it->kind == 2 && it.key() != "FINIX") {
            const auto target = actual.constFind(it.key());
            if (target == actual.cend() || target->kind != 2 || target->mode != it->mode ||
                target->owner != it->owner || target->group != it->group)
                return QStringLiteral("missing directory or changed directory permissions: ") + it.key();
        }
        if (it->kind != 1) continue;
        expectedFiles.append(mappedName(it.key()));
        const auto target = actual.constFind(mappedName(it.key()));
        if (target == actual.cend() || target->kind != 1)
            return QStringLiteral("missing copied file: ") + mappedName(it.key());
        if (target->bytes != it->bytes || target->mode != it->mode ||
            target->owner != it->owner || target->group != it->group)
            return QStringLiteral("copy bytes/permissions differ: ") + mappedName(it.key());
    }
    for (auto it = actual.cbegin(); it != actual.cend(); ++it) {
        if (it->kind != 1 && it->kind != 2) return QStringLiteral("unexpected special file in the copy");
        if (it->kind == 1) actualFiles.append(it.key());
    }
    expectedFiles.sort();
    actualFiles.sort();
    if (actualFiles != expectedFiles)
        return QStringLiteral("copied file list/count does not match source plus completion marker");
    if (QFileInfo::exists(QDir(destination).filePath("FINIX/FINIX Dev.ini")) ||
        QFileInfo::exists(QDir(destination).filePath("FINIX.ini")))
        return QStringLiteral("old settings file names remain in the copy");
    return {};
}

class ChunkWriter final : public QIODevice {
public:
    QByteArray bytes;
    qint64 limit = std::numeric_limits<qint64>::max();
    bool zeroAtLimit = false;
    explicit ChunkWriter(qint64 budget = std::numeric_limits<qint64>::max(), bool zero = false)
        : limit(budget), zeroAtLimit(zero) { open(QIODevice::WriteOnly | QIODevice::Unbuffered); }
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 length) override {
        if (bytes.size() >= limit) {
            setErrorString(QStringLiteral("synthetic ENOSPC"));
            return zeroAtLimit ? 0 : -1;
        }
        const auto count = std::min({length, qint64(7), limit - bytes.size()});
        bytes.append(data, count);
        return count;
    }
};

class BrokenReader final : public QIODevice {
public:
    const QByteArray payload = QByteArray(53, 'R');
    qint64 position = 0;
    BrokenReader() { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
    bool isSequential() const override { return true; }
    bool atEnd() const override { return position == payload.size(); }
    qint64 bytesAvailable() const override { return payload.size() - position + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 maxSize) override {
        if (position >= 11) {
            setErrorString(QStringLiteral("synthetic read failure"));
            return -1;
        }
        const auto count = std::min(maxSize, qint64(11) - position);
        std::memcpy(data, payload.constData() + position, size_t(count));
        position += count;
        return count;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
};

#ifdef Q_OS_UNIX
QString crashAfterStage(Fixture& fixture, int phase) {
    const pid_t pid = ::fork();
    if (pid < 0) return QStringLiteral("synthetic child fork failed");
    if (pid == 0) {
        QString error;
        auto session = ProfileCopySession::begin(fixture.paths, Version, &error);
        if (!session) ::_Exit(40);
        if (phase >= 1 && !session->copy(&error)) ::_Exit(41);
        if (phase >= 2 && !session->verify(&error)) ::_Exit(42);
        if (phase >= 3 && !session->publish(&error)) ::_Exit(43);
        // Only this synthetic child exits. Its destructors deliberately do not run.
        ::_Exit(0);
    }
    QElapsedTimer timer;
    timer.start();
    int status = 0;
    while (timer.elapsed() < 5000) {
        const auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return {};
            return QStringLiteral("synthetic stage child did not complete: %1").arg(
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
        if (result < 0 && errno != EINTR) break;
        QTest::qWait(10);
    }
    fixture.root.setAutoRemove(false);
    return QStringLiteral("synthetic child not confirmed exited; temporary files preserved");
}
#endif
} // namespace

class ProfileMigrationTest final : public QObject {
    Q_OBJECT
private slots:
    void injectedBaseDeterminesOnlyDevPaths();
    void decisionIsReadOnlyForEveryUnmarkedState_data();
    void decisionIsReadOnlyForEveryUnmarkedState();
    void explicitDirectoryBypassesBrokenDefaultState();
    void choicesKeepBothOriginalProfilesUntouched_data();
    void choicesKeepBothOriginalProfilesUntouched();
    void copyRenamesOnlyIniPathsAndKeepsTheOldDev99VaultCompatible();
    void completedProfileCanChangeAndStillBeSelected();
    void invalidCompletionMarkerNeverAuthorizesTheCurrentProfile_data();
    void invalidCompletionMarkerNeverAuthorizesTheCurrentProfile();
    void reservedCompletionMarkerIsNeverOverwritten();
    void automaticSelectionsDoNotTurnCancelIntoAnExit_data();
    void automaticSelectionsDoNotTurnCancelIntoAnExit();
    void copyRechecksTheDestinationAfterAnOffer();
    void sourceReadFailureCleansTheStageAndKeepsOriginalBytes();
    void foreignLocksAreBusyAndRemainByteIdentical_data();
    void foreignLocksAreBusyAndRemainByteIdentical();
    void copyFailureFallsBackToLegacyAndKeepsItsSettingsIdentity();
    void activeSessionBlocksLegacyAndSecondMigration();
    void sourceAndStagingChangesCannotPassVerification_data();
    void sourceAndStagingChangesCannotPassVerification();
    void changesAfterVerificationCannotBePublished_data();
    void changesAfterVerificationCannotBePublished();
    void destinationClaimCannotBeOverwritten_data();
    void destinationClaimCannotBeOverwritten();
    void publishRequiresCopyAndVerification_data();
    void publishRequiresCopyAndVerification();
    void iniMappingCollisionNeverLosesEitherFile_data();
    void iniMappingCollisionNeverLosesEitherFile();
    void dataTypesAreSafeAndOrdinaryModesArePreserved_data();
    void dataTypesAreSafeAndOrdinaryModesArePreserved();
    void abandonedSessionRemovesOnlyItsOwnTemporaryFiles();
    void shortWritesStillCopyEveryByte();
    void streamErrorsAreNotReportedAsSuccessfulCopies_data();
    void streamErrorsAreNotReportedAsSuccessfulCopies();
    void interruptedSessionsRecoverOrKeepTheCommittedCopy_data();
    void interruptedSessionsRecoverOrKeepTheCommittedCopy();
    void replacedOldLockIsNeverDeletedDuringRecovery();
    void replacedStageIsNeverDeletedDuringRecovery();
    void copiedVaultStillClearsPrivateStateOnSecurityLock();
};

void ProfileMigrationTest::injectedBaseDeterminesOnlyDevPaths() {
    Fixture f;
    QVERIFY(f.root.isValid());
    const auto before = treeState(f.path());
    const auto paths = devProfilePaths(f.path());
    QCOMPARE(paths.baseDirectory, f.paths.baseDirectory);
    QCOMPARE(paths.legacyDirectory, f.paths.legacyDirectory);
    QCOMPARE(paths.currentDirectory, f.paths.currentDirectory);
    QCOMPARE(paths.transactionDirectory, f.paths.transactionDirectory);
    QVERIFY(f.contains(paths.transactionDirectory));
    QVERIFY(paths.transactionDirectory != paths.currentDirectory);
    QVERIFY(paths.transactionDirectory != paths.legacyDirectory);
    QCOMPARE(treeDifference(before, treeState(f.path())), QString{});
}

void ProfileMigrationTest::decisionIsReadOnlyForEveryUnmarkedState_data() {
    QTest::addColumn<bool>("oldExists");
    QTest::addColumn<bool>("newExists");
    QTest::addColumn<int>("action");
    QTest::newRow("fresh") << false << false << int(DevProfileAction::CreateCurrent);
    QTest::newRow("legacy-only") << true << false << int(DevProfileAction::OfferCopy);
    QTest::newRow("current-only-no-marker") << false << true << int(DevProfileAction::UseCurrent);
    QTest::newRow("both-no-marker") << true << true << int(DevProfileAction::ChooseExisting);
}

void ProfileMigrationTest::decisionIsReadOnlyForEveryUnmarkedState() {
    QFETCH(bool, oldExists);
    QFETCH(bool, newExists);
    QFETCH(int, action);
    Fixture f;
    QVERIFY(f.root.isValid());
    if (oldExists) QVERIFY(f.legacy());
    if (newExists) QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath("current-only.bin"), "SYNTHETIC_NEW"));
    const auto before = treeState(f.path());
    const auto plan = planDevProfile(std::nullopt, f.path());
    QCOMPARE(int(plan.action), action);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.paths.legacyDirectory, f.paths.legacyDirectory);
    QCOMPARE(plan.paths.currentDirectory, f.paths.currentDirectory);
    if (plan.action == DevProfileAction::UseCurrent || plan.action == DevProfileAction::CreateCurrent) {
        QCOMPARE(plan.selectedDirectory, f.paths.currentDirectory);
        QCOMPARE(plan.settings.organization, QStringLiteral("KEEFI"));
        QCOMPARE(plan.settings.application, QStringLiteral("KEEFI Dev"));
    }
    QCOMPARE(treeDifference(before, treeState(f.path())), QString{});
}

void ProfileMigrationTest::explicitDirectoryBypassesBrokenDefaultState() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath(Marker), "NOT_VALID_JSON{"));
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath("instance.lock"), "FOREIGN_LOCK"));
    const auto explicitPath = f.filePath("explicit-synthetic-profile");
    QVERIFY(f.write(QDir(explicitPath).filePath("FINIX/FINIX Dev.ini"), "[General]\nonlyHere=17\n"));
    const auto before = treeState(f.path());
    const auto plan = planDevProfile(std::optional<QString>{explicitPath + "/unused/.."}, f.path());
    QCOMPARE(plan.action, DevProfileAction::UseExplicit);
    QCOMPARE(plan.selectedDirectory, explicitPath);
    QCOMPARE(plan.settings.organization, QStringLiteral("FINIX"));
    QCOMPARE(plan.settings.application, QStringLiteral("FINIX Dev"));
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    const auto result = applyDevProfileChoice(plan, ProfileMigrationChoice::UseCurrent, Version);
    QCOMPARE(result.directory, explicitPath);
    QVERIFY(!result.copied);
    QVERIFY(!result.cancelled);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(treeDifference(before, treeState(f.path())), QString{});
}

void ProfileMigrationTest::choicesKeepBothOriginalProfilesUntouched_data() {
    QTest::addColumn<int>("choice");
    QTest::newRow("later") << int(ProfileMigrationChoice::Later);
    QTest::newRow("choose-legacy") << int(ProfileMigrationChoice::UseLegacy);
    QTest::newRow("choose-current") << int(ProfileMigrationChoice::UseCurrent);
    QTest::newRow("cancel") << int(ProfileMigrationChoice::Cancel);
}

void ProfileMigrationTest::choicesKeepBothOriginalProfilesUntouched() {
    QFETCH(int, choice);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    // Explicit selection priority must not change Later/UseLegacy's old identity.
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath("KEEFI/KEEFI Dev.ini"), "[General]\nnewName=99\n"));
    const auto selected = ProfileMigrationChoice(choice);
    if (selected == ProfileMigrationChoice::UseLegacy || selected == ProfileMigrationChoice::UseCurrent)
        QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath("current.bin"), "KEEP_CURRENT"));
    auto plan = f.offer();
    if (selected == ProfileMigrationChoice::UseLegacy || selected == ProfileMigrationChoice::UseCurrent)
        plan.action = DevProfileAction::ChooseExisting;
    const auto before = treeState(f.path());
    const auto result = applyDevProfileChoice(plan, selected, Version);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!result.copied);
    QCOMPARE(result.cancelled, selected == ProfileMigrationChoice::Cancel);
    if (selected != ProfileMigrationChoice::Cancel) {
        const bool current = selected == ProfileMigrationChoice::UseCurrent;
        QCOMPARE(result.directory, current ? f.paths.currentDirectory : f.paths.legacyDirectory);
        QCOMPARE(result.settings.organization, current ? QString("KEEFI") : QString("FINIX"));
        QCOMPARE(result.settings.application, current ? QString("KEEFI Dev") : QString("FINIX Dev"));
    }
    QCOMPARE(treeDifference(before, treeState(f.path())), QString{});
    if (selected == ProfileMigrationChoice::Later)
        QCOMPARE(planDevProfile(std::nullopt, f.path()).action, DevProfileAction::OfferCopy);
}

void ProfileMigrationTest::copyRenamesOnlyIniPathsAndKeepsTheOldDev99VaultCompatible() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto fixturePath = QString::fromUtf8(KEEFI_DEV99_PROFILE_FIXTURE);
    const auto ciphertext = readBytes(fixturePath);
    QCOMPARE(ciphertext.size(), qsizetype(760));
    QCOMPARE(QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256).toHex(), FixtureHash);
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath("credentials.finix-vault"), ciphertext));
    const auto old = treeState(f.paths.legacyDirectory);
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY2(result.copied, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.directory, f.paths.currentDirectory);
    QCOMPARE(result.settings.organization, QStringLiteral("KEEFI"));
    QCOMPARE(result.settings.application, QStringLiteral("KEEFI Dev"));
    QCOMPARE(copiedDifference(old, result.directory), QString{});
    const auto marker = QJsonDocument::fromJson(readBytes(QDir(result.directory).filePath(Marker))).object();
    QCOMPARE(marker.value("format").toString(), QStringLiteral("keefi-dev-profile-copy/1"));
    QCOMPARE(marker.value("source").toString(), f.paths.legacyDirectory);
    QCOMPARE(marker.value("target").toString(), f.paths.currentDirectory);
    QCOMPARE(marker.value("version").toString(), Version);
    QVERIFY(marker.value("verified").toBool());
    QVERIFY(QDateTime::fromString(marker.value("completed_at_utc").toString(), Qt::ISODateWithMs).isValid());
    int fileCount = 0;
    qint64 byteCount = 0;
    for (const auto& file : old) if (file.kind == 1) { ++fileCount; byteCount += file.bytes.size(); }
    QCOMPARE(marker.value("file_count").toInt(-1), fileCount);
    QCOMPARE(marker.value("total_bytes").toVariant().toLongLong(), byteCount);
    const auto manifestHash = marker.value("source_manifest_sha256").toString().toLatin1();
    QCOMPARE(manifestHash.size(), qsizetype(64));
    QVERIFY(std::all_of(manifestHash.cbegin(), manifestHash.cend(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
    Vault vault(QDir(result.directory).filePath("credentials.finix-vault"));
    QString error;
    QVERIFY2(vault.unlock(FixturePassword, &error), qPrintable(error));
    QCOMPARE(vault.accounts().size(), 1);
    const auto account = vault.accounts().first();
    QCOMPARE(account.id, QStringLiteral("00000000-0000-4000-8000-000000000099"));
    QCOMPARE(account.alias, QStringLiteral("DEV99_SYNTHETIC_ACCOUNT_NOT_REAL"));
    QCOMPARE(account.provider, QStringLiteral("upbit"));
    QCOMPARE(account.environment, QStringLiteral("live"));
    QCOMPARE(account.market, QStringLiteral("spot"));
    QCOMPARE(account.currency, QStringLiteral("KRW"));
    QVERIFY(account.readOnly);
    bool gotExistingKey = false;
    QVERIFY2(vault.withAssetsLedgerKey(false, [&](const SecureBuffer& key) {
        gotExistingKey = key.size() == 32;
    }, &error), qPrintable(error));
    QVERIFY(gotExistingKey);
    vault.lock();
    QVERIFY(vault.accounts().isEmpty());
    QCOMPARE(readBytes(vault.filePath()), ciphertext);
    QCOMPARE(readBytes(fixturePath), ciphertext);
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
}

void ProfileMigrationTest::completedProfileCanChangeAndStillBeSelected() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    const auto copied = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY2(copied.copied, qPrintable(copied.error));
    QVERIFY(replaceBytes(QDir(f.paths.currentDirectory).filePath("payload.bin"), "NORMAL_NEW_PROFILE_SAVE"));
    QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath("new-user-setting.bin"), "SYNTHETIC_NEW_SAVE"));
    const auto changed = treeState(f.paths.currentDirectory);
    const auto plan = planDevProfile(std::nullopt, f.path());
    QCOMPARE(plan.action, DevProfileAction::UseCurrent);
    QCOMPARE(plan.selectedDirectory, f.paths.currentDirectory);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(treeDifference(changed, treeState(f.paths.currentDirectory)), QString{});
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
}

void ProfileMigrationTest::invalidCompletionMarkerNeverAuthorizesTheCurrentProfile_data() {
    QTest::addColumn<QString>("fault");
    QTest::newRow("malformed-json") << QString("json");
    QTest::newRow("wrong-format") << QString("format");
    QTest::newRow("wrong-source-binding") << QString("source");
    QTest::newRow("wrong-target-binding") << QString("target");
    QTest::newRow("not-verified") << QString("verified");
}

void ProfileMigrationTest::invalidCompletionMarkerNeverAuthorizesTheCurrentProfile() {
    QFETCH(QString, fault);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    const auto copied = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY2(copied.copied, qPrintable(copied.error));
    const auto markerPath = QDir(f.paths.currentDirectory).filePath(Marker);
    const auto original = readBytes(markerPath);
    QVERIFY(!original.isEmpty());
    if (fault == "json") {
        QVERIFY(replaceBytes(markerPath, "NOT_VALID_JSON{"));
    } else {
        auto marker = QJsonDocument::fromJson(original).object();
        QVERIFY(!marker.isEmpty());
        if (fault == "verified") marker["verified"] = false;
        else if (fault == "format") marker["format"] = QStringLiteral("unknown-future-format/999");
        else marker[fault] = f.filePath("wrong-synthetic-binding");
        QVERIFY(replaceBytes(markerPath, QJsonDocument(marker).toJson(QJsonDocument::Compact)));
    }
    const auto newAfterInjection = treeState(f.paths.currentDirectory);
    const auto plan = planDevProfile(std::nullopt, f.path());
    QCOMPARE(plan.action, DevProfileAction::Unavailable);
    QVERIFY(!plan.error.isEmpty());
    QCOMPARE(treeDifference(newAfterInjection, treeState(f.paths.currentDirectory)), QString{});
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
}

void ProfileMigrationTest::reservedCompletionMarkerIsNeverOverwritten() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath(Marker), "ORIGINAL_RESERVED_NAME_DATA"));
    const auto old = treeState(f.paths.legacyDirectory);
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY(!result.copied);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.directory, f.paths.legacyDirectory);
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::automaticSelectionsDoNotTurnCancelIntoAnExit_data() {
    QTest::addColumn<QString>("state");
    QTest::newRow("fresh-create-current") << QString("fresh");
    QTest::newRow("existing-current") << QString("current");
    QTest::newRow("explicit") << QString("explicit");
}

void ProfileMigrationTest::automaticSelectionsDoNotTurnCancelIntoAnExit() {
    QFETCH(QString, state);
    Fixture f;
    QVERIFY(f.root.isValid());
    std::optional<QString> explicitPath;
    QString expected = f.paths.currentDirectory;
    if (state == "current") QVERIFY(f.write(QDir(expected).filePath("current.bin"), "SYNTHETIC_CURRENT"));
    if (state == "explicit") {
        expected = f.filePath("explicit-synthetic");
        QVERIFY(f.directory(expected));
        explicitPath = expected;
    }
    const auto before = treeState(f.path());
    const auto plan = planDevProfile(explicitPath, f.path());
    QVERIFY2(plan.action != DevProfileAction::Unavailable, qPrintable(plan.error));
    const auto result = applyDevProfileChoice(plan, ProfileMigrationChoice::Cancel, Version);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.copied);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.directory, expected);
    QCOMPARE(treeDifference(before, treeState(f.path())), QString{});
}

void ProfileMigrationTest::copyRechecksTheDestinationAfterAnOffer() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto plan = f.offer();
    QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath("foreign-current.bin"), "NEWLY_CLAIMED_CURRENT"));
    const auto old = treeState(f.paths.legacyDirectory);
    const auto current = treeState(f.paths.currentDirectory);
    const auto result = applyDevProfileChoice(plan, ProfileMigrationChoice::Copy, Version);
    QVERIFY(!result.copied);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.directory, f.paths.legacyDirectory);
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QCOMPARE(treeDifference(current, treeState(f.paths.currentDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::sourceReadFailureCleansTheStageAndKeepsOriginalBytes() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    const auto source = QDir(f.paths.legacyDirectory).filePath("payload.bin");
    struct RestorePermissions {
        QString path;
        QFileDevice::Permissions permissions;
        ~RestorePermissions() { QFile::setPermissions(path, permissions); }
    } restore{source, QFile::permissions(source)};
    QVERIFY(QFile::setPermissions(source, QFileDevice::Permissions{}));
    QFile probe(source);
    QVERIFY2(!probe.open(QIODevice::ReadOnly), "chmod 000 must actually prevent this test process from reading");
    QVERIFY(!session->copy(&error));
    QVERIFY(!error.isEmpty());
    session.reset();
    QVERIFY(QFile::setPermissions(source, restore.permissions));
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::foreignLocksAreBusyAndRemainByteIdentical_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("contents");
    for (const auto& name : {QString("instance.lock"), QString("credentials.finix-vault.lock"),
                            QString("assets.finix.lock"), QString("assets-current.finix.lock"),
                            QString("swaps.finix.lock"), QString("prices.finix.lock"),
                            QString("imported-records.finix.lock"), QString("FINIX/FINIX Dev.ini.lock"),
                            QString("KEEFI/KEEFI Dev.ini.lock"), QString("FINIX.ini.lock"), QString("KEEFI.ini.lock")}) {
        const auto prefix = name.toUtf8();
        QTest::newRow((prefix + "-empty").constData()) << name << QByteArray{};
        QTest::newRow((prefix + "-foreign").constData()) << name << QByteArray("999999999\nforeign-synthetic-app\nforeign-host\n");
    }
}

void ProfileMigrationTest::foreignLocksAreBusyAndRemainByteIdentical() {
    QFETCH(QString, name);
    QFETCH(QByteArray, contents);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath(name), contents));
    const auto old = treeState(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY(!session);
    QVERIFY(!error.isEmpty());
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::copyFailureFallsBackToLegacyAndKeepsItsSettingsIdentity() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath("credentials.finix-vault.lock"), "FOREIGN_WRITER"));
    const auto before = treeState(f.paths.legacyDirectory);
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY(!result.copied);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.directory, f.paths.legacyDirectory);
    QCOMPARE(result.settings.organization, QStringLiteral("FINIX"));
    QCOMPARE(result.settings.application, QStringLiteral("FINIX Dev"));
    QCOMPARE(treeDifference(before, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
}

void ProfileMigrationTest::activeSessionBlocksLegacyAndSecondMigration() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    QString error;
    auto first = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(first, qPrintable(error));
    const auto locked = treeState(f.paths.legacyDirectory);
    QVERIFY(locked.contains("instance.lock"));
    {
        QLockFile legacy(QDir(f.paths.legacyDirectory).filePath("instance.lock"));
        legacy.setStaleLockTime(0);
        QVERIFY(!legacy.tryLock(0));
    }
    QCOMPARE(treeDifference(locked, treeState(f.paths.legacyDirectory)), QString{});
    const auto control = treeState(f.paths.transactionDirectory);
    QVERIFY(!control.isEmpty());
    auto second = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY(!second);
    QVERIFY(!error.isEmpty());
    QCOMPARE(treeDifference(control, treeState(f.paths.transactionDirectory)), QString{});
    QCOMPARE(treeDifference(locked, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY2(first->copy(&error), qPrintable(error));
    QVERIFY2(first->verify(&error), qPrintable(error));
    QVERIFY2(first->publish(&error), qPrintable(error));
    first.reset();
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QCOMPARE(copiedDifference(old, f.paths.currentDirectory), QString{});
}

void ProfileMigrationTest::sourceAndStagingChangesCannotPassVerification_data() {
    QTest::addColumn<QString>("change");
    QTest::newRow("stage-same-size-byte") << QString("stage-byte");
    QTest::newRow("stage-missing-file") << QString("stage-missing");
    QTest::newRow("stage-extra-file") << QString("stage-extra");
    QTest::newRow("source-same-size-byte") << QString("source-byte");
}

void ProfileMigrationTest::sourceAndStagingChangesCannotPassVerification() {
    QFETCH(QString, change);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    const auto stage = session->stagingDirectory();
    QVERIFY(f.contains(stage));
    QVERIFY(stage.startsWith(f.paths.transactionDirectory + '/'));
    QVERIFY2(session->copy(&error), qPrintable(error));
    const auto payload = QDir(change.startsWith("source") ? f.paths.legacyDirectory : stage).filePath("payload.bin");
    if (change.endsWith("byte")) {
        auto bytes = readBytes(payload);
        QVERIFY(!bytes.isEmpty());
        bytes[0] = bytes.at(0) == 'X' ? 'Y' : 'X';
        QVERIFY(replaceBytes(payload, bytes));
    } else if (change == "stage-missing") {
        QVERIFY(QFile::remove(payload));
    } else {
        QVERIFY(f.write(QDir(stage).filePath("extra.bin"), "SYNTHETIC_EXTRA"));
    }
    auto sourceAfterInjection = treeState(f.paths.legacyDirectory);
    sourceAfterInjection.remove("instance.lock");
    QVERIFY(!session->verify(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!session->publish(&error));
    session.reset();
    QCOMPARE(treeDifference(sourceAfterInjection, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::changesAfterVerificationCannotBePublished_data() {
    QTest::addColumn<bool>("changeSource");
    QTest::newRow("source-after-verify") << true;
    QTest::newRow("stage-after-verify") << false;
}

void ProfileMigrationTest::changesAfterVerificationCannotBePublished() {
    QFETCH(bool, changeSource);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    QVERIFY2(session->copy(&error), qPrintable(error));
    QVERIFY2(session->verify(&error), qPrintable(error));
    const auto path = QDir(changeSource ? f.paths.legacyDirectory : session->stagingDirectory()).filePath("payload.bin");
    auto bytes = readBytes(path);
    QVERIFY(!bytes.isEmpty());
    bytes[0] = bytes.at(0) == 'X' ? 'Y' : 'X';
    QVERIFY(replaceBytes(path, bytes));
    auto sourceAfterInjection = treeState(f.paths.legacyDirectory);
    sourceAfterInjection.remove("instance.lock");
    QVERIFY(!session->publish(&error));
    QVERIFY(!error.isEmpty());
    session.reset();
    QCOMPARE(treeDifference(sourceAfterInjection, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::destinationClaimCannotBeOverwritten_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("directory-with-foreign-data") << QString("directory");
    QTest::newRow("empty-directory") << QString("empty");
    QTest::newRow("file") << QString("file");
#ifdef Q_OS_UNIX
    QTest::newRow("dangling-symlink") << QString("symlink");
#endif
}

void ProfileMigrationTest::destinationClaimCannotBeOverwritten() {
    QFETCH(QString, kind);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    QVERIFY2(session->copy(&error), qPrintable(error));
    QVERIFY2(session->verify(&error), qPrintable(error));
    if (kind == "directory") {
        QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath("foreign.bin"), "FOREIGN_TARGET_DATA"));
    } else if (kind == "empty") {
        QVERIFY(f.directory(f.paths.currentDirectory));
    } else if (kind == "file") {
        QVERIFY(f.write(f.paths.currentDirectory, "FOREIGN_TARGET_FILE"));
    } else {
#ifdef Q_OS_UNIX
        QCOMPARE(::symlink(QFile::encodeName(f.filePath("nonexistent-synthetic-target")).constData(),
                           QFile::encodeName(f.paths.currentDirectory).constData()), 0);
#endif
    }
    const auto targetState = treeState(f.paths.currentDirectory);
    const auto targetParent = QFileInfo(f.paths.currentDirectory).absolutePath();
    const auto targetName = QFileInfo(f.paths.currentDirectory).fileName();
    const auto occupiedRoot = treeState(targetParent).value(targetName);
    const auto targetBytes = kind == "file" ? readBytes(f.paths.currentDirectory) : QByteArray{};
    QVERIFY(!session->publish(&error));
    QVERIFY(!error.isEmpty());
    session.reset();
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(occupiedRoot == treeState(targetParent).value(targetName));
    if (kind == "symlink") {
        QVERIFY(QFileInfo(f.paths.currentDirectory).isSymLink());
        QCOMPARE(QFileInfo(f.paths.currentDirectory).symLinkTarget(), f.filePath("nonexistent-synthetic-target"));
    } else if (kind == "file") {
        QCOMPARE(readBytes(f.paths.currentDirectory), targetBytes);
    } else {
        QVERIFY(QFileInfo(f.paths.currentDirectory).isDir());
        QCOMPARE(treeDifference(targetState, treeState(f.paths.currentDirectory)), QString{});
    }
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::publishRequiresCopyAndVerification_data() {
    QTest::addColumn<bool>("copyFirst");
    QTest::newRow("before-copy") << false;
    QTest::newRow("copied-but-not-verified") << true;
}

void ProfileMigrationTest::publishRequiresCopyAndVerification() {
    QFETCH(bool, copyFirst);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    if (copyFirst) QVERIFY2(session->copy(&error), qPrintable(error));
    QVERIFY(!session->publish(&error));
    QVERIFY(!error.isEmpty());
    session.reset();
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
}

void ProfileMigrationTest::iniMappingCollisionNeverLosesEitherFile_data() {
    QTest::addColumn<QString>("newName");
    QTest::newRow("application-ini") << QString("KEEFI/KEEFI Dev.ini");
    QTest::newRow("organization-ini") << QString("KEEFI.ini");
}

void ProfileMigrationTest::iniMappingCollisionNeverLosesEitherFile() {
    QFETCH(QString, newName);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath(newName), "[General]\nFOREIGN_NEW_NAME=99\n"));
    const auto old = treeState(f.paths.legacyDirectory);
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY(!result.copied);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.directory, f.paths.legacyDirectory);
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::dataTypesAreSafeAndOrdinaryModesArePreserved_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("ordinary-0644-file-and-0755-directory") << QString("permissions");
#ifdef Q_OS_UNIX
    QTest::newRow("data-symlink") << QString("symlink");
    QTest::newRow("data-hardlink") << QString("hardlink");
    QTest::newRow("data-fifo") << QString("fifo");
#endif
}

void ProfileMigrationTest::dataTypesAreSafeAndOrdinaryModesArePreserved() {
    QFETCH(QString, kind);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto external = f.filePath("external-synthetic/canary.bin");
    QVERIFY(f.write(external, "KEEP_EXTERNAL_SYNTHETIC_BYTES"));
    const auto unsafe = QDir(f.paths.legacyDirectory).filePath("unsafe-data");
    if (kind == "permissions") {
        QVERIFY(f.write(unsafe, "SYNTHETIC_ORDINARY_0644_FILE"));
        QVERIFY(QFile::setPermissions(unsafe, PrivateFile | QFile::ReadGroup | QFile::ReadOther));
        QVERIFY(QFile::setPermissions(QDir(f.paths.legacyDirectory).filePath("nested"),
                                     PrivateDirectory | QFile::ReadGroup | QFile::ExeGroup |
                                         QFile::ReadOther | QFile::ExeOther));
    } else {
#ifdef Q_OS_UNIX
        const auto encodedExternal = QFile::encodeName(external);
        const auto encodedUnsafe = QFile::encodeName(unsafe);
        if (kind == "symlink") QCOMPARE(::symlink(encodedExternal.constData(), encodedUnsafe.constData()), 0);
        else if (kind == "hardlink") QCOMPARE(::link(encodedExternal.constData(), encodedUnsafe.constData()), 0);
        else QCOMPARE(::mkfifo(encodedUnsafe.constData(), 0600), 0);
#endif
    }
    const auto old = treeState(f.paths.legacyDirectory);
    const auto externalBefore = treeState(QFileInfo(external).absolutePath());
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    if (kind == "permissions") {
        QVERIFY2(result.copied, qPrintable(result.error));
        QVERIFY(result.error.isEmpty());
        QCOMPARE(copiedDifference(old, f.paths.currentDirectory), QString{});
        QCOMPARE(QFile::permissions(QDir(f.paths.currentDirectory).filePath("nested")),
                 QFile::permissions(QDir(f.paths.legacyDirectory).filePath("nested")));
    } else {
        QVERIFY(!result.copied);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    }
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QCOMPARE(treeDifference(externalBefore, treeState(QFileInfo(external).absolutePath())), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfileMigrationTest::abandonedSessionRemovesOnlyItsOwnTemporaryFiles() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto unknown = f.filePath("KEEFI/.keefi-dev-profile-copy-foreign/canary.bin");
    QVERIFY(f.write(unknown, "FOREIGN_TRANSACTION_NOT_OURS"));
    const auto unknownBefore = treeState(QFileInfo(unknown).absolutePath());
    const auto old = treeState(f.paths.legacyDirectory);
    QString error;
    {
        auto session = ProfileCopySession::begin(f.paths, Version, &error);
        QVERIFY2(session, qPrintable(error));
        QVERIFY2(session->copy(&error), qPrintable(error));
        QVERIFY(QFileInfo::exists(session->stagingDirectory()));
    }
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QCOMPARE(treeDifference(unknownBefore, treeState(QFileInfo(unknown).absolutePath())), QString{});
}

void ProfileMigrationTest::shortWritesStillCopyEveryByte() {
    QByteArray payload;
    for (int i = 0; i < 513; ++i) payload.append(char(i % 256));
    QBuffer input(&payload);
    QVERIFY(input.open(QIODevice::ReadOnly));
    ChunkWriter output;
    QString error;
    QVERIFY2(copyProfileStream(input, output, &error), qPrintable(error));
    QCOMPARE(output.bytes, payload);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void ProfileMigrationTest::streamErrorsAreNotReportedAsSuccessfulCopies_data() {
    QTest::addColumn<QString>("fault");
    QTest::newRow("partial-write-then-ENOSPC") << QString("write");
    QTest::newRow("partial-write-then-no-progress") << QString("zero");
    QTest::newRow("partial-read-then-error") << QString("read");
}

void ProfileMigrationTest::streamErrorsAreNotReportedAsSuccessfulCopies() {
    QFETCH(QString, fault);
    QString error;
    if (fault == "read") {
        BrokenReader input;
        ChunkWriter output;
        QVERIFY(!copyProfileStream(input, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(input.position, qint64(11));
        QVERIFY(output.bytes.size() <= 11);
    } else {
        QByteArray payload(53, 'W');
        QBuffer input(&payload);
        QVERIFY(input.open(QIODevice::ReadOnly));
        ChunkWriter output(11, fault == "zero");
        QVERIFY(!copyProfileStream(input, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(output.bytes, payload.left(11));
    }
}

void ProfileMigrationTest::interruptedSessionsRecoverOrKeepTheCommittedCopy_data() {
    QTest::addColumn<int>("phase");
    QTest::newRow("after-begin") << 0;
    QTest::newRow("after-copy") << 1;
    QTest::newRow("after-verify") << 2;
    QTest::newRow("after-atomic-publish") << 3;
}

void ProfileMigrationTest::interruptedSessionsRecoverOrKeepTheCommittedCopy() {
#ifndef Q_OS_UNIX
    QSKIP("This synthetic destructor-free child fixture uses POSIX fork/waitpid.");
#else
    QFETCH(int, phase);
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto old = treeState(f.paths.legacyDirectory);
    const auto unknown = f.filePath("KEEFI/.keefi-dev-profile-copy-foreign/keep.bin");
    QVERIFY(f.write(unknown, "FOREIGN_INTERRUPTED_DATA"));
    const auto unknownBefore = treeState(QFileInfo(unknown).absolutePath());
    const auto childError = crashAfterStage(f, phase);
    QVERIFY2(childError.isEmpty(), qPrintable(childError));
    QString error;
    if (phase < 3) {
        QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
        QVERIFY(QFileInfo::exists(f.paths.transactionDirectory));
        auto recovered = ProfileCopySession::begin(f.paths, Version, &error);
        QVERIFY2(recovered, qPrintable(error));
        QVERIFY2(recovered->copy(&error), qPrintable(error));
        QVERIFY2(recovered->verify(&error), qPrintable(error));
        QVERIFY2(recovered->publish(&error), qPrintable(error));
        recovered.reset();
        QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
        QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    } else {
        const auto committed = treeState(f.paths.currentDirectory);
        QVERIFY(!committed.isEmpty());
        const auto plan = planDevProfile(std::nullopt, f.path());
        QCOMPARE(plan.action, DevProfileAction::UseCurrent);
        const auto result = applyDevProfileChoice(plan, ProfileMigrationChoice::UseCurrent, Version);
        QCOMPARE(result.directory, f.paths.currentDirectory);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(treeDifference(committed, treeState(f.paths.currentDirectory)), QString{});
        auto oldAfter = treeState(f.paths.legacyDirectory);
        oldAfter.remove("instance.lock");
        QCOMPARE(treeDifference(old, oldAfter), QString{});
    }
    QCOMPARE(copiedDifference(old, f.paths.currentDirectory), QString{});
    QCOMPARE(treeDifference(unknownBefore, treeState(QFileInfo(unknown).absolutePath())), QString{});
#endif
}

void ProfileMigrationTest::replacedOldLockIsNeverDeletedDuringRecovery() {
#ifndef Q_OS_UNIX
    QSKIP("This synthetic destructor-free child fixture uses POSIX fork/waitpid.");
#else
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto childError = crashAfterStage(f, 1);
    QVERIFY2(childError.isEmpty(), qPrintable(childError));
    const auto lock = QDir(f.paths.legacyDirectory).filePath("instance.lock");
    QVERIFY(QFileInfo::exists(lock));
    QVERIFY(QFile::remove(lock));
    QVERIFY(f.write(lock, "FOREIGN_REPLACEMENT_LOCK"));
    const auto oldAfterInjection = treeState(f.paths.legacyDirectory);
    const auto controlAfterInjection = treeState(f.paths.transactionDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY(!session);
    QVERIFY(!error.isEmpty());
    QCOMPARE(treeDifference(oldAfterInjection, treeState(f.paths.legacyDirectory)), QString{});
    QCOMPARE(treeDifference(controlAfterInjection, treeState(f.paths.transactionDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
#endif
}

void ProfileMigrationTest::replacedStageIsNeverDeletedDuringRecovery() {
#ifndef Q_OS_UNIX
    QSKIP("This synthetic destructor-free child fixture uses POSIX fork/waitpid.");
#else
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto childError = crashAfterStage(f, 1);
    QVERIFY2(childError.isEmpty(), qPrintable(childError));
    const auto stage = QDir(f.paths.transactionDirectory).filePath("profile");
    QVERIFY(f.contains(stage));
    const auto parked = f.filePath("preserved-interrupted-staging");
    QVERIFY(QDir().rename(stage, parked));
    QVERIFY(f.write(QDir(stage).filePath("foreign-canary.bin"), "FOREIGN_RECOVERY_STAGE_MUST_REMAIN"));
    const auto old = treeState(f.paths.legacyDirectory);
    const auto replacement = treeState(stage);
    const auto parkedBefore = treeState(parked);
    const auto control = treeState(f.paths.transactionDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY(!session);
    QVERIFY(!error.isEmpty());
    QCOMPARE(treeDifference(old, treeState(f.paths.legacyDirectory)), QString{});
    QCOMPARE(treeDifference(replacement, treeState(stage)), QString{});
    QCOMPARE(treeDifference(parkedBefore, treeState(parked)), QString{});
    QCOMPARE(treeDifference(control, treeState(f.paths.transactionDirectory)), QString{});
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
#endif
}

void ProfileMigrationTest::copiedVaultStillClearsPrivateStateOnSecurityLock() {
    Fixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(f.legacy());
    const auto ciphertext = readBytes(QString::fromUtf8(KEEFI_DEV99_PROFILE_FIXTURE));
    QCOMPARE(QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256).toHex(), FixtureHash);
    QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath("credentials.finix-vault"), ciphertext));
    const auto source = treeState(f.paths.legacyDirectory);
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY2(result.copied, qPrintable(result.error));
    Vault vault(QDir(result.directory).filePath("credentials.finix-vault"));
    QString error;
    QVERIFY2(vault.unlock(FixturePassword, &error), qPrintable(error));
    QCOMPARE(vault.accounts().size(), 1);
    const auto accountId = vault.accounts().first().id;
    PlatformSecurity security;
    connect(&security, &PlatformSecurity::lockRequested, &vault, [&vault](const QString&) { vault.lock(); });
    QSignalSpy requested(&security, &PlatformSecurity::lockRequested);
    QSignalSpy locked(&vault, &Vault::locked);
    QSignalSpy cleared(&vault, &Vault::accountsChanged);
    // This is the native adapter's existing application slot, not a physical OS lock.
    security.requestLock(QStringLiteral("session_inactive"));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(locked.count(), 1);
    QCOMPARE(cleared.count(), 1);
    QVERIFY(!vault.isUnlocked());
    QVERIFY(vault.accounts().isEmpty());
    bool credentialsCalled = false;
    QVERIFY(!vault.withCredentials(accountId, [&](const CredentialSet&) { credentialsCalled = true; }, &error));
    QVERIFY(!credentialsCalled);
    bool keyCalled = false;
    QVERIFY(!vault.withAssetsLedgerKey(false, [&](const SecureBuffer&) { keyCalled = true; }, &error));
    QVERIFY(!keyCalled);
    QCOMPARE(readBytes(vault.filePath()), ciphertext);
    QCOMPARE(treeDifference(source, treeState(f.paths.legacyDirectory)), QString{});
}

QTEST_GUILESS_MAIN(ProfileMigrationTest)
#include "tst_profile_migration.moc"
