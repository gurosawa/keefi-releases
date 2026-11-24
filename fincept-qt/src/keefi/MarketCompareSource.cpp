#include "keefi/MarketCompareSource.h"

#include "keefi/CollectorStore.h"
#include "keefi/TradingTransport.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <optional>

namespace keefi {
namespace {
constexpr qint64 Minute = 60000, Hour = 3600000;
constexpr qint64 CatalogMaxAgeMs = 6 * Hour, EcbMaxAgeMs = 12 * Hour, CollectorReadEveryMs = 5 * Minute;
constexpr int PaceMs = 250;

QString identityOf(const CompareSpec& spec) { return spec.identity; }
} // namespace

struct MarketCompareSource::Private {
    enum class Job { Catalog, Klines, Ecb };
    struct Pending {
        Job job = Job::Klines;
        CompareSpec spec;
        qint64 intervalMs = 0, startedAtMs = 0;
        quint64 token = 0;
    };

    MarketCompareSource* q = nullptr;
    TradingTransport* transport = nullptr;
    Clock clock;
    bool active = false;
    qint64 interval = Hour;
    CompareWonMode wonMode = CompareWonMode::Dollar;
    QString collectorRoot;
    QHash<QString, QString> perps;
    qint64 catalogAt = 0, ecbAt = 0, collectorReadAt = 0, lastSendAt = 0, collectorWrittenAt = 0;
    qint64 hanaConfirmedThrough = 0;
    QString catalogReason, fxReason, collectorReason;
    QHash<QString, CompareSeries> series;
    QList<FxRate> ecb, hana, collectorEcb;
    QList<Pending> queue;
    std::optional<Pending> running;
    quint64 nextToken = 1;
    QTimer pacing, refreshTimer;
    qint64 nextRefreshAt = 0;

    qint64 now() const { return clock ? clock() : QDateTime::currentMSecsSinceEpoch(); }

    QList<CompareSpec> specs() const {
        auto out = MarketCompare::defaultSpecs();
        if (wonMode != CompareWonMode::Dollar) out.last() = MarketCompare::upbitBitcoin();
        return out;
    }

    void readCollectorStatus() {
        collectorWrittenAt = collectorRoot.isEmpty() ? 0 : CollectorReader(collectorRoot).status().value("writtenAt").toInteger();
    }

    void readCollector() {
        if (collectorRoot.isEmpty() || now() - collectorReadAt < CollectorReadEveryMs) return;
        collectorReadAt = now();
        CollectorReader reader(collectorRoot);
        QString reason;
        const auto segments = reader.segments(&reason);
        if (segments.isEmpty()) { collectorReason = reason.isEmpty() ? QStringLiteral("수집기 기록이 아직 없어요") : reason; return; }
        QList<FxRate> hanaRates, ecbRates;
        qint64 confirmedThrough = 0;
        QSet<QString> ecbDays;
        // Hana postings serve intraday bars: the longest intraday page is 200 four-hour bars, plus a day.
        // Reading by how old the page can be (not "the last three days") keeps a past bar's posting stable.
        auto from = (now() - (200 * 4 * Hour + 24 * Hour)) / Hour * Hour;
        // A failed refresh keeps the last successful page, even after a long outage.
        // Its historical FX must survive for exactly as long as those bars remain visible.
        const auto retained = series.constFind(MarketCompare::upbitBitcoin().identity);
        if (interval < 24 * Hour && retained != series.cend() && retained->available && retained->intervalMs == interval && !retained->bars.isEmpty())
            from = std::min(from, (retained->bars.first().openMs - 24 * Hour) / Hour * Hour);
        for (const auto& segment : segments) {
            if (segment.kind != QLatin1String("fx") || segment.symbol != QLatin1String("USDKRW")) continue;
            const bool isHana = segment.venue == QLatin1String("naver-hana");
            const bool isEcb = segment.venue == QLatin1String("ecb");
            if ((!isHana && !isEcb) || (isHana && segment.hourStartMs < from)) continue;
            QString readReason;
            bool validHana = false;
            for (const auto& row : reader.read(segment, &readReason)) {
                FxRate rate;
                rate.krwPerUsd = row.value("x").toString();
                rate.value = rate.krwPerUsd.toDouble();
                rate.postedAtMs = row.value("p").toInteger();
                rate.receivedAtMs = row.value("r").toInteger();
                if (!(rate.value > 0) || rate.postedAtMs <= 0 || rate.receivedAtMs <= 0) continue;
                if (isHana) {
                    rate.source = QStringLiteral("hana");
                    rate.knownAtMs = rate.receivedAtMs;
                    hanaRates.append(rate);
                    validHana = true;
                } else {
                    rate.source = QStringLiteral("ecb");
                    rate.date = QDate::fromString(row.value("d").toString(), Qt::ISODate);
                    rate.knownAtMs = row.value("k").toInteger();
                    if (!rate.date.isValid() || rate.knownAtMs <= 0 || ecbDays.contains(rate.date.toString(Qt::ISODate))) continue;
                    ecbDays.insert(rate.date.toString(Qt::ISODate)); // First observation wins.
                    ecbRates.append(rate);
                }
            }
            if (validHana && readReason.isEmpty() && segment.hourStartMs + Hour <= now())
                confirmedThrough = std::max(confirmedThrough, segment.hourStartMs + Hour);
            if (!readReason.isEmpty()) reason = readReason;
        }
        const auto byKnown = [](const FxRate& a, const FxRate& b) { return a.knownAtMs < b.knownAtMs; };
        std::sort(hanaRates.begin(), hanaRates.end(), byKnown);
        std::sort(ecbRates.begin(), ecbRates.end(), byKnown);
        hana = hanaRates;
        hanaConfirmedThrough = confirmedThrough;
        collectorEcb = ecbRates;
        collectorReason = reason;
    }

    // A catalog refresh can revoke a previously verified contract even if its
    // last candle request succeeded and that series wasn't queued for retry.
    bool verifyContract(const CompareSpec& spec, qint64 intervalMs) {
        if (spec.venue != CompareVenue::BinanceUsdM) return true;
        QString reason;
        if (!perps.isEmpty() && MarketCompare::perpAllowed(spec, perps, &reason)) return true;
        if (perps.isEmpty())
            reason = QStringLiteral("선물 종목 목록을 확인하지 못해 보류") + (catalogReason.isEmpty() ? QString{} : QStringLiteral(" · ") + catalogReason);
        CompareSeries blocked;
        blocked.spec = spec;
        blocked.intervalMs = intervalMs;
        blocked.reason = reason;
        series.insert(spec.identity, blocked);
        return false;
    }

    enum class Batch { Full, RetryFailures };
    void schedule(Batch batch) {
        if (!active || !transport) return;
        const bool wasLoading = running.has_value() || !queue.isEmpty();
        queue.clear();
        const auto at = now();
        const bool full = batch == Batch::Full;
        const bool catalogDue = perps.isEmpty() || at - catalogAt >= CatalogMaxAgeMs;
        bool needCatalog = full && catalogDue;
        if (full) readCollectorStatus();
        for (const auto& spec : specs()) {
            const auto cached = series.constFind(spec.identity);
            if (!full && cached != series.cend() && cached->available && cached->reason.isEmpty()) continue;
            if (spec.venue == CompareVenue::BinanceUsdM) {
                if (catalogDue) needCatalog = true;
                else if (!full && !MarketCompare::perpAllowed(spec, perps)) continue;
            }
            queue.append({Job::Klines, spec, interval, 0, 0});
        }
        if (needCatalog) queue.prepend({Job::Catalog, {}, 0, 0, 0});
        if (wonMode == CompareWonMode::WonToDollar) {
            if (full || (collectorEcb.isEmpty() && (ecb.isEmpty() || !fxReason.isEmpty()))) readCollector();
            if (collectorEcb.isEmpty() && (ecb.isEmpty() || at - ecbAt >= EcbMaxAgeMs || !fxReason.isEmpty()))
                queue.append({Job::Ecb, {}, 0, 0, 0});
        }
        if (!wasLoading && !queue.isEmpty()) emit q->changed();
        pump();
    }

    void pump() {
        if (!active || !transport || running || queue.isEmpty()) return;
        const auto at = now();
        if (at < lastSendAt + PaceMs) { pacing.start(int(lastSendAt + PaceMs - at)); return; }
        auto job = queue.takeFirst();
        RequestSpec request;
        if (job.job == Job::Catalog) request = MarketCompare::perpCatalogRequest();
        else if (job.job == Job::Ecb) request = FxHistory::ecbHistoryRequest();
        else {
            if (!verifyContract(job.spec, job.intervalMs)) {
                emit q->changed();
                pump();
                return;
            }
            request = MarketCompare::klinesRequest(job.spec, job.intervalMs);
        }
        if (request.url.isEmpty()) { pump(); return; }
        job.token = nextToken++;
        job.startedAtMs = at;
        lastSendAt = at;
        running = job;
        transport->send(job.token, request);
    }

    void finished(quint64 token, const HttpResult& result) {
        if (!running || running->token != token) return;
        const auto job = *running;
        running.reset();
        const auto at = now();
        if (job.job == Job::Catalog) {
            QString reason;
            const auto parsed = MarketCompare::parsePerpCatalog(result, &reason);
            // An expired allowlist cannot authorize requests after its refresh failed.
            perps = parsed;
            catalogAt = parsed.isEmpty() ? 0 : at;
            catalogReason = reason;
            for (const auto& spec : specs()) {
                const auto cached = series.constFind(spec.identity);
                if (cached != series.cend() && cached->available) verifyContract(spec, interval);
            }
        } else if (job.job == Job::Ecb) {
            QString reason;
            const auto parsed = result.status == 200 && !result.networkError && !result.timedOut
                                    ? FxHistory::parseEcb(result.body, at, &reason)
                                    : QList<FxRate>{};
            if (!parsed.isEmpty()) { ecb = parsed; ecbAt = at; fxReason.clear(); }
            else fxReason = reason.isEmpty() ? QStringLiteral("ECB 환율을 받지 못했어요") : reason;
        } else if (job.intervalMs == interval) {
            auto parsed = MarketCompare::parseKlines(job.spec, job.intervalMs, result, at);
            auto& entry = series[identityOf(job.spec)];
            if (parsed.available) {
                parsed.lastOkAtMs = at;
                entry = parsed;
            } else if (entry.available && entry.intervalMs == job.intervalMs) {
                entry.reason = QStringLiteral("갱신 실패: ") + parsed.reason; // The last good bars stay, marked as such.
            } else {
                entry = parsed;
            }
        }
        emit q->changed();
        pump();
    }
};

MarketCompareSource::MarketCompareSource(TradingTransport* transport, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->q = this;
    d_->transport = transport;
    d_->clock = std::move(clock);
    d_->pacing.setSingleShot(true);
    d_->refreshTimer.setSingleShot(true);
    connect(&d_->pacing, &QTimer::timeout, this, [this] { d_->pump(); });
    connect(&d_->refreshTimer, &QTimer::timeout, this, [this] { refresh(); });
    if (transport) connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) { d_->finished(token, result); });
}

MarketCompareSource::~MarketCompareSource() = default;

qint64 MarketCompareSource::refreshPeriodMs(qint64 intervalMs) { return intervalMs <= 15 * Minute ? Minute : 5 * Minute; }

void MarketCompareSource::setActive(bool active) {
    if (d_->active == active) return;
    d_->active = active;
    if (!active) {
        d_->refreshTimer.stop();
        d_->pacing.stop();
        return;
    }
    // Resume the current batch until its original refresh deadline. An in-flight
    // reply may have updated the cache while the tab was hidden.
    const auto remaining = d_->nextRefreshAt - d_->now();
    if (remaining > 0 && remaining <= refreshPeriodMs(d_->interval)) {
        d_->refreshTimer.start(int(remaining));
        if (!d_->running && d_->queue.isEmpty()) d_->schedule(Private::Batch::RetryFailures);
        else d_->pump();
    } else {
        refresh();
    }
}

bool MarketCompareSource::active() const { return d_->active; }

void MarketCompareSource::setInterval(qint64 intervalMs) {
    if (!MarketCompare::supportedInterval(intervalMs) || d_->interval == intervalMs) return;
    d_->interval = intervalMs;
    d_->series.clear();
    emit changed();
    refresh();
}

qint64 MarketCompareSource::intervalMs() const { return d_->interval; }

void MarketCompareSource::setWonMode(CompareWonMode mode) {
    if (d_->wonMode == mode) return;
    d_->wonMode = mode;
    emit changed();
    refresh();
}

void MarketCompareSource::setCollectorRoot(const QString& root) {
    d_->collectorRoot = root;
    d_->collectorReadAt = 0;
}

void MarketCompareSource::refresh() {
    if (!d_->active) {
        d_->nextRefreshAt = 0;
        d_->queue.clear();
        return;
    }
    const auto period = refreshPeriodMs(d_->interval);
    d_->nextRefreshAt = d_->now() + period;
    d_->refreshTimer.start(int(period));
    d_->collectorReadAt = qMin(d_->collectorReadAt, d_->now() - CollectorReadEveryMs + Minute);
    d_->schedule(Private::Batch::Full);
}

MarketCompareSnapshot MarketCompareSource::snapshot() const {
    MarketCompareSnapshot out;
    out.intervalMs = d_->interval;
    out.updatedAtMs = d_->now();
    out.catalogReason = d_->catalogReason;
    out.collectorReason = d_->collectorReason;
    out.loading = d_->running.has_value() || (d_->active && !d_->queue.isEmpty());
    out.wonMode = d_->wonMode;
    out.hana = d_->hana;
    out.ecb = d_->collectorEcb.isEmpty() ? d_->ecb : d_->collectorEcb;
    out.fxReason = d_->collectorEcb.isEmpty() ? d_->fxReason : QString{};
    out.collectorWrittenAtMs = d_->collectorWrittenAt;
    out.hanaConfirmedThroughMs = d_->hanaConfirmedThrough;
    for (const auto& spec : d_->specs()) {
        auto entry = d_->series.value(spec.identity);
        if (entry.spec.identity.isEmpty()) {
            entry.spec = spec;
            entry.intervalMs = d_->interval;
            entry.reason = QStringLiteral("받는 중");
        }
        if (spec.venue == CompareVenue::UpbitKrw && d_->wonMode == CompareWonMode::WonToDollar) {
            CompareSeries converted = entry;
            converted.spec.name = QStringLiteral("비트코인(원화→달러)");
            converted.spec.currency = QStringLiteral("USD 환산");
            converted.bars.clear();
            converted.fx.clear();
            int missing = 0;
            for (const auto& bar : entry.bars) {
                const auto choice = FxHistory::choose(bar.closeMs, entry.intervalMs, out.ecb, out.hana);
                if (!choice.available || !(choice.krwPerUsd > 0)) { ++missing; continue; }
                auto next = bar;
                next.close = bar.close / choice.krwPerUsd;
                next.closeText = QString::number(next.close, 'f', 2);
                converted.bars.append(next);
                converted.fx.append(choice); // Which posting each bar used, kept for display.
                // The open hour is intentionally not read. Until its sealed segment is
                // visible, a newly closed intraday bar can still acquire another posting.
                const auto confirmedThrough = out.hanaConfirmedThroughMs > 0 ? out.hanaConfirmedThroughMs
                                                                            : (out.updatedAtMs - 5 * Minute) / Hour * Hour;
                if (entry.intervalMs < 24 * Hour && bar.closeMs + 1 > confirmedThrough)
                    out.provisionalFxOpenMs.insert(bar.openMs);
            }
            converted.available = !converted.bars.isEmpty();
            if (missing) converted.reason = (converted.reason.isEmpty() ? QString{} : converted.reason + QStringLiteral(" · ")) +
                                            QStringLiteral("환산 불가 %1봉").arg(missing);
            entry = converted;
        }
        out.series.append(entry);
    }
    return out;
}
} // namespace keefi
