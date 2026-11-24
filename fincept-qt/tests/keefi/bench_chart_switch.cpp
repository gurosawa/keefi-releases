// Opt-in chart switching probe: synthetic public OHLCV, no account/profile/network.
#include "keefi/ChartWorkspace.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/TradingTransport.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrlQuery>
#include <QTimeZone>
#include <QTest>
#include <functional>
using namespace keefi;
namespace {
QString code(int n){return QString("KRW-S%1").arg(n,3,10,QChar('0'));}
QString identity(int n){return "coin:upbit:"+code(n);}
bool waitFor(const std::function<bool()>& ready,int timeout=5000){
    QElapsedTimer timer;timer.start();
    while(!ready()&&timer.elapsed()<timeout)QTest::qWait(1);
    return ready();
}
HttpResult seed(const QString& market,qint64 now,qint64 interval){
    QJsonArray rows;const auto end=now/interval*interval;
    for(int i=0;i<200;++i){
        const auto at=end-(200-i)*interval;const auto price=1000+i%20;
        rows.append(QJsonObject{{"market",market},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,QTimeZone::utc()).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(at+1000)},{"unit",QString::number(interval/60000)},
            {"opening_price",QString::number(price)},{"high_price",QString::number(price+2)},
            {"low_price",QString::number(price-2)},{"trade_price",QString::number(price+1)},
            {"candle_acc_trade_volume","2"}});
    }
    HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);return result;
}
struct Measurement {
    QElapsedTimer clock;
    QString expected;
    double inputReturned=-1,feedbackPaint=-1,requestSent=-1,response=-1,modelReady=-1;
    double firstCandlesPaint=-1,freshCandlesPaint=-1,studiesReady=-1;
    bool cacheAtInput=false;
};
class Http final:public TradingTransport {
  public:
    int delay=400,sends=0;quint64 epoch=0;Measurement* sample=nullptr;
    void abort()override{++epoch;}
    void send(quint64 token,const RequestSpec& request)override{
        if(request.authenticated||!MarketCandles::isRequest(request)||request.url.host()!="api.upbit.com")
            qFatal("Unexpected transport use in synthetic public chart probe");
        ++sends;
        const auto symbol=QUrlQuery(request.url).queryItemValue("market");
        const auto interval=request.url.path().section('/',-1).toLongLong()*60000;
        const auto generation=epoch;
        if(sample&&sample->expected=="coin:upbit:"+symbol&&sample->requestSent<0)
            sample->requestSent=sample->clock.nsecsElapsed()/1e6;
        QTimer::singleShot(delay,this,[this,generation,token,symbol,interval]{
            if(generation!=epoch)return;
            const auto result=seed(symbol,QDateTime::currentMSecsSinceEpoch(),interval);
            if(sample&&sample->expected=="coin:upbit:"+symbol&&sample->response<0)
                sample->response=sample->clock.nsecsElapsed()/1e6;
            emit finished(token,result);
        });
    }
};
class App final:public QApplication {
  public:
    using QApplication::QApplication;
    MarketCandles* candles=nullptr;QWidget* canvas=nullptr;QWidget* rows=nullptr;Measurement* sample=nullptr;
    bool notify(QObject* receiver,QEvent* event)override{
        const bool painting=sample&&event->type()==QEvent::Paint&&(receiver==canvas||receiver==rows);
        const bool result=QApplication::notify(receiver,event);
        if(!painting||!sample)return result;
        const auto elapsed=sample->clock.nsecsElapsed()/1e6;
        if(sample->feedbackPaint<0)sample->feedbackPaint=elapsed;
        if(receiver==canvas&&candles){
            const auto displayed=candles->displayObservation();
            if(displayed.identity==sample->expected&&!displayed.candles.isEmpty()&&
               canvas->property("displayIdentity").toString()==sample->expected&&
               canvas->property("displayCompletedCount").toInt()>0){
                if(sample->firstCandlesPaint<0)sample->firstCandlesPaint=elapsed;
                if(displayed.available&&!canvas->property("displayHistoricalOnly").toBool()&&sample->freshCandlesPaint<0)sample->freshCandlesPaint=elapsed;
            }
        }
        return result;
    }
};
}
int main(int argc,char** argv){
    App app(argc,argv);QCommandLineParser parser;parser.addHelpOption();
    parser.addOptions({{"output","Raw CSV path","path"},{"capture","Chart PNG path","path"},
        {"rounds","Rounds per scenario","count","20"},{"delay","Synthetic response delay (ms)","ms","400"},
        {"indicators","on or off","state","on"},{"width","Window width","pixels","1800"},
        {"height","Window height","pixels","1050"}});
    parser.process(app);const auto rounds=parser.value("rounds").toInt();
    if(rounds<1||rounds>100||!QStringList{"on","off"}.contains(parser.value("indicators")))return 2;
    QTemporaryDir temporary;if(!temporary.isValid())return 3;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.filePath("settings"));
    QCoreApplication::setOrganizationName("KEEFI-Synthetic-Chart-Goal");
    QCoreApplication::setApplicationName("KEEFI Synthetic Chart Switch Probe");
    KeefiFonts::registerBundledFonts();app.setFont(KeefiFonts::font(FontRole::Body15));
    KeefiTheme::instance()->setMode(ThemeMode::Dark);
    Http http;http.delay=parser.value("delay").toInt();
    MarketCandles candles(&http,std::unique_ptr<PublicCandleSocket>{});
    QStringList markets;QList<WatchlistIdentity> identities;
    for(int i=0;i<50;++i){markets.append(code(i));identities.append({"coin","upbit",code(i)});}
    candles.setVerifiedUpbitMarkets(markets,true);
    WatchlistWorkspaceList lists;lists.setVerifiedInstruments(identities);
    const auto list=lists.createList("합성 관심 50");lists.useList(list);
    for(const auto& id:identities)if(!lists.addIdentity(list,id))qFatal("Synthetic list rejected");
    ChartWorkspace workspace(&candles,nullptr);workspace.setWatchlistModel(&lists);
    auto* preferences=workspace.indicatorPreferences();preferences->markIntroSeen();
    const bool enabled=parser.value("indicators")=="on";
    if(!enabled)for(const auto& id:preferences->supportedIds("coin"))preferences->setEnabled(id,false);
    workspace.setTimeframe(60000);workspace.setIdentity(identity(0));
    workspace.resize(parser.value("width").toInt(),parser.value("height").toInt());workspace.show();workspace.activateWindow();
    auto* view=workspace.findChild<MarketCandlesView*>();
    auto* rows=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    auto* canvas=workspace.findChild<QWidget*>("marketCandlesChart");
    if(!rows||!view||!canvas)qFatal("Actual chart widgets unavailable");
    if(!waitFor([&]{return candles.series().available&&candles.series().candles.size()==200;}))qFatal("Initial OHLCV seed unavailable");
    QTest::qWait(50);app.canvas=canvas;app.rows=rows->viewport();app.candles=&candles;
    QFile file(parser.value("output"));if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 4;
    QTextStream csv(&file);csv.setRealNumberNotation(QTextStream::FixedNotation);csv.setRealNumberPrecision(6);
    csv<<"scenario,round,indicators,cache_at_input,input_returned_ms,feedback_paint_ms,request_sent_ms,response_ms,model_ready_ms,first_candles_paint_ms,fresh_candles_paint_ms,studies_ready_ms,response_to_paint_ms,response_to_model_ms\n";
    Measurement sample;
    QObject::connect(&candles,&MarketCandles::changed,&workspace,[&]{
        if(!app.sample)return;const auto live=candles.series();
        if(live.available&&live.identity==sample.expected&&sample.modelReady<0)
            sample.modelReady=sample.clock.nsecsElapsed()/1e6;
    });
    QObject::connect(view,&MarketCandlesView::studiesChanged,&workspace,[&]{
        if(app.sample&&enabled&&candles.series().identity==sample.expected&&view->chartStudies().available&&sample.studiesReady<0)
            sample.studiesReady=sample.clock.nsecsElapsed()/1e6;
    });
    auto measure=[&](const QString& scenario,int round,int target,bool revisitTab=false){
        sample=Measurement{};sample.expected=identity(target);sample.clock.start();app.sample=&sample;http.sample=&sample;
        if(revisitTab){workspace.show();}
        else {
            QTreeWidgetItem* item=nullptr;
            for(QTreeWidgetItemIterator it(rows);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==sample.expected){item=*it;break;}
            if(!item)qFatal("Synthetic selectable row missing");
            rows->scrollToItem(item);
            // The actual public row-activation signal keeps the input contract identical
            // across old/new column layouts; physical mouse targeting is in the full UI probe.
            emit rows->itemActivated(item,1);
        }
        sample.inputReturned=sample.clock.nsecsElapsed()/1e6;
        const auto display=candles.displayObservation();
        sample.cacheAtInput=display.identity==sample.expected&&!display.available&&!display.candles.isEmpty();
        if(workspace.selectedChartIdentity()!=sample.expected)qFatal("Wrong selected identity");
        if(!waitFor([&]{return sample.firstCandlesPaint>=0&&sample.freshCandlesPaint>=0&&(!enabled||sample.studiesReady>=0);}))
            qFatal("Selected candles or supplemental studies never painted/became ready");
        csv<<scenario<<','<<round<<','<<parser.value("indicators")<<','<<sample.cacheAtInput<<','<<sample.inputReturned<<','<<sample.feedbackPaint<<','<<sample.requestSent<<','<<sample.response<<','<<sample.modelReady<<','<<sample.firstCandlesPaint<<','<<sample.freshCandlesPaint<<','<<sample.studiesReady<<','
           <<(sample.response>=0?sample.freshCandlesPaint-sample.response:-1)<<','<<(sample.response>=0?sample.modelReady-sample.response:-1)<<'\n';csv.flush();
        app.sample=nullptr;http.sample=nullptr;QTest::qWait(15);
    };
    // Two cold combinations, followed by alternating warm revisits.
    measure("cold",0,1);measure("cold",1,2);
    for(int n=0;n<rounds;++n)measure("warm",n,n%2?2:1);
    for(int n=0;n<rounds;++n){const auto selected=workspace.selectedChartIdentity();const auto index=selected.endsWith("001")?1:2;workspace.hide();QTest::qWait(20);measure("tab_return",n,index,true);}
    // At least nine other combinations evict the target from an eight-entry LRU.
    for(int n=3;n<12;++n)measure("fill_lru",n,n);
    measure("evicted",0,1);
    if(!parser.value("capture").isEmpty()&&!workspace.grab().save(parser.value("capture")))return 5;
    QJsonObject evidence{{"synthetic",true},{"realNetworkRequests",0},{"platform",QGuiApplication::platformName()},
        {"responseDelayMs",http.delay},{"completedBars",200},{"listRows",50},{"rounds",rounds},{"fakeRequests",http.sends},
        {"firstPaintScope","actual canvas QWidget Paint handler completion; not compositor presentation"},
        {"inputScope","real Qt row activation handler; physical mouse separately measured"},
        {"modelReadyScope","finished response handling including parsing and synchronous view updates"},
        {"indicators",parser.value("indicators")}};
    QTextStream(stdout)<<QJsonDocument(evidence).toJson(QJsonDocument::Compact)<<Qt::endl;
    workspace.hide();return 0;
}

