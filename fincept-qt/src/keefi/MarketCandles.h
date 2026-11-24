#pragma once
#include "keefi/TradingTypes.h"
#include <QObject>
#include <functional>
#include <memory>
#include <optional>
namespace keefi {
class TradingTransport;
class PublicCandleSocket : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    ~PublicCandleSocket() override = default;
    virtual void open(quint64 generation, const QUrl& endpoint) = 0;
    virtual void send(quint64 generation, const QByteArray& message) = 0;
    virtual void abort() = 0;
  signals:
    void connected(quint64 generation);
    void received(quint64 generation, const QByteArray& message);
    void failed(quint64 generation, int httpStatus = 0);
};
struct MarketCandle {
    qint64 startsAtMs=0, lastStoredTickMs=0;
    QString open, high, low, close, volume;
};
struct MarketCandleSeries {
    bool available=false;
    QString asset, sourceUrl, reason;
    qint64 receivedAtMs=0;
    QList<MarketCandle> candles; // Closed, actually observed native candles only.
    std::optional<MarketCandle> forming; // Display only; never passed to studies or alerts.
    QString identity, currency;
    qint64 intervalMs=15*60000, sourceTimeMs=0; // Zero source time is unavailable, never receipt time.
    // Public quote presentation only. Expired observations stay unavailable
    // with no candles/forming candle; this value never enters a calculation.
    QString staleClose;
    bool delayed=false;
    // Provenance of the closed rows: the last start and receipt time of the REST
    // response. Rows the public stream closes later have no REST receipt yet.
    qint64 restThroughMs=0, restReceivedAtMs=0;
    bool fromArchive=false; // Display only: rows came from the encrypted price archive.
    bool fromMemoryCache=false; // Display only: completed rows from a previous selection in this run.
    quint64 restRevision=0; // Model receipt ID; distinct responses may share a millisecond.
};
struct CandleArchiveStatus {
    QString message, detail;
    bool canRetry=false;
    bool operator==(const CandleArchiveStatus&) const = default;
};
// Ephemeral public presentation only. Never persisted or used for studies,
// alerts, account valuation, or the freshness of completed candle evidence.
struct MarketCandleLiveTrade {
    bool available=false;
    QString identity, price;
    qint64 intervalMs=0, tradeAtMs=0, receivedAtMs=0;
};
enum class CandleHistoryState { Ready, Loading, Delayed, RateLimited, End, Limit, Unavailable };
struct CandleHistoryStatus {
    CandleHistoryState state=CandleHistoryState::Unavailable;
    QString message;
    qint64 oldestAtMs=0, newestAtMs=0, retryAtMs=0;
    int completedCount=0;
};
enum class StoredCandlesOffer { Accepted, NotNow, Refused };
struct CandleCompression {
    bool available=false, compressed=false;
    double bandLow=0, bandHigh=0, channelLow=0, channelHigh=0;
    QString reason;
};
// Display-only study: BB(SMA20, population sigma*2) inside KC(EMA20, Wilder ATR20*1.5).
// EMA and ATR seed at the first 20 observations; a gap restarts the warm-up.
// These floating point study coordinates never enter account valuation.
CandleCompression candleCompression(const QList<MarketCandle>&, qint64 intervalMs=15*60000);
struct ChartStudyTimeline;
CandleCompression candleCompression(const QList<MarketCandle>&, qint64 intervalMs, const ChartStudyTimeline&);
class MarketCandles final : public QObject {
    Q_OBJECT
  public:
    static constexpr qint64 IntervalMs=15*60000, FreshnessMs=90000;
    static constexpr int HistoryLimit=5000, HistoryPageSize=200;
    using Clock = std::function<qint64()>;
    explicit MarketCandles(TradingTransport*, QObject* parent=nullptr);
    MarketCandles(TradingTransport*, std::unique_ptr<PublicCandleSocket>, QObject* parent=nullptr);
    MarketCandles(TradingTransport*, std::unique_ptr<PublicCandleSocket>, Clock, QObject* parent=nullptr);
    ~MarketCandles() override;
    void setAsset(const QString&); // Empty stops and clears. One selected public market only.
    void setSelection(const QString& fullIdentity, qint64 intervalMs);
    // Enables fail-closed production routing; call before the first selection.
    // Repeated price refreshes of identical metadata do not restart the chart.
    void setVerifiedUpbitMarkets(const QStringList& markets, bool available);
    void setInstruments(const QString& provider, const QList<MarketInstrument>&);
    MarketCandleSeries series(qint64 asOfMs=0) const;
    // Immutable validated REST observation, never the WebSocket-mutated chart data.
    // Cleared on selection/metadata revocation, connection reset or failed REST receipt.
    MarketCandleSeries restObservation() const;
    CandleArchiveStatus archiveStatus() const;
    void setArchiveStatus(const CandleArchiveStatus&); // Presentation only; not calculation input.
    void retryArchive();
    // Rendering only: same-selection completed history survives transient refresh
    // failures. A delayed observation remains unavailable to calculations; no
    // forming candle is retained. Selection/metadata revocation clears it.
    MarketCandleSeries displayObservation(qint64 asOfMs=0) const;
    MarketCandleLiveTrade liveTradeObservation(qint64 asOfMs=0) const;
    // Paging affects completed display history only. It never refreshes live
    // evidence, calculations, REST archive observations or alert freshness.
    void requestOlder();
    CandleHistoryStatus historyStatus() const;
    // Display only: at most 200 recent candles the price archive kept for this exact
    // selection, shown until the first completed observation arrives. Never enters
    // series(). NotNow means market metadata is still pending; retry on changed().
    StoredCandlesOffer setStoredCompleted(const MarketCandleSeries&);
    void clearStoredCompleted(); // Drops only archive rows, never a live observation.
    // Lock/session boundary: discard historical display memory and revoke old
    // REST/WS callbacks. A currently fresh public observation remains visible;
    // the same public selection then refreshes under a new generation.
    // Provider Retry-After deadlines deliberately survive this boundary.
    void clearDisplayCache();
    static RequestSpec request(const QString& asset);
    static RequestSpec request(const QString& fullIdentity, qint64 intervalMs);
    static RequestSpec historyRequest(const QString& fullIdentity, qint64 intervalMs, qint64 beforeMs);
    static bool isRequest(const RequestSpec&);
    static MarketCandleSeries parse(const QString& asset, const HttpResult&, qint64 receivedAtMs);
    static MarketCandleSeries parse(const QString& fullIdentity, qint64 intervalMs, const HttpResult&,
        qint64 receivedAtMs, qint64 requestStartedAtMs, const MarketInstrument& instrument = {});
  signals:
    void changed();
    void archiveStatusChanged();
    void historyChanged();
    void liveChanged(); // Coalesced display-only trade updates; no study refresh.
    void archiveRetryRequested();
  private:
    struct Private; std::unique_ptr<Private> d_;
};
} // namespace keefi
