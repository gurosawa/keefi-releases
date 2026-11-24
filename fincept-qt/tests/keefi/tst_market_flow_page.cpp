#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "fixtures/ProviderReadFixture.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/AccountModel.h"
#include "keefi/Vault.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/Spotlight.h"
#include "keefi/InfoButton.h"
#include "keefi/MarketMetrics.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/MarketCandles.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/OfficialCalendarView.h"
#include "keefi/OfficialNotices.h"
#include "keefi/OfficialNoticesView.h"
#include "keefi/EarningsCalendar.h"
#include "keefi/EarningsCalendarView.h"
#include "keefi/MarketOverview.h"
#include "keefi/MarketOverviewView.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/TradingTransport.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QTimeZone>
#include <QSignalBlocker>
#include <QTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QPushButton>
#include <QDir>
#include <QTextDocument>
#include <QPlainTextEdit>
#include <QProgressBar>
#include "keefi/InfoButton.h"
#include <QCheckBox>
#include <QComboBox>
#include <QBoxLayout>
#include <QTabWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QLocale>
#include <QUrlQuery>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QLineEdit>
#include <QFontMetrics>
#include <QTableWidget>
#include <cmath>
using namespace keefi;
namespace {
const qint64 Now=QDateTime::currentMSecsSinceEpoch();
class FeedHttp final:public TradingTransport {
  public:
    QList<RequestSpec> requests;quint64 token=0;
    void send(quint64 next,const RequestSpec& request)override{QVERIFY(NetworkTradingTransport::allowed(request));QVERIFY(!request.authenticated);requests.append(request);token=next;}
    void abort()override{}
    void answer(int status,const QByteArray& body={}){HttpResult r;r.status=status;r.body=body;emit finished(token,r);}
};
class DashboardBoardHttp final:public TradingTransport {
public:
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(PublicEquityBoard::isBoardRequest(request));HttpResult reply;reply.status=200;QJsonArray rows;
        const QStringList coins{"BTC","ETH","SOL","XRP","BNB","DOGE","ADA","AVAX","DOT","LINK","TRX","SUI","NEAR","ATOM","LTC","BCH","UNI","XLM","AAVE","ETC"};
        for(int i=0;i<20;++i){const auto change=QString::number((i%2?-1:1)*(i+1)*0.31,'f',2);
            if(request.url.host()=="api.coingecko.com")rows.append(QJsonObject{{"id","synthetic-"+coins[i].toLower()},{"symbol",coins[i].toLower()},{"name",QString("합성 ")+coins[i]},{"market_cap",double(2000000000000LL-i*50000000000LL)},{"current_price",i==0?144200000.0:5000000.0/(i+1)},{"price_change_percentage_24h",change.toDouble()},{"total_volume",double(9000000000LL-i*1000000)}});
            else if(request.url.host()=="m.stock.naver.com")rows.append(QJsonObject{{"itemCode",QString::number(5930+i).rightJustified(6,'0')},{"stockName",QString("합성 한국 %1").arg(i+1)},{"closePrice",QString::number(220000+i*500)},{"fluctuationsRatio",change},{"compareToPreviousPrice",QJsonObject{{"code",i%2?"5":"2"}}},{"marketValue",QString::number(13000000-i*300000)}});
            else rows.append(QJsonObject{{"symbol",QString("US")+QChar('A'+i)},{"name",QString("Synthetic US %1").arg(i+1)},{"lastsale",QString("$%1.25").arg(210+i*12)},{"pctchange",change+"%"},{"marketCap",QString::number(5000000000000LL-i*50000000000LL)},{"sector","Technology"}});
        }
        reply.body=request.url.host()=="api.coingecko.com"?QJsonDocument(rows).toJson():request.url.host()=="m.stock.naver.com"?QJsonDocument(QJsonObject{{"marketStatus","CLOSE"},{"stocks",rows}}).toJson():QJsonDocument(QJsonObject{{"data",QJsonObject{{"rows",rows}}}}).toJson();
        QTimer::singleShot(1,this,[this,token,reply]{emit finished(token,reply);});
    }
};
// Synthetic week of earnings for the capture: KIND HTML rows and one Nasdaq day.
QByteArray syntheticKindWeek(const QDate& today){
    const auto row=[](const char* code,const char* name,const char* title,const QDate& date,const char* time){return QByteArray("<tr><td>1</td><td><a href=\"#\" onclick=\"companysummary_open('")+code+"'); return false;\">"+name+"</a></td><td>"+title+"</td><td>-</td><td>"+date.toString(Qt::ISODate).toLatin1()+"</td><td>"+time+"</td></tr>";};
    return "<table><tbody>"+row("00066","가상하이닉스","3분기 경영실적 발표",today.addDays(1),"09:00")+row("00593","가상전자","2026년 3분기 실적 설명회",today.addDays(2),"10:00")+"</tbody></table>전체 <em>2</em>건";
}
QByteArray syntheticNasdaqDay(const QDate& date,bool rows){
    QJsonArray list;if(rows)list={QJsonObject{{"symbol","MEGA"},{"name","Mega Synthetic Inc."},{"time","time-after-hours"},{"marketCap","$2,000,000,000,000"},{"fiscalQuarterEnding","Sep/2026"},{"epsForecast","$2.01"},{"noOfEsts","14"}}};
    return QJsonDocument(QJsonObject{{"data",QJsonObject{{"asOf",QLocale(QLocale::English).toString(date,"ddd, MMM d, yyyy")},{"rows",list}}},{"status",QJsonObject{{"rCode",200}}}}).toJson(QJsonDocument::Compact);
}
class FlowSocket final:public PublicTickerSocket {
  public:
    int opens=0;void open(const QUrl&)override{++opens;}void send(const QByteArray&)override{}void ping()override{}void abort()override{}
    void ready(){emit connected();}void tick(const QJsonObject& row){emit received(QJsonDocument(row).toJson(QJsonDocument::Compact));}
};
class FlowSink final:public SystemNotificationSink {
  public:
    int authorizations=0;QList<SystemNotice> posts;
    SystemNoticePermission permission()const override{return SystemNoticePermission::Granted;}
    QString status()const override{return {};}
    void refreshPermission()override{}void requestAuthorization()override{++authorizations;}
    void post(const SystemNotice& notice)override{posts.append(notice);emit submissionFinished(notice.identifier,notice.privateEpoch,true);}
    void remove(const QStringList&)override{}void setPrivateContext(quint64,bool)override{}void stop()override{}
};
QByteArray event(const QByteArray& uid,const QByteArray& title,qint64 when){
    return "BEGIN:VEVENT\r\nUID:"+uid+"\r\nSUMMARY:"+title+"\r\nDTSTART:"+QDateTime::fromMSecsSinceEpoch(when,Qt::UTC).toString("yyyyMMdd'T'HHmmss'Z'").toUtf8()+"\r\nDTSTAMP:"+QDateTime::fromMSecsSinceEpoch(Now-60000,Qt::UTC).toString("yyyyMMdd'T'HHmmss'Z'").toUtf8()+"\r\nSEQUENCE:0\r\nEND:VEVENT\r\n";
}
QByteArray calendar(){return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"+event("SYNTHETIC_CPI","Consumer Price Index",Now+7200000)+event("SYNTHETIC_JOBS","Employment Situation",Now+6*86400000LL)+event("SYNTHETIC_FAR","Gross Domestic Product",Now+8*86400000LL)+"END:VCALENDAR\r\n";}
}
class MarketFlowPageTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
  private slots:
    void todayKeepsItsPositionDuringNoticeRefresh_data(){
        QTest::addColumn<int>("width");
        QTest::newRow("stacked")<<1024;
        QTest::newRow("desktop")<<1440;
        QTest::newRow("wide")<<1920;
    }
    void todayKeepsItsPositionDuringNoticeRefresh(){
        QFETCH(int,width);
        Vault vault(settings_.filePath(QString("stable-today-%1").arg(width)));AccountModel accounts(&vault);
        FeedHttp catalogHttp,metricHttp,noticesHttp;PublicMarketCatalog catalog(&catalogHttp);MarketMetrics metrics(&metricHttp);OfficialNotices notices(&noticesHttp);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setOfficialNotices(&notices);
        window.resize(width,900);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* dashboard=window.findChild<ExploreTodayView*>();auto* metric=window.findChild<QWidget*>("todayMetricPanel");auto* notice=window.findChild<QWidget*>("todayNoticePanel");auto* schedule=window.findChild<QWidget*>("todayScheduleRow");QVERIFY(dashboard&&metric&&notice&&schedule);
        const auto settle=[](){for(int i=0;i<16;++i)QCoreApplication::processEvents();};
        const QByteArray row=R"({"code":"0","data":[{"details":[{"annType":"announcements-delisting","title":"Synthetic delisting","url":"https://www.okx.com/help/synthetic","pTime":"1750000000000"}]}]})";
        const QByteArray empty=R"({"code":"0","data":[{"details":[]}]})";
        QTRY_COMPARE(noticesHttp.requests.size(),1);noticesHttp.answer(200,row);
        QTRY_COMPARE(noticesHttp.requests.size(),2);noticesHttp.answer(200,"[]");QTRY_VERIFY(notice->isVisible());settle();
        const auto position=[&](){return QPoint(metric->mapTo(&window,QPoint()).y(),schedule->mapTo(&window,QPoint()).y());};
        const auto shown=position();
        notices.refresh();QTRY_COMPARE(noticesHttp.requests.size(),3);settle();
        QVERIFY(notices.feed(OfficialNoticeSource::Okx).notices.isEmpty());QCOMPARE(position(),shown);QVERIFY(notice->isVisible());
        // A transient refresh must not retain an actionable row from the old feed.
        QVERIFY(notice->findChildren<InfoButton*>().isEmpty());
        noticesHttp.answer(200,row);QTRY_COMPARE(noticesHttp.requests.size(),4);noticesHttp.answer(200,"[]");settle();QCOMPARE(position(),shown);
        if(width==1440){
            // The summary header lives above this viewport. Pick the height at
            // which enlarging that header used to shrink the viewport below 850.
            window.resize(width,window.height()+860-dashboard->height());settle();
            const auto atBoundary=position();
            for(int i=0;i<24;++i){QCoreApplication::processEvents();QCOMPARE(position(),atBoundary);}
        }
        notices.refresh();QTRY_COMPARE(noticesHttp.requests.size(),5);noticesHttp.answer(200,empty);
        QTRY_COMPARE(noticesHttp.requests.size(),6);settle();QVERIFY(notice->isVisible());
        noticesHttp.answer(503);settle();QVERIFY(notice->isHidden());

        // Lock removes the Upbit feed even when another public source is
        // refreshing. Holding space for the spinner must not keep its rows.
        OfficialNotice item;item.title="Synthetic Upbit delisting";item.url="https://upbit.com/service_center/notice?id=1";
        OfficialNoticeFeed upbit;upbit.source=OfficialNoticeSource::Upbit;upbit.available=true;upbit.notices={item};
        dashboard->setNoticeFeeds({upbit});QVERIFY(notice->isVisible());
        auto* info=notice->findChild<InfoButton*>();QVERIFY(info);info->click();
        QPointer<QWidget> dialog=info->findChild<QWidget*>("infoDialog");QVERIFY(dialog);
        OfficialNoticeFeed loading;loading.source=OfficialNoticeSource::Okx;loading.loading=true;
        dashboard->setNoticeFeeds({loading});QVERIFY(dialog.isNull());QVERIFY(notice->findChildren<InfoButton*>().isEmpty());
        dashboard->setNoticeFeeds({});QVERIFY(notice->isHidden());
    }
    void todaySummaryKeepsHeightWhilePrivateHistoryIsBeingRead(){
        // Production extracts the header above the main viewport. A history
        // read clears the old comparison and chart before publishing its result.
        QWidget host;auto* layout=new QVBoxLayout(&host);auto* view=new ExploreTodayView(&host);
        auto* header=view->takeSummaryHeader();layout->addWidget(header);layout->addWidget(view,1);
        PortfolioDayHistory history;auto& prior=history.previousDay;
        prior.available=true;prior.comparison.available=true;prior.comparison.complete=true;
        prior.comparison.openingValue="100";prior.comparison.closingValue="110";prior.comparison.difference="10";prior.comparison.fromMs=1700000000000;
        for(auto group:{PortfolioContributionClass::Coin,PortfolioContributionClass::Cash,PortfolioContributionClass::Unclassified})
            prior.groups.append({group,"10","15","5",1});
        history.points={{1700000000000,true,true,false,"100",{}},{1700000100000,true,true,true,"110",{}}};
        host.show();QVERIFY(QTest::qWaitForWindowExposed(&host));
        auto* card=host.findChild<QWidget*>("todayContributionPanel");auto* delta=host.findChild<QLabel*>("todayPrivateChange");QVERIFY(card&&delta);
        OfficialNoticeFeed feed;feed.source=OfficialNoticeSource::Okx;feed.available=true;
        for(int i=0;i<3;++i){
            OfficialNotice item;item.title=QString("Synthetic delisting %1").arg(i);
            item.url=QString("https://www.okx.com/help/delisting-%1").arg(i);item.publishedAtMs=1700000000000+i;
            feed.notices.append(item);
        }
        view->setNoticeFeeds({feed});
        for(int width:{800,1000,1300,1840}){
            host.resize(width,900);view->setWideLayout(width>=1680);
            const auto present=[&](bool populated){
                view->setPrivateSummary(true,"110","KRW",populated?history:PortfolioDayHistory{});
                for(int i=0;i<6;++i)QCoreApplication::processEvents();
            };
            present(true);QVERIFY(card->isVisible());QVERIFY(delta->isVisible());
            for(auto* text:card->findChildren<QLabel*>())if(text->isVisible()){
                const QRect bounds(text->mapTo(card,QPoint()),text->size());
                QVERIFY2(card->rect().contains(bounds),qPrintable(text->text()));
            }
            const auto top=view->mapTo(&host,QPoint()).y();
            present(false);QVERIFY(card->isHidden());QVERIFY(delta->isHidden());QVERIFY(delta->text().isEmpty());
            QCOMPARE(view->mapTo(&host,QPoint()).y(),top);
            present(true);QCOMPARE(view->mapTo(&host,QPoint()).y(),top);
        }
        view->clearPrivate();QVERIFY(card->isHidden());QVERIFY(delta->text().isEmpty());
    }
    void dev99DashboardRailShowsTenOrTwelveWholeRows_data(){
        QTest::addColumn<int>("windowWidth");QTest::addColumn<int>("windowHeight");QTest::addColumn<int>("visibleRows");
        QTest::newRow("window")<<1440<<900<<10;
        QTest::newRow("full")<<2000<<1100<<12;
    }
    void dev99DashboardRailShowsTenOrTwelveWholeRows(){
        QFETCH(int,windowWidth);QFETCH(int,windowHeight);QFETCH(int,visibleRows);
        QSettings defaults;const auto key=QStringLiteral("explore/watchlist/coin");const bool had=defaults.contains(key);const auto before=defaults.value(key);
        QStringList ids;for(int i=0;i<12;++i)ids<<QString("SYN%1").arg(i);defaults.setValue(key,ids);
        Vault vault(settings_.filePath(QString("dev99-rows-%1-no-vault").arg(windowWidth)));AccountModel accounts(&vault);
        FeedHttp catalogHttp,metricHttp;PublicMarketCatalog catalog(&catalogHttp);MarketMetrics metrics(&metricHttp);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);
        if(had)defaults.setValue(key,before);else defaults.remove(key);
        window.resize(windowWidth,windowHeight);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));QTest::qWait(50);
        auto* watch=window.findChild<ExploreWatchlist*>();auto* table=watch?watch->findChild<QTableWidget*>("watchlistTable"):nullptr;QVERIFY(table);
        QCOMPARE(table->rowCount(),12);const auto last=table->visualRect(table->model()->index(visibleRows-1,1));
        QVERIFY2(table->viewport()->rect().contains(last),qPrintable(QString("row %1 rect %2,%3 %4x%5 exceeds viewport %6x%7").arg(visibleRows).arg(last.x()).arg(last.y()).arg(last.width()).arg(last.height()).arg(table->viewport()->width()).arg(table->viewport()->height())));
        auto* summary=window.findChild<ExploreCoinSummary*>();auto* today=window.findChild<ExploreTodayView*>();auto* metric=window.findChild<QWidget*>("todayMetricPanel");QVERIFY(summary&&today&&metric);
        QCOMPARE(summary->mapTo(&window,QPoint()).y(),metric->mapTo(&window,QPoint()).y());
        QVERIFY(!vault.exists());
    }
    void dev99ExploreUsesAlignedWindowAndWideRails_data(){
        QTest::addColumn<int>("windowWidth");
        for(const int width:{1024,1440,1680,2000,2600})QTest::newRow(qPrintable(QString::number(width)))<<width;
    }
    void dev99ExploreUsesAlignedWindowAndWideRails(){
        QFETCH(int,windowWidth);
        Vault vault(settings_.filePath(QString("dev99-layout-%1-no-vault").arg(windowWidth)));AccountModel accounts(&vault);
        FeedHttp catalogHttp,metricHttp;PublicMarketCatalog catalog(&catalogHttp);MarketMetrics metrics(&metricHttp);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);
        window.resize(windowWidth,windowWidth>=1680?1100:900);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));QTest::qWait(50);
        auto* panels=window.findChild<QWidget*>("explorePanels");auto* content=window.findChild<QWidget*>("exploreContent");
        auto* brand=window.findChild<QLabel*>("brandLabel");auto* options=window.findChild<QPushButton*>("openOptions");
        auto* side=window.findChild<QWidget*>("exploreCommonSidebar");auto* today=window.findChild<ExploreTodayView*>();
        QVERIFY(panels&&content&&brand&&options&&side&&today);
        auto* outer=windowWidth<1100?window.findChild<QWidget*>("explorePanelsScroll"):panels;QVERIFY(outer);
        const auto left=outer->mapTo(&window,QPoint()).x(),right=left+outer->width();
        QCOMPARE(left,brand->mapTo(&window,QPoint()).x());
        QCOMPARE(right,options->mapTo(&window,QPoint()).x()+options->width());
        QCOMPARE(LocalWindow::ExploreMaximumWidth,2400);
        QVERIFY(content->width()<=2400);
        if(windowWidth<1100){
            auto* scroll=qobject_cast<QScrollArea*>(outer);QVERIFY(scroll);QCOMPARE(scroll->widget(),panels);
            QCOMPARE(panels->width(),scroll->viewport()->width());QCOMPARE(panels->mapTo(scroll->viewport(),QPoint()).x(),0);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);QVERIFY(scroll->verticalScrollBar()->maximum()>0);
            QVERIFY(side->mapTo(panels,QPoint()).y()>today->mapTo(panels,QPoint()).y());
            QCOMPARE(content->mapTo(&window,QPoint()).x(),left);QCOMPARE(content->width(),outer->width());
        }
        else {QCOMPARE(side->width(),windowWidth>=1680?380:360);QVERIFY(side->mapTo(panels,QPoint()).x()>today->mapTo(panels,QPoint()).x());}
        QVERIFY(!window.findChild<QPushButton*>("exploreDisplaySettings"));
        QCOMPARE(options->accessibleName(),QStringLiteral("옵션"));QCOMPARE(options->toolTip(),QStringLiteral("옵션 (⌘,)"));
        QVERIFY(!vault.exists());QCOMPARE(accounts.rowCount(),0);
    }
    void todayIsTheDefaultWithPrivatePlaceholderAndSharedWatchlist(){
        Vault vault(settings_.filePath("today-layout-no-vault")); AccountModel accounts(&vault);
        FeedHttp catalogHttp, metricHttp; PublicMarketCatalog catalog(&catalogHttp); MarketMetrics metrics(&metricHttp);
        PortfolioController::Factories factories; factories.prices=[]{return static_cast<TradingTransport*>(nullptr);}; PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts); window.setPortfolioController(&portfolio); window.setPublicMarketCatalog(&catalog,false); window.setMarketMetrics(&metrics);
        window.resize(1440,900); window.show(); QTest::qWait(40);
        auto* selector=window.findChild<QComboBox*>("exploreView"); QVERIFY(selector);
        QCOMPARE(selector->currentData().toString(),QString("flow"));
        QCOMPARE(selector->currentText(),QString("오늘"));
        auto* today=window.findChild<QWidget*>("todayDashboard"); QVERIFY(today); QVERIFY(today->isVisible());
        auto* locked=window.findChild<QLabel*>("todayPrivatePlaceholder"); QVERIFY(locked); QVERIFY(locked->isVisible());
        QVERIFY(locked->text().contains("금고"));
        auto* side=window.findChild<QWidget*>("exploreCommonSidebar"); QVERIFY(side); QVERIFY(side->isVisible());
        selector->setCurrentIndex(selector->findData("list")); QTest::qWait(20); QVERIFY(side->isVisible());
        selector->setCurrentIndex(selector->findData("map")); QTest::qWait(20); QVERIFY(!side->isVisible());
    }
    void todayPrivateValuesAndOpenInformationDisappearBeforeLockReturns(){
        Vault vault(settings_.filePath("today-private-vault"));QVERIFY(vault.create("Synthetic dashboard privacy password 2026!"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        FeedHttp source,catalogHttp;MarketMetrics metrics(&source);PublicMarketCatalog catalog(&catalogHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.resize(1440,900);window.show();
        auto* today=window.findChild<ExploreTodayView*>();auto* summary=window.findChild<ExploreCoinSummary*>();QVERIFY(today&&summary);
        PortfolioDayHistory history;history.comparison.available=true;history.comparison.difference="1234567";
        today->setPrivateSummary(true,"987,654,321","KRW",history);summary->setPrivateHolding(true,"SYNTHETIC_PRIVATE_HOLDING 111222333");
        const auto height=window.findChild<QWidget*>("todayPrivatePanel")->height();
        bool checked=false;connect(&vault,&Vault::aboutToLock,&window,[&]{checked=true;for(auto* l:window.findChildren<QLabel*>()){QVERIFY(!l->text().contains("987,654,321"));QVERIFY(!l->text().contains("111222333"));QVERIFY(!l->toolTip().contains("SYNTHETIC_PRIVATE"));}});
        vault.lock();QVERIFY(checked);QVERIFY(window.findChild<QLabel*>("todayPrivateTotal")->text().isEmpty());QCOMPARE(window.findChild<QWidget*>("todayPrivatePanel")->height(),height);
    }
    void summaryRemovesRetiredMetricCellsAndKeepsMarketSupport(){
        ExploreCoinSummary summary;summary.resize(400,440);summary.show();
        ExploreMetricCardData gap;gap.title="김프";gap.value="계산할 수 없어요";gap.changeText="업비트 원화 시장에 없는 코인이에요";
        ExploreMetricCardData funding;funding.title="공시 펀딩비";funding.value="0.01";funding.unit="%";funding.freshnessText="지연";
        summary.setMetrics(gap,{},funding,{});
        for(const auto& key:QStringList{"gap","coinbase","funding"})QVERIFY(!summary.findChild<QLabel*>("exploreSummaryTitle_"+key));
        for(const auto& key:QStringList{"gap","coinbase","funding"})QVERIFY(!summary.findChild<QLabel*>("exploreSummaryFreshness_"+key));
        for(const auto& key:QStringList{"gap","coinbase","funding"})QVERIFY(!summary.findChild<InfoButton*>("exploreSummaryInfo_"+key));
        auto* support=summary.findChild<QLabel*>("exploreSummaryMarketSupport");QVERIFY(support);QVERIFY(support->text().isEmpty());
    }
    void initTestCase(){QVERIFY(settings_.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic Flow");QCoreApplication::setApplicationName("Synthetic Flow Page");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());QVERIFY(KeefiFonts::registerBundledFonts());}
    void cleanup(){KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(ThemeMode::System);}
    void currentSyntheticFuturesPositionExpandsWithoutLeakingAmounts(){
        Vault vault(settings_.filePath("synthetic-flow-position-vault"));QVERIFY(vault.create("Synthetic flow position password 2026!"));keefi::test::ReadFixture fixture("binance-usdm");fixture.account.alias="SYNTHETIC_FLOW_PRIVATE_ALIAS";QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));AccountModel accounts(&vault);keefi::test::ProviderReadTransport accountHttp;accountHttp.provider="binance-usdm";
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &accountHttp;};factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        const auto now=QDateTime::currentMSecsSinceEpoch();portfolio.setPublicPrices({{"USDT","KRW","1400",{"Upbit","https://api.upbit.com/v1/ticker","public:upbit:KRW-USDT",now,now,true}}},{"KRW-USDT"});
        FeedHttp metricHttp,calendarHttp,catalogHttp;MarketMetrics metrics(&metricHttp);OfficialCalendar calendarModel(&calendarHttp);PublicMarketCatalog catalog(&catalogHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setOfficialCalendar(&calendarModel);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",12000);portfolio.setAutomaticRefresh(false);
        auto* toggle=window.findChild<QPushButton*>("marketMetricsDerivativesToggle");QVERIFY(toggle);QTRY_VERIFY(toggle->isChecked());
        auto* context=window.findChild<QWidget*>("marketFlowContext");QVERIFY(context);for(auto* label:context->findChildren<QLabel*>())QVERIFY(!label->text().contains("SYNTHETIC_FLOW_PRIVATE_ALIAS"));
        vault.lock();QVERIFY(!toggle->isChecked());QVERIFY(window.findChild<QLabel*>("marketFlowCalendarPrivateContext")->text().isEmpty());
    }
    void optionalSyntheticFlowRenders(){
        const auto out=qEnvironmentVariable("KEEFI_P4_RENDER_DIR");if(out.isEmpty())QSKIP("Optional synthetic P4 market-flow PNG artifacts; set KEEFI_P4_RENDER_DIR to export.");QVERIFY(QDir().mkpath(out));
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
            QSettings profile(settings_.filePath(mode==ThemeMode::Dark?"capture-dark.ini":"capture-light.ini"),QSettings::IniFormat);Vault vault(settings_.filePath(mode==ThemeMode::Dark?"capture-dark-vault":"capture-light-vault"));QVERIFY(vault.create("Synthetic flow capture password 2026!"));AccountModel accounts(&vault);PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
            DashboardBoardHttp boardHttp;PublicEquityBoard board(&boardHttp);FeedHttp candleHttp,metadataHttp;MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
            FeedHttp metricHttp,calendarHttp,noticeHttp,treasuryHttp,catalogHttp,earningsHttp;EarningsCalendar earnings(&earningsHttp);MarketMetrics metrics(&metricHttp);OfficialCalendar calendarModel(&calendarHttp);OfficialNotices notices(&noticeHttp);MarketOverview overview(&treasuryHttp);PublicMarketCatalog catalog(&catalogHttp);
            auto a=std::make_unique<FlowSocket>(),b=std::make_unique<FlowSocket>();auto* up=a.get();auto* bn=b.get();PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
            LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalog,false);window.setPublicEquityBoard(&board);window.setChartInstrumentTransport(&metadataHttp);window.setMarketCandles(&candles);window.setMarketMetrics(&metrics,nullptr,&upbit,&binance);window.setMarketOverview(&overview);window.setOfficialCalendar(&calendarModel);window.setOfficialNotices(&notices);window.setEarningsCalendar(&earnings);
            KeefiTheme::instance()->setMode(mode);window.resize(1440,900);window.show();auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* flow=window.findChild<QWidget*>("marketFlowPage");auto* stack=window.findChild<QStackedWidget*>("exploreStack");QVERIFY(pages&&flow&&stack);for(int i=0;i<pages->count();++i)if(pages->widget(i)->isAncestorOf(stack))pages->setCurrentIndex(i);auto* selector=window.findChild<QComboBox*>("exploreView");QVERIFY(selector);selector->setCurrentIndex(selector->findData("flow"));QCOMPARE(stack->currentWidget(),flow);
            QTRY_COMPARE(metricHttp.requests.size(),1);QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);up->ready();bn->ready();metricHttp.answer(200,R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})");QTRY_COMPARE(metricHttp.requests.size(),2);metricHttp.answer(200,"[]");QTRY_COMPARE(metricHttp.requests.size(),3);const auto now=QDateTime::currentMSecsSinceEpoch()-20;
            metricHttp.answer(200,QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0004"},{"interestRate","0.0001"},{"markPrice","100000"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}}).toJson());QTRY_COMPARE(metricHttp.requests.size(),4);metricHttp.answer(200,QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"openInterest","12345.67"},{"time",QString::number(now)}}).toJson());
            const auto tick=[&](const QString& code,double price){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",price},{"prev_closing_price",price},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};up->tick(tick("KRW-BTC",144200000));up->tick(tick("KRW-USDT",1400));bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
            QTRY_COMPARE(calendarHttp.requests.size(),1);calendarHttp.answer(200,calendar());QTRY_COMPARE(calendarHttp.requests.size(),2);calendarHttp.answer(403);QTRY_COMPARE(calendarHttp.requests.size(),3);calendarHttp.answer(403);QTRY_COMPARE(calendarHttp.requests.size(),4);calendarHttp.answer(403);
            QTRY_COMPARE(earningsHttp.requests.size(),1);earningsHttp.answer(200,syntheticKindWeek(QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Seoul")).date()));
            for(int n=2;!earnings.feed(EarningsSource::Nasdaq).available&&n<12;++n){QTRY_COMPARE(earningsHttp.requests.size(),n);const auto day=QDate::fromString(QUrlQuery(earningsHttp.requests.last().url).queryItemValue("date"),Qt::ISODate);earningsHttp.answer(200,syntheticNasdaqDay(day,n==2));}
            QTRY_VERIFY(earnings.feed(EarningsSource::Nasdaq).available);window.findChild<EarningsCalendarView*>()->setInterest({"000660","005930"},{});
            QTRY_COMPARE(noticeHttp.requests.size(),1);noticeHttp.answer(200,QJsonDocument(QJsonObject{{"code","0"},{"msg",""},{"data",QJsonArray{QJsonObject{{"details",QJsonArray{QJsonObject{{"annType","announcements-delisting"},{"title","합성 네트워크 점검 · 입출금 일시 중단"},{"url","https://www.okx.com/help/synthetic-only"},{"pTime",QString::number(now)},{"businessPTime",QString::number(now)}}}},{"totalPage","1"}}}}}).toJson());QTRY_COMPARE(noticeHttp.requests.size(),2);noticeHttp.answer(403);
            QTRY_COMPARE(treasuryHttp.requests.size(),1);const auto date=QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("America/New_York")).date().toString(Qt::ISODate).toUtf8();
            treasuryHttp.answer(200,"<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:d=\"http://schemas.microsoft.com/ado/2007/08/dataservices\" xmlns:m=\"http://schemas.microsoft.com/ado/2007/08/dataservices/metadata\"><entry><content type=\"application/xml\"><m:properties><d:NEW_DATE>"+date+"T00:00:00</d:NEW_DATE><d:BC_10YEAR>4.25</d:BC_10YEAR></m:properties></content></entry></feed>");
            auto* view=window.findChild<MarketMetricsView*>();auto* calendarView=window.findChild<OfficialCalendarView*>();QVERIFY(view&&calendarView);QTRY_COMPARE(view->findChild<ExploreMetricCard*>("marketMetricsGapCard")->data().value,QString("3.00"));QCOMPARE(view->findChild<ExploreMetricCard*>("marketMetricsFundingCard")->data().value,QString("43.8"));
            catalog.refresh();QTRY_COMPARE(catalogHttp.requests.size(),1);catalogHttp.answer(200,R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin","market_event":{"warning":false,"caution":{}}},{"market":"KRW-USDT","korean_name":"테더","english_name":"Tether","market_event":{"warning":false,"caution":{}}}])");
            QTRY_COMPARE(catalogHttp.requests.size(),2);catalogHttp.answer(200,QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"trade_price","144200000"},{"prev_closing_price","140000000"},{"signed_change_rate","0.03"},{"timestamp",QString::number(now)},{"acc_trade_price_24h","123450000000"}},QJsonObject{{"market","KRW-USDT"},{"trade_price","1400"},{"prev_closing_price","1400"},{"signed_change_rate","0"},{"timestamp",QString::number(now)},{"acc_trade_price_24h","123450000"}}}).toJson());
            auto* watch=window.findChild<ExploreWatchlist*>();QVERIFY(watch);for(const auto& id:{"BTC","ETH","SOL","XRP"})if(!watch->contains("coin",id))watch->toggle("coin",id);board.refresh();QTRY_VERIFY(!board.items("us").isEmpty());
            const QSignalBlocker deterministicContext(window.findChild<QTimer*>("marketFlowContextTimer"));
            const QString theme=mode==ThemeMode::Dark?"dark":"light";const auto save=[&](const QString& name){for(const auto& size:{QSize(1440,900),QSize(1920,1080),QSize(1280,800)}){window.resize(size);for(const auto& route:{QString("flow"),QString("list"),QString("map")}){selector->setCurrentIndex(selector->findData(route));QTest::qWait(120);QCoreApplication::processEvents();QVERIFY(window.grab().save(out+"/"+route+"-"+QString::number(size.width())+"-"+name+"-"+theme+".png"));}}selector->setCurrentIndex(selector->findData("flow"));};
            const auto comparisonClose=now/MarketFlowPolicy::HourMs*MarketFlowPolicy::HourMs-MarketFlowPolicy::DayMs;MarketFlowHistoricalGap previous{true,"1",comparisonClose,now,"BTC",{}};
            // Synthetic hourly line for the wide gap chart: 25 completed closes, one window missing.
            for(int hour=0;hour<=24;++hour)if(hour!=17)previous.series.append({comparisonClose+hour*MarketFlowPolicy::HourMs,QString::number(1.0+2.0*hour/24.0+0.35*std::sin(hour*0.9),'f',6)});
            view->setPreviousGap(previous);QVERIFY(view->findChild<ExploreMetricCard*>("marketMetricsGapCard")->historyVisible());view->setPrivateContext(true,true);calendarView->setPrivateContext(true,"60.0%","10.0%");save("unlocked-expanded-delta-present");
            vault.lock();QVERIFY(!view->findChild<QPushButton*>("marketMetricsDerivativesToggle")->isChecked());QVERIFY(calendarView->findChild<QLabel*>("marketFlowCalendarPrivateContext")->text().isEmpty());save("locked-collapsed-delta-present");
            view->setPreviousGap({});calendarModel.refresh();QTRY_COMPARE(calendarHttp.requests.size(),5);calendarHttp.answer(403);QTRY_COMPARE(calendarHttp.requests.size(),6);calendarHttp.answer(403);QTRY_COMPARE(calendarHttp.requests.size(),7);calendarHttp.answer(403);QTRY_COMPARE(calendarHttp.requests.size(),8);calendarHttp.answer(403);notices.refresh();QTRY_COMPARE(noticeHttp.requests.size(),3);noticeHttp.answer(403);QTRY_COMPARE(noticeHttp.requests.size(),4);noticeHttp.answer(403);save("locked-collapsed-no-delta-empty");
            for(auto* widget:flow->findChildren<QWidget*>())if(widget->isVisibleTo(flow)){QString text;if(auto* label=qobject_cast<QLabel*>(widget)){QTextDocument doc;doc.setHtml(label->text());text=doc.toPlainText();}QVERIFY2(!QRegularExpression("\\[[0-9]+\\]|[a-z][a-z0-9+.-]*://|(?:stock|coin):|SYNTHETIC_FLOW_PRIVATE_ALIAS").match(text+widget->toolTip()+widget->accessibleName()).hasMatch(),qPrintable(widget->objectName()+text));}
        }
    }
    void flowAlertChoicePersistsOnlyFourPublicFlags(){
        QSettings profile(settings_.filePath("synthetic-flow-alerts.ini"),QSettings::IniFormat);Vault vault(settings_.filePath("synthetic-flow-alerts-no-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        FeedHttp metricHttp,candleHttp,catalogHttp,metadata;MarketMetrics metrics(&metricHttp);PublicMarketCatalog catalog(&catalogHttp);MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::make_unique<FlowSocket>()),binance(PublicQuoteStream::Provider::Binance,std::make_unique<FlowSocket>());FlowSink sink;ChartAlertEngine engine(&sink);
        LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics,nullptr,&upbit,&binance);window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,nullptr,&profile);
        auto* box=window.findChild<QCheckBox*>("marketMetricsGapAlert");QVERIFY(box);QVERIFY(!box->isChecked());const auto original=engine.publicState();box->setChecked(true);
        QVERIFY(engine.marketMetricEnabled(MarketFlowMetric::KoreaGap));QCOMPARE(profile.value("market/flow-alerts/v1").toByteArray(),engine.marketMetricState());QCOMPARE(engine.publicState(),original);QCOMPARE(sink.authorizations,0);QVERIFY(sink.posts.isEmpty());
        const auto state=QJsonDocument::fromJson(engine.marketMetricState()).object();QCOMPARE(state.size(),5);QVERIFY(state.value("koreaGap").toBool());QCOMPARE(accounts.rowCount(),0);QVERIFY(!vault.isUnlocked());
    }
    void vaultBoundaryClearsFlowContextBeforeLockReturns(){
        Vault vault(settings_.filePath("synthetic-flow-lock-vault"));QVERIFY(vault.create("Synthetic flow boundary password 2026!"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);FeedHttp metricHttp,calendarHttp,catalogHttp;MarketMetrics metrics(&metricHttp);OfficialCalendar model(&calendarHttp);PublicMarketCatalog catalog(&catalogHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setOfficialCalendar(&model);
        auto* view=window.findChild<OfficialCalendarView*>();QVERIFY(view);view->setPrivateContext(true,"60%","10%");auto* label=view->findChild<QLabel*>("marketFlowCalendarPrivateContext");QVERIFY(label);QVERIFY(!label->text().isEmpty());
        bool boundary=false;connect(&vault,&Vault::aboutToLock,&window,[&]{boundary=true;QVERIFY(label->text().isEmpty());QVERIFY(label->accessibleName().isEmpty());});vault.lock();QVERIFY(boundary);QVERIFY(label->text().isEmpty());QVERIFY(!vault.isUnlocked());
    }
    void flowNoticeListMergesAtMostFiveRelevantPublicItems(){
        FeedHttp http;OfficialNotices model(&http,nullptr,[]{return Now;});OfficialNoticesView view(&model);view.setMarketFlowMode(true);view.resize(900,700);view.show();
        QJsonArray rows;for(int i=0;i<6;++i)rows.append(QJsonObject{{"annType","announcements-delisting"},{"title",QString("Synthetic suspension %1").arg(i)},{"url",QString("https://www.okx.com/help/synthetic-%1").arg(i)},{"pTime",QString::number(Now-i*60000)},{"businessPTime",QString::number(Now-i*60000)}});
        rows.append(QJsonObject{{"annType","campaign"},{"title","Synthetic unrelated promotion"},{"url","https://www.okx.com/help/other"},{"pTime",QString::number(Now)},{"businessPTime",QString::number(Now)}});
        QTRY_COMPARE(http.requests.size(),1);http.answer(200,QJsonDocument(QJsonObject{{"code","0"},{"msg",""},{"data",QJsonArray{QJsonObject{{"details",rows},{"totalPage","1"}}}}}).toJson());
        QTRY_COMPARE(http.requests.size(),2);http.answer(200,QJsonDocument(QJsonArray{QJsonObject{{"categories",QJsonArray{"거래유의"}},{"title","합성 거래유의 지정"},{"pc_url","https://feed.bithumb.com/notice/12345"},{"published_at","2026-01-01 09:00:00"},{"modified_at","2026-01-01 09:00:00"}}}).toJson());
        QCOMPARE(model.feed(OfficialNoticeSource::Okx).notices.size(),7);QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.size(),1);
        QCOMPARE(view.findChildren<QWidget*>("officialNoticeRow").size(),5);
        for(auto* row:view.findChildren<QWidget*>("officialNoticeRow")){const auto text=row->findChild<QLabel*>("officialNoticeTitle")->accessibleName();QVERIFY(!text.contains("promotion"));QVERIFY(!text.contains("합성 거래유의"));QVERIFY(text.contains("OKX"));}
        QVERIFY(view.findChild<QLabel*>("officialNoticesStatus")->isHidden());QCOMPARE(http.requests.size(),2);
        QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.first().publishedAtMs,0);QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.first().publishedText,QString("2026-01-01 09:00:00"));
    }
    void flowCalendarPrivateRatioClearsSynchronously(){
        OfficialCalendarView view(nullptr);view.setMarketFlowMode(true);view.setPrivateContext(true,"60%","10%");
        auto* context=view.findChild<QLabel*>("marketFlowCalendarPrivateContext");QVERIFY(context);QCOMPARE(context->text(),QStringLiteral("내 자산 비중: 코인 60% · 미국 주식 10%"));
        view.setPrivateContext(false,"60%","10%");QVERIFY(context->text().isEmpty());QVERIFY(context->accessibleName().isEmpty());QVERIFY(context->toolTip().isEmpty());QVERIFY(context->isHidden());QVERIFY(view.publicSummary().isEmpty());
        view.setPrivateContext(true,"101%","10%");QVERIFY(context->text().isEmpty());
    }
    void fixedFlowOrderUsesOnlyPublicSections(){
        Vault vault(settings_.filePath("synthetic-nonexistent-vault"));AccountModel accounts(&vault);
        FeedHttp a,b,c,d,e,f;MarketMetrics metrics(&a);OfficialCalendar calendarModel(&b);OfficialNotices notices(&c);MarketOverview overview(&d);PublicMarketCatalog catalog(&e);EarningsCalendar earnings(&f);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setMarketOverview(&overview);window.setOfficialNotices(&notices);window.setOfficialCalendar(&calendarModel);window.setEarningsCalendar(&earnings);
        auto* flow=window.findChild<QWidget*>("marketFlowPage");QVERIFY(flow);auto* context=window.findChild<QWidget*>("marketFlowContext");QVERIFY(context);
        auto* today=window.findChild<ExploreTodayView*>();QVERIFY(today);
        QVERIFY(today->calendar());QVERIFY(window.findChild<QWidget*>("todayMetricPanel"));
        QCOMPARE(accounts.rowCount(),0);QVERIFY(!vault.isUnlocked());
        window.resize(1920,1080);window.show();
        auto* content=window.findChild<QWidget*>("exploreContent");
        auto* brand=window.findChild<QLabel*>("brandLabel");auto* options=window.findChild<QPushButton*>("openOptions");QVERIFY(brand&&options);
        QTRY_COMPARE(content->mapTo(&window,QPoint()).x(),brand->mapTo(&window,QPoint()).x());
        QCOMPARE(content->mapTo(&window,QPoint()).x()+content->width(),options->mapTo(&window,QPoint()).x()+options->width());QVERIFY(content->width()<=2400);
        QVERIFY(!context->isVisible());
        auto* side=window.findChild<QWidget*>("exploreCommonSidebar");QVERIFY(side);QTRY_COMPARE(side->width(),380);
        QTRY_VERIFY(side->mapTo(content,QPoint()).x()>today->mapTo(content,QPoint()).x());
        for(const auto& size:{QSize(1440,900),QSize(1920,1080)}){
            window.resize(size);QTest::qWait(80);auto* scroll=qobject_cast<QScrollArea*>(flow);QVERIFY(scroll);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);QCOMPARE(scroll->verticalScrollBar()->maximum(),0);
        }

    }
    void rankingKeepsThreeMarketsVisibleThroughWindowResizeSequence(){
        Vault vault(settings_.filePath("ranking-resize-no-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        FeedHttp catalogHttp,metricHttp;DashboardBoardHttp boardHttp;
        PublicMarketCatalog catalog(&catalogHttp);PublicEquityBoard board(&boardHttp);MarketMetrics metrics(&metricHttp);
        PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::make_unique<FlowSocket>()),binance(PublicQuoteStream::Provider::Binance,std::make_unique<FlowSocket>());
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,false);
        window.setPublicEquityBoard(&board);window.setMarketMetrics(&metrics,nullptr,&upbit,&binance);window.resize(1440,900);window.show();board.refresh();
        QTRY_COMPARE(board.items("coin").size(),20);QTRY_COMPARE(board.items("kr").size(),20);QTRY_COMPARE(board.items("us").size(),20);
        auto* views=window.findChild<QComboBox*>("exploreView");QVERIFY(views);
        auto* inner=window.findChild<QScrollArea*>("spotlightScroll");auto* outer=window.findChild<QScrollArea*>("spotlightListScroll");QVERIFY(inner&&outer);
        QList<SpotlightCard*> cards;for(const auto* market:{"coin","kr","us"}){auto* card=window.findChild<SpotlightCard*>(QString("spotlightCard_")+market);QVERIFY(card);cards.append(card);}
        // Exercise the real view-switch and resize sequence used by the native
        // captures, then return to 1440 to catch retained wide row counts.
        for(const auto& size:{QSize(1440,900),QSize(1920,1080),QSize(1280,800),QSize(1440,900)}){
            views->setCurrentIndex(views->findData("flow"));window.resize(size);
            views->setCurrentIndex(views->findData("list"));QTest::qWait(60);
            QTRY_COMPARE(inner->horizontalScrollBar()->maximum(),0);QTRY_COMPARE(outer->horizontalScrollBar()->maximum(),0);
            for(auto* card:cards){
                QTRY_VERIFY(card->isVisibleTo(&window));
                if(size.width()<1700)QTRY_VERIFY(card->rowsPerPage()>=3&&card->rowsPerPage()<=5);
                else QTRY_VERIFY(card->rowsPerPage()>5&&card->rowsPerPage()<=20);
            }
            if(size.width()>=1440){
                QTRY_COMPARE(inner->verticalScrollBar()->maximum(),0);QTRY_COMPARE(outer->verticalScrollBar()->maximum(),0);
                for(auto* card:cards){
                    const QRect rect(card->mapTo(inner->viewport(),QPoint()),card->size());
                    QVERIFY2(inner->viewport()->rect().contains(rect),qPrintable(QString("%1 at %2px extends beyond the visible ranking viewport").arg(card->key()).arg(size.width())));
                }
            }
            if(size.width()==1440)for(auto* card:cards)QVERIFY(card->rowsPerPage()<=4);
        }
        QVERIFY(!vault.exists());QVERIFY(vault.accounts().isEmpty());
    }
    void weekOnlyPresentationKeepsFullOfficialSchedule(){
        FeedHttp http;OfficialCalendar model(&http,nullptr,[]{return Now;});OfficialCalendarView view(&model);view.setMarketFlowMode(true);view.resize(720,700);view.show();
        QTRY_COMPARE_WITH_TIMEOUT(http.requests.size(),1,2000);http.answer(200,calendar());QTRY_COMPARE_WITH_TIMEOUT(http.requests.size(),2,2000);http.answer(403);
        const auto original=model.feed(OfficialCalendarSource::Bls);QVERIFY(original.available);QCOMPARE(original.events.size(),3);
        QCOMPARE(view.findChildren<QWidget*>("officialCalendarEventRow").size(),2);
        QCOMPARE(view.findChild<QLabel*>("officialCalendarTitle")->text(),QStringLiteral("이번 주 경제 일정"));
        QVERIFY(view.publicSummary().contains("CPI")||view.publicSummary().contains("Consumer Price Index"));QVERIFY(!view.publicSummary().contains("SYNTHETIC_CPI"));
        const auto after=model.feed(OfficialCalendarSource::Bls);QCOMPARE(after.events.size(),3);QCOMPARE(after.events.last().uid,original.events.last().uid);QCOMPARE(after.events.last().startsAtMs,original.events.last().startsAtMs);QCOMPARE(after.receivedAtMs,original.receivedAtMs);QCOMPARE(http.requests.size(),2);
    }
    void flowSourcesLoadInTheBackgroundWithProgress(){
        // The user opens 시장 흐름 later; every source starts as soon as the window is open.
        Vault vault(settings_.filePath("synthetic-background-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        FeedHttp metricHttp,calendarHttp,noticeHttp,treasuryHttp,catalogHttp,earningsHttp;EarningsCalendar earnings(&earningsHttp);MarketMetrics metrics(&metricHttp);OfficialCalendar calendarModel(&calendarHttp);OfficialNotices notices(&noticeHttp);MarketOverview overview(&treasuryHttp);PublicMarketCatalog catalog(&catalogHttp);
        auto a=std::make_unique<FlowSocket>(),b=std::make_unique<FlowSocket>();auto* up=a.get();auto* bn=b.get();PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
        FeedHttp candleHttp,metadata;MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics,nullptr,&upbit,&binance);
        window.setMarketOverview(&overview);window.setOfficialCalendar(&calendarModel);window.setOfficialNotices(&notices);window.setEarningsCalendar(&earnings);window.setMarketCandles(&candles);
        QTest::qWait(1200);QCOMPARE(metricHttp.requests.size()+calendarHttp.requests.size()+noticeHttp.requests.size()+treasuryHttp.requests.size()+earningsHttp.requests.size()+metadata.requests.size(),0);QCOMPARE(up->opens,0);
        window.resize(1440,1000);window.show();auto* selector=window.findChild<QComboBox*>("exploreView");QVERIFY(selector);
        auto* tabs=window.findChild<QWidget*>("exploreViewTabs");auto* first=window.findChild<QWidget*>("todayEmptyStrip");QVERIFY(tabs&&first);
        selector->setCurrentIndex(selector->findData("flow"));QCoreApplication::processEvents();QVERIFY(first->isVisibleTo(&window));
        const auto tabsPosition=tabs->mapTo(&window,QPoint()),firstPosition=first->mapTo(&window,QPoint());
        selector->setCurrentIndex(selector->findData("list"));
        auto* flow=window.findChild<QWidget*>("marketFlowPage");auto* progress=window.findChild<QWidget*>("marketFlowProgress");auto* text=window.findChild<QProgressBar*>("marketFlowProgressBar");QVERIFY(flow&&progress&&text);
        QTRY_COMPARE(metricHttp.requests.size(),1);QTRY_COMPARE(calendarHttp.requests.size(),1);QTRY_COMPARE(earningsHttp.requests.size(),1);QTRY_COMPARE(noticeHttp.requests.size(),1);QTRY_COMPARE(treasuryHttp.requests.size(),1);
        QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);QVERIFY(!flow->isVisible());
        QVERIFY(progress->isHidden());QVERIFY(text->value()<text->maximum());
        QVERIFY2(text->accessibleName().startsWith(QStringLiteral("시장 정보 받는 중 · "))&&text->accessibleName().endsWith("/8"),qPrintable(text->accessibleName()));
        // Answers and failures both count: a source that failed is no longer loading.
        up->ready();bn->ready();const auto now=QDateTime::currentMSecsSinceEpoch()-20;
        const auto tick=[&](const QString& code,double price){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",price},{"prev_closing_price",price},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};
        up->tick(tick("KRW-BTC",144200000));up->tick(tick("KRW-USDT",1400));bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
        metricHttp.answer(403);
        for(int n=1;n<=4;++n){QTRY_COMPARE(calendarHttp.requests.size(),n);calendarHttp.answer(403);}
        QTRY_COMPARE(earningsHttp.requests.size(),1);earningsHttp.answer(403);QTRY_COMPARE(earningsHttp.requests.size(),2);earningsHttp.answer(403);
        noticeHttp.answer(403);QTRY_COMPARE(noticeHttp.requests.size(),2);noticeHttp.answer(403);treasuryHttp.answer(403);
        QTRY_COMPARE_WITH_TIMEOUT(text->value(),text->maximum(),4000);QVERIFY(progress->isHidden());QVERIFY(!flow->isVisible());
        // Opening the page later paints what already arrived, without another round of loading.
        const auto sent=metricHttp.requests.size();selector->setCurrentIndex(selector->findData("flow"));QTRY_VERIFY(flow->isVisible());
        auto* gap=window.findChild<ExploreMetricCard*>("marketMetricsGapCard");QVERIFY(gap);QTRY_COMPARE(gap->data().value,QString("3.00"));
        QCOMPARE(window.findChild<ExploreMetricCard*>("marketMetricsFundingCard")->data().value,usabilityText(UsabilityText::FlowNoData));
        QTest::qWait(1200);QVERIFY(progress->isHidden());QCOMPARE(metricHttp.requests.size(),sent);
        QCOMPARE(tabs->mapTo(&window,QPoint()),tabsPosition);QCOMPARE(first->mapTo(&window,QPoint()),firstPosition);
        // Once the Upbit KRW and Binance spot lists are in, a coin missing from either ends the 김프 wait with that reason.
        QTRY_COMPARE(metadata.requests.size(),1);QCOMPARE(metadata.requests.first().url.path(),QString("/api/v3/exchangeInfo"));
        metadata.answer(200,QJsonDocument(QJsonObject{{"symbols",QJsonArray{QJsonObject{{"symbol","BTCUSDT"},{"baseAsset","BTC"},{"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}}}}}).toJson());
        catalog.refresh();QTRY_COMPARE(catalogHttp.requests.size(),1);
        catalogHttp.answer(200,R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin","market_event":{"warning":false,"caution":{}}},{"market":"KRW-ETH","korean_name":"이더리움","english_name":"Ethereum","market_event":{"warning":false,"caution":{}}}])");
        QTRY_COMPARE(catalogHttp.requests.size(),2);catalogHttp.answer(200,QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"trade_price","144200000"},{"prev_closing_price","140000000"},{"signed_change_rate","0.03"},{"timestamp",QString::number(now)},{"acc_trade_price_24h","12345"}},QJsonObject{{"market","KRW-ETH"},{"trade_price","5000000"},{"prev_closing_price","5000000"},{"signed_change_rate","0"},{"timestamp",QString::number(now)},{"acc_trade_price_24h","12345"}}}).toJson());
        QTRY_VERIFY(!catalog.rows().isEmpty());auto* input=window.findChild<ExploreCoinSummary*>();QVERIFY(input);
        emit input->coinSelected("BTW");
        QTRY_COMPARE(gap->data().changeText,QStringLiteral("업비트 원화마켓 미지원"));QCOMPARE(gap->data().value,usabilityText(UsabilityText::FlowGapUnavailable));
        emit input->coinSelected("ETH");QTRY_COMPARE(gap->data().changeText,usabilityText(UsabilityText::FlowGapNotOnBinance));
        emit input->coinSelected("BTC");QTRY_VERIFY(gap->data().publicInformation.contains(usabilityText(UsabilityText::FlowGapProgress).arg(0).arg(3)));QVERIFY(gap->data().changeText.isEmpty());
    }
    void wrappedScheduleTitlesKeepTheirHeight(){
        // Replacing the size policy dropped height-for-width, so a two-line title got one line of height.
        FeedHttp calendarHttp,earningsHttp;OfficialCalendar calendarModel(&calendarHttp,nullptr,[]{return Now;});EarningsCalendar earnings(&earningsHttp);
        OfficialCalendarView calendarView(&calendarModel);calendarView.setMarketFlowMode(true);calendarView.resize(250,700);calendarView.show();
        QTRY_COMPARE(calendarHttp.requests.size(),1);calendarHttp.answer(200,calendar());
        EarningsCalendarView earningsView(&earnings);earningsView.resize(250,900);earningsView.show();QTRY_COMPARE(earningsHttp.requests.size(),1);earningsHttp.answer(403);
        for(int n=2;!earnings.feed(EarningsSource::Nasdaq).available&&n<12;++n){QTRY_COMPARE(earningsHttp.requests.size(),n);const auto day=QDate::fromString(QUrlQuery(earningsHttp.requests.last().url).queryItemValue("date"),Qt::ISODate);earningsHttp.answer(200,syntheticNasdaqDay(day,n==2));}
        QTRY_VERIFY(!earningsView.findChildren<QLabel*>("earningsEventTitle").isEmpty());QTRY_VERIFY(!calendarView.findChildren<QLabel*>("officialCalendarEventTitle").isEmpty());
        QTest::qWait(50);QCoreApplication::processEvents();
        QList<QLabel*> titles=calendarView.findChildren<QLabel*>("officialCalendarEventTitle");titles+=earningsView.findChildren<QLabel*>("earningsEventTitle");
        bool wrapped=false;
        for(auto* title:titles){
            QVERIFY(title->sizePolicy().hasHeightForWidth());const auto needed=title->heightForWidth(title->width());
            wrapped|=needed>QFontMetrics(title->font()).height()*3/2;
            QVERIFY2(title->height()>=needed,qPrintable(QString("%1: %2 < %3").arg(title->text()).arg(title->height()).arg(needed)));
        }
        QVERIFY2(wrapped,"At least one title must wrap at this width for the check to mean anything");
    }
};
QTEST_MAIN(MarketFlowPageTest)
#include "tst_market_flow_page.moc"
