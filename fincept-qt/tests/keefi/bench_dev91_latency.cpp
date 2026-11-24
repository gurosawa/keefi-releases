// Opt-in native synthetic latency probe. No live transport, vault or personal profile.
// Results end at QWidget Paint handler return, never compositor/physical-display visibility.
#include "keefi/AssetShareCard.h"
#include "keefi/ChartGridView.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/MarketCompareSource.h"
#include "keefi/MarketCompareView.h"
#include "keefi/TradingTransport.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUrlQuery>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <vector>
using namespace keefi;
namespace {
constexpr qint64 Minute=60000;
constexpr int Rounds=5;
void require(bool ok,const QString& reason){if(!ok)throw std::runtime_error(reason.toStdString());}
bool waitFor(const std::function<bool()>& condition,int timeout=8000){
    QElapsedTimer timer;timer.start();while(!condition()&&timer.elapsed()<timeout)QTest::qWait(1);return condition();
}
class ProbeApp final:public QApplication {
public:
    using QApplication::QApplication;
    struct Watch {std::function<bool(QObject*)> matches;std::function<bool()> ready;double elapsed=-1;};
    QElapsedTimer clock;std::vector<Watch> watches;
    void begin(){watches.clear();clock.start();}
    int watch(std::function<bool(QObject*)> match,std::function<bool()> ready=[]{return true;}){
        watches.push_back({std::move(match),std::move(ready),-1});return int(watches.size())-1;
    }
    bool notify(QObject* receiver,QEvent* event)override{
        std::vector<int> observed;
        if(event->type()==QEvent::Paint)for(int i=0;i<int(watches.size());++i)
            if(watches[i].elapsed<0&&watches[i].matches(receiver)&&watches[i].ready())observed.push_back(i);
        const bool handled=QApplication::notify(receiver,event);
        for(const auto index:observed)if(index<int(watches.size())&&watches[index].elapsed<0)
            watches[index].elapsed=clock.nsecsElapsed()/1e6;
        return handled;
    }
};
class PublicHttp final:public TradingTransport {
public:
    int delayMs=0,sends=0;quint64 generation=0;QString failure;
    void abort()override{++generation;}
    void send(quint64 token,const RequestSpec& request)override{
        ++sends;HttpResult response;response.status=200;
        try{
        require(!request.authenticated&&request.method=="GET"&&!isMutation(request.operation),"Unexpected private/mutation request");
        const auto host=request.url.host(),path=request.url.path();const QUrlQuery query(request.url);
        if(host=="fapi.binance.com"&&path=="/fapi/v1/exchangeInfo"){
            response.body=R"({"symbols":[{"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},{"symbol":"CLUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},{"symbol":"QQQUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"EQUITY"}]})";
        }else if((host=="api.binance.com"&&path=="/api/v3/klines")||(host=="fapi.binance.com"&&path=="/fapi/v1/klines")){
            const QMap<QString,qint64> intervals{{"1m",Minute},{"5m",5*Minute},{"15m",15*Minute},{"1h",60*Minute},{"4h",240*Minute},{"1d",1440*Minute}};
            const auto interval=intervals.value(query.queryItemValue("interval"));require(interval>0,"Unexpected compare interval");
            const auto end=QDateTime::currentMSecsSinceEpoch()/interval*interval;QJsonArray bars;
            for(int i=0;i<120;++i){const auto open=end-(120-i)*interval;const auto price=QString::number(100+i%17);
                bars.append(QJsonArray{double(open),price,price,price,price,"2",double(open+interval-1),"200",1,"1","100","0"});}
            response.body=QJsonDocument(bars).toJson(QJsonDocument::Compact);
        }else if(host=="api.upbit.com"&&path.startsWith("/v1/candles/minutes/")){
            bool ok=false;const int minutes=path.section('/',-1).toInt(&ok);require(ok&&minutes>0,"Unexpected candle interval");
            const qint64 interval=minutes*Minute,end=QDateTime::currentMSecsSinceEpoch()/interval*interval;QJsonArray bars;
            const auto market=query.queryItemValue("market");require(market=="KRW-BTC"||market=="KRW-ETH","Unexpected synthetic market");
            for(int i=0;i<200;++i){const auto open=end-(200-i)*interval;const auto price=QString::number(100+i%17);
                bars.prepend(QJsonObject{{"market",market},{"unit",minutes},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(open,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                    {"timestamp",QString::number(open+1000)},{"opening_price",price},{"high_price",price},{"low_price",price},{"trade_price",price},{"candle_acc_trade_volume","2"}});}
            response.body=QJsonDocument(bars).toJson(QJsonDocument::Compact);
        }else require(false,"Unexpected public request: "+host+path);
        }catch(const std::exception& error){failure=QString::fromUtf8(error.what());response.status=503;response.body.clear();}
        const auto epoch=generation;
        QTimer::singleShot(delayMs,this,[this,epoch,token,response]{if(epoch==generation)emit finished(token,response);});
    }
};
class SilentSocket final:public PublicCandleSocket {
public:
    quint64 generation=0;
    void open(quint64 epoch,const QUrl&)override{generation=epoch;QTimer::singleShot(0,this,[this,epoch]{if(epoch==generation)emit connected(epoch);});}
    void send(quint64,const QByteArray&)override{}
    void abort()override{++generation;}
};
QWidget* accessibleChild(QWidget& root,const QString& name){
    for(auto* widget:root.findChildren<QWidget*>())if(widget->accessibleName()==name)return widget;return nullptr;
}
AssetShareSnapshot shareFixture(){
    AssetShareSnapshot value;value.assetName="합성 자산";value.assetSymbol="SYN";
    const qint64 interval=15*Minute,end=QDateTime::currentMSecsSinceEpoch()/interval*interval,begin=end-97*interval;
    auto& market=value.market;market.identity={"coin","upbit","KRW-SYN"};market.currency="KRW";market.intervalMs=interval;market.displayTimeZoneId="Asia/Seoul";
    for(int i=0;i<97;++i){const auto price=QString::number(100+i);market.closed.append({begin+i*interval,begin+(i+1)*interval-1,price,price,price,price,"10"});}
    market.calculatedThroughMs=end;market.asOfMs=end+1000;market.viewport={begin,end-interval};market.studies=keefiChartStudies(market.closed,interval);
    market.source.provider=ChartShareSource::Upbit;market.source.canonicalUrl=MarketCandles::request(market.identity.key(),interval).url.toString();
    market.source.receivedAtMs=end+500;market.availability=ChartShareAvailability::Ready;
    value.requestedFromMs=begin+interval;value.requestedToMs=end;return value;
}
void summarize(QJsonObject& operations,const QString& name,const QList<double>& samples,const QString& scope){
    require(samples.size()==Rounds,"Missing latency samples: "+name);auto sorted=samples;std::sort(sorted.begin(),sorted.end());
    QJsonArray raw;for(const auto value:samples){require(value>=0,"Unobserved paint: "+name);raw.append(value);}
    operations.insert(name,QJsonObject{{"samples_ms",raw},{"median_ms",sorted[2]},{"max_ms",sorted.last()},{"rounds",Rounds},{"scope",scope}});
}
}
int main(int argc,char** argv){
    ProbeApp app(argc,argv);QCommandLineParser parser;parser.addHelpOption();
    parser.addOptions({{"output","New absolute JSON receipt path","path"},{"theme","light or dark","theme","light"},
        {"width","Window width","pixels","1800"},{"height","Window height","pixels","1125"}});parser.process(app);
    const auto output=parser.value("output");if(!QFileInfo(output).isAbsolute()||output.isEmpty())return 2;
    QFile file(output);if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
    QJsonObject operations,receipt{{"schema",1},{"probe","dev91_m1_synthetic_native"},{"platform",QGuiApplication::platformName()},
        {"qt_version",qVersion()},{"architecture",QSysInfo::currentCpuArchitecture()},{"rounds",Rounds},
        {"input","QTest mouse/key events on actual public widgets; physical input unmeasured"},
        {"paint_boundary","target QWidget Paint handler returned; compositor visibility unmeasured"},
        {"synthetic",true},{"live_network_transports_injected",0},{"personal_profile_opened",false},{"vault_opened",false}};
    int result=0;
    try{
        require(QGuiApplication::platformName()=="cocoa","Native measurements require cocoa; no offscreen measurement was run");
        const int width=parser.value("width").toInt(),height=parser.value("height").toInt();
        require(width>=1280&&height>=900,"Probe requires at least 1280x900 to keep actual controls visible");
        require(QStringList{"light","dark"}.contains(parser.value("theme")),"Unknown theme");
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary settings unavailable");QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("KEEFI Synthetic M1");QCoreApplication::setApplicationName("KEEFI Dev91 Synthetic Latency");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.filePath("user"));
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,temporary.filePath("system"));
        require(KeefiFonts::registerBundledFonts(),"Bundled fonts unavailable");app.setFont(KeefiFonts::font(FontRole::Body15));
        KeefiTheme::instance()->setMode(parser.value("theme")=="dark"?ThemeMode::Dark:ThemeMode::Light);
        receipt.insert("theme",parser.value("theme"));receipt.insert("width",width);receipt.insert("height",height);
        {
            PublicHttp wire;MarketCompareSource source(&wire);source.setCollectorRoot(temporary.filePath("not-created-collector"));
            MarketCompareView view(&source);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.resize(width,height);view.show();view.activateWindow();
            require(QTest::qWaitForWindowExposed(&view),"Compare window was not exposed");
            const auto complete=[&]{const auto snapshot=source.snapshot();if(snapshot.loading||snapshot.series.size()!=4)return false;
                for(const auto& series:snapshot.series)if(!series.available||series.bars.size()!=120||series.intervalMs!=snapshot.intervalMs)return false;return true;};
            require(waitFor(complete),"Compare fixture failed to load four series");auto* chart=accessibleChild(view,"시장 비교 차트");require(chart,"Compare paint surface unavailable");
            for(const int delay:{0,300}){
                wire.delayMs=delay;QList<double> selected,data;
                for(int round=0;round<Rounds;++round){const qint64 interval=source.intervalMs()==5*Minute?15*Minute:5*Minute;
                    auto* button=view.findChild<QPushButton*>(QString("marketCompareTimeframe_%1").arg(interval));require(button&&button->isVisible(),"Compare period control unavailable");
                    app.begin();const int choice=app.watch([=](QObject* object){return object==button;},[=]{return button->isChecked();});
                    const int ready=app.watch([=](QObject* object){return object==chart;},[&,interval]{return source.intervalMs()==interval&&complete();});
                    QTest::mouseClick(button,Qt::LeftButton,Qt::NoModifier,QPoint(),0);
                    require(source.intervalMs()==interval,"Compare period did not change");
                    require(waitFor([&]{return app.watches[choice].elapsed>=0&&app.watches[ready].elapsed>=0;}),"Compare paint was not observed");
                    selected.append(app.watches[choice].elapsed);data.append(app.watches[ready].elapsed);app.watches.clear();
                }
                summarize(operations,QString("compare_selection_delay_%1").arg(delay),selected,"Click to selected button paint; four series x 120 bars");
                summarize(operations,QString("compare_data_delay_%1").arg(delay),data,"Click to chart paint with all four requested series ready; includes source pacing and per-request synthetic delay");
            }
            require(wire.failure.isEmpty(),wire.failure);receipt.insert("compare_fake_requests",wire.sends);view.close();source.setActive(false);
        }
        {
            PublicHttp candlesHttp,studiesHttp,contractsHttp;MarketCandles candles(&candlesHttp,std::make_unique<SilentSocket>());
            candles.setVerifiedUpbitMarkets({"KRW-BTC","KRW-ETH"},true);
            ChartStudyFeed feed(&studiesHttp);feed.setVerifiedUpbitMarkets({"KRW-BTC","KRW-ETH"},true);
            ChartWorkspace workspace(&candles,nullptr);workspace.setContractTransport(&contractsHttp);workspace.setStudyFeed(&feed);
            auto* lists=workspace.watchlistModel();const QList<WatchlistIdentity> identities{{"coin","upbit","KRW-BTC"},{"coin","upbit","KRW-ETH"}};
            lists->setVerifiedInstruments(identities);const auto list=lists->createList("합성 M1 공개 목록");
            for(const auto& identity:identities)require(lists->addIdentity(list,identity),"Cannot create synthetic watchlist");lists->useList(list);
            workspace.indicatorPreferences()->markIntroSeen();workspace.setTimeframe(Minute);require(workspace.setIdentity(identities.first().key()),"Cannot select synthetic public chart");
            workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.resize(width,height);workspace.show();workspace.activateWindow();
            require(QTest::qWaitForWindowExposed(&workspace),"Chart window was not exposed");
            require(waitFor([&]{return candles.series().available&&candles.series().candles.size()==200;}),"Public candle fixture unavailable");
            auto* picker=workspace.findChild<QPushButton*>("chartStudyPicker");require(picker,"Indicator picker unavailable");
            QList<double> opened,searched;
            for(int round=0;round<Rounds;++round){
                app.begin();const auto treePaint=[&](QObject* object){auto* tree=workspace.findChild<QTreeWidget*>("chartStudyPickerTree");return tree&&object==tree->viewport();};
                const int opening=app.watch(treePaint);QString failure;
                QTimer::singleShot(0,&workspace,[&]{
                    try{
                        require(waitFor([&]{return app.watches[opening].elapsed>=0;}),"Indicator list first paint unavailable");opened.append(app.watches[opening].elapsed);
                        auto* dialog=workspace.findChild<QDialog*>("chartStudyPickerDialog");auto* search=workspace.findChild<QLineEdit*>("chartStudySearch");
                        require(dialog&&search,"Indicator dialog controls unavailable");app.begin();const int searching=app.watch(treePaint,[search]{return search->text()=="RSI";});
                        QTest::keyClicks(search,"RSI",Qt::NoModifier,0);
                        require(waitFor([&]{return app.watches[searching].elapsed>=0;}),"Filtered indicator list paint unavailable");
                        require(search->text()=="RSI","Search input not delivered");searched.append(app.watches[searching].elapsed);app.watches.clear();dialog->reject();
                    }catch(const std::exception& error){failure=QString::fromUtf8(error.what());app.watches.clear();if(auto* dialog=workspace.findChild<QDialog*>("chartStudyPickerDialog"))dialog->reject();}
                });
                QTest::mouseClick(picker,Qt::LeftButton,Qt::NoModifier,QPoint(),0);require(failure.isEmpty(),failure);
                require(opened.size()==round+1&&searched.size()==round+1,"Indicator modal interaction did not complete");
            }
            summarize(operations,"indicator_open",opened,"Click to actual indicator tree viewport first paint; 200 closed public candles");
            summarize(operations,"indicator_search",searched,"QTest RSI key sequence to actual indicator tree viewport paint");
            auto* single=workspace.findChild<QPushButton*>("chartLayout_1");auto* two=workspace.findChild<QPushButton*>("chartLayout_2");
            require(single&&two,"Actual split controls unavailable");QList<double> split;
            for(int round=0;round<Rounds;++round){
                QTest::mouseClick(single,Qt::LeftButton,Qt::NoModifier,QPoint(),0);QTest::qWait(30);
                app.begin();const int painting=app.watch([](QObject* object){auto* widget=qobject_cast<QWidget*>(object);return widget&&widget->accessibleName()=="분할 차트 완료 봉";});
                QTest::mouseClick(two,Qt::LeftButton,Qt::NoModifier,QPoint(),0);
                require(two->isChecked(),"Two-panel selection did not apply");require(waitFor([&]{return app.watches[painting].elapsed>=0;}),"Passive split panel paint unavailable");
                split.append(app.watches[painting].elapsed);app.watches.clear();
            }
            summarize(operations,"split_1_to_2",split,"Actual toolbar click to passive panel first paint; active chart has 200 bars; passive data readiness is not required");
            require(candlesHttp.failure.isEmpty()&&studiesHttp.failure.isEmpty()&&contractsHttp.failure.isEmpty(),"A chart fixture received an unexpected request");
            receipt.insert("chart_fake_requests",candlesHttp.sends);receipt.insert("study_fake_requests",studiesHttp.sends);
            receipt.insert("contract_fake_requests",contractsHttp.sends);workspace.close();feed.stop();
        }
        const auto snapshot=shareFixture();QList<double> render;
        for(int round=0;round<Rounds;++round){QElapsedTimer timer;timer.start();const auto image=AssetShareCard::render(snapshot);const auto elapsed=timer.nsecsElapsed()/1e6;
            require(image.available&&image.image.size()==QSize(1000,1200),"Synthetic asset card render failed: "+image.reason);render.append(elapsed);}
        summarize(operations,"asset_card_render",render,"Synchronous public AssetShareCard::render to complete 1000x1200 QImage; no personal fields or file save");
        receipt.insert("status","passed");
    }catch(const std::exception& error){app.watches.clear();receipt.insert("status","failed");receipt.insert("error",QString::fromUtf8(error.what()));result=1;}
    receipt.insert("operations",operations);
    receipt.insert("unmeasured",QJsonArray{"LocalWindow navigation/loading/private holdings","Physical input and compositor/display latency","Live HTTP timing","Production or installed-app performance"});
    file.write(QJsonDocument(receipt).toJson(QJsonDocument::Indented));file.close();return result;
}
