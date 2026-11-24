#include "TooltipTestSupport.h"
#include <QAccessible>
// Synthetic public schedules only. No network, accounts or profile access.
#include "keefi/OfficialCalendar.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/InfoButton.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>
using namespace keefi;
namespace {
const qint64 Now = QDateTime::fromString("2026-10-28T10:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
QByteArray scheduleEvent(const QByteArray& uid, const QByteArray& title, const QByteArray& at) {
    return "BEGIN:VEVENT\r\nUID:" + uid + "\r\nSUMMARY:" + title + "\r\nDTSTART:" + at +
        "\r\nDTSTAMP:20261001T120000Z\r\nSEQUENCE:0\r\nEND:VEVENT\r\n";
}
QByteArray calendar(const QByteArray& events) {
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + events + "END:VCALENDAR\r\n";
}
QByteArray fomc() {
    return "<h4><a>2026 FOMC Meetings</a></h4><div class=\"fomc-meeting__month\"><strong>October</strong></div>"
        "<div class=\"fomc-meeting__date\">27-28</div>";
}
QByteArray bok() {
    return "<h3>2026년</h3><table id=\"tableId\"><tbody><tr><th scope=\"row\">10월 28일(수)</th></tr></tbody></table>";
}
QByteArray kind() {
    return "<section class=\"scrarea type-00\"><table class=\"list type-00 mt10\"><tbody><tr><td>1</td>"
        "<td><img alt='유가증권'/><a title=\"가상전자\" onclick=\"companysummary_open('00593');\">가상전자</a></td>"
        "<td><a>3분기 실적 발표</a></td><td>-</td><td>2026-10-29</td><td>09:00</td></tr></tbody></table></section>"
        "<section class=\"paging-group\"><div class=\"info type-00\"> 전체 <em>1</em>건 : <strong>1</strong>/1</div></section>";
}
QByteArray nasdaq(const QDate& date) {
    const QJsonObject row{{"symbol", "SYN"}, {"name", "Synthetic Inc."}, {"time", "time-pre-market"}, {"marketCap", "$300,000,000,000"},
        {"fiscalQuarterEnding", "Sep/2026"}, {"epsForecast", "$1.25"}, {"noOfEsts", "12"}};
    const QJsonObject data{{"asOf", QLocale(QLocale::English).toString(date, "ddd, MMM d, yyyy")}, {"headers", QJsonObject{}},
        {"rows", date == QDate(2026, 10, 28) ? QJsonArray{row} : QJsonArray{}}};
    return QJsonDocument(QJsonObject{{"data", data}, {"status", QJsonObject{{"rCode", 200}}}}).toJson(QJsonDocument::Compact);
}
class Transport final : public TradingTransport {
  public:
    QList<RequestSpec> sent; quint64 token = 0;
    void send(quint64 next, const RequestSpec& request) override { token = next; sent.append(request); }
    void abort() override {}
    void answer(int status, const QByteArray& body = {}) { HttpResult r; r.status = status; r.body = body; emit finished(token, r); }
};
}
class TodayCalendarTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QVERIFY(KeefiFonts::registerBundledFonts());
        KeefiTheme::instance()->setMode(ThemeMode::Light);
    }
    void receivedCalendarTitleStaysLiteralInItsTooltip() {
        Transport transport;
        OfficialCalendar model(&transport, nullptr, [] { return Now; });
        TodayCalendarView view(nullptr, [] { return Now; });
        view.setCalendar(&model);
        model.setActive(true);
        QTRY_COMPARE(transport.sent.size(), 1);
        const auto title = QByteArray("<b>Synthetic schedule</b>");
        transport.answer(200, calendar(scheduleEvent("synthetic-tooltip", title, "20261030T123000Z")));
        auto* label = view.findChild<QLabel*>("todayCalendarEventTitle");
        QVERIFY(label);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QCOMPARE(label->text(), QString::fromUtf8(title));
        const auto shown = keefi::test::projectTooltip(label->toolTip());
        QCOMPARE(shown.text, label->text());
        QVERIFY(!shown.image);
        QVERIFY(!shown.anchor);
        QCOMPARE(shown.resourceRequests, 0);
    }
    void thisWeekKeepsReceivedPastDatesWithoutChangingTheExistingFeed() {
        Transport transport; OfficialCalendar model(&transport, nullptr, [] { return Now; });
        model.setActive(true); QTRY_COMPARE(transport.sent.size(), 1);
        transport.answer(200, calendar(scheduleEvent("last-week", "Employment Situation", "20261023T123000Z") +
            scheduleEvent("monday", "Consumer Price Index", "20261026T123000Z") +
            scheduleEvent("future", "Employment Situation", "20261030T123000Z")));
        QCOMPARE(model.feed(OfficialCalendarSource::Bls).events.size(), 1);
        const auto week = model.weekFeed(OfficialCalendarSource::Bls);
        QCOMPARE(week.events.size(), 2);
        QCOMPARE(week.events.first().uid, QString("monday"));
        QCOMPARE(week.events.last().uid, QString("future"));
        QVERIFY(model.weekFeed(OfficialCalendarSource::Bls, Now + OfficialCalendar::FreshnessMs + 1).events.isEmpty());
        QCOMPARE(transport.sent.size(), 1); // A different view does not collect a second copy.
    }
    void mergedSchedulesKeepDateOnlyRowsSeparateAndFiltersDoNotCollectAgain() {
        Transport publicTransport, earningsTransport;
        OfficialCalendar economic(&publicTransport, nullptr, [] { return Now; });
        EarningsCalendar earnings(&earningsTransport, nullptr, [] { return Now; });
        TodayCalendarView view(nullptr, [] { return Now; }); view.setCalendar(&economic); view.setEarnings(&earnings);
        view.setInterest({"005930"}, {}); view.resize(976, 284); view.show(); QTest::qWait(5);
        QCOMPARE(publicTransport.sent.size(), 0); QCOMPARE(earningsTransport.sent.size(), 0);
        economic.setActive(true); earnings.setActive(true);
        QTRY_COMPARE(publicTransport.sent.size(), 1);
        publicTransport.answer(200, calendar(scheduleEvent("monday", "Consumer Price Index", "20261026T123000Z")));
        QTRY_COMPARE(publicTransport.sent.size(), 2); publicTransport.answer(200, calendar({}));
        QTRY_COMPARE(publicTransport.sent.size(), 3); publicTransport.answer(200, fomc());
        QTRY_COMPARE(publicTransport.sent.size(), 4); publicTransport.answer(200, bok());
        int answered = 0;
        for (int i = 0; i < 9 && (earnings.feed(EarningsSource::Kind).loading || earnings.feed(EarningsSource::Nasdaq).loading); ++i) {
            QTRY_VERIFY(earningsTransport.sent.size() > answered);
            const auto request = earningsTransport.sent.at(answered++);
            if (request.url.host() == "kind.krx.co.kr") earningsTransport.answer(200, kind());
            else earningsTransport.answer(200, nasdaq(QDate::fromString(QUrlQuery(request.url).queryItemValue("date"), Qt::ISODate)));
        }
        QVERIFY(earnings.feed(EarningsSource::Kind).available); QVERIFY(earnings.feed(EarningsSource::Nasdaq).available);
        QTRY_COMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(), 5);
        QVERIFY(view.findChild<QLabel*>("todayCalendarStatus")->isHidden());
        auto* nowLine = view.findChild<QLabel*>("todayCalendarNow"); auto* dateGroup = view.findChild<QLabel*>("todayCalendarDateOnly");
        QVERIFY(nowLine); QVERIFY(dateGroup);
        bool seenPast = false, seenUsDate = false, seenBokDate = false;
        for (auto* row : view.findChildren<QWidget*>("todayCalendarRow")) {
            const auto state = row->findChild<QLabel*>("todayCalendarEventState")->text();
            const auto time = row->findChild<QLabel*>("todayCalendarTime")->text();
            if (state == QStringLiteral("발표됨")) { seenPast = true; QTRY_VERIFY(row->y() < nowLine->y()); }
            if (!row->property("exactTime").toBool()) {
                QTRY_VERIFY(row->y() > dateGroup->y()); QVERIFY(!time.contains(':')); QVERIFY(state != QStringLiteral("발표됨"));
                if (row->property("country") == "US") { seenUsDate = true; QCOMPARE(time, QStringLiteral("장 시작 전")); }
                else { seenBokDate = true; QCOMPARE(time, QStringLiteral("시각 미공개")); }
            }
            QVERIFY(row->geometry().bottom() < view.height());
        }
        QVERIFY(seenPast); QVERIFY(seenUsDate); QVERIFY(seenBokDate);
        if (const auto capture = qEnvironmentVariable("KEEFI_TODAY_CALENDAR_CAPTURE"); !capture.isEmpty()) QVERIFY(view.grab().save(capture));
        const auto requests = publicTransport.sent.size() + earningsTransport.sent.size();
        auto* us=view.findChild<QPushButton*>("todayCalendarFilter_us");QVERIFY(us);auto* ax=QAccessible::queryAccessibleInterface(us);QVERIFY(ax&&ax->actionInterface());ax->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
        QCOMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(),3);
        for(auto* row:view.findChildren<QWidget*>("todayCalendarRow"))QCOMPARE(row->property("country").toString(),QString("US"));
        QTest::mouseClick(view.findChild<QPushButton*>("todayCalendarFilter_kr"), Qt::LeftButton);
        QCOMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(), 2);
        for (auto* row : view.findChildren<QWidget*>("todayCalendarRow")) QCOMPARE(row->property("country").toString(), QString("KR"));
        QTest::mouseClick(view.findChild<QPushButton*>("todayCalendarFilter_earnings"), Qt::LeftButton);
        QCOMPARE(view.findChildren<QWidget*>("todayCalendarRow").size(), 2);
        for (auto* row : view.findChildren<QWidget*>("todayCalendarRow")) QVERIFY(row->property("earnings").toBool());
        QCOMPARE(publicTransport.sent.size() + earningsTransport.sent.size(), requests);
        auto* information = view.findChild<InfoButton*>("todayCalendarInfo"); information->openInformation();
        QTRY_VERIFY(information->informationOpen());
        const auto text = information->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();
        QVERIFY(text.contains(QStringLiteral("KEEFI 기준"))); QVERIFY(text.contains(QStringLiteral("실제 발표나 발표 수치를 확인했다는 뜻이 아닙니다")));
    }
    void pagesBoundTheRowsAndKeepSuccessfulSourcesWhenAnotherFails() {
        Transport transport; OfficialCalendar model(&transport, nullptr, [] { return Now; });
        TodayCalendarView view(nullptr, [] { return Now; }); view.setCalendar(&model); view.resize(976, 284); view.show();
        model.setActive(true); QTRY_COMPARE(transport.sent.size(), 1);
        QByteArray rows;
        for (int i = 0; i < 12; ++i) rows += scheduleEvent(QByteArray::number(i), "Consumer Price Index " + QByteArray::number(i),
            QDateTime::fromMSecsSinceEpoch(Now + (i + 1) * 3600000, QTimeZone::utc()).toString("yyyyMMdd'T'HHmmss'Z'").toLatin1());
        transport.answer(200, calendar(rows));
        QTRY_COMPARE(transport.sent.size(), 2); transport.answer(503);
        QTRY_COMPARE(transport.sent.size(), 3); transport.answer(503);
        QTRY_COMPARE(transport.sent.size(), 4); transport.answer(503);
        QTRY_VERIFY(view.findChildren<QWidget*>("todayCalendarRow").size() >= 3);
        QVERIFY(view.findChildren<QWidget*>("todayCalendarRow").size() <= 5);
        QVERIFY(view.findChild<QLabel*>("todayCalendarStatus")->text().contains(QStringLiteral("받지 못했어요")));
        const auto first = view.findChild<QLabel*>("todayCalendarTime")->text();
        auto* next = view.findChild<QPushButton*>("todayCalendarNext"); QVERIFY(next->isEnabled()); QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY(view.findChild<QLabel*>("todayCalendarTime")->text() != first);
        view.resize(1432, 400); QTest::qWait(5);
        QVERIFY(view.findChildren<QWidget*>("todayCalendarRow").size() <= 7);
        for (auto* row : view.findChildren<QWidget*>("todayCalendarRow")) QVERIFY(row->geometry().bottom() < view.findChild<QPushButton*>("todayCalendarNext")->y());
        const auto count = transport.sent.size(); view.hide(); view.show(); QTest::qWait(5); QCOMPARE(transport.sent.size(), count);
    }
};
QTEST_MAIN(TodayCalendarTest)
#include "tst_today_calendar.moc"
