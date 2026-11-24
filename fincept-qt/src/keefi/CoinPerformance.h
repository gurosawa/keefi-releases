#pragma once
#include "keefi/EarnTracking.h"
#include "keefi/FundFlowTracking.h"
#include "keefi/PortfolioCheckpoint.h"
#include <QJsonObject>
#include <QMap>

namespace keefi {
// G9-C1-a: coin investment PnL for Upbit and Binance (Spot, USD-M). The
// protected performance() formula is used unchanged; this module decides
// whether its inputs may be called complete, and says why when they may not.
struct CoinFlowLine {
    qint64 timeMs = 0;
    QString source, asset, amount, direction, note;
};
struct CoinPeriodResult {
    QString name;
    bool available = false;
    QString reason;          // First blocking reason, plain Korean.
    QStringList reasons;     // Every distinct blocking reason.
    qint64 fromMs = 0, toMs = 0;
    QString opening, closing, inflow, outflow, pnl; // KRW, exact decimal strings.
};
struct CoinPerformanceInput {
    QList<AssetSnapshot> history;   // Current-ledger history.
    QList<AssetSnapshot> today;     // Today's window including archives, when loaded.
    QList<AssetQuote> quotes;
    QList<AssetSnapshot> current;   // Latest snapshot per account scope.
    QList<FlowAccountState> flows;
    QList<EarnAccountState> earn;
    QJsonObject baseline;           // Stored fixed start, possibly empty.
    QMap<QString, QString> flowErrors; // Account scope -> last collector problem.
    qint64 nowMs = 0;
};
struct CoinPerformanceView {
    bool inScope = false;
    CoinPeriodResult today, sinceStart;
    QList<CoinFlowLine> lines;              // Movements in the since-start period.
    QStringList upbitScopesToConfirm;
    QJsonObject proposedBaseline;           // A new fixed start the owner should store.
};
class CoinPerformance final {
public:
    // A movement this close to a balance record cannot be placed on either side of it.
    static constexpr qint64 BoundaryMs = 60000;
    // Coverage must start before the earliest snapshot an opening may use.
    static constexpr qint64 PreOpeningMs = DisplayFreshnessMs + 2 * 60000;
    static bool inScope(const AssetSnapshot&);
    static CoinPerformanceView evaluate(const CoinPerformanceInput&);
};
} // namespace keefi
