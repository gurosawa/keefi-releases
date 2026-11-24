#include "keefi/AssetsWindow.h"
#include "keefi/AssetsLedger.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/BinanceEarnProtocol.h"
#include "keefi/BinanceEarnCollector.h"
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
using namespace keefi;
namespace {
class PublicPrices final:public TradingTransport{
public:
    int calls=0;QStringList markets{"KRW-USDT"},requested;QSet<QString> staleMarkets;QString price="1400";void abort()override{}
    void send(quint64 token,const RequestSpec&r)override{
        QVERIFY(!r.authenticated);QVERIFY(r.headers.isEmpty());QVERIFY(r.body.isEmpty());QVERIFY(NetworkTradingTransport::allowed(r));QCOMPARE(r.url.host(),QString("api.upbit.com"));++calls;
        const auto now=QDateTime::currentMSecsSinceEpoch();HttpResult response;response.status=200;QJsonArray rows;
        if(r.operation==Operation::Quote)requested=QUrlQuery(r.url).queryItemValue("markets").split(',');
        for(const auto& market:r.operation==Operation::Markets?markets:requested)
            rows.append(r.operation==Operation::Markets?QJsonObject{{"market",market}}:
                QJsonObject{{"market",market},{"trade_price",price},{"timestamp",QString::number(staleMarkets.contains(market)?now-600000:now)}});
        response.body=QJsonDocument(rows).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class SpotAssets final:public TradingTransport {
public:
    void abort()override{}
    void send(quint64 token,const RequestSpec& r)override{
        QVERIFY(r.operation==Operation::Time||r.operation==Operation::Account);HttpResult response;response.status=200;
        response.body=QJsonDocument(r.operation==Operation::Time?QJsonObject{{"serverTime",QString::number(QDateTime::currentMSecsSinceEpoch())}}:
            QJsonObject{{"canTrade",true},{"accountType","SPOT"},{"balances",QJsonArray{QJsonObject{{"asset","USDT"},{"free","2"},{"locked","0"}}}}}).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class NoAutomaticEarn final:public TradingTransport {
public:
    int calls=0;
    void abort()override{}
    void send(quint64,const RequestSpec&)override{++calls;}
};
AssetCapture capture(QString id,QString env,QString quantity){
    AssetCapture c;auto&s=c.snapshot;s.id=id;s.accountScope=id;s.provider="binance";s.market="spot";s.environment=env;s.timeMs=QDateTime::currentMSecsSinceEpoch();
    s.balanceComplete=s.reconciled=true;s.coverage="fake spot snapshot";s.evidence={"FAKE_OFFICIAL","https://example.invalid","fake",0,s.timeMs,true};
    AssetComponent p;p.id="USDT";p.asset="USDT";p.quantity=quantity;p.evidence=s.evidence;s.components.append(p);return c;
}
}
class AssetsWindowTest final:public QObject{
    Q_OBJECT
private slots:
    void coinPeriodsRenderAfterTabReorder(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-coin-tab-vault"));
        QVERIFY(vault.create("Synthetic Coin Tab! 2026"));
        NoAutomaticEarn wire;PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &wire;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        factories.earn=[&]{return &wire;};factories.flow=[&]{return &wire;};
        PortfolioController portfolio(vault,nullptr,factories);PublicPrices unused;
        AssetsWindow window(vault,nullptr,&unused,portfolio.historyLedger());
        window.setCurrentPortfolio(&portfolio);window.openRecords();
        auto* tabs=window.findChild<QTabWidget*>();QVERIFY(tabs);
        auto* periods=window.findChild<QTableWidget*>("coinPerformancePeriods");QVERIFY(periods);
        auto* page=periods->parentWidget();const int original=tabs->indexOf(page);QVERIFY(original>0);
        tabs->setCurrentWidget(page);QCOMPARE(periods->rowCount(),2);
        tabs->tabBar()->moveTab(original,0);QCOMPARE(tabs->indexOf(page),0);
        tabs->setCurrentIndex(1);tabs->setCurrentWidget(page);
        QCOMPARE(periods->rowCount(),2);
        QCOMPARE(wire.calls,0);QCOMPARE(unused.calls,0);
        vault.lock();QCOMPARE(periods->rowCount(),0);
    }
    void earnRecordLabels_data(){
        QTest::addColumn<int>("source");QTest::addColumn<QString>("wallet");QTest::addColumn<QString>("status");
        QTest::addColumn<qint64>("age");QTest::addColumn<QStringList>("issues");QTest::addColumn<QString>("loss");
        QTest::addColumn<QString>("route");QTest::addColumn<QString>("stateText");
        const auto flexible=int(EarnSource::FlexibleRedemptions),locked=int(EarnSource::LockedRedemptions);
        QTest::newRow("spot")<<flexible<<QString("SPOT")<<QString("PAID")<<qint64(1000)<<QStringList{}<<QString{}<<QString("도착: 현물 지갑")<<QString("지급 완료 기록");
        QTest::newRow("fund-legacy")<<flexible<<QString("FUND")<<QString("PAID")<<qint64(1000)<<QStringList{}<<QString{}<<QString("도착: Funding 지갑")<<QString("지급 완료 기록");
        QTest::newRow("funding")<<flexible<<QString("FUNDING")<<QString("PAID")<<qint64(1000)<<QStringList{}<<QString{}<<QString("도착: Funding 지갑")<<QString("지급 완료 기록");
        QTest::newRow("unknown-wallet")<<flexible<<QString("UNDOCUMENTED_WALLET")<<QString("PAID")<<qint64(1000)<<QStringList{"DESTINATION_UNKNOWN"}<<QString{}<<QString("도착 지갑 확인 필요")<<QString("확인 필요");
        QTest::newRow("early-redemption-profit-loss")<<locked<<QString{}<<QString("PAID")<<qint64(1000)<<QStringList{"DESTINATION_UNKNOWN","EARLY_REDEMPTION_LOSS"}<<QString("2")<<QString("중도 상환으로 줄어든 수익 2")<<QString("확인 필요");
        QTest::newRow("failed")<<flexible<<QString("SPOT")<<QString("FAILED")<<qint64(1000)<<QStringList{"STATUS_UNCONFIRMED"}<<QString{}<<QString("도착: 현물 지갑")<<QString("실패한 요청");
        QTest::newRow("failed-is-not-pending-payout")<<locked<<QString{}<<QString("FAILED")<<qint64(1000)<<QStringList{"DESTINATION_UNKNOWN","STATUS_UNCONFIRMED","PAYOUT_PENDING"}<<QString{}<<QString{}<<QString("실패한 요청");
        QTest::newRow("expired-wait")<<flexible<<QString("SPOT")<<QString("CREATED")<<qint64(8*86400000LL)<<QStringList{"STATUS_UNCONFIRMED"}<<QString{}<<QString("도착: 현물 지갑")<<QString("자동 재조회 기간이 지났어요");
        QTest::newRow("scheduled-not-yet-paid")<<locked<<QString{}<<QString("PAID")<<qint64(1000)<<QStringList{"DESTINATION_UNKNOWN","PAYOUT_PENDING"}<<QString{}<<QString{}<<QString("지급 완료 확인 중");
        QTest::newRow("unknown-provider-status")<<flexible<<QString{}<<QString("UNDOCUMENTED_STATUS")<<qint64(1000)<<QStringList{"STATUS_UNCONFIRMED"}<<QString{}<<QString{}<<QString("거래소 처리 결과 확인 필요");
    }
    void earnRecordLabels(){
        QFETCH(int,source);QFETCH(QString,wallet);QFETCH(QString,status);QFETCH(qint64,age);QFETCH(QStringList,issues);
        QFETCH(QString,loss);QFETCH(QString,route);QFETCH(QString,stateText);
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Earn Labels! 2026"));
        AccountRecord a;a.provider="binance";a.market="spot";a.environment="live";a.currency="USDT";a.alias="synthetic labels";
        QVERIFY(vault.addAccount(a,{{"api_key","SyntheticLabelKey123"},{"secret","SyntheticLabelSecret123"}},StorageMode::EncryptedVault));
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        auto c=capture(QString(64,'a'),"live","2");c.snapshot.connectionId=vault.accounts().first().id;c.snapshot.connectionBinding=QString(64,'b');
        QVERIFY(ledger.append({c.snapshot},{},{}));const auto now=QDateTime::currentMSecsSinceEpoch();
        EarnAccountState state;state.accountScope=c.snapshot.accountScope;state.connectionId=c.snapshot.connectionId;
        state.binding=c.snapshot.connectionBinding;state.trackingStartMs=now-10*86400000LL;state.revision=1;
        EarnRecord r;r.source=EarnSource(source);r.id=QString(64,'c');r.productId="SYN001";r.asset="SYN";r.amount="100";
        r.timeMs=now-age;r.receivedAtMs=now;r.status=status;r.destinationWallet=wallet;r.lossAmount=loss;r.issues=issues;
        if(issues.contains("PAYOUT_PENDING")){r.deliveryMs=now+86400000;r.payoutComplete=false;}
        state.observations.append({r.id,1,r,false});
        // Model a reopened old payload too. Current writes require the canonical
        // schema; FUND is decoded as FUNDING before it reaches that write API.
        EarnAccountState reopened;QVERIFY(EarnTracking::decode(EarnTracking::encode(state),&reopened));state=reopened;
        QString error;QSignalSpy saved(&ledger,&AssetsLedger::collectionWriteFinished);
        QCOMPARE(ledger.saveEarnState(state,0,910,&error),AssetsLedger::CollectionWrite::Pending);QTRY_COMPARE(saved.size(),1);QVERIFY2(saved.first()[1].toBool(),qPrintable(error));
        PublicPrices prices;AssetsWindow window(vault,nullptr,&prices,&ledger);window.openRecords();window.findChild<QTabWidget*>()->setCurrentIndex(4);
        auto* records=window.findChild<QTableWidget*>("earnRecords");QVERIFY(records);QCOMPARE(records->rowCount(),1);
        QVERIFY2(records->item(0,4)->text().contains(route),qPrintable(records->item(0,4)->text()));
        QVERIFY2(records->item(0,5)->text().contains(stateText),qPrintable(records->item(0,5)->text()));
        QVERIFY(records->item(0,1)->text().contains(source==int(EarnSource::FlexibleRedemptions)?"수시형":"기간형"));
        for(int column=0;column<records->columnCount();++column){const auto text=records->item(0,column)->text();
            QVERIFY(!text.contains("UNDOCUMENTED"));QVERIFY(!text.contains("원천 차감"));QVERIFY(!text.contains("도착 SPOT"));}
        if(issues.contains("PAYOUT_PENDING"))QVERIFY(!records->item(0,5)->text().contains("지급 완료 기록"));
        QCOMPARE(prices.calls,0);vault.lock();QCOMPARE(records->rowCount(),0);
    }
    void earnDifferentKeysStaySeparateAndWarnBeforeC1(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Earn Accounts! 2026"));
        for(int i=0;i<2;++i){AccountRecord a;a.provider="binance";a.market="spot";a.environment="live";a.currency="USDT";a.alias="synthetic";
            QVERIFY(vault.addAccount(a,{{"api_key",QString("SyntheticAccountKey%1").arg(i)},{"secret",QString("SyntheticAccountSecret%1").arg(i)}},StorageMode::EncryptedVault));}
        const auto accounts=vault.accounts();QVERIFY(accounts[0].credentialRef!=accounts[1].credentialRef);
        AssetsLedger ledger(&vault,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));const auto now=QDateTime::currentMSecsSinceEpoch();
        for(int i=0;i<2;++i){auto c=capture(QString(64,QChar('a'+i)),"live","2");c.snapshot.connectionId=accounts[i].id;c.snapshot.connectionBinding=QString(64,'d');
            QVERIFY(ledger.append({c.snapshot},{},{}));EarnAccountState state;state.accountScope=c.snapshot.accountScope;state.connectionId=accounts[i].id;
            state.binding=c.snapshot.connectionBinding;state.trackingStartMs=now;state.revision=1;
            EarnRecord r;r.source=EarnSource::FlexiblePositions;r.id=QString(64,QChar('e'+i));r.asset="USDT";r.amount=QString::number((i+1)*100);r.status="OBSERVED";r.receivedAtMs=now;
            state.positions.append(r);state.positionsAt[int(r.source)]=now;QSignalSpy saved(&ledger,&AssetsLedger::collectionWriteFinished);
            QCOMPARE(ledger.saveEarnState(state,0,quint64(920+i)),AssetsLedger::CollectionWrite::Pending);QTRY_COMPARE(saved.size(),1);QVERIFY(saved[0][1].toBool());}
        PublicPrices prices;AssetsWindow window(vault,nullptr,&prices,&ledger);window.openRecords();window.findChild<QTabWidget*>()->setCurrentIndex(4);
        auto* choices=window.findChild<QComboBox*>("earnAccount");QVERIFY(choices);QCOMPARE(choices->count(),2);
        const auto summary=window.findChild<QLabel*>("assetsSummary")->text();auto* rows=window.findChild<QTableWidget*>("earnRecords");
        for(int i=0;i<2;++i){const auto index=choices->findData(accounts[i].id);QVERIFY(index>=0);choices->setCurrentIndex(index);emit choices->activated(index);
            QCOMPARE(rows->rowCount(),1);QCOMPARE(rows->item(0,3)->text(),QString::number((i+1)*100));QCOMPARE(window.findChild<QLabel*>("assetsSummary")->text(),summary);}
        const auto explanation=window.findChild<QLabel*>("earnScope")->text();QVERIFY(explanation.contains("여러 키"));QVERIFY(explanation.contains("중복"));
        QCOMPARE(prices.calls,0);vault.lock();QCOMPARE(choices->count(),0);QCOMPARE(rows->rowCount(),0);
    }
    void earnRecordsRemainSeparateFromTotalsAndClearOnLock(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-earn-ui-vault"));QVERIFY(vault.create("Fake Earn Display! 2026"));
        AccountRecord a;a.provider="binance";a.market="spot";a.environment="live";a.currency="USDT";a.alias="synthetic Earn";
        QVERIFY(vault.addAccount(a,{{"api_key","SyntheticEarnUiKey123"},{"secret","SyntheticEarnUiSecret123"}},StorageMode::EncryptedVault));
        SpotAssets assets;auto* prices=new PublicPrices;NoAutomaticEarn earn;
        PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &assets;};f.prices=[prices]{return prices;};f.earn=[&]{return &earn;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        auto* ledger=portfolio.historyLedger();QTRY_VERIFY_WITH_TIMEOUT(ledger->isOpen()&&!ledger->isWriting()&&!portfolio.snapshots().isEmpty()&&!portfolio.pricingBusy(),4000);
        portfolio.setAutomaticRefresh(false);const auto s=portfolio.snapshots().first();const auto now=QDateTime::currentMSecsSinceEpoch();
        EarnAccountState state;state.accountScope=s.accountScope;state.connectionId=s.connectionId;state.binding=s.connectionBinding;state.trackingStartMs=now-60000;
        const auto store=[&](EarnSource source,const QJsonObject& row,quint64 receipt){
            EarnQuery query{source,earnPositions(source)?0:now-60000,earnPositions(source)?0:now,1,100};
            HttpResult response;response.status=200;response.body=QJsonDocument(QJsonObject{{"rows",QJsonArray{row}},{"total",1}}).toJson();
            const auto page=BinanceEarnProtocol::parsePage(query,response,now);QVERIFY(page.ok);
            EarnPageAccumulator pages;QVERIFY(pages.add(query,page,now));QVERIFY(pages.complete());EarnAccountState next;QString error;
            QVERIFY(vault.withAssetsLedgerKey(false,[&](const SecureBuffer& key){QVERIFY2(EarnTracking::merge(state,pages.batch(),key,&next,&error),qPrintable(error));}));
            QSignalSpy saved(ledger,&AssetsLedger::collectionWriteFinished);QCOMPARE(ledger->saveEarnState(next,state.revision,receipt,&error),AssetsLedger::CollectionWrite::Pending);
            QTRY_COMPARE(saved.size(),1);QVERIFY(saved.first()[1].toBool());state=next;
        };
        store(EarnSource::FlexiblePositions,{{"asset","USDT"},{"productId","USDT001"},{"totalAmount","100"},{"autoSubscribe",true}},901);
        store(EarnSource::FlexibleRewards,{{"asset","USDT"},{"projectId","USDT001"},{"rewards","0.2"},{"type","BONUS"},{"time",QString::number(now-1000)}},902);
        PublicPrices unused;AssetsWindow window(vault,nullptr,&unused,ledger);window.setCurrentPortfolio(&portfolio);window.openRecords();
        auto* tabs=window.findChild<QTabWidget*>();QVERIFY(tabs);QCOMPARE(tabs->tabText(4),QString("Earn 원금·보상"));tabs->setCurrentIndex(4);
        auto* records=window.findChild<QTableWidget*>("earnRecords");QVERIFY(records);QCOMPARE(records->rowCount(),2);
        QCOMPARE(records->item(0,3)->text(),QString("100"));QCOMPARE(records->item(1,3)->text(),QString("0.2"));
        QVERIFY(window.findChild<QLabel*>("assetsSummary")->text().contains("2,800 KRW"));
        QVERIFY(window.findChild<QLabel*>("earnScope")->text().contains("자동 가산하지 않습니다"));
        for(int i=0;i<records->rowCount();++i)for(int j=0;j<records->columnCount();++j){const auto text=records->item(i,j)->text();QVERIFY(!text.contains(s.accountScope));QVERIFY(!text.contains("USDT001"));}
        QCOMPARE(earn.calls,0);QCOMPARE(unused.calls,0);vault.lock();QCOMPARE(records->rowCount(),0);QCOMPARE(window.findChild<QComboBox*>("earnAccount")->count(),0);
        QVERIFY(!window.findChild<QPushButton*>("refreshEarnRecords")->isEnabled());QVERIFY(!portfolio.earnCollector()->busy());
    }
    void recordAmountsUseGroupingWithoutChangingStoredObservations(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-numbers-vault"));QVERIFY(vault.create("Fake Grouped Records! 2026"));
        AssetsLedger ledger(&vault,dir.filePath("synthetic-assets"));PublicPrices transport;AssetsWindow window(vault,nullptr,&transport,&ledger);
        auto original=capture("SYNTHETIC_NUMBERS_SCOPE","live","1234567.125");auto& part=original.snapshot.components[0];
        part.id=part.asset="KRW";part.kind=AssetComponentKind::Cash;part.reportedValue="1234567.125";part.reportedValueCurrency="KRW";part.providerUnrealizedPnl="-12345.625";
        AssetOrderObservation order;order.accountScope=original.snapshot.accountScope;order.provider="binance";order.market="spot";order.orderId="SYNTHETIC_OBSERVED_ORDER";
        order.observedAtMs=original.snapshot.timeMs;order.side="buy";order.type="limit";order.status="partial";order.quantity="1234.125";order.executed="1000.125";
        order.price="12345.6250";order.quoteAmount="12347168.203125";order.quantityUnit="base";order.currency="KRW";order.evidence=original.snapshot.evidence;original.orders.append(order);
        window.record(original);QCOMPARE(ledger.snapshots().size(),1);QCOMPARE(ledger.orders().size(),1);
        const auto storedBytes=[](const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};};
        const auto stored=storedBytes(ledger.filePath());QVERIFY(!stored.isEmpty());
        const auto archivePath=ledger.filePath()+".archive-synthetic.keefi";
        {AssetsLedger archive(&vault,archivePath);QVERIFY(archive.open(true));QVERIFY(archive.append({original.snapshot},{},{}));}
        const auto archived=storedBytes(archivePath);QVERIFY(!archived.isEmpty());
        window.openRecords();auto* values=window.findChild<QTableWidget*>("assetValues");QVERIFY(values);QCOMPARE(values->rowCount(),1);
        QCOMPARE(values->item(0,3)->text(),QString("1,234,567.125"));QCOMPARE(values->item(0,4)->text(),QString("1,234,567.125"));
        QCOMPARE(values->item(0,6)->text(),QString("-12,345.625"));QVERIFY(window.findChild<QLabel*>("assetsSummary")->text().contains("1,234,567.125 KRW"));
        auto* tabs=window.findChild<QTabWidget*>();QVERIFY(tabs);tabs->setCurrentIndex(2);
        auto* history=window.findChild<QTableWidget*>("assetSnapshots");QCOMPARE(history->rowCount(),1);QCOMPARE(history->item(0,3)->text(),QString("1,234,567.125"));QCOMPARE(history->item(0,4)->text(),QString("1,234,567.125"));
        tabs->setCurrentIndex(3);auto* orders=window.findChild<QTableWidget*>("assetOrderObservations");QCOMPARE(orders->rowCount(),1);
        QCOMPARE(orders->item(0,8)->text(),QString("1,234.125"));QCOMPARE(orders->item(0,9)->text(),QString("1,000.125"));
        QCOMPARE(orders->item(0,10)->text(),QString("12,345.625"));QCOMPARE(orders->item(0,11)->text(),QString("12,347,168.203125"));
        QTimer::singleShot(0,&window,[&]{for(auto* dialog:window.findChildren<QInputDialog*>())dialog->accept();});
        window.findChild<QPushButton*>("openAssetArchive")->click();auto* archiveRows=window.findChild<QTableWidget*>("archivedAssetRecords");QVERIFY(archiveRows);QCOMPARE(archiveRows->rowCount(),1);
        QCOMPARE(archiveRows->item(0,4)->text(),QString("1,234,567.125"));QCOMPARE(archiveRows->item(0,5)->text(),QString("1,234,567.125"));
        QCOMPARE(ledger.snapshots(),QList<AssetSnapshot>{original.snapshot});QCOMPARE(ledger.orders(),original.orders);QVERIFY(ledger.quotes().isEmpty());
        QCOMPARE(storedBytes(ledger.filePath()),stored);QCOMPARE(storedBytes(archivePath),archived);QCOMPARE(transport.calls,0);
        vault.lock();QCOMPARE(values->rowCount(),0);QCOMPARE(archiveRows->rowCount(),0);
    }
    void recordsHideSourceUrlsWithoutChangingLedgerOrValuation(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-vault"));QVERIFY(vault.create("Fake Display Evidence! 2026"));
        AssetsLedger ledger(&vault,dir.filePath("synthetic-assets"));PublicPrices transport;
        AssetsWindow window(vault,nullptr,&transport,&ledger);const auto original=capture("SYNTHETIC_ACCOUNT_SCOPE","live","2");
        window.record(original);window.openRecords();window.findChild<QPushButton*>("refreshAssetPrices")->click();
        auto* values=window.findChild<QTableWidget*>("assetValues");QVERIFY(values);
        QTRY_VERIFY_WITH_TIMEOUT(values->rowCount()==1&&values->item(0,4)->text()=="2,800"&&!ledger.quotes().isEmpty(),2500);
        const auto snapshots=ledger.snapshots();const auto quotes=ledger.quotes();
        QCOMPARE(snapshots.first().evidence,original.snapshot.evidence);
        QCOMPARE(snapshots.first().evidence.url,QString("https://example.invalid"));
        QVERIFY(quotes.first().evidence.url.startsWith("https://api.upbit.com/"));
        const auto bytes=[&]{QFile file(ledger.filePath());return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};};
        const auto stored=bytes();QVERIFY(!stored.isEmpty());const auto requests=transport.calls;
        for(auto* table:window.findChildren<QTableWidget*>())for(int row=0;row<table->rowCount();++row)for(int column=0;column<table->columnCount();++column){
            const auto* item=table->item(row,column);if(!item)continue;
            for(const auto& text:{item->text(),item->toolTip(),item->data(Qt::AccessibleTextRole).toString(),item->data(Qt::AccessibleDescriptionRole).toString()}){
                QVERIFY2(!text.contains("://"),qPrintable(text));QVERIFY(!text.contains("SYNTHETIC_ACCOUNT_SCOPE"));
            }
        }
        QVERIFY(values->item(0,4)->toolTip().contains(QStringLiteral("업비트")));QVERIFY(!values->item(0,4)->toolTip().contains('\n'));
        QCOMPARE(values->item(0,4)->text(),QString("2,800"));QVERIFY(ledger.snapshots()==snapshots);QVERIFY(ledger.quotes()==quotes);
        QCOMPARE(bytes(),stored);QCOMPARE(transport.calls,requests);vault.lock();QCOMPARE(values->rowCount(),0);
    }
    void stalePriceIsVisibleAsReferenceButNeverAddedToTotals(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Reference Price! 2026"));
        PublicPrices transport;transport.markets={"KRW-PUMP"};transport.staleMarkets.insert("KRW-PUMP");
        AssetsWindow window(vault,nullptr,&transport);auto c=capture("reference","live","2");
        c.snapshot.components[0].id=c.snapshot.components[0].asset="PUMP";
        auto cash=c.snapshot.components[0];cash.id=cash.asset="KRW";cash.quantity="10";c.snapshot.components.append(cash);
        window.record(c);window.openRecords();window.findChild<QPushButton*>("refreshAssetPrices")->click();
        auto* values=window.findChild<QTableWidget*>("assetValues");auto* summary=window.findChild<QLabel*>("assetsSummary");
        QTRY_VERIFY_WITH_TIMEOUT(transport.calls==2&&!window.findChild<QPushButton*>("refreshAssetPrices")->isEnabled(),2000);
        QTRY_COMPARE_WITH_TIMEOUT(values->item(0,4)->text(),QString("참고 2,800"),2000);
        QVERIFY(values->item(0,8)->text().contains("합계 제외"));QVERIFY(values->item(0,8)->text().contains("KST"));
        QVERIFY(summary->text().contains("부분합 10 KRW"));QVERIFY(!summary->text().contains("2,810"));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("refreshAssetPrices")->isEnabled(),1500);
        transport.staleMarkets.clear();transport.price="1500";window.findChild<QPushButton*>("refreshAssetPrices")->click();
        QTRY_COMPARE_WITH_TIMEOUT(values->item(0,4)->text(),QString("3,000"),2500);
        QVERIFY(summary->text().contains("순자산 3,010 KRW"));QVERIFY(!values->item(0,8)->text().contains("참고"));
        vault.lock();QCOMPARE(values->rowCount(),0);
    }
    void recordsWindowReusesThePortfolioPricingQueueAndCatalogue(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Shared Prices! 2026"));
        AccountRecord account;account.provider="binance";account.market="spot";account.environment="live";account.currency="USDT";account.alias="fake shared prices";
        QVERIFY(vault.addAccount(account,{{"api_key","FakeSharedPriceKey123456"},{"secret","FakeSharedPriceSecret123456"}},StorageMode::EncryptedVault));
        SpotAssets assets;auto* prices=new PublicPrices;PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &assets;};f.prices=[prices]{return prices;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.historyLedger()->isOpen(),2500);
        PublicPrices unused;AssetsWindow window(vault,nullptr,&unused,portfolio.historyLedger());window.setCurrentPortfolio(&portfolio);window.openRecords();
        auto* summary=window.findChild<QLabel*>("assetsSummary");QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("2,800"),2500);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1500);QCOMPARE(prices->calls,2);
        prices->price="1500";window.findChild<QPushButton*>("refreshAssetPrices")->click();
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("3,000"),2500);QCOMPARE(prices->calls,3);QCOMPARE(unused.calls,0);
        QCOMPARE(portfolio.quotes().first().price,QString("1500"));vault.lock();
    }
    void closingAnUnlockedRecordsWindowDoesNotInvalidateTheVault(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Closing Assets! 2026"));PublicPrices transport;
        auto window=std::make_unique<AssetsWindow>(vault,nullptr,&transport);window->record(capture("close","live","2"));window->openRecords();
        window.reset();QVERIFY(vault.isUnlocked());QCOMPARE(transport.calls,0);vault.lock();
    }
    void mixedPricesRetainFreshValuationsAndSkipZeroHoldings(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Mixed Assets! 2026"));
        PublicPrices transport;transport.markets={"KRW-USDT","KRW-USDC","KRW-BTC"};transport.staleMarkets.insert("KRW-USDC");
        AssetsWindow window(vault,nullptr,&transport);auto c=capture("mixed","live","2");
        auto stale=c.snapshot.components.first();stale.id=stale.asset="USDC";stale.quantity="1";c.snapshot.components.append(stale);
        auto zero=stale;zero.id=zero.asset="BTC";zero.quantity="0";c.snapshot.components.append(zero);window.record(c);window.openRecords();
        window.findChild<QPushButton*>("refreshAssetPrices")->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("assetsSummary")->text().contains("2,800"),2500);
        QVERIFY(!transport.requested.contains("KRW-BTC"));QCOMPARE(transport.requested.size(),2);
        // Cached/page prices paint before the complete cycle's diagnostic arrives.
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("assetFeedback")->text().contains("부분"),2500);
        QCOMPARE(window.findChild<QTableWidget*>("assetValues")->rowCount(),3);
        const int calls=transport.calls;window.findChild<QPushButton*>("assetPriceDetails")->click();
        auto* detail=window.findChild<QMessageBox*>("publicPriceDetails");QVERIFY(detail);QVERIFY(detail->isVisible());
        QVERIFY(detail->informativeText().contains("갱신 1개"));QVERIFY(detail->informativeText().contains("오래된 가격 1개"));
        QCOMPARE(transport.calls,calls);vault.lock();QVERIFY(detail->informativeText().isEmpty());QVERIFY(!detail->isVisible());
    }
    void zeroHoldingsDoNotRequestPublicPrices(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Zero Assets! 2026"));
        PublicPrices transport;AssetsWindow window(vault,nullptr,&transport);window.record(capture("zero","live","0"));window.openRecords();
        window.findChild<QPushButton*>("refreshAssetPrices")->click();QTest::qWait(30);QCOMPARE(transport.calls,0);vault.lock();
    }
    void currentViewNeverTotalsHistoricalAccountsWithoutCurrentOwnership(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Assets Window! 2026"));PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        auto* history=portfolio.historyLedger();QVERIFY(history->open(true));const auto old=capture("historical-old-key","live","999");QVERIFY(history->append({old.snapshot},{},{}));
        PublicPrices prices;AssetsWindow window(vault,nullptr,&prices,history);window.setCurrentPortfolio(&portfolio);window.openRecords();
        QCOMPARE(window.findChild<QTableWidget*>("assetValues")->rowCount(),0);QCOMPARE(history->snapshots().size(),1);QCOMPARE(prices.calls,0);
        vault.lock();QCOMPARE(window.findChild<QTableWidget*>("assetValues")->rowCount(),0);QVERIFY(portfolio.snapshots().isEmpty());
    }
    void storedScopeAndPublicPricesStaySeparateFromTrading(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Assets Window! 2026"));PublicPrices transport;AssetsWindow window(vault,nullptr,&transport);
        window.record(capture("fake-live","live","2"));window.record(capture("fake-demo","demo","999"));window.openRecords();QCOMPARE(transport.calls,0);
        auto* values=window.findChild<QTableWidget*>("assetValues");auto* summary=window.findChild<QLabel*>("assetsSummary");QCOMPARE(values->rowCount(),1);QCOMPARE(values->item(0,3)->text(),QString("2"));
        window.findChild<QPushButton*>("refreshAssetPrices")->click();QTRY_COMPARE_WITH_TIMEOUT(transport.calls,2,3000);
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("2,800"),3000);QVERIFY(window.findChild<QLabel*>("assetPerformance")->text().contains("자료 부족"));
        window.findChild<QComboBox*>("assetEnvironment")->setCurrentIndex(1);QCOMPARE(values->rowCount(),1);QCOMPARE(values->item(0,3)->text(),QString("999"));
        vault.lock();QCOMPARE(values->rowCount(),0);QVERIFY(!window.findChild<QPushButton*>("refreshAssetPrices")->isEnabled());QCOMPARE(transport.calls,2);
    }
};
QTEST_MAIN(AssetsWindowTest)
#include "tst_assets_window.moc"
