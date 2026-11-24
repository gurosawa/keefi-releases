#pragma once
#include "keefi/ChartShareCard.h"
#include <optional>

namespace keefi {
enum class AssetSharePeriod { Day, Week, Month, HalfYear };
struct AssetShareTodayObservation {
    QString difference, currency;
    qint64 fromMs = 0, toMs = 0;
};
// Optional current holding facts only. These never participate in market-price
// changes, and cannot represent cost basis, entry price or investment P/L.
struct AssetSharePersonalValues {
    QString quantity, currentValuation, currency;
    qint64 observedAtMs = 0;
    bool valuationComplete = false;
    QStringList custodyProviders; // Fixed provider IDs only; never account aliases.
    std::optional<AssetShareTodayObservation> today;
    QString todayUnavailableReason;
};
struct AssetShareSnapshot {
    QString assetName, assetSymbol;
    AssetSharePeriod period = AssetSharePeriod::Day;
    qint64 requestedFromMs = 0, requestedToMs = 0; // Exact completed-price times.
    ChartShareSnapshot market;
    std::optional<AssetSharePersonalValues> personal;
};
struct AssetShareOptions {
    QSize outputSize{1000,1200};
    bool includeQuantity = false, includeValuation = false, includeCustody = false, includeTodayObservation = false;
};
struct AssetShareRenderResult {
    bool available = false, personalValuesIncluded = false;
    QString reason;
    QImage image;
    QStringList captions;
    // Public period endpoints and exact signed change, never holding returns.
    QString startPrice, currentPrice, changeAmount, changePercent;
    qint64 fromMs = 0, toMs = 0;
    // Exact text painted as the card's largest market-change number.
    QString heroText;
};
class AssetShareCard final {
  public:
    static qint64 intervalMs(AssetSharePeriod);
    static qint64 durationMs(AssetSharePeriod);
    static QString periodLabel(AssetSharePeriod);
    static AssetShareRenderResult render(const AssetShareSnapshot&, const AssetShareOptions& = {});
    static bool savePng(const AssetShareSnapshot&, const AssetShareOptions&, const QString& absoluteLocalPath, QString* error = nullptr);
};
}
Q_DECLARE_METATYPE(keefi::AssetSharePeriod)
