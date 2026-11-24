#include "fixtures/PortfolioTestRow.h"
#include "keefi/AccountModel.h"
#include "keefi/AssetsLedger.h"
#include "keefi/AssetShareDialog.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/Vault.h"
#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QUrlQuery>
#include <algorithm>

using keefi::test::PortfolioTestRow;
using keefi::test::portfolioRow;

using namespace keefi;
namespace {
constexpr qint64 Interval=900000;
class WalletHttp final:public SwapTransport {
public:
    QString lamports;
    explicit WalletHttp(QString value):lamports(std::move(value)){}
    void abort()override{}
    void send(quint64 token,const SwapRequest& request)override{
        QVERIFY(request.operation==SwapOperation::Genesis||request.operation==SwapOperation::Balance||request.operation==SwapOperation::TokenAccounts);
        const auto body=QJsonDocument::fromJson(request.body).object();QJsonValue result;
        if(request.operation==SwapOperation::Genesis)result="5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d";
        else if(request.operation==SwapOperation::Balance)result=QJsonObject{{"context",QJsonObject{{"slot","100"}}},{"value",lamports}};
        else result=QJsonObject{{"context",QJsonObject{{"slot","101"}}},{"value",QJsonArray{}}};
        HttpResult response;response.status=200;response.body=QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",body.value("id")},{"result",result}}).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class PublicHttp final:public TradingTransport {
public:
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());HttpResult response;response.status=200;
        if(request.operation==Operation::Markets)response.body=R"([{"market":"KRW-SOL","korean_name":"솔라나","english_name":"Solana","market_event":{"warning":false,"caution":{}}}])";
        else response.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-SOL"},{"trade_price","200000"},
            {"prev_closing_price","190000"},{"signed_change_rate","0.05"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch())}}}).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class CandleHttp final:public TradingTransport {
public:
    quint64 token=0;int requests=0;int offsetIntervals=0;
    void abort()override{}
    void send(quint64 next,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(MarketCandles::isRequest(request));
        QCOMPARE(request.url.host(),QString("api.upbit.com"));QCOMPARE(request.url.path(),QString("/v1/candles/minutes/15"));
        QCOMPARE(QUrlQuery(request.url).queryItemValue("market"),QString("KRW-SOL"));token=next;++requests;
    }
    void complete(quint64 next){
        QJsonArray rows;const auto end=QDateTime::currentMSecsSinceEpoch()/Interval*Interval-offsetIntervals*Interval;
        for(int i=200;i>0;--i){const auto at=end-i*Interval;const auto price=QString::number(190000+200-i);
            rows.append(QJsonObject{{"market","KRW-SOL"},{"unit","15"},{"timestamp",QString::number(at+1000)},
                {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                {"opening_price",price},{"high_price",price},{"low_price",price},{"trade_price",price},{"candle_acc_trade_volume","2"}});}
        HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit finished(next,response);
    }
};
}
class AssetShareIntegrationTest final:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){
        QVERIFY(settings_.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings_.path());
        QVERIFY(KeefiFonts::registerBundledFonts());
    }
    void selectedHoldingStaysScopedAndLockRevokesLatePublicReplies_data(){
        QTest::addColumn<bool>("completeBeforeLock");QTest::addColumn<bool>("withToday");QTest::addColumn<bool>("delayed");
        QTest::newRow("ready")<<true<<false<<false;QTest::newRow("waiting")<<false<<false<<false;
        QTest::newRow("today-after-midnight")<<true<<true<<false;QTest::newRow("delayed-public")<<true<<false<<true;
    }
    void selectedHoldingStaysScopedAndLockRevokesLatePublicReplies(){
        QFETCH(bool,completeBeforeLock);QFETCH(bool,withToday);QFETCH(bool,delayed);QTemporaryDir temporary;Vault vault(temporary.filePath("synthetic.bin"));
        QVERIFY(vault.create("Synthetic Share! 2026"));AccountRecord account;account.provider="phantom";
        account.alias="SYNTHETIC_FIRST";account.environment="mainnet";account.market="solana";account.currency="SOL";
        QVERIFY(vault.addAccount(account,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));
        account.alias="SYNTHETIC_SECOND";
        QVERIFY(vault.addAccount(account,{{"address","So11111111111111111111111111111111111111112"}},StorageMode::EncryptedVault));
        WalletHttp first("3000000000"),second("8000000000");PublicHttp marketHttp;CandleHttp candles;candles.offsetIntervals=delayed?1:0;
        PortfolioController::Factories factories;factories.prices=[]{return new PublicHttp;};
        factories.wallet=[&](const AccountRecord& value)->SwapTransport*{return value.alias=="SYNTHETIC_FIRST"?&first:&second;};
        PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);
        PublicMarketCatalog market(&marketHttp);ChartStudyFeed feed(&candles);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
        window.setPublicMarketCatalog(&market);window.setChartStudyFeed(&feed);window.resize(1400,1000);window.show();
        const auto cleanup=qScopeGuard([&]{window.lockAndClear();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);});
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");QVERIFY(pages);
        auto* home=window.findChild<QWidget*>("portfolioHome");QVERIFY(home);pages->setCurrentWidget(home);
        auto* tree=window.findChild<QTreeView*>("portfolioAssets");QVERIFY(tree);
        const auto group=[&]() -> PortfolioTestRow {for(int i=0;i<tree->model()->rowCount();++i){const auto identity=portfolioRow(tree, i).data(0,Qt::UserRole+1).toString();if(identity=="exchange:SOL"||identity=="solana:native")return portfolioRow(tree, i);}return {};};
        QTRY_VERIFY_WITH_TIMEOUT(([&]{const auto values=portfolio.connections();return values.size()==2 && std::all_of(values.cbegin(),values.cend(),[](const auto& item){return item.state=="complete";});}()),5000);
        QTRY_VERIFY(market.marketListAvailable());QTRY_VERIFY_WITH_TIMEOUT(group()&&group().childCount()==2,5000);
        PortfolioTestRow selected;for(int i=0;i<group().childCount();++i)if(group().child(i).text(1).contains("SYNTHETIC_FIRST"))selected=group().child(i);
        QVERIFY(selected);tree->setCurrentIndex(selected.modelIndex());QCoreApplication::processEvents();
        if(withToday){
            // Seed one explicit synthetic joint checkpoint in this temporary
            // ledger; the production five-minute writer cadence stays unchanged.
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
            qint64 known=0;for(const auto& value:portfolio.snapshots())known=qMax(known,value.timeMs);
            for(const auto& quote:portfolio.quotes())known=qMax(known,quote.evidence.receivedAtMs);
            auto baseline=portfolio.snapshots();int seeded=0;
            for(auto& value:baseline){
                value.id+="-synthetic-share-joint";value.timeMs=known;
                for(auto& component:value.components)if(component.id=="solana:native"){
                    if(component.quantity=="3")component.quantity="1";
                    else{QCOMPARE(component.quantity,QString("8"));component.quantity="7";}
                    ++seeded;
                }
            }
            QCOMPARE(seeded,2);
            QVERIFY(portfolio.historyLedger()->append(baseline,{},portfolio.quotes()));
            const auto previous=known;
            portfolio.refreshAll();
            QTRY_VERIFY_WITH_TIMEOUT(([&]{const auto values=portfolio.snapshots();return values.size()==2&&std::all_of(values.cbegin(),values.cend(),[&](const auto& value){return value.timeMs>previous;});}()),5000);
            QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(),PortfolioController::RecordCheckpoint::Ready,4000);
            qint64 at=0;for(const auto& state:portfolio.connections())at=qMax(at,state.receivedAtMs);
            for(const auto& quote:portfolio.quotes())at=qMax(at,quote.evidence.receivedAtMs);
            portfolio.requestDayHistory({},at);
            QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),2000);
            QVERIFY2(portfolio.dayHistoryResult().comparison.available,qPrintable(portfolio.dayHistoryResult().comparison.reason));
            QVERIFY(portfolio.dayHistoryResult().comparison.fromMs>portfolioHistoryDayStart(at));
            QVERIFY(portfolio.dayHistoryResult().comparison.fromMs<portfolio.dayHistoryResult().comparison.toMs);
            emit portfolio.changed();QTest::qWait(10);
        }
        auto* share=window.findChild<QPushButton*>("assetDetailShare");
        QVERIFY2(share,"My Assets needs its own public-price asset card entry");QVERIFY(share->isVisible());
        // The card freezes the comparison at opening; later public callbacks can
        // advance the controller's cache without rewriting that private input.
        const auto expectedToday=portfolio.dayHistoryResult().contributions.comparison;
        if(withToday){QVERIFY(expectedToday.available);QCOMPARE(expectedToday.difference,QString("600000"));}
        share->click();
        QTRY_VERIFY(window.findChild<AssetShareDialog*>());QPointer<AssetShareDialog> dialog=window.findChild<AssetShareDialog*>();
        QVERIFY(dialog->isVisible());QVERIFY(!dialog->options().includeQuantity);QVERIFY(!dialog->options().includeValuation);
        QVERIFY(!dialog->options().includeCustody);QVERIFY(!dialog->options().includeTodayObservation);
        QTRY_COMPARE(feed.ownerCount("coin:upbit:KRW-SOL",Interval),1);QTRY_VERIFY(candles.token>0);const auto pending=candles.token;
        if(completeBeforeLock){
            candles.complete(pending);QTRY_VERIFY(dialog->previewAvailable());const auto snapshot=dialog->snapshot();
            QCOMPARE(snapshot.market.identity.key(),QString("coin:upbit:KRW-SOL"));QCOMPARE(snapshot.market.closed.size(),200);
            QCOMPARE(snapshot.market.viewport.toMs-snapshot.market.viewport.fromMs,96*Interval);
            QVERIFY(snapshot.personal);QCOMPARE(snapshot.personal->quantity,QString("3"));QCOMPARE(snapshot.personal->currentValuation,QString("600000"));
            QCOMPARE(snapshot.personal->currency,QString("KRW"));
            QCOMPARE(snapshot.market.availability,delayed?ChartShareAvailability::Stale:ChartShareAvailability::Ready);
            if(withToday){QVERIFY(snapshot.personal->today);QCOMPARE(snapshot.personal->today->difference,QString("400000"));
                QCOMPARE(snapshot.personal->today->fromMs,expectedToday.fromMs);
                QCOMPARE(snapshot.personal->today->toMs,expectedToday.toMs);}
            else QVERIFY(!dialog->options().includeTodayObservation);
            const auto publicImage=dialog->previewImage();auto* include=dialog->findChild<QCheckBox*>("assetShareQuantity");QVERIFY(include);QVERIFY(include->isEnabled());
            include->click();QVERIFY(dialog->options().includeQuantity);QVERIFY(dialog->previewImage()!=publicImage);
        }
        window.lockAndClear();QCOMPARE(feed.ownerCount("coin:upbit:KRW-SOL",Interval),0);
        QTRY_VERIFY(dialog.isNull());
        candles.complete(pending);QCoreApplication::processEvents();
        QVERIFY(!window.findChild<AssetShareDialog*>());QCOMPARE(feed.ownerCount("coin:upbit:KRW-SOL",Interval),0);
    }
private:
    QTemporaryDir settings_;
};
QTEST_MAIN(AssetShareIntegrationTest)
#include "tst_asset_share_integration.moc"
