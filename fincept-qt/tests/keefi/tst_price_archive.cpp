#include "keefi/PriceArchive.h"
#include "keefi/AssetsLedger.h"
#include "keefi/MarketCandles.h"
#include "keefi/SecureBuffer.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <sodium.h>
#include <cstring>
#include <atomic>
namespace keefi {
namespace {
const QString Password="Fake price Vault password 2026!",OtherPassword="Other fake Price password 2026!";
constexpr qint64 Now=1792598400000,Minute=60000,Quarter=900000,Hour=3600000,Day=86400000,Midnight=Now-Now%Day;
// The archive never creates the vault's ledger key; a ledger opened for writing does.
bool keyedVault(Vault& vault,const QTemporaryDir& dir){
    if(!vault.create(Password))return false;AssetsLedger ledger(&vault,dir.filePath("assets.bin"));const bool ok=ledger.open(true);ledger.lock();return ok;
}
MarketCandleSeries series(const QList<qint64>& starts,qint64 received=Now+Minute,const QString& identity="coin:upbit:KRW-BTC",qint64 interval=Hour,
    const QString& close="12345.678"){
    MarketCandleSeries s;s.available=true;s.identity=identity;s.intervalMs=interval;
    const auto market=identity.section(':',-1);s.currency=market.section('-',0,0);s.asset=market.section('-',1);
    s.sourceUrl=MarketCandles::request(identity,interval).url.toString();s.receivedAtMs=s.restReceivedAtMs=received;
    for(const auto start:starts){MarketCandle c;c.startsAtMs=start;c.lastStoredTickMs=start+1000;c.open="12000";c.high="13000";c.low="11000";c.close=close;c.volume="2.5";s.candles.append(c);}
    if(!starts.isEmpty())s.restThroughMs=starts.last();
    return s;
}
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
bool write(const QString& path,const QByteArray& bytes){
    QFile f(path);return f.open(QIODevice::WriteOnly|QIODevice::Truncate)&&f.write(bytes)==bytes.size()&&f.flush()&&f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
}
QStringList setAside(const QTemporaryDir& dir){return QDir(dir.path()).entryList({"prices.keefi.unreadable-*"},QDir::Files);}
QByteArray derivedKey(Vault& vault,const char* domain){
    QByteArray key(32,'\0');
    vault.withAssetsLedgerKey(false,[&](const SecureBuffer& ledger){crypto_generichash(reinterpret_cast<unsigned char*>(key.data()),32,
        reinterpret_cast<const unsigned char*>(domain),std::strlen(domain),ledger.data(),ledger.size());});
    return key;
}
// The documented container, built independently of PriceArchive.
QByteArray seal(QByteArray plain,const QByteArray& key){
    plain.append(QByteArray((plain.size()+PriceArchive::PaddingBytes-1)/PriceArchive::PaddingBytes*PriceArchive::PaddingBytes-plain.size(),'\0'));
    QByteArray out(40+plain.size()+16,'\0');auto* p=reinterpret_cast<unsigned char*>(out.data());std::memcpy(p,"KEEFIPRC",8);
    qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+36);unsigned long long size=0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(p+40,&size,reinterpret_cast<const unsigned char*>(plain.constData()),std::size_t(plain.size()),p,40,nullptr,p+12,
        reinterpret_cast<const unsigned char*>(key.constData()));
    return out;
}
QJsonObject storedSeries(const QString& identity,const QString& source,const QString& asset){
    const QJsonArray row{QString::number(Now-Hour),QString::number(Now-Hour+1000),"12000","13000","11000","12345.678","2.5",QString::number(Now+Minute)};
    QJsonArray candles;candles.append(row); // QJsonArray{row} would copy row instead of nesting it.
    return {{"identity",identity},{"interval_ms",QString::number(Hour)},{"asset",asset},{"currency","KRW"},{"source",source},
        {"updated_at_ms",QString::number(Now+Minute)},{"last_viewed_at_ms",QString::number(Now+Minute)},{"conflict_overflow","0"},
        {"candles",candles},{"conflicts",QJsonArray{}}};
}
QJsonObject upbitQuarter(qint64 start,const QString& close="105"){
    return {{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(start,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(start+1000)},{"opening_price","100"},{"high_price","110"},{"low_price","90"},
        {"trade_price",close},{"candle_acc_trade_volume","2"},{"unit","15"}};
}
HttpResult quarters(const QString& lastClose="105"){
    HttpResult r;r.status=200;r.body=QJsonDocument(QJsonArray{upbitQuarter(Now-Quarter,lastClose),upbitQuarter(Now-2*Quarter),upbitQuarter(Now-3*Quarter)}).toJson();return r;
}
class CandleHttp final:public TradingTransport {
  public:
    QList<quint64> tokens;
    void send(quint64 token,const RequestSpec&)override{tokens.append(token);}
    void abort()override{}
};
class ArchiveSocket final:public PublicCandleSocket {
  public:
    quint64 generation=0;
    void open(quint64 value,const QUrl&)override{generation=value;}
    void send(quint64,const QByteArray&)override{}
    void abort()override{}
};
struct WorkerGate {
    struct State { QSemaphore entered,release; };
    std::shared_ptr<State> state=std::make_shared<State>();bool opened=false;
    std::function<void()> hook()const{return [held=state]{held->entered.release();held->release.acquire();};}
    void open(){if(!opened){opened=true;state->release.release();}}
    ~WorkerGate(){open();}
};
QString exitReason(const QString& path,Vault& vault){
    const auto bytes=read(path);if(bytes.size()!=312)return {};
    const auto key=derivedKey(vault,"KEEFI/price-archive/v1");
    QByteArray plain(256,'\0');unsigned long long size=0;
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()),&size,nullptr,p+40,bytes.size()-40,p,40,p+12,
        reinterpret_cast<const unsigned char*>(key.constData())))return {};
    while(!plain.isEmpty()&&plain.back()=='\0')plain.chop(1);
    return QJsonDocument::fromJson(plain).object().value("reason").toString();
}
}
class PriceArchiveTests final:public QObject {
    Q_OBJECT
  private slots:
    void quitDeadlineBoundsTheWholeSyntheticProcess(){
        const auto childMode=qEnvironmentVariable("KEEFI_PRICE_QUIT_CHILD");
        if(!childMode.isEmpty()){
            QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
            PriceArchive archive(&vault,dir.filePath("prices.keefi"));QVERIFY(archive.open(true));
            QVERIFY(archive.record(series({Now-Hour})).accepted);
            auto gate=std::make_shared<QSemaphore>();
            const auto stall=[gate]{gate->acquire();}; // Intentionally never released: process exit must not join this worker.
            if(childMode=="after")archive.setVerifyWorkerForTest(stall);else archive.setPrepareWorkerForTest(stall);
            bool completed=false,saved=true;QElapsedTimer elapsed;elapsed.start();
            archive.prepareToQuit([&](bool ok,const QString&){completed=true;saved=ok;QCoreApplication::quit();});
            QCoreApplication::exec();
            QVERIFY(completed);QVERIFY(!saved);QVERIFY(elapsed.elapsed()<3600);return;
        }
        for(const auto* stage:{"before","after"}){
            QProcess child;auto env=QProcessEnvironment::systemEnvironment();env.insert("KEEFI_PRICE_QUIT_CHILD",QString::fromLatin1(stage));
            child.setProcessEnvironment(env);child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(),{"quitDeadlineBoundsTheWholeSyntheticProcess","-silent"});
            QVERIFY(child.waitForStarted(3000));
            const bool finished=child.waitForFinished(8000);if(!finished){child.kill();child.waitForFinished(1000);}
            const auto output=child.readAll();QVERIFY2(finished,output.constData());
            QCOMPARE(child.exitStatus(),QProcess::NormalExit);QVERIFY2(child.exitCode()==0,output.constData());
        }
    }
    void cleanQuitDoesNotRetryOldFailures_data(){
        QTest::addColumn<QString>("state");
        for(const auto* state:{"writer-lock","missing-key-with-file","previous-write-failure","clean-open"})QTest::newRow(state)<<QString(state);
    }
    void cleanQuitDoesNotRetryOldFailures(){
        QFETCH(QString,state);QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));
        if(state=="missing-key-with-file")QVERIFY(vault.create(Password));else QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);
        QLockFile occupied(path+".lock");occupied.setStaleLockTime(0);
        if(state=="writer-lock")QVERIFY(occupied.tryLock(0));
        if(state=="missing-key-with-file")QVERIFY(write(path,QByteArray(128,'x')));
        if(state=="previous-write-failure"||state=="clean-open"){
            QVERIFY(archive.open(true));
            if(state=="previous-write-failure"){
                auto bytes=read(path);bytes[bytes.size()-1]=char(bytes.back()^1);QVERIFY(write(path,bytes));
                QVERIFY(archive.record(series({Now-Hour})).accepted);QVERIFY(!archive.flush());
            }
        }else{
            CandleHttp http;MarketCandles model(&http,{},[]{return Now+Minute;});archive.watch(&model);
            model.setSelection("coin:upbit:KRW-BTC",Quarter);QVERIFY(!archive.isOpen());
        }
        QVERIFY(!archive.dirty());const auto original=read(path),vaultBefore=read(vault.filePath());
        int saves=0;QSignalSpy writes(&archive,&PriceArchive::written);
        archive.flushAsync([&](bool ok,const QString& error){QVERIFY(ok);QVERIFY(error.isEmpty());++saves;});
        QCOMPARE(saves,1);QElapsedTimer elapsed;elapsed.start();
        archive.prepareToQuit([&](bool ok,const QString& error){QVERIFY(ok);QVERIFY(error.isEmpty());++saves;});
        QCOMPARE(saves,2);QVERIFY(elapsed.elapsed()<500);QCOMPARE(writes.size(),0);
        QCOMPARE(read(path),original);QCOMPARE(read(vault.filePath()),vaultBefore);QVERIFY(!QFileInfo::exists(path+".exit"));
    }
    void quitSaveSuccessLeavesNoFailureNotice(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        {PriceArchive archive(&vault,path);QVERIFY(archive.open(true));QVERIFY(archive.record(series({Now-Hour})).accepted);
            bool done=false,saved=false;archive.prepareToQuit([&](bool ok,const QString&){saved=ok;done=true;});
            QTRY_VERIFY_WITH_TIMEOUT(done,3500);QVERIFY(saved);QVERIFY(!QFileInfo::exists(path+".exit"));}
        PriceArchive reopened(&vault,path);QVERIFY(reopened.open(false));QCOMPARE(reopened.storedCount("coin:upbit:KRW-BTC",Hour),1);
    }
    void quitSaveFailureSurvivesReopenOnlyAsEncryptedInformation(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");QByteArray original;
        {PriceArchive archive(&vault,path);QVERIFY(archive.open(true));original=read(path);QVERIFY(archive.record(series({Now-Hour})).accepted);
            auto changed=original;changed[changed.size()-1]=char(changed.back()^1);QVERIFY(write(path,changed));
            bool done=false,saved=true;archive.prepareToQuit([&](bool ok,const QString&){saved=ok;done=true;});
            QTRY_VERIFY_WITH_TIMEOUT(done,3500);QVERIFY(!saved);QCOMPARE(read(path),changed);
            QCOMPARE(exitReason(path+".exit",vault),QString("external-change"));QVERIFY(!read(path+".exit").contains("external-change"));
            QVERIFY(Vault::erasedFileNames(vault.filePath()).contains("prices.keefi.exit"));}
        QVERIFY(write(path,original));
        PriceArchive reopened(&vault,path);QVERIFY(reopened.open(false));CandleHttp http;MarketCandles model(&http);reopened.watch(&model);
        QVERIFY(model.archiveStatus().detail.contains("이전 종료"));QVERIFY(model.archiveStatus().detail.contains("외부 변경"));
        QVERIFY(model.archiveStatus().message.isEmpty());QVERIFY(!model.archiveStatus().canRetry);
        vault.lock();QVERIFY(model.archiveStatus().detail.isEmpty());
    }
    void exitNoticeIsReadWithoutAnExistingArchive(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        {
            PriceArchive archive(&vault,path);CandleHttp http;MarketCandles model(&http,{},[]{return Now+Minute;});archive.watch(&model);
            model.setSelection("coin:upbit:KRW-BTC",Quarter);emit http.finished(http.tokens.last(),quarters());
            QVERIFY(archive.dirty());QVERIFY(!archive.exists());
            archive.setBeforeCommitForTest([&]{archive.cancelQuit();});
            bool done=false,saved=true;archive.prepareToQuit([&](bool ok,const QString&){saved=ok;done=true;});
            QTRY_VERIFY_WITH_TIMEOUT(done,2000);QVERIFY(!saved);QVERIFY(!QFileInfo::exists(path));
            QTRY_COMPARE_WITH_TIMEOUT(exitReason(path+".exit",vault),QString("interrupted"),2000);
        }
        PriceArchive reopened(&vault,path);CandleHttp http;MarketCandles model(&http,{},[]{return Now+Minute;});reopened.watch(&model);
        model.setSelection("coin:upbit:KRW-BTC",Quarter);
        QTRY_VERIFY_WITH_TIMEOUT(reopened.isOpen(),2000);
        QVERIFY(model.archiveStatus().detail.contains("이전 종료"));QVERIFY(model.archiveStatus().detail.contains("중단"));
        QVERIFY(model.archiveStatus().message.isEmpty());QVERIFY(!model.archiveStatus().canRetry);QVERIFY(!reopened.exists());
    }
    void quitDeadlineDetachesStalledCipherWorker_data(){
        QTest::addColumn<bool>("afterCommit");QTest::newRow("before-commit")<<false;QTest::newRow("after-commit")<<true;
    }
    void quitDeadlineDetachesStalledCipherWorker(){
        QFETCH(bool,afterCommit);QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        auto archive=std::make_unique<PriceArchive>(&vault,path);QVERIFY(archive->open(true));const auto original=read(path);
        QVERIFY(archive->record(series({Now-Hour})).accepted);WorkerGate gate;
        if(afterCommit)archive->setVerifyWorkerForTest(gate.hook());else archive->setPrepareWorkerForTest(gate.hook());
        bool done=false,saved=true;int callbacks=0;QString reason;QElapsedTimer elapsed;elapsed.start();
        archive->prepareToQuit([&](bool ok,const QString& error){saved=ok;reason=error;done=true;++callbacks;});
        QTRY_VERIFY_WITH_TIMEOUT(gate.state->entered.available()>0,2000);
        QTRY_VERIFY_WITH_TIMEOUT(done,3500);QVERIFY(!saved);QVERIFY(reason.contains("3초"));
        QVERIFY2(elapsed.elapsed()<3600,qPrintable(QString::number(elapsed.elapsed())));
        const auto committed=read(path);if(afterCommit)QVERIFY(committed!=original);else QCOMPARE(committed,original);
        const auto quitElapsed=elapsed.elapsed();
        QElapsedTimer destruction;destruction.start();archive.reset();const auto destructionMs=destruction.elapsed();QVERIFY(destructionMs<500);
        qInfo("synthetic quit: stage=%s wait_ms=%lld destructor_ms=%lld",afterCommit?"after-commit":"before-commit",qlonglong(quitElapsed),qlonglong(destructionMs));
        gate.open();
        QLockFile writer(path+".lock");writer.setStaleLockTime(0);
        // QTRY evaluates its predicate repeatedly, including after success.
        // Re-acquiring an already held QLockFile is not an idempotent predicate.
        QTRY_VERIFY_WITH_TIMEOUT(writer.isLocked()||writer.tryLock(0),3000);writer.unlock();
        QCOMPARE(read(path),committed);QCOMPARE(callbacks,1);
        QTRY_COMPARE_WITH_TIMEOUT(exitReason(path+".exit",vault),QString("timeout"),2000);
        PriceArchive reopened(&vault,path);QVERIFY(reopened.open(false));CandleHttp http;MarketCandles model(&http);reopened.watch(&model);
        QVERIFY(model.archiveStatus().detail.contains("3초"));QVERIFY(model.archiveStatus().message.isEmpty());
        QCOMPARE(reopened.storedCount("coin:upbit:KRW-BTC",Hour),afterCommit?1:0);
    }
    void lockAtPreparedCommitCannotReplaceTheFile(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        const auto before=read(path);QVERIFY(archive.record(series({Now-Hour})).accepted);
        bool entered=false,competingWriter=false,done=false,saved=true;
        archive.setBeforeCommitForTest([&]{
            entered=true;vault.lock();
            QLockFile competitor(path+".lock");competitor.setStaleLockTime(0);competingWriter=competitor.tryLock(0);
        });
        archive.flushAsync([&](bool ok,const QString&){saved=ok;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,10000);
        QVERIFY(entered);QVERIFY(!competingWriter);QVERIFY(!saved);QCOMPARE(read(path),before);
        QLockFile after(path+".lock");after.setStaleLockTime(0);QVERIFY(after.tryLock(0));after.unlock();
        QVERIFY(vault.unlock(Password));QVERIFY(archive.open(false));QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),0);
    }
    void eraseAtPreparedCommitCannotRecreateTheArchive(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        QVERIFY(archive.record(series({Now-Hour})).accepted);bool erased=false,done=false,saved=true;
        archive.setBeforeCommitForTest([&]{erased=vault.erase();});
        archive.flushAsync([&](bool ok,const QString&){saved=ok;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,10000);
        QVERIFY(erased);QVERIFY(!saved);QVERIFY(!QFileInfo::exists(path));QVERIFY(!vault.exists());
    }
    void flushReportsTheFailureOfTheWorkerItWaitedFor(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        auto changed=read(path);changed[changed.size()-1]=char(changed.back()^1);
        QVERIFY(archive.record(series({Now-Hour})).accepted);bool injected=false;
        archive.setAfterSnapshotForTest([&]{injected=write(path,changed);});
        archive.startWriteForTest();QString error;
        QVERIFY(!archive.flush(&error));QVERIFY(injected);QVERIFY(!error.isEmpty());
        QVERIFY(!archive.isOpen());QCOMPARE(read(path),changed);
        // A second flush must not turn the failed dirty state into a success.
        QVERIFY(!archive.flush(&error));
    }
    void explicitQuitFlushSavesBeforeTheTenMinuteTimer(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);
        CandleHttp http;MarketCandles model(&http,{},[]{return Now+Minute;});archive.watch(&model);
        model.setSelection("coin:upbit:KRW-BTC",Quarter);emit http.finished(http.tokens.last(),quarters());
        QVERIFY(archive.dirty());QVERIFY(!archive.exists());bool done=false,saved=false;
        archive.flushAsync([&](bool ok,const QString&){saved=ok;if(ok)vault.lock();done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(saved);QVERIFY(!vault.isUnlocked());
        QVERIFY(vault.unlock(Password));QVERIFY(archive.open(false));
        QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Quarter),3);
    }
    void externalReplacementBetweenPrepareAndCommitIsRejected(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        auto replacement=read(path);QVERIFY(!replacement.isEmpty());replacement[replacement.size()-1]=char(replacement.back()^1);
        QVERIFY(archive.record(series({Now-Hour})).accepted);bool injected=false,done=false,saved=true;QString error;
        archive.setBeforeCommitForTest([&]{
            // A different inode makes the prepare/commit boundary deterministic,
            // independent of filesystem timestamp granularity.
            const auto incoming=dir.filePath("replacement.keefi");
            injected=write(incoming,replacement)&&QFile::remove(path)&&QFile::rename(incoming,path);
        });
        archive.flushAsync([&](bool ok,const QString& message){saved=ok;error=message;done=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,10000);
        QVERIFY(injected);QVERIFY(!saved);QVERIFY(!error.isEmpty());QCOMPARE(read(path),replacement);
    }
    void aSameStartStreamRevisionCannotBorrowRestProvenance(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        CandleHttp http;auto socket=std::make_unique<ArchiveSocket>();auto* stream=socket.get();
        MarketCandles model(&http,std::move(socket),[]{return Now+Minute;});
        MarketInstrument item;item.symbol="SYNUSDT";item.baseAsset="SYN";item.quoteAsset="USDT";
        item.market="spot";item.status="TRADING";item.tradable=true;model.setInstruments("binance",{item});
        model.setSelection("coin:binance:SYNUSDT",Minute);emit stream->connected(stream->generation);
        const auto start=Now-Minute;
        QJsonArray rows;rows.append(QJsonArray{QString::number(start),"100","110","90","105","2",QString::number(Now-1),"210","5","1","105","0"});
        HttpResult rest;rest.status=200;rest.body=QJsonDocument(rows).toJson();emit http.finished(http.tokens.last(),rest);
        const auto receipt=model.restObservation();QVERIFY(receipt.available);
        const QJsonObject k{{"t",QString::number(start)},{"T",QString::number(Now-1)},{"s","SYNUSDT"},{"i","1m"},
            {"o","100"},{"h","110"},{"l","90"},{"c","106"},{"v","3"},{"n","6"},{"x",true},{"q","318"},{"V","1"},{"Q","106"},{"B","0"}};
        emit stream->received(stream->generation,QJsonDocument(QJsonObject{{"e","kline"},{"E",QString::number(Now+Minute)},{"s","SYNUSDT"},{"k",k}}).toJson());
        QCOMPARE(model.series().candles.last().close,QString("106"));
        QCOMPARE(model.restObservation().candles.last().close,QString("105"));
        QCOMPARE(model.restObservation().restRevision,receipt.restRevision);
        // Attaching only now is the same snapshot boundary as finishing an async open.
        PriceArchive archive(&vault,dir.filePath("prices.keefi"));archive.watch(&model);
        const auto archived=archive.stored("coin:binance:SYNUSDT",Minute);QVERIFY(!archived.candles.isEmpty());
        QCOMPARE(archived.candles.last().close,QString("105"));
        model.setSelection({},Minute);QVERIFY(model.restObservation().candles.isEmpty());
    }
    void staleCapacityEvictionKeepsNewerRowsAndPersistsThemNext(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        QList<qint64> starts;for(int i=PriceArchive::MaxCandlesPerSeries;i>0;--i)starts.append(Now-qint64(i)*Hour);
        const QString amount=QString(80,u'9')+u'.'+QString(80,u'1');
        const auto wide=[&](const QString& identity,const QList<qint64>& times,qint64 at){
            auto input=series(times,at,identity);for(auto& row:input.candles)row.open=row.high=row.low=row.close=row.volume=amount;return input;
        };
        for(int i=0;i<6;++i)QVERIFY(archive.record(wide("coin:upbit:KRW-C"+QString(QChar(u'A'+i)),starts,Now+Minute)).accepted);
        bool added=false;
        archive.setAfterSnapshotForTest([&]{added=archive.record(wide("coin:upbit:KRW-CA",{Now},Now+Hour+Minute)).accepted;});
        QSignalSpy written(&archive,&PriceArchive::written);archive.startWriteForTest();
        QTRY_VERIFY_WITH_TIMEOUT(!written.isEmpty(),30000);QVERIFY(added);QVERIFY(written.first().first().toBool());
        QVERIFY(read(path).size()<=PriceArchive::MaxFileBytes);
        auto kept=archive.stored("coin:upbit:KRW-CA",Hour,1);QVERIFY(!kept.candles.isEmpty());
        QCOMPARE(kept.candles.last().startsAtMs,Now);QVERIFY(archive.dirty());
        QVERIFY(archive.flush());archive.lock();QVERIFY(archive.open(false));
        kept=archive.stored("coin:upbit:KRW-CA",Hour,1);QVERIFY(!kept.candles.isEmpty());
        QCOMPARE(kept.candles.last().startsAtMs,Now);
    }
    void retryTimerDoesNotNeedAnotherModelChange(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));
        const auto path=dir.filePath("prices.keefi");QLockFile occupied(path+".lock");occupied.setStaleLockTime(0);QVERIFY(occupied.tryLock(0));
        PriceArchive archive(&vault,path);archive.setRetryDelayForTest(1);
        CandleHttp http;MarketCandles model(&http,{},[]{return Now+Minute;});archive.watch(&model);
        model.setSelection("coin:upbit:KRW-BTC",Quarter);QVERIFY(!archive.isOpen());QVERIFY(model.archiveStatus().canRetry);
        QSignalSpy modelChanges(&model,&MarketCandles::changed);occupied.unlock();
        QTRY_VERIFY_WITH_TIMEOUT(archive.isOpen(),10000);QCOMPARE(modelChanges.size(),0);QVERIFY(model.archiveStatus().message.isEmpty());
        vault.lock();QVERIFY(model.archiveStatus().message.isEmpty());
    }
    void completedCandlesReopenOnlyWithTheSameVaultKey(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        PriceArchive archive(&vault,path);QVERIFY(!archive.open(false));QVERIFY(archive.open(true));QVERIFY(archive.exists());
        auto input=series({Now-3*Hour,Now-2*Hour,Now-Hour});input.forming=input.candles.last();input.forming->startsAtMs=Now;
        QString error;const auto merged=archive.record(input,&error);QVERIFY2(merged.accepted,qPrintable(error));QCOMPARE(merged.added,3);
        QVERIFY(archive.dirty());QVERIFY(archive.flush(&error));QVERIFY(!archive.dirty());
        const auto saved=read(path);QVERIFY(!saved.contains("KRW-BTC"));QVERIFY(!saved.contains("12345.678"));QVERIFY(!saved.contains("upbit"));
        // Zero padding: the size shows only whole 64 KiB blocks of history.
        QVERIFY((saved.size()-56)%PriceArchive::PaddingBytes==0);
        auto stored=archive.stored("coin:upbit:KRW-BTC",Hour);QCOMPARE(stored.candles.size(),3);QCOMPARE(stored.candles.last().startsAtMs,Now-Hour);
        QCOMPARE(stored.receivedAtMs,Now+Minute);QCOMPARE(stored.asset,QString("BTC"));QVERIFY(!stored.forming);QVERIFY(stored.fromArchive);
        QVERIFY(archive.record(input).added==0);QVERIFY(!archive.dirty());
        QVERIFY(vault.changePassword(Password,OtherPassword));QCOMPARE(read(path),saved);
        vault.lock();QVERIFY(!archive.isOpen());QVERIFY(archive.stored("coin:upbit:KRW-BTC",Hour).candles.isEmpty());QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),0);
        QVERIFY(vault.unlock(OtherPassword));QVERIFY(archive.open(false));stored=archive.stored("coin:upbit:KRW-BTC",Hour);
        QCOMPARE(stored.candles.size(),3);QCOMPARE(stored.candles.first().close,QString("12345.678"));QCOMPARE(stored.candles.first().lastStoredTickMs,Now-3*Hour+1000);
    }
    void archiveNeverCreatesTheVaultLedgerKey(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(vault.create(Password));const auto before=read(vault.filePath());
        QVERIFY(!vault.hasAssetsLedgerKey());
        PriceArchive archive(&vault,dir.filePath("prices.keefi"));QString error;QVERIFY(!archive.open(true,&error));QVERIFY(error.contains("원장 키"));
        QCOMPARE(read(vault.filePath()),before);QVERIFY(!archive.exists());QVERIFY(vault.isUnlocked());
        CandleHttp http;MarketCandles model(&http,{},[]{return Now+Minute;});archive.watch(&model);
        model.setSelection("coin:upbit:KRW-BTC",Quarter);
        QVERIFY(model.archiveStatus().message.isEmpty());QVERIFY(model.archiveStatus().detail.contains("차트 기록 저장 꺼짐"));QVERIFY(!model.archiveStatus().canRetry);
        QVERIFY(!vault.hasAssetsLedgerKey());QCOMPARE(read(vault.filePath()),before);
        // Only the existing asset-ledger flow creates the key. A later model update
        // activates recording; there is no chart-side activation or key creation.
        AssetsLedger ledger(&vault,dir.filePath("assets.bin"));QVERIFY(ledger.open(true));ledger.lock();
        emit model.changed();QVERIFY(archive.isOpen());QVERIFY(model.archiveStatus().detail.isEmpty());
    }
    void foreignVaultsAndOtherKeyDomainsFailAuthentication(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        {PriceArchive archive(&vault,path);QVERIFY(archive.open(true));QVERIFY(archive.record(series({Now-Hour})).accepted);QVERIFY(archive.flush());}
        const auto saved=read(path);
        QTemporaryDir otherDir;Vault other(otherDir.filePath("vault.bin"));QVERIFY(keyedVault(other,otherDir));
        PriceArchive foreign(&other,path);QString error;QVERIFY(!foreign.open(false,&error));QVERIFY2(error.contains("인증"),qPrintable(error));QCOMPARE(read(path),saved);
        // Right container, right vault, but the asset ledger's key domain: authentication fails.
        const auto json=QJsonDocument(QJsonObject{{"schema_version",1},{"series",QJsonArray{}}}).toJson(QJsonDocument::Compact);
        QVERIFY(write(path,seal(json,derivedKey(vault,"KEEFI/assets-ledger/v1"))));const auto ledgerDomain=read(path);
        PriceArchive archive(&vault,path);QVERIFY(!archive.open(false,&error));QVERIFY2(error.contains("인증"),qPrintable(error));QCOMPARE(read(path),ledgerDomain);
        QVERIFY(write(path,seal(json,derivedKey(vault,"KEEFI/price-archive/v1"))));QVERIFY(archive.open(false,&error));
    }
    void invalidRowsRejectTheWholeCallAndNothingIsMerged(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));PriceArchive archive(&vault,dir.filePath("prices.keefi"));QVERIFY(archive.open(true));
        auto unfinished=series({Now-2*Hour,Now-Hour,Now});QVERIFY(!archive.record(unfinished).accepted);
        auto rounded=series({Now-2*Hour,Now-Hour});rounded.candles[1].close="105.0";QVERIFY(!archive.record(rounded).accepted);
        auto inverted=series({Now-2*Hour,Now-Hour});inverted.candles[0].low="12500";QVERIFY(!archive.record(inverted).accepted);
        auto unordered=series({Now-Hour,Now-2*Hour});unordered.restThroughMs=Now-Hour;QVERIFY(!archive.record(unordered).accepted);
        auto offGrid=series({Now-Hour+1});QVERIFY(!archive.record(offGrid).accepted);
        auto foreignSource=series({Now-Hour});foreignSource.sourceUrl="https://example.invalid/v1/candles/minutes/60?market=KRW-BTC&count=200";QVERIFY(!archive.record(foreignSource).accepted);
        auto stock=series({Now-Hour},Now+Minute,"stock:KRX:005930");QVERIFY(!archive.record(stock).accepted);
        auto unsupportedInterval=series({Now-Hour},Now+Minute,"coin:upbit:KRW-BTC",7200000);QVERIFY(!archive.record(unsupportedInterval).accepted);
        auto noReceipt=series({Now-Hour},0);QVERIFY(!archive.record(noReceipt).accepted);
        auto wrongAsset=series({Now-Hour});wrongAsset.asset="ETH";QVERIFY(!archive.record(wrongAsset).accepted);
        auto binance=series({Now-Hour},Now+Minute,"coin:binance:SYNUSDT");binance.asset="SYN";binance.currency="USDT";QVERIFY(archive.record(binance).accepted);
        binance.currency="BTC";QVERIFY(!archive.record(binance).accepted);
        QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),0);QCOMPARE(archive.storedCount("coin:binance:SYNUSDT",Hour),1);
    }
    void rowsWaitForASettledRestReceipt(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));PriceArchive archive(&vault,dir.filePath("prices.keefi"));QVERIFY(archive.open(true));
        // Read ten seconds after its close: the last hour may still miss late trades.
        auto early=archive.record(series({Now-2*Hour,Now-Hour},Now+10000));QVERIFY(early.accepted);QCOMPARE(early.added,1);QCOMPARE(early.pending,1);
        QCOMPARE(archive.stored("coin:upbit:KRW-BTC",Hour).candles.last().startsAtMs,Now-2*Hour);
        // The stream closed the last hour after the REST response: it waits for the next REST receipt.
        auto streamed=series({Now-2*Hour,Now-Hour},Now+Minute);streamed.restThroughMs=Now-2*Hour;
        const auto waiting=archive.record(streamed);QCOMPARE(waiting.added,0);QCOMPARE(waiting.pending,1);QCOMPARE(waiting.unchanged,1);
        QCOMPARE(archive.record(series({Now-2*Hour,Now-Hour})).added,1);QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),2);
    }
    void aDifferentValueIsAConflictAndOverflowStaysMarked(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        QList<qint64> starts;for(int i=70;i>0;--i)starts.append(Now-qint64(i)*Hour);
        QVERIFY(archive.record(series(starts)).accepted);
        const auto revised=archive.record(series(starts,Now+2*Minute,"coin:upbit:KRW-BTC",Hour,"12400"));
        QVERIFY(revised.accepted);QCOMPARE(revised.added,0);QCOMPARE(revised.conflicts,PriceArchive::MaxConflictsPerSeries);QCOMPARE(revised.droppedConflicts,70-PriceArchive::MaxConflictsPerSeries);
        // A revision already kept as a conflict is not counted again.
        const auto repeated=archive.record(series({Now-70*Hour},Now+3*Minute,"coin:upbit:KRW-BTC",Hour,"12400"));QCOMPARE(repeated.conflicts,0);QCOMPARE(repeated.droppedConflicts,0);
        QCOMPARE(archive.conflictCount("coin:upbit:KRW-BTC",Hour),PriceArchive::MaxConflictsPerSeries);QCOMPARE(archive.conflictOverflow("coin:upbit:KRW-BTC",Hour),70-PriceArchive::MaxConflictsPerSeries);
        QCOMPARE(archive.stored("coin:upbit:KRW-BTC",Hour).candles.last().close,QString("12345.678"));
        QVERIFY(archive.flush());archive.lock();QVERIFY(archive.open(false));
        QCOMPARE(archive.conflictCount("coin:upbit:KRW-BTC",Hour),PriceArchive::MaxConflictsPerSeries);QCOMPARE(archive.conflictOverflow("coin:upbit:KRW-BTC",Hour),70-PriceArchive::MaxConflictsPerSeries);
        QCOMPARE(archive.stored("coin:upbit:KRW-BTC",Hour).candles.first().close,QString("12345.678"));
    }
    void storedReturnsOnlyTheLastUninterruptedRun(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));PriceArchive archive(&vault,dir.filePath("prices.keefi"));QVERIFY(archive.open(true));
        QVERIFY(archive.record(series({Now-10*Hour,Now-9*Hour},Now-8*Hour+Minute)).accepted);
        QVERIFY(archive.record(series({Now-4*Hour,Now-3*Hour,Now-2*Hour,Now-Hour})).accepted);
        QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),6);
        auto run=archive.stored("coin:upbit:KRW-BTC",Hour);QCOMPARE(run.candles.size(),4);QCOMPARE(run.candles.first().startsAtMs,Now-4*Hour);
        run=archive.stored("coin:upbit:KRW-BTC",Hour,2);QCOMPARE(run.candles.size(),2);QCOMPARE(run.candles.first().startsAtMs,Now-2*Hour);
        QVERIFY(archive.stored("coin:upbit:KRW-BTC",Quarter).candles.isEmpty());QVERIFY(archive.stored("coin:upbit:KRW-ETH",Hour).candles.isEmpty());
    }
    void boundsTrimOldCandlesAndEvictShortSeriesViewedLeastRecently(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));PriceArchive archive(&vault,dir.filePath("prices.keefi"));QVERIFY(archive.open(true));
        QList<qint64> starts;for(int i=PriceArchive::MaxCandlesPerSeries+5;i>0;--i)starts.append(Now-qint64(i)*Hour);
        QVERIFY(archive.record(series(starts)).accepted);QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),PriceArchive::MaxCandlesPerSeries);
        QCOMPARE(archive.stored("coin:upbit:KRW-BTC",Hour,PriceArchive::MaxCandlesPerSeries).candles.first().startsAtMs,Now-qint64(PriceArchive::MaxCandlesPerSeries)*Hour);
        // A daily series viewed long ago outlives hourly ones viewed later.
        QVERIFY(archive.record(series({Midnight-Day},Midnight+Minute,"coin:upbit:KRW-DAY",Day)).accepted);
        for(int i=1;i<PriceArchive::MaxSeries-1;++i)QVERIFY(archive.record(series({Now-Hour},Now+Minute,"coin:upbit:KRW-C"+QString(QChar(u'A'+i)))).accepted);
        QVERIFY(archive.record(series({Now-Hour},Now+2*Minute,"coin:upbit:KRW-NEW")).accepted);
        QCOMPARE(archive.storedCount("coin:upbit:KRW-DAY",Day),1);QCOMPARE(archive.storedCount("coin:upbit:KRW-NEW",Hour),1);
        // Equal view minutes fall back to the key order, never hash order: BTC was first.
        QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),0);QCOMPARE(archive.storedCount("coin:upbit:KRW-CB",Hour),1);
        QVERIFY(archive.record(series({Now-Hour},Now+3*Minute,"coin:upbit:KRW-NEWER")).accepted);
        QCOMPARE(archive.storedCount("coin:upbit:KRW-CB",Hour),0);QCOMPARE(archive.storedCount("coin:upbit:KRW-CC",Hour),1);QCOMPARE(archive.storedCount("coin:upbit:KRW-DAY",Day),1);
        QString error;QVERIFY2(archive.flush(&error),qPrintable(error));archive.lock();QVERIFY(archive.open(false));QCOMPARE(archive.storedCount("coin:upbit:KRW-NEWER",Hour),1);
    }
    void lockDropsUnwrittenRowsWithoutTouchingTheFile(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");PriceArchive archive(&vault,path);QVERIFY(archive.open(true));
        QVERIFY(archive.record(series({Now-Hour})).accepted);QVERIFY(archive.dirty());const auto before=read(path);
        vault.lock();QVERIFY(!archive.isOpen());QVERIFY(!archive.dirty());QCOMPARE(read(path),before);
        QString error;QVERIFY(!archive.open(false,&error));QVERIFY(!archive.record(series({Now-2*Hour})).accepted);
        QVERIFY(vault.unlock(Password));QVERIFY(archive.open(false));QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),0);
    }
    void tamperConcurrentUseAndExternalChangeNeverReplaceTheFile(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        PriceArchive archive(&vault,path);QVERIFY(archive.open(true));QVERIFY(archive.record(series({Now-Hour})).accepted);QVERIFY(archive.flush());const auto original=read(path);
        PriceArchive concurrent(&vault,path);QVERIFY(!concurrent.open(false));QVERIFY(archive.isOpen());
        auto corrupt=original;corrupt[corrupt.size()-1]=char(corrupt.back()^1);QVERIFY(write(path,corrupt));
        QVERIFY(archive.record(series({Now-2*Hour})).accepted);QString error;QVERIFY(!archive.flush(&error));QVERIFY(!archive.isOpen());QCOMPARE(read(path),corrupt);
        QVERIFY(!archive.open(false));QCOMPARE(read(path),corrupt);QVERIFY(setAside(dir).isEmpty());
        QVERIFY(write(path,original));QVERIFY(archive.open(false));QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),1);
    }
    void anUnreadableFileIsSetAsideOnlyWhenCreatingIsAllowed(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        QVERIFY(write(path,QByteArray(4096,'x')));PriceArchive archive(&vault,path);QVERIFY(!archive.open(false));QCOMPARE(read(path),QByteArray(4096,'x'));
        QVERIFY(archive.open(true));const auto aside=setAside(dir);QCOMPARE(aside.size(),1);QCOMPARE(read(dir.filePath(aside.first())),QByteArray(4096,'x'));
        QVERIFY(archive.lastError().contains("새로 시작"));QVERIFY(archive.record(series({Now-Hour})).accepted);QVERIFY(archive.flush());
        archive.lock();QVERIFY(archive.open(false));QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Hour),1);
        QVERIFY(Vault::erasedFileNames(vault.filePath()).contains(aside.first()));
    }
    void aSeriesThatNoLongerValidatesIsDroppedAndTheRestOpens(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        const QJsonArray all{storedSeries("coin:upbit:KRW-BTC","https://api.upbit.com/v1/candles/minutes/60","BTC"),
                             storedSeries("coin:upbit:KRW-ETH","https://api.upbit.com/v1/candles/minutes/61","ETH")};
        const auto json=QJsonDocument(QJsonObject{{"schema_version",1},{"series",all}}).toJson(QJsonDocument::Compact);
        QVERIFY(write(path,seal(json,derivedKey(vault,"KEEFI/price-archive/v1"))));
        PriceArchive archive(&vault,path);QString error;QVERIFY2(archive.open(false,&error),qPrintable(error));
        QVERIFY2(archive.storedCount("coin:upbit:KRW-BTC",Hour)==1,qPrintable(archive.lastError()));QCOMPARE(archive.storedCount("coin:upbit:KRW-ETH",Hour),0);
        QVERIFY(archive.lastError().contains("1개"));QVERIFY(archive.dirty());QVERIFY(archive.flush());QVERIFY(setAside(dir).isEmpty());
    }
    void watchedChartRecordsInTheBackgroundAndShowsStoredRowsForDisplayOnly(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        PriceArchive archive(&vault,path);archive.setFlushDelayMs(0);QSignalSpy written(&archive,&PriceArchive::written);
        CandleHttp http;qint64 now=Now;MarketCandles model(&http,{},[&]{return now;});archive.watch(&model);
        model.setSelection("coin:upbit:KRW-BTC",Quarter);QCOMPARE(http.tokens.size(),1);QVERIFY(archive.isOpen());QVERIFY(!archive.exists());
        now=Now+Minute;emit http.finished(http.tokens.last(),quarters());QVERIFY(model.series().available);
        QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Quarter),3);
        QTRY_VERIFY(!written.isEmpty());QVERIFY(written.first().first().toBool());QVERIFY(archive.exists());QVERIFY(!archive.dirty());
        model.setSelection("coin:upbit:KRW-ETH",Quarter);QVERIFY(model.displayObservation().candles.isEmpty());
        // A warm session now prefers its public memory cache. Force a cache miss
        // while the other selection is empty so this still tests the archive.
        model.clearDisplayCache();QVERIFY(model.displayObservation().candles.isEmpty());
        model.setSelection("coin:upbit:KRW-BTC",Quarter);
        auto shown=model.displayObservation();QCOMPARE(shown.candles.size(),3);QVERIFY(shown.delayed);QVERIFY(!shown.available);QVERIFY(shown.fromArchive);
        QVERIFY(!shown.fromMemoryCache);QCOMPARE(shown.identity,QString("coin:upbit:KRW-BTC"));QCOMPARE(shown.intervalMs,Quarter);
        QCOMPARE(shown.sourceUrl,MarketCandles::request(shown.identity,Quarter).url.toString());
        QVERIFY(model.series().candles.isEmpty());QVERIFY(!model.series().available);
        // A live response replaces the stored rows.
        now=Now+2*Minute;emit http.finished(http.tokens.last(),quarters());shown=model.displayObservation();QVERIFY(shown.available);QVERIFY(!shown.fromArchive);
        // With no memory hit, hiding/restoring offers the archive again. Its
        // display must still be revoked by the lock without any reselection.
        model.setSelection({},Quarter);model.clearDisplayCache();model.setSelection("coin:upbit:KRW-BTC",Quarter);QVERIFY(model.displayObservation().fromArchive);
        vault.lock();QVERIFY(!archive.isOpen());QVERIFY(model.displayObservation().candles.isEmpty());
        model.setSelection("coin:upbit:KRW-ETH",Quarter);model.setSelection("coin:upbit:KRW-BTC",Quarter);QVERIFY(model.displayObservation().candles.isEmpty());
        now=Now+3*Minute;emit http.finished(http.tokens.last(),quarters());QVERIFY(model.series().available);QVERIFY(!archive.isOpen());
    }
    void aRevisionInTheSameWindowIsRecordedAsAConflict(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));PriceArchive archive(&vault,dir.filePath("prices.keefi"));
        CandleHttp http;qint64 now=Now+Minute;MarketCandles model(&http,{},[&]{return now;});archive.watch(&model);
        model.setSelection("coin:upbit:KRW-BTC",Quarter);emit http.finished(http.tokens.last(),quarters());QCOMPARE(archive.storedCount("coin:upbit:KRW-BTC",Quarter),3);
        model.setSelection("coin:upbit:KRW-ETH",Quarter);model.setSelection("coin:upbit:KRW-BTC",Quarter);
        now=Now+2*Minute;emit http.finished(http.tokens.last(),quarters("106"));
        QCOMPARE(model.series().candles.last().close,QString("106"));QCOMPARE(archive.conflictCount("coin:upbit:KRW-BTC",Quarter),1);
        QCOMPARE(archive.stored("coin:upbit:KRW-BTC",Quarter).candles.last().close,QString("105"));
    }
    void storedRowsWaitForMarketMetadataAndExpireAfterADay(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(keyedVault(vault,dir));const auto path=dir.filePath("prices.keefi");
        {PriceArchive seed(&vault,path);QVERIFY(seed.open(true));QVERIFY(seed.record(series({Now-3*Quarter,Now-2*Quarter,Now-Quarter},Now+Minute,"coin:upbit:KRW-BTC",Quarter)).accepted);QVERIFY(seed.flush());}
        PriceArchive archive(&vault,path);CandleHttp http;qint64 now=Now+2*Minute;MarketCandles model(&http,{},[&]{return now;});archive.watch(&model);
        model.setVerifiedUpbitMarkets({},false);model.setSelection("coin:upbit:KRW-BTC",Quarter);
        QTRY_VERIFY(archive.isOpen());QVERIFY(model.displayObservation().candles.isEmpty());
        model.setVerifiedUpbitMarkets({"KRW-BTC"},true);QCOMPARE(model.displayObservation().candles.size(),3);
        model.setSelection("coin:upbit:KRW-ETH",Quarter);now=Now+2*Day;model.setSelection("coin:upbit:KRW-BTC",Quarter);QVERIFY(model.displayObservation().candles.isEmpty());
    }
    void offeredRowsAreGuardedAndClearingKeepsLiveRows(){
        CandleHttp http;qint64 now=Now+Minute;MarketCandles model(&http,{},[&]{return now;});model.setSelection("coin:upbit:KRW-BTC",Quarter);
        auto stored=series({Now-2*Quarter,Now-Quarter},Now,"coin:upbit:KRW-BTC",Quarter);stored.fromArchive=true;
        auto other=stored;other.identity="coin:upbit:KRW-ETH";QVERIFY(model.setStoredCompleted(other)==StoredCandlesOffer::Refused);
        auto interval=stored;interval.intervalMs=Hour;QVERIFY(model.setStoredCompleted(interval)==StoredCandlesOffer::Refused);
        auto many=stored;many.candles.clear();for(int i=201;i>0;--i){auto c=stored.candles.first();c.startsAtMs=Now-qint64(i)*Quarter;many.candles.append(c);}
        QVERIFY(model.setStoredCompleted(many)==StoredCandlesOffer::Refused);
        auto future=stored;future.receivedAtMs=now+1;QVERIFY(model.setStoredCompleted(future)==StoredCandlesOffer::Refused);
        auto old=stored;now=Now+Day+Minute;QVERIFY(model.setStoredCompleted(old)==StoredCandlesOffer::Refused);now=Now+Minute;
        QVERIFY(model.setStoredCompleted(stored)==StoredCandlesOffer::Accepted);QVERIFY(model.setStoredCompleted(stored)==StoredCandlesOffer::Refused);
        now=Now+Day+Minute;QVERIFY(model.displayObservation().candles.isEmpty());now=Now+Minute;
        model.clearStoredCompleted();QVERIFY(model.displayObservation().candles.isEmpty());
        emit http.finished(http.tokens.last(),quarters());now=Now+Minute+FreshnessGap;
        QVERIFY(model.displayObservation().delayed);QVERIFY(!model.displayObservation().fromArchive);
        model.clearStoredCompleted();QCOMPARE(model.displayObservation().candles.size(),3);
        model.setVerifiedUpbitMarkets({},false);model.setSelection("coin:upbit:KRW-ETH",Quarter);model.setSelection("coin:upbit:KRW-BTC",Quarter);
        QVERIFY(model.setStoredCompleted(stored)==StoredCandlesOffer::NotNow);
    }
  private:
    static constexpr qint64 FreshnessGap=MarketCandles::FreshnessMs+1000;
};
} // namespace keefi
QTEST_GUILESS_MAIN(keefi::PriceArchiveTests)
#include "tst_price_archive.moc"
