#include "keefi/OkxOrderEvents.h"
#include "keefi/OkxProtocol.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <exception>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <sodium.h>

namespace keefi {
namespace {
void wipe(QByteArray& bytes) { if (!bytes.isEmpty()) sodium_memzero(bytes.data(), bytes.size()); bytes.clear(); }
void wipe(QString& text) { if (!text.isEmpty()) sodium_memzero(text.data(), std::size_t(text.size()) * sizeof(QChar)); text.clear(); }
class QtOkxEventSocket final : public OkxEventSocket {
public:
    using OkxEventSocket::OkxEventSocket;
    ~QtOkxEventSocket() override { abort(); }
    void open(const QUrl& url) override {
        abort(); const auto revision = epoch_;
        socket_ = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
        auto* socket = socket_.data(); socket->setProxy(QNetworkProxy::NoProxy);
        socket->setReadBufferSize(MaxStandardResponseBytes);
        socket->setMaxAllowedIncomingFrameSize(MaxStandardResponseBytes);
        socket->setMaxAllowedIncomingMessageSize(MaxStandardResponseBytes);
        auto tls = QSslConfiguration::defaultConfiguration(); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); socket->setSslConfiguration(tls);
        connect(socket, &QWebSocket::connected, this, [this, revision] { if (revision == epoch_) emit connected(); });
        connect(socket, &QWebSocket::textMessageReceived, this, [this, revision](const QString& text) { if (revision == epoch_) emit textReceived(text); });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [this, revision](const QByteArray&) { if (revision == epoch_) emit failed(); });
        connect(socket, &QWebSocket::disconnected, this, [this, revision] { if (revision == epoch_) emit failed(); });
        connect(socket, &QWebSocket::errorOccurred, this, [this, revision](QAbstractSocket::SocketError) { if (revision == epoch_) emit failed(); });
        connect(socket, &QWebSocket::sslErrors, this, [this, revision](const QList<QSslError>&) { if (revision == epoch_) emit failed(); });
        QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        socket->open(request);
    }
    void sendText(const QString& text) override { if (socket_) socket_->sendTextMessage(text); }
    void abort() override {
        ++epoch_;
        if (socket_) { auto* old = socket_.data(); socket_ = nullptr; old->abort(); old->deleteLater(); }
    }
private:
    quint64 epoch_ = 0;
    QPointer<QWebSocket> socket_;
};
QUrl endpoint(const AccountRecord& a) {
    if (a.provider != "okx" || a.market != "spot" || !okxRegionValid(a.region) || !okxEnvironmentValid(a.environment)) return {};
    const QString host = a.region == "global" ? "ws.okx.com" : a.region == "us" ? "wsus.okx.com" : "wseea.okx.com";
    return QUrl("wss://" + host + "/ws/v5/private");
}
SecureBuffer loginPayload(const CredentialSet& credentials, qint64 nowMs) {
    for (const auto* key : {"api_key", "secret", "passphrase"}) {
        const auto i = credentials.find(key);
        if (i == credentials.end() || i->second.empty() || i->second.size() > 8192) return {};
    }
    const auto timestamp = QByteArray::number(nowMs / 1000);
    const auto input = timestamp + "GET/users/self/verify";
    SecureBuffer state(sizeof(crypto_auth_hmacsha256_state)), digest(crypto_auth_hmacsha256_BYTES);
    auto* hmac = reinterpret_cast<crypto_auth_hmacsha256_state*>(state.data());
    const auto& secret = credentials.at("secret");
    crypto_auth_hmacsha256_init(hmac, secret.data(), secret.size());
    crypto_auth_hmacsha256_update(hmac, reinterpret_cast<const unsigned char*>(input.constData()), input.size());
    crypto_auth_hmacsha256_final(hmac, digest.data()); state.clear();
    auto signature = QByteArray::fromRawData(reinterpret_cast<const char*>(digest.data()), digest.size()).toBase64(); digest.clear();
    auto key = QString::fromUtf8(reinterpret_cast<const char*>(credentials.at("api_key").data()), credentials.at("api_key").size());
    auto passphrase = QString::fromUtf8(reinterpret_cast<const char*>(credentials.at("passphrase").data()), credentials.at("passphrase").size());
    QJsonObject arg{{"apiKey", key}, {"passphrase", passphrase}, {"timestamp", QString::fromLatin1(timestamp)}, {"sign", QString::fromLatin1(signature)}};
    auto bytes = QJsonDocument(QJsonObject{{"op", "login"}, {"args", QJsonArray{arg}}}).toJson(QJsonDocument::Compact);
    SecureBuffer result(bytes.constData(), bytes.size());
    arg = {}; wipe(bytes); wipe(key); wipe(passphrase); wipe(signature);
    return result;
}
}
struct NetworkOkxOrderEvents::Private {
    enum class State { Stopped, Connecting, Login, Subscribe, Ready, Failed };
    NetworkOkxOrderEvents* q;
    std::unique_ptr<OkxEventSocket> socket;
    State state = State::Stopped;
    quint64 revision = 0;
    QString message = QStringLiteral("주문 이벤트 연결 중지"), scope, region, environment, subscriptionId;
    SecureBuffer login;
    QTimer deadline, heartbeat;
    bool awaitingPong = false;
    QMap<QString, qint64> latest;
    QMap<QString, QByteArray> lastPayload;
    QMap<QString, QString> amendments;
    explicit Private(NetworkOkxOrderEvents* owner, std::unique_ptr<OkxEventSocket> wire) : q(owner), socket(std::move(wire)) {
        deadline.setSingleShot(true); heartbeat.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, q, [this] { fail("주문 이벤트 연결 응답 시간이 만료되었습니다."); });
        QObject::connect(&heartbeat, &QTimer::timeout, q, [this] {
            if (state != State::Ready) return;
            if (awaitingPong) { fail("주문 이벤트 연결 응답이 끊겼습니다."); return; }
            awaitingPong = true; heartbeat.start(10000); socket->sendText(QStringLiteral("ping"));
        });
    }
    void change(State next, const QString& text) {
        const bool changed = state != next || message != text; state = next; message = text;
        if (changed) emit q->stateChanged();
    }
    void fail(const QString& text) {
        if (state == State::Stopped || state == State::Failed) return;
        ++revision; deadline.stop(); heartbeat.stop(); login.clear(); awaitingPong = false;
        socket->abort(); change(State::Failed, text);
    }
    void receive(const QString& text) {
        if (state != State::Login && state != State::Subscribe && state != State::Ready) return;
        if (text == "pong") {
            if (state != State::Ready || !awaitingPong) return;
            awaitingPong = false; heartbeat.start(20000); return;
        }
        if (text.size() > MaxStandardResponseBytes) { fail("주문 이벤트 크기가 허용 범위를 넘었습니다."); return; }
        const auto bytes = text.toUtf8(); QString error;
        const auto doc = parseExactJson(bytes, &error);
        if (!error.isEmpty() || !doc.isObject()) { fail("주문 이벤트 형식이 올바르지 않습니다."); return; }
        const auto root = doc.object(); const auto event = root.value("event").toString();
        if (state == State::Login) {
            if (event != "login" || root.value("code") != QJsonValue("0")) { fail("주문 이벤트 인증을 확인하지 못했습니다."); return; }
            const auto before = revision;
            deadline.start(10000); change(State::Subscribe, "현물 주문 이벤트 구독 확인 중");
            if (revision != before || state != State::Subscribe) return;
            subscriptionId = QUuid::createUuid().toString(QUuid::Id128);
            socket->sendText(QString::fromUtf8(QJsonDocument(QJsonObject{{"id", subscriptionId}, {"op", "subscribe"},
                {"args", QJsonArray{QJsonObject{{"channel", "orders"}, {"instType", "SPOT"}}}}}).toJson(QJsonDocument::Compact)));
            return;
        }
        const auto arg = root.value("arg").toObject();
        if (state == State::Subscribe) {
            if (event != "subscribe" || root.value("id") != QJsonValue(subscriptionId) ||
                arg.value("channel") != QJsonValue("orders") || arg.value("instType") != QJsonValue("SPOT") ||
                (root.contains("code") && root.value("code") != QJsonValue("0"))) {
                fail("현물 주문 이벤트 구독을 확인하지 못했습니다."); return;
            }
            deadline.stop(); heartbeat.start(20000); change(State::Ready, "현물 주문 이벤트 수신 준비 완료"); return;
        }
        if (event == "channel-conn-count" && root.value("channel") == QJsonValue("orders")) return;
        if (!event.isEmpty()) { fail("주문 이벤트 구독이 중단되었습니다. 다시 연결하세요."); return; }
        if (arg.value("channel") != QJsonValue("orders") || arg.value("instType") != QJsonValue("SPOT") ||
            !arg.value("uid").isString() || okxAccountScope(region, environment, arg.value("uid").toString()) != scope ||
            !root.value("data").isArray() || root.value("data").toArray().size() > 10000) {
            fail("주문 이벤트 계정·범위를 확인하지 못했습니다."); return;
        }
        if (!awaitingPong) heartbeat.start(20000);
        for (const auto& item : root.value("data").toArray()) {
            if (!item.isObject()) { fail("주문 이벤트 행 형식이 올바르지 않습니다."); return; }
            const auto row = item.toObject(); OrderRecord record;
            if (!parseOkxOrder(row, OkxOrderSource::WebSocket, &record, &error) ||
                (arg.contains("instId") && arg.value("instId") != QJsonValue(record.symbol))) {
                fail("주문 이벤트 값·단위를 확인하지 못했습니다."); return;
            }
            const auto key = record.symbol + ':' + record.orderId;
            const auto at = record.sourceUpdatedAt.toLongLong();
            if (latest.contains(key) && at < latest.value(key)) continue;
            const auto req = row.value("reqId").toString(), result = row.value("amendResult").toString();
            if (!req.isEmpty() && !result.isEmpty()) {
                const auto amendmentKey = key + ':' + req;
                if (amendments.contains(amendmentKey)) {
                    if (amendments.value(amendmentKey) != result) { fail("정정 결과 이벤트가 서로 다릅니다. 공식 주문을 확인하세요."); return; }
                    continue;
                }
                if (amendments.size() >= 20000) { fail("주문 이벤트 보관 상한에 도달했습니다. 다시 연결하세요."); return; }
                amendments.insert(amendmentKey, result);
            }
            const auto digest = QCryptographicHash::hash(QJsonDocument(row).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
            if (lastPayload.value(key) == digest) continue;
            if (!latest.contains(key) && latest.size() >= 20000) { fail("주문 이벤트 보관 상한에 도달했습니다. 다시 연결하세요."); return; }
            latest.insert(key, at); lastPayload.insert(key, digest);
            const auto sentRevision = revision;
            emit q->orderEvent(OkxOrderEvent{revision, scope, record.symbol, record.orderId, req, result, record.sourceUpdatedAt, record});
            if (revision != sentRevision || state != State::Ready) return; // A slot may lock/stop immediately.
        }
    }
};
NetworkOkxOrderEvents::NetworkOkxOrderEvents(QObject* parent)
    : NetworkOkxOrderEvents(std::make_unique<QtOkxEventSocket>(), parent) {}
NetworkOkxOrderEvents::NetworkOkxOrderEvents(std::unique_ptr<OkxEventSocket> socket, QObject* parent)
    : OkxOrderEvents(parent), d_(std::make_unique<Private>(this, std::move(socket))) {
    Q_ASSERT(d_->socket); qRegisterMetaType<OkxOrderEvent>();
}
NetworkOkxOrderEvents::~NetworkOkxOrderEvents() { stop(); }
void NetworkOkxOrderEvents::start(const AccountRecord& account, const CredentialSet& credentials, const QString& scope, qint64 nowMs) {
    stop(); const auto url = endpoint(account);
    if (url.isEmpty() || nowMs <= 0 || !QRegularExpression("\\A[a-f0-9]{64}\\z").match(scope).hasMatch()) {
        d_->change(Private::State::Failed, "주문 이벤트 연결 지역·계정 범위를 확인하세요."); return;
    }
    try { d_->login = loginPayload(credentials, nowMs); }
    catch (const std::exception&) { d_->change(Private::State::Failed, "인증정보 보호 메모리를 준비하지 못했습니다."); return; }
    if (d_->login.empty()) { d_->change(Private::State::Failed, "주문 이벤트 인증정보를 확인하세요."); return; }
    d_->scope = scope; d_->region = account.region; d_->environment = account.environment;
    const auto revision = d_->revision;
    connect(d_->socket.get(), &OkxEventSocket::connected, this, [this, revision] {
        if (revision != d_->revision || d_->state != Private::State::Connecting) return;
        auto payload = QString::fromUtf8(reinterpret_cast<const char*>(d_->login.data()), d_->login.size()); d_->login.clear();
        d_->deadline.start(10000); d_->change(Private::State::Login, "주문 이벤트 인증 확인 중");
        if (revision == d_->revision) d_->socket->sendText(payload);
        wipe(payload);
    });
    connect(d_->socket.get(), &OkxEventSocket::textReceived, this, [this, revision](const QString& text) { if (revision == d_->revision) d_->receive(text); });
    connect(d_->socket.get(), &OkxEventSocket::failed, this, [this, revision] { if (revision == d_->revision) d_->fail("주문 이벤트 보안 연결이 중단되었습니다."); });
    d_->deadline.start(10000); d_->change(Private::State::Connecting, "주문 이벤트 연결 중");
    if (revision == d_->revision) d_->socket->open(url);
}
void NetworkOkxOrderEvents::stop() {
    ++d_->revision; d_->deadline.stop(); d_->heartbeat.stop();
    disconnect(d_->socket.get(), nullptr, this, nullptr); d_->login.clear(); d_->awaitingPong = false;
    d_->latest.clear(); d_->lastPayload.clear(); d_->amendments.clear(); d_->scope.clear(); d_->region.clear(); d_->environment.clear();
    d_->socket->abort(); d_->change(Private::State::Stopped, "주문 이벤트 연결 중지");
}
bool NetworkOkxOrderEvents::ready() const { return d_->state == Private::State::Ready; }
QString NetworkOkxOrderEvents::status() const { return d_->message; }
quint64 NetworkOkxOrderEvents::generation() const { return d_->revision; }
} // namespace keefi
