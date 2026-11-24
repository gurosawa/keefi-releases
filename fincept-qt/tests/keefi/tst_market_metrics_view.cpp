#include "PublicViewPreview.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/TradingTransport.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/InfoButton.h"
#include "keefi/MovementColorPolicy.h"
#include <QApplication>
#include <QDateTime>
#include <QCheckBox>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QJsonArray>
#include <QDialog>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTimeZone>
#include <QDir>
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
using namespace keefi;
namespace {
class Socket final : public PublicTickerSocket {
  public:
    int opens = 0, aborts = 0; QList<QByteArray> messages;
    void open(const QUrl&) override { ++opens; }
    void send(const QByteArray& bytes) override { messages.append(bytes); }
    void ping() override {}
    void abort() override { ++aborts; }
    void ready() { emit connected(); }
    void tick(const QJsonObject& o) { emit received(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
};
class Transport final : public TradingTransport {
  public:
    int sends = 0, aborts = 0;quint64 token=0;
    void send(quint64 next, const RequestSpec& request) override { QVERIFY(MarketMetrics::isRequest(request));token=next;++sends; }
    void abort() override { ++aborts; }
    void answer(const QByteArray& body,int status=200){HttpResult response;response.status=status;response.body=body;emit finished(token,response);}
};
}
class MarketMetricsViewTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase(){QVERIFY(KeefiFonts::registerBundledFonts());QApplication::setFont(KeefiFonts::font(FontRole::Body15));}
    void derivativesStartCollapsedAndTogglingDoesNotAddDemand();
    void privateFuturesContextExpandsAndLockImmediatelyClearsIt();
    void explicitAlertDemandRetainsPublicInputsAndPublishesSnapshots();
    void publicFlowPresentationUsesValidatedStatesAndPublicOptIns();
    void unknownFundingPeriodStaysPublishedAndHasNoHeatState();
    void optionalSyntheticMarketMetricsRenders();
    void metricsAreOrderedLedgerRowsAndHaveEvidenceFooter();
    void selectedQuotesStayPublicAndHiddenViewStopsSubscriptions();
    void publicCardsKeepIndependentSourcesAndFoldAbsentHistory();
    void absoluteMetricCardsStayNeutralWhenTheWindowPaletteChanges();
    void backgroundLoadingCountsAnswersAndUnlistedCoinsStopWaiting();
    void coinbaseReadsOnlyTheMarketStillRequested(){
        class Wire:public TradingTransport{public:QStringList paths;
            void abort()override{}void send(quint64,const RequestSpec& spec)override{QVERIFY(CoinbaseQuotes::isRequest(spec));paths<<spec.url.path();}
        } product,bridge;
        PublicQuoteStream up(PublicQuoteStream::Provider::Upbit,std::make_unique<Socket>()),bn(PublicQuoteStream::Provider::Binance,std::make_unique<Socket>());
        Transport wire;MarketMetrics metrics(&wire);CoinbaseQuotes cb(&product,nullptr,&bridge);MarketMetricsView view(&metrics,&up,&bn);
        view.setCoinbaseQuotes(&cb);view.setAsset("BTC");view.setBackgroundActive(true);view.setBackgroundActive(false);
        QCoreApplication::processEvents();QVERIFY(product.paths.isEmpty());QVERIFY(bridge.paths.isEmpty());
        view.setBackgroundActive(true);view.setAsset("ETH");QCoreApplication::processEvents();
        QCOMPARE(product.paths,QStringList{"/products/ETH-USD/ticker"});QCOMPARE(bridge.paths,QStringList{"/products/USDT-USD/ticker"});
    }
};
void MarketMetricsViewTest::derivativesStartCollapsedAndTogglingDoesNotAddDemand(){
    auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();auto* up=a.get();auto* bn=b.get();
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
    Transport transport;MarketMetrics metrics(&transport);MarketMetricsView view(&metrics,&upbit,&binance);view.setAsset("BTC");view.resize(900,800);view.show();
    QTRY_COMPARE(transport.sends,1);QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);
    auto* toggle=view.findChild<QPushButton*>("marketMetricsDerivativesToggle");QVERIFY2(toggle,"The public flow starts with a collapsed derivatives section");
    auto* details=view.findChild<QWidget*>("marketMetricsDerivatives");QVERIFY(details);QVERIFY(details->isHidden());
    QVERIFY(view.findChild<ExploreMetricCard*>("marketMetricsGapCard")->isVisibleTo(&view));
    const auto requests=transport.sends,upOpens=up->opens,bnOpens=bn->opens;
    toggle->click();QCoreApplication::processEvents();QVERIFY(details->isVisibleTo(&view));
    QVERIFY(view.findChild<ExploreMetricCard*>("marketMetricsFundingCard")->isVisibleTo(&view));
    toggle->click();QCoreApplication::processEvents();QVERIFY(details->isHidden());
    QCOMPARE(transport.sends,requests);QCOMPARE(up->opens,upOpens);QCOMPARE(bn->opens,bnOpens);
}
void MarketMetricsViewTest::privateFuturesContextExpandsAndLockImmediatelyClearsIt(){
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::make_unique<Socket>()),binance(PublicQuoteStream::Provider::Binance,std::make_unique<Socket>());
    MarketMetricsView view(nullptr,&upbit,&binance);view.setAsset("BTC");view.resize(900,800);view.show();
    auto* toggle=view.findChild<QPushButton*>("marketMetricsDerivativesToggle");
    auto* details=view.findChild<QWidget*>("marketMetricsDerivatives");QVERIFY(toggle);QVERIFY(details);QVERIFY(details->isHidden());
    view.setPrivateContext(true,true);QVERIFY(toggle->isChecked());QVERIFY(details->isVisibleTo(&view));
    view.setPrivateContext(false,true);QVERIFY(!toggle->isChecked());QVERIFY(details->isHidden());
    toggle->click();QVERIFY(details->isVisibleTo(&view));
    view.setPrivateContext(false,false);QVERIFY(details->isVisibleTo(&view));
    view.setPrivateContext(true,false);QVERIFY(details->isVisibleTo(&view));
    view.setPrivateContext(false,false);QVERIFY(details->isHidden());
}
void MarketMetricsViewTest::explicitAlertDemandRetainsPublicInputsAndPublishesSnapshots(){
    auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();auto* up=a.get();auto* bn=b.get();
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
    Transport transport;MarketMetrics metrics(&transport);MarketMetricsView view(&metrics,&upbit,&binance);view.setAsset("BTC");
    int observations=0;QString observedAsset;ExchangePriceGap observedGap;
    connect(&view,&MarketMetricsView::snapshotReady,&view,[&](const QString& asset,const ExchangePriceGap& gap,const ExchangePriceGap&,const PublicDerivativesMetrics&){++observations;observedAsset=asset;observedGap=gap;});
    QCOMPARE(transport.sends,0);view.setAlertDemandActive(true);QTRY_COMPARE(transport.sends,1);QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);
    QVERIFY(!view.isVisible());up->ready();bn->ready();const auto now=QDateTime::currentMSecsSinceEpoch()-20;
    const auto tick=[&](const QString& code,double value){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",value},{"prev_closing_price",value},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};
    up->tick(tick("KRW-BTC",142800000));up->tick(tick("KRW-USDT",1400));bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
    QTRY_VERIFY(observedGap.available);QCOMPARE(observedAsset,QString("BTC"));QCOMPARE(observedGap.percent,QString("2"));QCOMPARE(observedGap.legs.size(),3);QVERIFY(observations>0);
    for(const auto& leg:observedGap.legs)QVERIFY(leg.evidence.scope.startsWith("public:"));
    const auto upAborts=up->aborts,bnAborts=bn->aborts;const auto sends=transport.sends;
    view.show();view.hide();QCOMPARE(up->aborts,upAborts);QCOMPARE(bn->aborts,bnAborts);QCOMPARE(transport.sends,sends);
    view.setAlertDemandActive(false);QVERIFY(up->aborts>upAborts);QVERIFY(bn->aborts>bnAborts);QVERIFY(!observedGap.available);QVERIFY(metrics.derivatives().symbol.isEmpty());
}
void MarketMetricsViewTest::publicFlowPresentationUsesValidatedStatesAndPublicOptIns(){
    auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();auto* up=a.get();auto* bn=b.get();
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));Transport transport;MarketMetrics metrics(&transport);
    MarketMetricsView view(&metrics,&upbit,&binance);view.setAsset("BTC");view.resize(900,1050);view.show();QTRY_COMPARE(transport.sends,1);QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);up->ready();bn->ready();
    transport.answer(R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})");QTRY_COMPARE(transport.sends,2);transport.answer("[]");QTRY_COMPARE(transport.sends,3);
    const auto now=QDateTime::currentMSecsSinceEpoch()-20;
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0004"},{"interestRate","0.0001"},{"markPrice","100"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}}).toJson());QTRY_COMPARE(transport.sends,4);
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"openInterest","123.45"},{"time",QString::number(now)}}).toJson());
    const auto tick=[&](const QString& code,double price){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",price},{"prev_closing_price",price},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};
    up->tick(tick("KRW-BTC",144200000));up->tick(tick("KRW-USDT",1400));bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
    // Usability 10.3: gaps display two decimals; the state still compares the exact value.
    QTRY_COMPARE(view.findChild<ExploreMetricCard*>("marketMetricsGapCard")->data().value,QString("3.00"));
    view.setPreviousGap({true,"1",now/MarketFlowPolicy::HourMs*MarketFlowPolicy::HourMs-MarketFlowPolicy::DayMs,now,"BTC"});view.setCalendarSummary("CPI 발표");
    auto* summary=view.findChild<QLabel*>("marketMetricsSummary");QVERIFY2(summary,"Today summary must combine only public states and calendar");
    QVERIFY(summary->text().contains(QStringLiteral("과열")));QVERIFY(summary->text().contains("CPI"));
    QCOMPARE(view.findChild<QLabel*>("marketMetricsTitle")->text(),usabilityText(UsabilityText::FlowTodayTitle));
    auto* gap=view.findChild<ExploreMetricCard*>("marketMetricsGapCard");QCOMPARE(gap->data().title,QStringLiteral("테더 환산 가격차"));QVERIFY(gap->data().changeText.contains("2.00%p"));
    QCOMPARE(view.findChild<QLabel*>("marketMetricsGapState")->text(),usabilityText(UsabilityText::FlowStateHot));
    auto* funding=view.findChild<ExploreMetricCard*>("marketMetricsFundingCard");QCOMPARE(funding->data().value,QString("43.8"));QCOMPARE(funding->data().title,usabilityText(UsabilityText::FlowFundingTitle));QCOMPARE(metrics.derivatives().fundingRate,QString("0.0004"));
    QCOMPARE(view.findChild<QLabel*>("marketMetricsFundingState")->text(),usabilityText(UsabilityText::FlowStateHot));
    QVERIFY(view.findChild<QLabel*>("marketMetricsCoinbaseGapState")->isHidden());QVERIFY(view.findChild<QLabel*>("marketMetricsOpenInterestFlow")->isHidden());
    QSignalSpy toggles(&view,&MarketMetricsView::metricAlertToggled);
    const QList<MarketFlowMetric> kinds{MarketFlowMetric::KoreaGap,MarketFlowMetric::Funding,MarketFlowMetric::CoinbaseGap,MarketFlowMetric::OpenInterest};
    const QStringList names{"marketMetricsGapAlert","marketMetricsFundingAlert","marketMetricsCoinbaseGapAlert","marketMetricsOpenInterestAlert"};
    for(int i=0;i<names.size();++i){auto* box=view.findChild<QCheckBox*>(names[i]);QVERIFY(box);QVERIFY(!box->isChecked());QVERIFY(!box->accessibleName().isEmpty());view.setMetricAlertEnabled(kinds[i],true);QVERIFY(box->isChecked());}
    QCOMPARE(toggles.size(),0);view.findChild<QCheckBox*>(names.first())->click();QCOMPARE(toggles.size(),1);QCOMPARE(toggles.first()[0].value<MarketFlowMetric>(),MarketFlowMetric::KoreaGap);QCOMPARE(toggles.first()[1].toBool(),false);
    const auto sends=transport.sends;view.setPreviousGap({});QVERIFY(gap->data().changeText.isEmpty());QCOMPARE(gap->data().direction,MovementDirection::Unknown);QCOMPARE(transport.sends,sends);
}
void MarketMetricsViewTest::unknownFundingPeriodStaysPublishedAndHasNoHeatState(){
    Transport transport;MarketMetrics metrics(&transport);auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
    MarketMetricsView view(&metrics,&upbit,&binance);view.setAsset("BTC");view.show();QTRY_COMPARE(transport.sends,1);
    transport.answer(R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})");QTRY_COMPARE(transport.sends,2);transport.answer("{}",503);QTRY_COMPARE(transport.sends,3);const auto now=QDateTime::currentMSecsSinceEpoch();
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0004"},{"interestRate","0.0001"},{"markPrice","100"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}}).toJson());
    QTRY_COMPARE(metrics.derivatives().fundingRate,QString("0.0004"));QCOMPARE(metrics.derivatives().fundingIntervalHours,0);
    auto* state=view.findChild<QLabel*>("marketMetricsFundingState");QVERIFY(state);QVERIFY(state->isHidden());
    const auto data=view.findChild<ExploreMetricCard*>("marketMetricsFundingCard")->data();QCOMPARE(data.value,QString("0.04"));QCOMPARE(data.title,usabilityText(UsabilityText::FlowPublishedFundingTitle));QCOMPARE(data.direction,MovementDirection::Unknown);
}
void MarketMetricsViewTest::optionalSyntheticMarketMetricsRenders(){
    const auto directory=qEnvironmentVariable("KEEFI_D4_RENDER_DIR");if(directory.isEmpty())QSKIP("Optional synthetic D4 market metric view PNG artifacts; set KEEFI_D4_RENDER_DIR to export.");
    QVERIFY(QDir().mkpath(directory));const auto original=KeefiTheme::instance()->mode();
    for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
        KeefiTheme::instance()->setMode(mode);auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();auto* up=a.get();auto* bn=b.get();
        PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));Transport transport;MarketMetrics metrics(&transport);MarketMetricsView view(&metrics,&upbit,&binance);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setAsset("BTC");view.resize(1200,740);view.show();QTRY_COMPARE(transport.sends,1);QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);up->ready();bn->ready();
        const auto now=QDateTime::currentMSecsSinceEpoch()-20;const auto tick=[&](const QString& code,double price){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",price},{"prev_closing_price",price},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};
        up->tick(tick("KRW-BTC",142800000));up->tick(tick("KRW-USDT",1400));bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
        transport.answer(R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})");QTRY_COMPARE(transport.sends,2);transport.answer("[]");QTRY_COMPARE(transport.sends,3);
        transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0001"},{"interestRate","0.0001"},{"markPrice","100"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}}).toJson());QTRY_COMPARE(transport.sends,4);
        transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"openInterest","123.45"},{"time",QString::number(now)}}).toJson());QTRY_COMPARE(view.findChild<ExploreMetricCard*>("marketMetricsGapCard")->data().value,QString("2"));
        // The historical all-metric preview explicitly opens the spec 8.1 fold.
        view.findChild<QPushButton*>("marketMetricsDerivativesToggle")->setChecked(true);
        for(const int width:{1280,720}){view.resize(width,740);QCoreApplication::processEvents();QVERIFY(view.grab().save(QDir(directory).filePath(QString("market-metrics-%1-%2.png").arg(mode==ThemeMode::Light?"light":"dark").arg(width))));}view.hide();
    }
    KeefiTheme::instance()->setMode(original);
}
void MarketMetricsViewTest::metricsAreOrderedLedgerRowsAndHaveEvidenceFooter() {
    MarketMetricsView view(nullptr);view.setAsset("BTC");view.resize(1200,600);view.show();
    auto* first=view.findChild<ExploreMetricCard*>("marketMetricsGapCard");auto* second=view.findChild<ExploreMetricCard*>("marketMetricsCoinbaseGapCard");
    QVERIFY(first);QVERIFY(second);view.findChild<QPushButton*>("marketMetricsDerivativesToggle")->click();QCoreApplication::processEvents();
    // Usability 10.3: the gap leads full width; open derivatives sit below it as
    // three side-by-side cells, which stack again in a narrow view.
    auto* fundingCard=view.findChild<ExploreMetricCard*>("marketMetricsFundingCard");QVERIFY(fundingCard);
    const auto firstOrigin=first->mapTo(&view,QPoint{}),secondOrigin=second->mapTo(&view,QPoint{}),fundingOrigin=fundingCard->mapTo(&view,QPoint{});
    QVERIFY(secondOrigin.y()>=firstOrigin.y()+first->height());QCOMPARE(fundingOrigin.y(),secondOrigin.y());QVERIFY(fundingOrigin.x()<secondOrigin.x());
    view.resize(520,900);QCoreApplication::processEvents();
    QVERIFY(second->mapTo(&view,QPoint{}).y()>=fundingCard->mapTo(&view,QPoint{}).y()+fundingCard->height());
    QVERIFY(view.findChild<QWidget*>("marketMetricsEvidenceFooter"));
}
void MarketMetricsViewTest::publicCardsKeepIndependentSourcesAndFoldAbsentHistory(){
    auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
    Transport transport;MarketMetrics metrics(&transport);MarketMetricsView view(&metrics,&upbit,&binance);view.setAsset("BTC");view.resize(900,900);view.show();QTRY_COMPARE(transport.sends,1);
    transport.answer(R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL","status":"TRADING","underlyingType":"COIN"}]})");QTRY_COMPARE(transport.sends,2);
    transport.answer("[]");QTRY_COMPARE(transport.sends,3);const auto now=QDateTime::currentMSecsSinceEpoch();
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastFundingRate","0.0001"},{"interestRate","0.0001"},{"markPrice","100"},{"nextFundingTime",QString::number(now+3600000)},{"time",QString::number(now)}}).toJson());QTRY_COMPARE(transport.sends,4);
    transport.answer(QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"openInterest","123.45"},{"time",QString::number(QDateTime::currentMSecsSinceEpoch())}}).toJson());QCOMPARE(metrics.derivatives().openInterest,QString("123.45"));QCOMPARE(metrics.derivatives().intervalEvidence.sourceTimeMs,qint64(0));
    view.setEvidenceStart(7);QCOMPARE(view.evidenceCount(),3);
    auto* footer=view.findChild<ExploreEvidenceFooter*>("marketMetricsEvidenceFooter");QVERIFY(footer);QCOMPARE(footer->numbers(),QList<int>({7,8,9}));
    QVERIFY(footer->isHidden());QVERIFY(footer->findChildren<QLabel*>().isEmpty());
    QCOMPARE(footer->entries().size(),3);QCOMPARE(footer->entries()[0].provider,QStringLiteral("Binance"));
    QCOMPARE(footer->entries()[0].sourceTimeMs,now);QCOMPARE(footer->entries()[1].sourceTimeMs,qint64(0));
    QVERIFY(view.findChild<ExploreMetricCard*>("marketMetricsFundingCard")->findChild<QLabel*>("metricEvidenceReference")->isHidden());
    for(const auto* name:{"marketMetricsGapCard","marketMetricsCoinbaseGapCard","marketMetricsFundingCard","marketMetricsOpenInterestCard"}){
        auto* card=view.findChild<ExploreMetricCard*>(name);QVERIFY2(card,name);QVERIFY(!card->historyVisible());QVERIFY(card->plottedHistory().isEmpty());QVERIFY(card->findChild<QWidget*>("metricSparkline")->isHidden());}
    // Spec 8.3 displays known-period annual funding (usability 10.3: one decimal); the source ratio remains unchanged.
    QCOMPARE(metrics.derivatives().fundingRate,QString("0.0001"));
    auto* funding=view.findChild<ExploreMetricCard*>("marketMetricsFundingCard");QCOMPARE(funding->data().value,QString("11.0"));QCOMPARE(funding->data().unit,QString("%"));QCOMPARE(funding->data().direction,MovementDirection::Unknown);
    auto* oi=view.findChild<ExploreMetricCard*>("marketMetricsOpenInterestCard");QCOMPARE(oi->data().value,QString("123.45"));QCOMPARE(oi->data().unit,QString("수량"));
    QVERIFY(funding->data().publicInformation.contains(QStringLiteral("원천 시각 미제공")));QVERIFY(funding->data().publicInformation.contains("https://fapi.binance.com/fapi/v1/fundingInfo"));QVERIFY(!funding->data().freshnessText.isEmpty());
    view.findChild<QPushButton*>("marketMetricsDerivativesToggle")->setChecked(true);
    auto* info=funding->informationButton();QVERIFY(info);QVERIFY(!info->accessibleName().isEmpty());info->setFocus();QTest::keyClick(info,Qt::Key_Space);QTRY_VERIFY(info->informationOpen());
    auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);auto* text=dialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);QVERIFY(text->isReadOnly());QVERIFY(text->toPlainText().contains(QStringLiteral("Binance")));QVERIFY(!text->toPlainText().contains("https://"));QVERIFY(!text->toPlainText().contains(QStringLiteral("수신")));
    QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!info->informationOpen());QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());QTest::keyClick(info->findChild<QDialog*>("infoDialog"),Qt::Key_Escape);
    QCoreApplication::processEvents();QVERIFY(funding->height()<250);QVERIFY(view.findChild<ExploreMetricCard*>("marketMetricsGapCard")->geometry().top()<100);
    QCOMPARE(transport.sends,4);QVERIFY(keefi::test::savePublicViewPreview(view,"c9-market-metrics"));view.hide();
}
void MarketMetricsViewTest::absoluteMetricCardsStayNeutralWhenTheWindowPaletteChanges(){
    QWidget window;MovementColorPolicy policy(&window);auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();auto* up=a.get();auto* bn=b.get();
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));Transport transport;MarketMetrics metrics(&transport);
    MarketMetricsView view(&metrics,&upbit,&binance,&window);view.setAsset("BTC");window.resize(950,900);view.resize(900,850);window.show();view.show();QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);up->ready();bn->ready();const auto now=QDateTime::currentMSecsSinceEpoch()-20;
    QSignalSpy upTicks(&upbit,&PublicQuoteStream::ticks),binanceTicks(&binance,&PublicQuoteStream::ticks);
    const auto tick=[&](const QString& code,double price){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",price},{"prev_closing_price",price},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};
    up->tick(tick("KRW-BTC",142800000));up->tick(tick("KRW-USDT",1400));bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
    QTRY_COMPARE(upTicks.size(),1);QTRY_COMPARE(binanceTicks.size(),1);
    auto* card=view.findChild<ExploreMetricCard*>("marketMetricsGapCard");QVERIFY(card);QCOMPARE(card->data().value,QString("2.00"));QCOMPARE(card->data().direction,MovementDirection::Unknown);const auto before=card->data();const auto requests=transport.sends;
    policy.setScheme(MovementScheme::KoreanRedBlue);QCOMPARE(policy.scheme(),MovementScheme::KoreanRedBlue);QCOMPARE(card->data(),before);QCOMPARE(transport.sends,requests);QVERIFY(!card->findChild<QLabel*>("metricChange")->text().contains(QChar(0x25b2)));
    QCOMPARE(card->findChild<QLabel*>("metricChange")->palette().color(QPalette::WindowText),policy.foreground(MovementDirection::Unknown));
}
void MarketMetricsViewTest::selectedQuotesStayPublicAndHiddenViewStopsSubscriptions() {
    auto a = std::make_unique<Socket>(); auto* up = a.get();
    auto b = std::make_unique<Socket>(); auto* bn = b.get();
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit, std::move(a));
    PublicQuoteStream binance(PublicQuoteStream::Provider::Binance, std::move(b));
    Transport transport; MarketMetrics metrics(&transport);
    MarketMetricsView view(&metrics, &upbit, &binance); view.setAsset("BTC");
    QCOMPARE(up->opens, 0); QCOMPARE(transport.sends, 0);
    view.show(); QTRY_COMPARE(up->opens, 1); QTRY_COMPARE(bn->opens, 1);
    up->ready(); bn->ready();
    QVERIFY(up->messages.last().contains("KRW-USDT"));
    QVERIFY(bn->messages.last().contains("btcusdt"));
    const auto now = QDateTime::currentMSecsSinceEpoch() - 20;
    const auto upTick = [&](QString code, double value) {
        return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",value},{"prev_closing_price",value},
            {"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};
    };
    up->tick(upTick("KRW-BTC",142800000)); up->tick(upTick("KRW-USDT",1400));
    bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});
    auto* gap = view.findChild<ExploreMetricCard*>("marketMetricsGapCard"); QVERIFY(gap);
    QVERIFY(view.findChild<ExploreMetricCard*>("marketMetricsCoinbaseGapCard"));
    QTRY_COMPARE(gap->data().value,QString("2.00"));QCOMPARE(gap->data().unit,QString("%"));
    // Spec 8.5 shortens visible absence text while retaining the missing source fact.
    QCOMPARE(view.findChild<ExploreMetricCard*>("marketMetricsFundingCard")->data().value,usabilityText(UsabilityText::FlowChecking));QVERIFY(metrics.derivatives().fundingRate.isEmpty());
    emit up->failed(); QVERIFY(gap->data().unit.isEmpty());QVERIFY(gap->data().value!=QString("2"));
    const auto upAborts = up->aborts, bnAborts = bn->aborts;
    view.hide(); QVERIFY(up->aborts > upAborts); QVERIFY(bn->aborts > bnAborts);
    QVERIFY(metrics.derivatives().symbol.isEmpty()); QVERIFY(transport.aborts > 0);
    view.setAsset({}); QVERIFY(gap->data().value.isEmpty());QVERIFY(gap->isHidden());
}
void MarketMetricsViewTest::backgroundLoadingCountsAnswersAndUnlistedCoinsStopWaiting(){
    auto a=std::make_unique<Socket>(),b=std::make_unique<Socket>();auto* up=a.get();auto* bn=b.get();
    PublicQuoteStream upbit(PublicQuoteStream::Provider::Upbit,std::move(a)),binance(PublicQuoteStream::Provider::Binance,std::move(b));
    Transport transport;MarketMetrics metrics(&transport);MarketMetricsView view(&metrics,&upbit,&binance);view.setAsset("BTC");
    QCOMPARE(view.progress(),qMakePair(0,0));QCOMPARE(transport.sends,0);QCOMPARE(up->opens,0);
    // Hidden page, open window: the public inputs load before anyone opens 시장 흐름.
    view.setBackgroundActive(true);QVERIFY(!view.isVisible());QTRY_COMPARE(transport.sends,1);QTRY_COMPARE(up->opens,1);QTRY_COMPARE(bn->opens,1);
    up->ready();bn->ready();QCOMPARE(view.progress(),qMakePair(1,4)); // No Coinbase model: nothing to wait for there.
    const auto now=QDateTime::currentMSecsSinceEpoch()-20;
    const auto tick=[&](const QString& code,double value){return QJsonObject{{"type","ticker"},{"code",code},{"trade_price",value},{"prev_closing_price",value},{"signed_change_rate",0},{"acc_trade_price_24h",100},{"timestamp",double(now)}};};
    up->tick(tick("KRW-BTC",142800000));up->tick(tick("KRW-USDT",1400));
    view.show();auto* gap=view.findChild<ExploreMetricCard*>("marketMetricsGapCard");QVERIFY(gap);
    QTRY_VERIFY(gap->data().publicInformation.contains(usabilityText(UsabilityText::FlowGapProgress).arg(2).arg(3)));QVERIFY(gap->data().changeText.isEmpty());QCOMPARE(gap->data().value,usabilityText(UsabilityText::FlowChecking));
    bn->tick({{"e","24hrMiniTicker"},{"s","BTCUSDT"},{"c","100000"},{"E",double(now)}});QTRY_COMPARE(gap->data().value,QString("2.00"));QCOMPARE(view.progress(),qMakePair(2,4));
    // A failed read is an answer: the card says there is no data instead of loading forever.
    transport.answer({},403);QCOMPARE(view.progress(),qMakePair(4,4));
    auto* funding=view.findChild<ExploreMetricCard*>("marketMetricsFundingCard");QTRY_COMPARE(funding->data().value,usabilityText(UsabilityText::FlowNoData));QVERIFY(!funding->data().changeText.isEmpty());
    // Hiding the page keeps the background streams.
    const auto aborts=up->aborts;view.hide();QCOMPARE(up->aborts,aborts);
    // A coin missing from Upbit KRW ends the wait at once, and its market is not requested.
    view.show();const auto opened=up->opens;view.setAsset("BTW");view.setListing(MarketMetricsView::Listing::Missing,MarketMetricsView::Listing::Missing);
    QTRY_VERIFY(up->opens>opened);up->ready();QVERIFY(!up->messages.last().contains("KRW-BTW"));QVERIFY(up->messages.last().contains("KRW-USDT"));
    QTRY_COMPARE(gap->data().value,usabilityText(UsabilityText::FlowGapUnavailable));QCOMPARE(gap->data().changeText,QStringLiteral("업비트 원화마켓 미지원"));QVERIFY(gap->data().unit.isEmpty());
    QCOMPARE(view.progress().first>=2,true);
    const auto reopened=up->opens;view.setListing(MarketMetricsView::Listing::Listed,MarketMetricsView::Listing::Missing);QTRY_COMPARE(gap->data().changeText,usabilityText(UsabilityText::FlowGapNotOnBinance));
    QTRY_VERIFY(up->opens>reopened);up->ready();QVERIFY(up->messages.last().contains("KRW-BTW"));
    view.setBackgroundActive(false);view.hide();QVERIFY(up->aborts>aborts);QCOMPARE(view.progress(),qMakePair(0,0));
}
QTEST_MAIN(MarketMetricsViewTest)
#include "tst_market_metrics_view.moc"
