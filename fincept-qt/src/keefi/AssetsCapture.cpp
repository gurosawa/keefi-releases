#include "keefi/AssetsCapture.h"
#include <QCryptographicHash>
#include <QJsonArray>
namespace keefi {
namespace {
QString hash(const QString& v){return QString::fromLatin1(QCryptographicHash::hash(v.toUtf8(),QCryptographicHash::Sha256).toHex());}
QString origin(const QString& provider){
    if(provider=="binance")return "https://api.binance.com";if(provider=="upbit")return "https://api.upbit.com";
    if(provider=="bithumb")return "https://api.bithumb.com";if(provider=="okx")return "https://openapi.okx.com";return {};
}
AssetSnapshot snapshot(const AccountRecord& a,const QString& scope,qint64 received,const QString& coverage){
    AssetSnapshot s;s.provider=a.provider;s.environment=a.environment;s.market=a.market;s.accountScope=hash(a.provider+":"+a.environment+":"+a.market+":"+scope);
    s.timeMs=received;s.id=hash(s.accountScope+":"+QString::number(received));s.connectionId=a.id;s.coverage=coverage;return s;
}
void observe(AssetCapture& c,const QList<OrderRecord>& rows,const AssetEvidence& evidence){
    if(evidence.receivedAtMs<=0)return;
    for(const auto& r:rows){AssetOrderObservation o;o.accountScope=c.snapshot.accountScope;o.provider=c.snapshot.provider;o.market=c.snapshot.market;
        const bool algo=r.futures&&r.futures->family==OrderFamily::Conditional;o.orderFamily=(algo||r.brokerConditional)?"conditional":"ordinary";o.orderId=algo?r.algoId:r.orderId;
        if(o.orderId.isEmpty())continue;o.observedAtMs=evidence.receivedAtMs;o.side=r.side;o.type=r.type;o.status=r.status;o.price=r.price;o.quantity=r.quantity;
        o.executed=r.executed;o.quoteAmount=r.quoteAmount;o.quantityUnit=r.quantityUnit;o.currency=r.currency;o.evidence=evidence;c.orders.append(o);
    }
}
}
AssetCapture captureExchangeAssets(const AccountRecord& a,const QString& scope,const MarketSnapshot& raw,qint64 received,qint64 historyReceived,
    bool complete,bool reconciled,const QString& coverage,const QList<OrderRecord>& history){
    AssetCapture c;if(scope.isEmpty()||received<=0)return c;c.snapshot=snapshot(a,scope,received,coverage);auto& s=c.snapshot;
    s.balanceComplete=complete;s.reconciled=reconciled;s.evidence={a.provider+" 공식 잔고",origin(a.provider),s.accountScope,0,received,complete};
    if(a.market=="usdm_futures")s.evidence.url="https://fapi.binance.com/fapi/v3/account";
    if(a.provider=="okx")s.evidence.url=a.region=="us"?"https://us.okx.com":a.region=="eea"?"https://eea.okx.com":"https://openapi.okx.com";
    for(const auto& b:raw.balances){AssetComponent part;part.valuationGroup=b.section.isEmpty()?a.market:b.section;part.id=part.valuationGroup+":"+b.asset;part.asset=b.asset;part.evidence=s.evidence;
        if(!b.equity.isEmpty()){part.kind=AssetComponentKind::Equity;part.quantity=b.equity;}
        else{part.kind=AssetComponentKind::Holding;part.quantity=decimal::valid(b.free)&&decimal::valid(b.locked)?decimal::add(b.free,b.locked):QString{};}
        if(a.market=="usdm_futures")for(const auto& v:raw.data.value(Operation::Account).value("assets").toArray())if(v.toObject().value("asset")==b.asset){
            part.providerUnrealizedPnl=v.toObject().value("unrealizedProfit").toString();part.reportedValueCurrency=b.asset;break;}
        s.components.append(part);
    }
    // Keep open exposure visible even when account equity nets to zero. These
    // informational rows are excluded from net assets by AssetValuation.
    if (a.market == "usdm_futures") for (const auto& position : raw.positions) {
        AssetComponent part;
        part.id = "position:" + position.symbol + ":" + position.positionSide;
        part.asset = position.symbol;
        part.quantity = position.amount;
        part.kind = AssetComponentKind::PositionNotional;
        part.valuationGroup = "positions";
        part.evidence = s.evidence;
        s.components.append(part);
    }
    const auto quote=raw.data.value(Operation::Quote),rules=raw.data.value(Operation::Rules);const auto price=quote.value("price").toString();
    QString base=rules.value("baseAsset").toString(),currency=rules.value("quoteAsset").toString();
    if(!base.isEmpty()&&!currency.isEmpty()&&decimal::positive(price)&&raw.quoteReceivedAtMs>0){
        AssetQuote q{base,currency,price,{a.provider+" 공식 시세",origin(a.provider),s.accountScope,quote.value("timestamp").toString().toLongLong(),raw.quoteReceivedAtMs,true}};
        c.quotes.append(q);
    }
    auto h=s.evidence;h.source=a.provider+" 공식 주문 이력 관측";h.receivedAtMs=historyReceived;observe(c,history,h);return c;
}
AssetCapture captureBrokerAssets(const AccountRecord& a,const QString& scope,const QJsonObject& holdings,const QJsonObject& cash,const QJsonObject& buyingPower,
    const AssetEvidence& h,const AssetEvidence& cashEvidence,bool reconciled,const QString& coverage,const QList<OrderRecord>& history,const AssetEvidence& he){
    AssetCapture c;if(scope.isEmpty()||h.receivedAtMs<=0)return c;c.snapshot=snapshot(a,scope,h.receivedAtMs,coverage);auto& s=c.snapshot;s.evidence=h;s.evidence.scope=s.accountScope;s.reconciled=reconciled;
    s.balanceComplete=h.complete;
    for(const auto& v:holdings.value("items").toArray()){const auto row=v.toObject();AssetComponent p;p.id=row.value("positionKey").toString();if(p.id.isEmpty())p.id=row.value("symbol").toString();
        p.asset=row.value("symbol").toString();p.quantity=row.value("quantity").toString();p.reportedValue=row.value("marketValue").toObject().value("amount").toString();p.reportedValueCurrency="KRW";
        p.providerUnrealizedPnl=row.value("unrealizedPnl").toString();if(p.providerUnrealizedPnl.isEmpty())p.providerUnrealizedPnl=row.value("profitLoss").toObject().value("amount").toString();
        p.costBasis=row.value("marketValue").toObject().value("purchaseAmount").toString();if(!p.costBasis.isEmpty())p.costCurrency="KRW";
        p.evidence=s.evidence;s.components.append(p);
        if(row.contains("cashHolding")&&row.value("cashHolding")!=QJsonValue(true))s.balanceComplete=false; // Loan/debt basis is incomplete.
    }
    AssetComponent deposit;deposit.id="depositCash";deposit.asset="KRW";deposit.kind=AssetComponentKind::Cash;deposit.evidence=cashEvidence;deposit.evidence.scope=s.accountScope;
    deposit.quantity=cash.value("depositCash").toString();if(deposit.quantity.isEmpty()){deposit.quantity=holdings.value("depositCash").toString();deposit.evidence=s.evidence;}
    if(deposit.quantity.isEmpty()||!deposit.evidence.complete)s.balanceComplete=false;s.components.append(deposit);
    const auto power=buyingPower.value("cashBuyingPower").toString();if(!power.isEmpty()){AssetComponent p;p.id="buyingPower";p.asset="KRW";p.quantity=power;p.kind=AssetComponentKind::BuyingPower;p.evidence=s.evidence;s.components.append(p);}
    auto historyEvidence=he;historyEvidence.scope=s.accountScope;observe(c,history,historyEvidence);return c;
}
}
