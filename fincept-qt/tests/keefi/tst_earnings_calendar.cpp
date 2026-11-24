// Synthetic KIND/Nasdaq schedules and a fake transport only. No network, account or profile.
#include "PublicViewPreview.h"
#include "keefi/EarningsCalendar.h"
#include "keefi/EarningsCalendarView.h"
#include "keefi/TradingTransport.h"
#include "keefi/UsabilityPresentation.h"
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>
using namespace keefi;
namespace {
const QTimeZone Kst("Asia/Seoul"), NewYork("America/New_York");
QByteArray kindRow(const QByteArray& code, const QByteArray& name, const QByteArray& title, const QDate& date, const QByteArray& time) {
    return "<tr class=\"first\" ><td class=\"first txc\">1</td><td><img src='/images/common/icn_t_yu.gif' class='vmiddle' alt='유가증권' />"
        " <a href=\"#\" title=\"" + name + "\" onclick=\"companysummary_open('" + code + "'); return false;\">" + name + "</a></td>"
        "<td><a href=\"#\" onclick=\"fnDetailView('1'); return false;\">" + title + "</a></td><td>-</td>"
        "<td class=\"txc\">" + date.toString(Qt::ISODate).toLatin1() + "</td><td class=\"txc\"> " + time + " </td></tr>\n";
}
QByteArray kindPage(const QByteArray& rows, int total) {
    return "<section class=\"scrarea type-00\"><table class=\"list type-00 mt10\"><thead><tr><th>번호</th><th>회사명</th></tr></thead><tbody>\n" + rows +
        "</tbody></table></section><section class=\"paging-group\"><div class=\"info type-00\"> 전체 <em>" + QByteArray::number(total) + "</em>건 : <strong>1</strong>/1</div></section>";
}
QJsonObject nasdaqRow(const QString& symbol, const QString& time, const QString& cap, const QString& eps = "$1.25", const QString& ests = "12") {
    return {{"symbol", symbol}, {"name", symbol + " Synthetic Inc."}, {"time", time}, {"marketCap", cap}, {"fiscalQuarterEnding", "Sep/2026"},
            {"epsForecast", eps}, {"noOfEsts", ests}, {"lastYearRptDt", "10/30/2025"}, {"lastYearEPS", "$1.00"}};
}
QByteArray nasdaqPage(const QDate& date, const QJsonArray& rows) {
    const QJsonObject data{{"asOf", QLocale(QLocale::English).toString(date, "ddd, MMM d, yyyy")}, {"headers", QJsonObject{}}, {"rows", rows}};
    return QJsonDocument(QJsonObject{{"data", data}, {"message", QJsonValue()}, {"status", QJsonObject{{"rCode", 200}}}}).toJson(QJsonDocument::Compact);
}
class FakeTransport final : public TradingTransport {
  public:
    QList<RequestSpec> sent; quint64 token = 0; int aborts = 0;
    void send(quint64 next, const RequestSpec& request) override { token = next; sent.append(request); }
    void abort() override { ++aborts; }
    void answer(int status, const QByteArray& body = {}) { HttpResult r; r.status = status; r.body = body; emit finished(token, r); }
};
QDate formDate(const RequestSpec& r, const char* name) { return QDate::fromString(QUrlQuery(QString::fromLatin1(r.body)).queryItemValue(name), Qt::ISODate); }
QDate nasdaqDate(const RequestSpec& r) { return QDate::fromString(QUrlQuery(r.url).queryItemValue("date"), Qt::ISODate); }
}
class EarningsCalendarTest final : public QObject {
    Q_OBJECT
  private slots:
    void kindRowsKeepPublishedKstTimesAndUnknownTimes() {
        const QDate from(2026, 10, 28), to(2026, 11, 4);
        const auto page = kindPage(kindRow("00066", "가상하이닉스", "2026년 3분기 경영실적 발표 &amp; Q&#38;A", QDate(2026, 10, 29), "09:00") +
                                   kindRow("00593", "가상전자", "To discuss FY 2026 Q3 Financial Results", QDate(2026, 10, 30), "--:--") +
                                   kindRow("27221", "가상바이오", "당사 사업현황 및 성장전략 설명", QDate(2026, 11, 2), "14:00"), 3);
        QList<EarningsEvent> rows; int total = 0;
        QVERIFY(EarningsCalendar::parseKind(page, from, to, &rows, &total)); QCOMPARE(total, 3); QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].code, QString("00066")); QCOMPARE(rows[0].title, QStringLiteral("2026년 3분기 경영실적 발표 & Q&A")); // Entities decoded once. QCOMPARE(rows[0].name, QStringLiteral("가상하이닉스")); QCOMPARE(rows[0].timing, EarningsTiming::Scheduled);
        QCOMPARE(rows[0].startsAtMs, QDateTime(QDate(2026, 10, 29), QTime(9, 0), Kst).toMSecsSinceEpoch());
        QCOMPARE(rows[1].timing, EarningsTiming::NotSupplied); QCOMPARE(rows[1].startsAtMs, QDateTime(QDate(2026, 10, 30), QTime(0, 0), Kst).toMSecsSinceEpoch());
        QVERIFY(EarningsCalendar::earningsTitle(rows[0].title)); QVERIFY(EarningsCalendar::earningsTitle(rows[1].title)); QVERIFY(!EarningsCalendar::earningsTitle(rows[2].title));
        // An empty search is a valid empty week.
        QVERIFY(EarningsCalendar::parseKind(kindPage("<tr><td colspan=\"6\">조회된 결과값이 없습니다.</td></tr>", 0), from, to, &rows, &total));
        QVERIFY(rows.isEmpty()); QCOMPARE(total, 0);
    }
    void kindRejectsAnyUnreadableRow_data() {
        QTest::addColumn<QByteArray>("page");
        const QDate day(2026, 10, 29); const auto good = kindRow("00066", "가상하이닉스", "3분기 실적", day, "09:00");
        QTest::newRow("no-total") << QByteArray("<tbody>" + good + "</tbody>");
        QTest::newRow("no-body") << kindPage({}, 1).replace("<tbody>", "<tbodx>");
        QTest::newRow("bad-code") << kindPage(good + kindRow("6606", "가상", "실적", day, "09:00"), 2);
        QTest::newRow("out-of-range") << kindPage(good + kindRow("00593", "가상전자", "실적", QDate(2026, 12, 1), "09:00"), 2);
        QTest::newRow("bad-time") << kindPage(good + kindRow("00593", "가상전자", "실적", day, "25:00"), 2);
        QTest::newRow("short-row") << kindPage(good + "<tr><td>2</td><td>가상</td></tr>", 2);
        QTest::newRow("total-without-rows") << kindPage({}, 4);
        QTest::newRow("not-utf8") << kindPage(good + "\xff", 1);
    }
    void kindRejectsAnyUnreadableRow() {
        QFETCH(QByteArray, page); QList<EarningsEvent> rows{EarningsEvent{}}; int total = 7;
        QVERIFY(!EarningsCalendar::parseKind(page, QDate(2026, 10, 28), QDate(2026, 11, 4), &rows, &total));
        QCOMPARE(rows.size(), 1); QCOMPARE(total, 7); // Outputs stay untouched on failure.
    }
    void nasdaqRowsKeepOpenCloseMarkersAndRejectChangedPages() {
        const QDate date(2026, 10, 29);
        const QJsonArray rows{nasdaqRow("AMZN", "time-after-hours", "$2,686,762,880,000"), nasdaqRow("LLY", "time-pre-market", "$1,204,127,530,000", "($0.12)"),
                              nasdaqRow("SYN", "time-not-supplied", "", "", "N/A"), nasdaqRow("WHOLE", "time-pre-market", "$1,000", "$3")};
        QList<EarningsEvent> parsed; QVERIFY(EarningsCalendar::parseNasdaq(nasdaqPage(date, rows), date, &parsed)); QCOMPARE(parsed.size(), 4);
        QCOMPARE(parsed[3].epsForecast, QString("$3")); // Whole-dollar forecasts arrive without decimals.
        QCOMPARE(parsed[0].timing, EarningsTiming::AfterHours); QCOMPARE(parsed[0].marketCapUsd, 2686762880000.0); QCOMPARE(parsed[0].epsForecast, QString("$1.25"));
        QCOMPARE(parsed[1].timing, EarningsTiming::PreMarket); QCOMPARE(parsed[1].epsForecast, QString("($0.12)"));
        QCOMPARE(parsed[2].timing, EarningsTiming::NotSupplied); QCOMPARE(parsed[2].marketCapUsd, 0.0); QVERIFY(parsed[2].estimates.isEmpty());
        QVERIFY(parsed[1].startsAtMs < parsed[2].startsAtMs && parsed[2].startsAtMs < parsed[0].startsAtMs); // Ordering only.
        const QByteArray empty = R"({"data":null,"message":"No data","status":{"rCode":200}})";
        QVERIFY(EarningsCalendar::parseNasdaq(empty, date, &parsed)); QVERIFY(parsed.isEmpty());
        parsed = {EarningsEvent{}};
        QVERIFY(!EarningsCalendar::parseNasdaq(nasdaqPage(date.addDays(1), rows), date, &parsed)); // asOf names another day.
        QVERIFY(!EarningsCalendar::parseNasdaq(nasdaqPage(date, {nasdaqRow("AMZN", "time-during-market", "$1")}), date, &parsed));
        QVERIFY(!EarningsCalendar::parseNasdaq(nasdaqPage(date, {nasdaqRow("amzn", "time-pre-market", "$1")}), date, &parsed));
        QVERIFY(!EarningsCalendar::parseNasdaq(nasdaqPage(date, {nasdaqRow("AMZN", "time-pre-market", "$1"), nasdaqRow("AMZN", "time-pre-market", "$1")}), date, &parsed));
        QVERIFY(!EarningsCalendar::parseNasdaq(nasdaqPage(date, {nasdaqRow("AMZN", "time-pre-market", "1 trillion")}), date, &parsed));
        QVERIFY(!EarningsCalendar::parseNasdaq(nasdaqPage(date, {nasdaqRow("AMZN", "time-pre-market", "$1", "$1.2.3")}), date, &parsed));
        QVERIFY(!EarningsCalendar::parseNasdaq(R"({"data":null,"status":{"rCode":400}})", date, &parsed));
        QCOMPARE(parsed.size(), 1);
    }
    void onlyTheExactPublicRequestsAreAllowed() {
        const QDate from(2026, 10, 28), to(2026, 11, 4);
        const auto kind = EarningsCalendar::kindRequest(from, to, 2);
        QVERIFY(EarningsCalendar::isRequest(kind)); QVERIFY(NetworkTradingTransport::allowed(kind)); QCOMPARE(kind.method, QByteArray("POST"));
        QVERIFY(kind.body.contains("marketType=1")); QVERIFY(!kind.authenticated);
        for (int fault = 0; fault < 8; ++fault) {
            auto bad = kind;
            if (fault == 0) bad.method = "GET";
            if (fault == 1) bad.authenticated = true;
            if (fault == 2) bad.headers.insert("Authorization", "secret");
            if (fault == 3) bad.body.replace("marketType=1", "marketType=2");
            if (fault == 4) bad.body.append("&account=private");
            if (fault == 5) bad = EarningsCalendar::kindRequest(from, from.addDays(30), 1);
            if (fault == 6) bad = EarningsCalendar::kindRequest(from, to, EarningsCalendar::KindMaxPages + 1);
            if (fault == 7) bad.url.setHost("example.test");
            QVERIFY2(!EarningsCalendar::isRequest(bad), qPrintable(QString::number(fault))); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        const auto us = EarningsCalendar::nasdaqRequest(QDate(2026, 10, 29));
        QVERIFY(EarningsCalendar::isRequest(us)); QVERIFY(NetworkTradingTransport::allowed(us));
        QCOMPARE(us.url, QUrl("https://api.nasdaq.com/api/calendar/earnings?date=2026-10-29"));
        for (int fault = 0; fault < 4; ++fault) {
            auto bad = us;
            if (fault == 0) bad.method = "POST";
            if (fault == 1) bad.url.setQuery("date=2026-10-29&account=private");
            if (fault == 2) bad.url.setPath("/api/calendar/dividends");
            if (fault == 3) bad.body = "{}";
            QVERIFY(!EarningsCalendar::isRequest(bad)); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
    }
    void refreshPagesKindAndReadsNewYorkWeekdaysOnly() {
        // Wednesday 2026-10-28 10:00 KST is Tuesday 2026-10-27 in New York.
        const auto now = QDateTime(QDate(2026, 10, 28), QTime(10, 0), Kst).toMSecsSinceEpoch();
        FakeTransport transport; EarningsCalendar model(&transport, nullptr, [now] { return now; }); model.setActive(true);
        QTRY_COMPARE(transport.sent.size(), 1); QVERIFY(NetworkTradingTransport::allowed(transport.sent[0]));
        QCOMPARE(formDate(transport.sent[0], "fromDate"), QDate(2026, 10, 28)); QCOMPARE(formDate(transport.sent[0], "toDate"), QDate(2026, 11, 4));
        QByteArray first, second;
        for (int i = 0; i < EarningsCalendar::KindPageSize; ++i) first += kindRow(QByteArray::number(10000 + i), "가상" + QByteArray::number(i), "3분기 실적", QDate(2026, 10, 29), "10:00");
        // The same session listed again with an English title on the next page keeps the Korean row.
        second = kindRow("10000", "가상0", "Q3 Results", QDate(2026, 10, 29), "10:00") + kindRow("20000", "가상끝", "결산 실적", QDate(2026, 11, 3), "--:--");
        transport.answer(200, kindPage(first, 102)); QTRY_COMPARE(transport.sent.size(), 2);
        QVERIFY(transport.sent[1].body.contains("pageIndex=2")); transport.answer(200, kindPage(second, 102));
        QList<QDate> days;
        for (int n = 2; n < 20; ++n) {
            QTRY_COMPARE(transport.sent.size(), n + 1); const auto request = transport.sent[n];
            if (request.method != "GET") break;
            QVERIFY(NetworkTradingTransport::allowed(request)); days << nasdaqDate(request);
            transport.answer(200, nasdaqPage(days.last(), {nasdaqRow(QString("SY%1").arg(n), "time-pre-market", "$300,000,000,000")}));
            if (days.size() == 6) break;
        }
        QCOMPARE(days, (QList<QDate>{QDate(2026, 10, 27), QDate(2026, 10, 28), QDate(2026, 10, 29), QDate(2026, 10, 30), QDate(2026, 11, 2), QDate(2026, 11, 3)}));
        QTRY_VERIFY(model.feed(EarningsSource::Nasdaq).available);
        const auto korea = model.feed(EarningsSource::Kind); QVERIFY(korea.available); QCOMPARE(korea.events.size(), 101);
        const auto kept = std::find_if(korea.events.cbegin(), korea.events.cend(), [](const auto& e) { return e.code == "10000"; });
        QVERIFY(kept != korea.events.cend()); QCOMPARE(kept->title, QStringLiteral("3분기 실적"));
        QCOMPARE(model.feed(EarningsSource::Nasdaq).events.size(), 6);
        QCoreApplication::processEvents(); QCOMPARE(transport.sent.size(), 8); // No extra request after the week.
    }
    void oneFailedDayMakesOnlyThatSourceUnavailable() {
        const auto now = QDateTime(QDate(2026, 10, 28), QTime(10, 0), Kst).toMSecsSinceEpoch();
        FakeTransport transport; EarningsCalendar model(&transport, nullptr, [now] { return now; }); model.setActive(true);
        QTRY_COMPARE(transport.sent.size(), 1); transport.answer(200, kindPage(kindRow("00066", "가상하이닉스", "3분기 실적", QDate(2026, 10, 29), "09:00"), 1));
        QTRY_COMPARE(transport.sent.size(), 2); transport.answer(200, nasdaqPage(QDate(2026, 10, 27), {nasdaqRow("SYN", "time-pre-market", "$300,000,000,000")}));
        QTRY_COMPARE(transport.sent.size(), 3); transport.answer(500);
        QTRY_VERIFY(!model.feed(EarningsSource::Nasdaq).loading);
        QCoreApplication::processEvents(); QCOMPARE(transport.sent.size(), 3); // The rest of the failed week is not requested.
        QVERIFY(!model.feed(EarningsSource::Nasdaq).available); QVERIFY(model.feed(EarningsSource::Nasdaq).events.isEmpty());
        QVERIFY(model.feed(EarningsSource::Nasdaq).reason.contains("500"));
        QVERIFY(model.feed(EarningsSource::Kind).available); QCOMPARE(model.feed(EarningsSource::Kind).events.size(), 1);
        // Old schedules expire instead of lingering.
        QVERIFY(!model.feed(EarningsSource::Kind, now + EarningsCalendar::FreshnessMs + 1).available);
    }
    void flowSectionShowsLargeOrFollowedCompaniesOnly() {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto korea = QDateTime::fromMSecsSinceEpoch(now, Kst).date(), us = QDateTime::fromMSecsSinceEpoch(now, NewYork).date();
        FakeTransport transport; EarningsCalendar model(&transport, nullptr, [now] { return now; });
        EarningsCalendarView view(&model); view.resize(720, 900); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTRY_COMPARE(transport.sent.size(), 1);
        transport.answer(200, kindPage(kindRow("00066", "가상하이닉스", "3분기 경영실적 발표", korea.addDays(1), "09:00") +
                                       kindRow("00593", "가상전자", "3분기 실적", korea.addDays(2), "--:--") +
                                       kindRow("27221", "가상바이오", "사업현황 설명", korea.addDays(1), "14:00") +
                                       kindRow("99999", "가상소형", "3분기 실적", korea.addDays(1), "10:00"), 4));
        int answered = 1;
        while (true) {
            QTRY_COMPARE(transport.sent.size(), answered + 1); const auto date = nasdaqDate(transport.sent[answered]);
            QJsonArray rows;
            if (answered == 1) rows = {nasdaqRow("MEGA", "time-after-hours", "$2,000,000,000,000", "$2.01", "14"), nasdaqRow("SMOL", "time-pre-market", "$5,000,000,000"),
                                       nasdaqRow("FAVE", "time-pre-market", "$9,000,000,000")};
            transport.answer(200, nasdaqPage(date, rows)); ++answered;
            if (model.feed(EarningsSource::Nasdaq).available) break;
        }
        // KOSPI top-50 codes and favourites are six characters; KIND matches the first five.
        view.setInterest({"000660", "005930", "272210"}, {"FAVE"});
        QTRY_COMPARE(view.findChildren<QLabel*>("earningsEventTitle").size(), 4);
        QStringList names; for (auto* label : view.findChildren<QLabel*>("earningsEventTitle")) names << label->text();
        QVERIFY(names.contains(QStringLiteral("가상하이닉스"))); QVERIFY(names.contains(QStringLiteral("가상전자")));
        QVERIFY(names.contains("MEGA Synthetic Inc. MEGA")); QVERIFY(names.contains("FAVE Synthetic Inc. FAVE"));
        QVERIFY(!names.contains(QStringLiteral("가상바이오"))); // Not a results session.
        QVERIFY(!names.contains(QStringLiteral("가상소형")));   // Not followed and not in the top 50.
        QVERIFY(!names.join(' ').contains("SMOL"));
        QString meanings; for (auto* label : view.findChildren<QLabel*>("earningsEventMeaning")) meanings += label->text() + '\n';
        QVERIFY(meanings.contains(usabilityText(UsabilityText::FlowEarningsAfterHours) + " · " + usabilityText(UsabilityText::FlowEarningsEps).arg("$2.01", "14")));
        QString times; for (auto* label : view.findChildren<QLabel*>("earningsEventTime")) times += label->text() + '\n';
        QVERIFY(times.contains("09:00")); QVERIFY(times.contains(usabilityText(UsabilityText::FlowEarningsTimeUnknown)));
        QVERIFY(!times.contains("00:00")); // A missing KIND time is never shown as midnight.
        QVERIFY(keefi::test::savePublicViewPreview(view, "earnings-calendar-flow"));
        // Unfollowing removes the small company again.
        view.setInterest({"000660", "005930", "272210"}, {}); QTRY_COMPARE(view.findChildren<QLabel*>("earningsEventTitle").size(), 3);
    }
    void optionalSavedOfficialPagesParse() {
        // Developer check against saved copies of the real pages; never fetched by the test.
        const auto kind = qEnvironmentVariable("KEEFI_KIND_IR_PAGE"), nasdaq = qEnvironmentVariable("KEEFI_NASDAQ_EARNINGS_PAGE");
        if (kind.isEmpty() && nasdaq.isEmpty()) QSKIP("KEEFI_KIND_IR_PAGE and KEEFI_NASDAQ_EARNINGS_PAGE not set");
        if (!kind.isEmpty()) {
            QFile file(kind); QVERIFY(file.open(QIODevice::ReadOnly)); QList<EarningsEvent> rows; int total = 0;
            QVERIFY(EarningsCalendar::parseKind(file.readAll(), QDate(2000, 1, 1), QDate(2099, 12, 31), &rows, &total));
            int results = 0; for (const auto& row : rows) results += EarningsCalendar::earningsTitle(row.title);
            qInfo("KIND rows %lld of %d, results sessions %d", qint64(rows.size()), total, results); QVERIFY(!rows.isEmpty());
        }
        if (!nasdaq.isEmpty()) {
            QFile file(nasdaq); QVERIFY(file.open(QIODevice::ReadOnly)); QList<EarningsEvent> rows;
            const auto date = QDate::fromString(qEnvironmentVariable("KEEFI_NASDAQ_EARNINGS_DATE"), Qt::ISODate);
            QVERIFY(EarningsCalendar::parseNasdaq(file.readAll(), date, &rows));
            int large = 0; for (const auto& row : rows) large += row.marketCapUsd >= EarningsCalendarView::LargeCapUsd;
            qInfo("Nasdaq rows %lld, large caps %d", qint64(rows.size()), large); QVERIFY(!rows.isEmpty());
        }
    }
};
QTEST_MAIN(EarningsCalendarTest)
#include "tst_earnings_calendar.moc"
