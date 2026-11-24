#include "keefi/PortfolioController.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingController.h"
#include "keefi/AssetsLedger.h"
#include "keefi/AssetValuation.h"
#include "keefi/Vault.h"
#include "fixtures/OkxResponses.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
using namespace keefi;
using namespace keefi::test;
namespace {
const QString Password="Fake Derivative Portfolio! 2026";
class OkxRead final:public TradingTransport {
public:
    QString uid="123456789",mode="2"; int calls=0; bool fail=false,failPositions=false; QList<RequestSpec> requests;
    void abort()override{++epoch;}
    void send(quint64 token,const RequestSpec& request)override{
        ++calls;requests.append(request);QVERIFY(!isMutation(request.operation));
        const auto now=QString::number(QDateTime::currentMSecsSinceEpoch());
        auto response=okxResponse(request);
        if(request.operation==Operation::AccountConfig)response=okxJson({QJsonObject{{"uid",uid},{"acctLv",mode},{"perm","read_only"}}});
        if(request.operation==Operation::Account)response=okxJson({QJsonObject{{"uTime",now},{"details",QJsonArray{QJsonObject{{"ccy","USDT"},{"eq","110"},{"cashBal","100"},{"upl","10"},{"uTime",now},{"availBal","80"},{"frozenBal","20"},{"liab","0"}}}}}});
        if(request.operation==Operation::FundingBalance)response=okxJson({QJsonObject{{"ccy","USDT"},{"bal","5"},{"availBal","5"},{"frozenBal","0"}}});
        if(request.operation==Operation::Positions){
            const auto family=QUrlQuery(request.url).queryItemValue("instType");
            response=okxJson({QJsonObject{{"instType",family},{"instId",family=="SWAP"?"BTC-USDT-SWAP":"ETH-USDT-261225"},{"posId",family=="SWAP"?"101":"102"},{"posSide","net"},{"pos","2"},{"avgPx","100"},{"markPx","110"},{"liqPx",""},{"upl","5"},{"notionalUsd","200"},{"ccy","USDT"},{"lever","3"},{"mgnMode","cross"},{"margin","0"},{"imr","5"},{"mmr","1"},{"uTime",now}}});
        }
        if(fail||(failPositions&&request.operation==Operation::Positions)){response.status=503;response.body="FAKE PRIVATE FAILURE";}
        const auto at=epoch;QTimer::singleShot(1,this,[this,at,token,response]{if(at==epoch)emit finished(token,response);});
    }
private:quint64 epoch=0;
};
class HyperliquidRead final:public TradingTransport {
public:
    int calls=0;void abort()override{}
    void send(quint64 token,const RequestSpec& r)override{
        ++calls;QVERIFY(NetworkTradingTransport::allowed(r));QVERIFY(!isMutation(r.operation));
        const auto type=QJsonDocument::fromJson(r.body).object().value("type").toString();HttpResult h;h.status=200;
        if(type=="userAbstraction")h.body="\"disabled\"";
        else if(type=="metaAndAssetCtxs")h.body=QJsonDocument(QJsonArray{QJsonObject{{"collateralToken",0},{"universe",QJsonArray{QJsonObject{{"name","ETH"}}}}},QJsonArray{QJsonObject{}}}).toJson();
        else if(type=="clearinghouseState"){
            const QJsonObject totals{{"accountValue","100.25"},{"totalNtlPos","300"},{"totalRawUsd","98"},{"totalMarginUsed","10"}};
            const QJsonObject position{{"coin","ETH"},{"szi","-0.1"},{"entryPx","3022.5"},{"positionValue","300"},{"unrealizedPnl","2.25"},{"marginUsed","10"},{"liquidationPx","3900"},{"leverage",QJsonObject{{"type","isolated"},{"value",30}}}};
            h.body=QJsonDocument(QJsonObject{{"marginSummary",totals},{"crossMarginSummary",totals},{"withdrawable","90.25"},{"crossMaintenanceMarginUsed","1"},{"time",QString::number(QDateTime::currentMSecsSinceEpoch())},{"assetPositions",QJsonArray{QJsonObject{{"type","oneWay"},{"position",position}}}}}).toJson();
        }else QFAIL("Unexpected Hyperliquid info read");
        QTimer::singleShot(0,this,[this,token,h]{emit finished(token,h);});
    }
};
class NoPrices final:public TradingTransport {
public:void abort()override{};void send(quint64 token,const RequestSpec& r)override{QVERIFY(!r.authenticated);HttpResult h;h.status=503;QTimer::singleShot(0,this,[this,token,h]{emit finished(token,h);});}
};
QString addOkx(Vault& vault,const QString& market,const QString& suffix){
    auto a=okxAccount();a.id.clear();a.market=market;a.alias="FAKE "+suffix;
    const bool added=vault.addAccount(a,{{"api_key","FakeReadKey"+suffix},{"secret","FakeReadSecret"+suffix},{"passphrase","FakeReadPass"}},StorageMode::EncryptedVault);
    return added?vault.accounts().last().id:QString{};
}
}
class DerivativesPortfolioTest final:public QObject {
    Q_OBJECT
private slots:
    void globalOkxAliasesShareOneCompleteEquitySnapshot(){
        QTemporaryDir directory;Vault vault(directory.filePath("vault.bin"));QVERIFY(vault.create(Password));
        QVERIFY(!addOkx(vault,"spot","ONE").isEmpty());QVERIFY(!addOkx(vault,"okx_futures","TWO").isEmpty());
        QList<OkxRead*> transports;PortfolioController::Factories f;
        f.trading=[&](const AccountRecord&){auto* t=new OkxRead;transports<<t;return t;};f.prices=[] {return new NoPrices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        const auto complete=[&]{const auto rows=portfolio.connections();return rows.size()==2&&std::all_of(rows.cbegin(),rows.cend(),[](const auto& c){return c.state=="complete";});};
        QTRY_VERIFY_WITH_TIMEOUT(complete(),6000);
        QCOMPARE(portfolio.snapshots().size(),1);const auto snapshot=portfolio.snapshots().first();
        QCOMPARE(snapshot.market,QString("okx_unified"));QVERIFY(snapshot.balanceComplete);
        QCOMPARE(std::count_if(snapshot.components.cbegin(),snapshot.components.cend(),[](const auto& c){return c.kind==AssetComponentKind::PositionNotional;}),2);
        const auto valued=AssetValuation::value(portfolio.snapshots(),{},"USDT",QDateTime::currentMSecsSinceEpoch());
        QVERIFY(valued.complete);QCOMPARE(valued.netAssets,QString("115"));
        const auto view=presentPortfolio(portfolio.snapshots(),valued,portfolio.connections());
        QVERIFY2(view.missing.isEmpty(),"Verified aliases of one unified account must not appear as missing accounts");
        for(auto* t:transports)QCOMPARE(t->requests.size(),6);
        vault.lock();QVERIFY(portfolio.snapshots().isEmpty());
    }

    void distinctOkxUidsRemainAdditive(){
        QTemporaryDir directory;Vault vault(directory.filePath("vault.bin"));QVERIFY(vault.create(Password));
        QVERIFY(!addOkx(vault,"spot","ONE").isEmpty());QVERIFY(!addOkx(vault,"okx_futures","TWO").isEmpty());
        PortfolioController::Factories f;
        f.trading=[](const AccountRecord& a){auto* t=new OkxRead;t->uid=a.alias.endsWith("ONE")?"123456789":"987654321";return t;};f.prices=[] {return new NoPrices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),2,3000);
        const auto valued=AssetValuation::value(portfolio.snapshots(),{},"USDT",QDateTime::currentMSecsSinceEpoch());
        QVERIFY(valued.complete);QCOMPARE(valued.netAssets,QString("230"));
    }
    void verifiedUidChangeDropsPreviousAccountEvenWhenPositionsFail(){
        QTemporaryDir directory;Vault vault(directory.filePath("vault.bin"));QVERIFY(vault.create(Password));
        const auto id=addOkx(vault,"spot","ONE");QVERIFY(!id.isEmpty());
        OkxRead transport;PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[] {return new NoPrices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
        QTRY_COMPARE(portfolio.connections().first().state,QString("complete"));
        transport.uid="987654321";transport.failPositions=true;portfolio.refreshConnection(id);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("failed"),3000);
        QVERIFY(portfolio.snapshots().isEmpty());
    }
    void phantomPerpsUseEthereumInfoAndDeduplicateAddressCase(){
        QTemporaryDir directory;Vault vault(directory.filePath("vault.bin"));QVERIFY(vault.create(Password));
        for(const auto& address:QStringList{"0xabcdef0123456789abcdef0123456789abcdef01","0xAbCdEf0123456789AbCdEf0123456789AbCdEf01"}){
            AccountRecord a;a.provider="phantom";a.market="hyperliquid_perps";a.environment="mainnet";a.alias="FAKE HYPERLIQUID";a.currency="USDC";
            QVERIFY(vault.addAccount(a,{{"address",address}},StorageMode::EncryptedVault));
        }
        int solanaCalls=0;PortfolioController::Factories f;
        f.trading=[](const AccountRecord&){return new HyperliquidRead;};
        f.wallet=[&](const AccountRecord&)->SwapTransport*{++solanaCalls;return nullptr;};f.prices=[] {return new NoPrices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        const auto complete=[&]{const auto rows=portfolio.connections();return rows.size()==2&&std::all_of(rows.cbegin(),rows.cend(),[](const auto& c){return c.state=="complete";});};
        QTRY_VERIFY_WITH_TIMEOUT(complete(),3000);QCOMPARE(solanaCalls,0);QCOMPARE(portfolio.snapshots().size(),1);
        const auto value=AssetValuation::value(portfolio.snapshots(),{},"USDC",QDateTime::currentMSecsSinceEpoch());
        QVERIFY(value.complete);QCOMPARE(value.netAssets,QString("100.25"));
        QVERIFY(presentPortfolio(portfolio.snapshots(),value,portfolio.connections()).missing.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.checkpointRecords()==PortfolioController::RecordCheckpoint::Ready,3000);
        vault.lock();QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.connections().isEmpty());
    }
    void defaultManualSpotControllerIsPreparedWithoutNetworkDispatch(){
        QTemporaryDir directory;Vault vault(directory.filePath("vault.bin"));QVERIFY(vault.create(Password));
        const auto id=addOkx(vault,"spot","ONE");QVERIFY(!id.isEmpty());
        PortfolioController portfolio(vault);portfolio.refreshAll(); // Automatic reads stay disabled.
        auto* manual=portfolio.trading(id);QVERIFY(manual);
        QCOMPARE(manual->account().id,id);QVERIFY(!manual->busy());
        QVERIFY(portfolio.snapshots().isEmpty());
    }
    void previousSpotOnlyCacheCannotBeAddedToUnifiedEquity(){
        QTemporaryDir directory;Vault vault(directory.filePath("vault.bin"));QVERIFY(vault.create(Password));
        QVERIFY(!addOkx(vault,"spot","ONE").isEmpty());AssetSnapshot saved;
        {OkxRead transport;PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[] {return new NoPrices;};
            PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);saved=portfolio.snapshots().first();
            QTRY_VERIFY_WITH_TIMEOUT(portfolio.checkpointRecords()==PortfolioController::RecordCheckpoint::Ready,3000);
        }
        // Preserve the real credential binding while simulating a pre-unified encrypted cache.
        saved.market="spot";saved.accountScope="fake-legacy-spot-scope";
        {AssetsLedger ledger(&vault,directory.filePath("assets-current.keefi"));QVERIFY(ledger.open(false));QVERIFY(ledger.replaceLatest({saved},{}));}
        OkxRead failed;failed.fail=true;PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &failed;};f.prices=[] {return new NoPrices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("failed"),3000);
        QVERIFY(portfolio.snapshots().isEmpty());
    }

};
QTEST_MAIN(DerivativesPortfolioTest)
#include "tst_derivatives_portfolio.moc"
