#include "TooltipTestSupport.h"
#include <QAccessible>
#include <QToolButton>
#include "keefi/PortfolioDetail.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/TradingTransport.h"
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTextDocument>
#include <QUrlQuery>
#include "keefi/KeefiFonts.h"
#include "keefi/InfoButton.h"
#include "keefi/Spotlight.h"
#include <QLabel>
#include "keefi/MovementColorPolicy.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/UsabilityPresentation.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QSignalSpy>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QMouseEvent>
#include <QDateTime>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <climits>
#include <QPaintEngine>
#include <QPainter>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QStyleOptionViewItem>
#include <QToolTip>

using namespace keefi;

namespace {
constexpr qint64 SourceAt = 1792713600000; // Existing synthetic fixture: 09:00 KST.

// The public world board as read on 2026-11-05 (Naver, ECB, MOF), stamped at SourceAt.
WorldMarketsSnapshot syntheticWorld(qint64 at) {
    WorldMarketsSnapshot s;
    const auto quote=[&](WorldMarketItem item,const QString& price,const QString& change,const QString& percent,WorldMarketSession session,int delay,const QString& venue){
        auto& q=s.quotes[int(item)];q.price=price;q.change=change;q.changePercent=percent;q.session=session;q.tradedAtMs=at-60000;
        q.localDate=QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).date();q.delayMinutes=delay;q.venue=venue;q.receivedAtMs=at;};
    quote(WorldMarketItem::Spx,"7688.62","119.70","1.58",WorldMarketSession::Closed,0,"뉴욕 거래소");
    quote(WorldMarketItem::Ndx,"28437.17","256.90","0.91",WorldMarketSession::Closed,0,"나스닥 증권거래소");
    quote(WorldMarketItem::Kospi,"5383.80","-300.00","-5.28",WorldMarketSession::Closed,0,"코스피");
    quote(WorldMarketItem::Nikkei,"61077.90","-1110.73","-1.79",WorldMarketSession::Closed,15,"도쿄 거래소");
    quote(WorldMarketItem::HangSeng,"24796.98","877.48","3.67",WorldMarketSession::Closed,15,"홍콩 거래소");
    quote(WorldMarketItem::Ftse,"10916.70","87.49","0.81",WorldMarketSession::Open,15,"런던증권거래소");
    quote(WorldMarketItem::Gold,"4137.00","64.00","1.57",WorldMarketSession::Open,10,"뉴욕상품거래소");
    quote(WorldMarketItem::Silver,"52.99","0.70","1.34",WorldMarketSession::Open,10,"뉴욕상품거래소");
    quote(WorldMarketItem::Wti,"89.29","1.17","1.33",WorldMarketSession::Open,10,"뉴욕상업거래소");
    quote(WorldMarketItem::UsdKrw,"1344.30","1.80","0.13",WorldMarketSession::Open,0,"하나은행");
    quote(WorldMarketItem::JpyKrw,"848.73","1.22","0.14",WorldMarketSession::Open,0,"하나은행");
    quote(WorldMarketItem::CnyKrw,"201.68","0.51","0.25",WorldMarketSession::Open,0,"하나은행");
    quote(WorldMarketItem::AudKrw,"941.68","1.12","0.12",WorldMarketSession::Open,0,"하나은행");
    s.dollarIndex={"101.25","101.87","-0.60",QDate(2026,11,4),QDate(2026,11,3),at};
    s.japanYield={"2.855","2.817",QDate(2026,11,4),QDate(2026,11,2),at};
    s.active=true;return s;
}
ExploreMetricCardData coreMetric(const QString& title,const QString& value,const QString& unit,const QString& note) {
    ExploreMetricCardData d;d.id="synthetic:"+title;d.title=title;d.value=value;d.unit=unit;d.changeText=note;return d;
}

// Record the text sent to the widget's public paint surface, rather than
// accepting an unclipped accessible label as evidence of visible digits.
class RowPaintEngine final : public QPaintEngine {
  public:
    RowPaintEngine() : QPaintEngine(AllFeatures) {}
    QList<QPair<QString, QFont>> texts;
    int lineSegments=0;
    bool begin(QPaintDevice*) override { return true; }
    bool end() override { return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawPath(const QPainterPath&) override {}
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawLines(const QLineF*, int count) override { lineSegments+=count; }
    void drawTextItem(const QPointF&, const QTextItem& item) override { texts.append({item.text(), item.font()}); }
};
class RowPaintDevice final : public QPaintDevice {
  public:
    explicit RowPaintDevice(QSize size) : size_(size) {}
    mutable RowPaintEngine engine;
    QPaintEngine* paintEngine() const override { return &engine; }
    int metric(PaintDeviceMetric value) const override {
        switch (value) {
        case PdmWidth: return size_.width(); case PdmHeight: return size_.height();
        case PdmDepth: return 32; case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return 96;
        }
    }
  private:
    QSize size_;
};

QString symbol(int index) { return QString("SYN%1").arg(index, 2, 10, QChar('0')); }
QList<WatchlistQuote> quotesAt(int revision, int count = 50) {
    QList<WatchlistQuote> quotes;
    for (int i = 0; i < count; ++i) {
        WatchlistQuote q;
        q.section = "coin"; q.id = symbol(i); q.name = QStringLiteral("합성 종목 %1").arg(i);
        q.priceValue = 10000 + i + revision * 1000;
        q.price = QString::number(10000 + i + revision * 1000) + " KRW";
        q.change = "2.50"; q.cap = QStringLiteral("12.3조원"); q.volume = QStringLiteral("456억원");
        q.history = {{0, 100}, {1, 103}, {2, 101}};
        q.evidence = "https://api.upbit.com/v1/ticker?markets=KRW-" + q.id;
        q.evidenceProvider = QStringLiteral("업비트");
        q.evidenceState = revision > 0 ? QStringLiteral("지연") : QString{};
        q.evidenceSourceTimeMs = SourceAt + revision * 60000;
        quotes.append(q);
    }
    return quotes;
}
int rowForId(const QTableWidget& table, const QString& id) {
    for (int row = 0; row < table.rowCount(); ++row)
        if (table.item(row, 1)->data(Qt::UserRole).toString() == id) return row;
    return -1;
}
QString selectedId(const QTableWidget& table) {
    const auto* item = table.item(table.currentRow(), 1);
    return item ? item->data(Qt::UserRole).toString() : QString{};
}
QStringList orderedIds(const QTableWidget& table) {
    QStringList result;
    for (int row = 0; row < table.rowCount(); ++row)
        result.append(table.item(row, 1)->data(Qt::UserRole).toString());
    return result;
}
// Observes the public model that the first real viewport Paint will consume.
// It never calls a private renderer or paints the widget itself.
class FirstPaint final : public QObject {
  public:
    explicit FirstPaint(QTableWidget& table) : table_(table) { table.viewport()->installEventFilter(this); }
    bool armed = false, seen = false;
    QString id, price, source, accessible;
    QList<int> references, historyReferences;
    int vertical = 0, horizontal = 0;
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (armed && !seen && watched == table_.viewport() && event->type() == QEvent::Paint) {
            seen = true; id = selectedId(table_);
            const auto row = table_.currentRow();
            if (row >= 0) {
                price = table_.item(row, 2)->text(); source = table_.item(row, 2)->toolTip();
                accessible = table_.item(row, 2)->data(Qt::AccessibleDescriptionRole).toString();
                references = table_.item(row, 2)->data(EvidenceNumbersRole).value<QList<int>>();
                historyReferences = table_.item(row, 4)->data(EvidenceNumbersRole).value<QList<int>>();
            }
            vertical = table_.verticalScrollBar()->value(); horizontal = table_.horizontalScrollBar()->value();
        }
        return QObject::eventFilter(watched, event);
    }
  private:
    QTableWidget& table_;
};
}

class ExploreLayoutTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
  private slots:
    void dev99CoinSummaryRemovesOnlyRetiredMetricCells();
    void dev99CoinSummaryUsesValue22AndRealHistory64();
    void dev99CoinSummaryKeepsHoldingBelowNameAndClearsImmediately();
    void dev99EmptyCardStripContainsOnlyEmptyCards_data();
    void dev99EmptyCardStripContainsOnlyEmptyCards();
    void dev99EmptyCardStripReappearsOnPrivateClear();
    void dev99CalendarDateAndTimeShareOneLine_data();
    void dev99CalendarDateAndTimeShareOneLine();
    void initTestCase(){
        QVERIFY(settingsRoot_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsRoot_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settingsRoot_.filePath("system"));
        QVERIFY(KeefiFonts::registerBundledFonts());QApplication::setFont(KeefiFonts::font(FontRole::Body15));
    }
    void commoditySourceUsesAShortStructuredDialogAndReturnsFocus() {
        ExploreTodayView view;
        view.resize(1024, 700);
        view.show();
        auto* info = view.findChild<InfoButton*>("todayGroupInfo_commodities");
        QVERIFY(info);
        QCOMPARE(info->accessibleName(), QStringLiteral("원자재 정보 보기"));
        QVERIFY(info->toolTip().contains(QStringLiteral("최근월 선물 가격")));
        QVERIFY(!info->toolTip().contains(QLatin1Char('\n')));
        QCOMPARE(info->size(), QSize(28, 28));
        info->setFocus();
        QTest::keyClick(info, Qt::Key_Space);
        QTRY_VERIFY(info->informationOpen());
        auto* dialog = info->findChild<QDialog*>("infoDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->width(), 420);
        QVERIFY(dialog->height() < 380);
        auto* table = dialog->findChild<QWidget*>("infoPublicSourceTable");
        QVERIFY(table);
        QStringList rows;
        for (const auto* label : table->findChildren<QLabel*>()) rows.append(label->text());
        for (const auto* key : {"기준", "출처", "지연", "단위", "주의"})
            QVERIFY(rows.contains(QString::fromUtf8(key)));
        QVERIFY(rows.contains(QStringLiteral("제공처 기준 10분 지연")));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(!info->informationOpen());
        QTRY_VERIFY(info->hasFocus());
    }

    void publicSourceTimesNameKstAndItsOffset(){
        const auto before=QDateTime::fromString("2026-11-06T14:59:00Z",Qt::ISODate).toMSecsSinceEpoch();
        const auto after=QDateTime::fromString("2026-11-06T15:01:00Z",Qt::ISODate).toMSecsSinceEpoch();
        QCOMPARE(usabilitySourceTime("업비트",before),QString("업비트 · 23:59 KST · UTC+9 기준"));
        QCOMPARE(usabilitySourceTime("업비트",after),QString("업비트 · 00:01 KST · UTC+9 기준"));
        QCOMPARE(usabilitySourceTime("업비트",0),QString("업비트"));
        QVERIFY(usabilitySourceTime({},after).isEmpty());
        QVERIFY(!usabilitySourceTime("업비트 https://example.invalid/public",after).contains("://"));
    }
    void todayModeSelectionHasItsOwnFillWhenMacFocusStaysOnMorning_data(){
        QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
    }
    void todayModeSelectionHasItsOwnFillWhenMacFocusStaysOnMorning(){
        QFETCH(bool,dark);auto* theme=KeefiTheme::instance();const auto before=theme->mode();
        const auto restore=qScopeGuard([&]{theme->setMode(before);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        ExploreTodayView view;view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.resize(1000,800);view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        for(auto* button:view.findChildren<QPushButton*>())if(button->objectName().startsWith("todayMode_"))button->setFocusPolicy(Qt::TabFocus);
        auto* morning=view.findChild<QPushButton*>("todayMode_morning");auto* korea=view.findChild<QPushButton*>("todayMode_korea");QVERIFY(morning&&korea);
        morning->setFocus(Qt::TabFocusReason);QTRY_VERIFY(morning->hasFocus());
        QTest::mouseClick(korea,Qt::LeftButton);QTest::mouseMove(&view,QPoint(view.width()-4,view.height()-4));QCoreApplication::processEvents();
        QVERIFY(morning->hasFocus());QVERIFY(!morning->isChecked());QVERIFY(korea->isChecked());
        const auto fill=[](QPushButton* button){const auto image=button->grab().toImage();const auto ratio=image.devicePixelRatio();
            return image.pixelColor(qRound(8*ratio),qRound(button->height()*ratio/2));};
        QCOMPARE(fill(korea),themeColors().text);QVERIFY(fill(morning)!=themeColors().text);
    }
    void todayContributionsNeverLeaveAnUnexplainedEmptyPanel_data(){
        QTest::addColumn<bool>("cash");
        QTest::newRow("cash-only")<<true;
        QTest::newRow("unclassified-only")<<false;
    }
    void todayContributionsNeverLeaveAnUnexplainedEmptyPanel(){
        QFETCH(bool,cash);
        const qint64 now=QDateTime(QDate(2026,11,6),QTime(10,0),QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();
        const auto snapshot=[&](qint64 at,const QString& quantity){
            AssetSnapshot s;s.id="synthetic:"+QString::number(at);s.accountScope="SYNTHETIC_SCOPE";
            s.provider="binance";s.environment="live";s.market="spot";s.timeMs=at;s.balanceComplete=true;
            s.connectionId="SYNTHETIC_CONNECTION";s.connectionBinding="SYNTHETIC_BINDING";
            s.evidence={"SYNTHETIC_SOURCE","https://example.invalid/synthetic",s.accountScope,at,at,true};
            AssetComponent c;c.id="synthetic-balance";c.asset="KRW";c.quantity=quantity;
            c.kind=cash?AssetComponentKind::Cash:AssetComponentKind::Holding;c.evidence=s.evidence;s.components={c};
            return s;
        };
        const auto opening=snapshot(now-60000,"100"),closing=snapshot(now,"125");
        const auto value=AssetValuation::value({closing},{},"KRW",now,DisplayFreshnessMs);
        const CheckpointSelection selection{{{"SYNTHETIC_CONNECTION","SYNTHETIC_BINDING"}},{}};
        auto history=portfolioDayHistory({opening,closing},{},selection,value,now);
        QVERIFY2(history.comparison.available,qPrintable(history.comparison.reason));
        history.contributions=portfolioContributions(history.baseline.valuation,history.baseline.policyVersion,
            history.baseline.usedSnapshots,value,ValuationPolicyVersion,{closing});
        QVERIFY2(history.contributions.available,qPrintable(history.contributions.reason));
        QCOMPARE(history.contributions.groups.size(),1);
        QCOMPARE(history.contributions.groups.first().assetClass,cash?PortfolioContributionClass::Cash:PortfolioContributionClass::Unclassified);
        QCOMPARE(history.contributions.groups.first().difference,QString("25"));
        ExploreTodayView view;view.resize(1000,800);view.show();
        auto* panel=view.findChild<QWidget*>("todayContributionPanel");QVERIFY(panel);
        auto* change=view.findChild<QLabel*>("todayPrivateChange");QVERIFY(change);
        view.setPrivateSummary(true,"125","KRW",history);
        QVERIFY(panel->isVisible());
        QVERIFY(!change->isVisible());
        const auto rows=panel->findChildren<QLabel*>();
        QVERIFY(std::any_of(rows.cbegin(),rows.cend(),
            [](QLabel* label){return label->text().contains("25 KRW");}));
        const auto blocked=portfolioDayHistory({},{},{},value,now);
        view.setPrivateSummary(true,"125","KRW",blocked);QVERIFY(panel->isHidden());
        view.setPrivateSummary(true,"125","KRW",{});QVERIFY(panel->isHidden());QVERIFY(change->text().isEmpty());
        view.clearPrivate();QVERIFY(panel->isHidden());
        for(auto* label:view.findChildren<QLabel*>("todayContributionValue")){QVERIFY(label->isHidden());QVERIFY(label->text().isEmpty());}
    }
    void todayOnlyShowsAHistoryLineWithARecordedSegment(){
        ExploreTodayView view;view.show();
        PortfolioDayHistory history;
        history.points={{1000,true,true,false,"100",{}},{2000,true,true,false,"120",{}}};
        auto* chart=view.findChild<QWidget*>("todayPrivateHistory");QVERIFY(chart);
        view.setPrivateSummary(true,"120","KRW",history);QVERIFY(chart->isHidden());
        history.points.last().connectToPrevious=true;
        view.setPrivateSummary(true,"120","KRW",history);QVERIFY(chart->isVisible());
        history.points.first().available=false;
        view.setPrivateSummary(true,"120","KRW",history);QVERIFY(chart->isHidden());
    }
    void todayYesterdayPercentRequiresARealPositiveBase_data(){
        QTest::addColumn<QString>("opening");QTest::addColumn<QString>("difference");QTest::addColumn<QString>("percent");
        QTest::newRow("gain")<<QString("80")<<QString("20")<<QString("+25.00%");
        QTest::newRow("loss")<<QString("80")<<QString("-10")<<QString("-12.50%");
        QTest::newRow("flat")<<QString("80")<<QString("0")<<QString("0.00%");
        QTest::newRow("over-double")<<QString("20")<<QString("40")<<QString("+200.00%");
        QTest::newRow("half-up")<<QString("32")<<QString("1")<<QString("+3.13%");
        QTest::newRow("tiny-loss")<<QString("100000")<<QString("-1")<<QString("0.01% 미만 하락");
        QTest::newRow("tiny-gain")<<QString("100000")<<QString("1")<<QString("0.01% 미만 상승");
        QTest::newRow("zero-opening")<<QString("0")<<QString("10")<<QString{};
        QTest::newRow("negative-opening")<<QString("-20")<<QString("10")<<QString{};
    }
    void todayYesterdayPercentRequiresARealPositiveBase(){
        QFETCH(QString,opening);QFETCH(QString,difference);QFETCH(QString,percent);
        ExploreTodayView view;view.show();
        const auto midnight=portfolioHistoryDayStart(QDateTime::currentMSecsSinceEpoch());
        PortfolioDayHistory history;auto& previous=history.previousDay;
        previous.comparison.available=previous.comparison.complete=true;
        previous.comparison.fromMs=midnight-60000;
        previous.comparison.openingValue=opening;previous.comparison.difference=difference;
        view.setPrivateSummary(true,"100","KRW",history);
        auto* change=view.findChild<QLabel*>("todayPrivateChange");QVERIFY(change);
        QCOMPARE(change->isVisible(),!percent.isEmpty());
        QCOMPARE(change->text(),percent.isEmpty()?QString{}:QString("어제 23:59 대비 ")+percent);
        history.comparison.available=true;
        history.contributions.available=true;history.contributions.groups={{PortfolioContributionClass::Cash,"100","101","1",1}};
        view.setPrivateSummary(true,"100","KRW",history);
        auto* contribution=view.findChild<QWidget*>("todayContributionPanel");QVERIFY(contribution&&contribution->isHidden());
        previous.available=true;previous.groups={{PortfolioContributionClass::Cash,"80","100","20",1}};
        view.setPrivateSummary(true,"100","KRW",history);QVERIFY(contribution->isVisible());
        auto* basis=view.findChild<QLabel*>("todayContributionBasis");QVERIFY(basis->text().contains("어제 23:59"));
        previous.groups.first().difference="0";
        view.setPrivateSummary(true,"100","KRW",history);QVERIFY(contribution->isHidden());
        view.clearPrivate();QVERIFY(change->text().isEmpty());QVERIFY(basis->toolTip().isEmpty());
    }
    void todayContributionReasonsFollowTheFullConnectionHistory_data(){
        QTest::addColumn<QString>("scenario");
        for(const auto* scenario:{"two-ready","missing-joint","changed-day-scope","closing-unpriced","same-partial"})
            QTest::newRow(scenario)<<QString(scenario);
    }
    void todayContributionReasonsFollowTheFullConnectionHistory(){
        QFETCH(QString,scenario);
        const auto now=QDateTime(QDate(2026,11,7),QTime(10,0),QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();
        const auto snapshot=[&](const QString& id,qint64 time,const QString& amount){
            AssetSnapshot s;s.id=id+QString::number(time);s.accountScope="SYNTHETIC_SCOPE_"+id;
            s.provider="upbit";s.environment="live";s.market="spot";s.timeMs=time;s.balanceComplete=true;
            s.connectionId=id;s.connectionBinding="SYNTHETIC_BINDING";
            s.evidence={"SYNTHETIC_SOURCE","https://example.invalid/synthetic",s.accountScope,time,time,true};
            AssetComponent c;c.id="holding:BTC";c.asset="BTC";c.quantity="1";c.kind=AssetComponentKind::Holding;
            c.reportedValue=amount;c.reportedValueCurrency="KRW";c.evidence=s.evidence;s.components={c};return s;
        };
        QList<AssetSnapshot> opening{snapshot("A",now-60000,"100"),snapshot("B",now-60000,"100")};
        QList<AssetSnapshot> closing{snapshot("A",now,"125"),snapshot("B",now,"125")};
        const auto unpriced=[](AssetSnapshot& s){auto c=s.components.first();c.id="holding:UNPRICED";c.asset="UNPRICED";
            c.reportedValue.clear();c.reportedValueCurrency.clear();s.components.append(c);};
        if(scenario=="missing-joint")opening.removeLast();
        if(scenario=="changed-day-scope")opening.prepend(snapshot("A",now-5*60000,"90"));
        if(scenario=="closing-unpriced"||scenario=="same-partial")unpriced(closing.last());
        if(scenario=="same-partial")unpriced(opening.last());
        const auto value=AssetValuation::value(closing,{},"KRW",now,DisplayFreshnessMs);
        const CheckpointSelection selection{{{"A","SYNTHETIC_BINDING"},{"B","SYNTHETIC_BINDING"}},{}};
        auto history=portfolioDayHistory(opening,{},selection,value,now);
        history.closingSnapshots=closing;
        if(history.comparison.available)history.contributions=portfolioContributions(history.baseline.valuation,
            history.baseline.policyVersion,history.baseline.usedSnapshots,value,ValuationPolicyVersion,closing);
        else history.contributions.reason=history.comparison.reason;
        if(scenario=="missing-joint")history.blockedConnectionNames={"SYN incomplete"};
        const bool comparable=scenario=="two-ready"||scenario=="same-partial";
        QCOMPARE(history.comparison.available,comparable);QCOMPARE(history.contributions.available,comparable);
        ExploreTodayView view;view.resize(1000,800);view.show();view.setPrivateSummary(true,value.knownSubtotal,"KRW",history);
        auto* panel=view.findChild<QWidget*>("todayContributionPanel");QVERIFY(panel);
        auto* basis=view.findChild<QLabel*>("todayContributionBasis");QVERIFY(basis);
        bool numeric=false;for(auto* label:view.findChildren<QLabel*>("todayContributionValue"))
            numeric|=label->isVisible()&&label->text().contains("코인")&&label->text().contains("50");
        QCOMPARE(numeric,comparable);QCOMPARE(panel->isVisible(),comparable);
        if(comparable){
            QVERIFY(basis->text().contains("오늘 09:59"));
            QCOMPARE(basis->text().contains("부분합"),scenario=="same-partial");
            QCOMPARE(history.comparison.difference,QString("50"));
        }else QVERIFY(basis->text().isEmpty());
        view.clearPrivate();QVERIFY(basis->text().isEmpty());QVERIFY(basis->toolTip().isEmpty());
        for(auto* label:view.findChildren<QLabel*>("todayContributionValue")){QVERIFY(label->isHidden());QVERIFY(label->text().isEmpty());}
    }
    void accessiblePublicSelectorsAndFavourites_data(){
        QTest::addColumn<QString>("control");for(const auto* name:{"coin","mode","rank-star","detail-star"})QTest::newRow(name)<<QString(name);
    }
    void accessiblePublicSelectorsAndFavourites(){
        QFETCH(QString,control);
        const auto toggle=[](QAbstractButton* button){QVERIFY(button);auto* ax=QAccessible::queryAccessibleInterface(button);QVERIFY(ax&&ax->actionInterface());ax->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());};
        if(control=="coin"){
            ExploreCoinSummary view;QSignalSpy selected(&view,&ExploreCoinSummary::coinSelected);view.setCoins({"BTC","ETH","SOL"},"BTC");QCOMPARE(selected.size(),0);view.show();
            QPushButton* eth=nullptr;for(auto* b:view.findChildren<QPushButton*>())if(b->property("asset")=="ETH")eth=b;toggle(eth);QCOMPARE(selected.size(),1);QCOMPARE(selected.first().first().toString(),QString("ETH"));
            view.setCoins({"BTC","ETH","SOL"},"SOL");QCOMPARE(selected.size(),1);
        }else if(control=="mode"){
            ExploreTodayView view;view.resize(1000,800);view.show();auto* kospi=view.findChild<QWidget*>("todayMetric_kospi");auto* spx=view.findChild<QWidget*>("todayMetric_spx");QVERIFY(kospi&&spx);QCoreApplication::processEvents();
            QVERIFY(spx->y()<kospi->y()); // 아침: last night's US close first.
            // Before any source answers, each row already reads as itself.
            QCOMPARE(static_cast<TodayLedgerRow*>(spx)->data().title,QString("S&P 500"));QCOMPARE(static_cast<TodayLedgerRow*>(kospi)->data().changeText,QStringLiteral("받는 중"));
            QCOMPARE(view.findChild<TodayLedgerRow*>("todayMetric_spread")->data().title,QStringLiteral("미·일 금리차"));
            toggle(view.findChild<QPushButton*>("todayMode_korea"));QCoreApplication::processEvents();QVERIFY(kospi->y()<spx->y()&&kospi->x()==spx->x());
            toggle(view.findChild<QPushButton*>("todayMode_night"));QCoreApplication::processEvents();
            auto* ftse=view.findChild<QWidget*>("todayMetric_ftse");QVERIFY(ftse);QVERIFY(ftse->y()<spx->y()&&spx->y()<kospi->y());
        }else if(control=="rank-star"){
            SpotlightCard view("coin",QStringLiteral("코인"));view.setModes({{"cap",QStringLiteral("시가총액"),{{"SYN","SYN",{},"100","1"}}}});QSignalSpy favourite(&view,&SpotlightCard::favouriteToggled);view.show();
            toggle(view.findChild<QToolButton*>("spotlightStar0"));QCOMPARE(favourite.size(),1);QCOMPARE(favourite.first().at(1).toString(),QString("SYN"));view.setFavourites({"SYN"});QCOMPARE(favourite.size(),1);
        }else{
            AssetDetailPanel view;view.enableExplorationControls();QSignalSpy favourite(&view,&AssetDetailPanel::favouriteRequested);view.setPublicSelection("coin","SYN",false);view.show();
            toggle(view.findChild<QPushButton*>("publicFavourite"));QCOMPARE(favourite.size(),1);QCOMPARE(favourite.first().at(1).toString(),QString("SYN"));view.setPublicSelection("coin","SYN",true);QCOMPARE(favourite.size(),1);
        }
    }
    void selectedRankStudyPreservesPricesPageAndRowActions(){
        SpotlightCard card("coin",QStringLiteral("코인"));SpotlightMode mode{"cap",QStringLiteral("시가총액"),{}};
        for(int i=0;i<25;++i)mode.items.append({QString::number(i),"SYN",{},"123,456,789", "2.00"});
        card.setModes({mode});card.setPage(1);QSignalSpy order(&card,&SpotlightCard::evidenceOrderChanged);
        card.setStudyValues({{"19",{QStringLiteral("3.00배"),QStringLiteral("합성 완료 1시간봉 · 중앙값 20개")}}});
        QCOMPARE(card.page(),1);QCOMPARE(order.size(),0);QCOMPARE(card.visibleItems().first().study,QStringLiteral("3.00배"));
        QCOMPARE(card.visibleItems().first().price,QStringLiteral("123,456,789"));
        card.resize(400,400);card.show();QTest::qWait(10);auto* rows=card.findChild<QWidget*>("spotlightRows");QVERIFY(rows);
        RowPaintDevice painted(rows->size());rows->render(&painted);bool price=false,state=false;
        for(const auto& t:painted.engine.texts){if(t.first=="123,456,789"){price=true;QCOMPARE(t.second.pixelSize(),22);}if(t.first==QStringLiteral("3.00배"))state=true;}
        QVERIFY(price);QVERIFY(state);
        card.resize(600,400);QTest::qWait(10);RowPaintDevice medium(rows->size());rows->render(&medium);state=false;for(const auto& t:medium.engine.texts)if(t.first==QStringLiteral("3.00배"))state=true;QVERIFY(state);
        card.setStudyValues({});QVERIFY(card.visibleItems().first().study.isEmpty());
    }
    void neverShownAcceptsQuotesAndInternalReferencesImmediately();
    void hiddenQuoteBurstDefersModelWorkAndFirstShowUsesLatestSource();
    void hiddenReferencesAreCachedAndFirstPaintUsesLatestNumbers();
    void visibleParentDefersNeverShownChild_data();
    void visibleParentDefersNeverShownChild();
    void watchlistShowsCompactSourceWithoutEvidenceReferences();
    void spotlightReferencesAreHiddenWithoutChangingPublicItems();
    void spotlightKoreanNameTooltipRemainsLiteral();
    void rankRowsDrawA24HourLineAndSessionStatusOnlyWhenProvided();
    void watchlistColumnsShareAvailableWidth_data();
    void watchlistColumnsShareAvailableWidth();
    void spotlightNextWrapsToFirstPage_data();
    void spotlightNextWrapsToFirstPage();
    void spotlightRowsFitTheScreenAndKeepTheFirstShownItem();
    void compactWatchlistKeepsNamePriceAndChange();
    void compactWatchlistFitsFourRowsInDashboardRail();
    void compactWatchlistPaintsNameAndCodeOnOneLine();
    void dev99WatchlistCompactRowsKeepFixedColumnsAndSingleLine_data();
    void dev99WatchlistCompactRowsKeepFixedColumnsAndSingleLine();
    void dev99WatchlistStaleDotReplacesNameSuffixAndKeepsLegend_data();
    void dev99WatchlistStaleDotReplacesNameSuffixAndKeepsLegend();
    void dev99WatchlistHeaderSortKeepsValuesAndSavedOrder();
    void dev99WatchlistCompactViewportFitsTenRows_data();
    void dev99WatchlistCompactViewportFitsTenRows();
    void narrowRankingPaintsTheWholePriceAt22Pixels();
    void todayGapLineOnlyDrawsMatchingConnectedRecords();
    void todayLedgersAlignAcrossRowsAndLists();
    void todayWorldRowsSayWhatTheyShow();
    void todayCrossRateTooltipKeepsRefreshFailure_data();
    void todayCrossRateTooltipKeepsRefreshFailure();
    void todayClosedRowsUseTheirOwnRefreshInterval_data();
    void todayClosedRowsUseTheirOwnRefreshInterval();
};

void ExploreLayoutTest::neverShownAcceptsQuotesAndInternalReferencesImmediately(){
    QTemporaryDir settings;QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());

    QSettings().setValue("explore/watchlist/coin", QStringList{symbol(0)});
    ExploreWatchlist view;
    QVERIFY(!view.isVisible());
    view.setQuotes(quotesAt(0, 1));
    view.setEvidenceNumbers({{symbol(0), {7}}}, {{symbol(0), {9}}});
    auto* table = view.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QSignalSpy changed(&view, &ExploreWatchlist::favouritesChanged);
    QSignalSpy section(&view, &ExploreWatchlist::sectionChanged);
    QSignalSpy rendered(&view, &ExploreWatchlist::evidenceOrderChanged);
    view.setQuotes(quotesAt(0, 1));
    QCOMPARE(rendered.size(), 0);
    view.setSection("us");
    view.setSection("coin");
    QCOMPARE(section.size(), 2);
    QCOMPARE(changed.size(), 0);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 2)->text(), QString("10000 KRW"));
    QCOMPARE(table->item(0, 2)->toolTip(), QStringLiteral("업비트 · 09:00 KST · UTC+9 기준"));
    QCOMPARE(table->item(0, 2)->data(EvidenceNumbersRole).value<QList<int>>(), QList<int>{7});
    QCOMPARE(table->item(0, 4)->data(EvidenceNumbersRole).value<QList<int>>(), QList<int>{9});
    QVERIFY(!table->isColumnHidden(4));
}

void ExploreLayoutTest::hiddenQuoteBurstDefersModelWorkAndFirstShowUsesLatestSource(){
    QTemporaryDir settings;QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());

    QStringList persisted;
    QHash<QString, QList<int>> quoteReferences, historyReferences;
    for (int i = 0; i < 50; ++i) {
        persisted.append(symbol(i)); quoteReferences.insert(symbol(i), {7}); historyReferences.insert(symbol(i), {9});
    }
    QSettings().setValue("explore/watchlist/coin", persisted);
    ExploreWatchlist view; view.setQuotes(quotesAt(0)); view.resize(720, 400); view.show();
    auto* table = view.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QTRY_VERIFY(table->viewport()->isVisible());
    QTest::qWait(30); // Complete initial header/layout delivery before the public header click.
    view.setEvidenceNumbers(quoteReferences, historyReferences);
    auto* header = table->horizontalHeader();
    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
        QPoint(header->sectionViewportPosition(2) + header->sectionSize(2) / 2, header->height() / 2));
    QTRY_COMPARE(header->sortIndicatorSection(), 2);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    QCOMPARE(table->item(0, 1)->data(Qt::UserRole).toString(), QString("SYN49"));
    const int selected = rowForId(*table, "SYN20"); QVERIFY(selected >= 0);
    table->setCurrentCell(selected, 1); table->scrollToItem(table->item(selected, 1), QAbstractItemView::PositionAtCenter);
    QTRY_VERIFY(table->verticalScrollBar()->value() > 0);
    if (table->horizontalScrollBar()->maximum() > 0)
        table->horizontalScrollBar()->setValue(qMin(25, table->horizontalScrollBar()->maximum()));
    QTest::qWait(30); // Settle the same responsive layout before comparing its preserved viewport.
    const auto order = orderedIds(*table);
    const int vertical = table->verticalScrollBar()->value(), horizontal = table->horizontalScrollBar()->value();
    QCOMPARE(selectedId(*table), QString("SYN20"));
    FirstPaint paint(*table);
    view.hide(); QCoreApplication::processEvents(); QVERIFY(!view.isVisible());
    QSignalSpy changes(table->model(), &QAbstractItemModel::dataChanged);
    QSignalSpy resets(table->model(), &QAbstractItemModel::modelReset);
    QSignalSpy inserted(table->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(table->model(), &QAbstractItemModel::rowsRemoved);
    QSignalSpy evidence(&view, &ExploreWatchlist::evidenceOrderChanged);
    QSignalSpy favourites(&view, &ExploreWatchlist::favouritesChanged);
    QVERIFY(changes.isValid()); QVERIFY(evidence.isValid());
    // Ten public cache updates exercise the 10Hz-shaped input burst without a wall-clock sleep.
    for (int revision = 1; revision <= 10; ++revision) view.setQuotes(quotesAt(revision));
    QCOMPARE(changes.count(), 0);
    QCOMPARE(resets.count(), 0); QCOMPARE(inserted.count(), 0); QCOMPARE(removed.count(), 0);
    QCOMPARE(evidence.count(), 0); QCOMPARE(favourites.count(), 0);
    QCOMPARE(view.ids("coin"), persisted);
    QCOMPARE(QSettings().value("explore/watchlist/coin").toStringList(), persisted);
    paint.armed = true; view.show(); QTRY_VERIFY(paint.seen);
    QCOMPARE(paint.id, QString("SYN20"));
    QCOMPARE(paint.price, QStringLiteral("20020 KRW · 지연"));
    QCOMPARE(paint.source, QStringLiteral("업비트 · 09:10 KST · UTC+9 기준"));
    QCOMPARE(paint.references, QList<int>{7}); QCOMPARE(paint.historyReferences, QList<int>{9});
    QCOMPARE(paint.vertical, vertical); QCOMPARE(paint.horizontal, horizontal);
    QCOMPARE(orderedIds(*table), order); QCOMPARE(selectedId(*table), QString("SYN20"));
    QCOMPARE(header->sortIndicatorSection(), 2); QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    QCOMPARE(evidence.count(), 1); QCOMPARE(favourites.count(), 0);
    QVERIFY(!table->isColumnHidden(4));
    QVERIFY(!paint.source.contains("https://")); QVERIFY(!paint.source.contains("[7]"));
}

void ExploreLayoutTest::hiddenReferencesAreCachedAndFirstPaintUsesLatestNumbers(){
    QTemporaryDir settings;QVERIFY(settings.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings().setValue("explore/watchlist/coin",QStringList{symbol(0)});
    ExploreWatchlist view;view.setQuotes(quotesAt(0,1));view.resize(720,400);view.show();
    auto* table=view.findChild<QTableWidget*>("watchlistTable");QVERIFY(table);QTest::qWait(30);
    table->setCurrentCell(0,1);view.setEvidenceNumbers({{symbol(0),{7}}},{{symbol(0),{9}}});
    FirstPaint paint(*table);view.hide();QCoreApplication::processEvents();
    QSignalSpy changes(table->model(),&QAbstractItemModel::dataChanged);
    QSignalSpy evidence(&view,&ExploreWatchlist::evidenceOrderChanged);
    view.setEvidenceNumbers({{symbol(0),{11}}},{{symbol(0),{13}}});
    QCOMPARE(changes.count(),0);QCOMPARE(evidence.count(),0);
    view.setQuotes(quotesAt(1,1));QCOMPARE(changes.count(),0);
    paint.armed=true;view.show();QTRY_VERIFY(paint.seen);
    QCOMPARE(paint.id,QString("SYN00"));QCOMPARE(paint.price,QStringLiteral("11000 KRW · 지연"));
    QCOMPARE(paint.source,QStringLiteral("업비트 · 09:01 KST · UTC+9 기준"));
    QCOMPARE(paint.references,QList<int>{11});QCOMPARE(paint.historyReferences,QList<int>{13});
    QCOMPARE(paint.accessible,paint.price);QCOMPARE(evidence.count(),1);
    QCOMPARE(view.ids("coin"),QStringList{symbol(0)});
}

void ExploreLayoutTest::visibleParentDefersNeverShownChild_data(){
    QTest::addColumn<bool>("referencesFirst");
    QTest::newRow("quote-first")<<false;
    QTest::newRow("references-first")<<true;
}
void ExploreLayoutTest::visibleParentDefersNeverShownChild(){
    QFETCH(bool,referencesFirst);
    QTemporaryDir settings;QVERIFY(settings.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings().setValue("explore/watchlist/coin",QStringList{symbol(0)});
    QWidget window;auto* layout=new QVBoxLayout(&window);QStackedWidget stack(&window);layout->addWidget(&stack);
    QWidget other;ExploreWatchlist view;stack.addWidget(&other);stack.addWidget(&view);stack.setCurrentWidget(&other);
    // Keep the established eager projection before the top-level window starts presentation.
    view.setQuotes(quotesAt(0,1));view.setEvidenceNumbers({{symbol(0),{7}}},{{symbol(0),{9}}});
    auto* table=view.findChild<QTableWidget*>("watchlistTable");QVERIFY(table);QCOMPARE(table->rowCount(),1);
    table->setCurrentCell(0,1);window.resize(800,500);window.show();QTest::qWait(30);
    QVERIFY(window.isVisible());QVERIFY(other.isVisible());QVERIFY(!view.isVisible());
    FirstPaint paint(*table);QSignalSpy changes(table->model(),&QAbstractItemModel::dataChanged);
    QSignalSpy evidence(&view,&ExploreWatchlist::evidenceOrderChanged);
    if(referencesFirst){view.setEvidenceNumbers({{symbol(0),{11}}},{{symbol(0),{13}}});view.setQuotes(quotesAt(1,1));}
    else{view.setQuotes(quotesAt(1,1));view.setEvidenceNumbers({{symbol(0),{11}}},{{symbol(0),{13}}});}
    QCOMPARE(changes.count(),0);QCOMPARE(evidence.count(),0);
    // Once parent presentation has started, hiding that parent must not restore eager updates.
    window.hide();QCoreApplication::processEvents();view.setQuotes(quotesAt(2,1));
    view.setEvidenceNumbers({{symbol(0),{17}}},{{symbol(0),{19}}});
    QCOMPARE(changes.count(),0);QCOMPARE(evidence.count(),0);
    window.show();QCoreApplication::processEvents();QVERIFY(!view.isVisible());
    paint.armed=true;stack.setCurrentWidget(&view);QTRY_VERIFY(paint.seen);
    QCOMPARE(paint.id,QString("SYN00"));QCOMPARE(paint.price,QStringLiteral("12000 KRW · 지연"));
    QCOMPARE(paint.source,QStringLiteral("업비트 · 09:02 KST · UTC+9 기준"));
    QCOMPARE(paint.references,QList<int>{17});QCOMPARE(paint.historyReferences,QList<int>{19});
    QCOMPARE(paint.accessible,paint.price);QCOMPARE(evidence.count(),1);
    QCOMPARE(view.ids("coin"),QStringList{symbol(0)});
}

void ExploreLayoutTest::rankRowsDrawA24HourLineAndSessionStatusOnlyWhenProvided(){
    for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
        KeefiTheme::instance()->setMode(mode);
        SpotlightCard card("us","미국 주식");
        SpotlightItem rising{"SYN","SYN","Vol 1","$10","2.00"},flat{"SYM","SYM","Vol 1","$10","2.00"}; // Same change; only the first has points.
        rising.spark={10,10.2,10.1,10.6,11};
        card.setModes({{"cap","시가총액",{rising,flat}}});card.resize(900,360);card.show();QVERIFY(QTest::qWaitForWindowExposed(&card));
        auto* status=card.findChild<QLabel*>("spotlightStatus");QVERIFY(status);QVERIFY(!status->isVisible());
        card.setStatus(QStringLiteral("장중"));QVERIFY(status->isVisible());QCOMPARE(status->text(),QStringLiteral("장중"));
        card.setStatus({});QVERIFY(!status->isVisible());
        auto* rows=card.findChild<QWidget*>("spotlightRows");QVERIFY(rows);
        const auto image=rows->grab().toImage();const auto up=MovementColorPolicy::colorFor(rows,MovementDirection::Up),line=themeColors().text2;
        // Only the row with points draws the line. Its points can lag the price by hours, so it is
        // neutral: it adds neutral pixels to its row and never the up/down colour of the change beside it.
        const auto count=[&](int rowIndex,const QColor& wanted){int found=0;for(int y=2+rowIndex*SpotlightCard::RowHeight;y<2+(rowIndex+1)*SpotlightCard::RowHeight;++y)for(int x=0;x<image.width();++x){const auto c=image.pixelColor(x,y);if(qAbs(c.red()-wanted.red())<24&&qAbs(c.green()-wanted.green())<24&&qAbs(c.blue()-wanted.blue())<24)++found;}return found;};
        const int neutralGain=count(0,line)-count(1,line),upGain=count(0,up)-count(1,up);
        QVERIFY2(neutralGain>60,qPrintable(QString::number(neutralGain)));
        QVERIFY2(qAbs(upGain)<=30,qPrintable(QString::number(upGain)));
    }
    KeefiTheme::instance()->setMode(ThemeMode::System);
}
void ExploreLayoutTest::spotlightReferencesAreHiddenWithoutChangingPublicItems(){
    SpotlightCard card("coin","CRYPTO");SpotlightItem item{"SYN","SYN","Vol 100 https://example.test/source coin:upbit:KRW-SYN","123 KRW","1"};
    card.setModes({{"gainers","상승",{item}}});card.setEvidenceNumbers({{"SYN",{7}}});card.setEvidenceStates({{"SYN",QStringLiteral("지연")}});card.setEvidenceSources({{"SYN",QStringLiteral("업비트 · 09:00 KST · UTC+9 기준")}});card.resize(500,450);card.show();
    QVERIFY2(!card.accessibleDescription().contains("[7]"),"Spotlight exposes prices and state without evidence reference numbers");
    QVERIFY(card.accessibleDescription().contains(QStringLiteral("지연")));
    auto* rows=card.findChild<QWidget*>("spotlightRows");QVERIFY(rows);QMouseEvent move(QEvent::MouseMove,QPointF(60,20),rows->mapToGlobal(QPoint(60,20)),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(rows,&move);
    QVERIFY(rows->toolTip().contains(QStringLiteral("업비트 · 09:00 KST · UTC+9 기준")));QVERIFY(!rows->toolTip().contains("https://"));QVERIFY(!rows->toolTip().contains("coin:"));
    QCOMPARE(card.visibleItems().size(),1);QCOMPARE(card.visibleItems().first().id,item.id);QCOMPARE(card.visibleItems().first().price,item.price);QCOMPARE(card.visibleItems().first().sub,item.sub);
}

void ExploreLayoutTest::watchlistShowsCompactSourceWithoutEvidenceReferences() {
    QTemporaryDir settings;QVERIFY(settings.isValid());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings().setValue("explore/watchlist/coin",QStringList{"SYN"});
    WatchlistQuote quote;quote.section="coin";quote.id="SYN";quote.name="합성 종목";quote.price="123 KRW";quote.priceValue=123;quote.change="1";
    quote.evidence="https://api.upbit.com/v1/ticker?binding=SYNTHETIC coin:upbit:KRW-SYN [7]";
    quote.evidenceProvider="업비트";quote.evidenceState="지연";quote.evidenceSourceTimeMs=1792713600000;
    ExploreWatchlist watch;watch.setQuotes({quote});watch.setEvidenceNumbers({{"SYN",{7}}});watch.resize(1000,400);watch.show();
    auto* table=watch.findChild<QTableWidget*>("watchlistTable");QVERIFY(table);QCOMPARE(table->rowCount(),1);
    QCOMPARE(table->item(0,2)->toolTip(),QStringLiteral("업비트 · 09:00 KST · UTC+9 기준"));
    QVERIFY(table->item(0,2)->text().contains(QStringLiteral("지연")));
    QCOMPARE(table->item(0,2)->data(EvidenceNumbersRole).value<QList<int>>(),QList<int>{7});
    for(int col=0;col<table->columnCount();++col){const auto* cell=table->item(0,col);QVERIFY(cell);
        for(const auto& text:{cell->text(),cell->toolTip(),cell->data(Qt::AccessibleDescriptionRole).toString()}){
            QVERIFY(!text.contains("[7]"));QVERIFY(!text.contains("https://"));QVERIFY(!text.contains("coin:"));QVERIFY(!text.contains("SYNTHETIC"));
        }
    }
    QCOMPARE(watch.ids("coin"),QStringList{"SYN"});QCOMPARE(QSettings().value("explore/watchlist/coin").toStringList(),QStringList{"SYN"});
}

void ExploreLayoutTest::watchlistColumnsShareAvailableWidth_data() {
    QTest::addColumn<QString>("market");
    for (const auto* key : {"coin", "kr", "us"}) QTest::newRow(key) << QString(key);
}

void ExploreLayoutTest::watchlistColumnsShareAvailableWidth() {
    QFETCH(QString, market);
    QTemporaryDir settings;
    QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QList<WatchlistQuote> quotes;
    QHash<QString, QStringList> stored;
    for (const auto* key : {"coin", "kr", "us"}) {
        for (int i = 0; i < 20; ++i) {
            WatchlistQuote quote;
            quote.section = key;
            quote.id = QString("FIXTURE%1").arg(i);
            quote.name = key == QString("kr") ? QStringLiteral("검증용 긴 종목 이름") : QStringLiteral("Fixture Company");
            quote.price = key == QString("us") ? "$123.45" : QStringLiteral("123,450원");
            quote.priceValue = 123450 + i;
            quote.change = "2.50";
            quote.cap = QStringLiteral("12.3조원");
            quote.volume = QStringLiteral("456억원");
            quote.history = {{0, 100}, {1, 103}, {2, 101}, {3, 104}};
            quotes.append(quote);
            stored[key].append(quote.id);
        }
        QSettings().setValue("explore/watchlist/" + QString(key), stored[key]);
    }
    ExploreWatchlist watch;
    watch.setQuotes(quotes);
    watch.setSection(market);
    watch.resize(1440, 400);
    watch.show();
    auto* table = watch.findChild<QTableWidget*>("watchlistTable");
    QVERIFY(table);
    QTRY_VERIFY(table->verticalScrollBar()->maximum() > 0);
    QVERIFY(!table->horizontalHeader()->stretchLastSection());

    // More width must benefit all data columns, with the name and trend getting
    // more room than volume. Last-column-only stretching fails these checks.
    for (const int width : {1440, 1000}) {
        watch.resize(width, 400);
        QTRY_COMPARE(table->horizontalHeader()->length(), table->viewport()->width());
        QTRY_COMPARE(table->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(table->columnWidth(0), 34);
        QVERIFY(table->columnWidth(1) > table->columnWidth(6));
        QVERIFY(table->columnWidth(4) > table->columnWidth(6));
        QVERIFY(table->columnWidth(6) < table->viewport()->width() / 5);
        QVERIFY(table->columnWidth(2) > 125);
        QVERIFY(table->columnWidth(3) > 88);
        QVERIFY(table->columnWidth(5) > 110);
    }

    // A narrow table retains readable columns; the last column remains
    // reachable by horizontal scrolling instead of squeezing financial values.
    watch.resize(480, 400);
    QTRY_VERIFY(table->horizontalScrollBar()->maximum() > 0);
    QVERIFY(table->columnWidth(1) >= 190);
    QVERIFY(table->columnWidth(2) >= 125);
    QVERIFY(table->columnWidth(4) >= 140);
    QVERIFY(table->columnWidth(6) >= 115);
    table->horizontalScrollBar()->setValue(table->horizontalScrollBar()->maximum());
    QTRY_VERIFY(table->visualItemRect(table->item(0, 6)).right() <= table->viewport()->width());

    watch.resize(1600, 400);
    QTRY_COMPARE(table->horizontalHeader()->length(), table->viewport()->width());
    QTRY_COMPARE(table->horizontalScrollBar()->maximum(), 0);
    const int volumeWidth = table->columnWidth(6);
    // Quote refresh, header sorting and changing market keep the allocation and
    // the independent persisted lists intact.
    watch.setQuotes(quotes);
    QCOMPARE(table->columnWidth(6), volumeWidth);
    auto* header = table->horizontalHeader();
    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(header->sectionViewportPosition(2) + header->sectionSize(2) / 2, header->height() / 2));
    QCOMPARE(table->columnWidth(6), volumeWidth);
    for (const auto* key : {"coin", "kr", "us"}) {
        watch.setSection(key);
        QTRY_COMPARE(table->horizontalHeader()->length(), table->viewport()->width());
        QCOMPARE(watch.ids(key), stored[key]);
        QCOMPARE(QSettings().value("explore/watchlist/" + QString(key)).toStringList(), stored[key]);
        QCOMPARE(table->columnWidth(6), volumeWidth);
    }
}

void ExploreLayoutTest::spotlightNextWrapsToFirstPage_data() {
    QTest::addColumn<QString>("market");
    for (const auto* key : {"coin", "kr", "us"}) QTest::newRow(key) << QString(key);
}

void ExploreLayoutTest::spotlightNextWrapsToFirstPage() {
    QFETCH(QString, market);
    SpotlightCard card(market, market.toUpper());
    SpotlightMode gainers{"gainers", QStringLiteral("상승률 ↓"), {}};
    SpotlightMode losers{"losers", QStringLiteral("하락률 ↓"), {}};
    for(int i=1;i<=43;++i)gainers.items.append({QString::number(i),QString::number(i)});
    losers.items=gainers.items.mid(0,20);
    card.setModes({gainers,losers});
    auto* next=card.findChild<QPushButton*>("spotlightNext");
    auto* previous=card.findChild<QPushButton*>("spotlightPrevious");
    QCOMPARE(card.pageCount(),2);QVERIFY(!previous->isEnabled());
    QCOMPARE(card.visibleItems().size(),19);QCOMPARE(card.visibleItems().last().id,"19");
    next->click();QCOMPARE(card.visibleItems().size(),20);
    QCOMPARE(card.visibleItems().first().id,"20");QCOMPARE(card.visibleItems().last().id,"39");
    card.setModes({gainers,losers});QCOMPARE(card.page(),1);
    QCOMPARE(next->accessibleName(),QStringLiteral("첫 페이지로"));next->click();QCOMPARE(card.page(),0);
    card.setMode("losers");QCOMPARE(card.pageCount(),1);QCOMPARE(card.visibleItems().last().id,"20");QVERIFY(!next->isEnabled());
    losers.items.clear();card.setModes({losers});QVERIFY(card.visibleItems().isEmpty());QVERIFY(!previous->isEnabled());
}

void ExploreLayoutTest::spotlightRowsFitTheScreenAndKeepTheFirstShownItem() {
    SpotlightCard card("coin", "COIN");
    SpotlightMode gainers{"gainers", QStringLiteral("상승률 ↓"), {}};
    for (int i = 0; i < 23; ++i) gainers.items.append({QString("F%1").arg(i), QString("F%1").arg(i), "Vol 100", "1", "1"});
    card.setModes({gainers});
    auto* rows = card.findChild<QWidget*>("spotlightRows"); QVERIFY(rows);
    const int chrome = card.chromeHeight(); QVERIFY(chrome > 0);
    QCOMPARE(card.sizeHint().height(), chrome + SpotlightCard::PageSize * SpotlightCard::RowHeight);
    card.setPage(1);QCOMPARE(card.visibleItems().first().id,QString("F19"));
    card.setRowsPerPage(10);QCOMPARE(card.rowsPerPage(),10);QCOMPARE(card.pageCount(),2);
    QCOMPARE(card.visibleItems().first().id,QString("F19"));QCOMPARE(card.visibleItems().size(),4);
    QCOMPARE(rows->minimumHeight(),10*SpotlightCard::RowHeight+4);
    QCOMPARE(card.sizeHint().height(),chrome+10*SpotlightCard::RowHeight);
    card.setRowsPerPage(3);QCOMPARE(card.page(),1);QCOMPARE(card.visibleItems().first().id,QString("F19"));
    card.setRowsPerPage(40);QCOMPARE(card.rowsPerPage(),SpotlightCard::LastPageRows);
    card.setRowsPerPage(0);QCOMPARE(card.rowsPerPage(),1);
}

void ExploreLayoutTest::compactWatchlistKeepsNamePriceAndChange() {
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings().setValue("explore/watchlist/coin", QStringList{symbol(0)});
    ExploreWatchlist watch; watch.setQuotes(quotesAt(0, 1)); watch.resize(420, 400); watch.show();
    auto* table = watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QTRY_VERIFY(!table->isColumnHidden(4)); QVERIFY(!table->isColumnHidden(5)); QVERIFY(!table->isColumnHidden(6));
    // The ranking's right column folds trend, market cap and volume; the rest share its width.
    watch.setCompact(true);
    for (int column : {4, 5, 6}) QVERIFY(table->isColumnHidden(column));
    for (int column : {0, 1, 2, 3}) QVERIFY(!table->isColumnHidden(column));
    QTRY_COMPARE(table->horizontalScrollBar()->maximum(), 0);
    QVERIFY(table->viewport()->rect().contains(table->visualItemRect(table->item(0, 3))));
    QCOMPARE(table->item(0, 2)->text(), QString("10000 KRW"));
    watch.setCompact(false); QVERIFY(!table->isColumnHidden(4)); QVERIFY(!table->isColumnHidden(6));
}


void ExploreLayoutTest::compactWatchlistFitsFourRowsInDashboardRail() {
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    const QStringList saved{symbol(0), symbol(1), symbol(2), symbol(3)};
    QSettings().setValue("explore/watchlist/coin", saved);
    ExploreWatchlist watch; watch.setCompact(true); watch.setQuotes(quotesAt(0, 4));
    watch.resize(400, 288); watch.show();
    auto* table=watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QVERIFY(QTest::qWaitForWindowExposed(&watch));
    QTRY_COMPARE(table->horizontalScrollBar()->maximum(), 0);
    QTRY_COMPARE(table->verticalScrollBar()->maximum(), 0);
    QVERIFY2(table->viewport()->rect().contains(table->visualItemRect(table->item(3, 3))),
             "The dashboard rail must show all four favourites without clipping or scrolling.");
    QSignalSpy selected(&watch, &ExploreWatchlist::itemSelected);
    QSignalSpy changed(&watch, &ExploreWatchlist::favouritesChanged);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->item(0, 0)).center());
    QCOMPARE(selected.count(), 0); QCOMPARE(changed.count(), 1);
    QCOMPARE(watch.ids("coin"), saved.mid(1));
    ExploreWatchlist reopened; QCOMPARE(reopened.ids("coin"), saved.mid(1));
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->item(0, 1)).center());
    QCOMPARE(selected.count(), 1); QCOMPARE(selected.first().at(1).toString(), symbol(1));
}


void ExploreLayoutTest::compactWatchlistPaintsNameAndCodeOnOneLine() {
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings().setValue("explore/watchlist/coin", QStringList{symbol(0)});
    ExploreWatchlist watch; watch.setCompact(true); watch.setQuotes(quotesAt(0, 1));
    watch.resize(400, 288); watch.show();
    QVERIFY(QTest::qWaitForWindowExposed(&watch));
    auto* table=watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    RowPaintDevice device(table->viewport()->size()); QPainter painter(&device);
    table->viewport()->render(&painter); painter.end();
    bool codeAt14=false; QString namesAt12;
    for(const auto& run:device.engine.texts){
        if(run.first==symbol(0)&&run.second.pixelSize()==14&&run.second.weight()>=QFont::DemiBold)codeAt14=true;
        if(run.second.pixelSize()==12)namesAt12+=run.first;
    }
    QVERIFY2(codeAt14,"The code must be painted in bold 14px beside the Caption12 name.");
    QVERIFY(namesAt12.contains(QStringLiteral("합성 종목 0")));
}

void ExploreLayoutTest::narrowRankingPaintsTheWholePriceAt22Pixels() {
    SpotlightCard card("coin", QStringLiteral("코인"));
    SpotlightItem quote{"SYN", "SYN", {}, "123,456,789", "12.34"};
    quote.display=QStringLiteral("합성 긴 이름의 비트코인");
    card.setModes({{"cap", QStringLiteral("시가총액"), {quote}}});
    card.setRowsPerPage(3); card.resize(400, 220); card.show();
    QVERIFY(QTest::qWaitForWindowExposed(&card));
    auto* rows=card.findChild<QWidget*>("spotlightRows"); QVERIFY(rows);
    RowPaintDevice device(rows->size()); QPainter painter(&device); rows->render(&painter); painter.end();
    QString emitted; bool fullAt22=false;
    for (const auto& run:device.engine.texts) {
        emitted+=run.first;
        if (run.first==quote.price && run.second.pixelSize()==22) fullAt22=true;
    }
    QVERIFY2(fullAt22, qPrintable("Expected the complete 22px price; painted: " + emitted));
}

void ExploreLayoutTest::todayGapLineOnlyDrawsMatchingConnectedRecords() {
    ExploreTodayView today; today.resize(976,736); today.show();
    QVERIFY(QTest::qWaitForWindowExposed(&today));
    auto* strip=today.findChild<QWidget*>("todayGapHistoryStrip"); QVERIFY(strip);
    auto* line=today.findChild<QWidget*>("todayGapHistory"); QVERIFY(line);
    QVERIFY(strip->isHidden());
    ExploreMetricCardData data;data.id="synthetic-gap";data.title="김프";data.value="1.2";data.unit="%";
    data.sourceKey="public:upbit-binance-gap:BTC";data.history.available=true;
    data.history.sourceKey=data.sourceKey;data.history.unit="%";
    data.history.points={{SourceAt,-0.5,false},{SourceAt+3600000,0.7,true},{SourceAt+10800000,1.2,false}};
    const int panelHeight=today.findChild<QWidget*>("todayMetricPanel")->height();
    auto* first=today.findChild<QWidget*>("todayMetric_gap");QVERIFY(first);
    today.setHeadline("김프 +1%");QCoreApplication::processEvents();const auto pos=first->mapTo(&today,QPoint());
    today.setHeadline(QString("응답이 늦어요 · 시세 2/3개 받음 ").repeated(9));QCoreApplication::processEvents();
    QCOMPARE(first->mapTo(&today,QPoint()),pos);
    today.setMetric("gap",data);QTRY_VERIFY(strip->isVisible());QCOMPARE(line->height(),22);
    QCoreApplication::processEvents();QCOMPARE(first->mapTo(&today,QPoint()),pos);
    RowPaintDevice device(line->size());QPainter painter(&device);line->render(&painter);painter.end();
    QCOMPARE(device.engine.lineSegments,1); // The missing interval must never be joined.
    QCOMPARE(today.findChild<QWidget*>("todayMetricPanel")->height(),panelHeight);
    auto wrongSource=data;wrongSource.history.sourceKey="public:upbit-binance-gap:ETH";
    today.setMetric("gap",wrongSource);QVERIFY(strip->isHidden());
    auto disconnected=data;for(auto& p:disconnected.history.points)p.connectsPrevious=false;
    today.setMetric("gap",disconnected);QVERIFY(strip->isHidden());
    today.setMetric("gap",data);QVERIFY(strip->isVisible());
    data.history={};today.setMetric("gap",data);QVERIFY(strip->isHidden());
    // The breadth cards left Today (they never had a dated sample); the card presentation still draws counts.
    for(const auto* removed:{"btc","funding","coinbase","usdt","kr","us"})QVERIFY2(!today.findChild<QWidget*>(QString("todayMetric_")+removed),removed);
    ExploreMetricCardData breadth;breadth.id="today:kr";breadth.title="코스피 상위 50";breadth.value="▲8 ▼2";breadth.breadth=BreadthCounts{8,2,1};breadth.changeText="━ 보합 1";
    ExploreMetricCard standalone;standalone.resize(320,80);standalone.show();auto* cell=&standalone;cell->setData(breadth);
    auto* value=cell->findChild<QLabel*>("metricValue");QVERIFY(value);QCOMPARE(value->accessibleName(),"상승 8종목, 하락 2종목, 보합 1종목");
    QVERIFY(value->text().contains(MovementColorPolicy::colorFor(cell,MovementDirection::Up).name()));
    QVERIFY(value->text().contains(MovementColorPolicy::colorFor(cell,MovementDirection::Down).name()));
    breadth.breadth.reset();breadth.value="—";breadth.changeText.clear();cell->setData(breadth);
    QCOMPARE(value->text(),QStringLiteral("—"));QVERIFY(value->accessibleName().isEmpty());

}

// User reports (Dev 87, Dev 88): numbers sat at different heights, and half the cards
// never had data. Today is now four ledgers whose rows share one structure: every
// value and note sits on the same right edge, and the first rows of all lists align.
void ExploreLayoutTest::todayLedgersAlignAcrossRowsAndLists() {
    ExploreTodayView today; today.resize(942,736); today.show();
    QVERIFY(QTest::qWaitForWindowExposed(&today));
    today.setMetric("gap",coreMetric("BTC 김프","+0.02","%","하나 07:52"));
    today.setMetric("tether",coreMetric("테더 김프","+0.16","%","하나 22:21"));
    today.setMetric("treasury",coreMetric("미국 10년물","4.90","%","11/04"));
    today.setMetric("spread",coreMetric("미·일 금리차","2.045","%p","11/04·11/05"));
    for(const auto& [key,data]:todayWorldMarketMetrics(syntheticWorld(SourceAt),SourceAt))today.setMetric(key,data);
    QCoreApplication::processEvents();
    auto* panel=today.findChild<QWidget*>("todayMetricPanel");QVERIFY(panel);
    // The 1440px window gives Today 942px, which had 1px to spare: the board may not grow.
    QVERIFY2(panel->height()<=318,qPrintable(QString::number(panel->height())));
    const QStringList ids{"rates","indices","fx","commodities"};
    QList<QWidget*> boxes;
    for(const auto& id:ids){auto* box=today.findChild<QWidget*>("todayGroup_"+id);QVERIFY2(box,qPrintable(id));boxes<<box;}
    const auto checkLists=[&](const QString& stage){
        for(auto* box:boxes){
            QVERIFY2(panel->rect().contains(QRect(box->mapTo(panel,QPoint()),box->size())),qPrintable(stage+" "+box->objectName()));
            auto rows=box->findChildren<TodayLedgerRow*>();QVERIFY(!rows.isEmpty());
            std::sort(rows.begin(),rows.end(),[](auto* a,auto* b){return a->y()<b->y();});
            QSet<int> valueRights,trailLefts;int previous=-1;
            for(auto* row:rows){
                if(previous>=0)QCOMPARE(row->y()-previous,TodayLedgerRow::RowHeight);
                previous=row->y();
                QVERIFY2(box->rect().contains(row->geometry()),qPrintable(row->objectName()));
                auto* title=row->findChild<QLabel*>("metricTitle");auto* value=row->findChild<QLabel*>("metricValue");auto* trail=row->findChild<QLabel*>("metricChange");
                QVERIFY(title&&value&&trail);
                QVERIFY2(title->fontMetrics().horizontalAdvance(title->text())<=title->width(),qPrintable(QString("%1 title clipped: %2 needs %3 of %4 (row %5, state %6, value %7, note %8)").arg(stage,row->objectName())
                    .arg(title->fontMetrics().horizontalAdvance(title->text())).arg(title->width()).arg(row->width()).arg(row->findChild<QLabel*>("metricState")->width()).arg(value->width()).arg(trail->width())));
                QVERIFY2(value->fontMetrics().horizontalAdvance(value->text())<=value->width(),qPrintable(stage+" value clipped: "+row->objectName()));
                QVERIFY2(trail->sizeHint().width()<=trail->width(),qPrintable(stage+" note clipped: "+row->objectName()));
                valueRights<<value->geometry().right();trailLefts<<trail->geometry().left();
            }
            QVERIFY2(valueRights.size()==1,qPrintable(stage+" values share a right edge in "+box->objectName()));
            QCOMPARE(trailLefts.size(),1);
        }
    };
    checkLists("wide");
    QSet<int> tops;for(auto* box:boxes)tops<<box->mapTo(&today,QPoint()).y();
    QCOMPARE(tops.size(),1);
    for(int i=1;i<boxes.size();++i)QVERIFY(boxes[i]->x()>boxes[i-1]->x()+boxes[i-1]->width());
    QSet<int> firstRows;
    for(auto* box:boxes){int top=INT_MAX;for(auto* row:box->findChildren<TodayLedgerRow*>())top=qMin(top,row->mapTo(&today,QPoint()).y());firstRows<<top;}
    QCOMPARE(firstRows.size(),1);
    const auto out=qEnvironmentVariable("KEEFI_TODAY_RENDER_DIR");
    const auto capture=[&](const QString& name){
        if(out.isEmpty())return;
        QVERIFY(QDir().mkpath(out));
        for(const bool dark:{false,true}){
            KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);today.setStyleSheet(KeefiTheme::localStyleSheet(&today));QCoreApplication::processEvents();
            QVERIFY(panel->grab().save(QDir(out).filePath(name+(dark?"-dark.png":"-light.png"))));
        }
    };
    capture("today-ledgers-942");
    // Narrow pages put two lists on a line and grow downwards instead of squeezing.
    today.resize(700,900);QCoreApplication::processEvents();
    QCOMPARE(boxes[1]->mapTo(&today,QPoint()).y(),boxes[0]->mapTo(&today,QPoint()).y());
    QStringList needs;for(auto* box:boxes)needs<<QString("%1:%2@%3,%4w%5").arg(box->objectName()).arg(box->property("ledgerNeed").toInt()).arg(box->x()).arg(box->y()).arg(box->width());
    QVERIFY2(boxes[2]->mapTo(&today,QPoint()).y()>boxes[0]->mapTo(&today,QPoint()).y()+boxes[0]->height(),qPrintable(QString("today %1 · ").arg(today.width())+needs.join(' ')));
    checkLists("narrow");
    capture("today-ledgers-700");
}

void ExploreLayoutTest::todayWorldRowsSayWhatTheyShow() {
    auto world=syntheticWorld(SourceAt);
    QHash<QString,ExploreMetricCardData> rows;
    for(const auto& [key,data]:todayWorldMarketMetrics(world,SourceAt))rows.insert(key,data);
    QCOMPARE(rows.size(),16);
    const auto spx=rows.value("spx");
    QCOMPARE(spx.title,QString("S&P 500"));QCOMPARE(spx.value,QString("7,688.62"));QCOMPARE(spx.changeText,QString("+1.58%"));
    QCOMPARE(spx.direction,MovementDirection::Up);QCOMPARE(spx.freshnessText,QStringLiteral("마감"));
    QVERIFY(spx.publicInformation.contains(QStringLiteral("장 마감")));QVERIFY(spx.publicInformation.contains(QStringLiteral("+119.70")));
    QCOMPARE(spx.evidence.size(),1);QCOMPARE(spx.evidence.first().sourceTimeMs,SourceAt-60000);
    QCOMPARE(rows.value("kospi").changeText,QString("-5.28%"));QCOMPARE(rows.value("kospi").direction,MovementDirection::Down);
    QVERIFY(rows.value("nikkei").publicInformation.contains(QStringLiteral("15분 지연")));
    QVERIFY(rows.value("ftse").freshnessText.isEmpty());
    QCOMPARE(rows.value("usdkrw").value,QString("1,344.30"));QCOMPARE(rows.value("jpykrw").title,QStringLiteral("원/100엔"));
    const auto usdJpy=WorldMarkets::cross(world.quote(WorldMarketItem::UsdKrw),1,world.quote(WorldMarketItem::JpyKrw),100);
    QCOMPARE(rows.value("usdjpy").value,usdJpy.price);QCOMPARE(rows.value("usdjpy").title,QStringLiteral("달러/엔"));
    QCOMPARE(rows.value("audjpy").title,QStringLiteral("호주달러/엔"));QVERIFY(rows.value("audjpy").value!=QStringLiteral("—"));
    QCOMPARE(rows.value("dxy").value,QString("101.25"));QCOMPARE(rows.value("dxy").changeText,QString("-0.60%"));
    QCOMPARE(rows.value("dxy").direction,MovementDirection::Down);
    QCOMPARE(rows.value("jgb").value,QString("2.855"));QCOMPARE(rows.value("jgb").unit,QString("%"));QCOMPARE(rows.value("jgb").changeText,QString("11/04"));
    QCOMPARE(rows.value("jgb").direction,MovementDirection::Unknown);
    QCOMPARE(rows.value("gold").value,QString("4,137.00"));QCOMPARE(rows.value("gold").unit,QString("USD/oz"));
    // A closed venue draws its dot; the marker and colour come from the movement policy.
    TodayLedgerRow row;row.setTrailWidth(80);row.resize(260,TodayLedgerRow::RowHeight);row.show();row.setData(rows.value("kospi"));
    QCOMPARE(row.findChild<QLabel*>("metricState")->text(),QStringLiteral("●"));QCOMPARE(row.findChild<QLabel*>("metricValue")->text(),QString("5,383.80"));
    QVERIFY(row.findChild<QLabel*>("metricChange")->text().contains(QStringLiteral("-5.28%")));
    QVERIFY(!row.findChild<QLabel*>("metricChange")->text().contains(QStringLiteral("▼"))); // Sign and colour, no marker column.
    QVERIFY(row.findChild<QLabel*>("metricChange")->styleSheet().contains(MovementColorPolicy::colorFor(&row,MovementDirection::Down).name()));
    QVERIFY(row.accessibleDescription().contains(QStringLiteral("하락")));QVERIFY(row.accessibleDescription().contains(QStringLiteral("장 마감")));
    QVERIFY(row.toolTip().contains(QStringLiteral("네이버 증권")));
    // Pre-market: the last close stays, but the provider's zero change is not shown as a flat day.
    world.quotes[int(WorldMarketItem::Spx)].session=WorldMarketSession::PreOpen;
    world.quotes[int(WorldMarketItem::Spx)].change="0.00";world.quotes[int(WorldMarketItem::Spx)].changePercent="0.00";
    // Late: this app has not re-read 원/달러 for three refresh periods.
    world.quotes[int(WorldMarketItem::UsdKrw)].receivedAtMs=SourceAt-4*WorldMarkets::RefreshMs;
    // Missing values are never guessed.
    world.quotes[int(WorldMarketItem::Gold)]={};world.failures[int(WorldMarketItem::Gold)]=QStringLiteral("금 응답 형식 확인 필요");
    world.quotes[int(WorldMarketItem::Silver)]={};
    world.japanYield={};world.japanYieldFailure=QStringLiteral("일본 10년물 이번 달 첫 공시 전");
    world.dollarIndex={};
    rows.clear();
    for(const auto& [key,data]:todayWorldMarketMetrics(world,SourceAt))rows.insert(key,data);
    QCOMPARE(rows.value("spx").value,QString("7,688.62"));QCOMPARE(rows.value("spx").changeText,QStringLiteral("장 전"));
    QCOMPARE(rows.value("spx").direction,MovementDirection::Unknown);
    QCOMPARE(rows.value("usdkrw").freshnessText,QStringLiteral("지연"));QCOMPARE(rows.value("usdjpy").freshnessText,QStringLiteral("지연"));
    QCOMPARE(rows.value("gold").value,QStringLiteral("—"));QCOMPARE(rows.value("gold").changeText,QStringLiteral("확인 필요"));
    QVERIFY(rows.value("gold").publicInformation.contains(QStringLiteral("금 응답 형식")));
    QCOMPARE(rows.value("silver").changeText,QStringLiteral("받는 중"));
    QCOMPARE(rows.value("jgb").changeText,QStringLiteral("공시 전"));QCOMPARE(rows.value("dxy").value,QStringLiteral("—"));
    // A late move is not shown as current; a late dated note keeps its date and gains the mark.
    row.setData(rows.value("usdkrw"));QVERIFY(row.findChild<QLabel*>("metricChange")->text().contains(QStringLiteral("지연")));
    QVERIFY(row.findChild<QLabel*>("metricState")->isHidden());QVERIFY(row.toolTip().contains(QStringLiteral("+0.13%")));
    auto lateNote=coreMetric("BTC 김프","+0.60","%","11/05 22:09");lateNote.freshnessText=QStringLiteral("지연");row.setData(lateNote);
    QCOMPARE(row.findChild<QLabel*>("metricState")->text(),QStringLiteral("지연"));QVERIFY(row.findChild<QLabel*>("metricChange")->text().contains(QStringLiteral("22:09")));
}

namespace {
ExploreMetricCardData dev99CoinQuote() {
    auto data=coreMetric(QStringLiteral("비트코인 BTC"),"74,531.90","USD",QStringLiteral("24시간 +2.50%"));
    data.sourceKey="synthetic:dev99:BTC";
    data.direction=MovementDirection::Up;
    data.history.available=true;data.history.sourceKey=data.sourceKey;data.history.unit=data.unit;
    data.history.points={{SourceAt-86400000,72000,false},{SourceAt-43200000,73000,true},{SourceAt,74531.90,true}};
    return data;
}
PortfolioDayHistory dev99FilledContribution() {
    PortfolioDayHistory history;
    history.comparison.available=true;history.comparison.complete=true;
    history.comparison.currency="KRW";history.comparison.openingValue="1000";history.comparison.closingValue="1025";
    history.comparison.difference="25";
    history.contributions.available=true;history.contributions.comparison=history.comparison;
    PortfolioContributionGroup coin;coin.assetClass=PortfolioContributionClass::Coin;
    coin.openingValue="1000";coin.closingValue="1025";coin.difference="25";coin.rowCount=1;
    history.contributions.groups={coin};
    return history;
}
QList<OfficialNoticeFeed> dev99NoticeFeeds(bool populated) {
    OfficialNoticeFeed feed;feed.source=OfficialNoticeSource::Okx;feed.available=true;feed.receivedAtMs=SourceAt;
    if(populated){
        OfficialNotice notice;notice.title=QStringLiteral("합성 거래지원 종료 안내");
        notice.url="https://www.okx.com/help/synthetic-dev99-delisting";notice.publishedAtMs=SourceAt;
        feed.notices.append(notice);
    }
    return {feed};
}
QLabel* dev99VisibleText(QWidget* owner,const QString& text) {
    for(auto* candidate:owner->findChildren<QLabel*>())if(candidate->isVisible()&&candidate->text()==text)return candidate;
    return nullptr;
}
class Dev99CalendarTransport final : public TradingTransport {
  public:
    QList<RequestSpec> sent;quint64 token=0;
    void send(quint64 next,const RequestSpec& request) override {token=next;sent.append(request);}
    void abort() override {}
    void answer(int status,const QByteArray& body={}) {HttpResult result;result.status=status;result.body=body;emit finished(token,result);}
};
const qint64 Dev99CalendarNow=QDateTime::fromString("2026-10-28T10:00:00Z",Qt::ISODate).toMSecsSinceEpoch();
QByteArray dev99Earnings(const QDate& date) {
    QJsonArray rows;
    if(date==QDate(2026,10,28))for(const auto& timing:{QString("time-pre-market"),QString("time-after-hours")}){
        rows.append(QJsonObject{{"symbol",timing=="time-pre-market"?"SYN1":"SYN2"},{"name","Synthetic Calendar Company With A Long Public Name"},
            {"time",timing},{"marketCap","$300,000,000,000"},{"fiscalQuarterEnding","Sep/2026"},{"epsForecast","$1.25"},{"noOfEsts","12"}});
    }
    const QJsonObject data{{"asOf",QLocale(QLocale::English).toString(date,"ddd, MMM d, yyyy")},{"headers",QJsonObject{}},{"rows",rows}};
    return QJsonDocument(QJsonObject{{"data",data},{"status",QJsonObject{{"rCode",200}}}}).toJson(QJsonDocument::Compact);
}
}

void ExploreLayoutTest::dev99CoinSummaryRemovesOnlyRetiredMetricCells() {
    ExploreCoinSummary summary;summary.resize(360,420);summary.setCoins({"BTC","ETH","BNB"},"BTC");
    const auto quote=dev99CoinQuote();summary.setQuote(quote);
    auto gap=coreMetric(QStringLiteral("테더 환산 가격차"),"11.111","%",QStringLiteral("업비트 원화마켓 미지원"));
    const auto coinbase=coreMetric(QStringLiteral("코인베이스 가격차"),"22.222","%",QStringLiteral("합성 시각"));
    const auto funding=coreMetric(QStringLiteral("이번 주기 펀딩비"),"33.333","%",QStringLiteral("합성 시각"));
    summary.setMetrics(gap,coinbase,funding,"DEV99_SYNTHETIC_FLOW");
    summary.setRelativeStrength(QStringLiteral("BTC 대비 +1.25%"),QStringLiteral("합성 공개 비교 근거"));
    summary.show();QTest::qWait(1);
    for(const auto& key:{QString("gap"),QString("coinbase"),QString("funding")}){
        QVERIFY2(!summary.findChild<QWidget*>("exploreSummaryTitle_"+key),qPrintable(key));
        QVERIFY2(!summary.findChild<QWidget*>("exploreSummaryInfo_"+key),qPrintable(key));
        QVERIFY2(!summary.findChild<QWidget*>("exploreSummaryFreshness_"+key),qPrintable(key));
    }
    QVERIFY(!summary.findChild<QWidget*>("exploreSelectedFlow"));
    for(auto* label:summary.findChildren<QLabel*>())for(const auto& removed:{QString("11.111"),QString("22.222"),QString("33.333"),QString("DEV99_SYNTHETIC_FLOW")})
        QVERIFY2(!label->text().contains(removed),qPrintable(label->text()));
    auto* support=summary.findChild<QLabel*>("exploreSummaryMarketSupport");QVERIFY(support);
    QCOMPARE(support->text(),QStringLiteral("업비트 원화마켓 미지원"));QVERIFY(support->isVisible());
    gap.changeText=QStringLiteral("합성 시각");summary.setMetrics(gap,coinbase,funding,{});QVERIFY(support->text().isEmpty());
    auto* card=summary.findChild<ExploreMetricCard*>("exploreSelectedCoinQuote");QVERIFY(card);QVERIFY(card->data()==quote);
    auto* relative=summary.findChild<QLabel*>("exploreRelativeStrength");QVERIFY(relative);QVERIFY(relative->isVisible());
    QCOMPARE(relative->text(),QStringLiteral("BTC 대비 +1.25%"));
    QSignalSpy chartRequested(&summary,&ExploreCoinSummary::chartRequested);
    auto* chart=summary.findChild<QPushButton*>("exploreSummaryChart");QVERIFY(chart);QTest::mouseClick(chart,Qt::LeftButton);QCOMPARE(chartRequested.count(),1);
    QSignalSpy coinSelected(&summary,&ExploreCoinSummary::coinSelected);QPushButton* ethereum=nullptr;QStringList tabs;
    for(auto* button:summary.findChildren<QPushButton*>())if(button->property("asset").isValid()){
        tabs.append(button->property("asset").toString());if(button->property("asset")=="ETH")ethereum=button;
    }
    QCOMPARE(tabs,QStringList({"BTC","ETH","BNB"}));QVERIFY(ethereum);QTest::mouseClick(ethereum,Qt::LeftButton);
    QCOMPARE(coinSelected.count(),1);QCOMPARE(coinSelected.first().first().toString(),QString("ETH"));
}

void ExploreLayoutTest::dev99CoinSummaryUsesValue22AndRealHistory64() {
    ExploreCoinSummary summary;summary.resize(380,420);const auto expected=dev99CoinQuote();summary.setQuote(expected);summary.show();QTest::qWait(1);
    auto* card=summary.findChild<ExploreMetricCard*>("exploreSelectedCoinQuote");QVERIFY(card);
    auto* value=card->findChild<QLabel*>("metricValue");auto* history=card->findChild<QWidget*>("metricSparkline");
    QVERIFY(value);QVERIFY(history);QVERIFY(value->isVisible());QVERIFY(history->isVisible());
    QCOMPARE(value->font().pixelSize(),KeefiFonts::font(FontRole::Value22).pixelSize());
    QCOMPARE(history->height(),64);QCOMPARE(value->text(),expected.value);
    QVERIFY(card->data()==expected);QVERIFY(card->plottedHistory()==expected.history.points);
    auto* change=card->findChild<QLabel*>("metricChange");QVERIFY(change);QVERIFY(change->isVisible());
    QTextDocument changeText;changeText.setHtml(change->text());
    QCOMPARE(changeText.toPlainText(),MovementColorPolicy::marker(expected.direction)+" "+expected.changeText);
    // The compact summary must not change the shared metric card's history contract.
    auto unavailable=expected;unavailable.history.available=false;summary.setQuote(unavailable);
    QVERIFY(!card->historyVisible());QVERIFY(card->plottedHistory().isEmpty());QVERIFY(card->data()==unavailable);
    summary.setQuote(expected);QVERIFY(card->historyVisible());QVERIFY(card->plottedHistory()==expected.history.points);
}

void ExploreLayoutTest::dev99CoinSummaryKeepsHoldingBelowNameAndClearsImmediately() {
    ExploreCoinSummary summary;summary.resize(360,420);const auto expected=dev99CoinQuote();summary.setQuote(expected);
    const auto privateText=QStringLiteral("DEV99_PRIVATE_HOLDING 123456");summary.setPrivateHolding(true,privateText);summary.show();QTest::qWait(1);
    auto* card=summary.findChild<ExploreMetricCard*>("exploreSelectedCoinQuote");QVERIFY(card);
    auto* name=card->findChild<QLabel*>("metricTitle");auto* value=card->findChild<QLabel*>("metricValue");
    auto* holding=summary.findChild<QLabel*>("exploreSelectedHolding");QVERIFY(name);QVERIFY(value);QVERIFY(holding);
    QVERIFY(holding->isVisible());QCOMPARE(holding->text(),privateText);QVERIFY(!holding->wordWrap());
    const int nameBottom=name->mapTo(&summary,name->rect().bottomLeft()).y();
    const int holdingTop=holding->mapTo(&summary,QPoint(0,0)).y();
    const int holdingBottom=holding->mapTo(&summary,holding->rect().bottomLeft()).y();
    const int valueTop=value->mapTo(&summary,QPoint(0,0)).y();
    QVERIFY(nameBottom<holdingTop);QVERIFY(holdingBottom<valueTop);
    summary.clearPrivate(); // No event-loop turn is allowed before these checks.
    QVERIFY(holding->isHidden());QVERIFY(holding->text().isEmpty());
    for(auto* widget:summary.findChildren<QWidget*>()){
        QVERIFY(!widget->toolTip().contains(privateText));QVERIFY(!widget->accessibleDescription().contains(privateText));
        if(auto* label=qobject_cast<QLabel*>(widget))QVERIFY(!label->text().contains(privateText));
    }
    QVERIFY(card->data()==expected);QVERIFY(card->plottedHistory()==expected.history.points);
}

void ExploreLayoutTest::dev99EmptyCardStripContainsOnlyEmptyCards_data() {
    QTest::addColumn<bool>("privateContent");QTest::addColumn<bool>("contributionContent");QTest::addColumn<bool>("noticeContent");
    QTest::newRow("all-empty")<<false<<false<<false;
    QTest::newRow("private-only")<<true<<false<<false;
    QTest::newRow("private-and-contribution")<<true<<true<<false;
    QTest::newRow("notice-only")<<false<<false<<true;
    QTest::newRow("all-filled")<<true<<true<<true;
}

void ExploreLayoutTest::dev99EmptyCardStripContainsOnlyEmptyCards() {
    QFETCH(bool,privateContent);QFETCH(bool,contributionContent);QFETCH(bool,noticeContent);
    ExploreTodayView view;view.resize(1000,800);view.setNoticeFeeds(dev99NoticeFeeds(noticeContent));
    if(privateContent)view.setPrivateSummary(true,"1,025","KRW",contributionContent?dev99FilledContribution():PortfolioDayHistory{});
    view.show();QTest::qWait(1);
    const auto strips=view.findChildren<QWidget*>("todayEmptyStrip");QCOMPARE(strips.size(),1);auto* strip=strips.first();
    const bool allFilled=privateContent&&noticeContent;
    QCOMPARE(strip->isVisible(),!allFilled);if(!allFilled)QCOMPARE(strip->height(),40);
    auto* placeholder=view.findChild<QLabel*>("todayPrivatePlaceholder");auto* contribution=view.findChild<QWidget*>("todayContributionPanel");
    auto* unlock=view.findChild<QPushButton*>("todayUnlock");QVERIFY(placeholder);QVERIFY(contribution);QVERIFY(unlock);
    QCOMPARE(placeholder->isVisible(),!privateContent);QCOMPARE(unlock->isVisible(),!privateContent);
    if(!privateContent){
        QVERIFY(strip->isAncestorOf(placeholder));QVERIFY(strip->isAncestorOf(unlock));
        QCOMPARE(placeholder->text(),QStringLiteral("금고를 열면 오늘 내 자산 변화가 여기에 보여요"));
        QVERIFY(dev99VisibleText(strip,QStringLiteral("오늘 내 자산")));
        QSignalSpy vaultRequested(&view,&ExploreTodayView::vaultOpenRequested);QTest::mouseClick(unlock,Qt::LeftButton);QCOMPARE(vaultRequested.count(),1);
    }else{
        auto* total=view.findChild<QLabel*>("todayPrivateTotal");QVERIFY(total);QVERIFY(total->isVisible());QCOMPARE(total->text(),QString("1,025"));
        auto* card=view.findChild<QWidget*>("todayPrivatePanel");QVERIFY(card);QVERIFY(card->isVisible());QVERIFY(card->height()>40);
        QVERIFY(!dev99VisibleText(strip,QStringLiteral("오늘 내 자산")));
    }
    QCOMPARE(contribution->isVisible(),contributionContent);
    QVERIFY(!dev99VisibleText(strip,QStringLiteral("어디서 움직였나")));
    if(contributionContent){
        QVERIFY(contribution->height()>40);
        bool hasValue=false;for(auto* value:view.findChildren<QLabel*>("todayContributionValue"))hasValue|=value->isVisible()&&value->text().contains("25");QVERIFY(hasValue);
    }
    if(!noticeContent){
        QVERIFY(dev99VisibleText(strip,QStringLiteral("거래소 공지")));QVERIFY(dev99VisibleText(strip,QStringLiteral("관련 공지가 없어요")));
    }else{
        auto* card=view.findChild<QWidget*>("todayNoticePanel");QVERIFY(card);QVERIFY(card->isVisible());QVERIFY(card->height()>40);
        QVERIFY(!dev99VisibleText(strip,QStringLiteral("거래소 공지")));
        QVERIFY(dev99VisibleText(card,OfficialNotices::sourceName(OfficialNoticeSource::Okx)+QStringLiteral(" · 거래지원 종료")));
    }
}

void ExploreLayoutTest::dev99EmptyCardStripReappearsOnPrivateClear() {
    ExploreTodayView view;view.resize(1000,800);view.setNoticeFeeds(dev99NoticeFeeds(true));
    view.setPrivateSummary(true,"991,234","KRW",dev99FilledContribution());view.show();QTest::qWait(1);
    auto* strip=view.findChild<QWidget*>("todayEmptyStrip");QVERIFY(strip);QVERIFY(strip->isHidden());
    view.clearPrivate(); // The collapsed state must not postpone private erasure.
    QVERIFY(strip->isVisible());QCOMPARE(strip->height(),40);
    auto* total=view.findChild<QLabel*>("todayPrivateTotal");QVERIFY(total);QVERIFY(total->text().isEmpty());
    for(auto* value:view.findChildren<QLabel*>("todayContributionValue")){QVERIFY(value->text().isEmpty());QVERIFY(value->isHidden());}
    QVERIFY(dev99VisibleText(strip,QStringLiteral("금고를 열면 오늘 내 자산 변화가 여기에 보여요")));
    QVERIFY(!dev99VisibleText(strip,QStringLiteral("어디서 움직였나")));
    auto* notices=view.findChild<QWidget*>("todayNoticePanel");QVERIFY(notices);QVERIFY(notices->isVisible());
    QVERIFY(dev99VisibleText(notices,OfficialNotices::sourceName(OfficialNoticeSource::Okx)+QStringLiteral(" · 거래지원 종료")));
    for(auto* label:view.findChildren<QLabel*>())QVERIFY(!label->text().contains("991,234"));
}

void ExploreLayoutTest::dev99CalendarDateAndTimeShareOneLine_data() {
    QTest::addColumn<int>("width");QTest::newRow("alongside-notices")<<700;QTest::newRow("full-body")<<976;
}

void ExploreLayoutTest::dev99CalendarDateAndTimeShareOneLine() {
    QFETCH(int,width);Dev99CalendarTransport publicTransport,earningsTransport;
    OfficialCalendar economic(&publicTransport,nullptr,[]{return Dev99CalendarNow;});
    EarningsCalendar earnings(&earningsTransport,nullptr,[]{return Dev99CalendarNow;});
    TodayCalendarView view(nullptr,[]{return Dev99CalendarNow;});view.setCalendar(&economic);view.setEarnings(&earnings);view.resize(width,400);view.show();
    QCOMPARE(publicTransport.sent.size(),0);QCOMPARE(earningsTransport.sent.size(),0);
    economic.setActive(true);earnings.setActive(true);QTRY_COMPARE(publicTransport.sent.size(),1);
    publicTransport.answer(200,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:dev99-exact\r\nSUMMARY:Consumer Price Index\r\nDTSTART:20261028T123000Z\r\nDTSTAMP:20261001T120000Z\r\nSEQUENCE:0\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    QTRY_COMPARE(publicTransport.sent.size(),2);publicTransport.answer(200,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n");
    QTRY_COMPARE(publicTransport.sent.size(),3);publicTransport.answer(503);
    QTRY_COMPARE(publicTransport.sent.size(),4);publicTransport.answer(200,QStringLiteral("<h3>2026년</h3><table id=\"tableId\"><tbody><tr><th scope=\"row\">10월 28일(수)</th></tr></tbody></table>").toUtf8());
    int answered=0;
    for(int i=0;i<9&&(earnings.feed(EarningsSource::Kind).loading||earnings.feed(EarningsSource::Nasdaq).loading);++i){
        QTRY_VERIFY(earningsTransport.sent.size()>answered);const auto request=earningsTransport.sent.at(answered++);
        if(request.url.host()=="kind.krx.co.kr")earningsTransport.answer(503);
        else earningsTransport.answer(200,dev99Earnings(QDate::fromString(QUrlQuery(request.url).queryItemValue("date"),Qt::ISODate)));
    }
    QVERIFY(earnings.feed(EarningsSource::Nasdaq).available);
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCoreApplication::processEvents();
    QTRY_COMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(),4);
    QSet<QString> times;
    for(auto* row:view.findChildren<QWidget*>("todayCalendarRow")){
        auto* date=row->findChild<QLabel*>("todayCalendarDate");auto* time=row->findChild<QLabel*>("todayCalendarTime");
        auto* title=row->findChild<QLabel*>("todayCalendarEventTitle");QVERIFY(date);QVERIFY(time);QVERIFY(title);
        QCOMPARE(date->text(),QString("10/28"));times.insert(time->text());
        QCOMPARE(date->parentWidget(),time->parentWidget());
        const int dateCenter=date->mapTo(row,date->rect().center()).y(),timeCenter=time->mapTo(row,time->rect().center()).y();
        QVERIFY2(qAbs(dateCenter-timeCenter)<=1,qPrintable(QString("date center %1, time center %2").arg(dateCenter).arg(timeCenter)));
        QVERIFY(date->mapTo(row,date->rect().topRight()).x()<time->mapTo(row,QPoint(0,0)).x());
        QVERIFY(time->width()>=time->fontMetrics().horizontalAdvance(time->text()));QVERIFY(!title->wordWrap());
        QVERIFY(row->geometry().bottom()<view.findChild<QPushButton*>("todayCalendarNext")->y());
        if(row->property("earnings").toBool())QVERIFY(title->accessibleName().contains("Synthetic Calendar Company With A Long Public Name"));
    }
    QCOMPARE(times,QSet<QString>({"21:30",QStringLiteral("시각 미공개"),QStringLiteral("장 시작 전"),QStringLiteral("장 마감 후")}));
    const int requests=publicTransport.sent.size()+earningsTransport.sent.size();
    QTest::mouseClick(view.findChild<QPushButton*>("todayCalendarFilter_kr"),Qt::LeftButton);QCOMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(),1);
    QTest::mouseClick(view.findChild<QPushButton*>("todayCalendarFilter_earnings"),Qt::LeftButton);QCOMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(),2);
    QCOMPARE(publicTransport.sent.size()+earningsTransport.sent.size(),requests);
}

void ExploreLayoutTest::todayCrossRateTooltipKeepsRefreshFailure_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<int>("failedItem");
    QTest::newRow("usd-base")<<QString("usdjpy")<<int(WorldMarketItem::UsdKrw);
    QTest::newRow("usd-yen")<<QString("usdjpy")<<int(WorldMarketItem::JpyKrw);
    QTest::newRow("aud-base")<<QString("audjpy")<<int(WorldMarketItem::AudKrw);
    QTest::newRow("aud-yen")<<QString("audjpy")<<int(WorldMarketItem::JpyKrw);
}

void ExploreLayoutTest::todayCrossRateTooltipKeepsRefreshFailure() {
    QFETCH(QString,key);
    QFETCH(int,failedItem);
    auto world=syntheticWorld(SourceAt);
    const auto metric=[&]{
        for(const auto& [name,data]:todayWorldMarketMetrics(world,SourceAt))if(name==key)return data;
        return ExploreMetricCardData{};
    };
    const auto healthy=metric();
    QVERIFY(!healthy.value.isEmpty()&&healthy.value!=QStringLiteral("—"));
    // A failed re-read retains the last valid two legs, but the displayed cross
    // must still explain that failure when its row is hovered.
    const auto failure=QStringLiteral("합성 환율 원천 재확인 실패");
    world.failures[failedItem]=failure;
    const auto retained=metric();
    QCOMPARE(retained.value,healthy.value);
    QCOMPARE(retained.changeText,healthy.changeText);
    TodayLedgerRow row;row.setData(retained);
    QVERIFY2(row.toolTip().contains(failure),qPrintable(row.toolTip()));
    QVERIFY(row.toolTip().contains(QStringLiteral("최근 재확인 실패")));
    world.failures[failedItem].clear();row.setData(metric());
    QVERIFY(!row.toolTip().contains(failure));
}

void ExploreLayoutTest::todayClosedRowsUseTheirOwnRefreshInterval_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<qint64>("ageMs");
    QTest::addColumn<bool>("late");
    QTest::newRow("fx-at-three-minutes")<<QString("usdkrw")<<qint64(180000)<<false;
    QTest::newRow("fx-after-three-minutes")<<QString("usdkrw")<<qint64(180001)<<true;
    QTest::newRow("usd-yen-after-three-minutes")<<QString("usdjpy")<<qint64(180001)<<true;
    QTest::newRow("aud-yen-after-three-minutes")<<QString("audjpy")<<qint64(180001)<<true;
    QTest::newRow("commodity-after-three-minutes")<<QString("gold")<<qint64(180001)<<true;
    QTest::newRow("index-after-three-minutes")<<QString("kospi")<<qint64(180001)<<false;
    QTest::newRow("index-at-thirty-minutes")<<QString("kospi")<<qint64(1800000)<<false;
    QTest::newRow("index-after-thirty-minutes")<<QString("kospi")<<qint64(1800001)<<true;
}

void ExploreLayoutTest::todayClosedRowsUseTheirOwnRefreshInterval() {
    QFETCH(QString,key);
    QFETCH(qint64,ageMs);
    QFETCH(bool,late);
    auto world=syntheticWorld(SourceAt);
    for(auto& quote:world.quotes){quote.session=WorldMarketSession::Closed;quote.receivedAtMs=SourceAt-ageMs;}
    ExploreMetricCardData metric;
    for(const auto& [name,data]:todayWorldMarketMetrics(world,SourceAt))if(name==key){metric=data;break;}
    QVERIFY(!metric.value.isEmpty()&&metric.value!=QStringLiteral("—"));
    // Closed indices are re-read every ten minutes; closed bank postings and
    // commodity quotes still use the one-minute lane. Each gets three periods.
    QCOMPARE(metric.freshnessText,late?QStringLiteral("지연"):QStringLiteral("마감"));
    TodayLedgerRow row;row.setData(metric);
    QCOMPARE(row.accessibleDescription().contains(QStringLiteral("지연 · 최근 확인값")),late);
    QVERIFY(row.toolTip().contains(QStringLiteral("장 마감")));
}

namespace {
struct Dev99WatchText {
    QString text;
    QFont font;
    QPointF at;
    qreal width = 0;
    QColor ink;
};
struct Dev99WatchDot { QRectF rect; QColor ink; };
class Dev99WatchPaintEngine final : public QPaintEngine {
public:
    Dev99WatchPaintEngine() : QPaintEngine(AllFeatures) {}
    QList<Dev99WatchText> texts;
    QList<Dev99WatchDot> dots;
    bool begin(QPaintDevice*) override { setActive(true); return true; }
    bool end() override { setActive(false); return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState& state) override {
        pen_ = state.pen(); brush_ = state.brush(); transform_ = state.transform();
    }
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawPolygon(const QPoint*, int, PolygonDrawMode) override {}
    void drawPath(const QPainterPath& path) override { recordDot(path.boundingRect()); }
    void drawEllipse(const QRectF& rect) override { recordDot(rect); }
    void drawEllipse(const QRect& rect) override { recordDot(rect); }
    void drawTextItem(const QPointF& at, const QTextItem& item) override {
        texts.append({item.text(), item.font(), transform_.map(at),
                      item.width() * transform_.m11(), pen_.color()});
    }
private:
    void recordDot(const QRectF& rect) {
        const auto actual = transform_.mapRect(rect);
        if (brush_.style() != Qt::NoBrush && qAbs(actual.width() - 6) <= 0.01 &&
            qAbs(actual.height() - 6) <= 0.01) dots.append({actual, brush_.color()});
    }
    QPen pen_;
    QBrush brush_;
    QTransform transform_;
};
class Dev99WatchPaintDevice final : public QPaintDevice {
public:
    explicit Dev99WatchPaintDevice(QSize size) : size_(size) {}
    mutable Dev99WatchPaintEngine engine;
    QPaintEngine* paintEngine() const override { return &engine; }
protected:
    int metric(PaintDeviceMetric value) const override {
        switch (value) {
        case PdmWidth: return size_.width(); case PdmHeight: return size_.height();
        case PdmDepth: return 32; case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return 96;
        }
    }
private:
    QSize size_;
};
struct Dev99WatchPaint { QList<Dev99WatchText> texts; QList<Dev99WatchDot> dots; };
Dev99WatchPaint dev99WatchPaintCell(QTableWidget* table, int row, int column) {
    auto* item = table->item(row, column);
    if (!item) return {};
    QStyleOptionViewItem option;
    option.initFrom(table);
    option.widget = table;
    option.rect = table->visualItemRect(item);
    option.state = QStyle::State_Enabled | QStyle::State_Active;
    Dev99WatchPaintDevice device(table->viewport()->size());
    QPainter painter(&device);
    auto* delegate = table->itemDelegateForColumn(column);
    if (!delegate) delegate = table->itemDelegate();
    delegate->paint(&painter, option, table->model()->index(row, column));
    painter.end();
    return {device.engine.texts, device.engine.dots};
}
QString dev99WatchPaintedText(const Dev99WatchPaint& paint) {
    QString text;
    for (const auto& run : paint.texts) text += run.text;
    return text;
}
qreal dev99WatchTextCenter(const Dev99WatchText& run) {
    const QFontMetricsF metrics(run.font);
    return run.at.y() + (metrics.descent() - metrics.ascent()) / 2;
}
WatchlistQuote dev99WatchQuote(const QString& id, const QString& name, double price, const QString& change) {
    WatchlistQuote quote;
    quote.section = "coin"; quote.id = id; quote.name = name;
    quote.priceValue = price; quote.price = QString::number(price, 'f', 0); quote.change = change;
    quote.evidenceProvider = QStringLiteral("합성 공개 원천");
    quote.evidenceSourceTimeMs = SourceAt;
    return quote;
}
}

void ExploreLayoutTest::dev99WatchlistCompactRowsKeepFixedColumnsAndSingleLine_data() {
    QTest::addColumn<int>("width");
    QTest::newRow("window-360") << 360;
    QTest::newRow("wide-380") << 380;
}

void ExploreLayoutTest::dev99WatchlistCompactRowsKeepFixedColumnsAndSingleLine() {
    QFETCH(int, width);
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    const QStringList saved{"BTC", "ETH"};
    QSettings().setValue("explore/watchlist/coin", saved);
    auto bitcoin = dev99WatchQuote("BTC", QStringLiteral("비트코인"), 103450000, "-1.70");
    bitcoin.price = "103,450,000";
    const auto ethereum = dev99WatchQuote("ETH", QStringLiteral("이더리움 합성 이름이 아주 길어지는 경우의 말줄임 확인"), 4128000, "2.50");
    ExploreWatchlist watch;
    watch.setCompact(true); watch.setQuotes({bitcoin, ethereum});
    watch.resize(width, 460); watch.show();
    auto* table = watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QTRY_VERIFY(table->isVisible() && table->rowCount() == 2);
    QTRY_COMPARE(table->columnWidth(0), 14);
    QCOMPARE(table->columnWidth(2), 92);
    QCOMPARE(table->columnWidth(3), 62);
    QCOMPARE(table->columnWidth(1), table->viewport()->width() - 14 - 92 - 62);
    QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
    for (int row = 0; row < 2; ++row) QCOMPARE(table->visualItemRect(table->item(row, 1)).height(), 32);
    const auto namePaint = dev99WatchPaintCell(table, 0, 1);
    const auto pricePaint = dev99WatchPaintCell(table, 0, 2);
    const auto changePaint = dev99WatchPaintCell(table, 0, 3);
    const Dev99WatchText* code = nullptr;
    const Dev99WatchText* name = nullptr;
    for (const auto& run : namePaint.texts) {
        if (run.text == "BTC") code = &run;
        if (run.text == QStringLiteral("비트코인")) name = &run;
    }
    QVERIFY(code && name);
    QVERIFY(code->font.weight() >= QFont::DemiBold);
    QCOMPARE(name->font.pixelSize(), KeefiFonts::font(FontRole::Caption12).pixelSize());
    QCOMPARE(name->ink, themeColors().text3);
    QVERIFY(code->at.x() + code->width <= name->at.x() + 1);
    QVERIFY(qAbs(dev99WatchTextCenter(*code) - dev99WatchTextCenter(*name)) <= 1);
    QVERIFY(!pricePaint.texts.isEmpty() && !changePaint.texts.isEmpty());
    for (const auto& paint : {pricePaint, changePaint}) {
        const auto text = dev99WatchPaintedText(paint);
        QVERIFY2(!text.contains(QChar(0x2026)) && !text.contains("..."), qPrintable(text));
        for (const auto& run : paint.texts)
            QVERIFY2(qAbs(dev99WatchTextCenter(run) - dev99WatchTextCenter(*code)) <= 1, qPrintable(run.text));
    }
    QVERIFY(dev99WatchPaintedText(pricePaint).contains("103,450,000"));
    QVERIFY(dev99WatchPaintedText(changePaint).contains("-1.70%"));
    const auto longName = dev99WatchPaintCell(table, 1, 1);
    QVERIFY(dev99WatchPaintedText(longName).contains(QChar(0x2026)));
    const auto nameBounds = table->visualItemRect(table->item(1, 1));
    for (const auto& run : longName.texts) {
        QVERIFY(run.at.x() >= nameBounds.left() - 1);
        QVERIFY(run.at.x() + run.width <= nameBounds.right() + 1);
    }
    QCOMPARE(watch.ids("coin"), saved);
    QCOMPARE(QSettings().value("explore/watchlist/coin").toStringList(), saved);
}

void ExploreLayoutTest::dev99WatchlistStaleDotReplacesNameSuffixAndKeepsLegend_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void ExploreLayoutTest::dev99WatchlistStaleDotReplacesNameSuffixAndKeepsLegend() {
    QFETCH(bool, dark);
    const auto oldTheme = KeefiTheme::instance()->mode();
    const auto restoreTheme = qScopeGuard([&] { KeefiTheme::instance()->setMode(oldTheme); });
    KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings().setValue("explore/watchlist/coin", QStringList{"BTC"});
    auto quote = dev99WatchQuote("BTC", QStringLiteral("비트코인"), 103450000, "-1.70");
    quote.evidenceState = usabilityText(UsabilityText::Delayed);
    quote.evidence = QStringLiteral("이전 시세 · 갱신 대기");
    ExploreWatchlist watch; watch.setCompact(true); watch.setQuotes({quote});
    watch.resize(360, 460); watch.show();
    auto* table = watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QTRY_VERIFY(table->isVisible() && table->rowCount() == 1);
    const auto name = dev99WatchPaintCell(table, 0, 1);
    const auto price = dev99WatchPaintCell(table, 0, 2);
    QVERIFY(!dev99WatchPaintedText(name).contains(QStringLiteral("이전 시세")));
    QVERIFY(!table->item(0, 1)->text().contains(QStringLiteral("이전 시세")));
    QVERIFY(!dev99WatchPaintedText(price).contains(usabilityText(UsabilityText::Delayed)));
    QList<Dev99WatchDot> dots = name.dots; dots.append(price.dots);
    const auto dot = std::find_if(dots.cbegin(), dots.cend(), [](const auto& value) {
        return value.ink == themeColors().text3;
    });
    QVERIFY2(dot != dots.cend(), "A stale compact quote must paint a grey 6px dot");
    QCOMPARE(dot->rect.size(), QSizeF(6, 6));
    const auto position = dot->rect.center().toPoint();
    QVERIFY(table->viewport()->rect().contains(position));
    const auto hideTooltip = qScopeGuard([] { QToolTip::hideText(); });
    QHelpEvent help(QEvent::ToolTip, position, table->viewport()->mapToGlobal(position));
    QApplication::sendEvent(table->viewport(), &help);
    QTRY_VERIFY(QToolTip::isVisible());
    QVERIFY2(QToolTip::text().contains(QStringLiteral("이전 시세 · 갱신 대기")), qPrintable(QToolTip::text()));
    QToolTip::hideText();
    bool legend = false;
    for (auto* label : watch.findChildren<QLabel*>())
        if (label->isVisible() && label->text().contains(QStringLiteral("이전 시세")) &&
            label->text().contains(QStringLiteral("갱신 대기"))) legend = true;
    QVERIFY2(legend, "The compact list must keep a visible stale-dot legend");
    const auto saved = QSettings().value("explore/watchlist/coin");
    quote.evidenceState.clear(); quote.evidence.clear(); watch.setQuotes({quote});
    const auto freshName = dev99WatchPaintCell(table, 0, 1);
    const auto freshPrice = dev99WatchPaintCell(table, 0, 2);
    QVERIFY(freshName.dots.isEmpty() && freshPrice.dots.isEmpty());
    QCOMPARE(QSettings().value("explore/watchlist/coin"), saved);
    watch.setCompact(false); quote.evidenceState = usabilityText(UsabilityText::Delayed); watch.setQuotes({quote});
    QVERIFY(table->item(0, 2)->text().contains(usabilityText(UsabilityText::Delayed)));
}

void ExploreLayoutTest::dev99WatchlistHeaderSortKeepsValuesAndSavedOrder() {
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    const QStringList saved{"SOL", "BTC", "ETH"};
    QSettings().setValue("explore/watchlist/coin", saved);
    const QList<WatchlistQuote> quotes{
        dev99WatchQuote("SOL", QStringLiteral("솔라나"), 50, "0.00"),
        dev99WatchQuote("BTC", QStringLiteral("비트코인"), 100, "-1.70"),
        dev99WatchQuote("ETH", QStringLiteral("이더리움"), 200, "2.50")};
    ExploreWatchlist watch; watch.setCompact(true); watch.setQuotes(quotes);
    watch.resize(360, 460); watch.show();
    auto* table = watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    auto* header = table->horizontalHeader();
    QTRY_VERIFY(header->isVisible());
    QHash<QString, QStringList> originalValues;
    for (int row = 0; row < table->rowCount(); ++row)
        originalValues.insert(table->item(row, 1)->data(Qt::UserRole).toString(),
                              {table->item(row, 2)->text(), table->item(row, 3)->text()});
    QSignalSpy selected(&watch, &ExploreWatchlist::itemSelected);
    QSignalSpy changed(&watch, &ExploreWatchlist::favouritesChanged);
    const auto click = [&](int column) {
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
            QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                   header->viewport()->height() / 2));
    };
    click(2); QCOMPARE(orderedIds(*table), (QStringList{"ETH", "BTC", "SOL"}));
    click(2); QCOMPARE(orderedIds(*table), (QStringList{"SOL", "BTC", "ETH"}));
    click(3); QCOMPARE(orderedIds(*table), (QStringList{"ETH", "SOL", "BTC"}));
    click(3); QCOMPARE(orderedIds(*table), (QStringList{"BTC", "SOL", "ETH"}));
    watch.setQuotes(quotes);
    QCOMPARE(orderedIds(*table), (QStringList{"BTC", "SOL", "ETH"}));
    for (int row = 0; row < table->rowCount(); ++row)
        QCOMPARE((QStringList{table->item(row, 2)->text(), table->item(row, 3)->text()}),
                 originalValues.value(table->item(row, 1)->data(Qt::UserRole).toString()));
    auto* reset = watch.findChild<QPushButton*>("watchlistInsertionOrder"); QVERIFY(reset && reset->isVisible());
    QTest::mouseClick(reset, Qt::LeftButton);
    QCOMPARE(orderedIds(*table), saved);
    QCOMPARE(watch.ids("coin"), saved);
    QCOMPARE(QSettings().value("explore/watchlist/coin").toStringList(), saved);
    QCOMPARE(selected.count(), 0); QCOMPARE(changed.count(), 0);
}

void ExploreLayoutTest::dev99WatchlistCompactViewportFitsTenRows_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("visibleRows");
    QTest::newRow("window-rail-ten") << QSize(360, 460) << 10;
    QTest::newRow("wide-rail-twelve") << QSize(380, 540) << 12;
}

void ExploreLayoutTest::dev99WatchlistCompactViewportFitsTenRows() {
    QFETCH(QSize, size);
    QFETCH(int, visibleRows);
    QTemporaryDir settings; QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QStringList saved;
    for (int row = 0; row < 16; ++row) saved.append(symbol(row));
    QSettings().setValue("explore/watchlist/coin", saved);
    ExploreWatchlist watch; watch.setCompact(true); watch.setQuotes(quotesAt(0, 16));
    watch.resize(size); watch.show();
    auto* table = watch.findChild<QTableWidget*>("watchlistTable"); QVERIFY(table);
    QTRY_VERIFY(table->isVisible());
    QTRY_VERIFY(table->viewport()->rect().contains(table->visualItemRect(table->item(visibleRows - 1, 3))));
    for (int row = 0; row < visibleRows; ++row)
        QCOMPARE(table->visualItemRect(table->item(row, 1)).height(), 32);
    QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(watch.ids("coin"), saved);
    QCOMPARE(QSettings().value("explore/watchlist/coin").toStringList(), saved);
}

void ExploreLayoutTest::spotlightKoreanNameTooltipRemainsLiteral() {
    const auto name = QStringLiteral("<b>합성 한국 종목</b>");
    SpotlightItem item{QStringLiteral("000001"), name, QStringLiteral("Vol 100"),
                       QStringLiteral("100 KRW"), QStringLiteral("1")};
    SpotlightCard card("kr", "KOREA");
    card.setModes({{"cap", "시가총액", {item}}});
    card.resize(500, 450);
    card.show();
    auto* rows = card.findChild<QWidget*>("spotlightRows");
    QVERIFY(rows);
    QMouseEvent move(QEvent::MouseMove, QPointF(60, 20), rows->mapToGlobal(QPoint(60, 20)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(rows, &move);
    const auto shown = keefi::test::projectTooltip(rows->toolTip());
    QVERIFY(shown.text.startsWith(name + QStringLiteral(" · 100 KRW")));
    auto* star = rows->findChild<QToolButton*>("spotlightStar0");
    QVERIFY(star);
    QCOMPARE(star->accessibleName(), name + QStringLiteral(" 관심 리스트에 추가"));
    QCOMPARE(keefi::test::projectTooltip(star->toolTip()).text, star->accessibleName());
    QVERIFY(!shown.image);
    QVERIFY(!shown.anchor);
    QCOMPARE(shown.resourceRequests, 0);
    QCOMPARE(card.visibleItems().first().symbol, name);
}
QTEST_MAIN(ExploreLayoutTest)
#include "tst_explore_layout.moc"
