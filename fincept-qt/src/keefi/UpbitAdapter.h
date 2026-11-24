#pragma once

#include "keefi/TradingTypes.h"

namespace keefi {

// Pure request construction, response parsing and validation. No sockets, timers,
// credential retention or automatic retries live in this adapter.
class UpbitAdapter final : public ExchangeAdapter {
  public:
    QString provider() const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation operation, const AccountRecord& account, const OrderInput& input,
        const CredentialSet& credentials, qint64 serverNowMs, QString* error) const override;
    AdapterResult parse(Operation operation, const HttpResult& response, const OrderInput& input) const override;
    OrderValidation validate(const OrderInput& input, const MarketSnapshot& snapshot,
        const OrderLimits& limits) const override;
    QString amendDescription() const override;
};
// "Bearer <JWT>" without a query hash, for one Upbit private WebSocket handshake.
// Throws like the REST signer when the read key is missing or malformed.
QByteArray upbitConnectionAuthorization(const CredentialSet& credentials);
// "Bearer <JWT>" whose query hash covers the exact unencoded query string.
QByteArray upbitQueryAuthorization(const CredentialSet& credentials, const QByteArray& unencodedQuery);

} // namespace keefi
