#pragma once
#include "keefi/ChartAlertEngine.h"
#include "keefi/PublicQuoteStream.h"
#include <functional>
#include <memory>

namespace keefi {
struct RapidMoveSnapshot {
    bool available = false;
    QString reason, changePercent, quoteTurnover, price;
    MovementDirection direction = MovementDirection::Unknown;
    qint64 closedMinuteMs = 0, fromObservedAtMs = 0, throughObservedAtMs = 0, receivedAtMs = 0;
    int completedMinutes = 0;
};
// Observes the existing public ticker subscription only. No network, account,
// private state, backfill, notification permission, or delivery belongs here.
class RapidMoveMonitor final : public QObject {
    Q_OBJECT
public:
    using Clock = std::function<qint64()>;
    explicit RapidMoveMonitor(QObject* parent = nullptr, Clock = {});
    ~RapidMoveMonitor() override;
    void setEnabled(bool);
    bool enabled() const;
    void observe(const QList<PublicTick>&);
    void tick();
    RapidMoveSnapshot snapshot(const QString& market) const;
signals:
    void eventReady(const keefi::ChartPublicStudyEvent&);
    void changed();
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
