#include "keefi/PortfolioContribution.h"
#include <map>
#include <tuple>

namespace keefi {
namespace {
using RowKey = std::tuple<QString, QString, QString>;
RowKey key(const AssetValueRow& row) { return {row.accountScope, row.componentId, row.asset}; }
QString negative(const QString& value) { return value.startsWith('-') ? value.mid(1) : "-" + value; }
struct Origin {
    QString provider, market, quantity, valuationGroup;
    AssetComponentKind kind;
    bool valid = false;
};
bool known(const AssetEvidence& evidence, qint64 at) {
    return !evidence.source.trimmed().isEmpty() && evidence.complete && evidence.receivedAtMs > 0 &&
        evidence.receivedAtMs <= at && evidence.sourceTimeMs >= 0 && evidence.sourceTimeMs <= at;
}
std::map<RowKey, Origin> origins(const QList<AssetSnapshot>& snapshots, qint64 at) {
    std::map<RowKey, Origin> result;
    std::map<QString, qsizetype> scopeCounts;
    for (const auto& snapshot : snapshots) ++scopeCounts[snapshot.accountScope];
    for (const auto& snapshot : snapshots) {
        std::map<QString, qsizetype> componentCounts;
        for (const auto& component : snapshot.components) ++componentCounts[component.id];
        for (const auto& component : snapshot.components) {
            const bool valid = scopeCounts[snapshot.accountScope] == 1 && componentCounts[component.id] == 1 &&
                snapshot.timeMs > 0 && snapshot.timeMs <= at && snapshot.balanceComplete && known(snapshot.evidence, at) &&
                known(component.evidence, at);
            auto [entry, inserted] = result.emplace(RowKey{snapshot.accountScope, component.id, component.asset},
                Origin{snapshot.provider, snapshot.market, component.quantity, component.valuationGroup, component.kind, valid});
            if (!inserted) entry->second.valid = false;
        }
    }
    return result;
}
PortfolioContributionClass classify(const Origin& origin, const QString& asset, QString& reason) {
    if (origin.kind == AssetComponentKind::Equity) {
        reason = QStringLiteral("계좌 equity 증감 · 기초자산의 현물 노출로 분류하지 않습니다.");
        return PortfolioContributionClass::Unclassified;
    }
    if (origin.kind == AssetComponentKind::Liability) {
        reason = QStringLiteral("부채 증감 · 자산군 노출을 확인할 수 없습니다.");
        return PortfolioContributionClass::Unclassified;
    }
    if (origin.kind == AssetComponentKind::Cash) return PortfolioContributionClass::Cash;
    if (origin.kind == AssetComponentKind::Holding) {
        if (origin.market == "domestic_equity" && (origin.provider == "toss" || origin.provider == "kis" ||
            origin.provider == "kiwoom" || origin.provider == "ls")) return PortfolioContributionClass::DomesticStock;
        if (origin.provider == "phantom" && origin.market == "solana") return PortfolioContributionClass::Coin;
        if (origin.market == "spot" && (origin.provider == "upbit" || origin.provider == "bithumb" || origin.provider == "binance" || origin.provider == "okx")) {
            // Old exchange captures store fiat balances as Holding. A currency
            // symbol alone is insufficient to upgrade them into Cash metadata.
            static const QSet<QString> fiatSymbols{"KRW", "USD", "EUR", "JPY", "GBP", "AUD", "CAD", "CHF", "CNY", "HKD", "SGD"};
            if (!fiatSymbols.contains(asset)) return PortfolioContributionClass::Coin;
            reason = QStringLiteral("현금성 분류 메타데이터가 없어 잔여에 포함합니다.");
            return PortfolioContributionClass::Unclassified;
        }
    }
    reason = QStringLiteral("자산군 분류 근거를 확인할 수 없습니다.");
    return PortfolioContributionClass::Unclassified;
}
}
QString portfolioContributionClassLabel(PortfolioContributionClass assetClass) {
    switch (assetClass) {
    case PortfolioContributionClass::Coin: return QStringLiteral("코인");
    case PortfolioContributionClass::DomesticStock: return QStringLiteral("국내 주식");
    case PortfolioContributionClass::UsStock: return QStringLiteral("미국 주식");
    case PortfolioContributionClass::Cash: return QStringLiteral("현금성");
    case PortfolioContributionClass::Unclassified: return QStringLiteral("분류 미확인·기타");
    }
    return QStringLiteral("분류 미확인·기타");
}
PortfolioContributions portfolioContributions(const AssetValuationResult& opening, const QString& openingPolicy,
    const QList<AssetSnapshot>& openingSnapshots, const AssetValuationResult& closing, const QString& closingPolicy,
    const QList<AssetSnapshot>& closingSnapshots) {
    PortfolioContributions result;
    result.comparison = comparePortfolioValues(opening, openingPolicy, closing, closingPolicy);
    if (!result.comparison.available) { result.reason = result.comparison.reason; return result; }
    const auto fail = [&](const QString& reason) {
        result.available = false; result.reason = reason; result.rows.clear(); result.groups.clear(); return result;
    };
    const auto before = origins(openingSnapshots, opening.timeMs), after = origins(closingSnapshots, closing.timeMs);
    std::map<RowKey, const AssetValueRow*> closingRows;
    for (const auto& row : closing.rows) closingRows.emplace(key(row), &row);
    struct Amounts { QStringList before, after, changes; };
    std::map<PortfolioContributionClass, Amounts> grouped;
    for (const auto& row : opening.rows) {
        if (!row.included || row.value.isEmpty()) continue;
        const auto rowKey = key(row);
        const auto& closingRow = *closingRows.at(rowKey);
        PortfolioContributionRow contribution;
        contribution.accountScope = row.accountScope; contribution.componentId = row.componentId; contribution.asset = row.asset;
        contribution.openingValue = sumAssetAmounts({row.value}); contribution.closingValue = sumAssetAmounts({closingRow.value});
        contribution.difference = sumAssetAmounts({contribution.closingValue, negative(contribution.openingValue)});
        if (contribution.difference.isEmpty()) return fail(QStringLiteral("자산별 증감이 금액 정밀도 상한을 넘었습니다."));
        const auto a = before.find(rowKey), b = after.find(rowKey);
        if (a == before.end() || b == after.end() || !a->second.valid || !b->second.valid ||
            a->second.quantity != row.quantity || b->second.quantity != closingRow.quantity) {
            contribution.reason = QStringLiteral("평가에 사용한 원본 자산 행·시각을 확인할 수 없어 잔여에 포함합니다.");
        } else if (a->second.provider != b->second.provider || a->second.market != b->second.market ||
            a->second.kind != b->second.kind || a->second.valuationGroup != b->second.valuationGroup) {
            contribution.reason = QStringLiteral("두 시점의 자산군 분류 근거가 달라 잔여에 포함합니다.");
        } else contribution.assetClass = classify(a->second, row.asset, contribution.reason);
        auto& amounts = grouped[contribution.assetClass];
        amounts.before.append(contribution.openingValue); amounts.after.append(contribution.closingValue);
        amounts.changes.append(contribution.difference); result.rows.append(contribution);
    }
    QStringList openingGroups, closingGroups, differences;
    for (const auto& [assetClass, amounts] : grouped) {
        PortfolioContributionGroup group{assetClass, sumAssetAmounts(amounts.before), sumAssetAmounts(amounts.after),
            sumAssetAmounts(amounts.changes), amounts.changes.size()};
        if (group.openingValue.isEmpty() || group.closingValue.isEmpty() || group.difference.isEmpty())
            return fail(QStringLiteral("자산군별 금액이 금액 정밀도 상한을 넘었습니다."));
        openingGroups.append(group.openingValue); closingGroups.append(group.closingValue); differences.append(group.difference);
        result.groups.append(group);
    }
    if (sumAssetAmounts(openingGroups) != result.comparison.openingValue ||
        sumAssetAmounts(closingGroups) != result.comparison.closingValue ||
        sumAssetAmounts(differences) != result.comparison.difference)
        return fail(QStringLiteral("자산군 기여도와 관측 증감 합계가 일치하지 않습니다."));
    result.available = true;
    return result;
}
} // namespace keefi
