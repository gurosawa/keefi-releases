#pragma once
#include "keefi/FxHistory.h"
#include "keefi/MarketCompare.h"
#include <QObject>
#include <QSet>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;

enum class CompareWonMode { Dollar, WonRaw, WonToDollar };

struct MarketCompareSnapshot {
    qint64 intervalMs = 0, updatedAtMs = 0;
    QList<CompareSeries> series; // Allowed specs in display order; bitcoin last.
    QString catalogReason, fxReason, collectorReason;
    qint64 collectorWrittenAtMs = 0; // Last status.json write by the collector; 0 when none.
    qint64 hanaConfirmedThroughMs = 0; // End of the latest verified, sealed Hana hour.
    QSet<qint64> provisionalFxOpenMs; // Intraday converted bars beyond the sealed horizon.
    bool loading = false;
    CompareWonMode wonMode = CompareWonMode::Dollar;
    QList<FxRate> ecb, hana; // Ascending by knownAtMs.
};

// Public, keyless reads for the 시장 비교 tab: one request in flight, paced.
// Display only: nothing here feeds valuation, alerts or account state.
class MarketCompareSource final : public QObject {
    Q_OBJECT
  public:
    using Clock = std::function<qint64()>;
    explicit MarketCompareSource(TradingTransport*, QObject* parent = nullptr, Clock clock = {});
    ~MarketCompareSource() override;
    void setActive(bool);
    bool active() const;
    void setInterval(qint64 intervalMs);
    qint64 intervalMs() const;
    void setWonMode(CompareWonMode);
    void setCollectorRoot(const QString&); // Read-only: Hana postings and ECB days the collector sealed.
    MarketCompareSnapshot snapshot() const;
    void refresh();
    static qint64 refreshPeriodMs(qint64 intervalMs);

  signals:
    void changed();

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
