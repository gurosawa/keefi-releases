#include "keefi/PortfolioDayView.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/AccountModel.h"
#include "keefi/AssetsLedger.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketMetrics.h"
#include "keefi/MarketOverviewView.h"
#include "keefi/OfficialCalendarView.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/OfficialNoticesView.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/InfoButton.h"
#include "keefi/Motion.h"
#include "keefi/PortfolioAllocation.h"
#include <QAbstractAnimation>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include "keefi/TossMarketStream.h"
#include "keefi/TossMarketView.h"
#include <QStackedWidget>
#include <QScrollArea>
#include "keefi/PortfolioController.h"
#include "keefi/Vault.h"
#include "fixtures/ProviderReadFixture.h"
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScopeGuard>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantAnimation>
#include <QVBoxLayout>
using namespace keefi;
class PortfolioDayViewTest : public QObject {
    Q_OBJECT
  private slots:
    void observedChangeShowsActualBaseline();
    void oneObservationDoesNotPretendToBeAFlow();
    void missingBaselineNeverLabelsMidnightAsAnObservation();
    void partialReasonsAndPrivateContentClear();
    void chartBreaksGapsAndUsesActualTime();
    void localWindowConnectsHistoryAndErasesImmediatelyOnLock();
    void contributionShowsResidualAndClearsWithLock();
    void publicMarketFlowIsSeparateFromPrivatePortfolio();
    void contributionAmountsAreLedgerRows();
    void computedAmountsKeepEvidenceInputsWithoutShowingReferenceLabels();
    void dollarChangeAndContributionsKeepRawHistoryAndChart_data();
    void dollarChangeAndContributionsKeepRawHistoryAndChart();
    void dollarAllocationChangesOnlyAccessibleAmounts_data();
    void dollarAllocationChangesOnlyAccessibleAmounts();
    void allocationAnimationOnlyStartsOnCompositionChange();
    void flowOpensOnRequest();
    void expandedRereadKeepsTheHoldingsPositionUntilItSettles();
};
void PortfolioDayViewTest::dollarAllocationChangesOnlyAccessibleAmounts_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void PortfolioDayViewTest::dollarAllocationChangesOnlyAccessibleAmounts(){
    KeefiFonts::registerBundledFonts();QFETCH(bool,dark);auto* theme=KeefiTheme::instance();theme->setSettings(nullptr);const auto original=theme->mode();
    const auto restore=qScopeGuard([=]{theme->setMode(original);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    PortfolioHoldingView coin;coin.asset="SYN";coin.quantity="1";coin.value="1250000";coin.state=PortfolioValueState::Fresh;
    PortfolioGroupView a;a.identity="exchange:SYN";a.asset="SYN";a.freshTotal="1250000";a.holdings={coin};
    auto cash=coin;cash.asset="KRW";cash.quantity="1250000";cash.kind=AssetComponentKind::Cash;
    PortfolioGroupView b;b.identity="cash:KRW";b.asset="KRW";b.freshTotal="1250000";b.holdings={cash};
    const QList<PortfolioGroupView> inputs{a,b};PortfolioAllocation view;view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.resize(600,100);view.show();view.setGroups(inputs);
    if (auto* motion = view.findChild<QVariantAnimation*>()) {
        QTRY_COMPARE_WITH_TIMEOUT(motion->state(), QAbstractAnimation::Stopped, 2000);
    }
    QCoreApplication::processEvents();
    const auto before=view.grab().toImage();const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{{"USD","KRW","1250",{"Synthetic FX","https://example.invalid/USD-KRW","public:synthetic:USD-KRW",now,now,true}}},now);
    view.setGroups(inputs,display);QCoreApplication::processEvents();
    QCOMPARE(view.accessibleDescription().count(QStringLiteral("1,000.00 USD")),2);QVERIFY(view.accessibleDescription().contains(QStringLiteral("50.0%")));
    QVERIFY(view.toolTip().contains(QStringLiteral("1,000.00 USD")));QVERIFY(!view.toolTip().contains(QStringLiteral("1,250,000 KRW")));
    QCOMPARE(view.grab().toImage(),before);QCOMPARE(a.freshTotal,QStringLiteral("1250000"));QCOMPARE(coin.value,QStringLiteral("1250000"));
    view.setGroups(inputs,PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{},now));
    QCOMPARE(view.accessibleDescription().count(QStringLiteral("1,250,000 KRW")),2);QVERIFY(view.accessibleDescription().contains(QStringLiteral("환율 불러오는 중")));
    QVERIFY(view.toolTip().contains(QStringLiteral("환율 불러오는 중")));view.setGroups({});QVERIFY(view.accessibleDescription().isEmpty());QVERIFY(!view.toolTip().contains("1,250,000"));
}
void PortfolioDayViewTest::dollarChangeAndContributionsKeepRawHistoryAndChart_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void PortfolioDayViewTest::dollarChangeAndContributionsKeepRawHistoryAndChart(){
    KeefiFonts::registerBundledFonts();QFETCH(bool,dark);auto* theme=KeefiTheme::instance();theme->setSettings(nullptr);const auto original=theme->mode();
    const auto restore=qScopeGuard([=]{theme->setMode(original);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    PortfolioDayHistory history;const qint64 from=1792598400000,to=1792602000000;
    history.comparison={true,true,from,to,0,"KRW","1250000","1500000","250000",QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"),{}};
    history.contributions.available=true;history.contributions.groups={{PortfolioContributionClass::Coin,"875000","1187500","312500",1},{PortfolioContributionClass::Unclassified,"375000","312500","-62500",1}};
    history.points={{from,true,true,false,"1250000",{}},{to,true,true,true,"1500000",{}}};history.closingValuation.knownSubtotal="1500000";
    PortfolioDayView view;view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setDetailsExpanded(true);view.resize(600,360);view.show();view.setHistory(history);QCoreApplication::processEvents();
    auto* chart=view.findChild<QWidget*>("portfolioDayChart");QVERIFY(chart);const auto chartBefore=chart->grab().toImage();
    const auto now=QDateTime::currentMSecsSinceEpoch();const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{{"USD","KRW","1250",{"Synthetic FX","https://example.invalid/USD-KRW","public:synthetic:USD-KRW",now,now,true}}},now);
    view.setHistory(history,display);QCoreApplication::processEvents();
    QCOMPARE(view.findChild<QLabel*>("portfolioDayChange")->text(),QStringLiteral("▲ +200.00 USD"));
    QCOMPARE(view.findChild<QLabel*>("portfolioContributionValue_0")->text(),QStringLiteral("▲ +250.00"));
    QCOMPARE(view.findChild<QLabel*>("portfolioContributionValue_4")->text(),QStringLiteral("▼ -50.00"));
    QCOMPARE(view.findChild<QLabel*>("portfolioContributionUnit_0")->text(),QStringLiteral("USD"));
    QVERIFY(view.accessibleDescription().contains(QStringLiteral("200.00 USD")));
    QVERIFY(view.findChild<QWidget*>("portfolioContributionBars")->accessibleDescription().contains(QStringLiteral("250.00 USD")));
    QCOMPARE(chart->grab().toImage(),chartBefore);QCOMPARE(history.comparison.difference,QStringLiteral("250000"));
    QCOMPARE(history.contributions.groups.first().difference,QStringLiteral("312500"));QCOMPARE(history.points.first().value,QStringLiteral("1250000"));QCOMPARE(history.closingValuation.knownSubtotal,QStringLiteral("1500000"));
    const auto out=qEnvironmentVariable("KEEFI_P3_WIDGET_RENDER_DIR");if(!out.isEmpty()){QVERIFY(QDir().mkpath(out));QVERIFY(view.grab().save(out+(dark?"/day-usd-dark.png":"/day-usd-light.png")));}
    auto* info=view.findChild<InfoButton*>("portfolioDayInfo");QVERIFY(info);info->openInformation();
    QPointer<QPlainTextEdit> privateText=view.findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(privateText);
    QVERIFY(privateText->toPlainText().contains(QStringLiteral("250.00 USD")));QVERIFY(privateText->toPlainText().contains(display.information()));
    const auto pending=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{},now);view.setHistory(history,pending);QCoreApplication::processEvents();
    QCOMPARE(view.findChild<QLabel*>("portfolioDayChange")->text(),QStringLiteral("▲ +250,000 KRW"));
    auto* status=view.findChild<QLabel*>("portfolioDayCurrencyStatus");QVERIFY(status);QCOMPARE(status->text(),QStringLiteral("환율 불러오는 중"));QVERIFY(status->isVisibleTo(&view));
    view.clear(QStringLiteral("금고를 열면 표시"));QVERIFY(status->text().isEmpty());QVERIFY(status->isHidden());
    QVERIFY(!view.accessibleDescription().contains("250,000"));QVERIFY(!privateText||privateText->toPlainText().isEmpty());
    QVERIFY(view.findChildren<QLabel*>(QRegularExpression("portfolioContributionValue_.*")).isEmpty());
}
void PortfolioDayViewTest::computedAmountsKeepEvidenceInputsWithoutShowingReferenceLabels() {
    KeefiFonts::registerBundledFonts();
    PortfolioDayView view; PortfolioDayHistory history;
    history.comparison = {true, true, 1792598400000, 1792602000000, 0,
        "KRW", "1000", "1200", "200", QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"), {}};
    history.contributions.available = true;
    history.contributions.groups = {{PortfolioContributionClass::Coin,"700","900","200",1}};
    view.setHistory(history); view.setEvidenceReferences("[4] [6]");
    auto* reference = view.findChild<QLabel*>("portfolioDayReference");QVERIFY(reference);
    QVERIFY(reference->text().isEmpty());QVERIFY(reference->isHidden());
    auto* contributionReference=view.findChild<QLabel*>("portfolioContributionReference_0");QVERIFY(contributionReference);
    QVERIFY(contributionReference->text().isEmpty());QVERIFY(contributionReference->isHidden());
    auto* amount=view.findChild<QLabel*>("portfolioDayChange");QVERIFY(amount);
    QCOMPARE(amount->text(),QStringLiteral("▲ +200 KRW"));
    QCOMPARE(history.comparison.difference,QStringLiteral("200"));
    QVERIFY(!QRegularExpression(QStringLiteral("\\[[0-9]+\\]")).match(view.accessibleDescription()).hasMatch());
    view.setEvidenceReferences("private-connection-alias [4]");
    QVERIFY(reference->text().isEmpty());QCOMPARE(amount->text(),QStringLiteral("▲ +200 KRW"));
    QVERIFY(!view.accessibleDescription().contains("private-connection-alias"));
    view.setEvidenceReferences("[4] [6]"); view.clear(QStringLiteral("금고를 열면 표시"));
    QVERIFY(reference->text().isEmpty());QVERIFY(!view.accessibleDescription().contains("200"));
    QVERIFY(!view.accessibleDescription().contains("[4]"));
    history.comparison.available = false; history.comparison.reason = QStringLiteral("비교 범위가 바뀌었습니다.");
    view.setHistory(history); view.setEvidenceReferences("[4]");
    QVERIFY(reference->text().isEmpty());
    auto* explanation = view.findChild<QLabel*>("portfolioDayChange");
    QCOMPARE(explanation->font().family(), KeefiFonts::bodyFamily());
    QCOMPARE(explanation->font().pixelSize(), 15);
}
void PortfolioDayViewTest::contributionAmountsAreLedgerRows() {
    KeefiFonts::registerBundledFonts();
    PortfolioDayView view; view.setStyleSheet(KeefiTheme::localStyleSheet(&view));
    PortfolioDayHistory history;
    history.comparison = {true, true, 1792598400000, 1792602000000, 0,
        "KRW", "1000", "1200", "200", QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"), {}};
    history.contributions.available = true;
    history.contributions.groups = {{PortfolioContributionClass::Coin, "700", "950", "250", 1},
        {PortfolioContributionClass::Unclassified, "300", "250", "-50", 1}};
    view.setHistory(history); view.resize(600, 280); view.show(); QCoreApplication::processEvents();
    const auto* coinName = view.findChild<QLabel*>("portfolioContributionName_0");
    QVERIFY2(coinName, "Observed contributions must expose line-separated ledger text instead of painted rounded bars");
    QCOMPARE(coinName->text(), QStringLiteral("코인"));
    QCOMPARE(coinName->font().family(), KeefiFonts::bodyFamily());
    const auto* coinValue = view.findChild<QLabel*>("portfolioContributionValue_0"); QVERIFY(coinValue);
    QCOMPARE(coinValue->text(), QStringLiteral("▲ +250"));
    QCOMPARE(coinValue->font().family(), KeefiFonts::numberFamily());
    QCOMPARE(coinValue->font().pixelSize(), 22);
    const auto* residual = view.findChild<QLabel*>("portfolioContributionName_4"); QVERIFY(residual);
    QCOMPARE(residual->text(), QStringLiteral("분류 미확인·기타"));
    QCOMPARE(view.findChild<QLabel*>("portfolioContributionValue_4")->text(), QStringLiteral("▼ -50"));
    view.clear();
    QVERIFY(view.findChildren<QLabel*>(QRegularExpression("portfolioContribution(Name|Value)_.*")).isEmpty());
}
void PortfolioDayViewTest::oneObservationDoesNotPretendToBeAFlow() {
    PortfolioDayView view; view.resize(540, 120); view.show();
    PortfolioDayHistory history;
    history.comparison.reason=QStringLiteral("오늘 비교 가능한 기록을 모으고 있습니다.");
    history.points={{1792598400000,true,true,false,"1000",{}}};
    view.setHistory(history); QCoreApplication::processEvents();
    QVERIFY2(!view.findChild<QWidget*>("portfolioDayChart")->isVisible(),
        "A single observation must not reserve a chart or duplicate its time on both ends");
    QVERIFY(view.findChild<QLabel*>("portfolioDayChange")->isVisible());
    QVERIFY(view.sizeHint().height()<100);
}
void PortfolioDayViewTest::observedChangeShowsActualBaseline() {
    PortfolioDayView view;
    PortfolioDayHistory history;
    history.comparison = {true, true, 1792598400000, 1792602000000, 0,
        "KRW", "1000", "1200", "200", QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"), {}};
    view.setHistory(history);
    auto* change = view.findChild<QLabel*>("portfolioDayChange");
    QVERIFY(change);
    QCOMPARE(change->text(), QStringLiteral("▲ +200 KRW"));
    QCOMPARE(view.findChild<QLabel*>("portfolioDayTitle")->text(), history.comparison.label);
    const auto caption = view.findChild<QLabel*>("portfolioDayBaseline")->text();
    QVERIFY(caption.contains("01:00:00"));
    QVERIFY(caption.contains("KST"));
}
void PortfolioDayViewTest::partialReasonsAndPrivateContentClear() {
    PortfolioDayView view; PortfolioDayHistory history;
    history.comparison = {true, false, 1792598400000, 1792602000000, 1,
        "KRW", "1000", "800", "-200", QStringLiteral("동일 범위 부분합 증감 · 입출금 포함 가능"), {}};
    history.points = {{1792598400000, true, false, false, "1000", {}}, {1792602000000, true, false, true, "800", {}}};
    view.setHistory(history);
    QCOMPARE(view.findChild<QLabel*>("portfolioDayChange")->text(), QStringLiteral("▼ -200 KRW"));
    QVERIFY(view.findChild<QLabel*>("portfolioDayTitle")->text().contains(QStringLiteral("부분")));
    view.clear(QStringLiteral("금고를 열면 표시"));
    for (auto* label : view.findChildren<QLabel*>()) {
        QVERIFY(!label->text().contains("200")); QVERIFY(!label->text().contains("01:00"));
    }
    QVERIFY(view.findChild<QWidget*>("portfolioDayChart")->accessibleDescription().isEmpty());
    QVERIFY(!view.accessibleDescription().contains("200"));
    history.comparison.available = false; history.comparison.reason = QStringLiteral("비교 범위가 바뀌었습니다.");
    history.points.clear(); view.setHistory(history);
    QCOMPARE(view.findChild<QLabel*>("portfolioDayChange")->text(), history.comparison.reason);
}
void PortfolioDayViewTest::missingBaselineNeverLabelsMidnightAsAnObservation() {
    PortfolioDayView view; PortfolioDayHistory history;
    history.baseline.timeMs=1792594800000;
    history.baseline.available=false;
    history.comparison.reason=QStringLiteral("오늘의 유효 기록이 없습니다.");
    view.setHistory(history);
    QVERIFY(view.findChild<QLabel*>("portfolioDayBaseline")->text().isEmpty());
    QCOMPARE(view.findChild<QLabel*>("portfolioDayChange")->text(),history.comparison.reason);
}
void PortfolioDayViewTest::chartBreaksGapsAndUsesActualTime() {
    PortfolioDayView view; view.setDetailsExpanded(true); view.resize(600, 170); view.show();
    PortfolioDayHistory history;
    const qint64 start = 1792598400000;
    history.points = {{start, true, true, false, "100", {}},
        {start + 1000, true, true, true, "100", {}},
        {start + 5000, false, false, false, {}, QStringLiteral("기록 없음")},
        {start + 10000, true, true, false, "100", {}}};
    view.setHistory(history); QCoreApplication::processEvents();
    auto* chart = view.findChild<QWidget*>("portfolioDayChart"); QVERIFY(chart);
    const auto pixels = chart->grab().toImage();
    const auto observationInk = themeColors().accent;
    QCOMPARE(pixels.pixelColor(10, 29), observationInk);
    // The second point is one tenth through actual time. A uniformly spaced
    // categorical axis or a line spanning the missing interval would colour x=1/2.
    QVERIFY(pixels.pixelColor(chart->width() / 2, 29) != observationInk);
    QVERIFY(pixels.pixelColor(chart->width() / 4, 29) != observationInk);
    QVERIFY(chart->accessibleDescription().contains(QStringLiteral("유효 기록 3개")));
}
void PortfolioDayViewTest::localWindowConnectsHistoryAndErasesImmediatelyOnLock() {
    QTemporaryDir dir;
    QCoreApplication::setOrganizationName("KEEFI Synthetic Day View");
    QCoreApplication::setApplicationName("Synthetic Day View");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("vault")); QVERIFY(vault.create("Fake Day View Vault! 2026"));
    keefi::test::ReadFixture fixture("binance");
    QVERIFY(vault.addAccount(fixture.account, fixture.credentials, StorageMode::EncryptedVault));
    keefi::test::ProviderReadTransport transport; transport.provider = "binance";
    transport.transform = [](const RequestSpec& request, HttpResult& response) {
        if (request.operation == Operation::Account)
            response.body = R"({"canTrade":true,"accountType":"SPOT","balances":[{"asset":"KRW","free":"100","locked":"0"}]})";
    };
    PortfolioController::Factories factories;
    factories.trading = [&](const AccountRecord&) { return &transport; };
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    AccountModel accounts(&vault); LocalWindow window(vault, accounts);
    window.setSecurityStatus(true); window.setPortfolioController(&portfolio);
    auto* view = window.findChild<PortfolioDayView*>("portfolioDayView"); QVERIFY(view);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(view->findChild<QLabel*>("portfolioDayChange")->text().contains("KRW"), 5000);
    window.hide(); QCoreApplication::processEvents();
    const auto hiddenAt=portfolio.dayHistoryResult().comparison.toMs;
    QVERIFY(hiddenAt>0); emit portfolio.changed(); QTest::qWait(1100);
    QCOMPARE(portfolio.dayHistoryResult().comparison.toMs,hiddenAt);
    vault.lock();
    QVERIFY(!view->findChild<QLabel*>("portfolioDayChange")->text().contains("KRW"));
    QVERIFY(view->findChild<QWidget*>("portfolioDayChart")->accessibleDescription().isEmpty());
    QTest::qWait(50);
    QVERIFY(!view->accessibleDescription().contains("KRW"));
}
void PortfolioDayViewTest::contributionShowsResidualAndClearsWithLock() {
    PortfolioDayView view; PortfolioDayHistory history;
    history.comparison = {true, true, 1792598400000, 1792602000000, 0,
        "KRW", "1000", "1200", "200", QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"), {}};
    history.contributions.available = true;
    history.contributions.groups = {{PortfolioContributionClass::Coin, "700", "950", "250", 1},
        {PortfolioContributionClass::Unclassified, "300", "250", "-50", 1}};
    view.setHistory(history);
    auto* label = view.findChild<QLabel*>("portfolioDayContributions"); QVERIFY(label);
    QVERIFY(label->text().contains(QStringLiteral("코인 ▲ +250 KRW")));
    QVERIFY(label->text().contains(QStringLiteral("분류 미확인·기타 ▼ -50 KRW")));
    view.clear(QStringLiteral("금고를 열면 표시")); QVERIFY(label->text().isEmpty());
}
void PortfolioDayViewTest::publicMarketFlowIsSeparateFromPrivatePortfolio() {
    QTemporaryDir dir;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    Vault vault(dir.filePath("vault")); AccountModel accounts(&vault);
    PortfolioController::Factories factories; factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    class NoReadTransport final : public TradingTransport {
      public: int requests=0; void abort() override {};
        void send(quint64,const RequestSpec&) override { ++requests; }
    } transport;
    PublicMarketCatalog catalog(&transport); MarketMetrics metrics(&transport);
    MarketOverview overview(&transport); OfficialCalendar calendar(&transport);
    MarketCandles candles(&transport); OfficialNotices notices(&transport);
    TossMarketStream stocks(nullptr, &transport);
    LocalWindow window(vault, accounts); window.setPortfolioController(&portfolio);
    window.setPublicMarketCatalog(&catalog, false); window.setMarketMetrics(&metrics);
    window.setMarketOverview(&overview); window.setOfficialCalendar(&calendar);
    window.setMarketCandles(&candles); window.setOfficialNotices(&notices); window.setTossMarketStream(&stocks);
    QVERIFY(window.findChild<TossMarketView*>());
    QVERIFY(window.findChild<MarketCandlesView*>()); QVERIFY(window.findChild<OfficialNoticesView*>());
    QVERIFY(window.findChild<MarketOverviewView*>()); QVERIFY(window.findChild<OfficialCalendarView*>());
    auto* control = window.findChild<QComboBox*>("exploreView"); QVERIFY(control);
    const int flow = control->findData("flow"); QVERIFY(flow >= 0);
    auto* detail = window.findChild<MarketMetricsView*>("marketMetricsView"); QVERIFY(detail);
    QVERIFY(!vault.isUnlocked());
    control->setCurrentIndex(flow);
    QCOMPARE(window.findChild<QStackedWidget*>("exploreStack")->currentWidget()->objectName(), "marketFlowPage");
    // The window is hidden: setting up public display must not start subscriptions.
    QCOMPARE(transport.requests, 0);
    QVERIFY(window.findChild<PortfolioDayView*>("portfolioDayView")->accessibleDescription().contains(QStringLiteral("금고")));
    // Every transport is fake. Clear coin selection before show so dedicated
    // public WebSockets also stay stopped in this synthetic layout preview.
    detail->setAsset({});window.findChild<MarketCandlesView*>()->setAsset({});
    window.resize(1280,900);window.show();QTest::qWait(80);
    QVERIFY(!window.findChild<QScrollArea*>("tossMarketQuoteScroll")->isVisible());
    QVERIFY(!window.findChild<QWidget*>("marketCandlesChart")->isVisible());
    const auto preview=qEnvironmentVariable("KEEFI_G8_PREVIEW");
    if(!preview.isEmpty()){
        QVERIFY(window.grab().save(QDir(preview).filePath("g8-flow-wide.png")));
        window.resize(760,800);QTest::qWait(80);
        QVERIFY(window.grab().save(QDir(preview).filePath("g8-flow-narrow.png")));
    }
}
void PortfolioDayViewTest::flowOpensOnRequest() {
    PortfolioDayView view; view.resize(600, 300); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    auto* toggle = view.findChild<QPushButton*>("portfolioDayToggle"); QVERIFY(toggle);
    auto* chart = view.findChild<QWidget*>("portfolioDayChart"); QVERIFY(chart);
    QVERIFY2(!toggle->isVisible(), "Nothing to open without a chart or contributions");
    PortfolioDayHistory history;
    const qint64 start = 1792598400000;
    history.comparison = {true, true, start, start + 3600000, 0, "KRW", "1000", "1200", "200", QStringLiteral("관측 평가액 증감"), {}};
    history.points = {{start, true, true, false, "1000", {}}, {start + 3600000, true, true, false, "1200", {}}};
    QSignalSpy expanded(&view, &PortfolioDayView::detailsExpandedChanged);
    view.setHistory(history); QCoreApplication::processEvents();
    // The change reads on the caption line; the chart waits to be asked for.
    QVERIFY(toggle->isVisible()); QVERIFY(!chart->isVisible()); QVERIFY(view.findChild<QLabel*>("portfolioDayChange")->isVisible());
    QCOMPARE(toggle->text(), QStringLiteral("흐름 보기")); QVERIFY(!toggle->icon().isNull());
    toggle->click(); QCoreApplication::processEvents();
    QVERIFY(chart->isVisible()); QCOMPARE(toggle->text(), QStringLiteral("흐름 접기"));
    QCOMPARE(expanded.count(), 1); QVERIFY(expanded.last().first().toBool());
    toggle->click(); QVERIFY(!chart->isVisible()); QCOMPARE(expanded.count(), 2);
    view.clear(QStringLiteral("금고를 열면 표시")); QVERIFY(!toggle->isVisible());
    // Optional review images of both states with the app theme and synthetic history.
    if (const auto out = qEnvironmentVariable("KEEFI_P3_WIDGET_RENDER_DIR"); !out.isEmpty()) {
        KeefiFonts::registerBundledFonts();
        PortfolioDayHistory full; const qint64 from = 1792598400000, to = 1792602000000;
        full.comparison = {true, true, from, to, 0, "KRW", "1250000", "1500000", "250000", QStringLiteral("관측 평가액 증감 · 입출금 포함 가능"), {}};
        full.contributions.available = true;
        full.contributions.groups = {{PortfolioContributionClass::Coin, "875000", "1187500", "312500", 1}, {PortfolioContributionClass::Unclassified, "375000", "312500", "-62500", 1}};
        full.points = {{from, true, true, false, "1250000", {}}, {to, true, true, true, "1500000", {}}}; full.closingValuation.knownSubtotal = "1500000";
        PortfolioDayView preview; preview.setStyleSheet(KeefiTheme::localStyleSheet(&preview)); preview.resize(900, 380); preview.show();
        preview.setHistory(full); QCoreApplication::processEvents();
        QVERIFY(QDir().mkpath(out));
        QVERIFY(preview.grab().save(out + "/day-flow-closed.png"));
        preview.setDetailsExpanded(true); QCoreApplication::processEvents();
        QVERIFY(preview.grab().save(out + "/day-flow-open.png"));
    }
}
void PortfolioDayViewTest::expandedRereadKeepsTheHoldingsPositionUntilItSettles() {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* day = new PortfolioDayView(&host);
    auto* holdings = new QLabel(QStringLiteral("종목"),&host);
    layout->addWidget(day);
    layout->addWidget(holdings);
    layout->addStretch();
    host.resize(480,840);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    PortfolioDayHistory ready;
    ready.comparison.available = ready.comparison.complete = ready.contributions.available = true;
    ready.comparison.difference = "250";
    ready.contributions.groups = {{PortfolioContributionClass::Coin,"1000","1250","250",1}};
    ready.points = {{1000,true,true,false,"1000",{}},{2000,true,true,true,"1250",{}}};
    day->setHistory(ready);
    day->setDetailsExpanded(true);
    QCoreApplication::processEvents();
    const int initial = holdings->y();
    const auto bars = day->findChild<QWidget*>("portfolioContributionBars");
    const auto delta = day->findChild<QLabel*>("portfolioDayChange");
    const auto toggle = day->findChild<QPushButton*>("portfolioDayToggle");
    QVERIFY(bars && delta && toggle && bars->isVisible());
    auto pending = ready;
    pending.comparison = {}; pending.contributions = {};
    pending.comparison.reason = QStringLiteral("계좌 잔고를 다시 확인하고 있습니다…");
    day->setHistory(pending,{},true);
    QCoreApplication::processEvents();
    QCOMPARE(holdings->y(),initial);
    QVERIFY(bars->isHidden());
    QVERIFY(bars->accessibleDescription().isEmpty());
    QVERIFY(bars->findChildren<QLabel*>().isEmpty());
    QCOMPARE(delta->text(),pending.comparison.reason);
    day->setHistory(ready);
    QCoreApplication::processEvents();
    QCOMPARE(holdings->y(),initial);
    QVERIFY(bars->isVisible());

    // Opening during an account reread keeps the same extent on completion.
    day->setDetailsExpanded(false);
    QCoreApplication::processEvents();
    day->setHistory(pending,{},true);
    day->setDetailsExpanded(true);
    QCoreApplication::processEvents();
    QCOMPARE(holdings->y(),initial);
    QVERIFY(bars->isHidden());
    day->setHistory(ready);
    QCoreApplication::processEvents();
    QCOMPARE(holdings->y(),initial);

    day->setHistory(pending,{},true);
    day->setHistory(pending); // A confirmed result without contributions must shrink.
    QCoreApplication::processEvents();
    QVERIFY(holdings->y()<initial);
    day->setHistory(ready);
    QCoreApplication::processEvents();
    day->setHistory({}, {}, true);
    QVERIFY(toggle->isVisible());
    toggle->click();
    QCoreApplication::processEvents();
    QVERIFY(!toggle->isVisible());
    QCOMPARE(day->minimumHeight(),0);

    // Narrow collapsed headers can take two lines for an actual amount.
    day->setFixedWidth(300);
    day->setHistory(ready);
    QCoreApplication::processEvents();
    const int collapsed = holdings->y();
    const int collapsedHeight = day->height();
    day->setHistory(pending,{},true);
    QCoreApplication::processEvents();
    QCOMPARE(day->minimumHeight(),collapsedHeight);
    QCOMPARE(holdings->y(),collapsed);
    day->setHistory(ready);
    QCoreApplication::processEvents();
    QCOMPARE(day->minimumHeight(),0);
    QCOMPARE(holdings->y(),collapsed);
    day->setDetailsExpanded(true);
    QCoreApplication::processEvents();
    day->setHistory(pending,{},true);
    CoinPerformanceView performance;performance.inScope=true;performance.today.available=true;performance.today.pnl="12345";
    const auto coin=day->findChild<QLabel*>("coinPerformanceValue");QVERIFY(coin);
    day->setCoinPerformance(performance);QVERIFY(coin->accessibleName().contains(coin->text()));
    day->setCoinPerformance({});QVERIFY(coin->accessibleName().isEmpty());
    day->setCoinPerformance(performance);
    day->clear(QStringLiteral("금고를 열면 표시"));
    QVERIFY(coin->text().isEmpty());QVERIFY(coin->accessibleName().isEmpty());
    QCOMPARE(day->minimumHeight(),0);
    QVERIFY(!bars->isVisible()&&!toggle->isVisible());
    QVERIFY(bars->findChildren<QLabel*>().isEmpty());

    // A settled no-history explanation can also be taller than the read status.
    day->setDetailsExpanded(false);
    PortfolioDayHistory missing;
    missing.comparison.reason = QStringLiteral("오늘 비교 기록이 없습니다. 계좌와 가격을 함께 관측한 뒤 비교합니다.");
    day->setHistory(missing);
    QCoreApplication::processEvents();
    const int noHistoryHeight = day->height();
    const int noHistoryY = holdings->y();
    day->setHistory(pending, {}, true);
    QCoreApplication::processEvents();
    QCOMPARE(day->minimumHeight(),noHistoryHeight);
    QCOMPARE(holdings->y(),noHistoryY);
    day->setHistory(missing);
    QCOMPARE(day->minimumHeight(),0);
}
void PortfolioDayViewTest::allocationAnimationOnlyStartsOnCompositionChange() {
    KeefiFonts::registerBundledFonts();
    PortfolioHoldingView btcHolding; btcHolding.asset = "BTC"; btcHolding.quantity = "1"; btcHolding.value = "3000000"; btcHolding.state = PortfolioValueState::Fresh;
    PortfolioGroupView btc; btc.identity = "exchange:BTC"; btc.asset = "BTC"; btc.freshTotal = "3000000"; btc.holdings = {btcHolding};
    PortfolioHoldingView krwHolding = btcHolding; krwHolding.asset = "KRW"; krwHolding.value = "1000000"; krwHolding.kind = AssetComponentKind::Cash;
    PortfolioGroupView krw; krw.identity = "cash:KRW"; krw.asset = "KRW"; krw.freshTotal = "1000000"; krw.holdings = {krwHolding};

    // Hidden widget goes directly to final state without running animation.
    PortfolioAllocation hiddenView;
    hiddenView.setStyleSheet(KeefiTheme::localStyleSheet(&hiddenView));
    hiddenView.resize(600, 100);
    hiddenView.setGroups({btc, krw});
    auto* hiddenMotion = hiddenView.findChild<QVariantAnimation*>();
    QVERIFY(hiddenMotion);
    QCOMPARE(hiddenMotion->state(), QAbstractAnimation::Stopped);
    hiddenView.show();
    QCoreApplication::processEvents();
    QCOMPARE(hiddenMotion->state(), QAbstractAnimation::Stopped);

    // Visible widget expands on initial population.
    PortfolioAllocation view;
    view.setStyleSheet(KeefiTheme::localStyleSheet(&view));
    view.resize(600, 100);
    view.show();
    auto* motion = view.findChild<QVariantAnimation*>();
    QVERIFY(motion);
    QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
    view.setGroups({btc, krw});
    if (motionEnabled()) {
        QCOMPARE(motion->state(), QAbstractAnimation::Running);
        QCOMPARE(motion->duration(), 420);
        QCOMPARE(motion->easingCurve().type(), QEasingCurve::OutExpo);
        QTRY_COMPARE_WITH_TIMEOUT(motion->state(), QAbstractAnimation::Stopped, 2000);
    } else {
        QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
    }
    QCoreApplication::processEvents();
    QCOMPARE(hiddenView.grab().toImage(), view.grab().toImage());

    // Price ticks change only values: no replay, also when they swap the slice order.
    PortfolioGroupView btcUpdated = btc;
    btcUpdated.freshTotal = "2000000";
    btcUpdated.holdings[0].value = "2000000";
    PortfolioGroupView krwUpdated = krw;
    krwUpdated.freshTotal = "2000000";
    krwUpdated.holdings[0].value = "2000000";
    QSignalSpy stateSpy(motion, &QAbstractAnimation::stateChanged);
    view.setGroups({btcUpdated, krwUpdated});
    QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
    PortfolioGroupView btcLower = btc;
    btcLower.freshTotal = "1000000";
    btcLower.holdings[0].value = "1000000";
    PortfolioGroupView krwHigher = krw;
    krwHigher.freshTotal = "3000000";
    krwHigher.holdings[0].value = "3000000";
    view.setGroups({btcLower, krwHigher}); // KRW now ranks above BTC.
    QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
    QCOMPARE(stateSpy.count(), 0);
    // Six assets put the two smallest into "기타"; a tick that moves an asset across that
    // boundary is still the same set of assets.
    const auto group = [&btcHolding](const QString& asset, const QString& value) {
        PortfolioHoldingView holding = btcHolding; holding.asset = asset; holding.value = value;
        PortfolioGroupView result; result.identity = "exchange:" + asset; result.asset = asset; result.freshTotal = value; result.holdings = {holding};
        return result;
    };
    PortfolioAllocation many;
    many.setStyleSheet(KeefiTheme::localStyleSheet(&many));
    many.resize(600, 100);
    many.show();
    auto* manyMotion = many.findChild<QVariantAnimation*>();
    QVERIFY(manyMotion);
    many.setGroups({group("A", "600"), group("B", "500"), group("C", "400"), group("D", "300"), group("E", "200"), group("F", "100")});
    QTRY_COMPARE_WITH_TIMEOUT(manyMotion->state(), QAbstractAnimation::Stopped, 2000);
    QSignalSpy manySpy(manyMotion, &QAbstractAnimation::stateChanged);
    many.setGroups({group("A", "600"), group("B", "500"), group("C", "400"), group("D", "100"), group("E", "200"), group("F", "700")});
    QCOMPARE(manyMotion->state(), QAbstractAnimation::Stopped);
    QCOMPARE(manySpy.count(), 0);

    // Changing slice composition (adding a new slice) restarts animation on visible widget.
    PortfolioHoldingView ethHolding = btcHolding; ethHolding.asset = "ETH"; ethHolding.value = "500000";
    PortfolioGroupView eth; eth.identity = "exchange:ETH"; eth.asset = "ETH"; eth.freshTotal = "500000"; eth.holdings = {ethHolding};
    view.setGroups({btcUpdated, krwUpdated, eth});
    if (motionEnabled()) {
        QCOMPARE(motion->state(), QAbstractAnimation::Running);
        // Hiding while running stops animation and snaps to final frame.
        view.hide();
        QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
    }
}
QTEST_MAIN(PortfolioDayViewTest)
#include "tst_portfolio_day_view.moc"
