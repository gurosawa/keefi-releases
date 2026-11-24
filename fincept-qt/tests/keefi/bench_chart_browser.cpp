#include "ChartInstrumentPanelTestSupport.h"
// Opt-in display-list probe. All source responses and quotes are synthetic.
// No vault, account, live transport, websocket or notification sink is created.
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <functional>
using namespace keefi;
namespace {
QString asset(int row){return QString("S%1").arg(row,4,10,QChar('0'));}
QString market(int row){return "KRW-"+asset(row);}
class ProbeApp final:public QApplication {
public:
    using QApplication::QApplication;
    QObject* target=nullptr;QElapsedTimer* clock=nullptr;double firstPaintEnd=-1;
    bool notify(QObject* receiver,QEvent* event)override{
        if(receiver!=target||event->type()!=QEvent::Paint)return QApplication::notify(receiver,event);
        const bool result=QApplication::notify(receiver,event);
        if(clock&&firstPaintEnd<0)firstPaintEnd=clock->nsecsElapsed()/1e6;
        return result;
    }
};
class SyntheticPublicHttp final:public TradingTransport {
public:
    int rows=1000,sends=0;quint64 generation=0;
    void abort()override{++generation;}
    void send(quint64 token,const RequestSpec& request)override{
        if(request.authenticated||request.method!="GET"||request.url.host()!="api.upbit.com")qFatal("Unexpected non-public synthetic browser request");
        ++sends;QJsonArray body;const auto now=QDateTime::currentMSecsSinceEpoch();
        if(request.operation==Operation::Markets){
            for(int i=0;i<rows;++i)body.append(QJsonObject{{"market",market(i)},{"korean_name",QString("합성 종목 %1").arg(i,4,10,QChar('0'))},{"english_name","Synthetic "+asset(i)},{"market_event",QJsonObject{{"warning",false},{"caution",QJsonObject{}}}}});
        }else if(request.operation==Operation::Quote){
            for(const auto& code:QUrlQuery(request.url).queryItemValue("markets").split(',',Qt::SkipEmptyParts))body.append(QJsonObject{{"market",code},{"trade_price","1000"},{"prev_closing_price","990"},{"signed_change_rate","0.01"},{"acc_trade_price_24h","1000000"},{"timestamp",QString::number(now)}});
        }else qFatal("Unexpected synthetic browser operation");
        HttpResult response;response.status=200;response.body=QJsonDocument(body).toJson(QJsonDocument::Compact);const auto epoch=generation;
        QTimer::singleShot(0,this,[this,token,response,epoch]{if(epoch==generation)emit finished(token,response);});
    }
};
bool waitFor(const std::function<bool()>& ready,int timeout=3000){QElapsedTimer timer;timer.start();while(!ready()&&timer.elapsed()<timeout)QTest::qWait(1);return ready();}
QTreeWidgetItem* itemFor(QTreeWidget* tree,const QString& key){for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==key)return *it;return nullptr;}
double percentile(QList<double> values,double q){std::sort(values.begin(),values.end());return values.isEmpty()?-1:values[qBound(0,int(std::ceil(q*values.size()))-1,int(values.size())-1)];}
double median(QList<double> values){if(values.isEmpty())return -1;std::sort(values.begin(),values.end());const int middle=int(values.size())/2;return values.size()%2?values[middle]:(values[middle-1]+values[middle])/2.;}
}
int main(int argc,char** argv){
    ProbeApp app(argc,argv);
    QTemporaryDir profile;if(!profile.isValid())return 3;QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("KEEFI-Synthetic-Chart-Browser");QCoreApplication::setApplicationName("KEEFI Synthetic Chart Browser");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.filePath("settings"));
    KeefiFonts::registerBundledFonts();app.setFont(KeefiFonts::font(FontRole::Body15));
    QCommandLineParser parser;parser.addHelpOption();
    for(const auto& option:QList<QCommandLineOption>{{"rows","Synthetic public catalogue rows","count","1000"},{"rounds","Measured rounds; two warmups precede them","count","20"},{"output","Result JSON file","path"},{"capture","Optional synthetic PNG","path"},{"theme","light or dark","mode","dark"}})parser.addOption(option);
    parser.process(app);const int rows=parser.value("rows").toInt(),rounds=parser.value("rounds").toInt();
    if(rows<500||rows>2000||rounds<5||rounds>100||parser.value("output").isEmpty())return 2;
    if(!QStringList{"light","dark"}.contains(parser.value("theme")))return 2;
    KeefiTheme::instance()->setMode(parser.value("theme")=="dark"?ThemeMode::Dark:ThemeMode::Light);
    SyntheticPublicHttp http;http.rows=rows;PublicMarketCatalog catalog(&http);catalog.refresh();
    if(!waitFor([&]{return catalog.marketListAvailable()&&catalog.rows().size()==rows;}))qFatal("Synthetic catalogue did not become available");
    // Keep the real metadata/label contract, but stop the paced ticker pages
    // before either variant is measured. Quote UI changes below are explicit.
    catalog.setPresentationActive(false);
    if(catalog.busy()||!catalog.marketListAvailable()||catalog.rows().size()!=rows)qFatal("Cannot freeze the synthetic catalogue without losing metadata");
    WatchlistWorkspaceList lists;ChartWorkspace workspace(nullptr,nullptr);workspace.setWatchlistModel(&lists);workspace.indicatorPreferences()->markIntroSeen();workspace.setPublicNameSources(&catalog,nullptr,nullptr);
    workspace.setIdentity("coin:upbit:"+market(0));workspace.resize(1440,900);workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.show();workspace.activateWindow();
    auto* panel=keefi::test::openInstrumentPanel(workspace);
    if(!panel)qFatal("Instrument panel could not be opened");
    auto* tree=panel->findChild<QTreeWidget*>("chartWorkspaceWatchlist");auto* search=panel->findChild<QLineEdit*>("chartWorkspaceAsset");
    if(!tree||!search||!waitFor([&]{return tree->topLevelItemCount()==rows&&tree->viewport()->isVisible();}))qFatal("Whole catalogue is not displayed");
    QStringList lastDemand;int largestDemand=0;
    QObject::connect(&workspace,&ChartWorkspace::visibleIdentitiesChanged,&workspace,[&](const QStringList& keys){lastDemand=keys;largestDemand=qMax(largestDemand,int(keys.size()));if(keys.size()>=100)qFatal("Whole catalogue became a visible-row demand");});
    QTest::qWait(50);const auto initialState=lists.publicState();const int sendsBefore=http.sends;
    QJsonArray raw;QMap<QString,QMap<QString,QList<double>>> samples;
    const auto measure=[&](int round,const QString& operation,const std::function<void()>& action){
        QElapsedTimer clock;double eventLoop=-1;app.target=tree->viewport();app.clock=&clock;app.firstPaintEnd=-1;clock.start();
        QTimer::singleShot(0,&workspace,[&]{eventLoop=clock.nsecsElapsed()/1e6;});action();const double sync=clock.nsecsElapsed()/1e6;
        if(!waitFor([&]{return eventLoop>=0&&app.firstPaintEnd>=0;},1000))qFatal("Expected browser viewport paint did not complete");
        const double paint=app.firstPaintEnd;app.clock=nullptr;app.target=nullptr;
        const bool warmup=round<2;raw.append(QJsonObject{{"round",round},{"warmup",warmup},{"operation",operation},{"sync_ms",sync},{"event_loop_ms",eventLoop},{"viewport_first_paint_end_ms",paint}});
        if(!warmup){samples[operation]["sync_ms"].append(sync);samples[operation]["event_loop_ms"].append(eventLoop);samples[operation]["viewport_first_paint_end_ms"].append(paint);}
        if(lists.publicState()!=initialState||!lists.userLists().isEmpty())qFatal("Browsing added or modified a saved list");
    };
    int resets=0;QObject::connect(tree->model(),&QAbstractItemModel::modelReset,&workspace,[&]{++resets;});
    for(int round=0;round<rounds+2;++round){
        search->clear();QTest::qWait(10);
        measure(round,"search",[&]{search->setText(asset(round%2));});
        search->clear();QTest::qWait(10);
        const int next=workspace.selectedChartIdentity()=="coin:upbit:"+market(0)?1:0;const auto key="coin:upbit:"+market(next);auto* item=itemFor(tree,key);
        if(!item)qFatal("Selection target missing");tree->scrollToItem(item,QAbstractItemView::PositionAtTop);QTest::qWait(10);
        measure(round,"row_selection",[&]{QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(item).center(),0);});
        if(workspace.selectedChartIdentity()!=key)qFatal("Row activation did not select the exact identity");
        const auto selected=tree->currentIndex();const int scroll=tree->verticalScrollBar()->value();
        measure(round,"quote_update",[&]{WatchlistRowState state;state.price=QString::number(1000+round);state.change="+1.00%";lists.setRowState(WatchlistWorkspaceList::identityFromKey(key),state);});
        if(tree->currentIndex()!=selected||tree->verticalScrollBar()->value()!=scroll||resets)qFatal("Quote update reset row identity, focus or viewport");
    }
    QJsonObject summary;
    for(auto operation=samples.cbegin();operation!=samples.cend();++operation){QJsonObject metrics;
        for(auto metric=operation->cbegin();metric!=operation->cend();++metric)metrics.insert(metric.key(),QJsonObject{{"n",metric->size()},{"median",median(*metric)},{"p95",percentile(*metric,.95)},{"maximum",percentile(*metric,1)}});
        summary.insert(operation.key(),metrics);
    }
    QJsonObject result{{"format","keefi-chart-public-catalogue-probe/1"},{"synthetic_only",true},{"real_network_requests",0},{"qt_platform",QGuiApplication::platformName()},{"qt_version",QString(QT_VERSION_STR)},{"public_catalogue_rows",rows},{"saved_list_rows",0},{"measured_rounds",rounds},{"discarded_warmup_rounds",2},{"window_width",workspace.width()},{"window_height",workspace.height()},{"fake_http_sends_before",sendsBefore},{"fake_http_sends_after",http.sends},{"largest_visible_row_demand",largestDemand},{"model_resets",resets},{"summary",summary},{"raw",raw},
        {"limits","Display-list search, selection and quote UI only; selected chart has no candles/source transport. Paint is target QWidget paint-event completion, not OS compositor or network latency."}};
    if(catalog.presentationActive()||catalog.busy()||http.sends!=sendsBefore)qFatal("The supposedly static catalogue issued another request during measurement");
    result.insert("catalogue_presentation_paused",true);result.insert("percentile_method","p95 nearest-rank; median averages the two middle samples when n is even");
    QFile output(parser.value("output"));if(!output.open(QIODevice::WriteOnly)||output.write(QJsonDocument(result).toJson(QJsonDocument::Indented))<0)return 3;output.close();
    if(!parser.value("capture").isEmpty()&&!panel->grab().save(parser.value("capture")))return 4;
    return 0;
}
