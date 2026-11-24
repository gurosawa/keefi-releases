#pragma once
#include "keefi/TradingTypes.h"
namespace keefi {
// HMAC-only USDⓈ-M REST codec, with ordinary and conditional ID namespaces.
class BinanceUsdmAdapter final : public ExchangeAdapter {
public:
    explicit BinanceUsdmAdapter(const AccountRecord&);
    QString provider() const override { return QStringLiteral("binance"); }
    ExchangePolicy exchangePolicy() const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation,const AccountRecord&,const OrderInput&,const CredentialSet&,
                      qint64 serverNowMs,QString* error)const override;
    AdapterResult parse(Operation,const HttpResult&,const OrderInput&)const override;
    OrderValidation validate(const OrderInput&,const MarketSnapshot&,const OrderLimits&)const override;
    QString amendDescription()const override;
private:
    QString environment_;
};
} // namespace keefi
