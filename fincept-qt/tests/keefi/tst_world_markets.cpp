#include "keefi/WorldMarkets.h"
#include "keefi/FxHistory.h"
#include "keefi/TradingTransport.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest>

using namespace keefi;

namespace {
// 2026-11-05 22:30 KST. Fixture times below are the provider strings seen that evening.
const qint64 Evening = QDateTime(QDate(2026, 11, 5), QTime(22, 30), QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();

class Wire final : public TradingTransport {
  public:
    QList<QPair<quint64, RequestSpec>> sent;
    int aborted = 0;
    void send(quint64 token, const RequestSpec& request) override { sent.append({token, request}); }
    void abort() override { ++aborted; }
    void answer(const QByteArray& body, int status = 200, const QMap<QByteArray, QByteArray>& headers = {}) {
        QVERIFY(!sent.isEmpty());
        HttpResult result; result.status = status; result.body = body; result.headers = headers;
        emit finished(sent.last().first, result);
    }
};

QByteArray indexBody(const QString& code, const QString& price, const QString& change, const QString& percent,
                     const QString& status, const QString& tradedAt, int delay = 0, const QString& venue = QStringLiteral("뉴욕 거래소")) {
    return QJsonDocument(QJsonObject{{"stockEndType", "index"}, {"reutersCode", code}, {"closePrice", price},
        {"compareToPreviousClosePrice", change}, {"fluctuationsRatio", percent}, {"marketStatus", status},
        {"localTradedAt", tradedAt}, {"delayTime", delay},
        {"stockExchangeType", QJsonObject{{"nameKor", venue}, {"delayTime", delay}}}}).toJson();
}
QByteArray marketBody(const QString& category, const QString& code, const QString& price, const QString& change,
                      const QString& percent, const QString& status, const QString& tradedAt, const QString& venue = QStringLiteral("하나은행")) {
    return QJsonDocument(QJsonObject{{"isSuccess", true}, {"detailCode", ""}, {"result", QJsonObject{
        {"categoryType", category}, {"reutersCode", code}, {"closePrice", price}, {"fluctuations", change},
        {"fluctuationsRatio", percent}, {"marketStatus", status}, {"localTradedAt", tradedAt},
        {"stockExchangeType", QJsonObject{{"nameKor", venue}, {"delayTime", category == "exchange" ? 0 : 10}}}}}}).toJson();
}
// A well-formed answer for whatever the model asked: closed indices, open postings and futures.
QByteArray answerFor(const RequestSpec& request, const QString& bankTime = QStringLiteral("2026-11-05T22:21:04+09:00")) {
    const auto url = request.url;
    if (url.path().endsWith(QStringLiteral("/basic"))) {
        const auto code = url.path().section('/', -2, -2);
        return indexBody(code, "5,383.80", "-300.00", "-5.28", "CLOSE", code == "KOSPI" ? "2026-11-05T20:15:00+09:00" : "2026-11-04T16:51:11-05:00");
    }
    const QUrlQuery query(url);
    const auto category = query.queryItemValue("category"), code = query.queryItemValue("reutersCode");
    if (category == "exchange") return marketBody(category, code, "1,344.90", "2.40", "0.18", "OPEN", bankTime);
    return marketBody(category, code, "4,129.00", "56.00", "1.37", "OPEN", "2026-11-05T07:14:35-06:00", QStringLiteral("뉴욕상품거래소"));
}
QByteArray ecbBody(bool completeLatest = true, const QString& ns = QStringLiteral("http://www.ecb.int/vocabulary/2002-08-01/eurofxref")) {
    // ECB eurofxref-hist-90d.xml shape; the 2026-11-04/03 rates are the published ones.
    return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?><gesmes:Envelope xmlns:gesmes="http://www.gesmes.org/xml/2002-08-01" xmlns="%1"><gesmes:subject>Reference rates</gesmes:subject><gesmes:Sender><gesmes:name>European Central Bank</gesmes:name></gesmes:Sender><Cube>)"
        R"(<Cube time="2026-11-04"><Cube currency="USD" rate="1.1267"/><Cube currency="JPY" rate="178.3"/><Cube currency="GBP" rate="0.8346"/><Cube currency="SEK" rate="10.9718"/>%2<Cube currency="CAD" rate="1.5701"/><Cube currency="KRW" rate="1508.13"/></Cube>)"
        R"(<Cube time="2026-11-03"><Cube currency="USD" rate="1.1179"/><Cube currency="JPY" rate="176.95"/><Cube currency="GBP" rate="0.83358"/><Cube currency="SEK" rate="10.9515"/><Cube currency="CHF" rate="0.9166"/><Cube currency="CAD" rate="1.5623"/></Cube>)"
        R"(<Cube time="2026-10-31"><Cube currency="USD" rate="1.1150"/><Cube currency="JPY" rate="176.10"/><Cube currency="GBP" rate="0.8330"/><Cube currency="SEK" rate="10.9400"/><Cube currency="CHF" rate="0.9150"/><Cube currency="CAD" rate="1.5600"/></Cube>)"
        R"(</Cube></gesmes:Envelope>)").arg(ns, completeLatest ? QStringLiteral(R"(<Cube currency="CHF" rate="0.9188"/>)") : QString{}).toUtf8();
}
// The Japan MOF current-month table exactly as served on 2026-11-05 (CRLF, Shift_JIS note).
const QByteArray MofNovember =
    "Interest Rate (November 2026),,,,,,,,,,,,,,,(Unit : %)\r\n"
    "Date,1Y,2Y,3Y,4Y,5Y,6Y,7Y,8Y,9Y,10Y,15Y,20Y,25Y,30Y,40Y\r\n"
    "2026/11/2,1.671,1.88,1.982,2.136,2.243,2.326,2.429,2.569,2.692,2.817,3.362,3.669,3.972,3.94,3.96\r\n"
    "2026/11/4,1.676,1.891,1.997,2.159,2.269,2.358,2.465,2.603,2.728,2.855,3.393,3.693,4.004,3.979,3.99\r\n"
    ",,,,,,,,,,,,,,,\r\n"
    "\"  \x81\xa6If you cannot download the latest csv data, please clear the browser's cache and download again.\",,,,,,,,,,,,,,,\r\n";

WorldMarketQuote bank(const QString& price, const QString& change) {
    WorldMarketQuote q; q.price = price; q.change = change; q.changePercent = "0.10"; q.session = WorldMarketSession::Open;
    q.tradedAtMs = Evening - 60000; q.localDate = QDate(2026, 11, 5); q.venue = QStringLiteral("하나은행"); q.receivedAtMs = Evening;
    return q;
}
} // namespace

class WorldMarketsTest : public QObject {
    Q_OBJECT
  private slots:
    void xmlAttributeBudgetKeepsNormalData() {
        WorldDollarIndex expected;
        QString reason;
        QVERIFY(WorldMarkets::parseEcbDollarIndex(ecbBody(), Evening, &expected, &reason));
        QByteArray extra = "<ignored";
        for (int i = 0; i < 64; ++i) extra += " a" + QByteArray::number(i) + "=\"\"";
        extra += "/>";
        auto body = ecbBody();
        body.replace("</gesmes:Envelope>", extra + "</gesmes:Envelope>");
        WorldDollarIndex actual;
        QVERIFY2(WorldMarkets::parseEcbDollarIndex(body, Evening, &actual, &reason), qPrintable(reason));
        QVERIFY(reason.isEmpty());
        QVERIFY(actual == expected);
    }

    void rejectsXmlAttributeFloodBeforeParsing() {
        QByteArray extra = "<ignored";
        for (int i = 0; i < 65; ++i) extra += " a" + QByteArray::number(i) + "=\"\"";
        extra += "/>";
        auto body = ecbBody();
        body.replace("</gesmes:Envelope>", extra + "</gesmes:Envelope>");
        WorldDollarIndex out;
        out.value = "kept";
        out.receivedAtMs = 17;
        const auto before = out;
        QString reason;
        QVERIFY(!WorldMarkets::parseEcbDollarIndex(body, Evening, &out, &reason));
        QVERIFY(out == before);
        QCOMPARE(reason, QStringLiteral("달러 인덱스 ECB XML 형식 확인 필요"));
    }

    void requestsAreExactKeylessReads() {
        QSet<QString> urls;
        for (int i = 0; i < WorldMarketItemCount; ++i) {
            const auto item = WorldMarketItem(i);
            const auto r = WorldMarkets::request(item);
            QVERIFY2(!WorldMarkets::itemName(item).isEmpty(), qPrintable(QString::number(i)));
            QCOMPARE(r.method, QByteArray("GET")); QCOMPARE(r.operation, Operation::Markets); QVERIFY(!r.authenticated); QVERIFY(r.body.isEmpty());
            QCOMPARE(r.url.scheme(), QString("https")); QVERIFY(r.url.host().endsWith(QStringLiteral("stock.naver.com")));
            QVERIFY(WorldMarkets::isRequest(r)); QVERIFY(NetworkTradingTransport::allowed(r));
            urls.insert(r.url.toString());
        }
        QCOMPARE(urls.size(), WorldMarketItemCount);
        QCOMPARE(WorldMarkets::request(WorldMarketItem::Spx).url, QUrl("https://api.stock.naver.com/index/.INX/basic"));
        QCOMPARE(WorldMarkets::request(WorldMarketItem::Kospi).url, QUrl("https://m.stock.naver.com/api/index/KOSPI/basic"));
        QCOMPARE(WorldMarkets::request(WorldMarketItem::JpyKrw).url, QUrl("https://m.stock.naver.com/front-api/marketIndex/productDetail?category=exchange&reutersCode=FX_JPYKRW"));
        for (const auto& daily : {WorldMarkets::ecbRequest(), WorldMarkets::mofRequest()}) {
            QVERIFY(WorldMarkets::isRequest(daily)); QVERIFY(NetworkTradingTransport::allowed(daily));
        }
        // Near misses stay outside every allowlist.
        auto other = WorldMarkets::request(WorldMarketItem::UsdKrw);
        other.url = QUrl("https://m.stock.naver.com/front-api/marketIndex/productDetail?category=exchange&reutersCode=FX_EURKRW");
        QVERIFY(!WorldMarkets::isRequest(other)); QVERIFY(!NetworkTradingTransport::allowed(other));
        auto reordered = WorldMarkets::request(WorldMarketItem::Gold);
        reordered.url = QUrl("https://m.stock.naver.com/front-api/marketIndex/productDetail?reutersCode=GCcv1&category=metals");
        QVERIFY(!WorldMarkets::isRequest(reordered));
        auto posted = WorldMarkets::request(WorldMarketItem::Spx); posted.method = "POST"; QVERIFY(!WorldMarkets::isRequest(posted)); QVERIFY(!NetworkTradingTransport::allowed(posted));
        auto signedRead = WorldMarkets::request(WorldMarketItem::Spx); signedRead.authenticated = true; QVERIFY(!WorldMarkets::isRequest(signedRead));
        auto header = WorldMarkets::request(WorldMarketItem::Spx); header.headers.insert("Authorization", "x"); QVERIFY(!WorldMarkets::isRequest(header)); QVERIFY(!NetworkTradingTransport::allowed(header));
        auto quote = WorldMarkets::request(WorldMarketItem::Spx); quote.operation = Operation::Quote; QVERIFY(!WorldMarkets::isRequest(quote));
        auto fullHistory = WorldMarkets::ecbRequest(); fullHistory.url = QUrl("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-hist.xml");
        // The dollar index never reads the full history. Since phase 2 the 시장 비교 tab's
        // FX history owns exactly this keyless read (FxHistory::ecbHistoryRequest).
        QVERIFY(!WorldMarkets::isRequest(fullHistory)); QVERIFY(FxHistory::isRequest(fullHistory));
        auto fullZip = fullHistory; fullZip.url = QUrl("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-hist.zip");
        QVERIFY(!NetworkTradingTransport::allowed(fullZip));
        auto allYears = WorldMarkets::mofRequest(); allYears.url = QUrl("https://www.mof.go.jp/english/policy/jgbs/reference/interest_rate/historical/jgbcme_all.csv");
        QVERIFY(!WorldMarkets::isRequest(allYears)); QVERIFY(!NetworkTradingTransport::allowed(allYears));
    }

    void parsesIndexBankAndFuturesRows() {
        WorldMarketQuote q; QString reason;
        QVERIFY2(WorldMarkets::parseQuote(WorldMarketItem::Spx, indexBody(".INX", "7,688.62", "119.70", "1.58", "CLOSE", "2026-11-04T16:51:11-05:00"), Evening, &q, &reason), qPrintable(reason));
        QCOMPARE(q.price, QString("7688.62")); QCOMPARE(q.change, QString("119.70")); QCOMPARE(q.changePercent, QString("1.58"));
        QCOMPARE(q.session, WorldMarketSession::Closed); QCOMPARE(q.localDate, QDate(2026, 11, 4));
        QCOMPARE(q.tradedAtMs, QDateTime(QDate(2026, 11, 4), QTime(21, 51, 11), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(q.venue, QStringLiteral("뉴욕 거래소")); QCOMPARE(q.delayMinutes, 0); QCOMPARE(q.receivedAtMs, Evening);
        QVERIFY2(WorldMarkets::parseQuote(WorldMarketItem::Nikkei, indexBody(".N225", "61,077.90", "-1,110.73", "-1.79", "CLOSE", "2026-11-05T15:45:03+09:00", 15, QStringLiteral("도쿄 증권거래소")), Evening, &q, &reason), qPrintable(reason));
        QCOMPARE(q.price, QString("61077.90")); QCOMPARE(q.change, QString("-1110.73")); QCOMPARE(q.changePercent, QString("-1.79")); QCOMPARE(q.delayMinutes, 15);
        QVERIFY2(WorldMarkets::parseQuote(WorldMarketItem::Kospi, indexBody("KOSPI", "5,383.80", "-300.00", "-5.28", "CLOSE", "2026-11-05T20:15:00+09:00", 0, QStringLiteral("코스피")), Evening, &q, &reason), qPrintable(reason));
        QCOMPARE(q.localDate, QDate(2026, 11, 5));
        QVERIFY2(WorldMarkets::parseQuote(WorldMarketItem::UsdKrw, marketBody("exchange", "FX_USDKRW", "1,344.90", "2.40", "0.18", "OPEN", "2026-11-05T22:21:04+09:00"), Evening, &q, &reason), qPrintable(reason));
        QCOMPARE(q.price, QString("1344.90")); QCOMPARE(q.session, WorldMarketSession::Open); QCOMPARE(q.venue, QStringLiteral("하나은행"));
        QVERIFY2(WorldMarkets::parseQuote(WorldMarketItem::Gold, marketBody("metals", "GCcv1", "4,129.00", "56.00", "1.37", "OPEN", "2026-11-05T07:14:35-06:00", QStringLiteral("뉴욕상품거래소")), Evening, &q, &reason), qPrintable(reason));
        QCOMPARE(q.price, QString("4129.00")); QCOMPARE(q.delayMinutes, 10); QCOMPARE(q.localDate, QDate(2026, 11, 5));
    }

    void rejectsMismatchedOrMalformedRows_data() {
        QTest::addColumn<int>("item"); QTest::addColumn<QByteArray>("body"); QTest::addColumn<QString>("why");
        const auto spx = int(WorldMarketItem::Spx), usd = int(WorldMarketItem::UsdKrw);
        QTest::newRow("other-index") << spx << indexBody(".NDX", "1", "0", "0", "OPEN", "2026-11-05T09:00:00-05:00") << QStringLiteral("식별");
        QTest::newRow("not-an-index") << spx << QJsonDocument(QJsonObject{{"stockEndType", "stock"}, {"reutersCode", ".INX"}}).toJson() << QStringLiteral("식별");
        QTest::newRow("other-category") << usd << marketBody("metals", "FX_USDKRW", "1", "0", "0", "OPEN", "2026-11-05T22:00:00+09:00") << QStringLiteral("식별");
        QTest::newRow("other-code") << usd << marketBody("exchange", "FX_EURKRW", "1", "0", "0", "OPEN", "2026-11-05T22:00:00+09:00") << QStringLiteral("식별");
        QTest::newRow("unsuccessful") << usd << QByteArray(R"({"isSuccess":false,"detailCode":"FE-4000","message":"wrong reuters code.","result":{}})") << QStringLiteral("실패");
        QTest::newRow("negative-price") << usd << marketBody("exchange", "FX_USDKRW", "-1,344.90", "2.40", "0.18", "OPEN", "2026-11-05T22:00:00+09:00") << QStringLiteral("가격");
        QTest::newRow("text-price") << usd << marketBody("exchange", "FX_USDKRW", "1.344,90", "2.40", "0.18", "OPEN", "2026-11-05T22:00:00+09:00") << QStringLiteral("가격");
        QTest::newRow("sign-mismatch") << usd << marketBody("exchange", "FX_USDKRW", "1,344.90", "-2.40", "0.18", "OPEN", "2026-11-05T22:00:00+09:00") << QStringLiteral("부호");
        QTest::newRow("implausible-move") << spx << indexBody(".INX", "7,688.62", "5000", "65.00", "OPEN", "2026-11-05T09:00:00-05:00") << QStringLiteral("등락");
        QTest::newRow("future-time") << usd << marketBody("exchange", "FX_USDKRW", "1,344.90", "2.40", "0.18", "OPEN", "2026-11-05T23:45:00+09:00") << QStringLiteral("기준 시각");
        QTest::newRow("stale-time") << usd << marketBody("exchange", "FX_USDKRW", "1,344.90", "2.40", "0.18", "OPEN", "2026-08-01T22:00:00+09:00") << QStringLiteral("기준 시각");
        QTest::newRow("no-offset") << usd << marketBody("exchange", "FX_USDKRW", "1,344.90", "2.40", "0.18", "OPEN", "2026-11-05T22:00:00") << QStringLiteral("기준 시각");
        QTest::newRow("bad-delay") << spx << indexBody(".INX", "7,688.62", "1", "0.01", "OPEN", "2026-11-05T08:00:00-05:00", -1) << QStringLiteral("지연");
        QTest::newRow("not-json") << spx << QByteArray("<html>점검 중</html>") << QStringLiteral("형식");
        QTest::newRow("oversized") << spx << QByteArray(int(WorldMarkets::MaximumNaverBytes) + 1, ' ') << QStringLiteral("크기");
    }
    void rejectsMismatchedOrMalformedRows() {
        QFETCH(int, item); QFETCH(QByteArray, body); QFETCH(QString, why);
        WorldMarketQuote q; q.price = "untouched"; QString reason;
        QVERIFY(!WorldMarkets::parseQuote(WorldMarketItem(item), body, Evening, &q, &reason));
        QCOMPARE(q.price, QString("untouched"));
        QVERIFY2(reason.startsWith(WorldMarkets::itemName(WorldMarketItem(item))) && reason.contains(why), qPrintable(reason));
    }

    void dollarIndexAppliesIceWeightsToEcbRates() {
        WorldDollarIndex index; QString reason;
        QVERIFY2(WorldMarkets::parseEcbDollarIndex(ecbBody(), Evening, &index, &reason), qPrintable(reason));
        // Independent reference (Python, same rates): 101.254821 and 101.868722.
        QCOMPARE(index.value, QString("101.25")); QCOMPARE(index.date, QDate(2026, 11, 4));
        QCOMPARE(index.previousValue, QString("101.87")); QCOMPARE(index.previousDate, QDate(2026, 11, 3));
        QCOMPARE(index.changePercent, QString("-0.60")); QCOMPARE(index.receivedAtMs, Evening);
        // A latest day missing a weight is skipped, never filled from another day.
        QVERIFY(WorldMarkets::parseEcbDollarIndex(ecbBody(false), Evening, &index, &reason));
        QCOMPARE(index.date, QDate(2026, 11, 3)); QCOMPARE(index.value, QString("101.87")); QCOMPARE(index.previousDate, QDate(2026, 10, 31));
        WorldDollarIndex untouched; untouched.value = "kept";
        QVERIFY(!WorldMarkets::parseEcbDollarIndex(ecbBody(true, "http://example.invalid/rates"), Evening, &untouched, &reason)); QVERIFY(reason.contains("ECB"));
        QVERIFY(!WorldMarkets::parseEcbDollarIndex("<!DOCTYPE x [<!ENTITY a 'b'>]>" + ecbBody(), Evening, &untouched, &reason));
        const auto lateNovember = QDateTime(QDate(2026, 11, 20), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        QVERIFY(!WorldMarkets::parseEcbDollarIndex(ecbBody(), lateNovember, &untouched, &reason)); QVERIFY(reason.contains(QStringLiteral("기준일")));
        QCOMPARE(untouched.value, QString("kept"));
    }

    void japanYieldReadsTheTenYearColumn() {
        WorldJapanYield yield; QString reason;
        QVERIFY2(WorldMarkets::parseJapanYield(MofNovember, Evening, &yield, &reason), qPrintable(reason));
        QCOMPARE(yield.tenYear, QString("2.855")); QCOMPARE(yield.date, QDate(2026, 11, 4));
        QCOMPARE(yield.previousTenYear, QString("2.817")); QCOMPARE(yield.previousDate, QDate(2026, 11, 2));
        const QByteArray header = "Interest Rate (December 2026),,,,,,,,,,,,,,,(Unit : %)\r\nDate,1Y,2Y,3Y,4Y,5Y,6Y,7Y,8Y,9Y,10Y,15Y,20Y,25Y,30Y,40Y\r\n,,,,,,,,,,,,,,,\r\n";
        QVERIFY(!WorldMarkets::parseJapanYield(header, Evening, &yield, &reason)); QVERIFY2(reason.contains(QStringLiteral("공시 전")), qPrintable(reason));
        QByteArray noTenYear = MofNovember; noTenYear.replace(",10Y,", ",10Yr,");
        QVERIFY(!WorldMarkets::parseJapanYield(noTenYear, Evening, &yield, &reason));
        QByteArray backwards = MofNovember; backwards.replace("2026/11/4,", "2026/11/1,");
        QVERIFY(!WorldMarkets::parseJapanYield(backwards, Evening, &yield, &reason)); QVERIFY(reason.contains(QStringLiteral("순서")));
        QByteArray dash = MofNovember; dash.replace(",2.855,", ",-,");
        QVERIFY(WorldMarkets::parseJapanYield(dash, Evening, &yield, &reason)); QCOMPARE(yield.date, QDate(2026, 11, 2));
        QCOMPARE(yield.tenYear, QString("2.817")); // The published 11/02 row, not a blend.
    }

    void crossRatesUseEachLegsPreviousPosting() {
        // Independent reference: 1344.90 / 8.4860 = 158.48; previous 1342.50 / 8.4751.
        const auto usdJpy = WorldMarkets::cross(bank("1344.90", "2.40"), 1, bank("848.60", "1.09"), 100);
        QVERIFY(usdJpy.available()); QCOMPARE(usdJpy.price, QString("158.48")); QCOMPARE(usdJpy.change, QString("0.08")); QCOMPARE(usdJpy.changePercent, QString("0.05"));
        QCOMPARE(usdJpy.session, WorldMarketSession::Open); QCOMPARE(usdJpy.venue, QStringLiteral("하나은행"));
        const auto audJpy = WorldMarkets::cross(bank("941.63", "1.03"), 1, bank("848.60", "1.09"), 100);
        QCOMPARE(audJpy.price, QString("110.96")); QCOMPARE(audJpy.changePercent, QString("-0.02"));
        QVERIFY(!WorldMarkets::cross({}, 1, bank("848.60", "1.09"), 100).available());
        QVERIFY(!WorldMarkets::cross(bank("1344.90", "1344.90"), 1, bank("848.60", "1.09"), 100).available()); // No previous posting.
    }

    void premiumAndSpreadArithmetic() {
        QCOMPARE(WorldMarkets::premiumPercent("1347", "1344.90"), QString("0.16"));
        QCOMPARE(WorldMarkets::premiumPercent("1340", "1344.90"), QString("-0.36"));
        QCOMPARE(WorldMarkets::premiumPercent("1344.90", "1344.90"), QString("0.00"));
        QVERIFY(WorldMarkets::premiumPercent("", "1344.90").isEmpty()); QVERIFY(WorldMarkets::premiumPercent("1,347", "1344.90").isEmpty());
        QVERIFY(WorldMarkets::premiumPercent("1347", "0").isEmpty());
        QCOMPARE(WorldMarkets::spreadPoints("4.90", "2.855"), QString("2.045"));
        QCOMPARE(WorldMarkets::spreadPoints("4.9", "2.8"), QString("2.10"));
        QCOMPARE(WorldMarkets::spreadPoints("1.0", "1.25"), QString("-0.25"));
        QCOMPARE(WorldMarkets::spreadPoints("2.855", "2.855"), QString("0.00"));
        QVERIFY(WorldMarkets::spreadPoints("abc", "1").isEmpty());
    }

    void pollingSendsOneReadPerLaneAndSlowsClosedVenues() {
        qint64 now = Evening;
        Wire naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof, nullptr, [&] { return now; });
        QVERIFY(naver.sent.isEmpty());
        world.setActive(true);
        QCOMPARE(naver.sent.size(), 1); QCOMPARE(ecb.sent.size(), 1); QCOMPARE(mof.sent.size(), 1);
        QCOMPARE(naver.sent.first().second.url, WorldMarkets::request(WorldMarketItem::UsdKrw).url); // 원/달러 first: the tether premium waits on it.
        ecb.answer(ecbBody()); mof.answer(MofNovember);
        for (int i = 0; i < WorldMarketItemCount; ++i) {
            QCOMPARE(naver.sent.size(), i + 1); // Never two reads in flight on one transport.
            naver.answer(answerFor(naver.sent.last().second));
            QCoreApplication::processEvents();
        }
        QCoreApplication::processEvents();
        QCOMPARE(naver.sent.size(), WorldMarketItemCount);
        const auto first = world.snapshot();
        for (int i = 0; i < WorldMarketItemCount; ++i) QVERIFY2(first.quotes[i].available() && first.failures[i].isEmpty(), qPrintable(WorldMarkets::itemName(WorldMarketItem(i))));
        QVERIFY(first.dollarIndex.available()); QVERIFY(first.japanYield.available()); QVERIFY(first.active);
        world.refresh(); QCOMPARE(naver.sent.size(), WorldMarketItemCount); QCOMPARE(ecb.sent.size(), 1);
        // One minute later only open venues and bank postings are read again.
        now += WorldMarkets::RefreshMs;
        QSet<QString> second;
        world.refresh();
        while (naver.sent.size() > WorldMarketItemCount + second.size()) {
            second.insert(naver.sent.last().second.url.toString()); naver.answer(answerFor(naver.sent.last().second)); QCoreApplication::processEvents();
        }
        QCOMPARE(second.size(), 7); // 4 KRW postings + 3 futures; the 6 closed indices wait.
        QVERIFY(!second.contains(WorldMarkets::request(WorldMarketItem::Spx).url.toString()));
        QCOMPARE(ecb.sent.size(), 1); QCOMPARE(mof.sent.size(), 1); // Daily tables wait an hour.
        now += WorldMarkets::ClosedRefreshMs;
        world.refresh(); QVERIFY(naver.sent.size() > WorldMarketItemCount + 7);
        QCOMPARE(naver.sent.last().second.url, WorldMarkets::request(WorldMarketItem::UsdKrw).url);
    }

    void closedPostingsAndFuturesKeepMinuteRefresh_data() {
        QTest::addColumn<int>("item");
        QTest::addColumn<qint64>("refreshMs");
        QTest::newRow("bank-posting") << int(WorldMarketItem::UsdKrw) << qint64(60000);
        QTest::newRow("commodity-future") << int(WorldMarketItem::Gold) << qint64(60000);
        QTest::newRow("closed-index") << int(WorldMarketItem::Spx) << qint64(600000);
    }
    void closedPostingsAndFuturesKeepMinuteRefresh() {
        QFETCH(int, item);
        QFETCH(qint64, refreshMs);
        qint64 now = Evening;
        Wire naver;
        WorldMarkets world(&naver, nullptr, nullptr, nullptr, [&] { return now; });
        WorldMarketsDemand demand; demand.quotes = {WorldMarketItem(item)};
        world.setDemand(demand);
        QCOMPARE(naver.sent.size(), 1);
        auto body = answerFor(naver.sent.last().second);
        body.replace("\"OPEN\"", "\"CLOSE\"");
        naver.answer(body);
        QCOMPARE(world.snapshot().quote(WorldMarketItem(item)).session, WorldMarketSession::Closed);
        // Only a closed index slows down. Bank postings and commodity futures
        // are still re-read each minute, including when their provider says CLOSE.
        now += refreshMs - 1;
        world.refresh();
        QCOMPARE(naver.sent.size(), 1);
        ++now;
        world.refresh();
        QCOMPARE(naver.sent.size(), 2);
        QCOMPARE(naver.sent.last().second.url, WorldMarkets::request(WorldMarketItem(item)).url);
    }

    void rateLimitPausesOnlyTheQuoteLane() {
        qint64 now = Evening;
        Wire naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof, nullptr, [&] { return now; });
        world.setActive(true);
        naver.answer("", 429, {{"Retry-After", "120"}});
        QCoreApplication::processEvents();
        QCOMPARE(naver.sent.size(), 1);
        QVERIFY2(world.snapshot().failure(WorldMarketItem::UsdKrw).contains(QStringLiteral("요청 제한")), qPrintable(world.snapshot().failure(WorldMarketItem::UsdKrw)));
        ecb.answer("", 503); // The ECB lane keeps its own schedule while the quote host is paused.
        QVERIFY(world.snapshot().dollarIndexFailure.contains(QStringLiteral("달러 인덱스")));
        now += WorldMarkets::MinimumRetryMs; world.refresh();
        QCOMPARE(ecb.sent.size(), 2); QCOMPARE(naver.sent.size(), 1);
        now = Evening + 119000; world.refresh(); QCOMPARE(naver.sent.size(), 1);
        now = Evening + 120000; world.refresh(); QCOMPARE(naver.sent.size(), 2);
    }

    void olderAnswerNeverReplacesANewerOne() {
        qint64 now = Evening;
        Wire naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof, nullptr, [&] { return now; });
        world.setActive(true);
        naver.answer(answerFor(naver.sent.last().second));
        const auto held = world.snapshot().quote(WorldMarketItem::UsdKrw);
        QVERIFY(held.available());
        // Run the rest of the first round, then return a posting an hour older for 원/달러.
        for (int i = 1; i < WorldMarketItemCount; ++i) { QCoreApplication::processEvents(); naver.answer(answerFor(naver.sent.last().second)); }
        now += WorldMarkets::RefreshMs; world.refresh();
        QCOMPARE(naver.sent.last().second.url, WorldMarkets::request(WorldMarketItem::UsdKrw).url);
        naver.answer(answerFor(naver.sent.last().second, QStringLiteral("2026-11-05T21:21:04+09:00")));
        const auto after = world.snapshot();
        QCOMPARE(after.quote(WorldMarketItem::UsdKrw), held);
        QVERIFY(after.failure(WorldMarketItem::UsdKrw).contains(QStringLiteral("과거")));
    }

    // Before the open Naver repeats the last close with a 0.00 change and the current clock.
    // A held close for that price keeps its real session change; nothing is shown as flat.
    void preOpenKeepsTheLastSessionsClose() {
        qint64 now = Evening;
        Wire naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof, nullptr, [&] { return now; });
        world.setActive(true);
        const auto spx = WorldMarkets::request(WorldMarketItem::Spx).url;
        const auto round = [&](const QByteArray& spxBody) {
            for (int guard = 0; guard < 2 * WorldMarketItemCount; ++guard) {
                const auto request = naver.sent.last().second;
                naver.answer(request.url == spx ? spxBody : answerFor(request));
                const auto before = naver.sent.size();
                QCoreApplication::processEvents();
                if (naver.sent.size() == before) break;
            }
        };
        round(indexBody(".INX", "7,688.62", "119.70", "1.58", "CLOSE", "2026-11-04T16:51:11-05:00"));
        QCOMPARE(world.snapshot().quote(WorldMarketItem::Spx).changePercent, QString("1.58"));
        now += WorldMarkets::ClosedRefreshMs; world.refresh();
        round(indexBody(".INX", "7,688.62", "0.00", "0.00", "PREOPEN", "2026-11-05T08:35:00-05:00"));
        const auto kept = world.snapshot().quote(WorldMarketItem::Spx);
        QCOMPARE(kept.changePercent, QString("1.58")); QCOMPARE(kept.session, WorldMarketSession::Closed);
        QCOMPARE(kept.localDate, QDate(2026, 11, 4)); QCOMPARE(kept.receivedAtMs, now); // Still current, re-confirmed.
        QVERIFY(world.snapshot().failure(WorldMarketItem::Spx).isEmpty());
        // A first read during the pre-market has nothing to keep: it is stored as pre-open.
        WorldMarketQuote fresh; QString reason;
        QVERIFY2(WorldMarkets::parseQuote(WorldMarketItem::Spx, indexBody(".INX", "7,688.62", "0.00", "0.00", "PREOPEN", "2026-11-05T08:35:00-05:00"), Evening + 3600000, &fresh, &reason), qPrintable(reason));
        QCOMPARE(fresh.session, WorldMarketSession::PreOpen);
        // Once the venue opens, the live quote replaces the kept close.
        now = Evening + 2 * 3600000; world.refresh();
        round(indexBody(".INX", "7,701.10", "12.48", "0.16", "OPEN", "2026-11-05T09:31:00-05:00"));
        QCOMPARE(world.snapshot().quote(WorldMarketItem::Spx).session, WorldMarketSession::Open);
        QCOMPARE(world.snapshot().quote(WorldMarketItem::Spx).price, QString("7701.10"));
    }

    void preOpenDoesNotPreserveAnUnconfirmedClose_data() {
        QTest::addColumn<QString>("heldStatus");
        QTest::newRow("last-seen-open") << QString("OPEN");
        QTest::newRow("last-seen-unknown") << QString("UNKNOWN");
    }
    void preOpenDoesNotPreserveAnUnconfirmedClose() {
        QFETCH(QString, heldStatus);
        qint64 now = Evening;
        Wire naver;
        WorldMarkets world(&naver, nullptr, nullptr, nullptr, [&] { return now; });
        WorldMarketsDemand demand; demand.quotes = {WorldMarketItem::Spx};
        world.setDemand(demand);
        QCOMPARE(naver.sent.size(), 1);
        naver.answer(indexBody(".INX", "7,688.62", "119.70", "1.58", heldStatus, "2026-11-04T15:59:00-05:00"));
        QVERIFY(world.snapshot().quote(WorldMarketItem::Spx).available());
        // Hiding the page can miss the actual close. An equal pre-open price does
        // not turn the last observed intraday/unknown quote into a confirmed close.
        world.setDemand({});
        now += WorldMarkets::RefreshMs;
        world.setDemand(demand);
        QCOMPARE(naver.sent.size(), 2);
        naver.answer(indexBody(".INX", "7,688.62", "0.00", "0.00", "PREOPEN", "2026-11-05T08:31:00-05:00"));
        const auto quote = world.snapshot().quote(WorldMarketItem::Spx);
        QCOMPARE(quote.session, WorldMarketSession::PreOpen);
        QCOMPARE(quote.changePercent, QString("0.00"));
        QCOMPARE(quote.localDate, QDate(2026, 11, 5));
        QCOMPARE(quote.receivedAtMs, now);
        QVERIFY(world.snapshot().failure(WorldMarketItem::Spx).isEmpty());
    }

    // The chart header asks only for the won rate, three indices and the dollar index.
    void chartDemandReadsOnlyWhatTheHeaderShows() {
        qint64 now = Evening;
        Wire naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof, nullptr, [&] { return now; });
        world.setDemand(WorldMarketsDemand::chartHeader());
        QVERIFY(world.active());
        QCOMPARE(naver.sent.size(), 1); QCOMPARE(ecb.sent.size(), 1); QVERIFY(mof.sent.isEmpty());
        QSet<QString> read;
        for (int guard = 0; guard < WorldMarketItemCount; ++guard) {
            read.insert(naver.sent.last().second.url.toString());
            naver.answer(answerFor(naver.sent.last().second));
            const auto before = naver.sent.size(); QCoreApplication::processEvents();
            if (naver.sent.size() == before) break;
        }
        const QSet<QString> expected{WorldMarkets::request(WorldMarketItem::UsdKrw).url.toString(), WorldMarkets::request(WorldMarketItem::Spx).url.toString(),
                                     WorldMarkets::request(WorldMarketItem::Ndx).url.toString(), WorldMarkets::request(WorldMarketItem::Kospi).url.toString()};
        QCOMPARE(read, expected);
        // Opening Today widens the demand: the rest is read, what is held stays.
        const auto held = world.snapshot().quote(WorldMarketItem::UsdKrw);
        world.setDemand(WorldMarketsDemand::full());
        QCOMPARE(mof.sent.size(), 1);
        QCOMPARE(world.snapshot().quote(WorldMarketItem::UsdKrw), held);
        QCOMPARE(naver.sent.last().second.url, WorldMarkets::request(WorldMarketItem::JpyKrw).url);
        // Narrowing again cancels the unwanted read in flight; it is read later when wanted.
        world.setDemand(WorldMarketsDemand::chartHeader());
        QCOMPARE(naver.aborted, 1); QCOMPARE(mof.aborted, 1);
        naver.answer(answerFor(naver.sent.last().second)); // A late answer for the cancelled read is ignored.
        QVERIFY(!world.snapshot().quote(WorldMarketItem::JpyKrw).available());
        world.setDemand({});
        QVERIFY(!world.active()); QVERIFY(!world.snapshot().active);
    }

    void inactiveAbortsInFlightAndIgnoresLateAnswers() {
        qint64 now = Evening;
        Wire naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof, nullptr, [&] { return now; });
        world.setActive(true);
        world.setActive(false);
        QCOMPARE(naver.aborted, 1); QCOMPARE(ecb.aborted, 1); QCOMPARE(mof.aborted, 1);
        naver.answer(answerFor(naver.sent.last().second)); ecb.answer(ecbBody()); mof.answer(MofNovember);
        const auto snapshot = world.snapshot();
        QVERIFY(!snapshot.active); QVERIFY(!snapshot.quote(WorldMarketItem::UsdKrw).available()); QVERIFY(!snapshot.dollarIndex.available()); QVERIFY(!snapshot.japanYield.available());
        QCoreApplication::processEvents(); world.refresh(); QCOMPARE(naver.sent.size(), 1); // Inactive: nothing new goes out.
        world.setActive(true); QCOMPARE(naver.sent.size(), 2); // The aborted read is retried, not forgotten.
    }

    // Opt-in: KEEFI_WORLD_MARKETS_LIVE=1 reads the real public sources once.
    void liveSourcesWhenRequested() {
        if (qEnvironmentVariableIntValue("KEEFI_WORLD_MARKETS_LIVE") != 1) QSKIP("Set KEEFI_WORLD_MARKETS_LIVE=1 to read the real public sources.");
        NetworkTradingTransport naver, ecb, mof;
        WorldMarkets world(&naver, &ecb, &mof);
        world.setActive(true);
        const auto ready = [&] {
            const auto s = world.snapshot();
            for (int i = 0; i < WorldMarketItemCount; ++i) if (!s.quotes[i].available() && s.failures[i].isEmpty()) return false;
            return (s.dollarIndex.available() || !s.dollarIndexFailure.isEmpty()) && (s.japanYield.available() || !s.japanYieldFailure.isEmpty());
        };
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 90000);
        const auto s = world.snapshot();
        for (int i = 0; i < WorldMarketItemCount; ++i) {
            const auto& q = s.quotes[i];
            qInfo().noquote() << QString("WORLD_LIVE %1 price=%2 change=%3 percent=%4 session=%5 local=%6 delay=%7 venue=%8 failure=%9")
                .arg(WorldMarkets::itemName(WorldMarketItem(i)), q.price, q.change, q.changePercent).arg(int(q.session))
                .arg(q.localDate.toString(Qt::ISODate)).arg(q.delayMinutes).arg(q.venue, s.failures[i]);
            QVERIFY2(q.available(), qPrintable(s.failures[i]));
        }
        qInfo().noquote() << QString("WORLD_LIVE dxy=%1 date=%2 change=%3 failure=%4").arg(s.dollarIndex.value, s.dollarIndex.date.toString(Qt::ISODate), s.dollarIndex.changePercent, s.dollarIndexFailure);
        qInfo().noquote() << QString("WORLD_LIVE jgb=%1 date=%2 previous=%3 failure=%4").arg(s.japanYield.tenYear, s.japanYield.date.toString(Qt::ISODate), s.japanYield.previousTenYear, s.japanYieldFailure);
        QVERIFY2(s.dollarIndex.available(), qPrintable(s.dollarIndexFailure));
        QVERIFY2(s.japanYield.available(), qPrintable(s.japanYieldFailure));
    }
};

QTEST_MAIN(WorldMarketsTest)
#include "tst_world_markets.moc"
