#include "keefi/BuildPolicy.h"
#include "keefi/KisMasterCatalog.h"
#include "keefi/TradingTransport.h"
#include <QTest>
#include "fixtures/KisMasterFixture.h"
using namespace keefi;
using namespace keefi::test;

class KisCatalogTest : public QObject {
    Q_OBJECT
  private slots:
    void transportSeparatesPublicCatalogAndPrivateApis() {
        RequestSpec r; r.operation = Operation::Markets; r.url = QUrl(KisMasterCatalog::sourceUrl("KOSPI"));
        QVERIFY(NetworkTradingTransport::allowed(r));
        r.headers["appkey"] = "fake-only"; QVERIFY(!NetworkTradingTransport::allowed(r)); r.headers.clear();
        r.authenticated = true; QVERIFY(!NetworkTradingTransport::allowed(r)); r.authenticated = false;
        r.body = "x"; QVERIFY(!NetworkTradingTransport::allowed(r)); r.body.clear();
        r.url.setQuery("anything=x"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url = QUrl("https://new.real.download.dws.co.kr/common/master/other.zip"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.operation = Operation::Authenticate; r.method = "POST"; r.authenticated = true;
        r.url = QUrl("https://openapi.koreainvestment.com:9443/oauth2/tokenP"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setPort(443); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url = QUrl("https://openapivts.koreainvestment.com:29443/oauth2/tokenP"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setPort(9443); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url = QUrl("https://openapi.koreainvestment.com:9443/uapi/domestic-stock/v1/trading/order-cash");
        r.operation = Operation::PlaceOrder; QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        r.operation = Operation::TestOrder; QVERIFY(!NetworkTradingTransport::allowed(r));
        r.operation = Operation::OpenOrders; r.method = "GET";
        r.url = QUrl("https://openapi.koreainvestment.com:9443/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setHost("openapivts.koreainvestment.com");r.url.setPort(29443);QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setPath("/uapi/domestic-stock/v1/trading/inquire-daily-ccld");QVERIFY(!NetworkTradingTransport::allowed(r));
        r.operation = Operation::TradingSchedule; r.url.setPath("/uapi/domestic-stock/v1/quotations/chk-holiday"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setHost("openapi.koreainvestment.com");r.url.setPort(9443);QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setHost("openapi.koreainvestment.com.attacker.invalid");QVERIFY(!NetworkTradingTransport::allowed(r));
    }
    void marketsAndEncoding_data() {
        QTest::addColumn<QString>("market");
        for (const auto* m : {"KOSPI", "KOSDAQ", "KONEX"}) QTest::newRow(m) << QString(m);
    }
    void marketsAndEncoding() {
        QFETCH(QString, market); QString error;
        const auto parsed = KisMasterCatalog::parseZip(zip(row(market), market.toLower().toLatin1() + "_code.mst", true), market, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed[0].symbol, "005930"); QCOMPARE(parsed[0].name, QString::fromUtf8("가짜한글종목"));
        QCOMPARE(parsed[0].market, market); QCOMPARE(parsed[0].quoteAsset, "KRW"); QVERIFY(parsed[0].tradable);
        QVERIFY(parsed[0].englishName.isEmpty());
    }
    void catalogueFlagsAreNotAccountPermissions() {
        const auto raw = row("KOSPI", "0000D0", "ST") + row("KOSPI", "Q500067", "EN") +
            row("KOSPI", "123456", "ST", "YNN00") + row("KOSPI", "234567", "ST", "NYN00") +
            row("KOSPI", "F70100034", "BC") + row("KOSPI", "345678", "ST", "NNY02");
        QString error; const auto parsed = KisMasterCatalog::parseZip(zip(raw), "KOSPI", &error);
        QCOMPARE(parsed.size(), 6); QVERIFY(error.isEmpty()); QVERIFY(parsed[0].tradable); QVERIFY(parsed[1].tradable);
        QVERIFY(!parsed[2].tradable); QCOMPARE(parsed[2].status, "HALTED"); QVERIFY(!parsed[3].tradable);
        QVERIFY(!parsed[4].tradable); QVERIFY(parsed[4].warning.contains("미구현"));
        QVERIFY(parsed[5].warning.contains("관리종목")); QVERIFY(parsed[5].warning.contains("02"));
    }
    void classificationPreservesOfficialFields_data() {
        QTest::addColumn<QString>("market");
        QTest::newRow("kospi") << QString("KOSPI");
        QTest::newRow("kosdaq") << QString("KOSDAQ");
    }
    void classificationPreservesOfficialFields() {
        QFETCH(QString, market);
        const int tailSize = market == "KOSPI" ? 227 : 221;
        const int preferredOffset = market == "KOSPI" ? 158 : 153;
        const int etpOffset = market == "KOSPI" ? 22 : 18;
        const auto classified = [&](const QByteArray& symbol, const QByteArray& group, char preferred, char etp) {
            auto bytes = row(market, symbol, group);
            const auto start = bytes.size() - 1 - tailSize;
            bytes[start + preferredOffset] = preferred;
            bytes[start + etpOffset] = etp;
            return bytes;
        };
        const auto raw = classified("005930", "ST", '0', ' ') + classified("005935", "ST", '1', ' ') +
            classified("069500", "EF", '0', '2') + classified("123456", "ST", '9', '8');
        QString error;
        const auto rows = KisMasterCatalog::parseZip(zip(raw, market.toLower().toLatin1() + "_code.mst"), market, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].preferredClass, "0"); QCOMPARE(rows[0].etpClass, " "); QCOMPARE(rows[0].securityType, "ST");
        QCOMPARE(rows[1].preferredClass, "1"); QCOMPARE(rows[2].etpClass, "2"); QCOMPARE(rows[2].securityType, "EF");
        QCOMPARE(rows[3].preferredClass, "9"); QCOMPARE(rows[3].etpClass, "8");
        // This parser does not reinterpret KIS's existing ST/EF/EN eligibility.
        // A provider needing a common-stock-only policy must check these fields.
        for (const auto& instrument : rows) QVERIFY(instrument.tradable);
        const auto konex = KisMasterCatalog::parseZip(zip(row("KONEX"), "konex_code.mst"), "KONEX", &error);
        QCOMPARE(konex.size(), 1); QVERIFY(konex[0].preferredClass.isEmpty()); QVERIFY(konex[0].etpClass.isEmpty());
    }
    void malformed_data() {
        QTest::addColumn<QByteArray>("bytes");
        const auto valid = zip(row("KOSPI"));
        QTest::newRow("bad-crc") << [&]{auto b=valid;b[45]^=1;return b;}();
        QTest::newRow("truncated") << valid.first(valid.size()-1);
        QTest::newRow("trailing") << valid + "payload";
        QTest::newRow("wrong-entry") << zip(row("KOSPI"), "kosdaq_code.mst");
        QTest::newRow("path-traversal") << zip(row("KOSPI"), "../kospi_code.mst");
        QTest::newRow("duplicate-symbol") << zip(row("KOSPI") + row("KOSPI"));
        QTest::newRow("invalid-cp949") << zip([&]{auto r=row("KOSPI");r[21]='\xff';return r;}());
        QTest::newRow("invalid-control") << zip([&]{auto r=row("KOSPI");r[21]='\x01';return r;}());
        QTest::newRow("invalid-symbol") << zip(row("KOSPI", "ab/cde"));
        QTest::newRow("invalid-flags") << zip(row("KOSPI", "005930", "ST", "?NN00"));
        QTest::newRow("unterminated-row") << zip(row("KOSPI").chopped(1));
        QTest::newRow("extra-entry-count") << [&]{auto b=valid;b[b.size()-12]=2;return b;}();
        QTest::newRow("expansion-cap") << [&]{auto b=valid;replace32(b,b.indexOf(QByteArray("PK\x01\x02",4))+24,17*1024*1024);return b;}();
        QTest::newRow("encrypted") << [&]{auto b=valid;b[6]=1;return b;}();
        QTest::newRow("symlink") << [&]{auto b=valid;replace32(b,b.indexOf(QByteArray("PK\x01\x02",4))+38,0120777u<<16);return b;}();
    }
    void malformed() { QFETCH(QByteArray, bytes); QString error; QVERIFY(KisMasterCatalog::parseZip(bytes,"KOSPI",&error).isEmpty()); QVERIFY(!error.isEmpty()); }
    void unknownMarket() { QString error; QVERIFY(KisMasterCatalog::sourceUrl("../x").isEmpty()); QVERIFY(KisMasterCatalog::parseZip(zip(row("KOSPI")),"OTHER",&error).isEmpty()); QVERIFY(!error.isEmpty()); }
};
QTEST_GUILESS_MAIN(KisCatalogTest)
#include "tst_kis_catalog.moc"
