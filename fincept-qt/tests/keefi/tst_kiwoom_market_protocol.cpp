// All codes, values, dates and payloads below are synthetic.
#include "keefi/KiwoomMarketProtocol.h"
#include "keefi/TradingTransport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
using namespace keefi;
namespace {
HttpResult response(QJsonObject body,const QByteArray& api,int status=200) {
    body.insert("return_code",0);HttpResult r;r.status=status;r.headers={{"api-id",api},{"cont-yn","N"}};
    r.body=QJsonDocument(body).toJson(QJsonDocument::Compact);return r;
}
KiwoomReadIntent intent(KiwoomDisplayRequest read,QString country,QString symbol={},QString exchange={},QString market={}) {
    KiwoomReadIntent i;i.read=read;i.country=country;i.symbol=symbol;i.exchange=exchange;i.catalogMarket=market;return i;
}
KiwoomVerifiedInstrument usInstrument(qint64 at=1792710000000LL) {
    KiwoomVerifiedInstrument i;i.identity={"stock","US","SYNTH"};i.exchange="ND";i.currency="USD";
    i.metadataApiId="usa10098";i.currencyApiId="usa20100";i.verified=true;
    i.metadataEvidence={"Kiwoom public metadata","https://api.kiwoom.com/api/us/stkinfo",{},0,at,true};
    i.currencyEvidence={"Kiwoom USD proof","https://api.kiwoom.com/api/us/mrkcond",{},0,at,true};return i;
}
QByteArray realtime(QString price="73.2500") {
    return QJsonDocument(QJsonObject{{"trnm","REAL"},{"data",QJsonArray{QJsonObject{{"type","FE"},{"item","SYNTH"},{"stexTp","ND"},
        {"values",QJsonObject{{"10",price},{"12","-1.25"},{"20","103015"},{"22","20261023"},{"51020","213015"}}}}}}}).toJson(QJsonDocument::Compact);
}
}
class KiwoomMarketProtocolTest final:public QObject {
    Q_OBJECT
  private slots:
    void dailyChartRequestHasOneExactAdjustedShape();
    void dailyChartParsesSignedPricesStrictly_data();
    void dailyChartParsesSignedPricesStrictly();
    void sixReferenceIntentsHaveExactShapes_data();
    void sixReferenceIntentsHaveExactShapes();
    void nativeGuardAcceptsOnlyActualAuthenticatedProducerShapes_data();
    void nativeGuardAcceptsOnlyActualAuthenticatedProducerShapes();
    void manualUsPriceReferenceExposesNativeGuardGapIndependently();
    void cursorIsOnlyABoundedContinuationHeader();
    void publicMetadataAndUsdProofKeepSeparateOriginalReceipts();
    void exactCountryVenueAndCurrencyAreNotGuessed_data();
    void exactCountryVenueAndCurrencyAreNotGuessed();
    void usRegistrationKeepsExactVenueAndOneType();
    void mixedCountryAnd201RegistrationFailAtomically();
    void acknowledgementsNeedPresentExactSuccessAndPingEchoIsOriginal();
    void usRealtimeKeepsUnknownUtcClockAndUndocumentedChangeHidden();
};
void KiwoomMarketProtocolTest::sixReferenceIntentsHaveExactShapes_data() {
    QTest::addColumn<int>("kind");QTest::addColumn<QString>("country");QTest::addColumn<QString>("symbol");QTest::addColumn<QString>("exchange");
    QTest::addColumn<QString>("market");QTest::addColumn<QByteArray>("api");QTest::addColumn<QString>("path");QTest::addColumn<QByteArray>("body");
    QTest::newRow("KR-catalog")<<int(KiwoomDisplayRequest::KrCatalog)<<QString("KR")<<QString()<<QString()<<QString("0")<<QByteArray("ka10099")<<QString("/api/dostk/stkinfo")<<QByteArray(R"({"mrkt_tp":"0"})");
    QTest::newRow("KR-metadata")<<int(KiwoomDisplayRequest::KrMetadata)<<QString("KR")<<QString("0101N0")<<QString("KRX")<<QString()<<QByteArray("ka10100")<<QString("/api/dostk/stkinfo")<<QByteArray(R"({"stk_cd":"0101N0"})");
    QTest::newRow("KR-price")<<int(KiwoomDisplayRequest::KrPrice)<<QString("KR")<<QString("0101N0")<<QString("KRX")<<QString()<<QByteArray("ka10001")<<QString("/api/dostk/stkinfo")<<QByteArray(R"({"stk_cd":"0101N0"})");
    QTest::newRow("US-catalog")<<int(KiwoomDisplayRequest::UsCatalog)<<QString("US")<<QString()<<QString("%")<<QString()<<QByteArray("usa10099")<<QString("/api/us/stkinfo")<<QByteArray(R"({"stex_tp":"%"})");
    QTest::newRow("US-metadata")<<int(KiwoomDisplayRequest::UsMetadata)<<QString("US")<<QString("SYNTH")<<QString()<<QString()<<QByteArray("usa10098")<<QString("/api/us/stkinfo")<<QByteArray(R"({"stk_cd":"SYNTH"})");
    QTest::newRow("US-price")<<int(KiwoomDisplayRequest::UsPrice)<<QString("US")<<QString("SYNTH")<<QString("ND")<<QString()<<QByteArray("usa20100")<<QString("/api/us/mrkcond")<<QByteArray(R"({"stex_tp":"ND","stk_cd":"SYNTH"})");
}
void KiwoomMarketProtocolTest::sixReferenceIntentsHaveExactShapes() {
    QFETCH(int,kind);QFETCH(QString,country);QFETCH(QString,symbol);QFETCH(QString,exchange);QFETCH(QString,market);QFETCH(QByteArray,api);QFETCH(QString,path);QFETCH(QByteArray,body);
    auto r=KiwoomMarketProtocol::request(intent(static_cast<KiwoomDisplayRequest>(kind),country,symbol,exchange,market));
    QVERIFY(KiwoomMarketProtocol::isRequest(r));QCOMPARE(r.operation,Operation::Reference);QCOMPARE(r.method,QByteArray("POST"));
    QCOMPARE(r.url,QUrl("https://api.kiwoom.com"+path));QVERIFY(r.authenticated);QCOMPARE(r.headers.value("api-id"),api);
    QVERIFY(!r.headers.contains("authorization"));QVERIFY(!r.headers.contains("Authorization"));QCOMPARE(QJsonDocument::fromJson(r.body).object(),QJsonDocument::fromJson(body).object());
    r.url.setPath("/api/dostk/ordr");QVERIFY(!KiwoomMarketProtocol::isRequest(r));
    r=KiwoomMarketProtocol::request(intent(static_cast<KiwoomDisplayRequest>(kind),country,symbol,exchange,market));r.headers["api-id"]="kt10000";QVERIFY(!KiwoomMarketProtocol::isRequest(r));
    r=KiwoomMarketProtocol::request(intent(static_cast<KiwoomDisplayRequest>(kind),country,symbol,exchange,market));auto extra=QJsonDocument::fromJson(r.body).object();extra["account_number"]="SYNTHETIC_PRIVATE_CANARY";r.body=QJsonDocument(extra).toJson();QVERIFY(!KiwoomMarketProtocol::isRequest(r));
}
void KiwoomMarketProtocolTest::nativeGuardAcceptsOnlyActualAuthenticatedProducerShapes_data() { sixReferenceIntentsHaveExactShapes_data(); }
void KiwoomMarketProtocolTest::nativeGuardAcceptsOnlyActualAuthenticatedProducerShapes() {
    QFETCH(int,kind);QFETCH(QString,country);QFETCH(QString,symbol);QFETCH(QString,exchange);QFETCH(QString,market);
    auto r=KiwoomMarketProtocol::request(intent(static_cast<KiwoomDisplayRequest>(kind),country,symbol,exchange,market));QVERIFY(KiwoomMarketProtocol::isRequest(r));
    QVERIFY(!NetworkTradingTransport::allowed(r));r.headers["authorization"]="Bearer SYNTHETIC_BORROWED_TOKEN";QVERIFY(NetworkTradingTransport::allowed(r));
    r.headers["api-id"]="kt10000";QVERIFY(!NetworkTradingTransport::allowed(r));r.headers["api-id"]="kt00018";QVERIFY(!NetworkTradingTransport::allowed(r));
}
void KiwoomMarketProtocolTest::manualUsPriceReferenceExposesNativeGuardGapIndependently() {
    // Deliberately independent of the still-nonworking Protocol factory.
    RequestSpec r;r.operation=Operation::Reference;r.method="POST";r.url=QUrl("https://api.kiwoom.com/api/us/mrkcond");r.authenticated=true;
    r.headers={{"Content-Type","application/json;charset=UTF-8"},{"api-id","usa20100"},{"authorization","Bearer SYNTHETIC_BORROWED_TOKEN"}};
    r.body=R"({"stex_tp":"ND","stk_cd":"SYNTH"})";
    QVERIFY(NetworkTradingTransport::allowed(r));
    r.headers["api-id"]="kt10000";QVERIFY(!NetworkTradingTransport::allowed(r));
    r.headers["api-id"]="usa20100";r.url.setPath("/api/dostk/ordr");QVERIFY(!NetworkTradingTransport::allowed(r));
}
void KiwoomMarketProtocolTest::cursorIsOnlyABoundedContinuationHeader() {
    auto i=intent(KiwoomDisplayRequest::UsCatalog,"US",{},"%");i.cursor="SYNTHETIC-NEXT-KEY";const auto r=KiwoomMarketProtocol::request(i);
    QVERIFY(KiwoomMarketProtocol::isRequest(r));QCOMPARE(r.headers.value("cont-yn"),QByteArray("Y"));QCOMPARE(r.headers.value("next-key"),i.cursor);QVERIFY(r.url.query().isEmpty());QVERIFY(!r.body.contains(i.cursor));
    i.cursor=QByteArray(51,'x');QVERIFY(KiwoomMarketProtocol::request(i).url.isEmpty());i.cursor="bad\r\nkey";QVERIFY(KiwoomMarketProtocol::request(i).url.isEmpty());
}
void KiwoomMarketProtocolTest::publicMetadataAndUsdProofKeepSeparateOriginalReceipts() {
    const qint64 metadataAt=1792710000000LL,currencyAt=metadataAt+400;
    const auto m=KiwoomMarketProtocol::parseMetadata(intent(KiwoomDisplayRequest::UsMetadata,"US","SYNTH"),
        response({{"list",QJsonArray{QJsonObject{{"stex_tp","ND"},{"stk_cd","SYNTH"},{"stk_nm","합성 공개 종목"}}}}},"usa10098"),metadataAt);
    QVERIFY(m.available);QCOMPARE(m.instruments.size(),1);const auto i=m.instruments.first();QCOMPARE(i.identity.key(),QString("stock:US:SYNTH"));QCOMPARE(i.exchange,QString("ND"));QVERIFY(i.currency.isEmpty());QVERIFY(!i.verified);QCOMPARE(i.metadataEvidence.receivedAtMs,metadataAt);QCOMPARE(i.metadataEvidence.sourceTimeMs,0LL);
    const auto p=KiwoomMarketProtocol::parseRestPrice(intent(KiwoomDisplayRequest::UsPrice,"US","SYNTH","ND"),
        response({{"stex_tp","ND"},{"stk_cd","SYNTH"},{"curr_unit","USD"},{"cur_prc","+73.2500"},{"flu_rt","-1.25"}},"usa20100"),currencyAt);
    QVERIFY(p.available);QCOMPARE(p.quote.currency,QString("USD"));QCOMPARE(p.quote.rawSignedPrice,QString("+73.2500"));QCOMPARE(p.quote.price,QString("73.25"));QCOMPARE(p.quote.direction,MovementDirection::Down);QCOMPARE(p.quote.evidence.sourceTimeMs,0LL);QCOMPARE(p.currencyEvidence.receivedAtMs,currencyAt);QCOMPARE(p.currencyApiId,QString("usa20100"));
}
void KiwoomMarketProtocolTest::exactCountryVenueAndCurrencyAreNotGuessed_data() {
    QTest::addColumn<QString>("field");QTest::addColumn<QString>("value");QTest::newRow("wrong-symbol")<<QString("stk_cd")<<QString("OTHER");QTest::newRow("wrong-venue")<<QString("stex_tp")<<QString("NY");QTest::newRow("wrong-currency")<<QString("curr_unit")<<QString("KRW");QTest::newRow("missing-currency")<<QString("curr_unit")<<QString();
}
void KiwoomMarketProtocolTest::exactCountryVenueAndCurrencyAreNotGuessed() {
    QFETCH(QString,field);QFETCH(QString,value);QJsonObject body{{"stex_tp","ND"},{"stk_cd","SYNTH"},{"curr_unit","USD"},{"cur_prc","+73.2500"}};body[field]=value;
    QVERIFY(!KiwoomMarketProtocol::parseRestPrice(intent(KiwoomDisplayRequest::UsPrice,"US","SYNTH","ND"),response(body,"usa20100"),1792710000000LL).available);
}
void KiwoomMarketProtocolTest::usRegistrationKeepsExactVenueAndOneType() {
    const QList<KiwoomMarketTarget> rows{{{"stock","US","SYNTH"},"ND",0,false},{{"stock","US","SYNTH-B"},"NY",1,false}};
    const auto bytes=KiwoomMarketProtocol::subscription("US",rows);QVERIFY(!bytes.isEmpty());const auto packet=QJsonDocument::fromJson(bytes).object();QCOMPARE(packet.value("trnm").toString(),QString("REG"));QCOMPARE(packet.value("refresh").toString(),QString("0"));
    const auto groups=packet.value("data").toArray();QCOMPARE(groups.size(),1);const auto group=groups.first().toObject();QCOMPARE(group.value("type").toArray(),QJsonArray{"FE"});const auto items=group.value("item").toArray();QCOMPARE(items.size(),2);QCOMPARE(items.first().toObject().value("jmcode").toString(),QString("SYNTH"));QCOMPARE(items.first().toObject().value("stex_tp").toString(),QString("ND"));QVERIFY(!items.first().toObject().contains("country"));
}
void KiwoomMarketProtocolTest::mixedCountryAnd201RegistrationFailAtomically() {
    QList<KiwoomMarketTarget> rows{{{"stock","US","SYNTH"},"ND",0,false},{{"stock","KR","0101N0"},"KRX",0,false}};QVERIFY(KiwoomMarketProtocol::subscription("US",rows).isEmpty());rows.removeLast();for(int n=1;n<=200;++n)rows.append({{"stock","US",(QStringLiteral("SYN")+QChar('A'+n/676%26)+QChar('A'+n/26%26)+QChar('A'+n%26))},"ND",3,false});QCOMPARE(rows.size(),201);QVERIFY(KiwoomMarketProtocol::subscription("US",rows).isEmpty());
}
void KiwoomMarketProtocolTest::acknowledgementsNeedPresentExactSuccessAndPingEchoIsOriginal() {
    QVERIFY(KiwoomMarketProtocol::controlAck(R"({"trnm":"LOGIN","return_code":0})","LOGIN").accepted);
    QVERIFY(KiwoomMarketProtocol::controlAck(R"({"trnm":"REG","return_code":"0"})","REG").accepted);
    QVERIFY(!KiwoomMarketProtocol::controlAck(R"({"trnm":"LOGIN"})","LOGIN").accepted);QVERIFY(!KiwoomMarketProtocol::controlAck(R"({"trnm":"LOGIN","return_code":1})","LOGIN").accepted);QVERIFY(!KiwoomMarketProtocol::controlAck(R"({"trnm":"REG","return_code":0})","LOGIN").accepted);
    const QByteArray ping=" { \"trnm\" : \"PING\", \"synthetic\":\"keep-whitespace\" } ";const auto echo=KiwoomMarketProtocol::pingEcho(ping);QVERIFY(echo);QCOMPARE(*echo,ping);const auto plain=KiwoomMarketProtocol::pingEcho("PING");QVERIFY(plain);QCOMPARE(*plain,QByteArray("PING"));QVERIFY(!KiwoomMarketProtocol::pingEcho("PING-extra"));
}
void KiwoomMarketProtocolTest::usRealtimeKeepsUnknownUtcClockAndUndocumentedChangeHidden() {
    const qint64 at=1792710000000LL;const auto rows=KiwoomMarketProtocol::realtimeQuotes(realtime(),"US",{usInstrument(at)},at+1);QCOMPARE(rows.size(),1);const auto q=rows.first();QCOMPARE(q.identity.key(),QString("stock:US:SYNTH"));QCOMPARE(q.currency,QString("USD"));QCOMPARE(q.price,QString("73.25"));QCOMPARE(q.evidence.sourceTimeMs,0LL);QCOMPARE(q.evidence.receivedAtMs,at+1);QCOMPARE(q.rawDate,QString("20261023"));QCOMPARE(q.rawTime,QString("103015"));QCOMPARE(q.rawLocalTime,QString("213015"));QVERIFY(q.changeText.isEmpty());QCOMPARE(q.direction,MovementDirection::Unknown);
    QVERIFY(KiwoomMarketProtocol::realtimeQuotes(realtime(),"KR",{usInstrument(at)},at+1).isEmpty());
}
void KiwoomMarketProtocolTest::dailyChartRequestHasOneExactAdjustedShape(){
    auto daily=intent(KiwoomDisplayRequest::KrDaily,"KR","005930");daily.baseDate="20261023";
    const auto r=KiwoomMarketProtocol::request(daily);QCOMPARE(r.url,QUrl("https://api.kiwoom.com/api/dostk/chart"));QCOMPARE(r.method,QByteArray("POST"));QVERIFY(r.authenticated);
    QCOMPARE(r.headers.value("api-id"),QByteArray("ka10081"));QCOMPARE(QJsonDocument::fromJson(r.body).object(),(QJsonObject{{"stk_cd","005930"},{"base_dt","20261023"},{"upd_stkpc_tp","1"}}));
    auto signedRequest=r;signedRequest.headers["authorization"]="Bearer SYNTHETIC_TOKEN";QVERIFY(KiwoomMarketProtocol::isRequest(signedRequest));QVERIFY(NetworkTradingTransport::allowed(signedRequest));
    // Unadjusted prices, other dates, extra fields, cursors and US symbols never match the exact shape.
    for(const auto& body:{QByteArray(R"({"stk_cd":"005930","base_dt":"20261023","upd_stkpc_tp":"0"})"),QByteArray(R"({"stk_cd":"005930","base_dt":"20261023","upd_stkpc_tp":"1","x":"1"})"),
                          QByteArray(R"({"stk_cd":"005930","base_dt":"2026-10-23","upd_stkpc_tp":"1"})"),QByteArray(R"({"stk_cd":"005930","base_dt":"20261332","upd_stkpc_tp":"1"})")}){
        auto bad=signedRequest;bad.body=body;QVERIFY2(!KiwoomMarketProtocol::isRequest(bad),body.constData());}
    auto cursor=signedRequest;cursor.headers["cont-yn"]="Y";cursor.headers["next-key"]="SYNTHETIC";QVERIFY(!KiwoomMarketProtocol::isRequest(cursor));
    auto us=daily;us.country="US";us.symbol="SYNTH";QVERIFY(KiwoomMarketProtocol::request(us).url.isEmpty());
    auto missing=daily;missing.baseDate.clear();QVERIFY(KiwoomMarketProtocol::request(missing).url.isEmpty());
    auto stray=intent(KiwoomDisplayRequest::KrPrice,"KR","005930");stray.baseDate="20261023";QVERIFY(KiwoomMarketProtocol::request(stray).url.isEmpty());
}
void KiwoomMarketProtocolTest::dailyChartParsesSignedPricesStrictly_data(){
    QTest::addColumn<QJsonArray>("rows");QTest::addColumn<QByteArray>("api");QTest::addColumn<bool>("valid");
    const auto row=[](QString dt,QString open,QString high,QString low,QString close,QString volume){return QJsonObject{{"dt",dt},{"open_pric",open},{"high_pric",high},{"low_pric",low},{"cur_prc",close},{"trde_qty",volume}};};
    QTest::newRow("signed")<<QJsonArray{row("20261023","+70000","+71500","-69800","-70100","1234567"),row("20261022","69000","70500","68900","70000","987654")}<<QByteArray("ka10081")<<true;
    QTest::newRow("empty")<<QJsonArray{}<<QByteArray("ka10081")<<true;
    QTest::newRow("after-base")<<QJsonArray{row("20261024","1","1","1","1","1")}<<QByteArray("ka10081")<<false;
    QTest::newRow("duplicate")<<QJsonArray{row("20261022","1","1","1","1","1"),row("20261022","1","1","1","1","1")}<<QByteArray("ka10081")<<false;
    QTest::newRow("high-below-close")<<QJsonArray{row("20261022","100","101","99","102","1")}<<QByteArray("ka10081")<<false;
    QTest::newRow("zero-price")<<QJsonArray{row("20261022","0","1","0","1","1")}<<QByteArray("ka10081")<<false;
    QTest::newRow("negative-volume")<<QJsonArray{row("20261022","1","1","1","1","-5")}<<QByteArray("ka10081")<<false;
    QTest::newRow("bad-date")<<QJsonArray{row("2026-10-22","1","1","1","1","1")}<<QByteArray("ka10081")<<false;
    QTest::newRow("other-api")<<QJsonArray{row("20261022","1","1","1","1","1")}<<QByteArray("ka10080")<<false;
}
void KiwoomMarketProtocolTest::dailyChartParsesSignedPricesStrictly(){
    QFETCH(QJsonArray,rows);QFETCH(QByteArray,api);QFETCH(bool,valid);
    auto daily=intent(KiwoomDisplayRequest::KrDaily,"KR","005930");daily.baseDate="20261023";
    const auto page=KiwoomMarketProtocol::parseDaily(daily,response({{"stk_cd","005930"},{"stk_dt_pole_chart_qry",rows}},api),1792710000000LL);
    QCOMPARE(page.available,valid);if(!valid){QVERIFY(page.rows.isEmpty());return;}
    QCOMPARE(page.rows.size(),rows.size());for(int i=1;i<page.rows.size();++i)QVERIFY(page.rows[i-1].date<page.rows[i].date);
    if(!page.rows.isEmpty()){QCOMPARE(page.rows.last().open,QString("70000"));QCOMPARE(page.rows.last().low,QString("69800"));QCOMPARE(page.rows.last().close,QString("70100"));}
    const auto other=KiwoomMarketProtocol::parseDaily(daily,response({{"stk_cd","000660"},{"stk_dt_pole_chart_qry",rows}},api),1792710000000LL);QVERIFY(!other.available);
    const auto limited=KiwoomMarketProtocol::parseDaily(daily,response({},api,429),1792710000000LL);QVERIFY(limited.rateLimited);QVERIFY(!limited.available);
}
QTEST_GUILESS_MAIN(KiwoomMarketProtocolTest)
#include "tst_kiwoom_market_protocol.moc"
