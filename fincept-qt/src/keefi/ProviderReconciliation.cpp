#include "keefi/ProviderReconciliation.h"
#include "keefi/ProductIdentity.h"

namespace keefi {
namespace {
QString magnitude(const QString& value) { return value.startsWith('-') ? value.mid(1) : value; }
QString normalized(const QString& value) {
    auto result = decimal::normalize(magnitude(value));
    if (!result.isEmpty() && result != "0" && value.startsWith('-')) result.prepend('-');
    return result;
}
QString signedDifference(const QString& left, const QString& right) {
    const bool negativeLeft = left.startsWith('-'), negativeRight = right.startsWith('-');
    const auto a = magnitude(left), b = magnitude(right);
    QString result;
    if (negativeLeft != negativeRight) {
        result = decimal::add(a, b);
        if (negativeLeft && result != "0") result.prepend('-');
    } else if (decimal::compare(a, b) >= 0) {
        result = decimal::subtract(a, b);
        if (negativeLeft && result != "0") result.prepend('-');
    } else {
        result = decimal::subtract(b, a);
        if (!negativeLeft && result != "0") result.prepend('-');
    }
    return normalized(result);
}
bool fresh(const AssetEvidence& evidence, qint64 at, qint64 age, qint64 maximumSourceLeadMs = 0) {
    return evidence.complete && !evidence.source.isEmpty() && !evidence.url.isEmpty() && !evidence.scope.isEmpty() &&
        evidence.receivedAtMs > 0 && evidence.receivedAtMs <= at && at - evidence.receivedAtMs <= age &&
        (evidence.sourceTimeMs == 0 || (evidence.sourceTimeMs > 0 && evidence.sourceTimeMs <= at &&
                                      (evidence.sourceTimeMs <= evidence.receivedAtMs ||
                                       evidence.sourceTimeMs - evidence.receivedAtMs <= maximumSourceLeadMs) &&
                                      at - evidence.sourceTimeMs <= age));
}
const AssetQuote* usdtKrwQuote(const QList<AssetQuote>& quotes, qint64 at, qint64 age) {
    const AssetQuote* selected = nullptr;
    bool conflict = false;
    for (const auto& quote : quotes) {
        const auto& evidence = quote.evidence;
        if (quote.baseAsset != "USDT" || quote.quoteAsset != "KRW" ||
            evidence.scope != "public:upbit:KRW-USDT" || !decimal::positive(quote.price) ||
            evidence.sourceTimeMs <= 0 || !fresh(evidence, at, age, 2000)) continue;
        if (!selected || evidence.sourceTimeMs > selected->evidence.sourceTimeMs) {
            selected = &quote;
            conflict = false;
        } else if (evidence.sourceTimeMs == selected->evidence.sourceTimeMs &&
                   decimal::normalize(quote.price) != decimal::normalize(selected->price)) conflict = true;
    }
    return conflict ? nullptr : selected;
}
} // namespace

ProviderTotalObservation unsupportedProviderTotal(const AccountRecord& account) {
    ProviderTotalObservation result;
    result.connectionId = account.id; result.provider = account.provider;
    result.environment = account.environment; result.market = account.market;
    result.reason = account.provider == "binance" && account.market == "spot"
        ? QStringLiteral("현물 잔고 API는 비교할 계좌 평가총액을 제공하지 않습니다.")
        : account.market == "solana" ? QStringLiteral("지갑 RPC는 수량을 제공하며 공식 평가총액을 제공하지 않습니다.")
        : QStringLiteral("같은 범위의 공식 총액 비교가 아직 지원되지 않습니다.");
    return result;
}

ProviderTotalObservation captureBinanceUsdmProviderTotal(const AssetSnapshot& snapshot, const QJsonObject& account,
    const QJsonObject& configuration, const AssetEvidence& configurationEvidence) {
    ProviderTotalObservation result;
    result.connectionId = snapshot.connectionId; result.accountScope = snapshot.accountScope; result.snapshotId = snapshot.id;
    result.provider = snapshot.provider; result.environment = snapshot.environment; result.market = snapshot.market;
    result.metric = "totalMarginBalance"; result.evidence = snapshot.evidence;
    result.configurationEvidence = configurationEvidence;
    result.availability = ProviderTotalAvailability::Unavailable;
    if (snapshot.provider != "binance" || snapshot.market != "usdm_futures") {
        result.availability = ProviderTotalAvailability::Unsupported;
        result.reason = QStringLiteral("지원하는 공식 선물 총액 범위가 아닙니다."); return result;
    }
    if (!configurationEvidence.complete) {
        result.reason = QStringLiteral("이번 조회에서 계좌 모드를 확인하지 못했습니다."); return result;
    }
    if (!configuration.value("multiAssetsMargin").isBool()) {
        result.reason = QStringLiteral("단일자산·다중자산 모드를 확인하지 못했습니다."); return result;
    }
    const bool multiAssets = configuration.value("multiAssetsMargin").toBool();
    result.currency = multiAssets ? "USD" : "USDT";
    result.coverage = multiAssets ? QStringLiteral("USDⓈ-M · 다중자산 · 업체 USD 환산")
                                  : QStringLiteral("USDⓈ-M · 단일자산 · USDT equity만");
    const auto value = account.value("totalMarginBalance");
    if (value.isString()) result.amount = normalized(value.toString());
    if (multiAssets) {
        result.availability = ProviderTotalAvailability::Unsupported;
        result.reason = QStringLiteral("다중자산 USD 총액과 개별 자산 평가의 환산 기준을 맞추지 못했습니다."); return result;
    }
    if (result.amount.isEmpty()) {
        result.reason = QStringLiteral("공식 totalMarginBalance가 제공되지 않았습니다."); return result;
    }
    result.availability = ProviderTotalAvailability::Available;
    return result;
}

ProviderReconciliationResult reconcileProviderTotal(const ProviderTotalObservation& official, const AssetSnapshot* snapshot,
    const QList<AssetQuote>& quotes, qint64 at, qint64 age) {
    ProviderReconciliationResult result; result.official = official;
    const auto unavailable = [&](const QString& reason) {
        result.state = ProviderReconciliationState::Unavailable; result.reason = reason; return result;
    };
    if (official.availability == ProviderTotalAvailability::Unsupported) {
        result.state = ProviderReconciliationState::Unsupported; result.reason = official.reason; return result;
    }
    if (official.availability != ProviderTotalAvailability::Available)
        return unavailable(official.reason.isEmpty() ? QStringLiteral("공식 총액을 조회하지 못했습니다.") : official.reason);
    if (!snapshot || at <= 0 || age < 0 || official.provider != "binance" || official.market != "usdm_futures" ||
        official.currency != "USDT" || official.metric != "totalMarginBalance" || normalized(official.amount).isEmpty() ||
        official.accountScope.isEmpty() || official.connectionId.isEmpty() || official.snapshotId.isEmpty() ||
        snapshot->id != official.snapshotId || snapshot->evidence != official.evidence ||
        snapshot->connectionId != official.connectionId || snapshot->accountScope != official.accountScope ||
        snapshot->provider != official.provider || snapshot->market != official.market || snapshot->environment != official.environment ||
        official.evidence.scope != official.accountScope || official.configurationEvidence.scope != official.accountScope ||
        snapshot->evidence.scope != official.accountScope)
        return unavailable(QStringLiteral("공식 총액과 %1의 계좌·통화·평가 범위가 일치하지 않습니다.").arg(productName()));
    if (!snapshot->balanceComplete || snapshot->timeMs <= 0 || snapshot->timeMs > at || at - snapshot->timeMs > age ||
        !fresh(snapshot->evidence, at, age) || !fresh(official.evidence, at, age) || !fresh(official.configurationEvidence, at, age))
        return unavailable(QStringLiteral("공식 잔고 또는 계좌 모드의 조회 시각을 다시 확인해야 합니다."));
    AssetSnapshot subset = *snapshot; subset.components.clear();
    for (const auto& component : snapshot->components)
        if (component.kind == AssetComponentKind::Equity && component.asset == "USDT" && component.valuationGroup == "futures")
            subset.components.append(component);
    if (subset.components.size() != 1 || subset.components.first().evidence.scope != official.accountScope)
        return unavailable(QStringLiteral("비교 범위인 USDT equity 행을 하나로 확인하지 못했습니다."));
    const auto valuation = AssetValuation::value({subset}, {}, "USDT", at, age);
    if (!valuation.complete || valuation.netAssets.isEmpty())
        return unavailable(QStringLiteral("%1의 USDT equity를 확인하지 못했습니다.").arg(productName()));
    result.keefiAmount = valuation.netAssets;
    result.difference = signedDifference(result.keefiAmount, official.amount);
    result.absoluteDifference = magnitude(result.difference);
    result.relativeTolerance = decimal::multiply(magnitude(official.amount), "0.005");
    if (result.difference.isEmpty() || !decimal::valid(result.absoluteDifference) || !decimal::valid(result.relativeTolerance))
        return unavailable(QStringLiteral("비교 금액의 정밀도 상한을 초과했습니다."));
    if (result.absoluteDifference == "0") { result.state = ProviderReconciliationState::Exact; return result; }
    if (decimal::compare(result.absoluteDifference, result.relativeTolerance) <= 0) {
        result.state = ProviderReconciliationState::WithinTolerance; return result;
    }
    const auto* fx = usdtKrwQuote(quotes, at, age);
    if (!fx) return unavailable(QStringLiteral("1,000원 허용 기준을 확인할 최신 USDT/KRW 시세가 필요합니다."));
    result.fxEvidence = fx->evidence;
    result.krwDifference = decimal::multiply(result.absoluteDifference, fx->price);
    if (!decimal::valid(result.krwDifference)) return unavailable(QStringLiteral("원화 차이의 정밀도 상한을 초과했습니다."));
    result.state = decimal::compare(result.krwDifference, "1000") <= 0
        ? ProviderReconciliationState::WithinTolerance : ProviderReconciliationState::Mismatch;
    return result;
}

QString providerReconciliationLabel(ProviderReconciliationState state) {
    switch (state) {
    case ProviderReconciliationState::Exact: return QStringLiteral("일치");
    case ProviderReconciliationState::WithinTolerance: return QStringLiteral("허용 범위");
    case ProviderReconciliationState::Mismatch: return QStringLiteral("불일치");
    case ProviderReconciliationState::Unavailable: return QStringLiteral("확인 필요");
    case ProviderReconciliationState::Unsupported: return QStringLiteral("비교 미지원");
    }
    return {};
}
} // namespace keefi
