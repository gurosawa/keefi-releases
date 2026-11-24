#include "keefi/StorageMigration.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include "keefi/ProfileMigrationPrivate.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/Vault.h"
#include "keefi/AssetsLedger.h"
#include "keefi/SwapJournal.h"
#include "keefi/PriceArchive.h"
#include "keefi/RecordTransfer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <sodium.h>
#include <algorithm>
#include <exception>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace keefi {
namespace {
using storage_detail::Kind;
using storage_detail::Format;
bool fail(QString* error,const QString& text){if(error)*error=text;return false;}
QString digest(const QByteArray& bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
qsizetype maximum(Kind kind){
    switch(kind){
    case Kind::Vault:return Vault::MaxFileBytes;
    case Kind::Assets:return AssetsLedger::MaxFileBytes;
    case Kind::Swap:return SwapJournal::MaxFileBytes;
    case Kind::Price:return PriceArchive::MaxFileBytes;
    case Kind::Imported:case Kind::Package:return RecordTransfer::MaxFileBytes;
    }return 0;
}
bool equal(const SecureBuffer& a,const SecureBuffer& b){
    return a.size()==b.size()&&(a.empty()||sodium_memcmp(a.data(),b.data(),a.size())==0);
}
QJsonObject json(const SecureBuffer& plain){
    auto bytes=QByteArray(reinterpret_cast<const char*>(plain.data()),qsizetype(plain.size()));
    auto object=QJsonDocument::fromJson(bytes).object();
    if(!bytes.isEmpty())sodium_memzero(bytes.data(),std::size_t(bytes.size()));
    return object;
}
bool validate(const SecureBuffer& plain,Kind kind,bool legacy,bool exitNotice,QString* error){
    switch(kind){
    case Kind::Assets:return storage_detail::validateAssetsPayload(plain,legacy,error);
    case Kind::Swap:return storage_detail::validateSwapPayload(plain,error);
    case Kind::Price:return storage_detail::validatePricePayload(plain,exitNotice,error);
    case Kind::Imported:return storage_detail::validateImportedPayload(plain,error);
    default:return fail(error,QStringLiteral("지원하지 않는 저장소 전환 종류입니다."));
    }
}
#ifdef Q_OS_UNIX
using profile_detail::Fd;
bool sameRevision(const struct stat& a,const struct stat& b){
    if(!profile_detail::sameInode(a,b)||a.st_mode!=b.st_mode||a.st_uid!=b.st_uid||a.st_gid!=b.st_gid
        ||a.st_nlink!=b.st_nlink||a.st_size!=b.st_size)return false;
#ifdef Q_OS_MACOS
    return a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec&&a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec
        &&a.st_ctimespec.tv_sec==b.st_ctimespec.tv_sec&&a.st_ctimespec.tv_nsec==b.st_ctimespec.tv_nsec;
#else
    return a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec
        &&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
#endif
}
bool safeFile(const struct stat& st){
    return profile_detail::safeNode(st,false)&&!(st.st_mode&0077);
}
struct Store {
    QString oldName,newName,sourceDigest,cipherDigest,temporary;
    Kind kind=Kind::Assets;
    bool exitNotice=false;
    struct stat original{},temporaryIdentity{};
    qsizetype cipherSize=0;
    QMap<QString,QString> dependencies;
    QJsonArray machineFields;
};
struct Stage {
    QString path;
    Fd directory;
    struct stat identity{};
    QList<Store> stores;
    ~Stage(){
        // Only temporary files created by this invocation are removed. A partial
        // set of published new names stays in the inactive stage for its owner to
        // discard/re-copy; the original profile is never a mutation argument.
        for(const auto& store:stores){
            if(store.temporary.isEmpty())continue;
            struct stat now{};const auto name=QFile::encodeName(store.temporary);
            if(::fstatat(directory.get(),name.constData(),&now,AT_SYMLINK_NOFOLLOW)==0
                &&profile_detail::sameInode(now,store.temporaryIdentity))
                ::unlinkat(directory.get(),name.constData(),0);
        }
    }
    bool unchanged(QString* error)const{
        auto current=profile_detail::openDirectory(path,nullptr,error);struct stat st{};
        return current&&::fstat(current.get(),&st)==0&&profile_detail::sameInode(st,identity)
            &&profile_detail::safeNode(st,true)&&(st.st_mode&0777)==0700
            &&storage_detail::privateStoragePath(path,true,error)
            ?true:fail(error,QStringLiteral("전환 사본 폴더의 정체성 또는 권한이 바뀌었습니다."));
    }
    bool absent(const QString& name,QString* error)const{
        struct stat st{};
        if(::fstatat(directory.get(),QFile::encodeName(name).constData(),&st,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)
            return fail(error,QStringLiteral("새 저장 파일 이름이 이미 존재하거나 안전하게 확인되지 않습니다."));
        return true;
    }
    bool read(const QString& name,qsizetype limit,QByteArray* bytes,struct stat* identityOut,QString* error)const{
        const auto encoded=QFile::encodeName(name);
        struct stat listed{},before{},after{},linked{};
        if(name.isEmpty()||name.contains('/')||name=="."||name==".."||name.contains(QChar(0))
            ||::fstatat(directory.get(),encoded.constData(),&listed,AT_SYMLINK_NOFOLLOW)
            ||!safeFile(listed)||listed.st_size<56||listed.st_size>limit)
            return fail(error,QStringLiteral("전환할 암호화 파일의 종류·권한·링크·크기를 거부했습니다."));
        Fd file(::openat(directory.get(),encoded.constData(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
        if(!file||::fstat(file.get(),&before)||!safeFile(before)||!sameRevision(listed,before)
            ||!storage_detail::privateStoragePath(QDir(path).filePath(name),false,error))
            return fail(error,QStringLiteral("전환할 암호문을 동일한 파일에서 안전하게 열지 못했습니다."));
        QFile input;
        if(!input.open(file.get(),QIODevice::ReadOnly,QFileDevice::DontCloseHandle))
            return fail(error,QStringLiteral("전환 암호문 읽기를 시작하지 못했습니다."));
        auto next=input.read(limit+1);
        if(input.error()!=QFileDevice::NoError||next.size()!=before.st_size||::fstat(file.get(),&after)
            ||!sameRevision(before,after)||::fstatat(directory.get(),encoded.constData(),&linked,AT_SYMLINK_NOFOLLOW)
            ||!sameRevision(after,linked))
            return fail(error,QStringLiteral("읽는 동안 전환 암호문의 정체성 또는 내용이 바뀌었습니다."));
        input.close();if(::close(file.release())!=0)return fail(error,QStringLiteral("전환 암호문을 닫지 못했습니다."));
        if(identityOut)*identityOut=after;*bytes=std::move(next);return true;
    }
    bool original(const Store& store,QByteArray* bytes,QString* error)const{
        struct stat now{};
        return read(store.oldName,maximum(store.kind),bytes,&now,error)
            &&(sameRevision(store.original,now)&&digest(*bytes)==store.sourceDigest
                ?true:fail(error,QStringLiteral("검증 이후 원본 사본 암호문이 바뀌어 전환을 중단했습니다.")));
    }
    bool writeTemporary(Store& store,const QByteArray& bytes,QString* error){
        if(!unchanged(error)||!absent(store.newName,error))return false;
        store.temporary=QStringLiteral(".keefi-storage-")+QUuid::createUuid().toString(QUuid::Id128);
        const auto name=QFile::encodeName(store.temporary);
        Fd file(::openat(directory.get(),name.constData(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
        if(!file)return fail(error,QStringLiteral("전환 전용 임시 파일을 만들지 못했습니다."));
        if(::fstat(file.get(),&store.temporaryIdentity)||!safeFile(store.temporaryIdentity)||(store.temporaryIdentity.st_mode&0777)!=0600)
            return fail(error,QStringLiteral("전환 임시 파일의 소유권을 확인하지 못했습니다."));
        qsizetype offset=0;
        while(offset<bytes.size()){
            const auto written=::write(file.get(),bytes.constData()+offset,std::size_t(bytes.size()-offset));
            if(written<0&&errno==EINTR)continue;
            if(written<=0)return fail(error,QStringLiteral("새 암호문을 임시 파일에 저장하지 못했습니다."));
            offset+=written;
        }
        if(::fsync(file.get())||::fstat(file.get(),&store.temporaryIdentity))
            return fail(error,QStringLiteral("새 암호문을 디스크에 반영하지 못했습니다."));
        if(::close(file.release())!=0)return fail(error,QStringLiteral("새 암호문 임시 파일을 닫지 못했습니다."));
        QByteArray checked;struct stat readIdentity{};
        if(!read(store.temporary,maximum(store.kind),&checked,&readIdentity,error)
            ||!sameRevision(store.temporaryIdentity,readIdentity)||checked!=bytes)
            return fail(error,QStringLiteral("새 암호문 임시 파일의 실제 readback이 일치하지 않습니다."));
        store.cipherDigest=digest(checked);store.cipherSize=checked.size();return true;
    }
};
bool names(Stage& stage,QStringList* output,QString* error){
    // A fresh directory description starts at offset zero on each preflight.
    const int duplicate=::openat(stage.directory.get(),".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(duplicate<0)return fail(error,QStringLiteral("전환 사본 목록을 열지 못했습니다."));
    DIR* directory=::fdopendir(duplicate);
    if(!directory){::close(duplicate);return fail(error,QStringLiteral("전환 사본 목록을 읽지 못했습니다."));}
    QStringList result;bool ok=true;errno=0;
    while(const auto* entry=::readdir(directory)){
        const QByteArray raw(entry->d_name);
        if(raw=="."||raw=="..")continue;
        const auto name=QFile::decodeName(raw);
        if(QFile::encodeName(name)!=raw||result.size()>=4096){ok=false;break;}
        result.append(name);
    }
    if(errno)ok=false;
    if(::closedir(directory)!=0)ok=false;
    if(!ok)return fail(error,QStringLiteral("전환 사본 파일명의 표현 또는 목록 상한을 확인하지 못했습니다."));
    result.sort();*output=std::move(result);return true;
}
bool noEncryptedStorage(Stage& stage,const QSet<QString>& allowed,int* entries,int depth,QString* error){
    if(depth>32)return fail(error,QStringLiteral("비활성 복원 사본의 폴더 깊이 상한을 넘었습니다."));
    QStringList listed;if(!names(stage,&listed,error))return false;
    for(const auto& name:listed){
        if(++*entries>4096)return fail(error,QStringLiteral("비활성 복원 사본의 항목 상한을 넘었습니다."));
        if(allowed.contains(name))continue;
        if(storage_detail::encryptedStorageName(name))
            return fail(error,QStringLiteral("기존 암호화 자료가 있는 사본에는 백업을 복원하지 않습니다."));
        const auto encoded=QFile::encodeName(name);struct stat node{};
        if(::fstatat(stage.directory.get(),encoded.constData(),&node,AT_SYMLINK_NOFOLLOW)
            ||!profile_detail::safeNode(node,S_ISDIR(node.st_mode)))
            return fail(error,QStringLiteral("비활성 복원 사본의 소유자 또는 링크를 거부했습니다."));
        if(S_ISDIR(node.st_mode)){
            Stage child;child.path=QDir(stage.path).filePath(name);
            child.directory=Fd(::openat(stage.directory.get(),encoded.constData(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
            if(!child.directory||::fstat(child.directory.get(),&child.identity)||!sameRevision(node,child.identity)
                ||!noEncryptedStorage(child,{},entries,depth+1,error))
                return fail(error,QStringLiteral("비활성 복원 사본의 하위 폴더를 안전하게 확인하지 못했습니다."));
            struct stat linked{};
            if(::fstatat(stage.directory.get(),encoded.constData(),&linked,AT_SYMLINK_NOFOLLOW)||!sameRevision(node,linked))
                return fail(error,QStringLiteral("검사 중 비활성 복원 사본의 하위 폴더가 바뀌었습니다."));
        }
    }
    return true;
}
struct BackupSource {
    QString path,name,parentPath;
    Fd parent,file;
    struct stat parentIdentity{},identity{};
    bool open(const QString& source,QString* error){
        path=profile_detail::normalized(source);
        if(path.isEmpty()||path.contains(QChar(0)))return fail(error,QStringLiteral("복원할 백업 경로가 올바르지 않습니다."));
        const QFileInfo info(path);name=info.fileName();parentPath=info.absolutePath();
        parent=profile_detail::openDirectory(parentPath,nullptr,error);
        if(!parent||::fstat(parent.get(),&parentIdentity))
            return fail(error,QStringLiteral("백업의 상위 경로를 안전하게 열지 못했습니다."));
        const auto encoded=QFile::encodeName(name);struct stat listed{};
        if(name.isEmpty()||::fstatat(parent.get(),encoded.constData(),&listed,AT_SYMLINK_NOFOLLOW)
            ||!safeFile(listed)||listed.st_size<Vault::HeaderBytes+16||listed.st_size>Vault::MaxFileBytes)
            return fail(error,QStringLiteral("백업의 소유자·권한·링크·크기가 안전하지 않습니다."));
        file=Fd(::openat(parent.get(),encoded.constData(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
        if(!file||::fstat(file.get(),&identity)||!safeFile(identity)||!sameRevision(listed,identity)
            ||!storage_detail::privateStoragePath(path,false,error))
            return fail(error,QStringLiteral("백업을 동일한 읽기 전용 파일로 열지 못했습니다."));
        return true;
    }
    bool read(QByteArray* bytes,QString* error)const{
        auto currentParent=profile_detail::openDirectory(parentPath,nullptr,error);struct stat parentNow{},before{},after{},linked{};
        if(!currentParent||::fstat(currentParent.get(),&parentNow)||!profile_detail::sameInode(parentIdentity,parentNow)
            ||::fstat(file.get(),&before)||!sameRevision(identity,before)||!safeFile(before)
            ||!storage_detail::privateStoragePath(path,false,error))
            return fail(error,QStringLiteral("인증할 백업의 경로 또는 파일 정체성이 바뀌었습니다."));
        QByteArray next(qsizetype(identity.st_size),'\0');qsizetype offset=0;
        while(offset<next.size()){
            const auto got=::pread(file.get(),next.data()+offset,std::size_t(next.size()-offset),off_t(offset));
            if(got<0&&errno==EINTR)continue;
            if(got<=0)return fail(error,QStringLiteral("백업 암호문을 끝까지 읽지 못했습니다."));
            offset+=got;
        }
        if(::fstat(file.get(),&after)||!sameRevision(before,after)
            ||::fstatat(parent.get(),QFile::encodeName(name).constData(),&linked,AT_SYMLINK_NOFOLLOW)
            ||!sameRevision(after,linked))
            return fail(error,QStringLiteral("읽는 동안 백업 암호문의 내용 또는 정체성이 바뀌었습니다."));
        *bytes=std::move(next);return true;
    }
    bool unchanged(const QByteArray& authenticated,QString* error)const{
        QByteArray checked;
        return read(&checked,error)&&(checked==authenticated
            ||fail(error,QStringLiteral("인증 이후 백업의 실제 암호문이 바뀌었습니다.")));
    }
};
bool assetDependencies(Store& store,const SecureBuffer& plain,QString* error){
    const auto root=json(plain);
    if(root.value("schema_version")==QJsonValue(3)){
        const auto migration=root.value("migration").toObject();
        store.dependencies.insert(migration.value("backup").toString(),migration.value("sourceDigest").toString());
        for(const auto& value:migration.value("archives").toArray()){
            const auto item=value.toObject();store.dependencies.insert(item.value("name").toString(),item.value("digest").toString());
        }
    }
    const auto policy=root.value("coinBaseline").toObject().value("policy").toString();
    const auto current=canonicalPublicFormat(policy);
    if(current==QStringLiteral("keefi-valuation-v2")&&current!=policy)
        store.machineFields.append(QJsonObject{{"field","coinBaseline.policy"},{"from",policy},{"to",current}});
    return store.dependencies.size()<=257||fail(error,QStringLiteral("원장 전환 증거의 참조 상한을 넘었습니다."));
}
bool currentReadback(Stage& stage,const Store& store,const storage_detail::VaultMaterial& material,
                     const QString& password,QString* error){
    QByteArray bytes;struct stat identity{};
    if(!stage.read(store.newName,maximum(store.kind),&bytes,&identity,error)||(identity.st_mode&0777)!=0600
        ||digest(bytes)!=store.cipherDigest||bytes.size()!=store.cipherSize
        ||storage_detail::formatOf(bytes,store.kind)!=Format::Current)
        return fail(error,QStringLiteral("게시된 새 암호문 해시 또는 형식을 확인하지 못했습니다."));
    if(store.kind==Kind::Vault){
        storage_detail::VaultMaterial readback;
        if(!storage_detail::authenticateVault(bytes,password,&readback,error))return false;
        return (readback.format==Format::Current&&readback.identityScheme==material.identityScheme
            &&readback.ops==material.ops&&readback.mem==material.mem&&readback.salt==material.salt
            &&equal(readback.key,material.key)&&equal(readback.payload,material.payload)&&equal(readback.ledgerRoot,material.ledgerRoot))
            ||fail(error,QStringLiteral("새 금고의 암호·원문·원장 키가 원본과 다릅니다."));
    }
    SecureBuffer plain;
    return storage_detail::decodeStore(bytes,store.kind,material.ledgerRoot,&plain,error)
        &&validate(plain,store.kind,false,store.exitNotice,error);
}
#endif
} // namespace

bool StorageMigration::convertStagedCopy(const QString& stageDirectory,const QString& password,
                                         StorageConversionReceipt* receipt,QString* error){
    if(receipt)*receipt={};if(error)error->clear();
    if(!receipt)return fail(error,QStringLiteral("검증된 저장소 전환 영수증을 받을 경로가 없습니다."));
#ifndef Q_OS_UNIX
    Q_UNUSED(stageDirectory)Q_UNUSED(password)
    return fail(error,QStringLiteral("이 운영체제의 저장소 전환은 지원하지 않습니다."));
#else
    try{
        Stage stage;stage.path=profile_detail::normalized(stageDirectory);
        stage.directory=profile_detail::openDirectory(stage.path,nullptr,error);
        if(!stage.directory||::fstat(stage.directory.get(),&stage.identity)||!profile_detail::safeNode(stage.identity,true)
            ||(stage.identity.st_mode&0777)!=0700||!storage_detail::privateStoragePath(stage.path,true,error))
            return fail(error,QStringLiteral("소유한 비활성 전환 사본 폴더가 아닙니다."));
        QStringList listed;if(!names(stage,&listed,error))return false;
        QMap<QString,int> index;
        for(const auto& name:listed){
            const auto replacement=storage_detail::rewrittenFileName(name);
            if(replacement.isEmpty()){
                if(storage_detail::legacyStorageName(name))
                    return fail(error,QStringLiteral("자동 전환을 지원하지 않는 옛 저장 파일이 있어 원본을 보존했습니다."));
                continue;
            }
            if(stage.stores.size()>=1024||!stage.absent(replacement,error))
                return fail(error,QStringLiteral("전환 파일 상한 또는 새 저장 파일 이름 충돌입니다."));
            Store store;store.oldName=name;store.newName=replacement;
            if(name==storage_detail::legacyFileName(Kind::Vault))store.kind=Kind::Vault;
            else if(name==storage_detail::legacyFileName(Kind::Swap))store.kind=Kind::Swap;
            else if(name==storage_detail::legacyFileName(Kind::Imported))store.kind=Kind::Imported;
            else if(name==storage_detail::legacyFileName(Kind::Price)||name==storage_detail::legacyFileName(Kind::Price)+".exit"){
                store.kind=Kind::Price;store.exitNotice=name.endsWith(".exit");
            }
            QByteArray bytes;
            if(!stage.read(name,maximum(store.kind),&bytes,&store.original,error))return false;
            if(storage_detail::formatOf(bytes,store.kind)!=Format::Legacy)
                return fail(error,QStringLiteral("옛 파일 이름과 인증된 저장 형식이 일치하지 않습니다."));
            store.sourceDigest=digest(bytes);index.insert(name,stage.stores.size());stage.stores.append(std::move(store));
        }
        const auto vaultName=storage_detail::legacyFileName(Kind::Vault);
        if(!index.contains(vaultName)){
            if(!stage.stores.isEmpty())return fail(error,QStringLiteral("암호화 자료가 있는데 해당 금고 사본이 없습니다."));
            for(const auto kind:{Kind::Vault,Kind::Assets,Kind::Swap,Kind::Price,Kind::Imported})
                if(listed.contains(storage_detail::currentFileName(kind)))
                    return fail(error,QStringLiteral("이미 현재 형식인 저장소를 옛 프로필로 전환할 수 없습니다."));
            QJsonArray settings;
            if(!stage.unchanged(error)||!convertStagedPublicSettings(stage.path,&settings,error)||!stage.unchanged(error))return false;
            receipt->identityScheme=QStringLiteral("legacy-v1");
            receipt->evidence={{"format","keefi-storage-rewrite/1"},{"convertedFiles",QJsonArray{}},{"publicSettings",settings}};
            receipt->verified=true;return true;
        }
        storage_detail::VaultMaterial material;QByteArray vaultBytes;
        if(!stage.original(stage.stores[index.value(vaultName)],&vaultBytes,error)
            ||!storage_detail::authenticateVault(vaultBytes,password,&material,error))return false;
        if(stage.stores.size()>1&&material.ledgerRoot.size()!=32)
            return fail(error,QStringLiteral("암호화 자료에 필요한 원본 원장 키가 없습니다. 새 키를 만들지 않았습니다."));

        // First pass is read-only: every ciphertext, strict schema, cross-file
        // digest and target name is checked before creating any output.
        for(auto& store:stage.stores){
            if(store.kind==Kind::Vault)continue;
            QByteArray bytes;SecureBuffer plain;
            if(!stage.original(store,&bytes,error)||!storage_detail::decodeStore(bytes,store.kind,material.ledgerRoot,&plain,error)
                ||!validate(plain,store.kind,true,store.exitNotice,error))return false;
            if(store.kind==Kind::Assets&&!assetDependencies(store,plain,error))return false;
        }
        for(const auto& store:stage.stores){
            for(auto dependency=store.dependencies.cbegin();dependency!=store.dependencies.cend();++dependency){
                if(!index.contains(dependency.key()))return fail(error,QStringLiteral("schema 3이 증명하는 원본 백업 또는 보관 파일이 없습니다."));
                const auto& original=stage.stores[index.value(dependency.key())];
                if(original.kind!=Kind::Assets||original.sourceDigest!=dependency.value())
                    return fail(error,QStringLiteral("schema 3의 원본 SHA 증거와 실제 사본 암호문이 다릅니다."));
            }
        }
        QList<int> order;QSet<QString> ready;
        while(order.size()<stage.stores.size()-1){
            const auto before=order.size();
            for(int i=0;i<stage.stores.size();++i){
                const auto& store=stage.stores[i];if(store.kind==Kind::Vault||ready.contains(store.oldName))continue;
                bool admitted=true;for(auto it=store.dependencies.cbegin();it!=store.dependencies.cend();++it)admitted=admitted&&ready.contains(it.key());
                if(admitted){order.append(i);ready.insert(store.oldName);}
            }
            if(order.size()==before)return fail(error,QStringLiteral("원장 전환 증거에 순환 참조가 있어 전환하지 않았습니다."));
        }
        order.append(index.value(vaultName));
        QJsonObject rewriteFiles;
        for(const int position:order){
            auto& store=stage.stores[position];QByteArray source,cipher;SecureBuffer expected;
            if(!stage.unchanged(error)||!stage.original(store,&source,error)||!stage.absent(store.newName,error))return false;
            if(store.kind==Kind::Vault)cipher=storage_detail::encodeCurrentVault(material,error);
            else{
                SecureBuffer plain;
                if(!storage_detail::decodeStore(source,store.kind,material.ledgerRoot,&plain,error))return false;
                if(store.kind==Kind::Assets){
                    if(!storage_detail::rewriteAssetsPayload(plain,rewriteFiles,&expected,error))return false;
                }else expected=std::move(plain);
                cipher=storage_detail::encodeStore(expected,store.kind,material.ledgerRoot,error);
                SecureBuffer checked;
                if(cipher.isEmpty()||!storage_detail::decodeStore(cipher,store.kind,material.ledgerRoot,&checked,error)
                    ||!equal(checked,expected)||!validate(checked,store.kind,false,store.exitNotice,error))
                    return fail(error,QStringLiteral("새 암호문의 전체 원문 readback이 전환 결과와 다릅니다."));
            }
            if(cipher.isEmpty()||!stage.writeTemporary(store,cipher,error))return false;
            rewriteFiles.insert(store.oldName,QJsonObject{{"name",store.newName},{"sourceDigest",store.sourceDigest},{"cipherDigest",store.cipherDigest}});
        }
        // All new bytes are already authenticated. Publish each name with
        // linkat(create-only), never rename-overwrite an unexpected destination.
        for(auto& store:stage.stores){
            if(!stage.unchanged(error)||!stage.absent(store.newName,error))return false;
            const auto temporary=QFile::encodeName(store.temporary),target=QFile::encodeName(store.newName);
            struct stat now{};
            if(::fstatat(stage.directory.get(),temporary.constData(),&now,AT_SYMLINK_NOFOLLOW)
                ||!sameRevision(now,store.temporaryIdentity)
                ||::linkat(stage.directory.get(),temporary.constData(),stage.directory.get(),target.constData(),0))
                return fail(error,QStringLiteral("새 저장 파일의 create-only 게시가 실패했습니다."));
            if(::unlinkat(stage.directory.get(),temporary.constData(),0))
                return fail(error,QStringLiteral("게시한 암호문의 임시 이름을 정리하지 못했습니다."));
            store.temporary.clear();
        }
        if(::fsync(stage.directory.get()))return fail(error,QStringLiteral("새 저장 파일 이름을 디스크에 반영하지 못했습니다."));
        for(const auto& store:stage.stores)
            if(!currentReadback(stage,store,material,password,error))return false;
        for(const auto& store:stage.stores){QByteArray source;if(!stage.original(store,&source,error))return false;}
        if(!stage.unchanged(error))return false;
        for(const auto& store:stage.stores){
            const auto name=QFile::encodeName(store.oldName);struct stat now{};
            if(::fstatat(stage.directory.get(),name.constData(),&now,AT_SYMLINK_NOFOLLOW)||!sameRevision(store.original,now)
                ||::unlinkat(stage.directory.get(),name.constData(),0))
                return fail(error,QStringLiteral("검증한 비활성 사본의 옛 파일 이름을 정리하지 못했습니다."));
        }
        if(::fsync(stage.directory.get()))return fail(error,QStringLiteral("사본의 저장 이름 전환을 디스크에 반영하지 못했습니다."));
        QJsonArray files,machineFields,settings;
        for(const auto& store:stage.stores){
            if(!stage.absent(store.oldName,error))return false;
            files.append(QJsonObject{{"source",store.oldName},{"name",store.newName},{"sourceDigest",store.sourceDigest},
                {"cipherDigest",store.cipherDigest},{"sourceBytes",qint64(store.original.st_size)},{"cipherBytes",qint64(store.cipherSize)}});
            for(const auto& value:store.machineFields){auto row=value.toObject();row["name"]=store.newName;machineFields.append(row);}
        }
        if(!stage.unchanged(error)||!convertStagedPublicSettings(stage.path,&settings,error)||!stage.unchanged(error))return false;
        receipt->identityScheme=material.identityScheme;
        receipt->evidence={{"format","keefi-storage-rewrite/1"},{"convertedFiles",files},{"machineFields",machineFields},{"publicSettings",settings}};
        receipt->verified=true;return true;
    }catch(const std::exception&){
        return fail(error,QStringLiteral("보호된 저장소 전환에 실패했습니다. 원본 프로필은 변경하지 않았습니다."));
    }
#endif
}
bool StorageMigration::restoreBackupStagedCopy(const QString& stageDirectory,const QString& sourceBackup,
                                              const QString& password,StorageConversionReceipt* receipt,QString* error){
    if(receipt)*receipt={};if(error)error->clear();
    if(!receipt)return fail(error,QStringLiteral("검증된 백업 복원 영수증을 받을 경로가 없습니다."));
#ifndef Q_OS_UNIX
    Q_UNUSED(stageDirectory)Q_UNUSED(sourceBackup)Q_UNUSED(password)
    return fail(error,QStringLiteral("이 운영체제의 비활성 백업 복원은 지원하지 않습니다."));
#else
    try{
        Stage stage;stage.path=profile_detail::normalized(stageDirectory);
        if(stage.path.contains(QChar(0)))return fail(error,QStringLiteral("비활성 복원 사본 경로가 올바르지 않습니다."));
        stage.directory=profile_detail::openDirectory(stage.path,nullptr,error);
        if(!stage.directory||::fstat(stage.directory.get(),&stage.identity)||!profile_detail::safeNode(stage.identity,true)
            ||(stage.identity.st_mode&0777)!=0700||!storage_detail::privateStoragePath(stage.path,true,error))
            return fail(error,QStringLiteral("소유한 비활성 복원 사본 폴더가 아닙니다."));
        int entries=0;if(!noEncryptedStorage(stage,{},&entries,0,error))return false;
        BackupSource source;QByteArray authenticated;
        storage_detail::VaultMaterial material;
        if(!source.open(sourceBackup,error)||!source.read(&authenticated,error)
            ||!storage_detail::authenticateVault(authenticated,password,&material,error))return false;
        // Current backups keep their exact authenticated ciphertext. The explicit
        // legacy reader rewraps only the header/nonce while retaining all keys,
        // KDF parameters, payload bytes and logical identity.
        const auto cipher=material.format==Format::Current?authenticated:storage_detail::encodeCurrentVault(material,error);
        if(cipher.isEmpty()||!source.unchanged(authenticated,error)||!stage.unchanged(error))return false;
        Store store;store.kind=Kind::Vault;store.newName=storage_detail::currentFileName(Kind::Vault);
        store.sourceDigest=digest(authenticated);store.original=source.identity;stage.stores.append(std::move(store));
        auto& output=stage.stores.last();
        entries=0;
        if(!noEncryptedStorage(stage,{},&entries,0,error)||!stage.writeTemporary(output,cipher,error))return false;
        entries=0;
        if(!source.unchanged(authenticated,error)||!stage.unchanged(error)
            ||!noEncryptedStorage(stage,{output.temporary},&entries,0,error)||!stage.absent(output.newName,error))return false;
        const auto temporary=QFile::encodeName(output.temporary),target=QFile::encodeName(output.newName);struct stat now{};
        if(::fstatat(stage.directory.get(),temporary.constData(),&now,AT_SYMLINK_NOFOLLOW)
            ||!sameRevision(now,output.temporaryIdentity)
            ||::linkat(stage.directory.get(),temporary.constData(),stage.directory.get(),target.constData(),0))
            return fail(error,QStringLiteral("새 금고의 create-only 복원 게시가 실패했습니다."));
        if(::unlinkat(stage.directory.get(),temporary.constData(),0))
            return fail(error,QStringLiteral("게시한 복원 금고의 임시 이름을 정리하지 못했습니다."));
        output.temporary.clear();
        if(::fsync(stage.directory.get())||!currentReadback(stage,output,material,password,error)
            ||!source.unchanged(authenticated,error)||!stage.unchanged(error))
            return fail(error,QStringLiteral("복원한 금고의 실제 암호문·원문·정체성을 검증하지 못했습니다."));
        entries=0;if(!noEncryptedStorage(stage,{output.newName},&entries,0,error))return false;
        receipt->identityScheme=material.identityScheme;
        receipt->evidence={{"restoredBackup",QJsonObject{{"name",output.newName},{"sourceDigest",output.sourceDigest},
            {"sourceBytes",qint64(authenticated.size())},{"cipherDigest",output.cipherDigest},{"cipherBytes",qint64(output.cipherSize)}}}};
        receipt->verified=true;return true;
    }catch(const std::exception&){
        return fail(error,QStringLiteral("비활성 백업 복원에 실패했습니다. 원본 백업은 변경하지 않았습니다."));
    }
#endif
}
} // namespace keefi
