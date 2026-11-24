#pragma once
#include "keefi/FundFlowRequests.h"
#include <QStringList>

namespace keefi {
struct FlowQuery {
    FlowSource source = FlowSource::UpbitDeposits;
    int page = 1;                     // Upbit page, or Binance transfer `current`.
    int offset = 0;                   // Binance deposit/withdrawal offset.
    qint64 fromMs = 0, throughMs = 0; // Binance window, inclusive. Upbit lists have none.
    QString transferType;             // Binance universal transfer type.
    QStringList uuids;                // Upbit exact recheck of records that were still pending.
    bool operator==(const FlowQuery&) const = default;
};
// One provider row, NOT an AssetEvent. IDs are raw only between parsePage()
// and FundFlowTracking::merge(), which seals them before anything is stored.
struct FlowRecord {
    FlowSource source = FlowSource::UpbitDeposits;
    QString id, reference, pendingRef;
    QString asset, amount, fee, state, type;
    int wallet = -1;                  // Binance walletType: 0 Spot, 1 Funding.
    bool internal = false;            // Upbit `internal` or Binance transferType 1.
    qint64 createdMs = 0, doneMs = 0, receivedAtMs = 0;
    bool final = false, success = false;
    bool operator==(const FlowRecord&) const = default;
};
struct FlowPage {
    bool ok = false;
    QString errorCode;
    int total = -1;                   // Binance transfers only.
    QList<FlowRecord> rows;
};
class FundFlowProtocol final {
public:
    static constexpr int UpbitPageSize = 100, BinanceLimit = 1000, TransferPageSize = 100;
    static constexpr qint64 BinanceWindowMs = 30LL * 86400000 - 1, TransferWindowMs = 7LL * 86400000 - 1;
    static RequestSpec request(const FlowQuery&, const CredentialSet&, qint64 serverNowMs, QString* error = nullptr);
    static FlowPage parsePage(const FlowQuery&, const HttpResult&, qint64 receivedAtMs);
    static bool validQuery(const FlowQuery&, qint64 nowMs);
    static QString label(FlowSource);
};
} // namespace keefi
