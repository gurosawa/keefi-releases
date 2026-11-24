#pragma once
#include "keefi/SwapTypes.h"
#include <QObject>
#include <memory>
namespace keefi {
class Vault;
class SwapJournal final : public QObject {
    Q_OBJECT
  public:
    static constexpr qsizetype MaxFileBytes = 2 * 1024 * 1024, MaxRecords = 1024;
    explicit SwapJournal(Vault*, QString filePath, QObject* parent = nullptr);
    ~SwapJournal() override;
    bool isOpen() const;
    bool open(bool createIfMissing, QString* error = nullptr);
    QList<SwapPendingRecord> records() const;
    bool upsert(const SwapPendingRecord&, QString* error = nullptr);
    void lock();
  signals:
    void changed();
    void locked();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
