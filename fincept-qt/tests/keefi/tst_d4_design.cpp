#include "fixtures/PortfolioTestRow.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/Vault.h"
#include "keefi/TradingTransport.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/MarketMetrics.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/MarketOverview.h"
#include "keefi/MarketOverviewView.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicCoinIdentity.h"
#include "keefi/MarketMap.h"
#include "keefi/Spotlight.h"
#include "keefi/InfoButton.h"
#include "keefi/ExploreWatchlist.h"
#include <QComboBox>
#include "keefi/PortfolioDetail.h"
#include <QTimeZone>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPainter>
#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>

using keefi::test::portfolioRow;
using namespace keefi;
class D4ReadTransport final:public TradingTransport {
public:
 QString amount="250000";bool includeBtc=false;
 void abort()override{}
 void send(quint64 token,const RequestSpec& r)override {
  QVERIFY(!isMutation(r.operation));QCOMPARE(r.operation,Operation::Account);
  HttpResult result;result.status=200;result.body=R"([{"currency":"KRW","balance":"250000","locked":"0","avg_buy_price":"0","unit_currency":"KRW"}])";
  result.body.replace("250000",amount.toUtf8());
  if(includeBtc){auto rows=QJsonDocument::fromJson(result.body).array();rows.append(QJsonObject{{"currency","BTC"},{"balance","1"},{"locked","0"},{"avg_buy_price","100000"},{"unit_currency","KRW"}});result.body=QJsonDocument(rows).toJson();}
  QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
 }
};

class D4Socket final:public PublicTickerSocket {
public:
 void open(const QUrl&)override{QTimer::singleShot(0,this,[this]{emit connected();});}
 void send(const QByteArray&)override{}
 void ping()override{}
 void abort()override{}
};
class D4PublicTransport final:public TradingTransport {
public:
 bool holdReplies=false;int delivered=0;
 void abort()override{}
 void send(quint64 token,const RequestSpec& request)override {
  QVERIFY(!request.authenticated);QVERIFY(NetworkTradingTransport::allowed(request));QVERIFY(!isMutation(request.operation));
  HttpResult result;result.status=200;const auto now=QDateTime::currentMSecsSinceEpoch()-20;
  if(OfficialCalendar::isRequest(request)) {
   const auto date=QDateTime::currentDateTimeUtc().addDays(2).toString("yyyyMMdd'T'HHmmss'Z'");const auto stamp=QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'");
   result.body=("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:SYNTHETIC-D4-RELEASE\r\nDTSTAMP:"+stamp+"\r\nDTSTART:"+date+"\r\nSUMMARY:Synthetic public release\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n").toUtf8();
  } else if(MarketOverview::isRequest(request)) {
   const auto date=QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("America/New_York")).date().toString(Qt::ISODate);
   result.body=("<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:d=\"http://schemas.microsoft.com/ado/2007/08/dataservices\" xmlns:m=\"http://schemas.microsoft.com/ado/2007/08/dataservices/metadata\"><entry><content><m:properties><d:NEW_DATE>"+date+"T00:00:00</d:NEW_DATE><d:BC_10YEAR>4.25</d:BC_10YEAR></m:properties></content></entry></feed>").toUtf8();
  } else if(MarketMetrics::isRequest(request)) {
   const auto path=request.url.path();
   if(path.endsWith("exchangeInfo")) result.body=R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})";
   else if(path.endsWith("fundingInfo"))result.body="[]";
   else if(path.endsWith("premiumIndex"))result.body=QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0001"},{"interestRate","0.0001"},{"markPrice","100000"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}}).toJson();
   else if(path.endsWith("openInterest"))result.body=QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"openInterest","12345.67"},{"time",QString::number(now)}}).toJson();
   else QFAIL("Unexpected synthetic metrics path");
  } else if(request.operation==Operation::Markets)result.body=R"([{"market":"KRW-BTC","korean_name":"합성 코인","english_name":"Bitcoin"}])";
  else {QCOMPARE(request.operation,Operation::Quote);result.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"trade_price","123450"},{"prev_closing_price","120000"},{"signed_change_rate","0.02875"},{"timestamp",QString::number(now)}}}).toJson();}
  QTimer::singleShot(0,this,[this,token,result]{if(!holdReplies){++delivered;emit finished(token,result);}});
 }
};

class D4BoardTransport final:public TradingTransport {
public:
 bool holdReplies=false;int delivered=0;
 void abort()override{}
 void send(quint64 token,const RequestSpec& request)override {
  QVERIFY(!request.authenticated);QVERIFY(PublicEquityBoard::isBoardRequest(request));
  HttpResult result;result.status=200;
  if(request.url.host()=="api.coingecko.com")result.body=R"([{"id":"bitcoin","symbol":"btc","name":"Bitcoin","market_cap":1000000,"current_price":123450,"price_change_percentage_24h":2.875,"total_volume":250000000}])";
  else if(request.url.host()=="m.stock.naver.com")result.body=R"({"marketStatus":"CLOSE","stocks":[{"itemCode":"005930","stockName":"Synthetic KR","closePrice":"100","fluctuationsRatio":"1","compareToPreviousPrice":{"code":"2"},"marketValue":"1000"}]})";
  else result.body=R"({"data":{"rows":[{"symbol":"NVDA","name":"Synthetic US","lastsale":"$100","pctchange":"1%","marketCap":"1000000","sector":"Technology"}]}})";
  QTimer::singleShot(0,this,[this,token,result]{if(!holdReplies){++delivered;emit finished(token,result);}});
 }
};

class D4DesignTest final:public QObject {
 Q_OBJECT
private slots:
 void publicFlowUsesOneEvidenceNumberSequence() {
  QTemporaryDir temp;QVERIFY(temp.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
  Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
  D4PublicTransport catalogHttp,metricsHttp,overviewHttp,calendarHttp;OfficialCalendar calendar(&calendarHttp);PublicMarketCatalog catalog(&catalogHttp);MarketMetrics metrics(&metricsHttp);MarketOverview overview(&overviewHttp);
  PublicQuoteStream up(PublicQuoteStream::Provider::Upbit,std::make_unique<D4Socket>()),bn(PublicQuoteStream::Provider::Binance,std::make_unique<D4Socket>());
  LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics,nullptr,&up,&bn);window.setMarketOverview(&overview);window.setOfficialCalendar(&calendar);window.setSecurityStatus(true);window.resize(1440,1050);window.show();
  auto* view=window.findChild<QComboBox*>("exploreView");QVERIFY(view);view->setCurrentIndex(view->findData("flow"));
  auto* treasury=window.findChild<MarketOverviewView*>();auto* selected=window.findChild<MarketMetricsView*>();QVERIFY(treasury);QVERIFY(selected);
  QTRY_COMPARE_WITH_TIMEOUT(treasury->evidenceCount(),1,5000);QTRY_COMPARE_WITH_TIMEOUT(selected->evidenceCount(),3,5000);
  auto* treasuryCard=treasury->findChild<ExploreMetricCard*>("marketOverviewTreasuryCard");
  auto* fundingCard=selected->findChild<ExploreMetricCard*>("marketMetricsFundingCard");QVERIFY(treasuryCard&&fundingCard);
  auto* treasuryBook=treasury->findChild<ExploreEvidenceFooter*>("marketOverviewEvidenceFooter");
  auto* notes=selected->findChild<ExploreEvidenceFooter*>("marketMetricsEvidenceFooter");QVERIFY(treasuryBook&&notes);
  QCOMPARE(treasuryBook->numbers(),QList<int>{1});QCOMPARE(notes->numbers(),(QList<int>{2,3,4}));
  QCOMPARE(treasuryCard->data().evidence.size(),1);QCOMPARE(fundingCard->data().evidence.size(),2);
  const auto treasuryEvidence=treasuryBook->entries(),metricEvidence=notes->entries();
  QVERIFY(treasuryBook->isHidden());QVERIFY(notes->isHidden());
  QVERIFY(treasuryCard->findChild<QLabel*>("metricEvidenceReference")->text().isEmpty());
  QVERIFY(fundingCard->findChild<QLabel*>("metricEvidenceReference")->text().isEmpty());
  window.lockAndClear();QCOMPARE(treasury->evidenceCount(),1);QCOMPARE(selected->evidenceCount(),3);
  QCOMPARE(treasuryBook->entries(),treasuryEvidence);QCOMPARE(notes->entries(),metricEvidence);
  const auto out=qEnvironmentVariable("KEEFI_D4_RENDER_DIR");if(!out.isEmpty()){
   QDir().mkpath(out);auto* scroll=window.findChild<QScrollArea*>("marketFlowPage");QVERIFY(scroll);
   for(auto mode:{ThemeMode::Light,ThemeMode::Dark}){KeefiTheme::instance()->setMode(mode);for(int width:{1440,720}){
    window.resize(width,1050);scroll->verticalScrollBar()->setValue(0);QTest::qWait(40);const auto prefix=out+"/d4-explore-"+(mode==ThemeMode::Light?"light":"dark")+"-"+QString::number(width);
    QVERIFY(window.grab().save(prefix+".png"));if(width==720){scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());QTest::qWait(30);QVERIFY(window.grab().save(prefix+"-lower.png"));}
   }}
  }
 }
 void valuationEvidenceStaysInternalAndClearsOnLock() {
  QTemporaryDir temp;QVERIFY(temp.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic D4");QCoreApplication::setApplicationName("D4 Synthetic Design");
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
  QVERIFY(KeefiFonts::registerBundledFonts());Vault vault(temp.filePath("synthetic-only.bin"));QVERIFY(vault.create("Fake D4 Vault Password! 2026"));
  AccountRecord account;account.provider="upbit";account.environment="live";account.market="spot";account.currency="KRW";account.alias="FAKE_D4_ALIAS";
  QVERIFY(vault.addAccount(account,{{"access_key","FAKE_D4_KEY"},{"secret","FAKE_D4_SECRET"}},StorageMode::EncryptedVault));
  D4ReadTransport http;PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &http;};f.prices=[] {return static_cast<TradingTransport*>(nullptr);};
  PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1440,1050);window.show();portfolio.refreshAll();
  auto* figure=window.findChild<QLabel*>("portfolioTotal");QVERIFY(figure);QTRY_COMPARE_WITH_TIMEOUT(figure->text(),QString("250,000"),6000);
  auto* refs=window.findChild<QLabel*>("portfolioTotalEvidence");QVERIFY(refs);QVERIFY(refs->text().isEmpty());QVERIFY(refs->isHidden());
  auto* footer=window.findChild<ExploreEvidenceFooter*>("portfolioEvidenceFooter");QVERIFY(footer);QVERIFY(footer->isHidden());
  QVERIFY(!footer->entries().isEmpty());QVERIFY(!footer->numbers().isEmpty());
  QCOMPARE(footer->entries().first().provider,QStringLiteral("업비트"));
  QVERIFY(footer->entries().first().receivedAtMs>0);
  QVERIFY(footer->findChildren<QLabel*>("metricEvidenceLine").isEmpty());
  portfolio.requestDayHistory();QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),5000);QTRY_VERIFY_WITH_TIMEOUT(portfolio.dayHistoryResult().comparison.available,5000);
  http.amount="275000";portfolio.refreshAll();QTRY_COMPARE_WITH_TIMEOUT(figure->text(),QString("275,000"),5000);portfolio.requestDayHistory();QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),5000);
  QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("portfolioDayChange")->text().contains("25,000"),5000);
  auto* tree=window.findChild<QTreeView*>("portfolioAssets");QVERIFY(tree);QVERIFY(tree->model()->rowCount()>0);tree->setCurrentIndex(portfolioRow(tree, 0).modelIndex());
  const auto out=qEnvironmentVariable("KEEFI_D4_RENDER_DIR");if(!out.isEmpty()){QDir().mkpath(out);for(auto mode:{ThemeMode::Light,ThemeMode::Dark}){KeefiTheme::instance()->setMode(mode);for(int width:{1440,720}){window.resize(width,1050);QTest::qWait(40);QVERIFY(window.grab().save(out+"/d4-assets-"+(mode==ThemeMode::Light?"light":"dark")+"-"+QString::number(width)+".png"));if(width==720){QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(portfolioRow(tree, 0).modelIndex()).center());QTest::qWait(30);QVERIFY(window.grab().save(out+"/d4-assets-"+(mode==ThemeMode::Light?"light":"dark")+"-720-detail.png"));window.findChild<QPushButton*>("assetDetailBack")->click();}}}}
  window.lockAndClear();QVERIFY(refs->text().isEmpty());QVERIFY(footer->entries().isEmpty());QVERIFY(footer->numbers().isEmpty());QVERIFY(footer->isHidden());
 }
 void selectedPublicBoardLinkageClearsAtEveryLockBoundary_data() {
  QTest::addColumn<bool>("dark");QTest::addColumn<QString>("route");QTest::addColumn<QString>("lockMethod");QTest::addColumn<bool>("boardSource");
  // The ranking has no selected detail any more (its rows open the chart). The map keeps the
  // board detail; the Upbit search detail keeps the Upbit card with hidden holding rows.
  for(bool dark:{false,true})for(const auto& method:{QString("vault"),QString("window"),QString("security")})
    QTest::newRow(qPrintable(QString("map-%1-%2").arg(dark?"dark":"light",method)))<<dark<<QString("map")<<method<<true;
  for(bool dark:{false,true})QTest::newRow(dark?"search-upbit-dark-vault":"search-upbit-light-vault")<<dark<<QString("search")<<QString("vault")<<false;
 }
 void selectedPublicBoardLinkageClearsAtEveryLockBoundary() {
  QFETCH(bool,dark);QFETCH(QString,route);QFETCH(QString,lockMethod);QFETCH(bool,boardSource);
  QTemporaryDir temp;QVERIFY(temp.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
  QVERIFY(KeefiFonts::registerBundledFonts());KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
  Vault vault(temp.filePath("synthetic-linkage-vault"));QVERIFY(vault.create("Fake D4 Linkage! 2026"));
  AccountRecord account;account.provider="upbit";account.environment="live";account.market="spot";account.currency="KRW";account.alias="FAKE_LINKAGE_ALIAS";
  QVERIFY(vault.addAccount(account,{{"access_key","FAKE_LINKAGE_KEY"},{"secret","FAKE_LINKAGE_SECRET"}},StorageMode::EncryptedVault));
  D4ReadTransport privateHttp;privateHttp.includeBtc=true;PortfolioController::Factories factories;
  factories.trading=[&](const AccountRecord&){return &privateHttp;};factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
  PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);
  D4PublicTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);D4BoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
  LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);
  if(boardSource)window.setPublicEquityBoard(&board);window.setSecurityStatus(true);window.resize(1440,1100);window.show();KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);if(boardSource)board.refresh();portfolio.refreshAll();
  QCOMPARE(KeefiTheme::instance()->isDark(),dark);
  QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy()&&!catalog.rows().isEmpty()&&catalog.rows().first().priced,5000);
  if(boardSource){
   QTRY_VERIFY_WITH_TIMEOUT(!board.busy()&&!board.items("coin").isEmpty(),5000);
   QVERIFY2(verifiedCoinMarketCap("BTC",catalog.rows(),board.items("coin")).has_value(),
            "The synthetic public sources must prove the same BTC before testing private linkage removal");
  }
  QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("portfolioLoading")->isHidden()&&!portfolio.snapshots().isEmpty(),5000);
  auto* view=window.findChild<QComboBox*>("exploreView");auto* section=window.findChild<QComboBox*>("exploreMarketSection");QVERIFY(view&&section);
  section->setCurrentIndex(section->findData("coin"));view->setCurrentIndex(view->findData(route=="map"?"map":"list"));QCoreApplication::processEvents();
  if(route=="map") {
   auto* map=window.findChild<MarketMapWidget*>("marketMap");QVERIFY(map);QRectF selected;
   for(const auto& tile:map->tileRects())if(tile.first=="coin:BTC")selected=tile.second;
   QVERIFY(selected.isValid());QTest::mouseClick(map,Qt::LeftButton,Qt::NoModifier,selected.center().toPoint());
  }else{
   auto* search=window.findChild<QLineEdit*>("exploreSearch");auto* table=window.findChild<QTableWidget*>("exploreMarkets");QVERIFY(search&&table);
   search->setText("BTC");int row=-1;for(int i=0;i<table->rowCount();++i)if(!table->isRowHidden(i)){row=i;break;}QVERIFY(row>=0);
   QTRY_VERIFY(table->isVisibleTo(&window));table->scrollToItem(table->item(row,0));QCoreApplication::processEvents();
   QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,table->visualItemRect(table->item(row,0)).center());
  }
  auto* panel=window.findChild<AssetDetailPanel*>(route=="map"?"marketMapDetail":"exploreDetail");QVERIFY(panel);
  auto* card=panel->findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(card);QVERIFY(card->isVisibleTo(&window));
  auto* information=card->informationButton();QVERIFY(information);information->openInformation();
  QTRY_VERIFY(information->informationOpen());QPointer<QDialog> dialog=information->findChild<QDialog*>("infoDialog");QVERIFY(dialog);
  QPointer<QPlainTextEdit> privateText=dialog->findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(privateText);
  QVERIFY2(privateText->toPlainText().contains(boardSource?QStringLiteral("보유 · 보관처 1곳"):QStringLiteral("보관처 1곳")),"Synthetic BTC holdings must be projected before testing their removal");
  if(!boardSource){
   QVERIFY(privateText->toPlainText().contains("FAKE_LINKAGE_ALIAS"));bool hiddenAlias=false;
   for(auto* label:panel->findChildren<QLabel*>())if(label->text().contains("FAKE_LINKAGE_ALIAS")&&!label->isVisibleTo(&window))hiddenAlias=true;
   QVERIFY2(hiddenAlias,"Fallback must contain the synthetic alias in hidden legacy holding widgets before lock");
  }
  // Mockup fact rows: public 24h traded value always; the 내 보유 row only while readable.
  const auto visibleRow=[&](const QString& text){for(auto* label:panel->findChildren<QLabel*>())if(label->isVisibleTo(&window)&&label->text().contains(text))return true;return false;};
  if(boardSource){QVERIFY(visibleRow(QStringLiteral("24시간 거래대금")));QVERIFY2(visibleRow(QStringLiteral("내 보유")),"Synthetic BTC holdings must show the 내 보유 row before lock");}
  QVERIFY(privateText->isVisible());QPointer<QLabel> privateHeading;
  for(auto* label:dialog->findChildren<QLabel*>())if(label->text()==QStringLiteral("내 연동"))privateHeading=label;
  QVERIFY(privateHeading&&privateHeading->isVisible());
  const auto publicData=card->data();const auto publicValue=card->findChild<QLabel*>("metricValue")->text();
  QVERIFY(!publicData.value.isEmpty());QVERIFY(publicData.sourceKey.startsWith(boardSource?"coin:coingecko:":"coin:upbit:"));
  // Pending public reads cannot repair a stale private projection after lock.
  catalogHttp.holdReplies=true;boardHttp.holdReplies=true;catalog.refresh();if(boardSource)board.refresh();QCoreApplication::processEvents();
  QVERIFY(catalog.busy());if(boardSource)QVERIFY(board.busy());const int publicReplies=catalogHttp.delivered+boardHttp.delivered;
  const auto capture=[&](const QString& phase) {
   const auto out=qEnvironmentVariable("KEEFI_G8_FOLLOWUP_PRIVACY_RENDER_DIR");if(out.isEmpty())return true;
   if(!QDir().mkpath(out))return false;const auto screen=window.grab();const bool open=dialog&&dialog->isVisible();
   const QPixmap detail=open?dialog->grab():QPixmap{};QImage image(screen.width()+qMax(520,detail.width()),qMax(screen.height(),detail.height()),QImage::Format_ARGB32_Premultiplied);
   image.fill(themeColors().bg);QPainter painter(&image);painter.drawPixmap(0,0,screen);if(open)painter.drawPixmap(screen.width(),0,detail);painter.end();
   return image.save(QDir(out).filePath(QString::fromLatin1(QTest::currentDataTag())+"-"+phase+".png"));
  };
  QVERIFY(capture("before"));
  if(lockMethod=="vault")vault.lock();else if(lockMethod=="window")window.lockAndClear();else window.setSecurityStatus(false,"SYNTHETIC-D4-SECURITY-LOSS");
  // Read immediately, without waiting for another public response/event turn.
  const bool privateEmpty=!privateText||privateText->toPlainText().isEmpty();
  const bool privateHeadingHidden=!privateHeading||!privateHeading->isVisible();
  const bool holdingRowGone=!visibleRow(QStringLiteral("내 보유"));
  const bool informationClosed=!information->informationOpen();
  bool noLegacyAlias=true;if(!boardSource){
   for(auto* label:panel->findChildren<QLabel*>())if(label->text().contains("FAKE_LINKAGE_ALIAS"))noLegacyAlias=false;
   for(auto* text:panel->findChildren<QPlainTextEdit*>())if(text->toPlainText().contains("FAKE_LINKAGE_ALIAS"))noLegacyAlias=false;
  }
  QVERIFY(capture("after"));
  QCOMPARE(catalogHttp.delivered+boardHttp.delivered,publicReplies);QVERIFY(!vault.isUnlocked());
  QCOMPARE(card->data().value,publicData.value);QCOMPARE(card->data().unit,publicData.unit);QCOMPARE(card->data().sourceKey,publicData.sourceKey);
  QCOMPARE(card->findChild<QLabel*>("metricValue")->text(),publicValue);
  QVERIFY2(noLegacyAlias,"Fallback must erase the synthetic alias from visible and hidden private holding text at lock");
  QVERIFY2(privateEmpty,"Public Map/Spot detail must synchronously erase private linkage before another public response");
  QVERIFY2(privateHeadingHidden,"The old private linkage heading must not remain visible after lock");
  QVERIFY2(holdingRowGone,"The 내 보유 fact row must disappear synchronously at lock");
  if(boardSource)QVERIFY(visibleRow(QStringLiteral("24시간 거래대금")));
  QVERIFY2(informationClosed,"Lock must close the private linkage information dialog");
 }
};
QTEST_MAIN(D4DesignTest)
#include "tst_d4_design.moc"
