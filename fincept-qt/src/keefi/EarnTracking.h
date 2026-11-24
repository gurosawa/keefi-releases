#pragma once
#include "keefi/BinanceEarnProtocol.h"
#include <QJsonArray>

namespace keefi {
struct EarnObservation {
    QString key; // Keyed source identity; never a raw transaction or position ID.
    quint64 version = 0;
    EarnRecord record;
    bool conflict = false;
    bool operator==(const EarnObservation&) const = default;
};
struct EarnWindow {
    EarnSource source = EarnSource::FlexibleSubscriptions;
    qint64 fromMs = 0, throughMs = 0, receivedAtMs = 0;
    bool operator==(const EarnWindow&) const = default;
};
struct EarnAccountState {
    QString accountScope, connectionId, binding;
    quint64 revision = 0;
    qint64 trackingStartMs = 0;
    QList<EarnObservation> observations;
    QList<EarnRecord> positions;
    QMap<int, qint64> positionsAt;
    QList<EarnWindow> windows; // Successfully fetched query spans, NOT accounting completeness.
    bool operator==(const EarnAccountState&) const = default;
};
struct EarnBatch {
    EarnQuery query;
    qint64 receivedAtMs = 0;
    QList<EarnRecord> rows;
};
// Page assembly never marks an incomplete/changed/duplicate page chain complete.
class EarnPageAccumulator final {
public:
    bool add(const EarnQuery&, const EarnPage&, qint64 receivedAtMs, QString* error = nullptr);
    bool complete() const;
    EarnBatch batch() const;
    void clear();
private:
    EarnBatch batch_;
    int expectedPage_ = 1, total_ = -1, size_ = 0;
    QSet<QString> ids_;
    bool failed_ = false;
};

class EarnTracking final {
public:
    static constexpr int MaxAccounts = 64, MaxObservations = 20000, MaxPositions = 2048, MaxWindows = 4096;
    // KEEFI polling policy, not a provider settlement SLA. Expiry never makes a
    // record complete and never deletes it. Applies to targeted pending rechecks.
    static constexpr qint64 PendingRecheckGraceMs = 7LL * 86400000;
    enum class Recheck { NotNeeded, Pending, Expired };
    static Recheck recheck(const EarnRecord&, qint64 nowMs);
    // Owner-thread only; ledger root key must be borrowed synchronously. No key
    // or raw official ID is retained in the returned persistent state.
    static bool merge(const EarnAccountState&, const EarnBatch&, const SecureBuffer& rootKey,
                      EarnAccountState* out, QString* error = nullptr);
    static QList<EarnObservation> active(const EarnAccountState&);
    static QList<EarnQuery> queries(const EarnAccountState&, qint64 nowMs);
    static QJsonObject encode(const EarnAccountState&);
    static bool decode(const QJsonObject&, EarnAccountState*);
    static bool sameMeaning(EarnRecord a, EarnRecord b);
    static bool validAccount(const EarnAccountState&);
};
} // namespace keefi
