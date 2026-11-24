#include "keefi/collector/CollectorRuntime.h"

#include "keefi/ChartIndicatorState.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/FxHistory.h"
#include "keefi/MarketCandles.h"
#include "keefi/NetworkPublicFlowSocket.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/TradingTransport.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStorageInfo>
#include <QTimeZone>
#include <QTimer>
#include <optional>
#include <sys/resource.h>

namespace keefi {
namespace {
constexpr qint64 Minute = 60000, Hour = 3600000, WarmUpMs = 5 * Minute, LedgerWindowMs = 3 * Hour;

QByteArray compact(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

QString venueName(CoinFlowVenue venue) {
    switch (venue) {
    case CoinFlowVenue::UpbitSpot: return QStringLiteral("upbit");
    case CoinFlowVenue::BinanceSpot: return QStringLiteral("binance-spot");
    case CoinFlowVenue::BinanceUsdM: return QStringLiteral("binance-usdm");
    }
    return QStringLiteral("unknown");
}

QString coverageName(CoinFlowCoverage coverage) {
    switch (coverage) {
    case CoinFlowCoverage::Unavailable: return QStringLiteral("unavailable");
    case CoinFlowCoverage::WarmingUp: return QStringLiteral("warming_up");
    case CoinFlowCoverage::ObservedContinuous: return QStringLiteral("observed");
    case CoinFlowCoverage::RecoveredOverlap: return QStringLiteral("recovered_overlap");
    case CoinFlowCoverage::Partial: return QStringLiteral("partial");
    case CoinFlowCoverage::Disconnected: return QStringLiteral("disconnected");
    case CoinFlowCoverage::Overflow: return QStringLiteral("overflow");
    }
    return QStringLiteral("unknown");
}

QString signalName(CoinFlowSignalKind kind) {
    switch (kind) {
    case CoinFlowSignalKind::LargeTrade: return QStringLiteral("LargeTrade");
    case CoinFlowSignalKind::TakerSkew: return QStringLiteral("TakerSkew");
    case CoinFlowSignalKind::OpenInterestJump: return QStringLiteral("OpenInterestJump");
    case CoinFlowSignalKind::VolumeBurst: return QStringLiteral("VolumeBurst");
    case CoinFlowSignalKind::Rebound: return QStringLiteral("Rebound");
    case CoinFlowSignalKind::TurnDown: return QStringLiteral("TurnDown");
    }
    return QStringLiteral("Unknown");
}

QString directionName(CoinFlowDirection direction) {
    switch (direction) {
    case CoinFlowDirection::Up: return QStringLiteral("up");
    case CoinFlowDirection::Down: return QStringLiteral("down");
    default: return QStringLiteral("neutral");
    }
}

QString indicatorDirectionName(IndicatorDirection direction) {
    switch (direction) {
    case IndicatorDirection::Up: return QStringLiteral("up");
    case IndicatorDirection::Down: return QStringLiteral("down");
    case IndicatorDirection::Mix: return QStringLiteral("mix");
    case IndicatorDirection::Info: return QStringLiteral("info");
    default: return QStringLiteral("none");
    }
}

QString gradeName(ExtendedReversalGrade grade) {
    switch (grade) {
    case ExtendedReversalGrade::Basic: return QStringLiteral("basic");
    case ExtendedReversalGrade::Reinforced: return QStringLiteral("reinforced");
    case ExtendedReversalGrade::Strong: return QStringLiteral("strong");
    case ExtendedReversalGrade::Incomplete: return QStringLiteral("incomplete");
    default: return QStringLiteral("none");
    }
}

QJsonObject rawTrade(const CoinTradeObservation& trade) {
    QJsonObject record{{"i", trade.id}, {"p", trade.price}, {"q", trade.quantity}, {"d", trade.side == CoinFlowSide::Buy ? "b" : "s"},
                       {"T", trade.tradeAtMs}, {"E", trade.eventAtMs}, {"r", trade.receivedAtMs}, {"k", trade.knownAtMs},
                       {"e", qint64(trade.epoch)}};
    if (trade.normalQuantity) record.insert("n", *trade.normalQuantity);
    if (trade.snapshot) record.insert("x", 1);
    if (!trade.liveEligible) record.insert("L", 0);
    return record;
}

QByteArray tradeContent(const CoinTradeObservation& trade) {
    return CollectorTradeLedger::content(trade.price, trade.quantity, trade.normalQuantity.value_or(QString{}),
                                         trade.side == CoinFlowSide::Buy ? QStringLiteral("b") : QStringLiteral("s"), trade.tradeAtMs);
}

QJsonObject evidence(const CoinFlowAvailability& state) {
    return collectorAvailabilityEvidence(state);
}

struct StreamStats {
    qint64 lastReceivedAtMs = 0, records = 0, duplicates = 0, conflicts = 0;
};
} // namespace

struct CollectorRuntime::Private {
    struct Pipeline {
        QString identity;
        bool upbit = false;
        std::unique_ptr<TradingTransport> flowTransport, minuteTransport;
        std::unique_ptr<CoinFlowSource> flow;
        std::unique_ptr<MarketCandles> minutes;
        bool minutesConfigured = false;
        qint64 lastStudyMinuteMs = 0;
        QSet<QString> seenLiquidations, seenPeriods, seenConflicts;
        QSet<qint64> seenMinutes;
        QHash<QString, qint64> sourceDuplicates;
        std::optional<ExtendedStrongReversal> previousStrong;
    };

    CollectorRuntimeOptions options;
    CollectorRuntimeDependencies dependencies;
    std::unique_ptr<CollectorStore> store;
    std::vector<std::unique_ptr<Pipeline>> pipelines;
    CollectorTradeLedger ledger;
    CollectorEventLedger eventLedger;
    std::unique_ptr<TradingTransport> hanaTransport, ecbTransport, clockTransport;
    QTimer extract, seal, status, hana, hourly, housekeeping;
    QHash<QString, StreamStats> streams;
    QHash<QString, qint64> pendingDuplicates; // venue/symbol -> not yet written count
    QHash<QString, qint64> pendingDuplicateFrom; // Failed streams keep their original uncommitted interval.
    QSet<QString> ecbDates;
    qint64 startedAtMs = 0, lastHanaPostedAtMs = 0, lastEcbFetchAtMs = 0, clockSentAtMs = 0, clockOffsetMs = 0, lastDuplicateFlushAtMs = 0;
    int preloadedTrades = 0;
    QString lastHanaPrice, hanaReason, ecbReason, clockReason;
    quint64 hanaToken = 0, ecbToken = 0, clockToken = 0, nextToken = 1;
    bool running = false, ecbFailed = false;

    qint64 now() const { return dependencies.clock ? dependencies.clock() : QDateTime::currentMSecsSinceEpoch(); }

    std::unique_ptr<TradingTransport> makeTransport(CollectorRuntimeDependencies::TransportRole role) {
        return dependencies.transport ? dependencies.transport(role) : std::make_unique<NetworkTradingTransport>();
    }
    std::unique_ptr<PublicCandleSocket> makeSocket() {
        return dependencies.socket ? dependencies.socket() : makeNetworkPublicFlowSocket();
    }

    bool write(const QString& kind, const QString& venue, const QString& symbol, qint64 bucket, qint64 received, const QJsonObject& record) {
        if (kind == QLatin1String("event") && eventLedger.contains(venue, symbol, record)) return true;
        if (!store->append(kind, venue, symbol, bucket, received, compact(record))) return false;
        if (kind == QLatin1String("event")) eventLedger.remember(venue, symbol, record);
        auto& stream = streams[kind + '/' + venue + '/' + symbol];
        stream.lastReceivedAtMs = qMax(stream.lastReceivedAtMs, received);
        ++stream.records;
        return true;
    }

    // Segments an event was decided from: trades and 1m klines of the primary
    // venue, trades of the USD-M companion, for the hours the study looked at.
    QJsonArray inputs(const CoinFlowSnapshot& snapshot, qint64 fromMs, qint64 throughMs) const {
        QJsonArray out;
        const auto first = qMax<qint64>(0, fromMs - Hour) / Hour * Hour;
        const auto last = qMax(throughMs, fromMs) / Hour * Hour;
        for (qint64 hour = first; hour <= last; hour += Hour) {
            const auto venue = venueName(snapshot.primary.instrument.venue);
            out.append(CollectorStore::relativeBase(QStringLiteral("trade"), venue, snapshot.primary.instrument.symbol, hour));
            out.append(CollectorStore::relativeBase(QStringLiteral("kline"), venue, snapshot.primary.instrument.symbol, hour));
            if (snapshot.companion)
                out.append(CollectorStore::relativeBase(QStringLiteral("trade"), venueName(snapshot.companion->instrument.venue),
                                                        snapshot.companion->instrument.symbol, hour));
        }
        return out;
    }

    void addPipeline(const QString& identity, bool upbit) {
        auto pipeline = std::make_unique<Pipeline>();
        pipeline->identity = identity;
        pipeline->upbit = upbit;
        pipeline->flowTransport = makeTransport(CollectorRuntimeDependencies::TransportRole::Flow);
        pipeline->minuteTransport = makeTransport(CollectorRuntimeDependencies::TransportRole::Minutes);
        pipeline->flow = std::make_unique<CoinFlowSource>(pipeline->flowTransport.get(), makeSocket(), makeSocket(), nullptr, dependencies.clock);
        pipeline->minutes = std::make_unique<MarketCandles>(pipeline->minuteTransport.get(), std::unique_ptr<PublicCandleSocket>{}, dependencies.clock);
        if (upbit) {
            QStringList markets;
            for (const auto& coin : options.upbitCoins) markets.append(QStringLiteral("KRW-") + coin);
            pipeline->minutes->setVerifiedUpbitMarkets(markets, true);
        }
        auto* flow = pipeline->flow.get();
        auto* minutes = pipeline->minutes.get();
        QObject::connect(minutes, &MarketCandles::changed, flow, [flow, minutes] { flow->setMinuteSeries(minutes->series()); });
        flow->setSelection(identity);
        flow->setActive(true);
        pipelines.push_back(std::move(pipeline));
    }

    void writeWindow(Pipeline& pipeline, const CoinFlowWindow& window, qint64 at) {
        if (!window.instrument.verified || window.instrument.symbol.isEmpty()) return;
        const auto venue = venueName(window.instrument.venue);
        const auto symbol = window.instrument.symbol;
        const auto streamKey = venue + '/' + symbol;
        for (const auto& trade : window.trades) {
            QByteArray first;
            const auto content = tradeContent(trade);
            bool accepted = true;
            switch (ledger.check(venue, symbol, trade.id, content, &first, trade.receivedAtMs, trade.epoch)) {
            case CollectorTradeLedger::Outcome::New: {
                auto record = rawTrade(trade);
                if (trade.receivedAtMs <= 0) record.insert("r", at);
                accepted = write(QStringLiteral("trade"), venue, symbol, trade.tradeAtMs, record.value("r").toInteger(), record);
                break;
            }
            case CollectorTradeLedger::Outcome::Duplicate:
                ++pendingDuplicates[streamKey];
                ++streams[QStringLiteral("trade/") + streamKey].duplicates;
                break;
            case CollectorTradeLedger::Outcome::Conflict: {
                // The first observation stays; the different payload is kept beside it.
                auto record = rawTrade(trade);
                record.insert("first", QString::fromUtf8(first));
                record.insert("source", QStringLiteral("ledger"));
                if (trade.receivedAtMs <= 0) record.insert("r", at);
                accepted = write(QStringLiteral("conflict"), venue, symbol, trade.tradeAtMs, record.value("r").toInteger(), record);
                if (accepted) ++streams[QStringLiteral("trade/") + streamKey].conflicts;
                break;
            }
            case CollectorTradeLedger::Outcome::Reinspection:
                break;
            }
            if (accepted) ledger.observe(venue, symbol, trade.id, content, trade.tradeAtMs, nullptr, trade.receivedAtMs, trade.epoch, at);
        }
        // CoinFlowSource suppresses identical incoming frames before exposing
        // the window. Count only its new delivery counter, never polling ticks.
        const auto previousDuplicates = pipeline.sourceDuplicates.value(streamKey);
        const auto sourceDuplicates = qint64(window.duplicateCount);
        const auto newDuplicates = sourceDuplicates >= previousDuplicates ? sourceDuplicates - previousDuplicates : sourceDuplicates;
        pipeline.sourceDuplicates.insert(streamKey, sourceDuplicates);
        pendingDuplicates[streamKey] += newDuplicates;
        streams[QStringLiteral("trade/") + streamKey].duplicates += newDuplicates;
        for (const auto& conflict : window.conflicts) {
            const auto key = venue + ':' + conflict.second.id + ':' + QString::fromUtf8(tradeContent(conflict.second));
            if (pipeline.seenConflicts.contains(key)) continue;
            QJsonObject record{{"i", conflict.second.id}, {"first", rawTrade(conflict.first)}, {"second", rawTrade(conflict.second)},
                               {"source", QStringLiteral("coinflow")}, {"r", at}};
            if (!write(QStringLiteral("conflict"), venue, symbol, conflict.second.tradeAtMs > 0 ? conflict.second.tradeAtMs : at, at, record)) continue;
            pipeline.seenConflicts.insert(key);
            ++streams[QStringLiteral("trade/") + streamKey].conflicts;
        }
        for (const auto& period : window.periods) {
            const auto key = QStringLiteral("%1:%2:%3:%4:%5").arg(venue).arg(period.fromMs).arg(period.throughMs).arg(int(period.coverage)).arg(period.epoch);
            if (pipeline.seenPeriods.contains(key) || period.fromMs <= 0) continue;
            QJsonObject record{{"from", period.fromMs}, {"to", period.throughMs}, {"k", period.knownAtMs}, {"c", coverageName(period.coverage)},
                               {"why", period.reason}, {"e", qint64(period.epoch)}, {"r", period.knownAtMs > 0 ? period.knownAtMs : at}};
            if (write(QStringLiteral("coverage"), venue, symbol, period.fromMs, record.value("r").toInteger(), record))
                pipeline.seenPeriods.insert(key);
        }
    }

    bool fresh(qint64 knownAtMs) const { return knownAtMs >= startedAtMs + WarmUpMs; }

    void recordEvents(Pipeline& pipeline, const CoinFlowSnapshot& snapshot, qint64 at) {
        // Same calculation path and version as the app's strong-reversal runner;
        // decided now with what is known now.
        const auto studies = coinFlowStudiesWithReversalEvidence(snapshot, at);
        const auto strong = extendedStrongReversal(studies, pipeline.previousStrong ? &*pipeline.previousStrong : nullptr);
        pipeline.previousStrong = strong;
        const auto venue = venueName(snapshot.primary.instrument.venue);
        const auto symbol = snapshot.primary.instrument.symbol;
        for (const auto& entry : studies.entries) {
            if (!fresh(entry.knownAtMs)) continue;
            QJsonObject record{{"kind", QStringLiteral("Signal")}, {"signal", signalName(entry.kind)}, {"dir", directionName(entry.direction)},
                               {"at", entry.atMs}, {"k", entry.knownAtMs}, {"entry", entry.entryClose}, {"evidence", evidence(entry.state)},
                               {"inputs", inputs(snapshot, entry.state.fromMs > 0 ? entry.state.fromMs : entry.atMs, entry.atMs)},
                               {"decidedAt", at}, {"r", at}, {"algorithm", options.revision}};
            if (entry.value) record.insert("value", *entry.value);
            write(QStringLiteral("event"), venue, symbol, entry.atMs, at, record);
        }
        for (const auto& group : studies.groups) {
            if (!fresh(group.knownAtMs)) continue;
            write(QStringLiteral("event"), venue, symbol, group.fromMs, at,
                  QJsonObject{{"kind", QStringLiteral("Group")}, {"signal", signalName(group.kind)}, {"dir", directionName(group.direction)},
                              {"from", group.fromMs}, {"through", group.throughMs}, {"k", group.knownAtMs}, {"entry", group.entryClose},
                              {"m", group.minutes}, {"ended", group.ended}, {"inputs", inputs(snapshot, group.fromMs, group.throughMs)},
                              {"decidedAt", at}, {"r", at}, {"algorithm", options.revision}});
        }
        for (const auto& candidate : studies.largeTrade.candidates) {
            if (!fresh(candidate.knownAtMs)) continue;
            write(QStringLiteral("event"), venue, symbol, candidate.tradeAtMs, at,
                  QJsonObject{{"kind", QStringLiteral("LargeTradePoint")}, {"i", candidate.id}, {"q", candidate.quoteNotional},
                              {"pct", candidate.percentile}, {"b", candidate.baselineSamples}, {"d", candidate.side == CoinFlowSide::Buy ? "b" : "s"},
                              {"T", candidate.tradeAtMs}, {"k", candidate.knownAtMs}, {"evidence", evidence(studies.largeTrade.state)},
                              {"inputs", inputs(snapshot, candidate.tradeAtMs, candidate.tradeAtMs)},
                              {"decidedAt", at}, {"r", at}, {"algorithm", options.revision}});
        }
        for (const auto& event : strong.events) {
            if (!fresh(event.decisionAtMs)) continue;
            write(QStringLiteral("event"), venue, symbol, event.signalAtMs, at,
                      QJsonObject{{"kind", QStringLiteral("StrongReversal")}, {"signal", signalName(event.kind)},
                                  {"dir", indicatorDirectionName(event.direction)}, {"grade", gradeName(event.grade)},
                                  {"signalAt", event.signalAtMs}, {"k", event.decisionAtMs},
                                  {"evidence", collectorStrongReversalEvidence(studies, event.kind, event.signalAtMs, event.decisionAtMs)},
                                  {"inputs", inputs(snapshot, event.signalAtMs - 5 * Minute, event.signalAtMs)},
                                  {"decidedAt", at}, {"r", at}, {"algorithm", options.revision}});
            if (event.outcome)
                write(QStringLiteral("event"), venue, symbol, event.signalAtMs, at,
                      QJsonObject{{"kind", QStringLiteral("StrongReversalOutcome")}, {"signal", signalName(event.kind)},
                                  {"signalAt", event.signalAtMs}, {"evaluationAt", event.outcome->evaluationAtMs},
                                  {"k", event.outcome->knownAtMs}, {"returnPercent", event.outcome->returnPercent},
                                  {"decidedAt", at}, {"r", at}, {"algorithm", options.revision}});
        }
    }

    void collect(Pipeline& pipeline) {
        const auto at = now();
        const auto snapshot = pipeline.flow->snapshot(at);
        if (!pipeline.minutesConfigured && snapshot.primary.instrument.verified && snapshot.primary.instrument.identity == pipeline.identity) {
            if (!pipeline.upbit) {
                MarketInstrument item;
                item.symbol = snapshot.primary.instrument.symbol;
                item.baseAsset = snapshot.primary.instrument.baseAsset;
                item.quoteAsset = snapshot.primary.instrument.quoteAsset;
                item.status = snapshot.primary.instrument.status;
                item.market = QStringLiteral("spot");
                item.tradable = true;
                pipeline.minutes->setInstruments(QStringLiteral("binance"), {item});
            }
            pipeline.minutes->setSelection(pipeline.identity, Minute);
            pipeline.minutesConfigured = true;
        }
        writeWindow(pipeline, snapshot.primary, at);
        if (snapshot.companion) writeWindow(pipeline, *snapshot.companion, at);
        for (const auto& liquidation : snapshot.liquidations) {
            const auto key = QStringLiteral("%1:%2:%3:%4:%5").arg(liquidation.symbol).arg(liquidation.eventAtMs).arg(liquidation.side, liquidation.price, liquidation.lastFilledQuantity);
            if (pipeline.seenLiquidations.contains(key)) continue;
            QJsonObject record{{"S", liquidation.side}, {"p", liquidation.price}, {"a", liquidation.averagePrice},
                               {"q", liquidation.originalQuantity}, {"l", liquidation.lastFilledQuantity}, {"z", liquidation.cumulativeFilledQuantity},
                               {"o", liquidation.orderStatus}, {"T", liquidation.tradeAtMs}, {"E", liquidation.eventAtMs},
                               {"r", liquidation.receivedAtMs > 0 ? liquidation.receivedAtMs : at}, {"k", liquidation.knownAtMs},
                               {"snapshotPerSymbolPerSecond", true}};
            if (write(QStringLiteral("liquidation"), QStringLiteral("binance-usdm"), liquidation.symbol, liquidation.tradeAtMs,
                      record.value("r").toInteger(), record))
                pipeline.seenLiquidations.insert(key);
        }
        qint64 newestMinute = 0;
        for (const auto& minute : snapshot.minutes) {
            newestMinute = qMax(newestMinute, minute.completesAtMs);
            if (pipeline.seenMinutes.contains(minute.candle.startsAtMs) || !snapshot.primary.instrument.verified) continue;
            const auto& candle = minute.candle;
            QJsonObject record{{"t", candle.startsAtMs}, {"o", candle.open}, {"h", candle.high}, {"l", candle.low}, {"c", candle.close},
                               {"v", candle.volume}, {"T", minute.completesAtMs}, {"k", minute.knownAtMs}, {"D", minute.decisionAtMs},
                               {"r", minute.knownAtMs > 0 ? minute.knownAtMs : at}};
            if (write(QStringLiteral("kline"), venueName(snapshot.primary.instrument.venue), snapshot.primary.instrument.symbol, candle.startsAtMs,
                      record.value("r").toInteger(), record))
                pipeline.seenMinutes.insert(candle.startsAtMs);
        }
        if (newestMinute <= pipeline.lastStudyMinuteMs || !snapshot.primary.instrument.verified) return;
        pipeline.lastStudyMinuteMs = newestMinute;
        // Warm-up: the first minutes run on REST seed data, which may fill baselines but never creates events.
        if (at < startedAtMs + WarmUpMs) return;
        recordEvents(pipeline, snapshot, at);
    }

    void flushDuplicates(qint64 at) {
        for (auto it = pendingDuplicates.begin(); it != pendingDuplicates.end(); ++it) {
            if (it.value() <= 0) continue;
            const auto parts = it.key().split('/');
            const auto from = pendingDuplicateFrom.value(it.key(), lastDuplicateFlushAtMs);
            if (write(QStringLiteral("dup"), parts.value(0), parts.value(1), at, at,
                      QJsonObject{{"n", it.value()}, {"from", from}, {"to", at}, {"r", at}})) {
                it.value() = 0;
                pendingDuplicateFrom.remove(it.key());
            } else {
                pendingDuplicateFrom.insert(it.key(), from);
            }
        }
        lastDuplicateFlushAtMs = at;
    }

    void requestHana() {
        hanaToken = nextToken++;
        hanaTransport->send(hanaToken, TodayBtcPremium::request(TodayBtcPremium::Read::Fx));
    }

    void requestEcb() {
        ecbToken = nextToken++;
        lastEcbFetchAtMs = now();
        ecbTransport->send(ecbToken, FxHistory::ecbHistoryRequest());
    }

    void requestClock() {
        RequestSpec request;
        request.operation = Operation::Time;
        request.url = QUrl(QStringLiteral("https://api.binance.com/api/v3/time"));
        clockToken = nextToken++;
        clockSentAtMs = now();
        clockTransport->send(clockToken, request);
    }

    bool ecbDue() const {
        const auto at = now();
        const auto retry = ecbFailed ? 10 * Minute : Hour; // A failed or cut-off read is retried sooner.
        // A batch can contain today's successful row and an older failed append.
        // Publication-date dedup must not cancel that batch's pending retry.
        if (ecbFailed) return at - lastEcbFetchAtMs > retry;
        if (ecbDates.isEmpty()) return at - lastEcbFetchAtMs > retry;
        const auto berlin = QDateTime::fromMSecsSinceEpoch(at, QTimeZone("Europe/Berlin"));
        const auto today = berlin.date();
        const bool weekday = today.dayOfWeek() <= 5;
        return weekday && berlin.time() >= QTime(17, 0) && !ecbDates.contains(today.toString(Qt::ISODate)) && at - lastEcbFetchAtMs > retry;
    }

    void preload() {
        // Trades written before a restart: a REST seed that re-sends them is counted, never written twice.
        const auto since = (now() - LedgerWindowMs) / Hour * Hour;
        for (const auto& record : store->recentRecords(QStringLiteral("trade"), since)) {
            ledger.remember(record.venue, record.symbol, record.object.value("i").toString(), CollectorTradeLedger::content(record.object),
                            record.object.value("T").toInteger());
            ++preloadedTrades;
        }
        for (const auto& record : store->recentRecords(QStringLiteral("event"), since))
            eventLedger.remember(record.venue, record.symbol, record.object);
        for (const auto& record : store->recentRecords(QStringLiteral("fx"), 0))
            if (record.venue == QLatin1String("ecb")) ecbDates.insert(record.object.value("d").toString());
    }

    QJsonObject statusObject() const {
        QJsonObject streamObject;
        for (auto it = streams.cbegin(); it != streams.cend(); ++it)
            streamObject.insert(it.key(), QJsonObject{{"lastReceivedAt", it.value().lastReceivedAtMs}, {"records", it.value().records},
                                                      {"duplicates", it.value().duplicates}, {"conflicts", it.value().conflicts}});
        QJsonObject flows;
        for (const auto& pipeline : pipelines) flows.insert(pipeline->identity, pipeline->flow->status());
        QJsonObject kinds;
        const auto stats = store->stats();
        for (auto it = stats.cbegin(); it != stats.cend(); ++it)
            kinds.insert(it.key(), QJsonObject{{"records", it.value().records}, {"rawBytes", it.value().rawBytes},
                                               {"sealedBytes", it.value().sealedBytes}, {"sealedRawBytes", it.value().sealedRawBytes},
                                               {"sealedSegments", it.value().sealedSegments}});
        return QJsonObject{{"schema", 1},
                           {"revision", options.revision},
                           {"startedAt", startedAtMs},
                           {"writtenAt", now()},
                           {"diskFreeBytes", QStorageInfo(options.root).bytesAvailable()},
                           {"clockOffsetMs", clockOffsetMs},
                           {"clockReason", clockReason},
                           {"hanaReason", hanaReason},
                           {"ecbReason", ecbReason},
                           {"preloadedTrades", preloadedTrades},
                           {"ledgerSize", ledger.size()},
                           {"recovery", QJsonArray::fromStringList(store->recoveryNotes())},
                           {"streams", streamObject},
                           {"flows", flows},
                           {"kinds", kinds}};
    }
};

CollectorRuntime::CollectorRuntime(CollectorRuntimeOptions options, QObject* parent)
    : CollectorRuntime(std::move(options), CollectorRuntimeDependencies{}, parent) {}

CollectorRuntime::CollectorRuntime(CollectorRuntimeOptions options, CollectorRuntimeDependencies dependencies, QObject* parent)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->options = std::move(options);
    d_->dependencies = std::move(dependencies);
    d_->hanaTransport = d_->makeTransport(CollectorRuntimeDependencies::TransportRole::Hana);
    d_->ecbTransport = d_->makeTransport(CollectorRuntimeDependencies::TransportRole::Ecb);
    d_->clockTransport = d_->makeTransport(CollectorRuntimeDependencies::TransportRole::Clock);
}

CollectorRuntime::~CollectorRuntime() { stop(); }

bool CollectorRuntime::start(QString* error) {
    if (d_->running) return true;
    CollectorStoreOptions storeOptions;
    storeOptions.root = d_->options.root;
    storeOptions.collectorVersion = d_->options.revision;
    storeOptions.algorithmVersion = d_->options.revision;
    storeOptions.diskFloorBytes = d_->options.diskFloorBytes;
    d_->store = std::make_unique<CollectorStore>(storeOptions, d_->dependencies.clock);
    if (!d_->store->open(error)) return false;
    d_->startedAtMs = d_->now();
    d_->lastDuplicateFlushAtMs = d_->startedAtMs;
    CollectorReader previous(d_->options.root);
    const auto lastStatus = previous.status();
    const auto lastWrittenAt = lastStatus.value("writtenAt").toInteger();
    d_->write(QStringLiteral("collector"), QStringLiteral("collector"), QStringLiteral("lifecycle"), d_->startedAtMs, d_->startedAtMs,
              QJsonObject{{"what", QStringLiteral("start")}, {"previousStatusAt", lastWrittenAt}, {"revision", d_->options.revision},
                          {"recovery", QJsonArray::fromStringList(d_->store->recoveryNotes())}, {"r", d_->startedAtMs}});
    if (lastWrittenAt > 0 && lastWrittenAt < d_->startedAtMs)
        d_->write(QStringLiteral("gap"), QStringLiteral("collector"), QStringLiteral("all"), lastWrittenAt, d_->startedAtMs,
                  QJsonObject{{"from", lastWrittenAt}, {"to", d_->startedAtMs}, {"why", QStringLiteral("중지")}, {"r", d_->startedAtMs}});
    d_->preload();
    for (const auto& coin : d_->options.binanceCoins) d_->addPipeline(QStringLiteral("coin:binance:") + coin + QStringLiteral("USDT"), false);
    for (const auto& coin : d_->options.upbitCoins) d_->addPipeline(QStringLiteral("coin:upbit:KRW-") + coin, true);

    connect(d_->hanaTransport.get(), &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) {
        if (token != d_->hanaToken) return;
        const auto at = d_->now();
        UsdKrwPosting posting;
        QString reason;
        if (!TodayBtcPremium::parseFxPosting(result.body, at, &posting, &reason)) { d_->hanaReason = reason; return; }
        d_->hanaReason.clear();
        if (posting.postedAtMs == d_->lastHanaPostedAtMs && posting.krwPerUsd == d_->lastHanaPrice) return;
        if (d_->write(QStringLiteral("fx"), QStringLiteral("naver-hana"), QStringLiteral("USDKRW"), at, at,
                      QJsonObject{{"x", posting.krwPerUsd}, {"p", posting.postedAtMs}, {"d", posting.postedDate.toString(Qt::ISODate)}, {"r", at}})) {
            d_->lastHanaPostedAtMs = posting.postedAtMs;
            d_->lastHanaPrice = posting.krwPerUsd;
        } else {
            d_->hanaReason = QStringLiteral("하나은행 환율 저장 실패 · 다음 조회에서 다시 시도");
        }
    });
    connect(d_->ecbTransport.get(), &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) {
        if (token != d_->ecbToken) return;
        const auto at = d_->now();
        QString reason;
        if (result.status != 200 || result.timedOut || result.networkError) {
            d_->ecbFailed = true;
            d_->ecbReason = result.timedOut ? QStringLiteral("ECB 응답 시간 초과 · 10분 뒤 다시 받음") : QStringLiteral("ECB 응답 실패 · 10분 뒤 다시 받음");
            return;
        }
        const auto rates = FxHistory::parseEcb(result.body, at, &reason);
        if (rates.isEmpty()) { d_->ecbFailed = true; d_->ecbReason = reason.isEmpty() ? QStringLiteral("ECB 응답 없음") : reason; return; }
        d_->ecbFailed = false;
        d_->ecbReason.clear();
        for (const auto& rate : rates) {
            const auto date = rate.date.toString(Qt::ISODate);
            if (d_->ecbDates.contains(date)) continue;
            if (d_->write(QStringLiteral("fx"), QStringLiteral("ecb"), QStringLiteral("USDKRW"), at, at,
                          QJsonObject{{"d", date}, {"x", rate.krwPerUsd}, {"p", rate.postedAtMs}, {"k", rate.knownAtMs}, {"r", at}})) {
                d_->ecbDates.insert(date);
            } else {
                d_->ecbFailed = true;
                d_->ecbReason = QStringLiteral("ECB 환율 저장 실패 · 10분 뒤 다시 받음");
            }
        }
    });
    connect(d_->clockTransport.get(), &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) {
        if (token != d_->clockToken) return;
        const auto at = d_->now();
        const auto server = parseExactJson(result.body).object().value("serverTime").toString().toLongLong();
        if (result.status != 200 || server <= 0) { d_->clockReason = QStringLiteral("서버 시각을 받지 못했어요"); return; }
        const auto local = (d_->clockSentAtMs + at) / 2;
        d_->clockOffsetMs = server - local;
        d_->clockReason = std::llabs(d_->clockOffsetMs) > 2000 ? QStringLiteral("시계 경고: 2초 넘게 차이") : QString{};
        d_->write(QStringLiteral("clock"), QStringLiteral("binance-spot"), QStringLiteral("time"), at, at,
                  QJsonObject{{"s", server}, {"l", local}, {"o", d_->clockOffsetMs}, {"rtt", at - d_->clockSentAtMs},
                              {"warn", std::llabs(d_->clockOffsetMs) > 2000}, {"r", at}});
    });

    connect(&d_->extract, &QTimer::timeout, this, [this] { for (auto& pipeline : d_->pipelines) d_->collect(*pipeline); });
    connect(&d_->seal, &QTimer::timeout, this, [this] { d_->store->flush(); d_->store->sealDue(); });
    connect(&d_->status, &QTimer::timeout, this, [this] { d_->store->writeStatus(d_->statusObject()); });
    connect(&d_->hana, &QTimer::timeout, this, [this] {
        d_->requestHana();
        if (d_->ecbDue()) d_->requestEcb();
        d_->flushDuplicates(d_->now());
    });
    connect(&d_->hourly, &QTimer::timeout, this, [this] { d_->requestClock(); });
    connect(&d_->housekeeping, &QTimer::timeout, this, [this] { d_->ledger.prune(d_->now() - LedgerWindowMs); });
    d_->extract.start(2000);
    d_->seal.start(30000);
    d_->status.start(10000);
    d_->hana.start(60000);
    d_->hourly.start(int(Hour));
    d_->housekeeping.start(int(10 * Minute));
    d_->requestHana();
    d_->requestClock();
    if (d_->ecbDue()) d_->requestEcb();
    d_->running = true;
    return true;
}

void CollectorRuntime::stop() {
    if (!d_->running) return;
    d_->running = false;
    for (auto* timer : {&d_->extract, &d_->seal, &d_->status, &d_->hana, &d_->hourly, &d_->housekeeping}) timer->stop();
    for (auto& pipeline : d_->pipelines) { d_->collect(*pipeline); pipeline->flow->stop(); }
    const auto at = d_->now();
    d_->flushDuplicates(at);
    d_->write(QStringLiteral("collector"), QStringLiteral("collector"), QStringLiteral("lifecycle"), at, at,
              QJsonObject{{"what", QStringLiteral("stop")}, {"r", at}});
    d_->store->sealDue();
    d_->store->writeStatus(d_->statusObject());
    d_->store->close();
    d_->pipelines.clear();
}

QJsonObject CollectorRuntime::summary() const {
    auto out = d_->store ? d_->statusObject() : QJsonObject{};
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    out.insert("cpuUserSeconds", double(usage.ru_utime.tv_sec) + usage.ru_utime.tv_usec / 1e6);
    out.insert("cpuSystemSeconds", double(usage.ru_stime.tv_sec) + usage.ru_stime.tv_usec / 1e6);
    out.insert("maxResidentBytes", qint64(usage.ru_maxrss)); // macOS reports bytes.
    out.insert("elapsedSeconds", double(d_->now() - d_->startedAtMs) / 1000.0);
    return out;
}
} // namespace keefi
