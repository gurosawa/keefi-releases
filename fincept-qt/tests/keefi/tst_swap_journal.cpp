#include "keefi/SwapJournal.h"
#include "keefi/AssetsLedger.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/Vault.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
namespace keefi {
namespace {
const QString Password="Fake swap Vault password 2026!",OtherPassword="Other fake Swap password 2026!";
SwapPendingRecord record(){
    SwapPendingRecord r;r.id="00000000-0000-4000-8000-000000000001";r.phantomAccountId="00000000-0000-4000-8000-000000000002";r.jupiterAccountId="00000000-0000-4000-8000-000000000003";r.credentialRef="00000000-0000-4000-8000-000000000004";
    r.chain="solana-mainnet";r.wallet=SolanaTransactionCodec::encodeBase58(QByteArray(32,'x'));r.inputMint=SolMint;r.outputMint=UsdcMint;r.inputAtomic="1000000";r.minimumRouteOutput="500";r.slippageBps="50";
    r.requestId="FAKE_REQUEST_ONLY";r.transactionId=SolanaTransactionCodec::encodeBase58(QByteArray(64,'z'));r.messageHash=QString(64,'a');r.lastValidBlockHeight="100";r.createdAt="1792200000000";r.state="signed_not_submitted";return r;
}
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
bool write(const QString& path,const QByteArray& bytes){QFile f(path);return f.open(QIODevice::WriteOnly|QIODevice::Truncate)&&f.write(bytes)==bytes.size()&&f.flush();}
}
class SwapJournalTests final:public QObject {
    Q_OBJECT
  private slots:
    void immutableSignedIntentReopensAndFinalAmountsStayUnknownUntilObserved(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(vault.create(Password));const auto path=dir.filePath("swap.bin");SwapJournal journal(&vault,path);QVERIFY(journal.open(true));auto r=record();QString error;
        QVERIFY2(journal.upsert(r,&error),qPrintable(error));const auto initial=read(path);QVERIFY(!initial.contains("FAKE_REQUEST_ONLY"));QVERIFY(!initial.contains(r.wallet.toUtf8()));QVERIFY(journal.upsert(r));QCOMPARE(read(path),initial);
        auto changed=r;changed.minimumRouteOutput="499";QVERIFY(!journal.upsert(changed));changed=r;changed.wallet=SolMint;QVERIFY(!journal.upsert(changed));QCOMPARE(read(path),initial);
        r.state="submitted_unknown";QVERIFY(journal.upsert(r));changed=r;changed.state="signed_not_submitted";QVERIFY(!journal.upsert(changed));
        r.state="confirmed";r.confirmedSlot="200";QVERIFY(journal.upsert(r));r.state="finalized";QVERIFY(journal.upsert(r));QVERIFY(journal.records().first().actualOutputAtomic.isEmpty());
        // A later official transaction response may fill an unknown field, never rewrite a known value.
        r.actualFeeLamports="5000";QVERIFY(journal.upsert(r));changed=r;changed.actualFeeLamports="0";QVERIFY(!journal.upsert(changed));changed=r;changed.state="submitted_unknown";QVERIFY(!journal.upsert(changed));
        const auto saved=read(path);QVERIFY(vault.changePassword(Password,OtherPassword));QCOMPARE(read(path),saved);vault.lock();QVERIFY(journal.records().isEmpty());QVERIFY(vault.unlock(OtherPassword));QVERIFY(journal.open(false));QCOMPARE(journal.records().first(),r);
        auto invalid=record();invalid.id="00000000-0000-4000-8000-000000000005";QVERIFY(!journal.upsert(invalid)); // Same signed transaction cannot have another local ID.
        invalid=record();invalid.chain="devnet";QVERIFY(!journal.upsert(invalid));invalid=record();invalid.inputAtomic="18446744073709551616";QVERIFY(!journal.upsert(invalid));
        invalid=record();invalid.state="confirmed";QVERIFY(!journal.upsert(invalid));invalid=record();invalid.messageHash="FAKE_NOT_A_HASH";QVERIFY(!journal.upsert(invalid));QCOMPARE(read(path),saved);
    }
    void distinctDomainTamperAndConcurrentSaveNeverReplaceExistingData(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault.bin"));QVERIFY(vault.create(Password));AssetsLedger assets(&vault,dir.filePath("assets.bin"));QVERIFY(assets.open(true));assets.lock();
        const auto originalAssets=read(assets.filePath());SwapJournal wrongKind(&vault,assets.filePath());QVERIFY(!wrongKind.open(false));QCOMPARE(read(assets.filePath()),originalAssets);
        const auto path=dir.filePath("swap.bin");SwapJournal journal(&vault,path);QVERIFY(journal.open(true));QVERIFY(journal.upsert(record()));const auto original=read(path);
        SwapJournal concurrent(&vault,path);QVERIFY(!concurrent.open(false));QVERIFY(journal.isOpen());auto corrupt=original;corrupt[corrupt.size()-1]=char(corrupt.back()^1);QVERIFY(write(path,corrupt));
        auto updated=record();updated.state="submitted_unknown";QVERIFY(!journal.upsert(updated));QVERIFY(!journal.isOpen());QCOMPARE(read(path),corrupt);QVERIFY(!journal.open(false));QVERIFY(write(path,original));QVERIFY(journal.open(false));QCOMPARE(journal.records().first(),record());
    }
};
}
QTEST_GUILESS_MAIN(keefi::SwapJournalTests)
#include "tst_swap_journal.moc"
