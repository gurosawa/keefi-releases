#pragma once

#include "keefi/MovementDirection.h"
#include "keefi/WorldMarkets.h"
#include <QList>
#include <QString>

namespace keefi {

// Chart header context built only from public reads the app already holds:
// Hana Bank USD/KRW (Today's world board), Upbit and Binance live prices, and
// the world indices. Display only: never an alert, order, valuation or history input.
struct ChartMarketInputs {
    QString market, source, code;                 // The selected chart identity.
    QString price, currency;                      // The chart's displayed price, plain decimal, and its own currency.
    QString upbitKrw;    qint64 upbitAtMs = 0;    // Upbit KRW price of the same coin when Upbit lists it.
    QString binanceUsdt; qint64 binanceAtMs = 0;  // Binance spot USDT price of the same coin when it streams.
    WorldMarketsSnapshot world;
    qint64 nowMs = 0;
};

struct ChartMarketItem {
    QString label, value, change, detail;
    MovementDirection direction = MovementDirection::Unknown;
    bool closed = false, late = false;
    bool operator==(const ChartMarketItem&) const = default;
};

struct ChartMarketContext {
    QString identityKey;                  // market:source:code; the header shows it only for that chart.
    QString conversion, conversionDetail; // "≈ 9,532,100원" or "바이낸스 106,946,000원".
    QString premium, premiumDetail;       // "김프 +0.02%" when both exchange prices are fresh.
    QList<ChartMarketItem> related;       // Most relevant first; the header drops trailing items that do not fit.
    bool operator==(const ChartMarketContext&) const = default;
    bool empty() const { return conversion.isEmpty() && premium.isEmpty() && related.isEmpty(); }
};

namespace chartmarket {
inline constexpr qint64 PriceFreshnessMs = 90000;  // Each exchange leg of the premium.
inline constexpr qint64 MaximumSkewMs = 60000;      // Between the two exchange legs.
}

ChartMarketContext chartMarketContext(const ChartMarketInputs&);
// Won amounts for the header: whole won from 100, two decimals from 1, four significant digits below.
QString chartKrwAmount(double won);

} // namespace keefi
