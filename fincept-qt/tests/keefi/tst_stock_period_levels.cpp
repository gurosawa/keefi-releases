#include "keefi/StockPeriodLevels.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>
#include <algorithm>
using namespace keefi;
namespace {
constexpr qint64 Day=86400000,Minute=60000;
qint64 time(QDate date,QTime clock,QString zone="America/New_York") {return QDateTime(date,clock,QTimeZone(zone.toUtf8())).toMSecsSinceEpoch();}
struct Fixture {
    QDate today{2026,11,3};
    StockCandlePage daily,minute;
    StockCalendar calendar;
    qint64 now=time(today,QTime(10,0));
    Fixture() {
        daily.available=true;daily.source=StockSourceKind::Toss;daily.adjustment=StockAdjustment::SourceAdjusted;daily.nativeIntervalMs=Day;
        daily.instrument={"stock:US:SYN","SYN","US","USD","NASDAQ","America/New_York",{},true};
        daily.sourceUrl="https://example.invalid/synthetic-daily";daily.requestStartedAtMs=now;daily.receivedAtMs=now;
        calendar.available=true;calendar.marketCountry="US";calendar.evidence={"합성 캘린더","https://example.invalid/synthetic-calendar","synthetic",0,now,true};
        QDate prior;
        for(QDate d(2026,9,30);d<=today;d=d.addDays(1)) {
            StockSessionDay day;day.tradingDate=d;
            // Explicit test calendar: Friday Oct30 is a synthetic holiday;
            // Thursday Oct29 closes early. DST changes Nov1.
            if(d.dayOfWeek()>5||d==QDate(2026,10,30))day.state=StockCalendarState::ExplicitHoliday;
            else {
                day.state=StockCalendarState::Sessions;
                day.sessions.append({StockSessionKind::Regular,time(d,QTime(9,30)),time(d,d==QDate(2026,10,29)?QTime(13,0):QTime(16,0)),{}, {}});
                if(prior.isValid())calendar.nextBusinessDay.insert(prior,d);prior=d;
                if(d<today)daily.candles.append({{time(d,QTime(0,0)),0,QString::number(100+d.day()),QString::number(120+d.day()),QString::number(80+d.day()),QString::number(110+d.day()),"100"},d,time(d,QTime(0,0))});
            }
            calendar.days.insert(d,day);
        }
        minute=daily;minute.nativeIntervalMs=Minute;minute.sourceUrl="https://example.invalid/synthetic-minute";minute.candles.clear();
        minute.candles.append({{time(today,QTime(9,30)),0,"120","122","119","121","10"},today,time(today,QTime(9,31))});
    }
};
const ExtendedLevelCandidate* candidate(const StockPeriodLevelResult& value,const QString& id) {
    for(const auto& row:value.candidates)if(row.id==id)return &row;return nullptr;
}
void addUnrelatedMinuteHistory(Fixture* f){
    const auto current=f->minute.candles.last();f->minute.candles.clear();const auto start=current.candle.startsAtMs-47999*Minute;
    for(int i=0;i<47999;++i){const auto stamp=start+i*Minute;f->minute.candles.append({{stamp,0,"100","100","100","100","1"},QDateTime::fromMSecsSinceEpoch(stamp,QTimeZone("America/New_York")).date(),stamp+Minute});}
    f->minute.candles.append(current);
}
}
class StockPeriodLevelsTest:public QObject {
    Q_OBJECT
private slots:
    void completedPeriodsUseCalendarBoundariesAndExactOhlc();
    void missingImmediatePeriodDoesNotFallBack();
    void unknownHolidayAndAdjustmentSuppressAffectedPeriods();
    void openingGapRequiresFirstCompletedRegularBarAndSameSession();
    void completionAndFutureReceiptsAreNotUsed();
    void dailyChartHidesDayAndRendererRetainsTwoPerSide();
    void koreanSessionRangesNeverUseConsolidatedDailyAsRegular();
    void maximumHistoryUsesOnlyRelevantSessionMinutes();
    void maximumFixtureBenchmark();
};
void StockPeriodLevelsTest::completedPeriodsUseCalendarBoundariesAndExactOhlc() {
    Fixture f;const auto r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);
    QVERIFY(candidate(r,"stock-day-open"));QCOMPARE(candidate(r,"stock-day-open")->price,QString("102"));
    QCOMPARE(candidate(r,"stock-week-open")->price,QString("126"));
    QCOMPARE(candidate(r,"stock-week-high")->price,QString("149"));
    QCOMPARE(candidate(r,"stock-week-low")->price,QString("106"));
    QCOMPARE(candidate(r,"stock-month-open")->price,QString("101"));
    QCOMPARE(candidate(r,"stock-month-high")->price,QString("149"));
    QCOMPARE(candidate(r,"stock-month-low")->price,QString("81"));
    QCOMPARE(candidate(r,"stock-early-high")->price,QString("122"));
    QCOMPARE(candidate(r,"stock-gap-close")->price,QString("112"));
    QCOMPARE(candidate(r,"stock-gap-open")->price,QString("120"));
    QCOMPARE(candidate(r,"stock-gap-open")->throughMs,time(f.today,QTime(9,31)));
    QVERIFY(candidate(r,"stock-week-high")->throughMs<time(QDate(2026,11,2),QTime(0,0)));
}
void StockPeriodLevelsTest::missingImmediatePeriodDoesNotFallBack() {
    Fixture f;for(auto i=f.daily.candles.begin();i!=f.daily.candles.end();++i)if(i->tradingDate==QDate(2026,11,2)){f.daily.candles.erase(i);break;}
    auto r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!candidate(r,"stock-day-open"));QVERIFY(!candidate(r,"stock-early-high"));QVERIFY(!candidate(r,"stock-gap-close"));QVERIFY(candidate(r,"stock-week-open"));
    for(auto i=f.daily.candles.begin();i!=f.daily.candles.end();++i)if(i->tradingDate==QDate(2026,10,28)){f.daily.candles.erase(i);break;}
    r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!candidate(r,"stock-week-open"));QVERIFY(!candidate(r,"stock-month-open"));QVERIFY(!r.reason.isEmpty());
}
void StockPeriodLevelsTest::unknownHolidayAndAdjustmentSuppressAffectedPeriods() {
    Fixture f;f.calendar.days.remove(QDate(2026,10,30));auto r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);
    QVERIFY(!candidate(r,"stock-week-open"));QVERIFY(!candidate(r,"stock-month-open"));QVERIFY(candidate(r,"stock-day-open"));
    f.daily.adjustment=StockAdjustment::CloseSplitAdjustedOnly;QVERIFY(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now).candidates.isEmpty());
    f.daily.adjustment=StockAdjustment::SourceAdjusted;f.calendar.available=false;QVERIFY(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now).candidates.isEmpty());
}
void StockPeriodLevelsTest::openingGapRequiresFirstCompletedRegularBarAndSameSession() {
    Fixture f;f.minute.candles[0].candle.startsAtMs+=Minute;f.minute.candles[0].sourceLabelAtMs+=Minute;
    QVERIFY(!candidate(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now),"stock-gap-open"));
    f=Fixture{};f.minute.requestStartedAtMs=time(f.today,QTime(9,30,30));f.minute.receivedAtMs=f.minute.requestStartedAtMs;
    QVERIFY(!candidate(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now),"stock-gap-open"));
    f=Fixture{};f.minute.adjustment=StockAdjustment::SourceUnadjusted;QVERIFY(!candidate(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now),"stock-gap-open"));
    f=Fixture{};f.minute.instrument.identity="stock:US:OTHER";QVERIFY(!candidate(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now),"stock-gap-open"));
}
void StockPeriodLevelsTest::completionAndFutureReceiptsAreNotUsed() {
    Fixture f;f.daily.requestStartedAtMs=time(QDate(2026,11,2),QTime(19,59));f.daily.receivedAtMs=f.daily.requestStartedAtMs;
    const auto r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!candidate(r,"stock-day-open"));QVERIFY(candidate(r,"stock-week-open"));
    f=Fixture{};f.daily.receivedAtMs=f.now+1;QVERIFY(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now).candidates.isEmpty());
    f=Fixture{};f.calendar.evidence.receivedAtMs=f.now+1;QVERIFY(stockPeriodLevels(f.daily,f.minute,f.calendar,f.now).candidates.isEmpty());
}
void StockPeriodLevelsTest::dailyChartHidesDayAndRendererRetainsTwoPerSide() {
    Fixture f;const auto r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!r.candidates.isEmpty());
    const auto lines=chartLevels(r.candidates,"120","0.01",Day,f.now);QVERIFY(lines.visible.size()<=4);
    for(const auto& row:lines.all)for(const auto& member:row.members)QVERIFY(member.period!=ExtendedLevelPeriod::Day);
}
void StockPeriodLevelsTest::koreanSessionRangesNeverUseConsolidatedDailyAsRegular() {
    Fixture f;f.now=time(f.today,QTime(10,0),"Asia/Seoul");
    f.daily.instrument={"stock:KR:005930","005930","KR","KRW","KOSPI","Asia/Seoul",true,true};
    f.daily.requestStartedAtMs=f.daily.receivedAtMs=f.now;f.calendar.marketCountry="KR";f.calendar.evidence.receivedAtMs=f.now;
    for(auto i=f.calendar.days.begin();i!=f.calendar.days.end();++i)if(i->state==StockCalendarState::Sessions) {
        i->sessions={{StockSessionKind::Regular,time(i.key(),QTime(9,0),"Asia/Seoul"),time(i.key(),QTime(15,30),"Asia/Seoul"),{},{}},
            {StockSessionKind::After,time(i.key(),QTime(15,30),"Asia/Seoul"),time(i.key(),QTime(20,0),"Asia/Seoul"),{},{}}};
    }
    for(auto& row:f.daily.candles)row.candle.startsAtMs=row.sourceLabelAtMs=time(row.tradingDate,QTime(0,0),"Asia/Seoul");
    f.minute=f.daily;f.minute.nativeIntervalMs=Minute;f.minute.candles.clear();
    auto r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(candidate(r,"stock-day-open"));
    QVERIFY(candidate(r,"stock-day-open")->label.contains("통합"));QVERIFY(!candidate(r,"stock-early-high"));QVERIFY(!candidate(r,"stock-gap-open"));
    const QDate monday(2026,11,2);const auto open=time(monday,QTime(9,0),"Asia/Seoul");
    for(int i=0;i<390;++i)f.minute.candles.append({{open+i*Minute,0,"110","123","100","111","1"},monday,open+(i+1)*Minute});
    const auto current=time(f.today,QTime(9,0),"Asia/Seoul");f.minute.candles.append({{current,0,"120","121","119","120","1"},f.today,current+Minute});
    r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(candidate(r,"stock-early-high"));QCOMPARE(candidate(r,"stock-early-high")->price,QString("123"));
    QCOMPARE(candidate(r,"stock-gap-close")->price,QString("111"));
    f.minute.candles.removeAt(200);r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!candidate(r,"stock-early-high"));QVERIFY(candidate(r,"stock-gap-close"));
    f.daily.available=false;r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!candidate(r,"stock-day-open"));QVERIFY(candidate(r,"stock-gap-close"));f.daily.available=true;
    f.minute.instrument.nxtSupported=false;r=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QVERIFY(!candidate(r,"stock-gap-close"));
}
void StockPeriodLevelsTest::maximumHistoryUsesOnlyRelevantSessionMinutes(){
    Fixture f;const auto expected=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);addUnrelatedMinuteHistory(&f);
    const auto actual=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);QCOMPARE(actual.candidates.size(),expected.candidates.size());
    for(int i=0;i<actual.candidates.size();++i){QCOMPARE(actual.candidates[i].id,expected.candidates[i].id);QCOMPARE(actual.candidates[i].price,expected.candidates[i].price);}
}
void StockPeriodLevelsTest::maximumFixtureBenchmark(){
    if(!qEnvironmentVariableIsSet("KEEFI_STOCK_LEVEL_BENCHMARK"))QSKIP("Optional Release 30-run bounded 48000-minute level calculation receipt");
    Fixture f;addUnrelatedMinuteHistory(&f);QList<qint64> samples;
    for(int i=0;i<30;++i){QElapsedTimer timer;timer.start();const auto value=stockPeriodLevels(f.daily,f.minute,f.calendar,f.now);samples.append(timer.nsecsElapsed());QVERIFY(candidate(value,"stock-gap-open"));}
    std::sort(samples.begin(),samples.end());const QJsonObject receipt{{"runs",30},{"nativeMinutes",48000},{"calculatorMedianNs",double((samples[14]+samples[15])/2)},
        {"calculatorP95Ns",double(samples[28])},{"note","StockPeriodLevels pure calculation only; excludes UI painting, networking and real accounts"}};
    qInfo().noquote()<<QJsonDocument(receipt).toJson(QJsonDocument::Compact);
}
QTEST_GUILESS_MAIN(StockPeriodLevelsTest)
#include "tst_stock_period_levels.moc"
