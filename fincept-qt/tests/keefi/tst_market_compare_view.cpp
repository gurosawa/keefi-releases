#include "keefi/CollectorStore.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/FxHistory.h"
#include "keefi/MarketCompareSource.h"
#include "keefi/MarketCompareView.h"
#include "keefi/InfoButton.h"
#include "keefi/TradingTransport.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/Vault.h"
#include <QRegularExpression>
#include <QAbstractItemView>
#include <QApplication>
#include <QStyleOptionComboBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPaintEngine>
#include <QPainter>
#include <QScopeGuard>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace keefi;

namespace {
constexpr qint64 Hour = 3600000;
constexpr qint64 Minute = 60000;
const qint64 T0 = QDateTime(QDate(2026, 11, 6), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();

class Wire final : public TradingTransport {
  public:
    QList<QPair<quint64, RequestSpec>> sent;
    void send(quint64 token, const RequestSpec& request) override { sent.append({token, request}); }
    void abort() override {}
    void answer(int index, const QByteArray& body, int status = 200) {
        HttpResult result;
        result.status = status;
        result.body = body;
        emit finished(sent[index].first, result);
    }
};

QByteArray catalog() {
    return R"({"symbols":[{"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},)"
           R"({"symbol":"CLUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},)"
           R"({"symbol":"QQQUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"EQUITY"}]})";
}

// Gold drifts up while bitcoin mirrors it; oil and nasdaq follow their own paths.
QByteArray klines(const QString& symbol, qint64 start, int count) {
    QStringList rows;
    double price = symbol == "XAUUSDT" ? 4000 : symbol == "CLUSDT" ? 60 : symbol == "QQQUSDT" ? 600 : 100000;
    for (int i = 0; i < count; ++i) {
        const double wave = std::sin(i * 0.37) * 0.004 + (i % 7 == 0 ? 0.002 : -0.001);
        double move = wave;
        if (symbol == "BTCUSDT") move = -wave * 1.6;
        if (symbol == "CLUSDT") move = std::cos(i * 0.21) * 0.006;
        if (symbol == "QQQUSDT") move = std::sin(i * 0.11 + 1) * 0.003;
        price *= std::exp(move);
        const auto close = QString::number(price, 'f', 2);
        const auto open = start + i * Hour;
        rows.append(QStringLiteral("[%1,\"%2\",\"%2\",\"%2\",\"%2\",\"1\",%3,\"1\",1,\"1\",\"1\",\"0\"]").arg(open).arg(close).arg(open + Hour - 1));
    }
    return '[' + rows.join(',').toUtf8() + ']';
}
QByteArray upbitKlines(qint64 start, int count) {
    QStringList rows;
    for (int i = count - 1; i >= 0; --i) {
        const auto open = start + i * Hour;
        const auto utc = QDateTime::fromMSecsSinceEpoch(open, QTimeZone::UTC).toString("yyyy-MM-ddTHH:mm:ss");
        rows.append(QStringLiteral("{\"market\":\"KRW-BTC\",\"candle_date_time_utc\":\"%1\",\"candle_date_time_kst\":\"%1\","
                                   "\"opening_price\":135000000,\"high_price\":135000000,\"low_price\":135000000,\"trade_price\":135000000,"
                                   "\"timestamp\":%2,\"candle_acc_trade_price\":1,\"candle_acc_trade_volume\":1,\"unit\":60}")
                        .arg(utc).arg(open + 1000));
    }
    return '[' + rows.join(',').toUtf8() + ']';
}
QByteArray jsonLine(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
template<class View> bool injectPublicSettings(View& view,QSettings* settings){
    if constexpr(requires{view.setSettings(settings);}){view.setSettings(settings);return true;}
    return false;
}

QList<double> relationPrices(bool target) {
    QList<double> prices;
    double base = 100, compared = 50;
    for (int i = 0; i < 300; ++i) {
        prices.append(target ? compared : base);
        const double move = .01 * std::sin(i * 1.3) + .005 * std::cos(i * .7);
        const double noise = .003 * std::sin(i * 2.9);
        const bool opposite = (i >= 120 && i < 150) || (i >= 190 && i < 215) || (i >= 250 && i < 270);
        base *= std::exp(move);
        compared *= std::exp((opposite ? -move : move) + noise);
    }
    return prices;
}

QByteArray relationBars(const QList<double>& prices, qint64 start, int first, int count, bool upbit) {
    QStringList rows;
    for (int i = first; i < first + count; ++i) {
        const auto open = start + i * 15 * Minute;
        const auto close = QString::number(prices[i] * (upbit ? 1350 : 1), 'g', 16);
        if (upbit) {
            const auto utc = QDateTime::fromMSecsSinceEpoch(open, QTimeZone::UTC).toString("yyyy-MM-ddTHH:mm:ss");
            rows.prepend(QStringLiteral("{\"market\":\"KRW-BTC\",\"candle_date_time_utc\":\"%1\",\"candle_date_time_kst\":\"%1\","
                                        "\"opening_price\":%2,\"high_price\":%2,\"low_price\":%2,\"trade_price\":%2,"
                                        "\"timestamp\":%3,\"candle_acc_trade_price\":1,\"candle_acc_trade_volume\":1,\"unit\":15}")
                             .arg(utc, close).arg(open + 1000));
        } else {
            rows.append(QStringLiteral("[%1,\"%2\",\"%2\",\"%2\",\"%2\",\"1\",%3,\"1\",1,\"1\",\"1\",\"0\"]")
                            .arg(open).arg(close).arg(open + 15 * Minute - 1));
        }
    }
    return '[' + rows.join(',').toUtf8() + ']';
}

bool answerRelationPage(Wire& wire, int requestStart, qint64 start, int first, int count) {
    for (int i = requestStart; i < requestStart + 4; ++i) {
        if (!QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000)) return false;
        const auto& url = wire.sent[i].second.url;
        const bool upbit = url.host() == QLatin1String("api.upbit.com");
        const bool target = upbit || QUrlQuery(url).queryItemValue("symbol") == QLatin1String("BTCUSDT");
        wire.answer(i, relationBars(relationPrices(target), start, first, count, upbit));
    }
    return true;
}
bool openCompareIntervals(MarketCompareView& view) {
    if(!view.isVisible()){
        if(!view.isWindow())return false;
        view.resize(1180,860);view.show();
        if(!QTest::qWaitForWindowExposed(&view))return false;
    }
    const auto visibleMenu=[&]{
        auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if(!menu||!menu->isVisible())return false;
        for(const auto interval:{Minute,5*Minute,15*Minute,Hour,4*Hour,24*Hour}){
            auto* button=view.findChild<QPushButton*>(QStringLiteral("marketCompareTimeframe_%1").arg(interval));
            if(!button||!menu->isAncestorOf(button)||!button->isVisible())return false;
        }
        return true;
    };
    if(visibleMenu())return true;
    auto* opener=view.findChild<QPushButton*>("marketCompareIntervalMenu");
    if(!opener)return false;
    if(auto* scroll=view.findChild<QScrollArea*>("marketCompareScroll"))scroll->ensureWidgetVisible(opener);
    QCoreApplication::processEvents();
    if(!opener->isVisibleTo(&view))return false;
    QTest::mouseClick(opener,Qt::LeftButton);
    return QTest::qWaitFor(visibleMenu,1000);
}
bool closeCompareIntervals(MarketCompareView& view) {
    auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());
    auto* minute=view.findChild<QPushButton*>("marketCompareTimeframe_60000");
    if(!menu||!minute||!menu->isAncestorOf(minute))return false;
    QTest::keyClick(menu,Qt::Key_Escape);
    return QTest::qWaitFor([&]{return !menu->isVisible();},1000);
}
QString presentedCompareLegend(MarketCompareView& view) {
    QStringList rows;
    for(const auto* symbol:{"XAUUSDT","CLUSDT","QQQUSDT","BTCUSDT"}){
        auto* chip=view.findChild<QPushButton*>(QString("marketCompareChip_")+symbol);
        if(!chip||!chip->isVisibleTo(&view)){QTest::qFail("The public legend must be presented by a visible chart chip",__FILE__,__LINE__);return {};}
        rows.append(chip->text()+"\n"+chip->toolTip());
    }
    return rows.join('\n');
}
QString presentedCompareInformation(MarketCompareView& view) {
    auto* info=view.findChild<InfoButton*>("marketCompareInformation");
    if(!info){QTest::qFail("Comparison information button is missing",__FILE__,__LINE__);return {};}
    if(auto* scroll=view.findChild<QScrollArea*>("marketCompareScroll"))scroll->ensureWidgetVisible(info);
    QCoreApplication::processEvents();
    if(!info->isVisibleTo(&view)){QTest::qFail("Comparison information button must be visible",__FILE__,__LINE__);return {};}
    QTest::mouseClick(info,Qt::LeftButton);
    QDialog* dialog=nullptr;
    const auto opened=QTest::qWaitFor([&]{for(auto* candidate:info->findChildren<QDialog*>("infoDialog"))if(candidate->isVisible()){dialog=candidate;return true;}return false;},1000);
    if(!opened){QTest::qFail("Clicking the comparison information button must open its real dialog",__FILE__,__LINE__);return {};}
    auto* body=dialog->findChild<QPlainTextEdit*>("infoPublicText");
    if(!body||!body->isVisibleTo(dialog)||!dialog->rect().contains(QRect(body->mapTo(dialog,QPoint()),body->size()))||body->viewport()->height()<body->fontMetrics().height()){
        QTest::qFail("The moved explanation must be readable inside the information dialog",__FILE__,__LINE__);dialog->close();return {};
    }
    const auto text=body->toPlainText();dialog->close();return text;
}
QString presentedCompareNotes(MarketCompareView& view) {
    auto* collector=view.findChild<QLabel*>("marketCompareCollectorStatus");
    if(!collector||!collector->isVisibleTo(&view)){QTest::qFail("Collector state must remain visible in the footer",__FILE__,__LINE__);return {};}
    return presentedCompareInformation(view)+"\n"+collector->text();
}
class CompareSelectorPaintEngine final : public QPaintEngine {
  public:
    CompareSelectorPaintEngine():QPaintEngine(AllFeatures){}
    QString text;
    bool begin(QPaintDevice*) override {setActive(true);return true;}
    bool end() override {setActive(false);return true;}
    Type type() const override {return User;}
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&) override {}
    void drawPolygon(const QPointF*,int,PolygonDrawMode) override {}
    void drawPolygon(const QPoint*,int,PolygonDrawMode) override {}
    void drawPath(const QPainterPath&) override {}
    void drawTextItem(const QPointF&,const QTextItem& item) override {text+=item.text();}
};
class CompareSelectorPaintDevice final : public QPaintDevice {
  public:
    explicit CompareSelectorPaintDevice(QSize size):size_(size){}
    mutable CompareSelectorPaintEngine engine;
    QPaintEngine* paintEngine() const override {return &engine;}
  protected:
    int metric(PaintDeviceMetric value) const override {switch(value){case PdmWidth:return size_.width();case PdmHeight:return size_.height();case PdmDepth:return 32;case PdmNumColors:return 0;case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;}}
  private:QSize size_;
};
} // namespace

class MarketCompareViewTests final : public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
  private slots:
    void returningToTheSameMarketKeepsSelectorRows() {
        MarketCompareSource source(nullptr);
        MarketCompareView view(&source);
        view.show();
        auto* selector = view.findChild<QComboBox*>("marketCompareBaseSelector");
        QVERIFY(selector);
        const QPersistentModelIndex selected(selector->model()->index(selector->currentIndex(), 0));
        QVERIFY(selected.isValid());
        const auto identity = selector->currentData();
        QSignalSpy removed(selector->model(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(selector->model(), &QAbstractItemModel::rowsInserted);
        view.hide();
        view.show();
        QCOMPARE(removed.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QVERIFY(selected.isValid());
        QCOMPARE(selector->currentData(), identity);
        source.setWonMode(CompareWonMode::WonRaw);
        QCOMPARE(selector->currentData().toString(), QStringLiteral("coin:upbit:KRW-BTC"));
    }
    void externalCatalogClassificationTooltipIsLiteral_data() {
        QTest::addColumn<QString>("classification");
        QTest::newRow("html-tag") << QStringLiteral("<b>catalog classification</b>");
        QTest::newRow("entity") << QStringLiteral("&lt;b&gt;catalog classification&lt;/b&gt;");
        QTest::newRow("plain-classification") << QStringLiteral("UNRECOGNIZED_CLASSIFICATION");
    }
    void externalCatalogClassificationTooltipIsLiteral() {
        QFETCH(QString, classification);
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        MarketCompareSource source(&wire, nullptr, [&] { return T0 + elapsed.elapsed(); });
        source.setInterval(Hour);
        MarketCompareView view(&source);
        view.resize(1180, 860);
        view.show();
        QTRY_COMPARE(wire.sent.size(), 1);
        QCOMPARE(wire.sent.first().second.url, MarketCompare::perpCatalogRequest().url);

        auto object = QJsonDocument::fromJson(catalog()).object();
        auto symbols = object.value("symbols").toArray();
        QVERIFY(!symbols.isEmpty());
        auto gold = symbols.first().toObject();
        QCOMPARE(gold.value("symbol").toString(), QStringLiteral("XAUUSDT"));
        gold.insert("underlyingType", classification);
        symbols[0] = gold;
        object.insert("symbols", symbols);
        wire.answer(0, QJsonDocument(object).toJson(QJsonDocument::Compact));

        const auto reason = QStringLiteral("거래소 분류(%1)가 허용 목록(%2)과 달라요")
                                .arg(classification, QStringLiteral("COMMODITY"));
        QTRY_COMPARE(source.snapshot().series.first().reason, reason);
        const auto snapshot = source.snapshot();
        QCOMPARE(snapshot.series.first().spec.symbol, QStringLiteral("XAUUSDT"));
        QVERIFY(!snapshot.series.first().available);

        auto* chip = view.findChild<QPushButton*>("marketCompareChip_XAUUSDT");
        QVERIFY(chip);
        QVERIFY(chip->isVisibleTo(&view));
        const auto literal = chip->accessibleDescription();
        QVERIFY(literal.contains(reason));
        QVERIFY(literal.contains(classification));

        // Follow QToolTip's AutoText rule; the fixture contains only bold tags
        // or entity text, with no links, images or resource-bearing markup.
        const auto tooltip = chip->toolTip();
        const auto shown = Qt::mightBeRichText(tooltip)
                               ? QTextDocumentFragment::fromHtml(tooltip).toPlainText()
                               : tooltip;
        QCOMPARE(shown, literal);
        QVERIFY(shown.contains(classification));
    }

    void dev99SummaryUsesOnlyObservedChanges_data(){
        QTest::addColumn<QList<double>>("changes");QTest::addColumn<QStringList>("required");QTest::addColumn<bool>("complete");
        QTest::newRow("only-nasdaq-up")<<QList<double>{-15,-8,3,-4}<<QStringList{"나스닥","+3.00%","금","-15.00%"}<<true;
        QTest::newRow("all-up")<<QList<double>{2,4,3,5}<<QStringList{"넷 다 올랐어요"}<<true;
        QTest::newRow("all-down")<<QList<double>{-2,-4,-3,-5}<<QStringList{"넷 다 내렸어요"}<<true;
        QTest::newRow("opposite")<<QList<double>{-5,2,3,5}<<QStringList{"반대로 움직였어요"}<<true;
        QTest::newRow("missing-series")<<QList<double>{2,4,3,5}<<QStringList{}<<false;
        QTest::newRow("all-zero")<<QList<double>{0,0,0,0}<<QStringList{"변동이 없어요"}<<true;
    }
    void dev99SummaryUsesOnlyObservedChanges(){
        QFETCH(QList<double>,changes);QFETCH(QStringList,required);QFETCH(bool,complete);
        Wire wire;const qint64 now=T0+90*Hour;QElapsedTimer clock;clock.start();MarketCompareSource source(&wire,nullptr,[&]{return now+clock.elapsed();});
        source.setInterval(Hour);MarketCompareView view(&source);view.resize(1440,900);view.show();
        QTRY_VERIFY(!wire.sent.isEmpty());wire.answer(0,catalog());
        const QStringList symbols{"XAUUSDT","CLUSDT","QQQUSDT","BTCUSDT"};
        for(int n=1;n<5;++n){
            QTRY_COMPARE(wire.sent.size(),n+1);
            const auto symbol=QUrlQuery(wire.sent[n].second.url).queryItemValue("symbol");const int slot=symbols.indexOf(symbol);QVERIFY(slot>=0);
            QStringList rows;
            if(complete||symbol!="CLUSDT")for(int i=0;i<80;++i){
                const auto at=T0+i*Hour;const double progress=double(i)/79;
                const bool opposite=QByteArray(QTest::currentDataTag())=="opposite";
                const double price=opposite?100*std::exp(std::log1p(changes[slot]/100.0)*progress+(slot==0?-1:1)*0.01*std::sin(8*std::acos(-1.0)*progress))
                    :100.0*(1+changes[slot]/100.0*progress);
                const auto close=QString::number(price,'f',8);
                rows<<QString("[%1,\"%2\",\"%2\",\"%2\",\"%2\",\"1\",%3,\"1\",1,\"1\",\"1\",\"0\"]").arg(at).arg(close).arg(at+Hour-1);
            }
            wire.answer(n,'['+rows.join(',').toUtf8()+']');
        }
        if(QByteArray(QTest::currentDataTag())=="opposite"){
            const auto snapshot=source.snapshot();CompareSeries gold,bitcoin;
            for(const auto& series:snapshot.series){if(series.spec.symbol=="XAUUSDT")gold=series;if(series.spec.symbol=="BTCUSDT")bitcoin=series;}
            const auto relation=MarketCompare::rollingCorrelation(bitcoin,gold);QVERIFY(!relation.isEmpty());QVERIFY(relation.last().r20.has_value());QVERIFY(*relation.last().r20<-.5);
        }
        auto* summary=view.findChild<QLabel*>("marketCompareSummary");QVERIFY(summary);
        if(!complete){QVERIFY(summary->isHidden());QVERIFY(summary->text().isEmpty());return;}
        QVERIFY(summary->isVisible());QCOMPARE(summary->font().pixelSize(),15);
        for(const auto& text:required)QVERIFY2(summary->text().contains(text),qPrintable(summary->text()+" missing "+text));
        for(const auto* forbidden:{"사다","팔다","사세요","파세요","위험","기회","추천"})QVERIFY(!summary->text().contains(QString::fromUtf8(forbidden)));
    }
    void dev99ChartChipsAndCorrelationToggleKeepRequestsAndPreference(){
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings settings(dir.filePath("dev99-public-compare.ini"),QSettings::IniFormat);
        Wire wire;const qint64 now=T0+90*Hour;QElapsedTimer clock;clock.start();MarketCompareSource source(&wire,nullptr,[&]{return now+clock.elapsed();});
        source.setInterval(Hour);MarketCompareView view(&source);view.setSettings(&settings);view.resize(1440,900);view.show();
        QTRY_VERIFY(!wire.sent.isEmpty());wire.answer(0,catalog());
        for(int n=1;n<5;++n){QTRY_COMPARE(wire.sent.size(),n+1);wire.answer(n,klines(QUrlQuery(wire.sent[n].second.url).queryItemValue("symbol"),T0,80));}
        auto* chart=view.findChild<QWidget*>("marketCompareChart");auto* correlation=view.findChild<QWidget*>("marketCompareCorrelation");
        auto* toggle=view.findChild<QPushButton*>("marketCompareCorrelationToggle");
        auto* gold=view.findChild<QPushButton*>("marketCompareChip_XAUUSDT");auto* btc=view.findChild<QPushButton*>("marketCompareChip_BTCUSDT");
        QVERIFY(chart&&correlation&&toggle&&gold&&btc);QVERIFY(correlation->isHidden());QVERIFY(gold->isChecked());QVERIFY(btc->isChecked());
        auto* base=view.findChild<QComboBox*>("marketCompareBaseSelector");auto* won=view.findChild<QComboBox*>("marketCompareWonSelector");
        QVERIFY(base&&won);QVERIFY(chart->isAncestorOf(base)&&chart->isAncestorOf(won));QCOMPARE(base->height(),24);QCOMPARE(won->height(),24);
        const int requests=wire.sent.size();const int collapsedHeight=chart->height();
        const auto goldPixels=[&]{const auto image=chart->grab().toImage();const auto ink=themeColors().studyOrange;int count=0;
            for(int y=qRound(80*image.devicePixelRatio());y<image.height()-qRound(30*image.devicePixelRatio());++y)
                for(int x=qRound(8*image.devicePixelRatio());x<image.width()-qRound(72*image.devicePixelRatio());++x){const auto pixel=image.pixelColor(x,y);
                    if(qAbs(pixel.red()-ink.red())<16&&qAbs(pixel.green()-ink.green())<16&&qAbs(pixel.blue()-ink.blue())<16)++count;}
            return count;};
        QVERIFY(goldPixels()>20);
        QTest::mouseClick(gold,Qt::LeftButton);QVERIFY(!gold->isChecked());QCOMPARE(wire.sent.size(),requests);QCOMPARE(goldPixels(),0);
        QTest::mouseClick(btc,Qt::LeftButton);QVERIFY(btc->isChecked());QCOMPARE(wire.sent.size(),requests);
        QTest::mouseClick(toggle,Qt::LeftButton);QTRY_VERIFY(correlation->isVisible());QVERIFY(chart->height()<collapsedHeight);
        auto* target=view.findChild<QComboBox*>("marketCompareTargetSelector");QVERIFY(target&&target->isVisible());QCOMPARE(wire.sent.size(),requests);
        settings.sync();QVERIFY(settings.value("marketCompare/correlationExpanded/v1").toBool());
        QVERIFY(settings.value("marketCompare/hiddenSeries/v1").toStringList().contains("perp:binance-usdm:XAUUSDT"));
        Wire reopenedWire;MarketCompareSource reopenedSource(&reopenedWire,nullptr,[&]{return now+clock.elapsed();});MarketCompareView reopened(&reopenedSource);reopened.setSettings(&settings);
        auto* restoredGold=reopened.findChild<QPushButton*>("marketCompareChip_XAUUSDT");auto* restoredCorrelation=reopened.findChild<QWidget*>("marketCompareCorrelation");QVERIFY(restoredGold&&restoredCorrelation);
        QVERIFY(!restoredGold->isChecked());QVERIFY(!restoredCorrelation->isHidden());QCOMPARE(reopenedWire.sent.size(),0);
        QTest::mouseClick(gold,Qt::LeftButton);QVERIFY(gold->isChecked());QCOMPARE(wire.sent.size(),requests);
        const auto originalBase=view.baseOpenMs();QTest::mouseMove(chart,chart->rect().center());QCoreApplication::processEvents();
        const QRegularExpression valuePattern("([+-]?[0-9]+[.][0-9]+)%$");double previous=std::numeric_limits<double>::infinity();int rows=0;
        for(const auto& row:chart->accessibleDescription().split('\n')){const auto match=valuePattern.match(row);if(!match.hasMatch())continue;const double value=match.captured(1).toDouble();QVERIFY(value<=previous);previous=value;++rows;}
        QCOMPARE(rows,4);QCOMPARE(view.baseOpenMs(),originalBase);QCOMPARE(wire.sent.size(),requests);
    }
    void initTestCase() {
        QVERIFY(settingsRoot_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot_.filePath("system"));
        KeefiFonts::registerBundledFonts();
    }


    void compactSelectorsKeepLongWonLabelClearOfArrow() {
        Wire wire;
        MarketCompareSource source(&wire, nullptr, [] { return T0; });
        MarketCompareView view(&source);
        view.setStyleSheet(KeefiTheme::localStyleSheet(&view));
        view.resize(1024, 700);
        view.show();
        QCoreApplication::processEvents();
        for (const auto* name : {"marketCompareBaseSelector", "marketCompareTargetSelector", "marketCompareWonSelector"}) {
            auto* selector = view.findChild<QComboBox*>(name);
            QVERIFY(selector);
            QCOMPARE(selector->height(),QString::fromLatin1(name)=="marketCompareTargetSelector"?28:24);
            QVERIFY(selector->width() <= 260);
        }
        auto* won = view.findChild<QComboBox*>("marketCompareWonSelector");
        won->setCurrentIndex(1);
        QCOMPARE(won->currentText(), QStringLiteral("비트코인: 원화(환율 효과 포함)"));
        QStyleOptionComboBox option;
        option.initFrom(won);
        option.currentText = won->currentText();
        option.frame = true;
        const auto text = won->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, won);
        const auto arrow = won->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, won);
        CompareSelectorPaintDevice device(won->size());QPainter painter(&device);won->render(&painter);painter.end();
        QVERIFY2(device.engine.text.contains(QStringLiteral("BTC 원화")),qPrintable(device.engine.text));
        const auto required = won->fontMetrics().horizontalAdvance(QStringLiteral("BTC 원화"));
        QVERIFY2(required <= text.width(),
                 qPrintable(QStringLiteral("label=%1px field=%2px box=%3px").arg(required).arg(text.width()).arg(won->width())));
        QVERIFY(text.right() < arrow.left());
        won->showPopup();
        QVERIFY(won->view()->window()->width() >= 260);
        const auto selected=won->model()->index(1,0);QCOMPARE(selected.data().toString(),QStringLiteral("비트코인: 원화(환율 효과 포함)"));
        QVERIFY(won->view()->isVisible());QVERIFY(won->view()->fontMetrics().horizontalAdvance(selected.data().toString())<=won->view()->visualRect(selected).width());
        won->hidePopup();
    }
    void checkedTimeframeHasItsOwnFillWhenMacFocusStaysOnFirstButton_data(){
        QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
    }
    void checkedTimeframeHasItsOwnFillWhenMacFocusStaysOnFirstButton(){
        QFETCH(bool,dark);
        auto* theme=KeefiTheme::instance();const auto before=theme->mode();
        const auto restore=qScopeGuard([&]{theme->setMode(before);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        Wire wire;MarketCompareSource source(&wire,nullptr,[]{return T0;});MarketCompareView view(&source);
        // LocalWindow supplies this stylesheet in the real application. Use the
        // same surface, then reproduce cocoa's TabFocus policy on offscreen Qt.
        view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.resize(1180,860);view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(openCompareIntervals(view));
        for(auto* button:view.findChildren<QPushButton*>())if(button->objectName().startsWith("marketCompareTimeframe_"))button->setFocusPolicy(Qt::TabFocus);
        auto* minute=view.findChild<QPushButton*>("marketCompareTimeframe_60000");
        auto* five=view.findChild<QPushButton*>("marketCompareTimeframe_300000");QVERIFY(minute&&five);
        minute->setFocus(Qt::TabFocusReason);QTRY_VERIFY(minute->hasFocus());
        QTest::mouseClick(five,Qt::LeftButton);QTest::mouseMove(&view,QPoint(view.width()-4,view.height()-4));
        QCoreApplication::processEvents();
        QVERIFY(minute->hasFocus());QVERIFY(!minute->isChecked());QVERIFY(five->isChecked());QCOMPARE(source.intervalMs(),5*Minute);
        const auto fill=[](QPushButton* button){const auto image=button->grab().toImage();const auto ratio=image.devicePixelRatio();
            return image.pixelColor(qRound(8*ratio),qRound(button->height()*ratio/2));};
        QCOMPARE(fill(five),themeColors().text);
        QVERIFY(fill(minute)!=themeColors().text);
    }

    void sourceOwnsTheDefaultAndEveryVisibleTimeframeSelection(){
        Wire wire;MarketCompareSource source(&wire,nullptr,[]{return T0;});
        QCOMPARE(source.intervalMs(),Hour);
        source.setInterval(4*Hour);MarketCompareView view(&source);
        QCOMPARE(source.intervalMs(),4*Hour);
        QVERIFY(openCompareIntervals(view));
        auto* four=view.findChild<QPushButton*>("marketCompareTimeframe_14400000");QVERIFY(four);QVERIFY(four->isChecked());
        QSignalSpy changes(&source,&MarketCompareSource::changed);source.setInterval(5*Minute);
        QCOMPARE(changes.size(),1);
        auto* five=view.findChild<QPushButton*>("marketCompareTimeframe_300000");QVERIFY(five);QVERIFY(five->isChecked());
        QVERIFY(!four->isChecked());
        for (auto* button : view.findChildren<QPushButton*>()) {
            if (!button->objectName().startsWith("marketCompareTimeframe_")) continue;
            QCOMPARE(button->focusPolicy(), Qt::StrongFocus);
            QCOMPARE(button->height(), 28);
        }
    }

    void timeframePreferenceUsesOnlyTheInjectedPublicSettings(){
        QTemporaryDir root;QVERIFY(root.isValid());QSettings settings(root.filePath("public.ini"),QSettings::IniFormat);
        const QString key="marketCompare/intervalMs/v1";settings.setValue(key,4*Hour);settings.setValue("unrelated/public","keep");
        {
            Wire wire;MarketCompareSource source(&wire,nullptr,[]{return T0;});MarketCompareView view(&source);
            QVERIFY2(injectPublicSettings(view,&settings),"MarketCompareView needs an injected public-settings boundary");
            QCOMPARE(source.intervalMs(),4*Hour);
            QVERIFY(openCompareIntervals(view));
            auto* four=view.findChild<QPushButton*>("marketCompareTimeframe_14400000");QVERIFY(four&&four->isChecked());
            view.resize(1180,860);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));
            auto* day=view.findChild<QPushButton*>("marketCompareTimeframe_86400000");QVERIFY(day);QTest::mouseClick(day,Qt::LeftButton);
            QCOMPARE(settings.value(key).toLongLong(),24*Hour);
        }
        Wire restoredWire;MarketCompareSource restored(&restoredWire,nullptr,[]{return T0;});MarketCompareView restoredView(&restored);
        QVERIFY(injectPublicSettings(restoredView,&settings));QCOMPARE(restored.intervalMs(),24*Hour);
        QVERIFY(openCompareIntervals(restoredView));
        QVERIFY(restoredView.findChild<QPushButton*>("marketCompareTimeframe_86400000")->isChecked());
        settings.setValue(key,123);Wire invalidWire;MarketCompareSource invalid(&invalidWire,nullptr,[]{return T0;});
        invalid.setInterval(5*Minute);MarketCompareView invalidView(&invalid);QVERIFY(injectPublicSettings(invalidView,&settings));
        QCOMPARE(invalid.intervalMs(),5*Minute);QCOMPARE(settings.value("unrelated/public").toString(),QString("keep"));
    }

    void timeframeClickSurvivesLateRepliesAndVisibilityChanges_data() {
        QTest::addColumn<qint64>("interval");
        QTest::addColumn<QString>("requestInterval");
        QTest::addColumn<bool>("hideDuringReply");
        const QList<QPair<qint64, QString>> frames{{Minute, "1m"}, {5 * Minute, "5m"}, {15 * Minute, "15m"},
                                                   {Hour, "1h"}, {4 * Hour, "4h"}, {24 * Hour, "1d"}};
        for (const auto& [interval, requestInterval] : frames)
            for (const bool hidden : {false, true})
                QTest::newRow(qPrintable(requestInterval + (hidden ? "-hide-resume" : "-visible")))
                    << interval << requestInterval << hidden;
    }

    void timeframeClickSurvivesLateRepliesAndVisibilityChanges() {
        QFETCH(qint64, interval);
        QFETCH(QString, requestInterval);
        QFETCH(bool, hideDuringReply);
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        MarketCompareSource source(&wire, nullptr, [&] { return T0 + elapsed.elapsed(); });
        MarketCompareView view(&source);
        view.resize(1180, 860);
        view.show();
        QTRY_COMPARE(wire.sent.size(), 1);
        wire.answer(0, catalog());
        QTRY_COMPARE(wire.sent.size(), 2); // Keep one original hourly response in flight.
        QCOMPARE(QUrlQuery(wire.sent[1].second.url).queryItemValue("interval"), QString("1h"));
        QVERIFY(openCompareIntervals(view));
        auto* minute = view.findChild<QPushButton*>(QStringLiteral("marketCompareTimeframe_60000"));
        auto* selected = view.findChild<QPushButton*>(QStringLiteral("marketCompareTimeframe_%1").arg(interval));
        QVERIFY(minute && selected);
        QTest::mouseClick(minute, Qt::LeftButton);
        if (selected != minute) QTest::mouseClick(selected, Qt::LeftButton);
        QVERIFY(selected->isChecked());
        QCOMPARE(source.intervalMs(), interval);
        if(hideDuringReply)QVERIFY(closeCompareIntervals(view));
        if (hideDuringReply) view.hide();
        QSignalSpy changes(&source, &MarketCompareSource::changed);
        QTimer::singleShot(0, &wire, [&] { wire.answer(1, klines("XAUUSDT", T0 - 4 * Hour, 4)); });
        QTRY_VERIFY(!changes.isEmpty());
        if (hideDuringReply) view.show();
        QVERIFY(openCompareIntervals(view));
        QCOMPARE(source.intervalMs(), interval);
        QVERIFY(selected->isChecked());
        const auto page = [=] {
            QStringList rows;
            for (int i = 0; i < 4; ++i) {
                const auto open = T0 - (4 - i) * interval;
                rows.append(QStringLiteral("[%1,\"100\",\"100\",\"100\",\"100\",\"1\",%2,\"1\",1,\"1\",\"1\",\"0\"]")
                                .arg(open).arg(open + interval - 1));
            }
            return '[' + rows.join(',').toUtf8() + ']';
        }();
        for (int i = 2; i < 6; ++i) {
            QTRY_VERIFY(wire.sent.size() > i);
            QCOMPARE(QUrlQuery(wire.sent[i].second.url).queryItemValue("interval"), requestInterval);
            const auto before = changes.size();
            QTimer::singleShot(0, &wire, [&, i] { wire.answer(i, page); });
            QTRY_VERIFY(changes.size() > before);
            QCOMPARE(source.intervalMs(), interval);
            QVERIFY(selected->isChecked());
        }
        const auto snapshot = source.snapshot();
        QCOMPARE(snapshot.intervalMs, interval);
        QCOMPARE(snapshot.series.size(), 4);
        for (const auto& series : snapshot.series) {
            QVERIFY2(series.available, qPrintable(series.reason));
            QCOMPARE(series.intervalMs, interval);
            QCOMPARE(series.bars.size(), 4);
        }
        const QMap<QString,QString> labels{{"1m","1분봉"},{"5m","5분봉"},{"15m","15분봉"},{"1h","1시간봉"},{"4h","4시간봉"},{"1d","1일봉"}};
        auto* status=view.findChild<QLabel*>("marketCompareStatus");QVERIFY(status);
        QVERIFY2(status->text().contains(labels.value(requestInterval)),qPrintable(status->text()));
        QVERIFY(!snapshot.loading);
        QVERIFY(closeCompareIntervals(view));
        view.hide();
    }

    void windowTimeframeClickSurvivesAsyncRepliesAndTabReturn_data() {
        QTest::addColumn<qint64>("interval");QTest::addColumn<QString>("requestInterval");
        QTest::newRow("five-minutes") << 5 * Minute << QString("5m");
        QTest::newRow("one-day") << 24 * Hour << QString("1d");
    }

    void windowTimeframeClickSurvivesAsyncRepliesAndTabReturn() {
        QFETCH(qint64, interval);QFETCH(QString, requestInterval);
        QTemporaryDir root;QVERIFY(root.isValid());
        QSettings profile(root.filePath("profile.ini"), QSettings::IniFormat);
        Vault vault(root.filePath("not-created-vault"));AccountModel accounts(&vault);
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        Wire publicWire;PublicMarketCatalog catalogModel(&publicWire);
        Wire wire;QElapsedTimer elapsed;elapsed.start();
        MarketCompareSource source(&wire,nullptr,[&]{return T0+elapsed.elapsed();});
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);
        window.setPublicMarketCatalog(&catalogModel,false);bool initialized=false;
        window.setDeferredWorkbenchInitializer([&]{window.setMarketCompareSource(&source);initialized=true;});
        window.showVaultGate();window.resize(1440,900);window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* browse=window.findChild<QPushButton*>("vaultGateBrowse");QVERIFY(browse&&browse->isVisible());
        QSignalSpy browseClicks(browse,&QPushButton::clicked);
        QTest::mouseClick(browse,Qt::LeftButton);
        QCOMPARE(browseClicks.size(),1);QTRY_VERIFY(initialized);
        auto* view=window.findChild<MarketCompareView*>();QVERIFY(view);
        QPushButton *compare=nullptr,*explore=nullptr;
        for(auto* button:window.findChild<QWidget*>("headerTabs")->findChildren<QPushButton*>()){
            if(button->text()==QStringLiteral("시장 비교"))compare=button;
            if(button->text()==QStringLiteral("탐색"))explore=button;
        }
        QVERIFY(compare&&explore);QTest::mouseClick(compare,Qt::LeftButton);QTRY_VERIFY(view->isVisible());
        QTRY_COMPARE(wire.sent.size(),1);wire.answer(0,catalog());QTRY_COMPARE(wire.sent.size(),2);
        QVERIFY(openCompareIntervals(*view));
        auto* selected=view->findChild<QPushButton*>(QStringLiteral("marketCompareTimeframe_%1").arg(interval));
        QVERIFY(selected&&selected->isVisible());QTest::mouseClick(selected,Qt::LeftButton);
        QCOMPARE(source.intervalMs(),interval);QVERIFY(selected->isChecked());
        QVERIFY(closeCompareIntervals(*view));
        QTest::mouseClick(explore,Qt::LeftButton);QTRY_VERIFY(!view->isVisible());
        QSignalSpy changes(&source,&MarketCompareSource::changed);
        QTimer::singleShot(0,&wire,[&]{wire.answer(1,klines("XAUUSDT",T0-4*Hour,4));});
        QTRY_VERIFY(!changes.isEmpty());
        QTest::mouseClick(compare,Qt::LeftButton);QTRY_VERIFY(view->isVisible());
        QVERIFY(openCompareIntervals(*view));
        QCOMPARE(source.intervalMs(),interval);QVERIFY(selected->isChecked());
        QStringList rows;
        for(int i=0;i<4;++i){const auto open=T0-(4-i)*interval;
            rows.append(QStringLiteral("[%1,\"100\",\"100\",\"100\",\"100\",\"1\",%2,\"1\",1,\"1\",\"1\",\"0\"]").arg(open).arg(open+interval-1));}
        const auto page='['+rows.join(',').toUtf8()+']';
        for(int i=2;i<6;++i){
            QTRY_VERIFY(wire.sent.size()>i);
            QCOMPARE(QUrlQuery(wire.sent[i].second.url).queryItemValue("interval"),requestInterval);
            const auto before=changes.size();QTimer::singleShot(0,&wire,[&,i]{wire.answer(i,page);});
            QTRY_VERIFY(changes.size()>before);QCOMPARE(source.intervalMs(),interval);QVERIFY(selected->isChecked());
        }
        QTRY_VERIFY(!source.snapshot().loading);
        for(const auto& series:source.snapshot().series){QVERIFY(series.available);QCOMPARE(series.intervalMs,interval);}
        QVERIFY(closeCompareIntervals(*view));
        QVERIFY(!vault.exists());window.close();
    }

    void missingBarLeavesAGapInTheRenderedComparisonLine() {
        auto* theme = KeefiTheme::instance();
        const auto oldMode = theme->mode();
        const auto restoreTheme = qScopeGuard([&] { theme->setMode(oldMode); });
        theme->setMode(ThemeMode::Light);
        const QColor gold = KeefiTheme::colorsFor(false).studyOrange;
        const auto isGold = [&](const QColor& pixel) {
            return std::abs(pixel.red() - gold.red()) <= 8 && std::abs(pixel.green() - gold.green()) <= 8 &&
                   std::abs(pixel.blue() - gold.blue()) <= 8;
        };
        const auto page = [](bool goldLine, bool missingMiddle) {
            QStringList rows;
            for (int i = 0; i < 7; ++i) {
                if (goldLine && missingMiddle && i == 3) continue;
                const auto open = T0 + i * Hour;
                const int close = goldLine ? 100 + 10 * i : 100;
                rows.append(QStringLiteral("[%1,\"%2\",\"%2\",\"%2\",\"%2\",\"1\",%3,\"1\",1,\"1\",\"1\",\"0\"]")
                                .arg(open).arg(close).arg(open + Hour - 1));
            }
            return '[' + rows.join(',').toUtf8() + ']';
        };
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        MarketCompareSource source(&wire, nullptr, [&] { return T0 + 8 * Hour + elapsed.elapsed(); });
        MarketCompareView view(&source);
        view.resize(1180, 860);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const bool goldLine = QUrlQuery(wire.sent[i].second.url).queryItemValue("symbol") == QLatin1String("XAUUSDT");
            wire.answer(i, page(goldLine, false));
        }
        QWidget* chart = nullptr;
        for (auto* child : view.findChildren<QWidget*>())
            if (child->accessibleName() == QStringLiteral("시장 비교 차트")) chart = child;
        QVERIFY(chart);
        const QImage continuous = chart->grab().toImage();
        QVERIFY(!continuous.isNull());
        int left = continuous.width(), right = -1;
        for (int y = 0; y < continuous.height(); ++y)
            for (int x = 0; x < continuous.width(); ++x)
                if (isGold(continuous.pixelColor(x, y))) { left = qMin(left, x); right = qMax(right, x); }
        QVERIFY2(right - left > 100, "The continuous control must contain a visible gold price line");
        // Locate the middle from the actual rendered line, without coupling to chart margins or DPI.
        const int bandLeft = left + (right - left) * 45 / 100;
        const int bandRight = left + (right - left) * 55 / 100;
        const auto goldPixels = [&](const QImage& image, int from, int to) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = from; x <= to; ++x) count += isGold(image.pixelColor(x, y));
            return count;
        };
        QVERIFY2(goldPixels(continuous, bandLeft, bandRight) > 0, "The continuous control must cross the middle bar");
        source.refresh();
        for (int i = 5; i <= 8; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const bool goldLine = QUrlQuery(wire.sent[i].second.url).queryItemValue("symbol") == QLatin1String("XAUUSDT");
            wire.answer(i, page(goldLine, true));
        }
        QCOMPARE(source.snapshot().series.first().bars.size(), 6);
        const QImage gapped = chart->grab().toImage();
        QCOMPARE(gapped.size(), continuous.size());
        QVERIFY2(goldPixels(gapped, left, bandLeft - 1) > 0, "The observed segment before the gap must remain visible");
        QVERIFY2(goldPixels(gapped, bandRight + 1, right) > 0, "The observed segment after the gap must remain visible");
        QCOMPARE(goldPixels(gapped, bandLeft, bandRight), 0);
    }

    void collectorStatusExplainsFreshStaleAndMissing_data() {
        QTest::addColumn<qint64>("age");
        QTest::addColumn<QString>("message");
        QTest::addColumn<int>("mode");
        for (const auto mode : {CompareWonMode::Dollar, CompareWonMode::WonToDollar}) {
            const QByteArray prefix = mode == CompareWonMode::Dollar ? "dollar-" : "converted-";
            QTest::newRow((prefix + "fresh-30-seconds").constData()) << qint64(30000) << QStringLiteral("수집기: 마지막 기록 30초 전") << int(mode);
            QTest::newRow((prefix + "stale-10-minutes").constData()) << qint64(10 * Minute) << QStringLiteral("수집기 마지막 기록 10분 전 · 멈춘 것 같아요") << int(mode);
            QTest::newRow((prefix + "missing").constData()) << qint64(-1) << QStringLiteral("수집기 기록 없음") << int(mode);
        }
    }

    void collectorStatusExplainsFreshStaleAndMissing() {
        QFETCH(qint64, age);
        QFETCH(QString, message);
        QFETCH(int, mode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto root = dir.path() + QStringLiteral("/v1");
        if (age >= 0) {
            CollectorStoreOptions options;
            options.root = root;
            options.collectorVersion = options.algorithmVersion = QStringLiteral("test");
            CollectorStore store(options, [] { return T0; }, [] { return 500LL * 1024 * 1024 * 1024; });
            QVERIFY(store.open());
            QVERIFY(store.writeStatus({{"writtenAt", T0 - age}}));
        }
        Wire wire;
        MarketCompareSource source(&wire, nullptr, [] { return T0; });
        source.setCollectorRoot(root);
        source.setWonMode(CompareWonMode(mode));
        MarketCompareView view(&source);
        view.show();
        const auto notes=presentedCompareNotes(view);QVERIFY2(notes.contains(message), qPrintable(notes));
        if (age < 0 || age > 3 * Minute) QVERIFY(notes.contains(QStringLiteral("ECB 근사")));
    }

    void failedRefreshShowsPreviousBarsAndLastSuccessfulTime() {
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 now = T0;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        MarketCompareView view(&source);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            wire.answer(i, klines(QUrlQuery(wire.sent[i].second.url).queryItemValue("symbol"), T0 - 4 * Hour, 4));
        }
        const auto before = source.snapshot();
        const auto base = view.baseOpenMs();
        now += 5 * Minute;
        source.refresh();
        for (int i = 5; i <= 8; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            wire.answer(i, QByteArray(R"({"msg":"unavailable"})"), 503);
        }
        const auto legend = presentedCompareLegend(view);
        QCOMPARE(legend.count(QStringLiteral("갱신 실패:")), 4);
        QCOMPARE(legend.count(QStringLiteral("마지막 정상 수신")), 4);
        QCOMPARE(view.baseOpenMs(), base);
        const auto after = source.snapshot();
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(after.series[i].lastOkAtMs, before.series[i].lastOkAtMs);
            QCOMPARE(after.series[i].bars.size(), before.series[i].bars.size());
            QCOMPARE(after.series[i].bars.last().close, before.series[i].bars.last().close);
            const auto at = QDateTime::fromMSecsSinceEpoch(before.series[i].lastOkAtMs, QTimeZone("Asia/Seoul"));
            QVERIFY(legend.contains(at.toString("MM.dd HH:mm")));
        }
    }

    void convertedBarsExplainTheirFxAndStayProvisionalUntilSealed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CollectorStoreOptions options;
        options.root = dir.path() + QStringLiteral("/v1");
        options.collectorVersion = options.algorithmVersion = QStringLiteral("test");
        qint64 now = T0 + 2 * Minute;
        CollectorStore store(options, [&] { return now; }, [] { return 500LL * 1024 * 1024 * 1024; });
        QVERIFY(store.open());
        const auto ecbDate = QDate(2026, 11, 5);
        const auto ecbKnown = FxHistory::ecbKnownAtMs(ecbDate);
        QVERIFY(store.append("fx", "ecb", "USDKRW", ecbKnown, ecbKnown,
                             jsonLine({{"x", "1340.00"}, {"d", ecbDate.toString(Qt::ISODate)}, {"p", ecbKnown - Hour}, {"k", ecbKnown}, {"r", ecbKnown}})));
        const auto oldKnown = T0 - 2 * Hour + 10000;
        QVERIFY(store.append("fx", "naver-hana", "USDKRW", oldKnown, oldKnown,
                             jsonLine({{"x", "1350.00"}, {"p", T0 - 2 * Hour}, {"d", "2026-11-05"}, {"r", oldKnown}})));
        const auto newKnown = T0 - Hour + 10000;
        QVERIFY(store.append("fx", "naver-hana", "USDKRW", newKnown, newKnown,
                             jsonLine({{"x", "1400.00"}, {"p", T0 - Hour}, {"d", "2026-11-05"}, {"r", newKnown}})));
        store.flush();
        QCOMPARE(store.sealDue(), 2); // The latest Hana hour remains .part until +5 minutes.
        QElapsedTimer elapsed;
        elapsed.start();
        Wire wire;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setCollectorRoot(options.root);
        source.setWonMode(CompareWonMode::WonToDollar);
        MarketCompareView view(&source);
        view.resize(1180, 860);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        for (int i = 1; i <= 4; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const auto& url = wire.sent[i].second.url;
            wire.answer(i, url.host() == QLatin1String("api.upbit.com") ? upbitKlines(T0 - 3 * Hour, 3)
                                                                      : klines(QUrlQuery(url).queryItemValue("symbol"), T0 - 3 * Hour, 3));
        }
        const auto before = source.snapshot();
        QCOMPARE(before.hana.size(), 1); // The application must not inspect the open file.
        QCOMPARE(before.hanaConfirmedThroughMs, T0 - Hour);
        QCOMPARE(before.provisionalFxOpenMs, QSet<qint64>{T0 - Hour});
        const auto bitcoin = before.series.last();
        QCOMPARE(bitcoin.bars.size(), 3);
        QCOMPARE(bitcoin.fx.size(), 3);
        QCOMPARE(bitcoin.fx[0].source, QStringLiteral("ecb"));
        QVERIFY(bitcoin.fx[0].approximate);
        QVERIFY(bitcoin.fx[0].label.contains(QStringLiteral("근사")));
        for (int i = 1; i < 3; ++i) {
            QCOMPARE(bitcoin.fx[i].source, QStringLiteral("hana"));
            QVERIFY(!bitcoin.fx[i].approximate);
            QCOMPARE(bitcoin.fx[i].postedAtMs, T0 - 2 * Hour);
            QCOMPARE(bitcoin.bars[i].close, 100000.0);
        }
        const auto legend = presentedCompareLegend(view);
        QVERIFY2(legend.contains(bitcoin.fx[0].label + QStringLiteral(" 1봉")), qPrintable(legend));
        QVERIFY2(legend.contains(bitcoin.fx[1].label + QStringLiteral(" 2봉")), qPrintable(legend));
        QVERIFY2(legend.contains(QStringLiteral("잠정 1봉")), qPrintable(legend));
        QVERIFY(presentedCompareNotes(view).contains(QStringLiteral("확정 뒤 환율이 바뀔 수 있어요")));
        QWidget* chart = nullptr;
        for (auto* child : view.findChildren<QWidget*>())
            if (child->accessibleName() == QStringLiteral("시장 비교 차트")) chart = child;
        QVERIFY(chart);
        QTest::mouseMove(chart, QPoint(chart->width() - 15, chart->height() / 2));
        QTRY_VERIFY(chart->accessibleDescription().contains(QStringLiteral("잠정")));
        QVERIFY(chart->accessibleDescription().contains(QStringLiteral("1350.00")));
        // Dev 96: the crosshair names the weekday and time of the bar under the pointer.
        const QRegularExpression hoverTime(QStringLiteral("\\A\\d\\d\\.\\d\\d \\([월화수목금토일]\\) \\d\\d:\\d\\d"));
        QVERIFY2(hoverTime.match(chart->accessibleDescription()).hasMatch(), qPrintable(chart->accessibleDescription()));

        now = T0 + 7 * Minute;
        QCOMPARE(store.sealDue(), 1);
        source.refresh();
        for (int i = 5; i <= 8; ++i) {
            QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000));
            const auto& url = wire.sent[i].second.url;
            wire.answer(i, url.host() == QLatin1String("api.upbit.com") ? upbitKlines(T0 - 3 * Hour, 3)
                                                                      : klines(QUrlQuery(url).queryItemValue("symbol"), T0 - 3 * Hour, 3));
        }
        const auto sealed = source.snapshot();
        QCOMPARE(sealed.hanaConfirmedThroughMs, T0);
        QVERIFY(sealed.provisionalFxOpenMs.isEmpty());
        const auto after = sealed.series.last();
        QCOMPARE(after.bars[0].close, bitcoin.bars[0].close);
        QCOMPARE(after.bars[1].close, bitcoin.bars[1].close);
        QCOMPARE(after.fx.last().postedAtMs, T0 - Hour);
        QCOMPARE(after.bars.last().close, 135000000.0 / 1400.0);
        QVERIFY2(!presentedCompareLegend(view).contains(QStringLiteral("잠정")), qPrintable(presentedCompareLegend(view)));
        // The refresh above replaced the chart data while the pointer rested on it: the crosshair stays.
        QTRY_VERIFY2(hoverTime.match(chart->accessibleDescription()).hasMatch(), qPrintable(chart->accessibleDescription()));
    }

    void relationMarkersStayAtTheirTimeWhenPageLosesWarmup() {
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 now = T0 + 80 * Hour;
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        MarketCompareView view(&source);
        source.setInterval(15 * Minute);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        QVERIFY(answerRelationPage(wire, 1, T0, 0, 200));
        const auto marker = T0 + 137 * 15 * Minute;
        QVERIFY(view.relationChangeOpens().contains(marker));
        now += Minute;
        source.refresh();
        QVERIFY(answerRelationPage(wire, 5, T0, 73, 200));
        const auto snapshot = source.snapshot();
        const auto recalculated = MarketCompare::relationChanges(MarketCompare::rollingCorrelation(snapshot.series.last(), snapshot.series.first()));
        for (const auto& change : recalculated) QVERIFY(change.openMs != marker);
        QVERIFY(view.relationChangeOpens().contains(marker));
        QCOMPARE(view.relationChangeOpens().count(marker), 1);
    }

    void relationMarkerCacheSeparatesRawWonFromConvertedDollar() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CollectorStoreOptions options;
        options.root = dir.path() + QStringLiteral("/v1");
        options.collectorVersion = options.algorithmVersion = QStringLiteral("test");
        qint64 now = T0 + 80 * Hour;
        const auto base = relationPrices(false), target = relationPrices(true);
        {
            CollectorStore store(options, [&] { return now; }, [] { return 500LL * 1024 * 1024 * 1024; });
            QVERIFY(store.open());
            for (int i = 0; i < 200; ++i) {
                const auto known = T0 + i * 15 * Minute + 1000;
                QVERIFY(store.append("fx", "naver-hana", "USDKRW", known, known,
                                     jsonLine({{"x", QString::number(1350 * target[i] / base[i], 'g', 16)},
                                               {"p", known - 1000}, {"r", known}, {"d", "2026-11-06"}})));
            }
            const auto date = QDate(2026, 11, 5);
            const auto known = FxHistory::ecbKnownAtMs(date);
            QVERIFY(store.append("fx", "ecb", "USDKRW", known, known,
                                 jsonLine({{"x", "1350.00"}, {"d", date.toString(Qt::ISODate)}, {"p", known - Hour}, {"k", known}, {"r", known}})));
            QCOMPARE(store.sealDue(), 51);
        }
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setCollectorRoot(options.root);
        source.setWonMode(CompareWonMode::WonToDollar);
        MarketCompareView view(&source);
        source.setInterval(15 * Minute);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        QVERIFY(answerRelationPage(wire, 1, T0, 0, 200));
        QVERIFY(view.relationChangeOpens().isEmpty());
        source.setWonMode(CompareWonMode::WonRaw);
        QVERIFY(answerRelationPage(wire, 5, T0, 0, 200));
        const auto marker = T0 + 137 * 15 * Minute;
        QVERIFY(view.relationChangeOpens().contains(marker));
        source.setWonMode(CompareWonMode::WonToDollar);
        QVERIFY(answerRelationPage(wire, 9, T0, 0, 200));
        const auto snapshot = source.snapshot();
        QVERIFY(MarketCompare::relationChanges(MarketCompare::rollingCorrelation(snapshot.series.last(), snapshot.series.first())).isEmpty());
        QVERIFY2(view.relationChangeOpens().isEmpty(), "Raw KRW markers must not be reused for the USD-converted series");
    }

    void provisionalRelationMarkerIsNotRememberedAfterFxSeals() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CollectorStoreOptions options;
        options.root = dir.path() + QStringLiteral("/v1");
        options.collectorVersion = options.algorithmVersion = QStringLiteral("test");
        qint64 now = T0 + 34 * Hour + 32 * Minute;
        const auto base = relationPrices(false), target = relationPrices(true);
        CollectorStore store(options, [&] { return now; }, [] { return 500LL * 1024 * 1024 * 1024; });
        QVERIFY(store.open());
        for (int i = 0; i < 138; ++i) {
            const auto known = T0 + i * 15 * Minute + 1000;
            // Only the open hour carries the new rates; the application cannot see them yet.
            const double rate = i < 136 ? 1350 : 1350 * target[i] / base[i];
            QVERIFY(store.append("fx", "naver-hana", "USDKRW", known, known,
                                 jsonLine({{"x", QString::number(rate, 'g', 16)}, {"p", known - 1000}, {"r", known}, {"d", "2026-11-06"}})));
        }
        const auto date = QDate(2026, 11, 5);
        const auto known = FxHistory::ecbKnownAtMs(date);
        QVERIFY(store.append("fx", "ecb", "USDKRW", known, known,
                             jsonLine({{"x", "1350.00"}, {"d", date.toString(Qt::ISODate)}, {"p", known - Hour}, {"k", known}, {"r", known}})));
        store.flush();
        QCOMPARE(store.sealDue(), 35);
        Wire wire;
        QElapsedTimer elapsed;
        elapsed.start();
        MarketCompareSource source(&wire, nullptr, [&] { return now + elapsed.elapsed(); });
        source.setCollectorRoot(options.root);
        source.setWonMode(CompareWonMode::WonToDollar);
        MarketCompareView view(&source);
        source.setInterval(15 * Minute);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() == 1; }, 2000));
        wire.answer(0, catalog());
        QVERIFY(answerRelationPage(wire, 1, T0, 0, 138));
        const auto marker = T0 + 137 * 15 * Minute;
        QVERIFY(source.snapshot().provisionalFxOpenMs.contains(marker));
        QVERIFY(view.relationChangeOpens().contains(marker));
        now = T0 + 35 * Hour + 7 * Minute;
        QCOMPARE(store.sealDue(), 1);
        source.refresh();
        QVERIFY(answerRelationPage(wire, 5, T0, 0, 138));
        const auto snapshot = source.snapshot();
        QVERIFY(snapshot.provisionalFxOpenMs.isEmpty());
        const auto calculated = MarketCompare::relationChanges(MarketCompare::rollingCorrelation(snapshot.series.last(), snapshot.series.first()));
        for (const auto& change : calculated) QVERIFY(change.openMs != marker);
        QVERIFY2(!view.relationChangeOpens().contains(marker), "A marker based on provisional FX must disappear when sealed FX no longer supports it");
    }

    void shortComparisonWindowKeepsBothChartsAndNotesSeparate() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings profile(temp.filePath("synthetic-profile.ini"), QSettings::IniFormat);
        Vault vault(temp.filePath("not-created-vault"));
        AccountModel accounts(&vault);
        PortfolioController::Factories factories;
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        Wire wire;
        MarketCompareSource source(&wire);
        LocalWindow window(vault, accounts);
        window.setPortfolioController(&portfolio);
        window.setChartSettings(&profile);
        window.setMarketCompareSource(&source);
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        auto* view = window.findChild<MarketCompareView*>();
        QVERIFY(pages && view);
        pages->setCurrentWidget(view);
        window.resize(1024, 700);
        window.show();

        QTRY_VERIFY_WITH_TIMEOUT(!wire.sent.isEmpty(), 3000);
        wire.answer(0, catalog());
        for (int index = 1; index <= 4; ++index) {
            QTRY_VERIFY_WITH_TIMEOUT(wire.sent.size() > index, 3000);
            const auto symbol = QUrlQuery(wire.sent[index].second.url).queryItemValue("symbol");
            wire.answer(index, klines(symbol, T0, 120));
        }
        QTRY_VERIFY_WITH_TIMEOUT(presentedCompareLegend(*view).contains(QStringLiteral("60봉")), 3000);
        QCOMPARE(window.size(), QSize(1024, 700));
        QWidget* priceChart = nullptr;
        QWidget* correlation = nullptr;
        for (auto* widget : view->findChildren<QWidget*>()) {
            if (widget->accessibleName() == QStringLiteral("시장 비교 차트")) priceChart = widget;
            if (widget->accessibleName() == QStringLiteral("연동 정도")) correlation = widget;
        }
        auto* notes = view->findChild<QLabel*>("marketCompareCollectorStatus");
        auto* status = view->findChild<QLabel*>("marketCompareStatus");
        QVERIFY(priceChart && correlation && notes && status);
        auto* toggle=view->findChild<QPushButton*>("marketCompareCorrelationToggle");auto* target=view->findChild<QComboBox*>("marketCompareTargetSelector");QVERIFY(toggle&&target);
        QVERIFY(correlation->isHidden());QVERIFY(!target->isVisibleTo(view));QTest::mouseClick(toggle,Qt::LeftButton);QTRY_VERIFY(correlation->isVisible());QVERIFY(target->isVisible());
        const auto bounds = [&](QWidget* widget) { return QRect(widget->mapTo(view, QPoint{}), widget->size()); };
        QCoreApplication::processEvents();
        const auto priceBounds = bounds(priceChart);
        const auto correlationBounds = bounds(correlation);
        const auto notesBounds = bounds(notes);
        QVERIFY2(priceBounds.bottom() < correlationBounds.top(),
                 qPrintable(QString("price bottom %1, correlation top %2").arg(priceBounds.bottom()).arg(correlationBounds.top())));
        QVERIFY2(correlationBounds.bottom() < notesBounds.top(),
                 qPrintable(QString("correlation bottom %1, notes top %2").arg(correlationBounds.bottom()).arg(notesBounds.top())));
        QVERIFY(notesBounds.height() >= notes->heightForWidth(notesBounds.width()));
        QVERIFY(notesBounds.bottom() < bounds(status).top());
        QVERIFY(presentedCompareInformation(*view).contains(QStringLiteral("무기한 계약")));

        auto* scroll = view->findChild<QScrollArea*>("marketCompareScroll");
        QVERIFY(scroll);
        scroll->ensureWidgetVisible(status, 0, 0);
        QVERIFY(scroll->viewport()->rect().contains(QRect(status->mapTo(scroll->viewport(), QPoint{}), status->size())));
    }

    void fixtureRendersFourLinesAndCorrelation() {
        Wire wire;
        MarketCompareSource source(&wire);
        MarketCompareView view(&source);
        view.resize(1180, 860);
        view.show();
        QVERIFY(QTest::qWaitFor([&] { return wire.sent.size() >= 1; }, 2000));
        QCOMPARE(wire.sent[0].second.url.toString(), QStringLiteral("https://fapi.binance.com/fapi/v1/exchangeInfo"));
        wire.answer(0, catalog());
        const auto start = QDateTime(QDate(2026, 11, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        for (int i = 1; i <= 4; ++i) {
            QVERIFY2(QTest::qWaitFor([&] { return wire.sent.size() > i; }, 3000), "Klines are requested one at a time");
            const auto symbol = QUrlQuery(wire.sent[i].second.url).queryItemValue("symbol");
            QCOMPARE(QUrlQuery(wire.sent[i].second.url).queryItemValue("interval"), QStringLiteral("1h"));
            wire.answer(i, klines(symbol, start, 120));
        }
        QTRY_VERIFY_WITH_TIMEOUT(presentedCompareLegend(view).contains(QStringLiteral("60봉")), 3000);
        const auto legend = presentedCompareLegend(view);
        for (const auto* name : {"금", "WTI 원유", "나스닥 100", "비트코인"}) QVERIFY2(legend.contains(QString::fromUtf8(name)), qPrintable(legend));
        QVERIFY(legend.contains(QStringLiteral("기준")));
        QVERIFY2(legend.contains(QStringLiteral("반대로 움직임")), qPrintable(legend));
        QVERIFY(presentedCompareNotes(view).contains(QStringLiteral("무기한 계약")));
        QCOMPARE(view.baseOpenMs(), start);
        if (const auto dir = qEnvironmentVariable("KEEFI_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            QVERIFY(view.grab().save(dir + QStringLiteral("/market-compare-fixture.png")));
        }
    }

    void liveScreenshotsWhenAsked() {
        const auto dir = qEnvironmentVariable("KEEFI_LIVE_SCREENSHOT_DIR");
        if (dir.isEmpty()) QSKIP("Set KEEFI_LIVE_SCREENSHOT_DIR to render live public data.");
        QDir().mkpath(dir);
        NetworkTradingTransport transport;
        MarketCompareSource source(&transport);
        MarketCompareView view(&source);
        view.resize(1180, 860);
        view.show();
        const auto ready = [&] {
            const auto snapshot = source.snapshot();
            int available = 0;
            for (const auto& series : snapshot.series) available += series.available ? 1 : 0;
            return available == 4 && !snapshot.loading;
        };
        const auto describe = [&] {
            const auto snapshot = source.snapshot();
            QStringList rows{QStringLiteral("loading=%1 catalog=%2").arg(snapshot.loading).arg(snapshot.catalogReason)};
            for (const auto& series : snapshot.series)
                rows.append(QStringLiteral("%1 available=%2 bars=%3 reason=%4").arg(series.spec.symbol).arg(series.available).arg(series.bars.size()).arg(series.reason));
            return rows.join(QStringLiteral(" | "));
        };
        if (!QTest::qWaitFor(ready, 45000)) QFAIL(qPrintable(QStringLiteral("Live klines did not arrive: ") + describe()));
        QTest::qWait(300);
        QVERIFY(view.grab().save(dir + QStringLiteral("/market-compare-live-1h.png")));
        QVERIFY(openCompareIntervals(view));
        for (auto* button : view.findChildren<QPushButton*>())
            if (button->objectName() == QStringLiteral("marketCompareTimeframe_86400000")) button->click();
        QVERIFY(closeCompareIntervals(view));
        QTest::qWait(500);
        QVERIFY2(QTest::qWaitFor(ready, 45000), "Live daily klines did not arrive");
        QTest::qWait(300);
        QVERIFY(view.grab().save(dir + QStringLiteral("/market-compare-live-1d.png")));
        source.setWonMode(CompareWonMode::WonToDollar);
        QVERIFY2(QTest::qWaitFor([&] { const auto s = source.snapshot(); return ready() && !s.ecb.isEmpty(); }, 60000), "ECB history did not arrive");
        QTest::qWait(300);
        QVERIFY(view.grab().save(dir + QStringLiteral("/market-compare-live-1d-won.png")));
        qInfo().noquote() << presentedCompareLegend(view);
        qInfo().noquote() << presentedCompareNotes(view);
    }
};

QTEST_MAIN(MarketCompareViewTests)
#include "tst_market_compare_view.moc"
