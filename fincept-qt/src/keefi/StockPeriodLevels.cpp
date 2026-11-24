#include "keefi/StockPeriodLevels.h"
#include "keefi/StockChartRules.h"
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>

namespace keefi {
namespace {
constexpr qint64 Minute=60000,Day=86400000;
struct ProvenBar { MarketCandle candle;qint64 completesAtMs=0; };
std::optional<StockSession> regularSession(const StockCalendar& calendar,QDate date) {
    const auto day=calendar.days.constFind(date);if(day==calendar.days.cend()||day->state!=StockCalendarState::Sessions)return {};
    std::optional<StockSession> found;
    for(const auto& session:day->sessions)if(session.kind==StockSessionKind::Regular){if(found||session.fromMs<=0||session.toMs<=session.fromMs)return {};found=session;}
    return found;
}
bool calendarRange(const StockCalendar& calendar,QDate first,QDate last,QList<QDate>* trading) {
    if(!first.isValid()||!last.isValid()||first>last||first.daysTo(last)>62)return false;
    for(auto day=first;day<=last;day=day.addDays(1)) {
        const auto found=calendar.days.constFind(day);if(found==calendar.days.cend()||found->tradingDate!=day)return false;
        if(found->state==StockCalendarState::ExplicitHoliday)continue;
        if(!regularSession(calendar,day))return false;trading->append(day);
    }
    return !trading->isEmpty();
}
bool readable(const StockCandlePage& page,qint64 native,qint64 now) {
    return page.available&&page.source==StockSourceKind::Toss&&page.instrument.verified&&
        page.nativeIntervalMs==native&&page.adjustment==StockAdjustment::SourceAdjusted&&
        page.requestStartedAtMs>0&&page.receivedAtMs>=page.requestStartedAtMs&&page.receivedAtMs<=now&&!page.sourceUrl.isEmpty();
}
QDate previousTradingDay(const StockCalendar& calendar,QDate today){
    for(auto date=today.addDays(-1);date>=today.addDays(-32);date=date.addDays(-1)){
        const auto entry=calendar.days.constFind(date);if(entry==calendar.days.cend()||entry->state==StockCalendarState::Unknown)return {};
        if(entry->state==StockCalendarState::ExplicitHoliday)continue;return regularSession(calendar,date)?date:QDate{};
    }return {};
}
QDate firstTradingDay(const StockCalendar& calendar,QDate today){
    for(auto date=today.addDays(1-today.dayOfWeek());date<=today;date=date.addDays(1)){
        const auto entry=calendar.days.constFind(date);if(entry==calendar.days.cend()||entry->state==StockCalendarState::Unknown)return {};
        if(entry->state==StockCalendarState::ExplicitHoliday)continue;return regularSession(calendar,date)?date:QDate{};
    }return {};
}
}
QList<StockLevelMinuteWindow> stockLevelMinuteWindows(const StockChartInstrument& instrument,const StockCalendar& calendar,qint64 now){
    QList<StockLevelMinuteWindow> windows;const QTimeZone zone(instrument.marketTimeZone.toUtf8());
    if(!instrument.verified||!zone.isValid()||!calendar.available||calendar.marketCountry!=instrument.marketCountry)return windows;
    const auto today=QDateTime::fromMSecsSinceEpoch(now,zone).date();
    if(const auto session=regularSession(calendar,today);session&&session->fromMs+Minute<=now)windows.append({session->fromMs,session->fromMs+Minute});
    if(const auto session=regularSession(calendar,previousTradingDay(calendar,today));session&&session->toMs<=now)windows.append({session->toMs-Minute,session->toMs});
    if(instrument.marketCountry=="KR")if(const auto session=regularSession(calendar,firstTradingDay(calendar,today));session&&session->toMs<=now)windows.append({session->fromMs,session->toMs});
    return windows;
}
StockPeriodLevelResult stockPeriodLevels(const StockCandlePage& daily,const StockCandlePage& minute,
    const StockCalendar& calendar,qint64 now) {
    StockPeriodLevelResult result;
    const auto unavailable=[&](const QString& reason){result.reason=reason;return result;};
    const bool dailyReadable=readable(daily,Day,now);
    if(!dailyReadable&&!readable(minute,Minute,now))return unavailable(QStringLiteral("완료 일봉·수정주가 범위를 확인할 수 없어요"));
    const auto& instrument=daily.instrument.verified?daily.instrument:minute.instrument;
    if(!calendar.available||calendar.marketCountry!=instrument.marketCountry||!calendar.evidence.complete||
       calendar.evidence.receivedAtMs<=0||calendar.evidence.receivedAtMs>now||calendar.evidence.sourceTimeMs>now)
        return unavailable(QStringLiteral("공식 거래일 캘린더를 확인할 수 없어요"));
    const QTimeZone zone(instrument.marketTimeZone.toUtf8());if(!zone.isValid())return unavailable(QStringLiteral("시장 시간대를 확인할 수 없어요"));
    const auto today=QDateTime::fromMSecsSinceEpoch(now,zone).date();
    const auto monday=today.addDays(1-today.dayOfWeek());const QDate month(today.year(),today.month(),1);
    StockChartOptions dailyOptions;dailyOptions.intervalMs=Day;
    const auto closed=dailyReadable?buildStockChart(daily,calendar,{},dailyOptions):StockChartSnapshot{};
    QMap<QDate,ProvenBar> days;
    for(int i=0;i<closed.series.candles.size()&&i<closed.timeline.rows.size();++i) {
        const auto& proof=closed.timeline.rows.at(i);if(proof.completesAtMs<=now)days.insert(proof.tradingDate,{closed.series.candles.at(i),proof.completesAtMs});
    }
    // A separate minute page is optional. Chunking preserves the chart's 200-row
    // display limit while validating every source minute used for session ranges.
    QMap<qint64,ProvenBar> minutes;
    const bool sameMinute=readable(minute,Minute,now)&&minute.instrument.identity==instrument.identity&&
        minute.instrument.currency==instrument.currency&&minute.instrument.marketCountry==instrument.marketCountry&&
        minute.instrument.marketTimeZone==instrument.marketTimeZone&&minute.candles.size()<=48000;
    if(sameMinute) {
        const auto windows=stockLevelMinuteWindows(instrument,calendar,now);QList<StockObservedCandle> relevant;
        bool ordered=true;qint64 previous=0;for(const auto& row:minute.candles){if(row.candle.startsAtMs<=previous){ordered=false;break;}previous=row.candle.startsAtMs;
            for(const auto& window:windows)if(row.candle.startsAtMs>=window.fromMs&&row.sourceLabelAtMs<=window.throughMs){relevant.append(row);break;}}
        if(ordered)for(int offset=0;offset<relevant.size();offset+=200) {
            auto part=minute;part.candles=relevant.mid(offset,200);StockChartOptions options;options.intervalMs=Minute;
            const auto bars=buildStockChart(part,calendar,{},options);
            for(int i=0;i<bars.series.candles.size()&&i<bars.timeline.rows.size();++i) {
                const auto& proof=bars.timeline.rows.at(i);if(proof.session!=StockSessionKind::Regular||proof.completesAtMs>now)continue;
                minutes.insert(proof.startsAtMs,{bars.series.candles.at(i),proof.completesAtMs});
            }
        }
    }
    const auto known=qMax(dailyReadable?daily.receivedAtMs:qint64(0),calendar.evidence.receivedAtMs);
    QStringList missing;
    const auto add=[&](const QString& id,const QString& label,const QString& price,ExtendedLevelPeriod period,qint64 from,qint64 through,qint64 observed,const QString& source){
        result.candidates.append({id,label,price,source,period,from,through,observed,true});
    };
    const auto group=[&](QDate first,QDate last,const QString& id,const QString& label,ExtendedLevelPeriod period){
        QList<QDate> dates;if(!calendarRange(calendar,first,last,&dates)){missing.append(label+QStringLiteral(" 거래일 미확인"));return;}
        for(const auto date:dates)if(!days.contains(date)){missing.append(label+QStringLiteral(" 완료 기록 부족"));return;}
        const auto& firstBar=days[dates.first()];const auto& lastBar=days[dates.last()];QString high=firstBar.candle.high,low=firstBar.candle.low;
        for(const auto date:dates){const auto& bar=days[date].candle;if(decimal::compare(bar.high,high)>0)high=bar.high;if(decimal::compare(bar.low,low)<0)low=bar.low;}
        const auto scope=instrument.marketCountry=="KR"?QStringLiteral(" 통합"):QString{};
        add(id+"-open",label+scope+QStringLiteral(" 시가"),firstBar.candle.open,period,firstBar.candle.startsAtMs,lastBar.completesAtMs,known,daily.sourceUrl);
        add(id+"-high",label+scope+QStringLiteral(" 고가"),high,period,firstBar.candle.startsAtMs,lastBar.completesAtMs,known,daily.sourceUrl);
        add(id+"-low",label+scope+QStringLiteral(" 저가"),low,period,firstBar.candle.startsAtMs,lastBar.completesAtMs,known,daily.sourceUrl);
    };
    const auto previousDay=previousTradingDay(calendar,today);
    if(previousDay.isValid())group(previousDay,previousDay,"stock-day",QStringLiteral("전일"),ExtendedLevelPeriod::Day);
    else missing.append(QStringLiteral("직전 거래일 미확인"));
    group(monday.addDays(-7),monday.addDays(-1),"stock-week",QStringLiteral("전주"),ExtendedLevelPeriod::Week);
    group(month.addMonths(-1),month.addDays(-1),"stock-month",QStringLiteral("전월"),ExtendedLevelPeriod::Month);
    const auto firstDay=firstTradingDay(calendar,today);
    bool early=false;
    if(firstDay.isValid()) {
        const auto session=regularSession(calendar,firstDay);
        if(instrument.marketCountry=="US"&&days.contains(firstDay)) {
            const auto& bar=days[firstDay];add("stock-early-high",QStringLiteral("주 첫 거래일 고가"),bar.candle.high,ExtendedLevelPeriod::EarlyWeek,session->fromMs,bar.completesAtMs,known,daily.sourceUrl);
            add("stock-early-low",QStringLiteral("주 첫 거래일 저가"),bar.candle.low,ExtendedLevelPeriod::EarlyWeek,session->fromMs,bar.completesAtMs,known,daily.sourceUrl);early=true;
        } else if(session&&session->toMs<=now&&(session->toMs-session->fromMs)%Minute==0) {
            QString high,low;bool covered=true;
            for(qint64 stamp=session->fromMs;stamp<session->toMs;stamp+=Minute) {
                const auto bar=minutes.constFind(stamp);if(bar==minutes.cend()||bar->completesAtMs!=stamp+Minute){covered=false;break;}
                if(high.isEmpty()||decimal::compare(bar->candle.high,high)>0)high=bar->candle.high;if(low.isEmpty()||decimal::compare(bar->candle.low,low)<0)low=bar->candle.low;
            }
            if(covered&&!high.isEmpty()) {
                const auto observed=qMax(known,minute.receivedAtMs);add("stock-early-high",QStringLiteral("주 첫 거래일 정규장 고가"),high,ExtendedLevelPeriod::EarlyWeek,session->fromMs,session->toMs,observed,minute.sourceUrl);
                add("stock-early-low",QStringLiteral("주 첫 거래일 정규장 저가"),low,ExtendedLevelPeriod::EarlyWeek,session->fromMs,session->toMs,observed,minute.sourceUrl);early=true;
            }
        }
    }
    if(!early)missing.append(QStringLiteral("주 첫 거래일 정규장 완료 기록 부족"));
    bool gap=false;const auto currentSession=regularSession(calendar,today),previousSession=regularSession(calendar,previousDay);
    if(currentSession&&previousSession&&minutes.contains(currentSession->fromMs)) {
        const auto& opening=minutes[currentSession->fromMs];QString close;
        if(instrument.marketCountry=="US"&&days.contains(previousDay))close=days[previousDay].candle.close;
        else {const auto closing=minutes.constFind(previousSession->toMs-Minute);if(closing!=minutes.cend()&&closing->completesAtMs==previousSession->toMs)close=closing->candle.close;}
        if(!close.isEmpty()&&opening.completesAtMs==currentSession->fromMs+Minute) {
            gap=true;if(decimal::compare(close,opening.candle.open)!=0) {
                const auto observed=qMax(known,minute.receivedAtMs);
                add("stock-gap-close",QStringLiteral("시가 갭 · 전일 정규장 종가"),close,ExtendedLevelPeriod::OpeningGap,previousSession->toMs,opening.completesAtMs,observed,instrument.marketCountry=="US"&&days.contains(previousDay)?daily.sourceUrl:minute.sourceUrl);
                add("stock-gap-open",QStringLiteral("시가 갭 · 첫 완료 봉 시가"),opening.candle.open,ExtendedLevelPeriod::OpeningGap,previousSession->toMs,opening.completesAtMs,observed,minute.sourceUrl);
            }
        }
    }
    if(!gap)missing.append(QStringLiteral("시가 갭 정규장 경계 기록 부족"));
    result.reason=missing.join(QStringLiteral(" · "));return result;
}
}
