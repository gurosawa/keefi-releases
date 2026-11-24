#include "keefi/OkxFuturesReadController.h"
#include "keefi/AssetsLedger.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/Vault.h"
#include "fixtures/OkxResponses.h"
#include <QDateTime>
#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

using namespace keefi;
using namespace keefi::test;
namespace {
class FuturesReadTransport final : public TradingTransport {
public:
    QList<RequestSpec> requests;
    QString accountMode="2",uid="123456789";
    std::function<void(const RequestSpec&,HttpResult&)> customize;
    std::function<void(const RequestSpec&)> beforeReply;
    QSet<Operation> heldOperations;
    struct Pending {quint64 token;HttpResult response;};
    QList<Pending> held;
    void abort() override { ++generation; }
    void send(quint64 token,const RequestSpec& request) override {
        requests.append(request);
        const auto family=QUrlQuery(request.url).queryItemValue("instType");
        const auto now=QString::number(QDateTime::currentMSecsSinceEpoch());
        auto response=okxResponse(request);
        if(request.operation==Operation::AccountConfig)response=okxJson({QJsonObject{{"uid",uid},{"acctLv",accountMode},{"perm","read_only"}}});
        if(request.operation==Operation::Account)response=okxJson({QJsonObject{{"uTime",now},{"details",QJsonArray{
            QJsonObject{{"ccy","USDT"},{"eq","110"},{"cashBal","100"},{"upl","10"},{"uTime",now},
                {"availBal","80"},{"frozenBal","20"},{"liab","0"}}}}}});
        if(request.operation==Operation::FundingBalance)response=okxJson({QJsonObject{{"ccy","USDT"},{"bal","5"},{"availBal","5"},{"frozenBal","0"}}});
        if(request.operation==Operation::Positions)response=okxJson({QJsonObject{
            {"instType",family},{"instId",family=="SWAP"?"BTC-USDT-SWAP":"ETH-USDT-261225"},
            {"posId",family=="SWAP"?"101":"102"},{"posSide",family=="SWAP"?"net":"long"},
            {"pos",family=="SWAP"?"-2":"3"},{"avgPx","100"},{"markPx","110"},{"liqPx",""},
            {"upl",family=="SWAP"?"-2":"12"},{"notionalUsd","200"},{"ccy","USDT"},
            {"lever","3"},{"mgnMode","cross"},{"margin","0"},{"imr","5"},{"mmr","1"},{"uTime",now}}});
        if(customize)customize(request,response);
        if(heldOperations.contains(request.operation)){held.append({token,response});return;}
        const auto epoch=generation;
        QTimer::singleShot(1,this,[this,epoch,token,response,request]{if(epoch==generation){
            if(beforeReply)beforeReply(request);emit finished(token,response);}});
    }
    void releaseOldest() {const auto pending=held.takeFirst();emit finished(pending.token,pending.response);}
private:
    quint64 generation=0;
};
QString addFakeAccount(Vault& vault,QString market="okx_futures") {
    auto a=okxAccount();a.id.clear();a.market=market;
    if(!vault.addAccount(a,{{"api_key","FakeOkxReadKey"},{"secret","FakeOkxReadSecret"},{"passphrase","FakeOkxReadPass"}},StorageMode::EncryptedVault))return {};
    return vault.accounts().last().id;
}
}
class OkxFuturesReadTest final : public QObject {
    Q_OBJECT
private slots:
    void readsUnifiedEquityAndBothDerivativeFamiliesWithoutWrites() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake-okx-futures.bin"));
        QVERIFY(vault.create("Fake OKX futures password! 2026"));
        auto a=okxAccount();a.id.clear();a.market="okx_futures";
        QVERIFY(vault.addAccount(a,{{"api_key","FakeOkxReadKey"},{"secret","FakeOkxReadSecret"},{"passphrase","FakeOkxReadPass"}},StorageMode::EncryptedVault));
        FuturesReadTransport transport;OkxFuturesReadController controller(vault,&transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),3000);
        QVERIFY2(controller.connected(),qPrintable(controller.status()));
        QCOMPARE(transport.requests.size(),6);
        const auto capture=controller.assetCapture();QVERIFY(capture.snapshot.balanceComplete);
        QCOMPARE(capture.snapshot.market,QString("okx_unified"));
        QCOMPARE(controller.positions().size(),2);
        QCOMPARE(controller.positions()[0].amount,QString("-2"));
        QCOMPARE(controller.positions()[1].amount,QString("3"));
        int equity=0,funding=0,positions=0;
        for(const auto& c:capture.snapshot.components){
            if(c.kind==AssetComponentKind::Equity){++equity;QCOMPARE(c.quantity,QString("110"));QCOMPARE(c.providerUnrealizedPnl,QString("10"));}
            if(c.kind==AssetComponentKind::Holding){++funding;QCOMPARE(c.quantity,QString("5"));}
            if(c.kind==AssetComponentKind::PositionNotional)++positions;
        }
        QCOMPARE(equity,1);QCOMPARE(funding,1);QCOMPARE(positions,2);
        for(const auto& r:transport.requests){
            QCOMPARE(r.method,QByteArray("GET"));QVERIFY(r.body.isEmpty());QVERIFY(!isMutation(r.operation));
            QVERIFY2(NetworkTradingTransport::allowed(r),qPrintable(r.url.toString()));
            if(r.authenticated){
                const auto target=r.url.path(QUrl::FullyEncoded).toUtf8()+(r.url.hasQuery()?"?"+r.url.query(QUrl::FullyEncoded).toUtf8():QByteArray{});
                const auto preimage=r.headers.value("OK-ACCESS-TIMESTAMP")+r.method+target;
                QCOMPARE(r.headers.value("OK-ACCESS-SIGN"),QMessageAuthenticationCode::hash(preimage,"FakeOkxReadSecret",QCryptographicHash::Sha256).toBase64());
            }
        }
        const auto value=AssetValuation::value({capture.snapshot},{},"USDT",QDateTime::currentMSecsSinceEpoch());
        QVERIFY(value.complete);QCOMPARE(value.netAssets,QString("115"));
        QVERIFY(vault.pendingOperations().isEmpty());
    }
    void spotOnlyModeSkipsDerivativesAndSameUidKeepsOneUnifiedScope() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto spot=addFakeAccount(vault,"spot"),future=addFakeAccount(vault);QVERIFY(!spot.isEmpty());QVERIFY(!future.isEmpty());
        FuturesReadTransport first,second;first.accountMode="1";
        OkxFuturesReadController a(vault,&first),b(vault,&second);
        QVERIFY(a.selectAccount(spot));QVERIFY(b.selectAccount(future));QVERIFY(a.refreshAssets());QVERIFY(b.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!a.busy()&&!b.busy(),3000);QVERIFY(a.connected());QVERIFY(b.connected());
        QCOMPARE(first.requests.size(),4);QVERIFY(a.positions().isEmpty());
        QCOMPARE(a.assetCapture().snapshot.accountScope,b.assetCapture().snapshot.accountScope);
        second.uid="987654321";QVERIFY(b.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!b.busy(),3000);QVERIFY(b.connected());
        QVERIFY(a.assetCapture().snapshot.accountScope!=b.assetCapture().snapshot.accountScope);
    }
    void timeRttBudget_data() {
        QTest::addColumn<qint64>("rtt");QTest::addColumn<bool>("allowed");
        QTest::newRow("exactly-two-seconds")<<qint64(2000)<<true;
        QTest::newRow("over-two-seconds")<<qint64(2001)<<false;
        QTest::newRow("negative-clock")<<qint64(-1)<<false;
    }
    void timeRttBudget() {
        QFETCH(qint64,rtt);QFETCH(bool,allowed);
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);QVERIFY(!id.isEmpty());FuturesReadTransport transport;
        qint64 timing=10000;transport.beforeReply=[&](const RequestSpec& r){if(r.operation==Operation::Time)timing+=rtt;};
        OkxFuturesReadController c(vault,&transport,nullptr,[&]{return timing;});QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);QCOMPARE(c.connected(),allowed);
        QCOMPARE(transport.requests.size(),allowed?6:1);if(!allowed)QVERIFY(c.assetCapture().snapshot.id.isEmpty());
    }
    void unknownAccountModeFailsClosed() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;t.accountMode="5";
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(!c.connected());QVERIFY(c.assetCapture().snapshot.id.isEmpty());QCOMPARE(t.requests.size(),2);
    }
    void verifiedIdentityIsPublishedBeforeLaterReadFailure() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;OkxFuturesReadController c(vault,&t);
        QSignalSpy identities(&c,&OkxFuturesReadController::identityChanged);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);QVERIFY(c.connected());QCOMPARE(identities.size(),1);
        QVERIFY(identities[0][0].toString().isEmpty());QCOMPARE(identities[0][1].toString(),c.assetCapture().snapshot.accountScope);
        const auto firstScope=c.assetCapture().snapshot.accountScope;QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QCOMPARE(identities.size(),1);
        t.uid="987654321";t.customize=[](const RequestSpec& r,HttpResult& response){if(r.operation==Operation::Positions)response.status=503;};
        QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);QVERIFY(!c.connected());
        QCOMPARE(identities.size(),2);QCOMPARE(identities[1][0].toString(),firstScope);QVERIFY(identities[1][1].toString()!=firstScope);
        QVERIFY(c.assetCapture().snapshot.id.isEmpty());
    }
    void sourceUpdateTimesDoNotMakeUnchangedBalancesStale() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;const qint64 updateTime=1600000000000LL;
        t.customize=[&](const RequestSpec& r,HttpResult& response){
            auto object=QJsonDocument::fromJson(response.body).object();auto rows=object.value("data").toArray();
            if(r.operation==Operation::Account){auto account=rows[0].toObject();account["uTime"]=QString::number(updateTime);auto details=account.value("details").toArray();auto row=details[0].toObject();row["uTime"]=QString::number(updateTime-1000);details[0]=row;account["details"]=details;rows[0]=account;}
            if(r.operation==Operation::Positions){auto row=rows[0].toObject();row["uTime"]=QString::number(updateTime-2000);rows[0]=row;}
            object["data"]=rows;response.body=QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(c.connected());const auto capture=c.assetCapture();QCOMPARE(capture.snapshot.evidence.sourceTimeMs,qint64(0));
        for(const auto& component:capture.snapshot.components){
            if(component.kind==AssetComponentKind::Equity)QCOMPARE(component.evidence.sourceTimeMs,qint64(0));
            if(component.kind==AssetComponentKind::PositionNotional)QCOMPARE(component.evidence.sourceTimeMs,qint64(0));
        }
        QCOMPARE(c.positions().first().updateTime,QString::number(updateTime-2000));
        const auto value=AssetValuation::value({capture.snapshot},{},"USDT",QDateTime::currentMSecsSinceEpoch());
        QVERIFY(value.complete);QCOMPARE(value.netAssets,QString("115"));
    }
    void unavailablePositionCurrencyAndOptionalMarginFieldsRemainUnknown() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;t.accountMode="3";
        t.customize=[](const RequestSpec& r,HttpResult& response){if(r.operation!=Operation::Positions)return;
            auto object=QJsonDocument::fromJson(response.body).object();auto rows=object.value("data").toArray();auto row=rows[0].toObject();
            for(const auto* field:{"ccy","lever","imr","mmr","margin"})row[QLatin1String(field)]="";
            rows[0]=row;object["data"]=rows;response.body=QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY2(c.connected(),qPrintable(c.status()));QCOMPARE(c.positions().size(),2);
        for(const auto& position:c.positions()){QVERIFY(position.marginAsset.isEmpty());QVERIFY(position.leverage.isEmpty());QVERIFY(position.initialMargin.isEmpty());}
        QCOMPARE(c.positions().first().unrealizedPnl,QString("-2"));
        const auto capture=c.assetCapture();for(const auto& part:capture.snapshot.components)if(part.kind==AssetComponentKind::PositionNotional){
            QVERIFY(part.reportedValueCurrency.isEmpty());QVERIFY(part.providerUnrealizedPnl.isEmpty());QVERIFY(part.reportedValue.isEmpty());}
        const auto value=AssetValuation::value({capture.snapshot},{},"USDT",QDateTime::currentMSecsSinceEpoch());QVERIFY(value.complete);QCOMPARE(value.netAssets,QString("115"));
        const auto path=dir.filePath("synthetic-assets.bin");QString error;AssetsLedger ledger(&vault,path);
        QVERIFY2(ledger.open(true,&error),qPrintable(error));QVERIFY2(ledger.append({capture.snapshot},{},{},&error),qPrintable(error));ledger.lock();
        AssetsLedger restored(&vault,path);QVERIFY2(restored.open(false,&error),qPrintable(error));QCOMPARE(restored.snapshots().size(),1);
        const auto restoredValue=AssetValuation::value(restored.snapshots(),{},"USDT",QDateTime::currentMSecsSinceEpoch());
        QVERIFY(restoredValue.complete);QCOMPARE(restoredValue.netAssets,QString("115"));
    }
    void signedUsdNotionalIsPreservedWithoutConvertingItToCollateral() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;
        t.customize=[](const RequestSpec& r,HttpResult& response){if(r.operation!=Operation::Positions)return;
            auto object=QJsonDocument::fromJson(response.body).object();auto rows=object.value("data").toArray();auto row=rows[0].toObject();
            row["notionalUsd"]="-200";row["ccy"]="BTC";rows[0]=row;object["data"]=rows;response.body=QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY2(c.connected(),qPrintable(c.status()));QCOMPARE(c.positions().first().notional,QString("-200"));QCOMPARE(c.positions().first().marginAsset,QString("BTC"));
        for(const auto& part:c.assetCapture().snapshot.components)if(part.kind==AssetComponentKind::PositionNotional){QVERIFY(part.reportedValue.isEmpty());QCOMPARE(part.reportedValueCurrency,QString("BTC"));}
        const auto value=AssetValuation::value({c.assetCapture().snapshot},{},"USDT",QDateTime::currentMSecsSinceEpoch());QVERIFY(value.complete);QCOMPARE(value.netAssets,QString("115"));
    }
    void futureOrMalformedSourceTimesFailClosed_data() {
        QTest::addColumn<QString>("target");
        for(const auto* target:{"account","component","position"})QTest::newRow(target)<<QString::fromLatin1(target);
    }
    void futureOrMalformedSourceTimesFailClosed() {
        QFETCH(QString,target);QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;const auto future=QString::number(QDateTime::currentMSecsSinceEpoch()+86400000);
        t.customize=[&](const RequestSpec& r,HttpResult& response){
            auto object=QJsonDocument::fromJson(response.body).object();auto rows=object.value("data").toArray();
            if(r.operation==Operation::Account){auto account=rows[0].toObject();if(target=="account")account["uTime"]=future;
                if(target=="component"){auto details=account.value("details").toArray();auto row=details[0].toObject();row["uTime"]="invalid";details[0]=row;account["details"]=details;}rows[0]=account;}
            if(target=="position"&&r.operation==Operation::Positions){auto row=rows[0].toObject();row["uTime"]=future;rows[0]=row;}
            object["data"]=rows;response.body=QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(!c.connected());QVERIFY(c.assetCapture().snapshot.id.isEmpty());
    }
    void failedReadRetainsOriginalEvidenceAndIdentityChangeDropsIt() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));
        QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);QVERIFY(c.connected());const auto original=c.assetCapture();
        t.customize=[](const RequestSpec& r,HttpResult& response){if(r.operation==Operation::Positions){response.status=503;response.body="PRIVATE-RAW-SERVER-MARKER";}};
        QVERIFY(c.refreshAssets());QVERIFY(c.assetCapture().snapshot.id.isEmpty());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(!c.connected());QCOMPARE(c.assetCapture().snapshot,original.snapshot);QCOMPARE(c.positions().size(),2);
        QVERIFY(!c.status().contains("PRIVATE"));QVERIFY(!c.status().contains("주문"));
        t.uid="987654321";QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(c.assetCapture().snapshot.id.isEmpty());QVERIFY(c.positions().isEmpty());
    }
    void authFailureRequiresExplicitReconnectAndNeverLeaksProviderText() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;
        t.customize=[](const RequestSpec& r,HttpResult& response){if(r.operation==Operation::AccountConfig){response.status=401;response.body="PRIVATE-KEY-MARKER";}};
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(!c.connected());QCOMPARE(t.requests.size(),2);QVERIFY(!c.refreshAssets());QVERIFY(c.canReconnectAssets());
        QVERIFY(!c.status().contains("PRIVATE"));t.customize={};QVERIFY(c.reconnectAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);QVERIFY(c.connected());
    }
    void incompleteOrMalformedAmountsFailClosed_data() {
        QTest::addColumn<QString>("field");
        for(const auto* field:{"eq","bal","pos","upl","family","duplicate","timestamp"})QTest::newRow(field)<<QString::fromLatin1(field);
    }
    void incompleteOrMalformedAmountsFailClosed() {
        QFETCH(QString,field);QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;
        t.customize=[&](const RequestSpec& r,HttpResult& response){
            auto object=QJsonDocument::fromJson(response.body).object();auto rows=object.value("data").toArray();
            if(field=="eq"&&r.operation==Operation::Account){auto account=rows[0].toObject();auto detail=account.value("details").toArray();auto row=detail[0].toObject();row.remove("eq");detail[0]=row;account["details"]=detail;rows[0]=account;}
            if(field=="bal"&&r.operation==Operation::FundingBalance){auto row=rows[0].toObject();row.remove("bal");rows[0]=row;}
            if(r.operation==Operation::Positions){auto row=rows[0].toObject();
                if(field=="pos")row["pos"]="NaN";if(field=="upl")row.remove("upl");if(field=="family")row["instType"]="MARGIN";
                if(field=="timestamp")row["uTime"]="0";rows[0]=row;if(field=="duplicate")rows.append(row);}
            object["data"]=rows;response.body=QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QVERIFY(!c.connected());QVERIFY(c.assetCapture().snapshot.id.isEmpty());QVERIFY(c.positions().isEmpty());
    }
    void interruptsAndLocksIgnoreLateReplies() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));
        QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);const auto first=c.assetCapture();QVERIFY(!first.snapshot.id.isEmpty());
        t.heldOperations.insert(Operation::Positions);QVERIFY(c.refreshAssets());QTRY_COMPARE_WITH_TIMEOUT(t.held.size(),1,3000);
        QVERIFY(c.interruptRead());QCOMPARE(c.assetCapture().snapshot,first.snapshot);QVERIFY(c.refreshAssets());
        QTRY_COMPARE_WITH_TIMEOUT(t.held.size(),2,3000);t.releaseOldest();QVERIFY(c.busy());
        vault.lock();QVERIFY(!c.busy());QVERIFY(c.assetCapture().snapshot.id.isEmpty());t.releaseOldest();
        QVERIFY(!c.connected());QVERIFY(c.assetCapture().snapshot.id.isEmpty());QVERIFY(c.positions().isEmpty());
    }
    void slowPositionReadsNeverRenewBalanceReceiptTime() {
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;t.heldOperations.insert(Operation::Positions);
        qint64 accountReply=0;t.beforeReply=[&](const RequestSpec& r){if(r.operation==Operation::Account)accountReply=QDateTime::currentMSecsSinceEpoch();};
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_COMPARE_WITH_TIMEOUT(t.held.size(),1,3000);
        QVERIFY(accountReply>0);QTest::qWait(50);t.heldOperations.clear();t.releaseOldest();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);QVERIFY(c.connected());
        const auto capture=c.assetCapture();QVERIFY(capture.snapshot.timeMs>=accountReply);QVERIFY(capture.snapshot.timeMs-accountReply<20);
        QVERIFY(QDateTime::currentMSecsSinceEpoch()-capture.snapshot.timeMs>=50);
        QCOMPARE(capture.snapshot.evidence.receivedAtMs,capture.snapshot.timeMs);
        for(const auto& part:capture.snapshot.components)if(part.kind==AssetComponentKind::Equity)QCOMPARE(part.evidence.receivedAtMs,capture.snapshot.timeMs);
    }
    void rateLimitStopsTheReadAndImmediateRetry_data() {
        QTest::addColumn<int>("mode");QTest::newRow("http429")<<0;QTest::newRow("local")<<1;QTest::newRow("api50011")<<2;
    }
    void rateLimitStopsTheReadAndImmediateRetry() {
        QFETCH(int,mode);QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;
        t.customize=[&](const RequestSpec& r,HttpResult& response){if(r.operation!=Operation::AccountConfig)return;
            response.body="PRIVATE-RATE-MARKER";
            if(mode==0){response.status=429;response.headers["Retry-After"]="120";}
            if(mode==1)response.localRetryAfterSeconds=120;
            if(mode==2)response=okxJson({},"50011");};
        OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));QVERIFY(c.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),3000);
        QCOMPARE(t.requests.size(),2);QVERIFY(!c.refreshAssets());QVERIFY(!c.canReconnectAssets());QVERIFY(!c.status().contains("PRIVATE"));
    }
    void zSharedNetworkCooldownAlsoBlocksFreshConnections() {
        // Last case: the real process-wide public cooldown is intentionally set.
        QTemporaryDir dir;Vault vault(dir.filePath("fake.bin"));QVERIFY(vault.create("Fake OKX futures password! 2026"));
        const auto id=addFakeAccount(vault);FuturesReadTransport t;OkxFuturesReadController c(vault,&t);QVERIFY(c.selectAccount(id));
        HttpResult limited;limited.status=429;limited.headers["Retry-After"]="60";
        TradingRequestLimits::shared()->observe(QUrl("https://openapi.okx.com/api/v5/account/balance"),limited);
        QVERIFY(!c.refreshAssets());QVERIFY(!c.canReconnectAssets());QVERIFY(t.requests.isEmpty());
    }
};
QTEST_MAIN(OkxFuturesReadTest)
#include "tst_okx_futures_read.moc"
