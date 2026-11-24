#include "keefi/ExploreMetricCard.h"
#include "keefi/InfoButton.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PublicMarketCatalog.h"
#include <QLabel>
#include <QPlainTextEdit>
#include <QTest>
#include <QVBoxLayout>
#include <QDir>
#include <QTextDocument>
#include <QScrollArea>
#include <QDateTime>
#include <QSettings>
#include <QTemporaryDir>
#include <limits>
using namespace keefi;
namespace {
constexpr qint64 First=1792713600000;
ExploreMetricCardData sample() {
    ExploreMetricCardData data;data.id="synthetic-public";data.title="공개 가격";data.value="102.00";data.unit="KRW";data.changeText="+2.00%";
    data.freshnessText="원천 시각 미제공 · 수신 합성 시각";data.sourceKey="fixture:public:SYN/KRW";data.publicInformation="공개 원천과 방법";data.direction=MovementDirection::Up;
    data.history={true,data.sourceKey,data.unit,{},{{First,100,false},{First+60000,102,true}}};return data;
}
}
class ExploreMetricCardTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
  private slots:
    void initTestCase(){
        QVERIFY(settingsRoot_.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsRoot_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settingsRoot_.filePath("system"));
    }
    void evidenceRemainsInternalWithoutReferencesOrFooter();
    void sourceDetailsStayInMemoryWhileDisplayedInformationIsCompact();
    void errorLabelsNeverExposeTransportDetails();
    void ledgerRowUsesNumericRolesAndActualCompactHistory();
    void selectedDetailFits340Pixels_data();
    void selectedDetailFits340Pixels();
    void refreshedDetailNeverDrawsAnotherLayout();
    void optionalSyntheticExploreLedgerRenders();
    void displaysPublicValueFreshnessAndActualPoints();
    void unavailableHistoryCollapses_data();
    void unavailableHistoryCollapses();
    void removingHistoryActuallyReleasesLayoutSpace();
    void evidenceDeduplicatesWithoutDisplayingPrivateIdentifiers();
    void evidenceUsesOriginalTimesAndRejectsUnknownProviderState();
    void gapsRetainObservedTimesAndConnectionFlags();
    void changingWindowSchemeRecolorsWithoutChangingSourceData();
    void privateLinkClearKeepsPublicValueAndHistory();
};
void ExploreMetricCardTest::evidenceRemainsInternalWithoutReferencesOrFooter() {
    QWidget host;auto* layout=new QVBoxLayout(&host);auto* card=new ExploreMetricCard(&host);auto* footer=new ExploreEvidenceFooter(&host);
    auto data=sample();data.evidence={{"synthetic-binding","업비트",{},First,First+60000,true,"유효"}};
    card->setData(data);card->setEvidenceNumbers({7});footer->setEntries(data.evidence,7);layout->addWidget(card);layout->addWidget(footer);
    host.show();QCoreApplication::processEvents();
    QCOMPARE(card->data(),data);QCOMPARE(footer->evidenceCount(),1);QCOMPARE(footer->numbers(),QList<int>{7});
    QVERIFY2(footer->isHidden(),"Evidence remains available internally without a visible footer");
    for(const auto* text:host.findChildren<QLabel*>())if(text->isVisible())QVERIFY2(!text->text().contains("[7]"),qPrintable(text->objectName()));
}
void ExploreMetricCardTest::sourceDetailsStayInMemoryWhileDisplayedInformationIsCompact() {
    ExploreMetricCard card;auto data=sample();
    data.evidence={{"coin:upbit:KRW-SYN","업비트",{},First,First+60000,true,"지연"}};
    data.freshnessText="원천 2026-10-23 09:00:00 KST · 수신 09:01:00 KST · 지연";
    data.publicInformation="계산 기준은 원래 관측값입니다.\nhttps://api.upbit.com/v1/ticker?private=SYNTHETIC\ncoin:upbit:KRW-SYN\n원천 2026-10-23 09:00:00 KST · 수신 09:01:00 KST";
    card.setData(data);card.show();auto* freshness=card.findChild<QLabel*>("metricFreshness");QVERIFY(freshness);
    QTextDocument text;text.setHtml(freshness->text());QCOMPARE(text.toPlainText(),QStringLiteral("지연"));
    QCOMPARE(card.data(),data);card.informationButton()->openInformation();
    auto* information=card.findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(information);
    const auto shown=information->toPlainText();QVERIFY(shown.contains(QStringLiteral("계산 기준은 원래 관측값입니다.")));
    QVERIFY(shown.contains(QStringLiteral("업비트 · 09:00 KST · UTC+9 기준")));QVERIFY(!shown.contains("https://"));QVERIFY(!shown.contains("coin:"));QVERIFY(!shown.contains(QStringLiteral("수신")));
    data.evidence[0].state="유효";data.freshnessText="원천 2026-10-23 09:00:00 KST · 수신 09:01:00 KST";card.setData(data);
    QVERIFY(freshness->isHidden());QCOMPARE(card.data(),data);
}
void ExploreMetricCardTest::errorLabelsNeverExposeTransportDetails(){
    auto data=sample();data.title="공개 가격 [9]";data.value="확인 필요 https://example.test/request?binding=SYNTHETIC coin:upbit:KRW-SYN";
    data.unit="";data.changeText="조회 실패 wss://example.test/stream [9]";
    ExploreMetricCard card;card.setData(data);card.show();
    QCOMPARE(card.data(),data);QCOMPARE(card.findChild<QLabel*>("metricValue")->text(),QStringLiteral("확인 필요"));
    for(const auto* label:card.findChildren<QLabel*>())if(label->isVisibleTo(&card)){
        QVERIFY(!label->text().contains("://"));QVERIFY(!label->text().contains("coin:"));QVERIFY(!label->text().contains("[9]"));
    }
}
void ExploreMetricCardTest::selectedDetailFits340Pixels_data() {
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ExploreMetricCardTest::selectedDetailFits340Pixels() {
    QFETCH(bool,dark);KeefiFonts::registerBundledFonts();const auto previous=KeefiTheme::instance()->mode();
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    AssetDetailPanel panel;panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.enableExplorationControls();
    PublicMarketRow data;data.market="KRW-SYN";data.base="SYN";data.koreanName="합성 공개 종목";data.price="109771433";data.previousClose="100000000";data.signedChangeRate="0.09771433";data.priced=true;
    data.sourceTimeMs=QDateTime::currentMSecsSinceEpoch()-1000;data.receivedAtMs=data.sourceTimeMs+500;
    panel.showMarket(data,false,nullptr,false);panel.setFixedWidth(340);panel.resize(340,900);panel.show();QCoreApplication::processEvents();
    auto* scroll=panel.findChild<QScrollArea*>("assetDetailScroll");QVERIFY(scroll);
    QVERIFY2(scroll->widget()->width()<=scroll->viewport()->width(),"A selected public quote must not expand outside the fixed 340px detail viewport");
    auto* card=panel.findChild<ExploreMetricCard*>("assetDetailMetric");QVERIFY(card);
    for(const auto* name:{"metricValue","metricUnit","metricChange"}){
        auto* label=card->findChild<QLabel*>(name);QVERIFY(label);QVERIFY(label->isVisible());
        const QRect bounds(label->mapTo(scroll->viewport(),QPoint()),label->size());
        QVERIFY2(bounds.left()>=0&&bounds.right()<scroll->viewport()->width(),name);
    }
    auto* footer=panel.findChild<ExploreEvidenceFooter*>("assetDetailEvidenceFooter");QVERIFY(footer);
    for(auto* line:footer->findChildren<QLabel*>())QVERIFY(line->width()<=scroll->viewport()->width());
    data.sourceTimeMs=QDateTime::currentMSecsSinceEpoch()-600000;panel.showMarket(data,false,nullptr,false);QCoreApplication::processEvents();
    auto* freshness=card->findChild<QLabel*>("metricFreshness");QVERIFY(freshness&&freshness->isVisible());
    QTextDocument state;state.setHtml(freshness->text());QCOMPARE(state.toPlainText(),QStringLiteral("지연"));
    for(const auto* name:{"metricValue","metricUnit","metricChange","metricFreshness"}){
        const auto* value=card->findChild<QLabel*>(name);const QRect bounds(value->mapTo(scroll->viewport(),QPoint()),value->size());
        QVERIFY2(bounds.left()>=0&&bounds.right()<scroll->viewport()->width(),name);
    }
    const auto out=qEnvironmentVariable("KEEFI_FINAL_F01_RENDER_DIR");if(!out.isEmpty()){
        QVERIFY(QDir().mkpath(out));QVERIFY(panel.grab().save(out+"/detail-340-"+(dark?"dark":"light")+".png"));
    }
    KeefiTheme::instance()->setMode(previous);
}
void ExploreMetricCardTest::refreshedDetailNeverDrawsAnotherLayout() {
    // A quote refresh used to show the ledger head for a moment. Qt lays out a shown widget at once
    // but a hidden one only later, so a frame drawn in between had the card 48px lower.
    KeefiFonts::registerBundledFonts();
    AssetDetailPanel panel;panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.enableExplorationControls();panel.resize(340,900);panel.show();
    const QString key="coin:coingecko:synthetic";
    const auto quote=[&](const QString& price,const QList<QPair<QString,QString>>& facts){
        panel.showQuote("Synthetic","SYN · Layer 1",price+" KRW","3.22",facts,{"CoinGecko · 10-28 17:58:01 수신","공개 시세"},"최근 24시간","KRW",key,false);
        panel.setPublicEvidence({{key,"CoinGecko",{},0,QDateTime::currentMSecsSinceEpoch()-1000,true,"유효"}});
        panel.setPublicSelection("coin","SYN",false);panel.setPublicHistoryContext(key,"KRW");panel.setChartIdentity({});panel.setPublicIndicators({});
    };
    const QList<QPair<QString,QString>> facts{{"24시간 거래대금","32.2조원"},{"시가총액","2192.8조원"}};
    quote("109,073,374",facts);QCoreApplication::processEvents();
    auto* card=panel.findChild<ExploreMetricCard*>("assetDetailMetric");auto* row=panel.findChild<QWidget*>("assetDetailPlace1");QVERIFY(card&&row);
    const auto cardAt=card->mapTo(&panel,QPoint()),rowAt=row->mapTo(&panel,QPoint());
    struct ShowWatch final : QObject {QStringList shown;bool eventFilter(QObject* o,QEvent* e)override{if(e->type()==QEvent::Show)shown<<o->objectName();return false;}} watch;
    for(auto* widget:panel.findChildren<QWidget*>())widget->installEventFilter(&watch);
    quote("109,073,375",facts);
    // Read before the event loop runs: this is what a frame drawn right now would show.
    QCOMPARE(card->mapTo(&panel,QPoint()),cardAt);QCOMPARE(row->mapTo(&panel,QPoint()),rowAt);
    QCOMPARE(panel.findChild<QWidget*>("assetDetailPlace1"),row);QVERIFY2(watch.shown.isEmpty(),qPrintable(watch.shown.join(", ")));
    QCOMPARE(row->findChild<QLabel*>("assetDetailPlaceValue1")->text(),QString("2192.8조원"));
    QCoreApplication::processEvents();QCOMPARE(card->mapTo(&panel,QPoint()),cardAt);QCOMPARE(row->mapTo(&panel,QPoint()),rowAt);
    // One fewer fact removes a row in the same pass.
    quote("109,073,376",facts.mid(1));QVERIFY(!panel.findChild<QWidget*>("assetDetailPlace1"));
    QCOMPARE(panel.findChild<QLabel*>("assetDetailPlaceValue0")->text(),QString("2192.8조원"));
}
void ExploreMetricCardTest::ledgerRowUsesNumericRolesAndActualCompactHistory() {
    ExploreMetricCard card;card.setData(sample());card.resize(900,100);card.show();
    auto* value=card.findChild<QLabel*>("metricValue");auto* unit=card.findChild<QLabel*>("metricUnit");
    auto* chart=card.findChild<QWidget*>("metricSparkline");auto* rule=card.findChild<QWidget*>("metricTopRule");
    QVERIFY(value);QCOMPARE(value->font(),KeefiFonts::font(FontRole::Value22));
    QVERIFY(unit);QCOMPARE(unit->text(),QString("KRW"));QCOMPARE(unit->font(),KeefiFonts::font(FontRole::Number11));
    QVERIFY(chart);QCOMPARE(chart->size(),QSize(110,24));QVERIFY(rule);
    auto timed=sample();timed.freshnessText="원천 2026-10-23 09:00:00 KST · 수신 09:01:00 KST";card.setData(timed);
    auto* freshness=card.findChild<QLabel*>("metricFreshness");QVERIFY(freshness);QTextDocument displayed;displayed.setHtml(freshness->text());QVERIFY(displayed.toPlainText().isEmpty());QVERIFY(freshness->isHidden());
    QCOMPARE(card.data().freshnessText,timed.freshnessText);
    QCoreApplication::processEvents();auto* title=card.findChild<QLabel*>("metricTitle");
    QVERIFY(value->mapTo(&card,QPoint()).x()>title->mapTo(&card,QPoint(title->width()-1,0)).x());
}
void ExploreMetricCardTest::evidenceDeduplicatesWithoutDisplayingPrivateIdentifiers() {
    ExploreEvidenceFooter footer;QVERIFY(footer.isHidden());ExploreMetricEvidence a{"FAKE_PRIVATE_SCOPE/key/alias", "업비트",{},First,First+60000,true,"유효"};
    auto b=a;b.id="FAKE_OTHER_ACCOUNT";footer.setEntries({a,a,b},7);footer.resize(900,200);footer.show();QCoreApplication::processEvents();
    footer.setEntries({a,a,b},7);
    QCOMPARE(footer.evidenceCount(),2);QCOMPARE(footer.numbers(),QList<int>({7,7,8}));
    QCOMPARE(footer.entries(),(QList<ExploreMetricEvidence>{a,a,b}));
    QVERIFY(footer.findChildren<QLabel*>().isEmpty());
    auto uncertain=a;uncertain.state="미확인";footer.setEntries({a,uncertain},7);QCOMPARE(footer.evidenceCount(),1);QCOMPARE(footer.numbers(),QList<int>({7,7}));QCOMPARE(footer.entries(),(QList<ExploreMetricEvidence>{a,uncertain}));QVERIFY(footer.isHidden());
    auto differentProvider = a; differentProvider.provider = "업비트|원천";
    auto differentDate = a; differentDate.sourceDate = "2026-10-23";
    auto differentSourceTime = a; ++differentSourceTime.sourceTimeMs;
    auto differentReceipt = a; ++differentReceipt.receivedAtMs;
    auto incomplete = a; incomplete.complete = false;
    const QList<ExploreMetricEvidence> interleaved{a, differentProvider, b, differentDate,
        uncertain, differentSourceTime, differentReceipt, incomplete, b, differentDate};
    footer.setEntries(interleaved, 3);
    QCOMPARE(footer.numbers(), QList<int>({3, 4, 5, 6, 3, 7, 8, 9, 5, 6}));
    QCOMPARE(footer.evidenceCount(), 7);
    QCOMPARE(footer.entries(), interleaved);
    footer.setEntries({});QVERIFY(footer.isHidden());
    ExploreMetricCard card;card.setData(sample());card.setEvidenceNumbers({7,7,8});QVERIFY(card.findChild<QLabel*>("metricEvidenceReference")->isHidden());QVERIFY(card.findChild<QLabel*>("metricEvidenceReference")->text().isEmpty());
}
void ExploreMetricCardTest::evidenceUsesOriginalTimesAndRejectsUnknownProviderState() {
    ExploreEvidenceFooter footer;const QList<ExploreMetricEvidence> original{{"private-binding", "SECRET_PROVIDER","2026-10-23",0,First+60000,true,"SECRET_STATE"}};
    footer.setEntries(original);QCOMPARE(footer.entries(),original);QCOMPARE(footer.evidenceCount(),1);
    QVERIFY(footer.isHidden());QVERIFY(footer.findChildren<QLabel*>().isEmpty());
    auto replaced=original;replaced[0].provider="Binance";replaced[0].sourceDate={};replaced[0].sourceTimeMs=First;
    footer.setEntries(replaced);QCOMPARE(footer.entries(),replaced);QVERIFY(footer.isHidden());
}

void ExploreMetricCardTest::optionalSyntheticExploreLedgerRenders() {
    const auto directory=qEnvironmentVariable("KEEFI_D4_RENDER_DIR");if(directory.isEmpty())QSKIP("Optional synthetic D4 explore ledger PNG artifacts; set KEEFI_D4_RENDER_DIR to export.");
    QVERIFY(QDir().mkpath(directory));const auto original=KeefiTheme::instance()->mode();
    for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
        KeefiTheme::instance()->setMode(mode);QWidget view;view.setStyleSheet(KeefiTheme::localStyleSheet(&view));auto* rows=new QVBoxLayout(&view);rows->setContentsMargins(40,26,40,26);rows->setSpacing(0);
        auto* title=new QLabel(QStringLiteral("지금 볼 시장 맥락"),&view);title->setFont(KeefiFonts::font(FontRole::Title17));rows->addWidget(title);
        auto data=sample();data.title="합성 공개 가격";data.evidence={{"fixture:public:SYN/KRW","업비트",{},First,First+60000,true,"유효"}};
        auto* first=new ExploreMetricCard(&view);first->setData(data);first->setEvidenceNumbers({1});rows->addWidget(first);
        auto absent=data;absent.id="synthetic-daily";absent.title="미국 10년물 금리";absent.value="4.25";absent.unit="%";absent.changeText={};absent.direction=MovementDirection::Unknown;absent.history={};absent.freshnessText="일별 공시 · 미국 관측 2026-10-23";absent.evidence={{"treasury:10year","미국 재무부","2026-10-23",0,First+120000,true,"일별 공시"}};
        auto* second=new ExploreMetricCard(&view);second->setData(absent);second->setEvidenceNumbers({2});rows->addWidget(second);
        auto missing=absent;missing.id="synthetic-missing";missing.title="공시 펀딩비율";missing.value="조회 대기";missing.unit={};missing.freshnessText="아직 수신하지 않음";missing.evidence={};
        auto* third=new ExploreMetricCard(&view);third->setData(missing);rows->addWidget(third);
        auto* footer=new ExploreEvidenceFooter(&view);footer->setEntries(data.evidence+absent.evidence);rows->addWidget(footer);rows->addStretch(1);
        for(const int width:{1280,720}){view.resize(width,580);view.show();QCoreApplication::processEvents();QVERIFY(view.grab().save(QDir(directory).filePath(QString("explore-ledger-%1-%2.png").arg(mode==ThemeMode::Light?"light":"dark").arg(width))));}
    }
    KeefiTheme::instance()->setMode(original);
}
void ExploreMetricCardTest::displaysPublicValueFreshnessAndActualPoints() {
    ExploreMetricCard card;const auto data=sample();card.setData(data);card.show();QVERIFY(card.informationButton());
    auto* title=card.findChild<QLabel*>("metricTitle");auto* freshness=card.findChild<QLabel*>("metricFreshness");auto* value=card.findChild<QLabel*>("metricValue");
    QVERIFY(title);QVERIFY(freshness);QVERIFY(value);QCOMPARE(title->text(),data.title);QTextDocument visibleFreshness;visibleFreshness.setHtml(freshness->text());QVERIFY(visibleFreshness.toPlainText().isEmpty());QVERIFY(freshness->isHidden());QVERIFY(value->text().contains(data.value));
    QVERIFY(card.historyVisible());QCOMPARE(card.plottedHistory(),data.history.points);QCOMPARE(card.data(),data);
}
void ExploreMetricCardTest::unavailableHistoryCollapses_data() {
    QTest::addColumn<QString>("fault");for(const auto* value:{"zero","one","duplicate","descending","nonfinite","bad-time","source-mismatch","unit-mismatch","unavailable","overbound"})QTest::newRow(value)<<QString(value);
}
void ExploreMetricCardTest::unavailableHistoryCollapses() {
    QFETCH(QString,fault);auto data=sample();
    if(fault=="zero")data.history.points.clear();if(fault=="one")data.history.points.removeLast();
    if(fault=="duplicate")data.history.points[1].atMs=First;if(fault=="descending")data.history.points[1].atMs=First-1;
    if(fault=="nonfinite")data.history.points[1].value=std::numeric_limits<double>::infinity();if(fault=="bad-time")data.history.points[0].atMs=0;
    if(fault=="source-mismatch")data.history.sourceKey="fixture:OTHER";if(fault=="unit-mismatch")data.history.unit="USD";
    if(fault=="unavailable"){data.history.available=false;data.history.reason="원천 만료";}
    if(fault=="overbound"){data.history.points.clear();for(int i=0;i<=ExploreMetricCard::MaxHistoryPoints;++i)data.history.points.append({First+60000*i,100,true});}
    ExploreMetricCard card;card.setData(sample());QVERIFY(card.historyVisible());card.setData(data);card.show();QVERIFY(!card.historyVisible());QVERIFY(card.plottedHistory().isEmpty());
    auto* chart=card.findChild<QWidget*>("metricSparkline");QVERIFY(chart);QVERIFY(chart->isHidden());QCOMPARE(card.data().freshnessText,data.freshnessText);
}
void ExploreMetricCardTest::removingHistoryActuallyReleasesLayoutSpace() {
    ExploreMetricCard card;card.resize(900,100);auto data=sample();card.setData(data);card.show();QCoreApplication::processEvents();QVERIFY(card.historyVisible());
    auto* title=card.findChild<QLabel*>("metricTitle");QVERIFY(title);const auto used=title->geometry().width();
    data.history.points.clear();card.setData(data);QVERIFY(!card.historyVisible());
    QTRY_VERIFY(title->geometry().width()>=used+110);
}
void ExploreMetricCardTest::gapsRetainObservedTimesAndConnectionFlags() {
    auto data=sample();data.history.points[1]={First+4*60000,102,false};ExploreMetricCard card;card.setData(data);card.show();
    QVERIFY(!card.historyVisible());QCOMPARE(card.plottedHistory(),data.history.points);QCOMPARE(card.plottedHistory().size(),2);QVERIFY(!card.plottedHistory()[1].connectsPrevious);
}
void ExploreMetricCardTest::changingWindowSchemeRecolorsWithoutChangingSourceData() {
    QWidget host;MovementColorPolicy policy(&host);auto* layout=new QVBoxLayout(&host);auto* card=new ExploreMetricCard(&host);layout->addWidget(card);const auto data=sample();card->setData(data);host.show();
    auto* change=card->findChild<QLabel*>("metricChange");QVERIFY(change);QCOMPARE(change->palette().color(QPalette::WindowText),themeColors().upText);
    QTextDocument changeDocument;changeDocument.setHtml(change->text());QVERIFY(changeDocument.toPlainText().startsWith("▲"));
    policy.setScheme(MovementScheme::KoreanRedBlue);QTRY_COMPARE(change->palette().color(QPalette::WindowText),themeColors().downText);QCOMPARE(card->data(),data);QCOMPARE(card->plottedHistory(),data.history.points);
    ExploreMetricCard late(&host);late.setData(data);auto* lateChange=late.findChild<QLabel*>("metricChange");QVERIFY(lateChange);QCOMPARE(lateChange->palette().color(QPalette::WindowText),themeColors().downText);
}
void ExploreMetricCardTest::privateLinkClearKeepsPublicValueAndHistory() {
    ExploreMetricCard card;const auto data=sample();card.setData(data);card.setPrivateAccess(true);QVERIFY(card.setPrivateInformation("FAKE_PRIVATE_LINK"));card.show();
    auto* info=card.informationButton();QVERIFY(info);info->openInformation();QTRY_VERIFY(info->informationOpen());card.clearPrivate();
    QVERIFY(!info->informationOpen());QCOMPARE(card.data(),data);QCOMPARE(card.plottedHistory(),data.history.points);QVERIFY(!card.setPrivateInformation("FAKE_PRIVATE_LATE"));
    for(const auto* text:card.findChildren<QPlainTextEdit*>())QVERIFY(!text->toPlainText().contains("FAKE_PRIVATE"));
}
QTEST_MAIN(ExploreMetricCardTest)
#include "tst_explore_metric_card.moc"
