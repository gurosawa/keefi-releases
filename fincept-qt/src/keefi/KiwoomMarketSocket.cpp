#include "keefi/KiwoomMarketStream.h"
#include <QAuthenticator>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QWebSocket>
namespace keefi {
namespace {
class NetworkKiwoomMarketSocket final:public KiwoomMarketSocket {
  public:
    ~NetworkKiwoomMarketSocket() override {abort();}
    void open(quint64 generation,const QString& country) override {
        // The caller cannot supply an endpoint or open another simultaneous socket.
        if(!generation||socket_||(country!="KR"&&country!="US")){emit failed(generation);return;}
        generation_=generation;const auto wire=++wire_;const QUrl endpoint(country=="US"?"wss://api.kiwoom.com:10000/api/us/websocket":"wss://api.kiwoom.com:10000/api/dostk/websocket");
        auto* socket=new QWebSocket(QString{},QWebSocketProtocol::VersionLatest,this);socket_=socket;
        socket->setProxy(QNetworkProxy::NoProxy);socket->setReadBufferSize(KiwoomMarketProtocol::MaxFrameBytes);socket->setMaxAllowedIncomingFrameSize(KiwoomMarketProtocol::MaxFrameBytes);socket->setMaxAllowedIncomingMessageSize(KiwoomMarketProtocol::MaxFrameBytes);
        auto tls=QSslConfiguration::defaultConfiguration();tls.setPeerVerifyMode(QSslSocket::VerifyPeer);socket->setSslConfiguration(tls);
        connect(socket,&QWebSocket::connected,this,[this,wire,generation]{if(wire==wire_)emit connected(generation);});
        connect(socket,&QWebSocket::textMessageReceived,this,[this,wire,generation](const QString& text){if(wire==wire_)emit received(generation,text.toUtf8());});
        connect(socket,&QWebSocket::binaryMessageReceived,this,[this,wire,generation](const QByteArray& bytes){if(wire==wire_)emit received(generation,bytes);});
        connect(socket,&QWebSocket::disconnected,this,[this,socket,wire,generation]{if(wire!=wire_)return;socket_=nullptr;generation_=0;socket->deleteLater();emit closed(generation);});
        connect(socket,&QWebSocket::authenticationRequired,this,[this,wire,generation](QAuthenticator*){if(wire==wire_)emit failed(generation,401);});
        connect(socket,&QWebSocket::errorOccurred,this,[this,socket,wire,generation](QAbstractSocket::SocketError){
            if(wire!=wire_)return;const auto diagnostic=socket->errorString();const int status=diagnostic.contains("WWW-Authenticate")?401:QRegularExpression("\\b403\\b").match(diagnostic).hasMatch()?403:0;emit failed(generation,status);
        });
        connect(socket,&QWebSocket::sslErrors,this,[this,wire,generation](const QList<QSslError>&){if(wire==wire_)emit failed(generation);});
        QNetworkRequest request(endpoint);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);socket->open(request);
    }
    void sendLogin(quint64 generation,QByteArrayView token) override {
        if(!socket_||generation!=generation_||token.empty()||token.size()>1000)return;
        for(const char ch:token)if(!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||QByteArrayView("-._~+/=").contains(ch)))return;
        QByteArray packet("{\"trnm\":\"LOGIN\",\"token\":\"");packet.append(token.data(),token.size());packet.append("\"}");QString text=QString::fromLatin1(packet);socket_->sendTextMessage(text);text.fill(QChar(0));packet.fill('\0');
    }
    void sendControl(quint64 generation,const QByteArray& payload) override {
        if(!socket_||generation!=generation_||payload.isEmpty()||payload.size()>KiwoomMarketProtocol::MaxFrameBytes)return;
        if(!KiwoomMarketProtocol::pingEcho(payload)){const auto doc=parseExactJson(payload);if(!doc.isObject()||!QStringList{"REG","REMOVE"}.contains(doc.object().value("trnm").toString()))return;}
        socket_->sendTextMessage(QString::fromUtf8(payload));
    }
    void close(quint64 generation) override {if(socket_&&generation==generation_)socket_->close(QWebSocketProtocol::CloseCodeNormal);}
    void abort() override {++wire_;generation_=0;if(socket_){auto* old=socket_.data();socket_=nullptr;old->abort();old->deleteLater();}}
  private:
    QPointer<QWebSocket> socket_;quint64 generation_=0,wire_=0;
};
}
std::unique_ptr<KiwoomMarketSocket> makeNetworkKiwoomMarketSocket(){return std::make_unique<NetworkKiwoomMarketSocket>();}
}
