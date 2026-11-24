#pragma once
#include "keefi/MarketCandles.h"
#include <optional>

namespace keefi {
enum class CoinFlowVenue { UpbitSpot, BinanceSpot, BinanceUsdM };
enum class CoinFlowGrain { UpbitTrade, SpotAggregate, Futures100msAggregate };
enum class CoinFlowSide { Buy, Sell }; // Upbit source BID/ASK; Binance actual taker.
enum class CoinFlowCoverage { Unavailable, WarmingUp, ObservedContinuous,
    RecoveredOverlap, Partial, Disconnected, Overflow };
enum class CoinFlowSignalKind { LargeTrade, TakerSkew, OpenInterestJump,
    VolumeBurst, Rebound, TurnDown };
enum class CoinFlowDirection { Neutral, Up, Down };

struct CoinFlowEvidence {
    QString source, sourceUrl;
    qint64 sourceTimeMs=0, receivedAtMs=0, knownAtMs=0;
    // Public provenance only. No account/scope/holding/amount/alias fields.
};
struct CoinFlowInstrument {
    CoinFlowVenue venue=CoinFlowVenue::UpbitSpot;
    CoinFlowGrain grain=CoinFlowGrain::UpbitTrade;
    QString identity, symbol, baseAsset, quoteAsset;
    // Primary uses a C2 public chart identity. USD-M companion uses an internal
    // usdm_futures:binance:<symbol> identity, never a profile/watchlist identity.
    QString marginAsset, contractType, status, underlyingType;
    bool verified=false;
    CoinFlowEvidence evidence;
    // USD-M: exact official TRADING, PERPETUAL, USDT quote+margin, COIN base.
};
struct CoinTradeObservation {
    QString id, price, quantity;
    std::optional<QString> normalQuantity; // Futures nq; never substitutes q.
    CoinFlowSide side=CoinFlowSide::Buy;
    qint64 tradeAtMs=0, eventAtMs=0, receivedAtMs=0, knownAtMs=0;
    quint64 epoch=0;
    bool snapshot=false, liveEligible=false;
    // Historical REST seed may warm denominator but cannot create old alerts.
};
struct CoinFlowCoveragePeriod {
    qint64 fromMs=0, throughMs=0, knownAtMs=0; // [from,through), actual UTC.
    quint64 epoch=0;
    CoinFlowCoverage coverage=CoinFlowCoverage::Unavailable;
    QString reason;
};
struct CoinTradeConflict {
    CoinTradeObservation first, second; // Same provider ID, different content: both raw observations.
};
struct CoinFlowWindow {
    CoinFlowInstrument instrument;
    QList<CoinTradeObservation> trades;
    QList<CoinFlowCoveragePeriod> periods; // Immutable closed coverage slices.
    QList<CoinTradeConflict> conflicts;    // Kept for record-keeping readers; never enters a calculation.
    int duplicateCount=0;
    QString reason;
    CoinFlowEvidence evidence;
};
struct CoinOiPoint {
    QString symbol, quantity, value;
    qint64 periodEndMs=0, knownAtMs=0;
    bool liveEligible=false;
    CoinFlowEvidence evidence; // Quantity/value units/currency not inferred.
};
struct CoinLiquidationObservation {
    QString symbol, side, price, averagePrice;
    QString originalQuantity, lastFilledQuantity, cumulativeFilledQuantity;
    QString orderStatus;
    qint64 tradeAtMs=0, eventAtMs=0, receivedAtMs=0, knownAtMs=0;
    quint64 epoch=0;
    CoinFlowEvidence evidence;
    // Last snapshot per symbol/second; no order ID or position direction guess.
};
struct CoinFlowClosedMinute {
    MarketCandle candle; // Actually observed native completed 1m only.
    qint64 completesAtMs=0, knownAtMs=0, decisionAtMs=0;
    bool connectsPrevious=false, liveEligible=false;
    CoinFlowEvidence evidence;
    // decisionAt is the first closed-minute decision, never recomputed on REST.
};
struct CoinFlowSnapshot {
    QString identity; // Primary C2 chart identity, never the companion identity.
    CoinFlowWindow primary;
    std::optional<CoinFlowWindow> companion;
    QList<CoinFlowClosedMinute> minutes;
    QList<CoinOiPoint> openInterest;
    QList<CoinLiquidationObservation> liquidations;
    QList<CoinFlowCoveragePeriod> liquidationCoverage;
    std::optional<MarketCandleSeries> candleSeries; // Independent canonical C3 native-1m proof.
};

struct CoinFlowAvailability {
    bool available=false;
    QString reason;
    CoinFlowCoverage coverage=CoinFlowCoverage::Unavailable;
    qint64 fromMs=0, throughMs=0, knownAtMs=0;
    int samples=0;
    QList<CoinFlowEvidence> evidence;
};
struct CoinLargeTradePoint {
    QString id, quoteNotional;
    qint64 tradeAtMs=0, knownAtMs=0;
    double percentile=0;
    int baselineSamples=0;
    CoinFlowSide side=CoinFlowSide::Buy;
};
struct CoinLargeTradeStudy {
    CoinFlowAvailability state;
    std::optional<double> latestPercentile;
    int baselineSamples=0;
    QList<CoinLargeTradePoint> candidates;
};
struct CoinTakerStudy {
    CoinFlowAvailability state;
    std::optional<double> signedFraction;
    QString buyQuoteNotional, sellQuoteNotional;
    bool sourceDirectionOnly=false; // Upbit: label BID/ASK, never strict taker.
};
struct CoinOiStudy {
    CoinFlowAvailability state;
    qint64 previousEndMs=0, currentEndMs=0;
    QString previousQuantity, currentQuantity, previousValue, currentValue;
    std::optional<double> quantityChangePercent, valueChangePercent;
};
struct CoinVolumeStudy {
    CoinFlowAvailability state;
    QString currentVolume, previousMedian;
    std::optional<double> multiple;
};
struct CoinReversalStudy {
    CoinFlowAvailability state;
    bool rebound=false, turnDown=false;
};
struct CoinCvdPoint {
    qint64 atMs=0, knownAtMs=0;
    quint64 epoch=0;
    QString signedBaseQuantity;
};
struct CoinCvdStudy {
    CoinFlowAvailability state;
    QList<CoinCvdPoint> points;
    bool sourceDirectionOnly=false;
    // Each epoch is a separate baseline. Never connect across gap/overflow.
};
struct CoinSpotFuturesStudy {
    CoinFlowAvailability state;
    CoinFlowGrain spotGrain=CoinFlowGrain::SpotAggregate;
    CoinFlowGrain futuresGrain=CoinFlowGrain::Futures100msAggregate;
    int spotSamples=0, futuresSamples=0;
    std::optional<double> spotSignedFraction, futuresSignedFraction;
    QString spotSignedBaseQuantity, futuresSignedBaseQuantity;
    bool directionsAgree=false;
    // Common fully observed window only. No shared percentile pool/FX guess.
};
struct CoinLiquidationStudy {
    CoinFlowAvailability state;
    bool sampledLastPerSecond=true;
    QList<CoinLiquidationObservation> observations;
    // Count is observed snapshot count, never reconstructed orders/24h total.
};
struct CoinReversalCorroboration {
    CoinVolumeStudy volume;
    CoinTakerStudy spot; // Exactly the five minutes ending at this reversal.
};
struct CoinFlowSignal {
    CoinFlowSignalKind kind=CoinFlowSignalKind::LargeTrade;
    CoinFlowDirection direction=CoinFlowDirection::Neutral;
    qint64 atMs=0, knownAtMs=0; // atMs = actual 1m completion boundary.
    QString entryClose;
    CoinFlowAvailability state;
    std::optional<double> value;
    // Frozen using this entry's original decisionAt (knownAtMs). Never join a
    // historical reversal to today's volume or a later five-minute trade window.
    std::optional<CoinReversalCorroboration> reversalCorroboration;
};
struct CoinFlowSignalGroup {
    CoinFlowSignalKind kind=CoinFlowSignalKind::LargeTrade;
    CoinFlowDirection direction=CoinFlowDirection::Neutral;
    qint64 fromMs=0, throughMs=0, knownAtMs=0;
    QString entryClose;
    int minutes=0;
    bool ended=false; // Only after an observed no-signal/opposite/gap decision.
};
struct CoinFlowOutcome {
    CoinFlowSignalKind kind=CoinFlowSignalKind::LargeTrade;
    qint64 signalAtMs=0, evaluationAtMs=0, knownAtMs=0;
    double returnPercent=0;
};
struct CoinFlowScorecard {
    bool available=false, costsExcluded=true;
    QString reason;
    qint64 horizonMs=60*60000;
    int samples=0, unresolvedSignals=0, overlappingSignals=0;
    int up=0, down=0, flat=0;
    std::optional<double> meanReturnPercent;
    QList<CoinFlowOutcome> outcomes;
};
struct CoinFlowStudySet {
    bool available=false;
    QString reason, identity;
    qint64 asOfMs=0, lastCompletedAtMs=0;
    CoinLargeTradeStudy largeTrade;
    CoinTakerStudy taker;
    CoinOiStudy openInterest;
    CoinVolumeStudy volume;
    CoinReversalStudy reversal;
    CoinCvdStudy cvd;
    CoinSpotFuturesStudy comparison;
    CoinLiquidationStudy liquidation;
    QList<CoinFlowSignal> entries;
    QList<CoinFlowSignalGroup> groups;
    CoinFlowScorecard scorecard;
};

// Presentation state for the core public-trade indicator. A valid, empty
// decision is different from an incomplete observation; no missing trade is 0.
enum class CoinFlowDisplayPhase { Collecting, ObservedNoSignal, Signal };
struct CoinFlowDisplayState {
    CoinFlowDisplayPhase phase=CoinFlowDisplayPhase::Collecting;
    bool requiredReady=false;
    int currentSignals=0;
    qint64 latestSignalAtMs=0;
    QString reason;
};
CoinFlowDisplayState coinFlowDisplayState(const CoinFlowStudySet&);
// Locate the actual candle containing a completed 1m signal; gaps stay empty.
std::optional<qint64> coinFlowMarkerCandleAt(qint64 completedAtMs,const QList<MarketCandle>&,qint64 intervalMs);

// Pure public display calculator. Explicit asOf; no clock/network/settings or
// private/controller/position DTO. Invalid parts remain independent unavailable.
CoinFlowStudySet coinFlowStudies(const CoinFlowSnapshot&, qint64 asOfMs);
// Opt-in evidence for N15. The ordinary calculator does not collect the extra
// five-minute joins while the strong-reversal indicator is off.
CoinFlowStudySet coinFlowStudiesWithReversalEvidence(const CoinFlowSnapshot&, qint64 asOfMs);
} // namespace keefi
