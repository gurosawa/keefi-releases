#include "keefi/TossMarketView.h"
#include "keefi/TossMarketStream.h"
#include "keefi/PortfolioController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/TossResponses.h"
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QTest>
using namespace keefi;
using namespace keefi::test;
namespace {
class Http final:public TradingTransport {
  public:
    QList<RequestSpec> requests;quint64 epoch=0;bool respond=true,withUsHolding=false,withKrCollision=false;
    void abort() override{++epoch;}
    void send(quint64 token,const RequestSpec& request) override{
        requests.append(request);if(!respond)return;
        OrderInput input;input.cursor=QUrlQuery(request.url).queryItemValue("market");input.symbol=QUrlQuery(request.url).queryItemValue("symbols");
        auto result=tossResponse(request.operation,input);const auto generation=epoch;
        if(withUsHolding&&request.operation==Operation::Holdings){
            auto envelope=QJsonDocument::fromJson(result.body).object();auto value=envelope["result"].toObject();auto items=value["items"].toArray();
            auto us=items.first().toObject();us["symbol"]="AAPL";us["name"]="Synthetic US holding";us["marketCountry"]="US";us["currency"]="USD";us["quantity"]="2";
            items.append(us);value["items"]=items;envelope["result"]=value;result.body=QJsonDocument(envelope).toJson();
        }
        if(withKrCollision&&request.operation==Operation::Markets&&input.cursor=="KOSPI"){
            auto envelope=QJsonDocument::fromJson(result.body).object();auto items=envelope["result"].toArray();
            items.append(QJsonObject{{"symbol","ABCDEF"},{"name","합성 국내 충돌 종목"},{"securityType","STOCK"},
                {"isCommonShare",true},{"isinCode","KR0000000000"}});
            envelope["result"]=items;result.body=QJsonDocument(envelope).toJson();
        }
        QTimer::singleShot(0,this,[this,generation,token,result]{if(generation==epoch)emit finished(token,result);});
    }
    int authentications()const{int n=0;for(const auto& r:requests)if(r.operation==Operation::Authenticate)++n;return n;}
};
class Socket final:public TossMarketSocket {
  public:
    int opens=0,aborts=0;QList<QByteArray> sent;
    void open(QByteArrayView) override{++opens;}
    void send(const QByteArray& bytes) override{sent.append(bytes);}
    void abort() override{++aborts;}
};
bool addToss(Vault& vault,const QString& suffix={}){
    AccountRecord a;a.provider="toss";a.alias="FAKE private Toss "+suffix;a.market="domestic_equity";a.environment="live";a.currency="KRW";
    return vault.addAccount(a,{{"client_id","FAKE_CLIENT_ID"+suffix},{"client_secret","FAKE_CLIENT_SECRET"+suffix}},StorageMode::EncryptedVault);
}
PortfolioController::Factories factories(Http& http){
    PortfolioController::Factories f;f.trading=[&http](const AccountRecord&){return &http;};f.prices=[] {return static_cast<TradingTransport*>(nullptr);};return f;
}
}
class TossMarketViewTest final:public QObject {
    Q_OBJECT
  private slots:
    void chartViewportSharesTheSessionWhileTheFlowViewIsHidden(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-chart-session"));QVERIFY(vault.create("FAKE chart session password 2026!"));QVERIFY(addToss(vault));
        Http http,market;http.withUsHolding=true;market.respond=false;PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto* broker=portfolio.broker(vault.accounts().first().id);QVERIFY(broker);broker->loadMarkets();QTRY_VERIFY_WITH_TIMEOUT(!broker->busy(),5000);
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));TossMarketView view(&portfolio,&stream);
        const auto auth=http.authentications();QVERIFY(!view.isVisible());
        view.setChartContext(true,{"stock:KR:035720"},"stock:KR:005930",{"stock:US:AAPL","stock:KR:035720"});
        QTRY_COMPARE_WITH_TIMEOUT(stream.targets().size(),3,2500);
        QMap<QString,int> priority;for(const auto& target:stream.targets())priority[target.symbol]=target.priority;
        QCOMPARE(priority.value("035720"),2);QCOMPARE(priority.value("005930"),0);QCOMPARE(priority.value("AAPL"),1);
        QCOMPARE(stream.targets().first().symbol,QString("005930"));
        QCOMPARE(http.authentications(),auth);QCOMPARE(fake->opens,1);
        // Removing a watch entry while it stays visible in the public catalog
        // revokes that demand; the selected chart and holdings remain.
        view.setChartContext(true,{"stock:KR:035720"},"stock:KR:005930",{"stock:US:AAPL"});
        QCOMPARE(stream.targets().size(),2);
        for(const auto& target:stream.targets())QVERIFY(target.symbol!="035720");
        QCOMPARE(http.authentications(),auth);
        view.setChartContext(false,{},QString{},{});QVERIFY(stream.targets().isEmpty());QVERIFY(stream.quotes().isEmpty());QCOMPARE(http.authentications(),auth);
    }
    void chartIdentityKeepsCountryWhenSymbolsCollide(){
        QTemporaryDir dir;Vault vault(dir.filePath("synthetic-chart-country-collision"));
        QVERIFY(vault.create("FAKE chart identity password 2026!"));QVERIFY(addToss(vault));
        Http http,market;http.withKrCollision=true;market.respond=false;
        PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto* broker=portfolio.broker(vault.accounts().first().id);QVERIFY(broker);
        broker->loadMarkets();QTRY_VERIFY_WITH_TIMEOUT(!broker->busy(),5000);
        QCOMPARE(broker->marketDataCountry("ABCDEF"),QString("KR"));
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();
        TossMarketStream stream(nullptr,&market,std::move(socket));TossMarketView view(&portfolio,&stream);
        const auto auth=http.authentications();
        view.setWatchlist({"ABCDEF"});
        view.setChartContext(true,{"stock:US:ABCDEF"},"stock:US:ABCDEF",{"stock:US:ABCDEF"});
        for(const auto& target:stream.targets())QVERIFY2(target.symbol!="ABCDEF","US chart identity must never be converted into the broker's KR symbol");
        const auto excluded=view.findChild<QLabel*>("tossMarketExcluded")->text();
        QVERIFY(excluded.contains("ABCDEF"));QVERIFY(excluded.contains(QStringLiteral("미국 주식")));QVERIFY(!excluded.contains("stock:"));
        QCOMPARE(fake->opens,1);
        emit fake->connected();QVERIFY(!fake->sent.isEmpty());
        const auto declarationId=QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
        QVERIFY(!declarationId.toString().isEmpty());
        QJsonArray subscribed;
        for(const auto& target:stream.targets())subscribed.append("trade:"+target.marketCountry.toLower()+':'+target.symbol);
        emit fake->received(QJsonDocument(QJsonObject{{"type","subscriptions"},{"id",declarationId},
            {"subscribed",subscribed},{"rejected",QJsonArray{}}}).toJson());
        view.setChartContext(true,{"stock:KR:ABCDEF"},"stock:US:ABCDEF",{"stock:KR:ABCDEF"});
        bool domestic=false;
        for(const auto& target:stream.targets())if(target.symbol=="ABCDEF"){
            domestic=true;QCOMPARE(target.marketCountry,QString("KR"));QCOMPARE(target.priority,2);
        }
        QVERIFY2(domestic,"The exact verified KR identity remains subscribable");
        QCOMPARE(http.authentications(),auth);QCOMPARE(fake->opens,1);
        view.setChartContext(false,{},QString{},{});
        QVERIFY(stream.targets().isEmpty());QVERIFY(stream.quotes().isEmpty());
        QCOMPARE(http.authentications(),auth);
    }
    void verifiedUsHoldingsSubscribeWithoutChangingG7ValuationCapture(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("FAKE view password 2026!"));QVERIFY(addToss(vault));
        Http http,market;http.withUsHolding=true;market.respond=false;PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto* broker=portfolio.broker(vault.accounts().first().id);QVERIFY(broker);QCOMPARE(broker->marketDataCountry("AAPL"),QString("US"));
        const auto original=broker->assetCapture();for(const auto& row:original.snapshot.components)QVERIFY(row.asset!="AAPL");
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));
        TossMarketView view(&portfolio,&stream);view.show();QTRY_COMPARE_WITH_TIMEOUT(fake->opens,1,2000);
        QCOMPARE(stream.targets().size(),2);bool us=false;
        for(const auto& target:stream.targets())if(target.symbol=="AAPL"){us=true;QCOMPARE(target.marketCountry,QString("US"));QCOMPARE(target.priority,1);}
        QVERIFY(us);QCOMPARE(http.authentications(),1);
        const auto after=broker->assetCapture();QCOMPARE(after.snapshot,original.snapshot);
    }
    void manualReadOwnershipKeepsDisplaySessionWithoutNewAuthentication(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("FAKE view password 2026!"));QVERIFY(addToss(vault));
        Http http,market;market.respond=false;PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));
        TossMarketView view(&portfolio,&stream);view.setSelection("005930");view.show();QTRY_COMPARE_WITH_TIMEOUT(fake->opens,1,2000);
        QVERIFY(!portfolio.connections().first().active); // This flag means an in-progress read, not enabled state.
        portfolio.suspend(vault.accounts().first().id,true);
        // suspend transfers private balance polling to the manual trading
        // window. The public display session neither reads nor owns balances.
        QCoreApplication::processEvents();
        QCOMPARE(stream.targets().size(),1);QCOMPARE(stream.targets().first().symbol,QString("005930"));
        QCOMPARE(fake->opens,1);QVERIFY(!view.findChild<QLabel*>("tossMarketAccount")->text().isEmpty());
        QCOMPARE(http.authentications(),1);
    }
    void sharesReadySessionAndUsesVerifiedPriorityWithoutAuthenticating(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("FAKE view password 2026!"));QVERIFY(addToss(vault));
        Http http,market;market.respond=false;PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto* broker=portfolio.broker(vault.accounts().first().id);QVERIFY(broker);broker->loadMarkets();QTRY_VERIFY_WITH_TIMEOUT(!broker->busy(),5000);
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));
        TossMarketView view(&portfolio,&stream);view.setSelection("035720");view.setWatchlist({"005930","UNKNOWN"});view.setMapSymbols({"035720"});
        const int auth=http.authentications();QCOMPARE(fake->opens,0);view.show();QTRY_COMPARE_WITH_TIMEOUT(fake->opens,1,2000);
        QCOMPARE(http.authentications(),auth);QCOMPARE(stream.targets().size(),2);
        for(const auto& target:stream.targets()){QCOMPARE(target.marketCountry,QString("KR"));QCOMPARE(target.priority,target.symbol=="005930"?1:0);}
        auto* combo=view.findChild<QComboBox*>("tossMarketConnections");QVERIFY(combo);QVERIFY(combo->isHidden());
        auto* excluded=view.findChild<QLabel*>("tossMarketExcluded");QVERIFY(excluded);QVERIFY(excluded->text().contains("UNKNOWN"));
        QVERIFY(view.findChild<QLabel*>("tossMarketLimitations")->text().contains("일부"));
        emit fake->connected();QVERIFY(!fake->sent.isEmpty());
        const auto id=QJsonDocument::fromJson(fake->sent.last()).array().first().toObject().value("id");
        emit fake->received(QJsonDocument(QJsonObject{{"type","subscriptions"},{"id",id},
            {"subscribed",QJsonArray{"trade:kr:005930","trade:kr:035720"}},{"rejected",QJsonArray{}}}).toJson());
        emit fake->received(QJsonDocument(QJsonObject{{"type","message"},{"topic","trade:kr:005930"},
            {"data",QJsonObject{{"price","70100"},{"volume","1"},{"currency","KRW"},
            {"timestamp",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}}}).toJson());
        const auto displayed=view.findChild<QLabel*>("tossMarketQuotes")->text();
        QVERIFY(displayed.contains("005930"));QVERIFY(displayed.contains("70,100 KRW"));QVERIFY(displayed.contains("KST"));
        const auto original=stream.quotes().first();QVERIFY(original.evidence.url.contains("tossinvest.com"));
        QCOMPARE(original.price,QString("70100"));
        const auto requestsBefore=http.requests.size(),marketBefore=market.requests.size(),controlsBefore=fake->sent.size();
        const auto evidence=view.findChild<QLabel*>("tossMarketEvidence")->text();
        QVERIFY2(!evidence.contains("://"),qPrintable(evidence));QVERIFY(evidence.contains(QStringLiteral("토스증권")));QVERIFY(!evidence.contains('\n'));
        for(auto* label:view.findChildren<QLabel*>())for(const auto& text:{label->text(),label->toolTip(),label->accessibleName(),label->accessibleDescription()}){
            QVERIFY2(!text.contains("://"),qPrintable(text));QVERIFY(!text.contains("stock:"));
        }
        emit stream.changed();QCOMPARE(stream.quotes().first().price,original.price);QCOMPARE(stream.quotes().first().evidence,original.evidence);QCOMPARE(stream.quotes().first().symbol,original.symbol);
        QCOMPARE(stream.quotes().first().marketCountry,original.marketCountry);QCOMPARE(http.requests.size(),requestsBefore);QCOMPARE(market.requests.size(),marketBefore);QCOMPARE(fake->sent.size(),controlsBefore);
        const auto before=fake->aborts;view.hide();QVERIFY(fake->aborts>before);QVERIFY(stream.targets().isEmpty());QVERIFY(stream.quotes().isEmpty());
        QVERIFY(view.findChild<QLabel*>("tossMarketQuotes")->text().isEmpty());
        QCOMPARE(http.authentications(),auth);
    }
    void multipleIndependentConnectionsAreSelectableWithoutNewAuthentication(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("FAKE view password 2026!"));QVERIFY(addToss(vault,"A"));QVERIFY(addToss(vault,"B"));
        Http first,second,market;market.respond=false;
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        f.trading=[&](const AccountRecord& a)->TradingTransport*{return a.alias.endsWith("A")?&first:&second;};
        PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().size()==2&&portfolio.connections()[0].state=="complete"&&portfolio.connections()[1].state=="complete",5000);
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));
        TossMarketView view(&portfolio,&stream);view.show();QTRY_COMPARE_WITH_TIMEOUT(fake->opens,1,2000);
        auto* combo=view.findChild<QComboBox*>("tossMarketConnections");QVERIFY(combo);QCOMPARE(combo->count(),2);QVERIFY(combo->isVisible());
        QVERIFY(combo->minimumHeight()>=44);QVERIFY(!combo->accessibleName().isEmpty());
        const auto previous=view.findChild<QLabel*>("tossMarketAccount")->text();combo->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(fake->opens,2,2000);QVERIFY(view.findChild<QLabel*>("tossMarketAccount")->text()!=previous);
        QCOMPARE(first.authentications(),1);QCOMPARE(second.authentications(),1);
    }
    void connectionChangeCannotRestoreOldAccountBeforeQueuedRebuild_data(){
        QTest::addColumn<bool>("replaceKey");
        QTest::newRow("remove") << false;
        QTest::newRow("replace-key") << true;
    }
    void connectionChangeCannotRestoreOldAccountBeforeQueuedRebuild(){
        QFETCH(bool,replaceKey);
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("FAKE view password 2026!"));QVERIFY(addToss(vault));
        Http http,market;market.respond=false;PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));
        TossMarketView view(&portfolio,&stream);view.show();QTRY_COMPARE_WITH_TIMEOUT(fake->opens,1,2000);
        const auto id=vault.accounts().first().id;const auto opens=fake->opens;
        auto* account=view.findChild<QLabel*>("tossMarketAccount");QVERIFY(account);QVERIFY(!account->text().isEmpty());
        if(replaceKey)QVERIFY(vault.replaceCredentials(id,{{"client_id","FAKE_ROTATED_CLIENT_ID"},{"client_secret","FAKE_ROTATED_CLIENT_SECRET"}}));
        else QVERIFY(vault.removeAccount(id));
        // accountsChanged clears private display synchronously. The same signal
        // turn must not reselect an old entry or reopen its borrowed session
        // while PortfolioController's entry rebuild is still queued.
        QCOMPARE(fake->opens,opens);
        QVERIFY(account->text().isEmpty());
        QCOMPARE(view.findChild<QComboBox*>("tossMarketConnections")->count(),0);
        QVERIFY(stream.targets().isEmpty());QVERIFY(stream.quotes().isEmpty());
        QVERIFY(view.findChild<QLabel*>("tossMarketQuotes")->text().isEmpty());
    }
    void resetErasesPrivateAccountAndDestroyedViewStopsSubscriptions(){
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("FAKE view password 2026!"));QVERIFY(addToss(vault));
        Http http,market;market.respond=false;PortfolioController portfolio(vault,nullptr,factories(http));portfolio.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),5000);
        auto socket=std::make_unique<Socket>();auto* fake=socket.get();TossMarketStream stream(nullptr,&market,std::move(socket));
        auto view=std::make_unique<TossMarketView>(&portfolio,&stream);view->show();QTRY_COMPARE_WITH_TIMEOUT(fake->opens,1,2000);
        auto* account=view->findChild<QLabel*>("tossMarketAccount");QVERIFY(account);QVERIFY(account->text().contains("FAKE private Toss"));
        vault.lock();QVERIFY(account->text().isEmpty());QVERIFY(view->findChild<QComboBox*>("tossMarketConnections")->count()==0);
        QVERIFY(stream.targets().isEmpty());QVERIFY(stream.quotes().isEmpty());
        QVERIFY(view->findChild<QLabel*>("tossMarketQuotes")->text().isEmpty());
        const auto before=fake->aborts;view.reset();QVERIFY(fake->aborts>before);
    }
};
QTEST_MAIN(TossMarketViewTest)
#include "tst_toss_market_view.moc"
