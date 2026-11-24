#include "keefi/PriceArchive.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/SecureBuffer.h"
#include "keefi/TradingTypes.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QSaveFile>
#include <QThread>
#include <QTemporaryFile>
#include <QTimer>
#include <QtEndian>
#include <sodium.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "WindowsPrivateStorage.h"
#include <io.h>
#endif
namespace keefi {
namespace {
// Same container as the swap journal: magic, version, reserved, XChaCha20 nonce,
// ciphertext length, then XChaCha20-Poly1305 over canonical JSON zero-padded to
// PaddingBytes so the file size shows only a coarse amount of history.
constexpr qsizetype HeaderBytes=40;
constexpr qint64 MinuteMs=60000,DayMs=86400000;
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}


bool privatePath(const QString& path, bool directory, QString* error) {
    const auto invalid = [&] { return fail(error, QStringLiteral("금고 경로의 소유자·권한·심볼릭 링크를 확인해 주세요.")); };
    const QFileInfo info(path);
    if (info.isSymLink() || !info.exists() || info.isDir() != directory)
        return invalid();
#ifdef Q_OS_UNIX
    struct stat st{};
    const QByteArray name = QFile::encodeName(path);
    if (::lstat(name.constData(), &st) || st.st_uid != ::geteuid() || (st.st_mode & 0077) ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)))
        return invalid();
#ifdef Q_OS_MACOS
    // Same conservative rule as the other vault-keyed files: an existing ACL entry is refused.
    filesec_t security = ::filesec_init();
    if (!security) return invalid();
    struct stat aclStat{};
    int hasAcl = 0;
    bool safe = ::lstatx_np(name.constData(), &aclStat, security) == 0 &&
        aclStat.st_dev == st.st_dev && aclStat.st_ino == st.st_ino &&
        ::filesec_query_property(security, FILESEC_ACL, &hasAcl) == 0;
    if (safe && hasAcl) {
        acl_t acl = nullptr;
        safe = ::filesec_get_property(security, FILESEC_ACL, &acl) == 0 && acl;
        if (safe) {
            acl_entry_t entry = nullptr;
            safe = ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) != 0;
        }
        if (acl) ::acl_free(acl);
    }
    ::filesec_free(security);
    if (!safe) return invalid();
#endif
#elif defined(Q_OS_WIN)
    if (!privateWindowsPath(path, false)) return invalid();
#else
    return fail(error, QStringLiteral("이 운영체제의 금고 접근 권한 검증은 지원하지 않습니다."));
#endif
    return true;
}

QByteArray lockFileIdentity(const QString& path) {
#ifdef Q_OS_UNIX
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid())
        return {};
    return QByteArray::number(static_cast<qulonglong>(st.st_dev)) + ':' +
           QByteArray::number(static_cast<qulonglong>(st.st_ino));
#elif defined(Q_OS_WIN)
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    CloseHandle(file);
    if (!valid) return {};
    return QByteArray::number(info.dwVolumeSerialNumber) + ':' + QByteArray::number(info.nFileIndexHigh) + ':' +
           QByteArray::number(info.nFileIndexLow);
#else
    Q_UNUSED(path)
    return {};
#endif
}

enum class ReadResult { Ok, Unsafe, Malformed };
ReadResult readEncrypted(const QString& path,QByteArray* bytes,QString* error){
    if(!privatePath(path,false,error))return ReadResult::Unsafe;QFile file(path);
#ifdef Q_OS_UNIX
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
    if(fd<0){fail(error,"시세 보관소 파일을 열지 못했습니다.");return ReadResult::Unsafe;}
    if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)){::close(fd);fail(error,"시세 보관소 접근 권한이 안전하지 않습니다.");return ReadResult::Unsafe;}
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);fail(error,"시세 보관소를 읽지 못했습니다.");return ReadResult::Unsafe;}
#else
    if(!file.open(QIODevice::ReadOnly)){fail(error,"시세 보관소를 읽지 못했습니다.");return ReadResult::Unsafe;}
#endif
    if(file.size()<HeaderBytes+16||file.size()>PriceArchive::MaxFileBytes){fail(error,"시세 보관소 크기 상한 또는 형식 오류입니다.");return ReadResult::Malformed;}
    *bytes=file.read(PriceArchive::MaxFileBytes+1);
    if(file.error()!=QFile::NoError||bytes->size()!=file.size()){fail(error,"시세 보관소 전체 읽기에 실패했습니다.");return ReadResult::Unsafe;}
    return ReadResult::Ok;
}
bool decrypt(const QByteArray& bytes,const SecureBuffer& key,QByteArray* plain,QString* error){
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(key.size()!=32||bytes.size()<HeaderBytes+16||bytes.size()>PriceArchive::MaxFileBytes||storage_detail::formatOf(bytes,storage_detail::Kind::Price)==storage_detail::Format::Invalid||qFromLittleEndian<quint16>(p+8)!=1||qFromLittleEndian<quint16>(p+10)!=0||qFromLittleEndian<quint32>(p+36)!=bytes.size()-HeaderBytes)return fail(error,"지원하지 않거나 손상된 시세 보관소 형식입니다.");
    QByteArray result(bytes.size()-HeaderBytes-16,'\0');unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(result.data()),&size,nullptr,p+HeaderBytes,bytes.size()-HeaderBytes,p,HeaderBytes,p+12,key.data())||size!=quint64(result.size())){
        sodium_memzero(result.data(),std::size_t(result.size()));return fail(error,"시세 보관소 인증에 실패했습니다. 다른 금고의 기록일 수 있습니다.");}
    *plain=std::move(result);return true;
}
QByteArray encrypt(const QByteArray& plain,const SecureBuffer& key){
    QByteArray out(HeaderBytes+plain.size()+16,'\0');auto* p=reinterpret_cast<unsigned char*>(out.data());std::memcpy(p,"KEEFIPRC",8);qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+36);
    unsigned long long size=0;if(key.size()!=32||crypto_aead_xchacha20poly1305_ietf_encrypt(p+HeaderBytes,&size,reinterpret_cast<const unsigned char*>(plain.constData()),std::size_t(plain.size()),p,HeaderBytes,nullptr,p+12,key.data())||size!=quint64(plain.size())+16)return {};return out;
}
void wipe(QByteArray& bytes){if(!bytes.isEmpty()){sodium_memzero(bytes.data(),std::size_t(bytes.size()));bytes.clear();}}
// Only fixed diagnostic codes, encrypted with the already available archive key.
// No account, symbol, path, wall clock, key or arbitrary error string is persisted.
QByteArray exitNoticeBytes(const SecureBuffer& key,const QString& reason){
    auto plain=QJsonDocument(QJsonObject{{"purpose","price-archive-exit/1"},{"reason",reason}}).toJson(QJsonDocument::Compact);
    plain.append(QByteArray(256-plain.size(),'\0'));auto bytes=encrypt(plain,key);wipe(plain);return bytes;
}
QString exitNoticeText(const QByteArray& bytes,const SecureBuffer& key){
    if(bytes.isEmpty())return {};
    QByteArray plain;
    if(bytes.size()!=312||!decrypt(bytes,key,&plain,nullptr))return QStringLiteral("이전 종료의 시세 기록 저장 상태를 확인하지 못했어요.");
    while(!plain.isEmpty()&&plain.back()=='\0')plain.chop(1);
    const auto object=QJsonDocument::fromJson(plain).object();wipe(plain);
    if(object.size()!=2||object.value("purpose")!=QJsonValue("price-archive-exit/1"))return QStringLiteral("이전 종료의 시세 기록 저장 상태를 확인하지 못했어요.");
    const auto reason=object.value("reason").toString();
    if(reason=="timeout")return QStringLiteral("이전 종료에서 시세 기록 저장이 3초 안에 끝나지 않아 중단했어요. 저장 완료를 확인하지 못한 시세는 다시 조회합니다.");
    if(reason=="failed")return QStringLiteral("이전 종료에서 시세 기록 저장 오류가 발생했어요. 저장 완료를 확인하지 못한 시세는 다시 조회합니다.");
    if(reason=="external-change")return QStringLiteral("이전 종료에서 시세 기록 파일의 외부 변경을 감지해 저장을 중단했어요. 다른 파일을 덮어쓰지 않았습니다.");
    if(reason=="unsafe-path")return QStringLiteral("이전 종료에서 시세 기록 경로의 권한 또는 안전성을 확인하지 못해 저장을 중단했어요.");
    if(reason=="disk")return QStringLiteral("이전 종료에서 시세 기록의 디스크 반영 또는 재검증에 실패했어요. 이미 교체된 파일이 있을 수 있어요.");
    if(reason=="encryption")return QStringLiteral("이전 종료에서 시세 기록의 암호화 검증에 실패해 저장을 중단했어요.");
    if(reason=="interrupted")return QStringLiteral("이전 종료에서 잠금 또는 앱 종료로 시세 기록 저장을 중단했어요. 이미 교체한 파일은 되돌리지 않았습니다.");
    return QStringLiteral("이전 종료의 시세 기록 저장 완료를 확인하지 못했어요. 3초 대기 한도 또는 저장 중단으로 일부 기록이 남지 않았을 수 있어요.");
}
QByteArray readExitNotice(const QString& path){
    const QFileInfo info(path);if(!info.exists()&&!info.isSymLink())return {};
    QByteArray bytes;QString ignored;
    if(info.size()!=312||readEncrypted(path,&bytes,&ignored)!=ReadResult::Ok)return QByteArray("invalid");
    return bytes;
}
struct ExitNotice {
    enum Outcome { Pending, Saved, Failed, Timeout, Interrupted };
    QString path,vaultPath;QByteArray pending,failed,timeout,interrupted;QHash<QString,QByteArray> failures;
    std::mutex mutex;std::condition_variable wake;Outcome outcome=Pending;
    QString failureCode;
    std::chrono::steady_clock::time_point deadline;
    std::atomic_bool finished=false;
    void decide(Outcome value,const QString& message={}){
        {std::lock_guard<std::mutex> guard(mutex);if(outcome==Pending){outcome=value;
            if(value==Failed){
                if(message.contains(QStringLiteral("외부에서 변경"))||message.contains(QStringLiteral("검증 중 변경")))failureCode="external-change";
                else if(message.contains(QStringLiteral("권한"))||message.contains(QStringLiteral("경로")))failureCode="unsafe-path";
                else if(message.contains(QStringLiteral("디스크"))||message.contains(QStringLiteral("교체된")))failureCode="disk";
                else if(message.contains(QStringLiteral("암호")))failureCode="encryption";
            }
        }}
        wake.notify_all();
    }
    bool write(const QByteArray& bytes){
        // Diagnostic persistence is best-effort, never an extra shutdown wait.
        // A deleted vault must not be recreated to save an informational notice.
        QString ignored;
        if(!QFileInfo::exists(vaultPath)||bytes.isEmpty()||!privatePath(QFileInfo(path).absolutePath(),true,&ignored))return false;
        const QFileInfo before(path);
        if((before.exists()||before.isSymLink())&&!privatePath(path,false,&ignored))return false;
        QSaveFile file(path);file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)||file.write(bytes)!=bytes.size())return false;
        if(!QFileInfo::exists(vaultPath)){file.cancelWriting();return false;}
        return file.commit();
    }
    void run(){
        // Leave an authenticated write-ahead notice if the process exits before
        // the terminal notice can be written. Disk failure cannot promise a notice.
        const bool armed=write(pending);
        Outcome result;QByteArray failure;
        {std::unique_lock<std::mutex> guard(mutex);
            if(!wake.wait_until(guard,deadline,[&]{return outcome!=Pending;}))outcome=Timeout;
            result=outcome;failure=failures.value(failureCode,failed);}
        if(result==Saved){if(armed)QFile::remove(path);}
        else write(result==Timeout?timeout:result==Failed?failure:interrupted);
        finished=true;
    }
};
// A cheap final identity check bridges worker verification and owner-thread commit.
// The writer lock remains held throughout; the digest is still checked on the worker.
QByteArray fileRevision(const QString& path){
#ifdef Q_OS_UNIX
    struct stat st{};
    if(::lstat(QFile::encodeName(path).constData(),&st)||!S_ISREG(st.st_mode))return {};
#ifdef Q_OS_MACOS
    const auto modified=st.st_mtimespec,changed=st.st_ctimespec;
#else
    const auto modified=st.st_mtim,changed=st.st_ctim;
#endif
    return QByteArray::number(qulonglong(st.st_dev))+':'+QByteArray::number(qulonglong(st.st_ino))+':'+
        QByteArray::number(qlonglong(st.st_size))+':'+QByteArray::number(qlonglong(modified.tv_sec))+':'+
        QByteArray::number(qlonglong(modified.tv_nsec))+':'+QByteArray::number(qlonglong(changed.tv_sec))+':'+
        QByteArray::number(qlonglong(changed.tv_nsec));
#else
    const QFileInfo info(path);const auto identity=lockFileIdentity(path);
    if(identity.isEmpty()||!info.isFile()||info.isSymLink())return {};
    return identity+':'+QByteArray::number(info.size())+':'+QByteArray::number(info.lastModified().toMSecsSinceEpoch());
#endif
}
struct CipherWrite {
    QString path,temporary,error;
    QByteArray bytes,previous,digest,expectedRevision,temporaryIdentity;
    bool creating=false,prepared=false,committed=false,ok=false;
    std::unique_ptr<QLockFile> lock;
#ifdef Q_OS_UNIX
    int directory=-1;
#endif
    ~CipherWrite(){
        if(!temporary.isEmpty()&&lockFileIdentity(temporary)==temporaryIdentity)QFile::remove(temporary);
#ifdef Q_OS_UNIX
        if(directory>=0)::close(directory);
#endif
    }
};
// Worker phase: ciphertext only; no key, Vault, or final-path replacement.
bool prepareCipher(CipherWrite& save,const std::atomic_bool* cancelled){
    const auto& path=save.path;const auto& bytes=save.bytes;const auto& previous=save.previous;
    const bool creating=save.creating;auto* error=&save.error;
    const auto stopped=[&]{return cancelled&&cancelled->load();};
    const auto unchanged=[&]{if(creating)return (!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink())||fail(error,"시세 보관소 파일이 이미 생겨 저장을 중단했습니다.");QByteArray current;return readEncrypted(path,&current,error)==ReadResult::Ok&&(QCryptographicHash::hash(current,QCryptographicHash::Sha256)==previous||fail(error,"시세 보관소가 외부에서 변경되어 저장을 중단했습니다."));};
    if(stopped())return fail(error,"잠금으로 시세 보관소 저장을 중단했습니다.");
    if(bytes.isEmpty()||!unchanged())return bytes.isEmpty()?fail(error,"시세 보관소 암호화에 실패했습니다."):false;
    QTemporaryFile temp(QFileInfo(path).absolutePath()+"/.keefi-prices-XXXXXX");temp.setAutoRemove(true);if(!temp.open())return fail(error,"시세 보관소 임시 파일을 만들지 못했습니다.");
#ifdef Q_OS_WIN
    if(!privateWindowsPath(temp.fileName(),true))return fail(error,"시세 보관소 임시 파일을 보호하지 못했습니다.");
#else
    if(!temp.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner))return fail(error,"시세 보관소 임시 파일을 보호하지 못했습니다.");
#endif
    if(!privatePath(temp.fileName(),false,error)||temp.write(bytes)!=bytes.size()||!temp.flush())return fail(error,"시세 보관소 저장에 실패했습니다. 기존 파일을 보존했습니다.");
#ifdef Q_OS_UNIX
    if(::fsync(temp.handle()))return fail(error,"시세 보관소를 디스크에 반영하지 못했습니다.");
#elif defined(Q_OS_WIN)
    if(!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(temp.handle()))))return fail(error,"시세 보관소를 디스크에 반영하지 못했습니다.");
#endif
    if(!temp.seek(0)||temp.read(PriceArchive::MaxFileBytes+1)!=bytes)return fail(error,"시세 보관소 저장 후 검증에 실패했습니다.");
    if(stopped())return fail(error,"잠금으로 교체 전 시세 보관소 저장을 중단했습니다.");
    const auto before=fileRevision(path);
    if(!unchanged()||!privatePath(QFileInfo(path).absolutePath(),true,error))return false;
    save.expectedRevision=fileRevision(path);
    if(!creating&&(before.isEmpty()||before!=save.expectedRevision))return fail(error,"시세 보관소가 검증 중 변경되어 저장을 중단했습니다.");
#ifdef Q_OS_UNIX
    save.directory=::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(),O_RDONLY|O_CLOEXEC);
    if(save.directory<0)return fail(error,"시세 보관소 폴더를 열지 못했습니다.");
#endif
    save.temporary=temp.fileName();save.temporaryIdentity=lockFileIdentity(save.temporary);
    if(save.temporaryIdentity.isEmpty())return fail(error,"시세 보관소 임시 파일의 식별자를 확인하지 못했습니다.");
    temp.close();temp.setAutoRemove(false);save.prepared=true;return true;
}
// Owner-thread phase. No signal emission/event processing between the generation
// check at the caller and this rename: lock() cannot interleave a later commit.
bool commitCipher(CipherWrite& save){
    auto* error=&save.error;
    if(!save.prepared||save.temporary.isEmpty()||lockFileIdentity(save.temporary)!=save.temporaryIdentity)
        return fail(error,"시세 보관소 임시 파일이 변경되어 저장을 중단했습니다.");
    if(save.creating){
        if(QFileInfo::exists(save.path)||QFileInfo(save.path).isSymLink())return fail(error,"시세 보관소 파일이 이미 생겨 저장을 중단했습니다.");
    }else if(fileRevision(save.path)!=save.expectedRevision)return fail(error,"시세 보관소가 외부에서 변경되어 저장을 중단했습니다.");
    if(!privatePath(QFileInfo(save.path).absolutePath(),true,error))return false;
#ifdef Q_OS_UNIX
    if(::rename(QFile::encodeName(save.temporary).constData(),QFile::encodeName(save.path).constData()))
        return fail(error,"시세 보관소 원자 교체에 실패했습니다.");
#elif defined(Q_OS_WIN)
    if(!MoveFileExW(reinterpret_cast<const wchar_t*>(save.temporary.utf16()),reinterpret_cast<const wchar_t*>(save.path.utf16()),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return fail(error,"시세 보관소 원자 교체에 실패했습니다.");
#else
    return fail(error,"이 운영체제의 시세 보관소 원자 교체는 지원하지 않습니다.");
#endif
    save.temporary.clear();save.committed=true;return true;
}
// Worker phase after commit. A subsequent lock may cancel result application,
// but directory durability/readback still finish with the same writer lock held.
bool finishCipher(CipherWrite& save){
    if(!save.committed)return fail(&save.error,"교체되지 않은 시세 보관소입니다.");
#ifdef Q_OS_UNIX
    if(::fsync(save.directory))return fail(&save.error,"교체 후 디스크 반영을 확인하지 못했습니다. 금고를 다시 열어 주세요.");
#endif
    QByteArray finalBytes;
    if(readEncrypted(save.path,&finalBytes,&save.error)!=ReadResult::Ok||finalBytes!=save.bytes)
        return fail(&save.error,"교체된 시세 보관소를 확인하지 못했습니다. 금고를 다시 열어 주세요.");
    save.digest=QCryptographicHash::hash(save.bytes,QCryptographicHash::Sha256);return true;
}

// decimal::normalize output, checked without regular expressions: up to 80 integer
// digits without a leading zero, and up to 80 fraction digits without a trailing zero.
bool canonical(const QString& value,bool positive){
    const auto size=value.size();if(size<1||size>161)return false;
    const auto dot=value.indexOf(u'.');const auto whole=dot<0?size:dot;
    if(whole<1||whole>80||(whole>1&&value[0]==u'0'))return false;
    if(dot>=0&&(size-dot-1<1||size-dot-1>80||value[size-1]==u'0'))return false;
    bool nonzero=false;
    for(qsizetype i=0;i<size;++i){if(i==dot)continue;const auto c=value[i];if(c<u'0'||c>u'9')return false;nonzero=nonzero||c!=u'0';}
    return !positive||nonzero;
}
int compareCanonical(const QString& a,const QString& b){
    const auto da=a.indexOf(u'.'),db=b.indexOf(u'.');const auto wa=da<0?a.size():da,wb=db<0?b.size():db;
    if(wa!=wb)return wa<wb?-1:1;
    for(qsizetype i=0;i<wa;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
    const auto fa=da<0?QStringView{}:QStringView(a).mid(da+1),fb=db<0?QStringView{}:QStringView(b).mid(db+1);
    for(qsizetype i=0;i<qMax(fa.size(),fb.size());++i){const QChar x=i<fa.size()?fa[i]:QChar(u'0'),y=i<fb.size()?fb[i]:QChar(u'0');if(x!=y)return x<y?-1:1;}
    return 0;
}
struct Row { MarketCandle candle; qint64 receivedAtMs=0; };
struct Series {
    QString identity, asset, currency, source;
    qint64 intervalMs=0, updatedAtMs=0, lastViewedAtMs=0;
    int conflictOverflow=0; // Revisions dropped at the conflict bound; the series is not clean.
    quint64 revision=0; // Owner-thread version, never serialized; protects in-flight eviction.
    QMap<qint64,Row> rows; // Keyed by candle start; first observation wins.
    QList<Row> conflicts;
};
QString seriesKey(const QString& identity,qint64 interval){return identity+QChar(0)+QString::number(interval);}
// Receipt and view times are kept to the minute: enough for provenance, less browsing detail.
qint64 floorMinute(qint64 ms){return ms>0?ms-ms%MinuteMs:0;}
qint64 wallClock(){return QDateTime::currentMSecsSinceEpoch();}
bool sameValues(const MarketCandle& a,const MarketCandle& b){return a.open==b.open&&a.high==b.high&&a.low==b.low&&a.close==b.close&&a.volume==b.volume;}
bool validRow(const Row& row,qint64 interval){
    const auto& c=row.candle;
    return c.startsAtMs>0&&interval>0&&c.startsAtMs%interval==0&&c.startsAtMs<=std::numeric_limits<qint64>::max()-interval&&
        // A completed candle ends no later than the moment it was received.
        row.receivedAtMs>0&&c.startsAtMs+interval-1<=row.receivedAtMs&&
        (c.lastStoredTickMs==0||(c.lastStoredTickMs>=c.startsAtMs&&c.lastStoredTickMs<=row.receivedAtMs))&&
        canonical(c.open,true)&&canonical(c.high,true)&&canonical(c.low,true)&&canonical(c.close,true)&&canonical(c.volume,false)&&
        compareCanonical(c.high,c.open)>=0&&compareCanonical(c.high,c.close)>=0&&compareCanonical(c.low,c.open)<=0&&compareCanonical(c.low,c.close)<=0;
}
// The candle endpoint (without its query) is the series source, so a later change to
// the requested count does not invalidate stored history.
QString feed(const QString& identity,qint64 interval){
    const auto url=MarketCandles::request(identity,interval).url;return url.isEmpty()?QString{}:url.adjusted(QUrl::RemoveQuery).toString();
}
bool consistent(const QString& identity,const QString& asset,const QString& currency){
    if(!binanceIdentifierValid(asset)||!binanceIdentifierValid(currency))return false;
    const auto pieces=identity.split(u':');if(pieces.size()!=3)return false;
    if(pieces[1]=="upbit")return pieces[2]==currency+u'-'+asset;
    return pieces[1]=="binance"&&pieces[2]==asset+currency;
}
bool validSeries(const QString& identity,qint64 interval,const QString& asset,const QString& currency,const QString& source){
    const auto expected=feed(identity,interval);return !expected.isEmpty()&&source==expected&&consistent(identity,asset,currency);
}
// Eviction order: shorter candles before daily or longer ones, then the least recently
// viewed, then the key, so equal times never depend on hash order.
QString victim(const QHash<QString,Series>& all,const QString& keep){
    QString chosen;const Series* best=nullptr;
    for(auto it=all.cbegin();it!=all.cend();++it){
        if(it.key()==keep)continue;const auto& s=*it;
        const auto rank=[](const Series& x){return x.intervalMs>=DayMs;};
        if(!best||rank(s)<rank(*best)||(rank(s)==rank(*best)&&(s.lastViewedAtMs<best->lastViewedAtMs||(s.lastViewedAtMs==best->lastViewedAtMs&&it.key()<chosen)))){best=&s;chosen=it.key();}
    }
    return chosen;
}
bool integer(const QJsonValue& value,qint64* out){
    if(!value.isString())return false;const auto text=value.toString();
    if(text.isEmpty()||text.size()>18||(text.size()>1&&text[0]==u'0'))return false;
    for(const auto c:text)if(c<u'0'||c>u'9')return false;
    bool ok=false;*out=text.toLongLong(&ok);return ok;
}
QJsonArray encodeRow(const Row& r){const auto& c=r.candle;return {QString::number(c.startsAtMs),QString::number(c.lastStoredTickMs),c.open,c.high,c.low,c.close,c.volume,QString::number(r.receivedAtMs)};}
bool decodeRow(const QJsonValue& value,qint64 interval,Row* r){
    const auto a=value.toArray();if(!value.isArray()||a.size()!=8)return false;
    for(const auto index:{2,3,4,5,6})if(!a[index].isString())return false;
    auto& c=r->candle;c.open=a[2].toString();c.high=a[3].toString();c.low=a[4].toString();c.close=a[5].toString();c.volume=a[6].toString();
    return integer(a[0],&c.startsAtMs)&&integer(a[1],&c.lastStoredTickMs)&&integer(a[7],&r->receivedAtMs)&&validRow(*r,interval);
}
QByteArray serialize(const QHash<QString,Series>& all){
    QJsonArray series;QStringList keys=all.keys();std::sort(keys.begin(),keys.end());
    for(const auto& key:keys){const auto& s=all[key];QJsonArray rows,conflicts;
        for(const auto& row:s.rows)rows.append(encodeRow(row));for(const auto& row:s.conflicts)conflicts.append(encodeRow(row));
        series.append(QJsonObject{{"identity",s.identity},{"interval_ms",QString::number(s.intervalMs)},{"asset",s.asset},{"currency",s.currency},
            {"source",s.source},{"updated_at_ms",QString::number(s.updatedAtMs)},{"last_viewed_at_ms",QString::number(s.lastViewedAtMs)},
            {"conflict_overflow",QString::number(s.conflictOverflow)},{"candles",rows},{"conflicts",conflicts}});}
    return QJsonDocument(QJsonObject{{"schema_version",1},{"series",series}}).toJson(QJsonDocument::Compact);
}
// Padded plaintext. Over the file bound, whole series leave in eviction order; never a
// partial run. Empty when a single series alone does not fit.
QByteArray pack(QHash<QString,Series> all,QStringList* evicted){
    for(;;){
        auto bytes=serialize(all);const auto padded=(bytes.size()+PriceArchive::PaddingBytes-1)/PriceArchive::PaddingBytes*PriceArchive::PaddingBytes;
        if(padded+HeaderBytes+16<=PriceArchive::MaxFileBytes){bytes.append(QByteArray(padded-bytes.size(),'\0'));return bytes;}
        wipe(bytes);if(all.size()<=1)return {};
        const auto key=victim(all,{});if(evicted)evicted->append(key);all.remove(key);
    }
}
// False only for a container that cannot be trusted as a whole (padding, JSON, schema).
// A series that no longer validates is dropped and counted; the rest stays usable.
bool unpack(const QByteArray& plain,QHash<QString,Series>* out,int* dropped){
    if(plain.isEmpty()||plain.size()%PriceArchive::PaddingBytes)return false;
    qsizetype end=plain.size();while(end>0&&plain[end-1]=='\0')--end;if(!end)return false;
    const auto json=QByteArray::fromRawData(plain.constData(),end);QJsonParseError error;const auto doc=QJsonDocument::fromJson(json,&error);
    auto again=doc.toJson(QJsonDocument::Compact);const bool canonicalJson=again==json;wipe(again);
    if(error.error!=QJsonParseError::NoError||!canonicalJson||!doc.isObject())return false;const auto o=doc.object();
    if(o.size()!=2||o["schema_version"]!=QJsonValue(1)||!o["series"].isArray()||o["series"].toArray().size()>PriceArchive::MaxSeries)return false;
    QHash<QString,Series> result;int skipped=0;
    for(const auto& value:o["series"].toArray()){
        const auto item=value.toObject();Series s;qint64 overflow=0;
        const bool shape=value.isObject()&&item.size()==10&&item["identity"].isString()&&item["asset"].isString()&&item["currency"].isString()&&item["source"].isString()&&
            item["candles"].isArray()&&item["conflicts"].isArray()&&integer(item["interval_ms"],&s.intervalMs)&&integer(item["updated_at_ms"],&s.updatedAtMs)&&
            integer(item["last_viewed_at_ms"],&s.lastViewedAtMs)&&integer(item["conflict_overflow"],&overflow)&&overflow<=std::numeric_limits<int>::max();
        if(!shape){++skipped;continue;}
        s.identity=item["identity"].toString();s.asset=item["asset"].toString();s.currency=item["currency"].toString();s.source=item["source"].toString();s.conflictOverflow=int(overflow);
        const auto rows=item["candles"].toArray(),conflicts=item["conflicts"].toArray();
        bool valid=validSeries(s.identity,s.intervalMs,s.asset,s.currency,s.source)&&!result.contains(seriesKey(s.identity,s.intervalMs))&&
            rows.size()<=PriceArchive::MaxCandlesPerSeries&&conflicts.size()<=PriceArchive::MaxConflictsPerSeries;
        qint64 previous=0;
        for(qsizetype i=0;valid&&i<rows.size();++i){Row r;valid=decodeRow(rows[i],s.intervalMs,&r)&&r.candle.startsAtMs>previous&&r.receivedAtMs<=s.updatedAtMs;
            if(valid){previous=r.candle.startsAtMs;s.rows.insert(previous,r);}}
        for(qsizetype i=0;valid&&i<conflicts.size();++i){Row r;valid=decodeRow(conflicts[i],s.intervalMs,&r)&&s.rows.contains(r.candle.startsAtMs)&&!sameValues(s.rows[r.candle.startsAtMs].candle,r.candle);
            if(valid)s.conflicts.append(r);}
        if(!valid){++skipped;continue;}
        result.insert(seriesKey(s.identity,s.intervalMs),s);
    }
    *out=std::move(result);if(dropped)*dropped=skipped;return true;
}
struct Job {
    std::function<void()> work,done;std::atomic_bool cancelled=false;QThread* thread=nullptr;bool handled=false,finishWhenCancelled=false;quint64 generation=0;
};
struct ReadState { QString path,error;QByteArray bytes,notice;bool present=true;ReadResult result=ReadResult::Unsafe; };
struct ParseState { QByteArray plain;QHash<QString,Series> rows;int dropped=0;bool ok=false;~ParseState(){wipe(plain);} };
struct PackState {
    QHash<QString,Series> series;QHash<QString,quint64> versions;QByteArray plain;QStringList evicted;
    ~PackState(){wipe(plain);}
};
} // namespace

struct PriceArchive::Private {
    PriceArchive* q=nullptr;Vault* vault=nullptr;QString path,error;SecureBuffer key,legacyKey;QByteArray digest,residualIdentity;std::unique_ptr<QLockFile> fileLock;
    QHash<QString,Series> series;bool opened=false,fileExists=false,legacy=false;
    quint64 changeEpoch=0,writtenEpoch=0,generation=0,revisionClock=0,failureSerial=0;
    qint64 flushDelay=PriceArchive::FlushDelayMs;int retryDelay=int(PriceArchive::RetryDelayMs);
    std::shared_ptr<Job> job;QTimer flushTimer,retryTimer,quitTimer;QPointer<MarketCandles> model;
    QString seededKey,lastSignature,lastFailure;bool locking=false,retryRequested=false,flushing=false,keyUnavailable=false;
    FlushCallback flushCallback;quint64 flushGeneration=0,flushFailureSerial=0;
    bool quitting=false,quitFinished=false,quitDataFinished=false,quitSaved=false;
    QDeadlineTimer quitDeadline;FlushCallback quitCallback;QString quitError,previousExit;
    std::shared_ptr<ExitNotice> exitNotice;QThread* noticeThread=nullptr;
    std::function<void()> beforeCommitForTest,afterSnapshotForTest,prepareWorkerForTest,verifyWorkerForTest;
    void abandonJob(){
        auto j=std::exchange(job,{});if(!j)return;
        j->cancelled=true;j->handled=true;j->done=nullptr;
        // The worker owns a separate work closure, with no Private/Vault/UI pointer.
        // Deleting a running QThread::create thread would wait; leave its lifetime
        // to finished/deleteLater instead. No late completion may commit a file.
        if(j->thread){QObject::disconnect(j->thread,nullptr,q,nullptr);j->thread=nullptr;}
    }
    ~Private(){
        abandonJob();
        if(exitNotice)exitNotice->decide(ExitNotice::Interrupted);
        if(noticeThread)QObject::disconnect(noticeThread,nullptr,q,nullptr);
    }
    bool dirty()const{return changeEpoch!=writtenEpoch;}
    const SecureBuffer& readKey(const QByteArray& bytes)const{
        return storage_detail::formatOf(bytes,storage_detail::Kind::Price)==storage_detail::Format::Legacy?legacyKey:key;
    }
    void classify(const QByteArray& bytes){
        const auto format=storage_detail::formatOf(bytes,storage_detail::Kind::Price);
        legacy=format==storage_detail::Format::Legacy||(format==storage_detail::Format::Invalid&&QFileInfo(path).fileName()==storage_detail::legacyFileName(storage_detail::Kind::Price));
    }
    void cleanStaleTemporaries(){
        // Cleanup belongs to an admitted current-format write, never to open(false).
        const QDir folder(QFileInfo(path).absolutePath());
        for(const auto& name:folder.entryList({QStringLiteral(".keefi-prices-*")},QDir::Files|QDir::Hidden|QDir::System)){
            const QFileInfo info(folder.filePath(name));if(!info.isSymLink())QFile::remove(info.filePath());}
    }
    void release(){if(fileLock&&fileLock->isLocked()){const auto identity=lockFileIdentity(path+".lock");fileLock->unlock();residualIdentity=!identity.isEmpty()&&lockFileIdentity(path+".lock")==identity?identity:QByteArray{};}fileLock.reset();}
    bool stale(const std::shared_ptr<Job>& j)const{return j->cancelled||j->generation!=generation;}
    void publishStatus(){
        if(!model)return;
        CandleArchiveStatus status;
        if(vault&&vault->isUnlocked()){
            status.detail=previousExit;
            if(!error.isEmpty()){
                if(!status.detail.isEmpty())status.detail+='\n';
                status.detail+=keyUnavailable?QStringLiteral("차트 기록 저장 꺼짐 · ")+error:error;
            }
            status.canRetry=!keyUnavailable&&!lastFailure.isEmpty()&&!job&&!flushing&&!quitting;
            if(status.canRetry)status.message=QStringLiteral("기록 저장 중단");
        }
        model->setArchiveStatus(status);
    }
    void finishFlush(bool ok,const QString& message){
        auto callback=std::exchange(flushCallback,{});
        // Keep collection paused until the receiver can lock/quit or cancel closing.
        QTimer::singleShot(0,q,[this,callback=std::move(callback),ok,message]{
            flushing=false;publishStatus();if(callback)callback(ok,message);
        });
    }
    void advanceFlush(){
        if(!flushCallback)return;
        if(quitting&&quitDeadline.hasExpired()){q->cancelQuit(true);return;}
        if(job)return;
        if(generation!=flushGeneration){finishFlush(false,lastFailure.isEmpty()?QStringLiteral("잠금으로 기록 저장을 중단했습니다."):lastFailure);return;}
        // Only a failure of THIS admitted dirty flush is a failure. Old diagnostic
        // state must not turn a later no-op quit into an unsuccessful save.
        if(failureSerial!=flushFailureSerial){finishFlush(false,lastFailure);return;}
        if(!q->isOpen()||!dirty()){finishFlush(true,{});return;}
        if(dirty()){writeAsync();return;}
        finishFlush(true,{});
    }
    void finishQuit(bool ok,const QString& message){
        if(quitFinished)return;quitFinished=true;quitTimer.stop();
        auto callback=std::exchange(quitCallback,{});flushCallback={};flushing=false;
        q->lock();abandonJob();
        if(callback)callback(ok,message);
    }
    void finishQuitData(bool ok,const QString& message){
        if(quitFinished)return;quitDataFinished=true;quitSaved=ok;quitError=message;
        if(exitNotice)exitNotice->decide(ok?ExitNotice::Saved:ExitNotice::Failed,message);
        if(!exitNotice||exitNotice->finished)finishQuit(ok,message);
    }
    void startExitNotice(){
        if(key.empty()||!vault||legacy)return;
        auto notice=std::make_shared<ExitNotice>();notice->path=path+".exit";notice->vaultPath=vault->filePath();
        notice->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(qMax<qint64>(0,quitDeadline.remainingTime()));
        notice->pending=exitNoticeBytes(key,"pending");notice->failed=exitNoticeBytes(key,"failed");
        notice->timeout=exitNoticeBytes(key,"timeout");notice->interrupted=exitNoticeBytes(key,"interrupted");
        for(const auto* reason:{"external-change","unsafe-path","disk","encryption"})notice->failures.insert(QString::fromLatin1(reason),exitNoticeBytes(key,QString::fromLatin1(reason)));
        exitNotice=notice;noticeThread=QThread::create([notice]{notice->run();});
        QObject::connect(noticeThread,&QThread::finished,noticeThread,&QObject::deleteLater);
        QObject::connect(noticeThread,&QThread::finished,q,[this]{
            noticeThread=nullptr;
            if(quitFinished)return;
            if(quitDataFinished)finishQuit(quitSaved,quitError);else if(quitDeadline.hasExpired())q->cancelQuit(true);
        });
        noticeThread->start();
    }
    void start(const std::shared_ptr<Job>& j){
        job=j;j->generation=generation;auto work=std::exchange(j->work,{});
        j->thread=QThread::create([j,work=std::move(work)]() mutable {
            // Release cipher state/file locks on worker return, not when a later
            // owner event loop finally deletes QThread's stored callable/future.
            auto run=std::move(work);if((!j->cancelled||j->finishWhenCancelled)&&run)run();
        });
        QObject::connect(j->thread,&QThread::finished,j->thread,&QObject::deleteLater);
        QObject::connect(j->thread,&QThread::finished,q,[this,j]{complete(j);});j->thread->start();
        publishStatus();
    }
    void complete(const std::shared_ptr<Job>& j){
        if(j->handled)return;j->handled=true;
        if(j->thread){j->thread->wait();j->thread->deleteLater();j->thread=nullptr;}
        if(job==j)job.reset();
        // Clearing the callbacks breaks the job's reference cycle once its result is used.
        auto done=std::move(j->done);j->done=nullptr;j->work=nullptr;if(done)done();
        advanceFlush();
        if(retryRequested&&!job&&!flushing&&!quitting){retryRequested=false;openAsync();}
        publishStatus();
    }
    void finishAll(){while(job){auto j=job;if(j->thread)j->thread->wait();complete(j);}}
    // Main thread only: profile checks, file lock, stale temporary files and the derived key.
    // The vault's ledger key is never created here; without it nothing is recorded.
    bool acquire(QString* message){
        if(!vault||!vault->isUnlocked())return fail(message,"시세 보관소를 열려면 금고 잠금을 해제해 주세요.");
        if(!vault->hasAssetsLedgerKey())return fail(message,"이 금고에 자산 원장 키가 아직 없습니다.");
        const auto folder=QFileInfo(path).absolutePath();
        if(!Vault::prepareProfileDirectory(folder,message)||QFileInfo(path).isSymLink()||QFileInfo(path+".lock").isSymLink())return fail(message,"시세 보관소 경로가 안전하지 않습니다.");
        if(!residualIdentity.isEmpty()){if(lockFileIdentity(path+".lock")==residualIdentity){QLockFile residual(path+".lock");if(!residual.removeStaleLockFile())return fail(message,"이전 시세 보관소 파일 잠금을 해제하지 못했습니다.");}residualIdentity.clear();}
        fileLock=std::make_unique<QLockFile>(path+".lock");fileLock->setStaleLockTime(0);if(!fileLock->tryLock(0)){fileLock.reset();return fail(message,"다른 앱이 시세 보관소를 사용하고 있습니다.");}
        bool derived=false;
        if(!vault->withAssetsLedgerKey(false,[&](const SecureBuffer& ledger){
            derived=storage_detail::deriveStoreKey(ledger,storage_detail::Kind::Price,storage_detail::Format::Current,&key,message)
                &&storage_detail::deriveStoreKey(ledger,storage_detail::Kind::Price,storage_detail::Format::Legacy,&legacyKey,message);},message)||!derived){
            key.clear();legacyKey.clear();release();return false;}
        return true;
    }
    bool setAside(QString* message){
        if(!storage_detail::rejectLegacyWrite(legacy,message))return false;
        const auto target=path+".unreadable-"+QString::number(wallClock());
        return QFile::rename(path,target)||fail(message,"읽을 수 없는 시세 보관소를 옆으로 옮기지 못했습니다.");
    }
    void apply(QHash<QString,Series>&& rows,const QByteArray& fileDigest,bool present,int dropped){
        series=std::move(rows);digest=fileDigest;fileExists=present;opened=true;changeEpoch=writtenEpoch=0;
        for(auto& s:series)s.revision=++revisionClock;
        lastFailure.clear();error.clear();retryTimer.stop();retryRequested=false;keyUnavailable=false;
        // Dropped series are rewritten away at the next save.
        if(dropped){if(!legacy)++changeEpoch;error=QStringLiteral("검증을 통과하지 못한 시세 기록 %1개를 빼고 열었어요.").arg(dropped);}
        publishStatus();emit q->changed();
    }
    void failOpen(const QString& message){
        ++failureSerial;lastFailure=message;error=message;q->lock();
        if(vault&&vault->isUnlocked())retryTimer.start(retryDelay);
        publishStatus();
    }
    void restart(const QString& reason){
        QString message;if(!setAside(&message)){failOpen(message);return;}
        apply({},{},false,0);error=reason+QStringLiteral(" · 이전 기록을 분리 보관했어요. 해당 구간은 복구된 기록이 아닙니다.");publishStatus();observe();
    }
    void openAsync(){
        if(quitting||opened||job||!vault||!vault->isUnlocked()||retryTimer.isActive())return;
        if(!vault->hasAssetsLedgerKey()){
            keyUnavailable=true;lastFailure.clear();
            error=q->exists()?QStringLiteral("원장 키가 없어 기존 시세 기록을 열지 못했어요. 기록을 만든 금고를 확인해 주세요."):
                QStringLiteral("계좌 연결 후 자산 기록이 저장용 키를 만들면 차트 기록도 저장합니다. 공개 차트 조회는 계속 사용할 수 있어요.");
            publishStatus();return;
        }
        keyUnavailable=false;
        QString message;
        try{if(!acquire(&message)){failOpen(message);return;}}catch(const std::exception&){failOpen(QStringLiteral("시세 보관소 보호 메모리 또는 키 처리에 실패했습니다."));return;}
        if(!q->exists()&&!QFileInfo::exists(path+".exit")){apply({},{},false,0);observe();return;} // Created at the first save.
        auto read=std::make_shared<ReadState>();read->path=path;read->present=q->exists();auto j=std::make_shared<Job>();
        j->work=[read]{read->notice=readExitNotice(read->path+".exit");read->result=read->present?readEncrypted(read->path,&read->bytes,&read->error):ReadResult::Ok;};
        j->done=[this,j,read]{
            if(stale(j))return;
            previousExit=exitNoticeText(read->notice,readKey(read->notice));
            if(read->result==ReadResult::Unsafe){failOpen(read->error);return;}
            if(!read->present){apply({},{},false,0);observe();return;}
            classify(read->bytes);
            // Decryption stays on this thread with the key; only plaintext rows go to the parser.
            auto parse=std::make_shared<ParseState>();QString message=read->error;
            if(read->result!=ReadResult::Ok||!decrypt(read->bytes,readKey(read->bytes),&parse->plain,&message)){if(legacy)failOpen(message);else restart(message);return;}
            const auto fileDigest=QCryptographicHash::hash(read->bytes,QCryptographicHash::Sha256);
            auto next=std::make_shared<Job>();
            next->work=[parse]{parse->ok=unpack(parse->plain,&parse->rows,&parse->dropped);wipe(parse->plain);};
            next->done=[this,next,parse,fileDigest]{
                if(stale(next))return;
                if(!parse->ok){if(legacy)failOpen(QStringLiteral("시세 보관소 형식을 읽지 못했습니다."));else restart(QStringLiteral("시세 보관소 형식을 읽지 못했습니다."));return;}
                apply(std::move(parse->rows),fileDigest,true,parse->dropped);observe();
            };
            start(next);
        };
        start(j);
    }
    void writeAsync(){
        if(!opened||!dirty()||!vault||!vault->isUnlocked())return;
        if(legacy){QString message;storage_detail::rejectLegacyWrite(true,&message);if(flushCallback)finishFlush(false,message);return;}
        if(job){if(!flushing)flushTimer.start(5000);return;}
        cleanStaleTemporaries();
        auto pk=std::make_shared<PackState>();pk->series=series;const auto epoch=changeEpoch;auto j=std::make_shared<Job>();
        for(auto it=series.cbegin();it!=series.cend();++it)pk->versions.insert(it.key(),it->revision);
        const auto before=generation;
        if(auto hook=std::exchange(afterSnapshotForTest,{}))hook();
        if(before!=generation)return;
        j->work=[pk]{pk->plain=pack(pk->series,&pk->evicted);pk->series.clear();};
        j->done=[this,j,pk,epoch]{
            if(stale(j)){wipe(pk->plain);return;}
            if(pk->plain.isEmpty()){failOpen(QStringLiteral("시세 보관소 크기 상한을 넘어 저장하지 못했습니다."));emit q->written(false);return;}
            auto save=std::make_shared<CipherWrite>();save->path=path;save->bytes=encrypt(pk->plain,key);
            QByteArray check;const bool verified=!save->bytes.isEmpty()&&decrypt(save->bytes,key,&check,nullptr)&&check==pk->plain;wipe(check);wipe(pk->plain);
            if(!verified){failOpen(QStringLiteral("저장 전 시세 보관소 암호문 검증에 실패했습니다."));emit q->written(false);return;}
            save->previous=digest;save->creating=!fileExists;save->lock=std::move(fileLock);
            const auto versions=pk->versions;const auto evicted=pk->evicted;
            auto next=std::make_shared<Job>();
            auto prepareHook=std::exchange(prepareWorkerForTest,{});
            next->work=[next,save,prepareHook=std::move(prepareHook)]{if(prepareHook)prepareHook();save->prepared=prepareCipher(*save,&next->cancelled);};
            next->done=[this,next,save,epoch,versions,evicted]{
                if(stale(next)){save->lock.reset();return;}
                if(auto hook=std::exchange(beforeCommitForTest,{}))hook();
                if(stale(next)||!vault||!vault->isUnlocked()){save->lock.reset();return;}
                if(quitting&&quitDeadline.hasExpired()){q->cancelQuit(true);return;}
                Q_ASSERT(QThread::currentThread()==q->thread());
                if(!save->prepared||!commitCipher(*save)){
                    fileLock=std::move(save->lock);failOpen(save->error);emit q->written(false);return;
                }
                auto verify=std::make_shared<Job>();verify->finishWhenCancelled=true;
                auto verifyHook=std::exchange(verifyWorkerForTest,{});
                verify->work=[save,verifyHook=std::move(verifyHook)]{if(verifyHook)verifyHook();save->ok=finishCipher(*save);};
                verify->done=[this,verify,save,epoch,versions,evicted]{
                    if(stale(verify)){save->lock.reset();return;}
                    fileLock=std::move(save->lock);
                    if(!save->ok){failOpen(save->error);emit q->written(false);return;}
                    // Apply only this snapshot's evictions, and only after durability
                    // succeeded. A revisited/recreated series has a different revision.
                    for(const auto& k:evicted){
                        const auto found=series.constFind(k);
                        if(found!=series.cend()&&found->revision==versions.value(k))series.remove(k);
                    }
                    digest=save->digest;fileExists=true;writtenEpoch=epoch;lastFailure.clear();
                    if(dirty()&&!flushing)flushTimer.start(int(flushDelay));
                    emit q->written(true);
                };
                start(verify);
            };
            start(next);
        };
        start(j);
    }
    void observe(){
        if(quitting||locking||flushing||!model||!vault||!vault->isUnlocked())return;
        const auto current=model->series();
        if(current.identity.isEmpty()){seededKey.clear();lastSignature.clear();return;} // Hidden chart: offer again when shown.
        if(!opened){openAsync();return;}
        const auto key=seriesKey(current.identity,current.intervalMs);
        if(seededKey!=key){
            // Display only, once per selection; metadata still pending retries on the next change.
            if(!model->displayObservation().candles.isEmpty())seededKey=key;
            else{const auto stored=q->stored(current.identity,current.intervalMs);
                if(stored.candles.isEmpty()||model->setStoredCompleted(stored)!=StoredCandlesOffer::NotNow)seededKey=key;}
            const auto found=series.find(key);const auto viewed=floorMinute(wallClock());
            if(!legacy&&found!=series.end()&&viewed>found->lastViewedAtMs){found->lastViewedAtMs=viewed;found->revision=++revisionClock;++changeEpoch;}
        }
        if(legacy)return;
        // Only a new REST response is recorded; stream ticks change nothing stored.
        if(dirty()&&!flushTimer.isActive())flushTimer.start(int(flushDelay));
        const auto receipt=model->restObservation();
        if(!receipt.available||receipt.restReceivedAtMs<=0)return;
        const auto signature=key+u'|'+QString::number(receipt.restRevision)+u'|'+QString::number(receipt.restReceivedAtMs);
        if(signature==lastSignature)return;lastSignature=signature;
        QString message;if(!q->record(receipt,&message).accepted){error=message;publishStatus();}
        if(dirty()&&!flushTimer.isActive())flushTimer.start(int(flushDelay));
    }
};
PriceArchive::PriceArchive(Vault* vault,QString path,QObject* parent):QObject(parent),d_(std::make_unique<Private>()){
    Q_ASSERT(vault);d_->q=this;d_->vault=vault;d_->path=QFileInfo(path).absoluteFilePath();d_->flushTimer.setSingleShot(true);
    d_->retryTimer.setSingleShot(true);
    d_->quitTimer.setSingleShot(true);d_->quitTimer.setTimerType(Qt::PreciseTimer);
    connect(&d_->quitTimer,&QTimer::timeout,this,[this]{cancelQuit(true);});
    connect(&d_->flushTimer,&QTimer::timeout,this,[this]{d_->writeAsync();});
    connect(&d_->retryTimer,&QTimer::timeout,this,&PriceArchive::retry);
    // Locking never waits on disk: rows not yet written are dropped (public data, fetched again).
    connect(vault,&Vault::aboutToLock,this,&PriceArchive::lock);
    connect(vault,&Vault::stateChanged,this,[this]{
        if(d_->vault&&d_->vault->isUnlocked())d_->observe();else if(d_->model)d_->model->setArchiveStatus({});
    });
    connect(vault,&QObject::destroyed,this,[this]{lock();d_->vault=nullptr;});
}
PriceArchive::~PriceArchive(){if(!d_->quitting&&isOpen()&&d_->dirty())flush();lock();}
QString PriceArchive::filePath()const{return d_->path;}
bool PriceArchive::exists()const{return QFileInfo::exists(d_->path);}
bool PriceArchive::isOpen()const{return d_->opened&&d_->vault&&d_->vault->isUnlocked()&&d_->key.size()==32;}
bool PriceArchive::dirty()const{return isOpen()&&d_->dirty();}
QString PriceArchive::lastError()const{return d_->error;}
void PriceArchive::setFlushDelayMs(int ms){d_->flushDelay=qMax(0,ms);}
void PriceArchive::lock(){
    Q_ASSERT(QThread::currentThread()==thread());
    if(d_->locking)return;
    const bool was=d_->opened||!d_->key.empty();d_->locking=true;
    ++d_->generation;if(d_->job)d_->job->cancelled=true;
    d_->key.clear();d_->legacyKey.clear();d_->legacy=false;d_->series.clear();d_->digest.clear();d_->release();
    d_->opened=d_->fileExists=false;d_->changeEpoch=d_->writtenEpoch=0;d_->seededKey.clear();d_->lastSignature.clear();d_->flushTimer.stop();
    d_->retryTimer.stop();d_->retryRequested=false;d_->keyUnavailable=false;d_->previousExit.clear();
    if(d_->model)d_->model->clearStoredCompleted();
    if(d_->model)d_->model->setArchiveStatus({});
    d_->locking=false;
    if(was){emit changed();emit locked();}
    if(d_->flushCallback)QTimer::singleShot(0,this,[this]{d_->advanceFlush();});
}
bool PriceArchive::open(bool create,QString* error){
    Q_ASSERT(QThread::currentThread()==thread());
    if(d_->quitting||d_->flushing)return fail(error,"시세 기록 저장을 마무리 중입니다.");
    if(error)error->clear();d_->finishAll();if(isOpen())return true;
    if(!d_->vault||!d_->vault->isUnlocked())return fail(error,"시세 보관소를 열려면 금고 잠금을 해제해 주세요.");
    const bool present=exists();if(!present&&!create)return fail(error,"저장된 시세 보관소가 없습니다.");
    try{
        QString message,recovery;d_->retryTimer.stop();
        if(!d_->acquire(&message)){lock();return fail(error,message);}
        const auto notice=readExitNotice(d_->path+".exit");d_->previousExit=exitNoticeText(notice,d_->readKey(notice));
        if(present){
            QByteArray bytes,plain;QHash<QString,Series> rows;int dropped=0;
            const auto read=readEncrypted(d_->path,&bytes,&message);if(read==ReadResult::Unsafe){lock();return fail(error,message);}
            d_->classify(bytes);
            const bool decrypted=read==ReadResult::Ok&&decrypt(bytes,d_->readKey(bytes),&plain,&message);
            const bool readable=decrypted&&unpack(plain,&rows,&dropped);wipe(plain);
            if(readable){d_->apply(std::move(rows),QCryptographicHash::hash(bytes,QCryptographicHash::Sha256),true,dropped);return true;}
            if(decrypted)message=QStringLiteral("시세 보관소 형식을 읽지 못했습니다.");
            if(!create||d_->legacy){lock();return fail(error,message);}
            if(!d_->setAside(&message)){lock();return fail(error,message);}
            recovery=message+QStringLiteral(" · 이전 기록을 분리 보관하고 새로 시작했어요. 이전 구간은 복구되지 않았습니다.");
        }
        d_->apply({},{},false,0);d_->error=recovery;const auto saved=d_->error;++d_->changeEpoch;
        if(!flush(error))return false; // An explicit create writes the empty archive now.
        d_->error=saved;d_->publishStatus();return true;
    }catch(const std::exception&){lock();return fail(error,"시세 보관소 보호 메모리 또는 암호화 처리에 실패했습니다.");}
}
PriceArchiveMerge PriceArchive::record(const MarketCandleSeries& input,QString* error){
    Q_ASSERT(QThread::currentThread()==thread());
    PriceArchiveMerge result;if(error)error->clear();
    if(!isOpen()){fail(error,"시세 보관소가 잠겨 있습니다.");return result;}
    if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return result;
    const auto expected=MarketCandles::request(input.identity,input.intervalMs).url;const auto interval=input.intervalMs,received=input.restReceivedAtMs;
    if(input.fromArchive||expected.isEmpty()||input.sourceUrl!=expected.toString()||!consistent(input.identity,input.asset,input.currency)||received<=0){fail(error,"지원하지 않는 캔들 원천이거나 종목 정보가 맞지 않습니다.");return result;}
    const auto key=seriesKey(input.identity,interval);const auto found=d_->series.constFind(key);const Series* current=found==d_->series.cend()?nullptr:&*found;
    if(current&&(current->asset!=input.asset||current->currency!=input.currency)){fail(error,"같은 캔들 원천의 종목 정보가 달라 저장하지 않았습니다.");return result;}
    QList<Row> rows;qint64 previous=0;
    for(const auto& candle:input.candles){
        if(candle.startsAtMs<=previous){fail(error,"캔들 순서가 올바르지 않아 저장하지 않았습니다.");return result;}previous=candle.startsAtMs;
        // Rows a public stream closed after the REST response wait for the next REST receipt.
        if(candle.startsAtMs>input.restThroughMs){++result.pending;continue;}
        if(candle.startsAtMs>std::numeric_limits<qint64>::max()-interval-SettleMs||candle.startsAtMs+interval-1>received){fail(error,"완료된 캔들 형식이 아니어서 저장하지 않았습니다.");return result;}
        // A candle read right at its close may still miss late trades; keep it after it settles.
        if(candle.startsAtMs+interval+SettleMs>received){++result.pending;continue;}
        if(current){const auto row=current->rows.constFind(candle.startsAtMs);if(row!=current->rows.cend()&&sameValues(row->candle,candle)){++result.unchanged;continue;}}
        const Row row{candle,floorMinute(received)};
        if(!validRow(row,interval)){fail(error,"완료된 캔들 형식이 아니어서 저장하지 않았습니다.");return result;}
        rows.append(row);
    }
    result.accepted=true;
    if(!current&&rows.isEmpty())return result;
    if(!current){
        if(d_->series.size()>=MaxSeries)d_->series.remove(victim(d_->series,key));
        Series s;s.identity=input.identity;s.intervalMs=interval;s.asset=input.asset;s.currency=input.currency;s.source=feed(input.identity,interval);d_->series.insert(key,s);
    }
    auto& s=d_->series[key];const auto viewed=qMax(s.lastViewedAtMs,floorMinute(received));
    const bool viewChanged=viewed!=s.lastViewedAtMs;s.lastViewedAtMs=viewed;
    for(const auto& row:rows){
        const auto existing=s.rows.constFind(row.candle.startsAtMs);
        if(existing==s.rows.cend()){s.rows.insert(row.candle.startsAtMs,row);++result.added;continue;}
        const bool known=std::any_of(s.conflicts.cbegin(),s.conflicts.cend(),[&](const Row& r){return r.candle.startsAtMs==row.candle.startsAtMs&&sameValues(r.candle,row.candle);});
        if(known)continue;
        if(s.conflicts.size()<MaxConflictsPerSeries){s.conflicts.append(row);++result.conflicts;}
        else{if(s.conflictOverflow<std::numeric_limits<int>::max())++s.conflictOverflow;++result.droppedConflicts;}
    }
    while(s.rows.size()>MaxCandlesPerSeries)s.rows.erase(s.rows.begin());
    const auto first=s.rows.isEmpty()?std::numeric_limits<qint64>::max():s.rows.firstKey();
    s.conflicts.erase(std::remove_if(s.conflicts.begin(),s.conflicts.end(),[&](const Row& r){return r.candle.startsAtMs<first;}),s.conflicts.end());
    const bool rowsChanged=result.added||result.conflicts||result.droppedConflicts;
    if(rowsChanged)s.updatedAtMs=qMax(s.updatedAtMs,floorMinute(received));
    if(rowsChanged||viewChanged){s.revision=++d_->revisionClock;++d_->changeEpoch;emit changed();}
    return result;
}
bool PriceArchive::flush(QString* error){
    Q_ASSERT(QThread::currentThread()==thread());
    if(error)error->clear();
    if(d_->quitting||d_->flushing)return fail(error,"시세 기록 저장을 마무리 중입니다.");
    const auto failures=d_->failureSerial,generation=d_->generation;const bool waiting=bool(d_->job);
    d_->finishAll();
    if(failures!=d_->failureSerial||!d_->lastFailure.isEmpty())return fail(error,d_->lastFailure);
    if(waiting&&generation!=d_->generation)return fail(error,"잠금으로 시세 기록 저장을 중단했습니다.");
    if(!isOpen())return true;if(!d_->dirty())return true;
    if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
    try{
        d_->cleanStaleTemporaries();
        QStringList evicted;auto plain=pack(d_->series,&evicted);const auto epoch=d_->changeEpoch;
        if(plain.isEmpty()){d_->failOpen(QStringLiteral("시세 보관소 크기 상한을 넘어 저장하지 못했습니다."));return fail(error,d_->error);}
        CipherWrite save;save.path=d_->path;save.bytes=encrypt(plain,d_->key);save.previous=d_->digest;save.creating=!d_->fileExists;
        QByteArray check;const bool verified=!save.bytes.isEmpty()&&decrypt(save.bytes,d_->key,&check,nullptr)&&check==plain;wipe(check);wipe(plain);
        if(!verified||!prepareCipher(save,nullptr)||!commitCipher(save)||!finishCipher(save)){
            if(save.error.isEmpty())save.error=QStringLiteral("시세 보관소 암호문 검증에 실패했습니다.");
            d_->failOpen(save.error);return fail(error,save.error);
        }
        for(const auto& k:evicted)d_->series.remove(k);
        d_->digest=save.digest;d_->fileExists=true;d_->writtenEpoch=epoch;d_->flushTimer.stop();emit written(true);return true;
    }catch(const std::exception&){d_->failOpen(QStringLiteral("시세 보관소 저장 실패로 보관소를 잠갔습니다."));return fail(error,d_->error);}
}
void PriceArchive::flushAsync(FlushCallback callback){
    Q_ASSERT(QThread::currentThread()==thread());
    if(!callback)return;
    if(d_->flushing){callback(false,QStringLiteral("시세 기록 저장을 마무리 중입니다."));return;}
    // Admission is intentionally before all old error/job checks. Do not open an
    // archive, read the chart, create a key or wait for an old failure on clean exit.
    if(!isOpen()||!dirty()){callback(true,{});return;}
    d_->flushing=true;d_->flushCallback=std::move(callback);
    d_->flushGeneration=d_->generation;d_->flushFailureSerial=d_->failureSerial;
    d_->flushTimer.stop();d_->advanceFlush();d_->publishStatus();
}
void PriceArchive::prepareToQuit(FlushCallback callback){
    Q_ASSERT(QThread::currentThread()==thread());
    if(d_->quitting){if(callback&&d_->quitFinished)callback(d_->quitSaved,d_->quitError);return;}
    d_->quitting=true;d_->quitCallback=std::move(callback);
    d_->quitDeadline=QDeadlineTimer(QuitWaitMs,Qt::PreciseTimer);
    if(!isOpen()||!dirty()){d_->quitSaved=true;d_->finishQuit(true,{});return;}
    d_->quitTimer.start(QuitWaitMs);d_->startExitNotice();
    flushAsync([this](bool ok,const QString& message){d_->finishQuitData(ok,message);});
}
void PriceArchive::cancelQuit(bool timedOut){
    Q_ASSERT(QThread::currentThread()==thread());
    if(d_->quitFinished)return;
    if(!d_->quitting){
        d_->quitting=true;d_->quitDeadline=QDeadlineTimer(0);
        if(isOpen()&&dirty())d_->startExitNotice();
    }
    if(d_->exitNotice)d_->exitNotice->decide(timedOut?ExitNotice::Timeout:ExitNotice::Interrupted);
    d_->quitSaved=d_->quitDataFinished&&d_->quitSaved;
    if(!d_->quitSaved)d_->quitError=timedOut?QStringLiteral("시세 기록 저장 3초 대기 한도 초과"):QStringLiteral("시세 기록 저장 중단");
    d_->finishQuit(d_->quitSaved,d_->quitError);
}
void PriceArchive::retry(){
    if(d_->quitting||!d_->vault||!d_->vault->isUnlocked()||d_->flushing)return;
    d_->retryTimer.stop();
    if(d_->job){d_->retryRequested=true;return;}
    d_->retryRequested=false;
    if(isOpen()){if(d_->dirty())d_->writeAsync();return;}
    d_->openAsync();
}
void PriceArchive::setBeforeCommitForTest(std::function<void()> hook){d_->beforeCommitForTest=std::move(hook);}
void PriceArchive::setAfterSnapshotForTest(std::function<void()> hook){d_->afterSnapshotForTest=std::move(hook);}
void PriceArchive::setRetryDelayForTest(int ms){d_->retryDelay=qMax(1,ms);}
void PriceArchive::setPrepareWorkerForTest(std::function<void()> hook){d_->prepareWorkerForTest=std::move(hook);}
void PriceArchive::setVerifyWorkerForTest(std::function<void()> hook){d_->verifyWorkerForTest=std::move(hook);}
void PriceArchive::startWriteForTest(){d_->writeAsync();}
MarketCandleSeries PriceArchive::stored(const QString& identity,qint64 interval,int maxCount)const{
    MarketCandleSeries out;out.identity=identity;out.intervalMs=interval;
    if(!isOpen()||maxCount<=0)return out;
    const auto found=d_->series.constFind(seriesKey(identity,interval));if(found==d_->series.cend()||found->rows.isEmpty())return out;
    QList<MarketCandle> run;qint64 received=0;auto it=found->rows.cend();
    while(it!=found->rows.cbegin()&&run.size()<maxCount){
        --it;if(!run.isEmpty()&&it->candle.startsAtMs+interval!=run.first().startsAtMs)break;
        run.prepend(it->candle);received=qMax(received,it->receivedAtMs);
    }
    out.available=true;out.candles=run;out.receivedAtMs=received;out.asset=found->asset;out.currency=found->currency;
    out.sourceUrl=MarketCandles::request(identity,interval).url.toString();out.fromArchive=true;out.reason=QStringLiteral("저장된 캔들");return out;
}
int PriceArchive::storedCount(const QString& identity,qint64 interval)const{
    if(!isOpen())return 0;const auto found=d_->series.constFind(seriesKey(identity,interval));return found==d_->series.cend()?0:int(found->rows.size());
}
int PriceArchive::conflictCount(const QString& identity,qint64 interval)const{
    if(!isOpen())return 0;const auto found=d_->series.constFind(seriesKey(identity,interval));return found==d_->series.cend()?0:int(found->conflicts.size());
}
int PriceArchive::conflictOverflow(const QString& identity,qint64 interval)const{
    if(!isOpen())return 0;const auto found=d_->series.constFind(seriesKey(identity,interval));return found==d_->series.cend()?0:found->conflictOverflow;
}
void PriceArchive::watch(MarketCandles* model){
    if(d_->model){disconnect(d_->model,nullptr,this,nullptr);d_->model->setArchiveStatus({});}
    d_->model=model;d_->seededKey.clear();d_->lastSignature.clear();
    if(model){
        connect(model,&MarketCandles::changed,this,[this]{d_->observe();});
        connect(model,&MarketCandles::archiveRetryRequested,this,&PriceArchive::retry);
    }
    d_->publishStatus();d_->observe();
}
namespace storage_detail {
bool validatePricePayload(const SecureBuffer& plain,bool exitNotice,QString* error){
    auto bytes=QByteArray(reinterpret_cast<const char*>(plain.data()),qsizetype(plain.size()));bool valid=false;
    if(exitNotice){
        if(bytes.size()==256){
            while(!bytes.isEmpty()&&bytes.back()=='\0')bytes.chop(1);
            QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);auto canonical=doc.toJson(QJsonDocument::Compact);
            const auto object=doc.object();
            valid=parse.error==QJsonParseError::NoError&&doc.isObject()&&canonical==bytes&&object.size()==2
                &&object.value("purpose")==QJsonValue("price-archive-exit/1")&&object.value("reason").isString()
                &&QStringList{"pending","timeout","failed","external-change","unsafe-path","disk","encryption","interrupted"}.contains(object.value("reason").toString());
            wipe(canonical);
        }
    }else{
        QHash<QString,Series> rows;int dropped=0;valid=unpack(bytes,&rows,&dropped)&&dropped==0;
    }
    wipe(bytes);
    return valid||fail(error,QStringLiteral("시세 보관소의 원문 전체를 손실 없이 검증하지 못했습니다."));
}
} // namespace storage_detail
} // namespace keefi
