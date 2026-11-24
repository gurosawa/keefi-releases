#pragma once
#include "keefi/MarketCandles.h"
#include "keefi/StockChartTypes.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QObject>
#include <memory>
namespace keefi {
class TradingTransport;
class ChartInstrumentCatalog;
class StockChartSource;
class ChartStudyRequestPacer;
struct ChartStudyRequest {
    WatchlistIdentity identity;
    qint64 intervalMs=3600000;
    int priority=10;
};
struct ChartStudyObservation {
    MarketCandleSeries series;
    ChartStudyTimeline timeline;
    QString status;
    bool stock=false,loading=false,delayed=false;
};
// Bounded, reference-counted public display requests. Owners disappear when
// their panel is hidden; removing one owner cannot cancel another's demand.
class ChartStudyFeed final:public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    explicit ChartStudyFeed(TradingTransport*,ChartInstrumentCatalog* =nullptr,QObject* =nullptr,Clock={});
    ~ChartStudyFeed() override;
    // Enable the same verified market boundary as the selected candle model.
    void setVerifiedUpbitMarkets(const QStringList& markets, bool available);
    void setDemands(const QString& owner,const QList<ChartStudyRequest>&);
    void clearOwner(const QString& owner);
    void setStockSource(StockChartSource*);
    void setRequestPacer(std::shared_ptr<ChartStudyRequestPacer>);
    void clearPrivate();
    void stop();
    void pump();
    ChartStudyObservation observation(const QString& identity,qint64 intervalMs) const;
    int demandCount() const;
    int ownerCount(const QString& identity,qint64 intervalMs) const;
  signals:
    void changed(QString identity,qint64 intervalMs);
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
