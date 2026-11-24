#pragma once
#include "keefi/FundFlowProtocol.h"
#include "keefi/SecureBuffer.h"
#include <QJsonObject>

namespace keefi {
struct FlowObservation {
    QString key;          // Keyed identity; never a raw provider ID.
    quint64 version = 0;
    FlowRecord record;    // record.pendingRef is the only raw ID, kept while not final.
    bool conflict = false;
    bool operator==(const FlowObservation&) const = default;
};
// A fully read request span. For Upbit lists it is the span the newest-first
// pages were read through; it is not a promise that late rows cannot appear.
struct FlowWindow {
    FlowSource source = FlowSource::UpbitDeposits;
    QString transferType;
    qint64 fromMs = 0, throughMs = 0, receivedAtMs = 0;
    bool operator==(const FlowWindow&) const = default;
};
struct FlowAccountState {
    QString accountScope, connectionId, binding, provider, market;
    quint64 revision = 0;
    qint64 trackingStartMs = 0;
    qint64 upbitConfirmedAtMs = 0; // User said Upbit pockets/staking are not used.
    QList<FlowObservation> observations;
    QList<FlowWindow> windows;
    bool operator==(const FlowAccountState&) const = default;
};
struct FlowJob {
    FlowQuery query;
    qint64 stopAtMs = 0;                  // Upbit: read pages until a row older than this.
    qint64 coverFromMs = 0, coverThroughMs = 0; // Window recorded once every page is read.
    bool operator==(const FlowJob&) const = default;
};
struct FlowBatch {
    QList<FlowWindow> windows;
    QList<FlowRecord> rows;
    qint64 receivedAtMs = 0;
};
class FundFlowTracking final {
public:
    static constexpr int MaxAccounts = 64, MaxObservations = 20000, MaxWindows = 4096, MaxUpbitPages = 100;
    static constexpr qint64 Day = 86400000;
    static constexpr qint64 FirstLookbackMs = 7 * Day, OverlapMs = Day, PendingRecheckMs = 7 * Day;
    // Rows can be listed a little after they happen; coverage stops this far before a read.
    static constexpr qint64 ListingLagMs = 60000;
    static QStringList transferTypes(const FlowAccountState&);
    static QList<FlowSource> sources(const FlowAccountState&);
    static QList<FlowJob> plan(const FlowAccountState&, qint64 runThroughMs);
    // Owner thread; the ledger root key is borrowed synchronously and never kept.
    static bool merge(const FlowAccountState&, const FlowBatch&, const SecureBuffer& rootKey,
                      FlowAccountState* out, QString* error = nullptr);
    // True when the merged windows for this source cover [fromMs, throughMs].
    static bool covers(const FlowAccountState&, FlowSource, const QString& transferType, qint64 fromMs, qint64 throughMs);
    static qint64 coverageStart(const FlowAccountState&);
    static QJsonObject encode(const FlowAccountState&);
    static bool decode(const QJsonObject&, FlowAccountState*);
    static bool validAccount(const FlowAccountState&);
};
} // namespace keefi
