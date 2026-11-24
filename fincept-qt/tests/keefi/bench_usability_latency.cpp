// Opt-in synthetic probe. Never links a live transport or opens a user profile.
#include "keefi/LocalWindow.h"
#include "keefi/BackgroundTray.h"
#include "keefi/AccountModel.h"
#include "keefi/Vault.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketMetrics.h"
#include "keefi/MarketFlowHistory.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/MarketOverview.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/OfficialNotices.h"
#include "fixtures/ProviderReadFixture.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numeric>
#include <sys/resource.h>
using namespace keefi;
namespace {
class NoTray final:public BackgroundTray {
  public:
    bool available()const override{return false;}bool visible()const override{return false;}
    void setMenu(QMenu*)override{}void show()override{}void hide()override{}
};
constexpr int Holdings=10,Watchlist=50;
double cpuMs(){rusage value{};getrusage(RUSAGE_SELF,&value);return value.ru_utime.tv_sec*1000.+value.ru_utime.tv_usec/1000.+value.ru_stime.tv_sec*1000.+value.ru_stime.tv_usec/1000.;}
QString asset(int i){return QString("S%1").arg(i,3,10,QChar('0'));}
QString market(int i){return "KRW-"+asset(i);}
QString samples(const QList<double>& values){QStringList text;for(const auto value:values)text.append(QString::number(value,'f',6));return text.join('|');}
double quantile(QList<double> values,double q){if(values.isEmpty())return -1;std::sort(values.begin(),values.end());return values[qBound(0,int(std::ceil(q*values.size()))-1,int(values.size())-1)];}
class ProbeApp final : public QApplication {
  public:
    using QApplication::QApplication;
    QWidget* target=nullptr;QElapsedTimer* inputClock=nullptr;double firstPaintMs=-1;QList<double> paintWall,paintCpu;
    bool notify(QObject* receiver,QEvent* event)override{
        if(receiver!=target||event->type()!=QEvent::Paint)return QApplication::notify(receiver,event);
        QElapsedTimer timer;timer.start();const auto before=cpuMs();const bool result=QApplication::notify(receiver,event);
        paintWall.append(timer.nsecsElapsed()/1e6);paintCpu.append(cpuMs()-before);
        if(firstPaintMs<0&&inputClock)firstPaintMs=inputClock->nsecsElapsed()/1e6;return result;
    }
};
class PublicHttp final : public TradingTransport {
  public:
    int sends=0;quint64 generation=0;
    void abort()override{++generation;}
    void send(quint64 token,const RequestSpec& request)override{
        if(request.authenticated||request.method!="GET"||request.url.host()!="api.upbit.com")qFatal("Synthetic public HTTP fixture received an unexpected request");
        ++sends;QJsonArray rows;const auto now=QDateTime::currentMSecsSinceEpoch();
        if(request.operation==Operation::Markets){for(int i=0;i<Watchlist;++i)rows.append(QJsonObject{{"market",market(i)},{"korean_name",QStringLiteral("합성 종목 %1").arg(i)},{"english_name","Synthetic "+asset(i)},{"market_event",QJsonObject{{"warning",false},{"caution",QJsonObject{}}}}});}
        else if(request.operation==Operation::Quote){for(const auto& code:QUrlQuery(request.url).queryItemValue("markets").split(',',Qt::SkipEmptyParts))rows.append(QJsonObject{{"market",code},{"trade_price","1000"},{"prev_closing_price","990"},{"signed_change_rate","0.01"},{"acc_trade_price_24h","100000000"},{"timestamp",QString::number(now)}});}
        else qFatal("Unexpected synthetic public HTTP operation");
        HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);const auto epoch=generation;
        QTimer::singleShot(0,this,[this,epoch,token,response]{if(epoch==generation)emit finished(token,response);});
    }
};
class NoNetwork final : public TradingTransport {
  public:
    int sends=0;quint64 generation=0;
    void abort()override{++generation;}
    void send(quint64 token,const RequestSpec& request)override{
        if(request.authenticated||isMutation(request.operation))qFatal("Unexpected private/mutation operation outside synthetic provider transport");
        ++sends;HttpResult response;response.status=503;const auto epoch=generation;
        QTimer::singleShot(0,this,[this,epoch,token,response]{if(epoch==generation)emit finished(token,response);});
    }
};
HttpResult candleSeed(const QString& code,qint64 now){
    QJsonArray rows;const auto end=now/60000*60000;
    for(int i=0;i<200;++i){const auto at=end-(200-i)*60000;const int price=1000+i%20;
        rows.append(QJsonObject{{"market",code},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(at+1000)},{"unit","1"},{"opening_price",QString::number(price)},{"high_price",QString::number(price+2)},
            {"low_price",QString::number(price-2)},{"trade_price",QString::number(price+1)},{"candle_acc_trade_volume","2"}});
    }
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);return response;
}
class CandleHttp final : public TradingTransport {
  public:
    std::function<qint64()> now;int sends=0;quint64 generation=0;
    void abort()override{++generation;}
    void send(quint64 token,const RequestSpec& request)override{
        if(!MarketCandles::isRequest(request)||request.url.host()!="api.upbit.com"||request.url.path()!="/v1/candles/minutes/1")qFatal("Unexpected synthetic candle request");
        ++sends;const auto response=candleSeed(QUrlQuery(request.url).queryItemValue("market"),now());const auto epoch=generation;
        QTimer::singleShot(0,this,[this,epoch,token,response]{if(epoch==generation)emit finished(token,response);});
    }
};
class CandleSocket final : public PublicCandleSocket {
  public:
    quint64 generation=0;int opens=0;
    void open(quint64 epoch,const QUrl&)override{generation=epoch;++opens;QTimer::singleShot(0,this,[this,epoch]{if(epoch==generation)emit connected(epoch);});}
    void send(quint64,const QByteArray&)override{}
    void abort()override{++generation;}
};
class QuoteSocket final : public PublicTickerSocket {
  public:
    quint64 generation=0;bool ready=false;int receivedCount=0;
    void open(const QUrl&)override{const auto epoch=++generation;QTimer::singleShot(0,this,[this,epoch]{if(epoch==generation){ready=true;emit connected();}});}
    void send(const QByteArray&)override{}
    void ping()override{emit alive();}
    void abort()override{++generation;ready=false;}
};
struct Rows {qint64 resets=0,inserted=0,removed=0,changed=0;};
void observeRows(QAbstractItemModel* model,QObject* owner,Rows* value){
    QObject::connect(model,&QAbstractItemModel::modelReset,owner,[value]{++value->resets;});
    QObject::connect(model,&QAbstractItemModel::rowsInserted,owner,[value](const QModelIndex&,int first,int last){value->inserted+=last-first+1;});
    QObject::connect(model,&QAbstractItemModel::rowsRemoved,owner,[value](const QModelIndex&,int first,int last){value->removed+=last-first+1;});
    QObject::connect(model,&QAbstractItemModel::dataChanged,owner,[value](const QModelIndex& first,const QModelIndex& last){value->changed+=last.row()-first.row()+1;});
}
void wheel(QWidget* widget,int delta){const auto point=widget->rect().center();QWheelEvent event(QPointF(point),QPointF(widget->mapToGlobal(point)),QPoint(),QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(widget,&event);}
void mouse(QWidget* widget,QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent event(type,point,QPointF(widget->mapToGlobal(point.toPoint())),button,buttons,Qt::NoModifier);QApplication::sendEvent(widget,&event);}
bool waitFor(const std::function<bool()>& condition,int timeout=5000){QElapsedTimer timer;timer.start();while(!condition()&&timer.elapsed()<timeout)QTest::qWait(1);return condition();}
}
int main(int argc,char** argv){
    ProbeApp app(argc,argv);KeefiFonts::registerBundledFonts();app.setFont(KeefiFonts::font(FontRole::Body15));QCommandLineParser parser;parser.addHelpOption();
    for(const auto& option:QList<QCommandLineOption>{{"width","Window width","pixels","1800"},{"height","Window height","pixels","1125"},{"theme","light or dark","theme","light"},{"rounds","Measured rounds","count","20"},{"mode","ticks or idle","mode","ticks"},{"output","Raw CSV output","path"},{"capture","Synthetic setup PNG","path"}})parser.addOption(option);
    parser.process(app);const int width=parser.value("width").toInt(),height=parser.value("height").toInt(),rounds=parser.value("rounds").toInt();const bool busy=parser.value("mode")=="ticks";
    if(width<720||height<720||rounds<1||rounds>100||!QStringList{"light","dark"}.contains(parser.value("theme")))return 2;
    QTemporaryDir dir;if(!dir.isValid())return 3;QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("KEEFI-Synthetic-P5");QCoreApplication::setApplicationName("KEEFI Synthetic P5 Probe");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    QSettings profile(dir.filePath("profile.ini"),QSettings::IniFormat);WatchlistWorkspaceList stored;QList<WatchlistIdentity> identities;QStringList keys;
    for(int i=0;i<Watchlist;++i){identities.append({"coin","upbit",market(i)});keys.append(asset(i));}
    stored.setVerifiedInstruments(identities);const auto list=stored.createList(QStringLiteral("합성 관심 50"));
    for(const auto& identity:identities)if(!stored.addIdentity(list,identity))qFatal("Cannot create synthetic public list");
    stored.useList(list);profile.setValue("chart/watchlists/v2",stored.publicState());profile.sync();QSettings().setValue("explore/watchlist/coin",keys);
    Vault vault(dir.filePath("synthetic-vault.bin"));if(!vault.create("Fake P5 Probe! 2026 Only"))qFatal("Synthetic vault creation failed");
    test::ReadFixture fixture("upbit");fixture.account.alias="Synthetic P5 only";
    if(!vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault))qFatal("Synthetic account registration failed");
    test::ProviderReadTransport accountHttp;accountHttp.provider="upbit";
    accountHttp.transform=[](const RequestSpec& request,HttpResult& response){if(request.operation!=Operation::Account)return;QJsonArray balances;
        for(int i=0;i<Holdings;++i)balances.append(QJsonObject{{"currency",asset(i)},{"balance","10"},{"locked","0"},{"avg_buy_price","900"},{"avg_buy_price_modified",false},{"unit_currency","KRW"}});
        response.status=200;response.body=QJsonDocument(balances).toJson(QJsonDocument::Compact);};
    auto* pricingHttp=new PublicHttp;PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &accountHttp;};factories.prices=[pricingHttp]{return pricingHttp;};factories.binanceBalances=[](Vault&){return static_cast<BinanceBalanceStream*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);portfolio.setBinanceBalancePilot(false);AccountModel accounts(&vault);
    PublicHttp catalogHttp;PublicMarketCatalog catalog(&catalogHttp);auto quoteSocket=std::make_unique<QuoteSocket>();auto* quoteFeed=quoteSocket.get();PublicQuoteStream quotes(PublicQuoteStream::Provider::Upbit,std::move(quoteSocket));catalog.setStream(&quotes);
    portfolio.setPublicMarketCatalog(&catalog);
    qint64 candleClock=QDateTime::currentMSecsSinceEpoch()/60000*60000+1000;CandleHttp candleHttp;candleHttp.now=[&]{return candleClock;};auto socket=std::make_unique<CandleSocket>();auto* candleFeed=socket.get();MarketCandles candles(&candleHttp,std::move(socket),[&]{return candleClock;});
    NoNetwork metadata,contracts,metricsHttp,historyHttp,coinbaseHttp,overviewHttp,calendarHttp,noticesHttp;
    PublicQuoteStream flowUpbit(PublicQuoteStream::Provider::Upbit,std::make_unique<QuoteSocket>()),flowBinance(PublicQuoteStream::Provider::Binance,std::make_unique<QuoteSocket>());
    MarketMetrics metrics(&metricsHttp);MarketFlowHistory history(&historyHttp);CoinbaseQuotes coinbase(&coinbaseHttp);MarketOverview overview(&overviewHttp);OfficialCalendar calendar(&calendarHttp);OfficialNotices notices(&noticesHttp);
    NoTray tray;LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalog,false);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);window.setChartContractTransport(&contracts);
    window.setMarketMetrics(&metrics,&coinbase,&flowUpbit,&flowBinance);window.setMarketFlowHistory(&history);window.setMarketOverview(&overview);window.setOfficialCalendar(&calendar);window.setOfficialNotices(&notices);
    const auto dormantNative=window.findChildren<NetworkTradingTransport*>();int dormantFinished=0;
    for(auto* transport:dormantNative)QObject::connect(transport,&TradingTransport::finished,&window,[&](quint64,const HttpResult&){++dormantFinished;qFatal("Dormant native transport unexpectedly produced a result");});
    if(auto* assets=window.findChild<QWidget*>("assetsWindow");!assets||assets->isVisible())qFatal("Dormant legacy assets dialog must remain hidden");
    KeefiFonts::registerBundledFonts();KeefiTheme::instance()->setMode(parser.value("theme")=="dark"?ThemeMode::Dark:ThemeMode::Light);
    window.resize(width,height);window.show();window.activateWindow();portfolio.refreshAll();catalog.refresh();
    auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* workspace=window.findChild<ChartWorkspace*>("chartWorkspace");auto* chartRows=window.findChild<QTreeWidget*>("chartWorkspaceWatchlist");auto* exploreRows=window.findChild<QTableWidget*>("watchlistTable");auto* portfolioRows=window.findChild<QTreeView*>("portfolioAssets");
    if(!pages||!workspace||!chartRows||!exploreRows||!portfolioRows)qFatal("Missing real workbench widgets");
    workspace->indicatorPreferences()->markIntroSeen();workspace->setTimeframe(60000);workspace->setIdentity(identities.first().key());
    if(!waitFor([&]{return !catalog.busy()&&catalog.rows().size()==Watchlist&&!portfolio.snapshots().isEmpty()&&portfolio.snapshots().first().components.size()==Holdings&&workspace->watchlistModel()->holdings().size()==Holdings&&exploreRows->rowCount()==Watchlist;}))qFatal("Synthetic holdings/watchlist fixture did not become complete");
    portfolio.setAutomaticRefresh(false);
    const int chart=pages->indexOf(workspace),explore=pages->indexOf(window.findChild<QWidget*>("explorePage")),home=pages->indexOf(window.findChild<QWidget*>("portfolioHome"));
    if(chart<0||explore<0||home<0)qFatal("Missing real navigation pages");
    auto tab=[&](int index){auto* button=window.findChild<QPushButton*>(QString("headerTab%1").arg(index));if(!button||!button->isVisible())qFatal("Missing visible real tab control");QTest::mouseClick(button,Qt::LeftButton,Qt::NoModifier,QPoint(),0);if(pages->currentIndex()!=index)qFatal("Real tab click did not navigate");};
    tab(chart);if(!waitFor([&]{return candles.series().available&&candles.series().candles.size()==200;}))qFatal("Synthetic native 200-bar fixture unavailable");
    if(workspace->currentListId()!=list||chartRows->topLevelItemCount()!=Watchlist)qFatal("Synthetic chart watchlist does not contain exactly 50 visible rows");
    for(int i=0;i<Watchlist;++i)if(chartRows->topLevelItem(i)->text(1).trimmed().isEmpty())qFatal("Synthetic chart watchlist public price missing");
    auto* view=workspace->findChild<MarketCandlesView*>();auto* chartPaint=workspace->findChild<QWidget*>("marketCandlesChart");auto* chartScroll=workspace->findChild<QScrollArea*>("chartWorkspaceScroll");if(!view||!chartPaint||!chartScroll)qFatal("Missing actual chart content");
    int ticks=0,candleChanges=0,quoteBatches=0;QTimer feed;feed.setTimerType(Qt::PreciseTimer);feed.setInterval(100);
    QObject::connect(&candles,&MarketCandles::changed,&window,[&]{++candleChanges;});QObject::connect(&quotes,&PublicQuoteStream::ticks,&window,[&]{++quoteBatches;});
    QObject::connect(&feed,&QTimer::timeout,&window,[&]{++ticks;candleClock+=100;const auto selected=WatchlistWorkspaceList::identityFromKey(workspace->selectedChartIdentity());
        if(selected.market=="coin"&&selected.source=="upbit"&&candleFeed->generation){const auto start=candleClock/60000*60000;QJsonObject row{{"type","candle.1m"},{"code",selected.code},{"stream_type","REALTIME"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(start,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},{"timestamp",QString::number(candleClock)},{"opening_price","1000"},{"high_price","1020"},{"low_price","980"},{"trade_price",QString::number(1000+ticks%3)},{"candle_acc_trade_volume",QString::number(ticks+1)}};emit candleFeed->received(candleFeed->generation,QJsonDocument(row).toJson(QJsonDocument::Compact));}
        if(quoteFeed->ready){const auto now=QDateTime::currentMSecsSinceEpoch();for(int i=0;i<Watchlist;++i){QJsonObject row{{"type","ticker"},{"code",market(i)},{"trade_price",QString::number(1000+(ticks+i)%3)},{"prev_closing_price","990"},{"signed_change_rate","0.01"},{"acc_trade_price_24h","100000000"},{"timestamp",QString::number(now)}};emit quoteFeed->received(QJsonDocument(row).toJson(QJsonDocument::Compact));++quoteFeed->receivedCount;}}
    });
    if(busy)feed.start();QTest::qWait(2100);if(busy&&(!candles.series().forming||ticks<10||quoteBatches<1))qFatal("Synthetic live feed is not reaching native public models");
    Rows chartCount,exploreCount,portfolioCount;observeRows(chartRows->model(),&window,&chartCount);observeRows(exploreRows->model(),&window,&exploreCount);observeRows(portfolioRows->model(),&window,&portfolioCount);
    QElapsedTimer heartClock;heartClock.start();double lastBeat=0;QList<double> gaps;QTimer heartbeat;heartbeat.setTimerType(Qt::PreciseTimer);heartbeat.setInterval(5);
    QObject::connect(&heartbeat,&QTimer::timeout,&window,[&]{const auto at=heartClock.nsecsElapsed()/1e6;if(lastBeat>0)gaps.append(qMax(0.,at-lastBeat-5.));lastBeat=at;});heartbeat.start();
    QFile raw(parser.value("output"));if(!raw.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text))qFatal("Raw output path unavailable");QTextStream csv(&raw);csv.setRealNumberNotation(QTextStream::FixedNotation);csv.setRealNumberPrecision(6);
    csv<<"theme,width,height,mode,round,operation,sync_ms,first_paint_end_ms,window_ms,cpu_ms,paint_wall_ms,paint_cpu_ms,paint_event_p95_ms,event_loop_gap_p95_ms,event_loop_gap_max_ms,paint_count,ticks,quote_batches,chart_resets,chart_inserted,chart_removed,chart_changed,explore_resets,explore_inserted,explore_removed,portfolio_resets,portfolio_inserted,portfolio_removed,paint_wall_samples_ms,paint_cpu_samples_ms,event_loop_gaps_ms,symbol_input_method\n";
    auto measure=[&](const QString& operation,int round,QWidget* target,const std::function<void()>& action,int windowMs=120){
        app.target=target;app.paintWall.clear();app.paintCpu.clear();app.firstPaintMs=-1;QElapsedTimer timer;timer.start();app.inputClock=&timer;const auto beforeCpu=cpuMs();const int firstGap=gaps.size(),firstTick=ticks,firstQuote=quoteBatches;const auto a=chartCount,b=exploreCount,c=portfolioCount;
        action();const auto sync=timer.nsecsElapsed()/1e6;while(timer.elapsed()<windowMs)QTest::qWait(1);QCoreApplication::processEvents(QEventLoop::AllEvents,5);
        const auto wall=timer.nsecsElapsed()/1e6,used=cpuMs()-beforeCpu;const auto wallSum=app.paintWall.isEmpty()?-1:std::accumulate(app.paintWall.cbegin(),app.paintWall.cend(),0.),cpuSum=app.paintCpu.isEmpty()?-1:std::accumulate(app.paintCpu.cbegin(),app.paintCpu.cend(),0.);const auto observedGaps=gaps.mid(firstGap);
        csv<<parser.value("theme")<<','<<width<<','<<height<<','<<(busy?"ticks":"idle")<<','<<round<<','<<operation<<','<<sync<<','<<app.firstPaintMs<<','<<wall<<','<<used<<','<<wallSum<<','<<cpuSum<<','<<quantile(app.paintWall,.95)<<','<<quantile(observedGaps,.95)<<','<<quantile(observedGaps,1)<<','<<app.paintWall.size()<<','<<ticks-firstTick<<','<<quoteBatches-firstQuote<<','<<chartCount.resets-a.resets<<','<<chartCount.inserted-a.inserted<<','<<chartCount.removed-a.removed<<','<<chartCount.changed-a.changed<<','<<exploreCount.resets-b.resets<<','<<exploreCount.inserted-b.inserted<<','<<exploreCount.removed-b.removed<<','<<portfolioCount.resets-c.resets<<','<<portfolioCount.inserted-c.inserted<<','<<portfolioCount.removed-c.removed<<','<<samples(app.paintWall)<<','<<samples(app.paintCpu)<<','<<samples(observedGaps)<<','<<(operation=="symbol_chart"?(width<1360?"qt_row_activation_chart_visible":"visible_list_mouse_chart_visible"):"not_applicable")<<'\n';csv.flush();app.target=nullptr;app.inputClock=nullptr;
    };
    measure("stream_steady",-1,chartPaint,[]{},2000);
    QList<double> symbolSourceReadyMs;
    for(int round=0;round<rounds;++round){
        measure("tab_portfolio",round,pages->widget(home),[&]{tab(home);});
        measure("tab_explore",round,pages->widget(explore),[&]{tab(explore);});
        measure("tab_chart",round,chartPaint,[&]{tab(chart);});
        auto* item=chartRows->topLevelItem(round%2?0:1);if(!item)qFatal("Chart selection row missing");
        const auto expectedIdentity=item->data(0,Qt::UserRole).toString();QElapsedTimer sourceReadyClock;
        if(width<1360){
            // List and chart occupy separate narrow scroll regions. Exercise the same public Qt
            // row-activation handler with the chart exposed; physical navigation is unmeasured.
            chartScroll->ensureWidgetVisible(chartPaint);QTest::qWait(30);
            sourceReadyClock.start();measure("symbol_chart",round,chartPaint,[&]{emit chartRows->itemActivated(item,0);});
        }else{
            chartScroll->ensureWidgetVisible(chartRows);chartRows->scrollToItem(item);QTest::qWait(30);
            sourceReadyClock.start();measure("symbol_chart",round,chartPaint,[&]{QTest::mouseClick(chartRows->viewport(),Qt::LeftButton,Qt::NoModifier,chartRows->visualItemRect(item).center(),0);});
        }
        if(workspace->selectedChartIdentity()!=expectedIdentity)qFatal("Measured symbol input did not select the expected identity");
        if(!waitFor([&]{return candles.series().available&&candles.series().identity==workspace->selectedChartIdentity();},2000))qFatal("Selected native source did not become ready");
        symbolSourceReadyMs.append(sourceReadyClock.nsecsElapsed()/1e6);
        chartScroll->ensureWidgetVisible(chartPaint);QTest::qWait(20);
        measure("wheel_zoom",round,chartPaint,[&]{wheel(chartPaint,round%2?-120:120);});
        const auto point=QPointF(chartPaint->width()/2.,chartPaint->height()/2.);
        measure("drag_pan",round,chartPaint,[&]{mouse(chartPaint,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);for(int i=1;i<=4;++i)mouse(chartPaint,QEvent::MouseMove,point+QPointF((round%2?-1:1)*i*20,0),Qt::NoButton,Qt::LeftButton);mouse(chartPaint,QEvent::MouseButtonRelease,point+QPointF((round%2?-1:1)*80,0),Qt::LeftButton,Qt::NoButton);});
        chartScroll->ensureWidgetVisible(chartRows);QTest::qWait(20);auto* bar=chartRows->verticalScrollBar();if(bar->maximum()>0&&bar->value()==0)bar->setValue(bar->maximum()/2);
        measure("watchlist_wheel",round,chartRows->viewport(),[&]{wheel(chartRows->viewport(),round%2?120:-120);});
        chartScroll->verticalScrollBar()->setValue(0);QTest::qWait(20);
    }
    feed.stop();heartbeat.stop();QTest::qWait(10);
    if(parser.isSet("capture")&&!window.grab().save(parser.value("capture")))qFatal("Synthetic setup PNG save failed");
    QJsonObject receipt{{"platform",QGuiApplication::platformName()},{"theme",parser.value("theme")},{"width",window.width()},{"height",window.height()},{"holdings",portfolio.snapshots().first().components.size()},{"chartHoldings",workspace->watchlistModel()->holdings().size()},{"watchlist",exploreRows->rowCount()},{"nativeCandles",candles.series().candles.size()},{"forming",candles.series().forming.has_value()},{"ticks",ticks},{"candleChanges",candleChanges},{"quoteBatches",quoteBatches},{"fakeQuoteMessages",quoteFeed->receivedCount},{"fakeCatalogRequests",catalogHttp.sends},{"fakePricingRequests",pricingHttp->sends},{"fakeCandleRequests",candleHttp.sends},{"injectedLiveTransports",0},{"dormantNativeTransportObjects",dormantNative.size()},{"dormantNativeFinished",dormantFinished},{"temporarySyntheticProfile",true},{"captureScope",pages->currentWidget()->objectName()},{"chartRows",chartRows->topLevelItemCount()},{"chartPaintWidth",chartPaint->width()},{"chartPaintHeight",chartPaint->height()},{"symbolSourceReadyMs",QJsonArray::fromVariantList([&]{QVariantList values;for(const auto value:symbolSourceReadyMs)values.append(value);return values;}())},{"symbolInputMethod",width<1360?"qt_row_activation_chart_visible":"visible_list_mouse_chart_visible"},{"firstPaintScope","target QWidget Paint handler returned; compositor visibility is unmeasured"}};
    fprintf(stderr,"P5_FIXTURE %s\n",QJsonDocument(receipt).toJson(QJsonDocument::Compact).constData());raw.close();portfolio.stopReads();window.close();return 0;
}
