#include "keefi/RapidMoveMonitor.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QTimer>
#include <cmath>

namespace keefi {
namespace {
constexpr qint64 Minute = 60000, Grace = 2000, EndObservationAge = 5000, FreshAge = 10000, Cooldown = 600000;
constexpr int Window = 5, MarketLimit = 1000;
bool validDecimal(const QString& text, bool positive = false) {
    return text.size() <= 161 && text == text.trimmed() && (positive ? decimal::positive(text) : decimal::valid(text));
}
bool supported(const PublicTick& t) {
    static const QRegularExpression market(QStringLiteral("\\AKRW-[A-Z0-9]{1,20}\\z"));
    const auto& q = t.quote;
    return market.match(t.market).hasMatch() && q.baseAsset == t.market.mid(4) && q.quoteAsset == "KRW" &&
        q.evidence.complete && q.evidence.scope == "public:upbit:" + t.market && q.evidence.url == "wss://api.upbit.com/websocket/v1";
}
QString invalidReason(const PublicTick& t, qint64 now) {
    const auto& e = t.quote.evidence;
    if (!t.realtime || !t.streamEpoch) return QStringLiteral("실시간 관측을 모으는 중");
    if (!validDecimal(t.quote.price, true) || !validDecimal(t.tradeValueUtcDay)) return QStringLiteral("분간 거래대금 자료가 없어요");
    if (e.sourceTimeMs <= 0 || e.sourceTimeMs > now + Grace || e.receivedAtMs > now + Grace ||
        e.receivedAtMs < e.sourceTimeMs - Grace || now - e.sourceTimeMs > FreshAge || now - e.receivedAtMs > FreshAge ||
        t.tradeTimestampMs <= 0 || t.tradeTimestampMs > e.sourceTimeMs + Grace || e.sourceTimeMs - t.tradeTimestampMs > FreshAge)
        return QStringLiteral("새로운 관측이 더 필요해요");
    const auto date = QDateTime::fromString(t.tradeDateUtc, "yyyyMMdd").date();
    if (!date.isValid() || QDateTime::fromMSecsSinceEpoch(e.sourceTimeMs, Qt::UTC).date() != date ||
        QDateTime::fromMSecsSinceEpoch(t.tradeTimestampMs, Qt::UTC).date() != date)
        return QStringLiteral("같은 UTC 날짜의 관측이 더 필요해요");
    return {};
}
struct Point {
    qint64 minute = 0, at = 0, receipt = 0;
    QString price, counter, date;
};
struct Lane {
    quint64 epoch = 0;
    qint64 lastSource = 0, lastClosed = -1;
    QString lastCounter, date;
    QMap<qint64, Point> pending;
    QList<Point> closed;
    RapidMoveSnapshot result;
    MovementDirection episode = MovementDirection::Unknown;
    int calmMinutes = 0;
    qint64 lastUp = 0, lastDown = 0;
};
void invalidate(Lane& lane, const QString& reason) {
    lane.pending.clear(); lane.closed.clear(); lane.result = {};
    lane.result.reason = reason;
    lane.episode = MovementDirection::Unknown; lane.calmMinutes = 0;
}
}
struct RapidMoveMonitor::Private {
    RapidMoveMonitor* q;
    Clock clock;
    QTimer timer;
    bool enabled = false;
    QMap<QString, Lane> lanes;
    Private(RapidMoveMonitor* owner, Clock supplied) : q(owner), clock(std::move(supplied)) {
        if (!clock) clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
        timer.setInterval(1000);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { q->tick(); });
    }
    void close(const QString& market, Lane& lane, const Point& point, qint64 now) {
        const auto end = point.minute + Minute;
        lane.lastClosed = point.minute;
        if (end - point.at > EndObservationAge || point.at >= end || now - end > FreshAge) {
            lane.closed.clear(); lane.result = {}; lane.result.reason = QStringLiteral("분말 관측이 부족해요");
            lane.episode = MovementDirection::Unknown; lane.calmMinutes = 0;
            return;
        }
        if (!lane.closed.isEmpty() && (lane.closed.last().minute + Minute != point.minute || lane.closed.last().date != point.date)) {
            lane.closed.clear(); lane.episode = MovementDirection::Unknown; lane.calmMinutes = 0;
        }
        lane.closed.append(point);
        while (lane.closed.size() > Window + 1) lane.closed.removeFirst();
        lane.result = {}; auto& result = lane.result;
        result.completedMinutes = qMax(0, int(lane.closed.size()) - 1);
        result.closedMinuteMs = end; result.throughObservedAtMs = point.at; result.receivedAtMs = now; result.price = point.price;
        if (lane.closed.size() != Window + 1) {
            result.reason = QStringLiteral("5분 관측을 모으는 중 · %1/5분").arg(result.completedMinutes);
            return;
        }
        const auto& start = lane.closed.first();
        result.fromObservedAtMs = start.at;
        result.quoteTurnover = decimal::subtract(point.counter, start.counter);
        if (result.quoteTurnover.isEmpty()) { invalidate(lane, QStringLiteral("연속 거래대금 관측이 부족해요")); return; }
        const bool negative = decimal::compare(point.price, start.price) < 0;
        const auto difference = negative ? decimal::subtract(start.price, point.price) : decimal::subtract(point.price, start.price);
        const auto hundredDifference = decimal::multiply(difference, "100");
        const auto threePercent = decimal::multiply(start.price, "3");
        const auto halfThreshold = decimal::multiply(start.price, "1.5");
        if (hundredDifference.isEmpty() || threePercent.isEmpty() || halfThreshold.isEmpty()) { invalidate(lane, QStringLiteral("가격 변화 자료를 확인할 수 없어요")); return; }
        bool ok = false; const auto rate = hundredDifference.toDouble(&ok) / start.price.toDouble();
        if (!ok || !std::isfinite(rate)) { invalidate(lane, QStringLiteral("가격 변화 자료를 확인할 수 없어요")); return; }
        result.available = true;
        result.changePercent = QString::number(negative ? -rate : rate, 'f', 2);
        result.direction = MovementDirection::Flat;
        const bool reached = decimal::compare(hundredDifference, threePercent) >= 0;
        const bool turnoverReady = decimal::compare(result.quoteTurnover, "100000000") >= 0;
        const bool calm = decimal::compare(hundredDifference, halfThreshold) < 0;
        if (calm) {
            if (++lane.calmMinutes >= 2) lane.episode = MovementDirection::Unknown;
        } else lane.calmMinutes = 0;
        if (!reached || !turnoverReady) return;
        const auto direction = negative ? MovementDirection::Down : MovementDirection::Up;
        result.direction = direction;
        if (lane.episode == direction) return;
        lane.episode = direction; lane.calmMinutes = 0;
        auto& lastSent = direction == MovementDirection::Up ? lane.lastUp : lane.lastDown;
        if (lastSent > 0 && end - lastSent < Cooldown) return;
        lastSent = end;
        ChartPublicStudyEvent event;
        event.identity = {"coin", "upbit", market}; event.kind = "rapid_move";
        event.eventId = market + ':' + QString::number(lane.epoch) + ':' + QString::number(end) + (negative ? ":down" : ":up");
        event.detail = QStringLiteral("%1 · 5분 관측 %2%3% · 거래대금 %4 KRW").arg(market.mid(4), negative ? QStringLiteral("▼") : QStringLiteral("▲"), QString::number(rate, 'f', 2), result.quoteTurnover);
        event.atMs = end; event.receivedAtMs = now; event.direction = direction; event.liveEligible = true;
        emit q->eventReady(event);
    }
};
RapidMoveMonitor::RapidMoveMonitor(QObject* parent, Clock clock) : QObject(parent), d_(std::make_unique<Private>(this, std::move(clock))) {}
RapidMoveMonitor::~RapidMoveMonitor() = default;
void RapidMoveMonitor::setEnabled(bool enabled) {
    if (d_->enabled == enabled) return;
    d_->enabled = enabled; d_->lanes.clear();
    if (enabled) d_->timer.start(); else d_->timer.stop();
    emit changed();
}
bool RapidMoveMonitor::enabled() const { return d_->enabled; }
void RapidMoveMonitor::observe(const QList<PublicTick>& ticks) {
    if (!d_->enabled || ticks.size() > MarketLimit) return;
    const auto now = d_->clock();
    tick();
    bool changedAny = false;
    for (const auto& t : ticks) {
        if (!supported(t) || (!d_->lanes.contains(t.market) && d_->lanes.size() >= MarketLimit)) continue;
        auto& lane = d_->lanes[t.market];
        const auto at = t.quote.evidence.sourceTimeMs;
        if (at <= lane.lastSource) continue;
        const auto reason = invalidReason(t, now);
        if (!reason.isEmpty()) {
            changedAny = changedAny || lane.result.reason != reason || lane.result.available || !lane.closed.isEmpty() || !lane.pending.isEmpty();
            invalidate(lane, reason); continue;
        }
        if (lane.epoch != t.streamEpoch || lane.date != t.tradeDateUtc ||
            (!lane.lastCounter.isEmpty() && decimal::compare(t.tradeValueUtcDay, lane.lastCounter) < 0)) {
            invalidate(lane, QStringLiteral("연속 관측을 다시 모으는 중"));
            lane.lastClosed = -1; changedAny = true;
        }
        lane.epoch = t.streamEpoch; lane.date = t.tradeDateUtc; lane.lastCounter = t.tradeValueUtcDay; lane.lastSource = at;
        const auto minute = at / Minute * Minute;
        if (minute <= lane.lastClosed) continue;
        lane.pending[minute] = {minute, at, t.quote.evidence.receivedAtMs, t.quote.price, t.tradeValueUtcDay, t.tradeDateUtc};
        while (lane.pending.size() > 2) lane.pending.erase(lane.pending.begin());
    }
    if (changedAny) emit changed();
}
void RapidMoveMonitor::tick() {
    if (!d_->enabled) return;
    const auto now = d_->clock(), boundary = (now - Grace) / Minute * Minute;
    bool changedAny = false;
    for (auto it = d_->lanes.begin(); it != d_->lanes.end(); ++it) {
        auto& lane = it.value();
        while (!lane.pending.isEmpty() && lane.pending.firstKey() + Minute <= boundary) {
            const auto point = lane.pending.take(lane.pending.firstKey());
            d_->close(it.key(), lane, point, now); changedAny = true;
        }
        if (lane.lastClosed >= 0 && lane.lastClosed + Minute < boundary && !lane.closed.isEmpty()) {
            invalidate(lane, QStringLiteral("연속 분 관측이 부족해요")); changedAny = true;
        }
    }
    if (changedAny) emit changed();
}
RapidMoveSnapshot RapidMoveMonitor::snapshot(const QString& market) const {
    if (!d_->enabled) { RapidMoveSnapshot r; r.reason = QStringLiteral("꺼짐"); return r; }
    if (d_->lanes.contains(market)) return d_->lanes.value(market).result;
    RapidMoveSnapshot r; r.reason = QStringLiteral("실시간 관측을 모으는 중"); return r;
}
}
