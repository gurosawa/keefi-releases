#pragma once

#include "keefi/TradingTypes.h"
#include <QDate>
#include <QObject>
#include <array>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;

// Keyless, display-only world market references for the Today page.
// - Indices, commodity futures and Hana Bank KRW postings: Naver Securities'
//   public pages. Not an official API, so every response is shape-checked and a
//   failure is shown instead of a guessed value.
// - Dollar index: ICE's six-currency formula applied to ECB daily reference rates.
// - Japan 10-year yield: Japan Ministry of Finance daily JGB yield table.
// Nothing here feeds alerts, history, orders or the private portfolio.
enum class WorldMarketItem { Spx, Ndx, Kospi, Nikkei, HangSeng, Ftse, Gold, Silver, Wti, UsdKrw, JpyKrw, CnyKrw, AudKrw };
inline constexpr int WorldMarketItemCount = 13;
// PreOpen: the venue's pre-market window. The provider then repeats the last close
// with a 0.00 change and the current clock, which is not a trade.
enum class WorldMarketSession { Unknown, Open, Closed, PreOpen };

struct WorldMarketQuote {
    QString price;         // Plain decimal, never grouped: "7688.62".
    QString change;        // Signed decimal against the provider's previous close.
    QString changePercent; // Signed decimal.
    WorldMarketSession session = WorldMarketSession::Unknown;
    qint64 tradedAtMs = 0; // Provider trade or posting time.
    QDate localDate;       // Venue-local date of that time, as the provider wrote it.
    int delayMinutes = 0;  // Provider-declared delay; 0 is real time.
    QString venue;         // Provider-reported venue, e.g. 뉴욕 거래소, 하나은행.
    qint64 receivedAtMs = 0;
    bool available() const { return !price.isEmpty() && tradedAtMs > 0 && receivedAtMs > 0; }
    bool operator==(const WorldMarketQuote&) const = default;
};

// An approximation: the ECB fixes once per business day (about 14:15 CET), not
// at ICE's close, so the value differs slightly from the traded index.
struct WorldDollarIndex {
    QString value, previousValue, changePercent;
    QDate date, previousDate;
    qint64 receivedAtMs = 0;
    bool available() const { return !value.isEmpty() && date.isValid() && receivedAtMs > 0; }
    bool operator==(const WorldDollarIndex&) const = default;
};

// Japan MOF publishes one row per Japanese business day (after the close).
struct WorldJapanYield {
    QString tenYear, previousTenYear; // Percent, as published.
    QDate date, previousDate;
    qint64 receivedAtMs = 0;
    bool available() const { return !tenYear.isEmpty() && date.isValid() && receivedAtMs > 0; }
    bool operator==(const WorldJapanYield&) const = default;
};

// Which reads are wanted right now. Today's board wants everything; the chart header
// only the won rate, the indices it names and the dollar index. Nothing else is read.
struct WorldMarketsDemand {
    QList<WorldMarketItem> quotes;
    bool dollarIndex = false, japanYield = false;
    bool any() const { return !quotes.isEmpty() || dollarIndex || japanYield; }
    bool wants(WorldMarketItem item) const { return quotes.contains(item); }
    static WorldMarketsDemand full();
    static WorldMarketsDemand chartHeader();
    bool operator==(const WorldMarketsDemand&) const = default;
};

struct WorldMarketsSnapshot {
    std::array<WorldMarketQuote, WorldMarketItemCount> quotes;
    std::array<QString, WorldMarketItemCount> failures; // Latest failure; a retained quote may still be shown.
    WorldDollarIndex dollarIndex;
    QString dollarIndexFailure;
    WorldJapanYield japanYield;
    QString japanYieldFailure;
    bool active = false;
    const WorldMarketQuote& quote(WorldMarketItem item) const { return quotes[int(item)]; }
    QString failure(WorldMarketItem item) const { return failures[int(item)]; }
};

class WorldMarkets final : public QObject {
    Q_OBJECT
  public:
    using Clock = std::function<qint64()>;
    static constexpr qint64 RefreshMs = 60000;        // Open venues and bank postings.
    static constexpr qint64 ClosedRefreshMs = 600000; // A closed venue's last value rarely changes.
    static constexpr qint64 DailyRefreshMs = 3600000; // ECB and MOF publish once per business day.
    static constexpr qint64 MinimumRetryMs = 60000;
    static constexpr qint64 MaximumRetryMs = 86400000;
    static constexpr qsizetype MaximumNaverBytes = 262144;
    static constexpr qsizetype MaximumEcbBytes = 524288;
    static constexpr qsizetype MaximumMofBytes = 65536;
    static constexpr int MaximumDailyAgeDays = 10; // Long holiday runs, never a stale month.
    // One transport per lane: a NetworkTradingTransport runs one request at a time,
    // so a slow daily table can never hold the price lane behind it.
    WorldMarkets(TradingTransport* naver, TradingTransport* ecb, TradingTransport* mof,
                 QObject* parent = nullptr, Clock clock = {});
    ~WorldMarkets() override;
    void setActive(bool); // true = WorldMarketsDemand::full(), false = nothing.
    bool active() const;
    void setDemand(const WorldMarketsDemand&);
    WorldMarketsDemand demand() const;
    void refresh(); // Sends the next due request per lane; never a duplicate in-flight one.
    WorldMarketsSnapshot snapshot() const;

    static QString itemName(WorldMarketItem);
    static RequestSpec request(WorldMarketItem);
    static RequestSpec ecbRequest();
    static RequestSpec mofRequest();
    static bool isRequest(const RequestSpec&);
    static bool parseQuote(WorldMarketItem, const QByteArray&, qint64 receivedAtMs, WorldMarketQuote*, QString* reason = nullptr);
    static bool parseEcbDollarIndex(const QByteArray&, qint64 receivedAtMs, WorldDollarIndex*, QString* reason = nullptr);
    static bool parseJapanYield(const QByteArray&, qint64 receivedAtMs, WorldJapanYield*, QString* reason = nullptr);
    // (base KRW per baseUnit) ÷ (quote KRW per quoteUnit), e.g. USD/JPY from 원/달러 and
    // 원/100엔. The previous value uses each leg's own previous posting. Two decimals.
    static WorldMarketQuote cross(const WorldMarketQuote& baseKrw, int baseUnit,
                                  const WorldMarketQuote& quoteKrw, int quoteUnit);
    // (USDT/KRW ÷ USD/KRW − 1) × 100, two decimals; empty when an input is unusable.
    static QString premiumPercent(const QString& usdtKrw, const QString& usdKrw);
    // a − b in percentage points, three decimals with trailing zeros removed; may be negative.
    static QString spreadPoints(const QString& aPercent, const QString& bPercent);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
