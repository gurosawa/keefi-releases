#include "keefi/CoinbaseQuotes.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
namespace keefi {
namespace {
bool valid(const QString& base){return base!="USD"&&QRegularExpression("\\A[A-Z0-9]{1,20}\\z").match(base).hasMatch();}
}
struct CoinbaseQuotes::Private {
    QPointer<TradingTransport> transport,bridge;
    QString asset;
    QStringList pending,missing;
    QMap<QString,AssetQuote> values;
    struct Read { quint64 token; QString base; };
    QHash<TradingTransport*,Read> active;
    QTimer timer,retry;
    quint64 sequence=1;
    qint64 retryAt=0;
    void next(){
        const auto wait=retryAt-QDateTime::currentMSecsSinceEpoch();
        if(wait>0){if(!retry.isActive())retry.start(int(wait));return;}
        for(const auto& base:QStringList(pending)){
            auto* wire=(base=="USDT"&&bridge)?bridge.data():transport.data();
            if(!wire||active.contains(wire)||!pending.contains(base))continue;
            pending.removeOne(base);
            const auto token=sequence++;
            active.insert(wire,{token,base});
            wire->send(token,CoinbaseQuotes::request(base));
        }
    }
    void enqueue(bool keepBridge=false){
        if(asset.isEmpty()||!active.isEmpty()||!pending.isEmpty())return;
        pending={asset};
        if(asset!="USDT"&&!keepBridge)pending.append("USDT");
        next();
    }
};
CoinbaseQuotes::CoinbaseQuotes(TradingTransport* transport,QObject* parent,TradingTransport* bridgeTransport)
    :QObject(parent),d_(std::make_unique<Private>()){
    d_->transport=transport;d_->bridge=bridgeTransport;
    d_->timer.setInterval(30000);d_->retry.setSingleShot(true);
    connect(&d_->timer,&QTimer::timeout,this,[this]{d_->enqueue();});
    connect(&d_->retry,&QTimer::timeout,this,[this]{if(d_->pending.isEmpty())d_->enqueue();else d_->next();});
    for(auto* wire:{transport,bridgeTransport}){
        if(!wire)continue;
        connect(wire,&TradingTransport::finished,this,[this,wire](quint64 token,const HttpResult& result){
            const auto read=d_->active.value(wire);
            if(!token||token!=read.token)return;
            d_->active.remove(wire);
            if(result.status==429||result.status==418||result.localRetryAfterSeconds>0){
                int seconds=result.localRetryAfterSeconds;
                for(auto it=result.headers.cbegin();it!=result.headers.cend();++it)if(it.key().compare("Retry-After",Qt::CaseInsensitive)==0)seconds=qMax(seconds,it.value().toInt());
                seconds=qBound(1,seconds>0?seconds:60,86400);
                d_->retryAt=qMax(d_->retryAt,QDateTime::currentMSecsSinceEpoch()+seconds*1000LL);
                d_->retry.start(int(d_->retryAt-QDateTime::currentMSecsSinceEpoch()));
            }
            const auto value=parse(read.base,result,QDateTime::currentMSecsSinceEpoch());
            d_->missing.removeAll(read.base);
            if(result.status==404&&!result.timedOut)d_->missing.append(read.base);
            if(value.price.isEmpty())d_->values.remove(read.base);else d_->values.insert(read.base,value);
            emit quoteObserved(read.base);emit changed();d_->next();
        });
    }
}
CoinbaseQuotes::~CoinbaseQuotes(){setAsset({});}
bool CoinbaseQuotes::unlisted(const QString& base) const{return d_->missing.contains(base);}
void CoinbaseQuotes::setAsset(const QString& value){
    const auto asset=valid(value)?value:QString{};if(asset==d_->asset)return;
    const auto bridge=quotes().value("USDT");
    const auto wires=d_->active.keys();d_->active.clear();d_->pending.clear();
    for(auto* wire:wires)wire->abort();
    d_->asset=asset;d_->values.clear();d_->missing.clear();
    if(asset.isEmpty()){d_->timer.stop();d_->retry.stop();}
    else{
        const auto now=QDateTime::currentMSecsSinceEpoch();
        const bool reuse=asset!="USDT"&&!bridge.price.isEmpty()&&now-bridge.evidence.sourceTimeMs<20000;
        if(reuse)d_->values.insert("USDT",bridge);
        d_->timer.start();d_->enqueue(reuse);
    }
    emit changed();
}
QMap<QString,AssetQuote> CoinbaseQuotes::quotes(qint64 now)const{
    if(!now)now=QDateTime::currentMSecsSinceEpoch();auto values=d_->values;
    for(auto it=values.begin();it!=values.end();){const auto& e=it.value().evidence;
        if(e.sourceTimeMs>now||e.receivedAtMs>now||now-e.sourceTimeMs>MarketMetrics::FreshnessMs||now-e.receivedAtMs>MarketMetrics::FreshnessMs)it=values.erase(it);else++it;}
    return values;
}
RequestSpec CoinbaseQuotes::request(const QString& base){
    RequestSpec r;r.operation=Operation::Markets;if(!valid(base))return r;
    r.url=QUrl("https://api.exchange.coinbase.com/products/"+base+"-USD/ticker");r.headers.insert("Accept","application/json");return r;
}
bool CoinbaseQuotes::isRequest(const RequestSpec& r){
    const auto match=QRegularExpression("\\A/products/([A-Z0-9]{1,20})-USD/ticker\\z").match(r.url.path());
    if(!match.hasMatch())return false;const auto expected=request(match.captured(1));
    return expected.url.isValid()&&r.url==expected.url&&r.headers==expected.headers&&r.operation==Operation::Markets&&r.method=="GET"&&!r.authenticated&&r.body.isEmpty();
}
AssetQuote CoinbaseQuotes::parse(const QString& base,const HttpResult& r,qint64 received){
    if(!valid(base)||received<=0||r.status!=200||r.networkError||r.timedOut||r.localRetryAfterSeconds>0)return {};
    const auto doc=parseExactJson(r.body);if(!doc.isObject())return {};const auto o=doc.object();
    const auto text=o.value("time").toString();
    if(!QRegularExpression("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,9})?Z\\z").match(text).hasMatch())return {};
    const auto date=QDateTime::fromString(text,Qt::ISODateWithMs);const auto at=date.toMSecsSinceEpoch();const auto price=decimal::normalize(o.value("price").toString());
    if(!date.isValid()||at<=0||at>received||received-at>MarketMetrics::FreshnessMs||!decimal::positive(price))return {};
    return {base,"USD",price,{"Coinbase Exchange 공식 최근 체결",request(base).url.toString(),"public:coinbase:"+base+"-USD",at,received,true}};
}
ExchangePriceGap CoinbaseQuotes::priceGap(const AssetQuote& cb,const AssetQuote& bn,const AssetQuote& bridge,qint64 now){
    if(cb.quoteAsset!="USD"||bridge.quoteAsset!="USD"){ExchangePriceGap out;out.reason="USD/USDT 환산 근거 부족";return out;}
    return MarketMetrics::exchangePriceGap(cb,bn,bridge,now);
}
}
