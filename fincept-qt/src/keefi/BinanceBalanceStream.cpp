#include "keefi/BinanceBalanceStream.h"
#include "keefi/Vault.h"
#include "keefi/TradingTypes.h"
#include "keefi/TradingRequestLimits.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QScopeGuard>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <exception>
#include <limits>
#include <sodium.h>

namespace keefi {
namespace {
constexpr int MaxMessage = 256 * 1024;
const QUrl Endpoint("wss://ws-api.binance.com:443/ws-api/v3");
class NetworkBalanceSocket final : public BinanceBalanceSocket {
public:
    ~NetworkBalanceSocket() override { abort(); }
    void open(const QUrl& url) override {
        abort(); const auto epoch = epoch_;
        auto* s = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this); socket_ = s;
        s->setProxy(QNetworkProxy::NoProxy); s->setReadBufferSize(MaxMessage);
        s->setMaxAllowedIncomingFrameSize(MaxMessage); s->setMaxAllowedIncomingMessageSize(MaxMessage);
        auto tls = QSslConfiguration::defaultConfiguration(); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); s->setSslConfiguration(tls);
        connect(s, &QWebSocket::connected, this, [this,epoch] { if(epoch == epoch_) emit connected(); });
        connect(s, &QWebSocket::textMessageReceived, this, [this,epoch](const QString& text) { if(epoch == epoch_) emit received(text.toUtf8()); });
        connect(s, &QWebSocket::binaryMessageReceived, this, [this,epoch](const QByteArray&) { if(epoch == epoch_) emit failed(); });
        connect(s, &QWebSocket::pong, this, [this,epoch](quint64,const QByteArray&) { if(epoch == epoch_) emit alive(); });
        connect(s, &QWebSocket::disconnected, this, [this,epoch] { if(epoch == epoch_) emit failed(); });
        connect(s, &QWebSocket::errorOccurred, this, [this,epoch](QAbstractSocket::SocketError) { if(epoch == epoch_) emit failed(); });
        connect(s, &QWebSocket::sslErrors, this, [this,epoch](const QList<QSslError>&) { if(epoch == epoch_) emit failed(); });
        QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        s->open(request); // QWebSocket answers server ping frames with the same payload.
    }
    void send(const QByteArray& bytes) override {
        if(!socket_) return;
        auto text = QString::fromUtf8(bytes); socket_->sendTextMessage(text);
        sodium_memzero(text.data(),std::size_t(text.size()) * sizeof(QChar));
    }
    void ping() override { if(socket_) socket_->ping(); }
    void abort() override { ++epoch_; if(socket_) { auto* old=socket_.data(); socket_=nullptr; old->abort(); old->deleteLater(); } }
private:
    quint64 epoch_=0;
    QPointer<QWebSocket> socket_;
};
bool credentialValid(const SecureBuffer& value) {
    if(value.empty() || value.size()>512) return false;
    for(std::size_t i=0;i<value.size();++i) {
        const auto c=value.data()[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))) return false;
    }
    return true;
}
void wipe(QByteArray& bytes) { if(!bytes.isEmpty()) sodium_memzero(bytes.data(),std::size_t(bytes.size())); bytes.clear(); }
SecureBuffer subscription(const CredentialSet& credentials, const QString& id, qint64 time) {
    const auto k=credentials.find("api_key"), s=credentials.find("secret");
    if(k==credentials.end()||s==credentials.end()||!credentialValid(k->second)||!credentialValid(s->second)) return {};
    // Fixed ASCII fields in alphabetical order. No user-supplied method or endpoint.
    QByteArray key,input,signature,payload;
    const auto cleanup=qScopeGuard([&]{wipe(key);wipe(input);wipe(signature);wipe(payload);});
    key=QByteArray(reinterpret_cast<const char*>(k->second.data()),qsizetype(k->second.size()));
    input="apiKey="+key+"&recvWindow=5000&timestamp="+QByteArray::number(time);
    SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)), digest(crypto_auth_hmacsha256_BYTES);
    auto* hmac=reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
    const bool ok=crypto_auth_hmacsha256_init(hmac,s->second.data(),s->second.size())==0 &&
        crypto_auth_hmacsha256_update(hmac,reinterpret_cast<const unsigned char*>(input.constData()),input.size())==0 &&
        crypto_auth_hmacsha256_final(hmac,digest.data())==0;
    wipe(input); state.clear();
    if(!ok) { wipe(key); return {}; }
    signature=QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()),digest.size()).toHex(); digest.clear();
    payload="{\"id\":\""+id.toLatin1()+"\",\"method\":\"userDataStream.subscribe.signature\",\"params\":{\"apiKey\":\""+key+
        "\",\"recvWindow\":5000,\"timestamp\":"+QByteArray::number(time)+",\"signature\":\""+signature+"\"}}";
    SecureBuffer result(payload.constData(),payload.size()); wipe(payload); wipe(key); wipe(signature); return result;
}
qint64 integer(const QJsonValue& value, qint64 fallback=-1) {
    const auto text=value.toString();
    if(text.isEmpty()||text.size()>16) return fallback;
    for(const auto c:text) if(c<'0'||c>'9') return fallback;
    bool ok=false; const auto n=text.toLongLong(&ok); return ok?n:fallback;
}
}
struct BinanceBalanceStream::Private {
    enum class State { Stopped, Connecting, Time, Subscribe, Ready, Waiting, Failed };
    BinanceBalanceStream* q; Vault& vault; std::unique_ptr<BinanceBalanceSocket> socket;
    std::shared_ptr<TradingRequestLimits> limits;
    State state=State::Stopped; QString accountId,credentialRef,requestId,message="잔고 알림 중지";
    qint64 subscriptionId=-1,openedAt=0,heardAt=0,pingAt=0,timeRequestedAt=0,serverAnchor=0,serverAt=0;
    int attempts=0; quint64 revision=0; QElapsedTimer clock;
    QTimer retry,health; QMap<QString,qint64> lastEvent;
    Private(BinanceBalanceStream* owner,Vault& v,std::unique_ptr<BinanceBalanceSocket> wire,std::shared_ptr<TradingRequestLimits> l)
        :q(owner),vault(v),socket(std::move(wire)),limits(l?std::move(l):TradingRequestLimits::shared()) {
        clock.start(); retry.setSingleShot(true); health.setInterval(1000);
    }
    bool available() const {
        if(!vault.isUnlocked()||accountId.isEmpty()) return false;
        for(const auto& a:vault.accounts()) if(a.id==accountId)
            return a.provider=="binance"&&a.market=="spot"&&a.environment=="live"&&a.credentialRef==credentialRef;
        return false;
    }
    void change(State next,const QString& text) {
        const bool changed=next!=state||text!=message; state=next; message=text; if(changed) emit q->changed();
    }
    void detach() {
        ++revision; QObject::disconnect(socket.get(),nullptr,q,nullptr); socket->abort(); health.stop(); retry.stop();
        subscriptionId=-1; requestId.clear(); lastEvent.clear();
    }
    void fail(bool reconnect,const QString& text) {
        if(state==State::Stopped||state==State::Waiting||state==State::Failed) return;
        detach();
        if(reconnect&&available()) {
            const int delay=qMin(60000,1000*(1<<qMin(attempts++,6)));
            retry.start(delay); change(State::Waiting,text+QString(" · %1초 후 재연결 · 20초 잔고 조회 유지").arg(delay/1000));
        } else change(State::Failed,text+" · 20초 잔고 조회 유지");
    }
    void open() {
        if(!available()) { q->stop(); return; }
        detach(); const auto epoch=revision;
        const auto cooldown=limits->remainingMs(Endpoint);
        if(cooldown>0) {
            retry.start(int(qMin<qint64>(cooldown,std::numeric_limits<int>::max())));
            change(State::Waiting,"Binance 요청 제한 대기 · 잔고 알림 재연결 보류"); return;
        }
        QObject::connect(socket.get(),&BinanceBalanceSocket::connected,q,[this,epoch] {
            if(epoch!=revision||state!=State::Connecting||!available()) return;
            requestId=QUuid::createUuid().toString(QUuid::Id128); timeRequestedAt=clock.elapsed();
            change(State::Time,"잔고 알림 · 서버 시각 확인 중");
            if(epoch==revision) socket->send("{\"id\":\""+requestId.toLatin1()+"\",\"method\":\"time\"}");
        });
        QObject::connect(socket.get(),&BinanceBalanceSocket::received,q,[this,epoch](const QByteArray& bytes) { if(epoch==revision) receive(bytes); });
        QObject::connect(socket.get(),&BinanceBalanceSocket::failed,q,[this,epoch] { if(epoch==revision) fail(true,"잔고 알림 연결 끊김"); });
        QObject::connect(socket.get(),&BinanceBalanceSocket::alive,q,[this,epoch] { if(epoch==revision) heardAt=clock.elapsed(); });
        openedAt=heardAt=pingAt=clock.elapsed(); health.start(); change(State::Connecting,"Binance 잔고 알림 연결 중");
        if(epoch==revision) socket->open(Endpoint);
    }
    void receive(const QByteArray& bytes) {
        if(!available()||(state!=State::Time&&state!=State::Subscribe&&state!=State::Ready)) return;
        if(bytes.size()>MaxMessage) { fail(false,"잔고 알림 크기 확인 실패"); return; }
        const auto doc=parseExactJson(bytes); if(!doc.isObject()) { fail(false,"잔고 알림 형식 확인 실패"); return; }
        const auto root=doc.object();
        if(root.contains("id")) {
            if(root.value("id").toString()!=requestId||(state!=State::Time&&state!=State::Subscribe)) return;
            const auto status=integer(root.value("status"));
            if(status!=200||root.contains("error")) {
                if(status==429||status==418) {
                    HttpResult result; result.status=int(status);
                    const auto until=integer(root.value("error").toObject().value("data").toObject().value("retryAfter"));
                    const auto now=serverAnchor>0?serverAnchor+clock.elapsed()-serverAt:QDateTime::currentMSecsSinceEpoch();
                    if(until>now) result.headers["Retry-After"]=QByteArray::number((until-now+999)/1000);
                    limits->observe(Endpoint,result); fail(false,"Binance 요청 제한 · 알림 구독 중지");
                } else fail(false,"잔고 알림 인증·구독 확인 실패 · 선택 계좌 다시 조회로 재시도");
                return; // Never display server error text or automatically retry authentication failures.
            }
            const auto result=root.value("result").toObject();
            if(state==State::Time) {
                const auto time=integer(result.value("serverTime"));
                if(time<=0||clock.elapsed()-timeRequestedAt>5000) { fail(false,"잔고 알림 서버 시각 확인 실패"); return; }
                serverAnchor=time;serverAt=clock.elapsed(); requestId=QUuid::createUuid().toString(QUuid::Id128);
                SecureBuffer payload;
                try { vault.withCredentials(accountId,[&](const CredentialSet& credentials) { payload=subscription(credentials,requestId,time); }); }
                catch(const std::exception&) { fail(false,"잔고 알림 인증정보 보호 메모리 준비 실패"); return; }
                if(payload.empty()) { fail(false,"잔고 알림 HMAC 인증정보 확인 실패"); return; }
                const auto epoch=revision; openedAt=clock.elapsed(); change(State::Subscribe,"잔고 알림 구독 확인 중");
                QByteArray wire(reinterpret_cast<const char*>(payload.data()),payload.size()); payload.clear();
                if(epoch==revision&&available()) socket->send(wire); wipe(wire); return;
            }
            subscriptionId=integer(result.value("subscriptionId"));
            if(subscriptionId<0||subscriptionId>65535) { fail(false,"잔고 알림 구독 범위 확인 실패"); return; }
            heardAt=clock.elapsed(); const auto epoch=revision;
            change(State::Ready,"실시간 잔고 알림 · 변경 시 재조회 · 20초 정기 조회");
            if(epoch==revision&&available()) emit q->balancesChanged(); // Reconcile the connection gap.
            return;
        }
        if(state!=State::Ready) return;
        if(!root.value("event").isObject()) { fail(false,"잔고 알림 이벤트 형식 확인 실패"); return; }
        const auto event=root.value("event").toObject(); const auto type=event.value("e").toString();
        if(type=="serverShutdown") { fail(true,"Binance 서버 재시작 · 잔고 알림 재연결"); return; }
        if(integer(root.value("subscriptionId"))!=subscriptionId) return;
        if(type=="eventStreamTerminated") { fail(false,"잔고 알림 구독 종료 · 선택 계좌 다시 조회로 재시도"); return; }
        if(type!="outboundAccountPosition"&&type!="balanceUpdate"&&type!="executionReport"&&type!="externalLockUpdate") return;
        const auto time=integer(event.value("E"));
        if(time<=0||time>serverAnchor+clock.elapsed()-serverAt+2000) return;
        // Invalidation only: no balance, delta, order ID or raw frame escapes this object.
        if(time<=lastEvent.value(type)) return;
        lastEvent[type]=time; heardAt=clock.elapsed(); emit q->balancesChanged();
    }
};
BinanceBalanceStream::BinanceBalanceStream(Vault& v,QObject* parent)
    :BinanceBalanceStream(v,std::make_unique<NetworkBalanceSocket>(),{},parent) {}
BinanceBalanceStream::BinanceBalanceStream(Vault& v,std::unique_ptr<BinanceBalanceSocket> socket,std::shared_ptr<TradingRequestLimits> limits,QObject* parent)
    :QObject(parent),d_(std::make_unique<Private>(this,v,std::move(socket),std::move(limits))) {
    Q_ASSERT(d_->socket);
    connect(&v,&Vault::aboutToLock,this,&BinanceBalanceStream::stop);
    connect(&v,&Vault::accountsChanged,this,&BinanceBalanceStream::stop);
    connect(&d_->retry,&QTimer::timeout,this,[this] { d_->open(); });
    connect(&d_->health,&QTimer::timeout,this,[this] {
        if(!d_->available()) { stop(); return; }
        const auto now=d_->clock.elapsed();
        if((d_->state!=Private::State::Ready&&now-d_->openedAt>10000)||now-d_->heardAt>45000) { d_->fail(true,"잔고 알림 응답 시간 초과"); return; }
        if(d_->state==Private::State::Ready&&now-d_->openedAt>60000) d_->attempts=0;
        if(now-d_->pingAt>=15000) { d_->pingAt=now; d_->socket->ping(); }
    });
}
BinanceBalanceStream::~BinanceBalanceStream() { d_->detach(); }
void BinanceBalanceStream::start(const QString& id) {
    stop(); d_->accountId=id;
    for(const auto& a:d_->vault.accounts()) if(a.id==id) d_->credentialRef=a.credentialRef;
    if(!d_->available()) { d_->change(Private::State::Failed,"잔고 알림은 열린 금고의 Binance 실전 현물 연결만 지원합니다."); return; }
    d_->open();
}
void BinanceBalanceStream::stop() {
    d_->detach(); d_->accountId.clear(); d_->credentialRef.clear(); d_->attempts=0; d_->serverAnchor=0;
    d_->change(Private::State::Stopped,"잔고 알림 중지");
}
bool BinanceBalanceStream::ready() const { return d_->state==Private::State::Ready; }
QString BinanceBalanceStream::status() const { return d_->message; }
}
