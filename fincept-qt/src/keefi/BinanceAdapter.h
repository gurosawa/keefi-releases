#pragma once
#include "keefi/TradingTypes.h"

namespace keefi {
// Stateless HMAC-only Spot codec. No transport, key storage, retries or logging.
class BinanceAdapter final : public ExchangeAdapter {
  public:
    QString provider() const override { return QStringLiteral("binance"); }
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation operation, const AccountRecord& account,
        const OrderInput& input, const CredentialSet& credentials, qint64 serverNowMs,
        QString* error) const override;
    AdapterResult parse(Operation operation, const HttpResult& response,
        const OrderInput& input) const override;
    OrderValidation validate(const OrderInput& input, const MarketSnapshot& snapshot,
        const OrderLimits& limits) const override;
    QString amendDescription() const override;
};
} // namespace keefi
