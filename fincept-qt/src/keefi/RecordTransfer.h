#pragma once
#include "keefi/SecureBuffer.h"
#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QStringList>
#include <atomic>
#include <memory>
#include <functional>

namespace keefi {
class Vault;
class AssetsLedger;
class SwapJournal;
struct RecordTransferSelection {
    bool snapshots=true, events=true, orders=true, swaps=true, pending=true;
};
struct RecordTransferBundle {
    QJsonArray records;
    QStringList sourceSummary; // collection receipt, no paths or credentials; not executable metadata
};
struct RecordTransferResult { int added=0, duplicates=0, total=0; };
using RecordTransferCancellation = std::shared_ptr<std::atomic_bool>;

// Instance methods: owner Qt thread only, like Vault. collect() requires the caller's
// completed storage checkpoint. Codec/file static methods: worker-safe; immutable
// snapshots only, never Vault/QObject access. Caller keeps password in SecureBuffer,
// clears widgets, and discards worker results after lock/cancel/generation changes.
class RecordTransfer final : public QObject {
    Q_OBJECT
  public:
    static constexpr qsizetype MaxFileBytes=64*1024*1024, MaxRecords=100000, MaxRecordBytes=256*1024, MaxSnapshotBytes=8*1024*1024;
    explicit RecordTransfer(Vault&, QObject* parent=nullptr);
    ~RecordTransfer() override;
    bool exists() const;
    bool isOpen() const;
    QString filePath() const;
    quint64 generation() const;
    // Explicit preparation: may provision a local record key and create an empty
    // encrypted store. Never called implicitly by import/decode/commit.
    bool initialize(QString* error=nullptr);
    bool open(QString* error=nullptr);
    void lock();
    QJsonArray records() const;
    bool collect(const RecordTransferSelection&, AssetsLedger* sharedHistory,
                 SwapJournal* sharedSwap, RecordTransferBundle*, QString* error=nullptr,
                 AssetsLedger* sharedLatest=nullptr);
    bool commit(const RecordTransferBundle&, quint64 expectedGeneration,
                RecordTransferResult* result=nullptr, QString* error=nullptr,
                const RecordTransferCancellation& cancel={},
                const std::function<bool()>& precommitCheck={});
    static bool encodePackage(const RecordTransferBundle&, const SecureBuffer& password,
                              QByteArray* ciphertext, QString* error=nullptr,
                              const RecordTransferCancellation& cancel={});
    static bool decodePackage(const QByteArray& ciphertext, const SecureBuffer& password,
                              RecordTransferBundle*, QString* error=nullptr,
                              const RecordTransferCancellation& cancel={});
    static bool readPackageFile(const QString&, QByteArray*, QString* error=nullptr,
                                const RecordTransferCancellation& cancel={});
    // Export creates only a new file; callers choose a fresh destination.
    static bool writePackageFile(const QString&, const QByteArray&, QString* error=nullptr,
                                 const RecordTransferCancellation& cancel={});
  signals:
    void changed();
    void locked();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
