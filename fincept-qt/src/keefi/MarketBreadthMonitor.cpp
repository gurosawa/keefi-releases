#include "keefi/MarketBreadthMonitor.h"
#include "keefi/TradingTypes.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>

namespace keefi {
namespace {
constexpr qint64 Minute = 60000, FreshAge = DisplayFreshnessMs;
struct Frame {
    qint64 capturedAt = 0;
    QList<PublicMarketRow> coins;
    QList<PublicBoardItem> stocks;
    QString error;
};
std::optional<double> rateDirection(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("\\A-?[0-9]{1,40}(?:\\.[0-9]{1,40})?(?:[eE][-+]?[0-9]{1,3})?\\z"));
    if (!pattern.match(value).hasMatch()) return {};
    const auto mantissa = value.section(QRegularExpression("[eE]"), 0, 0);
    const bool nonzero = std::any_of(mantissa.cbegin(), mantissa.cend(), [](QChar c) { return c >= '1' && c <= '9'; });
    return nonzero ? (value.startsWith('-') ? -1.0 : 1.0) : 0.0;
}
QByteArray fingerprint(const Frame& frame) {
    QStringList parts;
    for (const auto& r : frame.coins) parts.append(r.market + '|' + r.signedChangeRate + '|' + r.price + '|' + QString::number(r.priced) + '|' + QString::number(r.sourceTimeMs) + '|' + QString::number(r.receivedAtMs));
    for (const auto& r : frame.stocks) parts.append(r.id + '|' + r.symbol + '|' + r.changePercent + '|' + QString::number(r.receivedAtMs));
    parts.sort();
    return QCryptographicHash::hash((frame.error + '\n' + parts.join('\n')).toUtf8(), QCryptographicHash::Sha256);
}
MarketBreadthSnapshot calculate(const QString& market, const Frame& frame, qint64 completed) {
    MarketBreadthSnapshot out;
    out.market = market; out.capturedAtMs = frame.capturedAt; out.completedAtMs = completed;
    out.basis = market == "coin" ? QStringLiteral("업비트 원화 · 전일 종가 대비") :
        market == "kr" ? QStringLiteral("코스피 상위 표본 · 기준 거래일 미제공") : QStringLiteral("미국 상위 표본 · 기준 거래일 미제공");
    out.source = market == "coin" ? QStringLiteral("업비트 공개 ticker") : market == "kr" ? QStringLiteral("네이버 증권 코스피 시총 상위 50") : QStringLiteral("Nasdaq 시총 상위 80");
    QList<ExtendedBreadthObservation> observations;
    if (market != "coin") {
        for (const auto& item : frame.stocks) {
            const auto id = item.id.isEmpty() ? item.symbol : item.id;
            out.populationIds.append(market + ':' + id);
            observations.append({market + ':' + id, {}, false});
        }
        out.breadth = extendedBreadth(observations);
        out.reason = QStringLiteral("등락 기준 거래일을 확인할 수 없어요");
        out.breadth.state.reason = out.reason;
        return out;
    }
    static const QRegularExpression code(QStringLiteral("\\AKRW-[A-Z0-9]{1,24}\\z"));
    const auto observedDate = QDateTime::fromMSecsSinceEpoch(frame.capturedAt, Qt::UTC).date();
    for (const auto& row : frame.coins) {
        out.populationIds.append(row.market);
        const auto direction = rateDirection(row.signedChangeRate);
        const bool fresh = code.match(row.market).hasMatch() && row.base == row.market.mid(4) && row.priced &&
            decimal::positive(row.price) && row.sourceTimeMs > 0 && row.receivedAtMs > 0 &&
            row.sourceTimeMs <= frame.capturedAt && row.receivedAtMs <= frame.capturedAt &&
            row.receivedAtMs >= row.sourceTimeMs - 2000 && completed - row.sourceTimeMs <= FreshAge &&
            completed - row.receivedAtMs <= FreshAge &&
            QDateTime::fromMSecsSinceEpoch(row.sourceTimeMs, Qt::UTC).date() == observedDate;
        observations.append({row.market, direction, fresh});
        if (fresh && direction) {
            if (!out.earliestSourceTimeMs || row.sourceTimeMs < out.earliestSourceTimeMs) out.earliestSourceTimeMs = row.sourceTimeMs;
            out.latestSourceTimeMs = qMax(out.latestSourceTimeMs, row.sourceTimeMs);
        }
    }
    out.breadth = extendedBreadth(observations);
    if (!frame.error.isEmpty()) {
        out.breadth.state = {false, IndicatorDirection::None, frame.error}; out.breadth.percent.reset();
    }
    out.reason = out.breadth.state.reason;
    return out;
}
}
struct MarketBreadthMonitor::Private {
    MarketBreadthMonitor* q;
    Clock clock;
    QTimer timer;
    bool enabled = false;
    QMap<QString, QMap<qint64, Frame>> pending;
    QMap<QString, QByteArray> fingerprints;
    QMap<QString, MarketBreadthSnapshot> results;
    Private(MarketBreadthMonitor* owner, Clock supplied) : q(owner), clock(std::move(supplied)) {
        if (!clock) clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
        timer.setInterval(1000); QObject::connect(&timer, &QTimer::timeout, q, [this] { q->tick(); });
    }
    void observe(const QString& market, Frame frame) {
        const auto signature = fingerprint(frame);
        if (fingerprints.value(market) == signature) return;
        fingerprints[market] = signature;
        const auto minute = frame.capturedAt / Minute * Minute;
        pending[market][minute] = std::move(frame);
        while (pending[market].size() > 2) pending[market].erase(pending[market].begin());
    }
};
MarketBreadthMonitor::MarketBreadthMonitor(QObject* p, Clock clock) : QObject(p), d_(std::make_unique<Private>(this, std::move(clock))) {}
MarketBreadthMonitor::~MarketBreadthMonitor() = default;
void MarketBreadthMonitor::setEnabled(bool enabled) {
    if (d_->enabled == enabled) return;
    d_->enabled = enabled; d_->pending.clear(); d_->results.clear(); d_->fingerprints.clear();
    if (enabled) d_->timer.start(); else d_->timer.stop();
    emit changed();
}
bool MarketBreadthMonitor::enabled() const { return d_->enabled; }
void MarketBreadthMonitor::observeCoins(const QList<PublicMarketRow>& rows) {
    if (!d_->enabled) return;
    tick(); Frame frame; frame.capturedAt = d_->clock();
    if (rows.size() <= 1000) frame.coins = rows; else frame.error = QStringLiteral("관측 표본 범위를 확인할 수 없어요");
    d_->observe("coin", std::move(frame));
}
void MarketBreadthMonitor::observeStocks(const QString& market, const QList<PublicBoardItem>& rows) {
    if (!d_->enabled || (market != "kr" && market != "us")) return;
    tick(); Frame frame; frame.capturedAt = d_->clock();
    if (rows.size() <= (market == "kr" ? 50 : 80)) frame.stocks = rows;
    d_->observe(market, std::move(frame));
}
void MarketBreadthMonitor::tick() {
    if (!d_->enabled) return;
    const auto now = d_->clock(); bool updated = false;
    for (auto it = d_->pending.begin(); it != d_->pending.end(); ++it) {
        auto& frames = it.value();
        while (!frames.isEmpty() && frames.firstKey() + Minute <= now) {
            const auto minute = frames.firstKey(); const auto frame = frames.take(minute);
            d_->results[it.key()] = calculate(it.key(), frame, minute + Minute); updated = true;
        }
    }
    for (auto& result : d_->results) {
        if (result.breadth.state.available && now - result.completedAtMs > FreshAge) {
            result.reason = QStringLiteral("새로운 표본 관측이 필요해요");
            result.breadth.state = {false, IndicatorDirection::None, result.reason}; result.breadth.percent.reset(); updated = true;
        }
    }
    if (updated) emit changed();
}
MarketBreadthSnapshot MarketBreadthMonitor::snapshot(const QString& market) const {
    if (!d_->enabled) { MarketBreadthSnapshot r; r.market = market; r.reason = QStringLiteral("꺼짐"); r.breadth.state.reason = r.reason; return r; }
    if (d_->results.contains(market)) return d_->results.value(market);
    MarketBreadthSnapshot r; r.market = market; r.reason = QStringLiteral("완료한 1분 표본을 모으는 중"); r.breadth.state.reason = r.reason; return r;
}
}
