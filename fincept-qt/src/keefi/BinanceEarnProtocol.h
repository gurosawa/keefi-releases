#pragma once
#include "keefi/BinanceEarnRequests.h"
#include <QStringList>

namespace keefi {
struct EarnQuery {
    EarnSource source = EarnSource::FlexiblePositions;
    qint64 fromMs = 0, throughMs = 0;
    int page = 1, size = 100;
    bool operator==(const EarnQuery&) const = default;
};
// Typed, minimum source observations, NOT executable orders or AssetEvents.
// IDs are raw only inside parsePage(); the collector seals them before storage.
struct EarnRecord {
    EarnSource source = EarnSource::FlexiblePositions;
    QString id, positionId, productId, asset, amount;
    QString type, status, sourceWallet, destinationWallet;
    QString spotAmount, fundingAmount, originalAmount, lossAmount;
    QString rewardAsset, rewardAmount, collateralAmount, redeemingAmount;
    qint64 timeMs = 0, deliveryMs = 0, receivedAtMs = 0;
    std::optional<bool> payoutComplete; // Official isComplete, distinct from local completeness.
    bool complete = false, autoSubscribe = false;
    QStringList issues; // Fixed local codes only, never provider text.
    bool operator==(const EarnRecord&) const = default;
};
struct EarnPage {
    bool ok = false;
    QString errorCode;
    int total = 0;
    QList<EarnRecord> rows;
};
// Qt keeps only one of duplicate JSON object keys; private reads reject them first.
bool jsonObjectKeysUnique(const QByteArray& bytes);
class BinanceEarnProtocol final {
public:
    static constexpr int RequestWeight = 150;
    static constexpr qint64 WindowMs = 30LL * 86400000;
    static constexpr int MaxRows = 20000;
    static RequestSpec request(const EarnQuery&, const CredentialSet&, qint64 serverNowMs, QString* error = nullptr);
    static EarnPage parsePage(const EarnQuery&, const HttpResult&, qint64 receivedAtMs);
    // Length-delimited identities. Amount/status/receipt are not identity parts.
    static QString identity(const EarnRecord&);
    static QString label(EarnSource);
    static QString issueLabel(const QString&);
    static bool validQuery(const EarnQuery&, qint64 nowMs);
    // FUND is a legacy alias of the history response's FUNDING. Apply on read
    // and semantic comparison too, so spelling-only changes do not conflict.
    static void normalizeFlexibleDestination(EarnRecord&);
};
} // namespace keefi
