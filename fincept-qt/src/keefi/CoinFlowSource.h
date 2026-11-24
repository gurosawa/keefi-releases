#pragma once
#include "keefi/CoinFlowProtocol.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
// One selected public Spot population and its exact same-pair USD-M companion.
// Constructor is lazy; only main injects real fixed-endpoint socket Adapters.
class CoinFlowSource final : public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    explicit CoinFlowSource(TradingTransport*,QObject* parent=nullptr);
    CoinFlowSource(TradingTransport*,std::unique_ptr<PublicCandleSocket> primary,
                   std::unique_ptr<PublicCandleSocket> companion,QObject* parent=nullptr,Clock={});
    ~CoinFlowSource() override;
    void setSelection(const QString& fullIdentity);
    void setActive(bool);
    bool active() const;
    void setPublicCatalog(CoinFlowVenue,const QList<CoinFlowInstrument>&); // Empty/expired revokes evidence.
    void setMinuteSeries(const MarketCandleSeries&); // Actual completed native1m, original receipt retained.
    CoinFlowSnapshot snapshot(qint64 asOfMs=0) const;
    QString status() const;
    void pump();
    void stop();
    static RequestSpec request(CoinFlowRead,CoinFlowVenue,const QString& symbol={},const CoinFlowPageCursor& cursor={});
    static bool isRequest(const RequestSpec&);
  signals:
    void changed();
  private:
    struct Private;std::unique_ptr<Private> d_;
};
} // namespace keefi
