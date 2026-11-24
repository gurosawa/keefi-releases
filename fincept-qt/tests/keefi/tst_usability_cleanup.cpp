#include "ChartInstrumentPanelTestSupport.h"
#include "keefi/AccountModel.h"
#include "keefi/BackgroundTray.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <QTextDocument>
#include <QTreeWidget>

using namespace keefi;
namespace {
class NoTray final:public BackgroundTray {
  public:
    bool available()const override{return false;}
    bool visible()const override{return false;}
    void setMenu(QMenu*)override{}
    void show()override{}
    void hide()override{}
};
class CandlesTransport final:public TradingTransport {
  public:
    quint64 token=0;int sent=0;QUrl lastUrl;
    void send(quint64 t,const RequestSpec& request)override{QVERIFY(!request.authenticated);QVERIFY(MarketCandles::isRequest(request));token=t;++sent;lastUrl=request.url;}
    void abort()override{}
};
class IdlePublicTransport final:public TradingTransport {
  public:
    int sent=0;
    void send(quint64,const RequestSpec& request)override{QVERIFY(!request.authenticated);QCOMPARE(request.method,QByteArray("GET"));++sent;}
    void abort()override{}
};
class FixturePublicTransport final:public TradingTransport {
  public:
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QCOMPARE(request.url.host(),QString("api.upbit.com"));
        HttpResult result;
        if(request.operation==Operation::Markets){
            result.status=200;
            result.body=R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin"},{"market":"KRW-ETH","korean_name":"이더리움","english_name":"Ethereum"},{"market":"KRW-SOL","korean_name":"솔라나","english_name":"Solana"}])";
        }else{
            QCOMPARE(request.operation,Operation::Quote);
            // Feed the owner of the displayed rows. Study completion and the
            // quote timer may rebuild those rows after the fixture is shown.
            const QStringList markets{"KRW-BTC","KRW-ETH","KRW-SOL"};
            const QStringList prices{"110000000","3850000","238500"};
            const QStringList rates{"0.0125","-0.0042","0.0208"};
            QJsonArray rows;
            for(const auto& market:QUrlQuery(request.url).queryItemValue("markets").split(',')){
                const int index=markets.indexOf(market);QVERIFY(index>=0);
                rows.append(QJsonObject{{"market",market},{"trade_price",prices[index]},
                    {"prev_closing_price",prices[index]},{"signed_change_rate",rates[index]},
                    {"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch())}});
            }
            result.status=200;result.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
        }
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
    void abort()override{}
};
constexpr qint64 Observed=1728000000000+250*60000+10000;
HttpResult completedCandles(const QString& scale="1"){
    QJsonArray rows;const auto end=Observed/60000*60000;
    for(int i=0;i<81;++i){const auto at=end-(81-i)*60000;const QString price=i==80?"110":"100";
        rows.append(QJsonObject{{"market","KRW-BTC"},{"unit","1"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(at+1000)},{"opening_price",decimal::multiply(price,scale)},{"trade_price",decimal::multiply(price,scale)},
            {"high_price",decimal::multiply(i==11?QString("120"):decimal::add(price,"1"),scale)},{"low_price",decimal::multiply(i==10?QString("90"):decimal::subtract(price,"1"),scale)},{"candle_acc_trade_volume","2"}});}
    HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();return result;
}
QString plain(const QLabel* label){QTextDocument document;document.setHtml(label->text());return label->textFormat()==Qt::RichText?document.toPlainText():label->text();}
}
class UsabilityCleanupTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
  private slots:
    void initTestCase(){
        QVERIFY(settings_.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic Usability");QCoreApplication::setApplicationName("Synthetic Usability");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());
    }
    void cleanup(){KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(ThemeMode::System);QSettings().remove("display/windowSize/v1");}
    void defaultWindowKeepsTheRatioAndRestoresTheUsersSize(){
        QTemporaryDir profile;QVERIFY(profile.isValid());NoTray tray;Vault vault(profile.filePath("synthetic-nonexistent-vault"));AccountModel accounts(&vault);
        const auto available=QGuiApplication::primaryScreen()->availableGeometry().size();QSize chosen;
        {
            LocalWindow window(vault,accounts,nullptr,&tray);window.show();QCoreApplication::processEvents();
            if(available.width()>=1840&&available.height()>=1165)QCOMPARE(window.size(),QSize(1800,1125));
            else if(window.width()>720&&window.height()>640)QVERIFY2(qAbs(window.width()*5-window.height()*8)<=8,"The default window must shrink at 16:10, unless the 720x640 minimum takes priority");
            QVERIFY(window.width()>=720);QVERIFY(window.height()>=640);
            chosen=QSize(qMax(720,qMin(1000,available.width()-40)),qMax(640,qMin(760,available.height()-40)));
            window.resize(chosen);window.close();
        }
        QCOMPARE(QSettings().value("display/windowSize/v1").toSize(),chosen);
        LocalWindow reopened(vault,accounts,nullptr,&tray);QCOMPARE(reopened.size(),chosen);QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
    void currentQuoteHeaderAndSearchDoNotDependOnCompletedCandles(){
        ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
        auto* lists=workspace.watchlistModel();const WatchlistIdentity identity{"coin","upbit","KRW-BTC"};
        const auto list=lists->createList("합성 공개 관심");QVERIFY(lists->addIdentity(list,identity));lists->useList(list);
        WatchlistRowState quote;quote.price="107780000.123456789";quote.change="+1.25%";quote.evidence="https://api.upbit.com/v1/ticker";lists->setRowState(identity,quote);
        const auto stored=lists->publicState();workspace.resize(1800,1125);workspace.show();QCoreApplication::processEvents();
        auto* price=workspace.findChild<QLabel*>("chartCurrentPrice");QVERIFY(price);QCOMPARE(price->text(),QString("107,780,000.123456789"));
        auto* title=workspace.findChild<QLabel*>("marketCandlesTitle");QVERIFY(title);QVERIFY(plain(title).contains("BTC/KRW"));auto* toolbar=workspace.findChild<QWidget*>("chartToolbar");QVERIFY(toolbar);
        QVERIFY(toolbar->isAncestorOf(title));QVERIFY(toolbar->isAncestorOf(price));
        const QRect titleRect(title->mapTo(toolbar,QPoint()),title->size()),priceRect(price->mapTo(toolbar,QPoint()),price->size());
        QVERIFY(titleRect.right()<=priceRect.left());QVERIFY(qAbs(titleRect.center().y()-priceRect.center().y())<=2);
        auto* panel=test::openInstrumentPanel(workspace);QVERIFY(panel);
        auto* watch=panel->findChild<QPushButton*>("chartBrowserScope_watch");QVERIFY(watch);QTest::mouseClick(watch,Qt::LeftButton);QVERIFY(watch->isChecked());
        auto* search=panel->findChild<QLineEdit*>("chartWorkspaceAsset");QVERIFY(search);QCOMPARE(search->placeholderText(),QStringLiteral("이름 또는 코드 · BTC, 삼성전자, AAPL"));QVERIFY(search->text().isEmpty());
        QCOMPARE(search->accessibleName(),QStringLiteral("종목 이름 또는 코드 검색"));QVERIFY(search->isVisible());
        QCOMPARE(search->height(),32);
        for(const auto* name:{"chartBrowserMarket_upbit","chartBrowserMarket_binance","chartBrowserMarket_kr","chartBrowserMarket_us"}){
            auto* market=panel->findChild<QPushButton*>(name);QVERIFY(market);QVERIFY(market->isVisible());QCOMPARE(market->height(),24);
        }
        for(auto* button:workspace.findChildren<QPushButton*>())QVERIFY(button->text()!=QStringLiteral("BTC · 업비트 KRW"));
        auto* rows=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(rows);QCOMPARE(rows->topLevelItemCount(),1);
        QVERIFY(!rows->topLevelItem(0)->text(0).contains(QStringLiteral("업비트")));QVERIFY(rows->topLevelItem(0)->text(1).contains("107,780,000.123456789"));
        auto* markets=workspace.findChild<QLabel*>("chartWorkspaceMarkets");QVERIFY(markets);QCOMPARE(markets->text().count(QStringLiteral("업비트")),1);
        QCOMPARE(lists->rowState(identity).price,quote.price);QCOMPARE(lists->rowState(identity).evidence,quote.evidence);QCOMPARE(lists->publicState(),stored);
    }
    void occupiedSidePanelIsReachableAtEveryWidth_data(){
        QTest::addColumn<int>("width");QTest::addColumn<int>("height");
        QTest::newRow("wide")<<1800<<1125;QTest::newRow("desktop")<<1440<<900;
        QTest::newRow("compact-desktop")<<1280<<800;QTest::newRow("medium")<<1100<<900;
        QTest::newRow("stacked-boundary")<<1099<<900;QTest::newRow("narrow")<<720<<900;
    }
    void occupiedSidePanelIsReachableAtEveryWidth(){
        QFETCH(int,width);QFETCH(int,height);CandlesTransport http;MarketCandles model(&http,std::unique_ptr<PublicCandleSocket>{},[]{return Observed;});
        ChartWorkspace workspace(&model,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setTimeframe(60000);workspace.resize(width,height);workspace.show();QVERIFY(http.token);
        emit http.finished(http.token,completedCandles());QVERIFY(model.series().available);QTRY_VERIFY(workspace.chartStudies().scorecard.available);QCoreApplication::processEvents();
        const auto original=model.series();const auto requests=http.sent;
        auto* scroll=workspace.findChild<QScrollArea*>("chartWorkspaceScroll");auto* panel=workspace.findChild<QWidget*>("chartWorkspacePositions");auto* chart=workspace.findChild<QWidget*>("chartWorkspaceChartPane");
        auto* instrumentPanel=workspace.findChild<QWidget*>("chartInstrumentPanel");auto* disclosure=workspace.findChild<QPushButton*>("chartDetailsToggle");
        QVERIFY(scroll&&panel&&chart&&instrumentPanel&&disclosure);QVERIFY(!instrumentPanel->isVisible());QVERIFY(!panel->isVisible());QVERIFY(!disclosure->isChecked());QVERIFY(disclosure->isVisible());
        QVERIFY(workspace.width()<=width);QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        scroll->ensureWidgetVisible(disclosure,0,0);QCoreApplication::processEvents();
        QVERIFY(scroll->viewport()->rect().contains(QRect(disclosure->mapTo(scroll->viewport(),QPoint()),disclosure->size())));
        QTest::mouseClick(disclosure,Qt::LeftButton);QTRY_VERIFY(panel->isVisible());QVERIFY(disclosure->isChecked());QCoreApplication::processEvents();
        const auto chartTop=chart->mapTo(scroll->widget(),QPoint());const auto panelTop=panel->mapTo(scroll->widget(),QPoint());
        QCOMPARE(panelTop.x(),chartTop.x());QVERIFY(panelTop.y()>=chartTop.y()+chart->height());
        scroll->ensureWidgetVisible(panel,0,0);QCoreApplication::processEvents();
        const QRect panelRect(panel->mapTo(scroll->viewport(),QPoint()),panel->size());QVERIFY(panelRect.left()>=0);QVERIFY(panelRect.right()<scroll->viewport()->width());
        QVERIFY(scroll->viewport()->rect().intersects(panelRect));
        // C12 starts with current status; historical results occupy the same
        // panel only after the visible scorecard control is activated.
        auto* summary=panel->findChild<QWidget*>("chartIndicatorSummary");
        auto* toggle=panel->findChild<QPushButton*>("chartIndicatorScoreToggle");
        auto* scoreScroll=panel->findChild<QScrollArea*>("chartIndicatorScoreBody");
        auto* score=panel->findChild<QLabel*>("chartStudyScorecard");
        QVERIFY(summary&&toggle&&scoreScroll&&score);QVERIFY(summary->isVisible());
        QVERIFY(!scoreScroll->isVisible());QVERIFY(!score->isVisible());
        for(const auto* name:{"chartWorkspaceUnlock","chartIndicatorSummary","chartIndicatorScoreToggle"}){
            auto* content=panel->findChild<QWidget*>(name);QVERIFY(content);QVERIFY(content->isVisible());
            scroll->ensureWidgetVisible(content,0,0);QCoreApplication::processEvents();
            const QRect contentRect(content->mapTo(scroll->viewport(),QPoint()),content->size());
            QVERIFY2(scroll->viewport()->rect().contains(contentRect),name);
        }
        QTest::mouseClick(toggle,Qt::LeftButton);QTRY_VERIFY(scoreScroll->isVisible());
        QVERIFY(!summary->isVisible());QVERIFY(score->isVisible());QVERIFY(!score->text().isEmpty());
        QVERIFY(score->width()>0&&score->height()>0);
        // Reach the actual populated label through BOTH scroll containers.
        // Scrolling only the workspace could otherwise hide a clipped inner row.
        scroll->ensureWidgetVisible(scoreScroll,0,0);QCoreApplication::processEvents();
        scoreScroll->ensureWidgetVisible(score,0,0);scroll->ensureWidgetVisible(score,0,0);QCoreApplication::processEvents();
        const QRect innerScoreRect(score->mapTo(scoreScroll->viewport(),QPoint()),score->size());
        const QRect outerScoreRect(score->mapTo(scroll->viewport(),QPoint()),score->size());
        QVERIFY2(scoreScroll->viewport()->rect().contains(innerScoreRect),"The score must be fully reachable inside its own scroll area");
        QVERIFY2(scroll->viewport()->rect().contains(outerScoreRect),"The populated score must also be reachable inside the workspace viewport");
        QCOMPARE(scoreScroll->horizontalScrollBar()->maximum(),0);QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        scroll->ensureWidgetVisible(toggle,0,0);QCoreApplication::processEvents();
        const QRect toggleRect(toggle->mapTo(scroll->viewport(),QPoint()),toggle->size());QVERIFY(scroll->viewport()->rect().contains(toggleRect));
        QTest::mouseClick(toggle,Qt::LeftButton);QTRY_VERIFY(summary->isVisible());QVERIFY(!scoreScroll->isVisible());
        const auto geometry=[&](const char* phase){
            const auto rect=[](const QRect& value){return QJsonArray{value.x(),value.y(),value.width(),value.height()};};
            const QRect actual(summary->mapTo(scroll->viewport(),QPoint()),summary->size());
            const QJsonObject state{{"phase",phase},{"width",width},{"height",height},
                {"summary",rect(actual)},{"viewport",rect(scroll->viewport()->rect())},
                {"content",rect(scroll->widget()->geometry())},{"panel",rect(panel->geometry())},
                {"status_body",rect(summary->parentWidget()->geometry())},
                {"scroll_value",scroll->verticalScrollBar()->value()},{"scroll_max",scroll->verticalScrollBar()->maximum()},
                {"fully_contained",scroll->viewport()->rect().contains(actual)},
                {"focus",QApplication::focusWidget()?QApplication::focusWidget()->objectName():QString{}}};
            qInfo().noquote()<<"summary-geometry "<<QJsonDocument(state).toJson(QJsonDocument::Compact);
        };
        geometry("returned-visible-before-layout");
        scroll->ensureWidgetVisible(summary,0,0);geometry("immediate-scroll");
        QCoreApplication::processEvents();geometry("after-immediate-layout");
        // Visibility changes synchronously; nested geometry is posted. Resolve
        // that layout before requesting the same strict whole-label visibility.
        QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QCoreApplication::processEvents();
        geometry("settled-before-scroll");scroll->ensureWidgetVisible(summary,0,0);
        QCoreApplication::processEvents();geometry("settled-after-scroll");
        const QRect summaryRect(summary->mapTo(scroll->viewport(),QPoint()),summary->size());QVERIFY(scroll->viewport()->rect().contains(summaryRect));
        scroll->ensureWidgetVisible(disclosure,0,0);QCoreApplication::processEvents();QTest::mouseClick(disclosure,Qt::LeftButton);QTRY_VERIFY(!panel->isVisible());
        QCOMPARE(http.sent,requests);QCOMPARE(model.series().sourceUrl,original.sourceUrl);QCOMPARE(model.series().candles.size(),original.candles.size());QCOMPARE(workspace.chartStudies().scorecard.samples,1);
    }
    void chartPresentationMatrix_data(){
        QTest::addColumn<int>("width");QTest::addColumn<int>("height");QTest::addColumn<bool>("dark");
        for(bool dark:{false,true})for(const auto size:{QSize(1800,1125),QSize(1440,900),QSize(720,900)})
            QTest::newRow(qPrintable(QString("%1-%2x%3").arg(dark?"dark":"light").arg(size.width()).arg(size.height())))<<size.width()<<size.height()<<dark;
    }
    void chartPresentationMatrix(){
        QFETCH(int,width);QFETCH(int,height);QFETCH(bool,dark);
        QTemporaryDir profile;QVERIFY(profile.isValid());NoTray tray;Vault vault(profile.filePath("synthetic-chart-preview-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        QSettings display(profile.filePath("synthetic-chart-display.ini"),QSettings::IniFormat);
        WatchlistWorkspaceList saved;const auto id=saved.createList(QStringLiteral("관심 종목"));
        const QList<WatchlistIdentity> identities{{"coin","upbit","KRW-BTC"},{"coin","upbit","KRW-ETH"},{"coin","upbit","KRW-SOL"}};
        for(const auto& identity:identities)QVERIFY(saved.addIdentity(id,identity));saved.useList(id);display.setValue("chart/watchlists/v2",saved.publicState());
        CandlesTransport http;IdlePublicTransport metadata;FixturePublicTransport catalogueHttp;
        PublicMarketCatalog catalogue(&catalogueHttp);catalogue.refresh();QTRY_VERIFY(catalogue.marketListAvailable());QTRY_VERIFY(!catalogue.busy());
        QCOMPARE(catalogue.verifiedMarkets(),QStringList({"KRW-BTC","KRW-ETH","KRW-SOL"}));
        for(const auto& row:catalogue.rows())QVERIFY(row.priced);
        MarketCandles model(&http,std::unique_ptr<PublicCandleSocket>{},[]{return Observed;});
        LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&display);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalogue,false);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&model);
        auto* workspace=window.findChild<ChartWorkspace*>("chartWorkspace");QVERIFY(workspace);workspace->indicatorPreferences()->markIntroSeen();workspace->setTimeframe(60000);
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");QVERIFY(pages);pages->setCurrentWidget(workspace);
        KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);window.resize(width,height);window.show();QCoreApplication::processEvents();QTRY_VERIFY(!catalogue.busy());QVERIFY(http.token);
        QCOMPARE(QUrlQuery(http.lastUrl).queryItemValue("market"),QString("KRW-BTC"));
        emit http.finished(http.token,completedCandles("1000000"));QVERIFY(model.series().available);
        // Exercise the same refresh that a late study worker or the quote timer delivers.
        emit workspace->studiesChanged();QCoreApplication::processEvents();
        QCOMPARE(window.size(),QSize(width,height));QCOMPARE(workspace->findChild<QLabel*>("chartCurrentPrice")->text(),QString("110,000,000"));
        auto* change=workspace->findChild<QLabel*>("chartCurrentPriceReference");QVERIFY(change&&change->isVisible());
        const auto changeImage=change->grab().toImage();const auto movement=MovementColorPolicy::colorFor(change,MovementDirection::Up);int movementPixels=0;
        for(int y=0;y<changeImage.height();++y)for(int x=0;x<changeImage.width();++x)if(changeImage.pixelColor(x,y)==movement)++movementPixels;
        QVERIFY2(movementPixels>0,"The current quote change must paint with the existing movement policy in both themes");
        auto* scroll=workspace->findChild<QScrollArea*>("chartWorkspaceScroll");QVERIFY(scroll);QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        static const QRegularExpression forbidden("\\[[0-9]+\\]|[a-z][a-z0-9+.-]*://|www\\.|(?:stock|coin):|SYNTHETIC_PRIVATE_SCOPE");
        const auto verifyVisibleText=[](QWidget& surface){
            for(auto* widget:surface.findChildren<QWidget*>())if(widget->isVisible()){
                const auto label=qobject_cast<QLabel*>(widget);const auto text=label?plain(label):QString{};
                QVERIFY2(!forbidden.match(text+" "+widget->toolTip()+" "+widget->accessibleName()).hasMatch(),qPrintable(widget->objectName()+": "+text));
            }
        };
        verifyVisibleText(window);
        const auto out=qEnvironmentVariable("KEEFI_USABILITY_P2_RENDER_DIR");
        if(!out.isEmpty()){
            QVERIFY(QDir().mkpath(out));QCoreApplication::processEvents();
            const auto stem=QString("/chart-%1-%2x%3").arg(dark?"dark":"light").arg(width).arg(height);
            QVERIFY(window.grab().save(out+stem+".png"));
        }
        auto* instrumentPanel=test::openInstrumentPanel(*workspace);QVERIFY(instrumentPanel);
        verifyVisibleText(*instrumentPanel);
        if(!out.isEmpty()&&width==720){
            const auto stem=QString("/chart-%1-%2x%3").arg(dark?"dark":"light").arg(width).arg(height);
            QVERIFY(instrumentPanel->grab().save(out+stem+"-lower.png"));
        }
        QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
};
QTEST_MAIN(UsabilityCleanupTest)
#include "tst_usability_cleanup.moc"
