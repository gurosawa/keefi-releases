#pragma once
#include "keefi/MarketFlowPresentation.h"
#include <QObject>
#include <functional>
#include <memory>
namespace keefi {
class TradingTransport;
// Public display history only. The borrowed transport is dedicated to this service.
class MarketFlowHistory final : public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    explicit MarketFlowHistory(TradingTransport*,QObject* parent=nullptr);
    MarketFlowHistory(TradingTransport*,Clock,QObject* parent=nullptr);
    ~MarketFlowHistory() override;
    void setActive(bool);
    void setAsset(const QString&);
    void setInstruments(const QList<MarketInstrument>&);
    MarketFlowHistoricalGap snapshot(qint64 asOfMs=0) const;
    void refresh();
    void tick();
  signals:
    void changed();
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
