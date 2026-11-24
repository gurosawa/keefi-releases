#include "keefi/SwapJournal.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/Vault.h"
#include <QUuid>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include "StagedStorageFile.h"
#include <QUrl>
#include <QtEndian>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <stdexcept>
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
#include <aclapi.h>
#include <io.h>
#endif
namespace keefi {
namespace {
constexpr qsizetype HeaderBytes=40;
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}
#ifdef Q_OS_WIN
bool privateWindowsPath(const QString& path, bool setAcl) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    const bool gotUser = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    if (!gotUser) return false;
    const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    auto* name = const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(path.utf16()));
    if (setAcl) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
        const DWORD status = SetNamedSecurityInfoW(name, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
        if (status != ERROR_SUCCESS) return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    if (GetNamedSecurityInfoW(name, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                             &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    bool valid = owner && EqualSid(owner, sid) && acl;
    for (DWORD i = 0; valid && i < acl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(acl, i, &ace)) { valid = false; break; }
        const auto* head = static_cast<ACE_HEADER*>(ace);
        if (head->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            valid = EqualSid(const_cast<DWORD*>(&allowed->SidStart), sid);
        } else if (head->AceType != ACCESS_DENIED_ACE_TYPE) {
            valid = false;
        }
    }
    LocalFree(descriptor);
    return valid;
}
#endif

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
    // macOS extended ACL grants need not be reflected by the Unix mode bits.
    // Do not rewrite an existing ACL. Conservatively require an empty ACL.
    // acl_get_file returns NULL/ENOENT both for a missing path and for an
    // existing object without an ACL. Query the property after a successful
    // lstatx instead of treating those two cases as interchangeable.
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

bool readEncrypted(const QString& path,QByteArray* bytes,QString* error){
    if(!privatePath(path,false,error))return false;QFile file(path);
#ifdef Q_OS_UNIX
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
    if(fd<0)return fail(error,"스왑 기록 파일을 열지 못했습니다.");
    if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)){::close(fd);return fail(error,"스왑 기록 접근 권한이 안전하지 않습니다.");}
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return fail(error,"스왑 기록을 읽지 못했습니다.");}
#else
    if(!file.open(QIODevice::ReadOnly))return fail(error,"스왑 기록을 읽지 못했습니다.");
#endif
    if(file.size()<HeaderBytes+16||file.size()>SwapJournal::MaxFileBytes)return fail(error,"스왑 기록 크기 상한 또는 형식 오류입니다.");
    *bytes=file.read(SwapJournal::MaxFileBytes+1);return (file.error()==QFile::NoError&&bytes->size()==file.size())||fail(error,"스왑 기록 전체 읽기에 실패했습니다.");
}
bool decrypt(const QByteArray& bytes,const SecureBuffer& key,SecureBuffer* plain,QString* error){
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(key.size()!=32||bytes.size()<HeaderBytes+16||bytes.size()>SwapJournal::MaxFileBytes||storage_detail::formatOf(bytes,storage_detail::Kind::Swap)==storage_detail::Format::Invalid||qFromLittleEndian<quint16>(p+8)!=1||qFromLittleEndian<quint16>(p+10)!=0||qFromLittleEndian<quint32>(p+36)!=bytes.size()-HeaderBytes)return fail(error,"지원하지 않거나 손상된 스왑 기록 형식입니다.");
    SecureBuffer result(std::size_t(bytes.size()-HeaderBytes-16));unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(result.data(),&size,nullptr,p+HeaderBytes,bytes.size()-HeaderBytes,p,HeaderBytes,p+12,key.data())||size!=result.size())return fail(error,"스왑 기록 인증에 실패했습니다. 다른 금고의 원장일 수 있습니다.");
    *plain=std::move(result);return true;
}
QByteArray encrypt(const SecureBuffer& plain,const SecureBuffer& key){
    QByteArray out(HeaderBytes+qsizetype(plain.size())+16,'\0');auto* p=reinterpret_cast<unsigned char*>(out.data());std::memcpy(p,"KEEFISWP",8);qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+36);
    unsigned long long size=0;if(key.size()!=32||crypto_aead_xchacha20poly1305_ietf_encrypt(p+HeaderBytes,&size,plain.data(),plain.size(),p,HeaderBytes,nullptr,p+12,key.data())||size!=plain.size()+16)return {};return out;
}
bool persist(const QString& path,const SecureBuffer& key,const SecureBuffer& plain,const QByteArray& previous,bool creating,QByteArray* digest,QString* error){
    const auto unchanged=[&]{if(creating)return (!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink())||fail(error,"스왑 기록 파일이 이미 생겨 저장을 중단했습니다.");QByteArray bytes;return readEncrypted(path,&bytes,error)&&(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)==previous||fail(error,"스왑 기록이 외부에서 변경되어 저장을 중단했습니다."));};
    if(!unchanged())return false;const auto bytes=encrypt(plain,key);if(bytes.isEmpty())return fail(error,"스왑 기록 암호화에 실패했습니다.");
    StagedStorageFile staged(QFileInfo(path).absolutePath()+"/.keefi-assets-XXXXXX");
    auto& temp = staged.file();
    if(!temp.open())return fail(error,"스왑 기록 임시 파일을 만들지 못했습니다.");
#ifdef Q_OS_WIN
    if(!privateWindowsPath(temp.fileName(),true))return fail(error,"스왑 기록 임시 파일을 보호하지 못했습니다.");
#else
    if(!temp.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner))return fail(error,"스왑 기록 임시 파일을 보호하지 못했습니다.");
#endif
    if(!privatePath(temp.fileName(),false,error)||temp.write(bytes)!=bytes.size()||!temp.flush())return fail(error,"스왑 기록 저장에 실패했습니다. 기존 파일을 보존했습니다.");
#ifdef Q_OS_UNIX
    if(::fsync(temp.handle()))return fail(error,"스왑 기록을 디스크에 반영하지 못했습니다.");
#elif defined(Q_OS_WIN)
    if(!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(temp.handle()))))return fail(error,"스왑 기록을 디스크에 반영하지 못했습니다.");
#endif
    if(!temp.seek(0))return fail(error,"스왑 기록 검증 읽기에 실패했습니다.");const auto readback=temp.read(SwapJournal::MaxFileBytes+1);SecureBuffer check;
    if(readback!=bytes||!decrypt(readback,key,&check,error)||check.size()!=plain.size()||sodium_memcmp(check.data(),plain.data(),plain.size()))return fail(error,"스왑 기록 저장 후 검증에 실패했습니다.");
    if(!unchanged()||!privatePath(QFileInfo(path).absolutePath(),true,error))return false;const auto name=temp.fileName();temp.close();
#ifdef Q_OS_UNIX
    const int directory=::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(),O_RDONLY|O_CLOEXEC);if(directory<0)return fail(error,"스왑 기록 폴더를 열지 못했습니다.");
    const int renamed=::rename(QFile::encodeName(name).constData(),QFile::encodeName(path).constData());const int synced=renamed ? 0 : ::fsync(directory);::close(directory);
    if(renamed)return fail(error,"스왑 기록 원자 교체에 실패했습니다.");if(synced)return fail(error,"교체 후 디스크 반영을 확인하지 못했습니다. 원장을 다시 열어 주세요.");
#elif defined(Q_OS_WIN)
    if(!staged.publish(path,!creating))return fail(error,"스왑 기록 원자 교체에 실패했습니다.");
#else
    return fail(error,"이 운영체제의 원장 원자 교체는 지원하지 않습니다.");
#endif
    QByteArray finalBytes;if(!readEncrypted(path,&finalBytes,error)||finalBytes!=bytes)return fail(error,"교체된 원장을 확인하지 못했습니다. 다시 열어 주세요.");*digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);return true;
}
bool uuid(const QString& s){const QUuid id(s);return !id.isNull()&&id.toString(QUuid::WithoutBraces)==s;}
bool uint64(const QString& s,bool positive=false,bool optional=false){
    if(optional&&s.isEmpty())return true;static const QRegularExpression re("\\A[0-9]{1,20}\\z");bool ok=false;const auto n=s.toULongLong(&ok);return re.match(s).hasMatch()&&ok&&(!positive||n>0);
}
bool bounded(const QString& s,qsizetype max){if(s.isEmpty()||s.size()>max)return false;for(const auto c:s)if(c.category()==QChar::Other_Control||c.category()==QChar::Other_Format||c.isSurrogate())return false;return true;}
QJsonObject encode(const SwapPendingRecord& r){return {
    {"id",r.id},{"phantomAccountId",r.phantomAccountId},{"jupiterAccountId",r.jupiterAccountId},{"credentialRef",r.credentialRef},{"chain",r.chain},{"wallet",r.wallet},{"inputMint",r.inputMint},{"outputMint",r.outputMint},
    {"inputAtomic",r.inputAtomic},{"minimumRouteOutput",r.minimumRouteOutput},{"slippageBps",r.slippageBps},{"requestId",r.requestId},{"transactionId",r.transactionId},{"messageHash",r.messageHash},{"lastValidBlockHeight",r.lastValidBlockHeight},
    {"createdAt",r.createdAt},{"state",r.state},{"confirmedSlot",r.confirmedSlot},{"actualFeeLamports",r.actualFeeLamports},{"actualInputAtomic",r.actualInputAtomic},{"actualOutputAtomic",r.actualOutputAtomic}};}
bool valid(const SwapPendingRecord& r){
    static const QRegularExpression hash("\\A[a-f0-9]{64}\\z");
    bool millisOk=false;const auto millis=r.createdAt.toLongLong(&millisOk);
    if(!uuid(r.id)||!uuid(r.phantomAccountId)||!uuid(r.jupiterAccountId)||!uuid(r.credentialRef)||r.chain!="solana-mainnet"||
        !SolanaTransactionCodec::publicKeyValid(r.wallet)||!SolanaTransactionCodec::publicKeyValid(r.inputMint)||!SolanaTransactionCodec::publicKeyValid(r.outputMint)||r.inputMint==r.outputMint||
        !uint64(r.inputAtomic,true)||!uint64(r.minimumRouteOutput,true)||!uint64(r.slippageBps)||r.slippageBps.toULongLong()>10000||!bounded(r.requestId,128)||
        SolanaTransactionCodec::decodeBase58(r.transactionId,64).size()!=64||!hash.match(r.messageHash).hasMatch()||!uint64(r.lastValidBlockHeight,true)||
        !millisOk||millis<=0||millis>253402300799999LL||QString::number(millis)!=r.createdAt||
        !QStringList{"signed_not_submitted","submitted_unknown","confirmed","finalized","failed_onchain"}.contains(r.state)||
        !uint64(r.confirmedSlot,true,true)||!uint64(r.actualFeeLamports,false,true)||!uint64(r.actualInputAtomic,false,true)||!uint64(r.actualOutputAtomic,false,true))return false;
    const bool terminal=r.state=="confirmed"||r.state=="finalized"||r.state=="failed_onchain";
    return terminal?!r.confirmedSlot.isEmpty():r.confirmedSlot.isEmpty()&&r.actualFeeLamports.isEmpty()&&r.actualInputAtomic.isEmpty()&&r.actualOutputAtomic.isEmpty();
}
bool decode(const QJsonObject& o,SwapPendingRecord* r){
    if(o.size()!=21)return false;
    const auto get=[&](const char* name,QString* field){if(!o[name].isString())return false;*field=o[name].toString();return true;};
    return get("id",&r->id)&&get("phantomAccountId",&r->phantomAccountId)&&get("jupiterAccountId",&r->jupiterAccountId)&&get("credentialRef",&r->credentialRef)&&get("chain",&r->chain)&&get("wallet",&r->wallet)&&
        get("inputMint",&r->inputMint)&&get("outputMint",&r->outputMint)&&get("inputAtomic",&r->inputAtomic)&&get("minimumRouteOutput",&r->minimumRouteOutput)&&get("slippageBps",&r->slippageBps)&&get("requestId",&r->requestId)&&
        get("transactionId",&r->transactionId)&&get("messageHash",&r->messageHash)&&get("lastValidBlockHeight",&r->lastValidBlockHeight)&&get("createdAt",&r->createdAt)&&get("state",&r->state)&&get("confirmedSlot",&r->confirmedSlot)&&
        get("actualFeeLamports",&r->actualFeeLamports)&&get("actualInputAtomic",&r->actualInputAtomic)&&get("actualOutputAtomic",&r->actualOutputAtomic)&&valid(*r);
}
bool unpack(const SecureBuffer& plain,QList<SwapPendingRecord>* rows){
    auto bytes=QByteArray(reinterpret_cast<const char*>(plain.data()),qsizetype(plain.size()));QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
    const bool canonical=doc.toJson(QJsonDocument::Compact)==bytes;sodium_memzero(bytes.data(),std::size_t(bytes.size()));
    if(error.error!=QJsonParseError::NoError||!canonical||!doc.isObject())return false;const auto o=doc.object();
    if(o.size()!=2||o["schema_version"]!=QJsonValue(1)||!o["records"].isArray()||o["records"].toArray().size()>SwapJournal::MaxRecords)return false;
    QSet<QString> ids,transactions,requests;
    for(const auto& value:o["records"].toArray()){SwapPendingRecord r;if(!value.isObject()||!decode(value.toObject(),&r)||ids.contains(r.id)||transactions.contains(r.transactionId)||requests.contains(r.wallet+QChar(0)+r.requestId))return false;
        ids.insert(r.id);transactions.insert(r.transactionId);requests.insert(r.wallet+QChar(0)+r.requestId);rows->append(r);}
    return true;
}
SecureBuffer pack(const QList<SwapPendingRecord>& rows){
    QJsonArray array;for(const auto& r:rows)array.append(encode(r));auto bytes=QJsonDocument(QJsonObject{{"schema_version",1},{"records",array}}).toJson(QJsonDocument::Compact);
    if(bytes.size()>SwapJournal::MaxFileBytes-HeaderBytes-16){sodium_memzero(bytes.data(),std::size_t(bytes.size()));return {};}
    SecureBuffer out(bytes.constData(),std::size_t(bytes.size()));sodium_memzero(bytes.data(),std::size_t(bytes.size()));return out;
}
bool allowedUpdate(const SwapPendingRecord& before,const SwapPendingRecord& after){
    auto old=encode(before),next=encode(after);for(const auto* name:{"state","confirmedSlot","actualFeeLamports","actualInputAtomic","actualOutputAtomic"}){old.remove(name);next.remove(name);}if(old!=next)return false;
    if((before.state=="finalized"||before.state=="failed_onchain")&&before.state!=after.state)return false;
    if(before.state=="confirmed"&&after.state!="confirmed"&&after.state!="finalized")return false;
    if(before.state=="submitted_unknown"&&after.state=="signed_not_submitted")return false;
    for(const auto pair:{qMakePair(before.confirmedSlot,after.confirmedSlot),qMakePair(before.actualFeeLamports,after.actualFeeLamports),qMakePair(before.actualInputAtomic,after.actualInputAtomic),qMakePair(before.actualOutputAtomic,after.actualOutputAtomic)})
        if(!pair.first.isEmpty()&&pair.first!=pair.second)return false;
    return true;
}
} // namespace
struct SwapJournal::Private {
    Vault* vault=nullptr;QString path;SecureBuffer key,plain;QByteArray digest,residualIdentity;std::unique_ptr<QLockFile> fileLock;bool legacy=false;
    void release(){if(fileLock&&fileLock->isLocked()){const auto identity=lockFileIdentity(path+".lock");fileLock->unlock();residualIdentity=!identity.isEmpty()&&lockFileIdentity(path+".lock")==identity?identity:QByteArray{};}fileLock.reset();}
};
SwapJournal::SwapJournal(Vault* vault,QString path,QObject* parent):QObject(parent),d_(std::make_unique<Private>()){
    Q_ASSERT(vault);d_->vault=vault;d_->path=QFileInfo(path).absoluteFilePath();connect(vault,&Vault::aboutToLock,this,&SwapJournal::lock);connect(vault,&QObject::destroyed,this,[this]{lock();d_->vault=nullptr;});
}
SwapJournal::~SwapJournal(){lock();}
bool SwapJournal::isOpen()const{return d_->vault&&d_->vault->isUnlocked()&&d_->key.size()==32&&!d_->plain.empty();}
void SwapJournal::lock(){const bool was=!d_->key.empty()||!d_->plain.empty();d_->key.clear();d_->plain.clear();d_->digest.clear();d_->legacy=false;d_->release();if(was){emit changed();emit locked();}}
bool SwapJournal::open(bool create,QString* error){
    if(error)error->clear();if(isOpen())return true;if(!d_->vault||!d_->vault->isUnlocked())return fail(error,"스왑 기록을 열려면 금고 잠금을 해제해 주세요.");
    const bool present=QFileInfo::exists(d_->path);if(!present&&!create)return fail(error,"저장된 스왑 기록이 없습니다.");
    try{
        if(!Vault::prepareProfileDirectory(QFileInfo(d_->path).absolutePath(),error)||QFileInfo(d_->path).isSymLink()||QFileInfo(d_->path+".lock").isSymLink())return fail(error,"스왑 기록 경로가 안전하지 않습니다.");
        if(!d_->residualIdentity.isEmpty()){if(lockFileIdentity(d_->path+".lock")==d_->residualIdentity){QLockFile residual(d_->path+".lock");if(!residual.removeStaleLockFile())return fail(error,"이전 스왑 기록 파일 잠금을 해제하지 못했습니다.");}d_->residualIdentity.clear();}
        d_->fileLock=std::make_unique<QLockFile>(d_->path+".lock");d_->fileLock->setStaleLockTime(0);if(!d_->fileLock->tryLock(0)){lock();return fail(error,"다른 앱이 스왑 기록을 사용하고 있습니다.");}
        QByteArray bytes;if(present&&!readEncrypted(d_->path,&bytes,error)){lock();return false;}
        const auto format=present?storage_detail::formatOf(bytes,storage_detail::Kind::Swap):storage_detail::Format::Current;
        d_->legacy=format==storage_detail::Format::Legacy;bool derived=false;
        if(!d_->vault->withAssetsLedgerKey(create&&!present,[&](const SecureBuffer& key){
            derived=storage_detail::deriveStoreKey(key,storage_detail::Kind::Swap,format,&d_->key,error);},error)||!derived){lock();return false;}
        if(present){QList<SwapPendingRecord> rows;if(!decrypt(bytes,d_->key,&d_->plain,error)||!unpack(d_->plain,&rows)){lock();return fail(error,"스왑 기록 인증·스키마 검증에 실패했습니다.");}d_->digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
        else{d_->plain=pack({});if(!persist(d_->path,d_->key,d_->plain,{},true,&d_->digest,error)){lock();return false;}}
        emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"스왑 기록 보호 메모리 또는 암호화 처리에 실패했습니다.");}
}
QList<SwapPendingRecord> SwapJournal::records()const{QList<SwapPendingRecord> rows;if(isOpen())unpack(d_->plain,&rows);return rows;}
bool SwapJournal::upsert(const SwapPendingRecord& row,QString* error){
    if(error)error->clear();if(!isOpen())return fail(error,"스왑 기록이 잠겨 있습니다.");if(!valid(row))return fail(error,"스왑 기록의 식별자·수량·상태 형식이 올바르지 않습니다.");
    try{
        QList<SwapPendingRecord> next;if(!unpack(d_->plain,&next)){lock();return fail(error,"스왑 기록 메모리 검증에 실패했습니다.");}
        auto existing=std::find_if(next.begin(),next.end(),[&](const auto& r){return r.id==row.id;});
        if(existing!=next.end()){if(!allowedUpdate(*existing,row))return fail(error,"서명된 요청 내용 또는 확정 결과를 변경할 수 없습니다.");if(*existing==row)return true;*existing=row;}
        else{
            if(next.size()>=MaxRecords)return fail(error,"스왑 기록 보존 상한에 도달했습니다. 기존 기록은 보존했습니다.");
            for(const auto& r:next)if(r.transactionId==row.transactionId||(r.wallet==row.wallet&&r.requestId==row.requestId))return fail(error,"같은 서명·요청을 다른 기록으로 중복 저장할 수 없습니다.");
            next.append(row);
        }
        auto plain=pack(next);if(plain.empty())return fail(error,"스왑 기록 크기 상한을 초과했습니다.");
        if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
        if(!persist(d_->path,d_->key,plain,d_->digest,false,&d_->digest,error)){lock();return false;}
        d_->plain=std::move(plain);emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"스왑 기록 저장 실패로 기록을 잠갔습니다.");}
}
namespace storage_detail {
bool validateSwapPayload(const SecureBuffer& plain,QString* error){
    QList<SwapPendingRecord> rows;
    return unpack(plain,&rows)||fail(error,QStringLiteral("스왑 기록의 인증된 JSON 계약이 올바르지 않습니다."));
}
} // namespace storage_detail
} // namespace keefi
