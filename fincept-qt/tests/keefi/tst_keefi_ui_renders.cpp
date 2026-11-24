// KEEFI_KEEFI_RENDER_DIR selects a new directory for synthetic, offscreen PNGs.
#include "keefi/AccountDialog.h"
#include "keefi/AccountModel.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/EarningsCalendar.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/InfoButton.h"
#include "keefi/LocalWindow.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCompareView.h"
#include "keefi/MarketMetrics.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/OfficialNotices.h"
#include "keefi/PortfolioController.h"
#include "keefi/ProfileMigrationDialog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicMarketHistory.h"
#include "keefi/TradingTransport.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/Vault.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "fixtures/ProviderReadFixture.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <memory>

using namespace keefi;

namespace {
const QString Identity = QStringLiteral("coin:upbit:KRW-BTC");

class SyntheticTransport final : public TradingTransport {
  public:
    explicit SyntheticTransport(bool panelCatalog = false) : panelCatalog_(panelCatalog) {}
    void abort() override { ++generation_; }

    void send(quint64 token, const RequestSpec& request) override {
        if (request.authenticated) qFatal("Render fixture received an authenticated request");
        const auto generation = generation_;
        QTimer::singleShot(0, this, [this, token, generation, url = request.url] {
            if (generation != generation_) return;
            HttpResult result;
            result.status = 200;
            result.body = "[]";
            if (url.host() == "api.upbit.com" && url.path() == "/v1/market/all") {
                result.body = panelCatalog_ ? panelMarkets() :
                    QByteArray(R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin"}])");
            } else if (panelCatalog_ && url.host() == "api.upbit.com" && url.path() == "/v1/ticker") {
                result.body = panelQuotes();
            } else if (panelCatalog_ && url.host() == "api.binance.com" && url.path() == "/api/v3/exchangeInfo") {
                result.body = panelBinanceMarkets();
            } else if (panelCatalog_ && url.host() == "api.binance.com" && url.path() == "/api/v3/ticker/24hr") {
                result.body = panelBinanceQuotes();
            } else if (url.host() == "api.upbit.com" && url.path().startsWith("/v1/candles/minutes/")) {
                const auto unit = url.path().section('/', -1).toLongLong();
                if (unit > 0) result.body = candleRows(QUrlQuery(url).queryItemValue("market"), unit);
            } else if (url.host() == "fapi.binance.com" && url.path() == "/fapi/v1/exchangeInfo") {
                result.body = R"({"symbols":[{"symbol":"XAUUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},)"
                              R"({"symbol":"CLUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"COMMODITY"},)"
                              R"({"symbol":"QQQUSDT","contractType":"TRADIFI_PERPETUAL","status":"TRADING","underlyingType":"EQUITY"}]})";
            } else if ((url.host() == "fapi.binance.com" || url.host() == "api.binance.com") &&
                       url.path().endsWith("/klines")) {
                const QUrlQuery query(url);
                if (query.queryItemValue("interval") == "1h")
                    result.body = comparisonRows(query.queryItemValue("symbol"));
            }
            emit finished(token, result);
        });
    }

  private:
    static QByteArray panelMarkets() {
        return R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin"},)"
               R"({"market":"KRW-ETH","korean_name":"이더리움","english_name":"Ethereum"},)"
               R"({"market":"KRW-SOL","korean_name":"솔라나","english_name":"Solana"},)"
               R"({"market":"KRW-XRP","korean_name":"엑스알피","english_name":"XRP"},)"
               R"({"market":"KRW-DOGE","korean_name":"도지코인 합성 긴 이름 말줄임 확인용","english_name":"Dogecoin"}])";
    }

    static QByteArray panelQuotes() {
        const auto now = QString::number(QDateTime::currentMSecsSinceEpoch() - 100);
        QJsonArray rows;
        for (const auto& row : QList<QStringList>{{"KRW-BTC", "103450000", "0.0234", "523400000000", "5060"},
                                                 {"KRW-ETH", "4128000", "-0.0126", "214700000000", "52010"},
                                                 {"KRW-SOL", "248300", "0.0512", "153200000000", "616900"},
                                                 {"KRW-XRP", "3241", "0.0000", "119300000000", "36809000"},
                                                 {"KRW-DOGE", "401.7", "-0.0042", "57600000000", "143390000"}}) {
            rows.append(QJsonObject{{"market", row[0]}, {"trade_price", row[1]}, {"prev_closing_price", row[1]},
                                    {"signed_change_rate", row[2]}, {"timestamp", now},
                                    {"acc_trade_price_24h", row[3]}, {"acc_trade_volume_24h", row[4]}});
        }
        return QJsonDocument(rows).toJson(QJsonDocument::Compact);
    }

    static QByteArray panelBinanceMarkets() {
        QJsonArray symbols;
        for (const auto& base : QStringList{"BTC", "ETH", "SOL", "XRP", "DOGE"}) {
            symbols.append(QJsonObject{{"symbol", base + "USDT"}, {"baseAsset", base},
                {"quoteAsset", "USDT"}, {"status", "TRADING"}, {"isSpotTradingAllowed", true}});
        }
        return QJsonDocument(QJsonObject{{"symbols", symbols}}).toJson(QJsonDocument::Compact);
    }

    static QByteArray panelBinanceQuotes() {
        QJsonArray rows;
        const auto now = QDateTime::currentMSecsSinceEpoch() - 100;
        for (const auto& row : QList<QStringList>{{"BTCUSDT", "71500", "2.34", "523400000", "7320"},
                                                 {"ETHUSDT", "2850", "-1.26", "214700000", "75300"},
                                                 {"SOLUSDT", "171.6", "5.12", "153200000", "892700"},
                                                 {"XRPUSDT", "2.24", "0.00", "119300000", "53250000"},
                                                 {"DOGEUSDT", "0.278", "-0.42", "57600000", "207190000"}}) {
            rows.append(QJsonObject{{"symbol", row[0]}, {"lastPrice", row[1]},
                {"priceChangePercent", row[2]}, {"quoteVolume", row[3]}, {"volume", row[4]},
                {"closeTime", now}, {"count", 100}});
        }
        return QJsonDocument(rows).toJson(QJsonDocument::Compact);
    }

    static QByteArray comparisonRows(const QString& symbol) {
        constexpr qint64 Hour = 3600000;
        const auto end = QDateTime::currentMSecsSinceEpoch() / Hour * Hour;
        const double phase = symbol == "XAUUSDT" ? 0 : symbol == "CLUSDT" ? 1 : symbol == "QQQUSDT" ? 2 : 3;
        QJsonArray rows;
        for (int i = 0; i < 200; ++i) {
            const auto at = end - (200 - i) * Hour;
            const auto value = QString::number(100 + 0.06 * i * (phase - 1) + 3 * std::sin(i * .17 + phase), 'f', 2);
            rows.append(QJsonArray{at, value, value, value, value, "1", at + Hour - 1,
                                  "1", 1, "1", "1", "0"});
        }
        return QJsonDocument(rows).toJson(QJsonDocument::Compact);
    }

    static QByteArray candleRows(const QString& market, qint64 minutes) {
        const auto interval = minutes * 60000;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto end = now / interval * interval;
        QJsonArray rows;
        for (int i = 0; i < 200; ++i) {
            const auto at = end - (i + 1) * interval;
            const auto close = 110000000 + (199 - i) * 96000 + std::sin(i * .35) * 1600000;
            const auto open = close + std::cos(i * .47) * 680000;
            rows.append(QJsonObject{
                {"market", market},
                {"candle_date_time_utc", QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC)
                    .toString("yyyy-MM-ddTHH:mm:ss")},
                {"timestamp", QString::number(at + 1000)}, {"unit", QString::number(minutes)},
                {"opening_price", open}, {"high_price", qMax(open, close) + 320000},
                {"low_price", qMin(open, close) - 260000}, {"trade_price", close},
                {"candle_acc_trade_volume", 1.0 + i % 8}
            });
        }
        return QJsonDocument(rows).toJson(QJsonDocument::Compact);
    }

    bool panelCatalog_ = false;
    quint64 generation_ = 0;
};

// Dev99 has its own public quote fixture; the original render and Dev98
// transports keep their response shapes and five-row catalogue unchanged.
class Dev99ExploreTransport final : public TradingTransport {
  public:
    bool omitEthereum = false;
    static QStringList symbols() {
        return {"BTC", "ETH", "BNB", "SOL", "XRP", "DOGE", "ADA", "AVAX",
                "DOT", "LINK", "TRX", "SUI", "NEAR", "ATOM", "LTC", "BCH"};
    }
    void abort() override { ++generation_; }
    void send(quint64 token, const RequestSpec& request) override {
        if (request.authenticated) qFatal("Dev99 render fixture received an authenticated request");
        const auto generation = generation_;
        const auto url = request.url;
        const bool omit = omitEthereum;
        QTimer::singleShot(0, this, [this, token, generation, url, omit] {
            if (generation != generation_) return;
            HttpResult reply;
            reply.status = 200;
            QJsonArray rows;
            const auto ids = symbols();
            const QStringList names{"비트코인", "이더리움", "비앤비", "솔라나", "엑스알피", "도지코인",
                "에이다", "아발란체", "폴카닷", "체인링크", "트론", "수이", "니어프로토콜",
                "코스모스 합성 긴 이름 말줄임 확인용", "라이트코인", "비트코인캐시"};
            const QStringList sourceIds{"bitcoin", "ethereum", "binancecoin", "solana", "ripple", "dogecoin",
                "cardano", "avalanche-2", "polkadot", "chainlink", "tron", "sui", "near", "cosmos",
                "litecoin", "bitcoin-cash"};
            const QList<double> prices{103450000, 4128000, 847500, 248300, 3241, 401.7, 1234, 54100,
                8700, 25400, 326, 4820, 7100, 8900, 143200, 637000};
            const QList<double> changes{2.34, -1.70, 0.56, 5.12, 0.0, -0.42, 1.08, -2.36,
                0.76, 1.89, -0.12, 4.15, -1.24, 0.33, -0.85, 2.16};
            const auto now = QString::number(QDateTime::currentMSecsSinceEpoch() - 100);
            for (int i = 0; i < ids.size(); ++i) {
                if (url.host() == "api.upbit.com" && url.path() == "/v1/market/all") {
                    rows.append(QJsonObject{{"market", "KRW-" + ids[i]}, {"korean_name", names[i]},
                        {"english_name", "Synthetic " + ids[i]}});
                } else if (url.host() == "api.upbit.com" && url.path() == "/v1/ticker") {
                    rows.append(QJsonObject{{"market", "KRW-" + ids[i]}, {"trade_price", QString::number(prices[i], 'f', 1)},
                        {"prev_closing_price", QString::number(prices[i], 'f', 1)}, {"signed_change_rate", changes[i] / 100.0},
                        {"timestamp", now}, {"acc_trade_price_24h", QString::number(523400000000.0 / (i + 1), 'f', 0)},
                        {"acc_trade_volume_24h", QString::number(5000.0 * (i + 1), 'f', 0)}});
                } else if (url.host() == "api.coingecko.com") {
                    if (omit && ids[i] == "ETH") continue;
                    rows.append(QJsonObject{{"id", sourceIds[i]}, {"symbol", ids[i].toLower()}, {"name", names[i]},
                        {"market_cap", 2000000000000.0 - i * 50000000000.0}, {"current_price", prices[i]},
                        {"price_change_percentage_24h", changes[i]}, {"total_volume", 523400000000.0 / (i + 1)}});
                } else if (url.host() == "m.stock.naver.com") {
                    rows.append(QJsonObject{{"itemCode", QString::number(5930 + i).rightJustified(6, '0')},
                        {"stockName", QStringLiteral("합성 한국 %1").arg(i + 1)}, {"closePrice", QString::number(200000 + i * 500)},
                        {"fluctuationsRatio", QString::number(changes[i], 'f', 2)},
                        {"compareToPreviousPrice", QJsonObject{{"code", changes[i] < 0 ? "5" : "2"}}},
                        {"marketValue", QString::number(13000000 - i * 300000)}});
                } else if (url.host() == "api.nasdaq.com") {
                    rows.append(QJsonObject{{"symbol", QString("US") + QChar('A' + i)},
                        {"name", QString("Synthetic US %1").arg(i + 1)}, {"lastsale", QString("$%1.25").arg(210 + i * 12)},
                        {"pctchange", QString::number(changes[i], 'f', 2) + "%"},
                        {"marketCap", QString::number(5000000000000LL - i * 50000000000LL)}, {"sector", "Technology"}});
                }
            }
            if (url.host() == "m.stock.naver.com")
                reply.body = QJsonDocument(QJsonObject{{"marketStatus", "CLOSE"}, {"stocks", rows}}).toJson();
            else if (url.host() == "api.nasdaq.com")
                reply.body = QJsonDocument(QJsonObject{{"data", QJsonObject{{"rows", rows}}}}).toJson();
            else
                reply.body = QJsonDocument(rows).toJson();
            emit finished(token, reply);
        });
    }
  private:
    quint64 generation_ = 0;
};

// Public schedules and relevant notices for the Dev99 Today captures only.
// Every model owns a separate instance so one model's cancellation stays local.
class Dev99TodayTransport final : public TradingTransport {
  public:
    explicit Dev99TodayTransport(qint64 now) : now_(now) {}
    void abort() override { ++generation_; }
    void send(quint64 token, const RequestSpec& request) override {
        if (request.authenticated || (!OfficialCalendar::isRequest(request) &&
            !OfficialNotices::isRequest(request) && !EarningsCalendar::isRequest(request)))
            qFatal("Dev99 Today fixture received a non-public or unexpected request");
        HttpResult reply;
        reply.status = 200;
        const auto host = request.url.host();
        const auto today = QDateTime::fromMSecsSinceEpoch(now_, QTimeZone("Asia/Seoul")).date();
        if (host == "www.bls.gov" || host == "www.bea.gov") {
            const bool bls = host == "www.bls.gov";
            const QStringList titles = bls ? QStringList{"Consumer Price Index", "Employment Situation", "Producer Price Index"}
                                          : QStringList{"Gross Domestic Product"};
            QByteArray body = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n";
            for (int i = 0; i < titles.size(); ++i) {
                const QDateTime starts(today.addDays(bls ? 0 : 1), QTime(bls ? 9 + i * 5 : 10, 0), QTimeZone("Asia/Seoul"));
                body += QStringLiteral("BEGIN:VEVENT\r\nUID:dev99-%1-%2\r\nSUMMARY:%3\r\nDTSTART:%4\r\nDTSTAMP:%5\r\nSEQUENCE:0\r\nEND:VEVENT\r\n")
                    .arg(bls ? "bls" : "bea").arg(i).arg(titles[i], starts.toUTC().toString("yyyyMMdd'T'HHmmss'Z'"),
                        QDateTime::fromMSecsSinceEpoch(now_ - 86400000, QTimeZone::UTC).toString("yyyyMMdd'T'HHmmss'Z'")).toUtf8();
            }
            reply.body = body + "END:VCALENDAR\r\n";
        } else if (host == "www.federalreserve.gov") {
            const auto day = QDateTime::fromMSecsSinceEpoch(now_, QTimeZone("America/New_York")).date();
            const int first = qMin(day.day(), 27);
            reply.body = QStringLiteral("<h4><a>%1 FOMC Meetings</a></h4><div class=\"fomc-meeting__month\"><strong>%2</strong></div>"
                "<div class=\"fomc-meeting__date\">%3-%4</div>")
                .arg(day.year()).arg(QLocale(QLocale::English).monthName(day.month())).arg(first).arg(first + 1).toUtf8();
        } else if (host == "www.bok.or.kr") {
            reply.body = QStringLiteral("<h3>%1년</h3><table id=\"tableId\"><tbody><tr><th scope=\"row\">%2(%3)</th></tr></tbody></table>")
                .arg(today.year()).arg(today.toString("MM월 dd일"), QString(QStringLiteral("월화수목금토일").at(today.dayOfWeek() - 1))).toUtf8();
        } else if (host == "kind.krx.co.kr") {
            reply.body = QStringLiteral("전체 <em>0</em>건<table><tbody></tbody></table>").toUtf8();
        } else if (host == "api.nasdaq.com") {
            const auto date = QDate::fromString(QUrlQuery(request.url).queryItemValue("date"), Qt::ISODate);
            const auto nyToday = QDateTime::fromMSecsSinceEpoch(now_, QTimeZone("America/New_York")).date();
            QJsonArray rows;
            if (date == nyToday) for (int i = 0; i < 2; ++i)
                rows.append(QJsonObject{{"symbol", i == 0 ? "SYN1" : "SYN2"},
                    {"name", i == 0 ? "Synthetic Calendar Alpha" : "Synthetic Calendar Beta"},
                    {"time", i == 0 ? "time-pre-market" : "time-after-hours"}, {"marketCap", "$300,000,000,000"},
                    {"fiscalQuarterEnding", QLocale(QLocale::English).toString(date.addMonths(-1), "MMM/yyyy")},
                    {"epsForecast", "$1.25"}, {"noOfEsts", "12"}});
            reply.body = QJsonDocument(QJsonObject{{"data", QJsonObject{
                {"asOf", QLocale(QLocale::English).toString(date, "ddd, MMM d, yyyy")}, {"rows", rows}}},
                {"status", QJsonObject{{"rCode", 200}}}}).toJson(QJsonDocument::Compact);
        } else if (host == "openapi.okx.com") {
            QJsonArray rows;
            const QStringList titles{QStringLiteral("합성 자산 입출금 일시 중단 안내"), QStringLiteral("합성 자산 거래지원 종료 안내")};
            for (int i = 0; i < titles.size(); ++i)
                rows.append(QJsonObject{{"annType", i == 0 ? "announcements-deposit-suspension" : "announcements-delisting"},
                    {"title", titles[i]}, {"url", QStringLiteral("https://www.okx.com/help/synthetic-dev99-notice-%1").arg(i)},
                    {"pTime", QString::number(now_ - (i + 1) * 3600000)}});
            reply.body = QJsonDocument(QJsonObject{{"code", "0"}, {"data", QJsonArray{QJsonObject{{"details", rows}}}}})
                .toJson(QJsonDocument::Compact);
        } else {
            reply.body = QJsonDocument(QJsonArray{QJsonObject{{"categories", QJsonArray{QStringLiteral("거래유의")}},
                {"title", QStringLiteral("합성 자산 거래유의 지정 안내")}, {"pc_url", "https://feed.bithumb.com/notice/99001"},
                {"published_at", today.toString("yyyy-MM-dd") + " 09:00:00"}, {"modified_at", ""}}})
                .toJson(QJsonDocument::Compact);
        }
        const auto generation = generation_;
        QTimer::singleShot(0, this, [this, token, generation, reply] {
            if (generation == generation_) emit finished(token, reply);
        });
    }
  private:
    qint64 now_ = 0;
    quint64 generation_ = 0;
};

class Dev99CompareTransport final : public TradingTransport {
  public:
    explicit Dev99CompareTransport(MarketCompareSource::Clock clock) : clock_(std::move(clock)) {}
    QList<RequestSpec> sent;
    void abort() override { ++generation_; }
    void send(quint64 token, const RequestSpec& request) override {
        if (request.authenticated || !MarketCompare::isRequest(request))
            qFatal("Dev99 comparison fixture received a non-public or unexpected request");
        sent.append(request);
        HttpResult reply;
        reply.status = 200;
        if (request.url.path().endsWith("/exchangeInfo")) {
            QJsonArray symbols;
            for (const auto& spec : MarketCompare::defaultSpecs())
                if (spec.venue == CompareVenue::BinanceUsdM)
                    symbols.append(QJsonObject{{"symbol", spec.symbol}, {"contractType", "TRADIFI_PERPETUAL"},
                        {"status", "TRADING"}, {"underlyingType", spec.underlyingType}});
            reply.body = QJsonDocument(QJsonObject{{"symbols", symbols}}).toJson(QJsonDocument::Compact);
        } else {
            const QUrlQuery query(request.url);
            qint64 interval = 0;
            for (qint64 candidate : {60000LL, 300000LL, 900000LL, 3600000LL, 14400000LL, 86400000LL})
                if (MarketCompare::wireInterval(candidate) == query.queryItemValue("interval")) interval = candidate;
            if (!interval || query.queryItemValue("limit") != "200")
                qFatal("Dev99 comparison fixture requires a known interval and a 200-bar page");
            const auto symbol = query.queryItemValue("symbol");
            const int phase = symbol == "XAUUSDT" ? 0 : symbol == "CLUSDT" ? 1 : symbol == "QQQUSDT" ? 2 : 3;
            const double slope[] = {-0.13, -0.10, 0.03, -0.04};
            const auto endExclusive = clock_() / interval * interval;
            QJsonArray rows;
            for (int i = 0; i < 200; ++i) {
                const auto open = endExclusive - (200 - i) * interval;
                const auto close = QString::number(100 + slope[phase] * i + 2 * std::sin(i * .17 + phase), 'f', 2);
                rows.append(QJsonArray{open, close, close, close, close, "1", open + interval - 1, "1", 1, "1", "1", "0"});
            }
            reply.body = QJsonDocument(rows).toJson(QJsonDocument::Compact);
        }
        const auto generation = generation_;
        QTimer::singleShot(0, this, [this, token, generation, reply] {
            if (generation == generation_) emit finished(token, reply);
        });
    }
  private:
    MarketCompareSource::Clock clock_;
    quint64 generation_ = 0;
};

bool capture(QWidget& window, QSize size, const QString& destination,
             std::initializer_list<QDialog*> dialogs = {}) {
    QCoreApplication::processEvents();
    if (window.size() != size) return false;
    if(qEnvironmentVariableIsSet("KEEFI_KEEFI_MEASURE_ONLY"))return true;
    auto picture = window.grab();
    if (picture.isNull()) return false;
    QPainter painter(&picture);
    // QWidget::grab excludes separate dialog windows. Center the actual Qt
    // dialog renders on their parent frame so the exported canvas stays exact.
    for (auto* dialog : dialogs) {
        if (!dialog || !dialog->isVisible() || dialog->width() > size.width()
            || dialog->height() > size.height()) return false;
        const QPoint center = window.rect().center() - dialog->rect().center();
        painter.drawPixmap(center, dialog->grab());
        painter.setPen(themeColors().rule);
        painter.drawRect(QRect(center, dialog->size()).adjusted(0, 0, -1, -1));
    }
    painter.end();
    if(qEnvironmentVariableIsSet("KEEFI_KEEFI_VALIDATE_ONLY"))return true;
    return picture.save(destination);
}

bool capturePanel(QWidget& window, QWidget& panel, QSize size, const QString& destination,
                  std::initializer_list<QWidget*> popups = {}) {
    QCoreApplication::processEvents();
    const QRect bounds(window.mapFromGlobal(panel.mapToGlobal(QPoint{})), panel.size());
    if (window.size() != size || !panel.isVisible() || !window.rect().contains(bounds)) return false;
    if (qEnvironmentVariableIsSet("KEEFI_KEEFI_MEASURE_ONLY")) return true;
    auto picture = window.grab();
    QPainter painter(&picture);
    painter.drawPixmap(bounds.topLeft(), panel.grab());
    // Popup menus are separate windows. Preserve their real placement on the
    // application canvas instead of inventing a menu position for the image.
    for (auto* popup : popups) {
        if (!popup || !popup->isVisible()) return false;
        const QRect popupBounds(window.mapFromGlobal(popup->mapToGlobal(QPoint{})), popup->size());
        if (!window.rect().contains(popupBounds)) return false;
        painter.drawPixmap(popupBounds.topLeft(), popup->grab());
    }
    painter.end();
    if (qEnvironmentVariableIsSet("KEEFI_KEEFI_VALIDATE_ONLY")) return true;
    return picture.save(destination);
}
}

class KeefiUiRenderProbe final : public QObject {
    Q_OBJECT
    QDir output_;

  private slots:
    void initTestCase() {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
        const QFileInfo destination(qEnvironmentVariable("KEEFI_KEEFI_RENDER_DIR"));
        QVERIFY2(destination.isAbsolute() && !destination.exists() && !destination.isSymLink(),
                 "KEEFI_KEEFI_RENDER_DIR must name a new absolute output directory");
        const QFileInfo parent(destination.absolutePath());
        QVERIFY(parent.isDir());
        QCOMPARE(parent.canonicalFilePath(), parent.absoluteFilePath());
        QVERIFY(QDir(parent.absoluteFilePath()).mkdir(destination.fileName()));
        output_ = QDir(destination.absoluteFilePath());
        QCoreApplication::setOrganizationName("KEEFI Synthetic KEEFI Render");
        QCoreApplication::setApplicationName("KEEFI Dev");
        QCoreApplication::setApplicationVersion("0.18.0");
        QVERIFY(KeefiFonts::registerBundledFonts());
        QApplication::setFont(KeefiFonts::font(FontRole::Body15));
    }

    void dev100Profiles_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("existing");
        QTest::newRow("copy-light-1440") << false << false;
        QTest::newRow("copy-dark-1440") << true << false;
        QTest::newRow("choose-light-1440") << false << true;
        QTest::newRow("choose-dark-1440") << true << true;
    }

    void dev100Profiles() {
        QFETCH(bool, dark);
        QFETCH(bool, existing);
        QTemporaryDir isolatedSettings;
        QVERIFY(isolatedSettings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolatedSettings.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, isolatedSettings.filePath("system"));
        KeefiTheme::instance()->setSettings(nullptr);
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        QWidget backdrop;
        backdrop.setAutoFillBackground(true);
        backdrop.setPalette(KeefiTheme::palette());
        backdrop.resize(1440, 900);
        backdrop.show();
        ProfileMigrationPlan plan;
        plan.action = existing ? DevProfileAction::ChooseExisting : DevProfileAction::OfferCopy;
        plan.paths.legacyDirectory = "/synthetic/Application Support/KEEFI/KEEFI Dev";
        plan.paths.currentDirectory = "/synthetic/Application Support/KEEFI/KEEFI Dev";
        ProfileMigrationDialog dialog(plan, &backdrop);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        const auto primaryName = existing ? "profileMigrationUseCurrent" : "profileMigrationCopy";
        const auto secondaryName = existing ? "profileMigrationUseLegacy" : "profileMigrationLater";
        for (const auto* name : {primaryName, secondaryName}) {
            auto* button = dialog.findChild<QPushButton*>(name);
            QVERIFY(button && button->isVisibleTo(&dialog));
            QVERIFY(dialog.rect().contains(QRect(button->mapTo(&dialog, QPoint{}), button->size())));
        }
        QVERIFY(capture(backdrop, QSize(1440, 900),
            output_.filePath(QStringLiteral("S-11-profile-%1.png").arg(QString::fromLatin1(QTest::currentDataTag()))),
            {&dialog}));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.choice(), ProfileMigrationChoice::Cancel);
        qInfo() << "synthetic Dev100 profile dialog" << (existing ? "choose" : "copy")
                << (dark ? "dark" : "light") << QSize(1440, 900);
    }

    void render_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QSize>("size");
        QTest::newRow("light-1440") << false << QSize(1440, 900);
        QTest::newRow("dark-1440") << true << QSize(1440, 900);
        QTest::newRow("light-1024") << false << QSize(1024, 700);
        QTest::newRow("dark-1024") << true << QSize(1024, 700);
        QTest::newRow("light-2000") << false << QSize(2000, 1100);
        QTest::newRow("dark-2000") << true << QSize(2000, 1100);
    }

    void render() {
        QFETCH(bool, dark);
        QFETCH(QSize, size);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile.filePath("system"));
        const auto path = [&](const QString& name) {
            return output_.filePath(QStringLiteral("%1-%2.png").arg(name, QString::fromLatin1(QTest::currentDataTag())));
        };

        Vault vault(QDir(QDir(profile.path()).canonicalPath()).filePath("synthetic-vault"));
        AccountModel accounts(&vault);
        SyntheticTransport candleWire, compareWire, metadataWire, catalogWire, metricsWire;
        MarketCandles candles(&candleWire, std::unique_ptr<PublicCandleSocket>{});
        MarketCompareSource compare(&compareWire);
        PublicMarketCatalog catalog(&catalogWire);
        MarketMetrics metrics(&metricsWire);
        test::ProviderReadTransport accountWire;
        accountWire.provider = "upbit";
        PortfolioController::Factories factories;
        factories.trading = [&](const AccountRecord&) { return &accountWire; };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.setPortfolioController(&portfolio);
        window.setPublicMarketCatalog(&catalog, false);
        window.setChartSettings(&settings);
        window.setChartInstrumentTransport(&metadataWire);
        window.setChartContractTransport(&metadataWire);
        window.setDeferredWorkbenchInitializer([&] {
            window.setMarketMetrics(&metrics);
            window.setMarketCandles(&candles);
            window.setMarketCompareSource(&compare);
        });
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        window.showVaultGate();
        window.resize(size);
        window.show();
        QVERIFY(capture(window, size, path("S-01-vault")));

        auto* browse = window.findChild<QPushButton*>("vaultGateBrowse");
        QVERIFY(browse && browse->isVisible());
        browse->click();
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        auto* workspace = window.findChild<ChartWorkspace*>();
        auto* comparison = window.findChild<MarketCompareView*>();
        QVERIFY(pages && workspace && comparison);
        catalog.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(catalog.verifiedMarkets().contains("KRW-BTC"), 3000);
        pages->setCurrentWidget(workspace);
        auto* preferences = workspace->indicatorPreferences();
        preferences->markIntroSeen();
        preferences->setEnabled("ma", true);
        preferences->setVisible("ma", true);
        preferences->setEnabled("bb", true);
        preferences->setVisible("bb", true);
        workspace->setTimeframe(900000);
        QVERIFY(workspace->setIdentity(Identity));
        QTRY_VERIFY_WITH_TIMEOUT(candles.series().available, 3000);
        auto* applied = window.findChild<QPushButton*>("chartStudyAppliedToggle");
        QVERIFY(applied);
        if (applied->isChecked()) applied->click();
        auto* chart = workspace->findChild<QWidget*>("marketCandlesChart");
        QVERIFY(chart && chart->isVisible());
        QCoreApplication::processEvents();
        QTest::qWait(1);
        auto* toolbar=workspace->findChild<QWidget*>("chartToolbar");QVERIFY(toolbar);
        qInfo()<<"synthetic toolbar geometry"<<(dark?"dark":"light")<<size<<toolbar->size()<<toolbar->property("rows")<<toolbar->property("gridFolded")<<toolbar->property("styleFolded")<<toolbar->property("timeframeFolded");
        qInfo() << "synthetic candle geometry, default" << (dark ? "dark" : "light") << size << chart->size();
        if(size.width()==1440){
            QVERIFY2(chart->height()>=500,"The default candle canvas must retain the Dev94 minimum");
            QCOMPARE(toolbar->property("rows").toInt(),1);
            QVERIFY(toolbar->height()<=44);
        }
        QVERIFY(toolbar->height()<=96);
        QVERIFY2(window.rect().contains(QRect(chart->mapTo(&window,QPoint{}),chart->size())),
                 "Dev95 must show the complete default candle canvas without scrolling");
        if (!applied->isChecked()) applied->click();
        // Include the first one-second refresh of the synthetic study summary.
        // Its populated text can change the expanded card height.
        QTest::qWait(1100);
        QVERIFY(capture(window, size, path("S-02-chart")));
        qInfo() << "synthetic candle geometry, applied studies open" << (dark ? "dark" : "light") << size << chart->size();

        if(qEnvironmentVariableIsSet("KEEFI_KEEFI_MEASURE_ONLY")){
            const auto* chartLayout=workspace->findChild<QWidget*>("chartWorkspaceChartPane")->layout();
            for(int i=0;i<chartLayout->count();++i)if(auto* widget=chartLayout->itemAt(i)->widget();widget&&widget->isVisible())qInfo()<<"chart row"<<widget->objectName()<<widget->size();
            const auto original=toolbar->size();QString last;
            for(int width=500;width<=1800;++width){
                toolbar->resize(width,toolbar->height());
                const auto state=QStringLiteral("%1/%2/%3/%4").arg(toolbar->property("rows").toInt()).arg(toolbar->property("gridFolded").toBool()).arg(toolbar->property("styleFolded").toBool()).arg(toolbar->property("timeframeFolded").toBool());
                if(state!=last){qInfo()<<"toolbar fold threshold, available width"<<width<<state;last=state;}
            }
            toolbar->resize(original);
            return;
        }

        auto* instrumentButton = workspace->findChild<QAbstractButton*>("chartInstrumentButton");
        auto* instrumentPanel = workspace->findChild<QWidget*>("chartInstrumentPanel");
        auto* instrumentSearch = workspace->findChild<QLineEdit*>("chartWorkspaceAsset");
        auto* wideList = workspace->findChild<QPushButton*>("chartBrowserExpand");
        QVERIFY(instrumentButton && instrumentPanel && instrumentSearch && wideList);
        // S-08 uses the default collapsed studies state. The expanded live
        // summary has its own data-driven height and is measured above.
        if(applied->isChecked())applied->click();
        QTest::qWait(1);
        const auto chartBeforePanel = chart->geometry();
        const QRect viewportBeforePanel(chart->mapTo(&window,QPoint{}),chart->size());
        QVERIFY(window.rect().contains(viewportBeforePanel));
        instrumentButton->click();
        QTRY_VERIFY(instrumentPanel->isVisible());
        QCOMPARE(instrumentPanel->width(),qMin(560,size.width()-32));
        QVERIFY(instrumentPanel->height()<=520);
        QVERIFY(capturePanel(window,*instrumentPanel,size,path("S-08-instruments")));
        QCOMPARE(chart->geometry(),chartBeforePanel);
        QCOMPARE(QRect(chart->mapTo(&window,QPoint{}),chart->size()),viewportBeforePanel);
        wideList->click();
        QTRY_COMPARE(instrumentPanel->width(),qMin(760,size.width()-32));
        QVERIFY(capturePanel(window,*instrumentPanel,size,path("S-08-instruments-wide")));
        QCOMPARE(chart->geometry(),chartBeforePanel);
        QCOMPARE(QRect(chart->mapTo(&window,QPoint{}),chart->size()),viewportBeforePanel);
        wideList->click();
        instrumentSearch->setText(QStringLiteral("no-synthetic-instrument-20261112"));
        auto* emptyResult = instrumentPanel->findChild<QWidget*>("chartInstrumentEmpty");
        QVERIFY(emptyResult);
        QTRY_VERIFY(emptyResult->isVisible());
        QVERIFY(capturePanel(window,*instrumentPanel,size,path("S-08-instruments-empty")));
        instrumentSearch->clear();
        QTest::keyClick(instrumentSearch,Qt::Key_Escape);
        QTRY_VERIFY(!instrumentPanel->isVisible());
        QTRY_COMPARE(QApplication::focusWidget(),instrumentButton);

        pages->setCurrentWidget(comparison);
        auto* won = comparison->findChild<QComboBox*>("marketCompareWonSelector");
        QVERIFY(won);
        const auto wonMode = won->findData(int(CompareWonMode::WonRaw));
        QVERIFY(wonMode >= 0);
        won->setCurrentIndex(wonMode);
        QVERIFY(QMetaObject::invokeMethod(won, "activated", Q_ARG(int, wonMode)));
        const auto allComparisonSeriesReady = [&] {
            const auto snapshot = compare.snapshot();
            return snapshot.series.size() == 4 && std::ranges::all_of(snapshot.series, &CompareSeries::available);
        };
        QTRY_VERIFY_WITH_TIMEOUT(allComparisonSeriesReady(), 5000);
        QCOMPARE(won->currentData().toInt(), int(CompareWonMode::WonRaw));
        QVERIFY(capture(window, size, path("S-03-compare")));

        pages->setCurrentWidget(workspace);
        auto* picker = window.findChild<QPushButton*>("chartStudyPicker");
        QVERIFY(picker && picker->isVisible());
        bool indicatorCaptured = false;
        QTimer::singleShot(0, picker, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            const auto closePicker = qScopeGuard([&] { if (dialog) dialog->reject(); });
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QStringLiteral("chartStudyPickerDialog"));
            QVERIFY(capture(window, size, path("S-04-indicators"), {dialog}));
            auto* ma = dialog->findChild<QPushButton*>("chartStudyMaSettings");
            auto* info = dialog->findChild<QPushButton*>("chartStudyInfo_bb");
            QVERIFY(ma && info);
            bool maCaptured = false;
            QTimer::singleShot(0, ma, [&] {
                auto* settingsDialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                const auto closeSettings = qScopeGuard([&] { if (settingsDialog) settingsDialog->reject(); });
                QVERIFY(settingsDialog);
                QCOMPARE(settingsDialog->objectName(), QStringLiteral("chartStudyMaSettingsDialog"));
                QVERIFY(capture(window, size, path("S-04-moving-averages"), {dialog, settingsDialog}));
                maCaptured = true;
            });
            ma->click();
            QVERIFY(maCaptured);

            bool helpCaptured = false;
            QTimer::singleShot(0, info, [&] {
                auto* help = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                const auto closeHelp = qScopeGuard([&] { if (help) help->reject(); });
                QVERIFY(help);
                QCOMPARE(help->objectName(), QStringLiteral("chartStudyHelpDialog"));
                QVERIFY(capture(window, size, path("S-05-indicator-help"), {dialog, help}));
                helpCaptured = true;
            });
            info->click();
            QVERIFY(helpCaptured);
            indicatorCaptured = true;
        });
        picker->click();
        QVERIFY(indicatorCaptured);

        auto* commodity = window.findChild<InfoButton*>("todayGroupInfo_commodities");
        QVERIFY(commodity);
        auto* todayPage = commodity->parentWidget();
        while (todayPage->parentWidget() && pages->indexOf(todayPage) < 0) todayPage = todayPage->parentWidget();
        QVERIFY(pages->indexOf(todayPage) >= 0);
        pages->setCurrentWidget(todayPage);
        commodity->click();
        auto* source = commodity->findChild<QDialog*>("infoDialog");
        QVERIFY(source);
        QVERIFY(capture(window, size, path("S-05-commodity-info"), {source}));
        source->close();

        QVERIFY(vault.create("Synthetic Render Fixture! 2026"));
        test::ReadFixture account("upbit");
        account.account.alias = QStringLiteral("합성 테스트 계좌");
        QString setupError;
        QVERIFY2(vault.addAccount(account.account, account.credentials, StorageMode::EncryptedVault, &setupError),
                 qPrintable(setupError));
        auto* connections = window.findChild<QWidget*>("connectionsPage");
        auto* list = window.findChild<QListView*>("accountList");
        auto* detail = window.findChild<QWidget*>("connectionDetail");
        QVERIFY(connections && list && detail);
        pages->setCurrentWidget(connections);
        QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 1, 3000);
        list->setCurrentIndex(list->model()->index(0, 0));
        QVERIFY(capture(window, size, path("S-06-accounts")));
        if (!detail->isVisible()) {
            QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualRect(list->currentIndex()).center());
            QVERIFY(detail->isVisible());
            QVERIFY(capture(window, size, path("S-06-account-detail")));
        }

        auto* add = window.findChild<QPushButton*>("addAccount");
        QVERIFY(add);
        add->click();
        auto* wizard = window.findChild<AccountDialog*>("accountDialog");
        QVERIFY(wizard);
        QVERIFY(capture(window, size, path("S-07-connect-account"), {wizard}));
        auto* binance = wizard->findChild<QAbstractButton*>("accountService_binance");
        QVERIFY(binance);
        binance->click();
        QVERIFY(capture(window, size, path("S-07-connection-fields"), {wizard}));
        wizard->reject();
        auto* assets = window.findChild<QWidget*>("portfolioHome");
        QVERIFY(assets);
        pages->setCurrentWidget(assets);
        auto* dayChange = window.findChild<QLabel*>("portfolioDayChange");
        QVERIFY(dayChange);
        QString previousDayText;
        QList<QRect> previousDayGeometry;
        int stableDayPasses = 0;
        const auto assetsReadyForCapture = [&] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            const auto requiredHeight = qMax(dayChange->minimumSizeHint().height(),
                                             dayChange->heightForWidth(dayChange->width()));
            if (portfolio.dayHistoryBusy() || !dayChange->isVisible() || dayChange->text().isEmpty()
                || dayChange->height() < requiredHeight) {
                stableDayPasses = 0;
                return false;
            }
            QList<QRect> geometry{dayChange->geometry(), dayChange->contentsRect()};
            for (auto* ancestor = dayChange->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
                const QRect visibleLabel(dayChange->mapTo(ancestor, QPoint{}), dayChange->size());
                if (!ancestor->contentsRect().contains(visibleLabel)) {
                    stableDayPasses = 0;
                    return false;
                }
                geometry << ancestor->geometry() << ancestor->contentsRect();
                if (ancestor == &window) break;
            }
            stableDayPasses = geometry == previousDayGeometry && dayChange->text() == previousDayText
                ? stableDayPasses + 1 : 1;
            previousDayGeometry = geometry;
            previousDayText = dayChange->text();
            return stableDayPasses >= 2;
        };
        // History delivery and the deferred responsive layout must both settle.
        QTRY_VERIFY_WITH_TIMEOUT(assetsReadyForCapture(), 3000);
        QVERIFY(capture(window, size, path("S-10-assets")));
        // The day chart and contribution bars open only on request.
        auto* dayToggle = window.findChild<QPushButton*>("portfolioDayToggle");
        if (dayToggle && dayToggle->isVisible()) {
            dayToggle->click();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QCoreApplication::processEvents();
            QVERIFY(capture(window, size, path("S-10-assets-flow")));
            dayToggle->click();
        }
    }

    void dev99Explore_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("minimumWholeRows");
        QTest::newRow("light-1440") << false << QSize(1440, 900) << 10;
        QTest::newRow("dark-1440") << true << QSize(1440, 900) << 10;
        QTest::newRow("light-2000") << false << QSize(2000, 1100) << 12;
        QTest::newRow("dark-2000") << true << QSize(2000, 1100) << 12;
    }

    void dev99Explore() {
        QFETCH(bool, dark);
        QFETCH(QSize, size);
        QFETCH(int, minimumWholeRows);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile.filePath("system"));
        const auto saved = Dev99ExploreTransport::symbols();
        const auto watchlistKey = QStringLiteral("explore/watchlist/coin");
        // ExploreWatchlist reads the application's public defaults at creation;
        // both those defaults and the injected settings live in this temp dir.
        { QSettings defaults; defaults.setValue(watchlistKey, saved); defaults.sync(); }
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue(watchlistKey, saved);
        settings.setValue("explore/observations/breadth", false);
        settings.setValue("explore/observations/relative", false);

        Vault vault(profile.filePath("synthetic-dev99-no-vault"));
        AccountModel accounts(&vault);
        Dev99ExploreTransport catalogWire, boardWire;
        const auto fixtureNow = QDateTime::currentMSecsSinceEpoch();
        Dev99TodayTransport calendarWire(fixtureNow), noticeWire(fixtureNow), earningsWire(fixtureNow);
        OfficialCalendar calendar(&calendarWire);
        OfficialNotices notices(&noticeWire);
        EarningsCalendar earnings(&earningsWire);
        SyntheticTransport historyWire, metadataWire(true), metricsWire;
        PublicMarketCatalog catalog(&catalogWire);
        PublicEquityBoard board(&boardWire);
        PublicMarketHistory history(&historyWire);
        MarketMetrics metrics(&metricsWire);
        PortfolioController::Factories factories;
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.setPortfolioController(&portfolio);
        window.setChartSettings(&settings);
        window.setPublicMarketCatalog(&catalog, false);
        window.setPublicEquityBoard(&board);
        window.setPublicMarketHistory(&history);
        window.setChartInstrumentTransport(&metadataWire);
        window.setDeferredWorkbenchInitializer([&] {
            window.setMarketMetrics(&metrics);
            window.setOfficialCalendar(&calendar);
            window.setEarningsCalendar(&earnings);
            if (size.width() >= 1680) window.setOfficialNotices(&notices);
        });
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        window.showVaultGate();
        window.resize(size);
        window.show();
        auto* browse = window.findChild<QPushButton*>("vaultGateBrowse");
        QVERIFY(browse && browse->isVisible());
        QTest::mouseClick(browse, Qt::LeftButton);
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        auto* explore = window.findChild<QWidget*>("explorePage");
        auto* view = window.findChild<QComboBox*>("exploreView");
        auto* watch = window.findChild<ExploreWatchlist*>("exploreWatchlist");
        QVERIFY(pages && explore && view && watch);
        pages->setCurrentWidget(explore);
        const auto todayIndex = view->findData("flow");
        QVERIFY(todayIndex >= 0);
        view->setCurrentIndex(todayIndex);
        catalog.refresh();
        board.refresh();
        history.setTargets({{"coin", "BTC", {}}});
        QTRY_COMPARE_WITH_TIMEOUT(catalog.rows().size(), 16, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(std::ranges::all_of(catalog.rows(), [](const auto& row) { return row.priced; }), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(board.items("coin").size(), 16, 5000);
        QVERIFY2(board.status("coin").isEmpty(), qPrintable(board.status("coin")));
        QTRY_VERIFY_WITH_TIMEOUT(history.history("coin", "BTC").points.size() > 1, 5000);
        auto* quote = window.findChild<ExploreMetricCard*>("exploreSelectedCoinQuote");
        auto* table = watch->findChild<QTableWidget*>("watchlistTable");
        QVERIFY(quote && table);
        QTRY_VERIFY_WITH_TIMEOUT(quote->data().history.available, 5000);
        QTRY_COMPARE(table->rowCount(), 16);
        QCOMPARE(watch->ids("coin"), saved);
        QVERIFY(watch->compact());

        // A successful refresh omits one known public quote. The app retains
        // its value and creates the ordinary stale dot, without invented UI.
        QTRY_VERIFY_WITH_TIMEOUT(!board.busy(), 5000);
        boardWire.omitEthereum = true;
        board.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(board.items("coin").size(), 15, 5000);
        QVERIFY(!board.knownItem("coin", "ETH").price.isEmpty());
        QVERIFY2(board.status("coin").isEmpty(), qPrintable(board.status("coin")));
        QTRY_COMPARE(table->rowCount(), 16);
        QVERIFY(!table->item(1, 1)->text().contains(QStringLiteral("이전 시세")));
        QVERIFY(table->item(1, 2)->text() != QStringLiteral("—"));
        const auto hasVisibleStaleLegend = [&] {
            for (const auto* label : watch->findChildren<QLabel*>())
                if (label->isVisibleTo(&window) && label->text().contains(QStringLiteral("이전 시세")) &&
                    label->text().contains(QStringLiteral("갱신 대기"))) return true;
            return false;
        };
        QTRY_VERIFY(hasVisibleStaleLegend());
        const auto wholeRows = [&] {
            int count = 0;
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->isRowHidden(row)) continue;
                const auto bounds = table->visualItemRect(table->item(row, 1))
                    .united(table->visualItemRect(table->item(row, 3)));
                if (!bounds.isEmpty() && table->viewport()->rect().contains(bounds) &&
                    window.rect().contains(QRect(table->viewport()->mapTo(&window, bounds.topLeft()), bounds.size()))) ++count;
            }
            return count;
        };
        QTRY_VERIFY(table->isVisibleTo(&window));
        QTRY_VERIFY_WITH_TIMEOUT(wholeRows() >= minimumWholeRows, 5000);
        QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(table->columnWidth(0), 14);
        QCOMPARE(table->columnWidth(2), 92);
        QCOMPARE(table->columnWidth(3), 62);
        for (int row = 0; row < minimumWholeRows; ++row)
            QCOMPARE(table->visualItemRect(table->item(row, 1)).height(), 32);

        const auto calendarsReady = [&] {
            for (const auto source : {OfficialCalendarSource::Bls, OfficialCalendarSource::Bea,
                                      OfficialCalendarSource::Fomc, OfficialCalendarSource::Bok})
                if (!calendar.feed(source).available || calendar.feed(source).loading) return false;
            for (const auto source : {EarningsSource::Kind, EarningsSource::Nasdaq})
                if (!earnings.feed(source).available || earnings.feed(source).loading) return false;
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(calendarsReady(), 5000);
        auto* calendarView = window.findChild<TodayCalendarView*>("todayCalendarView");
        auto* noticePanel = window.findChild<QWidget*>("todayNoticePanel");
        QVERIFY(calendarView && calendarView->isVisibleTo(&window) && noticePanel);
        const auto visibleCalendarRows = [&] {
            int count = 0;
            for (auto* row : calendarView->findChildren<QWidget*>("todayCalendarRow"))
                if (row->isVisibleTo(&window)) ++count;
            return count;
        };
        QTRY_VERIFY(visibleCalendarRows() >= 3);
        auto* calendarStatus = calendarView->findChild<QLabel*>("todayCalendarStatus");
        QVERIFY(calendarStatus && calendarStatus->text().isEmpty());
        for (auto* row : calendarView->findChildren<QWidget*>("todayCalendarRow")) {
            if (!row->isVisibleTo(&window)) continue;
            auto* date = row->findChild<QLabel*>("todayCalendarDate");
            auto* time = row->findChild<QLabel*>("todayCalendarTime");
            QVERIFY(date && time);
            QVERIFY(qAbs(date->mapTo(row, date->rect().center()).y() - time->mapTo(row, time->rect().center()).y()) <= 1);
        }
        if (size.width() >= 1680) {
            QTRY_VERIFY_WITH_TIMEOUT(notices.feed(OfficialNoticeSource::Okx).available &&
                                     notices.feed(OfficialNoticeSource::Bithumb).available, 5000);
            QTRY_VERIFY(noticePanel->isVisibleTo(&window));
            int noticeRows = 0;
            for (auto* info : noticePanel->findChildren<InfoButton*>()) if (info->isVisibleTo(&window)) ++noticeRows;
            QCOMPARE(noticeRows, 3);
            const QRect scheduleBounds(calendarView->mapTo(&window, QPoint{}), calendarView->size());
            const QRect noticeBounds(noticePanel->mapTo(&window, QPoint{}), noticePanel->size());
            QCOMPARE(scheduleBounds.top(), noticeBounds.top());
            QVERIFY(scheduleBounds.right() < noticeBounds.left());
            QVERIFY(scheduleBounds.width() >= 320 && noticeBounds.width() >= 320);
            QVERIFY(window.rect().contains(scheduleBounds) && window.rect().contains(noticeBounds));
        } else {
            QVERIFY(!noticePanel->isVisibleTo(&window));
        }
        // The empty reasons share one 40px frame. Child backgrounds must not
        // erase its straight top/bottom border between the rounded corners.
        auto* emptyStrip = window.findChild<QWidget*>("todayEmptyStrip");
        QVERIFY(emptyStrip && emptyStrip->isVisibleTo(&window));
        QCOMPARE(emptyStrip->height(), 40);
        const auto frame = window.grab().toImage();
        const auto stripOrigin = emptyStrip->mapTo(&window, QPoint{});
        const auto dpr = frame.devicePixelRatio();
        for (int x = 16; x < emptyStrip->width() - 16; ++x)
            for (const int y : {0, emptyStrip->height() - 1}) {
                const QPoint pixel(qFloor((stripOrigin.x() + x + 0.5) * dpr),
                                   qFloor((stripOrigin.y() + y + 0.5) * dpr));
                QCOMPARE(frame.pixelColor(pixel), themeColors().rule);
            }
        QVERIFY(capture(window, size, output_.filePath(QStringLiteral("S-09-dev99-explore-%1.png")
            .arg(QString::fromLatin1(QTest::currentDataTag())))));
        QCOMPARE(watch->ids("coin"), saved);
        QCOMPARE(QSettings().value(watchlistKey).toStringList(), saved);
        QVERIFY(!vault.exists());
        QCOMPARE(accounts.rowCount(), 0);
        qInfo() << "synthetic Dev99 exploration" << (dark ? "dark" : "light") << size
                << "whole watchlist rows" << wholeRows();
    }

    void dev99Comparison_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light-1440") << false;
        QTest::newRow("dark-1440") << true;
    }

    void dev99Comparison() {
        QFETCH(bool, dark);
        const QSize size(1440, 900);
        constexpr qint64 Day = 86400000;
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile.filePath("system"));
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("marketCompare/intervalMs/v1", Day);
        const auto path = [&](const QString& state) {
            return output_.filePath(QStringLiteral("S-03-dev99-compare-%1-%2.png")
                .arg(state, QString::fromLatin1(QTest::currentDataTag())));
        };
        Vault vault(profile.filePath("synthetic-dev99-comparison-no-vault"));
        AccountModel accounts(&vault);
        QElapsedTimer elapsed;
        elapsed.start();
        const auto syntheticNoon = QDateTime::currentMSecsSinceEpoch() / Day * Day + Day / 2;
        const auto clock = [&] { return syntheticNoon + elapsed.elapsed(); };
        Dev99CompareTransport compareWire(clock);
        MarketCompareSource compare(&compareWire, nullptr, clock);
        PortfolioController::Factories factories;
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.setPortfolioController(&portfolio);
        window.setChartSettings(&settings);
        window.setDeferredWorkbenchInitializer([&] { window.setMarketCompareSource(&compare); });
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        window.showVaultGate();
        window.resize(size);
        window.show();
        auto* browse = window.findChild<QPushButton*>("vaultGateBrowse");
        QVERIFY(browse && browse->isVisible());
        QTest::mouseClick(browse, Qt::LeftButton);
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        auto* comparison = window.findChild<MarketCompareView*>();
        QVERIFY(pages && comparison);
        pages->setCurrentWidget(comparison);
        const auto allSeriesReady = [&] {
            const auto snapshot = compare.snapshot();
            return !snapshot.loading && snapshot.series.size() == 4 &&
                std::ranges::all_of(snapshot.series, [](const auto& series) {
                    return series.available && series.bars.size() == 200;
                });
        };
        QTRY_VERIFY_WITH_TIMEOUT(allSeriesReady(), 5000);
        QCOMPARE(compare.intervalMs(), Day);
        QCOMPARE(int(compare.snapshot().wonMode), int(CompareWonMode::Dollar));
        QCOMPARE(compareWire.sent.size(), 5);
        auto* quarter = comparison->findChild<QPushButton*>("marketComparePeriod_3m");
        auto* score = comparison->findChild<QTableWidget*>("marketComparePeriodTable");
        QVERIFY(quarter && quarter->isVisibleTo(&window) && score && score->isVisibleTo(&window));
        QTRY_VERIFY(quarter->isEnabled());
        QVERIFY(!quarter->isChecked());
        QTest::mouseClick(quarter, Qt::LeftButton);
        QTRY_VERIFY(quarter->isChecked());
        QCOMPARE(compare.intervalMs(), Day);
        QCOMPARE(comparison->baseOpenMs(), clock() / Day * Day - Day - 90 * Day);
        QCOMPARE(settings.value("marketCompare/period/v1").toString(), QStringLiteral("3m"));
        QVERIFY(settings.value("marketCompare/automaticInterval/v1").toBool());
        QCOMPARE(score->rowCount(), 4);
        QCOMPARE(score->columnCount(), 6);
        for (int row = 0; row < 4; ++row) for (int column = 1; column <= 4; ++column) {
            auto* cell = score->item(row, column);
            QVERIFY(cell && cell->text().contains('%') && !cell->text().contains(QStringLiteral("—")));
        }
        const auto scoreFits = [&] {
            return window.rect().contains(QRect(score->mapTo(&window, QPoint{}), score->size()));
        };
        QTRY_VERIFY(scoreFits());
        QCOMPARE(compareWire.sent.size(), 5); // One daily snapshot supplies all four score columns.
        auto* chart = comparison->findChild<QWidget*>("marketCompareChart");
        auto* correlation = comparison->findChild<QWidget*>("marketCompareCorrelation");
        auto* toggle = comparison->findChild<QPushButton*>("marketCompareCorrelationToggle");
        auto* target = comparison->findChild<QComboBox*>("marketCompareTargetSelector");
        auto* options = window.findChild<QPushButton*>("openOptions");
        QVERIFY(chart && correlation && toggle && target && options);
        QTRY_VERIFY(chart->isVisibleTo(&window) && toggle->isVisibleTo(&window));
        QVERIFY(!correlation->isVisibleTo(&window));
        QVERIFY(!target->isVisibleTo(&window));
        for (const auto& symbol : QStringList{"XAUUSDT", "CLUSDT", "QQQUSDT", "BTCUSDT"}) {
            auto* chip = comparison->findChild<QAbstractButton*>("marketCompareChip_" + symbol);
            QVERIFY2(chip && chip->isVisibleTo(&window), qPrintable(symbol));
        }
        const auto baseBefore = comparison->baseOpenMs();
        const auto collapsedHeight = chart->height();
        QTest::mouseMove(options, options->rect().center());
        QTRY_VERIFY(chart->accessibleDescription().isEmpty());
        QVERIFY(capture(window, size, path("main")));

        QTest::mouseMove(chart, QPoint(chart->width() * 3 / 5, chart->height() / 2));
        QTRY_VERIFY(!chart->accessibleDescription().isEmpty());
        const auto hover = chart->accessibleDescription();
        QVERIFY(hover.contains(QStringLiteral("금")) && hover.contains(QStringLiteral("원유")) &&
                hover.contains(QStringLiteral("나스닥")) &&
                (hover.contains(QStringLiteral("BTC")) || hover.contains(QStringLiteral("비트코인"))));
        QVERIFY(hover.count('%') >= 4);
        QCOMPARE(comparison->baseOpenMs(), baseBefore);
        QVERIFY(capture(window, size, path("hover")));

        QTest::mouseMove(options, options->rect().center());
        QTRY_VERIFY(chart->accessibleDescription().isEmpty());
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(correlation->isVisibleTo(&window) && target->isVisibleTo(&window));
        QTRY_VERIFY(chart->height() < collapsedHeight);
        QTRY_VERIFY(scoreFits());
        QVERIFY(capture(window, size, path("corr")));
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(!correlation->isVisibleTo(&window));
        QCOMPARE(comparison->baseOpenMs(), baseBefore);
        QCOMPARE(compare.intervalMs(), Day);
        QCOMPARE(compareWire.sent.size(), 5); // Captures, hover and correlation expansion reuse that snapshot.
        QVERIFY(!vault.exists());
        QCOMPARE(accounts.rowCount(), 0);
        qInfo() << "synthetic Dev99 comparison" << (dark ? "dark" : "light") << size
                << "main/hover/corr";
    }

    void dev98Panels_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light-1440") << false;
        QTest::newRow("dark-1440") << true;
    }

    void dev98Panels() {
        QFETCH(bool, dark);
        const QSize size(1440, 900);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath("user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile.filePath("system"));
        const auto path = [&](const QString& state) {
            return output_.filePath(QStringLiteral("S-08-dev98-panel-%1-%2.png")
                .arg(state, QString::fromLatin1(QTest::currentDataTag())));
        };

        // This complete app fixture has no user files or live transports. The
        // synthetic provider feeds the normal holdings resolver, so periodic
        // LocalWindow refreshes preserve the same rows while a menu is open.
        Vault vault(QDir(QDir(profile.path()).canonicalPath()).filePath("synthetic-dev98-vault"));
        AccountModel accounts(&vault);
        SyntheticTransport candleWire, compareWire, metadataWire(true), catalogWire(true), rankingWire(true), metricsWire;
        MarketCandles candles(&candleWire, std::unique_ptr<PublicCandleSocket>{});
        MarketCompareSource compare(&compareWire);
        PublicMarketCatalog catalog(&catalogWire);
        PublicEquityBoard board(&rankingWire);
        MarketMetrics metrics(&metricsWire);
        test::ProviderReadTransport accountWire;
        accountWire.provider = "upbit";
        accountWire.transform = [](const RequestSpec& request, HttpResult& response) {
            if (request.operation != Operation::Account) return;
            QJsonArray balances;
            for (const auto& row : QList<QStringList>{{"BTC", "0.125"}, {"ETH", "2"},
                                                     {"SOL", "12"}, {"SYN", "4"}}) {
                balances.append(QJsonObject{{"currency", row[0]}, {"balance", row[1]}, {"locked", "0"},
                    {"avg_buy_price", "0"}, {"avg_buy_price_modified", false}, {"unit_currency", "KRW"}});
            }
            response = upbit_fixture::json(balances);
        };
        PortfolioController::Factories factories;
        factories.trading = [&](const AccountRecord&) { return &accountWire; };
        factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
        PortfolioController portfolio(vault, nullptr, factories);
        QSettings settings(profile.filePath("settings.ini"), QSettings::IniFormat);
        // These images describe the normal row layout. Disable optional studies
        // through the same public preferences used by the app, before creation.
        ChartStudyPreferences panelPreferences;
        panelPreferences.markIntroSeen();
        for (const auto& id : panelPreferences.supportedIds("coin"))
            QVERIFY(panelPreferences.setEnabled(id, false));
        settings.setValue("chart/studies/v1", panelPreferences.publicState());
        settings.setValue("explore/observations/breadth", false);
        settings.setValue("explore/observations/relative", false);
        LocalWindow window(vault, accounts);
        window.setSecurityStatus(true);
        window.setPortfolioController(&portfolio);
        window.setPublicMarketCatalog(&catalog, false);
        window.setPublicEquityBoard(&board);
        window.setChartSettings(&settings);
        window.setChartInstrumentTransport(&metadataWire);
        window.setChartContractTransport(&metadataWire);
        window.setDeferredWorkbenchInitializer([&] {
            window.setMarketMetrics(&metrics);
            window.setMarketCandles(&candles);
            window.setMarketCompareSource(&compare);
        });
        KeefiTheme::instance()->setMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        window.showVaultGate();
        window.resize(size);
        window.show();
        auto* browse = window.findChild<QPushButton*>("vaultGateBrowse");
        QVERIFY(browse && browse->isVisible());
        QTest::mouseClick(browse, Qt::LeftButton);
        auto* pages = window.findChild<QTabWidget*>("portfolioPages");
        auto* workspace = window.findChild<ChartWorkspace*>();
        QVERIFY(pages && workspace);
        workspace->setObservationStudies({});
        for (const auto& id : panelPreferences.supportedIds("coin"))
            QVERIFY(!workspace->indicatorPreferences()->enabled(id));
        auto* instruments = window.findChild<ChartInstrumentCatalog*>("chartInstrumentCatalog");
        QVERIFY(instruments);
        instruments->ensure("binance");
        board.refreshBinanceRanking();
        QTRY_COMPARE_WITH_TIMEOUT(instruments->instruments("binance").size(), 5, 3000);
        QVERIFY2(instruments->failureReason("binance").isEmpty(), qPrintable(instruments->failureReason("binance")));
        QVERIFY(std::ranges::all_of(instruments->instruments("binance"), [](const auto& instrument) {
            return instrument.tradable && instrument.status == "TRADING" && instrument.quoteAsset == "USDT"
                && instrument.symbol == instrument.baseAsset + instrument.quoteAsset;
        }));
        QTRY_COMPARE_WITH_TIMEOUT(board.items("binance").size(), 5, 3000);
        QVERIFY2(board.status("binance").isEmpty(), qPrintable(board.status("binance")));
        catalog.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(catalog.verifiedMarkets().contains("KRW-DOGE"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(catalog.rows().size() == 5 &&
            std::ranges::all_of(catalog.rows(), [](const auto& row) { return row.priced; }), 3000);
        QCOMPARE(catalog.marketListState(), PublicMarketCatalog::MarketListState::Ready);

        QVERIFY(vault.create("Synthetic Dev98 Render Fixture! 2026"));
        test::ReadFixture account("upbit");
        account.account.alias = QStringLiteral("Dev98 합성 테스트 계좌");
        QString setupError;
        QVERIFY2(vault.addAccount(account.account, account.credentials, StorageMode::EncryptedVault, &setupError),
                 qPrintable(setupError));
        portfolio.refreshAll();
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(portfolio.connections().first().state, QStringLiteral("complete"), 5000);
        pages->setCurrentWidget(workspace);
        workspace->indicatorPreferences()->markIntroSeen();
        workspace->setTimeframe(900000);
        QVERIFY(workspace->setIdentity(Identity));
        QTRY_VERIFY_WITH_TIMEOUT(candles.series().available, 3000);
        auto* applied = workspace->findChild<QPushButton*>("chartStudyAppliedToggle");
        QVERIFY(applied);
        if (applied->isChecked()) QTest::mouseClick(applied, Qt::LeftButton);
        QTest::qWait(1100);

        auto* lists = workspace->watchlistModel();
        QVERIFY(lists);
        const auto listId = lists->createList(QStringLiteral("합성 관심"));
        QVERIFY(!listId.isEmpty());
        const WatchlistIdentity bitcoin{"coin", "upbit", "KRW-BTC"};
        QVERIFY(lists->addIdentity(listId, bitcoin));
        auto* open = workspace->findChild<QAbstractButton*>("chartInstrumentButton");
        auto* panel = workspace->findChild<QWidget*>("chartInstrumentPanel");
        auto* all = workspace->findChild<QAbstractButton*>("chartBrowserScope_all");
        auto* hold = workspace->findChild<QAbstractButton*>("chartBrowserScope_holdings");
        auto* watch = workspace->findChild<QAbstractButton*>("chartBrowserScope_watch");
        auto* rows = workspace->findChild<QTreeWidget*>("chartWorkspaceWatchlist");
        auto* holdingRows = workspace->findChild<QTreeWidget*>("chartHoldingAssets");
        auto* selector = workspace->findChild<QComboBox*>("chartWorkspaceListSelector");
        auto* menuButton = workspace->findChild<QToolButton*>("chartWorkspaceListMenu");
        auto* status = workspace->findChild<QLabel*>("chartWorkspaceListStatus");
        auto* retry = workspace->findChild<QAbstractButton*>("chartBrowserRetry");
        QVERIFY(open && panel && all && hold && watch && rows && holdingRows && selector && menuButton && status && retry);
        const auto normalRowsReady = [&] {
            for (const auto& symbol : QStringList{"BTC", "ETH", "SOL", "XRP", "DOGE"}) {
                const auto state = lists->rowState({"coin", "upbit", "KRW-" + symbol});
                if (state.price.isEmpty() || state.quoteState != WatchlistQuoteState::Receiving ||
                    !state.badges.isEmpty() || !state.badgePresentations.isEmpty()) return false;
            }
            return true;
        };
        const auto visibleRows = [&] {
            int count = 0;
            for (QTreeWidgetItemIterator it(rows); *it; ++it) {
                auto* row = *it;
                if (row->isHidden() || row->data(0, Qt::UserRole).toString().isEmpty()) continue;
                bool visible = true;
                for (auto* parent = row->parent(); parent; parent = parent->parent())
                    visible = visible && !parent->isHidden() && parent->isExpanded();
                if (visible) ++count;
            }
            return count;
        };
        QTest::mouseClick(open, Qt::LeftButton);
        QTRY_VERIFY(panel->isVisible());
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_VERIFY(rows->isVisible() && visibleRows() == 5);
        QTRY_VERIFY(normalRowsReady());
        QCOMPARE(status->text(), QStringLiteral("5개"));
        QVERIFY(!status->isVisible() && !retry->isVisible());
        QVERIFY(panel->height() <= 520);
        QVERIFY(capturePanel(window, *panel, size, path("all")));

        QTest::mouseClick(hold, Qt::LeftButton);
        QTRY_VERIFY(holdingRows->isVisible() && holdingRows->topLevelItemCount() == 4);
        bool unsupportedHolding = false;
        for (int i = 0; i < holdingRows->topLevelItemCount(); ++i) {
            auto* holding = holdingRows->topLevelItem(i);
            unsupportedHolding = unsupportedHolding || (holding->text(0).contains("SYN") &&
                holding->text(1).contains(QStringLiteral("차트 자료 없음")));
        }
        QVERIFY(unsupportedHolding);
        QVERIFY(!retry->isVisible());
        QVERIFY(capturePanel(window, *panel, size, path("hold")));

        const auto majors = lists->addSection(listId, QStringLiteral("메이저"));
        const auto watchSection = lists->addSection(listId, QStringLiteral("관찰 중"));
        QVERIFY(!majors.isEmpty() && !watchSection.isEmpty());
        QVERIFY(lists->moveIdentity(listId, bitcoin, 0, majors));
        QVERIFY(lists->addIdentity(listId, {"coin", "upbit", "KRW-ETH"}, majors));
        QVERIFY(lists->addIdentity(listId, {"coin", "upbit", "KRW-SOL"}, watchSection));
        QVERIFY(lists->addIdentity(listId, {"coin", "upbit", "KRW-DOGE"}, watchSection));
        QVERIFY(lists->addIdentity(listId, {"coin", "upbit", "KRW-XRP"}));
        QTest::mouseClick(watch, Qt::LeftButton);
        QTRY_VERIFY(rows->isVisible() && selector->isVisible());
        const auto listIndex = selector->findData(listId);
        QVERIFY(listIndex >= 0);
        if (selector->currentData().toString() != listId) {
            QTest::mouseClick(selector, Qt::LeftButton);
            QTest::keyClick(selector, Qt::Key_Home);
            for (int i = 0; i < listIndex; ++i) QTest::keyClick(selector, Qt::Key_Down);
            QTest::keyClick(selector, Qt::Key_Return);
        }
        QTRY_COMPARE(selector->currentData().toString(), listId);
        for (const auto& sectionId : {majors, watchSection}) {
            QTreeWidgetItem* section = nullptr;
            for (int i = 0; i < rows->topLevelItemCount(); ++i) {
                auto* candidate = rows->topLevelItem(i);
                if (candidate->data(0, Qt::UserRole).toString().isEmpty() &&
                    candidate->data(0, Qt::UserRole + 1).toString() == sectionId) section = candidate;
            }
            QVERIFY(section);
            const auto sectionRect = rows->visualItemRect(section);
            QVERIFY(!sectionRect.isEmpty());
            QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, sectionRect.center());
            QTest::keyClick(rows, Qt::Key_Left);
            QTRY_VERIFY(!section->isExpanded());
            QTest::keyClick(rows, Qt::Key_Right);
            QTRY_VERIFY(section->isExpanded());
        }
        QTRY_COMPARE(visibleRows(), 5);
        QTRY_VERIFY(normalRowsReady());
        QCOMPARE(status->text(), QStringLiteral("5개"));
        QVERIFY(!status->isVisible() && !retry->isVisible());
        QVERIFY(capturePanel(window, *panel, size, path("watch")));

        auto* menu = menuButton->menu();
        QVERIFY(menu && menuButton->isVisible());
        bool menuOpened = false, menuCaptured = false;
        QTimer captureMenu;
        captureMenu.setInterval(1);
        connect(&captureMenu, &QTimer::timeout, &window, [&] {
            if (!menu->isVisible()) return;
            captureMenu.stop();
            const auto closeMenu = qScopeGuard([&] { QTest::keyClick(menu, Qt::Key_Escape); });
            menuOpened = true;
            menuCaptured = capturePanel(window, *panel, size, path("menu"), {menu});
        });
        captureMenu.start();
        QTest::mouseClick(menuButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(menuOpened, 3000);
        QVERIFY(menuCaptured);
        QTRY_VERIFY(!menu->isVisible());
        QVERIFY(panel->isVisible());

        const auto publicRow = [&](const QString& key) -> QTreeWidgetItem* {
            for (QTreeWidgetItemIterator it(rows); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toString() == key) return *it;
            return nullptr;
        };
        const auto savedListState = lists->publicState();
        const auto chartBeforeInteraction = workspace->selectedChartIdentity();
        QSignalSpy renderOpens(workspace, &ChartWorkspace::identitySelected);
        QVERIFY(renderOpens.isValid());
        auto* draggedRow = publicRow(bitcoin.key());
        auto* targetRow = publicRow(QStringLiteral("coin:upbit:KRW-SOL"));
        QVERIFY(draggedRow && targetRow);
        const auto draggedRect = rows->visualItemRect(draggedRow);
        const auto targetRect = rows->visualItemRect(targetRow);
        QVERIFY(!draggedRect.isEmpty() && !targetRect.isEmpty());
        const QPoint dragFrom(draggedRect.left() + qMin(12, qMax(1, draggedRect.width() / 3)),
                              draggedRect.center().y());
        const QPoint dragTo(targetRect.left() + qMin(110, qMax(1, targetRect.width() / 3)),
                            targetRect.top() + 1);
        auto* viewport = rows->viewport();
        bool dragFinished = false;
        const auto finishDrag = [&] {
            if (dragFinished) return;
            dragFinished = true;
            QDragLeaveEvent leave;
            QApplication::sendEvent(viewport, &leave);
            QDrag::cancel();
            QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, dragTo);
        };
        const auto cancelDragOnExit = qScopeGuard(finishDrag);
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, dragFrom);
        std::unique_ptr<QMimeData> rowMime(rows->model()->mimeData(rows->selectionModel()->selectedIndexes()));
        QVERIFY(rowMime);
        bool dragDelivered = false, dragEntered = false, dragMoved = false, dragCaptured = false;
        QTimer captureDrag;
        captureDrag.setSingleShot(true);
        connect(&captureDrag, &QTimer::timeout, &window, [&] {
            const auto finish = qScopeGuard(finishDrag);
            dragDelivered = true;
            // Use the real view MIME; a native drag may own a newer copy while
            // its nested event loop is running. No Drop event is delivered.
            const QMimeData* mime = rowMime.get();
            const auto drags = rows->findChildren<QDrag*>();
            for (auto it = drags.crbegin(); it != drags.crend(); ++it)
                if ((*it)->mimeData()) { mime = (*it)->mimeData(); break; }
            QDragEnterEvent enter(dragTo, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            enter.setAccepted(false);
            QApplication::sendEvent(viewport, &enter);
            dragEntered = enter.isAccepted();
            QDragMoveEvent move(dragTo, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            move.setAccepted(false);
            QApplication::sendEvent(viewport, &move);
            dragMoved = move.isAccepted();
            if (dragEntered && dragMoved)
                dragCaptured = capturePanel(window, *panel, size, path("drag"));
        });
        captureDrag.start(0);
        const auto dragMotion = dragFrom + QPoint(qMax(24, QApplication::startDragDistance() + 4), 0);
        QMouseEvent motion(QEvent::MouseMove, QPointF(dragMotion), QPointF(viewport->mapToGlobal(dragMotion)),
                           Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &motion);
        QTRY_VERIFY_WITH_TIMEOUT(dragDelivered, 3000);
        QVERIFY(dragEntered && dragMoved && dragCaptured && dragFinished);
        QCOMPARE(lists->publicState(), savedListState);
        QCOMPARE(workspace->selectedChartIdentity(), chartBeforeInteraction);
        QCOMPARE(renderOpens.count(), 0);
        QVERIFY(panel->isVisible());

        targetRow = publicRow(QStringLiteral("coin:upbit:KRW-SOL"));
        QVERIFY(targetRow);
        const auto contextRect = rows->visualItemRect(targetRow);
        QVERIFY(!contextRect.isEmpty());
        const QPoint contextPoint(contextRect.left() + qMin(110, qMax(1, contextRect.width() / 3)),
                                  contextRect.center().y());
        bool contextOpened = false, contextCaptured = false;
        QPointer<QMenu> rowMenu;
        QStringList contextActions;
        QTimer captureContext, contextDeadline;
        captureContext.setInterval(1);
        contextDeadline.setSingleShot(true);
        connect(&contextDeadline, &QTimer::timeout, &window, [&] {
            captureContext.stop();
            if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                QTest::keyClick(popup, Qt::Key_Escape);
        });
        connect(&captureContext, &QTimer::timeout, &window, [&] {
            auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!popup || popup->objectName() != QStringLiteral("chartInstrumentRowMenu")) return;
            captureContext.stop();
            contextDeadline.stop();
            rowMenu = popup;
            contextOpened = true;
            const auto closeContext = qScopeGuard([&] {
                if (rowMenu) QTest::keyClick(rowMenu, Qt::Key_Escape);
            });
            for (auto* action : popup->actions())
                if (action->isVisible() && !action->isSeparator())
                    contextActions.append(QString(action->text()).remove('&'));
            contextCaptured = capturePanel(window, *panel, size, path("context"), {popup});
        });
        captureContext.start();
        contextDeadline.start(3000);
        QContextMenuEvent context(QContextMenuEvent::Mouse, contextPoint, viewport->mapToGlobal(contextPoint));
        QApplication::sendEvent(viewport, &context);
        QTRY_VERIFY_WITH_TIMEOUT(contextOpened, 3000);
        QVERIFY(contextCaptured);
        QCOMPARE(contextActions, QStringList({QStringLiteral("섹션으로 이동"), QStringLiteral("맨 위로"),
                                             QStringLiteral("알림 설정"), QStringLiteral("관심 해제")}));
        QTRY_VERIFY(!rowMenu || !rowMenu->isVisible());
        QVERIFY(panel->isVisible());
        QCOMPARE(lists->publicState(), savedListState);
        QCOMPARE(workspace->selectedChartIdentity(), chartBeforeInteraction);
        QCOMPARE(renderOpens.count(), 0);
        qInfo() << "synthetic Dev98 panel renders" << (dark ? "dark" : "light")
                << size << "all/hold/watch/menu/drag/context";
    }
};

QTEST_MAIN(KeefiUiRenderProbe)
#include "tst_keefi_ui_renders.moc"
