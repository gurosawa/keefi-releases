#pragma once
#include "keefi/TradingTypes.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QObject>
#include <QByteArrayView>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
class BrokerTradingController;
class TossMarketStream;
class KiwoomMarketStream;
struct StockChartSnapshot;
enum class StockChartConnectionState { VaultClosed, NoBroker, OtherBroker, AuthenticationRequired, KiwoomAuthenticationRequired, Ready };
// Temporary user-facing copy lives in this one function. No account identifiers.
QString stockChartConnectionMessage(StockChartConnectionState);
enum class StockChartRead { Metadata,Calendar,Candles,PriceLimits,Warnings,Investors,Program };
// Dedicated display GET lane. Borrows an existing market-data authorization;
// never creates/authenticates/selects/refreshes an account controller.
class StockChartSource final:public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    using AuthorizationBorrow=std::function<void(QByteArrayView,qint64 remainingMs,quint64 revision)>;
    using AuthorizationProvider=std::function<bool(const AuthorizationBorrow&)>;
    explicit StockChartSource(TradingTransport*,QObject* parent=nullptr,Clock={});
    ~StockChartSource() override;
    quint64 observedAuthorizationRevision() const; // Last existing borrow; no additional authorization read.
    bool authenticationRequired(quint64 newestObservedRevision=0) const; // Existing in-memory failure/expiry only.
    void setConnectionState(StockChartConnectionState); // Display context already in memory; never starts a request.
    void setBroker(BrokerTradingController*); // Existing borrowed controller only.
    void setAuthorizationProvider(AuthorizationProvider); // Same borrow seam for alternate session owners.
    void setRealtimeStream(TossMarketStream*); // Borrow existing stream; no second authenticated socket.
    // Borrowed Kiwoom display stream: KR 1일 bars via ka10081 when the connected broker is Kiwoom.
    void setKiwoomDailyStream(KiwoomMarketStream*);
    void setPreferKiwoomDaily(bool); // Applies to the domestic daily chart only.
    void setSelection(const WatchlistIdentity&,qint64 intervalMs,bool includeExtended=false);
    void setLevelDemand(bool enabled); // Completed-period support; OFF makes no added reads.
    void setActive(bool);
    bool active() const;
    void refresh();
    void stop();
    StockChartSnapshot snapshot(qint64 asOfMs=0) const;
    static RequestSpec request(StockChartRead,const WatchlistIdentity&,qint64 nativeIntervalMs=60000,
                               const QString& cursorOrDate={});
    static bool isRequest(const RequestSpec&);
  signals:
    void changed();
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
