#include "keefi/NetworkPublicFlowSocket.h"
#include "keefi/MarketCandles.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/PublicFlowFrameQueue.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
namespace keefi {
namespace {
bool wireSymbol(const QString& symbol){return symbol==symbol.toLower()&&binanceUsdmSymbolValid(symbol.toUpper());}
bool rawStream(const QString& path,const QString& prefix,const QStringList& suffixes){
    if(!path.startsWith(prefix)||path.count('@')!=1)return false;const auto name=path.mid(prefix.size());const auto separator=name.indexOf('@');
    return separator>0&&wireSymbol(name.left(separator))&&suffixes.contains(name.mid(separator+1));
}
}
bool isPublicFlowSocketEndpoint(const QUrl& url){
    if(url==QUrl("wss://api.upbit.com/websocket/v1"))return true;
    if(!url.isValid()||url.scheme()!="wss"||!url.userInfo().isEmpty()||url.hasFragment())return false;
    if(url.host()=="stream.binance.com")return url.port()==9443&&!url.hasQuery()&&rawStream(url.path(),"/ws/",{"aggTrade"});
    if(url.host()!="fstream.binance.com"||url.port(443)!=443)return false;
    if(!url.hasQuery())return rawStream(url.path(),"/market/ws/",{"aggTrade","forceOrder"});
    if(url.path()!="/market/stream")return false;const auto fields=QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if(fields.size()!=1||fields.first().first!="streams")return false;const auto streams=fields.first().second.split('/');
    if(streams.size()!=2||!streams.first().endsWith("@aggTrade"))return false;const auto symbol=streams.first().chopped(9);
    return wireSymbol(symbol)&&streams.last()==symbol+"@forceOrder";
}
namespace {
class NetworkPublicFlowSocket final:public PublicCandleSocket {
  public:
    NetworkPublicFlowSocket(){clock_.start();delivery_.setSingleShot(true);delivery_.setTimerType(Qt::PreciseTimer);connect(&delivery_,&QTimer::timeout,this,[this]{deliver();});}
    ~NetworkPublicFlowSocket()override{abort();}
    void open(quint64 generation,const QUrl& endpoint)override{
        abort();if(!generation||!isPublicFlowSocketEndpoint(endpoint)){emit failed(generation);return;}
        generation_=generation;const auto wire=wire_;auto* socket=new QWebSocket(QString{},QWebSocketProtocol::VersionLatest,this);socket_=socket;
        socket->setProxy(QNetworkProxy::NoProxy);socket->setReadBufferSize(65536);socket->setMaxAllowedIncomingFrameSize(65536);socket->setMaxAllowedIncomingMessageSize(65536);
        auto tls=QSslConfiguration::defaultConfiguration();tls.setPeerVerifyMode(QSslSocket::VerifyPeer);socket->setSslConfiguration(tls);
        connect(socket,&QWebSocket::connected,this,[this,wire,generation]{if(wire==wire_)emit connected(generation);});
        connect(socket,&QWebSocket::textMessageReceived,this,[this,wire](const QString& message){if(wire==wire_)accept(message.toUtf8());});
        connect(socket,&QWebSocket::binaryMessageReceived,this,[this,wire](const QByteArray& message){if(wire==wire_)accept(message);});
        connect(socket,&QWebSocket::disconnected,this,[this,wire]{if(wire==wire_)fail();});
        connect(socket,&QWebSocket::errorOccurred,this,[this,wire](QAbstractSocket::SocketError){if(wire==wire_)fail();});
        connect(socket,&QWebSocket::sslErrors,this,[this,wire](const QList<QSslError>&){if(wire==wire_)fail();});
        // Qt automatically replies to WebSocket ping control frames. No periodic
        // application messages are invented; each provider owns its heartbeat.
        QNetworkRequest request(endpoint);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);socket->open(request);
    }
    void send(quint64 generation,const QByteArray& message)override{
        if(generation==generation_&&socket_&&message.size()<=65536)socket_->sendTextMessage(QString::fromUtf8(message));
    }
    void abort()override{++wire_;generation_=0;delivery_.stop();pending_.clear();if(socket_){auto* previous=socket_.data();socket_=nullptr;previous->abort();previous->deleteLater();}}
  private:
    void fail(){const auto generation=generation_;abort();if(generation)emit failed(generation);}
    void accept(const QByteArray& frame){
        if(!pending_.push(frame,QDateTime::currentMSecsSinceEpoch(),clock_.elapsed())){fail();return;}
        deliver();
    }
    void deliver(){
        const auto wire=wire_,generation=generation_;
        const auto ready=pending_.takeReady(QDateTime::currentMSecsSinceEpoch(),clock_.elapsed());
        for(const auto& frame:ready){if(wire!=wire_||!generation)return;emit received(generation,frame);}
        const auto wait=pending_.waitMs(QDateTime::currentMSecsSinceEpoch(),clock_.elapsed());
        if(wait>=0)delivery_.start(qMax(1,wait));else delivery_.stop();
    }
    QPointer<QWebSocket> socket_;quint64 wire_=0,generation_=0;
    PublicFlowFrameQueue pending_;QElapsedTimer clock_;QTimer delivery_;
};
}
std::unique_ptr<PublicCandleSocket> makeNetworkPublicFlowSocket(){return std::make_unique<NetworkPublicFlowSocket>();}
}
