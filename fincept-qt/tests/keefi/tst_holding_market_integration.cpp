#include "fixtures/PortfolioTestRow.h"
#include "ChartInstrumentPanelTestSupport.h"
#include "keefi/AccountModel.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketCandles.h"
#include "keefi/PortfolioController.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <algorithm>

using keefi::test::PortfolioTestRow;
using keefi::test::portfolioRow;
using namespace keefi;
namespace {
const QString PhantomScope=QStringLiteral("Phantom은 지갑에 바로 있는 SOL만 읽어요. 스테이킹한 SOL은 아직 포함하지 않아요. jitoSOL 같은 스테이킹 토큰은 별도 토큰으로 보여요.");
const QString SessionMarketGuidance=QStringLiteral("여기서 고른 시장은 이번 실행에만 기억해요. 계속 쓰려면 그 시장을 관심 목록에 추가하세요.");
QVariantMap settingsValues(QSettings& settings){
    settings.sync();QVariantMap values;for(const auto& key:settings.allKeys())values.insert(key,settings.value(key));return values;
}
QString visibleLabels(QWidget* root){
    QStringList values;for(const auto* label:root->findChildren<QLabel*>())if(label->isVisibleTo(root))values.append(label->text());return values.join('\n');
}
class WalletHttp final:public SwapTransport {
public:
    void abort()override{}
    void send(quint64 token,const SwapRequest& request)override{
        QVERIFY(request.operation==SwapOperation::Genesis||request.operation==SwapOperation::Balance||request.operation==SwapOperation::TokenAccounts);
        const auto body=QJsonDocument::fromJson(request.body).object();QJsonValue result;
        if(request.operation==SwapOperation::Genesis)result="5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d";
        else if(request.operation==SwapOperation::Balance)result=QJsonObject{{"context",QJsonObject{{"slot","100"}}},{"value","1234567890"}};
        else result=QJsonObject{{"context",QJsonObject{{"slot","101"}}},{"value",QJsonArray{}}};
        HttpResult response;response.status=200;response.body=QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",body.value("id")},{"result",result}}).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class ExchangeSolHttp final:public TradingTransport {
public:
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QCOMPARE(request.operation,Operation::Account);QVERIFY(request.authenticated);
        HttpResult response;response.status=200;response.body=R"([{"currency":"SOL","balance":"2","locked":"0","avg_buy_price":"100000","avg_buy_price_modified":false,"unit_currency":"KRW"}])";
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class CatalogHttp final:public TradingTransport {
public:
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());HttpResult response;response.status=200;
        if(request.url.host()=="api.binance.com")response.body=R"({"symbols":[{"symbol":"SOLUSDT","baseAsset":"SOL","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})";
        else if(request.operation==Operation::Markets)response.body=R"([{"market":"KRW-SOL","korean_name":"솔라나","english_name":"Solana","market_event":{"warning":false,"caution":{}}}])";
        else response.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-SOL"},{"trade_price","200000"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch())}}}).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class CandleHttp final:public TradingTransport {
public:
    QList<RequestSpec> requests;
    void abort()override{}
    void send(quint64,const RequestSpec& request)override{QVERIFY(!request.authenticated);QVERIFY(MarketCandles::isRequest(request));requests.append(request);}
};
}
class HoldingMarketIntegrationTest final:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){
        QVERIFY(settings_.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("KEEFI Synthetic Tests");
        QCoreApplication::setApplicationName("Holding Market Integration");
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings_.path());
        QVERIFY(KeefiFonts::registerBundledFonts());
    }
    void nativeWalletUsesPublicMarketAndSessionChoiceNeverPersists(){
        QTemporaryDir temporary;QSettings profile(temporary.filePath("synthetic.ini"),QSettings::IniFormat);
        profile.setValue("synthetic/public-sentinel",QStringLiteral("keep-existing-public-settings"));
        Vault vault(temporary.filePath("synthetic.bin"));QVERIFY(vault.create("Synthetic Holding! 2026"));
        AccountRecord account;account.provider="phantom";account.environment="mainnet";account.market="solana";account.currency="SOL";account.alias="NEVER_EXPOSE_ALIAS";
        QVERIFY(vault.addAccount(account,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));
        WalletHttp wallet;CatalogHttp marketHttp,metadata;CandleHttp candleHttp;
        PortfolioController::Factories factories;factories.wallet=[&](const AccountRecord&)->SwapTransport*{return &wallet;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        AccountModel accounts(&vault);PublicMarketCatalog market(&marketHttp);
        MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
        LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);
        window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&market);window.setMarketCandles(&candles);
        window.resize(1440,1000);window.show();auto* workspace=window.findChild<ChartWorkspace*>();QVERIFY(workspace);
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");QVERIFY(pages);pages->setCurrentWidget(workspace);
        QVERIFY(test::openInstrumentPanel(*workspace));
        auto* scope=workspace->findChild<QPushButton*>("chartBrowserScope_holdings");QVERIFY(scope);scope->click();
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",5000);
        QTRY_VERIFY(market.marketListAvailable());auto* tree=workspace->findChild<QTreeWidget*>("chartHoldingAssets");QVERIFY(tree);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),1,5000);
        auto* row=tree->topLevelItem(0);QCOMPARE(row->data(0,Qt::UserRole).toString(),QString("solana:native"));
        QVERIFY(row->text(0).contains("Phantom"));QVERIFY(row->text(0).contains("KRW-SOL"));QVERIFY(!row->text(0).contains("NEVER_EXPOSE_ALIAS"));
        QCOMPARE(workspace->watchlistModel()->holdings(),QList<WatchlistIdentity>({{"coin","upbit","KRW-SOL"}}));
        const auto publicState=workspace->watchlistModel()->publicState();
        tree->scrollToItem(row);QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(row).center());
        QTRY_COMPARE(workspace->selectedChartIdentity(),QString("coin:upbit:KRW-SOL"));
        auto* selector=workspace->findChild<QComboBox*>("chartHoldingMarketSelector");QVERIFY(selector);
        QTRY_VERIFY(selector->findData("coin:binance:SOLUSDT")>=0);const auto target=selector->findData("coin:binance:SOLUSDT");
        auto* reason=workspace->findChild<QLabel*>("chartHoldingReason");QVERIFY(reason);
        QCOMPARE(selector->toolTip(),SessionMarketGuidance);QCOMPARE(selector->accessibleDescription(),SessionMarketGuidance);
        QVERIFY(reason->text().contains(PhantomScope));QVERIFY(!reason->text().contains(SessionMarketGuidance));
        QSettings defaults;QVERIFY(defaults.fileName().startsWith(settings_.path()+"/"));QVERIFY(defaults.isWritable());
        defaults.setValue("synthetic/public-default-sentinel",QStringLiteral("keep-existing-default-settings"));
        const auto profileBefore=settingsValues(profile);const auto defaultsBefore=settingsValues(defaults);
        QCOMPARE(profile.status(),QSettings::NoError);QCOMPARE(defaults.status(),QSettings::NoError);
        selector->setCurrentIndex(target);QVERIFY(QMetaObject::invokeMethod(selector,"activated",Qt::DirectConnection,Q_ARG(int,target)));
        QTRY_COMPARE(workspace->selectedChartIdentity(),QString("coin:binance:SOLUSDT"));
        QTRY_COMPARE(workspace->watchlistModel()->holdings(),QList<WatchlistIdentity>({{"coin","binance","SOLUSDT"}}));
        QCOMPARE(workspace->watchlistModel()->publicState(),publicState);
        QCOMPARE(reason->text().count(SessionMarketGuidance),1);QVERIFY(reason->text().contains(PhantomScope));
        QCOMPARE(settingsValues(profile),profileBefore);QCOMPARE(settingsValues(defaults),defaultsBefore);
        QCOMPARE(profile.status(),QSettings::NoError);QCOMPARE(defaults.status(),QSettings::NoError);
        window.lockAndClear();QCOMPARE(tree->topLevelItemCount(),0);QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
        QVERIFY(reason->text().isEmpty());QCOMPARE(selector->count(),0);
        QCOMPARE(workspace->watchlistModel()->publicState(),publicState);
        QCOMPARE(settingsValues(profile),profileBefore);QCOMPARE(settingsValues(defaults),defaultsBefore);
        QCOMPARE(profile.status(),QSettings::NoError);QCOMPARE(defaults.status(),QSettings::NoError);
        QVERIFY(vault.unlock("Synthetic Holding! 2026"));
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",5000);
        QTRY_COMPARE(workspace->watchlistModel()->holdings(),QList<WatchlistIdentity>({{"coin","upbit","KRW-SOL"}}));
        window.lockAndClear();
    }
    void portfolioDetailShowsPhantomScopeOnlyForSelectedNativeCustody_data(){
        QTest::addColumn<bool>("phantom");QTest::addColumn<bool>("exchange");
        QTest::newRow("phantom-native")<<true<<false;
        QTest::newRow("exchange-only-sol")<<false<<true;
        QTest::newRow("mixed-native-selected-custody")<<true<<true;
    }
    void portfolioDetailShowsPhantomScopeOnlyForSelectedNativeCustody(){
        QFETCH(bool,phantom);QFETCH(bool,exchange);QTemporaryDir temporary;QVERIFY(temporary.isValid());
        Vault vault(temporary.filePath("synthetic-detail.bin"));QVERIFY(vault.create("Synthetic Detail! 2026"));
        if(phantom){
            AccountRecord account;account.provider="phantom";account.environment="mainnet";account.market="solana";account.currency="SOL";account.alias="SYNTHETIC_WALLET";
            QVERIFY(vault.addAccount(account,{{"address","11111111111111111111111111111111"}},StorageMode::EncryptedVault));
        }
        if(exchange){
            AccountRecord account;account.provider="upbit";account.environment="live";account.market="spot";account.currency="KRW";account.alias="SYNTHETIC_EXCHANGE";
            QVERIFY(vault.addAccount(account,{{"access_key","SyntheticDetailAccess123"},{"secret","SyntheticDetailSecret123"}},StorageMode::EncryptedVault));
        }
        WalletHttp wallet;ExchangeSolHttp exchangeHttp;CatalogHttp marketHttp;
        PortfolioController::Factories factories;factories.wallet=[&](const AccountRecord&)->SwapTransport*{return &wallet;};
        factories.trading=[&](const AccountRecord&)->TradingTransport*{return &exchangeHttp;};factories.prices=[]{return new CatalogHttp;};
        PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);PublicMarketCatalog market(&marketHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&market);
        window.resize(1440,1000);window.show();const auto cleanup=qScopeGuard([&]{window.lockAndClear();});
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* home=window.findChild<QWidget*>("portfolioHome");
        auto* tree=window.findChild<QTreeView*>("portfolioAssets");QVERIFY(pages);QVERIFY(home);QVERIFY(tree);pages->setCurrentWidget(home);
        auto* detail=home->findChild<AssetDetailPanel*>();QVERIFY(detail);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{const auto states=portfolio.connections();return states.size()==int(phantom)+int(exchange)&&
            std::all_of(states.cbegin(),states.cend(),[](const auto& state){return state.state=="complete";});}()),5000);
        const auto group=[&]() -> PortfolioTestRow {
            for(int i=0;i<tree->model()->rowCount();++i)if(portfolioRow(tree, i).data(0,Qt::UserRole+1).toString()=="solana:native")return portfolioRow(tree, i);return {};
        };
        QTRY_VERIFY_WITH_TIMEOUT(group()&&group().childCount()==int(phantom)+int(exchange),5000);
        QTRY_VERIFY_WITH_TIMEOUT(tree->isVisibleTo(&window)&&group()&&!group().isHidden(),5000);
        // The UI opens detail on an explicit row click/activation. Wait for the
        // visible projection, then deliver that event instead of only setting focus.
        tree->scrollTo(group().modelIndex());QVERIFY(tree->visualRect(group().modelIndex()).isValid());
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(group().modelIndex()).center());
        QTRY_VERIFY(detail->hasSelection());QTRY_VERIFY(detail->isVisibleTo(&window));
        QCOMPARE(visibleLabels(detail).contains(PhantomScope),phantom);
        if(phantom)QCOMPARE(visibleLabels(detail).count(PhantomScope),1);
        if(phantom&&exchange){
            PortfolioTestRow exchangeChild;
            for(int i=0;i<group().childCount();++i)if(group().child(i).text(1).contains("SYNTHETIC_EXCHANGE"))exchangeChild=group().child(i);
            QVERIFY(exchangeChild);group().setExpanded(true);tree->scrollTo(exchangeChild.modelIndex());QVERIFY(tree->visualRect(exchangeChild.modelIndex()).isValid());
            QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(exchangeChild.modelIndex()).center());QCoreApplication::processEvents();
            QVERIFY(!visibleLabels(detail).contains(PhantomScope));
            tree->scrollTo(group().modelIndex());QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(group().modelIndex()).center());
            QCoreApplication::processEvents();QVERIFY(visibleLabels(detail).contains(PhantomScope));
        }
        window.lockAndClear();QVERIFY(!detail->hasSelection());
        for(const auto* label:detail->findChildren<QLabel*>())QVERIFY(!label->text().contains(PhantomScope));
    }
private:QTemporaryDir settings_;
};
QTEST_MAIN(HoldingMarketIntegrationTest)
#include "tst_holding_market_integration.moc"
