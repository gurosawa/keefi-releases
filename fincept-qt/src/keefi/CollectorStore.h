#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

namespace keefi {
// Public-market collector records (COLLECTOR-COMPARE-RULES-2026-11-06.md, 5절).
// Hourly UTC segments are appended while open (.part) and sealed after the hour
// plus a settle delay: gzip, SHA-256, one manifest line, read-only, .part removed.
// Every step is restart-safe: open() finishes or cleans a seal that stopped part
// way, so each record is read exactly once. A sealed file is never changed; data
// for a sealed hour goes to a numbered late segment.
enum class CollectorSealFault { None, AfterCompressedFile, AfterManifestLine, AfterReadOnly }; // Tests only.

struct CollectorStoreOptions {
    QString root;                                       // .../KEEFI-Collector/v1
    qint64 settleMs = 5 * 60000;
    qint64 diskFloorBytes = 30LL * 1024 * 1024 * 1024; // Raw trades pause below this.
    QString collectorVersion, algorithmVersion;
    int schemaVersion = 1;
    CollectorSealFault testSealFault = CollectorSealFault::None; // Simulates a stop inside a seal.
};

struct CollectorKindStats {
    qint64 records = 0, rawBytes = 0, sealedBytes = 0, sealedRawBytes = 0;
    int sealedSegments = 0;
};

struct CollectorStoredRecord {
    QString kind, venue, symbol;
    qint64 hourStartMs = 0;
    bool sealed = false;
    QJsonObject object;
};

class CollectorStore final {
  public:
    using Clock = std::function<qint64()>;
    using FreeBytes = std::function<qint64()>;
    explicit CollectorStore(CollectorStoreOptions, Clock = {}, FreeBytes = {});
    ~CollectorStore();
    bool open(QString* error = nullptr); // Single instance; fails closed if .part recovery cannot be committed.
    bool isOpen() const;
    void close(); // Flushes; closes an open disk-pause gap; keeps .part files for the next start.
    // One compact JSON object per call; it must carry "r" (receivedAtMs).
    // bucketAtMs picks the UTC hour. Raw trades ("trade") are refused while the
    // free space is under the floor; the pause is written as gap records.
    bool append(const QString& kind, const QString& venue, const QString& symbol,
                qint64 bucketAtMs, qint64 receivedAtMs, const QByteArray& compactJson);
    int sealDue();
    void flush();
    bool rawPaused() const;
    bool writeStatus(const QJsonObject& status); // Adds rawPaused / rawPausedSince.
    // Sealed (hash-verified) and still-open records of one kind from sinceHourMs on.
    QList<CollectorStoredRecord> recentRecords(const QString& kind, qint64 sinceHourMs);
    QStringList recoveryNotes() const;
    QHash<QString, CollectorKindStats> stats() const;
    QString root() const;
    QString lastError() const;
    static QString relativeBase(const QString& kind, const QString& venue, const QString& symbol, qint64 hourStartMs, int late = 0);
    static bool validName(const QString&);

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};

// First-observation ledger for raw trades (규칙서 5.6). Remembers the content of
// each (venue, symbol, id) it has written, including ones reloaded from the store
// after a restart, so a re-sent trade is counted instead of written twice and a
// different payload under the same ID is reported as a conflict.
class CollectorTradeLedger final {
  public:
    enum class Outcome { New, Duplicate, Conflict, Reinspection };
    static QByteArray content(const QString& price, const QString& quantity, const QString& normalQuantity, const QString& side,
                              qint64 tradeAtMs);
    static QByteArray content(const QJsonObject& storedTrade); // Keys p, q, n, d, T.
    void remember(const QString& venue, const QString& symbol, const QString& id, const QByteArray& content, qint64 tradeAtMs);
    // Checking a delivery must not claim a raw record before its append succeeds.
    // A positive receive time plus epoch identifies the observation retained in
    // a source window, so polling that window again is not another delivery.
    Outcome check(const QString& venue, const QString& symbol, const QString& id, const QByteArray& content,
                  QByteArray* firstContent = nullptr, qint64 receivedAtMs = 0, quint64 epoch = 0) const;
    Outcome observe(const QString& venue, const QString& symbol, const QString& id, const QByteArray& content, qint64 tradeAtMs,
                    QByteArray* firstContent = nullptr, qint64 receivedAtMs = 0, quint64 epoch = 0, qint64 inspectedAtMs = 0);
    void prune(qint64 olderThanTradeAtMs);
    qint64 duplicates(const QString& venue, const QString& symbol) const;
    qint64 conflicts(const QString& venue, const QString& symbol) const;
    int size() const { return int(entries_.size()); }

  private:
    struct Entry {
        QByteArray content;
        qint64 tradeAtMs = 0;
        QByteArray lastContent;
        qint64 lastReceivedAtMs = 0, lastInspectedAtMs = 0;
        quint64 lastEpoch = 0;
    };
    QHash<QString, Entry> entries_;
    QHash<QString, qint64> duplicates_, conflicts_;
};

// Durable event identity is reconstructed from saved events after a restart.
// A group's end, a grade change and an outcome remain separate observations.
class CollectorEventLedger final {
  public:
    bool contains(const QString& venue, const QString& symbol, const QJsonObject& event) const;
    void remember(const QString& venue, const QString& symbol, const QJsonObject& event);
  private:
    static QByteArray key(const QString& venue, const QString& symbol, const QJsonObject& event);
    QSet<QByteArray> keys_;
};

struct CollectorSegment {
    QString path, kind, venue, symbol, sha256, origin, collectorVersion, algorithmVersion;
    qint64 hourStartMs = 0, records = 0, bytes = 0, firstReceivedAtMs = 0, lastReceivedAtMs = 0, sealedAtMs = 0;
    int late = 0, schema = 0;
};

class CollectorReader final {
  public:
    static constexpr qsizetype MaximumSegmentBytes = 512LL * 1024 * 1024;
    explicit CollectorReader(QString root);
    QList<CollectorSegment> segments(QString* reason = nullptr) const;
    // Verifies the manifest SHA-256 before decompressing. Never reads .part files.
    QList<QJsonObject> read(const CollectorSegment&, QString* reason = nullptr) const;
    QList<QJsonObject> records(const QString& kind, const QString& venue, const QString& symbol,
                               qint64 fromHourMs, qint64 toHourMs, QString* reason = nullptr) const;
    QJsonObject status(QString* reason = nullptr) const;
    QString root() const { return root_; }

  private:
    QString root_;
};

QByteArray collectorGzip(const QByteArray&, bool* ok = nullptr);
QByteArray collectorGunzip(const QByteArray&, qsizetype maximumBytes, bool* ok = nullptr);
QString collectorDefaultRoot(); // ~/Library/Application Support/KEEFI-Collector/v1
} // namespace keefi
