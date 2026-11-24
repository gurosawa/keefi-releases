#include "keefi/AccountModel.h"
#include "keefi/ChartMarketContext.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/InfoButton.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketCandles.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace keefi;

namespace {
// 2026-11-05 23:30 KST, the board as read that night (US markets just open).
const qint64 Now = QDateTime(QDate(2026, 11, 5), QTime(23, 30), QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();

class PublicTransport final : public TradingTransport {
  public:
    quint64 token = 0;
    void send(quint64 id, const RequestSpec& request) override { QVERIFY(!request.authenticated); token = id; }
    void abort() override {}
};

WorldMarketQuote quote(const QString& price, const QString& change, const QString& percent, WorldMarketSession session, int delay = 0) {
    WorldMarketQuote q;
    q.price = price; q.change = change; q.changePercent = percent; q.session = session; q.delayMinutes = delay;
    q.tradedAtMs = Now - 60000; q.localDate = QDate(2026, 11, 5); q.venue = QStringLiteral("합성"); q.receivedAtMs = Now - 1000;
    return q;
}
WorldMarketsSnapshot world() {
    WorldMarketsSnapshot s;
    s.quotes[int(WorldMarketItem::UsdKrw)] = quote("1346.30", "3.80", "0.28", WorldMarketSession::Open);
    s.quotes[int(WorldMarketItem::Spx)] = quote("7674.53", "-14.09", "-0.18", WorldMarketSession::Open);
    s.quotes[int(WorldMarketItem::Ndx)] = quote("28043.71", "-393.46", "-1.38", WorldMarketSession::Open);
    s.quotes[int(WorldMarketItem::Kospi)] = quote("5383.80", "-300.00", "-5.28", WorldMarketSession::Closed);
    s.dollarIndex = {"101.25", "101.87", "-0.60", QDate(2026, 11, 4), QDate(2026, 11, 3), Now - 1000};
    s.active = true;
    return s;
}
ChartMarketInputs inputs(const QString& market, const QString& source, const QString& code, const QString& price, const QString& currency) {
    ChartMarketInputs in;
    in.market = market; in.source = source; in.code = code; in.price = price; in.currency = currency;
    in.world = world(); in.nowMs = Now;
    return in;
}
QStringList labels(const ChartMarketContext& context) {
    QStringList result;
    for (const auto& item : context.related) result.append(item.label);
    return result;
}
} // namespace

class ChartMarketContextTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); QApplication::setFont(KeefiFonts::font(FontRole::Body15)); }

    void usStockShowsItsWonValueAndUsIndices() {
        const auto context = chartMarketContext(inputs("stock", "us", "AAPL", "230.12", "USD"));
        QCOMPARE(context.identityKey, QString("stock:us:AAPL"));
        QCOMPARE(context.conversion, QStringLiteral("≈ 309,811원")); // 230.12 × 1,346.30 = 309,810.556
        QVERIFY(context.conversionDetail.contains(QStringLiteral("하나은행"))); QVERIFY(context.conversionDetail.contains(QStringLiteral("1,346.30")));
        QVERIFY(context.premium.isEmpty());
        QCOMPARE(labels(context), (QStringList{"S&P 500", QStringLiteral("나스닥 100"), QStringLiteral("원/달러")}));
        QCOMPARE(context.related.at(0).change, QString("-0.18%")); QCOMPARE(context.related.at(0).direction, MovementDirection::Down);
        QCOMPARE(context.related.at(2).value, QString("1,346.30")); QCOMPARE(context.related.at(2).change, QString("+0.28%"));
    }

    void binanceCoinShowsItsWonValueAndThePremium() {
        auto in = inputs("coin", "binance", "BTCUSDT", "79437.12", "USDT");
        in.upbitKrw = "106971000"; in.upbitAtMs = Now - 2000; in.binanceUsdt = "79437.12"; in.binanceAtMs = Now - 1000;
        const auto context = chartMarketContext(in);
        QCOMPARE(context.conversion, QStringLiteral("≈ 106,946,195원")); // 79,437.12 × 1,346.30
        QVERIFY(context.conversionDetail.contains(QStringLiteral("1 USDT ≈ 1 USD")));
        QCOMPARE(context.premium, QStringLiteral("김프 +0.02%"));      // independent: +0.0232%
        QVERIFY(context.premiumDetail.contains(QStringLiteral("하나은행")));
        QCOMPARE(labels(context), (QStringList{QStringLiteral("나스닥 100"), QStringLiteral("달러 인덱스"), QStringLiteral("원/달러")}));
        QCOMPARE(context.related.at(1).value, QString("101.25")); QCOMPARE(context.related.at(1).change, QString("-0.60%"));
    }

    void upbitCoinShowsTheBinancePriceInWon() {
        auto in = inputs("coin", "upbit", "KRW-BTC", "106971000", "KRW");
        in.upbitKrw = "106971000"; in.upbitAtMs = Now - 2000; in.binanceUsdt = "79437.12"; in.binanceAtMs = Now - 1000;
        const auto context = chartMarketContext(in);
        QCOMPARE(context.conversion, QStringLiteral("바이낸스 106,946,195원"));
        QCOMPARE(context.premium, QStringLiteral("김프 +0.02%"));
    }

    void stalePricesOrRateHideTheComparison() {
        auto in = inputs("coin", "upbit", "KRW-BTC", "106971000", "KRW");
        in.upbitKrw = "106971000"; in.upbitAtMs = Now - 2000; in.binanceUsdt = "79437.12"; in.binanceAtMs = Now - 91000;
        auto context = chartMarketContext(in);
        QVERIFY(context.conversion.isEmpty()); QVERIFY(context.premium.isEmpty()); // A Binance price 91 s old is not compared.
        QCOMPARE(context.related.size(), 3); // The related markets do not depend on it.
        in.binanceAtMs = Now - 1000; in.upbitAtMs = Now - 62000;
        context = chartMarketContext(in);
        QVERIFY(!context.conversion.isEmpty()); QVERIFY(context.premium.isEmpty()); // Legs 61 s apart.
        in.upbitAtMs = Now - 2000;
        in.world.quotes[int(WorldMarketItem::UsdKrw)].receivedAtMs = Now - 3 * WorldMarkets::RefreshMs - 1000;
        context = chartMarketContext(in);
        QVERIFY(context.conversion.isEmpty()); QVERIFY(context.premium.isEmpty()); // The rate itself is late.
        QVERIFY(context.related.at(2).late); // ...and its row says so.
        in.world.quotes[int(WorldMarketItem::UsdKrw)] = {};
        context = chartMarketContext(in);
        QVERIFY(context.conversion.isEmpty()); QCOMPARE(labels(context), (QStringList{QStringLiteral("나스닥 100"), QStringLiteral("달러 인덱스")}));
    }

    void closedRelatedMarketsUseTheirOwnRefreshInterval_data() {
        QTest::addColumn<qint64>("ageMs");
        QTest::addColumn<bool>("rateLate");
        QTest::addColumn<bool>("indexLate");
        QTest::newRow("at-three-minutes") << qint64(180000) << false << false;
        QTest::newRow("after-three-minutes") << qint64(180001) << true << false;
        QTest::newRow("at-thirty-minutes") << qint64(1800000) << true << false;
        QTest::newRow("after-thirty-minutes") << qint64(1800001) << true << true;
    }

    void closedRelatedMarketsUseTheirOwnRefreshInterval() {
        QFETCH(qint64, ageMs);
        QFETCH(bool, rateLate);
        QFETCH(bool, indexLate);
        auto in = inputs("stock", "us", "AAPL", "230.12", "USD");
        for (const auto item : {WorldMarketItem::UsdKrw, WorldMarketItem::Spx}) {
            auto& quote = in.world.quotes[int(item)];
            quote.session = WorldMarketSession::Closed;
            quote.receivedAtMs = Now - ageMs;
        }
        const auto context = chartMarketContext(in);
        QCOMPARE(labels(context), (QStringList{"S&P 500", QStringLiteral("나스닥 100"), QStringLiteral("원/달러")}));
        const auto& index = context.related.first();
        const auto& rate = context.related.last();
        // The exchange's closed flag does not change the bank rate's one-minute
        // refresh interval; a closed index alone uses ten-minute refreshes.
        QVERIFY(index.closed && rate.closed);
        QCOMPARE(index.late, indexLate);
        QCOMPARE(rate.late, rateLate);
        QCOMPARE(rate.detail.contains(QStringLiteral("최근 확인")), rateLate);
    }

    void koreanStockKeepsItsPriceAndShowsKospiFirst() {
        const auto context = chartMarketContext(inputs("stock", "kr", "005930", "98000", "KRW"));
        QVERIFY(context.conversion.isEmpty());
        QCOMPARE(labels(context), (QStringList{QStringLiteral("코스피"), QStringLiteral("원/달러"), QStringLiteral("나스닥 100")}));
        QVERIFY(context.related.at(0).closed); QCOMPARE(context.related.at(0).change, QString("-5.28%"));
    }

    void preOpenSaysSoAndMissingValuesAreOmitted() {
        auto in = inputs("stock", "us", "AAPL", "230.12", "USD");
        in.world.quotes[int(WorldMarketItem::Spx)].session = WorldMarketSession::PreOpen;
        in.world.quotes[int(WorldMarketItem::Ndx)] = {};
        const auto context = chartMarketContext(in);
        QCOMPARE(labels(context), (QStringList{"S&P 500", QStringLiteral("원/달러")}));
        QCOMPARE(context.related.at(0).change, QStringLiteral("장 전")); QCOMPARE(context.related.at(0).direction, MovementDirection::Unknown);
        QVERIFY(chartMarketContext(ChartMarketInputs{}).empty());
    }

    void wonAmountsKeepUsefulPrecision() {
        QCOMPARE(chartKrwAmount(309810.556), QString("309,811"));
        QCOMPARE(chartKrwAmount(1234.5), QString("1,235"));
        QCOMPARE(chartKrwAmount(12.5), QString("12.50"));
        QCOMPARE(chartKrwAmount(0.16634), QString("0.1663"));
        QCOMPARE(chartKrwAmount(0.00001234), QString("0.00001234"));
        QVERIFY(chartKrwAmount(0).isEmpty()); QVERIFY(chartKrwAmount(-1).isEmpty());
    }

    // The header row keeps its height: the won value and related markets share the price
    // line, the related list drops trailing items instead of clipping, and context for
    // another chart is never shown.
    void chartHeaderShowsContextOnlyForItsChart() {
        PublicTransport transport;
        MarketCandles source(&transport, std::unique_ptr<PublicCandleSocket>{}, [] { return Now; });
        ChartWorkspace workspace(&source, nullptr);
        workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace));
        workspace.indicatorPreferences()->markIntroSeen();
        auto* lists = workspace.watchlistModel();
        const auto list = lists->createList(QStringLiteral("공개 관심"));
        QVERIFY(lists->addIdentity(list, {"coin", "upbit", "KRW-SYN"}));
        workspace.setIdentity("coin:upbit:KRW-SYN"); workspace.setTimeframe(60000);
        workspace.resize(2400, 790); workspace.show(); QTRY_VERIFY(transport.token);
        QCoreApplication::processEvents();
        auto* price = workspace.findChild<QLabel*>("chartCurrentPrice");
        auto* timeframes = workspace.findChild<QWidget*>("chartWorkspaceTimeframeButtons");
        auto* conversion = workspace.findChild<QLabel*>("chartPriceConversion");
        auto* related = workspace.findChild<QLabel*>("chartRelatedMarkets");
        QVERIFY(price && timeframes && conversion && related);
        QVERIFY(conversion->isHidden()); QVERIFY(related->isHidden());
        const auto priceAt = price->mapTo(&workspace, QPoint()), timeframesAt = timeframes->mapTo(&workspace, QPoint());
        ChartMarketContext context;
        context.identityKey = "coin:upbit:KRW-SYN";
        context.conversion = QStringLiteral("바이낸스 1,234,000원"); context.premium = QStringLiteral("김프 +0.20%");
        context.conversionDetail = QStringLiteral("합성 환산 설명"); context.premiumDetail = QStringLiteral("합성 김프 설명");
        context.related = {{QStringLiteral("나스닥 100"), {}, "-1.38%", QStringLiteral("나스닥 설명"), MovementDirection::Down, false, false},
                           {QStringLiteral("달러 인덱스"), "101.25", "-0.60%", QStringLiteral("달러 설명"), MovementDirection::Down, false, false},
                           {QStringLiteral("원/달러"), "1,346.30", "+0.28%", QStringLiteral("원 설명"), MovementDirection::Up, false, false},
                           {QStringLiteral("코스피"), {}, "-5.28%", QStringLiteral("코스피 설명"), MovementDirection::Down, true, false},
                           {"S&P 500", {}, "-0.18%", QStringLiteral("S&P 설명"), MovementDirection::Down, false, false}};
        workspace.setMarketContext(context);
        QTRY_VERIFY(related->isVisible() && related->width() > 0);
        QCoreApplication::processEvents();
        QVERIFY(conversion->isVisible());
        QCOMPARE(conversion->accessibleName(), QStringLiteral("바이낸스 1,234,000원 · 김프 +0.20%"));
        QVERIFY(conversion->toolTip().contains(QStringLiteral("합성 김프 설명")));
        QCOMPARE(price->mapTo(&workspace, QPoint()), priceAt);
        QCOMPARE(timeframes->mapTo(&workspace, QPoint()), timeframesAt);
        auto* toolbar=workspace.findChild<QWidget*>("chartToolbar");QVERIFY(toolbar);
        auto* title=workspace.findChild<QLabel*>("marketCandlesTitle");QVERIFY(title);
        const QRect titleRect(title->mapTo(toolbar,QPoint()),title->size()),priceRect(price->mapTo(toolbar,QPoint()),price->size());
        QVERIFY(titleRect.right()<=priceRect.left());QVERIFY(qAbs(titleRect.center().y()-priceRect.center().y())<=2);
        QVERIFY(toolbar->rect().contains(QRect(timeframes->mapTo(toolbar,QPoint()),timeframes->size())));
        const auto shownWide = related->accessibleName().split(QStringLiteral(", "), Qt::SkipEmptyParts);
        QVERIFY(!shownWide.isEmpty()); QVERIFY(shownWide.first().startsWith(QStringLiteral("나스닥 100")));
        QVERIFY2(related->sizeHint().width() <= related->width(), "Shown related markets fit without clipping");
        const QRect conversionRect(conversion->mapTo(&workspace, QPoint()), conversion->size()), relatedRect(related->mapTo(&workspace, QPoint()), related->size());
        QVERIFY(!conversionRect.intersects(QRect(relatedRect.topLeft() + QPoint(related->width() - related->sizeHint().width(), 0), related->sizeHint())));
        const auto out = qEnvironmentVariable("KEEFI_CHART_CONTEXT_RENDER_DIR");
        if (!out.isEmpty()) {
            QVERIFY(QDir().mkpath(out));
            for (const bool dark : {false, true}) {
                KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
                workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace)); QCoreApplication::processEvents();
                QVERIFY(workspace.findChild<QWidget*>("chartWorkspaceChartPane")->grab(QRect(0, 0, 1100, 120))
                    .save(QDir(out).filePath(dark ? "chart-header-context-dark.png" : "chart-header-context-light.png")));
            }
        }
        workspace.resize(1440, 790);
        QTRY_COMPARE(toolbar->property("rows").toInt(), 1);
        QVERIFY(toolbar->height() <= 44);
        QVERIFY(related->isHidden());
        auto* info = toolbar->findChild<InfoButton*>("marketCandlesInformation");
        QVERIFY(info && info->isVisible());
        info->click();
        auto* dialog = info->findChild<QDialog*>("infoDialog");
        auto* body = info->findChild<QPlainTextEdit*>("infoPublicText");
        QVERIFY(dialog && dialog->isVisible() && body && body->isVisible());
        const auto text = body->toPlainText();
        for (const auto& item : context.related) {
            QVERIFY(text.contains(item.label));
            if (!item.value.isEmpty()) QVERIFY(text.contains(item.value));
            QVERIFY(text.contains(item.change));
            QVERIFY(text.contains(item.detail));
        }
        dialog->reject();

        // A narrower window shows fewer whole items, never a cut one.
        workspace.resize(1100, 790);
        QTRY_VERIFY(related->accessibleName().split(QStringLiteral(", "), Qt::SkipEmptyParts).size() <= shownWide.size());
        QCoreApplication::processEvents();
        QVERIFY2(related->sizeHint().width() <= related->width() || related->accessibleName().isEmpty(), "Narrow related markets fit without clipping");
        // Context of another chart is never drawn over this one.
        context.identityKey = "coin:upbit:KRW-OTHER";
        workspace.setMarketContext(context);
        QVERIFY(conversion->isHidden()); QVERIFY(related->isHidden());
    }

    // Opt-in: KEEFI_CHART_CONTEXT_LIVE_DIR=<dir> draws the Upbit BTC chart from real public
    // candles and fills its header from the real Upbit/Binance tickers and the world board.
    void liveChartHeaderWhenRequested() {
        const auto out = qEnvironmentVariable("KEEFI_CHART_CONTEXT_LIVE_DIR");
        if (out.isEmpty()) QSKIP("Set KEEFI_CHART_CONTEXT_LIVE_DIR to draw the chart header from real public sources.");
        QVERIFY(QDir().mkpath(out));
        NetworkTradingTransport candles, upbit, binance, fx, quotes, ecb, mof;
        MarketCandles source(&candles, std::unique_ptr<PublicCandleSocket>{});
        ChartWorkspace workspace(&source, nullptr);
        workspace.indicatorPreferences()->markIntroSeen();
        auto* lists = workspace.watchlistModel();
        const auto list = lists->createList(QStringLiteral("공개 관심"));
        QVERIFY(lists->addIdentity(list, {"coin", "upbit", "KRW-BTC"}));
        workspace.setIdentity("coin:upbit:KRW-BTC"); workspace.setTimeframe(900000);
        workspace.resize(1440, 790); workspace.show();
        TodayBtcPremium tickers(&upbit, &binance, &fx); tickers.setActive(true);
        WorldMarkets world(&quotes, &ecb, &mof); world.setDemand(WorldMarketsDemand::chartHeader());
        const auto ready = [&] {
            const auto t = tickers.snapshot(); const auto w = world.snapshot();
            return !t.upbit.price.isEmpty() && !t.binance.price.isEmpty() && w.quote(WorldMarketItem::UsdKrw).available()
                && w.quote(WorldMarketItem::Ndx).available() && w.dollarIndex.available() && source.series().available;
        };
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 90000);
        const auto t = tickers.snapshot();
        ChartMarketInputs in;
        in.market = "coin"; in.source = "upbit"; in.code = "KRW-BTC"; in.price = t.upbit.price; in.currency = "KRW";
        in.upbitKrw = t.upbit.price; in.upbitAtMs = t.upbit.evidence.sourceTimeMs;
        in.binanceUsdt = t.binance.price; in.binanceAtMs = t.binance.evidence.sourceTimeMs;
        in.world = world.snapshot(); in.nowMs = QDateTime::currentMSecsSinceEpoch();
        const auto context = chartMarketContext(in);
        workspace.setMarketContext(context);
        QStringList related;
        for (const auto& item : context.related) related.append(item.label + " " + item.value + " " + item.change + (item.closed ? " (closed)" : ""));
        qInfo().noquote() << QString("CHART_LIVE upbit=%1 binance=%2 conversion=%3 premium=%4 related=%5")
            .arg(t.upbit.price, t.binance.price, context.conversion, context.premium, related.join(" | "));
        QVERIFY(!context.conversion.isEmpty()); QVERIFY(!context.premium.isEmpty());
        for (const bool dark : {false, true}) {
            KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
            workspace.setStyleSheet(KeefiTheme::localStyleSheet(&workspace)); QCoreApplication::processEvents(); QTest::qWait(200);
            QVERIFY(workspace.grab().save(QDir(out).filePath(dark ? "live-chart-btc-dark.png" : "live-chart-btc-light.png")));
        }
    }

    void populatedEtherHeaderFitsTheSingleRowAt1440() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile.filePath("system"));
        Vault vault(profile.filePath("synthetic-empty-vault"));
        AccountModel accounts(&vault);
        PublicTransport transport;
        PublicMarketCatalog catalogue(&transport, nullptr, [] { return Now; });
        catalogue.refresh();
        QVERIFY(transport.token);
        HttpResult listing;
        listing.status = 200;
        listing.body = R"([{"market":"KRW-ETH","korean_name":"이더리움","english_name":"Ethereum"}])";
        emit transport.finished(transport.token, listing);
        QVERIFY(catalogue.marketListAvailable());

        PortfolioController::Factories factories;
        factories.trading = [](const AccountRecord&) { return static_cast<TradingTransport*>(nullptr); };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        LocalWindow window(vault, accounts);
        window.setPortfolioController(&portfolio);
        window.showVaultGate();
        window.resize(1440, 900);
        window.show();
        auto* browse = window.findChild<QPushButton*>("vaultGateBrowse");
        QVERIFY(browse && browse->isVisible());
        browse->click();
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        QVERIFY(pages);
        // Use the real window's chart page width with controlled public context.
        ChartWorkspace workspace(nullptr, nullptr, pages);
        pages->addTab(&workspace, QStringLiteral("차트"));
        pages->setCurrentWidget(&workspace);
        workspace.indicatorPreferences()->markIntroSeen();
        workspace.setPublicNameSources(&catalogue, nullptr, nullptr);
        const WatchlistIdentity eth{"coin", "upbit", "KRW-ETH"};
        QVERIFY(workspace.setIdentity(eth.key()));
        WatchlistRowState quote;
        quote.price = "4745000";
        quote.quoteCurrency = "KRW";
        quote.change = "-2.56%";
        workspace.watchlistModel()->setRowState(eth, quote);

        ChartMarketContext context;
        context.identityKey = eth.key();
        context.conversion = QStringLiteral("바이낸스 4,718,963원");
        context.premium = QStringLiteral("김프 +0.55%");
        context.conversionDetail = QStringLiteral("합성 이더리움 환산 설명");
        context.premiumDetail = QStringLiteral("합성 이더리움 김프 설명");
        context.related = {{QStringLiteral("나스닥 100"), {}, "-1.38%", QStringLiteral("합성 나스닥 설명")}};

        auto* toolbar = workspace.findChild<QWidget*>("chartToolbar");
        auto* title = workspace.findChild<QLabel*>("marketCandlesTitle");
        auto* price = workspace.findChild<QLabel*>("chartCurrentPrice");
        auto* unit = workspace.findChild<QLabel*>("chartCurrentPriceUnit");
        auto* change = workspace.findChild<QLabel*>("chartCurrentPriceReference");
        auto* conversion = workspace.findChild<QLabel*>("chartPriceConversion");
        auto* related = workspace.findChild<QLabel*>("chartRelatedMarkets");
        QVERIFY(toolbar && title && price && unit && change && conversion && related);
        QTRY_VERIFY(toolbar->isVisible());
        QCOMPARE(window.width(), 1440);
        QVERIFY(toolbar->width() < window.width());
        QVERIFY(title->accessibleName().contains(QStringLiteral("이더리움")));
        QCOMPARE(price->text(), QStringLiteral("4,745,000"));
        QCOMPARE(unit->text(), QStringLiteral("KRW"));
        QVERIFY(change->isVisible() && change->text().contains(quote.change));
        QTRY_COMPARE(toolbar->property("rows").toInt(), 1);
        QVERIFY(conversion->isHidden());
        // Public conversion arrives after the initial price-only layout.
        workspace.setMarketContext(context);
        QTRY_COMPARE(toolbar->property("rows").toInt(), 1);
        QVERIFY(toolbar->height() <= 44);
        QVERIFY(related->isHidden());
        QVERIFY(conversion->isHidden());

        auto* info = toolbar->findChild<InfoButton*>("marketCandlesInformation");
        QVERIFY(info && info->isVisible());
        info->click();
        auto* dialog = info->findChild<QDialog*>("infoDialog");
        auto* body = info->findChild<QPlainTextEdit*>("infoPublicText");
        QVERIFY(dialog && dialog->isVisible() && body && body->isVisible());
        const auto text = body->toPlainText();
        QVERIFY(text.contains(context.conversion));
        QVERIFY(text.contains(context.premium));
        QVERIFY(text.contains(context.conversionDetail));
        QVERIFY(text.contains(context.premiumDetail));
        dialog->reject();

        window.resize(1024, 700);
        QTRY_COMPARE(window.width(), 1024);
        QVERIFY(toolbar->height() <= 96);
        window.resize(2400, 900);
        QTRY_VERIFY(conversion->isVisible() && related->isVisible());
        QCOMPARE(conversion->accessibleName(), context.conversion + QStringLiteral(" · ") + context.premium);
        QVERIFY(conversion->toolTip().contains(context.conversionDetail));
        QVERIFY(conversion->toolTip().contains(context.premiumDetail));
    }
};

QTEST_MAIN(ChartMarketContextTest)
#include "tst_chart_market_context.moc"
