#include "ProfileMigration.h"
#include "ProfileMigrationPrivate.h"
#include "ProfileUpgrade.h"
#include "ProfileUpgradePrivate.h"
#include "ProfileRestore.h"
#include "LegacyStorageCodecPrivate.h"
#include "legacy/LegacyProfileNames.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QLockFile>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <array>
#include <limits>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/types.h>
#ifdef Q_OS_MACOS
#include <stdio.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#endif

namespace keefi {
using namespace profile_detail;

bool copyProfileStream(QIODevice& input, QIODevice& output, QString* error) {
    clear(error);
    if (!input.isOpen() || !input.isReadable() || !output.isOpen() || !output.isWritable())
        return fail(error, QStringLiteral("복사할 파일을 읽거나 쓸 수 없습니다."));
    std::array<char, 65536> buffer{};
    for (;;) {
        const qint64 count = input.read(buffer.data(), qint64(buffer.size()));
        if (count < 0) return fail(error, QStringLiteral("원본 파일을 읽는 중 오류가 발생했습니다."));
        if (count == 0) {
            if (input.atEnd()) return true;
            return fail(error, QStringLiteral("원본 파일 읽기가 진행되지 않았습니다."));
        }
        qint64 written = 0;
        while (written < count) {
            const qint64 part = output.write(buffer.data() + written, count - written);
            if (part <= 0 || part > count - written)
                return fail(error, QStringLiteral("새 프로필에 파일을 쓰는 중 오류가 발생했습니다."));
            written += part;
        }
    }
}

#ifdef Q_OS_UNIX
namespace {
constexpr auto OwnerName = "owner.json";
constexpr auto SourceLockName = "source-instance.lock";
constexpr auto StageName = "profile";
constexpr auto LegacyLockName = "instance.lock";

bool closeChecked(Fd& fd, QString* error) {
    if (!fd) return true;
    const int descriptor = fd.release();
    if (::close(descriptor) != 0)
        return fail(error, QStringLiteral("프로필 파일을 닫는 중 오류가 발생했습니다."));
    return true;
}
bool syncFd(int fd, QString* error) {
    int result;
    do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
    return result == 0 || fail(error, QStringLiteral("프로필 파일을 디스크에 동기화하지 못했습니다."));
}
bool exclusiveRename(int sourceDirectory, const QByteArray& source,
                     int targetDirectory, const QByteArray& target, QString* error) {
#ifdef Q_OS_MACOS
    const int result = ::renameatx_np(sourceDirectory, source.constData(),
                                    targetDirectory, target.constData(), RENAME_EXCL);
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    const int result = int(::syscall(SYS_renameat2, sourceDirectory, source.constData(),
                                     targetDirectory, target.constData(), RENAME_NOREPLACE));
#else
    Q_UNUSED(sourceDirectory); Q_UNUSED(source); Q_UNUSED(targetDirectory); Q_UNUSED(target);
    return fail(error, QStringLiteral("이 시스템은 프로필의 원자적 게시를 지원하지 않습니다."));
#endif
#if defined(Q_OS_MACOS) || (defined(Q_OS_LINUX) && defined(SYS_renameat2))
    return result == 0 || fail(error, QStringLiteral("새 프로필 경로가 이미 있거나 원자적 게시에 실패했습니다."));
#endif
}
bool relativeStat(int parent, const QByteArray& name, struct stat* st, bool* missing, QString* error) {
    if (missing) *missing = false;
    if (::fstatat(parent, name.constData(), st, AT_SYMLINK_NOFOLLOW) == 0) return true;
    if (errno == ENOENT && missing) { *missing = true; return true; }
    return fail(error, QStringLiteral("프로필 경로의 상태를 확인할 수 없습니다."));
}
bool sameModification(const struct stat& a, const struct stat& b) {
#ifdef Q_OS_MACOS
    return a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec
        && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec
        && a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec
        && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
    return a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
}
bool stableFile(const struct stat& a, const struct stat& b) {
    return sameInode(a, b) && a.st_uid == b.st_uid && a.st_gid == b.st_gid
        && a.st_mode == b.st_mode && a.st_size == b.st_size && a.st_nlink == b.st_nlink
        && sameModification(a, b);
}
bool hashDescriptor(int descriptor, QByteArray* result, QString* error) {
    if (::lseek(descriptor, 0, SEEK_SET) < 0)
        return fail(error, QStringLiteral("프로필 파일의 읽기 위치를 정할 수 없습니다."));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 65536> bytes{};
    for (;;) {
        const auto count = ::read(descriptor, bytes.data(), bytes.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return fail(error, QStringLiteral("프로필 파일의 해시를 읽지 못했습니다."));
        }
        if (count == 0) break;
        hash.addData(QByteArrayView(bytes.data(), qsizetype(count)));
    }
    *result = hash.result().toHex();
    return true;
}
bool hashFileAt(int directory, const QByteArray& name, struct stat* identity,
                QByteArray* hash, bool lockFile, QString* error, bool allowThirdLink = false) {
    Fd fd(::openat(directory, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat before{}, after{};
    if (!fd || ::fstat(fd.get(), &before) != 0 || !S_ISREG(before.st_mode)
        || before.st_uid != ::geteuid() || (before.st_mode & (S_ISUID | S_ISGID | S_ISVTX))
        || (!lockFile && before.st_nlink != 1)
        || (lockFile && before.st_nlink != 1 && before.st_nlink != 2
            && (!allowThirdLink || before.st_nlink != 3)))
        return fail(error, QStringLiteral("프로필 파일의 링크, 종류 또는 소유자가 안전하지 않습니다."));
    if (!hashDescriptor(fd.get(), hash, error)) return false;
    if (::fstat(fd.get(), &after) != 0 || !stableFile(before, after))
        return fail(error, QStringLiteral("읽는 동안 프로필 파일이 바뀌었습니다."));
    if (!closeChecked(fd, error)) return false;
    *identity = before;
    return true;
}
bool namesAt(int directory, QStringList* names, QString* error) {
    Fd duplicate(::dup(directory));
    if (!duplicate) return fail(error, QStringLiteral("프로필 폴더를 열 수 없습니다."));
    DIR* stream = ::fdopendir(duplicate.get());
    if (!stream) return fail(error, QStringLiteral("프로필 폴더의 목록을 읽을 수 없습니다."));
    duplicate.release();
    // dup shares the directory offset, so reset before every scan of a retained descriptor.
    ::rewinddir(stream);
    bool valid = true;
    errno = 0;
    while (const auto* item = ::readdir(stream)) {
        const QByteArray native(item->d_name);
        if (native == "." || native == "..") { errno = 0; continue; }
        const auto name = QFile::decodeName(native);
        if (QFile::encodeName(name) != native || name.isEmpty() || name.contains('/')) {
            valid = fail(error, QStringLiteral("프로필 파일 이름을 안전하게 해석할 수 없습니다."));
            break;
        }
        names->append(name);
        errno = 0;
    }
    const int readError = errno;
    if (::closedir(stream) != 0)
        valid = fail(error, QStringLiteral("프로필 폴더의 목록을 닫지 못했습니다."));
    if (valid && readError != 0)
        valid = fail(error, QStringLiteral("프로필 폴더의 목록을 읽는 중 오류가 발생했습니다."));
    if (valid) names->sort(Qt::CaseSensitive);
    return valid;
}
Fd relativeDirectory(int root, const QString& path, bool* missing, QString* error) {
    if (missing) *missing = false;
    Fd current(::dup(root));
    if (!current) { fail(error, QStringLiteral("프로필 폴더를 열 수 없습니다.")); return {}; }
    if (path.isEmpty() || path == QStringLiteral(".")) return current;
    for (const auto& part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") {
            fail(error, QStringLiteral("프로필 내부 경로가 올바르지 않습니다."));
            return {};
        }
        Fd next(::openat(current.get(), QFile::encodeName(part).constData(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) {
            if (errno == ENOENT && missing) { *missing = true; return {}; }
            fail(error, QStringLiteral("프로필 내부 경로의 링크 또는 접근 오류를 거부했습니다."));
            return {};
        }
        struct stat st{};
        if (::fstat(next.get(), &st) != 0 || !safeNode(st, true)) {
            fail(error, QStringLiteral("프로필 내부 폴더의 소유자 또는 권한이 안전하지 않습니다."));
            return {};
        }
        current = std::move(next);
    }
    return current;
}
QString parentPart(const QString& relative) {
    const auto slash = relative.lastIndexOf('/');
    return slash < 0 ? QString{} : relative.left(slash);
}
QByteArray leafPart(const QString& relative) {
    const auto slash = relative.lastIndexOf('/');
    return QFile::encodeName(slash < 0 ? relative : relative.mid(slash + 1));
}
bool privateDirectoryFd(int descriptor, struct stat* identity, QString* error) {
    struct stat st{};
    if (::fstat(descriptor, &st) != 0 || !safeNode(st, true) || (st.st_mode & 0777) != 0700)
        return fail(error, QStringLiteral("프로필 임시 폴더의 소유자 또는 권한이 안전하지 않습니다."));
    if (identity) *identity = st;
    return true;
}
bool boundDirectory(const QString& path, int retained, const struct stat& expected,
                    bool requirePrivate, QString* error) {
    auto current = openDirectory(path, nullptr, error);
    struct stat now{}, kept{};
    if (!current || ::fstat(current.get(), &now) != 0 || ::fstat(retained, &kept) != 0
        || !sameInode(now, expected) || !sameInode(kept, expected) || !safeNode(now, true)
        || (requirePrivate && (now.st_mode & 0777) != 0700))
        return fail(error, QStringLiteral("작업 중 프로필 폴더의 경로 또는 정체성이 바뀌었습니다."));
    return closeChecked(current, error);
}
bool writeAtomicJson(int directory, const QByteArray& name, const QJsonObject& object,
                     bool replaceOwnRecord, QByteArray* written, QString* error) {
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    const QByteArray temporary = QByteArray(".keefi-record-") + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    Fd output(::openat(directory, temporary.constData(),
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!output) return fail(error, QStringLiteral("프로필 이전 기록을 만들 수 없습니다."));
    struct stat created{};
    if (::fstat(output.get(), &created) != 0) {
        ::unlinkat(directory, temporary.constData(), 0);
        return fail(error, QStringLiteral("프로필 이전 기록의 정체성을 확인하지 못했습니다."));
    }
    bool success = true;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(output.get(), bytes.constData() + offset, size_t(bytes.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { success = fail(error, QStringLiteral("프로필 이전 기록을 쓰지 못했습니다.")); break; }
        offset += qsizetype(count);
    }
    if (success) success = syncFd(output.get(), error);
    if (!closeChecked(output, error)) success = false;
    if (success) {
        if (replaceOwnRecord) {
            struct stat existing{};
            if (::fstatat(directory, name.constData(), &existing, AT_SYMLINK_NOFOLLOW) != 0
                || !safeNode(existing, false) || (existing.st_mode & 0777) != 0600)
                success = fail(error, QStringLiteral("이전 소유 기록이 바뀌어 갱신하지 않았습니다."));
            else if (::renameat(directory, temporary.constData(), directory, name.constData()) != 0)
                success = fail(error, QStringLiteral("프로필 이전 기록을 원자적으로 갱신하지 못했습니다."));
        } else {
            success = exclusiveRename(directory, temporary, directory, name, error);
        }
    }
    if (!success) {
        struct stat remaining{};
        if (::fstatat(directory, temporary.constData(), &remaining, AT_SYMLINK_NOFOLLOW) == 0
            && sameInode(remaining, created))
            ::unlinkat(directory, temporary.constData(), 0);
        return false;
    }
    if (!syncFd(directory, error)) return false;
    if (written) *written = bytes;
    return true;
}

const QSet<QString>& activeWriters() {
    static const QSet<QString> paths = [] {
        QSet<QString> result(legacy::WriterLocks.cbegin(), legacy::WriterLocks.cend());
        for (const auto& name : {QStringLiteral("KEEFI/KEEFI Dev.ini.lock"), QStringLiteral("KEEFI.ini.lock"),
                                QStringLiteral("KEEFI/KEEFI.ini.lock"),
                                QStringLiteral("credentials.keefi-vault.lock"), QStringLiteral("assets.keefi.lock"),
                                QStringLiteral("assets-current.keefi.lock"), QStringLiteral("swaps.keefi.lock"),
                                QStringLiteral("prices.keefi.lock"), QStringLiteral("imported-records.keefi.lock")})
            result.insert(name);
        return result;
    }();
    return paths;
}
bool temporaryName(const QString& name, bool currentLayout = false) {
    auto prefixes = legacy::TemporaryPrefixes;
    if (currentLayout) prefixes.append({QStringLiteral(".keefi-vault-"), QStringLiteral(".keefi-restore-"),
        QStringLiteral(".keefi-assets-"), QStringLiteral(".keefi-prices-"),
        QStringLiteral(".keefi-record-transfer-")});
    for (const auto& prefix : prefixes) {
        if (!name.startsWith(prefix) || name.size() != prefix.size() + 6) continue;
        const auto suffix = QStringView(name).mid(prefix.size());
        bool ascii = true;
        for (const auto ch : suffix)
            ascii = ascii && ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
                         || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
                         || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')));
        if (ascii) return true;
    }
    return false;
}
QString mappedName(const QString& name) {
    if (name == legacy::AppSettings) return QStringLiteral("KEEFI/KEEFI Dev.ini");
    if (name == legacy::OrganizationSettings) return QStringLiteral("KEEFI.ini");
    return name;
}
struct Entry {
    struct stat identity{};
    QByteArray hash;
    bool directory() const { return S_ISDIR(identity.st_mode); }
};
using Manifest = QMap<QString, Entry>;

struct ScanRules {
    bool source = false;
    bool allowMarker = false;
    const struct stat* ownLock = nullptr;
    const QByteArray* ownLockHash = nullptr;
    bool allowLegacySourceMarker = false;
    QString completionMarker = MarkerName;
    bool currentLayout = false;
    bool allowActiveSourceMarker = false;
    bool keepSourceTemporaries = false;
    bool allowThirdLockLink = false;
};
bool scanDirectory(int directory, const QString& prefix, const ScanRules& rules,
                   Manifest* result, QString* error) {
    QStringList names;
    if (!namesAt(directory, &names, error)) return false;
    for (const auto& name : names) {
        const auto relative = prefix.isEmpty() ? name : prefix + '/' + name;
        const auto native = QFile::encodeName(name);
        struct stat listed{};
        if (::fstatat(directory, native.constData(), &listed, AT_SYMLINK_NOFOLLOW) != 0)
            return fail(error, QStringLiteral("검사 중 프로필 파일 목록이 바뀌었습니다."));
        if (rules.source && activeWriters().contains(relative))
            return fail(error, QStringLiteral("기존 프로필을 쓰는 작업이 있어 복사하지 않았습니다."));
        if (relative == QString::fromLatin1(LegacyLockName) && rules.source) {
            struct stat lockIdentity{};
            QByteArray hash;
            if (!rules.ownLock || !rules.ownLockHash
                || !hashFileAt(directory, native, &lockIdentity, &hash, true, error, rules.allowThirdLockLink)
                || !sameInode(lockIdentity, *rules.ownLock)
                || (lockIdentity.st_nlink != 2 && (!rules.allowThirdLockLink || lockIdentity.st_nlink != 3))
                || hash != *rules.ownLockHash)
                return fail(error, QStringLiteral("기존 프로필의 실행 잠금이 바뀌었습니다."));
            continue;
        }
        if (relative == MarkerName && rules.source && !rules.allowLegacySourceMarker)
            return fail(error, QStringLiteral("원본에 예약된 프로필 이전 표식이 있어 복사하지 않았습니다."));
        if (relative == profile_upgrade_detail::ActiveMarkerName && rules.source && rules.currentLayout
            && !rules.allowActiveSourceMarker)
            return fail(error, QStringLiteral("원본에 새 프로필 완료 표식이 있어 전환하지 않았습니다."));
        if (!rules.source && relative == rules.completionMarker && rules.allowMarker) {
            if (!safeNode(listed, false))
                return fail(error, QStringLiteral("완료 표식의 링크 또는 소유자가 바뀌었습니다."));
            continue;
        }
        Entry entry;
        if (S_ISDIR(listed.st_mode)) {
            if (!safeNode(listed, true))
                return fail(error, QStringLiteral("프로필 내부 폴더의 소유자 또는 권한이 안전하지 않습니다."));
            Fd child(::openat(directory, native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!child || ::fstat(child.get(), &entry.identity) != 0
                || !sameInode(listed, entry.identity) || !safeNode(entry.identity, true))
                return fail(error, QStringLiteral("프로필 폴더의 정체성이 바뀌었습니다."));
            result->insert(relative, entry);
            if (!scanDirectory(child.get(), relative, rules, result, error)) return false;
            struct stat after{};
            if (::fstat(child.get(), &after) != 0 || !sameInode(after, entry.identity)
                || after.st_mode != entry.identity.st_mode || after.st_uid != entry.identity.st_uid
                || after.st_gid != entry.identity.st_gid)
                return fail(error, QStringLiteral("검사 중 프로필 폴더의 권한이 바뀌었습니다."));
            if (!closeChecked(child, error)) return false;
        } else {
            if (!safeNode(listed, false))
                return fail(error, QStringLiteral("프로필의 심볼릭 링크, 하드 링크 또는 특수 파일을 거부했습니다."));
            if (rules.source && !rules.keepSourceTemporaries && temporaryName(name, rules.currentLayout)) continue;
            if (!hashFileAt(directory, native, &entry.identity, &entry.hash, false, error)
                || !stableFile(listed, entry.identity))
                return fail(error, QStringLiteral("검사 중 프로필 파일이 바뀌었거나 읽을 수 없습니다."));
            result->insert(relative, entry);
        }
    }
    return true;
}
bool scanRoot(int directory, const ScanRules& rules, Manifest* result, QString* error) {
    result->clear();
    Entry root;
    if (!privateDirectoryFd(directory, &root.identity, error)) return false;
    result->insert(QStringLiteral("."), root);
    return scanDirectory(directory, {}, rules, result, error);
}
bool sameSource(const Manifest& original, const Manifest& current, QString* error) {
    if (original.keys() != current.keys())
        return fail(error, QStringLiteral("복사 중 원본 파일 목록이 바뀌었습니다."));
    for (auto it = original.cbegin(); it != original.cend(); ++it) {
        const auto& a = it.value();
        const auto& b = current.value(it.key());
        if (a.directory() != b.directory() || !sameInode(a.identity, b.identity)
            || a.identity.st_uid != b.identity.st_uid || a.identity.st_gid != b.identity.st_gid
            || a.identity.st_mode != b.identity.st_mode || a.hash != b.hash
            || (!a.directory() && !stableFile(a.identity, b.identity)))
            return fail(error, QStringLiteral("복사 중 원본 파일의 내용, 권한 또는 정체성이 바뀌었습니다."));
    }
    return true;
}
bool expectedManifest(const Manifest& source, Manifest* target, QString* error) {
    target->clear();
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        const auto name = mappedName(it.key());
        if (target->contains(name))
            return fail(error, QStringLiteral("설정 파일 이름을 옮기면 다른 파일과 충돌합니다."));
        target->insert(name, it.value());
    }
    if (source.contains(legacy::AppSettings)) {
        if (target->contains(QStringLiteral("KEEFI"))) {
            if (!target->value(QStringLiteral("KEEFI")).directory())
                return fail(error, QStringLiteral("새 설정 폴더 이름이 다른 파일과 충돌합니다."));
        } else {
            if (!source.contains(legacy::SettingsDirectory) || !source.value(legacy::SettingsDirectory).directory())
                return fail(error, QStringLiteral("기존 설정 폴더를 확인할 수 없습니다."));
            target->insert(QStringLiteral("KEEFI"), source.value(legacy::SettingsDirectory));
        }
    }
    for (auto it = target->cbegin(); it != target->cend(); ++it) {
        auto parent = parentPart(it.key());
        if (!parent.isEmpty() && (!target->contains(parent) || !target->value(parent).directory()))
            return fail(error, QStringLiteral("설정 파일의 대상 경로가 다른 파일과 충돌합니다."));
    }
    return true;
}
QString upgradedName(const QString& name) {
    if (name == MarkerName) return {}; // Source proof is retained externally, never in the active tree.
    if (name == legacy::ProductionAppSettings) return QStringLiteral("KEEFI/KEEFI.ini");
    const auto mapped = mappedName(name);
    if (mapped != name) return mapped;
    if (name == legacy::SettingsDirectory) return QStringLiteral("KEEFI");
    if (name.startsWith(legacy::SettingsDirectory + '/'))
        return QStringLiteral("KEEFI") + name.mid(legacy::SettingsDirectory.size());
    return name;
}
bool upgradeManifest(const Manifest& source, Manifest* target, QString* error) {
    target->clear();
    // Prefer the existing current settings directory's metadata when a prior
    // name-only copy left an empty legacy directory beside it.
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        const auto name = upgradedName(it.key());
        if (name.isEmpty()) continue;
        if (target->contains(name)) {
            const bool settingsDirectory = name == QStringLiteral("KEEFI")
                && it.value().directory() && target->value(name).directory()
                && (it.key() == legacy::SettingsDirectory || it.key() == QStringLiteral("KEEFI"));
            if (!settingsDirectory)
                return fail(error, QStringLiteral("설정 이름 전환이 다른 파일과 충돌합니다."));
            if (it.key() == QStringLiteral("KEEFI")) target->insert(name, it.value());
            continue;
        }
        target->insert(name, it.value());
    }
    for (auto it = target->cbegin(); it != target->cend(); ++it) {
        const auto parent = parentPart(it.key());
        if (!parent.isEmpty() && (!target->contains(parent) || !target->value(parent).directory()))
            return fail(error, QStringLiteral("새 설정의 대상 폴더를 확인할 수 없습니다."));
    }
    return true;
}
bool sameCopy(const Manifest& expected, const Manifest& current, QString* error) {
    if (expected.keys() != current.keys())
        return fail(error, QStringLiteral("복사본의 파일 목록이 원본과 다릅니다."));
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const auto& a = it.value();
        const auto& b = current.value(it.key());
        if (a.directory() != b.directory() || a.identity.st_uid != b.identity.st_uid
            || a.identity.st_gid != b.identity.st_gid
            || (a.identity.st_mode & 07777) != (b.identity.st_mode & 07777)
            || (!a.directory() && (a.identity.st_size != b.identity.st_size || a.hash != b.hash)))
            return fail(error, QStringLiteral("복사본의 바이트, 소유자 또는 권한이 원본과 다릅니다."));
    }
    return true;
}
bool manifestEvidence(const Manifest& source, qint64* fileCount, qint64* totalBytes,
                      QString* hash, QString* error) {
    QJsonArray entries;
    *fileCount = 0;
    *totalBytes = 0;
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        const auto& entry = it.value();
        if (!entry.directory()) {
            if (entry.identity.st_size < 0 || entry.identity.st_size > std::numeric_limits<qint64>::max() - *totalBytes
                || *fileCount == std::numeric_limits<qint64>::max())
                return fail(error, QStringLiteral("프로필 파일 크기의 합이 지원 범위를 넘었습니다."));
            ++*fileCount;
            *totalBytes += qint64(entry.identity.st_size);
        }
        entries.append(QJsonObject{
            {"path", it.key()}, {"kind", entry.directory() ? "directory" : "file"},
            {"size", entry.directory() ? qint64(0) : qint64(entry.identity.st_size)},
            {"sha256", QString::fromLatin1(entry.hash)}, {"uid", qint64(entry.identity.st_uid)},
            {"gid", qint64(entry.identity.st_gid)}, {"mode", qint64(entry.identity.st_mode & 07777)}});
    }
    *hash = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(entries).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return true;
}

bool removeTreeContents(int directory, QString* error) {
    QStringList names;
    if (!namesAt(directory, &names, error)) return false;
    for (const auto& name : names) {
        const auto native = QFile::encodeName(name);
        struct stat entry{};
        if (::fstatat(directory, native.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0
            || entry.st_uid != ::geteuid())
            return fail(error, QStringLiteral("소유권이 확인되지 않는 임시 파일을 보존했습니다."));
        if (S_ISDIR(entry.st_mode)) {
            Fd child(::openat(directory, native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!child)
                return fail(error, QStringLiteral("소유한 임시 폴더를 정리하지 못했습니다."));
            struct stat kept{};
            if (::fstat(child.get(), &kept) != 0 || !sameInode(kept, entry)
                || ::fchmod(child.get(), 0700) != 0 || !removeTreeContents(child.get(), error))
                return fail(error, QStringLiteral("소유한 임시 폴더의 안전한 정리를 마치지 못했습니다."));
            if (!closeChecked(child, error)) return false;
            struct stat last{};
            if (::fstatat(directory, native.constData(), &last, AT_SYMLINK_NOFOLLOW) != 0
                || !sameInode(last, entry) || ::unlinkat(directory, native.constData(), AT_REMOVEDIR) != 0)
                return fail(error, QStringLiteral("임시 폴더의 정체성이 바뀌어 보존했습니다."));
        } else {
            // unlinkat removes the owned entry itself; it never follows a substituted link.
            if (::unlinkat(directory, native.constData(), 0) != 0)
                return fail(error, QStringLiteral("소유한 임시 파일을 정리하지 못했습니다."));
        }
    }
    return true;
}
bool knownControlNames(int control, QStringList* names, QString* error) {
    if (!namesAt(control, names, error)) return false;
    static const QRegularExpression recordPattern(
        QStringLiteral("^\\.keefi-record-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    for (const auto& name : *names) {
        if (name == QString::fromLatin1(OwnerName) || name == QString::fromLatin1(SourceLockName)
            || name == QString::fromLatin1(StageName)) continue;
        if (!recordPattern.match(name).hasMatch())
            return fail(error, QStringLiteral("알 수 없는 제어 파일이 있어 이전 작업 폴더를 보존했습니다."));
        struct stat record{};
        if (::fstatat(control, QFile::encodeName(name).constData(), &record, AT_SYMLINK_NOFOLLOW) != 0
            || !safeNode(record, false) || (record.st_mode & 0777) != 0600 || record.st_size > 65536)
            return fail(error, QStringLiteral("임시 소유 기록의 정체성이 불명확하여 보존했습니다."));
    }
    return true;
}
QJsonObject directoryProof(const struct stat& identity) {
    return {{"device", QString::number(qulonglong(identity.st_dev))},
            {"inode", QString::number(qulonglong(identity.st_ino))},
            {"uid", qint64(identity.st_uid)}, {"gid", qint64(identity.st_gid)},
            {"mode", qint64(identity.st_mode & 07777)}};
}
bool validDirectoryProof(const QJsonObject& proof, QString* error) {
    bool deviceOk = false, inodeOk = false;
    const auto device = proof.value("device").toString().toULongLong(&deviceOk);
    const auto inode = proof.value("inode").toString().toULongLong(&inodeOk);
    const auto gid = proof.value("gid").toInteger(-1);
    if (!deviceOk || !inodeOk || inode == 0
        || QString::number(device) != proof.value("device").toString()
        || QString::number(inode) != proof.value("inode").toString()
        || proof.value("uid") != QJsonValue(qint64(::geteuid()))
        || gid < 0 || quint64(gid) > std::numeric_limits<gid_t>::max()
        || proof.value("gid") != QJsonValue(gid) || proof.value("mode") != QJsonValue(qint64(0700)))
        return fail(error, QStringLiteral("임시 프로필 폴더의 소유 증거가 올바르지 않습니다."));
    return true;
}
bool stageProofMatches(int control, const QJsonObject& proof, bool allowMissing, QString* error) {
    struct stat stage{};
    bool missing = false;
    if (!relativeStat(control, StageName, &stage, &missing, error)) return false;
    if (missing) return allowMissing || fail(error, QStringLiteral("소유한 임시 프로필 폴더가 사라졌습니다."));
    if (!validDirectoryProof(proof, error) || !safeNode(stage, true) || directoryProof(stage) != proof)
        return fail(error, QStringLiteral("임시 프로필 폴더가 다른 폴더로 바뀌어 보존했습니다."));
    return true;
}
bool ownerProofMatches(int control, const struct stat* expected, const QByteArray* expectedHash,
                       QString* error) {
    struct stat current{};
    bool missing = false;
    if (!relativeStat(control, OwnerName, &current, &missing, error)) return false;
    if (!expected || !expectedHash) {
        return missing || fail(error, QStringLiteral("소유 증거가 없는 이전 기록을 보존했습니다."));
    }
    if (missing || !sameInode(current, *expected) || !safeNode(current, false)
        || current.st_uid != expected->st_uid || current.st_gid != expected->st_gid
        || current.st_mode != expected->st_mode)
        return fail(error, QStringLiteral("이전 작업의 소유 기록이 다른 파일로 바뀌어 보존했습니다."));
    QByteArray hash;
    struct stat readBack{};
    if (!hashFileAt(control, OwnerName, &readBack, &hash, false, error)
        || !sameInode(readBack, *expected) || readBack.st_uid != expected->st_uid
        || readBack.st_gid != expected->st_gid || readBack.st_mode != expected->st_mode
        || hash != *expectedHash)
        return fail(error, QStringLiteral("이전 작업의 소유 기록 내용이 바뀌어 보존했습니다."));
    return true;
}
struct CleanupProof {
    QJsonObject stage;
    const struct stat* owner = nullptr;
    const QByteArray* ownerHash = nullptr;
    const struct stat* lock = nullptr;
    const QByteArray* lockHash = nullptr;
};
bool controlLockProofMatches(int control, const CleanupProof& proof, bool allowMissing, QString* error) {
    struct stat listed{};
    bool missing = false;
    if (!relativeStat(control, SourceLockName, &listed, &missing, error)) return false;
    if (missing) return allowMissing || fail(error, QStringLiteral("자기 제어 잠금이 사라졌습니다."));
    struct stat identity{};
    QByteArray hash;
    if (!proof.lock || !proof.lockHash || !sameInode(listed, *proof.lock)
        || !hashFileAt(control, SourceLockName, &identity, &hash, true, error)
        || !sameInode(identity, *proof.lock) || identity.st_nlink != 1
        || identity.st_uid != proof.lock->st_uid || identity.st_gid != proof.lock->st_gid
        || identity.st_mode != proof.lock->st_mode || hash != *proof.lockHash)
        return fail(error, QStringLiteral("다른 제어 잠금을 발견해 보존했습니다."));
    return true;
}
bool removeControl(int parent, const QByteArray& name, int control,
                   const struct stat& expected, const CleanupProof& proof, QString* error) {
    struct stat current{};
    if (::fstatat(parent, name.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0
        || !sameInode(current, expected) || !safeNode(current, true) || (current.st_mode & 0777) != 0700)
        return fail(error, QStringLiteral("이전 작업 폴더가 바뀌어 자동 정리를 중단했습니다."));
    if (!ownerProofMatches(control, proof.owner, proof.ownerHash, error)
        || !stageProofMatches(control, proof.stage, true, error)
        || !controlLockProofMatches(control, proof, true, error)) return false;
    QStringList names;
    if (!knownControlNames(control, &names, error)) return false;

    // Retire only this fully verified control before deleting its owner record.
    // A crash during deletion now leaves a private random sibling, never an
    // ownerless fixed transaction directory that would block the next attempt.
    const QByteArray retired = QByteArray(".keefi-dev-profile-copy-cleanup-")
        + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    if (::fstatat(parent, name.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0
        || !sameInode(current, expected)
        || !exclusiveRename(parent, name, parent, retired, error)) return false;
    if (!syncFd(parent, error)) return false;
    names.removeAll(QString::fromLatin1(OwnerName));
    names.removeAll(QString::fromLatin1(SourceLockName));
    if (names.removeAll(QString::fromLatin1(StageName))) names.prepend(QString::fromLatin1(StageName));
    for (const auto& childName : names) {
        const auto native = QFile::encodeName(childName);
        struct stat childStat{};
        if (::fstatat(control, native.constData(), &childStat, AT_SYMLINK_NOFOLLOW) != 0
            || childStat.st_uid != ::geteuid())
            return fail(error, QStringLiteral("임시 자료의 소유권을 확인하지 못해 보존했습니다."));
        if (childName == QString::fromLatin1(StageName)
            && (!safeNode(childStat, true) || directoryProof(childStat) != proof.stage))
            return fail(error, QStringLiteral("정리 직전에 임시 프로필 폴더가 바뀌어 보존했습니다."));
        if (S_ISDIR(childStat.st_mode)) {
            Fd child(::openat(control, native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat retained{}, last{};
            if (!child || ::fstat(child.get(), &retained) != 0 || !sameInode(retained, childStat)
                || retained.st_uid != ::geteuid() || ::fchmod(child.get(), 0700) != 0
                || !removeTreeContents(child.get(), error))
                return fail(error, QStringLiteral("복사한 임시 자료를 정리하지 못했습니다."));
            if (::fstatat(control, native.constData(), &last, AT_SYMLINK_NOFOLLOW) != 0
                || !sameInode(last, childStat))
                return fail(error, QStringLiteral("임시 자료 폴더가 바뀌어 보존했습니다."));
            if (!closeChecked(child, error)) return false;
            if (::unlinkat(control, native.constData(), AT_REMOVEDIR) != 0)
                return fail(error, QStringLiteral("임시 자료 폴더를 지우지 못했습니다."));
        } else if (::unlinkat(control, native.constData(), 0) != 0) {
            return fail(error, QStringLiteral("임시 제어 파일을 정리하지 못했습니다."));
        }
    }
    if (!controlLockProofMatches(control, proof, true, error)
        || !ownerProofMatches(control, proof.owner, proof.ownerHash, error)) return false;
    for (const auto* metadata : {SourceLockName, OwnerName}) {
        struct stat st{};
        if (::fstatat(control, metadata, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            return fail(error, QStringLiteral("제어 파일의 상태를 확인하지 못했습니다."));
        }
        if (!safeNode(st, false) || ::unlinkat(control, metadata, 0) != 0)
            return fail(error, QStringLiteral("소유권이 확인되지 않는 제어 파일을 보존했습니다."));
    }
    if (::fstatat(parent, retired.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0
        || !sameInode(current, expected) || ::unlinkat(parent, retired.constData(), AT_REMOVEDIR) != 0)
        return fail(error, QStringLiteral("비워진 이전 작업 폴더를 정리하지 못했습니다."));
    return syncFd(parent, error);
}
bool ownerCommon(const QJsonObject& owner, const DevProfilePaths& paths, QString* error) {
    const auto pid = owner.value("pid").toInteger(-1);
    const auto phase = owner.value("phase").toString();
    if (owner.value("format").toString() != OwnerFormat
        || (phase != QStringLiteral("initial") && phase != QStringLiteral("locked"))
        || owner.value("source").toString() != paths.legacyDirectory
        || owner.value("target").toString() != paths.currentDirectory
        || owner.value("version").toString().isEmpty() || owner.value("nonce").toString().isEmpty()
        || pid <= 0 || pid > std::numeric_limits<pid_t>::max() || owner.value("pid") != QJsonValue(pid)
        || owner.value("host").toString().isEmpty() || owner.value("app").toString().isEmpty())
        return fail(error, QStringLiteral("이전 작업의 소유 기록 또는 경로 결속을 확인할 수 없습니다."));
    if (!validDirectoryProof(owner.value("stage").toObject(), error)) return false;
    if (phase == QStringLiteral("locked")) {
        const auto lock = owner.value("lock").toObject();
        bool deviceOk = false, inodeOk = false;
        const auto device = lock.value("device").toString().toULongLong(&deviceOk);
        const auto inode = lock.value("inode").toString().toULongLong(&inodeOk);
        if (!deviceOk || !inodeOk || inode == 0
            || QString::number(device) != lock.value("device").toString()
            || QString::number(inode) != lock.value("inode").toString()
            || lock.value("uid") != QJsonValue(qint64(::geteuid()))
            || !validHash(lock.value("sha256").toString())
            || lock.value("pid").toInteger(-1) != pid
            || lock.value("host").toString() != owner.value("host").toString()
            || lock.value("app").toString() != owner.value("app").toString())
            return fail(error, QStringLiteral("잠금 소유 기록의 형식이 올바르지 않습니다."));
    }
    return true;
}
bool confirmedDead(const QJsonObject& owner, QString* error) {
    if (owner.value("host").toString() != QSysInfo::machineHostName())
        return fail(error, QStringLiteral("다른 컴퓨터의 이전 작업 기록은 자동 정리하지 않습니다."));
    const auto pid = pid_t(owner.value("pid").toInteger());
    if (::kill(pid, 0) == 0 || errno != ESRCH)
        return fail(error, QStringLiteral("프로필 이전 작업이 실행 중이거나 종료를 확인할 수 없습니다."));
    return true;
}
bool identityMatches(const QJsonObject& metadata, const struct stat& identity, const QByteArray& hash,
                     QString* error) {
    const QString device = QString::number(qulonglong(identity.st_dev));
    const QString inode = QString::number(qulonglong(identity.st_ino));
    if (metadata.value("device").toString() != device || metadata.value("inode").toString() != inode
        || metadata.value("uid").toInteger(-1) != qint64(identity.st_uid)
        || metadata.value("sha256").toString() != QString::fromLatin1(hash)
        || !validHash(metadata.value("sha256").toString()))
        return fail(error, QStringLiteral("중단된 복사 작업의 잠금 정체성이 바뀌어 보존했습니다."));
    return true;
}
bool ownOldLock(int source, const struct stat& expected, const QByteArray& hash, bool allowMissing,
                bool* exists, QString* error) {
    struct stat listed{};
    bool missing = false;
    if (!relativeStat(source, LegacyLockName, &listed, &missing, error)) return false;
    if (exists) *exists = !missing;
    if (missing) return allowMissing || fail(error, QStringLiteral("원본의 복사 잠금이 사라졌습니다."));
    struct stat found{};
    QByteArray actual;
    if (!sameInode(listed, expected) || !hashFileAt(source, LegacyLockName, &found, &actual, true, error)
        || !sameInode(found, expected) || found.st_nlink != 2 || actual != hash)
        return fail(error, QStringLiteral("다른 실행 잠금을 발견해 그대로 보존했습니다."));
    return true;
}
bool recoverControl(const DevProfilePaths& paths, int parent, int source, QString* error) {
    const auto name = QFile::encodeName(QFileInfo(paths.transactionDirectory).fileName());
    struct stat listed{};
    bool missing = false;
    if (!relativeStat(parent, name, &listed, &missing, error)) return false;
    if (missing) return true;
    Fd control(::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat controlIdentity{};
    if (!control || !privateDirectoryFd(control.get(), &controlIdentity, error)
        || !sameInode(listed, controlIdentity))
        return fail(error, QStringLiteral("알 수 없는 이전 작업 폴더를 보존했습니다."));
    Fd ownerFd(::openat(control.get(), OwnerName, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat ownerIdentity{};
    if (!ownerFd || ::fstat(ownerFd.get(), &ownerIdentity) != 0 || !safeNode(ownerIdentity, false)
        || (ownerIdentity.st_mode & 0777) != 0600
        || ::flock(ownerFd.get(), LOCK_EX | LOCK_NB) != 0)
        return fail(error, QStringLiteral("이전 작업 소유 기록을 잠그거나 확인할 수 없습니다."));
    QJsonObject owner;
    QByteArray ownerBytes;
    if (!readJsonAt(control.get(), OwnerName, &owner, error, &ownerBytes)
        || !ownerCommon(owner, paths, error) || !confirmedDead(owner, error)) return false;
    struct stat stillOwner{};
    if (::fstatat(control.get(), OwnerName, &stillOwner, AT_SYMLINK_NOFOLLOW) != 0
        || !sameInode(stillOwner, ownerIdentity))
        return fail(error, QStringLiteral("복구 중 소유 기록이 바뀌었습니다."));
    QStringList controlNames;
    if (!knownControlNames(control.get(), &controlNames, error)) return false;

    struct stat lockIdentity{};
    bool lockMissing = false;
    if (!relativeStat(control.get(), SourceLockName, &lockIdentity, &lockMissing, error)) return false;
    struct stat oldListed{};
    bool oldMissing = true;
    if (source >= 0 && !relativeStat(source, LegacyLockName, &oldListed, &oldMissing, error)) return false;
    Fd nativeLock;
    QByteArray lockHash;
    if (!lockMissing) {
        nativeLock.reset(::openat(control.get(), SourceLockName, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        struct stat held{};
        if (!nativeLock || ::fstat(nativeLock.get(), &held) != 0 || !sameInode(held, lockIdentity)
            || !S_ISREG(held.st_mode) || held.st_uid != ::geteuid()
            || (held.st_mode & (S_ISUID | S_ISGID | S_ISVTX))
            || (held.st_nlink != 1 && held.st_nlink != 2)
            || ::flock(nativeLock.get(), LOCK_EX | LOCK_NB) != 0)
            return fail(error, QStringLiteral("중단된 잠금을 안전하게 복구할 수 없습니다."));
        if (!hashDescriptor(nativeLock.get(), &lockHash, error)) return false;
        QLockFile information(QDir(paths.transactionDirectory).filePath(QString::fromLatin1(SourceLockName)));
        qint64 pid = 0;
        QString host, app;
        if (!information.getLockInfo(&pid, &host, &app) || pid != owner.value("pid").toInteger()
            || host != owner.value("host").toString() || app.isEmpty())
            return fail(error, QStringLiteral("Qt 실행 잠금과 이전 소유 기록이 일치하지 않습니다."));
        if (owner.value("phase").toString() == QStringLiteral("locked")) {
            const auto metadata = owner.value("lock").toObject();
            if (!identityMatches(metadata, held, lockHash, error)
                || metadata.value("pid").toInteger(-1) != pid
                || metadata.value("host").toString() != host || metadata.value("app").toString() != app
                || owner.value("app").toString() != app)
                return fail(error, QStringLiteral("기록된 실행 잠금 소유자를 확인할 수 없습니다."));
        } else if (!oldMissing || held.st_nlink != 1) {
            return fail(error, QStringLiteral("초기 단계 기록과 원본 잠금 상태가 일치하지 않습니다."));
        }
        lockIdentity = held;
    }
    if (!oldMissing) {
        if (lockMissing || owner.value("phase").toString() != QStringLiteral("locked")
            || !ownOldLock(source, lockIdentity, lockHash, false, nullptr, error))
            return fail(error, QStringLiteral("기존 잠금이 다른 파일이므로 복구하지 않았습니다."));
    } else if (!lockMissing && lockIdentity.st_nlink != 1) {
        return fail(error, QStringLiteral("잠금 파일의 다른 링크를 확인할 수 없어 보존했습니다."));
    }
    const QByteArray ownerHash = QCryptographicHash::hash(ownerBytes, QCryptographicHash::Sha256).toHex();
    const CleanupProof proof{owner.value("stage").toObject(), &ownerIdentity, &ownerHash,
                             lockMissing ? nullptr : &lockIdentity, lockMissing ? nullptr : &lockHash};
    if (!ownerProofMatches(control.get(), proof.owner, proof.ownerHash, error)
        || !stageProofMatches(control.get(), proof.stage, true, error)) return false;
    if (owner.value("phase").toString() == QStringLiteral("initial")
        && controlNames.contains(QString::fromLatin1(StageName))) {
        Fd initialStage(::openat(control.get(), StageName, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        QStringList entries;
        if (!initialStage || !namesAt(initialStage.get(), &entries, error) || !entries.isEmpty())
            return fail(error, QStringLiteral("초기 단계의 빈 임시 폴더에 알 수 없는 자료가 있어 보존했습니다."));
        if (!closeChecked(initialStage, error)) return false;
    }
    // Every refusal above precedes any mutation. In particular, a replacement old lock
    // leaves both the old profile and the entire control directory byte-identical.
    if (!oldMissing) {
        if (!ownOldLock(source, lockIdentity, lockHash, false, nullptr, error)) return false;
        if (::unlinkat(source, LegacyLockName, 0) != 0 || !syncFd(source, error))
            return fail(error, QStringLiteral("확인한 자기 실행 잠금을 정리하지 못했습니다."));
        struct stat oneLink{};
        if (::fstat(nativeLock.get(), &oneLink) != 0 || oneLink.st_nlink != 1)
            return fail(error, QStringLiteral("자기 잠금 정리 후 링크 수를 확인하지 못했습니다."));
    }
    // Keep nativeLock alive through the unlink: a legacy QLockFile may otherwise
    // decide that the dead owner's old pathname is a stale lock and remove it.
    if (!removeControl(parent, name, control.get(), controlIdentity, proof, error)) return false;
    return closeChecked(nativeLock, error) && closeChecked(ownerFd, error) && closeChecked(control, error);
}

struct SessionState;
void cleanupSession(SessionState& state) noexcept;
struct SessionState {
    DevProfilePaths paths;
    QString version;
    QString parentPath;
    QString controlPath;
    QString stagePath;
    QByteArray controlName;
    QString nonce;
    QJsonObject owner;
    Fd source;
    Fd parent;
    Fd control;
    Fd stage;
    struct stat sourceIdentity{}, parentIdentity{}, controlIdentity{}, stageIdentity{}, lockIdentity{}, ownerIdentity{};
    QByteArray lockHash;
    QByteArray ownerHash;
    bool ownerProofValid = false;
    std::unique_ptr<QLockFile> lock;
    Manifest original;
    Manifest expected;
    QByteArray markerBytes;
    bool upgrade = false;
    bool convertedSnapshot = false;
    bool restoring = false;
    QString completionMarker = MarkerName;
    bool ownsControl = false;
    bool ownOldLink = false;
    bool copied = false;
    bool verified = false;
    bool committed = false;
    bool failed = false;
    QString warning;
    ~SessionState() { cleanupSession(*this); }
};
void addWarning(SessionState& state, const QString& text) {
    if (text.isEmpty()) return;
    if (!state.warning.isEmpty()) state.warning += '\n';
    state.warning += text;
}
CleanupProof sessionProof(const SessionState& state) {
    return {state.stageIdentity.st_ino ? directoryProof(state.stageIdentity) : QJsonObject{},
            state.ownerProofValid ? &state.ownerIdentity : nullptr,
            state.ownerProofValid ? &state.ownerHash : nullptr,
            state.lockIdentity.st_ino ? &state.lockIdentity : nullptr,
            state.lockIdentity.st_ino ? &state.lockHash : nullptr};
}
bool writeOwnerRecord(SessionState& state, bool replace, QString* error) {
    QByteArray bytes;
    if (!writeAtomicJson(state.control.get(), OwnerName, state.owner, replace, &bytes, error)) return false;
    struct stat identity{};
    QByteArray hash;
    if (!hashFileAt(state.control.get(), OwnerName, &identity, &hash, false, error)
        || (identity.st_mode & 0777) != 0600
        || hash != QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
        return fail(error, QStringLiteral("새 소유 기록의 정체성과 바이트를 검증하지 못했습니다."));
    state.ownerIdentity = identity;
    state.ownerHash = hash;
    state.ownerProofValid = true;
    return true;
}
bool releaseQtLock(SessionState& state, QString* error) {
    if (!state.lock) return true;
    // QLockFile::unlock uses its stored absolute pathname. A descriptor bound to
    // the original parent alone is insufficient after that parent was renamed.
    if (!boundDirectory(state.controlPath, state.control.get(), state.controlIdentity, true, error)
        || !controlLockProofMatches(state.control.get(), sessionProof(state), false, error)) {
        // No public QLockFile API can change the stored name or release only its
        // native descriptor. Retain this exceptional lock handle until process
        // exit instead of allowing its destructor to unlink a foreign pathname.
        state.lock.release();
        return false;
    }
    state.lock->unlock();
    state.lock.reset();
    return true;
}
void cleanupSession(SessionState& state) noexcept {
    if (!state.ownsControl) return;
    QString error;
    struct stat named{}, retained{};
    const bool controlAtOwnedName = state.parent && state.control
        && ::fstat(state.control.get(), &retained) == 0 && sameInode(retained, state.controlIdentity)
        && ::fstatat(state.parent.get(), state.controlName.constData(), &named, AT_SYMLINK_NOFOLLOW) == 0
        && sameInode(named, state.controlIdentity);
    const auto proof = sessionProof(state);
    bool mayRemoveControl = controlAtOwnedName;
    if (mayRemoveControl) {
        mayRemoveControl = ownerProofMatches(state.control.get(), proof.owner, proof.ownerHash, &error)
            && stageProofMatches(state.control.get(), proof.stage, true, &error);
    } else {
        error = QStringLiteral("임시 작업 경로가 바뀌어 다른 파일을 보존했습니다.");
    }
    if (!mayRemoveControl) addWarning(state, error);

    // The retained source descriptor and captured lock inode let us remove only
    // our old hardlink even when a control/owner/stage pathname was replaced.
    if (state.ownOldLink) {
        bool exists = false;
        if (!ownOldLock(state.source.get(), state.lockIdentity, state.lockHash, true, &exists, &error)) {
            addWarning(state, error);
            mayRemoveControl = false;
        } else if (exists && ::unlinkat(state.source.get(), LegacyLockName, 0) != 0) {
            addWarning(state, QStringLiteral("자기 실행 잠금을 정리하지 못해 복구 기록을 보존했습니다."));
            if (state.lock) state.lock.release();
            state.ownsControl = false;
            return;
        } else {
            state.ownOldLink = false;
            if (!syncFd(state.source.get(), &error)) addWarning(state, error);
        }
    }
    error.clear();
    if (!releaseQtLock(state, &error)) {
        addWarning(state, error);
        mayRemoveControl = false;
    }
    if (mayRemoveControl) {
        if (!removeControl(state.parent.get(), state.controlName, state.control.get(),
                           state.controlIdentity, proof, &error))
            addWarning(state, error);
    }
    state.ownsControl = false;
}
bool boundSession(SessionState& state, bool needStage, QString* error) {
    if (!boundDirectory(state.paths.legacyDirectory, state.source.get(), state.sourceIdentity, true, error)
        || !boundDirectory(state.parentPath, state.parent.get(), state.parentIdentity, false, error)
        || !boundDirectory(state.controlPath, state.control.get(), state.controlIdentity, true, error))
        return false;
    const auto proof = sessionProof(state);
    if (!ownerProofMatches(state.control.get(), proof.owner, proof.ownerHash, error)) return false;
    if (needStage && (!boundDirectory(state.stagePath, state.stage.get(), state.stageIdentity, true, error)
                      || !stageProofMatches(state.control.get(), proof.stage, false, error)))
        return false;
    if (!state.ownOldLink || !ownOldLock(state.source.get(), state.lockIdentity, state.lockHash, false, nullptr, error))
        return false;
    return true;
}
bool sourceUnchanged(SessionState& state, QString* error) {
    Manifest current;
    const ScanRules rules{true, false, &state.lockIdentity, &state.lockHash,
                          state.upgrade, state.completionMarker, state.upgrade, state.restoring, state.restoring};
    return scanRoot(state.source.get(), rules, &current, error) && sameSource(state.original, current, error);
}
bool stageMatches(SessionState& state, bool allowMarker, QString* error) {
    Manifest current;
    return scanRoot(state.stage.get(), ScanRules{false, allowMarker, nullptr, nullptr,
                    false, state.completionMarker, state.upgrade}, &current, error)
        && (state.convertedSnapshot ? sameSource(state.expected, current, error)
                                    : sameCopy(state.expected, current, error));
}
bool busyBeforeCopy(int source, QString* error) {
    for (const auto& relative : activeWriters()) {
        bool missing = false;
        auto parent = relativeDirectory(source, parentPart(relative), &missing, error);
        if (!parent) { if (missing) continue; return false; }
        struct stat st{};
        if (::fstatat(parent.get(), leafPart(relative).constData(), &st, AT_SYMLINK_NOFOLLOW) == 0)
            return fail(error, QStringLiteral("기존 프로필에 활성 쓰기 잠금이 있어 복사하지 않았습니다."));
        if (errno != ENOENT) return fail(error, QStringLiteral("기존 프로필 잠금을 확인하지 못했습니다."));
    }
    struct stat st{};
    if (::fstatat(source, LegacyLockName, &st, AT_SYMLINK_NOFOLLOW) == 0)
        return fail(error, QStringLiteral("기존 프로필이 잠겨 있어 복사하지 않았습니다."));
    return errno == ENOENT || fail(error, QStringLiteral("기존 실행 잠금을 확인할 수 없습니다."));
}
Fd ensureDirectoryTree(const QString& path, bool privateRoot, QString* error) {
    bool missing = false;
    auto current = openDirectory(path, &missing, error);
    if (current) {
        struct stat identity{};
        if (::fstat(current.get(), &identity) != 0 || !safeNode(identity, true)
            || (privateRoot && (identity.st_mode & 0777) != 0700)) {
            fail(error, QStringLiteral("새 프로필 상위 폴더의 소유자 또는 권한이 안전하지 않습니다."));
            return {};
        }
        return current;
    }
    if (!missing) return {};
    const QFileInfo info(path);
    if (info.absolutePath() == path || info.fileName().isEmpty()) {
        fail(error, QStringLiteral("새 프로필 폴더를 만들 기반 경로가 없습니다."));
        return {};
    }
    auto parent = ensureDirectoryTree(info.absolutePath(), false, error);
    if (!parent) return {};
    const auto name = QFile::encodeName(info.fileName());
    if (::mkdirat(parent.get(), name.constData(), 0700) != 0 && errno != EEXIST) {
        fail(error, QStringLiteral("새 프로필의 비공개 폴더를 만들 수 없습니다."));
        return {};
    }
    current.reset(::openat(parent.get(), name.constData(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat identity{};
    if (!current || ::fstat(current.get(), &identity) != 0 || !safeNode(identity, true)
        || (privateRoot && (identity.st_mode & 0777) != 0700)
        || !syncFd(parent.get(), error) || !closeChecked(parent, error)) {
        fail(error, QStringLiteral("만든 프로필 폴더를 안전하게 확인할 수 없습니다."));
        return {};
    }
    return current;
}

bool initializeSession(SessionState& state, const DevProfilePaths& paths,
                       const QString& version, bool upgrade, bool fresh, QString* error) {
    state.upgrade = upgrade;
    state.completionMarker = upgrade ? profile_upgrade_detail::ActiveMarkerName : MarkerName;
    state.paths = paths;
    state.version = version;
    state.parentPath = QFileInfo(paths.currentDirectory).absolutePath();
    const auto reject = [&]() {
        cleanupSession(state);
        if (error && !state.warning.isEmpty()) {
            if (!error->isEmpty()) *error += '\n';
            *error += state.warning;
        }
        return false;
    };
    state.source = openDirectory(paths.legacyDirectory, nullptr, error);
    if (!state.source || !privateDirectoryFd(state.source.get(), &state.sourceIdentity, error))
        return reject();
    if (upgrade) {
        state.parent = ensureDirectoryTree(state.parentPath, false, error);
        if (!state.parent || ::fstat(state.parent.get(), &state.parentIdentity) != 0
            || !safeNode(state.parentIdentity, true)
            || state.parentIdentity.st_dev != state.sourceIdentity.st_dev) {
            fail(error, QStringLiteral("원본과 새 프로필이 안전한 같은 볼륨에 있지 않습니다."));
            return reject();
        }
    } else {
        auto base = openDirectory(paths.baseDirectory, nullptr, error);
        struct stat baseIdentity{};
        if (!base || ::fstat(base.get(), &baseIdentity) != 0 || !safeNode(baseIdentity, true)) {
            fail(error, QStringLiteral("프로필의 기반 폴더가 안전하지 않습니다."));
            return reject();
        }
        struct stat parentListed{};
        bool parentMissing = false;
        if (!relativeStat(base.get(), "KEEFI", &parentListed, &parentMissing, error)) return reject();
        if (parentMissing && ::mkdirat(base.get(), "KEEFI", 0700) != 0) {
            // Another process may have created the parent. It still must pass the no-follow check below.
            if (errno != EEXIST) {
                fail(error, QStringLiteral("새 프로필의 부모 폴더를 만들 수 없습니다."));
                return reject();
            }
        }
        state.parent = openDirectory(state.parentPath, nullptr, error);
        if (!state.parent || ::fstat(state.parent.get(), &state.parentIdentity) != 0
            || !safeNode(state.parentIdentity, true)
            || state.parentIdentity.st_dev != state.sourceIdentity.st_dev) {
            fail(error, QStringLiteral("원본과 새 프로필이 안전한 같은 볼륨에 있지 않습니다."));
            return reject();
        }
        if (parentMissing && !syncFd(base.get(), error)) return reject();
        if (!closeChecked(base, error)) return reject();
    }
    struct stat target{};
    bool targetMissing = false;
    if (!relativeStat(state.parent.get(), QFile::encodeName(QFileInfo(paths.currentDirectory).fileName()),
                      &target, &targetMissing, error)) return reject();
    if ((!fresh && !targetMissing) || (fresh && (targetMissing || !sameInode(target, state.sourceIdentity)))) {
        fail(error, QStringLiteral("새 프로필 경로가 이미 있거나 초기 폴더의 정체성이 바뀌었습니다."));
        return reject();
    }
    if (!recoverControl(paths, state.parent.get(), state.source.get(), error)
        || !boundDirectory(paths.legacyDirectory, state.source.get(), state.sourceIdentity, true, error)
        || !boundDirectory(state.parentPath, state.parent.get(), state.parentIdentity, false, error)
        || !busyBeforeCopy(state.source.get(), error))
        return reject();

    QTemporaryDir temporary(QDir(state.parentPath).filePath(upgrade
        ? QStringLiteral(".keefi-profile-upgrade-XXXXXX") : QStringLiteral(".keefi-dev-profile-copy-XXXXXX")));
    if (!temporary.isValid()) {
        fail(error, QStringLiteral("비공개 임시 작업 폴더를 만들 수 없습니다."));
        return reject();
    }
    state.controlPath = temporary.path();
    state.controlName = QFile::encodeName(QFileInfo(state.controlPath).fileName());
    state.control.reset(::openat(state.parent.get(), state.controlName.constData(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!state.control || !privateDirectoryFd(state.control.get(), &state.controlIdentity, error))
        return reject();
    temporary.setAutoRemove(false);
    state.ownsControl = true;
    if (::mkdirat(state.control.get(), StageName, 0700) != 0) {
        fail(error, QStringLiteral("복사할 프로필의 비공개 임시 폴더를 만들 수 없습니다."));
        return reject();
    }
    state.stagePath = QDir(state.controlPath).filePath(QString::fromLatin1(StageName));
    state.stage.reset(::openat(state.control.get(), StageName,
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!state.stage || !privateDirectoryFd(state.stage.get(), &state.stageIdentity, error))
        return reject();
    if (state.stageIdentity.st_gid != state.sourceIdentity.st_gid
        && ::fchown(state.stage.get(), uid_t(-1), state.sourceIdentity.st_gid) != 0) {
        fail(error, QStringLiteral("새 프로필 폴더의 그룹을 보존할 수 없습니다."));
        return reject();
    }
    if (!privateDirectoryFd(state.stage.get(), &state.stageIdentity, error)
        || !syncFd(state.stage.get(), error) || !syncFd(state.control.get(), error))
        return reject();
    state.nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto application = QCoreApplication::applicationName();
    if (application.isEmpty()) application = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    state.owner = {
        {"format", OwnerFormat}, {"phase", "initial"}, {"source", paths.legacyDirectory},
        {"target", paths.currentDirectory}, {"version", version}, {"nonce", state.nonce},
        {"stage", directoryProof(state.stageIdentity)},
        {"pid", qint64(::getpid())}, {"host", QSysInfo::machineHostName()}, {"app", application}};
    if (application.isEmpty() || !writeOwnerRecord(state, false, error))
        return reject();
    if (!boundDirectory(state.parentPath, state.parent.get(), state.parentIdentity, false, error))
        return reject();
    const auto fixedName = QFile::encodeName(QFileInfo(paths.transactionDirectory).fileName());
    if (!exclusiveRename(state.parent.get(), state.controlName, state.parent.get(), fixedName, error))
        return reject();
    // Publish only the control before acquiring QLockFile: its stored pathname must never move.
    state.controlName = fixedName;
    state.controlPath = paths.transactionDirectory;
    state.stagePath = QDir(state.controlPath).filePath(QString::fromLatin1(StageName));
    if (!syncFd(state.parent.get(), error)) return reject();
    state.lock = std::make_unique<QLockFile>(
        QDir(state.controlPath).filePath(QString::fromLatin1(SourceLockName)));
    state.lock->setStaleLockTime(0);
    if (!state.lock->tryLock(0)) {
        state.lock.reset();
        fail(error, QStringLiteral("새 복사 작업의 실행 잠금을 얻지 못했습니다."));
        return reject();
    }
    if (!hashFileAt(state.control.get(), SourceLockName, &state.lockIdentity, &state.lockHash, true, error)
        || state.lockIdentity.st_nlink != 1) return reject();
    qint64 pid = 0;
    QString host, app;
    if (!state.lock->getLockInfo(&pid, &host, &app) || pid != qint64(::getpid())
        || host.isEmpty() || app.isEmpty()) {
        fail(error, QStringLiteral("새 실행 잠금의 소유자를 확인하지 못했습니다."));
        return reject();
    }
    Fd syncLock(::openat(state.control.get(), SourceLockName, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat syncIdentity{};
    if (!syncLock || ::fstat(syncLock.get(), &syncIdentity) != 0
        || !sameInode(syncIdentity, state.lockIdentity) || !syncFd(syncLock.get(), error)
        || !closeChecked(syncLock, error))
        return reject();
    state.owner["phase"] = QStringLiteral("locked");
    state.owner["pid"] = pid;
    state.owner["host"] = host;
    state.owner["app"] = app;
    state.owner["lock"] = QJsonObject{
        {"device", QString::number(qulonglong(state.lockIdentity.st_dev))},
        {"inode", QString::number(qulonglong(state.lockIdentity.st_ino))},
        {"uid", qint64(state.lockIdentity.st_uid)}, {"sha256", QString::fromLatin1(state.lockHash)},
        {"pid", pid}, {"host", host}, {"app", app}};
    if (!writeOwnerRecord(state, true, error)
        || !boundDirectory(paths.legacyDirectory, state.source.get(), state.sourceIdentity, true, error))
        return reject();
    // linkat is exclusive. Do not call tryLock on the legacy path: that can remove a foreign stale lock.
    if (::linkat(state.control.get(), SourceLockName, state.source.get(), LegacyLockName, 0) != 0) {
        fail(error, QStringLiteral("기존 실행 잠금이 생겼거나 복사 잠금을 게시하지 못했습니다."));
        return reject();
    }
    state.ownOldLink = true;
    if (!syncFd(state.source.get(), error)
        || !ownOldLock(state.source.get(), state.lockIdentity, state.lockHash, false, nullptr, error))
        return reject();
    const ScanRules sourceRules{true, false, &state.lockIdentity, &state.lockHash,
                                upgrade, state.completionMarker, upgrade};
    if (!scanRoot(state.source.get(), sourceRules, &state.original, error)
        || !(upgrade ? upgradeManifest(state.original, &state.expected, error)
                     : expectedManifest(state.original, &state.expected, error)))
        return reject();
    return true;
}

bool copySessionFiles(SessionState& state, QString* error) {
    clear(error);
    if (state.failed || state.committed || state.copied)
        return fail(error, QStringLiteral("이 복사 작업은 파일 복사를 시작할 수 없습니다."));
    const auto failed = [&]() { state.failed = true; return false; };
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)) return failed();
    QStringList stageNames;
    if (!namesAt(state.stage.get(), &stageNames, error)) return failed();
    if (!stageNames.isEmpty()) {
        fail(error, QStringLiteral("임시 프로필 폴더가 비어 있지 않아 복사하지 않았습니다."));
        return failed();
    }
    QStringList directories;
    for (auto it = state.expected.cbegin(); it != state.expected.cend(); ++it)
        if (it.value().directory() && it.key() != QStringLiteral(".")) directories.append(it.key());
    std::sort(directories.begin(), directories.end(), [](const QString& a, const QString& b) {
        const auto ac = a.count('/'), bc = b.count('/');
        return ac == bc ? a < b : ac < bc;
    });
    for (const auto& path : directories) {
        auto parent = relativeDirectory(state.stage.get(), parentPart(path), nullptr, error);
        if (!parent) return failed();
        if (::mkdirat(parent.get(), leafPart(path).constData(), 0700) != 0) {
            fail(error, QStringLiteral("복사본 폴더를 만들 수 없거나 같은 이름이 이미 있습니다."));
            return failed();
        }
        if (!closeChecked(parent, error)) return failed();
    }
    for (auto it = state.original.cbegin(); it != state.original.cend(); ++it) {
        if (it.value().directory()) continue;
        const auto targetName = state.restoring && it.key() == profile_upgrade_detail::ActiveMarkerName
            ? QString{} : state.upgrade ? upgradedName(it.key()) : mappedName(it.key());
        if (targetName.isEmpty()) continue;
        auto sourceParent = relativeDirectory(state.source.get(), parentPart(it.key()), nullptr, error);
        auto targetParent = relativeDirectory(state.stage.get(), parentPart(targetName), nullptr, error);
        if (!sourceParent || !targetParent) return failed();
        Fd sourceFile(::openat(sourceParent.get(), leafPart(it.key()).constData(),
                               O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        struct stat before{};
        if (!sourceFile || ::fstat(sourceFile.get(), &before) != 0 || !safeNode(before, false)
            || !stableFile(before, it.value().identity)) {
            fail(error, QStringLiteral("복사할 원본 파일을 읽을 수 없거나 파일이 바뀌었습니다."));
            return failed();
        }
        Fd targetFile(::openat(targetParent.get(), leafPart(targetName).constData(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!targetFile) {
            fail(error, QStringLiteral("복사할 대상 파일을 만들 수 없습니다."));
            return failed();
        }
        QFile input, output;
        if (!input.open(sourceFile.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)
            || !output.open(targetFile.get(), QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) {
            fail(error, QStringLiteral("파일 복사 스트림을 열 수 없습니다."));
            return failed();
        }
        if (!copyProfileStream(input, output, error)) return failed();
        if (!output.flush() || output.error() != QFileDevice::NoError) {
            fail(error, QStringLiteral("복사한 파일을 완전히 기록하지 못했습니다."));
            return failed();
        }
        struct stat targetIdentity{};
        if (::fstat(targetFile.get(), &targetIdentity) != 0 || !safeNode(targetIdentity, false)) {
            fail(error, QStringLiteral("복사한 파일의 정체성을 확인하지 못했습니다."));
            return failed();
        }
        if ((targetIdentity.st_gid != before.st_gid && ::fchown(targetFile.get(), uid_t(-1), before.st_gid) != 0)
            || ::fchmod(targetFile.get(), before.st_mode & 0777) != 0) {
            fail(error, QStringLiteral("복사한 파일의 권한 또는 그룹을 보존하지 못했습니다."));
            return failed();
        }
        if (!syncFd(targetFile.get(), error)) return failed();
        struct stat after{};
        if (::fstat(sourceFile.get(), &after) != 0 || !stableFile(before, after)) {
            fail(error, QStringLiteral("복사하는 동안 원본 파일이 바뀌었습니다."));
            return failed();
        }
        input.close();
        output.close();
        if (!closeChecked(sourceFile, error) || !closeChecked(targetFile, error)
            || !closeChecked(sourceParent, error) || !closeChecked(targetParent, error))
            return failed();
    }
    // Set final directory permissions only after their children have been written.
    std::reverse(directories.begin(), directories.end());
    directories.append(QStringLiteral("."));
    for (const auto& path : directories) {
        auto directory = relativeDirectory(state.stage.get(), path, nullptr, error);
        if (!directory) return failed();
        const auto& expected = state.expected.value(path).identity;
        struct stat current{};
        if (::fstat(directory.get(), &current) != 0
            || (current.st_gid != expected.st_gid && ::fchown(directory.get(), uid_t(-1), expected.st_gid) != 0)
            || ::fchmod(directory.get(), expected.st_mode & 0777) != 0) {
            fail(error, QStringLiteral("복사한 폴더의 권한 또는 그룹을 보존하지 못했습니다."));
            return failed();
        }
        if (!syncFd(directory.get(), error) || !closeChecked(directory, error)) return failed();
    }
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)
        || !stageMatches(state, false, error)) return failed();
    state.copied = true;
    return true;
}

} // namespace

struct ProfileCopySession::Private : SessionState {};


std::unique_ptr<ProfileCopySession> ProfileCopySession::begin(const DevProfilePaths& paths,
                                                             const QString& version,
                                                             QString* error) {
    clear(error);
    if (!samePaths(paths, devProfilePaths(paths.baseDirectory)) || version.trimmed().isEmpty()) {
        fail(error, QStringLiteral("프로필 복사 계획의 경로 또는 버전이 올바르지 않습니다."));
        return nullptr;
    }
    auto state = std::make_unique<Private>();
    if (!initializeSession(*state, paths, version, false, false, error)) return nullptr;
    return std::unique_ptr<ProfileCopySession>(new ProfileCopySession(std::move(state)));
}

bool ProfileCopySession::copy(QString* error) {
    if (!d_) return fail(error, QStringLiteral("이 복사 작업은 파일 복사를 시작할 수 없습니다."));
    return copySessionFiles(*d_, error);
}

bool ProfileCopySession::verify(QString* error) {
    clear(error);
    if (!d_ || d_->failed || !d_->copied || d_->verified || d_->committed)
        return fail(error, QStringLiteral("파일 복사를 마치기 전에는 프로필을 검증할 수 없습니다."));
    auto& state = *d_;
    const auto failed = [&]() { state.failed = true; return false; };
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)
        || !stageMatches(state, false, error)) return failed();
    qint64 fileCount = 0, totalBytes = 0;
    QString manifestHash;
    if (!manifestEvidence(state.original, &fileCount, &totalBytes, &manifestHash, error)) return failed();
    const QJsonObject marker{
        {"format", MarkerFormat}, {"source", state.paths.legacyDirectory},
        {"target", state.paths.currentDirectory}, {"version", state.version}, {"verified", true},
        {"completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"file_count", fileCount}, {"total_bytes", totalBytes}, {"source_manifest_sha256", manifestHash}};
    if (!writeAtomicJson(state.stage.get(), QFile::encodeName(MarkerName), marker, false,
                         &state.markerBytes, error)) return failed();
    QJsonObject readBack;
    QByteArray raw;
    if (!readJsonAt(state.stage.get(), QFile::encodeName(MarkerName), &readBack, error, &raw)
        || raw != state.markerBytes || readBack != marker || !validMarker(readBack, state.paths, error)) {
        fail(error, QStringLiteral("완료 표식을 기록한 뒤 검증하지 못했습니다."));
        return failed();
    }
    state.verified = true;
    return true;
}

bool ProfileCopySession::publish(QString* error) {
    clear(error);
    if (!d_ || d_->failed || !d_->copied || !d_->verified || d_->committed)
        return fail(error, QStringLiteral("검증이 끝난 복사본만 새 프로필로 게시할 수 있습니다."));
    auto& state = *d_;
    const auto failed = [&]() { state.failed = true; return false; };
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)
        || !stageMatches(state, true, error)) return failed();
    QJsonObject marker;
    QByteArray raw;
    if (!readJsonAt(state.stage.get(), QFile::encodeName(MarkerName), &marker, error, &raw)
        || raw != state.markerBytes || !validMarker(marker, state.paths, error)) {
        fail(error, QStringLiteral("게시 전에 완료 표식이 바뀌어 중단했습니다."));
        return failed();
    }
    struct stat target{};
    bool missing = false;
    const auto targetName = QFile::encodeName(QFileInfo(state.paths.currentDirectory).fileName());
    if (!relativeStat(state.parent.get(), targetName, &target, &missing, error)) return failed();
    if (!missing) {
        fail(error, QStringLiteral("새 프로필 경로가 생겨 기존 자료를 덮어쓰지 않았습니다."));
        return failed();
    }
    if (!syncFd(state.stage.get(), error) || !syncFd(state.control.get(), error)) return failed();
    if (!exclusiveRename(state.control.get(), StageName, state.parent.get(), targetName, error))
        return failed();
    // The successful no-replace rename is the commit. Nothing below may delete,
    // rename back, or report an uncommitted copy of the published current profile.
    state.committed = true;
    QString cleanupError;
    if (!syncFd(state.parent.get(), &cleanupError)) addWarning(state, cleanupError);
    cleanupSession(state);
    for (auto* descriptor : {&state.stage, &state.control, &state.source, &state.parent}) {
        cleanupError.clear();
        if (!closeChecked(*descriptor, &cleanupError)) addWarning(state, cleanupError);
    }
    return true;
}
QString ProfileCopySession::stagingDirectory() const { return d_ ? d_->stagePath : QString{}; }
QString ProfileCopySession::warning() const { return d_ ? d_->warning : QString{}; }

#else
struct ProfileCopySession::Private {};
std::unique_ptr<ProfileCopySession> ProfileCopySession::begin(const DevProfilePaths&, const QString&, QString* error) {
    fail(error, QStringLiteral("이 시스템에서는 안전한 프로필 복사를 지원하지 않습니다."));
    return nullptr;
}
bool ProfileCopySession::copy(QString* error) { return fail(error, QStringLiteral("프로필 복사를 지원하지 않습니다.")); }
bool ProfileCopySession::verify(QString* error) { return fail(error, QStringLiteral("프로필 복사를 지원하지 않습니다.")); }
bool ProfileCopySession::publish(QString* error) { return fail(error, QStringLiteral("프로필 복사를 지원하지 않습니다.")); }
QString ProfileCopySession::stagingDirectory() const { return {}; }
QString ProfileCopySession::warning() const { return {}; }
#endif

ProfileCopySession::ProfileCopySession(std::unique_ptr<Private> state) : d_(std::move(state)) {}
ProfileCopySession::~ProfileCopySession() = default;

#ifdef Q_OS_UNIX
namespace {
using namespace profile_upgrade_detail;

bool synchronizeManifest(int root, const Manifest& manifest, QString* error) {
    QStringList directories;
    for (auto it = manifest.cbegin(); it != manifest.cend(); ++it) {
        if (it.value().directory()) { directories.append(it.key()); continue; }
        auto parent = relativeDirectory(root, parentPart(it.key()), nullptr, error);
        if (!parent) return false;
        Fd file(::openat(parent.get(), leafPart(it.key()).constData(),
                         O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        struct stat identity{};
        if (!file || ::fstat(file.get(), &identity) != 0 || !safeNode(identity, false)
            || !stableFile(identity, it.value().identity)
            || !syncFd(file.get(), error) || !closeChecked(file, error)
            || !closeChecked(parent, error))
            return fail(error, QStringLiteral("변환한 파일을 완전히 저장하거나 확인하지 못했습니다."));
    }
    std::sort(directories.begin(), directories.end(), [](const QString& a, const QString& b) {
        return a.count('/') > b.count('/');
    });
    for (const auto& path : directories) {
        auto directory = relativeDirectory(root, path, nullptr, error);
        if (!directory || !syncFd(directory.get(), error) || !closeChecked(directory, error)) return false;
    }
    return true;
}
bool convertedLayout(const Manifest& manifest, QString* error) {
    for (auto it = manifest.cbegin(); it != manifest.cend(); ++it) {
        const auto& path = it.key();
        if (path == MarkerName || path == ActiveMarkerName
            || path == legacy::SettingsDirectory || path.startsWith(legacy::SettingsDirectory + '/')
            || path == legacy::OrganizationSettings || legacy::StorageFiles.contains(path)
            || activeWriters().contains(path) || path == QString::fromLatin1(LegacyLockName))
            return fail(error, QStringLiteral("변환 뒤 옛 형식 파일 또는 쓰기 잠금이 남아 있어 게시하지 않았습니다."));
    }
    return true;
}
bool conversionEvidenceMatches(const QJsonObject& evidence, const Manifest& source,
                               const Manifest& converted, QString* error) {
    // A converter may perform only public-state migration. When it reports
    // encrypted outputs, each readback proof must bind both input and output.
    if (!evidence.contains("convertedFiles")) return true;
    const auto records = evidence.value("convertedFiles");
    if (!records.isArray())
        return fail(error, QStringLiteral("변환 파일 검증 증거의 형식이 올바르지 않습니다."));
    QSet<QString> outputs;
    for (const auto& record : records.toArray()) {
        if (!record.isObject()) return fail(error, QStringLiteral("변환 파일 검증 증거를 읽을 수 없습니다."));
        const auto item = record.toObject();
        const auto oldName = item.value("source").toString();
        const auto name = item.value("name").toString();
        const auto safeRelative = [](const QString& path) {
            return !path.isEmpty() && path != QStringLiteral(".") && !QDir::isAbsolutePath(path)
                && QDir::cleanPath(path) == path && !path.split('/').contains(QStringLiteral(".."));
        };
        const auto oldSize = item.value("sourceBytes").toInteger(-1);
        const auto newSize = item.value("cipherBytes").toInteger(-1);
        if (!safeRelative(oldName) || !safeRelative(name) || outputs.contains(name)
            || !source.contains(oldName) || source.value(oldName).directory()
            || !converted.contains(name) || converted.value(name).directory()
            || oldSize < 0 || newSize < 0
            || item.value("sourceBytes") != QJsonValue(oldSize) || item.value("cipherBytes") != QJsonValue(newSize)
            || item.value("sourceDigest").toString() != QString::fromLatin1(source.value(oldName).hash)
            || item.value("cipherDigest").toString() != QString::fromLatin1(converted.value(name).hash)
            || oldSize != qint64(source.value(oldName).identity.st_size)
            || newSize != qint64(converted.value(name).identity.st_size))
            return fail(error, QStringLiteral("변환기가 검증한 원본 또는 새 파일과 실제 사본이 다릅니다."));
        outputs.insert(name);
    }
    return true;
}
bool validateImportedMarker(SessionState& state, QString* error) {
    if (!state.original.contains(MarkerName)) return true;
    QJsonObject marker;
    if (!readJsonAt(state.source.get(), QFile::encodeName(MarkerName), &marker, error)) return false;
    // Historical paths are evidence only. They are never used to open or write
    // a file; an explicitly selected archived copy may have moved since Dev100.
    const DevProfilePaths recorded{state.paths.baseDirectory,
        marker.value("source").toString(), marker.value("target").toString(), {}};
    if (recorded.legacyDirectory.isEmpty() || recorded.currentDirectory.isEmpty()
        || normalized(recorded.legacyDirectory) != recorded.legacyDirectory
        || normalized(recorded.currentDirectory) != recorded.currentDirectory
        || !validMarker(marker, recorded, error))
        return fail(error, QStringLiteral("가져올 프로필의 과거 이전 기록을 확인할 수 없습니다."));
    return true;
}
void finishCommittedSession(SessionState& state) {
    QString error;
    if (!syncFd(state.parent.get(), &error)) addWarning(state, error);
    cleanupSession(state);
    for (auto* descriptor : {&state.stage, &state.control, &state.source, &state.parent}) {
        error.clear();
        if (!closeChecked(*descriptor, &error)) addWarning(state, error);
    }
}
QString recoverUpgradeAfterPublication(const ProfileUpgradePlan& plan) {
    if (!pathExists(plan.transactionDirectory)) return {};
    QString error;
    const auto parentPath = QFileInfo(plan.targetDirectory).absolutePath();
    auto parent = openDirectory(parentPath, nullptr, &error);
    if (!parent) return error;
    QJsonObject owner;
    if (!readJson(QDir(plan.transactionDirectory).filePath(QString::fromLatin1(OwnerName)), &owner, &error))
        return error;
    const auto source = owner.value("source").toString();
    const bool explicitInput = plan.explicitDirectory
        && (source == normalized(*plan.explicitDirectory)
            || source + QStringLiteral(".keefi") == plan.targetDirectory);
    const bool defaultInput = !plan.explicitDirectory
        && (source == plan.paths.legacyOriginalDirectory || source == plan.paths.dev100Directory);
    if (owner.value("target").toString() != plan.targetDirectory
        || (source != plan.targetDirectory && !explicitInput && !defaultInput))
        return QStringLiteral("새 프로필은 사용할 수 있으나 알 수 없는 이전 작업 기록을 보존했습니다.");
    bool missing = false;
    auto original = openDirectory(source, &missing, &error);
    if (!original && !missing) return error;
    const DevProfilePaths paths{plan.paths.baseDirectory, source,
                                plan.targetDirectory, plan.transactionDirectory};
    if (!recoverControl(paths, parent.get(), original ? original.get() : -1, &error)) return error;
    return {};
}
} // namespace

struct ProfileUpgradeSession::Private : SessionState {
    ProfileUpgradePlan plan;
    ProfileConversionReceipt conversion;
    bool converted = false;
};

std::unique_ptr<ProfileUpgradeSession> ProfileUpgradeSession::begin(
    const ProfileUpgradePlan& plan, const QString& version, QString* error) {
    clear(error);
    auto current = profile_upgrade_detail::revalidatedPlan(plan, error);
    if (current.action != ProfileUpgradeAction::OfferUpgrade || version.trimmed().isEmpty()
        || current.sourceDirectory.isEmpty()
        || current.targetDirectory == current.sourceDirectory
        || current.targetDirectory.startsWith(current.sourceDirectory + '/')
        || current.sourceDirectory.startsWith(current.targetDirectory + '/')
        || QFileInfo(current.transactionDirectory).absolutePath()
           != QFileInfo(current.targetDirectory).absolutePath()) {
        if (!error || error->isEmpty())
            fail(error, QStringLiteral("안전한 원본과 별도 대상이 선택된 전환만 시작할 수 있습니다."));
        return {};
    }
    auto state = std::make_unique<Private>();
    state->plan = current;
    const DevProfilePaths paths{current.paths.baseDirectory, current.sourceDirectory,
                                current.targetDirectory, current.transactionDirectory};
    if (!initializeSession(*state, paths, version, true, false, error)
        || !validateImportedMarker(*state, error)) return {};
    return std::unique_ptr<ProfileUpgradeSession>(new ProfileUpgradeSession(std::move(state)));
}
bool ProfileUpgradeSession::copy(QString* error) {
    if (!d_) return fail(error, QStringLiteral("전환 작업을 시작하지 않았습니다."));
    return copySessionFiles(*d_, error);
}
bool ProfileUpgradeSession::convert(const QString& password, const ProfileStageConverter& converter,
                                    QString* error) {
    clear(error);
    if (!d_ || d_->failed || !d_->copied || d_->converted || d_->verified || d_->committed || !converter)
        return fail(error, QStringLiteral("확인한 임시 복사본만 새 저장 형식으로 변환할 수 있습니다."));
    auto& state = *d_;
    const auto failed = [&]() { state.failed = true; return false; };
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)
        || !stageMatches(state, false, error)) return failed();
    state.conversion = {};
    if (!converter(state.stagePath, password, &state.conversion, error)) {
        if (!error || error->isEmpty()) fail(error, QStringLiteral("저장 형식 변환을 마치지 못했습니다."));
        return failed();
    }
    if (!state.conversion.verified || state.conversion.identityScheme != QStringLiteral("legacy-v1")) {
        fail(error, QStringLiteral("저장 형식 변환의 검증 결과 또는 기록 식별 방식을 확인할 수 없습니다."));
        return failed();
    }
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)) return failed();
    Manifest converted;
    if (!scanRoot(state.stage.get(), ScanRules{false, false, nullptr, nullptr, false,
                  state.completionMarker, true}, &converted, error)
        || !convertedLayout(converted, error)
        || !conversionEvidenceMatches(state.conversion.evidence, state.original, converted, error)
        || !synchronizeManifest(state.stage.get(), converted, error))
        return failed();
    state.expected = std::move(converted);
    state.convertedSnapshot = true;
    if (!stageMatches(state, false, error) || !sourceUnchanged(state, error)) return failed();
    state.converted = true;
    return true;
}
bool ProfileUpgradeSession::verify(QString* error) {
    clear(error);
    if (!d_ || d_->failed || !d_->converted || d_->verified || d_->committed)
        return fail(error, QStringLiteral("새 저장 형식으로 변환된 사본만 검증할 수 있습니다."));
    auto& state = *d_;
    const auto failed = [&]() { state.failed = true; return false; };
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)
        || !stageMatches(state, false, error)) return failed();
    qint64 sourceFiles = 0, sourceBytes = 0, convertedFiles = 0, convertedBytes = 0;
    QString sourceHash, convertedHash;
    if (!manifestEvidence(state.original, &sourceFiles, &sourceBytes, &sourceHash, error)
        || !manifestEvidence(state.expected, &convertedFiles, &convertedBytes, &convertedHash, error))
        return failed();
    const auto completed = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject marker{
        {"format", ActiveMarkerFormat}, {"identity_scheme", state.conversion.identityScheme},
        {"version", state.version}, {"verified", true}, {"completed_at_utc", completed},
        {"source_kind", sourceName(state.plan.sourceKind)}, {"source_manifest_sha256", sourceHash},
        {"file_count", convertedFiles}, {"total_bytes", convertedBytes}, {"import_id", state.nonce}};
    // Original names and paths stay outside the active profile. This receipt
    // records verification before publication; its existence never proves commit.
    const QJsonObject evidence{
        {"format", "keefi-profile-import-evidence/1"}, {"phase", "verified-staging"},
        {"version", state.version}, {"import_id", state.nonce}, {"verified_at_utc", completed},
        {"source_kind", sourceName(state.plan.sourceKind)}, {"source_directory", state.paths.legacyDirectory},
        {"target_directory", state.paths.currentDirectory}, {"identity_scheme", state.conversion.identityScheme},
        {"source_manifest_sha256", sourceHash}, {"source_file_count", sourceFiles}, {"source_total_bytes", sourceBytes},
        {"converted_manifest_sha256", convertedHash}, {"conversion", state.conversion.evidence}};
    const auto evidenceName = QFile::encodeName(QStringLiteral(".keefi-profile-import-") + state.nonce
                                                + QStringLiteral(".json"));
    if (!writeAtomicJson(state.parent.get(), evidenceName, evidence, false, nullptr, error)
        || !writeAtomicJson(state.stage.get(), QFile::encodeName(ActiveMarkerName), marker, false,
                            &state.markerBytes, error))
        return failed();
    QJsonObject readBack;
    QByteArray raw;
    if (!readJsonAt(state.stage.get(), QFile::encodeName(ActiveMarkerName), &readBack, error, &raw)
        || raw != state.markerBytes || readBack != marker || !activeMarker(readBack, error)) {
        fail(error, QStringLiteral("새 프로필의 완료 표식을 검증하지 못했습니다."));
        return failed();
    }
    state.verified = true;
    return true;
}
bool ProfileUpgradeSession::publish(QString* error) {
    clear(error);
    if (!d_ || d_->failed || !d_->converted || !d_->verified || d_->committed)
        return fail(error, QStringLiteral("변환과 검증이 모두 끝난 새 프로필만 게시할 수 있습니다."));
    auto& state = *d_;
    const auto failed = [&]() { state.failed = true; return false; };
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)
        || !stageMatches(state, true, error)) return failed();
    QJsonObject marker;
    QByteArray raw;
    if (!readJsonAt(state.stage.get(), QFile::encodeName(ActiveMarkerName), &marker, error, &raw)
        || raw != state.markerBytes || !activeMarker(marker, error)) {
        fail(error, QStringLiteral("게시 전에 새 프로필 완료 표식이 바뀌었습니다."));
        return failed();
    }
    const auto name = QFile::encodeName(QFileInfo(state.paths.currentDirectory).fileName());
    struct stat target{};
    bool missing = false;
    if (!relativeStat(state.parent.get(), name, &target, &missing, error)) return failed();
    if (!missing) {
        fail(error, QStringLiteral("새 대상 경로가 생겨 기존 파일을 덮어쓰지 않았습니다."));
        return failed();
    }
    if (!syncFd(state.stage.get(), error) || !syncFd(state.control.get(), error)
        || !exclusiveRename(state.control.get(), StageName, state.parent.get(), name, error))
        return failed();
    state.committed = true;
    finishCommittedSession(state);
    return true;
}
QString ProfileUpgradeSession::stagingDirectory() const { return d_ ? d_->stagePath : QString{}; }
ProfileReady ProfileUpgradeSession::ready() const {
    return d_ && d_->committed
        ? ProfileReady{d_->paths.currentDirectory, d_->conversion.identityScheme, d_->warning}
        : ProfileReady{};
}

ProfileReady existingProfileReady(const ProfileUpgradePlan& plan, QString* error) {
    const auto current = profile_upgrade_detail::revalidatedPlan(plan, error);
    if (current.action != ProfileUpgradeAction::UseCurrent) {
        if (!error || error->isEmpty()) fail(error, QStringLiteral("완료된 새 프로필을 선택하지 않았습니다."));
        return {};
    }
    return {current.targetDirectory, current.identityScheme, recoverUpgradeAfterPublication(current)};
}
ProfileReady createFreshProfile(const ProfileUpgradePlan& plan, const QString& version, QString* error) {
    clear(error);
    const auto current = profile_upgrade_detail::revalidatedPlan(plan, error);
    if (current.action != ProfileUpgradeAction::CreateCurrent || version.trimmed().isEmpty()) {
        if (!error || error->isEmpty()) fail(error, QStringLiteral("새 빈 프로필을 만들 계획이 아닙니다."));
        return {};
    }
    // A pre-existing empty explicit directory stays at that exact path. It is
    // activated only by an exclusive verified marker while the ordinary instance
    // lock is held; no legacy data is written through this path.
    auto root = ensureDirectoryTree(current.targetDirectory, true, error);
    if (!root || !closeChecked(root, error)) return {};
    SessionState state;
    const DevProfilePaths paths{current.paths.baseDirectory, current.targetDirectory,
                                current.targetDirectory, current.transactionDirectory};
    if (!initializeSession(state, paths, version, true, true, error)) return {};
    if (state.original.size() != 1 || !state.original.contains(QStringLiteral("."))) {
        fail(error, QStringLiteral("빈 프로필에 다른 파일이 생겨 초기화하지 않았습니다."));
        return {};
    }
    if (!boundSession(state, true, error) || !sourceUnchanged(state, error)) return {};
    qint64 count = 0, bytes = 0;
    QString digest;
    if (!manifestEvidence(state.original, &count, &bytes, &digest, error)) return {};
    const QJsonObject marker{
        {"format", ActiveMarkerFormat}, {"identity_scheme", "keefi-v1"}, {"version", version},
        {"verified", true}, {"source_kind", "fresh"}, {"source_manifest_sha256", digest},
        {"completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QByteArray written, raw;
    QJsonObject readBack;
    if (!writeAtomicJson(state.source.get(), QFile::encodeName(ActiveMarkerName), marker, false, &written, error)
        || !readJsonAt(state.source.get(), QFile::encodeName(ActiveMarkerName), &readBack, error, &raw)
        || raw != written || readBack != marker || !activeMarker(readBack, error)) return {};
    state.committed = true;
    finishCommittedSession(state);
    return {current.targetDirectory, QStringLiteral("keefi-v1"), state.warning};
}
#else
struct ProfileUpgradeSession::Private {};
std::unique_ptr<ProfileUpgradeSession> ProfileUpgradeSession::begin(
    const ProfileUpgradePlan&, const QString&, QString* error) {
    fail(error, QStringLiteral("이 시스템에서는 안전한 프로필 전환을 지원하지 않습니다.")); return {};
}
bool ProfileUpgradeSession::copy(QString* error) { return fail(error, QStringLiteral("프로필 전환을 지원하지 않습니다.")); }
bool ProfileUpgradeSession::convert(const QString&, const ProfileStageConverter&, QString* error) {
    return fail(error, QStringLiteral("프로필 전환을 지원하지 않습니다."));
}
bool ProfileUpgradeSession::verify(QString* error) { return fail(error, QStringLiteral("프로필 전환을 지원하지 않습니다.")); }
bool ProfileUpgradeSession::publish(QString* error) { return fail(error, QStringLiteral("프로필 전환을 지원하지 않습니다.")); }
QString ProfileUpgradeSession::stagingDirectory() const { return {}; }
ProfileReady ProfileUpgradeSession::ready() const { return {}; }
#ifndef Q_OS_WIN
ProfileReady existingProfileReady(const ProfileUpgradePlan&, QString* error) {
    fail(error, QStringLiteral("이 시스템에서는 새 프로필을 확인할 수 없습니다.")); return {};
}
ProfileReady createFreshProfile(const ProfileUpgradePlan&, const QString&, QString* error) {
    fail(error, QStringLiteral("이 시스템에서는 새 프로필을 만들 수 없습니다.")); return {};
}
#endif
#endif
ProfileUpgradeSession::ProfileUpgradeSession(std::unique_ptr<Private> state) : d_(std::move(state)) {}
ProfileUpgradeSession::~ProfileUpgradeSession() = default;

#ifdef Q_OS_UNIX
namespace {
const QString RestoreOwnerFormat = QStringLiteral("keefi-profile-restore-owner/1");
const QStringList& publicSettingFiles() {
    static const QStringList paths{QStringLiteral("KEEFI/KEEFI Dev.ini"),
        QStringLiteral("KEEFI/KEEFI.ini"), QStringLiteral("KEEFI.ini")};
    return paths;
}
QJsonObject restoreLockProof(const struct stat& identity, const QByteArray& hash,
                             qint64 pid, const QString& host, const QString& app) {
    auto result = directoryProof(identity);
    result["sha256"] = QString::fromLatin1(hash);
    result["pid"] = pid; result["host"] = host; result["app"] = app;
    return result;
}
bool noEncryptedArtifacts(const Manifest& manifest, QString* error) {
    for (auto it = manifest.cbegin(); it != manifest.cend(); ++it) {
        if (!it.value().directory()
            && (activeWriters().contains(it.key())
                || storage_detail::encryptedStorageName(QFileInfo(it.key()).fileName())))
            return fail(error, QStringLiteral("현재 프로필에 금고 또는 암호화 자료가 있어 백업으로 교체하지 않았습니다."));
    }
    return true;
}
QString identityManifestDigest(const Manifest& manifest, bool omitSettings, bool omitSettingsDirectory) {
    QJsonArray entries;
    for (auto it = manifest.cbegin(); it != manifest.cend(); ++it) {
        if (omitSettings && publicSettingFiles().contains(it.key())) continue;
        if (omitSettingsDirectory && it.key() == QStringLiteral("KEEFI")) continue;
        const auto& node = it.value();
        const auto& st = node.identity;
        QJsonObject row{{"path", it.key()}, {"kind", node.directory() ? "directory" : "file"},
            {"device", QString::number(qulonglong(st.st_dev))},
            {"inode", QString::number(qulonglong(st.st_ino))},
            {"uid", qint64(st.st_uid)}, {"gid", qint64(st.st_gid)}, {"mode", qint64(st.st_mode & 07777)}};
        if (!node.directory()) {
            row["size"] = qint64(st.st_size); row["sha256"] = QString::fromLatin1(node.hash);
#ifdef Q_OS_MACOS
            row["mtime_s"] = QString::number(st.st_mtimespec.tv_sec);
            row["mtime_ns"] = QString::number(st.st_mtimespec.tv_nsec);
#else
            row["mtime_s"] = QString::number(st.st_mtim.tv_sec);
            row["mtime_ns"] = QString::number(st.st_mtim.tv_nsec);
#endif
        }
        entries.append(row);
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(entries).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool currentRestoreMarker(int source, const QString& scheme, QString* hash, QString* error) {
    QJsonObject marker; QByteArray raw;
    if (!readJsonAt(source, QFile::encodeName(profile_upgrade_detail::ActiveMarkerName), &marker, error, &raw)
        || !profile_upgrade_detail::activeMarker(marker, error)
        || marker.value("identity_scheme").toString() != scheme)
        return fail(error, QStringLiteral("현재 프로필의 완료 기록 또는 식별 방식이 바뀌었습니다."));
    *hash = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
    return true;
}
bool restoreOutputProof(const ProfileConversionReceipt& receipt, const Manifest& manifest, QString* error) {
    const auto record = receipt.evidence.value("restoredBackup").toObject();
    const auto name = record.value("name").toString();
    const auto sourceBytes = record.value("sourceBytes").toInteger(-1);
    const auto cipherBytes = record.value("cipherBytes").toInteger(-1);
    if (!receipt.verified || !profile_upgrade_detail::supportedScheme(receipt.identityScheme)
        || name != profile_upgrade_detail::CurrentVaultName
        || !manifest.contains(name) || manifest.value(name).directory()
        || !validHash(record.value("sourceDigest").toString())
        || sourceBytes <= 0 || record.value("sourceBytes") != QJsonValue(sourceBytes)
        || cipherBytes <= 0 || record.value("cipherBytes") != QJsonValue(cipherBytes)
        || cipherBytes != qint64(manifest.value(name).identity.st_size)
        || record.value("cipherDigest").toString() != QString::fromLatin1(manifest.value(name).hash))
        return fail(error, QStringLiteral("인증된 백업과 임시 금고의 검증 증거가 일치하지 않습니다."));
    return true;
}
bool removeOwnedControlLock(SessionState& io, QString* error) {
    struct stat identity{}; QByteArray hash;
    bool missing = false;
    if (!relativeStat(io.control.get(), SourceLockName, &identity, &missing, error)) return false;
    if (missing) return true;
    if (!hashFileAt(io.control.get(), SourceLockName, &identity, &hash, true, error, true)
        || !sameInode(identity, io.lockIdentity) || hash != io.lockHash
        || (identity.st_nlink < 1 || identity.st_nlink > 3))
        return fail(error, QStringLiteral("다른 실행 잠금 링크를 보존했습니다."));
    if (::unlinkat(io.control.get(), SourceLockName, 0) != 0 || !syncFd(io.control.get(), error))
        return fail(error, QStringLiteral("자기 복원 잠금 링크를 정리하지 못했습니다."));
    return true;
}
struct RestoreState {
    SessionState io;
    QLockFile* borrower = nullptr;
    ProfileReady selected;
    ProfileConversionReceipt receipt;
    QString archivePath;
    bool keepControl = false;
    bool restored = false;
    bool prepared = false;
    ~RestoreState() {
        // SessionState normally owns the source hardlink. A restore preparation
        // only borrows the app's ordinary lock and must never unlink that source.
        io.ownOldLink = false;
        if (!io.ownsControl) return;
        if (keepControl) { io.ownsControl = false; return; }
        QString error;
        const auto proof = sessionProof(io);
        QStringList names;
        bool owned = boundDirectory(io.controlPath, io.control.get(), io.controlIdentity, true, &error)
            && ownerProofMatches(io.control.get(), proof.owner, proof.ownerHash, &error)
            && stageProofMatches(io.control.get(), proof.stage, true, &error)
            && knownControlNames(io.control.get(), &names, &error);
        if (owned && !removeOwnedControlLock(io, &error)) owned = false;
        if (owned && !removeControl(io.parent.get(), io.controlName, io.control.get(),
                                   io.controlIdentity, proof, &error)) owned = false;
        if (!owned) addWarning(io, error);
        io.ownsControl = false;
    }
};
bool preparationBound(RestoreState& state, QString* error) {
    if (!state.borrower || !state.borrower->isLocked())
        return fail(error, QStringLiteral("현재 앱의 프로필 실행 잠금이 해제되어 복원 준비를 멈췄습니다."));
    return boundSession(state.io, true, error);
}
bool initializeRestore(RestoreState& state, const ProfileReady& current, QLockFile& instance,
                       const QString& version, QString* error) {
    const auto paths = profileRestorePaths(current.directory);
    if (paths.currentDirectory.isEmpty() || paths.currentDirectory != current.directory
        || version.trimmed().isEmpty() || !profile_upgrade_detail::supportedScheme(current.identityScheme)
        || !instance.isLocked())
        return fail(error, QStringLiteral("잠금을 보유한 완료된 현재 프로필만 복원 준비할 수 있습니다."));
    auto& io = state.io;
    state.selected = current; state.borrower = &instance;
    io.paths = {QFileInfo(current.directory).absolutePath(), current.directory,
                current.directory, paths.transactionDirectory};
    io.version = version; io.upgrade = true; io.restoring = true;
    io.completionMarker = profile_upgrade_detail::ActiveMarkerName;
    io.parentPath = QFileInfo(current.directory).absolutePath();
    io.source = openDirectory(current.directory, nullptr, error);
    io.parent = openDirectory(io.parentPath, nullptr, error);
    if (!io.source || !privateDirectoryFd(io.source.get(), &io.sourceIdentity, error)
        || !io.parent || ::fstat(io.parent.get(), &io.parentIdentity) != 0
        || !safeNode(io.parentIdentity, true) || io.parentIdentity.st_dev != io.sourceIdentity.st_dev)
        return fail(error, QStringLiteral("현재 프로필과 복원 폴더가 안전한 같은 볼륨에 있지 않습니다."));
    struct stat pending{}; bool missing = false;
    const auto fixedName = QFile::encodeName(QFileInfo(paths.transactionDirectory).fileName());
    if (!relativeStat(io.parent.get(), fixedName, &pending, &missing, error)) return false;
    if (!missing) return fail(error, QStringLiteral("이전 복원 준비 기록이 있어 새 작업으로 덮어쓰지 않았습니다."));
    QString markerHash;
    if (!currentRestoreMarker(io.source.get(), current.identityScheme, &markerHash, error)) return false;
    if (!hashFileAt(io.source.get(), LegacyLockName, &io.lockIdentity, &io.lockHash, true, error)
        || io.lockIdentity.st_nlink != 1)
        return fail(error, QStringLiteral("현재 프로필의 단일 실행 잠금을 확인할 수 없습니다."));
    qint64 pid = 0, listedPid = 0; QString host, app, listedHost, listedApp;
    QLockFile listed(QDir(current.directory).filePath(QString::fromLatin1(LegacyLockName)));
    if (!instance.getLockInfo(&pid, &host, &app) || pid != qint64(::getpid())
        || host.isEmpty() || app.isEmpty()
        || !listed.getLockInfo(&listedPid, &listedHost, &listedApp)
        || pid != listedPid || host != listedHost || app != listedApp)
        return fail(error, QStringLiteral("현재 앱이 보유한 프로필 실행 잠금이 아닙니다."));
    Fd check(::openat(io.source.get(), LegacyLockName, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat checked{};
    if (!check || ::fstat(check.get(), &checked) != 0 || !sameInode(checked, io.lockIdentity))
        return fail(error, QStringLiteral("현재 실행 잠금의 정체성이 바뀌었습니다."));
    if (::flock(check.get(), LOCK_EX | LOCK_NB) == 0) {
        ::flock(check.get(), LOCK_UN);
        return fail(error, QStringLiteral("현재 앱의 네이티브 실행 잠금이 유지되지 않습니다."));
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN)
        return fail(error, QStringLiteral("네이티브 실행 잠금을 확인할 수 없습니다."));
    if (!closeChecked(check, error)) return false;
    // Capture the whole public source before publishing any control. The app's
    // existing lock is a regular single-link file at this point; remove only
    // that authenticated lock entry from the persistent source snapshot.
    if (!scanRoot(io.source.get(), {}, &io.original, error)) return false;
    const auto instanceEntry = io.original.value(QString::fromLatin1(LegacyLockName));
    if (!sameInode(instanceEntry.identity, io.lockIdentity) || instanceEntry.hash != io.lockHash)
        return fail(error, QStringLiteral("현재 실행 잠금이 원본 검사 중 바뀌었습니다."));
    io.original.remove(QString::fromLatin1(LegacyLockName));
    if (!noEncryptedArtifacts(io.original, error)
        || QString::fromLatin1(io.original.value(profile_upgrade_detail::ActiveMarkerName).hash) != markerHash)
        return fail(error, QStringLiteral("현재 프로필이 비어 있지 않거나 완료 기록이 바뀌었습니다."));
    const bool absent = !io.original.contains(QStringLiteral("KEEFI"));
    QTemporaryDir temporary(QDir(io.parentPath).filePath(QStringLiteral(".keefi-profile-restore-XXXXXX")));
    if (!temporary.isValid()) return fail(error, QStringLiteral("비공개 복원 작업 폴더를 만들 수 없습니다."));
    io.controlPath = temporary.path();
    io.controlName = QFile::encodeName(QFileInfo(io.controlPath).fileName());
    io.control.reset(::openat(io.parent.get(), io.controlName.constData(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!io.control || !privateDirectoryFd(io.control.get(), &io.controlIdentity, error)) return false;
    temporary.setAutoRemove(false); io.ownsControl = true;
    if (::mkdirat(io.control.get(), StageName, 0700) != 0)
        return fail(error, QStringLiteral("비공개 복원 사본 폴더를 만들 수 없습니다."));
    io.stagePath = QDir(io.controlPath).filePath(QString::fromLatin1(StageName));
    io.stage.reset(::openat(io.control.get(), StageName, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!io.stage || !privateDirectoryFd(io.stage.get(), &io.stageIdentity, error)) return false;
    if (io.stageIdentity.st_gid != io.sourceIdentity.st_gid
        && ::fchown(io.stage.get(), uid_t(-1), io.sourceIdentity.st_gid) != 0)
        return fail(error, QStringLiteral("복원 사본 폴더의 그룹을 보존하지 못했습니다."));
    if (!privateDirectoryFd(io.stage.get(), &io.stageIdentity, error)
        || !syncFd(io.stage.get(), error) || !syncFd(io.control.get(), error)) return false;
    io.nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    state.archivePath = QDir(io.parentPath).filePath(QStringLiteral(".keefi-profile-before-restore-") + io.nonce);
    io.owner = {{"format", RestoreOwnerFormat}, {"phase", "initial"}, {"source", current.directory},
        {"target", current.directory}, {"archive", state.archivePath}, {"version", version}, {"nonce", io.nonce},
        {"pid", pid}, {"host", host}, {"app", app}, {"source_root", directoryProof(io.sourceIdentity)},
        {"stage", directoryProof(io.stageIdentity)}, {"lock", restoreLockProof(io.lockIdentity, io.lockHash, pid, host, app)},
        {"source_identity_scheme", current.identityScheme}, {"source_marker_sha256", markerHash},
        {"settings_directory_absent", absent},
        {"source_protected_sha256", identityManifestDigest(io.original, true, absent)}};
    if (!writeOwnerRecord(io, false, error)
        || !boundDirectory(io.parentPath, io.parent.get(), io.parentIdentity, false, error)
        || !boundDirectory(current.directory, io.source.get(), io.sourceIdentity, true, error)
        || !exclusiveRename(io.parent.get(), io.controlName, io.parent.get(), fixedName, error))
        return false;
    io.controlName = fixedName; io.controlPath = paths.transactionDirectory;
    io.stagePath = QDir(io.controlPath).filePath(QString::fromLatin1(StageName));
    if (!syncFd(io.parent.get(), error)) return false;
    // Link the already-held ordinary app lock into the control. This does not
    // acquire or replace another lock and leaves the source pathname untouched.
    if (::linkat(io.source.get(), LegacyLockName, io.control.get(), SourceLockName, 0) != 0)
        return fail(error, QStringLiteral("현재 실행 잠금을 복원 작업과 결속하지 못했습니다."));
    io.ownOldLink = true; // Used only by boundSession; RestoreState never removes it.
    if (!syncFd(io.control.get(), error) || !preparationBound(state, error)) return false;
    if (!sourceUnchanged(io, error)) return false;
    io.expected = io.original;
    io.expected.remove(profile_upgrade_detail::ActiveMarkerName);
    io.owner["phase"] = QStringLiteral("preparing");
    io.owner["settings_directory_absent"] = absent;
    io.owner["source_protected_sha256"] = identityManifestDigest(io.original, true, absent);
    return writeOwnerRecord(io, true, error);
}
} // namespace

struct ProfileRestoreSession::Private : RestoreState {};
ProfileRestoreSession::ProfileRestoreSession(std::unique_ptr<Private> state) : d_(std::move(state)) {}
ProfileRestoreSession::~ProfileRestoreSession() = default;
std::unique_ptr<ProfileRestoreSession> ProfileRestoreSession::begin(
    const ProfileReady& current, QLockFile& instance, const QString& version, QString* error) {
    clear(error);
    auto state = std::make_unique<Private>();
    if (!initializeRestore(*state, current, instance, version, error)) return {};
    return std::unique_ptr<ProfileRestoreSession>(new ProfileRestoreSession(std::move(state)));
}
bool ProfileRestoreSession::copy(QString* error) {
    clear(error);
    if (!d_ || !preparationBound(*d_, error)) return false;
    return copySessionFiles(d_->io, error);
}
bool ProfileRestoreSession::restore(const QString& backup, const QString& password,
                                    const ProfileBackupRestorer& restorer, QString* error) {
    clear(error);
    if (!d_ || d_->io.failed || !d_->io.copied || d_->restored || d_->prepared || !restorer)
        return fail(error, QStringLiteral("빈 프로필의 확인한 복사본만 백업으로 복원할 수 있습니다."));
    auto& state = *d_; auto& io = state.io;
    const auto failed = [&] { io.failed = true; return false; };
    if (!preparationBound(state, error) || !sourceUnchanged(io, error) || !stageMatches(io, false, error))
        return failed();
    Manifest before;
    if (!scanRoot(io.stage.get(), {}, &before, error)) return failed();
    state.receipt = {};
    if (!restorer(io.stagePath, backup, password, &state.receipt, error)) {
        if (!error || error->isEmpty()) fail(error, QStringLiteral("백업을 인증하거나 복원하지 못했습니다."));
        return failed();
    }
    Manifest after;
    if (!preparationBound(state, error) || !sourceUnchanged(io, error)
        || !scanRoot(io.stage.get(), {}, &after, error)
        || !restoreOutputProof(state.receipt, after, error)) return failed();
    auto originalPublic = after;
    originalPublic.remove(profile_upgrade_detail::CurrentVaultName);
    if (!sameSource(before, originalPublic, error) || !synchronizeManifest(io.stage.get(), after, error))
        return failed();
    io.expected = std::move(after); io.convertedSnapshot = true;
    state.restored = true;
    return true;
}
bool ProfileRestoreSession::verify(QString* error) {
    clear(error);
    if (!d_ || d_->io.failed || !d_->restored || d_->io.verified || d_->prepared)
        return fail(error, QStringLiteral("인증된 백업 사본만 검증할 수 있습니다."));
    auto& state = *d_; auto& io = state.io;
    const auto failed = [&] { io.failed = true; return false; };
    if (!preparationBound(state, error) || !sourceUnchanged(io, error) || !stageMatches(io, false, error))
        return failed();
    qint64 count = 0, bytes = 0; QString digest;
    if (!manifestEvidence(io.original, &count, &bytes, &digest, error)) return failed();
    const auto backupProof = state.receipt.evidence.value("restoredBackup").toObject();
    const QJsonObject marker{{"format", profile_upgrade_detail::ActiveMarkerFormat},
        {"identity_scheme", state.receipt.identityScheme}, {"version", io.version}, {"verified", true},
        {"source_kind", "backup-restore"}, {"source_manifest_sha256", digest},
        {"completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"restore_id", io.nonce}, {"backup_sha256", backupProof.value("sourceDigest")}};
    if (!writeAtomicJson(io.stage.get(), QFile::encodeName(profile_upgrade_detail::ActiveMarkerName),
                         marker, false, &io.markerBytes, error)) return failed();
    QJsonObject actual; QByteArray raw;
    if (!readJsonAt(io.stage.get(), QFile::encodeName(profile_upgrade_detail::ActiveMarkerName), &actual, error, &raw)
        || raw != io.markerBytes || actual != marker || !profile_upgrade_detail::activeMarker(actual, error))
        return failed();
    io.verified = true;
    return true;
}
bool ProfileRestoreSession::prepareForRestart(QString* error) {
    clear(error);
    if (!d_ || d_->io.failed || !d_->io.verified || d_->prepared)
        return fail(error, QStringLiteral("검증이 끝난 백업만 다음 실행을 위해 준비할 수 있습니다."));
    auto& state = *d_; auto& io = state.io;
    const auto failed = [&] { io.failed = true; return false; };
    if (!preparationBound(state, error) || !sourceUnchanged(io, error) || !stageMatches(io, true, error))
        return failed();
    Manifest stage;
    if (!scanRoot(io.stage.get(), {}, &stage, error) || !restoreOutputProof(state.receipt, stage, error))
        return failed();
    io.owner["phase"] = QStringLiteral("prepared");
    io.owner["restored_identity_scheme"] = state.receipt.identityScheme;
    io.owner["restored_backup"] = state.receipt.evidence.value("restoredBackup");
    io.owner["restored_marker_sha256"] = QString::fromLatin1(QCryptographicHash::hash(io.markerBytes, QCryptographicHash::Sha256).toHex());
    io.owner["stage_protected_sha256"] = identityManifestDigest(stage, true, true);
    io.owner["stage_manifest_sha256"] = identityManifestDigest(stage, false, false);
    if (!synchronizeManifest(io.stage.get(), stage, error) || !writeOwnerRecord(io, true, error)
        || !syncFd(io.control.get(), error) || !syncFd(io.parent.get(), error)) return failed();
    state.prepared = true; state.keepControl = true;
    return true;
}
QString ProfileRestoreSession::stagingDirectory() const { return d_ ? d_->io.stagePath : QString{}; }
QString ProfileRestoreSession::archiveDirectory() const { return d_ ? d_->archivePath : QString{}; }

namespace {
struct RestoreCompletionState {
    SessionState io;
    ProfileReady selected;
    ProfileConversionReceipt receipt;
    Fd nativeLock;
    QString archivePath;
    QString cleanupBlocked;
    Manifest sourceSnapshot, stageSnapshot;
    bool restoredActive = false;
    bool aborted = false;
    bool refreshed = false;
    bool verified = false;
    bool archived = false;
    bool controlRemoved = false;
    ~RestoreCompletionState() {
        // Completion never owns a QLockFile object and never uses the ordinary
        // copy destructor to delete a prepared stage or an archived original.
        io.ownOldLink = false;
        io.ownsControl = false;
    }
};
bool restoreOwnerCommon(const QJsonObject& owner, const ProfileRestorePaths& paths, QString* error) {
    static const QStringList phases{QStringLiteral("initial"), QStringLiteral("preparing"),
        QStringLiteral("prepared"), QStringLiteral("refreshing"), QStringLiteral("ready-to-swap"),
        QStringLiteral("swapped"), QStringLiteral("archived")};
    static const QRegularExpression noncePattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    const auto nonce = owner.value("nonce").toString();
    const auto pid = owner.value("pid").toInteger(-1);
    const auto archive = QDir(QFileInfo(paths.currentDirectory).absolutePath()).filePath(
        QStringLiteral(".keefi-profile-before-restore-") + nonce);
    const auto lock = owner.value("lock").toObject();
    const auto mode = lock.value("mode").toInteger(-1);
    const auto gid = lock.value("gid").toInteger(-1);
    bool deviceOk = false, inodeOk = false;
    const auto device = lock.value("device").toString().toULongLong(&deviceOk);
    const auto inode = lock.value("inode").toString().toULongLong(&inodeOk);
    if (owner.value("format").toString() != RestoreOwnerFormat
        || !phases.contains(owner.value("phase").toString()) || !noncePattern.match(nonce).hasMatch()
        || owner.value("source").toString() != paths.currentDirectory
        || owner.value("target").toString() != paths.currentDirectory
        || owner.value("archive").toString() != archive || owner.value("version").toString().isEmpty()
        || pid <= 0 || pid > std::numeric_limits<pid_t>::max() || owner.value("pid") != QJsonValue(pid)
        || owner.value("host").toString().isEmpty() || owner.value("app").toString().isEmpty()
        || !owner.value("settings_directory_absent").isBool()
        || !profile_upgrade_detail::supportedScheme(owner.value("source_identity_scheme").toString())
        || !validHash(owner.value("source_marker_sha256").toString())
        || !validHash(owner.value("source_protected_sha256").toString())
        || !deviceOk || !inodeOk || inode == 0
        || QString::number(device) != lock.value("device").toString()
        || QString::number(inode) != lock.value("inode").toString()
        || lock.value("uid") != QJsonValue(qint64(::geteuid()))
        || mode < 0 || mode > 0777 || lock.value("mode") != QJsonValue(mode)
        || gid < 0 || quint64(gid) > std::numeric_limits<gid_t>::max() || lock.value("gid") != QJsonValue(gid)
        || !validHash(lock.value("sha256").toString()) || lock.value("pid") != owner.value("pid")
        || lock.value("host") != owner.value("host") || lock.value("app") != owner.value("app"))
        return fail(error, QStringLiteral("복원 작업의 소유 기록 또는 경로 결속이 올바르지 않습니다."));
    if (!validDirectoryProof(owner.value("source_root").toObject(), error)
        || !validDirectoryProof(owner.value("stage").toObject(), error)) return false;
    const auto phase = owner.value("phase").toString();
    if (phase != QStringLiteral("initial") && phase != QStringLiteral("preparing")) {
        if (!profile_upgrade_detail::supportedScheme(owner.value("restored_identity_scheme").toString())
            || !validHash(owner.value("restored_marker_sha256").toString())
            || !validHash(owner.value("stage_protected_sha256").toString())
            || !validHash(owner.value("stage_manifest_sha256").toString())
            || !owner.value("restored_backup").isObject())
            return fail(error, QStringLiteral("준비한 백업의 검증 기록이 완전하지 않습니다."));
    }
    return true;
}
bool restoreNamedLock(int directory, const QByteArray& name, const SessionState& io,
                      bool allowMissing, bool* exists, QString* error) {
    struct stat named{}, actual{};
    bool missing = false;
    QByteArray hash;
    if (!relativeStat(directory, name, &named, &missing, error)) return false;
    if (exists) *exists = !missing;
    if (missing) return allowMissing || fail(error, QStringLiteral("복원 실행 잠금이 사라졌습니다."));
    if (!sameInode(named, io.lockIdentity)
        || !hashFileAt(directory, name, &actual, &hash, true, error, true)
        || !sameInode(actual, io.lockIdentity) || hash != io.lockHash
        || actual.st_uid != io.lockIdentity.st_uid || actual.st_gid != io.lockIdentity.st_gid
        || actual.st_mode != io.lockIdentity.st_mode)
        return fail(error, QStringLiteral("다른 실행 잠금을 보존하며 복원을 멈췄습니다."));
    return true;
}
bool completionControlBound(RestoreCompletionState& state, QString* error) {
    auto& io = state.io;
    QStringList names;
    return boundDirectory(io.parentPath, io.parent.get(), io.parentIdentity, false, error)
        && boundDirectory(io.controlPath, io.control.get(), io.controlIdentity, true, error)
        && ownerProofMatches(io.control.get(), &io.ownerIdentity, &io.ownerHash, error)
        && knownControlNames(io.control.get(), &names, error);
}
bool completionBeforeBound(RestoreCompletionState& state, QString* error) {
    auto& io = state.io;
    return completionControlBound(state, error)
        && boundDirectory(io.paths.currentDirectory, io.source.get(), io.sourceIdentity, true, error)
        && boundDirectory(io.stagePath, io.stage.get(), io.stageIdentity, true, error)
        && directoryProof(io.sourceIdentity) == io.owner.value("source_root").toObject()
        && directoryProof(io.stageIdentity) == io.owner.value("stage").toObject()
        && restoreNamedLock(io.control.get(), SourceLockName, io, false, nullptr, error)
        && restoreNamedLock(io.source.get(), LegacyLockName, io, false, nullptr, error);
}
bool restoreTree(int directory, const SessionState& io, Manifest* manifest, QString* error) {
    const ScanRules rules{true, false, &io.lockIdentity, &io.lockHash, false,
        profile_upgrade_detail::ActiveMarkerName, true, true, true, true};
    return scanRoot(directory, rules, manifest, error);
}
bool restoreSourceSnapshot(RestoreCompletionState& state, Manifest* source, QString* error) {
    auto& io = state.io;
    QString markerHash;
    return restoreTree(io.source.get(), io, source, error)
        && noEncryptedArtifacts(*source, error)
        && currentRestoreMarker(io.source.get(), io.owner.value("source_identity_scheme").toString(), &markerHash, error)
        && (markerHash == io.owner.value("source_marker_sha256").toString()
            || fail(error, QStringLiteral("원래 프로필의 완료 기록이 바뀌었습니다.")))
        && (identityManifestDigest(*source, true, io.owner.value("settings_directory_absent").toBool())
                == io.owner.value("source_protected_sha256").toString()
            || fail(error, QStringLiteral("공개 설정 이외의 원래 프로필 자료가 바뀌어 교체하지 않았습니다.")));
}
bool restoredMarkerProof(RestoreCompletionState& state, int directory, QString* error) {
    auto& io = state.io;
    QJsonObject marker; QByteArray raw;
    if (!readJsonAt(directory, QFile::encodeName(profile_upgrade_detail::ActiveMarkerName), &marker, error, &raw)
        || !profile_upgrade_detail::activeMarker(marker, error)
        || marker.value("identity_scheme").toString() != state.receipt.identityScheme
        || marker.value("source_kind").toString() != QStringLiteral("backup-restore")
        || marker.value("restore_id").toString() != io.nonce
        || QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex())
            != io.owner.value("restored_marker_sha256").toString())
        return fail(error, QStringLiteral("복원 사본의 완료 기록과 소유 증거가 일치하지 않습니다."));
    return true;
}
bool restoreStageSnapshot(RestoreCompletionState& state, bool fullPrepared, Manifest* stage, QString* error) {
    auto& io = state.io;
    if (!restoreTree(io.stage.get(), io, stage, error)
        || !restoreOutputProof(state.receipt, *stage, error)
        || !restoredMarkerProof(state, io.stage.get(), error)) return false;
    const auto expected = io.owner.value(fullPrepared ? "stage_manifest_sha256" : "stage_protected_sha256").toString();
    if (identityManifestDigest(*stage, !fullPrepared, !fullPrepared) != expected)
        return fail(error, QStringLiteral("검증한 복원 사본의 파일 또는 권한이 바뀌었습니다."));
    return true;
}
bool completionLock(RestoreCompletionState& state, bool incomplete, QString* error) {
    auto& io = state.io;
    struct stat listed{};
    bool missing = false;
    if (!relativeStat(io.control.get(), SourceLockName, &listed, &missing, error)) return false;
    int lockParent = io.control.get();
    QByteArray lockName(SourceLockName);
    QString lockPath = QDir(io.controlPath).filePath(QString::fromLatin1(SourceLockName));
    if (missing) {
        if (!incomplete && !state.restoredActive)
            return fail(error, QStringLiteral("준비한 복원의 제어 잠금이 사라졌습니다."));
        if (!io.source) return true;
        if (!relativeStat(io.source.get(), LegacyLockName, &listed, &missing, error)) return false;
        if (missing) return true;
        lockParent = io.source.get(); lockName = LegacyLockName;
        lockPath = QDir(state.restoredActive && state.archived ? state.archivePath
            : state.restoredActive ? io.stagePath : io.paths.currentDirectory).filePath(QString::fromLatin1(LegacyLockName));
    }
    struct stat identity{}; QByteArray hash;
    const auto metadata = io.owner.value("lock").toObject();
    if (!hashFileAt(lockParent, lockName, &identity, &hash, true, error, true)
        || !identityMatches(metadata, identity, hash, error)
        || metadata.value("gid") != QJsonValue(qint64(identity.st_gid))
        || metadata.value("mode") != QJsonValue(qint64(identity.st_mode & 07777)))
        return false;
    Fd lock(::openat(lockParent, lockName.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat actual{};
    if (!lock || ::fstat(lock.get(), &actual) != 0 || !sameInode(actual, identity)
        || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0)
        return fail(error, QStringLiteral("프로필이 실행 중이거나 다른 복원이 진행 중입니다."));
    QLockFile info(lockPath);
    qint64 pid = 0; QString host, app;
    if (!info.getLockInfo(&pid, &host, &app) || pid != metadata.value("pid").toInteger()
        || host != metadata.value("host").toString() || app != metadata.value("app").toString())
        return fail(error, QStringLiteral("중단된 실행 잠금의 주체를 확인할 수 없습니다."));
    io.lockIdentity = identity; io.lockHash = hash;
    state.nativeLock = std::move(lock);
    if (state.restoredActive) return true;
    if (lockParent != io.control.get()) {
        if (::linkat(io.source.get(), LegacyLockName, io.control.get(), SourceLockName, 0) != 0
            || !syncFd(io.control.get(), error))
            return fail(error, QStringLiteral("중단된 복원 제어 잠금을 다시 결속하지 못했습니다."));
    }
    bool sourceExists = false;
    if (!restoreNamedLock(io.source.get(), LegacyLockName, io, true, &sourceExists, error)) return false;
    if (!sourceExists) {
        // A normal QLockFile destructor removes the app pathname. The retained
        // control link authenticates the same dead owner's inode for restart.
        if (::linkat(io.control.get(), SourceLockName, io.source.get(), LegacyLockName, 0) != 0
            || !syncFd(io.source.get(), error))
            return fail(error, QStringLiteral("정상 종료 후의 프로필 잠금을 안전하게 다시 연결하지 못했습니다."));
    }
    bool stageExists = false;
    if (!restoreNamedLock(io.stage.get(), LegacyLockName, io, true, &stageExists, error)
        || ::fstat(state.nativeLock.get(), &actual) != 0
        || actual.st_nlink != nlink_t(stageExists ? 3 : 2))
        return fail(error, QStringLiteral("알 수 없는 실행 잠금 링크가 있어 복원을 멈췄습니다."));
    return true;
}
bool abortIncompleteRestore(RestoreCompletionState& state, QString* error) {
    auto& io = state.io;
    Manifest source;
    if (!completionControlBound(state, error)
        || !boundDirectory(io.paths.currentDirectory, io.source.get(), io.sourceIdentity, true, error)
        || !stageProofMatches(io.control.get(), io.owner.value("stage").toObject(), false, error)
        || !restoreSourceSnapshot(state, &source, error)) return false;
    if (!removeOwnedControlLock(io, error)) return false;
    const CleanupProof proof{io.owner.value("stage").toObject(), &io.ownerIdentity, &io.ownerHash,
        io.lockIdentity.st_ino ? &io.lockIdentity : nullptr, io.lockIdentity.st_ino ? &io.lockHash : nullptr};
    if (!removeControl(io.parent.get(), io.controlName, io.control.get(), io.controlIdentity, proof, error)) return false;
    state.controlRemoved = true; state.aborted = true;
    return true;
}
bool initializeRestoreCompletion(RestoreCompletionState& state, const ProfileReady& selected,
                                 const QString& version, QString* error) {
    auto& io = state.io;
    const auto paths = profileRestorePaths(selected.directory);
    if (paths.currentDirectory.isEmpty() || paths.currentDirectory != selected.directory
        || version.trimmed().isEmpty() || !profile_upgrade_detail::supportedScheme(selected.identityScheme))
        return fail(error, QStringLiteral("완료할 복원의 프로필 선택이 올바르지 않습니다."));
    state.selected = selected; io.warning = selected.warning; io.version = version;
    io.paths = {QFileInfo(selected.directory).absolutePath(), selected.directory,
                selected.directory, paths.transactionDirectory};
    io.parentPath = QFileInfo(selected.directory).absolutePath();
    io.controlPath = paths.transactionDirectory;
    io.controlName = QFile::encodeName(QFileInfo(io.controlPath).fileName());
    io.stagePath = QDir(io.controlPath).filePath(QString::fromLatin1(StageName));
    io.parent = openDirectory(io.parentPath, nullptr, error);
    io.control = openDirectory(io.controlPath, nullptr, error);
    if (!io.parent || ::fstat(io.parent.get(), &io.parentIdentity) != 0
        || !safeNode(io.parentIdentity, true) || !io.control
        || !privateDirectoryFd(io.control.get(), &io.controlIdentity, error)
        || ::flock(io.control.get(), LOCK_EX | LOCK_NB) != 0
        || !boundDirectory(io.controlPath, io.control.get(), io.controlIdentity, true, error))
        return fail(error, QStringLiteral("복원 제어 폴더를 잠그거나 확인할 수 없습니다."));
    QByteArray ownerBytes;
    if (!readJsonAt(io.control.get(), OwnerName, &io.owner, error, &ownerBytes)
        || !restoreOwnerCommon(io.owner, paths, error) || !confirmedDead(io.owner, error)
        || !hashFileAt(io.control.get(), OwnerName, &io.ownerIdentity, &io.ownerHash, false, error)
        || (io.ownerIdentity.st_mode & 0777) != 0600
        || io.ownerHash != QCryptographicHash::hash(ownerBytes, QCryptographicHash::Sha256).toHex())
        return false;
    io.ownerProofValid = true; io.nonce = io.owner.value("nonce").toString();
    state.archivePath = io.owner.value("archive").toString();
    const auto phase = io.owner.value("phase").toString();
    const bool incomplete = phase == QStringLiteral("initial") || phase == QStringLiteral("preparing");
    state.receipt = {true, io.owner.value("restored_identity_scheme").toString(),
                     {{"restoredBackup", io.owner.value("restored_backup")}}};
    auto current = openDirectory(selected.directory, nullptr, error);
    struct stat currentIdentity{};
    if (!current || !privateDirectoryFd(current.get(), &currentIdentity, error)
        || currentIdentity.st_dev != io.controlIdentity.st_dev
        || currentIdentity.st_dev != io.parentIdentity.st_dev)
        return fail(error, QStringLiteral("복원 대상 폴더가 바뀌었거나 다른 볼륨입니다."));
    const auto currentProof = directoryProof(currentIdentity);
    state.restoredActive = currentProof == io.owner.value("stage").toObject();
    if (!state.restoredActive && currentProof != io.owner.value("source_root").toObject())
        return fail(error, QStringLiteral("원래 프로필 또는 복원 사본의 폴더 정체성이 바뀌었습니다."));
    if (state.restoredActive) {
        if (incomplete || selected.identityScheme != state.receipt.identityScheme)
            return fail(error, QStringLiteral("게시된 복원 프로필의 식별 방식을 확인할 수 없습니다."));
        io.stage = std::move(current); io.stageIdentity = currentIdentity;
        if (!restoredMarkerProof(state, io.stage.get(), error)) return false;
        // Publication is irreversible. Do not compare active vault bytes with
        // prepared ciphertext after ordinary application writes have begun.
        io.committed = true;
    } else {
        if (selected.identityScheme != io.owner.value("source_identity_scheme").toString())
            return fail(error, QStringLiteral("원래 프로필의 식별 방식이 바뀌었습니다."));
        io.source = std::move(current); io.sourceIdentity = currentIdentity;
    }
    QString localError;
    QStringList names;
    if (!knownControlNames(io.control.get(), &names, &localError)) {
        if (!state.restoredActive) return fail(error, localError);
        state.cleanupBlocked = localError; addWarning(io, localError); return true;
    }
    struct stat staged{}, archived{};
    bool stageMissing = false, archiveMissing = false;
    if (!relativeStat(io.control.get(), StageName, &staged, &stageMissing, &localError)
        || !relativeStat(io.parent.get(), QFile::encodeName(QFileInfo(state.archivePath).fileName()),
                         &archived, &archiveMissing, &localError)) {
        if (!state.restoredActive) return fail(error, localError);
        state.cleanupBlocked = localError; addWarning(io, localError); return true;
    }
    if (!state.restoredActive) {
        if (stageMissing || !safeNode(staged, true) || directoryProof(staged) != io.owner.value("stage").toObject())
            return fail(error, QStringLiteral("준비한 복원 폴더가 없어졌거나 다른 폴더로 바뀌었습니다."));
        io.stage = openDirectory(io.stagePath, nullptr, error);
        if (!io.stage || !privateDirectoryFd(io.stage.get(), &io.stageIdentity, error)
            || !sameInode(io.stageIdentity, staged)) return false;
    } else {
        QString originalPath;
        if (!stageMissing && safeNode(staged, true)
            && directoryProof(staged) == io.owner.value("source_root").toObject()) originalPath = io.stagePath;
        else if (stageMissing && !archiveMissing && safeNode(archived, true)
                 && directoryProof(archived) == io.owner.value("source_root").toObject()) {
            originalPath = state.archivePath; state.archived = true;
        } else {
            state.cleanupBlocked = QStringLiteral("게시된 프로필은 유지하며 확인할 수 없는 이전 폴더를 보존했습니다.");
            addWarning(io, state.cleanupBlocked); return true;
        }
        io.source = openDirectory(originalPath, nullptr, &localError);
        if (!io.source || !privateDirectoryFd(io.source.get(), &io.sourceIdentity, &localError)
            || directoryProof(io.sourceIdentity) != io.owner.value("source_root").toObject()) {
            state.cleanupBlocked = localError; addWarning(io, localError); return true;
        }
    }
    if (!completionLock(state, incomplete, &localError)) {
        if (!state.restoredActive) return fail(error, localError);
        state.cleanupBlocked = localError; addWarning(io, localError); return true;
    }
    if (state.restoredActive) return true;
    if (incomplete) return abortIncompleteRestore(state, error);
    Manifest source, stage;
    return completionBeforeBound(state, error) && restoreSourceSnapshot(state, &source, error)
        && restoreStageSnapshot(state, phase == QStringLiteral("prepared"), &stage, error);
}
} // namespace


namespace {
bool copyRestoreSetting(RestoreCompletionState& state, const QString& path,
                        const Manifest& source, QString* error) {
    auto& io = state.io;
    bool targetParentMissing = false;
    auto targetParent = relativeDirectory(io.stage.get(), parentPart(path), &targetParentMissing, error);
    if (!targetParent) return targetParentMissing && !source.contains(path);
    const auto name = leafPart(path);
    struct stat targetIdentity{};
    bool targetMissing = false;
    if (!relativeStat(targetParent.get(), name, &targetIdentity, &targetMissing, error)) return false;
    if (!targetMissing && !safeNode(targetIdentity, false))
        return fail(error, QStringLiteral("갱신할 임시 공개 설정의 링크 또는 종류가 안전하지 않습니다."));
    if (!source.contains(path)) {
        if (!targetMissing && ::unlinkat(targetParent.get(), name.constData(), 0) != 0)
            return fail(error, QStringLiteral("임시 사본에서 삭제된 공개 설정을 반영하지 못했습니다."));
        return syncFd(targetParent.get(), error) && closeChecked(targetParent, error);
    }
    const auto& expected = source.value(path);
    if (expected.directory()) return fail(error, QStringLiteral("공개 설정 경로가 일반 파일이 아닙니다."));
    auto sourceParent = relativeDirectory(io.source.get(), parentPart(path), nullptr, error);
    if (!sourceParent) return false;
    Fd inputFd(::openat(sourceParent.get(), name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat before{};
    if (!inputFd || ::fstat(inputFd.get(), &before) != 0 || !safeNode(before, false)
        || !stableFile(before, expected.identity))
        return fail(error, QStringLiteral("최신 공개 설정을 읽는 동안 원본이 바뀌었습니다."));
    // Only these three stage files are replaceable. The persistent refreshing
    // phase makes a partial write retryable without ignoring any other file.
    if (!targetMissing && ::unlinkat(targetParent.get(), name.constData(), 0) != 0)
        return fail(error, QStringLiteral("소유한 임시 공개 설정을 갱신하지 못했습니다."));
    Fd outputFd(::openat(targetParent.get(), name.constData(),
        O_WRONLY | O_CREAT | O_EXCL | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!outputFd) return fail(error, QStringLiteral("최신 공개 설정 사본을 만들지 못했습니다."));
    QFile input, output;
    if (!input.open(inputFd.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)
        || !output.open(outputFd.get(), QIODevice::WriteOnly, QFileDevice::DontCloseHandle)
        || !copyProfileStream(input, output, error)) return false;
    if (!output.flush() || output.error() != QFileDevice::NoError)
        return fail(error, QStringLiteral("최신 공개 설정을 완전히 기록하지 못했습니다."));
    struct stat written{}, after{};
    if (::fstat(outputFd.get(), &written) != 0 || !safeNode(written, false)
        || (written.st_gid != before.st_gid && ::fchown(outputFd.get(), uid_t(-1), before.st_gid) != 0)
        || ::fchmod(outputFd.get(), before.st_mode & 0777) != 0 || !syncFd(outputFd.get(), error)
        || ::fstat(inputFd.get(), &after) != 0 || !stableFile(before, after))
        return fail(error, QStringLiteral("공개 설정의 원본, 권한 또는 기록을 검증하지 못했습니다."));
    input.close(); output.close();
    return closeChecked(inputFd, error) && closeChecked(outputFd, error)
        && syncFd(targetParent.get(), error) && closeChecked(sourceParent, error)
        && closeChecked(targetParent, error);
}
bool refreshRestoreSettings(RestoreCompletionState& state, const Manifest& source, QString* error) {
    auto& io = state.io;
    const QByteArray settingsName("KEEFI");
    struct stat identity{};
    bool missing = false;
    if (!relativeStat(io.stage.get(), settingsName, &identity, &missing, error)) return false;
    const bool sourceHasDirectory = source.contains(QStringLiteral("KEEFI"));
    if (sourceHasDirectory && !source.value(QStringLiteral("KEEFI")).directory())
        return fail(error, QStringLiteral("원본 공개 설정 폴더가 올바르지 않습니다."));
    Fd settings;
    if (sourceHasDirectory || !missing) {
        if (missing && ::mkdirat(io.stage.get(), settingsName.constData(), 0700) != 0)
            return fail(error, QStringLiteral("임시 공개 설정 폴더를 만들지 못했습니다."));
        settings.reset(::openat(io.stage.get(), settingsName.constData(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat actual{};
        if (!settings || ::fstat(settings.get(), &actual) != 0 || !safeNode(actual, true)
            || (!missing && !sameInode(actual, identity)) || ::fchmod(settings.get(), 0700) != 0)
            return fail(error, QStringLiteral("임시 공개 설정 폴더를 안전하게 갱신할 수 없습니다."));
    }
    for (const auto& path : publicSettingFiles())
        if (!copyRestoreSetting(state, path, source, error)) return false;
    if (settings) {
        if (sourceHasDirectory) {
            const auto& expected = source.value(QStringLiteral("KEEFI")).identity;
            struct stat actual{};
            if (::fstat(settings.get(), &actual) != 0
                || (actual.st_gid != expected.st_gid && ::fchown(settings.get(), uid_t(-1), expected.st_gid) != 0)
                || ::fchmod(settings.get(), expected.st_mode & 0777) != 0 || !syncFd(settings.get(), error))
                return fail(error, QStringLiteral("공개 설정 폴더의 권한과 그룹을 보존하지 못했습니다."));
        } else {
            QStringList names;
            if (!namesAt(settings.get(), &names, error) || !names.isEmpty())
                return fail(error, QStringLiteral("공개 설정 이외의 파일이 있어 임시 폴더를 보존했습니다."));
            if (::unlinkat(io.stage.get(), settingsName.constData(), AT_REMOVEDIR) != 0)
                return fail(error, QStringLiteral("비워진 임시 설정 폴더를 정리하지 못했습니다."));
        }
        if (!closeChecked(settings, error)) return false;
    }
    return syncFd(io.stage.get(), error);
}
bool samePublicSettings(const Manifest& source, const Manifest& stage, QString* error) {
    Manifest expected, current;
    auto names = publicSettingFiles(); names.append(QStringLiteral("KEEFI"));
    for (const auto& name : names) {
        if (source.contains(name)) expected.insert(name, source.value(name));
        if (stage.contains(name)) current.insert(name, stage.value(name));
    }
    return sameCopy(expected, current, error);
}
bool exchangeRestoreDirectories(int firstDirectory, const QByteArray& first,
                                int secondDirectory, const QByteArray& second, QString* error) {
#ifdef Q_OS_MACOS
    const int result = ::renameatx_np(firstDirectory, first.constData(), secondDirectory, second.constData(), RENAME_SWAP);
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    const int result = int(::syscall(SYS_renameat2, firstDirectory, first.constData(),
                                    secondDirectory, second.constData(), RENAME_EXCHANGE));
#else
    Q_UNUSED(firstDirectory); Q_UNUSED(first); Q_UNUSED(secondDirectory); Q_UNUSED(second);
    return fail(error, QStringLiteral("이 시스템은 복원 프로필의 원자적 교환을 지원하지 않습니다."));
#endif
#if defined(Q_OS_MACOS) || (defined(Q_OS_LINUX) && defined(SYS_renameat2))
    return result == 0 || fail(error, QStringLiteral("원래 프로필과 검증한 복원 사본을 원자적으로 교환하지 못했습니다."));
#endif
}
bool archiveWarning(RestoreCompletionState& state, const QString& message, QString* error) {
    addWarning(state.io, message);
    return fail(error, message);
}
} // namespace

struct ProfileRestoreCompletion::Private : RestoreCompletionState {};
ProfileRestoreCompletion::ProfileRestoreCompletion(std::unique_ptr<Private> state) : d_(std::move(state)) {}
ProfileRestoreCompletion::~ProfileRestoreCompletion() = default;
std::unique_ptr<ProfileRestoreCompletion> ProfileRestoreCompletion::begin(
    const ProfileReady& selected, const QString& version, QString* error) {
    clear(error);
    auto state = std::make_unique<Private>();
    if (!initializeRestoreCompletion(*state, selected, version, error)) return {};
    return std::unique_ptr<ProfileRestoreCompletion>(new ProfileRestoreCompletion(std::move(state)));
}
bool ProfileRestoreCompletion::refreshPublicSettings(QString* error) {
    clear(error);
    if (!d_) return fail(error, QStringLiteral("완료할 복원 작업이 없습니다."));
    auto& state = *d_; auto& io = state.io;
    if (state.restoredActive || state.aborted) return true;
    if (io.failed || state.refreshed)
        return fail(error, QStringLiteral("이 복원의 공개 설정 갱신을 시작할 수 없습니다."));
    const auto failed = [&] { io.failed = true; return false; };
    Manifest source, stage;
    const bool fullPrepared = io.owner.value("phase").toString() == QStringLiteral("prepared");
    if (!completionBeforeBound(state, error) || !restoreSourceSnapshot(state, &source, error)
        || !restoreStageSnapshot(state, fullPrepared, &stage, error)) return failed();
    io.owner["phase"] = QStringLiteral("refreshing");
    if (!writeOwnerRecord(io, true, error) || !refreshRestoreSettings(state, source, error)) return failed();
    Manifest afterSource, afterStage;
    if (!completionBeforeBound(state, error) || !restoreSourceSnapshot(state, &afterSource, error)
        || !sameSource(source, afterSource, error)
        || !restoreStageSnapshot(state, false, &afterStage, error)
        || !samePublicSettings(afterSource, afterStage, error)) return failed();
    state.sourceSnapshot = std::move(afterSource); state.stageSnapshot = std::move(afterStage);
    state.refreshed = true;
    return true;
}
bool ProfileRestoreCompletion::verify(QString* error) {
    clear(error);
    if (!d_) return fail(error, QStringLiteral("검증할 복원 작업이 없습니다."));
    auto& state = *d_; auto& io = state.io;
    if (state.restoredActive || state.aborted) return true;
    if (io.failed || !state.refreshed || state.verified)
        return fail(error, QStringLiteral("공개 설정 갱신을 마친 복원 사본만 검증할 수 있습니다."));
    const auto failed = [&] { io.failed = true; return false; };
    Manifest source, stage;
    if (!completionBeforeBound(state, error) || !restoreSourceSnapshot(state, &source, error)
        || !sameSource(state.sourceSnapshot, source, error)
        || !restoreStageSnapshot(state, false, &stage, error)
        || !samePublicSettings(source, stage, error)
        || !synchronizeManifest(io.stage.get(), stage, error)) return failed();
    state.sourceSnapshot = std::move(source); state.stageSnapshot = std::move(stage);
    io.owner["phase"] = QStringLiteral("ready-to-swap");
    io.owner["ready_source_sha256"] = identityManifestDigest(state.sourceSnapshot, false, false);
    io.owner["ready_stage_sha256"] = identityManifestDigest(state.stageSnapshot, false, false);
    if (!writeOwnerRecord(io, true, error) || !syncFd(io.control.get(), error)) return failed();
    state.verified = true;
    return true;
}
bool ProfileRestoreCompletion::publish(QString* error) {
    clear(error);
    if (!d_) return fail(error, QStringLiteral("게시할 복원 작업이 없습니다."));
    auto& state = *d_; auto& io = state.io;
    if (state.restoredActive || state.aborted) return true;
    if (io.failed || !state.verified)
        return fail(error, QStringLiteral("검증이 끝난 복원 사본만 게시할 수 있습니다."));
    const auto failed = [&] { io.failed = true; return false; };
    Manifest source, stage;
    if (!completionBeforeBound(state, error) || !restoreSourceSnapshot(state, &source, error)
        || !sameSource(state.sourceSnapshot, source, error)
        || !restoreStageSnapshot(state, false, &stage, error)
        || !sameSource(state.stageSnapshot, stage, error)) return failed();
    bool stageLockExists = false;
    if (!restoreNamedLock(io.stage.get(), LegacyLockName, io, true, &stageLockExists, error)) return failed();
    if (!stageLockExists && ::linkat(io.control.get(), SourceLockName, io.stage.get(), LegacyLockName, 0) != 0) {
        fail(error, QStringLiteral("게시 이후에도 유지할 새 프로필 실행 잠금을 연결하지 못했습니다."));
        return failed();
    }
    struct stat lock{};
    if (!state.nativeLock || ::fstat(state.nativeLock.get(), &lock) != 0 || lock.st_nlink != 3
        || !restoreNamedLock(io.stage.get(), LegacyLockName, io, false, nullptr, error)
        || !completionBeforeBound(state, error) || !syncFd(io.stage.get(), error)
        || !syncFd(io.source.get(), error) || !syncFd(io.control.get(), error))
        return failed();
    if (!exchangeRestoreDirectories(io.control.get(), StageName, io.parent.get(),
                                    QFile::encodeName(QFileInfo(io.paths.currentDirectory).fileName()), error))
        return failed();
    // This successful exchange is the commit. No later error may swap back,
    // delete the active profile, or require its ciphertext to remain unchanged.
    state.restoredActive = true; io.committed = true;
    io.owner["phase"] = QStringLiteral("swapped");
    QString warning;
    if (!writeOwnerRecord(io, true, &warning)) addWarning(io, warning);
    warning.clear();
    if (!syncFd(io.parent.get(), &warning) || !syncFd(io.control.get(), &warning)) addWarning(io, warning);
    return true;
}
bool ProfileRestoreCompletion::archive(QString* error) {
    clear(error);
    if (!d_) return fail(error, QStringLiteral("정리할 복원 작업이 없습니다."));
    auto& state = *d_; auto& io = state.io;
    if (state.aborted || state.controlRemoved) return true;
    if (!state.restoredActive)
        return fail(error, QStringLiteral("게시하기 전에 원래 프로필을 옮기지 않습니다."));
    if (!state.cleanupBlocked.isEmpty()) return archiveWarning(state, state.cleanupBlocked, error);
    QString detail;
    if (!completionControlBound(state, &detail)
        || !boundDirectory(io.paths.currentDirectory, io.stage.get(), io.stageIdentity, true, &detail)
        || !restoredMarkerProof(state, io.stage.get(), &detail))
        return archiveWarning(state, detail.isEmpty() ? QStringLiteral("게시된 프로필의 남은 정리를 확인하지 못했습니다.") : detail, error);
    const auto archiveName = QFile::encodeName(QFileInfo(state.archivePath).fileName());
    struct stat existing{}, staged{};
    bool missing = false, stageMissing = false;
    if (!relativeStat(io.parent.get(), archiveName, &existing, &missing, &detail)
        || !relativeStat(io.control.get(), StageName, &staged, &stageMissing, &detail))
        return archiveWarning(state, detail, error);
    if (!state.archived) {
        if (!stageMissing && !missing)
            return archiveWarning(state, QStringLiteral("원래 프로필 보관 경로가 이미 있어 새 프로필은 유지하고 두 폴더를 보존했습니다."), error);
        if (stageMissing || !safeNode(staged, true)
            || directoryProof(staged) != io.owner.value("source_root").toObject()
            || !boundDirectory(io.stagePath, io.source.get(), io.sourceIdentity, true, &detail))
            return archiveWarning(state, QStringLiteral("원래 프로필의 정체성을 확인하지 못해 정리하지 않았습니다."), error);
        bool oldLinkExists = false;
        if (!restoreNamedLock(io.source.get(), LegacyLockName, io, true, &oldLinkExists, &detail))
            return archiveWarning(state, detail, error);
        if (oldLinkExists) {
            if (!state.nativeLock || ::unlinkat(io.source.get(), LegacyLockName, 0) != 0
                || !syncFd(io.source.get(), &detail))
                return archiveWarning(state, QStringLiteral("원래 프로필의 자기 실행 잠금을 안전하게 정리하지 못했습니다."), error);
        }
        if (!exclusiveRename(io.control.get(), StageName, io.parent.get(), archiveName, &detail))
            return archiveWarning(state, detail, error);
        state.archived = true;
        io.owner["phase"] = QStringLiteral("archived");
        if (!writeOwnerRecord(io, true, &detail) || !syncFd(io.parent.get(), &detail)
            || !syncFd(io.control.get(), &detail))
            return archiveWarning(state, detail, error);
    } else if (!stageMissing || missing || !safeNode(existing, true)
               || directoryProof(existing) != io.owner.value("source_root").toObject()
               || !boundDirectory(state.archivePath, io.source.get(), io.sourceIdentity, true, &detail)) {
        return archiveWarning(state, QStringLiteral("보관한 원래 프로필의 정체성이 바뀌어 남은 기록을 보존했습니다."), error);
    }
    // Verify every control proof before removing its own link. The new active
    // pathname is never unlinked here, including after a later normal launch.
    if (!completionControlBound(state, &detail)
        || !stageProofMatches(io.control.get(), io.owner.value("source_root").toObject(), true, &detail)
        || !removeOwnedControlLock(io, &detail))
        return archiveWarning(state, detail, error);
    const CleanupProof proof{io.owner.value("source_root").toObject(), &io.ownerIdentity, &io.ownerHash,
        io.lockIdentity.st_ino ? &io.lockIdentity : nullptr, io.lockIdentity.st_ino ? &io.lockHash : nullptr};
    if (!removeControl(io.parent.get(), io.controlName, io.control.get(), io.controlIdentity, proof, &detail))
        return archiveWarning(state, detail, error);
    state.controlRemoved = true;
    return true;
}
ProfileReady ProfileRestoreCompletion::ready() const {
    if (!d_) return {};
    if (d_->aborted) return {d_->selected.directory, d_->selected.identityScheme, d_->io.warning};
    if (!d_->restoredActive) return {};
    return {d_->io.paths.currentDirectory, d_->receipt.identityScheme, d_->io.warning};
}
QString ProfileRestoreCompletion::archiveDirectory() const { return d_ ? d_->archivePath : QString{}; }

#else
struct ProfileRestoreSession::Private {};
struct ProfileRestoreCompletion::Private {};
ProfileRestoreSession::ProfileRestoreSession(std::unique_ptr<Private> state) : d_(std::move(state)) {}
ProfileRestoreSession::~ProfileRestoreSession() = default;
ProfileRestoreCompletion::ProfileRestoreCompletion(std::unique_ptr<Private> state) : d_(std::move(state)) {}
ProfileRestoreCompletion::~ProfileRestoreCompletion() = default;
std::unique_ptr<ProfileRestoreSession> ProfileRestoreSession::begin(
    const ProfileReady&, QLockFile&, const QString&, QString* error) {
    fail(error, QStringLiteral("이 시스템에서는 안전한 프로필 복원을 지원하지 않습니다.")); return {};
}
bool ProfileRestoreSession::copy(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
bool ProfileRestoreSession::restore(const QString&, const QString&, const ProfileBackupRestorer&, QString* error) {
    return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다."));
}
bool ProfileRestoreSession::verify(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
bool ProfileRestoreSession::prepareForRestart(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
QString ProfileRestoreSession::stagingDirectory() const { return {}; }
QString ProfileRestoreSession::archiveDirectory() const { return {}; }
std::unique_ptr<ProfileRestoreCompletion> ProfileRestoreCompletion::begin(
    const ProfileReady&, const QString&, QString* error) {
    fail(error, QStringLiteral("이 시스템에서는 안전한 프로필 복원을 지원하지 않습니다.")); return {};
}
bool ProfileRestoreCompletion::refreshPublicSettings(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
bool ProfileRestoreCompletion::verify(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
bool ProfileRestoreCompletion::publish(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
bool ProfileRestoreCompletion::archive(QString* error) { return fail(error, QStringLiteral("프로필 복원을 지원하지 않습니다.")); }
ProfileReady ProfileRestoreCompletion::ready() const { return {}; }
QString ProfileRestoreCompletion::archiveDirectory() const { return {}; }
#endif


namespace {
bool selectedLegacyIsSafe(const DevProfilePaths& paths, QString* error) {
    bool exists = false;
    if (!safeDirectory(paths.legacyDirectory, false, true, &exists, error) || !exists) return false;
    if (profileSettingsIdentity(paths.legacyDirectory).organization.isEmpty())
        return fail(error, QStringLiteral("기존 프로필의 설정 경로가 안전하지 않습니다."));
    return true;
}
bool selectedCurrentIsSafe(const DevProfilePaths& paths, bool mayBeMissing,
                           bool* markerPresent, QString* error) {
    bool exists = false;
    if (!safeDirectory(paths.currentDirectory, mayBeMissing, true, &exists, error)) return false;
    if (markerPresent) *markerPresent = false;
    if (!exists) return mayBeMissing;
    if (profileSettingsIdentity(paths.currentDirectory).organization.isEmpty())
        return fail(error, QStringLiteral("새 프로필의 설정 경로가 안전하지 않습니다."));
    const auto markerPath = QDir(paths.currentDirectory).filePath(MarkerName);
    if (pathExists(markerPath)) {
        QJsonObject marker;
        if (!readJson(markerPath, &marker, error) || !validMarker(marker, paths, error)) return false;
        if (markerPresent) *markerPresent = true;
    }
    return true;
}
#ifdef Q_OS_UNIX
bool finishPublishedRecovery(const DevProfilePaths& paths, QString* error) {
    if (!pathExists(paths.transactionDirectory)) return true;
    auto parent = openDirectory(QFileInfo(paths.currentDirectory).absolutePath(), nullptr, error);
    if (!parent) return false;
    bool legacyMissing = false;
    auto source = openDirectory(paths.legacyDirectory, &legacyMissing, error);
    if (!source && !legacyMissing) return false;
    if (source && !privateDirectoryFd(source.get(), nullptr, error)) return false;
    if (!recoverControl(paths, parent.get(), source.get(), error)) return false;
    return closeChecked(source, error) && closeChecked(parent, error);
}
#endif
} // namespace

ProfileMigrationResult applyDevProfileChoice(const ProfileMigrationPlan& plan,
                                             ProfileMigrationChoice choice,
                                             const QString& version) {
    ProfileMigrationResult result;
    if (plan.action == DevProfileAction::UseExplicit) {
        // Explicit selection bypasses all default paths, including broken markers and controls.
        const auto fresh = planDevProfile(std::optional<QString>{plan.selectedDirectory}, {});
        if (fresh.action != DevProfileAction::UseExplicit) {
            result.error = fresh.error.isEmpty() ? QStringLiteral("지정한 프로필을 사용할 수 없습니다.") : fresh.error;
            return result;
        }
        result.directory = fresh.selectedDirectory;
        result.settings = fresh.settings;
        return result;
    }
    if (plan.action == DevProfileAction::Unavailable
        || !samePaths(plan.paths, devProfilePaths(plan.paths.baseDirectory))) {
        result.error = plan.error.isEmpty() ? QStringLiteral("프로필 선택 계획을 확인할 수 없습니다.") : plan.error;
        return result;
    }
    const bool automatic = plan.action == DevProfileAction::UseCurrent || plan.action == DevProfileAction::CreateCurrent;
    if (!automatic && choice == ProfileMigrationChoice::Cancel) {
        result.cancelled = true;
        return result;
    }
    if (automatic) {
        if (plan.action == DevProfileAction::CreateCurrent) {
            // A prompt-free create plan is still only a selection. The existing vault
            // code owns directory creation and its existing permission checks.
            const auto fresh = planDevProfile(std::nullopt, plan.paths.baseDirectory);
            if (fresh.action != DevProfileAction::CreateCurrent && fresh.action != DevProfileAction::UseCurrent) {
                result.error = fresh.error.isEmpty()
                    ? QStringLiteral("프로필 선택 중 폴더 상태가 바뀌어 다시 선택해야 합니다.") : fresh.error;
                return result;
            }
        }
        bool completed = false;
        if (!selectedCurrentIsSafe(plan.paths, plan.action == DevProfileAction::CreateCurrent, &completed, &result.error))
            return result;
#ifdef Q_OS_UNIX
        if (completed) {
            QString cleanupError;
            if (!finishPublishedRecovery(plan.paths, &cleanupError))
                result.warning = cleanupError.isEmpty()
                    ? QStringLiteral("완료된 프로필은 사용할 수 있지만 남은 이전 작업을 정리하지 못했습니다.")
                    : cleanupError;
        }
#endif
        result.directory = plan.paths.currentDirectory;
        result.settings = currentSettings();
        return result;
    }
    if (choice == ProfileMigrationChoice::Later || choice == ProfileMigrationChoice::UseLegacy) {
        if (!selectedLegacyIsSafe(plan.paths, &result.error)) return result;
        result.directory = plan.paths.legacyDirectory;
        result.settings = legacySettings();
        return result;
    }
    if (choice == ProfileMigrationChoice::UseCurrent) {
        bool completed = false;
        if (plan.action != DevProfileAction::ChooseExisting
            || !selectedCurrentIsSafe(plan.paths, false, &completed, &result.error)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("선택할 새 프로필이 없습니다.");
            return result;
        }
#ifdef Q_OS_UNIX
        if (completed) {
            QString cleanupError;
            if (!finishPublishedRecovery(plan.paths, &cleanupError))
                result.warning = cleanupError.isEmpty()
                    ? QStringLiteral("완료된 프로필은 사용할 수 있지만 남은 이전 작업을 정리하지 못했습니다.")
                    : cleanupError;
        }
#endif
        result.directory = plan.paths.currentDirectory;
        result.settings = currentSettings();
        return result;
    }
    if (choice != ProfileMigrationChoice::Copy) {
        result.error = QStringLiteral("이 프로필 선택을 처리할 수 없습니다.");
        return result;
    }
    const auto fallback = [&]() {
        QString fallbackError;
        if (selectedLegacyIsSafe(plan.paths, &fallbackError)) {
            result.directory = plan.paths.legacyDirectory;
            result.settings = legacySettings();
        } else {
            if (!result.error.isEmpty()) result.error += '\n';
            result.error += fallbackError.isEmpty()
                ? QStringLiteral("기존 프로필을 안전하게 다시 사용할 수 없습니다.") : fallbackError;
        }
    };
    const auto fresh = planDevProfile(std::nullopt, plan.paths.baseDirectory);
    if (plan.action != DevProfileAction::OfferCopy || fresh.action != DevProfileAction::OfferCopy) {
        result.error = fresh.error.isEmpty()
            ? QStringLiteral("안내 후 프로필 상태가 바뀌어 복사하거나 덮어쓰지 않았습니다.") : fresh.error;
        fallback();
        return result;
    }
    auto session = ProfileCopySession::begin(plan.paths, version, &result.error);
    if (!session) {
        fallback();
        return result;
    }
    const bool copied = session->copy(&result.error) && session->verify(&result.error) && session->publish(&result.error);
    if (copied) {
        result.directory = plan.paths.currentDirectory;
        result.settings = currentSettings();
        result.copied = true;
        result.warning = session->warning();
        return result;
    }
#ifdef Q_OS_UNIX
    // This production entrypoint can report cleanup problems before the RAII session
    // is destroyed; manual phase users retain the same destructor cleanup guarantee.
    cleanupSession(*session->d_);
    for (auto* descriptor : {&session->d_->stage, &session->d_->control,
                              &session->d_->source, &session->d_->parent}) {
        QString closingError;
        if (!closeChecked(*descriptor, &closingError)) addWarning(*session->d_, closingError);
    }
#endif
    result.warning = session->warning();
    session.reset();
    fallback();
    return result;
}
} // namespace keefi
