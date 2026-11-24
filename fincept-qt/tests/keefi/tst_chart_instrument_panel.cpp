#include "keefi/ChartWorkspace.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/InfoButton.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"

#include <QAbstractButton>
#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPaintEngine>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScopeGuard>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QToolButton>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

using namespace keefi;

namespace {
struct PanelPaintedText {
    QString text;
    QFont font;
    QPointF at;
    qreal width;
    QColor ink;
};
class PanelTextEngine final : public QPaintEngine {
public:
    PanelTextEngine() : QPaintEngine(QPaintEngine::AllFeatures) {}
    QList<PanelPaintedText> texts;
    bool begin(QPaintDevice*) override { setActive(true); return true; }
    bool end() override { setActive(false); return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState& state) override { ink_ = state.pen().color(); }
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawPath(const QPainterPath&) override {}
    void drawTextItem(const QPointF& at, const QTextItem& text) override {
        texts.append({text.text(), text.font(), at, text.width(), ink_});
    }
private:
    QColor ink_;
};
class PanelTextDevice final : public QPaintDevice {
public:
    mutable PanelTextEngine engine;
    QPaintEngine* paintEngine() const override { return &engine; }
protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth: return 1440;
        case PdmHeight: return 900;
        case PdmWidthMM: return 381;
        case PdmHeightMM: return 238;
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDepth: return 32;
        case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return QPaintDevice::metric(metric);
        }
    }
};
QList<PanelPaintedText> panelPaintedCell(QTreeWidget* tree, QTreeWidgetItem* item, int column) {
    const auto index = tree->indexFromItem(item, column);
    QStyleOptionViewItem option;
    option.initFrom(tree);
    option.widget = tree;
    option.rect = tree->visualRect(index);
    option.state = QStyle::State_Enabled;
    PanelTextDevice device;
    QPainter painter(&device);
    tree->itemDelegateForColumn(column) ?
        tree->itemDelegateForColumn(column)->paint(&painter, option, index) :
        tree->itemDelegate()->paint(&painter, option, index);
    painter.end();
    return device.engine.texts;
}
qreal panelTextCenter(const PanelPaintedText& text) {
    const QFontMetricsF metrics(text.font);
    return text.at.y() + (metrics.descent() - metrics.ascent()) / 2;
}
class PanelTransport final : public TradingTransport {
public:
    QList<quint64> tokens;
    QList<RequestSpec> requests;
    void send(quint64 token, const RequestSpec& request) override {
        tokens.append(token);
        requests.append(request);
    }
    void abort() override {}
    void reply(int index, const QByteArray& body) {
        HttpResult result;
        result.status = 200;
        result.body = body;
        emit finished(tokens.at(index), result);
    }
};
bool seedPanelMarkets(PanelTransport& http, PublicMarketCatalog& catalog,
                      const QString& bitcoinName = QStringLiteral("비트코인")) {
    catalog.refresh();
    if (http.tokens.size() != 1) return false;
    const QJsonArray markets{
        QJsonObject{{"market", "KRW-BTC"}, {"korean_name", bitcoinName}, {"english_name", "Bitcoin"}},
        QJsonObject{{"market", "KRW-ETH"}, {"korean_name", "이더리움"}, {"english_name", "Ethereum"}},
        QJsonObject{{"market", "KRW-SOL"}, {"korean_name", "솔라나"}, {"english_name", "Solana"}}
    };
    http.reply(0, QJsonDocument(markets).toJson(QJsonDocument::Compact));
    if (!QTest::qWaitFor([&] { return http.tokens.size() >= 2; }, 3000)) return false;
    QJsonArray quotes;
    const auto now = QString::number(QDateTime::currentMSecsSinceEpoch() - 100);
    for (const auto& row : QList<QStringList>{{"KRW-BTC", "123456", "0.0234", "3000000", "24"},
                                             {"KRW-ETH", "200000", "-0.03", "1000000", "5"},
                                             {"KRW-SOL", "42", "0.08", "2000000", "47619"}}) {
        quotes.append(QJsonObject{{"market", row[0]}, {"trade_price", row[1]}, {"prev_closing_price", row[1]},
                                  {"signed_change_rate", row[2]}, {"timestamp", now},
                                  {"acc_trade_price_24h", row[3]}, {"acc_trade_volume_24h", row[4]}});
    }
    http.reply(1, QJsonDocument(quotes).toJson(QJsonDocument::Compact));
    return catalog.rows().size() == 3;
}
QTreeWidgetItem* panelRow(QTreeWidget* rows, const QString& key) {
    for (QTreeWidgetItemIterator it(rows); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == key) return *it;
    return nullptr;
}
QStringList panelVisibleKeys(QTreeWidget* rows) {
    QStringList keys;
    for (QTreeWidgetItemIterator it(rows); *it; ++it) {
        auto* row = *it;
        const auto key = row->data(0, Qt::UserRole).toString();
        if (row->isHidden() || key.isEmpty()) continue;
        bool visible = true;
        for (auto* parent = row->parent(); parent; parent = parent->parent())
            visible = visible && !parent->isHidden() && parent->isExpanded();
        if (visible) keys.append(key);
    }
    return keys;
}
int panelColumn(QTreeWidget* rows, const QString& heading) {
    for (int column = 0; column < rows->columnCount(); ++column)
        if (rows->headerItem()->text(column).startsWith(heading)) return column;
    return -1;
}
bool clickPanelHeading(QTreeWidget* rows, const QString& heading) {
    const auto column = panelColumn(rows, heading);
    if (column < 0 || rows->isColumnHidden(column)) return false;
    auto* header = rows->header();
    const QPoint point(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                       header->viewport()->height() / 2);
    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    return true;
}
QAbstractButton* panelTextButton(QWidget* panel, const QString& text) {
    for (auto* button : panel->findChildren<QAbstractButton*>())
        if (button->isVisible() && button->text() == text) return button;
    return nullptr;
}
QString paintedPanelText(QWidget* panel) {
    QStringList text;
    for (auto* label : panel->findChildren<QLabel*>()) if (label->isVisible()) text.append(label->text());
    for (auto* button : panel->findChildren<QAbstractButton*>()) if (button->isVisible()) text.append(button->text());
    for (auto* rows : panel->findChildren<QTreeWidget*>()) {
        if (!rows->isVisible()) continue;
        for (QTreeWidgetItemIterator it(rows); *it; ++it) {
            if ((*it)->isHidden() || rows->visualItemRect(*it).isEmpty()) continue;
            for (int column = 0; column < rows->columnCount(); ++column) {
                if (rows->isColumnHidden(column)) continue;
                for (const auto& run : panelPaintedCell(rows, *it, column)) text.append(run.text);
            }
        }
    }
    return text.join('\n');
}
}

class ChartInstrumentPanelTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase();
    void keyboardSearchAndDismissal();
    void closingPanelDropsOnlyVisibleListDemand();
    void nestedMenuAndOutsideClick();
    void holdingsOpenChartThroughSearch();
    void splitStockCanBeUnstarredWithoutOpeningAChart();
    void dev98RowsUseSingleLineGeometry_data();
    void dev98RowsUseSingleLineGeometry();
    void dev98StatusBadgesRemainVisible_data();
    void dev98StatusBadgesRemainVisible();
    void dev98RetainedQuoteStatesRemainVisible_data();
    void dev98RetainedQuoteStatesRemainVisible();
    void dev98HoldingCoverageLivesInInformation_data();
    void dev98HoldingCoverageLivesInInformation();
    void dev98LongNamesStayBeforeNumericColumns();
    void dev98PublicReferenceIsTooltipOnly();
    void dev98HeaderSortPreservesSavedOrderAndRequests();
    void dev98HeaderSortIsKeyboardReachable();
    void dev98HeaderReplacesFooterControls();
    void dev98MenuKeepsOnlyDesignedActions();
    void dev98CsvFailuresStayVisible_data();
    void dev98CsvFailuresStayVisible();
    void dev98TurnoverFitsCompactColumnWithoutRepeatedCurrency();
    void dev98RowContextRemoveUsesDangerColor_data();
    void dev98RowContextRemoveUsesDangerColor();
    void dev98WatchInteractionHintsFollowSortMode();
};

void ChartInstrumentPanelTest::initTestCase() {
    QVERIFY(settings_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
}

void ChartInstrumentPanelTest::keyboardSearchAndDismissal() {
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("합성 종목");
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-ETH"}));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    workspace.activateWindow();
    QTRY_VERIFY(workspace.isActiveWindow());

    auto* button = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* input = workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(button && panel && input && rows);
    QVERIFY(!panel->isVisible());
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible() && input->hasFocus());
    QCOMPARE(input->height(), 32);
    workspace.findChild<QAbstractButton*>("chartBrowserScope_watch")->click();

    QSignalSpy selected(&workspace, &ChartWorkspace::identitySelected);
    QTest::keyClicks(input, "KRW");
    QTest::keyClick(input, Qt::Key_Space);
    QCOMPARE(input->text(), QString("KRW "));
    QTest::keyClick(input, Qt::Key_Escape);
    QCOMPARE(input->text(), QString());
    QVERIFY(panel->isVisible());
    QVERIFY(input->hasFocus());

    QTest::keyClick(input, Qt::Key_Down);
    auto* first = rows->currentItem();
    QVERIFY(first);
    QTest::keyClick(input, Qt::Key_Down);
    QVERIFY(rows->currentItem() && rows->currentItem() != first);
    QTest::keyClick(input, Qt::Key_Up);
    QCOMPARE(rows->currentItem(), first);
    QVERIFY(input->hasFocus());
    QCOMPARE(selected.size(), 0);

    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(!panel->isVisible() && button->hasFocus());
    QCOMPARE(selected.size(), 1);

    QTest::keyClick(button, Qt::Key_Space);
    QTRY_VERIFY(panel->isVisible() && input->hasFocus());
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(!panel->isVisible() && button->hasFocus());
    QCOMPARE(selected.size(), 1);
}

void ChartInstrumentPanelTest::closingPanelDropsOnlyVisibleListDemand() {
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("합성 수요");
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-ETH"}));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.setWatchlistModel(&lists);
    QVERIFY(workspace.setIdentity("coin:upbit:KRW-BTC"));
    QSignalSpy visible(&workspace, &ChartWorkspace::visibleIdentitiesChanged);
    QSignalSpy selected(&workspace, &ChartWorkspace::selectionChanged);
    workspace.resize(1440, 900);
    workspace.show();
    QCOMPARE(workspace.visiblePublicIdentityKeysForDisplay(), QStringList());

    auto* button = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* input = workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    QVERIFY(button && input);
    button->click();
    QTRY_VERIFY(!visible.isEmpty() && visible.last().first().toStringList().contains("coin:upbit:KRW-ETH"));
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(visible.last().first().toStringList().isEmpty());
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:upbit:KRW-BTC"));
    QCOMPARE(selected.size(), 0);
}

void ChartInstrumentPanelTest::nestedMenuAndOutsideClick() {
    QTabWidget tabs;
    auto* workspace = new ChartWorkspace(nullptr, nullptr);
    tabs.addTab(workspace, "차트");
    auto* other = new QLineEdit;
    tabs.addTab(other, "다른 화면");
    tabs.resize(1440, 900);
    tabs.show();
    tabs.activateWindow();
    QTRY_VERIFY(tabs.isActiveWindow());

    auto* button = workspace->findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace->findChild<QWidget*>("chartInstrumentPanel");
    auto* input = workspace->findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* menuButton = workspace->findChild<QToolButton*>("chartWorkspaceListMenu");
    QVERIFY(button && panel && input && menuButton && menuButton->menu());
    button->click();
    QTRY_VERIFY(input->hasFocus());
    input->setText("합성");
    auto* menu = menuButton->menu();
    menu->popup(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
    QTRY_VERIFY(menu->isVisible());
    QTest::keyClick(menu, Qt::Key_Escape);
    QTRY_VERIFY(!menu->isVisible());
    QVERIFY(panel->isVisible());
    QCOMPARE(input->text(), QString("합성"));

    // Qt delivers an outside press to the currently grabbed popup window.
    QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, QPoint(-6, 6));
    QTRY_VERIFY(!panel->isVisible());

    button->click();
    QTRY_VERIFY(panel->isVisible() && input->hasFocus());
    tabs.setCurrentWidget(other);
    QTRY_VERIFY(!panel->isVisible());
    QVERIFY(tabs.currentWidget() == other && other->isVisible());
    QVERIFY(!input->hasFocus() && !button->hasFocus());
}

void ChartInstrumentPanelTest::holdingsOpenChartThroughSearch() {
    Vault vault(settings_.filePath("synthetic-popup-holdings"));
    QVERIFY(vault.create("Synthetic instrument panel holdings 2026!"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    ChartWorkspace workspace(nullptr, &portfolio);
    workspace.setPrivateEnabled(true);
    HoldingChartRow unavailable;
    unavailable.assetKey = "wallet:synthetic";
    unavailable.symbol = "SYN";
    unavailable.chartReason = "합성 미지원 종목";
    HoldingChartRow available;
    available.assetKey = "solana:native";
    available.symbol = "SOL";
    available.custodies = {{"phantom", "Phantom", HoldingCustodyState::Fresh, 0}};
    available.chart = WatchlistIdentity{"coin", "upbit", "KRW-SOL"};
    available.candidates = {*available.chart, {"coin", "binance", "SOLUSDT"}};
    workspace.setHoldingRows({unavailable, available});
    workspace.resize(1440, 900);
    workspace.show();

    auto* button = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* input = workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* rows = workspace.findChild<QTreeWidget*>("chartHoldingAssets");
    auto* reason = workspace.findChild<QLabel*>("chartHoldingReason");
    auto* market = workspace.findChild<QComboBox*>("chartHoldingMarketSelector");
    QVERIFY(button && panel && input && rows && reason && market);
    button->click();
    workspace.findChild<QAbstractButton*>("chartBrowserScope_holdings")->click();
    QSignalSpy selected(&workspace, &ChartWorkspace::identitySelected);

    QTRY_COMPARE(rows->topLevelItemCount(), 2);
    QTest::keyClicks(input, "SYN");
    QTest::keyClick(input, Qt::Key_Down);
    QCOMPARE(rows->currentItem(), rows->topLevelItem(0));
    QVERIFY(input->hasFocus());
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(panel->isVisible());
    QCOMPARE(selected.size(), 0);
    QVERIFY(reason->text().contains("합성 미지원 종목"));

    QTest::keyClick(input, Qt::Key_Escape);
    QTest::keyClicks(input, "SOL");
    QTest::keyClick(input, Qt::Key_Down);
    QCOMPARE(rows->currentItem(), rows->topLevelItem(1));
    QVERIFY(input->hasFocus());
    QCOMPARE(selected.size(), 0);
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(!panel->isVisible() && button->hasFocus());
    QCOMPARE(selected.size(), 1);
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:upbit:KRW-SOL"));

    button->click();
    QVERIFY(panel->isVisible());
    QCOMPARE(market->count(), 2);
    workspace.clearPrivate();
    QCOMPARE(rows->topLevelItemCount(), 0);
    QCOMPARE(market->count(), 0);
    QVERIFY(reason->text().isEmpty());
}

void ChartInstrumentPanelTest::splitStockCanBeUnstarredWithoutOpeningAChart() {
    WatchlistWorkspaceList lists;
    const WatchlistIdentity stock{"stock", "KR", "005930"};
    const auto list = lists.createList("합성 주식");
    lists.setPublicStockInstruments({stock});
    QVERIFY(lists.addIdentity(list, stock));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.setWatchlistModel(&lists);
    QVERIFY(workspace.setIdentity("coin:upbit:KRW-BTC"));
    workspace.resize(1440, 900);
    workspace.show();

    auto* button = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* input = workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    auto* split = workspace.findChild<QAbstractButton*>("chartLayout_2");
    QVERIFY(button && panel && input && rows && split);
    split->click();
    button->click();
    workspace.findChild<QAbstractButton*>("chartBrowserScope_watch")->click();
    workspace.findChild<QAbstractButton*>("chartBrowserMarket_kr")->click();
    QTest::keyClicks(input, "005930");
    QTest::keyClick(input, Qt::Key_Down);
    auto* row = rows->currentItem();
    QVERIFY(row);
    QVERIFY(row->data(0, Qt::AccessibleTextRole).toString().contains("주식은 단일 차트에서 열어 주세요."));
    QSignalSpy selected(&workspace, &ChartWorkspace::identitySelected);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(workspace.selectedChartIdentity(), QString("coin:upbit:KRW-BTC"));
    QCOMPARE(selected.size(), 0);
    QVERIFY(panel->isVisible());

    rows->setFocus();
    QTest::keyClick(rows, Qt::Key_Space);
    QTRY_VERIFY(lists.userList(list).entries.isEmpty());
    QVERIFY(panel->isVisible());
}

void ChartInstrumentPanelTest::dev98RowsUseSingleLineGeometry_data() {
    QTest::addColumn<QString>("scope");
    QTest::newRow("all") << QString("all");
    QTest::newRow("watch") << QString("watch");
    QTest::newRow("holdings") << QString("holdings");
}

void ChartInstrumentPanelTest::dev98RowsUseSingleLineGeometry() {
    QFETCH(QString, scope);
    WatchlistWorkspaceList lists;
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    const auto list = lists.createList("한 줄 합성 목록");
    QVERIFY(lists.addIdentity(list, identity));
    WatchlistRowState quote;
    quote.price = "123456";
    quote.change = "+2.34%";
    quote.quoteCurrency = "KRW";
    lists.setRowState(identity, quote);
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    Vault vault(profile.filePath("synthetic-dev98-row-geometry"));
    QVERIFY(vault.create("Synthetic Dev98 row geometry 2026!"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    ChartWorkspace workspace(nullptr, &portfolio);
    workspace.setWatchlistModel(&lists);
    workspace.setPrivateEnabled(true);
    HoldingChartRow holding;
    holding.assetKey = "exchange:BTC";
    holding.symbol = "BTC";
    holding.displayName = "비트코인";
    holding.chart = identity;
    holding.candidates = {identity};
    holding.custodies = {{"upbit", "업비트", HoldingCustodyState::Fresh, 0}};
    holding.alertsEligible = true;
    workspace.setHoldingRows({holding});
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* scopeButton = workspace.findChild<QAbstractButton*>("chartBrowserScope_" + scope);
    QVERIFY(open && panel && scopeButton);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(scopeButton, Qt::LeftButton);
    auto* rows = workspace.findChild<QTreeWidget*>(scope == "holdings" ? "chartHoldingAssets" : "chartWorkspaceWatchlist");
    QVERIFY(rows);
    QTRY_VERIFY(rows->isVisible() && rows->topLevelItemCount() == 1);
    auto* row = rows->topLevelItem(0);
    QTRY_COMPARE(rows->visualItemRect(row).height(), 32);
    if(scope=="watch") {
        QString painted;
        for(const auto& run:panelPaintedCell(rows,row,0))painted+=run.text;
        QVERIFY2(painted.contains(QStringLiteral("업비트")),qPrintable(painted));
    }
    QCOMPARE(rows->headerItem()->text(2), QStringLiteral("★"));

    QList<PanelPaintedText> priceRuns, changeRuns;
    for (int column = 0; column < rows->columnCount(); ++column) {
        if (rows->isColumnHidden(column)) continue;
        for (const auto& run : panelPaintedCell(rows, row, column)) {
            if (run.text.contains("123,456")) priceRuns.append(run);
            if (run.text.contains("2.34%")) changeRuns.append(run);
        }
    }
    QVERIFY2(!priceRuns.isEmpty(), "The row must paint the existing public price");
    QVERIFY2(!changeRuns.isEmpty(), "The row must paint the existing percent change");
    QVERIFY2(qAbs(panelTextCenter(priceRuns.first()) - panelTextCenter(changeRuns.first())) <= 1,
             "Price and percent change must share one vertical center");
    QVERIFY(priceRuns.first().at.x() + priceRuns.first().width < changeRuns.first().at.x());
}

void ChartInstrumentPanelTest::dev98StatusBadgesRemainVisible_data() {
    QTest::addColumn<QString>("scope");
    QTest::newRow("all") << QString("all");
    QTest::newRow("watch") << QString("watch");
}

void ChartInstrumentPanelTest::dev98StatusBadgesRemainVisible() {
    QFETCH(QString, scope);
    WatchlistWorkspaceList lists;
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    const auto list = lists.createList("배지 합성 목록");
    QVERIFY(lists.addIdentity(list, identity));
    WatchlistRowState quote;
    quote.price = "123456";
    quote.change = "-2.34%";
    quote.quoteCurrency = "KRW";
    quote.completedAtMs = QDateTime::currentMSecsSinceEpoch() - 1;
    quote.badgePresentations = {{QStringLiteral("하락"), MovementDirection::Down, false},
                                {QStringLiteral("경고"), MovementDirection::Unknown, true}};
    lists.setRowState(identity, quote);
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* scopeButton = workspace.findChild<QAbstractButton*>("chartBrowserScope_" + scope);
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && scopeButton && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(scopeButton, Qt::LeftButton);
    QTRY_VERIFY(rows->isVisible() && panelRow(rows, identity.key()));
    auto* row = panelRow(rows, identity.key());
    QVERIFY(!row->isHidden());
    QCOMPARE(rows->visualItemRect(row).height(), 32);
    QString text;
    bool ordinaryColor = false, riskColor = false;
    for (const auto& run : panelPaintedCell(rows, row, 0)) {
        text += run.text;
        ordinaryColor |= run.text.contains(QStringLiteral("하락")) &&
                         run.ink == MovementColorPolicy::colorFor(rows, MovementDirection::Down);
        riskColor |= run.text.contains(QStringLiteral("경고")) && run.ink == themeColors().danger;
    }
    QVERIFY2(text.contains(QStringLiteral("하락")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("경고")), qPrintable(text));
    QVERIFY2(ordinaryColor, "The ordinary down badge must keep its movement color beside a fixed risk");
    QVERIFY2(riskColor, "The fixed risk badge must keep its danger color beside an ordinary badge");
    QCOMPARE(lists.rowState(identity).badgePresentations, quote.badgePresentations);
}

void ChartInstrumentPanelTest::dev98RetainedQuoteStatesRemainVisible_data() {
    QTest::addColumn<QString>("scope");
    QTest::addColumn<int>("quoteState");
    QTest::addColumn<QString>("status");
    for (const auto& scope : QStringList{"all", "watch", "holdings"}) {
        QTest::newRow(qPrintable(scope + "-delayed")) << scope << int(WatchlistQuoteState::Delayed) << QStringLiteral("지연");
        QTest::newRow(qPrintable(scope + "-disconnected")) << scope << int(WatchlistQuoteState::Disconnected) << QStringLiteral("끊김");
    }
}

void ChartInstrumentPanelTest::dev98RetainedQuoteStatesRemainVisible() {
    QFETCH(QString, scope);
    QFETCH(int, quoteState);
    QFETCH(QString, status);
    WatchlistWorkspaceList lists;
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    const auto list = lists.createList("시세 상태 합성 목록");
    QVERIFY(lists.addIdentity(list, identity));
    WatchlistRowState quote;
    quote.price = "123456";
    quote.change = "+2.34%";
    quote.quoteCurrency = "KRW";
    quote.quoteState = static_cast<WatchlistQuoteState>(quoteState);
    lists.setRowState(identity, quote);
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    Vault vault(profile.filePath("synthetic-dev98-retained-quote"));
    QVERIFY(vault.create("Synthetic Dev98 retained quote 2026!"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    ChartWorkspace workspace(nullptr, &portfolio);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPrivateEnabled(true);
    HoldingChartRow holding;
    holding.assetKey = "exchange:BTC";
    holding.symbol = "BTC";
    holding.displayName = "비트코인";
    holding.chart = identity;
    holding.candidates = {identity};
    holding.custodies = {{"upbit", "업비트", HoldingCustodyState::Fresh, 0}};
    holding.alertsEligible = true;
    workspace.setHoldingRows({holding});
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* scopeButton = workspace.findChild<QAbstractButton*>("chartBrowserScope_" + scope);
    auto* rows = workspace.findChild<QTreeWidget*>(scope == "holdings" ? "chartHoldingAssets" : "chartWorkspaceWatchlist");
    QVERIFY(open && panel && scopeButton && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(scopeButton, Qt::LeftButton);
    const auto key = scope == "holdings" ? holding.assetKey : identity.key();
    QTRY_VERIFY(rows->isVisible() && panelRow(rows, key));
    QVERIFY(!panelRow(rows, key)->isHidden());
    QCOMPARE(rows->visualItemRect(panelRow(rows, key)).height(), 32);
    const auto paintedRow = [&] {
        QString text;
        auto* row = panelRow(rows, key);
        if (!row) return text;
        for (int column = 0; column < rows->columnCount(); ++column) {
            if (rows->isColumnHidden(column)) continue;
            for (const auto& run : panelPaintedCell(rows, row, column)) text += run.text;
        }
        return text;
    };
    const auto stale = paintedRow();
    QVERIFY2(stale.contains("123,456"), qPrintable(stale));
    QVERIFY2(stale.contains("2.34%"), qPrintable(stale));
    QVERIFY2(stale.contains(status), qPrintable(stale));
    quote.quoteState = WatchlistQuoteState::Receiving;
    lists.setRowState(identity, quote);
    QCoreApplication::processEvents();
    const auto current = paintedRow();
    QVERIFY2(current.contains("123,456"), qPrintable(current));
    QVERIFY2(current.contains("2.34%"), qPrintable(current));
    QVERIFY2(!current.contains(QStringLiteral("지연")) && !current.contains(QStringLiteral("끊김")), qPrintable(current));
    QCOMPARE(lists.rowState(identity).quoteState, WatchlistQuoteState::Receiving);
}

void ChartInstrumentPanelTest::dev98HoldingCoverageLivesInInformation_data() {
    QTest::addColumn<int>("holdingCount");
    QTest::newRow("one-holding") << 1;
    QTest::newRow("no-holdings") << 0;
}

void ChartInstrumentPanelTest::dev98HoldingCoverageLivesInInformation() {
    QFETCH(int, holdingCount);
    WatchlistWorkspaceList lists;
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    const auto list = lists.createList("보유 설명 합성 목록");
    QVERIFY(lists.addIdentity(list, identity));
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    Vault vault(profile.filePath("synthetic-dev98-holdings-information"));
    QVERIFY(vault.create("Synthetic Dev98 holdings information 2026!"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    ChartWorkspace workspace(nullptr, &portfolio);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPrivateEnabled(true);
    HoldingChartRow holding;
    holding.assetKey = "exchange:BTC";
    holding.symbol = "BTC";
    holding.displayName = "비트코인";
    holding.chart = identity;
    holding.candidates = {identity};
    holding.custodies = {{"upbit", "업비트", HoldingCustodyState::Fresh, 0}};
    holding.alertsEligible = true;
    workspace.setHoldingRows(holdingCount ? QList<HoldingChartRow>{holding} : QList<HoldingChartRow>{});
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* scope = workspace.findChild<QAbstractButton*>("chartBrowserScope_holdings");
    auto* rows = workspace.findChild<QTreeWidget*>("chartHoldingAssets");
    auto* count = workspace.findChild<QLabel*>("chartBrowserCount");
    auto* status = workspace.findChild<QLabel*>("chartWorkspaceListStatus");
    auto* information = workspace.findChild<InfoButton*>("chartHoldingInformation");
    QVERIFY(open && panel && scope && rows && count && status && information);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(scope, Qt::LeftButton);
    const auto reason = QStringLiteral("합성 보유 범위 사유 · 공개 종목 식별 자료 부족 · 알림 제외");
    workspace.setHoldingsUnavailableReason(reason);
    QCOMPARE(rows->topLevelItemCount(), holdingCount);
    QCOMPARE(workspace.holdingsUnavailableReason(), reason);
    QTRY_VERIFY(count->isVisibleTo(panel) && information->isVisibleTo(panel));
    QCOMPARE(count->text(), QStringLiteral("%1개").arg(holdingCount));
    QVERIFY2(information->isEnabled(), "Coverage information must be available even with no holding rows");
    const QRect countRect(count->mapTo(panel, QPoint{}), count->size());
    const QRect informationRect(information->mapTo(panel, QPoint{}), information->size());
    QVERIFY(informationRect.left() > countRect.right());
    QVERIFY(qAbs(informationRect.center().y() - countRect.center().y()) <= 1);
    QVERIFY2(!status->isVisibleTo(panel), "Holding coverage belongs behind the info button beside the count");
    const auto publicState = lists.publicState();
    QVERIFY(!publicState.contains(reason.toUtf8()));

    QTest::mouseClick(information, Qt::LeftButton);
    QTRY_VERIFY(information->informationOpen());
    const QPointer<QPlainTextEdit> privateBody = information->findChild<QPlainTextEdit*>("infoPrivateText");
    auto* publicBody = information->findChild<QPlainTextEdit*>("infoPublicText");
    QVERIFY(privateBody && publicBody);
    QVERIFY2(privateBody->isVisible() && privateBody->toPlainText().contains(reason), qPrintable(privateBody->toPlainText()));
    QCOMPARE(privateBody->toPlainText().count(reason), 1);
    QVERIFY(!publicBody->toPlainText().contains(reason));

    workspace.clearPrivate();
    QVERIFY(!information->informationOpen());
    QVERIFY(!privateBody || privateBody->toPlainText().isEmpty());
    QVERIFY(!information->isVisibleTo(panel));
    QVERIFY(!information->isEnabled());
    QVERIFY(workspace.holdingsUnavailableReason().isEmpty());
    QVERIFY(!status->text().contains(reason));
    QCOMPARE(rows->topLevelItemCount(), 0);
    QCOMPARE(lists.publicState(), publicState);
    workspace.setHoldingsUnavailableReason(reason);
    QVERIFY(workspace.holdingsUnavailableReason().isEmpty());
    QVERIFY(!information->isVisibleTo(panel));
    QVERIFY(!information->isEnabled());
    for (const auto* text : information->findChildren<QPlainTextEdit*>())
        QVERIFY(!text->toPlainText().contains(reason));
}

void ChartInstrumentPanelTest::dev98LongNamesStayBeforeNumericColumns() {
    PanelTransport http;
    PublicMarketCatalog markets(&http);
    const QString longName = QStringLiteral("비트코인 이름이 아주 길어서 가격 영역으로 넘어가면 안 되는 합성 종목입니다");
    QVERIFY(seedPanelMarkets(http, markets, longName));
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("말줄임 합성 목록");
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPublicNameSources(&markets, nullptr, nullptr);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    auto* row = panelRow(rows, "coin:upbit:KRW-BTC");
    QVERIFY(row);
    const auto runs = panelPaintedCell(rows, row, 0);
    QString joined;
    const auto bounds = rows->visualRect(rows->indexFromItem(row, 0));
    for (const auto& run : runs) {
        joined += run.text;
        QVERIFY2(run.at.x() >= bounds.left() - 1, qPrintable(run.text));
        QVERIFY2(run.at.x() + run.width <= bounds.right() + 1, qPrintable(run.text));
    }
    QVERIFY2(joined.contains("BTC"), qPrintable(joined));
    QVERIFY2(!joined.contains("BTC/KRW"), qPrintable(joined));
    QVERIFY2(!joined.contains(longName), qPrintable(joined));
    QVERIFY2(joined.contains(QChar(0x2026)), "A long name must be elided within the instrument column");
    const auto priceColumn = panelColumn(rows, "가격");
    QVERIFY(priceColumn >= 0);
    const auto price = rows->visualRect(rows->indexFromItem(row, priceColumn));
    QVERIFY(bounds.right() < price.left());
}

void ChartInstrumentPanelTest::dev98PublicReferenceIsTooltipOnly() {
    PanelTransport instrumentHttp, boardHttp;
    ChartInstrumentCatalog instruments(&instrumentHttp);
    PublicEquityBoard board(&boardHttp);
    instruments.ensure("binance");
    QCOMPARE(instrumentHttp.tokens.size(), 1);
    instrumentHttp.reply(0, R"({"symbols":[{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true}]})");
    board.refreshBinanceRanking();
    QTRY_COMPARE(boardHttp.tokens.size(), 1);
    const QJsonArray ticks{QJsonObject{{"symbol", "BTCUSDT"}, {"lastPrice", "100000"},
        {"priceChangePercent", "-0.420"}, {"quoteVolume", "1000000000"},
        {"closeTime", double(QDateTime::currentMSecsSinceEpoch() - 1000)}, {"count", 10}}};
    boardHttp.reply(0, QJsonDocument(ticks).toJson(QJsonDocument::Compact));
    WatchlistWorkspaceList lists;
    lists.createList("참고 시세 합성 목록");
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPublicNameSources(nullptr, &board, &instruments);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* market = workspace.findChild<QAbstractButton*>("chartBrowserMarket_binance");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && market && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(market, Qt::LeftButton);
    QTRY_VERIFY(panelRow(rows, "coin:binance:BTCUSDT"));
    auto* row = panelRow(rows, "coin:binance:BTCUSDT");
    QVERIFY2((row->toolTip(0) + row->toolTip(1)).contains("공개 참고 시세"),
             "The public-source qualification must remain available in the row tooltip");
    const auto visible = paintedPanelText(panel);
    for (const auto& unwanted : QStringList{"Spot", "현물 USDT", "공개 참고"})
        QVERIFY2(!visible.contains(unwanted), qPrintable(visible));
    QVERIFY2(visible.contains("-0.42%"), qPrintable(visible));
}

void ChartInstrumentPanelTest::dev98HeaderSortPreservesSavedOrderAndRequests() {
    PanelTransport http, boardHttp;
    PublicMarketCatalog markets(&http);
    PublicEquityBoard board(&boardHttp);
    QVERIFY(seedPanelMarkets(http, markets));
    board.refresh();
    QCOMPARE(boardHttp.tokens.size(), 1);
    boardHttp.reply(0, R"([{"id":"bitcoin","symbol":"btc","name":"Bitcoin","market_cap":2000000000000,"current_price":123456},{"id":"ethereum","symbol":"eth","name":"Ethereum","market_cap":500000000000,"current_price":200000},{"id":"solana","symbol":"sol","name":"Solana","market_cap":100000000000,"current_price":42}])");
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("정렬 합성 목록");
    lists.useList(list);
    for (const auto& code : QStringList{"KRW-ETH", "KRW-BTC", "KRW-SOL"})
        QVERIFY(lists.addIdentity(list, {"coin", "upbit", code}));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPublicNameSources(&markets, &board, nullptr);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    const QString btc = "coin:upbit:KRW-BTC", eth = "coin:upbit:KRW-ETH", sol = "coin:upbit:KRW-SOL";
    QTRY_COMPARE(panelVisibleKeys(rows), (QStringList{btc, sol, eth}));
    const auto saved = lists.publicState();
    const auto selected = workspace.selectedChartIdentity();
    const auto requestCount = http.requests.size() + boardHttp.requests.size();
    QVERIFY(clickPanelHeading(rows, "24시간"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{sol, btc, eth}));
    QVERIFY(clickPanelHeading(rows, "24시간"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{eth, btc, sol}));
    QVERIFY(clickPanelHeading(rows, "종목"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{btc, sol, eth}));
    QVERIFY(clickPanelHeading(rows, "종목"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{eth, sol, btc}));
    QVERIFY(clickPanelHeading(rows, "가격"));
    const auto byPrice = panelVisibleKeys(rows);
    const QStringList priceUp{sol, btc, eth}, priceDown{eth, btc, sol};
    QVERIFY(byPrice == priceUp || byPrice == priceDown);
    QVERIFY(clickPanelHeading(rows, "가격"));
    QCOMPARE(panelVisibleKeys(rows), byPrice == priceUp ? priceDown : priceUp);
    QVERIFY(clickPanelHeading(rows, "거래대금"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{btc, sol, eth}));
    QVERIFY(clickPanelHeading(rows, "거래대금"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{eth, sol, btc}));
    auto* expand = workspace.findChild<QAbstractButton*>("chartBrowserExpand");
    QVERIFY(expand);
    QTest::mouseClick(expand, Qt::LeftButton);
    QVERIFY(clickPanelHeading(rows, "시가총액"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{btc, eth, sol}));
    QVERIFY(clickPanelHeading(rows, "시가총액"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{sol, eth, btc}));
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    QVERIFY(watch);
    QTest::mouseClick(watch, Qt::LeftButton);
    QVERIFY(clickPanelHeading(rows, "종목"));
    QCOMPARE(panelVisibleKeys(rows), (QStringList{btc, sol, eth}));
    auto* restore = panelTextButton(panel, "내 순서로");
    QVERIFY2(restore, "Temporary sorting needs a visible return to the saved order");
    QTest::mouseClick(restore, Qt::LeftButton);
    QCOMPARE(panelVisibleKeys(rows), (QStringList{eth, btc, sol}));
    QCOMPARE(lists.publicState(), saved);
    QCOMPARE(workspace.selectedChartIdentity(), selected);
    QCOMPARE(http.requests.size() + boardHttp.requests.size(), requestCount);
    auto* input = workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    QVERIFY(input);
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(!panel->isVisible());
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QCOMPARE(http.requests.size() + boardHttp.requests.size(), requestCount);
    QCOMPARE(lists.publicState(), saved);
}

void ChartInstrumentPanelTest::dev98HeaderSortIsKeyboardReachable() {
    PanelTransport http;
    PublicMarketCatalog markets(&http);
    QVERIFY(seedPanelMarkets(http, markets));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setPublicNameSources(&markets, nullptr, nullptr);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* input = workspace.findChild<QLineEdit*>("chartWorkspaceAsset");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && input && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QAbstractButton* price = nullptr;
    for (auto* button : panel->findChildren<QAbstractButton*>())
        if (button->isVisible() && button->accessibleName() == "가격순 정렬") price = button;
    QVERIFY2(price, "Price sorting must have a named keyboard control");
    QVERIFY(price->focusPolicy() & Qt::TabFocus);
    QTest::mouseClick(input, Qt::LeftButton);
    for (int count = 0; count < 48 && !price->hasFocus(); ++count) {
        QVERIFY(QApplication::focusWidget());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    }
    QVERIFY2(price->hasFocus(), "Tab must reach the price sort header from search");
    QTest::keyClick(price, Qt::Key_Space);
    const auto first = panelVisibleKeys(rows);
    const QStringList up{"coin:upbit:KRW-SOL", "coin:upbit:KRW-BTC", "coin:upbit:KRW-ETH"};
    const QStringList down{"coin:upbit:KRW-ETH", "coin:upbit:KRW-BTC", "coin:upbit:KRW-SOL"};
    QVERIFY(first == up || first == down);
    QTest::keyClick(price, Qt::Key_Return);
    QCOMPARE(panelVisibleKeys(rows), first == up ? down : up);
}

void ChartInstrumentPanelTest::dev98HeaderReplacesFooterControls() {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    Vault vault(profile.filePath("synthetic-dev98-panel-header"));
    QVERIFY(vault.create("Synthetic Dev98 panel header 2026!"));
    PortfolioController::Factories factories;
    factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault, nullptr, factories);
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("머리 합성 목록");
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    ChartWorkspace workspace(nullptr, &portfolio);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPrivateEnabled(true);
    HoldingChartRow holding;
    holding.assetKey = "exchange:BTC";
    holding.symbol = "BTC";
    holding.chart = WatchlistIdentity{"coin", "upbit", "KRW-BTC"};
    holding.candidates = {*holding.chart, {"coin", "binance", "BTCUSDT"}};
    holding.custodies = {{"upbit", "업비트", HoldingCustodyState::Fresh, 0}};
    holding.alertsEligible = true;
    workspace.setHoldingRows({holding});
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    auto* selector = workspace.findChild<QComboBox*>("chartWorkspaceListSelector");
    auto* trend = workspace.findChild<QAbstractButton*>("chartWatchTrendToggle");
    auto* expand = workspace.findChild<QAbstractButton*>("chartBrowserExpand");
    QVERIFY(open && panel && watch && selector && trend && expand);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(watch, Qt::LeftButton);
    auto* sort = workspace.findChild<QWidget*>("chartBrowserSort");
    auto* find = workspace.findChild<QWidget*>("chartInstrumentFindPublic");
    QVERIFY2(!sort || !sort->isVisible(), "The sort dropdown must leave the visible panel");
    QVERIFY2(!find || !find->isVisible(), "The old instrument-finder footer button must be absent");
    QVERIFY(selector->isVisible() && trend->isVisible() && expand->isVisible());
    QVERIFY(panel->height() <= 520);
    const auto center = [panel](QWidget* widget) { return widget->mapTo(panel, widget->rect().center()).y(); };
    QVERIFY(qAbs(center(selector) - center(watch)) <= 1);
    QVERIFY(qAbs(center(trend) - center(watch)) <= 1);
    QVERIFY(qAbs(center(expand) - center(watch)) <= 1);
    QVERIFY2(!expand->text().contains("목록 넓게"), "Expand is an icon control in the header");
    QVERIFY2(panelTextButton(panel, "＋"), "The list header needs its own create button");
    auto* holdings = workspace.findChild<QAbstractButton*>("chartBrowserScope_holdings");
    auto* rows = workspace.findChild<QTreeWidget*>("chartHoldingAssets");
    QVERIFY(holdings && rows);
    QTest::mouseClick(holdings, Qt::LeftButton);
    QTRY_VERIFY(rows->isVisible() && rows->topLevelItemCount() == 1);
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows->visualItemRect(rows->topLevelItem(0)).center());
    auto* market = workspace.findChild<QWidget*>("chartHoldingMarketSelector");
    auto* reason = workspace.findChild<QWidget*>("chartHoldingReason");
    QVERIFY2(!market || !market->isVisible(), "Holding chart sources belong in the row context menu");
    QVERIFY2(!reason || !reason->isVisible(), "Holding explanations belong behind the info control");
}

void ChartInstrumentPanelTest::dev98MenuKeepsOnlyDesignedActions() {
    WatchlistWorkspaceList lists;
    const auto list = lists.createList("메뉴 합성 목록");
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    auto* button = workspace.findChild<QToolButton*>("chartWorkspaceListMenu");
    QVERIFY(open && panel && watch && button && button->menu());
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(watch, Qt::LeftButton);
    auto* menu = button->menu();
    QStringList core;
    bool inspected = false;
    const QStringList legacyMoves{"chartListRenameSection", "chartListMoveSection", "chartListRemoveIdentity",
        "chartListMoveUp", "chartListMoveDown", "chartListMoveListUp", "chartListMoveListDown"};
    QTimer inspect;
    inspect.setInterval(1);
    connect(&inspect, &QTimer::timeout, menu, [&] {
        if (!menu->isVisible()) return;
        for (auto* action : menu->actions()) {
            if (!action->isVisible() || action->isSeparator() || legacyMoves.contains(action->objectName())) continue;
            core.append(action->text());
        }
        inspected = true;
        inspect.stop();
        QTest::keyClick(menu, Qt::Key_Escape);
    });
    inspect.start();
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY(inspected);
    QCOMPARE(core, (QStringList{"이름 바꾸기", "색 깃발", "섹션 추가", "대표 종목으로 채우기…",
                                "CSV 가져오기", "CSV 내보내기", "리스트 알림 설정", "리스트 삭제"}));
    QVERIFY(panel->isVisible());
}

void ChartInstrumentPanelTest::dev98CsvFailuresStayVisible_data() {
    QTest::addColumn<QString>("failure");
    QTest::addColumn<QString>("expectedError");
    QTest::newRow("invalid-csv") << QString("parse")
        << QStringLiteral("CSV 형식·공개 종목 identity·시장 메타데이터를 확인해 주세요.");
    QTest::newRow("source-disappears-after-selection") << QString("read")
        << QStringLiteral("CSV를 읽지 못했습니다.");
    QTest::newRow("destination-disappears-after-selection") << QString("write")
        << QStringLiteral("CSV를 저장하지 못했습니다.");
}

void ChartInstrumentPanelTest::dev98CsvFailuresStayVisible() {
    QFETCH(QString, failure);
    QFETCH(QString, expectedError);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    WatchlistWorkspaceList lists;
    const auto list = lists.createList(QStringLiteral("CSV 오류 합성 목록"));
    QVERIFY(!list.isEmpty());
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    const bool exporting = failure == "write";
    const auto directory = exporting ? files.filePath("synthetic-destination") : files.path();
    QVERIFY(QDir().mkpath(directory));
    const auto csv = QDir(directory).filePath("synthetic-watchlist.csv");
    if (!exporting) {
        const auto contents = failure == "parse" ? QByteArray("not,a,watchlist\ninvalid,synthetic,csv\n")
                                                : lists.exportCsv(list);
        QVERIFY(!contents.isEmpty());
        QFile file(csv);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();
    }

    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    auto* menuButton = workspace.findChild<QToolButton*>("chartWorkspaceListMenu");
    auto* status = workspace.findChild<QLabel*>("chartWorkspaceListStatus");
    QVERIFY(open && panel && watch && menuButton && menuButton->menu() && status);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(watch, Qt::LeftButton);
    QTRY_COMPARE(workspace.currentListId(), list);
    QTRY_COMPARE(status->text(), QStringLiteral("1개"));
    QVERIFY(!status->isVisible());
    const auto saved = lists.publicState();
    QSignalSpy changed(&lists, &WatchlistWorkspaceList::changed);
    auto* menu = menuButton->menu();
    auto* action = workspace.findChild<QAction*>(exporting ? "chartListExportCsv" : "chartListImportCsv");
    QVERIFY(action);
    bool menuClicked = false, dialogPrepared = false, dialogAccepted = false;
    bool expectedDialog = true, timedOut = false, failureInjected = failure == "parse";

    QTimer chooseFile;
    chooseFile.setInterval(1);
    connect(&chooseFile, &QTimer::timeout, &workspace, [&] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (!dialogPrepared) {
            expectedDialog = dialog->objectName() == (exporting ? "chartWatchlistExportDialog" : "chartWatchlistImportDialog")
                && dialog->testOption(QFileDialog::DontUseNativeDialog);
            if (!expectedDialog) { chooseFile.stop(); dialog->reject(); return; }
            dialog->setDirectory(directory);
            dialog->selectFile(QFileInfo(csv).fileName());
            connect(dialog, &QDialog::accepted, &workspace, [&] {
                dialogAccepted = true;
                // Only this test's temporary paths disappear, after QFileDialog
                // validates the selection and before application I/O begins.
                if (failure == "read") failureInjected = QFile::remove(csv);
                else if (failure == "write") failureInjected = QDir().rmdir(directory);
            });
            dialogPrepared = true;
            return;
        }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!buttons) return;
        for (auto* button : buttons->buttons()) {
            if (buttons->buttonRole(button) != QDialogButtonBox::AcceptRole || !button->isEnabled()) continue;
            chooseFile.stop();
            QTest::mouseClick(button, Qt::LeftButton);
            if (!dialogAccepted && dialog->isVisible()) chooseFile.start();
            return;
        }
    });
    QTimer chooseAction;
    chooseAction.setInterval(1);
    connect(&chooseAction, &QTimer::timeout, &workspace, [&] {
        if (!menu->isVisible()) return;
        chooseAction.stop();
        if (!action->isVisible() || !action->isEnabled() || menu->actionGeometry(action).isEmpty()) {
            QTest::keyClick(menu, Qt::Key_Escape);
            return;
        }
        menuClicked = true;
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(5000);
    connect(&watchdog, &QTimer::timeout, &workspace, [&] {
        timedOut = true;
        chooseFile.stop();
        chooseAction.stop();
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
        if (menu->isVisible()) QTest::keyClick(menu, Qt::Key_Escape);
    });
    chooseFile.start();
    chooseAction.start();
    watchdog.start();
    QTest::mouseClick(menuButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(dialogAccepted || timedOut, 6000);
    chooseFile.stop();
    chooseAction.stop();
    watchdog.stop();
    QVERIFY2(!timedOut, "The actual CSV menu and non-native file dialog must finish");
    QVERIFY(menuClicked && dialogPrepared && expectedDialog && dialogAccepted);
    QVERIFY2(failureInjected, "The intended temporary-file failure must be reached");
    QCOMPARE(status->text(), expectedError);
    QVERIFY2(status->isVisible(), qPrintable(status->text()));
    QVERIFY(panel->isVisible());
    QCOMPARE(workspace.currentListId(), list);
    QCOMPARE(lists.publicState(), saved);
    QCOMPARE(changed.size(), 0);
}

void ChartInstrumentPanelTest::dev98TurnoverFitsCompactColumnWithoutRepeatedCurrency() {
    PanelTransport http;
    PublicMarketCatalog markets(&http);
    markets.refresh();
    QCOMPARE(http.tokens.size(), 1);
    http.reply(0, R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin"}])");
    QTRY_COMPARE_WITH_TIMEOUT(http.tokens.size(), 2, 3000);
    const QJsonArray quotes{QJsonObject{{"market", "KRW-BTC"}, {"trade_price", "123456"},
        {"prev_closing_price", "123456"}, {"signed_change_rate", "0.0234"},
        {"timestamp", QString::number(QDateTime::currentMSecsSinceEpoch() - 100)},
        {"acc_trade_price_24h", "523400000000"}, {"acc_trade_volume_24h", "4239742"}}};
    http.reply(1, QJsonDocument(quotes).toJson(QJsonDocument::Compact));
    QCOMPARE(markets.rows().size(), 1);
    QCOMPARE(markets.rows().first().tradeValue24h, QString("523400000000"));

    WatchlistWorkspaceList lists;
    const auto list = lists.createList(QStringLiteral("거래대금 합성 목록"));
    QVERIFY(lists.addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.setPublicNameSources(&markets, nullptr, nullptr);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* all = workspace.findChild<QAbstractButton*>("chartBrowserScope_all");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && all && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(all, Qt::LeftButton);
    QTRY_VERIFY(rows->isVisible() && panelRow(rows, "coin:upbit:KRW-BTC"));
    auto* row = panelRow(rows, "coin:upbit:KRW-BTC");
    const auto column = panelColumn(rows, QStringLiteral("거래대금"));
    QVERIFY(column >= 0 && !rows->isColumnHidden(column));
    QCOMPARE(rows->columnWidth(column), 64);
    const auto metadata = row->data(column, Qt::DisplayRole);
    QCOMPARE(metadata.toString(), QStringLiteral("5234.0억 KRW"));
    const auto saved = lists.publicState();
    const auto requestCount = http.requests.size();
    const auto bounds = rows->visualRect(rows->indexFromItem(row, column));
    QCOMPARE(bounds.width(), 64);
    const auto runs = panelPaintedCell(rows, row, column);
    QVERIFY(!runs.isEmpty());
    QString painted;
    for (const auto& run : runs) {
        painted += run.text;
        QVERIFY2(run.at.x() >= bounds.left() - 1, qPrintable(run.text));
        QVERIFY2(run.at.x() + run.width <= bounds.right() + 1, qPrintable(run.text));
    }
    QVERIFY2(!painted.contains(QChar(0x2026)) && !painted.contains("..."), qPrintable(painted));
    QVERIFY2(!painted.contains("KRW"), qPrintable(painted));
    QVERIFY2(painted.endsWith(QStringLiteral("억")), qPrintable(painted));
    auto amount = painted;
    amount.chop(1);
    amount.remove(',');
    bool numeric = false;
    const auto value = amount.trimmed().toDouble(&numeric);
    QVERIFY2(numeric, qPrintable(painted));
    QCOMPARE(value, 5234.0);
    QCOMPARE(row->data(column, Qt::DisplayRole), metadata);
    QCOMPARE(markets.rows().first().tradeValue24h, QString("523400000000"));
    QCOMPARE(lists.publicState(), saved);
    QCOMPARE(http.requests.size(), requestCount);
}

void ChartInstrumentPanelTest::dev98RowContextRemoveUsesDangerColor_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void ChartInstrumentPanelTest::dev98RowContextRemoveUsesDangerColor() {
    QFETCH(bool, dark);
    const auto previousTheme = KeefiTheme::instance()->mode();
    const auto restoreTheme = qScopeGuard([&] { KeefiTheme::instance()->setMode(previousTheme); });
    KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
    WatchlistWorkspaceList lists;
    const WatchlistIdentity identity{"coin", "upbit", "KRW-BTC"};
    const auto list = lists.createList(QStringLiteral("행 메뉴 색 합성 목록"));
    QVERIFY(lists.addIdentity(list, identity));
    lists.useList(list);
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    QVERIFY(open && panel && watch && rows);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(watch, Qt::LeftButton);
    QTRY_VERIFY(rows->isVisible() && panelRow(rows, identity.key()));
    const auto row = rows->visualItemRect(panelRow(rows, identity.key()));
    QVERIFY(!row.isEmpty());
    const auto saved = lists.publicState();
    const auto selected = workspace.selectedChartIdentity();
    QSignalSpy opened(&workspace, &ChartWorkspace::identitySelected);
    const QPoint position(row.left() + qMin(110, qMax(1, row.width() / 3)), row.center().y());
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, rows->viewport()->mapToGlobal(position));
    QApplication::sendEvent(rows->viewport(), &context);
    const QPointer<QMenu> menu = workspace.findChild<QMenu*>("chartInstrumentRowMenu");
    const auto closeMenu = qScopeGuard([&] {
        if (menu && menu->isVisible()) QTest::keyClick(menu, Qt::Key_Escape);
    });
    QTRY_VERIFY(menu && menu->isVisible());
    QAction* remove = nullptr;
    QAction* neutral = nullptr;
    for (auto* action : menu->actions()) {
        if (action->text() == QStringLiteral("관심 해제")) remove = action;
        if (action->text() == QStringLiteral("알림 설정")) neutral = action;
    }
    QVERIFY(remove && neutral && remove->isVisible() && remove->isEnabled());
    QTest::mouseMove(menu, menu->actionGeometry(neutral).center());
    const auto actionRect = menu->actionGeometry(remove).adjusted(2, 2, -2, -2);
    QVERIFY(!actionRect.isEmpty() && menu->rect().contains(actionRect));
    const auto image = menu->grab().toImage();
    QVERIFY(!image.isNull());
    const auto ratio = image.devicePixelRatio();
    const auto pixels = QRectF(actionRect.x() * ratio, actionRect.y() * ratio,
                              actionRect.width() * ratio, actionRect.height() * ratio)
                            .toAlignedRect().intersected(image.rect());
    QVERIFY(!pixels.isEmpty());
    int dangerPixels = 0;
    for (int y = pixels.top(); y <= pixels.bottom(); ++y)
        for (int x = pixels.left(); x <= pixels.right(); ++x)
            if (image.pixelColor(x, y) == themeColors().danger) ++dangerPixels;
    QTest::keyClick(menu, Qt::Key_Escape);
    QTRY_VERIFY(!menu || !menu->isVisible());
    QCOMPARE(lists.publicState(), saved);
    QCOMPARE(workspace.selectedChartIdentity(), selected);
    QCOMPARE(opened.count(), 0);
    QVERIFY2(dangerPixels > 0, qPrintable(QString("The visible remove action must use danger ink: theme=%1 color=%2 pixels=%3")
        .arg(dark ? "dark" : "light", themeColors().danger.name()).arg(dangerPixels)));
}

void ChartInstrumentPanelTest::dev98WatchInteractionHintsFollowSortMode() {
    WatchlistWorkspaceList lists;
    const auto list = lists.createList(QStringLiteral("관심 조작 안내 합성 목록"));
    for (const auto& code : QStringList{"KRW-ETH", "KRW-BTC"})
        QVERIFY(lists.addIdentity(list, {"coin", "upbit", code}));
    lists.useList(list);
    ChartWorkspace workspace(nullptr, nullptr);
    workspace.indicatorPreferences()->markIntroSeen();
    workspace.setWatchlistModel(&lists);
    workspace.resize(1440, 900);
    workspace.show();
    auto* open = workspace.findChild<QAbstractButton*>("chartInstrumentButton");
    auto* panel = workspace.findChild<QWidget*>("chartInstrumentPanel");
    auto* watch = workspace.findChild<QAbstractButton*>("chartBrowserScope_watch");
    auto* rows = workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");
    auto* hint = workspace.findChild<QLabel*>("chartBrowserHint");
    QVERIFY(open && panel && watch && rows && hint);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY(panel->isVisible());
    QTest::mouseClick(watch, Qt::LeftButton);
    QTRY_VERIFY(rows->isVisible() && hint->isVisible());
    const QStringList manualKeys{"coin:upbit:KRW-ETH", "coin:upbit:KRW-BTC"};
    QCOMPARE(panelVisibleKeys(rows), manualKeys);
    const auto manualHint = hint->text();
    const auto saved = lists.publicState();
    const auto selected = workspace.selectedChartIdentity();
    QSignalSpy opened(&workspace, &ChartWorkspace::identitySelected);
    QVERIFY(clickPanelHeading(rows, QStringLiteral("종목")));
    QTRY_COMPARE(hint->text(), QStringLiteral("내 순서로 돌아가면 옮길 수 있어요"));
    QVERIFY(hint->isVisible());
    auto* restore = panelTextButton(panel, QStringLiteral("내 순서로"));
    QVERIFY(restore);
    QTest::mouseClick(restore, Qt::LeftButton);
    QTRY_COMPARE(hint->text(), manualHint);
    QVERIFY(hint->isVisible());
    QCOMPARE(panelVisibleKeys(rows), manualKeys);
    QCOMPARE(lists.publicState(), saved);
    QCOMPARE(workspace.selectedChartIdentity(), selected);
    QCOMPARE(opened.count(), 0);
    QVERIFY2(manualHint.contains(QStringLiteral("끌기")) && manualHint.contains(QStringLiteral("⌥↑↓")) &&
             manualHint.contains(QStringLiteral("Delete")), qPrintable(manualHint));
}

QTEST_MAIN(ChartInstrumentPanelTest)
#include "tst_chart_instrument_panel.moc"
