#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/ChartStudies.h"
#include "keefi/MarketStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include <QDate>
#include <QMap>
#include <optional>

namespace keefi {
enum class StockChartConnectionState;
enum class StockSourceKind { None, Toss, KiwoomDaily }; // KiwoomDaily: KR 1일 bars only, no intraday or realtime.
enum class StockAdjustment { Unknown, SourceAdjusted, SourceUnadjusted, CloseSplitAdjustedOnly };
enum class StockCalendarState { Unknown, ExplicitHoliday, Sessions };
enum class StockSessionKind { Unknown, Day, Pre, Regular, After, Mixed };
enum class StockHistoryCoverage { Unknown, CurrentOnly, VerifiedIntervals };
enum class StockBarExclusion {
    CalendarUnknown, SessionUnknown, Incomplete, MutableClosing, MixedSession,
    MissingChild, VolatilityInterruption, PriceLimit, RestrictionCoverageUnknown, AdjustmentUnknown, InvalidPriceLimit
};
struct StockChartInstrument {
    QString identity, symbol, marketCountry, currency, market, marketTimeZone;
    std::optional<bool> nxtSupported;
    bool verified = false; // Supplied by the existing public metadata registry; no account identity.
};
struct StockSession {
    StockSessionKind kind = StockSessionKind::Unknown;
    qint64 fromMs = 0, toMs = 0;
    std::optional<qint64> auctionFromMs, auctionToMs;
};
struct StockSessionDay {
    QDate tradingDate;
    StockCalendarState state = StockCalendarState::Unknown;
    QList<StockSession> sessions;
};
struct StockCalendar {
    bool available = false;
    QString marketCountry, reason;
    QMap<QDate, StockSessionDay> days;
    QMap<QDate, QDate> nextBusinessDay; // Official previous/today/next links only.
    AssetEvidence evidence;
};
struct StockRestrictionRange {
    qint64 fromMs = 0, toMs = 0;
    bool vi = false;
    std::optional<QString> upperLimit, lowerLimit;
    AssetEvidence evidence;
};
struct StockRestrictions {
    std::optional<bool> currentVI;
    std::optional<QString> upperLimit, lowerLimit;
    qint64 currentObservationAtMs = 0; // Observation clock, never claimed as a provider update.
    AssetEvidence evidence;
    StockHistoryCoverage historicalCoverage = StockHistoryCoverage::Unknown;
    QList<StockRestrictionRange> intervals; // No historical intervals may be invented from current warnings.
};
struct ChartStudyTimelineRow {
    qint64 startsAtMs = 0, completesAtMs = 0;
    QDate tradingDate;
    StockSessionKind session = StockSessionKind::Unknown;
    bool connectsPrevious = false, eligible = false;
    std::optional<int> missingSlotsBefore; // Unknown is not zero. Display only; no invented candles.
    QList<StockBarExclusion> exclusions;
    bool closeDisplayEligible() const {
        return eligible || (exclusions.size()==1 && exclusions.first()==StockBarExclusion::RestrictionCoverageUnknown);
    }
    QString reason;
};
struct ChartStudyTimeline {
    QList<ChartStudyTimelineRow> rows; // Exactly one per series.candles row, with identical startsAtMs.
    QList<qint64> nextStartsAtMs; // Exactly 20 actual calendar coordinates, or empty with a reason.
    bool projectionCovered = false, adjustedComparable = false;
    QString projectionReason, comparisonReason;
    QString recentExclusionReason(int lookback,bool closeDisplay=false) const {
        for(qsizetype i=rows.size();i>0&&lookback>0;--i,--lookback){
            const auto& row=rows[i-1];
            if(closeDisplay?!row.closeDisplayEligible():!row.eligible)return row.reason;
            if(!row.connectsPrevious)break;
        }
        return {};
    }
    ChartStudyTimeline forCloseDisplay() const {
        auto display=*this;
        for(auto& row:display.rows)row.eligible=row.closeDisplayEligible();
        return display;
    }
};
struct StockObservedCandle {
    MarketCandle candle;
    QDate tradingDate;
    qint64 sourceLabelAtMs = 0; // Stock 1m END label; never a source-update timestamp.
};
struct StockPendingCandle {
    StockObservedCandle observation;
    qint64 completesAtMs = 0; // Zero means the completion boundary is not established.
    StockSessionKind session = StockSessionKind::Unknown;
    QList<StockBarExclusion> exclusions;
    QString reason;
};
struct StockCandlePage {
    bool available = false;
    QString reason, nextBefore, sourceUrl;
    StockChartInstrument instrument;
    StockSourceKind source = StockSourceKind::None;
    StockAdjustment adjustment = StockAdjustment::Unknown;
    qint64 nativeIntervalMs = 0, requestStartedAtMs = 0, receivedAtMs = 0;
    QList<StockObservedCandle> candles; // Ascending, observed native rows; not yet all proven complete.
};
struct StockChartOptions {
    qint64 intervalMs = 15 * 60000;
    bool includeExtended = false, realtimeAllowed = false;
};
struct StockFlowDay {
    QDate date;
    std::optional<QString> foreignerNetBuyVolume, institutionNetBuyVolume;
    std::optional<QString> arbitrageNetBuyVolume, nonArbitrageNetBuyVolume;
    AssetEvidence investorEvidence, programEvidence;
    bool provisional = false;
};
struct StockMarketStudies {
    QList<StockFlowDay> days;
    std::optional<int> foreignerConsecutiveBuyDays, institutionConsecutiveBuyDays;
    QString flowContinuityReason;
    std::optional<double> volumeMultiple;
    AssetEvidence volumeEvidence;
    int volumeSamples = 0;
    QString volumeScope, reason;
};
struct StockChartSnapshot {
    QList<ExtendedLevelCandidate> levelCandidates;
    QString levelsReason;
    MarketCandleSeries series; // Only proven completed rows here; original UTC coordinates retained.
    ChartStudyTimeline timeline;
    QList<StockPendingCandle> pending; // Native observations/reasons only; never target-timeframe bodies or analysis input.
    ChartStudySet studies;
    CandleStudySet marketContextStudies;
    CandleCompression compression;
    StockSourceKind source = StockSourceKind::None;
    StockAdjustment adjustment = StockAdjustment::Unknown;
    StockRestrictions restrictions;
    StockMarketStudies marketStudies;
    QString livePrice; // Latest LOSSY quote reference only; never aggregate into OHLCV.
    AssetEvidence liveEvidence;
    bool delayed = false, realtimeAllowed = false, intradayAllowed = false, calendarAvailable = false;
    bool loading = false; // Partial pages may render, but a polling lane waits for the complete bounded read.
    QString adjustmentNote, sessionNote, calendarReason;
    std::optional<StockChartConnectionState> connectionIssue;
    bool kiwoomDailyAlternative = false;
};
} // namespace keefi
