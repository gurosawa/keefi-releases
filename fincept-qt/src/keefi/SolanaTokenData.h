#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/SwapTypes.h"

namespace keefi {
// Public mint lookups. No wallet address, balance, credential or order is sent.
struct SolanaTokenMetadata {
    QString mint, symbol, name, program;
    int decimals = -1;
    AssetEvidence evidence;
};
struct SolanaTokenPrice {
    QString mint, usd, block;
    int decimals = -1;
};
class SolanaTokenData final {
public:
    static SwapRequest request(bool metadata, const QStringList& mints);
    static SwapRequest blockTimesRequest(const QString& id, const QStringList& blocks);
    static QList<SolanaTokenMetadata> metadata(const HttpResult&, const QStringList&, QString* error);
    static QList<SolanaTokenPrice> prices(const HttpResult&, const QStringList&, QString* error);
    static QMap<QString,qint64> blockTimes(const HttpResult&, const QString& id, const QStringList&, QString* error);
    static bool displayTextValid(const QString&, int maximum);
    static bool supportsBaseUnits(const QJsonValue& mintAccount, const QString& program, int decimals);
};
// Decimal long division rounded down at 24 fractional digits, never binary float.
// Used only for an explicit USD price / USDC USD price conversion.
QString solanaPriceRatio(const QString& numerator, const QString& denominator);
}
