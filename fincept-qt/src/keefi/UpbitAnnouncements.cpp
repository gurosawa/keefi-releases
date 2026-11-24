#include "keefi/UpbitAnnouncements.h"
#include "keefi/AccountModel.h"
#include "keefi/UpbitAdapter.h"
#include "keefi/Vault.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QWebSocket>
#include <algorithm>

namespace keefi {
namespace {
constexpr qsizetype MaximumMessageBytes = 64 * 1024;

class NetworkUpbitAnnouncementSocket final : public UpbitAnnouncementSocket {
  public:
    using UpbitAnnouncementSocket::UpbitAnnouncementSocket;
    void open(const QNetworkRequest& request) override {
        abort(); const auto epoch = ++epoch_;
        auto* s = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this); socket_ = s;
        s->setReadBufferSize(MaximumMessageBytes);
        s->setMaxAllowedIncomingFrameSize(MaximumMessageBytes);
        s->setMaxAllowedIncomingMessageSize(MaximumMessageBytes);
        connect(s, &QWebSocket::connected, this, [this, epoch] { if (epoch == epoch_) emit connected(); });
        connect(s, &QWebSocket::textMessageReceived, this, [this, epoch](const QString& text) { if (epoch == epoch_) emit received(text.toUtf8()); });
        connect(s, &QWebSocket::binaryMessageReceived, this, [this, epoch](const QByteArray& bytes) { if (epoch == epoch_) emit received(bytes); });
        connect(s, &QWebSocket::disconnected, this, [this, epoch] { if (epoch == epoch_) emit failed(); });
        connect(s, &QWebSocket::errorOccurred, this, [this, epoch](QAbstractSocket::SocketError) { if (epoch == epoch_) emit failed(); });
        connect(s, &QWebSocket::sslErrors, this, [this, epoch](const QList<QSslError>&) { if (epoch == epoch_) emit failed(); });
        s->open(request);
    }
    void send(const QByteArray& text) override { if (socket_) socket_->sendTextMessage(QString::fromUtf8(text)); }
    void abort() override {
        ++epoch_; if (!socket_) return;
        auto* s = socket_.data(); socket_.clear(); s->disconnect(this); s->abort(); s->deleteLater();
    }
  private:
    QPointer<QWebSocket> socket_; quint64 epoch_ = 0;
};
bool boundedText(const QString& text, qsizetype maximum) {
    if (text.trimmed().isEmpty() || text.size() > maximum) return false;
    for (auto c : text) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
// Upbit's documented links carry extra parameters such as `&view=share`. Only the
// numeric id is kept; the stored link is rebuilt so nothing else reaches the view.
QString articleUrl(const QString& text) {
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty() || url.port() != -1 || url.hasFragment() ||
        (url.host() != "upbit.com" && url.host() != "www.upbit.com") || url.path() != "/service_center/notice") return {};
    const auto ids = QUrlQuery(url).allQueryItemValues(QStringLiteral("id"), QUrl::FullyDecoded);
    static const QRegularExpression digits(QStringLiteral("\\A[0-9]{1,20}\\z"));
    if (ids.size() != 1 || !digits.match(ids.first()).hasMatch()) return {};
    return QStringLiteral("https://") + url.host() + QStringLiteral("/service_center/notice?id=") + ids.first();
}
qint64 listedAt(const QString& text) {
    // Upbit documents KST ISO 8601; an explicit offset wins, otherwise the documented KST applies.
    static const QRegularExpression shape(QStringLiteral("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]{1,6})?(Z|[+-][0-9]{2}:[0-9]{2})?\\z"));
    const auto match = shape.match(text); if (!match.hasMatch()) return 0;
    auto time = QDateTime::fromString(text, Qt::ISODateWithMs); if (!time.isValid()) return 0;
    if (match.captured(1).isEmpty()) time = QDateTime(time.date(), time.time(), QTimeZone("Asia/Seoul"));
    return time.isValid() ? time.toMSecsSinceEpoch() : 0;
}
} // namespace

struct UpbitAnnouncements::Private {
    UpbitAnnouncements* q; Vault& vault; std::unique_ptr<UpbitAnnouncementSocket> socket; Clock clock;
    QTimer ping, retry; bool active = false, connected = false, opening = false; int failures = 0;
    qint64 receivedAtMs = 0; QString reason = QStringLiteral("업비트 공지 연결 전"), noticeAccount;
    struct Entry { QString uuid; OfficialNotice notice; };
    QList<Entry> notices;
    Private(UpbitAnnouncements* owner, Vault& v) : q(owner), vault(v) {}
    QString accountId() const {
        if (!vault.isUnlocked()) return {};
        for (const auto& account : vault.accounts())
            if (account.provider == "upbit" && account.environment == "live") return account.id;
        return {};
    }
    void close() {
        retry.stop(); ping.stop(); opening = connected = false;
        if (socket) socket->abort();
    }
    void open() {
        if (!active || connected || opening) return;
        const auto id = accountId();
        if (id.isEmpty()) { reason = vault.isUnlocked() ? QStringLiteral("업비트 연결 없음") : QStringLiteral("금고 잠김"); emit q->changed(); return; }
        QByteArray authorizationHeader; QString error;
        const bool borrowed = vault.withCredentials(id, [&](const CredentialSet& credentials) {
            try { authorizationHeader = upbitConnectionAuthorization(credentials); } catch (...) { authorizationHeader.clear(); }
        }, &error);
        if (!borrowed || authorizationHeader.isEmpty()) { reason = QStringLiteral("업비트 키로 공지 연결을 서명하지 못했어요"); emit q->changed(); return; }
        QNetworkRequest request(UpbitAnnouncements::endpoint()); request.setRawHeader("Authorization", authorizationHeader);
        std::fill(authorizationHeader.begin(), authorizationHeader.end(), '\0');
        opening = true; noticeAccount = id; reason = QStringLiteral("업비트 공지 연결 중"); emit q->changed();
        socket->open(request);
    }
    void scheduleRetry() {
        if (!active || retry.isActive()) return;
        retry.start(int(qMin<qint64>(300000, 5000LL << qBound(0, failures - 1, 6)))); // 5s, 10s, ... up to 5 minutes.
    }
    void connectionFailed() {
        if (!opening && !connected) return;
        close(); ++failures; reason = QStringLiteral("업비트 공지 연결 끊김 · 다시 연결 중"); scheduleRetry(); emit q->changed();
    }
};

UpbitAnnouncements::UpbitAnnouncements(Vault& vault, QObject* parent)
    : UpbitAnnouncements(vault, std::make_unique<NetworkUpbitAnnouncementSocket>(), {}, parent) {}
UpbitAnnouncements::UpbitAnnouncements(Vault& vault, std::unique_ptr<UpbitAnnouncementSocket> socket, Clock clock, QObject* parent)
    : OfficialNoticeStream(parent), d_(std::make_unique<Private>(this, vault)) {
    d_->socket = std::move(socket); d_->socket->setParent(nullptr);
    d_->clock = clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); };
    d_->ping.setInterval(60000); d_->retry.setSingleShot(true);
    // Upbit closes idle sockets after 120 seconds; a text PING keeps the subscription alive.
    connect(&d_->ping, &QTimer::timeout, this, [this] { if (d_->connected) d_->socket->send("PING"); });
    connect(&d_->retry, &QTimer::timeout, this, [this] { d_->open(); });
    connect(d_->socket.get(), &UpbitAnnouncementSocket::connected, this, [this] {
        if (!d_->active || !d_->opening) return;
        d_->opening = false; d_->connected = true; d_->failures = 0; d_->receivedAtMs = d_->clock(); d_->reason.clear();
        d_->socket->send(subscription(QStringLiteral("keefi-notice-") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
        d_->ping.start(); emit changed();
    });
    connect(d_->socket.get(), &UpbitAnnouncementSocket::received, this, [this](const QByteArray& bytes) {
        if (!d_->connected) return;
        if (bytes.size() > MaximumMessageBytes) { d_->connectionFailed(); return; }
        const auto object = QJsonDocument::fromJson(bytes).object();
        if (object.value("status").toString() == "UP") { d_->receivedAtMs = d_->clock(); return; }
        if (object.contains("error")) { d_->reason = QStringLiteral("업비트 공지 구독 오류"); d_->close(); ++d_->failures; d_->scheduleRetry(); emit changed(); return; }
        OfficialNotice notice; QString uuid;
        if (!parse(bytes, &notice, &uuid)) return; // Unknown message shapes never become a notice.
        d_->receivedAtMs = d_->clock();
        d_->notices.removeIf([&](const auto& entry) { return entry.uuid == uuid; });
        d_->notices.append({uuid, notice});
        std::stable_sort(d_->notices.begin(), d_->notices.end(), [](const auto& a, const auto& b) { return a.notice.publishedAtMs > b.notice.publishedAtMs; });
        while (d_->notices.size() > MaximumNotices) d_->notices.removeLast();
        emit changed();
    });
    connect(d_->socket.get(), &UpbitAnnouncementSocket::failed, this, [this] { d_->connectionFailed(); });
    connect(&vault, &Vault::aboutToLock, this, [this] { d_->close(); d_->notices.clear(); d_->reason = QStringLiteral("금고 잠김"); emit changed(); });
    connect(&vault, &Vault::accountsChanged, this, [this] {
        d_->close();
        // Removing or replacing the Upbit connection also removes what it received.
        if (d_->accountId() != d_->noticeAccount) { d_->notices.clear(); d_->receivedAtMs = 0; d_->noticeAccount.clear(); }
        if (d_->active) d_->open(); else emit changed();
    });
}
UpbitAnnouncements::~UpbitAnnouncements() { d_->active = false; d_->close(); }
void UpbitAnnouncements::setActive(bool active) {
    if (d_->active == active) return; d_->active = active;
    if (!active) { d_->close(); d_->reason = QStringLiteral("업비트 공지 연결 쉼"); emit changed(); return; }
    d_->failures = 0; d_->open();
}
OfficialNoticeFeed UpbitAnnouncements::feed() const {
    OfficialNoticeFeed result; result.source = OfficialNoticeSource::Upbit;
    result.available = d_->connected; result.loading = d_->opening; result.receivedAtMs = d_->receivedAtMs; result.reason = d_->reason;
    for (const auto& entry : d_->notices) result.notices.append(entry.notice);
    return result;
}
QUrl UpbitAnnouncements::endpoint() { return QUrl(QStringLiteral("wss://api.upbit.com/websocket/v1/private")); }
QByteArray UpbitAnnouncements::subscription(const QString& ticket) {
    // Trade (거래지원 종료·유의) and wallet (입출금 중단) are the only categories the flow view shows.
    const QJsonArray message{QJsonObject{{"ticket", ticket}},
        QJsonObject{{"type", "announcement"}, {"categories", QJsonArray{"trade", "wallet"}}, {"include_body", false}}, QJsonObject{{"format", "DEFAULT"}}};
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}
bool UpbitAnnouncements::parse(const QByteArray& bytes, OfficialNotice* out, QString* uuid) {
    if (!out || !uuid || bytes.isEmpty() || bytes.size() > MaximumMessageBytes) return false;
    QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto o = document.object();
    if (o.value("type").toString() != "announcement") return false;
    const auto event = o.value("event_type").toString();
    if (event != "CREATED" && event != "UPDATED") return false;
    const auto id = o.value("uuid").toString(), title = o.value("title").toString(), category = o.value("category").toString();
    const auto url = o.value("url").toString(), listed = o.value("listed_at").toString();
    static const QStringList categories{"notice", "trade", "wallet", "maintenance", "digital_asset", "nft", "service", "event"};
    const auto article = url.size() <= 2048 ? articleUrl(url) : QString{};
    if (!boundedText(id, 128) || !boundedText(title, 2048) || !categories.contains(category) || article.isEmpty()) return false;
    const auto at = listedAt(listed); if (at <= 0) return false;
    OfficialNotice notice; notice.title = title.simplified(); notice.url = article; notice.categories = {category};
    notice.publishedAtMs = at; notice.publishedText = listed;
    *out = notice; *uuid = id; return true;
}
} // namespace keefi
