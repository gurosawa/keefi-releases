#pragma once
#include "keefi/ExtendedChartStudies.h"
#include "keefi/TradingTypes.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
class ChartStudyRequestPacer;
struct ChartPublicCell {
    bool available=false,hidden=false;
    QString value,unit,reason,sourceUrl;
    qint64 fromMs=0,throughMs=0,receivedAtMs=0;
};
struct ChartLiquidationRow {
    QString eventId,quoteNotional;
    CoinLiquidationObservation observation;
};
struct ChartLiquidationFeed {
    bool supported=false,connected=false;
    QString reason,minimumQuote="100000",unit="USDT";
    int observedCount=0,coverageMinutes=0;
    qint64 fromMs=0,throughMs=0;
    QList<ChartLiquidationRow> rows; // Filtered individual snapshots. Never a total.
};
struct ChartPublicContext {
    QString identity,levelsReason;
    QList<ExtendedLevelCandidate> levelCandidates;
    bool futuresChecked=false,futuresSupported=false;
    QString futuresSymbol,futuresReason;
    ChartPublicCell funding,oiChange,priceChange,topPosition,takerBuy;
    QList<ChartPricePoint> openInterest; // Observed completed 1h END, verified base quantity.
    QString openInterestUnit;
    ChartLiquidationFeed liquidation;
};
// Selected public context only. Lazy, unsigned, fixed requests; no account path.
class ChartPublicContextSource final:public QObject {
    Q_OBJECT
  public:
    enum class Read { Day,Week,Month,Contracts,Funding,OpenInterest,TopPosition,Taker,FuturesPrice };
    using Clock=std::function<qint64()>;
    explicit ChartPublicContextSource(TradingTransport*,QObject* parent=nullptr,Clock={});
    ChartPublicContextSource(TradingTransport*,std::unique_ptr<PublicCandleSocket>,QObject* parent=nullptr,Clock={});
    ~ChartPublicContextSource() override;
    void setSelection(const QString& identity,const QString& baseAsset={},const QString& quoteAsset={});
    void setDemand(bool levelsEnabled,bool futuresEnabled);
    void setLiquidationMinimum(const QString& quoteNotional); // Exact USDT, positive; default 100000.
    void setRequestPacer(std::shared_ptr<ChartStudyRequestPacer>);
    ChartPublicContext snapshot(qint64 asOfMs=0) const;
    void pump();
    static RequestSpec request(Read,const QString& identity,const QString& futuresSymbol={});
    static bool isRequest(const RequestSpec&);
    // Public, deterministic parser seam; completed periods only, no networking.
    static QList<ExtendedLevelCandidate> parseLevels(Read,const QString& identity,const HttpResult&,qint64 asOfMs,qint64 receivedAtMs,QString* reason=nullptr);
  signals:
    void changed();
    void liquidationObserved(const QString& identity,const QString& eventId,const QString& detail,qint64 atMs,qint64 receivedAtMs);
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
