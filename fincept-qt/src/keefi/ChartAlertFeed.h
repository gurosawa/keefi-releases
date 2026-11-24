#pragma once
#include "keefi/ChartAlertEngine.h"
#include "keefi/StockChartTypes.h"
#include "keefi/CoinFlowStudies.h"
#include <QPointer>
#include <memory>
namespace keefi {
class TradingTransport;
class ChartInstrumentCatalog;
class StockChartSource;
class CoinFlowSource;
struct StockChartSnapshot;
// Dedicated, serial public completed-candle polling. No chart widget, private
// account controller, credential or valuation consumer is owned here.
class ChartAlertFeed final:public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    explicit ChartAlertFeed(TradingTransport*,ChartInstrumentCatalog* catalog=nullptr,
                            QObject* parent=nullptr,Clock={});
    ~ChartAlertFeed() override;
    void setDemands(const QList<ChartAlertDemand>&);
    void setVerifiedUpbitMarkets(const QStringList&, bool available);
    void setPriorityIdentities(const QStringList&);
    void setStockSource(StockChartSource*); // Dedicated serial display lane; existing authorization only.
    void setCoinFlowSource(CoinFlowSource*); // Borrowed single collector; selection belongs to the window arbiter.
    void setRunning(bool);
    bool running() const;
    QString status() const;
    QStringList scheduledIdentities() const;
    void pump();
  signals:
    void changed();
    void laneStarted(QString identity,qint64 intervalMs,quint64 generation,bool realtimeAllowed);
    void laneInvalidated(QString identity,qint64 intervalMs,quint64 generation);
    void observationReady(keefi::MarketCandleSeries,keefi::ChartStudySet,quint64 generation);
    void stockObservationReady(keefi::StockChartSnapshot,quint64 generation);
    void coinFlowObservationReady(keefi::CoinFlowSnapshot,quint64 generation);
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
