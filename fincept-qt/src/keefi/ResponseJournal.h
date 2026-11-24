#pragma once
#include "keefi/TradingTypes.h"
#include <QList>

namespace keefi {
// One network response kept for the valuation diagnostic export. Only the
// host and path of the request are kept: query strings, headers and request
// bodies carry signatures, keys, account numbers and wallet addresses.
struct JournalEntry {
    QString channel, operation, method, host, path;
    int status = 0;
    bool authenticated = false, timedOut = false, networkError = false, bodyOmitted = false;
    qint64 receivedAtMs = 0;
    qsizetype bodySize = 0;
    QByteArray body, bodySha256;
};

// Opt-in, in-memory journal of recent responses. Off by default and never
// written to disk by itself; disabling or clearing drops every entry. The
// window turns it off when the vault locks.
class ResponseJournal final {
  public:
    static constexpr int EntryLimit = 400;
    static constexpr qsizetype BodyLimit = 2 * 1024 * 1024;
    static constexpr qsizetype TotalLimit = 64 * 1024 * 1024;
    static void setEnabled(bool enabled);
    static bool enabled();
    static void record(const QString& channel, const QString& operation, const QByteArray& method,
                       const QUrl& url, bool authenticated, const HttpResult& result);
    // The path NetworkTradingTransport uses. Authentication responses are never kept;
    // the decision uses the request operation, not its display name.
    static void recordTrading(Operation operation, const QByteArray& method, const QUrl& url,
                              bool authenticated, const HttpResult& result);
    static QList<JournalEntry> entries();
    static void clear();
};
} // namespace keefi
