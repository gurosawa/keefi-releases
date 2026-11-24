#include "keefi/LegacyStorageCodecPrivate.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <sodium.h>
#include <cstring>

namespace keefi::storage_detail {
namespace {
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}
qsizetype maximum(Kind kind){
    switch(kind){case Kind::Assets:return 16*1024*1024;case Kind::Swap:return 2*1024*1024;
    case Kind::Price:return 8*1024*1024;case Kind::Imported:return 64*1024*1024;default:return 0;}
}
bool hash(const QString& text){static const QRegularExpression rx("\\A[a-f0-9]{64}\\z");return rx.match(text).hasMatch();}
}
QByteArray magic(Kind kind,Format format){
    if(format==Format::Legacy){
        switch(kind){case Kind::Vault:return "FINIXVLT";case Kind::Assets:return "FINIXAST";case Kind::Swap:return "FINIXSWP";
        case Kind::Price:return "FINIXPRC";case Kind::Imported:return "FINIXIMP";case Kind::Package:return "FINIXRTX";}
    }else if(format==Format::Current){
        switch(kind){case Kind::Vault:return "KEEFIVLT";case Kind::Assets:return "KEEFIAST";case Kind::Swap:return "KEEFISWP";
        case Kind::Price:return "KEEFIPRC";case Kind::Imported:return "KEEFIIMP";case Kind::Package:return "KEEFIRTX";}
    }return {};
}
Format formatOf(const QByteArray& bytes,Kind kind){
    if(bytes.size()<8)return Format::Invalid;
    const auto prefix=bytes.first(8);
    if(prefix==magic(kind,Format::Legacy))return Format::Legacy;
    if(prefix==magic(kind,Format::Current))return Format::Current;
    return Format::Invalid;
}
QByteArray keyDomain(Kind kind,Format format){
    if(format!=Format::Legacy&&format!=Format::Current)return {};
    const QByteArray prefix=format==Format::Legacy?QByteArray("FINIX/"):QByteArray("KEEFI/");
    switch(kind){case Kind::Assets:return prefix+"assets-ledger/v1";case Kind::Swap:return prefix+"swap-journal/v1";
    case Kind::Price:return prefix+"price-archive/v1";case Kind::Imported:return prefix+"imported-records/v1";default:return {};}
}
bool deriveStoreKey(const SecureBuffer& root,Kind kind,Format format,SecureBuffer* key,QString* error){
    const auto domain=keyDomain(kind,format);
    if(!key||root.size()!=32||domain.isEmpty())return fail(error,QStringLiteral("저장소의 원본 키 또는 형식이 올바르지 않습니다."));
    SecureBuffer next(32);
    if(crypto_generichash(next.data(),next.size(),reinterpret_cast<const unsigned char*>(domain.constData()),domain.size(),root.data(),root.size()))
        return fail(error,QStringLiteral("저장소 키를 유도하지 못했습니다."));
    *key=std::move(next);return true;
}
bool decodeStore(const QByteArray& bytes,Kind kind,const SecureBuffer& root,SecureBuffer* plain,QString* error){
    const auto format=formatOf(bytes,kind);const auto limit=maximum(kind);
    if(!plain||format==Format::Invalid||!limit||bytes.size()<56||bytes.size()>limit)return fail(error,QStringLiteral("저장소 형식 또는 크기가 올바르지 않습니다."));
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(qFromLittleEndian<quint16>(p+8)!=1||qFromLittleEndian<quint16>(p+10)!=0||qFromLittleEndian<quint32>(p+36)!=bytes.size()-40)
        return fail(error,QStringLiteral("저장소 헤더의 버전 또는 길이가 올바르지 않습니다."));
    SecureBuffer key;if(!deriveStoreKey(root,kind,format,&key,error))return false;
    SecureBuffer next(std::size_t(bytes.size()-56));unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(next.data(),&size,nullptr,p+40,bytes.size()-40,p,40,p+12,key.data())||size!=next.size())
        return fail(error,QStringLiteral("저장소 암호문 인증에 실패했습니다."));
    *plain=std::move(next);return true;
}
QByteArray encodeStore(const SecureBuffer& plain,Kind kind,const SecureBuffer& root,QString* error){
    const auto limit=maximum(kind);
    if(!limit||plain.empty()||plain.size()>std::size_t(limit-56)){fail(error,QStringLiteral("전환할 저장소 크기가 올바르지 않습니다."));return {};}
    SecureBuffer key;if(!deriveStoreKey(root,kind,Format::Current,&key,error))return {};
    QByteArray bytes(qsizetype(plain.size())+56,'\0');auto* p=reinterpret_cast<unsigned char*>(bytes.data());
    const auto name=magic(kind,Format::Current);std::memcpy(p,name.constData(),8);qToLittleEndian<quint16>(1,p+8);
    randombytes_buf(p+12,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+36);unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+40,&size,plain.data(),plain.size(),p,40,nullptr,p+12,key.data())||size!=plain.size()+16){
        fail(error,QStringLiteral("새 저장소 암호화에 실패했습니다."));return {};}
    SecureBuffer checked;
    if(!decodeStore(bytes,kind,root,&checked,error)||checked.size()!=plain.size()||sodium_memcmp(checked.data(),plain.data(),plain.size())){
        fail(error,QStringLiteral("새 저장소 암호화 검증에 실패했습니다."));return {};}
    return bytes;
}
QString legacyFileName(Kind kind){
    switch(kind){case Kind::Vault:return QStringLiteral("credentials.finix-vault");case Kind::Assets:return QStringLiteral("assets.finix");
    case Kind::Swap:return QStringLiteral("swaps.finix");case Kind::Price:return QStringLiteral("prices.finix");
    case Kind::Imported:return QStringLiteral("imported-records.finix");case Kind::Package:return QStringLiteral("portable.finixrtx");}
    return {};
}
QString currentFileName(Kind kind){
    switch(kind){case Kind::Vault:return QStringLiteral("credentials.keefi-vault");case Kind::Assets:return QStringLiteral("assets.keefi");
    case Kind::Swap:return QStringLiteral("swaps.keefi");case Kind::Price:return QStringLiteral("prices.keefi");
    case Kind::Imported:return QStringLiteral("imported-records.keefi");case Kind::Package:return QStringLiteral("portable.keefirtx");}
    return {};
}
QString rewrittenFileName(const QString& name){
    for(const auto kind:{Kind::Vault,Kind::Assets,Kind::Swap,Kind::Price,Kind::Imported})
        if(name==legacyFileName(kind))return currentFileName(kind);
    if(name==QStringLiteral("assets-current.finix"))return QStringLiteral("assets-current.keefi");
    if(name==QStringLiteral("prices.finix.exit"))return QStringLiteral("prices.keefi.exit");
    static const QRegularExpression archive("\\Aassets\\.finix\\.(archive-[a-f0-9]{32}|pre-schema3-[a-f0-9]{64})\\.finix\\z");
    const auto found=archive.match(name);
    if(found.hasMatch())return QStringLiteral("assets.keefi.")+found.captured(1)+QStringLiteral(".keefi");
    return {};
}
QStringList archivePatterns(const QString& base){
    return {base+QStringLiteral(".archive-*.keefi"),base+QStringLiteral(".archive-*.finix")};
}
bool legacyStorageName(const QString& name){
    return name.endsWith(QStringLiteral(".finix"))||name.endsWith(QStringLiteral(".finix-vault"))||name.endsWith(QStringLiteral(".finixrtx"))
        ||name.startsWith(QStringLiteral("assets.finix."))||name.startsWith(QStringLiteral("prices.finix."))
        ||name.startsWith(QStringLiteral(".finix-"));
}
bool encryptedStorageName(const QString& name){
    for(const auto format:{Format::Legacy,Format::Current}){
        const auto extension=format==Format::Legacy?QStringLiteral(".finix"):QStringLiteral(".keefi");
        for(const auto& suffix:{extension,extension+QStringLiteral("-vault"),extension+QStringLiteral("rtx")})
            if(name.endsWith(suffix)||name.endsWith(suffix+QStringLiteral(".lock")))return true;
        for(const auto kind:{Kind::Vault,Kind::Assets,Kind::Swap,Kind::Price,Kind::Imported}){
            const auto base=format==Format::Legacy?legacyFileName(kind):currentFileName(kind);
            if(name==base||name.startsWith(base+QLatin1Char('.')))return true;
        }
        if(name.startsWith(latestAssetsFileName(format)+QLatin1Char('.')))return true;
        const auto prefix=format==Format::Legacy?QStringLiteral(".finix-"):QStringLiteral(".keefi-");
        for(const auto* kind:{"vault-","restore-","assets-","prices-","record-transfer-"})
            if(name.startsWith(prefix+QLatin1String(kind)))return true;
    }
    if(name.startsWith(QStringLiteral(".keefi-storage-")))return true;
    return false;
}
QString readableFileName(const QString& directory,Kind kind){
    const QDir folder(directory);const auto current=currentFileName(kind),legacy=legacyFileName(kind);
    const QFileInfo next(folder.filePath(current)),old(folder.filePath(legacy));
    return !next.exists()&&!next.isSymLink()&&(old.exists()||old.isSymLink())?legacy:current;
}
QString latestAssetsFileName(Format format){
    return format==Format::Legacy?QStringLiteral("assets-current.finix"):QStringLiteral("assets-current.keefi");
}
QString readableLatestAssetsFileName(const QString& directory){
    const QDir folder(directory);const auto current=latestAssetsFileName(Format::Current),legacy=latestAssetsFileName(Format::Legacy);
    const QFileInfo next(folder.filePath(current)),old(folder.filePath(legacy));
    return !next.exists()&&!next.isSymLink()&&(old.exists()||old.isSymLink())?legacy:current;
}
QStringList relatedFileNames(const QString& directory){
    const QDir folder(directory);QStringList names;
    for(const auto kind:{Kind::Assets,Kind::Swap,Kind::Price,Kind::Imported})
        for(const auto& name:{legacyFileName(kind),currentFileName(kind)}){const QFileInfo info(folder.filePath(name));if(info.exists()||info.isSymLink())names.append(name);}
    for(const auto& name:{QStringLiteral("assets-current.finix"),QStringLiteral("assets-current.keefi"),QStringLiteral("prices.finix.exit"),QStringLiteral("prices.keefi.exit")}){
        const QFileInfo info(folder.filePath(name));if(info.exists()||info.isSymLink())names.append(name);}
    names.append(folder.entryList({QStringLiteral("assets.finix.archive-*.finix"),QStringLiteral("assets.keefi.archive-*.keefi"),
        QStringLiteral("assets.finix.pre-schema3-*.finix"),QStringLiteral("assets.keefi.pre-schema3-*.keefi"),
        QStringLiteral("prices.finix.unreadable-*"),QStringLiteral("prices.keefi.unreadable-*"),
        QStringLiteral(".finix-prices-*"),QStringLiteral(".keefi-prices-*")},QDir::Files|QDir::System|QDir::Hidden,QDir::Name));
    names.removeDuplicates();return names;
}
bool legacyAssetMigrationValid(const QJsonObject& object){
    static const QRegularExpression backup("\\Aassets\\.finix\\.pre-schema3-[a-f0-9]{64}\\.finix\\z");
    static const QRegularExpression archive("\\Aassets\\.finix\\.archive-[a-f0-9]{32}\\.finix\\z");
    if(object.size()!=3||!hash(object.value("sourceDigest").toString())||!backup.match(object.value("backup").toString()).hasMatch()
        ||!object.value("archives").isArray()||object.value("archives").toArray().size()>256)return false;
    QSet<QString> seen;
    for(const auto& value:object.value("archives").toArray()){
        if(!value.isObject())return false;const auto row=value.toObject();const auto name=row.value("name").toString();
        if(row.size()!=2||!archive.match(name).hasMatch()||seen.contains(name)||!hash(row.value("digest").toString()))return false;seen.insert(name);
    }return true;
}
bool rejectLegacyWrite(bool legacy,QString* error){
    return !legacy||fail(error,QStringLiteral("옛 저장 형식의 원본에는 쓰지 않습니다. 검증된 새 프로필 사본으로 전환해 주세요."));
}
} // namespace finix::storage_detail

