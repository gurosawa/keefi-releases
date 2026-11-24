#include "fixtures/ProviderReadFixture.h"
#include "keefi/AssetsLedger.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/WorldMarkets.h"
#include "keefi/TradingTransport.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketMetrics.h"
#include "keefi/MarketOverview.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/PortfolioController.h"
#include "keefi/AccountModel.h"
#include "keefi/Vault.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollBar>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
#include <QTextDocument>
#include <QUrlQuery>
using namespace keefi;
namespace {
class Wire final:public TradingTransport {
  public:
    quint64 token=0;QList<RequestSpec> sent;
    void send(quint64 next,const RequestSpec& r)override{token=next;sent.append(r);}
    void abort()override{}
    void answer(const QByteArray& bytes,int status=200){HttpResult r;r.status=status;r.body=bytes;emit finished(token,r);}
};
class Socket final:public PublicTickerSocket {
  public:
    void open(const QUrl&)override{}void send(const QByteArray&)override{}void ping()override{}void abort()override{}
};
class CatalogWire final:public TradingTransport {
  public:
    QString tether="1400";
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        HttpResult response;response.status=200;
        if(request.operation==Operation::Markets)response.body=R"([{"market":"KRW-BTC","korean_name":"합성 비트코인","english_name":"Fixture BTC"},{"market":"KRW-ETH","korean_name":"합성 이더리움","english_name":"Fixture ETH"},{"market":"KRW-USDT","korean_name":"합성 테더","english_name":"Fixture USDT"}])";
        else {QJsonArray rows;const auto now=QDateTime::currentMSecsSinceEpoch();for(const auto& market:QUrlQuery(request.url).queryItemValue("markets").split(','))
            rows.append(QJsonObject{{"market",market},{"trade_price",market=="KRW-USDT"?tether:market=="KRW-BTC"?QString("141400000"):QString("3000000")},{"prev_closing_price","140000000"},{"signed_change_rate","0.01"},{"timestamp",QString::number(now)}});
            response.body=QJsonDocument(rows).toJson();}
        QTimer::singleShot(0,this,[this,token,response]{emit finished(token,response);});
    }
};
QByteArray fx(qint64 now){
    // Hana Bank's USD/KRW posting one minute before the fixture clock (Naver Securities shape).
    const auto at=QDateTime::fromMSecsSinceEpoch(now-60000,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd'T'HH:mm:ss")+"+09:00";
    return QJsonDocument(QJsonObject{{"isSuccess",true},{"result",QJsonObject{{"categoryType","exchange"},{"reutersCode","FX_USDKRW"},{"closePrice","1,400.00"},
        {"fluctuations","1.40"},{"fluctuationsRatio","0.10"},{"marketStatus","OPEN"},{"localTradedAt",at},{"stockExchangeType",QJsonObject{{"nameKor","하나은행"},{"delayTime",0}}}}}}).toJson();
}
void price(Wire& up,Wire& bn,qint64 now,const QString& won="141400000"){
    up.answer(QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"trade_price",won},{"timestamp",QString::number(now)}}}).toJson());
    bn.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastPrice","100000"},{"closeTime",QString::number(now)}}).toJson());
}
QString plain(QLabel* label){return label?label->text():QString{};}
// Naver's Hana Bank 원/달러 posting, one minute before the fixture clock.
QByteArray bankFx(qint64 now){
    const auto at=QDateTime::fromMSecsSinceEpoch(now-60000,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd'T'HH:mm:ss")+"+09:00";
    return QJsonDocument(QJsonObject{{"isSuccess",true},{"result",QJsonObject{{"categoryType","exchange"},{"reutersCode","FX_USDKRW"},{"closePrice","1,400.00"},
        {"fluctuations","1.40"},{"fluctuationsRatio","0.10"},{"marketStatus","OPEN"},{"localTradedAt",at},{"stockExchangeType",QJsonObject{{"nameKor","하나은행"},{"delayTime",0}}}}}}).toJson();
}
const char* const TodayRows[]{"gap","tether","treasury","jgb","spread","spx","ndx","kospi","nikkei","hsi","ftse",
    "usdkrw","jpykrw","cnykrw","usdjpy","dxy","audjpy","gold","silver","wti"};


}
class TodayPremiumWindowTest:public QObject {
    Q_OBJECT
  private slots:
    void initTestCase(){QVERIFY(KeefiFonts::registerBundledFonts());QApplication::setFont(KeefiFonts::font(FontRole::Body15));}
    void yesterdayBalanceFromLedgerReachesTodayAndClearsOnLock() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings profile(dir.filePath("today.ini"), QSettings::IniFormat);
        Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Synthetic Today fixture 106!"));
        test::ReadFixture account("upbit");
        QVERIFY(vault.addAccount(account.account, account.credentials, StorageMode::EncryptedVault));
        test::ProviderReadTransport balances;
        balances.provider = "upbit";
        balances.transform = [](const RequestSpec& request, HttpResult& response) {
            if (request.operation == Operation::Account)
                response.body = R"([{"currency":"KRW","balance":"90000","locked":"0","avg_buy_price":"0","avg_buy_price_modified":false,"unit_currency":"KRW"}])";
        };
        PortfolioController::Factories factories;
        factories.trading = [&](const AccountRecord&) { return &balances; };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        AccountModel accounts(&vault);
        CatalogWire catalogWire;
        PublicMarketCatalog catalog(&catalogWire);
        Wire publicMetrics;
        MarketMetrics metrics(&publicMetrics);
        LocalWindow window(vault, accounts);
        window.setChartSettings(&profile);
        window.setPortfolioController(&portfolio);
        window.setSecurityStatus(true);
        window.setPublicMarketCatalog(&catalog, false);
        window.setMarketMetrics(&metrics, nullptr, nullptr, nullptr);
        window.resize(1440, 980);
        window.show();
        QTRY_VERIFY(!portfolio.snapshots().isEmpty());
        QTRY_COMPARE(portfolio.checkpointRecords(), PortfolioController::RecordCheckpoint::Ready);

        // Use the connection and binding emitted by the production balance adapter.
        auto yesterday = portfolio.snapshots().first();
        QCOMPARE(yesterday.components.size(), 1);
        QCOMPARE(yesterday.components.first().asset, QString("KRW"));
        yesterday.id += "-yesterday";
        yesterday.timeMs = portfolioHistoryDayStart(yesterday.timeMs) - 60000;
        yesterday.evidence.sourceTimeMs = yesterday.evidence.receivedAtMs = yesterday.timeMs;
        yesterday.components[0].quantity = "100000";
        yesterday.components[0].evidence = yesterday.evidence;
        QString error;
        QVERIFY2(portfolio.historyLedger()->append({yesterday}, {}, {}, &error), qPrintable(error));

        auto* change = window.findChild<QLabel*>("todayPrivateChange");
        auto* total = window.findChild<QLabel*>("todayPrivateTotal");
        QVERIFY(change && total);
        QTRY_VERIFY(total->isVisible() && total->text() == "90,000");
        QTRY_VERIFY2(change->isVisible() && change->text().contains(QRegularExpression(QStringLiteral("[-−]10(?:\\.0+)?%"))),
                     qPrintable(change->text()));
        QCOMPARE(balances.sent.count(Operation::Account), 1);
        const auto captures = qEnvironmentVariable("KEEFI_DEV86_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(window.grab().save(QDir(captures).filePath("today-previous-day.png")));
        }
        vault.lock();
        QTRY_VERIFY(change->text().isEmpty());
        QVERIFY(total->text().isEmpty());
    }
    void fixedBtcStateAndLayout_data(){
        QTest::addColumn<bool>("dark");QTest::addColumn<int>("windowWidth");
        QTest::newRow("fixture-light-wide")<<false<<1600;
        QTest::newRow("fixture-dark-wide")<<true<<1600;
        QTest::newRow("fixture-light-narrow")<<false<<1120;
        QTest::newRow("fixture-dark-narrow")<<true<<1120;
    }
    void fixedBtcStateAndLayout(){
        QFETCH(bool,dark);QFETCH(int,windowWidth);QTemporaryDir dir;QVERIFY(dir.isValid());
        // Construction also creates default QSettings before the explicit
        // fixture profile is bound. Isolate that initial read as well.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
        QSettings profile(dir.filePath("synthetic-today.ini"),QSettings::IniFormat);
        Vault vault(dir.filePath("not-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        CatalogWire catalogWire;PublicMarketCatalog catalog(&catalogWire);
        Wire selectedMetrics,metadata,up,bn,fxWire,worldQuotes,worldEcb,worldMof;MarketMetrics metrics(&selectedMetrics);
        PublicQuoteStream upStream(PublicQuoteStream::Provider::Upbit,std::make_unique<Socket>()),bnStream(PublicQuoteStream::Provider::Binance,std::make_unique<Socket>());
        qint64 now=QDateTime::currentMSecsSinceEpoch();TodayBtcPremium premium(&up,&bn,&fxWire,nullptr,[&]{return now;});
        WorldMarkets world(&worldQuotes,&worldEcb,&worldMof,nullptr,[&]{return now;});
        LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
        window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics,nullptr,&upStream,&bnStream);window.setTodayBtcPremium(&premium);window.setWorldMarkets(&world);
        window.resize(windowWidth,980);window.show();
        QCOMPARE(metrics.derivatives().symbol,QStringLiteral("BTCUSDT"));
        QVERIFY(!selectedMetrics.sent.isEmpty());
        catalog.refresh();QTRY_VERIFY(catalog.marketListAvailable());
        // LocalWindow and setChartSettings restore saved appearance. Choose
        // the requested capture theme after those steps, then verify pixels.
        const auto requestedTheme=dark?ThemeMode::Dark:ThemeMode::Light;
        KeefiTheme::instance()->setMode(requestedTheme);QCoreApplication::processEvents();
        QCOMPARE(KeefiTheme::instance()->mode(),requestedTheme);QCOMPARE(KeefiTheme::instance()->isDark(),dark);
        auto* lightChoice=window.findChild<QPushButton*>("themeLight");auto* darkChoice=window.findChild<QPushButton*>("themeDark");
        QVERIFY(lightChoice&&darkChoice);QCOMPARE(lightChoice->isChecked(),!dark);QCOMPARE(darkChoice->isChecked(),dark);
        auto* gap=window.findChild<TodayLedgerRow*>("todayMetric_gap");auto* first=window.findChild<QWidget*>("todayEmptyStrip");auto* tabs=window.findChild<QWidget*>("exploreViewTabs");
        QVERIFY(gap&&first&&tabs);QVERIFY(first->isVisibleTo(&window));QVERIFY(premium.active());QVERIFY(!up.sent.isEmpty()&&!bn.sent.isEmpty()&&!fxWire.sent.isEmpty());
        QCOMPARE(gap->data().title,QStringLiteral("BTC 김프"));QCOMPARE(gap->data().value,QStringLiteral("—"));QCOMPARE(gap->data().changeText,QStringLiteral("받는 중"));
        QVERIFY(gap->data().publicInformation.contains(QStringLiteral("원·달러 환율 수신 전")));QTRY_VERIFY(world.active());
        const auto firstPosition=first->mapTo(&window,QPoint()),tabsPosition=tabs->mapTo(&window,QPoint());
        const auto capture=[&](const QString& stage){
            QCoreApplication::processEvents();
            QCOMPARE(KeefiTheme::instance()->mode(),requestedTheme);QCOMPARE(KeefiTheme::instance()->isDark(),dark);
            QCOMPARE(lightChoice->isChecked(),!dark);QCOMPARE(darkChoice->isChecked(),dark);
            auto* metricPanel=window.findChild<QWidget*>("todayMetricPanel");QVERIFY(metricPanel);
            QCOMPARE(metricPanel->findChildren<TodayLedgerRow*>().size(),int(std::size(TodayRows)));
            for(auto* card:metricPanel->findChildren<TodayLedgerRow*>()){
                const QRect cardBounds(card->mapTo(metricPanel,QPoint()),card->size());
                QVERIFY2(metricPanel->rect().contains(cardBounds),qPrintable(card->objectName()+" must stay inside the compact panel"));
                for(const auto* name:{"metricTitle","metricValue","metricChange"})if(auto* label=card->findChild<QLabel*>(name)){
                    const QRect labelBounds(label->mapTo(card,QPoint()),label->size());
                    QVERIFY2(card->rect().contains(labelBounds),qPrintable(card->objectName()+"/"+name+" must not be clipped by compact layout"));
                }
            }
            const auto frame=window.grab();const auto pixels=frame.toImage();
            // Eight pixels inside the panel's side margin is away from its
            // border, rounded corners and children. Read the actual capture.
            const auto probe=first->mapTo(&window,QPoint(8,first->height()/2));
            const auto sample=QPoint(qRound(probe.x()*frame.devicePixelRatio()),qRound(probe.y()*frame.devicePixelRatio()));
            QVERIFY(pixels.rect().contains(sample));const auto actualBackground=pixels.pixelColor(sample);
            const auto expectedBackground=KeefiTheme::colorsFor(dark).bg;
            QCOMPARE(actualBackground.rgb(),expectedBackground.rgb());QCOMPARE(actualBackground.lightness()<128,dark);
            const auto path=qEnvironmentVariable("KEEFI_DEV86_CAPTURE_DIR");
            if(!path.isEmpty()){QVERIFY(QDir().mkpath(path));QVERIFY(frame.save(QDir(path).filePath(QString::fromLatin1(QTest::currentDataTag())+"-"+stage+".png")));}
            qInfo().noquote()<<QString("TODAY_THEME stage=%1 requested=%2 effective=%3 pixel=%4 expected=%5").arg(stage,dark?"dark":"light",KeefiTheme::instance()->isDark()?"dark":"light",actualBackground.name(),expectedBackground.name());
            qInfo().noquote()<<QString("TODAY_FIXTURE stage=%1 width=%2 tabs=%3,%4 first=%5,%6 value=%7 title=%8 state=%9").arg(stage).arg(window.width()).arg(tabs->mapTo(&window,QPoint()).x()).arg(tabs->mapTo(&window,QPoint()).y()).arg(first->mapTo(&window,QPoint()).x()).arg(first->mapTo(&window,QPoint()).y()).arg(gap->data().value,gap->data().title,gap->data().changeText);
        };
        capture("initial");price(up,bn,now);QCOMPARE(gap->data().value,QStringLiteral("—"));fxWire.answer(fx(now));QCoreApplication::processEvents();
        QCOMPARE(gap->data().value,QString("+1.00"));QCOMPARE(gap->data().unit,QString("%"));QVERIFY(gap->data().changeText.startsWith(QStringLiteral("하나 ")));
        QCOMPARE(first->mapTo(&window,QPoint()),firstPosition);QCOMPARE(tabs->mapTo(&window,QPoint()),tabsPosition);capture("current");
        // The selected-asset read is still unanswered when the independent BTC card updates.
        // The world board reads its own keyless sources; 원/달러 goes first because the
        // tether premium waits on it. Answer that read and leave the others pending.
        QTRY_VERIFY(!worldQuotes.sent.isEmpty());QCOMPARE(worldQuotes.sent.first().url,WorldMarkets::request(WorldMarketItem::UsdKrw).url);
        QVERIFY(!worldEcb.sent.isEmpty()&&!worldMof.sent.isEmpty());
        worldQuotes.answer(bankFx(now));QCoreApplication::processEvents();
        auto* usd=window.findChild<TodayLedgerRow*>("todayMetric_usdkrw");QVERIFY(usd);QTRY_COMPARE(usd->data().value,QString("1,400.00"));
        // USDT 1,400 against the 1,400.00 posting: a flat premium, settled before the rows are compared.
        auto* tether=window.findChild<TodayLedgerRow*>("todayMetric_tether");QVERIFY(tether);QTRY_COMPARE(tether->data().value,QString("0.00"));
        const auto metricValues=[&]{QHash<QString,QString> result;for(const auto* key:TodayRows){
            auto* card=window.findChild<TodayLedgerRow*>(QString("todayMetric_")+key);if(card){const auto data=card->data();result.insert(key,data.title+"|"+data.value+"|"+data.unit+"|"+data.sourceKey);}}return result;};
        const auto fixedValues=metricValues();QCOMPARE(fixedValues.size(),int(std::size(TodayRows)));
        auto* summary=window.findChild<ExploreCoinSummary*>();QVERIFY(summary);
        for(const auto* asset:{"ETH","BNB","BTC","BNB"}){
            emit summary->coinSelected(asset);QCoreApplication::processEvents();QCOMPARE(metricValues(),fixedValues);
            QCOMPARE(gap->data().value,QString("+1.00"));QCOMPARE(gap->data().sourceKey,QString("public:btc-premium:usdkrw:hana"));
            int presets=0,checked=0;
            for(auto* choice:summary->findChildren<QPushButton*>())if(choice->property("asset").isValid()){
                ++presets;const bool expected=choice->property("asset").toString()==QString::fromLatin1(asset);
                QCOMPARE(choice->isChecked(),expected);QVERIFY(choice->autoExclusive());checked+=choice->isChecked();
            }
            QCOMPARE(presets,3);QCOMPARE(checked,QString::fromLatin1(asset)=="BNB"?0:1);
        }
        auto* support=window.findChild<QLabel*>("exploreSummaryMarketSupport");QVERIFY(support);QCOMPARE(support->text(),QStringLiteral("업비트 원화마켓 미지원"));
        QCOMPARE(tether->data().value,QString("0.00"));QCOMPARE(tether->data().unit,QString("%"));QVERIFY(tether->data().changeText.startsWith(QStringLiteral("하나 ")));
        const auto before=gap->data();catalogWire.tether="1414";
        // The catalogue's own request cooldown decides when the new USDT price is read.
        QTRY_VERIFY_WITH_TIMEOUT((catalog.refresh(),tether->data().value==QString("+1.00")),20000);
        QCOMPARE(gap->data().value,QString("+1.00"));QCOMPARE(gap->data().sourceKey,before.sourceKey);capture("bnb-and-tether-1414");
        now+=TodayBtcPremium::FxRefreshMs;premium.refresh();price(up,bn,now);now+=TodayBtcPremium::LateMs;premium.refresh();price(up,bn,now);emit premium.changed();
        QCOMPARE(gap->data().value,QString("+1.00"));QCOMPARE(gap->data().freshnessText,QStringLiteral("지연"));QVERIFY(gap->data().publicInformation.contains(QStringLiteral("최근 확인값")));
        QCOMPARE(gap->data().direction,MovementDirection::Unknown);capture("fx-delayed");
        fxWire.answer("",503);QCOMPARE(gap->data().value,QString("+1.00"));QVERIFY(gap->data().publicInformation.contains(QStringLiteral("수신 실패")));
        now+=60000;premium.refresh();price(up,bn,now,"142800000");fxWire.answer(fx(now));QCoreApplication::processEvents();
        QCOMPARE(gap->data().value,QString("+2.00"));QVERIFY(gap->data().freshnessText.isEmpty());QVERIFY(gap->data().changeText.startsWith(QStringLiteral("하나 ")));capture("recovered");
        const auto lastGood=premium.snapshot();
        const auto checkedAt=QDateTime::fromMSecsSinceEpoch(lastGood.checkedAtMs,QTimeZone("Asia/Seoul"));
        const auto compactChecked=checkedAt.toString("MM/dd HH:mm");
        const auto fullChecked=checkedAt.toString("yyyy-MM-dd HH:mm:ss KST");
        QVERIFY(gap->data().publicInformation.contains(QStringLiteral("BTC 김프 확인: ")+fullChecked));
        auto* change=gap->findChild<QLabel*>("metricChange");QVERIFY(change);
        QCoreApplication::processEvents();
        QVERIFY2(change->sizeHint().width()<=change->width(),"The dated note must fit its column");
        // A day-long outage must keep the original date, not replace it with
        // today's receipt/render date while retaining yesterday's percentage.
        now+=24LL*60*60*1000;emit premium.changed();QCoreApplication::processEvents();
        QCOMPARE(premium.snapshot().state,TodayBtcPremiumSnapshot::State::Recent);
        QCOMPARE(premium.snapshot().checkedAtMs,lastGood.checkedAtMs);
        QCOMPARE(gap->data().value,QString("+2.00"));QCOMPARE(gap->data().freshnessText,QStringLiteral("지연"));
        QCOMPARE(gap->data().changeText,compactChecked); // The last good check keeps its own date.
        QVERIFY(gap->data().publicInformation.contains(QStringLiteral("BTC 김프 확인: ")+fullChecked));
        QTextDocument renderedCaption;renderedCaption.setHtml(change->text());
        QVERIFY(renderedCaption.toPlainText().contains(compactChecked));
        QVERIFY(QDateTime::fromMSecsSinceEpoch(now,QTimeZone("Asia/Seoul")).date()!=checkedAt.date());
        QVERIFY2(change->sizeHint().width()<=change->width(),"The previous-day note must fit its column");
        capture("previous-day-recent");
        QCOMPARE(first->mapTo(&window,QPoint()),firstPosition);QCOMPARE(tabs->mapTo(&window,QPoint()),tabsPosition);
        auto* value=gap->findChild<QLabel*>("metricValue");auto* title=gap->findChild<QLabel*>("metricTitle");QVERIFY(value&&title);QVERIFY(value->isVisibleTo(&window));
        QCOMPARE(value->text(),QString("+2.00%")); // The ledger shows the unit inline.QVERIFY2(value->fontMetrics().horizontalAdvance(value->text())<=value->width(),"Actual numeric label must fit, not merely its data object");
        QVERIFY2(title->fontMetrics().horizontalAdvance(title->text())<=title->width(),"The BTC title must be fully visible");
        for(const auto* key:{"kr","us","btc","funding","coinbase","usdt"})QVERIFY2(!window.findChild<QWidget*>(QString("todayMetric_")+key),key); // Never-dated breadth and duplicate coin cards left Today.
        auto* viewChoice=window.findChild<QComboBox*>("exploreView");QVERIFY(viewChoice);viewChoice->setCurrentIndex(viewChoice->findData("list"));QTRY_VERIFY(!world.active());viewChoice->setCurrentIndex(viewChoice->findData("flow"));QTRY_VERIFY(world.active()); // Reads only while Today is on screen.
        QVERIFY(window.findChild<QWidget*>("marketFlowProgress")->isHidden());QVERIFY(window.findChild<QCheckBox*>("exploreObservation_breadth")->isHidden());QVERIFY(window.findChild<QCheckBox*>("exploreObservation_relative")->isHidden());
        QVERIFY(!window.findChild<QPushButton*>("exploreDisplaySettings"));auto* settings=window.findChild<QPushButton*>("openOptions");QVERIFY(settings&&settings->isVisibleTo(&window));
        QTimer::singleShot(0,&window,[&]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QDialog*>(widget))if(dialog->isVisible()){
            auto* display=dialog->findChild<QPushButton*>("optionsDisplayTab");QVERIFY(display);QTest::mouseClick(display,Qt::LeftButton);
            auto* breadth=dialog->findChild<QCheckBox*>("optionExploreObservation_breadth");auto* relative=dialog->findChild<QCheckBox*>("optionExploreObservation_relative");QVERIFY(breadth&&relative);QVERIFY(breadth->isVisibleTo(dialog)&&relative->isVisibleTo(dialog));breadth->setChecked(true);relative->setChecked(true);dialog->close();}});
        settings->click();QTRY_VERIFY(profile.value("explore/observations/breadth").toBool());QTRY_VERIFY(profile.value("explore/observations/relative").toBool());
        QCoreApplication::processEvents();QCOMPARE(first->mapTo(&window,QPoint()),firstPosition);QCOMPARE(tabs->mapTo(&window,QPoint()),tabsPosition);QVERIFY(!vault.exists());
    }
    void hanaPostingGapDoesNotShowBothPremiumsAsCurrent_data(){
        QTest::addColumn<qint64>("postingGapMs");QTest::addColumn<bool>("btcNewer");
        for(const qint64 seconds:{59,60,61,72,120,258})for(const bool btcNewer:{true,false})
            QTest::newRow(qPrintable(QString("%1s-%2-newer").arg(seconds).arg(btcNewer?"btc":"world")))<<seconds*1000<<btcNewer;
    }
    void hanaPostingGapDoesNotShowBothPremiumsAsCurrent(){
        QFETCH(qint64,postingGapMs);QFETCH(bool,btcNewer);
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
        QSettings profile(dir.filePath("synthetic-hana-gap.ini"),QSettings::IniFormat);
        Vault vault(dir.filePath("not-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        CatalogWire catalogWire;PublicMarketCatalog catalog(&catalogWire);
        Wire selectedMetrics,metadata,up,bn,fxWire,worldQuotes,worldEcb,worldMof;MarketMetrics metrics(&selectedMetrics);
        PublicQuoteStream upStream(PublicQuoteStream::Provider::Upbit,std::make_unique<Socket>()),bnStream(PublicQuoteStream::Provider::Binance,std::make_unique<Socket>());
        qint64 now=QDateTime::currentMSecsSinceEpoch();
        TodayBtcPremium premium(&up,&bn,&fxWire,nullptr,[&]{return now;});
        WorldMarkets world(&worldQuotes,&worldEcb,&worldMof,nullptr,[&]{return now;});
        LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
        window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&catalog,false);
        window.setMarketMetrics(&metrics,nullptr,&upStream,&bnStream);window.setTodayBtcPremium(&premium);window.setWorldMarkets(&world);
        window.resize(1440,900);window.show();catalog.refresh();
        const auto tetherPriceReady=[&]{for(const auto& row:catalog.rows())if(row.base=="USDT")return row.priced&&row.price=="1400";return false;};
        QTRY_VERIFY(tetherPriceReady());QTRY_VERIFY(!worldQuotes.sent.isEmpty());
        QVERIFY(!up.sent.isEmpty()&&!bn.sent.isEmpty()&&!fxWire.sent.isEmpty());
        QCOMPARE(worldQuotes.sent.first().url,WorldMarkets::request(WorldMarketItem::UsdKrw).url);
        QCOMPARE(fxWire.sent.first().url,TodayBtcPremium::request(TodayBtcPremium::Read::Fx).url);
        // These are controlled source postings, not a claim about the original
        // 16:10/16:08 responses. The live trace separately observed 72s and 258s gaps.
        const qint64 latest=(now/1000)*1000-1000; // Posting helpers preserve whole seconds.
        const qint64 btcAt=btcNewer?latest:latest-postingGapMs,worldAt=btcNewer?latest-postingGapMs:latest;
        price(up,bn,now);
        // The existing helpers post exactly one minute before their argument.
        fxWire.answer(fx(btcAt+60000));worldQuotes.answer(bankFx(worldAt+60000));QCoreApplication::processEvents();
        QCOMPARE(premium.snapshot().fx.postedAtMs,btcAt);QCOMPARE(world.snapshot().quote(WorldMarketItem::UsdKrw).tradedAtMs,worldAt);
        auto* gap=window.findChild<TodayLedgerRow*>("todayMetric_gap");
        auto* tether=window.findChild<TodayLedgerRow*>("todayMetric_tether");QVERIFY(gap&&tether);
        const QRegularExpression numeric(QStringLiteral("^[+−-]?[0-9][0-9,.]*%$"));
        const auto freshNumeric=[&](TodayLedgerRow* row){
            const auto data=row->data();auto* value=row->findChild<QLabel*>("metricValue");
            return row->isVisibleTo(&window)&&value&&value->isVisibleTo(&window)&&data.unit=="%"&&
                data.freshnessText.isEmpty()&&numeric.match(value->text()).hasMatch();
        };
        const bool bothFreshNumeric=freshNumeric(gap)&&freshNumeric(tether);
        const auto detail=QString("posting gap=%1ms, newer=%2, BTC=%3 [%4/%5], tether=%6 [%7/%8]")
            .arg(postingGapMs).arg(btcNewer?"BTC":"World",gap->data().value,gap->data().changeText,gap->data().freshnessText,
                tether->data().value,tether->data().changeText,tether->data().freshnessText);
        if(postingGapMs<=60000)QVERIFY2(bothFreshNumeric,qPrintable(detail));
        else {
            QVERIFY2(!bothFreshNumeric,qPrintable(detail));
            auto* newer=btcNewer?gap:tether;auto* older=btcNewer?tether:gap;
            QVERIFY2(freshNumeric(newer),qPrintable(detail));QVERIFY(!freshNumeric(older));
            QCOMPARE(older->data().value,QStringLiteral("—"));QCOMPARE(older->data().changeText,QStringLiteral("확인 필요"));
            // Cancel the older model's outstanding read, then use its normal
            // minute refresh to receive the same posting as the newer model.
            if(btcNewer){
                world.setActive(false);now+=60000;const auto before=worldQuotes.sent.size();world.setActive(true);
                QCOMPARE(worldQuotes.sent.size(),before+1);
                QCOMPARE(worldQuotes.sent.last().url,WorldMarkets::request(WorldMarketItem::UsdKrw).url);
                worldQuotes.answer(bankFx(latest+60000));
            }else{
                premium.setActive(false);now+=60000;const auto before=fxWire.sent.size();premium.setActive(true);
                QCOMPARE(fxWire.sent.size(),before+1);
                QCOMPARE(fxWire.sent.last().url,TodayBtcPremium::request(TodayBtcPremium::Read::Fx).url);
                price(up,bn,now);fxWire.answer(fx(latest+60000));
            }
            QCoreApplication::processEvents();
            QCOMPARE(premium.snapshot().fx.postedAtMs,latest);QCOMPARE(world.snapshot().quote(WorldMarketItem::UsdKrw).tradedAtMs,latest);
            QTRY_VERIFY(freshNumeric(gap)&&freshNumeric(tether));
        }
        premium.setActive(false);world.setActive(false);KeefiTheme::instance()->setSettings(nullptr);QVERIFY(!vault.exists());
    }
    // Opt-in: KEEFI_TODAY_LIVE_DIR=<dir> renders Today from the real public sources
    // (Upbit, Binance, ECB, US Treasury, Naver Securities, Japan MOF) with an empty,
    // never-created vault. Captures the window and the board in both themes.
    void liveTodayBoardCapture_data(){
        QTest::addColumn<QSize>("size");
        QTest::newRow("1440x900")<<QSize(1440,900);
        QTest::newRow("1280x768")<<QSize(1280,768);
    }
    void liveTodayBoardCapture(){
        const auto out=qEnvironmentVariable("KEEFI_TODAY_LIVE_DIR");
        if(out.isEmpty())QSKIP("Set KEEFI_TODAY_LIVE_DIR to capture Today from the real public sources.");
        QFETCH(QSize,size);QTemporaryDir dir;QVERIFY(dir.isValid());QVERIFY(QDir().mkpath(out));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
        QSettings profile(dir.filePath("live-today.ini"),QSettings::IniFormat);
        Vault vault(dir.filePath("not-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        NetworkTradingTransport catalogNet,metricsNet,upNet,bnNet,fxNet,overviewNet,worldNet,ecbNet,mofNet;
        PublicMarketCatalog catalog(&catalogNet);MarketMetrics metrics(&metricsNet);
        PublicQuoteStream upStream(PublicQuoteStream::Provider::Upbit,std::make_unique<Socket>()),bnStream(PublicQuoteStream::Provider::Binance,std::make_unique<Socket>());
        TodayBtcPremium premium(&upNet,&bnNet,&fxNet);MarketOverview overview(&overviewNet);WorldMarkets world(&worldNet,&ecbNet,&mofNet);
        LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
        window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics,nullptr,&upStream,&bnStream);
        window.setTodayBtcPremium(&premium);window.setMarketOverview(&overview);window.setWorldMarkets(&world);
        window.resize(size);window.show();catalog.refresh();
        const auto row=[&](const char* key){auto* found=window.findChild<TodayLedgerRow*>(QString("todayMetric_")+key);return found?found->data():ExploreMetricCardData{};};
        const auto ready=[&]{
            const auto s=world.snapshot();for(const auto& q:s.quotes)if(!q.available())return false;
            return s.dollarIndex.available()&&s.japanYield.available()&&row("gap").unit==QStringLiteral("%")&&row("tether").unit==QStringLiteral("%")
                &&row("treasury").unit==QStringLiteral("%")&&row("spread").unit==QStringLiteral("%p");
        };
        QTRY_VERIFY_WITH_TIMEOUT(ready(),90000);
        for(const auto* key:TodayRows){const auto d=row(key);
            qInfo().noquote()<<QString("TODAY_LIVE %1 | %2 | %3%4 | %5 | %6").arg(key,d.title,d.value,d.unit==QStringLiteral("%")||d.unit==QStringLiteral("%p")?d.unit:QString{},d.changeText,d.freshnessText);
            QVERIFY2(d.value!=QStringLiteral("—"),key);}
        auto* scroll=window.findChild<QScrollArea*>("marketFlowPage");QVERIFY(scroll);
        if(size.width()>=1440)QTRY_COMPARE(scroll->verticalScrollBar()->maximum(),0);
        for(const bool dark:{false,true}){
            KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);QCoreApplication::processEvents();QTest::qWait(200);
            const auto name=QString("live-today-%1x%2-%3").arg(size.width()).arg(size.height()).arg(dark?"dark":"light");
            QVERIFY(window.grab().save(QDir(out).filePath(name+".png")));
            QVERIFY(window.findChild<QWidget*>("todayMetricPanel")->grab().save(QDir(out).filePath(name+"-board.png")));
        }
        QVERIFY(!vault.exists());
    }
};
QTEST_MAIN(TodayPremiumWindowTest)
#include "tst_today_premium_window.moc"
