#pragma once
#include "keefi/TradingTypes.h"
#include <QDate>
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
enum class MarketTimeMode { Automatic, Morning, Korea, Night };
struct DailyTreasuryYield {
    bool available=false, noObservations=false;
    QString percent, source, sourceUrl, reason;
    QDate observationDate; // Official NEW_DATE, a US observation date, not an invented instant.
    qint64 receivedAtMs=0;
};
// Public display-only context. No account, credential, ledger or valuation input.
class MarketOverview final : public QObject {
    Q_OBJECT
  public:
    using Clock = std::function<qint64()>;
    static constexpr qint64 SourceRecheckMs=60*60000;
    static constexpr int MaximumObservationAgeDays=7;
    explicit MarketOverview(TradingTransport*,QObject* parent=nullptr);
    MarketOverview(TradingTransport*,Clock,QObject* parent=nullptr);
    ~MarketOverview() override;
    void setEnabled(bool);
    void refresh();
    bool busy() const;
    DailyTreasuryYield treasury10Year(qint64 asOfMs=0) const;
    static MarketTimeMode effectiveMode(MarketTimeMode,qint64 asOfMs);
    static QString modeLabel(MarketTimeMode);
    static QString modeContext(MarketTimeMode,qint64 asOfMs);
    static RequestSpec request(const QDate& month);
    static bool isRequest(const RequestSpec&);
    static DailyTreasuryYield parse(const HttpResult&,const QDate& requestedMonth,qint64 receivedAtMs);
  signals:
    void changed();
  private:
    struct Private;std::unique_ptr<Private> d_;
};
} // namespace keefi
