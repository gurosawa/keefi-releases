#pragma once
#include "keefi/TradingTypes.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
enum class OfficialCalendarSource { Bls, Bea, Fomc, Bok };
struct OfficialCalendarEvent {
    QString uid, title, sourceTimeZone;
    qint64 startsAtMs = 0, updatedAtMs = 0;
    int sequence = 0;
    bool cancelled = false;
    bool dateOnly = false; // The source publishes a date only; startsAtMs is 00:00 in sourceTimeZone.
};
struct OfficialCalendarFeed {
    OfficialCalendarSource source = OfficialCalendarSource::Bls;
    bool available = false, loading = false;
    qint64 receivedAtMs = 0, sourceUpdatedAtMs = 0;
    QString reason;
    QList<OfficialCalendarEvent> events;
};
// Official published schedules only. No actual/previous/consensus values,
// portfolio inputs, credentials or server are involved.
class OfficialCalendar final : public QObject {
    Q_OBJECT
  public:
    static constexpr qint64 FreshnessMs = 6 * 3600000LL; // App display policy, not a provider guarantee.
    using Clock = std::function<qint64()>;
    explicit OfficialCalendar(TradingTransport*, QObject* parent = nullptr, Clock clock = {});
    ~OfficialCalendar() override;
    void setActive(bool active);
    void refresh();
    // Only KST today through the following 30 days; expired/error feeds have no events.
    OfficialCalendarFeed feed(OfficialCalendarSource, qint64 asOfMs = 0) const;
    // The same received feed, also including this week's earlier KST dates.
    // No extra requests and no claim that a scheduled release actually occurred.
    OfficialCalendarFeed weekFeed(OfficialCalendarSource, qint64 asOfMs = 0) const;
    static QString sourceName(OfficialCalendarSource);
    // year selects the Bank of Korea list (0 = current KST year); other sources ignore it.
    static RequestSpec request(OfficialCalendarSource, int year = 0);
    static bool isRequest(const RequestSpec&);
    // Strict finite VEVENT subset of RFC 5545. No floating/all-day/recurring
    // times, guessed timezone aliases, or partial success for malformed feeds.
    static bool parse(OfficialCalendarSource, const QByteArray&, qint64 receivedAtMs, OfficialCalendarFeed*);
    // Federal Reserve FOMC calendar page (HTML). Regular two-day meetings only;
    // the statement time is the Fed's standing 14:00 America/New_York release.
    static bool parseFomc(const QByteArray&, qint64 receivedAtMs, OfficialCalendarFeed*);
    // Bank of Korea monetary policy meeting list (HTML) for the KST year of receipt.
    // Dates only (no published time); every row must parse with a matching weekday.
    static bool parseBok(const QByteArray&, qint64 receivedAtMs, OfficialCalendarFeed*);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
