#pragma once
#include "keefi/FxHistory.h"
#include "keefi/TradingTypes.h"
#include <QHash>
#include <QList>
#include <QString>
#include <optional>
#include <utility>

namespace keefi {
// Display-only cross-market comparison (시장 비교 탭). Floating point values here
// never enter valuation, alerts or account state.
enum class CompareVenue { BinanceSpot, BinanceUsdM, UpbitKrw };

struct CompareSpec {
    QString identity;       // coin:binance:BTCUSDT, perp:binance-usdm:XAUUSDT, coin:upbit:KRW-BTC
    QString name;           // Short Korean name shown in the legend.
    QString symbol;         // Exchange symbol: BTCUSDT, XAUUSDT, KRW-BTC.
    CompareVenue venue = CompareVenue::BinanceSpot;
    QString underlyingType; // Perpetuals only: COMMODITY, EQUITY, KR_EQUITY.
    QString currency;       // USDT or KRW.
    bool operator==(const CompareSpec&) const = default;
};

struct CompareBar {
    qint64 openMs = 0;
    qint64 closeMs = 0; // Last millisecond of the bar: open + interval - 1.
    QString closeText;
    double close = 0;
    bool operator==(const CompareBar&) const = default;
};

struct CompareSeries {
    CompareSpec spec;
    qint64 intervalMs = 0, receivedAtMs = 0;
    qint64 lastOkAtMs = 0;  // Receipt of the last page that parsed; kept when a refresh fails.
    QList<CompareBar> bars; // Completed bars only, ascending and unique.
    QList<FxChoice> fx;     // Converted series only: the posting each bar used, aligned with bars.
    bool available = false;
    QString reason;         // Why nothing is shown, or why the latest refresh failed.
};

struct ComparePercentPoint {
    qint64 openMs = 0;
    double percent = 0;
};

struct CorrelationPoint {
    qint64 openMs = 0;
    std::optional<double> r20, r60;
    int pairs20 = 0, pairs60 = 0;
};

struct RelationChange {
    qint64 openMs = 0;
    double r20 = 0, r60 = 0;
};

class MarketCompare final {
  public:
    static constexpr int DefaultLimit = 200;
    static constexpr double RelationMinimumAbs = 0.3;
    static constexpr int RelationPersistBars = 3; // The condition holds on the mark bar and the two before it.
    static constexpr int RelationClearBars = 3;   // ...and did not hold on the three bars before those.
    static constexpr double TogetherThreshold = 0.5;

    static QList<CompareSpec> defaultSpecs();  // 금, WTI 원유, 나스닥 100, 비트코인
    static QList<CompareSpec> optionalSpecs(); // Further allow-listed perpetuals.
    static CompareSpec upbitBitcoin();         // KRW-BTC for the won view.
    static bool supportedInterval(qint64 intervalMs);
    static QString wireInterval(qint64 intervalMs);

    static RequestSpec klinesRequest(const CompareSpec&, qint64 intervalMs, int limit = DefaultLimit);
    static RequestSpec perpCatalogRequest();
    static bool isRequest(const RequestSpec&);

    // Completed bars only (close before the receipt). Rejects the whole page on
    // any malformed tuple, misaligned bar or out-of-order row.
    static CompareSeries parseKlines(const CompareSpec&, qint64 intervalMs, const HttpResult&, qint64 receivedAtMs);
    // symbol -> underlyingType for TRADING USD-M perpetuals (PERPETUAL or TRADIFI_PERPETUAL).
    static QHash<QString, QString> parsePerpCatalog(const HttpResult&, QString* reason = nullptr);
    // A perpetual is accepted only when the live catalog lists it with the same
    // classification as its allow-list entry. Spot specs need no catalog.
    static bool perpAllowed(const CompareSpec&, const QHash<QString, QString>& catalog, QString* reason = nullptr);

    static QList<ComparePercentPoint> percentFromBase(const CompareSeries&, qint64 baseOpenMs);
    // Pairs only bars with identical open and close times in both series and
    // consecutive bars on both sides; a gap restarts the run.
    static QList<CorrelationPoint> rollingCorrelation(const CompareSeries& base, const CompareSeries& other);
    // Window-independent: a mark at a bar depends only on that bar and the five
    // before it, each with both correlations. A new page that drops the oldest
    // bar therefore never moves or invents a mark in the bars both pages share.
    static QList<RelationChange> relationChanges(const QList<CorrelationPoint>&);
    static QString relationLabel(std::optional<double> r);
    static bool underlyingWeekend(const CompareSpec&, qint64 openMs);
    static std::optional<double> pearson(const QList<std::pair<double, double>>&);
};
} // namespace keefi
