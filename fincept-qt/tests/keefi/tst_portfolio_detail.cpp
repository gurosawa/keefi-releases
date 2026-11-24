#include "keefi/PortfolioDetail.h"
#include "keefi/BuildPolicy.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QDateTime>
#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScopeGuard>
#include <QTest>
#include <QTextDocument>
using namespace keefi;
namespace {
PortfolioGroupView group() {
    PortfolioGroupView result; result.identity="exchange:SYN"; result.asset="SYN"; result.freshTotal="123456";
    PortfolioHoldingView holding;holding.asset="SYN";holding.quantity="12";holding.value="123456";
    holding.provider="upbit";holding.market="spot";holding.accountAlias="SYNTHETIC_PRIVATE_ALIAS";
    holding.accountScope="SYNTHETIC_SCOPE";holding.connectionId="SYNTHETIC_BINDING";
    holding.state=PortfolioValueState::Fresh;holding.presence=AssetPresence::Held;
    holding.priceEvidence={"Upbit SYNTHETIC_SECRET", "https://api.upbit.com/v1/ticker?key=SYNTHETIC_SECRET", "SYNTHETIC_SCOPE",1792600000000,1792600005000,true};
    result.holdings={holding};result.storages={holding.accountAlias};return result;
}
QString visibleText(QWidget& root) {QStringList text;for(const auto* l:root.findChildren<QLabel*>())if(!l->isHidden())text<<l->text();return text.join('\n');}

}
class PortfolioDetailTest:public QObject {
    Q_OBJECT
  private slots:
    void initTestCase(){KeefiFonts::registerBundledFonts();}
    void privateEvidenceCannotBeDisclosedWhileSummaryRemains();
    void publicTradePreviewFollowsBuildPolicy_data();
    void publicTradePreviewFollowsBuildPolicy();
    void privateValuationHasLedgerEvidenceAndNoAvatar();
    void selectedPricesKeepExactSourceAndReceipt();
    void missingAndClearDropEveryReference();
    void publicQuoteOnlyAcceptsTypedProvenance();
    void publicMarketEvidenceMatchesQuoteAge_data();
    void publicMarketEvidenceMatchesQuoteAge();
    void publicQuoteFreshnessUsesTheExistingSixtySecondBoundary();
    void dollarValuationKeepsCashQuantityAndOriginalEvidence_data();
    void dollarValuationKeepsCashQuantityAndOriginalEvidence();
    void listReturnTakesFocusOnlyOutOfTheDetail();
};
// Returning to the list (a refresh, a filter, a lock) must not pull focus from
// where the user is; on a list view that focus would also make the first row
// current without a change signal. Only the detail's own back button hands
// focus to the list.
void PortfolioDetailTest::listReturnTakesFocusOnlyOutOfTheDetail(){
    QWidget host;auto* layout=new QVBoxLayout(&host);
    auto* search=new QLineEdit(&host);layout->addWidget(search);
    auto* list=new QTreeWidget;list->setColumnCount(1);new QTreeWidgetItem(list,{"A"});new QTreeWidgetItem(list,{"B"});
    auto* detail=new AssetDetailPanel;auto* view=new ListDetailView(list,detail,&host);layout->addWidget(view);
    view->setDetailOnlyWithSelection(true);
    host.resize(1200,700);host.show();QVERIFY(QTest::qWaitForWindowActive(&host));
    search->setFocus(Qt::TabFocusReason);QTRY_VERIFY(search->hasFocus());
    QTRY_VERIFY(!view->narrow());
    view->showList();QCoreApplication::processEvents();
    QVERIFY(search->hasFocus());QVERIFY(!list->currentItem());
    QVERIFY(!detail->isVisible()); // wide, and nothing is selected
    host.resize(800,700);QTRY_VERIFY(view->narrow());
    detail->showGroup(group(),"123456");view->showDetail();QTRY_VERIFY(detail->isVisible());QVERIFY(!list->isVisible());
    auto* back=detail->findChild<QPushButton*>("assetDetailBack");QVERIFY(back);QVERIFY(back->isVisible());
    back->setFocus(Qt::TabFocusReason);QTRY_VERIFY(back->hasFocus());
    back->click();
    QTRY_VERIFY(list->isVisible());QTRY_VERIFY(list->hasFocus());
}
void PortfolioDetailTest::dollarValuationKeepsCashQuantityAndOriginalEvidence_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void PortfolioDetailTest::dollarValuationKeepsCashQuantityAndOriginalEvidence(){
    QFETCH(bool,dark);auto* theme=KeefiTheme::instance();theme->setSettings(nullptr);const auto old=theme->mode();
    const auto restore=qScopeGuard([=]{theme->setMode(old);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    auto input=group();input.identity="cash:KRW";input.asset="KRW";input.freshTotal="1250000";
    auto& holding=input.holdings.first();holding.asset="KRW";holding.quantity="1250000";holding.value="1250000";
    holding.providerPnl="125000";holding.providerPnlCurrency="KRW";
    AssetDetailPanel panel;panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.resize(340,1000);panel.show();panel.showGroup(input,"2500000");
    auto* hidden=panel.findChild<QLabel*>("assetDetailHolding0");QVERIFY(hidden);const auto originalBlock=hidden->text();
    auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer);const auto proof=footer->entries();
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{{"USD","KRW","1250",{"Synthetic FX","https://example.invalid/USD-KRW","public:synthetic:USD-KRW",now,now,true}}},now);
    panel.showGroup(input,"2500000",display);QCoreApplication::processEvents();
    hidden=panel.findChild<QLabel*>("assetDetailHolding0");QVERIFY(hidden);
    QCOMPARE(panel.findChild<QLabel*>("assetDetailHeadline")->text(),QStringLiteral("1,000.00 USD"));
    auto* place=panel.findChild<QLabel*>("assetDetailPlaceValue1");QVERIFY(place);
    QVERIFY(place->text().contains(QStringLiteral("1,250,000 KRW")));QVERIFY(place->text().contains(QStringLiteral("1,000.00 USD")));
    QVERIFY(place->text().contains(QStringLiteral("업체 평가손익 +125,000 KRW")));
    QCOMPARE(panel.findChild<QLabel*>("assetDetailPlaceValue0")->text(),QStringLiteral("50.0%"));
    QCOMPARE(hidden->text(),originalBlock);QVERIFY(footer->entries()==proof);
    QCOMPARE(footer->entries().first().id,proof.first().id);QCOMPARE(footer->entries().first().sourceTimeMs,proof.first().sourceTimeMs);QCOMPARE(footer->entries().first().receivedAtMs,proof.first().receivedAtMs);
    QVERIFY(footer->isHidden());QVERIFY(hidden->parentWidget()->isHidden());
    auto* scroll=panel.findChild<QScrollArea*>("assetDetailScroll");QVERIFY(scroll);QVERIFY(scroll->widget()->width()<=scroll->viewport()->width());
    const auto out=qEnvironmentVariable("KEEFI_P3_WIDGET_RENDER_DIR");if(!out.isEmpty()){QVERIFY(QDir().mkpath(out));QVERIFY(panel.grab().save(out+(dark?"/detail-usd-dark.png":"/detail-usd-light.png")));}
    const auto pending=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{},now);QVERIFY(pending.waitingForRate());
    panel.showGroup(input,"2500000",pending);
    QCOMPARE(panel.findChild<QLabel*>("assetDetailHeadline")->text(),QStringLiteral("1,250,000 KRW"));
    QVERIFY(panel.findChild<QLabel*>("assetDetailHeadlineSub")->text().contains(QStringLiteral("환율 불러오는 중")));
    PublicMarketRow quote;quote.market="KRW-BTC";quote.base="BTC";quote.price="100000000";quote.previousClose="99000000";quote.signedChangeRate="0.01";quote.priced=true;
    panel.showMarket(quote,false,nullptr,false);QCOMPARE(panel.findChild<QLabel*>("assetDetailHeadline")->text(),QStringLiteral("100,000,000 KRW"));
    QVERIFY(!visibleText(panel).contains(QStringLiteral("환율 불러오는 중")));
    panel.clearDetail();for(const auto* label:panel.findChildren<QLabel*>())QVERIFY(!label->text().contains("SYNTHETIC_PRIVATE_ALIAS"));
}
void PortfolioDetailTest::privateEvidenceCannotBeDisclosedWhileSummaryRemains(){
    AssetDetailPanel panel;const auto input=group();panel.showGroup(input,"246912");panel.resize(340,900);panel.show();
    auto* disclosure=panel.findChild<QPushButton*>("assetDetailEvidence");auto* details=panel.findChild<QWidget*>("assetDetailHoldings");QVERIFY(disclosure);QVERIFY(details);
    QVERIFY2(disclosure->isHidden(),"Private detail has summary rows without a detailed-evidence disclosure");
    QVERIFY(disclosure->text().isEmpty());QVERIFY(disclosure->toolTip().isEmpty());QVERIFY(disclosure->accessibleName().isEmpty());
    disclosure->setChecked(true);QVERIFY(details->isHidden());panel.showGroup(input,"246912");QVERIFY(disclosure->isHidden());QVERIFY(details->isHidden());
    QCOMPARE(panel.findChild<QLabel*>("assetDetailHeadline")->text(),QStringLiteral("123,456 KRW"));
    QVERIFY(panel.findChild<QWidget*>("assetDetailPlaces")->isVisibleTo(&panel));
    QVERIFY(visibleText(panel).contains(QStringLiteral("전체 비중")));
    auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer);QCOMPARE(footer->evidenceCount(),1);
    QCOMPARE(footer->entries().first().sourceTimeMs,input.holdings.first().priceEvidence.sourceTimeMs);
    QCOMPARE(footer->entries().first().receivedAtMs,input.holdings.first().priceEvidence.receivedAtMs);
}
void PortfolioDetailTest::publicTradePreviewFollowsBuildPolicy_data(){
    QTest::addColumn<QString>("section");QTest::addColumn<bool>("dark");
    for(const auto& section:QStringList{"coin","kr","us"})for(const bool dark:{false,true})
        QTest::newRow(qPrintable(section+(dark?"-dark":"-light")))<<section<<dark;
}
void PortfolioDetailTest::publicTradePreviewFollowsBuildPolicy(){
    QFETCH(QString,section);QFETCH(bool,dark);auto* theme=KeefiTheme::instance();const auto original=theme->mode();
    const auto restore=qScopeGuard([=]{theme->setMode(original);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    AssetDetailPanel panel;panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.enableExplorationControls();
    panel.setFixedWidth(340);panel.resize(340,900);panel.show();
    const QString id=section=="kr"?"005930":section=="us"?"SYN":"BTC";
    for(int pass=0;pass<2;++pass){
        if(pass)panel.clearDetail();
        if(section=="coin"){
            PublicMarketRow row;row.market="KRW-BTC";row.base="BTC";row.koreanName="합성 비트코인";row.price="100000000";row.previousClose="99000000";row.signedChangeRate="0.0101";row.priced=true;
            row.sourceTimeMs=QDateTime::currentMSecsSinceEpoch()-1000;row.receivedAtMs=row.sourceTimeMs+500;
            panel.showMarket(row,false,nullptr,false);
        }else{
            panel.showQuote("합성 공개 종목",id,"100","1",{}, {"합성 공개 시세"},"전일 대비",section=="kr"?"KRW":"USD",section+":"+id);
            panel.setPublicEvidence({{section+":"+id,section=="kr"?QStringLiteral("네이버 증권"):QStringLiteral("Nasdaq"),{},0,QDateTime::currentMSecsSinceEpoch(),true,"유효"}});
        }
        panel.setPublicSelection(section,id,false);QCoreApplication::processEvents();
        for(const auto* name:{"tradePreview","tradePreviewMode","tradePreviewAmount","tradePreviewSubmit","tradePayBox","tradeReceiveBox"})
            QVERIFY2(panel.findChildren<QWidget*>(name).isEmpty(),name);
        auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer);QVERIFY(footer->isHidden());QCOMPARE(footer->evidenceCount(),1);
        auto* scroll=panel.findChild<QScrollArea*>("assetDetailScroll");QVERIFY(scroll);QVERIFY(scroll->widget()->width()<=scroll->viewport()->width());
    }
    const auto out=qEnvironmentVariable("KEEFI_TOP4_RENDER_DIR");if(!out.isEmpty()){
        QVERIFY(QDir().mkpath(out));QVERIFY(panel.grab().save(out+"/readonly-detail-"+section+(dark?"-dark":"-light")+".png"));
    }
}
void PortfolioDetailTest::privateValuationHasLedgerEvidenceAndNoAvatar(){
    AssetDetailPanel panel;panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.showGroup(group());panel.resize(500,900);panel.show();QCoreApplication::processEvents();
    QVERIFY2(!panel.findChild<QWidget*>("avatarBadge"),"Selected-asset detail must show the existing ticker as text without a circular avatar");
    auto* headline=panel.findChild<QLabel*>("assetDetailHeadline");QVERIFY(headline);QCOMPARE(headline->text(),QStringLiteral("123,456 KRW"));
    QCOMPARE(headline->font().family(),KeefiFonts::bodyFamily());
    auto* footer=dynamic_cast<ExploreEvidenceFooter*>(panel.findChild<QWidget*>("assetDetailEvidenceFooter"));QVERIFY2(footer,"Selected valuation needs a provider and exact timestamp evidence footer");
    QCOMPARE(footer->evidenceCount(),1);QCOMPARE(footer->numbers(),QList<int>{1});
    QCOMPARE(footer->entries().first().provider,QStringLiteral("업비트"));QCOMPARE(footer->entries().first().sourceTimeMs,group().holdings.first().priceEvidence.sourceTimeMs);QVERIFY(footer->isHidden());QVERIFY(footer->findChildren<QLabel*>().isEmpty());
}
void PortfolioDetailTest::selectedPricesKeepExactSourceAndReceipt(){
    AssetDetailPanel panel;auto data=group();data.holdings.first().provider="binance"; // Storage differs from the actual public price source.
    data.holdings.first().priceEvidence.sourceTimeMs=0;
    auto second=data.holdings.first();second.connectionId="SYNTHETIC_OTHER_ACCOUNT";data.holdings.append(second);
    data.holdings.first().conversionEvidence={AssetEvidence{"Coinbase 공식 환산", "https://api.exchange.coinbase.com/products/USDT-USD/trades?secret=SYNTHETIC_SECRET", "SYNTHETIC_CONVERSION",1792600010000,1792600020000,true}};
    panel.showGroup(data);auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer);
    QCOMPARE(footer->evidenceCount(),2);QCOMPARE(footer->numbers(),(QList<int>{1,2,1}));
    QCOMPARE(footer->entries().size(),3);QCOMPARE(footer->entries()[0].provider,QStringLiteral("업비트"));QCOMPARE(footer->entries()[1].provider,QStringLiteral("Coinbase"));
    QCOMPARE(footer->entries()[0].sourceTimeMs,qint64(0));QCOMPARE(footer->entries()[0].receivedAtMs,qint64(1792600005000));
    QCOMPARE(footer->entries()[1].sourceTimeMs,qint64(1792600010000));QCOMPARE(footer->entries()[1].receivedAtMs,qint64(1792600020000));
    QVERIFY(footer->isHidden());QVERIFY(panel.findChild<QLabel*>("assetDetailReferences")->isHidden());
}
void PortfolioDetailTest::missingAndClearDropEveryReference(){
    AssetDetailPanel panel;panel.showGroup(group());panel.setValuationEvidenceNumbers({7,8,7});
    QVERIFY(panel.findChild<QLabel*>("assetDetailReferences")->isHidden());
    auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer->isHidden());
    auto* theme=KeefiTheme::instance();const auto original=theme->mode();theme->setMode(original==ThemeMode::Light?ThemeMode::Dark:ThemeMode::Light);QCoreApplication::processEvents();
    const bool remainsHidden=footer->isHidden();theme->setMode(original);
    QVERIFY2(remainsHidden,"Theme updates must not restore the standalone footer after an external screen evidence book owns the numbers");
    panel.showMissing({"SYNTHETIC_CONNECTION","SYNTHETIC_ALIAS","spot",{},false});
    QCOMPARE(footer->evidenceCount(),0);QVERIFY(panel.findChild<QLabel*>("assetDetailReferences")->text().isEmpty());
    panel.showGroup(group());QCOMPARE(footer->evidenceCount(),1);QVERIFY(footer->isHidden());
    panel.clearDetail();QCOMPARE(footer->evidenceCount(),0);
    for(const auto* label:panel.findChildren<QLabel*>())QVERIFY(!label->text().contains("SYNTHETIC"));
}
void PortfolioDetailTest::publicQuoteOnlyAcceptsTypedProvenance(){
    AssetDetailPanel panel;panel.enableExplorationControls();
    panel.showQuote("합성 종목","SYN","100","1",{}, {"임의 문자열 · 2026-10-22 01:26:40 수신"},"전일 대비","KRW","public:naver:SYN");
    auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer);QCOMPARE(footer->evidenceCount(),0);
    panel.setPublicEvidence({{"SYNTHETIC_PRIVATE_ID","네이버 증권",{},0,1792600005000,true,"지연"}});
    QCOMPARE(footer->evidenceCount(),1);QCOMPARE(footer->entries().first().sourceTimeMs,qint64(0));QCOMPARE(footer->entries().first().receivedAtMs,qint64(1792600005000));QCOMPARE(footer->entries().first().state,QStringLiteral("지연"));QVERIFY(footer->isHidden());
    auto* card=panel.findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(card);QCOMPARE(card->data().evidence.size(),1);
    QVERIFY(card->findChild<QLabel*>("metricEvidenceReference")->isHidden());
    PublicMarketRow row;row.market="KRW-SYN";row.base="SYN";row.priced=true;row.price="101";row.sourceTimeMs=1792600010000;row.receivedAtMs=1792600020000;
    panel.showMarket(row,false,nullptr,false);QCOMPARE(footer->entries().first().provider,QStringLiteral("업비트"));QCOMPARE(footer->entries().first().sourceTimeMs,qint64(1792600010000));QVERIFY(footer->isHidden());
}
void PortfolioDetailTest::publicMarketEvidenceMatchesQuoteAge_data(){
    QTest::addColumn<qint64>("sourceAgeMs");QTest::addColumn<qint64>("receivedAgeMs");QTest::addColumn<QString>("expectedState");
    QTest::newRow("stale-ten-minutes")<<qint64(600000)<<qint64(1000)<<QStringLiteral("지연");
    QTest::newRow("fresh")<<qint64(10000)<<qint64(5000)<<QStringLiteral("유효");
    // -1 means an absent timestamp; other negative ages represent future time.
    QTest::newRow("missing-source")<<qint64(-1)<<qint64(1000)<<QStringLiteral("미확인");
    QTest::newRow("missing-receipt")<<qint64(10000)<<qint64(-1)<<QStringLiteral("미확인");
    QTest::newRow("missing-both")<<qint64(-1)<<qint64(-1)<<QStringLiteral("미확인");
    QTest::newRow("future-source")<<qint64(-600000)<<qint64(1000)<<QStringLiteral("미확인");
    QTest::newRow("future-receipt")<<qint64(10000)<<qint64(-600000)<<QStringLiteral("미확인");
}
void PortfolioDetailTest::publicMarketEvidenceMatchesQuoteAge(){
    QFETCH(qint64,sourceAgeMs);QFETCH(qint64,receivedAgeMs);QFETCH(QString,expectedState);
    auto* theme=KeefiTheme::instance();const auto originalMode=theme->mode();
    const auto restore=qScopeGuard([=]{theme->setMode(originalMode);});
    const auto now=QDateTime::currentMSecsSinceEpoch();
    PublicMarketRow row;row.market="KRW-SYN";row.base="SYN";row.koreanName="합성 공개 시세";
    row.priced=true;row.price="101.25";row.previousClose="100";row.signedChangeRate="0.0125";
    row.sourceTimeMs=sourceAgeMs==-1?0:now-sourceAgeMs;row.receivedAtMs=receivedAgeMs==-1?0:now-receivedAgeMs;
    AssetDetailPanel panel;panel.enableExplorationControls();theme->setMode(ThemeMode::Light);
    panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.showMarket(row,false,nullptr,false);panel.resize(720,760);panel.show();
    auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");auto* card=panel.findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(footer);QVERIFY(card);
    QCOMPARE(footer->evidenceCount(),1);QCOMPARE(footer->numbers(),QList<int>{1});
    const auto quote=card->data();QCOMPARE(quote.value,QStringLiteral("101.25"));QCOMPARE(quote.evidence.size(),1);
    QCOMPARE(quote.evidence.first().sourceTimeMs,row.sourceTimeMs);QCOMPARE(quote.evidence.first().receivedAtMs,row.receivedAtMs);
    QVERIFY(quote.evidence.first().complete);
    if(sourceAgeMs==600000)QVERIFY(quote.publicInformation.contains(QStringLiteral("반영 시각이 10분 전입니다. 새로고침으로 다시 확인하세요.")));
    for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
        theme->setMode(mode);panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));QCoreApplication::processEvents();
        QCOMPARE(footer->entries().first().state,expectedState);QCOMPARE(card->data().evidence.first().state,expectedState);QVERIFY(footer->isHidden());
        auto* freshness=card->findChild<QLabel*>("metricFreshness");QVERIFY(freshness);
        QTextDocument state;state.setHtml(freshness->text());
        QCOMPARE(state.toPlainText(),expectedState==QStringLiteral("유효")?QString{}:expectedState==QStringLiteral("지연")?QStringLiteral("지연"):QStringLiteral("확인 필요"));
        QCOMPARE(card->data().value,quote.value);QCOMPARE(card->data().evidence.first().sourceTimeMs,row.sourceTimeMs);QCOMPARE(card->data().evidence.first().receivedAtMs,row.receivedAtMs);
        const auto output=qEnvironmentVariable("KEEFI_F03_RENDER_DIR");
        if(!output.isEmpty()){
            QVERIFY(QDir().mkpath(output));
            QVERIFY(panel.grab().save(QDir(output).filePath(QStringLiteral("upbit-%1-%2.png").arg(QString::fromLatin1(QTest::currentDataTag()),mode==ThemeMode::Light?QStringLiteral("light"):QStringLiteral("dark")))));
        }
    }
}
void PortfolioDetailTest::publicQuoteFreshnessUsesTheExistingSixtySecondBoundary(){
    constexpr qint64 now=1792800000000;PublicMarketRow row;row.market="KRW-SYN";row.priced=true;
    row.sourceTimeMs=now-60000;row.receivedAtMs=now-1000;
    QCOMPARE(publicMarketQuoteEvidence(row,now).state,QStringLiteral("유효"));
    --row.sourceTimeMs;const auto delayed=publicMarketQuoteEvidence(row,now);
    QCOMPARE(delayed.state,QStringLiteral("지연"));QCOMPARE(delayed.sourceTimeMs,row.sourceTimeMs);QCOMPARE(delayed.receivedAtMs,row.receivedAtMs);
}
QTEST_MAIN(PortfolioDetailTest)
#include "tst_portfolio_detail.moc"
