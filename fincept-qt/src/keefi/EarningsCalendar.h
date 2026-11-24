#pragma once
#include "keefi/TradingTypes.h"
#include <QDate>
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
enum class EarningsSource { Kind, Nasdaq };
enum class EarningsTiming { Scheduled, PreMarket, AfterHours, NotSupplied };
// One published earnings date. Korean rows are KRX KIND investor-relations
// sessions (KST date and start time); US rows are Nasdaq earnings dates in the
// New York calendar with only a before-open/after-close marker.
struct EarningsEvent {
    EarningsSource source = EarningsSource::Kind;
    QString code, name, title; // Kind: KIND 5-character issuer code. Nasdaq: ticker.
    QDate date;
    EarningsTiming timing = EarningsTiming::NotSupplied;
    qint64 startsAtMs = 0; // Kind: published KST start, or 00:00 KST. Nasdaq: ordering only, never shown as a time.
    double marketCapUsd = 0; // Nasdaq only.
    QString fiscalQuarter, epsForecast, estimates; // Nasdaq only, as published.
};
struct EarningsFeed {
    EarningsSource source = EarningsSource::Kind;
    bool available = false, loading = false;
    qint64 receivedAtMs = 0;
    QString reason;
    QList<EarningsEvent> events;
};
// Keyless public schedules only: KIND's KOSPI IR list (one fixed search form) and
// Nasdaq's daily earnings calendar. No account, portfolio or credential input.
class EarningsCalendar final : public QObject {
    Q_OBJECT
  public:
    static constexpr int Days = 7, KindPageSize = 100, KindMaxPages = 5;
    static constexpr qint64 FreshnessMs = 6 * 3600000LL; // App display policy, not a provider guarantee.
    using Clock = std::function<qint64()>;
    explicit EarningsCalendar(TradingTransport*, QObject* parent = nullptr, Clock clock = {});
    ~EarningsCalendar() override;
    void setActive(bool active);
    void refresh();
    // Events from today (KST for Kind, New York for Nasdaq) through the next Days days.
    EarningsFeed feed(EarningsSource, qint64 asOfMs = 0) const;
    static QString sourceName(EarningsSource);
    static RequestSpec kindRequest(const QDate& from, const QDate& to, int page);
    static RequestSpec nasdaqRequest(const QDate& date);
    static bool isRequest(const RequestSpec&);
    // All rows or nothing. total receives KIND's "전체 N건" count for paging.
    static bool parseKind(const QByteArray&, const QDate& from, const QDate& to, QList<EarningsEvent>*, int* total);
    static bool parseNasdaq(const QByteArray&, const QDate& date, QList<EarningsEvent>*);
    // KIND lists every IR session; only results-related titles are earnings.
    static bool earningsTitle(const QString&);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
