set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_UNITY_BUILD OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(KEEFI_BUILD_TESTS "Build the isolated KEEFI verification suite" ON)
# Financial execution remains absent regardless of this compatibility setting.
# The KEEFI build uses a fresh cache and never imports prior option values.
set(KEEFI_READ_ONLY_BETA ON CACHE BOOL "Deprecated compatibility setting; KEEFI is permanently read-only" FORCE)
add_compile_definitions(KEEFI_READ_ONLY_BETA=1)
option(KEEFI_ENABLE_CRASH_PROBES "Run intentional child crashes (may show OS crash dialogs)" OFF)
set(KEEFI_SODIUM_ROOT "" CACHE PATH "libsodium 1.0.22 installation prefix")
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
if(APPLE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
    endif()
    enable_language(OBJCXX)
endif()
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
find_package(ZLIB REQUIRED)
if(NOT WIN32)
    find_package(Iconv REQUIRED)
endif()
# Qt 6.8.3 adds AGL even when the selected macOS SDK has removed it.
# KEEFI uses QWidget rendering and has no AGL calls. Keep OpenGL unchanged.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    set(_keefi_sdk "${CMAKE_OSX_SYSROOT}")
    if(NOT IS_ABSOLUTE "${_keefi_sdk}")
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE _keefi_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    endif()
    if(NOT EXISTS "${_keefi_sdk}/System/Library/Frameworks/AGL.framework")
        get_target_property(_keefi_gl_links WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
        list(FILTER _keefi_gl_links EXCLUDE REGEX "(^-framework AGL$|/AGL\\.framework(/AGL)?$)")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${_keefi_gl_links}")
        message(STATUS "KEEFI: omitted Qt's obsolete AGL link for this macOS SDK")
    endif()
endif()
find_path(KEEFI_SODIUM_INCLUDE_DIR sodium.h HINTS "${KEEFI_SODIUM_ROOT}/include" REQUIRED)
find_library(KEEFI_SODIUM_LIBRARY NAMES sodium libsodium HINTS "${KEEFI_SODIUM_ROOT}/lib" REQUIRED)
file(STRINGS "${KEEFI_SODIUM_INCLUDE_DIR}/sodium/version.h" _keefi_sodium_version
    REGEX "^#define SODIUM_VERSION_STRING ")
if(NOT _keefi_sodium_version MATCHES "\"1\\.0\\.22\"")
    message(FATAL_ERROR "KEEFI G1 pins libsodium 1.0.22; found ${_keefi_sodium_version}")
endif()
add_library(keefi_sodium UNKNOWN IMPORTED)
set_target_properties(keefi_sodium PROPERTIES IMPORTED_LOCATION "${KEEFI_SODIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${KEEFI_SODIUM_INCLUDE_DIR}")
if(WIN32)
    set_property(TARGET keefi_sodium PROPERTY INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC)
endif()

add_library(keefi_vault STATIC src/keefi/SecureBuffer.cpp src/keefi/Vault.cpp src/keefi/AccountModel.cpp)
target_sources(keefi_vault PRIVATE src/keefi/ProfilePaths.cpp src/keefi/ProfileMigration.cpp src/keefi/ProfileSettings.cpp
    src/keefi/IdentityCompatibility.cpp src/keefi/IdentityCompatibility.h)
target_sources(keefi_vault PRIVATE src/keefi/ProfileUpgrade.cpp src/keefi/ProfileUpgrade.h)
target_sources(keefi_vault PRIVATE src/keefi/ProfileRestore.cpp src/keefi/ProfileRestore.h)
target_sources(keefi_vault PRIVATE src/keefi/PublicFormatMigration.cpp src/keefi/PublicFormatMigration.h)
target_sources(keefi_vault PRIVATE src/keefi/LegacyStorageCodecPrivate.cpp src/keefi/LegacyStorageCodecPrivate.h)
target_include_directories(keefi_vault PUBLIC src)
target_link_libraries(keefi_vault PUBLIC Qt6::Core keefi_sodium)

add_library(keefi_notifications STATIC src/keefi/SystemNotificationSink.cpp src/keefi/SystemNotificationSink.h
    src/keefi/SystemNotificationSink_p.h)
target_include_directories(keefi_notifications PUBLIC src)
target_link_libraries(keefi_notifications PUBLIC Qt6::Core)
if(APPLE)
    target_sources(keefi_notifications PRIVATE src/keefi/MacSystemNotificationSink.mm)
    find_library(KEEFI_USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
    target_link_libraries(keefi_notifications PRIVATE "${KEEFI_USERNOTIFICATIONS_FRAMEWORK}" "-framework Foundation")
    set_source_files_properties(src/keefi/MacSystemNotificationSink.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(keefi_notifications PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
endif()

add_library(keefi_security STATIC src/keefi/PlatformSecurity.cpp src/keefi/SecretWorker.cpp)
target_include_directories(keefi_security PUBLIC src)
target_link_libraries(keefi_security PUBLIC Qt6::Widgets)
if(APPLE)
    target_sources(keefi_security PRIVATE src/keefi/PlatformSecurity.mm)
    target_link_libraries(keefi_security PRIVATE "-framework AppKit" "-framework IOKit")
endif()
if(WIN32)
    target_sources(keefi_vault PRIVATE src/keefi/ProfileUpgradeWindows.cpp)
    target_link_libraries(keefi_security PRIVATE kernel32 wtsapi32 advapi32)
    target_link_libraries(keefi_vault PRIVATE advapi32)
endif()

add_library(keefi_trading STATIC src/keefi/RecordTransfer.cpp src/keefi/RecordTransfer.h src/keefi/PublicKrwPrices.cpp src/keefi/PublicKrwPrices.h src/keefi/PublicEquityBoard.cpp src/keefi/PublicEquityBoard.h src/keefi/PublicMarketCatalog.cpp src/keefi/PublicMarketCatalog.h src/keefi/PortfolioController.cpp src/keefi/PortfolioController.h src/keefi/AssetsCapture.cpp src/keefi/AssetsLedger.cpp src/keefi/AssetsLedger.h src/keefi/AssetValuation.cpp src/keefi/TradingTypes.cpp src/keefi/TradingTransport.cpp
    src/keefi/TradingController.cpp src/keefi/BithumbAdapter.cpp src/keefi/OkxAdapter.cpp src/keefi/OkxProtocol.cpp src/keefi/OkxOrderEvents.cpp src/keefi/OkxOrderEvents.h src/keefi/UpbitAdapter.cpp src/keefi/BinanceAdapter.cpp
    src/keefi/TossAdapter.cpp src/keefi/KisAdapter.cpp src/keefi/KiwoomAdapter.cpp src/keefi/LsAdapter.cpp src/keefi/KisMasterCatalog.cpp
    src/keefi/BrokerTradingController.cpp src/keefi/BrokerTradingController.h
    src/keefi/BinanceUsdmAdapter.cpp src/keefi/FuturesTradingController.cpp src/keefi/FuturesTradingController.h
    src/keefi/SolanaTransactionCodec.cpp src/keefi/SolanaReadRpc.cpp src/keefi/SolanaReadRpc.h
    src/keefi/SwapJournal.cpp src/keefi/SwapJournal.h
    src/keefi/PriceArchive.cpp src/keefi/PriceArchive.h
    src/keefi/WalletReadController.cpp src/keefi/WalletReadController.h
    src/keefi/SolanaTokenData.cpp src/keefi/SolanaTokenData.h)
target_include_directories(keefi_trading PUBLIC src)
target_sources(keefi_trading PRIVATE src/keefi/StorageMigration.cpp src/keefi/StorageMigration.h)
target_sources(keefi_trading PRIVATE src/keefi/PublicQuoteStream.cpp src/keefi/PublicQuoteStream.h)
target_sources(keefi_trading PRIVATE src/keefi/PublicMarketHistory.cpp src/keefi/PublicMarketHistory.h)
target_sources(keefi_trading PRIVATE src/keefi/BinanceBalanceStream.cpp src/keefi/BinanceBalanceStream.h)
target_sources(keefi_trading PRIVATE src/keefi/BinanceEarnProtocol.cpp src/keefi/BinanceEarnProtocol.h
    src/keefi/BinanceEarnRequests.h src/keefi/EarnTracking.cpp src/keefi/EarnTracking.h
    src/keefi/BinanceEarnCollector.cpp src/keefi/BinanceEarnCollector.h)
target_sources(keefi_trading PRIVATE src/keefi/FundFlowRequests.h src/keefi/FundFlowProtocol.cpp src/keefi/FundFlowProtocol.h
    src/keefi/FundFlowTracking.cpp src/keefi/FundFlowTracking.h src/keefi/FundFlowCollector.cpp src/keefi/FundFlowCollector.h
    src/keefi/CoinPerformance.cpp src/keefi/CoinPerformance.h)
target_sources(keefi_trading PRIVATE src/keefi/ResponseJournal.cpp src/keefi/ResponseJournal.h)
target_sources(keefi_trading PRIVATE src/keefi/ValuationPreferences.cpp src/keefi/ValuationPreferences.h)
target_sources(keefi_trading PRIVATE src/keefi/OkxFuturesReadController.cpp src/keefi/OkxFuturesReadController.h)
target_sources(keefi_trading PRIVATE src/keefi/HyperliquidReadController.cpp src/keefi/HyperliquidReadController.h)
target_sources(keefi_trading PRIVATE src/keefi/ProviderReconciliation.cpp src/keefi/ProviderReconciliation.h)
target_sources(keefi_trading PRIVATE src/keefi/PortfolioComparison.cpp src/keefi/PortfolioComparison.h)
target_sources(keefi_trading PRIVATE src/keefi/PortfolioCheckpoint.cpp src/keefi/PortfolioCheckpoint.h)
target_sources(keefi_trading PRIVATE src/keefi/PortfolioHistory.cpp src/keefi/PortfolioHistory.h)
target_sources(keefi_trading PRIVATE src/keefi/MarketMetrics.cpp src/keefi/MarketMetrics.h)
target_sources(keefi_trading PRIVATE src/keefi/TodayBtcPremium.cpp src/keefi/TodayBtcPremium.h)
target_sources(keefi_trading PRIVATE src/keefi/WorldMarkets.cpp src/keefi/WorldMarkets.h)
target_sources(keefi_trading PRIVATE src/keefi/MarketCompare.cpp src/keefi/MarketCompare.h
    src/keefi/MarketComparePeriod.cpp src/keefi/MarketComparePeriod.h
    src/keefi/MarketCompareSource.cpp src/keefi/MarketCompareSource.h
    src/keefi/FxHistory.cpp src/keefi/FxHistory.h
    src/keefi/CollectorStore.cpp src/keefi/CollectorStore.h)
target_sources(keefi_trading PRIVATE src/keefi/CoinbaseQuotes.cpp src/keefi/CoinbaseQuotes.h)
target_sources(keefi_trading PRIVATE src/keefi/OfficialCalendar.cpp src/keefi/OfficialCalendar.h)
target_sources(keefi_trading PRIVATE src/keefi/EarningsCalendar.cpp src/keefi/EarningsCalendar.h)
target_sources(keefi_trading PRIVATE src/keefi/OfficialNotices.cpp src/keefi/OfficialNotices.h)
target_sources(keefi_trading PRIVATE src/keefi/UpbitAnnouncements.cpp src/keefi/UpbitAnnouncements.h)
target_sources(keefi_trading PRIVATE src/keefi/MarketCandles.cpp src/keefi/MarketCandles.h
    src/keefi/ChartMarketResolver.cpp src/keefi/ChartMarketResolver.h)
target_sources(keefi_trading PRIVATE src/keefi/CandleTradeOverlay.cpp src/keefi/CandleTradeOverlay.h)
target_sources(keefi_trading PRIVATE src/keefi/NetworkPublicCandleSocket.cpp src/keefi/NetworkPublicCandleSocket.h)
target_sources(keefi_trading PRIVATE src/keefi/MarketStudies.cpp src/keefi/MarketStudies.h)
target_sources(keefi_trading PRIVATE src/keefi/CandleAggregation.cpp src/keefi/CandleAggregation.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudyPreferences.cpp src/keefi/ChartStudyPreferences.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudies.cpp src/keefi/ChartStudies.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartIndicatorState.cpp src/keefi/ChartIndicatorState.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudyHelp.cpp src/keefi/ChartStudyHelp.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudySearch.cpp src/keefi/ChartStudySearch.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudyFeed.cpp src/keefi/ChartStudyFeed.h)
target_sources(keefi_trading PRIVATE src/keefi/RankingStudies.cpp src/keefi/RankingStudies.h
    src/keefi/RankingStudyRunner.cpp src/keefi/RankingStudyRunner.h
    src/keefi/RankingFuturesFeed.cpp src/keefi/RankingFuturesStudies.cpp src/keefi/RankingFuturesFeed.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudyEventMonitor.cpp src/keefi/ChartStudyEventMonitor.h)
target_sources(keefi_trading PRIVATE src/keefi/RapidMoveMonitor.cpp src/keefi/RapidMoveMonitor.h)
target_sources(keefi_trading PRIVATE src/keefi/MarketBreadthMonitor.cpp src/keefi/MarketBreadthMonitor.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartStudyRequestPacer.cpp src/keefi/ChartStudyRequestPacer.h)
target_sources(keefi_trading PRIVATE src/keefi/ExtendedChartStudies.cpp src/keefi/ExtendedChartStudies.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartPublicContextSource.cpp src/keefi/ChartPublicContextRequests.cpp src/keefi/ChartPublicContextSource.h)
target_sources(keefi_trading PRIVATE src/keefi/CoinFlowStudyRunner.cpp src/keefi/CoinFlowStudyRunner.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartDrawings.cpp src/keefi/ChartDrawings.h
    src/keefi/AutoChartGeometry.cpp src/keefi/AutoChartGeometry.h
    src/keefi/CoinFlowStudies.cpp src/keefi/CoinFlowStudies.h
    src/keefi/CoinFlowProtocol.cpp src/keefi/CoinFlowProtocol.h
    src/keefi/CoinFlowSource.cpp src/keefi/CoinFlowSource.h
    src/keefi/NetworkPublicFlowSocket.cpp src/keefi/NetworkPublicFlowSocket.h
    src/keefi/PublicFlowFrameQueue.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartAlertFeed.cpp src/keefi/ChartAlertFeed.h)
target_sources(keefi_trading PRIVATE src/keefi/StockChartTypes.h src/keefi/StockChartRules.cpp src/keefi/StockChartRules.h
    src/keefi/StockChartSource.cpp src/keefi/StockChartSource.h
    src/keefi/StockPeriodLevels.cpp src/keefi/StockPeriodLevels.h)
target_sources(keefi_trading PRIVATE src/keefi/ChartAlertEngine.cpp src/keefi/ChartAlertEngine.h
    src/keefi/ChartPhrasePack.cpp src/keefi/ChartPhrasePack.h resources/keefi_phrases.qrc)
target_link_libraries(keefi_trading PUBLIC keefi_notifications)
target_sources(keefi_trading PRIVATE src/keefi/WatchlistWorkspaceList.cpp src/keefi/WatchlistWorkspaceList.h
    src/keefi/ChartInstrumentCatalog.cpp src/keefi/ChartInstrumentCatalog.h)
target_sources(keefi_trading PRIVATE src/keefi/PortfolioContribution.cpp src/keefi/PortfolioContribution.h)
target_sources(keefi_trading PRIVATE src/keefi/MarketOverview.cpp src/keefi/MarketOverview.h)
target_sources(keefi_trading PRIVATE src/keefi/TossMarketStream.cpp src/keefi/TossMarketStream.h)
target_sources(keefi_trading PRIVATE src/keefi/KiwoomRequestBudget.cpp src/keefi/KiwoomRequestBudget.h
    src/keefi/KiwoomMarketTypes.h src/keefi/KiwoomMarketProtocol.cpp src/keefi/KiwoomMarketProtocol.h
    src/keefi/KiwoomMarketStream.cpp src/keefi/KiwoomMarketStream.h src/keefi/KiwoomMarketSocket.cpp)
target_link_libraries(keefi_trading PUBLIC keefi_vault Qt6::Widgets Qt6::Network Qt6::WebSockets ZLIB::ZLIB)
if(NOT WIN32)
    target_link_libraries(keefi_trading PRIVATE Iconv::Iconv)
endif()

add_library(keefi_ui STATIC src/keefi/AssetsWindow.cpp src/keefi/AssetsWindow.h src/keefi/LocalWindow.cpp src/keefi/AccountDialog.cpp src/keefi/VaultDialog.cpp src/keefi/PressRevealButton.cpp src/keefi/PressRevealButton.h src/keefi/SegmentedComboBox.h src/keefi/KeefiVisuals.h src/keefi/MarketMap.cpp src/keefi/MarketMap.h
    src/keefi/Spotlight.cpp src/keefi/Spotlight.h src/keefi/ExploreWatchlist.cpp src/keefi/ExploreWatchlist.h
    src/keefi/RecordTransferDialog.cpp src/keefi/RecordTransferDialog.h
    src/keefi/AssetListFilter.cpp src/keefi/AssetListFilter.h
    src/keefi/PortfolioPresentation.cpp src/keefi/PortfolioPresentation.h src/keefi/PortfolioDetail.cpp src/keefi/PortfolioDetail.h
    src/keefi/PortfolioAllocation.cpp src/keefi/PortfolioAllocation.h
    src/keefi/ConnectionPresentation.cpp src/keefi/ConnectionPresentation.h
    src/keefi/ValuationBundle.cpp src/keefi/ValuationBundle.h
    src/keefi/LocalComboBox.h src/keefi/LocalTableWidget.h)
target_sources(keefi_ui PRIVATE
    src/keefi/LocalWindowPortfolio.cpp
    src/keefi/PortfolioAssetModel.cpp src/keefi/PortfolioAssetModel.h
    src/keefi/LocalWindowExplore.cpp
    src/keefi/LocalWindowCharts.cpp
    src/keefi/LocalWindowSession.cpp
    src/keefi/LocalWindowOptions.cpp
    src/keefi/LocalWindowHelpers.cpp src/keefi/LocalWindowHelpers_p.h)
target_sources(keefi_ui PRIVATE src/keefi/KeefiTheme.cpp src/keefi/KeefiTheme.h
    src/keefi/KeefiFonts.cpp src/keefi/KeefiFonts.h resources/keefi_fonts.qrc resources/keefi_notices.qrc)
target_sources(keefi_ui PRIVATE src/keefi/VaultGate.cpp src/keefi/VaultGate.h
    src/keefi/VaultMark.cpp src/keefi/VaultMark.h
    src/keefi/Motion.h src/keefi/LoadProgressBar.cpp src/keefi/LoadProgressBar.h)
target_sources(keefi_ui PRIVATE src/keefi/ProfileMigrationDialog.cpp src/keefi/ProfileMigrationDialog.h)
target_sources(keefi_ui PRIVATE src/keefi/ProfileUpgradeDialog.cpp src/keefi/ProfileUpgradeDialog.h)
target_sources(keefi_ui PRIVATE src/keefi/PortfolioDayView.cpp src/keefi/PortfolioDayView.h)
target_sources(keefi_ui PRIVATE src/keefi/ChartShareCard.cpp src/keefi/ChartShareCard.h)
target_sources(keefi_ui PRIVATE src/keefi/ChartShareDialog.cpp src/keefi/ChartShareDialog.h)
target_sources(keefi_ui PRIVATE src/keefi/ChartInstrumentPresentation.cpp src/keefi/ChartInstrumentPresentation.h)
target_sources(keefi_ui PRIVATE src/keefi/ChartInstrumentPicker.cpp src/keefi/ChartInstrumentPicker.h)
target_sources(keefi_ui PRIVATE src/keefi/MovementDirection.h src/keefi/MovementColorPolicy.cpp src/keefi/MovementColorPolicy.h
    src/keefi/InfoButton.cpp src/keefi/InfoButton.h src/keefi/ExploreMetricCard.cpp src/keefi/ExploreMetricCard.h)
target_sources(keefi_ui PRIVATE src/keefi/MarketMetricsView.cpp src/keefi/MarketMetricsView.h src/keefi/ExploreDashboard.cpp src/keefi/ExploreDashboard.h src/keefi/TodayCalendarView.cpp src/keefi/TodayCalendarView.h)
target_sources(keefi_ui PRIVATE src/keefi/TossMarketView.cpp src/keefi/TossMarketView.h)
target_sources(keefi_ui PRIVATE src/keefi/KiwoomMarketView.cpp src/keefi/KiwoomMarketView.h)
target_sources(keefi_ui PRIVATE src/keefi/OfficialCalendarView.cpp src/keefi/OfficialCalendarView.h)
target_sources(keefi_ui PRIVATE src/keefi/EarningsCalendarView.cpp src/keefi/EarningsCalendarView.h)
target_sources(keefi_ui PRIVATE src/keefi/OfficialNoticesView.cpp src/keefi/OfficialNoticesView.h)
target_sources(keefi_ui PRIVATE src/keefi/MarketOverviewView.cpp src/keefi/MarketOverviewView.h)
target_sources(keefi_ui PRIVATE src/keefi/MarketCandlesView.cpp src/keefi/MarketCandlesView.h)
target_sources(keefi_ui PRIVATE src/keefi/MarketCandlesViewLive.cpp)
target_sources(keefi_ui PRIVATE
    src/keefi/CandleChart.cpp src/keefi/CandleChart_p.h
    src/keefi/MarketCandlesViewDrawings.cpp
    src/keefi/MarketCandlesViewIndicatorPicker.cpp
    src/keefi/MarketCandlesViewFormatting.cpp src/keefi/MarketCandlesViewFormatting_p.h)
target_sources(keefi_ui PRIVATE src/keefi/ChartMarketContext.cpp src/keefi/ChartMarketContext.h)
target_sources(keefi_ui PRIVATE src/keefi/PublicCoinIdentity.cpp src/keefi/PublicCoinIdentity.h src/keefi/HoldingMarketResolver.cpp src/keefi/HoldingMarketResolver.h src/keefi/AssetIdentity.cpp src/keefi/AssetIdentity.h src/keefi/HoldingChartPanel.cpp src/keefi/HoldingChartPanel.h src/keefi/ChartGridView.cpp src/keefi/ChartGridView.h src/keefi/AssetShareCard.cpp src/keefi/AssetShareCard.h src/keefi/AssetShareDialog.cpp src/keefi/AssetShareDialog.h src/keefi/ChartWorkspace.cpp src/keefi/ChartWorkspace.h
    src/keefi/ChartMarketLedger.cpp src/keefi/ChartMarketLedger.h)
target_sources(keefi_ui PRIVATE
    src/keefi/ChartWorkspacePositions.cpp
    src/keefi/ChartWorkspaceStudies.cpp
    src/keefi/ChartWorkspaceBrowser.cpp
    src/keefi/ChartWorkspaceWatchlists.cpp
    src/keefi/ChartWorkspacePresentation.cpp src/keefi/ChartWorkspacePresentation_p.h)
target_sources(keefi_ui PRIVATE src/keefi/InstrumentListTree.cpp src/keefi/InstrumentListTree.h src/keefi/InstrumentListSelector.cpp src/keefi/InstrumentListSelector.h src/keefi/ChartWatchlistInteractions.cpp)
target_link_libraries(keefi_ui PUBLIC keefi_vault keefi_trading Qt6::Widgets)
target_sources(keefi_ui PRIVATE src/keefi/ChartAlertsPanel.cpp src/keefi/ChartAlertsPanel.h)
target_sources(keefi_ui PRIVATE src/keefi/MarketCompareView.cpp src/keefi/MarketCompareView.h)
target_sources(keefi_ui PRIVATE src/keefi/BackgroundTray.cpp src/keefi/BackgroundTray.h)
target_sources(keefi_ui PRIVATE src/keefi/ChartTimeAxis.cpp src/keefi/ChartTimeAxis.h)
target_include_directories(keefi_ui PUBLIC src)

# Headless public-market collector (COLLECTOR-COMPARE-RULES-2026-11-06.md). Not bundled into the app.
execute_process(COMMAND git describe --always --dirty --abbrev=8
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _keefi_collector_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _keefi_collector_revision)
    set(_keefi_collector_revision "unknown")
endif()
add_executable(keefi-collector src/keefi/collector/collector_main.cpp
    src/keefi/collector/CollectorRuntime.cpp src/keefi/collector/CollectorRuntime.h)
target_link_libraries(keefi-collector PRIVATE keefi_trading Qt6::Core Qt6::Network Qt6::WebSockets)
target_include_directories(keefi-collector PRIVATE src)
target_compile_definitions(keefi-collector PRIVATE KEEFI_COLLECTOR_REVISION="coinflow@${_keefi_collector_revision}")
set(KEEFI_RELEASE_VERSION "0.18.0")
set(KEEFI_RELEASE_BUILD "111")
qt_add_executable(KEEFI MACOSX_BUNDLE WIN32 src/keefi/main.cpp)
target_link_libraries(KEEFI PRIVATE keefi_ui keefi_security)
target_compile_definitions(KEEFI PRIVATE KEEFI_APPLICATION_VERSION="${KEEFI_RELEASE_VERSION}")
set_target_properties(KEEFI PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "KEEFI"
    MACOSX_BUNDLE_GUI_IDENTIFIER "xyz.beamgrove.keefi.local"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${KEEFI_RELEASE_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${KEEFI_RELEASE_BUILD}"
    MACOSX_BUNDLE_COPYRIGHT "KEEFI contributors; upstream Fincept notices retained")
if(WIN32)
    enable_language(RC)
    string(REPLACE "." "," KEEFI_WINDOWS_VERSION "${KEEFI_RELEASE_VERSION}.${KEEFI_RELEASE_BUILD}")
    configure_file(resources/keefi_windows.rc.in "${CMAKE_CURRENT_BINARY_DIR}/keefi_windows.rc" @ONLY)
    target_sources(KEEFI PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/keefi_windows.rc")
endif()
qt_add_executable(KEEFI_Dev MACOSX_BUNDLE WIN32 src/keefi/main.cpp)
target_link_libraries(KEEFI_Dev PRIVATE keefi_ui keefi_security)
target_compile_definitions(KEEFI_Dev PRIVATE KEEFI_DEVELOPMENT_BUILD=1
    KEEFI_APPLICATION_VERSION="${KEEFI_RELEASE_VERSION}-g8-preview-dev")
set_target_properties(KEEFI_Dev PROPERTIES
    OUTPUT_NAME "KEEFI Dev"
    MACOSX_BUNDLE_BUNDLE_NAME "KEEFI Dev"
    MACOSX_BUNDLE_GUI_IDENTIFIER "xyz.beamgrove.keefi.dev"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${KEEFI_RELEASE_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "110"
    MACOSX_BUNDLE_COPYRIGHT "KEEFI contributors; upstream Fincept notices retained")
if(APPLE)
    # Only the two installed channel bundles belong in the user's app list.
    set_target_properties(KEEFI KEEFI_Dev PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/.app-staging.noindex")
endif()
foreach(_target keefi_vault keefi_security keefi_trading keefi_ui KEEFI KEEFI_Dev)
    if(MSVC)
        target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->")
    else()
        target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wall;-Wextra;-Wpedantic>")
    endif()
endforeach()

if(KEEFI_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    add_subdirectory(tests/keefi)
endif()

target_sources(keefi_trading PRIVATE src/keefi/MarketFlowHistory.cpp src/keefi/MarketFlowHistory.h)
