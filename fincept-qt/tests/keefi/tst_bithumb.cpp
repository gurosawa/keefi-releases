#include "keefi/BithumbAdapter.h"
#include "fixtures/BithumbResponses.h"
#include <QCryptographicHash>
#include <QTest>
#include <QUrlQuery>
#include <sodium.h>
using namespace keefi;
using namespace keefi::test;
namespace {
CredentialSet keys(){CredentialSet k;k.emplace("api_key",SecureBuffer::fromUtf8("fake-bithumb-key"));k.emplace("secret",SecureBuffer::fromUtf8("fake-bithumb-secret"));return k;}
QJsonObject jwtPayload(const RequestSpec& r){const auto jwt=r.headers.value("Authorization").mid(7).split('.');
    return jwt.size()==3?QJsonDocument::fromJson(QByteArray::fromBase64(jwt[1],QByteArray::Base64UrlEncoding)).object():QJsonObject{};}
MarketSnapshot ready(BithumbAdapter& x,const OrderInput& i=bithumbInput()){
    MarketSnapshot s;auto k=keys();QString e;s.serverNowMs=QDateTime::currentMSecsSinceEpoch();
    for(auto op:{Operation::Rules,Operation::Account,Operation::Quote,Operation::Depth}){
        const auto req=x.build(op,bithumbAccount(),i,k,s.serverNowMs,&e);const auto r=x.parse(op,bithumbResponse(req,s.serverNowMs),i);
        if(!e.isEmpty()||!r.ok){auto d=QString("fixture operation=%1 buildFailed=%2 code=%3").arg(operationName(op)).arg(!e.isEmpty()).arg(r.code);
            QTest::qFail(qPrintable(d),__FILE__,__LINE__);return {};}
        s.data[op]=r.data;if(op==Operation::Account)s.balances=r.balances;
    }return s;
}
OrderLimits limits(){return {"1000000","KRW","5"};}
}
class BithumbTest final:public QObject{
    Q_OBJECT
private slots:
    void canceledOrderQueriesKeepStatusAndMissingIdentity();
    void quoteDepthBalanceAndRules();
    void initTestCase(){QVERIFY(sodium_init()>=0);}
    void signingVectorNonceAndPublicIsolation();
    void rawHashEncodedCursorAndWindowBinding();
    void pendingStreamsAndIncompletePages();

    void accountAveragePriceIsOptionalDisplayOnly();
    void optionalAveragePriceRejectsTrailingNewline();
    void nullableMarketAndTerminalSemantics();

    void exactMoneyFeeTickAndDepth();
};
void BithumbTest::signingVectorNonceAndPublicIsolation(){
    BithumbAdapter x([]{return QString("00000000-0000-4000-8000-000000000001");});auto k=keys();QString e;
    auto r=x.build(Operation::Rules,bithumbAccount(),bithumbInput(),k,1700000000123LL,&e);QVERIFY2(e.isEmpty(),qPrintable(e));
    const QByteArray expected="Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhY2Nlc3Nfa2V5IjoiZmFrZS1iaXRodW1iLWtleSIsIm5vbmNlIjoiMDAwMDAwMDAtMDAwMC00MDAwLTgwMDAtMDAwMDAwMDAwMDAxIiwidGltZXN0YW1wIjoxNzAwMDAwMDAwMTIzLCJxdWVyeV9oYXNoIjoiYjc0OWRmYzJlMTdmNzVlNWI0NmM4MTYxZjk3ZmU3YzkyOThlZDQxNjdlYTIxYzVjOTRkMTY1NzNlZmQ4YTgwMTM1MTQ3MGMwZmYxYTlhM2YxZTc2M2Y4MjQ5OTY4MjE4YzA0YzU3MWM4YjQ1YWE4MGNkNDU4OGU2YzRiZTA3MzgiLCJxdWVyeV9oYXNoX2FsZyI6IlNIQTUxMiJ9.lgJcicd487hHnJSB66KIoGeUTP7ZdInSQHQplezOOm8";
    QCOMPARE(r.headers.value("Authorization"),expected);QVERIFY(jwtPayload(r).value("timestamp").isDouble());
    auto p=x.build(Operation::Markets,bithumbAccount(),{}, {},0,&e);QVERIFY(e.isEmpty());QVERIFY(!p.authenticated);QVERIFY(!p.headers.contains("Authorization"));
    BithumbAdapter random;auto a=random.build(Operation::Account,bithumbAccount(),{},k,1700000000123LL,&e);
    auto b=random.build(Operation::Account,bithumbAccount(),{},k,1700000000123LL,&e);QVERIFY(jwtPayload(a).value("nonce")!=jwtPayload(b).value("nonce"));
    QVERIFY(!jwtPayload(a).contains("query_hash"));auto wrong=bithumbAccount();wrong.environment="demo";x.build(Operation::Account,wrong,{},k,1700000000123LL,&e);QVERIFY(!e.isEmpty());
}
void BithumbTest::rawHashEncodedCursorAndWindowBinding(){
    BithumbAdapter x;auto i=bithumbInput();auto page=x.parse(Operation::OpenOrders,bithumbPage({bithumbOrder("id-1","wait",true)},true,"ab+/=&z"),i);
    QVERIFY(page.ok);QVERIFY(page.hasMore);i.cursor=page.nextCursor;auto k=keys();QString e;
    auto req=x.build(Operation::OpenOrders,bithumbAccount(),i,k,1700000000123LL,&e);QVERIFY(e.isEmpty());
    QVERIFY(req.url.toEncoded().contains("next_key=ab%2B%2F%3D%26z"));QVERIFY(!QUrlQuery(req.url).hasQueryItem("market"));
    QCOMPARE(jwtPayload(req).value("query_hash").toString(),QString::fromLatin1(QCryptographicHash::hash("state=wait&limit=100&order_by=desc&next_key=ab+/=&z",QCryptographicHash::Sha512).toHex()));
    auto hist=bithumbInput();page=x.parse(Operation::History,bithumbPage({bithumbOrder("id-2","done",true,hist)},true,"next+key"),hist);QVERIFY(page.ok);
    hist.cursor=page.nextCursor;hist.endTimeMs++;x.build(Operation::History,bithumbAccount(),hist,k,1700000000123LL,&e);QVERIFY(!e.isEmpty());
    hist=bithumbInput();hist.startTimeMs=hist.endTimeMs-7LL*86400000-1;x.build(Operation::History,bithumbAccount(),hist,k,1700000000123LL,&e);QVERIFY(!e.isEmpty());
}
void BithumbTest::pendingStreamsAndIncompletePages(){
    BithumbAdapter x;auto i=bithumbInput();auto r=x.parse(Operation::OpenOrders,bithumbPage({}),i);QVERIFY(r.ok);QVERIFY(r.hasMore);
    i.cursor=r.nextCursor;auto k=keys();QString e;const auto req=x.build(Operation::OpenOrders,bithumbAccount(),i,k,1700000000123LL,&e);
    QVERIFY(e.isEmpty());QCOMPARE(QUrlQuery(req.url).queryItemValue("state"),QString("watch"));
    r=x.parse(Operation::OpenOrders,bithumbPage({bithumbOrder("watch-1","watch",true)}),i);QVERIFY(r.ok);QVERIFY(!r.hasMore);QVERIFY(r.data.value("scopeComplete").toBool());
    QCOMPARE(r.orders.size(),1);QCOMPARE(r.orders[0].status,QString("watch"));QVERIFY(!r.orders[0].cancelable);
    QVERIFY(!x.parse(Operation::OpenOrders,bithumbJson(QJsonObject{{"data",QJsonArray{}},{"has_next",false}}),i).ok);
    QVERIFY(!x.parse(Operation::OpenOrders,bithumbPage({},true,"x"),i).ok);QVERIFY(!x.parse(Operation::OpenOrders,bithumbPage({},false,""),i).ok);
    i=bithumbInput();r=x.parse(Operation::OpenOrders,bithumbPage({bithumbOrder("id-1","wait",true)},true,"same"),i);QVERIFY(r.ok);
    i.cursor=r.nextCursor;QVERIFY(!x.parse(Operation::OpenOrders,bithumbPage({bithumbOrder("id-2","wait",true)},true,"same"),i).ok);
}


void BithumbTest::accountAveragePriceIsOptionalDisplayOnly(){
    // Synthetic account response; no private profile or authenticated read.
    const QJsonObject original{{"currency","BTC"},{"balance","2.5"},{"locked","0.25"},
        {"avg_buy_price","00123.4500"},{"unit_currency","KRW"}};
    const auto parse=[](const QJsonObject& row){return BithumbAdapter().parse(Operation::Account,bithumbJson(QJsonArray{row}),{});};
    const auto reported=parse(original);QVERIFY(reported.ok);QCOMPARE(reported.balances.size(),1);
    QCOMPARE(reported.balances.first().providerAveragePrice,QString("123.45"));
    QCOMPARE(reported.balances.first().providerAverageCurrency,QString("KRW"));QVERIFY(!reported.tradingPermissionKnown);
    const QList<QPair<QJsonValue,QJsonValue>> unavailable{
        {QJsonValue::Undefined,"KRW"},{QJsonValue::Null,"KRW"},{"","KRW"},
        {"0","KRW"},{"-12","KRW"},{"NaN","KRW"},{true,"KRW"},
        {"123.45",QJsonValue::Undefined},{"123.45",QJsonValue::Null},
        {"123.45","krw"},{"123.45","KRW\n"},{"123.45",true}};
    for(const auto& [price,currency]:unavailable){auto row=original;row.insert("avg_buy_price",price);row.insert("unit_currency",currency);
        const auto result=parse(row);QVERIFY(result.ok);QCOMPARE(result.balances.size(),1);const auto balance=result.balances.first();
        QCOMPARE(balance.free,QString("2.5"));QCOMPARE(balance.locked,QString("0.25"));
        QVERIFY(balance.providerAveragePrice.isEmpty());QVERIFY(balance.providerAverageCurrency.isEmpty());}
}
void BithumbTest::optionalAveragePriceRejectsTrailingNewline(){
    const QJsonObject row{{"currency","BTC"},{"balance","2.5"},{"locked","0.25"},
        {"avg_buy_price","123.45\n"},{"unit_currency","KRW"}};
    const auto result=BithumbAdapter().parse(Operation::Account,bithumbJson(QJsonArray{row}),{});
    QVERIFY(result.ok);QCOMPARE(result.balances.size(),1);
    QVERIFY(result.balances.first().providerAveragePrice.isEmpty());
    QVERIFY(result.balances.first().providerAverageCurrency.isEmpty());
    QCOMPARE(result.balances.first().free,QString("2.5"));
}
void BithumbTest::nullableMarketAndTerminalSemantics(){
    BithumbAdapter x;auto i=bithumbInput();i.orderId="id-1";
    auto o=bithumbOrder(i.orderId,"done",false,i);o["volume"]="0.1";o["executed_volume"]="0.05";o["remaining_volume"]="0";o["time_in_force"]="ioc";
    auto r=x.parse(Operation::QueryOrder,bithumbJson(o),i);QVERIFY(r.ok);QCOMPARE(r.orders[0].status,QString("done"));QVERIFY(!r.orders[0].cancelable);
    o.remove("time_in_force");o["ord_type"]="price";o["price"]="10000";o["volume"]=QJsonValue::Null;o["remaining_volume"]=QJsonValue::Null;
    r=x.parse(Operation::QueryOrder,bithumbJson(o),i);QVERIFY(r.ok);const auto row=r.orders[0];QCOMPARE(row.quantityUnit,QString("quote"));QCOMPARE(row.requestedQuoteAmount,QString("10000"));
    QCOMPARE(row.executedBaseAmount,QString("0.05"));QVERIFY(row.quantity.isEmpty());QVERIFY(row.remaining.isEmpty());QCOMPARE(row.status,QString("done"));
    o["state"]="wait";o["price"]=QJsonValue::Null;r=x.parse(Operation::QueryOrder,bithumbJson(o),i);QVERIFY(r.ok);QVERIFY(r.orders[0].requestedQuoteAmount.isEmpty());QVERIFY(!r.orders[0].cancelable);
    o["executed_volume"]="garbage1";QVERIFY(!x.parse(Operation::QueryOrder,bithumbJson(o),i).ok);
}

void BithumbTest::exactMoneyFeeTickAndDepth(){
    BithumbAdapter x;auto s=ready(x);QVERIFY(!s.balances.isEmpty());auto i=bithumbInput();auto v=x.validate(i,s,limits());QVERIFY2(v.ok,qPrintable(v.message));
    i.price="100001";QVERIFY(!x.validate(i,s,limits()).ok);i=bithumbInput();i.quantity="0.000000001";QVERIFY(!x.validate(i,s,limits()).ok);
    i=bithumbInput();s.balances[0].free="10000";QVERIFY(!x.validate(i,s,limits()).ok);s.balances[0].free="10005";QVERIFY(x.validate(i,s,limits()).ok);
    i.type="market";i.price.clear();i.quantity.clear();i.quoteAmount="10000";v=x.validate(i,s,limits());QVERIFY2(v.ok,qPrintable(v.message));QCOMPARE(v.notional,QString("10000"));
    i.side="sell";i.quoteAmount.clear();i.quantity="0.1";v=x.validate(i,s,limits());QVERIFY2(v.ok,qPrintable(v.message));QCOMPARE(decimal::normalize(v.notional),QString("10000"));
    s.data[Operation::Depth]["bids"]=QJsonArray{QJsonValue(QJsonArray{"100000","0.01"})};QVERIFY(!x.validate(i,s,limits()).ok);
    s=ready(x);s.data[Operation::Rules]["sellFee"]=QJsonValue::Null;QVERIFY(!x.validate(i,s,limits()).ok);
}
void BithumbTest::quoteDepthBalanceAndRules(){
    BithumbAdapter x;auto s=ready(x);QCOMPARE(s.balances.size(),2);QCOMPARE(s.balances[0].section,QString("spot"));
    QCOMPARE(s.data[Operation::Quote].value("price").toString(),QString("100000"));QVERIFY(s.data[Operation::Quote].value("timestamp")!=s.data[Operation::Quote].value("tradeTimestamp"));
    const auto bids=s.data[Operation::Depth].value("bids").toArray();QCOMPARE(bids.size(),2);QCOMPARE(bids[0].toArray().size(),2);
    QVERIFY(s.data[Operation::Rules].value("buyPriceUnit").isNull());
    auto r=x.parse(Operation::Rules,bithumbJson(QJsonObject{{"error",QJsonObject{{"name","out_of_scope"},{"message","PRIVATE"}}}},401),bithumbInput());
    QVERIFY(!r.ok);QVERIFY(!r.authenticationFailed);QVERIFY(!r.message.contains("PRIVATE"));
    HttpResult h;h.status=401;h.body="<html>PRIVATE</html>";r=x.parse(Operation::Account,h,bithumbInput());QVERIFY(r.authenticationFailed);QVERIFY(!r.unknown);
}
void BithumbTest::canceledOrderQueriesKeepStatusAndMissingIdentity() {
    BithumbAdapter adapter;auto input=bithumbInput();input.orderId="cancel-target";
    auto result=adapter.parse(Operation::QueryOrder,bithumbJson(bithumbOrder(input.orderId,"cancel",false,input)),input);
    QVERIFY(result.ok);QCOMPARE(result.orders[0].status,QString("canceled"));
    result=adapter.parse(Operation::QueryOrder,bithumbJson(QJsonObject{{"error",QJsonObject{{"name","order_not_found"}}}},404),input);
    QVERIFY(result.notFound);QVERIFY(!result.ok);
}

QTEST_GUILESS_MAIN(BithumbTest)
#include "tst_bithumb.moc"
