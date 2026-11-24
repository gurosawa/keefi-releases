#include "keefi/StockChartRules.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <limits>

namespace keefi {
namespace {
constexpr qint64 Minute=60000,Day=86400000;
constexpr int MaxNativeRows=48000,MaxOutputRows=200;
bool supported(qint64 v){return v==Minute||v==5*Minute||v==15*Minute||v==60*Minute||v==240*Minute||v==Day;}
bool timestamp(const QString& text,qint64* out){
    static const QRegularExpression zone(QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
    if(!zone.match(text).hasMatch())return false;
    auto time=QDateTime::fromString(text,Qt::ISODateWithMs);if(!time.isValid())time=QDateTime::fromString(text,Qt::ISODate);
    if(!time.isValid()||time.toMSecsSinceEpoch()<=0)return false;*out=time.toMSecsSinceEpoch();return true;
}
bool date(const QString& text,QDate* out){*out=QDate::fromString(text,Qt::ISODate);return out->isValid()&&out->toString(Qt::ISODate)==text;}
bool numeric(const QString& text,bool positive){
    static const QRegularExpression digits(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));
    return text.size()<=30&&digits.match(text).hasMatch()&&(!positive||decimal::positive(text));
}
bool instrumentValid(const StockChartInstrument& i){
    const bool kr=i.marketCountry=="KR",us=i.marketCountry=="US";
    static const QRegularExpression krCode(QStringLiteral("\\A[0-9A-Z]{6}\\z")),usCode(QStringLiteral("\\A[A-Z][A-Z0-9.-]{0,15}\\z"));
    return i.verified&&(kr||us)&&i.identity=="stock:"+i.marketCountry+":"+i.symbol&&
        (kr?krCode:usCode).match(i.symbol).hasMatch()&&i.currency==(kr?"KRW":"USD")&&
        i.marketTimeZone==(kr?"Asia/Seoul":"America/New_York")&&
        (kr?QStringList{"KOSPI","KOSDAQ","KR_ETC"}:QStringList{"NASDAQ","NYSE","AMEX","US_ETC"}).contains(i.market);
}
bool httpValid(const HttpResult& r,qsizetype limit){return r.status==200&&!r.timedOut&&!r.networkError&&!r.body.isEmpty()&&r.body.size()<=limit;}
bool parseSession(const QJsonValue& value,StockSessionKind kind,StockSession* output){
    if(!value.isObject())return false;const auto o=value.toObject();StockSession s;s.kind=kind;
    if(!timestamp(o.value("startTime").toString(),&s.fromMs)||!timestamp(o.value("endTime").toString(),&s.toMs)||
       s.toMs<=s.fromMs||s.toMs-s.fromMs>2*Day)return false;
    for(const auto& field:QStringList{"singlePriceAuctionStartTime","singlePriceAuctionEndTime"}){
        if(!o.contains(field)||o.value(field).isNull())continue;qint64 at=0;
        if(!timestamp(o.value(field).toString(),&at)||at<s.fromMs||at>s.toMs)return false;
        if(field=="singlePriceAuctionStartTime")s.auctionFromMs=at;else s.auctionToMs=at;
    }
    *output=s;return true;
}
bool parseDay(const QJsonValue& value,const QString& country,StockSessionDay* output){
    if(!value.isObject())return false;const auto o=value.toObject();StockSessionDay day;
    if(!date(o.value("date").toString(),&day.tradingDate))return false;
    QJsonObject groups=o;bool complete=true;
    if(country=="KR"){
        if(!o.contains("integrated")){*output=day;return true;}
        if(o.value("integrated").isNull()){day.state=StockCalendarState::ExplicitHoliday;*output=day;return true;}
        if(!o.value("integrated").isObject())return false;groups=o.value("integrated").toObject();
    }
    const QList<QPair<QString,StockSessionKind>> kinds=country=="KR"?
        QList<QPair<QString,StockSessionKind>>{{"preMarket",StockSessionKind::Pre},{"regularMarket",StockSessionKind::Regular},{"afterMarket",StockSessionKind::After}}:
        QList<QPair<QString,StockSessionKind>>{{"dayMarket",StockSessionKind::Day},{"preMarket",StockSessionKind::Pre},{"regularMarket",StockSessionKind::Regular},{"afterMarket",StockSessionKind::After}};
    const QTimeZone zone(country=="KR"?"Asia/Seoul":"America/New_York");
    const auto from=QDateTime(day.tradingDate.addDays(country=="KR"?0:-1),QTime(0,0),zone).toMSecsSinceEpoch();
    const auto to=QDateTime(day.tradingDate.addDays(1),QTime(0,0),zone).toMSecsSinceEpoch();
    for(const auto& kind:kinds){
        if(!groups.contains(kind.first)){complete=false;continue;}
        const auto raw=groups.value(kind.first);if(raw.isNull())continue;
        StockSession s;if(!parseSession(raw,kind.second,&s)||s.fromMs<from||s.toMs>to)return false;day.sessions.append(s);
    }
    std::sort(day.sessions.begin(),day.sessions.end(),[](const auto&a,const auto&b){return a.fromMs<b.fromMs;});
    for(int i=1;i<day.sessions.size();++i)if(day.sessions[i].fromMs<day.sessions[i-1].toMs)return false;
    day.state=!complete?StockCalendarState::Unknown:day.sessions.isEmpty()?StockCalendarState::ExplicitHoliday:StockCalendarState::Sessions;
    *output=day;return true;
}
QList<StockSession> selectedSessions(const StockSessionDay& day,bool extended){
    QList<StockSession> result;if(day.state!=StockCalendarState::Sessions)return result;
    for(const auto& s:day.sessions)if(extended||s.kind==StockSessionKind::Regular)result.append(s);return result;
}
struct Slot{qint64 start=0;QDate date;};
std::optional<Slot> nextSlot(qint64 start,const QDate& tradingDate,qint64 interval,const StockCalendar& calendar,bool extended,const QString& zoneName){
    const auto day=calendar.days.constFind(tradingDate);if(day==calendar.days.cend()||day->state!=StockCalendarState::Sessions)return {};
    if(interval==Day){
        const auto link=calendar.nextBusinessDay.constFind(tradingDate);if(link==calendar.nextBusinessDay.cend())return {};
        const auto next=calendar.days.constFind(*link);if(next==calendar.days.cend()||next->state!=StockCalendarState::Sessions)return {};
        return Slot{QDateTime(*link,QTime(0,0),QTimeZone(zoneName.toUtf8())).toMSecsSinceEpoch(),*link};
    }
    const auto sessions=selectedSessions(*day,extended);
    for(const auto& s:sessions){
        if(s.toMs-s.fromMs<interval)continue;
        if(start<s.fromMs)return Slot{s.fromMs,tradingDate};
        if(start>=s.fromMs&&start<s.toMs){
            const auto at=s.fromMs+((start-s.fromMs)/interval+1)*interval;
            if(at<=s.toMs-interval)return Slot{at,tradingDate};
        }
    }
    const auto link=calendar.nextBusinessDay.constFind(tradingDate);if(link==calendar.nextBusinessDay.cend())return {};
    const auto next=calendar.days.constFind(*link);if(next==calendar.days.cend())return {};
    for(const auto& s:selectedSessions(*next,extended))if(s.toMs-s.fromMs>=interval)return Slot{s.fromMs,*link};
    return {};
}
void exclude(ChartStudyTimelineRow* row,StockBarExclusion why,const QString& reason){
    if(!row->exclusions.contains(why))row->exclusions.append(why);row->eligible=false;
    if(!row->reason.isEmpty())row->reason+=QStringLiteral(" · ");row->reason+=reason;
}
void restrictionMask(ChartStudyTimelineRow* state,const MarketCandle& candle,const StockRestrictions& restrictions){
    const StockRestrictionRange* covered=nullptr;
    if(restrictions.historicalCoverage==StockHistoryCoverage::VerifiedIntervals)
        for(const auto& range:restrictions.intervals)if(range.fromMs<=state->startsAtMs&&range.toMs>=state->completesAtMs&&range.evidence.complete){covered=&range;break;}
    if(!covered){exclude(state,StockBarExclusion::RestrictionCoverageUnknown,QStringLiteral("과거 VI·가격 제한 제외 근거 미제공"));return;}
    if(covered->vi)exclude(state,StockBarExclusion::VolatilityInterruption,QStringLiteral("확인된 VI 구간"));
    const bool badUpper=covered->upperLimit&&!numeric(*covered->upperLimit,true),badLower=covered->lowerLimit&&!numeric(*covered->lowerLimit,true);
    const bool inverted=!badUpper && !badLower && covered->upperLimit && covered->lowerLimit &&
        decimal::compare(*covered->lowerLimit,*covered->upperLimit)>0;
    if(badUpper||badLower||inverted){exclude(state,StockBarExclusion::InvalidPriceLimit,QStringLiteral("가격 제한 근거 형식 미확인"));return;}
    if((covered->upperLimit&&decimal::compare(candle.high,*covered->upperLimit)>=0)||
       (covered->lowerLimit&&decimal::compare(candle.low,*covered->lowerLimit)<=0))
        exclude(state,StockBarExclusion::PriceLimit,QStringLiteral("확인된 상·하한가 구간"));
}
StockPendingCandle pending(const StockObservedCandle& observation,qint64 completion,StockSessionKind session,StockBarExclusion reason,const QString& text){
    return {observation,completion,session,{reason},text};
}
}

StockCalendar parseStockCalendar(const QString& country,const HttpResult& response,qint64 receivedAtMs){
    StockCalendar result;
    auto invalid=[&](){result={};result.reason=QStringLiteral("공식 장 캘린더 형식 또는 범위가 올바르지 않습니다");return result;};
    if((country!="KR"&&country!="US")||receivedAtMs<=0||!httpValid(response,128*1024))return invalid();
    const auto doc=parseExactJson(response.body);if(!doc.isObject()||!doc.object().value("result").isObject())return invalid();
    const auto root=doc.object().value("result").toObject();StockSessionDay previous,today,next;
    if(!parseDay(root.value("previousBusinessDay"),country,&previous)||!parseDay(root.value("today"),country,&today)||!parseDay(root.value("nextBusinessDay"),country,&next)||
       !(previous.tradingDate<today.tradingDate&&today.tradingDate<next.tradingDate))return invalid();
    result.marketCountry=country;for(const auto& day:{previous,today,next})result.days.insert(day.tradingDate,day);
    if(previous.state==StockCalendarState::Sessions&&next.state==StockCalendarState::Sessions){
        if(today.state==StockCalendarState::ExplicitHoliday)result.nextBusinessDay.insert(previous.tradingDate,next.tradingDate);
        else if(today.state==StockCalendarState::Sessions){result.nextBusinessDay.insert(previous.tradingDate,today.tradingDate);result.nextBusinessDay.insert(today.tradingDate,next.tradingDate);}
    }
    if(previous.state==StockCalendarState::Unknown||today.state==StockCalendarState::Unknown||next.state==StockCalendarState::Unknown)
        result.reason=QStringLiteral("장 세션 일부 미제공 · 해당 구간 판정 대기");
    result.available=true;result.evidence={QStringLiteral("토스 공식 장 캘린더"),"https://openapi.tossinvest.com/api/v1/market-calendar/"+country,
        "public:toss:calendar:"+country,0,receivedAtMs,true};return result;
}

StockCandlePage parseTossStockCandles(const StockChartInstrument& instrument,qint64 interval,StockAdjustment adjustment,
    const HttpResult& response,qint64 requestStartedAtMs,qint64 receivedAtMs){
    StockCandlePage result;
    auto invalid=[&](){result={};result.reason=QStringLiteral("공식 주식 캔들 형식·identity·범위가 올바르지 않습니다");return result;};
    if(!instrumentValid(instrument)||(interval!=Minute&&interval!=Day)||requestStartedAtMs<=0||receivedAtMs<requestStartedAtMs||!httpValid(response,512*1024))return invalid();
    const auto doc=parseExactJson(response.body);if(!doc.isObject()||!doc.object().value("result").isObject())return invalid();
    const auto root=doc.object().value("result").toObject();if(!root.value("candles").isArray())return invalid();
    const auto raw=root.value("candles").toArray();if(raw.size()>200)return invalid();
    qint64 previous=std::numeric_limits<qint64>::max();const QTimeZone zone(instrument.marketTimeZone.toUtf8());
    for(const auto& value:raw){
        if(!value.isObject())return invalid();const auto o=value.toObject();qint64 label=0;
        if(!timestamp(o.value("timestamp").toString(),&label)||label>=previous||label%Minute!=0||o.value("currency").toString()!=instrument.currency)return invalid();
        MarketCandle candle;const auto local=QDateTime::fromMSecsSinceEpoch(label,zone);
        if(interval==Day&&local.time()!=QTime(0,0))return invalid();
        candle.startsAtMs=interval==Minute?label-Minute:label;
        candle.open=o.value("openPrice").toString();candle.high=o.value("highPrice").toString();candle.low=o.value("lowPrice").toString();
        candle.close=o.value("closePrice").toString();candle.volume=o.value("volume").toString();
        if(candle.startsAtMs<=0||!numeric(candle.open,true)||!numeric(candle.high,true)||!numeric(candle.low,true)||!numeric(candle.close,true)||!numeric(candle.volume,false)||
           decimal::compare(candle.low,candle.open)>0||decimal::compare(candle.low,candle.close)>0||
           decimal::compare(candle.high,candle.open)<0||decimal::compare(candle.high,candle.close)<0||decimal::compare(candle.low,candle.high)>0)return invalid();
        QDate tradingDate=local.date();
        if(interval==Minute&&instrument.marketCountry=="US"){
            // Native labels are END times: the minute ending exactly at20:00
            // belongs to the preceding trading day; the minute starting there does not.
            const auto startLocal=QDateTime::fromMSecsSinceEpoch(candle.startsAtMs,zone);
            tradingDate=startLocal.date().addDays(startLocal.time()>=QTime(20,0)?1:0);
        }
        result.candles.prepend({candle,tradingDate,label});previous=label;
    }
    if(root.contains("nextBefore")&&!root.value("nextBefore").isNull()){
        qint64 next=0;if(!root.value("nextBefore").isString()||!timestamp(root.value("nextBefore").toString(),&next)||
            (!result.candles.isEmpty()&&next>result.candles.first().sourceLabelAtMs))return invalid();
        result.nextBefore=root.value("nextBefore").toString();
    }
    result.available=true;result.instrument=instrument;result.source=StockSourceKind::Toss;result.adjustment=adjustment;
    result.nativeIntervalMs=interval;result.requestStartedAtMs=requestStartedAtMs;result.receivedAtMs=receivedAtMs;
    result.sourceUrl=QStringLiteral("https://openapi.tossinvest.com/api/v1/candles");return result;
}

StockChartSnapshot buildStockChart(const StockCandlePage& page,const StockCalendar& calendar,const StockRestrictions& restrictions,const StockChartOptions& options){
    StockChartSnapshot result;result.series.intervalMs=options.intervalMs;
    auto invalid=[&](){result={};result.series.intervalMs=options.intervalMs;result.series.reason=QStringLiteral("주식 캔들 구성 입력 또는 자료 범위가 올바르지 않습니다");return result;};
    if(!page.available||!instrumentValid(page.instrument)||!supported(options.intervalMs)||(page.nativeIntervalMs!=Minute&&page.nativeIntervalMs!=Day)||
       (page.nativeIntervalMs==Day&&options.intervalMs!=Day)||(page.nativeIntervalMs==Minute&&options.intervalMs==Day)||page.candles.size()>MaxNativeRows||
       page.requestStartedAtMs<=0||page.receivedAtMs<page.requestStartedAtMs)return invalid();
    result.source=page.source;result.adjustment=page.adjustment;result.restrictions=restrictions;
    result.intradayAllowed=page.source==StockSourceKind::Toss;
    result.realtimeAllowed=result.intradayAllowed&&options.realtimeAllowed;
    result.series.identity=page.instrument.identity;result.series.asset=page.instrument.symbol;result.series.currency=page.instrument.currency;
    result.series.sourceUrl=page.sourceUrl;result.series.receivedAtMs=page.receivedAtMs;result.series.sourceTimeMs=0;
    result.sessionNote=page.instrument.marketCountry=="KR"?QStringLiteral("KRX+NXT 통합 · 거래소별 분리 미제공"):QStringLiteral("미국 일부 거래소 · 일봉 OHLC 정규장 / 거래량 종합");
    result.adjustmentNote=page.adjustment==StockAdjustment::SourceAdjusted?QStringLiteral("원천 수정주가 적용 · 배당 전부 반영 보증 아님"):
        page.adjustment==StockAdjustment::SourceUnadjusted?QStringLiteral("원천 수정주가 미적용 · 과거 원가 복구 한계"):QStringLiteral("수정주가 범위 미확인");
    result.timeline.adjustedComparable=page.adjustment==StockAdjustment::SourceAdjusted;
    if(!result.timeline.adjustedComparable)result.timeline.comparisonReason=QStringLiteral("수정주가 비교 범위 미확인");
    struct Evaluated {StockObservedCandle observation;ChartStudyTimelineRow state;qint64 anchor=0,sessionEnd=0;};
    QList<Evaluated> closed; qint64 prior=0;
    for(const auto& observation:page.candles){
        const auto& candle=observation.candle;
        if(candle.startsAtMs<=prior||!numeric(candle.open,true)||!numeric(candle.high,true)||!numeric(candle.low,true)||!numeric(candle.close,true)||!numeric(candle.volume,false)||
           decimal::compare(candle.low,candle.open)>0||decimal::compare(candle.low,candle.close)>0||decimal::compare(candle.high,candle.open)<0||decimal::compare(candle.high,candle.close)<0)return invalid();
        prior=candle.startsAtMs;
        const auto day=calendar.days.constFind(observation.tradingDate);
        if(!calendar.available||calendar.marketCountry!=page.instrument.marketCountry||day==calendar.days.cend()||day->state!=StockCalendarState::Sessions){
            result.pending.append(pending(observation,0,StockSessionKind::Unknown,StockBarExclusion::CalendarUnknown,QStringLiteral("해당 거래일의 장 캘린더 미확인")));continue;
        }
        ChartStudyTimelineRow state;state.startsAtMs=candle.startsAtMs;state.tradingDate=observation.tradingDate;state.eligible=true;
        qint64 completion=observation.sourceLabelAtMs,anchor=0,sessionEnd=0;
        if(page.nativeIntervalMs==Day){
            completion=0;for(const auto& session:day->sessions)completion=qMax(completion,session.toMs);
            if(page.instrument.marketCountry=="US")completion=qMax(completion,QDateTime(observation.tradingDate,QTime(20,0),QTimeZone("America/New_York")).toMSecsSinceEpoch());
            else if(std::none_of(day->sessions.cbegin(),day->sessions.cend(),[](const auto&s){return s.kind==StockSessionKind::After;}))completion=0;
            state.session=StockSessionKind::Regular;
        }else{
            if(observation.sourceLabelAtMs!=candle.startsAtMs+Minute)return invalid();
            const auto local=QDateTime::fromMSecsSinceEpoch(observation.sourceLabelAtMs,QTimeZone("Asia/Seoul"));
            const bool kr=page.instrument.marketCountry=="KR";
            const bool closing=kr&&(local.time()==QTime(15,30)||local.time()==QTime(15,31));
            if(closing&&!page.instrument.nxtSupported){
                result.pending.append(pending(observation,0,StockSessionKind::Unknown,StockBarExclusion::SessionUnknown,QStringLiteral("NXT 지원 metadata 미제공 · 종가 봉 완료 보류")));continue;
            }
            if(kr&&page.instrument.nxtSupported==std::optional<bool>(false)&&local.time()==QTime(15,30)){
                completion=QDateTime(observation.tradingDate,QTime(16,0),QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();state.session=StockSessionKind::Mixed;
                exclude(&state,StockBarExclusion::MixedSession,QStringLiteral("종가·시간외종가 합산 특수 봉"));
            }else{
                for(const auto& session:day->sessions)if(candle.startsAtMs>=session.fromMs&&observation.sourceLabelAtMs<=session.toMs){state.session=session.kind;anchor=session.fromMs;sessionEnd=session.toMs;break;}
                if(kr&&page.instrument.nxtSupported==std::optional<bool>(true)&&local.time()==QTime(15,31)){
                    state.session=StockSessionKind::Mixed;exclude(&state,StockBarExclusion::MixedSession,QStringLiteral("종가 단일가와 통합 세션 경계 혼합"));
                }
            }
            if(state.session!=StockSessionKind::Mixed&&state.session!=StockSessionKind::Regular&&!options.includeExtended)continue;
        }
        state.completesAtMs=completion;
        if(completion<=0||page.requestStartedAtMs<completion){
            const auto why=state.session==StockSessionKind::Mixed?StockBarExclusion::MutableClosing:StockBarExclusion::Incomplete;
            result.pending.append(pending(observation,completion,state.session,why,completion?QStringLiteral("완료 경계 이후 REST 대사 대기"):QStringLiteral("전체 집계 종료 근거 미제공")));continue;
        }
        if(state.session==StockSessionKind::Unknown){result.pending.append(pending(observation,completion,state.session,StockBarExclusion::SessionUnknown,QStringLiteral("세션 소속 미확인")));continue;}
        restrictionMask(&state,candle,restrictions);
        if(page.adjustment==StockAdjustment::Unknown)exclude(&state,StockBarExclusion::AdjustmentUnknown,QStringLiteral("수정주가 범위 미확인"));
        closed.append({observation,state,anchor,sessionEnd});
    }
    if(page.nativeIntervalMs==options.intervalMs){
        for(const auto& item:closed){result.series.candles.append(item.observation.candle);result.timeline.rows.append(item.state);}
        if(page.nativeIntervalMs==Minute&&!result.pending.isEmpty()){
            const auto& last=result.pending.last();
            if(last.completesAtMs==last.observation.sourceLabelAtMs&&last.session!=StockSessionKind::Mixed&&last.session!=StockSessionKind::Unknown&&
               (result.series.candles.isEmpty()||last.observation.candle.startsAtMs>result.series.candles.last().startsAtMs))result.series.forming=last.observation.candle;
        }
    }else{
        QMap<qint64,QList<Evaluated>> groups;
        for(const auto& item:closed){
            if(item.anchor<=0||item.state.session==StockSessionKind::Mixed){
                result.pending.append(pending(item.observation,item.state.completesAtMs,item.state.session,StockBarExclusion::MixedSession,QStringLiteral("특수 봉은 정상 분봉 집계에서 제외")));continue;
            }
            const auto start=item.anchor+(item.observation.candle.startsAtMs-item.anchor)/options.intervalMs*options.intervalMs;
            groups[start].append(item);
        }
        const int expected=int(options.intervalMs/Minute);
        for(auto it=groups.cbegin();it!=groups.cend();++it){
            const auto& children=it.value();bool complete=children.size()==expected&&it.key()<=children.first().sessionEnd-options.intervalMs;
            for(int i=0;complete&&i<children.size();++i)complete=children[i].observation.candle.startsAtMs==it.key()+i*Minute&&children[i].state.completesAtMs==it.key()+(i+1)*Minute;
            if(!complete){for(const auto& child:children)result.pending.append(pending(child.observation,child.state.completesAtMs,child.state.session,StockBarExclusion::MissingChild,QStringLiteral("완전한 상위 봉 구성 표본 부족")));continue;}
            MarketCandle candle=children.first().observation.candle;candle.startsAtMs=it.key();candle.close=children.last().observation.candle.close;candle.volume="0";
            auto state=children.first().state;state.startsAtMs=it.key();state.completesAtMs=children.last().state.completesAtMs;
            for(const auto& child:children){
                const auto& c=child.observation.candle;if(decimal::compare(c.high,candle.high)>0)candle.high=c.high;if(decimal::compare(c.low,candle.low)<0)candle.low=c.low;
                candle.volume=decimal::add(candle.volume,c.volume);if(!child.state.eligible){state.eligible=false;for(const auto reason:child.state.exclusions)if(!state.exclusions.contains(reason))state.exclusions.append(reason);if(state.reason.isEmpty())state.reason=child.state.reason;}
            }
            result.series.candles.append(candle);result.timeline.rows.append(state);
        }
    }
    while(result.series.candles.size()>MaxOutputRows){result.series.candles.removeFirst();result.timeline.rows.removeFirst();}
    for(int i=0;i<result.timeline.rows.size();++i){
        auto& state=result.timeline.rows[i];if(i==0){state.missingSlotsBefore=0;continue;}
        const auto& previous=result.timeline.rows[i-1];
        auto expected=nextSlot(previous.startsAtMs,previous.tradingDate,options.intervalMs,calendar,options.includeExtended,page.instrument.marketTimeZone);
        if(expected&&expected->start==state.startsAtMs){state.connectsPrevious=true;state.missingSlotsBefore=0;}
        else{
            if(expected&&state.startsAtMs>expected->start){int missing=0;
                while(expected&&expected->start<state.startsAtMs&&missing<MaxNativeRows){++missing;expected=nextSlot(expected->start,expected->date,options.intervalMs,calendar,options.includeExtended,page.instrument.marketTimeZone);}
                if(expected&&expected->start==state.startsAtMs)state.missingSlotsBefore=missing;
            }
            if(state.reason.isEmpty())state.reason=QStringLiteral("장중 관측 공백 또는 장 캘린더 연속성 미확인");
        }
    }
    if(!result.timeline.rows.isEmpty()){
        auto next=Slot{result.timeline.rows.last().startsAtMs,result.timeline.rows.last().tradingDate};bool complete=true;
        for(int i=0;i<20;++i){const auto value=nextSlot(next.start,next.date,options.intervalMs,calendar,options.includeExtended,page.instrument.marketTimeZone);if(!value){complete=false;break;}result.timeline.nextStartsAtMs.append(value->start);next=*value;}
        result.timeline.projectionCovered=complete;
        if(!complete)result.timeline.nextStartsAtMs.clear();
        result.calendarAvailable=true;
    }
    if(!result.timeline.projectionCovered)result.timeline.projectionReason=QStringLiteral("다음 20봉의 실제 거래 좌표를 확인할 장 캘린더 부족");
    if(!result.calendarAvailable)result.calendarReason=QStringLiteral("완료 봉의 장 캘린더 또는 완료 근거 미확인");
    result.series.available=!result.series.candles.isEmpty()||result.series.forming.has_value()||!result.pending.isEmpty();
    if(result.series.candles.isEmpty())result.series.reason=QStringLiteral("완료된 주식 봉 없음 · 원천·장 경계 확인 대기");
    if(!result.series.candles.isEmpty()){
        result.studies=keefiChartStudies(result.series.candles,options.intervalMs,result.timeline);
        result.marketContextStudies=candleStudies(result.series.candles,options.intervalMs,result.timeline);
        result.compression=candleCompression(result.series.candles,options.intervalMs,result.timeline);
    }
    return result;
}
} // namespace keefi
