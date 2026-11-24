#include "keefi/AccountModel.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/Vault.h"
#include "keefi/TradingTransport.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/Spotlight.h"
#include "keefi/MarketMap.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/MarketCandles.h"
#include "keefi/ChartWorkspace.h"
#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>
#include <QUrlQuery>
using namespace keefi;
namespace {
class LegacyBoardTransport final:public TradingTransport {
public:
    QStringList paths;
    QByteArray coinOverride;
    bool holdReplies=false;
    static QByteArray coins(){return R"([{"id":"bitcoin","symbol":"btc","name":"Bitcoin","market_cap":2206446166687052,"current_price":109771433,"price_change_percentage_24h":0.47},
        {"symbol":"usdt","name":"Tether","market_cap":244091566738932,"current_price":1430,"price_change_percentage_24h":-0.3},
        {"symbol":"figr_heloc","name":"Figure Heloc","market_cap":24000000000000,"current_price":1450,"price_change_percentage_24h":0.0},
        {"symbol":"xaut","name":"Tether Gold","market_cap":2400000000000,"current_price":5000000,"price_change_percentage_24h":0.1},
        {"symbol":"buidl","name":"BlackRock USD Institutional Digital Liquidity Fund","market_cap":2000000000000,"current_price":1430,"price_change_percentage_24h":0.0},
        {"symbol":"zec","name":"Zcash","market_cap":25849075088475,"current_price":1520034,"price_change_percentage_24h":-1.95}])";}
    static QByteArray korea(){return R"({"marketStatus":"CLOSE","stocks":[
        {"itemCode":"005930","stockName":"삼성전자","closePrice":"223,500","fluctuationsRatio":"3.71","compareToPreviousPrice":{"code":"2"},"marketValue":"13,066,433"},
        {"itemCode":"000660","stockName":"SK하이닉스","closePrice":"1,304,000","fluctuationsRatio":"1.20","compareToPreviousPrice":{"code":"5"},"marketValue":"9,525,620"}]})";}
    static QByteArray us(){return R"({"data":{"rows":[
        {"symbol":"NVDA","name":"NVIDIA Corporation Common Stock","lastsale":"$214.13","pctchange":"-1.427%","marketCap":"5,160,533,000,000","sector":"Technology"},
        {"symbol":"JPM","name":"JPMorgan Chase & Co. Common Stock","lastsale":"$301.20","pctchange":"1.25%","marketCap":"830,000,000,000","sector":"Finance"},
        {"symbol":"BRK/A","name":"Odd","lastsale":"$1","pctchange":"0%","marketCap":"1","sector":"Finance"}]}})";}
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(PublicEquityBoard::isBoardRequest(request));paths.append(request.url.host());
        HttpResult response;response.status=200;
        response.body=request.url.host()=="api.coingecko.com"?(coinOverride.isEmpty()?coins():coinOverride):request.url.host()=="m.stock.naver.com"?korea():us();
        QTimer::singleShot(1,this,[this,token,response]{if(!holdReplies)emit finished(token,response);});
    }
};
class LegacyCatalogTransport final:public TradingTransport {
public:
    QList<RequestSpec> requests;int quoteStatus=200;bool holdReplies=false;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());QVERIFY(NetworkTradingTransport::allowed(request));requests.append(request);
        HttpResult response;response.status=200;const auto now=QString::number(QDateTime::currentMSecsSinceEpoch());
        if(request.operation==Operation::Markets)response.body=R"([{"market":"KRW-PUMP","korean_name":"가짜펌프","english_name":"Fake Pump","market_event":{"warning":false,"caution":{}}},)"
            R"({"market":"KRW-BTC","korean_name":"가짜비트","english_name":"Fake Bit","market_event":{"warning":true,"caution":{"PRICE_FLUCTUATIONS":false}}},)"
            R"({"market":"BTC-ETH","korean_name":"가짜이더","english_name":"Fake Eth","market_event":{"warning":false,"caution":{}}}])";
        else if(quoteStatus!=200){response.status=quoteStatus;response.headers.insert("Retry-After","3");response.body="{}";}
        else{QStringList rows;for(const auto& market:QUrlQuery(request.url).queryItemValue("markets").split(','))
                rows.append(QString(R"({"market":"%1","trade_price":%2,"prev_closing_price":%3,"signed_change_rate":%4,"timestamp":%5})")
                    .arg(market,market=="KRW-PUMP"?"2.75":"95000000",market=="KRW-PUMP"?"2.5":"96000000",market=="KRW-PUMP"?"0.1":"-0.0104166666666667",now));
            response.body=("["+rows.join(',')+"]").toUtf8();}
        QTimer::singleShot(1,this,[this,token,response]{if(!holdReplies)emit finished(token,response);});
    }
};
// A test-only fingerprint of typed provenance. No rendered QLabel text is
// used to establish source identity, observation clocks or shared numbering.
QString footerFacts(ExploreEvidenceFooter* footer) {
    QStringList values;const auto entries=footer->entries();const auto numbers=footer->numbers();
    const auto at=[](qint64 ms){return ms>0?QDateTime::fromMSecsSinceEpoch(ms,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss")+" KST":QStringLiteral("미제공");};
    for(int i=0;i<entries.size();++i) {
        const auto& e=entries[i];
        values.append(QString("[%1] %2 · 원천 %3 · 수신 %4 · %5").arg(numbers.value(i)).arg(e.provider,e.sourceDate.isEmpty()?at(e.sourceTimeMs):e.sourceDate,at(e.receivedAtMs),e.state));
    }
    return values.join('\n');
}
bool referenceFree(const QString& text) {
    return !QRegularExpression(QStringLiteral("\\[[0-9]+\\]|https?://|(?:stock|coin):")).match(text).hasMatch();
}
qreal ledgerGlyphWidth(const QString& text) {
    QTextLayout layout(text,KeefiFonts::font(FontRole::Secondary14));layout.setFormats(KeefiFonts::numberRuns(text));
    layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();return line.isValid()?line.naturalTextWidth():0;
}
}
class D4LegacyDesignTest final:public QObject {
    Q_OBJECT
private slots:
    void publicPriceKeepsEvidenceAssignmentsWithoutExposingReferences() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic P1");QCoreApplication::setApplicationName("Public evidence display");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);LegacyCatalogTransport http;PublicMarketCatalog catalog(&http);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setSecurityStatus(true);window.show();
        auto* mode=window.findChild<QComboBox*>("exploreView");auto* section=window.findChild<QComboBox*>("exploreMarketSection");QVERIFY(mode&&section);
        mode->setCurrentIndex(mode->findData("list"));section->setCurrentIndex(section->findData("coin"));
        auto* search=window.findChild<QLineEdit*>("exploreSearch");auto* table=window.findChild<QTableWidget*>("exploreMarkets");QVERIFY(search&&table);
        QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);search->setText("BTC");
        QTRY_VERIFY(table->rowCount()>0);int selected=-1;
        for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)){selected=row;break;}
        QVERIFY(selected>=0);table->setCurrentCell(selected,0);
        auto* price=table->item(selected,2);QVERIFY(price);
        QTRY_VERIFY(!price->data(EvidenceNumbersRole).value<QList<int>>().isEmpty());
        const auto assigned=price->data(EvidenceNumbersRole).value<QList<int>>();
        const auto shown=price->text()+price->toolTip()+price->data(Qt::AccessibleDescriptionRole).toString();
        QVERIFY2(!QRegularExpression(QStringLiteral("\\[\\d+\\]")).match(shown).hasMatch(),qPrintable(shown));
        QVERIFY(!shown.contains("https://"));QVERIFY(!shown.contains("coin:"));
        QCOMPARE(price->data(EvidenceNumbersRole).value<QList<int>>(),assigned);
        QVERIFY(!catalog.rows().first().quoteUrl.isEmpty());QVERIFY(!vault.exists());
    }
    void lockingKeepsTheSelectedMapReferenceSynchronousWithItsCommonBook_data() {
        QTest::addColumn<int>("theme");QTest::addColumn<QString>("market");QTest::addColumn<QString>("symbol");QTest::addColumn<int>("number");
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark}) {
            const auto theme=mode==ThemeMode::Light?QString("light"):QString("dark");
            QTest::newRow(qPrintable(theme+"-kr"))<<int(mode)<<QString("kr")<<QString("005930")<<2;
            QTest::newRow(qPrintable(theme+"-us"))<<int(mode)<<QString("us")<<QString("NVDA")<<3;
        }
    }
    void lockingKeepsTheSelectedMapReferenceSynchronousWithItsCommonBook() {
        QFETCH(int,theme);QFETCH(QString,market);QFETCH(QString,symbol);QFETCH(int,number);QTemporaryDir temp;QVERIFY(temp.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Synchronous Source Book");QCoreApplication::setApplicationName("Synthetic Synchronous Source Book");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());Vault vault(temp.filePath("synthetic-reference-vault"));QVERIFY(vault.create("Fake D4 Linkage! 2026"));
        QVERIFY(vault.isUnlocked());QVERIFY(accountReadsAvailable(vault));QCOMPARE(vault.accounts().size(),0);AccountModel accounts(&vault);
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);
        window.resize(1440,1050);window.show();KeefiTheme::instance()->setMode(ThemeMode(theme));board.refresh();
        QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);for(const auto& section:{QString("coin"),QString("kr"),QString("us")})QTRY_COMPARE(board.items(section).size(),2);
        auto* views=window.findChild<QComboBox*>("exploreView");auto* sections=window.findChild<QComboBox*>("exploreMarketSection");auto* map=window.findChild<MarketMapWidget*>("marketMap");QVERIFY(views&&sections&&map);
        views->setCurrentIndex(views->findData("map"));sections->setCurrentIndex(sections->findData("all"));
        QRectF target;for(const auto& tile:map->tileRects())if(tile.first==market+":"+symbol)target=tile.second;QVERIFY(!target.isEmpty());
        QTest::mouseClick(map,Qt::LeftButton,Qt::NoModifier,target.center().toPoint());
        auto* detail=window.findChild<AssetDetailPanel*>("marketMapDetail");auto* book=window.findChild<ExploreEvidenceFooter*>("exploreBoardEvidenceFooter");QVERIFY(detail&&book);QTRY_VERIFY(detail->isVisibleTo(&window));
        auto* selectedCard=detail->findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(selectedCard);
        const auto selectedEvidence=selectedCard->data().evidence;QVERIFY(!selectedEvidence.isEmpty());
        QStringList lines;const QStringList markets{"coin","kr","us"},providers{"CoinGecko",QStringLiteral("네이버 증권"),"Nasdaq"};
        for(int i=0;i<markets.size();++i) {
            const auto received=board.receivedAtMs(markets[i]);QVERIFY(received>0);
            const auto timestamp=QDateTime::fromMSecsSinceEpoch(received,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss")+" KST";
            lines.append(QString("[%1] %2 · 원천 미제공 · 수신 %3 · 유효").arg(i+1).arg(providers[i],timestamp));
        }
        const auto expectedBook=lines.join('\n');QTRY_COMPARE(book->evidenceCount(),3);QTRY_COMPARE(footerFacts(book),expectedBook);QCOMPARE(book->numbers().at(number-1),number);
        QCOMPARE(selectedEvidence.first().provider,book->entries().at(number-1).provider);
        QCOMPARE(selectedEvidence.first().receivedAtMs,book->entries().at(number-1).receivedAtMs);
        QVERIFY(referenceFree(selectedCard->findChild<QLabel*>("metricEvidenceReference")->text()));
        QTRY_VERIFY(!catalog.busy()&&!board.busy());catalogHttp.holdReplies=true;boardHttp.holdReplies=true;
        QVERIFY(accountReadsAvailable(vault));QCOMPARE(vault.accounts().size(),0);
        // The same completed public observations must retain the same shared
        // number when lock returns, without a queued repaint fixing it later.
        window.lockAndClear();
        QCOMPARE(footerFacts(book),expectedBook);QCOMPARE(selectedCard->data().evidence,selectedEvidence);
        QCOMPARE(map->selectedSection(),market);QCOMPARE(map->selectedId(),symbol);QVERIFY(vault.exists());QVERIFY(!vault.isUnlocked());QVERIFY(!accountReadsAvailable(vault));QCOMPARE(vault.accounts().size(),0);
    }
    void narrowExploreKeyboardSelectionAndBackKeepTheRealListFocused_data() {
        QTest::addColumn<int>("theme");QTest::addColumn<QString>("action");
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto& action:{QString("map-arrows"),QString("map-back")})
            QTest::newRow(qPrintable(QString("%1-%2").arg(mode==ThemeMode::Light?"light":"dark",action)))<<int(mode)<<action;
    }
    void narrowExploreKeyboardSelectionAndBackKeepTheRealListFocused() {
        QFETCH(int,theme);QFETCH(QString,action);QTemporaryDir temp;QVERIFY(temp.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Narrow Keyboard");QCoreApplication::setApplicationName("Synthetic Narrow Keyboard");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);
        window.resize(720,1050);window.show();KeefiTheme::instance()->setMode(ThemeMode(theme));board.refresh();
        QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);QTRY_COMPARE(board.items("coin").size(),2);
        auto* views=window.findChild<QComboBox*>("exploreView");auto* sections=window.findChild<QComboBox*>("exploreMarketSection");auto* watch=window.findChild<ExploreWatchlist*>("exploreWatchlist");QVERIFY(views&&sections&&watch);
        const bool mapAction=action.startsWith("map-");views->setCurrentIndex(views->findData(mapAction?"map":"list"));sections->setCurrentIndex(sections->findData("coin"));
        watch->toggle("coin","BTC");watch->toggle("coin","ZEC");watch->setSection("coin");QTRY_VERIFY(!catalog.busy()&&!board.busy());
        if(mapAction){
            auto* readyTable=watch->findChild<QTableWidget*>("watchlistTable");QVERIFY(readyTable);
            views->setCurrentIndex(views->findData("list"));QTRY_COMPARE(readyTable->rowCount(),2);
            views->setCurrentIndex(views->findData("map"));QTRY_VERIFY(!catalog.busy()&&!board.busy());
        }
        catalogHttp.holdReplies=true;boardHttp.holdReplies=true;QCoreApplication::processEvents();window.activateWindow();QApplication::setActiveWindow(&window);
        auto* map=window.findChild<MarketMapWidget*>("marketMap");auto* table=watch->findChild<QTableWidget*>("watchlistTable");QVERIFY(map&&table);QTRY_COMPARE(table->rowCount(),2);
        auto* detail=window.findChild<AssetDetailPanel*>("marketMapDetail");QVERIFY(detail);
        auto* owner=window.findChild<ListDetailView*>("marketMapListDetail");QVERIFY(owner);QTRY_VERIFY(owner->narrow());
        auto* splitter=owner->findChild<QSplitter*>("assetListDetailSplitter");QVERIFY(splitter);auto* list=splitter->widget(0);QVERIFY(list->isVisibleTo(&window));QVERIFY(detail->isHidden());
        QSignalSpy selected(map,&MarketMapWidget::tileSelected);
        if(action=="map-arrows") {
            map->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(map));
            for(const auto key:{Qt::Key_Right,Qt::Key_Down}) {
                const auto before=map->selectedSection()+":"+map->selectedId();QTest::keyClick(QApplication::focusWidget(),key);
                QVERIFY(map->selectedSection()+":"+map->selectedId()!=before);
                QVERIFY2(list->isVisibleTo(&window)&&detail->isHidden(),"Arrow selection must keep the map visible until an explicit Enter activation");
                QCOMPARE(QApplication::focusWidget(),static_cast<QWidget*>(map));
            }
            QCOMPARE(selected.size(),2);QTest::keyClick(QApplication::focusWidget(),Qt::Key_Return);QCOMPARE(selected.size(),3);
        } else {
            map->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(map));
            QRectF bitcoin;for(const auto& tile:map->tileRects())if(tile.first=="coin:BTC")bitcoin=tile.second;QVERIFY(!bitcoin.isEmpty());QTest::mouseClick(map,Qt::LeftButton,Qt::NoModifier,bitcoin.center().toPoint());
        }
        QTRY_VERIFY(detail->isVisibleTo(&window));QVERIFY(list->isHidden());
        auto* back=detail->findChild<QPushButton*>("assetDetailBack");QVERIFY(back);QVERIFY(back->isVisibleTo(&window));back->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(back));
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_Space);QTRY_VERIFY(list->isVisibleTo(&window));QVERIFY(detail->isHidden());
        if(mapAction) {
            QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(map));const auto before=map->selectedSection()+":"+map->selectedId();const auto selectionSignals=selected.size();
            QTest::keyClick(QApplication::focusWidget(),Qt::Key_Right);QVERIFY(map->selectedSection()+":"+map->selectedId()!=before);QCOMPARE(selected.size(),selectionSignals+1);
        }
        QVERIFY(list->isVisibleTo(&window));QVERIFY(detail->isHidden());QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
    void narrowSpotlightKeepsTheSavedQuoteReadable_data() {
        QTest::addColumn<int>("theme");QTest::addColumn<QString>("market");
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto& market:{QString("coin"),QString("kr"),QString("us")})
            QTest::newRow(qPrintable(QString("%1-%2").arg(mode==ThemeMode::Light?"light":"dark",market)))<<int(mode)<<market;
    }
    void narrowSpotlightKeepsTheSavedQuoteReadable() {
        QFETCH(int,theme);QFETCH(QString,market);
        QTemporaryDir temp;QVERIFY(temp.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Narrow Explore");QCoreApplication::setApplicationName("Synthetic Narrow Explore");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());
        Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);
        window.resize(1280,800);window.show();KeefiTheme::instance()->setMode(ThemeMode(theme));board.refresh();
        QTRY_COMPARE(window.width(),1280);QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);QTRY_COMPARE(board.items(market).size(),2);
        auto* views=window.findChild<QComboBox*>("exploreView");auto* sections=window.findChild<QComboBox*>("exploreMarketSection");auto* watch=window.findChild<ExploreWatchlist*>("exploreWatchlist");
        QVERIFY(views&&sections&&watch);views->setCurrentIndex(views->findData("list"));sections->setCurrentIndex(sections->findData(market));
        const QStringList ids=market=="coin"?QStringList{"BTC","PUMP"}:market=="kr"?QStringList{"005930","000660"}:QStringList{"NVDA","JPM"};
        for(const auto& id:ids)watch->toggle(market,id);watch->setSection(market);
        auto* table=watch->findChild<QTableWidget*>("watchlistTable");QVERIFY(table);QTRY_COMPARE(table->rowCount(),2);
        QTRY_VERIFY(!catalog.busy()&&!board.busy());catalogHttp.holdReplies=true;boardHttp.holdReplies=true;QCoreApplication::processEvents();
        QTRY_VERIFY(table->isVisibleTo(&window));QCOMPARE(watch->ids(market),ids);
        table->horizontalScrollBar()->setValue(0);table->verticalScrollBar()->setValue(0);
        const auto originalPrice=table->item(0,2)->text(),originalChange=table->item(0,3)->text();
        QVERIFY(originalPrice!=QStringLiteral("—"));QVERIFY(originalChange!=QStringLiteral("—"));
        // The common dashboard rail keeps name, complete price/change and
        // internal evidence; secondary columns fold instead of scrolling.
        QVERIFY(watch->compact());
        for(int column:{0,1,2,3}) {
            QVERIFY(!table->isColumnHidden(column));const auto* cell=table->item(0,column);
            if(column>=2) {
                QCOMPARE(cell->font(),KeefiFonts::font(FontRole::Number11));
                const auto references=cell->data(EvidenceNumbersRole).value<QList<int>>();QVERIFY(!references.isEmpty());
                QVERIFY(referenceFree(cell->data(Qt::AccessibleDescriptionRole).toString()));
                const qreal needed=ledgerGlyphWidth(cell->text())+24;
                QVERIFY2(table->columnWidth(column)>=needed,"The original amount/change must retain its full glyph width after reference removal");
            }
            const auto rect=table->visualItemRect(cell);
            QVERIFY2(table->viewport()->rect().contains(rect),qPrintable(QString("Saved quote column %1 must be initially readable: rect %2,%3 %4x%5, viewport %6x%7, original price %8, change %9")
                .arg(column).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()).arg(table->viewport()->width()).arg(table->viewport()->height()).arg(originalPrice,originalChange)));
        }
        QCOMPARE(table->item(0,2)->text(),originalPrice);QCOMPARE(table->item(0,3)->text(),originalChange);
        for(int column:{4,5,6})QVERIFY(table->isColumnHidden(column));
        QTRY_COMPARE(table->horizontalScrollBar()->maximum(),0);
        auto* sidebar=window.findChild<QWidget*>("exploreCommonSidebar");QVERIFY(sidebar);QVERIFY(sidebar->isAncestorOf(table));
        for(int column:{1,2,3}) {
            const auto rect=table->visualItemRect(table->item(0,column));
            QVERIFY2(sidebar->rect().contains(rect.translated(table->viewport()->mapTo(sidebar,QPoint()))),"The saved name, price and change must remain inside the shared sidebar");
            QVERIFY2(window.rect().contains(rect.translated(table->viewport()->mapTo(&window,QPoint()))),"The shared sidebar must keep its saved quote visible at 1280x800");
        }
        // A saved row opens the chart. This window has no chart, so the list stays with a short reason.
        QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,table->visualItemRect(table->item(0,1)).center());
        QVERIFY(!window.findChild<AssetDetailPanel*>("spotlightDetail"));QTRY_VERIFY(!window.findChild<QLabel*>("localFeedback")->text().isEmpty());QVERIFY(table->isVisibleTo(&window));
        auto* footer=window.findChild<ExploreEvidenceFooter*>("exploreBoardEvidenceFooter");QVERIFY(footer);QTRY_VERIFY(footer->isHidden());
        for(int column:{2,3})QTRY_VERIFY(!table->item(0,column)->data(EvidenceNumbersRole).value<QList<int>>().isEmpty());
        const auto evidence=footerFacts(footer);QVERIFY(!evidence.isEmpty());
        window.lockAndClear();QCoreApplication::processEvents();QCOMPARE(footerFacts(footer),evidence);
        QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
    void narrowPublicSelectionUsesTheExistingListDetailNavigation_data() {
        QTest::addColumn<int>("theme");QTest::addColumn<QString>("route");QTest::addColumn<int>("height");
        // The ranking opens the chart instead of a detail, so only the map keeps list-detail navigation.
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto& route:{QString("map")})for(int height:{1050,640})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(mode==ThemeMode::Light?"light":"dark",route).arg(height)))<<int(mode)<<route<<height;
    }
    void narrowPublicSelectionUsesTheExistingListDetailNavigation() {
        QFETCH(int,theme);QFETCH(QString,route);QFETCH(int,height);QTemporaryDir temp;QVERIFY(temp.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Narrow Navigation");QCoreApplication::setApplicationName("Synthetic Narrow Navigation");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);
        window.resize(720,height);window.show();KeefiTheme::instance()->setMode(ThemeMode(theme));board.refresh();
        QTRY_COMPARE(window.size(),QSize(720,height));QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);QTRY_COMPARE(board.items("coin").size(),2);
        auto* views=window.findChild<QComboBox*>("exploreView");auto* sections=window.findChild<QComboBox*>("exploreMarketSection");auto* watch=window.findChild<ExploreWatchlist*>("exploreWatchlist");QVERIFY(views&&sections&&watch);
        views->setCurrentIndex(views->findData(route));sections->setCurrentIndex(sections->findData(route=="map"?"all":"coin"));
        watch->toggle("coin","BTC");watch->toggle("coin","PUMP");watch->setSection("coin");
        auto* table=watch->findChild<QTableWidget*>("watchlistTable");auto* map=window.findChild<MarketMapWidget*>("marketMap");QVERIFY(table&&map);
        if(route=="map")views->setCurrentIndex(views->findData("list"));
        QTRY_COMPARE(table->rowCount(),2);
        if(route=="map")views->setCurrentIndex(views->findData("map"));
        QTRY_VERIFY(!catalog.busy()&&!board.busy());catalogHttp.holdReplies=true;boardHttp.holdReplies=true;QCoreApplication::processEvents();
        auto* owner=window.findChild<ListDetailView*>("marketMapListDetail");QVERIFY(owner);QTRY_VERIFY(owner->narrow());
        auto* splitter=owner->findChild<QSplitter*>("assetListDetailSplitter");QVERIFY(splitter);auto* list=splitter->widget(0);
        auto* detail=window.findChild<AssetDetailPanel*>("marketMapDetail");QVERIFY(detail);
        auto* footer=window.findChild<ExploreEvidenceFooter*>("exploreBoardEvidenceFooter");QVERIFY(footer);QTRY_VERIFY(footer->isHidden());
        QTRY_VERIFY(list->isVisibleTo(&window));QVERIFY(detail->isHidden());
        const auto initialBook=footerFacts(footer);QVERIFY(!initialBook.isEmpty());
        const auto saved=watch->ids("coin");const auto price=table->item(0,2)->text(),change=table->item(0,3)->text();
        const auto capture=[&](const QString& phase){
            const auto out=qEnvironmentVariable("KEEFI_G8_FOLLOWUP_LAYOUT_RENDER_DIR");if(out.isEmpty())return true;QDir().mkpath(out);QTest::qWait(30);
            return window.grab().save(out+QString("/layout-%1-%2-%3x%4-%5.png").arg(route,theme==int(ThemeMode::Light)?"light":"dark").arg(window.width()).arg(height).arg(phase));
        };
        QVERIFY(capture("initial-list"));
        QRectF bitcoin;for(const auto& tile:map->tileRects())if(tile.first=="coin:BTC")bitcoin=tile.second;QVERIFY(!bitcoin.isEmpty());
        QTest::mouseClick(map,Qt::LeftButton,Qt::NoModifier,bitcoin.center().toPoint());
        QTRY_VERIFY(detail->isVisibleTo(&window));QVERIFY(list->isHidden());
        auto* back=detail->findChild<QPushButton*>("assetDetailBack");QVERIFY(back);QVERIFY(back->isVisibleTo(&window));QCOMPARE(back->text(),QStringLiteral("‹ 목록"));
        QVERIFY(window.rect().contains(QRect(back->mapTo(&window,QPoint()),back->size())));
        auto* card=detail->findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(card);QCOMPARE(card->data().title,QString("Bitcoin"));QCOMPARE(card->data().value,QString("109,771,433"));
        QTRY_COMPARE(footerFacts(footer),initialBook);QVERIFY(capture("selected-detail"));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
        QVERIFY2(!back->rect().isEmpty(),"The native layout must allocate a nonzero back-button hit rectangle before a synthetic mouse click");
        QVERIFY(window.rect().contains(QRect(back->mapTo(&window,QPoint()),back->size())));
        QSignalSpy clicked(back,&QPushButton::clicked);QTest::mouseClick(back,Qt::LeftButton);QCOMPARE(clicked.size(),1);
        QTRY_VERIFY(list->isVisibleTo(&window));QVERIFY(detail->isHidden());
        QCOMPARE(watch->ids("coin"),saved);QCOMPARE(table->item(0,2)->text(),price);QCOMPARE(table->item(0,3)->text(),change);QTRY_COMPARE(footerFacts(footer),initialBook);QVERIFY(capture("back-list"));
        window.resize(1440,height);QTRY_COMPARE(window.size(),QSize(1440,height));QTRY_VERIFY(!owner->narrow());QTRY_VERIFY(list->isVisibleTo(&window)&&detail->isVisibleTo(&window));
        // The detail follows the window: 30% of the list-detail width, wider than the old fixed 340px here.
        QTRY_COMPARE(detail->width(),ListDetailView::detailWidth(owner->width()));QVERIFY(detail->width()>340);
        QCOMPARE(splitter->handleWidth(),14);QVERIFY(capture("wide"));
        window.resize(720,height);QTRY_COMPARE(window.size(),QSize(720,height));QTRY_VERIFY(owner->narrow());QTRY_VERIFY(list->isVisibleTo(&window));QVERIFY(detail->isHidden());
        QTRY_COMPARE(footerFacts(footer),initialBook);window.lockAndClear();QCoreApplication::processEvents();QCOMPARE(footerFacts(footer),initialBook);
        QCOMPARE(watch->ids("coin"),saved);QCOMPARE(table->item(0,2)->text(),price);QCOMPARE(table->item(0,3)->text(),change);QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
    void publicNumbersShareOneReadableSourceBook_data() {
        QTest::addColumn<int>("theme");QTest::addColumn<QString>("route");QTest::addColumn<int>("width");
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto& route:{QString("map"),QString("list"),QString("search")})for(int width:{1440,720})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(mode==ThemeMode::Light?"light":"dark",route).arg(width)))<<int(mode)<<route<<width;
    }
    void publicNumbersShareOneReadableSourceBook() {
        QFETCH(int,theme);QFETCH(QString,route);QFETCH(int,width);
        QTemporaryDir temp;QVERIFY(temp.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic Legacy Design");QCoreApplication::setApplicationName("Synthetic Legacy Design");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());
        Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);window.resize(width,1050);window.show();board.refresh();
        KeefiTheme::instance()->setMode(ThemeMode(theme));
        QCOMPARE(KeefiTheme::instance()->isDark(),theme==int(ThemeMode::Dark));
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.rows().isEmpty()&&catalog.rows().first().priced,4000);QTRY_COMPARE_WITH_TIMEOUT(board.items("coin").size(),2,4000);QTRY_COMPARE_WITH_TIMEOUT(board.items("kr").size(),2,4000);QTRY_COMPARE_WITH_TIMEOUT(board.items("us").size(),2,4000);
        auto* views=window.findChild<QComboBox*>("exploreView");auto* sections=window.findChild<QComboBox*>("exploreMarketSection");auto* search=window.findChild<QLineEdit*>("exploreSearch");auto* watch=window.findChild<ExploreWatchlist*>("exploreWatchlist");
        QVERIFY(views&&sections&&search&&watch);sections->setCurrentIndex(sections->findData("all"));
        views->setCurrentIndex(views->findData(route=="map"?"map":"list"));
        watch->toggle("coin","BTC");watch->toggle("coin","PUMP");watch->setSection("coin");
        if(route=="search") {search->setText("BTC");auto* table=window.findChild<QTableWidget*>("exploreMarkets");QVERIFY(table);for(int row=0;row<table->rowCount();++row)if(!table->isRowHidden(row)){table->setCurrentCell(row,0);break;}}
        QTRY_VERIFY(!board.busy()&&!catalog.busy());
        boardHttp.holdReplies=true;catalogHttp.holdReplies=true;
        QCoreApplication::processEvents();
        auto* footer=window.findChild<ExploreEvidenceFooter*>("exploreBoardEvidenceFooter");
        QVERIFY2(footer,"Public prices/calculations retain their internal typed source book");
        QTRY_VERIFY(footer->isHidden());QVERIFY(footer->evidenceCount()>0);
        auto* footerScroll=window.findChild<QScrollArea*>("exploreBoardEvidenceScroll");QVERIFY(footerScroll);
        QCOMPARE(footerScroll->height(),0);QVERIFY(footerScroll->isHidden());
        QVERIFY(footer->findChildren<QLabel*>("metricEvidenceLine").isEmpty());
        const auto text=footerFacts(footer);QVERIFY(text.contains("[1]"));QVERIFY(text.contains(" · 원천 "));QVERIFY(text.contains(" · 수신 "));QVERIFY(text.contains("KST"));
        if(route=="search") {
            QCOMPARE(footer->evidenceCount(),1);QVERIFY(text.contains("업비트"));
            const auto rows=catalog.rows();const auto it=std::find_if(rows.cbegin(),rows.cend(),[](const auto& r){return r.base=="BTC";});QVERIFY(it!=rows.cend());const auto row=*it;
            const auto source=QDateTime::fromMSecsSinceEpoch(row.sourceTimeMs,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss");
            const auto received=QDateTime::fromMSecsSinceEpoch(row.receivedAtMs,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss");
            QVERIFY(text.contains(source));QVERIFY(text.contains(received));
            auto* table=window.findChild<QTableWidget*>("exploreMarkets");const auto* price=table->item(table->currentRow(),2);QCOMPARE(price->text(),QString("95,000,000"));QVERIFY(referenceFree(price->data(Qt::AccessibleDescriptionRole).toString()));QCOMPARE(price->data(EvidenceNumbersRole).value<QList<int>>(),QList<int>{1});
            for(int column:{2,3}){const auto* cell=table->item(table->currentRow(),column);const auto display=column==3?QString::fromUtf8("▼ ")+cell->text():cell->text();const qreal needed=ledgerGlyphWidth(display)+24;QVERIFY2(table->columnWidth(column)>=needed,qPrintable(QString("Full price/change and reference need %1px; column has %2px").arg(needed).arg(table->columnWidth(column))));}
        } else {
            QVERIFY(text.contains("CoinGecko"));QVERIFY(text.contains("네이버 증권"));QVERIFY(text.contains("Nasdaq"));QVERIFY(text.contains("원천 미제공"));
            if(route=="map")QVERIFY(referenceFree(window.findChild<MarketMapWidget*>("marketMap")->accessibleDescription()));
            else {QVERIFY(referenceFree(window.findChild<SpotlightCard*>("spotlightCard_coin")->accessibleDescription()));auto* table=watch->findChild<QTableWidget*>("watchlistTable");QVERIFY(referenceFree(table->item(0,2)->data(Qt::AccessibleDescriptionRole).toString()));QVERIFY(!table->item(0,2)->data(EvidenceNumbersRole).value<QList<int>>().isEmpty());for(int column:{2,3}){const auto* cell=table->item(0,column);const qreal needed=ledgerGlyphWidth(cell->text())+24;QVERIFY(table->columnWidth(column)>=needed);}}
        }
        for(const auto* privateOrIdentity:{"FAKE_","scope:","binding","KRW-BTC","005930","NVDA","https://"})QVERIFY2(!text.contains(privateOrIdentity),privateOrIdentity);
        for(int n=1;n<=footer->evidenceCount();++n)QVERIFY(text.contains(QString("[%1]").arg(n)));
        // The ranking opens the chart instead of a selected panel; the other routes keep theirs.
        if(route!="list"){auto* selectedPanel=window.findChild<AssetDetailPanel*>(route=="map"?"marketMapDetail":"exploreDetail");QVERIFY(selectedPanel);
            QTRY_VERIFY(selectedPanel->findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter")->isHidden());}
        else QVERIFY(!window.findChild<AssetDetailPanel*>("spotlightDetail"));
        if (route == "map") {
            sections->setCurrentIndex(sections->findData("coin"));
            QTRY_COMPARE(footer->evidenceCount(), 1);
            QCOMPARE(footer->entries().first().provider, QString("CoinGecko"));
            sections->setCurrentIndex(sections->findData("all"));
            QTRY_COMPARE(footerFacts(footer), text);
        }
        const auto out=qEnvironmentVariable("KEEFI_D4_LEGACY_RENDER_DIR");if(!out.isEmpty()){QDir().mkpath(out);QTest::qWait(30);const auto pixmap=window.grab();QCOMPARE(pixmap.toImage().pixelColor(0,0),themeColors().bg);QVERIFY(pixmap.save(out+QString("/legacy-%1-%2-%3.png").arg(route,theme==int(ThemeMode::Light)?"light":"dark").arg(width)));}
        QTRY_VERIFY(!board.busy()&&!catalog.busy());QCoreApplication::processEvents();
        // Compare the same completed public observations across lock. A new
        // public reply is allowed to update timestamps, but is a different test.
        boardHttp.holdReplies=true;catalogHttp.holdReplies=true;
        const auto before=footerFacts(footer);window.lockAndClear();QCoreApplication::processEvents();QCOMPARE(footerFacts(footer),before);QVERIFY(footer->isHidden());QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
    void sourceBookFollowsVisibleFavouriteOrderAndDynamicPages_data() {
        QTest::addColumn<int>("theme");QTest::newRow("light")<<int(ThemeMode::Light);QTest::newRow("dark")<<int(ThemeMode::Dark);
    }
    void sourceBookFollowsVisibleFavouriteOrderAndDynamicPages() {
        QFETCH(int,theme);QTemporaryDir temp;QVERIFY(temp.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;
        boardHttp.coinOverride=R"([{"id":"bitcoin","symbol":"btc","name":"Synthetic Bitcoin","market_cap":6000000,"current_price":100,"price_change_percentage_24h":6},{"id":"ethereum","symbol":"eth","name":"Synthetic Ethereum","market_cap":5000000,"current_price":90,"price_change_percentage_24h":5},{"id":"solana","symbol":"sol","name":"Synthetic Solana","market_cap":4000000,"current_price":80,"price_change_percentage_24h":4},{"id":"avalanche","symbol":"avax","name":"Synthetic Avalanche","market_cap":3000000,"current_price":70,"price_change_percentage_24h":3},{"id":"cardano","symbol":"ada","name":"Synthetic Cardano","market_cap":2000000,"current_price":60,"price_change_percentage_24h":2},{"id":"zcash","symbol":"zec","name":"Synthetic Zcash","market_cap":1000000,"current_price":50,"price_change_percentage_24h":1}])";
        PublicEquityBoard board(&boardHttp);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);window.resize(1440,1050);window.show();KeefiTheme::instance()->setMode(ThemeMode(theme));board.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(board.items("us").size(),2,4000);QTRY_VERIFY_WITH_TIMEOUT(!catalog.rows().isEmpty()&&catalog.rows().first().priced,4000);
        auto* views=window.findChild<QComboBox*>("exploreView");views->setCurrentIndex(views->findData("list"));QTest::qWait(40);
        auto* watch=window.findChild<ExploreWatchlist*>("exploreWatchlist");watch->toggle("coin","PUMP");watch->toggle("coin","BTC");watch->setSection("coin");
        auto* table=watch->findChild<QTableWidget*>("watchlistTable");auto* footer=window.findChild<ExploreEvidenceFooter*>("exploreBoardEvidenceFooter");QVERIFY(table&&footer);
        auto* coins=window.findChild<SpotlightCard*>("spotlightCard_coin");QVERIFY(coins);coins->setMode("cap");
        // The ranking always shows its three markets in screen order; the watch-only Upbit quote follows them.
        QTRY_VERIFY(footerFacts(footer).startsWith("[1] CoinGecko"));QTRY_VERIFY(footerFacts(footer).contains("[2] 네이버 증권"));QVERIFY(footerFacts(footer).contains("[3] Nasdaq"));QTRY_VERIFY(footerFacts(footer).contains("[4] 업비트"));
        QCOMPARE(table->item(0,1)->data(Qt::UserRole).toString(),QString("PUMP"));QCOMPARE(table->item(0,2)->data(EvidenceNumbersRole).value<QList<int>>(),QList<int>{4});
        // The compact rail has no sortable header. Removing and saving an item
        // again changes its visible saved order without changing its source.
        QVERIFY(watch->compact());QVERIFY(table->horizontalHeader()->isHidden());
        watch->toggle("coin","PUMP");watch->toggle("coin","PUMP");
        QTRY_COMPARE(table->item(0,1)->data(Qt::UserRole).toString(),QString("BTC"));QTRY_COMPARE(table->item(0,2)->data(EvidenceNumbersRole).value<QList<int>>(),QList<int>{1});QVERIFY(referenceFree(table->item(0,2)->data(Qt::AccessibleDescriptionRole).toString()));
        QVERIFY(footerFacts(footer).contains("[4] 업비트"));
        watch->toggle("coin","BTC");watch->toggle("coin","BTC");
        QTRY_COMPARE(table->item(0,1)->data(Qt::UserRole).toString(),QString("PUMP"));QTRY_COMPARE(table->item(0,2)->data(EvidenceNumbersRole).value<QList<int>>(),QList<int>{4});
        // Stacked markets choose three to five rows from the actual height.
        // Six cap rows fit on one logical page and scroll inside the same source book.
        QVERIFY(coins->rowsPerPage()>=3&&coins->rowsPerPage()<=5);QCOMPARE(coins->pageCount(),1);
        auto* scroll=coins->findChild<QScrollArea*>("spotlightRowScroll");QVERIFY(scroll);scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());QCOMPARE(coins->page(),0);QTRY_VERIFY(coins->accessibleDescription().contains("ZEC"));QVERIFY(referenceFree(coins->accessibleDescription()));
        QVERIFY(footerFacts(footer).startsWith("[1] CoinGecko"));QVERIFY(!window.findChild<AssetDetailPanel*>("spotlightDetail"));
        QTRY_VERIFY(!board.busy()&&!catalog.busy());QCoreApplication::processEvents();boardHttp.holdReplies=true;catalogHttp.holdReplies=true;
        const auto before=footerFacts(footer);window.lockAndClear();QCoreApplication::processEvents();QCOMPARE(footerFacts(footer),before);QVERIFY(!vault.exists());
    }
    void legacyExplorationUsesRulesAndTextSymbols_data() {
        QTest::addColumn<int>("theme");QTest::addColumn<QString>("surface");
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const auto& surface:{QString("avatar"),QString("card"),QString("watch"),QString("map"),QString("search"),QString("navigation")})
            QTest::newRow(qPrintable(QString("%1-%2").arg(mode==ThemeMode::Light?"light":"dark",surface)))<<int(mode)<<surface;
    }
    void legacyExplorationUsesRulesAndTextSymbols() {
        QFETCH(int,theme);QFETCH(QString,surface);QTemporaryDir temp;QVERIFY(temp.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic Legacy Design");QCoreApplication::setApplicationName("Synthetic Legacy Design");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);window.setSecurityStatus(true);window.resize(1440,1050);window.show();KeefiTheme::instance()->setMode(ThemeMode(theme));board.refresh();
        QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);QTRY_COMPARE(board.items("us").size(),2);
        auto* views=window.findChild<QComboBox*>("exploreView");views->setCurrentIndex(views->findData(surface=="map"?"map":"list"));
        if(surface=="search")window.findChild<QLineEdit*>("exploreSearch")->setText("BTC");
        QTRY_VERIFY(!board.busy()&&!catalog.busy());boardHttp.holdReplies=true;catalogHttp.holdReplies=true;QCoreApplication::processEvents();
        auto* card=window.findChild<SpotlightCard*>("spotlightCard_coin");card->setSelected({});QTest::mouseMove(&window,QPoint(0,0));QTest::qWait(20);
        if(surface=="avatar") {
            auto* rows=card->findChild<QWidget*>("spotlightRows");QVERIFY(rows);
            const auto pixmap=rows->grab();const auto image=pixmap.toImage();
            // The former thirty-pixel colored initial circle occupied this
            // empty gutter. The symbol itself remains readable as plain text.
            QCOMPARE(image.pixelColor(QPoint(36,42)*pixmap.devicePixelRatio()),themeColors().bg);
            QVERIFY(card->accessibleDescription().contains("BTC"));
        } else if(surface=="navigation") {
            auto* control=card->findChild<QPushButton*>("spotlightOpen");
            const auto pixmap=window.grab();const auto image=pixmap.toImage();
            // Read the actual composed screen. A transparent control's own
            // isolated grab correctly has alpha0 rather than its parent's ink.
            QCOMPARE(image.pixelColor(control->mapTo(&window,QPoint(2,control->height()/2))*pixmap.devicePixelRatio()),themeColors().bg);
        } else {
            QWidget* widget=surface=="card"?static_cast<QWidget*>(card):surface=="watch"?window.findChild<QWidget*>("exploreWatchlist"):surface=="map"?window.findChild<QWidget*>("marketMapCard"):window.findChild<QWidget*>("exploreMarkets");
            QVERIFY(widget);const auto pixmap=widget->grab();const auto image=pixmap.toImage();
            // Ranking and saved-list panels now use the approved flat 1px
            // enclosure. Existing map/search ledgers retain their top rule.
            const bool enclosed=surface=="card"||surface=="watch";
            QCOMPARE(image.pixelColor(QPoint(0,widget->height()/2)*pixmap.devicePixelRatio()),enclosed?themeColors().rule:themeColors().bg);
            QCOMPARE(image.pixelColor(QPoint(widget->width()/2,0)*pixmap.devicePixelRatio()),themeColors().rule);
        }
        QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
    void rankingFitsThreeMarketsBesideTheWatchlistAndOpensTheChart() {
        QTemporaryDir temp;QVERIFY(temp.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());Vault vault(temp.filePath("never-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,f);
        LegacyCatalogTransport catalogHttp;PublicMarketCatalog catalog(&catalogHttp);LegacyBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
        struct Silent final:TradingTransport{void send(quint64,const RequestSpec& request)override{QVERIFY(!request.authenticated);}void abort()override{}} candleHttp,metadata;
        MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setPublicMarketCatalog(&catalog,true);window.setPublicEquityBoard(&board);
        window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);window.setSecurityStatus(true);window.resize(1920,1080);window.show();board.refresh();
        QTRY_VERIFY(!catalog.rows().isEmpty()&&catalog.rows().first().priced);QTRY_COMPARE(board.items("coin").size(),2);QTRY_COMPARE(board.items("kr").size(),2);
        auto* views=window.findChild<QComboBox*>("exploreView");auto* sections=window.findChild<QComboBox*>("exploreMarketSection");auto* tabs=window.findChild<QWidget*>("exploreMarketTabs");QVERIFY(views&&sections&&tabs);
        // The market filter belongs to the map: choosing 코인 there leaves the ranking with all three markets.
        views->setCurrentIndex(views->findData("map"));QTRY_VERIFY(tabs->isVisibleTo(&window));sections->setCurrentIndex(sections->findData("coin"));
        views->setCurrentIndex(views->findData("list"));QTRY_VERIFY(!tabs->isVisibleTo(&window));
        QList<SpotlightCard*> cards;for(const auto* key:{"coin","kr","us"}){auto* card=window.findChild<SpotlightCard*>(QString("spotlightCard_")+key);QVERIFY(card);cards.append(card);}
        for(auto* card:cards)QTRY_VERIFY(card->isVisibleTo(&window));
        auto* venue = cards[0]->findChild<QComboBox*>("rankingCoinVenue");
        QVERIFY(venue);
        const auto fitsHeader = [&] {
            int right = 0;
            for (const auto* id : {"spotlightTitle", "rankingCoinVenue", "spotlightMode", "spotlightStatus", "spotlightOpen"}) {
                const auto* control = cards[0]->findChild<QWidget*>(id);
                if (!control || !control->isVisible()) return false;
                const auto bounds = control->geometry();
                if (bounds.left() <= right || bounds.right() >= cards[0]->width()
                    || bounds.width() < control->minimumSizeHint().width()) return false;
                right = bounds.right();
            }
            return true;
        };
        // 1920px: the three markets side by side, the watchlist in the common right rail, no scrolling.
        auto* watch=window.findChild<ExploreWatchlist*>("exploreWatchlist");auto* outer=window.findChild<QScrollArea*>("spotlightListScroll");auto* scroll=window.findChild<QScrollArea*>("spotlightScroll");QVERIFY(watch&&outer&&scroll);
        auto* panels=window.findChild<QWidget*>("explorePanels");QVERIFY(panels);
        const auto x=[&](QWidget* w){return w->mapTo(&window,QPoint()).x();};const auto y=[&](QWidget* w){return w->mapTo(&window,QPoint()).y();};
        QTRY_VERIFY(y(cards[0])==y(cards[1])&&y(cards[1])==y(cards[2])&&x(cards[0])<x(cards[1])&&x(cards[1])<x(cards[2]));
        for (int i = 1; i < cards.size(); ++i) {
            QTRY_COMPARE(cards[0]->height(), cards[i]->height());
            QVERIFY(qAbs(cards[0]->width() - cards[i]->width()) <= 1); // Integer grid rounding.
            const auto rowTop = [](SpotlightCard* card) {
                return card->findChild<QScrollArea*>("spotlightRowScroll")->mapTo(card, QPoint()).y();
            };
            QCOMPARE(rowTop(cards[0]), rowTop(cards[i]));
        }
        venue->setCurrentIndex(1);
        cards[0]->setMode("cap");
        QTRY_VERIFY(fitsHeader());
        venue->setCurrentIndex(0);
        QCOMPARE(cards[0]->mode(), QString("gainers"));
        QTRY_VERIFY(fitsHeader());
        QTRY_VERIFY(x(watch)>=x(cards[2])+cards[2]->width());QTRY_COMPARE(watch->width(),LocalWindow::rankingWatchlistWidth(panels->width()));QVERIFY(watch->compact());
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(),0);QCOMPARE(outer->verticalScrollBar()->maximum(),0);
        const int tallRows=cards[0]->rowsPerPage();QVERIFY(tallRows>=3&&tallRows<=20);
        for(auto* card:cards)QVERIFY(y(card)+card->height()<=y(scroll)+scroll->viewport()->height());
        window.resize(1920,760);QTRY_VERIFY(cards[0]->rowsPerPage()<tallRows);QVERIFY(cards[0]->rowsPerPage()>=3);QTRY_COMPARE(scroll->verticalScrollBar()->maximum(),0);
        // 1440px: too narrow for three 400px columns, so the markets stack with fewer rows, still without scrolling.
        window.resize(1440,1000);QTRY_VERIFY(y(cards[0])<y(cards[1])&&y(cards[1])<y(cards[2]));QTRY_VERIFY(x(watch)>=x(cards[0])+cards[0]->width());
        QVERIFY(cards[0]->rowsPerPage()>=3&&cards[0]->rowsPerPage()<=5);QTRY_COMPARE(scroll->verticalScrollBar()->maximum(),0);QVERIFY(watch->compact());
        window.resize(1440,900);QTRY_VERIFY(cards[0]->rowsPerPage()>=3&&cards[0]->rowsPerPage()<=5);QTRY_COMPARE(scroll->verticalScrollBar()->maximum(),0);QCOMPARE(outer->verticalScrollBar()->maximum(),0);
        for (int i = 1; i < cards.size(); ++i) QTRY_COMPARE(cards[0]->size(), cards[i]->size());
        for(auto* card:cards)QVERIFY(y(card)+card->height()<=y(scroll)+scroll->viewport()->height());
        // At the supported minimum size, vertical scrolling is allowed; the
        // compact saved list still occupies the common right rail.
        window.resize(1280,800);QTRY_VERIFY(x(watch)>=x(cards[0])+cards[0]->width());QVERIFY(watch->compact());
        QTRY_VERIFY(fitsHeader());
        for(auto* card:cards)QVERIFY(card->isVisibleTo(&window));
        QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);QCOMPARE(outer->horizontalScrollBar()->maximum(),0);
        window.resize(1920,1080);QTRY_VERIFY(y(cards[0])==y(cards[1])&&y(cards[1])==y(cards[2]));QVERIFY(watch->compact());
        // A row opens the chart: Upbit KRW for BTC; no Upbit or Binance market (ZEC) or no broker (stocks) gives a reason instead.
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* chart=window.findChild<ChartWorkspace*>("chartWorkspace");auto* explore=window.findChild<QWidget*>("explorePage");auto* feedback=window.findChild<QLabel*>("localFeedback");
        QVERIFY(pages&&chart&&explore&&feedback);const auto shown=[&]{return chart->selectedChartIdentity();};
        const auto click=[&](SpotlightCard* card,const QString& id){const auto items=card->visibleItems();int row=-1;for(int i=0;i<items.size();++i)if(items[i].id==id)row=i;QVERIFY(row>=0);
            auto* rows=card->findChild<QWidget*>("spotlightRows");QTest::mouseClick(rows,Qt::LeftButton,Qt::NoModifier,QPoint(rows->width()/3,6+row*SpotlightCard::RowHeight+SpotlightCard::RowHeight/2));};
        chart->setIdentity("coin:upbit:KRW-PUMP",true);QCOMPARE(shown(),QString("coin:upbit:KRW-PUMP"));
        click(cards[0],"BTC");QTRY_COMPARE(pages->currentWidget(),static_cast<QWidget*>(chart));QCOMPARE(shown(),QString("coin:upbit:KRW-BTC"));
        pages->setCurrentWidget(explore);QTRY_VERIFY(cards[0]->isVisibleTo(&window));
        cards[0]->setMode("cap"); // ZEC belongs only to this fixture’s global cap list.
        click(cards[0],"ZEC");QCOMPARE(pages->currentWidget(),explore);QVERIFY2(feedback->text().contains("ZEC")&&feedback->text().contains(QStringLiteral("차트를 열 수 없어요")),qPrintable(feedback->text()));
        click(cards[1],board.items("kr").first().symbol);QCOMPARE(pages->currentWidget(),explore);QVERIFY2(feedback->text().startsWith(QStringLiteral("주식 차트 · ")),qPrintable(feedback->text()));
        auto* guide=window.findChild<QWidget*>("chartConnectionGuide");QVERIFY(guide);guide->close();QCoreApplication::processEvents();
        // A saved Upbit-only coin opens the same way from the watchlist column.
        watch->toggle("coin","PUMP");watch->setSection("coin");auto* table=watch->findChild<QTableWidget*>("watchlistTable");QTRY_COMPARE(table->rowCount(),1);
        QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,table->visualItemRect(table->item(0,1)).center());
        QTRY_COMPARE(pages->currentWidget(),static_cast<QWidget*>(chart));QCOMPARE(shown(),QString("coin:upbit:KRW-PUMP"));
        QVERIFY(!vault.exists());QCOMPARE(vault.accounts().size(),0);
    }
};
QTEST_MAIN(D4LegacyDesignTest)
#include "tst_d4_legacy_design.moc"
