#pragma once
#include "keefi/CoinFlowStudies.h"
#include "keefi/TradingTypes.h"

namespace keefi {
enum class CoinFlowRead { Catalog, Trades, OpenInterest };
struct CoinFlowPageCursor {
    QString tradeId; // Exact provider ID, inclusive Binance / exclusive Upbit.
    qint64 fromMs=0, throughMs=0;
    int daysAgo=-1; // Upbit UTC today omitted; explicit historical days1..7 only.
};
struct CoinFlowCatalogResult {
    bool available=false;
    QString reason;
    QList<CoinFlowInstrument> instruments;
};
struct CoinTradeBatch {
    bool available=false;
    QString reason,nextCursor;
    QList<CoinTradeObservation> trades;
};
struct CoinOiBatch {
    bool available=false;
    QString reason;
    QList<CoinOiPoint> points;
};
struct CoinLiquidationBatch {
    bool available=false;
    QString reason;
    QList<CoinLiquidationObservation> observations;
};
// Pure codecs for dedicated unsigned market reads. No controller/session or
// account DTO participates; source clocks and exact numeric lexemes survive.
class CoinFlowProtocol final {
  public:
    static RequestSpec request(CoinFlowRead,CoinFlowVenue,const QString& symbol={},const CoinFlowPageCursor& cursor={});
    static bool isRequest(const RequestSpec&);
    static QUrl streamEndpoint(const CoinFlowInstrument&);
    static CoinFlowCatalogResult catalog(CoinFlowVenue,const HttpResult&,qint64 receivedAtMs);
    static CoinTradeBatch trades(const CoinFlowInstrument&,const HttpResult&,qint64 receivedAtMs,quint64 epoch);
    static CoinTradeBatch tradeFrame(const CoinFlowInstrument&,const QByteArray&,qint64 receivedAtMs,quint64 epoch);
    static CoinOiBatch openInterest(const CoinFlowInstrument&,const HttpResult&,qint64 receivedAtMs);
    static CoinLiquidationBatch liquidationFrame(const CoinFlowInstrument&,const QByteArray&,qint64 receivedAtMs,quint64 epoch);
};
} // namespace keefi
