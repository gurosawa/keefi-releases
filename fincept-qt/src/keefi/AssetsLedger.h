#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/EarnTracking.h"
#include "keefi/FundFlowTracking.h"
#include <QObject>
#include <memory>
#include <functional>

namespace keefi {
class Vault;
class SecureBuffer;
// No network or credentials. Only explicit open/append writes; lock erases the key and plaintext.
class AssetsLedger final : public QObject {
    Q_OBJECT
  public:
    static constexpr qsizetype MaxFileBytes = 16 * 1024 * 1024;
    static constexpr qsizetype MaxSnapshots = 2048, MaxEvents = 20000, MaxQuotes = 20000, MaxOrders = 20000;
    explicit AssetsLedger(Vault*, QString filePath, QObject* parent = nullptr);
    ~AssetsLedger() override;
    bool isOpen() const;
    bool exists() const;
    QString filePath() const;
    bool open(bool createIfMissing, QString* error = nullptr);
    void lock();
    // IDs are idempotent only for exactly identical records. Conflicts fail without overwriting.
    bool append(const QList<AssetSnapshot>&, const QList<AssetEvent>&,
                const QList<AssetQuote>&, QString* error = nullptr);
    bool appendBatch(const QList<AssetSnapshot>&, const QList<AssetEvent>&,
                     const QList<AssetQuote>&, const QList<AssetOrderObservation>&, QString* error = nullptr);
    // Only for the separate current-state cache, never used to prune history.
    bool replaceLatest(const QList<AssetSnapshot>&,const QList<AssetQuote>&,QString* error=nullptr,bool background=false);
    bool appendWithArchive(const QList<AssetSnapshot>&,const QList<AssetEvent>&,
        const QList<AssetQuote>&,const QList<AssetOrderObservation>&,QString* error=nullptr,bool background=false);
    QStringList archiveFiles() const;
    struct HistoryWindow {
        QList<AssetSnapshot> snapshots;
        QList<AssetQuote> quotes;
        qint64 nextEligibleMs = 0; // Earliest later evidence; no out-of-window record is returned.
    };
    // Read-only, inclusive interval from this ledger and its archives while the
    // vault is unlocked. A closed ledger can be read without opening a write lock.
    // No history files means a successful empty window. Archives
    // have no date index: each encrypted container must be authenticated in memory,
    // but only in-range records are returned. No files or locks are created.
    // Snapshot time / quote receipt time selects the interval; future evidence is
    // omitted. The caller must also apply each checkpoint's own as-of cutoff.
    // Any unreadable archive or conflicting record fails the whole read and clears output.
    bool readWindow(qint64 fromMs, qint64 toMs, HistoryWindow* output, QString* error = nullptr) const;
    using HistoryWindowCallback = std::function<void(bool, HistoryWindow, QString)>;
    // Deferred, read-only equivalent. Replaces a pending request; lock/cancel
    // discard it without a callback. Completion runs on this object's thread.
    // Ciphertext I/O is off-thread; authentication/filtering yields between files.
    void readWindowAsync(qint64 fromMs, qint64 toMs, HistoryWindowCallback);
    void cancelWindowRead();
    bool isWriting() const;
    QList<AssetSnapshot> snapshots() const;
    QList<AssetEvent> events() const;
    QList<AssetQuote> quotes() const;
    QList<AssetOrderObservation> orders() const;
    // Schema 3 collection state is never included in the current valuation or
    // G4 automatically. The complete expected revision must match at dispatch.
    enum class CollectionWrite { Failed, Unchanged, Pending };
    QList<EarnAccountState> earnStates() const;
    CollectionWrite saveEarnState(const EarnAccountState&, quint64 expectedRevision,
                                 quint64 receipt, QString* error = nullptr);
    // G9-C1 deposit/withdrawal collection, one state per account scope.
    QList<FlowAccountState> flowStates() const;
    CollectionWrite saveFlowState(const FlowAccountState&, quint64 expectedRevision,
                                  quint64 receipt, QString* error = nullptr);
    // The fixed coin-performance start. Empty means none has been fixed yet.
    QJsonObject coinBaseline() const;
    // Everything coin performance reads, from one authenticated decode. Closed
    // ledgers use an existing key without creating files, keys or a writer lock.
    struct CoinInputs {
        QList<AssetSnapshot> snapshots; QList<AssetQuote> quotes;
        QList<EarnAccountState> earn; QList<FlowAccountState> flows; QJsonObject baseline;
    };
    CoinInputs coinInputs(QString* error = nullptr) const;
    CollectionWrite saveCoinBaseline(const QJsonObject&, quint64 receipt, QString* error = nullptr);
  signals:
    void changed();
    void locked();
    void writeFinished(bool success,const QString& message);
    void collectionWriteFinished(quint64 receipt, bool success, const QString& message);
  private:
    struct Private;
    std::unique_ptr<Private> d_;
    void readWindowStep();
    void finishWindowRead(bool success,const QString& error);
    bool persistAsync(SecureBuffer&& candidate,const QString& archivePath,const QByteArray& archiveBytes,QString* error,quint64 receipt=0);
    bool prepareSchema3(QJsonObject* migration,QString* error);
};
} // namespace keefi
