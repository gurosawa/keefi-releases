#include "keefi/AssetsLedger.h"
#include "keefi/AssetsCapture.h"
#include "keefi/Vault.h"
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTest>
#include <QSignalSpy>
#include <sodium.h>
#include <cmath>
#include <algorithm>
#include <limits>

namespace keefi {
namespace {
const QString Password="Fake assets Vault password 2026!",NewPassword="Other fake Assets password 2026!";
constexpr qint64 Start=1792200000000LL,Year=365LL*86400000;
AssetEvidence evidence(qint64 time=Start){return {"FAKE_OFFICIAL_SOURCE","https://example.invalid/official","fake_complete_scope",time,time,true};}
AssetEvidence publicEvidence(const QString& asset,const QString& currency="KRW",qint64 time=Start){
    auto e=evidence(time);e.scope=currency=="USDT"?"public:binance:"+asset+"USDT":"public:upbit:"+currency+"-"+asset;return e;
}
AssetSnapshot snapshot(QString id,QString value,qint64 time=Start){
    AssetSnapshot s;s.id=id;s.accountScope="FAKE_ACCOUNT_SCOPE_ONLY";s.provider="fake";s.environment="test";s.market="spot";s.timeMs=time;s.balanceComplete=true;s.reconciled=true;s.coverage="synthetic complete fixture";s.evidence=evidence(time);
    AssetComponent c;c.id="cash";c.asset="KRW";c.quantity=value;c.kind=AssetComponentKind::Cash;c.evidence=evidence(time);s.components={c};return s;
}
AssetEvent assetEvent(QString id,AssetEventKind kind,QString qty,qint64 time=Start+1000){
    AssetEvent e;e.id=id;e.accountScope="FAKE_ACCOUNT_SCOPE_ONLY";e.asset="KRW";e.quantity=qty;e.officialId=id;e.timeMs=time;e.kind=kind;e.evidence=evidence(time);return e;
}
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
bool write(const QString& path,const QByteArray& bytes){QFile f(path);return f.open(QIODevice::WriteOnly|QIODevice::Truncate)&&f.write(bytes)==bytes.size()&&f.flush();}
}
class AssetsTests final:public QObject {
    Q_OBJECT
  private slots:
    void chartAveragesNeverEnterCapturedAssetsOrValuation() {
        AccountRecord account;account.id="synthetic-chart-account";account.provider="upbit";account.environment="live";account.market="spot";
        MarketSnapshot raw;raw.balances={{"BTC","1","0"}};
        const auto baseline=captureExchangeAssets(account,"FAKE_CHART_SCOPE",raw,Start,0,true,true,"synthetic",{});
        raw.balances[0].providerAveragePrice="999999999";raw.balances[0].providerAverageCurrency="USD";
        const auto changed=captureExchangeAssets(account,"FAKE_CHART_SCOPE",raw,Start,0,true,true,"synthetic",{});
        QVERIFY(baseline.snapshot==changed.snapshot);
        const AssetQuote quote{"BTC","KRW","100",publicEvidence("BTC")};
        const auto before=AssetValuation::value({baseline.snapshot},{quote},"KRW",Start);
        const auto after=AssetValuation::value({changed.snapshot},{quote},"KRW",Start);
        QCOMPARE(after.netAssets,before.netAssets);QCOMPARE(after.knownSubtotal,before.knownSubtotal);
        QCOMPARE(after.knownSubtotal,QString("100"));
    }
    void asyncHistoryWindowYieldsAndReadsArchiveWithoutWrites() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        const auto path=dir.filePath("assets.keefi");
        AssetsLedger ledger(&vault,path);QVERIFY(ledger.open(true));QVERIFY(ledger.append({snapshot("current","2",Start+1)},{},{}));ledger.lock();
        AssetsLedger archive(&vault,path+".archive-fixture.keefi");QVERIFY(archive.open(true));QVERIFY(archive.append({snapshot("archive","1",Start)},{},{}));archive.lock();
        const auto files=QDir(dir.path()).entryList(QDir::Files|QDir::Hidden);
        const auto before=read(path);AssetsLedger reader(&vault,path);
        bool done=false,ok=false,heartbeat=false;AssetsLedger::HistoryWindow result;QThread* callbackThread=nullptr;
        reader.readWindowAsync(Start,Start+100,[&](bool success,AssetsLedger::HistoryWindow window,QString){done=true;ok=success;result=std::move(window);callbackThread=QThread::currentThread();});
        QVERIFY(!done);QTimer::singleShot(0,&reader,[&]{heartbeat=true;});
        QTRY_VERIFY_WITH_TIMEOUT(done,2000);QVERIFY(ok);QVERIFY(heartbeat);QCOMPARE(callbackThread,reader.thread());
        QCOMPARE(result.snapshots.size(),2);QCOMPARE(result.snapshots.first().id,QString("archive"));
        QCOMPARE(read(path),before);QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Hidden),files);QVERIFY(!reader.isOpen());
    }
    void asyncHistoryWindowCancelsPendingReadOnLockAndReplacement() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        QVERIFY(ledger.append({snapshot("first","1",Start),snapshot("second","2",Start+1)},{},{}));ledger.lock();
        int obsolete=0,completed=0;AssetsLedger::HistoryWindow result;
        ledger.readWindowAsync(Start,Start+100,[&](bool,AssetsLedger::HistoryWindow,QString){++obsolete;});
        vault.lock();QVERIFY(vault.unlock(Password));
        ledger.readWindowAsync(Start,Start,[&](bool,AssetsLedger::HistoryWindow,QString){++obsolete;});
        ledger.readWindowAsync(Start+1,Start+100,[&](bool ok,AssetsLedger::HistoryWindow window,QString){if(ok){++completed;result=std::move(window);}});
        QTRY_COMPARE_WITH_TIMEOUT(completed,1,2000);QCOMPARE(obsolete,0);QCOMPARE(result.snapshots.size(),1);QCOMPARE(result.snapshots.first().id,QString("second"));
    }
    void historyWindowReportsFutureEligibilityWithoutReturningFutureData() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        auto delayed=snapshot("delayed","3",Start+10);delayed.evidence.receivedAtMs=Start+200;
        QVERIFY(ledger.append({snapshot("present","1",Start),snapshot("future","2",Start+101),delayed},{},{}));
        AssetsLedger::HistoryWindow window;QVERIFY(ledger.readWindow(Start,Start+100,&window));
        QCOMPARE(window.snapshots.size(),1);QCOMPARE(window.nextEligibleMs,Start+101);
    }
    void historyWindowCrossesArchiveWithoutWriting() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        QList<AssetSnapshot> rows;
        for(qsizetype i=0;i<AssetsLedger::MaxSnapshots-1;++i)rows.append(snapshot(QString::number(i),"0",Start-86400000+i));
        rows.append(snapshot("today-archive","10",Start));
        AssetQuote first{"PUMP","KRW","2",publicEvidence("PUMP","KRW",Start)};
        QVERIFY(ledger.append(rows,{},{first}));
        AssetQuote last=first;last.price="3";last.evidence=publicEvidence("PUMP","KRW",Start+300000);
        QVERIFY(ledger.appendWithArchive({snapshot("today-current","11",Start+300000)},{},{last},{}));
        QCOMPARE(ledger.archiveFiles().size(),1);
        const auto files=QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name);
        QMap<QString,QByteArray> before;for(const auto& name:files)before.insert(name,read(dir.filePath(name)));
        AssetsLedger::HistoryWindow window;QString error;
        QVERIFY2(ledger.readWindow(Start,Start+300000,&window,&error),qPrintable(error));
        QCOMPARE(window.snapshots.size(),2);QCOMPARE(window.quotes.size(),2);
        QCOMPARE(window.snapshots.first().id,QString("today-archive"));QCOMPARE(window.snapshots.last().id,QString("today-current"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name),files);
        for(const auto& name:files)QCOMPARE(read(dir.filePath(name)),before.value(name));
    }
    void historyWindowFiltersInclusiveRangeAndFutureEvidence() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        auto lateReceipt=snapshot("late-receipt","1",Start+10);lateReceipt.evidence.receivedAtMs=Start+101;
        auto lateSource=snapshot("late-source","1",Start+10);lateSource.evidence.sourceTimeMs=Start+101;
        QList<AssetSnapshot> rows{snapshot("before","1",Start-1),snapshot("first","1",Start),snapshot("last","2",Start+100),snapshot("after","2",Start+101),lateReceipt,lateSource};
        QList<AssetQuote> quotes;
        for(qint64 time:{Start-1,Start,Start+100,Start+101})quotes.append({"PUMP","KRW","2",publicEvidence("PUMP","KRW",time)});
        AssetQuote late{"SOL","KRW","3",publicEvidence("SOL","KRW",Start+101)};late.evidence.receivedAtMs=Start+10;quotes.append(late);
        QVERIFY(ledger.append(rows,{},quotes));AssetsLedger::HistoryWindow window;
        QVERIFY(ledger.readWindow(Start,Start+100,&window));QCOMPARE(window.snapshots.size(),2);QCOMPARE(window.quotes.size(),2);
        QCOMPARE(window.snapshots.first().id,QString("first"));QCOMPARE(window.snapshots.last().id,QString("last"));
        QVERIFY(ledger.readWindow(Start+100,Start+100,&window));QCOMPARE(window.snapshots.size(),1);QCOMPARE(window.quotes.size(),1);
    }
    void historyWindowRejectsCorruptArchiveWithoutPartialResult() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));QVERIFY(ledger.append({snapshot("current","1")},{},{}));
        const auto archivePath=dir.filePath("assets.keefi.archive-fake.keefi");
        {AssetsLedger archive(&vault,archivePath);QVERIFY(archive.open(true));QVERIFY(archive.append({snapshot("old","1",Start-86400000)},{},{}));}
        auto bytes=read(archivePath);bytes[bytes.size()-1]=char(bytes.back()^1);QVERIFY(write(archivePath,bytes));
        AssetsLedger::HistoryWindow window;window.snapshots={snapshot("stale-output","99")};QString error;
        QVERIFY(!ledger.readWindow(Start,Start+100,&window,&error));QVERIFY(!error.isEmpty());
        QVERIFY(window.snapshots.isEmpty());QVERIFY(window.quotes.isEmpty());QCOMPARE(read(archivePath),bytes);
    }
    void historyWindowDeduplicatesAndRejectsConflicts() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        const auto row=snapshot("same","1");const AssetQuote quote{"PUMP","KRW","2",publicEvidence("PUMP")};
        QVERIFY(ledger.append({row},{},{quote}));
        {AssetsLedger archive(&vault,dir.filePath("assets.keefi.archive-same.keefi"));QVERIFY(archive.open(true));QVERIFY(archive.append({row},{},{quote}));}
        AssetsLedger::HistoryWindow window;QVERIFY(ledger.readWindow(Start,Start+100,&window));QCOMPARE(window.snapshots.size(),1);QCOMPARE(window.quotes.size(),1);
        {AssetsLedger archive(&vault,dir.filePath("assets.keefi.archive-conflict.keefi"));QVERIFY(archive.open(true));QVERIFY(archive.append({snapshot("same","2")},{},{}));}
        QString error;QVERIFY(!ledger.readWindow(Start,Start+100,&window,&error));QVERIFY(!error.isEmpty());QVERIFY(window.snapshots.isEmpty());QVERIFY(window.quotes.isEmpty());
    }
    void historyWindowClearsOutputOnLockAndInvalidRange() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));QVERIFY(ledger.append({snapshot("one","1")},{},{}));
        AssetsLedger::HistoryWindow window;QVERIFY(ledger.readWindow(Start,Start+100,&window));QCOMPARE(window.snapshots.size(),1);
        QString error;QVERIFY(!ledger.readWindow(Start+1,Start,&window,&error));QVERIFY(!error.isEmpty());QVERIFY(window.snapshots.isEmpty());
        QVERIFY(ledger.readWindow(Start,Start+100,&window));vault.lock();
        QVERIFY(!ledger.readWindow(Start,Start+100,&window,&error));QVERIFY(!error.isEmpty());QVERIFY(window.snapshots.isEmpty());QVERIFY(window.quotes.isEmpty());
    }
    void historyWindowReadsClosedCurrentWithoutCreatingLock() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        const auto path=dir.filePath("assets.keefi");
        {AssetsLedger writer(&vault,path);QVERIFY(writer.open(true));QVERIFY(writer.append({snapshot("one","1")},{},{}));}
        const auto files=QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name);
        AssetsLedger reader(&vault,path);AssetsLedger::HistoryWindow window;QString error;
        QVERIFY2(reader.readWindow(Start,Start+100,&window,&error),qPrintable(error));QCOMPARE(window.snapshots.size(),1);
        QVERIFY(!reader.isOpen());QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name),files);
    }
    void historyWindowReadsArchiveWithoutCurrentFile() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        {AssetsLedger writer(&vault,dir.filePath("assets.keefi.archive-only.keefi"));QVERIFY(writer.open(true));QVERIFY(writer.append({snapshot("one","1")},{},{}));}
        const auto files=QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name);
        AssetsLedger reader(&vault,dir.filePath("assets.keefi"));AssetsLedger::HistoryWindow window;
        QVERIFY(reader.readWindow(Start,Start+100,&window));QCOMPARE(window.snapshots.size(),1);QVERIFY(!reader.exists());
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name),files);
    }
    void historyWindowMissingHistoryDoesNotInventKeyOrFiles() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        const auto vaultBytes=read(vault.filePath());
        const auto files=QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name);
        AssetsLedger reader(&vault,dir.filePath("assets.keefi"));AssetsLedger::HistoryWindow window;QString error;
        QVERIFY2(reader.readWindow(Start,Start+100,&window,&error),qPrintable(error));QVERIFY(window.snapshots.isEmpty());QVERIFY(window.quotes.isEmpty());
        QVERIFY(!reader.exists());QCOMPARE(read(vault.filePath()),vaultBytes);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Hidden,QDir::Name),files);
    }
    void g7PublicPriceClockSkewKeepsOriginalTimes_data() {
        QTest::addColumn<qint64>("sourceLeadMs");
        QTest::addColumn<bool>("crossRoute");
        QTest::addColumn<bool>("accepted");
        for(qint64 lead:{0,3,19,35,1999,2000,2001})for(bool cross:{false,true})
            QTest::newRow(qPrintable(QString("lead-%1-%2").arg(lead).arg(cross?"cross":"direct")))
                <<lead<<cross<<(lead<=2000);
    }
    void g7PublicPriceClockSkewKeepsOriginalTimes() {
        QFETCH(qint64,sourceLeadMs);QFETCH(bool,crossRoute);QFETCH(bool,accepted);
        auto s=snapshot("clock-skew","2");s.components[0].asset="PUMP";
        AssetQuote direct{"PUMP","KRW","3",publicEvidence("PUMP","KRW",Start-1000)};
        AssetQuote coin{"PUMP","USDT","0.003",publicEvidence("PUMP","USDT")};
        AssetQuote fx{"USDT","KRW","1000",publicEvidence("USDT","KRW",Start-1000)};
        auto& skewed=crossRoute?fx:direct;skewed.evidence.receivedAtMs-=sourceLeadMs;
        const auto original=skewed.evidence;
        const auto r=AssetValuation::value({s},crossRoute?QList<AssetQuote>{coin,fx}:QList<AssetQuote>{direct},"KRW",Start,DisplayFreshnessMs);
        QCOMPARE(r.complete,accepted);
        if(!accepted){QVERIFY(r.rows.first().value.isEmpty());return;}
        QCOMPARE(r.netAssets,QString("6"));
        if(crossRoute){QCOMPARE(r.rows.first().conversionEvidence.size(),2);QVERIFY(r.rows.first().conversionEvidence.contains(original));}
        else QCOMPARE(r.rows.first().priceEvidence,original);
        QCOMPARE(skewed.evidence,original);
    }
    void g7ClockSkewDoesNotExtendFreshnessOrReferenceWindows_data() {
        QTest::addColumn<qint64>("sourceAge");QTest::addColumn<qint64>("receiptAge");
        QTest::addColumn<bool>("crossRoute");QTest::addColumn<bool>("current");QTest::addColumn<bool>("reference");
        const auto add=[](const char* name,qint64 source,qint64 receipt,bool current,bool reference){
            for(bool cross:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(name,cross?"cross":"direct")))
                <<source<<receipt<<cross<<current<<reference;
        };
        add("source-at-asof",0,35,true,false);
        add("future-source",-1,34,false,false);add("future-receipt",1000,-1,false,false);
        add("fresh-source-boundary",DisplayFreshnessMs,DisplayFreshnessMs,true,false);
        add("fresh-receipt-boundary",DisplayFreshnessMs-35,DisplayFreshnessMs,true,false);
        add("fresh-receipt-expired",DisplayFreshnessMs-34,DisplayFreshnessMs+1,false,true);
        add("reference-source-boundary",ReferenceMaxAgeMs,ReferenceMaxAgeMs,false,true);
        add("reference-receipt-boundary",ReferenceMaxAgeMs-35,ReferenceMaxAgeMs,false,true);
        add("reference-receipt-expired",ReferenceMaxAgeMs-34,ReferenceMaxAgeMs+1,false,false);
    }
    void g7ClockSkewDoesNotExtendFreshnessOrReferenceWindows() {
        QFETCH(qint64,sourceAge);QFETCH(qint64,receiptAge);QFETCH(bool,crossRoute);QFETCH(bool,current);QFETCH(bool,reference);
        auto s=snapshot("clock-windows","2");s.components[0].asset="PUMP";
        AssetQuote direct{"PUMP","KRW","3",publicEvidence("PUMP")};
        AssetQuote coin{"PUMP","USDT","0.003",publicEvidence("PUMP","USDT")};
        AssetQuote fx{"USDT","KRW","1000",publicEvidence("USDT")};
        auto& skewed=crossRoute?fx:direct;
        skewed.evidence.sourceTimeMs=Start-sourceAge;skewed.evidence.receivedAtMs=Start-receiptAge;
        const auto original=skewed.evidence;const auto quotes=crossRoute?QList<AssetQuote>{coin,fx}:QList<AssetQuote>{direct};
        const auto r=AssetValuation::value({s},quotes,"KRW",Start,DisplayFreshnessMs,quotes);
        QCOMPARE(r.complete,current);QCOMPARE(r.rows.first().value,current?QString("6"):QString{});
        QCOMPARE(r.rows.first().referenceValue,reference?QString("6"):QString{});
        QCOMPARE(skewed.evidence,original);
        if(sourceAge==-1)QCOMPARE(AssetValuation::value({s},quotes,"KRW",Start+1,DisplayFreshnessMs,quotes).netAssets,QString("6"));
    }
    void g7HistoricalPriceRejectsMalformedClockBoundaries() {
        auto event=assetEvent("malformed-clock",AssetEventKind::ExternalIn,"2",Start+1000);event.asset="PUMP";
        AssetQuote quote{"PUMP","KRW","3",publicEvidence("PUMP")};
        for(qint64 receipt:{std::numeric_limits<qint64>::min(),std::numeric_limits<qint64>::min()+1,qint64(-1),qint64(0)}){
            quote.evidence.receivedAtMs=receipt;
            const auto r=AssetValuation::flows({event},{"FAKE_ACCOUNT_SCOPE_ONLY"},{quote},"KRW",Start,Start+2000,true,DisplayFreshnessMs);
            QVERIFY(!r.complete);QVERIFY(r.netExternal.isEmpty());
        }
    }
    void referencePricesPreserveTimeAndNeverBecomeConfirmedAssets(){
        auto s=snapshot("reference","2");s.components[0].asset="PUMP";s.components[0].kind=AssetComponentKind::Holding;
        s.components[0].costBasis="1";s.components[0].costCurrency="KRW";
        AssetQuote quote{"PUMP","KRW","2.75",publicEvidence("PUMP","KRW",Start-176000)};quote.evidence.receivedAtMs=Start;
        auto r=AssetValuation::value({s},{},"KRW",Start,60000,{quote});
        QCOMPARE(r.rows[0].referenceValue,QString("5.5"));QCOMPARE(r.rows[0].priceEvidence,quote.evidence);
        QVERIFY(r.rows[0].value.isEmpty());QVERIFY(r.rows[0].unrealizedPnl.isEmpty());
        QVERIFY(!r.complete);QVERIFY(r.netAssets.isEmpty());QCOMPARE(r.knownSubtotal,QString("0"));
        QVERIFY(r.rows[0].reason.contains("참고 평가"));QVERIFY(r.rows[0].reason.contains("KST"));
        s.components[0].kind=AssetComponentKind::Liability;
        r=AssetValuation::value({s},{},"KRW",Start,60000,{quote});QCOMPARE(r.rows[0].referenceValue,QString("-5.5"));QCOMPARE(r.knownSubtotal,QString("0"));
        s.components[0].kind=AssetComponentKind::Holding;
        auto older=quote;older.price="9";older.evidence.sourceTimeMs-=1000;older.evidence.receivedAtMs+=1;
        r=AssetValuation::value({s},{},"KRW",Start+1,60000,{quote,older});QCOMPARE(r.rows[0].referenceValue,QString("5.5"));
        auto freshQuote=quote;freshQuote.price="3";freshQuote.evidence=publicEvidence("PUMP");
        r=AssetValuation::value({s},{freshQuote},"KRW",Start,60000,{quote});
        QCOMPARE(r.netAssets,QString("6"));QVERIFY(r.rows[0].referenceValue.isEmpty());QCOMPARE(r.rows[0].unrealizedPnl,QString("5"));
    }
    void referencePricesRejectInvalidOrAmbiguousEvidence_data(){
        QTest::addColumn<QString>("fault");
        for(const auto& fault:QStringList{"missing-price","negative-price","missing-times","future-source","future-receipt","receipt-before-source","incomplete","no-source","no-url","wrong-pair","conflict","stale-balance"})
            QTest::newRow(qPrintable(fault))<<fault;
    }
    void referencePricesRejectInvalidOrAmbiguousEvidence(){
        QFETCH(QString,fault);auto s=snapshot("bad-reference","2");s.components[0].asset="PUMP";
        AssetQuote quote{"PUMP","KRW","2.75",publicEvidence("PUMP","KRW",Start-176000)};quote.evidence.receivedAtMs=Start;
        if(fault=="missing-price")quote.price.clear();if(fault=="negative-price")quote.price="-2.75";
        if(fault=="missing-times"){quote.evidence.sourceTimeMs=0;quote.evidence.receivedAtMs=0;}
        if(fault=="future-source")quote.evidence.sourceTimeMs=Start+1;
        if(fault=="future-receipt")quote.evidence.receivedAtMs=Start+1;
        if(fault=="receipt-before-source")quote.evidence.receivedAtMs=Start-200000;
        if(fault=="incomplete")quote.evidence.complete=false;
        if(fault=="no-source")quote.evidence.source.clear();if(fault=="no-url")quote.evidence.url.clear();
        if(fault=="wrong-pair")quote.quoteAsset="USD";
        if(fault=="stale-balance")s.components[0].evidence=evidence(Start-120000);
        QList<AssetQuote> quotes{quote};if(fault=="conflict"){auto conflict=quote;conflict.price="3";quotes.append(conflict);}
        const auto r=AssetValuation::value({s},{},"KRW",Start,60000,quotes);
        QVERIFY(r.rows[0].referenceValue.isEmpty());QVERIFY(r.rows[0].value.isEmpty());QVERIFY(!r.complete);QCOMPARE(r.knownSubtotal,QString("0"));
    }
    void referenceCrossCanCombineSavedAndCurrentLegsWithoutPromotingEither() {
        auto account = snapshot("cross-reference", "2");
        account.components[0].asset = "PUMP";
        const auto staleAt = Start - DisplayFreshnessMs - 1;
        AssetQuote savedCoin{"PUMP", "USDT", "2", publicEvidence("PUMP", "USDT", staleAt)};
        AssetQuote currentFx{"USDT", "KRW", "1400", publicEvidence("USDT", "KRW")};
        auto duplicate = savedCoin;
        duplicate.evidence.url += "/duplicate";
        const auto evaluate = [&](const QList<AssetQuote>& current) {
            return AssetValuation::value({account}, current, "KRW", Start, DisplayFreshnessMs, {savedCoin});
        };
        auto view = evaluate({currentFx, duplicate});
        QVERIFY(!view.complete);
        QCOMPARE(view.knownSubtotal, QString("0"));
        QCOMPARE(view.rows.first().referenceValue, QString("5600"));
        QCOMPARE(view.rows.first().conversionEvidence, QList<AssetEvidence>({savedCoin.evidence, currentFx.evidence}));

        duplicate.price = "3";
        view = evaluate({currentFx, duplicate});
        QVERIFY(view.rows.first().referenceValue.isEmpty());
        QVERIFY(view.rows.first().conversionEvidence.isEmpty());
    }

    void directPriceWinsEvenWithNewerCrossAndCostInAnotherCurrency() {
        auto account = snapshot("direct-priority", "2");
        auto& holding = account.components.first();
        holding.asset = "PUMP";
        holding.kind = AssetComponentKind::Holding;
        holding.costBasis = "1.5";
        holding.costCurrency = "USDT";
        AssetQuote direct{"PUMP", "KRW", "1500.25", publicEvidence("PUMP", "KRW", Start-10)};
        AssetQuote coin{"PUMP", "USDT", "2", publicEvidence("PUMP", "USDT")};
        AssetQuote fx{"USDT", "KRW", "1400", publicEvidence("USDT")};
        const auto view = AssetValuation::value({account}, {coin, fx, direct}, "KRW", Start);
        QCOMPARE(view.netAssets, QString("3000.5"));
        QCOMPARE(view.rows.first().unrealizedPnl, QString("900.5"));
        QCOMPARE(view.rows.first().priceEvidence, direct.evidence);
        QVERIFY(view.rows.first().conversionEvidence.isEmpty());
    }

    void accountCompletenessAndRowEvidenceAreJudgedSeparately() {
        // Review R4: pins the current C++ rule the policy document and recalculator must follow.
        const qint64 at=Start+1000;
        auto incomplete=snapshot("incomplete","1000");incomplete.balanceComplete=false; // e.g. a loan holding or missing deposit
        auto r=AssetValuation::value({incomplete},{},"KRW",at,DisplayFreshnessMs);
        QVERIFY(!r.complete);QVERIFY(r.netAssets.isEmpty());QCOMPARE(r.knownSubtotal,QString("1000"));
        QVERIFY(r.rows.first().included);QCOMPARE(r.rows.first().value,QString("1000"));
        auto stale=incomplete;stale.components[0].evidence=evidence(at-DisplayFreshnessMs-1); // Old rows never become current.
        r=AssetValuation::value({stale},{},"KRW",at,DisplayFreshnessMs);
        QVERIFY(!r.complete);QCOMPARE(r.knownSubtotal,QString("0"));QVERIFY(r.rows.first().value.isEmpty());
        auto aged=snapshot("aged","1000");aged.timeMs=at-DisplayFreshnessMs-1;aged.evidence=evidence(at-DisplayFreshnessMs-1);
        r=AssetValuation::value({aged},{},"KRW",at,DisplayFreshnessMs); // Only the account-level time expired.
        QVERIFY(!r.complete);QCOMPARE(r.knownSubtotal,QString("1000"));QCOMPARE(r.rows.first().value,QString("1000"));
        r=AssetValuation::value({snapshot("complete","0")},{},"KRW",at,DisplayFreshnessMs);
        QVERIFY(r.complete);QCOMPARE(r.netAssets,QString("0")); // A real zero stays a complete zero.
    }
    void g7PricesUseOriginalTimePublicScopeAndOrderIndependentConflicts() {
        auto s=snapshot("g7-prices","1");s.components[0].asset="PUMP";
        AssetQuote original{"PUMP","KRW","102",publicEvidence("PUMP","KRW",Start-1000)};
        auto delayed=original;delayed.price="99";delayed.evidence.sourceTimeMs=Start-5000;delayed.evidence.receivedAtMs=Start;
        auto privatePrice=original;privatePrice.price="999";privatePrice.evidence=evidence();
        QList<AssetQuote> quotes{original,delayed,privatePrice};QList<int> order{0,1,2};
        do {QList<AssetQuote> permutation;for(int index:order)permutation.append(quotes[index]);
            QCOMPARE(AssetValuation::value({s},permutation,"KRW",Start,DisplayFreshnessMs).netAssets,QString("102"));
        }while(std::next_permutation(order.begin(),order.end()));
        auto old=original;old.price="1";old.evidence=publicEvidence("PUMP","KRW",Start-5000);
        auto clash=old;clash.price="2";quotes={old,clash,original};order={0,1,2};
        do {QList<AssetQuote> permutation;for(int index:order)permutation.append(quotes[index]);
            QCOMPARE(AssetValuation::value({s},permutation,"KRW",Start,DisplayFreshnessMs).netAssets,QString("102"));
        }while(std::next_permutation(order.begin(),order.end()));
        AssetQuote coin{"PUMP","USDT","0.5",publicEvidence("PUMP","USDT")},fx{"USDT","KRW","1400",publicEvidence("USDT")};
        auto r=AssetValuation::value({s},{old,clash,coin,fx},"KRW",Start,DisplayFreshnessMs);
        QCOMPARE(r.netAssets,QString("700"));QCOMPARE(r.rows.first().conversionEvidence.size(),2);
        clash.price="1.0";QCOMPARE(AssetValuation::value({s},{old,clash},"KRW",Start,DisplayFreshnessMs).netAssets,QString("1"));
        original.evidence.sourceTimeMs=0;
        QCOMPARE(AssetValuation::value({s},{original},"KRW",Start,DisplayFreshnessMs).netAssets,QString("102"));
        QVERIFY(!AssetValuation::value({s},{privatePrice},"KRW",Start,DisplayFreshnessMs).complete);
    }
    void g7ReferencePricesExpireAfterTwentyFourHoursAndInvalidLegsStayUnavailable() {
        auto s=snapshot("g7-reference","2");s.components[0].asset="PUMP";
        AssetQuote old{"PUMP","KRW","2.75",publicEvidence("PUMP","KRW",Start-ReferenceMaxAgeMs)};
        auto r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs,{old});
        QCOMPARE(r.rows.first().referenceValue,QString("5.5"));
        --old.evidence.sourceTimeMs;
        r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs,{old});QVERIFY(r.rows.first().referenceValue.isEmpty());
        const AssetQuote fx{"USDT","KRW","1400",publicEvidence("USDT")};
        for(const auto& fault:QStringList{"future-source","future-receipt","no-url","no-source","no-receipt","wrong-scope"}){
            AssetQuote coin{"PUMP","USDT","0.5",publicEvidence("PUMP","USDT")};
            if(fault=="future-source")coin.evidence.sourceTimeMs=Start+1;
            if(fault=="future-receipt")coin.evidence.receivedAtMs=Start+1;
            if(fault=="no-url")coin.evidence.url.clear();if(fault=="no-source")coin.evidence.source.clear();
            if(fault=="no-receipt")coin.evidence.receivedAtMs=0;if(fault=="wrong-scope")coin.evidence.scope="public:binance:OTHERUSDT";
            r=AssetValuation::value({s},{coin,fx},"KRW",Start,DisplayFreshnessMs);
            QVERIFY2(r.rows.first().value.isEmpty()&&r.rows.first().referenceValue.isEmpty(),qPrintable(fault));
        }
    }
    void g7ExcludesOnlyExplicitCanonicalMintsWithoutErasingHoldings() {
        const QString usdc="solana:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
        const QString other="solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3";
        auto s=snapshot("g7-exclude","2");s.market="solana";s.components[0].asset=usdc;
        auto r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs,{},QSet<QString>{usdc});
        QVERIFY(r.complete);QCOMPARE(r.netAssets,QString("0"));QCOMPARE(r.rows.first().quantity,QString("2"));
        QVERIFY(r.rows.first().excludedByUser);QVERIFY(!r.rows.first().included);QVERIFY(r.rows.first().value.isEmpty());
        auto c=s.components.first();c.id="other";c.asset=other;c.quantity="3";s.components.append(c);
        r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs,{},QSet<QString>{usdc,"SKHY"});
        QVERIFY(!r.complete);QVERIFY(r.rows.first().excludedByUser);QVERIFY(!r.rows.last().excludedByUser);
        s.components[0].asset="solana:invalid";
        r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs,{},QSet<QString>{"solana:invalid"});
        QVERIFY(!r.rows.first().excludedByUser);
    }
    void g7ArithmeticKeepsExactDivisionExplicitCurrenciesAndDuplicateEquityRules() {
        const QString usdc="solana:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
        const QString token="solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3";
        auto s=snapshot("g7-arithmetic","1");s.market="solana";s.components[0].asset=token;
        auto mintEvidence=evidence();mintEvidence.scope="public:jupiter:price:"+token.mid(7);
        AssetQuote mint{token,"USD","1",mintEvidence};
        mintEvidence.scope="public:jupiter:price:"+usdc.mid(7);
        AssetQuote dollar{usdc,"USD","3",mintEvidence},won{"USDC","KRW","1000",publicEvidence("USDC")};
        QCOMPARE(AssetValuation::value({s},{mint,dollar,won},"KRW",Start,DisplayFreshnessMs).netAssets,
                 QString("333.333333333333333333333333"));
        auto large=snapshot("overflow",QString(80,'9'));auto one=large.components.first();one.id="one";one.quantity="1";large.components.append(one);
        auto r=AssetValuation::value({large},{},"KRW",Start,DisplayFreshnessMs);
        QVERIFY(!r.complete);QVERIFY(r.knownSubtotal.isEmpty());QVERIFY(!r.rows[0].value.isEmpty());QVERIFY(!r.rows[1].value.isEmpty());
        auto reported=snapshot("reported","99");reported.components[0].asset="STOCK";reported.components[0].reportedValue="2";
        r=AssetValuation::value({reported},{},"KRW",Start,DisplayFreshnessMs);QVERIFY(r.rows.first().value.isEmpty());
        reported.components[0].reportedValueCurrency="USD";
        AssetQuote usd{"USD","KRW","1300",publicEvidence("USD")};
        QCOMPARE(AssetValuation::value({reported},{usd},"KRW",Start,DisplayFreshnessMs).netAssets,QString("2600"));
        auto equity=snapshot("equity","10");equity.components[0].kind=AssetComponentKind::Equity;
        auto duplicate=equity.components.first();duplicate.id="other";duplicate.quantity="20";equity.components.append(duplicate);
        r=AssetValuation::value({equity},{},"KRW",Start,DisplayFreshnessMs);
        QVERIFY(!r.complete);QCOMPARE(r.knownSubtotal,QString("10"));QCOMPARE(r.rows.size(),2);
        QVERIFY(r.rows.last().reason.contains("equity 중복"));
    }
    void g7SignedAggregateAppliesPrecisionLimitAfterCancellation() {
        const QString maximum(80,'9');
        QCOMPARE(sumAssetAmounts({}),QString("0"));
        QCOMPARE(sumAssetAmounts({"-0.00","0"}),QString("0"));
        QCOMPARE(sumAssetAmounts({"-"+maximum,"-1","1"}),"-"+maximum);
        QCOMPARE(sumAssetAmounts({maximum,maximum,"-"+maximum,"-"+maximum}),QString("0"));
        const QString smallest="0."+QString(79,'0')+'1';
        QCOMPARE(sumAssetAmounts({maximum,smallest,"-"+smallest}),maximum);
        QVERIFY(sumAssetAmounts({"-"+maximum,"-1"}).isEmpty());
        QVERIFY(sumAssetAmounts({"1\n","-1"}).isEmpty());
        QVERIFY(sumAssetAmounts({QString(81,'9'),"-"+QString(81,'9')}).isEmpty());
        auto s=snapshot("g7-signed-aggregate",maximum);
        auto one=s.components.first();one.id="one";one.quantity="1";
        auto debt=one;debt.id="debt";debt.kind=AssetComponentKind::Liability;
        const QList<AssetComponent> components{s.components.first(),one,debt};
        QList<int> order{0,1,2};
        do {
            s.components.clear();for(int i:order)s.components.append(components[i]);
            const auto r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs);
            QVERIFY2(r.complete,qPrintable(QString("order %1%2%3").arg(order[0]).arg(order[1]).arg(order[2])));
            QCOMPARE(r.netAssets,maximum);QCOMPARE(r.knownSubtotal,maximum);
            for(const auto& row:r.rows){QVERIFY(row.included);QVERIFY(!row.value.isEmpty());QVERIFY(row.reason.isEmpty());}
        }while(std::next_permutation(order.begin(),order.end()));
        order={0,1};
        do {
            s.components.clear();for(int i:order)s.components.append(components[i]);
            const auto r=AssetValuation::value({s},{},"KRW",Start,DisplayFreshnessMs);
            QVERIFY(!r.complete);QVERIFY(r.knownSubtotal.isEmpty());QVERIFY(r.netAssets.isEmpty());
            for(const auto& row:r.rows){QVERIFY(row.included);QVERIFY(!row.value.isEmpty());}
        }while(std::next_permutation(order.begin(),order.end()));
    }
    void zeroEquityStillRetainsOpposingFuturesPositionsWithoutAddingTheirNotional() {
        AccountRecord account; account.provider="binance"; account.environment="live"; account.market="usdm_futures";
        MarketSnapshot raw; Balance balance; balance.asset="USDT"; balance.equity="0"; raw.balances.append(balance);
        PositionRecord longPosition; longPosition.symbol="BTCUSDT"; longPosition.positionSide="LONG"; longPosition.amount="2";
        PositionRecord shortPosition=longPosition; shortPosition.positionSide="SHORT"; shortPosition.amount="-2";
        raw.positions={longPosition,shortPosition};
        const auto capture=captureExchangeAssets(account,"FAKE_SCOPE",raw,Start,0,true,true,"synthetic",{});
        QCOMPARE(capture.snapshot.components.size(),3);
        QCOMPARE(capture.snapshot.components[1].quantity,QString("2"));
        QCOMPARE(capture.snapshot.components[2].quantity,QString("-2"));
        const auto result=AssetValuation::value({capture.snapshot},{},"USDT",Start);
        QVERIFY(result.complete); QCOMPARE(result.netAssets,QString("0"));
        QVERIFY(!result.rows[1].included); QVERIFY(!result.rows[2].included);
    }
    void lineBreaksAreAllowedOnlyInCoverage(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault,dir.filePath("assets"));QVERIFY(ledger.open(true));QString error;
        auto s=snapshot("multiline","0");s.coverage="잔고 · 조회 완료\n포지션 · 조회 완료";
        QVERIFY2(ledger.append({s},{},{},&error),qPrintable(error));const auto bytes=read(ledger.filePath());
        auto bad=s;bad.id="bad-id\n";QVERIFY(!ledger.append({bad},{},{}));
        bad=s;bad.id="bad-coverage-cr";bad.coverage="not allowed\r\n";QVERIFY(!ledger.append({bad},{},{}));
        bad=s;bad.id="bad-coverage-tab";bad.coverage="not allowed\t";QVERIFY(!ledger.append({bad},{},{}));
        bad=s;bad.id="bad-evidence";bad.evidence.scope="scope\n";QVERIFY(!ledger.append({bad},{},{}));
        QCOMPARE(read(ledger.filePath()),bytes);ledger.lock();QVERIFY(ledger.open(false));QCOMPARE(ledger.snapshots().first().coverage,s.coverage);
    }
    void encryptedRecordsSurvivePasswordChangeAndLockWithoutInventingFills(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(vault.create(Password));const auto noKey=read(vault.filePath());
        QVERIFY(!vault.withAssetsLedgerKey(false,[](const auto&){}));QCOMPARE(read(vault.filePath()),noKey);
        AssetsLedger ledger(&vault,dir.filePath("assets.bin"));QString error;QVERIFY2(ledger.open(true,&error),qPrintable(error));
        AssetOrderObservation partial;partial.accountScope="FAKE_ACCOUNT_SCOPE_ONLY";partial.provider="fake";partial.market="spot";partial.orderId="FAKE_PARTIAL_ORDER";partial.observedAtMs=Start;partial.side="buy";partial.type="limit";partial.status="partial";partial.price="100";partial.quantity="3";partial.executed="1";partial.quantityUnit="base";partial.currency="KRW";partial.evidence=evidence();
        auto canceled=partial;canceled.status="canceled";canceled.observedAtMs=Start+2000;canceled.evidence=evidence(Start+2000);
        const auto later=snapshot("later","180",Start+2000),earlier=snapshot("earlier","100");
        QVERIFY2(ledger.appendBatch({later,earlier},{assetEvent("deposit",AssetEventKind::ExternalIn,"50")},{},{canceled,partial},&error),qPrintable(error));
        QCOMPARE(ledger.snapshots().first().id,QString("earlier"));QCOMPARE(ledger.orders().last().executed,QString("1"));QCOMPARE(ledger.events().size(),1);
        const auto bytes=read(ledger.filePath());QVERIFY(!bytes.contains("FAKE_ACCOUNT_SCOPE_ONLY"));QVERIFY(!bytes.contains("FAKE_PARTIAL_ORDER"));
        QVERIFY(ledger.appendBatch({earlier},{assetEvent("deposit",AssetEventKind::ExternalIn,"50")},{},{partial}));QCOMPARE(read(ledger.filePath()),bytes);
        auto conflict=earlier;conflict.components[0].quantity="999";QVERIFY(!ledger.append({conflict},{},{}));QCOMPARE(read(ledger.filePath()),bytes);
        QVERIFY(vault.changePassword(Password,NewPassword));QCOMPARE(read(ledger.filePath()),bytes);QSignalSpy locked(&ledger,&AssetsLedger::locked);vault.lock();QCOMPARE(locked.size(),1);QVERIFY(!ledger.isOpen());QVERIFY(ledger.snapshots().isEmpty());
        QVERIFY(vault.unlock(NewPassword));QVERIFY(ledger.open(false));QCOMPARE(ledger.snapshots().size(),2);QCOMPARE(ledger.orders().size(),2);QCOMPARE(read(ledger.filePath()),bytes);
    }
    void tamperingExternalChangesAndConcurrentWritersFailClosed(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(vault.create(Password));AssetsLedger ledger(&vault,dir.filePath("assets.bin"));QVERIFY(ledger.open(true));QVERIFY(ledger.append({snapshot("one","100")},{},{}));
        const auto original=read(ledger.filePath());AssetsLedger concurrent(&vault,ledger.filePath());QVERIFY(!concurrent.open(false));QVERIFY(ledger.isOpen());
        auto corrupt=original;corrupt[corrupt.size()-1]=char(corrupt.back()^1);QVERIFY(write(ledger.filePath(),corrupt));
        QString error;QVERIFY(!ledger.append({snapshot("two","200",Start+1000)},{},{},&error));QVERIFY(!error.isEmpty());QVERIFY(!ledger.isOpen());QCOMPARE(read(ledger.filePath()),corrupt);QVERIFY(!ledger.open(false));
        QVERIFY(write(ledger.filePath(),original));QVERIFY(ledger.open(false));QCOMPARE(ledger.snapshots().size(),1);
        auto invalid=snapshot("bad","0");invalid.components[0].quantity="1\n";QVERIFY(!ledger.append({invalid},{},{}));QCOMPARE(read(ledger.filePath()),original);
        QList<AssetSnapshot> tooMany;for(qsizetype i=0;i<=AssetsLedger::MaxSnapshots;++i)tooMany<<snapshot(QString::number(i),"0");QVERIFY(!ledger.append(tooMany,{},{}));QCOMPARE(read(ledger.filePath()),original);
    }
    void archiveOverflowKeepsOldRecordsAndContinuesWriting(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        QList<AssetSnapshot> rows;for(qsizetype i=0;i<AssetsLedger::MaxSnapshots;++i)rows.append(snapshot(QString::number(i),"1",Start+i));
        QVERIFY(ledger.append(rows,{},{}));QSignalSpy finished(&ledger,&AssetsLedger::writeFinished);QString error;
        QVERIFY2(ledger.appendWithArchive({snapshot("next","2",Start+10000)},{},{},{},&error,true),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,5000);QVERIFY(finished[0][0].toBool());QCOMPARE(ledger.snapshots().size(),1);QCOMPARE(ledger.snapshots()[0].id,QString("next"));
        QCOMPARE(ledger.archiveFiles().size(),1);AssetsLedger archive(&vault,dir.filePath(ledger.archiveFiles()[0]));QVERIFY(archive.open(false));QCOMPARE(archive.snapshots().size(),int(AssetsLedger::MaxSnapshots));
        QCOMPARE(archive.snapshots().first().id,QString("0"));QCOMPARE(archive.snapshots().last().id,QString::number(AssetsLedger::MaxSnapshots-1));
    }
    void asynchronousCurrentCacheClearsPlaintextOnLockAndReopens(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));AssetsLedger latest(&vault,dir.filePath("assets-current.keefi"));QVERIFY(latest.open(true));
        QSignalSpy finished(&latest,&AssetsLedger::writeFinished);QString error;
        QVERIFY(latest.replaceLatest({snapshot("latest","2")},{},&error,true));QVERIFY(latest.isWriting());QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,3000);QVERIFY(finished[0][0].toBool());
        const auto cipher=read(latest.filePath());QVERIFY(!cipher.contains("FAKE_ACCOUNT_SCOPE_ONLY"));QVERIFY(!cipher.contains("components"));
        QVERIFY(latest.replaceLatest({snapshot("after","3",Start+1000)},{},&error,true));vault.lock();QVERIFY(latest.snapshots().isEmpty());QVERIFY(!latest.isOpen());
        QTRY_VERIFY_WITH_TIMEOUT(!latest.isWriting(),3000);QVERIFY(vault.unlock(Password));QVERIFY2(latest.open(false,&error),qPrintable(error));
        QCOMPARE(latest.snapshots().size(),1);const auto id=latest.snapshots()[0].id;QVERIFY(id=="latest"||id=="after");
    }
    void valuationKeepsCashMeaningsAndNeverAssumesFxOrCost(){
        auto s=snapshot("equity","100");s.components[0].kind=AssetComponentKind::Equity;
        auto component=s.components[0];component.id="pnl";component.quantity="20";component.kind=AssetComponentKind::UnrealizedPnl;s.components<<component;
        component.id="notional";component.quantity="1000";component.kind=AssetComponentKind::PositionNotional;s.components<<component;
        component.id="buying";component.quantity="200";component.kind=AssetComponentKind::BuyingPower;s.components<<component;
        auto r=AssetValuation::value({s},{},"KRW",Start);QVERIFY(r.complete);QCOMPARE(r.netAssets,QString("100"));
        component.id="funding-cash";component.quantity="30";component.kind=AssetComponentKind::Cash;component.valuationGroup="funding";s.components<<component;
        r=AssetValuation::value({s},{},"KRW",Start);QVERIFY(r.complete);QCOMPARE(r.netAssets,QString("130"));
        s=snapshot("holding","2");s.components[0].asset="005930";s.components[0].kind=AssetComponentKind::Holding;s.components[0].reportedValue="140000";s.components[0].reportedValueCurrency="KRW";s.components[0].providerUnrealizedPnl="1000";
        r=AssetValuation::value({s},{},"KRW",Start);QCOMPARE(r.netAssets,QString("140000"));QVERIFY(r.rows.first().unrealizedPnl.isEmpty());QCOMPARE(r.rows.first().providerUnrealizedPnl,QString("1000"));
        s=snapshot("stablecoin","100");s.components[0].asset="USDT";r=AssetValuation::value({s},{},"USD",Start);QVERIFY(!r.complete);QVERIFY(r.netAssets.isEmpty());
        AssetQuote quote{"USDT","KRW","1400",publicEvidence("USDT")};r=AssetValuation::value({s},{quote},"KRW",Start);QCOMPARE(r.netAssets,QString("140000"));
        s.balanceComplete=false;r=AssetValuation::value({s},{quote},"KRW",Start);QVERIFY(!r.complete);QCOMPARE(r.knownSubtotal,QString("140000"));
        s.balanceComplete=true;r=AssetValuation::value({s},{quote},"KRW",Start+60001);QVERIFY(!r.complete);
    }
    void performanceRequiresCompleteFlowsAndShowsRateAmbiguity(){
        const auto opening=AssetValuation::value({snapshot("start","100")},{},"KRW",Start);
        const auto closing=AssetValuation::value({snapshot("end","180",Start+3000)},{},"KRW",Start+3000);
        const QStringList scopes{"FAKE_ACCOUNT_SCOPE_ONLY"};
        auto flows=AssetValuation::flows({assetEvent("deposit",AssetEventKind::ExternalIn,"50"),assetEvent("fee",AssetEventKind::Fee,"2"),assetEvent("funding",AssetEventKind::Funding,"-1")},scopes,{},"KRW",Start,Start+3000,true);
        AssetPerformanceCoverage coverage{Start,Start+3000,scopes,true,true,false};auto performance=AssetValuation::performance(opening,closing,flows,coverage);QVERIFY(performance.complete);QCOMPARE(performance.investmentPnl,QString("30"));
        auto transfer=assetEvent("out",AssetEventKind::TransferOut,"10");transfer.transferReference="FAKE_CHAIN_TRANSACTION";transfer.peerAccountScope="FAKE_SECOND_SCOPE";
        auto incoming=transfer;incoming.id="in";incoming.officialId="in";incoming.kind=AssetEventKind::TransferIn;incoming.accountScope=transfer.peerAccountScope;incoming.peerAccountScope=transfer.accountScope;
        auto paired=AssetValuation::flows({transfer,incoming,assetEvent("network-fee",AssetEventKind::Fee,"1")},{scopes.first(),"FAKE_SECOND_SCOPE"},{},"KRW",Start,Start+3000,true);QVERIFY(paired.complete);QCOMPARE(paired.netExternal,QString("0"));QCOMPARE(paired.internalEventIds.size(),2);
        paired=AssetValuation::flows({transfer},{scopes.first(),"FAKE_SECOND_SCOPE"},{},"KRW",Start,Start+3000,true);QVERIFY(!paired.complete);QVERIFY(paired.netExternal.isEmpty());
        coverage.hasGaps=true;QVERIFY(!AssetValuation::performance(opening,closing,flows,coverage).complete);coverage.hasGaps=false;
        auto twr=AssetValuation::twr({{Start,"100","100",true},{Start+3000,"180","180",true}},flows,coverage);QCOMPARE(twr.state,AssetRateState::InsufficientData);
        twr=AssetValuation::twr({{Start,"100","100",true},{Start+1000,"110","160",true},{Start+3000,"180","180",true}},flows,coverage);QCOMPARE(twr.state,AssetRateState::Available);QVERIFY(!twr.annualized);QVERIFY(std::abs(twr.rate.toDouble()-0.2375)<1e-10);
        auto mwr=AssetValuation::mwr({{"a","-100",Start},{"b","110",Start+Year}},true);QCOMPARE(mwr.state,AssetRateState::Available);QVERIFY(mwr.annualized);QVERIFY(std::abs(mwr.rate.toDouble()-0.1)<1e-9);
        mwr=AssetValuation::mwr({{"a","100",Start},{"b","110",Start+Year}},true);QCOMPARE(mwr.state,AssetRateState::NoSolution);QVERIFY(mwr.rate.isEmpty());
        mwr=AssetValuation::mwr({{"a","-100",Start},{"b","230",Start+Year},{"c","-132",Start+2*Year}},true);QCOMPARE(mwr.state,AssetRateState::MultipleSolutions);QVERIFY(mwr.rate.isEmpty());
        mwr=AssetValuation::mwr({{"a","-100",Start},{"b","1",Start+Year},{"c","-1",Start+2*Year}},true);QCOMPARE(mwr.state,AssetRateState::UniquenessUnverified);QVERIFY(mwr.rate.isEmpty());
    }
};
}
QTEST_GUILESS_MAIN(keefi::AssetsTests)
#include "tst_assets.moc"
