#pragma once

#include "ProfilePaths.h"
#include "legacy/LegacyProfileNames.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>
#include <limits>
#include <utility>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace keefi::profile_detail {

inline const QString MarkerName = QStringLiteral(".keefi-profile-copy.json");
inline const QString MarkerFormat = QStringLiteral("keefi-dev-profile-copy/1");
inline const QString OwnerFormat = QStringLiteral("keefi-dev-profile-copy-owner/1");
inline ProfileSettingsIdentity legacySettings() { return {legacy::Organization, legacy::Application}; }
inline ProfileSettingsIdentity currentSettings() { return {QStringLiteral("KEEFI"), QStringLiteral("KEEFI Dev")}; }
inline bool fail(QString* error, const QString& text) {
    if (error) *error = text;
    return false;
}
inline void clear(QString* error) { if (error) error->clear(); }
inline QString normalized(const QString& path) {
    return path.isEmpty() ? QString{} : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
inline bool pathExists(const QString& path) {
#ifdef Q_OS_UNIX
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0) return true;
    // An inaccessible marker must be validated and refused, not treated as absent.
    return errno != ENOENT;
#else
    return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
#endif
}
inline bool samePaths(const DevProfilePaths& a, const DevProfilePaths& b) {
    return !a.baseDirectory.isEmpty() && a.baseDirectory == b.baseDirectory
        && a.legacyDirectory == b.legacyDirectory && a.currentDirectory == b.currentDirectory
        && a.transactionDirectory == b.transactionDirectory;
}
inline bool validHash(const QString& hash) {
    if (hash.size() != 64) return false;
    for (const auto c : hash)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
           || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}

#ifdef Q_OS_UNIX
class Fd {
public:
    Fd() = default;
    explicit Fd(int value) : value_(value) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { reset(); value_ = other.release(); }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    explicit operator bool() const { return value_ >= 0; }
    int release() { return std::exchange(value_, -1); }
    void reset(int value = -1) {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }
private:
    int value_ = -1;
};
inline bool sameInode(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
inline bool safeNode(const struct stat& st, bool directory) {
    return (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))
        && st.st_uid == ::geteuid() && !(st.st_mode & (S_ISUID | S_ISGID | S_ISVTX))
        && (directory || st.st_nlink == 1);
}
// Walk every component using directory descriptors; do not follow ancestor symlinks.
inline Fd openDirectory(const QString& absolute, bool* missing, QString* error) {
    if (missing) *missing = false;
    if (absolute.isEmpty() || !QDir::isAbsolutePath(absolute) || normalized(absolute) != absolute) {
        fail(error, QStringLiteral("프로필 경로가 올바르지 않습니다."));
        return {};
    }
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current) { fail(error, QStringLiteral("프로필의 상위 경로를 열 수 없습니다.")); return {}; }
    QString walkPath = absolute;
#ifdef Q_OS_MACOS
    // macOS exposes these root-owned system aliases to QTemporaryDir and
    // QStandardPaths. Accept only their exact OS targets; user/profile links
    // elsewhere still pass through O_NOFOLLOW below.
    for (const auto& alias : {QStringLiteral("/var"), QStringLiteral("/tmp")}) {
        if (absolute != alias && !absolute.startsWith(alias + '/')) continue;
        const auto encoded = QFile::encodeName(alias);
        struct stat link{};
        if (::lstat(encoded.constData(), &link) != 0) {
            fail(error, QStringLiteral("시스템 임시 경로의 정체성을 확인할 수 없습니다."));
            return {};
        }
        if (S_ISLNK(link.st_mode)) {
            char bytes[256]{};
            const auto length = ::readlink(encoded.constData(), bytes, sizeof(bytes));
            const auto target = length > 0 && length < ssize_t(sizeof(bytes))
                ? QFile::decodeName(QByteArray(bytes, int(length))) : QString{};
            const auto resolved = target.startsWith('/') ? QDir::cleanPath(target)
                : QDir::cleanPath('/' + target);
            if (link.st_uid != 0 || target.isEmpty() || resolved != QStringLiteral("/private") + alias) {
                fail(error, QStringLiteral("예상하지 못한 시스템 경로 링크를 거부했습니다."));
                return {};
            }
            walkPath = resolved + absolute.mid(alias.size());
        }
        break;
    }
#endif
    const auto parts = walkPath.split('/', Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        const auto name = QFile::encodeName(part);
        Fd next(::openat(current.get(), name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) {
            if (errno == ENOENT && missing) { *missing = true; return {}; }
            fail(error, QStringLiteral("프로필 경로의 링크 또는 접근할 수 없는 폴더를 거부했습니다."));
            return {};
        }
        current = std::move(next);
    }
    return current;
}
inline bool safeDirectory(const QString& path, bool allowMissing, bool privateRoot,
                          bool* exists, QString* error) {
    bool missing = false;
    auto fd = openDirectory(path, &missing, error);
    if (exists) *exists = bool(fd);
    if (!fd) return missing && allowMissing
        ? true : (missing ? fail(error, QStringLiteral("프로필 폴더가 없습니다.")) : false);
    struct stat st{};
    if (::fstat(fd.get(), &st) || !safeNode(st, true)
        || (privateRoot && (st.st_mode & 0777) != 0700))
        return fail(error, QStringLiteral("프로필 폴더의 소유자 또는 권한이 안전하지 않습니다."));
    return true;
}
inline bool readJsonAt(int directory, const QByteArray& name, QJsonObject* result,
                       QString* error, QByteArray* raw = nullptr) {
    struct stat listed{}, before{}, after{};
    if (::fstatat(directory, name.constData(), &listed, AT_SYMLINK_NOFOLLOW) != 0
        || !safeNode(listed, false) || listed.st_size <= 0 || listed.st_size > 65536)
        return fail(error, QStringLiteral("프로필 이전 기록의 파일 종류를 확인할 수 없습니다."));
    // O_NONBLOCK also makes a FIFO substitution between stat and open fail
    // without waiting for an external writer.
    Fd fd(::openat(directory, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!fd || ::fstat(fd.get(), &before) || !safeNode(before, false)
        || !sameInode(listed, before) || before.st_size <= 0 || before.st_size > 65536)
        return fail(error, QStringLiteral("프로필 이전 기록을 안전하게 읽을 수 없습니다."));
    QFile file;
    if (!file.open(fd.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return fail(error, QStringLiteral("프로필 이전 기록을 열 수 없습니다."));
    const auto bytes = file.read(65537);
    if (file.error() != QFileDevice::NoError || ::fstat(fd.get(), &after)
        || !sameInode(before, after) || before.st_size != after.st_size
        || before.st_mode != after.st_mode || before.st_uid != after.st_uid
        || before.st_gid != after.st_gid || before.st_nlink != after.st_nlink
        || bytes.size() != before.st_size)
        return fail(error, QStringLiteral("읽는 동안 프로필 이전 기록이 바뀌었습니다."));
    file.close();
    if (::close(fd.release()) != 0)
        return fail(error, QStringLiteral("프로필 이전 기록을 닫는 중 오류가 발생했습니다."));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("프로필 이전 기록의 형식이 올바르지 않습니다."));
    *result = document.object();
    if (raw) *raw = bytes;
    return true;
}
#else
inline bool safeDirectory(const QString& path, bool allowMissing, bool,
                          bool* exists, QString* error) {
    QString current = normalized(path);
    if (current.isEmpty()) return fail(error, QStringLiteral("프로필 경로가 올바르지 않습니다."));
    const QFileInfo info(current);
    if (exists) *exists = info.exists();
    for (QString part = current; !part.isEmpty();) {
        const QFileInfo node(part);
        if (node.isSymLink() || node.isJunction()) return fail(error, QStringLiteral("프로필 경로의 링크를 거부했습니다."));
        const auto parent = node.absolutePath();
        if (parent == part) break;
        part = parent;
    }
    return (!info.exists() && allowMissing) || (info.exists() && info.isDir())
        || fail(error, QStringLiteral("프로필 폴더를 열 수 없습니다."));
}
#endif

inline bool readJson(const QString& path, QJsonObject* result, QString* error) {
#ifdef Q_OS_UNIX
    auto parent = openDirectory(QFileInfo(path).absolutePath(), nullptr, error);
    return parent && readJsonAt(parent.get(), QFile::encodeName(QFileInfo(path).fileName()), result, error);
#else
    if (QFileInfo(path).isSymLink()) return fail(error, QStringLiteral("이전 기록의 링크를 거부했습니다."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 65536)
        return fail(error, QStringLiteral("프로필 이전 기록을 읽을 수 없습니다."));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("프로필 이전 기록의 형식이 올바르지 않습니다."));
    *result = document.object();
    return true;
#endif
}
inline bool validMarker(const QJsonObject& marker, const DevProfilePaths& paths, QString* error) {
    const auto completed = QDateTime::fromString(marker.value("completed_at_utc").toString(), Qt::ISODateWithMs);
    if (marker.value("format").toString() != MarkerFormat
        || marker.value("source").toString() != paths.legacyDirectory
        || marker.value("target").toString() != paths.currentDirectory
        || marker.value("version").toString().isEmpty()
        || !marker.value("verified").isBool() || !marker.value("verified").toBool()
        || !completed.isValid() || completed.offsetFromUtc() != 0
        || marker.value("file_count").toInteger(-1) < 0
        || marker.value("file_count") != QJsonValue(marker.value("file_count").toInteger(-1))
        || marker.value("total_bytes").toInteger(-1) < 0
        || marker.value("total_bytes") != QJsonValue(marker.value("total_bytes").toInteger(-1))
        || !validHash(marker.value("source_manifest_sha256").toString()))
        return fail(error, QStringLiteral("완료된 프로필 이전 기록을 확인할 수 없습니다."));
    return true;
}
} // namespace keefi::profile_detail
