#include "fixtures/PortfolioTestRow.h"
#include "ChartInstrumentPanelTestSupport.h"
#include "keefi/AccountDialog.h"
#include "keefi/AccountModel.h"
#include "keefi/BuildPolicy.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/LocalWindow.h"
#include "keefi/ResponseJournal.h"
#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingController.h"
#include "keefi/PortfolioController.h"
#include "keefi/AssetsLedger.h"
#include "keefi/AssetListFilter.h"
#include "keefi/RecordTransferDialog.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioAssetModel.h"
#include "keefi/PortfolioAllocation.h"
#include "fixtures/ProviderReadFixture.h"
#include "fixtures/BinanceUsdmResponses.h"
#include "keefi/MarketMetrics.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/PressRevealButton.h"
#include "keefi/MarketMap.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/Spotlight.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/MarketCandles.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/InfoButton.h"
#include <QPlainTextEdit>
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/MarketBreadthMonitor.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/ChartStudies.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/MarketFlowPresentation.h"
#include "keefi/ChartAlertFeed.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/BackgroundTray.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/StockChartSource.h"
#include "keefi/TossMarketView.h"
#include "keefi/TossMarketStream.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/KiwoomMarketView.h"
#include <QToolButton>
#include <QTabBar>
#include <QHeaderView>
#include <QListWidget>
#include <QMap>
#include <QSettings>
#include <QStackedWidget>
#include "keefi/SegmentedComboBox.h"
#include <QWheelEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QUrlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QStackedWidget>

#include <QCheckBox>
#include <QApplication>
#include <QAccessible>
#include <QStandardPaths>
#include <QFileDialog>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDateTime>
#include <QTimeZone>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QLabel>
#include <QProgressBar>
#include <vector>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QScrollBar>
#include <QScrollArea>
#include <QSplitter>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTimer>
#include <QTextDocument>
#include <functional>

using keefi::test::PortfolioTestRow;
using keefi::test::portfolioRow;
using keefi::test::selectedPortfolioRow;

using namespace keefi;

namespace {
template <typename T> T* child(QObject& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}
void fillFakeKeys(AccountDialog& dialog, const QString& providerId, const QString& suffix = "first") {
    auto* selector = child<QComboBox>(dialog, "providerSelector");
    selector->setCurrentIndex(selector->findData(providerId));
    child<QLineEdit>(dialog, "accountAlias")->setText(QStringLiteral("가짜 계좌"));
    const auto* provider = AccountModel::provider(providerId);
    for (const auto& field : provider->fields) {
        auto* input = dialog.findChild<QLineEdit*>("credential_" + field.key);
        input->setText("FAKE-ONLY-" + field.key + "-" + suffix);
    }
}
// Dialogs are not shown in these tests, so input is delivered directly.
void sendMouse(QWidget* widget, QEvent::Type type, QPoint position, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(position), widget->mapToGlobal(QPointF(position)),
                      Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &event);
}
void holdReveal(QWidget* button) { sendMouse(button, QEvent::MouseButtonPress, {2, 2}, Qt::LeftButton); }
bool sendKey(QWidget* widget, QEvent::Type type, int key, bool autoRepeat = false, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(type, key, modifiers, QString(), autoRepeat);
    QCoreApplication::sendEvent(widget, &event);
    return event.isAccepted();
}
void processDeletion() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}
class NoNetworkBrokerTransport final : public TradingTransport {
  public:
    int sent = 0;
    void send(quint64, const RequestSpec&) override { ++sent; }
    void abort() override {}
};
class CoinWindowPublicTransport final:public TradingTransport {
  public:
    QList<RequestSpec> requests;int aborts=0;quint64 token=0;
    void send(quint64 value,const RequestSpec& request)override{token=value;QVERIFY(!request.authenticated);QVERIFY(request.method=="GET");requests.append(request);}
    void abort()override{++aborts;}
};
class StockWindowMarketTransport final:public TradingTransport {
  public:
    QList<RequestSpec> requests;int aborts=0;quint64 token=0;
    void send(quint64 value,const RequestSpec& request)override{token=value;QVERIFY(StockChartSource::isRequest(request));requests.append(request);}
    void abort()override{++aborts;}
};
class StockWindowSocket final:public TossMarketSocket {
  public:
    int opens=0;void open(QByteArrayView)override{++opens;}
    void send(const QByteArray&)override{}void abort()override{}
};
class KiwoomWindowSocket final:public KiwoomMarketSocket {
  public:
    int opens=0;
    void open(quint64,const QString&)override{++opens;}
    void sendLogin(quint64,QByteArrayView)override{}
    void sendControl(quint64,const QByteArray&)override{}
    void close(quint64 generation)override{emit closed(generation);}
    void abort()override{}
};
class KiwoomWindowMarketTransport final:public TradingTransport {
  public:
    QList<RequestSpec> requests;int aborts=0;
    void send(quint64,const RequestSpec& request)override{QVERIFY(KiwoomMarketStream::isRequest(request));requests.append(request);}
    void abort()override{++aborts;}
};
class AssetReadTransport final:public TradingTransport {
public:
    int sent=0;bool fail=false,hold=false;
    QJsonArray balances;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!isMutation(request.operation));++sent;HttpResult response;response.status=200;
        if(request.operation==Operation::Time)response.body=QJsonDocument(QJsonObject{{"serverTime",QString::number(QDateTime::currentMSecsSinceEpoch())}}).toJson();
        else if(fail){response.status=401;response.body=R"({"code":-2015,"msg":"PRIVATE_RAW_MESSAGE"})";}
        else if (!balances.isEmpty()) response.body=QJsonDocument(QJsonObject{{"canTrade",true},{"accountType","SPOT"},{"balances",balances}}).toJson();
        else response.body=R"({"canTrade":true,"accountType":"SPOT","balances":[{"asset":"BTC","free":"1","locked":"0"}]})";
        if(!hold)QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class FailedPublicPriceTransport final:public TradingTransport {
public:
    int sent=0;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override {
        QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());++sent;
        HttpResult response;response.status=request.operation==Operation::Markets?200:503;
        response.body=request.operation==Operation::Markets?QByteArray(R"([{"market":"KRW-BTC"}])"):QByteArray("FAKE_PRIVATE_PRICE_ERROR");
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class ReferencePublicPriceTransport final:public TradingTransport {
public:
    bool fresh=false;int sent=0;qint64 sourceTime=0;
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override {
        QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());QVERIFY(NetworkTradingTransport::allowed(request));++sent;
        HttpResult response;response.status=200;const auto now=QDateTime::currentMSecsSinceEpoch();
        sourceTime=fresh?now:now-600000; // Older than the 180 s display window.
        response.body=QJsonDocument(QJsonArray{request.operation==Operation::Markets?QJsonObject{{"market","KRW-PUMP"}}:
            QJsonObject{{"market","KRW-PUMP"},{"trade_price","2.75"},{"timestamp",QString::number(sourceTime)}}}).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
// Fresh public KRW prices for whatever markets are requested.
class FreshKrwPriceTransport final:public TradingTransport {
public:
    QMap<QString,QString> prices{{"USDT","1385"},{"DUST","0.5"},{"BIG","100000"}};
    qint64 sourceTime=0; // Optional fixed source time for evidence-expiry tests.
    qint64 sourceTimeLeadMs=0; // Set relative to the actual quote request, after UI setup.
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override {
        QVERIFY(!request.authenticated);
        HttpResult response;response.status=200;const auto now=QDateTime::currentMSecsSinceEpoch();QJsonArray rows;
        if(request.operation!=Operation::Markets&&sourceTimeLeadMs>0)sourceTime=now+sourceTimeLeadMs;
        if(request.operation==Operation::Markets){for(auto it=prices.cbegin();it!=prices.cend();++it)rows.append(QJsonObject{{"market","KRW-"+it.key()}});}
        else for(const auto& market:QUrlQuery(request.url).queryItemValue("markets").split(',',Qt::SkipEmptyParts))
            rows.append(QJsonObject{{"market",market},{"trade_price",prices.value(market.mid(4))},{"timestamp",QString::number(sourceTime>0?sourceTime:now)}});
        response.body=QJsonDocument(rows).toJson();
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
// Synthetic account/public-price responses that can be held across unlock.
// No connection, authentication header or profile is read by this fixture.
class DeferredPortfolioTransport final : public TradingTransport {
  public:
    explicit DeferredPortfolioTransport(bool publicPrices) : publicPrices_(publicPrices) {}
    bool hold = false, fail = false, supported = true;
    QString asset = "USDT", quantity = "10", price = "1000";
    int extraZeroRows = 0;
    QStringList supportedAssets{"USDT"}, quoteRequests;
    QList<QPair<quint64, HttpResult>> pending;
    void abort() override { pending.clear(); }
    void release(bool keepHolding = false) {
        hold = keepHolding;
        const auto responses = pending; pending.clear();
        for (const auto& response : responses)
            QTimer::singleShot(1, this, [this, response] { emit finished(response.first, response.second); });
    }
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(!isMutation(request.operation));
        HttpResult response; response.status = 200;
        const auto now = QString::number(QDateTime::currentMSecsSinceEpoch());
        if (publicPrices_) {
            QVERIFY(!request.authenticated); QVERIFY(request.headers.isEmpty());
            if (request.operation == Operation::Markets) {
                QJsonArray markets;
                for (const auto& supportedAsset : supported ? supportedAssets : QStringList{"BTC"})
                    markets.append(QJsonObject{{"market","KRW-"+supportedAsset}});
                response.body = QJsonDocument(markets).toJson();
            }
            else if (fail) { response.status = 503; response.body = "FAKE_PRIVATE_PRICE_FAILURE"; }
            else {
                QJsonArray quotes;
                const auto markets = QUrlQuery(request.url).queryItemValue("markets").split(',',Qt::SkipEmptyParts);
                quoteRequests.append(markets);
                for (const auto& market : markets)
                    quotes.append(QJsonObject{{"market",market},{"trade_price",price},{"timestamp",now}});
                response.body = QJsonDocument(quotes).toJson();
            }
        } else if (request.operation == Operation::Time) {
            response.body = QJsonDocument(QJsonObject{{"serverTime",now}}).toJson();
        } else if (fail) {
            response.status = 401; response.body = R"({"code":-2015,"msg":"FAKE_PRIVATE_ACCOUNT_FAILURE"})";
        } else {
            QJsonArray balances{QJsonObject{{"asset",asset},{"free",quantity},{"locked","0"}}};
            for(int i=0;i<extraZeroRows;++i)balances.append(QJsonObject{{"asset",QString("ZERO%1").arg(i)},{"free","0"},{"locked","0"}});
            response.body = QJsonDocument(QJsonObject{{"canTrade",true},{"accountType","SPOT"},
                {"balances",balances}}).toJson();
        }
        // Hold balance/price delivery, not the clock handshake whose round trip
        // intentionally rejects delays over two seconds.
        if (hold && request.operation != Operation::Time) pending.append({token,response});
        else QTimer::singleShot(1, this, [this, token, response] { emit finished(token,response); });
    }
  private:
    bool publicPrices_;
};
// Keyless public boards: CoinGecko, Naver 증권 and Nasdaq shaped fixtures.
class FakeBoardTransport final:public TradingTransport {
public:
    QStringList paths;
    QByteArray coinOverride;
    bool deferUsStatus = false;
    quint64 deferredUsStatusToken = 0;
    HttpResult deferredUsStatusResponse;
    void releaseUsStatusAfter(int delayMs) {
        QVERIFY(deferredUsStatusToken != 0);
        const auto token = deferredUsStatusToken;
        const auto response = deferredUsStatusResponse;
        deferredUsStatusToken = 0;
        QTimer::singleShot(delayMs, this, [this, token, response] { emit finished(token, response); });
    }
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
        response.body=request.url.host()=="api.coingecko.com"?(coinOverride.isEmpty()?coins():coinOverride):request.url.host()=="m.stock.naver.com"?korea():
            request.url.path()=="/api/market-info"?QByteArray(R"({"data":{"mrktStatus":"After-Hours"}})"):us();
        if (deferUsStatus && request.url.path() == "/api/market-info") {
            QVERIFY(deferredUsStatusToken == 0);
            deferredUsStatusToken = token;
            deferredUsStatusResponse = response;
            return;
        }
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
class PublicMarketTransport final:public TradingTransport {
public:
    QList<RequestSpec> requests;int quoteStatus=200;
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
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
};
// Explicit public exchangeInfo evidence for synthetic row subscription tests.
// A syntactically valid ticker by itself must never authorize a subscription.
class PublicBinanceRowMetadata final:public TradingTransport {
public:
    void abort()override{}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QCOMPARE(request.operation,Operation::Markets);
        QCOMPARE(request.url.host(),QString("api.binance.com"));
        QCOMPARE(request.url.path(),QString("/api/v3/exchangeInfo"));
        QJsonArray symbols;
        for(const auto& base:{QString("SYN"),QString("ALT"),QString("THIRD"),QString("OTHER")})
            symbols.append(QJsonObject{{"symbol",base+"USDT"},{"baseAsset",base},{"quoteAsset","USDT"},
                {"status","TRADING"},{"isSpotTradingAllowed",true}});
        symbols.append(QJsonObject{{"symbol","SYNUSDC"},{"baseAsset","SYN"},{"quoteAsset","USDC"},
            {"status","TRADING"},{"isSpotTradingAllowed",true}});
        HttpResult result;result.status=200;result.body=QJsonDocument(QJsonObject{{"symbols",symbols}}).toJson();
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
};
} // namespace

class LocalWindowTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDirectory_;
    QList<QUrl> openedGuideUrls_;
  public slots:
    void recordGuideUrl(const QUrl& url) { openedGuideUrls_.append(url); }
  private slots:
    void connectionsPageOpensGuidesWithoutUnlocking_data();
    void connectionsPageOpensGuidesWithoutUnlocking();
    void initTestCase() {
        QVERIFY(settingsDirectory_.isValid());
        // Selected tests must not depend on an earlier case to isolate default QSettings.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.filePath("settings"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory_.filePath("system-settings"));
    }
    void accessibleHeaderSelectionChangesPageAndStaysExclusive();
    void accessiblePublicControlsDriveContent_data();
    void accessiblePublicControlsDriveContent();
    void publicObservationChoicesStayOffAndReleaseHiddenDemands();
    void rankingStudySelectionOwnsOnlyVisibleRows();
    void chartTabKeepsPublicCandlesWhileVaultLocked();
    void coinFlowDemandFollowsVisibleChartAndStopsOnTeardown();
    void coinFlowMinuteModelRequiresCurrentPublicUpbitCatalog();
    void coinFlowWindowSettlesAfterOverlappingMinuteConsumers();
    void coinFlowRawTradeBurstDoesNotSynchronouslyRefreshLists();
    void hiddenCoinFlowStopsWhenItsPublicAlertFeedStops();
    void chartAlertFeedUsesOnlyCurrentVerifiedUpbitMarkets();
    void coinFlowWatchlistBadgeTracksEnabledIndependentMinuteStudy();
    void brokerConnectionReturnsToOrigin_data();
    void brokerConnectionReturnsToOrigin();
    void stockConnectionGuidanceUsesExistingState_data();
    void stockConnectionGuidanceUsesExistingState();
    void chartInstrumentNamesAreDisplayOnly_data();
    void chartInstrumentNamesAreDisplayOnly();
    void stockChartRequiresBrokerWhileLockedAndKeepsSavedPublicRows_data();
    void stockChartRequiresBrokerWhileLockedAndKeepsSavedPublicRows();
    void stockSessionPreferencePersistsOnlyThePublicChoice();
    void stockSourcesShareTheSelectedExistingTossOwnerAndRevokeBeforeLock();
    void kiwoomDisplayUsesOneInjectedSourceAndStaysOffWithoutAuthorization();
    void publicChartWithStoppedKiwoomSourceSettlesAfterQueuedEvents();
    void kiwoomDisplayBorrowsTheSelectedExistingOwnerAndRevokesBeforeLock();
    void stockSourceTeardownCannotRebindThroughForwardedStudyChanges();
    void chartWatchlistsPersistOnlyPublicStateInInjectedProfile();
    void chartWatchlistsMigrateLegacyOnceAndLeaveUnverifiedStocksPending();
    void chartWatchlistsDeriveHoldingsFromReadyUpbitAndCannotRehydrateAfterLock();
    void chartWatchlistsUseVerifiedBrokerCountriesForMixedHoldingsAndLegacy();
    void chartWatchlistKeepsCurrentTickerPriceWhenCompletedBadgesArrive();
    void chartWatchlistDropsCandleDerivedRowsWhenSourceBecomesUnavailable();
    void chartWatchlistsLoadPublicSpotMetadataWithoutNewAccountReads_data();
    void lockedPublicBinanceSelectionLoadsMetadataAndSelectedTimeframe();
    void chartRowsShowBinancePublicPricesForShownRows();
    void chartRowsKeepLastBinanceQuoteAcrossReconnect();
    void chartRowsCompareDisconnectedTickerWithCompletedCandle_data();
    void chartRowsCompareDisconnectedTickerWithCompletedCandle();
    void chartRowsRejectOldBinanceOriginalTime();
    void chartRowsSkipUnsupportedBinanceQuotesWithoutDroppingUsdt();
    void chartRowsRequireVerifiedBinanceSpotCatalog();
    void chartStudyPreferencesPersistOnlyPublicDisplayChoices();
    void chartDrawingsPersistPublicCoordinatesInInjectedProfile();
    void chartDrawingsMigrateV1ToV2OnChangeAndKeepCorruptInRejected();
    void chartDrawingLineProjectionRequiresEveryExplicitPublicOptIn();
    void chartStudyBadgesUseOnlyCompletedBarsAndEnabledChoices();
    void chartAlertsPersistPublicPreferencesWithoutSystemAuthorization();
    void chartSettingsRetargetAndSurviveOptionsLifetime();
    void chartAlertPrivateStateIsClearedByTheWindowLockBoundary();
    void injectedTrayKeepsCloseRunningAndPreferenceOffQuits();
    void normalQuitPreparationPrecedesLockAndSecurityLockBypassesIt_data();
    void normalQuitPreparationPrecedesLockAndSecurityLockBypassesIt();
    void quitPreparationTimeoutCancelsOnceAndNeverAsks();
    void applicationQuitUsesTheBoundedPreparation();
    void chartAlertRulesRestoreAfterTheirPublicListIdentities();
    void hiddenWindowRoutesOwnedFuturesRiskWithoutExtraAccountReads();
    void chartWatchlistsLoadPublicSpotMetadataWithoutNewAccountReads();
    void chartWatchlistsPublicMetadataFailurePreservesPrivateCapture_data();
    void chartWatchlistsPublicMetadataFailurePreservesPrivateCapture();
    void recordTransferEntryRequiresVaultAndClearsOnSecurityLoss();
    void portfolioHomeShowsUnreadAccountAndStaysNavigableDuringRead();
    void hiddenPortfolioQuotesWaitForVisibleProjectionAndKeepSelection();
    void hiddenPortfolioPendingProjectionClearsAtPrivacyBoundary_data();
    void hiddenPortfolioPendingProjectionClearsAtPrivacyBoundary();
    void portfolioShowsAssetsOnlyWhileVaultUnlocked();
    void accountManagementDisplaysReadFailureReason();
    void publicPriceDetailsSeparateAccountSuccessAndClearOnLock();
    void publicPriceDiagnosticKeepsFailuresVisible_data();
    void publicPriceDiagnosticKeepsFailuresVisible();
    void publicPriceFailureSurvivesPartialStreamUpdate();
    void portfolioShowsReferencePriceAndPromotesOnlyFreshPrice();
    void coldUnlockWaitsForCurrentData_data();
    void loadingProgressCountsRealTasksAndCompletesAfterProjection();
    void loadingProgressCountsRealTasksAndCompletesAfterProjection_data();
    void loadingProgressNeverRegressesDuringAutomaticReread();
    void loadingProgressFailureAndLockStayTruthful();
    void loadingProgressWaitsForFirstStreamPriceAfterRestFinishes();
    void loadingProgressOpensAfterBoundedWaitWhenStreamPriceNeverArrives();
    void loadingProgressWaitsForExistingManualRead();
    void loadingProgressUsesFixedConnectionSet();
    void loadingProgressDropsQueuedPresentationAcrossRelock();
    void loadingHiddenProjectionAndBurstProbe();
    void loadingColdProjectionFlushesLatestOnPageChangeAndSettle();
    void coldUnlockWaitsForCurrentData();
    void coldUnlockWaitsThroughChangedAssetPriceCycle();
    void warmUnlockRejectsIncompleteOrChangedScope_data();
    void warmUnlockRejectsIncompleteOrChangedScope();
    void unlockShowsDatedSavedAssetsUntilFreshReadCompletes_data();
    void unlockShowsDatedSavedAssetsUntilFreshReadCompletes();
    void unlockWithoutPricedCacheLoadsThenExposesSettledGaps_data();
    void unlockWithoutPricedCacheLoadsThenExposesSettledGaps();
    void loginFreeEntryAndSecurityGate();
    void publicCatalogAutoLoadDisabledDoesNotRequestOnFirstShow();
    void vaultDialogKeepsModalInputBoundaryWithoutNativeSheet();
    void providerFieldsAreMaskedAndCleared();
    void okxRegionSelectionAndReplacementAreExplicit();
    void okxLocalReplacementSavesRegionAndKeysTogether();
    void kiwoomOptionalAccountNumberIsComparisonOnly();
    void suspendedBrokersKeepOnlyRemovalAction();
    void accountDialogClearsOnSuccessAndCancel();
    void connectionWizardPicksServiceTilesAndClearsOnServiceChange();
    void connectionWizardReportsSavedAndReadStatesWithoutExposingKeys();
    void passwordDialogValidationAndClear();
    void passwordCompositionRequiredOnlyForNewPasswords();
    void passwordKeyboardConversionKeepsAsciiAndRejectsUnsupportedText();
    void nativeKoreanPasswordInput();
    void pressRevealShowsOnlyWhileHeld();
    void credentialKeysTrimOnlyOuterWhiteSpace();
    void shortChoicesAreSegmentsThatSelectOnlyOnPurpose();
    void workbenchStaysHonestWithLargeAndFailingData();
    void smallBalancesHideOnlyValuedRowsBelowOneDollar();
    void publicSmallBalanceThresholdExpiresWithoutAnotherQuote();
    void displayHidingDefaultsLiveInOptionsAndPersist();
    void valuationDiagnosticsMatchTheScreenWithoutIdentities();
    void valuationDiagnosticsFollowPriceExpiryWithoutNewInputs();
    void referencePriceExpiresAfterTwentyFourHoursWithoutNewInputs();
    void futurePublicPriceBecomesCurrentWithoutNewInputs();
    void valuationExportEndsWithTheUnlockedSession_data();
    void valuationExportEndsWithTheUnlockedSession();
    void marketMapUsesKeylessPublicBoards();
    void exploreCompactAmountsUseGroupingWithoutChangingPublicData_data();
    void exploreCompactAmountsUseGroupingWithoutChangingPublicData();
    void publicStockDetailsAndWatchlistsRequireBrokerHistory_data();
    void publicStockDetailsAndWatchlistsRequireBrokerHistory();
    void spotlightPagesFiveRowsAndOptionsPinSmallBalances();
    void watchlistsPersistByMarketAndDoNotEnableOrders();
    void exploreControlsStayPutWhenSwitchingViews();
    void todayFitsTheNativeWindowWithLoadingAndPartialCalendarFailure_data();
    void todayFitsTheNativeWindowWithLoadingAndPartialCalendarFailure();
    void spotlightAndWatchlistNeverOverlap();
    void initialLockIsSilent();
    void livePublicExplorePreview();
    void dustTotalsUseExactDecimalsAndKeepUnknowns();
    void krwDisplayRoundsWithoutChangingExactAmounts_data();
    void krwDisplayRoundsWithoutChangingExactAmounts();
    void zeroFuturesEquityDoesNotBlockDustGroups_data();
    void zeroFuturesEquityDoesNotBlockDustGroups();
    void oversizedSecretsCannotBecomeValidTruncatedPrefixes();
    void accountReplaceDeleteAndLockUseVault();
    void vaultButtonsRouteCreateChangeUnlock();
    void securityLossClosesSecretInput();
    void brokerWindowEntryNeverAuthenticatesAndHonorsSecurityGate_data();
    void brokerWindowEntryNeverAuthenticatesAndHonorsSecurityGate();
    void binanceSharedCredentialsPreserveOtherMarketConnection();
    void lockedVaultBlocksSavingUntilOpened();
    void openVaultInsideAccountDialogPreservesDraft();
    void nestedVaultDialogClearsBothInputsOnSecurityLoss();
    void openVaultSavesEncryptedWithExplicitFeedback();
    void holdingsFilterUsesExactQuantitiesAndKeepsUnknowns();
    void portfolioSearchAndScopeOnlyChangeVisibility();
    void presentationGroupsWithoutChangingAmounts();
    void portfolioAllocationScopesAndProviderPnlStayHonest();
    void portfolioFiltersSortRefreshAndClearOnLock();
    void assetDetailShowsEveryStorageAndClearsOnLock();
    void connectionStatusOffersCauseSpecificRecovery_data();
    void connectionStatusOffersCauseSpecificRecovery();
    void connectionGroupsOnlyUseExplicitSharedKeys();
    void groupedConnectionsKeepMarketActionsAndPrivacy();
    void narrowWindowSwitchesBetweenListAndDetail_data();
    void narrowWindowSwitchesBetweenListAndDetail();
    void changePercentRoundsOnlyForDisplay();
    void publicExplorationWorksLockedAndLinksHoldingsByTicker();
    void publicExplorationHonorsRateLimitWithoutResending();
};

void LocalWindowTest::chartInstrumentNamesAreDisplayOnly_data(){
    QTest::addColumn<QString>("key");QTest::addColumn<QString>("expected");QTest::addColumn<bool>("dark");
    const QList<QPair<QString,QString>> cases{
        {"stock:KR:005930",QStringLiteral("삼성전자 005930 · 국내 주식")},
        {"stock:US:NVDA",QStringLiteral("NVIDIA Corporation NVDA · 미국 주식")},
        {"stock:US:SYN",QStringLiteral("SYN · 미국 주식")},
        {"coin:upbit:KRW-BTC",QStringLiteral("가짜비트 BTC/KRW · 업비트")},
        {"coin:upbit:KRW-SYN",QStringLiteral("SYN/KRW · 업비트")},
        {"coin:upbit:BTC-BTC",QStringLiteral("BTC/BTC · 업비트")},
        {"coin:binance:BTCUSDT",QStringLiteral("BTC/USDT · 바이낸스")}};
    for(const auto& row:cases)for(bool dark:{false,true})QTest::newRow(qPrintable(row.first+(dark?"-dark":"-light")))<<row.first<<row.second<<dark;
}
void LocalWindowTest::chartInstrumentNamesAreDisplayOnly(){
    QFETCH(QString,key);QFETCH(QString,expected);QFETCH(bool,dark);
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-instrument-names.ini"),QSettings::IniFormat);
    const auto identity=WatchlistWorkspaceList::identityFromKey(key);WatchlistWorkspaceList saved;saved.setVerifiedInstruments({identity});
    const auto listId=saved.createList(QStringLiteral("합성 이름 확인"));QVERIFY(saved.addIdentity(listId,identity));saved.useList(listId);
    const auto bytes=saved.publicState();profile.setValue("chart/watchlists/v2",bytes);profile.sync();
    Vault vault(directory.filePath("synthetic-names-locked-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    PublicMarketTransport publicHttp;PublicMarketCatalog markets(&publicHttp);FakeBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
    CoinWindowPublicTransport metadata;NoNetworkBrokerTransport candleHttp;MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
    LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&markets,false);window.setPublicEquityBoard(&board);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);workspace->indicatorPreferences()->markIntroSeen();workspace->setIdentity(key);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);window.resize(1280,1000);window.show();
    child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);QCoreApplication::processEvents();
    auto* lists=workspace->watchlistModel();const bool verifiedBefore=lists->isCurrentlyVerified(identity);
    markets.refresh();board.refresh();QTRY_VERIFY(!markets.busy()&&!board.busy());
    if(identity.source=="binance"){
        QTRY_VERIFY(metadata.token);HttpResult response;response.status=200;
        response.body=R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})";
        emit metadata.finished(metadata.token,response);QCoreApplication::processEvents();
    }
    const bool rejectedCoin=key=="coin:upbit:KRW-SYN"||key=="coin:upbit:BTC-BTC";
    const auto chartExpected=rejectedCoin?QStringLiteral("가짜비트 BTC/KRW · 업비트"):expected;
    if(rejectedCoin){
        // Unknown names still render in a saved public row; they do not prove
        // that a market exists or authorize replacing the confirmed chart.
        const auto requests=candleHttp.sent;
        QVERIFY(!workspace->setIdentity(key));
        QCOMPARE(workspace->selectedChartIdentity(),QString("coin:upbit:KRW-BTC"));
        QCOMPARE(candleHttp.sent,requests);
    }
    const auto plain=[](const QLabel* label){if(label->textFormat()==Qt::RichText){QTextDocument document;document.setHtml(label->text());return document.toPlainText();}return label->text();};
    auto* title=child<QLabel>(window,"marketCandlesTitle");QVERIFY(title);
    QVERIFY2(plain(title).startsWith(chartExpected),qPrintable(plain(title)));
    QVERIFY(!plain(title).contains(QRegularExpression("(?:stock|coin):")));QVERIFY(title->accessibleName().startsWith(chartExpected));
    // The terminal browser opens in All. This case verifies the one saved row,
    // so select the real Watch scope and its market without mutating membership.
    auto* panel=test::openInstrumentPanel(*workspace);QVERIFY(panel);
    auto* watchScope=child<QPushButton>(*panel,"chartBrowserScope_watch");QVERIFY(watchScope);watchScope->click();QVERIFY(watchScope->isChecked());
    const auto marketKey=identity.market=="stock"?(identity.source=="KR"?QString("kr"):QString("us")):identity.source=="binance"?QString("binance"):QString("upbit");
    auto* marketTab=workspace->findChild<QPushButton*>("chartBrowserMarket_"+marketKey);QVERIFY(marketTab);marketTab->click();QVERIFY(marketTab->isChecked());
    QCOMPARE(workspace->currentListId(),listId);QCoreApplication::processEvents();
    auto* tree=child<QTreeWidget>(window,"chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),1);
    const auto* row=tree->topLevelItem(0);QCOMPARE(row->data(0,Qt::UserRole).toString(),key);
    QVERIFY(!row->isHidden());
    if(rejectedCoin){QVERIFY(!row->isSelected());QVERIFY(!tree->currentItem());}
    const auto rowExpected=expected.section(QStringLiteral(" · "),0,0);
    QVERIFY2(row->text(0).startsWith(rowExpected),qPrintable(row->text(0)));
    QVERIFY(row->data(0,Qt::AccessibleTextRole).toString().startsWith(rowExpected));QVERIFY(row->toolTip(0).startsWith(expected));
    QVERIFY(!row->text(0).contains(QStringLiteral(" · ")+expected.section(QStringLiteral(" · "),1)));
    QVERIFY(child<QLabel>(window,"chartWorkspaceMarkets")->text().contains(expected.section(QStringLiteral(" · "),1)));
    for(const auto* label:workspace->findChildren<QLabel*>())if(label->isVisible()){
        QVERIFY2(!plain(label).contains(QRegularExpression("(?:stock|coin):")),qPrintable(label->objectName()+": "+plain(label)));
        QVERIFY(!label->accessibleName().contains(QRegularExpression("(?:stock|coin):")));
    }
    QCOMPARE(lists->isCurrentlyVerified(identity),verifiedBefore);QCOMPARE(lists->publicState(),bytes);
    QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(),bytes);
    const auto requests=publicHttp.requests.size()+boardHttp.paths.size()+metadata.requests.size()+candleHttp.sent;
    emit markets.changed();emit board.changed();QCoreApplication::processEvents();
    QCOMPARE(publicHttp.requests.size()+boardHttp.paths.size()+metadata.requests.size()+candleHttp.sent,requests);
    QCOMPARE(lists->isCurrentlyVerified(identity),verifiedBefore);QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(),bytes);
    const auto out=qEnvironmentVariable("KEEFI_TOP4_RENDER_DIR");if(!out.isEmpty()){
        QVERIFY(QDir().mkpath(out));auto stem=key;stem.replace(':','-');
        QVERIFY(panel->grab().save(out+"/names-"+stem+(dark?"-dark.png":"-light.png")));
    }
    KeefiTheme::instance()->setMode(ThemeMode::System);
}

void LocalWindowTest::coinFlowDemandFollowsVisibleChartAndStopsOnTeardown(){
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-flow.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-flow-locked-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    NoNetworkBrokerTransport candleHttp,metadata;PublicMarketTransport publicHttp;PublicMarketCatalog markets(&publicHttp);
    CoinWindowPublicTransport flowHttp,minuteHttp;MarketCandles candles(&candleHttp),minuteModel(&minuteHttp);CoinFlowSource flow(&flowHttp);
    auto window=std::make_unique<LocalWindow>(vault,accounts);window->setPortfolioController(&portfolio);window->setChartSettings(&profile);
    window->setChartInstrumentTransport(&metadata);window->setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window->setCoinFlowSource(&flow);window->setCoinFlowMinuteModel(&minuteModel);window->setMarketCandles(&candles);
    window->resize(1280,900);window->show();auto* workspace=child<ChartWorkspace>(*window,"chartWorkspace");QVERIFY(workspace);
    QVERIFY(!flow.active());workspace->setIdentity("coin:upbit:KRW-BTC");
    auto* pages=child<QTabWidget>(*window,"portfolioPages");pages->setCurrentWidget(workspace);
    QTRY_VERIFY_WITH_TIMEOUT(flow.active(),1500);QCOMPARE(flow.snapshot().identity,QString("coin:upbit:KRW-BTC"));
    QTRY_VERIFY_WITH_TIMEOUT(!minuteHttp.requests.isEmpty(),1500);
    QVERIFY(MarketCandles::isRequest(minuteHttp.requests.last()));QVERIFY(minuteHttp.requests.last().url.path().endsWith("/1"));
    workspace->setTimeframe(60000);QVERIFY(minuteModel.series().identity.isEmpty());QVERIFY(minuteHttp.aborts>0);
    window->lockAndClear();QVERIFY(flow.active());QVERIFY(workspace->selectedChartIdentity()=="coin:upbit:KRW-BTC");
    pages->setCurrentIndex(pages->indexOf(workspace)==0?1:0);QTRY_VERIFY_WITH_TIMEOUT(!flow.active(),1500);
    pages->setCurrentWidget(workspace);QTRY_VERIFY_WITH_TIMEOUT(flow.active(),1500);
    const auto reads=flowHttp.requests.size();window.reset();QVERIFY(!flow.active());QCOMPARE(flowHttp.requests.size(),reads);
}
void LocalWindowTest::coinFlowMinuteModelRequiresCurrentPublicUpbitCatalog(){
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings profile(directory.filePath("synthetic-minute-catalogue.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-minute-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    CoinWindowPublicTransport minuteHttp,publicHttp;NoNetworkBrokerTransport chartHttp,metadata;
    MarketCandles minutes(&minuteHttp),candles(&chartHttp);PublicMarketCatalog markets(&publicHttp);
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);
    window.setCoinFlowMinuteModel(&minutes);
    minutes.setSelection("coin:upbit:KRW-BTC",60000);
    QCOMPARE(minuteHttp.requests.size(),0); // The injected independent model starts guarded.
    window.setPublicMarketCatalog(&markets,false);window.setMarketCandles(&candles);
    markets.refresh();QTRY_VERIFY(publicHttp.token>0);
    HttpResult response;response.status=200;
    response.body=R"([{"market":"KRW-BTC","korean_name":"합성 비트코인","english_name":"Synthetic Bitcoin","market_event":{"warning":false,"caution":{}}}])";
    emit publicHttp.finished(publicHttp.token,response);
    QTRY_VERIFY(markets.marketListAvailable());
    QTRY_COMPARE(minuteHttp.requests.size(),1); // Refresh authorizes the now-confirmed identity.
    QCOMPARE(QUrlQuery(minuteHttp.requests.last().url).queryItemValue("market"),QString("KRW-BTC"));
    minutes.setSelection("coin:upbit:KRW-HYPE",60000);
    emit markets.changed();QCoreApplication::processEvents();
    QCOMPARE(minuteHttp.requests.size(),1); // A valid-looking, absent pair remains blocked.
    QVERIFY(!vault.isUnlocked());
}
void LocalWindowTest::coinFlowWatchlistBadgeTracksEnabledIndependentMinuteStudy(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings profile(directory.filePath("synthetic-flow-badge.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-flow-badge-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    qint64 now=QDateTime::currentMSecsSinceEpoch();const auto end=now/60000*60000;
    CoinWindowPublicTransport candlesHttp,flowHttp,publicHttp;NoNetworkBrokerTransport metadata;PublicMarketCatalog markets(&publicHttp);
    MarketCandles candles(&candlesHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    CoinFlowSource flow(&flowHttp,{},{},nullptr,[&]{return now;});
    CoinFlowInstrument item;item.venue=CoinFlowVenue::UpbitSpot;item.grain=CoinFlowGrain::UpbitTrade;item.identity="coin:upbit:KRW-SYN";
    item.symbol="KRW-SYN";item.baseAsset="SYN";item.quoteAsset="KRW";item.verified=true;
    item.evidence={"합성 공개 종목 목록","https://api.upbit.com/v1/market/all",0,now,now};flow.setPublicCatalog(CoinFlowVenue::UpbitSpot,{item});
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);
    window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&markets,false);markets.refresh();QVERIFY(publicHttp.token);
    HttpResult metadataResult;metadataResult.status=200;
    metadataResult.body=R"([{"market":"KRW-SYN","korean_name":"합성 종목","english_name":"Synthetic","market_event":{"warning":false,"caution":{}}}])";
    emit publicHttp.finished(publicHttp.token,metadataResult);QVERIFY(markets.marketListAvailable());
    window.setCoinFlowSource(&flow);window.setMarketCandles(&candles);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);workspace->setIdentity(item.identity);workspace->setTimeframe(60000);
    window.resize(1280,1000);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);QVERIFY(candlesHttp.token);
    QJsonArray rows;for(int i=0;i<26;++i){const auto at=end-i*60000;rows.append(QJsonObject{{"market","KRW-SYN"},{"unit","1"},
        {"timestamp",QString::number(qMin(now,at+1000))},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,QTimeZone("UTC")).toString("yyyy-MM-ddTHH:mm:ss")},
        {"opening_price","100"},{"high_price","101"},{"low_price","99"},{"trade_price","100"},{"candle_acc_trade_volume",i==1?"40":"10"}});}
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit candlesHttp.finished(candlesHttp.token,response);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->coinFlowStudies().volume.state.available,1500);
    const auto badge=[&]{return workspace->watchlistModel()->rowState({"coin","upbit","KRW-SYN"}).badges.join(" · ");};
    QTRY_VERIFY_WITH_TIMEOUT(badge().contains(QStringLiteral("Whales & Reversal")),1500);QVERIFY(badge().contains("4.00"));
    workspace->indicatorPreferences()->setVisible("whales",false);QVERIFY(badge().contains(QStringLiteral("Whales & Reversal")));
    workspace->indicatorPreferences()->setEnabled("whales",false);QVERIFY(!badge().contains(QStringLiteral("Whales & Reversal")));
}

void LocalWindowTest::stockSourceTeardownCannotRebindThroughForwardedStudyChanges(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-stock-teardown.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-stock-teardown-vault"));QVERIFY(vault.create("Synthetic stock teardown password 2026!"));
    keefi::test::ReadFixture fixture("toss");QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));
    keefi::test::ProviderReadTransport privateHttp;privateHttp.provider="toss";
    PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &privateHttp;};factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);
    NoNetworkBrokerTransport coinHttp;MarketCandles candles(&coinHttp);StockWindowMarketTransport chartHttp;
    qint64 now=QDateTime::currentMSecsSinceEpoch();StockChartSource source(&chartHttp,nullptr,[&]{return now;});
    auto window=std::make_unique<LocalWindow>(vault,accounts);window->setChartSettings(&profile);window->setPortfolioController(&portfolio);window->setSecurityStatus(true);
    window->setMarketCandles(&candles);window->setStockChartSource(&source);window->resize(1280,900);window->show();portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",6000);
    auto* workspace=child<ChartWorkspace>(*window,"chartWorkspace");QVERIFY(workspace);child<QTabWidget>(*window,"portfolioPages")->setCurrentWidget(workspace);
    workspace->setIdentity("stock:KR:005930");QTRY_VERIFY_WITH_TIMEOUT(!chartHttp.requests.isEmpty(),1500);
    QCOMPARE(source.snapshot().source,StockSourceKind::Toss);QVERIFY(source.active());
    const auto requests=chartHttp.requests.size();const auto reads=privateHttp.sent.size();now+=2000;
    window.reset();
    QCOMPARE(chartHttp.requests.size(),requests);QCOMPARE(privateHttp.sent.size(),reads);QVERIFY(!source.active());
}

void LocalWindowTest::brokerConnectionReturnsToOrigin_data() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("mode");
    QTest::newRow("create-toss-US") << QString("toss") << QString("new");
    QTest::newRow("create-kiwoom-KR") << QString("kiwoom") << QString("new");
    QTest::newRow("create-kiwoom-domestic-daily") << QString("kiwoom") << QString("new-daily");
    QTest::newRow("unlock-reuse-existing") << QString("toss") << QString("reuse");
    QTest::newRow("unlock-replace-expired") << QString("toss") << QString("replace");
    QTest::newRow("failed-read-retry") << QString("toss") << QString("retry");
}

void LocalWindowTest::brokerConnectionReturnsToOrigin() {
    QFETCH(QString, provider);
    QFETCH(QString, mode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString password = QStringLiteral("Synthetic chart return password 108!");
    const WatchlistIdentity stock = provider == "kiwoom" ? WatchlistIdentity{"stock","KR","005930"}
                                                         : WatchlistIdentity{"stock","US","NVDA"};
    const auto key = stock.key();
    const qint64 interval = mode == "new-daily" ? 86400000 : 15 * 60000;
    QSettings settings(dir.filePath("chart.ini"), QSettings::IniFormat);
    WatchlistWorkspaceList lists;
    const WatchlistIdentity coin{"coin","upbit","KRW-BTC"};
    lists.setVerifiedInstruments({stock, coin});
    const auto listId = lists.createList("차트 복귀 테스트");
    QVERIFY(lists.addIdentity(listId, stock));
    QVERIFY(lists.addIdentity(listId, coin));
    settings.setValue("chart/watchlists/v2", lists.publicState());
    Vault vault(dir.filePath("test-vault"));
    test::ReadFixture fixture(provider);
    const bool existing = mode == "reuse" || mode == "replace";
    if (existing) {
        QVERIFY(vault.create(password));
        QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
        vault.lock();
    }
    AccountModel accounts(&vault);
    test::ProviderReadTransport brokerHttp;
    brokerHttp.provider = provider;
    bool denied = mode == "replace" || mode == "retry";
    brokerHttp.transform = [&](const RequestSpec& request, HttpResult& response) {
        if (denied && request.operation == Operation::Authenticate) { response.status = 401; response.body = "{}"; }
    };
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord&) { return &brokerHttp; };
    PortfolioController portfolio(vault, nullptr, factories);
    NoNetworkBrokerTransport coinHttp, metadata;
    PublicMarketTransport publicHttp;
    PublicMarketCatalog markets(&publicHttp);
    MarketCandles candles(&coinHttp);
    StockWindowMarketTransport chartHttp;
    StockChartSource source(&chartHttp);
    LocalWindow window(vault, accounts);
    window.setChartSettings(&settings);
    window.setStockChartSource(&source);
    window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true);
    window.setChartInstrumentTransport(&metadata);
    window.setPublicMarketCatalog(&markets, false);
    markets.refresh();
    QTRY_VERIFY(markets.marketListAvailable());
    window.setMarketCandles(&candles);
    window.resize(1280, 900);
    window.show();
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace");
    auto* pages = child<QTabWidget>(window, "portfolioPages");
    QVERIFY(workspace && pages);
    pages->setCurrentWidget(workspace);
    QVERIFY(workspace->setIdentity(key));
    workspace->setTimeframe(interval);
    auto* connectStock = child<QPushButton>(window, "chartStockConnect");
    QTRY_VERIFY(connectStock && connectStock->isVisible());
    QCOMPARE(connectStock->text(), QStringLiteral("증권 연결하기"));
    connectStock->click();
    auto* gate = child<VaultDialog>(window, "vaultDialog");
    QVERIFY(gate && gate->isVisible());
    if (existing) child<QLineEdit>(*gate,"currentPassword")->setText(password);
    else {
        child<QLineEdit>(*gate,"newPassword")->setText(password);
        child<QLineEdit>(*gate,"confirmPassword")->setText(password);
    }
    child<QPushButton>(*gate,"submitVault")->click();
    QTRY_VERIFY(vault.isUnlocked());
    QPointer<AccountDialog> dialog = child<AccountDialog>(window,"accountDialog");
    QVERIFY(dialog && dialog->isVisible());
    QVERIFY(child<QAbstractButton>(*dialog,"accountService_toss"));
    QVERIFY(child<QAbstractButton>(*dialog,"accountService_kiwoom"));
    QVERIFY(!child<QAbstractButton>(*dialog,"accountService_kis"));
    QSignalSpy returned(dialog, &AccountDialog::chartReturnRequested);
    QList<AssetSnapshot> balancesAtReturn;
    connect(dialog, &AccountDialog::chartReturnRequested, &window, [&] { balancesAtReturn = portfolio.snapshots(); });
    if (existing) {
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty() && portfolio.connections().first().state ==
            (denied ? QStringLiteral("authentication_required") : QStringLiteral("complete")), 8000);
        const auto id = vault.accounts().first().id;
        auto* reuse = dialog->findChild<QPushButton*>("existingChartConnection_" + id);
        QVERIFY(reuse);
        reuse->click();
        denied = false;
    } else {
        dialog->findChild<QAbstractButton*>("accountService_" + provider)->click();
    }
    // Simulate intervening navigation to verify the captured target, even if this dialog outlives its original tab.
    QVERIFY(workspace->setIdentity("coin:upbit:KRW-BTC"));
    auto* split = child<QComboBox>(*workspace, "chartLayoutSelector");
    split->setCurrentIndex(split->findData(2));
    QCOMPARE(split->currentData().toInt(), 2);
    pages->setCurrentWidget(window.findChild<QWidget*>("connectionsPage"));
    workspace->setTimeframe(60000);
    if (mode != "reuse") {
        for (auto it = fixture.credentials.cbegin(); it != fixture.credentials.cend(); ++it) {
            auto* input = dialog->findChild<QLineEdit*>("credential_" + it.key());
            QVERIFY(input);
            input->setText(it.value() + (mode == "replace" ? QStringLiteral("New") : QString{}));
        }
        auto* save = child<QPushButton>(*dialog,"saveAccount");
        QVERIFY(save->isEnabled());
        save->click();
    }
    if (mode == "retry") {
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty() && portfolio.connections().first().state ==
                                 QStringLiteral("authentication_required"), 8000);
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(returned.count(), 0);
        auto* retry = child<QPushButton>(*dialog, "accountConnectionRetry");
        QTRY_VERIFY(retry && retry->isVisible() && retry->isEnabled());
        denied = false;
        retry->click();
    }
    QTRY_COMPARE_WITH_TIMEOUT(returned.count(), 1, 10000);
    QCOMPARE(vault.accounts().size(), 1);
    QCOMPARE(returned.first().first().toString(), vault.accounts().first().id);
    QCOMPARE(portfolio.connections().first().state, QStringLiteral("complete"));
    QCOMPARE(balancesAtReturn.size(), 1);
    const auto& balance = balancesAtReturn.first();
    QCOMPARE(balance.provider, provider);
    QCOMPARE(balance.connectionId, vault.accounts().first().id);
    QVERIFY(brokerHttp.sent.contains(Operation::Authenticate));
    QVERIFY(brokerHttp.sent.contains(Operation::Holdings));
    const auto& components = balance.components;
    const auto stockHolding = std::find_if(components.cbegin(), components.cend(), [](const AssetComponent& c) {
        return c.kind == AssetComponentKind::Holding && c.asset == "005930";
    });
    QVERIFY(stockHolding != components.cend());
    QCOMPARE(stockHolding->quantity, QString("100"));
    QCOMPARE(stockHolding->reportedValue, QString("7000000"));
    QCOMPARE(stockHolding->reportedValueCurrency, QString("KRW"));
    const auto deposit = std::find_if(components.cbegin(), components.cend(), [](const AssetComponent& c) {
        return c.kind == AssetComponentKind::Cash && c.asset == "KRW";
    });
    QVERIFY(deposit != components.cend());
    if (provider == "kiwoom") {
        QVERIFY(brokerHttp.sent.contains(Operation::Account));
        QVERIFY(deposit->evidence.complete);
        QCOMPARE(deposit->quantity, QString("7000000"));
    } else {
        // The Toss holdings endpoint supplies stock values, without a deposit balance.
        QVERIFY(deposit->quantity.isEmpty());
        QVERIFY(!balance.balanceComplete);
    }
    QCOMPARE(pages->currentWidget(), workspace);
    QCOMPARE(workspace->selectedChartIdentity(), key);
    QCOMPARE(workspace->timeframeMs(), interval);
    QCOMPARE(split->currentData().toInt(), 1);
}

void LocalWindowTest::stockConnectionGuidanceUsesExistingState_data(){
    QTest::addColumn<QString>("state");QTest::addColumn<bool>("dark");QTest::addColumn<QString>("reason");
    const QList<QPair<QString,QString>> states{
        {"A-missing",QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요")},
        {"A-locked",QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요")},
        {"B-empty",QStringLiteral("연결된 증권사가 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요")},
        {"C-kiwoom",QStringLiteral("현재 연결로는 이 자료를 조회할 수 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요")},
        {"D-toss-denied",QStringLiteral("토스증권 인증을 다시 확인해 주세요. 키움증권으로는 국내 일봉을 볼 수 있어요")},
        {"D-kiwoom-denied",QStringLiteral("키움증권 인증을 다시 확인해 주세요. 국내 일봉은 키움증권 또는 토스증권에서 볼 수 있어요")},
        {"ready-toss",QString{}}};
    for(const auto& [state,reason]:states)for(const bool dark:{false,true})
        QTest::newRow(qPrintable(state+(dark?"-dark":"-light")))<<state<<dark<<reason;
}
void LocalWindowTest::stockConnectionGuidanceUsesExistingState(){
    QFETCH(QString,state);QFETCH(bool,dark);QFETCH(QString,reason);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-guidance.ini"),QSettings::IniFormat);
    const WatchlistIdentity stock{"stock","KR","005930"};WatchlistWorkspaceList saved;saved.setVerifiedInstruments({stock});
    const auto listId=saved.createList("합성 공개 목록");QVERIFY(saved.addIdentity(listId,stock));profile.setValue("chart/watchlists/v2",saved.publicState());
    Vault vault(directory.filePath("synthetic-guidance-vault"));
    if(state!="A-missing")QVERIFY(vault.create("Synthetic guidance password 2026!"));
    const QString provider=state.contains("kiwoom")?QString("kiwoom"):QString("toss");
    const bool denied=state.endsWith("-denied");
    const bool connection=state=="A-locked"||state=="C-kiwoom"||denied||state=="ready-toss";
    if(connection){test::ReadFixture fixture(provider);fixture.account.alias="SYNTHETIC_GUIDANCE_PRIVATE_ALIAS";
        QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));}
    if(state=="A-locked")vault.lock();
    test::ProviderReadTransport accountHttp;accountHttp.provider=provider;
    if(denied)accountHttp.transform=[](const RequestSpec& request,HttpResult& response){
        if(request.operation==Operation::Authenticate){response.status=401;response.body="{}";}};
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    factories.trading=[&](const AccountRecord&){return &accountHttp;};
    PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);
    PublicMarketTransport marketsHttp;PublicMarketCatalog markets(&marketsHttp);FakeBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
    NoNetworkBrokerTransport coinHttp;MarketCandles candles(&coinHttp);StockWindowMarketTransport chartHttp,alertHttp;
    StockChartSource chart(&chartHttp),alerts(&alertHttp);NoNetworkBrokerTransport historyHttp;PublicMarketHistory history(&historyHttp);
    LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);
    window.setPublicMarketCatalog(&markets,false);window.setPublicEquityBoard(&board);window.setPublicMarketHistory(&history);window.setMarketCandles(&candles);
    window.setStockChartSources(&chart,&alerts);window.setSecurityStatus(true);window.resize(1440,1000);window.show();
    QTest::mouseClick(child<QPushButton>(window,dark?"themeDark":"themeLight"),Qt::LeftButton);
    if(connection&&state!="A-locked"){
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().size(),1,3000);
        if(denied)QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("authentication_required"),8000);
        else QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),8000);
    }
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* pages=child<QTabWidget>(window,"portfolioPages");pages->setCurrentWidget(workspace);workspace->setIdentity(stock.key());
    alerts.setSelection(stock,60000);alerts.setActive(true);
    QTRY_VERIFY(chart.active());
    if(!reason.isEmpty()){
        QCOMPARE(chart.snapshot().series.reason,reason);QCOMPARE(alerts.snapshot().series.reason,reason);
        QVERIFY(chartHttp.requests.isEmpty());QVERIFY(alertHttp.requests.isEmpty());
        QVERIFY(!chart.snapshot().series.reason.contains("SYNTHETIC_GUIDANCE_PRIVATE_ALIAS"));
    }else{QTRY_VERIFY(!chartHttp.requests.isEmpty());QTRY_VERIFY(!alertHttp.requests.isEmpty());}
    const auto output=qEnvironmentVariable("KEEFI_TOP4_RENDER_DIR");
    const auto capture=[&](const QString& name){if(output.isEmpty())return true;return QDir().mkpath(output)&&window.grab().save(QDir(output).filePath(name+(dark?"-dark.png":"-light.png")));};
    QCoreApplication::processEvents();if(state!="ready-toss")QVERIFY(capture("chart-guidance-"+state));
    // Baseline after normal wiring and authorized fake setup. Pure state refresh
    // must not add HTTP work, authenticate again, or alter saved identities.
    const auto chartCount=chartHttp.requests.size(),alertCount=alertHttp.requests.size(),accountCount=accountHttp.sent.size();
    const auto marketCount=marketsHttp.requests.size(),boardCount=boardHttp.paths.size();const auto historyCount=historyHttp.sent;
    window.refreshState();emit portfolio.changed();
    QCOMPARE(chartHttp.requests.size(),chartCount);QCOMPARE(alertHttp.requests.size(),alertCount);QCOMPARE(accountHttp.sent.size(),accountCount);
    QCOMPARE(marketsHttp.requests.size(),marketCount);QCOMPARE(boardHttp.paths.size(),boardCount);QCOMPARE(historyHttp.sent,historyCount);
    QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(),saved.publicState());
    // Existing public list/detail cells use exactly the same connection state.
    for(int i=0;i<pages->count();++i)if(pages->tabText(i)==QStringLiteral("탐색"))pages->setCurrentIndex(i);
    board.refresh();QTRY_VERIFY(!board.items("kr").isEmpty()&&!board.items("us").isEmpty());
    auto* map=child<MarketMapWidget>(window,"marketMap");auto* detail=child<AssetDetailPanel>(window,"marketMapDetail");
    auto* watch=child<ExploreWatchlist>(window,"exploreWatchlist");auto* basis=child<QLabel>(*watch,"watchlistBasis");
    for(const QString section:{QString("kr"),QString("us")}){
        const QString symbol=section=="kr"?QString("005930"):QString("NVDA");
        map->select(section,symbol);emit map->tileSelected(section,symbol);
        auto* metric=detail->findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(metric);
        QCOMPARE(metric->data().id,symbol);
        if(reason.isEmpty())QVERIFY(!metric->data().history.reason.contains(QStringLiteral("키움증권")));
        else QCOMPARE(metric->data().history.reason,reason);
        auto* indicator=child<QLabel>(*detail,"assetDetailIndicator0");
        if(reason.isEmpty())QVERIFY(!indicator->text().contains(QStringLiteral("키움증권")));
        else QVERIFY(indicator->text().contains(reason));
        QCoreApplication::processEvents();if(state=="A-missing"||state=="ready-toss")QVERIFY(capture("explore-detail-"+state+"-"+section));
        watch->toggle(section,symbol);watch->setSection(section);
        auto* view=child<QComboBox>(window,"exploreView");view->setCurrentIndex(view->findData("list"));QCoreApplication::processEvents();
        auto* table=child<QTableWidget>(*watch,"watchlistTable");QTRY_COMPARE(table->rowCount(),1);
        if(reason.isEmpty()){QVERIFY(!basis->text().contains(QStringLiteral("키움증권")));QVERIFY(!table->item(0,4)->toolTip().contains(QStringLiteral("키움증권")));}
        else{QVERIFY(basis->text().contains(reason));QVERIFY(!table->item(0,4)->toolTip().contains("https://"));QVERIFY(table->item(0,4)->toolTip().count('\n')==0);QCOMPARE(metric->data().history.reason,reason);}
        if(state=="A-missing"||state=="ready-toss")QVERIFY(capture("explore-list-"+state+"-"+section));view->setCurrentIndex(view->findData("map"));
    }
    if(state=="ready-toss"){
        pages->setCurrentWidget(workspace);QTRY_VERIFY(chart.active());QVERIFY(chartHttp.token);
        const auto sent=chartHttp.requests.size()+alertHttp.requests.size(),reads=accountHttp.sent.size();
        HttpResult denied;denied.status=403;emit chartHttp.finished(chartHttp.token,denied);
        const auto deniedReason=QStringLiteral("토스증권 인증을 다시 확인해 주세요. 키움증권으로는 국내 일봉을 볼 수 있어요");
        QCOMPARE(chart.snapshot().series.reason,deniedReason);QCOMPARE(alerts.snapshot().series.reason,deniedReason);
        QVERIFY(basis->text().contains(deniedReason));
        QCOMPARE(detail->findChild<ExploreMetricCard*>("assetDetailMetric")->data().history.reason,deniedReason);
        QCOMPARE(chartHttp.requests.size()+alertHttp.requests.size(),sent);QCOMPARE(accountHttp.sent.size(),reads);
        // One lane can stop after a rejected revision. A different active lane
        // observing the next revision must release that stale shared guidance.
        quint64 revision=100;
        const auto authorization=[&](const StockChartSource::AuthorizationBorrow& borrow){borrow("SYNTHETIC_NEW_GUIDANCE_SESSION",60000,revision);return true;};
        const auto chartBefore=chartHttp.requests.size(),alertsBefore=alertHttp.requests.size();
        chart.setAuthorizationProvider(authorization);alerts.setAuthorizationProvider(authorization);
        QTRY_VERIFY(chartHttp.requests.size()>chartBefore&&alertHttp.requests.size()>alertsBefore);
        QCOMPARE(chart.observedAuthorizationRevision(),quint64(100));QCOMPARE(alerts.observedAuthorizationRevision(),quint64(100));
        emit portfolio.changed();
        emit alertHttp.finished(alertHttp.token,denied);alerts.stop();
        QCOMPARE(chart.snapshot().series.reason,deniedReason);
        revision=101;HttpResult fresh;fresh.status=200;fresh.body="{}";emit chartHttp.finished(chartHttp.token,fresh);
        QCOMPARE(chart.observedAuthorizationRevision(),quint64(101));QCOMPARE(alerts.observedAuthorizationRevision(),quint64(100));
        QVERIFY(!chart.snapshot().series.reason.contains(QStringLiteral("키움증권")));
        QVERIFY(!basis->text().contains(QStringLiteral("키움증권")));
        QCOMPARE(accountHttp.sent.size(),reads);
        window.lockAndClear();
        const auto lockedReason=QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요");
        QVERIFY(basis->text().contains(lockedReason));
        QCOMPARE(detail->findChild<ExploreMetricCard*>("assetDetailMetric")->data().history.reason,lockedReason);
    }
    if(state=="A-locked"){QVERIFY(!vault.isUnlocked());QVERIFY(accountHttp.sent.isEmpty());}
}

void LocalWindowTest::stockChartRequiresBrokerWhileLockedAndKeepsSavedPublicRows_data(){
    QTest::addColumn<QString>("identityKey");
    QTest::newRow("domestic")<<QString("stock:KR:005930");
    QTest::newRow("us")<<QString("stock:US:SYN");
}
void LocalWindowTest::stockChartRequiresBrokerWhileLockedAndKeepsSavedPublicRows(){
    QFETCH(QString,identityKey);
    const auto stock=WatchlistWorkspaceList::identityFromKey(identityKey);
    const WatchlistIdentity coin{"coin","upbit","KRW-BTC"};
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-stock-guest.ini"),QSettings::IniFormat);
    // Simulate a public list saved when source metadata was available. Restoring
    // an identity must not promote it into current market-data verification.
    WatchlistWorkspaceList saved;saved.setVerifiedInstruments({stock});
    const auto listId=saved.createList("합성 공개 관심 리스트");
    QVERIFY(saved.addIdentity(listId,coin));QVERIFY(saved.addIdentity(listId,stock));
    const auto publicState=saved.publicState();profile.setValue("chart/watchlists/v2",publicState);
    Vault vault(directory.filePath("synthetic-stock-locked-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);NoNetworkBrokerTransport coins,metadata;MarketCandles candles(&coins);
    PublicMarketTransport publicHttp;publicHttp.quoteStatus=503;PublicMarketCatalog markets(&publicHttp);
    StockWindowMarketTransport privateHttp;StockChartSource source(&privateHttp);
    LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setStockChartSource(&source);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setChartInstrumentTransport(&metadata);
    window.setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window.setMarketCandles(&candles);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    workspace->setIdentity(identityKey);window.resize(1280,900);window.show();
    child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QTRY_COMPARE(workspace->selectedChartIdentity(),identityKey);
    QTRY_VERIFY_WITH_TIMEOUT(source.active(),1500);
    QTRY_COMPARE(source.snapshot().series.reason,QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QCOMPARE(source.snapshot().source,StockSourceKind::None);
    QVERIFY(!source.snapshot().series.available);QVERIFY(source.snapshot().series.candles.isEmpty());
    QTRY_VERIFY(child<QLabel>(window,"marketCandlesStatus")->text().contains(QStringLiteral("금고가 열려 있지 않아요")));
    QVERIFY(child<QPushButton>(window,"chartStockConnect")->isVisible());
    QVERIFY(privateHttp.requests.isEmpty());QCOMPARE(accounts.rowCount(),0);QVERIFY(!vault.isUnlocked());
    QVERIFY(!workspace->watchlistModel()->isCurrentlyVerified(stock));
    QVERIFY(workspace->watchlistModel()->rowState(stock).price.isEmpty());
    QCOMPARE(workspace->watchlistModel()->publicState(),publicState);
    window.lockAndClear();QVERIFY(!source.snapshot().series.available);QVERIFY(privateHttp.requests.isEmpty());
    QCOMPARE(workspace->watchlistModel()->publicState(),publicState);
    QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(),publicState);
    workspace->setIdentity(coin.key());QCOMPARE(workspace->selectedChartIdentity(),coin.key());
    QVERIFY(!vault.isUnlocked());
}
void LocalWindowTest::stockSessionPreferencePersistsOnlyThePublicChoice(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-stock-session.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-stock-session-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);NoNetworkBrokerTransport transport;MarketCandles candles(&transport);
    {LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setMarketCandles(&candles);
        auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);QVERIFY(!workspace->includeExtendedSessions());workspace->setIncludeExtendedSessions(true);
        const auto saved=QJsonDocument::fromJson(profile.value("chart/stock/sessions/v1").toByteArray()).object();
        QCOMPARE(saved.size(),2);QCOMPARE(saved.value("format").toString(),QString("keefi-stock-sessions/1"));QVERIFY(saved.value("includeExtended").toBool());}
    {LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setMarketCandles(&candles);
        QVERIFY(child<ChartWorkspace>(window,"chartWorkspace")->includeExtendedSessions());}
    profile.setValue("chart/stock/sessions/v1",QJsonDocument(QJsonObject{{"format","keefi-stock-sessions/1"},{"includeExtended",true},{"account","SYNTHETIC_FORBIDDEN_PROFILE_FIELD"}}).toJson());
    {LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setMarketCandles(&candles);
        QVERIFY(!child<ChartWorkspace>(window,"chartWorkspace")->includeExtendedSessions());}
}
void LocalWindowTest::kiwoomDisplayUsesOneInjectedSourceAndStaysOffWithoutAuthorization(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    Vault vault(directory.filePath("synthetic-kiwoom-locked-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);NoNetworkBrokerTransport quoteHttp,metricHttp,publicHttp,coinHttp;
    auto socket=std::make_unique<KiwoomWindowSocket>();auto* wire=socket.get();KiwoomMarketStream stream(nullptr,&quoteHttp,std::move(socket));
    MarketMetrics metrics(&metricHttp);PublicMarketCatalog catalog(&publicHttp);MarketCandles candles(&coinHttp);
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setMarketCandles(&candles);
    QVERIFY(window.findChild<QWidget*>("marketFlowContext"));window.setKiwoomMarketStream(&stream);
    auto* view=window.findChild<KiwoomMarketView*>("kiwoomMarketView");QVERIFY(view);
    window.setKiwoomMarketStream(&stream);QCOMPARE(window.findChildren<KiwoomMarketView*>().size(),1);
    QVERIFY(!view->privateEnabled());QVERIFY(view->selectedConnectionId().isEmpty());QVERIFY(!stream.broker());
    window.resize(1280,900);window.show();QCoreApplication::processEvents();
    QCOMPARE(wire->opens,0);QCOMPARE(quoteHttp.sent,0);QCOMPARE(accounts.rowCount(),0);QVERIFY(!vault.isUnlocked());
    window.lockAndClear();QVERIFY(QMetaObject::invokeMethod(&stream,"changed",Qt::DirectConnection));
    QCoreApplication::processEvents();QVERIFY(!view->privateEnabled());QVERIFY(view->selectedConnectionId().isEmpty());
    QVERIFY(stream.quotes().isEmpty());QCOMPARE(wire->opens,0);QCOMPARE(quoteHttp.sent,0);
}
void LocalWindowTest::publicChartWithStoppedKiwoomSourceSettlesAfterQueuedEvents(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings profile(directory.filePath("synthetic-full-catalogue.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-no-account-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    class WidePublicTransport final:public TradingTransport{public:int sent=0;
        void abort()override{}void send(quint64 token,const RequestSpec& request)override{
            QVERIFY(!request.authenticated);QVERIFY(request.method=="GET");++sent;QJsonArray rows;
            if(request.operation==Operation::Markets)for(int i=0;i<300;++i){
                const auto symbol=i?QString("KRW-SYN%1").arg(i):QString("KRW-BTC");
                rows.append(QJsonObject{{"market",symbol},{"korean_name",QString("합성 종목 %1").arg(i)},{"english_name",QString("Synthetic %1").arg(i)}});
            }else{QCOMPARE(request.operation,Operation::Quote);
                for(const auto& symbol:QUrlQuery(request.url).queryItemValue("markets").split(','))rows.append(QJsonObject{
                    {"market",symbol},{"trade_price","101"},{"prev_closing_price","100"},{"signed_change_rate","0.01"},
                    {"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch())},{"acc_trade_price_24h","1000000"}});
            }
            HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
            QTimer::singleShot(0,this,[this,token,response]{emit finished(token,response);});
        }} publicHttp;
    PublicMarketCatalog catalog(&publicHttp);catalog.refresh();QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(),2000);
    QCOMPARE(catalog.rows().size(),300);for(const auto& row:catalog.rows())QVERIFY(row.priced);
    NoNetworkBrokerTransport quoteHttp,metricHttp,candleHttp,metadata;MarketMetrics metrics(&metricHttp);
    MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
    auto socket=std::make_unique<KiwoomWindowSocket>();auto* wire=socket.get();KiwoomMarketStream stream(nullptr,&quoteHttp,std::move(socket));
    LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setChartInstrumentTransport(&metadata);
    window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setMarketCandles(&candles);
    // main injects this source even for public browsing. Its queued changed
    // notification must not create another stop/changed cycle at every refresh.
    QSignalSpy changed(&stream,&KiwoomMarketStream::changed);window.setKiwoomMarketStream(&stream);
    window.resize(1440,900);window.show();auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    for(int turn=0;turn<12;++turn)QCoreApplication::sendPostedEvents(&stream,QEvent::MetaCall);
    QVERIFY2(changed.size()<=1,qPrintable(QString("stopped Kiwoom source emitted %1 queued changes in 12 finite turns").arg(changed.size())));
    QCOMPARE(workspace->watchlistModel()->rowState({"coin","upbit","KRW-BTC"}).price,QString("101"));
    QCOMPARE(wire->opens,0);QCOMPARE(quoteHttp.sent,0);QCOMPARE(accounts.rowCount(),0);
    QVERIFY(!stream.broker());QVERIFY(!vault.exists());QVERIFY(!vault.isUnlocked());
}
void LocalWindowTest::kiwoomDisplayBorrowsTheSelectedExistingOwnerAndRevokesBeforeLock(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    const WatchlistIdentity stock{"stock","US","AAPL"};
    QSettings profile(directory.filePath("synthetic-kiwoom-owner-profile.ini"),QSettings::IniFormat);
    // A previously saved public identity expresses the user's quote demand;
    // it is not current broker metadata and must not depend on Yahoo mappings.
    WatchlistWorkspaceList saved;saved.setVerifiedInstruments({stock});
    const auto listId=saved.createList("합성 키움 공개 관심 리스트");QVERIFY(saved.addIdentity(listId,stock));
    profile.setValue("chart/watchlists/v2",saved.publicState());
    Vault vault(directory.filePath("synthetic-kiwoom-owner-vault"));QVERIFY(vault.create("Synthetic Kiwoom source password 2026!"));
    keefi::test::ReadFixture one("kiwoom"),two("kiwoom");one.account.alias="Synthetic Kiwoom owner A";two.account.alias="Synthetic Kiwoom owner B";
    one.credentials["app_key"]="SyntheticKiwoomOwnerKeyA";two.credentials["app_key"]="SyntheticKiwoomOwnerKeyB";
    QVERIFY(vault.addAccount(one.account,one.credentials,StorageMode::EncryptedVault));QVERIFY(vault.addAccount(two.account,two.credentials,StorageMode::EncryptedVault));
    keefi::test::ProviderReadTransport accountA,accountB;accountA.provider=accountB.provider="kiwoom";
    accountA.transform=[](const RequestSpec& request,HttpResult& response){if(request.operation==Operation::Authenticate)response.body.replace("fake-token.only_for_test","syntheticKiwoomOwnerA");if(request.operation==Operation::Accounts)response.body.replace("1234567890","1111111111");};
    accountB.transform=[](const RequestSpec& request,HttpResult& response){if(request.operation==Operation::Authenticate)response.body.replace("fake-token.only_for_test","syntheticKiwoomOwnerB");if(request.operation==Operation::Accounts)response.body.replace("1234567890","2222222222");};
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    factories.trading=[&](const AccountRecord& account){return account.alias==one.account.alias?&accountA:&accountB;};
    PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);
    NoNetworkBrokerTransport metricHttp,publicHttp,coinHttp;MarketMetrics metrics(&metricHttp);PublicMarketCatalog catalog(&publicHttp);MarketCandles candles(&coinHttp);
    KiwoomWindowMarketTransport quoteHttp;auto socket=std::make_unique<KiwoomWindowSocket>();auto* wire=socket.get();KiwoomMarketStream stream(nullptr,&quoteHttp,std::move(socket));
    StockChartSource publicChart(nullptr);LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setMarketCandles(&candles);window.setStockChartSource(&publicChart);window.setKiwoomMarketStream(&stream);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);window.resize(1280,900);window.show();
    child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);workspace->setIdentity(stock.key());
    QCOMPARE(workspace->selectedChartIdentity(),stock.key());
    QVERIFY(!workspace->watchlistModel()->isCurrentlyVerified(stock));
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().size(),2,4000);
    QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections()[0].state=="complete"&&portfolio.connections()[1].state=="complete",15000);
    auto* view=child<KiwoomMarketView>(window,"kiwoomMarketView");QVERIFY(view);auto* owners=child<QComboBox>(*view,"kiwoomMarketConnections");QVERIFY(owners);
    QTRY_COMPARE(owners->count(),2);QVERIFY(view->privateEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(stream.broker(),portfolio.broker(owners->currentData().toString()),2500);
    QCOMPARE(workspace->selectedChartIdentity(),stock.key());
    QVERIFY(!workspace->watchlistModel()->isCurrentlyVerified(stock));
    QTRY_VERIFY_WITH_TIMEOUT(!quoteHttp.requests.isEmpty(),3500);
    const auto sourceRequests=quoteHttp.requests.size();const int sourceAborts=quoteHttp.aborts;
    owners->setCurrentIndex(owners->currentIndex()==0?1:0);
    QTRY_COMPARE_WITH_TIMEOUT(stream.broker(),portfolio.broker(owners->currentData().toString()),2500);
    QTRY_VERIFY_WITH_TIMEOUT(quoteHttp.requests.size()>sourceRequests,3500);QVERIFY(quoteHttp.aborts>sourceAborts);
    const auto* selected=portfolio.broker(owners->currentData().toString());QVERIFY(selected);
    const auto expected=selected->account().alias==one.account.alias?QByteArray("syntheticKiwoomOwnerA"):QByteArray("syntheticKiwoomOwnerB");
    QVERIFY(quoteHttp.requests.last().headers.value("authorization").contains(expected));
    const int accountReads=accountA.sent.size()+accountB.sent.size(),authA=accountA.sent.count(Operation::Authenticate),authB=accountB.sent.count(Operation::Authenticate);
    const auto requestsBeforeLock=quoteHttp.requests.size();const auto opensBeforeLock=wire->opens;bool boundary=false;
    connect(&vault,&Vault::aboutToLock,&window,[&]{boundary=true;stream.tick();QVERIFY(!view->privateEnabled());QVERIFY(!stream.broker());QCOMPARE(quoteHttp.requests.size(),requestsBeforeLock);});
    window.lockAndClear();QVERIFY(boundary);QVERIFY(!view->privateEnabled());QVERIFY(view->selectedConnectionId().isEmpty());QVERIFY(!stream.broker());QVERIFY(stream.quotes().isEmpty());
    QVERIFY(workspace->watchlistModel()->holdings().isEmpty());QVERIFY(QMetaObject::invokeMethod(&stream,"changed",Qt::DirectConnection));QCoreApplication::processEvents();
    QCOMPARE(quoteHttp.requests.size(),requestsBeforeLock);QCOMPARE(wire->opens,opensBeforeLock);QCOMPARE(accountA.sent.size()+accountB.sent.size(),accountReads);
    QCOMPARE(accountA.sent.count(Operation::Authenticate),authA);QCOMPARE(accountB.sent.count(Operation::Authenticate),authB);
}
void LocalWindowTest::stockSourcesShareTheSelectedExistingTossOwnerAndRevokeBeforeLock(){
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-stock-owner.ini"),QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-stock-owner-vault"));QVERIFY(vault.create("Synthetic stock source password 2026!"));
    keefi::test::ReadFixture one("toss"),two("toss");one.account.alias="Synthetic owner A";two.account.alias="Synthetic owner B";
    one.credentials["client_id"]="SyntheticStockOwnerClientA";two.credentials["client_id"]="SyntheticStockOwnerClientB";
    QVERIFY(vault.addAccount(one.account,one.credentials,StorageMode::EncryptedVault));QVERIFY(vault.addAccount(two.account,two.credentials,StorageMode::EncryptedVault));
    keefi::test::ProviderReadTransport accountA,accountB;accountA.provider=accountB.provider="toss";
    accountA.transform=[](const RequestSpec& request,HttpResult& result){if(request.operation==Operation::Authenticate)result.body.replace("fakesignature","syntheticOwnerA");};
    accountB.transform=[](const RequestSpec& request,HttpResult& result){if(request.operation==Operation::Authenticate)result.body.replace("fakesignature","syntheticOwnerB");};
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    factories.trading=[&](const AccountRecord& account){return account.alias==one.account.alias?&accountA:&accountB;};
    PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);NoNetworkBrokerTransport coinHttp;MarketCandles candles(&coinHttp);
    const WatchlistIdentity stock{"stock","KR","005930"};
    StockWindowMarketTransport chartHttp,alertHttp;StockChartSource source(&chartHttp),alerts(&alertHttp);
    NoNetworkBrokerTransport quoteHttp,metricHttp,exploreHttp;MarketMetrics metrics(&metricHttp);PublicMarketCatalog catalog(&exploreHttp);
    auto socket=std::make_unique<StockWindowSocket>();TossMarketStream stream(nullptr,&quoteHttp,std::move(socket));
    LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog,false);QVERIFY(window.findChild<QStackedWidget*>("exploreStack"));
    window.setMarketMetrics(&metrics);QVERIFY(window.findChild<QWidget*>("marketFlowContext"));
    window.setMarketCandles(&candles);window.setTossMarketStream(&stream);window.setStockChartSources(&source,&alerts);window.resize(1280,900);window.show();
    portfolio.setAutomaticRefresh(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().size(),2,2500);
    QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections()[0].state=="complete"&&portfolio.connections()[1].state=="complete",8000);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    workspace->setIdentity(stock.key());
    auto* flow=child<TossMarketView>(window,"tossMarketView");QVERIFY(flow);auto* owners=child<QComboBox>(*flow,"tossMarketConnections");QVERIFY(owners);
    QTRY_COMPARE(owners->count(),2);
    QTRY_VERIFY_WITH_TIMEOUT(!chartHttp.requests.isEmpty(),2500);
    const auto previousRequests=chartHttp.requests.size();const auto previousAborts=chartHttp.aborts;
    owners->setCurrentIndex(owners->currentIndex()==0?1:0);
    QCOMPARE(flow->selectedConnectionId(),owners->currentData().toString());
    auto* selected=portfolio.broker(owners->currentData().toString());QVERIFY(selected);
    const auto expected=selected->account().alias==one.account.alias?QByteArray("syntheticOwnerA"):QByteArray("syntheticOwnerB");
    source.refresh();alerts.setSelection(stock,900000);alerts.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(chartHttp.requests.size()>previousRequests&&chartHttp.requests.last().headers.value("Authorization").contains(expected),2500);
    QTRY_VERIFY_WITH_TIMEOUT(!alertHttp.requests.isEmpty()&&alertHttp.requests.last().headers.value("Authorization").contains(expected),2500);
    QVERIFY2(chartHttp.aborts>previousAborts,"Changing the borrowed owner must cancel the previous owner's pending dedicated GET");
    const auto reads=accountA.sent.size()+accountB.sent.size();const auto authA=accountA.sent.count(Operation::Authenticate),authB=accountB.sent.count(Operation::Authenticate);
    window.hide();QVERIFY(alerts.active());QCOMPARE(flow->selectedConnectionId(),owners->currentData().toString());
    const auto chartRequests=chartHttp.requests.size(),alertRequests=alertHttp.requests.size();
    bool boundary=false;connect(&vault,&Vault::aboutToLock,&window,[&]{boundary=true;source.refresh();alerts.refresh();
        QCOMPARE(chartHttp.requests.size(),chartRequests);QCOMPARE(alertHttp.requests.size(),alertRequests);});
    window.lockAndClear();QVERIFY(boundary);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QTRY_COMPARE(source.snapshot().series.reason,QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QCOMPARE(source.snapshot().source,StockSourceKind::None);QVERIFY(!source.snapshot().series.available);
    QCOMPARE(chartHttp.requests.size(),chartRequests);QCOMPARE(alertHttp.requests.size(),alertRequests);
    QVERIFY(workspace->watchlistModel()->holdings().isEmpty());QCOMPARE(accountA.sent.size()+accountB.sent.size(),reads);
    QCOMPARE(accountA.sent.count(Operation::Authenticate),authA);QCOMPARE(accountB.sent.count(Operation::Authenticate),authB);
}

namespace {
class ChartWindowSink final:public SystemNotificationSink {
  public:
    int requests=0,posts=0;bool granted=false;SystemNotice last;
    SystemNoticePermission permission()const override{return granted?SystemNoticePermission::Granted:SystemNoticePermission::Unknown;}
    QString status()const override{return "합성 권한 미확인";}
    void refreshPermission()override{}
    void requestAuthorization()override{++requests;}
    void post(const SystemNotice& notice)override{++posts;last=notice;emit submissionFinished(notice.identifier,notice.privateEpoch,true);}
    void remove(const QStringList&)override{}
    void setPrivateContext(quint64,bool)override{}
    void stop()override{}
};
class ChartWindowTray final:public BackgroundTray {
  public:
    bool shown=false;QMenu* menu=nullptr;
    bool available()const override{return true;}
    bool visible()const override{return shown;}
    void setMenu(QMenu* value)override{menu=value;}
    void show()override{shown=true;}
    void hide()override{shown=false;}
};
}
void LocalWindowTest::hiddenCoinFlowStopsWhenItsPublicAlertFeedStops(){
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-flow-stop.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-flow-stop-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);
    CoinWindowPublicTransport flowHttp,feedHttp;NoNetworkBrokerTransport metadata;CoinFlowSource flow(&flowHttp);MarketCandles candles(nullptr);ChartAlertFeed feed(&feedHttp);
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);
    window.setMarketCandles(&candles);window.setCoinFlowSource(&flow);window.setChartAlerts(&engine,&feed,&sink,&pack,&profile);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    const auto list=workspace->watchlistModel()->createList("합성 공개 체결 알림");
    QVERIFY(workspace->watchlistModel()->addIdentity(list,{"coin","upbit","KRW-SYN"}));
    ChartAlertRule rule;rule.listId=list;rule.enabled=true;rule.pullback=false;rule.results=false;rule.whales=true;
    QVERIFY(engine.setRule(rule));QVERIFY(feed.running());QTRY_VERIFY_WITH_TIMEOUT(flow.active(),1500);
    QCOMPARE(flow.snapshot().identity,QString("coin:upbit:KRW-SYN"));
    feed.setRunning(false);QVERIFY2(!flow.active(),"Stopping a hidden window's sole alert consumer must stop the public collector immediately");
    const auto count=flowHttp.requests.size();flow.pump();QCOMPARE(flowHttp.requests.size(),count);
}
void LocalWindowTest::chartAlertFeedUsesOnlyCurrentVerifiedUpbitMarkets(){
    QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-alert-catalogue.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-alert-catalogue-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);
    CoinWindowPublicTransport feedHttp,publicHttp;NoNetworkBrokerTransport metadata;
    PublicMarketCatalog markets(&publicHttp);MarketCandles candles(nullptr);ChartAlertFeed feed(&feedHttp);
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);
    window.setPublicMarketCatalog(&markets,false);window.setMarketCandles(&candles);window.setChartAlerts(&engine,&feed,&sink,&pack,&profile);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    const auto list=workspace->watchlistModel()->createList("합성 공개 알림 종목");
    QVERIFY(workspace->watchlistModel()->addIdentity(list,{"coin","upbit","KRW-BTC"}));
    QVERIFY(workspace->watchlistModel()->addIdentity(list,{"coin","upbit","KRW-HYPE"}));
    ChartAlertRule rule;rule.listId=list;rule.enabled=true;rule.pullback=true;rule.results=false;rule.whales=false;
    QVERIFY(engine.setRule(rule));feed.pump();QCoreApplication::processEvents();
    QCOMPARE(feedHttp.requests.size(),0);
    QVERIFY(!window.isVisible());QVERIFY(!markets.presentationActive());
    markets.refresh();QTRY_VERIFY(publicHttp.token>0);
    HttpResult response;response.status=200;
    response.body=R"([{"market":"KRW-BTC","korean_name":"합성 비트코인","english_name":"Synthetic Bitcoin","market_event":{"warning":false,"caution":{}}}])";
    emit publicHttp.finished(publicHttp.token,response);QTRY_VERIFY(markets.marketListAvailable());feed.pump();
    QTRY_VERIFY(!feedHttp.requests.isEmpty());
    for(const auto& request:feedHttp.requests){QVERIFY(!request.authenticated);
        QCOMPARE(QUrlQuery(request.url).queryItemValue("market"),QString("KRW-BTC"));}
    for(const auto& request:publicHttp.requests)QCOMPARE(request.url.path(),QString("/v1/market/all"));
    QVERIFY(!markets.presentationActive());
    QVERIFY(!vault.isUnlocked());QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
}
void LocalWindowTest::coinFlowWindowSettlesAfterOverlappingMinuteConsumers(){
    QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-flow-feedback.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-flow-feedback-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    const qint64 now=QDateTime::currentMSecsSinceEpoch(),end=now/60000*60000;
    PublicMarketTransport publicHttp;publicHttp.quoteStatus=503;PublicMarketCatalog markets(&publicHttp);
    CoinWindowPublicTransport chartHttp,minuteHttp,flowHttp,alertHttp;NoNetworkBrokerTransport metadata;
    MarketCandles candles(&chartHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    MarketCandles minutes(&minuteHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    CoinFlowSource flow(&flowHttp,{},{},nullptr,[&]{return now;});
    CoinFlowInstrument instrument;instrument.venue=CoinFlowVenue::UpbitSpot;instrument.grain=CoinFlowGrain::UpbitTrade;
    instrument.identity="coin:upbit:KRW-BTC";instrument.symbol="KRW-BTC";instrument.baseAsset="BTC";instrument.quoteAsset="KRW";instrument.verified=true;
    instrument.evidence={"합성 공식 종목 목록","https://api.upbit.com/v1/market/all",0,now,now};
    flow.setPublicCatalog(CoinFlowVenue::UpbitSpot,{instrument});
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);ChartAlertFeed alerts(&alertHttp);
    bool monitoring=false;int notices=0;std::unique_ptr<QSignalBlocker> stopRunawaySignals;
    LocalWindow window(vault,accounts);
    // Observe before the real window slots, so even a synchronous feedback loop
    // has a finite signal budget and reports a failure instead of hanging CI.
    connect(&flow,&CoinFlowSource::changed,&window,[&]{
        if(monitoring&&++notices>64&&!stopRunawaySignals)stopRunawaySignals=std::make_unique<QSignalBlocker>(&flow);
    });
    window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);
    window.setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window.setCoinFlowSource(&flow);window.setCoinFlowMinuteModel(&minutes);window.setMarketCandles(&candles);
    window.setChartAlerts(&engine,&alerts,&sink,&pack,&profile);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    const auto list=workspace->watchlistModel()->createList("합성 공개 재현");
    QVERIFY(workspace->watchlistModel()->addIdentity(list,{"coin","upbit","KRW-BTC"}));
    QVERIFY(workspace->watchlistModel()->addIdentity(list,{"coin","upbit","KRW-HYPE"}));
    window.resize(1440,900);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QTRY_VERIFY(minuteHttp.token>0);QVERIFY(flow.active());
    QJsonArray bars;for(int i=0;i<200;++i){const auto at=end-(i+2)*60000;
        bars.append(QJsonObject{{"market","KRW-BTC"},{"unit","1"},{"timestamp",QString::number(at+1000)},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,QTimeZone("UTC")).toString("yyyy-MM-ddTHH:mm:ss")},
            {"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price","101"},{"candle_acc_trade_volume","2"}});
    }
    HttpResult response;response.status=200;response.body=QJsonDocument(bars).toJson();emit minuteHttp.finished(minuteHttp.token,response);
    QTRY_COMPARE(flow.snapshot().minutes.size(),200);
    auto newer=minutes.series();QVERIFY(newer.available);QCOMPARE(newer.candles.size(),200);
    newer.candles.removeFirst();newer.candles.append({end-60000,0,"100","110","90","102","3"});
    // A second real consumer (the alert feed) may complete one minute sooner
    // than the selected chart's still-valid 200-bar window.
    monitoring=true;flow.setMinuteSeries(newer);
    for(int turn=0;turn<20&&!stopRunawaySignals;++turn){
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCoreApplication::processEvents(QEventLoop::AllEvents,2);
    }
    QCOMPARE(notices,1);
    QCOMPARE(flow.snapshot().minutes.size(),200);
    QCOMPARE(flow.snapshot().minutes.first().candle.startsAtMs,end-200*60000);
    QVERIFY(!vault.isUnlocked());
}
void LocalWindowTest::coinFlowRawTradeBurstDoesNotSynchronouslyRefreshLists(){
    QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-flow-burst.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-flow-burst-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);const qint64 now=QDateTime::currentMSecsSinceEpoch();
    PublicMarketTransport publicHttp;publicHttp.quoteStatus=503;PublicMarketCatalog markets(&publicHttp);
    CoinWindowPublicTransport flowHttp,alertHttp;NoNetworkBrokerTransport chartHttp,metadata;
    class Socket final:public PublicCandleSocket{public:quint64 generation=0;int aborts=0;
        void open(quint64 value,const QUrl&)override{generation=value;}
        void send(quint64,const QByteArray&)override{}void abort()override{++aborts;}};
    auto primary=std::make_unique<Socket>();auto* wire=primary.get();
    MarketCandles candles(&chartHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});
    CoinFlowSource flow(&flowHttp,std::move(primary),{},nullptr,[&]{return now;});
    CoinFlowInstrument instrument;instrument.venue=CoinFlowVenue::UpbitSpot;instrument.grain=CoinFlowGrain::UpbitTrade;
    instrument.identity="coin:upbit:KRW-BTC";instrument.symbol="KRW-BTC";instrument.baseAsset="BTC";instrument.quoteAsset="KRW";instrument.verified=true;
    instrument.evidence={"합성 공식 종목 목록","https://api.upbit.com/v1/market/all",0,now,now};flow.setPublicCatalog(CoinFlowVenue::UpbitSpot,{instrument});
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);ChartAlertFeed alerts(&alertHttp);
    auto window=std::make_unique<LocalWindow>(vault,accounts);window->setPortfolioController(&portfolio);window->setChartSettings(&profile);
    window->setChartInstrumentTransport(&metadata);window->setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window->setCoinFlowSource(&flow);window->setMarketCandles(&candles);window->setChartAlerts(&engine,&alerts,&sink,&pack,&profile);
    auto* workspace=child<ChartWorkspace>(*window,"chartWorkspace");QVERIFY(workspace);
    window->resize(1440,900);window->show();auto* pages=child<QTabWidget>(*window,"portfolioPages");pages->setCurrentWidget(workspace);
    QTRY_VERIFY(wire->generation>0);QVERIFY(flowHttp.token>0);QCOMPARE(flowHttp.requests.last().url.path(),QString("/v1/trades/ticks"));
    HttpResult seeded;seeded.status=200;seeded.body="[]";emit flowHttp.finished(flowHttp.token,seeded);emit wire->connected(wire->generation);
    // Finish initial widget layout, metadata delivery and baseline presentation
    // before measuring the unrelated raw-trade burst.
    QTest::qWait(400);
    // Observe the real public socket intake and the expensive list-to-alert
    // resynchronization independently. No private account/profile is involved.
    QSignalSpy observations(&flow,&CoinFlowSource::changed),listSyncs(&engine,&ChartAlertEngine::demandsChanged);
    for(int i=0;i<100;++i){const auto at=now-100+i;
        const auto bytes=QJsonDocument(QJsonObject{{"type","trade"},{"code","KRW-BTC"},{"trade_price","100"},{"trade_volume","2"},
            {"ask_bid","BID"},{"sequential_id",QString::number(9007199254740000LL+i)},
            {"trade_timestamp",QString::number(at)},{"timestamp",QString::number(at)},{"stream_type","REALTIME"}}).toJson(QJsonDocument::Compact);
        emit wire->received(wire->generation,bytes);
    }
    QCOMPARE(observations.size(),100);QCOMPARE(flow.snapshot().primary.trades.size(),100);
    QCOMPARE(listSyncs.size(),0); // Raw event intake must not rebuild the whole list for every trade.
    QTRY_VERIFY_WITH_TIMEOUT(!listSyncs.isEmpty(),750);QVERIFY2(listSyncs.size()<=8,qPrintable(QString("burst list resynchronizations: %1").arg(listSyncs.size())));
    // View changes and teardown still revoke collection immediately, even when
    // another raw event has already scheduled a delayed presentation update.
    emit flow.changed();pages->setCurrentIndex(0);QVERIFY(!flow.active());
    pages->setCurrentWidget(workspace);QVERIFY(flow.active());emit flow.changed();window.reset();QVERIFY(!flow.active());
    QVERIFY(!vault.exists());
}
void LocalWindowTest::chartDrawingLineProjectionRequiresEveryExplicitPublicOptIn(){
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-c8-line.ini"),QSettings::IniFormat);Vault vault(dir.filePath("synthetic-c8-line-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);MarketCandles candles(nullptr);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);auto* drawings=workspace->drawingModel();QVERIFY(drawings);
    const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};const auto owner=workspace->watchlistModel()->createList("합성 기준선");QVERIFY(!owner.isEmpty());
    QVERIFY(workspace->watchlistModel()->addIdentity(owner,identity));QVERIFY(drawings->setContext({identity,"KRW",true},60000));
    constexpr qint64 start=1728000000000;ChartDrawingFrame frame{identity,"KRW",60000,start+120000,drawings->contextGeneration(),{}};
    frame.bars.append({{start,0,"100","110","90","105","1"},start+60000,true,false});const auto id=drawings->create(ChartDrawingKind::Horizontal,{{start,"100.1250"}},frame);QVERIFY(!id.isEmpty());
    QVERIFY(drawings->setAlert(id,{true,owner,60000}));QVERIFY(engine.demands().isEmpty());
    auto rule=engine.rule(owner);rule.pullback=rule.results=rule.whales=false;rule.horizontal=true;QVERIFY(engine.setRule(rule));QVERIFY(engine.demands().isEmpty());
    rule.enabled=true;QVERIFY(engine.setRule(rule));QTRY_COMPARE(engine.demands().size(),1);QVERIFY(engine.demands().first().horizontal);
    drawings->setVisible(false);drawings->setLocked(true);QCOMPARE(engine.demands().size(),1);QCOMPARE(engine.demands().first().intervalMs,60000LL);
    QVERIFY(drawings->setContext({identity,"KRW",true},3600000));QCOMPARE(engine.demands().first().intervalMs,60000LL);
    drawings->setLocked(false);QVERIFY(drawings->clear());QTRY_VERIFY(engine.demands().isEmpty());QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void LocalWindowTest::chartAlertsPersistPublicPreferencesWithoutSystemAuthorization(){
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-alert-ui.ini"),QSettings::IniFormat);Vault vault(dir.filePath("synthetic-locked-alerts"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);MarketCandles candles(nullptr);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);
    auto* panel=window.findChild<ChartAlertsPanel*>("chartAlertsPanel");QVERIFY(panel);QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
    engine.setDripEnabled(false);const auto state=profile.value("chart/alerts/v1").toByteArray();QVERIFY(!state.isEmpty());
    const auto object=QJsonDocument::fromJson(state).object();QVERIFY(!object.value("settings").toObject().value("dripEnabled").toBool());
    QVERIFY(!state.contains("holdingsIdentities"));QVERIFY(!state.contains("quantity"));QVERIFY(!vault.isUnlocked());
    if(const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!preview.isEmpty()){
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");QVERIFY(pages);pages->setCurrentWidget(window.findChild<ChartWorkspace*>());
        window.resize(1440,1040);window.show();QTest::qWait(60);QVERIFY(window.grab().save(preview+"/c7-alerts-wide.png"));
        window.resize(820,1400);QTest::qWait(60);QVERIFY(window.grab().save(preview+"/c7-alerts-narrow.png"));
        panel->openSettings();QTest::qWait(40);auto* dialog=window.findChild<QDialog*>("optionsDialog");QVERIFY(dialog);
        QVERIFY(dialog->grab().save(preview+"/c7-alert-settings.png"));dialog->close();
    }
}
void LocalWindowTest::chartSettingsRetargetAndSurviveOptionsLifetime(){
    QTemporaryDir dir;QSettings profile(dir.filePath("chart-options.ini"),QSettings::IniFormat);
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    Vault vault(dir.filePath("synthetic-chart-options-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);ChartWindowTray tray;
    ChartWindowSink sink;sink.granted=true;ChartPhrasePack pack;ChartAlertEngine engine(&sink);
    NoNetworkBrokerTransport http,metadata,catalogHttp,metricHttp;MarketCandles candles(&http);
    PublicMarketCatalog catalog(&catalogHttp);MarketMetrics metrics(&metricHttp);LocalWindow window(vault,accounts,nullptr,&tray);
    window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setChartInstrumentTransport(&metadata);
    window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);
    window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* panel=child<ChartAlertsPanel>(window,"chartAlertsPanel");QVERIFY(panel);
    QCOMPARE(panel->parentWidget(),&window);QVERIFY(panel->isHidden());
    QVERIFY(!workspace->findChild<ChartAlertsPanel*>());
    const auto first=workspace->watchlistModel()->createList("합성 첫 리스트");
    const auto second=workspace->watchlistModel()->createList("합성 둘째 리스트");
    window.resize(1280,900);window.show();
    child<QPushButton>(window,"openOptions")->click();QPointer<QDialog> options=child<QDialog>(window,"optionsDialog");QVERIFY(options);
    auto* displayTab=child<QPushButton>(*options,"optionsDisplayTab");displayTab->click();displayTab->setFocus(Qt::OtherFocusReason);QTRY_VERIFY(displayTab->hasFocus());
    emit workspace->listAlertsRequested(first);
    QCOMPARE(child<QDialog>(window,"optionsDialog"),options.data());
    QVERIFY(child<QWidget>(*options,"optionsAlerts")->isVisibleTo(options));
    auto* alertsTab=child<QPushButton>(*options,"optionsAlertsTab");QTRY_VERIFY(alertsTab->hasFocus());
    auto* list=child<QComboBox>(*options,"chartAlertList");QVERIFY(list);QCOMPARE(list->currentData().toString(),first);
    list->setFocus(Qt::OtherFocusReason);QTRY_VERIFY(list->hasFocus());
    emit workspace->listAlertsRequested(second);QCOMPARE(list->currentData().toString(),second);
    QTRY_VERIFY(alertsTab->hasFocus());
    QCOMPARE(window.findChildren<QDialog*>("optionsDialog").size(),1);
    auto* drip=child<QCheckBox>(*options,"chartAlertSettingsDrip");QVERIFY(drip);
    drip->setChecked(false);QVERIFY(!engine.settings().dripEnabled);
    auto* enabled=child<QCheckBox>(*options,"chartListAlertsEnabled");QVERIFY(enabled);enabled->setChecked(true);
    QVERIFY(engine.rule(second).enabled);
    QVERIFY(!child<QCheckBox>(*options,"chartAlertBackgroundEnabled"));
    auto* background=child<QCheckBox>(*options,"optionBackground");QVERIFY(background);
    emit panel->generalSettingsRequested();QVERIFY(child<QWidget>(*options,"optionsGeneral")->isVisibleTo(options));
    QTRY_VERIFY(child<QPushButton>(*options,"optionsGeneralTab")->hasFocus());
    background->setChecked(false);QVERIFY(!panel->backgroundEnabled());
    auto* today=window.findChild<ExploreTodayView*>();QVERIFY(today);emit today->alertSettingsRequested();
    QCOMPARE(child<QDialog>(window,"optionsDialog"),options.data());
    QVERIFY(child<QWidget>(*options,"optionsMarketAlerts")->isVisibleTo(options));
    QTRY_VERIFY(child<QPushButton>(*options,"optionsMarketAlertsTab")->hasFocus());
    auto* metricAlert=child<QCheckBox>(*options,"optionmarketMetricsGapAlert");QVERIFY(metricAlert);metricAlert->setChecked(true);
    QVERIFY(engine.marketMetricEnabled(MarketFlowMetric::KoreaGap));
    QVERIFY(!profile.value("market/flow-alerts/v1").toByteArray().isEmpty());QCOMPARE(sink.requests,0);
    const auto state=profile.value("chart/alerts/v1").toByteArray();
    const auto ui=profile.value("chart/alerts/ui/v1").toByteArray();
    options->close();QTRY_VERIFY(options.isNull());
    QVERIFY(!panel->backgroundEnabled());QCOMPARE(profile.value("chart/alerts/v1").toByteArray(),state);
    QCOMPARE(profile.value("chart/alerts/ui/v1").toByteArray(),ui);
    emit engine.noticeSubmitted("after-options-close","합성 공개 알림","OPTIONS_CLOSED_PUBLIC_HISTORY",false);
    emit workspace->listAlertsRequested(first);options=child<QDialog>(window,"optionsDialog");QVERIFY(options);
    QTRY_VERIFY(child<QPushButton>(*options,"optionsAlertsTab")->hasFocus());
    QCOMPARE(child<QComboBox>(*options,"chartAlertList")->currentData().toString(),first);
    QVERIFY(!child<QCheckBox>(*options,"chartAlertSettingsDrip")->isChecked());
    bool found=false;for(auto* label:options->findChildren<QLabel*>())found|=label->text().contains("OPTIONS_CLOSED_PUBLIC_HISTORY");
    QVERIFY(found);QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
    if(const auto renderDir=qEnvironmentVariable("KEEFI_CHART_SETTINGS_RENDER_DIR");!renderDir.isEmpty()){
        // Opt-in captures use this test's synthetic profile and injected sources.
        // Keep the current theme so the native run can choose its own appearance.
        QVERIFY(QDir().mkpath(renderDir));
        const auto capture=[&](const QString& pageName,const QSize& size,const QString& name){
            auto* tab=options->findChild<QPushButton*>(pageName+"Tab");if(!tab)return false;
            tab->click();tab->setFocus(Qt::OtherFocusReason);options->resize(size);options->raise();options->activateWindow();
            QCoreApplication::processEvents();QTest::qWait(80);QCoreApplication::processEvents();
            return tab->hasFocus()&&options->grab().save(QDir(renderDir).filePath(name+".png"));
        };
        QVERIFY(capture("optionsAlerts",QSize(900,720),"alerts-wide"));
        QVERIFY(capture("optionsAlerts",QSize(560,560),"alerts-narrow"));
        QVERIFY(capture("optionsMarketAlerts",QSize(900,720),"market-alerts-wide"));
        QVERIFY(capture("optionsMarketAlerts",QSize(560,560),"market-alerts-narrow"));
    }
    options->close();QTRY_VERIFY(options.isNull());
}
void LocalWindowTest::chartAlertRulesRestoreAfterTheirPublicListIdentities(){
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-alert-restore.ini"),QSettings::IniFormat);
    WatchlistWorkspaceList savedLists;const auto listId=savedLists.createList("합성 알림 리스트");
    QVERIFY(savedLists.addIdentity(listId,{"coin","upbit","KRW-SYN"}));
    profile.setValue("chart/watchlists/v2",savedLists.publicState());
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine savedEngine(&sink);
    QVERIFY(savedEngine.setPublicLists({{listId,{{"coin","upbit","KRW-SYN"}}}}));
    auto rule=savedEngine.rule(listId);rule.enabled=true;rule.minimumScore=8;QVERIFY(savedEngine.setRule(rule));
    profile.setValue("chart/alerts/v1",savedEngine.publicState());
    Vault vault(dir.filePath("synthetic-alert-restore-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartAlertEngine engine(&sink);MarketCandles candles(nullptr);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);
    QVERIFY(engine.rule(listId).enabled);QCOMPARE(engine.rule(listId).minimumScore,8);QCOMPARE(engine.demands().size(),1);
    QCOMPARE(sink.requests,0);QVERIFY(!vault.isUnlocked());
}
void LocalWindowTest::chartAlertPrivateStateIsClearedByTheWindowLockBoundary(){
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-alert-lock.ini"),QSettings::IniFormat);Vault vault(dir.filePath("synthetic-locked-alerts"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);MarketCandles candles(nullptr);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);
    auto* panel=window.findChild<ChartAlertsPanel*>("chartAlertsPanel");QVERIFY(panel);
    engine.setPrivateEnabled(true);const auto epoch=engine.privateEpoch();engine.setHoldings({{"coin","upbit","KRW-SYN"}},QDateTime::currentMSecsSinceEpoch(),epoch);QCOMPARE(engine.demands().size(),1);
    emit engine.noticeSubmitted("synthetic-private","합성 보유","SYNTHETIC_PRIVATE_BODY",true);
    emit panel->settingsRequested({});
    QPointer<QDialog> options=window.findChild<QDialog*>("optionsDialog");QVERIFY(options);
    bool privateVisible=false;for(auto* label:options->findChildren<QLabel*>())privateVisible|=label->text().contains("SYNTHETIC_PRIVATE_BODY");
    QVERIFY(privateVisible);
    window.lockAndClear();QVERIFY(engine.privateEpoch()>epoch);QVERIFY(engine.demands().isEmpty());
    emit panel->settingsRequested({});options=window.findChild<QDialog*>("optionsDialog");QVERIFY(options);
    for(auto* label:options->findChildren<QLabel*>()){
        QVERIFY(!label->text().contains("SYNTHETIC_PRIVATE_BODY"));
        QVERIFY(!label->toolTip().contains("SYNTHETIC_PRIVATE_BODY"));
        QVERIFY(!label->accessibleName().contains("SYNTHETIC_PRIVATE_BODY"));
    }
    auto* history=panel->findChild<QListWidget*>("chartAlertHistory");QVERIFY(history);for(int i=0;i<history->count();++i)QVERIFY(!history->item(i)->text().contains("SYNTHETIC_PRIVATE_BODY"));
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void LocalWindowTest::hiddenWindowRoutesOwnedFuturesRiskWithoutExtraAccountReads(){
    class PrivateTransport final:public TradingTransport {public:int calls=0;void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!isMutation(request.operation));++calls;const auto response=keefi::test::usdmResponse(request);
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});}void abort()override{}} privateTransport;
    class ContractTransport final:public TradingTransport {public:int calls=0;void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QCOMPARE(request.url,MarketMetrics::request(MarketMetrics::Read::Contracts).url);++calls;
        const auto response=keefi::test::usdmResponse(request);QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});}void abort()override{}} contracts;
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-hidden-risk.ini"),QSettings::IniFormat);Vault vault(dir.filePath("synthetic-hidden-risk-vault"));
    QVERIFY(vault.create("Synthetic window risk password 2026!"));auto account=keefi::test::usdmAccount();account.id.clear();
    QVERIFY(vault.addAccount(account,{{"api_key","SyntheticWindowRiskKey"},{"secret","SyntheticWindowRiskSecret"}},StorageMode::EncryptedVault));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &privateTransport;};PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowSink sink;sink.granted=true;ChartPhrasePack pack;ChartAlertEngine engine(&sink);auto rule=engine.rule("@holdings");rule.liquidationDistancePercent=100;QVERIFY(engine.setRule(rule));
    MarketCandles candles(nullptr);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,8000);const auto existingReads=privateTransport.calls;
    window.setChartSettings(&profile);window.setMarketCandles(&candles);window.setChartContractTransport(&contracts);
    window.findChild<ChartWorkspace*>()->setAsset("ETH");window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);QVERIFY(!window.isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(contracts.calls,1,2500);QTRY_COMPARE_WITH_TIMEOUT(sink.posts,1,2500);
    QVERIFY(sink.last.privateData);QVERIFY(sink.last.body.contains("BTCUSDT"));QVERIFY(!sink.last.body.contains("SyntheticWindowRisk"));
    QCOMPARE(privateTransport.calls,existingReads);QVERIFY(engine.demands().isEmpty()); // No spot holding is fabricated from futures exposure.
    const auto epoch=engine.privateEpoch();window.lockAndClear();QVERIFY(engine.privateEpoch()>epoch);QVERIFY(window.findChild<ChartWorkspace*>()->futuresPositionObservations().isEmpty());
    emit portfolio.changed();QCoreApplication::processEvents();QCOMPARE(sink.posts,1);QCOMPARE(sink.requests,0);
}
void LocalWindowTest::normalQuitPreparationPrecedesLockAndSecurityLockBypassesIt_data(){
    QTest::addColumn<bool>("saved");QTest::newRow("saved")<<true;QTest::newRow("failed")<<false;
}
void LocalWindowTest::normalQuitPreparationPrecedesLockAndSecurityLockBypassesIt(){
    QFETCH(bool,saved);
    QTemporaryDir dir;Vault vault(dir.filePath("synthetic-quit-vault"));
    const QString password="Fake quitting password 2026!";QVERIFY(vault.create(password));
    AccountModel accounts(&vault);ChartWindowTray tray;LocalWindow window(vault,accounts,nullptr,&tray);
    auto* background=window.findChild<QCheckBox*>("backgroundOption");QVERIFY(background);background->setChecked(false);
    int preparations=0;std::function<void(bool)> complete;
    window.setQuitPreparation([&](std::function<void(bool)> callback){++preparations;complete=std::move(callback);});
    QSignalSpy quits(&window,&LocalWindow::quitRequested);
    window.lockAndClear();QCOMPARE(preparations,0);QVERIFY(!vault.isUnlocked());
    QVERIFY(vault.unlock(password));window.show();window.close();
    QCOMPARE(preparations,1);QVERIFY(vault.isUnlocked());QCOMPARE(quits.size(),0);QVERIFY(bool(complete));
    window.close();QCOMPARE(preparations,1); // Repeated quit shares the original deadline.
    complete(saved);QTRY_COMPARE(quits.size(),1);QVERIFY(!vault.isUnlocked());
    complete(!saved);QCoreApplication::processEvents();QCOMPARE(quits.size(),1);
    QVERIFY(window.findChildren<QMessageBox*>().isEmpty());
}
void LocalWindowTest::quitPreparationTimeoutCancelsOnceAndNeverAsks(){
    QTemporaryDir dir;Vault vault(dir.filePath("synthetic-timeout-vault"));QVERIFY(vault.create("Fake quitting password 2026!"));
    AccountModel accounts(&vault);ChartWindowTray tray;LocalWindow window(vault,accounts,nullptr,&tray);
    window.findChild<QCheckBox*>("backgroundOption")->setChecked(false);
    int preparations=0,cancellations=0;std::function<void(bool)> late;
    window.setQuitPreparation([&](std::function<void(bool)> callback){++preparations;late=std::move(callback);},[&]{++cancellations;});
    QSignalSpy quits(&window,&LocalWindow::quitRequested);window.show();QElapsedTimer timer;timer.start();window.close();
    window.close();QCOMPARE(preparations,1);QVERIFY(vault.isUnlocked());
    QTRY_COMPARE_WITH_TIMEOUT(quits.size(),1,3600);QCOMPARE(cancellations,1);QVERIFY(timer.elapsed()<3600);QVERIFY(!vault.isUnlocked());
    QVERIFY(window.findChildren<QMessageBox*>().isEmpty());late(false);QCoreApplication::processEvents();
    QCOMPARE(quits.size(),1);QCOMPARE(cancellations,1);
}
void LocalWindowTest::applicationQuitUsesTheBoundedPreparation(){
    QTemporaryDir dir;Vault vault(dir.filePath("synthetic-application-quit"));AccountModel accounts(&vault);ChartWindowTray tray;
    LocalWindow window(vault,accounts,nullptr,&tray);std::function<void(bool)> complete;
    window.setQuitPreparation([&](std::function<void(bool)> callback){complete=std::move(callback);});
    QSignalSpy quits(&window,&LocalWindow::quitRequested);QEvent request(QEvent::Quit);
    QCoreApplication::sendEvent(QCoreApplication::instance(),&request);QVERIFY(bool(complete));QCOMPARE(quits.size(),0);
    complete(false);QTRY_COMPARE(quits.size(),1);
}
void LocalWindowTest::injectedTrayKeepsCloseRunningAndPreferenceOffQuits(){
    QTemporaryDir dir;QSettings profile(dir.filePath("synthetic-tray.ini"),QSettings::IniFormat);Vault vault(dir.filePath("synthetic-locked-tray"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartWindowTray tray;ChartWindowSink sink;ChartPhrasePack pack;ChartAlertEngine engine(&sink);MarketCandles candles(nullptr);
    LocalWindow window(vault,accounts,nullptr,&tray);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);window.setChartAlerts(&engine,nullptr,&sink,&pack,&profile);
    auto* background=window.findChild<QCheckBox*>("backgroundOption");QVERIFY(background);QVERIFY(background->isChecked());QVERIFY(tray.visible());
    auto* panel=window.findChild<ChartAlertsPanel*>("chartAlertsPanel");QVERIFY(panel);QSignalSpy quit(&window,&LocalWindow::quitRequested);window.show();
    QTimer::singleShot(0,&window,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget))message->accept();});
    window.close();QTRY_VERIFY(!window.isVisible());QCOMPARE(quit.size(),0);QVERIFY(panel->closeHintSeen());
    window.show();panel->setBackgroundEnabled(false);QVERIFY(!background->isChecked());window.close();QCOMPARE(quit.size(),1);QCOMPARE(sink.requests,0);
}
void LocalWindowTest::chartStudyBadgesUseOnlyCompletedBarsAndEnabledChoices(){
    QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-c4-badges.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-c4-badges-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    class Http final:public TradingTransport{public:quint64 token=0;void send(quint64 t,const RequestSpec&)override{token=t;}void abort()override{}} http;
    class Socket final:public PublicCandleSocket{public:quint64 epoch=0;void open(quint64 e,const QUrl&)override{epoch=e;}void send(quint64,const QByteArray&)override{}void abort()override{}};
    auto socket=std::make_unique<Socket>();auto* stream=socket.get();MarketCandles candles(&http,std::move(socket));
    PublicMarketTransport publicHttp;publicHttp.quoteStatus=503;PublicMarketCatalog markets(&publicHttp);NoNetworkBrokerTransport metadata;
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);
    window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window.setMarketCandles(&candles);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    window.resize(1280,850);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);QTRY_VERIFY(http.token);
    emit stream->connected(stream->epoch);
    const auto now=QDateTime::currentMSecsSinceEpoch(),start=now/MarketCandles::IntervalMs*MarketCandles::IntervalMs;
    const auto bar=[&](qint64 at,QString close,QString volume){return QJsonObject{{"market","KRW-BTC"},{"unit","15"},
        {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(at==start?now:at+1000)},{"opening_price","100"},{"high_price","110"},{"low_price","90"},{"trade_price",close},{"candle_acc_trade_volume",volume}};};
    QJsonArray rows;for(int i=30;i>0;--i)rows.append(bar(start-i*MarketCandles::IntervalMs,"101","2"));rows.append(bar(start,"105","3"));
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit http.finished(http.token,response);
    auto* lists=workspace->watchlistModel();const WatchlistIdentity identity{"coin","upbit","KRW-BTC"};
    QTRY_VERIFY(lists->rowState(identity).badges.join(' ').contains(QStringLiteral("Trend Cloud")));
    const auto before=lists->rowState(identity).badges;auto* prefs=workspace->indicatorPreferences();QVERIFY(prefs);
    QVERIFY(prefs->setVisible("cloud",false));QCOMPARE(lists->rowState(identity).badges,before);
    QVERIFY(prefs->setEnabled("cloud",false));QVERIFY(!lists->rowState(identity).badges.join(' ').contains(QStringLiteral("Trend Cloud")));
    const auto disabled=lists->rowState(identity).badges;
    auto forming=bar(start,"109","999");forming.remove("market");forming.remove("unit");forming["type"]="candle.15m";forming["code"]="KRW-BTC";forming["stream_type"]="REALTIME";
    emit stream->received(stream->epoch,QJsonDocument(forming).toJson());QCOMPARE(candles.series().forming->close,QString("109"));
    QCOMPARE(lists->rowState(identity).badges,disabled);
    if(const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!preview.isEmpty()){
        // Public synthetic observations only; render all C4 layers in the real app theme.
        prefs->setEnabled("cloud",true);prefs->setVisible("cloud",true);
        candles.setSelection({},MarketCandles::IntervalMs);candles.setSelection(identity.key(),MarketCandles::IntervalMs);
        QJsonArray history;
        for(int i=199;i>0;--i){auto value=bar(start-i*MarketCandles::IntervalMs,QString::number(100+(i*7)%10),"2");
            if(i==10)value["low_price"]="50";if(i==9)value["high_price"]="150";history.append(value);}
        history.append(bar(start,"105","3"));response.body=QJsonDocument(history).toJson();emit http.finished(http.token,response);
        QVERIFY(candles.series().available);QTRY_VERIFY(workspace->chartStudies().pullback.available);QTRY_VERIFY(workspace->chartStudies().similarity.available);
        window.resize(1440,1040);QTest::qWait(30);QVERIFY(window.grab().save(QDir(preview).filePath("c4-studies-wide.png")));
        window.resize(820,1400);QTest::qWait(30);QVERIFY(window.grab().save(QDir(preview).filePath("c4-studies-narrow.png")));
    }
}

void LocalWindowTest::chartDrawingsPersistPublicCoordinatesInInjectedProfile(){
    QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-c8.ini"),QSettings::IniFormat);
    profile.setValue("unrelated/synthetic","preserve");Vault vault(dir.filePath("synthetic-c8-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);MarketCandles candles(nullptr);QByteArray saved;QString id;
    const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};constexpr qint64 start=1728000000000;
    {
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);
        auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);auto* drawings=workspace->drawingModel();QVERIFY(drawings);
        QVERIFY(drawings->setContext({identity,"KRW",true},60000));ChartDrawingFrame frame{identity,"KRW",60000,start+120000,drawings->contextGeneration(),{}};
        frame.bars.append({{start,0,"100","110","90","105","1"},start+60000,true,false});
        id=drawings->create(ChartDrawingKind::Horizontal,{{start,"100.125000"}},frame);QVERIFY(!id.isEmpty());saved=drawings->publicState();
        QTRY_COMPARE(profile.value("chart/drawings/v2").toByteArray(),saved);
        QVERIFY(!profile.contains("chart/drawings/v1"));
        QVERIFY(drawings->setContext({identity,"KRW",true},3600000));QCOMPARE(drawings->drawings().first().id,id);QCOMPARE(drawings->publicState(),saved);
        QVERIFY(drawings->setContext({identity,"KRW",false},60000));frame.contextGeneration=drawings->contextGeneration();
        QVERIFY(!drawings->create(ChartDrawingKind::Horizontal,{{start,"123.456"}},frame).isEmpty());QCOMPARE(drawings->publicState(),saved);
        workspace->clearPrivate();QVERIFY(drawings->drawings().isEmpty());QCOMPARE(drawings->publicState(),saved);QVERIFY(!vault.isUnlocked());
    }
    {
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);
        auto* drawings=child<ChartWorkspace>(window,"chartWorkspace")->drawingModel();QVERIFY(drawings);QVERIFY(drawings->setContext({identity,"KRW",true},60000));
        QCOMPARE(drawings->drawings().size(),1);QCOMPARE(drawings->drawings().first().id,id);QCOMPARE(drawings->drawings().first().anchors.first().price,QString("100.125000"));
        QCOMPARE(drawings->publicState(),saved);QCOMPARE(profile.value("unrelated/synthetic").toString(),QString("preserve"));
    }
}

void LocalWindowTest::chartDrawingsMigrateV1ToV2OnChangeAndKeepCorruptInRejected(){
    const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};constexpr qint64 start=1728000000000;
    const QString v1Id=QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QByteArray v1Bytes=QJsonDocument(QJsonObject{
        {"schema","keefi-chart-drawings/1"},
        {"magnet",false},
        {"contexts",QJsonArray{QJsonObject{
            {"identity",QJsonObject{{"market","coin"},{"source","upbit"},{"code","KRW-SYN"}}},
            {"currency","KRW"},
            {"bucket","intraday"},
            {"visible",true},
            {"locked",false},
            {"drawings",QJsonArray{QJsonObject{
                {"id",v1Id},
                {"kind","horizontal"},
                {"anchors",QJsonArray{QJsonObject{{"atMs",QString::number(start)},{"price","100.25"}}}},
                {"alert",QJsonObject{{"enabled",false},{"listId",""},{"intervalMs",0}}}
            }}}
        }}}
    }).toJson(QJsonDocument::Compact);

    // 1) Profile with v1 only: loads in memory as v2 defaults; first change writes v2 and leaves v1 intact.
    {
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("v1-only.ini"),QSettings::IniFormat);
        profile.setValue("chart/drawings/v1",v1Bytes);
        Vault vault(dir.filePath("v1-only-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);MarketCandles candles(nullptr);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);
        auto* drawings=child<ChartWorkspace>(window,"chartWorkspace")->drawingModel();QVERIFY(drawings);
        QVERIFY(drawings->setContext({identity,"KRW",true},60000));
        QCOMPARE(drawings->drawings().size(),1);QCOMPARE(drawings->drawings().first().id,v1Id);
        QCOMPARE(drawings->drawings().first().visible,true);QCOMPARE(drawings->drawings().first().locked,false);
        QVERIFY(drawings->drawings().first().autoBatchId.isEmpty());
        QVERIFY(!profile.contains("chart/drawings/v2"));
        QCOMPARE(profile.value("chart/drawings/v1").toByteArray(),v1Bytes);

        QVERIFY(drawings->setDrawingLocked(v1Id,true));
        QTRY_COMPARE(profile.value("chart/drawings/v2").toByteArray(),drawings->publicState());
        QCOMPARE(QJsonDocument::fromJson(profile.value("chart/drawings/v2").toByteArray()).object()["schema"].toString(),
                 QString("keefi-chart-drawings/2"));
        QCOMPARE(profile.value("chart/drawings/v1").toByteArray(),v1Bytes);
    }

    // 2) Profile with corrupt v2 and valid v1: rejects v2 into chart/drawings/rejected, sets issue, starts empty without falling back to v1.
    {
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("corrupt-v2.ini"),QSettings::IniFormat);
        const QByteArray corruptV2="{\"schema\":\"keefi-chart-drawings/2\",\"contexts\":[{\"broken\":true}],\"magnet\":false}";
        profile.setValue("chart/drawings/v1",v1Bytes);
        profile.setValue("chart/drawings/v2",corruptV2);
        Vault vault(dir.filePath("corrupt-v2-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);MarketCandles candles(nullptr);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);
        auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
        auto* drawings=workspace->drawingModel();QVERIFY(drawings);
        QCOMPARE(profile.value("chart/drawings/rejected").toByteArray(),corruptV2);
        QVERIFY(!workspace->property("drawingStorageIssue").toString().isEmpty());
        QVERIFY(drawings->setContext({identity,"KRW",true},60000));
        QVERIFY(drawings->drawings().isEmpty());
        QCOMPARE(profile.value("chart/drawings/v1").toByteArray(),v1Bytes);
    }
}

void LocalWindowTest::chartStudyPreferencesPersistOnlyPublicDisplayChoices(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-c4-display.ini"),QSettings::IniFormat);
    profile.setValue("unrelated/synthetic","preserve");
    Vault vault(dir.filePath("synthetic-c4-locked"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);MarketCandles candles(nullptr);
    const QList<ExtendedMaSpec> wanted{{"ma1",7,ExtendedMaKind::Simple},{"ma2",34,ExtendedMaKind::Exponential},{"ma3",120,ExtendedMaKind::Weighted}};
    const QStringList kinds{"sma","ema","wma"};QByteArray stored;QStringList storedOrder;
    {
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);
        auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
        auto* prefs=workspace->indicatorPreferences();QVERIFY(prefs);
        // Dev86 enables all three core studies for a new profile; saved visibility remains independent.
        QVERIFY(prefs->enabled("similarity"));QVERIFY(prefs->visible("similarity"));
        QVERIFY(prefs->setVisible("similarity",false));QVERIFY(prefs->setEnabled("cloud",false));prefs->markIntroSeen();
        QVERIFY(prefs->setMovingAverages(wanted));QVERIFY(prefs->setEnabled("ma",true));
        QVERIFY(prefs->setEnabled("rsi",true));QVERIFY(prefs->setEnabled("macd",true));
        QCOMPARE(prefs->visiblePanes("coin"),QStringList({"rsi","macd"}));QVERIFY(prefs->enabled("rsi"));QVERIFY(prefs->visible("rsi"));
        storedOrder=prefs->enabledOrder("coin");
        // The profile key stays compatible; the public payload has schema 5.
        QTRY_COMPARE(profile.value("chart/studies/v1").toByteArray(),prefs->publicState());
        stored=profile.value("chart/studies/v1").toByteArray();QVERIFY(!stored.isEmpty());
        QJsonParseError parseError;const auto document=QJsonDocument::fromJson(stored,&parseError);
        QCOMPARE(parseError.error,QJsonParseError::NoError);QVERIFY(document.isObject());const auto object=document.object();
        auto keys=object.keys();keys.sort();QCOMPARE(keys,QStringList({"averages","format","introSeen","order","studies"}));
        QVERIFY(object.value("format").isString());QCOMPARE(object.value("format").toString(),QString("keefi-chart-studies/5"));
        QVERIFY(object.value("studies").isObject());QVERIFY(object.value("introSeen").isBool());QVERIFY(object.value("introSeen").toBool());
        QVERIFY(object.value("order").isArray());
        QVERIFY(object.value("averages").isArray());
        const auto studies=object.value("studies").toObject();auto studyKeys=studies.keys();studyKeys.sort();
        auto supported=prefs->supportedIds("coin")+prefs->supportedIds("stock");supported.removeDuplicates();supported.sort();QCOMPARE(studyKeys,supported);
        for(auto it=studies.constBegin();it!=studies.constEnd();++it){
            QVERIFY(it.value().isObject());const auto flags=it.value().toObject();auto flagKeys=flags.keys();flagKeys.sort();
            QCOMPARE(flagKeys,QStringList({"enabled","visible"}));QVERIFY(flags.value("enabled").isBool());QVERIFY(flags.value("visible").isBool());
            QCOMPARE(flags.value("enabled").toBool(),prefs->enabled(it.key()));QCOMPARE(flags.value("visible").toBool(),prefs->visible(it.key()));
        }
        QStringList order;for(const auto& value:object.value("order").toArray()){
            QVERIFY(value.isString());QVERIFY(studies.contains(value.toString()));QVERIFY(!order.contains(value.toString()));order.append(value.toString());
        }
        auto orderedKeys=order;orderedKeys.sort();QCOMPARE(orderedKeys,studyKeys);
        const auto averages=object.value("averages").toArray();QCOMPARE(averages.size(),wanted.size());
        for(int i=0;i<wanted.size();++i){
            QVERIFY(averages[i].isObject());const auto average=averages[i].toObject();auto averageKeys=average.keys();averageKeys.sort();
            QCOMPARE(averageKeys,QStringList({"enabled","kind","period"}));QVERIFY(average.value("enabled").isBool());QVERIFY(average.value("kind").isString());QVERIFY(average.value("period").isDouble());
            QCOMPARE(average.value("enabled").toBool(),wanted[i].enabled);QCOMPARE(average.value("kind").toString(),kinds[i]);QCOMPARE(average.value("period").toDouble(),double(wanted[i].period));
        }
        QVERIFY(!vault.isUnlocked());QCOMPARE(profile.value("unrelated/synthetic").toString(),QString("preserve"));
    }
    {
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setMarketCandles(&candles);
        auto* prefs=child<ChartWorkspace>(window,"chartWorkspace")->indicatorPreferences();QVERIFY(prefs);
        QVERIFY(prefs->enabled("similarity"));QVERIFY(!prefs->visible("similarity"));
        QVERIFY(!prefs->enabled("cloud"));QVERIFY(prefs->visible("cloud"));QVERIFY(prefs->introSeen());
        QVERIFY(prefs->enabled("ma"));QVERIFY(prefs->visible("ma"));QCOMPARE(prefs->movingAverages().size(),wanted.size());
        for(int i=0;i<wanted.size();++i){const auto average=prefs->movingAverages()[i];QCOMPARE(average.id,wanted[i].id);QCOMPARE(average.period,wanted[i].period);QCOMPARE(average.kind,wanted[i].kind);QCOMPARE(average.enabled,wanted[i].enabled);}
        QCOMPARE(prefs->visiblePanes("coin"),QStringList({"rsi","macd"}));QVERIFY(prefs->enabled("macd"));QVERIFY(prefs->visible("macd"));
        QVERIFY(prefs->enabled("rsi"));QVERIFY(prefs->visible("rsi"));QVERIFY(!prefs->enabled("bb"));
        QCOMPARE(prefs->enabledOrder("coin"),storedOrder);QCOMPARE(prefs->publicState(),stored);
        QCOMPARE(profile.value("unrelated/synthetic").toString(),QString("preserve"));QVERIFY(!vault.isUnlocked());
    }
}

void LocalWindowTest::lockedPublicBinanceSelectionLoadsMetadataAndSelectedTimeframe(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-public-c3.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-c3"));AccountModel accounts(&vault);
    class PublicTransport final:public TradingTransport {
      public:
        QList<RequestSpec> requests;quint64 lastToken=0;
        void send(quint64 token,const RequestSpec& r)override{
            QVERIFY(!r.authenticated);QCOMPARE(r.method,QByteArray("GET"));QVERIFY(r.body.isEmpty());
            QVERIFY(!r.headers.contains("Authorization"));QVERIFY(!r.headers.contains("X-MBX-APIKEY"));
            requests.append(r);lastToken=token;
        }
        void abort()override{}
    } metadata,chart;
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles model(&chart);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&model);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    window.resize(1280,800);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    workspace->setIdentity("coin:binance:SYNUSDT");workspace->setTimeframe(300000);
    QTRY_COMPARE_WITH_TIMEOUT(metadata.requests.size(),1,2500);
    QCOMPARE(metadata.requests.last().url.path(),QString("/api/v3/exchangeInfo"));
    HttpResult instruments;instruments.status=200;
    instruments.body=R"({"symbols":[{"symbol":"SYNUSDT","baseAsset":"SYN","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})";
    emit metadata.finished(metadata.lastToken,instruments);
    QTRY_VERIFY_WITH_TIMEOUT(!chart.requests.isEmpty()&&chart.requests.last().url.host()=="api.binance.com",2500);
    QCOMPARE(chart.requests.last().url.path(),QString("/api/v3/klines"));
    QCOMPARE(QUrlQuery(chart.requests.last().url).queryItemValue("interval"),QString("5m"));
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto at=(now/300000-1)*300000;
    HttpResult candle;candle.status=200;
    QJsonArray tupleRows;tupleRows.append(QJsonArray{QString::number(at),"100","110","90","101","2",QString::number(at+299999),"200","1","1","100","0"});
    candle.body=QJsonDocument(tupleRows).toJson();
    emit chart.finished(chart.lastToken,candle);
    QTRY_VERIFY(model.series().available);QCOMPARE(model.series().identity,QString("coin:binance:SYNUSDT"));
    QCOMPARE(model.series().asset,QString("SYN"));QCOMPARE(model.series().currency,QString("USDT"));
    QCOMPARE(model.series().intervalMs,qint64(300000));QCOMPARE(model.series().sourceTimeMs,qint64(0));
    const auto row=workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"});
    QCOMPARE(row.price,QString("101"));QCOMPARE(row.completedAtMs,at+300000);
    QVERIFY(workspace->watchlistModel()->rowState({"coin","upbit","KRW-SYN"}).price.isEmpty());
    QVERIFY(!vault.isUnlocked());QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
}

void LocalWindowTest::chartWatchlistsLoadPublicSpotMetadataWithoutNewAccountReads_data() {
    QTest::addColumn<QString>("provider"); QTest::addColumn<QString>("identityKey");
    QTest::newRow("binance-public-pair") << QString("binance") << QString("coin:binance:BTCUSDT");
    QTest::newRow("bithumb-public-pair") << QString("bithumb") << QString("coin:bithumb:KRW-BTC");
}
void LocalWindowTest::chartWatchlistsLoadPublicSpotMetadataWithoutNewAccountReads() {
    QFETCH(QString, provider); QFETCH(QString, identityKey);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-spot-metadata.ini"), QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-spot-metadata-vault"));
    QVERIFY(vault.create("FAKE C2 public metadata password 2026!"));
    keefi::test::ReadFixture fixture(provider); fixture.account.alias = "FAKE_PRIVATE_C2_METADATA_ALIAS";
    QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
    class MetadataTransport final : public TradingTransport {
      public:
        QString provider; int markets = 0, accountReads = 0; quint64 pending = 0, epoch = 0; HttpResult catalog;
        void abort() override { ++epoch; }
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation)); QVERIFY(NetworkTradingTransport::allowed(request));
            if (request.operation == Operation::Markets) {
                ++markets; QVERIFY(!request.authenticated); QCOMPARE(request.method, QByteArray("GET"));
                QVERIFY(request.body.isEmpty()); QVERIFY(!request.headers.contains("Authorization")); QVERIFY(!request.headers.contains("X-MBX-APIKEY"));
                QCOMPARE(request.url.path(), provider == "binance" ? QString("/api/v3/exchangeInfo") : QString("/v1/market/all"));
                pending = token;
                if (provider == "bithumb") catalog = keefi::test::bithumbResponse(request);
                else { catalog.status = 200; catalog.body = R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})"; }
                return;
            }
            if (request.operation == Operation::Account) ++accountReads;
            HttpResult response;
            if (provider == "bithumb") response = keefi::test::bithumbResponse(request);
            else response = keefi::test::usdmJson(request.operation == Operation::Time ? QJsonObject{{"serverTime", QString::number(QDateTime::currentMSecsSinceEpoch())}} :
                QJsonObject{{"canTrade", true}, {"accountType", "SPOT"}, {"balances", QJsonArray{QJsonObject{{"asset", "BTC"}, {"free", "0.125"}, {"locked", "0.025"}}}}});
            const auto generation = epoch;
            QTimer::singleShot(0, this, [this, generation, token, response] { if (generation == epoch) emit finished(token, response); });
        }
        void releaseCatalog() { const auto token = pending; pending = 0; emit finished(token, catalog); }
    } transport;
    transport.provider = provider;
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) { return &transport; };
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories); AccountModel accounts(&vault);
    LocalWindow window(vault, accounts); window.setChartSettings(&profile);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true); portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QString("complete"), 5000);
    auto* controller = portfolio.trading(vault.accounts().first().id); QVERIFY(controller);
    QVERIFY(controller->instruments().isEmpty());
    const auto before = portfolio.snapshots(); QVERIFY(!before.isEmpty());
    const auto accountReads = transport.accountReads;
    const bool connectedBefore = controller->connected();
    NoNetworkBrokerTransport chartTransport; MarketCandles candles(&chartTransport);
    window.setChartInstrumentTransport(&transport);
    window.setMarketCandles(&candles); window.resize(1200, 800); window.show();
    auto* catalog = child<ChartInstrumentCatalog>(window, "chartInstrumentCatalog"); QVERIFY(catalog);
    QVERIFY(catalog->instruments(provider).isEmpty());
    if(provider=="bithumb"){
        // Dev91 keeps custody visible but offers only implemented chart history.
        // No unsupported Bithumb chart metadata or extra account read is started.
        auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
        auto* rows=child<QTreeWidget>(*workspace,"chartHoldingAssets");QVERIFY(rows);
        QTRY_COMPARE(rows->topLevelItemCount(),1);QCOMPARE(transport.markets,0);
        QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
        QVERIFY(rows->topLevelItem(0)->text(0).contains(QStringLiteral("빗썸")));
        QCOMPARE(rows->topLevelItem(0)->data(0,Qt::UserRole).toString(),QString("exchange:BTC"));
        QVERIFY(rows->topLevelItem(0)->toolTip(0).contains(QStringLiteral("미지원"))||rows->topLevelItem(0)->text(0).contains(QStringLiteral("차트 지원 없음")));
        QCOMPARE(transport.accountReads,accountReads);QVERIFY(portfolio.snapshots()==before);
        QCOMPARE(controller->connected(),connectedBefore);vault.lock();QCOMPARE(rows->topLevelItemCount(),0);return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(transport.markets, 1, 2500);
    QVERIFY(transport.pending != 0); QCOMPARE(transport.accountReads, accountReads);
    QVERIFY(portfolio.snapshots() == before); QCOMPARE(controller->connected(), connectedBefore);
    QCOMPARE(portfolio.connections().first().state, QString("complete"));
    QVERIFY(controller->instruments().isEmpty());
    transport.releaseCatalog();
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace"); QVERIFY(workspace);
    auto* lists = workspace->watchlistModel(); QVERIFY(lists);
    const auto identity = WatchlistWorkspaceList::identityFromKey(identityKey);
    QTRY_VERIFY_WITH_TIMEOUT(!catalog->busy(), 2500);
    QVERIFY2(!catalog->instruments(provider).isEmpty(), qPrintable(catalog->status(provider)));
    QVERIFY(controller->instruments().isEmpty());
    QCOMPARE(controller->connected(), connectedBefore);
    QCOMPARE(portfolio.connections().first().state, QString("complete"));
    QVERIFY(portfolio.snapshots() == before);
    qInfo().nospace() << "public_metadata provider=" << provider
        << " source_count=" << catalog->instruments(provider).size()
        << " identity_verified=" << lists->isCurrentlyVerified(identity)
        << " holdings_count=" << lists->holdings().size();
    QTRY_VERIFY_WITH_TIMEOUT(lists->holdings().contains(identity), 2500);
    QVERIFY(controller->instruments().isEmpty()); QVERIFY(portfolio.snapshots() == before);
    emit portfolio.changed(); QTest::qWait(1100);
    QCOMPARE(transport.markets, 1); QCOMPARE(transport.accountReads, accountReads);
    QCOMPARE(controller->connected(), connectedBefore);
    QCOMPARE(portfolio.connections().first().state, QString("complete"));
    QVERIFY(portfolio.snapshots() == before);
    QVERIFY(!lists->publicState().contains("FAKE_PRIVATE"));
    vault.lock(); QTRY_VERIFY(lists->holdings().isEmpty());
}

void LocalWindowTest::chartWatchlistsPublicMetadataFailurePreservesPrivateCapture_data() {
    QTest::addColumn<int>("publicStatus");
    QTest::addColumn<QByteArray>("publicBody");
    QTest::newRow("public-rate-limit") << 429 << QByteArray(R"({"code":-1003,"msg":"FAKE_PUBLIC_CATALOG_FAILURE"})");
    QTest::newRow("public-malformed-catalog") << 200 << QByteArray(R"({"symbols":"FAKE_PUBLIC_CATALOG_FAILURE"})");
}
void LocalWindowTest::chartWatchlistsPublicMetadataFailurePreservesPrivateCapture() {
    QFETCH(int, publicStatus); QFETCH(QByteArray, publicBody);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-metadata-failure.ini"), QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-metadata-failure-vault"));
    QVERIFY(vault.create("FAKE C2 metadata failure password 2026!"));
    keefi::test::ReadFixture fixture("binance"); fixture.account.alias = "FAKE_PRIVATE_C2_METADATA_FAILURE_ALIAS";
    QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
    class MetadataFailureTransport final : public TradingTransport {
      public:
        int markets = 0, accountReads = 0; quint64 pending = 0, epoch = 0;
        void abort() override { ++epoch; }
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation)); QVERIFY(NetworkTradingTransport::allowed(request));
            if (request.operation == Operation::Markets) {
                ++markets; QVERIFY(!request.authenticated); QCOMPARE(request.method, QByteArray("GET"));
                QVERIFY(request.body.isEmpty()); QVERIFY(!request.headers.contains("Authorization"));
                QVERIFY(!request.headers.contains("X-MBX-APIKEY"));
                QCOMPARE(request.url.path(), QString("/api/v3/exchangeInfo"));
                pending = token; return;
            }
            if (request.operation == Operation::Account) ++accountReads;
            const auto response = keefi::test::usdmJson(request.operation == Operation::Time ?
                QJsonObject{{"serverTime", QString::number(QDateTime::currentMSecsSinceEpoch())}} :
                QJsonObject{{"canTrade", true}, {"accountType", "SPOT"}, {"balances", QJsonArray{
                    QJsonObject{{"asset", "BTC"}, {"free", "0.125"}, {"locked", "0.025"}}}}});
            const auto generation = epoch;
            QTimer::singleShot(0, this, [this, generation, token, response] { if (generation == epoch) emit finished(token, response); });
        }
        void releaseFailure(int status, const QByteArray& body) {
            const auto token = pending; pending = 0;
            HttpResult response; response.status = status; response.body = body;
            if (status == 429) response.headers.insert("Retry-After", "60");
            emit finished(token, response);
        }
    } transport;
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) { return &transport; };
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories); AccountModel accounts(&vault);
    LocalWindow window(vault, accounts); window.setChartSettings(&profile);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true); portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QString("complete"), 5000);
    auto* controller = portfolio.trading(vault.accounts().first().id); QVERIFY(controller);
    QVERIFY(controller->instruments().isEmpty());
    const auto before = portfolio.snapshots(); QVERIFY(!before.isEmpty());
    const auto accountReads = transport.accountReads;
    const bool connectedBefore = controller->connected();
    NoNetworkBrokerTransport chartTransport; MarketCandles candles(&chartTransport);
    window.setChartInstrumentTransport(&transport);
    window.setMarketCandles(&candles); window.resize(1200, 800); window.show();
    auto* catalog = child<ChartInstrumentCatalog>(window, "chartInstrumentCatalog"); QVERIFY(catalog);
    QVERIFY(catalog->instruments("binance").isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(transport.markets, 1, 2500); QVERIFY(transport.pending != 0);
    QVERIFY(portfolio.snapshots() == before); QCOMPARE(transport.accountReads, accountReads);
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace"); QVERIFY(workspace);
    // This assertion inspects chart UI. C7 defers hidden chart rendering.
    auto* chartPages=child<QTabWidget>(window,"portfolioPages");QVERIFY(chartPages);chartPages->setCurrentWidget(workspace);
    auto* lists = workspace->watchlistModel(); QVERIFY(lists);
    auto* panel=test::openInstrumentPanel(*workspace);QVERIFY(panel);
    auto* holdingsScope = child<QPushButton>(*panel, "chartBrowserScope_holdings"); QVERIFY(holdingsScope);
    holdingsScope->click(); QVERIFY(holdingsScope->isChecked());
    transport.releaseFailure(publicStatus, publicBody);
    QTRY_VERIFY_WITH_TIMEOUT(!catalog->busy(), 2000);
    qInfo().nospace() << "public_metadata_failure status=" << publicStatus
        << " connected_before=" << connectedBefore << " connected_after=" << controller->connected();
    QVERIFY(portfolio.snapshots() == before); QCOMPARE(transport.accountReads, accountReads);
    QCOMPARE(controller->connected(), connectedBefore);
    QCOMPARE(portfolio.connections().first().state, QString("complete"));
    QVERIFY(controller->instruments().isEmpty()); QVERIFY(catalog->instruments("binance").isEmpty());
    QVERIFY(!catalog->status("binance").isEmpty());
    QVERIFY(lists->holdings().isEmpty());
    auto* status = child<QLabel>(*workspace, "chartWorkspaceListStatus"); QVERIFY(status);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->holdingsUnavailableReason().contains(QStringLiteral("공개 종목 식별 자료")), 2000);
    const auto coverageReason=workspace->holdingsUnavailableReason();
    auto* information=child<InfoButton>(*workspace,"chartHoldingInformation");QVERIFY(information);
    QVERIFY(information->isVisibleTo(panel));QVERIFY(information->isEnabled());
    QTest::mouseClick(information,Qt::LeftButton);QTRY_VERIFY(information->informationOpen());
    const QPointer<QPlainTextEdit> coverageText=information->findChild<QPlainTextEdit*>("infoPrivateText");
    QVERIFY(coverageText);QVERIFY(coverageText->isVisible());
    QVERIFY(coverageText->toPlainText().contains(coverageReason));
    QVERIFY(!status->text().contains("FAKE_PRIVATE"));QVERIFY(!status->text().contains("FAKE_PUBLIC_CATALOG_FAILURE"));
    QVERIFY(!coverageText->toPlainText().contains("FAKE_PRIVATE"));
    QVERIFY(!coverageText->toPlainText().contains("FAKE_PUBLIC_CATALOG_FAILURE"));
    auto* holdingsRows=child<QTreeWidget>(*workspace,"chartWorkspaceWatchlist");QVERIFY(holdingsRows);QCOMPARE(holdingsRows->topLevelItemCount(),0);
    QVERIFY(workspace->holdingsUnavailableReason().contains(QStringLiteral("제외")));
    QVERIFY(!lists->publicState().contains("FAKE_PRIVATE"));
    QVERIFY(!lists->publicState().contains("FAKE_PUBLIC_CATALOG_FAILURE"));
    emit portfolio.changed(); QTest::qWait(1100);
    QCOMPARE(transport.markets, 1); QCOMPARE(transport.accountReads, accountReads);
    QVERIFY(portfolio.snapshots() == before);
    QCOMPARE(controller->connected(), connectedBefore);
    QCOMPARE(portfolio.connections().first().state, QString("complete"));
    vault.lock(); QTRY_VERIFY(lists->holdings().isEmpty());
    QVERIFY(workspace->holdingsUnavailableReason().isEmpty());QVERIFY(!status->text().contains(coverageReason));
    QVERIFY(!information->informationOpen());
    QVERIFY(!coverageText||coverageText->toPlainText().isEmpty());
    QVERIFY(!information->isVisibleTo(panel));QVERIFY(!information->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("금고를 열면")));QCOMPARE(holdingsRows->topLevelItemCount(),0);
}

void LocalWindowTest::chartWatchlistKeepsCurrentTickerPriceWhenCompletedBadgesArrive() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-ticker-badges.ini"), QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-ticker-badges-vault")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    PublicMarketTransport publicTransport; PublicMarketCatalog markets(&publicTransport);
    class CandleTransport final : public TradingTransport { public: quint64 token = 0;
        void send(quint64 value, const RequestSpec& request) override { QVERIFY(!request.authenticated); token = value; }
        void abort() override {} } candleTransport;
    MarketCandles candles(&candleTransport); LocalWindow window(vault, accounts);
    window.setChartSettings(&profile); window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&markets, false); window.setMarketCandles(&candles);
    auto* pages = child<QTabWidget>(window, "portfolioPages"); QVERIFY(pages);
    for (int i = 0; i < pages->count(); ++i) if (pages->tabText(i) == QStringLiteral("차트")) pages->setCurrentIndex(i);
    window.resize(1200, 800); window.show(); markets.refresh();
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace"); QVERIFY(workspace);
    auto* lists = workspace->watchlistModel(); QVERIFY(lists);
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(identity).price, QString("95000000"), 3000);
    QTRY_VERIFY(candleTransport.token != 0);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto at = (now / MarketCandles::IntervalMs - 1) * MarketCandles::IntervalMs;
    const QJsonObject candle{{"market", "KRW-BTC"}, {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp", QString::number(at + 1000)}, {"unit", "15"}, {"opening_price", "100"}, {"high_price", "110"},
        {"low_price", "90"}, {"trade_price", "101"}, {"candle_acc_trade_volume", "2"}};
    HttpResult response; response.status = 200; response.body = QJsonDocument(QJsonArray{candle}).toJson();
    emit candleTransport.finished(candleTransport.token, response);
    QTRY_VERIFY(lists->rowState(identity).completedAtMs > 0);
    QCOMPARE(lists->rowState(identity).price, QString("95000000"));
    QVERIFY(lists->rowState(identity).evidence.contains("/v1/ticker"));
    QCOMPARE(lists->rowState(identity).completedAtMs, at + MarketCandles::IntervalMs);
    QVERIFY(!lists->rowState(identity).badges.contains(QStringLiteral("완료 봉")));QCOMPARE(candles.series().candles.last().startsAtMs+MarketCandles::IntervalMs,lists->rowState(identity).completedAtMs);
    QVERIFY(markets.presentationActive()); // Chart tab still consumes the shared public ticker cache.
    QVERIFY(!markets.rows().isEmpty());const auto receipt=markets.rows().first().receivedAtMs;
    window.hide();QVERIFY(!markets.presentationActive());
    QCOMPARE(markets.rows().first().receivedAtMs,receipt);QCOMPARE(lists->rowState(identity).price,QString("95000000"));
    window.show();QTRY_VERIFY(markets.presentationActive());
}

void LocalWindowTest::chartWatchlistDropsCandleDerivedRowsWhenSourceBecomesUnavailable() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-candle-row-expiry.ini"), QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-candle-row-expiry-vault")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    class CandleTransport final : public TradingTransport { public: QList<quint64> tokens;
        void send(quint64 value, const RequestSpec& request) override { QVERIFY(!request.authenticated); tokens.append(value); }
        void abort() override {} } transport;
    MarketCandles candles(&transport);
    PublicMarketTransport publicHttp;publicHttp.quoteStatus=503;PublicMarketCatalog markets(&publicHttp);NoNetworkBrokerTransport metadata;
    LocalWindow window(vault, accounts);
    window.setChartSettings(&profile); window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window.setMarketCandles(&candles);
    auto* pages = child<QTabWidget>(window, "portfolioPages"); QVERIFY(pages);
    for (int i = 0; i < pages->count(); ++i) if (pages->tabText(i) == QStringLiteral("차트")) pages->setCurrentIndex(i);
    window.resize(1200, 800); window.show(); QTRY_VERIFY(!transport.tokens.isEmpty());
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace"); QVERIFY(workspace);
    auto* lists = workspace->watchlistModel(); QVERIFY(lists);
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto at = (now / MarketCandles::IntervalMs - 1) * MarketCandles::IntervalMs;
    const QJsonObject candle{{"market", "KRW-BTC"}, {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp", QString::number(at + 1000)}, {"unit", "15"}, {"opening_price", "100"}, {"high_price", "110"},
        {"low_price", "90"}, {"trade_price", "101"}, {"candle_acc_trade_volume", "2"}};
    HttpResult response; response.status = 200; response.body = QJsonDocument(QJsonArray{candle}).toJson();
    const auto oldToken = transport.tokens.last(); emit transport.finished(oldToken, response);
    QTRY_COMPARE(lists->rowState(identity).price, QString("101"));
    QVERIFY(lists->rowState(identity).completedAtMs > 0);
    // A dropped series uses the same unavailable branch as stale/failed data.
    // No wall-clock change or ninety-second sleep is needed for this contract.
    candles.setAsset({});
    QTRY_VERIFY_WITH_TIMEOUT(lists->rowState(identity).price.isEmpty(), 2500);
    QVERIFY(lists->rowState(identity).badges.isEmpty()); QCOMPARE(lists->rowState(identity).completedAtMs, 0);
    candles.setAsset("BTC"); QTRY_VERIFY_WITH_TIMEOUT(transport.tokens.size() >= 2, 2500);
    HttpResult failure; failure.status = 503; emit transport.finished(transport.tokens.last(), failure);
    emit transport.finished(oldToken, response); QCoreApplication::processEvents();
    QVERIFY(lists->rowState(identity).price.isEmpty()); QVERIFY(lists->rowState(identity).badges.isEmpty());
}

void LocalWindowTest::chartWatchlistsDeriveHoldingsFromReadyUpbitAndCannotRehydrateAfterLock() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-derived-holdings.ini"), QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-derived-holdings-vault"));
    QVERIFY(vault.create("FAKE C2 source holdings password 2026!"));
    keefi::test::ReadFixture fixture("upbit"); fixture.account.alias = "FAKE_PRIVATE_C2_UPBIT_ALIAS";
    QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
    keefi::test::ProviderReadTransport accountTransport; accountTransport.provider = "upbit";
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) { return &accountTransport; };
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories); AccountModel accounts(&vault);
    PublicMarketTransport publicTransport; PublicMarketCatalog markets(&publicTransport);
    NoNetworkBrokerTransport chartTransport; MarketCandles candles(&chartTransport);
    LocalWindow window(vault, accounts);
    window.setChartSettings(&profile); window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&markets, false); window.setMarketCandles(&candles);
    window.resize(1200, 800); window.show(); markets.refresh(); portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QString("complete"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!markets.rows().isEmpty(), 3000);
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace"); QVERIFY(workspace);
    auto* lists = workspace->watchlistModel(); QVERIFY(lists);
    const WatchlistIdentity held{"coin", "upbit", "KRW-BTC"};
    QTRY_VERIFY_WITH_TIMEOUT(lists->holdings().contains(held), 2500);
    const auto publicList = lists->createList("합성 공개 BTC", "green");
    QVERIFY(lists->addIdentity(publicList, held));
    QCoreApplication::processEvents();
    const auto publicState = lists->publicState();
    QTRY_COMPARE(profile.value("chart/watchlists/v2").toByteArray(), publicState);
    lists->setRowState(held, {"FAKE_HOLDINGS_DERIVED_PRICE", {}, fixture.account.alias, {"FAKE_PRIVATE_C2_BADGE"}, 1000, true});
    QCOMPARE(lists->publicState(), publicState);
    QVERIFY(!publicState.contains("FAKE_PRIVATE")); QVERIFY(!publicState.contains("FakeLifecycle"));
    bool observedAboutToLock = false;
    connect(&vault, &Vault::aboutToLock, &window, [&] {
        observedAboutToLock = true;
        QVERIFY(!vault.isUnlocked());
        emit portfolio.changed();
        QVERIFY(lists->holdings().isEmpty());
        QVERIFY(!lists->rowState(held).privateData);
        QVERIFY(!lists->rowState(held).price.contains("FAKE_HOLDINGS_DERIVED"));
        QVERIFY(!lists->rowState(held).evidence.contains("FAKE_PRIVATE"));
    });
    vault.lock();
    QVERIFY(observedAboutToLock);
    QTRY_VERIFY(lists->holdings().isEmpty());
    QVERIFY(!lists->rowState(held).privateData);
    emit portfolio.changed(); QTest::qWait(1100);
    // A fresh public ticker may refill this public user-list row after lock;
    // only holdings-derived markers and private provenance must stay erased.
    QVERIFY(lists->holdings().isEmpty()); QVERIFY(!lists->rowState(held).privateData);
    QVERIFY(!lists->rowState(held).price.contains("FAKE_HOLDINGS_DERIVED"));
    QVERIFY(!lists->rowState(held).evidence.contains("FAKE_PRIVATE"));
    QVERIFY(!lists->rowState(held).badges.join(' ').contains("FAKE_PRIVATE"));
    QCOMPARE(lists->publicState(), publicState); QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(), publicState);
    QCOMPARE(lists->userList(publicList).entries.first().identity, held);
}

void LocalWindowTest::chartWatchlistsUseVerifiedBrokerCountriesForMixedHoldingsAndLegacy() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-broker-watchlists.ini"), QSettings::IniFormat);
    profile.setValue("explore/watchlist/kr", QStringList{"005930"});
    profile.setValue("explore/watchlist/us", QStringList{"NVDA"});
    Vault vault(directory.filePath("synthetic-broker-watchlists-vault"));
    QVERIFY(vault.create("FAKE C2 broker metadata password 2026!"));
    keefi::test::ReadFixture fixture("toss"); fixture.account.alias = "FAKE_PRIVATE_C2_TOSS_ALIAS";
    QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
    keefi::test::ProviderReadTransport transport; transport.provider = "toss";
    transport.transform = [](const RequestSpec& request, HttpResult& response) {
        if (request.operation != Operation::Holdings) return;
        auto envelope = QJsonDocument::fromJson(response.body).object(); auto result = envelope["result"].toObject();
        auto items = result["items"].toArray(); auto us = items.first().toObject();
        us["symbol"] = "NVDA"; us["name"] = "Synthetic US stock"; us["marketCountry"] = "US";
        us["currency"] = "USD"; us["quantity"] = "2"; items.append(us);
        result["items"] = items; envelope["result"] = result; response.body = QJsonDocument(envelope).toJson();
    };
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) { return &transport; };
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories); AccountModel accounts(&vault);
    NoNetworkBrokerTransport chartTransport; MarketCandles candles(&chartTransport);
    LocalWindow window(vault, accounts); window.setChartSettings(&profile);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true); window.setMarketCandles(&candles);
    window.resize(1200, 800); window.show(); portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QString("complete"), 5000);
    auto* broker = portfolio.broker(vault.accounts().first().id); QVERIFY(broker);
    QCOMPARE(broker->marketDataCountry("005930"), QString("KR"));
    QCOMPARE(broker->marketDataCountry("NVDA"), QString("US"));
    auto* workspace = child<ChartWorkspace>(window, "chartWorkspace"); QVERIFY(workspace);
    auto* lists = workspace->watchlistModel(); QVERIFY(lists);
    const WatchlistIdentity kr{"stock", "KR", "005930"}, us{"stock", "US", "NVDA"};
    QTRY_VERIFY_WITH_TIMEOUT(lists->isCurrentlyVerified(kr) && lists->isCurrentlyVerified(us), 2500);
    QVERIFY(lists->holdings().contains(kr)); QVERIFY(lists->holdings().contains(us));
    QStringList migrated;
    for (const auto& list : lists->userLists()) for (const auto& entry : list.entries) migrated.append(entry.identity.key());
    QVERIFY(migrated.contains(kr.key())); QVERIFY(migrated.contains(us.key()));
    QCOMPARE(profile.value("explore/watchlist/kr").toStringList(), QStringList{"005930"});
    QCOMPARE(profile.value("explore/watchlist/us").toStringList(), QStringList{"NVDA"});
    const auto saved = lists->publicState(); QVERIFY(!saved.contains("FAKE_PRIVATE")); QVERIFY(!saved.contains("FakeLifecycle"));
    vault.lock(); emit portfolio.changed(); QTest::qWait(1100);
    QVERIFY(lists->holdings().isEmpty()); QCOMPARE(lists->publicState(), saved);
    QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(), saved);
}

void LocalWindowTest::chartWatchlistsPersistOnlyPublicStateInInjectedProfile() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-chart-profile.ini"), QSettings::IniFormat);
    Vault vault(directory.filePath("synthetic-locked-chart-vault"));
    AccountModel accounts(&vault);
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    NoNetworkBrokerTransport transport;
    MarketCandles candles(&transport);
    QByteArray saved;
    QString listId;
    {
        LocalWindow window(vault, accounts);
        window.setChartSettings(&profile);
        window.setPortfolioController(&portfolio);
        window.setMarketCandles(&candles);
        auto* workspace = child<ChartWorkspace>(window, "chartWorkspace");
        QVERIFY(workspace);
        auto* lists = workspace->watchlistModel();
        QVERIFY(lists);
        listId = lists->createList("합성 혼합 공개 리스트", "purple");
        QVERIFY(!listId.isEmpty());
        const auto section = lists->addSection(listId, "합성 코인과 주식");
        QVERIFY(!section.isEmpty());
        const WatchlistIdentity coin{"coin", "upbit", "KRW-BTC"}, stock{"stock", "US", "NVDA"};
        // Synthetic public metadata registry, without a broker connection or key.
        lists->setVerifiedInstruments({stock});
        QVERIFY(lists->addIdentity(listId, coin, section));
        QVERIFY(lists->addIdentity(listId, stock, section));
        lists->setHoldings({{"coin", "binance", "ETHUSDT"}});
        lists->setRowState(coin, {"123456.789", "+0.123", "FAKE_PRIVATE_ACCOUNT_ALIAS", {"FAKE_PRIVATE_BADGE"}, 1000, true});
        saved = lists->publicState();
        QTRY_COMPARE(profile.value("chart/watchlists/v2").toByteArray(), saved);
        QVERIFY(!saved.contains("FAKE_PRIVATE"));
        QVERIFY(!saved.contains("ETHUSDT"));
        QVERIFY(!saved.contains("123456.789"));
        QVERIFY(!vault.isUnlocked());
    }
    profile.sync();
    {
        LocalWindow reopened(vault, accounts);
        reopened.setChartSettings(&profile);
        reopened.setPortfolioController(&portfolio);
        reopened.setMarketCandles(&candles);
        auto* workspace = child<ChartWorkspace>(reopened, "chartWorkspace");
        QVERIFY(workspace);
        auto* lists = workspace->watchlistModel();
        QVERIFY(lists);
        QCOMPARE(lists->publicState(), saved);
        const auto list = lists->userList(listId);
        QCOMPARE(list.name, QString("합성 혼합 공개 리스트"));
        QCOMPARE(list.flag, QString("purple"));
        QCOMPARE(list.sections.size(), 1);
        QCOMPARE(list.entries.size(), 2);
        QCOMPARE(list.entries[0].identity.key(), QString("coin:upbit:KRW-BTC"));
        QCOMPARE(list.entries[1].identity.key(), QString("stock:US:NVDA"));
        QVERIFY(lists->holdings().isEmpty());
        QVERIFY(lists->rowState({"coin", "upbit", "KRW-BTC"}).price.isEmpty());
        QVERIFY(!lists->isCurrentlyVerified({"stock", "US", "NVDA"}));
        QVERIFY(!vault.isUnlocked());
        QCOMPARE(profile.value("chart/watchlists/v2").toByteArray(), saved);
    }
}

void LocalWindowTest::chartWatchlistsMigrateLegacyOnceAndLeaveUnverifiedStocksPending() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    QSettings profile(directory.filePath("synthetic-legacy-profile.ini"), QSettings::IniFormat);
    const QMap<QString, QStringList> legacy{{"coin", {"BTC", "ETH"}}, {"kr", {"005930"}}, {"us", {"NVDA"}}};
    for (auto it = legacy.cbegin(); it != legacy.cend(); ++it)
        profile.setValue("explore/watchlist/" + it.key(), it.value());
    Vault vault(directory.filePath("synthetic-legacy-locked-vault"));
    AccountModel accounts(&vault);
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    NoNetworkBrokerTransport transport;
    MarketCandles candles(&transport);
    QByteArray migrated;
    {
        LocalWindow window(vault, accounts);
        window.setChartSettings(&profile);
        window.setPortfolioController(&portfolio);
        window.setMarketCandles(&candles);
        auto* workspace = child<ChartWorkspace>(window, "chartWorkspace");
        QVERIFY(workspace);
        auto* lists = workspace->watchlistModel();
        QVERIFY(lists);
        WatchlistUserList coins;
        for (const auto& list : lists->userLists()) {
            if (list.name == "CRYPTO") coins = list;
            for (const auto& entry : list.entries) QVERIFY(entry.identity.market != "stock");
        }
        QVERIFY2(!coins.id.isEmpty(), "Legacy CRYPTO uses the existing market tab name");
        QCOMPARE(coins.entries.size(), 2);
        QCOMPARE(coins.entries[0].identity.key(), QString("coin:upbit:KRW-BTC"));
        QCOMPARE(coins.entries[1].identity.key(), QString("coin:upbit:KRW-ETH"));
        migrated = lists->publicState();
        QTRY_COMPARE(profile.value("chart/watchlists/v2").toByteArray(), migrated);
        for (auto it = legacy.cbegin(); it != legacy.cend(); ++it)
            QCOMPARE(profile.value("explore/watchlist/" + it.key()).toStringList(), it.value());
        QVERIFY(!vault.isUnlocked());
    }
    {
        LocalWindow reopened(vault, accounts);
        reopened.setChartSettings(&profile);
        reopened.setPortfolioController(&portfolio);
        reopened.setMarketCandles(&candles);
        auto* workspace = child<ChartWorkspace>(reopened, "chartWorkspace");
        QVERIFY(workspace);
        auto* lists = workspace->watchlistModel();
        QVERIFY(lists);
        QCOMPARE(lists->publicState(), migrated);
        int coinRows = 0;
        for (const auto& list : lists->userLists()) for (const auto& entry : list.entries) {
            QVERIFY(entry.identity.market != "stock");
            ++coinRows;
        }
        QCOMPARE(coinRows, 2);
        for (auto it = legacy.cbegin(); it != legacy.cend(); ++it)
            QCOMPARE(profile.value("explore/watchlist/" + it.key()).toStringList(), it.value());
        QVERIFY(!vault.isUnlocked());
    }
}

void LocalWindowTest::accessiblePublicControlsDriveContent_data(){
    QTest::addColumn<QString>("control");
    for(const auto* name:{"theme","explore","options","options-focus"})QTest::newRow(name)<<QString(name);
}
void LocalWindowTest::accessiblePublicControlsDriveContent(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,dir.filePath("system-settings"));
    QSettings profile(dir.filePath("public-navigation.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("nonexistent-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);ChartWindowTray tray;
    NoNetworkBrokerTransport publicHttp,candleHttp,metadata,metricHttp;MarketMetrics metrics(&metricHttp);
    PublicMarketCatalog catalog(&publicHttp);MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
    LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog,false);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);
    window.setMarketMetrics(&metrics);window.resize(1440,900);window.show();
    QFETCH(QString,control);
    const auto toggle=[](QPushButton* button){auto* ax=QAccessible::queryAccessibleInterface(button);QVERIFY(ax&&ax->actionInterface());ax->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());};
    if(control=="theme"){
        KeefiTheme::instance()->setMode(ThemeMode::Light);
        auto* dark=child<QPushButton>(window,"themeDark");toggle(dark);QVERIFY(KeefiTheme::instance()->isDark());
        toggle(dark);QVERIFY(dark->isChecked());QVERIFY(KeefiTheme::instance()->isDark());
        toggle(child<QPushButton>(window,"themeLight"));QVERIFY(!KeefiTheme::instance()->isDark());
    }else if(control=="explore"){
        auto* view=child<QComboBox>(window,"exploreView");auto* stack=child<QStackedWidget>(window,"exploreStack");QVERIFY(view&&stack);
        for(const auto* key:{"map","list","flow"}){toggle(child<QPushButton>(window,qPrintable(QString("exploreViewTabs_")+key)));QCOMPARE(view->currentData().toString(),QString(key));QVERIFY(stack->currentWidget()->isVisibleTo(&window));}
    }else if(control=="options-focus"){
        auto* opener=child<QPushButton>(window,"openOptions");QVERIFY(opener);
        window.activateWindow();child<QPushButton>(window,"headerTab0")->setFocus();
        auto* accessible=QAccessible::queryAccessibleInterface(opener);QVERIFY(accessible&&accessible->actionInterface());
        // AXPress does not move keyboard focus to the opener before opening it.
        accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        QTRY_VERIFY(child<QDialog>(window,"optionsDialog"));
        QPointer<QDialog> dialog=child<QDialog>(window,"optionsDialog");QVERIFY(dialog);
        toggle(child<QPushButton>(*dialog,"optionsDisplayTab"));
        dialog->reject();QTRY_VERIFY(dialog.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(window.focusWidget(),opener,1000);
        QTest::keyClick(opener,Qt::Key_Space);
        QVERIFY(child<QDialog>(window,"optionsDialog"));
        child<QDialog>(window,"optionsDialog")->reject();
        processDeletion();
        auto* origin=child<QPushButton>(window,"headerTab0");origin->setFocus();
        auto* preferences=child<QAction>(window,"optionsShortcut");
        QVERIFY(preferences);preferences->trigger();
        dialog=child<QDialog>(window,"optionsDialog");QVERIFY(dialog);dialog->reject();
        QTRY_VERIFY(dialog.isNull());QTRY_COMPARE(window.focusWidget(),origin);
    }else{
        child<QPushButton>(window,"openOptions")->click();auto* dialog=child<QDialog>(window,"optionsDialog");QVERIFY(dialog);
        auto* display=child<QPushButton>(*dialog,"optionsDisplayTab");toggle(display);
        auto* stack=child<QStackedWidget>(*dialog,"optionsPages");QVERIFY(stack);QCOMPARE(stack->currentIndex(),1);
        QVERIFY(child<QWidget>(*dialog,"optionsDisplay")->isVisibleTo(dialog));
        toggle(display);QCOMPARE(stack->currentIndex(),1);QVERIFY(display->isChecked());dialog->reject();
    }
    QVERIFY(!vault.exists());
}

void LocalWindowTest::accessibleHeaderSelectionChangesPageAndStaysExclusive(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,dir.filePath("system-settings"));
    QSettings profile(dir.filePath("public-navigation.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("nonexistent-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);ChartWindowTray tray;
    NoNetworkBrokerTransport publicHttp,candleHttp,metadata;
    PublicMarketCatalog catalog(&publicHttp);MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
    LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog,false);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);
    window.resize(1440,900);window.show();
    auto* pages=child<QTabWidget>(window,"portfolioPages");QVERIFY(pages);QCOMPARE(pages->count(),4);
    pages->setCurrentIndex(0);
    for(const int index:{1,2,3,0}){
        auto* button=child<QPushButton>(window,qPrintable(QString("headerTab%1").arg(index)));QVERIFY(button);
        auto* accessible=QAccessible::queryAccessibleInterface(button);QVERIFY(accessible&&accessible->actionInterface());
        QVERIFY(accessible->actionInterface()->actionNames().contains(QAccessibleActionInterface::toggleAction()));
        accessible->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(pages->currentIndex(),index);QVERIFY(pages->currentWidget()->isVisibleTo(&window));
        for(int other=0;other<4;++other)QCOMPARE(child<QPushButton>(window,qPrintable(QString("headerTab%1").arg(other)))->isChecked(),other==index);
        // Activating the current page again cannot leave the header unselected.
        accessible->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(pages->currentIndex(),index);QVERIFY(button->isChecked());
    }
    QTest::mouseClick(child<QPushButton>(window,"headerTab1"),Qt::LeftButton);
    QCOMPARE(pages->currentIndex(),1);
    QVERIFY(!vault.exists());
}

void LocalWindowTest::chartTabKeepsPublicCandlesWhileVaultLocked(){
    QTemporaryDir dir;Vault vault(dir.filePath("synthetic-chart-vault"));
    QSettings chartProfile(dir.filePath("synthetic-chart-settings.ini"),QSettings::IniFormat);
    AccountModel accounts(&vault);PortfolioController::Factories f;
    f.prices=[]{return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,f);
    class PublicChartTransport final:public TradingTransport{public:int sent=0;quint64 token=0;
        void send(quint64 t,const RequestSpec& request)override{QVERIFY(!request.authenticated);token=t;++sent;}void abort()override{}} transport;
    MarketCandles candles(&transport);
    PublicMarketTransport publicHttp;publicHttp.quoteStatus=503;PublicMarketCatalog markets(&publicHttp);NoNetworkBrokerTransport metadata;
    LocalWindow window(vault,accounts);
    window.setChartSettings(&chartProfile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.setChartInstrumentTransport(&metadata);window.setPublicMarketCatalog(&markets,false);markets.refresh();QTRY_VERIFY(markets.marketListAvailable());
    window.setMarketCandles(&candles);
    auto* pages=child<QTabWidget>(window,"portfolioPages");QVERIFY(pages);
    int chart=-1;for(int i=0;i<pages->count();++i)if(pages->tabText(i)==QStringLiteral("차트"))chart=i;
    QVERIFY2(chart>=0,"Main navigation must include the public chart workspace");
    pages->setCurrentIndex(chart);window.resize(1200,800);window.show();
    QTRY_COMPARE(transport.sent,1);
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto end=now/MarketCandles::IntervalMs*MarketCandles::IntervalMs;QJsonArray rows;
    for(int i=0;i<45;++i){const auto at=end-i*MarketCandles::IntervalMs;const auto price=QString::number(100+(i*7)%13);
        rows.append(QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(i?at+1000:now)},{"unit","15"},{"opening_price",price},{"high_price",decimal::add(price,"4")},
            {"low_price",decimal::subtract(price,"3")},{"trade_price",decimal::add(price,"1")},{"candle_acc_trade_volume","2"}});}
    HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,response);
    QVERIFY(candles.series().available);window.lockAndClear();
    QCOMPARE(candles.series().asset,QString("BTC"));
    QCOMPARE(candles.series().candles.size(),44);QVERIFY(candles.series().forming.has_value());
    QVERIFY(child<QWidget>(window,"chartWorkspace"));
    if(const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!preview.isEmpty()){
        window.resize(1440,940);QTest::qWait(30);QVERIFY(window.grab().save(QDir(preview).filePath("c1-chart-wide.png")));
        window.resize(820,1100);QTest::qWait(30);QVERIFY(window.grab().save(QDir(preview).filePath("c1-chart-narrow.png")));
        auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");auto* lists=workspace->watchlistModel();
        const auto id=lists->createList("합성 공개 혼합 리스트","purple");const auto section=lists->addSection(id,"합성 종목");
        lists->setVerifiedInstruments({{"stock","US","NVDA"}});
        QVERIFY(lists->addIdentity(id,{"coin","upbit","KRW-BTC"},section));
        QVERIFY(lists->addIdentity(id,{"coin","bithumb","KRW-BTC"},section));
        QVERIFY(lists->addIdentity(id,{"stock","US","NVDA"},section));
        lists->setRowState({"coin","upbit","KRW-BTC"},{"123.45","+1.23%","합성 공개 시세",{"합성 지표 7/10"},end,false});
        auto* panel=test::openInstrumentPanel(*workspace);QVERIFY(panel);
        auto* selector=child<QComboBox>(*panel,"chartWorkspaceListSelector");selector->setCurrentIndex(selector->findData(id));
        window.resize(1440,940);QTest::qWait(30);QVERIFY(panel->grab().save(QDir(preview).filePath("c2-workspace-wide.png")));
        window.resize(820,1100);QTest::qWait(30);QVERIFY(panel->grab().save(QDir(preview).filePath("c2-workspace-narrow.png")));
    }
}

void LocalWindowTest::portfolioHomeShowsUnreadAccountAndStaysNavigableDuringRead(){
    QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Portfolio UI! 2026"));AccountRecord a;a.provider="binance";a.alias="FAKE UNREAD";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    NoNetworkBrokerTransport transport;PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    auto* pages=child<QTabWidget>(window,"portfolioPages");QVERIFY(pages);QCOMPARE(pages->currentIndex(),0);
    QTRY_VERIFY_WITH_TIMEOUT(transport.sent>0,1000);QVERIFY(!window.findChild<QPushButton*>("stopPortfolioReads"));
    auto* tree=child<QTreeView>(window,"portfolioAssets");QVERIFY(tree->model()->rowCount()>0);QCOMPARE(portfolioRow(tree, 0).child(0).text(1),QString("FAKE UNREAD · 현물"));
    QVERIFY(child<QLabel>(window,"portfolioSummary")->text().contains("부분합"));pages->setCurrentIndex(1);QCOMPARE(pages->currentIndex(),1);
    vault.lock();QCOMPARE(tree->model()->rowCount(),0);QVERIFY(!child<QLabel>(window,"portfolioSummary")->text().contains("FAKE"));
}
void LocalWindowTest::recordTransferEntryRequiresVaultAndClearsOnSecurityLoss(){
    QTemporaryDir dir;Vault vault(dir.filePath("vault"));AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    auto* entry=child<QPushButton>(window,"openRecordTransfer");QVERIFY(entry);QVERIFY(!entry->isEnabled());
    window.setSecurityStatus(true);QVERIFY(!entry->isEnabled());QVERIFY(vault.create("Fake Records Entry! 2026"));QVERIFY(entry->isEnabled());
    entry->click();auto* dialog=child<RecordTransferDialog>(window,"recordTransferDialog");QVERIFY(dialog);QVERIFY(dialog->isVisible());
    QVERIFY(!QFileInfo::exists(dir.filePath("imported-records.keefi")));
    child<QLineEdit>(*dialog,"exportRecordPassword")->setText("Fake Package Password! 2026");
    window.setSecurityStatus(false,"fake-security-loss");QVERIFY(!entry->isEnabled());QVERIFY(!vault.isUnlocked());
    QVERIFY(child<QLineEdit>(*dialog,"exportRecordPassword")->text().isEmpty());
    QVERIFY(!child<QPushButton>(*dialog,"prepareRecordTransfer")->isEnabled());
    QVERIFY(!QFileInfo::exists(dir.filePath("imported-records.keefi")));
}
namespace {
// Existing provider/public-price seams; every account, key and file is synthetic.
struct HiddenPortfolioProjectionFixture {
    QTemporaryDir directory;
    QSettings profile{directory.filePath("hidden-projection.ini"),QSettings::IniFormat};
    Vault vault{directory.filePath("hidden-projection-vault.bin")};
    AssetReadTransport assets;
    FreshKrwPriceTransport* prices=nullptr; // Owned by PortfolioController.
    std::unique_ptr<PortfolioController> portfolio;
    std::unique_ptr<AccountModel> accounts;
    NoNetworkBrokerTransport metadata,candleHttp,contracts;
    MarketCandles candles{&candleHttp,std::unique_ptr<PublicCandleSocket>{}};
    ChartWindowTray tray;
    std::unique_ptr<LocalWindow> window;
    QString error;
    static QString asset(int index){return QString("S%1").arg(index,3,10,QChar('0'));}
    bool setup(){
        if(!directory.isValid()){error="Temporary synthetic directory unavailable";return false;}
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
        if(!vault.create("Fake Hidden Projection! 2026")){error="Synthetic vault creation failed";return false;}
        test::ReadFixture fixture("binance");fixture.account.alias="Synthetic hidden projection";
        if(!vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault)){error="Synthetic account registration failed";return false;}
        prices=new FreshKrwPriceTransport;prices->prices.clear();
        for(int i=0;i<10;++i){assets.balances.append(QJsonObject{{"asset",asset(i)},{"free","10"},{"locked","0"}});prices->prices.insert(asset(i),"1000");}
        PortfolioController::Factories factories;factories.trading=[this](const AccountRecord&){return &assets;};factories.prices=[this]{return prices;};
        portfolio=std::make_unique<PortfolioController>(vault,nullptr,factories);accounts=std::make_unique<AccountModel>(&vault);
        window=std::make_unique<LocalWindow>(vault,*accounts,nullptr,&tray);window->setChartSettings(&profile);
        window->setPortfolioController(portfolio.get());window->setSecurityStatus(true);window->setChartInstrumentTransport(&metadata);
        window->setMarketCandles(&candles);window->setChartContractTransport(&contracts);
        auto* workspace=child<ChartWorkspace>(*window,"chartWorkspace");workspace->indicatorPreferences()->markIntroSeen();
        window->resize(1440,900);window->show();pages()->setCurrentWidget(home());return true;
    }
    QTabWidget* pages()const{return child<QTabWidget>(*window,"portfolioPages");}
    QWidget* home()const{return child<QWidget>(*window,"portfolioHome");}
    QTreeView* tree()const{return child<QTreeView>(*window,"portfolioAssets");}
    bool settled()const{return !portfolio->connections().isEmpty()&&portfolio->connections().first().state=="complete"&&
        !portfolio->pricingPending()&&child<QLabel>(*window,"portfolioLoading")->isHidden()&&tree()->model()->rowCount()==10;}
    PortfolioTestRow group(const QString& name)const{for(int i=0;i<tree()->model()->rowCount();++i)if(portfolioRow(tree(), i).text(0)==name)return portfolioRow(tree(), i);return {};}
    QStringList codes()const{QStringList result;for(int i=0;i<10;++i)result.append("KRW-"+asset(i));return result;}
    QList<AssetQuote> batch(const QString& price)const{
        const auto now=QDateTime::currentMSecsSinceEpoch();QList<AssetQuote> result;
        for(int i=0;i<10;++i)result.append({asset(i),"KRW",price,{"Synthetic public ticker","https://api.upbit.com/v1/ticker","public:upbit:KRW-"+asset(i),now,now,true}});
        return result;
    }
    void push(const QString& price){QTest::qWait(2);portfolio->setPublicPrices(batch(price),codes());}
    bool sourcePrice(const QString& expected)const{int matches=0;for(const auto& quote:portfolio->quotes())if(quote.quoteAsset=="KRW"&&quote.baseAsset.startsWith('S')&&quote.price==expected)++matches;return matches==10;}
    bool click(QWidget* page){const auto index=pages()->indexOf(page);auto* button=child<QPushButton>(*window,qPrintable(QString("headerTab%1").arg(index)));
        if(!button||!button->isVisible()||!button->isEnabled())return false;QTest::mouseClick(button,Qt::LeftButton,Qt::NoModifier,QPoint(),0);return pages()->currentWidget()==page;}
    bool hidePortfolio(){const bool shown=click(child<ChartWorkspace>(*window,"chartWorkspace"));QCoreApplication::processEvents();return shown&&!tree()->isVisibleTo(window.get());}
};
class HiddenPortfolioFirstPaint final : public QObject {
  public:
    bool seen=false;std::function<void()> inspect;
    bool eventFilter(QObject*,QEvent* event)override{if(!seen&&event->type()==QEvent::Paint){seen=true;if(inspect)inspect();}return false;}
};
}
void LocalWindowTest::hiddenPortfolioQuotesWaitForVisibleProjectionAndKeepSelection(){
    HiddenPortfolioProjectionFixture fixture;QVERIFY2(fixture.setup(),qPrintable(fixture.error));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.settled(),5000);fixture.portfolio->setAutomaticRefresh(false);
    auto* tree=fixture.tree();auto selected=fixture.group("S005");QVERIFY(selected&&selected.childCount()==1);
    tree->expandAll();tree->setCurrentIndex(selected.child(0).modelIndex());
    auto* outer=child<QScrollArea>(*fixture.window,"portfolioHomeScroll");outer->ensureWidgetVisible(tree);QCoreApplication::processEvents();
    tree->verticalScrollBar()->setValue(qMin(40,tree->verticalScrollBar()->maximum()));
    const auto selectedText=selectedPortfolioRow(tree).text(0);const auto selectedParent=selectedPortfolioRow(tree).parent().text(0);const auto scroll=tree->verticalScrollBar()->value();
    QSet<QString> expanded;for(int i=0;i<tree->model()->rowCount();++i)if(portfolioRow(tree, i).isExpanded())expanded.insert(portfolioRow(tree, i).text(0));
    QPersistentModelIndex stable(tree->currentIndex().siblingAtColumn(0));QVERIFY(stable.isValid());
    QVERIFY(fixture.hidePortfolio());
    QSignalSpy resets(tree->model(),&QAbstractItemModel::modelReset),inserted(tree->model(),&QAbstractItemModel::rowsInserted),removed(tree->model(),&QAbstractItemModel::rowsRemoved),changed(tree->model(),&QAbstractItemModel::dataChanged);
    QSignalSpy priceReplies(fixture.prices,&TradingTransport::finished);const auto reads=fixture.assets.sent;
    fixture.push("1100");fixture.push("1200");fixture.push("1234");
    QTRY_VERIFY_WITH_TIMEOUT(fixture.sourcePrice("1234"),1000);QTest::qWait(1100);
    // The upstream public cache is current. Hidden UI work must stay coalesced.
    QCOMPARE(resets.count(),0);QCOMPARE(inserted.count(),0);QCOMPARE(removed.count(),0);QCOMPARE(changed.count(),0);QVERIFY(stable.isValid());
    QCOMPARE(fixture.assets.sent,reads);QCOMPARE(priceReplies.count(),0);
    HiddenPortfolioFirstPaint paint;QString firstValue,firstTotal,firstSelected,firstParent;QSet<QString> firstExpanded;int firstScroll=-1;
    paint.inspect=[&]{if(auto group=fixture.group("S005"))firstValue=group.text(3);firstTotal=child<QLabel>(*fixture.window,"portfolioTotal")->text();
        if(auto current=selectedPortfolioRow(tree)){firstSelected=current.text(0);if(current.parent())firstParent=current.parent().text(0);}
        for(int i=0;i<tree->model()->rowCount();++i)if(portfolioRow(tree, i).isExpanded())firstExpanded.insert(portfolioRow(tree, i).text(0));firstScroll=tree->verticalScrollBar()->value();};
    tree->viewport()->installEventFilter(&paint);QVERIFY(fixture.click(fixture.home()));QTRY_VERIFY_WITH_TIMEOUT(paint.seen,1000);
    QCOMPARE(firstValue,QString("12,340"));QCOMPARE(firstTotal,QString("123,400"));
    QCOMPARE(firstSelected,selectedText);QCOMPARE(firstParent,selectedParent);QCOMPARE(firstExpanded,expanded);QCOMPARE(firstScroll,scroll);
    QCOMPARE(resets.count(),0);QCOMPARE(inserted.count(),0);QCOMPARE(removed.count(),0);
    QVERIFY(stable.isValid());QCOMPARE(stable,QPersistentModelIndex(tree->currentIndex().siblingAtColumn(0)));
    QCOMPARE(tree->model()->rowCount(),10);QCOMPARE(fixture.assets.sent,reads);QCOMPARE(priceReplies.count(),0);

    // A price-only reordering must also keep the selected holding's model index.
    auto quotes=fixture.batch("1234");
    for(auto& quote:quotes)if(quote.baseAsset==selectedParent)quote.price="5678";
    fixture.portfolio->setPublicPrices(quotes,fixture.codes());
    QTRY_COMPARE(portfolioRow(tree, 0).text(0),selectedParent);
    QCOMPARE(portfolioRow(tree, 0).text(3),QString("56,780"));
    auto highlighted=portfolioRow(tree, 0);
    QCOMPARE(highlighted.text(5),QString("33.8%"));
    QCOMPARE(highlighted.child(0).text(5),highlighted.text(5));
    const auto references=highlighted.child(0).data(3,EvidenceNumbersRole).value<QList<int>>();
    const auto entries=child<ExploreEvidenceFooter>(*fixture.window,"portfolioEvidenceFooter")->entries();
    QVERIFY(std::any_of(references.cbegin(),references.cend(),[&](int ref){
        return ref>0&&ref<=entries.size()&&entries[ref-1].provider=="업비트"&&entries[ref-1].sourceTimeMs==quotes.first().evidence.sourceTimeMs;
    }));
    QVERIFY(stable.isValid());QCOMPARE(stable,QPersistentModelIndex(tree->currentIndex().siblingAtColumn(0)));
    QCOMPARE(resets.count(),0);QCOMPARE(inserted.count(),0);QCOMPARE(removed.count(),0);
}
void LocalWindowTest::hiddenPortfolioPendingProjectionClearsAtPrivacyBoundary_data(){
    QTest::addColumn<QString>("boundary");QTest::newRow("vault-lock")<<QString("vault-lock");QTest::newRow("security-loss")<<QString("security-loss");QTest::newRow("account_reset")<<QString("account_reset");
}
void LocalWindowTest::hiddenPortfolioPendingProjectionClearsAtPrivacyBoundary(){
    QFETCH(QString,boundary);const bool accountReset=boundary=="account_reset";HiddenPortfolioProjectionFixture fixture;QVERIFY2(fixture.setup(),qPrintable(fixture.error));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.settled(),5000);fixture.portfolio->setAutomaticRefresh(false);auto* tree=fixture.tree();
    tree->setCurrentIndex(portfolioRow(tree, 0).child(0).modelIndex());QVERIFY(!child<QLabel>(*fixture.window,"assetDetailTitle")->text().isEmpty());
    QPersistentModelIndex stable(tree->model()->index(0,0));QVERIFY(stable.isValid());QVERIFY(fixture.hidePortfolio());
    fixture.push("1234");QTRY_VERIFY_WITH_TIMEOUT(fixture.sourcePrice("1234"),1000);
    const auto late=fixture.batch("9999");const auto codes=fixture.codes();bool delivered=false;
    QTimer::singleShot(0,fixture.window.get(),[&]{delivered=true;fixture.portfolio->setPublicPrices(late,codes);});
    if(accountReset){
        QVERIFY(fixture.vault.removeAccount(fixture.vault.accounts().first().id));
        QCOMPARE(tree->model()->rowCount(),0);QVERIFY(!stable.isValid());QVERIFY(child<QLabel>(*fixture.window,"assetDetailTitle")->text().isEmpty());
        // connections() masks reset-pending entries before snapshots are rebuilt.
        QTRY_VERIFY_WITH_TIMEOUT(fixture.portfolio->connections().isEmpty()&&fixture.portfolio->snapshots().isEmpty(),1000);
    }
    else if(boundary=="security-loss")fixture.window->setSecurityStatus(false,"SYNTHETIC_HIDDEN_PROJECTION_SECURITY_LOSS");else fixture.vault.lock();
    const auto emptyTotal=QStringLiteral("—"); // No current valued assets at any of these privacy boundaries.
    // Locks revoke synchronously; account removal must clear as soon as its queued rebuild finishes.
    QCOMPARE(tree->model()->rowCount(),0);QVERIFY(!stable.isValid());QCOMPARE(child<QLabel>(*fixture.window,"portfolioTotal")->text(),emptyTotal);
    QVERIFY(child<QLabel>(*fixture.window,"assetDetailTitle")->text().isEmpty());QCOMPARE(child<QTableWidget>(*fixture.window,"connectionStatusTable")->rowCount(),0);
    QTRY_VERIFY_WITH_TIMEOUT(delivered,1000);if(accountReset)fixture.portfolio->setPublicPrices(late,codes);QCoreApplication::processEvents();
    fixture.pages()->setCurrentWidget(fixture.home());QCoreApplication::processEvents();
    QCOMPARE(tree->model()->rowCount(),0);QVERIFY(!stable.isValid());QCOMPARE(child<QLabel>(*fixture.window,"portfolioTotal")->text(),emptyTotal);
    QVERIFY(child<QLabel>(*fixture.window,"assetDetailTitle")->text().isEmpty());QVERIFY(child<QLabel>(*fixture.window,"portfolioLoading")->isHidden());
    QVERIFY(!fixture.window->findChild<QLabel*>("assetDetailHolding0"));QCOMPARE(child<QTableWidget>(*fixture.window,"connectionStatusTable")->rowCount(),0);
}

void LocalWindowTest::portfolioShowsAssetsOnlyWhileVaultUnlocked(){
    QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Portfolio Gate! 2026"));AccountRecord a;a.provider="binance";a.alias="FAKE GATE";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport transport;PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    auto* tree=child<QTreeView>(window,"portfolioAssets");QTRY_VERIFY_WITH_TIMEOUT(tree->model()->rowCount()>0&&portfolioRow(tree, 0).text(0)=="BTC",2000);
    QVERIFY(!child<QLabel>(window,"portfolioProgress")->text().contains("임시 조회"));
    // Reads refresh on their own: no manual refresh, and a healthy account offers no retry.
    QVERIFY(!window.findChild<QPushButton*>("refreshPortfolio"));
    tree->setCurrentIndex(portfolioRow(tree, 0).child(0).modelIndex());QVERIFY(!child<QPushButton>(window,"retryPortfolioConnection")->isEnabled());
    vault.lock();QVERIFY(!child<QPushButton>(window,"retryPortfolioConnection")->isEnabled());QCOMPARE(tree->model()->rowCount(),0);
}
void LocalWindowTest::accountManagementDisplaysReadFailureReason(){
    QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake UI Failure! 2026"));AccountRecord a;a.provider="binance";a.alias="FAKE FAILURE";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport transport;transport.fail=true;PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    auto* state=child<QLabel>(window,"connectionIssue");QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("실패"),2000);QVERIFY(!state->text().contains("PRIVATE_RAW_MESSAGE"));
    vault.lock();
}
void LocalWindowTest::publicPriceDetailsSeparateAccountSuccessAndClearOnLock(){
    QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Price Details! 2026"));AccountRecord a;a.provider="binance";a.alias="FAKE PRICE DETAILS";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport assets;auto* prices=new FailedPublicPriceTransport;PortfolioController::Factories f;f.prices=[prices]{return prices;};f.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();
    QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingStatus().contains("HTTP 503"),2500);QCOMPARE(portfolio.connections().first().state,QString("complete"));
    auto* details=child<QPushButton>(window,"portfolioPriceDetails");QVERIFY(details->isEnabled());const auto sent=prices->sent;details->click();
    auto* box=child<QMessageBox>(window,"publicPriceDetails");QVERIFY(box);QVERIFY(box->text().contains("업비트 현재가"));
    QVERIFY(box->informativeText().contains("조회 실패 1개"));QVERIFY(!box->informativeText().contains("FAKE_PRIVATE"));QCOMPARE(prices->sent,sent);
    vault.lock();QVERIFY(!details->isEnabled());QVERIFY(!box->isVisible());QVERIFY(box->informativeText().isEmpty());
}
void LocalWindowTest::publicPriceDiagnosticKeepsFailuresVisible_data(){
    QTest::addColumn<bool>("hadSuccessfulQuote");
    QTest::newRow("first-failure")<<false;
    QTest::newRow("failure-after-success")<<true;
}
void LocalWindowTest::publicPriceDiagnosticKeepsFailuresVisible(){
    QFETCH(bool,hadSuccessfulQuote);
    QTemporaryDir dir;QVERIFY(dir.isValid());
    Vault vault(dir.filePath("synthetic-price-diagnostic"));QVERIFY(vault.create("Fake Price Diagnostic! 2026"));
    AccountRecord account;account.provider="binance";account.alias="가짜 가격 진단 계좌";account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    DeferredPortfolioTransport assets(false);auto* prices=new DeferredPortfolioTransport(true);
    prices->hold=!hadSuccessfulQuote;prices->fail=!hadSuccessfulQuote;
    PortfolioController::Factories factories;factories.prices=[prices]{return prices;};factories.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1440,900);window.show();
    auto* pages=child<QTabWidget>(window,"portfolioPages");pages->setCurrentIndex(1);
    auto* table=child<QTableWidget>(window,"connectionStatusTable");
    child<QPushButton>(window,"connectionStatusToggle")->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(table->rowCount()==2&&!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",3000);
    if(hadSuccessfulQuote){
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy()&&!portfolio.quotes().isEmpty(),3000);
        prices->hold=true;prices->fail=true;QTest::qWait(1100);portfolio.refreshPrices();
    }else{
        // The first held response is the catalogue; the following quote fails.
        QTRY_VERIFY_WITH_TIMEOUT(!prices->pending.isEmpty(),2000);prices->release(true);
    }
    QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingBusy()&&!prices->pending.isEmpty(),2500);
    prices->release();
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy()&&portfolio.pricingStatus().contains("HTTP 503"),2500);
    // Both data rows fail here in Dev 84: it showed "조회 전" on a first
    // failure and "가격 조회 완료" when an earlier quote remained available.
    QTRY_VERIFY_WITH_TIMEOUT(table->item(1,1)->text().contains("실패")&&table->item(1,1)->text().contains("HTTP 503"),1500);
    QCOMPARE(table->item(0,1)->text(),QString("조회 완료"));
    QCOMPARE(!portfolio.quotes().isEmpty(),hadSuccessfulQuote);
    QVERIFY(table->item(1,1)->toolTip().contains("HTTP 503"));
    QVERIFY(!table->item(1,1)->text().contains("FAKE_PRIVATE_PRICE_FAILURE"));
    QVERIFY(!table->item(1,1)->toolTip().contains("FAKE_PRIVATE_PRICE_FAILURE"));
    auto* action=qobject_cast<QPushButton*>(table->cellWidget(1,4));QVERIFY(action);QVERIFY(action->isEnabled());
    QCoreApplication::processEvents();
    const auto actionBounds=QRect(action->mapTo(table->viewport(),QPoint()),action->size());
    const auto rowHeight=table->rowHeight(1);QCOMPARE(rowHeight,44);
    const auto accountReads=portfolio.connections().first().receivedAtMs;
    prices->hold=true;prices->fail=false;QTest::qWait(1100);action->click();
    QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingBusy()&&!prices->pending.isEmpty(),2500);
    QCOMPARE(table->item(1,1)->text(),hadSuccessfulQuote?QString("다시 조회 중"):QString("첫 조회 중"));
    QVERIFY(!action->isEnabled());QCOMPARE(table->rowHeight(1),rowHeight);
    QCOMPARE(QRect(action->mapTo(table->viewport(),QPoint()),action->size()),actionBounds);
    prices->release();
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy()&&table->item(1,1)->text().contains("조회 완료"),2500);
    QVERIFY(!table->item(1,1)->text().contains("실패"));QVERIFY(!table->item(1,1)->toolTip().contains("HTTP 503"));
    QVERIFY(action->isEnabled());QCOMPARE(table->rowHeight(1),rowHeight);
    QCOMPARE(QRect(action->mapTo(table->viewport(),QPoint()),action->size()),actionBounds);
    QCOMPARE(portfolio.connections().first().receivedAtMs,accountReads);
    vault.lock();QCOMPARE(table->rowCount(),0);
    QVERIFY(!child<QPushButton>(window,"portfolioPriceDetails")->isEnabled());
}
void LocalWindowTest::portfolioShowsReferencePriceAndPromotesOnlyFreshPrice(){
    QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake Reference UI! 2026"));AccountRecord a;a.provider="binance";a.alias="가짜 참고평가 검증";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport assets;assets.balances={QJsonObject{{"asset","PUMP"},{"free","2"},{"locked","0"}},QJsonObject{{"asset","KRW"},{"free","10"},{"locked","0"}}};
    auto* prices=new ReferencePublicPriceTransport;PortfolioController::Factories f;f.prices=[prices]{return prices;};f.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1600,900);window.show();
    auto* tree=child<QTreeView>(window,"portfolioAssets");
    const auto pump=[tree]() -> PortfolioTestRow {for(int i=0;i<tree->model()->rowCount();++i){auto group=portfolioRow(tree, i);if(group.text(0)=="PUMP"&&group.childCount())return group.child(0);}return {};};
    QTRY_VERIFY_WITH_TIMEOUT(pump()&&pump().text(3)=="참고 6",3000);
    QVERIFY(pump().text(4).contains("합계 제외"));QVERIFY(pump().text(4).contains("KST"));
    // The price basis is part of the detail body, not only a tooltip.
    tree->setCurrentIndex(pump().modelIndex());QVERIFY(child<QLabel>(window,"assetDetailHolding0")->text().contains("업비트 공식 KRW 현재가"));
    QVERIFY(child<QLabel>(window,"assetDetailHolding0")->text().contains("참고"));QVERIFY(child<QLabel>(window,"assetDetailHolding0")->text().contains("합계 제외"));
    QVERIFY(child<QLabel>(window,"portfolioSummary")->text().contains("부분합  10 KRW"));QVERIFY(portfolio.quotes().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1500);
    tree->expandAll();const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    // The selected row's detail opens beside the list at a usable width. The
    // splitter keeps no width for a detail hidden until a selection exists.
    auto* split=qobject_cast<QSplitter*>(tree->parentWidget());QVERIFY(split);QCOMPARE(split->count(),2);
    QTRY_VERIFY2(split->widget(1)->isVisible()&&split->widget(1)->width()>=ListDetailView::DetailMinimumWidth,qPrintable(QString::number(split->widget(1)->width())));
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("portfolio-reference-price.png")));
    const auto reads=assets.sent;prices->fresh=true;portfolio.refreshPrices();
    QTRY_VERIFY_WITH_TIMEOUT(pump()&&pump().text(3)=="6",2500);QVERIFY(!pump().text(4).contains("참고"));
    QVERIFY(child<QLabel>(window,"portfolioSummary")->text().contains("15.5 KRW"));QCOMPARE(assets.sent,reads);
    vault.lock();QCOMPARE(tree->model()->rowCount(),0);QVERIFY(portfolio.referenceQuotes().isEmpty());
}

// Usability defect 3: Binance Spot rows showed "—" because only Upbit KRW and the
// selected series fed row prices. A dedicated public ticker stream fills shown rows.
void LocalWindowTest::chartRowsShowBinancePublicPricesForShownRows(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-public-rows.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-rows"));AccountModel accounts(&vault);
    class Socket final:public PublicTickerSocket {
      public:
        int opens=0,aborts=0;QByteArray sent;
        void open(const QUrl&)override{++opens;}
        void send(const QByteArray& data)override{sent=data;}
        void ping()override{}
        void abort()override{++aborts;}
        void ready(){emit connected();}
        void tick(const QJsonObject& o){emit received(QJsonDocument(o).toJson(QJsonDocument::Compact));}
    };
    PublicBinanceRowMetadata metadata;
    class Silent final:public TradingTransport {public:void send(quint64,const RequestSpec& r)override{QVERIFY(!r.authenticated);}void abort()override{}} chart;
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles model(&chart);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&model);
    auto socket=std::make_unique<Socket>();auto* wire=socket.get();PublicQuoteStream rows(PublicQuoteStream::Provider::Binance,std::move(socket));
    window.setChartRowQuoteStream(&rows);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* catalogue=window.findChild<ChartInstrumentCatalog*>();QVERIFY(catalogue);
    catalogue->ensure("binance");QTRY_COMPARE(catalogue->instruments("binance").size(),5);
    window.resize(1280,800);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QVERIFY(test::openInstrumentPanel(*workspace));
    child<QPushButton>(*workspace,"chartBrowserMarket_binance")->click();
    child<QPushButton>(*workspace,"chartBrowserScope_all")->click();
    child<QLineEdit>(*workspace,"chartWorkspaceAsset")->setText("SYNUSDT");
    QVERIFY(workspace->setIdentity("coin:binance:SYNUSDT"));
    QTRY_COMPARE(workspace->visiblePublicIdentityKeysForDisplay(),QStringList{"coin:binance:SYNUSDT"});
    // The new all-market list may reconcile its viewport after the initial
    // chart selection. Connect the currently opened synthetic socket, not an
    // earlier subscription whose sent payload happens to contain this symbol.
    QTRY_VERIFY_WITH_TIMEOUT(wire->opens>0,3000);
    QTRY_VERIFY_WITH_TIMEOUT(([&]{wire->ready();return rows.status().contains(QStringLiteral("수신 대기"))&&
        wire->sent.contains("synusdt@miniTicker");})(),3000);
    QCOMPARE(QJsonDocument::fromJson(wire->sent).object().value("params").toArray().size(),1);
    const auto now=QDateTime::currentMSecsSinceEpoch();
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","1.2345"},{"o","1"},{"E",double(now)}});
    QTRY_COMPARE_WITH_TIMEOUT(workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).price,QString("1.2345"),5000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).change,QString("+23.45%"),3000);
    QVERIFY(!workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).privateData);
    // Leaving the chart stops the subscription; nothing streams for hidden rows.
    auto* pages=child<QTabWidget>(window,"portfolioPages");const auto beforeAbort=wire->aborts;
    QVERIFY(pages->count()>1);pages->setCurrentIndex(pages->indexOf(workspace)==0?1:0);
    QTRY_VERIFY_WITH_TIMEOUT(wire->aborts>beforeAbort,2500);
    QVERIFY(!vault.isUnlocked());QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
}

void LocalWindowTest::chartRowsKeepLastBinanceQuoteAcrossReconnect(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-reconnect-rows.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-reconnect"));AccountModel accounts(&vault);
    class Socket final:public PublicTickerSocket {
      public:
        int opens=0;QSet<QString> subscribed;
        void open(const QUrl&)override{++opens;subscribed.clear();}
        void send(const QByteArray& data)override{
            const auto msg=QJsonDocument::fromJson(data).object();
            for(const auto& name:msg.value("params").toArray()){
                if(msg.value("method")=="SUBSCRIBE")subscribed.insert(name.toString());
                else if(msg.value("method")=="UNSUBSCRIBE")subscribed.remove(name.toString());
            }
        }
        void ping()override{}
        void abort()override{}
        void ready(){emit connected();}
        void tick(const QJsonObject& value){emit received(QJsonDocument(value).toJson(QJsonDocument::Compact));}
        void disconnectSource(){emit failed();}
    };
    PublicBinanceRowMetadata metadata;
    class Silent final:public TradingTransport {public:void send(quint64,const RequestSpec& r)override{QVERIFY(!r.authenticated);}void abort()override{}} chart;
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles model(&chart);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&model);
    auto socket=std::make_unique<Socket>();auto* wire=socket.get();PublicQuoteStream quotes(PublicQuoteStream::Provider::Binance,std::move(socket));
    window.setChartRowQuoteStream(&quotes);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* catalogue=window.findChild<ChartInstrumentCatalog*>();QVERIFY(catalogue);
    catalogue->ensure("binance"); // This fixture explicitly needs these public Binance rows.
    QTRY_COMPARE(catalogue->instruments("binance").size(),5);
    const WatchlistIdentity identity{"coin","binance","SYNUSDT"};auto* lists=workspace->watchlistModel();
    const auto list=lists->createList(QStringLiteral("합성 재연결 관심"));QVERIFY(lists->addIdentity(list,identity));
    const WatchlistIdentity other{"coin","binance","ALTUSDT"};QVERIFY(lists->addIdentity(list,other));
    window.resize(1440,900);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    workspace->setIdentity(identity.key());
    auto* selector=child<QComboBox>(*workspace,"chartWorkspaceListSelector");QVERIFY(selector);
    selector->setCurrentIndex(selector->findData(list));
    child<QPushButton>(*workspace,"chartBrowserMarket_binance")->click();
    child<QPushButton>(*workspace,"chartBrowserScope_watch")->click();
    child<QLineEdit>(*workspace,"chartWorkspaceAsset")->clear();
    // Seed the saved scope before the panel first publishes its viewport.
    QVERIFY(test::openInstrumentPanel(*workspace));
    auto* tree=child<QTreeWidget>(*workspace,"chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),2);
    QTRY_COMPARE(workspace->visiblePublicIdentityKeysForDisplay(),(QStringList{identity.key(),other.key()}));
    QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("—"));
    QTRY_VERIFY_WITH_TIMEOUT(wire->opens>0,3000);wire->ready();
    const QSet<QString> initial{"synusdt@miniTicker","altusdt@miniTicker"};
    QTRY_COMPARE_WITH_TIMEOUT(wire->subscribed,initial,3000);
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","1.2345"},{"o","1"},{"E",double(QDateTime::currentMSecsSinceEpoch())}});
    QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(identity).price,QString("1.2345"),5000);
    QVERIFY(tree->topLevelItem(0)->text(1).contains("1.2345"));
    const auto publicState=lists->publicState();const auto opened=wire->opens;
    wire->disconnectSource();emit model.changed(); // Exercise the normal source-notification projection synchronously.
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Disconnected);
    QVERIFY(tree->topLevelItem(0)->text(1).contains(QStringLiteral("끊김")));
    QTRY_VERIFY_WITH_TIMEOUT(wire->opens>opened,3000);emit model.changed();
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Delayed);
    wire->ready();emit model.changed();
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Delayed);
    QTest::qWait(1200); // Consume the real coalesced row refresh after reconnect, without a replacement tick.
    QCOMPARE(lists->rowState(identity).price,QString("1.2345"));
    QVERIFY(tree->topLevelItem(0)->text(1).contains("1.2345"));
    QVERIFY(!tree->topLevelItem(0)->text(1).contains(QStringLiteral("받는 중")));
    QCOMPARE(lists->publicState(),publicState);QVERIFY(!lists->rowState(identity).privateData);
    QVERIFY(tree->topLevelItem(0)->text(1).contains(QStringLiteral("지연")));
    wire->tick({{"e","24hrMiniTicker"},{"s","ALTUSDT"},{"c","5.6789"},{"o","5"},{"E",double(QDateTime::currentMSecsSinceEpoch())}});
    QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(other).price,QString("5.6789"),5000);
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Delayed);
    QVERIFY(tree->topLevelItem(0)->text(1).contains(QStringLiteral("지연")));
    const auto refreshedAt=QDateTime::currentMSecsSinceEpoch();
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","2.3456"},{"o","2"},{"E",double(refreshedAt)}});
    QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(identity).price,QString("2.3456"),5000);
    QVERIFY(tree->topLevelItem(0)->text(1).contains("2.3456"));
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Receiving);
    QVERIFY(!tree->topLevelItem(0)->text(1).contains(QStringLiteral("지연")));
    QVERIFY(!tree->topLevelItem(0)->text(1).contains(QStringLiteral("끊김")));
    // The third row only adds a subscription; the two existing prices remain live.
    const auto beforeChange=wire->opens;QVERIFY(lists->addIdentity(list,{"coin","binance","THIRDUSDT"}));
    QTRY_VERIFY_WITH_TIMEOUT(wire->subscribed.contains("thirdusdt@miniTicker"),3000);
    QCOMPARE(wire->opens,beforeChange);emit model.changed();
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Receiving);
    QSignalSpy delivered(&quotes,&PublicQuoteStream::ticks);
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","1.1111"},{"o","1"},{"E",double(refreshedAt-1)}});
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","2.3456"},{"o","2"},{"E",double(refreshedAt)}});
    QTest::qWait(1100);QCOMPARE(delivered.size(),0);emit model.changed();
    QCOMPARE(lists->rowState(identity).price,QString("2.3456"));
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Receiving);
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","4.5678"},{"o","4"},{"E",double(refreshedAt+1)}});
    QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(identity).price,QString("4.5678"),5000);
    QCOMPARE(lists->rowState(identity).quoteState,WatchlistQuoteState::Receiving);
    // Replacing the source object revokes its cached observation immediately,
    // and a different selected identity cannot borrow the old public row.
    window.setChartRowQuoteStream(nullptr);
    QVERIFY(lists->rowState(identity).price.isEmpty());
    QVERIFY(!tree->topLevelItem(0)->text(1).contains("4.5678"));
    workspace->setIdentity("coin:binance:OTHERUSDT");
    auto* current=child<QLabel>(*workspace,"chartCurrentPrice");QVERIFY(current);QCOMPARE(current->text(),QStringLiteral("—"));
    auto replacement=std::make_unique<Socket>();auto* replacementWire=replacement.get();
    PublicQuoteStream newQuotes(PublicQuoteStream::Provider::Binance,std::move(replacement));window.setChartRowQuoteStream(&newQuotes);
    QTRY_VERIFY_WITH_TIMEOUT(replacementWire->opens>0,3000);replacementWire->ready();
    replacementWire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","9.8765"},{"o","9"},{"E",double(QDateTime::currentMSecsSinceEpoch())}});
    QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(identity).price,QString("9.8765"),5000);
    QCOMPARE(current->text(),QStringLiteral("—"));
    replacementWire->tick({{"e","24hrMiniTicker"},{"s","OTHERUSDT"},{"c","3.4567"},{"o","3"},{"E",double(QDateTime::currentMSecsSinceEpoch())}});
    QTRY_COMPARE_WITH_TIMEOUT(current->text(),QString("3.4567"),5000);
    workspace->setIdentity("coin:binance:SYNUSDC");QCOMPARE(current->text(),QStringLiteral("—"));
    QVERIFY(lists->rowState({"coin","binance","SYNUSDC"}).price.isEmpty());
    QVERIFY(!vault.isUnlocked());QVERIFY(lists->holdings().isEmpty());
}

void LocalWindowTest::chartRowsCompareDisconnectedTickerWithCompletedCandle_data(){
    QTest::addColumn<bool>("newerCandle");QTest::newRow("newer-completed-price")<<true;QTest::newRow("older-candle-does-not-replace-ticker")<<false;
}
void LocalWindowTest::chartRowsCompareDisconnectedTickerWithCompletedCandle(){
    QFETCH(bool,newerCandle);QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-source-precedence.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-precedence"));AccountModel accounts(&vault);
    class Metadata final:public TradingTransport{public:
        void send(quint64 token,const RequestSpec& request)override{
            QVERIFY(!request.authenticated);QVERIFY(request.url.path().endsWith("exchangeInfo"));
            HttpResult result;result.status=200;result.body=QJsonDocument(QJsonObject{{"symbols",QJsonArray{QJsonObject{
                {"symbol","SYNUSDT"},{"baseAsset","SYN"},{"quoteAsset","USDT"},{"status","TRADING"},{"isSpotTradingAllowed",true}}}}}).toJson();
            QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
        }void abort()override{}
    } metadata;
    class ChartHttp final:public TradingTransport{public:quint64 token=0;qint64 requestedAt=0;QString path;
        void send(quint64 value,const RequestSpec& request)override{QVERIFY(!request.authenticated);token=value;path=request.url.path();requestedAt=QDateTime::currentMSecsSinceEpoch();}
        void abort()override{}
    } chart;
    class Socket final:public PublicTickerSocket{public:int opens=0;QByteArray sent;
        void open(const QUrl&)override{++opens;}void send(const QByteArray& data)override{sent=data;}void ping()override{}void abort()override{}
        void ready(){emit connected();}void tick(const QJsonObject& data){emit received(QJsonDocument(data).toJson());}void fail(){emit failed();}
    };
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles candles(&chart,std::unique_ptr<PublicCandleSocket>{});LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);
    auto socket=std::make_unique<Socket>();auto* wire=socket.get();PublicQuoteStream quotes(PublicQuoteStream::Provider::Binance,std::move(socket));
    window.setChartRowQuoteStream(&quotes);auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    window.resize(1440,900);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    const WatchlistIdentity identity{"coin","binance","SYNUSDT"};workspace->setIdentity(identity.key());workspace->setTimeframe(60000);
    QTRY_COMPARE_WITH_TIMEOUT(chart.path,QString("/api/v3/klines"),3000);QCOMPARE(candles.series().intervalMs,qint64(60000));QTRY_VERIFY_WITH_TIMEOUT(wire->opens>0,3000);wire->ready();
    const auto end=chart.requestedAt/60000*60000;
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","1.2345"},{"o","1"},{"E",double(end-1000)}});
    auto* lists=workspace->watchlistModel();QTRY_COMPARE_WITH_TIMEOUT(lists->rowState(identity).price,QString("1.2345"),5000);
    wire->fail();
    const auto startsAt=end-(newerCandle?60000:120000);
    QJsonArray candleRows;candleRows.append(QJsonArray{QString::number(startsAt),"100","110","90","105","2",
        QString::number(startsAt+59999),"210","5","1","105","0"});
    HttpResult result;result.status=200;result.body=QJsonDocument(candleRows).toJson();
    emit chart.finished(chart.token,result);
    QVERIFY2(candles.series().available,qPrintable(candles.series().reason+" / "+QString::fromUtf8(result.body)));
    const auto row=lists->rowState(identity);
    QCOMPARE(row.price,newerCandle?QString("105"):QString("1.2345"));
    QCOMPARE(row.completedAtMs,startsAt+60000);QVERIFY(!row.privateData);
    QCOMPARE(row.quoteState,newerCandle?WatchlistQuoteState::Receiving:WatchlistQuoteState::Disconnected);
    QVERIFY(row.evidence.contains(newerCandle?QString("klines"):QString("wss://")));
}

void LocalWindowTest::chartRowsRequireVerifiedBinanceSpotCatalog(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-public-catalogue.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-catalogue"));AccountModel accounts(&vault);
    class Socket final:public PublicTickerSocket {
    public:
        int opens=0;QByteArray sent;
        void open(const QUrl&)override{++opens;}
        void send(const QByteArray& data)override{sent=data;}
        void ping()override{}void abort()override{}
        void ready(){emit connected();}
    };
    CoinWindowPublicTransport metadata,chart;
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);MarketCandles candles(&chart);
    LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);
    window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);
    auto socket=std::make_unique<Socket>();auto* wire=socket.get();
    PublicQuoteStream quotes(PublicQuoteStream::Provider::Binance,std::move(socket));window.setChartRowQuoteStream(&quotes);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* lists=workspace->watchlistModel();const auto list=lists->createList("합성 공개 목록 검증");
    for(const auto& code:{"SYNUSDT","MISSINGUSDT","HALTUSDT","NOSPOTUSDT","SYNUSDC"})
        QVERIFY(lists->addIdentity(list,{"coin","binance",code}));
    window.resize(1440,900);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QVERIFY(test::openInstrumentPanel(*workspace));
    auto* selector=child<QComboBox>(*workspace,"chartWorkspaceListSelector");QVERIFY(selector);
    QTRY_VERIFY(selector->findData(list)>=0);selector->setCurrentIndex(selector->findData(list));
    auto* tree=child<QTreeWidget>(*workspace,"chartWorkspaceWatchlist");QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(),5);
    QStringList demands;for(const auto& entry:lists->userList(list).entries)demands.append(entry.identity.key());
    // An already published viewport demand can outlive its metadata. The
    // receiving window must authorize it again, independently of the UI filter.
    emit workspace->visibleIdentitiesChanged(demands);emit candles.changed();
    QTest::qWait(350); // The public socket coalesces subscription changes for 250 ms.
    QCOMPARE(wire->opens,0); // No catalogue response: no row may start the ticker socket.
    QTRY_VERIFY(metadata.token>0);
    HttpResult response;response.status=200;
    response.body=R"({"symbols":[
        {"symbol":"SYNUSDT","baseAsset":"SYN","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},
        {"symbol":"HALTUSDT","baseAsset":"HALT","quoteAsset":"USDT","status":"BREAK","isSpotTradingAllowed":true},
        {"symbol":"NOSPOTUSDT","baseAsset":"NOSPOT","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":false},
        {"symbol":"SYNUSDC","baseAsset":"SYN","quoteAsset":"USDC","status":"TRADING","isSpotTradingAllowed":true}
    ]})";
    emit metadata.finished(metadata.token,response);
    emit workspace->visibleIdentitiesChanged(demands);emit candles.changed();
    QTRY_COMPARE(wire->opens,1);wire->ready();
    QTRY_VERIFY(wire->sent.contains("synusdt@miniTicker"));
    const auto subscription=QJsonDocument::fromJson(wire->sent).object().value("params").toArray();
    QCOMPARE(subscription,QJsonArray{"synusdt@miniTicker"});
    QVERIFY(!vault.isUnlocked());QVERIFY(lists->holdings().isEmpty());
}

// An unsupported pair must not poison otherwise supported public subscriptions.
void LocalWindowTest::chartRowsSkipUnsupportedBinanceQuotesWithoutDroppingUsdt(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-public-rows.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-rows"));AccountModel accounts(&vault);
    class Socket final:public PublicTickerSocket {
      public:
        int opens=0,aborts=0;QByteArray sent;
        void open(const QUrl&)override{++opens;}
        void send(const QByteArray& data)override{sent=data;}
        void ping()override{}
        void abort()override{++aborts;}
        void ready(){emit connected();}
        void tick(const QJsonObject& o){emit received(QJsonDocument(o).toJson(QJsonDocument::Compact));}
    };
    PublicBinanceRowMetadata metadata;
    class Silent final:public TradingTransport {public:void send(quint64,const RequestSpec& r)override{QVERIFY(!r.authenticated);}void abort()override{}} chart;
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles model(&chart);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&model);
    auto socket=std::make_unique<Socket>();auto* wire=socket.get();PublicQuoteStream rows(PublicQuoteStream::Provider::Binance,std::move(socket));
    window.setChartRowQuoteStream(&rows);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* lists=workspace->watchlistModel();const auto listId=lists->createList("합성 혼합 호가");
    QVERIFY(lists->addIdentity(listId,{"coin","binance","SYNUSDC"}));
    QVERIFY(lists->addIdentity(listId,{"coin","binance","SYNUSDT"}));
    auto* selector=child<QComboBox>(*workspace,"chartWorkspaceListSelector");
    workspace->setIdentity("coin:binance:SYNUSDC");
    window.resize(1280,800);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QVERIFY(test::openInstrumentPanel(*workspace));
    QTRY_VERIFY_WITH_TIMEOUT(selector->findData(listId)>=0,2000);selector->setCurrentIndex(selector->findData(listId));
    QTRY_COMPARE_WITH_TIMEOUT(workspace->currentListId(),listId,2000);
    QTRY_VERIFY_WITH_TIMEOUT(wire->opens>0,3000);wire->ready();
    QTRY_VERIFY_WITH_TIMEOUT(wire->sent.contains("synusdt@miniTicker"),3000);
    QVERIFY(!wire->sent.contains("synusdc@miniTicker"));
    const auto now=QDateTime::currentMSecsSinceEpoch();
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","1.2345"},{"o","1"},{"E",double(now)}});
    QTRY_COMPARE_WITH_TIMEOUT(workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).price,QString("1.2345"),5000);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).change,QString("+23.45%"),3000);
    QVERIFY(!workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).privateData);
    QVERIFY(lists->rowState({"coin","binance","SYNUSDC"}).price.isEmpty());
    // Leaving the chart stops the subscription; nothing streams for hidden rows.
    auto* pages=child<QTabWidget>(window,"portfolioPages");const auto beforeAbort=wire->aborts;
    QVERIFY(pages->count()>1);pages->setCurrentIndex(pages->indexOf(workspace)==0?1:0);
    QTRY_VERIFY_WITH_TIMEOUT(wire->aborts>beforeAbort,2500);
    QVERIFY(!vault.isUnlocked());QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
}

// Reconnection may deliver an old source event with a new receipt timestamp.
void LocalWindowTest::chartRowsRejectOldBinanceOriginalTime(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings profile(dir.filePath("synthetic-public-rows.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-locked-rows"));AccountModel accounts(&vault);
    class Socket final:public PublicTickerSocket {
      public:
        int opens=0;QByteArray sent;
        void open(const QUrl&)override{++opens;}
        void send(const QByteArray& data)override{sent=data;}
        void ping()override{}
        void abort()override{}
        void ready(){emit connected();}
        void tick(const QJsonObject& o){emit received(QJsonDocument(o).toJson(QJsonDocument::Compact));}
    };
    PublicBinanceRowMetadata metadata;
    class Silent final:public TradingTransport {public:void send(quint64,const RequestSpec& r)override{QVERIFY(!r.authenticated);}void abort()override{}} chart;
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles model(&chart);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    window.setChartSettings(&profile);window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&model);
    auto socket=std::make_unique<Socket>();auto* wire=socket.get();PublicQuoteStream rows(PublicQuoteStream::Provider::Binance,std::move(socket));
    window.setChartRowQuoteStream(&rows);
    auto* workspace=child<ChartWorkspace>(window,"chartWorkspace");QVERIFY(workspace);
    auto* catalogue=window.findChild<ChartInstrumentCatalog*>();QVERIFY(catalogue);
    catalogue->ensure("binance");QTRY_COMPARE(catalogue->instruments("binance").size(),5);
    window.resize(1280,800);window.show();child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(workspace);
    QVERIFY(test::openInstrumentPanel(*workspace));
    child<QPushButton>(*workspace,"chartBrowserMarket_binance")->click();
    child<QPushButton>(*workspace,"chartBrowserScope_all")->click();
    child<QLineEdit>(*workspace,"chartWorkspaceAsset")->setText("SYNUSDT");
    QVERIFY(workspace->setIdentity("coin:binance:SYNUSDT"));
    QTRY_COMPARE(workspace->visiblePublicIdentityKeysForDisplay(),QStringList{"coin:binance:SYNUSDT"});
    QTRY_VERIFY_WITH_TIMEOUT(wire->opens>0,3000);
    QTRY_VERIFY_WITH_TIMEOUT(([&]{wire->ready();return rows.status().contains(QStringLiteral("수신 대기"))&&
        wire->sent.contains("synusdt@miniTicker");})(),3000);
    QCOMPARE(QJsonDocument::fromJson(wire->sent).object().value("params").toArray().size(),1);
    QSignalSpy batches(&rows,&PublicQuoteStream::ticks);
    const auto now=QDateTime::currentMSecsSinceEpoch();
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","1.2345"},{"E",double(now-600000)}});
    QTRY_COMPARE_WITH_TIMEOUT(batches.count(),1,3000);
    // Allow the public row presentation cycle to consume the received batch.
    // A new receipt must not make a ten-minute-old source observation current.
    QTest::qWait(1200);
    QCOMPARE(workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).price,QString{});
    wire->tick({{"e","24hrMiniTicker"},{"s","SYNUSDT"},{"c","2.3456"},{"E",double(QDateTime::currentMSecsSinceEpoch())}});
    QTRY_COMPARE_WITH_TIMEOUT(workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).price,QString("2.3456"),5000);
    QVERIFY(!workspace->watchlistModel()->rowState({"coin","binance","SYNUSDT"}).privateData);
    // Leaving the chart stops the subscription; nothing streams for hidden rows.
    auto* pages=child<QTabWidget>(window,"portfolioPages");
    QVERIFY(pages->count()>1);pages->setCurrentIndex(pages->indexOf(workspace)==0?1:0);
    QVERIFY(!vault.isUnlocked());QVERIFY(workspace->watchlistModel()->holdings().isEmpty());
}

namespace {
class LoadingFirstPriceSocket final:public PublicTickerSocket {
  public:
    int opens=0;
    void open(const QUrl&)override{++opens;}
    void send(const QByteArray&)override{}
    void ping()override{}
    void abort()override{}
};
class LoadingFirstPriceHttp final:public TradingTransport {
  public:
    explicit LoadingFirstPriceHttp(bool prices):prices_(prices){}
    int calls=0;
    bool failQuotes=false;
    quint64 heldAccountToken=0;HttpResult heldAccount;
    void abort()override{heldAccountToken=0;heldAccount={};}
    void releaseAccount(){
        const auto token=heldAccountToken;const auto response=heldAccount;
        heldAccountToken=0;heldAccount={};
        if(token)QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
    void send(quint64 token,const RequestSpec& request)override{
        ++calls;QVERIFY(!isMutation(request.operation));
        HttpResult response;response.status=200;
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(!prices_){
            QVERIFY(request.operation==Operation::Time||request.operation==Operation::Account);
            if(request.operation==Operation::Time)
                response.body=QJsonDocument(QJsonObject{{"serverTime",QString::number(now)}}).toJson();
            else{
                response.body=QJsonDocument(QJsonObject{{"canTrade",true},{"accountType","SPOT"},
                    {"balances",QJsonArray{QJsonObject{{"asset","FWS"},{"free","2"},{"locked","0"}}}}}).toJson();
                heldAccountToken=token;heldAccount=response;return;
            }
        }else{
            QVERIFY(!request.authenticated);QVERIFY(request.headers.isEmpty());
            QVERIFY(NetworkTradingTransport::allowed(request));
            if(request.operation==Operation::Markets&&request.url.host()=="api.binance.com"){
                // FWS has a verified Binance Spot route, but no direct Upbit KRW route.
                response.body=QJsonDocument(QJsonObject{{"symbols",QJsonArray{QJsonObject{
                    {"symbol","FWSUSDT"},{"baseAsset","FWS"},{"quoteAsset","USDT"},
                    {"status","TRADING"},{"isSpotTradingAllowed",true}}}}}).toJson();
            }else if(request.operation==Operation::Markets){
                QCOMPARE(request.url.host(),QString("api.upbit.com"));
                response.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-USDT"}}}).toJson();
            }else{
                QCOMPARE(request.operation,Operation::Quote);
                QCOMPARE(request.url.host(),QString("api.upbit.com"));
                QCOMPARE(QUrlQuery(request.url).queryItemValue("markets"),QString("KRW-USDT"));
                response.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-USDT"},
                    {"trade_price","1400"},{"timestamp",QString::number(now)}}}).toJson();
                if(failQuotes){response.status=503;response.body="FAKE_PRIVATE_PRICE_FAILURE";}
            }
        }
        QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
    }
  private:bool prices_;
};
}


namespace {
struct LoadingProgressFixture {
    QTemporaryDir dir;Vault vault{dir.filePath("synthetic-loading-vault")};
    DeferredPortfolioTransport assets{false};DeferredPortfolioTransport* prices=nullptr;
    qint64 clock=0;std::unique_ptr<PortfolioController> portfolio;std::unique_ptr<AccountModel> accounts;
    std::vector<std::unique_ptr<DeferredPortfolioTransport>> extraTransports;QMap<QString,DeferredPortfolioTransport*> transports;
    ChartWindowTray tray;std::unique_ptr<LocalWindow> window;
    bool setup(int rows=1,int connections=1){
        if(!dir.isValid())return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
        if(!vault.create("Fake Loading Progress! 2026"))return false;
        AccountRecord a;a.provider="binance";a.alias="SYNTHETIC LOADING";a.environment="live";a.market="spot";a.currency="USDT";
        for(int i=0;i<connections;++i){
            a.alias=QString("SYNTHETIC LOADING %1").arg(i);
            if(!vault.addAccount(a,{{"api_key",QString("FakeLoadingProgressKey%1").arg(i)},{"secret",QString("FakeLoadingProgressSecret%1").arg(i)}},StorageMode::EncryptedVault))return false;
            auto* wire=&assets;if(i){extraTransports.push_back(std::make_unique<DeferredPortfolioTransport>(false));wire=extraTransports.back().get();wire->hold=true;}
            for(const auto& saved:vault.accounts())if(saved.alias==a.alias)transports.insert(saved.id,wire);
        }
        vault.lock();assets.hold=true;assets.extraZeroRows=rows-1;prices=new DeferredPortfolioTransport(true);prices->hold=true;
        PortfolioController::Factories f;f.trading=[this](const AccountRecord& a){return transports.value(a.id);};f.prices=[this]{return prices;};
        f.monotonicMs=[this]{return clock;};f.binanceBalances=[](Vault&){return static_cast<BinanceBalanceStream*>(nullptr);};
        portfolio=std::make_unique<PortfolioController>(vault,nullptr,f);portfolio->setBinanceBalancePilot(false);accounts=std::make_unique<AccountModel>(&vault);
        window=std::make_unique<LocalWindow>(vault,*accounts,nullptr,&tray);window->setPortfolioController(portfolio.get());window->setSecurityStatus(true);
        window->resize(1440,900);window->show();return vault.unlock("Fake Loading Progress! 2026");
    }
    QProgressBar* bar()const{return window->findChild<QProgressBar*>("portfolioLoadProgress");}
    QLabel* stage()const{return window->findChild<QLabel*>("portfolioLoadStage");}
    QLabel* count()const{return window->findChild<QLabel*>("portfolioLoadTaskCount");}
    QLabel* percent()const{return window->findChild<QLabel*>("portfolioLoadPercent");}
    QTreeView* tree()const{return window->findChild<QTreeView*>("portfolioAssets");}
};
}
void LocalWindowTest::loadingProgressCountsRealTasksAndCompletesAfterProjection_data(){
    QTest::addColumn<int>("theme");QTest::addColumn<int>("width");
    for(auto theme:{ThemeMode::Light,ThemeMode::Dark})for(int width:{1440,720})
        QTest::newRow(qPrintable(QString("%1-%2").arg(theme==ThemeMode::Light?"light":"dark").arg(width)))<<int(theme)<<width;
}
void LocalWindowTest::loadingProgressCountsRealTasksAndCompletesAfterProjection(){
    QFETCH(int,theme);QFETCH(int,width);LoadingProgressFixture f;QVERIFY(f.setup());
    KeefiTheme::instance()->setMode(ThemeMode(theme));f.window->resize(width,900);
    auto* bar=f.bar();QVERIFY2(bar,"The loader needs a real task progress bar");QVERIFY(f.stage());QVERIFY(f.count());
    QTRY_VERIFY(!f.assets.pending.isEmpty());QCOMPARE(bar->maximum(),3);QCOMPARE(bar->value(),0);
    QCOMPARE(f.stage()->text(),QStringLiteral("잔고 불러오기"));QVERIFY(f.count()->text().contains("0 / 3단계 완료"));
    // The number follows the eased fill: it may trail the finished steps but is never ahead of them.
    QVERIFY(f.percent());QCOMPARE(f.percent()->text(),QStringLiteral("0%"));int highestPercent=0;
    connect(bar,&QProgressBar::valueChanged,f.window.get(),[&]{highestPercent=qMax(highestPercent,f.percent()->text().chopped(1).toInt());});
    const auto capture=[&](const QString& phase){const auto dir=qEnvironmentVariable("KEEFI_LOADING_RENDER_DIR");if(dir.isEmpty())return true;
        if(!QDir().mkpath(dir))return false;QTest::qWait(100);return f.window->grab().save(QDir(dir).filePath(QString("loading-%1%2-%3.png").arg(phase,theme==int(ThemeMode::Light)?"light":"dark").arg(width)));};
    QVERIFY(capture("account-"));
    QSignalSpy values(bar,&QProgressBar::valueChanged);bool projectedBefore100=false;
    connect(bar,&QProgressBar::valueChanged,f.window.get(),[&](int value){if(value==3)projectedBefore100=f.tree()->model()->rowCount()==1;});
    f.assets.release();QTRY_COMPARE(f.portfolio->connections().first().state,QString("complete"));QTRY_VERIFY(!f.prices->pending.isEmpty());
    QCOMPARE(bar->value(),1);QCOMPARE(f.stage()->text(),QStringLiteral("시세 확인"));QVERIFY(f.window->findChild<QLabel*>("portfolioLoading")->isVisibleTo(f.window.get()));
    QVERIFY(f.percent()->text().chopped(1).toInt()<=33);QTRY_COMPARE(f.percent()->text(),QStringLiteral("33%"));
    QTest::qWait(60);QCOMPARE(f.percent()->text(),QStringLiteral("33%"));QCOMPARE(bar->value(),1); // waiting on prices adds nothing to the count
    QVERIFY(capture({}));
    f.prices->release();QTRY_VERIFY(f.window->findChild<QLabel*>("portfolioLoading")->isHidden());
    QCOMPARE(bar->value(),3);QVERIFY(projectedBefore100);QCOMPARE(f.window->findChild<QLabel*>("portfolioTotal")->text(),QString("10,000"));
    int previous=0;bool sawPreparing=false;for(const auto& row:values){const auto value=row.first().toInt();QVERIFY(value>=previous);previous=value;sawPreparing|=value==2;}QVERIFY(sawPreparing);
    QVERIFY2(highestPercent<=66,"While a step was still open the number never claimed more than the finished steps.");
    f.vault.lock();QCOMPARE(bar->value(),0);QCOMPARE(f.percent()->text(),QStringLiteral("0%"));QVERIFY(bar->isHidden()||!bar->isVisibleTo(f.window.get()));QCOMPARE(f.tree()->model()->rowCount(),0);
}
void LocalWindowTest::loadingProgressNeverRegressesDuringAutomaticReread(){
    LoadingProgressFixture f;QVERIFY(f.setup());auto* bar=f.bar();QVERIFY2(bar,"The loader needs first-cycle task latches");
    QTRY_VERIFY(!f.assets.pending.isEmpty());f.assets.release();QTRY_COMPARE(f.portfolio->connections().first().state,QString("complete"));
    QTRY_VERIFY(!f.prices->pending.isEmpty());QCOMPARE(bar->value(),1);f.assets.hold=true;f.clock+=21000;
    QTRY_VERIFY_WITH_TIMEOUT(!f.assets.pending.isEmpty(),1800);QCOMPARE(bar->maximum(),3);QCOMPARE(bar->value(),1);QCOMPARE(f.stage()->text(),QStringLiteral("시세 확인"));
    f.vault.lock();f.assets.release();f.prices->release();QCoreApplication::processEvents();QCOMPARE(bar->value(),0);QCOMPARE(f.tree()->model()->rowCount(),0);
}
void LocalWindowTest::loadingProgressFailureAndLockStayTruthful(){
    LoadingProgressFixture f;QVERIFY(f.setup());auto* bar=f.bar();QVERIFY2(bar,"Failure is a terminal task outcome, not a successful balance");
    QTRY_VERIFY(!f.assets.pending.isEmpty());f.assets.fail=true; // Replace only the held synthetic response with a failure.
    for(auto& response:f.assets.pending){response.second.status=401;response.second.body=R"({"code":-2015,"msg":"SYNTHETIC"})";}
    f.assets.release();QTRY_COMPARE(f.portfolio->connections().first().state,QString("failed"));
    QTRY_VERIFY(f.window->findChild<QLabel*>("portfolioLoading")->isHidden());QCOMPARE(bar->maximum(),3);QCOMPARE(bar->value(),3);
    QVERIFY(f.window->findChild<QLabel*>("portfolioSummary")->text().contains(QStringLiteral("순자산 확인 미완료")));
    QVERIFY(f.window->findChild<QLabel*>("portfolioTotal")->text()!=QString("0"));f.vault.lock();QCOMPARE(bar->value(),0);QCOMPARE(f.tree()->model()->rowCount(),0);
}
void LocalWindowTest::loadingProgressUsesFixedConnectionSet(){
    LoadingProgressFixture f;QVERIFY(f.setup(1,2));auto* bar=f.bar();QVERIFY(bar);
    auto* second=f.extraTransports.front().get();QTRY_VERIFY(!f.assets.pending.isEmpty()&&!second->pending.isEmpty());QCOMPARE(bar->maximum(),4);
    f.assets.release();QTRY_COMPARE(bar->value(),1);QCOMPARE(f.stage()->text(),QStringLiteral("잔고 불러오기"));
    for(auto& response:second->pending){response.second.status=401;response.second.body=R"({"code":-2015,"msg":"SYNTHETIC"})";}
    second->release();QTRY_COMPARE(bar->value(),2);QCOMPARE(bar->maximum(),4);QCOMPARE(f.stage()->text(),QStringLiteral("시세 확인"));
    if(const auto dir=qEnvironmentVariable("KEEFI_LOADING_RENDER_DIR");!dir.isEmpty())for(auto theme:{ThemeMode::Light,ThemeMode::Dark}){
        KeefiTheme::instance()->setMode(theme);QTest::qWait(100);QVERIFY(f.window->grab().save(QDir(dir).filePath(QString("loading-failure-%1-1440.png").arg(theme==ThemeMode::Light?"light":"dark"))));
    }
    auto* partial=child<QPushButton>(*f.window,"portfolioPendingDetails");QTRY_VERIFY(partial->isVisibleTo(f.window.get()));partial->click();
    QCoreApplication::processEvents();QVERIFY(child<QWidget>(*f.window,"portfolioWorkbench")->isVisibleTo(f.window.get()));QCOMPARE(bar->value(),2);QVERIFY(f.portfolio->pricingPending());
    f.prices->release();QTRY_COMPARE(bar->value(),4);QVERIFY(f.window->findChild<QLabel*>("portfolioLoading")->isHidden());QCOMPARE(bar->maximum(),4);
    int failed=0;for(const auto& state:f.portfolio->connections())failed+=state.state=="failed";QCOMPARE(failed,1);f.vault.lock();
}
void LocalWindowTest::loadingProgressDropsQueuedPresentationAcrossRelock(){
    LoadingProgressFixture f;QVERIFY(f.setup());auto* bar=f.bar();QVERIFY(bar);QTRY_VERIFY(!f.assets.pending.isEmpty());
    f.assets.release();QTRY_COMPARE(bar->value(),1);for(int i=0;i<100;++i)emit f.portfolio->changed();
    f.vault.lock();QCOMPARE(bar->value(),0);f.assets.hold=true;QVERIFY(f.vault.unlock("Fake Loading Progress! 2026"));
    QTRY_VERIFY(!f.assets.pending.isEmpty());QCoreApplication::processEvents();QCOMPARE(bar->value(),0);QCOMPARE(bar->maximum(),3);
    QCOMPARE(f.stage()->text(),QStringLiteral("잔고 불러오기"));QVERIFY(f.window->findChild<QLabel*>("portfolioLoading")->isVisibleTo(f.window.get()));
    f.vault.lock();QCOMPARE(f.tree()->model()->rowCount(),0);
}
void LocalWindowTest::loadingHiddenProjectionAndBurstProbe(){
    const int rows=qMax(10,qEnvironmentVariableIntValue("KEEFI_LOADING_PROBE_ROWS"));LoadingProgressFixture f;QVERIFY(f.setup(rows));
    QTRY_VERIFY(!f.assets.pending.isEmpty());
    QSignalSpy resets(f.tree()->model(),&QAbstractItemModel::modelReset),inserted(f.tree()->model(),&QAbstractItemModel::rowsInserted);
    f.assets.release();QTRY_COMPARE(f.portfolio->connections().first().state,QString("complete"));QTRY_VERIFY(!f.prices->pending.isEmpty());
    const auto initialResets=resets.count();const auto initialRows=f.tree()->model()->rowCount();const auto initialInserted=inserted.count();resets.clear();
    QVERIFY(f.window->findChild<QLabel*>("portfolioLoading")->isVisibleTo(f.window.get()));
    auto* connections=child<QListView>(*f.window,"accountList")->model();QVERIFY(connections);QSignalSpy refreshes(connections,&QAbstractItemModel::dataChanged);
    int beats=0;QList<qint64> gaps;QElapsedTimer heart;heart.start();qint64 previous=0;QTimer heartbeat;
    connect(&heartbeat,&QTimer::timeout,f.window.get(),[&]{const auto now=heart.nsecsElapsed();if(previous)gaps.append(now-previous);previous=now;++beats;});heartbeat.start(5);
    QTest::qWait(15);QElapsedTimer timer;timer.start();
    for(int i=0;i<12;++i){QTest::qWait(2);const auto now=QDateTime::currentMSecsSinceEpoch();f.portfolio->setPublicPrices({{"USDT","KRW",QString::number(1001+i),{"Synthetic","https://api.upbit.com/v1/ticker","public:upbit:KRW-USDT",now,now,true}}},{"KRW-USDT"});}
    for(int i=0;i<100;++i)emit f.portfolio->changed();QTest::qWait(20);heartbeat.stop();
    qint64 maximum=0;for(const auto gap:gaps)maximum=qMax(maximum,gap);qInfo().noquote()<<QString("LOADING_PROBE rows=%1 wall_ms=%2 max_heartbeat_gap_ms=%3 beats=%4 resets=%5 refreshes=%6 initial_resets=%7 bursts=100 quotes=12").arg(rows).arg(timer.nsecsElapsed()/1e6,0,'f',3).arg(maximum/1e6,0,'f',3).arg(beats).arg(resets.count()).arg(refreshes.count()).arg(initialResets);
    QVERIFY2(refreshes.count()<=20,"One event-turn burst must coalesce the repeated presentation cascade");QCOMPARE(initialResets,0);QCOMPARE(initialRows,0);QCOMPARE(initialInserted,0);QCOMPARE(resets.count(),0);f.vault.lock();QCOMPARE(f.tree()->model()->rowCount(),0);
}

void LocalWindowTest::loadingColdProjectionFlushesLatestOnPageChangeAndSettle(){
    // Same source/evaluation DTOs stay current while the cold home loader hides
    // display-only allocation and private explore/flow projections.
    PublicMarketTransport publicHttp;PublicMarketCatalog markets(&publicHttp);
    NoNetworkBrokerTransport metricHttp,calendarHttp;MarketMetrics metrics(&metricHttp);OfficialCalendar calendar(&calendarHttp);
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::make_unique<LoadingFirstPriceSocket>());
    PublicQuoteStream binance(PublicQuoteStream::Provider::Binance,std::make_unique<LoadingFirstPriceSocket>());
    LoadingProgressFixture f;QVERIFY(f.setup());
    f.window->setPublicMarketCatalog(&markets,false);f.window->setMarketMetrics(&metrics,nullptr,&upbit,&binance);f.window->setOfficialCalendar(&calendar);
    auto* pages=child<QTabWidget>(*f.window,"portfolioPages");auto* home=child<QWidget>(*f.window,"portfolioHome");
    auto* view=child<QComboBox>(*f.window,"exploreView");
    auto* table=child<QTableWidget>(*f.window,"exploreMarkets");
    pages->setCurrentWidget(child<QWidget>(*f.window,"explorePage"));
    view->setCurrentIndex(view->findData("list"));
    child<QLineEdit>(*f.window,"exploreSearch")->setText("KRW-BTC");
    markets.refresh();
    QTRY_COMPARE(table->rowCount(),2);
    pages->setCurrentWidget(home);
    QTRY_VERIFY(!f.assets.pending.isEmpty());
    for(auto& response:f.assets.pending)response.second.body=QJsonDocument(QJsonObject{{"canTrade",true},{"accountType","SPOT"},{"balances",QJsonArray{
        QJsonObject{{"asset","BTC"},{"free","10"},{"locked","0"}},QJsonObject{{"asset","KRW"},{"free","10000"},{"locked","0"}}}}}).toJson();
    f.prices->supportedAssets={"BTC"};f.prices->price="2000";
    f.assets.release();QTRY_COMPARE(f.portfolio->connections().first().state,QString("complete"));QTRY_VERIFY(!f.prices->pending.isEmpty());
    const auto quote=[&](const QString& price){const auto now=QDateTime::currentMSecsSinceEpoch();f.portfolio->setPublicPrices({{"BTC","KRW",price,{"Synthetic","https://api.upbit.com/v1/ticker","public:upbit:KRW-BTC",now,now,true}}},{"KRW-BTC"});};
    quote("1000");QTRY_VERIFY(child<QLabel>(*f.window,"portfolioSummary")->text().contains("20,000"));
    quote("2000");QTRY_VERIFY(child<QLabel>(*f.window,"portfolioSummary")->text().contains("30,000"));
    QVERIFY(f.portfolio->pricingPending());QVERIFY(child<QLabel>(*f.window,"portfolioLoading")->isVisibleTo(f.window.get()));
    auto* allocation=child<QWidget>(*f.window,"portfolioAllocation");QVERIFY(allocation);QVERIFY(allocation->accessibleDescription().isEmpty());
    auto* context=child<QLabel>(*f.window,"marketFlowCalendarPrivateContext");QVERIFY(context);QVERIFY(context->text().isEmpty());
    int bitcoin=-1;
    for(int row=0;row<table->rowCount();++row)if(table->item(row,1)->text()=="KRW-BTC")bitcoin=row;
    QVERIFY(bitcoin>=0);QVERIFY(table->item(bitcoin,4)->text().isEmpty());
    // Inspect immediately after selecting the page, before queued callbacks or
    // a one-second context timer can cover a missing flush.
    pages->setCurrentWidget(child<QWidget>(*f.window,"explorePage"));
    QCOMPARE(table->item(bitcoin,4)->text(),QString("보유"));
    view->setCurrentIndex(view->findData("flow"));
    QVERIFY(context->text().contains("66.7%"));QVERIFY(!context->text().contains("50.0%"));
    QCOMPARE(f.portfolio->snapshots().size(),1);
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto evaluated=AssetValuation::value(f.portfolio->snapshots(),f.portfolio->quotes(),"KRW",now);
    QCOMPARE(evaluated.netAssets,QString("30000"));
    pages->setCurrentWidget(home);QVERIFY(child<QLabel>(*f.window,"portfolioLoading")->isVisibleTo(f.window.get()));
    f.prices->release();QTRY_VERIFY(child<QLabel>(*f.window,"portfolioLoading")->isHidden());
    QVERIFY(allocation->accessibleDescription().contains("66.7%"));QCOMPARE(child<QLabel>(*f.window,"portfolioTotal")->text(),QString("30,000"));
    f.vault.lock();QVERIFY(allocation->accessibleDescription().isEmpty());QVERIFY(context->text().isEmpty());QVERIFY(context->accessibleName().isEmpty());
    QVERIFY(table->item(bitcoin,4)->text().isEmpty());QCOMPARE(f.tree()->model()->rowCount(),0);
    quote("3000");emit f.portfolio->changed();QCoreApplication::processEvents();
    QVERIFY(allocation->accessibleDescription().isEmpty());QVERIFY(context->text().isEmpty());QVERIFY(table->item(bitcoin,4)->text().isEmpty());
}

void LocalWindowTest::loadingProgressWaitsForExistingManualRead(){
    LoadingProgressFixture f;QVERIFY(f.setup());f.portfolio->setAutomaticRefresh(false);
    const auto id=f.vault.accounts().first().id;TradingController* manual=nullptr;
    QTRY_VERIFY((manual=f.portfolio->trading(id))&&!manual->busy());QVERIFY(manual->refreshAssets());
    QTRY_VERIFY(!f.assets.pending.isEmpty());f.portfolio->setAutomaticRefresh(true);
    QTRY_COMPARE(f.portfolio->connections().first().state,QString("waiting_for_manual_request"));
    QTest::qWait(30);QCOMPARE(f.bar()->maximum(),3);QCOMPARE(f.bar()->value(),0);
    QCOMPARE(f.stage()->text(),QStringLiteral("잔고 불러오기"));QVERIFY(child<QLabel>(*f.window,"portfolioLoading")->isVisibleTo(f.window.get()));
    f.vault.lock();QCOMPARE(f.bar()->value(),0);QCOMPARE(f.tree()->model()->rowCount(),0);
}
void LocalWindowTest::loadingProgressWaitsForFirstStreamPriceAfterRestFinishes(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    const QString password="Fake First Stream Price! 2026";
    Vault vault(dir.filePath("synthetic-first-stream-price"));QVERIFY(vault.create(password));
    AccountRecord account;account.provider="binance";account.alias="SYNTHETIC FIRST PRICE";
    account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeFirstPriceKey123456"},
        {"secret","FakeFirstPriceSecret123456"}},StorageMode::EncryptedVault));
    vault.lock();
    LoadingFirstPriceHttp assets(false);auto* prices=new LoadingFirstPriceHttp(true);
    PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::make_unique<LoadingFirstPriceSocket>());
    PortfolioController::Factories factories;
    factories.trading=[&](const AccountRecord&){return &assets;};factories.prices=[prices]{return prices;};
    factories.binanceBalances=[](Vault&){return static_cast<BinanceBalanceStream*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);portfolio.setBinanceBalancePilot(false);
    portfolio.setBinancePriceStream(&stream);
    AccountModel accounts(&vault);ChartWindowTray tray;LocalWindow window(vault,accounts,nullptr,&tray);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1440,900);window.show();
    auto* bar=child<QProgressBar>(window,"portfolioLoadProgress");
    auto* stage=child<QLabel>(window,"portfolioLoadStage");
    auto* count=child<QLabel>(window,"portfolioLoadTaskCount");
    auto* loading=child<QLabel>(window,"portfolioLoading");
    auto* total=child<QLabel>(window,"portfolioTotal");
    QVERIFY(bar);QVERIFY(stage);QVERIFY(count);QVERIFY(loading);QVERIFY(total);
    QVERIFY(vault.unlock(password));QTRY_VERIFY(assets.heldAccountToken>0);
    QCOMPARE(bar->maximum(),3);QCOMPARE(bar->value(),0);
    assets.releaseAccount();
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
    // The three original public REST requests (Upbit catalogue, FX quote,
    // Binance catalogue) finish, but the first FWS/USDT tick never arrives.
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy()&&portfolio.pricingPending(),4000);
    QTest::qWait(40); // Allow queued presentation/settle callbacks; not a performance threshold.
    QVERIFY(!portfolio.pricingBusy());QVERIFY(portfolio.pricingPending());
    QCOMPARE(bar->maximum(),3);QCOMPARE(bar->value(),1);
    QCOMPARE(stage->text(),QStringLiteral("시세 확인"));QVERIFY(count->text().contains("1 / 3단계 완료"));
    QVERIFY(loading->isVisibleTo(&window));QCOMPARE(total->text(),QStringLiteral("—"));
    const int accountReads=assets.calls,publicReads=prices->calls;
    const auto received=QDateTime::currentMSecsSinceEpoch();
    PublicTick tick;tick.market="FWSUSDT";
    tick.quote={"FWS","USDT","3",{"Binance synthetic public USDT tick",
        "wss://stream.binance.com:9443/ws","public:binance:FWSUSDT",received,received,true}};
    emit stream.ticks(QList<PublicTick>{tick});
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingPending(),1500);
    QTRY_VERIFY_WITH_TIMEOUT(loading->isHidden()&&bar->value()==3,2500);
    QCOMPARE(bar->maximum(),3);QCOMPARE(total->text(),QStringLiteral("8,400")); // 2 FWS × 3 USDT × 1400 KRW.
    QCOMPARE(assets.calls,accountReads);QCOMPARE(prices->calls,publicReads);
    vault.lock();QCOMPARE(bar->value(),0);QVERIFY(!bar->isVisibleTo(&window));
    QCOMPARE(child<QTreeView>(window,"portfolioAssets")->model()->rowCount(),0);
}

void LocalWindowTest::loadingProgressOpensAfterBoundedWaitWhenStreamPriceNeverArrives(){
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    const QString password="Fake First Stream Price! 2026";
    Vault vault(dir.filePath("synthetic-silent-stream-price"));QVERIFY(vault.create(password));
    AccountRecord account;account.provider="binance";account.alias="SYNTHETIC FIRST PRICE";
    account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeFirstPriceKey123456"},
        {"secret","FakeFirstPriceSecret123456"}},StorageMode::EncryptedVault));
    vault.lock();
    LoadingFirstPriceHttp assets(false);auto* prices=new LoadingFirstPriceHttp(true);
    PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::make_unique<LoadingFirstPriceSocket>());
    PortfolioController::Factories factories;
    factories.trading=[&](const AccountRecord&){return &assets;};factories.prices=[prices]{return prices;};
    factories.binanceBalances=[](Vault&){return static_cast<BinanceBalanceStream*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);portfolio.setBinanceBalancePilot(false);
    portfolio.setBinancePriceStream(&stream);
    AccountModel accounts(&vault);ChartWindowTray tray;LocalWindow window(vault,accounts,nullptr,&tray);
    window.setPortfolioStreamGraceMs(250);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1440,900);window.show();
    auto* bar=child<QProgressBar>(window,"portfolioLoadProgress");
    auto* stage=child<QLabel>(window,"portfolioLoadStage");
    auto* count=child<QLabel>(window,"portfolioLoadTaskCount");
    auto* loading=child<QLabel>(window,"portfolioLoading");
    auto* total=child<QLabel>(window,"portfolioTotal");
    QVERIFY(bar);QVERIFY(stage);QVERIFY(count);QVERIFY(loading);QVERIFY(total);
    QVERIFY(vault.unlock(password));QTRY_VERIFY(assets.heldAccountToken>0);
    QCOMPARE(bar->maximum(),3);QCOMPARE(bar->value(),0);
    assets.releaseAccount();
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),3000);
    // The three original public REST requests (Upbit catalogue, FX quote,
    // Binance catalogue) finish, but the first FWS/USDT tick never arrives.
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy()&&portfolio.pricingPending(),4000);
    // Dev 96: the silent market holds the first screen only for the bounded wait.
    const int accountReads=assets.calls,publicReads=prices->calls;
    QTRY_VERIFY_WITH_TIMEOUT(loading->isHidden()&&bar->value()==3,4000);
    QVERIFY2(portfolio.pricingPending(),"The missing stream quote is still outstanding after the screen opens");
    auto* tree=child<QTreeView>(window,"portfolioAssets");QTRY_VERIFY_WITH_TIMEOUT(tree->model()->rowCount()==1,2000);
    // An unpriced holding must not be presented as a completed total.
    QVERIFY2(total->text()!=QStringLiteral("8,400"),qPrintable(total->text()));
    QCOMPARE(assets.calls,accountReads);QCOMPARE(prices->calls,publicReads);
    const auto received=QDateTime::currentMSecsSinceEpoch();
    PublicTick tick;tick.market="FWSUSDT";
    tick.quote={"FWS","USDT","3",{"Binance synthetic public USDT tick",
        "wss://stream.binance.com:9443/ws","public:binance:FWSUSDT",received,received,true}};
    emit stream.ticks(QList<PublicTick>{tick});
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingPending(),1500);
    QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("8,400"),2500); // The late quote completes the valuation without another read.
    QCOMPARE(assets.calls,accountReads);QCOMPARE(prices->calls,publicReads);
    vault.lock();QCOMPARE(bar->value(),0);QVERIFY(!bar->isVisibleTo(&window));
    QTest::qWait(300); // An expired wait must not reopen a locked screen.
    QCOMPARE(tree->model()->rowCount(),0);
}

void LocalWindowTest::publicPriceFailureSurvivesPartialStreamUpdate() {
    QTemporaryDir dir; Vault vault(dir.filePath("synthetic-price-tick"));
    QVERIFY(vault.create("Fake Price Tick! 2026"));
    AccountRecord account; account.provider="binance"; account.alias="Synthetic"; account.environment="live";
    account.market="spot"; account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeTickKey123456789"},{"secret","FakeTickSecret123456789"}},StorageMode::EncryptedVault));
    LoadingFirstPriceHttp assets(false); auto* prices=new LoadingFirstPriceHttp(true);
    PublicQuoteStream stream(PublicQuoteStream::Provider::Binance,std::make_unique<LoadingFirstPriceSocket>());
    PortfolioController::Factories factories;
    factories.trading=[&](const AccountRecord&){return &assets;}; factories.prices=[prices]{return prices;};
    PortfolioController portfolio(vault,nullptr,factories); portfolio.setBinancePriceStream(&stream);
    AccountModel accounts(&vault); LocalWindow window(vault,accounts); window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true); window.resize(1280,900); window.show();
    auto* table=child<QTableWidget>(window,"connectionStatusTable");
    child<QTabWidget>(window,"portfolioPages")->setCurrentIndex(1);
    child<QPushButton>(window,"connectionStatusToggle")->setChecked(true);
    QTRY_VERIFY(assets.heldAccountToken); assets.releaseAccount();
    QTRY_VERIFY(!portfolio.pricingBusy()&&portfolio.pricingPending()); portfolio.setAutomaticRefresh(false);
    prices->failQuotes=true; portfolio.refreshPrices();
    QTRY_VERIFY(table->rowCount()==2&&table->item(1,1)->text().contains("HTTP 503"));
    const auto at=QDateTime::currentMSecsSinceEpoch(); PublicTick tick; tick.market="FWSUSDT";
    tick.quote={"FWS","USDT","3",{"Synthetic Binance tick","wss://stream.binance.com:9443/ws","public:binance:FWSUSDT",at,at,true}};
    emit stream.ticks({tick});
    const auto quotes=portfolio.quotes();
    QVERIFY(std::any_of(quotes.cbegin(),quotes.cend(),[](const auto& q){return q.baseAsset=="FWS";}));
    QVERIFY(portfolio.pricingStatus().contains("자동 갱신"));
    QCoreApplication::processEvents(); // Deliver the queued screen refresh caused by the tick.
    QVERIFY(table->item(1,1)->text().contains("HTTP 503")); QVERIFY(!table->item(1,1)->text().contains("FAKE_PRIVATE"));
    prices->failQuotes=false; portfolio.refreshPrices(); QTRY_VERIFY(!portfolio.pricingBusy()&&!table->item(1,1)->text().contains("실패"));
    vault.lock(); QCOMPARE(table->rowCount(),0);
}

void LocalWindowTest::coldUnlockWaitsForCurrentData_data() {
    QTest::addColumn<bool>("partial");
    QTest::newRow("complete-old-cache") << false;
    QTest::newRow("partial-old-cache") << true;
}
void LocalWindowTest::coldUnlockWaitsForCurrentData() {
    QFETCH(bool,partial);
    QTemporaryDir dir;Vault vault(dir.filePath("fake-cold-vault"));
    const QString password="Fake Cold Start! 2026";QVERIFY(vault.create(password));
    AccountRecord a;a.provider="binance";a.alias="FAKE COLD START";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FakeColdKey123456789"},{"secret","FakeColdSecret123456789"}},StorageMode::EncryptedVault));
    DeferredPortfolioTransport assets(false);auto* prices=new DeferredPortfolioTransport(true);
    PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &assets;};f.prices=[prices]{return prices;};
    PortfolioController portfolio(vault,nullptr,f);portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.snapshots().isEmpty()&&!portfolio.quotes().isEmpty()&&!portfolio.pricingPending(),4000);
    QString error;QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Ready,3000);
    portfolio.setAutomaticRefresh(false);
    auto snapshots=portfolio.snapshots();auto quotes=portfolio.quotes();const auto at=QDateTime::currentMSecsSinceEpoch()-86400000;
    const auto old=[at](AssetEvidence& e){if(e.sourceTimeMs)e.sourceTimeMs=at;if(e.receivedAtMs)e.receivedAtMs=at;};
    for(auto& snapshot:snapshots){snapshot.timeMs=at;old(snapshot.evidence);for(auto& component:snapshot.components)old(component.evidence);}
    for(auto& quote:quotes)old(quote.evidence);
    if(partial){auto c=snapshots.first().components.first();c.id="unknown-fixture";c.asset="NOQUOTE";c.quantity="0.01";snapshots.first().components.append(c);}
    QVERIFY(portfolio.latestLedger()->replaceLatest(snapshots,quotes,&error));vault.lock();
    assets.hold=true;assets.quantity="20";prices->hold=true;prices->price="2000";
    AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1200,800);window.show();
    QVERIFY(vault.unlock(password));
    auto* total=child<QLabel>(window,"portfolioTotal");auto* loading=child<QLabel>(window,"portfolioLoading");auto* tree=child<QTreeView>(window,"portfolioAssets");
    QTRY_VERIFY_WITH_TIMEOUT(!assets.pending.isEmpty()&&!prices->pending.isEmpty(),2000);
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath(QString("cold-unlock-%1.png").arg(partial?"partial":"complete"))));
    QVERIFY2(!loading->isHidden(),"First unlock must wait even when yesterday's cache contains some priced assets");
    QCOMPARE(total->text(),QStringLiteral("—"));QVERIFY(!tree->isVisibleTo(&window));
    for(auto* chip:window.findChildren<QPushButton*>("portfolioGapChip"))QVERIFY(!chip->isVisibleTo(&window));
    if(partial){
        QTest::qWait(15300);
        QTRY_VERIFY_WITH_TIMEOUT(loading->text().contains("늦어지고"),1500);
        QVERIFY(!loading->isHidden());QVERIFY(!tree->isVisibleTo(&window));
        QVERIFY(child<QPushButton>(window,"portfolioPendingDetails")->isVisibleTo(&window));
    }
    prices->release();QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingPending(),2000);
    QVERIFY(!loading->isHidden());QCOMPARE(total->text(),QStringLiteral("—"));
    assets.release();QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("40,000"),3000);
    QVERIFY(loading->isHidden());QVERIFY(tree->isVisibleTo(&window));
    QVERIFY(child<QLabel>(window,"portfolioRefreshNote")->isHidden());vault.lock();
    QCOMPARE(tree->model()->rowCount(),0);
}

void LocalWindowTest::coldUnlockWaitsThroughChangedAssetPriceCycle() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    const QString password="Fake Changed Assets! 2026";
    Vault vault(dir.filePath("fake-changed-assets"));
    QVERIFY(vault.create(password));
    AccountRecord account;account.provider="binance";account.alias="FAKE CHANGING HOLDINGS";
    account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeChangedAssetKey123"},{"secret","FakeChangedAssetSecret123"}},
        StorageMode::EncryptedVault));
    {
        DeferredPortfolioTransport oldAssets(false);
        auto* oldPrices=new DeferredPortfolioTransport(true);
        PortfolioController::Factories oldFactories;
        oldFactories.trading=[&](const AccountRecord&){return &oldAssets;};
        oldFactories.prices=[oldPrices]{return oldPrices;};
        PortfolioController previous(vault,nullptr,oldFactories);
        previous.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(!previous.snapshots().isEmpty()&&!previous.quotes().isEmpty()&&!previous.pricingPending(),3000);
        QString error;
        QTRY_COMPARE_WITH_TIMEOUT(previous.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Ready,3000);
    }
    vault.lock();
    DeferredPortfolioTransport assets(false);
    assets.hold=true;assets.asset="PUMP";assets.quantity="20";
    auto* prices=new DeferredPortfolioTransport(true);
    prices->hold=true;prices->supportedAssets={"USDT","PUMP"};prices->price="2";
    PortfolioController::Factories factories;
    factories.trading=[&](const AccountRecord&){return &assets;};
    factories.prices=[prices]{return prices;};
    PortfolioController portfolio(vault,nullptr,factories);
    AccountModel accounts(&vault);
    LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.resize(1200,800);window.show();
    auto* loading=child<QLabel>(window,"portfolioLoading");
    auto* loadProgress=child<QProgressBar>(window,"portfolioLoadProgress");QVERIFY(loadProgress);
    auto* total=child<QLabel>(window,"portfolioTotal");
    auto* tree=child<QTreeView>(window,"portfolioAssets");
    QVERIFY(vault.unlock(password));
    QTRY_VERIFY_WITH_TIMEOUT(!assets.pending.isEmpty()&&!prices->pending.isEmpty(),2000);
    QVERIFY(!loading->isHidden());QCOMPARE(total->text(),QStringLiteral("—"));

    // The restored holdings start a USDT price cycle. Keep its quote in flight
    // while the current balance switches to PUMP, queuing a second cycle.
    prices->release(true); // Release the catalogue only; future quotes remain held.
    QTRY_VERIFY_WITH_TIMEOUT(prices->quoteRequests.contains("KRW-USDT")&&!prices->pending.isEmpty(),1500);
    QVERIFY(!prices->quoteRequests.contains("KRW-PUMP"));
    assets.release();
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),2000);
    QVERIFY(!loading->isHidden());QCOMPARE(total->text(),QStringLiteral("—"));
    bool watchPendingCycle=true,exposedPartialResult=false;
    connect(&portfolio,&PortfolioController::changed,&window,[&]{
        if(watchPendingCycle&&loading->isHidden())exposedPartialResult=true;
    });
    prices->release(true); // Finish USDT; do not release the newly queued PUMP quote.
    QTRY_VERIFY_WITH_TIMEOUT(prices->quoteRequests.contains("KRW-PUMP")&&!prices->pending.isEmpty(),2000);
    QVERIFY(portfolio.pricingPending());QCOMPARE(loadProgress->value(),1);QCOMPARE(loadProgress->maximum(),3);
    QVERIFY2(!exposedPartialResult,"An intermediate cycle completion must not briefly reveal unpriced current holdings.");
    QVERIFY2(!loading->isHidden(),"The new holding's queued price cycle must keep the first-unlock loader visible.");
    QCOMPARE(total->text(),QStringLiteral("—"));QVERIFY(!tree->isVisibleTo(&window));
    for(auto* chip:window.findChildren<QPushButton*>("portfolioGapChip"))QVERIFY(!chip->isVisibleTo(&window));

    watchPendingCycle=false;
    prices->release();
    QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("40"),2500);
    QVERIFY(loading->isHidden());QVERIFY(tree->isVisibleTo(&window));QCOMPARE(loadProgress->value(),3);
    QCOMPARE(tree->model()->rowCount(),1);
    QCOMPARE(portfolioRow(tree, 0).text(0),QStringLiteral("PUMP"));
    QVERIFY(child<QLabel>(window,"portfolioRefreshNote")->isHidden());
    vault.lock();QCOMPARE(tree->model()->rowCount(),0);
}

void LocalWindowTest::warmUnlockRejectsIncompleteOrChangedScope_data() {
    QTest::addColumn<bool>("replaceKeys");
    QTest::newRow("incomplete-saved-valuation") << false;
    QTest::newRow("replaced-credentials") << true;
}
void LocalWindowTest::warmUnlockRejectsIncompleteOrChangedScope() {
    QFETCH(bool,replaceKeys);
    QTemporaryDir dir;Vault vault(dir.filePath("fake-warm-scope"));
    const QString password="Fake Warm Scope! 2026";QVERIFY(vault.create(password));
    AccountRecord account;account.provider="binance";account.alias="FAKE WARM SCOPE";account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeScopeKey123456"},{"secret","FakeScopeSecret123456"}},StorageMode::EncryptedVault));
    DeferredPortfolioTransport assets(false);auto* prices=new DeferredPortfolioTransport(true);
    PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &assets;};f.prices=[prices]{return prices;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();
    auto* total=child<QLabel>(window,"portfolioTotal");auto* loading=child<QLabel>(window,"portfolioLoading");
    QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("10,000"),3000);
    QString error;QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Ready,3000);
    portfolio.setAutomaticRefresh(false);
    if(!replaceKeys){
        auto snapshots=portfolio.snapshots();auto c=snapshots.first().components.first();
        c.id="unknown-warm-fixture";c.asset="NOQUOTE";c.quantity="0.01";snapshots.first().components.append(c);
        QVERIFY(portfolio.latestLedger()->replaceLatest(snapshots,portfolio.quotes(),&error));
    }
    assets.hold=true;prices->hold=true;
    if(replaceKeys)QVERIFY(vault.replaceCredentials(vault.accounts().first().id,
        {{"api_key","FakeChangedScopeKey123"},{"secret","FakeChangedScopeSecret123"}},&error));
    vault.lock();portfolio.setAutomaticRefresh(true);QVERIFY(vault.unlock(password));
    QTRY_VERIFY_WITH_TIMEOUT(!assets.pending.isEmpty(),2000);
    QVERIFY(!loading->isHidden());QCOMPARE(total->text(),QStringLiteral("—"));
    QVERIFY(child<QLabel>(window,"portfolioRefreshNote")->isHidden());
    QVERIFY(!child<QTreeView>(window,"portfolioAssets")->isVisibleTo(&window));
    vault.lock();
}

void LocalWindowTest::unlockShowsDatedSavedAssetsUntilFreshReadCompletes_data() {
    QTest::addColumn<int>("zeroComponents");
    QTest::newRow("one-holding") << 0;
    QTest::newRow("2002-components") << 2001;
}

void LocalWindowTest::unlockShowsDatedSavedAssetsUntilFreshReadCompletes() {
    QFETCH(int,zeroComponents);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    const QString password = "Fake Dated Portfolio! 2026";
    Vault vault(dir.filePath("fake-dated-vault")); QVERIFY(vault.create(password));
    AccountRecord account; account.provider="binance";account.alias="FAKE DATED HOLDINGS";
    account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeDatedKey123456789"},{"secret","FakeDatedSecret123456789"}},StorageMode::EncryptedVault));
    DeferredPortfolioTransport assets(false);
    auto* prices = new DeferredPortfolioTransport(true);
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) { return &assets; };
    factories.prices = [prices] { return prices; };
    PortfolioController portfolio(vault,nullptr,factories);
    AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.resize(1440,900);window.show();
    portfolio.setAutomaticRefresh(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.snapshots().isEmpty() && !portfolio.quotes().isEmpty() &&
        !portfolio.pricingBusy() && portfolio.connections().first().state=="complete",4000);
    QTRY_COMPARE_WITH_TIMEOUT(child<QLabel>(window,"portfolioTotal")->text(),QStringLiteral("10,000"),3000);
    QString error;
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Ready,3000);
    portfolio.setAutomaticRefresh(false);
    auto savedSnapshots = portfolio.snapshots();
    auto savedQuotes = portfolio.quotes();
    const auto asOf = QDateTime::currentMSecsSinceEpoch()-600000;
    const auto ageEvidence = [asOf](AssetEvidence& evidence) {
        if(evidence.sourceTimeMs>0)evidence.sourceTimeMs=asOf;
        if(evidence.receivedAtMs>0)evidence.receivedAtMs=asOf;
    };
    for(auto& snapshot:savedSnapshots) {
        QVERIFY(!snapshot.connectionBinding.isEmpty());
        snapshot.timeMs=asOf;ageEvidence(snapshot.evidence);
        for(auto& component:snapshot.components)ageEvidence(component.evidence);
    }
    for(auto& quote:savedQuotes)ageEvidence(quote.evidence);
    QCOMPARE(savedSnapshots.size(),1);
    QCOMPARE(savedSnapshots.first().components.size(),1);
    for(int i=0;i<zeroComponents;++i) {
        auto zero=savedSnapshots.first().components.first();
        zero.id=QString("fake-zero-component-%1").arg(i);zero.asset=QString("ZERO%1").arg(i);
        zero.quantity="0";zero.reportedValue.clear();zero.reportedValueCurrency.clear();
        savedSnapshots.first().components.append(zero);
    }
    const auto savedView=recordedPortfolio(savedSnapshots,savedQuotes,portfolio.connections(),QDateTime::currentMSecsSinceEpoch());
    QCOMPARE(savedView.groups.size(),zeroComponents+1);
    QCOMPARE(savedView.summary.netAssets,QStringLiteral("10000"));QVERIFY(savedView.summary.complete);
    for(const auto& group:savedView.groups)if(group.asset!="USDT") {
        QCOMPARE(group.presence,AssetPresence::Zero);QCOMPARE(group.freshTotal,QStringLiteral("0"));
        QCOMPARE(group.holdings.first().state,PortfolioValueState::Fresh);
    }
    {
        auto future=savedSnapshots;future.first().timeMs=QDateTime::currentMSecsSinceEpoch()+60000;
        QVERIFY(recordedPortfolio(future,savedQuotes,portfolio.connections(),QDateTime::currentMSecsSinceEpoch()).groups.isEmpty());
        auto incompleteConnections=portfolio.connections();
        auto missing=incompleteConnections.first();missing.account.id="fake-missing-connection";missing.account.alias="FAKE MISSING ACCOUNT";missing.state="not_read";
        incompleteConnections.append(missing);
        const auto partial=recordedPortfolio(savedSnapshots,savedQuotes,incompleteConnections,QDateTime::currentMSecsSinceEpoch());
        QVERIFY(!partial.summary.complete);QCOMPARE(partial.summary.knownSubtotal,QStringLiteral("10000"));
        QCOMPARE(partial.summary.missingAccounts.size(),1);
        auto unsupported=savedSnapshots;auto unknown=unsupported.first().components.first();
        unknown.id="fake-unsupported-positive";unknown.asset="NOQUOTE";unknown.quantity="1";
        unsupported.first().components.append(unknown);
        const auto gap=recordedPortfolio(unsupported,savedQuotes,portfolio.connections(),QDateTime::currentMSecsSinceEpoch());
        QVERIFY(!gap.summary.complete);QCOMPARE(gap.summary.knownSubtotal,QStringLiteral("10000"));
        QCOMPARE(gap.summary.unvaluedGroups,1); // No saved evidence means no invented value.
    }
    QVERIFY2(portfolio.latestLedger()->replaceLatest(savedSnapshots,savedQuotes,&error),qPrintable(error));
    vault.lock();
    portfolio.setAutomaticRefresh(true);
    assets.hold=true;assets.quantity="20";prices->hold=true;prices->price="2000";

    auto* tree=child<QTreeView>(window,"portfolioAssets");
    auto* total=child<QLabel>(window,"portfolioTotal");
    auto* note=child<QLabel>(window,"portfolioRefreshNote");
    auto* loading=child<QLabel>(window,"portfolioLoading");
    auto* totalLabel=child<QLabel>(window,"portfolioTotalLabel");
    QElapsedTimer elapsed;elapsed.start();
    QVERIFY(vault.unlock(password));
    const auto unlockMs=elapsed.restart();
    QTRY_VERIFY_WITH_TIMEOUT(note->text().contains("마지막 확인") && total->text()=="10,000",1500);
    const auto renderedMs=elapsed.elapsed();
    qInfo().noquote()<<QString("Synthetic encrypted cache [%1]: unlock operation %2 ms; unlock completion to dated portfolio display %3 ms")
        .arg(QString::fromLatin1(QTest::currentDataTag())).arg(unlockMs).arg(renderedMs);
    QVERIFY(renderedMs<1500);
    QCOMPARE(totalLabel->text(),QStringLiteral("마지막 확인 자산"));
    QVERIFY(note->text().contains(QDateTime::fromMSecsSinceEpoch(asOf).toString("yyyy-MM-dd HH:mm:ss")));
    QVERIFY(!note->isHidden());QVERIFY(loading->isHidden());
    QTRY_VERIFY_WITH_TIMEOUT(!assets.pending.isEmpty() && !prices->pending.isEmpty(),1500);
    QCOMPARE(portfolio.snapshots(),savedSnapshots);
    QCOMPARE(portfolio.quotes(),savedQuotes);
    const auto live=AssetValuation::value(portfolio.snapshots(),portfolio.quotes(),"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs);
    QVERIFY(!live.complete); // Dated presentation never promotes stale evidence to current truth.
    QCOMPARE(tree->model()->rowCount(),zeroComponents+1);
    QCOMPARE(portfolioRow(tree, 0).text(0),QStringLiteral("USDT"));
    for(int i=1;i<tree->model()->rowCount();++i)QVERIFY(portfolioRow(tree, i).isHidden());
    tree->setCurrentIndex(portfolioRow(tree, 0).modelIndex());
    auto* detail=child<AssetDetailPanel>(window,"assetDetail");
    auto* detailSub=child<QLabel>(*detail,"assetDetailHeadlineSub");
    QVERIFY(detailSub->text().contains("마지막 확인"));
    QVERIFY(child<QLabel>(*detail,"assetDetailHolding0")->text().contains("마지막 확인"));
    for(auto* label:detail->findChildren<QLabel*>())QVERIFY(!label->text().contains("최신 평가"));
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("portfolio-dated-unlock.png")));
    prices->release();
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),2000);
    QCOMPARE(total->text(),QStringLiteral("10,000")); // New price + old balance never changes the saved figure.
    QVERIFY(note->text().contains("마지막 확인"));
    // R3: the dated view and the current inputs now differ (price 2000 vs saved 1000).
    // No export may pair the saved headline with current rows.
    QVERIFY(window.valuationDiagnostics().isEmpty());
    QCOMPARE(total->text(),QStringLiteral("10,000"));QVERIFY(note->text().contains("마지막 확인"));
    assets.release();
    QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("40,000"),3000);
    QTRY_VERIFY_WITH_TIMEOUT(note->isHidden(),1500);
    QVERIFY(loading->isHidden());
    QCOMPARE(totalLabel->text(),QStringLiteral("순자산"));
    QVERIFY(AssetValuation::value(portfolio.snapshots(),portfolio.quotes(),"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs).complete);
    {   // Once the current valuation is on screen, the export describes exactly it.
        const auto bundle=window.valuationDiagnostics();QVERIFY(!bundle.isEmpty());
        const auto result=bundle.value("keefi_result").toObject();const auto screen=result.value("screen").toObject();
        QCOMPARE(screen.value("figure").toString(),total->text());QCOMPARE(screen.value("label").toString(),totalLabel->text());
        QVERIFY(!screen.value("displayed_saved_view").toBool());
        QCOMPARE(result.value("valuation").toObject().value("known_subtotal").toString(),QStringLiteral("40000"));
        QCOMPARE(bundle.value("snapshots").toArray().first().toObject().value("components").toArray().first().toObject().value("quantity").toString(),QStringLiteral("20"));
    }
    tree->setCurrentIndex(portfolioRow(tree, 0).modelIndex());
    QVERIFY(!child<QLabel>(*detail,"assetDetailHeadlineSub")->text().contains("마지막 확인"));
    vault.lock();
    QCOMPARE(tree->model()->rowCount(),0);QCOMPARE(total->text(),QStringLiteral("—"));
    QVERIFY(note->text().isEmpty());QVERIFY(note->isHidden());QVERIFY(loading->isHidden());
    QVERIFY(portfolio.snapshots().isEmpty());QVERIFY(portfolio.quotes().isEmpty());
    for(auto* label:detail->findChildren<QLabel*>()) {
        QVERIFY(!label->text().contains("FAKE DATED"));QVERIFY(!label->text().contains("40,000"));
    }
}

void LocalWindowTest::unlockWithoutPricedCacheLoadsThenExposesSettledGaps_data() {
    QTest::addColumn<bool>("savedBalances");
    QTest::addColumn<QString>("outcome");
    QTest::newRow("no-cache-price-failure") << false << QString("price-failure");
    QTest::newRow("saved-unpriced-price-failure") << true << QString("price-failure");
    QTest::newRow("no-cache-unsupported-price") << false << QString("unsupported");
    QTest::newRow("no-cache-account-failure") << false << QString("account-failure");
    QTest::newRow("saved-unpriced-account-failure") << true << QString("account-failure");
}

void LocalWindowTest::unlockWithoutPricedCacheLoadsThenExposesSettledGaps() {
    QFETCH(bool,savedBalances);QFETCH(QString,outcome);
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    const QString password="Fake Pending Portfolio! 2026";
    Vault vault(dir.filePath("fake-pending-vault"));QVERIFY(vault.create(password));
    AccountRecord account;account.provider="binance";account.alias="FAKE LOADING HOLDINGS";
    account.environment="live";account.market="spot";account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FakeLoadingKey123456789"},{"secret","FakeLoadingSecret123456789"}},StorageMode::EncryptedVault));
    if(savedBalances) {
        DeferredPortfolioTransport initialAssets(false);
        PortfolioController::Factories initialFactories;
        initialFactories.trading=[&](const AccountRecord&){return &initialAssets;};
        initialFactories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController initial(vault,nullptr,initialFactories);initial.setAutomaticRefresh(true);
        QTRY_COMPARE_WITH_TIMEOUT(initial.snapshots().size(),1,2500);
        QString error;
        QTRY_COMPARE_WITH_TIMEOUT(initial.checkpointRecords(&error),PortfolioController::RecordCheckpoint::Ready,3000);
        QVERIFY(initial.latestLedger()->quotes().isEmpty());
    }
    vault.lock();
    DeferredPortfolioTransport assets(false);assets.hold=true;assets.fail=outcome=="account-failure";
    auto* prices=new DeferredPortfolioTransport(true);prices->hold=true;
    prices->fail=outcome=="price-failure";prices->supported=outcome!="unsupported";
    PortfolioController::Factories factories;
    factories.trading=[&](const AccountRecord&){return &assets;};factories.prices=[prices]{return prices;};
    PortfolioController portfolio(vault,nullptr,factories);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1200,800);window.show();
    QVERIFY(vault.unlock(password));
    auto* loading=child<QLabel>(window,"portfolioLoading");
    auto* note=child<QLabel>(window,"portfolioRefreshNote");
    auto* total=child<QLabel>(window,"portfolioTotal");
    auto* tree=child<QTreeView>(window,"portfolioAssets");
    QTRY_VERIFY_WITH_TIMEOUT(!loading->isHidden()&&!assets.pending.isEmpty(),1500);
    QCOMPARE(total->text(),QStringLiteral("—"));
    QVERIFY(!tree->isVisibleTo(&window));
    QVERIFY(!child<QLabel>(window,"portfolioListEmpty")->isVisibleTo(&window));
    QVERIFY(!note->text().contains("마지막 확인"));
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty()&&!savedBalances&&outcome=="price-failure")
        QVERIFY(window.grab().save(QDir(preview).filePath("portfolio-first-unlock-loading.png")));
    assets.release();
    if(outcome=="account-failure") {
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("failed"),2000);
        if(savedBalances){
            auto* partial=child<QPushButton>(window,"portfolioPendingDetails");
            QTRY_VERIFY_WITH_TIMEOUT(partial->isVisibleTo(&window),1500);
            QVERIFY(!loading->isHidden());QVERIFY(loading->text().contains("불러오지 못했어요"));
            partial->click(); // Explicitly open available data while pricing is still pending.
        }else{
            QVERIFY(!portfolio.pricingPending()); // A settled failure stays explicit, never a permanent spinner.
            QVERIFY(child<QLabel>(window,"connectionIssue")->text().contains("실패"));
        }
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state,QString("complete"),2000);
        QTRY_VERIFY_WITH_TIMEOUT(!prices->pending.isEmpty(),1500);
        QVERIFY(!loading->isHidden());QCOMPARE(total->text(),QStringLiteral("—"));
        prices->release();
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingPending(),2500);
    }
    QTRY_VERIFY_WITH_TIMEOUT(loading->isHidden()&&note->isHidden(),1500);
    QVERIFY(tree->isVisibleTo(&window));
    QVERIFY(total->text()!=QStringLiteral("0"));
    const auto shownChip=[&](const QString& prefix){
        for(auto* chip:window.findChildren<QPushButton*>("portfolioGapChip"))
            if(chip->isVisibleTo(&window)&&chip->text().startsWith(prefix))return true;
        return false;
    };
    if(outcome=="account-failure"){
        QVERIFY(shownChip(QStringLiteral("미확인 계좌")));
    }else{
        auto* hide=child<QPushButton>(window,"hideUnvaluedBalances");QVERIFY(hide->isChecked());
        QCOMPARE(total->text(),QStringLiteral("—"));QVERIFY(!shownChip(QStringLiteral("가격 미확인")));
        const auto screen=window.valuationDiagnostics()["keefi_result"].toObject()["screen"].toObject();
        QVERIFY(screen["unvalued_groups"].toInt()>0); // Hiding an unknown value does not erase the holding.
        hide->setChecked(false);QTRY_VERIFY(shownChip(QStringLiteral("가격 미확인")));QCOMPARE(total->text(),QStringLiteral("—"));
    }
    if(outcome=="price-failure")QVERIFY(portfolio.pricingStatus().contains("실패"));
    if(outcome=="unsupported")QVERIFY(portfolio.pricingStatus().contains("미지원"));
    QVERIFY(!child<QLabel>(window,"portfolioSummary")->text().contains("FAKE_PRIVATE"));
    vault.lock();QVERIFY(loading->isHidden());QVERIFY(note->text().isEmpty());QCOMPARE(tree->model()->rowCount(),0);
}

void LocalWindowTest::binanceSharedCredentialsPreserveOtherMarketConnection() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Vault vault(QDir(QDir(directory.path()).canonicalPath()).filePath("fake-shared-ui.bin")); QVERIFY(vault.create("Fake Shared UI! 2026"));
    AccountModel accounts(&vault); LocalWindow window(vault, accounts); window.setSecurityStatus(true); window.show();
    AccountRecord spot; spot.provider = "binance"; spot.alias = "FAKE SPOT"; spot.market = "spot";
    spot.environment = "live"; spot.currency = "USDT";
    QString error; QVERIFY2(vault.addAccount(spot, {{"api_key", "FAKESHAREDKEY2026"}, {"secret", "FAKESHAREDSECRET2026"}}, StorageMode::EncryptedVault, &error), qPrintable(error));
    spot = vault.accounts().first();
    child<QPushButton>(window, "addAccount")->click();
    auto* dialog = child<AccountDialog>(window, "accountDialog"); QVERIFY(dialog);
    auto* provider = child<QComboBox>(*dialog, "providerSelector"); provider->setCurrentIndex(provider->findData("binance"));
    auto* market = child<QComboBox>(*dialog, "marketSelector"); market->setCurrentIndex(market->findData("usdm_futures"));
    auto* environment = child<QComboBox>(*dialog, "environmentSelector");
    QCOMPARE(environment->count(), 1); QCOMPARE(environment->currentData().toString(), QString("live"));
    QCOMPARE(environment->findData("testnet"), -1); QCOMPARE(environment->findData("demo"), -1);
    auto* source = child<QComboBox>(*dialog, "credentialSourceSelector"); QVERIFY(source->findData(spot.id) >= 0);
    child<QLineEdit>(*dialog, "accountAlias")->setText("FAKE FUTURES");
    child<QLineEdit>(*dialog, "credential_secret")->setText("FAKE-DISCARDED-INPUT");
    source->setCurrentIndex(source->findData(spot.id));
    QVERIFY(child<QLineEdit>(*dialog, "credential_secret")->text().isEmpty());
    QVERIFY(!child<QLineEdit>(*dialog, "credential_secret")->isVisible());
    QVERIFY(!dialog->findChild<QComboBox*>("storageSelector"));
    QVERIFY(child<QPushButton>(*dialog, "saveAccount")->isEnabled());
    child<QPushButton>(*dialog, "saveAccount")->click(); processDeletion();
    QCOMPARE(vault.accounts().size(), 2);
    AccountRecord futures;
    for (const auto& account : vault.accounts()) if (account.market == "usdm_futures") futures = account;
    QVERIFY(!futures.id.isEmpty()); QCOMPARE(futures.credentialRef, spot.credentialRef);
    auto* list = child<QListView>(window, "accountList");
    QCOMPARE(list->model()->rowCount(), 1);
    auto* markets = child<QComboBox>(window, "connectionMarketSelector");
    markets->setCurrentIndex(markets->findData(futures.id));
    child<QPushButton>(window, "replaceCredentials")->click(); dialog = child<AccountDialog>(window, "accountDialog"); QVERIFY(dialog);
    const auto impact = child<QLabel>(*dialog, "sharedCredentialHint")->text();
    QVERIFY(impact.contains("FAKE SPOT")); QVERIFY(impact.contains("FAKE FUTURES"));
    QVERIFY(child<QLineEdit>(*dialog, "credential_api_key")->text().isEmpty());
    QVERIFY(child<QLineEdit>(*dialog, "credential_secret")->text().isEmpty());
    dialog->reject(); processDeletion();
    child<QPushButton>(window, "removeAccount")->click();
    auto* confirm = child<QMessageBox>(window, "removeAccountConfirmation"); QVERIFY(confirm);
    QVERIFY(confirm->text().contains("FAKE SPOT")); QVERIFY(confirm->text().contains("유지"));
    child<QPushButton>(*confirm, "confirmRemoveAccount")->click(); processDeletion();
    QCOMPARE(vault.accounts().size(), 1); QCOMPARE(vault.accounts().first().id, spot.id);
    QCOMPARE(vault.accounts().first().credentialRef, spot.credentialRef);
    // Both markets offer live only; only the other market can reuse this key.
    AccountDialog connection(true); connection.setCredentialSources(vault.accounts());
    provider = child<QComboBox>(connection, "providerSelector"); provider->setCurrentIndex(provider->findData("binance"));
    market = child<QComboBox>(connection, "marketSelector"); market->setCurrentIndex(market->findData("usdm_futures"));
    environment = child<QComboBox>(connection, "environmentSelector");
    QCOMPARE(environment->count(), 1); QCOMPARE(environment->currentData().toString(), QString("live"));
    QVERIFY(child<QComboBox>(connection, "credentialSourceSelector")->findData(spot.id) >= 0);
    market->setCurrentIndex(market->findData("spot"));
    QCOMPARE(environment->count(), 1); QCOMPARE(environment->currentData().toString(), QString("live"));
    QCOMPARE(child<QComboBox>(connection, "credentialSourceSelector")->count(), 1);
    QCOMPARE(environment->findData("testnet"), -1); QCOMPARE(environment->findData("demo"), -1);
}

void LocalWindowTest::loginFreeEntryAndSecurityGate() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Vault vault(QDir(QDir(directory.path()).canonicalPath()).filePath("test-vault.bin"));
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    QVERIFY(window.windowTitle().startsWith("KEEFI"));
    QCOMPARE(child<QLabel>(window, "brandLabel")->text(), QString("KEEFI"));
    QVERIFY(!child<QPushButton>(window, "addAccount")->isEnabled());
    QVERIFY(!child<QPushButton>(window, "createVault")->isEnabled());
    window.setSecurityStatus(true);
    QVERIFY(child<QPushButton>(window, "addAccount")->isEnabled());
    QVERIFY(child<QPushButton>(window, "createVault")->isEnabled());
    QVERIFY(!child<QPushButton>(window, "replaceCredentials")->isEnabled());
    QVERIFY(!child<QPushButton>(window, "removeAccount")->isEnabled());
    QCOMPARE(accounts.rowCount(), 0);
    const auto balance = child<QLabel>(window, "balanceState")->text();
    // The current workbench clears account-specific content until a row is selected.
    QVERIFY(balance.isEmpty());
    QVERIFY(!balance.contains('0'));
    for (auto* item : window.findChildren<QPushButton*>()) {
        QVERIFY(!item->text().contains(QStringLiteral("로그인")));
        QVERIFY(!item->text().contains(QStringLiteral("회원 가입")));
        QVERIFY(!item->text().contains(QStringLiteral("매수")));
        QVERIFY(!item->text().contains(QStringLiteral("매도")));
    }
    QVERIFY(child<QLabel>(window, "stageBanner")->text().contains(QStringLiteral("기본은 조회전용")));
    QVERIFY(!window.findChild<QPushButton*>("openTrading"));
}

void LocalWindowTest::vaultDialogKeepsModalInputBoundaryWithoutNativeSheet() {
    QTemporaryDir directory;QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    Vault vault(directory.filePath("synthetic-modal-vault"));
    QVERIFY(vault.create("Synthetic Modal Vault! 2026"));vault.lock();AccountModel accounts(&vault);
    LocalWindow window(vault,accounts);window.setSecurityStatus(true);window.show();
    auto* unlock=window.findChild<QPushButton*>("unlockVault");QVERIFY(unlock);QVERIFY(unlock->isEnabled());
    unlock->click();auto* dialog=window.findChild<VaultDialog*>("vaultDialog");QVERIFY(dialog);QVERIFY(dialog->isVisible());
    QVERIFY(dialog->isModal());QCOMPARE(QApplication::activeModalWidget(),dialog);
#ifdef Q_OS_MACOS
    QCOMPARE(dialog->windowModality(),Qt::ApplicationModal);
#else
    QCOMPARE(dialog->windowModality(),Qt::WindowModal);
#endif
    QCOMPARE(dialog->windowType(),Qt::Dialog); // A Cocoa sheet adds a blocking native close transition.
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty()){
        QVERIFY(QDir().mkpath(preview));
        for(auto theme:{ThemeMode::Light,ThemeMode::Dark}){
            KeefiTheme::instance()->setMode(theme);QTest::qWait(40);
            QVERIFY(dialog->grab().save(QDir(preview).filePath(theme==ThemeMode::Light?"vault-modal-light.png":"vault-modal-dark.png")));
        }
    }
    auto* password=dialog->findChild<QLineEdit*>("currentPassword");QVERIFY(password);password->setText("Synthetic Modal Vault! 2026");
    bool clearedBeforeFinished=false;
    connect(dialog,&QDialog::finished,&window,[&]{clearedBeforeFinished=password->text().isEmpty()&&password->echoMode()==QLineEdit::Password;});
    dialog->reject();QVERIFY(clearedBeforeFinished);QVERIFY(!vault.isUnlocked());
    QTRY_VERIFY(!QApplication::activeModalWidget());
}

void LocalWindowTest::publicCatalogAutoLoadDisabledDoesNotRequestOnFirstShow() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.filePath("settings"));
    Vault vault(directory.filePath("synthetic-no-autoload-vault")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    NoNetworkBrokerTransport transport; PublicMarketCatalog markets(&transport);
    LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    window.setPublicMarketCatalog(&markets, false);
    window.showVaultGate();
    QCOMPARE(transport.sent, 0);
    window.show();
    QTest::qWait(30); // Includes the queued presentation callback after the first show.
    QVERIFY(window.isVisible());
    QCOMPARE(transport.sent, 0);
    markets.refresh();
    QCOMPARE(transport.sent, 1); // An explicit refresh still starts the public request.
}

void LocalWindowTest::initialLockIsSilent() {
    QTemporaryDir dir;
    Vault vault(dir.filePath("fake-lock.bin"));
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    auto* feedback = child<QLabel>(window, "localFeedback");
    QVERIFY(feedback->text().isEmpty());
    window.setSecurityStatus(false, "initial-check");
    QVERIFY2(feedback->text().isEmpty(), qPrintable(feedback->text()));
    window.setSecurityStatus(true);
    QVERIFY(feedback->text().isEmpty());
    QVERIFY(vault.create("Fake Lock UI! 2026"));
    window.lockAndClear();
    QVERIFY(!vault.isUnlocked());
    QVERIFY(!feedback->text().contains(QStringLiteral("인증정보")));
}

void LocalWindowTest::publicObservationChoicesStayOffAndReleaseHiddenDemands(){
    QTemporaryDir dir;QVERIFY(dir.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));QSettings profile(dir.filePath("public-study.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-only-vault"));AccountModel accounts(&vault);
    PublicMarketTransport catalogTransport;PublicMarketCatalog catalog(&catalogTransport);
    NoNetworkBrokerTransport metricTransport,studyTransport;MarketMetrics metrics(&metricTransport);ChartStudyFeed studies(&studyTransport);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    LocalWindow window(vault,accounts);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.setPublicMarketCatalog(&catalog,false);window.setMarketMetrics(&metrics);window.setChartStudyFeed(&studies);window.resize(1440,900);window.show();catalog.refresh();
    QTRY_COMPARE(catalog.rows().size(),2);
    auto* breadth=child<QCheckBox>(window,"exploreObservation_breadth");auto* relative=child<QCheckBox>(window,"exploreObservation_relative");
    auto* view=child<QComboBox>(window,"exploreView");QVERIFY(breadth&&relative&&view);QVERIFY(!breadth->isChecked());QVERIFY(!relative->isChecked());QCOMPARE(studies.demandCount(),0);
    auto* summary=window.findChild<ExploreCoinSummary*>();QVERIFY(summary);emit summary->coinSelected("PUMP");
    relative->setChecked(true);QTRY_COMPARE(studies.demandCount(),2);QVERIFY(profile.value("explore/observations/relative").toBool());
    const QString identity="coin:upbit:KRW-PUMP";
    studies.setDemands("synthetic-other-owner",{{WatchlistWorkspaceList::identityFromKey(identity),3600000,5}});
    QCOMPARE(studies.ownerCount(identity,3600000),2);
    view->setCurrentIndex(view->findData("map"));QTRY_COMPARE(studies.ownerCount(identity,3600000),1);QVERIFY(!relative->isVisible());
    view->setCurrentIndex(view->findData("flow"));QTRY_COMPARE(studies.ownerCount(identity,3600000),2);
    breadth->setChecked(true);QVERIFY(child<QWidget>(window,"exploreBreadthRow")->isHidden());
    QVERIFY(window.property("publicBreadthSummary").toString().contains(QStringLiteral("KOSPI")));
    window.hide();QTRY_COMPARE(studies.ownerCount(identity,3600000),1);QVERIFY(!window.findChild<MarketBreadthMonitor*>()->enabled());
    studies.clearOwner("synthetic-other-owner");QCOMPARE(studies.demandCount(),0);QVERIFY(!vault.exists());
}

void LocalWindowTest::rankingStudySelectionOwnsOnlyVisibleRows(){
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    QSettings profile(dir.filePath("ranking-study.ini"),QSettings::IniFormat);
    Vault vault(dir.filePath("synthetic-only-vault")); AccountModel accounts(&vault);
    PublicMarketTransport catalogTransport; PublicMarketCatalog catalog(&catalogTransport);
    FakeBoardTransport boardTransport; PublicEquityBoard board(&boardTransport);
    NoNetworkBrokerTransport studyTransport,metadataTransport,metricTransport; ChartStudyFeed studies(&studyTransport); MarketMetrics metrics(&metricTransport);
    PortfolioController::Factories factories; factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    LocalWindow window(vault,accounts); window.setChartSettings(&profile);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog,false); window.setPublicEquityBoard(&board);
    window.setChartInstrumentTransport(&metadataTransport); window.setChartStudyFeed(&studies); window.setMarketMetrics(&metrics);
    window.resize(1440,900); window.show(); catalog.refresh(); board.refresh();
    auto* view=child<QComboBox>(window,"exploreView");
    auto* choice=child<QComboBox>(window,"exploreRankingStudy");
    QCOMPARE(choice->currentData().toString(),QString("none")); QVERIFY(!choice->isVisible());
    QCOMPARE(studies.demandCount(),0);
    view->setCurrentIndex(view->findData("list")); QTRY_VERIFY(choice->isVisible());
    auto* coins=child<SpotlightCard>(window,"spotlightCard_coin");
    auto* korea=child<SpotlightCard>(window,"spotlightCard_kr");
    auto* us=child<SpotlightCard>(window,"spotlightCard_us");
    QTRY_VERIFY(!coins->visibleItems().isEmpty()&&!korea->visibleItems().isEmpty()&&!us->visibleItems().isEmpty());
    choice->setCurrentIndex(choice->findData("volume"));
    const QString identity="coin:upbit:KRW-BTC";
    QTRY_COMPARE(studies.ownerCount(identity,3600000),1);
    QCOMPARE(profile.value("explore/ranking/study").toString(),QString("volume"));
    QTRY_COMPARE(korea->visibleItems().first().study,QString("금고 열기 필요"));
    QTRY_COMPARE(us->visibleItems().first().study,QString("금고 열기 필요"));
    studies.setDemands("synthetic-other-owner",{{WatchlistWorkspaceList::identityFromKey(identity),3600000,5}});
    QCOMPARE(studies.ownerCount(identity,3600000),2);
    view->setCurrentIndex(view->findData("map")); QTRY_COMPARE(studies.ownerCount(identity,3600000),1);
    QVERIFY(!choice->isVisible());
    auto* market=child<QComboBox>(window,"exploreMarketSection");market->setCurrentIndex(market->findData("coin"));
    view->setCurrentIndex(view->findData("list"));
    QTRY_VERIFY(coins->isVisible()&&korea->isVisible()&&us->isVisible());
    QTRY_COMPARE(studies.ownerCount(identity,3600000),2);
    view->setCurrentIndex(view->findData("flow")); QTRY_COMPARE(studies.ownerCount(identity,3600000),1);
    QVERIFY(!choice->isVisible());
    view->setCurrentIndex(view->findData("list")); QTRY_COMPARE(studies.ownerCount(identity,3600000),2);
    window.hide(); QTRY_COMPARE(studies.ownerCount(identity,3600000),1);
    studies.clearOwner("synthetic-other-owner"); QCOMPARE(studies.demandCount(),0);
    QVERIFY(!vault.exists());
}

void LocalWindowTest::exploreControlsStayPutWhenSwitchingViews() {
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("fake-toolbar.bin")); AccountModel accounts(&vault);
    PortfolioController::Factories f; f.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, f);
    LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    PublicMarketTransport transport; PublicMarketCatalog markets(&transport);
    window.setPublicMarketCatalog(&markets, false); window.setSecurityStatus(true); window.show();
    auto* view = child<SegmentedComboBox>(window, "exploreView");
    auto* section = child<SegmentedComboBox>(window, "exploreMarketSection");
    auto* viewTabs=child<QWidget>(window,"exploreViewTabs");
    auto* marketTabs=child<QWidget>(window,"exploreMarketTabs");
    const auto bounds = [&window](QWidget* w) { return QRect(w->mapTo(&window, QPoint()), w->size()); };
    for (const QSize size : {QSize(1440, 900), QSize(1920, 1080), QSize(1280, 800)}) {
        window.resize(size); view->setCurrentIndex(view->findData("map")); QTest::qWait(80);
        const QRect viewBounds = bounds(viewTabs);
        for (const auto& route : {QString("list"),QString("map"),QString("list"),QString("map")}) {
            view->setCurrentIndex(view->findData(route));QTest::qWait(30);
            // Inspect the visible tabs, not the hidden legacy combo's geometry.
            QCOMPARE(bounds(viewTabs).top(),viewBounds.top());
            QCOMPARE(bounds(viewTabs).size(),viewBounds.size());
            if(size.width()>=1440)QCOMPARE(bounds(viewTabs),viewBounds);
            QCOMPARE(marketTabs->isVisible(),route=="map");
            QVERIFY(bounds(viewTabs).right()<window.width());
            if(marketTabs->isVisible())QVERIFY(!bounds(viewTabs).intersects(bounds(marketTabs)));
        }
    }
}

void LocalWindowTest::todayFitsTheNativeWindowWithLoadingAndPartialCalendarFailure_data() {
    QTest::addColumn<QSize>("clientSize");
    QTest::addColumn<bool>("dark");
    // The installed macOS window's 1440×900 frame includes a 32px title bar.
    // Exercise its 868px client as well as the design's unframed 900px canvas.
    for (bool dark : {false, true})
        for (const QSize size : {QSize(1440, 868), QSize(1440, 900), QSize(1920, 1048), QSize(1280, 768)})
            QTest::newRow(qPrintable(QString("%1-%2x%3").arg(dark ? "dark" : "light").arg(size.width()).arg(size.height()))) << size << dark;
}

void LocalWindowTest::todayFitsTheNativeWindowWithLoadingAndPartialCalendarFailure() {
    QFETCH(QSize, clientSize); QFETCH(bool, dark);
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("never-created-today-vault")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    NoNetworkBrokerTransport catalogTransport, metricTransport;
    CoinWindowPublicTransport calendarTransport;
    PublicMarketCatalog catalog(&catalogTransport); MarketMetrics metrics(&metricTransport);
    OfficialCalendar calendar(&calendarTransport);
    LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    window.setPublicMarketCatalog(&catalog, false); window.setMarketMetrics(&metrics);
    window.setOfficialCalendar(&calendar); window.setSecurityStatus(true);
    // Construction restores the window's saved theme; choose the requested
    // capture theme afterwards instead of only naming a light image "dark".
    KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
    QCOMPARE(KeefiTheme::instance()->isDark(), dark);
    window.resize(clientSize); window.show();
    child<QTabWidget>(window, "portfolioPages")->setCurrentWidget(child<QWidget>(window, "explorePage"));
    auto* selector = child<QComboBox>(window, "exploreView"); selector->setCurrentIndex(selector->findData("flow"));
    calendar.setActive(true);
    const auto now = QDateTime::currentDateTimeUtc();
    const auto published = now.addSecs(-60).toString("yyyyMMdd'T'HHmmss'Z'").toLatin1();
    QByteArray events;
    for (int i = 1; i <= 10; ++i) {
        const auto at = now.addSecs(i * 3600).toString("yyyyMMdd'T'HHmmss'Z'").toLatin1();
        events += "BEGIN:VEVENT\r\nUID:synthetic-today-" + QByteArray::number(i) +
            "\r\nSUMMARY:Consumer Price Index\r\nDTSTART:" + at + "\r\nDTSTAMP:" + published + "\r\nSEQUENCE:0\r\nEND:VEVENT\r\n";
    }
    const QDate kstDate = now.toTimeZone(QTimeZone("Asia/Seoul")).date();
    const QString weekday = QStringLiteral("월화수목금토일").mid(kstDate.dayOfWeek() - 1, 1);
    const auto bok = QString("<h3>%1년</h3><table id=\"tableId\"><tbody><tr><th scope=\"row\">%2월 %3일(%4)</th></tr></tbody></table>")
        .arg(kstDate.year()).arg(kstDate.month(), 2, 10, QChar('0')).arg(kstDate.day(), 2, 10, QChar('0')).arg(weekday).toUtf8();
    for (int i = 0; i < 4; ++i) {
        QTRY_VERIFY(calendarTransport.requests.size() > i);
        const auto request = calendarTransport.requests.at(i); HttpResult result; result.status = 503;
        if (request.url.host().contains("bls.gov")) { result.status = 200; result.body = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + events + "END:VCALENDAR\r\n"; }
        else if (request.url.host().contains("bok.or.kr")) { result.status = 200; result.body = bok; }
        emit calendarTransport.finished(calendarTransport.token, result);
    }
    const auto blsFeed = calendar.weekFeed(OfficialCalendarSource::Bls);
    const auto bokFeed = calendar.weekFeed(OfficialCalendarSource::Bok);
    QVERIFY2(blsFeed.available, qPrintable(blsFeed.reason)); QCOMPARE(blsFeed.events.size(), 10);
    QVERIFY2(bokFeed.available, qPrintable(bokFeed.reason)); QCOMPARE(bokFeed.events.size(), 1);
    auto* scroll = child<QScrollArea>(window, "marketFlowPage");
    auto* today = child<ExploreTodayView>(window, "todayDashboard");
    auto* schedule = today ? today->calendar() : nullptr;
    auto* progress = child<QWidget>(window, "marketFlowProgress");
    QVERIFY(scroll && today && schedule && progress);
    QVERIFY(progress->isHidden()); // Dev86: no full-width progress line above the first card.
    QTRY_VERIFY(child<QLabel>(*schedule, "todayCalendarStatus")->text().contains(QStringLiteral("받지 못했어요")));
    QTRY_VERIFY(schedule->findChildren<QWidget*>("todayCalendarRow").size() >= 2);
    QTRY_VERIFY(child<QLabel>(*schedule, "todayCalendarDateOnly"));
    auto* next = child<QPushButton>(*schedule, "todayCalendarNext");
    QVERIFY(next && next->isEnabled());
    QTest::qWait(80);
    qInfo().noquote() << QString("TODAY_GEOMETRY client=%1x%2 viewport=%3x%4 body=%5 today=%6 calendar=%7 scrollY=%8")
        .arg(window.width()).arg(window.height()).arg(scroll->viewport()->width()).arg(scroll->viewport()->height())
        .arg(scroll->widget()->height()).arg(today->height()).arg(schedule->height()).arg(scroll->verticalScrollBar()->maximum());
    const auto bounds = [](QWidget* widget, QWidget* parent) { return QRect(widget->mapTo(parent, QPoint()), widget->size()); };
    if (clientSize.width() >= 1440) {
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QVERIFY2(scroll->viewport()->rect().contains(bounds(next, scroll->viewport())), "Calendar paging must be fully visible without scrolling at the supported desktop size");
        for (auto* panel : {child<QWidget>(window, "todayPrivatePanel"), child<QWidget>(window, "todayMetricPanel"), static_cast<QWidget*>(schedule)}) {
            QVERIFY(panel); QVERIFY(scroll->viewport()->rect().contains(bounds(panel, scroll->viewport())));
        }
    } else {
        // Smaller windows may scroll, but their final control stays reachable.
        scroll->ensureWidgetVisible(next); QCoreApplication::processEvents();
        QVERIFY(scroll->viewport()->rect().contains(bounds(next, scroll->viewport())));
    }
    for (auto* row : schedule->findChildren<QWidget*>("todayCalendarRow"))
        QVERIFY2(row->geometry().bottom() < next->y(), "Schedule rows must not overlap paging controls");
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    QVERIFY(!vault.exists());
    QVERIFY(child<QLabel>(window, "todayPrivateTotal")->text().isEmpty());
    const auto output = qEnvironmentVariable("KEEFI_TODAY_LAYOUT_RENDER_DIR");
    if (!output.isEmpty()) {
        QCOMPARE(KeefiTheme::instance()->isDark(), dark);
        QVERIFY(QDir().mkpath(output));
        QVERIFY(window.grab().save(QDir(output).filePath(QString("today-%1-%2x%3.png").arg(dark ? "dark" : "light").arg(clientSize.width()).arg(clientSize.height()))));
    }
}

void LocalWindowTest::spotlightAndWatchlistNeverOverlap() {
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("fake-layout.bin")); AccountModel accounts(&vault);
    PortfolioController::Factories f; f.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, f);
    LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    PublicMarketTransport transport; PublicMarketCatalog markets(&transport);
    window.setPublicMarketCatalog(&markets, false); window.setSecurityStatus(true);
    child<QComboBox>(window, "exploreView")->setCurrentIndex(1); window.show();
    auto* scroll = child<QScrollArea>(window, "spotlightScroll");
    auto* watchlist = window.findChild<ExploreWatchlist*>();
    QVERIFY(scroll); QVERIFY(watchlist);
    for (auto* card : window.findChildren<SpotlightCard*>()) {
        SpotlightMode mode{"gainers", QStringLiteral("상승률 ↓"), {}};
        for (int i = 0; i < 10; ++i) mode.items.append({QString::number(i), QStringLiteral("삼성바이오로직스"), "Vol 123억원", "1,234,000원", "1.23"});
        card->setModes({mode});
    }
    const auto bounds = [&window](QWidget* w) { return QRect(w->mapTo(&window, QPoint()), w->size()); };
    for (const QSize size : {QSize(1440, 900), QSize(1920, 1080), QSize(1280, 800)}) {
        window.resize(size); QTest::qWait(80);
        const auto preview = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
        if (!preview.isEmpty()) QVERIFY(window.grab().save(QDir(preview).filePath(QString("layout-%1x%2.png").arg(size.width()).arg(size.height()))));
        const auto cards = bounds(scroll), watch = bounds(watchlist);
        // The supported dashboard sizes keep the shared interest column beside all markets.
        if (watch.left() > cards.left()) QVERIFY2(cards.right() + 8 < watch.left() && !cards.intersects(watch),
            qPrintable(QString("cards %1..%2, watchlist %3..%4, window %5").arg(cards.left()).arg(cards.right()).arg(watch.left()).arg(watch.right()).arg(window.width())));
        else QVERIFY2(cards.bottom() + 8 < watch.top(), qPrintable(QString("cards %1..%2, watchlist %3..%4, window %5").arg(cards.top()).arg(cards.bottom()).arg(watch.top()).arg(watch.bottom()).arg(window.height())));
        QVERIFY(watch.bottom() < window.height()); QVERIFY(watch.right() < window.width());
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
        const auto viewport = bounds(scroll->viewport());
        for (auto* card : window.findChildren<SpotlightCard*>()) {
            if (!card->isVisible()) continue;
            auto* next = child<QPushButton>(*card, "spotlightNext");
            const auto button = bounds(next);
            if (button.top() >= viewport.top()) QVERIFY2(viewport.contains(button),
                qPrintable(QString("Paging button %1,%2 %3x%4 outside viewport %5,%6 %7x%8 at window %9x%10")
                    .arg(button.x()).arg(button.y()).arg(button.width()).arg(button.height())
                    .arg(viewport.x()).arg(viewport.y()).arg(viewport.width()).arg(viewport.height())
                    .arg(window.width()).arg(window.height())));
        }
    }
}

void LocalWindowTest::providerFieldsAreMaskedAndCleared() {
    AccountDialog dialog(true);
    fillFakeKeys(dialog, "okx");
    child<QComboBox>(dialog, "okxRegionSelector")->setCurrentIndex(1);
    auto* secret = child<QLineEdit>(dialog, "credential_secret");
    QVERIFY(secret);
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    QVERIFY(child<QLineEdit>(dialog, "credential_passphrase"));
    QVERIFY(child<QPushButton>(dialog, "saveAccount")->isEnabled());
    QPointer<PressRevealButton> reveal(child<PressRevealButton>(dialog, "reveal_secret"));
    holdReveal(reveal);
    QCOMPARE(secret->echoMode(), QLineEdit::Normal);
    // One button shows only its own field.
    QCOMPARE(child<QLineEdit>(dialog, "credential_api_key")->echoMode(), QLineEdit::Password);
    QPointer<QLineEdit> oldSecret(secret);
    auto* selector = child<QComboBox>(dialog, "providerSelector");
    selector->setCurrentIndex(selector->findData("phantom"));
    QVERIFY(oldSecret.isNull());
    QVERIFY(reveal.isNull());
    QVERIFY(!child<QPushButton>(dialog, "saveAccount")->isEnabled());
    QVERIFY(!child<QLineEdit>(dialog, "credential_secret"));
    const auto seedNotice = child<QLabel>(dialog, "accountStageStatus")->text();
    QVERIFY(seedNotice.contains(QStringLiteral("시드 문구")) && seedNotice.contains(QStringLiteral("입력하지 않습니다")));
    selector->setCurrentIndex(selector->findData("toss"));
    QVERIFY(child<QLineEdit>(dialog, "credential_client_id"));
    QVERIFY(child<QLineEdit>(dialog, "credential_client_secret"));
    QVERIFY(child<QLineEdit>(dialog, "credential_client_secret")->text().isEmpty());
    QVERIFY(!dialog.findChild<QComboBox*>("storageSelector"));
}

void LocalWindowTest::okxRegionSelectionAndReplacementAreExplicit() {
    AccountDialog dialog(true); fillFakeKeys(dialog, "okx");
    auto* region = child<QComboBox>(dialog, "okxRegionSelector");
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QVERIFY(region); QCOMPARE(region->currentData().toString(), QString("global"));
    QVERIFY(save->isEnabled()); QVERIFY(region->isHidden());
    // Hidden legacy choices cannot reroute a new Global registration.
    region->setCurrentIndex(region->findData("eea"));
    const QString passphrase = " FAKE PASSPHRASE ";
    child<QLineEdit>(dialog, "credential_passphrase")->setText(passphrase);
    AccountDraft received;
    auto connection = connect(&dialog, &AccountDialog::submitted, &dialog, [&received](const AccountDraft& draft) { received = draft; });
    save->click(); QObject::disconnect(connection);
    QCOMPARE(received.region, QString("global")); QCOMPARE(received.credentials.value("passphrase"), passphrase);
    received.credentials.clear();
    AccountDraft legacy; legacy.provider = "okx"; legacy.alias = "FAKE LEGACY"; legacy.environment = "live";
    legacy.market = "spot"; legacy.currency = "USDT";
    dialog.setReplacement(legacy);
    QCOMPARE(region->currentData().toString(), QString{}); QVERIFY(region->isHidden()); QVERIFY(!save->isEnabled());
    QVERIFY(child<QLineEdit>(dialog, "credential_secret")->text().isEmpty());
    legacy.region = "us"; dialog.setReplacement(legacy);
    QCOMPARE(region->currentData().toString(), QString("us")); QVERIFY(region->isHidden());
    QVERIFY(child<QLineEdit>(dialog, "credential_passphrase")->text().isEmpty());
    dialog.reject(); QVERIFY(child<QLineEdit>(dialog, "credential_api_key")->text().isEmpty());
}

void LocalWindowTest::okxLocalReplacementSavesRegionAndKeysTogether() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    Vault vault(QDir(QDir(directory.path()).canonicalPath()).filePath("fake-okx-ui.bin")); QVERIFY(vault.create("Fake OKX UI! 2026"));
    AccountRecord record; record.provider = "okx"; record.alias = "FAKE OKX"; record.environment = "live";
    record.market = "spot"; record.currency = "USDT"; record.region = "global";
    QString error;
    QVERIFY2(vault.addAccount(record, {{"api_key","FAKE-OLD-KEY"},{"secret","FAKE-OLD-SECRET"},{"passphrase","FAKE-OLD-PASSPHRASE"}}, StorageMode::EncryptedVault, &error), qPrintable(error));
    const QString id = vault.accounts().first().id;
    AccountModel accounts(&vault); LocalWindow window(vault, accounts); window.setSecurityStatus(true);
    child<QPushButton>(window, "replaceCredentials")->click();
    auto* dialog = child<AccountDialog>(window, "accountDialog"); QVERIFY(dialog);
    auto* region = child<QComboBox>(*dialog, "okxRegionSelector"); QCOMPARE(region->currentData().toString(), QString("global"));
    QVERIFY(child<QLineEdit>(*dialog, "credential_secret")->text().isEmpty());
    region->setCurrentIndex(region->findData("eea"));
    child<QLineEdit>(*dialog, "credential_api_key")->setText("FAKE-NEW-KEY");
    child<QLineEdit>(*dialog, "credential_secret")->setText("FAKE-NEW-SECRET");
    child<QLineEdit>(*dialog, "credential_passphrase")->setText(" FAKE-NEW-PASSPHRASE ");
    child<QPushButton>(*dialog, "saveAccount")->click();
    QCOMPARE(vault.accounts().first().id, id); QCOMPARE(vault.accounts().first().region, QString("global"));
    QVERIFY(child<QLineEdit>(*dialog, "credential_secret")->text().isEmpty());
    QVERIFY(child<QLabel>(window, "accountDetails")->text().contains("global"));
    processDeletion();
}

void LocalWindowTest::kiwoomOptionalAccountNumberIsComparisonOnly() {
    AccountDialog dialog(true);
    fillFakeKeys(dialog, "kiwoom");
    auto* number = child<QLineEdit>(dialog, "credential_account_number"); QVERIFY(number);
    QCOMPARE(number->echoMode(), QLineEdit::Password);
    QVERIFY(number->placeholderText().contains("10자리"));
    QVERIFY(number->placeholderText().contains("앞자리 0 보존"));
    const auto note = child<QLabel>(dialog, "accountStageStatus")->text();
    QVERIFY(note.contains("일치 확인용")); QVERIFY(note.contains("다른 계좌를 지정하지 않습니다"));
    QVERIFY(note.contains("계좌 비밀번호는 받지 않습니다"));
    number->clear(); QVERIFY(child<QPushButton>(dialog, "saveAccount")->isEnabled());
    number->setText("0012345678");
    QString savedNumber;
    const auto connection = connect(&dialog, &AccountDialog::submitted, &dialog, [&savedNumber](const AccountDraft& draft) {
        savedNumber = draft.credentials.value("account_number");
    });
    child<QPushButton>(dialog, "saveAccount")->click();
    QObject::disconnect(connection);
    QCOMPARE(savedNumber, QString("0012345678")); savedNumber.clear();
    dialog.reject(); QVERIFY(number->text().isEmpty());
}

void LocalWindowTest::suspendedBrokersKeepOnlyRemovalAction() {
    QTemporaryDir temp;
    const auto file = temp.filePath("credentials.keefi-vault");
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/suspended-brokers/credentials.keefi-vault"), file));
    QVERIFY(QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    Vault vault(file);
    QVERIFY(vault.unlock("Synthetic suspended brokers 2026!"));
    QCOMPARE(vault.accounts().size(), 2);
    int attempts = 0;
    NoNetworkBrokerTransport transport;
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) -> TradingTransport* { ++attempts; return &transport; };
    PortfolioController portfolio(vault, nullptr, factories);
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true);
    window.resize(1440, 900);
    window.show();
    auto* pages = child<QTabWidget>(window, "portfolioPages");
    pages->setCurrentWidget(child<QWidget>(window, "connectionsPage"));
    auto* list = child<QListView>(window, "accountList");
    auto* remove = child<QPushButton>(window, "removeAccount");
    const auto saved = vault.accounts();
    for (const auto& account : saved) {
        auto* model = dynamic_cast<const ConnectionListModel*>(list->model());
        QVERIFY(model);
        const auto row = model->rowForAccount(account.id);
        QVERIFY(row >= 0);
        list->setCurrentIndex(model->index(row, 0));
        QVERIFY(model->index(row, 0).data(Qt::AccessibleTextRole).toString().contains("연결 비활성"));
        QVERIFY(child<QLabel>(window, "connectionReadState")->text().contains("비활성"));
        QVERIFY(child<QLabel>(window, "connectionActionScope")->isVisibleTo(&window));
        for (const auto* name : {"replaceCredentials", "refreshSelectedConnection", "reconnectReadAccount", "connectionAssets"}) {
            auto* action = child<QPushButton>(window, name);
            QVERIFY(!action->isEnabled());
            QVERIFY(!action->isVisibleTo(&window));
            action->click();
        }
        QCOMPARE(child<QComboBox>(window, "portfolioAccountFilter")->findData(account.id), -1);
        QVERIFY(!window.findChild<AccountDialog*>("accountDialog"));
        QVERIFY(remove->isVisibleTo(&window) && remove->isEnabled());
        remove->click();
        auto* confirm = child<QMessageBox>(window, "removeAccountConfirmation");
        QVERIFY(confirm->text().contains(account.alias));
        child<QPushButton>(*confirm, "confirmRemoveAccount")->click();
        processDeletion();
    }
    QCOMPARE(attempts, 0);
    QCOMPARE(transport.sent, 0);
    QVERIFY(vault.accounts().isEmpty());
}

void LocalWindowTest::connectionsPageOpensGuidesWithoutUnlocking_data() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("buttonName");
    QTest::newRow("phantom") << QString("phantom") << QString("phantomAddressGuide");
    QTest::newRow("bithumb") << QString("bithumb") << QString("bithumbConnectionGuide");
    QTest::newRow("okx") << QString("okx") << QString("okxConnectionGuide");
}

void LocalWindowTest::connectionsPageOpensGuidesWithoutUnlocking() {
    QFETCH(QString, provider);
    QFETCH(QString, buttonName);
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    Vault vault(profile.filePath("vault"));
    AccountModel accounts(&vault);
    PortfolioController portfolio(vault);
    LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true);
    window.resize(720, 640);
    window.show();
    auto* pages = child<QTabWidget>(window, "portfolioPages");
    auto* connections = child<QWidget>(window, "connectionsPage");
    QVERIFY(pages && connections);
    pages->setCurrentWidget(connections);
    auto* guide = connections->findChild<QPushButton*>(buttonName);
    QVERIFY(guide);
    QTRY_VERIFY(guide->isVisible() && guide->isEnabled());
    QVERIFY(window.rect().contains(QRect(guide->mapTo(&window, QPoint(0, 0)), guide->size())));
    openedGuideUrls_.clear();
    QDesktopServices::setUrlHandler("https", this, "recordGuideUrl");
    QTest::mouseClick(guide, Qt::LeftButton);
    QDesktopServices::unsetUrlHandler("https");
    QCOMPARE(openedGuideUrls_, QList<QUrl>{QUrl(QStringLiteral("https://victorious-ground-064b41900.6.azurestaticapps.net/guides/%1/").arg(provider))});
    QCOMPARE(pages->currentWidget(), connections);
    QVERIFY(!vault.exists());
    QVERIFY(!child<AccountDialog>(window, "accountDialog"));
}

void LocalWindowTest::accountDialogClearsOnSuccessAndCancel() {
    AccountDialog cancelled(true);
    fillFakeKeys(cancelled, "upbit");
    holdReveal(child<PressRevealButton>(cancelled, "reveal_secret"));
    child<QLineEdit>(cancelled, "credential_secret")->insert("-UNDO-FAKE");
    cancelled.reject();
    QVERIFY(child<QLineEdit>(cancelled, "credential_secret")->text().isEmpty());
    QVERIFY(child<QLineEdit>(cancelled, "credential_access_key")->text().isEmpty());
    QVERIFY(!child<PressRevealButton>(cancelled, "reveal_secret")->revealing());
    QCOMPARE(child<QLineEdit>(cancelled, "credential_secret")->echoMode(), QLineEdit::Password);
    child<QLineEdit>(cancelled, "credential_secret")->setEchoMode(QLineEdit::Normal);
    QVERIFY(!child<QLineEdit>(cancelled, "credential_secret")->isUndoAvailable());
    child<QLineEdit>(cancelled, "credential_secret")->undo();
    QVERIFY(child<QLineEdit>(cancelled, "credential_secret")->text().isEmpty());

    AccountDialog saved(true);
    fillFakeKeys(saved, "upbit");
    int received = 0;
    connect(&saved, &AccountDialog::submitted, &saved, [&](const AccountDraft& draft) {
        ++received;
        QCOMPARE(draft.provider, QString("upbit"));
        QVERIFY(!draft.credentials.value("secret").isEmpty());
        saved.accept();
    });
    child<QPushButton>(saved, "saveAccount")->click();
    QCOMPARE(received, 1);
    QCOMPARE(saved.result(), int(QDialog::Accepted));
    QVERIFY(child<QLineEdit>(saved, "credential_secret")->text().isEmpty());
}

void LocalWindowTest::connectionWizardPicksServiceTilesAndClearsOnServiceChange() {
    AccountDialog dialog(true);dialog.resize(560,600);dialog.show();
    auto* pages=child<QStackedWidget>(dialog,"accountConnectionSteps");QVERIFY(pages);
    QCOMPARE(pages->currentIndex(),0);
    // Nine services, one tile each; no search field and no separate "next" step.
    QVERIFY(!dialog.findChild<QLineEdit*>("accountServiceSearch"));
    QVERIFY(!dialog.findChild<QPushButton*>("accountServiceNext"));
    const QStringList ids{"binance","upbit","okx","bithumb","toss","kis","kiwoom","ls","phantom"};
    for(const auto& id:ids)QVERIFY2(dialog.findChild<QAbstractButton*>("accountService_"+id),qPrintable(id));
    QVERIFY(!dialog.findChild<QAbstractButton*>("accountService_jupiter"));
    auto* binance=dialog.findChild<QAbstractButton*>("accountService_binance");
    auto* upbit=dialog.findChild<QAbstractButton*>("accountService_upbit");
    auto* okx=dialog.findChild<QAbstractButton*>("accountService_okx");
    auto* bithumb=dialog.findChild<QAbstractButton*>("accountService_bithumb");
    // Three tiles per row: Binance, Upbit and OKX share a row; 빗썸 starts the next.
    QTRY_VERIFY(binance->x()<upbit->x()&&upbit->x()<okx->x());
    QCOMPARE(upbit->y(),binance->y());QCOMPARE(okx->y(),binance->y());
    QCOMPARE(bithumb->x(),binance->x());QVERIFY(bithumb->y()>binance->y());
    QCOMPARE(binance->accessibleName(),QStringLiteral("Binance 연결"));
    QTest::mouseClick(binance,Qt::LeftButton);
    QCOMPARE(pages->currentIndex(),1);QCOMPARE(child<QComboBox>(dialog,"providerSelector")->currentData().toString(),QString("binance"));
    child<QLineEdit>(dialog,"accountAlias")->setText(QStringLiteral("가짜 연결"));
    child<QLineEdit>(dialog,"credential_secret")->setText("FAKE-SECRET-ONLY");
    child<QPushButton>(dialog,"accountServiceBack")->click();QCOMPARE(pages->currentIndex(),0);
    QVERIFY(child<QLineEdit>(dialog,"credential_secret")->text().isEmpty());
    // Keyboard: Return on a focused tile opens that service.
    upbit->setFocus(Qt::TabFocusReason);QTest::keyClick(upbit,Qt::Key_Return);
    QCOMPARE(pages->currentIndex(),1);QCOMPARE(child<QComboBox>(dialog,"providerSelector")->currentData().toString(),QString("upbit"));
    dialog.reject();
}

void LocalWindowTest::connectionWizardReportsSavedAndReadStatesWithoutExposingKeys() {
    QTemporaryDir dir;Vault vault(dir.filePath("fake-wizard.bin"));QVERIFY(vault.create("Fake Wizard UI! 2026"));
    AssetReadTransport transport;transport.hold=true;
    auto* prices=new DeferredPortfolioTransport(true);prices->hold=true;prices->supportedAssets={"BTC"};
    PortfolioController::Factories f;f.trading=[&](const AccountRecord&){return &transport;};f.prices=[prices]{return prices;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();
    NoNetworkBrokerTransport publicTransport;PublicMarketCatalog catalog(&publicTransport);window.setPublicMarketCatalog(&catalog,false);
    child<QPushButton>(window,"addAccount")->click();auto* dialog=child<AccountDialog>(window,"accountDialog");QVERIFY(dialog);
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty()){QCoreApplication::processEvents();QVERIFY(dialog->grab().save(QDir(preview).filePath("connection-services.png")));}
    fillFakeKeys(*dialog,"binance");
    child<QLineEdit>(*dialog,"credential_api_key")->setText("FAKEONLYAPIKEY123456");
    child<QLineEdit>(*dialog,"credential_secret")->setText("FAKEONLYSECRET123456");
    if(!preview.isEmpty()){QCoreApplication::processEvents();QVERIFY(dialog->grab().save(QDir(preview).filePath("connection-form.png")));}
    child<QPushButton>(*dialog,"saveAccount")->click();
    QCoreApplication::processEvents();auto* pages=child<QStackedWidget>(*dialog,"accountConnectionSteps");QVERIFY(pages);
    QCOMPARE(pages->currentIndex(),2);QCOMPARE(vault.accounts().size(),1);
    QVERIFY(child<QLineEdit>(*dialog,"credential_secret")->text().isEmpty());
    auto* result=child<QLabel>(*dialog,"accountConnectionResult");QVERIFY(result);
    QTRY_VERIFY_WITH_TIMEOUT(result->text().contains(QStringLiteral("조회 중")),2000);
    QVERIFY(result->text().contains(QStringLiteral("저장 완료")));QVERIFY(!result->text().contains("FAKE-ONLY"));
    QVERIFY(!result->text().contains(QStringLiteral("잔고 조회 완료")));
    if(!preview.isEmpty())QVERIFY(dialog->grab().save(QDir(preview).filePath("g6-connection-result-pending.png")));
    transport.hold=false;portfolio.stopReads();portfolio.refreshAll();
    QTRY_VERIFY2_WITH_TIMEOUT(result->text().contains(QStringLiteral("잔고 조회 완료")),qPrintable(result->text()),3000);
    QVERIFY(!result->text().contains(QStringLiteral("원화 평가")));
    if(!preview.isEmpty())QVERIFY(dialog->grab().save(QDir(preview).filePath("connection-result-success.png")));
    QCOMPARE(child<QPushButton>(*dialog,"accountConnectionFinish")->text(),QString("보유 자산 보기"));
    QTRY_VERIFY_WITH_TIMEOUT(portfolio.pricingBusy()&&!prices->pending.isEmpty(),2000);
    child<QPushButton>(*dialog,"accountConnectionFinish")->click();
    QCOMPARE(child<QTabWidget>(window,"portfolioPages")->currentWidget()->objectName(),QString("portfolioHome"));
    auto* filter=child<QComboBox>(window,"portfolioAccountFilter");const auto id=vault.accounts().first().id;
    QCOMPARE(filter->currentData().toString(),id);
    prices->release();QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),3000);processDeletion();
    QCOMPARE(filter->currentData().toString(),id);
    vault.lock();processDeletion();QVERIFY(!child<AccountDialog>(window,"accountDialog"));
}

void LocalWindowTest::passwordDialogValidationAndClear() {
    VaultDialog dialog(VaultDialog::Mode::Create);
    auto* password = child<QLineEdit>(dialog, "newPassword");
    auto* confirm = child<QLineEdit>(dialog, "confirmPassword");
    auto* submit = child<QPushButton>(dialog, "submitVault");
    password->setText("short");
    confirm->setText("short");
    QVERIFY(!submit->isEnabled());
    password->setText("Fake-password-one-2026!");
    confirm->setText("Fake-password-two-2026!");
    QVERIFY(!submit->isEnabled());
    confirm->setText("Fake-password-one-2026!");
    QVERIFY(submit->isEnabled());
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    holdReveal(child<PressRevealButton>(dialog, "newPasswordReveal"));
    QCOMPARE(password->echoMode(), QLineEdit::Normal);
    QCOMPARE(confirm->echoMode(), QLineEdit::Password);
    dialog.reject();
    QVERIFY(password->text().isEmpty());
    QVERIFY(confirm->text().isEmpty());
    QCOMPARE(password->echoMode(), QLineEdit::Password);
}

void LocalWindowTest::passwordKeyboardConversionKeepsAsciiAndRejectsUnsupportedText() {
    for (const auto mode : {VaultDialog::Mode::Create, VaultDialog::Mode::ChangePassword}) {
        VaultDialog dialog(mode);
        auto* password = child<QLineEdit>(dialog, "newPassword");
        auto* notice = child<QLabel>(dialog, "passwordInputNotice");
        QVERIFY(password->placeholderText().contains(QStringLiteral("12자 이상")));
        password->insert(QStringLiteral("Fake-Vault-2026!"));
        QVERIFY(notice->isHidden());
        for (const auto& rejected : {QStringLiteral("！"), QStringLiteral("😀"), QStringLiteral("a\tb")}) {
            password->insert(rejected);
            QCOMPARE(password->text(), QStringLiteral("Fake-Vault-2026!"));
            QVERIFY(!notice->isHidden());
            QVERIFY(notice->text().contains(QStringLiteral("지원하지 않는 문자")));
        }
        QInputMethodEvent preedit(QStringLiteral("ㄱ"), {});
        QCoreApplication::sendEvent(password, &preedit);
        QCOMPARE(password->text(), QStringLiteral("Fake-Vault-2026!"));
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("가"));
        QCoreApplication::sendEvent(password, &commit);
        QCOMPARE(password->text(), QStringLiteral("Fake-Vault-2026!rk"));
        QCOMPARE(password->displayText().size(), password->text().size());
        QInputMethodEvent ascii; ascii.setCommitString(QStringLiteral("x"));
        QCoreApplication::sendEvent(password, &ascii);
        QCOMPARE(password->text(), QStringLiteral("Fake-Vault-2026!rkx"));
        QVERIFY(notice->isHidden());
    }
    for (const auto mode : {VaultDialog::Mode::Unlock, VaultDialog::Mode::ChangePassword}) {
        VaultDialog dialog(mode);
        auto* current = child<QLineEdit>(dialog, "currentPassword");
        auto* notice = child<QLabel>(dialog, "passwordInputNotice");
        QVERIFY(!dialog.findChild<QCheckBox*>("legacyNonAsciiPassword"));
        QVERIFY(!dialog.findChild<QLabel*>("currentPasswordHint"));
        QVERIFY(current->inputMethodHints() & Qt::ImhLatinOnly);
        QVERIFY(!current->inputMethodQuery(Qt::ImEnabled).toBool());
        QKeyEvent shiftedA(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, 0, 0x00, 0, QStringLiteral("ㅁ"));
        QCoreApplication::sendEvent(current, &shiftedA);
        QKeyEvent plainQ(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, 0, 0x0C, 0, QStringLiteral("ㅂ"));
        QCoreApplication::sendEvent(current, &plainQ);
        QCOMPARE(current->text(), QStringLiteral("Aq"));
        QVERIFY(notice->isHidden());
        current->clear();current->insert(QStringLiteral("Fake-Vault-2026!"));
        current->insert(QStringLiteral("한"));
        QCOMPARE(current->text(), QStringLiteral("Fake-Vault-2026!gks"));
        QInputMethodEvent preedit(QStringLiteral("ㅎ"), {});
        QCoreApplication::sendEvent(current, &preedit);
        QCOMPARE(current->text(), QStringLiteral("Fake-Vault-2026!gks"));
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("가"));
        QCoreApplication::sendEvent(current, &commit);
        QCOMPARE(current->text(), QStringLiteral("Fake-Vault-2026!gksrk"));
        if (mode == VaultDialog::Mode::Unlock) QVERIFY(child<QPushButton>(dialog, "submitVault")->isEnabled());
        dialog.reject();QVERIFY(current->text().isEmpty());
    }
}

void LocalWindowTest::nativeKoreanPasswordInput() {
    const auto driver=qEnvironmentVariable("KEEFI_NATIVE_IME_DRIVER");
    if(driver.isEmpty()||QGuiApplication::platformName()!="cocoa")QSKIP("Explicit isolated macOS IME probe only");
    QTemporaryDir directory;Vault vault(directory.filePath("fake-native-ime.bin"));AccountModel accounts(&vault);LocalWindow host(vault,accounts);
    VaultDialog dialog(VaultDialog::Mode::Unlock,&host);dialog.setWindowTitle("KEEFI fake-password IME probe");
    dialog.show();dialog.raise();dialog.activateWindow();
    auto* current=child<QLineEdit>(dialog,"currentPassword");current->setFocus();
    QTest::qWait(300);const auto before=current->geometry();
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    QProcess input;input.start("/usr/bin/python3",{driver,QString::number(QCoreApplication::applicationPid())});
    QVERIFY(input.waitForStarted());QTRY_COMPARE_WITH_TIMEOUT(input.state(),QProcess::NotRunning,20000);
    QVERIFY2(input.exitCode()==0,input.readAllStandardError().constData());
    if(!preview.isEmpty())QVERIFY(dialog.grab().save(QDir(preview).filePath("g6-native-ime-masked.png")));
    QCOMPARE(current->text(),QString("Abc123!Native"));
    QCOMPARE(current->displayText().size(),current->text().size());
    QCOMPARE(current->geometry(),before);
    holdReveal(child<PressRevealButton>(dialog,"currentPasswordReveal"));QCoreApplication::processEvents();
    if(!preview.isEmpty())QVERIFY(dialog.grab().save(QDir(preview).filePath("g6-native-ime-revealed.png")));
    QCOMPARE(current->displayText(),QString("Abc123!Native"));
    dialog.reject();
}

void LocalWindowTest::pressRevealShowsOnlyWhileHeld() {
    VaultDialog dialog(VaultDialog::Mode::ChangePassword);
    auto* current = child<QLineEdit>(dialog, "currentPassword");
    auto* password = child<QLineEdit>(dialog, "newPassword");
    auto* button = child<PressRevealButton>(dialog, "newPasswordReveal");
    button->resize(40, 24);
    QCOMPARE(button->accessibleName(), QStringLiteral("누르는 동안 암호 표시"));
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    // Pointer: shown while held, hidden on release.
    holdReveal(button);
    QCOMPARE(password->echoMode(), QLineEdit::Normal);
    QCOMPARE(current->echoMode(), QLineEdit::Password);
    sendMouse(button, QEvent::MouseButtonRelease, {2, 2}, Qt::NoButton);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    // Leaving while held hides; re-entering does not show again without a new press.
    holdReveal(button);
    sendMouse(button, QEvent::MouseMove, {200, 200}, Qt::LeftButton);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    sendMouse(button, QEvent::MouseMove, {2, 2}, Qt::LeftButton);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    sendMouse(button, QEvent::MouseButtonRelease, {200, 200}, Qt::NoButton);
    // Release outside the button after a press inside also hides.
    holdReveal(button);
    sendMouse(button, QEvent::MouseButtonRelease, {200, 200}, Qt::NoButton);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    // Keyboard: Space/Enter hold, auto-repeat never toggles, Enter never submits.
    bool submitted = false;
    QObject::connect(&dialog, &VaultDialog::submitted, &dialog, [&] { submitted = true; });
    for (const int key : {int(Qt::Key_Space), int(Qt::Key_Return), int(Qt::Key_Enter)}) {
        QVERIFY(sendKey(button, QEvent::KeyPress, key));
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QVERIFY(sendKey(button, QEvent::KeyPress, key, true));
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QVERIFY(sendKey(button, QEvent::KeyRelease, key, true));
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QVERIFY(sendKey(button, QEvent::KeyRelease, key));
        QCOMPARE(password->echoMode(), QLineEdit::Password);
    }
    QVERIFY(!submitted); QVERIFY(dialog.result() == QDialog::Rejected && !dialog.isVisible());
    QVERIFY(sendKey(button, QEvent::KeyPress, Qt::Key_Space));
    QVERIFY(sendKey(button, QEvent::KeyPress, Qt::Key_Escape));
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    sendKey(button, QEvent::KeyRelease, Qt::Key_Space);
    // Lifecycle: focus loss, window deactivation, disabling and hiding all mask.
    const auto masksOn = [&](const std::function<void()>& change) {
        holdReveal(button);
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        change();
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(!button->revealing());
    };
    masksOn([&] { QFocusEvent out(QEvent::FocusOut); QCoreApplication::sendEvent(button, &out); });
    masksOn([&] { QEvent deactivate(QEvent::WindowDeactivate); QCoreApplication::sendEvent(button, &deactivate); });
    masksOn([&] { button->setEnabled(false); button->setEnabled(true); });
    masksOn([&] { password->setEnabled(false); password->setEnabled(true); });
    masksOn([&] { dialog.clearSensitiveInputs(); });
    button->setEnabled(false);
    holdReveal(button);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
}

void LocalWindowTest::credentialKeysTrimOnlyOuterWhiteSpace() {
    AccountDialog dialog(true);
    fillFakeKeys(dialog, "okx");
    child<QComboBox>(dialog, "okxRegionSelector")->setCurrentIndex(1);
    auto* key = child<QLineEdit>(dialog, "credential_api_key");
    auto* secret = child<QLineEdit>(dialog, "credential_secret");
    auto* passphrase = child<QLineEdit>(dialog, "credential_passphrase");
    auto* notice = child<QLabel>(dialog, "credentialTrimNotice");
    QVERIFY(notice->isHidden());
    // Spaces, tabs, CR/LF and Unicode White_Space at either end are removed when input finishes.
    key->setText(QStringLiteral(" \t\u3000FAKE KEY\u00a0\r\n"));
    emit key->editingFinished();
    QCOMPARE(key->text(), QStringLiteral("FAKE KEY"));
    QVERIFY(!notice->isHidden());
    QCOMPARE(notice->text(), QStringLiteral("앞뒤 공백을 제거했습니다."));
    QVERIFY(!notice->text().contains(QStringLiteral("FAKE")));
    // Characters outside White_Space, such as U+200B, are kept.
    key->setText(QStringLiteral("\u200bFAKE-KEY\u200b "));
    emit key->editingFinished();
    QCOMPARE(key->text(), QStringLiteral("\u200bFAKE-KEY\u200b"));
    // Paste trims the whole candidate, not only the pasted fragment.
    auto* clipboard = QGuiApplication::clipboard();
    const QString previousClipboard = clipboard->text();
    clipboard->setText(QStringLiteral("  FAKE-PASTED-SECRET\n"));
    if (clipboard->text() == QStringLiteral("  FAKE-PASTED-SECRET\n")) {
        secret->clear();
        QVERIFY(sendKey(secret, QEvent::KeyPress, Qt::Key_V, false, Qt::ControlModifier));
        QCOMPARE(secret->text(), QStringLiteral("FAKE-PASTED-SECRET"));
        QCOMPARE(clipboard->text(), QStringLiteral("  FAKE-PASTED-SECRET\n")); // Clipboard is never rewritten.
        secret->setText(QStringLiteral("AB"));
        secret->setCursorPosition(1);
        clipboard->setText(QStringLiteral(" mid "));
        sendKey(secret, QEvent::KeyPress, Qt::Key_V, false, Qt::ControlModifier);
        QCOMPARE(secret->text(), QStringLiteral("A mid B")); // Interior spaces stay exact.
        clipboard->setText(previousClipboard);
    } else {
        qWarning("Platform clipboard unavailable; paste path not exercised.");
    }
    // A whitespace-only required key cannot be saved.
    secret->setText(QStringLiteral(" \t "));
    QVERIFY(!child<QPushButton>(dialog, "saveAccount")->isEnabled());
    // Saving uses the trimmed key and the exact passphrase.
    secret->setText(QStringLiteral("  FAKE-SECRET\t"));
    passphrase->setText(QStringLiteral("  fake passphrase  "));
    QVERIFY(child<QPushButton>(dialog, "saveAccount")->isEnabled());
    AccountDraft saved;
    const auto connection = QObject::connect(&dialog, &AccountDialog::submitted, &dialog,
                                             [&](const AccountDraft& draft) { saved = draft; });
    child<QPushButton>(dialog, "saveAccount")->click();
    QObject::disconnect(connection);
    QCOMPARE(saved.credentials.value("secret"), QStringLiteral("FAKE-SECRET"));
    QCOMPARE(saved.credentials.value("passphrase"), QStringLiteral("  fake passphrase  "));
    QCOMPARE(secret->text(), QStringLiteral("FAKE-SECRET")); // Screen and saved value match.
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
}

void LocalWindowTest::shortChoicesAreSegmentsThatSelectOnlyOnPurpose() {
    AccountDialog dialog(true);
    auto* provider = child<QComboBox>(dialog, "providerSelector");
    QVERIFY(!qobject_cast<SegmentedComboBox*>(provider)); // Long lists stay dropdowns.
    for (const auto* name : {"environmentSelector", "marketSelector", "currencySelector", "okxRegionSelector"})
        QVERIFY2(dialog.findChild<SegmentedComboBox*>(name), name);
    const auto pressAt = [](SegmentedComboBox* box, int row) {
        const auto center = box->segmentRects().at(row).center();
        sendMouse(box, QEvent::MouseButtonPress, center, Qt::LeftButton);
        sendMouse(box, QEvent::MouseButtonRelease, center, Qt::NoButton);
    };
    fillFakeKeys(dialog, "okx");
    // Exercise retained generic segments independently of the now-hidden OKX rollout control.
    SegmentedComboBox choices;
    choices.addItem("선택", ""); choices.addItem("Global", "global");
    choices.addItem("미국", "us"); choices.addItem("EEA", "eea");
    auto* region = &choices;
    region->resize(region->sizeHint());
    // The placeholder is never drawn: a required region starts unselected and outlined.
    QVERIFY(region->currentData().toString().isEmpty());
    QVERIFY(region->segmentRects().at(0).isNull());
    // Opening a popup is impossible; every choice is visible.
    region->showPopup();
    QVERIFY(!region->view()->isVisible());
    // A press selects; the error clears and saving becomes possible.
    QSignalSpy activated(region, &QComboBox::activated);
    pressAt(region, region->findData("eea"));
    QCOMPARE(region->currentData().toString(), QStringLiteral("eea"));
    QCOMPARE(activated.count(), 1);
    // Arrow keys only move focus; Space/Enter selects; auto-repeat and the wheel never change it.
    QFocusEvent focusIn(QEvent::FocusIn, Qt::TabFocusReason);
    QCoreApplication::sendEvent(region, &focusIn);
    QCOMPARE(region->keyboardCursor(), region->findData("eea"));
    sendKey(region, QEvent::KeyPress, Qt::Key_Left);
    QCOMPARE(region->keyboardCursor(), region->findData("us"));
    QCOMPARE(region->currentData().toString(), QStringLiteral("eea"));
    sendKey(region, QEvent::KeyPress, Qt::Key_Left);
    sendKey(region, QEvent::KeyPress, Qt::Key_Left); // Stops at the first real choice, never the placeholder.
    QCOMPARE(region->keyboardCursor(), region->findData("global"));
    QVERIFY(sendKey(region, QEvent::KeyPress, Qt::Key_Space, true));
    QCOMPARE(region->currentData().toString(), QStringLiteral("eea"));
    sendKey(region, QEvent::KeyPress, Qt::Key_Space);
    QCOMPARE(region->currentData().toString(), QStringLiteral("global"));
    sendKey(region, QEvent::KeyPress, Qt::Key_Tab); // Tab moves focus away, never the choice.
    QCOMPARE(region->currentData().toString(), QStringLiteral("global"));
    sendKey(region, QEvent::KeyPress, Qt::Key_Escape); // Escape is passed on to the dialog.
    QCOMPARE(region->currentData().toString(), QStringLiteral("global"));
    QWheelEvent wheel(QPointF(5, 5), region->mapToGlobal(QPointF(5, 5)), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(region, &wheel);
    QCOMPARE(region->currentData().toString(), QStringLiteral("global"));
    // Narrow space stacks segments instead of clipping them.
    QVERIFY(region->heightForWidth(region->sizeHint().width()) < region->heightForWidth(region->minimumSizeHint().width()));
    // Disabled choices cannot be selected, and the environment has only live.
    auto* items = qobject_cast<QStandardItemModel*>(region->model());
    const int disabledRegion = region->findData("us");
    items->item(disabledRegion)->setEnabled(false);
    pressAt(region, disabledRegion);
    QCOMPARE(region->currentData().toString(), QStringLiteral("global"));
    auto* environment = child<SegmentedComboBox>(dialog, "environmentSelector");
    environment->resize(environment->sizeHint());
    QCOMPARE(environment->count(), 1);
    QCOMPARE(environment->findData("demo"), -1);
    pressAt(environment, 0);
    QCOMPARE(environment->currentData().toString(), QStringLiteral("live"));
    provider->setCurrentIndex(provider->findData("upbit"));
    environment->resize(environment->sizeHint());
    QCOMPARE(environment->count(), 1);
    pressAt(environment, 0);
    QCOMPARE(environment->currentData().toString(), QStringLiteral("live"));
    // Both Binance markets retain the single live choice.
    provider->setCurrentIndex(provider->findData("binance"));
    auto* market = child<SegmentedComboBox>(dialog, "marketSelector");
    market->resize(market->sizeHint());
    environment->resize(environment->sizeHint());
    QCOMPARE(environment->count(), 1); QCOMPARE(environment->findData("testnet"), -1);
    pressAt(market, market->findData("usdm_futures"));
    QCOMPARE(market->currentData().toString(), QStringLiteral("usdm_futures"));
    QCOMPARE(environment->count(), 1); QCOMPARE(environment->findData("demo"), -1); QCOMPARE(environment->findData("testnet"), -1);
    QCOMPARE(environment->currentData().toString(), QStringLiteral("live"));
    // Home filters are segments too.
    QTemporaryDir directory;
    Vault vault(QDir(directory.path()).filePath("fake-segments-vault.bin"));
    NoNetworkBrokerTransport transport;
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord&) { return &transport; };
    PortfolioController portfolio(vault, nullptr, factories);
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true);
    QVERIFY(window.findChild<SegmentedComboBox*>("portfolioEnvironment"));
    const auto artifacts = qEnvironmentVariable("KEEFI_SEGMENT_ARTIFACT_DIR");
    if (!artifacts.isEmpty()) {
        QDir().mkpath(artifacts);
        provider->setCurrentIndex(provider->findData("okx"));
        dialog.resize(560, dialog.sizeHint().height());
        dialog.grab().save(QDir(artifacts).filePath("account-okx-wide.png"));
        dialog.resize(dialog.minimumSizeHint().width(), dialog.sizeHint().height());
        dialog.grab().save(QDir(artifacts).filePath("account-okx-narrow.png"));
        window.resize(1200, 800);
        window.grab().save(QDir(artifacts).filePath("home-1200.png"));
    }
}

void LocalWindowTest::marketMapUsesKeylessPublicBoards() {
    // Layout math: every area lands inside the box and keeps its proportion.
    const QRectF box(0, 0, 400, 200);
    const QList<double> areas{40000, 20000, 10000, 10000};
    const auto rects = squarifiedRects(areas, box);
    QCOMPARE(rects.size(), 4);
    for (int i = 0; i < rects.size(); ++i) {
        QVERIFY(box.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rects[i]));
        QVERIFY(std::abs(rects[i].width() * rects[i].height() - areas[i]) < 1.0);
    }
    // Parsers: stablecoins out, signed Korean falls, odd US symbols out, largest first.
    QList<PublicBoardItem> rows; QString status;
    QVERIFY(PublicEquityBoard::parseCoins(FakeBoardTransport::coins(), &rows));
    QCOMPARE(rows.size(), 2); QCOMPARE(rows[0].symbol, QString("BTC")); QCOMPARE(rows[1].symbol, QString("ZEC"));
    QCOMPARE(rows[1].changePercent, QString("-1.95"));
    QVERIFY(PublicEquityBoard::parseKorea(FakeBoardTransport::korea(), &rows, &status));
    QCOMPARE(rows[0].name, QString("삼성전자")); QCOMPARE(rows[0].group, QString("반도체·전자")); QCOMPARE(rows[0].price, QString("223500"));
    QCOMPARE(rows[1].changePercent, QString("-1.20")); QCOMPARE(status, QString("장 마감"));
    QVERIFY(PublicEquityBoard::parseUs(FakeBoardTransport::us(), &rows));
    QCOMPARE(rows.size(), 2); QCOMPARE(rows[0].symbol, QString("NVDA")); QCOMPARE(rows[0].group, QString("기술"));
    QCOMPARE(rows[0].name, QString("NVIDIA Corporation")); QCOMPARE(rows[0].changePercent, QString("-1.427"));
    QVERIFY(!PublicEquityBoard::parseUs("{\"data\":{}}", &rows));
    // US session label follows New York daylight saving (2026: Mar 8 – Nov 1), keyed to the New York date.
    const QTimeZone seoul("Asia/Seoul");
    QCOMPARE(PublicEquityBoard::usSessionKst(QDateTime(QDate(2026, 10, 21), QTime(23, 45), seoul)), QString("22:30–05:00 KST"));
    QCOMPARE(PublicEquityBoard::usSessionKst(QDateTime(QDate(2026, 10, 22), QTime(4, 0), seoul)), QString("22:30–05:00 KST"));
    QCOMPARE(PublicEquityBoard::usSessionKst(QDateTime(QDate(2026, 11, 2), QTime(23, 45), seoul)), QString("23:30–06:00 KST"));
    QCOMPARE(PublicEquityBoard::usSessionKst(QDateTime(QDate(2026, 3, 6), QTime(23, 45), seoul)), QString("23:30–06:00 KST"));
    QCOMPARE(PublicEquityBoard::usSessionKst(QDateTime(QDate(2026, 3, 9), QTime(23, 45), seoul)), QString("22:30–05:00 KST"));
    // Allowlist: only the exact unauthenticated GETs.
    for (const auto* section : {"coin", "kr", "us"}) QVERIFY(NetworkTradingTransport::allowed(PublicEquityBoard::request(section)));
    auto post = PublicEquityBoard::request("kr"); post.method = "POST"; QVERIFY(!NetworkTradingTransport::allowed(post));
    auto other = PublicEquityBoard::request("kr"); other.url.setPath("/api/stocks/other"); QVERIFY(!NetworkTradingTransport::allowed(other));
    auto signedRequest = PublicEquityBoard::request("us"); signedRequest.headers.insert("Authorization", "x"); QVERIFY(!NetworkTradingTransport::allowed(signedRequest));
    // Window: no vault, no account — all three sections still draw by market cap.
    QTemporaryDir dir; Vault vault(dir.filePath("fake-map.bin"));
    AssetReadTransport assets;
    PortfolioController::Factories f; f.prices = [] { return static_cast<TradingTransport*>(nullptr); }; f.trading = [&](const AccountRecord&) { return &assets; };
    PortfolioController portfolio(vault, nullptr, f); AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    PublicMarketTransport transport; PublicMarketCatalog markets(&transport);
    FakeBoardTransport boardTransport; boardTransport.deferUsStatus = true; PublicEquityBoard board(&boardTransport);
    window.setPublicMarketCatalog(&markets, true); window.setPublicEquityBoard(&board); board.refresh();
    window.setSecurityStatus(true); window.resize(1440, 900); window.show();
    auto* map = child<MarketMapWidget>(window, "marketMap");
    QCOMPARE(child<QStackedWidget>(window, "exploreStack")->currentIndex(), 0);
    const auto hasTile = [map](const QString& key) { for (const auto& tile : map->tileRects()) if (tile.first == key) return true; return false; };
    QTRY_VERIFY_WITH_TIMEOUT(hasTile("coin:BTC") && hasTile("kr:005930") && hasTile("us:NVDA"), 3000);
    QVERIFY(!hasTile("coin:USDT"));
    QVERIFY(board.marketBoardBusy());
    QVERIFY(board.marketStatus("us").isEmpty()); // Stock tiles can arrive before the separate session response.
    boardTransport.releaseUsStatusAfter(100);
    QTRY_VERIFY_WITH_TIMEOUT(!board.marketBoardBusy(), 3000);
    QCOMPARE(boardTransport.paths.size(), 4); // coin, kr, us and the keyless US session.
    QCOMPARE(board.marketStatus("us"), QStringLiteral("애프터마켓"));QCOMPARE(board.marketStatus("kr"), QStringLiteral("장 마감"));
    // Larger cap, larger tile.
    QRectF nvda, jpm;
    for (const auto& tile : map->tileRects()) { if (tile.first == "us:NVDA") nvda = tile.second; if (tile.first == "us:JPM") jpm = tile.second; }
    QVERIFY(nvda.width() * nvda.height() > jpm.width() * jpm.height());
    // Drill into US stocks: the same data grouped by sector fills the map.
    child<QComboBox>(window, "exploreMarketSection")->setCurrentIndex(3);
    QCOMPARE(map->filter(), QString("us"));
    for (const auto& tile : map->tileRects()) QVERIFY(tile.first.startsWith("us:"));
    QRectF big; for (const auto& tile : map->tileRects()) if (tile.first == "us:NVDA") big = tile.second;
    QVERIFY(big.width() * big.height() > nvda.width() * nvda.height());
    QTest::mouseClick(map, Qt::LeftButton, {}, big.center().toPoint());
    auto* detail = child<AssetDetailPanel>(window, "marketMapDetail");
    QCOMPARE(detail->findChild<QLabel*>("assetDetailTitle")->text(), QString("NVIDIA Corporation"));
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding0")->text().contains("Nasdaq"));
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding0")->text().contains("자산 평가에는 쓰지 않습니다"));
    const auto preview = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if (!preview.isEmpty()) {
        QVERIFY(window.grab().save(QDir(preview).filePath("g6-market-map-us.png")));
        child<QComboBox>(window, "exploreMarketSection")->setCurrentIndex(0); QCoreApplication::processEvents();
        QVERIFY(window.grab().save(QDir(preview).filePath("g6-market-map-all.png")));
    }
    // List view still offers the Upbit table and its search.
    child<QComboBox>(window, "exploreView")->setCurrentIndex(1);
    QCOMPARE(child<QStackedWidget>(window, "exploreStack")->currentIndex(), 1);
    QVERIFY(child<QLineEdit>(window, "exploreSearch")->isVisible());
}

void LocalWindowTest::exploreCompactAmountsUseGroupingWithoutChangingPublicData_data(){
    QTest::addColumn<double>("amount");QTest::addColumn<QString>("display");
    QTest::newRow("ten-thousand-won")<<99990000.0<<QString("9,999만원");
    QTest::newRow("hundred-million-won")<<999900000000.0<<QString("9,999억원");
    QTest::newRow("trillion-won")<<9999000000000000.0<<QString("9,999.0조원");
}

void LocalWindowTest::exploreCompactAmountsUseGroupingWithoutChangingPublicData(){
    QFETCH(double,amount);QFETCH(QString,display);
    QTemporaryDir dir;Vault vault(dir.filePath("synthetic-amount-vault"));AccountModel accounts(&vault);
    PortfolioController::Factories f;f.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,f);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    PublicMarketTransport marketHttp;PublicMarketCatalog markets(&marketHttp);
    FakeBoardTransport boardHttp;boardHttp.coinOverride=QJsonDocument(QJsonArray{QJsonObject{{"id","bitcoin"},{"symbol","btc"},{"name","Synthetic Bitcoin"},
        {"market_cap",amount},{"total_volume",amount},{"current_price",1234567},{"price_change_percentage_24h",1.25}}}).toJson();
    PublicEquityBoard board(&boardHttp);window.setPublicMarketCatalog(&markets,true);window.setPublicEquityBoard(&board);board.refresh();
    window.setSecurityStatus(true);window.resize(1440,900);window.show();child<QComboBox>(window,"exploreView")->setCurrentIndex(1);
    auto* spotlight=child<SpotlightCard>(window,"spotlightCard_coin");QTRY_VERIFY_WITH_TIMEOUT(!board.items("coin").isEmpty()&&!board.busy(),3000);
    spotlight->setMode("cap");QCOMPARE(spotlight->mode(),QString("cap"));QCOMPARE(spotlight->visibleItems().first().id,QString("BTC"));
    auto* watch=child<ExploreWatchlist>(window,"exploreWatchlist");if(!watch->contains("coin","BTC"))watch->toggle("coin","BTC");watch->setSection("coin");
    auto* table=child<QTableWidget>(*watch,"watchlistTable");QTRY_COMPARE(table->rowCount(),1);
    const auto original=board.knownItem("coin","BTC");const auto requests=boardHttp.paths.size();const auto marketRequests=marketHttp.requests.size();
    QCOMPARE(original.marketCap,amount);QCOMPARE(original.tradedValue,amount);QCOMPARE(original.price,QString("1234567"));
    QCOMPARE(table->item(0,2)->text(),QString("1,234,567원"));
    QCOMPARE(table->item(0,5)->text(),display);QCOMPARE(table->item(0,6)->text(),display);
    QCOMPARE(spotlight->visibleItems().first().sub,QString("24h Vol ")+display);
    emit board.changed();const auto after=board.knownItem("coin","BTC");
    QCOMPARE(after.marketCap,original.marketCap);QCOMPARE(after.tradedValue,original.tradedValue);QCOMPARE(after.price,original.price);QCOMPARE(after.receivedAtMs,original.receivedAtMs);
    QCOMPARE(boardHttp.paths.size(),requests);QCOMPARE(marketHttp.requests.size(),marketRequests);
}

void LocalWindowTest::publicStockDetailsAndWatchlistsRequireBrokerHistory_data(){
    QTest::addColumn<QString>("section");QTest::addColumn<QString>("symbol");QTest::addColumn<bool>("dark");
    for(const bool dark:{false,true}){
        QTest::newRow(dark?"kr-dark":"kr-light")<<QString("kr")<<QString("005930")<<dark;
        QTest::newRow(dark?"us-dark":"us-light")<<QString("us")<<QString("NVDA")<<dark;
    }
}
void LocalWindowTest::publicStockDetailsAndWatchlistsRequireBrokerHistory(){
    QFETCH(QString,section);QFETCH(QString,symbol);QFETCH(bool,dark);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    // A real legacy DTO response, entirely synthetic, exposes the previous
    // stock history path before its request factory/parser are removed.
    class HistoryHttp final:public TradingTransport{
      public:
        QList<RequestSpec> requests;
        void abort()override{}
        void send(quint64 token,const RequestSpec& request)override{
            requests.append(request);HttpResult response;response.status=200;const auto now=QDateTime::currentMSecsSinceEpoch();
            if(request.url.host()=="api.stock.naver.com"){
                const auto at=QDateTime::fromMSecsSinceEpoch(now-60000,QTimeZone("Asia/Seoul"));
                response.body=QJsonDocument(QJsonObject{{"code",request.url.path().section('/',4,4)},{"priceInfos",QJsonArray{
                    QJsonObject{{"localDateTime",at.addSecs(-60).toString("yyyyMMddHHmmss")},{"currentPrice",100}},
                    QJsonObject{{"localDateTime",at.toString("yyyyMMddHHmmss")},{"currentPrice",101}}}}}).toJson();
            }else response.body=QJsonDocument(QJsonObject{{"data",QJsonObject{{"symbol",request.url.path().section('/',3,3)},{"chart",QJsonArray{
                QJsonObject{{"x",double(now-120000)},{"y",100}},QJsonObject{{"x",double(now-60000)},{"y",101}}}}}}}).toJson();
            QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});
        }
    } historyHttp;
    PublicMarketHistory history(&historyHttp);history.setTargets({{section,symbol,{}}});QTest::qWait(20);
    qInfo()<<"Synthetic legacy stock history points before presentation:"<<history.history(section,symbol).points.size();
    QTemporaryDir directory;QVERIFY(directory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.filePath("settings"));
    Vault vault(directory.filePath("synthetic-public-stock-detail-never-opened"));AccountModel accounts(&vault);
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    PublicMarketTransport marketsHttp;PublicMarketCatalog markets(&marketsHttp);FakeBoardTransport boardHttp;PublicEquityBoard board(&boardHttp);
    window.setPublicMarketCatalog(&markets,true);window.setPublicEquityBoard(&board);window.setPublicMarketHistory(&history);
    window.setSecurityStatus(true);window.resize(1440,1000);window.show();
    QTest::mouseClick(child<QPushButton>(window,dark?"themeDark":"themeLight"),Qt::LeftButton);
    QCOMPARE(KeefiTheme::instance()->mode(),dark?ThemeMode::Dark:ThemeMode::Light);board.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!board.items(section).isEmpty(),3000);
    auto* map=child<MarketMapWidget>(window,"marketMap");QRectF selectedTile;
    for(const auto& tile:map->tileRects())if(tile.first==section+":"+symbol)selectedTile=tile.second;
    QVERIFY(!selectedTile.isEmpty());QTest::mouseClick(map,Qt::LeftButton,{},selectedTile.center().toPoint());
    auto* detail=child<AssetDetailPanel>(window,"marketMapDetail");QVERIFY(detail);
    auto* metric=detail->findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(metric);
    QTRY_COMPARE(metric->data().id,symbol);
    QVERIFY2(metric->data().history.points.isEmpty(),"Public stock history must never attach to the guest detail card");
    QCOMPARE(metric->data().history.reason,QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QVERIFY(!metric->historyVisible());QVERIFY(metric->plottedHistory().isEmpty());
    auto* guidance=child<QLabel>(*detail,"assetDetailIndicator0");
    QVERIFY(guidance->text().contains(QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요")));QVERIFY(!guidance->isHidden());
    QVERIFY(child<QPushButton>(*detail,"assetDetailChart")->isHidden());
    auto* watch=child<ExploreWatchlist>(window,"exploreWatchlist");watch->toggle(section,symbol);watch->setSection(section);
    auto* view=child<QComboBox>(window,"exploreView");view->setCurrentIndex(view->findData("list"));QCoreApplication::processEvents();
    auto* table=child<QTableWidget>(*watch,"watchlistTable");QTRY_COMPARE(table->rowCount(),1);
    QVERIFY(table->item(0,4)->data(Qt::UserRole+1).value<QList<QPointF>>().isEmpty());
    QVERIFY(!table->item(0,4)->toolTip().contains("https://"));QVERIFY(table->item(0,4)->toolTip().count('\n')==0);QCOMPARE(metric->data().history.reason,QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));
    QVERIFY(table->item(0,2)->text()!=QStringLiteral("—")); // Public quote lists remain available.
    auto* basis=child<QLabel>(*watch,"watchlistBasis");QVERIFY(basis->isHidden());
    QVERIFY(watch->compact());QVERIFY(table->isColumnHidden(4));
    // The narrow public list keeps quote columns. Selecting a stock explains
    // its broker requirement instead of reserving a permanent source footer.
    QTest::mouseClick(table->viewport(),Qt::LeftButton,{},table->visualItemRect(table->item(0,1)).center());
    auto* guide=child<QDialog>(window,"chartConnectionGuide");QVERIFY(guide&&guide->isVisible());
    QVERIFY(guide->findChild<QLabel*>()->text().contains(QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요")));
    guide->close();processDeletion();
    watch->setSection("coin");QVERIFY(basis->isHidden());
    watch->setSection(section);QVERIFY(basis->isHidden());
    for(const auto& request:historyHttp.requests)QVERIFY(request.url.host()!="api.stock.naver.com"&&request.url.host()!="api.nasdaq.com");
    if(const auto output=qEnvironmentVariable("KEEFI_BROKER_ONLY_RENDER_DIR");!output.isEmpty()){
        view->setCurrentIndex(view->findData("map"));QCoreApplication::processEvents();
        QVERIFY(QDir().mkpath(output));QCoreApplication::processEvents();
        QVERIFY(window.grab().save(QDir(output).filePath(QString("public-stock-detail-%1-%2.png").arg(section,dark?"dark":"light"))));
        view->setCurrentIndex(view->findData("list"));QCoreApplication::processEvents();
        QVERIFY(basis->isHidden());
        QVERIFY(window.grab().save(QDir(output).filePath(QString("public-stock-watchlist-%1-%2.png").arg(section,dark?"dark":"light"))));
    }
    QVERIFY(!vault.exists());QVERIFY(!vault.isUnlocked());QCOMPARE(accounts.rowCount(),0);
}

void LocalWindowTest::spotlightPagesFiveRowsAndOptionsPinSmallBalances() {
    // Logical ranking pages survive live updates and viewport changes.
    SpotlightCard card("coin", "CRYPTO");
    SpotlightMode cap{"cap", "Market Cap", {}}, gainers{"gainers", "Top Gainers", {}};
    for (int i = 0; i < 23; ++i) cap.items.append({QString("C%1").arg(i), QString("C%1").arg(i), "MC 1조원", "1원", "1.5"});
    gainers.items = cap.items.mid(0, 7);
    card.setModes({cap, gainers});
    QCOMPARE(card.pageCount(), 2); QCOMPARE(card.visibleItems().size(), 19);
    card.setPage(9); QCOMPARE(card.page(), 1); QCOMPARE(card.visibleItems().last().id, QString("C22"));
    card.setModes({cap, gainers}); QCOMPARE(card.page(), 1);
    card.setMode("gainers"); QCOMPARE(card.page(), 0); QCOMPARE(card.pageCount(), 1);
    card.setPage(1); QCOMPARE(card.visibleItems().size(), 7);
    // Window: Spotlight shows CRYPTO · KOSPI · NASDAQ cards from the same public boards.
    QTemporaryDir dir; Vault vault(dir.filePath("fake-spotlight.bin"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    AssetReadTransport assets;
    PortfolioController::Factories f; f.prices = [] { return static_cast<TradingTransport*>(nullptr); }; f.trading = [&](const AccountRecord&) { return &assets; };
    PortfolioController portfolio(vault, nullptr, f); AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    PublicMarketTransport transport; PublicMarketCatalog markets(&transport);
    FakeBoardTransport boardTransport; PublicEquityBoard board(&boardTransport);
    window.setPublicMarketCatalog(&markets, true); window.setPublicEquityBoard(&board); board.refresh();
    window.setSecurityStatus(true); window.resize(1440, 900); window.show();
    auto* view = child<QComboBox>(window, "exploreView");
    // Usability 10.1: the hidden view/market models carry the same Korean names as their visible text tabs.
    QCOMPARE(view->itemText(0), QStringLiteral("지도")); QCOMPARE(view->itemText(1), QStringLiteral("순위"));
    QVERIFY(view->isHidden()); QVERIFY(window.findChild<QWidget*>("exploreViewTabs")); QVERIFY(window.findChild<QWidget*>("exploreMarketTabs"));
    auto* section = child<QComboBox>(window, "exploreMarketSection");
    QCOMPARE(section->itemText(1), QStringLiteral("코인")); QCOMPARE(section->itemText(2), QStringLiteral("국내 주식")); QCOMPARE(section->itemText(3), QStringLiteral("미국 주식"));
    QVERIFY(!window.findChild<QPushButton*>("exploreRefresh"));
    view->setCurrentIndex(1);
    QCOMPARE(child<QStackedWidget>(window, "exploreStack")->currentIndex(), 1);
    auto* us = child<SpotlightCard>(window, "spotlightCard_us");
    QTRY_VERIFY_WITH_TIMEOUT(!us->visibleItems().isEmpty() && us->visibleItems().first().symbol == "JPM", 3000);
    QVERIFY(us->visibleItems().first().price.startsWith("$"));
    QVERIFY(us->visibleItems().first().sub.startsWith("Vol ")); // Traded value under every row.
    us->setMode("volume"); QCOMPARE(us->mode(), QString("volume")); us->setMode("cap");
    auto* coins = child<SpotlightCard>(window, "spotlightCard_coin");
    QTRY_VERIFY_WITH_TIMEOUT(!coins->visibleItems().isEmpty(), 3000);
    coins->setMode("gainers"); // Upbit KST 09:00 previous close.
    QTRY_VERIFY_WITH_TIMEOUT(coins->visibleItems().size() == 2 && coins->visibleItems().first().id == "PUMP", 3000);
    QVERIFY(coins->visibleItems().first().sub.startsWith("24h Vol "));
    // A query swaps the cards for the full coin search, and clearing it brings them back.
    child<QLineEdit>(window, "exploreSearch")->setText("pump");
    QCOMPARE(child<QStackedWidget>(window, "exploreStack")->currentIndex(), 2);
    child<QLineEdit>(window, "exploreSearch")->clear();
    QCOMPARE(child<QStackedWidget>(window, "exploreStack")->currentIndex(), 1);
    // The market filter belongs to the map; the ranking keeps all three markets.
    section->setCurrentIndex(3); QVERIFY(coins->isVisible()); QVERIFY(us->isVisible());
    section->setCurrentIndex(0);
    const auto preview = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if (!preview.isEmpty()) { QCoreApplication::processEvents(); QTest::qWait(50); QVERIFY(window.grab().save(QDir(preview).filePath("g6-spotlight-1440.png"))); }
    // Options: one window with categories; pinning small balances persists and applies.
    child<QPushButton>(window, "openOptions")->click();
    auto* options = child<QDialog>(window, "optionsDialog");
    QVERIFY(options->isVisible());
    const auto optionTabs=child<QWidget>(*options,"optionsNav")->findChildren<QPushButton*>(QString(),Qt::FindDirectChildrenOnly);
    QStringList actualTabs;for(auto* tab:optionTabs){actualTabs.append(tab->objectName());QVERIFY(tab->isCheckable());QVERIFY(tab->isVisibleTo(options));}
    QStringList expectedTabs{"optionsGeneralTab","optionsDisplayTab","optionsSecurityTab","optionsRecordsTab","optionsAboutTab","optionsAlertsTab","optionsMarketAlertsTab"};
    actualTabs.sort();expectedTabs.sort();QCOMPARE(actualTabs,expectedTabs);
    QVERIFY(!child<QPushButton>(*options, "optionChangePassword")->isEnabled()); // No vault yet.
    auto* pin = child<QCheckBox>(*options, "optionHideSmall");
    QVERIFY(pin->isChecked());
    pin->setChecked(false);
    QVERIFY(!QSettings().value("display/hideSmallBalances").toBool());
    pin->setChecked(true);
    QVERIFY(QSettings().value("display/hideSmallBalances").toBool());
    QVERIFY(child<QPushButton>(window, "hideSmallBalances")->isChecked());
    if (!preview.isEmpty()) { child<QPushButton>(*options, "optionsDisplayTab")->click(); QCoreApplication::processEvents(); QVERIFY(options->grab().save(QDir(preview).filePath("g6-options.png"))); }
    options->close(); processDeletion();
    // Erase from 옵션 · 보안: one "정말 지우겠습니까?" box; cancel keeps the vault, 지우기 removes it.
    QVERIFY(vault.create("Fake Erase Vault! 2026")); vault.lock();
    child<QPushButton>(window, "openOptions")->click();
    options = child<QDialog>(window, "optionsDialog");
    auto* erase = child<QPushButton>(*options, "optionEraseVault"); QVERIFY(erase->isEnabled());
    erase->click(); processDeletion();
    auto* confirm = child<QMessageBox>(window, "eraseVaultConfirmation"); QVERIFY(confirm);
    QCOMPARE(confirm->text(), QStringLiteral("정말 지우겠습니까?"));
    QCOMPARE(confirm->defaultButton(), child<QPushButton>(*confirm, "cancelEraseVault")); // Enter never erases.
    child<QPushButton>(*confirm, "cancelEraseVault")->click();
    processDeletion(); QVERIFY(vault.exists());
    child<QPushButton>(window, "openOptions")->click();
    child<QPushButton>(*child<QDialog>(window, "optionsDialog"), "optionEraseVault")->click(); processDeletion();
    confirm = child<QMessageBox>(window, "eraseVaultConfirmation"); QVERIFY(confirm);
    child<QPushButton>(*confirm, "confirmEraseVault")->click(); processDeletion();
    QVERIFY(!vault.exists());
    QVERIFY(child<QPushButton>(window, "createVault")->isVisibleTo(&window));
    LocalWindow second(vault, accounts); second.setPortfolioController(&portfolio);
    QVERIFY(child<QPushButton>(second, "hideSmallBalances")->isChecked());
    QSettings().remove("display/hideSmallBalances");
}

void LocalWindowTest::valuationExportEndsWithTheUnlockedSession_data() {
    QTest::addColumn<QString>("lockAt");   // "", "confirm" or "file"
    QTest::addColumn<bool>("unlockAgain"); // a later unlock must not revive the earlier export
    QTest::addColumn<bool>("existing");    // the chosen path already holds a file
    QTest::addColumn<QString>("lockMethod"); // Each production entry must cancel the export.
    QTest::newRow("saved") << QString() << false << false << QString("vault");
    QTest::newRow("locked-at-confirmation") << QString("confirm") << false << false << QString("vault");
    QTest::newRow("locked-at-file-selection") << QString("file") << false << true << QString("vault");
    QTest::newRow("relocked-and-unlocked") << QString("file") << true << false << QString("vault");
    QTest::newRow("window-lock-at-file-selection") << QString("file") << false << true << QString("window");
    QTest::newRow("security-loss-at-confirmation") << QString("confirm") << false << false << QString("security");
}

void LocalWindowTest::valuationExportEndsWithTheUnlockedSession() {
    // R2: an export started before a lock never writes after it, even if the vault is unlocked again.
    QFETCH(QString, lockAt); QFETCH(bool, unlockAgain); QFETCH(bool, existing); QFETCH(QString, lockMethod);
    QStandardPaths::setTestModeEnabled(true); // The suggested folder is never the real Documents.
    QTemporaryDir dir; const QString password = "Fake Export Lock! 2026";
    Vault vault(dir.filePath("fake-export.bin")); QVERIFY(vault.create(password));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    AccountRecord a; a.provider = "binance"; a.alias = QStringLiteral("가짜 내보내기 계좌"); a.environment = "live"; a.market = "spot"; a.currency = "USDT";
    QVERIFY(vault.addAccount(a, {{"api_key", "FAKEAPIKEY123456789"}, {"secret", "FAKESECRET123456789"}}, StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances = {QJsonObject{{"asset", "USDT"}, {"free", "10"}, {"locked", "0"}}, QJsonObject{{"asset", "BIG"}, {"free", "1"}, {"locked", "0"}}};
    auto* prices = new FreshKrwPriceTransport;
    PortfolioController::Factories f; f.prices = [prices] { return prices; }; f.trading = [&](const AccountRecord&) { return &assets; };
    PortfolioController portfolio(vault, nullptr, f); AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window, "portfolioLoading")->isHidden(), 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.valuationDiagnostics().isEmpty(), 4000);
    const auto target = dir.filePath("export.json");
    if (existing) { QFile old(target); QVERIFY(old.open(QIODevice::WriteOnly)); old.write("SENTINEL-R2"); }
    // A scripted user: answers whatever export dialog is open, locking once at the chosen step.
    bool locked = false; QStringList results, exportTrace;
    const auto lock = [&] {
        locked = true;
        if (lockMethod == "window") window.lockAndClear();
        else if (lockMethod == "security") window.setSecurityStatus(false, "FAKE-EXPORT-SECURITY-LOSS");
        else vault.lock();
        if (unlockAgain) QVERIFY(vault.unlock(password));
    };
    QTimer user; user.setInterval(20); int ticks = 0; bool stuck = false;
    connect(&user, &QTimer::timeout, &window, [&] {
        auto* modal = QApplication::activeModalWidget();
        if (++ticks > 500 && modal) { stuck = true; QMetaObject::invokeMethod(modal, "reject"); return; } // Never hang the suite.
        if (auto* box = qobject_cast<QMessageBox*>(modal); box && box->isVisible()) {
            exportTrace << QStringLiteral("message:%1").arg(box->objectName());
            if (auto* yes = box->button(QMessageBox::Yes)) {
                if (lockAt == "confirm" && !locked) lock();
                if (box->isVisible()) yes->click(); // A late answer after the lock.
            } else { results << box->text(); box->accept(); }
        } else if (auto* files = qobject_cast<QFileDialog*>(modal); files && files->isVisible()) {
            if (lockAt == "file" && !locked) lock();
            if (files->isVisible()) {
                files->selectFile(target);
                // Keep only booleans/counts: on a rare failure distinguish a
                // scripted chooser mismatch from an export/session failure.
                exportTrace << QStringLiteral("file:%1 selected_target=%2 count=%3")
                    .arg(files->objectName()).arg(files->selectedFiles() == QStringList{target})
                    .arg(files->selectedFiles().size());
                QMetaObject::invokeMethod(files, "accept");
                exportTrace << QStringLiteral("accepted=%1 selected_target_after=%2")
                    .arg(files->result() == QDialog::Accepted).arg(files->selectedFiles() == QStringList{target});
            }
        }
    });
    user.start();
    child<QPushButton>(window, "openOptions")->click();
    auto* exportButton = window.findChild<QPushButton*>("optionExportValuation");
    QVERIFY(exportButton && exportButton->isEnabled());
    exportButton->click(); // Runs the whole modal sequence.
    user.stop();
    QVERIFY2(!stuck, "an export dialog stayed open");
    QCOMPARE(locked, !lockAt.isEmpty());
    QFile saved(target);
    if (lockAt.isEmpty()) {
        exportTrace << QStringLiteral("results=%1 success_message=%2 vault_open=%3")
            .arg(results.size()).arg(results.size() == 1 && results.first().contains(QStringLiteral("저장했어요")))
            .arg(vault.isUnlocked());
        const bool opened = saved.open(QIODevice::ReadOnly);
        exportTrace << QStringLiteral("directory_valid=%1 exists=%2 file_error=%3")
            .arg(dir.isValid()).arg(saved.exists()).arg(int(saved.error()));
        QVERIFY2(opened, qPrintable(exportTrace.join(QStringLiteral("; "))));
        const auto document = QJsonDocument::fromJson(saved.readAll());
        QCOMPARE(document.object().value("format").toString(), QStringLiteral("keefi-valuation-bundle/1"));
        QCOMPARE(saved.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        QVERIFY(results.size() == 1 && results.first().contains(QStringLiteral("저장했어요")));
    } else if (existing) {
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("SENTINEL-R2")); // Untouched.
    } else {
        QVERIFY2(!saved.exists(), "an export from before the lock was written");
    }
}

void LocalWindowTest::valuationDiagnosticsFollowPriceExpiryWithoutNewInputs() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-diagnostics-expiry.bin"));
    QVERIFY(vault.create("Fake Diagnostics Expiry! 2026"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    AccountRecord a; a.provider="binance"; a.alias="FAKE EXPIRY"; a.environment="live"; a.market="spot"; a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEEXPIRYKEY123456"},{"secret","FAKEEXPIRYSECRET123456"}},StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances={QJsonObject{{"asset","USDT"},{"free","10"},{"locked","0"}}};
    auto* prices=new FreshKrwPriceTransport;
    prices->prices={{"USDT","1000"}};
    // Use the real capture/render clock while crossing the 180 s boundary in seconds.
    prices->sourceTime=QDateTime::currentMSecsSinceEpoch()-DisplayFreshnessMs+3000;
    PortfolioController::Factories f; f.prices=[prices]{return prices;}; f.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,f); AccountModel accounts(&vault); LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    auto* total=child<QLabel>(window,"portfolioTotal"); auto* title=child<QLabel>(window,"portfolioTotalLabel");
    QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("10,000"),2000);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"portfolioLoading")->isHidden(),1000);
    portfolio.setAutomaticRefresh(false);
    const auto fresh=window.valuationDiagnostics(); QVERIFY(!fresh.isEmpty());
    const auto freshResult=fresh.value("keefi_result").toObject();
    QCOMPARE(freshResult.value("valuation").toObject().value("known_subtotal").toString(),QStringLiteral("10000"));
    QCOMPARE(freshResult.value("rows").toArray().first().toObject().value("state").toString(),QStringLiteral("Fresh"));
    QCOMPARE(freshResult.value("screen").toObject().value("figure").toString(),total->text());
    const auto reads=assets.sent;
    QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch()>prices->sourceTime+DisplayFreshnessMs,4000);
    const auto expired=window.valuationDiagnostics(); QVERIFY(!expired.isEmpty());
    const auto result=expired.value("keefi_result").toObject(); const auto screen=result.value("screen").toObject();
    const auto row=result.value("rows").toArray().first().toObject();
    QVERIFY(!result.value("valuation").toObject().value("complete").toBool());
    QCOMPARE(result.value("valuation").toObject().value("known_subtotal").toString(),QStringLiteral("0"));
    QCOMPARE(row.value("state").toString(),QStringLiteral("Reference"));
    QVERIFY(row.value("value").isNull()); QCOMPARE(row.value("reference_value").toString(),QStringLiteral("10000"));
    QCOMPARE(screen.value("figure").toString(),total->text()); QCOMPARE(total->text(),QStringLiteral("—"));
    QCOMPARE(screen.value("label").toString(),title->text());
    QVERIFY(expired.value("as_of_ms").toDouble()>fresh.value("as_of_ms").toDouble());
    QCOMPARE(assets.sent,reads); // Expiry alone changes the display; no new balance is needed.
    vault.lock(); QVERIFY(window.valuationDiagnostics().isEmpty());
}

void LocalWindowTest::referencePriceExpiresAfterTwentyFourHoursWithoutNewInputs() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-reference-ttl.bin"));
    QVERIFY(vault.create("Fake Reference TTL! 2026"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    AccountRecord a; a.provider="binance"; a.alias="FAKE REFERENCE TTL"; a.environment="live"; a.market="spot"; a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEREFERENCETTLKEY123"},{"secret","FAKEREFERENCETTLSECRET123"}},StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances={QJsonObject{{"asset","PUMP"},{"free","2"},{"locked","0"}},
                     QJsonObject{{"asset","KRW"},{"free","10"},{"locked","0"}}};
    auto* prices=new FreshKrwPriceTransport;
    prices->prices={{"PUMP","2.75"}};
    // The reference's 24 h source-age limit arrives before the fresh balance's 180 s expiry.
    prices->sourceTime=QDateTime::currentMSecsSinceEpoch()-ReferenceMaxAgeMs+3000;
    PortfolioController::Factories f; f.prices=[prices]{return prices;}; f.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,f); AccountModel accounts(&vault); LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    auto* tree=child<QTreeView>(window,"portfolioAssets");
    const auto pump=[tree]() -> PortfolioTestRow {
        for(int i=0;i<tree->model()->rowCount();++i) {
            auto group=portfolioRow(tree, i);
            if(group.text(0)=="PUMP"&&group.childCount()) return group.child(0);
        }
        return {};
    };
    QTRY_VERIFY_WITH_TIMEOUT(pump()&&pump().text(3)==QStringLiteral("참고 6"),2000);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.pricingBusy(),1000);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"portfolioLoading")->isHidden(),1000);
    portfolio.setAutomaticRefresh(false);
    const auto before=window.valuationDiagnostics(); QVERIFY(!before.isEmpty());
    QCOMPARE(before.value("keefi_result").toObject().value("screen").toObject().value("reference_groups").toInt(),1);
    const auto reads=assets.sent;
    QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch()>prices->sourceTime+ReferenceMaxAgeMs,4000);
    // Check ordinary rendering first: export forces a fresh valuation and would mask this cache bug.
    QTRY_VERIFY_WITH_TIMEOUT(pump()&&pump().text(3)==QStringLiteral("미평가"),1000);
    const auto after=window.valuationDiagnostics(); QVERIFY(!after.isEmpty());
    const auto result=after.value("keefi_result").toObject(); const auto screen=result.value("screen").toObject();
    QCOMPARE(screen.value("reference_groups").toInt(),0);
    QCOMPARE(screen.value("unvalued_groups").toInt(),1);
    QCOMPARE(result.value("valuation").toObject().value("known_subtotal").toString(),QStringLiteral("10"));
    for(const auto& value:result.value("rows").toArray()) {
        const auto row=value.toObject();
        if(row.value("asset").toString()!="PUMP") continue;
        QCOMPARE(row.value("state").toString(),QStringLiteral("Unvalued"));
        QVERIFY(row.value("value").isNull()); QVERIFY(row.value("reference_value").isNull());
    }
    QCOMPARE(screen.value("figure").toString(),child<QLabel>(window,"portfolioTotal")->text());
    QCOMPARE(screen.value("label").toString(),child<QLabel>(window,"portfolioTotalLabel")->text());
    QCOMPARE(assets.sent,reads);
    vault.lock(); QVERIFY(window.valuationDiagnostics().isEmpty());
}

void LocalWindowTest::futurePublicPriceBecomesCurrentWithoutNewInputs() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-future-price.bin"));
    QVERIFY(vault.create("Fake Future Public Price! 2026"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    AccountRecord a; a.provider="binance"; a.alias="FAKE FUTURE PRICE"; a.environment="live"; a.market="spot"; a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEFUTUREPRICEKEY123"},{"secret","FAKEFUTUREPRICESECRET123"}},StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances={QJsonObject{{"asset","USDT"},{"free","10"},{"locked","0"}}};
    auto* prices=new FreshKrwPriceTransport;
    prices->prices={{"USDT","1000"}};
    // The public transport admits up to 2 s of source-clock skew; valuation must
    // wait until that original time without requiring another HTTP response.
    prices->sourceTimeLeadMs=1900;
    PortfolioController::Factories f; f.prices=[prices]{return prices;}; f.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,f); AccountModel accounts(&vault); LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.quotes().isEmpty()&&!portfolio.pricingPending(),2500);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"portfolioLoading")->isHidden(),500);
    portfolio.setAutomaticRefresh(false);
    const auto quote=portfolio.quotes().first();
    QCOMPARE(quote.evidence.sourceTimeMs,prices->sourceTime);
    QVERIFY(quote.evidence.receivedAtMs<quote.evidence.sourceTimeMs);
    QVERIFY(quote.evidence.sourceTimeMs-quote.evidence.receivedAtMs<=2000);
    QVERIFY(QDateTime::currentMSecsSinceEpoch()<prices->sourceTime);
    auto* total=child<QLabel>(window,"portfolioTotal");
    QCOMPARE(total->text(),QStringLiteral("—"));
    const auto reads=assets.sent; const auto priceCount=portfolio.quotes().size();
    QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch()>=prices->sourceTime,2500);
    // Assert ordinary rendering before diagnostics, which deliberately clears the cache.
    QTRY_COMPARE_WITH_TIMEOUT(total->text(),QStringLiteral("10,000"),1000);
    const auto bundle=window.valuationDiagnostics(); QVERIFY(!bundle.isEmpty());
    const auto result=bundle.value("keefi_result").toObject();
    const auto row=result.value("rows").toArray().first().toObject();
    QCOMPARE(row.value("state").toString(),QStringLiteral("Fresh"));
    QCOMPARE(row.value("value").toString(),QStringLiteral("10000"));
    QCOMPARE(result.value("screen").toObject().value("figure").toString(),total->text());
    QCOMPARE(result.value("screen").toObject().value("label").toString(),child<QLabel>(window,"portfolioTotalLabel")->text());
    QCOMPARE(assets.sent,reads); QCOMPARE(portfolio.quotes().size(),priceCount);
    QCOMPARE(portfolio.quotes().first().evidence.receivedAtMs,quote.evidence.receivedAtMs);
    vault.lock(); QVERIFY(window.valuationDiagnostics().isEmpty());
}

void LocalWindowTest::valuationDiagnosticsMatchTheScreenWithoutIdentities() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-diagnostics.bin")); QVERIFY(vault.create("Fake Diagnostics! 2026"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    AccountRecord a; a.provider = "binance"; a.alias = QStringLiteral("가짜 진단 계좌"); a.environment = "live"; a.market = "spot"; a.currency = "USDT";
    QVERIFY(vault.addAccount(a, {{"api_key", "FAKEAPIKEY123456789"}, {"secret", "FAKESECRET123456789"}}, StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances = {QJsonObject{{"asset", "USDT"}, {"free", "10"}, {"locked", "0"}}, QJsonObject{{"asset", "DUST"}, {"free", "1"}, {"locked", "0"}},
                       QJsonObject{{"asset", "BIG"}, {"free", "1.23456789"}, {"locked", "0"}}, QJsonObject{{"asset", "NOPRICE"}, {"free", "0.0001"}, {"locked", "0"}},
                       QJsonObject{{"asset", "KRW"}, {"free", "0"}, {"locked", "0"}}};
    auto* prices = new FreshKrwPriceTransport;
    PortfolioController::Factories f; f.prices = [prices] { return prices; }; f.trading = [&](const AccountRecord&) { return &assets; };
    PortfolioController portfolio(vault, nullptr, f); AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    QVERIFY(window.valuationDiagnostics().isEmpty()); // No portfolio yet: nothing to export.
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window, "portfolioLoading")->isHidden(), 4000);
    QTRY_COMPARE_WITH_TIMEOUT(child<QLabel>(window, "portfolioTotal")->text(), QStringLiteral("137,307"), 4000);
    const auto bundle = window.valuationDiagnostics();
    QCOMPARE(bundle.value("format").toString(), QStringLiteral("keefi-valuation-bundle/1"));
    QVERIFY(bundle.value("as_of_ms").toDouble() > 0);
    const auto result = bundle.value("keefi_result").toObject();
    QCOMPARE(result.value("screen").toObject().value("figure").toString(), child<QLabel>(window, "portfolioTotal")->text());
    QVERIFY(result.value("valuation").toObject().value("known_subtotal").toString().startsWith("137307.289"));
    QVERIFY(result.value("dust").toObject().value("hidden").toArray().contains(QStringLiteral("exchange:DUST")));
    QCOMPARE(bundle.value("connections").toArray().first().toObject().value("id").toString(), QStringLiteral("conn-1"));
    const auto snapshot = bundle.value("snapshots").toArray().first().toObject();
    QCOMPARE(snapshot.value("account_scope").toString(), QStringLiteral("scope-1"));
    QCOMPARE(snapshot.value("connection_id").toString(), QStringLiteral("conn-1"));
    bool upbit = false;
    for (const auto& value : bundle.value("quotes").toArray()) {
        const auto venue = value.toObject().value("venue").toString();
        QVERIFY(QStringList({"upbit", "binance", "jupiter", "account"}).contains(venue));
        upbit |= venue == "upbit";
    }
    QVERIFY(upbit);
    // Credentials, bindings and raw account identities never leave the vault.
    const auto text = QJsonDocument(bundle).toJson();
    const auto original = portfolio.snapshots().first();
    for (const auto& secret : {QStringLiteral("FAKEAPIKEY123456789"), QStringLiteral("FAKESECRET123456789"), vault.accounts().first().id,
                               original.accountScope, original.connectionBinding})
        QVERIFY2(!text.contains(secret.toUtf8()), qPrintable(secret.left(12)));
    QVERIFY(!bundle.value("raw_responses").toObject().value("recording").toBool());
    if (const auto out = qEnvironmentVariable("KEEFI_BUNDLE_OUT"); !out.isEmpty()) {
        QFile file(out); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(text);
    }
    child<QPushButton>(window, "openOptions")->click();
    auto* exportButton = window.findChild<QPushButton*>("optionExportValuation");
    auto* record = window.findChild<QCheckBox*>("optionRecordResponses");
    QVERIFY(exportButton && record && exportButton->isEnabled() && record->isEnabled() && !record->isChecked());
    record->setChecked(true);
    QVERIFY(ResponseJournal::enabled());
    vault.lock(); // Locking ends recording and leaves nothing to export.
    QVERIFY(!ResponseJournal::enabled());
    QVERIFY(window.valuationDiagnostics().isEmpty());
    QTRY_VERIFY(!exportButton->isEnabled() && !record->isChecked());
}

void LocalWindowTest::displayHidingDefaultsLiveInOptionsAndPersist() {
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("fake-display-options.bin"));
    QVERIFY(vault.create("Fake Display Options! 2026"));
    AccountRecord account; account.provider="binance"; account.alias="FAKE DISPLAY OPTIONS";
    account.environment="live"; account.market="spot"; account.currency="USDT";
    QVERIFY(vault.addAccount(account,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances={QJsonObject{{"asset","USDT"},{"free","10"},{"locked","0"}},
        QJsonObject{{"asset","DUST"},{"free","1"},{"locked","0"}},
        QJsonObject{{"asset","BIG"},{"free","1.23456789"},{"locked","0"}},
        QJsonObject{{"asset","NOPRICE"},{"free","0.0001"},{"locked","0"}}};
    auto* prices=new FreshKrwPriceTransport;
    PortfolioController::Factories factories;
    factories.prices=[prices]{return prices;}; factories.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,factories); AccountModel accounts(&vault);
    {
        LocalWindow window(vault,accounts); window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
        window.show(); child<QTabWidget>(window,"portfolioPages")->setCurrentWidget(child<QWidget>(window,"portfolioHome"));
        QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"portfolioLoading")->isHidden(),4000);
        auto* tree=child<QTreeView>(window,"portfolioAssets");
        const auto group=[tree](const QString& asset) -> PortfolioTestRow {
            for(int i=0;i<tree->model()->rowCount();++i)if(portfolioRow(tree, i).text(0)==asset)return portfolioRow(tree, i);
            return {};
        };
        QTRY_VERIFY_WITH_TIMEOUT(group("DUST")&&group("NOPRICE")&&group("BIG"),3000);
        child<QPushButton>(window,"openOptions")->click();
        auto* options=child<QDialog>(window,"optionsDialog"); QVERIFY(options);
        child<QPushButton>(*options,"optionsDisplayTab")->click();
        auto* small=child<QCheckBox>(*options,"optionHideSmall");
        auto* unvalued=child<QCheckBox>(*options,"optionHideUnvalued");
        QVERIFY(small); QVERIFY(small->isChecked()); QVERIFY(unvalued); QVERIFY(unvalued->isChecked());
        QVERIFY(small->isVisibleTo(options)); QVERIFY(unvalued->isVisibleTo(options));
        QVERIFY(!child<QPushButton>(window,"hideSmallBalances")->isVisibleTo(&window));
        QVERIFY(!child<QPushButton>(window,"hideUnvaluedBalances")->isVisibleTo(&window));
        QVERIFY(group("DUST").isHidden()); QVERIFY(group("NOPRICE").isHidden()); QVERIFY(!group("BIG").isHidden());
        QCOMPARE(child<QLabel>(window,"portfolioTotal")->text(),QStringLiteral("137,307"));
        const auto reads=assets.sent;
        small->setChecked(false); unvalued->setChecked(false);
        QVERIFY(!group("DUST").isHidden()); QVERIFY(!group("NOPRICE").isHidden());
        QCOMPARE(child<QLabel>(window,"portfolioTotal")->text(),QStringLiteral("137,307"));
        QCOMPARE(assets.sent,reads);
        QVERIFY(QSettings().contains("display/hideSmallBalances"));
        QVERIFY(!QSettings().value("display/hideSmallBalances").toBool());
        QVERIFY(!QSettings().value("display/hideUnvaluedBalances").toBool());
    }
    LocalWindow reopened(vault,accounts); reopened.setPortfolioController(&portfolio); reopened.setSecurityStatus(true);
    child<QPushButton>(reopened,"openOptions")->click();
    auto* options=child<QDialog>(reopened,"optionsDialog"); QVERIFY(options);
    QVERIFY(!child<QCheckBox>(*options,"optionHideSmall")->isChecked());
    auto* unvalued=child<QCheckBox>(*options,"optionHideUnvalued"); QVERIFY(unvalued); QVERIFY(!unvalued->isChecked());
}

void LocalWindowTest::publicSmallBalanceThresholdExpiresWithoutAnotherQuote() {
    QTemporaryDir directory;
    Vault vault(directory.filePath("threshold.bin"));
    AccountModel accounts(&vault);
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    CoinWindowPublicTransport http;
    PublicMarketCatalog catalog(&http);
    LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true);
    window.setPublicMarketCatalog(&catalog, false);
    window.show();
    auto* pages = child<QTabWidget>(window, "portfolioPages");
    auto* home = child<QWidget>(window, "portfolioHome");
    auto* toggle = child<QPushButton>(window, "hideSmallBalances");
    QVERIFY(pages && home && toggle);
    QVERIFY(pages->currentWidget() != home);
    QVERIFY(!toggle->isEnabled());
    catalog.refresh();
    QTRY_VERIFY(http.token);
    HttpResult reply;
    reply.status = 200;
    reply.body = R"([{"market":"KRW-USDT","korean_name":"테더","english_name":"Tether","market_event":{"warning":false,"caution":{}}}])";
    emit http.finished(http.token, reply);
    QTRY_VERIFY(http.requests.size() == 2);
    const auto sourceMs = QDateTime::currentMSecsSinceEpoch() - DisplayFreshnessMs + 1200;
    reply.body = QJsonDocument(QJsonArray{QJsonObject{
        {"market", "KRW-USDT"}, {"trade_price", 1385}, {"prev_closing_price", 1385},
        {"signed_change_rate", 0}, {"timestamp", sourceMs}
    }}).toJson();
    emit http.finished(http.token, reply);
    QVERIFY(toggle->isEnabled());
    QCOMPARE(catalog.quoteForMarket("KRW-USDT").price, QStringLiteral("1385"));
    pages->setCurrentWidget(home);
    QVERIFY(toggle->toolTip().contains(QStringLiteral("1,385원")));
    QTRY_VERIFY_WITH_TIMEOUT(!toggle->isEnabled(), 4000);
    QVERIFY(!toggle->toolTip().contains(QStringLiteral("1,385원")));
    QCOMPARE(http.requests.size(), 2);
}

void LocalWindowTest::smallBalancesHideOnlyValuedRowsBelowOneDollar() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-small.bin")); QVERIFY(vault.create("Fake Small UI! 2026"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    // This test covers explicit opt-out and independent toggles; defaults have their own regression.
    QSettings().setValue("display/hideSmallBalances", false);
    QSettings().setValue("display/hideUnvaluedBalances", false);
    AccountRecord a; a.provider = "binance"; a.alias = QStringLiteral("가짜 소액 계좌"); a.environment = "live"; a.market = "spot"; a.currency = "USDT";
    QVERIFY(vault.addAccount(a, {{"api_key", "FAKEAPIKEY123456789"}, {"secret", "FAKESECRET123456789"}}, StorageMode::EncryptedVault));
    AssetReadTransport assets;
    assets.balances = {QJsonObject{{"asset", "USDT"}, {"free", "10"}, {"locked", "0"}}, QJsonObject{{"asset", "DUST"}, {"free", "1"}, {"locked", "0"}},
                       QJsonObject{{"asset", "BIG"}, {"free", "1.23456789"}, {"locked", "0"}}, QJsonObject{{"asset", "NOPRICE"}, {"free", "0.0001"}, {"locked", "0"}}};
    assets.balances.append(QJsonObject{{"asset","KRW"},{"free","0"},{"locked","0"}});
    auto* prices = new FreshKrwPriceTransport;
    PortfolioController::Factories f; f.prices = [prices] { return prices; }; f.trading = [&](const AccountRecord&) { return &assets; };
    PortfolioController portfolio(vault, nullptr, f); AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true);
    auto* tree = child<QTreeView>(window, "portfolioAssets");
    auto* toggle = child<QPushButton>(window, "hideSmallBalances");
    QTRY_VERIFY_WITH_TIMEOUT(toggle->isEnabled(), 4000);
    QVERIFY(toggle->toolTip().contains(QStringLiteral("1 USDT")));
    QVERIFY(toggle->toolTip().contains(QStringLiteral("1,385원")));
    const auto group = [tree](const QString& asset) -> PortfolioTestRow {
        for (int i = 0; i < tree->model()->rowCount(); ++i) if (portfolioRow(tree, i).text(0) == asset) return portfolioRow(tree, i);
        return {}; };
    QTRY_VERIFY_WITH_TIMEOUT(group("DUST") && group("BIG") && group("NOPRICE"), 3000);
    // Initial loading now waits for the complete pricing cycle; hidden rows
    // can already contain prices before the headline is published.
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"portfolioLoading")->isHidden(),3000);
    const auto total = child<QLabel>(window, "portfolioTotal")->text();
    QCOMPARE(total, QStringLiteral("137,307"));
    QCOMPARE(group("BIG").text(2), QStringLiteral("1.23456789"));
    QCOMPARE(group("BIG").text(3), QStringLiteral("123,457"));
    QVERIFY(child<QLabel>(window,"portfolioSummary")->text().contains("137,307.289"));
    QVERIFY(!group("DUST").isHidden());
    toggle->setChecked(true);
    // Under $1 and fully valued: hidden. Unvalued rows never count as small.
    QVERIFY(group("DUST").isHidden());
    QVERIFY(!group("BIG").isHidden()); QVERIFY(!group("USDT").isHidden());
    QVERIFY(!group("NOPRICE").isHidden());
    QCOMPARE(toggle->text(), QStringLiteral("소액 숨김"));
    QCOMPARE(child<QLabel>(window, "portfolioTotal")->text(), total); // Hiding never changes the total.
    QVERIFY(!window.findChild<QLabel*>("portfolioDustSummary"));
    const auto dust=toggle->toolTip();
    QVERIFY(dust.contains(QStringLiteral("숨긴 소액")));
    child<QLineEdit>(window,"portfolioAssetFilterSearch")->setText("BIG");
    QCOMPARE(toggle->toolTip(),dust);
    child<QLineEdit>(window,"portfolioAssetFilterSearch")->clear();
    tree->setCurrentIndex(group("BIG").modelIndex());
    QCOMPARE(child<QLabel>(window,"assetDetailHeadline")->text(), QStringLiteral("123,457 KRW"));
    QVERIFY(child<QLabel>(window,"assetDetailHolding0")->text().contains("123,456.789 KRW"));
    QVERIFY(child<QLabel>(window,"assetDetailIdentity")->isHidden());
    QVERIFY(child<QLabel>(window,"assetDetailHeadlineSub")->isHidden());
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty())for(const auto& size:{QSize(1200,800),QSize(720,640)}){
        window.resize(size);window.show();QCoreApplication::processEvents();QTest::qWait(50);
        QVERIFY(window.grab().save(QDir(preview).filePath(QString("assets-clean-%1.png").arg(size.width()))));
    }
    toggle->setChecked(false);
    QVERIFY(!group("DUST").isHidden());
    auto* unvalued=child<QPushButton>(window,"hideUnvaluedBalances");QVERIFY(unvalued);QVERIFY(!unvalued->isChecked());
    QCOMPARE(group("NOPRICE").text(3),QString("미평가"));QVERIFY(group("KRW"));QCOMPARE(group("KRW").text(3),QString("0"));
    child<QComboBox>(window,"portfolioAssetFilterScope")->setCurrentIndex(1);
    const auto width=unvalued->width();unvalued->setChecked(true);
    QVERIFY(group("NOPRICE").isHidden());QVERIFY(!group("DUST").isHidden());QVERIFY(!group("BIG").isHidden());QVERIFY(!group("KRW").isHidden());
    QCOMPARE(unvalued->width(),width);QCOMPARE(unvalued->text(),QString("미평가 숨김 · 1"));
    QCOMPARE(child<QLabel>(window,"portfolioTotal")->text(),total);
    unvalued->setChecked(false);QVERIFY(!group("NOPRICE").isHidden());
    // Reproduce a projected asset held in two places, with only one row priced.
    unvalued->setChecked(true);
    PortfolioHoldingView priced;
    priced.asset="BIG";priced.componentId="first";priced.accountScope="priced-account";
    priced.quantity="1";priced.value="5000";priced.state=PortfolioValueState::Fresh;priced.presence=AssetPresence::Held;
    auto unknown=priced;
    unknown.componentId="second";unknown.accountScope="unpriced-account";unknown.value.clear();unknown.state=PortfolioValueState::Unvalued;
    PortfolioGroupView holdings;
    holdings.asset="BIG";holdings.identity="exchange:BIG";holdings.freshTotal="5000";holdings.holdings={priced,unknown};
    holdings.presence=AssetPresence::Held;holdings.unvalued=1;
    auto* assetModel=dynamic_cast<PortfolioAssetModel*>(tree->model());QVERIFY(assetModel);
    assetModel->setSnapshot({holdings}, {}, {}, "5000", true);
    toggle->setChecked(!toggle->isChecked());
    const auto mixed=portfolioRow(tree, 0);
    QCOMPARE(mixed.childCount(),2);QVERIFY(!mixed.isHidden());QVERIFY(!mixed.child(0).isHidden());QVERIFY(mixed.child(1).isHidden());
    QCOMPARE(child<QLabel>(window,"portfolioTotal")->text(),total);
    QSettings().remove("display/hideUnvaluedBalances");
    QSettings().remove("display/hideSmallBalances");
    // The home shows live accounts only; the paper/testnet switch is gone.
    QVERIFY(!child<QComboBox>(window, "portfolioEnvironment")->isVisibleTo(&window));
}

void LocalWindowTest::watchlistsPersistByMarketAndDoNotEnableOrders() {
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("fake-watchlist.bin")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories); LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    PublicMarketTransport transport; PublicMarketCatalog markets(&transport);
    FakeBoardTransport boardTransport; PublicEquityBoard board(&boardTransport);
    window.setPublicMarketCatalog(&markets, true); window.setPublicEquityBoard(&board); board.refresh();
    window.setSecurityStatus(true); window.resize(1440, 900); window.show();
    auto* exploreView = child<QComboBox>(window, "exploreView");
    exploreView->setCurrentIndex(exploreView->findData("list"));
    auto* coins = child<SpotlightCard>(window, "spotlightCard_coin");
    auto* watch = child<ExploreWatchlist>(window, "exploreWatchlist");
    QTRY_VERIFY_WITH_TIMEOUT(!coins->visibleItems().isEmpty(), 3000);
    QCOMPARE(coins->mode(), QString("gainers")); QCOMPARE(coins->visibleItems().first().id, QString("PUMP"));
    child<QToolButton>(*coins, "spotlightStar0")->click();
    QVERIFY(watch->contains("coin", "PUMP")); QVERIFY(!watch->contains("us", "PUMP"));
    watch->toggle("kr", "207940"); watch->toggle("us", "NVDA");
    { ExploreWatchlist reopened; QCOMPARE(reopened.ids("coin"), QStringList{"PUMP"}); QCOMPARE(reopened.ids("kr"), QStringList{"207940"}); QCOMPARE(reopened.ids("us"), QStringList{"NVDA"}); }
    // The ranking rows open the chart; map details contain no order preview.
    QVERIFY(!window.findChild<AssetDetailPanel*>("spotlightDetail"));
    QTRY_COMPARE_WITH_TIMEOUT(board.knownItem("coin", "BTC").sourceId, QString("bitcoin"), 3000);
    exploreView->setCurrentIndex(exploreView->findData("map"));
    auto* detail = child<AssetDetailPanel>(window, "marketMapDetail");
    auto* map = child<MarketMapWidget>(window, "marketMap");
    QCOMPARE(map->selectedId(), QString("BTC"));
    auto* displayedQuote = child<ExploreMetricCard>(*detail, "assetDetailMetric");
    QCOMPARE(displayedQuote->data().sourceKey, QString("coin:coingecko:bitcoin"));
    QRectF bitcoinTile;
    for (const auto& tile : map->tileRects()) if (tile.first == "coin:BTC") bitcoinTile = tile.second;
    QVERIFY(!bitcoinTile.isEmpty());
    QVERIFY(map->isVisible());
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, bitcoinTile.center().toPoint());
    QVERIFY(detail->isVisible());
    auto* change = child<QLabel>(*displayedQuote, "metricChange");
    QVERIFY(change->isVisible());
    QTextDocument changeText;
    changeText.setHtml(change->text());
    QVERIFY(changeText.toPlainText().contains("24시간"));
    for (const auto* name : {"tradePreview", "tradePreviewSubmit", "tradePreviewAmount", "tradePreviewMode"})
        QVERIFY(detail->findChildren<QWidget*>(name).isEmpty());
    QVERIFY(detail->findChild<QLabel*>("assetDetailHeadlineSub")->text().contains("24시간"));
    QVERIFY(!child<AssetDetailPanel>(window, "assetDetail")->findChild<QWidget*>("tradePreview"));
    exploreView->setCurrentIndex(exploreView->findData("list"));
    // Changes to the global cap board cannot alter the Upbit ranking.
    boardTransport.coinOverride = FakeBoardTransport::coins();
    boardTransport.coinOverride.replace("0.47", "-9.0").replace("-1.95", "8.0");
    QTRY_VERIFY_WITH_TIMEOUT(!board.busy(), 3000); board.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(coins->visibleItems().first().id, QString("PUMP"), 3000);
    QCOMPARE(child<QLabel>(*detail, "assetDetailTitle")->text(), QString("Bitcoin"));
    coins->setMode("losers"); QCOMPARE(coins->visibleItems().first().id, QString("BTC"));
    // Wait for the global update itself; the independent Upbit ranking has not changed.
    QTRY_COMPARE_WITH_TIMEOUT(board.knownItem("coin","BTC").changePercent.toDouble(),-9.0,3000);
    const auto before = board.knownItem("coin", "BTC").receivedAtMs;
    boardTransport.coinOverride = R"([{"id":"zcash","symbol":"zec","name":"Zcash","market_cap":1234000000,"current_price":1500000,"price_change_percentage_24h":8.0}])";
    QTRY_VERIFY_WITH_TIMEOUT(!board.busy(), 3000); board.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(board.items("coin").size(), 1, 3000);
    QCOMPARE(board.knownItem("coin", "BTC").receivedAtMs, before);
    auto* tableAt = child<QTableWidget>(*watch, "watchlistTable");
    QVERIFY(tableAt->item(0,1)->text().contains("PUMP")||tableAt->item(0,1)->data(Qt::UserRole).toString()=="PUMP");
    // Long Korean names and six/seven digit prices occupy separate bounded boxes.
    auto* kr = child<SpotlightCard>(window, "spotlightCard_kr");
    kr->setModes({{"gainers", "상승률 ↓", {{"207940", "삼성바이오로직스", "Vol 1854억원", "1,386,000원", "6.78"}}}});
    const auto preview = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if (!preview.isEmpty()) { QCoreApplication::processEvents(); QVERIFY(kr->grab().save(QDir(preview).filePath("spotlight-long-name.png")));
        QVERIFY(window.grab().save(QDir(preview).filePath("watchlist-layout.png"))); }
    // Independent internal scrolling; removing an item survives construction again.
    for (int i = 0; i < 35; ++i) watch->toggle("coin", QString("FAKE%1").arg(i));
    watch->setSection("coin"); QCoreApplication::processEvents();
    auto* table = child<QTableWidget>(*watch, "watchlistTable");
    QTRY_VERIFY(table->verticalScrollBar()->maximum() > 0);
    table->verticalScrollBar()->setValue(table->verticalScrollBar()->maximum());
    const auto at = table->verticalScrollBar()->value();
    emit board.changed(); QCOMPARE(table->verticalScrollBar()->value(), at);
    window.lockAndClear(); QVERIFY(watch->contains("coin", "PUMP"));
    watch->toggle("coin", "PUMP");
    { ExploreWatchlist reopened; QVERIFY(!reopened.contains("coin", "PUMP")); QVERIFY(reopened.contains("us", "NVDA")); }
}

void LocalWindowTest::livePublicExplorePreview() {
    if (qEnvironmentVariable("KEEFI_PUBLIC_PREVIEW") != "1") QSKIP("Explicit public network preview only");
    const auto output = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR"); QVERIFY(!output.isEmpty());
    QTemporaryDir dir; QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("empty.bin")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories); LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    NetworkTradingTransport marketTransport, boardTransport, historyTransport;
    PublicMarketCatalog markets(&marketTransport); PublicEquityBoard board(&boardTransport); PublicMarketHistory history(&historyTransport);
    window.setPublicMarketCatalog(&markets, true); window.setPublicEquityBoard(&board); window.setPublicMarketHistory(&history);
    window.setSecurityStatus(true); window.resize(1440, 1000); window.show();
    child<QComboBox>(window, "exploreView")->setCurrentIndex(1); board.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!board.busy(), 30000);
    QVERIFY2(!board.items("coin").isEmpty(), qPrintable(board.status("coin")));
    QVERIFY2(!board.items("kr").isEmpty(), qPrintable(board.status("kr")));
    QVERIFY2(!board.items("us").isEmpty(), qPrintable(board.status("us")));
    auto* watch = child<ExploreWatchlist>(window, "exploreWatchlist");
    for (const auto& id : {"BTC", "ETH", "SOL"}) watch->toggle("coin", id);
    for (const auto& id : {"005930", "207940", "000660"}) watch->toggle("kr", id);
    for (const auto& id : {"NVDA", "AAPL", "MSFT"}) watch->toggle("us", id);
    for (const auto& key : {QString("coin"), QString("kr"), QString("us")}) {
        watch->setSection(key);
        for (const auto& id : watch->ids(key)) {
            if(key=="coin")QTRY_VERIFY_WITH_TIMEOUT(!history.history(key,id).points.isEmpty(),25000);
            else {QVERIFY(history.history(key,id).points.isEmpty());QCOMPARE(history.history(key,id).status,QStringLiteral("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요"));}
        }
        QTest::qWait(400);
        QVERIFY(window.grab().save(QDir(output).filePath("live-watchlist-" + key + ".png")));
    }
    watch->setSection("kr");
    window.resize(1400, 833); QTest::qWait(150); QVERIFY(window.grab().save(QDir(output).filePath("live-1400.png")));
    window.resize(1200, 800); QTest::qWait(150); QVERIFY(window.grab().save(QDir(output).filePath("live-1200.png")));
    window.resize(720, 640); QTest::qWait(150); QVERIFY(window.grab().save(QDir(output).filePath("live-720.png")));
    QVERIFY(child<QComboBox>(window, "exploreMarketSection")->height() < 60);
    QVERIFY(child<QTableWidget>(window, "watchlistTable")->viewport()->height() >= 58);
    QVERIFY(!vault.exists()); QCOMPARE(accounts.rowCount(), 0);
}

void LocalWindowTest::dustTotalsUseExactDecimalsAndKeepUnknowns() {
    PortfolioPresentation view;view.summary.knownSubtotal="2.5";
    const auto group=[](QString id,QString value,PortfolioValueState state){PortfolioGroupView g;g.identity=id;g.freshTotal=value;
        PortfolioHoldingView h;h.value=value;h.quantity="1";h.state=state;h.presence=AssetPresence::Held;g.holdings={h};return g;};
    view.groups={group("below","0.999999999999999999",PortfolioValueState::Fresh),group("equal","1",PortfolioValueState::Fresh),
        group("negative","-0.5",PortfolioValueState::Fresh),group("unknown","0",PortfolioValueState::Unvalued)};
    const auto dust=portfolioDust(view,"1",true);QCOMPARE(dust.hiddenIdentities,QStringList{"below"});
    QCOMPARE(dust.hidden,QString("0.999999999999999999"));QCOMPARE(dust.displayed,QString("1.500000000000000001"));QCOMPARE(dust.subtotal,QString("2.5"));
    QCOMPARE(portfolioDust(view,"",true).hidden,QString("0"));QCOMPARE(portfolioDust(view,"1",false).displayed,QString("2.5"));
}

void LocalWindowTest::krwDisplayRoundsWithoutChangingExactAmounts_data() {
    QTest::addColumn<QString>("source");QTest::addColumn<QString>("display");
    QTest::newRow("integer")<<QString("1234567")<<QString("1,234,567");
    QTest::newRow("below-half-exact")<<QString("1234567.499999999999999999")<<QString("1,234,567");
    QTest::newRow("half")<<QString("1234567.5")<<QString("1,234,568");
    QTest::newRow("carry")<<QString("999999.999")<<QString("1,000,000");
    QTest::newRow("beyond-double-precision")<<QString("9007199254740993.5")<<QString("9,007,199,254,740,994");
    QTest::newRow("negative")<<QString("-1234.5")<<QString("-1,235");
    QTest::newRow("negative-near-zero")<<QString("-0.49")<<QString("0");
    QTest::newRow("negative-zero")<<QString("-0.000")<<QString("0");
    QTest::newRow("trailing-zeros")<<QString("00012.000")<<QString("12");
    QTest::newRow("tiny")<<QString("0.000000001")<<QString("0");
    QTest::newRow("unknown")<<QString("—")<<QString("—");
    QTest::newRow("missing")<<QString{}<<QString{};
}

void LocalWindowTest::krwDisplayRoundsWithoutChangingExactAmounts() {
    QFETCH(QString,source);QFETCH(QString,display);
    const auto exact=source;
    QCOMPARE(krwAmount(source),display);
    QCOMPARE(source,exact);
    QCOMPARE(priceAmount("0.00048000"),QString("0.00048"));
    QCOMPARE(groupedAmount("1.23456789"),QString("1.23456789"));
}

void LocalWindowTest::zeroFuturesEquityDoesNotBlockDustGroups_data() {
    QTest::addColumn<QString>("pnl");QTest::addColumn<bool>("positions");QTest::addColumn<bool>("complete");QTest::addColumn<bool>("hidden");QTest::addColumn<qint64>("age");
    QTest::newRow("flat-zero-equity")<<QString("0")<<false<<true<<true<<qint64(0);
    QTest::newRow("opposing-positions-stay-visible")<<QString("0")<<true<<true<<true<<qint64(0);
    QTest::newRow("missing-pnl")<<QString{}<<false<<true<<false<<qint64(0);
    QTest::newRow("negative-pnl")<<QString("-1")<<false<<true<<false<<qint64(0);
    QTest::newRow("positive-pnl")<<QString("1")<<false<<true<<false<<qint64(0);
    QTest::newRow("invalid-pnl")<<QString("unknown")<<false<<true<<false<<qint64(0);
    QTest::newRow("incomplete-balance")<<QString("0")<<false<<false<<false<<qint64(0);
    QTest::newRow("expired-balance")<<QString("0")<<false<<true<<false<<qint64(DisplayFreshnessMs+1);
}

void LocalWindowTest::zeroFuturesEquityDoesNotBlockDustGroups() {
    QFETCH(QString,pnl);QFETCH(bool,positions);QFETCH(bool,complete);QFETCH(bool,hidden);QFETCH(qint64,age);
    const auto now=QDateTime::currentMSecsSinceEpoch();
    AccountRecord spot;spot.id="fake-spot";spot.provider="binance";spot.environment="live";spot.market="spot";
    AccountRecord futures=spot;futures.id="fake-futures";futures.market="usdm_futures";
    MarketSnapshot cash;Balance small;small.asset="DUST";small.free="1";small.locked="0";cash.balances.append(small);
    Balance zero=small;zero.asset="ZERO";zero.free="0";cash.balances.append(zero);
    MarketSnapshot margin;QJsonArray assets;
    for(const auto& asset:{"DUST","ZERO","ONLYZERO"}){
        Balance b;b.asset=asset;b.equity="0";margin.balances.append(b);
        assets.append(QJsonObject{{"asset",asset},{"unrealizedProfit",pnl}});
    }
    margin.data[Operation::Account]=QJsonObject{{"assets",assets}};
    if(positions){PositionRecord longPosition;longPosition.symbol="FAKEUSDT";longPosition.positionSide="LONG";longPosition.amount="1";
        auto shortPosition=longPosition;shortPosition.positionSide="SHORT";shortPosition.amount="-1";margin.positions={longPosition,shortPosition};}
    const auto spotSnapshot=captureExchangeAssets(spot,"fake-spot-scope",cash,now,0,true,true,"synthetic",{}).snapshot;
    const auto futuresSnapshot=captureExchangeAssets(futures,"fake-futures-scope",margin,now-age,0,complete,true,"synthetic",{}).snapshot;
    const QList<AssetSnapshot> snapshots{spotSnapshot,futuresSnapshot};
    const AssetQuote price{"DUST","KRW","12.5",{"fake public","https://example.invalid","public:upbit:KRW-DUST",now,now,true}};
    const auto valuation=AssetValuation::value(snapshots,{price},"KRW",now,DisplayFreshnessMs);
    const auto view=presentPortfolio(snapshots,valuation,{});
    const auto dust=portfolioDust(view,"1000",true);
    QCOMPARE(dust.hiddenIdentities.contains("exchange:DUST"),hidden);
    QCOMPARE(dust.hiddenIdentities.contains("exchange:ZERO"),hidden);
    QCOMPARE(dust.hiddenIdentities.contains("exchange:ONLYZERO"),hidden);
    QCOMPARE(dust.subtotal,QString("12.5"));QCOMPARE(dust.hidden,hidden?QString("12.5"):QString("0"));
    QCOMPARE(dust.displayed,hidden?QString("0"):QString("12.5"));
    const auto zeroGroup=std::find_if(view.groups.cbegin(),view.groups.cend(),[](const auto& g){return g.asset=="ONLYZERO";});
    QVERIFY(zeroGroup!=view.groups.cend());QCOMPARE(zeroGroup->presence,hidden?AssetPresence::Zero:AssetPresence::NeedsCheck);
    if(positions){
        const auto group=std::find_if(view.groups.cbegin(),view.groups.cend(),[](const auto& g){return g.asset=="FAKEUSDT";});
        QVERIFY(group!=view.groups.cend());QCOMPARE(group->holdings.size(),2);QCOMPARE(group->presence,AssetPresence::Held);
        QVERIFY(!dust.hiddenIdentities.contains(group->identity));
        for(const auto& h:group->holdings)QCOMPARE(h.state,PortfolioValueState::NotCounted);
        AssetDetailPanel detail;detail.showGroup(*group);
        QCOMPARE(child<QLabel>(detail,"assetDetailHeadline")->text(),QString("합계 제외"));
    }
}

void LocalWindowTest::workbenchStaysHonestWithLargeAndFailingData() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-stress.bin")); QVERIFY(vault.create("Fake Stress UI! 2026"));
    AccountRecord big; big.provider = "binance"; big.alias = QStringLiteral("가짜 아주 긴 계좌 이름 · 장기 보관용 메인 거래소 계정");
    big.environment = "live"; big.market = "spot"; big.currency = "USDT";
    AccountRecord broken; broken.provider = "upbit"; broken.alias = QStringLiteral("가짜 실패 업비트"); broken.environment = "live"; broken.market = "spot"; broken.currency = "KRW";
    QVERIFY(vault.addAccount(big, {{"api_key", "FAKEAPIKEY123456789"}, {"secret", "FAKESECRET123456789"}}, StorageMode::EncryptedVault));
    QVERIFY(vault.addAccount(broken, {{"access_key", "FAKEACCESS123456789"}, {"secret", "FAKESECRET123456789"}}, StorageMode::EncryptedVault));
    AssetReadTransport good, bad; bad.fail = true;
    good.balances = {QJsonObject{{"asset", "KRW"}, {"free", "1234567890"}, {"locked", "0"}}};
    for (int i = 0; i < 60; ++i) good.balances.append(QJsonObject{{"asset", QStringLiteral("TKN%1").arg(i)}, {"free", "123456.789"}, {"locked", "0"}});
    PortfolioController::Factories f; f.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    f.trading = [&](const AccountRecord& account) { return account.provider == "upbit" ? &bad : &good; };
    PortfolioController portfolio(vault, nullptr, f); AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio); window.setSecurityStatus(true); window.resize(1200, 800); window.show();
    auto* tree = child<QTreeView>(window, "portfolioAssets");
    QTRY_VERIFY_WITH_TIMEOUT(tree->model()->rowCount() >= 60, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(window, "trustPill")->text().contains(QStringLiteral("확인 필요")), 3000);
    // The confirmed KRW is the figure; unvalued coins and the failed account are chips, never folded into it.
    QTRY_COMPARE_WITH_TIMEOUT(child<QLabel>(window, "portfolioTotal")->text(), QStringLiteral("1,234,567,890"),4000);
    QVERIFY(child<QLabel>(window, "portfolioTotalUnit")->isVisible());
    QVERIFY(!child<QPushButton>(window, "trustPill")->text().contains(QStringLiteral("모두 확인")));
    QVERIFY(!child<QPushButton>(window, "connectionProblemBanner")->isVisible());
    auto* connections = child<QListView>(window, "accountList")->model();
    bool failedRow = false;
    for(int row=0;row<connections->rowCount();++row)
        failedRow |= connections->index(row,0).data(Qt::AccessibleTextRole).toString().contains("조회 실패");
    QVERIFY(failedRow);
    // A card never scrolls sideways; long names elide inside their column.
    QCoreApplication::processEvents();
    QVERIFY(!tree->horizontalScrollBar()->isVisible());
    QVERIFY(!window.findChild<QPushButton*>("stopPortfolioReads"));
    const auto preview = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    auto* pages = child<QTabWidget>(window, "portfolioPages");
    if (!preview.isEmpty()) {
        tree->setCurrentIndex(portfolioRow(tree, 0).modelIndex());
        for (const auto& size : {QSize(1200, 800), QSize(720, 640)}) {
            window.resize(size); QCoreApplication::processEvents(); QTest::qWait(50);
            QVERIFY(window.grab().save(QDir(preview).filePath(QString("g6-stress-assets-%1.png").arg(size.width()))));
            pages->setCurrentIndex(1); QCoreApplication::processEvents(); QTest::qWait(50);
            QVERIFY(window.grab().save(QDir(preview).filePath(QString("g6-stress-connections-%1.png").arg(size.width()))));
            pages->setCurrentIndex(0);
        }
        window.resize(1200, 800);
    }
    // Locked: no figure, no pill, nothing personal left on screen.
    vault.lock();
    QCOMPARE(child<QLabel>(window, "portfolioTotal")->text(), QStringLiteral("—"));
    QVERIFY(!child<QLabel>(window, "portfolioTotalUnit")->isVisible());
    QVERIFY(!child<QPushButton>(window, "trustPill")->isVisible());
    QCOMPARE(tree->model()->rowCount(), 0);
    if (!preview.isEmpty()) QVERIFY(window.grab().save(QDir(preview).filePath("g6-stress-locked-1200.png")));
}

void LocalWindowTest::passwordCompositionRequiredOnlyForNewPasswords() {
    const QString legacyPassword = "old lowercase passphrase";
    const QString strongPassword = "Fake-Vault-2026!";
    const QStringList invalid = {"lowercase123!", "UPPERCASE123!", "LongPassword!",
                                 "LongPassword123", "Long Password123", "Aa1!short"};
    for (const auto mode : {VaultDialog::Mode::Create, VaultDialog::Mode::ChangePassword}) {
        VaultDialog dialog(mode);
        auto* current = child<QLineEdit>(dialog, "currentPassword");
        if (current) current->setText(legacyPassword);
        auto* password = child<QLineEdit>(dialog, "newPassword");
        auto* confirm = child<QLineEdit>(dialog, "confirmPassword");
        auto* submit = child<QPushButton>(dialog, "submitVault");
        auto* error = child<QLabel>(dialog, "vaultError");
        for (const auto& value : invalid) {
            password->setText(value); confirm->setText(value);
            QVERIFY2(!submit->isEnabled(), "Each missing password requirement must prevent creation or change.");
            QVERIFY(!error->text().isEmpty());
        }
        password->setText(strongPassword); confirm->setText(strongPassword + "different");
        QVERIFY(!submit->isEnabled()); QVERIFY(!error->text().isEmpty());
        confirm->setText(strongPassword);
        QVERIFY(submit->isEnabled()); QVERIFY(error->text().isEmpty());
        if (current) QCOMPARE(current->text(), legacyPassword);
        holdReveal(child<PressRevealButton>(dialog, "newPasswordReveal"));
        dialog.setError(QStringLiteral("가짜 금고 작업 실패"));
        QCOMPARE(error->text(), QStringLiteral("가짜 금고 작업 실패"));
        QVERIFY(password->text().isEmpty()); QVERIFY(confirm->text().isEmpty());
        if (current) QVERIFY(current->text().isEmpty());
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(!submit->isEnabled());
    }
    VaultDialog unlock(VaultDialog::Mode::Unlock);
    child<QLineEdit>(unlock, "currentPassword")->setText(legacyPassword);
    QVERIFY(child<QPushButton>(unlock, "submitVault")->isEnabled());
    QVERIFY(!child<QLineEdit>(unlock, "newPassword"));
}

void LocalWindowTest::oversizedSecretsCannotBecomeValidTruncatedPrefixes() {
    VaultDialog create(VaultDialog::Mode::Create);
    auto* password = child<QLineEdit>(create, "newPassword");
    auto* confirm = child<QLineEdit>(create, "confirmPassword");
    password->setText(QString(5000, 'x'));
    confirm->setText(QString(5000, 'x'));
    QVERIFY(password->text().size() > Vault::MaxPasswordBytes);
    QVERIFY(!child<QPushButton>(create, "submitVault")->isEnabled());
    QVERIFY(child<QLabel>(create, "vaultError")->text().contains("1024"));
    VaultDialog unlock(VaultDialog::Mode::Unlock);
    child<QLineEdit>(unlock, "currentPassword")->setText(QString(600, QChar(0xD55C)));
    QVERIFY(!child<QPushButton>(unlock, "submitVault")->isEnabled());
    QVERIFY(child<QLabel>(unlock, "vaultError")->text().contains("1024"));
    AccountDialog account(true);
    fillFakeKeys(account, "upbit");
    auto* secret = child<QLineEdit>(account, "credential_secret");
    secret->setText(QString(16000, 'x'));
    QVERIFY(secret->text().size() > 8192);
    QVERIFY(!child<QPushButton>(account, "saveAccount")->isEnabled());
    QVERIFY(child<QLabel>(account, "accountError")->text().contains("8192"));
    secret->setText(QString(3000, QChar(0xD55C)));
    QVERIFY(!child<QPushButton>(account, "saveAccount")->isEnabled());
    secret->setText("FAKE-ONLY-valid-length");
    QVERIFY(child<QPushButton>(account, "saveAccount")->isEnabled());
}

void LocalWindowTest::accountReplaceDeleteAndLockUseVault() {
    QTemporaryDir directory;
    Vault vault(QDir(QDir(directory.path()).canonicalPath()).filePath("test-vault.bin"));
    QVERIFY(vault.create("Fake Replace UI! 2026"));
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    window.setSecurityStatus(true);
    child<QPushButton>(window, "addAccount")->click();
    auto* dialog = window.findChild<AccountDialog*>();
    QVERIFY(dialog);
    fillFakeKeys(*dialog, "upbit");
    child<QPushButton>(*dialog, "saveAccount")->click();
    QCOMPARE(accounts.rowCount(), 1);
    QCOMPARE(accounts.recordAt(0).storageMode, StorageMode::EncryptedVault);
    QCOMPARE(accounts.recordAt(0).connectionState, QString("not_checked"));
    QVERIFY(vault.exists());
    QCOMPARE(child<QLabel>(window, "connectionReadState")->text(), QString("조회 대기"));
    processDeletion();
    child<QPushButton>(window, "replaceCredentials")->click();
    dialog = window.findChild<AccountDialog*>();
    QVERIFY(dialog);
    QVERIFY(!child<QComboBox>(*dialog, "providerSelector")->isEnabled());
    QVERIFY(child<QLineEdit>(*dialog, "credential_secret")->text().isEmpty());
    child<QLineEdit>(*dialog, "credential_access_key")->setText("FAKE-ONLY-replaced-access");
    child<QLineEdit>(*dialog, "credential_secret")->setText("FAKE-ONLY-replaced-secret");
    child<QPushButton>(*dialog, "saveAccount")->click();
    QCOMPARE(accounts.rowCount(), 1);
    processDeletion();
    child<QPushButton>(window, "removeAccount")->click();
    auto* confirm = child<QDialog>(window, "removeAccountConfirmation");
    QVERIFY(confirm);
    child<QPushButton>(*confirm, "confirmRemoveAccount")->click();
    QCOMPARE(accounts.rowCount(), 0);
    processDeletion();
    child<QPushButton>(window, "addAccount")->click();
    dialog = window.findChild<AccountDialog*>();
    QVERIFY(dialog);
    fillFakeKeys(*dialog, "upbit", "second");
    child<QPushButton>(*dialog, "saveAccount")->click();
    QCOMPARE(accounts.rowCount(), 1);
    processDeletion();
    child<QPushButton>(window, "lockVault")->click();
    QCOMPARE(accounts.rowCount(), 0);
    QVERIFY(!vault.isUnlocked());
    QVERIFY(!child<QPushButton>(window, "replaceCredentials")->isEnabled());
}

void LocalWindowTest::vaultButtonsRouteCreateChangeUnlock() {
    QTemporaryDir directory;
    Vault vault(QDir(QDir(directory.path()).canonicalPath()).filePath("test-vault.bin"));
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    window.setSecurityStatus(true);
    child<QPushButton>(window, "createVault")->click();
    auto* dialog = window.findChild<VaultDialog*>();
    QVERIFY(dialog);
    child<QLineEdit>(*dialog, "newPassword")->setText("Fake-password-one-2026!");
    child<QLineEdit>(*dialog, "confirmPassword")->setText("Fake-password-one-2026!");
    QVERIFY2(child<QPushButton>(*dialog, "submitVault")->isEnabled(), qPrintable(child<QLabel>(*dialog, "vaultError")->text()));
    child<QPushButton>(*dialog, "submitVault")->click();
    QVERIFY2(vault.isUnlocked(), qPrintable(child<QLabel>(window, "localFeedback")->text()));
    QVERIFY(vault.exists());
    QVERIFY(child<QLineEdit>(*dialog, "newPassword")->text().isEmpty());
    processDeletion();
    child<QPushButton>(window, "changeVaultPassword")->click();
    dialog = window.findChild<VaultDialog*>();
    QVERIFY(dialog);
    child<QLineEdit>(*dialog, "currentPassword")->setText("Fake-password-one-2026!");
    child<QLineEdit>(*dialog, "newPassword")->setText("Fake-password-two-2026!");
    child<QLineEdit>(*dialog, "confirmPassword")->setText("Fake-password-two-2026!");
    child<QPushButton>(*dialog, "submitVault")->click();
    QVERIFY(child<QLineEdit>(*dialog, "currentPassword")->text().isEmpty());
    processDeletion();
    window.lockAndClear();
    QVERIFY(!vault.isUnlocked());
    child<QPushButton>(window, "unlockVault")->click();
    dialog = window.findChild<VaultDialog*>();
    QVERIFY(dialog);
    child<QLineEdit>(*dialog, "currentPassword")->setText("fake-wrong-password");
    child<QPushButton>(*dialog, "submitVault")->click();
    QVERIFY(!vault.isUnlocked());
    QVERIFY(!dialog->isVisible());
    QVERIFY(child<QLineEdit>(*dialog, "currentPassword")->text().isEmpty());
    QVERIFY(!child<QLabel>(window, "localFeedback")->text().isEmpty());
    processDeletion();
    child<QPushButton>(window, "unlockVault")->click();
    dialog = window.findChild<VaultDialog*>();
    QVERIFY(dialog);
    child<QLineEdit>(*dialog, "currentPassword")->setText("Fake-password-two-2026!");
    child<QPushButton>(*dialog, "submitVault")->click();
    QVERIFY(vault.isUnlocked());
    QVERIFY(child<QLineEdit>(*dialog, "currentPassword")->text().isEmpty());
    processDeletion();
}

void LocalWindowTest::securityLossClosesSecretInput() {
    QTemporaryDir directory;
    Vault vault(QDir(QDir(directory.path()).canonicalPath()).filePath("test-vault.bin"));
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    window.setSecurityStatus(true);
    child<QPushButton>(window, "addAccount")->click();
    auto* dialog = window.findChild<AccountDialog*>();
    QVERIFY(dialog);
    fillFakeKeys(*dialog, "upbit");
    holdReveal(child<PressRevealButton>(*dialog, "reveal_secret"));
    window.setSecurityStatus(false, "TEST_SECURITY_UNAVAILABLE");
    QVERIFY(!dialog->isVisible());
    QVERIFY(child<QLineEdit>(*dialog, "credential_secret")->text().isEmpty());
    QVERIFY(!child<QPushButton>(window, "addAccount")->isEnabled());
    QCOMPARE(accounts.rowCount(), 0);
    processDeletion();
}

void LocalWindowTest::brokerWindowEntryNeverAuthenticatesAndHonorsSecurityGate_data() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("environment");
    QTest::newRow("toss-live") << QString("toss") << QString("live");
    QTest::newRow("kis-live") << QString("kis") << QString("live");
    QTest::newRow("kis-retired-paper-rejected") << QString("kis") << QString("paper");
    QTest::newRow("kiwoom-live") << QString("kiwoom") << QString("live");
    QTest::newRow("kiwoom-retired-paper-rejected") << QString("kiwoom") << QString("paper");
    QTest::newRow("ls-live") << QString("ls") << QString("live");
    QTest::newRow("ls-retired-paper-rejected") << QString("ls") << QString("paper");
}

void LocalWindowTest::brokerWindowEntryNeverAuthenticatesAndHonorsSecurityGate() {
    QFETCH(QString, provider); QFETCH(QString, environment);
    QTemporaryDir directory;
    Vault vault(directory.filePath("fake-vault")); QVERIFY(vault.create("Fake Broker Entry! 2026"));
    AccountRecord account; account.provider = provider; account.alias = "가짜 국내주식 연결";
    account.environment = environment; account.market = "domestic_equity"; account.currency = "KRW";
    const QMap<QString, QString> credentials = provider == "kis"
        ? QMap<QString, QString>{{"app_key", "FAKE-KIS-KEY"}, {"app_secret", "FAKE-KIS-SECRET"}, {"account_number", "12345678"}, {"product_code", "01"}}
        : provider == "kiwoom" ? QMap<QString, QString>{{"app_key", "FAKE-KIWOOM-KEY"}, {"app_secret", "FAKE-KIWOOM-SECRET"}, {"account_number", "0012345678"}}
        : provider == "ls" ? QMap<QString, QString>{{"app_key", "FAKE-LS-KEY"}, {"app_secret", "FAKE-LS-SECRET"}}
        : QMap<QString, QString>{{"client_id", "FAKE-ONLY-ID"}, {"client_secret", "FAKE-ONLY-SECRET"}};
    if (environment == "paper" || AccountModel::isProviderSuspended(provider)) {
        QVERIFY(!vault.addAccount(account, credentials, StorageMode::EncryptedVault));
        QVERIFY(vault.accounts().isEmpty());
        return;
    }
    QVERIFY(vault.addAccount(account, credentials, StorageMode::EncryptedVault));
    AccountModel accounts(&vault);
    NoNetworkBrokerTransport transport;
    BrokerTradingController controller(vault, &transport);
    LocalWindow window(vault, accounts);
    QVERIFY(!window.findChild<QPushButton*>("openTrading"));
    window.setSecurityStatus(true);
    QVERIFY(!window.findChild<QPushButton*>("openTrading"));
    QCOMPARE(transport.sent, 0);
    QVERIFY(!controller.authenticated()); QVERIFY(!controller.connected()); QVERIFY(!controller.liveEnabled());
    window.lockAndClear();
    QCOMPARE(transport.sent, 0); QVERIFY(!controller.authenticated()); QVERIFY(!controller.liveEnabled());
    QVERIFY(!window.findChild<QPushButton*>("openTrading")); QCOMPARE(accounts.rowCount(), 0);
}

void LocalWindowTest::lockedVaultBlocksSavingUntilOpened() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-locked-save.bin"));
    QVERIFY(vault.create("Fake Locked Save! 2026")); vault.lock();
    AccountModel accounts(&vault); LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.show();
    QVERIFY(!window.findChild<QPushButton*>("saveConnectionToVault"));
    child<QPushButton>(window,"addAccount")->click();
    auto* account=child<AccountDialog>(window,"accountDialog"); QVERIFY(account); fillFakeKeys(*account,"upbit");
    QVERIFY(!account->findChild<QComboBox*>("storageSelector"));
    QVERIFY(!child<QPushButton>(*account,"saveAccount")->isEnabled());
    QVERIFY(child<QPushButton>(*account,"openVaultForAccount")->isVisible());
    QVERIFY(child<QLabel>(*account,"storageHint")->text().contains("금고에만"));
    QVERIFY(vault.accounts().isEmpty());
    account->reject(); QVERIFY(child<QLineEdit>(*account,"credential_secret")->text().isEmpty());
}

void LocalWindowTest::openVaultInsideAccountDialogPreservesDraft() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-inline-ui.bin"));
    AccountModel accounts(&vault); LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.show();
    child<QPushButton>(window,"addAccount")->click();
    auto* account=child<AccountDialog>(window,"accountDialog"); QVERIFY(account); fillFakeKeys(*account,"upbit");
    const auto secret=child<QLineEdit>(*account,"credential_secret")->text();
    QVERIFY(!child<QPushButton>(*account,"saveAccount")->isEnabled());
    child<QPushButton>(*account,"openVaultForAccount")->click();
    auto* dialog=child<VaultDialog>(*account,"vaultDialog"); QVERIFY(dialog);
    const QString password="Fake Inline UI! 2026";
    QCOMPARE(QApplication::activeModalWidget(),dialog);QCOMPARE(dialog->parentWidget(),account);
    child<QLineEdit>(*dialog,"newPassword")->setText(password);
    child<QLineEdit>(*dialog,"confirmPassword")->setText(password);
    child<QPushButton>(*dialog,"submitVault")->click();
    QVERIFY(vault.isUnlocked()); QVERIFY(account->isVisible());
    QTRY_COMPARE(QApplication::activeModalWidget(),account);
    QCOMPARE(child<QLineEdit>(*account,"credential_secret")->text(),secret);
    QVERIFY(child<QPushButton>(*account,"saveAccount")->isEnabled());
    child<QPushButton>(*account,"saveAccount")->click();
    QCOMPARE(vault.accounts().size(),1); QCOMPARE(vault.accounts().first().storageMode,StorageMode::EncryptedVault);
    vault.lock(); QVERIFY(vault.unlock(password)); QCOMPARE(vault.accounts().size(),1);
}

void LocalWindowTest::nestedVaultDialogClearsBothInputsOnSecurityLoss() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-nested-lock.bin"));
    AccountModel accounts(&vault); LocalWindow window(vault,accounts); window.setSecurityStatus(true); window.show();
    child<QPushButton>(window,"addAccount")->click();
    auto* account=child<AccountDialog>(window,"accountDialog"); fillFakeKeys(*account,"upbit");
    child<QPushButton>(*account,"openVaultForAccount")->click();
    auto* dialog=child<VaultDialog>(*account,"vaultDialog"); QVERIFY(dialog);
    child<QLineEdit>(*dialog,"newPassword")->setText("Fake Nested UI! 2026");
    window.setSecurityStatus(false,"FAKE_SECURITY_LOSS");
    QVERIFY(!dialog->isVisible()); QVERIFY(!account->isVisible());
    QVERIFY(child<QLineEdit>(*dialog,"newPassword")->text().isEmpty());
    QVERIFY(child<QLineEdit>(*account,"credential_secret")->text().isEmpty());
    QVERIFY(!vault.exists());
    QTRY_VERIFY(!QApplication::activeModalWidget());
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    window.setSecurityStatus(true);child<QPushButton>(window,"addAccount")->click();
    auto* reopened=child<AccountDialog>(window,"accountDialog");QVERIFY(reopened);QVERIFY(reopened->isVisible());
    QCOMPARE(QApplication::activeModalWidget(),reopened);
    child<QPushButton>(*reopened,"openVaultForAccount")->click();
    auto* next=child<VaultDialog>(*reopened,"vaultDialog");QVERIFY(next);QCOMPARE(QApplication::activeModalWidget(),next);
    window.lockAndClear();QTRY_VERIFY(!QApplication::activeModalWidget());
}

void LocalWindowTest::openVaultSavesEncryptedWithExplicitFeedback() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-storage-message.bin")); QVERIFY(vault.create("Fake Message UI! 2026"));
    AccountModel accounts(&vault); LocalWindow window(vault,accounts); window.setSecurityStatus(true);
    child<QPushButton>(window,"addAccount")->click();
    auto* account=child<AccountDialog>(window,"accountDialog"); fillFakeKeys(*account,"upbit");
    QVERIFY(!child<QPushButton>(*account,"openVaultForAccount")->isVisible());
    QVERIFY(child<QLabel>(*account,"storageHint")->text().contains("암호화 금고"));
    child<QPushButton>(*account,"saveAccount")->click();
    QCOMPARE(vault.accounts().first().storageMode,StorageMode::EncryptedVault);
    QVERIFY(child<QLabel>(window,"localFeedback")->text().contains("저장"));
    QVERIFY(!child<QLabel>(window,"accountDetails")->text().contains("이번 실행"));
}

void LocalWindowTest::holdingsFilterUsesExactQuantitiesAndKeepsUnknowns() {
    QCOMPARE(assetPresence({"0","0.000"}),AssetPresence::Zero);
    QCOMPARE(assetPresence({"-0.00"}),AssetPresence::Zero);
    QCOMPARE(assetPresence({"0","0.000000000000000000000000001"}),AssetPresence::Held);
    QCOMPARE(assetPresence({"0","2"}),AssetPresence::Held); // Locked funds.
    QCOMPARE(assetPresence({"-2"}),AssetPresence::Held);
    QCOMPARE(assetPresence({"2","-2"}),AssetPresence::Held); // Never net source rows.
    QCOMPARE(assetPresence({""}),AssetPresence::NeedsCheck);
    QCOMPARE(assetPresence({"0","unknown"}),AssetPresence::NeedsCheck);
    QCOMPARE(assetPresence({"0"},{"-4"}),AssetPresence::Held); // Debt despite zero free balance.
    QCOMPARE(assetDisplayOrder({AssetPresence::Zero,AssetPresence::Held,AssetPresence::NeedsCheck,AssetPresence::Held}),QList<int>({1,3,2,0}));
    QTableWidget table(2,1); table.setItem(0,0,new QTableWidgetItem("ZERO")); table.setItem(1,0,new QTableWidgetItem("UNKNOWN"));
    AssetListFilter filter("fakeFilter");
    AssetListFilter::annotate(&table,0,AssetPresence::Zero,"ZERO");
    AssetListFilter::annotate(&table,1,AssetPresence::NeedsCheck,"UNKNOWN");
    filter.setTarget(&table); filter.apply(false);
    QVERIFY(table.isRowHidden(0)); QVERIFY(!table.isRowHidden(1));
    QVERIFY(!child<QLabel>(filter,"fakeFilterStatus")->text().contains("보유 자산이 없습니다"));
    child<QLineEdit>(filter,"fakeFilterSearch")->setText("NO-MATCH");
    QVERIFY(child<QLabel>(filter,"fakeFilterStatus")->text().contains("수량 확인 필요 1개"));
}

void LocalWindowTest::portfolioSearchAndScopeOnlyChangeVisibility() {
    QTemporaryDir dir; Vault vault(dir.filePath("fake-filter-ui.bin")); QVERIFY(vault.create("Fake Filter UI! 2026"));
    AccountRecord a; a.provider="binance";a.alias="FAKE FILTER";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport transport;
    transport.balances={QJsonObject{{"asset","BTC"},{"free","0"},{"locked","0"}},QJsonObject{{"asset","PUMP"},{"free","0"},{"locked","2"}}};
    PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    // This navigation/filter test intentionally displays assets without quotes.
    child<QPushButton>(window,"hideUnvaluedBalances")->setChecked(false);
    QTRY_VERIFY_WITH_TIMEOUT(!portfolio.connections().isEmpty()&&portfolio.connections().first().state=="complete",2000);
    auto* tree=child<QTreeView>(window,"portfolioAssets"); QCOMPARE(tree->model()->rowCount(),2);
    QCOMPARE(portfolioRow(tree, 0).text(0),QString("PUMP")); QVERIFY(!portfolioRow(tree, 0).isHidden());
    QVERIFY(portfolioRow(tree, 1).isHidden()); QVERIFY(portfolioRow(tree, 0).child(0).text(3).contains("미평가"));
    window.resize(1080,760); window.show(); QCoreApplication::processEvents();
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if (!preview.isEmpty()) QVERIFY(window.grab().save(QDir(preview).filePath("portfolio-holdings.png")));
    QVERIFY2(tree->viewport()->height() >= 200, qPrintable(QString("Asset viewport height: %1; window height: %2").arg(tree->viewport()->height()).arg(window.height())));
    const auto snapshots=portfolio.snapshots(); const auto summary=child<QLabel>(window,"portfolioSummary")->text(); const auto sent=transport.sent;
    child<QComboBox>(window,"portfolioAssetFilterScope")->setCurrentIndex(1);
    QVERIFY(!portfolioRow(tree, 1).isHidden()); QCOMPARE(portfolioRow(tree, 0).text(0),QString("PUMP"));
    child<QLineEdit>(window,"portfolioAssetFilterSearch")->setText("pUmP");
    QVERIFY(!portfolioRow(tree, 0).isHidden()); QVERIFY(portfolioRow(tree, 1).isHidden());
    QCOMPARE(portfolio.snapshots(),snapshots); QCOMPARE(child<QLabel>(window,"portfolioSummary")->text(),summary); QCOMPARE(transport.sent,sent);
}


void LocalWindowTest::presentationGroupsWithoutChangingAmounts() {
    const auto component=[](const QString& id,const QString& asset,const QString& quantity,AssetComponentKind kind=AssetComponentKind::Holding){
        AssetComponent c;c.id=id;c.asset=asset;c.quantity=quantity;c.kind=kind;c.evidence.complete=true;return c;};
    AssetSnapshot a;a.accountScope="scope-a";a.connectionId="conn-a";a.provider="binance";a.market="spot";a.timeMs=1000;
    a.components={component("spot:PUMP","PUMP","700"),component("spot:DUST","DUST","0.00000001"),component("power","KRW","5",AssetComponentKind::BuyingPower)};
    AssetSnapshot b=a;b.accountScope="scope-b";b.connectionId="conn-b";b.provider="upbit";b.components={component("spot:PUMP","PUMP","800")};
    const auto row=[](const QString& scope,const QString& id,const QString& asset,const QString& quantity,const QString& value,bool included,const QString& reason={},const QString& reference={}){
        AssetValueRow r;r.accountScope=scope;r.componentId=id;r.asset=asset;r.quantity=quantity;r.value=value;r.included=included;r.reason=reason;r.referenceValue=reference;return r;};
    AssetValuationResult valuation;valuation.complete=false;valuation.knownSubtotal="1500";
    valuation.rows={row("scope-a","spot:PUMP","PUMP","700","700",true),row("scope-b","spot:PUMP","PUMP","800","800",true),
        row("scope-a","spot:DUST","DUST","0.00000001",{},true,"평가가격 없음","0.000001"),row("scope-a","power","KRW","5",{},false,"순자산과 다른 의미의 표시 항목")};
    PortfolioConnection ca;ca.account.id="conn-a";ca.account.alias="가짜 A";ca.state="complete";
    PortfolioConnection cb;cb.account.id="conn-b";cb.account.alias="가짜 B";cb.state="complete";
    PortfolioConnection cc;cc.account.id="conn-c";cc.account.alias="가짜 C";cc.state="failed";cc.stage="조회 실패";
    const auto view=presentPortfolio({a,b},valuation,{ca,cb,cc});
    const auto pump=std::find_if(view.groups.cbegin(),view.groups.cend(),[](const auto& g){return g.identity=="exchange:PUMP";});
    QVERIFY(pump!=view.groups.cend());QCOMPARE(pump->holdings.size(),2);QCOMPARE(pump->storages,QStringList({"가짜 A","가짜 B"}));QCOMPARE(pump->freshTotal,QString("1500"));
    QCOMPARE(pump->holdings[0].quantity,QString("700"));QCOMPARE(pump->holdings[1].quantity,QString("800"));
    const auto dust=std::find_if(view.groups.cbegin(),view.groups.cend(),[](const auto& g){return g.identity=="exchange:DUST";});
    QVERIFY(dust!=view.groups.cend());QCOMPARE(dust->holdings.first().state,PortfolioValueState::Reference);QCOMPARE(dust->holdings.first().quantity,QString("0.00000001"));
    QCOMPARE(dust->holdings.first().presence,AssetPresence::Held);
    const auto power=std::find_if(view.groups.cbegin(),view.groups.cend(),[](const auto& g){return g.identity=="cash:KRW";});
    QVERIFY(power!=view.groups.cend());QCOMPARE(power->holdings.first().state,PortfolioValueState::NotCounted);QCOMPARE(power->unvalued,0);
    QVERIFY(!view.summary.complete);QCOMPARE(view.summary.unvaluedGroups,0);QCOMPARE(view.summary.referenceGroups,1);QCOMPARE(view.summary.referenceTotal,QString("0.000001"));
    QCOMPARE(view.summary.missingAccounts,QStringList({"가짜 C"}));QCOMPARE(view.missing.size(),1);
    QCOMPARE(groupedAmount("1248300.123"),QString("1,248,300.123"));QCOMPARE(groupedAmount("-1000"),QString("-1,000"));QCOMPARE(groupedAmount("999"),QString("999"));
    QCOMPARE(groupedAmount("0.000000001"),QString("0.000000001"));
}

void LocalWindowTest::assetDetailShowsEveryStorageAndClearsOnLock() {
    QTemporaryDir dir;QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    Vault vault(dir.filePath("fake-detail.bin"));QVERIFY(vault.create("Fake Detail UI! 2026"));
    AccountRecord a;a.provider="binance";a.alias="가짜 바이낸스";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AccountRecord b=a;b.provider="upbit";b.alias="가짜 업비트";b.currency="KRW";
    QVERIFY(vault.addAccount(b,{{"access_key","FAKEACCESS123456789"},{"secret","FAKESECRET987654321"}},StorageMode::EncryptedVault));
    AssetReadTransport binance,upbit;binance.balances={QJsonObject{{"asset","PUMP"},{"free","2"},{"locked","1"}}};
    PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};
    f.trading=[&](const AccountRecord& account)->TradingTransport*{return account.provider=="binance"?&binance:&upbit;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    window.resize(1200,800);window.show();
    auto* tree=child<QTreeView>(window,"portfolioAssets");
    const auto group=[tree](const QString& asset) -> PortfolioTestRow {for(int i=0;i<tree->model()->rowCount();++i)if(portfolioRow(tree, i).text(0)==asset)return portfolioRow(tree, i);return {};};
    QTRY_VERIFY_WITH_TIMEOUT(group("PUMP")&&group("PUMP").childCount()==1,3000);
    auto* hide=child<QPushButton>(window,"hideUnvaluedBalances");QVERIFY(hide&&hide->isChecked());
    QVERIFY(group("PUMP").isHidden());hide->setChecked(false);
    QTRY_VERIFY(group("PUMP")&&!group("PUMP").isHidden());
    QCOMPARE(child<QTabWidget>(window,"portfolioPages")->tabText(0),QString("내 자산"));
    QVERIFY(!group("PUMP").isExpanded());
    tree->setCurrentIndex(group("PUMP").modelIndex());
    QCOMPARE(child<QLabel>(window,"assetDetailTitle")->text(),QString("PUMP"));
    QVERIFY(child<QLabel>(window,"assetDetailIdentity")->isHidden());
    QVERIFY(child<QLabel>(window,"assetDetailHeadlineSub")->isHidden());
    auto* ticker=child<QLabel>(window,"assetDetailHolding1");QVERIFY(ticker);QVERIFY(ticker->text().contains("티커 기준"));
    auto* holdingLabel=child<QLabel>(window,"assetDetailHolding0");QVERIFY(holdingLabel);
    const auto holding=holdingLabel->text();
    QVERIFY(holding.contains("가짜 바이낸스"));QVERIFY(holding.contains("수량  3 PUMP"));QVERIFY(holding.contains("주문 묶임"));QVERIFY(holding.contains("미평가"));
    QVERIFY(holding.contains("잔고 기준"));QVERIFY(child<QLabel>(window,"assetDetailTotals")->text().contains("미평가 1건"));
    QVERIFY(child<QLabel>(window,"portfolioSummary")->text().contains("빠진 범위"));
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("g6-assets-detail-1200.png")));
    vault.lock();
    QCOMPARE(tree->model()->rowCount(),0);QVERIFY(child<QLabel>(window,"assetDetailTitle")->text().isEmpty());
    QVERIFY(child<QLabel>(window,"assetDetailIdentity")->text().isEmpty());QVERIFY(!window.findChild<QLabel*>("assetDetailHolding0"));
    QCOMPARE(child<QTableWidget>(window,"connectionStatusTable")->rowCount(),0);
    QCOMPARE(child<QTabWidget>(window,"portfolioPages")->tabText(1),QString("계좌·연결"));
}

void LocalWindowTest::connectionStatusOffersCauseSpecificRecovery_data() {
    QTest::addColumn<bool>("expanded");
    QTest::addColumn<int>("height");
    QTest::newRow("collapsed") << false << 900;
    QTest::newRow("expanded") << true << 730;
    QTest::newRow("short-expanded") << true << 640;
}

void LocalWindowTest::connectionStatusOffersCauseSpecificRecovery() {
    QFETCH(bool, expanded);
    QFETCH(int, height);
    QTemporaryDir dir;Vault vault(dir.filePath("fake-recovery.bin"));QVERIFY(vault.create("Fake Recovery UI! 2026"));
    AccountRecord a;a.provider="binance";a.alias="가짜 실패 계좌";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport transport;transport.fail=true;
    PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    auto* pages=child<QTabWidget>(window,"portfolioPages");auto* table=child<QTableWidget>(window,"connectionStatusTable");
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"connectionReadState")->text().contains("조회 실패"),3000);
    QCOMPARE(pages->tabText(1),QString("계좌·연결"));
    QCOMPARE(table->rowCount(),2);QVERIFY(table->item(0,0)->text().contains("가짜 실패 계좌"));QVERIFY(table->item(0,1)->text().contains("조회 실패"));
    QVERIFY(!table->item(0,1)->text().contains("PRIVATE_RAW_MESSAGE"));QVERIFY(table->item(1,0)->text().contains("원화 가격"));
    QVERIFY(child<QLabel>(window,"portfolioProgress")->text().contains("확인 필요 1"));
    auto* action=qobject_cast<QPushButton*>(table->cellWidget(0,4));QVERIFY(action);
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty()){pages->setCurrentIndex(1);for(const auto& size:{QSize(1200,800),QSize(720,640)}){window.resize(size);window.show();QCoreApplication::processEvents();QTest::qWait(50);
        QVERIFY(window.grab().save(QDir(preview).filePath(QString("g6-connections-%1.png").arg(size.width()))));}pages->setCurrentIndex(0);}
    auto* connectionPage=child<QScrollArea>(window,"connectionsPage");
    auto* more=child<QPushButton>(window,"accountMoreToggle");
    auto* diagnostics=child<QPushButton>(window,"connectionStatusToggle");
    pages->setCurrentWidget(connectionPage);window.resize(1280,height);window.show();
    more->setChecked(expanded);diagnostics->setChecked(expanded);
    QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(window,"reconnectReadAccount")->isVisible(),3000);
    QCoreApplication::processEvents();
    auto* scroll=connectionPage->verticalScrollBar();
    scroll->setValue(qMin(40,scroll->maximum()));
    auto* messages=child<QScrollArea>(window,"accountMore");
    QVERIFY(messages);
    auto* messageScroll=messages->verticalScrollBar();
    if(expanded) messageScroll->setValue(qMin(6,messageScroll->maximum()));
    const int messageOffset=messageScroll->value();
    const auto positions=[&] {
        QList<QPoint> anchors;
        for(const auto* name:{"accountTitle","accountMoreToggle","replaceCredentials","removeAccount","connectionStatusToggle"})
            anchors.append(child<QWidget>(window,name)->mapTo(connectionPage,QPoint()));
        if(expanded) {
            for(const auto* name:{"accountSteps","accountDetails"})
                anchors.append(child<QWidget>(window,name)->mapTo(messages->widget(),QPoint()));
        }
        return anchors;
    };
    const auto anchors=positions();
    const int scrollValue=scroll->value(),scrollMaximum=scroll->maximum();
    const auto recoveryPosition=child<QPushButton>(window,"reconnectReadAccount")->mapTo(connectionPage,QPoint());
    QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(),3000);
    const auto kind=action->property("actionKind").toString();QVERIFY(kind=="reconnect"||kind=="refresh");
    transport.fail=false;const auto sent=transport.sent;QTest::qWait(1100);action->click();
    QTRY_VERIFY_WITH_TIMEOUT(transport.sent>sent,3000);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"connectionReadState")->text()==QString("조회 완료"),3000);
    auto* refresh=child<QPushButton>(window,"refreshSelectedConnection");
    QTRY_VERIFY(refresh->isVisible());
    QCOMPARE(refresh->mapTo(connectionPage,QPoint()),recoveryPosition);
    QTRY_COMPARE(positions(),anchors);
    QCOMPARE(scroll->value(),scrollValue);QCOMPARE(scroll->maximum(),scrollMaximum);
    QCOMPARE(messageScroll->value(),qMin(messageOffset,messageScroll->maximum()));
    pages->setCurrentIndex(0);pages->setCurrentWidget(connectionPage);
    QTRY_COMPARE(positions(),anchors);QCOMPARE(scroll->value(),scrollValue);
    vault.lock();QCOMPARE(table->rowCount(),0);
}

void LocalWindowTest::connectionGroupsOnlyUseExplicitSharedKeys() {
    AccountRecord spot;spot.id="spot";spot.alias="동일 별칭";spot.provider="binance";
    spot.environment="live";spot.market="spot";spot.credentialRef="shared-a";
    auto futures=spot;futures.id="futures";futures.market="usdm_futures";
    auto other=spot;other.id="other";other.credentialRef="different-key";
    auto demo=futures;demo.id="demo";demo.environment="demo";
    auto legacy=spot;legacy.id="legacy";legacy.credentialRef.clear();
    auto legacy2=legacy;legacy2.id="legacy2";
    ConnectionListModel model;
    PortfolioConnection good;good.account=spot;good.state="complete";good.receivedAtMs=QDateTime::currentMSecsSinceEpoch();
    PortfolioConnection failed;failed.account=futures;failed.state="failed";
    const QList<AccountRecord> accounts{spot,futures,other,demo,legacy,legacy2};
    model.refresh(accounts,{good,failed});
    QCOMPARE(model.rowCount(),5);
    QCOMPARE(model.rowForAccount("spot"),model.rowForAccount("futures"));
    QVERIFY(model.rowForAccount("other")!=model.rowForAccount("spot"));
    QVERIFY(model.rowForAccount("demo")!=model.rowForAccount("spot"));
    QVERIFY(model.rowForAccount("legacy")!=model.rowForAccount("legacy2"));
    const auto row=model.index(model.rowForAccount("futures"));
    QVERIFY(row.data(Qt::AccessibleTextRole).toString().contains("현물 · 조회 완료"));
    QVERIFY(row.data(Qt::AccessibleTextRole).toString().contains("선물 · 조회 실패"));
    QPersistentModelIndex stable(row);
    failed.state="complete";failed.receivedAtMs=good.receivedAtMs;
    model.refresh(accounts,{good,failed});
    QVERIFY(stable.isValid());QVERIFY(!stable.data(Qt::AccessibleTextRole).toString().contains("조회 실패"));
    model.refresh({},{});
    QCOMPARE(model.rowCount(),0);QVERIFY(!stable.isValid());
}

void LocalWindowTest::groupedConnectionsKeepMarketActionsAndPrivacy() {
    QTemporaryDir dir;Vault vault(dir.filePath("fake-groups.bin"));QVERIFY(vault.create("Fake Groups! 2026"));
    test::ReadFixture source("binance");source.account.alias="가짜 통합 계좌";
    QVERIFY(vault.addAccount(source.account,source.credentials,StorageMode::EncryptedVault));
    const auto spot=vault.accounts().first();
    auto futures=spot;futures.id.clear();futures.credentialRef.clear();futures.market="usdm_futures";futures.alias="가짜 선물 연결";
    QVERIFY(vault.addAccountUsingCredentials(futures,spot.id));
    for(const auto& a:vault.accounts())if(a.market=="usdm_futures")futures=a;
    auto other=source.account;other.alias="가짜 별도 계좌";
    QVERIFY(vault.addAccount(other,{{"api_key","FAKEOTHERKEY123456"},{"secret","FAKEOTHERSECRET123456"}},StorageMode::EncryptedVault));
    for(const auto& a:vault.accounts())if(a.alias==other.alias)other=a;
    test::ProviderReadTransport spotTransport,futuresTransport,otherTransport;
    spotTransport.provider="binance";futuresTransport.provider="binance-usdm";otherTransport.provider="binance";
    bool fail=true;
    futuresTransport.transform=[&](const RequestSpec& r,HttpResult& result){
        if(fail&&r.operation==Operation::Account)result=test::usdmJson(QJsonObject{{"code",-2015},{"msg","PRIVATE_RAW_MESSAGE"}},401);
    };
    auto* prices=new FreshKrwPriceTransport;
    PortfolioController::Factories f;f.prices=[prices]{return prices;};
    f.trading=[&](const AccountRecord& a)->TradingTransport*{
        return a.id==spot.id?&spotTransport:a.id==futures.id?&futuresTransport:&otherTransport;
    };
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);
    window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    NoNetworkBrokerTransport publicTransport;PublicMarketCatalog catalog(&publicTransport);window.setPublicMarketCatalog(&catalog,false);
    auto* pages=child<QTabWidget>(window,"portfolioPages");pages->setCurrentWidget(child<QWidget>(window,"connectionsPage"));
    window.resize(1200,800);window.show();
    auto* list=child<QListView>(window,"accountList");
    auto* model=static_cast<ConnectionListModel*>(list->model());
    QCOMPARE(model->rowCount(),2);
    list->setCurrentIndex(model->index(model->rowForAccount(spot.id)));
    auto* markets=child<QComboBox>(window,"connectionMarketSelector");
    QTRY_VERIFY_WITH_TIMEOUT(markets->itemText(markets->findData(spot.id)).contains("조회 완료"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(markets->itemText(markets->findData(futures.id)).contains("조회 실패"),5000);
    markets->setCurrentIndex(markets->findData(futures.id));
    QCOMPARE(child<QLabel>(window,"connectionReadState")->text(),QString("조회 실패"));
    QVERIFY(!child<QPushButton>(window,"connectionAssets")->isEnabled());
    QVERIFY(!child<QLabel>(window,"connectionIssue")->text().contains("PRIVATE_RAW_MESSAGE"));
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty()){QTest::qWait(60);QVERIFY(window.grab().save(QDir(preview).filePath("connections-grouped-1200.png")));}
    const auto spotReads=spotTransport.sent.size(),otherReads=otherTransport.sent.size();
    auto* reconnect=child<QPushButton>(window,"reconnectReadAccount");
    QTRY_VERIFY_WITH_TIMEOUT(reconnect->isEnabled(),3000);
    fail=false;QTest::qWait(1100);reconnect->click();
    QTRY_COMPARE_WITH_TIMEOUT(child<QLabel>(window,"connectionReadState")->text(),QString("조회 완료"),5000);
    QCOMPARE(spotTransport.sent.size(),spotReads);QCOMPARE(otherTransport.sent.size(),otherReads);
    QCOMPARE(markets->currentData().toString(),futures.id);
    QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(window,"connectionValuationStatus")->text().contains("미평가 0"),4000);
    QVERIFY(child<QLabel>(window,"connectionValuationStatus")->text().contains("합계 제외 1"));
    {
        // Re-reading a previously successful market must not move controls or oscillate its state.
        markets->setCurrentIndex(markets->findData(spot.id));
        auto* stateLabel=child<QLabel>(window,"connectionReadState");auto* more=child<QPushButton>(window,"accountMoreToggle");
        QCoreApplication::processEvents();const auto anchor=more->mapTo(&window,QPoint());bool observedRead=false,stable=true;
        const auto observation=connect(&portfolio,&PortfolioController::changed,&window,[&]{
            for(const auto& state:portfolio.connections())if(state.account.id==spot.id&&state.active&&state.receivedAtMs>0){
                observedRead=true;stable &= stateLabel->text()==QStringLiteral("조회 완료")&&more->mapTo(&window,QPoint())==anchor;
            }
        });
        portfolio.refreshConnection(spot.id);QTRY_VERIFY_WITH_TIMEOUT(observedRead,5000);
        const auto idle=[&]{for(const auto& state:portfolio.connections())if(state.account.id==spot.id)return !state.active;return false;};
        QTRY_VERIFY_WITH_TIMEOUT(idle(),5000);
        disconnect(observation);QVERIFY(stable);
    }
    // A single-market account must keep its hidden controls out of the layout throughout a re-read.
    const auto singleMarketRow = model->index(model->rowForAccount(other.id));
    list->setCurrentIndex(singleMarketRow);
    auto* card = child<QWidget>(window, "connectionDetail");
    auto* stateLabel = child<QLabel>(window, "connectionReadState");
    auto* more = child<QPushButton>(window, "accountMoreToggle");
    auto* actionScope = child<QLabel>(window, "connectionActionScope");
    const auto idle = [&] {
        for (const auto& state : portfolio.connections())
            if (state.account.id == other.id) return !state.active;
        return false;
    };
    QTRY_COMPARE_WITH_TIMEOUT(stateLabel->text(), QStringLiteral("조회 완료"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 5000);
    QCoreApplication::processEvents();
    QVERIFY(card->isVisible());
    QVERIFY(!markets->isVisible());
    QVERIFY(!actionScope->isVisible());
    const auto stateAnchor = stateLabel->mapTo(card, QPoint());
    const auto moreAnchor = more->mapTo(card, QPoint());
    const auto rowRect = list->visualRect(singleMarketRow);
    bool observedRead = false;
    bool statusStable = true;
    class VisibilityChanges final : public QObject {
      public:
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Show || event->type() == QEvent::Hide) ++count;
            return false;
        }
    } visibility;
    markets->installEventFilter(&visibility);
    actionScope->installEventFilter(&visibility);
    const auto observation = connect(&portfolio, &PortfolioController::changed, &visibility, [&] {
        for (const auto& state : portfolio.connections()) {
            if (state.account.id != other.id || !state.active || state.receivedAtMs <= 0) continue;
            observedRead = true;
            statusStable &= stateLabel->text() == QStringLiteral("조회 완료");
        }
    });
    QSignalSpy projection(model, &QAbstractItemModel::dataChanged);
    portfolio.refreshConnection(other.id);
    QTRY_VERIFY_WITH_TIMEOUT(observedRead, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(idle() && !projection.isEmpty(), 5000);
    disconnect(observation);
    QCoreApplication::processEvents();
    markets->removeEventFilter(&visibility);
    actionScope->removeEventFilter(&visibility);
    QCOMPARE(visibility.count, 0);
    QVERIFY(statusStable);
    QCOMPARE(stateLabel->text(), QStringLiteral("조회 완료"));
    QCOMPARE(stateLabel->mapTo(card, QPoint()), stateAnchor);
    QCOMPARE(more->mapTo(card, QPoint()), moreAnchor);
    QCOMPARE(list->currentIndex(), singleMarketRow);
    QCOMPARE(list->visualRect(singleMarketRow), rowRect);
    list->setCurrentIndex(model->index(model->rowForAccount(futures.id)));
    markets->setCurrentIndex(markets->findData(futures.id));
    // Detail navigation must open holdings, even when exploration occupies tab zero.
    child<QPushButton>(window,"connectionAssets")->click();
    QCOMPARE(pages->currentWidget()->objectName(),QString("portfolioHome"));
    QCOMPARE(child<QComboBox>(window,"portfolioAccountFilter")->currentData().toString(),futures.id);
    pages->setCurrentWidget(child<QWidget>(window,"connectionsPage"));
    window.resize(720,640);QCoreApplication::processEvents();QTest::qWait(40);
    QVERIFY(list->isVisible());QVERIFY(!child<QWidget>(window,"connectionDetail")->isVisible());
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("connections-list-720.png")));
    QTest::mouseClick(list->viewport(),Qt::LeftButton,{},list->visualRect(list->currentIndex()).center());
    QVERIFY(!list->isVisible());QVERIFY(child<QWidget>(window,"connectionDetail")->isVisible());
    QVERIFY(child<QPushButton>(window,"connectionDetailBack")->isVisible());
    QVERIFY(child<QWidget>(window,"connectionDetail")->width()>600);
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("connections-detail-720.png")));
    child<QPushButton>(window,"connectionDetailBack")->click();QVERIFY(list->isVisible());
    // A destructive action names and removes only the selected market.
    markets->setCurrentIndex(markets->findData(futures.id));
    child<QPushButton>(window,"removeAccount")->click();
    auto* confirm=child<QMessageBox>(window,"removeAccountConfirmation");QVERIFY(confirm);
    QVERIFY(confirm->text().contains(futures.alias));QVERIFY(confirm->text().contains("선물"));
    child<QPushButton>(*confirm,"confirmRemoveAccount")->click();processDeletion();
    QCOMPARE(vault.accounts().size(),2);QCOMPARE(vault.credentialUsers(spot.id).size(),1);
    QCOMPARE(model->rowForAccount(futures.id),-1);QVERIFY(model->rowForAccount(spot.id)>=0);
    vault.lock();QCOMPARE(model->rowCount(),0);QCOMPARE(markets->count(),0);
    for(const auto* name:{"accountTitle","connectionIssue","connectionReadTime","connectionActionScope","accountDetails"})
        QVERIFY(child<QLabel>(window,name)->text().isEmpty());
    QVERIFY(!child<QLabel>(window,"connectionValuationStatus"));
}

void LocalWindowTest::narrowWindowSwitchesBetweenListAndDetail_data() {
    QTest::addColumn<bool>("refreshNote");
    QTest::newRow("settled")<<false;
    QTest::newRow("refresh-status-visible")<<true;
}
void LocalWindowTest::narrowWindowSwitchesBetweenListAndDetail() {
    QFETCH(bool,refreshNote);
    QTemporaryDir dir;Vault vault(dir.filePath("fake-narrow.bin"));QVERIFY(vault.create("Fake Narrow UI! 2026"));
    AccountRecord a;a.provider="binance";a.alias="가짜 좁은 창 계좌 이름이 매우 긴 경우";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    AssetReadTransport transport;transport.balances={QJsonObject{{"asset","PUMP"},{"free","1"},{"locked","0"}},QJsonObject{{"asset","BTC"},{"free","0.5"},{"locked","0"}}};
    PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &transport;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);
    // This navigation/filter test intentionally displays assets without quotes.
    child<QPushButton>(window,"hideUnvaluedBalances")->setChecked(false);
    QCOMPARE(window.minimumSize(),QSize(720,640));
    window.resize(720,640);window.show();QCoreApplication::processEvents();
    auto* tree=child<QTreeView>(window,"portfolioAssets");auto* detail=child<AssetDetailPanel>(window,"assetDetail");
    QTRY_VERIFY_WITH_TIMEOUT(tree->model()->rowCount()==2,3000);
    // Rows arrive before the queued initial-read settle reveals the asset pane.
    QTRY_VERIFY_WITH_TIMEOUT(child<ListDetailView>(window,"assetListDetail")->isVisible(),3000);
    // The same small window must retain a readable detail body when a refresh
    // status row consumes additional vertical space above it.
    if(refreshNote){auto* note=child<QLabel>(window,"portfolioRefreshNote");note->setText("새 자료를 확인하고 있어요. 마지막 확인 값을 표시합니다.");note->show();QCoreApplication::processEvents();}
    QCOMPARE(window.width(),720);
    QTRY_VERIFY2_WITH_TIMEOUT(child<ListDetailView>(window,"assetListDetail")->narrow(),qPrintable(QString("window width %1, minimum hint %2, list/detail width %3, minimum hint %4").arg(window.width()).arg(window.minimumSizeHint().width()).arg(child<ListDetailView>(window,"assetListDetail")->width()).arg(child<ListDetailView>(window,"assetListDetail")->minimumSizeHint().width())),1000);QVERIFY(tree->isVisible());QVERIFY(!detail->isVisible());
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("g6-assets-list-720.png")));
    auto second=portfolioRow(tree, 1);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,{},tree->visualRect(second.modelIndex()).center());QCOMPARE(tree->currentIndex().siblingAtColumn(0),second.modelIndex());
    QVERIFY(detail->isVisible());QVERIFY(!tree->isVisible());QVERIFY(child<QPushButton>(window,"assetDetailBack")->isVisible());
    QTRY_VERIFY2(detail->width()>500,qPrintable(QString("detail width %1").arg(detail->width())));
    auto* scroll=child<QScrollArea>(window,"assetDetailScroll");QVERIFY(scroll);
    QTRY_VERIFY2(scroll->viewport()->height()>=240,qPrintable(QString("detail viewport %1").arg(scroll->viewport()->height())));
    QVERIFY(child<QLabel>(window,"assetDetailTitle")->isVisible());QVERIFY(!child<QLabel>(window,"assetDetailTitle")->text().isEmpty());
    if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath("g6-assets-detail-720.png")));
    child<QPushButton>(window,"assetDetailBack")->click();
    QVERIFY(tree->isVisible());QVERIFY(!detail->isVisible());QCOMPARE(tree->currentIndex().siblingAtColumn(0),second.modelIndex());
    window.resize(1200,800);QCoreApplication::processEvents();
    QVERIFY(!child<ListDetailView>(window,"assetListDetail")->narrow());QVERIFY(tree->isVisible());QVERIFY(detail->isVisible());
    QVERIFY(!child<QPushButton>(window,"assetDetailBack")->isVisible());
    vault.lock();
}


void LocalWindowTest::changePercentRoundsOnlyForDisplay() {
    // Market prices drop trailing fractional zeros only; the value stays exact.
    QCOMPARE(priceAmount("22.10000000"), QString("22.1"));
    QCOMPARE(priceAmount("0.00048000"), QString("0.00048"));
    QCOMPARE(priceAmount("100.00000000"), QString("100"));
    QCOMPARE(priceAmount("3833.00000000"), QString("3,833"));
    QCOMPARE(priceAmount("95000000"), QString("95,000,000"));
    QCOMPARE(changePercentText("0.015"),QString("+1.50%"));QCOMPARE(changePercentText("-0.0123"),QString("-1.23%"));
    QCOMPARE(changePercentText("0"),QString("0.00%"));QCOMPARE(changePercentText("-0.00004"),QString("0.00%"));
    QCOMPARE(changePercentText("0.00005"),QString("+0.01%"));QCOMPARE(changePercentText("1.2E-4"),QString("+0.01%"));
    QCOMPARE(changePercentText("-0.0104166666666667"),QString("-1.04%"));QCOMPARE(changePercentText("0.12995"),QString("+13.00%"));
    QCOMPARE(changePercentText("not-a-number"),QString());
}

void LocalWindowTest::publicExplorationWorksLockedAndLinksHoldingsByTicker() {
    QTemporaryDir dir;Vault vault(dir.filePath("fake-explore.bin"));
    AssetReadTransport assets;assets.balances={QJsonObject{{"asset","PUMP"},{"free","4"},{"locked","0"}}};
    PortfolioController::Factories f;f.prices=[]{return static_cast<TradingTransport*>(nullptr);};f.trading=[&](const AccountRecord&){return &assets;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);
    PublicMarketTransport transport;PublicMarketCatalog markets(&transport);
    window.setPublicMarketCatalog(&markets,true);window.setSecurityStatus(true);window.resize(1200,800);window.show();
    auto* pages=child<QTabWidget>(window,"portfolioPages");QCOMPARE(pages->tabText(0),QString("탐색"));QCOMPARE(pages->currentIndex(),0);
    QCOMPARE(pages->tabText(2),QString("계좌·연결"));
    auto* view=child<QComboBox>(window,"exploreView");
    view->setCurrentIndex(view->findData("list"));
    auto* search=child<QLineEdit>(window,"exploreSearch");
    search->setText("KRW-");
    auto* table=child<QTableWidget>(window,"exploreMarkets");
    QTRY_VERIFY_WITH_TIMEOUT(table->rowCount()==2&&table->item(0,2)&&table->item(0,2)->text()=="2.75",3000);
    QVERIFY(!vault.exists());QVERIFY(!vault.isUnlocked());
    QCOMPARE(table->item(0,1)->text(),QString("KRW-PUMP"));QCOMPARE(table->item(0,3)->text(),QString("+10.00%"));
    QCOMPARE(table->item(1,2)->text(),QString("95,000,000"));QCOMPARE(table->item(1,3)->text(),QString("-1.04%"));QVERIFY(table->item(1,0)->text().contains("유의"));
    QVERIFY(table->item(0,4)->text().isEmpty());
    QVERIFY(child<QLabel>(window,"exploreStatus")->text().contains("KST 09시"));
    QCOMPARE(transport.requests.first().operation,Operation::Markets);QCOMPARE(QUrlQuery(transport.requests.first().url).queryItemValue("is_details"),QString("true"));
    auto* detail=child<AssetDetailPanel>(window,"exploreDetail");
    table->setCurrentCell(0,0);
    QCOMPARE(detail->findChild<QLabel*>("assetDetailTitle")->text(),QString("가짜펌프"));
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding0")->text().contains("전일 종가 2.5 KRW"));
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding0")->text().contains("UTC 0시"));
    QCOMPARE(detail->findChild<QLabel*>("assetDetailTotals")->text(),QString("2.75 KRW · 전일 대비 +10.00%"));
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding1")->text().contains("잠금 해제 후"));
    child<QLineEdit>(window,"exploreSearch")->setText("비트");QVERIFY(table->isRowHidden(0));QVERIFY(!table->isRowHidden(1));
    child<QLineEdit>(window,"exploreSearch")->setText("pump");QVERIFY(!table->isRowHidden(0));QVERIFY(table->isRowHidden(1));
    search->clear();
    search->setText("KRW-");
    // Unlock: the same ticker links the personal holding; the public list is not refetched.
    const auto publicRequests=transport.requests.size();
    QVERIFY(vault.create("Fake Explore UI! 2026"));
    AccountRecord a;a.provider="binance";a.alias="가짜 탐색 계좌";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKEAPIKEY123456789"},{"secret","FAKESECRET123456789"}},StorageMode::EncryptedVault));
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0,4)->text(),QString("보유"),3000);QVERIFY(table->item(1,4)->text().isEmpty());
    table->setCurrentCell(0,0);
    QTRY_VERIFY_WITH_TIMEOUT(detail->findChild<QLabel*>("assetDetailHolding1")->text().contains("같은 티커(PUMP)"),3000);
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding2")->text().contains("가짜 탐색 계좌"));
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if(!preview.isEmpty()){QCoreApplication::processEvents();QVERIFY(window.grab().save(QDir(preview).filePath("g6-explore-1200.png")));
        window.resize(720,640);QCoreApplication::processEvents();QVERIFY(window.grab().save(QDir(preview).filePath("g6-explore-list-720.png")));window.resize(1200,800);}
    vault.lock();
    QCOMPARE(table->rowCount(),2);QCOMPARE(table->item(0,2)->text(),QString("2.75"));QVERIFY(table->item(0,4)->text().isEmpty());
    QVERIFY(detail->findChild<QLabel*>("assetDetailHolding1")->text().contains("잠금 해제 후"));QVERIFY(!detail->findChild<QLabel*>("assetDetailHolding2"));
    QCOMPARE(transport.requests.size(),publicRequests);
}

void LocalWindowTest::publicExplorationHonorsRateLimitWithoutResending() {
    PublicMarketTransport transport;transport.quoteStatus=429;PublicMarketCatalog markets(&transport);
    markets.refresh();QTRY_VERIFY_WITH_TIMEOUT(!markets.busy(),3000);
    QVERIFY(markets.status().contains("요청 제한"));QCOMPARE(markets.rows().size(),2);QVERIFY(!markets.rows().first().priced);
    const auto sent=transport.requests.size();markets.refresh();QCOMPARE(transport.requests.size(),sent);QVERIFY(markets.status().contains("대기"));
}

void LocalWindowTest::portfolioAllocationScopesAndProviderPnlStayHonest() {
    const auto now=QDateTime::currentMSecsSinceEpoch();
    auto snapshot=[&](const QString& id,const QString& market){AssetSnapshot s;s.accountScope=id;s.connectionId=id;s.provider="fake";s.market=market;s.environment="live";s.timeMs=now;s.balanceComplete=true;s.evidence={"fake official","https://example.invalid",id,now,now,true};return s;};
    auto part=[&](const AssetSnapshot& s,const QString& id,const QString& asset,const QString& quantity,AssetComponentKind kind){AssetComponent p;p.id=id;p.asset=asset;p.quantity=quantity;p.kind=kind;p.evidence=s.evidence;return p;};
    auto a=snapshot("a","spot"),b=snapshot("b","spot"),stock=snapshot("stock","domestic_equity");
    a.components={part(a,"usdt","USDT","10",AssetComponentKind::Holding),part(a,"krw","KRW","100",AssetComponentKind::Cash),part(a,"unknown","UNKNOWN","1",AssetComponentKind::Holding)};
    b.components={part(b,"usdt","USDT","5",AssetComponentKind::Holding),part(b,"position","FAKEUSDT","99",AssetComponentKind::PositionNotional)};
    auto equity=part(stock,"equity","005930","2",AssetComponentKind::Holding);equity.reportedValue="2000";equity.reportedValueCurrency="KRW";equity.providerUnrealizedPnl="100.49";
    stock.components={equity,part(stock,"cash","KRW","5000",AssetComponentKind::Cash),part(stock,"debt","KRW","1000",AssetComponentKind::Liability)};
    const QList<AssetSnapshot> snapshots{a,b,stock};
    const QList<AssetQuote> prices{{"USDT","KRW","1000",{"fake public","https://example.invalid","public:upbit:KRW-USDT",now,now,true}}};
    const auto valuation=AssetValuation::value(snapshots,prices,"KRW",now,DisplayFreshnessMs);
    const auto view=presentPortfolio(snapshots,valuation,{});
    QCOMPARE(view.summary.knownSubtotal,QString("21100"));QVERIFY(!view.summary.complete);
    QCOMPARE(portfolioAllocationTotal(view.groups),QString("22100"));
    const auto usdt=scopedPortfolioGroups(view.groups,"coin",{}).first();
    QCOMPARE(usdt.asset,QString("USDT"));QCOMPARE(portfolioQuantity(usdt),QString("15"));QCOMPARE(usdt.freshTotal,QString("15000"));
    QCOMPARE(portfolioShareText(portfolioAllocationValue(usdt),portfolioAllocationTotal(view.groups)),QString("67.9%"));
    const auto onlyB=scopedPortfolioGroups(view.groups,"coin","b");QCOMPARE(onlyB.first().freshTotal,QString("5000"));QCOMPARE(portfolioQuantity(onlyB.first()),QString("5"));
    const auto cash=scopedPortfolioGroups(view.groups,"cash",{});QCOMPARE(cash.size(),1);QCOMPARE(cash.first().identity,QString("cash:KRW"));QCOMPARE(cash.first().freshTotal,QString("4100"));QCOMPARE(portfolioQuantity(cash.first()),QString("4100"));
    const auto stocks=scopedPortfolioGroups(view.groups,"stock",{});QCOMPARE(stocks.size(),1);
    QCOMPARE(providerPnlText(stocks.first().holdings.first()),QString("+100 KRW"));
    QVERIFY(usdt.holdings.first().providerPnl.isEmpty());
    AssetDetailPanel detail;detail.showGroup(stocks.first(),portfolioAllocationTotal(view.groups));
    QVERIFY(child<QLabel>(detail,"assetDetailPlaceValue1")->text().contains("업체 평가손익 +100 KRW"));
    auto stale=stock;stale.timeMs=now-DisplayFreshnessMs-1;
    auto oldView=presentPortfolio({stale},AssetValuation::value({stale},{},"KRW",now,DisplayFreshnessMs),{});
    for(const auto& g:oldView.groups)for(const auto& h:g.holdings)QVERIFY(h.providerPnl.isEmpty());
    auto future=stock;future.components[0].evidence.receivedAtMs=now+1;
    auto futureView=presentPortfolio({future},AssetValuation::value({future},{},"KRW",now,DisplayFreshnessMs),{});
    for(const auto& g:futureView.groups)for(const auto& h:g.holdings)QVERIFY(h.providerPnl.isEmpty());
    auto mismatched=stock;mismatched.components[0].evidence.scope="different-account";
    auto mismatchedView=presentPortfolio({mismatched},AssetValuation::value({mismatched},{},"KRW",now,DisplayFreshnessMs),{});
    for(const auto& g:mismatchedView.groups)for(const auto& h:g.holdings)QVERIFY(h.providerPnl.isEmpty());
    QCOMPARE(portfolioShareText("1","3"),QString("33.3%"));QCOMPARE(portfolioShareText("2","3"),QString("66.7%"));
    QCOMPARE(portfolioShareText("0.00000001","100"),QString("<0.1%"));QCOMPARE(portfolioShareText("1","0"),QString("—"));
    QCOMPARE(portfolioShareText("-1","2"),QString("—"));QCOMPARE(portfolioShareText("3","2"),QString("—"));
    QCOMPARE(portfolioShareText("0","2000"),QString("0.0%"));
    QCOMPARE(portfolioShareText("1","2000"),QString("0.1%"));
    QCOMPARE(portfolioShareText("1","2001"),QString("<0.1%"));
    QCOMPARE(portfolioShareText("999999999999998","999999999999999"),QString("100.0%"));
    QCOMPARE(portfolioShareText("0.000499999999999999999","1"),QString("<0.1%"));
    QCOMPARE(portfolioShareText("5000000000000000","10000000000000000000"),QString("0.1%"));
    PortfolioGroupView large=usdt,larger=usdt;large.identity="large";larger.identity="larger";
    large.holdings=large.holdings.mid(0,1);larger.holdings=larger.holdings.mid(0,1);
    large.holdings[0].value="9007199254740992";larger.holdings[0].value="9007199254740993";
    QCOMPARE(scopedPortfolioGroups({large,larger},{},{}).first().identity,QString("larger"));
    auto otherScope=usdt.holdings.first();otherScope.accountScope="another";
    QVERIFY(portfolioHoldingKey(usdt.holdings.first())!=portfolioHoldingKey(otherScope));
}

void LocalWindowTest::portfolioFiltersSortRefreshAndClearOnLock() {
    QTemporaryDir dir;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.filePath("settings"));
    Vault vault(dir.filePath("fake-composition.bin"));QVERIFY(vault.create("Fake Composition UI! 2026"));
    AccountRecord a;a.provider="binance";a.alias="테스트 현물 A";a.environment="live";a.market="spot";a.currency="USDT";
    QVERIFY(vault.addAccount(a,{{"api_key","FAKECOMPOSITIONKEYA"},{"secret","FAKECOMPOSITIONSECRETA"}},StorageMode::EncryptedVault));
    a.alias="테스트 현물 B";QVERIFY(vault.addAccount(a,{{"api_key","FAKECOMPOSITIONKEYB"},{"secret","FAKECOMPOSITIONSECRETB"}},StorageMode::EncryptedVault));
    keefi::test::ReadFixture broker("kiwoom");broker.account.alias="테스트 증권";QVERIFY(vault.addAccount(broker.account,broker.credentials,StorageMode::EncryptedVault));
    AssetReadTransport first,second;
    first.balances={QJsonObject{{"asset","USDT"},{"free","10"},{"locked","0"}},QJsonObject{{"asset","BIG"},{"free","1"},{"locked","0"}},QJsonObject{{"asset","UNKNOWN"},{"free","0.25"},{"locked","0"}}};
    second.balances={QJsonObject{{"asset","USDT"},{"free","5"},{"locked","0"}},QJsonObject{{"asset","DUST"},{"free","1"},{"locked","0"}}};
    keefi::test::ProviderReadTransport brokerTransport;brokerTransport.provider="kiwoom";
    auto* prices=new FreshKrwPriceTransport;
    PortfolioController::Factories f;f.prices=[prices]{return prices;};
    f.trading=[&](const AccountRecord& account)->TradingTransport*{return account.provider=="kiwoom"?static_cast<TradingTransport*>(&brokerTransport):account.alias.endsWith('A')?&first:&second;};
    PortfolioController portfolio(vault,nullptr,f);AccountModel accounts(&vault);LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.resize(1440,900);window.show();
    auto* tree=child<QTreeView>(window,"portfolioAssets");auto* type=child<QComboBox>(window,"portfolioClassFilter");auto* account=child<QComboBox>(window,"portfolioAccountFilter");
    QVERIFY(qobject_cast<SegmentedComboBox*>(type)->isSegment(0));
    const auto group=[tree](const QString& asset) -> PortfolioTestRow {for(int i=0;i<tree->model()->rowCount();++i)if(portfolioRow(tree, i).text(0)==asset)return portfolioRow(tree, i);return {};};
    const auto diagnostic=[&]{QStringList lines;for(const auto& c:portfolio.connections())lines<<c.account.alias+":"+c.state+":"+c.stage;for(int i=0;i<tree->model()->rowCount();++i)lines<<portfolioRow(tree, i).text(0)+":"+QString::number(portfolioRow(tree, i).childCount());return lines.join(" | ");};
    QTRY_VERIFY2_WITH_TIMEOUT(group("USDT")&&group("USDT").childCount()==2&&group("005930")&&!portfolio.pricingBusy(),qPrintable(diagnostic()),12000);
    QTRY_VERIFY_WITH_TIMEOUT(group("USDT").text(3)=="20,775",3000);QCOMPARE(group("USDT").text(2),QString("15"));
    QCOMPARE(account->count(),4);const auto fullTotal=child<QLabel>(window,"portfolioTotal")->text();const auto allocation=child<QWidget>(window,"portfolioAllocation")->accessibleDescription();
    const auto snapshot=portfolio.snapshots();const auto requests=first.sent+second.sent+brokerTransport.sent.size();
    type->setCurrentIndex(type->findData("coin"));QVERIFY(!group("005930"));QCOMPARE(portfolioRow(tree, 0).text(0),QString("BIG"));
    tree->setCurrentIndex(group("USDT").modelIndex());QVERIFY(child<QLabel>(window,"assetDetailPlaceValue0")->text().contains("15 USDT"));
    PortfolioTestRow secondRow;for(int i=0;i<group("USDT").childCount();++i)if(group("USDT").child(i).text(1).contains("현물 B"))secondRow=group("USDT").child(i);QVERIFY(secondRow);
    const auto secondId=secondRow.data(0,Qt::UserRole).toString();
    tree->setCurrentIndex(secondRow.modelIndex());QVERIFY(child<QLabel>(window,"assetDetailHolding0")->text().contains("5 USDT"));
    QVERIFY(!child<QLabel>(window,"assetDetailHolding0")->text().contains("10 USDT"));
    account->setCurrentIndex(account->findData(secondId));QCOMPARE(group("USDT").text(2),QString("5"));QCOMPARE(group("USDT").text(3),QString("6,925"));QVERIFY(!group("BIG"));
    QCOMPARE(child<QLabel>(window,"portfolioTotal")->text(),fullTotal);QCOMPARE(child<QWidget>(window,"portfolioAllocation")->accessibleDescription(),allocation);
    type->setCurrentIndex(type->findData("stock"));QCOMPARE(tree->model()->rowCount(),0);
    account->setCurrentIndex(0);QCOMPARE(tree->model()->rowCount(),1);QCOMPARE(portfolioRow(tree, 0).text(0),QString("005930"));
    type->setCurrentIndex(type->findData("cash"));QVERIFY(group("KRW"));QVERIFY(!group("005930"));
    QCOMPARE(portfolio.snapshots(),snapshot);QCOMPARE(first.sent+second.sent+brokerTransport.sent.size(),requests);
    type->setCurrentIndex(0);tree->setCurrentIndex(group("USDT").modelIndex());
    const auto preview=qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    for(const auto& size:{QSize(1440,900),QSize(1200,800),QSize(720,640)}){
        window.resize(size);QCoreApplication::processEvents();QTest::qWait(60);
        auto* scope=child<SegmentedComboBox>(window,"portfolioAssetFilterScope");
        auto* search=child<QLineEdit>(window,"portfolioAssetFilterSearch");
        QVERIFY(scope->width()>=scope->sizeHint().width());
        QVERIFY(scope->height()>=scope->sizeHint().height());
        QVERIFY(scope->mapTo(&window,QPoint(scope->width(),0)).x()<search->mapTo(&window,QPoint()).x());
        const QRect scopeBounds(scope->mapTo(&window,QPoint()),scope->size());
        for(int index:{1,0}){
            QTest::mouseClick(scope,Qt::LeftButton,Qt::NoModifier,scope->segmentRects()[index].center());
            QCoreApplication::processEvents();
            QCOMPARE(QRect(scope->mapTo(&window,QPoint()),scope->size()),scopeBounds);
        }
        if(!preview.isEmpty())QVERIFY(window.grab().save(QDir(preview).filePath(QString("composition-%1.png").arg(size.width()))));
        if(size.width()==720){
            QVERIFY2(tree->viewport()->height()>=200,qPrintable(QString("holdings viewport %1").arg(tree->viewport()->height())));
            QList<QPushButton*> headerButtons;
            for(const auto* name:{"themeLight","themeDark","trustPill","createVault","unlockVault","lockVault","changeVaultPassword","openOptions"}){
                auto* control=child<QPushButton>(window,name);
                if(control->isVisibleTo(&window))headerButtons.append(control);
            }
            for(auto* tab:child<QWidget>(window,"headerTabs")->findChildren<QPushButton*>())
                if(tab->isVisibleTo(&window))headerButtons.append(tab);
            for(qsizetype first=0;first<headerButtons.size();++first){
                auto* a=headerButtons[first];const QRect aBounds(a->mapTo(&window,QPoint()),a->size());
                for(qsizetype second=first+1;second<headerButtons.size();++second){
                    auto* b=headerButtons[second];const QRect bBounds(b->mapTo(&window,QPoint()),b->size());
                    const auto overlap=aBounds.intersected(bBounds);
                    QVERIFY2(overlap.isEmpty(),qPrintable(QString("Header controls %1 and %2 overlap by %3×%4px at 720×640")
                        .arg(a->objectName(),b->objectName()).arg(overlap.width()).arg(overlap.height())));
                }
            }
        }
    }
    type->setCurrentIndex(type->findData("coin"));tree->setCurrentIndex(group("USDT").modelIndex());
    prices->prices["USDT"]="10000";QTest::qWait(1100);portfolio.refreshPrices();
    QTRY_COMPARE_WITH_TIMEOUT(portfolioRow(tree, 0).text(0),QString("USDT"),3000);
    QCOMPARE(selectedPortfolioRow(tree).text(0),QString("USDT"));QCOMPARE(group("USDT").text(3),QString("150,000"));
    vault.lock();QCOMPARE(tree->model()->rowCount(),0);QCOMPARE(account->count(),1);
    QVERIFY(child<QWidget>(window,"portfolioAllocation")->accessibleDescription().isEmpty());
    QVERIFY(child<QLabel>(window,"assetDetailTitle")->text().isEmpty());
}

QTEST_MAIN(LocalWindowTest)
#include "tst_local_window.moc"
