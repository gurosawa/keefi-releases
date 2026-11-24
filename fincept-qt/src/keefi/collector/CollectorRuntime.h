#pragma once
#include "keefi/CollectorStore.h"
#include "keefi/CoinFlowStudies.h"
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <memory>
#include <functional>

namespace keefi {
class TradingTransport;
class PublicCandleSocket;
inline QJsonObject collectorAvailabilityEvidence(const CoinFlowAvailability& state) {
    QString coverage;
    switch (state.coverage) {
    case CoinFlowCoverage::Unavailable: coverage = QStringLiteral("unavailable"); break;
    case CoinFlowCoverage::WarmingUp: coverage = QStringLiteral("warming_up"); break;
    case CoinFlowCoverage::ObservedContinuous: coverage = QStringLiteral("observed"); break;
    case CoinFlowCoverage::RecoveredOverlap: coverage = QStringLiteral("recovered_overlap"); break;
    case CoinFlowCoverage::Partial: coverage = QStringLiteral("partial"); break;
    case CoinFlowCoverage::Disconnected: coverage = QStringLiteral("disconnected"); break;
    case CoinFlowCoverage::Overflow: coverage = QStringLiteral("overflow"); break;
    }
    return {{"coverage", coverage}, {"from", state.fromMs}, {"through", state.throughMs},
            {"k", state.knownAtMs}, {"samples", state.samples}, {"why", state.reason}};
}

inline QJsonObject collectorStrongReversalEvidence(const CoinFlowStudySet& studies, CoinFlowSignalKind kind,
                                                   qint64 signalAtMs, qint64 decisionAtMs) {
    for (const auto& entry : studies.entries) {
        if (entry.kind != kind || entry.atMs != signalAtMs || entry.knownAtMs != decisionAtMs) continue;
        auto result = collectorAvailabilityEvidence(entry.state);
        result.insert("matched", true);
        result.insert("corroborationAvailable", entry.reversalCorroboration.has_value());
        if (entry.reversalCorroboration) {
            const auto& original = *entry.reversalCorroboration;
            auto volume = collectorAvailabilityEvidence(original.volume.state);
            volume.insert("available", original.volume.state.available);
            volume.insert("currentVolume", original.volume.currentVolume);
            volume.insert("previousMedian", original.volume.previousMedian);
            if (original.volume.multiple) volume.insert("multiple", *original.volume.multiple);
            auto spot = collectorAvailabilityEvidence(original.spot.state);
            spot.insert("available", original.spot.state.available);
            spot.insert("buyQuoteNotional", original.spot.buyQuoteNotional);
            spot.insert("sellQuoteNotional", original.spot.sellQuoteNotional);
            spot.insert("sourceDirectionOnly", original.spot.sourceDirectionOnly);
            if (original.spot.signedFraction) spot.insert("signedFraction", *original.spot.signedFraction);
            result.insert("volume", volume);
            result.insert("spot", spot);
        }
        return result;
    }
    // A retained event can outlive its source window. Never attach today's
    // market state as if it had been available for that original decision.
    return {{"matched", false}, {"why", QStringLiteral("이 사건의 원래 입력을 현재 창에서 찾지 못했어요")}};
}

struct CollectorRuntimeOptions {
    QString root;
    QStringList binanceCoins{QStringLiteral("BTC"), QStringLiteral("ETH"), QStringLiteral("SOL"), QStringLiteral("HYPE"), QStringLiteral("ZEC")};
    QStringList upbitCoins{QStringLiteral("BTC"), QStringLiteral("ETH"), QStringLiteral("SOL")};
    QString revision; // Build revision; also the calculation-module version for events.
    qint64 diskFloorBytes = 30LL * 1024 * 1024 * 1024;
};

// External I/O and time boundary. Empty factories keep the production adapters;
// supplied factories own their returned adapters and must not return null.
struct CollectorRuntimeDependencies {
    enum class TransportRole { Hana, Ecb, Clock, Flow, Minutes };
    std::function<std::unique_ptr<TradingTransport>(TransportRole)> transport;
    std::function<std::unique_ptr<PublicCandleSocket>()> socket;
    std::function<qint64()> clock;
};

// Headless public-market collector (COLLECTOR-COMPARE-RULES-2026-11-06.md).
// It reuses KEEFI's coin-flow source per coin (same parsers, coverage and
// study code), writes observed records to CollectorStore and never reads the
// KEEFI vault, profile, accounts or keys.
class CollectorRuntime final : public QObject {
    Q_OBJECT
  public:
    explicit CollectorRuntime(CollectorRuntimeOptions, QObject* parent = nullptr);
    CollectorRuntime(CollectorRuntimeOptions, CollectorRuntimeDependencies, QObject* parent = nullptr);
    ~CollectorRuntime() override;
    bool start(QString* error = nullptr);
    void stop(); // Writes a stop record and flushes; open hours stay .part for the next start.
    QJsonObject summary() const;

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
