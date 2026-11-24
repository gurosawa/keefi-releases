#include "keefi/PublicMarketHistory.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>

using namespace keefi;
class PublicHistoryTest : public QObject {
    Q_OBJECT
  private slots:
    void onlyExactPublicReads();
    void preservesActualTimesAndRejectsMissingOrWrongSymbols();
    void stockHistoryHasNoPublicRequest_data();
    void stockHistoryHasNoPublicRequest();
    void stockHistoryCannotBeParsed_data();
    void stockHistoryCannotBeParsed();
    void stockTargetsExplainBrokerRequirementWithoutSending_data();
    void stockTargetsExplainBrokerRequirementWithoutSending();
};
void PublicHistoryTest::onlyExactPublicReads() {
    for (const auto& t : QList<HistoryTarget>{{"coin", "BTC", "bitcoin"}, {"coin", "PUMP", {}}}) {
        const auto r = PublicMarketHistory::request(t);
        QVERIFY(PublicMarketHistory::isRequest(r)); QVERIFY(NetworkTradingTransport::allowed(r));
        auto bad = r; bad.method = "POST"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.authenticated = true; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers.insert("Authorization", "fake"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setQuery(r.url.query() + "&extra=1"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setPath(r.url.path() + "/other"); QVERIFY(!NetworkTradingTransport::allowed(bad));
    }
    QVERIFY(!PublicMarketHistory::isRequest(PublicMarketHistory::request({"coin", "BTC", "../../order"})));
    QVERIFY(!PublicMarketHistory::isRequest(PublicMarketHistory::request({"us", "X/../NVDA", {}})));
}
void PublicHistoryTest::preservesActualTimesAndRejectsMissingOrWrongSymbols() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    PublicHistory h;
    QJsonArray prices{QJsonArray{double(now - 60000), 102.0}, QJsonArray{double(now - 86460000), 88.0},
                      QJsonArray{double(now - 120000), 100.0}, QJsonArray{double(now - 90000), QJsonValue::Null},
                      QJsonArray{double(now + 3600000), 999.0}};
    QVERIFY(PublicMarketHistory::parse({"coin", "BTC", "bitcoin"}, QJsonDocument(QJsonObject{{"prices", prices}}).toJson(), now, &h));
    QCOMPARE(h.points.size(), 2); QCOMPARE(qint64(h.points.first().x()), now - 120000); QCOMPARE(h.points.last().y(), 102.0);
    QVERIFY(!PublicMarketHistory::parse({"coin", "BTC", "bitcoin"}, "{\"prices\":[]}", now, &h));
    QCOMPARE(h.points.size(), 2); // Parse failure cannot overwrite the caller's last good series.
    const auto at=QDateTime::fromMSecsSinceEpoch(now-60000,QTimeZone("UTC"));
    const QJsonArray native{
        QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",at.addSecs(-60).toString("yyyy-MM-ddTHH:mm:ss")},{"trade_price",100}},
        QJsonObject{{"market","KRW-BTC"},{"candle_date_time_utc",at.toString("yyyy-MM-ddTHH:mm:ss")},{"trade_price",102}}};
    QVERIFY(PublicMarketHistory::parse({"coin","BTC",{}},QJsonDocument(native).toJson(),now,&h));
    QCOMPARE(h.points.size(),2);QCOMPARE(h.points.last().y(),102.0);
    QVERIFY(!PublicMarketHistory::parse({"coin","ETH",{}},QJsonDocument(native).toJson(),now,&h));
    QCOMPARE(h.points.size(),2);
}
namespace {
void stockRows() {
    QTest::addColumn<QString>("section");QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("url");QTest::addColumn<QByteArray>("body");
    QTest::newRow("domestic")<<QString("kr")<<QString("207940")
        <<QString("https://api.stock.naver.com/chart/domestic/item/207940?periodType=day")
        <<QByteArray("{\"code\":\"207940\",\"priceInfos\":[{\"localDateTime\":\"20261023090000\",\"currentPrice\":100},{\"localDateTime\":\"20261023091000\",\"currentPrice\":102}]}");
    QTest::newRow("us")<<QString("us")<<QString("NVDA")
        <<QString("https://api.nasdaq.com/api/quote/NVDA/chart?assetclass=stocks")
        <<QByteArray("{\"data\":{\"symbol\":\"NVDA\",\"chart\":[{\"x\":1792670400000,\"y\":100},{\"x\":1792671000000,\"y\":102}]}}");
}
}
void PublicHistoryTest::stockHistoryHasNoPublicRequest_data(){stockRows();}
void PublicHistoryTest::stockHistoryHasNoPublicRequest(){
    QFETCH(QString,section);QFETCH(QString,id);QFETCH(QString,url);
    QVERIFY(PublicMarketHistory::request({section,id,{}}).url.isEmpty());
    RequestSpec legacy;legacy.operation=Operation::Markets;legacy.url=QUrl(url);
    legacy.headers={{"Accept","application/json"},{"User-Agent","Mozilla/5.0 (KEEFI public watchlist)"}};
    QVERIFY(!PublicMarketHistory::isRequest(legacy));QVERIFY(!NetworkTradingTransport::allowed(legacy));
}
void PublicHistoryTest::stockHistoryCannotBeParsed_data(){stockRows();}
void PublicHistoryTest::stockHistoryCannotBeParsed(){
    QFETCH(QString,section);QFETCH(QString,id);QFETCH(QByteArray,body);
    const auto now=QDateTime::fromString("2026-10-25T12:00:00Z",Qt::ISODate).toMSecsSinceEpoch();
    PublicHistory prior;prior.points={{1,100},{2,101}};prior.source="synthetic coin history";
    QVERIFY(!PublicMarketHistory::parse({section,id,{}},body,now,&prior));
    QCOMPARE(prior.points,(QList<QPointF>{{1,100},{2,101}}));QCOMPARE(prior.source,QString("synthetic coin history"));
}
void PublicHistoryTest::stockTargetsExplainBrokerRequirementWithoutSending_data(){stockRows();}
void PublicHistoryTest::stockTargetsExplainBrokerRequirementWithoutSending(){
    QFETCH(QString,section);QFETCH(QString,id);
    class Http final:public TradingTransport {public:QList<RequestSpec> sent;
        void send(quint64,const RequestSpec& request)override{sent.append(request);}void abort()override{}
    } transport;
    PublicMarketHistory history(&transport);history.setTargets({{section,id,{}}});QCoreApplication::processEvents();
    QVERIFY(transport.sent.isEmpty());const auto unavailable=history.history(section,id);
    QVERIFY(unavailable.points.isEmpty());QCOMPARE(unavailable.receivedAtMs,qint64(0));QVERIFY(unavailable.source.isEmpty());
    QCOMPARE(unavailable.status,QStringLiteral("증권사를 연결하면 볼 수 있어요"));
}
QTEST_GUILESS_MAIN(PublicHistoryTest)
#include "tst_public_history.moc"
