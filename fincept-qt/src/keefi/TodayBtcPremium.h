#pragma once

#include "keefi/AssetValuation.h"
#include "keefi/TradingTypes.h"
#include <QDate>
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;

// Hana Bank's KRW per USD posting (매매기준율) as Naver Securities publishes it.
// A bank posting, not an interbank quote: it changes per posting round and stays
// at the last round over weekends and holidays.
struct UsdKrwPosting {
    QString krwPerUsd;
    qint64 postedAtMs = 0;
    QDate postedDate; // KST date of the posting.
    qint64 receivedAtMs = 0;
};
struct TodayBtcPremiumSnapshot {
    // A retained number is explicitly historical. It is never an alert input.
    enum class State { Waiting, Current, Recent };
    State state = State::Waiting;
    QString percent, reason;
    qint64 checkedAtMs = 0;
    AssetQuote upbit, binance;
    UsdKrwPosting fx;
};

// Public display-only BTC reference. It deliberately has no selected-asset API,
// portfolio dependency, history/alert sink, USDT/KRW input, or private credentials.
// Since 2026-11-06 the won leg is Hana Bank's posting (was the ECB daily reference),
// so the premium follows the won within the day.
class TodayBtcPremium final : public QObject {
    Q_OBJECT
  public:
    enum class Read { UpbitBtc, BinanceBtc, Fx };
    using Clock = std::function<qint64()>;
    static constexpr qint64 QuoteFreshnessMs = 180000;
    static constexpr qint64 MaximumQuoteSkewMs = 30000;
    static constexpr qint64 QuoteRefreshMs = 15000;
    static constexpr qint64 FxRefreshMs = 60000;          // Re-read the posting every minute.
    static constexpr qint64 FxReceiptFreshnessMs = 180000; // Not re-read for three minutes: late.
    static constexpr int MaximumFxCalendarAgeDays = 7;      // Posting date (KST); covers long holidays.
    static constexpr qint64 LateMs = 30000;
    // An exchange clock can run a little ahead of this computer's. A BTC reply stamped at
    // most this far ahead waits, unparsed, until the local clock reaches its time, then
    // meets the unchanged strict parser; monotonic time ends the wait at this bound even
    // if the wall clock steps back. A larger lead is rejected with the gap named. Same
    // rule as PublicFlowFrameQueue. The Hana posting lane keeps rejecting any future time.
    static constexpr qint64 ClockLeadHoldMs = 2000;
    // Each lane must own a separate transport: a slow FX request cannot cancel
    // a price request or hold the next quote refresh behind it. `elapsed` is a monotonic
    // millisecond clock (tests inject it; by default a QElapsedTimer).
    TodayBtcPremium(TradingTransport* upbit, TradingTransport* binance,
                    TradingTransport* fx, QObject* parent = nullptr, Clock clock = {},
                    Clock elapsed = {});
    ~TodayBtcPremium() override;
    void setActive(bool);
    bool active() const;
    void refresh(); // Due lanes only; duplicate/in-flight requests are not sent.
    TodayBtcPremiumSnapshot snapshot(qint64 nowMs = 0) const;
    static RequestSpec request(Read);
    static bool isRequest(const RequestSpec&);
    static bool parseFxPosting(const QByteArray&, qint64 receivedAtMs,
                               UsdKrwPosting*, QString* reason = nullptr);
    static bool parseQuote(Read, const QByteArray&, qint64 receivedAtMs, AssetQuote*);
    static QString percent(const QString& upbitKrw, const QString& binanceUsdt,
                           const QString& usdKrw);
  signals:
    void changed();
  private:
    void settle(int lane, const HttpResult& response, qint64 now, qint64 receivedAtMs);
    void releaseHeld();
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
