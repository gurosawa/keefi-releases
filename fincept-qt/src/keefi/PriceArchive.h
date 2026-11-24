#pragma once
#include "keefi/MarketCandles.h"
#include <QObject>
#include <functional>
#include <memory>
namespace keefi {
class Vault;
struct PriceArchiveMerge {
    int added=0, unchanged=0, conflicts=0, droppedConflicts=0;
    int pending=0; // Closed rows not yet settled or not yet confirmed by a REST response.
    bool accepted=false;
};
// G9-B: completed public candles kept for later charts and event-time prices.
// Encrypted with a key derived from the vault's random ledger key; the archive never
// creates that key. Locking prevents any later archive replacement and drops
// pending rows, the key, decoded owner-thread rows and any archive rows already
// offered to the chart. Forming candles never enter. A different value for a stored
// candle is kept as a conflict and never replaces the first observation.
// Already dispatched ciphertext I/O may finish/clean up after lock; it cannot commit.
// All public methods and destruction run on the QObject's owning thread.
class PriceArchive final : public QObject {
    Q_OBJECT
  public:
    static constexpr qsizetype MaxFileBytes = 8 * 1024 * 1024, PaddingBytes = 64 * 1024;
    static constexpr int MaxSeries = 24, MaxCandlesPerSeries = 2000, MaxConflictsPerSeries = 64;
    // A row is kept only after its close plus SettleMs, from a REST response.
    static constexpr qint64 SettleMs = 30000, FlushDelayMs = 600000, RetryDelayMs = 600000;
    static constexpr int QuitWaitMs = 3000;
    explicit PriceArchive(Vault*, QString filePath, QObject* parent = nullptr);
    ~PriceArchive() override; // Writes pending rows only while the vault is still unlocked.
    QString filePath() const;
    bool exists() const;
    bool isOpen() const;
    QString lastError() const;
    // Synchronous: reads, decrypts and validates on the calling thread. With createIfMissing
    // an absent file is created and an unreadable one is set aside; otherwise both fail.
    bool open(bool createIfMissing, QString* error = nullptr);
    void lock();
    // In memory until written. The whole call is refused for an unsupported identity,
    // interval or source, or when one new row is invalid; nothing is partially merged.
    PriceArchiveMerge record(const MarketCandleSeries&, QString* error = nullptr);
    bool dirty() const;
    bool flush(QString* error = nullptr); // Synchronous; waits for background work first.
    // Quiesces collection and drains a save without synchronously waiting for workers. The
    // callback runs on the owner thread; failure/cancellation is never success.
    using FlushCallback = std::function<void(bool, const QString&)>;
    void flushAsync(FlushCallback);
    // Terminal, best-effort shutdown: dirty rows only, at most QuitWaitMs. Neither
    // failure nor timeout asks the user; an encrypted diagnostic is read next time.
    void prepareToQuit(FlushCallback);
    void cancelQuit(bool timedOut = false); // Deadline/direct application exit; never waits for a worker.
    void retry(); // Explicit retry, without changing permissions or creating a key.
    // The last uninterrupted run of at most maxCount stored candles; empty when locked.
    MarketCandleSeries stored(const QString& identity, qint64 intervalMs, int maxCount = 200) const;
    int storedCount(const QString& identity, qint64 intervalMs) const;
    int conflictCount(const QString& identity, qint64 intervalMs) const;
    int conflictOverflow(const QString& identity, qint64 intervalMs) const; // Revisions past the conflict bound.
    // While the vault is unlocked: opens and writes on a worker thread, records closed
    // rows from REST responses and offers stored rows to a new selection for display only.
    void watch(MarketCandles*);
    void setFlushDelayMs(int); // Background save delay after new rows; FlushDelayMs by default.
  signals:
    void changed();
    void locked();
    void written(bool success);
  private:
    friend class PriceArchiveTests;
    // Owner-thread deterministic regression seams; unset in production.
    void setBeforeCommitForTest(std::function<void()>);
    void setAfterSnapshotForTest(std::function<void()>);
    void setRetryDelayForTest(int);
    void setPrepareWorkerForTest(std::function<void()>);
    void setVerifyWorkerForTest(std::function<void()>);
    void startWriteForTest();
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
