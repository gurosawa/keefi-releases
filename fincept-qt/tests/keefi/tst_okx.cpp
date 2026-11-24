#include "keefi/OkxAdapter.h"
#include "keefi/OkxProtocol.h"
#include "fixtures/OkxResponses.h"
#include <QMessageAuthenticationCode>
#include <QTest>
#include <QUrlQuery>
#include <sodium.h>
using namespace keefi;
using namespace keefi::test;
namespace {
CredentialSet fakeKeys() {
    CredentialSet c;c.emplace("api_key",SecureBuffer::fromUtf8("fakeKeyOnly"));
    c.emplace("secret",SecureBuffer::fromUtf8("fakeSecretOnly"));c.emplace("passphrase",SecureBuffer::fromUtf8(" pass phrase "));return c;
}
MarketSnapshot ready(OkxAdapter& adapter,const OrderInput& in=okxInput()) {
    MarketSnapshot s;const auto a=okxAccount();auto keys=fakeKeys();QString e;const auto now=QDateTime::currentMSecsSinceEpoch();
    s.serverNowMs=now;
    for(auto op:{Operation::AccountConfig,Operation::Account,Operation::Rules,Operation::Commissions,Operation::BuyingPower,
                Operation::FxUsd,Operation::FxUsdt,Operation::PriceLimits,Operation::Quote,Operation::Depth,Operation::OpenOrders}){
        const auto req=adapter.build(op,a,in,keys,now,&e);const auto r=adapter.parse(op,okxResponse(req,now),in);
        if(!e.isEmpty()||!r.ok){
            const auto diagnostic=QString("Fake OKX fixture contract failed: operation=%1 buildFailed=%2 code=%3")
                .arg(operationName(op)).arg(!e.isEmpty()).arg(r.code);
            QTest::qFail(qPrintable(diagnostic),__FILE__,__LINE__);return {};
        }
        s.data[op]=r.data;if(op==Operation::Account)s.balances=r.balances;if(op==Operation::OpenOrders)s.openOrders=r.orders;
    }return s;
}
OrderLimits limits(){return {"1000","USDT","10"};}
}
class OkxTest final:public QObject {
    Q_OBJECT
private slots:
    void invalidAuthBodyAndTimeout();
    void exactSigningAndPassphrase();
    void initTestCase(){QVERIFY(sodium_init()>=0);}

    void regionsPublicAndRetiredEnvironmentIsolation();

    void configScopeAndPrivacy();
    void balanceSectionsAndUnknownValues();
    void depthFixturePreservesNestedLevels();
    void quoteBuyNeverSubtractsBaseFills();

    void paginationAccountScopeAndBounds();
    void knownRulesAcceptLimitAndMarket();
    void permissionModeAndBorrowBoundaries();
    void feeGroupSignAndFeeCurrency();
    void dollarAndTetherAreDistinct();
    void tickSizeDepthAndStaleFx();
    void sameOrderAmendTotalAndReservedPrincipal();
};

void OkxTest::regionsPublicAndRetiredEnvironmentIsolation(){
    for(auto region:{QString("global"),QString("us"),QString("eea")}){
        auto a=okxAccount("live",region);OkxAdapter x(a);QString e;auto k=fakeKeys();
        auto publicRequest=x.build(Operation::Markets,a,{}, {},0,&e);QVERIFY(e.isEmpty());QVERIFY(!publicRequest.authenticated);
        for(auto it=publicRequest.headers.cbegin();it!=publicRequest.headers.cend();++it)QVERIFY(!it.key().startsWith("OK-ACCESS"));
        auto privateRequest=x.build(Operation::Account,a,{},k,1700000000123LL,&e);QVERIFY(e.isEmpty());QVERIFY(!privateRequest.headers.contains("x-simulated-trading"));
        auto wrong=a;wrong.environment="demo";x.build(Operation::Account,wrong,{},k,1700000000123LL,&e);QVERIFY(!e.isEmpty());
    }
    auto a=okxAccount();a.region="";OkxAdapter x(a);QString e;x.build(Operation::Time,a,{}, {},0,&e);QVERIFY(!e.isEmpty());
}

void OkxTest::configScopeAndPrivacy(){
    const auto a=okxAccount();OkxAdapter x(a);RequestSpec req;req.operation=Operation::AccountConfig;
    const auto r=x.parse(req.operation,okxResponse(req),{});QVERIFY(r.ok);QVERIFY(r.tradingPermissionKnown);QVERIFY(r.tradingAllowed);
    QCOMPARE(r.scope,okxAccountScope("global","live","123456789"));
    auto text=QJsonDocument(r.data).toJson();QVERIFY(!text.contains("123456789"));QVERIFY(!text.contains("PRIVATE"));
    QVERIFY(okxAccountScope("global","demo","123456789").isEmpty());QVERIFY(r.scope!=okxAccountScope("us","live","123456789"));
    const auto partial=x.parse(Operation::AccountConfig,okxJson({QJsonObject{{"uid","123456789"},{"enableSpotBorrow","false"}}}),{});
    QVERIFY(partial.ok);QVERIFY(!partial.tradingPermissionKnown);QVERIFY(!partial.tradingAllowed);
    QVERIFY(partial.data.value("acctLv").isNull());QVERIFY(partial.data.value("enableSpotBorrow").isNull());
}
void OkxTest::balanceSectionsAndUnknownValues(){
    OkxAdapter x(okxAccount());RequestSpec req;req.operation=Operation::Account;auto r=x.parse(req.operation,okxResponse(req),{});
    QVERIFY(r.ok);QCOMPARE(r.balances.size(),2);QCOMPARE(r.balances[0].section,QString("trading"));QVERIFY(r.balances[0].liability.isEmpty());
    req.operation=Operation::FundingBalance;r=x.parse(req.operation,okxResponse(req),{});QVERIFY(r.ok);QCOMPARE(r.balances[0].section,QString("funding"));
    const QJsonObject malformedBalance{{"ccy","BTC"},{"availBal","garbage1"},{"frozenBal","0"}};
    auto bad=okxJson({QJsonObject{{"details",QJsonArray{malformedBalance}}}});
    QVERIFY(!x.parse(Operation::Account,bad,{}).ok);
}
void OkxTest::depthFixturePreservesNestedLevels(){
    RequestSpec req;req.operation=Operation::Depth;
    const auto response=okxResponse(req);
    const auto data=QJsonDocument::fromJson(response.body).object().value("data").toArray();
    QCOMPARE(data.size(),1);
    for(const auto* side:{"bids","asks"}){
        const auto levels=data[0].toObject().value(QLatin1String(side)).toArray();
        QCOMPARE(levels.size(),1);QVERIFY(levels[0].isArray());QCOMPARE(levels[0].toArray().size(),4);
    }
    OkxAdapter x(okxAccount());QVERIFY(x.parse(Operation::Depth,response,okxInput()).ok);
}
void OkxTest::quoteBuyNeverSubtractsBaseFills(){
    OkxAdapter x(okxAccount());auto i=okxInput();i.orderId="100";
    auto r=x.parse(Operation::QueryOrder,okxJson({okxOrder("100","partially_filled","buy","market","200","1.25")}),i);
    QVERIFY(r.ok);QCOMPARE(r.orders.size(),1);auto o=r.orders[0];QCOMPARE(o.quantityUnit,QString("quote"));
    QCOMPARE(o.requestedQuoteAmount,QString("200"));QCOMPARE(o.executedBaseAmount,QString("1.25"));QVERIFY(o.quantity.isEmpty());QVERIFY(o.remaining.isEmpty());
}



void OkxTest::paginationAccountScopeAndBounds(){
    auto a=okxAccount();OkxAdapter x(a);QString e;auto i=okxInput();auto k=fakeKeys();
    auto request=x.build(Operation::OpenOrders,a,i,k,1700000000123LL,&e);QVERIFY(e.isEmpty());QVERIFY(!QUrlQuery(request.url).hasQueryItem("instId"));
    QJsonArray rows;for(int n=200;n>100;--n)rows.append(okxOrder(QString::number(n)));
    auto r=x.parse(Operation::OpenOrders,okxJson(rows),i);QVERIFY(r.ok);QVERIFY(r.hasMore);QCOMPARE(r.nextCursor,QString("101"));
    i.cursor="101";r=x.parse(Operation::OpenOrders,okxJson(rows),i);QVERIFY(!r.ok);
    i.cursor.clear();i.startTimeMs=1700000000000;i.endTimeMs=i.startTimeMs+8LL*86400000;
    x.build(Operation::History,a,i,k,1700000000123LL,&e);QVERIFY(!e.isEmpty());
}
void OkxTest::knownRulesAcceptLimitAndMarket(){
    OkxAdapter x(okxAccount());auto s=ready(x);auto i=okxInput();auto r=x.validate(i,s,limits());QVERIFY2(r.ok,qPrintable(r.message));
    i.type="market";i.price.clear();i.quantity.clear();i.quoteAmount="200";r=x.validate(i,s,limits());QVERIFY2(r.ok,qPrintable(r.message));QCOMPARE(r.notional,QString("200"));
    i.side="sell";i.quantity="2";i.quoteAmount.clear();r=x.validate(i,s,limits());QVERIFY2(r.ok,qPrintable(r.message));QCOMPARE(decimal::normalize(r.notional),QString("199.8"));
}
void OkxTest::permissionModeAndBorrowBoundaries(){
    OkxAdapter x(okxAccount());auto s=ready(x);auto c=s.data[Operation::AccountConfig];
    c["enableSpotBorrow"]=true;s.data[Operation::AccountConfig]=c;QVERIFY(!x.validate(okxInput(),s,limits()).ok);
    c["acctLv"]="2";c["enableSpotBorrow"]=QJsonValue::Null;c["autoLoan"]=QJsonValue::Null;s.data[Operation::AccountConfig]=c;
    QVERIFY(x.validate(okxInput(),s,limits()).ok);
    c["acctLv"]="3";s.data[Operation::AccountConfig]=c;QVERIFY(!x.validate(okxInput(),s,limits()).ok);
    c["acctLv"]="1";c["enableSpotBorrow"]=false;c["canTrade"]=false;s.data[Operation::AccountConfig]=c;QVERIFY(!x.validate(okxInput(),s,limits()).ok);
}
void OkxTest::feeGroupSignAndFeeCurrency(){
    OkxAdapter x(okxAccount());auto s=ready(x);QVERIFY(!s.balances.isEmpty());s.balances[0].free="200";s.data[Operation::BuyingPower]["availBuy"]="200";
    QVERIFY(x.validate(okxInput(),s,limits()).ok);s.data[Operation::AccountConfig]["feeType"]="1";QVERIFY(!x.validate(okxInput(),s,limits()).ok);
    auto fees=s.data[Operation::Commissions];fees["feeGroup"]=QJsonArray{QJsonObject{{"groupId","12"},{"maker","0.001"},{"taker","0.002"}}};
    s.data[Operation::Commissions]=fees;QVERIFY(x.validate(okxInput(),s,limits()).ok);
    fees["feeGroup"]=QJsonArray{QJsonObject{{"groupId","99"},{"maker","0"},{"taker","0"}}};s.data[Operation::Commissions]=fees;QVERIFY(!x.validate(okxInput(),s,limits()).ok);
}
void OkxTest::dollarAndTetherAreDistinct(){
    OkxAdapter x(okxAccount());auto s=ready(x);s.data[Operation::Rules]["maxLmtAmt"]="198";
    QVERIFY(x.validate(okxInput(),s,limits()).ok);s.data[Operation::FxUsd]["rate"]="1.01";QVERIFY(!x.validate(okxInput(),s,limits()).ok);
    s.data[Operation::FxUsd]["rate"]="0.99";s.data[Operation::FxUsd]["indexId"]="USDC-USD";QVERIFY(!x.validate(okxInput(),s,limits()).ok);
    s=ready(x);s.data[Operation::Rules]["maxMktSz"]="";auto i=okxInput();i.type="market";i.price.clear();i.quantity.clear();i.quoteAmount="200";QVERIFY(!x.validate(i,s,limits()).ok);
}
void OkxTest::tickSizeDepthAndStaleFx(){
    OkxAdapter x(okxAccount());auto s=ready(x);auto i=okxInput();i.price="100.05";QVERIFY(!x.validate(i,s,limits()).ok);
    i=okxInput();s.data[Operation::FxUsd]["ts"]=QString::number(QDateTime::currentMSecsSinceEpoch()-20000);QVERIFY(!x.validate(i,s,limits()).ok);
    // A corrected server clock may differ from the PC clock. Source age must use
    // the same server time as the service and request signer, not local wall time.
    s=ready(x);s.serverNowMs+=60000;s.data[Operation::FxUsd]["ts"]=QString::number(s.serverNowMs);
    QVERIFY(x.validate(i,s,limits()).ok);
    s.serverNowMs=0;QVERIFY(!x.validate(i,s,limits()).ok);
    s=ready(x);i.type="market";i.price.clear();i.quantity.clear();i.quoteAmount="200";
    s.data[Operation::Depth]["asks"]=QJsonArray{QJsonValue(QJsonArray{"100.1","0.1"})};
    const auto insufficient=x.validate(i,s,limits());QVERIFY(!insufficient.ok);QVERIFY(insufficient.message.contains("전체 시장가 주문"));
}
void OkxTest::sameOrderAmendTotalAndReservedPrincipal(){
    OkxAdapter x(okxAccount());auto s=ready(x);QVERIFY(!s.openOrders.isEmpty());QVERIFY(!s.balances.isEmpty());
    auto i=okxInput();i.orderId="100";i.price="100.1";i.amendRequestId="amend1";
    auto r=x.validate(i,s,limits());QVERIFY2(r.ok,qPrintable(r.message));
    s.openOrders[0].executed="1";s.openOrders[0].remaining="1";i.quantity="1";QVERIFY(!x.validate(i,s,limits()).ok);
    i.quantity="2";s.balances[0].free="1";s.data[Operation::BuyingPower]["availBuy"]="1";
    r=x.validate(i,s,limits());QVERIFY2(r.ok,qPrintable(r.message));
    s.openOrders[0].amendable=false;QVERIFY(!x.validate(i,s,limits()).ok);
}
void OkxTest::exactSigningAndPassphrase(){
    auto a=okxAccount();OkxAdapter x(a);QString e;auto k=fakeKeys();
    auto r=x.build(Operation::Rules,a,okxInput(),k,1700000000123LL,&e);QVERIFY2(e.isEmpty(),qPrintable(e));
    QCOMPARE(r.url.toEncoded(),QByteArray("https://openapi.okx.com/api/v5/account/instruments?instType=SPOT&instId=BTC-USDT"));
    QCOMPARE(r.headers.value("OK-ACCESS-PASSPHRASE"),QByteArray(" pass phrase "));
    const auto expected=QMessageAuthenticationCode::hash("2023-11-14T22:13:20.123ZGET/api/v5/account/instruments?instType=SPOT&instId=BTC-USDT","fakeSecretOnly",QCryptographicHash::Sha256).toBase64();
    QCOMPARE(expected,QByteArray("fbYhvcQnTbAwzY6I2T7eql/fKJP7XswmjUPWMM++2RQ="));
    QCOMPARE(r.headers.value("OK-ACCESS-SIGN"),expected);
    QVERIFY(!r.body.contains("fakeSecretOnly"));
}
void OkxTest::invalidAuthBodyAndTimeout() {
    OkxAdapter adapter(okxAccount());HttpResult response;response.status=401;response.body="<html>private</html>";
    auto result=adapter.parse(Operation::Account,response,okxInput());QVERIFY(result.authenticationFailed);QVERIFY(!result.unknown);QVERIFY(!result.message.contains("private"));
    response.status=0;response.timedOut=true;result=adapter.parse(Operation::QueryOrder,response,okxInput());QVERIFY(!result.ok);QVERIFY(!result.unknown);
    result=adapter.parse(Operation::QueryOrder,okxJson({},"51603"),okxInput());QVERIFY(result.notFound);QVERIFY(!result.ok);
}

QTEST_GUILESS_MAIN(OkxTest)
#include "tst_okx.moc"
