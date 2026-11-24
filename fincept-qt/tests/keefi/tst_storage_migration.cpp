#include "keefi/StorageMigration.h"
#include "keefi/Vault.h"
#include "keefi/AssetsLedger.h"
#include "keefi/SwapJournal.h"
#include "keefi/PriceArchive.h"
#include "keefi/RecordTransfer.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <sodium.h>
#include <cstring>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace keefi {
namespace {
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path,const QByteArray& bytes) {
    QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate)
        &&file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)&&file.write(bytes)==bytes.size()&&file.flush();
}
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()); }
QMap<QString,QByteArray> files(const QString& directory) {
    QMap<QString,QByteArray> result;
    for(const auto& name:QDir(directory).entryList(QDir::Files|QDir::Hidden|QDir::System,QDir::Name))
        result.insert(name,read(QDir(directory).filePath(name)));
    return result;
}
bool copyProfile(const QString& source,const QString& destination) {
    for(const auto& name:QDir(source).entryList(QDir::Files|QDir::Hidden,QDir::Name)) {
        const auto target=QDir(destination).filePath(name);
        if(!QFile::copy(QDir(source).filePath(name),target)
            ||!QFile::setPermissions(target,QFileDevice::ReadOwner|QFileDevice::WriteOwner))return false;
    }return true;
}
// Independent inspection of synthetic fixture bytes. No product codec is used.
QByteArray vaultPlain(const QByteArray& bytes,const QString& password) {
    if(bytes.size()<88)return {};
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    SecureBuffer key(32),pass=SecureBuffer::fromUtf8(password);
    if(crypto_pwhash(key.data(),key.size(),reinterpret_cast<const char*>(pass.data()),pass.size(),p+28,
        qFromLittleEndian<quint32>(p+16),std::size_t(qFromLittleEndian<quint64>(p+20)),crypto_pwhash_ALG_ARGON2ID13))return {};
    QByteArray plain(bytes.size()-88,'\0');unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()),&size,nullptr,
        p+72,bytes.size()-72,p,72,p+44,key.data())||size!=quint64(plain.size()))return {};
    return plain;
}
QByteArray storePlain(const QByteArray& bytes,Vault& vault,const QByteArray& domain) {
    if(bytes.size()<56)return {};
    SecureBuffer key(32);
    if(!vault.withAssetsLedgerKey(false,[&](const SecureBuffer& base){
        if(crypto_generichash(key.data(),key.size(),reinterpret_cast<const unsigned char*>(domain.constData()),domain.size(),base.data(),base.size()))key.clear();
    })||key.size()!=32)return {};
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    QByteArray plain(bytes.size()-56,'\0');unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()),&size,nullptr,
        p+40,bytes.size()-40,p,40,p+12,key.data())||size!=quint64(plain.size()))return {};
    return plain;
}
QByteArray encodeSyntheticStore(const QByteArray& plain,Vault& vault,const QByteArray& magic,const QByteArray& domain){
    SecureBuffer key(32);
    if(magic.size()!=8||!vault.withAssetsLedgerKey(false,[&](const SecureBuffer& root){
        if(crypto_generichash(key.data(),key.size(),reinterpret_cast<const unsigned char*>(domain.constData()),domain.size(),root.data(),root.size()))key.clear();
    })||key.size()!=32)return {};
    QByteArray bytes(plain.size()+56,'\0');auto* p=reinterpret_cast<unsigned char*>(bytes.data());
    std::memcpy(p,magic.constData(),8);qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(plain.size()+16,p+36);
    unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+40,&size,reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),p,40,nullptr,p+12,key.data())
        ||size!=quint64(plain.size()+16))return {};
    return bytes;
}
QByteArray encodeSyntheticLegacyVault(const QByteArray& original,const QByteArray& plain,const QString& password){
    if(original.size()<88||plain.isEmpty())return {};
    auto bytes=original;auto* p=reinterpret_cast<unsigned char*>(bytes.data());
    SecureBuffer key(32),pass=SecureBuffer::fromUtf8(password);randombytes_buf(p+44,24);
    if(crypto_pwhash(key.data(),key.size(),reinterpret_cast<const char*>(pass.data()),pass.size(),p+28,
        qFromLittleEndian<quint32>(p+16),std::size_t(qFromLittleEndian<quint64>(p+20)),crypto_pwhash_ALG_ARGON2ID13))return {};
    unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+72,&size,reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),p,72,nullptr,p+44,key.data())
        ||size!=quint64(bytes.size()-72))return {};
    return bytes;
}
QByteArray syntheticVaultHeader(const QByteArray& original,const QString& password,const QByteArray& magic,quint16 version,quint16 scheme){
    const auto plain=vaultPlain(original,password);if(plain.isEmpty()||magic.size()!=8)return {};
    auto bytes=original;auto* p=reinterpret_cast<unsigned char*>(bytes.data());std::memcpy(p,magic.constData(),8);
    qToLittleEndian<quint16>(version,p+8);qToLittleEndian<quint16>(scheme,p+14);randombytes_buf(p+44,24);
    SecureBuffer key(32),pass=SecureBuffer::fromUtf8(password);
    if(crypto_pwhash(key.data(),key.size(),reinterpret_cast<const char*>(pass.data()),pass.size(),p+28,
        qFromLittleEndian<quint32>(p+16),std::size_t(qFromLittleEndian<quint64>(p+20)),crypto_pwhash_ALG_ARGON2ID13))return {};
    unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+72,&size,reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),p,72,nullptr,p+44,key.data())
        ||size!=quint64(bytes.size()-72))return {};
    return bytes;
}
quint16 headerWord(const QByteArray& bytes,qsizetype offset){
    return bytes.size()>=offset+2?qFromLittleEndian<quint16>(reinterpret_cast<const unsigned char*>(bytes.constData())+offset):0xffff;
}
}
class StorageMigrationTests final:public QObject {
    Q_OBJECT
    QString fixtureDirectory_,profileDirectory_,password_,packagePassword_;QJsonObject expected_;
  private slots:
    void initTestCase() {
        const auto path=QFINDTESTDATA("fixtures/legacy-storage-v1/data-02/expected.json");QVERIFY(!path.isEmpty());
        fixtureDirectory_=QFileInfo(path).absolutePath();profileDirectory_=QDir(fixtureDirectory_).filePath("profile");
        expected_=QJsonDocument::fromJson(read(path)).object();QVERIFY(expected_.value("synthetic").toBool());
        QCOMPARE(sha(read(path)),QString("4078e48447e7b4b736e8670ba80f95a7efd273af2c991f62e058e069b19e6489"));
        password_=expected_.value("password").toString();packagePassword_=expected_.value("packagePassword").toString();
        const auto manifest=expected_.value("files").toObject();QCOMPARE(manifest.size(),10);
        for(auto it=manifest.begin();it!=manifest.end();++it){
            const auto bytes=read(QDir(fixtureDirectory_).filePath(it.key()));const auto item=it.value().toObject();
            QCOMPARE(bytes.size(),item.value("bytes").toInteger());QCOMPARE(sha(bytes),item.value("sha256").toString());
            QCOMPARE(bytes.left(8),item.value("magic").toString().toLatin1());
        }
    }
    void goldenLegacyReadNeverRewritesOrInventsKeys() {
        QTemporaryDir stage;QVERIFY(stage.isValid());QVERIFY(copyProfile(profileDirectory_,stage.path()));
        const auto before=files(stage.path());Vault vault(stage.filePath("credentials.finix-vault"));QString error;
        QVERIFY2(vault.unlock(password_,&error),qPrintable(error));QCOMPARE(vault.pendingOperations(),expected_.value("pendingOperations").toArray());
        AssetsLedger history(&vault,stage.filePath("assets.finix"));QVERIFY2(history.open(false,&error),qPrintable(error));
        QCOMPARE(history.snapshots().size(),2);QCOMPARE(history.coinBaseline(),expected_.value("coinBaseline").toObject());
        AssetsLedger::HistoryWindow window;QVERIFY(history.readWindow(1,1792200000000LL,&window,&error));QCOMPARE(window.snapshots.size(),2);
        SwapJournal swaps(&vault,stage.filePath("swaps.finix"));QVERIFY2(swaps.open(false,&error),qPrintable(error));QCOMPARE(swaps.records().size(),1);
        PriceArchive prices(&vault,stage.filePath("prices.finix"));QVERIFY2(prices.open(false,&error),qPrintable(error));
        QCOMPARE(prices.storedCount("coin:upbit:KRW-BTC",3600000),2);QCOMPARE(prices.conflictCount("coin:upbit:KRW-BTC",3600000),2);
        RecordTransfer transfer(vault);QVERIFY2(transfer.open(&error),qPrintable(error));QCOMPARE(transfer.records(),expected_.value("records").toArray());
        vault.lock();QCOMPARE(files(stage.path()),before);
    }
    void currentWritersUseCurrentMagicAndDomains_data() {
        QTest::addColumn<QString>("kind");QTest::addColumn<QByteArray>("magic");QTest::addColumn<QByteArray>("domain");
        QTest::newRow("vault")<<QString("vault")<<QByteArray("KEEFIVLT")<<QByteArray();
        QTest::newRow("assets")<<QString("assets")<<QByteArray("KEEFIAST")<<QByteArray("assets-ledger/v1");
        QTest::newRow("swaps")<<QString("swaps")<<QByteArray("KEEFISWP")<<QByteArray("swap-journal/v1");
        QTest::newRow("prices")<<QString("prices")<<QByteArray("KEEFIPRC")<<QByteArray("price-archive/v1");
        QTest::newRow("imported")<<QString("imported")<<QByteArray("KEEFIIMP")<<QByteArray("imported-records/v1");
        QTest::newRow("package")<<QString("package")<<QByteArray("KEEFIRTX")<<QByteArray();
    }
    void currentWritersUseCurrentMagicAndDomains() {
        QFETCH(QString,kind);QFETCH(QByteArray,magic);QFETCH(QByteArray,domain);
        QTemporaryDir temp;QVERIFY(temp.isValid());Vault vault(temp.filePath("credentials.keefi-vault"));QString error;
        QVERIFY2(vault.create(password_,&error),qPrintable(error));QVERIFY(vault.withAssetsLedgerKey(true,[](const SecureBuffer&){}));
        QByteArray bytes;
        if(kind=="vault")bytes=read(vault.filePath());
        else if(kind=="assets"){AssetsLedger store(&vault,temp.filePath("assets.keefi"));QVERIFY2(store.open(true,&error),qPrintable(error));store.lock();bytes=read(temp.filePath("assets.keefi"));}
        else if(kind=="swaps"){SwapJournal store(&vault,temp.filePath("swaps.keefi"));QVERIFY2(store.open(true,&error),qPrintable(error));store.lock();bytes=read(temp.filePath("swaps.keefi"));}
        else if(kind=="prices"){PriceArchive store(&vault,temp.filePath("prices.keefi"));QVERIFY2(store.open(true,&error),qPrintable(error));store.lock();bytes=read(temp.filePath("prices.keefi"));}
        else if(kind=="imported"){RecordTransfer store(vault);QVERIFY2(store.initialize(&error),qPrintable(error));
            QCOMPARE(QFileInfo(store.filePath()).fileName(),QString("imported-records.keefi"));bytes=read(store.filePath());}
        else{RecordTransferBundle bundle;auto password=SecureBuffer::fromUtf8(packagePassword_);
            QVERIFY2(RecordTransfer::encodePackage(bundle,password,&bytes,&error),qPrintable(error));}
        QCOMPARE(bytes.left(8),magic);
        if(!domain.isEmpty()){QVERIFY(!storePlain(bytes,vault,"KEEFI/"+domain).isEmpty());QVERIFY(storePlain(bytes,vault,"FINIX/"+domain).isEmpty());}
    }
    void convertsGoldenCopyPreservingPasswordKeyPayloadAndRecords() {
        QTemporaryDir stage;QVERIFY(stage.isValid());QVERIFY(copyProfile(profileDirectory_,stage.path()));
        const auto original=files(profileDirectory_),before=files(stage.path());StorageConversionReceipt receipt;QString error;
        QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),password_,&receipt,&error),qPrintable(error));
        QVERIFY(receipt.verified);QCOMPARE(receipt.identityScheme,QString("legacy-v1"));QVERIFY(!receipt.evidence.isEmpty());
        QCOMPARE(files(profileDirectory_),original);const auto current=files(stage.path());QCOMPARE(current.size(),before.size());
        for(auto it=before.begin();it!=before.end();++it){auto name=it.key();name.replace("finix","keefi");
            QVERIFY2(current.contains(name),qPrintable(name));QVERIFY(!current.contains(it.key()));QVERIFY(current.value(name).startsWith("KEEFI"));QVERIFY(current.value(name)!=it.value());}
        const auto oldVault=before.value("credentials.finix-vault"),newVault=current.value("credentials.keefi-vault");
        QCOMPARE(headerWord(newVault,8),quint16(2));QCOMPARE(headerWord(newVault,14),quint16(2));
        QCOMPARE(newVault.mid(10,4),oldVault.mid(10,4));QCOMPARE(newVault.mid(16,28),oldVault.mid(16,28));
        QVERIFY(newVault.mid(44,24)!=oldVault.mid(44,24));
        QCOMPARE(vaultPlain(newVault,password_),vaultPlain(oldVault,password_));QVERIFY(!vaultPlain(newVault,password_).isEmpty());
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY2(vault.unlock(password_,&error),qPrintable(error));
        QCOMPARE(vault.accounts().size(),1);QCOMPARE(vault.accounts().first().alias,QString("SYNTHETIC LEGACY ACCOUNT"));
        QString rootHash;QVERIFY(vault.withAssetsLedgerKey(false,[&](const SecureBuffer& key){rootHash=sha(QByteArray(reinterpret_cast<const char*>(key.data()),qsizetype(key.size())));}));
        QCOMPARE(rootHash,expected_.value("ledgerRootSha256").toString());QCOMPARE(vault.pendingOperations(),expected_.value("pendingOperations").toArray());
        AssetsLedger history(&vault,stage.filePath("assets.keefi"));QVERIFY2(history.open(false,&error),qPrintable(error));
        QCOMPARE(history.snapshots().size(),2);QCOMPARE(history.events().size(),1);QCOMPARE(history.orders().size(),1);QCOMPARE(history.coinBaseline(),expected_.value("coinBaseline").toObject());
        AssetsLedger::HistoryWindow window;QVERIFY2(history.readWindow(1,1792200000000LL,&window,&error),qPrintable(error));QCOMPARE(window.snapshots.size(),2);
        SwapJournal swaps(&vault,stage.filePath("swaps.keefi"));QVERIFY2(swaps.open(false,&error),qPrintable(error));QCOMPARE(swaps.records().size(),1);QCOMPARE(swaps.records().first().actualFeeLamports,QString("5000"));
        PriceArchive prices(&vault,stage.filePath("prices.keefi"));QVERIFY2(prices.open(false,&error),qPrintable(error));QCOMPARE(prices.storedCount("coin:upbit:KRW-BTC",3600000),2);QCOMPARE(prices.conflictCount("coin:upbit:KRW-BTC",3600000),2);
        auto notice=storePlain(read(stage.filePath("prices.keefi.exit")),vault,"KEEFI/price-archive/v1");
        QCOMPARE(notice.size(),256);while(!notice.isEmpty()&&notice.back()=='\0')notice.chop(1);QCOMPARE(QJsonDocument::fromJson(notice).object().value("reason").toString(),QString("timeout"));
        RecordTransfer transfer(vault);QVERIFY2(transfer.open(&error),qPrintable(error));QCOMPARE(transfer.records(),expected_.value("records").toArray());
        vault.lock();QCOMPARE(files(stage.path()),current);
    }
    void rewrittenEvidenceKeepsOriginalAndCurrentDigestsDistinct() {
        QTemporaryDir stage;QVERIFY(copyProfile(profileDirectory_,stage.path()));StorageConversionReceipt receipt;QString error;
        QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),password_,&receipt,&error),qPrintable(error));
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY(vault.unlock(password_));
        const auto plain=storePlain(read(stage.filePath("assets.keefi")),vault,"KEEFI/assets-ledger/v1");const auto migration=QJsonDocument::fromJson(plain).object().value("migration").toObject();
        const auto expectedFiles=expected_.value("files").toObject();const auto originalBackup=QDir(profileDirectory_).entryList({"assets.finix.pre-schema3-*.finix"},QDir::Files).value(0);QVERIFY(!originalBackup.isEmpty());
        QCOMPARE(migration.value("sourceDigest").toString(),expectedFiles.value("profile/"+originalBackup).toObject().value("sha256").toString());QVERIFY(migration.value("backup").toString().startsWith("assets.keefi.pre-schema3-"));
        const auto evidence=migration.value("rewriteEvidence").toObject();QCOMPARE(evidence.value("version").toInt(),1);
        const auto rewritten=evidence.value("files").toArray();QCOMPARE(rewritten.size(),2);
        for(const auto& value:rewritten){const auto row=value.toObject();const auto name=row.value("name").toString();
            QVERIFY(!name.contains("finix",Qt::CaseInsensitive));QVERIFY(QFileInfo::exists(stage.filePath(name)));QCOMPARE(row.value("cipherDigest").toString(),sha(read(stage.filePath(name))));
            auto legacyName=name;legacyName.replace("keefi","finix");QCOMPARE(row.value("sourceDigest").toString(),expectedFiles.value("profile/"+legacyName).toObject().value("sha256").toString());QVERIFY(row.value("sourceDigest")!=row.value("cipherDigest"));}
    }
    void oldPortablePackageRemainsDuplicateAfterConversion() {
        QTemporaryDir stage;QVERIFY(copyProfile(profileDirectory_,stage.path()));StorageConversionReceipt receipt;QString error;
        QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),password_,&receipt,&error),qPrintable(error));
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY(vault.unlock(password_));RecordTransfer transfer(vault);QVERIFY(transfer.open(&error));
        auto password=SecureBuffer::fromUtf8(packagePassword_);RecordTransferBundle incoming;
        QVERIFY2(RecordTransfer::decodePackage(read(QDir(fixtureDirectory_).filePath("portable.finixrtx")),password,&incoming,&error),qPrintable(error));
        QCOMPARE(incoming.records,expected_.value("records").toArray());const auto before=read(transfer.filePath());RecordTransferResult result;
        QVERIFY2(transfer.commit(incoming,transfer.generation(),&result,&error),qPrintable(error));QCOMPARE(result.added,0);QCOMPARE(result.duplicates,7);QCOMPARE(read(transfer.filePath()),before);
        QByteArray encoded;QVERIFY(RecordTransfer::encodePackage(incoming,password,&encoded,&error));QCOMPARE(encoded.left(8),QByteArray("KEEFIRTX"));
        RecordTransferBundle decoded;QVERIFY(RecordTransfer::decodePackage(encoded,password,&decoded));QCOMPARE(decoded.records,incoming.records);
    }
    void settingsOnlyCopyDoesNotInventAVaultOrKey(){
        QTemporaryDir stage;QVERIFY(stage.isValid());
        const QByteArray note="[General]\nuserNote=User text FINIX stays exactly here\n";
        QVERIFY(write(stage.filePath("preferences.ini"),note));const auto before=files(stage.path());
        StorageConversionReceipt receipt;QString error;
        QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),{},&receipt,&error),qPrintable(error));
        QVERIFY(receipt.verified);QCOMPARE(receipt.identityScheme,QString("legacy-v1"));
        QVERIFY(receipt.evidence.value("convertedFiles").toArray().isEmpty());QCOMPARE(files(stage.path()),before);
    }
    void ownerReadOnlyInputConvertsWithoutWeakeningOutputPermissions(){
        QTemporaryDir stage;QVERIFY(stage.isValid());QVERIFY(copyProfile(profileDirectory_,stage.path()));
        const auto original=files(profileDirectory_);
        QVERIFY(QFile::setPermissions(stage.filePath("assets.finix"),QFileDevice::ReadOwner));
        StorageConversionReceipt receipt;QString error;
        QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),password_,&receipt,&error),qPrintable(error));
        QVERIFY(receipt.verified);QCOMPARE(files(profileDirectory_),original);
        QVERIFY(!QFileInfo::exists(stage.filePath("assets.finix")));
        const auto current=stage.filePath("assets.keefi");QCOMPARE(read(current).left(8),QByteArray("KEEFIAST"));
        const auto permissions=QFile::permissions(current);
        QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(permissions&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ExeGroup
            |QFileDevice::ReadOther|QFileDevice::WriteOther|QFileDevice::ExeOther)));
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY(vault.unlock(password_));
        AssetsLedger ledger(&vault,current);QVERIFY2(ledger.open(false,&error),qPrintable(error));
        QCOMPARE(ledger.snapshots().size(),2);QCOMPARE(ledger.coinBaseline(),expected_.value("coinBaseline").toObject());
    }
    void preservesExistingPasswordBytes_data(){
        QTest::addColumn<QString>("password");
        QTest::newRow("older-weak")<<QString("abc");
        QTest::newRow("unicode-with-spaces")<<QString::fromUtf8("  예전 금고 암호 🔐  ");
    }
    void preservesExistingPasswordBytes(){
        QFETCH(QString,password);QTemporaryDir stage;QVERIFY(copyProfile(profileDirectory_,stage.path()));
        const auto original=read(stage.filePath("credentials.finix-vault"));const auto plain=vaultPlain(original,password_);
        const auto legacy=encodeSyntheticLegacyVault(original,plain,password);QVERIFY(!legacy.isEmpty());
        QVERIFY(write(stage.filePath("credentials.finix-vault"),legacy));StorageConversionReceipt receipt;QString error;
        QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),password,&receipt,&error),qPrintable(error));QVERIFY(receipt.verified);
        const auto current=read(stage.filePath("credentials.keefi-vault"));
        QCOMPARE(headerWord(current,8),quint16(2));QCOMPARE(headerWord(current,14),quint16(2));
        QCOMPARE(current.mid(10,4),legacy.mid(10,4));QCOMPARE(current.mid(16,28),legacy.mid(16,28));QCOMPARE(vaultPlain(current,password),plain);
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY2(vault.unlock(password,&error),qPrintable(error));
        QCOMPARE(vault.pendingOperations(),expected_.value("pendingOperations").toArray());QVERIFY(vault.hasAssetsLedgerKey());
    }
    void currentSchemaRejectsLegacyOrUnknownEvidence_data(){
        QTest::addColumn<QString>("fault");
        QTest::newRow("missing-rewrite-evidence")<<QString("missing");
        QTest::newRow("unknown-evidence-key")<<QString("unknown");
    }
    void currentSchemaRejectsLegacyOrUnknownEvidence(){
        QFETCH(QString,fault);QTemporaryDir stage;QVERIFY(copyProfile(profileDirectory_,stage.path()));
        StorageConversionReceipt receipt;QString error;QVERIFY(StorageMigration::convertStagedCopy(stage.path(),password_,&receipt,&error));
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY(vault.unlock(password_));
        auto root=QJsonDocument::fromJson(storePlain(read(stage.filePath("assets.keefi")),vault,"KEEFI/assets-ledger/v1")).object();
        auto migration=root.value("migration").toObject();
        if(fault=="missing")migration.remove("rewriteEvidence");
        else{auto evidence=migration.value("rewriteEvidence").toObject();evidence["unexpected"]=true;migration["rewriteEvidence"]=evidence;}
        root["migration"]=migration;
        const auto bytes=encodeSyntheticStore(QJsonDocument(root).toJson(QJsonDocument::Compact),vault,"KEEFIAST","KEEFI/assets-ledger/v1");
        QVERIFY(!bytes.isEmpty());QVERIFY(write(stage.filePath("assets.keefi"),bytes));const auto before=files(stage.path());
        AssetsLedger ledger(&vault,stage.filePath("assets.keefi"));QVERIFY(!ledger.open(false,&error));QVERIFY(!error.isEmpty());
        vault.lock();auto after=files(stage.path());auto expected=before;expected.remove("credentials.keefi-vault.lock");QCOMPARE(after,expected);
    }
    void rewritesOnlyTheRegisteredValuationPolicyField(){
        QTemporaryDir stage;QVERIFY(copyProfile(profileDirectory_,stage.path()));QString error;
        QJsonObject beforeBaseline;
        {
            Vault vault(stage.filePath("credentials.finix-vault"));QVERIFY(vault.unlock(password_));
            auto root=QJsonDocument::fromJson(storePlain(read(stage.filePath("assets.finix")),vault,"FINIX/assets-ledger/v1")).object();
            beforeBaseline=root.value("coinBaseline").toObject();beforeBaseline["policy"]="finix-valuation-v2";root["coinBaseline"]=beforeBaseline;
            const auto bytes=encodeSyntheticStore(QJsonDocument(root).toJson(QJsonDocument::Compact),vault,"FINIXAST","FINIX/assets-ledger/v1");
            QVERIFY(!bytes.isEmpty());QVERIFY(write(stage.filePath("assets.finix"),bytes));
        }
        StorageConversionReceipt receipt;QVERIFY2(StorageMigration::convertStagedCopy(stage.path(),password_,&receipt,&error),qPrintable(error));
        Vault vault(stage.filePath("credentials.keefi-vault"));QVERIFY(vault.unlock(password_));
        AssetsLedger ledger(&vault,stage.filePath("assets.keefi"));QVERIFY(ledger.open(false,&error));
        auto expected=beforeBaseline;expected["policy"]="keefi-valuation-v2";QCOMPARE(ledger.coinBaseline(),expected);
        const auto fields=receipt.evidence.value("machineFields").toArray();QCOMPARE(fields.size(),1);
        QCOMPARE(fields.first().toObject().value("field").toString(),QString("coinBaseline.policy"));
        QCOMPARE(fields.first().toObject().value("from").toString(),QString("finix-valuation-v2"));
        QCOMPARE(fields.first().toObject().value("to").toString(),QString("keefi-valuation-v2"));
    }
    void restoringGoldenVaultPreservesItsSourceAndUsesCurrentContainer(){
        QTemporaryDir destination;QVERIFY(destination.isValid());
        QTemporaryDir sourceProfile;QVERIFY(sourceProfile.isValid());
        QVERIFY(copyProfile(profileDirectory_,sourceProfile.path()));
        const auto source=sourceProfile.filePath("credentials.finix-vault");
        const auto original=read(source);QString error;
        Vault vault(destination.filePath("credentials.keefi-vault"));
        QVERIFY2(vault.restoreBackup(source,password_,&error),qPrintable(error));
        QCOMPARE(read(source),original);QCOMPARE(read(vault.filePath()).left(8),QByteArray("KEEFIVLT"));
        QCOMPARE(vaultPlain(read(vault.filePath()),password_),vaultPlain(original,password_));
        QCOMPARE(vault.pendingOperations(),expected_.value("pendingOperations").toArray());
    }
    void freshVaultHeaderCarriesAuthenticatedCurrentScheme(){
        QTemporaryDir directory;Vault vault(directory.filePath("credentials.keefi-vault"));QString error;
        QVERIFY2(vault.create(password_,&error),qPrintable(error));const auto bytes=read(vault.filePath());
        QCOMPARE(bytes.left(8),QByteArray("KEEFIVLT"));QCOMPARE(headerWord(bytes,8),quint16(2));QCOMPARE(headerWord(bytes,14),quint16(1));
        QCOMPARE(headerWord(bytes,10),quint16(1));QCOMPARE(headerWord(bytes,12),quint16(1));
        QString scheme;QVERIFY2(Vault::authenticatedBackupIdentity(vault.filePath(),password_,&scheme,&error),qPrintable(error));
        QCOMPARE(scheme,QString("keefi-v1"));QCOMPARE(read(vault.filePath()),bytes);
    }
    void authenticatedHeaderDistinguishesLegacyAndCurrent_data(){
        QTest::addColumn<QByteArray>("magic");QTest::addColumn<quint16>("version");QTest::addColumn<quint16>("code");
        QTest::addColumn<bool>("accepted");QTest::addColumn<QString>("scheme");
        QTest::newRow("legacy-v1")<<QByteArray("FINIXVLT")<<quint16(1)<<quint16(0)<<true<<QString("legacy-v1");
        QTest::newRow("legacy-future-version")<<QByteArray("FINIXVLT")<<quint16(2)<<quint16(0)<<false<<QString();
        QTest::newRow("legacy-reserved-nonzero")<<QByteArray("FINIXVLT")<<quint16(1)<<quint16(1)<<false<<QString();
        QTest::newRow("current-keefi")<<QByteArray("KEEFIVLT")<<quint16(2)<<quint16(1)<<true<<QString("keefi-v1");
        QTest::newRow("current-legacy")<<QByteArray("KEEFIVLT")<<quint16(2)<<quint16(2)<<true<<QString("legacy-v1");
        QTest::newRow("unreleased-current-v1")<<QByteArray("KEEFIVLT")<<quint16(1)<<quint16(0)<<false<<QString();
        QTest::newRow("current-missing-scheme")<<QByteArray("KEEFIVLT")<<quint16(2)<<quint16(0)<<false<<QString();
        QTest::newRow("current-unknown-scheme")<<QByteArray("KEEFIVLT")<<quint16(2)<<quint16(3)<<false<<QString();
        QTest::newRow("current-future-version")<<QByteArray("KEEFIVLT")<<quint16(3)<<quint16(1)<<false<<QString();
    }
    void authenticatedHeaderDistinguishesLegacyAndCurrent(){
        QFETCH(QByteArray,magic);QFETCH(quint16,version);QFETCH(quint16,code);QFETCH(bool,accepted);QFETCH(QString,scheme);
        QTemporaryDir directory;QVERIFY(directory.isValid());
        const auto original=read(QDir(profileDirectory_).filePath("credentials.finix-vault"));
        const auto candidate=syntheticVaultHeader(original,password_,magic,version,code);QVERIFY(!candidate.isEmpty());
        const auto path=directory.filePath("candidate.keefi-vault");QVERIFY(write(path,candidate));
        Vault vault(path);QString error;QCOMPARE(vault.unlock(password_,&error),accepted);
        if(accepted)QCOMPARE(vault.pendingOperations(),expected_.value("pendingOperations").toArray());
        QString observed="MUST_BE_CLEARED";
        QCOMPARE(Vault::authenticatedBackupIdentity(path,password_,&observed,&error),accepted);
        QCOMPARE(observed,scheme);vault.lock();QCOMPARE(read(path),candidate);
        QCOMPARE(files(directory.path()).size(),1);
    }
    void authenticatedSchemeCannotBeChangedByHeaderTampering(){
        QTemporaryDir directory;const auto original=read(QDir(profileDirectory_).filePath("credentials.finix-vault"));
        auto candidate=syntheticVaultHeader(original,password_,"KEEFIVLT",2,2);QVERIFY(!candidate.isEmpty());
        qToLittleEndian<quint16>(1,reinterpret_cast<unsigned char*>(candidate.data())+14);
        const auto path=directory.filePath("candidate.keefi-vault");QVERIFY(write(path,candidate));
        QString scheme="MUST_BE_CLEARED",error;
        QVERIFY(!Vault::authenticatedBackupIdentity(path,password_,&scheme,&error));QVERIFY(scheme.isEmpty());QVERIFY(!error.isEmpty());
        Vault vault(path);QVERIFY(!vault.unlock(password_,&error));QCOMPARE(read(path),candidate);
    }
    void profileIdentityMismatchRefusesUnlock_data(){
        QTest::addColumn<bool>("legacySource");
        QTest::newRow("legacy-backup-in-current-profile")<<true;
        QTest::newRow("current-backup-in-legacy-profile")<<false;
    }
    void profileIdentityMismatchRefusesUnlock(){
        QFETCH(bool,legacySource);QTemporaryDir directory;QString error;
        const auto path=directory.filePath("credentials.keefi-vault");
        if(legacySource)QVERIFY(write(path,read(QDir(profileDirectory_).filePath("credentials.finix-vault"))));
        else{Vault created(path);QVERIFY(created.create(password_));}
        const auto before=files(directory.path());
        Vault vault(path,legacySource?QString("keefi-v1"):QString("legacy-v1"),nullptr);bool unlockedSeen=false;
        connect(&vault,&Vault::stateChanged,this,[&]{unlockedSeen=unlockedSeen||vault.isUnlocked();});
        QVERIFY(!vault.unlock(password_,&error));QVERIFY(!error.isEmpty());QVERIFY(!unlockedSeen);QVERIFY(!vault.isUnlocked());QVERIFY(vault.accounts().isEmpty());
        vault.lock();QCOMPARE(files(directory.path()),before);
    }
    void profileIdentityMismatchRefusesRestoreBeforeCreatingDestination_data(){profileIdentityMismatchRefusesUnlock_data();}
    void profileIdentityMismatchRefusesRestoreBeforeCreatingDestination(){
        QFETCH(bool,legacySource);QTemporaryDir source,destination;QString error;
        const auto sourcePath=source.filePath("backup.keefi-vault"),target=destination.filePath("credentials.keefi-vault");
        if(legacySource)QVERIFY(write(sourcePath,read(QDir(profileDirectory_).filePath("credentials.finix-vault"))));
        else{Vault created(sourcePath);QVERIFY(created.create(password_));}
        const auto original=files(source.path()),before=files(destination.path());
        Vault vault(target,legacySource?QString("keefi-v1"):QString("legacy-v1"),nullptr);bool unlockedSeen=false;
        connect(&vault,&Vault::stateChanged,this,[&]{unlockedSeen=unlockedSeen||vault.isUnlocked();});
        QVERIFY(!vault.restoreBackup(sourcePath,password_,&error));QVERIFY(!error.isEmpty());QVERIFY(!unlockedSeen);QVERIFY(!vault.isUnlocked());
        vault.lock();QCOMPARE(files(source.path()),original);QCOMPARE(files(destination.path()),before);
    }
    void legacyProfileCurrentVaultKeepsSchemeAcrossPasswordChangeAndBackup(){
        QTemporaryDir source,destination;QString error;
        Vault vault(source.filePath("credentials.keefi-vault"),QString("legacy-v1"),nullptr);
        QVERIFY2(vault.create(password_,&error),qPrintable(error));QVERIFY(vault.withAssetsLedgerKey(true,[](const SecureBuffer&){}));
        auto original=read(vault.filePath());QCOMPARE(headerWord(original,8),quint16(2));QCOMPARE(headerWord(original,14),quint16(2));
        const auto plain=vaultPlain(original,password_);const QString next="Changed synthetic Vault password 2027!";
        QVERIFY2(vault.changePassword(password_,next,&error),qPrintable(error));
        const auto changed=read(vault.filePath());QCOMPARE(headerWord(changed,8),quint16(2));QCOMPARE(headerWord(changed,14),quint16(2));
        QCOMPARE(vaultPlain(changed,next),plain);vault.lock();QString scheme;
        QVERIFY2(Vault::authenticatedBackupIdentity(vault.filePath(),next,&scheme,&error),qPrintable(error));QCOMPARE(scheme,QString("legacy-v1"));
        Vault restored(destination.filePath("credentials.keefi-vault"),QString("legacy-v1"),nullptr);
        QVERIFY2(restored.restoreBackup(vault.filePath(),next,&error),qPrintable(error));
        QCOMPARE(read(restored.filePath()),changed);QCOMPARE(read(vault.filePath()),changed);
    }
    void inactiveBackupRestorePreservesIdentityAndAuthenticatedBytes_data(){
        QTest::addColumn<QString>("kind");QTest::addColumn<bool>("readOnly");
        QTest::newRow("legacy-v1")<<QString("legacy")<<false;
        QTest::newRow("legacy-v1-readonly")<<QString("legacy")<<true;
        QTest::newRow("current-legacy-v2")<<QString("current-legacy")<<false;
        QTest::newRow("current-legacy-v2-readonly")<<QString("current-legacy")<<true;
        QTest::newRow("current-keefi-v2")<<QString("current-keefi")<<false;
    }
    void inactiveBackupRestorePreservesIdentityAndAuthenticatedBytes(){
        QFETCH(QString,kind);QFETCH(bool,readOnly);QTemporaryDir source,stage;QVERIFY(source.isValid());QVERIFY(stage.isValid());QString error;
        const auto input=source.filePath("owned-backup.keefi-vault");
        if(kind=="current-keefi"){Vault created(input);QVERIFY(created.create(password_));QVERIFY(created.withAssetsLedgerKey(true,[](const SecureBuffer&){}));}
        else{
            auto bytes=read(QDir(profileDirectory_).filePath("credentials.finix-vault"));
            if(kind=="current-legacy")bytes=syntheticVaultHeader(bytes,password_,"KEEFIVLT",2,2);
            QVERIFY(!bytes.isEmpty());QVERIFY(write(input,bytes));
        }
        if(readOnly)QVERIFY(QFile::setPermissions(input,QFileDevice::ReadOwner));
        const auto sourceBytes=read(input);const auto sourceFiles=files(source.path());const auto permissions=QFile::permissions(input);
        const QByteArray settings="[General]\nuserNote=Keep user FINIX text exactly\n";
        QVERIFY(write(stage.filePath("preferences.ini"),settings));StorageConversionReceipt receipt;
        QVERIFY2(StorageMigration::restoreBackupStagedCopy(stage.path(),input,password_,&receipt,&error),qPrintable(error));
        QVERIFY(receipt.verified);const auto scheme=kind=="current-keefi"?QString("keefi-v1"):QString("legacy-v1");
        QCOMPARE(receipt.identityScheme,scheme);QCOMPARE(files(source.path()),sourceFiles);QCOMPARE(QFile::permissions(input),permissions);
        QCOMPARE(read(stage.filePath("preferences.ini")),settings);QCOMPARE(files(stage.path()).size(),2);
        const auto output=stage.filePath("credentials.keefi-vault");const auto current=read(output);
        QCOMPARE(current.left(8),QByteArray("KEEFIVLT"));QCOMPARE(headerWord(current,8),quint16(2));
        QCOMPARE(headerWord(current,14),quint16(kind=="current-keefi"?1:2));QCOMPARE(vaultPlain(current,password_),vaultPlain(sourceBytes,password_));
        if(kind=="legacy"){QVERIFY(current!=sourceBytes);QCOMPARE(current.mid(10,4),sourceBytes.mid(10,4));QCOMPARE(current.mid(16,28),sourceBytes.mid(16,28));}
        else QCOMPARE(current,sourceBytes);
        const auto outputPermissions=QFile::permissions(output);
        QVERIFY(outputPermissions.testFlag(QFileDevice::ReadOwner));QVERIFY(outputPermissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(outputPermissions&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::WriteOther|QFileDevice::ExeOther)));
        const auto evidence=receipt.evidence.value("restoredBackup").toObject();QCOMPARE(evidence.size(),5);
        QCOMPARE(evidence.value("name").toString(),QString("credentials.keefi-vault"));
        QCOMPARE(evidence.value("sourceDigest").toString(),sha(sourceBytes));QCOMPARE(evidence.value("sourceBytes").toInteger(),sourceBytes.size());
        QCOMPARE(evidence.value("cipherDigest").toString(),sha(current));QCOMPARE(evidence.value("cipherBytes").toInteger(),current.size());
        Vault reopened(output,scheme,nullptr);QVERIFY2(reopened.unlock(password_,&error),qPrintable(error));QVERIFY(reopened.hasAssetsLedgerKey());
        if(kind!="current-keefi"){QCOMPARE(reopened.accounts().size(),1);QCOMPARE(reopened.pendingOperations(),expected_.value("pendingOperations").toArray());}
    }
    void encryptedStorageNamesCoverEveryOwnedWriter_data(){
        QTest::addColumn<QString>("name");QTest::addColumn<bool>("blocked");
        for(const auto& brand:{QString("finix"),QString("keefi")}){
            const QStringList names={"credentials."+brand+"-vault","credentials."+brand+"-vault.lock","assets."+brand,"assets-current."+brand,
                "swaps."+brand,"prices."+brand,"prices."+brand+".exit","imported-records."+brand,
                "assets."+brand+".archive-11111111111111111111111111111111."+brand,
                "assets."+brand+".pre-schema3-"+QString(64,'a')+"."+brand,"prices."+brand+".unreadable-123",
                "user-backup."+brand+"-vault","user-records."+brand+"rtx",
                "."+brand+"-vault-ABC","."+brand+"-restore-ABC","."+brand+"-assets-ABC","."+brand+"-prices-ABC","."+brand+"-record-transfer-ABC"};
            for(const auto& name:names)QTest::newRow(qPrintable(name))<<name<<true;
        }
        QTest::newRow("migration-temporary")<<QString(".keefi-storage-abc")<<true;
        for(const auto* name:{"preferences.ini","user-note.txt",".keefi-profile.json",".keefi-profile-v2.json",".keefi-profile-copy.json","instance.lock"})
            QTest::newRow(name)<<QString::fromLatin1(name)<<false;
    }
    void encryptedStorageNamesCoverEveryOwnedWriter(){
        QFETCH(QString,name);QFETCH(bool,blocked);QCOMPARE(storage_detail::encryptedStorageName(name),blocked);
    }
    void inactiveBackupRestoreRejectsOccupiedOrUnsafePathsWithoutWriting_data(){
        QTest::addColumn<QString>("fault");
        for(const auto* name:{"password","tampered","occupied-current-vault","occupied-legacy-vault","assets","archive","pending-temporary",
            "target-directory","dangling-target","stage-symlink","source-symlink","source-hardlink","stage-mode","source-mode"})
            QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void inactiveBackupRestoreRejectsOccupiedOrUnsafePathsWithoutWriting(){
        QFETCH(QString,fault);QTemporaryDir source,stage,other;QString error,password=password_,stagePath=stage.path();
        auto input=source.filePath("backup.finix-vault");QVERIFY(write(input,read(QDir(profileDirectory_).filePath("credentials.finix-vault"))));
        if(fault=="password")password="Wrong synthetic restore password!";
        else if(fault=="tampered"){auto bytes=read(input);bytes.back()=char(bytes.back()^1);QVERIFY(write(input,bytes));}
        else if(fault=="occupied-current-vault")QVERIFY(write(stage.filePath("credentials.keefi-vault"),"KEEP_CURRENT_VAULT"));
        else if(fault=="occupied-legacy-vault")QVERIFY(write(stage.filePath("credentials.finix-vault"),"KEEP_LEGACY_VAULT"));
        else if(fault=="assets")QVERIFY(write(stage.filePath("assets-current.keefi"),"KEEP_CURRENT_ASSETS"));
        else if(fault=="archive")QVERIFY(write(stage.filePath("assets.keefi.archive-11111111111111111111111111111111.keefi"),"KEEP_ARCHIVE"));
        else if(fault=="pending-temporary")QVERIFY(write(stage.filePath(".keefi-prices-ABC"),"KEEP_PENDING_WRITE"));
        else if(fault=="target-directory")QVERIFY(QDir(stage.path()).mkdir("credentials.keefi-vault"));
        else if(fault=="stage-mode")QVERIFY(QFile::setPermissions(stage.path(),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup));
        else if(fault=="source-mode")QVERIFY(QFile::setPermissions(input,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadOther));
        else{
#ifdef Q_OS_UNIX
            if(fault=="source-hardlink"){
                QCOMPARE(::link(QFile::encodeName(input).constData(),QFile::encodeName(source.filePath("second-link")).constData()),0);
            }else if(fault=="source-symlink"){
                const auto link=source.filePath("source-link");QCOMPARE(::symlink(QFile::encodeName(input).constData(),QFile::encodeName(link).constData()),0);input=link;
            }else if(fault=="stage-symlink"){
                stagePath=other.filePath("stage-link");QCOMPARE(::symlink(QFile::encodeName(stage.path()).constData(),QFile::encodeName(stagePath).constData()),0);
            }else if(fault=="dangling-target"){
                QCOMPARE(::symlink(QFile::encodeName(other.filePath("absent-target")).constData(),QFile::encodeName(stage.filePath("credentials.keefi-vault")).constData()),0);
            }
#else
            QSKIP("Link fixtures require Unix.");
#endif
        }
        const auto original=files(source.path()),before=files(stage.path()),outside=files(other.path());
        const auto entries=QDir(stage.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDir::Name);
        StorageConversionReceipt receipt;receipt.verified=true;
        QVERIFY(!StorageMigration::restoreBackupStagedCopy(stagePath,input,password,&receipt,&error));QVERIFY(!receipt.verified);QVERIFY(!error.isEmpty());
        QCOMPARE(files(source.path()),original);QCOMPARE(files(stage.path()),before);QCOMPARE(files(other.path()),outside);
        QCOMPARE(QDir(stage.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDir::Name),entries);
    }
    void refusesUnsafeOrInvalidCopyBeforeWriting_data() {
        QTest::addColumn<QString>("fault");
        for(const auto* name:{"password","vault-tag","asset-tag","price-tag","missing-backup","missing-vault","collision","symlink","hardlink","unsafe-mode","unknown-legacy-artifact"})QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void refusesUnsafeOrInvalidCopyBeforeWriting() {
        QFETCH(QString,fault);QTemporaryDir stage,outside;QVERIFY(copyProfile(profileDirectory_,stage.path()));QString password=password_;
        if(fault=="password")password="Wrong synthetic password 2026!";
        else if(fault.endsWith("-tag")){const auto name=fault=="vault-tag"?"credentials.finix-vault":fault=="asset-tag"?"assets.finix":"prices.finix";
            const auto path=stage.filePath(name);auto bytes=read(path);QVERIFY(!bytes.isEmpty());bytes.back()=char(bytes.back()^1);QVERIFY(write(path,bytes));}
        else if(fault=="missing-backup"){const auto name=QDir(stage.path()).entryList({"assets.finix.pre-schema3-*.finix"},QDir::Files).value(0);QVERIFY(!name.isEmpty());QVERIFY(QFile::remove(stage.filePath(name)));}
        else if(fault=="missing-vault")QVERIFY(QFile::remove(stage.filePath("credentials.finix-vault")));
        else if(fault=="collision")QVERIFY(write(stage.filePath("credentials.keefi-vault"),"DO_NOT_REPLACE"));
        else if(fault=="unsafe-mode")QVERIFY(QFile::setPermissions(stage.filePath("assets.finix"),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadGroup));
        else if(fault=="unknown-legacy-artifact")QVERIFY(write(stage.filePath("prices.finix.unreadable-123"),"PRESERVE_THIS_UNRECOGNIZED_COPY"));
        else{const auto external=outside.filePath("owned-synthetic.bin");QVERIFY(write(external,read(stage.filePath("assets.finix"))));QVERIFY(QFile::remove(stage.filePath("assets.finix")));
            if(fault=="symlink")QVERIFY(QFile::link(external,stage.filePath("assets.finix")));
            else{
#ifdef Q_OS_UNIX
                QCOMPARE(::link(QFile::encodeName(external).constData(),QFile::encodeName(stage.filePath("assets.finix")).constData()),0);
#else
                QSKIP("Hard-link fixture requires Unix link()");
#endif
            }}
        const auto before=files(stage.path()),source=files(profileDirectory_);StorageConversionReceipt receipt;receipt.verified=true;QString error;
        QVERIFY(!StorageMigration::convertStagedCopy(stage.path(),password,&receipt,&error));QVERIFY(!receipt.verified);QVERIFY(!error.isEmpty());QCOMPARE(files(stage.path()),before);QCOMPARE(files(profileDirectory_),source);
    }
};
} // namespace finix
QTEST_GUILESS_MAIN(keefi::StorageMigrationTests)
#include "tst_storage_migration.moc"

