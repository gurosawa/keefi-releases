#include "keefi/FxHistory.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/TradingTransport.h"
#include "keefi/XmlInputGuard.h"
#include <QDateTime>
#include <QTimeZone>
#include <QtTest>
#include <cmath>
#include <optional>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000, Hour = 3600000, Day = 86400000;

qint64 utc(int month, int day, int hour, int minute = 0) {
    return QDateTime(QDate(2026, month, day), QTime(hour, minute), QTimeZone::UTC).toMSecsSinceEpoch();
}

const QByteArray Xml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<gesmes:Envelope xmlns:gesmes=\"http://www.gesmes.org/xml/2002-08-01\" xmlns=\"http://www.ecb.int/vocabulary/2002-08-01/eurofxref\">"
    "<gesmes:subject>Reference rates</gesmes:subject><Cube>"
    "<Cube time=\"2026-11-05\"><Cube currency=\"USD\" rate=\"1.1224\"/><Cube currency=\"JPY\" rate=\"177.85\"/><Cube currency=\"KRW\" rate=\"1509.28\"/></Cube>"
    "<Cube time=\"2026-11-04\"><Cube currency=\"USD\" rate=\"1.12\"/><Cube currency=\"KRW\" rate=\"1500\"/></Cube>"
    "<Cube time=\"2026-11-03\"><Cube currency=\"USD\" rate=\"1.11\"/></Cube>"
    "</Cube></gesmes:Envelope>";

FxRate hana(const QString& krw, qint64 posted, qint64 received) {
    UsdKrwPosting posting;
    posting.krwPerUsd = krw;
    posting.postedAtMs = posted;
    posting.postedDate = QDateTime::fromMSecsSinceEpoch(posted, QTimeZone("Asia/Seoul")).date();
    posting.receivedAtMs = received;
    return FxHistory::fromHana(posting);
}

template <class Choice>
std::optional<qint64> chosenKnownAt(const Choice& choice) {
    // Keep the pre-fix regression executable: a missing public evidence field is a test failure, not a compiler failure.
    if constexpr (requires { choice.knownAtMs; }) return choice.knownAtMs;
    return std::nullopt;
}
QByteArray xmlBudgetTag(int attributes, int declarations = 0) {
    QByteArray tag = "<ignored";
    for (int i = 0; i < attributes; ++i) tag += " a" + QByteArray::number(i) + "=\"\"";
    for (int i = 0; i < declarations; ++i)
        tag += " xmlns:p" + QByteArray::number(i) + "=\"urn:keefi:xml-budget:" + QByteArray::number(i) + "\"";
    return tag + "/>";
}

} // namespace

class FxHistoryTests final : public QObject {
    Q_OBJECT
  private slots:
    void xmlPreflightBoundaryCases_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("accepted");
        const auto wrap = [](const QByteArray& body) { return "<root>" + body + "</root>"; };
        const auto wide = [](const QByteArray& text, int width, bool little) {
            QByteArray bytes;
            for (char c : text) {
                if (little) bytes.append(c);
                for (int i = 1; i < width; ++i) bytes.append('\0');
                if (!little) bytes.append(c);
            }
            return bytes;
        };
        QTest::newRow("no-declaration") << QByteArray("<root/>") << true;
        QTest::newRow("utf8-bom-no-declaration") << QByteArray("\xEF\xBB\xBF", 3) + "<root/>" << true;
        QTest::newRow("attributes-64") << xmlBudgetTag(64) << true;
        QTest::newRow("attributes-65") << xmlBudgetTag(65) << false;
        QTest::newRow("namespace-declarations-64") << xmlBudgetTag(0, 64) << true;
        QTest::newRow("namespace-declarations-65") << xmlBudgetTag(0, 65) << false;
        QTest::newRow("mixed-64") << xmlBudgetTag(62, 2) << true;
        QTest::newRow("mixed-65") << xmlBudgetTag(63, 2) << false;
        QTest::newRow("budget-resets-per-element") << wrap(xmlBudgetTag(64) + xmlBudgetTag(64)) << true;
        QTest::newRow("nested-over-budget") << wrap("<ignored>" + xmlBudgetTag(65) + "</ignored>") << false;
        QTest::newRow("quote-delimiters") << QByteArray("<root a=\"one > two = three\" b='one \" two &lt; three'/>") << true;
        QTest::newRow("attribute-whitespace") << QByteArray("<root a = \"x\"\r\n\t b = 'y' />") << true;
        QTest::newRow("utf8-name") << QByteArray("<root \xC3\xA9='ok'/>") << true;
        QTest::newRow("comment-decoy") << wrap("<!--" + xmlBudgetTag(65) + "-->") << true;
        QTest::newRow("cdata-decoy") << wrap("<![CDATA[" + xmlBudgetTag(65) + "]]>") << true;
        QTest::newRow("pi-decoy") << wrap("<?probe " + xmlBudgetTag(65) + "?>") << true;
        QTest::newRow("after-comment") << wrap("<!--done-->" + xmlBudgetTag(65)) << false;
        QTest::newRow("after-cdata") << wrap("<![CDATA[done]]>" + xmlBudgetTag(65)) << false;
        QTest::newRow("after-pi") << wrap("<?probe done?>" + xmlBudgetTag(65)) << false;
        QTest::newRow("pi-quotes-do-not-hide-terminator") << wrap("<?probe value='?>" + xmlBudgetTag(65)) << false;
        QTest::newRow("xml-stylesheet-is-not-declaration") << QByteArray("<?xml-stylesheet encoding='UTF-16'?><root/>") << true;
        QTest::newRow("generic-pi-encoding-is-not-declaration") << QByteArray("<?probe encoding='UTF-32'?><root/>") << true;
        QTest::newRow("declaration-whitespace-case") << QByteArray("<?xml version = '1.0'\n encoding = \"uTf-8\" standalone = 'yes' ?><root/>") << true;
        QTest::newRow("bom-and-declaration") << QByteArray("\xEF\xBB\xBF", 3) + "<?xml version='1.0' encoding='UTF-8'?><root/>" << true;
        QTest::newRow("declared-utf8-alias") << QByteArray("<?xml version='1.0' encoding='UTF8'?><root/>") << true;
        QTest::newRow("declared-ascii") << QByteArray("<?xml version='1.0' encoding='ASCII'?><root/>") << true;
        QTest::newRow("declared-us-ascii") << QByteArray("<?xml version='1.0' encoding='US-ASCII'?><root/>") << true;
        QTest::newRow("declared-iso-latin1") << QByteArray("<?xml version='1.0' encoding='ISO-8859-1'?><root/>") << true;
        QTest::newRow("declared-iso-alias") << QByteArray("<?xml version='1.0' encoding='ISO8859-1'?><root/>") << true;
        QTest::newRow("declared-latin1") << QByteArray("<?xml version='1.0' encoding='Latin1'?><root/>") << true;
        QTest::newRow("declared-latin1-alias") << QByteArray("<?xml version='1.0' encoding='Latin-1'?><root/>") << true;
        QTest::newRow("declared-utf16") << QByteArray("<?xml version='1.0' encoding='UTF-16'?><root/>") << false;
        QTest::newRow("declared-utf32") << QByteArray("<?xml version='1.0' encoding='UTF-32'?><root/>") << false;
        QTest::newRow("declared-stateful-codec") << QByteArray("<?xml version='1.0' encoding='ISO-2022-JP'?><root/>") << false;
        QTest::newRow("declared-system-codec") << QByteArray("<?xml version='1.0' encoding='System'?><root/>") << false;
        QTest::newRow("utf16le-no-bom") << wide("<?xml version='1.0' encoding='UTF-16'?><root/>", 2, true) << false;
        QTest::newRow("utf16be-with-bom") << QByteArray("\xFE\xFF", 2) + wide("<?xml version='1.0' encoding='UTF-16'?><root/>", 2, false) << false;
        QTest::newRow("utf32le-no-bom") << wide("<?xml version='1.0' encoding='UTF-32'?><root/>", 4, true) << false;
        QTest::newRow("utf32be-with-bom") << QByteArray("\x00\x00\xFE\xFF", 4) + wide("<root/>", 4, false) << false;
        QByteArray nul = "<root><!--";
        nul.append('\0');
        nul += "--></root>";
        QTest::newRow("nul-inside-comment") << nul << false;
        QTest::newRow("doctype-default-attributes") << QByteArray("<!DOCTYPE root [<!ATTLIST root injected CDATA 'value'>]><root/>") << false;
        QTest::newRow("doctype-internal-entity") << QByteArray("<!DOCTYPE root [<!ENTITY x 'value'>]><root>&x;</root>") << false;
        QTest::newRow("unknown-declaration") << QByteArray("<!UNKNOWN><root/>") << false;
        QTest::newRow("unfinished-tag") << QByteArray("<root") << false;
        QTest::newRow("unfinished-quote") << QByteArray("<root a='x") << false;
        QTest::newRow("unquoted-value") << QByteArray("<root a=x/>") << false;
        QTest::newRow("missing-equals") << QByteArray("<root a 'x'/>") << false;
        QTest::newRow("missing-attribute-space") << QByteArray("<root a='x'b='y'/>") << false;
        QTest::newRow("literal-less-than-in-value") << QByteArray("<root a='x<y'/>") << false;
        QTest::newRow("unfinished-comment") << QByteArray("<root><!--") << false;
        QTest::newRow("unfinished-cdata") << QByteArray("<root><![CDATA[") << false;
        QTest::newRow("unfinished-pi") << QByteArray("<root><?probe") << false;
        QTest::newRow("end-tag-attribute") << QByteArray("<root></root a='x'>") << false;
        QTest::newRow("xml-declaration-after-whitespace") << QByteArray(" <?xml version='1.0'?><root/>") << false;
        QTest::newRow("duplicate-xml-declaration") << QByteArray("<?xml version='1.0'?><?xml version='1.0'?><root/>") << false;
        QTest::newRow("uppercase-xml-target") << QByteArray("<?XML version='1.0'?><root/>") << false;
        QTest::newRow("duplicate-encoding") << QByteArray("<?xml version='1.0' encoding='UTF-8' encoding='UTF-16'?><root/>") << false;
        QTest::newRow("encoding-before-version") << QByteArray("<?xml encoding='UTF-8' version='1.0'?><root/>") << false;
        QTest::newRow("encoding-after-standalone") << QByteArray("<?xml version='1.0' standalone='yes' encoding='UTF-8'?><root/>") << false;
        QTest::newRow("empty-input") << QByteArray{} << false;
    }

    void xmlPreflightBoundaryCases() {
        QFETCH(QByteArray, body);
        QFETCH(bool, accepted);
        // This guard is an admission budget; Qt remains the XML validator.
        QCOMPARE(xmlAttributeBudgetAllows(body), accepted);
    }

    void xmlAttributeBudgetKeepsNormalData() {
        const auto received = utc(11, 6, 9);
        const auto expected = FxHistory::parseEcb(Xml, received);
        QCOMPARE(expected.size(), 2);
        auto body = Xml;
        const auto extra = xmlBudgetTag(64) + xmlBudgetTag(62, 2) +
            QByteArray("<ignored a=\"one > two = three\" b='one \" two &lt; three'/>");
        body.replace("</gesmes:Envelope>", extra + "</gesmes:Envelope>");
        QString reason;
        const auto actual = FxHistory::parseEcb(body, received, &reason);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(actual.size(), expected.size());
        for (qsizetype i = 0; i < actual.size(); ++i) {
            QCOMPARE(actual[i].date, expected[i].date);
            QCOMPARE(actual[i].value, expected[i].value);
            QCOMPARE(actual[i].krwPerUsd, expected[i].krwPerUsd);
            QCOMPARE(actual[i].postedAtMs, expected[i].postedAtMs);
            QCOMPARE(actual[i].knownAtMs, expected[i].knownAtMs);
            QCOMPARE(actual[i].receivedAtMs, expected[i].receivedAtMs);
            QCOMPARE(actual[i].source, expected[i].source);
        }
    }

    void qtStillRejectsDuplicateExpandedAttributes() {
        const QByteArray extra = "<ignored xmlns:p='urn:synthetic' xmlns:q='urn:synthetic' p:a='1' q:a='2'/>";
        QVERIFY(xmlAttributeBudgetAllows(extra));
        auto body = Xml;
        body.replace("</gesmes:Envelope>", extra + "</gesmes:Envelope>");
        QString reason;
        QVERIFY(FxHistory::parseEcb(body, utc(11, 6, 9), &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("ECB XML 형식 확인 필요"));
    }

    void ecbKeepsItsExistingUtf8Boundary() {
        auto body = Xml;
        body.replace("encoding=\"UTF-8\"", "encoding=\"ISO-8859-1\"");
        body.replace("</gesmes:Envelope>", QByteArray("<ignored>\xE9</ignored></gesmes:Envelope>"));
        QString reason;
        QVERIFY(FxHistory::parseEcb(body, utc(11, 6, 9), &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("ECB 응답 형식 확인 필요"));
    }

    void rejectsXmlAttributeFloodBeforeParsing() {
        QByteArray extra = "<ignored";
        for (int i = 0; i < 65; ++i) extra += " a" + QByteArray::number(i) + "=\"\"";
        extra += "/>";
        auto body = Xml;
        body.replace("</gesmes:Envelope>", extra + "</gesmes:Envelope>");
        QString reason;
        QVERIFY(FxHistory::parseEcb(body, utc(11, 6, 9), &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("ECB XML 형식 확인 필요"));
    }

    void choicesPreserveWhenTheirPostingBecameKnown() {
        const qint64 end = utc(11, 5, 10);
        const auto ecb = FxHistory::parseEcb(Xml, utc(11, 6, 9));
        const auto received = utc(11, 5, 9, 55);
        const auto prior = hana("1351.40", utc(11, 5, 9, 50), received);
        const auto future = hana("1360.00", utc(11, 5, 9, 59), end + 1);
        const auto intraday = FxHistory::choose(end - 1, 15 * Minute, ecb, {prior, future});
        QCOMPARE(intraday.source, QStringLiteral("hana"));
        QCOMPARE(intraday.krwPerUsd, 1351.40);
        const auto hanaKnown = chosenKnownAt(intraday);
        QVERIFY2(hanaKnown.has_value(), "FxChoice must retain knownAtMs for the selected posting");
        QCOMPARE(*hanaKnown, received); // First receipt, not the earlier posting or the later excluded receipt.

        const auto approximate = FxHistory::choose(end - 1, 15 * Minute, ecb, {future});
        QVERIFY(approximate.approximate);
        const auto approximateKnown = chosenKnownAt(approximate);
        QVERIFY(approximateKnown.has_value());
        QCOMPARE(*approximateKnown, utc(11, 4, 16)); // ECB's published availability, not its later fetch time.
        const auto daily = FxHistory::choose(utc(11, 6, 0) - 1, Day, ecb, {prior});
        QVERIFY(daily.available);
        const auto dailyKnown = chosenKnownAt(daily);
        QVERIFY(dailyKnown.has_value());
        QCOMPARE(*dailyKnown, utc(11, 5, 16));
        const auto unavailable = FxHistory::choose(end - 1, 15 * Minute, {}, {});
        QVERIFY(!unavailable.available);
        const auto unavailableKnown = chosenKnownAt(unavailable);
        QVERIFY(unavailableKnown.has_value());
        QCOMPARE(*unavailableKnown, qint64(0));
    }

    void requestIsTheExactEcbHistoryRead() {
        const auto request = FxHistory::ecbHistoryRequest();
        QCOMPARE(request.url.toString(), QStringLiteral("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-hist.xml"));
        QVERIFY(FxHistory::isRequest(request));
        QVERIFY(NetworkTradingTransport::allowed(request));
        auto other = request;
        other.url = QUrl(QStringLiteral("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-hist.zip"));
        QVERIFY(!FxHistory::isRequest(other));
    }

    void parsesWonPerDollarFromEuroRates() {
        QString reason;
        const auto rates = FxHistory::parseEcb(Xml, utc(11, 6, 9), &reason);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(rates.size(), 2); // 11-03 has no KRW.
        QCOMPARE(rates[0].date, QDate(2026, 11, 4));
        QCOMPARE(rates[1].date, QDate(2026, 11, 5));
        QVERIFY(std::abs(rates[1].value - 1509.28 / 1.1224) < 1e-9);
        QCOMPARE(rates[1].krwPerUsd, QString::number(1509.28 / 1.1224, 'f', 4));
        QCOMPARE(rates[1].knownAtMs, utc(11, 5, 16)); // 17:00 CET is 16:00 UTC in November.
        QCOMPARE(FxHistory::ecbKnownAtMs(QDate(2026, 7, 1)), QDateTime(QDate(2026, 7, 1), QTime(15, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QVERIFY(FxHistory::parseEcb("<!DOCTYPE x><Cube/>", utc(11, 6, 9), &reason).isEmpty());
        QVERIFY(FxHistory::parseEcb(Xml, 0, &reason).isEmpty());
    }

    void dailyBarsUseEcbAndMarkThePreviousPosting() {
        const auto ecb = FxHistory::parseEcb(Xml, utc(11, 6, 9));
        auto choice = FxHistory::choose(utc(11, 5, 0) + Day - 1, Day, ecb, {});
        QVERIFY(choice.available);
        QCOMPARE(choice.label, QStringLiteral("ECB 일별"));
        QCOMPARE(choice.date, QDate(2026, 11, 5));
        QVERIFY(!choice.previous);
        choice = FxHistory::choose(utc(11, 7, 0) + Day - 1, Day, ecb, {}); // Saturday
        QCOMPARE(choice.date, QDate(2026, 11, 5));
        QVERIFY(choice.previous);
        choice = FxHistory::choose(utc(11, 4, 12) + 12 * Hour - 1, 12 * Hour, ecb, {});
        QVERIFY2(choice.approximate, "A half-day bar is not daily");
        QCOMPARE(choice.date, QDate(2026, 11, 4));
        choice = FxHistory::choose(utc(11, 4, 0) + 12 * Hour - 1, 12 * Hour, ecb, {});
        QVERIFY2(!choice.available, "11-04 is known only at 16:00 UTC, after this bar");
        choice = FxHistory::choose(utc(11, 1, 0) + Day - 1, Day, ecb, {});
        QVERIFY(!choice.available);
        QCOMPARE(choice.label, QStringLiteral("환산 불가"));
    }

    void hanaReceiptAtBarEndIsKnownButOneMillisecondLaterIsNot() {
        const qint64 end = utc(11, 5, 10);
        const auto ecb = FxHistory::parseEcb(Xml, utc(11, 6, 9));
        const auto prior = hana("1340", end - 2 * Hour, end - 2 * Hour + Minute);
        const auto atEnd = hana("1350", end - Hour, end);
        const auto afterEnd = hana("1350", end - Hour, end + 1);
        QCOMPARE(atEnd.postedAtMs, end - Hour);
        QCOMPARE(atEnd.knownAtMs, end);
        QCOMPARE(afterEnd.knownAtMs, end + 1);

        const auto beforeAppend = FxHistory::choose(end - 1, 15 * Minute, ecb, {prior});
        const auto afterAppend = FxHistory::choose(end - 1, 15 * Minute, ecb, {prior, afterEnd});
        QCOMPARE(beforeAppend.source, QStringLiteral("hana"));
        QCOMPARE(afterAppend.source, beforeAppend.source);
        QCOMPARE(afterAppend.krwPerUsd, beforeAppend.krwPerUsd);
        QCOMPARE(afterAppend.krwPerUsd, 1340.0);
        const auto boundary = FxHistory::choose(end - 1, 15 * Minute, ecb, {prior, atEnd});
        QCOMPARE(boundary.source, QStringLiteral("hana"));
        QCOMPARE(boundary.krwPerUsd, 1350.0);

        // ECB is reconstructed using its published availability convention;
        // the later fetch time is not treated as a historical Hana receipt.
        QVERIFY(!ecb.isEmpty());
        QVERIFY(ecb.first().receivedAtMs > end);
        const auto reconstructed = FxHistory::choose(end - 1, 15 * Minute, ecb, {afterEnd});
        QCOMPARE(reconstructed.source, QStringLiteral("ecb"));
        QCOMPARE(reconstructed.date, QDate(2026, 11, 4));
        QVERIFY(reconstructed.approximate);
        const auto daily = FxHistory::choose(utc(11, 6, 0) - 1, Day, ecb, {atEnd});
        QCOMPARE(daily.source, QStringLiteral("ecb"));
        QCOMPARE(daily.date, QDate(2026, 11, 5));
        QVERIFY(!daily.approximate);
    }

    void hanaAgeThresholdsUseExactSixAndTwentyFourHourBoundaries() {
        const qint64 end = utc(11, 5, 10);
        const auto ecb = FxHistory::parseEcb(Xml, utc(11, 6, 9));
        const auto chooseAtAge = [&](qint64 age) {
            const qint64 posted = end - age;
            return FxHistory::choose(end - 1, 15 * Minute, ecb,
                                     {hana("1345", posted, posted + Minute)});
        };
        auto choice = chooseAtAge(6 * Hour);
        QCOMPARE(choice.source, QStringLiteral("hana"));
        QVERIFY(!choice.previous);
        choice = chooseAtAge(6 * Hour + 1);
        QCOMPARE(choice.source, QStringLiteral("hana"));
        QVERIFY(choice.previous);
        choice = chooseAtAge(24 * Hour);
        QCOMPARE(choice.source, QStringLiteral("hana"));
        QCOMPARE(choice.krwPerUsd, 1345.0);
        QVERIFY(choice.previous);
        choice = chooseAtAge(24 * Hour + 1);
        QCOMPARE(choice.source, QStringLiteral("ecb"));
        QVERIFY(choice.approximate);
    }

    void intradayPrefersHanaThenEcbApproximationWithoutLookahead() {
        const auto ecb = FxHistory::parseEcb(Xml, utc(11, 6, 9));
        const auto barClose = utc(11, 5, 10) - 1; // 15-minute bar ending 10:00 UTC
        QList<FxRate> postings{hana("1351.40", utc(11, 5, 9, 50), utc(11, 5, 9, 55))};
        auto choice = FxHistory::choose(barClose, 15 * Minute, ecb, postings);
        QCOMPARE(choice.label, QStringLiteral("하나은행 고시"));
        QCOMPARE(choice.krwPerUsd, 1351.40);
        QVERIFY(!choice.previous);

        postings = {hana("1351.40", utc(11, 5, 9, 50), utc(11, 5, 10, 1))}; // Known only after the bar.
        choice = FxHistory::choose(barClose, 15 * Minute, ecb, postings);
        QCOMPARE(choice.label, QStringLiteral("ECB 일별(근사)"));
        QVERIFY(choice.approximate);
        QCOMPARE(choice.date, QDate(2026, 11, 4)); // 11-05 is known at 16:00 UTC, after this bar.
        QVERIFY(choice.previous);

        postings = {hana("1340", utc(11, 4, 7), utc(11, 4, 7, 1))}; // Older than a day.
        choice = FxHistory::choose(barClose, 15 * Minute, ecb, postings);
        QCOMPARE(choice.label, QStringLiteral("ECB 일별(근사)"));

        postings = {hana("1345", utc(11, 5, 1), utc(11, 5, 1, 1))}; // Nine hours old.
        choice = FxHistory::choose(barClose, 15 * Minute, ecb, postings);
        QCOMPARE(choice.label, QStringLiteral("하나은행 고시"));
        QVERIFY(choice.previous);

        choice = FxHistory::choose(barClose, 15 * Minute, {}, {});
        QVERIFY(!choice.available);
        QVERIFY(!FxHistory::fromHana(UsdKrwPosting{}).value);
    }
};

QTEST_GUILESS_MAIN(FxHistoryTests)
#include "tst_fx_history.moc"
