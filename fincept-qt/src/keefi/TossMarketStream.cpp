#include "keefi/TossMarketStream.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TradingTransport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslSocket>
#include <QWebSocket>
#include <QUuid>
#include <QDateTime>
#include <QAuthenticator>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>

namespace keefi {
namespace {
constexpr int MaxMessage = 65536;
class NetworkTossMarketSocket final : public TossMarketSocket {
public:
    explicit NetworkTossMarketSocket(QUrl endpoint = QUrl("wss://openapi-ws.tossinvest.com/ws/v1")) : endpoint_(std::move(endpoint)) {}
    void open(QByteArrayView token) override {
        abort(); const auto epoch = epoch_;
        auto* s = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this); socket_ = s;
        s->setProxy(QNetworkProxy::NoProxy); s->setReadBufferSize(MaxMessage);
        s->setMaxAllowedIncomingFrameSize(MaxMessage); s->setMaxAllowedIncomingMessageSize(MaxMessage);
        auto tls = QSslConfiguration::defaultConfiguration(); tls.setPeerVerifyMode(QSslSocket::VerifyPeer); s->setSslConfiguration(tls);
        connect(s, &QWebSocket::connected, this, [this, epoch] { if (epoch == epoch_) emit connected(); });
        connect(s, &QWebSocket::textMessageReceived, this, [this, epoch](const QString& bytes) { if (epoch == epoch_) emit received(bytes.toUtf8()); });
        connect(s, &QWebSocket::disconnected, this, [this, epoch] { if (epoch == epoch_) emit failed(); });
        connect(s, &QWebSocket::authenticationRequired, this, [this, epoch](QAuthenticator*) { if (epoch == epoch_) emit failed(401); });
        connect(s, &QWebSocket::errorOccurred, this, [this, s, epoch](QAbstractSocket::SocketError) {
            if (epoch != epoch_) return;
            // Qt 6.8.3 processHandshake includes the numeric HTTP status in this
            // diagnostic. Inspect locally, never return raw server text to UI/logs.
            static const QRegularExpression forbidden("\\b403\\b");
            const auto diagnostic = s->errorString();
            // A missing/unsupported 401 challenge does not emit Qt's auth signal.
            const int status = diagnostic.contains("WWW-Authenticate") ? 401 : forbidden.match(diagnostic).hasMatch() ? 403 : 0;
            emit failed(status);
        });
        connect(s, &QWebSocket::sslErrors, this, [this, epoch](const QList<QSslError>&) { if (epoch == epoch_) emit failed(); });
        QNetworkRequest request(endpoint_);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        QByteArray authorization("Bearer "); authorization.append(token.data(), token.size());
        request.setRawHeader("Authorization", authorization); s->open(request);
    }
    void send(const QByteArray& bytes) override { if (socket_) socket_->sendTextMessage(QString::fromUtf8(bytes)); }
    void abort() override {
        ++epoch_; if (socket_) { auto* old = socket_.data(); socket_ = nullptr; old->abort(); old->deleteLater(); }
    }
private:
    quint64 epoch_ = 0; QPointer<QWebSocket> socket_;
    const QUrl endpoint_;
};
QString topic(const TossMarketTarget& target) { return "trade:" + target.marketCountry.toLower() + ':' + target.symbol; }
}
std::unique_ptr<TossMarketSocket> TossMarketSocket::forLoopbackTest(quint16 port) {
    if (!port) return {};
    return std::make_unique<NetworkTossMarketSocket>(QUrl(QString("ws://127.0.0.1:%1/ws/v1").arg(port)));
}
struct TossMarketStream::Private {
    TossMarketStream* q; QPointer<BrokerTradingController> broker; QPointer<TradingTransport> rest;
    std::unique_ptr<TossMarketSocket> socket;
    QList<TossMarketTarget> wanted, selected, restOnly, restQueue, restBatch;
    qsizetype restCursor = 0, restBatchStart = 0;
    QMap<QString, TossMarketQuote> values;
    QSet<QString> accepted, rejected;
    QTimer retry, health;
    QElapsedTimer elapsed;
    MonotonicClock monotonic;
    qint64 expiresAt = 0, openedAt = 0, lastPing = 0, lastHeard = 0, restStarted = 0;
    qint64 lastDeclared = -1000, declareAfter = 0, nextRestAt = 0, periodicRestAt = 0;
    bool declarationPending = false, restDue = false;
    int unverified = 0;
    bool connecting = false, connected = false, restActive = false;
    quint64 revision = 0, blockedRevision = 0, restToken = 0;
    int attempts = 0;
    QString declarationId, message = "기존 토스 인증이 필요합니다.";
    qint64 now() const { return monotonic ? monotonic() : elapsed.elapsed(); }
    void change(const QString& text) { message = text; emit q->changed(); }
    void resetRestPlan() {
        restQueue.clear(); restBatch.clear(); restCursor = restBatchStart = 0;
        periodicRestAt = 0; restDue = false;
    }
    void clearWire() {
        connecting = connected = false; restActive = false; ++restToken;
        retry.stop(); health.stop(); socket->abort(); if (rest) rest->abort();
        values.clear(); accepted.clear(); declarationId.clear();
        resetRestPlan(); declarationPending = false; lastDeclared = -1000;
    }
    void clear() {
        clearWire(); selected.clear(); restOnly.clear(); rejected.clear(); revision = blockedRevision = 0; expiresAt = 0; unverified = 0;
    }
    bool sessionCurrent() const {
        if (!broker || blockedRevision == revision || (expiresAt && now() >= expiresAt)) return false;
        quint64 current = 0;
        return broker->withMarketAuthorization([&](QByteArrayView, qint64, quint64 rev) { current = rev; }) && current == revision;
    }
    void sync() {
        quint64 current = 0;
        if (!broker || !broker->withMarketAuthorization([&](QByteArrayView, qint64, quint64 rev) { current = rev; })) {
            clear(); change("기존 토스 인증이 필요합니다."); return;
        }
        if (current != revision) { clear(); revision = current; }
        if (blockedRevision == current) return;
        QList<TossMarketTarget> valid;
        auto ordered = wanted;
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.priority < b.priority; });
        QSet<QString> selectedTopics, seen; unverified = 0;
        for (const auto& target : ordered) {
            const auto key = topic(target);
            if (seen.contains(key)) continue;
            if ((target.marketCountry == "KR" || target.marketCountry == "US") && target.priority >= 0 && target.priority <= 3 &&
                broker->marketDataCountry(target.symbol) == target.marketCountry && !rejected.contains(key) && !selectedTopics.contains(key)) {
                seen.insert(key);
                selectedTopics.insert(key); valid.append(target);
            } else if (!rejected.contains(key)) ++unverified;
        }
        const auto newSelected = valid.mid(0, 100), newRest = valid.mid(100);
        if (newSelected == selected && newRest == restOnly && (connecting || connected || retry.isActive())) return;
        if (connected) {
            selected = newSelected; restOnly = newRest;
            if (selected.isEmpty()) { clearWire(); change("구독 가능한 토스 종목이 없습니다."); return; }
            QSet<QString> keep; for (const auto& t : valid) keep.insert(topic(t));
            for (auto it = values.begin(); it != values.end();) { if (!keep.contains(it.key())) it = values.erase(it); else ++it; }
            accepted.intersect(keep); restActive = false; ++restToken; if (rest) rest->abort();
            resetRestPlan();
            declarationPending = true; declareAfter = qMax(now(), lastDeclared + 1000); maybeDeclare(); emit q->changed(); return;
        }
        clearWire(); selected = newSelected; restOnly = newRest;
        if (selected.isEmpty()) { change("공식 시장 정보가 확인된 토스 종목을 선택하세요."); return; }
        open();
    }
    void open() {
        if (selected.isEmpty() || !sessionCurrent()) { clear(); change("기존 토스 인증이 필요합니다."); return; }
        connecting = true;
        openedAt = lastPing = lastHeard = now();
        change("토스 실시간 연결 중 · 실연결 미검증");
        broker->withMarketAuthorization([&](QByteArrayView token, qint64 remaining, quint64) {
            const auto deadline = now() + remaining;
            expiresAt = expiresAt ? qMin(expiresAt, deadline) : deadline;
            health.start(); socket->open(token);
        });
    }
    void ready() {
        if (!connecting || selected.isEmpty() || !sessionCurrent()) return;
        connecting = false; connected = true;
        declarationPending = true; declareAfter = now(); maybeDeclare();
    }
    void maybeDeclare() {
        if (!connected || !declarationPending || now() < declareAfter || !sessionCurrent()) return;
        declarationPending = false; lastDeclared = now(); accepted.clear();
        declarationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonArray declaration{QJsonObject{{"id", declarationId}}};
        for (const auto& country : {QString("KR"), QString("US")}) {
            QJsonArray codes; for (const auto& t : selected) if (t.marketCountry == country) codes.append(t.symbol);
            if (!codes.isEmpty()) declaration.append(QJsonObject{{"type", "trade:" + country.toLower()}, {"codes", codes}});
        }
        socket->send(QJsonDocument(declaration).toJson(QJsonDocument::Compact));
        change("토스 구독 승인 대기 · 실연결 미검증");
    }
    void fail(int httpStatus = 0) {
        if ((!connecting && !connected) || retry.isActive()) return;
        clearWire();
        if (httpStatus == 401 || httpStatus == 403) {
            blockedRevision = revision; change("토스 인증 또는 허용 IP 확인이 필요합니다. 기존 연결에서 직접 확인하세요."); return;
        }
        const int delay = qMin(60000, 1000 * (1 << qMin(attempts++, 6))) + QRandomGenerator::global()->bounded(250);
        health.start(); retry.start(delay); change("토스 실시간 연결 끊김 · 재연결 대기 · 표시값 소거");
    }
    void pulse() {
        if (expiresAt && now() >= expiresAt) {
            clearWire(); blockedRevision = revision; change("토스 세션 만료 · 실시간 구독과 표시값 소거 · 기존 연결에서 직접 다시 인증하세요."); return;
        }
        if (!sessionCurrent()) { clear(); change("토스 인증 만료 또는 연결 해제 · 표시값 소거"); return; }
        if ((connecting && now() - openedAt > 15000) || (connected && now() - lastHeard > 180000)) { fail(); return; }
        if (connected && now() - lastPing >= 60000) { lastPing = now(); socket->send("PING"); }
        maybeDeclare();
        if (restActive && now() - restStarted >= 15000) {
            restActive = false; ++restToken; if (rest) rest->abort();
            restCursor = restBatchStart; restBatch.clear(); periodicRestAt = 0; restDue = true;
        }
        if (connected && (restDue || (periodicRestAt && now() >= periodicRestAt))) reconcile();
    }
    bool decode(const TossMarketTarget& target, const QJsonObject& row, bool trade, TossMarketQuote* output) const {
        const QString price = row.value(trade ? "price" : "lastPrice").toString();
        const QString currency = row.value("currency").toString(), timestamp = row.value("timestamp").toString();
        static const QRegularExpression zone("(?:Z|[+-][0-9]{2}:[0-9]{2})$");
        const auto at = QDateTime::fromString(timestamp, Qt::ISODateWithMs); const auto now = QDateTime::currentMSecsSinceEpoch();
        if (price.size() > 30 || !decimal::positive(price) || !zone.match(timestamp).hasMatch() || !at.isValid() ||
            at.toMSecsSinceEpoch() > now + 2000 || now - at.toMSecsSinceEpoch() > 180000 ||
            currency != (target.marketCountry == "KR" ? "KRW" : "USD")) return false;
        if (trade && (row.value("volume").toString().size() > 30 || !decimal::valid(row.value("volume").toString()))) return false;
        output->symbol = target.symbol; output->marketCountry = target.marketCountry; output->price = price; output->currency = currency;
        output->evidence = {trade ? "토스 공식 실시간 체결가" : "토스 공식 REST 현재가",
            trade ? "wss://openapi-ws.tossinvest.com/ws/v1" : "https://openapi.tossinvest.com/api/v1/prices",
            "display:toss:" + target.marketCountry + ':' + target.symbol, at.toMSecsSinceEpoch(), now, true};
        return true;
    }
    void merge(const QString& key, const TossMarketQuote& quote) {
        if (!values.contains(key) || values[key].evidence.sourceTimeMs < quote.evidence.sourceTimeMs) values[key] = quote;
    }
    void reconcile() {
        if (!rest || restActive || accepted.isEmpty() || !sessionCurrent()) return;
        if (now() < nextRestAt) { restDue = true; return; }
        if (restQueue.isEmpty()) {
            for (const auto& target : selected) if (accepted.contains(topic(target))) restQueue.append(target);
            restQueue.append(restOnly);
        }
        if (restQueue.isEmpty()) return;
        if (restCursor >= restQueue.size()) restCursor = 0;
        const auto batch = restQueue.mid(restCursor, 200);
        QStringList symbols; for (const auto& target : batch) symbols.append(target.symbol);
        if (symbols.isEmpty()) return;
        RequestSpec request; request.operation = Operation::Quote; request.authenticated = true;
        request.url = QUrl("https://openapi.tossinvest.com/api/v1/prices");
        QUrlQuery query; query.addQueryItem("symbols", symbols.join(',')); request.url.setQuery(query);
        broker->withMarketAuthorization([&](QByteArrayView token, qint64, quint64 rev) {
            if (rev != revision) return;
            QByteArray auth("Bearer "); auth.append(token.data(), token.size()); request.headers.insert("Authorization", auth);
            restBatchStart = restCursor; restBatch = batch; restCursor += batch.size();
            restActive = true; restDue = restCursor < restQueue.size(); restStarted = now(); nextRestAt = now() + 1000;
            periodicRestAt = restDue ? 0 : now() + 60000;
            rest->send(++restToken, request);
        });
    }
    void restResult(quint64 token, const HttpResult& response) {
        if (!restActive || token != restToken || !connected || !sessionCurrent()) return;
        restActive = false;
        if (response.status == 401 || response.status == 403) { fail(response.status); return; }
        if (response.status == 429 || response.localRetryAfterSeconds > 0) {
            bool ok = false; const int seconds = response.headers.value("Retry-After").toInt(&ok);
            nextRestAt = now() + qBound(1, qMax(response.localRetryAfterSeconds, ok ? seconds : 1), 3600) * 1000LL;
            restCursor = restBatchStart; restBatch.clear(); periodicRestAt = 0;
            restDue = true; change("토스 REST 요청 제한 대기 · 출처와 기준 시각 유지"); return;
        }
        const auto requested = restBatch; restBatch.clear();
        const auto payload = parseExactJson(response.body).object().value("result");
        if (response.networkError || response.timedOut || response.status != 200 || !payload.isArray() || payload.toArray().size() > requested.size()) {
            change("토스 REST 대사 실패 · 실시간 표시 출처와 시각을 확인하세요."); return;
        }
        QMap<QString, TossMarketQuote> batch; QSet<QString> seen;
        for (const auto& value : payload.toArray()) {
            const auto row = value.toObject(); const auto symbol = row.value("symbol").toString();
            if (!value.isObject() || seen.contains(symbol)) { change("토스 REST 응답을 확인할 수 없습니다."); return; }
            seen.insert(symbol); bool found = false;
            for (const auto& t : requested) if (t.symbol == symbol) {
                TossMarketQuote quote; if (!decode(t, row, false, &quote)) { change("토스 REST 가격의 시각·통화를 확인할 수 없습니다."); return; }
                batch[topic(t)] = quote; found = true; break;
            }
            if (!found) { change("토스 REST 응답의 종목을 확인할 수 없습니다."); return; }
        }
        for (auto i = batch.cbegin(); i != batch.cend(); ++i) merge(i.key(), i.value());
        emit q->changed();
    }
    void receive(const QByteArray& data) {
        if (!connected || !sessionCurrent() || data.size() > MaxMessage) return;
        const auto doc = parseExactJson(data); if (!doc.isObject()) return; const auto object = doc.object();
        const auto type = object.value("type").toString();
        if (type == "pong") { lastHeard = now(); return; }
        if (type == "subscriptions") {
            if (object.value("id").toString() != declarationId || !object.value("subscribed").isArray() || !object.value("rejected").isArray()) return;
            QSet<QString> desired; for (const auto& t : selected) desired.insert(topic(t));
            QSet<QString> confirmed, denied;
            for (const auto& value : object.value("subscribed").toArray()) {
                if (!value.isString() || !desired.contains(value.toString()) || confirmed.contains(value.toString())) return;
                confirmed.insert(value.toString());
            }
            for (const auto& value : object.value("rejected").toArray()) {
                const auto key = value.toObject().value("target").toString();
                if (!value.isObject() || !desired.contains(key) || confirmed.contains(key)) return;
                denied.insert(key);
            }
            accepted = confirmed; rejected.unite(denied);
            if (restActive) { restActive = false; ++restToken; if (rest) rest->abort(); }
            resetRestPlan();
            lastHeard = now();
            for (const auto& key : denied) values.remove(key);
            change(denied.isEmpty() ? "토스 시세 수신 대기 · 실연결 미검증" : "토스 일부 종목 구독 거부 · 거부 항목은 재구독에서 제외");
            reconcile(); if (!denied.isEmpty()) sync(); return;
        }
        if (type == "error") {
            const auto code = object.value("error").toObject().value("code").toString();
            if (code == "server-shutdown") { fail(); return; }
            if (object.value("id").toString() != declarationId) return;
            if (code == "rate-limit-exceeded") {
                declarationPending = true; declareAfter = now() + 1000; change("토스 구독 요청 제한 · 1초 후 재선언"); return;
            }
            fail(); return;
        }
        if (type != "message") return;
        const auto key = object.value("topic").toString(); if (!accepted.contains(key) || !object.value("data").isObject()) return;
        for (const auto& t : selected) if (topic(t) == key) {
            TossMarketQuote quote; if (!decode(t, object.value("data").toObject(), true, &quote)) return;
            merge(key, quote); lastHeard = now(); attempts = 0; change("토스 실시간 체결가 · 실연결 미검증"); return;
        }
    }
};
TossMarketStream::TossMarketStream(BrokerTradingController* b, TradingTransport* t, QObject* p)
    : TossMarketStream(b, t, std::make_unique<NetworkTossMarketSocket>(), p) {}
TossMarketStream::TossMarketStream(BrokerTradingController* b, TradingTransport* t, std::unique_ptr<TossMarketSocket> socket, QObject* p)
    : TossMarketStream(b, t, std::move(socket), MonotonicClock{}, p) {}
TossMarketStream::TossMarketStream(BrokerTradingController* b, TradingTransport* t, std::unique_ptr<TossMarketSocket> socket, MonotonicClock clock, QObject* p)
    : QObject(p), d_(std::make_unique<Private>()) {
    d_->q = this; d_->rest = t; d_->socket = socket ? std::move(socket) : std::make_unique<NetworkTossMarketSocket>(); d_->retry.setSingleShot(true);
    d_->elapsed.start(); d_->monotonic = std::move(clock); d_->health.setInterval(200);
    connect(d_->socket.get(), &TossMarketSocket::connected, this, [this] { d_->ready(); });
    connect(d_->socket.get(), &TossMarketSocket::received, this, [this](const QByteArray& data) { d_->receive(data); });
    connect(d_->socket.get(), &TossMarketSocket::failed, this, [this](int status) { d_->fail(status); });
    connect(&d_->retry, &QTimer::timeout, this, [this] { d_->sync(); });
    connect(&d_->health, &QTimer::timeout, this, [this] { d_->pulse(); });
    if (t) connect(t, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) { d_->restResult(token, result); });
    setBroker(b);
}
TossMarketStream::~TossMarketStream() { d_->clear(); }
void TossMarketStream::setBroker(BrokerTradingController* b) {
    if (d_->broker == b) return;
    if (d_->broker) disconnect(d_->broker, nullptr, this, nullptr);
    d_->clear(); d_->broker = b;
    if (b) { connect(b, &BrokerTradingController::changed, this, [this] { d_->sync(); });
        connect(b, &QObject::destroyed, this, [this] { d_->clear(); emit changed(); }); }
    d_->sync();
}
void TossMarketStream::setTargets(const QList<TossMarketTarget>& t) { d_->wanted = t; d_->sync(); }
void TossMarketStream::stop() { d_->wanted.clear(); d_->clear(); emit changed(); }
QList<TossMarketTarget> TossMarketStream::targets() const { return d_->selected; }
QList<TossMarketTarget> TossMarketStream::restOnlyTargets() const { return d_->restOnly; }
QList<TossMarketQuote> TossMarketStream::quotes() const {
    if (!d_->sessionCurrent() || !d_->connected) return {};
    QList<TossMarketQuote> result; const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& quote : d_->values) if (quote.evidence.sourceTimeMs <= now && quote.evidence.receivedAtMs <= now &&
        now - quote.evidence.sourceTimeMs <= 180000 && now - quote.evidence.receivedAtMs <= 180000) result.append(quote);
    return result;
}
QStringList TossMarketStream::rejectedTopics() const { return d_->rejected.values(); }
QString TossMarketStream::status() const { return d_->message; }
QString TossMarketStream::coverage() const {
    return QString("실시간 %1 / 100 · REST 순환 대체 %2 · 요청당 최대 200 · 순환 뒤 60초 대기 · 시장 정보 미확인 %3 · 구독 거부 %4")
        .arg(d_->selected.size()).arg(d_->restOnly.size()).arg(d_->unverified).arg(d_->rejected.size());
}
QString TossMarketStream::limitations() { return "토스 실시간 · 실연결 미검증 · 일부 체결 유실 가능 · 국내 KRX+NXT 통합 · 미국 일부 거래소 기준(NBBO 아님)"; }
}
