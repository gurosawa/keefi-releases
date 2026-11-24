#include "keefi/PortfolioComparison.h"
#include <map>
#include <tuple>

namespace keefi {
namespace {
using RowKey = std::tuple<QString, QString, QString>;
using RowCoverage = std::tuple<bool, bool, bool>; // included, explicitly excluded, value available
struct Coverage {
    QSet<QString> accounts;
    std::map<RowKey, RowCoverage> rows;
    QString subtotal;
};
QString inspect(const AssetValuationResult& point, Coverage& coverage) {
    // A result cannot recover which account was incomplete or removed from an
    // earlier capture. Such coverage needs a future checkpoint contract.
    if (!point.issues.isEmpty()) return QStringLiteral("계좌 조회 범위 또는 평가 입력의 문제가 있어 비교할 수 없습니다.");
    for (const auto& scope : point.accountScopes) {
        if (scope.trimmed().isEmpty() || coverage.accounts.contains(scope))
            return QStringLiteral("계좌 범위가 없거나 중복되어 비교할 수 없습니다.");
        coverage.accounts.insert(scope);
    }
    if (coverage.accounts.isEmpty()) return QStringLiteral("비교할 계좌 범위가 없습니다.");
    std::map<std::pair<QString, QString>, bool> components;
    QStringList values;
    for (const auto& row : point.rows) {
        if (!coverage.accounts.contains(row.accountScope) || row.componentId.trimmed().isEmpty() || row.asset.trimmed().isEmpty() ||
            row.currency != point.currency || !components.emplace(std::make_pair(row.accountScope, row.componentId), true).second)
            return QStringLiteral("자산 식별자·통화·계좌 범위가 없거나 중복되어 비교할 수 없습니다.");
        if ((row.included && row.excludedByUser) || (!row.included && !row.value.isEmpty()))
            return QStringLiteral("자산의 평가 포함 상태가 일관되지 않습니다.");
        if (row.included) {
            if (row.value.isEmpty()) {
                if (point.complete || row.reason.trimmed().isEmpty())
                    return QStringLiteral("평가액 누락 상태가 일관되지 않습니다.");
            } else {
                const auto amount = sumAssetAmounts({row.value});
                if (amount.isEmpty()) return QStringLiteral("비교할 평가액이 유효한 금액이 아닙니다.");
                values.append(amount);
            }
        }
        coverage.rows.emplace(RowKey{row.accountScope, row.componentId, row.asset},
                              RowCoverage{row.included, row.excludedByUser, !row.value.isEmpty()});
    }
    if (!point.complete && values.isEmpty()) return QStringLiteral("두 시점에서 비교할 평가 완료 자산이 없습니다.");
    coverage.subtotal = sumAssetAmounts(values);
    if (coverage.subtotal.isEmpty() || coverage.subtotal != sumAssetAmounts({point.knownSubtotal}) ||
        (point.complete ? sumAssetAmounts({point.netAssets}) != coverage.subtotal : !point.netAssets.isEmpty()))
        return QStringLiteral("평가 행과 합계가 일치하지 않아 비교할 수 없습니다.");
    return {};
}

PortfolioComparison compareValues(const AssetValuationResult& opening, const QString& openingPolicyVersion,
    const AssetValuationResult& closing, const QString& closingPolicyVersion, bool wholeTotals) {
    PortfolioComparison result; result.fromMs = opening.timeMs; result.toMs = closing.timeMs;
    const auto unavailable = [&](const QString& reason) { result.reason = reason; return result; };
    if (openingPolicyVersion.trimmed().isEmpty() || openingPolicyVersion != closingPolicyVersion)
        return unavailable(QStringLiteral("같은 평가 정책 버전의 기록이 필요합니다."));
    if (opening.timeMs <= 0 || closing.timeMs <= opening.timeMs)
        return unavailable(QStringLiteral("앞선 기준 시각과 이후 관측 시각이 필요합니다."));
    if (opening.currency != "KRW" || closing.currency != opening.currency)
        return unavailable(QStringLiteral("같은 원화 평가 기록이 필요합니다."));
    Coverage before, after;
    if (const auto reason = inspect(opening, before); !reason.isEmpty()) return unavailable(reason);
    if (const auto reason = inspect(closing, after); !reason.isEmpty()) return unavailable(reason);
    if (before.accounts != after.accounts || opening.complete != closing.complete ||
        (wholeTotals ? !opening.complete : before.rows != after.rows))
        return unavailable(QStringLiteral("계좌·자산·미평가·평가 제외 범위가 달라 직접 비교할 수 없습니다."));
    const auto negativeOpening = before.subtotal.startsWith('-') ? before.subtotal.mid(1) : "-" + before.subtotal;
    const auto difference = sumAssetAmounts({after.subtotal, negativeOpening});
    if (difference.isEmpty()) return unavailable(QStringLiteral("평가액 차이가 금액 정밀도 상한을 넘었습니다."));
    result.available = true; result.complete = opening.complete && closing.complete;
    result.currency = opening.currency; result.openingValue = before.subtotal; result.closingValue = after.subtotal;
    result.difference = difference;
    for (const auto& row : opening.rows) if (row.excludedByUser) ++result.excludedRowCount;
    result.label = result.complete ? QStringLiteral("관측 평가액 증감 · 입출금 포함 가능")
                                  : QStringLiteral("동일 범위 부분합 증감 · 입출금 포함 가능");
    if (result.excludedRowCount > 0)
        result.label = (result.complete ? QStringLiteral("평가 대상 합계 증감") : QStringLiteral("동일 평가 대상 부분합 증감")) +
                       QStringLiteral(" · %1항목 제외 · 입출금 포함 가능").arg(result.excludedRowCount);
    return result;
}
} // namespace

PortfolioComparison comparePortfolioValues(const AssetValuationResult& opening, const QString& openingPolicy,
    const AssetValuationResult& closing, const QString& closingPolicy) {
    return compareValues(opening, openingPolicy, closing, closingPolicy, false);
}

PortfolioComparison compareCompletePortfolioTotals(const AssetValuationResult& opening, const QString& openingPolicy,
    const AssetValuationResult& closing, const QString& closingPolicy) {
    return compareValues(opening, openingPolicy, closing, closingPolicy, true);
}
} // namespace keefi
