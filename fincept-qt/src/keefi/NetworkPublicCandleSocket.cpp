#include "keefi/NetworkPublicCandleSocket.h"
#include "keefi/MarketCandles.h"
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QWebSocket>
namespace keefi {
namespace {
bool publicEndpoint(const QUrl& url){
    if(url==QUrl("wss://api.upbit.com/websocket/v1"))return true;
    if(!url.isValid()||url.scheme()!="wss"||url.host()!="stream.binance.com"||url.port()!=9443||
        !url.userName().isEmpty()||!url.password().isEmpty()||url.hasQuery()||url.hasFragment())return false;
    const auto path=url.path();
    if(!path.startsWith("/ws/")||path.count("@kline_")!=1)return false;
    const auto marker=path.indexOf("@kline_");const auto symbol=path.mid(4,marker-4),interval=path.mid(marker+7);
    if(symbol.isEmpty()||symbol.size()>80||symbol!=symbol.toLower()||
        !QStringList{"1m","5m","15m","1h","4h","1d"}.contains(interval))return false;
    for(const auto ch:symbol)if(ch.isSpace()||ch.category()==QChar::Other_Control||ch.category()==QChar::Other_Format||
        ch.category()==QChar::Other_Surrogate||QStringLiteral(":/?#@%&=+\\").contains(ch))return false;
    return true;
}
class NetworkPublicCandleSocket final:public PublicCandleSocket {
  public:
    NetworkPublicCandleSocket(){
        heartbeat_.setInterval(60000);
        connect(&heartbeat_,&QTimer::timeout,this,[this]{if(socket_)socket_->ping();});
    }
    ~NetworkPublicCandleSocket()override{abort();}
    void open(quint64 generation,const QUrl& endpoint)override{
        abort();
        if(!generation||!publicEndpoint(endpoint)){emit failed(generation);return;}
        generation_=generation;const auto wire=wire_;
        auto* socket=new QWebSocket(QString{},QWebSocketProtocol::VersionLatest,this);socket_=socket;
        socket->setProxy(QNetworkProxy::NoProxy);socket->setReadBufferSize(65536);
        socket->setMaxAllowedIncomingFrameSize(65536);socket->setMaxAllowedIncomingMessageSize(65536);
        auto tls=QSslConfiguration::defaultConfiguration();tls.setPeerVerifyMode(QSslSocket::VerifyPeer);socket->setSslConfiguration(tls);
        connect(socket,&QWebSocket::connected,this,[this,wire,generation]{if(wire==wire_){heartbeat_.start();emit connected(generation);}});
        connect(socket,&QWebSocket::textMessageReceived,this,[this,wire,generation](const QString& message){if(wire==wire_)emit received(generation,message.toUtf8());});
        connect(socket,&QWebSocket::binaryMessageReceived,this,[this,wire,generation](const QByteArray& message){if(wire==wire_)emit received(generation,message);});
        connect(socket,&QWebSocket::disconnected,this,[this,wire,generation]{if(wire==wire_)emit failed(generation);});
        connect(socket,&QWebSocket::errorOccurred,this,[this,wire,generation](QAbstractSocket::SocketError){if(wire==wire_)emit failed(generation);});
        connect(socket,&QWebSocket::sslErrors,this,[this,wire,generation](const QList<QSslError>&){if(wire==wire_)emit failed(generation);});
        QNetworkRequest request(endpoint);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        socket->open(request);
    }
    void send(quint64 generation,const QByteArray& message)override{
        if(generation==generation_&&socket_&&message.size()<=65536)socket_->sendTextMessage(QString::fromUtf8(message));
    }
    void abort()override{
        ++wire_;generation_=0;heartbeat_.stop();
        if(socket_){auto* previous=socket_.data();socket_=nullptr;previous->abort();previous->deleteLater();}
    }
  private:
    QPointer<QWebSocket> socket_;
    QTimer heartbeat_;
    quint64 wire_=0,generation_=0;
};
}
std::unique_ptr<PublicCandleSocket> makeNetworkPublicCandleSocket(){return std::make_unique<NetworkPublicCandleSocket>();}
}
