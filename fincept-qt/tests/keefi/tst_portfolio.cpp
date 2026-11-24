#include "keefi/BuildPolicy.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/PublicKrwPrices.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/BrokerTradingController.h"
#include "fixtures/TossResponses.h"
#include "fixtures/SuspendedBrokerVault.h"
#include "fixtures/BinanceUsdmResponses.h"
#include "fixtures/BinanceBalanceSocket.h"
#include "fixtures/UpbitResponses.h"
#include "fixtures/OkxResponses.h"
#include "fixtures/BithumbResponses.h"
#include "fixtures/KisResponses.h"
#include "fixtures/KiwoomResponses.h"
#include "fixtures/LsResponses.h"
#include "keefi/AssetsLedger.h"
#include "keefi/WalletReadController.h"
#include "keefi/SolanaTokenData.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/Vault.h"
#include <QDateTime>
#include <QApplication>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QPointer>
#include <QSignalSpy>
#include <QUrlQuery>
#include <vector>
using namespace keefi;
namespace {
const QString Password="Fake Portfolio Password! 2026";
class AccountTransport final:public TradingTransport {
public:
    QList<Operation> sent;bool fail=false,hold=false;quint64 pending=0;HttpResult answer;
    QJsonArray balances{QJsonObject{{"asset","BTC"},{"free","0.125"},{"locked","0.025"}}};
    void abort()override{}
    void send(quint64 token,const RequestSpec& r)override{
        sent.append(r.operation);QVERIFY(!isMutation(r.operation));QVERIFY(r.operation==Operation::Time||r.operation==Operation::Account);
        answer={};answer.status=fail?503:200;
        if(r.operation==Operation::Time)answer.body=QJsonDocument(QJsonObject{{"serverTime",QString::number(QDateTime::currentMSecsSinceEpoch())}}).toJson();
        else answer.body=QJsonDocument(QJsonObject{{"canTrade",true},{"accountType","SPOT"},{"balances",balances}}).toJson();
        pending=token;if(!hold)QTimer::singleShot(1,this,[this,token,response=answer]{emit finished(token,response);});
    }
};
class FuturesReadTransport final:public TradingTransport {
public:
    bool hold=false,zeroBalances=false;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!isMutation(request.operation));
        auto response=keefi::test::usdmResponse(request);
        if(zeroBalances&&request.operation==Operation::Account){
            auto object=QJsonDocument::fromJson(response.body).object();auto assets=object["assets"].toArray();
            for(qsizetype i=0;i<assets.size();++i){auto row=assets[i].toObject();row["walletBalance"]="0";row["marginBalance"]="0";row["availableBalance"]="0";row["unrealizedProfit"]="0";assets[i]=row;}
            object["assets"]=assets;response=keefi::test::usdmJson(object);
        }
        if(zeroBalances&&request.operation==Operation::Positions)response=keefi::test::usdmJson(QJsonArray{});
        if(!hold)QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class PublicPriceTransport final:public TradingTransport {
public:
    int calls=0;bool stale=false,holdQuote=false;int status=200;quint64 heldToken=0;HttpResult held;
    QStringList markets{"KRW-USDT"},binanceAssets;QSet<QString> staleMarkets;QString fault;
    std::function<void(Operation,HttpResult&)> transform;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());QVERIFY(NetworkTradingTransport::allowed(request));++calls;
        if(request.url.host()=="api.binance.com"&&request.operation==Operation::Markets){
            QJsonArray symbols;
            for(const auto& asset:binanceAssets)symbols.append(QJsonObject{{"symbol",asset+"USDT"},{"baseAsset",asset},
                {"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}});
            HttpResult response;response.status=200;response.body=QJsonDocument(QJsonObject{{"symbols",symbols}}).toJson();
            QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});return;
        }
        const auto now=QDateTime::currentMSecsSinceEpoch();
        QJsonArray rows;
        const auto requested=request.operation==Operation::Markets?markets:QUrlQuery(request.url).queryItemValue("markets").split(',');
        for(const auto& market:requested)rows.append(request.operation==Operation::Markets?QJsonObject{{"market",market}}:
            QJsonObject{{"market",market},{"trade_price","1400"},{"timestamp",QString::number(stale||staleMarkets.contains(market)?now-600000:now)}});
        if(request.operation==Operation::Quote&&!rows.isEmpty()){
            if(fault=="missing")rows.removeLast();
            else if(fault=="duplicate")rows.append(rows.first());
            else if(!fault.isEmpty()){
                auto row=rows.last().toObject();
                if(fault=="invalid-price")row["trade_price"]="invalid";
                if(fault=="future-time")row["timestamp"]=QString::number(now+60000);
                if(fault=="unexpected-market")row["market"]="KRW-UNREQUESTED";
                rows[rows.size()-1]=row;
            }
        }
        auto response=keefi::test::usdmJson(rows);
        if(request.operation==Operation::Quote){response.status=status;if(status==429){response.headers["Retry-After"]="90";response.body="FAKE_PRIVATE_ERROR_MUST_NOT_BE_SHOWN";}
            if(holdQuote){heldToken=token;held=response;return;}}
        if(transform)transform(request.operation,response);
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class OfflinePublicSocket final:public PublicTickerSocket {
public:
    void open(const QUrl&)override{}
    void send(const QByteArray&)override{}
    void ping()override{}
    void abort()override{}
};
class TossReadTransport final:public TradingTransport {
public:
    QList<Operation> sent;bool conditionsEnabled=false,timeoutConditional=false,writeAheadObserved=false;Vault* vault=nullptr;bool holdAuthentication=false;quint64 pending=0;HttpResult held;void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        sent.append(request.operation);QVERIFY(QList<Operation>({Operation::Authenticate,Operation::Accounts,Operation::Holdings,Operation::Account}).contains(request.operation)||(conditionsEnabled&&QList<Operation>({Operation::AlgoOpenOrders,Operation::AlgoCancelOrder,Operation::AlgoQueryOrder}).contains(request.operation)));
        auto response=keefi::test::tossResponse(request.operation);
        if(request.operation==Operation::AlgoOpenOrders||request.operation==Operation::AlgoQueryOrder){
            const QJsonObject leg{{"type","STOP"},{"status","WATCHING"},{"orderSide","SELL"},{"triggerPrice","70000"},{"orderPrice","69900"},{"triggeredOrderId",QJsonValue::Null}};
            const QJsonObject item{{"conditionalOrderId","fake-conditional-id"},{"type","SINGLE"},{"status","WATCHING"},{"symbol","005930"},{"market","KR"},{"quantity","1"},{"orderType","LIMIT"},{"first",leg},{"second",QJsonValue::Null},{"createdAt","2026-10-17T09:00:00+09:00"}};
            response.status=200;response.body=QJsonDocument(QJsonObject{{"result",request.operation==Operation::AlgoQueryOrder?item:QJsonObject{{"conditionalOrders",QJsonArray{item}},{"hasNext",false},{"nextCursor",QJsonValue::Null}}}}).toJson();
        }
        if(request.operation==Operation::AlgoCancelOrder){QCOMPARE(request.url.path(),QString("/api/v1/conditional-orders/fake-conditional-id"));writeAheadObserved=vault&&!vault->pendingOperations().isEmpty()&&vault->pendingOperations()[0].toObject().value("brokerConditional")=="true";response={};response.status=timeoutConditional?0:204;response.timedOut=timeoutConditional;}
        if(holdAuthentication&&request.operation==Operation::Authenticate){pending=token;held=response;return;}QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class WalletTransport final:public SwapTransport {
public:
    QList<SwapOperation> sent;bool wrongOwner=false,hold=false,metadataFail=false,priceFail=false,withSkhy=false,holdMetadata=false,zeroNativeBalance=false;qint64 blockAgeSeconds=1;quint64 token=0;HttpResult response;
    void abort()override{}
    void send(quint64 n,const SwapRequest& request)override{
        QVERIFY(NetworkSwapTransport::allowed(request));QVERIFY(request.operation!=SwapOperation::Order&&request.operation!=SwapOperation::Execute);
        sent.append(request.operation);
        if(request.operation==SwapOperation::TokenMetadata||request.operation==SwapOperation::TokenPrices||request.operation==SwapOperation::BlockTimes){
            response={};response.status=200;QJsonDocument doc;
            if(request.operation==SwapOperation::TokenMetadata){
                response.status=metadataFail?503:200;
                QJsonArray names{QJsonObject{{"id",UsdcMint},{"name","USD Coin"},{"symbol","USDC"},{"decimals",6},{"tokenProgram","TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"}}};
                if(withSkhy)names.append(QJsonObject{{"id","SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3"},{"name","SK Hynix - Backpack Securities"},{"symbol","SKHY"},{"decimals",6},{"tokenProgram","TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"}});
                doc=QJsonDocument(names);
            }else if(request.operation==SwapOperation::TokenPrices){
                response.status=priceFail?503:200;
                QJsonObject prices{{UsdcMint,QJsonObject{{"usdPrice","0.9999"},{"blockId","12345"},{"decimals",6}}}};
                if(withSkhy)prices["SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3"]=QJsonObject{{"usdPrice","136.1"},{"blockId","12344"},{"decimals",6}};
                doc=QJsonDocument(prices);
            }else{
                QJsonArray times;for(const auto& v:QJsonDocument::fromJson(request.body).array())times.append(QJsonObject{{"jsonrpc","2.0"},{"id",v.toObject().value("id")},{"result",QString::number(QDateTime::currentSecsSinceEpoch()-blockAgeSeconds)}});
                doc=QJsonDocument(times);
            }
            response.body=doc.toJson();token=n;
            if(!hold&&!(holdMetadata&&request.operation==SwapOperation::TokenMetadata))QTimer::singleShot(1,this,[this,n,result=response]{emit finished(n,result);});return;
        }
        const auto body=QJsonDocument::fromJson(request.body).object();const auto params=body.value("params").toArray();QJsonValue value;
        if(request.operation==SwapOperation::Genesis)value="5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d";
        else if(request.operation==SwapOperation::Balance)value=QJsonObject{{"context",QJsonObject{{"slot","100"}}},{"value",zeroNativeBalance?"0":"1234567890"}};
        else if(request.operation==SwapOperation::Accounts){
            QJsonArray values;for(const auto& m:params[0].toArray()){
                const auto program=m.toString()==UsdcMint?QString("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"):QString("TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb");
                values.append(QJsonObject{{"owner",program},{"executable",false},{"data",QJsonObject{{"parsed",QJsonObject{{"type","mint"},{"info",QJsonObject{{"decimals",6},{"isInitialized",true}}}}}}}});
            }value=QJsonObject{{"context",QJsonObject{{"slot","101"}}},{"value",values}};
        }
        else {QJsonArray accounts;const auto program=params[1].toObject().value("programId").toString();
            if(program=="TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"){
                const QJsonObject info{{"owner",wrongOwner?UsdcMint:params[0].toString()},{"mint",UsdcMint},{"state","initialized"},{"tokenAmount",QJsonObject{{"amount","123456789012345"},{"decimals","6"}}}};
                accounts.append(QJsonObject{{"pubkey","ComputeBudget111111111111111111111111111111"},{"account",QJsonObject{{"owner",program},{"data",QJsonObject{{"parsed",QJsonObject{{"type","account"},{"info",info}}}}}}}});
            }else if(withSkhy){
                const QJsonObject info{{"owner",params[0].toString()},{"mint","SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3"},{"state","initialized"},{"tokenAmount",QJsonObject{{"amount","2000000"},{"decimals","6"}}}};
                accounts.append(QJsonObject{{"pubkey","SysvarRent111111111111111111111111111111111"},{"account",QJsonObject{{"owner",program},{"data",QJsonObject{{"parsed",QJsonObject{{"type","account"},{"info",info}}}}}}}});
            }value=QJsonObject{{"context",QJsonObject{{"slot","101"}}},{"value",accounts}};
        }
        response={};response.status=200;response.body=QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",body.value("id")},{"result",value}}).toJson();token=n;
        if(!hold)QTimer::singleShot(1,this,[this,n,result=response]{emit finished(n,result);});
    }
};
AccountRecord binance(QString alias){AccountRecord a;a.provider="binance";a.alias=alias;a.environment="live";a.market="spot";a.currency="USDT";return a;}
class LifecycleTransport final:public TradingTransport {
public:
    QString provider,market;QList<Operation> sent;quint64 generation=0;
    void abort()override{++generation;}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!isMutation(request.operation));sent.append(request.operation);
        QVERIFY(QList<Operation>({Operation::Time,Operation::Authenticate,Operation::Accounts,Operation::AccountConfig,Operation::Account,Operation::Holdings,Operation::FundingBalance,Operation::Positions}).contains(request.operation));
        HttpResult response;const auto now=QDateTime::currentDateTimeUtc();
        if(provider=="binance"&&market=="usdm_futures")response=keefi::test::usdmResponse(request);
        else if(provider=="binance")response=keefi::test::usdmJson(request.operation==Operation::Time?QJsonObject{{"serverTime",QString::number(now.toMSecsSinceEpoch())}}:
            QJsonObject{{"canTrade",true},{"accountType","SPOT"},{"balances",QJsonArray{QJsonObject{{"asset","BTC"},{"free","0.125"},{"locked","0"}}}}});
        else if(provider=="upbit")response=upbit_fixture::response(request.operation);
        else if(provider=="okx")response=keefi::test::okxResponse(request);
        else if(provider=="bithumb")response=keefi::test::bithumbResponse(request);
        else if(provider=="toss")response=keefi::test::tossResponse(request.operation);
        else if(provider=="kis")response=keefi::test::kisResponse(request,now);
        else if(provider=="kiwoom")response=keefi::test::kiwoomResponse(request,now);
        else if(provider=="ls")response=keefi::test::lsResponse(request,now);
        const auto epoch=generation;QTimer::singleShot(1,this,[this,token,response,epoch]{if(epoch==generation)emit finished(token,response);});
    }
};
}
namespace {
template<class History> QStringList blockedConnectionNames(const History& history){
    if constexpr(requires{history.blockedConnectionNames;})return history.blockedConnectionNames;
    return {};
}
void compareHistoryPoints(const PortfolioDayHistory& cached,const PortfolioDayHistory& synchronous){
    QCOMPARE(cached.comparison.available,synchronous.comparison.available);
    QCOMPARE(cached.comparison.complete,synchronous.comparison.complete);
    QCOMPARE(cached.comparison.difference,synchronous.comparison.difference);
    QCOMPARE(cached.points.size(),synchronous.points.size());
    for(qsizetype i=0;i<cached.points.size();++i){
        QCOMPARE(cached.points[i].timeMs,synchronous.points[i].timeMs);QCOMPARE(cached.points[i].value,synchronous.points[i].value);
        QCOMPARE(cached.points[i].available,synchronous.points[i].available);QCOMPARE(cached.points[i].complete,synchronous.points[i].complete);
        QCOMPARE(cached.points[i].connectToPrevious,synchronous.points[i].connectToPrevious);QCOMPARE(cached.points[i].reason,synchronous.points[i].reason);
    }
}
}
class PortfolioTests final:public QObject {
    Q_OBJECT
private slots:
    void coinPerformanceClearsBeforeConnectionRebuild_data(){
        QTest::addColumn<QString>("change");
        for(const auto* change:{"add","remove","replace-key"})QTest::newRow(change)<<QString(change);
    }
    void coinPerformanceClearsBeforeConnectionRebuild(){
        QFETCH(QString,change);QTemporaryDir dir;Vault vault(dir.filePath("synthetic-coin-scope-vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("Synthetic coin scope"),{{"api_key","SyntheticScopeKey123"},{"secret","SyntheticScopeSecret123"}},StorageMode::EncryptedVault));
        AccountTransport wire;wire.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &wire;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.coinPerformance().inScope,4000);
        const auto id=vault.accounts().first().id;wire.hold=true;
        if(change=="remove")QVERIFY(vault.removeAccount(id));
        else if(change=="replace-key")QVERIFY(vault.replaceCredentials(id,{{"api_key","SyntheticNewScopeKey123"},{"secret","SyntheticNewScopeSecret123"}}));
        else QVERIFY(vault.addAccount(binance("Synthetic new coin"),{{"api_key","SyntheticNewCoinKey123"},{"secret","SyntheticNewCoinSecret123"}},StorageMode::EncryptedVault));
        QVERIFY(!portfolio.coinPerformance().inScope); // Before the queued connection rebuild.
        QCoreApplication::processEvents();QVERIFY(portfolio.coinPerformance().today.pnl.isEmpty());
    }
    void coinPerformanceWaitsForEveryConfiguredCoinConnection(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-missing-coin-vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("Synthetic ready coin"),{{"api_key","SyntheticReadyKey123"},{"secret","SyntheticReadySecret123"}},StorageMode::EncryptedVault));
        AccountRecord pendingAccount;pendingAccount.provider="upbit";pendingAccount.environment="live";pendingAccount.market="spot";pendingAccount.currency="KRW";pendingAccount.alias="Synthetic pending coin";
        QVERIFY(vault.addAccount(pendingAccount,{{"access_key","SyntheticPendingAccess123"},{"secret","SyntheticPendingSecret123"}},StorageMode::EncryptedVault));
        AccountTransport ready,pending,earnWire;pending.hold=earnWire.hold=true;ready.balances={QJsonObject{{"asset","KRW"},{"free","110"},{"locked","0"}}};
        qint64 clock=0,wall=QDateTime::currentMSecsSinceEpoch();PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord& a){return a.provider=="binance"?&ready:&pending;};
        factories.earn=[&]{return &earnWire;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};factories.monotonicMs=[&]{return clock;};factories.wallClockMs=[&]{return wall;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        auto* ledger=portfolio.historyLedger();QVERIFY(ledger->open(true));const auto current=portfolio.snapshots().first();wall=current.timeMs;
        auto opening=current,closing=current;opening.id+="-opening";closing.id+="-closing";
        opening.timeMs=wall-600000;closing.timeMs=wall-120000;opening.components[0].quantity="100";
        for(auto* s:{&opening,&closing}){s->evidence.receivedAtMs=s->evidence.sourceTimeMs=s->timeMs;for(auto& c:s->components)c.evidence=s->evidence;}
        QVERIFY(ledger->append({opening,closing},{},{}));
        FlowAccountState flow;flow.accountScope=current.accountScope;flow.connectionId=current.connectionId;flow.binding=current.connectionBinding;
        flow.provider="binance";flow.market="spot";flow.revision=1;flow.trackingStartMs=wall-86400000;
        for(const auto source:FundFlowTracking::sources(flow)){
            const auto types=source==FlowSource::BinanceTransfers?FundFlowTracking::transferTypes(flow):QStringList{QString{}};
            for(const auto& type:types)flow.windows.append({source,type,wall-8LL*86400000,wall,wall});}
        QSignalSpy saved(ledger,&AssetsLedger::collectionWriteFinished);QString error;
        QCOMPARE(ledger->saveFlowState(flow,0,701,&error),AssetsLedger::CollectionWrite::Pending);QTRY_COMPARE(saved.size(),1);QVERIFY(saved[0][1].toBool());
        EarnAccountState earn;earn.accountScope=current.accountScope;earn.connectionId=current.connectionId;earn.binding=current.connectionBinding;earn.revision=1;earn.trackingStartMs=wall-86400000;
        for(const auto source:earnSources())if(!earnPositions(source))earn.windows.append({source,wall-31LL*86400000,wall,wall});
        QCOMPARE(ledger->saveEarnState(earn,0,702,&error),AssetsLedger::CollectionWrite::Pending);QTRY_COMPARE(saved.size(),2);QVERIFY(saved[1][1].toBool());
        QSignalSpy evaluated(&portfolio,&PortfolioController::coinPerformanceChanged);clock=60000;portfolio.refreshCoinFlows();
        QTRY_VERIFY_WITH_TIMEOUT(!evaluated.isEmpty(),3000);const auto view=portfolio.coinPerformance();
        QVERIFY(view.inScope);QVERIFY(!view.today.available);QVERIFY(!view.sinceStart.available);
        QVERIFY(view.today.pnl.isEmpty());QVERIFY(view.proposedBaseline.isEmpty());
        QVERIFY2(view.today.reason.contains("모든 코인 계좌"),qPrintable(view.today.reason));
    }
    void automaticFlowReadRequiresAnEligibleCoinSnapshot_data() {
        QTest::addColumn<bool>("pendingCoinAccount");
        QTest::newRow("no-coin-accounts") << false;
        QTest::newRow("coin-balance-still-pending") << true;
    }
    void automaticFlowReadRequiresAnEligibleCoinSnapshot() {
        QFETCH(bool,pendingCoinAccount);
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        if(pendingCoinAccount)QVERIFY(vault.addAccount(binance("Synthetic pending coin"),
            {{"api_key","SyntheticPendingKey123"},{"secret","SyntheticPendingSecret123"}},StorageMode::EncryptedVault));
        AccountTransport accountWire,flowWire;accountWire.hold=true;
        PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &accountWire;};
        factories.flow=[&]{return &flowWire;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        QSignalSpy evaluated(&portfolio,&PortfolioController::coinPerformanceChanged);
        QVERIFY(!vault.hasAssetsLedgerKey());portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(!evaluated.isEmpty(),3000); // The automatic timer has run.
        QVERIFY(!vault.hasAssetsLedgerKey());
        QVERIFY(!portfolio.historyLedger()->isOpen());
        QVERIFY(!QFileInfo::exists(dir.filePath("assets.keefi")));
        QVERIFY(!QFileInfo::exists(dir.filePath("assets.keefi.lock")));
        QVERIFY(flowWire.sent.isEmpty());
    }
    void coinEvaluationDoesNotAcquireHistoryWriteLock() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AssetsLedger seed(&vault,dir.filePath("assets.keefi"));QVERIFY(seed.open(true));seed.lock();
        QFile beforeFile(seed.filePath());QVERIFY(beforeFile.open(QIODevice::ReadOnly));
        const auto before=beforeFile.readAll();beforeFile.close();
        const auto permissions=QFileInfo(seed.filePath()).permissions();
        PortfolioController::Factories factories;
        factories.trading=[](const AccountRecord&){return static_cast<TradingTransport*>(nullptr);};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        QSignalSpy evaluated(&portfolio,&PortfolioController::coinPerformanceChanged);
        portfolio.setAutomaticRefresh(true);QTRY_VERIFY_WITH_TIMEOUT(!evaluated.isEmpty(),3000);
        QVERIFY(!portfolio.historyLedger()->isOpen());
        QVERIFY(!QFileInfo::exists(seed.filePath()+".lock"));
        QCOMPARE(QFileInfo(seed.filePath()).permissions(),permissions);
        QFile afterFile(seed.filePath());QVERIFY(afterFile.open(QIODevice::ReadOnly));QCOMPARE(afterFile.readAll(),before);
        AssetsLedger writer(&vault,seed.filePath());QVERIFY(writer.open(false));
    }
    void destroyingPortfolioWithPendingDayHistoryDoesNotPublishResults() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE history destruction"),{{"api_key","FakeHistoryDestructionKey123"},{"secret","FakeHistoryDestructionSecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        auto portfolio=std::make_unique<PortfolioController>(vault,nullptr,factories);portfolio->setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio->connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio->checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        int published=0;connect(portfolio.get(),&PortfolioController::dayHistoryChanged,this,[&]{++published;});
        portfolio->requestDayHistory({},portfolio->snapshots().first().timeMs+1000);QVERIFY(portfolio->dayHistoryBusy());
        portfolio.reset();QTest::qWait(30);QCOMPARE(published,0);QVERIFY(vault.isUnlocked());
    }
    void dayHistoryContributionsMatchComparisonAndClearOnLock() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE contribution history"),{{"api_key","FakeContributionKey123"},{"secret","FakeContributionSecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        auto older=portfolio.snapshots().first();older.id+="-older";--older.timeMs;older.evidence.sourceTimeMs=older.evidence.receivedAtMs=older.timeMs;
        older.components[0].quantity="50";older.components[0].evidence=older.evidence;
        QVERIFY(portfolio.historyLedger()->append({older},{},{}));const auto at=portfolio.snapshots().first().timeMs+1000;
        auto result=portfolio.dayHistory({},at);QVERIFY(result.comparison.available);QVERIFY(result.contributions.available);
        QCOMPARE(result.contributions.rows.size(),1);QCOMPARE(result.contributions.rows.first().difference,QString("50"));
        QCOMPARE(result.contributions.comparison.difference,result.comparison.difference);
        portfolio.requestDayHistory({},at);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        result=portfolio.dayHistoryResult();QVERIFY(result.contributions.available);QCOMPARE(result.contributions.comparison.difference,QString("50"));
        // G7 permits exclusions only for canonical Solana mints; KRW remains in scope.
        portfolio.requestDayHistory({"KRW"},at);result=portfolio.dayHistoryResult();QVERIFY(result.contributions.available);
        QCOMPARE(result.contributions.comparison.difference,QString("50"));QCOMPARE(result.contributions.rows.size(),1);
        vault.lock();QVERIFY(portfolio.dayHistoryResult().contributions.rows.isEmpty());QVERIFY(!portfolio.dayHistory({},at).contributions.available);
    }
    void previousDayUsesTheLatestCompleteKstObservationAndNeverTheTodayOpening() {
        QTemporaryDir dir;Vault vault(dir.filePath("yesterday-vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE yesterday"),{{"api_key","FakeYesterdayKey123"},{"secret","FakeYesterdaySecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        const auto current=portfolio.snapshots().first();
        const auto at=current.timeMs+1;
        const auto midnight=portfolioHistoryDayStart(at);
        const auto record=[&](qint64 time,const QString& quantity) {
            auto row=current;row.id=QString::number(time);row.timeMs=time;
            row.evidence.sourceTimeMs=row.evidence.receivedAtMs=time;
            row.components[0].quantity=quantity;row.components[0].evidence=row.evidence;return row;
        };
        const auto otherDate=record(portfolioHistoryDayStart(midnight-1)-1,"1");
        QVERIFY(portfolio.historyLedger()->append({otherDate},{},{}));
        QVERIFY(!portfolio.dayHistory({},at).previousDay.comparison.available);
        const auto earlier=record(midnight-600000,"50"),previous=record(midnight-240000,"80");
        auto incomplete=record(midnight-1000,"70");incomplete.balanceComplete=false;
        auto late=record(previous.timeMs+1000,"999");late.evidence.receivedAtMs=late.timeMs+1000;
        late.components[0].evidence=late.evidence;
        QVERIFY(portfolio.historyLedger()->append({earlier,previous,late,incomplete},{},{}));
        const auto sync=portfolio.dayHistory({},at);
        portfolio.requestDayHistory({},at);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),3000);
        const auto deferred=portfolio.dayHistoryResult();
        for(const auto& day:{sync,deferred}){
            QVERIFY2(day.previousDay.available,qPrintable(day.previousDay.reason));
            const auto& yesterday=day.previousDay.comparison;
            QVERIFY(yesterday.complete);QCOMPARE(yesterday.fromMs,previous.timeMs);QCOMPARE(yesterday.toMs,at);
            QCOMPARE(yesterday.openingValue,QString("80"));QCOMPARE(yesterday.closingValue,QString("100"));
            QCOMPARE(yesterday.difference,QString("20"));QCOMPARE(day.previousDay.groups.first().difference,QString("20"));
            QVERIFY(day.baseline.timeMs>=midnight);for(const auto& point:day.points)QVERIFY(point.timeMs>=midnight);
        }
        portfolio.requestDayHistory({},at+1000);
        const auto next=portfolio.dayHistoryResult();
        QCOMPARE(next.previousDay.comparison.toMs,at+1000);
        QCOMPARE(next.closingValuation.timeMs,at+1000);
        // Kept immutable row storage proves the ordinary timer tick skipped classification.
        QVERIFY(next.previousDay.rows.constData()==deferred.previousDay.rows.constData());
        portfolio.requestDayHistory({"selection-changed"},at+1001);
        const auto reselected=portfolio.dayHistoryResult();
        QCOMPARE(reselected.previousDay.comparison.difference,QString("20"));
        QVERIFY(reselected.previousDay.rows.constData()!=next.previousDay.rows.constData());
        portfolio.requestDayHistory({},at+DisplayFreshnessMs);
        QVERIFY(!portfolio.dayHistoryResult().previousDay.comparison.available);
        // A traded instrument changes the constituent keys. Complete account
        // totals stay comparable; individual contributions cannot be assigned.
        auto changed=record(midnight-500,"60");changed.components[0].id="previous-cash";
        QVERIFY(portfolio.historyLedger()->append({changed},{},{}));
        const auto traded=portfolio.dayHistory({},at).previousDay;
        QVERIFY(traded.comparison.available);QCOMPARE(traded.comparison.difference,QString("40"));
        QVERIFY(!traded.available);QVERIFY(traded.groups.isEmpty());
        portfolio.requestDayHistory({},at);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),3000);
        QCOMPARE(portfolio.dayHistoryResult().previousDay.comparison.difference,traded.comparison.difference);
        // An old registration outside today's set cannot simply disappear from yesterday's total.
        auto foreign=record(midnight-300,"20");foreign.connectionId="old-registration";foreign.accountScope="old-scope";
        foreign.evidence.scope=foreign.accountScope;foreign.components[0].evidence=foreign.evidence;
        QVERIFY(portfolio.historyLedger()->append({foreign},{},{}));
        QVERIFY(!portfolio.dayHistory({},at).previousDay.comparison.available);
        portfolio.requestDayHistory({},at);vault.lock();
        QVERIFY(!portfolio.dayHistoryResult().previousDay.comparison.available);
        QVERIFY(portfolio.dayHistoryResult().previousDay.groups.isEmpty());
    }
    void previousDayRepricesOffsettingRowsAndRenewsEqualPriceEvidence() {
        QTemporaryDir dir;Vault vault(dir.filePath("repricing-vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE repricing"),{{"api_key","FakeRepricingKey123"},{"secret","FakeRepricingSecret123"}},StorageMode::EncryptedVault));
        AccountTransport wire;wire.balances={};
        for(const auto& asset:{"BTC","ETH"})
            wire.balances.append(QJsonObject{{"asset",asset},{"free","1"},{"locked","0"}});
        auto* prices=new PublicPriceTransport;prices->markets={"KRW-BTC","KRW-ETH"};
        PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &wire;};factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().size(),2,3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        portfolio.setAutomaticRefresh(false);
        const auto observedAt=QDateTime::currentMSecsSinceEpoch();
        const auto priorAt=portfolioHistoryDayStart(observedAt)-60000;
        auto opening=portfolio.snapshots().first();opening.id="previous-close";opening.timeMs=priorAt;
        opening.evidence.sourceTimeMs=opening.evidence.receivedAtMs=priorAt;
        for(auto& component:opening.components)component.evidence=opening.evidence;
        auto oldQuotes=portfolio.quotes();
        for(auto& quote:oldQuotes)quote.evidence.sourceTimeMs=quote.evidence.receivedAtMs=priorAt;
        QVERIFY(portfolio.historyLedger()->append({opening},{},oldQuotes));
        portfolio.requestDayHistory({},observedAt);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),3000);
        const auto first=portfolio.dayHistoryResult();QVERIFY(first.previousDay.available);
        QCOMPARE(first.previousDay.comparison.difference,QString("0"));
        const auto setTicks=[&](qint64 at) {
            auto ticks=oldQuotes;
            for(auto& quote:ticks){
                quote.price=quote.baseAsset=="BTC"?"1410":"1390";
                quote.evidence.sourceTimeMs=quote.evidence.receivedAtMs=at;
            }
            portfolio.setPublicPrices(ticks,prices->markets);
            portfolio.requestDayHistory({},at);
            return portfolio.dayHistoryResult();
        };
        QTRY_VERIFY(QDateTime::currentMSecsSinceEpoch()>observedAt);
        const auto changedAt=QDateTime::currentMSecsSinceEpoch();
        const auto moved=setTicks(changedAt);QVERIFY(moved.previousDay.available);
        QCOMPARE(moved.previousDay.comparison.difference,QString("0"));
        QCOMPARE(moved.previousDay.rows.size(),2);
        for(const auto& row:moved.previousDay.rows)QCOMPARE(row.difference,row.asset=="BTC"?QString("10"):QString("-10"));
        QTRY_VERIFY(QDateTime::currentMSecsSinceEpoch()>changedAt);
        const auto renewedAt=QDateTime::currentMSecsSinceEpoch();
        const auto renewed=setTicks(renewedAt);
        QCOMPARE(renewed.previousDay.comparison.toMs,renewedAt);
        QCOMPARE(renewed.closingValuation.rows.first().priceEvidence.receivedAtMs,renewedAt);
        QVERIFY(renewed.previousDay.available);
        QVERIFY(renewed.previousDay.rows.constData()!=moved.previousDay.rows.constData());
    }
    void dayHistoryPreservesTheExactClosingProvenance() {
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-closing-vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE closing provenance"),{{"api_key","FakeClosingKey123"},{"secret","FakeClosingSecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        const auto selected=portfolio.snapshots().first();const auto at=selected.timeMs+1000;const auto sent=transport.sent;
        const auto synchronous=portfolio.dayHistory({},at);QVERIFY(synchronous.comparison.available);
        QCOMPARE(synchronous.closingValuation.timeMs,at);QCOMPARE(synchronous.closingValuation.netAssets,QString("100"));
        QCOMPARE(synchronous.closingSnapshots,(QList<AssetSnapshot>{selected}));
        QCOMPARE(synchronous.closingValuation.rows.first().priceEvidence,selected.components.first().evidence);
        portfolio.requestDayHistory({},at+1);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        const auto deferred=portfolio.dayHistoryResult();QVERIFY(deferred.comparison.available);
        QCOMPARE(deferred.closingValuation.timeMs,at+1);QCOMPARE(deferred.comparison.toMs,deferred.closingValuation.timeMs);
        QCOMPARE(deferred.closingSnapshots,(QList<AssetSnapshot>{selected}));
        QCOMPARE(deferred.closingValuation.rows.first().priceEvidence,selected.components.first().evidence);
        QCOMPARE(transport.sent,sent); // Provenance storage must never add a financial request.
        vault.lock();QVERIFY(portfolio.dayHistoryResult().closingSnapshots.isEmpty());QVERIFY(portfolio.dayHistoryResult().closingValuation.rows.isEmpty());
    }
    void asyncDayHistoryCachesInputsAndDropsPendingResultOnLock() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE async history"),{{"api_key","FakeAsyncHistoryKey123"},{"secret","FakeAsyncHistorySecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        const auto at=portfolio.snapshots().first().timeMs+1000;
        portfolio.requestDayHistory({},at);QVERIFY(portfolio.dayHistoryBusy());QVERIFY(!portfolio.dayHistoryResult().comparison.available);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        auto result=portfolio.dayHistoryResult();QVERIFY2(result.comparison.available,qPrintable(result.comparison.reason));QCOMPARE(result.comparison.difference,QString("0"));
        portfolio.requestDayHistory({},at+1);QVERIFY(!portfolio.dayHistoryBusy());QCOMPARE(portfolio.dayHistoryResult().points.last().timeMs,at+1);
        // Unsupported exclusions are ignored, preserving the G7 canonical-mint rule.
        portfolio.requestDayHistory({"KRW"},at+2);QVERIFY(!portfolio.dayHistoryBusy());QCOMPARE(portfolio.dayHistoryResult().comparison.excludedRowCount,0);
        portfolio.requestDayHistory({},at+3);QCOMPARE(portfolio.dayHistoryResult().comparison.excludedRowCount,0);
        auto older=portfolio.snapshots().first();older.id+="-older";--older.timeMs;older.evidence.sourceTimeMs=older.evidence.receivedAtMs=older.timeMs;
        older.components[0].quantity="50";older.components[0].evidence=older.evidence;
        QVERIFY(portfolio.historyLedger()->append({older},{},{}));
        portfolio.requestDayHistory({},at+4);QVERIFY(portfolio.dayHistoryBusy());QVERIFY(portfolio.dayHistoryResult().comparison.difference.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);QCOMPARE(portfolio.dayHistoryResult().comparison.difference,QString("50"));
        auto future=older;future.id+="-future";future.timeMs=at+50;future.evidence.sourceTimeMs=future.evidence.receivedAtMs=future.timeMs;
        future.components[0].quantity="75";future.components[0].evidence=future.evidence;QVERIFY(portfolio.historyLedger()->append({future},{},{}));
        // A newer request may finish after the caller has rewound its as-of.
        // Coalescing must reread the narrower cutoff before publishing any rows.
        portfolio.requestDayHistory({},at+51);QVERIFY(portfolio.dayHistoryBusy());
        portfolio.requestDayHistory({},at+40);QVERIFY(portfolio.dayHistoryBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        for(const auto& point:portfolio.dayHistoryResult().points)QVERIFY(point.timeMs<future.timeMs);
        compareHistoryPoints(portfolio.dayHistoryResult(),portfolio.dayHistory({},at+40));
        portfolio.requestDayHistory({},at+51);QVERIFY(portfolio.dayHistoryBusy());QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        const auto activated=portfolio.dayHistoryResult();bool foundFuture=false;
        for(const auto& point:activated.points)if(point.timeMs==future.timeMs){foundFuture=true;QCOMPARE(point.value,QString("75"));}
        QVERIFY(foundFuture);QCOMPARE(activated.comparison.difference,QString("50"));
        compareHistoryPoints(activated,portfolio.dayHistory({},at+51));
        auto gap=older;gap.id+="-gap";gap.timeMs=older.timeMs-JointHistoryIntervalMs-DisplayFreshnessMs-1;
        gap.evidence.sourceTimeMs=gap.evidence.receivedAtMs=gap.timeMs;gap.components[0].quantity="25";gap.components[0].evidence=gap.evidence;
        QVERIFY(portfolio.historyLedger()->append({gap},{},{}));portfolio.requestDayHistory({},at+51);
        QVERIFY(portfolio.dayHistoryBusy()); // A ledger revision invalidates even when its row is outside today.
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);const auto gapResult=portfolio.dayHistoryResult();
        // The live capture uses wall time. During the first eight KST minutes,
        // this deliberately older gap belongs to yesterday and must be excluded.
        // Fixed 00:04/00:09 cases in tst_portfolio_history cover both contracts on every run.
        const auto dayStart=portfolioHistoryDayStart(at+51);
        const bool gapToday=gap.timeMs>=dayStart;
        QCOMPARE(gapResult.comparison.difference,gapToday?QString("75"):QString("50"));
        QCOMPARE(gapResult.baseline.timeMs,gapToday?gap.timeMs:older.timeMs);
        compareHistoryPoints(gapResult,portfolio.dayHistory({},at+51));
        bool foundOlder=false,foundGap=false;
        for(const auto& point:gapResult.points){
            QVERIFY(point.timeMs>=dayStart);foundGap=foundGap||point.timeMs==gap.timeMs;
            if(point.timeMs==older.timeMs){foundOlder=true;QVERIFY(!point.connectToPrevious);
                if(gapToday)QVERIFY(point.reason.contains("자료가 없어"));else QVERIFY(point.reason.isEmpty());}
        }
        QVERIFY(foundOlder);QCOMPARE(foundGap,gapToday);
        qInfo("Live KST gap fixture: %s",gapToday?"same-day gap included, difference 75":"previous-day gap excluded, difference 50");
        portfolio.requestDayHistory({},portfolio.snapshots().first().timeMs+DisplayFreshnessMs+1);QVERIFY(!portfolio.dayHistoryBusy());
        QVERIFY(!portfolio.dayHistoryResult().comparison.available);QVERIFY(!portfolio.dayHistoryResult().points.last().available);
        compareHistoryPoints(portfolio.dayHistoryResult(),portfolio.dayHistory({},portfolio.snapshots().first().timeMs+DisplayFreshnessMs+1));
        const auto nextDay=portfolioHistoryDayStart(at)+86400000;
        portfolio.requestDayHistory({},nextDay+1000);QVERIFY(portfolio.dayHistoryBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        const auto afterMidnight=portfolio.dayHistoryResult();
        QVERIFY(!afterMidnight.baseline.available);QVERIFY(!afterMidnight.comparison.available);
        QVERIFY(afterMidnight.contributions.rows.isEmpty());
        for(const auto& point:afterMidnight.points)QVERIFY(point.timeMs>=nextDay);
        compareHistoryPoints(afterMidnight,portfolio.dayHistory({},nextDay+1000));
        portfolio.requestDayHistory({},at+51);QVERIFY(portfolio.dayHistoryBusy());
        vault.lock();QVERIFY(!portfolio.dayHistoryBusy());QVERIFY(portfolio.dayHistoryResult().points.isEmpty());QVERIFY(portfolio.dayHistoryResult().comparison.difference.isEmpty());
        QTest::qWait(50);QVERIFY(!portfolio.dayHistoryResult().comparison.available);QVERIFY(portfolio.dayHistoryResult().points.isEmpty());
    }
    void dayHistoryNamesOnlyActuallyIncompleteConnectionsAndClearsOnRecovery_data(){
        QTest::addColumn<bool>("replaceBinding");QTest::newRow("lock")<<false;QTest::newRow("connection-reset")<<true;
    }
    void dayHistoryNamesOnlyActuallyIncompleteConnectionsAndClearsOnRecovery(){
        QFETCH(bool,replaceBinding);QTemporaryDir dir;QVERIFY(dir.isValid());Vault vault(dir.filePath("synthetic-vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("SYN Ready"),{{"api_key","FakeReadyKey123"},{"secret","FakeReadySecret123"}},StorageMode::EncryptedVault));
        QVERIFY(vault.addAccount(binance("SYN Failed"),{{"api_key","FakeFailedKey123"},{"secret","FakeFailedSecret123"}},StorageMode::EncryptedVault));
        AccountTransport ready,failed;ready.balances=failed.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord& account)->TradingTransport*{return account.alias=="SYN Ready"?&ready:&failed;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        const auto state=[&](const QString& alias){for(const auto& c:portfolio.connections())if(c.account.alias==alias)return c.state;return QString{};};
        QTRY_COMPARE_WITH_TIMEOUT(state("SYN Ready"),QString("complete"),4000);
        QTRY_COMPARE_WITH_TIMEOUT(state("SYN Failed"),QString("complete"),4000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        QString failedId;for(const auto& c:portfolio.connections())if(c.account.alias=="SYN Failed")failedId=c.account.id;
        QVERIFY(!failedId.isEmpty());failed.fail=true;portfolio.refreshConnection(failedId);
        QTRY_COMPARE_WITH_TIMEOUT(state("SYN Failed"),QString("failed"),4000);
        auto at=QDateTime::currentMSecsSinceEpoch()+1000;
        const auto result=portfolio.dayHistory({},at);QVERIFY(!result.comparison.available);
        QCOMPARE(blockedConnectionNames(result),QStringList{"SYN Failed"});
        portfolio.requestDayHistory({},at);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        QCOMPARE(blockedConnectionNames(portfolio.dayHistoryResult()),QStringList{"SYN Failed"});
        failed.fail=false;QVERIFY(portfolio.reconnectConnection(failedId));
        QTRY_COMPARE_WITH_TIMEOUT(state("SYN Failed"),QString("complete"),4000);
        at=QDateTime::currentMSecsSinceEpoch()+1000;portfolio.requestDayHistory({},at);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        QVERIFY(blockedConnectionNames(portfolio.dayHistory({},at)).isEmpty());
        QVERIFY(blockedConnectionNames(portfolio.dayHistoryResult()).isEmpty());
        failed.fail=true;portfolio.refreshConnection(failedId);QTRY_COMPARE_WITH_TIMEOUT(state("SYN Failed"),QString("failed"),4000);
        at=QDateTime::currentMSecsSinceEpoch()+1000;portfolio.requestDayHistory({},at);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);QVERIFY(!blockedConnectionNames(portfolio.dayHistoryResult()).isEmpty());
        if(replaceBinding){
            QVERIFY(vault.replaceCredentials(failedId,{{"api_key","FakeReplacementKey123"},{"secret","FakeReplacementSecret123"}}));
            // This field held an alias immediately before accountsChanged; reset must clear it synchronously.
            QVERIFY(blockedConnectionNames(portfolio.dayHistoryResult()).isEmpty());
            QVERIFY(blockedConnectionNames(portfolio.dayHistory({},at)).isEmpty());
        }
        vault.lock();QVERIFY(blockedConnectionNames(portfolio.dayHistoryResult()).isEmpty());
        QVERIFY(blockedConnectionNames(portfolio.dayHistory({},at)).isEmpty());
        QTest::qWait(20);QVERIFY(blockedConnectionNames(portfolio.dayHistoryResult()).isEmpty());
    }
    void dayHistoryUsesCurrentSelectionAndErasesOnLock() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE day history"), {{"api_key","FakeDayHistoryKey123"},{"secret","FakeDayHistorySecret123"}}, StorageMode::EncryptedVault));
        AccountTransport transport; transport.balances = {QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories; factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord&) {return &transport;};
        PortfolioController portfolio(vault,nullptr,factories); portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        const auto at=portfolio.snapshots().first().timeMs+1000;
        const auto sent=transport.sent;
        const auto result=portfolio.dayHistory({},at);
        QVERIFY2(result.comparison.available,qPrintable(result.comparison.reason));
        QCOMPARE(result.comparison.difference,QString("0"));
        QCOMPARE(result.baseline.timeMs,portfolio.snapshots().first().timeMs);
        QVERIFY(!result.points.isEmpty()); QCOMPARE(transport.sent,sent);
        // An earlier observation lives only in the encrypted archive, with no copying of live profiles.
        auto archived=portfolio.snapshots().first();archived.id+="-archive-fixture";--archived.timeMs;
        archived.evidence.sourceTimeMs=archived.evidence.receivedAtMs=archived.timeMs;
        archived.components[0].quantity="50";archived.components[0].evidence=archived.evidence;
        AssetsLedger archive(&vault,portfolio.historyLedger()->filePath()+".archive-fixture.keefi");
        QVERIFY(archive.open(true));QVERIFY(archive.append({archived},{},{}));archive.lock();
        const auto acrossArchive=portfolio.dayHistory({},at);
        QVERIFY2(acrossArchive.comparison.available,qPrintable(acrossArchive.comparison.reason));
        QCOMPARE(acrossArchive.baseline.timeMs,archived.timeMs);QCOMPARE(acrossArchive.comparison.difference,QString("50"));
        vault.lock();
        const auto locked=portfolio.dayHistory({},at);
        QVERIFY(!locked.comparison.available); QVERIFY(!locked.baseline.available);
        QVERIFY(locked.points.isEmpty()); QVERIFY(locked.comparison.difference.isEmpty());
        QVERIFY(locked.comparison.reason.contains("금고"));
    }
    void dayHistoryInvalidatesSynchronouslyForConnectionChanges_data() {
        QTest::addColumn<QString>("change");
        for(const auto* change:{"add","remove","replace-key"}) QTest::newRow(change)<<QString(change);
    }
    void dayHistoryInvalidatesSynchronouslyForConnectionChanges() {
        QFETCH(QString,change);
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("FAKE day changes"), {{"api_key","FakeDayChangesKey123"},{"secret","FakeDayChangesSecret123"}}, StorageMode::EncryptedVault));
        AccountTransport transport; transport.balances = {QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        PortfolioController::Factories factories; factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord&) {return &transport;};
        PortfolioController portfolio(vault,nullptr,factories); portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        const auto at=portfolio.snapshots().first().timeMs+1000;
        QVERIFY(portfolio.dayHistory({},at).comparison.available);
        portfolio.requestDayHistory({},at);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        QVERIFY(portfolio.dayHistoryResult().comparison.available);
        QVERIFY(portfolio.dayHistoryResult().contributions.available);
        const auto id=vault.accounts().first().id;
        if(change=="remove") QVERIFY(vault.removeAccount(id));
        else if(change=="replace-key") QVERIFY(vault.replaceCredentials(id,{{"api_key","FakeReplacementKey123"},{"secret","FakeReplacementSecret123"}}));
        else QVERIFY(vault.addAccount(binance("FAKE additional"),{{"api_key","FakeAdditionalKey123"},{"secret","FakeAdditionalSecret123"}},StorageMode::EncryptedVault));
        // accountsChanged schedules synchronize; the old value must disappear before that timer runs.
        const auto immediate=portfolio.dayHistory({},at);
        QVERIFY(!immediate.comparison.available); QVERIFY(immediate.comparison.difference.isEmpty());
        QVERIFY(immediate.points.isEmpty()); QVERIFY(!immediate.comparison.reason.isEmpty());
        const auto cached=portfolio.dayHistoryResult();
        QVERIFY(!cached.comparison.available);QVERIFY(cached.comparison.difference.isEmpty());
        QVERIFY(cached.points.isEmpty());QVERIFY(cached.contributions.rows.isEmpty());QVERIFY(!portfolio.dayHistoryBusy());
        QVERIFY(blockedConnectionNames(immediate).isEmpty());QVERIFY(blockedConnectionNames(cached).isEmpty());
        QCoreApplication::processEvents();
        QVERIFY(!portfolio.dayHistory({},at).comparison.available);
        portfolio.requestDayHistory({},at);QVERIFY(!portfolio.dayHistoryBusy());
        QVERIFY(!portfolio.dayHistoryResult().comparison.available);QVERIFY(portfolio.dayHistoryResult().points.isEmpty());
        vault.lock();
    }
    void dayHistoryExclusionsRevalueBothEndsWithoutCachingTheOldResult() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
        AccountRecord account;account.alias="FAKE exclusions";account.provider="phantom";account.environment="mainnet";account.market="solana";account.currency="SOL";
        QVERIFY(vault.addAccount(account,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));
        WalletTransport transport;transport.zeroNativeBalance=true;transport.priceFail=true;
        const QString mint="solana:"+UsdcMint;
        PortfolioController::Factories factories; factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.wallet=[&](const AccountRecord&) {return &transport;};
        PortfolioController portfolio(vault,nullptr,factories); portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),4000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        const auto at=portfolio.snapshots().first().timeMs+1000;
        const auto partial=portfolio.dayHistory({},at);
        QVERIFY2(partial.comparison.available,qPrintable(partial.comparison.reason)); QVERIFY(!partial.comparison.complete);
        const auto excluded=portfolio.dayHistory({mint},at);
        QVERIFY2(excluded.comparison.available,qPrintable(excluded.comparison.reason)); QVERIFY(excluded.comparison.complete);
        QCOMPARE(excluded.comparison.excludedRowCount,1); QCOMPARE(excluded.comparison.difference,QString("0"));
        QVERIFY(!portfolio.dayHistory({},at).comparison.complete);
        portfolio.requestDayHistory({},at);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
        QVERIFY(portfolio.dayHistoryResult().comparison.available);QVERIFY(!portfolio.dayHistoryResult().comparison.complete);
        QVERIFY(portfolio.dayHistoryResult().contributions.available);QVERIFY(!portfolio.dayHistoryResult().contributions.comparison.complete);
        compareHistoryPoints(portfolio.dayHistoryResult(),portfolio.dayHistory({},at));
        portfolio.requestDayHistory({mint},at);QVERIFY(!portfolio.dayHistoryBusy());
        const auto cached=portfolio.dayHistoryResult();QVERIFY(cached.comparison.available);QVERIFY(cached.comparison.complete);
        QCOMPARE(cached.comparison.excludedRowCount,1);QCOMPARE(cached.comparison.difference,QString("0"));
        QVERIFY(cached.contributions.available);QCOMPARE(cached.contributions.comparison.excludedRowCount,1);
        QStringList contributions;for(const auto& group:cached.contributions.groups)contributions.append(group.difference);
        QCOMPARE(sumAssetAmounts(contributions),cached.comparison.difference);compareHistoryPoints(cached,portfolio.dayHistory({mint},at));
        portfolio.requestDayHistory({},at);QVERIFY(!portfolio.dayHistoryResult().comparison.complete);
        vault.lock();
    }
    void restoredFreshBalanceDoesNotJoinHistoryWhileConnectionLoads() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(vault.create(Password));
        for(const auto& alias:{"fast","slow"}) QVERIFY(vault.addAccount(binance(alias),{{"api_key",QString("FakeRestoredKey123")+alias},{"secret","FakeRestoredSecret123"}},StorageMode::EncryptedVault));
        bool holdSlow=false; std::vector<std::unique_ptr<AccountTransport>> transports;
        PortfolioController::Factories factories; factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord& a) {auto t=std::make_unique<AccountTransport>(); t->hold=holdSlow&&a.alias=="slow"; auto* out=t.get(); transports.push_back(std::move(t)); return out;};
        {
            PortfolioController first(vault,nullptr,factories); first.setAutomaticRefresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(first.historyLedger()->snapshots().size(),2,4000);
            QTRY_COMPARE_WITH_TIMEOUT(first.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
            first.stopReads();
        }
        holdSlow=true; transports.clear();
        PortfolioController second(vault,nullptr,factories); second.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT([&]{for(const auto& c:second.connections())if(c.account.alias=="fast")return c.state=="complete";return false;}(),3000);
        QTRY_COMPARE_WITH_TIMEOUT(second.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        QVERIFY(second.historyLedger()->open(false));
        QCOMPARE(second.historyLedger()->snapshots().size(),2);
        second.stopReads(); vault.lock();
    }
    void balancePilotWaitsForRestAndCoalescesChangesDuringReads(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake event balance"),{{"api_key","FakeEventKey123"},{"secret","FakeEventSecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.hold=true;qint64 now=100000;QPointer<test::BalanceSocket> socket;int streams=0;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.monotonicMs=[&]{return now;};
        f.binanceBalances=[&](Vault& v){++streams;auto wire=std::make_unique<test::BalanceSocket>();socket=wire.get();return new BinanceBalanceStream(v,std::move(wire));};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setBinanceBalancePilot(true);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(),1,1000);QCOMPARE(streams,0);
        transport.hold=false;emit transport.finished(transport.pending,transport.answer);
        QTRY_COMPARE_WITH_TIMEOUT(streams,1,3000);const auto at=QDateTime::currentMSecsSinceEpoch();socket->handshake(at);
        for(int i=1;i<=100;++i)socket->event(at+i);
        now+=999;QTest::qWait(300);QCOMPARE(transport.sent.count(Operation::Time),1);
        transport.hold=true;++now;QTRY_COMPARE_WITH_TIMEOUT(transport.sent.count(Operation::Time),2,1500);
        for(int i=101;i<=200;++i)socket->event(at+i);
        now+=1000;QTest::qWait(300);QCOMPARE(transport.sent.count(Operation::Time),2);
        transport.hold=false;emit transport.finished(transport.pending,transport.answer);
        QTRY_COMPARE_WITH_TIMEOUT(transport.sent.count(Operation::Account),3,3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),1500);
        QCOMPARE(streams,1);QVERIFY(portfolio.connections().first().balanceStreamStatus.contains("실시간 잔고 알림"));
        QVERIFY(!portfolio.trading(vault.accounts().first().id)->liveEnabled());
        const auto sent=transport.sent.size();QTest::qWait(400);QCOMPARE(transport.sent.size(),sent);
        vault.lock();QVERIFY(socket.isNull());QVERIFY(portfolio.snapshots().isEmpty());
    }
    void balancePilotStopsWithReadsSecurityAndManualSuspension(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake event stop"),{{"api_key","FakeStopKey123"},{"secret","FakeStopSecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;QPointer<test::BalanceSocket> socket;int streams=0;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        f.binanceBalances=[&](Vault& v){++streams;auto wire=std::make_unique<test::BalanceSocket>();socket=wire.get();return new BinanceBalanceStream(v,std::move(wire));};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setBinanceBalancePilot(true);QCOMPARE(streams,0);
        portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(streams,1,3000);const auto id=vault.accounts().first().id;
        portfolio.stopReads();QVERIFY(socket.isNull());QTest::qWait(300);QCOMPARE(streams,1);
        portfolio.refreshConnection(id);QTRY_COMPARE_WITH_TIMEOUT(streams,2,3000);
        portfolio.suspend(id,true);QVERIFY(socket.isNull());QTest::qWait(300);QCOMPARE(streams,2);
        portfolio.suspend(id,false);QTRY_COMPARE_WITH_TIMEOUT(streams,3,3000);
        portfolio.setAutomaticRefresh(false);QVERIFY(socket.isNull());QTest::qWait(300);QCOMPARE(streams,3);
        portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(streams,4,3000);vault.lock();QVERIFY(socket.isNull());
    }
    void balancePilotUsesOneConnectionAndPollingSurvivesStreamFailure(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake pilot one"),{{"api_key","FakeOneKey123"},{"secret","FakeOneSecret123"}},StorageMode::EncryptedVault));
        QVERIFY(vault.addAccount(binance("fake pilot two"),{{"api_key","FakeTwoKey123"},{"secret","FakeTwoSecret123"}},StorageMode::EncryptedVault));
        AccountTransport one,two;QPointer<test::BalanceSocket> socket;int streams=0;qint64 now=100000;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord& a){return a.alias.endsWith("one")?&one:&two;};f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.monotonicMs=[&]{return now;};
        f.binanceBalances=[&](Vault& v){++streams;auto wire=std::make_unique<test::BalanceSocket>();socket=wire.get();return new BinanceBalanceStream(v,std::move(wire));};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),2,3000);QCOMPARE(streams,0);portfolio.setBinanceBalancePilot(true);QCOMPARE(streams,1);
        emit socket->connected();socket->deliver({{"id",socket->lastId()},{"status",401},{"error",QJsonObject{{"msg","FAKE_PRIVATE"}}}});
        now+=20000;QTRY_COMPARE_WITH_TIMEOUT(one.sent.count(Operation::Account)+two.sent.count(Operation::Account),3,2500);
        ++now;now+=1000;QTRY_COMPARE_WITH_TIMEOUT(one.sent.count(Operation::Account)+two.sent.count(Operation::Account),4,2500);
        QCOMPARE(streams,1);QCOMPARE(socket->opens,1);vault.lock();QVERIFY(socket.isNull());
    }
    void idleAccountPollingDoesNotPublishChanges(){
        QTemporaryDir dir;Vault vault(dir.filePath("quiet-poll"));QVERIFY(vault.create(Password));
        AccountTransport transport;
        PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &transport;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        QVERIFY(vault.addAccount(binance("Synthetic quiet account"),
            {{"api_key","FakeQuietKey123456"},{"secret","FakeQuietSecret123456"}},StorageMode::EncryptedVault));
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
        QTest::qWait(350); // Drain the scheduled account and storage notifications.
        QSignalSpy changes(&portfolio,&PortfolioController::changed);
        QTest::qWait(850);
        QCOMPARE(changes.count(),0);
        portfolio.refreshConnection(vault.accounts().first().id);
        QTRY_VERIFY(!changes.isEmpty());
        QTRY_COMPARE(portfolio.connections().first().state,QString("complete"));
    }
    void automaticRefreshUsesTwentySecondsWithoutOverlappingOrRetryingFailures(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake automatic cadence"),{{"api_key","FakeAutoKey123456"},{"secret","FakeAutoSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;qint64 now=100000;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};
        f.prices=[] {return static_cast<TradingTransport*>(nullptr);};f.monotonicMs=[&]{return now;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QCOMPARE(transport.sent.count(Operation::Account),1);
        now+=19999;QTest::qWait(1100);QCOMPARE(transport.sent.count(Operation::Account),1);
        transport.hold=true;++now;
        QTRY_COMPARE_WITH_TIMEOUT(transport.sent.count(Operation::Time),2,2500);
        now+=60000;QTest::qWait(1100);QCOMPARE(transport.sent.count(Operation::Time),2);
        transport.hold=false;emit transport.finished(transport.pending,transport.answer);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QCOMPARE(transport.sent.count(Operation::Account),2);
        transport.fail=true;now+=20000;
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("failed"),3000);
        const auto calls=transport.sent.size();now+=60000;QTest::qWait(1100);
        QCOMPARE(transport.sent.size(),calls);QCOMPARE(portfolio.snapshots().size(),1);
        portfolio.setAutomaticRefresh(false);portfolio.setAutomaticRefresh(true);
        QTest::qWait(1100);QCOMPARE(transport.sent.size(),calls);
        vault.lock();now+=60000;QTest::qWait(1100);
        QCOMPARE(transport.sent.size(),calls);QVERIFY(portfolio.snapshots().isEmpty());
    }
    void automaticRefreshKeepsUnreviewedServicesAtSixtySeconds(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        auto account=keefi::test::usdmAccount();account.id.clear();account.alias="fake unchanged futures cadence";
        QVERIFY(vault.addAccount(account,{{"api_key","FakeFutureAutoKey123456"},{"secret","FakeFutureAutoSecret123456"}},StorageMode::EncryptedVault));
        LifecycleTransport transport;transport.provider="binance";transport.market="usdm_futures";qint64 now=100000;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};
        f.prices=[] {return static_cast<TradingTransport*>(nullptr);};f.monotonicMs=[&]{return now;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        const auto count=transport.sent.count(Operation::Account);
        now+=20000;QTest::qWait(1100);QCOMPARE(transport.sent.count(Operation::Account),count);
        now+=40000;QTRY_COMPARE_WITH_TIMEOUT(transport.sent.count(Operation::Account),count+1,3000);
        vault.lock();
    }
    void automaticRefreshSpacesConnectionsAndStopsQueuedWork(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake auto one"),{{"api_key","FakeAutoOneKey123456"},{"secret","FakeAutoOneSecret123456"}},StorageMode::EncryptedVault));
        QVERIFY(vault.addAccount(binance("fake auto two"),{{"api_key","FakeAutoTwoKey123456"},{"secret","FakeAutoTwoSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport one,two;qint64 now=100000;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord& a){return a.alias.endsWith("one")?&one:&two;};
        f.prices=[] {return static_cast<TradingTransport*>(nullptr);};f.monotonicMs=[&]{return now;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        const auto complete=[&]{const auto states=portfolio.connections();return states.size()==2&&states[0].state=="complete"&&states[1].state=="complete";};
        QTRY_VERIFY_WITH_TIMEOUT(complete(),3000);
        const auto calls=[&]{return one.sent.count(Operation::Time)+two.sent.count(Operation::Time);};
        QCOMPARE(calls(),2);now+=20000;
        QTRY_COMPARE_WITH_TIMEOUT(calls(),3,2500);QTest::qWait(1100);QCOMPARE(calls(),3);
        portfolio.setAutomaticRefresh(false);now+=1000;QTest::qWait(1100);QCOMPARE(calls(),3);
        portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(calls(),4,2500);
        vault.lock();now+=60000;QTest::qWait(1100);QCOMPARE(calls(),4);
    }
    void automaticRefreshUsesReviewedSpotReadPaths_data(){
        QTest::addColumn<QString>("provider");
        QTest::newRow("upbit")<<QString("upbit");QTest::newRow("okx")<<QString("okx");
    }
    void automaticRefreshUsesReviewedSpotReadPaths(){
        QFETCH(QString,provider);QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AccountRecord account;account.provider=provider;account.alias="fake reviewed spot";account.market="spot";account.environment="live";
        account.currency=provider=="upbit"?"KRW":"USDT";
        QMap<QString,QString> credentials{{"secret","FakeAutoSecret123456"}};
        if(provider=="upbit")credentials["access_key"]="FakeAutoKey123456";
        else{account.region="global";credentials["api_key"]="FakeAutoKey123456";credentials["passphrase"]="Fake auto passphrase";}
        QVERIFY(vault.addAccount(account,credentials,StorageMode::EncryptedVault));
        LifecycleTransport transport;transport.provider=provider;transport.market="spot";qint64 now=100000;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};
        f.prices=[] {return static_cast<TradingTransport*>(nullptr);};f.monotonicMs=[&]{return now;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
        QCOMPARE(transport.sent.count(Operation::Account),1);now+=20000;
        QTRY_COMPARE_WITH_TIMEOUT(transport.sent.count(Operation::Account),2,3000);vault.lock();
    }
    void aPreviouslyFreshPriceRemainsAReferenceAfterAgingAndNetworkFailure(){
        PublicPriceTransport transport;transport.markets={"KRW-PUMP"};PublicKrwPrices prices(&transport);
        const auto sourceTime=QDateTime::currentMSecsSinceEpoch()-177000; // Fresh now, older than the 180 s window within seconds.
        transport.transform=[sourceTime](Operation operation,HttpResult& response){if(operation!=Operation::Quote)return;
            response.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-PUMP"},{"timestamp",QString::number(sourceTime)},{"trade_price","2.75"}}}).toJson();};
        QList<AssetQuote> updates;connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& value){updates=value;});
        AssetSnapshot snapshot;AssetComponent c;c.asset="PUMP";c.quantity="2";snapshot.components.append(c);
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);QCOMPARE(updates.size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(prices.referenceQuotes().size(),1,4000);
        const auto reference=prices.referenceQuotes().first();QCOMPARE(reference.evidence.sourceTimeMs,sourceTime);
        transport.status=503;prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);
        QCOMPARE(prices.report().failure,PublicPriceFailure::Http);QVERIFY(updates.isEmpty());QCOMPARE(prices.referenceQuotes().first(),reference);
        prices.clear();QVERIFY(prices.referenceQuotes().isEmpty());
    }
    void conflictingReferencePricesStayUnavailableUntilANewerSourceTime(){
        PublicPriceTransport transport;transport.markets={"KRW-PUMP"};PublicKrwPrices prices(&transport);
        auto sourceTime=QDateTime::currentMSecsSinceEpoch()-600000;QString price="2.75";
        transport.transform=[&](Operation operation,HttpResult& response){if(operation!=Operation::Quote)return;
            response.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-PUMP"},{"timestamp",QString::number(sourceTime)},{"trade_price",price}}}).toJson();};
        AssetSnapshot snapshot;AssetComponent c;c.asset="PUMP";c.quantity="2";snapshot.components.append(c);
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);QCOMPARE(prices.referenceQuotes().size(),1);
        price="9";prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);
        QVERIFY(prices.referenceQuotes().isEmpty());QCOMPARE(prices.report().failure,PublicPriceFailure::Quote);
        price="2.75";prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);QVERIFY(prices.referenceQuotes().isEmpty());
        ++sourceTime;prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);
        QCOMPARE(prices.referenceQuotes().size(),1);QCOMPARE(prices.referenceQuotes().first().evidence.sourceTimeMs,sourceTime);
    }
    void referencePricesStaySeparateAndInvalidPagesCannotReplaceThem(){
        PublicPriceTransport transport;transport.markets={"KRW-PUMP"};PublicKrwPrices prices(&transport);
        const auto sourceTime=QDateTime::currentMSecsSinceEpoch()-600000;
        transport.transform=[sourceTime](Operation operation,HttpResult& response){if(operation!=Operation::Quote)return;
            auto rows=QJsonDocument::fromJson(response.body).array();auto row=rows[0].toObject();
            row["timestamp"]=QString::number(sourceTime);row["trade_timestamp"]=QString::number(sourceTime-176000);row["trade_price"]="2.75";
            rows[0]=row;response.body=QJsonDocument(rows).toJson();};
        QList<AssetQuote> updates;connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& value){updates=value;});
        AssetSnapshot snapshot;AssetComponent c;c.asset="PUMP";c.quantity="2";snapshot.components.append(c);
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2500);
        QCOMPARE(prices.report().stale,1);QCOMPARE(prices.report().updated,0);QVERIFY(updates.isEmpty());
        QCOMPARE(prices.referenceQuotes().size(),1);const auto reference=prices.referenceQuotes().first();
        QCOMPARE(reference.price,QString("2.75"));QCOMPARE(reference.evidence.sourceTimeMs,sourceTime);
        QVERIFY(reference.evidence.receivedAtMs>sourceTime);QVERIFY(prices.status().contains("참고 가격"));
        transport.transform={};transport.fault="missing";prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);
        QCOMPARE(prices.report().failure,PublicPriceFailure::Missing);QCOMPARE(prices.referenceQuotes().first(),reference);QVERIFY(updates.isEmpty());
        transport.fault.clear();prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);
        QCOMPARE(updates.size(),1);QVERIFY(prices.referenceQuotes().isEmpty());
        // The last fresh source is newer than this stale reply, so retain it.
        transport.stale=true;prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);QVERIFY(prices.referenceQuotes().isEmpty());
        prices.clear();prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2000);QCOMPARE(prices.referenceQuotes().size(),1);
        prices.clear();QVERIFY(prices.referenceQuotes().isEmpty());
    }
    void checkpointFlushesPendingStorageWithoutFinancialRequests() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake checkpoint"),{{"api_key","FakeCheckpointKey123456"},{"secret","FakeCheckpointSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;PortfolioController::Factories factories;
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
        const auto sent=transport.sent;QString error;
        const auto state=portfolio.checkpointRecords(&error);
        QVERIFY(state==PortfolioController::RecordCheckpoint::Waiting||state==PortfolioController::RecordCheckpoint::Ready);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Ready,4000);
        QCOMPARE(portfolio.latestLedger()->snapshots().size(),1);QCOMPARE(portfolio.historyLedger()->snapshots().size(),1);
        QCOMPARE(portfolio.latestLedger()->snapshots().first().timeMs,portfolio.snapshots().first().timeMs);
        QCOMPARE(transport.sent,sent);QVERIFY(error.isEmpty());vault.lock();
        QCOMPARE(portfolio.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Failed);QVERIFY(error.contains("금고"));
    }
    void checkpointRejectsStorageFailure() {
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake failed checkpoint"),{{"api_key","FakeFailedCheckpointKey123456"},{"secret","FakeFailedCheckpointSecret123456"}},StorageMode::EncryptedVault));
        QFile corrupt(dir.filePath("assets-current.keefi"));QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write("fake-corrupt-store"),qint64(18));QVERIFY(corrupt.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner));corrupt.close();
        AccountTransport transport;PortfolioController::Factories factories;
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);const auto sent=transport.sent;QString error;
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Failed,4000);
        QVERIFY(error.contains("기록 저장 실패"));QCOMPARE(transport.sent,sent);
        QVERIFY(corrupt.open(QIODevice::ReadOnly));QCOMPARE(corrupt.readAll(),QByteArray("fake-corrupt-store"));corrupt.close();vault.lock();
    }
    void publicPricesExplainUnsupportedMarkets() {
        PublicPriceTransport transport; transport.markets={"KRW-BTC"}; PublicKrwPrices prices(&transport);
        AssetSnapshot snapshot;
        for(const auto& asset:QStringList{"USDT","PUMP"}) { AssetComponent c;c.asset=asset;c.quantity="1";snapshot.components.append(c); }
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2500);
        QCOMPARE(transport.calls,1);
        QVERIFY2(prices.status().contains("평가 경로 없음"),qPrintable(prices.status()));
        QVERIFY(prices.status().contains("2개"));
        QVERIFY(!prices.status().contains("조회 완료"));
    }
    void solanaMintsRequestUsdcConversionWithoutSymbolGuessing(){
        PublicPriceTransport transport;transport.markets={"KRW-USDC"};PublicKrwPrices prices(&transport);
        AssetSnapshot s;s.market="solana";AssetComponent c;c.asset="solana:"+UsdcMint;c.quantity="1";s.components.append(c);
        c.asset="solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3";c.displaySymbol="FAKE";s.components.append(c);
        prices.refresh({s});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2500);
        QCOMPARE(prices.report().requested,1);QCOMPARE(prices.report().updated,1);QCOMPARE(prices.report().unmapped,0);
    }
    void publicPriceFailureIdentifiesSourceAndStage() {
        PublicPriceTransport transport; transport.markets={"KRW-BTC"};transport.status=503; PublicKrwPrices prices(&transport);
        AssetSnapshot snapshot;AssetComponent c;c.asset="BTC";c.quantity="1";snapshot.components.append(c);
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2500);
        QVERIFY2(prices.status().contains("업비트 현재가"),qPrintable(prices.status()));
        QVERIFY(prices.status().contains("HTTP 503"));
    }
    void publicPriceDiagnostics_data() {
        QTest::addColumn<QString>("fault");QTest::addColumn<bool>("catalogue");
        for(const auto& fault:QStringList{"timeout","network","http","limited","json","invalid"})
            for(bool catalogue:{false,true})QTest::newRow(qPrintable(fault+(catalogue?"-catalogue":"-quotes")))<<fault<<catalogue;
    }
    void publicPriceDiagnostics() {
        QFETCH(QString,fault);QFETCH(bool,catalogue);
        PublicPriceTransport transport;transport.markets={"KRW-BTC"};PublicKrwPrices prices(&transport);
        transport.transform=[&](Operation operation,HttpResult& response){
            if((operation==Operation::Markets)!=catalogue)return;
            if(fault=="timeout"){response.timedOut=true;response.status=0;}
            else if(fault=="network"){response.networkError=true;response.status=0;}
            else if(fault=="http")response.status=503;
            else if(fault=="limited"){response.status=429;response.headers["Retry-After"]="90";}
            if(fault=="invalid")response.body=R"([{"FAKE_PRIVATE_ERROR_MUST_NOT_BE_SHOWN":"secret"}])";
            else response.body="FAKE_PRIVATE_ERROR_MUST_NOT_BE_SHOWN";
        };
        AssetSnapshot snapshot;AssetComponent component;component.asset="BTC";component.quantity="1";snapshot.components.append(component);
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2500);
        const QMap<QString,PublicPriceFailure> kinds{{"timeout",PublicPriceFailure::Timeout},{"network",PublicPriceFailure::Network},
            {"http",PublicPriceFailure::Http},{"limited",PublicPriceFailure::RateLimited},{"json",PublicPriceFailure::Json},
            {"invalid",catalogue?PublicPriceFailure::Catalogue:PublicPriceFailure::Quote}};
        const auto r=prices.report();QCOMPARE(r.failure,kinds.value(fault));QCOMPARE(r.stage,catalogue?PublicPriceStage::Catalogue:PublicPriceStage::Quotes);
        QCOMPARE(r.requested,1);QCOMPARE(r.failed,1);QCOMPARE(r.updated,0);QCOMPARE(r.unsupported,0);QCOMPARE(r.catalogueKnown,!catalogue);
        QVERIFY(!prices.status().contains("FAKE_PRIVATE"));QVERIFY(!prices.details().contains("FAKE_PRIVATE"));
        QVERIFY(prices.details().contains("원인:"));QVERIFY(prices.details().contains("없거나 유효하지 않으면 미평가"));
        if(catalogue)QVERIFY(prices.details().contains("지원 여부는 미확인"));
        if(fault=="limited"){
            QVERIFY(r.retryAtMs>QDateTime::currentMSecsSinceEpoch()+85000);QVERIFY(prices.details().contains("KST"));
            const auto calls=transport.calls;prices.refresh({snapshot});QTest::qWait(30);QCOMPARE(transport.calls,calls);
        }
        prices.clear();QVERIFY(prices.status().isEmpty());QVERIFY(prices.details().isEmpty());QCOMPARE(prices.report().requested,0);
    }
    void mixedPriceOutcomesPreserveCountsAndUnmappedAssets() {
        PublicPriceTransport transport;transport.markets={"KRW-BTC","KRW-ETH"};transport.staleMarkets.insert("KRW-ETH");PublicKrwPrices prices(&transport);
        AssetSnapshot snapshot;
        for(const auto& asset:QStringList{"BTC","ETH","PUMP","fake-chain-mint"}){AssetComponent c;c.asset=asset;c.quantity="1";snapshot.components.append(c);}
        prices.refresh({snapshot});QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),2500);
        const auto r=prices.report();QCOMPARE(r.requested,4);QCOMPARE(r.updated,1);QCOMPARE(r.stale,1);QCOMPARE(r.unsupported,1);QCOMPARE(r.unmapped,1);QCOMPARE(r.failed,0);
        QVERIFY(prices.status().contains("부분"));QVERIFY(prices.details().contains("오래된 가격 1개"));
    }
    void displayedFuturesExposureDoesNotRequestCoinPrices() {
        PublicPriceTransport transport; PublicKrwPrices prices(&transport);
        MarketSnapshot raw; Balance balance; balance.asset="USDT"; balance.equity="0"; raw.balances.append(balance);
        PositionRecord position; position.symbol="BTCUSDT"; position.positionSide="BOTH"; position.amount="2"; raw.positions.append(position);
        const auto capture=captureExchangeAssets(keefi::test::usdmAccount(),"FAKE_PRICE_SCOPE",raw,
            QDateTime::currentMSecsSinceEpoch(),0,true,true,"synthetic",{});
        QCOMPARE(capture.snapshot.components.size(),2);
        prices.refresh({capture.snapshot}); QTest::qWait(30);
        QCOMPARE(transport.calls,0);
    }
    void publicPricesBatchLongMarketNamesWithoutDroppingHoldings(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake long market batch"),{{"api_key","FakeBatchKey123456"},{"secret","FakeBatchSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={};auto* prices=new PublicPriceTransport;prices->markets.clear();
        for(int i=0;i<101;++i){const auto asset=QString("LONGASSET%1").arg(i,15,10,QChar('0'));prices->markets.append("KRW-"+asset);transport.balances.append(QJsonObject{{"asset",asset},{"free","1"},{"locked","0"}});}
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().size(),101,3000);QCOMPARE(prices->calls,3);
        QCOMPARE(portfolio.snapshots().first().components.size(),101);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1000);
        // Every initial page reaches the encrypted cache promptly, not just the
        // first batch. The old ten-second initial debounce failed this bound.
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.latestLedger()->quotes().size(),101,2000);
        // The cycle's terminal signal repeats its last batch and may still
        // have a coalesced initial checkpoint pending. Drain that initial work
        // before isolating the steady-tick write policy below.
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,2000);
        const auto stored=portfolio.latestLedger()->quotes();
        auto tick=stored.first();tick.price="1500";QTest::qWait(2);
        tick.evidence.sourceTimeMs=tick.evidence.receivedAtMs=QDateTime::currentMSecsSinceEpoch();
        portfolio.setPublicPrices({tick},prices->markets);
        const auto updated=portfolio.quotes();
        QVERIFY(std::any_of(updated.cbegin(),updated.cend(),[&](const auto& quote){return quote==tick;}));
        // Once the initial cycle is complete, routine ticks must not create a
        // durable write every 200 ms.
        QTest::qWait(350);QCOMPARE(portfolio.latestLedger()->quotes(),stored);
        vault.lock();
    }
    void futureFxTickReachesPortfolioWithoutAnotherPriceOrBalanceEvent(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake future FX tick"),
            {{"api_key","FakeFutureFxKey123456"},{"secret","FakeFutureFxSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;
        transport.balances={QJsonObject{{"asset","USDT"},{"free","1"},{"locked","0"}}};
        auto* prices=new PublicPriceTransport;prices->markets={"KRW-USDT"};prices->holdQuote=true;
        PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &transport;};
        factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,2500);
        QTRY_VERIFY_WITH_TIMEOUT(prices->heldToken>0,1500);
        portfolio.setAutomaticRefresh(false);
        const auto before=QDateTime::currentMSecsSinceEpoch();
        const AssetQuote previous{"USDT","KRW","1400",
            {"업비트 공식 KRW 현재가","wss://api.upbit.com/websocket/v1","public:upbit:KRW-USDT",
             before-DisplayFreshnessMs+1000,before-DisplayFreshnessMs+1000,true}};
        portfolio.setPublicPrices({previous},prices->markets);
        QCOMPARE(portfolio.quotes().size(),1);
        QVERIFY(AssetValuation::value(portfolio.snapshots(),portfolio.quotes(),"KRW",before,DisplayFreshnessMs).complete);
        const int balanceReads=transport.sent.size(),publicReads=prices->calls;
        auto upcoming=previous;upcoming.price="1401";
        upcoming.evidence.receivedAtMs=QDateTime::currentMSecsSinceEpoch();
        upcoming.evidence.sourceTimeMs=upcoming.evidence.receivedAtMs+1500;
        // This is the real Upbit stream -> PortfolioController public-price
        // entry point. The stream accepts an original clock up to 2 s ahead.
        portfolio.setPublicPrices({upcoming},prices->markets);
        QCOMPARE(portfolio.quotes().first(),previous); // Preserve the usable price until activation.
        QVERIFY(AssetValuation::value(portfolio.snapshots(),portfolio.quotes(),"KRW",
            QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs).complete);
        QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch()>upcoming.evidence.sourceTimeMs,2500);
        // No next tick, balance response or explicit pricing refresh arrives.
        // The accepted candidate must still reach valuation after activation.
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().first().evidence.sourceTimeMs,upcoming.evidence.sourceTimeMs,1000);
        const auto valuation=AssetValuation::value(portfolio.snapshots(),portfolio.quotes(),"KRW",
            QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs,portfolio.referenceQuotes());
        QVERIFY(valuation.complete);QCOMPARE(valuation.rows.first().value,QString("1401"));
        QCOMPARE(transport.sent.size(),balanceReads);QCOMPARE(prices->calls,publicReads);
        vault.lock();
    }
    void futureFxActivationKeepsSixDependentGroupsFresh(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake FX fanout"),
            {{"api_key","FakeFxFanoutKey123456"},{"secret","FakeFxFanoutSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances={};
        const QStringList dependent{"FAKEA","FAKEB","FAKEC","FAKED","FAKEE"};
        QStringList assets=dependent;assets.prepend("USDT");
        for(int i=0;i<7;++i)assets.append("UNKNOWN"+QString::number(i));
        for(const auto& asset:assets)transport.balances.append(QJsonObject{{"asset",asset},{"free","1"},{"locked","0"}});
        auto* prices=new PublicPriceTransport;prices->markets={"KRW-USDT"};prices->binanceAssets=dependent;
        PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::make_unique<OfflinePublicSocket>());
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &transport;};
        factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setBinancePriceStream(&stream);
        const auto before=QDateTime::currentMSecsSinceEpoch();
        const AssetQuote previous{"USDT","KRW","1400",
            {"업비트 공식 KRW 현재가","wss://api.upbit.com/websocket/v1","public:upbit:KRW-USDT",
             before-DisplayFreshnessMs+2000,before-DisplayFreshnessMs+2000,true}};
        portfolio.setPublicPrices({previous},prices->markets);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,1500);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1500);
        portfolio.setAutomaticRefresh(false);
        const auto received=QDateTime::currentMSecsSinceEpoch();QList<PublicTick> ticks;
        for(const auto& asset:dependent){PublicTick tick;tick.market=asset+"USDT";
            tick.quote={asset,"USDT","2",{"Binance 공식 USDT 현재가","wss://stream.binance.com:9443/ws",
                "public:binance:"+tick.market,received,received,true}};ticks.append(tick);}
        emit stream.ticks(ticks);
        QCOMPARE(portfolio.quotes().size(),6);
        const auto counts=[&]{
            const auto valuation=AssetValuation::value(portfolio.snapshots(),portfolio.quotes(),"KRW",
                QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs,portfolio.referenceQuotes());
            QList<int> result{0,0,0};
            for(const auto& row:valuation.rows)if(row.included){
                if(!row.value.isEmpty())++result[0];else if(!row.referenceValue.isEmpty())++result[1];else ++result[2];}
            return result;};
        QCOMPARE(counts(),QList<int>({6,0,7}));
        const auto snapshots=portfolio.snapshots();const int balanceReads=transport.sent.size(),publicReads=prices->calls;
        auto upcoming=previous;upcoming.price="1401";upcoming.evidence.receivedAtMs=QDateTime::currentMSecsSinceEpoch();
        upcoming.evidence.sourceTimeMs=upcoming.evidence.receivedAtMs+1500;
        portfolio.setPublicPrices({upcoming},prices->markets);
        const auto expiredAt=qMax(previous.evidence.sourceTimeMs+DisplayFreshnessMs+1,upcoming.evidence.sourceTimeMs)+20;
        QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch()>expiredAt,3000);
        // A shared FX dependency fans out to six groups. No real T2 symbols,
        // amounts or addresses enter this synthetic reproduction.
        QTRY_COMPARE_WITH_TIMEOUT(counts(),QList<int>({6,0,7}),1000);
        QCOMPARE(portfolio.snapshots(),snapshots);QCOMPARE(transport.sent.size(),balanceReads);QCOMPARE(prices->calls,publicReads);
        vault.lock();
    }
    void futurePublicQuoteActivationHonorsClearAndSubscriptionChanges_data(){
        QTest::addColumn<QString>("action");
        QTest::newRow("clear")<<QString("clear");
        QTest::newRow("other-market")<<QString("other-market");
        QTest::newRow("newer-candidate")<<QString("newer-candidate");
        QTest::newRow("same-source-conflict")<<QString("same-source-conflict");
    }
    void futurePublicQuoteActivationHonorsClearAndSubscriptionChanges(){
        QFETCH(QString,action);PublicPriceTransport transport;transport.markets={"KRW-USDT","KRW-ETH"};
        transport.holdQuote=true;PublicKrwPrices prices(&transport);
        const auto now=QDateTime::currentMSecsSinceEpoch();
        AssetSnapshot snapshot;snapshot.id="fake-activation";snapshot.accountScope="fake-activation";
        snapshot.timeMs=now;snapshot.balanceComplete=true;snapshot.evidence={"fake","https://example.invalid","fake-activation",now,now,true};
        AssetComponent cash;cash.id="cash";cash.asset="USDT";cash.quantity="1";cash.evidence=snapshot.evidence;snapshot.components={cash};
        const AssetQuote previous{"USDT","KRW","1400",{"업비트 공식 KRW 현재가","wss://api.upbit.com/websocket/v1",
            "public:upbit:KRW-USDT",now-1000,now-1000,true}};
        prices.setPublicQuotes({previous},transport.markets);prices.refresh({snapshot});
        QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),1000);
        QList<AssetQuote> delivered;connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& quotes){delivered.append(quotes);});
        auto upcoming=previous;upcoming.price="1401";upcoming.evidence.receivedAtMs=QDateTime::currentMSecsSinceEpoch();
        upcoming.evidence.sourceTimeMs=upcoming.evidence.receivedAtMs+1400;
        prices.setPublicQuotes({upcoming},transport.markets);
        auto latest=upcoming;
        if(action=="clear")prices.clear();
        else if(action=="other-market"){snapshot.components[0].asset="ETH";prices.refresh({snapshot});}
        else if(action=="same-source-conflict"){latest.price="1402";prices.setPublicQuotes({latest},transport.markets);}
        else {latest.price="1402";latest.evidence.sourceTimeMs+=200;prices.setPublicQuotes({latest},transport.markets);}
        QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch()>latest.evidence.sourceTimeMs+50,2500);
        if(action=="newer-candidate"){
            QTRY_VERIFY_WITH_TIMEOUT(!delivered.isEmpty(),700);
            QCOMPARE(delivered.size(),2);QCOMPARE(delivered.first(),upcoming);QCOMPARE(delivered.last(),latest);
        }else QVERIFY(delivered.isEmpty());
        if(action=="same-source-conflict"){
            prices.setPublicQuotes({upcoming},transport.markets); // A retry cannot revive one side.
            QVERIFY(delivered.isEmpty());
            auto recovered=upcoming;recovered.price="1403";
            recovered.evidence.sourceTimeMs=recovered.evidence.receivedAtMs=QDateTime::currentMSecsSinceEpoch();
            prices.setPublicQuotes({recovered},transport.markets);
            QCOMPARE(delivered.size(),1);QCOMPARE(delivered.first(),recovered);
        }
        prices.clear();
    }
    void continuousFutureFxTicksDoNotPostponeEveryActivation(){
        PublicPriceTransport transport;transport.markets={"KRW-USDT"};PublicKrwPrices prices(&transport);
        const auto now=QDateTime::currentMSecsSinceEpoch();
        AssetSnapshot snapshot;snapshot.id="fake-continuous";snapshot.accountScope="fake-continuous";
        snapshot.timeMs=now;snapshot.balanceComplete=true;snapshot.evidence={"fake","https://example.invalid","fake-continuous",now,now,true};
        AssetComponent cash;cash.id="cash";cash.asset="USDT";cash.quantity="1";cash.evidence=snapshot.evidence;snapshot.components={cash};
        AssetQuote seed{"USDT","KRW","1400",{"업비트 공식 KRW 현재가","wss://api.upbit.com/websocket/v1",
            "public:upbit:KRW-USDT",now-1000,now-1000,true}};
        prices.setPublicQuotes({seed},transport.markets);prices.refresh({snapshot});
        QTRY_VERIFY_WITH_TIMEOUT(!prices.busy(),1000);
        QList<AssetQuote> delivered;connect(&prices,&PublicKrwPrices::completed,this,[&](const auto& quotes){delivered.append(quotes);});
        int sent=0;QTimer stream;stream.setInterval(1000);stream.setTimerType(Qt::PreciseTimer);
        const auto send=[&]{auto tick=seed;tick.price=QString::number(1401+sent++);
            tick.evidence.receivedAtMs=QDateTime::currentMSecsSinceEpoch();tick.evidence.sourceTimeMs=tick.evidence.receivedAtMs+1500;
            prices.setPublicQuotes({tick},transport.markets);};
        connect(&stream,&QTimer::timeout,this,send);send();stream.start();
        // At 1 s a later future tick arrives before the first 1.5 s candidate
        // matures. Keeping only the latest candidate starves an active stream.
        QTRY_VERIFY_WITH_TIMEOUT(!delivered.isEmpty(),2600);
        stream.stop();QVERIFY(sent>=2);QCOMPARE(delivered.first().price,QString("1401"));
        QVERIFY(delivered.first().evidence.sourceTimeMs<=QDateTime::currentMSecsSinceEpoch());
        prices.clear();
    }
    void publicPricesCoalesceAndLockRejectsLateReply(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake delayed public prices"),{{"api_key","FakeDelayedKey123456"},{"secret","FakeDelayedSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;auto* prices=new PublicPriceTransport;prices->markets={"KRW-BTC"};prices->holdQuote=true;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(prices->heldToken>0,2500);QCOMPARE(prices->calls,2);
        portfolio.refreshPrices();portfolio.refreshPrices();QTest::qWait(30);QCOMPARE(prices->calls,2);
        const auto calls=transport.sent.size();portfolio.stopReads();emit prices->finished(prices->heldToken,prices->held);QTest::qWait(30);
        QVERIFY(portfolio.quotes().isEmpty());QCOMPARE(portfolio.snapshots().size(),1);
        prices->heldToken=0;portfolio.refreshPrices();QTRY_VERIFY_WITH_TIMEOUT(prices->heldToken>0,1500);
        vault.lock();emit prices->finished(prices->heldToken,prices->held);QTest::qWait(30);
        QVERIFY(portfolio.quotes().isEmpty());QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.pricingStatus().isEmpty());
        QCOMPARE(transport.sent.size(),calls);QCOMPARE(prices->calls,3);
    }
    void publicRateLimitCannotBeBypassedByAnotherRefresh(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake rate limit"),{{"api_key","FakeRateKey123456"},{"secret","FakeRateSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;auto* prices=new PublicPriceTransport;prices->markets={"KRW-BTC"};prices->status=429;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingStatus().contains("429"),2500);
        QVERIFY(portfolio.pricingStatus().contains("90초"));QVERIFY(!portfolio.pricingStatus().contains("FAKE_PRIVATE"));
        portfolio.refreshPrices();portfolio.refreshAll();QTest::qWait(700);QCOMPARE(prices->calls,2);
        QVERIFY(portfolio.quotes().isEmpty());vault.lock();
    }
    void lockedVaultStartsNoPersonalReadsAndLockErasesThem(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake vault gate"),{{"api_key","FakeGateKey123456"},{"secret","FakeGateSecret123456"}},StorageMode::EncryptedVault));
        vault.lock();AccountTransport transport;PortfolioController::Factories factories;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);portfolio.refreshAll();QTest::qWait(50);
        QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.connections().isEmpty());QCOMPARE(transport.sent.size(),0);
        QVERIFY(vault.unlock(Password));QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,1500);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath("assets-current.keefi")),3000);
        QVERIFY(!portfolio.trading(vault.accounts().first().id)->liveEnabled());
        const auto calls=transport.sent.size();vault.lock();QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(vault.accounts().isEmpty());
        portfolio.refreshAll();QTest::qWait(30);QCOMPARE(transport.sent.size(),calls);
    }
    void suspendedBrokersDoNotRestorePreviouslyVerifiedBalances_data() {
        QTest::addColumn<QString>("provider");
        QTest::newRow("with-toss") << QStringLiteral("toss");
        QTest::newRow("with-kiwoom") << QStringLiteral("kiwoom");
    }
    void suspendedBrokersDoNotRestorePreviouslyVerifiedBalances() {
        QFETCH(QString, provider);
        QTemporaryDir dir;
        Vault vault(dir.filePath("credentials.keefi-vault"));
        QVERIFY(keefi::test::openSuspendedBrokerFixture(vault));
        const auto path = dir.filePath("assets-current.keefi");
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/suspended-brokers/assets-current.keefi"), path));
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        {
            AssetsLedger prior(&vault, path);
            QVERIFY(prior.open(false));
            QSet<QString> providers;
            for (const auto& snapshot : prior.snapshots()) {
                QVERIFY(!snapshot.connectionBinding.isEmpty() && snapshot.evidence.complete);
                QVERIFY(std::any_of(snapshot.components.cbegin(), snapshot.components.cend(), [](const auto& c) {
                    return c.asset == "005930" && decimal::positive(c.quantity);
                }));
                providers.insert(snapshot.provider);
            }
            QCOMPARE(providers, (QSet<QString>{"kis","ls"}));
        }
        AccountRecord active;
        active.provider = provider; active.alias = "합성 증권 계좌"; active.environment = "live";
        active.market = "domestic_equity"; active.currency = "KRW";
        const QMap<QString,QString> credentials = provider == "toss"
            ? QMap<QString,QString>{{"client_id","FakeActiveToss123"},{"client_secret","FakeActiveSecret123"}}
            : QMap<QString,QString>{{"app_key","FakeActiveKiwoom123"},{"app_secret","FakeActiveSecret123"}};
        QVERIFY(vault.addAccount(active, credentials, StorageMode::EncryptedVault));
        LifecycleTransport transport;
        transport.provider = active.provider;
        QStringList requested;
        PortfolioController::Factories factories;
        factories.trading = [&](const AccountRecord& record) -> TradingTransport* {
            requested.append(record.provider);
            return &transport;
        };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        portfolio.setAutomaticRefresh(true);
        QCOMPARE(portfolio.connections().size(), 3);
        // REST replies arrive on subsequent event turns. This first view can only contain restored snapshots.
        QVERIFY(portfolio.snapshots().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(), 1, 4000);
        const auto current = portfolio.snapshots().first();
        QCOMPARE(current.provider, active.provider);
        const auto stock = std::find_if(current.components.cbegin(), current.components.cend(), [](const auto& c) {
            return c.kind == AssetComponentKind::Holding && c.asset == "005930";
        });
        QVERIFY(stock != current.components.cend());
        QCOMPARE(stock->quantity, QString("100"));
        QCOMPARE(stock->reportedValue, QString("7000000"));
        QCOMPARE(stock->reportedValueCurrency, QString("KRW"));
        const auto deposit = std::find_if(current.components.cbegin(), current.components.cend(), [](const auto& c) {
            return c.kind == AssetComponentKind::Cash && c.asset == "KRW";
        });
        QVERIFY(deposit != current.components.cend());
        // Toss provides holdings here. Only the Kiwoom reply includes the cash deposit.
        QCOMPARE(deposit->quantity, provider == "kiwoom" ? QString("7000000") : QString{});
        QCOMPARE(current.balanceComplete, provider == "kiwoom");
        QCOMPARE(requested, QStringList{active.provider});
        QVERIFY(transport.sent.contains(Operation::Authenticate));
        for (const auto& connection : portfolio.connections()) {
            if (connection.account.provider == active.provider) QCOMPARE(connection.state, QString("complete"));
            else QCOMPARE(connection.state, QString("unsupported"));
        }
    }
    void assetReadLifecycleAcrossProviders_data(){
        QTest::addColumn<QString>("provider");QTest::addColumn<QString>("market");
        for(const auto& provider:QStringList{"binance","upbit","okx","bithumb","toss","kiwoom"})
            QTest::newRow(qPrintable(provider+"-vault"))<<provider<<QString(QStringList{"toss","kiwoom"}.contains(provider)?"domestic_equity":"spot");
        QTest::newRow("binance-usdm-vault")<<QString("binance")<<QString("usdm_futures");
    }
    void assetReadLifecycleAcrossProviders(){
        QFETCH(QString,provider);QFETCH(QString,market);
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AccountRecord a;a.provider=provider;a.market=market;a.environment="live";a.alias="fake lifecycle";a.currency=market=="domestic_equity"||provider=="upbit"||provider=="bithumb"?"KRW":"USDT";
        QMap<QString,QString> credentials{{"api_key","FakeLifecycleKey123456"},{"secret","FakeLifecycleSecret123456"}};
        if(provider=="upbit")credentials={{"access_key","FakeLifecycleKey123456"},{"secret","FakeLifecycleSecret123456"}};
        else if(provider=="okx"){a.region="global";credentials["passphrase"]="Fake lifecycle passphrase";}
        else if(provider=="toss")credentials={{"client_id","FakeLifecycleClient123456"},{"client_secret","FakeLifecycleSecret123456"}};
        else if(market=="domestic_equity"){
            credentials={{"app_key","FakeLifecycleKey123456"},{"app_secret","FakeLifecycleSecret123456"}};
        }
        const auto mode=StorageMode::EncryptedVault;
        QString error;QVERIFY2(vault.addAccount(a,credentials,mode,&error),qPrintable(error));
        LifecycleTransport transport;transport.provider=provider;transport.market=market;PortfolioController::Factories factories;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        const auto complete=[&]{return !portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete"&&!portfolio.snapshots().isEmpty();};
        QTRY_VERIFY_WITH_TIMEOUT(complete(),7000);
        const auto firstReads=transport.sent.count(Operation::Account)+transport.sent.count(Operation::Holdings);QVERIFY(firstReads>0);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().storageStatus=="마지막 자산 저장됨",3000);
        vault.lock();QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.connections().isEmpty());
        QVERIFY(vault.unlock(Password));
        QTRY_VERIFY_WITH_TIMEOUT(transport.sent.count(Operation::Account)+transport.sent.count(Operation::Holdings)>firstReads,7000);
        QTRY_VERIFY_WITH_TIMEOUT(complete(),7000);vault.lock();
    }
    void unlockRestartsSpotAssetReadWithoutTradingWindow(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake lifecycle"),{{"api_key","FakeLifecycleKey123456"},{"secret","FakeLifecycleSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;PortfolioController::Factories factories;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
        const auto firstReads=transport.sent.count(Operation::Account);QVERIFY(firstReads>0);
        vault.lock();QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.connections().isEmpty());
        QVERIFY(vault.unlock(Password));
        QTRY_VERIFY_WITH_TIMEOUT(transport.sent.count(Operation::Account)>firstReads,3000);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",3000);
        QCOMPARE(portfolio.snapshots().size(),1);QVERIFY(!portfolio.trading(vault.accounts().first().id)->liveEnabled());
        vault.lock();
    }
    void zeroBalancesDoNotRequestPrices(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        auto account=keefi::test::usdmAccount();account.id.clear();
        QVERIFY(vault.addAccount(account,{{"api_key","FakeZeroFuturesKey123456"},{"secret","FakeZeroFuturesSecret123456"}},StorageMode::EncryptedVault));
        FuturesReadTransport transport;transport.zeroBalances=true;auto* prices=new PublicPriceTransport;
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &transport;};factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingStatus().isEmpty(),2000);QCOMPARE(prices->calls,0);
        vault.lock();
    }
    void freshPricesPersistWhileStalePricesPreserveTheLastQuote(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        auto account=keefi::test::usdmAccount();account.id.clear();
        QVERIFY(vault.addAccount(account,{{"api_key","FakePriceFuturesKey123456"},{"secret","FakePriceFuturesSecret123456"}},StorageMode::EncryptedVault));
        FuturesReadTransport transport;auto* prices=new PublicPriceTransport;
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &transport;};factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().size(),1,4000);const auto quote=portfolio.quotes().first();
        prices->stale=true;portfolio.refreshAll();
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingStatus().contains("참고 가격"),4000);
        QCOMPARE(portfolio.quotes().first(),quote);QVERIFY(!portfolio.connections().first().storageStatus.contains("실패"));
        QVERIFY(portfolio.referenceQuotes().isEmpty());vault.lock();QVERIFY(portfolio.referenceQuotes().isEmpty());
    }
    void staleMarketDoesNotDiscardOtherFreshPrices(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake mixed prices"),{{"api_key","FakeMixedPriceKey123456"},{"secret","FakeMixedPriceSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;
        transport.balances.append(QJsonObject{{"asset","USDC"},{"free","2"},{"locked","0"}});
        auto* prices=new PublicPriceTransport;prices->markets={"KRW-BTC","KRW-USDC"};prices->staleMarkets.insert("KRW-USDC");
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &transport;};factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().size(),1,3000);
        QCOMPARE(portfolio.quotes().first().baseAsset,QString("BTC"));
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingStatus().contains("부분"),1500);
        QCOMPARE(portfolio.snapshots().first().components.size(),2);
        QCOMPARE(portfolio.connections().first().state,QString("complete"));
        // Vault-backed reads persist the encrypted latest snapshot; the key never appears in it.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath("assets-current.keefi")),3000);
        {QFile latest(dir.filePath("assets-current.keefi"));QVERIFY(latest.open(QIODevice::ReadOnly));QVERIFY(!latest.readAll().contains("FakeMixedPriceKey123456"));}
        vault.lock();QVERIFY(portfolio.quotes().isEmpty());
    }
    void invalidPricePageDoesNotCommitAnyQuotes_data(){
        QTest::addColumn<QString>("fault");
        for(const auto& fault:QStringList{"missing","duplicate","invalid-price","future-time","unexpected-market"})
            QTest::newRow(qPrintable(fault))<<fault;
    }
    void invalidPricePageDoesNotCommitAnyQuotes(){
        QFETCH(QString,fault);QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake invalid price page"),{{"api_key","FakeInvalidPriceKey123456"},{"secret","FakeInvalidPriceSecret123456"}},StorageMode::EncryptedVault));
        AccountTransport transport;transport.balances.append(QJsonObject{{"asset","USDC"},{"free","2"},{"locked","0"}});
        auto* prices=new PublicPriceTransport;prices->markets={"KRW-BTC","KRW-USDC"};prices->fault=fault;prices->staleMarkets.insert("KRW-BTC");
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &transport;};factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingStatus().contains("실패"),2000);
        QVERIFY(portfolio.quotes().isEmpty());
        QVERIFY(portfolio.referenceQuotes().isEmpty());
        QCOMPARE(portfolio.connections().first().state,QString("complete"));
        vault.lock();
    }
    void futuresAssetsPersistAndReopenWithMultilineCoverage(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        auto account=keefi::test::usdmAccount();account.id.clear();
        QVERIFY(vault.addAccount(account,{{"api_key","FakeFuturesApiKey123456"},{"secret","FakeFuturesSecret123456"}},StorageMode::EncryptedVault));
        FuturesReadTransport transport;PortfolioController::Factories factories;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord&){return &transport;};
        {
            PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
            const auto coverage=portfolio.snapshots().first().coverage;QVERIFY(coverage.contains('\n'));
            QTRY_VERIFY_WITH_TIMEOUT([&]{const auto status=portfolio.connections().first().storageStatus;return status.contains("실패")||status=="마지막 자산 저장됨";}(),3000);
            QVERIFY2(!portfolio.connections().first().storageStatus.contains("실패"),qPrintable(portfolio.connections().first().storageStatus));
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.historyLedger()->snapshots().size(),1,3000);
            QCOMPARE(portfolio.historyLedger()->snapshots().first().coverage,coverage);
            portfolio.stopReads();vault.lock();
        }
        transport.hold=true;QVERIFY(vault.unlock(Password));
        PortfolioController restored(vault,nullptr,factories);restored.setAutomaticRefresh(true);
        QCOMPARE(restored.snapshots().size(),1);QVERIFY(restored.snapshots().first().coverage.contains('\n'));
        vault.lock();
    }
    void accountReadHasNoInstrumentHistoryOrOrderPreparation(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));QVERIFY(vault.addAccount(binance("fake"),{{"api_key","FakeApiKey123456"},{"secret","FakeSecret1234567890"}},StorageMode::EncryptedVault));
        AccountTransport transport;TradingController controller(vault,&transport);controller.setAutomaticRefresh(false);QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);QCOMPARE(transport.sent,QList<Operation>({Operation::Time,Operation::Account}));
        const auto c=controller.assetCapture();QCOMPARE(c.snapshot.components.size(),1);QCOMPARE(c.snapshot.components[0].quantity,QString("0.15"));
        QVERIFY(!controller.liveEnabled());QVERIFY(!controller.connected());
        vault.lock();QVERIFY(controller.assetCapture().snapshot.id.isEmpty());
    }
    void independentConnectionsPublishBeforeSlowerFailureAndKeepEncryptedCache(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fast"),{{"api_key","FakeFastApiKey123456"},{"secret","FakeSecret1234567890"}},StorageMode::EncryptedVault));
        QVERIFY(vault.addAccount(binance("slow"),{{"api_key","FakeSlowApiKey123456"},{"secret","FakeSecret1234567890"}},StorageMode::EncryptedVault));
        std::vector<std::unique_ptr<AccountTransport>> transports;PortfolioController::Factories factories;bool holdAll=false;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord& a){auto t=std::make_unique<AccountTransport>();t->hold=holdAll||a.alias=="slow";auto* out=t.get();transports.push_back(std::move(t));return out;};
        {
            PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);const auto states=portfolio.connections();QVERIFY(std::any_of(states.cbegin(),states.cend(),[](const auto& c){return c.active;}));
            // Only the current cache is written; history waits until both accounts are read together.
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath("assets-current.keefi")),3000);
            QTRY_VERIFY_WITH_TIMEOUT([&]{for(const auto& c:portfolio.connections())if(c.storageStatus=="마지막 자산 저장됨")return true;return false;}(),3000);
            for(const auto& t:transports)if(t->hold){HttpResult failure;failure.status=503;emit t->finished(t->pending,failure);
                QTRY_COMPARE_WITH_TIMEOUT(t->sent.count(Operation::Time),2,2000);
                QCOMPARE(t->sent.count(Operation::Account),0);emit t->finished(t->pending,failure);}
            QTRY_VERIFY_WITH_TIMEOUT([&]{for(const auto& c:portfolio.connections())if(c.state=="failed")return true;return false;}(),2000);QCOMPARE(portfolio.snapshots().size(),1);
            portfolio.stopReads();vault.lock();QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.connections().isEmpty());
        }
        holdAll=true;QVERIFY(vault.unlock(Password));PortfolioController restored(vault,nullptr,factories);restored.setAutomaticRefresh(true);
        QCOMPARE(restored.snapshots().size(),1);QCOMPARE(restored.snapshots()[0].components[0].quantity,QString("0.15"));
        QString fast;for(const auto& account:vault.accounts())if(account.alias=="fast")fast=account.id;
        QVERIFY(vault.replaceCredentials(fast,{{"api_key","FakeRotatedApiKey123456"},{"secret","FakeRotatedSecret1234567890"}}));
        QTRY_VERIFY_WITH_TIMEOUT(restored.snapshots().isEmpty(),2000); // Old-key cache is history, never current assets of a newly configured account.
        vault.lock();
    }
    void coinHistoryContinuesWhileStockIsPendingWithoutJoiningFullGraph(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-coin-stock-history"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("Synthetic ready coin"),{{"api_key","SyntheticJointCoinKey123"},{"secret","SyntheticJointCoinSecret123"}},StorageMode::EncryptedVault));
        AccountRecord stock;stock.provider="toss";stock.alias="Synthetic pending stock";stock.environment="live";stock.market="domestic_equity";stock.currency="KRW";
        QVERIFY(vault.addAccount(stock,{{"client_id","BROKER_FAKE_CLIENT_ID"},{"client_secret","BROKER_FAKE_CLIENT_SECRET"}},StorageMode::EncryptedVault));
        AccountTransport coin;coin.balances={QJsonObject{{"asset","KRW"},{"free","100"},{"locked","0"}}};
        TossReadTransport broker;broker.holdAuthentication=true;PortfolioController::Factories factories;
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord& a)->TradingTransport*{return a.provider=="toss"?static_cast<TradingTransport*>(&broker):&coin;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.historyLedger()->snapshots().size(),1,4000);
        const auto snapshot=portfolio.historyLedger()->snapshots().first();QCOMPARE(snapshot.provider,QString("binance"));
        const auto graph=portfolio.dayHistory({},snapshot.timeMs+1000);QVERIFY(!graph.comparison.available);
        for(const auto& point:graph.points){QVERIFY(!point.available);QVERIFY(!point.connectToPrevious);}
        QCOMPARE(broker.sent.count(Operation::Authenticate),1);QVERIFY(!broker.sent.contains(Operation::Holdings));
        vault.lock();QVERIFY(portfolio.snapshots().isEmpty());
    }
    void historyRecordsAllConnectionsTogether(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fast"),{{"api_key","FakeJointFastKey123456"},{"secret","FakeSecret1234567890"}},StorageMode::EncryptedVault));
        QVERIFY(vault.addAccount(binance("slow"),{{"api_key","FakeJointSlowKey123456"},{"secret","FakeSecret1234567890"}},StorageMode::EncryptedVault));
        std::vector<std::unique_ptr<AccountTransport>> transports;PortfolioController::Factories factories;bool holdSlow=true;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        factories.trading=[&](const AccountRecord& a){auto t=std::make_unique<AccountTransport>();t->hold=holdSlow&&a.alias=="slow";auto* out=t.get();transports.push_back(std::move(t));return out;};
        {
            // One account still loading: no instant holds both, so nothing joins history.
            PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,3000);
            QTRY_VERIFY_WITH_TIMEOUT([&]{for(const auto& c:portfolio.connections())if(c.storageStatus=="마지막 자산 저장됨")return true;return false;}(),3000);
            QTest::qWait(400);QVERIFY(portfolio.historyLedger()->snapshots().isEmpty());
            portfolio.stopReads();
        }
        holdSlow=false;transports.clear();
        PortfolioController both(vault,nullptr,factories);both.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(both.historyLedger()->snapshots().size(),2,4000);
        const auto saved=both.historyLedger()->snapshots();QSet<QString> connections;
        for(const auto& s:saved)connections.insert(s.connectionId);QCOMPARE(connections.size(),2);
        QVERIFY(qAbs(saved[0].timeMs-saved[1].timeMs)<=DisplayFreshnessMs);
        both.stopReads();vault.lock();
    }
    void restoredHoldingsStartPricingBeforeAccountReadCompletes_data(){
        QTest::addColumn<bool>("warm");
        QTest::newRow("public-request") << false;
        QTest::newRow("warm-public-cache") << true;
    }
    void restoredHoldingsStartPricingBeforeAccountReadCompletes(){
        QFETCH(bool,warm);
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        QVERIFY(vault.addAccount(binance("fake restored pricing"),
            {{"api_key","FakeRestoredPriceKey123"},{"secret","FakeRestoredPriceSecret123"}},StorageMode::EncryptedVault));
        AccountTransport transport;
        auto* prices=new PublicPriceTransport;prices->markets={"KRW-BTC"};
        PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &transport;};
        factories.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,2500);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().size(),1,2500);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1000);
        // No explicit export/checkpoint is needed to restore the first priced
        // view after a normal short session.
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.latestLedger()->quotes().size(),1,2000);
        const auto saved=portfolio.snapshots().first();
        const auto savedQuote=portfolio.quotes().first();
        const auto priceCalls=prices->calls;
        vault.lock();QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.quotes().isEmpty());
        transport.hold=true;transport.sent.clear();prices->holdQuote=true;
        if(warm){
            const auto now=QDateTime::currentMSecsSinceEpoch();
            portfolio.setPublicPrices({{"BTC","KRW","1500",
                {"업비트 공식 KRW 현재가","https://api.upbit.com/v1/ticker","public:upbit:KRW-BTC",now,now,true}}},{"KRW-BTC"});
            QVERIFY(portfolio.quotes().isEmpty()); // Public cache never unlocks personal data.
        }
        QVERIFY(vault.unlock(Password));
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,1000);
        QCOMPARE(portfolio.snapshots().first(),saved);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().active,1000);
        QCOMPARE(transport.sent,QList<Operation>({Operation::Time}));
        if(warm){
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.quotes().first().price,QString("1500"),1000);
            QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1000);
            QCOMPARE(prices->calls,priceCalls);
        }else{
            QTRY_VERIFY_WITH_TIMEOUT(prices->heldToken>0,1500);
            QCOMPARE(prices->calls,priceCalls+1);
            QCOMPARE(portfolio.quotes().first(),savedQuote); // Pending refresh cannot renew saved evidence.
        }
        QCOMPARE(portfolio.snapshots().first(),saved); // Still the cache: no balance response was released.
        vault.lock();
        if(prices->heldToken)emit prices->finished(prices->heldToken,prices->held);
        QTest::qWait(30);
        QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.quotes().isEmpty());
    }
    void duplicateTossRegistrationSharesOneAuthenticationAndReadsAssetsOnly(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));AccountRecord account;account.provider="toss";account.alias="fake toss";account.environment="live";account.market="domestic_equity";account.currency="KRW";
        const QMap<QString,QString> credentials{{"client_id","BROKER_FAKE_CLIENT_ID"},{"client_secret","BROKER_FAKE_CLIENT_SECRET"}};
        QVERIFY(vault.addAccount(account,credentials,StorageMode::EncryptedVault));account.alias="same client second registration";QVERIFY(vault.addAccount(account,credentials,StorageMode::EncryptedVault));
        std::vector<std::unique_ptr<TossReadTransport>> transports;PortfolioController::Factories factories;
        factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){auto t=std::make_unique<TossReadTransport>();auto* ptr=t.get();transports.push_back(std::move(t));return ptr;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,6000);
        int authentications=0;for(const auto& t:transports)authentications+=t->sent.count(Operation::Authenticate);QCOMPARE(authentications,1);
        const auto connections=portfolio.connections();QCOMPARE(connections.size(),2);QVERIFY(std::any_of(connections.begin(),connections.end(),[](const auto& c){return c.shared;}));
        QVERIFY(!portfolio.broker(vault.accounts()[0].id)->liveEnabled());vault.lock();QVERIFY(portfolio.snapshots().isEmpty());
    }
    void stoppingAuthenticationDoesNotStartAssetReadsOrReissueToken(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));AccountRecord a;a.provider="toss";a.alias="held auth";a.environment="live";a.market="domestic_equity";a.currency="KRW";
        QVERIFY(vault.addAccount(a,{{"client_id","BROKER_FAKE_CLIENT_ID"},{"client_secret","BROKER_FAKE_CLIENT_SECRET"}},StorageMode::EncryptedVault));
        TossReadTransport transport;transport.holdAuthentication=true;PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(transport.sent.size(),1,1000);
        portfolio.stopReads();emit transport.finished(transport.pending,transport.held);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections()[0].active,2000);QCOMPARE(portfolio.connections()[0].state,QString("stopped"));
        QVERIFY(portfolio.snapshots().isEmpty());QCOMPARE(transport.sent.count(Operation::Authenticate),1);QVERIFY(!transport.sent.contains(Operation::Account));QVERIFY(!transport.sent.contains(Operation::Holdings));
        portfolio.suspend(vault.accounts()[0].id,true);QVERIFY(portfolio.broker(vault.accounts()[0].id)->authenticated());vault.lock();QVERIFY(portfolio.connections().isEmpty());
    }


    void walletResolvesMintMetadataAndPriceIndependently(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AccountRecord a;a.provider="phantom";a.alias="test";a.environment="mainnet";a.market="solana";a.currency="SOL";
        QVERIFY(vault.addAccount(a,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));
        WalletTransport transport;transport.withSkhy=true;WalletReadController controller(vault,&transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));QVERIFY(controller.refresh());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        const auto capture=controller.assetCapture();
        const auto it=std::find_if(capture.snapshot.components.cbegin(),capture.snapshot.components.cend(),[](const auto& c){return c.asset=="solana:"+UsdcMint;});
        QVERIFY(it!=capture.snapshot.components.cend());QCOMPARE(it->displaySymbol,QString("USDC"));
        QCOMPARE(it->displayName,QString("USD Coin"));
        const auto skhy=std::find_if(capture.snapshot.components.cbegin(),capture.snapshot.components.cend(),[](const auto& c){return c.displaySymbol=="SKHY";});
        QVERIFY(skhy!=capture.snapshot.components.cend());QCOMPARE(skhy->asset,QString("solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3"));QCOMPARE(skhy->quantity,QString("2"));
        QVERIFY(std::any_of(capture.quotes.cbegin(),capture.quotes.cend(),[](const auto& q){return q.baseAsset=="solana:"+UsdcMint&&q.quoteAsset=="USD"&&decimal::positive(q.price);}));
        // Metadata and price errors are independent, and neither deletes holdings.
        transport.metadataFail=true;QVERIFY(controller.refresh());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        auto failedMetadata=controller.assetCapture();QCOMPARE(failedMetadata.snapshot.components.size(),3);QCOMPARE(failedMetadata.quotes.size(),2);
        for(const auto& c:failedMetadata.snapshot.components)if(c.asset=="solana:"+UsdcMint){QVERIFY(c.displaySymbol.isEmpty());QVERIFY(!c.metadataIssue.isEmpty());QVERIFY(c.priceIssue.isEmpty());}
        transport.metadataFail=false;transport.priceFail=true;QVERIFY(controller.refresh());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        const auto failedPrice=controller.assetCapture();QCOMPARE(failedPrice.snapshot.components.size(),3);QVERIFY(failedPrice.quotes.isEmpty());
        for(const auto& c:failedPrice.snapshot.components)if(c.asset=="solana:"+UsdcMint){QCOMPARE(c.displaySymbol,QString("USDC"));QVERIFY(c.metadataIssue.isEmpty());QVERIFY(!c.priceIssue.isEmpty());}
        AssetsLedger ledger(&vault,dir.filePath("metadata.keefi"));QVERIFY(ledger.open(true));QVERIFY(ledger.replaceLatest({failedPrice.snapshot},{}));
        ledger.lock();QVERIFY(ledger.open(false));QCOMPARE(ledger.snapshots(),QList<AssetSnapshot>{failedPrice.snapshot});
        transport.holdMetadata=true;const auto count=transport.sent.count(SwapOperation::TokenMetadata);
        QVERIFY(controller.refresh());QTRY_COMPARE_WITH_TIMEOUT(transport.sent.count(SwapOperation::TokenMetadata),count+1,2000);
        vault.lock();emit transport.finished(transport.token,transport.response);QVERIFY(controller.assetCapture().snapshot.id.isEmpty());QVERIFY(!controller.busy());
    }
    void walletKeepsAgedMintPriceAsReference(){
        // T2-b (2026-10-21): Jupiter returned SKHY at a block 207 s old. Policy v2 keeps a
        // public price older than 180 s and within 24 h as a reference, never as current value.
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create(Password));
        AccountRecord a;a.provider="phantom";a.alias="test";a.environment="mainnet";a.market="solana";a.currency="SOL";
        QVERIFY(vault.addAccount(a,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));
        WalletTransport transport;transport.withSkhy=true;transport.blockAgeSeconds=207;WalletReadController controller(vault,&transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));QVERIFY(controller.refresh());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        const auto capture=controller.assetCapture();const QString skhy="solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3";
        const auto quote=std::find_if(capture.quotes.cbegin(),capture.quotes.cend(),[&](const auto& q){return q.baseAsset==skhy;});
        QVERIFY2(quote!=capture.quotes.cend(),"aged Jupiter price was discarded");
        const auto now=QDateTime::currentMSecsSinceEpoch();
        QVERIFY(now-quote->evidence.sourceTimeMs>DisplayFreshnessMs);   // Original block time, not renewed.
        QVERIFY(quote->evidence.receivedAtMs>=quote->evidence.sourceTimeMs);
        const auto component=std::find_if(capture.snapshot.components.cbegin(),capture.snapshot.components.cend(),[&](const auto& c){return c.asset==skhy;});
        QVERIFY(component!=capture.snapshot.components.cend());QVERIFY(component->priceIssue.contains(QStringLiteral("참고")));
        auto quotes=capture.quotes;
        quotes.append({"USDC","KRW","1332",{"업비트 공식 KRW 현재가","https://api.upbit.com/v1/ticker","public:upbit:KRW-USDC",now-1000,now-900,true}});
        const auto result=AssetValuation::value({capture.snapshot},quotes,"KRW",now,DisplayFreshnessMs);
        const auto row=std::find_if(result.rows.cbegin(),result.rows.cend(),[&](const auto& r){return r.asset==skhy;});
        QVERIFY(row!=result.rows.cend());
        QVERIFY(row->value.isEmpty());QVERIFY2(!row->referenceValue.isEmpty(),qPrintable(row->reason)); // Reference, outside the total.
        QVERIFY(!result.complete);
        // Beyond 24 h the price is dropped entirely.
        transport.blockAgeSeconds=ReferenceMaxAgeMs/1000+60;QVERIFY(controller.refresh());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);
        const auto old=controller.assetCapture();
        QVERIFY(std::none_of(old.quotes.cbegin(),old.quotes.cend(),[&](const auto& q){return q.baseAsset==skhy;}));
        for(const auto& c:old.snapshot.components)if(c.asset==skhy)QVERIFY(c.priceIssue.contains(QStringLiteral("만료")));
    }
    void walletReadUsesBothProgramsAndRejectsWrongOwnerAndLateResult_data(){QTest::addColumn<bool>("sessionOnly");QTest::newRow("vault")<<false;}
    void walletReadUsesBothProgramsAndRejectsWrongOwnerAndLateResult(){
        QFETCH(bool,sessionOnly);QTemporaryDir dir;Vault vault(dir.filePath("vault"));if(!sessionOnly)QVERIFY(vault.create(Password));AccountRecord a;a.provider="phantom";a.alias="fake wallet";a.environment="mainnet";a.market="solana";a.currency="SOL";
        QVERIFY(vault.addAccount(a,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));WalletTransport transport;WalletReadController controller(vault,&transport);QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refresh());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);const auto c=controller.assetCapture();QCOMPARE(c.snapshot.components.size(),2);
        QMap<QString,QString> quantities;for(const auto& item:c.snapshot.components)quantities[item.asset]=item.quantity;
        QCOMPARE(quantities.value("SOL"),QString("1.23456789"));QCOMPARE(quantities.value("solana:"+UsdcMint),QString("123456789.012345"));
        QCOMPARE(transport.sent,QList<SwapOperation>({SwapOperation::Genesis,SwapOperation::Balance,SwapOperation::TokenAccounts,SwapOperation::TokenAccounts,SwapOperation::Accounts,SwapOperation::TokenMetadata,SwapOperation::TokenPrices,SwapOperation::BlockTimes}));
        transport.wrongOwner=true;QVERIFY(controller.refresh());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),2000);QVERIFY(controller.status().contains("소유자"));QCOMPARE(controller.assetCapture().snapshot.id,c.snapshot.id);
        transport.hold=true;QVERIFY(controller.refresh());vault.lock();emit transport.finished(transport.token,transport.response);QVERIFY(controller.assetCapture().snapshot.id.isEmpty());
    }
};
QTEST_MAIN(PortfolioTests)
#include "tst_portfolio.moc"
