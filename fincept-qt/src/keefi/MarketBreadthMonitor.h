#pragma once
#include "keefi/ExtendedChartStudies.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/PublicMarketCatalog.h"
#include <functional>
#include <memory>

namespace keefi {
struct MarketBreadthSnapshot {
    ExtendedBreadth breadth;
    QString market, basis, reason, source;
    QStringList populationIds;
    qint64 capturedAtMs = 0, completedAtMs = 0;
    qint64 earliestSourceTimeMs = 0, latestSourceTimeMs = 0;
};
// Source-separated, frozen one-minute public sample. No requests, notifications,
// historical cohort reconstruction, or individual-chart direction vote.
class MarketBreadthMonitor final : public QObject {
    Q_OBJECT
public:
    using Clock = std::function<qint64()>;
    explicit MarketBreadthMonitor(QObject* parent = nullptr, Clock = {});
    ~MarketBreadthMonitor() override;
    void setEnabled(bool);
    bool enabled() const;
    void observeCoins(const QList<PublicMarketRow>&);
    // Current board DTO has no verified common trade date. This keeps a visible
    // missing result, never a date inferred from receivedAtMs or the session label.
    void observeStocks(const QString& market, const QList<PublicBoardItem>&);
    void tick();
    MarketBreadthSnapshot snapshot(const QString& market) const;
signals:
    void changed();
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
