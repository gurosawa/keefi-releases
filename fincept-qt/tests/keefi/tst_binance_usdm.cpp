#include "keefi/BuildPolicy.h"
#include "keefi/BinanceUsdmAdapter.h"
#include "keefi/TradingTransport.h"
#include "fixtures/BinanceUsdmResponses.h"
#include <QMessageAuthenticationCode>
#include <QTest>
#include <QUrlQuery>
#include <sodium.h>
using namespace keefi;
using namespace keefi::test;
namespace {
CredentialSet keys() { CredentialSet k; k.emplace("api_key",SecureBuffer::fromUtf8("FAKEAPIKEY123")); k.emplace("secret",SecureBuffer::fromUtf8("FAKESECRET456")); return k; }
OrderLimits limits() { return {"1000","USDT","5"}; }
MarketSnapshot ready(BinanceUsdmAdapter& x,OrderInput i=usdmInput(),AccountRecord a=usdmAccount()) {
    MarketSnapshot s; s.serverNowMs = QDateTime::currentMSecsSinceEpoch(); auto k = keys(); QString e;
    for (auto op : {Operation::AccountConfig,Operation::ApiRestrictions,Operation::Rules,Operation::SymbolConfig,Operation::LeverageBrackets,Operation::Commissions,
        Operation::TradingStatus,Operation::Account,Operation::Positions,Operation::OpenOrders,Operation::AlgoOpenOrders,Operation::MarkPrice,Operation::Quote,Operation::Depth}) {
        const auto req = x.build(op,a,i,k,s.serverNowMs,&e); const auto r = x.parse(op,usdmResponse(req,s.serverNowMs),i);
        if (!e.isEmpty() || !r.ok) { const auto message = QString("fake operation=%1 buildFailed=%2 code=%3").arg(operationName(op)).arg(!e.isEmpty()).arg(r.code);
            QTest::qFail(qPrintable(message),__FILE__,__LINE__); return {}; }
        s.data[op] = r.data; if (op == Operation::Account) s.balances = r.balances; if (op == Operation::Positions) s.positions = r.positions;
        if (op == Operation::OpenOrders || op == Operation::AlgoOpenOrders) s.openOrders.append(r.orders);
    } return s;
}
OrderInput algoInput() { OrderInput i; i.symbol="BTCUSDT";i.side="sell";i.type="stop_market";i.quantity="0";i.price="0";i.algoId="100";i.clientAlgoId="fake-conditional-client";
    FuturesOrderTerms f;f.family=OrderFamily::Conditional;f.positionSide="BOTH";f.timeInForce="GTC";f.workingType="MARK_PRICE";f.triggerPrice="90";f.closePosition=true;i.futures=f;return i; }
}
class BinanceUsdmTest final : public QObject {
    Q_OBJECT
private slots:
    void historicalAmendmentsAndReadErrorsRemainAvailable();
    void separateAlgoIdentityAndLifecycle();
    void signatureWireAndEnvironment();
    void initTestCase(){QVERIFY(sodium_init()>=0);}

    void signedBalancesPositionsAndUnknownMode();
    void exactFiltersMarginAndExposure();

    void readCompletenessAndSourceTime();

    void deliverySymbolsDoNotBreakTheWholeCatalog();
};
void BinanceUsdmTest::deliverySymbolsDoNotBreakTheWholeCatalog() {
    BinanceUsdmAdapter x(usdmAccount());
    auto delivery=usdmInstrument("BTCUSDT_261225");delivery["contractType"]="CURRENT_QUARTER";
    HttpResult response=usdmJson(QJsonObject{{"symbols",QJsonArray{usdmInstrument(),delivery}}});
    auto result=x.parse(Operation::Markets,response,{});QVERIFY2(result.ok,qPrintable(result.code));
    QCOMPARE(result.data.value("symbols").toArray().size(),2);
    auto input=usdmInput();input.symbol="BTCUSDT_261225";QString error;
    const auto request=x.build(Operation::Quote,usdmAccount(),input,{},0,&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(QUrlQuery(request.url).queryItemValue("symbol"),input.symbol);
    for(const auto& invalid:QStringList{"BTCUSDT_261225&symbol=ETHUSDT","BTCUSDT_261225\n","BTCUSDT__261225","BTCUSDT_26/225"}){
        input.symbol=invalid;x.build(Operation::Quote,usdmAccount(),input,{},0,&error);QVERIFY(!error.isEmpty());
    }
    delivery["symbol"]="BTCUSDT_261225&symbol=ETHUSDT";
    response=usdmJson(QJsonObject{{"symbols",QJsonArray{usdmInstrument(),delivery}}});
    QVERIFY(!x.parse(Operation::Markets,response,{}).ok);
}



void BinanceUsdmTest::signedBalancesPositionsAndUnknownMode() {
    BinanceUsdmAdapter x(usdmAccount());auto s=ready(x);QVERIFY(!s.balances.isEmpty());QVERIFY(!s.positions.isEmpty());QCOMPARE(s.balances[0].section,QString("futures"));QVERIFY(s.balances[0].locked.isEmpty());
    QCOMPARE(s.positions[0].unrealizedPnl,QString("-1"));QVERIFY(s.positions[0].marginType.isEmpty());QVERIFY(s.positions[0].leverage.isEmpty());
    auto r=x.parse(Operation::Positions,usdmJson(QJsonArray{usdmPosition("-2")}),{});QVERIFY(r.ok);QCOMPARE(r.positions[0].amount,QString("-2"));QCOMPARE(r.positions[0].notional,QString("-200"));
    auto bad=usdmPosition();bad["positionAmt"]="garbage-2";QVERIFY(!x.parse(Operation::Positions,usdmJson(QJsonArray{bad}),{}).ok);
    s.data[Operation::AccountConfig]["multiAssetsMargin"]=true;QVERIFY(!x.validate(usdmInput(),s,limits()).ok); // Reading remains successful.
    auto config=QJsonObject{{"canTrade",true},{"dualSidePosition",true},{"multiAssetsMargin",true},{"uid","PRIVATE_UID"}};
    r=x.parse(Operation::AccountConfig,usdmJson(config),{});QVERIFY(r.ok);QVERIFY(!r.data.contains("uid"));
    BinanceUsdmAdapter live(usdmAccount("live"));s=ready(live,usdmInput(),usdmAccount("live"));s.data.remove(Operation::ApiRestrictions);QVERIFY(!live.validate(usdmInput(),s,limits()).ok);
    s.data[Operation::ApiRestrictions]={{"enableFutures",true}};QVERIFY2(live.validate(usdmInput(),s,limits()).ok,qPrintable(live.validate(usdmInput(),s,limits()).message));
}
void BinanceUsdmTest::exactFiltersMarginAndExposure() {
    BinanceUsdmAdapter x(usdmAccount());auto s=ready(x);auto i=usdmInput();auto v=x.validate(i,s,limits());QVERIFY2(v.ok,qPrintable(v.message));QCOMPARE(v.notional,QString("100"));
    i.price="100.01";QVERIFY(!x.validate(i,s,limits()).ok);i=usdmInput();i.quantity="0.0001";QVERIFY(!x.validate(i,s,limits()).ok);
    i=usdmInput();s.balances[0].free="10";QVERIFY(!x.validate(i,s,limits()).ok); // 10 margin + nonzero fees, never a zero-fee assumption.
    i.side="sell";i.futures->exposure=ExposureIntent::Reduce;i.futures->reduceOnly=true;QVERIFY2(x.validate(i,s,limits()).ok,qPrintable(x.validate(i,s,limits()).message));
    i.quantity="3";QVERIFY(!x.validate(i,s,limits()).ok);i.quantity="1";i.futures->reduceOnly=false;QVERIFY(!x.validate(i,s,limits()).ok);
    s=ready(x);s.data[Operation::AccountConfig]["dualSidePosition"]=true;s.positions[0].positionSide="LONG";
    i.futures->positionSide="LONG";QVERIFY2(x.validate(i,s,limits()).ok,qPrintable(x.validate(i,s,limits()).message));i.side="buy";QVERIFY(!x.validate(i,s,limits()).ok);
    s=ready(x);auto rows=s.data[Operation::LeverageBrackets].value("rows").toArray();auto bracket=rows[0].toObject();bracket["notionalCoef"]="2";rows[0]=bracket;s.data[Operation::LeverageBrackets]["rows"]=rows;
    QVERIFY(!x.validate(usdmInput(),s,limits()).ok);
}


void BinanceUsdmTest::readCompletenessAndSourceTime() {
    BinanceUsdmAdapter x(usdmAccount());auto i=usdmInput();QJsonArray rows;
    for(int n=1;n<=1000;++n)rows.append(usdmOrder(QString::number(n),"NEW",i));
    auto r=x.parse(Operation::History,usdmJson(rows),i);QVERIFY(!r.ok);QCOMPARE(r.code,QString("HISTORY_LIMIT"));QCOMPARE(r.orders.size(),1000);
    auto k=keys();QString e;i.startTimeMs=i.endTimeMs-7LL*86400000;x.build(Operation::History,usdmAccount(),i,k,i.endTimeMs,&e);QVERIFY(!e.isEmpty());
    auto s=ready(x);i=usdmInput();QVERIFY(x.validate(i,s,limits()).ok);s.serverNowMs+=20000;QVERIFY(!x.validate(i,s,limits()).ok);
    s=ready(x);s.data[Operation::AlgoOpenOrders].remove("complete");QVERIFY(!x.validate(i,s,limits()).ok);
    auto o=usdmOrder();o["orderId"]="9007199254740993";r=x.parse(Operation::OpenOrders,usdmJson(QJsonArray{o}),{});QVERIFY(r.ok);QCOMPARE(r.orders[0].orderId,QString("9007199254740993"));
    auto body=usdmJson(QJsonArray{o}).body;body.replace("\"9007199254740993\"","9007199254740993");HttpResult h;h.status=200;h.body=body;r=x.parse(Operation::OpenOrders,h,{});QVERIFY(r.ok);QCOMPARE(r.orders[0].orderId,QString("9007199254740993"));
}

void BinanceUsdmTest::signatureWireAndEnvironment() {
    BinanceUsdmAdapter x(usdmAccount()); auto k=keys(); QString error; auto i=usdmInput();
    auto r=x.build(Operation::Account,usdmAccount(),{},k,1700000000123LL,&error);QVERIFY2(error.isEmpty(),qPrintable(error));
    QCOMPARE(r.url.host(),QString("fapi.binance.com"));QCOMPARE(r.method,QByteArray("GET"));QVERIFY(r.body.isEmpty());
    const auto query=r.url.query(QUrl::FullyEncoded).toUtf8();const auto split=query.lastIndexOf("&signature=");QVERIFY(split>0);
    QCOMPARE(query.mid(split+11),QMessageAuthenticationCode::hash(query.left(split),"FAKESECRET456",QCryptographicHash::Sha256).toHex());
    QCOMPARE(r.headers.value("X-MBX-APIKEY"),QByteArray("FAKEAPIKEY123"));
    i.symbol=QString::fromUtf8("币USDT");r=x.build(Operation::Quote,usdmAccount(),i,{},0,&error);QVERIFY(error.isEmpty());QVERIFY(!r.authenticated);QVERIFY(r.headers.isEmpty());QVERIFY(r.url.toEncoded().contains("%E5%B8%81"));
    i.symbol="BTCUSDT\n";x.build(Operation::Quote,usdmAccount(),i,{},0,&error);QVERIFY(!error.isEmpty());
    x.build(Operation::ApiRestrictions,usdmAccount(),{},k,1700000000123LL,&error);QVERIFY(error.isEmpty());
    BinanceUsdmAdapter retired(usdmAccount("demo"));r=retired.build(Operation::Account,usdmAccount("demo"),{},k,1700000000123LL,&error);QVERIFY(r.url.isEmpty());QVERIFY(!error.isEmpty());
    BinanceUsdmAdapter live(usdmAccount("live"));r=live.build(Operation::ApiRestrictions,usdmAccount("live"),{},k,1700000000123LL,&error);QVERIFY(error.isEmpty());QCOMPARE(r.url.host(),QString("api.binance.com"));
    r=x.build(Operation::Markets,usdmAccount(),{}, {},0,&error);QVERIFY(error.isEmpty());QVERIFY(r.url.query().isEmpty());QVERIFY(!r.authenticated);
}

void BinanceUsdmTest::separateAlgoIdentityAndLifecycle() {
    BinanceUsdmAdapter x(usdmAccount());auto ordinary=x.parse(Operation::OpenOrders,usdmJson(QJsonArray{usdmOrder()}),usdmInput());
    auto algos=x.parse(Operation::AlgoOpenOrders,usdmJson(QJsonArray{usdmAlgo()}),usdmInput());QVERIFY(ordinary.ok);QVERIFY(algos.ok);QCOMPARE(ordinary.orders.size(),1);QCOMPARE(algos.orders.size(),1);
    QCOMPARE(ordinary.orders[0].orderId,algos.orders[0].algoId);QVERIFY(ordinary.orders[0].algoId.isEmpty());QVERIFY(algos.orders[0].orderId.isEmpty());QVERIFY(algos.orders[0].cancelable);
    auto a=usdmAlgo("100","FINISHED");a["actualOrderId"]="500";auto r=x.parse(Operation::AlgoQueryOrder,usdmJson(a),algoInput());QVERIFY(r.ok);QCOMPARE(r.orders[0].status,QString("unknown"));QVERIFY(!r.orders[0].cancelable);QCOMPARE(r.orders[0].actualOrderId,QString("500"));
    auto trailing=usdmAlgo();trailing["orderType"]="TRAILING_STOP_MARKET";trailing["quantity"]="1";trailing["closePosition"]=false;trailing["triggerPrice"]="0";trailing["activatePrice"]="110";trailing["callbackRate"]="0.3";
    r=x.parse(Operation::AlgoOpenOrders,usdmJson(QJsonArray{trailing}),{});QVERIFY(r.ok);QVERIFY(r.orders[0].cancelable);QCOMPARE(r.orders[0].futures->callbackRate,QString("0.3"));
    a=usdmAlgo();a["orderType"]="UNKNOWN_NEW_TYPE";r=x.parse(Operation::AlgoOpenOrders,usdmJson(QJsonArray{a}),{});QVERIFY(r.ok);QVERIFY(!r.orders[0].cancelable);
}

void BinanceUsdmTest::historicalAmendmentsAndReadErrorsRemainAvailable() {
    BinanceUsdmAdapter adapter(usdmAccount()); auto input=usdmInput(); input.orderId="100"; input.futures->modifyId="42";
    QJsonObject amendment{{"modifyId","42"},{"price",QJsonObject{{"before","100"},{"after","100.1"}}},{"origQty",QJsonObject{{"before","3"},{"after","3"}}}};
    QJsonObject row{{"orderId","100"},{"time","1700000000123"},{"amendment",amendment}};
    auto result=adapter.parse(Operation::AmendHistory,usdmJson(QJsonArray{row}),input);QVERIFY(result.ok);
    QCOMPARE(result.data.value("rows").toArray()[0].toObject().value("amendment").toObject().value("modifyId").toString(),QString("42"));
    result=adapter.parse(Operation::QueryOrder,usdmJson(QJsonObject{{"code","-2013"},{"msg","PRIVATE_RAW"}},400),input);
    QVERIFY(result.notFound);QVERIFY(!result.ok);QVERIFY(!result.message.contains("PRIVATE_RAW"));
}

QTEST_GUILESS_MAIN(BinanceUsdmTest)
#include "tst_binance_usdm.moc"
