#include "TooltipTestSupport.h"
#include "keefi/HoldingChartPanel.h"
#include "keefi/InfoButton.h"
#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QAbstractItemView>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
using namespace keefi;
namespace {
const QString PhantomScope=QStringLiteral("Phantom은 지갑에 바로 있는 SOL만 읽어요. 스테이킹한 SOL은 아직 포함하지 않아요. jitoSOL 같은 스테이킹 토큰은 별도 토큰으로 보여요.");
const QString SessionMarketGuidance=QStringLiteral("여기서 고른 시장은 이번 실행에만 기억해요. 계속 쓰려면 그 시장을 관심 목록에 추가하세요.");
HoldingChartRow nativeSol(){
    HoldingChartRow row;row.assetKey="solana:native";row.symbol="SOL";row.displayName="솔라나";
    row.chart=WatchlistIdentity{"coin","upbit","KRW-SOL"};row.candidates={*row.chart,{"coin","binance","SOLUSDT"}};
    row.selectionRule=3;row.alertsEligible=true;row.valuationMarket="업비트 KRW-SOL";
    row.custodies={{"phantom","Phantom",HoldingCustodyState::Fresh,1728000000000}};return row;
}
}
class HoldingChartPanelTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase(){QVERIFY(settings_.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings_.path());}
    void holdingNameTooltipAndPriceCopyRemainLiteral() {
        auto row = nativeSol();
        row.displayName = QStringLiteral("<b>합성 보유 이름</b> & literal");
        HoldingChartPanel panel;
        panel.setRows({row});
        auto* tree = panel.findChild<QTreeWidget*>("chartHoldingAssets");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto* item = tree->topLevelItem(0);
        QVERIFY(item->text(0).startsWith(row.displayName));
        const auto name = keefi::test::projectTooltip(item->toolTip(0));
        QVERIFY(name.text.startsWith(row.displayName));
        QCOMPARE(name.text, item->text(0) + QLatin1Char('\n') + PhantomScope);
        const auto price = keefi::test::projectTooltip(item->toolTip(1));
        QCOMPARE(price.text, name.text);
        QCOMPARE(keefi::test::projectTooltip(item->toolTip(3)).text, name.text);
        QVERIFY(!name.image && !price.image);
        QVERIFY(!name.anchor && !price.anchor);
        QCOMPARE(name.resourceRequests + price.resourceRequests, 0);
        panel.clearPrivate();
        QCOMPARE(tree->topLevelItemCount(), 0);
    }
    void unsupportedAndDelayedAssetsStayVisibleAndClearWithPrivacy(){
        HoldingChartRow sol;sol.assetKey="solana:native";sol.symbol="SOL";sol.displayName="솔라나";
        sol.chart=WatchlistIdentity{"coin","upbit","KRW-SOL"};sol.candidates={*sol.chart,{"coin","binance","SOLUSDT"}};
        sol.custodies={{"phantom","Phantom",HoldingCustodyState::Delayed,1728000000000}};sol.valuationMarket="업비트 KRW-SOL";
        HoldingChartRow token;token.assetKey="solana:synthetic-mint";token.symbol="SOL";token.displayName="별도 토큰";token.chartReason="공개 시장 연결 없음";token.custodies=sol.custodies;
        HoldingChartPanel panel;panel.resize(500,650);panel.setRows({sol,token});panel.show();
        auto* tree=panel.findChild<QTreeWidget*>("chartHoldingAssets");QVERIFY(tree);QCOMPARE(tree->topLevelItemCount(),2);
        QVERIFY(tree->topLevelItem(0)->text(0).contains("지연"));QVERIFY(tree->topLevelItem(1)->text(0).contains("차트 지원 없음"));
        QSignalSpy activated(&panel,&HoldingChartPanel::chartActivated),selected(&panel,&HoldingChartPanel::marketSelected);
        tree->itemClicked(tree->topLevelItem(1),0);QCOMPARE(activated.count(),0);
        QVERIFY(panel.findChild<QLabel*>("chartHoldingReason")->text().contains("공개 시장 연결 없음"));
        tree->itemClicked(tree->topLevelItem(0),0);QCOMPARE(activated.count(),1);QCOMPARE(activated.first()[1].toString(),QString("coin:upbit:KRW-SOL"));
        auto* markets=panel.findChild<QComboBox*>("chartHoldingMarketSelector");QVERIFY(markets);QCOMPARE(markets->count(),2);
        markets->setCurrentIndex(1);markets->activated(1);QCOMPARE(selected.count(),1);QCOMPARE(selected.first()[0].toString(),QString("solana:native"));
        panel.clearPrivate();QCOMPARE(tree->topLevelItemCount(),0);QCOMPARE(markets->count(),0);
        QVERIFY(panel.findChild<QLabel*>("chartHoldingReason")->text().isEmpty());
        panel.setRows({token});QCOMPARE(tree->topLevelItemCount(),1);QVERIFY(!panel.findChild<QLabel*>("chartHoldingReason")->text().contains("Phantom"));
    }
    void phantomStakingScopeRequiresNativeIdentityAndCustody_data(){
        QTest::addColumn<QString>("assetKey");QTest::addColumn<QStringList>("providers");
        QTest::addColumn<bool>("delayed");QTest::addColumn<bool>("expected");
        QTest::newRow("phantom-native")<<QString("solana:native")<<QStringList{"phantom"}<<false<<true;
        QTest::newRow("phantom-native-delayed")<<QString("solana:native")<<QStringList{"phantom"}<<true<<true;
        QTest::newRow("mixed-native")<<QString("solana:native")<<QStringList{"upbit","phantom"}<<false<<true;
        QTest::newRow("exchange-only-native")<<QString("solana:native")<<QStringList{"upbit"}<<false<<false;
        QTest::newRow("phantom-same-symbol-token")<<QString("solana:synthetic-mint")<<QStringList{"phantom"}<<false<<false;
    }
    void phantomStakingScopeRequiresNativeIdentityAndCustody(){
        QFETCH(QString,assetKey);QFETCH(QStringList,providers);QFETCH(bool,delayed);QFETCH(bool,expected);
        auto row=nativeSol();row.assetKey=assetKey;row.custodies.clear();
        for(const auto& provider:providers)row.custodies.append({provider,provider=="phantom"?QString("Phantom"):QString("업비트"),
            delayed?HoldingCustodyState::Delayed:HoldingCustodyState::Fresh,1728000000000});
        if(assetKey!="solana:native"){row.chart.reset();row.candidates.clear();row.chartReason="공개 시장 연결 없음";}
        HoldingChartPanel panel;panel.resize(340,650);panel.setRows({row});panel.show();
        auto* tree=panel.findChild<QTreeWidget*>("chartHoldingAssets");auto* reason=panel.findChild<QLabel*>("chartHoldingReason");
        QVERIFY(tree);QVERIFY(reason);QCOMPARE(tree->topLevelItemCount(),1);
        tree->itemClicked(tree->topLevelItem(0),0);
        QCOMPARE(tree->topLevelItem(0)->toolTip(0).contains(PhantomScope),expected);
        QCOMPARE(reason->text().contains(PhantomScope),expected);
        auto* information=panel.findChild<InfoButton*>("chartHoldingInformation");QVERIFY(information);
        if(expected){
            QVERIFY(reason->isHidden());QVERIFY(reason->wordWrap());QCOMPARE(reason->text().count(PhantomScope),1);
            QVERIFY(information->isVisibleTo(&panel));QTest::mouseClick(information,Qt::LeftButton);
            QTRY_VERIFY(information->informationOpen());
            auto* text=information->findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(text);
            QCOMPARE(text->toPlainText(),reason->text());QCOMPARE(text->toPlainText().count(PhantomScope),1);
        }
        panel.clearPrivate();QCOMPARE(tree->topLevelItemCount(),0);QVERIFY(reason->text().isEmpty());QVERIFY(reason->isHidden());
        QVERIFY(!information->informationOpen());
        for(const auto* text:information->findChildren<QPlainTextEdit*>())QVERIFY(!text->toPlainText().contains(PhantomScope));
        // A later exchange-only selection must not inherit the prior wallet scope.
        auto exchange=nativeSol();exchange.custodies={{"upbit","업비트",HoldingCustodyState::Fresh,1728000000000}};
        panel.setRows({exchange});tree->itemClicked(tree->topLevelItem(0),0);
        QVERIFY(!reason->text().contains(PhantomScope));QVERIFY(!tree->topLevelItem(0)->toolTip(0).contains(PhantomScope));
    }
    void sessionMarketGuidanceIsAccessibleAndOnlyFollowsUserOverride(){
        auto row=nativeSol();auto other=row;other.assetKey="exchange:BTC";other.symbol="BTC";other.displayName="비트코인";
        other.chart=WatchlistIdentity{"coin","upbit","KRW-BTC"};other.candidates={*other.chart};other.custodies={{"upbit","업비트",HoldingCustodyState::Fresh,1728000000000}};
        HoldingChartPanel panel;panel.resize(340,650);panel.setRows({row,other});panel.show();
        auto* tree=panel.findChild<QTreeWidget*>("chartHoldingAssets");auto* markets=panel.findChild<QComboBox*>("chartHoldingMarketSelector");
        auto* reason=panel.findChild<QLabel*>("chartHoldingReason");QVERIFY(tree);QVERIFY(markets);QVERIFY(reason);
        QCOMPARE(markets->toolTip(),SessionMarketGuidance);QCOMPARE(markets->accessibleDescription(),SessionMarketGuidance);
        tree->itemClicked(tree->topLevelItem(0),0);QCOMPARE(markets->currentData().toString(),QString("coin:upbit:KRW-SOL"));
        QVERIFY(!reason->text().contains(SessionMarketGuidance));
        // Production refreshes rows and chart identity synchronously from this signal.
        connect(&panel,&HoldingChartPanel::marketSelected,&panel,[&](const QString& assetKey,const QString& marketKey){
            QCOMPARE(assetKey,row.assetKey);row.chart=WatchlistWorkspaceList::identityFromKey(marketKey);row.selectionRule=0;
            panel.setRows({row,other});panel.setChartIdentity(marketKey);
        });
        markets->activated(markets->currentIndex());QVERIFY(!reason->text().contains(SessionMarketGuidance));
        const auto binance=markets->findData("coin:binance:SOLUSDT");QVERIFY(binance>=0);
        markets->setCurrentIndex(binance);markets->activated(binance);
        QCOMPARE(reason->text().count(SessionMarketGuidance),1);QVERIFY(reason->text().contains(PhantomScope));
        QVERIFY(reason->isHidden());
        auto* information=panel.findChild<InfoButton*>("chartHoldingInformation");QVERIFY(information);
        QVERIFY(information->isVisibleTo(&panel));QTest::mouseClick(information,Qt::LeftButton);
        QTRY_VERIFY(information->informationOpen());
        auto* text=information->findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(text);
        QCOMPARE(text->toPlainText(),reason->text());QCOMPARE(text->toPlainText().count(SessionMarketGuidance),1);
        QVERIFY(text->toPlainText().contains(PhantomScope));
        auto* dialog=information->findChild<QDialog*>("infoDialog");QVERIFY(dialog);
        QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!information->informationOpen());
        tree->itemClicked(tree->topLevelItem(1),0);QVERIFY(!reason->text().contains(SessionMarketGuidance));
        tree->itemClicked(tree->topLevelItem(0),0);QVERIFY(!reason->text().contains(SessionMarketGuidance));
        markets->activated(markets->currentIndex());QVERIFY(!reason->text().contains(SessionMarketGuidance));
        const auto original=markets->findData("coin:upbit:KRW-SOL");QVERIFY(original>=0);
        markets->setCurrentIndex(original);markets->activated(original);QVERIFY(!reason->text().contains(SessionMarketGuidance));
        panel.clearPrivate();QVERIFY(reason->text().isEmpty());QCOMPARE(markets->count(),0);QVERIFY(markets->isHidden());
    }
    void repeatedChartRefreshPreservesKeyboardNavigation(){
        const auto sol=nativeSol();
        auto token=sol;token.assetKey="solana:synthetic-wrapped-mint";token.symbol="wSOL";token.displayName="Wrapped SOL";
        token.chart.reset();token.candidates.clear();token.alertsEligible=false;token.chartReason="공개 시장 연결 없음";
        HoldingChartPanel panel;panel.resize(500,650);panel.setRows({sol,token});panel.show();
        auto* tree=panel.findChild<QTreeWidget*>("chartHoldingAssets");QVERIFY(tree);
        const auto key=sol.chart->key();panel.setChartIdentity(key);
        tree->setFocus();QTest::keyClick(tree,Qt::Key_Down);
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),token.assetKey);
        // The owner's periodic refresh repeats the active chart identity while
        // the user is navigating to another holding, before activating it.
        panel.setRows({sol,token});panel.setChartIdentity(key);
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),token.assetKey);
        QSignalSpy activated(&panel,&HoldingChartPanel::chartActivated);
#ifdef Q_OS_MACOS
        QTest::keyClick(tree,Qt::Key_O,Qt::ControlModifier);
#else
        QTest::keyClick(tree,Qt::Key_Return);
#endif
        QCOMPARE(activated.count(),0);
        auto* reason=panel.findChild<QLabel*>("chartHoldingReason");QVERIFY(reason);
        QVERIFY(reason->text().contains(token.chartReason));
        panel.setRows({sol,token});panel.setChartIdentity(key);
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),token.assetKey);
        QVERIFY(reason->text().contains(token.chartReason));
    }
    void custodyRefreshPreservesThePendingKeyboardRow(){
        auto sol=nativeSol();auto other=sol;other.assetKey="exchange:BTC";other.symbol="BTC";
        other.chart=WatchlistIdentity{"coin","upbit","KRW-BTC"};other.candidates={*other.chart};
        HoldingChartPanel panel;panel.resize(500,650);panel.setRows({sol,other});panel.show();
        auto* tree=panel.findChild<QTreeWidget*>("chartHoldingAssets");QVERIFY(tree);
        panel.setChartIdentity(sol.chart->key());tree->setFocus();QTest::keyClick(tree,Qt::Key_Down);
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),other.assetKey);
        sol.custodies[0].state=HoldingCustodyState::Delayed;sol.alertsEligible=false;
        panel.setRows({sol,other});panel.setChartIdentity(sol.chart->key());
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),other.assetKey);
        QVERIFY(tree->topLevelItem(0)->text(0).contains("지연"));
        // A real chart change still synchronizes the list, including after lock.
        panel.setChartIdentity(other.chart->key());panel.setChartIdentity(sol.chart->key());
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),sol.assetKey);
        panel.clearPrivate();panel.setRows({sol,other});panel.setChartIdentity(other.chart->key());
        QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).toString(),other.assetKey);
    }
    void marketPopupChoiceSurvivesChartAndCustodyRefresh(){
        auto sol=nativeSol();HoldingChartPanel panel;panel.resize(500,650);panel.setRows({sol});panel.show();
        panel.setChartIdentity(sol.chart->key());
        auto* markets=panel.findChild<QComboBox*>("chartHoldingMarketSelector");QVERIFY(markets);
        QSignalSpy selected(&panel,&HoldingChartPanel::marketSelected);
        auto* tree=panel.findChild<QTreeWidget*>("chartHoldingAssets");QVERIFY(tree);
        QTRY_VERIFY(!tree->visualItemRect(tree->topLevelItem(0)).isEmpty());
        const auto position=tree->visualItemRect(tree->topLevelItem(0)).center();
        QContextMenuEvent context(QContextMenuEvent::Mouse,position,tree->viewport()->mapToGlobal(position));
        QCoreApplication::sendEvent(tree->viewport(),&context);
        auto* menu=panel.findChild<QMenu*>("chartHoldingContextMenu");QVERIFY(menu);QTRY_VERIFY(menu->isVisible());
        auto* choices=menu->findChild<QMenu*>("chartHoldingChartSources");QVERIFY(choices);
        QTest::keyClick(menu,Qt::Key_Home);
        for(int i=0;i<menu->actions().size()&&menu->activeAction()!=choices->menuAction();++i)QTest::keyClick(menu,Qt::Key_Down);
        QCOMPARE(menu->activeAction(),choices->menuAction());
        QTest::keyClick(menu,Qt::Key_Right);QTRY_VERIFY(choices->isVisible());
        QTest::keyClick(choices,Qt::Key_Home);
        QTest::keyClick(choices,Qt::Key_Down);
        QVERIFY(choices->activeAction());QCOMPARE(choices->activeAction()->data().toString(),QString("coin:binance:SOLUSDT"));
        panel.setRows({sol});panel.setChartIdentity(sol.chart->key());
        QVERIFY(choices->isVisible());
        QVERIFY(choices->activeAction());QCOMPARE(choices->activeAction()->data().toString(),QString("coin:binance:SOLUSDT"));
        // A custody update changes the explanation, not the market choices.
        sol.custodies[0].state=HoldingCustodyState::Delayed;sol.alertsEligible=false;
        panel.setRows({sol});panel.setChartIdentity(sol.chart->key());
        QVERIFY(choices->isVisible());
        QVERIFY(choices->activeAction());QCOMPARE(choices->activeAction()->data().toString(),QString("coin:binance:SOLUSDT"));
        QCOMPARE(selected.count(),0);
        QTest::keyClick(choices,Qt::Key_Return);
        QCOMPARE(selected.count(),1);
        QCOMPARE(selected.first()[1].toString(),QString("coin:binance:SOLUSDT"));
        sol.chart=WatchlistIdentity{"coin","binance","SOLUSDT"};sol.selectionRule=0;
        panel.setRows({sol});panel.setChartIdentity(sol.chart->key());
        QCOMPARE(markets->currentData().toString(),QString("coin:binance:SOLUSDT"));
        // A removed candidate must still disappear; preserving a popup does
        // not preserve a market which the resolver no longer permits.
        sol.chart=WatchlistIdentity{"coin","upbit","KRW-SOL"};sol.candidates={*sol.chart};
        panel.setRows({sol});panel.setChartIdentity(sol.chart->key());
        QCOMPARE(markets->count(),1);QCOMPARE(markets->findData("coin:binance:SOLUSDT"),-1);
        QCOMPARE(markets->currentData().toString(),QString("coin:upbit:KRW-SOL"));QCOMPARE(selected.count(),1);
        panel.clearPrivate();QCOMPARE(markets->count(),0);QVERIFY(markets->isHidden());
    }
};
QTEST_MAIN(HoldingChartPanelTest)
#include "tst_holding_chart_panel.moc"
