#pragma once
#include "keefi/TradingTypes.h"
#include <functional>
namespace keefi {
// Pure REST codec: no sockets, credential retention or automatic mutation retry.
class BithumbAdapter final : public ExchangeAdapter {
public:
    explicit BithumbAdapter(std::function<QString()> nonce = {});
    QString provider() const override { return QStringLiteral("bithumb"); }
    ExchangePolicy exchangePolicy() const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation, const AccountRecord&, const OrderInput&, const CredentialSet&,
                      qint64 serverNowMs, QString* error) const override;
    AdapterResult parse(Operation, const HttpResult&, const OrderInput&) const override;
    OrderValidation validate(const OrderInput&, const MarketSnapshot&, const OrderLimits&) const override;
    QString amendDescription() const override;
private:
    std::function<QString()> nonce_;
};
} // namespace keefi
