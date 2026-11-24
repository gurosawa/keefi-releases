#include "keefi/ChartAlertEngine.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/ChartDrawings.h"
#include "keefi/MarketFlowPresentation.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
constexpr qint64 ReceiptAge=90000,HoldingAge=180000,RetryDelay=5000,Day=86400000;
const QString Holdings=QStringLiteral("@holdings");
bool supported(qint64 interval){return interval==60000||interval==300000||interval==900000||interval==3600000||interval==14400000||interval==Day;}
bool supported(const WatchlistIdentity& id){return WatchlistWorkspaceList::validIdentity(id)&&
    ((id.market=="coin"&&(id.source=="upbit"||id.source=="binance"))||(id.market=="stock"&&(id.source=="KR"||id.source=="US")));}
bool flowIdentity(const WatchlistIdentity& id){return supported(id)&&id.market=="coin";}
bool ruleInterval(const ChartAlertRule& rule,ChartAlertKind kind,qint64 interval){return kind==ChartAlertKind::HorizontalLine?supported(interval):kind==ChartAlertKind::Whale?interval==60000:rule.intervalMs==interval;}
bool riskIdentity(const WatchlistIdentity& id){static const QRegularExpression symbol(QStringLiteral("\\A[A-Z0-9]{1,80}\\z"));return id.market=="usdm_futures"&&id.source=="binance"&&symbol.match(id.code).hasMatch();}
bool listIdValid(const QString& id){static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));return id!="holdings"&&pattern.match(id).hasMatch();}
bool fresh(qint64 received,qint64 now,qint64 limit){return received>0&&received<=now&&now-received<=limit;}
bool marketMetricValid(MarketFlowMetric metric){return metric==MarketFlowMetric::KoreaGap||metric==MarketFlowMetric::Funding||metric==MarketFlowMetric::CoinbaseGap||metric==MarketFlowMetric::OpenInterest;}
bool marketAssetValid(const QString& asset){static const QRegularExpression pattern("\\A[A-Z0-9]{1,20}\\z");return pattern.match(asset).hasMatch();}
bool marketStateValid(MarketFlowState state){return state==MarketFlowState::Cool||state==MarketFlowState::Normal||state==MarketFlowState::Hot;}
bool contains(const QList<WatchlistIdentity>& ids,const WatchlistIdentity& id){return ids.contains(id);}
QString laneKey(const WatchlistIdentity& id,qint64 interval){return id.key()+"|"+QString::number(interval);}
QString kindKey(ChartAlertKind kind){return QString::number(int(kind));}
QString movementTitle(const QString& title,MovementDirection direction){
    const auto marker=direction==MovementDirection::Up?QStringLiteral("▲ "):direction==MovementDirection::Down?QStringLiteral("▼ "):QString{};
    return marker.isEmpty()||title.startsWith(marker)?title:marker+title;
}
ChartAlertRule defaultRule(const QString& id){ChartAlertRule rule;rule.listId=id;if(id==Holdings){rule.enabled=true;rule.liquidation=true;}return rule;}
bool settingsValid(const ChartAlertSettings& settings){return settings.quietStartMinute>=0&&settings.quietStartMinute<1440&&settings.quietEndMinute>=0&&settings.quietEndMinute<1440
    &&settings.dailyLimit>=1&&settings.dailyLimit<=1000&&settings.mutedUntilMs>=0&&settings.timeZoneId.size()<=128&&QTimeZone(settings.timeZoneId).isValid();}
bool ruleValid(const ChartAlertRule& rule){return supported(rule.intervalMs)&&rule.cooldownMs>=0&&rule.cooldownMs<=Day&&rule.minimumScore>=0&&rule.minimumScore<=10
    &&std::isfinite(rule.liquidationDistancePercent)&&rule.liquidationDistancePercent>0&&rule.liquidationDistancePercent<=100&&(!rule.liquidation||rule.listId==Holdings)&&(!rule.horizontal||rule.listId!=Holdings);}
bool exactKeys(const QJsonObject& object,std::initializer_list<const char*> keys){
    if(object.size()!=qsizetype(keys.size()))return false;
    for(const auto* key:keys)if(!object.contains(QLatin1String(key)))return false;return true;
}
bool integer(const QJsonValue& value,qint64 minimum,qint64 maximum,qint64* out){
    if(!value.isDouble())return false;const auto number=value.toDouble();
    if(!std::isfinite(number)||number<std::numeric_limits<qint64>::min()||number>=double(std::numeric_limits<qint64>::max())||std::floor(number)!=number)return false;
    const auto result=qint64(number);if(result<minimum||result>maximum)return false;*out=result;return true;
}
bool price(const QString& text,double* result){
    static const QRegularExpression pattern(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));
    if(!pattern.match(text).hasMatch())return false;bool ok=false;*result=text.toDouble(&ok);return ok&&std::isfinite(*result)&&*result>0;
}
int lineDecimalCompare(const QString& a,const QString& b){
    const auto ap=a.indexOf('.'),bp=b.indexOf('.');const auto ai=ap<0?a:a.left(ap),bi=bp<0?b:b.left(bp);
    if(ai.size()!=bi.size())return ai.size()<bi.size()?-1:1;const auto whole=QString::compare(ai,bi);if(whole)return whole<0?-1:1;
    const auto af=ap<0?QString{}:a.mid(ap+1),bf=bp<0?QString{}:b.mid(bp+1);
    for(qsizetype i=0;i<qMax(af.size(),bf.size());++i){const auto ac=i<af.size()?af.at(i):QChar('0'),bc=i<bf.size()?bf.at(i):QChar('0');if(ac!=bc)return ac<bc?-1:1;}return 0;
}
bool lineDecimal(const QString& text,bool volume=false){
    static const QRegularExpression grammar("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z");static const QString maximum="1"+QString(100,'0'),minimum="0."+QString(99,'0')+"1";
    return text.size()<=ChartDrawings::MaxPriceChars&&grammar.match(text).hasMatch()&&lineDecimalCompare(text,maximum)<=0&&(volume||lineDecimalCompare(text,minimum)>=0);
}
bool lineCandle(const MarketCandle& row,qint64 received){
    return row.startsAtMs>0&&row.lastStoredTickMs>=0&&(!row.lastStoredTickMs||(row.lastStoredTickMs>=row.startsAtMs&&row.lastStoredTickMs<=received))&&lineDecimal(row.open)&&lineDecimal(row.high)&&lineDecimal(row.low)&&lineDecimal(row.close)&&lineDecimal(row.volume,true)
        &&lineDecimalCompare(row.low,row.open)<=0&&lineDecimalCompare(row.low,row.close)<=0&&lineDecimalCompare(row.high,row.open)>=0&&lineDecimalCompare(row.high,row.close)>=0;
}
bool sameLineCandle(const MarketCandle& a,const MarketCandle& b){return a.startsAtMs==b.startsAtMs&&a.lastStoredTickMs==b.lastStoredTickMs&&a.open==b.open&&a.high==b.high&&a.low==b.low&&a.close==b.close&&a.volume==b.volume;}
bool coinLineSeries(const MarketCandleSeries& series,qint64 now){
    const auto id=WatchlistWorkspaceList::identityFromKey(series.identity);if(!flowIdentity(id)||!supported(series.intervalMs)||!series.available||!fresh(series.receivedAtMs,now,ReceiptAge)||series.sourceTimeMs<0||series.sourceTimeMs>series.receivedAtMs||series.candles.isEmpty()||series.candles.size()>200)return false;
    const auto request=MarketCandles::request(series.identity,series.intervalMs);if(request.url.isEmpty()||series.sourceUrl!=request.url.toString())return false;
    static const QRegularExpression upbit("\\A[A-Z0-9]{1,40}\\z");const bool identifiers=id.source=="binance"?binanceIdentifierValid(series.asset)&&binanceIdentifierValid(series.currency):upbit.match(series.asset).hasMatch()&&upbit.match(series.currency).hasMatch();
    if(!identifiers||id.code!=(id.source=="upbit"?series.currency+"-"+series.asset:series.asset+series.currency))return false;
    qint64 previous=0;for(const auto& row:series.candles){if(!lineCandle(row,series.receivedAtMs)||row.startsAtMs<=previous||row.startsAtMs%series.intervalMs||row.startsAtMs>std::numeric_limits<qint64>::max()-series.intervalMs||row.startsAtMs+series.intervalMs>series.receivedAtMs)return false;previous=row.startsAtMs;}return true;
}
bool coinCandleFacts(const CoinFlowSnapshot& input,qint64 now,QString* asset,QString* currency){
    const auto identity=WatchlistWorkspaceList::identityFromKey(input.identity);if(!flowIdentity(identity))return false;
    static const QRegularExpression symbol(QStringLiteral("\\A[A-Z0-9]{1,40}\\z"));
    const auto matches=[&](const QString& base,const QString& quote){
        const bool identifiers=identity.source=="binance"?binanceIdentifierValid(base)&&binanceIdentifierValid(quote):
            symbol.match(base).hasMatch()&&symbol.match(quote).hasMatch();
        return identifiers&&identity.code==(identity.source=="upbit"?quote+"-"+base:base+quote);
    };
    if(input.candleSeries){
        const auto& series=*input.candleSeries;const auto request=MarketCandles::request(input.identity,60000);
        if(!series.available||series.identity!=input.identity||series.intervalMs!=60000||request.url.isEmpty()||series.sourceUrl!=request.url.toString()
           ||!fresh(series.receivedAtMs,now,ReceiptAge)||series.candles.isEmpty()||series.candles.size()>200||!matches(series.asset,series.currency))return false;
        qint64 previous=0;
        for(const auto& candle:series.candles){double open=0,high=0,low=0,close=0;
            if(candle.startsAtMs<=previous||candle.startsAtMs%60000||candle.startsAtMs>std::numeric_limits<qint64>::max()-60000||candle.startsAtMs+60000>series.receivedAtMs
               ||!price(candle.open,&open)||!price(candle.high,&high)||!price(candle.low,&low)||!price(candle.close,&close)||high>1e100||low<1e-100||low>open||low>close||high<open||high<close)return false;
            static const QRegularExpression volume(QStringLiteral("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z"));bool quantityOk=false;const auto quantity=candle.volume.toDouble(&quantityOk);
            if(candle.volume.size()>512||!volume.match(candle.volume).hasMatch()||!quantityOk||!std::isfinite(quantity)||quantity<0||quantity>1e100)return false;
            const auto row=std::find_if(input.minutes.cbegin(),input.minutes.cend(),[&](const auto& m){return m.candle.startsAtMs==candle.startsAtMs;});
            if(row==input.minutes.cend()||row->candle.open!=candle.open||row->candle.high!=candle.high||row->candle.low!=candle.low||row->candle.close!=candle.close||row->candle.volume!=candle.volume)return false;
            previous=candle.startsAtMs;
        }
        if(input.minutes.isEmpty()||input.minutes.last().candle.startsAtMs!=series.candles.last().startsAtMs)return false;
        *asset=series.asset;*currency=series.currency;return true;
    }
    // Existing typed observation callers retain their original metadata seam.
    // Its independently supplied fields must reconstruct the exact identity;
    // no symbol suffix is used to invent a base or quote asset.
    const auto& item=input.primary.instrument;
    if(!item.verified||item.identity!=input.identity||item.symbol!=identity.code||!matches(item.baseAsset,item.quoteAsset)
       ||!fresh(item.evidence.receivedAtMs,now,300000)||item.evidence.source.isEmpty()||item.evidence.sourceUrl.isEmpty()
       ||(identity.source=="upbit"?item.venue!=CoinFlowVenue::UpbitSpot:item.venue!=CoinFlowVenue::BinanceSpot||item.status!="TRADING"))return false;
    *asset=item.baseAsset;*currency=item.quoteAsset;return true;
}
bool liquidationObservation(const ChartLiquidationObservation& value,qint64 now,double* distance){
    if(!value.verified||!fresh(value.receivedAtMs,now,HoldingAge)||!riskIdentity(value.identity)||value.currency.isEmpty()
       ||value.positionToken.isEmpty()||value.positionToken.size()>128||value.sourceUrl!="https://fapi.binance.com/fapi/v3/positionRisk"
       ||!QStringList{"LONG","SHORT","BOTH"}.contains(value.positionSide))return false;
    for(const auto c:value.positionToken)if(c.category()==QChar::Other_Control||c.category()==QChar::Other_Format)return false;
    double mark=0,liquidation=0;if(!price(value.markPrice,&mark)||!price(value.liquidationPrice,&liquidation))return false;
    *distance=std::abs(liquidation-mark)/mark*100;return std::isfinite(*distance);
}
QJsonObject ruleJson(const ChartAlertRule& r){return {{"listId",r.listId},{"enabled",r.enabled},{"pullback",r.pullback},{"results",r.results},{"officialNotices",r.officialNotices},
    {"liquidation",r.liquidation},{"intervalMs",r.intervalMs},{"cooldownMs",r.cooldownMs},{"minimumScore",r.minimumScore},{"liquidationDistancePercent",r.liquidationDistancePercent},{"whales",r.whales},{"horizontal",r.horizontal}};}
bool readRule(const QJsonObject& object,ChartAlertRule* result,int version){
    const bool keys=version==1?exactKeys(object,{"listId","enabled","pullback","results","officialNotices","liquidation","intervalMs","cooldownMs","minimumScore","liquidationDistancePercent"}):
        version==2?exactKeys(object,{"listId","enabled","pullback","results","officialNotices","liquidation","intervalMs","cooldownMs","minimumScore","liquidationDistancePercent","whales"}):
        exactKeys(object,{"listId","enabled","pullback","results","officialNotices","liquidation","intervalMs","cooldownMs","minimumScore","liquidationDistancePercent","whales","horizontal"});
    if(!keys||(version>=2&&!object.value("whales").isBool())||(version==3&&!object.value("horizontal").isBool())
       ||!object.value("listId").isString()||!object.value("liquidationDistancePercent").isDouble())return false;
    for(const auto* key:{"enabled","pullback","results","officialNotices","liquidation"})if(!object.value(key).isBool())return false;
    qint64 interval=0,cooldown=0,score=0;if(!integer(object.value("intervalMs"),60000,Day,&interval)||!integer(object.value("cooldownMs"),0,Day,&cooldown)||!integer(object.value("minimumScore"),0,10,&score))return false;
    ChartAlertRule r;r.listId=object.value("listId").toString();r.enabled=object.value("enabled").toBool();r.pullback=object.value("pullback").toBool();r.results=object.value("results").toBool();
    r.officialNotices=object.value("officialNotices").toBool();r.liquidation=object.value("liquidation").toBool();r.intervalMs=interval;r.cooldownMs=cooldown;r.minimumScore=int(score);
    r.whales=version>=2&&object.value("whales").toBool();r.horizontal=version==3&&object.value("horizontal").toBool();
    r.liquidationDistancePercent=object.value("liquidationDistancePercent").toDouble();if(!ruleValid(r))return false;*result=r;return true;
}
QJsonObject settingsJson(const ChartAlertSettings& s){return {{"quietEnabled",s.quietEnabled},{"dripEnabled",s.dripEnabled},{"quietStartMinute",s.quietStartMinute},{"quietEndMinute",s.quietEndMinute},
    {"dailyLimit",s.dailyLimit},{"timeZoneId",QString::fromUtf8(s.timeZoneId)},{"mutedUntilMs",s.mutedUntilMs}};}
bool readSettings(const QJsonObject& object,ChartAlertSettings* result){
    if(!exactKeys(object,{"quietEnabled","dripEnabled","quietStartMinute","quietEndMinute","dailyLimit","timeZoneId","mutedUntilMs"})
       ||!object.value("quietEnabled").isBool()||!object.value("dripEnabled").isBool()||!object.value("timeZoneId").isString())return false;
    qint64 start=0,end=0,cap=0,mute=0;
    if(!integer(object.value("quietStartMinute"),0,1439,&start)||!integer(object.value("quietEndMinute"),0,1439,&end)||!integer(object.value("dailyLimit"),1,1000,&cap)
       ||!integer(object.value("mutedUntilMs"),0,std::numeric_limits<qint64>::max(),&mute))return false;
    ChartAlertSettings s;s.quietEnabled=object.value("quietEnabled").toBool();s.dripEnabled=object.value("dripEnabled").toBool();s.quietStartMinute=int(start);s.quietEndMinute=int(end);
    s.dailyLimit=int(cap);s.timeZoneId=object.value("timeZoneId").toString().toUtf8();s.mutedUntilMs=mute;if(!settingsValid(s))return false;*result=s;return true;
}
}
struct ChartAlertEngine::Private {
    ChartAlertEngine* q;QPointer<SystemNotificationSink> sink;QPointer<ChartPhrasePack> pack,plain;
    Clock clock;ChartAlertSettings settings;QMap<QString,ChartAlertRule> rules;QMap<QString,QList<WatchlistIdentity>> lists;
    QList<WatchlistIdentity> holdings;qint64 holdingsReceived=0;bool privateEnabled=false;quint64 epoch=0;
    struct Lane {WatchlistIdentity identity;qint64 interval=0,watermark=0;quint64 generation=0;bool realtime=false,seeded=false; qint64 flowWatermark=0;bool flowSeeded=false;};QMap<QString,Lane> lanes;
    struct Line {ChartHorizontalLineRule rule;MarketCandle prior;qint64 completed=0;quint64 generation=0;bool seeded=false,inside=false;};QMap<QString,Line> lines;
    struct Count {int accepted=0,reserved=0;};QMap<QDate,Count> counts;
    struct Pending {
        SystemNotice notice;QString eventKey,cooldownKey,listId,phraseKey,phraseId;WatchlistIdentity identity;
        ChartAlertKind kind=ChartAlertKind::PullbackEntry;ChartPhraseKind phraseKind=ChartPhraseKind::Pullback;ChartPhraseFacts facts;QString riskToken,lineId,lineRevision;
        qint64 interval=0,received=0,ageLimit=ReceiptAge,retryAt=0;QDate day;int attempts=0;bool reserved=false,inFlight=false;
        MarketFlowMetric marketMetric=MarketFlowMetric::KoreaGap;QString marketAsset;int marketState=-1;qint64 marketSourceTime=0,marketOldestSourceTime=0;
    };
    QMap<QString,Pending> pending;QSet<QString> privateIds,publicConsumed,privateConsumed;QQueue<QString> publicOrder,privateOrder;
    QMap<QString,qint64> cooldowns;QMap<QString,QString> lastPhrase;
    struct MarketLane {QString asset,text;int state=-1;bool seeded=false;qint64 sourceTime=0,received=0,oldestSourceTime=0;};
    QMap<MarketFlowMetric,bool> marketEnabled;QMap<MarketFlowMetric,MarketLane> marketLanes;QString marketAsset;
    QMap<QString,bool> observationEnabled;
    QMap<QString,qint64> observationEnabledAt;
    struct DeferredReversal {WatchlistIdentity identity;QString condition;qint64 at=0,received=0;ChartPhraseFacts facts;MovementDirection direction=MovementDirection::Unknown;quint64 generation=0;bool privateData=false;};
    QMap<QString,DeferredReversal> deferredReversals;
    QMap<QString,qint64> resolvedReversals;
    struct Risk {WatchlistIdentity identity;qint64 received=0;bool inside=false;quint64 sequence=0,epoch=0;double distance=0;};QMap<QString,Risk> risks;
    QTimer* timer=nullptr;
    explicit Private(ChartAlertEngine* owner,SystemNotificationSink* delivery,Clock value):q(owner),sink(delivery),clock(std::move(value)){
        if(!clock)clock=[](){return QDateTime::currentMSecsSinceEpoch();};settings.timeZoneId=QTimeZone::systemTimeZoneId();rules.insert(Holdings,defaultRule(Holdings));
        plain=new ChartPhrasePack(q);timer=new QTimer(q);timer->setInterval(1000);
    }
    qint64 now() const{return clock();}
    bool holdingsFresh() const{return privateEnabled&&fresh(holdingsReceived,now(),HoldingAge);}
    bool enabledFor(const ChartAlertRule& rule,ChartAlertKind kind) const{
        if(kind==ChartAlertKind::MarketMetric||kind==ChartAlertKind::PublicStudy)return false;
        return rule.enabled&&(kind==ChartAlertKind::PullbackEntry?rule.pullback:kind==ChartAlertKind::StudyResult?rule.results:kind==ChartAlertKind::OfficialNotice?rule.officialNotices:kind==ChartAlertKind::Whale?rule.whales:kind==ChartAlertKind::HorizontalLine?rule.horizontal:rule.liquidation);
    }
    bool member(const QString& id,const WatchlistIdentity& identity) const{return id==Holdings?holdingsFresh()&&contains(holdings,identity):contains(lists.value(id),identity);}
    bool lineActive(const ChartHorizontalLineRule& line)const{return line.enabled&&line.listId!=Holdings&&lists.contains(line.listId)&&member(line.listId,line.identity)&&enabledFor(rules.value(line.listId),ChartAlertKind::HorizontalLine);}
    bool hasLine(const WatchlistIdentity& identity,qint64 interval)const{for(const auto& line:lines)if(line.rule.identity==identity&&line.rule.intervalMs==interval&&lineActive(line.rule))return true;return false;}
    void resetLines(const WatchlistIdentity& identity,qint64 interval){
        QSet<QString> reset;for(auto it=lines.begin();it!=lines.end();++it)if(it->rule.identity==identity&&it->rule.intervalMs==interval){it->seeded=false;it->completed=0;reset.insert(it.key());}
        QStringList remove;for(auto it=pending.cbegin();it!=pending.cend();++it)if(it->kind==ChartAlertKind::HorizontalLine&&reset.contains(it->lineId))remove.append(it.key());for(const auto& id:remove)removePending(id);
    }
    QString eligible(const WatchlistIdentity& identity,qint64 interval,ChartAlertKind kind,int score=10) const{
        if(kind==ChartAlertKind::Whale&&!flowIdentity(identity))return {};
        if(kind==ChartAlertKind::HorizontalLine){for(const auto& line:lines)if(line.rule.identity==identity&&line.rule.intervalMs==interval&&lineActive(line.rule))return line.rule.listId;return {};}
        if(kind==ChartAlertKind::LiquidationDistance){const auto rule=rules.value(Holdings);if(!privateEnabled||rule.intervalMs!=interval||!enabledFor(rule,kind))return {};
            for(const auto& risk:risks)if(risk.identity==identity&&risk.epoch==epoch&&risk.inside&&fresh(risk.received,now(),HoldingAge))return Holdings;return {};
        }
        for(auto it=rules.cbegin();it!=rules.cend();++it)if(it.key()!=Holdings&&ruleInterval(*it,kind,interval)&&enabledFor(*it,kind)&&member(it.key(),identity)
            &&(kind!=ChartAlertKind::PullbackEntry||score>=it->minimumScore))return it.key();
        const auto rule=rules.value(Holdings);if(ruleInterval(rule,kind,interval)&&enabledFor(rule,kind)&&member(Holdings,identity)
            &&(kind!=ChartAlertKind::PullbackEntry||score>=rule.minimumScore))return Holdings;return {};
    }
    QList<ChartAlertDemand> demandList() const{
        QMap<QString,ChartAlertDemand> merged;
        const auto add=[&](const WatchlistIdentity& id,qint64 interval,const ChartAlertRule& rule,bool priv,bool flow){
            const auto key=laneKey(id,interval);auto found=merged.find(key);
            if(found==merged.end()){ChartAlertDemand demand;demand.identity=id;demand.intervalMs=interval;demand.privateOnly=priv;found=merged.insert(key,demand);}
            if(flow)found->whales=true;else{found->pullback=found->pullback||rule.pullback;found->results=found->results||rule.results;}
            if(!priv)found->privateOnly=false;
        };
        for(auto it=rules.cbegin();it!=rules.cend();++it){const auto& rule=*it;if(!rule.enabled||(!rule.pullback&&!rule.results&&!rule.whales))continue;
            const auto identities=it.key()==Holdings?(holdingsFresh()?holdings:QList<WatchlistIdentity>{}):lists.value(it.key());
            for(const auto& id:identities)if(supported(id)){
                if(rule.pullback||rule.results)add(id,rule.intervalMs,rule,it.key()==Holdings,false);
                if(rule.whales&&flowIdentity(id))add(id,60000,rule,it.key()==Holdings,true);
            }
        }
        for(const auto& line:lines)if(lineActive(line.rule)){
            const auto key=laneKey(line.rule.identity,line.rule.intervalMs);auto found=merged.find(key);
            if(found==merged.end()){ChartAlertDemand value;value.identity=line.rule.identity;value.intervalMs=line.rule.intervalMs;found=merged.insert(key,value);}found->horizontal=true;found->privateOnly=false;
        }
        return merged.values();
    }
    bool publicLane(const WatchlistIdentity& identity,qint64 interval) const{
        if(hasLine(identity,interval))return true;
        for(auto it=rules.cbegin();it!=rules.cend();++it)if(it.key()!=Holdings&&it->enabled&&member(it.key(),identity)&&
            ((it->intervalMs==interval&&(it->pullback||it->results))||(it->whales&&flowIdentity(identity)&&interval==60000)))return true;return false;
    }
    bool demanded(const WatchlistIdentity& identity,qint64 interval) const{
        return hasLine(identity,interval)||!eligible(identity,interval,ChartAlertKind::PullbackEntry).isEmpty()||!eligible(identity,interval,ChartAlertKind::StudyResult).isEmpty()||!eligible(identity,interval,ChartAlertKind::Whale).isEmpty();
    }
    QDate date(qint64 at) const{return QDateTime::fromMSecsSinceEpoch(at,QTimeZone(settings.timeZoneId)).date();}
    bool quiet(qint64 at) const{
        if(!settings.quietEnabled)return false;const auto time=QDateTime::fromMSecsSinceEpoch(at,QTimeZone(settings.timeZoneId)).time();const int minute=time.hour()*60+time.minute();
        const int start=settings.quietStartMinute,end=settings.quietEndMinute;
        return start==end||(start<end?(minute>=start&&minute<end):(minute>=start||minute<end));
    }
    bool suppressed(qint64 at) const{return at<settings.mutedUntilMs||quiet(at)||!sink||sink->permission()!=SystemNoticePermission::Granted;}
    void remember(const QString& key,bool privateData){
        auto& set=privateData?privateConsumed:publicConsumed;auto& order=privateData?privateOrder:publicOrder;
        if(set.contains(key))return;set.insert(key);order.enqueue(key);while(order.size()>4096)set.remove(order.dequeue());
    }
    void release(Pending& item){if(item.reserved){auto& count=counts[item.day];count.reserved=std::max(0,count.reserved-1);item.reserved=false;}}
    void removePending(const QString& id,bool removeNative=true){auto it=pending.find(id);if(it==pending.end())return;release(*it);pending.erase(it);if(removeNative&&sink)sink->remove({id});}
    bool authorized(const Pending& item) const{
        if(item.kind==ChartAlertKind::PublicStudy)return observationEnabled.value(item.marketAsset)&&
            !item.notice.privateData&&item.notice.privateEpoch==0&&item.marketSourceTime>observationEnabledAt.value(item.marketAsset)&&
            fresh(item.marketSourceTime,now(),ReceiptAge)&&fresh(item.received,now(),ReceiptAge);
        if(item.kind==ChartAlertKind::MarketMetric){const auto lane=marketLanes.constFind(item.marketMetric);const auto at=now();
            return marketEnabled.value(item.marketMetric)&&!item.notice.privateData&&item.notice.privateEpoch==0&&lane!=marketLanes.cend()&&lane->seeded&&
                lane->asset==item.marketAsset&&lane->state==item.marketState&&fresh(lane->sourceTime,at,MarketMetrics::FreshnessMs)&&
                fresh(lane->received,at,MarketMetrics::FreshnessMs)&&fresh(lane->oldestSourceTime,at,MarketMetrics::FreshnessMs)&&
                fresh(item.marketSourceTime,at,MarketMetrics::FreshnessMs)&&fresh(item.marketOldestSourceTime,at,MarketMetrics::FreshnessMs);
        }
        const auto rule=rules.value(item.listId);
        if(!enabledFor(rule,item.kind)||!ruleInterval(rule,item.kind,item.interval))return false;
        if(item.kind==ChartAlertKind::HorizontalLine){const auto line=lines.constFind(item.lineId);const auto lane=lanes.constFind(laneKey(item.identity,item.interval));return !item.notice.privateData&&line!=lines.cend()&&line->seeded&&line->inside&&lane!=lanes.cend()&&lane->realtime&&line->generation==lane->generation&&line->completed==item.facts.observedAtMs&&lineActive(line->rule)&&line->rule.revision==item.lineRevision&&line->rule.listId==item.listId&&line->rule.identity==item.identity&&line->rule.intervalMs==item.interval&&item.facts.priceText&&*item.facts.priceText==line->rule.price&&item.facts.currency==line->rule.currency;}
        if(item.kind==ChartAlertKind::LiquidationDistance){const auto risk=risks.constFind(item.riskToken);
            return privateEnabled&&item.notice.privateEpoch==epoch&&risk!=risks.cend()&&risk->epoch==epoch&&risk->identity==item.identity&&risk->inside
                &&risk->distance<=rule.liquidationDistancePercent&&fresh(risk->received,now(),HoldingAge);
        }
        return member(item.listId,item.identity)&&(item.kind!=ChartAlertKind::PullbackEntry||item.facts.score>=rule.minimumScore);
    }
    void reconcile(){
        for(auto& line:lines)if(!lineActive(line.rule)){line.seeded=false;line.completed=0;}
        for(auto it=lanes.begin();it!=lanes.end();)if(!demanded(it->identity,it->interval))it=lanes.erase(it);else ++it;
        QStringList remove;for(auto it=pending.cbegin();it!=pending.cend();++it)if(!authorized(*it))remove.append(it.key());
        for(const auto& id:remove)removePending(id);
    }
    void clearHoldings(){holdings.clear();holdingsReceived=0;reconcile();}
    void purgePrivate(bool disable){
        if(disable)privateEnabled=false;if(++epoch==0)++epoch;if(sink)sink->setPrivateContext(epoch,privateEnabled);
        QStringList remove;for(auto it=pending.cbegin();it!=pending.cend();++it)if(it->notice.privateData)remove.append(it.key());
        for(const auto& id:remove)removePending(id,false);
        const auto ids=privateIds.values();privateIds.clear();if(sink&&!ids.isEmpty())sink->remove(ids);
        holdings.clear();holdingsReceived=0;risks.clear();privateConsumed.clear();privateOrder.clear();
        for(auto it=deferredReversals.begin();it!=deferredReversals.end();)if(it->privateData)it=deferredReversals.erase(it);else ++it;
        resolvedReversals.clear();
        for(auto it=lanes.begin();it!=lanes.end();)if(!publicLane(it->identity,it->interval))it=lanes.erase(it);else ++it;
        for(auto it=cooldowns.begin();it!=cooldowns.end();)if(it.key().startsWith("private|"))it=cooldowns.erase(it);else ++it;
        for(auto it=lastPhrase.begin();it!=lastPhrase.end();)if(it.key().startsWith("private|"))it=lastPhrase.erase(it);else ++it;
    }
    void submit(const QString& id){
        auto it=pending.find(id);if(it==pending.end())return;const auto at=now();
        if(it->notice.privateData&&(!privateEnabled||it->notice.privateEpoch!=epoch)){removePending(id);return;}
        if(suppressed(at)||!fresh(it->received,at,it->ageLimit)||!authorized(*it)){removePending(id);return;}
        if(it->attempts>0&&it->kind!=ChartAlertKind::MarketMetric&&it->kind!=ChartAlertKind::PublicStudy){
            auto* renderer=pack?pack.data():plain.data();if(!renderer){removePending(id);return;}
            const auto text=renderer->render(it->phraseKind,it->facts,it->kind==ChartAlertKind::LiquidationDistance?false:settings.dripEnabled,lastPhrase.value(it->phraseKey));
            if(!text.available||text.title.isEmpty()||text.body.isEmpty()||(it->kind==ChartAlertKind::LiquidationDistance&&!text.serious)){removePending(id);return;}
            it->notice.title=movementTitle(text.title,it->notice.direction);it->notice.body=text.body;it->phraseId=text.phraseId;
        }
        const auto day=date(at);auto& count=counts[day];if(count.accepted+count.reserved>=settings.dailyLimit){removePending(id);return;}
        ++count.reserved;it->reserved=true;it->day=day;it->inFlight=true;it->retryAt=0;++it->attempts;
        const auto notice=it->notice;if(notice.privateData)privateIds.insert(id);if(sink)sink->post(notice);
        if(notice.privateData&&(!privateEnabled||notice.privateEpoch!=epoch)&&sink)sink->remove({id});
    }
    void finished(const QString& id,quint64 receivedEpoch,bool accepted){
        if(id.startsWith("keefi.g8.private.")&&(!privateEnabled||receivedEpoch!=epoch)){if(sink)sink->remove({id});return;}
        auto it=pending.find(id);if(it==pending.end()||!it->inFlight||it->notice.privateEpoch!=receivedEpoch)return;
        release(*it);it->inFlight=false;
        if(!accepted){if(it->attempts<2){it->retryAt=now()+RetryDelay;return;}pending.erase(it);return;}
        ++counts[it->day].accepted;if(it->kind!=ChartAlertKind::MarketMetric&&it->kind!=ChartAlertKind::PublicStudy)lastPhrase.insert(it->phraseKey,it->phraseId);const auto notice=it->notice;pending.erase(it);
        if(!notice.privateData||(privateEnabled&&notice.privateEpoch==epoch))emit q->noticeSubmitted(notice.identifier,notice.title,notice.body,notice.privateData,notice.direction,notice.fixedRisk);
    }
    void candidate(const WatchlistIdentity& identity,qint64 interval,ChartAlertKind kind,const QString& condition,qint64 eventAt,qint64 received,
                   const ChartPhraseFacts& facts,ChartPhraseKind phraseKind,int score=10,qint64 ageLimit=ReceiptAge,const QString& riskToken={},const QString& ownerList={},const QString& lineId={},const QString& lineRevision={},MovementDirection direction=MovementDirection::Unknown){
        const auto list=kind==ChartAlertKind::HorizontalLine?ownerList:eligible(identity,interval,kind,score);if(list.isEmpty())return;
        if(kind==ChartAlertKind::HorizontalLine){const auto line=lines.constFind(lineId);if(line==lines.cend()||!lineActive(line->rule)||line->rule.listId!=list||line->rule.identity!=identity||line->rule.intervalMs!=interval||line->rule.revision!=lineRevision)return;}
        const bool priv=list==Holdings;const auto event=laneKey(identity,interval)+"|"+kindKey(kind)+"|"+QString::number(eventAt)+"|"+condition;
        if(publicConsumed.contains(event)||privateConsumed.contains(event))return;remember(event,priv);
        const auto at=now();if(!fresh(eventAt,at,ageLimit)||!fresh(received,at,ageLimit)||suppressed(at))return;
        const auto scope=priv?QStringLiteral("private|"):QStringLiteral("public|");const auto cooldown=scope+laneKey(identity,interval)+"|"+kindKey(kind)+(kind==ChartAlertKind::HorizontalLine?"|"+lineId:QString{});
        const auto rule=rules.value(list);const auto prior=cooldowns.constFind(cooldown);
        if(prior!=cooldowns.cend()&&(at<*prior||at-*prior<rule.cooldownMs))return;
        auto* renderer=pack?pack.data():plain.data();if(!renderer)return;const auto phraseKey=scope+kindKey(kind);
        const auto text=renderer->render(phraseKind,facts,kind==ChartAlertKind::LiquidationDistance?false:settings.dripEnabled,lastPhrase.value(phraseKey));
        if(!text.available||text.title.isEmpty()||text.body.isEmpty()||(kind==ChartAlertKind::LiquidationDistance&&!text.serious))return;
        const auto day=date(at);const auto count=counts.value(day);if(count.accepted+count.reserved>=settings.dailyLimit)return;
        const auto id=QStringLiteral("keefi.g8.")+(priv?QStringLiteral("private."):QStringLiteral("public."))+QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(kind==ChartAlertKind::StudyResult&&facts.resultReturnPercent&&std::isfinite(*facts.resultReturnPercent))direction=*facts.resultReturnPercent>0?MovementDirection::Up:*facts.resultReturnPercent<0?MovementDirection::Down:MovementDirection::Flat;
        Pending item;item.notice={id,movementTitle(text.title,direction),text.body,priv,priv?epoch:0,direction,kind==ChartAlertKind::LiquidationDistance};item.notice.playSound=kind==ChartAlertKind::LiquidationDistance;item.eventKey=event;item.cooldownKey=cooldown;item.listId=list;item.phraseKey=phraseKey;item.phraseId=text.phraseId;
        item.identity=identity;item.interval=interval;item.received=received;item.ageLimit=ageLimit;item.kind=kind;item.phraseKind=phraseKind;item.facts=facts;item.riskToken=riskToken;item.lineId=lineId;item.lineRevision=lineRevision;pending.insert(id,item);cooldowns.insert(cooldown,at);submit(id);
    }
    void clearMarketPending(MarketFlowMetric metric){
        QStringList remove;for(auto it=pending.cbegin();it!=pending.cend();++it)if(it->kind==ChartAlertKind::MarketMetric&&it->marketMetric==metric)remove.append(it.key());
        for(const auto& id:remove)removePending(id);
    }
    void resetMarket(MarketFlowMetric metric,bool forget=false){
        if(forget)marketLanes.remove(metric);else{auto it=marketLanes.find(metric);if(it!=marketLanes.end())it->seeded=false;}
        clearMarketPending(metric);
    }
    void observeMarket(const QString& asset,MarketFlowMetric metric,int state,const QString& text,qint64 sourceTime,qint64 received,qint64 oldestSourceTime=0){
        const auto at=now();if(!marketEnabled.value(metric))return;if(!oldestSourceTime)oldestSourceTime=sourceTime;
        if(marketAssetValid(asset)&&marketAsset!=asset){
            marketAsset=asset;const auto metrics=marketLanes.keys();for(auto prior:metrics)resetMarket(prior,true);
        }
        if(!marketAssetValid(asset)||state<0||text.isEmpty()||oldestSourceTime>sourceTime||sourceTime>received||!fresh(oldestSourceTime,at,MarketMetrics::FreshnessMs)||!fresh(sourceTime,at,MarketMetrics::FreshnessMs)||!fresh(received,at,MarketMetrics::FreshnessMs)){resetMarket(metric);return;}
        auto previous=marketLanes.value(metric);
        if(previous.asset!=asset){resetMarket(metric,true);previous={};previous.asset=asset;}
        if(sourceTime<previous.sourceTime||received<previous.received||oldestSourceTime<previous.oldestSourceTime)return;
        if(sourceTime==previous.sourceTime&&received==previous.received){if(previous.seeded&&previous.state!=state)resetMarket(metric);return;}
        const bool comparable=previous.seeded&&fresh(previous.sourceTime,at,MarketMetrics::FreshnessMs)&&fresh(previous.received,at,MarketMetrics::FreshnessMs)&&fresh(previous.oldestSourceTime,at,MarketMetrics::FreshnessMs);
        marketLanes.insert(metric,{asset,text,state,true,sourceTime,received,oldestSourceTime});
        if(!comparable){clearMarketPending(metric);return;}if(previous.state==state)return;
        clearMarketPending(metric);
        const auto event=QString("market|%1|%2|%3|%4|%5").arg(int(metric)).arg(asset).arg(sourceTime).arg(received).arg(state);
        if(publicConsumed.contains(event))return;remember(event,false);if(suppressed(at))return;
        const auto count=counts.value(date(at));if(count.accepted+count.reserved>=settings.dailyLimit)return;
        const auto metricLabel=marketFlowMetricLabel(metric);
        const auto title=usabilityText(UsabilityText::FlowMetricAlertTitle).arg(asset,metricLabel);
        const auto body=usabilityText(UsabilityText::FlowMetricAlertBody).arg(previous.text,text);
        if(metricLabel.isEmpty()||title.isEmpty()||body.isEmpty())return;
        const auto id=QStringLiteral("keefi.g8.public.")+QUuid::createUuid().toString(QUuid::WithoutBraces);
        Pending item;item.notice={id,title,body,false,0,MovementDirection::Unknown,false};item.kind=ChartAlertKind::MarketMetric;
        item.marketMetric=metric;item.marketAsset=asset;item.marketState=state;item.marketSourceTime=sourceTime;item.marketOldestSourceTime=oldestSourceTime;item.received=received;item.ageLimit=MarketMetrics::FreshnessMs;
        pending.insert(id,item);submit(id);
    }
    void observeLines(const MarketCandleSeries& series,quint64 generation,const ChartStudyTimeline* timeline=nullptr){
        const auto identity=WatchlistWorkspaceList::identityFromKey(series.identity);if(!hasLine(identity,series.intervalMs))return;
        if(!timeline&&!coinLineSeries(series,now())){resetLines(identity,series.intervalMs);return;}
        QList<qint64> ends;QList<bool> eligible,connected;
        qint64 previous=0;for(qsizetype i=0;i<series.candles.size();++i){const auto& candle=series.candles.at(i);qint64 end=0;bool allowed=true,link=false;
            if(timeline){const auto& row=timeline->rows.at(i);end=row.completesAtMs;allowed=row.eligible&&row.exclusions.isEmpty()&&row.session!=StockSessionKind::Unknown&&row.session!=StockSessionKind::Mixed&&row.tradingDate.isValid();link=row.connectsPrevious;
                if(row.startsAtMs!=candle.startsAtMs||!lineCandle(candle,series.receivedAtMs)||candle.startsAtMs<=0||end<=candle.startsAtMs||end<=previous||end>series.receivedAtMs||end>now()){resetLines(identity,series.intervalMs);return;}}
            else{end=candle.startsAtMs+series.intervalMs;link=i>0&&candle.startsAtMs==series.candles.at(i-1).startsAtMs+series.intervalMs;}
            ends.append(end);eligible.append(allowed);connected.append(link);previous=end;
        }
        if(ends.isEmpty())return;const auto latest=ends.last();const auto candle=series.candles.last();const auto ids=lines.keys();
        for(const auto& id:ids){auto current=lines.find(id);if(current==lines.end()||current->rule.identity!=identity||current->rule.intervalMs!=series.intervalMs||!lineActive(current->rule))continue;
            if(current->rule.currency!=series.currency){current->seeded=false;current->completed=0;continue;}
            const auto rule=current->rule;const bool inside=lineDecimalCompare(candle.low,rule.price)<=0&&lineDecimalCompare(rule.price,candle.high)<=0;
            const auto seed=[&](){current->seeded=eligible.last();current->inside=inside;current->completed=latest;current->prior=candle;current->generation=generation;};
            if(!current->seeded||current->generation!=generation){seed();continue;}if(latest<current->completed)continue;
            if(latest==current->completed){if(!sameLineCandle(current->prior,candle)){current->seeded=false;current->completed=0;}continue;}
            const auto priorIndex=ends.indexOf(current->completed);const bool continuous=priorIndex>=0&&priorIndex==ends.size()-2&&eligible.at(priorIndex)&&eligible.last()&&connected.last()&&sameLineCandle(current->prior,series.candles.at(priorIndex));
            const bool entering=continuous&&!current->inside&&inside;seed();if(!entering)continue;
            ChartPhraseFacts facts;facts.symbol=series.asset;facts.source=series.sourceUrl;facts.currency=series.currency;facts.intervalMs=series.intervalMs;facts.observedAtMs=latest;facts.samples=2;facts.priceText=rule.price;
            candidate(identity,series.intervalMs,ChartAlertKind::HorizontalLine,id+"|"+rule.revision,latest,series.receivedAtMs,facts,ChartPhraseKind::Line,10,ReceiptAge,{},rule.listId,id,rule.revision);
        }
    }
    bool recordRisk(const ChartLiquidationObservation& value,quint64 receivedEpoch,quint64* sequence){
        double distance=0;if(!privateEnabled||receivedEpoch!=epoch||!liquidationObservation(value,now(),&distance))return false;
        const auto rule=rules.value(Holdings);if(!enabledFor(rule,ChartAlertKind::LiquidationDistance))return false;
        auto& risk=risks[value.positionToken];if(risk.received>value.receivedAtMs)return false;
        const bool inside=distance<=rule.liquidationDistancePercent,entering=inside&&(!risk.inside||risk.identity!=value.identity);
        risk.identity=value.identity;risk.received=value.receivedAtMs;risk.inside=inside;risk.epoch=receivedEpoch;risk.distance=distance;
        if(entering)++risk.sequence;*sequence=risk.sequence;return entering;
    }
    void submitRisk(const ChartLiquidationObservation& value,quint64 receivedEpoch,quint64 sequence){
        const auto risk=risks.constFind(value.positionToken);
        if(!privateEnabled||receivedEpoch!=epoch||risk==risks.cend()||risk->sequence!=sequence||risk->identity!=value.identity||!risk->inside)return;
        const auto rule=rules.value(Holdings);ChartPhraseFacts facts;
        facts.symbol=value.identity.code;facts.source=value.sourceUrl;facts.currency=value.currency;facts.intervalMs=rule.intervalMs;facts.observedAtMs=value.receivedAtMs;
        facts.positionSide=value.positionSide;facts.distancePercent=risk->distance;facts.price=value.markPrice.toDouble();
        candidate(value.identity,rule.intervalMs,ChartAlertKind::LiquidationDistance,value.positionToken+"|"+QString::number(sequence),value.receivedAtMs,value.receivedAtMs,facts,ChartPhraseKind::Liquidation,10,HoldingAge,value.positionToken);
    }
};
QStringList ChartAlertEngine::observationKinds(){return {"ma","bb","rsi","macd","breakout","rapid_move","strong_reversal","liquidation_feed"};}
QString ChartAlertEngine::observationLabel(const QString& kind){
    static const QMap<QString,QString> labels{{"ma",QStringLiteral("이동평균")},{"bb",QStringLiteral("볼린저 밴드")},
        {"rsi",QStringLiteral("RSI")},{"macd",QStringLiteral("MACD")},{"breakout",QStringLiteral("돌파")},
        {"rapid_move",QStringLiteral("급변")},{"strong_reversal",QStringLiteral("강한 반전")},{"liquidation_feed",QStringLiteral("청산 관측")}};
    return labels.value(kind);
}
bool ChartAlertEngine::setObservationEnabled(const QString& kind,bool enabled){
    if(!observationKinds().contains(kind))return false;
    if(d_->observationEnabled.value(kind)==enabled)return true;
    d_->observationEnabled[kind]=enabled;d_->observationEnabledAt[kind]=d_->now();
    if(kind=="strong_reversal"){d_->deferredReversals.clear();d_->resolvedReversals.clear();}
    d_->reconcile();emit changed();return true;
}
bool ChartAlertEngine::observationEnabled(const QString& kind)const{return d_->observationEnabled.value(kind);}
QByteArray ChartAlertEngine::observationSettingsState()const{
    QJsonObject flags;for(const auto& kind:observationKinds())flags.insert(kind,observationEnabled(kind));
    return QJsonDocument(QJsonObject{{"format","keefi-chart-observations/1"},{"enabled",flags}}).toJson(QJsonDocument::Compact);
}
bool ChartAlertEngine::restoreObservationSettingsState(const QByteArray& bytes){
    if(bytes.size()>8192)return false;QJsonParseError error;const auto document=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())return false;const auto object=document.object();
    if(object.size()!=2||canonicalPublicFormat(object.value("format").toString())!="keefi-chart-observations/1"||!object.value("enabled").isObject())return false;
    const auto flags=object.value("enabled").toObject();const auto kinds=observationKinds();if(flags.size()!=kinds.size())return false;
    for(const auto& kind:kinds)if(!flags.value(kind).isBool())return false;
    for(const auto& kind:kinds){d_->observationEnabled[kind]=flags.value(kind).toBool();d_->observationEnabledAt[kind]=d_->now();}
    d_->deferredReversals.clear();d_->resolvedReversals.clear();d_->reconcile();emit changed();return true;
}
void ChartAlertEngine::observePublicStudy(const ChartPublicStudyEvent& value){
    const auto at=d_->now();const auto label=observationLabel(value.kind);
    if(label.isEmpty()||!observationEnabled(value.kind)||!value.liveEligible||!supported(value.identity)||
        value.eventId.isEmpty()||value.eventId.size()>256||value.detail.isEmpty()||value.detail.size()>512||
        value.atMs<=d_->observationEnabledAt.value(value.kind)||value.receivedAtMs<value.atMs||
        !fresh(value.atMs,at,ReceiptAge)||!fresh(value.receivedAtMs,at,ReceiptAge))return;
    const auto event=QString("study|%1|%2|%3|%4").arg(value.kind,value.identity.key(),value.eventId).arg(value.atMs);
    if(d_->publicConsumed.contains(event))return;d_->remember(event,false);
    const auto cooldown=QString("public|study|%1|%2|%3").arg(value.kind,value.identity.key()).arg(int(value.direction));
    const qint64 duration=value.kind=="rapid_move"?600000:value.kind=="liquidation_feed"?60000:0;
    if((duration&&d_->cooldowns.contains(cooldown)&&at-d_->cooldowns[cooldown]<duration)||d_->suppressed(at))return;
    const auto count=d_->counts.value(d_->date(at));if(count.accepted+count.reserved>=d_->settings.dailyLimit)return;
    const auto id=QStringLiteral("keefi.g8.public.")+(value.kind=="strong_reversal"?QString::fromLatin1(QCryptographicHash::hash(event.toUtf8(),QCryptographicHash::Sha256).toHex()):QUuid::createUuid().toString(QUuid::WithoutBraces));
    Private::Pending item;item.kind=ChartAlertKind::PublicStudy;item.marketAsset=value.kind;item.marketSourceTime=value.atMs;
    item.received=value.receivedAtMs;item.eventKey=event;item.cooldownKey=cooldown;
    item.notice={id,movementTitle(value.identity.code+QStringLiteral(" · ")+label,value.direction),value.detail,false,0,value.direction,false};
    item.notice.playSound=value.kind=="strong_reversal"||value.kind=="liquidation_feed";
    d_->cooldowns.insert(cooldown,at);d_->pending.insert(id,item);d_->submit(id);
}
ChartAlertEngine::ChartAlertEngine(SystemNotificationSink* sink,QObject* parent,Clock clock):QObject(parent),d_(std::make_unique<Private>(this,sink,std::move(clock))){
    if(sink){sink->setPrivateContext(0,false);connect(sink,&SystemNotificationSink::submissionFinished,this,[this](const QString& id,quint64 epoch,bool accepted){d_->finished(id,epoch,accepted);});
        connect(sink,&SystemNotificationSink::permissionChanged,this,&ChartAlertEngine::changed);
        connect(sink,&SystemNotificationSink::actionRequested,this,[this](SystemNoticeAction action){if(action==SystemNoticeAction::DisableDrip)setDripEnabled(false);else if(action==SystemNoticeAction::OpenWindow)emit openWindowRequested();});}
    connect(d_->timer,&QTimer::timeout,this,&ChartAlertEngine::tick);d_->timer->start();
}
ChartAlertEngine::~ChartAlertEngine(){d_->timer->stop();d_->purgePrivate(true);if(d_->sink)disconnect(d_->sink,nullptr,this,nullptr);}
void ChartAlertEngine::setPhrasePack(ChartPhrasePack* pack){d_->pack=pack;}
bool ChartAlertEngine::setPublicLists(const QList<ChartAlertList>& lists,QString* error){
    const auto fail=[&](){if(error)*error=QStringLiteral("공개 리스트 설정 형식이 올바르지 않습니다");return false;};
    if(lists.size()>32)return fail();QMap<QString,QList<WatchlistIdentity>> next;int total=0;
    for(const auto& list:lists){if(!listIdValid(list.listId)||next.contains(list.listId)||list.identities.size()>2000)return fail();QSet<QString> seen;
        for(const auto& identity:list.identities){if(!WatchlistWorkspaceList::validIdentity(identity)||seen.contains(identity.key()))return fail();seen.insert(identity.key());}
        total+=int(list.identities.size());if(total>10000)return fail();next.insert(list.listId,list.identities);}
    d_->lists=next;for(auto it=d_->rules.begin();it!=d_->rules.end();)if(it.key()!=Holdings&&!next.contains(it.key()))it=d_->rules.erase(it);else ++it;
    for(auto it=next.cbegin();it!=next.cend();++it)if(!d_->rules.contains(it.key()))d_->rules.insert(it.key(),defaultRule(it.key()));d_->reconcile();emit changed();emit demandsChanged();return true;
}
bool ChartAlertEngine::setRule(const ChartAlertRule& rule,QString* error){
    if(!ruleValid(rule)||(rule.listId!=Holdings&&!d_->lists.contains(rule.listId))){if(error)*error=QStringLiteral("리스트 알림 규칙이 올바르지 않습니다");return false;}
    d_->rules.insert(rule.listId,rule);d_->reconcile();emit changed();emit demandsChanged();return true;
}
ChartAlertRule ChartAlertEngine::rule(const QString& id) const{return d_->rules.value(id,defaultRule(id));}
bool ChartAlertEngine::setHorizontalLines(const QList<ChartHorizontalLineRule>& lines,QString* error){
    const auto fail=[&](){if(error)*error=QStringLiteral("공개 수평선 알림 형식이 올바르지 않습니다");return false;};if(lines.size()>ChartDrawings::MaxTotal)return fail();
    static const QRegularExpression revision("\\A[0-9a-f]{64}\\z");QMap<QString,Private::Line> next;bool changed=false;
    const auto equal=[](const ChartHorizontalLineRule& a,const ChartHorizontalLineRule& b){return a.drawingId==b.drawingId&&a.listId==b.listId&&a.revision==b.revision&&a.identity==b.identity&&a.currency==b.currency&&a.price==b.price&&a.intervalMs==b.intervalMs&&a.enabled==b.enabled;};
    for(const auto& rule:lines){const auto uuid=QUuid(rule.drawingId);
        if(uuid.isNull()||uuid.toString(QUuid::WithoutBraces)!=rule.drawingId||next.contains(rule.drawingId)||!revision.match(rule.revision).hasMatch()||!supported(rule.identity)||!supported(rule.intervalMs)||!lineDecimal(rule.price)||rule.listId==Holdings||!d_->lists.contains(rule.listId)||!d_->member(rule.listId,rule.identity)||!binanceIdentifierValid(rule.currency))return fail();
        if(rule.identity.market=="stock"&&rule.currency!=(rule.identity.source=="KR"?"KRW":"USD"))return fail();
        if(rule.identity.source=="upbit"&&rule.currency!=rule.identity.code.section('-',0,0))return fail();
        const auto old=d_->lines.constFind(rule.drawingId);Private::Line value;
        if(old!=d_->lines.cend()&&equal(old->rule,rule))value=*old;else changed=true;value.rule=rule;next.insert(rule.drawingId,value);
    }
    changed=changed||next.size()!=d_->lines.size();if(!changed){if(error)error->clear();return true;}
    d_->lines=next;d_->reconcile();if(error)error->clear();emit this->changed();emit demandsChanged();return true;
}
bool ChartAlertEngine::setSettings(const ChartAlertSettings& settings,QString* error){
    if(!settingsValid(settings)){if(error)*error=QStringLiteral("시간대 또는 알림 제한 설정이 올바르지 않습니다");return false;}
    const bool disabled=d_->settings.dripEnabled&&!settings.dripEnabled;d_->settings=settings;emit changed();if(disabled)emit dripDisabled();return true;
}
ChartAlertSettings ChartAlertEngine::settings() const{return d_->settings;}
void ChartAlertEngine::muteForOneHour(){auto settings=d_->settings;settings.mutedUntilMs=d_->now()>std::numeric_limits<qint64>::max()-3600000?std::numeric_limits<qint64>::max():d_->now()+3600000;setSettings(settings);}
void ChartAlertEngine::muteForToday(){auto settings=d_->settings;const auto date=QDateTime::fromMSecsSinceEpoch(d_->now(),QTimeZone(settings.timeZoneId)).date();settings.mutedUntilMs=date.addDays(1).startOfDay(QTimeZone(settings.timeZoneId)).toMSecsSinceEpoch();setSettings(settings);}
void ChartAlertEngine::setDripEnabled(bool enabled){if(d_->settings.dripEnabled==enabled)return;auto settings=d_->settings;settings.dripEnabled=enabled;setSettings(settings);}
void ChartAlertEngine::setPrivateEnabled(bool enabled){
    if(d_->privateEnabled==enabled)return;if(!enabled){clearPrivate();return;}d_->privateEnabled=true;if(++d_->epoch==0)++d_->epoch;if(d_->sink)d_->sink->setPrivateContext(d_->epoch,true);emit changed();emit demandsChanged();
}
quint64 ChartAlertEngine::privateEpoch() const{return d_->epoch;}
void ChartAlertEngine::setHoldings(const QList<WatchlistIdentity>& identities,qint64 received,quint64 epoch){
    if(!d_->privateEnabled||epoch!=d_->epoch)return;bool valid=identities.size()<=10000&&fresh(received,d_->now(),HoldingAge);QSet<QString> seen;
    for(const auto& identity:identities){valid=valid&&WatchlistWorkspaceList::validIdentity(identity)&&!seen.contains(identity.key());seen.insert(identity.key());}
    if(!valid){d_->clearHoldings();emit changed();emit demandsChanged();return;}
    d_->holdings=identities;d_->holdingsReceived=received;d_->reconcile();emit changed();emit demandsChanged();
}
void ChartAlertEngine::clearPrivate(){d_->purgePrivate(true);emit changed();emit demandsChanged();}
QList<ChartAlertDemand> ChartAlertEngine::demands() const{return d_->demandList();}
void ChartAlertEngine::beginLane(const WatchlistIdentity& identity,qint64 interval,quint64 generation,bool realtimeAllowed){
    const auto key=laneKey(identity,interval);if(!supported(identity)||!supported(interval)||generation==0||!d_->demanded(identity,interval)){d_->lanes.remove(key);d_->resetLines(identity,interval);return;}
    const auto old=d_->lanes.constFind(key);if(old==d_->lanes.cend()||old->generation!=generation||old->realtime!=realtimeAllowed)d_->resetLines(identity,interval);
    d_->lanes.insert(key,{identity,interval,0,generation,realtimeAllowed,false});
}
void ChartAlertEngine::observe(const MarketCandleSeries& series,const ChartStudySet& studies,quint64 generation){
    const auto identity=WatchlistWorkspaceList::identityFromKey(series.identity);const auto key=laneKey(identity,series.intervalMs);const auto before=d_->lanes.constFind(key);
    if(before!=d_->lanes.cend()&&before->generation==generation&&before->realtime)d_->observeLines(series,generation);
    tick();auto lane=d_->lanes.find(key);
    if(lane==d_->lanes.end()||lane->generation!=generation||!lane->realtime||!d_->demanded(identity,series.intervalMs))return;
    const auto at=d_->now();const auto request=MarketCandles::request(series.identity,series.intervalMs);
    const auto invalid=[&](){lane->seeded=false;lane->watermark=0;};
    if(!series.available||!studies.available||!fresh(series.receivedAtMs,at,ReceiptAge)||series.asset.isEmpty()||series.currency.isEmpty()
       ||series.sourceUrl!=request.url.toString()||request.url.isEmpty()||series.candles.isEmpty()||studies.intervalMs!=series.intervalMs||studies.observations!=series.candles.size()
       ||studies.lastCompletedAtMs!=series.candles.last().startsAtMs+series.intervalMs){invalid();return;}
    const auto completed=studies.lastCompletedAtMs;
    if(completed>at){invalid();return;}
    if(!lane->seeded){lane->seeded=true;lane->watermark=completed;return;}
    const auto previous=lane->watermark;if(completed<=previous)return;
    qint64 expected=previous+series.intervalMs;bool continuous=false;
    for(const auto& candle:series.candles){const auto end=candle.startsAtMs+series.intervalMs;if(end<=previous)continue;if(end!=expected){continuous=false;break;}continuous=true;expected=end+series.intervalMs;}
    lane->watermark=completed;if(!continuous)return;
    for(const auto& entry:studies.entries)if(entry.atMs>previous&&entry.atMs<=completed&&entry.score>=0&&entry.score<=10&&entry.waveLowAtMs>0&&entry.waveLowAtMs<entry.atMs&&std::isfinite(entry.entryClose)&&entry.entryClose>0){
        ChartPhraseFacts facts;facts.symbol=series.asset;facts.source=series.sourceUrl;facts.currency=series.currency;facts.intervalMs=series.intervalMs;facts.observedAtMs=entry.atMs;facts.score=entry.score;facts.samples=studies.observations;facts.price=entry.entryClose;
        d_->candidate(identity,series.intervalMs,ChartAlertKind::PullbackEntry,QString::number(entry.waveLowAtMs),entry.atMs,series.receivedAtMs,facts,ChartPhraseKind::Pullback,entry.score);
    }
    if(studies.scorecard.available)for(const auto& outcome:studies.scorecard.outcomes)if(outcome.evaluationAtMs>previous&&outcome.evaluationAtMs<=completed&&outcome.signalAtMs>0
        &&outcome.evaluationAtMs-outcome.signalAtMs==studies.scorecard.horizonMs&&std::isfinite(outcome.returnPercent)){
        ChartPhraseFacts facts;facts.symbol=series.asset;facts.source=series.sourceUrl;facts.currency=series.currency;facts.intervalMs=series.intervalMs;facts.horizonMs=studies.scorecard.horizonMs;
        facts.observedAtMs=outcome.evaluationAtMs;facts.samples=studies.scorecard.samples;facts.resultReturnPercent=outcome.returnPercent;
        const auto kind=outcome.returnPercent>0?ChartPhraseKind::ResultUp:outcome.returnPercent<0?ChartPhraseKind::ResultDown:ChartPhraseKind::ResultFlat;
        d_->candidate(identity,series.intervalMs,ChartAlertKind::StudyResult,QString::number(outcome.signalAtMs),outcome.evaluationAtMs,series.receivedAtMs,facts,kind);
    }
}
void ChartAlertEngine::observeStock(const StockChartSnapshot& stock,quint64 generation){
    const auto& series=stock.series;const auto identity=WatchlistWorkspaceList::identityFromKey(series.identity);const auto key=laneKey(identity,series.intervalMs);
    auto lane=d_->lanes.find(key);
    if(identity.market!="stock"||lane==d_->lanes.end()||lane->generation!=generation||!lane->realtime||!d_->demanded(identity,series.intervalMs)){tick();return;}
    const auto at=d_->now();
    const auto request=StockChartSource::request(StockChartRead::Candles,identity,series.intervalMs==Day?Day:60000);
    const bool invalidSource=stock.source!=StockSourceKind::Toss||stock.delayed||stock.loading||!stock.realtimeAllowed||!stock.calendarAvailable
       ||(series.intervalMs!=Day&&!stock.intradayAllowed)||stock.adjustment!=StockAdjustment::SourceAdjusted
       ||!series.available||!fresh(series.receivedAtMs,at,ReceiptAge)||series.asset!=identity.code
       ||series.currency!=(identity.source=="KR"?"KRW":"USD")||!StockChartSource::isRequest(request)
       ||series.sourceUrl!=request.url.toString()||series.sourceTimeMs<0||series.sourceTimeMs>series.receivedAtMs||series.candles.isEmpty()||series.candles.size()>200||stock.timeline.rows.size()!=series.candles.size();
    if(invalidSource)d_->resetLines(identity,series.intervalMs);else d_->observeLines(series,generation,&stock.timeline);
    tick();lane=d_->lanes.find(key);if(lane==d_->lanes.end()||lane->generation!=generation)return;
    if(invalidSource){lane->seeded=false;lane->watermark=0;return;}
    // Recompute from typed completion/eligibility; a caller's cached study set
    // cannot turn a missing restriction mask or a pending row into a signal.
    const auto studies=keefiChartStudies(series.candles,series.intervalMs,stock.timeline);
    if(!studies.available||studies.lastCompletedAtMs>at){lane->seeded=false;lane->watermark=0;return;}
    const auto completed=studies.lastCompletedAtMs;
    if(!lane->seeded){lane->seeded=true;lane->watermark=completed;return;}
    const auto previous=lane->watermark;if(completed<=previous)return;
    bool continuous=false,previousFound=false;
    for(const auto& row:stock.timeline.rows){
        if(row.completesAtMs==previous){previousFound=true;continue;}
        if(row.completesAtMs<=previous)continue;
        if(!previousFound||!row.connectsPrevious||!row.eligible){continuous=false;break;}continuous=true;
    }
    lane->watermark=completed;if(!continuous)return;
    for(const auto& entry:studies.entries)if(entry.atMs>previous&&entry.atMs<=completed&&entry.score>=0&&entry.score<=10
       &&entry.waveLowAtMs>0&&entry.waveLowAtMs<entry.atMs&&std::isfinite(entry.entryClose)&&entry.entryClose>0){
        ChartPhraseFacts facts;facts.symbol=series.asset;facts.source=series.sourceUrl;facts.currency=series.currency;
        facts.intervalMs=series.intervalMs;facts.observedAtMs=entry.atMs;facts.score=entry.score;facts.samples=studies.observations;facts.price=entry.entryClose;
        d_->candidate(identity,series.intervalMs,ChartAlertKind::PullbackEntry,QString::number(entry.waveLowAtMs),entry.atMs,series.receivedAtMs,facts,ChartPhraseKind::Pullback,entry.score);
    }
    if(studies.scorecard.available)for(const auto& outcome:studies.scorecard.outcomes)if(outcome.evaluationAtMs>previous&&outcome.evaluationAtMs<=completed
       &&outcome.signalAtMs>0&&outcome.evaluationAtMs-outcome.signalAtMs==studies.scorecard.horizonMs&&std::isfinite(outcome.returnPercent)){
        ChartPhraseFacts facts;facts.symbol=series.asset;facts.source=series.sourceUrl;facts.currency=series.currency;facts.intervalMs=series.intervalMs;
        facts.horizonMs=studies.scorecard.horizonMs;facts.observedAtMs=outcome.evaluationAtMs;facts.samples=studies.scorecard.samples;facts.resultReturnPercent=outcome.returnPercent;
        const auto kind=outcome.returnPercent>0?ChartPhraseKind::ResultUp:outcome.returnPercent<0?ChartPhraseKind::ResultDown:ChartPhraseKind::ResultFlat;
        d_->candidate(identity,series.intervalMs,ChartAlertKind::StudyResult,QString::number(outcome.signalAtMs),outcome.evaluationAtMs,series.receivedAtMs,facts,kind);
    }
}
void ChartAlertEngine::observeStrongReversalCalculated(const CoinFlowSnapshot& input,const CoinFlowStudySet& studies,const ExtendedStrongReversal& grade,quint64 generation){
    tick();if(!observationEnabled("strong_reversal"))return;
    const auto now=d_->now();const auto identity=WatchlistWorkspaceList::identityFromKey(input.identity);
    if(!flowIdentity(identity)||input.minutes.isEmpty()||studies.identity!=input.identity||!studies.available||studies.asOfMs<=0||studies.asOfMs>now||!fresh(studies.asOfMs,now,ReceiptAge))return;
    const bool privateOnly=generation&&d_->eligible(identity,60000,ChartAlertKind::Whale)==Holdings;
    if(generation){const auto lane=d_->lanes.constFind(laneKey(identity,60000));if(lane==d_->lanes.cend()||lane->generation!=generation||!lane->realtime||!lane->flowSeeded||d_->eligible(identity,60000,ChartAlertKind::Whale).isEmpty())return;}
    const auto& latest=input.minutes.last();QString asset,currency;const auto request=MarketCandles::request(input.identity,60000);
    if(request.url.isEmpty()||!latest.liveEligible||!latest.connectsPrevious||latest.completesAtMs!=studies.lastCompletedAtMs||latest.completesAtMs<=d_->observationEnabledAt.value("strong_reversal")||
       latest.knownAtMs>latest.decisionAtMs||latest.decisionAtMs>studies.asOfMs||!fresh(latest.evidence.receivedAtMs,now,ReceiptAge)||!coinCandleFacts(input,now,&asset,&currency))return;
    for(const auto& row:input.minutes)if(row.evidence.sourceUrl!=request.url.toString()||row.completesAtMs!=row.candle.startsAtMs+60000)return;
    for(const auto& group:studies.groups){
        const bool rebound=group.kind==CoinFlowSignalKind::Rebound,down=group.kind==CoinFlowSignalKind::TurnDown;
        if((!rebound&&!down)||group.fromMs!=latest.completesAtMs||group.knownAtMs>latest.decisionAtMs)continue;
        const auto first=std::find_if(studies.entries.cbegin(),studies.entries.cend(),[&](const auto& value){return value.kind==group.kind&&value.direction==group.direction&&value.atMs==group.fromMs;});
        if(first==studies.entries.cend()||!first->state.available||first->state.samples<1||first->knownAtMs>latest.decisionAtMs)continue;
        const auto condition=QString::number(int(first->kind))+"|"+QString::number(int(first->direction))+"|"+QString::number(group.fromMs);
        const auto canonical=laneKey(identity,60000)+"|"+kindKey(ChartAlertKind::Whale)+"|"+QString::number(first->atMs)+"|"+condition;
        if(d_->publicConsumed.contains(canonical)||d_->privateConsumed.contains(canonical))continue;
        d_->resolvedReversals[canonical]=first->atMs;while(d_->resolvedReversals.size()>128)d_->resolvedReversals.erase(d_->resolvedReversals.begin());
        const auto expected=rebound?IndicatorDirection::Up:IndicatorDirection::Down;
        const bool strong=!privateOnly&&grade.state.available&&grade.grade==ExtendedReversalGrade::Strong&&grade.state.direction==expected&&grade.throughMs==latest.completesAtMs&&grade.knownAtMs>=grade.throughMs&&grade.knownAtMs<=latest.decisionAtMs;
        if(strong){
            d_->deferredReversals.remove(canonical);d_->remember(canonical,false);
            const auto direction=rebound?MovementDirection::Up:MovementDirection::Down;
            observePublicStudy({identity,"strong_reversal",canonical,QString("완료 1분 반전 · 강함 · 관측 거래량과 같은 5분 체결 방향 확인"),first->atMs,latest.evidence.receivedAtMs,direction,true});
        }else if(d_->deferredReversals.contains(canonical)){
            const auto saved=d_->deferredReversals.take(canonical);const auto lane=d_->lanes.constFind(laneKey(saved.identity,60000));
            if(lane!=d_->lanes.cend()&&lane->generation==saved.generation&&lane->realtime)
                d_->candidate(saved.identity,60000,ChartAlertKind::Whale,saved.condition,saved.at,saved.received,saved.facts,ChartPhraseKind::Whale,10,ReceiptAge,{},{},{},{},saved.direction);
        }
    }
}
void ChartAlertEngine::observeCoinFlow(const CoinFlowSnapshot& input,quint64 generation){
    observeCoinFlowCalculated(input,keefi::coinFlowStudies(input,d_->now()),generation);
}
void ChartAlertEngine::observeCoinFlowCalculated(const CoinFlowSnapshot& input,const CoinFlowStudySet& studies,quint64 generation){
    tick();const auto identity=WatchlistWorkspaceList::identityFromKey(input.identity);auto lane=d_->lanes.find(laneKey(identity,60000));
    if(!flowIdentity(identity)||lane==d_->lanes.end()||lane->generation!=generation||!lane->realtime||d_->eligible(identity,60000,ChartAlertKind::Whale).isEmpty())return;
    const auto invalid=[&]{lane->flowSeeded=false;lane->flowWatermark=0;};const auto now=d_->now();
    const auto request=MarketCandles::request(input.identity,60000);
    if(input.minutes.isEmpty()||request.url.isEmpty()){invalid();return;}
    const auto& latest=input.minutes.last();
    QString asset,currency;
    if(!fresh(latest.evidence.receivedAtMs,now,ReceiptAge)||latest.completesAtMs>now||latest.knownAtMs>now||latest.decisionAtMs>now||latest.knownAtMs>latest.decisionAtMs
        ||!coinCandleFacts(input,now,&asset,&currency)){invalid();return;}
    for(const auto& row:input.minutes)if(row.evidence.sourceUrl!=request.url.toString()||row.completesAtMs!=row.candle.startsAtMs+60000){invalid();return;}
    if(lane->flowSeeded&&latest.completesAtMs<=lane->flowWatermark)return; // Raw trade updates cannot revisit an immutable completed-minute decision.
    const auto completed=studies.lastCompletedAtMs;
    if(!studies.available||studies.identity!=input.identity||studies.asOfMs<latest.decisionAtMs||studies.asOfMs>now||!fresh(studies.asOfMs,now,ReceiptAge)||completed!=latest.completesAtMs){invalid();return;}
    if(!lane->flowSeeded){lane->flowSeeded=true;lane->flowWatermark=completed;return;}
    const auto previous=lane->flowWatermark;if(completed<=previous)return;
    bool priorFound=false,continuous=false;qint64 expected=previous+60000;
    for(const auto& row:input.minutes){if(row.completesAtMs==previous){priorFound=true;continue;}if(row.completesAtMs<=previous)continue;
        if(!priorFound||!row.connectsPrevious||row.completesAtMs!=expected){continuous=false;break;}continuous=true;expected+=60000;}
    lane->flowWatermark=completed;if(!continuous)return;
    for(const auto& group:studies.groups){
        if(group.fromMs<=previous||group.fromMs>completed||group.knownAtMs>now)continue;
        const CoinFlowSignal* first=nullptr;for(const auto& entry:studies.entries)if(entry.atMs==group.fromMs&&entry.kind==group.kind&&entry.direction==group.direction){first=&entry;break;}
        if(!first||!first->state.available||first->state.samples<1||first->knownAtMs>now)continue;
        const auto observed=std::find_if(input.minutes.cbegin(),input.minutes.cend(),[&](const auto& row){return row.completesAtMs==first->atMs;});
        if(observed==input.minutes.cend()||!observed->liveEligible||first->knownAtMs>observed->decisionAtMs)continue;
        double entryPrice=0;if(!price(first->entryClose,&entryPrice))continue;ChartPhraseFacts facts;
        facts.symbol=asset;facts.currency=currency;facts.source=request.url.toString();
        facts.intervalMs=60000;facts.observedAtMs=first->atMs;facts.samples=first->state.samples;facts.price=entryPrice;
        switch(first->kind){case CoinFlowSignalKind::LargeTrade:facts.whaleSignal=ChartWhalePhraseSignal::LargeTrade;break;
        case CoinFlowSignalKind::TakerSkew:facts.whaleSignal=ChartWhalePhraseSignal::TakerSkew;break;
        case CoinFlowSignalKind::OpenInterestJump:facts.whaleSignal=ChartWhalePhraseSignal::OpenInterestJump;break;
        case CoinFlowSignalKind::VolumeBurst:facts.whaleSignal=ChartWhalePhraseSignal::VolumeBurst;break;
        case CoinFlowSignalKind::Rebound:facts.whaleSignal=ChartWhalePhraseSignal::Rebound;break;
        case CoinFlowSignalKind::TurnDown:facts.whaleSignal=ChartWhalePhraseSignal::TurnDown;break;}
        const auto condition=QString::number(int(first->kind))+"|"+QString::number(int(first->direction))+"|"+QString::number(group.fromMs);
        const auto direction=group.direction==CoinFlowDirection::Up?MovementDirection::Up:group.direction==CoinFlowDirection::Down?MovementDirection::Down:MovementDirection::Unknown;
        const auto canonical=laneKey(identity,60000)+"|"+kindKey(ChartAlertKind::Whale)+"|"+QString::number(first->atMs)+"|"+condition;
        const bool reversal=first->kind==CoinFlowSignalKind::Rebound||first->kind==CoinFlowSignalKind::TurnDown;
        if(reversal&&observationEnabled("strong_reversal")&&first->atMs==completed&&first->atMs>d_->observationEnabledAt.value("strong_reversal")&&!d_->resolvedReversals.contains(canonical)){
            if(!d_->publicConsumed.contains(canonical)&&!d_->privateConsumed.contains(canonical)){
                d_->deferredReversals[canonical]={identity,condition,first->atMs,latest.evidence.receivedAtMs,facts,direction,generation,d_->eligible(identity,60000,ChartAlertKind::Whale)==Holdings};
                while(d_->deferredReversals.size()>128)d_->deferredReversals.erase(d_->deferredReversals.begin());
            }
            continue;
        }
        d_->candidate(identity,60000,ChartAlertKind::Whale,condition,first->atMs,latest.evidence.receivedAtMs,facts,ChartPhraseKind::Whale,10,ReceiptAge,{},{},{},{},direction);
    }
}
void ChartAlertEngine::invalidateLane(const WatchlistIdentity& identity,qint64 interval,quint64 generation){auto lane=d_->lanes.find(laneKey(identity,interval));if(lane!=d_->lanes.end()&&lane->generation==generation){lane->seeded=false;lane->watermark=0;lane->flowSeeded=false;lane->flowWatermark=0;d_->resetLines(identity,interval);}}
void ChartAlertEngine::observeLiquidation(const ChartLiquidationObservation& value,quint64 epoch){
    if(!d_->privateEnabled||epoch!=d_->epoch)return;quint64 sequence=0;const bool entering=d_->recordRisk(value,epoch,&sequence);
    d_->reconcile();if(entering)d_->submitRisk(value,epoch,sequence);tick();
}
void ChartAlertEngine::setLiquidationObservations(const QList<ChartLiquidationObservation>& observations,quint64 epoch){
    if(!d_->privateEnabled||epoch!=d_->epoch)return;
    QMap<QString,ChartLiquidationObservation> current;QSet<QString> duplicates;
    if(observations.size()<=10000)for(const auto& value:observations){
        double distance=0;if(!liquidationObservation(value,d_->now(),&distance)||duplicates.contains(value.positionToken))continue;
        if(current.contains(value.positionToken)){current.remove(value.positionToken);duplicates.insert(value.positionToken);}else current.insert(value.positionToken,value);
    }
    // This is a complete current snapshot. An absent or invalid proof revokes
    // its pending retry before any new observation can be submitted.
    for(auto it=d_->risks.begin();it!=d_->risks.end();)if(!current.contains(it.key()))it=d_->risks.erase(it);else ++it;
    QList<QPair<ChartLiquidationObservation,quint64>> entering;
    for(const auto& value:current){quint64 sequence=0;if(d_->recordRisk(value,epoch,&sequence))entering.append({value,sequence});}
    d_->reconcile();
    for(const auto& event:entering){if(!d_->privateEnabled||epoch!=d_->epoch)return;d_->submitRisk(event.first,epoch,event.second);}
    tick();
}
void ChartAlertEngine::tick(){
    const auto at=d_->now();
    for(auto it=d_->resolvedReversals.begin();it!=d_->resolvedReversals.end();)if(!fresh(it.value(),at,ReceiptAge))it=d_->resolvedReversals.erase(it);else ++it;
    for(auto it=d_->deferredReversals.begin();it!=d_->deferredReversals.end();){const auto lane=d_->lanes.constFind(laneKey(it->identity,60000));if(!fresh(it->at,at,ReceiptAge)||lane==d_->lanes.cend()||lane->generation!=it->generation||d_->eligible(it->identity,60000,ChartAlertKind::Whale).isEmpty())it=d_->deferredReversals.erase(it);else ++it;}
if(d_->privateEnabled&&d_->holdingsReceived&&!fresh(d_->holdingsReceived,at,HoldingAge)){d_->clearHoldings();emit changed();emit demandsChanged();}
    QStringList retry;for(auto it=d_->pending.cbegin();it!=d_->pending.cend();++it)if(it->retryAt&&at>=it->retryAt)retry.append(it.key());for(const auto& id:retry)d_->submit(id);
    const auto today=d_->date(at);for(auto it=d_->counts.begin();it!=d_->counts.end();)if(it.key()<today.addDays(-2)&&it->reserved==0)it=d_->counts.erase(it);else ++it;
}
QByteArray ChartAlertEngine::publicState() const{
    QJsonArray rules;for(const auto& rule:d_->rules)rules.append(ruleJson(rule));return QJsonDocument(QJsonObject{{"version",3},{"settings",settingsJson(d_->settings)},{"rules",rules}}).toJson(QJsonDocument::Compact);
}
bool ChartAlertEngine::restorePublicState(const QByteArray& state,QString* error){
    const auto fail=[&](){if(error)*error=QStringLiteral("공개 알림 설정 형식이 올바르지 않습니다");return false;};if(state.size()>256*1024)return fail();QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(state,&parse);if(parse.error!=QJsonParseError::NoError||!document.isObject())return fail();const auto object=document.object();
    const auto version=object.value("version");if(!exactKeys(object,{"version","settings","rules"})||(version!=QJsonValue(1)&&version!=QJsonValue(2)&&version!=QJsonValue(3))||!object.value("settings").isObject()||!object.value("rules").isArray())return fail();
    ChartAlertSettings settings;if(!readSettings(object.value("settings").toObject(),&settings))return fail();const auto array=object.value("rules").toArray();if(array.size()>33)return fail();QMap<QString,ChartAlertRule> rules;
    for(const auto& value:array){ChartAlertRule rule;if(!value.isObject()||!readRule(value.toObject(),&rule,version.toInt())||rules.contains(rule.listId)||(rule.listId!=Holdings&&!d_->lists.contains(rule.listId)))return fail();rules.insert(rule.listId,rule);}
    if(!rules.contains(Holdings))rules.insert(Holdings,defaultRule(Holdings));for(auto it=d_->lists.cbegin();it!=d_->lists.cend();++it)if(!rules.contains(it.key()))rules.insert(it.key(),defaultRule(it.key()));
    const bool disabled=d_->settings.dripEnabled&&!settings.dripEnabled;d_->settings=settings;d_->rules=rules;d_->reconcile();emit changed();emit demandsChanged();if(disabled)emit dripDisabled();return true;
}
bool ChartAlertEngine::setMarketMetricEnabled(MarketFlowMetric metric,bool enabled){
    if(!marketMetricValid(metric))return false;if(d_->marketEnabled.value(metric)==enabled)return true;
    d_->marketEnabled.insert(metric,enabled);d_->resetMarket(metric,true);emit changed();emit demandsChanged();return true;
}
bool ChartAlertEngine::marketMetricEnabled(MarketFlowMetric metric)const{return marketMetricValid(metric)&&d_->marketEnabled.value(metric);}
void ChartAlertEngine::observeMarketMetric(const QString& asset,MarketFlowMetric metric,MarketFlowState state,qint64 sourceTime,qint64 received){
    if(!marketMetricValid(metric)||metric==MarketFlowMetric::OpenInterest)return;
    d_->observeMarket(asset,metric,marketStateValid(state)?int(state):-1,marketStateValid(state)?marketFlowStateLabel(state):QString{},sourceTime,received);
}
void ChartAlertEngine::observeContractFlow(const QString& asset,const ContractFlow& flow,qint64 received){
    const bool valid=flow.available&&flow.priceDirection>=-1&&flow.priceDirection<=1&&flow.quantityDirection>=-1&&flow.quantityDirection<=1&&
        flow.fromMs<flow.toMs&&fresh(flow.fromMs,d_->now(),MarketMetrics::FreshnessMs);
    d_->observeMarket(asset,MarketFlowMetric::OpenInterest,valid?100+(flow.priceDirection+1)*3+flow.quantityDirection+1:-1,
        valid?marketFlowContractLabel(flow):QString{},flow.toMs,received,flow.fromMs);
}
QByteArray ChartAlertEngine::marketMetricState()const{
    return QJsonDocument(QJsonObject{{"version",1},{"koreaGap",marketMetricEnabled(MarketFlowMetric::KoreaGap)},
        {"funding",marketMetricEnabled(MarketFlowMetric::Funding)},{"coinbaseGap",marketMetricEnabled(MarketFlowMetric::CoinbaseGap)},
        {"openInterest",marketMetricEnabled(MarketFlowMetric::OpenInterest)}}).toJson(QJsonDocument::Compact);
}
bool ChartAlertEngine::restoreMarketMetricState(const QByteArray& bytes){
    if(bytes.isEmpty()||bytes.size()>4096)return false;const auto document=QJsonDocument::fromJson(bytes);if(!document.isObject())return false;
    const auto object=document.object();qint64 version=0;
    if(!exactKeys(object,{"version","koreaGap","funding","coinbaseGap","openInterest"})||!integer(object.value("version"),1,1,&version))return false;
    for(const auto* key:{"koreaGap","funding","coinbaseGap","openInterest"})if(!object.value(key).isBool())return false;
    const QMap<MarketFlowMetric,bool> next{{MarketFlowMetric::KoreaGap,object.value("koreaGap").toBool()},
        {MarketFlowMetric::Funding,object.value("funding").toBool()},{MarketFlowMetric::CoinbaseGap,object.value("coinbaseGap").toBool()},
        {MarketFlowMetric::OpenInterest,object.value("openInterest").toBool()}};
    bool changed=false;for(auto it=next.cbegin();it!=next.cend();++it)if(d_->marketEnabled.value(it.key())!=it.value()){
        changed=true;d_->marketEnabled.insert(it.key(),it.value());d_->resetMarket(it.key(),true);
    }
    if(changed){emit this->changed();emit demandsChanged();}return true;
}
}
