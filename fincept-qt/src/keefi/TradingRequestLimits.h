#pragma once
#include "keefi/TradingTypes.h"
#include <QMutex>
#include <functional>
#include <memory>

namespace keefi {
// One in-process cooldown shared by every network transport. It stores only
// public service origins and monotonic deadlines, never credentials or payloads.
// It does not claim to predict all endpoint weights or coordinate other PCs/apps.
class TradingRequestLimits final {
public:
    using Clock = std::function<qint64()>;
    explicit TradingRequestLimits(Clock clock = {});
    static std::shared_ptr<TradingRequestLimits> shared();
    static qint64 retryAfterMs(const HttpResult&);
    qint64 remainingMs(const QUrl&) const;
    void observe(const QUrl&, const HttpResult&);
private:
    static QString service(const QUrl&);
    Clock clock_;
    mutable QMutex mutex_;
    QMap<QString, qint64> deadlines_;
};
}
