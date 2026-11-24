#pragma once

#include "keefi/TradingTypes.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;

// Public chart identity metadata only. This module never owns an account
// controller, credentials, balances, or the borrowed dedicated transport.
class ChartInstrumentCatalog final : public QObject {
    Q_OBJECT
  public:
    static constexpr qint64 FreshnessMs = 300000;
    using Clock = std::function<qint64()>; // Monotonic milliseconds for request pacing.
    explicit ChartInstrumentCatalog(TradingTransport*, QObject* parent = nullptr, Clock clock = {});
    ~ChartInstrumentCatalog() override;
    void ensure(const QString& provider);
    QList<MarketInstrument> instruments(const QString& provider, qint64 asOfMs = 0) const;
    QString status(const QString& provider) const;
    QString failureReason(const QString& provider) const;
    qint64 retryAfterMs(const QString& provider) const;
    bool canRetry(const QString& provider) const;
    bool busy() const;
    void stop();
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
