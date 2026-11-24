#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/StockChartRules.h"
#include "keefi/StockPeriodLevels.h"
#include "keefi/TradingTransport.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TossMarketStream.h"
#include "keefi/KiwoomMarketStream.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <QScopedValueRollback>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <limits>

namespace keefi {
QString stockChartConnectionMessage(StockChartConnectionState state){
    switch(state){
    case StockChartConnectionState::VaultClosed:return QObject::tr("금고를 열어 증권 연결을 확인해 주세요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요");
    case StockChartConnectionState::NoBroker:return QObject::tr("연결된 증권사가 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요");
    case StockChartConnectionState::OtherBroker:return QObject::tr("현재 연결로는 이 자료를 조회할 수 없어요. 토스증권은 국내·미국 주식, 키움증권은 국내 일봉을 지원해요");
    case StockChartConnectionState::AuthenticationRequired:return QObject::tr("토스증권 인증을 다시 확인해 주세요. 키움증권으로는 국내 일봉을 볼 수 있어요");
    case StockChartConnectionState::KiwoomAuthenticationRequired:return QObject::tr("키움증권 인증을 다시 확인해 주세요. 국내 일봉은 키움증권 또는 토스증권에서 볼 수 있어요");
    case StockChartConnectionState::Ready:return {};
    }return {};
}
namespace {
constexpr qint64 Minute=60000,Day=86400000,Freshness=90000;
constexpr int MaximumNative=48000,MaximumPages=256,MaximumCalendarReads=256;
bool supported(qint64 interval){return interval==Minute||interval==300000||interval==900000||interval==3600000||interval==14400000||interval==Day;}
qint64 after(qint64 now,qint64 delay){return delay>std::numeric_limits<qint64>::max()-now?std::numeric_limits<qint64>::max():now+delay;}
bool bearerValid(QByteArrayView value){if(value.empty()||value.size()>8192)return false;for(const auto c:value)if(uchar(c)<0x21||uchar(c)>0x7e)return false;return true;}
bool timestamp(const QString& value,qint64* out){
    static const QRegularExpression offset("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]{1,3})?(?:Z|[+-][0-9]{2}:[0-9]{2})\\z");
    if(!offset.match(value).hasMatch())return false;const auto parsed=QDateTime::fromString(value,Qt::ISODate);if(!parsed.isValid()||parsed.toMSecsSinceEpoch()<=0)return false;*out=parsed.toMSecsSinceEpoch();return true;
}
QDate date(const QString& text){const auto value=QDate::fromString(text,Qt::ISODate);return value.isValid()&&value.toString(Qt::ISODate)==text?value:QDate{};}
bool decimalText(const QString& value,bool positive=true){static const QRegularExpression number("\\A(?:0|[1-9][0-9]{0,79})(?:\\.[0-9]{1,80})?\\z");bool ok=false;const auto parsed=value.toDouble(&ok);return number.match(value).hasMatch()&&ok&&std::isfinite(parsed)&&(positive?parsed>0:parsed>=0);}
bool integerText(const QString& value,bool signedValue=false){static const QRegularExpression natural("\\A(?:0|[1-9][0-9]{0,79})\\z"),signedInteger("\\A(?:0|-?[1-9][0-9]{0,79})\\z");return (signedValue?signedInteger:natural).match(value).hasMatch();}
QJsonValue payload(const HttpResult& response){
    if(response.status!=200||response.timedOut||response.networkError||response.localRetryAfterSeconds>0||response.body.size()>4*1024*1024||!response.body.isValidUtf8())return {};
    const auto document=parseExactJson(response.body);return document.isObject()?document.object().value("result"):QJsonValue{};
}
qint64 retryAt(const HttpResult& response,qint64 now){
    qint64 deadline=after(now,qMax<qint64>(1000,qint64(qMax(0,response.localRetryAfterSeconds))*1000));
    for(auto it=response.headers.cbegin();it!=response.headers.cend();++it){if(it.key().compare("Retry-After",Qt::CaseInsensitive)!=0)continue;
        bool ok=false;const auto seconds=it.value().trimmed().toLongLong(&ok);if(ok&&seconds>0){deadline=qMax(deadline,after(now,seconds>std::numeric_limits<qint64>::max()/1000?std::numeric_limits<qint64>::max():seconds*1000));continue;}
        const auto text=QString::fromLatin1(it.value().trimmed());static const QRegularExpression imf("\\A(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT\\z");
        if(imf.match(text).hasMatch()){const auto parsed=QLocale::c().toDateTime(text,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");if(parsed.isValid())deadline=qMax(deadline,QDateTime(parsed.date(),parsed.time(),QTimeZone("UTC")).toMSecsSinceEpoch());}
    }
    return deadline;
}
bool same(const StockObservedCandle& a,const StockObservedCandle& b){const auto& x=a.candle;const auto& y=b.candle;return a.sourceLabelAtMs==b.sourceLabelAtMs&&a.tradingDate==b.tradingDate&&x.startsAtMs==y.startsAtMs&&x.open==y.open&&x.high==y.high&&x.low==y.low&&x.close==y.close&&x.volume==y.volume;}
bool same(const StockSessionDay& a,const StockSessionDay& b){if(a.state!=b.state||a.sessions.size()!=b.sessions.size())return false;for(int i=0;i<a.sessions.size();++i){const auto& x=a.sessions[i];const auto& y=b.sessions[i];if(x.kind!=y.kind||x.fromMs!=y.fromMs||x.toMs!=y.toMs||x.auctionFromMs!=y.auctionFromMs||x.auctionToMs!=y.auctionToMs)return false;}return true;}
bool netVolume(const QJsonValue& value,std::optional<QString>* out){
    if(value.isNull()){out->reset();return true;}if(!value.isObject())return false;const auto object=value.toObject();const auto buy=object.value("buyVolume").toString(),sell=object.value("sellVolume").toString(),net=object.value("netBuyVolume").toString();
    if(!integerText(buy)||!integerText(sell)||!integerText(net,true))return false;const auto difference=decimal::compare(buy,sell)>=0?decimal::subtract(buy,sell):"-"+decimal::subtract(sell,buy);
    if(difference!=net)return false;*out=net;return true;
}
}
struct StockChartSource::Private {
    struct Job {StockChartRead read=StockChartRead::Metadata;QString cursor;bool volume=false;bool level=false;qint64 levelFrom=0;};
    struct LevelCache {QMap<qint64,StockObservedCandle> daily,minutes;StockCalendar calendar;qint64 dailyStarted=0,dailyReceived=0,minuteStarted=0,minuteReceived=0,until=0;quint64 revision=0;};
    StockChartSource* q=nullptr;QPointer<TradingTransport> transport;QPointer<BrokerTradingController> broker;
    QPointer<TossMarketStream> stream;QPointer<KiwoomMarketStream> kiwoom;bool kiwoomTarget=false;AuthorizationProvider authorization;Clock clock;
    QTimer* timer=nullptr;WatchlistIdentity selection;StockChartSnapshot result,lastGood;StockChartInstrument instrument;StockCalendar calendar;StockRestrictions restrictions;
    QMap<qint64,StockObservedCandle> native,daily;QMap<QDate,StockFlowDay> flows;QList<Job> jobs;Job running;
    QMap<qint64,StockObservedCandle> levelMinutes;QMap<QString,LevelCache> levelCache;QSet<QString> levelTargets,levelCursors;
    bool levelsDemand=false,levelCalendarsQueued=false;QString levelFailure;qint64 levelStarted=0,levelReceived=0;int levelPages=0;
    QSet<QString> cursors,dailyCursors,calendarRequests;int pages=0,dailyPages=0;bool enabled=false,extended=false,pumping=false,toss=false,cycle=false,dailyQueued=false;
    bool requiringBroker=false,authorizationFailed=false,preferKiwoomDaily=false;
    StockChartConnectionState connectionState=StockChartConnectionState::NoBroker;
    qint64 interval=900000,deadline=0,sentAt=0,nextSend=0,nextRefresh=0,candleStarted=0,candleReceived=0,dailyStarted=0,dailyReceived=0;
    quint64 token=0,nextToken=1,observedRevision=0,revision=0,sentRevision=0,blockedRevision=0,lastGoodRevision=0;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    bool selected()const{return WatchlistWorkspaceList::validIdentity(selection)&&selection.market=="stock"&&supported(interval);}
    qint64 nativeInterval()const{return interval==Day?Day:Minute;}
    int nativeLimit()const{return interval==Day?200:qMin<qint64>(MaximumNative,200*(interval/Minute));}
    void cancel(){const bool pending=token!=0;token=0;if(pending&&transport)transport->abort();}
    void erase(const QString& reason={},bool discardLastGood=true){
        requiringBroker=false;
        cancel();native.clear();daily.clear();flows.clear();jobs.clear();instrument={};calendar={};restrictions={};cursors.clear();dailyCursors.clear();calendarRequests.clear();pages=dailyPages=0;cycle=dailyQueued=false;candleStarted=candleReceived=dailyStarted=dailyReceived=0;
        levelMinutes.clear();levelTargets.clear();levelCursors.clear();levelCalendarsQueued=false;levelFailure.clear();levelStarted=levelReceived=0;levelPages=0;
        if(discardLastGood){lastGood={};lastGoodRevision=0;}
        result={};result.series.identity=selection.key();result.series.intervalMs=interval;result.series.reason=reason;
    }
    void start(bool preserve=false){
        authorizationFailed=false;
        const bool keep=preserve&&lastGoodRevision==revision&&lastGood.series.available;erase({},!keep);if(keep)result=lastGood;
        toss=true;cycle=true;jobs.append({StockChartRead::Metadata,{}});result.source=StockSourceKind::Toss;result.loading=true;result.series.reason=QObject::tr("토스 주식 차트 원천 조회 중");
    }
    void unavailable(const QString& reason,qint64 delay=30000){
        const bool keep=toss&&lastGoodRevision==revision&&lastGood.series.available;erase(reason,!keep);if(keep){result=lastGood;result.loading=false;result.series.reason=reason;}
        nextRefresh=after(now(),delay);nextSend=qMax(nextSend,nextRefresh);result.source=toss?StockSourceKind::Toss:StockSourceKind::None;emit q->changed();
    }
    void requireBroker(std::optional<StockChartConnectionState> requested={}){
        const auto issue=requested.value_or(connectionState==StockChartConnectionState::Ready?StockChartConnectionState::AuthenticationRequired:connectionState);
        const auto message=stockChartConnectionMessage(issue);
        const bool dailyAvailable=canOfferKiwoomDaily();
        if(requiringBroker&&result.series.reason==message&&result.kiwoomDailyAlternative==dailyAvailable)return;
        levelCache.clear();erase(message);result.connectionIssue=issue;result.kiwoomDailyAlternative=dailyAvailable;toss=false;revision=0;deadline=0;requiringBroker=true;emit q->changed();
    }
    void requireAuthentication(){authorizationFailed=true;requireBroker(StockChartConnectionState::AuthenticationRequired);}
    void quote(){
        if(!enabled||!toss||!stream||!instrument.verified)return;QString latest;AssetEvidence evidence;
        for(const auto& value:stream->quotes())if(value.symbol==selection.code&&value.marketCountry==selection.source&&value.currency==instrument.currency&&decimalText(value.price)&&
            value.evidence.complete&&value.evidence.receivedAtMs>0&&value.evidence.receivedAtMs<=now()&&now()-value.evidence.receivedAtMs<=Freshness&&
            !value.evidence.source.isEmpty()&&!value.evidence.url.isEmpty()) {latest=value.price;evidence=value.evidence;break;}
        const bool changed=result.livePrice!=latest||result.liveEvidence.receivedAtMs!=evidence.receivedAtMs;result.livePrice=latest;result.liveEvidence=evidence;if(changed)emit q->changed();
    }
    void needCalendar(const QDate& day,bool level=false){
        const auto key=day.toString(Qt::ISODate);if(!day.isValid()||calendar.days.contains(day)||calendarRequests.contains(key)||calendarRequests.size()>=MaximumCalendarReads)return;
        calendarRequests.insert(key);jobs.append({StockChartRead::Calendar,key,false,level});
    }
    std::optional<StockSession> regular(QDate day)const{
        const auto found=calendar.days.constFind(day);if(found==calendar.days.cend()||found->state!=StockCalendarState::Sessions)return {};
        std::optional<StockSession> result;for(const auto& session:found->sessions)if(session.kind==StockSessionKind::Regular){if(result)return {};result=session;}return result;
    }
    void queueLevelSupport(){
        if(!levelsDemand||!instrument.verified||native.isEmpty())return;
        const QTimeZone zone(instrument.marketTimeZone.toUtf8());const auto today=QDateTime::fromMSecsSinceEpoch(now(),zone).date();
        if(!levelCalendarsQueued){levelCalendarsQueued=true;const QDate month(today.year(),today.month(),1);
            for(auto day=today;day>=month.addMonths(-1);day=day.addDays(-1))needCalendar(day,true);}
        const auto addMinutes=[&](qint64 from,qint64 through){
            if(from<=0||through<=from||through>now())return;
            const auto cursor=QDateTime::fromMSecsSinceEpoch(through,QTimeZone("UTC")).toString(Qt::ISODate);
            if(levelTargets.contains(cursor))return;levelTargets.insert(cursor);
            bool covered=true;for(qint64 stamp=from;stamp<through;stamp+=Minute)if(!levelMinutes.contains(stamp)&&!(nativeInterval()==Minute&&native.contains(stamp))){covered=false;break;}
            if(!covered)jobs.append({StockChartRead::Candles,cursor,false,true,from});
        };
        if(const auto session=regular(today))addMinutes(session->fromMs,session->fromMs+Minute);
        if(selection.source!="KR")return;
        for(auto day=today.addDays(-1);day>=today.addDays(-32);day=day.addDays(-1)){
            const auto found=calendar.days.constFind(day);if(found==calendar.days.cend()||found->state==StockCalendarState::Unknown)break;
            if(found->state==StockCalendarState::ExplicitHoliday)continue;if(const auto session=regular(day))addMinutes(session->toMs-Minute,session->toMs);break;
        }
        const auto monday=today.addDays(1-today.dayOfWeek());
        for(auto day=monday;day<=today;day=day.addDays(1)){
            const auto found=calendar.days.constFind(day);if(found==calendar.days.cend()||found->state==StockCalendarState::Unknown)break;
            if(found->state==StockCalendarState::ExplicitHoliday)continue;if(const auto session=regular(day))addMinutes(session->fromMs,session->toMs);break;
        }
    }
    StockCandlePage levelPage(bool minute)const{
        StockCandlePage page;page.available=true;page.instrument=instrument;page.source=StockSourceKind::Toss;page.adjustment=StockAdjustment::SourceAdjusted;
        page.nativeIntervalMs=minute?Minute:Day;page.sourceUrl=StockChartSource::request(StockChartRead::Candles,selection,page.nativeIntervalMs).url.toString();
        const auto& rows=minute?levelMinutes:nativeInterval()==Day?native:daily;
        page.requestStartedAtMs=minute?levelStarted:nativeInterval()==Day?candleStarted:dailyStarted;
        page.receivedAtMs=minute?levelReceived:nativeInterval()==Day?candleReceived:dailyReceived;
        for(const auto& row:rows)page.candles.append(row);
        if(minute&&nativeInterval()==Minute){QMap<qint64,StockObservedCandle> combined=levelMinutes;const auto windows=stockLevelMinuteWindows(instrument,calendar,now());
            for(const auto& row:native)for(const auto& window:windows)if(row.candle.startsAtMs>=window.fromMs&&row.sourceLabelAtMs<=window.throughMs){combined.insert(row.candle.startsAtMs,row);break;}
            page.candles=combined.values();page.requestStartedAtMs=page.requestStartedAtMs?qMin(page.requestStartedAtMs,candleStarted):candleStarted;page.receivedAtMs=page.receivedAtMs?qMax(page.receivedAtMs,candleReceived):candleReceived;}
        return page;
    }
    void publishLevels(){
        result.levelCandidates.clear();result.levelsReason.clear();if(!levelsDemand)return;
        const auto levels=stockPeriodLevels(levelPage(false),levelPage(true),calendar,now());result.levelCandidates=levels.candidates;
        result.levelsReason=levelFailure.isEmpty()?levels.reason:levelFailure+(levels.reason.isEmpty()?QString{}:QStringLiteral(" · ")+levels.reason);
        if(!result.loading&&!levelPage(false).candles.isEmpty()&&calendar.available&&levelFailure.isEmpty()){
            const QTimeZone zone(instrument.marketTimeZone.toUtf8());const auto today=QDateTime::fromMSecsSinceEpoch(now(),zone).date();
            qint64 until=QDateTime(today.addDays(1),QTime(0,0),zone).toMSecsSinceEpoch();
            if(calendar.days.contains(today)){qint64 completion=0;for(const auto& s:calendar.days.value(today).sessions){completion=qMax(completion,s.toMs);if(s.fromMs>now())until=qMin(until,s.fromMs);if(s.toMs>now())until=qMin(until,s.toMs);}
                if(selection.source=="US")completion=qMax(completion,QDateTime(today,QTime(20,0),zone).toMSecsSinceEpoch());if(completion>now())until=qMin(until,completion);}
            const auto dailyPage=levelPage(false),minutePage=levelPage(true);QMap<qint64,StockObservedCandle> cachedMinutes;
            const auto monday=today.addDays(1-today.dayOfWeek());
            for(const auto& row:minutePage.candles)if(row.tradingDate>=monday&&row.sourceLabelAtMs<=minutePage.requestStartedAtMs){const auto session=regular(row.tradingDate);
                if(session&&row.candle.startsAtMs>=session->fromMs&&row.sourceLabelAtMs<=session->toMs)cachedMinutes.insert(row.candle.startsAtMs,row);}
            for(const auto& row:levelMinutes)cachedMinutes.insert(row.candle.startsAtMs,row);
            levelCache.insert(selection.key(),{nativeInterval()==Day?native:daily,cachedMinutes,calendar,dailyPage.requestStartedAtMs,dailyPage.receivedAtMs,minutePage.requestStartedAtMs,minutePage.receivedAtMs,until,revision});
            while(levelCache.size()>8)levelCache.erase(levelCache.begin());
        }
    }
    std::optional<int> streak(bool foreigner)const{
        if(flows.isEmpty())return {};auto day=flows.lastKey();int count=0;
        for(int n=0;n<=flows.size();++n){const auto found=flows.constFind(day);if(found==flows.cend())return {};const auto net=foreigner?found->foreignerNetBuyVolume:found->institutionNetBuyVolume;
            if(!net)return {};if(!decimal::positive(*net))return count;++count;QDate previous;
            for(auto it=calendar.nextBusinessDay.cbegin();it!=calendar.nextBusinessDay.cend();++it)if(it.value()==day){if(previous.isValid())return {};previous=it.key();}
            if(!previous.isValid())return {};day=previous;
        }return {};
    }
    void volumeStudy(){
        auto& studies=result.marketStudies;studies.volumeMultiple.reset();studies.volumeSamples=0;studies.volumeEvidence={};
        studies.volumeScope=QObject::tr("원천 일봉 거래량 · 이전 20완료 거래일 중앙값 대비 배수 · 주");studies.reason=QObject::tr("20완료일 일봉 거래량 중앙값 자료 부족");
        const auto& rows=nativeInterval()==Day?native:daily;const auto started=nativeInterval()==Day?candleStarted:dailyStarted,received=nativeInterval()==Day?candleReceived:dailyReceived;
        if(rows.isEmpty()||!instrument.verified||!started||!received)return;
        const auto url=StockChartSource::request(StockChartRead::Candles,selection,Day).url.toString();
        StockCandlePage page;page.available=true;page.instrument=instrument;page.source=StockSourceKind::Toss;page.adjustment=StockAdjustment::SourceAdjusted;page.nativeIntervalMs=Day;
        page.requestStartedAtMs=started;page.receivedAtMs=received;page.sourceUrl=url;for(const auto& row:rows)page.candles.append(row);
        StockChartOptions options;options.intervalMs=Day;const auto closed=buildStockChart(page,calendar,{},options);
        studies.volumeEvidence={QObject::tr("토스 원천 일봉 거래량 · 주"),url,"public:toss:daily-volume:"+selection.key(),0,received,!closed.series.candles.isEmpty()};
        if(closed.series.candles.isEmpty()||closed.timeline.rows.size()!=closed.series.candles.size())return;
        QStringList volumes;auto next=closed.timeline.rows.last().tradingDate;
        for(int i=closed.series.candles.size()-2;i>=0&&volumes.size()<20;--i){const auto day=closed.timeline.rows.at(i).tradingDate;
            if(calendar.nextBusinessDay.value(day)!=next)break;volumes.append(closed.series.candles.at(i).volume);next=day;
        }
        studies.volumeSamples=volumes.size();if(volumes.size()!=20)return;
        std::sort(volumes.begin(),volumes.end(),[](const QString& a,const QString& b){return decimal::compare(a,b)<0;});
        // The provider quantities are exact strings. Only this final display ratio uses floating point.
        if(!decimal::positive(volumes.at(9))&&!decimal::positive(volumes.at(10))){studies.reason=QObject::tr("이전 20완료일 거래량 중앙값 0 · 배수 미표시");return;}
        bool okA=false,okB=false,okCurrent=false;const auto a=volumes.at(9).toDouble(&okA),b=volumes.at(10).toDouble(&okB),current=closed.series.candles.last().volume.toDouble(&okCurrent);
        const auto multiple=double((2.L*static_cast<long double>(current))/(static_cast<long double>(a)+static_cast<long double>(b)));
        if(okA&&okB&&okCurrent&&std::isfinite(multiple)&&multiple>=0){studies.volumeMultiple=multiple;studies.reason.clear();}
    }
    void publish(){
        const auto oldPrice=result.livePrice;const auto oldEvidence=result.liveEvidence;
        if(instrument.verified&&!native.isEmpty()){
            StockCandlePage page;page.available=true;page.instrument=instrument;page.source=StockSourceKind::Toss;page.adjustment=StockAdjustment::SourceAdjusted;
            page.nativeIntervalMs=nativeInterval();page.requestStartedAtMs=candleStarted;page.receivedAtMs=candleReceived;page.sourceUrl=StockChartSource::request(StockChartRead::Candles,selection,nativeInterval()).url.toString();
            for(const auto& row:native)page.candles.append(row);StockChartOptions options;options.intervalMs=interval;options.includeExtended=extended;options.realtimeAllowed=true;
            result=buildStockChart(page,calendar,restrictions,options);
        }
        result.loading=token||!jobs.isEmpty();result.source=StockSourceKind::Toss;result.livePrice=oldPrice;result.liveEvidence=oldEvidence;
        result.marketStudies.days=flows.values();result.marketStudies.foreignerConsecutiveBuyDays=streak(true);result.marketStudies.institutionConsecutiveBuyDays=streak(false);
        if(!result.marketStudies.foreignerConsecutiveBuyDays||!result.marketStudies.institutionConsecutiveBuyDays)result.marketStudies.flowContinuityReason=QObject::tr("연속 순매수 거래일을 증명할 공식 캘린더 링크·이전 기록 부족");
        volumeStudy();publishLevels();quote();if(result.series.available&&!result.loading){lastGood=result;lastGoodRevision=revision;}emit q->changed();
    }
    bool metadata(const HttpResult& response){
        const auto raw=payload(response);if(!raw.isArray()||raw.toArray().size()!=1||!raw.toArray().first().isObject())return false;const auto stock=raw.toArray().first().toObject();
        const auto market=stock.value("market").toString(),currency=stock.value("currency").toString();const bool kr=QStringList{"KOSPI","KOSDAQ","KR_ETC"}.contains(market),us=QStringList{"NYSE","NASDAQ","AMEX","US_ETC"}.contains(market);
        if(stock.value("symbol").toString()!=selection.code||stock.value("status").toString()!="ACTIVE"||(!kr&&!us)||(kr?"KR":"US")!=selection.source||currency!=(kr?"KRW":"USD"))return false;
        instrument.identity=selection.key();instrument.symbol=selection.code;instrument.marketCountry=selection.source;instrument.currency=currency;instrument.market=market;
        instrument.marketTimeZone=kr?"Asia/Seoul":"America/New_York";instrument.verified=true;const auto detail=stock.value("koreanMarketDetail").toObject();if(detail.value("nxtSupported").isBool())instrument.nxtSupported=detail.value("nxtSupported").toBool();
        if(levelsDemand){const auto cached=levelCache.constFind(selection.key());if(cached!=levelCache.cend()&&cached->revision==revision&&cached->until>now()){
            daily=cached->daily;calendar=cached->calendar;dailyStarted=cached->dailyStarted;dailyReceived=cached->dailyReceived;dailyQueued=true;
            levelMinutes=cached->minutes;levelStarted=cached->minuteStarted;levelReceived=cached->minuteReceived;
        }}
        jobs.append({StockChartRead::Calendar,{}});if(kr)jobs.append({StockChartRead::PriceLimits,{}});if(kr)jobs.append({StockChartRead::Warnings,{}});if(kr)jobs.append({StockChartRead::Investors,{}});if(kr)jobs.append({StockChartRead::Program,{}});jobs.append({StockChartRead::Candles,{}});return true;
    }
    bool calendarPage(const HttpResult& response,const QString& requested){
        const auto parsed=parseStockCalendar(selection.source,response,now());if(!parsed.available||(!requested.isEmpty()&&!parsed.days.contains(date(requested))))return false;
        for(auto it=parsed.days.cbegin();it!=parsed.days.cend();++it)if(calendar.days.contains(it.key())&&!same(calendar.days.value(it.key()),it.value()))return false;
        for(auto it=parsed.nextBusinessDay.cbegin();it!=parsed.nextBusinessDay.cend();++it)if(calendar.nextBusinessDay.contains(it.key())&&calendar.nextBusinessDay.value(it.key())!=it.value())return false;
        for(auto it=parsed.days.cbegin();it!=parsed.days.cend();++it){calendar.days.insert(it.key(),it.value());calendarRequests.insert(it.key().toString(Qt::ISODate));}
        for(auto it=parsed.nextBusinessDay.cbegin();it!=parsed.nextBusinessDay.cend();++it)calendar.nextBusinessDay.insert(it.key(),it.value());
        calendar.available=true;calendar.marketCountry=selection.source;calendar.evidence=parsed.evidence;calendar.reason=parsed.reason;queueLevelSupport();return true;
    }
    void limits(const HttpResult& response){
        const auto raw=payload(response);if(!raw.isObject())return;const auto object=raw.toObject();qint64 observed=0;
        if(object.value("currency").toString()!=instrument.currency||!timestamp(object.value("timestamp").toString(),&observed)||observed>now())return;
        const auto upper=object.value("upperLimitPrice"),lower=object.value("lowerLimitPrice");if((!upper.isNull()&&(!upper.isString()||!decimalText(upper.toString())))||(!lower.isNull()&&(!lower.isString()||!decimalText(lower.toString()))))return;
        if(!upper.isNull())restrictions.upperLimit=upper.toString();if(!lower.isNull())restrictions.lowerLimit=lower.toString();
        restrictions.currentObservationAtMs=now();restrictions.historicalCoverage=StockHistoryCoverage::CurrentOnly;restrictions.evidence={"토스 현재 상하한가 · 과거 이력 미제공","https://openapi.tossinvest.com/api/v1/price-limits","public:toss:limits:"+selection.key(),observed,now(),true};
    }
    void warnings(const HttpResult& response){
        const auto raw=payload(response);if(!raw.isArray()||raw.toArray().size()>1000)return;bool vi=false;
        for(const auto& value:raw.toArray()){if(!value.isObject())return;const auto object=value.toObject();const auto type=object.value("warningType").toString();if(type.isEmpty())return;if(QStringList{"VI_STATIC","VI_DYNAMIC","VI_STATIC_AND_DYNAMIC"}.contains(type))vi=true;}
        restrictions.currentVI=vi;restrictions.currentObservationAtMs=now();restrictions.historicalCoverage=StockHistoryCoverage::CurrentOnly;
        if(!restrictions.evidence.complete)restrictions.evidence={"토스 현재 VI 관측 · 과거 이력 미제공","https://openapi.tossinvest.com/api/v1/stocks/"+selection.code+"/warnings","public:toss:warnings:"+selection.key(),0,now(),true};
    }
    void flow(const HttpResult& response,bool investor){
        const auto raw=payload(response);if(!raw.isObject()||!raw.toObject().value("records").isArray())return;const auto records=raw.toObject().value("records").toArray();if(records.size()>100)return;
        QMap<QDate,StockFlowDay> parsed;QDate previous;const auto currentDate=QDateTime::fromMSecsSinceEpoch(now(),QTimeZone("Asia/Seoul")).date();
        for(const auto& value:records){if(!value.isObject())return;const auto object=value.toObject();const auto day=date(object.value("date").toString());if(!day.isValid()||day>currentDate||(!previous.isNull()&&day>=previous))return;previous=day;
            StockFlowDay row;row.date=day;row.provisional=day==currentDate;
            if(investor){qint64 updated=0;if(!timestamp(object.value("updatedAt").toString(),&updated)||updated>now()||!netVolume(object.value("foreigner"),&row.foreignerNetBuyVolume)||!netVolume(object.value("institution"),&row.institutionNetBuyVolume))return;
                row.investorEvidence={"토스 투자자별 매매동향 · KRX+NXT · 주","https://openapi.tossinvest.com/api/v1/stocks/"+selection.code+"/investor-trading","public:toss:investors:"+selection.key(),updated,now(),true};
            }else{if(!netVolume(object.value("arbitrage"),&row.arbitrageNetBuyVolume)||!netVolume(object.value("nonArbitrage"),&row.nonArbitrageNetBuyVolume))return;
                row.programEvidence={"토스 프로그램매매 · KRX · 주","https://openapi.tossinvest.com/api/v1/stocks/"+selection.code+"/program-trades","public:toss:program:"+selection.key(),0,now(),true};}
            parsed.insert(day,row);
        }
        for(auto it=parsed.cbegin();it!=parsed.cend();++it){auto& existing=flows[it.key()];existing.date=it.key();existing.provisional=it->provisional;
            if(investor){existing.foreignerNetBuyVolume=it->foreignerNetBuyVolume;existing.institutionNetBuyVolume=it->institutionNetBuyVolume;existing.investorEvidence=it->investorEvidence;}
            else{existing.arbitrageNetBuyVolume=it->arbitrageNetBuyVolume;existing.nonArbitrageNetBuyVolume=it->nonArbitrageNetBuyVolume;existing.programEvidence=it->programEvidence;}needCalendar(it.key());}
    }
    bool candlePage(const HttpResult& response){
        if(running.level){
            if(++levelPages>16)return false;const auto parsed=parseTossStockCandles(instrument,Minute,StockAdjustment::SourceAdjusted,response,sentAt,now());if(!parsed.available)return false;
            qint64 bound=0;if(!timestamp(running.cursor,&bound)||(!parsed.candles.isEmpty()&&parsed.candles.last().sourceLabelAtMs>bound))return false;
            for(const auto& row:parsed.candles)if(levelMinutes.contains(row.candle.startsAtMs)&&!same(levelMinutes.value(row.candle.startsAtMs),row))return false;
            for(const auto& row:parsed.candles)levelMinutes.insert(row.candle.startsAtMs,row);
            levelStarted=levelStarted?qMin(levelStarted,parsed.requestStartedAtMs):parsed.requestStartedAtMs;levelReceived=qMax(levelReceived,parsed.receivedAtMs);
            if(!parsed.nextBefore.isEmpty()&&!parsed.candles.isEmpty()&&parsed.candles.first().candle.startsAtMs>running.levelFrom){
                if(levelCursors.contains(parsed.nextBefore)||parsed.nextBefore==running.cursor)return false;levelCursors.insert(parsed.nextBefore);
                jobs.append({StockChartRead::Candles,parsed.nextBefore,false,true,running.levelFrom});
            }
            return levelMinutes.size()<=3200;
        }
        auto& destination=running.volume?daily:native;auto& seen=running.volume?dailyCursors:cursors;auto& count=running.volume?dailyPages:pages;
        auto& started=running.volume?dailyStarted:candleStarted;auto& received=running.volume?dailyReceived:candleReceived;
        const auto pageInterval=running.volume?Day:nativeInterval();const int limit=running.volume?(levelsDemand?65:22):nativeLimit();
        if(++count>MaximumPages)return false;const auto parsed=parseTossStockCandles(instrument,pageInterval,StockAdjustment::SourceAdjusted,response,sentAt,now());if(!parsed.available)return false;
        if(!running.cursor.isEmpty()){qint64 bound=0;if(!timestamp(running.cursor,&bound)||(!parsed.candles.isEmpty()&&parsed.candles.last().sourceLabelAtMs>bound))return false;}
        const int before=destination.size();for(const auto& row:parsed.candles){const auto key=row.candle.startsAtMs;if(destination.contains(key)&&!same(destination.value(key),row))return false;destination.insert(key,row);if(destination.size()>MaximumNative+200)return false;}
        started=started?qMin(started,parsed.requestStartedAtMs):parsed.requestStartedAtMs;received=received?qMin(received,parsed.receivedAtMs):parsed.receivedAtMs;
        if(!parsed.nextBefore.isEmpty()&&destination.size()<limit){
            if(seen.contains(parsed.nextBefore)||parsed.nextBefore==running.cursor||destination.size()==before||count>=MaximumPages)return false;seen.insert(parsed.nextBefore);jobs.prepend({StockChartRead::Candles,parsed.nextBefore,running.volume});
        }
        while(destination.size()>limit)destination.erase(destination.begin());for(auto row=destination.cend();row!=destination.cbegin();){--row;needCalendar(row->tradingDate,running.volume&&levelsDemand);}
        if(!running.volume&&nativeInterval()!=Day&&!dailyQueued){dailyQueued=true;jobs.append({StockChartRead::Candles,{},true});}queueLevelSupport();return true;
    }
    bool response(const HttpResult& response){
        switch(running.read){case StockChartRead::Metadata:return metadata(response);case StockChartRead::Calendar:return calendarPage(response,running.cursor);
        case StockChartRead::Candles:return candlePage(response);case StockChartRead::PriceLimits:limits(response);break;case StockChartRead::Warnings:warnings(response);break;
        case StockChartRead::Investors:flow(response,true);break;case StockChartRead::Program:flow(response,false);break;}return true;
    }
    bool hasKiwoom()const{
        return kiwoom&&kiwoom->broker()&&kiwoom->broker()->policy().provider=="kiwoom";
    }
    bool canOfferKiwoomDaily()const{
        return selection.source=="KR"&&interval!=Day&&connectionState!=StockChartConnectionState::VaultClosed&&hasKiwoom()&&kiwoom->authorizationUsable();
    }
    bool kiwoomMode()const{
        return selection.source=="KR"&&interval==Day&&hasKiwoom()&&
            (connectionState==StockChartConnectionState::OtherBroker||
             ((connectionState==StockChartConnectionState::AuthenticationRequired||
               (connectionState==StockChartConnectionState::Ready&&preferKiwoomDaily))&&kiwoom->authorizationUsable()));
    }
    void releaseKiwoom(){if(kiwoomTarget&&kiwoom)kiwoom->setDailyTarget({});kiwoomTarget=false;}
    void kiwoomDaily(){
        kiwoom->setDailyTarget(selection);kiwoomTarget=true;const auto series=kiwoom->daily(selection);
        StockChartSnapshot next;next.series.identity=selection.key();next.series.intervalMs=interval;next.source=StockSourceKind::KiwoomDaily;
        StockChartInstrument instrument;
        for(const auto& i:kiwoom->instruments())if(i.identity==selection&&i.verified&&i.currency=="KRW"){
            instrument.identity=selection.key();instrument.symbol=selection.code;instrument.marketCountry="KR";instrument.currency="KRW";
            instrument.market="KR_ETC";instrument.marketTimeZone="Asia/Seoul";instrument.verified=true;break;
        }
        if(!kiwoom->authorizationUsable()){
            next.connectionIssue=StockChartConnectionState::KiwoomAuthenticationRequired;next.loading=false;next.series.reason=QObject::tr("키움 인증이 필요하거나 만료되었습니다. 기존 연결에서 직접 다시 인증하세요.");
        }else if(!series.available||!instrument.verified){
            if(!series.available&&!series.reason.isEmpty()){
                next.loading=series.loading;next.series.reason=series.reason;
            }else{
                next.loading=series.loading||!instrument.verified;
                next.series.reason=!instrument.verified?QObject::tr("키움 종목·통화 확인 대기"):QObject::tr("키움 일봉 조회 중");
            }
        }else{
            const QTimeZone kst("Asia/Seoul");StockCalendar derived;derived.available=true;derived.marketCountry="KR";
            StockCandlePage page;page.available=true;page.instrument=instrument;page.source=StockSourceKind::KiwoomDaily;page.adjustment=StockAdjustment::SourceAdjusted;
            page.nativeIntervalMs=Day;page.requestStartedAtMs=series.requestStartedAtMs;page.receivedAtMs=series.receivedAtMs;page.sourceUrl=QStringLiteral("https://api.kiwoom.com/api/dostk/chart");
            for(const auto& row:series.rows.mid(qMax<qsizetype>(0,series.rows.size()-200))){
                // Trading days come from Kiwoom's own rows (no official calendar links are invented).
                // A day's bar completes after KRX regular hours and the NXT after-market, 20:00 KST.
                StockSessionDay day;day.tradingDate=row.date;day.state=StockCalendarState::Sessions;
                day.sessions={{StockSessionKind::Regular,QDateTime(row.date,QTime(9,0),kst).toMSecsSinceEpoch(),QDateTime(row.date,QTime(15,30),kst).toMSecsSinceEpoch(),{},{}},
                              {StockSessionKind::After,QDateTime(row.date,QTime(15,30),kst).toMSecsSinceEpoch(),QDateTime(row.date,QTime(20,0),kst).toMSecsSinceEpoch(),{},{}}};
                derived.days.insert(row.date,day);
                StockObservedCandle observed;observed.tradingDate=row.date;observed.candle.startsAtMs=QDateTime(row.date,QTime(0,0),kst).toMSecsSinceEpoch();
                observed.candle.open=row.open;observed.candle.high=row.high;observed.candle.low=row.low;observed.candle.close=row.close;observed.candle.volume=row.volume;page.candles.append(observed);
            }
            StockChartOptions options;options.intervalMs=Day;
            next=buildStockChart(page,derived,{},options);next.source=StockSourceKind::KiwoomDaily;next.loading=false;next.calendarAvailable=false;
            next.sessionNote=QObject::tr("키움 KRX 일봉 · 거래일은 키움 일봉 행 기준 · 당일 봉은 20:00 KST 이후 완료");
            if(levelsDemand)next.levelsReason=QObject::tr("키움 레벨선 · 공식 거래일 캘린더 확인이 필요해요");
        }
        const bool same=result.source==next.source&&result.loading==next.loading&&result.series.identity==next.series.identity&&result.series.reason==next.series.reason&&
            result.series.receivedAtMs==next.series.receivedAtMs&&result.series.candles.size()==next.series.candles.size()&&result.pending.size()==next.pending.size();
        if(!same){result=next;emit q->changed();}
    }
    void pump(){
        if(!enabled||pumping||!selected())return;const QScopedValueRollback guard(pumping,true);const auto time=now();
        if(kiwoomMode()){
            if(toss){erase();toss=false;revision=0;deadline=0;}
            kiwoomDaily();return;
        }
        if(kiwoomTarget){releaseKiwoom();if(result.source==StockSourceKind::KiwoomDaily){erase();emit q->changed();}}
        if(token){if(time>=sentAt&&time-sentAt<10000)return;cancel();
            if(levelsDemand&&(running.level||running.volume)){levelFailure=QObject::tr("일부 레벨 자료를 받지 못했어요");if(running.volume){daily.clear();dailyStarted=dailyReceived=0;}
                if(running.level&&running.read==StockChartRead::Candles){levelMinutes.clear();levelStarted=levelReceived=0;}if(jobs.isEmpty())nextRefresh=after(time,60000);publish();return;}
            unavailable(QObject::tr("주식 차트 원천 응답 시간 초과"),5000);return;}
        bool skippedCalendar=false;
        while(!jobs.isEmpty()&&jobs.first().read==StockChartRead::Calendar&&!jobs.first().cursor.isEmpty()){
            const auto day=date(jobs.first().cursor);const auto proof=calendar.days.constFind(day);
            if(!calendar.available||calendar.marketCountry!=selection.source||proof==calendar.days.cend()||proof->tradingDate!=day||
               (proof->state!=StockCalendarState::Sessions&&proof->state!=StockCalendarState::ExplicitHoliday))break;
            jobs.removeFirst();skippedCalendar=true;
        }
        if(skippedCalendar&&jobs.isEmpty()){nextRefresh=after(time,60000);publish();return;}
        if(time<nextSend)return;const bool refreshDue=cycle&&jobs.isEmpty()&&time>=nextRefresh;if(cycle&&jobs.isEmpty()&&!refreshDue)return;
        bool valid=false,blocked=false;
        if(authorization&&transport)authorization([&](QByteArrayView bearer,qint64 remaining,quint64 version){
            if(!bearerValid(bearer)||remaining<=0||!version)return;valid=true;observedRevision=version;
            if(version==blockedRevision){blocked=true;return;}if(blockedRevision&&version!=blockedRevision)blockedRevision=0;
            const bool sameRevision=toss&&revision==version;if(sameRevision&&deadline&&time>=deadline){blockedRevision=version;blocked=true;return;}
            const auto expiry=after(time,qMin<qint64>(remaining,24*3600000LL));deadline=sameRevision&&deadline?qMin(deadline,expiry):expiry;
            if(!toss||!cycle||revision!=version||refreshDue){start(sameRevision);revision=version;}
            if(jobs.isEmpty())return;
            running=jobs.takeFirst();auto request=StockChartSource::request(running.read,selection,running.volume?Day:running.level?Minute:nativeInterval(),running.cursor);if(!StockChartSource::isRequest(request)){unavailable(QObject::tr("주식 차트 공개 원천 GET 검증 실패"));return;}
            QByteArray auth("Bearer ");auth.append(bearer.data(),bearer.size());request.headers.insert("Authorization",auth);sentAt=time;sentRevision=version;nextSend=after(time,levelsDemand?1000:100);token=nextToken++;result.loading=true;
            transport->send(token,request);
        });
        if(blocked||(!valid&&connectionState==StockChartConnectionState::Ready))requireAuthentication();
        else if(!valid)requireBroker();
    }
    void finished(quint64 receivedToken,const HttpResult& reply){
        if(!enabled||!token||receivedToken!=token)return;token=0;quint64 current=0;qint64 remaining=0;
        if(authorization)authorization([&](QByteArrayView bearer,qint64 available,quint64 version){if(bearerValid(bearer)&&available>0&&version){current=version;remaining=available;observedRevision=version;}});
        if(!current||current==blockedRevision){requireAuthentication();return;}
        if(current!=sentRevision){start();revision=current;deadline=after(now(),qMin<qint64>(remaining,24*3600000LL));emit q->changed();return;}
        if(deadline&&now()>=deadline){blockedRevision=current;requireAuthentication();return;}
        const auto expiry=after(now(),qMin<qint64>(remaining,24*3600000LL));deadline=deadline?qMin(deadline,expiry):expiry;
        if(reply.status==401||reply.status==403){blockedRevision=current;requireAuthentication();return;}
        const bool levelJob=levelsDemand&&(running.level||running.volume);
        if(reply.status==429||reply.localRetryAfterSeconds>0){const auto pause=retryAt(reply,now());
            if(levelJob){jobs.prepend(running);nextSend=qMax(nextSend,pause);levelFailure=QObject::tr("레벨 자료 요청 제한 · 잠시 후 다시 확인해요");publish();return;}
            erase(QObject::tr("토스 차트 요청 제한 · Retry-After 대기"));toss=true;nextSend=qMax(nextSend,pause);nextRefresh=nextSend;emit q->changed();return;}
        const bool validReply=reply.status==200&&!reply.timedOut&&!reply.networkError;
        if(!validReply||!response(reply)){
            if(levelJob){levelFailure=QObject::tr("일부 레벨 자료를 받지 못했어요");if(running.volume){daily.clear();dailyStarted=dailyReceived=0;}
                if(running.read==StockChartRead::Candles&&running.level){levelMinutes.clear();levelStarted=levelReceived=0;}
                if(jobs.isEmpty())nextRefresh=after(now(),60000);publish();return;}
            unavailable(validReply?QObject::tr("토스 주식 차트 identity·캘린더·페이지 원천 검증 실패"):QObject::tr("토스 주식 차트 원천 응답 실패"));return;}
        if(levelFailure.startsWith(QObject::tr("레벨 자료 요청 제한")))levelFailure.clear();
        if(jobs.isEmpty())nextRefresh=after(now(),60000);publish();
    }
};
StockChartSource::StockChartSource(TradingTransport* transport,QObject* parent,Clock clock):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->clock=std::move(clock);d_->timer=new QTimer(this);d_->timer->setInterval(100);connect(d_->timer,&QTimer::timeout,this,[this]{d_->pump();});
    if(transport){connect(transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& response){d_->finished(token,response);});connect(transport,&QObject::destroyed,this,[this]{d_->transport=nullptr;d_->token=0;d_->unavailable(tr("주식 차트 원천에 요청을 보낼 수 없어요"));});}
}
StockChartSource::~StockChartSource(){d_->timer->stop();if(d_->transport)disconnect(d_->transport,nullptr,this,nullptr);if(d_->broker)disconnect(d_->broker,nullptr,this,nullptr);if(d_->stream)disconnect(d_->stream,nullptr,this,nullptr);d_->enabled=false;d_->cancel();}
quint64 StockChartSource::observedAuthorizationRevision()const{return d_->observedRevision;}
bool StockChartSource::authenticationRequired(quint64 newestObservedRevision)const{
    // Both lanes borrow the same owner. Toss revisions advance within that owner;
    // a stopped lane must not mask a newer revision observed by the active lane.
    if(newestObservedRevision>d_->observedRevision)return false;
    return d_->authorizationFailed||(d_->deadline&&d_->now()>=d_->deadline);
}
void StockChartSource::setConnectionState(StockChartConnectionState state){
    if(d_->connectionState==state)return;d_->connectionState=state;
    if(d_->enabled&&d_->kiwoomMode()){d_->pump();return;}
    // This only replaces connection guidance. Keep source errors, retry timing,
    // successful data and blocked authorization revisions intact.
    if(d_->enabled){if(d_->requiringBroker||(!d_->toss&&!d_->result.loading))d_->requireBroker();else emit changed();}
}
void StockChartSource::setBroker(BrokerTradingController* broker){
    if(d_->broker==broker)return;if(d_->broker)disconnect(d_->broker,nullptr,this,nullptr);d_->broker=broker;d_->blockedRevision=0;QPointer<BrokerTradingController> safe=broker;
    setAuthorizationProvider([safe](const AuthorizationBorrow& callback){return safe&&safe->account().provider=="toss"&&safe->withMarketAuthorization(callback);});
    if(broker){connect(broker,&BrokerTradingController::intentRevoked,this,[this]{d_->blockedRevision=d_->revision;d_->authorizationFailed=true;d_->levelCache.clear();d_->erase(stockChartConnectionMessage(StockChartConnectionState::AuthenticationRequired));d_->result.connectionIssue=StockChartConnectionState::AuthenticationRequired;d_->toss=false;d_->revision=0;d_->deadline=0;emit changed();});connect(broker,&QObject::destroyed,this,[this]{d_->broker=nullptr;setAuthorizationProvider({});});}
}
void StockChartSource::setAuthorizationProvider(AuthorizationProvider provider){
    d_->observedRevision=0;d_->authorizationFailed=false;d_->levelCache.clear();
    const auto issue=d_->connectionState==StockChartConnectionState::Ready?StockChartConnectionState::AuthenticationRequired:d_->connectionState;
    d_->erase(provider?QString{}:stockChartConnectionMessage(issue));
    if(!provider)d_->result.connectionIssue=issue;
    d_->revision=0;d_->deadline=0;d_->toss=false;d_->authorization=std::move(provider);d_->pump();emit changed();
}
void StockChartSource::setKiwoomDailyStream(KiwoomMarketStream* stream){
    if(d_->kiwoom==stream)return;d_->releaseKiwoom();if(d_->kiwoom)disconnect(d_->kiwoom,nullptr,this,nullptr);d_->kiwoom=stream;
    if(stream)connect(stream,&KiwoomMarketStream::changed,this,[this]{if(d_->enabled&&d_->kiwoomMode())d_->pump();});
    if(d_->result.source==StockSourceKind::KiwoomDaily){d_->erase();emit changed();}d_->pump();
}
void StockChartSource::setPreferKiwoomDaily(bool preferred){
    if(d_->preferKiwoomDaily==preferred)return;
    d_->preferKiwoomDaily=preferred;d_->pump();
}
void StockChartSource::setRealtimeStream(TossMarketStream* stream){if(d_->stream==stream)return;if(d_->stream)disconnect(d_->stream,nullptr,this,nullptr);d_->stream=stream;if(stream)connect(stream,&TossMarketStream::changed,this,[this]{d_->quote();});}
void StockChartSource::setSelection(const WatchlistIdentity& identity,qint64 interval,bool extended){
    if(d_->selection==identity&&d_->interval==interval&&d_->extended==extended)return;d_->erase();d_->selection=identity;d_->interval=interval;d_->extended=extended;d_->revision=0;d_->deadline=0;d_->toss=false;
    d_->result.series.identity=identity.key();d_->result.series.intervalMs=interval;d_->pump();emit changed();
}
void StockChartSource::setLevelDemand(bool enabled){
    if(d_->levelsDemand==enabled)return;d_->levelsDemand=enabled;
    if(!enabled){if(d_->token&&d_->running.level)d_->cancel();d_->jobs.removeIf([](const auto& job){return job.level;});d_->result.levelCandidates.clear();d_->result.levelsReason.clear();d_->lastGood.levelCandidates.clear();d_->lastGood.levelsReason.clear();d_->result.loading=d_->token||!d_->jobs.isEmpty();
        if(!d_->result.loading&&d_->result.series.available)d_->nextRefresh=after(d_->now(),60000);}
    else if(d_->enabled&&d_->toss&&d_->instrument.verified&&!d_->native.isEmpty()){
        if(d_->nativeInterval()!=Day&&d_->daily.size()<65){d_->dailyQueued=true;d_->dailyPages=0;d_->dailyCursors.clear();d_->jobs.append({StockChartRead::Candles,{},true});}
        d_->queueLevelSupport();d_->publish();
    }
    if(d_->enabled&&d_->kiwoomMode())d_->kiwoomDaily();d_->pump();emit changed();
}
void StockChartSource::setActive(bool enabled){if(d_->enabled==enabled)return;d_->enabled=enabled;if(enabled){d_->timer->start();d_->pump();}else{d_->timer->stop();d_->releaseKiwoom();d_->levelCache.clear();d_->erase();d_->revision=0;d_->deadline=0;d_->toss=false;}emit changed();}
bool StockChartSource::active()const{return d_->enabled;}
void StockChartSource::refresh(){d_->pump();}
void StockChartSource::stop(){setActive(false);}
StockChartSnapshot StockChartSource::snapshot(qint64 asOf)const{
    if(!d_->enabled)return {};auto result=d_->result;const auto time=asOf?asOf:d_->now();
    const bool kiwoom=result.source==StockSourceKind::KiwoomDaily;
    if(kiwoom&&(!d_->hasKiwoom()||!d_->kiwoom->authorizationUsable())&&result.series.available){
        result={};result.source=StockSourceKind::KiwoomDaily;
        result.connectionIssue=StockChartConnectionState::KiwoomAuthenticationRequired;
    }else if(!kiwoom&&(d_->connectionState==StockChartConnectionState::AuthenticationRequired||(d_->deadline&&time>=d_->deadline))){
        result={};result.connectionIssue=StockChartConnectionState::AuthenticationRequired;
    }
    if(result.connectionIssue&&result.series.identity.isEmpty()){
        result.series.identity=d_->selection.key();result.series.intervalMs=d_->interval;
        result.series.reason=stockChartConnectionMessage(*result.connectionIssue);
    }
    if(result.connectionIssue)result.kiwoomDailyAlternative=d_->canOfferKiwoomDaily();
    const qint64 freshness=result.source==StockSourceKind::KiwoomDaily?300000:Freshness; // Kiwoom daily rereads each minute.
    if(result.series.receivedAtMs&&(result.series.receivedAtMs>time||time-result.series.receivedAtMs>freshness)){result.series.available=false;result.series.reason=result.source==StockSourceKind::KiwoomDaily?tr("키움 일봉 원래 수신 자료 만료"):tr("토스 주식 차트 원래 수신 자료 만료");result.series.candles.clear();result.series.forming.reset();result.timeline={};result.studies={};result.marketContextStudies={};result.compression={};result.marketStudies={};result.levelCandidates.clear();result.levelsReason=tr("차트 자료가 오래되어 레벨선을 숨겼어요");}
    if(result.liveEvidence.receivedAtMs&&(result.liveEvidence.receivedAtMs>time||time-result.liveEvidence.receivedAtMs>Freshness)){result.livePrice.clear();result.liveEvidence={};}return result;
}
RequestSpec StockChartSource::request(StockChartRead read,const WatchlistIdentity& identity,qint64 interval,const QString& cursor){
    RequestSpec r;if(!WatchlistWorkspaceList::validIdentity(identity)||identity.market!="stock")return r;const bool domestic=identity.source=="KR";QString path;QUrlQuery query;
    switch(read){
    case StockChartRead::Metadata:path="/api/v1/stocks";query.addQueryItem("symbols",identity.code);break;
    case StockChartRead::Calendar:path="/api/v1/market-calendar/"+identity.source;if(!cursor.isEmpty())query.addQueryItem("date",cursor);break;
    case StockChartRead::Candles:if(interval!=Minute&&interval!=Day)return {};path="/api/v1/candles";query.addQueryItem("symbol",identity.code);query.addQueryItem("interval",interval==Minute?"1m":"1d");query.addQueryItem("count","200");query.addQueryItem("adjusted","true");if(!cursor.isEmpty())query.addQueryItem("before",cursor);break;
    case StockChartRead::PriceLimits:path="/api/v1/price-limits";query.addQueryItem("symbol",identity.code);break;
    case StockChartRead::Warnings:if(!domestic)return {};path="/api/v1/stocks/"+identity.code+"/warnings";break;
    case StockChartRead::Investors:case StockChartRead::Program:if(!domestic)return {};path="/api/v1/stocks/"+identity.code+(read==StockChartRead::Investors?"/investor-trading":"/program-trades");query.addQueryItem("count","10");if(!cursor.isEmpty())query.addQueryItem("until",cursor);break;
    }
    r.operation=Operation::Reference;r.authenticated=true;r.url=QUrl("https://openapi.tossinvest.com"+path);if(!query.isEmpty())r.url.setQuery(query.toString(QUrl::FullyEncoded).replace("+","%2B"),QUrl::StrictMode);r.headers.insert("Accept","application/json");return r;
}
bool StockChartSource::isRequest(const RequestSpec& request){return request.operation==Operation::Reference&&request.url.host()=="openapi.tossinvest.com"&&NetworkTradingTransport::allowed(request);}
}
