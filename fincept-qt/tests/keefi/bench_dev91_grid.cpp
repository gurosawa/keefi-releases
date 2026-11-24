// Opt-in cocoa benchmark: synthetic public candles only, no network or profile.
// One invocation measures one of 1/2 panels x 1m/15m x off/default indicators.
// A short invocation is a harness check, never evidence for the 5/30-minute run.
#include "keefi/ChartGridView.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/TradingTransport.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <functional>
#include <sys/resource.h>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif

using namespace keefi;
namespace {
constexpr qint64 Minute=60000;
const QString Active="coin:upbit:KRW-SYN", Passive="coin:upbit:KRW-ZZZ";
double cpuMs(){
    rusage r{};if(getrusage(RUSAGE_SELF,&r))return -1;
    return r.ru_utime.tv_sec*1000.+r.ru_utime.tv_usec/1000.+r.ru_stime.tv_sec*1000.+r.ru_stime.tv_usec/1000.;
}
qint64 residentBytes(){
#ifdef Q_OS_MACOS
    mach_task_basic_info_data_t info{};mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,reinterpret_cast<task_info_t>(&info),&count)==KERN_SUCCESS)
        return qint64(info.resident_size);
#endif
    return -1;
}
bool waitFor(const std::function<bool()>& ready,int timeoutMs){
    QElapsedTimer elapsed;elapsed.start();
    while(!ready()&&elapsed.elapsed()<timeoutMs)QTest::qWait(10);
    return ready();
}
QJsonObject counters(int requests,int aborts,const QJsonObject& byMarket){
    return {{"requests",requests},{"aborts",aborts},{"requests_by_market",byMarket}};
}
class CandleHttp final:public TradingTransport {
  public:
    std::function<qint64()> now;int requests=0,aborts=0;quint64 generation=0;
    QString error;QJsonObject byMarket;
    void abort()override{++generation;++aborts;}
    void send(quint64 token,const RequestSpec& request)override{
        ++requests;const auto query=QUrlQuery(request.url);const auto code=query.queryItemValue("market");
        const auto path=request.url.path();const qint64 interval=path=="/v1/candles/minutes/1"?Minute:path=="/v1/candles/minutes/15"?15*Minute:0;
        HttpResult reply;reply.status=503;
        if(request.authenticated||request.method!="GET"||!MarketCandles::isRequest(request)||
           request.url.host()!="api.upbit.com"||!interval||!QStringList{"KRW-SYN","KRW-ZZZ"}.contains(code)){
            error="Unexpected request rejected by synthetic transport";
        }else{
            byMarket[code]=byMarket.value(code).toInt()+1;
            qint64 end=now()/interval*interval;
            const auto before=query.queryItemValue("to");
            if(!before.isEmpty()){
                const auto parsed=QDateTime::fromString(before,Qt::ISODate);
                if(!parsed.isValid())error="Invalid synthetic history cursor";
                else end=qMin(end,parsed.toMSecsSinceEpoch()/interval*interval);
            }
            if(error.isEmpty()){
                QJsonArray rows;
                for(int n=0;n<200;++n){
                    const auto at=end-(200-n)*interval;const int price=1000+int(at/interval%20);
                    rows.append(QJsonObject{{"market",code},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                        {"timestamp",QString::number(at+1000)},{"unit",QString::number(interval/Minute)},
                        {"opening_price",QString::number(price)},{"high_price",QString::number(price+3)},
                        {"low_price",QString::number(price-3)},{"trade_price",QString::number(price+1)},
                        {"candle_acc_trade_volume","2"}});
                }
                reply.status=200;reply.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
            }
        }
        const auto epoch=generation;
        QTimer::singleShot(0,this,[this,epoch,token,reply]{if(epoch==generation)emit finished(token,reply);});
    }
    QJsonObject snapshot()const{return counters(requests,aborts,byMarket);}
};
class CandleSocket final:public PublicCandleSocket {
  public:
    quint64 generation=0;bool active=false;int opens=0,aborts=0,subscriptions=0,frames=0,peakActive=0;
    QString code,type,error;
    void open(quint64 epoch,const QUrl& url)override{
        if(url!=QUrl("wss://api.upbit.com/websocket/v1")){error="Unexpected socket URL rejected";return;}
        if(active){error="New socket opened before the previous synthetic socket was aborted";return;}
        generation=epoch;active=true;++opens;peakActive=1;
        QTimer::singleShot(0,this,[this,epoch]{if(active&&epoch==generation)emit connected(epoch);});
    }
    void send(quint64 epoch,const QByteArray& bytes)override{
        if(!active||epoch!=generation)return;
        const auto values=QJsonDocument::fromJson(bytes).array();
        if(values.size()!=3){error="Unexpected synthetic socket subscription";return;}
        const auto request=values[1].toObject();type=request.value("type").toString();
        const auto codes=request.value("codes").toArray();code=codes.size()==1?codes[0].toString():QString{};
        if(!QStringList{"KRW-SYN","KRW-ZZZ"}.contains(code)||!QStringList{"candle.1m","candle.15m"}.contains(type)){
            error="Unexpected synthetic socket market or timeframe";return;
        }
        ++subscriptions;
    }
    void abort()override{active=false;code.clear();type.clear();++generation;++aborts;}
    void tick(qint64 now){
        if(!active||code.isEmpty()||!error.isEmpty())return;
        const qint64 interval=type=="candle.1m"?Minute:15*Minute;const auto start=now/interval*interval;
        const QJsonObject row{{"type",type},{"code",code},{"stream_type","REALTIME"},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(start,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(now)},{"opening_price","1000"},{"high_price","1020"},
            {"low_price","980"},{"trade_price",QString::number(1000+frames%3)},
            {"candle_acc_trade_volume",QString::number(frames+1)}};
        ++frames;emit received(generation,QJsonDocument(row).toJson(QJsonDocument::Compact));
    }
    QJsonObject snapshot()const{return {{"opens",opens},{"aborts",aborts},{"subscriptions",subscriptions},
        {"frames_emitted",frames},{"active",active?1:0},{"peak_active",peakActive}};}
};
QJsonObject sample(const QElapsedTimer& elapsed,double initialCpu){
    const double at=elapsed.nsecsElapsed()/1e6,cpu=cpuMs();
    return {{"elapsed_ms",at},{"process_cpu_ms",cpu},{"cpu_since_baseline_ms",cpu-initialCpu},
        {"average_cpu_percent",at>0?100.*(cpu-initialCpu)/at:0.},{"rss_bytes",residentBytes()}};
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;parser.setApplicationDescription("Synthetic cocoa-only 1/2-panel chart benchmark");parser.addHelpOption();
    for(const auto& option:QList<QCommandLineOption>{{"panels","1 or 2 panels","count","1"},
        {"interval-ms","60000 or 900000","milliseconds","60000"},{"indicators","off or default","mode","default"},
        {"duration-seconds","Bounded duration; 1800 is needed for both milestones","seconds","1800"},
        {"width","Window width","pixels","1360"},{"height","Window height","pixels","900"},
        {"output","New absolute JSON path; parent must already exist","path"}})parser.addOption(option);
    parser.process(app);
    bool panelsOk=false,intervalOk=false,durationOk=false,widthOk=false,heightOk=false;
    const int panels=parser.value("panels").toInt(&panelsOk),seconds=parser.value("duration-seconds").toInt(&durationOk);
    const qint64 interval=parser.value("interval-ms").toLongLong(&intervalOk);
    const int width=parser.value("width").toInt(&widthOk),height=parser.value("height").toInt(&heightOk);
    const auto mode=parser.value("indicators"),output=parser.value("output");
    if(!panelsOk||(panels!=1&&panels!=2)||!intervalOk||(interval!=Minute&&interval!=15*Minute)||
       !durationOk||seconds<1||seconds>3600||!widthOk||!heightOk||width<1100||height<760||width>3840||height>2160||
       !QStringList{"off","default"}.contains(mode)||!QFileInfo(output).isAbsolute()||QFileInfo::exists(output)||
       !QFileInfo(QFileInfo(output).absolutePath()).isDir())return 2;
    QJsonObject report{{"schema","keefi-dev91-synthetic-grid-v1"},{"synthetic_only",true},
        {"platform",QGuiApplication::platformName()},{"panels",panels},{"interval_ms",interval},
        {"indicator_mode",mode},{"duration_requested_seconds",seconds},{"scheduled_tick_hz",10},
        {"window_requested",QJsonObject{{"width",width},{"height",height}}},
        {"real_account_validation",false},{"live_network_validation",false},
        {"private_profile_opened",false},{"long_duration_complete",false},
        {"five_minute",QJsonValue::Null},{"thirty_minute",QJsonValue::Null},
        {"rss_growth_5_to_30_minutes_bytes",QJsonValue::Null},
        {"scope","Existing active MarketCandlesView plus ChartGridView passive REST panel; no flow/context feed, account, vault, or production app"},
        {"indicator_mode_scope","Active full chart only; passive panel always uses its existing volume/SMA20 renderer"},
        {"passive_rest_panels",panels-1},{"active_socket_models",1}};
    QFile executable(QCoreApplication::applicationFilePath());
    if(executable.open(QIODevice::ReadOnly))report["executable_sha256"]=QString::fromLatin1(QCryptographicHash::hash(executable.readAll(),QCryptographicHash::Sha256).toHex());
    const auto finish=[&](const QString& status,const QString& reason,int result){
        report["status"]=status;report["reason"]=reason;
        QSaveFile file(output);file.setDirectWriteFallback(false);
        const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);
        if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit())return 3;
        QTextStream(stdout)<<output<<Qt::endl;return result;
    };
    if(QGuiApplication::platformName()!="cocoa")return finish("blocked","cocoa platform required; no window was shown",4);
    QTemporaryDir temporary;if(!temporary.isValid())return finish("blocked","Cannot create synthetic settings directory",4);
    QStandardPaths::setTestModeEnabled(true);QCoreApplication::setOrganizationName("KEEFI-Synthetic-Dev91");
    QCoreApplication::setApplicationName("KEEFI Synthetic Dev91 Grid Benchmark");QSettings::setDefaultFormat(QSettings::IniFormat);
    for(const auto scope:{QSettings::UserScope,QSettings::SystemScope})QSettings::setPath(QSettings::IniFormat,scope,temporary.path());
    KeefiFonts::registerBundledFonts();app.setFont(KeefiFonts::font(FontRole::Body15));KeefiTheme::instance()->setMode(ThemeMode::Light);
    QElapsedTimer sourceClock;sourceClock.start();const auto sourceStart=QDateTime::currentMSecsSinceEpoch();
    const auto now=[&]{return sourceStart+sourceClock.elapsed();};
    CandleHttp activeHttp,passiveHttp;activeHttp.now=now;passiveHttp.now=now;
    auto socket=std::make_unique<CandleSocket>();auto* wire=socket.get();
    MarketCandles model(&activeHttp,std::move(socket),now);ChartStudyFeed feed(&passiveHttp,nullptr,nullptr,now);
    model.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);feed.setVerifiedUpbitMarkets({"KRW-SYN","KRW-ZZZ"},true);
    ChartStudyPreferences preferences;preferences.markIntroSeen();
    if(mode=="off")for(const auto& id:preferences.supportedIds("coin"))preferences.setEnabled(id,false);
    report["enabled_indicator_ids"]=QJsonArray::fromStringList(preferences.enabledOrder("coin"));
    QWidget host;host.setWindowTitle("KEEFI Synthetic Dev91 Grid Benchmark");auto* layout=new QVBoxLayout(&host);
    auto* view=new MarketCandlesView(&model,&host);view->setIndicatorPreferences(&preferences);view->setSelection(Active,interval);
    auto* grid=new ChartGridView(&host);grid->setSource(&feed);
    QObject::connect(grid,&ChartGridView::activated,view,[view](const QString& key,qint64 value){view->setSelection(key,value);});
    if(panels==1){grid->hide();layout->addWidget(view);}else{
        grid->setActiveView(view);grid->setSelection(Active,interval);grid->seed({Active,Passive});
        grid->setPanelCount(2);layout->addWidget(grid);
        auto* period=grid->findChild<QComboBox*>("chartGridTimeframe_1");auto* first=grid->findChild<QPushButton*>("chartGridSelect_0");
        if(!period||!first)return finish("blocked","Grid controls missing before measurement",4);
        period->setCurrentIndex(period->findData(interval));first->click();
    }
    host.resize(width,height);host.show();
    const auto fixtureError=[&]{return !activeHttp.error.isEmpty()?activeHttp.error:!passiveHttp.error.isEmpty()?passiveHttp.error:wire->error;};
    const auto stop=[&]{host.close();view->hide();grid->hide();model.setSelection({},interval);feed.stop();};
    QTimer ticks;ticks.setTimerType(Qt::PreciseTimer);ticks.setInterval(100);
    int tickEvents=0;QObject::connect(&ticks,&QTimer::timeout,&host,[&]{++tickEvents;wire->tick(now());});ticks.start();
    const bool ready=waitFor([&]{return !fixtureError().isEmpty()||(model.series().available&&wire->frames>0&&
        (panels==1||feed.observation(Passive,interval).series.available));},10000);
    if(!ready||!fixtureError().isEmpty()){
        ticks.stop();stop();report["active_http"]=activeHttp.snapshot();report["passive_http"]=passiveHttp.snapshot();report["socket"]=wire->snapshot();
        return finish("blocked",fixtureError().isEmpty()?"Synthetic fixture readiness timed out":fixtureError(),4);
    }
    QTest::qWait(2000); // warm-up excluded from the measured duration and deltas
    report["warmup_ms"]=sourceClock.elapsed();report["window_actual"]=QJsonObject{{"width",host.width()},{"height",host.height()}};
    report["baseline_active_http"]=activeHttp.snapshot();report["baseline_passive_http"]=passiveHttp.snapshot();report["baseline_socket"]=wire->snapshot();
    const int initialFrames=wire->frames,initialTicks=tickEvents,initialActiveRequests=activeHttp.requests,initialPassiveRequests=passiveHttp.requests;
    QElapsedTimer elapsed;elapsed.start();const double initialCpu=cpuMs();const auto baseline=sample(elapsed,initialCpu);report["baseline"]=baseline;
    QJsonArray observations;QJsonObject five,thirty;QEventLoop loop;QTimer sampler,deadline;
    const auto observe=[&]{const auto point=sample(elapsed,initialCpu);observations.append(point);
        if(elapsed.elapsed()>=300000&&five.isEmpty())five=point;
        if(elapsed.elapsed()>=1800000&&thirty.isEmpty())thirty=point;};
    sampler.setInterval(1000);QObject::connect(&sampler,&QTimer::timeout,&loop,[&]{observe();if(!fixtureError().isEmpty())loop.quit();});
    deadline.setSingleShot(true);deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);sampler.start();deadline.start(seconds*1000);loop.exec();
    sampler.stop();deadline.stop();ticks.stop();observe();const auto measuredMs=elapsed.elapsed();
    report["duration_actual_ms"]=measuredMs;report["samples"]=observations;report["final_sample"]=observations.last();
    report["tick_events_measured"]=tickEvents-initialTicks;report["socket_frames_measured"]=wire->frames-initialFrames;
    report["effective_frame_hz"]=measuredMs>0?1000.*(wire->frames-initialFrames)/measuredMs:0.;
    report["active_http_requests_measured"]=activeHttp.requests-initialActiveRequests;report["passive_http_requests_measured"]=passiveHttp.requests-initialPassiveRequests;
    report["active_http"]=activeHttp.snapshot();report["passive_http"]=passiveHttp.snapshot();report["socket_before_close"]=wire->snapshot();
    if(!five.isEmpty())report["five_minute"]=five;if(!thirty.isEmpty())report["thirty_minute"]=thirty;
    const bool milestones=!five.isEmpty()&&!thirty.isEmpty();report["long_duration_complete"]=milestones;
    if(milestones){
        report["rss_growth_5_to_30_minutes_bytes"]=thirty.value("rss_bytes").toInteger()-five.value("rss_bytes").toInteger();
        const double span=thirty.value("elapsed_ms").toDouble()-five.value("elapsed_ms").toDouble();
        report["cpu_percent_5_to_30_minutes"]=span>0?100.*(thirty.value("process_cpu_ms").toDouble()-five.value("process_cpu_ms").toDouble())/span:0.;
    }
    report["rss_growth_from_baseline_bytes"]=observations.last().toObject().value("rss_bytes").toInteger()-baseline.value("rss_bytes").toInteger();
    const auto finalSeries=model.series();report["active_closed_bars"]=finalSeries.candles.size();report["active_forming_available"]=finalSeries.forming.has_value();
    stop();const int requestsAfterClose=activeHttp.requests+passiveHttp.requests;QTest::qWait(500);
    report["socket_after_close"]=wire->snapshot();report["demands_after_close"]=feed.demandCount();
    report["requests_during_close_wait"]=activeHttp.requests+passiveHttp.requests-requestsAfterClose;
    const bool clean=!host.isVisible()&&!wire->active&&feed.demandCount()==0&&activeHttp.requests+passiveHttp.requests==requestsAfterClose;
    report["normal_shutdown_observed"]=clean;
    if(!fixtureError().isEmpty()||!clean||!finalSeries.available||!finalSeries.forming||initialCpu<0||baseline.value("rss_bytes").toInteger()<0)
        return finish("blocked",fixtureError().isEmpty()?"Fixture, resource sample or normal shutdown validation failed":fixtureError(),4);
    return finish(milestones?"measurement_complete":"short_harness_run_complete",milestones?
        "Synthetic measurement only; compare all eight conditions separately":"5/30-minute evidence incomplete; no long-run inference permitted",0);
}
