#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <memory>

namespace keefi {
class TradingTransport;
struct RequestSpec;

// One market-map entry from an unofficial public endpoint. Display only:
// these values never feed holdings valuation, totals or any order path.
struct PublicBoardItem {
    QString id, symbol, name, group;     // group: Korean sector label
    QString price, changePercent;        // exact display strings; changePercent signed ("-1.43")
    QString currency;                    // "KRW" or "USD"
    double marketCap = 0;                // in currency units; sizes tiles only
    double tradedValue = 0;              // traded value in currency units (coins 24h, stocks today); 0 if unknown
    QString sourceId;                    // Unique CoinGecko id for the public chart route.
    double tradedUnits = 0;              // Actual stock share volume, not price times shares.
    QList<double> sparkline;             // Coins: last 25 hourly CoinGecko prices (oldest first). Display only.
    qint64 receivedAtMs = 0;
    qint64 sourceTimeMs = 0;
    // The map keeps one display row per symbol, but a later valid source row
    // must not disappear from the public identity check.
    bool ambiguousSymbol = false;
};

// Keyless public boards for the explore market map:
//   coin — CoinGecko /coins/markets (market cap, KRW price, 24h change)
//   kr   — Naver 증권 mobile market-value list (KOSPI by market cap)
//   us   — Nasdaq public screener (market cap, sector, change) and market-info session
// These endpoints have no published contract: every response is validated,
// failures keep the last good board and are reported, nothing is retried in a loop.
class PublicEquityBoard final : public QObject {
    Q_OBJECT
  public:
    // A separate ranking transport keeps Binance independent of slower board providers.
    // Omit it (or pass the same transport) to retain the legacy serial request path.
    explicit PublicEquityBoard(TradingTransport* transport, QObject* parent = nullptr,
                               TradingTransport* rankingTransport = nullptr);
    ~PublicEquityBoard() override;
    void refresh(); // coin and kr every call; us at most once a minute
    void refreshBinanceRanking(); // On demand, at most once a minute.
    static bool parseBinanceTickers(const QByteArray&, QList<PublicBoardItem>*);
    QList<PublicBoardItem> items(const QString& section) const; // sorted by market cap, largest first
    PublicBoardItem knownItem(const QString& section, const QString& symbol) const; // Last good quote, including an item leaving the board.
    qint64 receivedAtMs(const QString& section) const;
    QString status(const QString& section) const; // empty when the last read succeeded
    QString marketStatus(const QString& section) const; // e.g. "장중", "장 마감"
    bool busy() const;
    bool marketBoardBusy() const; // General board reads, independent of a dedicated Binance ranking request.
    // Exact request shapes; also used by the transport allowlist and tests.
    static RequestSpec request(const QString& section);
    static bool isBoardRequest(const RequestSpec& request);
    // Parsers are public for tests; they return false on any malformed payload.
    static bool parseCoins(const QByteArray& body, QList<PublicBoardItem>* out);
    static bool parseKorea(const QByteArray& body, QList<PublicBoardItem>* out, QString* marketStatus);
    static bool parseUs(const QByteArray& body, QList<PublicBoardItem>* out);
    // Nasdaq market-info session ("장중", "프리마켓", "애프터마켓", "장 마감"); false when unknown.
    static bool parseUsMarketStatus(const QByteArray& body, QString* status);
    // US regular session (09:30–16:00 New York) on the New York date of `now`, in KST.
    // Follows US daylight saving: "22:30–05:00 KST" in summer, "23:30–06:00 KST" in winter.
    static QString usSessionKst(const QDateTime& now);

  signals:
    void changed();

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace keefi
