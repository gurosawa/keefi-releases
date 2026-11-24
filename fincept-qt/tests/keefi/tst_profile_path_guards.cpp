#include "keefi/ProfileMigration.h"
#include "keefi/ProfilePaths.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <thread>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace keefi;

namespace {
const auto FileMode = QFile::ReadOwner | QFile::WriteOwner;
const auto DirectoryMode = FileMode | QFile::ExeOwner;
const QString Marker = QStringLiteral(".keefi-profile-copy.json");
const QString Version = QStringLiteral("100");

DevProfilePaths boundPaths(const QString& base) {
    return {base, QDir(base).filePath("FINIX/FINIX Dev"), QDir(base).filePath("KEEFI/KEEFI Dev"),
            QDir(base).filePath("KEEFI/.keefi-dev-profile-copy")};
}

struct GuardFixture {
    QTemporaryDir temporary;
    QString root;
    DevProfilePaths paths;

    GuardFixture() {
        root = QFileInfo(temporary.path()).canonicalFilePath();
        paths = boundPaths(path("application-support"));
    }
    QString path(const QString& relative) const { return QDir(root).filePath(relative); }
    bool contains(const QString& value) const {
        const auto clean = QDir::cleanPath(value);
        return temporary.isValid() && !root.isEmpty() && (clean == root || clean.startsWith(root + '/'));
    }
    bool directory(const QString& value) const {
        if (!contains(value) || !QDir().mkpath(value)) return false;
        auto current = QDir::cleanPath(value);
        while (contains(current)) {
            if (!QFile::setPermissions(current, DirectoryMode)) return false;
            if (current == root) return true;
            current = QFileInfo(current).absolutePath();
        }
        return false;
    }
    bool write(const QString& value, const QByteArray& data) const {
        if (!contains(value) || !directory(QFileInfo(value).absolutePath())) return false;
        QFile file(value);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
               file.setPermissions(FileMode) && file.write(data) == data.size() && file.flush();
    }
    bool legacy() const {
        return write(QDir(paths.legacyDirectory).filePath("payload.bin"), "SYNTHETIC_GUARD_PAYLOAD_ONLY") &&
               write(QDir(paths.legacyDirectory).filePath("FINIX/FINIX Dev.ini"),
                     "; preserve bytes\r\n[General]\r\nunknown=FINIX\r\n");
    }
    ProfileMigrationPlan offer() const {
        ProfileMigrationPlan result;
        result.action = DevProfileAction::OfferCopy;
        result.paths = paths;
        result.selectedDirectory = paths.legacyDirectory;
        result.settings = {QStringLiteral("FINIX"), QStringLiteral("FINIX Dev")};
        return result;
    }
};

QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QByteArray entryState(const QString& path) {
    QJsonObject value;
#ifdef Q_OS_UNIX
    struct stat statValue {};
    if (::lstat(QFile::encodeName(path).constData(), &statValue) != 0) return "MISSING";
    const bool regular = S_ISREG(statValue.st_mode);
    value["type"] = regular ? "file" : S_ISDIR(statValue.st_mode) ? "directory" :
                    S_ISLNK(statValue.st_mode) ? "symlink" : "special";
    value["inode"] = QString::number(qulonglong(statValue.st_ino));
    value["device"] = QString::number(qulonglong(statValue.st_dev));
    value["uid"] = qint64(statValue.st_uid);
    value["gid"] = qint64(statValue.st_gid);
    value["mode"] = int(statValue.st_mode & 07777);
    if (regular) {
        value["bytes"] = QString::fromLatin1(bytes(path).toHex());
#ifdef Q_OS_MACOS
        value["mtime"] = QString::number(qint64(statValue.st_mtimespec.tv_sec));
        value["mtime_ns"] = QString::number(qint64(statValue.st_mtimespec.tv_nsec));
#else
        value["mtime"] = QString::number(qint64(statValue.st_mtim.tv_sec));
        value["mtime_ns"] = QString::number(qint64(statValue.st_mtim.tv_nsec));
#endif
    } else if (S_ISLNK(statValue.st_mode)) {
        value["target"] = QFileInfo(path).symLinkTarget();
    }
#else
    const QFileInfo info(path);
    value["mode"] = int(info.permissions());
    value["type"] = info.isSymLink() ? "symlink" : info.isDir() ? "directory" : "file";
    if (info.isFile() && !info.isSymLink()) value["bytes"] = QString::fromLatin1(bytes(path).toHex());
#endif
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

using Snapshot = QMap<QString, QByteArray>;
Snapshot snapshot(const QString& root) {
    Snapshot result{{QStringLiteral("."), entryState(root)}};
    QDirIterator entries(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const auto path = entries.next();
        result.insert(QDir(root).relativeFilePath(path), entryState(path));
    }
    return result;
}

QStringList regularFiles(const QString& directory) {
    QStringList result;
    QDirIterator entries(directory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) result.append(QDir(directory).relativeFilePath(entries.next()));
    result.sort();
    return result;
}

struct PermissionRestore {
    QString path;
    QFileDevice::Permissions original;
    ~PermissionRestore() { QFile::setPermissions(path, original); }
};
} // namespace

class ProfilePathGuardsTest final : public QObject {
    Q_OBJECT
private slots:
    void explicitIniIdentityUsesTheDocumentedPriority_data();
    void explicitIniIdentityUsesTheDocumentedPriority();
    void symlinkBoundariesPreserveExternalCanaries_data();
    void symlinkBoundariesPreserveExternalCanaries();
    void realWriteDenialPreventsCopyOrCompletion_data();
    void realWriteDenialPreventsCopyOrCompletion();
    void onlyExactKnownTemporaryNamesAreExcluded();
    void unownedOrLiveFixedControlIsPreserved_data();
    void unownedOrLiveFixedControlIsPreserved();
    void selectionRechecksAPathReplacedAfterThePrompt_data();
    void selectionRechecksAPathReplacedAfterThePrompt();
    void macSystemTemporaryAliasRemainsUsable_data();
    void macSystemTemporaryAliasRemainsUsable();
    void completionFifoIsRejectedWithoutWaitingForAWriter();
    void inaccessibleMarkerIsAnErrorInsteadOfAbsence();
    void replacedStagingDirectoryIsPreservedDuringCleanup();
    void replacedOwnerRecordIsPreservedDuringCleanup();
    void replacedParentDoesNotRedirectQtUnlock();
    void completedProfileRemainsAvailableWhenCleanupCannotRun();
};

void ProfilePathGuardsTest::explicitIniIdentityUsesTheDocumentedPriority_data() {
    QTest::addColumn<QStringList>("present");
    QTest::addColumn<bool>("legacy");
    QTest::newRow("new-app-over-all") << QStringList{"KEEFI/KEEFI Dev.ini", "FINIX/FINIX Dev.ini", "KEEFI.ini", "FINIX.ini"} << false;
    QTest::newRow("old-app-over-both-organizations") << QStringList{"FINIX/FINIX Dev.ini", "KEEFI.ini", "FINIX.ini"} << true;
    QTest::newRow("new-organization-over-old") << QStringList{"KEEFI.ini", "FINIX.ini"} << false;
    QTest::newRow("old-organization-only") << QStringList{"FINIX.ini"} << true;
    QTest::newRow("empty-defaults-to-current") << QStringList{} << false;
}

void ProfilePathGuardsTest::explicitIniIdentityUsesTheDocumentedPriority() {
    QFETCH(QStringList, present);
    QFETCH(bool, legacy);
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    const auto explicitPath = f.path("explicit-synthetic-profile");
    QVERIFY(f.directory(explicitPath));
    for (const auto& name : present)
        QVERIFY(f.write(QDir(explicitPath).filePath(name), "; keep FINIX comment\r\n[General]\r\nunknown=KEEFI\r\n"));
    const auto before = snapshot(f.root);
    const auto identity = profileSettingsIdentity(explicitPath);
    QCOMPARE(identity.organization, legacy ? QString("FINIX") : QString("KEEFI"));
    QCOMPARE(identity.application, legacy ? QString("FINIX Dev") : QString("KEEFI Dev"));
    const auto plan = planDevProfile(std::optional<QString>{explicitPath}, f.paths.baseDirectory);
    QCOMPARE(plan.action, DevProfileAction::UseExplicit);
    QCOMPARE(plan.selectedDirectory, explicitPath);
    QCOMPARE(plan.settings.organization, identity.organization);
    QCOMPARE(plan.settings.application, identity.application);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(snapshot(f.root), before);
}

void ProfilePathGuardsTest::symlinkBoundariesPreserveExternalCanaries_data() {
    QTest::addColumn<QString>("boundary");
    QTest::newRow("base") << QString("base");
    QTest::newRow("source") << QString("source");
    QTest::newRow("current-parent") << QString("current-parent");
    QTest::newRow("staging") << QString("staging");
}

void ProfilePathGuardsTest::symlinkBoundariesPreserveExternalCanaries() {
#ifndef Q_OS_UNIX
    QSKIP("These path guards use private POSIX symlink fixtures.");
#else
    QFETCH(QString, boundary);
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto outside = f.path("external-synthetic-directory");
    QVERIFY(f.write(QDir(outside).filePath("canary.bin"), "KEEP_EXTERNAL_SYNTHETIC_CANARY"));
    const auto outsideBefore = snapshot(outside);
    auto paths = f.paths;
    QString sourceLocation = paths.legacyDirectory;
    const auto sourceBefore = snapshot(sourceLocation);
    QString link;
    QString target = outside;
    QString error;
    std::unique_ptr<ProfileCopySession> session;
    if (boundary == "base") {
        link = f.path("aliased-application-support");
        target = paths.baseDirectory;
        paths = boundPaths(link);
    } else if (boundary == "source") {
        link = paths.legacyDirectory;
        sourceLocation = f.path("preserved-original-source");
        QVERIFY(QDir().rename(link, sourceLocation));
    } else if (boundary == "current-parent") {
        link = QFileInfo(paths.currentDirectory).absolutePath();
    } else {
        session = ProfileCopySession::begin(paths, Version, &error);
        QVERIFY2(session, qPrintable(error));
        link = session->stagingDirectory();
        QVERIFY(f.contains(link));
        QVERIFY(link.startsWith(paths.transactionDirectory + '/'));
        if (QFileInfo::exists(link)) QVERIFY(QDir().rmdir(link));
    }
    QVERIFY(f.contains(link));
    QCOMPARE(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()), 0);
    if (boundary != "staging") session = ProfileCopySession::begin(paths, Version, &error);
    const bool copied = session && session->copy(&error);
    QVERIFY(!copied);
    QVERIFY(!error.isEmpty());
    session.reset();
    QCOMPARE(snapshot(outside), outsideBefore);
    QCOMPARE(snapshot(sourceLocation), sourceBefore);
    if (boundary != "staging") {
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(QFileInfo(link).symLinkTarget(), target);
    }
#endif
}

void ProfilePathGuardsTest::realWriteDenialPreventsCopyOrCompletion_data() {
    QTest::addColumn<bool>("afterCopy");
    QTest::newRow("stage-copy-write-denied") << false;
    QTest::newRow("completion-marker-write-denied") << true;
}

void ProfilePathGuardsTest::realWriteDenialPreventsCopyOrCompletion() {
    QFETCH(bool, afterCopy);
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto original = snapshot(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    const auto stage = session->stagingDirectory();
    QVERIFY(f.contains(stage));
    QVERIFY(QFileInfo(stage).isDir());
    if (afterCopy) QVERIFY2(session->copy(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(QDir(stage).filePath(Marker)));
    PermissionRestore restore{stage, QFile::permissions(stage)};
    QVERIFY(QFile::setPermissions(stage, QFile::ReadOwner | QFile::ExeOwner));
    const auto probePath = QDir(stage).filePath("synthetic-write-denial-probe");
    QFile probe(probePath);
    const bool unexpectedlyWritable = probe.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    if (unexpectedlyWritable) { probe.close(); QVERIFY(QFile::remove(probePath)); }
    QVERIFY2(!unexpectedlyWritable, "The private staging directory must actually reject a new file");
    if (afterCopy) QVERIFY(!session->verify(&error));
    else QVERIFY(!session->copy(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!session->publish(&error));
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
    // Restore the environmental fault before asking the owned session to clean up.
    if (QFileInfo::exists(stage)) QVERIFY(QFile::setPermissions(stage, restore.original));
    session.reset();
    QCOMPARE(snapshot(f.paths.legacyDirectory), original);
    QVERIFY(!QFileInfo::exists(f.paths.transactionDirectory));
}

void ProfilePathGuardsTest::onlyExactKnownTemporaryNamesAreExcluded() {
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    QStringList excluded, retained{"payload.bin", "FINIX/FINIX Dev.ini", "ordinary.lock", "ordinary.tmp", "prices.finix.exit", "archive/history.bin"};
    for (const auto& name : retained.mid(2))
        QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath(name), "UNKNOWN_NAME_IS_STILL_ORIGINAL_DATA"));
    for (const auto* prefix : {".finix-vault-", ".finix-restore-", ".finix-assets-", ".finix-prices-", ".finix-record-transfer-"}) {
        const auto valid = QString::fromLatin1(prefix) + "aB09zX";
        QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath(valid), "KNOWN_SYNTHETIC_TEMPORARY_WRITE"));
        excluded.append(valid);
        for (const auto& suffix : {QString("abc12"), QString("abc1234"), QString("abc_12"), QStringLiteral("가나다abc")}) {
            const auto similar = QString::fromLatin1(prefix) + suffix;
            QVERIFY(f.write(QDir(f.paths.legacyDirectory).filePath(similar), "SIMILAR_NAME_MUST_BE_PRESERVED"));
            retained.append(similar);
        }
    }
    const auto original = snapshot(f.paths.legacyDirectory);
    const auto result = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY2(result.copied, qPrintable(result.error));
    QCOMPARE(result.directory, f.paths.currentDirectory);
    for (const auto& name : excluded)
        QVERIFY2(!QFileInfo::exists(QDir(result.directory).filePath(name)), qPrintable(name));
    QStringList expectedFiles{Marker};
    for (const auto& name : retained) {
        const auto mapped = name == "FINIX/FINIX Dev.ini" ? QString("KEEFI/KEEFI Dev.ini") : name;
        expectedFiles.append(mapped);
        QCOMPARE(bytes(QDir(result.directory).filePath(mapped)), bytes(QDir(f.paths.legacyDirectory).filePath(name)));
    }
    expectedFiles.sort();
    QCOMPARE(regularFiles(result.directory), expectedFiles);
    const auto marker = QJsonDocument::fromJson(bytes(QDir(result.directory).filePath(Marker))).object();
    QCOMPARE(marker.value("file_count").toInt(-1), retained.size());
    QCOMPARE(snapshot(f.paths.legacyDirectory), original);
}

void ProfilePathGuardsTest::unownedOrLiveFixedControlIsPreserved_data() {
    QTest::addColumn<QString>("state");
    QTest::newRow("owner-missing") << QString("missing");
    QTest::newRow("owner-malformed") << QString("malformed");
    QTest::newRow("valid-initial-owner-still-live") << QString("live");
    QTest::newRow("owner-path-binding-mismatch") << QString("mismatch");
}

void ProfilePathGuardsTest::unownedOrLiveFixedControlIsPreserved() {
    QFETCH(QString, state);
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.transactionDirectory).filePath("foreign-canary.bin"), "DO_NOT_DELETE_FOREIGN_CONTROL"));
    const auto ownerPath = QDir(f.paths.transactionDirectory).filePath("owner.json");
    if (state == "malformed") QVERIFY(f.write(ownerPath, "BROKEN_OWNER_JSON{"));
    else if (state != "missing") {
        const auto stagePath = QDir(f.paths.transactionDirectory).filePath("profile");
        QVERIFY(f.directory(stagePath));
        auto stageProof = QJsonDocument::fromJson(entryState(stagePath)).object();
        QCOMPARE(stageProof.value("type").toString(), QStringLiteral("directory"));
        stageProof.remove("type");
        QJsonObject owner{{"format", "keefi-dev-profile-copy-owner/1"}, {"phase", "initial"},
                          {"source", f.paths.legacyDirectory}, {"target", f.paths.currentDirectory},
                          {"version", Version}, {"nonce", "synthetic-foreign-owner-nonce"},
                          {"stage", stageProof},
                          {"pid", QCoreApplication::applicationPid()}, {"host", QSysInfo::machineHostName()},
                          {"app", QCoreApplication::applicationName()}};
        if (state == "mismatch") owner["source"] = f.path("different-synthetic-source");
        QVERIFY(f.write(ownerPath, QJsonDocument(owner).toJson(QJsonDocument::Compact)));
    }
    const auto original = snapshot(f.paths.legacyDirectory);
    const auto foreign = snapshot(f.paths.transactionDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY(!session);
    QVERIFY(!error.isEmpty());
    if (state == "live") QVERIFY2(error.contains(QStringLiteral("실행 중")), qPrintable(error));
    QCOMPARE(snapshot(f.paths.legacyDirectory), original);
    QCOMPARE(snapshot(f.paths.transactionDirectory), foreign);
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
}

void ProfilePathGuardsTest::selectionRechecksAPathReplacedAfterThePrompt_data() {
    QTest::addColumn<bool>("chooseCurrent");
    QTest::newRow("use-legacy-after-replacement") << false;
    QTest::newRow("use-current-after-replacement") << true;
}

void ProfilePathGuardsTest::selectionRechecksAPathReplacedAfterThePrompt() {
#ifndef Q_OS_UNIX
    QSKIP("These path guards use private POSIX symlink fixtures.");
#else
    QFETCH(bool, chooseCurrent);
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.write(QDir(f.paths.currentDirectory).filePath("current.bin"), "SYNTHETIC_CURRENT_PROFILE"));
    auto plan = f.offer();
    plan.action = DevProfileAction::ChooseExisting;
    const auto selected = chooseCurrent ? f.paths.currentDirectory : f.paths.legacyDirectory;
    const auto unselected = chooseCurrent ? f.paths.legacyDirectory : f.paths.currentDirectory;
    const auto selectedBefore = snapshot(selected);
    const auto otherBefore = snapshot(unselected);
    const auto preserved = f.path("preserved-selected-profile");
    QVERIFY(QDir().rename(selected, preserved));
    const auto outside = f.path("external-selected-path");
    QVERIFY(f.write(QDir(outside).filePath("canary.bin"), "EXTERNAL_PRIVATE_FIXTURE_ONLY"));
    const auto outsideBefore = snapshot(outside);
    QCOMPARE(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(selected).constData()), 0);
    const auto result = applyDevProfileChoice(plan,
        chooseCurrent ? ProfileMigrationChoice::UseCurrent : ProfileMigrationChoice::UseLegacy, Version);
    QVERIFY(!result.copied);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.directory != selected);
    QVERIFY(QFileInfo(selected).isSymLink());
    QCOMPARE(snapshot(preserved), selectedBefore);
    QCOMPARE(snapshot(unselected), otherBefore);
    QCOMPARE(snapshot(outside), outsideBefore);
#endif
}

void ProfilePathGuardsTest::macSystemTemporaryAliasRemainsUsable_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("default-base") << QString("default");
    QTest::newRow("explicit-existing") << QString("existing");
    QTest::newRow("explicit-new") << QString("new");
}

void ProfilePathGuardsTest::macSystemTemporaryAliasRemainsUsable() {
#ifndef Q_OS_MACOS
    QSKIP("The system /var alias belongs to macOS.");
#else
    QFETCH(QString, kind);
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY2(f.root.startsWith("/private/var/"), "Expected this Mac's private temporary fixture location");
    QVERIFY(f.legacy());
    const auto explicitPath = f.path(kind == "existing" ? "existing-explicit" : "new-explicit");
    if (kind == "existing") QVERIFY(f.directory(explicitPath));
    const auto aliased = [](QString path) { path.remove(0, QString("/private").size()); return path; };
    const auto before = snapshot(f.root);
    const auto plan = kind == "default"
        ? planDevProfile(std::nullopt, aliased(f.paths.baseDirectory))
        : planDevProfile(std::optional<QString>{aliased(explicitPath)}, QString{});
    QCOMPARE(plan.action, kind == "default" ? DevProfileAction::OfferCopy : DevProfileAction::UseExplicit);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    if (kind == "default")
        QCOMPARE(QFileInfo(plan.paths.legacyDirectory).canonicalFilePath(), f.paths.legacyDirectory);
    else if (kind == "existing")
        QCOMPARE(QFileInfo(plan.selectedDirectory).canonicalFilePath(), explicitPath);
    else {
        QCOMPARE(QFileInfo(plan.selectedDirectory).fileName(), QString("new-explicit"));
        QCOMPARE(QFileInfo(QFileInfo(plan.selectedDirectory).absolutePath()).canonicalFilePath(), f.root);
    }
    QCOMPARE(snapshot(f.root), before);
#endif
}

void ProfilePathGuardsTest::completionFifoIsRejectedWithoutWaitingForAWriter() {
#ifndef Q_OS_UNIX
    QSKIP("This synthetic metadata fixture uses a private POSIX FIFO.");
#else
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    QVERIFY(f.directory(f.paths.currentDirectory));
    const auto marker = QDir(f.paths.currentDirectory).filePath(Marker);
    const auto encoded = QFile::encodeName(marker);
    QCOMPARE(::mkfifo(encoded.constData(), 0600), 0);
    const auto before = snapshot(f.root);
    std::atomic_bool returned{false};
    std::atomic_bool neededWriter{false};
    // A blocking pre-fix read is released by this private writer, so the regression
    // fails normally instead of leaving a blocked child or requiring termination.
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (!returned.load()) {
            const int fd = ::open(encoded.constData(), O_WRONLY | O_NONBLOCK | O_NOFOLLOW);
            if (fd >= 0) { neededWriter.store(true); ::close(fd); return; }
            if (errno != ENXIO && errno != EINTR) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto plan = planDevProfile(std::nullopt, f.paths.baseDirectory);
    returned.store(true);
    release.join();
    QVERIFY2(!neededWriter.load(), "Metadata planning blocked until the private FIFO writer connected");
    QCOMPARE(plan.action, DevProfileAction::Unavailable);
    QVERIFY(!plan.error.isEmpty());
    QCOMPARE(snapshot(f.root), before);
#endif
}

void ProfilePathGuardsTest::inaccessibleMarkerIsAnErrorInsteadOfAbsence() {
#ifndef Q_OS_UNIX
    QSKIP("This synthetic metadata fixture uses POSIX directory search permissions.");
#else
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto marker = QDir(f.paths.currentDirectory).filePath(Marker);
    QVERIFY(f.write(marker, "SYNTHETIC_UNREADABLE_COMPLETION_RECORD"));
    const auto original = snapshot(f.root);
    PermissionRestore restore{f.paths.currentDirectory, QFile::permissions(f.paths.currentDirectory)};
    QVERIFY(QFile::setPermissions(f.paths.currentDirectory, QFile::ReadOwner));
    struct stat state {};
    const int inspected = ::lstat(QFile::encodeName(marker).constData(), &state);
    const int inspectionError = errno;
    QCOMPARE(inspected, -1);
    QCOMPARE(inspectionError, EACCES);
    const auto plan = planDevProfile(std::nullopt, f.paths.baseDirectory);
    QVERIFY(QFile::setPermissions(f.paths.currentDirectory, restore.original));
    QCOMPARE(snapshot(f.root), original);
    QCOMPARE(plan.action, DevProfileAction::Unavailable);
    QVERIFY(!plan.error.isEmpty());
#endif
}

void ProfilePathGuardsTest::replacedStagingDirectoryIsPreservedDuringCleanup() {
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto original = snapshot(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    const auto stage = session->stagingDirectory();
    QVERIFY(f.contains(stage));
    const auto parked = f.path("preserved-original-staging-directory");
    QVERIFY(QDir().rename(stage, parked));
    QVERIFY(f.write(QDir(stage).filePath("foreign-canary.bin"), "FOREIGN_STAGE_REPLACEMENT_MUST_REMAIN"));
    const auto replacement = snapshot(stage);
    const auto parkedBefore = snapshot(parked);
    QVERIFY(!session->copy(&error));
    QVERIFY(!error.isEmpty());
    session.reset();
    QCOMPARE(snapshot(stage), replacement);
    QCOMPARE(snapshot(parked), parkedBefore);
    QCOMPARE(snapshot(f.paths.legacyDirectory), original);
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
}

void ProfilePathGuardsTest::replacedOwnerRecordIsPreservedDuringCleanup() {
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto original = snapshot(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    const auto owner = QDir(f.paths.transactionDirectory).filePath("owner.json");
    const auto parked = f.path("preserved-original-owner.json");
    QVERIFY(QFile::rename(owner, parked));
    QVERIFY(f.write(owner, "FOREIGN_OWNER_RECORD_MUST_REMAIN"));
    const auto replacement = entryState(owner);
    const auto parkedBefore = entryState(parked);
    session.reset();
    QCOMPARE(entryState(owner), replacement);
    QCOMPARE(entryState(parked), parkedBefore);
    QCOMPARE(snapshot(f.paths.legacyDirectory), original);
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
}

void ProfilePathGuardsTest::replacedParentDoesNotRedirectQtUnlock() {
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto original = snapshot(f.paths.legacyDirectory);
    QString error;
    auto session = ProfileCopySession::begin(f.paths, Version, &error);
    QVERIFY2(session, qPrintable(error));
    const auto parent = QFileInfo(f.paths.currentDirectory).absolutePath();
    const auto parked = f.path("preserved-original-migration-parent");
    QVERIFY(QDir().rename(parent, parked));
    const auto foreign = QDir(f.paths.transactionDirectory).filePath("source-instance.lock");
    QVERIFY(f.write(foreign, "FOREIGN_ABSOLUTE_LOCK_PATH_MUST_REMAIN"));
    const auto replacement = snapshot(f.paths.transactionDirectory);
    QVERIFY(!session->copy(&error));
    QVERIFY(!error.isEmpty());
    session.reset();
    QCOMPARE(snapshot(f.paths.transactionDirectory), replacement);
    QCOMPARE(snapshot(f.paths.legacyDirectory), original);
    QVERIFY(!QFileInfo::exists(f.paths.currentDirectory));
}

void ProfilePathGuardsTest::completedProfileRemainsAvailableWhenCleanupCannotRun() {
    GuardFixture f;
    QVERIFY(f.temporary.isValid());
    QVERIFY(f.legacy());
    const auto copied = applyDevProfileChoice(f.offer(), ProfileMigrationChoice::Copy, Version);
    QVERIFY2(copied.copied, qPrintable(copied.error));
    const auto current = snapshot(f.paths.currentDirectory);
    const auto old = snapshot(f.paths.legacyDirectory);
    QVERIFY(f.write(QDir(f.paths.transactionDirectory).filePath("foreign-control.bin"),
                    "UNKNOWN_CONTROL_DOES_NOT_REVOKE_A_COMPLETED_COPY"));
    const auto control = snapshot(f.paths.transactionDirectory);
    const auto plan = planDevProfile(std::nullopt, f.paths.baseDirectory);
    QCOMPARE(plan.action, DevProfileAction::UseCurrent);
    const auto chosen = applyDevProfileChoice(plan, ProfileMigrationChoice::Cancel, Version);
    QCOMPARE(chosen.directory, f.paths.currentDirectory);
    QVERIFY2(chosen.error.isEmpty(), qPrintable(chosen.error));
    QVERIFY(!chosen.warning.isEmpty());
    QVERIFY(!chosen.copied);
    QVERIFY(!chosen.cancelled);
    QCOMPARE(snapshot(f.paths.currentDirectory), current);
    QCOMPARE(snapshot(f.paths.legacyDirectory), old);
    QCOMPARE(snapshot(f.paths.transactionDirectory), control);
}

QTEST_GUILESS_MAIN(ProfilePathGuardsTest)
#include "tst_profile_path_guards.moc"
