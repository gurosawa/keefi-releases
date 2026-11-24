#pragma once
#include "keefi/TradingTypes.h"

namespace keefi {
// Pure REST codec. Only explicit endpoint metadata is retained; no credentials.
class OkxAdapter final : public ExchangeAdapter {
public:
    explicit OkxAdapter(const AccountRecord& account);
    QString provider() const override { return QStringLiteral("okx"); }
    ExchangePolicy exchangePolicy() const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation, const AccountRecord&, const OrderInput&,
                      const CredentialSet&, qint64 serverNowMs, QString* error) const override;
    AdapterResult parse(Operation, const HttpResult&, const OrderInput&) const override;
    OrderValidation validate(const OrderInput&, const MarketSnapshot&, const OrderLimits&) const override;
    QString amendDescription() const override;
private:
    QString region_, environment_;
};
} // namespace keefi
