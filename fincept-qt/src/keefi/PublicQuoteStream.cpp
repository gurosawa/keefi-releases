#include "keefi/PublicQuoteStream.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <cmath>

namespace keefi {
namespace {
constexpr int MaxMessage=65536;
QString binanceRollingChangeRate(const QString& close,const QString& open){
    if(!decimal::positive(close)||!decimal::positive(open)||close.trimmed()!=close||open.trimmed()!=open)return {};
    bool closeOk=false,openOk=false;
    const auto closing=close.toDouble(&closeOk),opening=open.toDouble(&openOk);
    if(!closeOk||!openOk||!std::isfinite(closing)||!std::isfinite(opening)||closing<=0||opening<=0)return {};
    // Subtract the original decimals before the display-only division, avoiding
    // cancellation for a small change without modifying quote or valuation data.
    const bool negative=decimal::compare(close,open)<0;bool differenceOk=false;
    const auto difference=(negative?decimal::subtract(open,close):decimal::subtract(close,open)).toDouble(&differenceOk);
    if(!differenceOk||!std::isfinite(difference))return {};
    const auto rate=(negative?-difference:difference)/opening;
    return std::isfinite(rate)?QString::number(rate,'g',16):QString{};
}
class NetworkPublicSocket final : public PublicTickerSocket {
public:
    void open(const QUrl& url) override {
        abort();const auto epoch=epoch_;
        auto* s=new QWebSocket(QString{},QWebSocketProtocol::VersionLatest,this);socket_=s;
        s->setProxy(QNetworkProxy::NoProxy);s->setReadBufferSize(MaxMessage);
        s->setMaxAllowedIncomingFrameSize(MaxMessage);s->setMaxAllowedIncomingMessageSize(MaxMessage);
        auto tls=QSslConfiguration::defaultConfiguration();tls.setPeerVerifyMode(QSslSocket::VerifyPeer);s->setSslConfiguration(tls);
        connect(s,&QWebSocket::connected,this,[this,epoch]{if(epoch==epoch_)emit connected();});
        connect(s,&QWebSocket::textMessageReceived,this,[this,epoch](const QString& data){if(epoch==epoch_)emit received(data.toUtf8());});
        connect(s,&QWebSocket::binaryMessageReceived,this,[this,epoch](const QByteArray& data){if(epoch==epoch_)emit received(data);});
        connect(s,&QWebSocket::pong,this,[this,epoch](quint64,const QByteArray&){if(epoch==epoch_)emit alive();});
        connect(s,&QWebSocket::disconnected,this,[this,epoch]{if(epoch==epoch_)emit failed();});
        connect(s,&QWebSocket::errorOccurred,this,[this,epoch](QAbstractSocket::SocketError){if(epoch==epoch_)emit failed();});
        connect(s,&QWebSocket::sslErrors,this,[this,epoch](const QList<QSslError>&){if(epoch==epoch_)emit failed();});
        QNetworkRequest request(url);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);s->open(request);
    }
    void send(const QByteArray& data) override {if(socket_)socket_->sendTextMessage(QString::fromUtf8(data));}
    void ping() override {if(socket_)socket_->ping();}
    void abort() override {++epoch_;if(socket_){auto* old=socket_.data();socket_=nullptr;old->abort();old->deleteLater();}}
private:
    quint64 epoch_=0;QPointer<QWebSocket> socket_;
};
}
struct PublicQuoteStream::Private {
    PublicQuoteStream* q;Provider provider;std::unique_ptr<PublicTickerSocket> socket;
    QStringList markets,subscribed;QMap<QString,PublicTick> pending,last;
    QTimer flush,reconnect,health,subscriptions;QElapsedTimer clock;
    QString message;bool connected=false,connecting=false;int attempts=0;
    qint64 heardAt=0,openedAt=0,pingAt=0;
    quint64 streamEpoch=0;
    Private(PublicQuoteStream* owner,Provider p,std::unique_ptr<PublicTickerSocket> s):q(owner),provider(p),socket(std::move(s)){clock.start();reconnect.setSingleShot(true);flush.setInterval(1000);flush.setTimerType(Qt::PreciseTimer);health.setInterval(1000);subscriptions.setSingleShot(true);subscriptions.setInterval(750);}
    void state(const QString& value){if(message!=value){message=value;emit q->changed();}}
    QString name()const{return provider==Provider::Upbit?QString("업비트"):QString("Binance");}
    void open(){
        if(markets.isEmpty())return;
        ++streamEpoch;connecting=true;connected=false;heardAt=openedAt=pingAt=clock.elapsed();
        state(name()+" 실시간 시세 연결 중");
        socket->open(QUrl(provider==Provider::Upbit?"wss://api.upbit.com/websocket/v1":"wss://stream.binance.com:9443/ws"));
        health.start();flush.start();
    }
    void syncBinance(){
        if(!connected)return;
        const auto sendDiff=[this](const QString& method,const QStringList& from,const QStringList& except){
            QJsonArray codes;
            for(const auto& market:from)if(!except.contains(market))codes.append(market.toLower()+"@miniTicker");
            if(!codes.isEmpty())socket->send(QJsonDocument(QJsonObject{{"method",method},{"params",codes},{"id","keefi-public"}}).toJson(QJsonDocument::Compact));
        };
        sendDiff("UNSUBSCRIBE",subscribed,markets);
        sendDiff("SUBSCRIBE",markets,subscribed);
        subscribed=markets;
    }
    void ready(){
        if(!connecting||markets.isEmpty())return;connecting=false;connected=true;heardAt=clock.elapsed();
        if(provider==Provider::Binance){subscribed.clear();syncBinance();}
        else{QJsonArray codes;for(const auto& market:markets)codes.append(market);
            socket->send(QJsonDocument(QJsonArray{QJsonObject{{"ticket",QUuid::createUuid().toString(QUuid::WithoutBraces)}},QJsonObject{{"type","ticker"},{"codes",codes}},QJsonObject{{"format","DEFAULT"}}}).toJson(QJsonDocument::Compact));}
        state(name()+" 시세 수신 대기 · 화면 1초 갱신");
    }
    void fail(){
        if(markets.isEmpty()||reconnect.isActive())return;
        connected=connecting=false;subscriptions.stop();subscribed.clear();socket->abort();pending.clear();health.stop();
        const int delay=qMin(60000,1000*(1<<qMin(attempts++,6)));
        state(name()+QString(" 시세 연결 끊김 · %1초 후 재연결 · 마지막 가격 시각 유지").arg(delay/1000));reconnect.start(delay);
    }
    void receive(const QByteArray& data){
        if(!connected||data.size()>MaxMessage)return;
        const auto doc=parseExactJson(data);if(!doc.isObject())return;const auto o=doc.object();
        if(provider==Provider::Binance&&o.value("id").toString()=="keefi-public"){
            if(!o.contains("result")||!o.value("result").isNull())fail();return;
        }
        const bool upbit=provider==Provider::Upbit;
        if(o.value(upbit?"type":"e").toString()!=(upbit?"ticker":"24hrMiniTicker"))return;
        PublicTick tick;tick.market=o.value(upbit?"code":"s").toString();
        if(!markets.contains(tick.market))return;
        tick.streamEpoch=streamEpoch;
        auto& quote=tick.quote;quote.baseAsset=upbit?tick.market.mid(4):tick.market.chopped(4);quote.quoteAsset=upbit?"KRW":"USDT";
        quote.price=o.value(upbit?"trade_price":"c").toString();
        bool ok=false;const auto source=o.value(upbit?"timestamp":"E").toString().toLongLong(&ok),now=QDateTime::currentMSecsSinceEpoch();
        if(!ok||source<=0||source>now+2000||!decimal::positive(quote.price))return;
        quote.evidence={upbit?"업비트 공식 KRW 현재가":"Binance 공식 USDT 현재가",upbit?"wss://api.upbit.com/websocket/v1":"wss://stream.binance.com:9443/ws",
            (upbit?"public:upbit:":"public:binance:")+tick.market,source,now,true};
        if(upbit){
            tick.previousClose=o.value("prev_closing_price").toString();tick.signedChangeRate=o.value("signed_change_rate").toString();
            {const auto value=o.value("acc_trade_price_24h").toString();if(decimal::positive(value))tick.tradeValue24h=value;}
            // The UTC-day cumulative quote counter is a separate observation
            // contract. Missing/invalid values do not borrow rolling 24h volume.
            {const auto value=o.value("acc_trade_price").toString();if(decimal::valid(value))tick.tradeValueUtcDay=value;}
            tick.realtime=o.value("stream_type").toString()=="REALTIME";
            tick.tradeDateUtc=o.value("trade_date").toString();
            bool tradeTimeOk=false;const auto tradeTime=o.value("trade_timestamp").toString().toLongLong(&tradeTimeOk);
            if(tradeTimeOk&&tradeTime>0&&tradeTime<=source+2000)tick.tradeTimestampMs=tradeTime;
            static const QRegularExpression rate("\\A-?[0-9]{1,40}(?:\\.[0-9]{1,40})?(?:[eE][-+]?[0-9]{1,3})?\\z");
            if(!decimal::positive(tick.previousClose)||!rate.match(tick.signedChangeRate).hasMatch())return;
        }else tick.signedChangeRate=binanceRollingChangeRate(quote.price,o.value("o").toString());
        heardAt=clock.elapsed();attempts=0;
        const auto old=last.constFind(tick.market);
        if(old!=last.cend()&&source<=old->quote.evidence.sourceTimeMs){
            if(source==old->quote.evidence.sourceTimeMs&&decimal::normalize(quote.price)!=decimal::normalize(old->quote.price))fail();
            return; // Duplicate/out-of-order data must not renew freshness.
        }
        last[tick.market]=tick;pending[tick.market]=tick;
    }
};
PublicQuoteStream::PublicQuoteStream(Provider p,QObject* parent):PublicQuoteStream(p,std::make_unique<NetworkPublicSocket>(),parent){}
PublicQuoteStream::PublicQuoteStream(Provider p,std::unique_ptr<PublicTickerSocket> s,QObject* parent):QObject(parent),d_(std::make_unique<Private>(this,p,std::move(s))){
    connect(d_->socket.get(),&PublicTickerSocket::connected,this,[this]{d_->ready();});
    connect(d_->socket.get(),&PublicTickerSocket::received,this,[this](const QByteArray& data){d_->receive(data);});
    connect(d_->socket.get(),&PublicTickerSocket::alive,this,[this]{if(d_->connected)d_->heardAt=d_->clock.elapsed();});
    connect(d_->socket.get(),&PublicTickerSocket::failed,this,[this]{d_->fail();});
    connect(&d_->reconnect,&QTimer::timeout,this,[this]{d_->open();});
    connect(&d_->subscriptions,&QTimer::timeout,this,[this]{d_->syncBinance();});
    connect(&d_->flush,&QTimer::timeout,this,[this]{if(d_->pending.isEmpty())return;const auto data=d_->pending.values();d_->pending.clear();
        d_->state(d_->name()+" 실시간 시세 · 화면 1초 갱신");emit ticks(data);});
    connect(&d_->health,&QTimer::timeout,this,[this]{const auto now=d_->clock.elapsed();
        if((d_->connecting&&now-d_->openedAt>10000)||now-d_->heardAt>45000){d_->fail();return;}
        if(d_->connected&&now-d_->pingAt>=15000){d_->pingAt=now;d_->socket->ping();}
    });
}
PublicQuoteStream::~PublicQuoteStream(){d_->socket->abort();}
void PublicQuoteStream::setMarkets(QStringList markets){
    markets.removeDuplicates();markets.sort();
    static const QRegularExpression upbitCode(QStringLiteral("\\AKRW-[A-Z0-9]{1,24}\\z"));
    if(markets.size()>1000||std::any_of(markets.cbegin(),markets.cend(),[&](const QString& m){
        return d_->provider==Provider::Upbit ? !upbitCode.match(m).hasMatch() : !binanceUsdtTickerSymbolValid(m);
    }))return;
    if(markets==d_->markets)return;
    d_->markets=markets;
    if(d_->provider==Provider::Binance&&!markets.isEmpty()&&(d_->connected||d_->connecting||d_->reconnect.isActive())){
        d_->pending.removeIf([&](QMap<QString,PublicTick>::iterator it){return !markets.contains(it.key());});
        d_->last.removeIf([&](QMap<QString,PublicTick>::iterator it){return !markets.contains(it.key());});
        if(d_->connected&&!d_->subscriptions.isActive())d_->subscriptions.start();
        return;
    }
    d_->subscriptions.stop();d_->subscribed.clear();d_->connected=d_->connecting=false;d_->socket->abort();d_->pending.clear();d_->last.clear();
    d_->reconnect.stop();d_->health.stop();d_->flush.stop();d_->attempts=0;
    if(markets.isEmpty()){d_->state({});return;}
    // Coalesce subscription changes across accounts and cap reconnect churn.
    d_->reconnect.start(250);
}
QString PublicQuoteStream::status()const{return d_->message;}
quint64 PublicQuoteStream::streamEpoch()const{return d_->streamEpoch;}
}
