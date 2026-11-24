#include "keefi/CoinFlowSource.h"
#include "keefi/TradingTransport.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <algorithm>
#include <limits>

namespace keefi {
namespace {
constexpr qint64 Minute=60000,FiveMinutes=300000;
constexpr int MaxTrades=10000,MaxPages=10,MaxMinutes=200;
qint64 after(qint64 at,qint64 delay){return delay>std::numeric_limits<qint64>::max()-at?std::numeric_limits<qint64>::max():at+delay;}
QString venueIdentity(CoinFlowVenue venue,const QString& symbol){return (venue==CoinFlowVenue::UpbitSpot?"coin:upbit:":venue==CoinFlowVenue::BinanceSpot?"coin:binance:":"usdm_futures:binance:")+symbol;}
bool fresh(const CoinFlowInstrument& item,qint64 now){
    if(!item.verified||item.identity!=venueIdentity(item.venue,item.symbol)||item.evidence.receivedAtMs<=0||item.evidence.receivedAtMs>now||now-item.evidence.receivedAtMs>FiveMinutes||item.evidence.source.isEmpty())return false;
    const auto url=QUrl(item.evidence.sourceUrl);const auto expected=CoinFlowProtocol::request(CoinFlowRead::Catalog,item.venue).url;auto bare=expected;bare.setQuery(QString{});
    if(!url.isValid()||url.scheme()!="https"||url.port(443)!=443||!url.userInfo().isEmpty()||url.hasFragment()||(url!=bare&&url!=expected))return false;
    if(item.venue==CoinFlowVenue::UpbitSpot)return item.grain==CoinFlowGrain::UpbitTrade&&item.symbol==item.quoteAsset+'-'+item.baseAsset&&!item.baseAsset.isEmpty()&&QStringList{"KRW","BTC","USDT"}.contains(item.quoteAsset);
    if(item.status!="TRADING"||item.baseAsset.isEmpty()||item.quoteAsset.isEmpty())return false;
    if(item.venue==CoinFlowVenue::BinanceSpot)return item.grain==CoinFlowGrain::SpotAggregate&&item.symbol==item.baseAsset+item.quoteAsset;
    return item.grain==CoinFlowGrain::Futures100msAggregate&&item.contractType=="PERPETUAL"&&item.quoteAsset=="USDT"&&item.marginAsset=="USDT"&&item.underlyingType=="COIN";
}
bool sameTrade(const CoinTradeObservation& a,const CoinTradeObservation& b){return a.id==b.id&&a.price==b.price&&a.quantity==b.quantity&&a.normalQuantity==b.normalQuantity&&a.side==b.side&&a.tradeAtMs==b.tradeAtMs;}
bool sameCandle(const MarketCandle& a,const MarketCandle& b){return a.startsAtMs==b.startsAtMs&&a.open==b.open&&a.high==b.high&&a.low==b.low&&a.close==b.close&&a.volume==b.volume;}
QString liquidationKey(const CoinLiquidationObservation& value){return value.symbol+'|'+value.side+'|'+value.price+'|'+value.averagePrice+'|'+value.originalQuantity+'|'+value.lastFilledQuantity+'|'+value.cumulativeFilledQuantity+'|'+value.orderStatus+'|'+QString::number(value.tradeAtMs)+'|'+QString::number(value.eventAtMs);}
qint64 retryUntil(const HttpResult& response,qint64 now){
    qint64 until=after(now,qMax<qint64>(1000,qint64(qMax(0,response.localRetryAfterSeconds))*1000));
    for(auto it=response.headers.cbegin();it!=response.headers.cend();++it){if(it.key().compare("Retry-After",Qt::CaseInsensitive))continue;const auto text=it.value().trimmed();bool ok=false;const auto seconds=text.toLongLong(&ok);
        if(ok&&seconds>0){until=qMax(until,after(now,seconds>std::numeric_limits<qint64>::max()/1000?std::numeric_limits<qint64>::max():seconds*1000));continue;}
        static const QRegularExpression imf("\\A(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT\\z");
        const auto value=QString::fromLatin1(text);if(imf.match(value).hasMatch()){const auto parsed=QLocale::c().toDateTime(value,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");if(parsed.isValid())until=qMax(until,QDateTime(parsed.date(),parsed.time(),QTimeZone("UTC")).toMSecsSinceEpoch());}
    }return until;
}
}
struct CoinFlowSource::Private {
    struct Lane {
        std::unique_ptr<PublicCandleSocket> socket;
        CoinFlowWindow window;QMap<QString,CoinTradeObservation> known,staged;
        QSet<QString> cursors;QString anchor; qint64 anchorAt=0;
        quint64 generation=0,epoch=0;
        qint64 openedAt=0,connectedAt=0,coverageFrom=0,coverageThrough=0,reconnectAt=0;
        qint64 trimmedBefore=0; // Held trades and coverage start here after a budget trim (minute aligned).
        int attempts=0,pages=0;bool connected=false,seeded=false,recovering=false,anchorFound=false;
    } primary,companion;
    struct Job {CoinFlowRead read=CoinFlowRead::Catalog;CoinFlowVenue venue=CoinFlowVenue::UpbitSpot;CoinFlowPageCursor cursor;bool recovery=false;QDate utcDay;};
    CoinFlowSource* q=nullptr;QPointer<TradingTransport> transport;Clock clock;QTimer* timer=nullptr;
    QMap<int,QList<CoinFlowInstrument>> catalogs;QMap<int,qint64> catalogDue,cooldowns;
    QList<Job> jobs;Job running;quint64 token=0,nextToken=1,nextGeneration=1,nextEpoch=1;
    qint64 sentAt=0,nextSend=0,nextOi=0,activeSince=0,oiWatermark=0;
    QString selected,message;bool enabled=false,pumping=false,minutesSeeded=false,oiSeeded=false;
    QMap<qint64,CoinFlowClosedMinute> minutes;QSet<qint64> minuteConflicts;std::optional<MarketCandleSeries> candleSeries;QList<CoinOiPoint> oi;QList<CoinLiquidationObservation> liquidations;QSet<QString> liquidationIds;
    QList<CoinFlowCoveragePeriod> liquidationPeriods;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    bool selectionValid()const{const auto parts=selected.split(':');if(parts.size()!=3||parts[0]!="coin")return false;if(parts[1]=="binance")return binanceIdentifierValid(parts[2],2);static const QRegularExpression upbit("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,40}\\z");return parts[1]=="upbit"&&upbit.match(parts[2]).hasMatch();}
    CoinFlowVenue selectedVenue()const{return selected.startsWith("coin:upbit:")?CoinFlowVenue::UpbitSpot:CoinFlowVenue::BinanceSpot;}
    Lane* lane(CoinFlowVenue venue){return venue==CoinFlowVenue::BinanceUsdM?&companion:&primary;}
    void state(const QString& text){if(message!=text){message=text;emit q->changed();}}
    void cancelRest(){const bool pending=token!=0;token=0;if(pending&&transport)transport->abort();}
    void clearLane(Lane& value,bool keepHistory=false){
        value.generation=nextGeneration++;value.connected=value.seeded=false;value.openedAt=value.connectedAt=value.coverageFrom=value.coverageThrough=value.reconnectAt=0;value.staged.clear();value.cursors.clear();value.pages=0;
        if(value.socket)value.socket->abort();if(!keepHistory){value.window={};value.known.clear();value.anchor.clear();value.anchorAt=0;value.recovering=value.anchorFound=false;value.trimmedBefore=0;}
    }
    void clearWork(bool clearMinutes){cancelRest();jobs.clear();clearLane(primary);clearLane(companion);nextOi=0;oi.clear();oiSeeded=false;oiWatermark=0;liquidations.clear();liquidationIds.clear();liquidationPeriods.clear();if(clearMinutes){minutes.clear();minuteConflicts.clear();candleSeries.reset();minutesSeeded=false;}}
    CoinFlowInstrument find(CoinFlowVenue venue,const QString& symbol)const{CoinFlowInstrument result;int matches=0;for(const auto& item:catalogs.value(int(venue)))if(item.symbol==symbol&&fresh(item,now())){result=item;++matches;}return matches==1?result:CoinFlowInstrument{};}
    void enqueue(const Job& job,bool first=false){
        const auto same=[&](const Job& other){return other.read==job.read&&other.venue==job.venue&&other.cursor.tradeId==job.cursor.tradeId&&other.cursor.fromMs==job.cursor.fromMs&&other.cursor.throughMs==job.cursor.throughMs&&other.cursor.daysAgo==job.cursor.daysAgo;};
        if((token&&same(running))||std::any_of(jobs.cbegin(),jobs.cend(),same))return;if(first)jobs.prepend(job);else jobs.append(job);
    }
    void ensure(CoinFlowVenue venue){
        if(!enabled||!selectionValid()||!transport||now()<catalogDue.value(int(venue)))return;
        bool valid=false;for(const auto& item:catalogs.value(int(venue)))if(fresh(item,now())){valid=true;break;}if(valid)return;
        enqueue({CoinFlowRead::Catalog,venue,{},false,{}});
    }
    void mark(Lane& value,CoinFlowCoverage coverage,const QString& reason){
        const auto at=now();value.window.reason=reason;value.window.periods.append({value.coverageFrom?value.coverageFrom:at,at,at,value.epoch,coverage,reason});
        while(value.window.periods.size()>400)value.window.periods.removeFirst();
    }
    void failure(Lane& value,const QString& reason,CoinFlowCoverage coverage=CoinFlowCoverage::Disconnected){
        if(!enabled||!value.window.instrument.verified)return;
        if(!value.window.trades.isEmpty()){const auto last=value.window.trades.last();value.anchor=last.id;value.anchorAt=last.tradeAtMs;}
        mark(value,coverage,reason);clearLane(value,true);value.recovering=!value.anchor.isEmpty();value.anchorFound=false;value.reconnectAt=after(now(),qMin<qint64>(60000,1000LL*(1LL<<qMin(value.attempts++,6))));
        const auto venue=value.window.instrument.venue;jobs.erase(std::remove_if(jobs.begin(),jobs.end(),[&](const auto& job){return job.read!=CoinFlowRead::Catalog&&job.venue==venue;}),jobs.end());
        if(token&&running.read!=CoinFlowRead::Catalog&&running.venue==venue)cancelRest();state(reason);emit q->changed();
    }
    void open(Lane& value,const CoinFlowInstrument& instrument){
        const bool same=value.window.instrument.identity==instrument.identity&&value.window.instrument.symbol==instrument.symbol;
        if(same&&value.reconnectAt){value.window.instrument=instrument;return;}
        if(same&&value.openedAt){value.window.instrument=instrument;return;}
        if(!same)clearLane(value);value.window.instrument=instrument;value.epoch=nextEpoch++;value.generation=nextGeneration++;value.openedAt=now();value.pages=0;value.cursors.clear();value.window.reason="공개 체결 표본 준비 중 · 거래소 전체 완전성 보장 아님";
        if(value.socket)value.socket->open(value.generation,CoinFlowProtocol::streamEndpoint(instrument));
        CoinFlowPageCursor cursor;const auto at=now();if(value.recovering&&instrument.venue!=CoinFlowVenue::UpbitSpot){
            if(instrument.venue==CoinFlowVenue::BinanceUsdM&&at-value.anchorAt>48*3600000LL){value.recovering=false;mark(value,CoinFlowCoverage::Partial,"48시간 원천 이력 밖 · 새 관측 기준선");}
            else cursor.tradeId=value.anchor;
        }
        enqueue({CoinFlowRead::Trades,instrument.venue,cursor,value.recovering,QDateTime::fromMSecsSinceEpoch(at,QTimeZone("UTC")).date()});
        if(instrument.venue==CoinFlowVenue::BinanceUsdM&&nextOi<=at){enqueue({CoinFlowRead::OpenInterest,instrument.venue,{},false,{}});nextOi=after(at-at%FiveMinutes,FiveMinutes);}
    }
    void reconcile(){
        if(!enabled||!selectionValid())return;const auto venue=selectedVenue();const auto symbol=selected.section(':',2,2);const auto item=find(venue,symbol);
        if(!item.verified){if(primary.window.instrument.verified||companion.window.instrument.verified){cancelRest();jobs.clear();clearLane(primary);clearLane(companion);oi.clear();liquidations.clear();liquidationIds.clear();}ensure(venue);state("공개 종목 메타데이터 확인 대기");return;}
        open(primary,item);
        CoinFlowInstrument pair;
        if(venue==CoinFlowVenue::BinanceSpot&&item.quoteAsset=="USDT"){
            int matches=0;for(const auto& candidate:catalogs.value(int(CoinFlowVenue::BinanceUsdM)))if(fresh(candidate,now())&&candidate.baseAsset==item.baseAsset&&candidate.quoteAsset==item.quoteAsset){pair=candidate;++matches;}if(matches!=1)pair={};
            if(!pair.verified)ensure(CoinFlowVenue::BinanceUsdM);
        }
        if(pair.verified)open(companion,pair);else if(companion.window.instrument.verified){if(token&&running.venue==CoinFlowVenue::BinanceUsdM)cancelRest();clearLane(companion);oi.clear();oiSeeded=false;liquidations.clear();liquidationIds.clear();}
    }
    void ready(Lane& value,quint64 generation){
        if(!enabled||generation!=value.generation||!fresh(value.window.instrument,now()))return;value.connected=true;value.connectedAt=now();
        if(value.window.instrument.venue==CoinFlowVenue::UpbitSpot&&value.socket){const auto payload=QJsonArray{QJsonObject{{"ticket",QString::number(generation)}},QJsonObject{{"type","trade"},{"codes",QJsonArray{value.window.instrument.symbol}}},QJsonObject{{"format","DEFAULT"}}};value.socket->send(generation,QJsonDocument(payload).toJson(QJsonDocument::Compact));}
        if(value.seeded)value.coverageFrom=now();emit q->changed();
    }
    bool coverage(Lane& value){
        if(!value.connected||!value.seeded||!value.coverageFrom||!fresh(value.window.instrument,now()))return false;
        auto from=value.coverageThrough?value.coverageThrough:after(value.coverageFrom-value.coverageFrom%Minute,value.coverageFrom%Minute?Minute:0);const auto through=now()-now()%Minute;
        int count=0;while(from+Minute<=through&&count++<200){value.window.periods.append({from,from+Minute,now(),value.epoch,CoinFlowCoverage::ObservedContinuous,"앱 연결 관측 연속 · 전체 체결 전송 보장 아님"});if(value.window.instrument.venue==CoinFlowVenue::BinanceUsdM)liquidationPeriods.append({from,from+Minute,now(),value.epoch,CoinFlowCoverage::Partial,"종목별 1초 마지막 청산 snapshot · 전체 아님"});from+=Minute;value.coverageThrough=from;}
        while(value.window.periods.size()>400)value.window.periods.removeFirst();while(liquidationPeriods.size()>400)liquidationPeriods.removeFirst();return count>0;
    }
    // Bring a full window back to 4/5 of the budget by dropping its oldest whole minutes, never the
    // minute being written. Their coverage slices are withdrawn too, so no calculation can read an
    // emptied minute as observed with no flow. Only a prefix goes: every later minute keeps exactly
    // the same trades, the same 200-trade baselines and the same coverage, so its results stay.
    bool trim(Lane& value,qint64 incomingAt){
        auto& trades=value.window.trades;constexpr int target=MaxTrades*4/5;
        if(trades.size()<=target)return false;
        const qint64 newest=std::max(incomingAt,trades.last().tradeAtMs),current=newest-newest%Minute;
        const auto& lastDropped=trades.at(trades.size()-target-1);
        const qint64 boundary=lastDropped.tradeAtMs-lastDropped.tradeAtMs%Minute+Minute;
        if(boundary>current)return false;
        const auto keep=std::lower_bound(trades.cbegin(),trades.cend(),boundary,[](const auto& held,qint64 time){return held.tradeAtMs<time;});
        const auto removed=keep-trades.cbegin();
        for(qsizetype i=0;i<removed;++i)value.known.remove(trades.at(i).id);
        trades.remove(0,removed);
        auto& periods=value.window.periods;
        for(auto it=periods.begin();it!=periods.end();){if(it->throughMs<=boundary){it=periods.erase(it);continue;}if(it->fromMs<boundary)it->fromMs=boundary;++it;}
        value.trimmedBefore=std::max(value.trimmedBefore,boundary);
        return true;
    }
    bool add(Lane& value,CoinTradeObservation observation){
        const auto old=value.known.constFind(observation.id);if(old!=value.known.cend()){
            if(!sameTrade(old.value(),observation)){
                // Keep both raw observations for record readers (bounded); the window keeps the first.
                if(value.window.conflicts.size()<256)value.window.conflicts.append({old.value(),observation});
                failure(value,"동일 체결 ID의 원천 값 충돌 · 부분 표본",CoinFlowCoverage::Partial);return false;
            }++value.window.duplicateCount;return true;
        }
        // A trade older than a trimmed minute is not held again: that minute's coverage was withdrawn.
        if(observation.tradeAtMs<value.trimmedBefore)return true;
        // At the budget the oldest whole minutes are dropped. Only a single minute holding more than
        // the budget still ends the window as an overflow (previously every full window did, every
        // 15-18 minutes for Binance BTC, followed by a reconnect and up to 10 REST recovery pages).
        if(value.known.size()>=MaxTrades&&!trim(value,observation.tradeAtMs)){failure(value,"체결 표본 예산 초과 · 기준선과 관측 구간 분리",CoinFlowCoverage::Overflow);value.known.clear();value.window.trades.clear();return false;}
        value.known.insert(observation.id,observation);
        // Same order as appending and stable-sorting by trade time, without re-sorting the whole window per trade:
        // a new trade goes after every held trade with the same or an earlier time (usually the end).
        auto& trades=value.window.trades;const auto at=std::upper_bound(trades.cbegin(),trades.cend(),observation.tradeAtMs,[](qint64 time,const auto& held){return time<held.tradeAtMs;});
        trades.insert(at-trades.cbegin(),observation);
        const auto venue=value.window.instrument.venue;
        const QString source=venue==CoinFlowVenue::UpbitSpot?"업비트 BID/ASK · REST seed/WS 관측 · 각 관측 시각 별도":"Binance taker 집계 · REST seed/WS 관측 · 각 관측 시각 별도";
        const QString url=venue==CoinFlowVenue::UpbitSpot?"https://docs.upbit.com/kr/reference/websocket-trade":venue==CoinFlowVenue::BinanceSpot?"https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-streams.md":"https://developers.binance.com/en/docs/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/market";
        value.window.evidence={source,url,observation.eventAtMs,observation.receivedAtMs,observation.knownAtMs};return true;
    }
    void receive(Lane& value,quint64 generation,const QByteArray& bytes){
        if(!enabled||generation!=value.generation||!value.connected||!fresh(value.window.instrument,now()))return;
        auto parsed=CoinFlowProtocol::tradeFrame(value.window.instrument,bytes,now(),value.epoch);
        if(parsed.available){for(auto observation:parsed.trades){if(observation.snapshot)continue;if(!value.seeded){
                    if(value.staged.size()>=MaxTrades){failure(value,"재연결 대기 표본 예산 초과",CoinFlowCoverage::Overflow);return;}
                    const auto found=value.staged.constFind(observation.id);if(found!=value.staged.cend()){
                        if(!sameTrade(found.value(),observation)){
                            if(value.window.conflicts.size()<256)value.window.conflicts.append({found.value(),observation});
                            failure(value,"대기 중 동일 체결 ID 충돌",CoinFlowCoverage::Partial);return;
                        }
                        ++value.window.duplicateCount;continue; // The first receipt/known-at stays frozen.
                    }value.staged.insert(observation.id,observation);
                }else if(!add(value,observation))return;}coverage(value);emit q->changed();return;}
        if(value.window.instrument.venue!=CoinFlowVenue::BinanceUsdM)return;const auto liquidation=CoinFlowProtocol::liquidationFrame(value.window.instrument,bytes,now(),value.epoch);if(!liquidation.available)return;
        for(const auto& observation:liquidation.observations){const auto id=liquidationKey(observation);if(liquidationIds.contains(id))continue;liquidationIds.insert(id);liquidations.append(observation);while(liquidations.size()>1000){liquidationIds.remove(liquidationKey(liquidations.first()));liquidations.removeFirst();}}
        emit q->changed();
    }
    void seedFinished(Lane& value,bool recovered){
        value.seeded=true;value.coverageFrom=now();value.coverageThrough=0;value.recovering=false;value.attempts=0;
        mark(value,recovered?CoinFlowCoverage::RecoveredOverlap:CoinFlowCoverage::WarmingUp,recovered?"REST 실제 anchor 겹침 복구 · 과거 알림 재생 없음":"새 관측 기준선 · 완전한 완료 1분 표본 준비 중");
        const auto staged=value.staged.values();value.staged.clear();for(const auto& observation:staged)if(!add(value,observation))return;emit q->changed();
    }
    bool tradeResult(const HttpResult& response){
        auto& value=*lane(running.venue);const auto parsed=CoinFlowProtocol::trades(value.window.instrument,response,now(),value.epoch);if(!parsed.available)return false;++value.pages;
        bool overlap=false;int added=0;for(auto observation:parsed.trades){
            if(running.venue==CoinFlowVenue::UpbitSpot&&running.utcDay.isValid()&&QDateTime::fromMSecsSinceEpoch(observation.tradeAtMs,QTimeZone("UTC")).date()!=running.utcDay)return false;
            if(observation.id==value.anchor)value.anchorFound=true;const auto staged=value.staged.find(observation.id);
            if(staged!=value.staged.end()){
                if(!sameTrade(staged.value(),observation)){
                    if(value.window.conflicts.size()<256)value.window.conflicts.append({staged.value(),observation});
                    failure(value,"복구 REST와 대기 체결 ID의 원천 값 충돌",CoinFlowCoverage::Partial);return true;
                }
                overlap=true;observation=staged.value();value.staged.erase(staged);++value.window.duplicateCount;
            }
            if(!value.known.contains(observation.id))++added;if(!add(value,observation))return true;
        }
        if(!running.recovery){seedFinished(value,false);return true;}
        if(value.anchorFound&&(overlap||parsed.trades.size()<500)){seedFinished(value,overlap);if(!overlap)mark(value,CoinFlowCoverage::Partial,"anchor 확인 · 실시간 겹침은 미확인, 새 구간만 관측");return true;}
        if(value.pages>=MaxPages){seedFinished(value,false);mark(value,CoinFlowCoverage::Partial,"10페이지 복구 예산 내 anchor·겹침 미확인");return true;}
        Job next=running;
        if(parsed.trades.isEmpty()&&running.venue==CoinFlowVenue::UpbitSpot){next.utcDay=running.utcDay.addDays(-1);const auto offset=next.utcDay.daysTo(QDateTime::fromMSecsSinceEpoch(now(),QTimeZone("UTC")).date());if(offset<1||offset>7){seedFinished(value,false);mark(value,CoinFlowCoverage::Partial,"업비트 UTC 체결 보존 구간 밖");return true;}next.cursor={};next.cursor.daysAgo=int(offset);}
        else{
            if(parsed.nextCursor.isEmpty()||parsed.nextCursor==running.cursor.tradeId||value.cursors.contains(parsed.nextCursor)||(!added&&parsed.trades.size()>=500)){seedFinished(value,false);mark(value,CoinFlowCoverage::Partial,"체결 복구 cursor 반복·진전 없음");return true;}
            value.cursors.insert(parsed.nextCursor);next.cursor.tradeId=parsed.nextCursor;
        }
        enqueue(next,true);return true;
    }
    void finished(quint64 replyToken,const HttpResult& response){
        if(!enabled||!token||replyToken!=token)return;token=0;const auto time=now();
        if(response.status==429||response.status==418||response.localRetryAfterSeconds>0){const auto until=retryUntil(response,time);cooldowns[int(running.venue)]=qMax(cooldowns.value(int(running.venue)),until);nextSend=qMax(nextSend,until);enqueue(running,true);state("공개 체결 원천 요청 제한 · 원래 Retry-After 유지");return;}
        if(running.read==CoinFlowRead::Catalog){const auto parsed=CoinFlowProtocol::catalog(running.venue,response,time);if(parsed.available){catalogs[int(running.venue)]=parsed.instruments;catalogDue[int(running.venue)]=0;reconcile();}else{catalogs[int(running.venue)].clear();catalogDue[int(running.venue)]=after(time,60000);state(parsed.reason);}emit q->changed();return;}
        auto* value=lane(running.venue);if(!fresh(value->window.instrument,time))return;
        if(running.read==CoinFlowRead::Trades){if(!tradeResult(response))failure(*value,"공개 체결 REST 응답 검증 실패 · 부분 관측",CoinFlowCoverage::Partial);return;}
        const auto parsed=CoinFlowProtocol::openInterest(value->window.instrument,response,time);if(!parsed.available){oi.clear();state(parsed.reason);emit q->changed();return;}
        for(auto point:parsed.points){auto found=std::find_if(oi.begin(),oi.end(),[&](const auto& existing){return existing.periodEndMs==point.periodEndMs;});if(found!=oi.end()){if(found->quantity!=point.quantity||found->value!=point.value){oi.clear();oiSeeded=false;state("동일 5분 OI 원천 값 충돌");break;}continue;}
            point.liveEligible=oiSeeded&&point.periodEndMs>oiWatermark&&point.periodEndMs>=activeSince;oi.append(point);oiWatermark=qMax(oiWatermark,point.periodEndMs);}
        oiSeeded=true;std::sort(oi.begin(),oi.end(),[](const auto& a,const auto& b){return a.periodEndMs<b.periodEndMs;});while(oi.size()>500)oi.removeFirst();emit q->changed();
    }
    void pump(){
        if(!enabled||pumping||!selectionValid())return;const QScopedValueRollback guard(pumping,true);reconcile();const auto at=now();
        for(auto* value:{&primary,&companion}){if(!value->window.instrument.verified)continue;if(value->reconnectAt&&at>=value->reconnectAt){value->reconnectAt=0;value->openedAt=0;open(*value,value->window.instrument);}else if(value->socket&&value->openedAt&&!value->connected&&at-value->openedAt>=10000&&!value->reconnectAt)failure(*value,"공개 체결 WebSocket 연결 시간 초과");else if(value->connected&&at-value->openedAt>=24*3600000LL)failure(*value,"공개 체결 24시간 연결 주기 종료");coverage(*value);}
        if(companion.window.instrument.verified&&at>=nextOi){enqueue({CoinFlowRead::OpenInterest,CoinFlowVenue::BinanceUsdM,{},false,{}});nextOi=after(at-at%FiveMinutes,FiveMinutes);}
        if(token){if(at>=sentAt&&at-sentAt<10000)return;const auto job=running;cancelRest();nextSend=qMax(nextSend,after(at,5000));if(job.read==CoinFlowRead::Catalog){catalogDue[int(job.venue)]=after(at,5000);enqueue(job,true);state("공개 메타데이터 응답 시간 초과");}else if(job.read==CoinFlowRead::Trades)failure(*lane(job.venue),"공개 체결 REST 응답 시간 초과",CoinFlowCoverage::Partial);else{oi.clear();state("5분 OI 응답 시간 초과");}return;}
        if(!transport||jobs.isEmpty()||at<nextSend)return;
        int selectedJob=-1;for(int i=0;i<jobs.size();++i)if(at>=cooldowns.value(int(jobs.at(i).venue))){selectedJob=i;break;}if(selectedJob<0)return;running=jobs.takeAt(selectedJob);
        if(running.read!=CoinFlowRead::Catalog&&!fresh(lane(running.venue)->window.instrument,at))return;
        if(running.venue==CoinFlowVenue::UpbitSpot&&running.utcDay.isValid()){const auto offset=running.utcDay.daysTo(QDateTime::fromMSecsSinceEpoch(at,QTimeZone("UTC")).date());if(offset<0||offset>7){failure(*lane(running.venue),"UTC 체결 이력 보존 구간 밖",CoinFlowCoverage::Partial);return;}running.cursor.daysAgo=offset?int(offset):-1;}
        const auto symbol=running.read==CoinFlowRead::Catalog?QString{}:lane(running.venue)->window.instrument.symbol;const auto request=CoinFlowProtocol::request(running.read,running.venue,symbol,running.cursor);if(!CoinFlowProtocol::isRequest(request)){state("공개 체결 GET 범위 검증 실패");return;}
        token=nextToken++;sentAt=at;nextSend=after(at,500);transport->send(token,request);
    }
};
CoinFlowSource::CoinFlowSource(TradingTransport* transport,QObject* parent):CoinFlowSource(transport,{},{},parent,{}){}
CoinFlowSource::CoinFlowSource(TradingTransport* transport,std::unique_ptr<PublicCandleSocket> primary,std::unique_ptr<PublicCandleSocket> companion,QObject* parent,Clock clock):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->clock=std::move(clock);d_->primary.socket=std::move(primary);d_->companion.socket=std::move(companion);d_->timer=new QTimer(this);d_->timer->setInterval(100);
    connect(d_->timer,&QTimer::timeout,this,[this]{d_->pump();});
    for(auto* value:{&d_->primary,&d_->companion})if(value->socket){connect(value->socket.get(),&PublicCandleSocket::connected,this,[this,value](quint64 generation){d_->ready(*value,generation);});connect(value->socket.get(),&PublicCandleSocket::received,this,[this,value](quint64 generation,const QByteArray& bytes){d_->receive(*value,generation,bytes);});connect(value->socket.get(),&PublicCandleSocket::failed,this,[this,value](quint64 generation,int){if(generation==value->generation)d_->failure(*value,"공개 체결 연결 끊김 · REST 겹침 복구 대기");});}
    if(transport){connect(transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& response){d_->finished(token,response);});connect(transport,&QObject::destroyed,this,[this]{d_->transport=nullptr;d_->token=0;d_->jobs.clear();d_->state("공개 체결 전용 transport 없음");});}
}
CoinFlowSource::~CoinFlowSource(){d_->enabled=false;d_->timer->stop();if(d_->transport)disconnect(d_->transport,nullptr,this,nullptr);for(auto* value:{&d_->primary,&d_->companion})if(value->socket)disconnect(value->socket.get(),nullptr,this,nullptr);d_->clearWork(true);}
void CoinFlowSource::setSelection(const QString& identity){if(d_->selected==identity)return;d_->clearWork(true);d_->selected=identity;d_->activeSince=d_->now();d_->pump();emit changed();}
void CoinFlowSource::setActive(bool enabled){if(d_->enabled==enabled)return;d_->enabled=enabled;if(enabled){d_->activeSince=d_->now();d_->timer->start();d_->pump();}else{d_->timer->stop();d_->clearWork(true);}emit changed();}
bool CoinFlowSource::active()const{return d_->enabled;}
void CoinFlowSource::setPublicCatalog(CoinFlowVenue venue,const QList<CoinFlowInstrument>& values){
    QList<CoinFlowInstrument> accepted;QSet<QString> seen;bool valid=values.size()<=20000;for(const auto& value:values){if(value.venue!=venue||seen.contains(value.symbol)){valid=false;break;}seen.insert(value.symbol);if(!value.verified)continue;if(!fresh(value,d_->now())){valid=false;break;}accepted.append(value);}d_->catalogs[int(venue)]=valid?accepted:QList<CoinFlowInstrument>{};d_->catalogDue[int(venue)]=0;if(valid&&!accepted.isEmpty())d_->jobs.erase(std::remove_if(d_->jobs.begin(),d_->jobs.end(),[&](const auto& job){return job.read==CoinFlowRead::Catalog&&job.venue==venue;}),d_->jobs.end());d_->reconcile();d_->pump();emit changed();
}
void CoinFlowSource::setMinuteSeries(const MarketCandleSeries& series){
    if(series.identity!=d_->selected||series.intervalMs!=Minute||!series.available||series.receivedAtMs<=0||series.receivedAtMs>d_->now()||d_->now()-series.receivedAtMs>90000)return;
    const auto parts=d_->selected.split(':');if(parts.size()!=3)return;if(parts[1]=="binance"){const auto instrument=d_->find(CoinFlowVenue::BinanceSpot,parts[2]);if(!instrument.verified||series.asset!=instrument.baseAsset||series.currency!=instrument.quoteAsset)return;}else if(series.asset!=parts[2].section('-',1,1)||series.currency!=parts[2].section('-',0,0))return;
    const auto canonical=MarketCandles::request(series.identity,Minute).url.toString();if(series.sourceUrl!=canonical)return;
    bool changed=d_->coverage(d_->primary);changed=d_->coverage(d_->companion)||changed;
    QMap<qint64,MarketCandle> incoming;for(const auto& candle:series.candles){if(candle.startsAtMs<=0||candle.startsAtMs%Minute||candle.startsAtMs>std::numeric_limits<qint64>::max()-Minute||candle.startsAtMs+Minute>series.receivedAtMs)continue;incoming.insert(candle.startsAtMs,candle);}
    // Chart and alert snapshots can overlap with different window ends. Rows
    // already evicted from the bounded history must not be reinserted and
    // immediately trimmed, which would emit changed without changing evidence.
    while(incoming.size()>MaxMinutes)incoming.erase(incoming.begin());
    if(d_->minutes.size()>=MaxMinutes)while(!incoming.isEmpty()&&incoming.firstKey()<d_->minutes.firstKey())incoming.erase(incoming.begin());
    int newRows=0;for(const auto& candle:incoming)if(!d_->minutes.contains(candle.startsAtMs))++newRows;const bool live=d_->minutesSeeded&&newRows==1;
    for(const auto& candle:incoming){
        const auto found=d_->minutes.constFind(candle.startsAtMs);if(found!=d_->minutes.cend()){
            if(!sameCandle(found->candle,candle)&&!d_->minuteConflicts.contains(candle.startsAtMs)){d_->minuteConflicts.insert(candle.startsAtMs);d_->message="이미 결정한 완료 1분 원천 값 변경 · 다음 구간 연결 끊김";changed=true;}continue;
        }
        CoinFlowClosedMinute value;value.candle=candle;value.completesAtMs=candle.startsAtMs+Minute;value.knownAtMs=series.receivedAtMs;value.decisionAtMs=d_->now();value.liveEligible=live;value.connectsPrevious=d_->minutes.contains(candle.startsAtMs-Minute)&&!d_->minuteConflicts.contains(candle.startsAtMs-Minute);value.evidence={"공식 실제 완료 1분",canonical,series.sourceTimeMs,series.receivedAtMs,series.receivedAtMs};d_->minutes.insert(candle.startsAtMs,value);changed=true;
    }
    while(d_->minutes.size()>MaxMinutes)d_->minutes.erase(d_->minutes.begin());for(auto it=d_->minuteConflicts.begin();it!=d_->minuteConflicts.end();)if(!d_->minutes.contains(*it))it=d_->minuteConflicts.erase(it);else ++it;
    if(newRows){auto proof=series;proof.candles.clear();proof.forming.reset();for(const auto& row:d_->minutes)proof.candles.append(row.candle);d_->candleSeries=proof;}
    d_->minutesSeeded=true;if(changed)emit this->changed();
}
CoinFlowSnapshot CoinFlowSource::snapshot(qint64 asOf)const{
    CoinFlowSnapshot result;result.identity=d_->selected;if(!d_->enabled)return result;const auto at=asOf?asOf:d_->now();result.primary=d_->primary.window;
    if(!fresh(result.primary.instrument,at)){result.primary={};result.primary.reason="공개 종목 메타데이터 만료 또는 미확인";}
    if(result.primary.instrument.verified&&fresh(d_->companion.window.instrument,at))result.companion=d_->companion.window;
    for(const auto& minute:d_->minutes)if(minute.knownAtMs<=at)result.minutes.append(minute);if(d_->candleSeries&&d_->candleSeries->receivedAtMs<=at)result.candleSeries=d_->candleSeries;if(result.companion){result.openInterest=d_->oi;result.liquidations=d_->liquidations;result.liquidationCoverage=d_->liquidationPeriods;}
    return result;
}
QString CoinFlowSource::status()const{return d_->message;}
void CoinFlowSource::pump(){d_->pump();}
void CoinFlowSource::stop(){setActive(false);}
RequestSpec CoinFlowSource::request(CoinFlowRead read,CoinFlowVenue venue,const QString& symbol,const CoinFlowPageCursor& cursor){return CoinFlowProtocol::request(read,venue,symbol,cursor);}
bool CoinFlowSource::isRequest(const RequestSpec& request){return CoinFlowProtocol::isRequest(request);}
}
