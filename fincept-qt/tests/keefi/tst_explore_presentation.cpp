#include "keefi/PortfolioDayView.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/InfoButton.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/MarketMap.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/ChartInstrumentCatalog.h"
#include <QDir>
#include <QComboBox>
#include <QTableWidget>
#include <QLineEdit>
#include "keefi/MarketCandles.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/TradingTransport.h"
#include "keefi/PublicQuoteStream.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QTabWidget>
#include <QScrollArea>
#include "keefi/AccountModel.h"
#include "keefi/Vault.h"
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSettings>
#include "keefi/Spotlight.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTest>
using namespace keefi;
class ExplorePresentationTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
private slots:
    void initTestCase(){QVERIFY(settingsRoot_.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic C9");QCoreApplication::setApplicationName("Synthetic C9");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsRoot_.path());}
    void dayBasisIsKeyboardInformationAndClearsOpenPrivateDialog();
    void dayContributionsUseObservedAmountsAndLivePalette();
    void refreshingUnavailableCoinPerformanceKeepsItsNoteOutOfTheLayout() {
        PortfolioDayView view;
        CoinPerformanceView pending;
        pending.inScope = true;
        pending.today.reason = QStringLiteral("잔고 확인 중");
        view.setCoinPerformance(pending);
        view.show();
        auto* note = view.findChild<QLabel*>("coinPerformanceNote");
        QVERIFY(note && note->isHidden());
        struct NoteEvents final : QObject {
            int shows = 0;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Show) ++shows;
                return false;
            }
        } events;
        note->installEventFilter(&events);
        for (int i = 0; i < 4; ++i) view.setCoinPerformance(pending);
        QCOMPARE(events.shows, 0);
        pending.today.available = true;
        pending.today.pnl = "10";
        pending.today.fromMs = QDateTime::currentMSecsSinceEpoch();
        view.setCoinPerformance(pending);
        QVERIFY(note->isVisible());
    }
    void publicDetailUsesSameSourceHistoryAndExactChartIdentity();
    void publicDetailPrivateLinkageLeavesNoTextAfterClear();
    void windowPolicyRestoresInjectedProfileAndWritesOnlyChangedPublicState();
    void publicSourceSelectionOpensTheActualChartTabWithoutPrivateReads();
    void retainedPublicQuoteShowsPreviousSourceStatus();
    void mapDetailsUseTheSharedChartRouteAndConnectionGuidance();
    void boardCoinsOutsidePinnedIdsOpenChartsAndUnverifiedHoldingsSaySo();
    void retainedOrAmbiguousBoardCoinsNeverRouteBySymbol_data();
    void retainedOrAmbiguousBoardCoinsNeverRouteBySymbol();
};
namespace {
PortfolioDayHistory history(){PortfolioDayHistory h;h.comparison={true,true,1792598400000,1792602000000,0,"KRW","1000","1200","200",QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"),{}};h.contributions.available=true;h.contributions.groups={{PortfolioContributionClass::Coin,"700","950","250",1},{PortfolioContributionClass::Unclassified,"300","250","-50",1}};h.points={{1792598400000,true,true,false,"1000",{}},{1792602000000,true,true,true,"1200",{}}};return h;}
PublicMarketRow row(){PublicMarketRow r;r.market="KRW-SYN";r.base="SYN";r.koreanName="합성 코인";r.priced=true;r.price="120";r.previousClose="100";r.signedChangeRate="0.2";r.quoteUrl="https://api.upbit.com/v1/ticker?markets=KRW-SYN";r.receivedAtMs=1792602000000;r.sourceTimeMs=0;return r;}
int pixels(const QImage& image,const QColor& wanted){int n=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const auto c=image.pixelColor(x,y);if(qAbs(c.red()-wanted.red())<8&&qAbs(c.green()-wanted.green())<8&&qAbs(c.blue()-wanted.blue())<8)++n;}return n;}
}
void ExplorePresentationTest::dayBasisIsKeyboardInformationAndClearsOpenPrivateDialog(){PortfolioDayView v;v.resize(560,260);v.setHistory(history());v.show();QTest::qWait(20);auto* info=v.findChild<InfoButton*>("portfolioDayInfo");QVERIFY(info);QVERIFY(!info->accessibleName().isEmpty());QVERIFY(v.findChild<QLabel*>("portfolioDayBaseline")->isHidden());info->setFocus();QTest::keyClick(info,Qt::Key_Return);QVERIFY(info->informationOpen());auto* d=v.findChild<QDialog*>("infoDialog");QVERIFY(d);QVERIFY(d->findChild<QPlainTextEdit*>("infoPrivateText")->toPlainText().contains("01:00:00"));v.clear("금고 잠금");QVERIFY(!info->informationOpen());QVERIFY(!info->setPrivateInformation("FAKE LATE PRIVATE"));for(auto* label:v.findChildren<QLabel*>()){QVERIFY(!label->text().contains("200"));QVERIFY(!label->text().contains("01:00"));}}
void ExplorePresentationTest::dayContributionsUseObservedAmountsAndLivePalette(){QWidget owner;MovementColorPolicy p(&owner);PortfolioDayView v(&owner);v.setDetailsExpanded(true);v.resize(600,280);v.setHistory(history());v.show();QTest::qWait(20);auto* bars=v.findChild<QWidget*>("portfolioContributionBars");QVERIFY(bars);QVERIFY(!bars->isHidden());const auto before=bars->grab().toImage();QVERIFY(pixels(before,themeColors().upText)>10);QVERIFY(pixels(before,themeColors().downText)>10);p.setScheme(MovementScheme::KoreanRedBlue);QCoreApplication::processEvents();const auto after=bars->grab().toImage();QVERIFY(pixels(after,themeColors().koreanDownText)>10);QVERIFY(pixels(after,themeColors().upText)==0);QVERIFY(v.findChild<QLabel*>("portfolioDayContributions")->text().contains("250"));v.clear();QVERIFY(bars->isHidden());QVERIFY(bars->accessibleDescription().isEmpty());}
void ExplorePresentationTest::publicDetailUsesSameSourceHistoryAndExactChartIdentity(){AssetDetailPanel v;v.enableExplorationControls();v.resize(500,900);v.showMarket(row(),false,nullptr,false);v.setPublicSelection("coin","SYN",false);v.show();QTest::qWait(20);auto* spark=v.findChild<QWidget*>("assetDetailSparkline");QVERIFY(spark);QVERIFY(spark->isHidden());ExploreMetricHistory h{true,"coin:upbit:KRW-SYN","KRW",{},{{1792600000000,100,false},{1792601000000,110,true},{1792602000000,120,true}}};v.setPublicHistory(h);QVERIFY(!spark->isHidden());h.unit="USD";v.setPublicHistory(h);QVERIFY(spark->isHidden());h.unit="KRW";h.sourceKey="coin:binance:SYNUSDT";v.setPublicHistory(h);QVERIFY(spark->isHidden());v.setChartIdentity("coin:upbit:KRW-SYN");auto* link=v.findChild<QPushButton*>("assetDetailChart");QVERIFY(link&&link->isVisible());QSignalSpy request(&v,&AssetDetailPanel::publicChartRequested);QTest::mouseClick(link,Qt::LeftButton);QCOMPARE(request.size(),1);QCOMPARE(request.first().first().toString(),QString("coin:upbit:KRW-SYN"));v.setChartIdentity("SYN");QVERIFY(link->isHidden());v.clearDetail();QVERIFY(spark->isHidden());QVERIFY(link->isHidden());}
void ExplorePresentationTest::publicDetailPrivateLinkageLeavesNoTextAfterClear(){AssetDetailPanel v;v.enableExplorationControls();PortfolioGroupView g;g.identity="exchange:SYN";g.asset="SYN";PortfolioHoldingView h;h.accountAlias="FAKE C9 PRIVATE LINK";h.quantity="123.456";h.asset="SYN";h.provider="upbit";h.market="spot";h.state=PortfolioValueState::Fresh;h.value="987654";g.holdings.append(h);v.showMarket(row(),true,&g,false);v.resize(500,900);v.show();QTest::qWait(20);auto* info=v.findChild<InfoButton*>("assetDetailInfo");QVERIFY(info);auto* places=v.findChild<QWidget*>("assetDetailPlaces");QVERIFY(places->isHidden());QTest::mouseClick(info,Qt::LeftButton);QVERIFY(info->informationOpen());QVERIFY(v.findChild<QPlainTextEdit*>("infoPrivateText")->toPlainText().contains("FAKE C9 PRIVATE LINK"));QVERIFY(!v.findChild<QPlainTextEdit*>("infoPublicText")->toPlainText().contains("반영 시각이"));auto changed=row();changed.price="121";v.showMarket(changed,true,&g,false);QVERIFY(info->informationOpen());v.showMarket(row(),false,nullptr,false);QVERIFY(!info->informationOpen());for(auto* label:v.findChildren<QLabel*>()){QVERIFY(!label->text().contains("FAKE C9 PRIVATE LINK"));QVERIFY(!label->text().contains("987654"));}QVERIFY(!info->setPrivateInformation("FAKE LATE PRIVATE"));QCOMPARE(v.findChild<QLabel*>("assetDetailHeadline")->text(),QString("120 KRW"));}
void ExplorePresentationTest::windowPolicyRestoresInjectedProfileAndWritesOnlyChangedPublicState(){QTemporaryDir dir;QSettings settings(dir.filePath("profile.ini"),QSettings::IniFormat);settings.setValue("unrelated","keep");settings.setValue("display/movementColors/v1",QByteArray("{\"schema\":\"keefi-movement-colors/1\",\"scheme\":\"korean-red-blue\"}"));Vault vault(dir.filePath("vault"));AccountModel accounts(&vault);PortfolioController::Factories factories;factories.prices=[](){return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);LocalWindow v(vault,accounts);v.setPortfolioController(&portfolio);v.setChartSettings(&settings);auto* p=v.findChild<MovementColorPolicy*>(MovementColorPolicy::ObjectName,Qt::FindDirectChildrenOnly);QVERIFY(p);QCOMPARE(p->scheme(),MovementScheme::KoreanRedBlue);QVERIFY(v.styleSheet().contains("QLabel[tone=\"down\"] { color: "+themeColors().koreanDownText.name()+"; }"));v.findChild<QPushButton*>("openOptions")->click();auto* options=v.findChild<QDialog*>("optionsDialog");QVERIFY(options);auto* displayTab=options->findChild<QPushButton*>("optionsDisplayTab");QVERIFY(displayTab);displayTab->click();auto* combo=options->findChild<QComboBox*>("optionMovementColors");QVERIFY(combo);QCOMPARE(combo->currentData().toInt(),int(MovementScheme::KoreanRedBlue));combo->setCurrentIndex(combo->findData(int(MovementScheme::GreenRed)));QCOMPARE(settings.value("display/movementColors/v1").toByteArray(),p->publicState());QCOMPARE(settings.value("unrelated").toString(),QString("keep"));QCOMPARE(settings.allKeys().size(),2);QSignalSpy changed(p,&MovementColorPolicy::changed);p->setScheme(MovementScheme::GreenRed);QCOMPARE(changed.size(),0);}
void ExplorePresentationTest::publicSourceSelectionOpensTheActualChartTabWithoutPrivateReads(){
    class Transport final:public TradingTransport {public:QList<RequestSpec> requests;bool catalog=false,historyMode=false;void abort()override{}void send(quint64 token,const RequestSpec& r)override{QVERIFY(!r.authenticated);requests.append(r);if(!catalog&&!historyMode)return;HttpResult h;h.status=200;if(historyMode){QVERIFY(PublicMarketHistory::isRequest(r));const auto now=QDateTime::currentDateTimeUtc();h.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-SYN"},{"candle_date_time_utc",now.addSecs(-1200).toString("yyyy-MM-ddTHH:mm:ss")},{"trade_price",100}},QJsonObject{{"market","KRW-SYN"},{"candle_date_time_utc",now.addSecs(-600).toString("yyyy-MM-ddTHH:mm:ss")},{"trade_price",110}}}).toJson();}else if(r.operation==Operation::Markets)h.body=R"([{"market":"KRW-SYN","korean_name":"합성 종목","english_name":"Synthetic","market_event":{"warning":false,"caution":{}}}])";else h.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-SYN"},{"trade_price","100"},{"prev_closing_price","90"},{"signed_change_rate","0.1"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch()-100)},{"acc_trade_price_24h","12345"}}}).toJson();QTimer::singleShot(0,this,[this,token,h]{emit finished(token,h);});}} catalogTransport,chartTransport,historyTransport;
    class Socket final:public PublicCandleSocket {public:void open(quint64,const QUrl&)override{}void send(quint64,const QByteArray&)override{}void abort()override{}};
    historyTransport.historyMode=true;PublicMarketHistory recorded(&historyTransport);catalogTransport.catalog=true;PublicMarketCatalog catalog(&catalogTransport);catalog.refresh();QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy()&&!catalog.rows().isEmpty()&&catalog.rows().first().priced,1500);
    MarketCandles candles(&chartTransport,std::make_unique<Socket>());QTemporaryDir dir;QSettings settings(dir.filePath("profile.ini"),QSettings::IniFormat);Vault vault(dir.filePath("vault"));AccountModel accounts(&vault);PortfolioController::Factories factories;factories.prices=[](){return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);LocalWindow v(vault,accounts);v.setPortfolioController(&portfolio);v.setChartSettings(&settings);v.setChartInstrumentTransport(&chartTransport);v.setPublicMarketCatalog(&catalog,false);v.setPublicMarketHistory(&recorded);v.setMarketCandles(&candles);v.resize(1280,900);v.show();QTest::qWait(30);
    auto* mode=v.findChild<QComboBox*>("exploreView");QVERIFY(mode);mode->setCurrentIndex(mode->findData("list"));auto* search=v.findChild<QLineEdit*>("exploreSearch");QVERIFY(search);search->setText("SYN");auto* table=v.findChild<QTableWidget*>("exploreMarkets");QVERIFY(table);QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(),1,1000);QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,table->visualItemRect(table->item(0,0)).center());auto* detail=v.findChild<AssetDetailPanel*>("exploreDetail");QVERIFY(detail);auto* spark=detail->findChild<QWidget*>("assetDetailSparkline");QVERIFY(spark);QTRY_VERIFY_WITH_TIMEOUT(!spark->isHidden(),2000);auto* card=detail->findChild<ExploreMetricCard*>();QVERIFY(card);QCOMPARE(card->plottedHistory().size(),2);QVERIFY(card->plottedHistory()[1].connectsPrevious);const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");if(!preview.isEmpty())QVERIFY(v.grab().save(QDir(preview).filePath("c9-explore-wide.png")));v.resize(760,820);QTest::qWait(30);if(!preview.isEmpty())QVERIFY(v.grab().save(QDir(preview).filePath("c9-explore-narrow.png")));auto* narrowLink=detail->findChild<QPushButton*>("assetDetailChart");auto* narrowScroll=detail->findChild<QScrollArea*>("assetDetailScroll");QVERIFY(narrowLink&&narrowScroll);QVERIFY2(narrowScroll->viewport()->rect().contains(narrowLink->mapTo(narrowScroll->viewport(),narrowLink->rect().center())),"Chart link must precede the disabled order preview in a narrow detail");v.resize(1280,900);auto* link=detail->findChild<QPushButton*>("assetDetailChart");QVERIFY(link&&link->isVisible());QCoreApplication::processEvents();auto* scroll=detail->findChild<QScrollArea*>("assetDetailScroll");QVERIFY(scroll);QVERIFY2(scroll->viewport()->rect().contains(link->mapTo(scroll->viewport(),link->rect().center())),"Chart link must be reachable in the displayed detail before the disabled order preview");QTest::mouseClick(link,Qt::LeftButton);auto* workspace=v.findChild<ChartWorkspace*>();QVERIFY(workspace);QCOMPARE(v.findChild<QTabWidget*>("portfolioPages")->currentWidget(),static_cast<QWidget*>(workspace));QCOMPARE(workspace->selectedChartIdentity(),QString("coin:upbit:KRW-SYN"));QVERIFY(!vault.isUnlocked());for(const auto& r:chartTransport.requests)QVERIFY(!r.authenticated);
}
void ExplorePresentationTest::retainedPublicQuoteShowsPreviousSourceStatus(){AssetDetailPanel panel;panel.enableExplorationControls();const QStringList evidence{"Nasdaq · 수신 01-01 00:00:00","공개 표시만"};panel.showQuote("Synthetic","SYN · Nasdaq","$100","1",{},evidence,"전일 대비","USD","public:nasdaq:SYN",false);auto* card=panel.findChild<ExploreMetricCard*>();QVERIFY(card);QCOMPARE(card->data().value,QString("100"));QVERIFY(!card->data().freshnessText.contains("이전 시세"));panel.showQuote("Synthetic","SYN · Nasdaq · 이전 시세","$100","1",{},evidence,"전일 대비","USD","public:nasdaq:SYN",true);QCOMPARE(card->data().value,QString("100"));QCOMPARE(card->data().sourceKey,QString("public:nasdaq:SYN"));QVERIFY(card->data().freshnessText.contains("이전 시세"));}
void ExplorePresentationTest::mapDetailsUseTheSharedChartRouteAndConnectionGuidance(){
    class Transport final:public TradingTransport {
      public:
        QString role;QList<RequestSpec> requests;
        void abort()override{}
        void send(quint64 token,const RequestSpec& request)override{
            QVERIFY(!request.authenticated);requests.append(request);if(role=="silent")return;
            HttpResult reply;reply.status=200;
            if(role=="catalog"){
                if(request.operation==Operation::Markets)reply.body=R"([{"market":"KRW-SYN","korean_name":"합성 종목","english_name":"Synthetic","market_event":{"warning":false,"caution":{}}}])";
                else reply.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-SYN"},{"trade_price","100"},{"prev_closing_price","90"},{"signed_change_rate","0.1"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch()-100)}}}).toJson();
            }else if(role=="metadata"){
                QJsonArray rows;for(const auto& asset:{QString("SYN"),QString("ALT"),QStringLiteral("合成")})rows.append(QJsonObject{{"symbol",asset+"USDT"},{"baseAsset",asset},{"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}});
                reply.body=QJsonDocument(QJsonObject{{"symbols",rows}}).toJson();
            }else if(request.url.host()=="api.binance.com"){
                QCOMPARE(request.url,PublicEquityBoard::request("binance").url);QVERIFY(NetworkTradingTransport::allowed(request));
                QJsonArray rows;for(const auto& base:{"SYN","ALT","合成","UNLISTED"})rows.append(QJsonObject{{"symbol",QString(base)+"USDT"},{"lastPrice","2.5"},{"priceChangePercent",QString(base)=="SYN"?"8":"1"},{"quoteVolume","12000"},{"count",5},{"closeTime",QDateTime::currentMSecsSinceEpoch()-100}});
                reply.body=QJsonDocument(rows).toJson();
            }else if(request.url.host()=="api.coingecko.com"){
                QJsonArray rows;int rank=0;for(const auto& asset:{QString("SYN"),QString("ALT"),QString("NOPE")})rows.append(QJsonObject{{"id","synthetic-"+asset.toLower()},{"symbol",asset.toLower()},{"name","Synthetic "+asset},{"market_cap",1000000000-rank++*1000},{"current_price",100},{"price_change_percentage_24h",1.0}});
                reply.body=QJsonDocument(rows).toJson();
            }else if(request.url.host()=="m.stock.naver.com")reply.body=R"({"marketStatus":"CLOSE","stocks":[{"itemCode":"005930","stockName":"합성 국내 주식","closePrice":"100","fluctuationsRatio":"1","compareToPreviousPrice":{"code":"2"},"marketValue":"13000000"}]})";
            else reply.body=R"({"data":{"rows":[{"symbol":"SYN","name":"Synthetic US","lastsale":"$100","pctchange":"1%","marketCap":"1000000000","sector":"Technology"}]}})";
            QTimer::singleShot(0,this,[this,token,reply]{emit finished(token,reply);});
        }
    } catalogHttp,boardHttp,metadataHttp,candleHttp;
    catalogHttp.role="catalog";boardHttp.role="board";metadataHttp.role="metadata";candleHttp.role="silent";
    class Socket final:public PublicCandleSocket {public:void open(quint64,const QUrl&)override{}void send(quint64,const QByteArray&)override{}void abort()override{}};
    class TickerSocket final:public PublicTickerSocket {
      public:
        QSet<QString> subscribed;
        void open(const QUrl&)override{emit connected();}
        void abort()override{subscribed.clear();}void ping()override{}
        void send(const QByteArray& data)override{const auto msg=QJsonDocument::fromJson(data).object();for(const auto& v:msg["params"].toArray()){
            if(msg["method"]=="SUBSCRIBE")subscribed.insert(v.toString());else if(msg["method"]=="UNSUBSCRIBE")subscribed.remove(v.toString());}}
        void tick(const QString& symbol,const QString& price,qint64 when){emit received(QJsonDocument(QJsonObject{{"e","24hrMiniTicker"},{"s",symbol},{"c",price},{"o","2"},{"E",QString::number(when)}}).toJson(QJsonDocument::Compact));}
    };
    auto ticker=std::make_unique<TickerSocket>();auto* wire=ticker.get();PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::move(ticker));
    QTemporaryDir directory;QSettings profile(directory.filePath("profile.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("never-created-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[](){return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    PublicMarketCatalog catalog(&catalogHttp);PublicEquityBoard board(&boardHttp);MarketCandles candles(&candleHttp,std::make_unique<Socket>());
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setPublicMarketCatalog(&catalog,true);
    window.setPublicEquityBoard(&board);window.setChartInstrumentTransport(&metadataHttp);window.setMarketCandles(&candles);window.setChartRowQuoteStream(&stream);window.resize(1440,900);window.show();board.refresh();
    auto* instruments=window.findChild<ChartInstrumentCatalog*>();QVERIFY(instruments);instruments->ensure("binance");
    QTRY_COMPARE(instruments->instruments("binance").size(),3);QTRY_COMPARE(board.items("coin").size(),3);QTRY_COMPARE(board.items("kr").size(),1);
    QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);
    auto* views=window.findChild<QComboBox*>("exploreView");QVERIFY(views);views->setCurrentIndex(views->findData("map"));
    auto* map=window.findChild<MarketMapWidget*>("marketMap");auto* detail=window.findChild<AssetDetailPanel*>("marketMapDetail");
    auto* chart=window.findChild<ChartWorkspace*>();auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* explore=window.findChild<QWidget*>("explorePage");
    QVERIFY(map&&detail&&chart&&pages&&explore);auto* link=detail->findChild<QPushButton*>("assetDetailChart");QVERIFY(link);
    const auto select=[&](const QString& section,const QString& id){
        pages->setCurrentWidget(explore);QCoreApplication::processEvents();QRectF rect;
        for(const auto& tile:map->tileRects())if(tile.first==section+":"+id)rect=tile.second;
        QVERIFY(!rect.isEmpty());QTest::mouseClick(map,Qt::LeftButton,{},rect.center().toPoint());
        QCOMPARE(pages->currentWidget(),explore);QVERIFY(link->isVisible());
    };
    auto* search = window.findChild<QLineEdit*>("exploreSearch");
    auto* results = window.findChild<QTableWidget*>("exploreMarkets");
    QVERIFY(search && results);
    QSignalSpy edits(search, &QLineEdit::textChanged);
    emit catalog.changed();
    QCOMPARE(edits.size(), 0);
    views->setCurrentIndex(views->findData("list"));
    search->setText("KRW-SYN");
    QCOMPARE(edits.size(), 1);
    QCOMPARE(results->rowCount(), catalog.rows().size());
    for (int row = 0; row < results->rowCount(); ++row)
        QCOMPARE(results->isRowHidden(row), results->item(row, 1)->text() != "KRW-SYN");
    emit catalog.changed();
    QCOMPARE(edits.size(), 1);
    search->clear();
    views->setCurrentIndex(views->findData("map"));
    const auto initialChart=chart->selectedChartIdentity();
    // Upbit lists SYN as "Synthetic" while the global quote is "Synthetic SYN":
    // a conflicting exchange name never redirects the quote to that market.
    select("coin","SYN");QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),explore);QCOMPARE(chart->selectedChartIdentity(),initialChart);
    QVERIFY(window.findChild<QLabel*>("localFeedback")->text().contains(QStringLiteral("같은 자산 여부")));
    // ALT has no Upbit KRW market, so no exchange name contradicts the quote;
    // navigation keeps the earlier Binance spot route (Dev 90 behaviour).
    select("coin","ALT");QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),static_cast<QWidget*>(chart));QCOMPARE(chart->selectedChartIdentity(),QString("coin:binance:ALTUSDT"));
    select("coin","NOPE");QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),explore);QCOMPARE(chart->selectedChartIdentity(),QString("coin:binance:ALTUSDT"));
    QVERIFY(window.findChild<QLabel*>("localFeedback")->text().contains(QStringLiteral("NOPE 차트를 열 수 없어요")));
    select("kr","005930");QTest::mouseClick(link,Qt::LeftButton);QCOMPARE(pages->currentWidget(),explore);
    auto* guide=window.findChild<QDialog*>("chartConnectionGuide");QVERIFY(guide&&guide->isVisible());guide->close();
    pages->setCurrentWidget(explore);views->setCurrentIndex(views->findData("list"));
    auto* ranking=window.findChild<SpotlightCard*>("spotlightCard_coin");auto* venue=window.findChild<QComboBox*>("rankingCoinVenue");QVERIFY(ranking&&venue);
    QCOMPARE(venue->currentData(), "upbit");
    QCOMPARE(venue->itemData(1), "binance");
    QTRY_COMPARE(ranking->visibleItems().size(),1);QCOMPARE(ranking->visibleItems().first().id,"SYN");QCOMPARE(ranking->visibleItems().first().changePercent,"10");
    const auto coinBox = ranking->size();
    venue->setFocus();QTest::keyClick(venue,Qt::Key_Down);
    QTRY_COMPARE(venue->currentData(), "binance");QTRY_COMPARE(ranking->visibleItems().size(),3);QCOMPARE(ranking->visibleItems().first().id,"SYN");QCOMPARE(ranking->visibleItems().first().changePercent,"8");
    QCOMPARE(ranking->size(),coinBox);
    QVERIFY(ranking->visibleItems().first().price.endsWith(" USDT"));QVERIFY(ranking->findChild<QLabel*>("spotlightStatus")->text().contains("24시간"));
    emit ranking->itemSelected("coin","SYN");QCOMPARE(pages->currentWidget(),static_cast<QWidget*>(chart));QCOMPARE(chart->selectedChartIdentity(),"coin:binance:SYNUSDT");
    auto* price=chart->findChild<QLabel*>("chartCurrentPrice");QVERIFY(price);QTRY_COMPARE(price->text(),QString("2.5"));
    QTRY_VERIFY(wire->subscribed.contains("synusdt@miniTicker"));
    const auto first=board.knownItem("binance","SYNUSDT");QVERIFY(first.sourceTimeMs>0);
    QSignalSpy ticks(&stream,&PublicQuoteStream::ticks);wire->tick("SYNUSDT","2.1",first.sourceTimeMs-1000);
    QTRY_COMPARE(ticks.size(),1);emit candles.changed();QCOMPARE(price->text(),QString("2.5"));
    wire->tick("SYNUSDT","2.75",QDateTime::currentMSecsSinceEpoch());QTRY_COMPARE_WITH_TIMEOUT(price->text(),QString("2.75"),4500);
    pages->setCurrentWidget(explore);emit ranking->itemSelected("coin",QStringLiteral("合成"));
    QCOMPARE(chart->selectedChartIdentity(),QStringLiteral("coin:binance:合成USDT"));QTRY_COMPARE(price->text(),QString("2.5"));
    QTRY_VERIFY_WITH_TIMEOUT(wire->subscribed.contains(QStringLiteral("合成usdt@miniTicker")),3000);
    wire->tick(QStringLiteral("合成USDT"),"2.8",QDateTime::currentMSecsSinceEpoch());QTRY_COMPARE_WITH_TIMEOUT(price->text(),QString("2.8"),4500);
    QVERIFY(candles.series().candles.isEmpty());
    pages->setCurrentWidget(explore);venue->setCurrentIndex(0);emit ranking->itemSelected("coin","SYN");QCOMPARE(chart->selectedChartIdentity(),"coin:upbit:KRW-SYN");
    QVERIFY(!vault.exists());QVERIFY(vault.accounts().isEmpty());
    for(const auto* transport:{&catalogHttp,&boardHttp,&metadataHttp,&candleHttp})for(const auto& request:transport->requests)QVERIFY(!request.authenticated);
}

void ExplorePresentationTest::boardCoinsOutsidePinnedIdsOpenChartsAndUnverifiedHoldingsSaySo(){
    // Dev 91 review: only 18 pinned CoinGecko ids could reach a chart or a
    // holding from a global quote. PEPE and BNB are outside that table.
    class Transport final:public TradingTransport {
      public:
        QString role;QList<RequestSpec> requests;
        void abort()override{}
        void send(quint64 token,const RequestSpec& request)override{
            QVERIFY(!request.authenticated);requests.append(request);if(role=="silent")return;
            HttpResult reply;reply.status=200;
            if(role=="catalog"){
                if(request.operation==Operation::Markets)reply.body=R"([{"market":"KRW-PEPE","korean_name":"페페","english_name":"Pepe","market_event":{"warning":false,"caution":{}}},{"market":"KRW-MIS","korean_name":"다른 코인","english_name":"Different Coin","market_event":{"warning":false,"caution":{}}}])";
                else{QJsonArray rows;for(const auto& market:{QString("KRW-PEPE"),QString("KRW-MIS")})
                        rows.append(QJsonObject{{"market",market},{"trade_price","10"},{"prev_closing_price","9"},{"signed_change_rate","0.1"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch()-100)}});
                    reply.body=QJsonDocument(rows).toJson();}
            }else if(role=="metadata"){
                QJsonArray rows;for(const auto& asset:{QString("PEPE"),QString("BNB")})rows.append(QJsonObject{{"symbol",asset+"USDT"},{"baseAsset",asset},{"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}});
                reply.body=QJsonDocument(QJsonObject{{"symbols",rows}}).toJson();
            }else if(request.url.host()=="api.binance.com"){
                QJsonArray rows;for(const auto& base:{"PEPE","BNB"})rows.append(QJsonObject{{"symbol",QString(base)+"USDT"},{"lastPrice","2.5"},{"priceChangePercent","1"},{"quoteVolume","12000"},{"count",5},{"closeTime",QDateTime::currentMSecsSinceEpoch()-100}});
                reply.body=QJsonDocument(rows).toJson();
            }else if(request.url.host()=="api.coingecko.com"){
                // MIS shares Upbit's symbol but not its English name.
                QJsonArray rows;int rank=0;
                for(const auto& coin:QList<QStringList>{{"pepe","pepe","Pepe"},{"mis","mismatched-coin","Mismatched Coin"},{"bnb","binancecoin","BNB"},{"nope","nope-coin","Nope"}})
                    rows.append(QJsonObject{{"id",coin[1]},{"symbol",coin[0]},{"name",coin[2]},{"market_cap",1000000000-rank++*1000},{"current_price",100},{"price_change_percentage_24h",1.0}});
                reply.body=QJsonDocument(rows).toJson();
            }else if(request.url.host()=="m.stock.naver.com")reply.body=R"({"marketStatus":"CLOSE","stocks":[{"itemCode":"005930","stockName":"합성 국내 주식","closePrice":"100","fluctuationsRatio":"1","compareToPreviousPrice":{"code":"2"},"marketValue":"13000000"}]})";
            else reply.body=R"({"data":{"rows":[{"symbol":"SYN","name":"Synthetic US","lastsale":"$100","pctchange":"1%","marketCap":"1000000000","sector":"Technology"}]}})";
            QTimer::singleShot(0,this,[this,token,reply]{emit finished(token,reply);});
        }
    } catalogHttp,boardHttp,metadataHttp,candleHttp;
    catalogHttp.role="catalog";boardHttp.role="board";metadataHttp.role="metadata";candleHttp.role="silent";
    class Socket final:public PublicCandleSocket {public:void open(quint64,const QUrl&)override{}void send(quint64,const QByteArray&)override{}void abort()override{}};
    QTemporaryDir directory;QSettings profile(directory.filePath("profile.ini"),QSettings::IniFormat);
    // An unlocked synthetic vault with no accounts: holdings are readable and empty.
    Vault vault(directory.filePath("synthetic-route.bin"));QVERIFY(vault.create("Synthetic Route! 2026"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[](){return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    PublicMarketCatalog catalog(&catalogHttp);PublicEquityBoard board(&boardHttp);MarketCandles candles(&candleHttp,std::make_unique<Socket>());
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setChartSettings(&profile);
    window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setChartInstrumentTransport(&metadataHttp);window.setMarketCandles(&candles);
    window.resize(1440,900);window.show();board.refresh();
    const auto cleanup=qScopeGuard([&]{window.lockAndClear();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);});
    auto* instruments=window.findChild<ChartInstrumentCatalog*>();QVERIFY(instruments);instruments->ensure("binance");
    QTRY_COMPARE(instruments->instruments("binance").size(),2);QTRY_COMPARE(board.items("coin").size(),4);
    QTRY_VERIFY(catalog.marketListAvailable()&&catalog.rows().size()==2&&catalog.rows().first().priced);
    auto* views=window.findChild<QComboBox*>("exploreView");QVERIFY(views);views->setCurrentIndex(views->findData("map"));
    auto* map=window.findChild<MarketMapWidget*>("marketMap");auto* detail=window.findChild<AssetDetailPanel*>("marketMapDetail");
    auto* chart=window.findChild<ChartWorkspace*>();auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* explore=window.findChild<QWidget*>("explorePage");
    QVERIFY(map&&detail&&chart&&pages&&explore);auto* link=detail->findChild<QPushButton*>("assetDetailChart");QVERIFY(link);
    auto* info=detail->findChild<InfoButton*>("assetDetailInfo");QVERIFY(info);
    const auto select=[&](const QString& id){
        pages->setCurrentWidget(explore);QCoreApplication::processEvents();QRectF rect;
        for(const auto& tile:map->tileRects())if(tile.first=="coin:"+id)rect=tile.second;
        QVERIFY2(!rect.isEmpty(),qPrintable(id));QTest::mouseClick(map,Qt::LeftButton,{},rect.center().toPoint());
        QCOMPARE(pages->currentWidget(),explore);QVERIFY(link->isVisible());
    };
    const auto holdingLine=[&]{
        info->openInformation();QString text;
        for(auto* dialog:info->findChildren<QDialog*>("infoDialog"))if(dialog->isVisible()){
            if(auto* body=dialog->findChild<QPlainTextEdit*>("infoPrivateText"))text=body->toPlainText();dialog->reject();}
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return text;
    };
    const auto feedback=[&]{return window.findChild<QLabel*>("localFeedback")->text();};
    // Same symbol and the exchange's own English name: Upbit KRW opens, and an
    // empty readable vault truthfully says the coin is not held.
    select("PEPE");QVERIFY2(holdingLine().contains(QStringLiteral("내 보유 · 없음")),qPrintable(holdingLine()));
    QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),static_cast<QWidget*>(chart));QCOMPARE(chart->selectedChartIdentity(),QString("coin:upbit:KRW-PEPE"));
    // Upbit has MIS under a different English name: no chart, and the holding
    // line says the identity is unverified instead of claiming "없음".
    select("MIS");const auto mismatched=holdingLine();
    QVERIFY2(mismatched.contains(QStringLiteral("확인 불가"))&&!mismatched.contains(QStringLiteral("없음")),qPrintable(mismatched));
    QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),explore);QCOMPARE(chart->selectedChartIdentity(),QString("coin:upbit:KRW-PEPE"));
    QVERIFY2(feedback().contains(QStringLiteral("같은 자산 여부")),qPrintable(feedback()));
    // No Upbit KRW market means no conflicting exchange name: Binance spot opens
    // for navigation, while the private holding join stays unverified.
    select("BNB");const auto binanceOnly=holdingLine();
    QVERIFY2(binanceOnly.contains(QStringLiteral("확인 불가")),qPrintable(binanceOnly));
    QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),static_cast<QWidget*>(chart));QCOMPARE(chart->selectedChartIdentity(),QString("coin:binance:BNBUSDT"));
    select("NOPE");QTest::mouseClick(link,Qt::LeftButton);
    QCOMPARE(pages->currentWidget(),explore);QCOMPARE(chart->selectedChartIdentity(),QString("coin:binance:BNBUSDT"));
    QVERIFY2(feedback().contains(QStringLiteral("NOPE 차트를 열 수 없어요")),qPrintable(feedback()));
    for(const auto* transport:{&catalogHttp,&boardHttp,&metadataHttp,&candleHttp})for(const auto& request:transport->requests)QVERIFY(!request.authenticated);
}

void ExplorePresentationTest::retainedOrAmbiguousBoardCoinsNeverRouteBySymbol_data(){
    QTest::addColumn<bool>("retained");QTest::addColumn<bool>("duplicate");QTest::addColumn<bool>("upbitListed");
    QTest::newRow("retained-name-conflict")<<true<<false<<true;
    QTest::newRow("ambiguous-upbit-symbol")<<false<<true<<true;
    QTest::newRow("ambiguous-binance-fallback")<<false<<true<<false;
}

void ExplorePresentationTest::retainedOrAmbiguousBoardCoinsNeverRouteBySymbol(){
    QFETCH(bool,retained);QFETCH(bool,duplicate);QFETCH(bool,upbitListed);
    class Transport final:public TradingTransport {
      public:
        QString role;bool duplicate=false,upbitListed=false,omitTarget=false;QList<RequestSpec> requests;
        void abort()override{}
        void send(quint64 token,const RequestSpec& request)override{
            QVERIFY(!request.authenticated);requests.append(request);if(role=="silent")return;
            HttpResult reply;reply.status=200;
            if(role=="catalog"){
                QJsonArray rows;
                for(const auto& base:upbitListed?QStringList{"PEPE","MIS"}:QStringList{"PEPE"}){
                    if(request.operation==Operation::Markets)rows.append(QJsonObject{{"market","KRW-"+base},{"korean_name",base},
                        {"english_name",base=="PEPE"?QString("Pepe"):duplicate?QString("Mismatched Coin"):QString("Different Coin")}});
                    else rows.append(QJsonObject{{"market","KRW-"+base},{"trade_price","10"},{"prev_closing_price","9"},
                        {"signed_change_rate","0.1"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch()-100)}});
                }
                reply.body=QJsonDocument(rows).toJson();
            }else if(role=="metadata"){
                QJsonArray rows;for(const auto& base:QStringList{"PEPE","MIS"})rows.append(QJsonObject{{"symbol",base+"USDT"},{"baseAsset",base},
                    {"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}});
                reply.body=QJsonDocument(QJsonObject{{"symbols",rows}}).toJson();
            }else if(request.url.host()=="api.coingecko.com"){
                QJsonArray rows{QJsonObject{{"id","pepe"},{"symbol","pepe"},{"name","Pepe"},{"market_cap",2000},{"current_price",1}}};
                if(!omitTarget){
                    rows.append(QJsonObject{{"id","mis-first"},{"symbol","mis"},{"name","Mismatched Coin"},{"market_cap",1000},{"current_price",1}});
                    if(duplicate)rows.append(QJsonObject{{"id","mis-second"},{"symbol","mis"},{"name","Mismatched Coin"},{"market_cap",900},{"current_price",1}});
                }
                reply.body=QJsonDocument(rows).toJson();
            }else if(request.url.host()=="m.stock.naver.com")reply.body=R"({"marketStatus":"CLOSE","stocks":[{"itemCode":"005930","stockName":"합성 주식","closePrice":"100","fluctuationsRatio":"1","compareToPreviousPrice":{"code":"2"},"marketValue":"13000000"}]})";
            else reply.body=R"({"data":{"rows":[{"symbol":"SYN","name":"Synthetic US","lastsale":"$100","pctchange":"1%","marketCap":"1000000000","sector":"Technology"}]}})";
            QTimer::singleShot(0,this,[this,token,reply]{emit finished(token,reply);});
        }
    } catalogHttp,boardHttp,metadataHttp,candleHttp;
    catalogHttp.role="catalog";catalogHttp.upbitListed=upbitListed;catalogHttp.duplicate=duplicate;
    boardHttp.role="board";boardHttp.duplicate=duplicate;metadataHttp.role="metadata";candleHttp.role="silent";
    class Socket final:public PublicCandleSocket {public:void open(quint64,const QUrl&)override{}void send(quint64,const QByteArray&)override{}void abort()override{}};
    QTemporaryDir directory;QSettings profile(directory.filePath("profile.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("never-created-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[](){return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    PublicMarketCatalog catalog(&catalogHttp);PublicEquityBoard board(&boardHttp);MarketCandles candles(&candleHttp,std::make_unique<Socket>());
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);
    window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setChartInstrumentTransport(&metadataHttp);window.setMarketCandles(&candles);
    window.resize(1440,900);window.show();board.refresh();
    auto* instruments=window.findChild<ChartInstrumentCatalog*>();QVERIFY(instruments);instruments->ensure("binance");
    QTRY_COMPARE(instruments->instruments("binance").size(),2);QTRY_COMPARE(board.items("coin").size(),2);
    QTRY_VERIFY(!board.busy()&&catalog.marketListAvailable()&&!catalog.busy());
    auto* watchlist=window.findChild<ExploreWatchlist*>();auto* chart=window.findChild<ChartWorkspace*>();
    auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* explore=window.findChild<QWidget*>("explorePage");
    auto* feedback=window.findChild<QLabel*>("localFeedback");QVERIFY(watchlist&&chart&&pages&&explore&&feedback);
    watchlist->toggle("coin","MIS");pages->setCurrentWidget(explore);const auto initialChart=chart->selectedChartIdentity();
    emit watchlist->itemSelected("coin","MIS");
    QCOMPARE(pages->currentWidget(),explore);QCOMPARE(chart->selectedChartIdentity(),initialChart);
    QVERIFY(feedback->text().contains(QStringLiteral("같은 자산 여부")));
    if(retained){
        boardHttp.omitTarget=true;board.refresh();QTRY_COMPARE(board.items("coin").size(),1);QTRY_VERIFY(!board.busy());
        QCOMPARE(board.knownItem("coin","MIS").sourceId,QString("mis-first"));
        emit watchlist->itemSelected("coin","MIS");
        QCOMPARE(pages->currentWidget(),explore);QCOMPARE(chart->selectedChartIdentity(),initialChart);
        QVERIFY(feedback->text().contains(QStringLiteral("같은 자산 여부")));
    }
    QVERIFY(!vault.exists());QVERIFY(vault.accounts().isEmpty());
    for(const auto* transport:{&catalogHttp,&boardHttp,&metadataHttp,&candleHttp})for(const auto& request:transport->requests)QVERIFY(!request.authenticated);
}

QTEST_MAIN(ExplorePresentationTest)
#include "tst_explore_presentation.moc"
