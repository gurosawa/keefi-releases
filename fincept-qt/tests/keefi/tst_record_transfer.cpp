#include "keefi/RecordTransfer.h"
#include "keefi/AssetsLedger.h"
#include "keefi/SwapJournal.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/Vault.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QProcess>
#include <QCoreApplication>
#include <cstdlib>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <sodium.h>
#include <thread>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
namespace keefi {
namespace {
const QString Password="Fake transfer Vault password 2026!",PackagePassword="Fake portable Records password 2026!";
const QString Fingerprint(64,'a'),Binding(64,'b');
constexpr qint64 At=1792200000000LL;
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
bool write(const QString& path,const QByteArray& bytes){QFile f(path);return f.open(QIODevice::WriteOnly|QIODevice::Truncate)&&f.write(bytes)==bytes.size()&&f.flush();}
AssetEvidence evidence(){return {"FAKE_OFFICIAL_SOURCE","https://example.invalid/official",Fingerprint,At,At,true};}
AssetSnapshot snapshot(const QString& id="one"){
    AssetSnapshot s;s.id=id;s.accountScope=Fingerprint;s.connectionBinding=Binding;s.connectionId="opaque-local-registration";s.provider="binance";s.environment="live";s.market="spot";s.timeMs=At;s.balanceComplete=true;s.reconciled=true;s.coverage="FAKE_COVERAGE_CONTAINS_"+Fingerprint;s.evidence=evidence();
    AssetComponent c;c.id="cash";c.asset="KRW";c.quantity="100.01";c.kind=AssetComponentKind::Cash;c.evidence=evidence();s.components={c};return s;
}
AssetEvent assetEvent(){AssetEvent e;e.id="local:"+Fingerprint;e.accountScope=Fingerprint;e.asset="KRW";e.quantity="50";e.officialId="FAKE_OFFICIAL_FILL";e.timeMs=At;e.kind=AssetEventKind::Fill;e.evidence=evidence();return e;}
AssetOrderObservation order(){AssetOrderObservation o;o.accountScope=Fingerprint;o.provider="binance";o.market="spot";o.orderId="FAKE_OFFICIAL_ORDER";o.observedAtMs=At;o.side="buy";o.type="limit";o.status="partial";o.price="1.25";o.quantity="3";o.executed="1";o.quoteAmount="1.25";o.quantityUnit="base";o.currency="KRW";o.evidence=evidence();return o;}
QJsonObject pending(){return {{"id","fake-pending-id"},{"accountId","fake-old-registration"},{"fingerprint",Fingerprint},{"provider","binance"},{"environment","live"},{"symbol","BTCUSDT"},{"kind","place"},{"clientId","FAKE_ORIGINAL_CLIENT"},{"orderId",""},{"replacementClientId",""},{"state","pending"},{"createdAt",QString::number(At)},{"quantity","1"},{"price","1.25"},{"quoteAmount",""},{"side","buy"},{"type","limit"}};}
SwapPendingRecord swap(){SwapPendingRecord r;r.id="00000000-0000-4000-8000-000000000001";r.phantomAccountId="00000000-0000-4000-8000-000000000002";r.jupiterAccountId="00000000-0000-4000-8000-000000000003";r.credentialRef="00000000-0000-4000-8000-000000000004";r.chain="solana-mainnet";r.wallet=SolanaTransactionCodec::encodeBase58(QByteArray(32,'x'));r.inputMint=SolMint;r.outputMint=UsdcMint;r.inputAtomic="1000000";r.minimumRouteOutput="500";r.slippageBps="50";r.requestId="FAKE_REQUEST";r.transactionId=SolanaTransactionCodec::encodeBase58(QByteArray(64,'z'));r.messageHash=QString(64,'c');r.lastValidBlockHeight="100";r.createdAt=QString::number(At);r.state="submitted_unknown";return r;}
bool collectOne(Vault& vault,RecordTransfer& transfer,RecordTransferBundle* out,QString* error){
    AssetsLedger history(&vault,QFileInfo(vault.filePath()).dir().filePath("assets.keefi"));if(!history.open(true,error)||!history.append({snapshot()},{},{},error)||!transfer.initialize(error))return false;
    RecordTransferSelection selection;selection.events=selection.orders=selection.swaps=selection.pending=false;return transfer.collect(selection,&history,nullptr,out,error);
}
QByteArray rewriteFake(const QByteArray& input,const std::function<void(QJsonObject&)>& mutate){
    const auto* p=reinterpret_cast<const unsigned char*>(input.constData());const auto password=PackagePassword.toUtf8();QByteArray key(32,'\0');
    if(crypto_pwhash(reinterpret_cast<unsigned char*>(key.data()),32,password.constData(),password.size(),p+28,qFromLittleEndian<quint32>(p+16),std::size_t(qFromLittleEndian<quint64>(p+20)),crypto_pwhash_ALG_ARGON2ID13))return {};
    QByteArray plain(input.size()-88,'\0');unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()),&size,nullptr,p+72,input.size()-72,p,72,p+44,reinterpret_cast<const unsigned char*>(key.constData())))return {};
    auto root=QJsonDocument::fromJson(plain).object();mutate(root);plain=QJsonDocument(root).toJson(QJsonDocument::Compact);auto output=input.left(72);output.resize(88+plain.size());auto* header=reinterpret_cast<unsigned char*>(output.data());qToLittleEndian<quint32>(quint32(plain.size()+16),header+68);
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(header+72,&size,reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),header,72,nullptr,header+44,reinterpret_cast<const unsigned char*>(key.constData())))return {};return output;
}
}
class RecordTransferTests final:public QObject {
    Q_OBJECT
  private slots:
    void twoProfilesRoundTripReopenAndNoFinancialStateChanges(){
        QTemporaryDir source,target;Vault sourceVault(source.filePath("credentials.keefi-vault")),targetVault(target.filePath("credentials.keefi-vault"));QVERIFY(sourceVault.create(Password));QVERIFY(targetVault.create(Password));
        AccountRecord account;account.provider="upbit";account.environment="live";account.market="spot";account.currency="KRW";account.alias="FAKE_ALIAS_MUST_NOT_EXPORT";
        QVERIFY(sourceVault.addAccount(account,{{"access_key","FAKE_FINANCIAL_KEY_MUST_NOT_EXPORT"},{"secret","FAKE_SECRET_MUST_NOT_EXPORT"}},StorageMode::EncryptedVault));
        QVERIFY(targetVault.addAccount(account,{{"access_key","FAKE_TARGET_KEY"},{"secret","FAKE_TARGET_SECRET"}},StorageMode::EncryptedVault));
        QVERIFY(targetVault.setPendingOperations({pending()}));
        AssetsLedger targetAssets(&targetVault,target.filePath("assets.keefi"));QVERIFY(targetAssets.open(true));QVERIFY(targetAssets.append({snapshot("target-existing")},{},{}));
        SwapJournal targetJournal(&targetVault,target.filePath("swaps.keefi"));QVERIFY(targetJournal.open(true));QVERIFY(targetJournal.upsert(swap()));
        const auto targetAssetBytes=read(targetAssets.filePath()),targetJournalBytes=read(target.filePath("swaps.keefi"));
        AssetsLedger history(&sourceVault,source.filePath("assets.keefi"));QVERIFY(history.open(true));QVERIFY(history.appendBatch({snapshot()},{assetEvent()},{},{order()}));
        QVERIFY(QFile::copy(history.filePath(),source.filePath("assets.keefi.archive-fixture.keefi")));QVERIFY(history.append({snapshot("two")},{},{}));
        AssetsLedger latest(&sourceVault,source.filePath("assets-current.keefi"));QVERIFY(latest.open(true));QVERIFY(latest.replaceLatest({snapshot("latest")},{}));
        SwapJournal journal(&sourceVault,source.filePath("swaps.keefi"));QVERIFY(journal.open(true));QVERIFY(journal.upsert(swap()));QVERIFY(sourceVault.setPendingOperations({pending()}));
        RecordTransfer from(sourceVault),to(targetVault);QVERIFY(from.initialize());QVERIFY(to.initialize());RecordTransferBundle collected;QString error;
        QVERIFY2(from.collect({},&history,&journal,&collected,&error,&latest),qPrintable(error));QCOMPARE(collected.records.size(),7);QVERIFY(!collected.sourceSummary.isEmpty());
        const auto json=QJsonDocument(collected.records).toJson(QJsonDocument::Compact);
        for(const auto& forbidden:QList<QByteArray>{"FAKE_FINANCIAL_KEY_MUST_NOT_EXPORT","FAKE_SECRET_MUST_NOT_EXPORT","FAKE_ALIAS_MUST_NOT_EXPORT",Fingerprint.toUtf8(),Binding.toUtf8(),"connectionBinding","fingerprint","credentialRef","phantomAccountId","jupiterAccountId","transactionBytes","quotes"})QVERIFY2(!json.contains(forbidden),forbidden.constData());
        QVERIFY(json.contains("FAKE_OFFICIAL_ORDER"));QVERIFY(json.contains("FAKE_OFFICIAL_FILL"));QVERIFY(json.contains("submitted_unknown"));QVERIFY(json.contains("FAKE_ORIGINAL_CLIENT"));
        auto packagePassword=SecureBuffer::fromUtf8(PackagePassword);QByteArray encoded,other;QVERIFY2(RecordTransfer::encodePackage(collected,packagePassword,&encoded,&error),qPrintable(error));QVERIFY(RecordTransfer::encodePackage(collected,packagePassword,&other));QVERIFY(encoded!=other);
        const auto packagePath=source.filePath("portable.keefi-records");QVERIFY(RecordTransfer::writePackageFile(packagePath,encoded));QByteArray loaded;QVERIFY(RecordTransfer::readPackageFile(packagePath,&loaded));QCOMPARE(loaded,encoded);QVERIFY(!RecordTransfer::writePackageFile(packagePath,other));
        RecordTransferBundle decoded;QVERIFY(RecordTransfer::decodePackage(loaded,packagePassword,&decoded));QCOMPARE(decoded.records,collected.records);
        const auto vaultBefore=read(targetVault.filePath());const auto sourcePending=sourceVault.pendingOperations();const auto sourceJournal=journal.records();QSignalSpy accountChanges(&targetVault,&Vault::accountsChanged);RecordTransferResult receipt;
        QVERIFY2(to.commit(decoded,to.generation(),&receipt,&error),qPrintable(error));QCOMPARE(receipt.added,7);QCOMPARE(read(targetVault.filePath()),vaultBefore);QCOMPARE(targetVault.accounts().size(),1);QCOMPARE(targetVault.pendingOperations(),QJsonArray{pending()});QCOMPARE(accountChanges.size(),0);QCOMPARE(read(targetAssets.filePath()),targetAssetBytes);QCOMPARE(read(target.filePath("swaps.keefi")),targetJournalBytes);QCOMPARE(sourceVault.pendingOperations(),sourcePending);QCOMPARE(journal.records(),sourceJournal);
        const auto before=read(to.filePath());QVERIFY(to.commit(decoded,to.generation(),&receipt));QCOMPARE(receipt.added,0);QCOMPARE(receipt.duplicates,7);QCOMPARE(read(to.filePath()),before);
        targetVault.lock();QVERIFY(to.records().isEmpty());QVERIFY(targetVault.unlock(Password));QVERIFY(to.open());QCOMPARE(to.records(),decoded.records);
        RecordTransferBundle reexport;QVERIFY(to.collect({},&targetAssets,&targetJournal,&reexport));for(const auto& original:decoded.records)QVERIFY(reexport.records.contains(original)); // original origin survives another profile
    }
    void conflictRejectsEntireImportAndPreservesCiphertext(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));QVERIFY(transfer.commit(data,transfer.generation()));const auto before=read(transfer.filePath());
        auto row=data.records[0].toObject(),payload=row["payload"].toObject();auto components=payload["components"].toArray();auto part=components[0].toObject();part["quantity"]="999";components[0]=part;payload["components"]=components;row["payload"]=payload;data.records[0]=row;
        QVERIFY(!transfer.commit(data,transfer.generation(),nullptr,&error));QVERIFY(error.contains("다른 내용"));QCOMPARE(read(transfer.filePath()),before);QCOMPARE(transfer.records().size(),1);
    }
    void laterSwapAndPendingObservationsKeepHistoryAndPastPackagesRemainDuplicates(){
        QTemporaryDir source,target;Vault original(source.filePath("credentials.keefi-vault")),destination(target.filePath("credentials.keefi-vault"));QVERIFY(original.create(Password));QVERIFY(destination.create(Password));
        SwapJournal journal(&original,source.filePath("swaps.keefi"));QVERIFY(journal.open(true));auto signedRecord=swap();QVERIFY(journal.upsert(signedRecord));QVERIFY(original.setPendingOperations({pending()}));
        RecordTransfer from(original),to(destination);QVERIFY(from.initialize());QVERIFY(to.initialize());RecordTransferSelection selection;selection.snapshots=selection.events=selection.orders=false;RecordTransferBundle first,later,final;QString error;RecordTransferResult result;
        QVERIFY(from.collect(selection,nullptr,&journal,&first));QCOMPARE(first.records.size(),2);QVERIFY(to.commit(first,to.generation(),&result));QCOMPARE(result.added,2);
        auto unknown=pending();unknown["state"]="unknown";unknown["orderId"]="FAKE_RECEIVED_ORDER";QVERIFY(original.setPendingOperations({unknown}));signedRecord.state="confirmed";signedRecord.confirmedSlot="200";signedRecord.actualFeeLamports="5000";QVERIFY(journal.upsert(signedRecord));
        QVERIFY(from.collect(selection,nullptr,&journal,&later));QVERIFY2(to.commit(later,to.generation(),&result,&error),qPrintable(error));QCOMPARE(result.added,2);QCOMPARE(result.total,4);
        signedRecord.state="finalized";signedRecord.actualOutputAtomic="501";QVERIFY(journal.upsert(signedRecord));QVERIFY(from.collect(selection,nullptr,&journal,&final));QVERIFY(to.commit(final,to.generation(),&result));QCOMPARE(result.added,1);QCOMPARE(result.duplicates,1);QCOMPARE(result.total,5);
        const auto bytes=read(to.filePath());QVERIFY(to.commit(first,to.generation(),&result));QCOMPARE(result.added,0);QCOMPARE(result.duplicates,2);QCOMPARE(read(to.filePath()),bytes);
        RecordTransferBundle conflict=final;for(qsizetype i=0;i<conflict.records.size();++i){auto row=conflict.records[i].toObject();if(row["kind"]=="swap"){auto p=row["payload"].toObject();p["inputAtomic"]="999";row["payload"]=p;conflict.records[i]=row;}}
        QVERIFY(!to.commit(conflict,to.generation(),nullptr,&error));QVERIFY(error.contains("내용"));QCOMPARE(read(to.filePath()),bytes);
        // Two contradictory known fees get separate revisions, yet the request group rejects them.
        auto contradictory=final;for(qsizetype i=0;i<contradictory.records.size();++i){auto row=contradictory.records[i].toObject();if(row["kind"]=="swap"){auto p=row["payload"].toObject();p["actualFeeLamports"]="6000";QJsonObject observation;for(const auto* name:{"state","confirmedSlot","actualFeeLamports","actualInputAtomic","actualOutputAtomic"})observation[name]=p[name];const auto revision=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(observation).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());row["identity"]=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{p["id"],revision}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());row["payload"]=p;contradictory.records[i]=row;}}
        QVERIFY(!to.commit(contradictory,to.generation(),nullptr,&error));QCOMPARE(read(to.filePath()),bytes);
    }
    void badPasswordsHeadersTruncationAndAuthenticatedUnknownFieldsNeverCommit(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));auto password=SecureBuffer::fromUtf8(PackagePassword);QByteArray encoded;QVERIFY(RecordTransfer::encodePackage(data,password,&encoded));const auto before=read(transfer.filePath()),vaultBefore=read(vault.filePath());RecordTransferBundle decoded;
        auto wrong=SecureBuffer::fromUtf8("wrong");QVERIFY(!RecordTransfer::decodePackage(encoded,wrong,&decoded));
        auto bad=encoded;bad.back()=char(bad.back()^1);QVERIFY(!RecordTransfer::decodePackage(bad,password,&decoded));bad=encoded;bad[8]=char(2);QVERIFY(!RecordTransfer::decodePackage(bad,password,&decoded));bad=encoded;qToLittleEndian<quint32>(0xffffffffU,reinterpret_cast<unsigned char*>(bad.data())+16);QVERIFY(!RecordTransfer::decodePackage(bad,password,&decoded,&error));QVERIFY(error.contains("KDF"));QVERIFY(!RecordTransfer::decodePackage(encoded.left(70),password,&decoded));QVERIFY(!RecordTransfer::decodePackage(encoded.chopped(1),password,&decoded));
        bad=rewriteFake(encoded,[](QJsonObject& root){auto rows=root["records"].toArray();auto row=rows[0].toObject(),payload=row["payload"].toObject();payload["connectionBinding"]=Binding;row["payload"]=payload;rows[0]=row;root["records"]=rows;});QVERIFY(!bad.isEmpty());QVERIFY(!RecordTransfer::decodePackage(bad,password,&decoded));
        bad=rewriteFake(encoded,[](QJsonObject& root){root["schema_version"]=2;});QVERIFY(!RecordTransfer::decodePackage(bad,password,&decoded));
        bad=rewriteFake(encoded,[](QJsonObject& root){auto rows=root["records"].toArray();rows.append(rows[0]);root["records"]=rows;});QVERIFY(!RecordTransfer::decodePackage(bad,password,&decoded));
        auto weak=SecureBuffer::fromUtf8("legacy");QVERIFY(!RecordTransfer::encodePackage(data,weak,&bad));QCOMPARE(read(transfer.filePath()),before);QCOMPARE(read(vault.filePath()),vaultBefore);
    }
    void explicitPreparationLockCancelAndPrecommitAbortPreserveExistingBytes(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);const auto unprepared=read(vault.filePath());RecordTransferBundle empty;QVERIFY(!transfer.commit(empty,transfer.generation()));QCOMPARE(read(vault.filePath()),unprepared);QVERIFY(!transfer.exists());
        RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));const auto before=read(transfer.filePath());const auto cancel=std::make_shared<std::atomic_bool>(true);
        QVERIFY(!transfer.commit(data,transfer.generation(),nullptr,&error,cancel));QCOMPARE(read(transfer.filePath()),before);cancel->store(false);
        QVERIFY(!transfer.commit(data,transfer.generation(),nullptr,&error,cancel,[&]{cancel->store(true);return true;}));QCOMPARE(read(transfer.filePath()),before);cancel->store(false);QVERIFY(transfer.open());
        QVERIFY(!transfer.commit(data,transfer.generation(),nullptr,&error,{},[]{return false;}));QCOMPARE(read(transfer.filePath()),before);QVERIFY(transfer.open());
        const auto generation=transfer.generation();QVERIFY(!transfer.commit(data,generation,nullptr,&error,{},[&]{vault.lock();return true;}));QCOMPARE(read(transfer.filePath()),before);QVERIFY(transfer.records().isEmpty());QVERIFY(vault.unlock(Password));QVERIFY(transfer.open());QVERIFY(!transfer.commit(data,generation));QCOMPARE(read(transfer.filePath()),before);
    }
    void digestRaceAndConcurrentStoreFailClosed(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));RecordTransfer concurrent(vault);QVERIFY(!concurrent.open());const auto original=read(transfer.filePath());auto changed=original;changed.back()=char(changed.back()^1);
        QVERIFY(!transfer.commit(data,transfer.generation(),nullptr,&error,{},[&]{return write(transfer.filePath(),changed);}));QCOMPARE(read(transfer.filePath()),changed);QVERIFY(!transfer.isOpen());QVERIFY(write(transfer.filePath(),original));QVERIFY(transfer.open());
        QVERIFY(write(transfer.filePath(),changed));QVERIFY(!transfer.commit(data,transfer.generation()));QCOMPARE(read(transfer.filePath()),changed);QVERIFY(!transfer.open());
    }
    void corruptArchiveAndBusyCheckpointNeverSilentlyOmitRecords(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));
        AssetsLedger history(&vault,dir.filePath("assets.keefi"));QVERIFY(history.open(false));QVERIFY(QFile::copy(history.filePath(),dir.filePath("assets.keefi.archive-corrupt.keefi")));auto bad=read(dir.filePath("assets.keefi.archive-corrupt.keefi"));bad.back()=char(bad.back()^1);QVERIFY(write(dir.filePath("assets.keefi.archive-corrupt.keefi"),bad));const auto before=read(transfer.filePath());QVERIFY(!transfer.collect({},&history,nullptr,&data,&error));QCOMPARE(read(transfer.filePath()),before);
        QVERIFY(QFile::remove(dir.filePath("assets.keefi.archive-corrupt.keefi")));QVERIFY(history.appendWithArchive({snapshot("pending")},{},{},{},&error,true));QVERIFY(history.isWriting());QVERIFY(!transfer.collect({},&history,nullptr,&data,&error));QTRY_VERIFY_WITH_TIMEOUT(!history.isWriting(),5000);
    }
    void processInterruptionBeforeReplaceKeepsCommittedCipherAndCanReopen(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));
        const auto before=read(transfer.filePath()),vaultBefore=read(vault.filePath());vault.lock(); // no parent owner locks while the isolated fake child runs
        QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--record-transfer-interrupt-child",dir.path()});QVERIFY2(child.waitForStarted(5000),qPrintable(child.errorString()));QVERIFY(child.waitForFinished(30000));QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),73);
        QCOMPARE(read(transfer.filePath()),before);QCOMPARE(read(vault.filePath()),vaultBefore);
        const auto leftovers=QDir(dir.path()).entryList({".keefi-record-transfer-*"},QDir::Files|QDir::Hidden,QDir::Name);QVERIFY(!leftovers.isEmpty());
        for(const auto& name:leftovers){const auto path=dir.filePath(name);const auto bytes=read(path);QVERIFY(bytes.startsWith("KEEFIIMP"));QVERIFY(!bytes.contains("schema_version"));QVERIFY(!bytes.contains(Fingerprint.toUtf8()));QVERIFY(!bytes.contains("FAKE_OFFICIAL_SOURCE"));QVERIFY(!bytes.contains("100.01"));
#ifdef Q_OS_UNIX
            QVERIFY(!(QFileInfo(path).permissions()&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ReadOther|QFileDevice::WriteOther)));
#endif
        }
        // Qt checks native/PID liveness before reclaiming the terminated child's locks.
        QVERIFY2(vault.unlock(Password,&error),qPrintable(error));QVERIFY2(transfer.open(&error),qPrintable(error));QCOMPARE(transfer.records().size(),0);QCOMPARE(read(transfer.filePath()),before);QVERIFY2(transfer.commit(data,transfer.generation(),nullptr,&error),qPrintable(error));QCOMPARE(transfer.records().size(),1);
        // Orphan ciphertext is ignored, never replayed as an import or active request.
        for(const auto& name:leftovers)QVERIFY(QFile::remove(dir.filePath(name)));
    }
    void fullNativeSnapshotKeepsEveryComponentAndPortableFoldersKeepPermissions(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));AssetsLedger history(&vault,dir.filePath("assets.keefi"));QVERIFY(history.open(true));auto large=snapshot("full-native-snapshot");large.components.clear();
        for(int i=0;i<2048;++i){AssetComponent c;c.id=QString("token-%1").arg(i);c.asset=QString("FAKE_TOKEN_%1").arg(i);c.quantity=i==2047?"0.00000001":"0";c.evidence=evidence();large.components.append(c);}QVERIFY(history.append({large},{},{}));
        RecordTransfer transfer(vault);QVERIFY(transfer.initialize());RecordTransferBundle data,decoded;QString error;RecordTransferSelection selection;selection.events=selection.orders=selection.swaps=selection.pending=false;
        QVERIFY2(transfer.collect(selection,&history,nullptr,&data,&error),qPrintable(error));QCOMPARE(data.records.size(),1);const auto payload=data.records[0].toObject()["payload"].toObject();QCOMPARE(payload["components"].toArray().size(),2048);QVERIFY(QJsonDocument(data.records[0].toObject()).toJson(QJsonDocument::Compact).size()>RecordTransfer::MaxRecordBytes);
        auto password=SecureBuffer::fromUtf8(PackagePassword);QByteArray encoded;QVERIFY2(RecordTransfer::encodePackage(data,password,&encoded,&error),qPrintable(error));QVERIFY(RecordTransfer::decodePackage(encoded,password,&decoded));QCOMPARE(decoded.records,data.records);
        const auto folder=dir.filePath("portable-destination");QVERIFY(QDir().mkdir(folder));
#ifdef Q_OS_UNIX
        const auto publicMode=QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther;QVERIFY(QFile::setPermissions(folder,publicMode));const auto beforeMode=QFileInfo(folder).permissions();
#endif
        const auto path=QDir(folder).filePath("records.keefi-records");QVERIFY2(RecordTransfer::writePackageFile(path,encoded,&error),qPrintable(error));QByteArray readback;QVERIFY(RecordTransfer::readPackageFile(path,&readback));QCOMPARE(readback,encoded);
#ifdef Q_OS_UNIX
        QCOMPARE(QFileInfo(folder).permissions(),beforeMode);QVERIFY(!(QFileInfo(path).permissions()&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ReadOther|QFileDevice::WriteOther)));
        QVERIFY(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadGroup|QFileDevice::ReadOther));QVERIFY(RecordTransfer::readPackageFile(path,&readback)); // copied portable ciphertext may have inherited mode 0644
        transfer.lock();const auto storeBytes=read(transfer.filePath());QVERIFY(QFile::setPermissions(transfer.filePath(),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadGroup));QVERIFY(!transfer.open());QCOMPARE(read(transfer.filePath()),storeBytes); // local store remains strict
#endif
    }
    void workerCodecUsesOnlyImmutableSnapshotsAndSafeFilePaths(){
        QTemporaryDir dir;Vault vault(dir.filePath("credentials.keefi-vault"));QVERIFY(vault.create(Password));RecordTransfer transfer(vault);RecordTransferBundle data;QString error;QVERIFY(collectOne(vault,transfer,&data,&error));bool okay=false;QByteArray encoded;RecordTransferBundle decoded;
        std::thread worker([&]{auto password=SecureBuffer::fromUtf8(PackagePassword);okay=RecordTransfer::encodePackage(data,password,&encoded,&error)&&RecordTransfer::decodePackage(encoded,password,&decoded,&error);});worker.join();QVERIFY2(okay,qPrintable(error));QCOMPARE(decoded.records,data.records);
        const auto path=dir.filePath("package.keefi-records");QVERIFY(RecordTransfer::writePackageFile(path,encoded));
#ifdef Q_OS_UNIX
        const auto symlink=dir.filePath("symlink.keefi-records"),hardlink=dir.filePath("hardlink.keefi-records");QVERIFY(::symlink(QFile::encodeName(path).constData(),QFile::encodeName(symlink).constData())==0);QByteArray readback;QVERIFY(!RecordTransfer::readPackageFile(symlink,&readback));QVERIFY(!RecordTransfer::writePackageFile(symlink,encoded));QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(hardlink).constData())==0);QVERIFY(!RecordTransfer::readPackageFile(hardlink,&readback));QVERIFY(!RecordTransfer::readPackageFile(path,&readback));
#endif
    }
};
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--record-transfer-interrupt-child"){
        const QString directory=QString::fromLocal8Bit(argv[2]);keefi::Vault vault(QDir(directory).filePath("credentials.keefi-vault"));QString error;
        if(!vault.unlock(keefi::Password,&error))return 90;keefi::RecordTransfer transfer(vault);if(!transfer.open(&error))return 91;
        keefi::RecordTransferSelection selection;selection.events=selection.orders=selection.swaps=selection.pending=false;keefi::RecordTransferBundle data;
        if(!transfer.collect(selection,nullptr,nullptr,&data,&error))return 92;
        transfer.commit(data,transfer.generation(),nullptr,&error,{},[]{std::_Exit(73);return false;});return 93;
    }
    keefi::RecordTransferTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "tst_record_transfer.moc"
