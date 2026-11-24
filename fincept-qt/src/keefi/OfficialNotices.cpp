#include "keefi/OfficialNotices.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
namespace keefi {
namespace {
bool knownSource(OfficialNoticeSource source) {
    return source == OfficialNoticeSource::Okx || source == OfficialNoticeSource::Bithumb;
}
bool boundedText(const QString& text, qsizetype maximum) {
    if (text.trimmed().isEmpty() || text.size() > maximum) return false;
    for (auto c : text) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
bool sourceLocalTime(const QString& text) {
    if (!QRegularExpression("\\A[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\\z").match(text).hasMatch()) return false;
    return QDate::fromString(text.left(10), "yyyy-MM-dd").isValid() && QTime::fromString(text.mid(11), "HH:mm:ss").isValid();
}
}
QString OfficialNotices::sourceName(OfficialNoticeSource source) {
    if (source == OfficialNoticeSource::Okx) return QStringLiteral("OKX Global");
    if (source == OfficialNoticeSource::Bithumb) return QStringLiteral("빗썸");
    if (source == OfficialNoticeSource::Upbit) return QStringLiteral("업비트");
    return QStringLiteral("미지원 원천");
}
QString OfficialNotices::matchingUnavailableReason(const QString& provider, const QString& market) {
    Q_UNUSED(market);
    if (provider == "phantom") return QStringLiteral("공지 연결 미지원 · Solana Mint와 거래소 자산의 검증된 매핑 없음");
    if (provider == "okx" || provider == "bithumb")
        return QStringLiteral("자동 보유 매칭 미지원 · 공지에 구조화된 자산·체인 식별자 없음");
    return QStringLiteral("공지 연결 미지원 · 이 서비스의 공개 공지 수집 경로를 연결하지 않았습니다.");
}
RequestSpec OfficialNotices::request(OfficialNoticeSource source) {
    RequestSpec r; r.operation = Operation::Markets;
    if (source == OfficialNoticeSource::Okx) {
        r.url = QUrl("https://openapi.okx.com/api/v5/support/announcements");
        r.headers.insert("Accept-Language", "en-US");
    } else if (source == OfficialNoticeSource::Bithumb) r.url = QUrl("https://api.bithumb.com/v1/notices?count=20");
    r.headers.insert("Accept", "application/json"); return r;
}
bool OfficialNotices::isRequest(const RequestSpec& r) {
    if (r.operation != Operation::Markets || r.method != "GET" || r.authenticated || !r.body.isEmpty()) return false;
    for (auto source : {OfficialNoticeSource::Okx, OfficialNoticeSource::Bithumb}) {
        const auto expected = request(source);
        if (r.url == expected.url && r.headers == expected.headers) return true;
    }
    return false;
}
bool OfficialNotices::allowedArticle(OfficialNoticeSource source, const QUrl& url) {
    if (source == OfficialNoticeSource::Upbit) // Upbit article ids are a query parameter on one fixed path.
        return url.isValid() && QRegularExpression("\\Ahttps://(?:www\\.)?upbit\\.com/service_center/notice\\?id=[0-9]{1,20}\\z").match(url.toString(QUrl::FullyEncoded)).hasMatch();
    if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty() || url.port() != -1 ||
        url.hasQuery() || url.hasFragment()) return false;
    const auto encoded = url.toString(QUrl::FullyEncoded);
    if (source == OfficialNoticeSource::Okx)
        return QRegularExpression("\\Ahttps://www\\.okx\\.com/help/[A-Za-z0-9][A-Za-z0-9-]{0,500}\\z").match(encoded).hasMatch();
    if (source == OfficialNoticeSource::Bithumb)
        return QRegularExpression("\\Ahttps://feed\\.bithumb\\.com/notice/[0-9]{1,20}\\z").match(encoded).hasMatch();
    return false;
}
bool OfficialNotices::parse(OfficialNoticeSource source, const QByteArray& body, qint64 received, OfficialNoticeFeed* out) {
    if (!out) return false;
    *out = {}; out->source = source; out->reason = QStringLiteral("공식 공지 형식·시각·원문 주소를 확인할 수 없습니다.");
    if (!knownSource(source) || received <= 0 || body.isEmpty() || body.size() > 512 * 1024) return false;
    QJsonParseError error; const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) return false;
    QJsonArray rows;
    if (source == OfficialNoticeSource::Okx) {
        const auto root = document.object(); const auto data = root.value("data").toArray();
        if (!document.isObject() || root.value("code").toString() != "0" || data.size() != 1 || !data[0].isObject() ||
            !data[0].toObject().value("details").isArray()) return false;
        rows = data[0].toObject().value("details").toArray();
    } else { if (!document.isArray()) return false; rows = document.array(); }
    if (rows.size() > 20) return false;
    QList<OfficialNotice> notices; QSet<QString> urls;
    for (const auto& value : rows) {
        if (!value.isObject()) return false;
        const auto row = value.toObject(); OfficialNotice notice; notice.title = row.value("title").toString();
        notice.url = row.value(source == OfficialNoticeSource::Okx ? "url" : "pc_url").toString();
        if (!boundedText(notice.title, 2048) || !allowedArticle(source, QUrl(notice.url, QUrl::StrictMode)) || urls.contains(notice.url)) return false;
        if (source == OfficialNoticeSource::Okx) {
            const auto category = row.value("annType").toString(); if (!boundedText(category, 128)) return false;
            notice.categories.append(category);
            const auto time = row.value("pTime").toString();
            if (!QRegularExpression("\\A[0-9]{1,16}\\z").match(time).hasMatch()) return false;
            bool ok = false; notice.publishedAtMs = time.toLongLong(&ok);
            if (!ok || notice.publishedAtMs <= 0 || notice.publishedAtMs > received) return false;
        } else {
            if (!row.value("categories").isArray()) return false;
            const auto categories = row.value("categories").toArray(); if (categories.size() > 8) return false;
            for (const auto& c : categories) { if (!c.isString() || !boundedText(c.toString(), 128)) return false; notice.categories.append(c.toString()); }
            notice.publishedText = row.value("published_at").toString(); notice.modifiedText = row.value("modified_at").toString();
            if (!sourceLocalTime(notice.publishedText) || (!notice.modifiedText.isEmpty() && !sourceLocalTime(notice.modifiedText))) return false;
        }
        urls.insert(notice.url); notices.append(notice);
    }
    out->notices = notices; out->receivedAtMs = received; out->available = true; out->reason.clear(); return true;
}
struct OfficialNotices::Private {
    QPointer<TradingTransport> transport; Clock clock; QTimer timer;
    bool active = false, pending = false; quint64 token = 0;
    QList<OfficialNoticeSource> queue; QMap<OfficialNoticeSource, OfficialNoticeFeed> feeds;
    QPointer<OfficialNoticeStream> upbit;
    OfficialNoticeSource current = OfficialNoticeSource::Okx;
    void next() {
        if (!active || pending || queue.isEmpty() || !transport) return;
        current = queue.takeFirst(); pending = true; transport->send(++token, OfficialNotices::request(current));
    }
    void stop() {
        ++token; pending = false; queue.clear();
        if (transport) transport->abort();
        for (auto& f : feeds) if (f.loading) { f.loading = false; f.reason = QStringLiteral("공지 조회 중단"); }
    }
};
OfficialNotices::OfficialNotices(TradingTransport* transport, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->transport = transport; d_->clock = clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); };
    for (auto source : {OfficialNoticeSource::Okx, OfficialNoticeSource::Bithumb}) {
        OfficialNoticeFeed f; f.source = source; f.reason = QStringLiteral("공식 공지 조회 전"); d_->feeds.insert(source, f);
    }
    d_->timer.setInterval(5 * 60000); connect(&d_->timer, &QTimer::timeout, this, &OfficialNotices::refresh);
    if (transport) connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& response) {
        if (!d_->active || !d_->pending || token != d_->token) return;
        auto& f = d_->feeds[d_->current]; const auto source = d_->current;
        f = {}; f.source = source; d_->pending = false;
        if (response.status == 200 && !response.networkError && !response.timedOut && response.localRetryAfterSeconds <= 0)
            parse(source, response.body, d_->clock(), &f);
        else f.reason = response.status > 0 ? QStringLiteral("공식 공지 조회 실패 · HTTP %1").arg(response.status) : QStringLiteral("공식 공지 연결 끊김");
        emit changed(); QTimer::singleShot(0, this, [this] { d_->next(); });
    });
}
OfficialNotices::~OfficialNotices() { d_->active = false; d_->stop(); if (d_->upbit) { disconnect(d_->upbit, nullptr, this, nullptr); d_->upbit->setActive(false); } }
void OfficialNotices::setUpbitAnnouncements(OfficialNoticeStream* upbit) {
    if (d_->upbit == upbit) return;
    if (d_->upbit) { disconnect(d_->upbit, nullptr, this, nullptr); d_->upbit->setActive(false); }
    d_->upbit = upbit;
    if (upbit) { connect(upbit, &OfficialNoticeStream::changed, this, &OfficialNotices::changed); upbit->setActive(d_->active); }
    emit changed();
}
void OfficialNotices::setActive(bool active) {
    if (d_->active == active) return; d_->active = active;
    if (d_->upbit) d_->upbit->setActive(active);
    if (!active) { d_->timer.stop(); d_->stop(); emit changed(); return; }
    d_->timer.start(); refresh();
}
void OfficialNotices::refresh() {
    if (!d_->active || d_->pending) return;
    d_->queue = {OfficialNoticeSource::Okx, OfficialNoticeSource::Bithumb};
    for (auto source : d_->queue) {
        auto& f = d_->feeds[source]; f = {}; f.source = source; f.loading = bool(d_->transport);
        f.reason = d_->transport ? QStringLiteral("공식 공지 불러오는 중") : QStringLiteral("공개 공지 조회 연결 없음");
    }
    emit changed(); d_->next();
}
OfficialNoticeFeed OfficialNotices::feed(OfficialNoticeSource source, qint64 now) const {
    if (source == OfficialNoticeSource::Upbit) {
        if (d_->upbit) return d_->upbit->feed();
        OfficialNoticeFeed none; none.source = source; none.reason = QStringLiteral("업비트 공지 연결 없음"); return none;
    }
    auto result = d_->feeds.value(source); if (!now) now = d_->clock();
    if (result.available && (result.receivedAtMs > now || now - result.receivedAtMs > FreshnessMs)) {
        result.available = false; result.reason = QStringLiteral("공지 수신 후 30분 경과 또는 시각 불일치 · 다시 조회 필요");
    }
    if (!result.available) result.notices.clear();
    return result;
}
}
