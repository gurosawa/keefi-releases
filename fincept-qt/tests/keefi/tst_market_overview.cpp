#include "keefi/MarketOverview.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QTimeZone>
#include <QUrlQuery>
#include <QTest>
namespace keefi {
namespace {
qint64 at(int hour,int minute=0){return QDateTime(QDate(2026,10,22),QTime(hour,minute),QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();}
QByteArray entry(const QByteArray& date,const QByteArray& value){return "<entry><content type=\"application/xml\"><m:properties><d:NEW_DATE>"+date+"T00:00:00</d:NEW_DATE><d:BC_10YEAR>"+value+"</d:BC_10YEAR></m:properties></content></entry>";}
HttpResult response(const QByteArray& entries){HttpResult r;r.status=200;r.body="<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:d=\"http://schemas.microsoft.com/ado/2007/08/dataservices\" xmlns:m=\"http://schemas.microsoft.com/ado/2007/08/dataservices/metadata\">"+entries+"</feed>";return r;}
class FeedTransport final:public TradingTransport {
  public:
    QList<RequestSpec> requests;QList<quint64> tokens;int aborts=0;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{++aborts;}
    void complete(const HttpResult& result){emit finished(tokens.last(),result);}
};
}
class MarketOverviewTests final:public QObject {
    Q_OBJECT
  private slots:
    void xmlAttributeBudgetKeepsNormalData() {
        QByteArray extra = "<ignored";
        for (int i = 0; i < 64; ++i) extra += " a" + QByteArray::number(i) + "=\"\"";
        extra += "/>";
        const auto expected = MarketOverview::parse(response(entry("2026-10-21", "4.25")), QDate(2026, 10, 1), at(10));
        const auto actual = MarketOverview::parse(response(entry("2026-10-21", "4.25") + extra), QDate(2026, 10, 1), at(10));
        QVERIFY(actual.available);
        QCOMPARE(actual.percent, expected.percent);
        QCOMPARE(actual.observationDate, expected.observationDate);
        QCOMPARE(actual.receivedAtMs, expected.receivedAtMs);
        QCOMPARE(actual.source, expected.source);
        QCOMPARE(actual.sourceUrl, expected.sourceUrl);
        QCOMPARE(actual.reason, expected.reason);
        QCOMPARE(actual.noObservations, expected.noObservations);
    }

    void asciiCompatibleLatin1FeedKeepsItsValue() {
        auto reply = response(entry("2026-10-21", "4.25") + QByteArray("<ignored>\xE9</ignored>"));
        reply.body.replace("<?xml version=\"1.0\"?>", "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>");
        const auto result = MarketOverview::parse(reply, QDate(2026, 10, 1), at(10));
        QVERIFY2(result.available, qPrintable(result.reason));
        QCOMPARE(result.percent, QStringLiteral("4.25"));
        QCOMPARE(result.observationDate, QDate(2026, 10, 21));
        QCOMPARE(result.receivedAtMs, at(10));
    }

    void rejectsXmlAttributeFloodBeforeParsing() {
        QByteArray extra = "<ignored";
        for (int i = 0; i < 65; ++i) extra += " a" + QByteArray::number(i) + "=\"\"";
        extra += "/>";
        const auto reply = response(entry("2026-10-21", "4.25") + extra);
        const auto result = MarketOverview::parse(reply, QDate(2026, 10, 1), at(10));
        QVERIFY(!result.available);
        QVERIFY(result.percent.isEmpty());
        QVERIFY(!result.noObservations);
        QCOMPARE(result.receivedAtMs, at(10));
        QCOMPARE(result.source, QStringLiteral("미국 재무부"));
        QCOMPARE(result.reason, QStringLiteral("공식 자료의 형식을 확인할 수 없습니다."));
    }

    void officialDailyYieldHasObservationDateAndReceptionInsteadOfAnInventedTick(){
        const auto result=MarketOverview::parse(response(entry("2026-10-20","4.20")+entry("2026-10-21","4.25")),QDate(2026,10,1),at(10));
        QVERIFY(result.available);QCOMPARE(result.percent,QString("4.25"));QCOMPARE(result.observationDate,QDate(2026,10,21));
        QCOMPARE(result.receivedAtMs,at(10));QCOMPARE(result.source,QString("미국 재무부"));QVERIFY(result.sourceUrl.startsWith("https://home.treasury.gov/"));
    }
    void automaticModeUsesKstBoundariesAndKeepsClosingContext(){
        QCOMPARE(MarketOverview::effectiveMode(MarketTimeMode::Automatic,at(6,59)),MarketTimeMode::Night);
        QCOMPARE(MarketOverview::effectiveMode(MarketTimeMode::Automatic,at(7)),MarketTimeMode::Morning);
        QCOMPARE(MarketOverview::effectiveMode(MarketTimeMode::Automatic,at(9)),MarketTimeMode::Korea);
        QCOMPARE(MarketOverview::effectiveMode(MarketTimeMode::Automatic,at(15,30)),MarketTimeMode::Korea);
        QVERIFY(MarketOverview::modeContext(MarketTimeMode::Automatic,at(15,30)).contains("마감 후"));
        QCOMPARE(MarketOverview::effectiveMode(MarketTimeMode::Automatic,at(21)),MarketTimeMode::Night);
        QCOMPARE(MarketOverview::effectiveMode(MarketTimeMode::Morning,at(23)),MarketTimeMode::Morning);
    }
    void treasuryRequestHasExactPublicGetContract(){
        auto request=MarketOverview::request(QDate(2026,10,22));QVERIFY(MarketOverview::isRequest(request));
        QCOMPARE(request.method,QByteArray("GET"));QVERIFY(!request.authenticated);QVERIFY(request.body.isEmpty());
        QCOMPARE(QUrlQuery(request.url).queryItemValue("field_tdr_date_value_month"),QString("202610"));
        QVERIFY(NetworkTradingTransport::allowed(request));
        request.headers["Authorization"]="secret";QVERIFY(!MarketOverview::isRequest(request));QVERIFY(!NetworkTradingTransport::allowed(request));
        request=MarketOverview::request(QDate(2026,10,1));request.method="POST";QVERIFY(!NetworkTradingTransport::allowed(request));
        request=MarketOverview::request(QDate(2026,10,1));request.url.setHost("home.treasury.gov.evil.invalid");QVERIFY(!NetworkTradingTransport::allowed(request));
        request=MarketOverview::request(QDate(2026,10,1));request.url.setQuery(request.url.query()+"&token=private");QVERIFY(!NetworkTradingTransport::allowed(request));
    }
    void futureMissingOldAndConflictingObservationsDoNotBecomeCurrentNumbers(){
        auto result=MarketOverview::parse(response(entry("2026-10-21","4.20")+entry("2026-10-22","9.99")),QDate(2026,10,1),at(10));
        QVERIFY(result.available);QCOMPARE(result.percent,QString("4.2"));QCOMPARE(result.observationDate,QDate(2026,10,21));
        result=MarketOverview::parse(response(entry("2026-10-20","4.20")+entry("2026-10-21","N/A")),QDate(2026,10,1),at(10));
        QVERIFY(!result.available);QVERIFY(result.percent.isEmpty());QVERIFY(result.reason.contains("제공"));
        result=MarketOverview::parse(response(entry("2026-10-01","4.20")),QDate(2026,10,1),at(10));QVERIFY(!result.available);QVERIFY(result.reason.contains("7일"));
        result=MarketOverview::parse(response(entry("2026-10-21","4.20")+entry("2026-10-21","4.30")),QDate(2026,10,1),at(10));QVERIFY(!result.available);QVERIFY(result.reason.contains("서로"));
        auto bad=response(entry("2026-10-21","4.20"));bad.body.chop(3);QVERIFY(!MarketOverview::parse(bad,QDate(2026,10,1),at(10)).available);
        bad=response(entry("2026-10-21","4.20"));bad.body.replace("<feed ","<!DOCTYPE feed [<!ENTITY e SYSTEM 'file:///private'>]><feed ");QVERIFY(!MarketOverview::parse(bad,QDate(2026,10,1),at(10)).available);
        bad.status=500;QVERIFY(!MarketOverview::parse(bad,QDate(2026,10,1),at(10)).available);
    }
    void emptyMonthFallsBackOnceAndHiddenLateRepliesAreIgnored(){
        FeedTransport transport;MarketOverview overview(&transport);overview.setEnabled(true);QCOMPARE(transport.requests.size(),1);QVERIFY(overview.busy());
        const auto monthText=QUrlQuery(transport.requests.first().url).queryItemValue("field_tdr_date_value_month");const auto month=QDate::fromString(monthText+"01","yyyyMMdd");
        transport.complete(response({}));QCOMPARE(transport.requests.size(),2);QCOMPARE(QUrlQuery(transport.requests.last().url).queryItemValue("field_tdr_date_value_month"),month.addMonths(-1).toString("yyyyMM"));
        transport.complete(response({}));QCOMPARE(transport.requests.size(),2);QVERIFY(!overview.busy());QVERIFY(!overview.treasury10Year().available);
        FeedTransport held;MarketOverview other(&held);other.setEnabled(true);QVERIFY(other.busy());other.setEnabled(false);QVERIFY(!other.busy());QCOMPARE(held.aborts,1);
        held.complete(response(entry(monthText.left(4).toUtf8()+"-"+monthText.right(2).toUtf8()+"-01","4.20")));QVERIFY(!other.treasury10Year().available);
    }
    void acceptedPublicValueExpiresAndTransportLossClearsIt(){
        auto transport=std::make_unique<FeedTransport>();MarketOverview overview(transport.get());overview.setEnabled(true);
        const auto now=QDateTime::currentMSecsSinceEpoch();const auto date=QDateTime::fromMSecsSinceEpoch(now,QTimeZone("America/New_York")).date();
        transport->complete(response(entry(date.toString(Qt::ISODate).toUtf8(),"4.25")));
        auto result=overview.treasury10Year();QVERIFY(result.available);QCOMPARE(result.percent,QString("4.25"));
        QVERIFY(!overview.treasury10Year(result.receivedAtMs+MarketOverview::SourceRecheckMs+1).available);
        transport.reset();result=overview.treasury10Year();QVERIFY(!result.available);QVERIFY(result.percent.isEmpty());QVERIFY(result.reason.contains("연결 없음"));
    }
    void regularRefreshKeepsTheLastPublicValueUntilAResponseArrives(){
        qint64 now=at(10);FeedTransport transport;MarketOverview overview(&transport,[&]{return now;});
        overview.setEnabled(true);transport.complete(response(entry("2026-10-21","4.25")));
        const auto original=overview.treasury10Year();QVERIFY(original.available);
        now+=60001;overview.refresh();QVERIFY(overview.busy());QCOMPARE(transport.requests.size(),2);
        const auto refreshing=overview.treasury10Year();
        QVERIFY(refreshing.available);QCOMPARE(refreshing.percent,QString("4.25"));
        QCOMPARE(refreshing.observationDate,QDate(2026,10,21));
        QCOMPARE(refreshing.receivedAtMs,original.receivedAtMs);
        // A refresh request is not successful source evidence and does not extend freshness.
        QVERIFY(!overview.treasury10Year(original.receivedAtMs+MarketOverview::SourceRecheckMs+1).available);
        HttpResult failure;failure.status=503;transport.complete(failure);
        QVERIFY(!overview.busy());const auto failed=overview.treasury10Year();
        QVERIFY(!failed.available);QVERIFY(failed.percent.isEmpty());QVERIFY(failed.reason.contains("연결 실패"));
    }
};
}
QTEST_GUILESS_MAIN(keefi::MarketOverviewTests)
#include "tst_market_overview.moc"
