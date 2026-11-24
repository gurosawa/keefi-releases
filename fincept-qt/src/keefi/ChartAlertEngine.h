#pragma once
#include "keefi/ChartStudies.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QByteArray>
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class SystemNotificationSink;
class ChartPhrasePack;
struct StockChartSnapshot;
struct CoinFlowSnapshot;
struct ExtendedStrongReversal;
struct CoinFlowStudySet;
struct ChartHorizontalLineRule;
struct ContractFlow;
enum class MarketFlowMetric;
enum class MarketFlowState;
enum class ChartAlertKind { PullbackEntry, StudyResult, OfficialNotice, LiquidationDistance, Whale, HorizontalLine, MarketMetric, PublicStudy };
struct ChartPublicStudyEvent {
    WatchlistIdentity identity;
    QString kind,eventId,detail;
    qint64 atMs=0,receivedAtMs=0;
    MovementDirection direction=MovementDirection::Unknown;
    bool liveEligible=false;
};
struct ChartAlertList { QString listId;QList<WatchlistIdentity> identities; };
struct ChartAlertRule {
    QString listId;
    bool enabled=false,pullback=true,results=true,officialNotices=false,liquidation=false;
    qint64 intervalMs=900000,cooldownMs=15*60000;
    int minimumScore=5;
    double liquidationDistancePercent=5;
    bool whales=false; // Explicit opt-in; completed native 1m, independent of the study timeframe.
    bool horizontal=false; // Public list opt-in; each drawing owns its exact alert interval.
};
struct ChartAlertSettings {
    bool quietEnabled=false,dripEnabled=true;
    int quietStartMinute=22*60,quietEndMinute=7*60,dailyLimit=20;
    QByteArray timeZoneId;
    qint64 mutedUntilMs=0;
};
struct ChartAlertDemand {
    WatchlistIdentity identity;
    qint64 intervalMs=900000;
    bool pullback=false,results=false,privateOnly=false;
    bool whales=false;
    bool horizontal=false;
};
struct ChartLiquidationObservation {
    WatchlistIdentity identity;
    QString positionToken,markPrice,liquidationPrice,currency,sourceUrl;
    qint64 receivedAtMs=0;
    bool verified=false;
    QString positionSide; // Provider LONG/SHORT/BOTH only; never inferred.
};
// Local display notifications only. No accounts, authenticated reads, network,
// settings reads, valuation, or forming-candle inputs are owned by this engine.
class ChartAlertEngine final:public QObject {
    Q_OBJECT
  public:
    using Clock=std::function<qint64()>;
    explicit ChartAlertEngine(SystemNotificationSink*,QObject* parent=nullptr,Clock={});
    ~ChartAlertEngine() override;
    void setPhrasePack(ChartPhrasePack*); // Borrowed, destruction-safe.
    bool setPublicLists(const QList<ChartAlertList>&,QString* error=nullptr);
    bool setRule(const ChartAlertRule&,QString* error=nullptr);
    bool setHorizontalLines(const QList<ChartHorizontalLineRule>&,QString* error=nullptr);
    ChartAlertRule rule(const QString& listId) const;
    bool setSettings(const ChartAlertSettings&,QString* error=nullptr);
    ChartAlertSettings settings() const;
    void muteForOneHour();
    void muteForToday();
    void setDripEnabled(bool);
    void setPrivateEnabled(bool);
    quint64 privateEpoch() const;
    void setHoldings(const QList<WatchlistIdentity>&,qint64 receivedAtMs,quint64 epoch);
    void clearPrivate();
    QList<ChartAlertDemand> demands() const;
    void beginLane(const WatchlistIdentity&,qint64 intervalMs,quint64 generation,bool realtimeAllowed);
    void observe(const MarketCandleSeries&,const ChartStudySet&,quint64 generation);
    void observeStock(const StockChartSnapshot&,quint64 generation);
    void observeCoinFlow(const CoinFlowSnapshot&,quint64 generation);
    void observeCoinFlowCalculated(const CoinFlowSnapshot&,const CoinFlowStudySet&,quint64 generation);
    void observeStrongReversalCalculated(const CoinFlowSnapshot&,const CoinFlowStudySet&,const ExtendedStrongReversal&,quint64 laneGeneration=0);
    void invalidateLane(const WatchlistIdentity&,qint64 intervalMs,quint64 generation);
    void observeLiquidation(const ChartLiquidationObservation&,quint64 epoch);
    void setLiquidationObservations(const QList<ChartLiquidationObservation>&,quint64 epoch);
    void tick();
    QByteArray publicState() const;
    bool restorePublicState(const QByteArray&,QString* error=nullptr);
    bool setMarketMetricEnabled(MarketFlowMetric,bool enabled);
    bool marketMetricEnabled(MarketFlowMetric) const;
    void observeMarketMetric(const QString& asset,MarketFlowMetric,MarketFlowState,qint64 sourceTimeMs,qint64 receivedAtMs);
    void observeContractFlow(const QString& asset,const ContractFlow&,qint64 receivedAtMs);
    QByteArray marketMetricState() const;
    bool restoreMarketMetricState(const QByteArray&);
    bool setObservationEnabled(const QString& kind,bool);
    bool observationEnabled(const QString& kind) const;
    QByteArray observationSettingsState() const;
    bool restoreObservationSettingsState(const QByteArray&);
    void observePublicStudy(const ChartPublicStudyEvent&);
    static QStringList observationKinds();
    static QString observationLabel(const QString&);
  signals:
    void changed();
    void demandsChanged();
    void dripDisabled();
    void openWindowRequested();
    void noticeSubmitted(QString identifier,QString title,QString body,bool privateData,
        keefi::MovementDirection direction=keefi::MovementDirection::Unknown,bool fixedRisk=false);
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
