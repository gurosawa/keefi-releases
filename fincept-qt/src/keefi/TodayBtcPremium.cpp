#include "keefi/TodayBtcPremium.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include "keefi/WorldMarkets.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLocale>
#include <QPointer>
#include <QTimeZone>
#include <QTimer>
#include <array>
#include <cmath>
#include <optional>

namespace keefi {
namespace {
QString name(TodayBtcPremium::Read read) {
    switch (read) {
    case TodayBtcPremium::Read::UpbitBtc: return QStringLiteral("업비트 BTC 원화가");
    case TodayBtcPremium::Read::BinanceBtc: return QStringLiteral("바이낸스 BTC 현물가");
    case TodayBtcPremium::Read::Fx: return QStringLiteral("원·달러 환율");
    }
    return {};
}
bool freshQuote(const AssetQuote& q, qint64 now) {
    const auto& e=q.evidence;
    return decimal::positive(q.price) && e.complete && e.sourceTimeMs>0 && e.sourceTimeMs<=now &&
        now-e.sourceTimeMs<=TodayBtcPremium::QuoteFreshnessMs && e.receivedAtMs>0 &&
        e.receivedAtMs<=now && now-e.receivedAtMs<=TodayBtcPremium::QuoteFreshnessMs;
}
QDate kstDate(qint64 now) { return QDateTime::fromMSecsSinceEpoch(now,QTimeZone("Asia/Seoul")).date(); }
qint64 retryDelay(const HttpResult& r,qint64 now) {
    qint64 delay=qMax<qint64>(60000,qint64(r.localRetryAfterSeconds)*1000);
    for(auto i=r.headers.cbegin();i!=r.headers.cend();++i)if(i.key().toLower()=="retry-after"){
        bool ok=false;const auto seconds=i.value().trimmed().toLongLong(&ok);
        if(ok&&seconds>=0)delay=qMax(delay,qMin<qint64>(86400000,seconds>86400?86400000:seconds*1000));
        else {
            const auto header=QString::fromLatin1(i.value()).trimmed();
            // Retry-After uses HTTP IMF-fixdate with literal GMT. Qt's RFC2822
            // parser does not consistently accept that zone on every runtime.
            const auto http=QLocale::c().toDateTime(header,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");
            const auto at=http.isValid()?QDateTime(http.date(),http.time(),QTimeZone("UTC")):QDateTime::fromString(header,Qt::RFC2822Date);
            if(at.isValid())delay=qMax(delay,qBound<qint64>(qint64(0),at.toMSecsSinceEpoch()-now,qint64(86400000)));}
    }
    return qMin<qint64>(delay,86400000);
}
// The exchange time parseQuote checks, read the same way; 0 when absent or malformed.
// It only decides whether a reply waits for the local clock. Validity stays with parseQuote.
qint64 replyTime(TodayBtcPremium::Read read,const QByteArray& body){
    if(body.size()>65536)return 0;
    const auto doc=parseExactJson(body);const bool upbit=read==TodayBtcPremium::Read::UpbitBtc;
    const auto row=upbit?(doc.isArray()&&doc.array().size()==1?doc.array().first().toObject():QJsonObject{}):doc.object();
    bool ok=false;const auto time=row.value(upbit?"timestamp":"closeTime").toString().toLongLong(&ok);
    return ok&&time>0?time:0;
}
// "2.1초", "11초", "3분": rounded up, so a lead is never understated.
QString leadText(qint64 ms){
    if(ms<10000)return QString::number(double((ms+99)/100)/10.0,'f',1)+QStringLiteral("초");
    if(ms<120000)return QString::number((ms+999)/1000)+QStringLiteral("초");
    return QString::number((ms+59999)/60000)+QStringLiteral("분");
}
}
struct TodayBtcPremium::Private {
    struct Lane { QPointer<TradingTransport> transport; quint64 token=0; qint64 started=0,nextDue=0; QString failure;
        // A BTC reply whose exchange time is ahead of the local clock by at most
        // ClockLeadHoldMs, kept unparsed with its first receipt time.
        std::optional<HttpResult> held; qint64 heldReceivedAt=0,heldUntil=0,heldSince=0; };
    std::array<Lane,3> lanes;
    Clock clock,elapsed;
    QElapsedTimer monotonic;
    QTimer timer,hold;
    bool active=false;
    quint64 nextToken=1;
    AssetQuote upbit,binance;
    UsdKrwPosting fx;
    TodayBtcPremiumSnapshot good;
    QString unavailable(qint64 now) const {
        // Report the missing daily source explicitly, even when other public
        // widgets also have not completed their first reads.
        if(fx.krwPerUsd.isEmpty())return lanes[2].failure.isEmpty()?QStringLiteral("원·달러 환율 수신 전"):lanes[2].failure;
        if(upbit.price.isEmpty())return lanes[0].failure.isEmpty()?QStringLiteral("업비트 BTC 원화가 수신 전"):lanes[0].failure;
        if(binance.price.isEmpty())return lanes[1].failure.isEmpty()?QStringLiteral("바이낸스 BTC 현물가 수신 전"):lanes[1].failure;
        for(int i=0;i<3;++i){const auto& lane=lanes[i];if(!lane.failure.isEmpty())return lane.failure;
            if(lane.token&&now-lane.started>=LateMs)return name(Read(i))+QStringLiteral(" 응답 지연");}
        if(!freshQuote(upbit,now))return QStringLiteral("업비트 BTC 시세 지연");
        if(!freshQuote(binance,now))return QStringLiteral("바이낸스 BTC 시세 지연");
        if(qAbs(upbit.evidence.sourceTimeMs-binance.evidence.sourceTimeMs)>MaximumQuoteSkewMs)return QStringLiteral("BTC 두 시세의 기준 시각 차이 30초 초과");
        const auto age=fx.postedDate.daysTo(kstDate(now));
        if(!fx.postedDate.isValid()||age<0||age>MaximumFxCalendarAgeDays)return QStringLiteral("원·달러 고시일 확인 필요");
        if(fx.receivedAtMs<=0||fx.receivedAtMs>now||now-fx.receivedAtMs>FxReceiptFreshnessMs)return QStringLiteral("원·달러 환율 재확인 지연");
        return {};
    }
};
TodayBtcPremium::TodayBtcPremium(TradingTransport* upbit,TradingTransport* binance,TradingTransport* fx,QObject* parent,Clock clock,Clock elapsed)
    :QObject(parent),d_(std::make_unique<Private>()) {
    d_->clock=clock?std::move(clock):Clock([]{return QDateTime::currentMSecsSinceEpoch();});
    d_->monotonic.start();
    d_->elapsed=elapsed?std::move(elapsed):Clock([d=d_.get()]{return d->monotonic.elapsed();});
    const std::array<TradingTransport*,3> transports{upbit,binance,fx};
    for(int i=0;i<3;++i){d_->lanes[i].transport=transports[i];if(!transports[i])continue;
        connect(transports[i],&TradingTransport::finished,this,[this,i](quint64 token,const HttpResult& response){
            auto& lane=d_->lanes[i];if(!d_->active||!token||lane.token!=token)return;
            lane.token=0;lane.started=0;const auto now=d_->clock();const auto read=Read(i);
            const bool ok=response.status==200&&!response.networkError&&!response.timedOut&&response.localRetryAfterSeconds<=0;
            if(ok&&read!=Read::Fx){
                // Stamped slightly ahead of this computer's clock: wait, unparsed and unshown,
                // instead of failing into the minute-long retry.
                const auto lead=replyTime(read,response.body)-now;
                if(lead>0&&lead<=ClockLeadHoldMs){lane.held=response;lane.heldReceivedAt=now;lane.heldUntil=now+lead;lane.heldSince=d_->elapsed();releaseHeld();return;}
            }
            settle(i,response,now,now);
        });
    }
    d_->timer.setInterval(1000);connect(&d_->timer,&QTimer::timeout,this,[this]{refresh();emit changed();});
    d_->hold.setSingleShot(true);connect(&d_->hold,&QTimer::timeout,this,&TodayBtcPremium::releaseHeld);
}
void TodayBtcPremium::settle(int i,const HttpResult& response,qint64 now,qint64 receivedAt){
    auto& lane=d_->lanes[i];const auto read=Read(i);
    bool ok=response.status==200&&!response.networkError&&!response.timedOut&&response.localRetryAfterSeconds<=0;
    QString parseReason;
    if(ok){if(read==Read::Fx){UsdKrwPosting next;ok=parseFxPosting(response.body,now,&next,&parseReason);
            if(ok){if(d_->fx.postedAtMs>0&&next.postedAtMs<d_->fx.postedAtMs){ok=false;parseReason=QStringLiteral("원·달러 고시 시각이 이전 응답보다 과거예요");}else d_->fx=next;}}
        else {AssetQuote next;ok=parseQuote(read,response.body,now,&next);
            if(ok){next.evidence.receivedAtMs=receivedAt; // A held reply keeps its first receipt time.
                auto& current=read==Read::UpbitBtc?d_->upbit:d_->binance;
                if(next.evidence.sourceTimeMs<current.evidence.sourceTimeMs){ok=false;parseReason=name(read)+QStringLiteral(" 기준 시각이 이전 응답보다 과거예요");}else current=next;}
            // Name the clock gap only when it is the sole fault: the same reply passes once
            // the local clock reaches its time.
            else if(const auto lead=replyTime(read,response.body)-now;lead>0&&parseQuote(read,response.body,now+lead,&next))
                parseReason=name(read)+QStringLiteral(" 시각이 이 컴퓨터 시계보다 %1 앞서요 · 시간 설정 확인").arg(leadText(lead));}}
    const bool limited=response.status==429||response.status==418||response.localRetryAfterSeconds>0;
    lane.failure=ok?QString{}:limited?name(read)+QStringLiteral(" 요청 제한 · 재시도 대기"):
        !parseReason.isEmpty()?parseReason:name(read)+QStringLiteral(" 수신 실패 또는 검증 실패");
    lane.nextDue=now+(ok?(read==Read::Fx?FxRefreshMs:QuoteRefreshMs):retryDelay(response,now));
    if(d_->unavailable(now).isEmpty()){
        const auto value=percent(d_->upbit.price,d_->binance.price,d_->fx.krwPerUsd);
        if(!value.isEmpty())d_->good={TodayBtcPremiumSnapshot::State::Current,value,{},now,d_->upbit,d_->binance,d_->fx};
    }
    emit changed();
}
void TodayBtcPremium::releaseHeld(){
    // Each clock is read once (as in the pacing-timer fix), so the wait below cannot turn
    // negative between the check and the timer start.
    const auto now=d_->clock();const auto mono=d_->elapsed();qint64 next=0;
    for(int i=0;i<2;++i){auto& lane=d_->lanes[i];if(!d_->active)return;if(!lane.held)continue;
        // Until the local clock reaches the reply's time, never past ClockLeadHoldMs of
        // monotonic time. Expiry does not make the time valid: parseQuote still decides.
        if(const auto wait=qMin(lane.heldUntil-now,ClockLeadHoldMs-(mono-lane.heldSince));wait>0){next=next>0?qMin(next,wait):wait;continue;}
        const auto reply=std::move(*lane.held);lane.held.reset();settle(i,reply,now,lane.heldReceivedAt);
    }
    if(!d_->active)return;
    if(next>0)d_->hold.start(int(next));else d_->hold.stop();
}
TodayBtcPremium::~TodayBtcPremium(){setActive(false);}
void TodayBtcPremium::setActive(bool active){
    if(d_->active==active)return;d_->active=active;
    if(active){d_->timer.start();refresh();}
    else {d_->timer.stop();d_->hold.stop();for(auto& lane:d_->lanes){const bool inFlight=lane.token!=0;lane.token=0;lane.started=0;lane.held.reset();if(inFlight&&lane.transport)lane.transport->abort();}}
    emit changed();
}
bool TodayBtcPremium::active()const{return d_->active;}
void TodayBtcPremium::refresh(){
    if(!d_->active)return;releaseHeld();if(!d_->active)return;const auto now=d_->clock();
    for(int i=0;i<3;++i){auto& lane=d_->lanes[i];if(!lane.transport||lane.token||lane.held||now<lane.nextDue)continue;
        lane.token=d_->nextToken++;lane.started=now;lane.transport->send(lane.token,request(Read(i)));}
}
TodayBtcPremiumSnapshot TodayBtcPremium::snapshot(qint64 now)const{
    if(!now)now=d_->clock();const auto reason=d_->unavailable(now);
    if(reason.isEmpty()){
        const auto value=percent(d_->upbit.price,d_->binance.price,d_->fx.krwPerUsd);
        if(!value.isEmpty())return {TodayBtcPremiumSnapshot::State::Current,value,{},d_->good.checkedAtMs,d_->upbit,d_->binance,d_->fx};
    }
    auto result=d_->good;result.state=result.percent.isEmpty()?TodayBtcPremiumSnapshot::State::Waiting:TodayBtcPremiumSnapshot::State::Recent;
    if(result.state==TodayBtcPremiumSnapshot::State::Waiting){result.upbit=d_->upbit;result.binance=d_->binance;result.fx=d_->fx;}
    result.reason=reason.isEmpty()?QStringLiteral("BTC 김프 계산 입력 확인 필요"):reason;return result;
}
RequestSpec TodayBtcPremium::request(Read read){
    RequestSpec r;r.operation=Operation::Markets;
    switch(read){
    case Read::UpbitBtc:r.url=QUrl("https://api.upbit.com/v1/ticker?markets=KRW-BTC");break;
    case Read::BinanceBtc:r.url=QUrl("https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT");break;
    case Read::Fx:return WorldMarkets::request(WorldMarketItem::UsdKrw); // The Today board's exact Hana posting read.
    default:return {};
    }
    r.headers.insert("Accept","application/json");return r;
}
bool TodayBtcPremium::isRequest(const RequestSpec& r){
    if(r.operation!=Operation::Markets||r.method!="GET"||r.authenticated||!r.body.isEmpty())return false;
    for(auto read:{Read::UpbitBtc,Read::BinanceBtc,Read::Fx}){const auto expected=request(read);if(r.url==expected.url&&r.headers==expected.headers)return true;}return false;
}
QString TodayBtcPremium::percent(const QString& upbit,const QString& binance,const QString& fx){
    // Fixed display basis: one USDT is treated as one USD. The legacy tether
    // bridge metric has its own sourceKey, histories, and alert engine.
    return MarketMetrics::priceGapPercent(upbit,binance,fx);
}
bool TodayBtcPremium::parseQuote(Read read,const QByteArray& body,qint64 now,AssetQuote* out){
    if(!out||now<=0||body.size()>65536||(read!=Read::UpbitBtc&&read!=Read::BinanceBtc))return false;
    const auto doc=parseExactJson(body);QJsonObject row;
    if(read==Read::UpbitBtc){if(!doc.isArray()||doc.array().size()!=1)return false;row=doc.array().first().toObject();if(row.value("market").toString()!="KRW-BTC")return false;}
    else {if(!doc.isObject())return false;row=doc.object();if(row.value("symbol").toString()!="BTCUSDT")return false;}
    const auto price=decimal::normalize(row.value(read==Read::UpbitBtc?"trade_price":"lastPrice").toString());
    bool timeOk=false;const auto time=row.value(read==Read::UpbitBtc?"timestamp":"closeTime").toString().toLongLong(&timeOk);
    if(!decimal::positive(price)||price.size()>40||!timeOk||time<=0||time>now||now-time>QuoteFreshnessMs)return false;
    *out={"BTC",read==Read::UpbitBtc?"KRW":"USDT",price,{read==Read::UpbitBtc?QStringLiteral("업비트"):QStringLiteral("Binance"),request(read).url.toString(),
        read==Read::UpbitBtc?QStringLiteral("today:btc:upbit:KRW-BTC"):QStringLiteral("today:btc:binance:BTCUSDT"),time,now,true}};return true;
}
bool TodayBtcPremium::parseFxPosting(const QByteArray& body,qint64 now,UsdKrwPosting* out,QString* reason){
    const auto fail=[&](const QString& why){if(reason)*reason=why;return false;};
    if(!out||now<=0)return fail(QStringLiteral("원·달러 환율 응답 형식 확인 필요"));
    // The world-market parser checks the identity (FX_USDKRW, exchange), a positive grouped
    // price, the provider clock with its offset and agreeing signs; nothing is guessed.
    WorldMarketQuote quote;QString why;
    if(!WorldMarkets::parseQuote(WorldMarketItem::UsdKrw,body,now,&quote,&why))return fail(why.isEmpty()?QStringLiteral("원·달러 환율 응답 형식 확인 필요"):why);
    bool ok=false;const auto rate=quote.price.toDouble(&ok);
    if(!ok||!std::isfinite(rate)||rate<500||rate>5000)return fail(QStringLiteral("원·달러 환율 범위 확인 필요"));
    if(quote.tradedAtMs>now)return fail(QStringLiteral("원·달러 고시 시각 확인 필요"));
    const auto posted=QDateTime::fromMSecsSinceEpoch(quote.tradedAtMs,QTimeZone("Asia/Seoul")).date();
    const auto age=posted.daysTo(kstDate(now));
    if(age<0||age>MaximumFxCalendarAgeDays)return fail(QStringLiteral("원·달러 고시일 확인 필요"));
    *out={decimal::normalize(quote.price),quote.tradedAtMs,posted,now};if(reason)reason->clear();return true;
}
} // namespace keefi
