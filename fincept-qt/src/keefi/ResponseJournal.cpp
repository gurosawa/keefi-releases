#include "keefi/ResponseJournal.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QMutex>
#include <QUrl>

namespace keefi {
namespace {
QMutex& guard() { static QMutex mutex; return mutex; }
bool& switchedOn() { static bool on = false; return on; }
QList<JournalEntry>& store() { static QList<JournalEntry> entries; return entries; }
qsizetype& storedBytes() { static qsizetype bytes = 0; return bytes; }
}

void ResponseJournal::setEnabled(bool enabled) {
    QMutexLocker lock(&guard());
    switchedOn() = enabled;
    if (!enabled) { store().clear(); storedBytes() = 0; }
}

bool ResponseJournal::enabled() { QMutexLocker lock(&guard()); return switchedOn(); }

void ResponseJournal::record(const QString& channel, const QString& operation, const QByteArray& method,
                             const QUrl& url, bool authenticated, const HttpResult& result) {
    QMutexLocker lock(&guard());
    if (!switchedOn()) return;
    JournalEntry entry;
    entry.channel = channel; entry.operation = operation; entry.method = QString::fromLatin1(method);
    entry.host = url.host(); entry.path = url.path();
    entry.status = result.status; entry.authenticated = authenticated;
    entry.timedOut = result.timedOut; entry.networkError = result.networkError;
    entry.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
    entry.bodySize = result.body.size();
    if (!result.body.isEmpty()) entry.bodySha256 = QCryptographicHash::hash(result.body, QCryptographicHash::Sha256).toHex();
    // Large public catalogues (Binance exchangeInfo) keep only their size and digest.
    entry.bodyOmitted = result.body.size() > BodyLimit;
    if (!entry.bodyOmitted) entry.body = result.body;
    auto& entries = store();
    entries.append(entry);
    storedBytes() += entry.body.size();
    while (!entries.isEmpty() && (entries.size() > EntryLimit || storedBytes() > TotalLimit)) {
        storedBytes() -= entries.first().body.size();
        entries.removeFirst();
    }
}

void ResponseJournal::recordTrading(Operation operation, const QByteArray& method, const QUrl& url,
                                    bool authenticated, const HttpResult& result) {
    // Sign-in replies (KIS, LS, Toss access_token; Kiwoom token) carry reusable session
    // credentials. They are dropped here, before any copy exists, whatever the status.
    // Private Earn and deposit/withdrawal responses (addresses, txids, amounts)
    // are omitted before body copies or hashes exist.
    if (operation == Operation::Authenticate || operation == Operation::EarnRead || operation == Operation::FundFlowRead) return;
    record("trading", operationName(operation), method, url, authenticated, result);
}

QList<JournalEntry> ResponseJournal::entries() { QMutexLocker lock(&guard()); return store(); }

void ResponseJournal::clear() { QMutexLocker lock(&guard()); store().clear(); storedBytes() = 0; }
} // namespace keefi
