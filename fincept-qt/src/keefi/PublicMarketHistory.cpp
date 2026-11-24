#include "keefi/PublicMarketHistory.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
bool valid(const HistoryTarget& t) {
    return t.section == "coin" && QRegularExpression("\\A[A-Z0-9]{2,15}\\z").match(t.id).hasMatch() &&
        (t.sourceId.isEmpty() || QRegularExpression("\\A[a-z0-9][a-z0-9-]{0,100}\\z").match(t.sourceId).hasMatch());
}
} // namespace

RequestSpec PublicMarketHistory::request(const HistoryTarget& t) {
    RequestSpec r; r.method = "GET"; r.operation = Operation::Markets;
    if (!valid(t)) return r;
    r.headers.insert("Accept", "application/json");
    r.headers.insert("User-Agent", "Mozilla/5.0 (KEEFI public watchlist)");
    QUrlQuery q;
    if (!t.sourceId.isEmpty()) {
        r.url = QUrl("https://api.coingecko.com/api/v3/coins/" + t.sourceId + "/market_chart");
        q.addQueryItem("vs_currency", "krw"); q.addQueryItem("days", "1");
    } else {
        r.url = QUrl("https://api.upbit.com/v1/candles/minutes/10");
        q.addQueryItem("market", "KRW-" + t.id); q.addQueryItem("count", "200");
    }
    r.url.setQuery(q);
    return r;
}
bool PublicMarketHistory::isRequest(const RequestSpec& r) {
    if (r.method != "GET" || r.authenticated || !r.body.isEmpty() || r.operation != Operation::Markets) return false;
    HistoryTarget t;
    const auto path = r.url.path(); const QUrlQuery q(r.url);
    if (r.url.host() == "api.coingecko.com") t = {"coin", "BTC", path.section('/', 4, 4)};
    else if (r.url.host() == "api.upbit.com") t = {"coin", q.queryItemValue("market").mid(4), {}};
    else return false;
    if (!valid(t)) return false;
    const auto expected = request(t);
    return r.url == expected.url && r.headers == expected.headers;
}
bool PublicMarketHistory::parse(const HistoryTarget& t, const QByteArray& body, qint64 now, PublicHistory* result) {
    if (!valid(t) || !result || body.size() > 2 * 1024 * 1024) return false;
    const auto doc = QJsonDocument::fromJson(body);
    PublicHistory parsed;
    const auto add = [&](qint64 at, double price) {
        if (at <= 0 || at > now + 300000 || !(price > 0) || !std::isfinite(price)) return;
        if (at < now - 86400000) return;
        parsed.points.append(QPointF(at, price));
    };
    if (!t.sourceId.isEmpty()) {
        parsed.source = "CoinGecko · 최근 24시간 · KRW";
        for (const auto& v : doc.object().value("prices").toArray()) {
            const auto pair = v.toArray();
            if (pair.size() == 2 && pair[0].isDouble() && pair[1].isDouble()) add(qint64(pair[0].toDouble()), pair[1].toDouble());
        }
    } else {
        parsed.source = "업비트 · 최근 24시간 · 10분 종가 · KRW";
        for (const auto& v : doc.array()) {
            const auto o = v.toObject();
            if (o.value("market").toString() != "KRW-" + t.id) continue;
            add(QDateTime::fromString(o.value("candle_date_time_utc").toString() + "Z", Qt::ISODate).toMSecsSinceEpoch(), o.value("trade_price").toDouble());
        }
    }
    std::stable_sort(parsed.points.begin(), parsed.points.end(), [](auto a, auto b) { return a.x() < b.x(); });
    parsed.points.erase(std::unique(parsed.points.begin(), parsed.points.end(), [](auto a, auto b) { return a.x() == b.x(); }), parsed.points.end());
    if (parsed.points.size() < 2 || parsed.points.size() > 3000) return false;
    parsed.receivedAtMs = now;
    *result = parsed;
    return true;
}

struct PublicMarketHistory::Private {
    PublicMarketHistory* q;
    TradingTransport* transport;
    QList<HistoryTarget> targets, queue;
    QHash<QString, PublicHistory> cache;
    QHash<QString, qint64> attempts;
    QTimer timer, pacing;
    quint64 token = 0, nextToken = 1;
    HistoryTarget current;
    void enqueue() {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto& t : targets) {
            if (!valid(t) || (token && current.key() == t.key()) || now - attempts.value(t.key()) < 60000) continue;
            if (std::none_of(queue.cbegin(), queue.cend(), [&](const auto& old) { return old.key() == t.key(); })) queue.append(t);
        }
        next();
    }
    void next() {
        if (!transport || token || pacing.isActive() || queue.isEmpty()) return;
        current = queue.takeFirst();
        attempts[current.key()] = QDateTime::currentMSecsSinceEpoch();
        token = nextToken++;
        transport->send(token, PublicMarketHistory::request(current));
    }
};
PublicMarketHistory::PublicMarketHistory(TradingTransport* transport, QObject* parent)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->q = this; d_->transport = transport;
    d_->timer.setInterval(60000);
    d_->pacing.setInterval(2200); // At most ~27 requests/min across the visible watchlist.
    d_->pacing.setSingleShot(true);
    connect(&d_->timer, &QTimer::timeout, this, [this] { d_->enqueue(); });
    connect(&d_->pacing, &QTimer::timeout, this, [this] { d_->next(); });
    if (transport) connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) {
        if (token != d_->token) return;
        d_->token = 0;
        PublicHistory parsed;
        if (r.status == 200 && !r.timedOut && !r.networkError && parse(d_->current, r.body, QDateTime::currentMSecsSinceEpoch(), &parsed))
            d_->cache[d_->current.key()] = parsed;
        else {
            auto& old = d_->cache[d_->current.key()];
            old.status = r.status == 429 ? tr("차트 요청 제한 · 다음 갱신 대기") : tr("차트 미수신 · 갱신 대기");
        }
        d_->pacing.start();
        emit changed();
    });
}
PublicMarketHistory::~PublicMarketHistory() = default;
void PublicMarketHistory::setTargets(const QList<HistoryTarget>& targets) {
    d_->targets.clear();
    for (const auto& target : targets) if (valid(target)) d_->targets.append(target);
    d_->queue.erase(std::remove_if(d_->queue.begin(), d_->queue.end(), [&](const auto& old) {
        return std::none_of(d_->targets.cbegin(), d_->targets.cend(), [&](const auto& t) { return old.key() == t.key(); });
    }), d_->queue.end());
    if (d_->targets.isEmpty()) d_->timer.stop(); else if (!d_->timer.isActive()) d_->timer.start();
    d_->enqueue();
}
PublicHistory PublicMarketHistory::history(const QString& section, const QString& id) const {
    if (section == "kr" || section == "us") {
        PublicHistory unavailable;unavailable.status=tr("증권사를 연결하면 볼 수 있어요");return unavailable;
    }
    auto result = d_->cache.value(section + ":" + id);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (section == "coin") result.points.erase(std::remove_if(result.points.begin(), result.points.end(),
        [now](auto p) { return p.x() < now - 86400000; }), result.points.end());
    if (!result.status.isEmpty() || now - result.receivedAtMs > 180000 || result.points.size() < 2) {
        result.points.clear();
        if (result.status.isEmpty()) result.status = tr("차트 갱신 대기");
    }
    return result;
}
} // namespace keefi
