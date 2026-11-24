#include "PublicViewPreview.h"
#include "keefi/OfficialCalendar.h"
#include "keefi/OfficialCalendarView.h"
#include "keefi/TradingTransport.h"
#include "keefi/InfoButton.h"
#include "keefi/UsabilityPresentation.h"
#include <QLocale>
#include <QDateTime>
#include <QFile>
#include <QLabel>
#include <QTest>
#include <QTimeZone>
#include <QDialog>
#include <QUrlQuery>
#include <QPlainTextEdit>
using namespace keefi;
namespace {
qint64 at(const QString& iso) { return QDateTime::fromString(iso, Qt::ISODate).toMSecsSinceEpoch(); }
const qint64 Now = at("2026-10-22T10:00:00Z");
QByteArray icsEvent(const QByteArray& uid, const QByteArray& start, const QByteArray& extra = {}) {
    return "BEGIN:VEVENT\r\nUID:" + uid + "\r\nSUMMARY:Gross Domestic Product\\,\r\n  advance estimate\r\nDTSTART" + start +
        "\r\nDTSTAMP:20261001T120000Z\r\nSEQUENCE:0\r\n" + extra + "END:VEVENT\r\n";
}
QByteArray calendar(const QByteArray& events) { return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + events + "END:VCALENDAR\r\n"; }
QByteArray fomcRow(const QByteArray& month, const QByteArray& days, bool shaded = false) {
    return QByteArray("<div class=\"") + (shaded ? "fomc-meeting--shaded row fomc-meeting" : "row fomc-meeting") + "\">\n  <div class=\"" +
        (shaded ? "fomc-meeting--shaded " : "") + "fomc-meeting__month col-xs-5 col-sm-3 col-md-2\"><strong>" + month + "</strong></div>\n\n" +
        "  <div class=\"fomc-meeting__date col-xs-4 col-sm-9 col-md-10 col-lg-1\">" + days + "</div>\n</div>\n";
}
QByteArray fomcPage(const QByteArray& panels) {
    return "<html><body>" + panels + "<p>Last Update: October 1, 2026</p></body></html>";
}
QByteArray fomcPanel(const QByteArray& year, const QByteArray& rows) {
    return "<div class=\"panel panel-default\"><div class=\"panel-heading\"><h4><a id=\"1\">" + year + " FOMC Meetings</a></h4></div>" + rows + "</div>";
}
QByteArray bokRow(const QDate& date, const char* weekday = nullptr) {
    static const char* names[] = {"월", "화", "수", "목", "금", "토", "일"};
    return "<tr> <th scope=\"row\">" + date.toString("MM").toLatin1() + "월 " + date.toString("dd").toLatin1() + "일(" + QByteArray(weekday ? weekday : names[date.dayOfWeek() - 1]) +
        ")</th> <td> </td> <td class=\"tal\"> <div class=\"listText\"> </div> </td> <td> </td> <td> </td> </tr>\n";
}
QByteArray bokPage(int year, const QByteArray& rows) {
    return "<html><body><h2>통화정책방향 결정회의</h2><div class=\"h-group\"><h3>" + QByteArray::number(year) + "년</h3></div>"
        "<div class=\"fileGoupBox\"><h3>다운로드할 파일을 선택하세요.</h3></div><table id=\"tableId\"><caption>통화정책방향 회의</caption>"
        "<thead><tr><th scope=\"col\">회의일자</th></tr></thead><tbody>" + rows + "</tbody></table></body></html>";
}
class CalendarTransport final : public TradingTransport {
  public:
    QList<RequestSpec> sent; quint64 token = 0; int aborts = 0;
    void send(quint64 next, const RequestSpec& request) override { token = next; sent.append(request); }
    void abort() override { ++aborts; }
    void answer(int status, const QByteArray& body = {}) { HttpResult r; r.status = status; r.body = body; emit finished(token, r); }
};
}
class OfficialCalendarTest final : public QObject {
    Q_OBJECT
  private slots:
    void futureReleaseCountdownUsesDaysAheadWithoutChangingSchedule(){
        const auto now=QDateTime::currentMSecsSinceEpoch();
        const auto future=QDateTime::fromSecsSinceEpoch(now/1000,Qt::UTC).addDays(3);
        CalendarTransport transport;OfficialCalendar model(&transport,nullptr,[now]{return now;});
        OfficialCalendarView view(&model);view.setMarketFlowMode(true);view.resize(900,700);view.show();
        QTRY_COMPARE(transport.sent.size(),1);
        transport.answer(200,calendar(icsEvent("SYNTHETIC_FUTURE_RELEASE",":"+future.toString("yyyyMMdd'T'HHmmss'Z'").toLatin1())));
        QTRY_COMPARE(transport.sent.size(),2);transport.answer(403);
        const auto feed=model.feed(OfficialCalendarSource::Bls);QVERIFY(feed.available);QCOMPARE(feed.events.size(),1);
        QCOMPARE(feed.events.first().startsAtMs,future.toMSecsSinceEpoch());
        auto* countdown=view.findChild<QLabel*>("officialCalendarEventDday");QVERIFY(countdown);
        const QTimeZone kst("Asia/Seoul");
        const auto days=QDateTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch(),kst).date().daysTo(future.toTimeZone(kst).date());
        QVERIFY(days>0);QCOMPARE(countdown->text(),QStringLiteral("%1일 뒤").arg(days));
        QVERIFY(!countdown->text().contains(QStringLiteral("일 전")));
        QTRY_COMPARE(transport.sent.size(),3);transport.answer(403);QTRY_COMPARE(transport.sent.size(),4);transport.answer(403); // FOMC, then the BOK list.
        QCoreApplication::processEvents();QCOMPARE(transport.sent.size(),4);
    }
    void calendarDisplayHidesUrlsWithoutChangingOfficialSchedule(){
        CalendarTransport transport;OfficialCalendar model(&transport,nullptr,[]{return Now;});
        OfficialCalendarView view(&model);view.resize(900,700);view.show();
        QTRY_COMPARE(transport.sent.size(),1);transport.answer(200,calendar(icsEvent("SYNTHETIC_PUBLIC_SCHEDULE",":20261029T123000Z")));
        QTRY_COMPARE(transport.sent.size(),2);transport.answer(403);
        const auto original=model.feed(OfficialCalendarSource::Bls);QVERIFY(original.available);QCOMPARE(original.events.size(),1);
        QCOMPARE(transport.sent.first().url,QUrl("https://www.bls.gov/schedule/news_release/bls.ics"));
        QCOMPARE(original.sourceUpdatedAtMs,at("2026-10-01T12:00:00Z"));
        auto* title=view.findChild<QLabel*>("officialCalendarEventTitle");QVERIFY(title);
        QVERIFY(title->toolTip().contains("BLS"));QVERIFY(title->toolTip().contains("21:00"));QVERIFY(!title->toolTip().contains('\n'));
        for(auto* widget:view.findChildren<QWidget*>()){
            QVERIFY2(!widget->toolTip().contains("://"),qPrintable(widget->toolTip()));
            QVERIFY(!widget->accessibleName().contains("://"));QVERIFY(!widget->accessibleDescription().contains("://"));
        }
        for(auto* info:view.findChildren<InfoButton*>()){
            info->openInformation();QTRY_VERIFY(info->informationOpen());auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);
            const auto text=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();
            QVERIFY2(!text.contains("://"),qPrintable(text));dialog->reject();
        }
        const auto after=model.feed(OfficialCalendarSource::Bls);
        QCOMPARE(after.sourceUpdatedAtMs,original.sourceUpdatedAtMs);QCOMPARE(after.receivedAtMs,original.receivedAtMs);
        QCOMPARE(after.events.first().uid,original.events.first().uid);QCOMPARE(after.events.first().title,original.events.first().title);
        QCOMPARE(after.events.first().startsAtMs,original.events.first().startsAtMs);QCOMPARE(after.events.first().sourceTimeZone,original.events.first().sourceTimeZone);
        QTRY_COMPARE(transport.sent.size(),3);transport.answer(403);QTRY_COMPARE(transport.sent.size(),4);transport.answer(403); // FOMC, then the BOK list.
        QCoreApplication::processEvents();QCOMPARE(transport.sent.size(),4);
    }
    void compactCalendarRowKeepsOnlySourceFieldsAndAccessibleInformation(){
        CalendarTransport transport;OfficialCalendar model(&transport,nullptr,[]{return Now;});OfficialCalendarView view(&model);view.resize(900,700);view.show();QTRY_COMPARE(transport.sent.size(),1);
        transport.answer(200,calendar(icsEvent("SYNTHETIC_RELEASE",":20261029T123000Z")));QTRY_COMPARE(transport.sent.size(),2);transport.answer(403);QVERIFY(model.feed(OfficialCalendarSource::Bls).available);
        auto* row=view.findChild<QWidget*>("officialCalendarEventRow");QVERIFY(row);auto* title=row->findChild<QLabel*>("officialCalendarEventTitle");QVERIFY(title);QVERIFY(!title->wordWrap());QCOMPARE(title->textFormat(),Qt::PlainText);QVERIFY(title->text().contains("21:30"));
        for(const auto* absent:{"officialCalendarPrevious","officialCalendarActual","officialCalendarForecast","officialCalendarImportance"}){auto* field=row->findChild<QWidget*>(absent);QVERIFY(!field||field->isHidden());}
        auto* info=row->findChild<InfoButton*>("officialCalendarEventInfo");QVERIFY(info);info->setFocus();QTest::keyClick(info,Qt::Key_Return);QTRY_VERIFY(info->informationOpen());auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);auto* text=dialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);
        QVERIFY(text->toPlainText().contains("Gross Domestic Product, advance estimate"));QVERIFY(text->toPlainText().contains(QStringLiteral("이전·실제·예상치 미제공")));QVERIFY(text->toPlainText().contains(QStringLiteral("중요도 미제공")));QVERIFY(!text->toPlainText().contains("://"));QVERIFY(text->toPlainText().contains(QStringLiteral("수신")));
        QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!info->informationOpen());QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());QTest::keyClick(info->findChild<QDialog*>("infoDialog"),Qt::Key_Escape);QCOMPARE(transport.sent.size(),2);QCoreApplication::processEvents();QVERIFY(row->height()<=60);QVERIFY(row->geometry().top()<=view.findChild<QLabel*>("officialCalendarStatus")->geometry().bottom()+24);QVERIFY(keefi::test::savePublicViewPreview(view,"c9-official-calendar"));
    }
    void fomcPageUsesRegularMeetingStatementTimesOnly() {
        const auto page = fomcPage(fomcPanel("2026", fomcRow("October", "27-28") + fomcRow("November", "4 (notation vote)", true) + fomcRow("December", "8-9*", true)) +
                                   fomcPanel("2027", fomcRow("Apr/May", "30-1") + fomcRow("March", "16 (unscheduled)")));
        OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Fomc, page, Now, &feed));
        QVERIFY(feed.available); QCOMPARE(feed.source, OfficialCalendarSource::Fomc); QCOMPARE(feed.events.size(), 3);
        // Second meeting day, 14:00 New York: EDT in October and May, EST in December.
        QCOMPARE(feed.events[0].startsAtMs, at("2026-10-28T18:00:00Z")); QCOMPARE(feed.events[0].uid, QString("fomc-2026-10-28"));
        QCOMPARE(feed.events[1].startsAtMs, at("2026-12-09T19:00:00Z"));
        QCOMPARE(feed.events[2].startsAtMs, at("2027-05-01T18:00:00Z"));
        for (const auto& event : feed.events) { QCOMPARE(event.title, QString("FOMC Statement")); QCOMPARE(event.sourceTimeZone, QString("America/New_York")); }
        QCOMPARE(feed.sourceUpdatedAtMs, QDateTime(QDate(2026, 10, 1), QTime(0, 0), QTimeZone("America/New_York")).toMSecsSinceEpoch());
        QCOMPARE(usabilityCalendarTitle(feed.events[0].title), QStringLiteral("미국 FOMC 금리 결정"));
    }
    void optionalSavedOfficialFomcPageParses() {
        // Developer check against a saved copy of the official page; never fetched by the test.
        const auto path = qEnvironmentVariable("KEEFI_FOMC_PAGE"); if (path.isEmpty()) QSKIP("KEEFI_FOMC_PAGE not set");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Fomc, file.readAll(), QDateTime::currentMSecsSinceEpoch(), &feed));
        QVERIFY(feed.events.size() >= 8); for (const auto& event : feed.events) qInfo("%s", qPrintable(event.uid));
    }
    void fomcPageRejectsMissingOrContradictoryMeetings_data() {
        QTest::addColumn<QByteArray>("page");
        QTest::newRow("no-panel") << fomcPage(fomcRow("October", "27-28"));
        QTest::newRow("only-unscheduled") << fomcPage(fomcPanel("2026", fomcRow("March", "16 (unscheduled)")));
        QTest::newRow("reversed-days") << fomcPage(fomcPanel("2026", fomcRow("October", "28-27")));
        QTest::newRow("unknown-month") << fomcPage(fomcPanel("2026", fomcRow("Octember", "27-28")));
        QTest::newRow("non-adjacent-months") << fomcPage(fomcPanel("2026", fomcRow("Jan/Mar", "31-1")));
        QTest::newRow("impossible-date") << fomcPage(fomcPanel("2026", fomcRow("February", "30-31")));
        QTest::newRow("not-utf8") << QByteArray("<h4><a>2026 FOMC Meetings</a></h4>\xff");
        // One changed row among good rows makes the whole page unavailable (no partial schedule).
        QTest::newRow("unknown-date-text") << fomcPage(fomcPanel("2026", fomcRow("October", "27-28") + fomcRow("December", "TBD")));
        QTest::newRow("unreadable-month-cell") << fomcPage(fomcPanel("2026", fomcRow("October", "27-28") +
            "<div class=\"fomc-meeting__month\"><span>December</span></div><div class=\"fomc-meeting__date\">8-9</div>"));
        QTest::newRow("bad-row-in-past-year") << fomcPage(fomcPanel("2026", fomcRow("October", "27-28")) + fomcPanel("2025", fomcRow("Octember", "28-29")));
        QTest::newRow("current-year-missing") << fomcPage(fomcPanel("2025", fomcRow("October", "28-29")) + fomcPanel("2027", fomcRow("January", "26-27")));
        QTest::newRow("current-year-only-notation") << fomcPage(fomcPanel("2026", fomcRow("August", "22 (notation vote)")) + fomcPanel("2027", fomcRow("January", "26-27")));
    }
    void fomcPageRejectsMissingOrContradictoryMeetings() {
        QFETCH(QByteArray, page); OfficialCalendarFeed feed;
        QVERIFY(!OfficialCalendar::parse(OfficialCalendarSource::Fomc, page, Now, &feed));
        QVERIFY(!feed.available); QVERIFY(feed.events.isEmpty()); QVERIFY(!feed.reason.isEmpty());
    }
    void fomcRowsJoinTheMarketFlowCalendar() {
        const auto now = QDateTime::currentMSecsSinceEpoch(); const QTimeZone eastern("America/New_York");
        const auto today = QDateTime::fromMSecsSinceEpoch(now, eastern).date();
        auto day = today.addDays(3); if (day.day() == 1) day = day.addDays(1); // Both meeting days stay in one month.
        auto panels = fomcPanel(QByteArray::number(day.year()), fomcRow(QLocale(QLocale::English).monthName(day.month()).toLatin1(),
            QByteArray::number(day.day() - 1) + "-" + QByteArray::number(day.day())));
        if (day.year() != today.year()) panels += fomcPanel(QByteArray::number(today.year()), fomcRow("January", "27-28")); // Current year must be present.
        const auto page = fomcPage(panels);
        CalendarTransport transport; OfficialCalendar model(&transport, nullptr, [now] { return now; });
        OfficialCalendarView view(&model); view.setMarketFlowMode(true); view.resize(900, 700); view.show();
        QTRY_COMPARE(transport.sent.size(), 1); transport.answer(403); QTRY_COMPARE(transport.sent.size(), 2); transport.answer(403);
        QTRY_COMPARE(transport.sent.size(), 3); QCOMPARE(transport.sent[2].url, QUrl("https://www.federalreserve.gov/monetarypolicy/fomccalendars.htm")); transport.answer(200, page);
        QTRY_VERIFY(model.feed(OfficialCalendarSource::Fomc).available);
        QTRY_VERIFY(view.findChild<QLabel*>("officialCalendarEventTitle"));
        auto* title = view.findChild<QLabel*>("officialCalendarEventTitle"); QCOMPARE(title->text(), QStringLiteral("미국 FOMC 금리 결정"));
        QCOMPARE(view.findChild<QLabel*>("officialCalendarEventMeaning")->text(), usabilityText(UsabilityText::FlowCalendarFomc));
        QVERIFY(title->accessibleName().contains(" · FOMC · "));
    }
    void bokListIsDateOnlyWithMatchingWeekdays() {
        const auto page = bokPage(2026, bokRow(QDate(2026, 10, 22)) + bokRow(QDate(2026, 11, 26)));
        OfficialCalendarFeed feed; QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bok, page, Now, &feed));
        QVERIFY(feed.available); QCOMPARE(feed.source, OfficialCalendarSource::Bok); QCOMPARE(feed.events.size(), 2);
        QCOMPARE(feed.events[1].uid, QString("bok-2026-11-26")); QVERIFY(feed.events[1].dateOnly);
        QCOMPARE(feed.events[1].startsAtMs, QDateTime(QDate(2026, 11, 26), QTime(0, 0), QTimeZone("Asia/Seoul")).toMSecsSinceEpoch());
        QCOMPARE(feed.events[1].sourceTimeZone, QString("Asia/Seoul"));
        QCOMPARE(usabilityCalendarTitle(feed.events[1].title), QStringLiteral("한국은행 기준금리 결정"));
        QCOMPARE(OfficialCalendar::sourceName(OfficialCalendarSource::Bok), QStringLiteral("한국은행 금통위"));
    }
    void bokListRejectsAnyChangedRow_data() {
        QTest::addColumn<QByteArray>("page"); QTest::addColumn<qint64>("received");
        const auto good = bokRow(QDate(2026, 10, 22));
        QTest::newRow("wrong-weekday") << bokPage(2026, good + bokRow(QDate(2026, 11, 26), "금")) << Now;
        QTest::newRow("out-of-order") << bokPage(2026, bokRow(QDate(2026, 11, 26)) + good) << Now;
        QTest::newRow("impossible-date") << bokPage(2026, good + "<tr> <th scope=\"row\">02월 30일(월)</th> </tr>") << Now;
        QTest::newRow("unknown-row") << bokPage(2026, good + "<tr> <th scope=\"row\">11월 26일(목) 임시</th> </tr>") << Now;
        QTest::newRow("empty-list") << bokPage(2026, {}) << Now;
        QTest::newRow("year-not-requested") << bokPage(2027, bokRow(QDate(2027, 1, 14))) << Now;
        QTest::newRow("no-table") << bokPage(2026, good).replace("tableId", "otherTable") << Now;
        QTest::newRow("two-years") << bokPage(2026, good).replace("<h2>", "<h3>2025년</h3><h2>") << Now;
    }
    void bokListRejectsAnyChangedRow() {
        QFETCH(QByteArray, page); QFETCH(qint64, received); OfficialCalendarFeed feed;
        QVERIFY(!OfficialCalendar::parse(OfficialCalendarSource::Bok, page, received, &feed));
        QVERIFY(!feed.available); QVERIFY(feed.events.isEmpty()); QVERIFY(!feed.reason.isEmpty());
    }
    void optionalSavedOfficialBokPageParses() {
        // Developer check against a saved copy of the official page; never fetched by the test.
        const auto path = qEnvironmentVariable("KEEFI_BOK_PAGE"); if (path.isEmpty()) QSKIP("KEEFI_BOK_PAGE not set");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bok, file.readAll(), QDateTime::currentMSecsSinceEpoch(), &feed));
        QVERIFY(!feed.events.isEmpty()); for (const auto& event : feed.events) qInfo("%s", qPrintable(event.uid));
    }
    void bokMeetingShowsTheDateWithoutAnInventedTime() {
        const auto now = QDateTime::currentMSecsSinceEpoch(); const QTimeZone kst("Asia/Seoul");
        const auto today = QDateTime::fromMSecsSinceEpoch(now, kst).date(); auto day = today.addDays(2);
        if (day.year() != today.year()) day = today; // The list covers the KST year it was requested for.
        CalendarTransport transport; OfficialCalendar model(&transport, nullptr, [now] { return now; });
        OfficialCalendarView view(&model); view.setMarketFlowMode(true); view.resize(900, 700); view.show();
        for (int n = 1; n <= 3; ++n) { QTRY_COMPARE(transport.sent.size(), n); transport.answer(403); }
        QTRY_COMPARE(transport.sent.size(), 4); QCOMPARE(transport.sent[3].url, OfficialCalendar::request(OfficialCalendarSource::Bok, today.year()).url);
        QVERIFY(NetworkTradingTransport::allowed(transport.sent[3]));
        transport.answer(200, bokPage(today.year(), bokRow(day)));
        QTRY_VERIFY(model.feed(OfficialCalendarSource::Bok).available);
        QTRY_VERIFY(view.findChild<QLabel*>("officialCalendarEventTitle"));
        QCOMPARE(view.findChild<QLabel*>("officialCalendarEventTitle")->text(), QStringLiteral("한국은행 기준금리 결정"));
        QCOMPARE(view.findChild<QLabel*>("officialCalendarEventMeaning")->text(), usabilityText(UsabilityText::FlowCalendarBok));
        const auto time = view.findChild<QLabel*>("officialCalendarEventTime")->text();
        QVERIFY(time.contains(usabilityText(UsabilityText::FlowCalendarDateOnly))); QVERIFY(!time.contains("00:00"));
        QVERIFY(view.findChild<QLabel*>("officialCalendarEventTitle")->accessibleName().contains(QStringLiteral(" · 한은 · ")));
        QVERIFY(!view.publicSummary().contains(usabilityText(UsabilityText::FlowDawn))); // No part of day for a date-only meeting.
        QVERIFY(view.publicSummary().contains(QStringLiteral("한국은행 기준금리 결정")));
        QVERIFY(keefi::test::savePublicViewPreview(view, "bok-calendar-flow"));
    }
    void exactPublicGetOnly() {
        for (auto source : {OfficialCalendarSource::Bls, OfficialCalendarSource::Bea, OfficialCalendarSource::Fomc, OfficialCalendarSource::Bok}) {
            const auto r = OfficialCalendar::request(source);
            QVERIFY(OfficialCalendar::isRequest(r)); QVERIFY(NetworkTradingTransport::allowed(r));
            QCOMPARE(r.method, QByteArray("GET")); QVERIFY(!r.authenticated); QVERIFY(r.body.isEmpty());
            for (int fault = 0; fault < 7; ++fault) {
                auto bad = r;
                if (fault == 0) bad.method = "POST";
                if (fault == 1) bad.authenticated = true;
                if (fault == 2) bad.headers.insert("Authorization", "secret");
                if (fault == 3) bad.url.setQuery("account=private");
                if (fault == 4) bad.url.setHost("example.test");
                if (fault == 5) bad.body = "{}";
                if (fault == 6) bad.url.setPort(444);
                QVERIFY(!OfficialCalendar::isRequest(bad)); QVERIFY(!NetworkTradingTransport::allowed(bad));
            }
        }
        // Only the BOK list year varies, within 2000-2099; any other change fails.
        QVERIFY(OfficialCalendar::isRequest(OfficialCalendar::request(OfficialCalendarSource::Bok, 2027)));
        QVERIFY(!OfficialCalendar::isRequest(OfficialCalendar::request(OfficialCalendarSource::Bok, 1999)));
        auto extra = OfficialCalendar::request(OfficialCalendarSource::Bok, 2026); auto query = QUrlQuery(extra.url); query.addQueryItem("menuNo", "1"); extra.url.setQuery(query);
        QVERIFY(!OfficialCalendar::isRequest(extra)); QVERIFY(!NetworkTradingTransport::allowed(extra));
    }
    void parsesOfficialUtcShapeWithoutInventingPublishedValues() {
        OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bea,
            calendar(icsEvent("FAKE_RELEASE", ";VALUE=DATE-TIME:20261029T123000Z")), Now, &feed));
        QVERIFY(feed.available); QCOMPARE(feed.receivedAtMs, Now); QCOMPARE(feed.events.size(), 1);
        QCOMPARE(feed.events[0].title, QString("Gross Domestic Product, advance estimate"));
        QCOMPARE(feed.events[0].startsAtMs, at("2026-10-29T12:30:00Z"));
        QCOMPARE(feed.sourceUpdatedAtMs, at("2026-10-01T12:00:00Z"));
        QCOMPARE(QDateTime::fromMSecsSinceEpoch(feed.events[0].startsAtMs, QTimeZone("Asia/Seoul")).toString("HH:mm"), QString("21:30"));
    }
    void viewLabelsScheduleAndUnavailableValues() {
        OfficialCalendar model(nullptr, nullptr, [] { return Now; }); OfficialCalendarView view(&model);
        auto* title = view.findChild<QLabel*>("officialCalendarTitle"); QVERIFY(title);
        QVERIFY(title->text().contains("공식 발표 일정"));
        auto* info=view.findChild<InfoButton*>("officialCalendarInfo");QVERIFY(info);view.show();QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());
        auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);const auto description=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();
        QVERIFY(description.contains("이전·실제·예상치 미제공"));QTest::keyClick(dialog,Qt::Key_Escape);
    }
    void destroyedViewStopsItsPollingWithoutOwningModel() {
        CalendarTransport transport; OfficialCalendar model(&transport, nullptr, [] { return Now; });
        auto* view = new OfficialCalendarView(&model); view->show();
        QTRY_COMPARE(transport.sent.size(), 1);
        delete view;
        QVERIFY(transport.aborts > 0);
        const auto count = transport.sent.size(); model.refresh();
        QCoreApplication::processEvents(); QCOMPARE(transport.sent.size(), count);
    }
    void timezoneUsesDstAndRejectsAmbiguousLocalTimes() {
        OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bls,
            calendar(icsEvent("SUMMER", ";TZID=America/New_York:20261029T083000") +
                     icsEvent("WINTER", ";TZID=America/New_York:20261104T083000")), Now, &feed));
        QCOMPARE(feed.events[0].startsAtMs, at("2026-10-29T12:30:00Z"));
        QCOMPARE(feed.events[1].startsAtMs, at("2026-11-04T13:30:00Z"));
        for (const QByteArray local : {QByteArray("20260308T023000"), QByteArray("20261101T013000")})
            QVERIFY(!OfficialCalendar::parse(OfficialCalendarSource::Bls,
                calendar(icsEvent("DST_GAP_OR_REPEAT", ";TZID=America/New_York:" + local)), Now, &feed));
    }
    void rejectsUnsupportedOrMalformedFeeds_data() {
        QTest::addColumn<QByteArray>("input");
        const auto good = calendar(icsEvent("ONE", ":20261029T123000Z"));
        QTest::newRow("html-instead-of-ics") << QByteArray("<html>Access denied</html>");
        QTest::newRow("floating-time") << calendar(icsEvent("ONE", ":20261029T083000"));
        QTest::newRow("all-day") << calendar(icsEvent("ONE", ";VALUE=DATE:20261029"));
        QTest::newRow("unknown-zone") << calendar(icsEvent("ONE", ";TZID=Unknown/Place:20261029T083000"));
        QTest::newRow("recurrence") << calendar(icsEvent("ONE", ":20261029T123000Z", "RRULE:FREQ=DAILY\r\n"));
        QTest::newRow("future-publication-stamp") << QByteArray(good).replace("20261001T120000Z", "20261023T120000Z");
        QTest::newRow("missing-calendar-end") << good.left(good.size() - 17);
        QTest::newRow("duplicate-start") << calendar(icsEvent("ONE", ":20261029T123000Z", "DTSTART:20261030T123000Z\r\n"));
        QTest::newRow("no-uid") << QByteArray(good).replace("UID:ONE\r\n", "");
        QTest::newRow("bad-date") << calendar(icsEvent("ONE", ":20260230T123000Z"));
        QTest::newRow("oversize") << QByteArray(1024 * 1024 + 1, 'x');
    }
    void rejectsUnsupportedOrMalformedFeeds() {
        QFETCH(QByteArray, input);
        OfficialCalendarFeed feed; feed.available = true; feed.events.append({"OLD", "old", "UTC", Now, Now});
        QVERIFY(!OfficialCalendar::parse(OfficialCalendarSource::Bls, input, Now, &feed));
        QVERIFY(!feed.available); QVERIFY(feed.events.isEmpty()); QVERIFY(!feed.reason.isEmpty());
    }
    void uidVersionsDeduplicateAndCancelWithoutGuessingConflicts() {
        const auto first = icsEvent("SAME", ":20261029T123000Z");
        const auto newer = QByteArray(first).replace("SEQUENCE:0", "SEQUENCE:1").replace("20261029T123000Z", "20261030T123000Z");
        OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bea, calendar(first + first + newer), Now, &feed));
        QCOMPARE(feed.events.size(), 1); QCOMPARE(feed.events[0].startsAtMs, at("2026-10-30T12:30:00Z"));
        const auto cancellation = QByteArray(newer).replace("SEQUENCE:1", "SEQUENCE:2").replace("END:VEVENT", "STATUS:CANCELLED\r\nEND:VEVENT");
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bea, calendar(first + cancellation + newer), Now, &feed));
        QVERIFY(feed.events.isEmpty());
        const auto conflicting = QByteArray(first).replace("20261029T123000Z", "20261030T123000Z");
        QVERIFY(!OfficialCalendar::parse(OfficialCalendarSource::Bea, calendar(first + conflicting), Now, &feed));
    }
    void independentSourcesClearFailuresExpireAndStopLateCallbacks() {
        CalendarTransport transport; OfficialCalendar model(&transport, nullptr, [] { return Now; });
        model.setActive(true); QTRY_COMPARE(transport.sent.size(), 1);
        QCOMPARE(transport.sent[0].url.host(), QString("www.bls.gov"));
        transport.answer(403); QTRY_COMPARE(transport.sent.size(), 2);
        QVERIFY(!model.feed(OfficialCalendarSource::Bls).available);
        QVERIFY(model.feed(OfficialCalendarSource::Bls).reason.contains("403"));
        transport.answer(200, calendar(icsEvent("BEA", ":20261029T123000Z")));
        QVERIFY(model.feed(OfficialCalendarSource::Bea).available); QCOMPARE(model.feed(OfficialCalendarSource::Bea).events.size(), 1);
        const auto expired = model.feed(OfficialCalendarSource::Bea, Now + OfficialCalendar::FreshnessMs + 1);
        QVERIFY(!expired.available); QVERIFY(expired.events.isEmpty());
        QTRY_COMPARE(transport.sent.size(), 3); QCOMPARE(transport.sent[2].url.host(), QString("www.federalreserve.gov")); transport.answer(403);
        QVERIFY(!model.feed(OfficialCalendarSource::Fomc).available); QVERIFY(model.feed(OfficialCalendarSource::Bea).available);
        model.refresh(); QTRY_COMPARE(transport.sent.size(), 4); transport.answer(503);
        QTRY_COMPARE(transport.sent.size(), 5); transport.answer(200, "bad calendar");
        QVERIFY(!model.feed(OfficialCalendarSource::Bea).available); QVERIFY(model.feed(OfficialCalendarSource::Bea).events.isEmpty());
        QTRY_COMPARE(transport.sent.size(), 6); transport.answer(403);
        model.refresh(); QTRY_COMPARE(transport.sent.size(), 7); const auto oldToken = transport.token;
        model.setActive(false); QVERIFY(transport.aborts > 0);
        HttpResult late; late.status = 200; late.body = calendar(icsEvent("LATE", ":20261029T123000Z"));
        emit transport.finished(oldToken, late);
        QVERIFY(model.feed(OfficialCalendarSource::Bls).events.isEmpty());
    }
    void todayWindowUsesKstAndElapsedScheduleDoesNotClaimActualRelease() {
        CalendarTransport transport; OfficialCalendar model(&transport, nullptr, [] { return Now; });
        model.setActive(true); QTRY_COMPARE(transport.sent.size(), 1);
        const auto events = icsEvent("YESTERDAY_KST", ":20261021T145959Z") +
            icsEvent("TODAY_KST", ":20261021T150000Z") + icsEvent("FUTURE", ":20261029T123000Z") +
            icsEvent("OUTSIDE", ":20261122T150000Z");
        transport.answer(200, calendar(events)); QTRY_COMPARE(transport.sent.size(), 2); transport.answer(403);
        QCOMPARE(model.feed(OfficialCalendarSource::Bls).events.size(), 2);
        OfficialCalendarView view(&model);
        const auto rows=view.findChildren<QLabel*>("officialCalendarEventTitle");QCOMPARE(rows.size(),2);
        QVERIFY(rows.first()->accessibleName().contains("발표 여부 미확인"));
        for(auto* row:rows)QCOMPARE(row->textFormat(),Qt::PlainText);
    }
    void publicBeaDownloadedFixture() {
        const auto path = qEnvironmentVariable("KEEFI_PUBLIC_CALENDAR_FIXTURE");
        if (path.isEmpty()) QSKIP("Opt-in official public feed fixture");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); OfficialCalendarFeed feed;
        QVERIFY(OfficialCalendar::parse(OfficialCalendarSource::Bea, file.readAll(), Now, &feed));
        QCOMPARE(feed.events.size(), 166);
        QList<qint64> current;
        const auto from = at("2026-10-21T15:00:00Z"), until = at("2026-11-21T15:00:00Z");
        for (const auto& e : feed.events) if (e.startsAtMs >= from && e.startsAtMs < until) current.append(e.startsAtMs);
        QCOMPARE(current, QList<qint64>({at("2026-10-29T12:30:00Z"), at("2026-10-29T12:30:00Z"),
            at("2026-11-04T13:30:00Z"), at("2026-11-20T13:30:00Z")}));
    }
    void publicFeedsOverTransport() {
        if (!qEnvironmentVariableIsSet("KEEFI_PUBLIC_CALENDAR_NETWORK")) QSKIP("Opt-in official public GET only");
        NetworkTradingTransport transport; OfficialCalendar model(&transport);
        model.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!model.feed(OfficialCalendarSource::Bls).loading && !model.feed(OfficialCalendarSource::Bea).loading, 45000);
        const auto bea = model.feed(OfficialCalendarSource::Bea);
        QVERIFY2(bea.available, qPrintable(bea.reason)); QVERIFY(!bea.events.isEmpty());
        qInfo() << "Public BEA displayed events:" << bea.events.size();
        qInfo() << "Public BLS status:" << model.feed(OfficialCalendarSource::Bls).reason;
        model.setActive(false);
    }
};
QTEST_MAIN(OfficialCalendarTest)
#include "tst_official_calendar.moc"
