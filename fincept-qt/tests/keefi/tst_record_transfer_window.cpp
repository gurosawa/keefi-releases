#include "keefi/RecordTransferDialog.h"
#include "keefi/RecordTransfer.h"
#include "keefi/AssetsLedger.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
using namespace keefi;
namespace {
const QString Password=QStringLiteral("Fake Transfer Password! 2026");
PortfolioController::Factories offlineFactories(){PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[](const AccountRecord&){return static_cast<TradingTransport*>(nullptr);};return f;}
QByteArray bytes(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
QPushButton* button(RecordTransferDialog& dialog,const char* name){return dialog.findChild<QPushButton*>(name);}
QLabel* status(RecordTransferDialog& dialog){return dialog.findChild<QLabel*>("recordTransferStatus");}
void input(RecordTransferDialog& dialog,const char* name,const QString& value){dialog.findChild<QLineEdit*>(name)->setText(value);}
void exportInput(RecordTransferDialog& dialog,const QString& path){input(dialog,"exportRecordPath",path);input(dialog,"exportRecordPassword",Password);input(dialog,"exportRecordConfirm",Password);}
void importInput(RecordTransferDialog& dialog,const QString& path){input(dialog,"importRecordPath",path);input(dialog,"importRecordPassword",Password);}
bool screenshot(RecordTransferDialog& dialog,const QString& name,int width){
    const auto output=qEnvironmentVariable("KEEFI_RECORD_TRANSFER_SCREENSHOT_DIR");if(output.isEmpty())return true;
    const QDir directory(output);if(!directory.exists())return false;
    dialog.resize(width,760);QTest::qWait(30);return dialog.grab().save(directory.filePath(name));
}
bool seed(AssetsLedger* ledger,int count){
    if(!ledger->open(true))return false;QList<AssetSnapshot> snapshots;
    for(int i=0;i<count;++i){AssetSnapshot s;s.id=QString("fake-snapshot-%1").arg(i);s.accountScope="FAKE_CREDENTIAL_SCOPE_NEVER_PORTABLE";s.provider="binance";s.environment="live";s.market="spot";s.timeMs=1700000000000LL+i;s.balanceComplete=s.reconciled=true;s.evidence={"FAKE_OFFICIAL","https://example.invalid","fake",s.timeMs,s.timeMs,true};AssetComponent c;c.id=c.asset="BTC";c.quantity="2";c.evidence=s.evidence;s.components.append(c);snapshots.append(s);}
    return ledger->append(snapshots,{},{});
}
bool package(Vault& vault,AssetsLedger* ledger,const QString& path,int count=1){
    if(!seed(ledger,count))return false;RecordTransfer transfer(vault);RecordTransferBundle bundle;QByteArray ciphertext;
    if(!transfer.initialize()||!transfer.collect({},ledger,nullptr,&bundle))return false;
    auto password=SecureBuffer::fromUtf8(Password);
    return RecordTransfer::encodePackage(bundle,password,&ciphertext)&&RecordTransfer::writePackageFile(path,ciphertext);
}
}
class RecordTransferWindowTest final:public QObject {
    Q_OBJECT
private slots:
    void prepareAndImportRequireExplicitButtons(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        RecordTransfer probe(vault);const auto vaultBefore=bytes(vault.filePath());RecordTransferDialog dialog(vault,nullptr,nullptr);dialog.openRecords();
        QVERIFY(!probe.exists());QCOMPARE(bytes(vault.filePath()),vaultBefore);QVERIFY(!button(dialog,"submitRecordImport")->isEnabled());
        input(dialog,"importRecordPath",dir.filePath("missing.keefirtx"));input(dialog,"importRecordPassword",Password);
        QVERIFY(!probe.exists());QVERIFY(!button(dialog,"submitRecordImport")->isEnabled());
        button(dialog,"prepareRecordTransfer")->click();QVERIFY(probe.exists());QVERIFY(button(dialog,"submitRecordImport")->isEnabled());
        auto* label=dialog.findChild<QLabel*>("recordTransferScope");QCOMPARE(label->textFormat(),Qt::PlainText);
        QVERIFY(label->text().contains("시세 캐시"));QVERIFY(label->text().contains("수동 보정"));
        dialog.close();QVERIFY(dialog.findChild<QLineEdit*>("importRecordPassword")->text().isEmpty());vault.lock();
    }
    void uiRoundTripPaginatesAndShowsDuplicatesWithoutChangingActiveState(){
        QTemporaryDir sourceDir,destinationDir;Vault source(sourceDir.filePath("vault")),destination(destinationDir.filePath("vault"));
        QVERIFY(source.create(Password));QVERIFY(destination.create(Password));
        AccountRecord existing;existing.provider="jupiter";existing.alias="fake existing connection";existing.environment="mainnet";existing.market="solana";existing.currency="SOL";
        QVERIFY(destination.addAccount(existing,{{"api_key","fake_jupiter_transfer_test_key"}},StorageMode::EncryptedVault));
        const QJsonObject existingRequest{{"id","fake-existing-active"},{"accountId","fake-binance-account"},{"fingerprint",QString(64,'a')},
            {"provider","binance"},{"environment","testnet"},{"symbol","BTCUSDT"},{"kind","place"},{"clientId","fake-active-client"},
            {"orderId",""},{"replacementClientId",""},{"state","unknown"},{"createdAt","1792200000000"},{"quantity","0.1"},{"price","100"},{"quoteAmount",""},{"side","buy"},{"type","limit"}};
        QVERIFY(destination.setPendingOperations({existingRequest}));
        PortfolioController sourcePortfolio(source,nullptr,offlineFactories()),destinationPortfolio(destination,nullptr,offlineFactories());
        QVERIFY(seed(sourcePortfolio.historyLedger(),205));RecordTransferDialog exporting(source,&sourcePortfolio,nullptr);exporting.openRecords();button(exporting,"prepareRecordTransfer")->click();
        const auto path=sourceDir.filePath("new.keefirtx");exportInput(exporting,path);QVERIFY(button(exporting,"submitRecordExport")->isEnabled());button(exporting,"submitRecordExport")->click();
        QVERIFY(exporting.findChild<QLineEdit*>("exportRecordPassword")->text().isEmpty());QVERIFY(exporting.findChild<QLineEdit*>("exportRecordConfirm")->text().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(status(exporting)->text().contains("내보내기를 완료"),10000);QVERIFY(QFile::exists(path));QVERIFY(status(exporting)->text().contains("205개"));
        auto* details=exporting.findChild<QPlainTextEdit*>("recordTransferDetails");QVERIFY(details->toPlainText().contains("스냅샷 205"));QVERIFY(details->toPlainText().contains("과거 보관"));
        button(exporting,"recordTransferDetailsToggle")->click();QVERIFY(screenshot(exporting,"record-transfer-export-820.png",820));QVERIFY(details->height()<=104);
        const auto originalPackage=bytes(path);exportInput(exporting,path);button(exporting,"submitRecordExport")->click();QVERIFY(status(exporting)->text().contains("덮어쓸 수 없습니다"));QCOMPARE(bytes(path),originalPackage);
        RecordTransferDialog importing(destination,&destinationPortfolio,nullptr);importing.openRecords();button(importing,"prepareRecordTransfer")->click();
        const auto destinationVault=bytes(destination.filePath());const auto active=destination.pendingOperations();const auto accounts=destination.accounts().size();
        importInput(importing,path);auto* table=importing.findChild<QTableWidget*>("importedRecordTable");QCOMPARE(table->rowCount(),0);
        button(importing,"submitRecordImport")->click();QVERIFY(importing.findChild<QLineEdit*>("importRecordPassword")->text().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(status(importing)->text().contains("추가 205"),10000);QCOMPARE(table->rowCount(),200);QCOMPARE(table->editTriggers(),QAbstractItemView::NoEditTriggers);QCOMPARE(table->item(0,0)->text(),QString("자산 스냅샷"));
        QVERIFY(screenshot(importing,"record-transfer-import-820.png",820));QVERIFY(screenshot(importing,"record-transfer-import-1080.png",1080));
        QVERIFY(table->item(0,9)->text()!="FAKE_CREDENTIAL_SCOPE_NEVER_PORTABLE");QCOMPARE(table->item(0,9)->text().size(),64);
        button(importing,"nextRecordPage")->click();QCOMPARE(table->rowCount(),5);QVERIFY(!button(importing,"nextRecordPage")->isEnabled());button(importing,"previousRecordPage")->click();QCOMPARE(table->rowCount(),200);
        importInput(importing,path);button(importing,"submitRecordImport")->click();QTRY_VERIFY_WITH_TIMEOUT(status(importing)->text().contains("추가 0 · 중복 205 · 총 205"),10000);
        QCOMPARE(destination.pendingOperations(),active);QCOMPARE(destination.accounts().size(),accounts);QCOMPARE(bytes(destination.filePath()),destinationVault);QVERIFY(destinationPortfolio.snapshots().isEmpty());
        destination.lock();QCOMPARE(table->rowCount(),0);source.lock();
    }
    void cancellationAndLockDiscardLateDecodeAndClearPasswords(){
        QTemporaryDir sourceDir,destinationDir;Vault source(sourceDir.filePath("vault")),destination(destinationDir.filePath("vault"));QVERIFY(source.create(Password));QVERIFY(destination.create(Password));
        PortfolioController portfolio(source,nullptr,offlineFactories());const auto path=sourceDir.filePath("cancel.keefirtx");QVERIFY(package(source,portfolio.historyLedger(),path));
        RecordTransferDialog dialog(destination,nullptr,nullptr);dialog.openRecords();button(dialog,"prepareRecordTransfer")->click();RecordTransfer probe(destination);const auto before=bytes(probe.filePath());
        importInput(dialog,path);button(dialog,"submitRecordImport")->click();button(dialog,"cancelRecordTransfer")->click();
        QVERIFY(dialog.findChild<QLineEdit*>("importRecordPassword")->text().isEmpty());QCOMPARE(dialog.findChild<QTableWidget*>("importedRecordTable")->rowCount(),0);
        // Allow the real KDF worker to return through the queued completion path.
        QTest::qWait(1000);QCOMPARE(bytes(probe.filePath()),before);QVERIFY(status(dialog)->text().contains("취소"));
        button(dialog,"prepareRecordTransfer")->click();importInput(dialog,path);button(dialog,"submitRecordImport")->click();destination.lock();
        QTest::qWait(1000);QCOMPARE(bytes(probe.filePath()),before);QCOMPARE(dialog.findChild<QTableWidget*>("importedRecordTable")->rowCount(),0);
        QVERIFY(dialog.findChild<QLineEdit*>("importRecordPassword")->text().isEmpty());QVERIFY(!button(dialog,"submitRecordImport")->isEnabled());source.lock();
    }
    void closeWhileWorkerRunsKeepsJobLifetimeIndependent(){
        QTemporaryDir sourceDir,destinationDir;Vault source(sourceDir.filePath("vault")),destination(destinationDir.filePath("vault"));QVERIFY(source.create(Password));QVERIFY(destination.create(Password));
        PortfolioController portfolio(source,nullptr,offlineFactories());const auto path=sourceDir.filePath("close.keefirtx");QVERIFY(package(source,portfolio.historyLedger(),path));
        auto dialog=std::make_unique<RecordTransferDialog>(destination,nullptr,nullptr);dialog->openRecords();button(*dialog,"prepareRecordTransfer")->click();RecordTransfer probe(destination);const auto before=bytes(probe.filePath());
        importInput(*dialog,path);button(*dialog,"submitRecordImport")->click();dialog.reset();QTest::qWait(1000);QCOMPARE(bytes(probe.filePath()),before);QVERIFY(destination.isUnlocked());destination.lock();source.lock();
    }
    void exportCancellationReportsWhetherCiphertextRemains(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));PortfolioController portfolio(vault,nullptr,offlineFactories());QVERIFY(seed(portfolio.historyLedger(),1));
        const auto original=bytes(portfolio.historyLedger()->filePath());
        for(int boundary=0;boundary<3;++boundary){
            if(!vault.isUnlocked())QVERIFY(vault.unlock(Password));RecordTransferDialog dialog(vault,&portfolio,nullptr);dialog.openRecords();button(dialog,"prepareRecordTransfer")->click();
            const auto path=dir.filePath(QString("cancel-export-%1.keefirtx").arg(boundary));exportInput(dialog,path);button(dialog,"submitRecordExport")->click();
            if(boundary==0)button(dialog,"cancelRecordTransfer")->click();else if(boundary==1)vault.lock();else dialog.close();
            QVERIFY(dialog.findChild<QLineEdit*>("exportRecordPassword")->text().isEmpty());QTest::qWait(1000);
            if(QFile::exists(path)){
                QVERIFY(status(dialog)->text().contains("파일이 남아"));const auto ciphertext=bytes(path);QVERIFY(ciphertext.startsWith("KEEFIRTX"));QVERIFY(!ciphertext.contains("FAKE_CREDENTIAL_SCOPE"));
                RecordTransferBundle bundle;auto password=SecureBuffer::fromUtf8(Password);QVERIFY(RecordTransfer::decodePackage(ciphertext,password,&bundle));QCOMPARE(bundle.records.size(),1);
            }else QVERIFY(status(dialog)->text().contains("파일이 없습니다"));
            QCOMPARE(bytes(portfolio.historyLedger()->filePath()),original);
        }
        vault.lock();
    }
    void exportPolicyAndUnselectedScopeRemainExplicit(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));PortfolioController portfolio(vault,nullptr,offlineFactories());QVERIFY(seed(portfolio.historyLedger(),1));
        RecordTransferDialog dialog(vault,&portfolio,nullptr);dialog.openRecords();button(dialog,"prepareRecordTransfer")->click();input(dialog,"exportRecordPath",dir.filePath("events-only.keefirtx"));input(dialog,"exportRecordPassword","weak");input(dialog,"exportRecordConfirm","weak");QVERIFY(!button(dialog,"submitRecordExport")->isEnabled());
        exportInput(dialog,dir.filePath("events-only.keefirtx"));dialog.findChild<QCheckBox*>("transferSnapshots")->setChecked(false);dialog.findChild<QCheckBox*>("transferOrders")->setChecked(false);dialog.findChild<QCheckBox*>("transferSwaps")->setChecked(false);dialog.findChild<QCheckBox*>("transferPending")->setChecked(false);
        button(dialog,"submitRecordExport")->click();QTRY_VERIFY_WITH_TIMEOUT(status(dialog)->text().contains("내보내기를 완료"),10000);
        RecordTransferBundle bundle;auto password=SecureBuffer::fromUtf8(Password);QVERIFY(RecordTransfer::decodePackage(bytes(dir.filePath("events-only.keefirtx")),password,&bundle));QVERIFY(bundle.records.isEmpty());
        QCOMPARE(status(dialog)->textFormat(),Qt::PlainText);vault.lock();
    }
};
QTEST_MAIN(RecordTransferWindowTest)
#include "tst_record_transfer_window.moc"
