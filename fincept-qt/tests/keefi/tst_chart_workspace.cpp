#include "TooltipTestSupport.h"
#include "ChartInstrumentPanelTestSupport.h"
#include <QAccessible>
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartGridView.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartInstrumentPicker.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/InfoButton.h"
#include <QPlainTextEdit>
#include <QPointer>
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/ChartDrawings.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/MarketMetrics.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/TradingTransport.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/LocalWindow.h"
#include "keefi/AccountModel.h"
#include "keefi/BackgroundTray.h"
#include "keefi/Vault.h"
#include "fixtures/UpbitResponses.h"
#include "fixtures/BinanceUsdmResponses.h"
#include "fixtures/TossResponses.h"
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QComboBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QFile>
#include <QSignalSpy>
#include <QPersistentModelIndex>
#include <QApplication>
#include <QImage>
#include <QScopeGuard>
#include <QFileInfo>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QTimeZone>
#include <QDir>
#include <QUrlQuery>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <QSettings>
#include <QTextDocument>
#include <QStyleOptionComboBox>
#include <QMenu>

using namespace keefi;
namespace {
HoldingChartRow syntheticHoldingRow(const WatchlistIdentity& identity){
    HoldingChartRow row;row.assetKey="exchange:"+identity.code.section('-',1);row.symbol=identity.code.section('-',1);row.displayName=row.symbol;
    row.chart=identity;row.candidates={identity};row.custodies={{"upbit","업비트",HoldingCustodyState::Fresh,QDateTime::currentMSecsSinceEpoch()}};row.alertsEligible=true;return row;
}
void selectSavedScopeAndExpandDetails(ChartWorkspace& workspace) {
    workspace.findChild<QPushButton*>("chartBrowserScope_watch")->click();
    workspace.findChild<QPushButton*>("chartDetailsToggle")->setChecked(true);
}
QString displayedText(const QLabel* label){
    if(label->textFormat()==Qt::RichText||(label->textFormat()==Qt::AutoText&&Qt::mightBeRichText(label->text()))){
        QTextDocument document;document.setHtml(label->text());return document.toPlainText();
    }
    return label->text();
}
}

class ChartWorkspaceTest final : public QObject {
    Q_OBJECT
  private slots:
    void revisitedCandlesDoNotPopulateTheCurrentPriceHeadline();
    void terminalBrowserDiscoversMarketsWithoutAddingFavorites();
    void coinBrowserListsMostTradedFirstAndPrefillsBinanceRows();
    void chartRankingAndExpandedMetricsPreserveSelection();
    void realPublicBrowserCaptureForReview_data();
    void realPublicBrowserCaptureForReview();
    void browserFiltersPreserveSavedStateAndQuoteRows();
    void browserMarketRestoresSearchSelectionAndScroll();
    void browserHoldingsOnlyRowsKeepDrawingsTransient();
    void browserNarrowLayoutKeepsSearchBeforeTheChart();
    void expandedBrowserKeepsEveryControlInsideItsPanel_data();
    void expandedBrowserKeepsEveryControlInsideItsPanel();
    void browserTabsRenderDistinctSelectionAndKeyboardFocus();
    void browserCatalogFailureRetryAndRecovery_data();
    void browserCatalogFailureRetryAndRecovery();
    void browserCatalogEmptySuccessIsNotFailure();
    void emptyUpbitListAndFailedBinanceFallbackSettle();
    void failedCataloguesRetryIndependently_data();
    void failedCataloguesRetryIndependently();
    void stockBrowserRequestsWhileBinanceRankingIsPending();
    void catalogRetryHonoursHttpDate();
    void browserCatalogRefreshKeepsOldRowsWithoutReverification();
    void browserCatalogRetryRespectsCooldown();
    void browserCatalogCooldownResumesAfterShow();
    void browserMetadataFailureKeepsAnotherChart();
    void programmaticSelectionSynchronizesVisibleRows();
    void discoveryAndReorderControlsAreVisible();
    void unsupportedSavedCoinRoutesAndRepairsSelectionBeforeCandleRequest();
    void representativePickerSavesPublicGroupsAndVisibleReorderRoundTrips();
    void currentStatusFoldsScorecardAndExplainsAllOff();
    void currentStatusPreservesAccessibleRowsDuringPublicUpdates();
    void scorecardSelectorDoesNotBorrowOtherStudyResults();
    void privateClearRemovesStockSupplementalButKeepsPublicCoinStudies();
    void optionalWatchTrendTableReleasesItsThirtyDemands();
    void splitChartsKeepIndependentSelectionsAndReleaseTheirFeed();
    void emptySplitSlotKeepsItsChosenTimeframe();
    void splitActivePanelKeepsFullIndicators();
    void splitUsesOneLiveSocketAndReleasesPassiveRequests();
    void splitPanelSelectionPreservesLiveFollowing_data();
    void splitPanelSelectionPreservesLiveFollowing();
    void splitPanelsRememberIndependentLiveZoom_data();
    void splitPanelsRememberIndependentLiveZoom();
    void splitSyncConnectsRealActiveAndPassiveInputsWithoutLoops();
    void splitSelectionKeepsVisibleMarketAndTimeframeConsistentAfterCatalogRemoval_data();
    void splitSelectionKeepsVisibleMarketAndTimeframeConsistentAfterCatalogRemoval();
    void initTestCase(){QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings_.path());}
  private:
    QTemporaryDir settings_;
  private slots:
    void timeframeTextButtonsDriveTheHiddenSelectorAndPriceUnitStaysOnItsLine();
    void missingQuotesUseDistinctShortStates();
    void cachedBinanceQuoteKeepsPriceAndExplainsDelay();
    void listFlagsUseLineIconsAndKeepTheirNames();
    void listNameTooltipKeepsMarkupLiteral();
    void publicListSwitcherWorksWithoutAnUnlockedVault();
    void savedStockRemainsSelectableForConnectionGuidance_data();
    void savedStockRemainsSelectableForConnectionGuidance();
    void moreMenuKeepsPublicPreviewWhileAssetCardMovesToHoldings();
    void stockSessionChoiceUsesVerifiedIdentityAndDefaultsToRegularOnly();
    void nativeStockPipelineRendersClosedBodiesFlowAndCurrentViWithoutInventedHistory();
    void publicCoinFlowPanelIsIndependentFromPrivateClearingAndStockStudies();
    void defaultIndicatorsKeepVisibilitySeparateFromEnabledAcrossSymbols();
    void indicatorPickerSearchesKeefiAndHidesIncompatibleMarketStudies();
    void indicatorIntroductionAppearsOnlyOnce();
    void completedStudyScorecardShowsHorizonSamplesAndExcludedCosts();
    void sixTimeframesKeepTheFullSelectedIdentity();
    void widePositionsRemainReadableBesideMixedPublicLists();
    void priceColumnRemainsVisibleAfterNarrowToWideResize();
    void expandedDetailsDoNotEnlargeTheNarrowWindow();
    void chartComesFirstInNarrowWindow();
    void providerAveragesStaySeparateAndLockCannotRehydrateThem();
    void positionQuantitySurvivesMissingAverageWithoutReadsOrDataChanges_data();
    void positionQuantitySurvivesMissingAverageWithoutReadsOrDataChanges();
    void brokerPositionShowsOnlyMatchingReportedValues();
    void futuresDistanceUsesReportedMarkAndNeverTheKrwAxis();
    void futuresRibbonUsesExistingReadsAndClearsOnLock();
    void futuresPricesUseTheMatchingVerifiedUsdtAxis();
    void hiddenPassiveFuturesObservationsUseOnlyExistingPrivateReads();
    void hiddenPublicRowsWaitForShowCatchup();
    void listMenusEditMixedPublicIdentitiesAndRoundTripCsv();
    void realtimeVisibilityFollowsTheActualViewport();
    void emptyPublicListDoesNotResetOnUnrelatedQuotes();
    void destroyedListModelKeepsEmptyTreeStable();
    void quoteRefreshPreservesScrollAndCollapsedSections();
    void holdingsPreviewPreservesRowsAndScrollOnQuoteRefresh();
    void reusedQuoteRowsClearPrivateAndRecoverVerificationPresentation();
    void rowReuseTracksSectionStructureAndModelRebinding();
    void ownedListModelTearsDownWithTheWorkspace();
    void publicCandleModelOutlivesWorkspaceTeardown();
    void holdingsCoverageIsPrivateAndUserListsSurviveLock();
    void usabilityOmitsWatchlistProvenanceAndEmptyPositionPanels();
};
void ChartWorkspaceTest::listFlagsUseLineIconsAndKeepTheirNames(){
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    auto* panel = test::openInstrumentPanel(workspace); QVERIFY(panel);

    bindThemePresentation(&workspace,[&]{workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));});
    auto* lists=workspace.watchlistModel();auto* selector=workspace.findChild<QComboBox*>("chartWorkspaceListSelector");QVERIFY(lists&&selector);
    auto* menu=workspace.findChild<QToolButton*>("chartWorkspaceListMenu");QVERIFY(menu&&menu->menu());
    const auto controlRenders=qEnvironmentVariable("KEEFI_WATCHLIST_CONTROLS_RENDER_DIR");
    for(auto mode:{ThemeMode::Light,ThemeMode::Dark})for(int width:{1280,1440}){
        KeefiTheme::instance()->setMode(mode);workspace.resize(width,900);QTest::qWait(25);
        QVERIFY(selector->currentData().toString().isEmpty());QVERIFY(!selector->currentText().isEmpty());
        QStyleOptionComboBox option;option.initFrom(selector);option.editable=false;
        const auto field=selector->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,selector);
        if(!controlRenders.isEmpty()){
            QVERIFY(QDir().mkpath(controlRenders));QVERIFY(panel->grab().save(controlRenders+"/watchlist-empty-"+(mode==ThemeMode::Light?"light":"dark")+"-"+QString::number(width)+".png"));
        }
        const auto labelWidth=selector->fontMetrics().horizontalAdvance(selector->currentText());
        const auto geometry=QString("window=%1 text=%2 textWidth=%3 combo=%4x%5 field=%6,%7 %8x%9 font=%10/%11px")
            .arg(width).arg(selector->currentText()).arg(labelWidth).arg(selector->width()).arg(selector->height())
            .arg(field.x()).arg(field.y()).arg(field.width()).arg(field.height()).arg(selector->font().family()).arg(selector->font().pixelSize());
        QVERIFY2(labelWidth<=field.width(),qPrintable(geometry));
        QCOMPARE(selector->height(),28);QCOMPARE(menu->size(),QSize(28,28));
        QVERIFY(panel->rect().contains(menu->mapTo(panel,menu->rect().bottomRight())));
        bool menuOpened=false;QTimer::singleShot(0,menu->menu(),[&]{menuOpened=menu->menu()->isVisible();menu->menu()->close();});
        QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,QPoint(2,2));QVERIFY(menuOpened);
        auto* create=workspace.findChild<QAction*>("chartListCreate");QVERIFY(create&&create->isEnabled());
    }
    QSet<QRgb> lightInks,darkInks;
    for(const auto& flag:{QString("red"),QString("orange"),QString("yellow"),QString("green"),QString("blue"),QString("purple")}){
        const auto name="합성 공개 리스트 "+flag;const auto id=lists->createList(name,flag);QVERIFY(!id.isEmpty());
        for(auto mode:{ThemeMode::Light,ThemeMode::Dark}){
            KeefiTheme::instance()->setMode(mode);selector->setCurrentIndex(selector->findData(id));QCoreApplication::processEvents();
            const auto index=selector->findData(id);QVERIFY(index>=0);
            QCOMPARE(selector->itemText(index),name);QVERIFY2(!selector->itemIcon(index).isNull(),"A selected list flag needs a line icon instead of an emoji text prefix");
            const auto icon=selector->itemIcon(index).pixmap(16,16).toImage();int painted=0;QColor ink;
            for(int y=0;y<icon.height();++y)for(int x=0;x<icon.width();++x){const auto pixel=icon.pixelColor(x,y);
                if(pixel.alpha()>0)++painted;if(!ink.isValid()||pixel.alpha()>ink.alpha())ink=pixel;}
            QVERIFY(painted>8&&painted<160);QCOMPARE(lists->userList(id).flag,flag);
            QVERIFY(ink.alpha()>240);QVERIFY(KeefiTheme::contrast(ink,themeColors().bg)>=4.5);QVERIFY(KeefiTheme::contrast(ink,themeColors().surface)>=4.5);
            (mode==ThemeMode::Light?lightInks:darkInks).insert(ink.rgb());
            const auto out=qEnvironmentVariable("KEEFI_FINAL_F07_RENDER_DIR");if(!out.isEmpty()){
                QVERIFY(QDir().mkpath(out));QVERIFY(panel->grab().save(out+"/flag-"+flag+"-"+(mode==ThemeMode::Light?"light":"dark")+".png"));
            }
        }
    }
    QCOMPARE(lightInks.size(),6);QCOMPARE(darkInks.size(),6);
    const QString longName=QStringLiteral("합성 관심 리스트 이름을 길게 써도 원래 이름은 보존해요");
    const auto longId=lists->createList(longName,"blue");QVERIFY(!longId.isEmpty());
    selector->setCurrentIndex(selector->findData(longId));QCoreApplication::processEvents();
    QCOMPARE(selector->currentText(),longName);QCOMPARE(lists->userList(longId).name,longName);
    QVERIFY(selector->toolTip().contains(longName));
}
void ChartWorkspaceTest::moreMenuKeepsPublicPreviewWhileAssetCardMovesToHoldings(){
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setIdentity("coin:upbit:KRW-SYN");workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(!workspace.findChild<QPushButton*>("chartShareCard"));
    auto* share=workspace.findChild<QAction*>("chartPublicShareAction");QVERIFY(share);share->trigger();
    auto* dialog=workspace.findChild<QDialog*>("chartShareDialog");QVERIFY(dialog);QVERIFY(dialog->isVisible());auto* privacy=dialog->findChild<QLabel*>("chartSharePrivacy");QVERIFY(privacy);QVERIFY(privacy->text().contains("계좌 금액·수량 제외"));auto* save=dialog->findChild<QPushButton*>("chartShareSave");QVERIFY(save);QVERIFY(!save->isEnabled());
    share->trigger();QCOMPARE(workspace.findChildren<QDialog*>("chartShareDialog").size(),1);workspace.clearPrivate();QVERIFY(dialog->isVisible());auto* close=dialog->findChild<QPushButton*>("chartShareClose");QVERIFY(close);QTest::mouseClick(close,Qt::LeftButton);QVERIFY(!dialog->isVisible());
}

void ChartWorkspaceTest::publicCoinFlowPanelIsIndependentFromPrivateClearingAndStockStudies(){
    CoinFlowSource source(nullptr);ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setCoinFlowSource(&source);workspace.setIdentity("coin:upbit:KRW-SYN");workspace.resize(1280,950);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(workspace.indicatorPreferences()->enabled("whales"));
    auto* details=workspace.findChild<QLabel*>("chartCoinFlowDetails");QVERIFY2(details,"The default coin indicator must expose source coverage and missing-part reasons while warming up");
    QVERIFY(!details->isVisible());QVERIFY(displayedText(details).contains(QStringLiteral("표본")));QVERIFY(displayedText(details).contains(QStringLiteral("원천")));
    const auto before=displayedText(details);auto* information=workspace.findChild<InfoButton*>("marketCandlesInformation");QVERIFY(information);information->click();QVERIFY(information->informationOpen());
    auto* body=information->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(body);QVERIFY(!body->toPlainText().contains("https://"));
    workspace.clearPrivate();QCOMPARE(displayedText(details),before);QVERIFY(details->isHidden());QVERIFY(information->informationOpen());
    information->findChild<QDialog*>("infoDialog")->close();
    auto* lists=workspace.watchlistModel();lists->setVerifiedInstruments({{"stock","US","SYN"}});workspace.setIdentity("stock:US:SYN");
    QVERIFY(details->isHidden());QVERIFY(workspace.coinFlowStudies().identity.isEmpty());QVERIFY(workspace.findChild<QWidget*>("chartStudyChip_whales")->isHidden());
}

void ChartWorkspaceTest::nativeStockPipelineRendersClosedBodiesFlowAndCurrentViWithoutInventedHistory() {
    // Every response, date and quantity below is synthetic. This fixture uses the
    // production transport/parser/timeline path without a vault or account controller.
    QTemporaryDir profileDirectory;QVERIFY(profileDirectory.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profileDirectory.filePath("settings"));
    class UnavailableTray final:public BackgroundTray{public:bool available()const override{return false;}bool visible()const override{return false;}
        void setMenu(QMenu*)override{}void show()override{}void hide()override{}} tray;
    Vault themeVault(profileDirectory.filePath("synthetic-ui-theme-vault"));AccountModel themeAccounts(&themeVault);
    LocalWindow themeWindow(themeVault,themeAccounts,nullptr,&tray);
    qint64 now=QDateTime::fromString("2026-10-23T09:35:10+09:00",Qt::ISODate).toMSecsSinceEpoch();
    class StockHttp final:public TradingTransport {
      public:
        qint64& clock;QList<RequestSpec> requests;quint64 lastToken=0;int aborts=0;
        explicit StockHttp(qint64& value):clock(value){}
        static QDate businessDay(QDate day,int direction){do{day=day.addDays(direction);}while(day.dayOfWeek()>5);return day;}
        static QJsonObject sessionDay(QDate day){const auto date=day.toString(Qt::ISODate);
            const auto session=[](const QString& from,const QString& to){return QJsonObject{{"startTime",from},{"endTime",to}};};
            return {{"date",date},{"integrated",QJsonObject{{"preMarket",QJsonValue::Null},
                {"regularMarket",session(date+"T09:00:00+09:00",date+"T15:30:00+09:00")},
                {"afterMarket",session(date+"T15:30:00+09:00",date+"T20:00:00+09:00")}}}};}
        static QJsonObject side(const QString& buy,const QString& sell,const QString& net){return {{"buyVolume",buy},{"sellVolume",sell},{"netBuyVolume",net}};}
        static QJsonObject candle(const QString& timestamp,int price,const QString& volume){
            return {{"timestamp",timestamp},{"openPrice",QString::number(price)},{"highPrice",QString::number(price+3)},
                {"lowPrice",QString::number(price-2)},{"closePrice",QString::number(price+1)},{"volume",volume},{"currency","KRW"}};}
        void abort()override{++aborts;lastToken=0;}
        void send(quint64 token,const RequestSpec& request)override{
            QVERIFY(StockChartSource::isRequest(request));QVERIFY(!isMutation(request.operation));
            QCOMPARE(request.headers.value("Authorization"),QByteArray("Bearer SYNTHETIC_STOCK_UI_ONLY"));
            requests.append(request);lastToken=token;const auto path=request.url.path();const QUrlQuery query(request.url);QJsonValue payload;
            if(path=="/api/v1/stocks")payload=QJsonArray{QJsonObject{{"symbol","0101N0"},{"name","합성 종목"},{"englishName","Synthetic Stock"},
                {"isinCode","KR7000000000"},{"market","KOSPI"},{"securityType","STOCK"},{"isCommonShare",true},{"status","ACTIVE"},{"currency","KRW"},
                {"sharesOutstanding","1000"},{"listDate",QJsonValue::Null},{"delistDate",QJsonValue::Null},{"leverageFactor",QJsonValue::Null},
                {"koreanMarketDetail",QJsonObject{{"liquidationTrading",false},{"nxtSupported",true},{"krxTradingSuspended",false},{"nxtTradingSuspended",false}}}}};
            else if(path.startsWith("/api/v1/market-calendar/")){
                const auto date=query.queryItemValue("date");const auto day=date.isEmpty()?QDate(2026,10,23):QDate::fromString(date,Qt::ISODate);
                payload=QJsonObject{{"today",sessionDay(day)},{"previousBusinessDay",sessionDay(businessDay(day,-1))},{"nextBusinessDay",sessionDay(businessDay(day,1))}};
            }else if(path=="/api/v1/price-limits")payload=QJsonObject{{"timestamp","2026-10-23T09:35:00+09:00"},{"upperLimitPrice","150"},{"lowerLimitPrice","50"},{"currency","KRW"}};
            else if(path.endsWith("/warnings"))payload=QJsonArray{QJsonObject{{"warningType","VI_STATIC"}}};
            else if(path.endsWith("/investor-trading"))payload=QJsonObject{{"nextUntil",QJsonValue::Null},{"records",QJsonArray{
                QJsonObject{{"date","2026-10-23"},{"updatedAt","2026-10-23T09:34:00+09:00"},{"individual",QJsonValue::Null},
                    {"foreigner",side("15","10","5")},{"institution",QJsonObject{{"buyVolume","23"},{"sellVolume","20"},{"netBuyVolume","3"},{"breakdown",QJsonValue::Null}}},
                    {"otherCorporation",QJsonValue::Null},{"foreignerHolding",QJsonValue::Null},{"cfd",QJsonValue::Null}},
                QJsonObject{{"date","2026-10-22"},{"updatedAt","2026-10-22T18:00:00+09:00"},{"individual",QJsonValue::Null},
                    {"foreigner",side("10","10","0")},{"institution",QJsonObject{{"buyVolume","20"},{"sellVolume","20"},{"netBuyVolume","0"},{"breakdown",QJsonValue::Null}}},
                    {"otherCorporation",QJsonValue::Null},{"foreignerHolding",QJsonValue::Null},{"cfd",QJsonValue::Null}}}}};
            else if(path.endsWith("/program-trades"))payload=QJsonObject{{"nextUntil",QJsonValue::Null},{"records",QJsonArray{
                QJsonObject{{"date","2026-10-23"},{"arbitrage",side("12","10","2")},{"nonArbitrage",side("4","5","-1")}}}}};
            else if(path=="/api/v1/candles"){
                QJsonArray rows;
                if(query.queryItemValue("interval")=="1d"){
                    auto day=QDate(2026,10,22);for(int i=0;i<21;++i){rows.append(candle(day.toString(Qt::ISODate)+"T00:00:00+09:00",100,i==0?"40":"20"));day=businessDay(day,-1);}
                }else{
                    const auto open=QDateTime::fromString("2026-10-23T09:00:00+09:00",Qt::ISODate);
                    for(int minute=36;minute>=1;--minute)rows.append(candle(open.addSecs(minute*60).toString(Qt::ISODate),100+minute%7,"20"));
                }
                payload=QJsonObject{{"candles",rows},{"nextBefore",QJsonValue::Null}};
            }else QFAIL("Unexpected synthetic stock UI request");
            HttpResult response;response.status=200;response.body=QJsonDocument(QJsonObject{{"result",payload}}).toJson(QJsonDocument::Compact);
            QTimer::singleShot(1,this,[this,token,response]{if(lastToken!=token)return;clock+=101;emit finished(token,response);});
        }
    } http(now);
    StockChartSource source(&http,nullptr,[&]{return now;});
    source.setAuthorizationProvider([](const auto& borrow){borrow("SYNTHETIC_STOCK_UI_ONLY",3600000,1);return true;});
    ChartWorkspace workspace(nullptr,nullptr);workspace.setStyleSheet(themeWindow.styleSheet());workspace.indicatorPreferences()->markIntroSeen();
    const WatchlistIdentity identity{"stock","KR","0101N0"};auto* lists=workspace.watchlistModel();lists->setVerifiedInstruments({identity});
    const auto id=lists->createList(QStringLiteral("합성 혼합 관찰"),"blue");QVERIFY(!id.isEmpty());QVERIFY(lists->addIdentity(id,identity));
    QVERIFY(lists->addIdentity(id,{"coin","upbit","KRW-SYN"}));lists->useList(id);
    workspace.setStockSource(&source);workspace.setIdentity(identity.key());workspace.setTimeframe(60000);workspace.resize(1280,1000);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QTRY_VERIFY_WITH_TIMEOUT(source.snapshot().series.available&&!source.snapshot().loading,12000);
    const auto snapshot=source.snapshot();QCOMPARE(snapshot.source,StockSourceKind::Toss);QCOMPARE(snapshot.series.candles.size(),35);
    QCOMPARE(snapshot.series.identity,identity.key());QCOMPARE(snapshot.series.intervalMs,qint64(60000));
    QCOMPARE(snapshot.series.candles.first().startsAtMs,QDateTime::fromString("2026-10-23T09:00:00+09:00",Qt::ISODate).toMSecsSinceEpoch());
    QCOMPARE(snapshot.timeline.rows.last().completesAtMs,QDateTime::fromString("2026-10-23T09:35:00+09:00",Qt::ISODate).toMSecsSinceEpoch());
    QVERIFY(snapshot.series.forming.has_value());QCOMPARE(snapshot.series.forming->startsAtMs,snapshot.timeline.rows.last().completesAtMs);
    QVERIFY(snapshot.restrictions.currentVI.value_or(false));QCOMPARE(snapshot.restrictions.historicalCoverage,StockHistoryCoverage::CurrentOnly);QVERIFY(snapshot.restrictions.intervals.isEmpty());
    QVERIFY(snapshot.marketStudies.volumeMultiple.has_value());QCOMPARE(*snapshot.marketStudies.volumeMultiple,2.0);QCOMPARE(snapshot.marketStudies.volumeSamples,20);
    QVERIFY(snapshot.marketStudies.volumeEvidence.receivedAtMs>snapshot.series.receivedAtMs);QCOMPARE(snapshot.series.sourceTimeMs,qint64(0));
    auto* context=workspace.findChild<QLabel*>("marketCandlesStockContext");QVERIFY(context);QVERIFY(context->text().contains(QStringLiteral("정규장")));
    QVERIFY(context->text().contains(QStringLiteral("수정주가 적용")));QVERIFY(context->text().contains(QStringLiteral("원천 갱신 시각 미제공")));
    auto* flow=workspace.findChild<QLabel*>("chartStudyStatus_stock_flow");QVERIFY(flow);
    QVERIFY(workspace.indicatorPreferences()->enabled("stock_flow"));
    const auto currentStates=workspace.findChild<MarketCandlesView*>()->indicatorStates();bool dailyFlow=false;
    for(const auto& state:currentStates)if(state.id=="stock_flow"){dailyFlow=true;QVERIFY(state.status.contains(QStringLiteral("일별")));}QVERIFY(dailyFlow);
    QVERIFY(!flow->text().contains(QStringLiteral("순매수")));QVERIFY(!flow->text().contains(QStringLiteral("거래일")));
    // P2 shows one core value; the source's investor quantities and continuity remain typed facts.
    QCOMPARE(snapshot.marketStudies.foreignerConsecutiveBuyDays.value_or(-1),1);
    QCOMPARE(snapshot.marketStudies.institutionConsecutiveBuyDays.value_or(-1),1);
    QVERIFY(!snapshot.marketStudies.days.isEmpty());
    const auto latestFlow=*std::max_element(snapshot.marketStudies.days.cbegin(),snapshot.marketStudies.days.cend(),[](const auto& a,const auto& b){return a.date<b.date;});
    QCOMPARE(latestFlow.foreignerNetBuyVolume.value_or(QString{}),QString("5"));
    QCOMPARE(latestFlow.institutionNetBuyVolume.value_or(QString{}),QString("3"));
    auto* daily=workspace.findChild<QWidget*>("chartFuturesLedger");QVERIFY(daily);QVERIFY2(daily->isVisible(),"Connected stocks replace the futures row with actual daily investor/program observations");
    const auto dailyLabels=daily->findChildren<QLabel*>();QString dailyText;for(auto* label:dailyLabels)dailyText+=label->text();QVERIFY(dailyText.contains(QStringLiteral("외국인")));QVERIFY(dailyText.contains(QStringLiteral("기관")));QVERIFY(dailyText.contains(QStringLiteral("주")));
    QVERIFY(workspace.indicatorPreferences()->setVisible("stock_flow",false));
    QVERIFY(!daily->isVisible());QVERIFY(workspace.indicatorPreferences()->enabled("stock_flow"));
    QVERIFY(workspace.indicatorPreferences()->setVisible("stock_flow",true));QVERIFY(daily->isVisible());
    auto* chip=flow->parentWidget();QVERIFY(!snapshot.marketStudies.days.isEmpty());QVERIFY(snapshot.marketStudies.days.first().investorEvidence.source.contains("KRX+NXT"));QVERIFY(!snapshot.marketStudies.days.isEmpty());QVERIFY(chip->toolTip().startsWith(QStringLiteral("Stock Flow & Volume")));
    QVERIFY(chip->toolTip().contains(QStringLiteral("일별")));QVERIFY(!chip->toolTip().contains("https://"));QVERIFY(snapshot.marketStudies.volumeEvidence.url.contains("interval=1d"));
    auto* whales=workspace.findChild<QWidget*>("chartStudyChip_whales");QVERIFY(whales);QVERIFY(whales->isHidden());
    auto* vi=workspace.findChild<QLabel*>("marketCandlesStockObservations");QVERIFY(vi);QVERIFY(vi->text().contains(QStringLiteral("VI 발동 중")));
    QCOMPARE(snapshot.restrictions.historicalCoverage,StockHistoryCoverage::CurrentOnly);QVERIFY(vi->isVisible());
    const auto viImage=vi->grab().toImage();int riskPixels=0;QMap<QRgb,int> leftColors;
    for(int y=0;y<viImage.height();++y)for(int x=0;x<qMin(5,viImage.width());++x){const auto color=viImage.pixelColor(x,y);++leftColors[color.rgba()];if(color==themeColors().danger)++riskPixels;}
    qDebug()<<"synthetic VI label"<<vi->size()<<vi->isVisible()<<vi->visibleRegion().boundingRect()<<"pixels"<<viImage.size()<<"DPR"<<viImage.devicePixelRatio()<<"style"<<vi->styleSheet();
    QList<QPair<int,QRgb>> counts;for(auto it=leftColors.cbegin();it!=leftColors.cend();++it)counts.append({it.value(),it.key()});
    std::sort(counts.begin(),counts.end(),[](const auto& a,const auto& b){return a.first>b.first;});
    for(int i=0;i<qMin(12,counts.size());++i)qDebug()<<"synthetic VI left color"<<QColor::fromRgba(counts[i].second).name(QColor::HexArgb)<<counts[i].first;
    if(const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!preview.isEmpty()){
        QVERIFY(QDir().mkpath(preview));QVERIFY(viImage.save(QDir(preview).filePath("c5-stock-vi-failure.png")));
        QCoreApplication::processEvents();QVERIFY(vi->grab().save(QDir(preview).filePath("c5-stock-vi-after-events.png")));
        QVERIFY(workspace.grab().save(QDir(preview).filePath("c5-stock-wide.png")));
        workspace.resize(820,1100);QCoreApplication::processEvents();QVERIFY(workspace.grab().save(QDir(preview).filePath("c5-stock-narrow.png")));
        workspace.resize(1280,1000);QCoreApplication::processEvents();
    }
    QVERIFY2(riskPixels>0,"A current VI observation must render the serious risk border, without claiming historical exclusion intervals");
    auto* chart=workspace.findChild<QWidget*>("marketCandlesChart");QVERIFY(chart);const auto image=chart->grab().toImage();int candlePixels=0;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==MovementColorPolicy::fillFor(chart,MovementDirection::Up))++candlePixels;
    QVERIFY2(candlePixels>0,"Verified native completed stock OHLCV must draw real candle bodies");
    auto* horizontal=workspace.findChild<QToolButton*>("chartDrawingTool_horizontal");QVERIFY(horizontal);QVERIFY(horizontal->isEnabled());
    QVERIFY(workspace.drawingModel());QPoint actualBody(-1,-1);
    for(int y=0;y<image.height()&&actualBody.x()<0;++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==MovementColorPolicy::fillFor(chart,MovementDirection::Up)){actualBody={x,y};break;}
    QVERIFY(actualBody.x()>=0);QTest::mouseClick(horizontal,Qt::LeftButton);QTest::mouseClick(chart,Qt::LeftButton,{},actualBody);
    QCOMPARE(workspace.drawingModel()->drawings().size(),1);

    if(const auto preview=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");!preview.isEmpty()){
        QVERIFY(QDir().mkpath(preview));QCoreApplication::processEvents();QVERIFY(workspace.grab().save(QDir(preview).filePath("c5-stock-wide.png")));
        workspace.resize(820,1100);QCoreApplication::processEvents();QVERIFY(workspace.grab().save(QDir(preview).filePath("c5-stock-narrow.png")));
    }
    workspace.hide();QVERIFY(!source.active());
}

void ChartWorkspaceTest::stockSessionChoiceUsesVerifiedIdentityAndDefaultsToRegularOnly() {
    ChartWorkspace workspace(nullptr, nullptr);
    auto* lists = workspace.watchlistModel(); QVERIFY(lists);
    const WatchlistIdentity stock{"stock", "US", "SYN"};
    lists->setVerifiedInstruments({stock});
    workspace.setStockSource(nullptr);
    workspace.setIdentity(stock.key()); workspace.resize(1280, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QCOMPARE(workspace.selectedChartIdentity(), stock.key());
    auto* extended = workspace.findChild<QCheckBox*>("chartWorkspaceExtendedSessions");
    QVERIFY2(extended, "Stock sessions need an explicit regular-only default and an extended-session choice");
    QVERIFY(extended->isVisible()); QVERIFY(!extended->isChecked());
    QVERIFY(!workspace.includeExtendedSessions());
    QSignalSpy policy(&workspace, &ChartWorkspace::sessionPolicyChanged);
    extended->click();
    QVERIFY(workspace.includeExtendedSessions()); QCOMPARE(policy.size(), 1);
    QCOMPARE(policy.first().first().toBool(), true);
    workspace.setAsset("SYN"); QVERIFY(extended->isHidden());
    workspace.setIdentity(stock.key()); QVERIFY(extended->isVisible());
    QCOMPARE(workspace.selectedChartIdentity(), stock.key());
}

void ChartWorkspaceTest::defaultIndicatorsKeepVisibilitySeparateFromEnabledAcrossSymbols() {
    ChartWorkspace workspace(nullptr, nullptr); workspace.resize(1280, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    workspace.findChild<QPushButton*>("chartStudyAppliedToggle")->click();
    auto* preferences = workspace.indicatorPreferences();
    QVERIFY2(preferences, "Chart indicator choices use public profile preferences independent of account state");
    QVERIFY(preferences->supportedIds("coin").size() > 5);
    for (const auto& id : preferences->supportedIds("coin").first(5)) {
        QVERIFY(preferences->enabled(id)); QVERIFY(preferences->visible(id));
        QVERIFY(workspace.findChild<QWidget*>("chartStudyChip_" + id));
    }
    auto* eye = workspace.findChild<QToolButton*>("chartStudyEye_cloud");
    auto* off = workspace.findChild<QToolButton*>("chartStudyOff_similarity");
    QVERIFY(eye); QVERIFY(off); eye->click();
    QVERIFY(preferences->enabled("cloud")); QVERIFY(!preferences->visible("cloud"));
    QVERIFY(workspace.findChild<QWidget*>("chartStudyChip_cloud")->isVisible());
    off->click(); QVERIFY(!preferences->enabled("similarity"));
    auto* removed = workspace.findChild<QWidget*>("chartStudyChip_similarity"); QVERIFY(!removed || removed->isHidden());
    workspace.setAsset("ETH");
    QVERIFY(!preferences->visible("cloud")); QVERIFY(!preferences->enabled("similarity"));
    ChartWorkspace restored(nullptr, nullptr);
    QVERIFY(restored.indicatorPreferences());
    QVERIFY(restored.indicatorPreferences()->restorePublicState(preferences->publicState()));
    restored.setAsset("SYN");
    QVERIFY(!restored.indicatorPreferences()->visible("cloud")); QVERIFY(!restored.indicatorPreferences()->enabled("similarity"));
}

void ChartWorkspaceTest::indicatorPickerSearchesKeefiAndHidesIncompatibleMarketStudies() {
    ChartWorkspace workspace(nullptr, nullptr); workspace.resize(1280, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    auto* preferences = workspace.indicatorPreferences(); QVERIFY(preferences);
    auto* view = workspace.findChild<MarketCandlesView*>(); QVERIFY(view);
    view->setSelection("stock:US:NVDA", 86400000);
    auto* toolbar = workspace.findChild<QWidget*>("chartToolbar"); QVERIFY(toolbar);
    auto* picker = toolbar->findChild<QPushButton*>("chartStudyPicker"); QVERIFY(picker);
    bool inspected = false;
    QTimer::singleShot(0, qApp, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        QTimer::singleShot(1500, dialog, &QDialog::reject);
        auto* search = dialog->findChild<QLineEdit*>("chartStudySearch");
        auto* tree = dialog->findChild<QTreeWidget*>("chartStudyPickerTree");
        if (!search || !tree) { dialog->reject(); return; }
        QTreeWidgetItem *cloud = nullptr, *pullback = nullptr; bool flow = false, whales = false, keefi = false;
        for (QTreeWidgetItemIterator item(tree); *item; ++item) {
            const auto id = (*item)->data(0, Qt::UserRole).toString();
            if ((*item)->text(0) == "KEEFI") keefi = true;
            if (id == "cloud") cloud = *item;
            if (id == "pullback") pullback = *item;
            if (id == "stock_flow") flow = true;
            if (id == "whales") whales = true;
        }
        QVERIFY(keefi); QVERIFY(flow); QVERIFY(!whales); QVERIFY(cloud); QVERIFY(pullback);
        search->setText(QStringLiteral("추세"));
        QVERIFY(!cloud->isHidden()); QVERIFY(pullback->isHidden());
        inspected = true; dialog->accept();
    });
    picker->click(); QVERIFY(inspected);
}

void ChartWorkspaceTest::indicatorIntroductionAppearsOnlyOnce() {
    ChartStudyPreferences preferences;
    MarketCandlesView first(nullptr); first.setIndicatorPreferences(&preferences); first.setAsset("BTC"); first.resize(900, 760); first.show();
    auto* intro = first.findChild<QWidget*>("chartStudyIntro");
    auto* dismiss = first.findChild<QPushButton*>("chartStudyIntroDismiss");
    QVERIFY(intro); QVERIFY(dismiss); QVERIFY(intro->isVisible()); QVERIFY(preferences.introSeen());
    dismiss->click(); QVERIFY(intro->isHidden());
    first.hide(); first.show(); QVERIFY(intro->isHidden());
    MarketCandlesView reopened(nullptr); reopened.setIndicatorPreferences(&preferences); reopened.setAsset("ETH"); reopened.resize(900, 760); reopened.show();
    auto* repeated = reopened.findChild<QWidget*>("chartStudyIntro"); QVERIFY(repeated); QVERIFY(repeated->isHidden());
}
void ChartWorkspaceTest::completedStudyScorecardShowsHorizonSamplesAndExcludedCosts() {
    class PublicTransport final : public TradingTransport {
      public:
        quint64 token = 0;
        void send(quint64 t, const RequestSpec&) override { token = t; }
        void abort() override {}
    } transport;
    constexpr qint64 observed = 1728000000000 + 250 * 60000 + 10000;
    MarketCandles model(&transport, std::unique_ptr<PublicCandleSocket>{}, [] { return observed; });
    ChartWorkspace workspace(&model, nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setTimeframe(60000); workspace.setAsset("SYN"); workspace.resize(1400, 1000); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(transport.token); QJsonArray rows; const auto end = observed / 60000 * 60000;
    for (int i = 0; i < 81; ++i) {
        const auto at = end - (81 - i) * 60000; const auto price = i == 80 ? QString("110") : QString("100");
        rows.append(QJsonObject{{"market", "KRW-SYN"}, {"unit", "1"}, {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp", QString::number(at + 1000)}, {"opening_price", price}, {"trade_price", price},
            {"high_price", i == 11 ? QString("120") : decimal::add(price, "1")}, {"low_price", i == 10 ? QString("90") : decimal::subtract(price, "1")}, {"candle_acc_trade_volume", "0"}});
    }
    HttpResult response; response.status = 200; response.body = QJsonDocument(rows).toJson(); emit transport.finished(transport.token, response);
    QTRY_VERIFY(workspace.chartStudies().scorecard.available);
    const auto studies = workspace.chartStudies(); QVERIFY(studies.scorecard.available); QCOMPARE(studies.scorecard.samples, 1);
    QCOMPARE(studies.scorecard.horizonMs, qint64(3600000));
    auto* scorecard = workspace.findChild<QLabel*>("chartStudyScorecard"); QVERIFY(scorecard);
    QVERIFY(!scorecard->text().contains("%"));QVERIFY(!scorecard->text().contains(QStringLiteral("올랐어요")));
    workspace.findChild<QPushButton*>("chartIndicatorScoreToggle")->click();
    QVERIFY(workspace.findChild<QLabel*>("chartStudyScorecardEmpty")->text().contains("1/20"));
    QVERIFY(scorecard->text().contains(QStringLiteral("결과 1")));
    QCOMPARE(workspace.chartStudies().scorecard.up,1);QVERIFY(workspace.chartStudies().scorecard.meanReturnPercent>0);
    QCOMPARE(workspace.chartStudies().scorecard.horizonMs,qint64(3600000));QVERIFY(workspace.chartStudies().scorecard.costsExcluded);
    QCOMPARE(workspace.chartStudies().scorecard.outcomes.size(),1);

}

void ChartWorkspaceTest::usabilityOmitsWatchlistProvenanceAndEmptyPositionPanels(){
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    auto* lists=workspace.watchlistModel();const auto id=lists->createList("합성 사용성");const WatchlistIdentity identity{"coin","upbit","KRW-BTC"};
    QVERIFY(lists->addIdentity(id,identity));lists->useList(id);
    WatchlistRowState state;state.price="107780000";state.evidence="https://api.upbit.com/v1/ticker coin:upbit:KRW-BTC [7]";lists->setRowState(identity,state);
    workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
    QCoreApplication::processEvents();
    auto* rows=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(rows);QCOMPARE(rows->topLevelItemCount(),1);
    QVERIFY(!rows->topLevelItem(0)->toolTip(0).contains("https://"));QVERIFY(!rows->topLevelItem(0)->toolTip(0).contains("coin:"));QVERIFY(!rows->topLevelItem(0)->toolTip(0).contains("[7]"));
    QVERIFY(!rows->topLevelItem(0)->text(0).contains(QStringLiteral("완료 봉 대기")));
    QCOMPARE(lists->rowState(identity).evidence,state.evidence);QCOMPARE(lists->rowState(identity).price,state.price);
    QVERIFY(workspace.findChild<QLabel*>("chartWorkspacePositionText")->isHidden());
    QVERIFY(!workspace.findChild<QLabel*>("chartStudyScorecard")->isVisible());
    QVERIFY(!workspace.findChild<QLabel*>("chartStudySimilaritySummary")->isVisible());
}
void ChartWorkspaceTest::holdingsCoverageIsPrivateAndUserListsSurviveLock() {
    QTemporaryDir directory;
    Vault vault(directory.filePath("synthetic-holdings-coverage-vault"));
    QVERIFY(vault.create("Synthetic holdings coverage password 2026!"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    MarketCandles candles(nullptr);
    ChartWorkspace workspace(&candles, &portfolio);
    connect(&vault, &Vault::aboutToLock, &workspace, &ChartWorkspace::clearPrivate);
    auto* lists = workspace.watchlistModel();
    const auto userList = lists->createList("합성 공개 리스트");
    QVERIFY(!userList.isEmpty());
    QVERIFY(lists->addIdentity(userList, {"coin", "upbit", "KRW-BTC"}));
    workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
    workspace.setPrivateEnabled(true);
    lists->setHoldings({{"coin", "upbit", "KRW-BTC"}});
    workspace.setHoldingRows({syntheticHoldingRow({"coin","upbit","KRW-BTC"})});
    auto* selector = workspace.findChild<QComboBox*>("chartWorkspaceListSelector"); QVERIFY(selector);
    workspace.findChild<QPushButton*>("chartBrowserScope_holdings")->click();
    const auto reason = QStringLiteral("합성 일부 연결의 공개 종목 식별 자료 부족");
    workspace.setHoldingsUnavailableReason(reason);
    auto* status = workspace.findChild<QLabel*>("chartWorkspaceListStatus"); QVERIFY(status);
    auto* panel=workspace.findChild<QWidget*>("chartInstrumentPanel");QVERIFY(panel);
    auto* count=workspace.findChild<QLabel*>("chartBrowserCount");QVERIFY(count);
    QVERIFY(count->isVisibleTo(panel));QVERIFY(count->text().startsWith(QStringLiteral("1개")));
    auto* information=workspace.findChild<InfoButton*>("chartHoldingInformation");QVERIFY(information);
    QVERIFY(information->isVisibleTo(panel));QVERIFY(information->isEnabled());
    QTest::mouseClick(information,Qt::LeftButton);QTRY_VERIFY(information->informationOpen());
    const QPointer<QPlainTextEdit> coverageText=information->findChild<QPlainTextEdit*>("infoPrivateText");
    QVERIFY(coverageText);QVERIFY(coverageText->isVisible());
    QVERIFY(coverageText->toPlainText().contains(reason));QCOMPARE(workspace.holdingsUnavailableReason(),reason);
    const auto publicState = lists->publicState();
    QVERIFY(!publicState.contains(reason.toUtf8()));
    vault.lock();
    QCOMPARE(selector->findData("@holdings"), -1);
    QVERIFY(selector->findData(userList) >= 0);
    QVERIFY(!status->text().contains(reason));QVERIFY(workspace.holdingsUnavailableReason().isEmpty());
    QVERIFY(!information->informationOpen());
    QVERIFY(!coverageText||coverageText->toPlainText().isEmpty());
    QVERIFY(!information->isVisibleTo(panel));QVERIFY(!information->isEnabled());
    QCOMPARE(lists->publicState(), publicState);
    workspace.setHoldingsUnavailableReason(reason); // A delayed private update cannot restore a locked message.
    QVERIFY(!status->text().contains(reason));QVERIFY(workspace.holdingsUnavailableReason().isEmpty());
    QVERIFY(!information->isVisibleTo(panel));QVERIFY(!information->isEnabled());
    for(const auto* text:information->findChildren<QPlainTextEdit*>())
        QVERIFY(!text->toPlainText().contains(reason));
}

void ChartWorkspaceTest::ownedListModelTearsDownWithTheWorkspace() {
    class OwnedTransport final : public TradingTransport {
      public:
        using TradingTransport::TradingTransport;
        void send(quint64, const RequestSpec&) override {}
        void abort() override {}
    };
    auto* workspace = new ChartWorkspace(nullptr, nullptr);
    QPointer<WatchlistWorkspaceList> model = workspace->watchlistModel();
    QPointer<TradingTransport> contracts = new OwnedTransport(workspace);
    workspace->setContractTransport(contracts);
    workspace->setIdentity("coin:binance:BTCUSDT");
    QVERIFY(model);
    QVERIFY(!model->createList("합성 종료 리스트").isEmpty());
    delete workspace;
    QVERIFY(model.isNull());
    QVERIFY(contracts.isNull());
}
void ChartWorkspaceTest::publicCandleModelOutlivesWorkspaceTeardown() {
    class PublicTransport final : public TradingTransport {
      public:
        quint64 token = 0; int aborts = 0;
        void send(quint64 t, const RequestSpec&) override { token = t; }
        void abort() override { ++aborts; }
    } transport;
    MarketCandles model(&transport);
    MarketInstrument instrument; instrument.symbol = "SYNUSDT"; instrument.baseAsset = "SYN";
    instrument.quoteAsset = "USDT"; instrument.market = "spot"; instrument.status = "TRADING"; instrument.tradable = true;
    model.setInstruments("binance", {instrument});
    auto* workspace = new ChartWorkspace(&model, nullptr);
    workspace->setIdentity("coin:binance:SYNUSDT"); workspace->resize(1280, 760); workspace->show();
    QTRY_VERIFY(transport.token);
    QPointer<ChartWorkspace> guard = workspace;
    delete workspace;
    QVERIFY(guard.isNull());
    QVERIFY(transport.aborts > 0);
    QVERIFY(model.series().identity.isEmpty());
    QVERIFY(model.series().candles.isEmpty());
}

void ChartWorkspaceTest::emptyPublicListDoesNotResetOnUnrelatedQuotes() {
    WatchlistWorkspaceList lists;
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.setWatchlistModel(&lists);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.resize(1440, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    QCoreApplication::processEvents();
    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    const WatchlistIdentity observed{"coin", "upbit", "KRW-BTC"};
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy rowChanges(&lists, &WatchlistWorkspaceList::changed);
    for (int cycle = 0; cycle < 2; ++cycle) {
        QVERIFY(workspace.currentListId().isEmpty());
        QCOMPARE(tree->topLevelItemCount(), 0);
        QCOMPARE(tree->columnCount(), 7); // Separate price/change columns plus three metrics revealed by expand.
        for(int column=3;column<6;++column)QVERIFY(tree->isColumnHidden(column));
        const auto publicState = lists.publicState();
        resets.clear(); rowChanges.clear();
        for (int update = 0; update < 3; ++update) {
            WatchlistRowState quote; quote.price = QString::number(100 + update);
            lists.setRowState(observed, quote);
            QCOMPARE(rowChanges.count(), update + 1);
            QCOMPARE(tree->topLevelItemCount(), 0);
            QCOMPARE(lists.publicState(), publicState);
            QCOMPARE(resets.count(), 0); // Empty native table columns must keep their identity.
        }
        QCoreApplication::processEvents();
        QCOMPARE(resets.count(), 0);
        if (cycle == 0) {
            const auto id = lists.createList(QStringLiteral("합성 삭제할 리스트"));
            QVERIFY(lists.addIdentity(id, observed));
            QCOMPARE(tree->topLevelItemCount(), 1);
            const QPersistentModelIndex removedIndex(tree->indexFromItem(tree->topLevelItem(0)));
            QVERIFY(lists.removeList(id));
            QVERIFY(!removedIndex.isValid());
            QVERIFY(resets.count() > 0); // Actual membership removal still invalidates rows.
        }
    }
}

void ChartWorkspaceTest::destroyedListModelKeepsEmptyTreeStable() {
    auto* lists = new WatchlistWorkspaceList;
    const auto id = lists->createList(QStringLiteral("합성 외부 모델"));
    const WatchlistIdentity observed{"coin", "upbit", "KRW-BTC"};
    QVERIFY(lists->addIdentity(id, observed));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.setWatchlistModel(lists);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.resize(1440, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    QCoreApplication::processEvents();
    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 1);
    const QPersistentModelIndex removedIndex(tree->indexFromItem(tree->topLevelItem(0)));
    delete lists;
    QVERIFY(!workspace.watchlistModel());
    QCOMPARE(tree->topLevelItemCount(), 0);
    QVERIFY(!removedIndex.isValid());
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    for (const auto& delayed : {QStringList{observed.key()}, QStringList{}}) {
        workspace.setRealtimeDelayedIdentities(delayed);
        QCOMPARE(tree->topLevelItemCount(), 0);
        QCOMPARE(resets.count(), 0);
    }
    workspace.hide(); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    QCoreApplication::processEvents();
    QVERIFY(!workspace.watchlistModel());
    QCOMPARE(tree->topLevelItemCount(), 0);
    QCOMPARE(resets.count(), 0);
}

void ChartWorkspaceTest::quoteRefreshPreservesScrollAndCollapsedSections() {
    WatchlistWorkspaceList lists;
    const auto id = lists.createList("합성 스크롤 리스트");
    const auto opened = lists.addSection(id, "열린 합성 섹션");
    const auto collapsed = lists.addSection(id, "접은 합성 섹션");
    for (int i = 0; i < 35; ++i) QVERIFY(lists.addIdentity(id, {"coin", "upbit", "KRW-SYN" + QString::number(i)}, opened));
    for (int i = 0; i < 3; ++i) QVERIFY(lists.addIdentity(id, {"coin", "upbit", "KRW-ALT" + QString::number(i)}, collapsed));
    MarketCandles candles(nullptr);
    ChartWorkspace workspace(&candles, nullptr); workspace.setWatchlistModel(&lists);
    QSignalSpy visible(&workspace, &ChartWorkspace::visibleIdentitiesChanged);
    workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 2);
    tree->topLevelItem(1)->setExpanded(false);
    tree->scrollToItem(tree->topLevelItem(0)->child(20), QAbstractItemView::PositionAtCenter);
    QTRY_VERIFY(!visible.isEmpty() && visible.last().first().toStringList().contains("coin:upbit:KRW-SYN20"));
    QTest::qWait(30); // Settle nested responsive layouts before the quote-mutation baseline.
    for(const auto& key:visible.last().first().toStringList())QVERIFY2(!key.contains("KRW-ALT"),"Collapsed section descendants must never receive a visible-row realtime budget");
    const auto stableViewport=tree->viewport()->size();
    const auto position = tree->verticalScrollBar()->value();
    QVERIFY(position > 0);
    const auto identities = visible.last().first().toStringList();
    tree->setCurrentItem(tree->topLevelItem(0)->child(20));
    const QPersistentModelIndex selectedIndex(tree->indexFromItem(tree->currentItem()));
    const QPersistentModelIndex neighbourIndex(tree->indexFromItem(tree->topLevelItem(0)->child(19)));
    const auto savedPublicState=lists.publicState();
    QSignalSpy resets(tree->model(),&QAbstractItemModel::modelReset);
    QSignalSpy changes(tree->model(),&QAbstractItemModel::dataChanged);
    WatchlistRowState state; state.price = "12345"; state.badges = {"합성 완료 지표"}; state.completedAtMs = QDateTime::currentMSecsSinceEpoch() - 1;
    lists.setRowState({"coin", "upbit", "KRW-SYN20"}, state);
    QVERIFY2(selectedIndex.isValid(),"Quote-only updates must retain the selected model index");
    QVERIFY2(neighbourIndex.isValid(),"An unchanged neighbour must retain its model index");
    QCOMPARE(resets.count(),0);
    QVERIFY(!changes.isEmpty());
    for(const auto& change:changes){
        const auto first=qvariant_cast<QModelIndex>(change[0]),last=qvariant_cast<QModelIndex>(change[1]);
        QCOMPARE(first.parent(),selectedIndex.parent());QCOMPARE(last.parent(),selectedIndex.parent());
        QCOMPARE(first.row(),selectedIndex.row());QCOMPARE(last.row(),selectedIndex.row());
    }
    QCOMPARE(tree->currentIndex(),QModelIndex(selectedIndex));
    QCOMPARE(tree->topLevelItem(0)->child(20)->text(1),QString("12,345"));
    QCOMPARE(lists.rowState({"coin","upbit","KRW-SYN20"}).price,QString("12345"));
    QCOMPARE(lists.publicState(),savedPublicState);
    QCoreApplication::processEvents();
    QCOMPARE(tree->viewport()->size(),stableViewport);
    QCOMPARE(tree->verticalScrollBar()->value(), position);
    QVERIFY(!tree->topLevelItem(1)->isExpanded());
    QCOMPARE(visible.last().first().toStringList(), identities);
}


void ChartWorkspaceTest::holdingsPreviewPreservesRowsAndScrollOnQuoteRefresh(){
    QTemporaryDir directory;QVERIFY(directory.isValid());
    Vault vault(directory.filePath("synthetic-holdings-preview"));QVERIFY(vault.create("Synthetic holdings preview password 2026!"));
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
    PortfolioController portfolio(vault,nullptr,factories);
    ChartWorkspace workspace(nullptr,&portfolio);workspace.indicatorPreferences()->markIntroSeen();
    workspace.resize(1440,790);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
    workspace.setPrivateEnabled(true);
    auto* lists=workspace.watchlistModel();QList<WatchlistIdentity> holdings;
    for(int i=0;i<20;++i)holdings.append({"coin","upbit",QString("KRW-SYN%1").arg(i)});
    lists->setHoldings(holdings);QList<HoldingChartRow> holdingsRows;for(const auto& id:holdings)holdingsRows.append(syntheticHoldingRow(id));workspace.setHoldingRows(holdingsRows);
    workspace.findChild<QPushButton*>("chartBrowserScope_holdings")->click();
    auto* tree=workspace.findChild<QTreeWidget*>("chartHoldingAssets");QVERIFY(tree);QVERIFY(tree->isVisible());
    QCOMPARE(tree->topLevelItemCount(),20);tree->setCurrentItem(tree->topLevelItem(10));
    tree->scrollToItem(tree->topLevelItem(10),QAbstractItemView::PositionAtCenter);QCoreApplication::processEvents();
    const auto position=tree->verticalScrollBar()->value();QVERIFY(position>0);
    const QPersistentModelIndex index(tree->indexFromItem(tree->topLevelItem(10)));QSignalSpy resets(tree->model(),&QAbstractItemModel::modelReset);
    WatchlistRowState row;row.price="123456";row.change="+1.25%";lists->setRowState(holdings[10],row);
    QVERIFY2(index.isValid(),"A quote update must preserve a surviving holdings preview row");
    QCOMPARE(resets.size(),0);QCOMPARE(tree->currentIndex(),QModelIndex(index));
    QVERIFY(tree->topLevelItem(10)->text(0).contains("SYN10"));
    QCOMPARE(lists->rowState(holdings[10]).price,QString("123456"));
    QCOMPARE(tree->verticalScrollBar()->value(),position);
    QCoreApplication::processEvents();QCOMPARE(tree->verticalScrollBar()->value(),position);
    // The holdings panel identifies custody and chart source, not public quote
    // badges. Refreshing custody state must preserve the same native row too.
    holdingsRows[10].custodies[0].state=HoldingCustodyState::Delayed;
    holdingsRows[10].alertsEligible=false;workspace.setHoldingRows(holdingsRows);
    QVERIFY(index.isValid());QCOMPARE(resets.size(),0);QCOMPARE(tree->currentIndex(),QModelIndex(index));
    QVERIFY(tree->topLevelItem(10)->toolTip(0).contains(QStringLiteral("지연")));
    QCOMPARE(tree->verticalScrollBar()->value(),position);
    holdingsRows[10].custodies[0].state=HoldingCustodyState::Fresh;holdingsRows[10].alertsEligible=true;workspace.setHoldingRows(holdingsRows);
    QVERIFY(!tree->topLevelItem(10)->toolTip(0).contains(QStringLiteral("지연")));QVERIFY(index.isValid());QCOMPARE(resets.size(),0);
    workspace.clearPrivate();QCOMPARE(tree->topLevelItemCount(),0);QVERIFY(!index.isValid());QVERIFY(workspace.findChild<QLabel*>("chartWorkspaceListStatus")->text().contains("금고"));
}

void ChartWorkspaceTest::reusedQuoteRowsClearPrivateAndRecoverVerificationPresentation(){
    const WatchlistIdentity coin{"coin","upbit","KRW-SYN"},stock{"stock","US","SYN"};
    WatchlistWorkspaceList lists;lists.setVerifiedInstruments({stock});
    const auto id=lists.createList("합성 상태 갱신");QVERIFY(lists.addIdentity(id,coin));QVERIFY(lists.addIdentity(id,stock));lists.useList(id);
    WatchlistRowState privateRow;privateRow.price="345";privateRow.change="+2.00%";privateRow.evidence="SYNTHETIC_PRIVATE_ROW";
    privateRow.badges={"SYNTHETIC_PRIVATE_BADGE"};privateRow.completedAtMs=QDateTime::currentMSecsSinceEpoch()-1;privateRow.privateData=true;
    lists.setRowState(coin,privateRow);
    QTemporaryDir profile;QVERIFY(profile.isValid());Vault vault(profile.filePath("synthetic-row-presentation-vault"));
    QVERIFY(vault.create("Synthetic row presentation password 2026!"));
    PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    ChartWorkspace workspace(nullptr,&portfolio);workspace.setWatchlistModel(&lists);workspace.setIdentity(stock.key());workspace.setPrivateEnabled(true);
    workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),2);
    const QPersistentModelIndex coinIndex(tree->indexFromItem(tree->topLevelItem(0))),stockIndex(tree->indexFromItem(tree->topLevelItem(1)));
    const auto saved=lists.publicState();QSignalSpy resets(tree->model(),&QAbstractItemModel::modelReset);
    QVERIFY(!tree->topLevelItem(0)->toolTip(0).contains("SYNTHETIC_PRIVATE_ROW"));QVERIFY(lists.rowState(coin).evidence.contains("SYNTHETIC_PRIVATE_ROW"));
    QCOMPARE(tree->topLevelItem(0)->data(1,ChangeDirectionRole).toInt(),1);
    workspace.clearPrivate();
    QVERIFY2(coinIndex.isValid(),"Private clearing must clear values without discarding a surviving public row");QVERIFY(stockIndex.isValid());
    QCOMPARE(resets.count(),0);QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("—"));
    QVERIFY(!tree->topLevelItem(0)->toolTip(0).contains("SYNTHETIC_PRIVATE"));QVERIFY(!tree->topLevelItem(0)->text(0).contains("SYNTHETIC_PRIVATE"));
    QCOMPARE(tree->topLevelItem(0)->data(1,ChangeDirectionRole).toInt(),0);QVERIFY(lists.rowState(coin)==WatchlistRowState{});
    lists.setVerifiedInstruments({});QVERIFY(stockIndex.isValid());
    QVERIFY(tree->topLevelItem(1)->text(0).contains(QStringLiteral("확인 필요")));
    QCOMPARE(tree->topLevelItem(1)->foreground(0).color(),themeColors().text3);
    lists.setVerifiedInstruments({stock});QVERIFY(stockIndex.isValid());
    QVERIFY(!tree->topLevelItem(1)->text(0).contains(QStringLiteral("확인 필요")));
    QVERIFY(!tree->topLevelItem(1)->toolTip(0).contains(QStringLiteral("확인 필요")));
    QCOMPARE(tree->topLevelItem(1)->foreground(0).style(),Qt::NoBrush);
    workspace.setRealtimeDelayedIdentities({coin.key()});QVERIFY(tree->topLevelItem(0)->toolTip(0).contains(QStringLiteral("지연")));
    workspace.setRealtimeDelayedIdentities({});QVERIFY(!tree->topLevelItem(0)->toolTip(0).contains(QStringLiteral("지연")));
    WatchlistRowState publicRow;publicRow.price="888";publicRow.change="-3.00%";publicRow.evidence="SYNTHETIC_PUBLIC_EVIDENCE";
    publicRow.badges={"SYNTHETIC_PUBLIC_BADGE"};publicRow.completedAtMs=QDateTime::currentMSecsSinceEpoch()-1;
    lists.setRowState(coin,publicRow);QVERIFY(coinIndex.isValid());QCOMPARE(tree->topLevelItem(0)->data(1,ChangeDirectionRole).toInt(),-1);
    lists.setRowState(coin,{});QVERIFY(coinIndex.isValid());QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("—"));
    QVERIFY(!tree->topLevelItem(0)->toolTip(0).contains("SYNTHETIC_PUBLIC"));
    QCOMPARE(tree->topLevelItem(0)->data(0,Qt::AccessibleTextRole).toString(),tree->topLevelItem(0)->text(0));
    QCOMPARE(tree->topLevelItem(0)->data(1,ChangeDirectionRole).toInt(),0);QCOMPARE(resets.count(),0);QCOMPARE(lists.publicState(),saved);
    QCoreApplication::processEvents();QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("—"));
}

void ChartWorkspaceTest::rowReuseTracksSectionStructureAndModelRebinding(){
    const WatchlistIdentity a{"coin","upbit","KRW-A"},b{"coin","upbit","KRW-B"},c{"coin","upbit","KRW-C"},loose{"coin","upbit","KRW-LOOSE"};
    WatchlistWorkspaceList lists,replacement;const auto id=lists.createList("합성 구조 갱신");
    const auto first=lists.addSection(id,"첫 합성 섹션"),second=lists.addSection(id,"둘째 합성 섹션");
    QVERIFY(lists.addIdentity(id,a,first));QVERIFY(lists.addIdentity(id,b,first));QVERIFY(lists.addIdentity(id,c,second));QVERIFY(lists.addIdentity(id,loose));lists.useList(id);
    ChartWorkspace workspace(nullptr,nullptr);workspace.setWatchlistModel(&lists);workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),3);
    const QPersistentModelIndex sectionIndex(tree->indexFromItem(tree->topLevelItem(0))),rowIndex(tree->indexFromItem(tree->topLevelItem(0)->child(0)));
    tree->topLevelItem(0)->setExpanded(false);QSignalSpy resets(tree->model(),&QAbstractItemModel::modelReset);QSignalSpy changes(tree->model(),&QAbstractItemModel::dataChanged);
    QVERIFY(lists.renameSection(id,first,"이름 바꾼 합성 섹션"));
    QVERIFY2(sectionIndex.isValid(),"Renaming a section is a display update, not a topology reset");QVERIFY(rowIndex.isValid());QCOMPARE(resets.count(),0);
    QCOMPARE(tree->topLevelItem(0)->text(0),QStringLiteral("이름 바꾼 합성 섹션"));QVERIFY(!tree->topLevelItem(0)->isExpanded());
    QVERIFY(!changes.isEmpty());for(const auto& change:changes){QCOMPARE(qvariant_cast<QModelIndex>(change[0]),QModelIndex(sectionIndex));QCOMPARE(qvariant_cast<QModelIndex>(change[1]),QModelIndex(sectionIndex));}
    resets.clear();QVERIFY(lists.moveIdentity(id,b,0,first));QCOMPARE(resets.count(),1);QVERIFY(!rowIndex.isValid());
    QCOMPARE(tree->topLevelItem(0)->child(0)->data(0,Qt::UserRole).toString(),b.key());QCOMPARE(tree->topLevelItem(0)->child(1)->data(0,Qt::UserRole).toString(),a.key());QVERIFY(!tree->topLevelItem(0)->isExpanded());
    resets.clear();QVERIFY(lists.moveSection(id,second,0));QCOMPARE(resets.count(),1);QCOMPARE(tree->topLevelItem(0)->text(0),QStringLiteral("둘째 합성 섹션"));
    QCOMPARE(tree->topLevelItem(0)->child(0)->data(0,Qt::UserRole).toString(),c.key());QVERIFY(!tree->topLevelItem(1)->isExpanded());
    resets.clear();QVERIFY(lists.removeSection(id,first));QCOMPARE(resets.count(),1);QCOMPARE(tree->topLevelItemCount(),4);
    QCOMPARE(tree->topLevelItem(1)->data(0,Qt::UserRole).toString(),b.key());QCOMPARE(tree->topLevelItem(2)->data(0,Qt::UserRole).toString(),a.key());
    resets.clear();QVERIFY(lists.removeIdentity(id,c));QCOMPARE(resets.count(),1);QCOMPARE(tree->topLevelItem(0)->childCount(),0);
    WatchlistRowState oldRow;oldRow.price="111";oldRow.evidence="SYNTHETIC_OLD_MODEL";lists.setRowState(a,oldRow);
    QVERIFY(replacement.restorePublicState(lists.publicState()));const QPersistentModelIndex reboundIndex(tree->indexFromItem(tree->topLevelItem(2)));resets.clear();
    workspace.setWatchlistModel(&replacement);QVERIFY(reboundIndex.isValid());QCOMPARE(resets.count(),0);
    QCOMPARE(tree->topLevelItem(2)->text(1),QStringLiteral("—"));QVERIFY(!tree->topLevelItem(2)->toolTip(0).contains("SYNTHETIC_OLD_MODEL"));
    oldRow.price="222";lists.setRowState(a,oldRow);QCOMPARE(tree->topLevelItem(2)->text(1),QStringLiteral("—"));
    WatchlistRowState fresh;fresh.price="444";replacement.setRowState(a,fresh);QCOMPARE(tree->topLevelItem(2)->text(1),QString("444"));QVERIFY(reboundIndex.isValid());QCOMPARE(resets.count(),0);
}

void ChartWorkspaceTest::listMenusEditMixedPublicIdentitiesAndRoundTripCsv() {
    WatchlistWorkspaceList lists;
    lists.setVerifiedInstruments({{"stock", "US", "NVDA"}});
    MarketCandles candles(nullptr);
    ChartWorkspace workspace(&candles, nullptr);
    workspace.setWatchlistModel(&lists);
    workspace.resize(1280, 760);
    workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    const auto action = [&](const char* name) { return workspace.findChild<QAction*>(name); };
    const auto enterText = [](const QString& value) {
        QTimer::singleShot(0, qApp, [value] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { dialog->setTextValue(value); dialog->accept(); }
        });
    };
    QVERIFY(action("chartListCreate"));
    enterText("합성 관심 리스트"); action("chartListCreate")->trigger();
    QCOMPARE(lists.userLists().size(), 1);
    const auto id = lists.userLists().first().id;
    QCOMPARE(workspace.currentListId(), id);
    enterText("합성 혼합 리스트"); action("chartListRename")->trigger();
    QCOMPARE(lists.userList(id).name, QString("합성 혼합 리스트"));
    action("chartListFlag_blue")->trigger();
    QCOMPARE(lists.userList(id).flag, QString("blue"));
    enterText("합성 섹션"); action("chartListAddSection")->trigger();
    QCOMPARE(lists.userList(id).sections.size(), 1);
    const auto sectionId = lists.userList(id).sections.first().id;
    // Existing saved mixed lists retain their editing/CSV behavior. New additions
    // now use the source-backed picker, covered by the discovery integration test.
    QVERIFY(lists.addIdentity(id, {"coin", "upbit", "KRW-BTC"}, sectionId));
    QVERIFY(lists.addIdentity(id, {"stock", "US", "NVDA"}, sectionId));
    QVERIFY(lists.addIdentity(id, {"coin", "bithumb", "KRW-BTC"}, sectionId));
    QCOMPARE(lists.userList(id).entries.size(), 3);
    QCOMPARE(lists.userList(id).entries[0].sectionId, sectionId);
    QCOMPARE(lists.userList(id).entries[1].identity.key(), QString("stock:US:NVDA"));
    auto* panel = test::openInstrumentPanel(workspace); QVERIFY(panel);
    auto* tree = panel->findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(tree);
    auto* section = tree->topLevelItem(0);
    QVERIFY(section);
    QCOMPARE(section->childCount(), 3);
    QCOMPARE(lists.userList(id).entries[2].identity.key(), QString("coin:bithumb:KRW-BTC"));
    workspace.findChild<QPushButton*>("chartBrowserMarket_us")->click();
    QTest::qWait(20);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(section->child(1)).center());
    QCOMPARE(workspace.selectedChartIdentity(), QString("stock:US:NVDA"));
    auto* unavailable = workspace.findChild<QLabel*>("chartWorkspaceChartUnavailable");
    QVERIFY(unavailable && unavailable->isHidden());
    auto* stockView=workspace.findChild<MarketCandlesView*>();QVERIFY(stockView);QVERIFY(stockView->isVisible());
    auto* stockContext=stockView->findChild<QLabel*>("marketCandlesStockContext");QVERIFY(stockContext);QVERIFY(stockContext->isHidden());
    QVERIFY(stockContext->text().contains(QStringLiteral("원천")));QVERIFY(stockContext->text().contains(QStringLiteral("수정주가")));
    QVERIFY(displayedText(workspace.findChild<QLabel*>("marketCandlesTitle")).contains(QStringLiteral("NVDA · 미국 주식")));
    QVERIFY(stockView->findChild<QWidget*>("marketCandlesChart")->isHidden());QVERIFY(stockView->priceLines().isEmpty());
    QVERIFY(!candles.series().available);QVERIFY(candles.series().identity.isEmpty()); // Stock waiting must not retain/request the previous coin selection.
    workspace.findChild<QPushButton*>("chartBrowserMarket_upbit")->click();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(section->child(2)).center());
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:bithumb:KRW-BTC"));
    QVERIFY(workspace.findChild<MarketCandlesView*>()->isHidden());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(section->child(0)).center());
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:upbit:KRW-BTC"));
    QVERIFY(unavailable->isHidden());
    tree->setCurrentItem(section->child(0)); action("chartListMoveDown")->trigger();
    QCOMPARE(lists.userList(id).entries[1].identity.key(), QString("coin:upbit:KRW-BTC"));
    WatchlistRowState quote; quote.price = "789123"; quote.badges = {"합성 지표 7/10"}; quote.completedAtMs = QDateTime::currentMSecsSinceEpoch() - 1;
    lists.setRowState({"coin", "upbit", "KRW-BTC"}, quote);
    QVERIFY(tree->topLevelItem(0)->child(1)->text(0).contains("합성 지표 7/10"));
    if (const auto preview = qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW"); !preview.isEmpty()) {
        QVERIFY(QDir().mkpath(preview));
        QCoreApplication::processEvents();
        QVERIFY(panel->grab().save(QDir(preview).filePath("c2-lists-wide.png")));
        workspace.resize(820, 1100); QCoreApplication::processEvents();
        QVERIFY(panel->grab().save(QDir(preview).filePath("c2-lists-narrow.png")));
        workspace.resize(1280, 760); QCoreApplication::processEvents();
    }
    QTemporaryDir files;
    const auto csv = files.filePath("synthetic-public-list.csv");
    const auto chooseFile = [&](const QString& path) {
        QTimer::singleShot(0, qApp, [path] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->setDirectory(QFileInfo(path).absolutePath()); dialog->selectFile(QFileInfo(path).fileName());
            QTimer::singleShot(100, dialog, [dialog] { static_cast<QDialog*>(dialog)->accept(); });
        });
    };
    chooseFile(csv); action("chartListExportCsv")->trigger();
    QFile file(csv); QVERIFY(file.open(QIODevice::ReadOnly)); const auto exported = file.readAll(); file.close();
    QVERIFY(exported.contains("NVDA")); QVERIFY(!exported.contains("789123")); QVERIFY(!exported.contains("accountScope"));
    chooseFile(csv); action("chartListImportCsv")->trigger();
    QCOMPARE(lists.userLists().size(), 2);
    QVERIFY(workspace.currentListId() != id);
    QCOMPARE(lists.userList(workspace.currentListId()).entries.size(), 3);
    QVERIFY(lists.recentLists().contains(workspace.currentListId()));
    QSignalSpy alerts(&workspace, &ChartWorkspace::listAlertsRequested);
    action("chartListAlerts")->trigger(); QCOMPARE(alerts.size(), 1);
    QCOMPARE(alerts.first().first().toString(), workspace.currentListId());
}

void ChartWorkspaceTest::realtimeVisibilityFollowsTheActualViewport() {
    WatchlistWorkspaceList lists;
    const auto id = lists.createList("합성 긴 리스트");
    for (int i = 0; i < 35; ++i) QVERIFY(lists.addIdentity(id, {"coin", "upbit", "KRW-SYN" + QString::number(i)}));
    MarketCandles candles(nullptr);
    ChartWorkspace workspace(&candles, nullptr); workspace.setWatchlistModel(&lists);
    QSignalSpy visible(&workspace, &ChartWorkspace::visibleIdentitiesChanged);
    workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    QTRY_VERIFY(!visible.isEmpty());
    auto first = visible.last().first().toStringList();
    QVERIFY(first.contains("coin:upbit:KRW-SYN0"));
    QVERIFY(!first.contains("coin:upbit:KRW-SYN34"));
    QVERIFY(first.size() < 35);
    tree->scrollToItem(tree->topLevelItem(34), QAbstractItemView::PositionAtBottom);
    QTRY_VERIFY(visible.last().first().toStringList().contains("coin:upbit:KRW-SYN34"));
    QVERIFY(!visible.last().first().toStringList().contains("coin:upbit:KRW-SYN0"));
    workspace.setRealtimeDelayedIdentities({"coin:upbit:KRW-SYN34"});
    QVERIFY(tree->topLevelItem(34)->text(0).contains("지연"));
    workspace.hide(); QTRY_VERIFY(visible.last().first().toStringList().isEmpty());
}
void ChartWorkspaceTest::publicListSwitcherWorksWithoutAnUnlockedVault(){
    ChartWorkspace workspace(nullptr,nullptr);workspace.resize(1280,760);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* lists=workspace.findChild<QComboBox*>("chartWorkspaceListSelector");
    QVERIFY2(lists,"Public user lists must be available from the chart without opening a vault");
    QVERIFY(lists->count()>0);QVERIFY(!lists->accessibleName().isEmpty());
}
void ChartWorkspaceTest::savedStockRemainsSelectableForConnectionGuidance_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartWorkspaceTest::savedStockRemainsSelectableForConnectionGuidance(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    WatchlistWorkspaceList lists;const WatchlistIdentity savedStock{"stock","KR","005930"};
    lists.setVerifiedInstruments({savedStock});const auto listId=lists.createList(QStringLiteral("합성 관심 리스트"));
    QVERIFY(lists.addIdentity(listId,savedStock));lists.useList(listId);lists.setVerifiedInstruments({});
    QVERIFY(!lists.isCurrentlyVerified(savedStock));
    QVERIFY(!lists.addIdentity(listId,{"stock","US","AAPL"})); // Adding a new stock still requires verified metadata.
    ChartWorkspace workspace(nullptr,nullptr);workspace.setWatchlistModel(&lists);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    workspace.findChild<QPushButton*>("chartBrowserMarket_kr")->click();QCoreApplication::processEvents();
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),1);
    auto* row=tree->topLevelItem(0);QVERIFY(row);QVERIFY(row->flags().testFlag(Qt::ItemIsEnabled));
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(row).center());
    QCOMPARE(workspace.selectedChartIdentity(),savedStock.key());
    auto* status=workspace.findChild<QLabel*>("marketCandlesStatus");QVERIFY(status);
    QVERIFY(displayedText(status).contains(QStringLiteral("연결된 증권사가 없어요")));
    QVERIFY(displayedText(status).contains(QStringLiteral("키움증권은 국내 일봉")));
    QVERIFY(workspace.findChild<QWidget*>("marketCandlesChart")->isHidden());
    QVERIFY(!workspace.findChild<QToolButton*>("chartDrawingTool_horizontal")->isEnabled());
    QSignalSpy connectRequest(&workspace,&ChartWorkspace::stockConnectionRequested);
    auto* connectButton=workspace.findChild<QPushButton*>("chartStockConnect");QVERIFY(connectButton&&connectButton->isVisible());
    connectButton->click();QCOMPARE(connectRequest.takeFirst(),QVariantList({savedStock.key(),workspace.timeframeMs()}));
    QVERIFY(workspace.setIdentity("stock:US:AAPL"));QCOMPARE(workspace.selectedChartIdentity(),QStringLiteral("stock:US:AAPL"));
    QVERIFY(!lists.isCurrentlyVerified({"stock","US","AAPL"}));
    QVERIFY(workspace.findChild<QWidget*>("marketCandlesChart")->isHidden());
    QVERIFY(!workspace.findChild<QToolButton*>("chartDrawingTool_horizontal")->isEnabled());
    QVERIFY(!workspace.setIdentity("stock:US:INVALID!"));
    QVERIFY(workspace.setIdentity(savedStock.key()));
    if(const auto directory=qEnvironmentVariable("KEEFI_BROKER_ONLY_RENDER_DIR");!directory.isEmpty()){
        QVERIFY(QDir().mkpath(directory));QCoreApplication::processEvents();
        QVERIFY(workspace.grab().save(QDir(directory).filePath(QString("saved-stock-%1.png").arg(dark?"dark":"light"))));
    }
}
void ChartWorkspaceTest::sixTimeframesKeepTheFullSelectedIdentity() {
    ChartWorkspace workspace(nullptr, nullptr); workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    auto* selector = workspace.findChild<QComboBox*>("chartWorkspaceTimeframe");
    QVERIFY2(selector, "Public charts expose their selected candle timeframe");
    QCOMPARE(selector->count(), 6);
    const QList<qint64> intervals{60000, 300000, 900000, 3600000, 14400000, 86400000};
    for (int i = 0; i < intervals.size(); ++i) QCOMPARE(selector->itemData(i).toLongLong(), intervals[i]);
    QCOMPARE(workspace.timeframeMs(), qint64(900000));
    QCOMPARE(selector->currentData().toLongLong(), qint64(900000));
    QSignalSpy selection(&workspace, &ChartWorkspace::selectionChanged);
    workspace.setTimeframe(60000);
    QCOMPARE(workspace.timeframeMs(), qint64(60000));
    QCOMPARE(selection.size(), 1);
    QCOMPARE(selection.last()[0].toString(), QString("coin:upbit:KRW-BTC"));
    QCOMPARE(selection.last()[1].toLongLong(), qint64(60000));
    workspace.setIdentity("coin:binance:SYNUSDT");
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:binance:SYNUSDT"));
    QCOMPARE(workspace.timeframeMs(), qint64(60000));
    QCOMPARE(selection.last()[0].toString(), QString("coin:binance:SYNUSDT"));
    QCOMPARE(selection.last()[1].toLongLong(), qint64(60000));
    selector->setFocus(); QTest::keyClick(selector, Qt::Key_End);
    QCOMPARE(workspace.timeframeMs(), qint64(86400000));
    const auto count = selection.size(); workspace.setTimeframe(123456);
    QCOMPARE(workspace.timeframeMs(), qint64(86400000)); QCOMPARE(selection.size(), count);
}
void ChartWorkspaceTest::revisitedCandlesDoNotPopulateTheCurrentPriceHeadline() {
    class DeferredCandles final:public TradingTransport{
      public:quint64 token=0;void send(quint64 t,const RequestSpec&)override{token=t;}void abort()override{}
    } http;
    qint64 now=QDateTime::currentMSecsSinceEpoch();const auto start=now/60000*60000-60000;
    MarketCandles model(&http,{},[&]{return now;});ChartWorkspace workspace(&model,nullptr);
    workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1440,900);workspace.show();
    workspace.setTimeframe(60000);QVERIFY(workspace.setIdentity("coin:upbit:KRW-SYN"));
    const QJsonObject row{{"market","KRW-SYN"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(start,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(start+1000)},{"unit","1"},{"opening_price","100"},{"high_price","110"},
        {"low_price","90"},{"trade_price","105"},{"candle_acc_trade_volume","2"}};
    HttpResult response;response.status=200;response.body=QJsonDocument(QJsonArray{row}).toJson();
    QVERIFY(http.token);emit http.finished(http.token,response);QVERIFY(model.series().available);
    auto* price=workspace.findChild<QLabel*>("chartCurrentPrice");auto* chart=workspace.findChild<QWidget*>("marketCandlesChart");
    QVERIFY(price&&chart);QCOMPARE(price->text(),QString("105"));
    QVERIFY(workspace.setIdentity("coin:upbit:KRW-OTHER"));QVERIFY(workspace.setIdentity("coin:upbit:KRW-SYN"));
    QVERIFY(model.displayObservation().fromMemoryCache);QVERIFY(!model.series().available);
    QCOMPARE(chart->property("displayCompletedCount").toInt(),1);QVERIFY(chart->isVisible());
    QCOMPARE(price->text(),QStringLiteral("—")); // a 24-hour display cache must not act as a current quote
    emit http.finished(http.token,response);QVERIFY(model.series().available);QCOMPARE(price->text(),QString("105"));
    now+=MarketCandles::FreshnessMs+1;emit model.changed();
    QVERIFY(!model.displayObservation().fromMemoryCache);
    QCOMPARE(price->text(),QString("105")); // existing same-selection delayed quote presentation remains intact
    QVERIFY(price->accessibleName().contains(QStringLiteral("지연")));
}
void ChartWorkspaceTest::cachedBinanceQuoteKeepsPriceAndExplainsDelay() {
    const WatchlistIdentity identity{"coin","binance","SYNUSDT"};
    constexpr qint64 received=1000000;
    PublicTick tick;tick.market=identity.code;tick.signedChangeRate="0.2345";
    tick.quote={"SYN","USDT","1.2345",{"Binance 공식 USDT 현재가","wss://stream.binance.com:9443/ws",
        "public:binance:SYNUSDT",received,received,true}};
    MarketCandles candles(nullptr);ChartWorkspace workspace(&candles,nullptr);auto* lists=workspace.watchlistModel();
    const auto list=lists->createList(QStringLiteral("합성 지연 관심"));QVERIFY(lists->addIdentity(list,identity));
    lists->setVerifiedInstruments({identity});workspace.indicatorPreferences()->markIntroSeen();
    workspace.resize(1440,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
    workspace.setIdentity(identity.key());
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),1);
    const auto publicState=lists->publicState();
    lists->setRowState(identity,projectBinanceWatchlistQuote(identity,tick,received+90000));
    QVERIFY(tree->topLevelItem(0)->text(1).contains("1.2345"));
    const auto delayed=projectBinanceWatchlistQuote(identity,tick,received+90001);
    QCOMPARE(delayed.price,QString("1.2345"));QCOMPARE(delayed.quoteState,WatchlistQuoteState::Delayed);
    lists->setRowState(identity,delayed);
    QVERIFY(tree->topLevelItem(0)->text(1).contains("1.2345"));
    QVERIFY(tree->topLevelItem(0)->text(1).contains(QStringLiteral("지연")));
    auto* current=workspace.findChild<QLabel*>("chartCurrentPrice");QVERIFY(current);
    auto* reference=workspace.findChild<QLabel*>("chartCurrentPriceReference");QVERIFY(reference);
    QCOMPARE(current->text(),QString("1.2345"));QVERIFY(current->accessibleName().contains(QStringLiteral("지연")));
    auto* unit=workspace.findChild<QLabel*>("chartCurrentPriceUnit");QVERIFY(unit);QCOMPARE(unit->text(),QString("USDT"));
    QVERIFY(displayedText(reference).contains(QStringLiteral("지연")));
    lists->setRowState(identity,projectBinanceWatchlistQuote(identity,tick,received+90001,true));
    QVERIFY(tree->topLevelItem(0)->text(1).contains("1.2345"));
    QVERIFY(tree->topLevelItem(0)->text(1).contains(QStringLiteral("끊김")));
    QVERIFY(displayedText(reference).contains(QStringLiteral("끊김")));
    QCOMPARE(lists->publicState(),publicState);QVERIFY(!lists->rowState(identity).privateData);
    workspace.setIdentity("coin:binance:OTHERUSDT");QCOMPARE(current->text(),QStringLiteral("—"));
    workspace.clearPrivate();QCOMPARE(lists->publicState(),publicState);
}
void ChartWorkspaceTest::missingQuotesUseDistinctShortStates() {
    ChartWorkspace workspace(nullptr,nullptr); auto* lists=workspace.watchlistModel();
    const WatchlistIdentity identity{"coin","binance","SYNUSDT"};
    const auto list=lists->createList(QStringLiteral("상태 확인")); QVERIFY(lists->addIdentity(list,identity));
    workspace.resize(1440,790);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
    QCoreApplication::processEvents();
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),1);
    QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("—"));
    WatchlistRowState row;row.quoteState=WatchlistQuoteState::Disconnected;lists->setRowState(identity,row);
    QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("끊김"));
    row.quoteState=WatchlistQuoteState::Unsupported;lists->setRowState(identity,row);
    QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("지원 없음"));
    row.quoteState=WatchlistQuoteState::ConnectionRequired;lists->setRowState(identity,row);
    QCOMPARE(tree->topLevelItem(0)->text(1),QStringLiteral("연결 필요"));
    row.price="123.45 USDT";row.change="+1.25%";lists->setRowState(identity,row);
    QVERIFY(tree->topLevelItem(0)->text(1).startsWith(QStringLiteral("123.45 USDT")));
    const auto before=lists->publicState();workspace.clearPrivate();QCOMPARE(lists->publicState(),before);
    QVERIFY(tree->topLevelItem(0)->text(1).startsWith(QStringLiteral("123.45 USDT")));
}
void ChartWorkspaceTest::widePositionsRemainReadableBesideMixedPublicLists() {
    WatchlistWorkspaceList lists;
    lists.setVerifiedInstruments({{"stock", "US", "NVDA"}});
    const auto id = lists.createList("합성 혼합 리스트");
    const auto section = lists.addSection(id, "합성 섹션");
    QVERIFY(lists.addIdentity(id, {"coin", "upbit", "KRW-BTC"}, section));
    QVERIFY(lists.addIdentity(id, {"stock", "US", "NVDA"}, section));
    QVERIFY(lists.addIdentity(id, {"coin", "bithumb", "KRW-BTC"}, section));
    MarketCandles candles(nullptr);
    ChartWorkspace workspace(&candles, nullptr);
    workspace.setWatchlistModel(&lists);
    workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QCoreApplication::processEvents();
    auto* positions = workspace.findChild<QWidget*>("chartWorkspacePositions");
    auto* anchor = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* chart = workspace.findChild<MarketCandlesView*>();
    QVERIFY(positions); QVERIFY(anchor); QVERIFY(chart);
    QVERIFY2(positions->isVisible(), "The locked position panel keeps its dashboard location");
    QVERIFY(workspace.findChild<QLabel*>("chartWorkspacePositionLocked")->isVisible());
    workspace.resize(820, 1100); QCoreApplication::processEvents();
    QVERIFY(workspace.width() <= 820);
    QVERIFY(chart->mapToGlobal(QPoint()).y() < positions->mapToGlobal(QPoint()).y());
    auto* panel = test::openInstrumentPanel(workspace); QVERIFY(panel);
    QVERIFY(panel->isAncestorOf(workspace.findChild<QWidget*>("chartWorkspaceWatchlist")));
    QVERIFY(positions->isVisible());
}
void ChartWorkspaceTest::priceColumnRemainsVisibleAfterNarrowToWideResize() {
    WatchlistWorkspaceList lists;
    lists.setVerifiedInstruments({{"stock", "US", "NVDA"}});
    const auto id = lists.createList("합성 가격 열 리스트");
    const auto section = lists.addSection(id, "합성 혼합 섹션");
    QVERIFY(lists.addIdentity(id, {"coin", "upbit", "KRW-BTC"}, section));
    QVERIFY(lists.addIdentity(id, {"stock", "US", "NVDA"}, section));
    QVERIFY(lists.addIdentity(id, {"coin", "bithumb", "KRW-BTC"}, section));
    MarketCandles candles(nullptr);
    ChartWorkspace workspace(&candles, nullptr); workspace.setWatchlistModel(&lists);
    workspace.resize(820, 1100); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
     QCoreApplication::processEvents();
    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    tree->setCurrentItem(tree->topLevelItem(0)->child(1));
    WatchlistRowState state; state.price = "789123"; state.change = "+1.23%";
    lists.setRowState({"coin", "upbit", "KRW-BTC"}, state);
    QCoreApplication::processEvents();
    auto* selection = tree->currentItem(); QVERIFY(selection);
    const auto identity = workspace.selectedIdentity();
    tree->horizontalScrollBar()->setValue(0);
    workspace.resize(1280, 760); QCoreApplication::processEvents();
    QCOMPARE(tree->horizontalScrollBar()->value(), 0);
    QVERIFY2(tree->columnViewportPosition(1) + tree->columnWidth(1) <= tree->viewport()->width(),
        "Both public identity and price columns must fit after returning to a wide chart layout");
    QCOMPARE(tree->currentItem(), selection);
    QCOMPARE(workspace.selectedIdentity(), identity);
}
void ChartWorkspaceTest::expandedDetailsDoNotEnlargeTheNarrowWindow(){
    class PublicTransport final:public TradingTransport{public:quint64 token=0;void send(quint64 t,const RequestSpec&)override{token=t;}void abort()override{}} transport;
    MarketCandles model(&transport);ChartWorkspace workspace(&model,nullptr);workspace.resize(720,760);workspace.show(); selectSavedScopeAndExpandDetails(workspace);QTRY_VERIFY(transport.token);
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto end=now/MarketCandles::IntervalMs*MarketCandles::IntervalMs;QJsonArray rows;
    for(int i=1;i<=60;++i){const auto at=end-i*MarketCandles::IntervalMs;const auto price=QString::number(100+(i*7)%13);
        rows.append(QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(at+1000)},{"unit","15"},{"opening_price",price},{"high_price",decimal::add(price,"4")},
            {"low_price",decimal::subtract(price,"3")},{"trade_price",price},{"candle_acc_trade_volume","2"}});}
    emit transport.finished(transport.token,upbit_fixture::json(rows));
    auto* information=workspace.findChild<InfoButton*>("marketCandlesInformation");QVERIFY(information);information->click();QCoreApplication::processEvents();
    QVERIFY2(workspace.height()<=760,"Calculation information must leave the narrow window size intact");QVERIFY(workspace.width()<=720);
    auto* dialog=information->findChild<QDialog*>("infoDialog");QVERIFY(dialog);QVERIFY(dialog->width()<=720);
    auto* body=dialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(body);QTRY_VERIFY(body->toPlainText().contains("ADX(14)"));QVERIFY(!body->toPlainText().contains("https://"));
    auto* scroll=workspace.findChild<QScrollArea*>("chartWorkspaceScroll");QVERIFY(scroll);QVERIFY(scroll->horizontalScrollBar()->maximum()==0);
    dialog->close();

}

void ChartWorkspaceTest::chartComesFirstInNarrowWindow() {
    class Fake final : public TradingTransport {
      public:
        void send(quint64, const RequestSpec&) override {}
        void abort() override {}
    } transport;
    MarketCandles model(&transport);
    ChartWorkspace workspace(&model, nullptr);
    workspace.resize(1280, 760);
    workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QTest::qWait(10);
    auto* chart = workspace.findChild<MarketCandlesView*>();
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* positions = workspace.findChild<QWidget*>("chartWorkspacePositions");
    QVERIFY(chart);
    QVERIFY(panel);
    QVERIFY(positions);
    QVERIFY(!panel->isVisible());
    QVERIFY(positions->isVisible());
    workspace.resize(720, 760);
    QTest::qWait(10);
    QVERIFY(chart->isVisible());
    QVERIFY(!panel->isVisible());
    QCOMPARE(test::openInstrumentPanel(workspace), panel);
    QVERIFY2(positions->isVisible(), "The narrow layout keeps the explicit vault explanation");
    QVERIFY(workspace.width() <= 720);
    QVERIFY(workspace.findChild<QLineEdit*>("chartWorkspaceAsset")->text().isEmpty());
    QVERIFY(workspace.findChild<QLineEdit*>("chartWorkspaceAsset")->placeholderText().contains(QStringLiteral("이름 또는 코드")));
    QCOMPARE(workspace.selectedChartIdentity(),QStringLiteral("coin:upbit:KRW-BTC"));
}

void ChartWorkspaceTest::positionQuantitySurvivesMissingAverageWithoutReadsOrDataChanges_data() {
    QTest::addColumn<bool>("snapshotOnly");
    QTest::newRow("upbit-missing-average") << false;
    QTest::newRow("binance-snapshot-only") << true;
}
void ChartWorkspaceTest::positionQuantitySurvivesMissingAverageWithoutReadsOrDataChanges() {
    QFETCH(bool, snapshotOnly);
    class PositionTransport final : public TradingTransport {
      public:
        int calls = 0;
        bool snapshotOnly = false;
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation));
            ++calls;
            auto response = upbit_fixture::response(request.operation);
            if (snapshotOnly) {
                QVERIFY(request.operation == Operation::Time || request.operation == Operation::Account);
                response = upbit_fixture::json(request.operation == Operation::Time
                    ? QJsonObject{{"serverTime", QString::number(QDateTime::currentMSecsSinceEpoch())}}
                    : QJsonObject{{"canTrade", true}, {"accountType", "SPOT"}, {"balances", QJsonArray{
                        QJsonObject{{"asset", "BTC"}, {"free", "2.5"}, {"locked", "0.75"}}}}});
            } else if (request.operation == Operation::Account) {
                auto rows = QJsonDocument::fromJson(response.body).array();
                auto btc = rows[1].toObject();
                btc["balance"] = "2.5"; btc["locked"] = "0.75";
                btc.remove("avg_buy_price");
                rows[1] = btc; response = upbit_fixture::json(rows);
            }
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override {}
    } transport;
    class PublicTransport final : public TradingTransport {
      public:
        int calls = 0;
        void send(quint64, const RequestSpec& request) override { QVERIFY(!request.authenticated); ++calls; }
        void abort() override {}
    } publicTransport;
    QTemporaryDir directory;
    Vault vault(directory.filePath("synthetic-position-quantity-vault"));
    QVERIFY(vault.create("Synthetic quantity projection password 2026!"));
    auto account = upbit_fixture::account(); account.id.clear();
    if (snapshotOnly) { account.provider = "binance"; account.currency = "USDT"; }
    transport.snapshotOnly = snapshotOnly;
    const QMap<QString, QString> credentials{{snapshotOnly ? "api_key" : "access_key", "FakeQuantityKey"}, {"secret", "FakeQuantitySecret"}};
    QVERIFY(vault.addAccount(account, credentials, StorageMode::EncryptedVault));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord&) { return &transport; };
    PortfolioController portfolio(vault, nullptr, factories);
    portfolio.setAutomaticRefresh(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(), 1, 8000);
    portfolio.setAutomaticRefresh(false);
    auto* existing = portfolio.trading(vault.accounts().first().id);
    QVERIFY(existing);
    const auto originalBalances = existing->balances();
    const auto originalSnapshots = portfolio.snapshots();
    const auto privateReads = transport.calls;
    MarketCandles model(&publicTransport);
    ChartWorkspace workspace(&model, &portfolio);
    workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace); workspace.setPrivateEnabled(true);
    auto* text = workspace.findChild<QLabel*>("chartWorkspacePositionText");
    QVERIFY(text);
    QVERIFY2(text->isVisible(), "A real holding stays visible when its provider supplies no average");
    QVERIFY(displayedText(text).contains("수량 3.25 BTC"));
    QVERIFY(!displayedText(text).contains("평균"));
    QVERIFY(!displayedText(text).contains("평가손익"));
    QCoreApplication::processEvents();
    QCOMPARE(transport.calls, privateReads);
    const auto publicReads = publicTransport.calls;
    const auto profileBytes = workspace.watchlistModel()->publicState();
    for (int i = 0; i < 4; ++i) emit portfolio.changed();
    QCoreApplication::processEvents();
    QCOMPARE(transport.calls, privateReads);
    QCOMPARE(publicTransport.calls, publicReads);
    QCOMPARE(portfolio.snapshots(), originalSnapshots);
    QCOMPARE(existing->balances().size(), originalBalances.size());
    for (int i = 0; i < originalBalances.size(); ++i) {
        const auto before = originalBalances[i], after = existing->balances()[i];
        QCOMPARE(after.asset, before.asset); QCOMPARE(after.free, before.free); QCOMPARE(after.locked, before.locked);
        QCOMPARE(after.section, before.section); QCOMPARE(after.cashBalance, before.cashBalance);
        QCOMPARE(after.equity, before.equity); QCOMPARE(after.liability, before.liability); QCOMPARE(after.providerAveragePrice, before.providerAveragePrice);
        QCOMPARE(after.providerAverageCurrency, before.providerAverageCurrency);
    }
    QCOMPARE(workspace.watchlistModel()->publicState(), profileBytes);
    vault.lock(); QVERIFY(text->isHidden()); QVERIFY(displayedText(text).isEmpty());
    emit portfolio.changed(); QVERIFY(text->isHidden()); QVERIFY(displayedText(text).isEmpty());
    QCOMPARE(transport.calls, privateReads);
}

void ChartWorkspaceTest::brokerPositionShowsOnlyMatchingReportedValues() {
    class BrokerTransport final : public TradingTransport {
      public:
        int calls = 0;
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation)); ++calls;
            auto response = keefi::test::tossResponse(request.operation);
            if (request.operation == Operation::Holdings) {
                auto root = QJsonDocument::fromJson(response.body).object();
                auto result = root["result"].toObject(); auto rows = result["items"].toArray();
                auto us = rows.first().toObject();
                us["symbol"] = "SYNX"; us["marketCountry"] = "US"; us["currency"] = "USD";
                us["quantity"] = "7"; us["averagePurchasePrice"] = "50";
                rows.append(us); result["items"] = rows; root["result"] = result;
                response.body = QJsonDocument(root).toJson(QJsonDocument::Compact);
            }
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override {}
    } transport;
    class PublicTransport final : public TradingTransport {
      public:
        int calls = 0;
        void send(quint64, const RequestSpec& request) override { QVERIFY(!request.authenticated); ++calls; }
        void abort() override {}
    } publicTransport;
    QTemporaryDir directory;
    Vault vault(directory.filePath("synthetic-broker-position-vault"));
    QVERIFY(vault.create("Synthetic broker projection password 2026!"));
    AccountRecord account; account.provider = "toss"; account.environment = "live";
    account.market = "domestic_equity"; account.currency = "KRW"; account.alias = "synthetic broker";
    QVERIFY(vault.addAccount(account, {{"client_id", "FakePositionClientId"}, {"client_secret", "FakePositionClientSecret"}}, StorageMode::EncryptedVault));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord&) { return &transport; };
    PortfolioController portfolio(vault, nullptr, factories); portfolio.setAutomaticRefresh(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(), 1, 8000);
    portfolio.setAutomaticRefresh(false);
    auto* broker = portfolio.broker(vault.accounts().first().id); QVERIFY(broker);
    const auto originalHoldings = QJsonDocument(broker->holdings()).toJson(QJsonDocument::Compact);
    const auto originalSnapshots = portfolio.snapshots(); const auto privateReads = transport.calls;
    QCOMPARE(portfolio.connections().first().state, QString("complete"));
    QCOMPARE(originalSnapshots.first().timeMs, portfolio.connections().first().receivedAtMs);
    QCOMPARE(originalSnapshots.first().accountScope, portfolio.connections().first().accountScope);
    QVERIFY(originalSnapshots.first().evidence.complete);
    QVERIFY(!originalSnapshots.first().balanceComplete); // Toss has no depositCash source; known holdings remain complete.
    const auto knownHolding = originalSnapshots.first().components.first();
    QCOMPARE(knownHolding.asset, QString("005930"));
    QVERIFY(knownHolding.evidence.complete);
    QCOMPARE(knownHolding.evidence.scope, originalSnapshots.first().accountScope);
    MarketCandles model(&publicTransport); ChartWorkspace workspace(&model, &portfolio);
    workspace.watchlistModel()->setVerifiedInstruments({{"stock", "KR", "005930"}, {"stock", "KR", "005935"}, {"stock", "US", "SYNX"}});
    workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace); workspace.setPrivateEnabled(true);
    workspace.setIdentity("stock:KR:005930");
    auto* text = workspace.findChild<QLabel*>("chartWorkspacePositionText"); QVERIFY(text);
    QVERIFY2(text->isVisible(), "Verified KR selection can show its already-loaded broker holding");
    QVERIFY(displayedText(text).contains("수량 100"));
    QVERIFY(displayedText(text).contains("내 평균단가 65,000 KRW"));
    QVERIFY(displayedText(text).contains("평가손익 500,000 KRW"));
    QVERIFY(!displayedText(text).contains('%')); // The stored rate's unit has not been established here.
    QVERIFY(!displayedText(text).contains("490,000")); // Do not silently substitute after-cost P&L.
    QCoreApplication::processEvents();
    const auto publicReads = publicTransport.calls;
    const auto profileBytes = workspace.watchlistModel()->publicState();
    for (int i = 0; i < 3; ++i) emit portfolio.changed();
    QCoreApplication::processEvents();
    QCOMPARE(transport.calls, privateReads); QCOMPARE(publicTransport.calls, publicReads);
    QCOMPARE(QJsonDocument(broker->holdings()).toJson(QJsonDocument::Compact), originalHoldings);
    QCOMPARE(portfolio.snapshots(), originalSnapshots);
    QCOMPARE(workspace.watchlistModel()->publicState(), profileBytes);
    if (const auto out = qEnvironmentVariable("KEEFI_USABILITY_P2_RENDER_DIR"); !out.isEmpty()) {
        QVERIFY(QDir().mkpath(out)); const auto previous = KeefiTheme::instance()->mode(); const auto originalSize = workspace.size();
        workspace.resize(1800, 1000);
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode); workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
            QCoreApplication::processEvents();
            QVERIFY(workspace.grab().save(QDir(out).filePath(QString("positions-broker-%1.png").arg(mode == ThemeMode::Light ? "light" : "dark"))));
        }
        KeefiTheme::instance()->setMode(previous); workspace.resize(originalSize);
    }
    workspace.setIdentity("stock:KR:005935"); QVERIFY(text->isHidden());
    workspace.setIdentity("stock:US:SYNX"); QVERIFY(text->isHidden()); // Identity presence is not a retained US quantity/average.
    workspace.setIdentity("stock:KR:005930"); QVERIFY(text->isVisible());
    vault.lock(); QVERIFY(text->isHidden()); QVERIFY(displayedText(text).isEmpty());
    emit portfolio.changed(); QVERIFY(text->isHidden()); QVERIFY(displayedText(text).isEmpty());
    QCOMPARE(transport.calls, privateReads);
}

void ChartWorkspaceTest::providerAveragesStaySeparateAndLockCannotRehydrateThem() {
    class AverageTransport final : public TradingTransport {
      public:
        QString average;
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation));
            auto response = upbit_fixture::response(request.operation);
            if (request.operation == Operation::Account) {
                auto rows = QJsonDocument::fromJson(response.body).array();
                auto btc = rows[1].toObject();
                btc["avg_buy_price"] = average;
                rows[1] = btc;
                response = upbit_fixture::json(rows);
            }
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override {}
    } first, second;
    class PublicTransport final : public TradingTransport {
      public:
        quint64 pending = 0;
        void send(quint64 token, const RequestSpec& request) override { QVERIFY(!request.authenticated); pending = token; }
        void abort() override {}
    } publicTransport;
    first.average = "90000";
    second.average = "91000";
    QTemporaryDir directory;
    Vault vault(directory.filePath("fake-chart-vault"));
    QVERIFY(vault.create("Fake chart workspace password 2026!"));
    auto account = upbit_fixture::account();
    account.id.clear();
    account.alias = "합성 차트 계좌 A";
    QVERIFY(vault.addAccount(account, {{"access_key", "FakeChartWorkspaceKeyA"}, {"secret", "FakeChartWorkspaceSecretA"}}, StorageMode::EncryptedVault));
    account.alias = "합성 차트 계좌 B";
    QVERIFY(vault.addAccount(account, {{"access_key", "FakeChartWorkspaceKeyB"}, {"secret", "FakeChartWorkspaceSecretB"}}, StorageMode::EncryptedVault));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord& selected) { return selected.alias.endsWith('A') ? &first : &second; };
    PortfolioController portfolio(vault, nullptr, factories);
    portfolio.setAutomaticRefresh(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(), 2, 8000);
    MarketCandles model(&publicTransport);
    ChartWorkspace workspace(&model, &portfolio);
    workspace.resize(1280, 760);
    workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    workspace.setPrivateEnabled(true);
    auto* lists = workspace.watchlistModel();
    QVERIFY(lists);
    const auto publicList = lists->createList("합성 공개 리스트 유지");
    QVERIFY(!publicList.isEmpty());
    QVERIFY(lists->addIdentity(publicList, {"coin", "upbit", "KRW-BTC"}));
    lists->setHoldings({{"coin", "upbit", "KRW-BTC"}});
    workspace.setHoldingRows({syntheticHoldingRow({"coin","upbit","KRW-BTC"})});
    auto* selector = workspace.findChild<QComboBox*>("chartWorkspaceListSelector");
    QVERIFY(selector);
    QVERIFY(workspace.findChild<QPushButton*>("chartBrowserScope_holdings")->isVisible());
    workspace.findChild<QPushButton*>("chartBrowserScope_holdings")->click();
    QCOMPARE(workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist")->topLevelItemCount(), 1);
    const auto savedPublicList = lists->publicState();
    auto* chart = workspace.findChild<MarketCandlesView*>();
    auto* text = workspace.findChild<QLabel*>("chartWorkspacePositionText");
    QVERIFY(chart);
    QVERIFY(text);
    QTRY_COMPARE_WITH_TIMEOUT(chart->priceLines().size(), 2, 2500);
    const auto lines = chart->priceLines();
    QStringList prices{lines[0].price, lines[1].price};
    prices.sort();
    QCOMPARE(prices, QStringList({"90000", "91000"}));
    QVERIFY(!lines[0].accountScope.isEmpty());
    QVERIFY(lines[0].accountScope != lines[1].accountScope);
    QVERIFY(lines[0].receivedAtMs > 0);
    QVERIFY(lines[0].sourceUrl.startsWith("https://api.upbit.com/"));
    QVERIFY(displayedText(text).contains("내 평균단가 90,000 KRW"));
    QVERIFY(displayedText(text).contains("내 평균단가 91,000 KRW"));
    QCOMPARE(displayedText(text).count("수량 10 BTC"), 2);
    QVERIFY(!displayedText(text).contains("KST"));QVERIFY(lines[0].receivedAtMs>0);QVERIFY(!lines[0].sourceUrl.isEmpty());
    QTRY_VERIFY(publicTransport.pending != 0);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto start = (now / MarketCandles::IntervalMs - 1) * MarketCandles::IntervalMs;
    QJsonObject candle{{"market", "KRW-BTC"}, {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(start, Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp", QString::number(start + 1000)}, {"unit", "15"}, {"opening_price", "100"}, {"high_price", "110"},
        {"low_price", "90"}, {"trade_price", "100"}, {"candle_acc_trade_volume", "2"}};
    emit publicTransport.finished(publicTransport.pending, upbit_fixture::json(QJsonArray{candle}));
    QVERIFY(model.series().available);
    if (const auto out = qEnvironmentVariable("KEEFI_USABILITY_P2_RENDER_DIR"); !out.isEmpty()) {
        QVERIFY(QDir().mkpath(out)); const auto previous = KeefiTheme::instance()->mode(); const auto originalSize = workspace.size();
        workspace.resize(1800, 1000);
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode); workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
            QCoreApplication::processEvents();
            QVERIFY(workspace.grab().save(QDir(out).filePath(QString("positions-spot-%1.png").arg(mode == ThemeMode::Light ? "light" : "dark"))));
        }
        KeefiTheme::instance()->setMode(previous); workspace.resize(originalSize);
    }
    workspace.clearPrivate();
    QVERIFY(lists->holdings().isEmpty());
    QCOMPARE(selector->findData("@holdings"), -1);
    QVERIFY(selector->findData(publicList) >= 0);
    QCOMPARE(lists->publicState(), savedPublicList);
    QVERIFY(chart->priceLines().isEmpty());
    QVERIFY(!displayedText(text).contains("90000"));
    emit portfolio.changed();
    QTest::qWait(1100);
    QVERIFY(chart->priceLines().isEmpty());
    workspace.setPrivateEnabled(true);
    QCOMPARE(chart->priceLines().size(), 2);
    lists->setHoldings({{"coin", "upbit", "KRW-BTC"}});
    vault.lock();
    QVERIFY(lists->holdings().isEmpty());
    QCOMPARE(selector->findData("@holdings"), -1);
    QVERIFY(selector->findData(publicList) >= 0);
    QVERIFY(chart->priceLines().isEmpty());
    QVERIFY(!displayedText(text).contains("90000"));
    QVERIFY(!displayedText(text).contains("91000"));
    QVERIFY(text->toolTip().isEmpty());
    emit portfolio.changed();
    QTest::qWait(1100);
    QVERIFY(chart->priceLines().isEmpty());
    QVERIFY(model.series().available);
    QCOMPARE(model.series().candles.size(), 1);
}

void ChartWorkspaceTest::futuresDistanceUsesReportedMarkAndNeverTheKrwAxis() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    PositionRecord position;
    position.symbol = "BTCUSDT";
    position.positionSide = "SHORT";
    position.amount = "-2";
    position.entryPrice = "100";
    position.markPrice = "125";
    position.liquidationPrice = "200";
    position.unrealizedPnl = "-50.5";
    position.marginAsset = "USDT";
    MarketInstrument instrument;
    instrument.symbol = "BTCUSDT";
    instrument.baseAsset = "BTC";
    instrument.quoteAsset = "USDT";
    const auto projected = projectChartFuturesPosition(position, instrument, "BTC", "binance", "synthetic-futures-scope", now, now + 500);
    QVERIFY(projected.available);
    QVERIFY(projected.distanceAvailable);
    QCOMPARE(projected.liquidationDistancePercent, 60.0); // |200 - 125| / 125 × 100.
    QCOMPARE(projected.asset, QString("BTC"));
    QCOMPARE(projected.currency, QString("USDT"));
    QCOMPARE(projected.provider, QString("binance"));
    QCOMPARE(projected.accountScope, QString("synthetic-futures-scope"));
    QCOMPARE(projected.contractSymbol, position.symbol);
    QCOMPARE(projected.positionSide, QString("SHORT"));
    QCOMPARE(projected.markPrice, QString("125"));
    QVERIFY(projected.quantityAvailable);
    QCOMPARE(projected.amount, QString("-2"));
    QCOMPARE(projected.unrealizedPnl, QString("-50.5"));
    QCOMPARE(projected.pnlCurrency, QString("USDT"));
    QCOMPARE(projected.sourceUrl, QString("https://fapi.binance.com/fapi/v3/positionRisk"));
    QCOMPARE(projected.receivedAtMs, now);
    QVERIFY(!projectChartFuturesPosition(position, {}, "BTC", "binance", "synthetic-futures-scope", now, now).available);
    QVERIFY(!projectChartFuturesPosition(position, instrument, "ETH", "binance", "synthetic-futures-scope", now, now).available);
    QVERIFY(!projectChartFuturesPosition(position, instrument, "BTC", "binance", "synthetic-futures-scope", now - 180001, now).available);
    QVERIFY(!projectChartFuturesPosition(position, instrument, "BTC", "binance", "synthetic-futures-scope", now + 1, now).available);
    auto malformed = position;
    malformed.markPrice = "125\n";
    QVERIFY(!projectChartFuturesPosition(malformed, instrument, "BTC", "binance", "synthetic-futures-scope", now, now).distanceAvailable);
    malformed = position; malformed.marginAsset = "USDC";
    const auto mismatchedCurrency = projectChartFuturesPosition(malformed, instrument, "BTC", "binance", "synthetic-futures-scope", now, now);
    QVERIFY(mismatchedCurrency.available); QVERIFY(mismatchedCurrency.quantityAvailable);
    QVERIFY(mismatchedCurrency.unrealizedPnl.isEmpty()); QVERIFY(mismatchedCurrency.pnlCurrency.isEmpty());
    malformed = position; malformed.unrealizedPnl = "-50.5\n";
    QVERIFY(projectChartFuturesPosition(malformed, instrument, "BTC", "binance", "synthetic-futures-scope", now, now).unrealizedPnl.isEmpty());
    malformed = position; malformed.unrealizedPnl.clear();
    QVERIFY(projectChartFuturesPosition(malformed, instrument, "BTC", "binance", "synthetic-futures-scope", now, now).unrealizedPnl.isEmpty());
    malformed.unrealizedPnl = "0";
    QCOMPARE(projectChartFuturesPosition(malformed, instrument, "BTC", "binance", "synthetic-futures-scope", now, now).unrealizedPnl, QString("0"));
    auto quantityOnly = position; quantityOnly.entryPrice = "0"; quantityOnly.liquidationPrice = "0";
    const auto withoutPrices = projectChartFuturesPosition(quantityOnly, instrument, "BTC", "binance", "synthetic-futures-scope", now, now);
    QVERIFY(withoutPrices.quantityAvailable); QVERIFY(!withoutPrices.available); QVERIFY(!withoutPrices.distanceAvailable);
    QVERIFY(!projectChartFuturesPosition(position, instrument, "BTC", "binance", "synthetic-futures-scope", now - 180001, now).quantityAvailable);
    QCOMPARE(position.amount, QString("-2")); QCOMPARE(position.unrealizedPnl, QString("-50.5"));
    MarketCandlesView view(nullptr);
    view.setAsset("BTC");
    view.setPriceLines({{projected.provider, projected.accountScope, projected.asset, projected.currency,
        projected.entryPrice, projected.sourceUrl, projected.receivedAtMs, ChartPriceKind::Entry}});
    QVERIFY(view.priceLines().isEmpty());
}

void ChartWorkspaceTest::futuresRibbonUsesExistingReadsAndClearsOnLock() {
    class FuturesTransport final : public TradingTransport {
      public:
        int calls = 0;
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation));
            ++calls;
            const auto response = keefi::test::usdmResponse(request);
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override {}
    } transport;
    QTemporaryDir directory;
    Vault vault(directory.filePath("fake-chart-futures-vault"));
    QVERIFY(vault.create("Fake futures chart workspace password 2026!"));
    auto account = keefi::test::usdmAccount();
    account.id.clear();
    QVERIFY(vault.addAccount(account, {{"api_key", "FakeChartFuturesKey"}, {"secret", "FakeChartFuturesSecret"}}, StorageMode::EncryptedVault));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord&) { return &transport; };
    PortfolioController portfolio(vault, nullptr, factories);
    portfolio.setAutomaticRefresh(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(), 1, 8000);
    auto* futures = portfolio.futures(vault.accounts().first().id);
    QVERIFY(futures);
    QVERIFY(futures->instruments().isEmpty());
    const auto originalPositions = futures->positions();
    const auto originalSnapshots = portfolio.snapshots();
    class ContractTransport final : public TradingTransport {
      public:
        int calls = 0, aborts = 0;
        bool hold = false;
        QList<quint64> tokens;
        void send(quint64 token, const RequestSpec& request) override {
            const auto expected = MarketMetrics::request(MarketMetrics::Read::Contracts);
            QVERIFY(!request.authenticated);
            QCOMPARE(request.method, QByteArray("GET"));
            QCOMPARE(request.url, expected.url);
            QCOMPARE(request.headers, expected.headers);
            QVERIFY(request.body.isEmpty());
            ++calls;
            tokens.append(token);
            if (hold) return;
            const auto response = keefi::test::usdmResponse(request);
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override { ++aborts; }
    } contracts;
    const auto readsBeforeWorkspace = transport.calls;
    MarketCandles model(nullptr);
    ChartWorkspace workspace(&model, &portfolio);
    workspace.setContractTransport(&contracts);
    workspace.resize(1280, 760);
    workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    workspace.setPrivateEnabled(true);
    auto* ribbon = workspace.findChild<QLabel*>("chartWorkspaceLiquidationRibbon");
    auto* chart = workspace.findChild<MarketCandlesView*>();
    QVERIFY(ribbon);
    QVERIFY(chart);
    QTRY_VERIFY_WITH_TIMEOUT(ribbon->isVisible(), 2500);
    QVERIFY(ribbon->text().contains("80.00%")); // Fixture mark 100 and liquidation 20.
    auto* positionText = workspace.findChild<QLabel*>("chartWorkspacePositionText");
    QVERIFY(positionText);
    QVERIFY(displayedText(positionText).contains("수량 2 BTC"));
    QVERIFY(displayedText(positionText).contains("내 평균단가 100 USDT"));
    QVERIFY(displayedText(positionText).contains("평가손익 -1 USDT"));
    QVERIFY(displayedText(positionText).contains("청산가 20 USDT"));
    QVERIFY(!displayedText(positionText).contains('%')); // The provider supplies no ROE/return percentage.
    const auto profileBytes = workspace.watchlistModel()->publicState();
    for (int i = 0; i < 3; ++i) emit portfolio.changed();
    QCOMPARE(futures->positions(), originalPositions);
    QCOMPARE(portfolio.snapshots(), originalSnapshots);
    QCOMPARE(workspace.watchlistModel()->publicState(), profileBytes);
    const auto observed=futures->positions();QCOMPARE(observed.size(),1);QCOMPARE(observed.first().markPrice,QString("100"));QCOMPARE(observed.first().liquidationPrice,QString("20"));
    QVERIFY(ribbon->text().contains("binance"));
    QVERIFY(ribbon->text().contains("BOTH"));
    QVERIFY(!ribbon->text().contains("KST"));QVERIFY(portfolio.connections().first().receivedAtMs>0);
    QVERIFY(!ribbon->text().contains("https://"));QVERIFY(!futures->positions().first().symbol.isEmpty());
    QVERIFY(chart->priceLines().isEmpty());
    QCOMPARE(contracts.calls, 1);
    QCOMPARE(transport.calls, readsBeforeWorkspace);
    if (const auto out = qEnvironmentVariable("KEEFI_USABILITY_P2_RENDER_DIR"); !out.isEmpty()) {
        QVERIFY(QDir().mkpath(out)); const auto previous = KeefiTheme::instance()->mode(); const auto originalSize = workspace.size();
        workspace.resize(1800, 1000);
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode); workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
            QCoreApplication::processEvents();
            QVERIFY(workspace.grab().save(QDir(out).filePath(QString("positions-futures-%1.png").arg(mode == ThemeMode::Light ? "light" : "dark"))));
        }
        KeefiTheme::instance()->setMode(previous); workspace.resize(originalSize);
    }
    contracts.hold = true;
    workspace.setAsset("ETH");
    QTRY_COMPARE_WITH_TIMEOUT(contracts.calls, 2, 2500);
    const auto superseded = contracts.tokens.last();
    workspace.setAsset("BTC");
    QCOMPARE(contracts.aborts, 1);
    QTRY_COMPARE_WITH_TIMEOUT(contracts.calls, 3, 2500);
    const auto current = contracts.tokens.last();
    const auto metadata = keefi::test::usdmResponse(MarketMetrics::request(MarketMetrics::Read::Contracts));
    emit contracts.finished(superseded, metadata);
    QVERIFY(ribbon->isHidden()); // A reply for the previous selected asset cannot hydrate the current chart.
    emit contracts.finished(current, metadata);
    QTRY_VERIFY(ribbon->isVisible());
    workspace.setAsset("ETH");
    QTRY_COMPARE_WITH_TIMEOUT(contracts.calls, 4, 2500);
    const auto hidden = contracts.tokens.last();
    workspace.hide();
    QCOMPARE(contracts.aborts, 2);
    workspace.setAsset("BTC");
    workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QTRY_COMPARE_WITH_TIMEOUT(contracts.calls, 5, 2500);
    emit contracts.finished(hidden, metadata);
    QVERIFY(ribbon->isHidden());
    emit contracts.finished(contracts.tokens.last(), metadata);
    QTRY_VERIFY(ribbon->isVisible());
    QCOMPARE(transport.calls, readsBeforeWorkspace);
    vault.lock();
    QVERIFY(ribbon->isHidden());
    QVERIFY(ribbon->text().isEmpty());
    QVERIFY(ribbon->toolTip().isEmpty());
    emit portfolio.changed();
    QTest::qWait(1100);
    QVERIFY(ribbon->text().isEmpty());
    QVERIFY(chart->priceLines().isEmpty());
}

void ChartWorkspaceTest::hiddenPassiveFuturesObservationsUseOnlyExistingPrivateReads(){
    class FuturesTransport final:public TradingTransport {public:int calls=0;void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!isMutation(request.operation));++calls;const auto response=keefi::test::usdmResponse(request);QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});}void abort()override{}} privateTransport;
    QTemporaryDir directory;Vault vault(directory.filePath("synthetic-hidden-risk-vault"));QVERIFY(vault.create("Synthetic hidden risk password 2026!"));auto account=keefi::test::usdmAccount();account.id.clear();
    QVERIFY(vault.addAccount(account,{{"api_key","SyntheticHiddenRiskKey"},{"secret","SyntheticHiddenRiskSecret"}},StorageMode::EncryptedVault));
    PortfolioController::Factories factories;factories.prices=[](){return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &privateTransport;};
    PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(),1,8000);
    QVERIFY(portfolio.futures(vault.accounts().first().id));QVERIFY(portfolio.futures(vault.accounts().first().id)->instruments().isEmpty());const auto privateReads=privateTransport.calls;
    class Contracts final:public TradingTransport {public:int calls=0;QList<quint64> tokens;void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!request.authenticated);QCOMPARE(request.url,MarketMetrics::request(MarketMetrics::Read::Contracts).url);++calls;tokens.append(token);
        const auto response=keefi::test::usdmResponse(request);QTimer::singleShot(1,this,[this,token,response]{emit finished(token,response);});}void abort()override{}} contracts;
    ChartWorkspace workspace(nullptr,&portfolio);workspace.setAsset("ETH");workspace.setContractTransport(&contracts);workspace.setPrivateEnabled(true);
    auto* label=workspace.findChild<QLabel*>("chartWorkspacePositionText");QVERIFY(label);const auto hiddenText=displayedText(label);QVERIFY(!workspace.isVisible());
    QSignalSpy changed(&workspace,&ChartWorkspace::futuresPositionObservationsChanged);workspace.setFuturesObservationActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(contracts.calls,1,2500);QTRY_COMPARE_WITH_TIMEOUT(workspace.futuresPositionObservations().size(),1,2500);
    const auto position=workspace.futuresPositionObservations().first();QCOMPARE(position.contractSymbol,QString("BTCUSDT"));QCOMPARE(position.asset,QString("BTC"));QCOMPARE(position.positionSide,QString("BOTH"));QCOMPARE(position.currency,QString("USDT"));
    QVERIFY(!changed.isEmpty());QCOMPARE(displayedText(label),hiddenText);QCOMPARE(privateTransport.calls,privateReads);
    workspace.setAsset("SYN");QCOMPARE(workspace.futuresPositionObservations().size(),1);QCOMPARE(displayedText(label),hiddenText);
    QVERIFY(workspace.futuresPositionObservations(position.receivedAtMs+180001).isEmpty());
    workspace.setFuturesObservationActive(false);QVERIFY(workspace.futuresPositionObservations().isEmpty());workspace.setFuturesObservationActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(workspace.futuresPositionObservations().size(),1,2500);vault.lock();QVERIFY(workspace.futuresPositionObservations().isEmpty());
    emit portfolio.changed();if(!contracts.tokens.isEmpty())emit contracts.finished(contracts.tokens.last(),keefi::test::usdmResponse(MarketMetrics::request(MarketMetrics::Read::Contracts)));
    QVERIFY(workspace.futuresPositionObservations().isEmpty());QCOMPARE(privateTransport.calls,privateReads);
}
void ChartWorkspaceTest::hiddenPublicRowsWaitForShowCatchup(){
    WatchlistWorkspaceList lists;const auto id=lists.createList("합성 숨김 행");QVERIFY(lists.addIdentity(id,{"coin","upbit","KRW-SYN"}));WatchlistRowState state;state.price="100";lists.setRowState({"coin","upbit","KRW-SYN"},state);
    ChartWorkspace workspace(nullptr,nullptr);workspace.setWatchlistModel(&lists);workspace.setAsset("SYN");workspace.resize(1280,760);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QTRY_COMPARE(tree->topLevelItemCount(),1);QCOMPARE(tree->topLevelItem(0)->text(1),QString("100"));
    workspace.hide();state.price="200";lists.setRowState({"coin","upbit","KRW-SYN"},state);QCoreApplication::processEvents();QCOMPARE(tree->topLevelItem(0)->text(1),QString("100"));
    workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    const auto* headline=workspace.findChild<QLabel*>("chartCurrentPrice");QVERIFY(headline);
    QCOMPARE(tree->topLevelItem(0)->text(1),QString("100"));
    QCOMPARE(headline->text(),QString("200"));
    state.price="300";lists.setRowState({"coin","upbit","KRW-SYN"},state);
    QCOMPARE(headline->text(),QString("300"));
    QCOMPARE(tree->topLevelItem(0)->text(1),QString("100"));
    QVERIFY(test::openInstrumentPanel(workspace));
    QCOMPARE(tree->topLevelItem(0)->text(1),QString("300"));
}

void ChartWorkspaceTest::futuresPricesUseTheMatchingVerifiedUsdtAxis() {
    class FuturesTransport final : public TradingTransport {
      public:
        int calls = 0;
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!isMutation(request.operation)); ++calls;
            const auto response = keefi::test::usdmResponse(request);
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override {}
    } privateTransport;
    class ContractTransport final : public TradingTransport {
      public:
        void send(quint64 token, const RequestSpec& request) override {
            QVERIFY(!request.authenticated);
            QCOMPARE(request.url, MarketMetrics::request(MarketMetrics::Read::Contracts).url);
            const auto response = keefi::test::usdmResponse(request);
            QTimer::singleShot(1, this, [this, token, response] { emit finished(token, response); });
        }
        void abort() override {}
    } contracts;
    class CandleTransport final : public TradingTransport {
      public:
        quint64 token = 0; RequestSpec request;
        void send(quint64 t, const RequestSpec& r) override { token = t; request = r; QVERIFY(!r.authenticated); }
        void abort() override {}
    } candles;
    QTemporaryDir directory;
    Vault vault(directory.filePath("synthetic-usdt-axis-vault"));
    QVERIFY(vault.create("Synthetic USDT axis password 2026!"));
    auto account = keefi::test::usdmAccount(); account.id.clear();
    QVERIFY(vault.addAccount(account, {{"api_key", "SyntheticUsdtAxisKey"}, {"secret", "SyntheticUsdtAxisSecret"}}, StorageMode::EncryptedVault));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    factories.trading = [&](const AccountRecord&) { return &privateTransport; };
    PortfolioController portfolio(vault, nullptr, factories); portfolio.setAutomaticRefresh(true);
    QTRY_COMPARE_WITH_TIMEOUT(portfolio.snapshots().size(), 1, 8000);
    const auto readsBeforeWorkspace = privateTransport.calls;
    const auto observed = QDateTime::currentMSecsSinceEpoch();
    MarketCandles model(&candles, std::unique_ptr<PublicCandleSocket>{}, [observed] { return observed; });
    MarketInstrument instrument; instrument.symbol = "BTCUSDT"; instrument.baseAsset = "BTC"; instrument.quoteAsset = "USDT";
    instrument.market = "spot"; instrument.status = "TRADING"; instrument.tradable = true;
    model.setInstruments("binance", {instrument});
    ChartWorkspace workspace(&model, &portfolio); workspace.setContractTransport(&contracts);
    workspace.setTimeframe(60000); workspace.setIdentity("coin:binance:BTCUSDT"); workspace.resize(1280, 760); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    workspace.setPrivateEnabled(true);
    QTRY_VERIFY(candles.token);
    QCOMPARE(candles.request.url.host(), QString("api.binance.com"));
    const auto start = (observed / 60000 - 1) * 60000;
    QJsonArray rows; rows.append(QJsonArray{QString::number(start), "100", "110", "10", "101", "2", QString::number(start + 59999), "202", "10", "1", "101", "0"});
    HttpResult response; response.status = 200; response.body = QJsonDocument(rows).toJson();
    emit candles.finished(candles.token, response); QVERIFY(model.series().available);
    auto* chart = workspace.findChild<MarketCandlesView*>(); QVERIFY(chart);
    auto* ribbon = workspace.findChild<QLabel*>("chartWorkspaceLiquidationRibbon"); QVERIFY(ribbon);
    QTRY_VERIFY(ribbon->isVisible());
    QCOMPARE(chart->priceLines().size(), 2);
    QSet<ChartPriceKind> kinds;
    for (const auto& line : chart->priceLines()) { QCOMPARE(line.asset, QString("BTC")); QCOMPARE(line.currency, QString("USDT")); kinds.insert(line.kind); }
    QVERIFY(kinds.contains(ChartPriceKind::Entry)); QVERIFY(kinds.contains(ChartPriceKind::Liquidation));
    QCOMPARE(privateTransport.calls, readsBeforeWorkspace);
    vault.lock();
    QVERIFY(chart->priceLines().isEmpty()); QVERIFY(ribbon->isHidden()); QVERIFY(model.series().available);
    emit portfolio.changed(); QVERIFY(chart->priceLines().isEmpty()); QCOMPARE(privateTransport.calls, readsBeforeWorkspace);
}

// Usability defects 1 and 2: no native timeframe popup over the price; the unit sits beside the price.
void ChartWorkspaceTest::splitUsesOneLiveSocketAndReleasesPassiveRequests(){
    class Http final:public TradingTransport {
    public:QList<RequestSpec> sent;int aborts=0;void send(quint64,const RequestSpec& r)override{sent.append(r);}void abort()override{++aborts;}
    } activeHttp,passiveHttp;
    class Socket final:public PublicCandleSocket {
    public:int opened=0,aborted=0,live=0,maxLive=0;
        void open(quint64,const QUrl&)override{++opened;++live;maxLive=qMax(maxLive,live);}
        void send(quint64,const QByteArray&)override{}
        void abort()override{++aborted;live=0;}
    };
    auto socket=std::make_unique<Socket>();auto* observed=socket.get();
    MarketCandles model(&activeHttp,std::move(socket));ChartStudyFeed feed(&passiveHttp);
    feed.setVerifiedUpbitMarkets({"KRW-BTC","KRW-ETH"},true);
    ChartWorkspace view(&model,nullptr);view.indicatorPreferences()->markIntroSeen();view.setStudyFeed(&feed);
    const auto list=view.watchlistModel()->createList("두 칸");
    view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-BTC"});view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-ETH"});view.watchlistModel()->useList(list);
    view.resize(1440,900);view.show();QCoreApplication::processEvents();QVERIFY(view.setIdentity("coin:upbit:KRW-BTC"));
    const int baselineSockets=observed->opened,baselineReads=activeHttp.sent.size();QVERIFY(baselineSockets>0);
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");layout->setCurrentIndex(layout->findData(2));QCoreApplication::processEvents();
    QCOMPARE(observed->opened,baselineSockets);QCOMPARE(activeHttp.sent.size(),baselineReads);QCOMPARE(feed.demandCount(),1);
    feed.pump();QCOMPARE(passiveHttp.sent.size(),1);QCOMPARE(QUrlQuery(passiveHttp.sent.last().url).queryItemValue("market"),QString("KRW-ETH"));
    view.findChild<QPushButton*>("chartGridSelect_1")->click();QCOMPARE(model.series().identity,QString("coin:upbit:KRW-ETH"));
    QCOMPARE(observed->maxLive,1);QCOMPARE(feed.ownerCount("coin:upbit:KRW-BTC",900000),1);QCOMPARE(feed.ownerCount("coin:upbit:KRW-ETH",900000),0);
    view.hide();QCOMPARE(observed->live,0);QCOMPARE(feed.demandCount(),0);const int reads=passiveHttp.sent.size();feed.pump();QCOMPARE(passiveHttp.sent.size(),reads);
    view.show();QCoreApplication::processEvents();QCOMPARE(observed->live,1);QCOMPARE(feed.demandCount(),1);
    layout->setCurrentIndex(layout->findData(1));QCOMPARE(observed->live,1);QCOMPARE(feed.demandCount(),0);
}

namespace {
class SplitCandleTransport final : public TradingTransport {
public:
    qint64 end=0;
    int calls=0;
    void send(quint64 token,const RequestSpec& request) override {
        ++calls;
        const auto market=QUrlQuery(request.url).queryItemValue("market");
        const int value=market=="KRW-ETH"?200:100;
        QJsonArray rows;
        for(int i=180;i>=1;--i){
            const auto at=end-i*900000;
            rows.append(QJsonObject{{"market",market},{"unit","15"},{"timestamp",QString::number(at+1000)},
                {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                {"opening_price",QString::number(value)},{"high_price",QString::number(value+2)},{"low_price",QString::number(value-2)},
                {"trade_price",QString::number(value)},{"candle_acc_trade_volume","2"}});
        }
        HttpResult result;
        result.status=200;
        result.body=QJsonDocument(rows).toJson();
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
    void abort() override {}
};
}

void ChartWorkspaceTest::splitPanelSelectionPreservesLiveFollowing_data(){
    QTest::addColumn<bool>("roundTrip");
    QTest::newRow("reselect-active-panel") << false;
    QTest::newRow("return-to-live-panel") << true;
}

void ChartWorkspaceTest::splitPanelSelectionPreservesLiveFollowing(){
    QFETCH(bool,roundTrip);
    constexpr qint64 interval=900000;
    const qint64 end=QDateTime::currentMSecsSinceEpoch()/interval*interval;
    qint64 now=end+10000;
    SplitCandleTransport activeHttp,passiveHttp;
    activeHttp.end=passiveHttp.end=end;
    class Socket final : public PublicCandleSocket {
    public:
        quint64 generation=0;
        void open(quint64 next,const QUrl&) override { generation=next; }
        void send(quint64,const QByteArray&) override {}
        void abort() override {}
    };
    auto socket=std::make_unique<Socket>();
    auto* wire=socket.get();
    MarketCandles model(&activeHttp,std::move(socket),[&]{return now;});
    ChartStudyFeed feed(&passiveHttp,nullptr,nullptr,[&]{return now;});
    feed.setVerifiedUpbitMarkets({"KRW-BTC","KRW-ETH"},true);
    ChartWorkspace view(&model,nullptr);
    view.indicatorPreferences()->markIntroSeen();
    for(const auto& id:view.indicatorPreferences()->supportedIds("coin"))
        view.indicatorPreferences()->setEnabled(id,false);
    view.setStudyFeed(&feed);
    const auto list=view.watchlistModel()->createList("Live split fixture");
    view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-BTC"});
    view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-ETH"});
    view.watchlistModel()->useList(list);
    view.resize(1440,1000);
    view.show();
    QVERIFY(view.setIdentity("coin:upbit:KRW-BTC"));
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");
    auto* active=view.findChild<MarketCandlesView*>();
    auto* first=view.findChild<QPushButton*>("chartGridSelect_0");
    auto* second=view.findChild<QPushButton*>("chartGridSelect_1");
    auto* live=view.findChild<QPushButton*>("chartReturnToLive");
    QVERIFY(layout&&active&&first&&second&&live);
    layout->setCurrentIndex(layout->findData(2));
    feed.pump();
    QTRY_VERIFY(active->syncViewport().valid());
    QVERIFY(!live->isEnabled());
    if(roundTrip){
        second->click();
        QTRY_VERIFY(model.series().available);
        QCOMPARE(view.selectedChartIdentity(),QString("coin:upbit:KRW-ETH"));
    }
    first->click();
    QCOMPARE(view.selectedChartIdentity(),QString("coin:upbit:KRW-BTC"));
    QTRY_VERIFY(model.series().available);
    QVERIFY2(!live->isEnabled(),"Selecting a live panel must preserve live following with sync off");
    const auto before=active->syncViewport();
    QVERIFY(before.valid());
    QVERIFY(wire->generation!=0);
    emit wire->connected(wire->generation);
    const QJsonObject tick{{"type","candle.15m"},{"code","KRW-BTC"},{"stream_type","REALTIME"},
        {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(end,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
        {"timestamp",QString::number(now)},{"opening_price","100"},{"high_price","110"},{"low_price","90"},
        {"trade_price","101"},{"candle_acc_trade_volume","2"}};
    emit wire->received(wire->generation,QJsonDocument(tick).toJson(QJsonDocument::Compact));
    QTRY_VERIFY(model.series().forming.has_value());
    QTRY_COMPARE(active->syncViewport().toMs,before.toMs+interval);
}

void ChartWorkspaceTest::splitPanelsRememberIndependentLiveZoom_data(){
    QTest::addColumn<bool>("checkPassive");
    QTest::newRow("zoom-while-passive") << true;
    QTest::newRow("zoom-after-reactivation") << false;
}

void ChartWorkspaceTest::splitPanelsRememberIndependentLiveZoom(){
    QFETCH(bool,checkPassive);
    constexpr qint64 interval=900000;
    const qint64 end=QDateTime::currentMSecsSinceEpoch()/interval*interval;
    qint64 now=end+10000;
    SplitCandleTransport activeHttp,passiveHttp;
    activeHttp.end=passiveHttp.end=end;
    MarketCandles model(&activeHttp,{},[&]{return now;});
    ChartStudyFeed feed(&passiveHttp,nullptr,nullptr,[&]{return now;});
    feed.setVerifiedUpbitMarkets({"KRW-BTC","KRW-ETH"},true);
    ChartWorkspace view(&model,nullptr);
    view.indicatorPreferences()->markIntroSeen();
    for(const auto& id:view.indicatorPreferences()->supportedIds("coin"))
        view.indicatorPreferences()->setEnabled(id,false);
    view.setStudyFeed(&feed);
    const auto list=view.watchlistModel()->createList("Zoom split fixture");
    view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-BTC"});
    view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-ETH"});
    view.watchlistModel()->useList(list);
    view.resize(1440,1000);
    view.show();
    QVERIFY(view.setIdentity("coin:upbit:KRW-BTC"));
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");
    auto* active=view.findChild<MarketCandlesView*>();
    auto* grid=view.findChild<ChartGridView*>();
    auto* first=view.findChild<QPushButton*>("chartGridSelect_0");
    auto* second=view.findChild<QPushButton*>("chartGridSelect_1");
    auto* primary=view.findChild<QWidget*>("marketCandlesChart");
    auto* live=view.findChild<QPushButton*>("chartReturnToLive");
    QVERIFY(layout&&active&&grid&&first&&second&&primary&&live);
    layout->setCurrentIndex(layout->findData(2));
    feed.pump();
    QTRY_VERIFY(active->syncViewport().valid()&&grid->panelViewport(1).valid());
    const auto span=[](const ChartSyncViewport& range){return range.toMs-range.fromMs;};
    const auto initial=span(active->syncViewport());
    QTest::keyClick(primary,Qt::Key_Plus);
    const auto firstZoom=span(active->syncViewport());
    QCOMPARE(firstZoom,initial-10*interval);
    QVERIFY(!live->isEnabled());

    second->click();
    now+=2000;
    feed.pump();
    QTRY_VERIFY(model.series().available&&grid->panelViewport(0).valid());
    QCOMPARE(view.selectedChartIdentity(),QString("coin:upbit:KRW-ETH"));
    if(checkPassive){
        QCOMPARE(span(grid->panelViewport(0)),firstZoom);
    }else{
        QTest::keyClick(primary,Qt::Key_Minus);
        const auto secondZoom=span(active->syncViewport());
        QVERIFY(secondZoom>firstZoom);
        first->click();
        QTRY_VERIFY(model.series().available);
        QCOMPARE(view.selectedChartIdentity(),QString("coin:upbit:KRW-BTC"));
        QCOMPARE(span(active->syncViewport()),firstZoom);
        QVERIFY(!live->isEnabled());
        second->click();
        QTRY_VERIFY(model.series().available);
        QCOMPARE(span(active->syncViewport()),secondZoom);
    }
    QVERIFY(!live->isEnabled());
}

void ChartWorkspaceTest::splitSyncConnectsRealActiveAndPassiveInputsWithoutLoops(){
    const qint64 now=QDateTime::currentMSecsSinceEpoch(),interval=900000,end=now/interval*interval;
    SplitCandleTransport activeHttp,passiveHttp;activeHttp.end=passiveHttp.end=end;
    MarketCandles model(&activeHttp,std::unique_ptr<PublicCandleSocket>{},[&]{return now;});ChartStudyFeed feed(&passiveHttp,nullptr,nullptr,[&]{return now;});
    feed.setVerifiedUpbitMarkets({"KRW-BTC","KRW-ETH"},true);ChartWorkspace view(&model,nullptr);view.indicatorPreferences()->markIntroSeen();
    for(const auto& id:view.indicatorPreferences()->supportedIds("coin"))view.indicatorPreferences()->setEnabled(id,false);
    view.setStudyFeed(&feed);const auto list=view.watchlistModel()->createList("Sync fixture");
    view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-BTC"});view.watchlistModel()->addIdentity(list,{"coin","upbit","KRW-ETH"});view.watchlistModel()->useList(list);
    view.resize(1440,1000);view.show();QVERIFY(view.setIdentity("coin:upbit:KRW-BTC"));
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");auto* menu=view.findChild<QToolButton*>("chartSyncMenuButton");QVERIFY(layout&&menu);QVERIFY(!menu->isEnabled());
    auto* grid=view.findChild<ChartGridView*>();auto* active=view.findChild<MarketCandlesView*>();QVERIFY(grid&&active);
    for(const char* name:{"Symbol","Interval","Crosshair","Time","DateRange"}){auto* action=view.findChild<QAction*>("chartSync_"+QString(name));QVERIFY(action);QVERIFY(!action->isChecked());}
    layout->setCurrentIndex(layout->findData(2));QVERIFY(menu->isEnabled());feed.pump();
    QTRY_VERIFY(active->syncViewport().valid());QTRY_VERIFY(grid->panelViewport(1).valid());
    auto* primary=active->findChild<QWidget*>("marketCandlesChart");auto* passive=view.findChild<QWidget*>("chartGridCanvas_1");QVERIFY(primary&&passive);
    auto* dateRange = view.findChild<QAction*>("chartSync_DateRange");
    auto* time = view.findChild<QAction*>("chartSync_Time");
    QVERIFY(dateRange && time);
    dateRange->setChecked(true);
    QCOMPARE(time->text(), QStringLiteral("Time"));
    time->setChecked(true);
    QCOMPARE(time->text(), QStringLiteral("Time · Date Range 우선"));
    dateRange->setChecked(false);
    QCOMPARE(time->text(), QStringLiteral("Time"));
    dateRange->setChecked(true);
    QCOMPARE(time->text(), QStringLiteral("Time · Date Range 우선"));
    time->setChecked(false);
    QCOMPARE(time->text(), QStringLiteral("Time"));
    time->setChecked(true);
    QCOMPARE(view.findChild<QPushButton*>("chartGridSelect_0")->height(), 28);
    QSignalSpy outbound(active,&MarketCandlesView::userViewportNavigated);
    primary->setFocus();QTest::keyClick(primary,Qt::Key_Left);
    QCOMPARE(outbound.size(),1);QCOMPARE(grid->panelViewport(1),active->syncViewport());
    const auto applied=grid->property("syncViewportApplyCount").toULongLong();QCOMPARE(applied,quint64(1));
    passive->setFocus();QTest::keyClick(passive,Qt::Key_Left);
    QCOMPARE(active->syncViewport(),grid->panelViewport(1));QCOMPARE(outbound.size(),1);
    QCOMPARE(grid->property("syncViewportApplyCount").toULongLong(),applied+1);
    view.findChild<QAction*>("chartSync_Crosshair")->setChecked(true);
    const int activeCalls=activeHttp.calls,passiveCalls=passiveHttp.calls;
    const auto middle=[](QWidget* chart){return QPoint(qRound(chart->property("viewportPlotLeft").toDouble()+chart->property("viewportPlotWidth").toDouble()*.5),
                                                       qRound(chart->property("viewportPlotTop").toDouble()+30));};
    QTest::mouseMove(primary,middle(primary));QTRY_VERIFY(passive->property("syncCrosshairMs").toLongLong()>0);QCOMPARE(passive->property("syncCrosshairPrice").toDouble(),200.0);
    QTest::mouseMove(passive,middle(passive));QTRY_VERIFY(primary->property("syncCrosshairMs").toLongLong()>0);QCOMPARE(primary->property("syncCrosshairPrice").toDouble(),100.0);
    QEvent leave(QEvent::Leave);QApplication::sendEvent(passive,&leave);QCOMPARE(primary->property("syncCrosshairMs").toLongLong(),0);
    QCOMPARE(activeHttp.calls,activeCalls);QCOMPARE(passiveHttp.calls,passiveCalls);QCOMPARE(outbound.size(),1);
    layout->setCurrentIndex(layout->findData(1));QVERIFY(!menu->isEnabled());QCOMPARE(primary->property("syncCrosshairMs").toLongLong(),0);
}
void ChartWorkspaceTest::splitActivePanelKeepsFullIndicators(){
    ChartWorkspace view(nullptr,nullptr);view.indicatorPreferences()->markIntroSeen();view.resize(1440,900);view.show();
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");QVERIFY(layout);layout->setCurrentIndex(layout->findData(2));
    auto* active=view.findChild<MarketCandlesView*>();QVERIFY(active);QVERIFY2(active->isVisible(),"The active split panel must retain the full indicator chart");
    auto* toolbar=view.findChild<QWidget*>("chartToolbar");QVERIFY(toolbar);
    auto* picker=toolbar->findChild<QPushButton*>("chartStudyPicker");QVERIFY(picker);QVERIFY(picker->isVisible());QCoreApplication::processEvents();
    auto* first=view.findChild<QPushButton*>("chartGridSelect_0");auto* second=view.findChild<QPushButton*>("chartGridSelect_1");QVERIFY(first&&second);
    QVERIFY(first->text().contains("활성"));
    QVERIFY(first->parentWidget()->rect().contains(QRect(first->parentWidget()->mapFromGlobal(active->mapToGlobal(QPoint{})),active->size())));
    second->click();QVERIFY(second->text().contains("활성"));QVERIFY(!first->text().contains("활성"));
    QVERIFY(active->isVisible());
    const auto capture=qEnvironmentVariable("KEEFI_CHART_WORKSPACE_CAPTURE_DIR");
    if(!capture.isEmpty()){QCoreApplication::processEvents();QVERIFY(view.grab().save(capture+"/synthetic-two-panel.png"));}
    layout->setCurrentIndex(layout->findData(1));QVERIFY(active->isVisible());
}

void ChartWorkspaceTest::emptySplitSlotKeepsItsChosenTimeframe(){
    class Http final:public TradingTransport{public:void send(quint64,const RequestSpec&)override{}void abort()override{}} http;
    MarketCandles model(&http,std::unique_ptr<PublicCandleSocket>{});
    ChartStudyFeed feed(nullptr);ChartWorkspace view(&model,nullptr);view.indicatorPreferences()->markIntroSeen();view.setStudyFeed(&feed);view.resize(1440,900);view.show();
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");QVERIFY(layout);layout->setCurrentIndex(layout->findData(2));
    auto* period=view.findChild<QComboBox*>("chartGridTimeframe_1");QVERIFY(period);period->setCurrentIndex(period->findData(86400000));
    QVERIFY2(model.series().identity.isEmpty(),"An empty split slot must not show the previous panel's market");
    QVERIFY(view.setIdentity("coin:upbit:KRW-SOL"));QCOMPARE(view.timeframeMs(),qint64(86400000));
    QCOMPARE(period->currentData().toLongLong(),qint64(86400000));
}

void ChartWorkspaceTest::splitChartsKeepIndependentSelectionsAndReleaseTheirFeed(){
    ChartStudyFeed feed(nullptr);ChartWorkspace view(nullptr,nullptr);view.indicatorPreferences()->markIntroSeen();view.setStudyFeed(&feed);
    auto* lists=view.watchlistModel();const auto list=lists->createList("분할 관심");
    for(const QString coin:{"BTC","ETH","SOL","XRP"})QVERIFY(lists->addIdentity(list,{"coin","upbit","KRW-"+coin}));lists->useList(list);
    view.resize(1440,900);view.show();QCoreApplication::processEvents();
    auto* layout=view.findChild<QComboBox*>("chartLayoutSelector");QVERIFY2(layout,"Split charts need a visible entry point");
    layout->setCurrentIndex(layout->findData(2));
    auto* first=view.findChild<QPushButton*>("chartGridSelect_0");auto* second=view.findChild<QPushButton*>("chartGridSelect_1");QVERIFY(first);QVERIFY(second);
    QVERIFY(first->isVisible());QTRY_COMPARE(feed.demandCount(),1);
    const auto secondBefore=second->property("chartIdentity").toString();
    first->click();QVERIFY(view.setIdentity("coin:upbit:KRW-DOGE"));
    QCOMPARE(first->property("chartIdentity").toString(),QString("coin:upbit:KRW-DOGE"));
    QCOMPARE(second->property("chartIdentity").toString(),secondBefore);
    auto* period=view.findChild<QComboBox*>("chartGridTimeframe_0");QVERIFY(period);period->setCurrentIndex(period->findData(3600000));
    QCOMPARE(view.timeframeMs(),qint64(3600000));
    second->click();QCOMPARE(view.selectedChartIdentity(),secondBefore);QCOMPARE(view.timeframeMs(),qint64(900000));
    QVERIFY(layout->findData(4)<0); // Four panels require a separate measured decision.
    layout->setCurrentIndex(layout->findData(1));
    layout->setCurrentIndex(layout->findData(2));
    QCOMPARE(first->property("chartIdentity").toString(),QString("coin:upbit:KRW-DOGE"));
    QCOMPARE(second->property("chartIdentity").toString(),secondBefore);
    QCOMPARE(view.selectedChartIdentity(),secondBefore);QCOMPARE(view.timeframeMs(),qint64(900000));
    view.hide();QTRY_COMPARE(feed.demandCount(),0);
    view.show();QTRY_COMPARE(feed.demandCount(),1);
    layout->setCurrentIndex(layout->findData(1));QTRY_COMPARE(feed.demandCount(),0);
}

void ChartWorkspaceTest::optionalWatchTrendTableReleasesItsThirtyDemands(){
    ChartStudyFeed feed(nullptr);ChartWorkspace view(nullptr,nullptr);view.indicatorPreferences()->markIntroSeen();view.setStudyFeed(&feed);auto* lists=view.watchlistModel();const auto id=lists->createList("합성 추세 관심");
    for(int i=0;i<11;++i)QVERIFY(lists->addIdentity(id,{"coin","upbit","KRW-SYN"+QString::number(i)}));lists->useList(id);
    view.resize(1440,790);view.show(); selectSavedScopeAndExpandDetails(view);
    QVERIFY(test::openInstrumentPanel(view));
    view.findChild<QComboBox*>("chartWorkspaceListSelector")->setCurrentIndex(view.findChild<QComboBox*>("chartWorkspaceListSelector")->findData(id));QCoreApplication::processEvents();
    auto* toggle=view.findChild<QPushButton*>("chartWatchTrendToggle");QVERIFY(toggle);QVERIFY(!toggle->isChecked());QCOMPARE(feed.demandCount(),0);
    toggle->click();QCOMPARE(feed.demandCount(),30);auto* rows=view.findChild<QTreeWidget*>("chartWatchTrendRows");QVERIFY(rows);QCOMPARE(rows->topLevelItemCount(),10);
    rows->itemClicked(rows->topLevelItem(1),2);QCOMPARE(view.selectedChartIdentity(),QString("coin:upbit:KRW-SYN1"));QCOMPARE(view.timeframeMs(),qint64(14400000));
    view.hide();QCOMPARE(feed.demandCount(),0);view.show(); selectSavedScopeAndExpandDetails(view);
    QVERIFY(test::openInstrumentPanel(view));
    QTRY_COMPARE(feed.demandCount(),30);toggle->click();QCOMPARE(feed.demandCount(),0);
}
void ChartWorkspaceTest::privateClearRemovesStockSupplementalButKeepsPublicCoinStudies(){
    ChartWorkspace view(nullptr,nullptr);view.indicatorPreferences()->markIntroSeen();view.indicatorPreferences()->setEnabled("trend",true);view.resize(1440,790);view.show(); selectSavedScopeAndExpandDetails(view);
    const WatchlistIdentity stock{"stock","KR","005930"};view.watchlistModel()->setVerifiedInstruments({stock});view.setIdentity(stock.key());auto* chart=view.findChild<MarketCandlesView*>();QVERIFY(chart);
    QList<ExtendedTimedDirection> higher;for(qint64 interval:{3600000,14400000,86400000}){ExtendedTimedDirection row;row.intervalMs=interval;row.completesAtMs=100;row.knownAtMs=101;row.state={true,IndicatorDirection::Up,{}};higher.append(row);}
    const auto ready=[&]{for(const auto& state:chart->indicatorStates())if(state.id=="trend")return state.requiredReady;return false;};
    chart->setSupplementalStudies(higher,{},{});QVERIFY(ready());view.clearPrivate();QVERIFY2(!ready(),"Broker-derived higher timeframes must disappear synchronously on vault clear");
    view.setIdentity("coin:upbit:KRW-BTC");chart->setSupplementalStudies(higher,{},{});QVERIFY(ready());view.clearPrivate();QVERIFY(ready());
}
void ChartWorkspaceTest::scorecardSelectorDoesNotBorrowOtherStudyResults(){
    ChartWorkspace view(nullptr,nullptr);view.indicatorPreferences()->markIntroSeen();view.resize(1440,790);view.show(); selectSavedScopeAndExpandDetails(view);
    view.findChild<QPushButton*>("chartIndicatorScoreToggle")->click();auto* selector=view.findChild<QComboBox*>("chartStudyScorecardSelector");QVERIFY2(selector,"Each defined study needs its own scorecard; results must not silently reuse pullback outcomes");
    for(const auto* id:{"pullback","whales","breakout","ma","bb","rsi","macd"}){const int index=selector->findData(id);QVERIFY(index>=0);selector->setCurrentIndex(index);const auto text=view.findChild<QLabel*>("chartStudyScorecard")->text();QVERIFY(!text.contains('%'));}
    QVERIFY(selector->height()>=44);
}
void ChartWorkspaceTest::currentStatusFoldsScorecardAndExplainsAllOff(){
    ChartWorkspace view(nullptr,nullptr);view.indicatorPreferences()->markIntroSeen();view.resize(1440,790);view.show(); selectSavedScopeAndExpandDetails(view);QCoreApplication::processEvents();
    auto* summary=view.findChild<QLabel*>("chartIndicatorSummary");QVERIFY2(summary,"The right panel starts with current indicator status");
    auto* toggle=view.findChild<QPushButton*>("chartIndicatorScoreToggle");QVERIFY(toggle);QVERIFY(toggle->height()>=44);
    QVERIFY(summary->isVisible());
    for(const auto& id:view.indicatorPreferences()->supportedIds("coin"))view.indicatorPreferences()->setEnabled(id,false);
    QVERIFY(summary->text().contains(QStringLiteral("현재 방향에 반영할 지표 없음")));QVERIFY(!summary->text().contains("0/0"));
    const auto before=view.findChild<QWidget*>("chartScorePanel")->geometry();toggle->click();QCoreApplication::processEvents();
    QCOMPARE(view.findChild<QWidget*>("chartScorePanel")->geometry(),before);QVERIFY(!summary->isVisible());
    auto* score=view.findChild<QLabel*>("chartStudyScorecard");QVERIFY(score);QVERIFY(!score->text().contains("%"));
    toggle->click();QVERIFY(summary->isVisible());
}
void ChartWorkspaceTest::currentStatusPreservesAccessibleRowsDuringPublicUpdates(){
    class PublicTransport final : public TradingTransport {
      public:
        quint64 token=0;
        void send(quint64 value,const RequestSpec& request) override {QVERIFY(MarketCandles::isRequest(request));token=value;}
        void abort() override {token=0;}
    } transport;
    constexpr qint64 observed=1728000000000+250*60000+10000;
    MarketCandles model(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return observed;});
    ChartWorkspace workspace(&model,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.indicatorPreferences()->setEnabled("rsi",true);
    workspace.setTimeframe(60000);workspace.setAsset("SYN");workspace.resize(1440,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    auto* rows=workspace.findChild<QTreeWidget*>("chartIndicatorRows");auto* chart=workspace.findChild<MarketCandlesView*>();
    QVERIFY(rows&&chart);QVERIFY(transport.token);QCOMPARE(rows->topLevelItemCount(),4);
    QMap<QString,QTreeWidgetItem*> originalItems;QMap<QString,QPersistentModelIndex> originalIndexes;
    for(int i=0;i<rows->topLevelItemCount();++i){
        auto* item=rows->topLevelItem(i);const auto label=item->data(0,Qt::UserRole).toString();
        originalItems.insert(label,item);originalIndexes.insert(label,rows->model()->index(i,0));
    }
    const QString cloudLabel=QStringLiteral("Trend Cloud"),removableLabel=QStringLiteral("RSI");
    QVERIFY(originalItems.contains(cloudLabel));QVERIFY(originalItems.contains(removableLabel));
    auto* cloud=originalItems.value(cloudLabel);rows->setCurrentItem(cloud);const auto before=cloud->text(0);
    QSignalSpy resets(rows->model(),&QAbstractItemModel::modelReset);
    QSignalSpy removed(rows->model(),&QAbstractItemModel::rowsRemoved);
    QSignalSpy inserted(rows->model(),&QAbstractItemModel::rowsInserted);
    QSignalSpy selection(rows,&QTreeWidget::currentItemChanged);
    QJsonArray candles;const auto end=observed/60000*60000;
    for(int i=0;i<40;++i){
        const auto at=end-(40-i)*60000;const auto price=QString::number(100+i);
        candles.append(QJsonObject{{"market","KRW-SYN"},{"unit","1"},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(at+1000)},{"opening_price",price},{"trade_price",price},
            {"high_price",QString::number(101+i)},{"low_price",QString::number(99+i)},{"candle_acc_trade_volume","1"}});
    }
    HttpResult response;response.status=200;response.body=QJsonDocument(candles).toJson();emit transport.finished(transport.token,response);
    QTRY_VERIFY(!chart->chartStudies().cloud.isEmpty());
    // A quote/study refresh changes text, not the accessible identity of every row.
    for(auto it=originalIndexes.cbegin();it!=originalIndexes.cend();++it){
        QVERIFY2(it.value().isValid(),qPrintable("Public updates invalidated the current row: "+it.key()));
        QCOMPARE(rows->topLevelItem(it.value().row()),originalItems.value(it.key()));
    }
    QCOMPARE(resets.size(),0);QCOMPARE(removed.size(),0);QCOMPARE(inserted.size(),0);QCOMPARE(selection.size(),0);
    QCOMPARE(rows->currentItem(),cloud);QVERIFY(cloud->isSelected());QVERIFY(cloud->text(0)!=before);
    QVERIFY(cloud->text(0).contains(QStringLiteral("빠른선 위")));
    QVERIFY(workspace.indicatorPreferences()->setVisible("cloud",false));
    QCOMPARE(rows->currentItem(),cloud);QVERIFY(cloud->text(0).contains(QStringLiteral("숨김")));
    QVERIFY(originalIndexes.value(cloudLabel).isValid());QCOMPARE(resets.size(),0);QCOMPARE(removed.size(),0);QCOMPARE(inserted.size(),0);
    QVERIFY(workspace.indicatorPreferences()->setEnabled("rsi",false));
    QCOMPARE(rows->topLevelItemCount(),3);QVERIFY(!originalIndexes.value(removableLabel).isValid());
    QCOMPARE(removed.size(),1);QCOMPARE(inserted.size(),0);QCOMPARE(resets.size(),0);
    for(auto it=originalIndexes.cbegin();it!=originalIndexes.cend();++it)if(it.key()!=removableLabel){
        QVERIFY(it.value().isValid());QCOMPARE(rows->topLevelItem(it.value().row()),originalItems.value(it.key()));
    }
    QVERIFY(workspace.indicatorPreferences()->setEnabled("rsi",true));
    QCOMPARE(rows->topLevelItemCount(),4);QCOMPARE(inserted.size(),1);QCOMPARE(removed.size(),1);QCOMPARE(resets.size(),0);
    QCOMPARE(rows->currentItem(),cloud);QVERIFY(cloud->isSelected());QCOMPARE(selection.size(),0);
    QStringList labels;for(int i=0;i<rows->topLevelItemCount();++i)labels.append(rows->topLevelItem(i)->data(0,Qt::UserRole).toString());
    QCOMPARE(labels.count(removableLabel),1);QCOMPARE(labels.indexOf(cloudLabel),originalIndexes.value(cloudLabel).row());
    // The new-profile default is enabled, so exercise both changes from that state.
    for(bool enabled:{false,true}){QVERIFY(workspace.indicatorPreferences()->setEnabled("similarity",enabled));QCOMPARE(rows->topLevelItemCount(),4);QCOMPARE(inserted.size(),1);QCOMPARE(removed.size(),1);QCOMPARE(resets.size(),0);QVERIFY(originalIndexes.value(cloudLabel).isValid());}
}
void ChartWorkspaceTest::timeframeTextButtonsDriveTheHiddenSelectorAndPriceUnitStaysOnItsLine(){
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    auto* selector=workspace.findChild<QComboBox*>("chartWorkspaceTimeframe");QVERIFY(selector);QVERIFY(selector->isHidden());
    auto* bar=workspace.findChild<QWidget*>("chartWorkspaceTimeframeButtons");QVERIFY(bar);QVERIFY(bar->isVisible());
    QCOMPARE(bar->findChildren<QPushButton*>().size(),selector->count());
    auto* fifteen=bar->findChild<QPushButton*>("chartWorkspaceTimeframe_900000");QVERIFY(fifteen);QVERIFY(fifteen->isChecked());
    auto* hour=bar->findChild<QPushButton*>("chartWorkspaceTimeframe_3600000");QVERIFY(hour);hour->click();
    QCOMPARE(workspace.timeframeMs(),qint64(3600000));QVERIFY(hour->isChecked());QVERIFY(!fifteen->isChecked());
    workspace.setTimeframe(86400000);QVERIFY(bar->findChild<QPushButton*>("chartWorkspaceTimeframe_86400000")->isChecked());QVERIFY(!hour->isChecked());
    auto* action=QAccessible::queryAccessibleInterface(fifteen)->actionInterface();QVERIFY(action);
    action->doAction(QAccessibleActionInterface::toggleAction());QCOMPARE(workspace.timeframeMs(),qint64(900000));
    action->doAction(QAccessibleActionInterface::toggleAction());QVERIFY(fifteen->isChecked());QCOMPARE(workspace.timeframeMs(),qint64(900000));
    auto* price=workspace.findChild<QLabel*>("chartCurrentPrice");auto* unit=workspace.findChild<QLabel*>("chartCurrentPriceUnit");QVERIFY(price&&unit);
    price->setText("742.88");unit->setText("USDT");QCoreApplication::processEvents();
    const auto priceBox=QRect(price->mapTo(&workspace,QPoint{}),price->size()),unitBox=QRect(unit->mapTo(&workspace,QPoint{}),unit->size());
    QVERIFY2(unitBox.left()>=priceBox.right()-1,"The unit follows the price on the same line");
    QVERIFY2(unitBox.top()<priceBox.bottom()&&unitBox.bottom()>priceBox.top(),"The unit shares the price's row, not the line below");
}
void ChartWorkspaceTest::programmaticSelectionSynchronizesVisibleRows(){
    WatchlistWorkspaceList lists;const auto id=lists.createList("공개 종목");
    QVERIFY(lists.addIdentity(id,{"coin","upbit","KRW-BTC"}));
    QVERIFY(lists.addIdentity(id,{"coin","upbit","KRW-ETH"}));
    ChartWorkspace workspace(nullptr,nullptr);workspace.setWatchlistModel(&lists);workspace.resize(1280,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* rows=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(rows);
    rows->setCurrentItem(rows->topLevelItem(0));
    workspace.setIdentity("coin:upbit:KRW-ETH");
    QCOMPARE(workspace.selectedIdentity(),QString("coin:upbit:KRW-ETH"));
    QCOMPARE(rows->selectedItems().size(),1);
    workspace.setIdentity("coin:upbit:KRW-SOL");
    QVERIFY(rows->selectedItems().isEmpty());
    QVERIFY(workspace.selectedIdentity().isEmpty());
}

void ChartWorkspaceTest::discoveryAndReorderControlsAreVisible(){
    WatchlistWorkspaceList lists;const auto id=lists.createList("공개 종목");
    QVERIFY(lists.addIdentity(id,{"coin","upbit","KRW-BTC"}));
    QVERIFY(lists.addIdentity(id,{"coin","upbit","KRW-ETH"}));
    ChartWorkspace workspace(nullptr,nullptr);workspace.setWatchlistModel(&lists);workspace.resize(1440,900);workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* find=workspace.findChild<QAction*>("chartListAddIdentity");QVERIFY(find);
    QVERIFY(workspace.findChild<QLineEdit*>("chartWorkspaceAsset")->isVisible());
    QVERIFY(!workspace.findChild<QPushButton*>("chartDiscoverInstruments"));
    auto* down=workspace.findChild<QAction*>("chartListMoveDown");QVERIFY(down);
    QVERIFY(!workspace.findChild<QPushButton*>("chartVisibleMoveDown"));
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");tree->setCurrentItem(tree->topLevelItem(0));
    down->trigger();QCOMPARE(lists.userList(id).entries.first().identity.code,QString("KRW-ETH"));
    WatchlistWorkspaceList restored;QVERIFY(restored.restorePublicState(lists.publicState()));
    QCOMPARE(restored.userList(id).entries.first().identity.code,QString("KRW-ETH"));
    find->trigger();auto* picker=workspace.findChild<QDialog*>("chartInstrumentPicker");QVERIFY(picker);QVERIFY(picker->isVisible());picker->close();
}

namespace {
class DiscoveryTransport final : public TradingTransport {
  public:
    QList<quint64> tokens;
    QList<RequestSpec> requests;
    void send(quint64 token, const RequestSpec& request) override { tokens.append(token); requests.append(request); }
    void abort() override {}
    void reply(int index, const QByteArray& body) {
        HttpResult response; response.status = 200; response.body = body;
        emit finished(tokens.at(index), response);
    }
};
QByteArray discoveryUpbitMarkets() {
    return R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin","market_event":{"warning":false,"caution":{}}}])";
}
QByteArray discoveryBinanceMarkets() {
    return R"({"symbols":[{"symbol":"HYPEUSDT","baseAsset":"HYPE","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})";
}
}

void ChartWorkspaceTest::splitSelectionKeepsVisibleMarketAndTimeframeConsistentAfterCatalogRemoval_data(){
    QTest::addColumn<bool>("fallbackAvailable");
    QTest::newRow("removed-market-keeps-previous-chart")<<false;
    QTest::newRow("removed-market-uses-verified-fallback")<<true;
}
void ChartWorkspaceTest::splitSelectionKeepsVisibleMarketAndTimeframeConsistentAfterCatalogRemoval(){
    QFETCH(bool,fallbackAvailable);
    qint64 catalogClock=1000;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    DiscoveryTransport upbitHttp,binanceHttp,candleHttp,studyHttp;
    PublicMarketCatalog upbit(&upbitHttp,nullptr,[&]{return catalogClock;});
    ChartInstrumentCatalog binance(&binanceHttp);
    upbit.refresh();upbitHttp.reply(0,R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin"},{"market":"KRW-ETH","korean_name":"이더리움","english_name":"Ethereum"}])");
    catalogClock+=400;QTRY_COMPARE(upbitHttp.requests.size(),2);
    QJsonArray quotes;
    for(const auto& market:{QString("KRW-BTC"),QString("KRW-ETH")})quotes.append(QJsonObject{
        {"market",market},{"trade_price","100"},{"prev_closing_price","100"},{"signed_change_rate","0"},{"timestamp",QString::number(now)}});
    upbitHttp.reply(1,QJsonDocument(quotes).toJson());catalogClock+=400;QTRY_VERIFY(!upbit.busy());
    binance.ensure("binance");
    binanceHttp.reply(0,fallbackAvailable?
        QByteArray(R"({"symbols":[{"symbol":"ETHUSDT","baseAsset":"ETH","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})"):
        discoveryBinanceMarkets());
    QVERIFY(!binance.instruments("binance").isEmpty());
    MarketCandles model(&candleHttp,std::unique_ptr<PublicCandleSocket>{},[now]{return now;});
    ChartStudyFeed feed(&studyHttp,&binance);
    ChartWorkspace workspace(&model,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    auto* lists=workspace.watchlistModel();const auto list=lists->createList("합성 두 시장");
    QVERIFY(lists->addIdentity(list,{"coin","upbit","KRW-BTC"}));
    QVERIFY(lists->addIdentity(list,{"coin","upbit","KRW-ETH"}));lists->useList(list);
    workspace.setPublicNameSources(&upbit,nullptr,&binance);workspace.setStudyFeed(&feed);
    workspace.setTimeframe(60000);workspace.resize(1440,900);workspace.show();selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    QVERIFY(workspace.setIdentity("coin:upbit:KRW-BTC"));
    auto* layout=workspace.findChild<QComboBox*>("chartLayoutSelector");QVERIFY(layout);
    layout->setCurrentIndex(layout->findData(2));QCoreApplication::processEvents();
    auto* first=workspace.findChild<QPushButton*>("chartGridSelect_0");
    auto* second=workspace.findChild<QPushButton*>("chartGridSelect_1");
    auto* firstPeriod=workspace.findChild<QComboBox*>("chartGridTimeframe_0");
    auto* secondPeriod=workspace.findChild<QComboBox*>("chartGridTimeframe_1");
    auto* chart=workspace.findChild<QWidget*>("marketCandlesChart");
    auto* reason=workspace.findChild<QLabel*>("chartWorkspaceListStatus");
    QVERIFY(first&&second&&firstPeriod&&secondPeriod&&chart&&reason);
    QVERIFY(first->isChecked());QVERIFY(second->isVisible());
    QVERIFY(first->text().contains("KRW-BTC"));QVERIFY(second->text().contains("KRW-ETH"));
    QCOMPARE(firstPeriod->currentData().toLongLong(),qint64(60000));
    QCOMPARE(secondPeriod->currentData().toLongLong(),qint64(900000));
    const auto upbitCandle=[now]{
        const auto start=(now/60000-1)*60000;
        return QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"unit","1"},
            {"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(start,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(start+1000)},{"opening_price","100"},{"high_price","110"},
            {"low_price","90"},{"trade_price","105"},{"candle_acc_trade_volume","2"}}}).toJson();
    };
    QTRY_VERIFY(!candleHttp.requests.isEmpty());
    QCOMPARE(QUrlQuery(candleHttp.requests.last().url).queryItemValue("market"),QString("KRW-BTC"));
    candleHttp.reply(candleHttp.requests.size()-1,upbitCandle());QVERIFY(model.series().available);
    QCOMPARE(chart->property("displayCompletedCount").toInt(),1);
    // Official metadata removes only the passive market. The active BTC has
    // fresh, valid candles again before the user selects that removed market.
    catalogClock+=300001;upbit.refresh();
    QTRY_COMPARE(upbitHttp.requests.last().url.path(),QString("/v1/market/all"));
    upbitHttp.reply(upbitHttp.requests.size()-1,discoveryUpbitMarkets());
    QCOMPARE(upbit.verifiedMarkets(),QStringList{"KRW-BTC"});
    QTRY_COMPARE(QUrlQuery(candleHttp.requests.last().url).queryItemValue("market"),QString("KRW-BTC"));
    candleHttp.reply(candleHttp.requests.size()-1,upbitCandle());QVERIFY(model.series().available);
    const auto previousCalls=candleHttp.requests.size();
    QTest::mouseClick(second,Qt::LeftButton);QCoreApplication::processEvents();
    if(!fallbackAvailable){
        QCOMPARE(workspace.selectedChartIdentity(),QString("coin:upbit:KRW-BTC"));
        QCOMPARE(model.series().identity,QString("coin:upbit:KRW-BTC"));QVERIFY(model.series().available);
        QCOMPARE(workspace.timeframeMs(),qint64(60000));
        QCOMPARE(model.series().intervalMs,qint64(60000));
        QCOMPARE(candleHttp.requests.size(),previousCalls);
        QVERIFY2(first->isChecked()&&!second->isChecked(),"A rejected market must leave the previous chart's panel visibly active");
        QVERIFY(first->text().contains("활성")&&first->text().contains("KRW-BTC"));
        QVERIFY(reason->isVisible());QVERIFY2(reason->text().contains("지원"),qPrintable(reason->text()));
    }else{
        QCOMPARE(workspace.selectedChartIdentity(),QString("coin:binance:ETHUSDT"));
        QCOMPARE(candleHttp.requests.size(),previousCalls+1); // One request at the requested panel interval.
        QCOMPARE(model.series().identity,QString("coin:binance:ETHUSDT"));
        QCOMPARE(workspace.timeframeMs(),qint64(900000));
        QCOMPARE(model.series().intervalMs,qint64(900000));
        QVERIFY(second->isChecked()&&!first->isChecked());
        QVERIFY2(second->text().contains("ETHUSDT")&&second->text().contains("binance"),
            qPrintable("The active panel must name the market actually shown: "+second->text()));
        QVERIFY(!second->text().contains("KRW-ETH"));
        QCOMPARE(QUrlQuery(candleHttp.requests.last().url).queryItemValue("symbol"),QString("ETHUSDT"));
        QCOMPARE(QUrlQuery(candleHttp.requests.last().url).queryItemValue("interval"),QString("15m"));
        const auto start=(now/900000-1)*900000;
        QJsonArray rows;rows.append(QJsonArray{QString::number(start),"200","220","180","205","2",QString::number(start+899999),"410","10","1","205","0"});
        QCOMPARE(rows.size(),1);QVERIFY(rows.first().isArray());QCOMPARE(rows.first().toArray().size(),12);
        HttpResult response;response.status=200;response.body=QJsonDocument(rows).toJson();
        const auto parsed=MarketCandles::parse("coin:binance:ETHUSDT",900000,response,now,now,binance.instruments("binance").first());
        QVERIFY2(parsed.available,qPrintable(parsed.reason));QCOMPARE(parsed.candles.size(),1);
        candleHttp.reply(candleHttp.requests.size()-1,response.body);
        QVERIFY2(model.series().available,qPrintable(model.series().reason));QCOMPARE(chart->property("displayCompletedCount").toInt(),1);
        QVERIFY(reason->isVisible());QVERIFY2(reason->text().contains("바이낸스"),qPrintable(reason->text()));
    }
    QCOMPARE(firstPeriod->currentData().toLongLong(),qint64(60000));
    QCOMPARE(secondPeriod->currentData().toLongLong(),qint64(900000));
    QVERIFY(chart->isVisible());
}

void ChartWorkspaceTest::unsupportedSavedCoinRoutesAndRepairsSelectionBeforeCandleRequest() {
    DiscoveryTransport upbitHttp, binanceHttp, candleHttp;
    PublicMarketCatalog upbit(&upbitHttp); ChartInstrumentCatalog binance(&binanceHttp);
    upbit.refresh(); upbitHttp.reply(0, discoveryUpbitMarkets());
    binance.ensure("binance"); binanceHttp.reply(0, discoveryBinanceMarkets());
    WatchlistWorkspaceList lists; const auto listId = lists.createList("합성 공개 목록");
    const auto section = lists.addSection(listId, "코인");
    QVERIFY(lists.addIdentity(listId, {"coin", "upbit", "KRW-BTC"}, section));
    QVERIFY(lists.addIdentity(listId, {"coin", "upbit", "KRW-HYPE"}, section));
    QVERIFY(lists.addIdentity(listId, {"coin", "upbit", "KRW-UNKNOWN"}, section));
    MarketCandles candles(&candleHttp, std::unique_ptr<PublicCandleSocket>{});
    candles.setVerifiedUpbitMarkets(upbit.verifiedMarkets(), upbit.marketListAvailable());
    ChartWorkspace workspace(&candles, nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&upbit, nullptr, &binance); workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));
     QTest::qWait(25);
    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(tree->topLevelItem(0)->childCount(), 3);
    QSignalSpy changes(&lists, &WatchlistWorkspaceList::changed);
    auto* oldHype = tree->topLevelItem(0)->child(1);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(oldHype).center());
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:binance:HYPEUSDT"));
    QCOMPARE(workspace.selectedIdentity(), workspace.selectedChartIdentity());
    QCOMPARE(candles.series().identity, workspace.selectedChartIdentity());
    QCOMPARE(changes.count(), 1); // One atomic row repair, without remove/add intermediate state.
    auto* routeStatus=workspace.findChild<QLabel*>("chartWorkspaceListStatus"); QVERIFY(routeStatus);
    QVERIFY(routeStatus->text().contains("바이낸스"));
    workspace.setRealtimeDelayedIdentities({workspace.selectedChartIdentity()});
    QVERIFY(routeStatus->text().contains("바이낸스")); // Quote refresh cannot erase the explanation immediately.
    const auto repaired = lists.userList(listId);
    QCOMPARE(repaired.entries.size(), 3);
    QCOMPARE(repaired.entries[1].identity.key(), QString("coin:binance:HYPEUSDT"));
    QCOMPARE(repaired.entries[1].sectionId, section);
    QCOMPARE(tree->selectedItems().size(), 1);
    QCOMPARE(tree->selectedItems().first()->data(0, Qt::UserRole).toString(), workspace.selectedChartIdentity());
    QVERIFY(std::any_of(candleHttp.requests.cbegin(), candleHttp.requests.cend(), [](const RequestSpec& request) {
        return request.url.host() == "api.binance.com" && QUrlQuery(request.url).queryItemValue("symbol") == "HYPEUSDT";
    }));
    for (const auto& request : candleHttp.requests) {
        QVERIFY(QUrlQuery(request.url).queryItemValue("market") != "KRW-HYPE");
        QVERIFY(QUrlQuery(request.url).queryItemValue("market") != "KRW-UNKNOWN");
    }
    const auto requestsBefore = candleHttp.requests.size();
    auto* unknown = tree->topLevelItem(0)->child(2);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(unknown).center());
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:binance:HYPEUSDT"));
    QCOMPARE(workspace.selectedIdentity(), workspace.selectedChartIdentity());
    QCOMPARE(candles.series().identity, workspace.selectedChartIdentity());
    QCOMPARE(candleHttp.requests.size(), requestsBefore);
    QCOMPARE(changes.count(), 1);
    DiscoveryTransport studyHttp; ChartStudyFeed feed(&studyHttp, &binance);
    feed.setVerifiedUpbitMarkets(upbit.verifiedMarkets(), upbit.marketListAvailable());
    workspace.setStudyFeed(&feed);
    workspace.findChild<QPushButton*>("chartWatchTrendToggle")->click();
    auto* trends=workspace.findChild<QTreeWidget*>("chartWatchTrendRows"); QVERIFY(trends);
    QCOMPARE(trends->topLevelItemCount(),3);
    const auto intervalBefore=workspace.timeframeMs();
    QMetaObject::invokeMethod(trends,"itemClicked",Qt::DirectConnection,
        Q_ARG(QTreeWidgetItem*,trends->topLevelItem(2)),Q_ARG(int,2));
    QCOMPARE(workspace.selectedChartIdentity(),QString("coin:binance:HYPEUSDT"));
    QCOMPARE(workspace.timeframeMs(),intervalBefore); // Rejected row cannot change the previous chart's timeframe.
}

void ChartWorkspaceTest::representativePickerSavesPublicGroupsAndVisibleReorderRoundTrips() {
    DiscoveryTransport upbitHttp, binanceHttp, boardHttp;
    PublicMarketCatalog upbit(&upbitHttp); ChartInstrumentCatalog binance(&binanceHttp); PublicEquityBoard board(&boardHttp);
    upbit.refresh(); upbitHttp.reply(0, discoveryUpbitMarkets());
    binance.ensure("binance"); binanceHttp.reply(0, discoveryBinanceMarkets());
    board.refresh(); boardHttp.reply(0, "[]");
    boardHttp.reply(1, R"({"stocks":[{"itemCode":"005930","stockName":"삼성전자","closePrice":"100","marketValue":"10"},{"itemCode":"000660","stockName":"SK하이닉스","closePrice":"200","marketValue":"9"}]})");
    boardHttp.reply(2, R"({"data":{"rows":[{"symbol":"AAPL","name":"Apple Common Stock","lastsale":"100","marketCap":"1000"},{"symbol":"MSFT","name":"Microsoft Common Stock","lastsale":"200","marketCap":"2000"},{"symbol":"NVDA","name":"NVIDIA Common Stock","lastsale":"300","marketCap":"3000"}]}})");
    WatchlistWorkspaceList lists; const auto previousId = lists.createList("기존 공개 목록");
    QVERIFY(lists.addIdentity(previousId, {"coin", "upbit", "KRW-BTC"}));
    const auto previous = lists.userList(previousId);
    ChartWorkspace workspace(nullptr, nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists); workspace.setPublicNameSources(&upbit, &board, &binance);
    workspace.resize(1440, 900); workspace.show(); selectSavedScopeAndExpandDetails(workspace);
    QVERIFY(test::openInstrumentPanel(workspace));

    bindThemePresentation(&workspace, [&] { workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace)); });
    auto* discover = workspace.findChild<QAction*>("chartListAddIdentity"); QVERIFY(discover); discover->trigger();
    auto* picker = workspace.findChild<ChartInstrumentPicker*>("chartInstrumentPicker"); QVERIFY(picker);
    QCOMPARE(picker->representativeIdentities(), QStringList({"coin:upbit:KRW-BTC", "coin:binance:HYPEUSDT",
        "stock:KR:005930", "stock:KR:000660", "stock:US:AAPL", "stock:US:MSFT", "stock:US:NVDA"}));
    auto* bulk = picker->findChild<QPushButton*>("chartInstrumentAddRepresentatives"); QVERIFY(bulk); QVERIFY(bulk->isEnabled());
    bulk->click(); QCOMPARE(lists.userLists().size(), 2); QCOMPARE(lists.userList(previousId), previous);
    const auto created = lists.userLists().last(); QCOMPARE(created.name, QString("대표 종목")); QCOMPARE(created.entries.size(), 7);
    QCOMPARE(created.sections.size(), 3); QCOMPARE(created.sections[0].name, QString("코인"));
    QCOMPARE(created.sections[1].name, QString("한국")); QCOMPARE(created.sections[2].name, QString("미국"));
    for (const auto& entry : created.entries) {
        const auto group = entry.identity.market == "coin" ? 0 : entry.identity.source == "KR" ? 1 : 2;
        QCOMPARE(entry.sectionId, created.sections[group].id);
        if (entry.identity.market == "stock") QVERIFY(!lists.isCurrentlyVerified(entry.identity));
    }
    const auto renders = qEnvironmentVariable("KEEFI_CHART_DISCOVERY_RENDER_DIR");
    if (!renders.isEmpty()) {
        QVERIFY(QDir().mkpath(renders));
        for (auto mode : {ThemeMode::Light, ThemeMode::Dark}) for (const QSize size : {QSize(760, 620), QSize(540, 440)}) {
            KeefiTheme::instance()->setMode(mode); picker->resize(size); QTest::qWait(25);
            QVERIFY(picker->grab().save(renders + "/picker-" + (mode == ThemeMode::Light ? "light-" : "dark-") + QString::number(size.width()) + ".png"));
        }
    }
    const auto previousIdentity=workspace.selectedChartIdentity();
    picker->setQuery("skhy");picker->findChild<QPushButton*>("chartInstrumentOpen")->click();
    QCOMPARE(workspace.selectedChartIdentity(),QStringLiteral("stock:KR:000660"));QVERIFY(!picker->isVisible());
    QVERIFY(workspace.findChild<QPushButton*>("chartStockConnect")->isVisible());
    QVERIFY(!lists.isCurrentlyVerified({"stock","KR","000660"}));
    QVERIFY(workspace.setIdentity(previousIdentity));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* panel = test::openInstrumentPanel(workspace); QVERIFY(panel);
    auto* tree = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree); QCOMPARE(tree->topLevelItemCount(), 3);
    auto* up = workspace.findChild<QAction*>("chartListMoveUp"); auto* down = workspace.findChild<QAction*>("chartListMoveDown");
    QVERIFY(up && down);
    tree->setCurrentItem(tree->topLevelItem(1)); QVERIFY(up->isEnabled()); up->trigger();
    QCOMPARE(lists.userList(created.id).sections[0].name, QString("한국"));
    tree->setCurrentItem(tree->topLevelItem(2)); QVERIFY(up->isEnabled()); up->trigger();
    QCOMPARE(lists.userList(created.id).sections[1].name, QString("미국"));
    tree->setCurrentItem(tree->topLevelItem(0)->child(0)); down->trigger();
    const auto reordered = lists.userList(created.id);
    QStringList korea;
    for (const auto& entry : reordered.entries) if (entry.identity.source == "KR") korea.append(entry.identity.code);
    QCOMPARE(korea, QStringList({"000660", "005930"}));
    WatchlistWorkspaceList restored; QVERIFY(restored.restorePublicState(lists.publicState()));
    QCOMPARE(restored.userList(created.id), reordered); QCOMPARE(restored.userList(previousId), previous);
    QVERIFY(!restored.isCurrentlyVerified({"stock", "KR", "005930"}));
    if (!renders.isEmpty()) for (auto mode : {ThemeMode::Light, ThemeMode::Dark}) for (const QSize size : {QSize(1440, 900), QSize(1280, 800)}) {
        KeefiTheme::instance()->setMode(mode); workspace.resize(size); QTest::qWait(25);
        QVERIFY(workspace.findChild<QLineEdit*>("chartWorkspaceAsset")->isVisible());
        QVERIFY(panel->grab().save(renders + "/chart-discovery-" + (mode == ThemeMode::Light ? "light-" : "dark-") + QString::number(size.width()) + ".png"));
    }
    KeefiTheme::instance()->setMode(ThemeMode::Light);
}

namespace {
QTreeWidgetItem* browserItem(QTreeWidget* tree,const QString& key) {
    for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==key)return *it;
    return nullptr;
}
QStringList browserVisibleKeys(QTreeWidget* tree) {
    QStringList result;for(QTreeWidgetItemIterator it(tree);*it;++it)if(!(*it)->isHidden()&&!(*it)->data(0,Qt::UserRole).toString().isEmpty())result.append((*it)->data(0,Qt::UserRole).toString());return result;
}
}
void ChartWorkspaceTest::chartRankingAndExpandedMetricsPreserveSelection() {
    DiscoveryTransport http, boardHttp;
    PublicMarketCatalog markets(&http); PublicEquityBoard board(&boardHttp);
    markets.refresh();
    // PEPE is outside the pinned id table: its market cap joins only through
    // Upbit's English name, as most Upbit KRW coins must.
    http.reply(0,R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin"},{"market":"KRW-ETH","korean_name":"이더리움","english_name":"Ethereum"},{"market":"KRW-SOL","korean_name":"솔라나","english_name":"Solana"},{"market":"KRW-PEPE","korean_name":"페페","english_name":"Pepe"}])");
    QTRY_VERIFY_WITH_TIMEOUT(http.tokens.size()>=2,3000);
    const auto now=QString::number(QDateTime::currentMSecsSinceEpoch()-100);
    QJsonArray tickers;
    for(const auto& row:QList<QStringList>{{"KRW-BTC","100000000","0.02","9000000000","90"},{"KRW-ETH","3000000","-0.03","6000000000","2000"},{"KRW-SOL","150000","0.08","1000000000","6666"},{"KRW-PEPE","20","0.01","500000000","25000000"}})
        tickers.append(QJsonObject{{"market",row[0]},{"trade_price",row[1]},{"prev_closing_price",row[1]},{"signed_change_rate",row[2]},{"timestamp",now},{"acc_trade_price_24h",row[3]},{"acc_trade_volume_24h",row[4]}});
    http.reply(1,QJsonDocument(tickers).toJson(QJsonDocument::Compact));
    board.refresh();
    boardHttp.reply(0,R"([{"id":"bitcoin","symbol":"btc","name":"Bitcoin","market_cap":2000000000000,"current_price":100000000},{"id":"ethereum","symbol":"eth","name":"Ethereum","market_cap":500000000000,"current_price":3000000},{"id":"pepe","symbol":"pepe","name":"Pepe","market_cap":200000000000,"current_price":20},{"id":"solana","symbol":"sol","name":"Solana","market_cap":100000000000,"current_price":150000}])");
    WatchlistWorkspaceList lists; const auto list=lists.createList("내 관심"); lists.useList(list);
    QVERIFY(lists.addIdentity(list,{"coin","upbit","KRW-ETH"}));const auto saved=lists.publicState();
    ChartWorkspace workspace(nullptr,nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists); workspace.setPublicNameSources(&markets,&board,nullptr);
    workspace.resize(1440,900); workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));
     QCoreApplication::processEvents();
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist"); QVERIFY(tree);
    auto* header=tree->header(); QVERIFY2(header&&header->isVisible(),"Chart ranking headers must be directly discoverable");
    const auto clickSort=[header](int column){QTest::mouseClick(header->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(header->sectionViewportPosition(column)+header->sectionSize(column)/2,header->viewport()->height()/2));};
    QVERIFY(workspace.setIdentity("coin:upbit:KRW-ETH"));
    QVERIFY(!tree->isColumnHidden(6));clickSort(6);
    QCOMPARE(browserVisibleKeys(tree),(QStringList{"coin:upbit:KRW-SOL","coin:upbit:KRW-BTC","coin:upbit:KRW-PEPE","coin:upbit:KRW-ETH"}));
    clickSort(6);
    QCOMPARE(browserVisibleKeys(tree),(QStringList{"coin:upbit:KRW-ETH","coin:upbit:KRW-PEPE","coin:upbit:KRW-BTC","coin:upbit:KRW-SOL"}));
    auto* expand=workspace.findChild<QPushButton*>("chartBrowserExpand"); QVERIFY(expand);QTest::mouseClick(expand,Qt::LeftButton);
    QTRY_VERIFY(!tree->isColumnHidden(5));clickSort(5);
    QCOMPARE(browserVisibleKeys(tree),(QStringList{"coin:upbit:KRW-BTC","coin:upbit:KRW-ETH","coin:upbit:KRW-PEPE","coin:upbit:KRW-SOL"}));
    QCOMPARE(workspace.selectedChartIdentity(),QString("coin:upbit:KRW-ETH"));
    QTRY_VERIFY(!tree->isColumnHidden(3)); QVERIFY(!tree->isColumnHidden(4)); QVERIFY(!tree->isColumnHidden(5));
    auto* btc=browserItem(tree,"coin:upbit:KRW-BTC"); QVERIFY(btc);
    QVERIFY2(btc->text(3).contains("KRW"),qPrintable(btc->text(3)));
    QVERIFY2(btc->text(4).contains("90")&&btc->text(4).contains("BTC"),qPrintable(btc->text(4)));
    QVERIFY2(btc->toolTip(5).contains("CoinGecko"),qPrintable(btc->toolTip(5)));
    auto* pepe=browserItem(tree,"coin:upbit:KRW-PEPE"); QVERIFY(pepe);
    QVERIFY2(pepe->text(5)!=QStringLiteral("—")&&pepe->text(5).contains("KRW"),qPrintable(pepe->text(5)));
    QVERIFY2(pepe->toolTip(5).contains("CoinGecko")&&pepe->toolTip(5).contains("pepe"),qPrintable(pepe->toolTip(5)));
    QTRY_VERIFY2(tree->width()>700,"Expanded metrics must not remain squeezed into a sidebar");
    QCOMPARE(workspace.selectedChartIdentity(),QString("coin:upbit:KRW-ETH"));
    QCOMPARE(browserItem(tree,"coin:upbit:KRW-ETH")->checkState(2),Qt::Checked);
    QCOMPARE(lists.publicState(),saved);workspace.hide();
}

void ChartWorkspaceTest::coinBrowserListsMostTradedFirstAndPrefillsBinanceRows() {
    DiscoveryTransport upbitHttp,binanceHttp,boardHttp;
    PublicMarketCatalog upbit(&upbitHttp);ChartInstrumentCatalog binance(&binanceHttp);PublicEquityBoard board(&boardHttp);
    upbit.refresh();upbitHttp.reply(0,R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin","market_event":{"warning":false,"caution":{}}},
        {"market":"KRW-XRP","korean_name":"엑스알피","english_name":"XRP","market_event":{"warning":false,"caution":{}}},
        {"market":"KRW-USDT","korean_name":"테더","english_name":"Tether","market_event":{"warning":false,"caution":{}}}])");
    QTRY_VERIFY_WITH_TIMEOUT(upbitHttp.tokens.size()>=2,3000);
    const auto now=QString::number(QDateTime::currentMSecsSinceEpoch()-100);QJsonArray tickers;
    for(const auto& row:QList<QStringList>{{"KRW-BTC","150000000","500000000000"},{"KRW-XRP","3000","900000000000"},{"KRW-USDT","1400","1000000000"}})
        tickers.append(QJsonObject{{"market",row[0]},{"trade_price",row[1]},{"prev_closing_price",row[1]},{"signed_change_rate","0.01"},{"timestamp",now},{"acc_trade_price_24h",row[2]}});
    upbitHttp.reply(1,QJsonDocument(tickers).toJson(QJsonDocument::Compact));
    binance.ensure("binance");binanceHttp.reply(0,R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},
        {"symbol":"ETHUSDT","baseAsset":"ETH","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},
        {"symbol":"AAAUSDT","baseAsset":"AAA","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})");
    WatchlistWorkspaceList lists;lists.useList(lists.createList("관심"));
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setWatchlistModel(&lists);workspace.setPublicNameSources(&upbit,&board,&binance);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QCoreApplication::processEvents();
    // Upbit and Binance are separate tabs, so one coin is listed once per tab (user report on Dev 87:
    // the sorted coin list showed BTC twice, once per exchange). The Upbit tab ranks by its own KRW value.
    QCOMPARE(workspace.findChild<QPushButton*>("chartBrowserMarket_upbit")->isChecked(),true);
    QCOMPARE(browserVisibleKeys(tree),(QStringList{"coin:upbit:KRW-XRP","coin:upbit:KRW-BTC","coin:upbit:KRW-USDT"}));
    auto* status=workspace.findChild<QLabel*>("chartWorkspaceListStatus");QVERIFY(status);
    QVERIFY2(status->text().contains(QStringLiteral("3개"))&&!status->text().contains(QStringLiteral("거래대금순"))&&!status->text().contains(QStringLiteral("업비트 원화")),qPrintable(status->text()));
    // The Binance 24h ranking is read only when the Binance tab is shown.
    const auto rankingRequests=[&]{int count=0;for(const auto& r:boardHttp.requests)if(r.url.toString().contains("ticker/24hr"))++count;return count;};
    QCOMPARE(rankingRequests(),0);
    workspace.findChild<QPushButton*>("chartBrowserMarket_binance")->click();
    QTRY_COMPARE(rankingRequests(),1);
    int ranking=-1;for(int i=0;i<boardHttp.requests.size();++i)if(boardHttp.requests[i].url.toString().contains("ticker/24hr"))ranking=i;
    QJsonArray binanceTickers;const auto close=QDateTime::currentMSecsSinceEpoch()-1000;
    for(const auto& row:QList<QStringList>{{"BTCUSDT","100000","-0.420","1000000000"},{"ETHUSDT","3000","1.5","100000000"},{"AAAUSDT","1","-0.004","1000000"}})
        binanceTickers.append(QJsonObject{{"symbol",row[0]},{"lastPrice",row[1]},{"priceChangePercent",row[2]},{"quoteVolume",row[3]},{"closeTime",close},{"count",10}});
    boardHttp.reply(ranking,QJsonDocument(binanceTickers).toJson(QJsonDocument::Compact));QCoreApplication::processEvents();
    // Binance ranks by its own USDT quote volume and no longer needs Upbit's KRW-USDT to do so.
    const auto keys=browserVisibleKeys(tree);
    QCOMPARE(keys,(QStringList{"coin:binance:BTCUSDT","coin:binance:ETHUSDT","coin:binance:AAAUSDT"}));
    QVERIFY2(status->text().contains(QStringLiteral("3개"))&&!status->text().contains(QStringLiteral("현물"))&&!status->text().contains(QStringLiteral("업비트")),qPrintable(status->text()));
    // Before any stream tick a Binance row shows its 24h ticker, labelled as a public reference.
    auto* btc=browserItem(tree,"coin:binance:BTCUSDT");QVERIFY(btc);QVERIFY2(!btc->text(1).contains(QStringLiteral("공개 참고")),qPrintable(btc->text(1)));QVERIFY(btc->toolTip(0).contains(QStringLiteral("공개 참고 시세")));QVERIFY(btc->text(1).contains("-0.42%"));
    auto* flat=browserItem(tree,"coin:binance:AAAUSDT");QVERIFY(flat);QVERIFY2(flat->text(1).contains("0.00%")&&!flat->text(1).contains("-0.00%"),qPrintable(flat->text(1)));
    // A later quote does not reshuffle rows that are being read.
    WatchlistRowState quote;quote.price="1";quote.change="+1.00%";lists.setRowState({"coin","binance","AAAUSDT"},quote);QCoreApplication::processEvents();
    QCOMPARE(browserVisibleKeys(tree),keys);
    // Saved rows also stay on their own exchange's tab. The watch scope keeps the user's own order by default.
    const auto list=lists.userLists().first().id;
    for(const WatchlistIdentity& identity:{WatchlistIdentity{"coin","upbit","KRW-USDT"},WatchlistIdentity{"coin","binance","BTCUSDT"},WatchlistIdentity{"coin","upbit","KRW-XRP"}})QVERIFY(lists.addIdentity(list,identity));
    workspace.findChild<QPushButton*>("chartBrowserScope_watch")->click();QCOMPARE(browserVisibleKeys(tree),QStringList{"coin:binance:BTCUSDT"});
    workspace.findChild<QPushButton*>("chartBrowserMarket_upbit")->click();QCOMPARE(browserVisibleKeys(tree),(QStringList{"coin:upbit:KRW-USDT","coin:upbit:KRW-XRP"}));
    auto* expand=workspace.findChild<QPushButton*>("chartBrowserExpand");QVERIFY(expand);QTest::mouseClick(expand,Qt::LeftButton);QTRY_VERIFY(!tree->isColumnHidden(3));
    auto* header=tree->header();QTest::mouseClick(header->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(header->sectionViewportPosition(3)+header->sectionSize(3)/2,header->viewport()->height()/2));QCOMPARE(browserVisibleKeys(tree),(QStringList{"coin:upbit:KRW-XRP","coin:upbit:KRW-USDT"}));
}
void ChartWorkspaceTest::realPublicBrowserCaptureForReview_data(){
    QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");
    QTest::newRow("dark-1440")<<true<<1440;QTest::newRow("light-1440")<<false<<1440;QTest::newRow("dark-1100")<<true<<1100;
}
void ChartWorkspaceTest::realPublicBrowserCaptureForReview(){
    // Opt-in review capture: answers each request with public responses saved beforehand; nothing is fetched here.
    const auto directory=qEnvironmentVariable("KEEFI_CHART_REAL_DIR");if(directory.isEmpty())QSKIP("Opt-in: KEEFI_CHART_REAL_DIR holds saved public market responses");
    QFETCH(bool,dark);QFETCH(int,width);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    const auto read=[&](const QString& name){QFile file(QDir(directory).filePath(name));return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};};
    const auto upbitTickers=QJsonDocument::fromJson(read("upbit-tickers.json")).array();QVERIFY(!upbitTickers.isEmpty());
    DiscoveryTransport upbitHttp,binanceHttp,boardHttp,candleHttp;
    PublicMarketCatalog upbit(&upbitHttp);ChartInstrumentCatalog binance(&binanceHttp);PublicEquityBoard board(&boardHttp);
    int upbitHandled=0,boardHandled=0,candleHandled=0;
    const auto serve=[&]{
        while(upbitHandled<upbitHttp.requests.size()){const auto url=upbitHttp.requests[upbitHandled].url;QByteArray body;
            if(url.path().endsWith("/market/all"))body=read("upbit-markets.json");
            else{const auto wanted=QUrlQuery(url).queryItemValue("markets").split(',');QJsonArray rows;for(const auto& row:upbitTickers)if(wanted.contains(row.toObject().value("market").toString()))rows.append(row);body=QJsonDocument(rows).toJson(QJsonDocument::Compact);}
            upbitHttp.reply(upbitHandled++,body);}
        while(boardHandled<boardHttp.requests.size()){const auto url=boardHttp.requests[boardHandled].url.toString();
            boardHttp.reply(boardHandled++,url.contains("ticker/24hr")?read("binance-24hr.json"):QByteArray("[]"));}
        while(candleHandled<candleHttp.requests.size()){const auto url=candleHttp.requests[candleHandled].url.toString();
            candleHttp.reply(candleHandled++,url.contains("api.binance.com")?read("binance-btc-15m.json"):read("upbit-btc-15m.json"));}
    };
    upbit.refresh();binance.ensure("binance");binanceHttp.reply(0,read("binance-exchangeinfo.json"));
    QTRY_VERIFY_WITH_TIMEOUT((serve(),upbit.completedAtMs()>0),20000);
    MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
    WatchlistWorkspaceList lists;lists.useList(lists.createList(QStringLiteral("관심")));
    ChartWorkspace workspace(&candles,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setWatchlistModel(&lists);workspace.setPublicNameSources(&upbit,&board,&binance);
    workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));workspace.resize(width,1000);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);
    for(int i=0;i<20;++i){serve();QTest::qWait(25);}
    workspace.setIdentity("coin:upbit:KRW-BTC");for(int i=0;i<40;++i){serve();QTest::qWait(25);}
    const auto out=qEnvironmentVariable("KEEFI_CHART_REAL_OUT",directory);QVERIFY(QDir().mkpath(out));
    // Each exchange tab lists only its own rows, so no coin appears twice in one list.
    const auto onlySource=[](const QStringList& keys,const QString& prefix){return std::all_of(keys.cbegin(),keys.cend(),[&](const auto& key){return key.startsWith(prefix);});};
    const auto upbitKeys=browserVisibleKeys(tree);qDebug()<<"real upbit order"<<upbitKeys.mid(0,12);QVERIFY(upbitKeys.size()>100);QVERIFY(onlySource(upbitKeys,"coin:upbit:"));
    QVERIFY(workspace.grab().save(QDir(out).filePath(QStringLiteral("browser-%1-upbit.png").arg(QString::fromLatin1(QTest::currentDataTag())))));
    workspace.findChild<QPushButton*>("chartBrowserMarket_binance")->click();
    QTRY_VERIFY_WITH_TIMEOUT((serve(),std::any_of(boardHttp.requests.cbegin(),boardHttp.requests.cend(),[](const auto& r){return r.url.toString().contains("ticker/24hr");})),5000);
    for(int i=0;i<40;++i){serve();QTest::qWait(25);}
    const auto binanceKeys=browserVisibleKeys(tree);qDebug()<<"real binance order"<<binanceKeys.mid(0,12);QVERIFY(binanceKeys.size()>100);QVERIFY(onlySource(binanceKeys,"coin:binance:"));
    QVERIFY(workspace.grab().save(QDir(out).filePath(QStringLiteral("browser-%1-binance.png").arg(QString::fromLatin1(QTest::currentDataTag())))));
}
void ChartWorkspaceTest::terminalBrowserDiscoversMarketsWithoutAddingFavorites() {
    DiscoveryTransport upbitHttp,binanceHttp,boardHttp;
    PublicMarketCatalog upbit(&upbitHttp);ChartInstrumentCatalog binance(&binanceHttp);PublicEquityBoard board(&boardHttp);
    upbit.refresh();upbitHttp.reply(0,discoveryUpbitMarkets());binance.ensure("binance");binanceHttp.reply(0,discoveryBinanceMarkets());
    board.refresh();boardHttp.reply(0,"[]");
    boardHttp.reply(1,R"({"stocks":[{"itemCode":"005930","stockName":"삼성전자","closePrice":"100","marketValue":"10"}]})");
    boardHttp.reply(2,R"({"data":{"rows":[{"symbol":"AAPL","name":"Apple Common Stock","lastsale":"100","marketCap":"1000"}]}})");
    WatchlistWorkspaceList lists;const auto first=lists.createList("관심 A"),second=lists.createList("관심 B");lists.useList(first);
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setWatchlistModel(&lists);workspace.setPublicNameSources(&upbit,&board,&binance);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");auto* search=workspace.findChild<QLineEdit*>("chartWorkspaceAsset");QVERIFY(tree&&search);
    QSignalSpy demand(&workspace,&ChartWorkspace::visibleIdentitiesChanged);
    QCoreApplication::processEvents();QVERIFY(workspace.findChild<QPushButton*>("chartBrowserScope_all")->isChecked());
    for(const auto* market:{"upbit","binance","kr","us"})QVERIFY(workspace.findChild<QPushButton*>(QString("chartBrowserMarket_")+market)->isVisible());
    QVERIFY(workspace.findChild<QWidget*>("chartWorkspacePositions")->isHidden());QVERIFY(!workspace.findChild<QLabel*>("chartDetailSummary"));
    QCOMPARE(workspace.currentListId(),first);const auto saved=lists.publicState();
    search->setText("비트코인");QCOMPARE(browserVisibleKeys(tree),QStringList{"coin:upbit:KRW-BTC"});QTest::keyClick(search,Qt::Key_Return);
    QCOMPARE(workspace.selectedChartIdentity(),QString("coin:upbit:KRW-BTC"));QCOMPARE(lists.publicState(),saved);
    QVERIFY(test::openInstrumentPanel(workspace));
    workspace.findChild<QPushButton*>("chartBrowserMarket_us")->click();search->setText("애플");
    QCOMPARE(browserVisibleKeys(tree),QStringList{"stock:US:AAPL"});QTest::keyClick(search,Qt::Key_Return);
    QCOMPARE(workspace.selectedChartIdentity(),QString("stock:US:AAPL"));QCOMPARE(lists.publicState(),saved);QVERIFY(!lists.isCurrentlyVerified({"stock","US","AAPL"}));
    QVERIFY(test::openInstrumentPanel(workspace));
    auto* apple=browserItem(tree,"stock:US:AAPL");QVERIFY(apple);QVERIFY(apple->text(1).contains("USD"));QVERIFY(apple->toolTip(0).contains("원천 시각 미제공"));
    QVERIFY(workspace.findChild<QLabel*>("chartWorkspaceListStatus")->text().contains("Nasdaq 거래소 최대 80"));
    QCoreApplication::processEvents();QVERIFY(!demand.isEmpty());QVERIFY(workspace.visiblePublicIdentityKeysForDisplay().contains("stock:US:AAPL"));
    for(const auto& row:demand)QVERIFY(!row.first().toStringList().contains("stock:US:AAPL"));
    workspace.findChild<QPushButton*>("chartBrowserMarket_kr")->click();search->setText("삼성");
    auto* samsung=browserItem(tree,"stock:KR:005930");QVERIFY(samsung&&!samsung->isHidden());tree->setCurrentItem(samsung);tree->setFocus();
    const auto selected=workspace.selectedChartIdentity();
    const QPoint favorite(tree->columnViewportPosition(2)+tree->columnWidth(2)/2,tree->visualItemRect(samsung).center().y());
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,favorite);
    QCOMPARE(workspace.selectedChartIdentity(),selected);QTRY_COMPARE(lists.userList(first).entries.size(),1);QCOMPARE(lists.userList(second).entries.size(),0);
    QCOMPARE(lists.userList(first).entries.first().identity.key(),QString("stock:KR:005930"));
    QTest::keyClick(tree,Qt::Key_Space);QTRY_COMPARE(lists.userList(first).entries.size(),0);QCOMPARE(workspace.selectedChartIdentity(),selected);
    QVERIFY(!workspace.findChild<QDialog*>("chartInstrumentPicker"));
}
void ChartWorkspaceTest::browserCatalogFailureRetryAndRecovery_data() {
    QTest::addColumn<bool>("timeout");
    QTest::newRow("http503")<<false;
    QTest::newRow("timeout")<<true;
}
void ChartWorkspaceTest::browserCatalogFailureRetryAndRecovery() {
    QFETCH(bool,timeout);
    qint64 now=1000;DiscoveryTransport http;
    PublicMarketCatalog catalog(&http,nullptr,[&]{return now;});
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&catalog,nullptr,nullptr);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* status=workspace.findChild<QLabel*>("chartWorkspaceListStatus");
    auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");
    auto* search=workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(status&&retry&&search&&tree);
    QCOMPARE(http.requests.size(),1);QVERIFY(status->text().contains("목록 조회 중"));
    QVERIFY(!status->text().contains("표시할 종목이 없습니다"));QVERIFY(retry->isHidden());
    HttpResult failure;failure.status=timeout?0:503;failure.timedOut=timeout;
    emit http.finished(http.tokens.last(),failure);
    const QString reason=timeout?QStringLiteral("시간 초과"):QStringLiteral("HTTP 503");
    QVERIFY(!catalog.busy());QVERIFY(status->text().contains("종목 목록 조회 실패"));QVERIFY(status->text().contains(reason));
    auto* chartStatus=workspace.findChild<QLabel*>("marketCandlesStatus");QVERIFY(chartStatus);
    QVERIFY(displayedText(chartStatus).contains("종목 목록 조회 실패"));QVERIFY(displayedText(chartStatus).contains(reason));
    QVERIFY(!displayedText(chartStatus).contains("종목 확인 중"));
    QVERIFY(!status->text().contains("확인하고 있어요"));QVERIFY(!status->text().contains("표시할 종목이 없습니다"));
    QVERIFY(retry->isVisible()&&retry->isEnabled());
    // An Upbit metadata failure cannot replace a stock tab's own result/scope.
    workspace.findChild<QPushButton*>("chartBrowserMarket_us")->click();search->setText("NOT_IN_LIST");
    QVERIFY(!status->text().contains(reason));QVERIFY(!status->text().contains("업비트"));QVERIFY(retry->isHidden());
    QVERIFY(status->text().contains("Nasdaq 거래소 최대 80"));QVERIFY(status->text().contains("NYSE"));
    workspace.findChild<QPushButton*>("chartBrowserMarket_kr")->click();search->setText("NOT_IN_LIST");
    QVERIFY(!status->text().contains(reason));QVERIFY(status->text().contains("KOSPI 최대 50"));QVERIFY(status->text().contains("KOSDAQ"));
    workspace.findChild<QPushButton*>("chartBrowserMarket_upbit")->click();
    QCOMPARE(http.requests.size(),1);QVERIFY(status->text().contains(reason));
    now+=400;retry->click();QCOMPARE(http.requests.size(),2);QVERIFY(catalog.busy());
    QVERIFY(!retry->isEnabled());QVERIFY(status->text().contains("목록 조회 중"));
    QVERIFY(!displayedText(chartStatus).contains("조회 실패"));
    retry->click();catalog.refresh();QCOMPARE(http.requests.size(),2); // No second in-flight request.
    http.reply(1,discoveryUpbitMarkets());
    QVERIFY(catalog.marketListAvailable());
    QVERIFY(retry->isHidden());QVERIFY(!status->text().contains("조회 실패"));
    QVERIFY(!displayedText(chartStatus).contains("조회 실패"));
    QVERIFY(browserVisibleKeys(tree).contains("coin:upbit:KRW-BTC"));
    // A later ticker failure must not be misclassified as a metadata failure.
    now+=400;QTRY_COMPARE(http.requests.size(),3);emit http.finished(http.tokens.last(),failure);
    QVERIFY(catalog.marketListAvailable());QVERIFY(retry->isHidden());
    QVERIFY(!status->text().contains("종목 목록 조회 실패"));
}
void ChartWorkspaceTest::browserCatalogEmptySuccessIsNotFailure() {
    qint64 now=1000;DiscoveryTransport http;PublicMarketCatalog catalog(&http,nullptr,[&]{return now;});
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&catalog,nullptr,nullptr);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    QCOMPARE(http.requests.size(),1);http.reply(0,"[]");
    now+=400;QTRY_VERIFY(!catalog.busy());
    auto* status=workspace.findChild<QLabel*>("chartWorkspaceListStatus");auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");
    QVERIFY(status&&retry);QVERIFY(catalog.marketListAvailable());
    QVERIFY(status->text().contains("표시할 종목이 없습니다"));QVERIFY(!status->text().contains("조회 실패"));
    QVERIFY(!status->text().contains("확인하고 있어요"));QVERIFY(retry->isHidden());
}
void ChartWorkspaceTest::emptyUpbitListAndFailedBinanceFallbackSettle() {
    DiscoveryTransport upbitHttp, binanceHttp;
    PublicMarketCatalog upbit(&upbitHttp); ChartInstrumentCatalog binance(&binanceHttp);
    ChartWorkspace workspace(nullptr,nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&upbit,nullptr,&binance); workspace.resize(1440,900); workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    QTRY_COMPARE(upbitHttp.requests.size(),1); upbitHttp.reply(0,"[]");
    QTRY_COMPARE(binanceHttp.requests.size(),1);
    HttpResult failure;failure.status=503;emit binanceHttp.finished(binanceHttp.tokens.first(),failure);
    QVERIFY(!workspace.setIdentity("coin:upbit:KRW-BTC"));
    const auto* status=workspace.findChild<QLabel*>("chartWorkspaceListStatus"); QVERIFY(status);
    QVERIFY(status->text().contains("업비트")); QVERIFY(status->text().contains("바이낸스"));
    QVERIFY(status->text().contains("실패")); QVERIFY(!status->text().contains("확인하고 있어요"));
    auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");
    QVERIFY(retry&&retry->isVisible()&&!retry->isEnabled()); // The fallback catalogue owns its cooldown.
}
void ChartWorkspaceTest::stockBrowserRequestsWhileBinanceRankingIsPending() {
    DiscoveryTransport primary,ranking;
    PublicEquityBoard board(&primary,nullptr,&ranking); board.refreshBinanceRanking();
    QCOMPARE(ranking.requests.size(),1); QCOMPARE(primary.requests.size(),0);
    ChartWorkspace workspace(nullptr,nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(nullptr,&board,nullptr); workspace.resize(1440,900); workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    workspace.findChild<QPushButton*>("chartBrowserMarket_kr")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!primary.requests.isEmpty(),1200);
    QCOMPARE(ranking.requests.size(),1); // Its reply is still held.
}
void ChartWorkspaceTest::failedCataloguesRetryIndependently_data() {
    QTest::addColumn<int>("upbitSeconds");QTest::addColumn<qint64>("advance");
    QTest::addColumn<bool>("upbitReady");QTest::addColumn<bool>("binanceReady");
    QTest::newRow("binance-first")<<600<<qint64(301000)<<false<<true;
    QTest::newRow("upbit-first")<<120<<qint64(121000)<<true<<false;
    QTest::newRow("both-ready")<<120<<qint64(301000)<<true<<true;
}
void ChartWorkspaceTest::failedCataloguesRetryIndependently() {
    QFETCH(int,upbitSeconds);QFETCH(qint64,advance);QFETCH(bool,upbitReady);QFETCH(bool,binanceReady);
    qint64 now=1000;DiscoveryTransport u,b;PublicMarketCatalog upbit(&u,nullptr,[&]{return now;});
    ChartInstrumentCatalog binance(&b,nullptr,[&]{return now;});
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&upbit,nullptr,&binance);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    QTRY_COMPARE(u.requests.size(),1);QTRY_COMPARE(b.requests.size(),1);
    HttpResult limited;limited.status=429;limited.headers["Retry-After"]=QByteArray::number(upbitSeconds);
    emit u.finished(u.tokens.first(),limited);HttpResult failed;failed.status=503;emit b.finished(b.tokens.first(),failed);
    auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");QVERIFY(retry&&retry->isVisible()&&!retry->isEnabled());
    now+=advance;QTRY_VERIFY_WITH_TIMEOUT(retry->isEnabled(),2500);
    QCOMPARE(retry->text().contains("업비트"),upbitReady);QCOMPARE(retry->text().contains("바이낸스"),binanceReady);
    retry->click();QCOMPARE(u.requests.size(),upbitReady?2:1);QCOMPARE(b.requests.size(),binanceReady?2:1);
}
void ChartWorkspaceTest::catalogRetryHonoursHttpDate() {
    qint64 now=1000; DiscoveryTransport http; PublicMarketCatalog catalog(&http,nullptr,[&]{return now;});
    ChartWorkspace workspace(nullptr,nullptr); workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&catalog,nullptr,nullptr);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    QTRY_COMPARE(http.requests.size(),1);HttpResult limit;limit.status=429;
    limit.headers["Retry-After"]=QDateTime::currentDateTimeUtc().addSecs(120).toString(Qt::RFC2822Date).toLatin1();
    emit http.finished(http.tokens.first(),limit);auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");
    QVERIFY(retry);now+=61000;QVERIFY(catalog.retryAfterMs()>55000);QVERIFY(!retry->isEnabled());
    catalog.retryMarketList();QCOMPARE(http.requests.size(),1);
    now+=60000;QTRY_VERIFY(retry->isEnabled());retry->click();QCOMPARE(http.requests.size(),2);
}
void ChartWorkspaceTest::browserCatalogRefreshKeepsOldRowsWithoutReverification() {
    qint64 now=1000;DiscoveryTransport http,candleHttp;PublicMarketCatalog catalog(&http,nullptr,[&]{return now;});
    MarketCandles candles(&candleHttp,std::unique_ptr<PublicCandleSocket>{});
    ChartWorkspace workspace(&candles,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&catalog,nullptr,nullptr);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    http.reply(0,discoveryUpbitMarkets());now+=400;QTRY_COMPARE(http.requests.size(),2);
    HttpResult failedQuote;failedQuote.status=503;emit http.finished(http.tokens.last(),failedQuote);
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");auto* status=workspace.findChild<QLabel*>("chartWorkspaceListStatus");
    auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");QVERIFY(tree&&status&&retry);
    const auto before=browserVisibleKeys(tree);QCOMPARE(before,QStringList{"coin:upbit:KRW-BTC"});
    now+=300001;catalog.refresh();QCOMPARE(http.requests.size(),3);
    QCOMPARE(browserVisibleKeys(tree),before);QVERIFY(status->text().contains("갱신하고 있어요"));
    HttpResult failure;failure.status=503;emit http.finished(http.tokens.last(),failure);
    QVERIFY(!catalog.marketListAvailable());QVERIFY(catalog.verifiedMarkets().isEmpty());
    QCOMPARE(browserVisibleKeys(tree),before);QVERIFY(status->text().contains("이전 목록"));QVERIFY(status->text().contains("HTTP 503"));
    const auto calls=candleHttp.requests.size();QVERIFY(!workspace.setIdentity("coin:upbit:KRW-BTC"));
    QCOMPARE(candleHttp.requests.size(),calls);QVERIFY(retry->isVisible());QVERIFY(status->text().contains("HTTP 503"));
    now+=400;retry->click();QCOMPARE(http.requests.size(),4);http.reply(3,discoveryUpbitMarkets());
    QVERIFY(catalog.marketListAvailable());QVERIFY(!status->text().contains("조회 실패"));QCOMPARE(browserVisibleKeys(tree),before);
}
void ChartWorkspaceTest::browserCatalogRetryRespectsCooldown() {
    qint64 now=1000;DiscoveryTransport http;PublicMarketCatalog catalog(&http,nullptr,[&]{return now;});
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&catalog,nullptr,nullptr);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    HttpResult failure;failure.status=429;failure.headers.insert("Retry-After","120");emit http.finished(http.tokens.last(),failure);
    auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");auto* search=workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* status=workspace.findChild<QLabel*>("chartWorkspaceListStatus");QVERIFY(retry&&search&&status);
    QVERIFY(retry->isVisible());QVERIFY(!retry->isEnabled());QVERIFY(retry->text().contains("120"));
    const auto requests=http.requests.size();
    retry->click();catalog.refresh();QCOMPARE(http.requests.size(),requests);
    QVERIFY(status->text().contains("요청 제한"));
    now+=120000;search->setText("BTC");QVERIFY(retry->isEnabled());retry->click();
    QCOMPARE(http.requests.size(),requests+1);QVERIFY(catalog.busy());
    retry->click();catalog.refresh();QCOMPARE(http.requests.size(),requests+1);
}

void ChartWorkspaceTest::browserCatalogCooldownResumesAfterShow() {
    qint64 now=1000;DiscoveryTransport http;PublicMarketCatalog catalog(&http,nullptr,[&]{return now;});
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&catalog,nullptr,nullptr);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    HttpResult limited;limited.status=429;limited.headers.insert("Retry-After","120");emit http.finished(http.tokens.last(),limited);
    auto* retry=workspace.findChild<QPushButton*>("chartBrowserRetry");QVERIFY(retry);QVERIFY(!retry->isEnabled());
    const auto calls=http.requests.size();workspace.hide();now+=1000;workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));
    QVERIFY(retry->isVisible());QVERIFY(!retry->isEnabled());QVERIFY(retry->text().contains("119"));QCOMPARE(http.requests.size(),calls);
    // Reopening the panel resumes countdown updates without a retry, search or source event.
    now+=119000;QTRY_VERIFY_WITH_TIMEOUT(retry->isEnabled(),2000);QCOMPARE(http.requests.size(),calls);
    workspace.hide();workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));
    QVERIFY(retry->isEnabled());QCOMPARE(http.requests.size(),calls);
    retry->click();QCOMPARE(http.requests.size(),calls+1);QVERIFY(catalog.busy());
}
void ChartWorkspaceTest::browserMetadataFailureKeepsAnotherChart() {
    DiscoveryTransport upbitHttp,binanceHttp,candleHttp;
    PublicMarketCatalog upbit(&upbitHttp);ChartInstrumentCatalog binance(&binanceHttp);
    binance.ensure("binance");binanceHttp.reply(0,discoveryBinanceMarkets());
    const auto now=QDateTime::currentMSecsSinceEpoch();MarketCandles model(&candleHttp,std::unique_ptr<PublicCandleSocket>{},[now]{return now;});
    ChartWorkspace workspace(&model,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setTimeframe(60000);
    workspace.setPublicNameSources(&upbit,nullptr,&binance);QVERIFY(workspace.setIdentity("coin:binance:HYPEUSDT"));
    workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));
    QTRY_VERIFY(!candleHttp.tokens.isEmpty());
    const auto start=(now/60000-1)*60000;QJsonArray rows;
    rows.append(QJsonArray{QString::number(start),"100","110","90","101","2",QString::number(start+59999),"202","10","1","101","0"});
    candleHttp.reply(candleHttp.tokens.size()-1,QJsonDocument(rows).toJson());QVERIFY(model.series().available);
    auto* chart=workspace.findChild<QWidget*>("marketCandlesChart");auto* status=workspace.findChild<QLabel*>("marketCandlesStatus");QVERIFY(chart&&status);
    QCOMPARE(chart->property("displayCompletedCount").toInt(),1);
    HttpResult failure;failure.status=503;emit upbitHttp.finished(upbitHttp.tokens.last(),failure);
    QVERIFY(model.series().available);QVERIFY(!displayedText(status).contains("조회 실패"));
    const auto calls=candleHttp.requests.size();QVERIFY(!workspace.setIdentity("coin:upbit:KRW-BTC"));
    QCOMPARE(workspace.selectedChartIdentity(),QString("coin:binance:HYPEUSDT"));
    QCOMPARE(model.series().identity,QString("coin:binance:HYPEUSDT"));QVERIFY(model.series().available);
    QCOMPARE(chart->property("displayCompletedCount").toInt(),1);QCOMPARE(candleHttp.requests.size(),calls);
    QVERIFY(!displayedText(status).contains("HTTP 503"));QVERIFY(!displayedText(status).contains("조회 실패"));
    QVERIFY(workspace.findChild<QLabel*>("chartWorkspaceListStatus")->text().contains("HTTP 503"));
}

void ChartWorkspaceTest::browserFiltersPreserveSavedStateAndQuoteRows() {
    WatchlistWorkspaceList lists;const auto id=lists.createList("그룹 보존","blue");const auto section=lists.addSection(id,"혼합 그룹");
    const WatchlistIdentity btc{"coin","upbit","KRW-BTC"},eth{"coin","upbit","KRW-ETH"},apple{"stock","US","AAPL"};
    lists.setVerifiedInstruments({apple});QVERIFY(lists.addIdentity(id,btc,section));QVERIFY(lists.addIdentity(id,apple,section));QVERIFY(lists.addIdentity(id,eth,section));lists.useList(id);
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setWatchlistModel(&lists);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    workspace.findChild<QPushButton*>("chartBrowserScope_watch")->click();auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");auto* search=workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    const auto saved=lists.publicState();auto* row=browserItem(tree,eth.key());QVERIFY(row);tree->setCurrentItem(row);const QPersistentModelIndex index(tree->indexFromItem(row));
    auto* group=tree->topLevelItem(0);group->setExpanded(false);tree->setCurrentItem(group);
    QSignalSpy resets(tree->model(),&QAbstractItemModel::modelReset);search->setText("ETH");QCOMPARE(browserVisibleKeys(tree),QStringList{eth.key()});QVERIFY(group->isExpanded());
    QTest::keyClick(search,Qt::Key_Return);QCOMPARE(workspace.selectedChartIdentity(),eth.key());
    auto* panel = test::openInstrumentPanel(workspace); QVERIFY(panel);
    WatchlistRowState quote;quote.price="123";quote.change="+1.20%";lists.setRowState(eth,quote);
    QCOMPARE(resets.count(),0);QVERIFY(index.isValid());QCOMPARE(tree->currentItem(),row);QCOMPARE(row->text(1),QString("123\n+1.20%"));
    workspace.findChild<QPushButton*>("chartBrowserMarket_us")->click();QCOMPARE(browserVisibleKeys(tree),QStringList{apple.key()});QVERIFY(index.isValid());QCOMPARE(resets.count(),0);
    workspace.findChild<QPushButton*>("chartBrowserMarket_upbit")->click();QCOMPARE(search->text(),QString("ETH"));QCOMPARE(browserVisibleKeys(tree),QStringList{eth.key()});
    QCOMPARE(lists.publicState(),saved);QCOMPARE(lists.userList(id).sections.first().id,section);QCOMPARE(lists.userList(id).flag,QString("blue"));
    panel->activateWindow();search->setFocus();QTRY_VERIFY(search->hasFocus());
    QTest::keyClick(search,Qt::Key_Down);QTRY_VERIFY(search->hasFocus());QTest::keyClick(search,Qt::Key_Return);QCOMPARE(workspace.selectedChartIdentity(),eth.key());
    QCOMPARE(lists.publicState(),saved);
}
void ChartWorkspaceTest::browserMarketRestoresSearchSelectionAndScroll() {
    WatchlistWorkspaceList lists;const auto id=lists.createList("상태 복원");const WatchlistIdentity apple{"stock","US","AAPL"};lists.setVerifiedInstruments({apple});
    for(int i=0;i<50;++i)QVERIFY(lists.addIdentity(id,{"coin","upbit",QString("KRW-S%1").arg(i,2,10,QChar('0'))}));QVERIFY(lists.addIdentity(id,apple));lists.useList(id);
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.setWatchlistModel(&lists);workspace.resize(1440,790);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));

    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");auto* search=workspace.findChild<QLineEdit*>("chartWorkspaceAsset");search->setText("S");
    const QString coin="coin:upbit:KRW-S30";workspace.setIdentity(coin);auto* selected=browserItem(tree,coin);QVERIFY(selected);tree->scrollToItem(selected,QAbstractItemView::PositionAtCenter);QCoreApplication::processEvents();
    const auto scroll=tree->verticalScrollBar()->value();QVERIFY(scroll>0);
    workspace.findChild<QPushButton*>("chartBrowserMarket_us")->click();QCOMPARE(search->text(),QString{});search->setText("AAP");QTest::keyClick(search,Qt::Key_Return);QCOMPARE(workspace.selectedChartIdentity(),apple.key());
    QVERIFY(test::openInstrumentPanel(workspace));
    workspace.findChild<QPushButton*>("chartBrowserMarket_upbit")->click();QCOMPARE(search->text(),QString("S"));QCOMPARE(workspace.selectedChartIdentity(),coin);QCOMPARE(tree->verticalScrollBar()->value(),scroll);
    workspace.findChild<QPushButton*>("chartBrowserMarket_us")->click();QCOMPARE(search->text(),QString("AAP"));QCOMPARE(workspace.selectedChartIdentity(),apple.key());
}
void ChartWorkspaceTest::browserHoldingsOnlyRowsKeepDrawingsTransient() {
    class PublicTransport final:public TradingTransport{public:quint64 token=0;void send(quint64 t,const RequestSpec&)override{token=t;}void abort()override{}} transport;
    WatchlistWorkspaceList lists;const auto id=lists.createList("공개 리스트");
    const WatchlistIdentity saved{"coin","upbit","KRW-ETH"},held{"coin","upbit","KRW-HELD"};
    QVERIFY(lists.addIdentity(id,saved));lists.useList(id);
    QTemporaryDir directory;Vault vault(directory.filePath("synthetic-browser-drawing-vault"));QVERIFY(vault.create("Synthetic browser drawing password 2026!"));
    PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
    MarketCandles model(&transport);ChartWorkspace workspace(&model,&portfolio);workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);workspace.resize(1440,900);workspace.show();
    QVERIFY(test::openInstrumentPanel(workspace));
    workspace.setPrivateEnabled(true);lists.setHoldings({held});
    auto* tree=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(tree);QVERIFY(workspace.drawingModel());
    // The all scope lists holdings beside saved rows; only saved or public-catalogue
    // rows may persist drawings to the public profile (C8 drawing contract).
    for(const auto& [identity,publicAllowed]:QList<QPair<WatchlistIdentity,bool>>{{held,false},{saved,true},{held,false}}) {
        QVERIFY(test::openInstrumentPanel(workspace));
        auto* row=browserItem(tree,identity.key());QVERIFY(row);QVERIFY(!row->isHidden());
        const auto before=transport.token;emit tree->itemActivated(row,0);
        QCOMPARE(workspace.selectedChartIdentity(),identity.key());QTRY_VERIFY(transport.token&&transport.token!=before);
        const auto end=QDateTime::currentMSecsSinceEpoch()/MarketCandles::IntervalMs*MarketCandles::IntervalMs;QJsonArray rows;
        for(int i=1;i<=30;++i){const auto at=end-i*MarketCandles::IntervalMs;
            rows.append(QJsonObject{{"market",identity.code},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,Qt::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                {"timestamp",QString::number(at+1000)},{"unit","15"},{"opening_price","100"},{"high_price","104"},
                {"low_price","97"},{"trade_price","101"},{"candle_acc_trade_volume","2"}});}
        emit transport.finished(transport.token,upbit_fixture::json(rows));
        QTRY_COMPARE(workspace.drawingModel()->context().identity.key(),identity.key());
        QCOMPARE(workspace.drawingModel()->context().publicPersistenceAllowed,publicAllowed);
    }
}
void ChartWorkspaceTest::browserNarrowLayoutKeepsSearchBeforeTheChart() {
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();workspace.resize(1440,900);workspace.show();
    auto* chart=workspace.findChild<QWidget*>("chartWorkspaceChartPane");
    auto* anchor=workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* details=workspace.findChild<QWidget*>("chartWorkspacePositions");
    auto* toggle=workspace.findChild<QPushButton*>("chartDetailsToggle");
    auto* scroll=workspace.findChild<QScrollArea*>("chartWorkspaceScroll");
    QVERIFY(chart&&anchor&&details&&toggle&&scroll);QVERIFY(details->isHidden());
    auto* panel=test::openInstrumentPanel(workspace);QVERIFY(panel);
    auto* list=panel->findChild<QWidget*>("chartWorkspaceList");
    auto* search=panel->findChild<QLineEdit*>("chartWorkspaceAsset");QVERIFY(list&&search);
    const auto renders=qEnvironmentVariable("KEEFI_CHART_BROWSER_RENDER_DIR");
    for(const auto size:{QSize(1440,900),QSize(820,900),QSize(720,760),QSize(1440,900)}) {
        workspace.resize(size);QCoreApplication::processEvents();QVERIFY(workspace.width()<=size.width());QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
        QCOMPARE(test::openInstrumentPanel(workspace),panel);
        QCOMPARE(panel->width(),qMin(560,workspace.width()-32));
        QVERIFY(panel->height()<=520);
        QVERIFY(panel->mapToGlobal(QPoint{}).y()>=anchor->mapToGlobal(QPoint(0,anchor->height())).y());
        QVERIFY(list->rect().contains(QRect(search->mapTo(list,QPoint{}),search->size())));
        QVERIFY(search->isVisible());QCOMPARE(search->height(),32);
        QVERIFY(!workspace.findChild<QPushButton*>("chartBrowserJumpToList"));
        if(size.width()<1100) {
            toggle->setChecked(true);QCoreApplication::processEvents();
            QVERIFY(chart->mapToGlobal(QPoint{}).y()<details->mapToGlobal(QPoint{}).y());
            toggle->setChecked(false);
        }
        if(!renders.isEmpty()){QVERIFY(QDir().mkpath(renders));QVERIFY(panel->grab().save(renders+QString("/browser-%1.png").arg(size.width())));}
    }
}

void ChartWorkspaceTest::expandedBrowserKeepsEveryControlInsideItsPanel_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("dark");
    for (const bool dark : {false, true}) {
        for (const auto size : {QSize(1440,900), QSize(720,760), QSize(1800,1000)}) {
            const auto name=QString("%1-%2x%3").arg(dark?"dark":"light").arg(size.width()).arg(size.height()).toUtf8();
            QTest::newRow(name.constData()) << size << dark;
        }
    }
}

void ChartWorkspaceTest::expandedBrowserKeepsEveryControlInsideItsPanel() {
    QFETCH(QSize,size); QFETCH(bool,dark);
    const auto previousTheme=KeefiTheme::instance()->mode();
    const auto restoreTheme=qScopeGuard([previousTheme]{KeefiTheme::instance()->setMode(previousTheme);});
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    ChartWorkspace workspace(nullptr,nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    bindThemePresentation(&workspace,[&]{workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));});
    auto* lists=workspace.watchlistModel();
    const auto listId=lists->createList("합성 배치");
    QVERIFY(lists->addIdentity(listId,{"coin","upbit","KRW-BTC"}));
    lists->useList(listId);
    workspace.resize(size); workspace.show();
    auto* panel=test::openInstrumentPanel(workspace);QVERIFY(panel);
    auto* list=panel->findChild<QWidget*>("chartWorkspaceList");
    auto* controls=panel->findChild<QWidget*>("chartBrowserControls");
    auto* search=panel->findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* expand=panel->findChild<QPushButton*>("chartBrowserExpand");
    auto* market=panel->findChild<QPushButton*>("chartBrowserMarket_upbit");
    auto* scope=panel->findChild<QPushButton*>("chartBrowserScope_all");
    auto* selector=panel->findChild<QComboBox*>("chartWorkspaceListSelector");
    auto* tree=panel->findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(list&&controls&&search&&expand&&market&&scope&&selector&&tree);
    auto* watch=panel->findChild<QPushButton*>("chartBrowserScope_watch");QVERIFY(watch);
    watch->click();QVERIFY(tree->isVisible());
    const auto bounds=[](QWidget* child,QWidget* parent){return QRect(child->mapTo(parent,QPoint{}),child->size());};
    // Every control must fit after each popup width transition.
    for (int pass=0;pass<2;++pass) {
        expand->setChecked(true); QTest::qWait(25);
        QCOMPARE(panel->width(),qMin(760,workspace.width()-32));
        QVERIFY(panel->height()<=520);
        const auto renders=qEnvironmentVariable("KEEFI_CHART_BROWSER_RENDER_DIR");
        if(!renders.isEmpty()) {
            QVERIFY(QDir().mkpath(renders));
            QVERIFY(panel->grab().save(renders+QString("/expanded-layout-%1-%2.png").arg(QString::fromLatin1(QTest::currentDataTag())).arg(pass)));
        }
        QVERIFY2(list->height()>=list->minimumSizeHint().height(),qPrintable(QString("Expanded list is shorter than its contents: actual=%1 minimum=%2").arg(list->height()).arg(list->minimumSizeHint().height())));
        for(const auto& name:{"chartBrowserMarket_upbit","chartBrowserMarket_binance","chartBrowserMarket_kr","chartBrowserMarket_us","chartBrowserScope_all","chartBrowserScope_holdings","chartBrowserScope_watch"}) {
            auto* button=workspace.findChild<QPushButton*>(name); QVERIFY(button);
            QCOMPARE(button->height(),24);
            QVERIFY2(controls->rect().contains(bounds(button,controls)),qPrintable(QString("Control is clipped by its parent: %1").arg(name)));
        }
        QVERIFY(bounds(search,controls).bottom()<bounds(market,controls).top());
        QVERIFY(bounds(search,controls).bottom()<bounds(scope,controls).top());
        QVERIFY(bounds(selector,list).bottom()<bounds(tree,list).top());
        QVERIFY(list->rect().contains(bounds(tree,list)));
        QVERIFY(tree->height()>=128);
        QVERIFY(workspace.width()<=size.width()&&workspace.height()<=size.height());
        expand->setChecked(false); QTest::qWait(25);
        QCOMPARE(panel->width(),qMin(560,workspace.width()-32));
        QVERIFY(controls->rect().contains(bounds(scope,controls)));
        QVERIFY(workspace.width()<=size.width()&&workspace.height()<=size.height());
    }
}

void ChartWorkspaceTest::browserTabsRenderDistinctSelectionAndKeyboardFocus() {
    const auto previousTheme=KeefiTheme::instance()->mode();
    const auto restoreTheme=qScopeGuard([previousTheme]{KeefiTheme::instance()->setMode(previousTheme);});
    ChartWorkspace workspace(nullptr,nullptr);workspace.indicatorPreferences()->markIntroSeen();
    bindThemePresentation(&workspace,[&]{workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));});
    workspace.resize(1440,900);workspace.show();workspace.activateWindow();
    QVERIFY(test::openInstrumentPanel(workspace));
    auto* search=workspace.findChild<QLineEdit*>("chartWorkspaceAsset");QVERIFY(search);
    // Sample actual painted interior/edge strips, away from text and rounded
    // corners. Device pixels are used so Retina and offscreen runs agree.
    const auto paintedColor=[](QPushButton* button,bool edge) {
        const auto pixels=button->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const qreal scale=pixels.devicePixelRatio();
        const int left=qBound(0,qRound(button->width()*.25*scale),pixels.width()-1);
        const int right=qBound(left,qRound(button->width()*.75*scale),pixels.width()-1);
        const int top=edge?0:qBound(0,qRound(3*scale),pixels.height()-1);
        const int bottom=qBound(top,qRound((edge?2:5)*scale)-1,pixels.height()-1);
        qint64 red=0,green=0,blue=0,count=0;
        for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){const auto color=pixels.pixelColor(x,y);red+=color.red();green+=color.green();blue+=color.blue();++count;}
        return QColor(int(red/count),int(green/count),int(blue/count));
    };
    for(const auto mode:{ThemeMode::Light,ThemeMode::Dark})for(const int width:{1440,720}) {
        auto* all=workspace.findChild<QPushButton*>("chartBrowserScope_all");QVERIFY(all);QTest::mouseClick(all,Qt::LeftButton);
        KeefiTheme::instance()->setMode(mode);workspace.resize(width,900);QCoreApplication::processEvents();
        for(const auto& group:QList<QPair<QString,QString>>{
            {"chartBrowserMarket_upbit","chartBrowserMarket_kr"},{"chartBrowserScope_all","chartBrowserScope_holdings"}}) {
            auto* first=workspace.findChild<QPushButton*>(group.first);auto* second=workspace.findChild<QPushButton*>(group.second);
            QVERIFY(first&&second);QVERIFY(first->isVisible()&&second->isVisible());
            first->click();search->setFocus();QTRY_VERIFY(search->hasFocus());QCoreApplication::processEvents();
            QVERIFY(first->isChecked());QVERIFY(!second->isChecked());
            const auto selected=paintedColor(first,false),ordinary=paintedColor(second,false);
            QVERIFY2(KeefiTheme::contrast(selected,ordinary)>=3.0,qPrintable(QString("Selected tab is not visibly distinct: theme=%1 width=%2 group=%3 colors=%4/%5").arg(int(mode)).arg(width).arg(group.first,selected.name(),ordinary.name())));
            second->click();search->setFocus();QTRY_VERIFY(search->hasFocus());QCoreApplication::processEvents();
            QVERIFY(second->isChecked());QVERIFY(!first->isChecked());
            QVERIFY2(KeefiTheme::contrast(paintedColor(first,false),paintedColor(second,false))>=3.0,"Moving the selection must move the visible contrast");
            const auto beforeFocus=paintedColor(second,true);
            second->setFocus(Qt::TabFocusReason);QTRY_VERIFY(second->hasFocus());QCoreApplication::processEvents();
            const auto focused=paintedColor(second,true);
            const int difference=qAbs(beforeFocus.red()-focused.red())+qAbs(beforeFocus.green()-focused.green())+qAbs(beforeFocus.blue()-focused.blue());
            QVERIFY2(difference>=64,qPrintable(QString("Keyboard focus border is not visibly distinct: %1/%2").arg(beforeFocus.name(),focused.name())));
            QVERIFY(second->isChecked());QVERIFY(!first->isChecked());
            QCOMPARE(first->height(),24);QCOMPARE(second->height(),24);
        }
    }
}

void ChartWorkspaceTest::listNameTooltipKeepsMarkupLiteral() {
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.resize(1280, 900);
    workspace.show();
    auto* lists = workspace.watchlistModel();
    QVERIFY(lists);
    const auto name = QStringLiteral("<b>합성 관심 목록</b> & literal");
    const auto id = lists->createList(name, QStringLiteral("blue"));
    QVERIFY(!id.isEmpty());
    auto* selector = workspace.findChild<QComboBox*>("chartWorkspaceListSelector");
    QVERIFY(selector);
    const int index = selector->findData(id);
    QVERIFY(index >= 0);
    selector->setCurrentIndex(index);
    QCOMPARE(lists->userList(id).name, name);
    QCOMPARE(selector->currentText(), name);
    const auto shown = keefi::test::projectTooltip(selector->toolTip());
    QCOMPARE(shown.text, name);
    QVERIFY(!shown.image);
    QVERIFY(!shown.anchor);
    QCOMPARE(shown.resourceRequests, 0);
}
QTEST_MAIN(ChartWorkspaceTest)
#include "tst_chart_workspace.moc"
