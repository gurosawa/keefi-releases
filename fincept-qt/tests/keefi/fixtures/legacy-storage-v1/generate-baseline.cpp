// Immutable synthetic baseline generator. Compile only against the recorded pre-rename libraries.
// Every credential, account, amount and timestamp below is invented; no network/UI/profile lookup.
#include "finix/Vault.h"
#include "finix/AssetsLedger.h"
#include "finix/SwapJournal.h"
#include "finix/PriceArchive.h"
#include "finix/RecordTransfer.h"
#include "finix/SolanaTransactionCodec.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtEndian>
#include <sodium.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace finix;
namespace {
const QString Password="Synthetic legacy Storage password 2026!";
const QString PackagePassword="Synthetic portable Storage password 2026!";
const QString Scope(64,'a'),Binding(64,'b');
constexpr qint64 At=1792200000000LL,Hour=3600000;
void require(bool ok,const QString& why){if(!ok)throw std::runtime_error(why.toStdString());}
QByteArray read(const QString& path){QFile f(path);require(f.open(QIODevice::ReadOnly),"read "+path);return f.readAll();}
QString sha(const QByteArray& b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
void write(const QString& path,const QByteArray& b){
    QFile f(path);require(f.open(QIODevice::WriteOnly|QIODevice::NewOnly),"create "+path);
    require(f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner),"mode "+path);
    require(f.write(b)==b.size()&&f.flush(),"write "+path);
}
void mkdir(const QString& path){
    require(!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink(),"destination exists: "+path);
    require(QDir().mkpath(path)&&QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner),"mkdir "+path);
}
AssetEvidence evidence(){return {"SYNTHETIC_OFFICIAL_SOURCE","https://example.invalid/official",Scope,At,At,true};}
AssetSnapshot snapshot(const QString& id){
    AssetSnapshot s;s.id=id;s.accountScope=Scope;s.connectionBinding=Binding;s.connectionId="synthetic-local-registration";
    s.provider="binance";s.environment="live";s.market="spot";s.timeMs=At;s.balanceComplete=true;s.reconciled=true;
    s.coverage="SYNTHETIC_COVERAGE";s.evidence=evidence();
    AssetComponent c;c.id="cash";c.asset="KRW";c.quantity="100.01";c.kind=AssetComponentKind::Cash;c.evidence=evidence();s.components={c};return s;
}
AssetEvent event(){AssetEvent e;e.id="local:"+Scope;e.accountScope=Scope;e.asset="KRW";e.quantity="50";
    e.officialId="SYNTHETIC_FILL";e.timeMs=At;e.kind=AssetEventKind::Fill;e.evidence=evidence();return e;}
AssetOrderObservation order(){AssetOrderObservation o;o.accountScope=Scope;o.provider="binance";o.market="spot";
    o.orderId="SYNTHETIC_ORDER";o.observedAtMs=At;o.side="buy";o.type="limit";o.status="partial";o.price="1.25";
    o.quantity="3";o.executed="1";o.quoteAmount="1.25";o.quantityUnit="base";o.currency="KRW";o.evidence=evidence();return o;}
QJsonObject pending(){return {{"id","synthetic-pending"},{"accountId","synthetic-registration"},{"fingerprint",Scope},
    {"provider","binance"},{"environment","live"},{"symbol","BTCUSDT"},{"kind","place"},{"clientId","SYNTHETIC_CLIENT"},
    {"orderId",""},{"replacementClientId",""},{"state","pending"},{"createdAt",QString::number(At)},
    {"quantity","1"},{"price","1.25"},{"quoteAmount",""},{"side","buy"},{"type","limit"}};}
SwapPendingRecord swap(){
    SwapPendingRecord r;r.id="00000000-0000-4000-8000-000000000001";r.phantomAccountId="00000000-0000-4000-8000-000000000002";
    r.jupiterAccountId="00000000-0000-4000-8000-000000000003";r.credentialRef="00000000-0000-4000-8000-000000000004";
    r.chain="solana-mainnet";r.wallet=SolanaTransactionCodec::encodeBase58(QByteArray(32,'x'));r.inputMint=SolMint;r.outputMint=UsdcMint;
    r.inputAtomic="1000000";r.minimumRouteOutput="500";r.slippageBps="50";r.requestId="SYNTHETIC_REQUEST";
    r.transactionId=SolanaTransactionCodec::encodeBase58(QByteArray(64,'z'));r.messageHash=QString(64,'c');
    r.lastValidBlockHeight="100";r.createdAt=QString::number(At);r.state="confirmed";r.confirmedSlot="200";r.actualFeeLamports="5000";return r;
}
MarketCandleSeries candles(const QString& close){
    constexpr qint64 CandleNow=1792598400000LL;
    MarketCandleSeries s;s.available=true;s.identity="coin:upbit:KRW-BTC";s.intervalMs=Hour;s.currency="KRW";s.asset="BTC";
    s.sourceUrl=MarketCandles::request(s.identity,Hour).url.toString();s.receivedAtMs=s.restReceivedAtMs=CandleNow+60000;
    for(const auto start:{CandleNow-2*Hour,CandleNow-Hour}){MarketCandle c;c.startsAtMs=start;c.lastStoredTickMs=start+1000;
        c.open="12000";c.high="13000";c.low="11000";c.close=close;c.volume="2.5";s.candles.append(c);}
    s.restThroughMs=s.candles.last().startsAtMs;return s;
}
// Exact public legacy exit-notice codec, independent of the final product implementation.
QByteArray exitNotice(Vault& vault){
    SecureBuffer key(32);require(vault.withAssetsLedgerKey(false,[&](const SecureBuffer& root){
        const QByteArray domain("FINIX/price-archive/v1");
        require(!crypto_generichash(key.data(),key.size(),reinterpret_cast<const unsigned char*>(domain.constData()),domain.size(),root.data(),root.size()),"exit key");
    }),"ledger root");
    auto plain=QJsonDocument(QJsonObject{{"purpose","price-archive-exit/1"},{"reason","timeout"}}).toJson(QJsonDocument::Compact);
    plain.append(QByteArray(256-plain.size(),'\0'));
    QByteArray bytes(312,'\0');auto* p=reinterpret_cast<unsigned char*>(bytes.data());std::memcpy(p,"FINIXPRC",8);
    qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(272,p+36);
    unsigned long long size=0;
    require(!crypto_aead_xchacha20poly1305_ietf_encrypt(p+40,&size,reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),p,40,nullptr,p+12,key.data())&&size==272,"exit encryption");
    sodium_memzero(plain.data(),plain.size());return bytes;
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("SyntheticStorageBaseline");
    try{
        require(argc==2,"one fresh output directory is required");
        const QString root=QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();mkdir(root);
        const auto profile=QDir(root).filePath("profile");mkdir(profile);
        QJsonObject expected{{"synthetic",true},{"password",Password},{"packagePassword",PackagePassword},
            {"snapshotCount",2},{"latestSnapshotCount",1},{"eventCount",1},{"orderCount",1},{"swapCount",1},{"priceCount",2},{"priceConflicts",2}};
        QString error;
        {
            Vault vault(QDir(profile).filePath("credentials.finix-vault"));require(vault.create(Password,&error),error);
            AccountRecord account;account.id="00000000-0000-4000-8000-000000000099";account.provider="upbit";account.environment="live";
            account.market="spot";account.currency="KRW";account.alias="SYNTHETIC LEGACY ACCOUNT";
            require(vault.addAccount(account,{{"access_key","SYNTHETIC_KEY_ONLY"},{"secret","SYNTHETIC_SECRET_ONLY"}},StorageMode::EncryptedVault,&error),error);
            require(vault.setPendingOperations({pending()},&error),error);
            AssetsLedger history(&vault,QDir(profile).filePath("assets.finix"));require(history.open(true,&error),error);
            require(history.appendBatch({snapshot("one")},{event()},{},{order()},&error),error);
            const auto archive=QDir(profile).filePath("assets.finix.archive-11111111111111111111111111111111.finix");
            require(QFile::copy(history.filePath(),archive),"archive copy");
            require(history.append({snapshot("two")},{},{},&error),error);
            const QJsonObject baseline{{"kind","coin-baseline-v1"},{"time",QString::number(At)},{"netAssets","100.01"},{"policy","synthetic-v1"},
                {"connections",QJsonArray{QJsonObject{{"id","synthetic-local-registration"},{"binding",Binding},{"snapshot",QString::number(At)}}}},
                {"scopes",QJsonArray{Scope}}};
            QEventLoop loop;bool written=false,finished=false;
            QObject::connect(&history,&AssetsLedger::collectionWriteFinished,&loop,[&](quint64 receipt,bool ok,const QString& message){
                if(receipt==71){written=ok;finished=true;error=message;loop.quit();}});
            require(history.saveCoinBaseline(baseline,71,&error)==AssetsLedger::CollectionWrite::Pending,error);
            QTimer::singleShot(10000,&loop,&QEventLoop::quit);loop.exec();require(finished&&written,"schema3: "+error);
            expected["coinBaseline"]=baseline;
            AssetsLedger latest(&vault,QDir(profile).filePath("assets-current.finix"));require(latest.open(true,&error),error);
            require(latest.replaceLatest({snapshot("latest")},{},&error),error);
            SwapJournal swaps(&vault,QDir(profile).filePath("swaps.finix"));require(swaps.open(true,&error)&&swaps.upsert(swap(),&error),error);
            PriceArchive prices(&vault,QDir(profile).filePath("prices.finix"));require(prices.open(true,&error),error);
            require(prices.record(candles("12345.678"),&error).added==2,error);
            require(prices.record(candles("12346.678"),&error).conflicts==2,error);require(prices.flush(&error),error);prices.lock();
            write(QDir(profile).filePath("prices.finix.exit"),exitNotice(vault));
            RecordTransfer transfer(vault);require(transfer.initialize(&error),error);RecordTransferBundle collected;
            require(transfer.collect({},&history,&swaps,&collected,&error,&latest),error);
            RecordTransferResult committed;require(transfer.commit(collected,transfer.generation(),&committed,&error),error);
            require(committed.added==collected.records.size()&&committed.added==7,"expected seven synthetic portable records");
            expected["records"]=collected.records;expected["pendingOperations"]=vault.pendingOperations();
            QByteArray package;auto pass=SecureBuffer::fromUtf8(PackagePassword);
            require(RecordTransfer::encodePackage(collected,pass,&package,&error),error);
            require(RecordTransfer::writePackageFile(QDir(root).filePath("portable.finixrtx"),package,&error),error);
            require(vault.withAssetsLedgerKey(false,[&](const SecureBuffer& key){expected["ledgerRootSha256"]=sha(QByteArray(reinterpret_cast<const char*>(key.data()),qsizetype(key.size())));}),"root hash");
            vault.lock();
        }
        QJsonObject manifest;
        QDirIterator it(root,QDir::Files|QDir::Hidden,QDirIterator::Subdirectories);
        while(it.hasNext()){const auto path=it.next();require(!path.endsWith(".lock"),"live fixture lock remained");const auto bytes=read(path);
            manifest[QDir(root).relativeFilePath(path)]=QJsonObject{{"sha256",sha(bytes)},{"bytes",double(bytes.size())},{"magic",QString::fromLatin1(bytes.left(8))}};}
        expected["files"]=manifest;write(QDir(root).filePath("expected.json"),QJsonDocument(expected).toJson(QJsonDocument::Indented));
        std::cout<<"synthetic legacy fixture: "<<manifest.size()<<" files; "<<root.toStdString()<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

