#include "keefi/ChartPublicContextSource.h"
#include "keefi/CoinFlowProtocol.h"
#include "keefi/ChartStudyRequestPacer.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QTimeZone>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
constexpr qint64 Hour=3600000;
bool coin(const QString& id){return QRegularExpression("\\Acoin:(?:upbit:KRW-[A-Z0-9]{1,20}|binance:[A-Z0-9]{2,40})\\z").match(id).hasMatch();}
bool symbol(const QString& s){return QRegularExpression("\\A[A-Z0-9]{1,20}USDT\\z").match(s).hasMatch();}
QDateTime utc(qint64 at){return QDateTime::fromMSecsSinceEpoch(at,QTimeZone::UTC);}
qint64 startOf(ChartPublicContextSource::Read read,qint64 now){auto date=utc(now).date();if(read==ChartPublicContextSource::Read::Week)date=date.addDays(1-date.dayOfWeek());if(read==ChartPublicContextSource::Read::Month)date.setDate(date.year(),date.month(),1);return QDateTime(date,QTime(),QTimeZone::UTC).toMSecsSinceEpoch();}
qint64 nextOf(ChartPublicContextSource::Read read,qint64 start,int delta=1){auto date=utc(start);return read==ChartPublicContextSource::Read::Month?date.addMonths(delta).toMSecsSinceEpoch():date.addDays((read==ChartPublicContextSource::Read::Week?7:1)*delta).toMSecsSinceEpoch();}
bool success(const HttpResult& r){return r.status==200&&!r.networkError&&!r.timedOut&&r.localRetryAfterSeconds<=0;}
bool positive(const QString& s){return decimal::positive(s)&&std::isfinite(s.toDouble());}
bool nonnegative(const QString& s){return decimal::valid(s)&&std::isfinite(s.toDouble());}
QString valueText(double v){return std::isfinite(v)?QString::number(v,'f',8):QString{};}
qint64 timestamp(const QJsonValue& v){bool ok=false;auto t=v.toString().toLongLong(&ok);return ok?t:0;}
}
QList<ExtendedLevelCandidate> ChartPublicContextSource::parseLevels(Read read,const QString& id,const HttpResult& response,qint64 asOf,qint64 received,QString* reason){
    if(reason)*reason="완료 기간 자료를 받지 못했어요";QList<ExtendedLevelCandidate> out;
    if(!coin(id)||asOf<=0||received<asOf||!success(response)||!(read==Read::Day||read==Read::Week||read==Read::Month))return out;
    const auto doc=parseExactJson(response.body);if(!doc.isArray()||doc.array().isEmpty()||doc.array().size()>(read==Read::Day?10:read==Read::Week?4:3))return out;
    const auto boundary=startOf(read,asOf),expected=nextOf(read,boundary,-1);const auto monday=startOf(Read::Week,asOf);QSet<qint64> seen;
    for(const auto& v:doc.array()){
        qint64 at=0;QString o,h,l,c,volume;
        if(id.startsWith("coin:upbit:")){if(!v.isObject())return {};const auto row=v.toObject();if(row.value("market").toString()!=id.section(':',2))return {};
            const auto text=row.value("candle_date_time_utc").toString();const auto date=QDateTime::fromString(text+"Z",Qt::ISODate);if(!date.isValid()||date.toUTC().toString("yyyy-MM-ddTHH:mm:ss")!=text)return {};at=date.toMSecsSinceEpoch();
            const auto stored=timestamp(row.value("timestamp"));if(stored<at||stored>received)return {};o=row.value("opening_price").toString();h=row.value("high_price").toString();l=row.value("low_price").toString();c=row.value("trade_price").toString();volume=row.value("candle_acc_trade_volume").toString();
        }else{if(!v.isArray())return {};const auto row=v.toArray();if(row.size()!=12)return {};at=timestamp(row[0]);if(timestamp(row[6])!=nextOf(read,at)-1)return {};o=row[1].toString();h=row[2].toString();l=row[3].toString();c=row[4].toString();volume=row[5].toString();}
        if(at<=0||at>asOf||at!=startOf(read,at)||seen.contains(at)||!positive(o)||!positive(h)||!positive(l)||!positive(c)||decimal::normalize(volume).isEmpty()||decimal::compare(l,o)>0||decimal::compare(l,c)>0||decimal::compare(h,o)<0||decimal::compare(h,c)<0)return {};seen.insert(at);
        const auto end=nextOf(read,at);if(end>asOf)continue;
        const auto add=[&](const QString& tag,ExtendedLevelPeriod period,bool includeOpen){const auto source=request(read,id).url.toString();
            const auto one=[&](QString suffix,QString name,QString price){out.append({tag+suffix,name,price,source,period,at,end,received,true});};
            one("High",tag=="early"?QString("주 초반 고"):tag=="day"?QString("전일 고"):tag=="week"?QString("전주 고"):QString("전월 고"),h);
            one("Low",tag=="early"?QString("주 초반 저"):tag=="day"?QString("전일 저"):tag=="week"?QString("전주 저"):QString("전월 저"),l);
            if(includeOpen)one("Open",tag=="day"?QString("전일 시"):tag=="week"?QString("전주 시"):QString("전월 시"),o);};
        if(at==expected)add(read==Read::Day?"day":read==Read::Week?"week":"month",read==Read::Day?ExtendedLevelPeriod::Day:read==Read::Week?ExtendedLevelPeriod::Week:ExtendedLevelPeriod::Month,true);
        if(read==Read::Day&&at==monday)add("early",ExtendedLevelPeriod::EarlyWeek,false);
    }
    if(reason)*reason=out.isEmpty()?QString("직전 완료 기간 기록이 없어요"):QString{};return out;
}
struct ChartPublicContextSource::Private {
    struct Cached {HttpResult reply;qint64 started=0,received=0,expires=0;};
    ChartPublicContextSource* q=nullptr;QPointer<TradingTransport> transport;Clock clock;QTimer timer;
    std::shared_ptr<ChartStudyRequestPacer> pacer;ChartStudyRequestPacer::Client pacingClient=0;
    std::unique_ptr<PublicCandleSocket> socket;CoinFlowInstrument liquidationContract;
    struct LiquidationRecord {ChartLiquidationRow row;bool liveEligible=false,notified=false;};
    QList<LiquidationRecord> liquidationRecords;QSet<QString> liquidationIds;
    QList<QPair<qint64,qint64>> liquidationSegments;
    quint64 socketGeneration=0,nextSocketGeneration=1;bool socketConnected=false,socketDenied=false;
    qint64 connectedAt=0,reconnectAt=0,publishedBoundary=0;
    QString minimumQuote="100000";
    QString identity,base,quote,future;bool levels=false,futures=false;ChartPublicContext data;QMap<int,QList<ExtendedLevelCandidate>> periodLevels;QMap<QString,Cached> cache;
    QList<Read> queue;Read running=Read::Day;quint64 token=0,nextToken=1; qint64 sentAt=0,nextSend=0,nextRefresh=0,contractsAt=0;bool topDenied=false;PublicDerivativesMetrics verified;
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
    void stopLiquidation(bool reset){
        if(socketConnected&&connectedAt>0)liquidationSegments.append({connectedAt,now()});
        const bool opened=socketGeneration!=0;socketGeneration=0;socketConnected=false;connectedAt=0;
        if(opened&&socket)socket->abort();
        if(reset){liquidationRecords.clear();liquidationIds.clear();liquidationSegments.clear();reconnectAt=0;socketDenied=false;publishedBoundary=0;liquidationContract={};}
    }
    void startLiquidation(){
        if(!socket||!futures||!data.futuresSupported||!contractsAt||now()-contractsAt>=180000||socketGeneration||socketDenied||now()<reconnectAt)return;
        liquidationContract={};auto& c=liquidationContract;c.venue=CoinFlowVenue::BinanceUsdM;c.grain=CoinFlowGrain::Futures100msAggregate;
        c.identity="usdm_futures:binance:"+future;c.symbol=future;c.baseAsset=base;c.quoteAsset=c.marginAsset="USDT";
        c.contractType="PERPETUAL";c.status="TRADING";c.underlyingType="COIN";c.verified=true;
        c.evidence={"Binance 공식 USD-M 계약",ChartPublicContextSource::request(Read::Contracts,identity).url.toString(),0,contractsAt,contractsAt};
        socketGeneration=nextSocketGeneration++;
        socket->open(socketGeneration,QUrl("wss://fstream.binance.com/market/ws/"+future.toLower()+"@forceOrder"));
    }
    void receiveLiquidation(quint64 generation,const QByteArray& raw){
        const auto at=now();if(generation!=socketGeneration||!socketConnected||!futures||!data.futuresSupported||!contractsAt||at-contractsAt>=180000)return;
        const auto batch=CoinFlowProtocol::liquidationFrame(liquidationContract,raw,at,generation);if(!batch.available)return;
        const auto document=parseExactJson(raw);if(!document.isObject())return;
        const auto id=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(document.object()).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        if(liquidationIds.contains(id))return;
        for(auto observation:batch.observations){
            if(observation.tradeAtMs<at-Hour)continue;
            observation.evidence.sourceUrl="wss://fstream.binance.com/market/ws/"+future.toLower()+"@forceOrder";
            const auto notional=decimal::multiply(observation.averagePrice,observation.cumulativeFilledQuantity);if(notional.isEmpty())continue;
            liquidationIds.insert(id);liquidationRecords.append({{id,notional,observation},observation.tradeAtMs>=connectedAt,false});
            while(liquidationRecords.size()>1000){liquidationIds.remove(liquidationRecords.first().row.eventId);liquidationRecords.removeFirst();}
        }
    }
    void publishLiquidation(bool force=false,bool notify=true){
        const auto at=now(),boundary=at-at%60000;if(!force&&publishedBoundary==boundary)return;publishedBoundary=boundary;
        ChartLiquidationFeed result;result.supported=futures&&data.futuresSupported&&contractsAt>0&&at-contractsAt<180000;result.connected=socketConnected;result.minimumQuote=minimumQuote;
        result.throughMs=boundary;result.fromMs=boundary-Hour;result.reason=socketConnected?QString("종목별 1초 마지막 snapshot · 부분 기록"):QString("청산 관측 연결이 끊겼어요");
        if(!result.supported){data.liquidation=result;return;}
        for(auto it=liquidationRecords.begin();it!=liquidationRecords.end();){if(it->row.observation.tradeAtMs<boundary-Hour){liquidationIds.remove(it->row.eventId);it=liquidationRecords.erase(it);}else ++it;}
        qint64 coverage=0;for(auto it=liquidationSegments.begin();it!=liquidationSegments.end();){if(it->second<boundary-Hour)it=liquidationSegments.erase(it);else{coverage+=qMax(qint64(0),qMin(it->second,boundary)-qMax(it->first,boundary-Hour));++it;}}
        if(socketConnected)coverage+=qMax(qint64(0),boundary-qMax(connectedAt,boundary-Hour));result.coverageMinutes=int(coverage/60000);
        for(auto& record:liquidationRecords){const auto& item=record.row.observation;if(item.tradeAtMs>=boundary||item.knownAtMs>boundary)continue;++result.observedCount;
            const bool included=decimal::compare(record.row.quoteNotional,minimumQuote)>=0;
            if(included)result.rows.append(record.row);
            if(!record.notified){record.notified=true;if(notify&&included&&record.liveEligible){
                const auto detail=QString("%1 · 원문 %2 · 관측 금액 %3 USDT · 부분 기록").arg(item.symbol,item.side,record.row.quoteNotional);
                emit q->liquidationObserved(identity,record.row.eventId,detail,item.tradeAtMs,item.receivedAtMs);
            }}
        }
        std::sort(result.rows.begin(),result.rows.end(),[](const auto& a,const auto& b){return a.observation.tradeAtMs>b.observation.tradeAtMs;});
        if(result.observedCount==0&&socketConnected)result.reason="관측 없음 · 시장 전체 청산 0을 뜻하지 않아요";
        data.liquidation=std::move(result);emit q->changed();
    }
    QString cacheKey(Read r)const{return QString::number(int(r))+":"+(int(r)<=int(Read::Month)?identity:future);}
    void abort(){const bool active=token;token=0;queue.clear();if(pacer)pacer->cancel(pacingClient);if(active&&transport)transport->abort();}
    void rebuild(){data.levelCandidates.clear();for(const auto& values:periodLevels)data.levelCandidates.append(values);data.levelsReason=data.levelCandidates.isEmpty()?QString("완료 기간 자료를 받는 중이에요"):QString{};}
    void enqueueMetrics(){queue.append({Read::Funding,Read::OpenInterest,Read::FuturesPrice});if(!topDenied)queue.append(Read::TopPosition);queue.append(Read::Taker);}
    void clearFutures(){stopLiquidation(false);verified={};contractsAt=0;data.futuresChecked=false;data.futuresSupported=false;data.futuresSymbol.clear();data.futuresReason.clear();data.funding={};data.oiChange={};data.priceChange={};data.topPosition={};data.topPosition.hidden=topDenied;data.takerBuy={};data.openInterest.clear();data.openInterestUnit.clear();data.liquidation={};}
    void enqueue(){if(!levels&&!futures)return;queue.clear();if(levels)queue={Read::Day,Read::Week,Read::Month};if(futures&&!future.isEmpty()){if(!contractsAt||now()-contractsAt>=180000)clearFutures();if(verified.baseAsset.isEmpty())queue.append(Read::Contracts);else enqueueMetrics();}}
    ChartPublicCell* cell(Read r){switch(r){case Read::Funding:return &data.funding;case Read::OpenInterest:return &data.oiChange;case Read::FuturesPrice:return &data.priceChange;case Read::TopPosition:return &data.topPosition;case Read::Taker:return &data.takerBuy;default:return nullptr;}}
    bool apply(Read read,const HttpResult& response,qint64 received,qint64 decision){
        if(int(read)<=int(Read::Month)){QString why;const auto candidates=ChartPublicContextSource::parseLevels(read,identity,response,decision,received,&why);periodLevels[int(read)]=candidates;rebuild();if(!why.isEmpty())data.levelsReason=why;return !candidates.isEmpty();}
        if(read==Read::Contracts){data.futuresChecked=true;data.futuresSupported=MarketMetrics::applyResponse(MarketMetrics::Read::Contracts,future,response,received,&verified)&&verified.baseAsset==base;data.futuresReason=data.futuresSupported?QString{}:QString("이 코인의 USD-M 계약을 확인할 수 없어요");if(data.futuresSupported){contractsAt=received;data.futuresSymbol=future;enqueueMetrics();startLiquidation();publishLiquidation(true,false);}else{verified={};contractsAt=0;stopLiquidation(false);}return data.futuresSupported;}
        auto* target=cell(read);if(!target)return false;*target={};target->receivedAtMs=received;target->sourceUrl=ChartPublicContextSource::request(read,identity,future).url.toString();target->reason="받지 못했어요";
        if(read==Read::OpenInterest){data.openInterest.clear();data.openInterestUnit.clear();}if(!success(response)){if(read==Read::TopPosition&&(response.status==401||response.status==403)){topDenied=true;target->hidden=true;target->reason="무키 제공을 확인할 수 없어요";}return false;}
        if(verified.baseAsset!=base||!contractsAt||decision-contractsAt>=180000){target->reason="계약 정보를 확인하고 있어요";return false;}
        if(read==Read::Funding){if(!MarketMetrics::applyResponse(MarketMetrics::Read::Funding,future,response,received,&verified))return false;target->value=valueText(verified.fundingRate.toDouble()*100);target->unit="%";target->throughMs=verified.fundingEvidence.sourceTimeMs;target->available=!target->value.isEmpty();target->reason.clear();return target->available;}
        const auto doc=parseExactJson(response.body);if(!doc.isArray()||doc.array().isEmpty()||doc.array().size()>(read==Read::OpenInterest?200:read==Read::FuturesPrice?3:2))return false;
        if(read==Read::FuturesPrice){
            QMap<qint64,QString> prices;QSet<qint64> seen;
            for(const auto& value:doc.array()){
                if(!value.isArray())return false;const auto row=value.toArray();if(row.size()!=12)return false;
                const auto start=timestamp(row[0]),end=timestamp(row[6])+1;
                if(start<=0||start%Hour||end!=start+Hour||start>received||seen.contains(start))return false;seen.insert(start);
                const auto o=row[1].toString(),h=row[2].toString(),l=row[3].toString(),c=row[4].toString();
                if(!positive(o)||!positive(h)||!positive(l)||!positive(c)||!nonnegative(row[5].toString())||decimal::compare(l,o)>0||decimal::compare(l,c)>0||decimal::compare(h,o)<0||decimal::compare(h,c)<0)return false;
                if(end<=decision)prices.insert(end,c);
            }
            if(prices.isEmpty())return false;const auto end=prices.lastKey();if(decision-end>2*Hour||!prices.contains(end-Hour))return false;
            target->fromMs=end-Hour;target->throughMs=end;target->value=valueText(100*(prices.value(end).toDouble()/prices.value(end-Hour).toDouble()-1));target->unit="%";target->available=!target->value.isEmpty();target->reason=target->available?QString{}:QString("계산할 수 없어요");return target->available;
        }
        QMap<qint64,QJsonObject> rows;
        for(const auto& value:doc.array()){if(!value.isObject())return false;const auto row=value.toObject();auto at=timestamp(row.value("timestamp"));if(at<=0||at%Hour||rows.contains(at)||at>received)return false;if(read!=Read::Taker&&row.value("symbol").toString()!=future)return false;if(read==Read::Taker)at+=Hour;if(at<=decision)rows.insert(at,row);}
        if(rows.isEmpty())return false;const auto at=rows.lastKey();if(decision-at>2*Hour){target->reason="지연";return false;}target->throughMs=at;target->fromMs=at-Hour;
        if(read==Read::OpenInterest){QList<ChartPricePoint> points;for(auto it=rows.cbegin();it!=rows.cend();++it){const auto n=it.value().value("sumOpenInterest").toString();if(!nonnegative(n))return false;points.append({it.key(),n.toDouble()});}data.openInterest=std::move(points);data.openInterestUnit=base;if(!rows.contains(at-Hour))return false;const auto before=rows.value(at-Hour).value("sumOpenInterest").toString(),current=rows.value(at).value("sumOpenInterest").toString();if(!positive(before))return false;target->value=valueText((current.toDouble()/before.toDouble()-1)*100);target->unit="%";}
        else if(read==Read::TopPosition){const auto r=rows.last().value("longShortRatio").toString();if(!nonnegative(r))return false;target->value=r;}
        else{const auto buy=rows.last().value("buyVol").toString(),sell=rows.last().value("sellVol").toString();if(!nonnegative(buy)||!nonnegative(sell)||!std::isfinite(buy.toDouble()+sell.toDouble())||buy.toDouble()+sell.toDouble()<=0)return false;target->value=valueText(100*buy.toDouble()/(buy.toDouble()+sell.toDouble()));target->unit="%";}
        target->available=!target->value.isEmpty();target->reason=target->available?QString{}:QString("계산할 수 없어요");return target->available;
    }
    void finished(quint64 replyToken,const HttpResult& response){if(!token||token!=replyToken)return;token=0;const auto at=now();const auto read=running;
        if(response.status==429||response.localRetryAfterSeconds>0){qint64 seconds=qMax(1,response.localRetryAfterSeconds);for(auto it=response.headers.cbegin();it!=response.headers.cend();++it)if(it.key().toLower()=="retry-after"){bool ok=false;auto value=it.value().toLongLong(&ok);if(ok)seconds=qMax(seconds,value);else{const auto retry=QDateTime::fromString(QString::fromLatin1(it.value()),Qt::RFC2822Date);if(retry.isValid())seconds=qMax(seconds,(retry.toMSecsSinceEpoch()-at+999)/1000);}}nextSend=qMax(nextSend,at+qMin<qint64>(seconds,86400)*1000);}
        const bool ok=apply(read,response,at,sentAt);if(ok){const auto expires=int(read)<=int(Read::Month)?nextOf(read,startOf(read,at)):read==Read::Contracts?at+180000:at+30000;cache[cacheKey(read)]={response,sentAt,at,expires};while(cache.size()>256)cache.erase(cache.begin());}emit q->changed();pump();
    }
    void pump(){const auto at=now();if(!transport||(!levels&&!futures)||!coin(identity))return;startLiquidation();publishLiquidation();if(token){if(at-sentAt<10000)return;const auto stale=token;Q_UNUSED(stale);token=0;transport->abort();HttpResult failed;failed.timedOut=true;apply(running,failed,at,at);nextSend=qMax(nextSend,at+5000);emit q->changed();return;}
        if(queue.isEmpty()&&at>=nextRefresh){enqueue();nextRefresh=at+30000;}
        while(!queue.isEmpty()){const auto read=queue.first();const auto key=cacheKey(read);if(cache.contains(key)&&cache[key].expires>at){queue.takeFirst();const auto saved=cache[key];apply(read,saved.reply,saved.received,saved.started);continue;}if(at<nextSend)return;const auto request=ChartPublicContextSource::request(read,identity,future);if(request.url.isEmpty()){queue.takeFirst();continue;}if(pacer&&!pacer->tryAcquire(pacingClient,request.url))return;queue.takeFirst();running=read;token=nextToken++;sentAt=at;nextSend=at+1000;transport->send(token,request);return;}}
};
ChartPublicContextSource::ChartPublicContextSource(TradingTransport* transport,QObject* parent,Clock clock):ChartPublicContextSource(transport,{},parent,std::move(clock)){}
ChartPublicContextSource::ChartPublicContextSource(TradingTransport* transport,std::unique_ptr<PublicCandleSocket> socket,QObject* parent,Clock clock):QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->transport=transport;d_->clock=std::move(clock);d_->timer.setInterval(200);connect(&d_->timer,&QTimer::timeout,this,[this]{d_->pump();});
    if(transport){connect(transport,&TradingTransport::finished,this,[this](quint64 t,const HttpResult& r){d_->finished(t,r);});connect(transport,&QObject::destroyed,this,[this]{d_->transport=nullptr;d_->token=0;d_->queue.clear();});}
    d_->socket=std::move(socket);if(d_->socket){
        connect(d_->socket.get(),&PublicCandleSocket::connected,this,[this](quint64 generation){if(generation!=d_->socketGeneration||!d_->futures)return;d_->socketConnected=true;d_->connectedAt=d_->now();d_->publishLiquidation(true,false);});
        connect(d_->socket.get(),&PublicCandleSocket::received,this,[this](quint64 generation,const QByteArray& raw){d_->receiveLiquidation(generation,raw);});
        connect(d_->socket.get(),&PublicCandleSocket::failed,this,[this](quint64 generation,int status){if(generation!=d_->socketGeneration)return;d_->stopLiquidation(false);d_->reconnectAt=d_->now()+5000;if(status==401||status==403)d_->socketDenied=true;d_->publishLiquidation(true,false);});
    }
}
ChartPublicContextSource::~ChartPublicContextSource(){d_->timer.stop();if(d_->transport)disconnect(d_->transport,nullptr,this,nullptr);if(d_->socket)disconnect(d_->socket.get(),nullptr,this,nullptr);d_->stopLiquidation(true);d_->abort();if(d_->pacer)d_->pacer->release(d_->pacingClient);}
void ChartPublicContextSource::setSelection(const QString& id,const QString& base,const QString& quote){
    const auto validId=coin(id)?id:QString{};
    const auto effectiveBase=id.startsWith("coin:upbit:KRW-")?id.section('-',1):base;
    const auto effectiveQuote=id.startsWith("coin:upbit:KRW-")?QString("KRW"):quote;
    if(d_->identity==validId&&d_->base==effectiveBase&&d_->quote==effectiveQuote)return;
    d_->abort();d_->stopLiquidation(true);d_->identity=validId;d_->base=effectiveBase;d_->quote=effectiveQuote;d_->future.clear();d_->verified={};d_->contractsAt=0;d_->topDenied=false;d_->periodLevels.clear();d_->data={};d_->data.identity=d_->identity;d_->nextRefresh=0;
    if(id.startsWith("coin:upbit:KRW-")){d_->base=id.section('-',1);d_->quote="KRW";}
    if(!d_->base.isEmpty()&&symbol(d_->base+"USDT")&&(id.startsWith("coin:upbit:")||id.section(':',2)==d_->base+d_->quote))d_->future=d_->base+"USDT";
    d_->pump();emit changed();}
void ChartPublicContextSource::setDemand(bool levels,bool futures){if(d_->levels==levels&&d_->futures==futures)return;d_->abort();d_->levels=levels;d_->futures=futures;d_->nextRefresh=0;if(!levels){d_->periodLevels.clear();d_->data.levelCandidates.clear();d_->data.levelsReason.clear();}if(!futures)d_->clearFutures();if(levels||futures){d_->timer.start();d_->pump();}else{d_->timer.stop();d_->data={};d_->data.identity=d_->identity;}emit changed();}
void ChartPublicContextSource::setLiquidationMinimum(const QString& value){const auto normalized=decimal::normalize(value);if(!decimal::positive(normalized)||normalized==d_->minimumQuote)return;d_->minimumQuote=normalized;d_->publishLiquidation(true,false);}
void ChartPublicContextSource::setRequestPacer(std::shared_ptr<ChartStudyRequestPacer> value){if(d_->pacer==value)return;if(d_->pacer)d_->pacer->release(d_->pacingClient);d_->pacer=std::move(value);d_->pacingClient=d_->pacer?d_->pacer->createClient():0;}
ChartPublicContext ChartPublicContextSource::snapshot(qint64 asOf)const{auto result=d_->data;const auto at=asOf?asOf:d_->now();const bool metadataStale=!d_->contractsAt||d_->contractsAt>at||at-d_->contractsAt>=180000;if(metadataStale){result.futuresSupported=false;result.liquidation={};}if(result.liquidation.throughMs>at)result.liquidation={};for(auto* c:{&result.funding,&result.oiChange,&result.priceChange,&result.topPosition,&result.takerBuy})if(c->available&&(metadataStale||c->receivedAtMs>at||at-c->receivedAtMs>180000)){c->available=false;c->reason="지연";}if(metadataStale||result.oiChange.receivedAtMs>at||at-result.oiChange.receivedAtMs>180000){result.openInterest.clear();result.openInterestUnit.clear();}return result;}
void ChartPublicContextSource::pump(){d_->pump();}
}
