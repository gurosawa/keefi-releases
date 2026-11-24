#include "keefi/PortfolioPresentation.h"
#include "keefi/AssetIdentity.h"
#include "keefi/TradingTypes.h"

#include <QHash>
#include <QMap>
#include <QSet>
#include <algorithm>

namespace keefi {
namespace {
QString magnitude(const QString& value) { return value.startsWith('-') ? value.mid(1) : value; }
// Signed exact addition; an invalid operand makes the result unknown (empty).
QString signedSum(const QString& a, const QString& b) {
    return sumAssetAmounts({a, b});
}
void addHolding(PortfolioGroupView& group, const PortfolioHoldingView& holding) {
    if(!holding.displaySymbol.isEmpty()&&group.displaySymbol.isEmpty()){
        group.displaySymbol=holding.displaySymbol;group.displayName=holding.displayName;
    }
    switch (holding.state) {
    case PortfolioValueState::Fresh: break;
    case PortfolioValueState::Reference: ++group.references; break;
    case PortfolioValueState::Unvalued: ++group.unvalued; break;
    case PortfolioValueState::NotCounted: ++group.notCounted; break;
    }
    group.excluded += holding.excludedByUser;
    if (!group.storages.contains(holding.accountAlias)) group.storages.append(holding.accountAlias);
    group.presence = std::min(group.presence, holding.presence);
    group.holdings.append(holding);
}
void finalizeGroup(PortfolioGroupView& group) {
    QStringList values;
    for (const auto& holding : group.holdings)
        if (holding.state == PortfolioValueState::Fresh) values.append(holding.value);
    group.freshTotal = sumAssetAmounts(values);
}
void sortGroups(QList<PortfolioGroupView>& groups) {
    const auto rank = [](const auto& group) {
        if (group.presence == AssetPresence::Zero) return 2;
        return std::any_of(group.holdings.cbegin(), group.holdings.cend(), [](const auto& h) {
            return h.state == PortfolioValueState::Fresh;
        }) ? 0 : 1;
    };
    std::stable_sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        if (rank(a) == 0 && a.freshTotal != b.freshTotal) {
            const bool an = a.freshTotal.startsWith('-'), bn = b.freshTotal.startsWith('-');
            if (an != bn) return !an;
            const int comparison = decimal::compare(magnitude(a.freshTotal), magnitude(b.freshTotal));
            if (comparison) return an ? comparison < 0 : comparison > 0;
        }
        return a.identity < b.identity;
    });
}
} // namespace

QString groupedAmount(const QString& exactDecimal) {
    const bool negative = exactDecimal.startsWith('-');
    const auto body = magnitude(exactDecimal);
    if (!decimal::valid(body)) return exactDecimal;
    const auto dot = body.indexOf('.');
    QString whole = dot < 0 ? body : body.left(dot);
    const QString fraction = dot < 0 ? QString{} : body.mid(dot);
    for (int i = int(whole.size()) - 3; i > 0; i -= 3) whole.insert(i, ',');
    return (negative ? "-" : "") + whole + fraction;
}

QString krwAmount(const QString& exactDecimal) {
    const bool negative = exactDecimal.startsWith('-');
    const auto body = magnitude(exactDecimal);
    if (!decimal::valid(body)) return exactDecimal;
    const auto normalized = decimal::normalize(body);
    const auto dot = normalized.indexOf('.');
    QString whole = dot < 0 ? normalized : normalized.left(dot);
    if (dot >= 0 && normalized.at(dot + 1) >= QLatin1Char('5'))
        whole = decimal::add(whole, QStringLiteral("1"));
    return groupedAmount((negative && whole != "0" ? "-" : "") + whole);
}

QString priceAmount(const QString& exactDecimal) {
    QString value = exactDecimal.trimmed();
    if (value.contains('.') && decimal::valid(magnitude(value))) {
        while (value.endsWith('0')) value.chop(1);
        if (value.endsWith('.')) value.chop(1);
    }
    return groupedAmount(value);
}

PortfolioDustSummary portfolioDust(const PortfolioPresentation& view,const QString& threshold,bool hide){
    PortfolioDustSummary out;out.subtotal=view.summary.knownSubtotal;out.displayed=out.subtotal;
    if(!hide||!decimal::positive(threshold))return out;
    for(const auto& group:view.groups){
        const bool safe=!group.holdings.isEmpty()&&std::all_of(group.holdings.cbegin(),group.holdings.cend(),[](const auto& h){
            return h.state==PortfolioValueState::Fresh&&h.presence!=AssetPresence::NeedsCheck&&decimal::valid(h.value)&&!h.quantity.startsWith('-');});
        if(!safe||!decimal::valid(group.freshTotal)||decimal::compare(group.freshTotal,threshold)>=0)continue;
        out.hidden=signedSum(out.hidden,group.freshTotal);out.hiddenIdentities.append(group.identity);
    }
    out.displayed=signedSum(out.subtotal,out.hidden=="0"?QString("0"):"-"+out.hidden);return out;
}

QString portfolioAssetClass(const PortfolioHoldingView& holding) {
    if (holding.asset == "KRW" || holding.kind == AssetComponentKind::Cash) return "cash";
    return holding.market == "domestic_equity" ? "stock" : "coin";
}

QString portfolioHoldingKey(const PortfolioHoldingView& holding) {
    return holding.connectionId + ':' + holding.accountScope + ':' + holding.componentId;
}

QList<PortfolioGroupView> scopedPortfolioGroups(const QList<PortfolioGroupView>& groups,
    const QString& assetClass, const QString& connectionId) {
    QList<PortfolioGroupView> result;
    for (const auto& original : groups) {
        PortfolioGroupView group; group.identity = original.identity; group.asset = original.asset;
        for (const auto& holding : original.holdings)
            if ((assetClass.isEmpty() || portfolioAssetClass(holding) == assetClass) &&
                (connectionId.isEmpty() || holding.connectionId == connectionId)) addHolding(group, holding);
        if (!group.holdings.isEmpty()) { finalizeGroup(group); result.append(group); }
    }
    sortGroups(result);
    return result;
}

QString portfolioAssetLabel(const QString& asset,const QString& symbol) {
    if(!asset.startsWith("solana:"))return asset;
    if(!symbol.isEmpty())return symbol;
    const auto mint=asset.mid(7);return mint.left(6)+QStringLiteral("…")+mint.right(6);
}
QString portfolioGroupLabel(const PortfolioGroupView& group) {
    const auto label = portfolioAssetLabel(group.asset, group.displaySymbol);
    if (group.identity.startsWith("solana:") && group.identity != "solana:native" && group.displaySymbol == "USDC")
        return label + QObject::tr(" · 지갑");
    if (group.identity == "exchange:SOL" || group.identity == "exchange:USDC")
        return label + QObject::tr(" · 거래소");
    return label;
}
QString portfolioQuantity(const PortfolioGroupView& group) {
    QStringList quantities;
    for (const auto& holding : group.holdings) {
        if (holding.state == PortfolioValueState::NotCounted && !holding.excludedByUser) continue;
        if (!decimal::valid(magnitude(holding.quantity))) return {};
        auto quantity = holding.quantity;
        if (holding.kind == AssetComponentKind::Liability && !quantity.startsWith('-')) quantity.prepend('-');
        quantities.append(quantity);
    }
    return quantities.isEmpty() ? QString{} : sumAssetAmounts(quantities);
}

QString portfolioHoldingQuantity(const PortfolioHoldingView& holding) {
    const auto quantity = holding.quantity.isEmpty() ? QObject::tr("확인 필요") : groupedAmount(holding.quantity);
    const bool contracts = holding.kind == AssetComponentKind::PositionNotional && holding.provider == "okx" &&
        (holding.market == "okx_unified" || holding.market == "okx_futures");
    return contracts ? QObject::tr("계약 수 %1").arg(quantity) : quantity;
}

QString portfolioAllocationValue(const PortfolioGroupView& group) {
    QString total = "0";
    for (const auto& holding : group.holdings)
        if (holding.state == PortfolioValueState::Fresh && holding.kind != AssetComponentKind::Liability &&
            decimal::positive(holding.value)) total = signedSum(total, holding.value);
    return total;
}

QString portfolioAllocationTotal(const QList<PortfolioGroupView>& groups) {
    QString total = "0";
    for (const auto& group : groups) total = signedSum(total, portfolioAllocationValue(group));
    return total;
}

int portfolioShareTenths(const QString& value, const QString& total) {
    if (!decimal::valid(value) || !decimal::valid(total)) return -1;
    // Whole-won values of up to 15 digits leave room for the scale and the
    // half-up rounding term in 64 bits. Keep the full decimal path otherwise.
    if (value.size() <= 15 && total.size() <= 15 && !value.contains('.') && !total.contains('.')) {
        const auto amount = value.toULongLong();
        const auto sum = total.toULongLong();
        if (!sum || amount > sum) return -1;
        return int((amount * 1000 + sum / 2) / sum);
    }
    if (!decimal::positive(total) || decimal::compare(value, total) > 0) return -1;
    const auto scaled = decimal::multiply(value, "2000");
    if (!decimal::valid(scaled)) return -1;
    int low = 0, high = 1000;
    while (low < high) {
        const int middle = (low + high + 1) / 2;
        const auto threshold = decimal::multiply(total, QString::number(2 * middle - 1));
        if (!decimal::valid(threshold)) return -1;
        if (decimal::compare(scaled, threshold) >= 0) low = middle;
        else high = middle - 1;
    }
    return low;
}

QString portfolioShareText(const QString& value, const QString& total) {
    const int share = portfolioShareTenths(value, total);
    if (share < 0) return QStringLiteral("—");
    if (share == 0 && decimal::positive(value)) return QStringLiteral("<0.1%");
    return QString::number(share / 10) + '.' + QString::number(share % 10) + '%';
}

QString providerPnlText(const PortfolioHoldingView& holding) {
    if (holding.providerPnl.isEmpty() || holding.providerPnlCurrency.isEmpty()) return QStringLiteral("—");
    const auto amount = holding.providerPnlCurrency == "KRW" ? krwAmount(holding.providerPnl) : priceAmount(holding.providerPnl);
    const auto prefix = !holding.providerPnl.startsWith('-') && decimal::positive(holding.providerPnl) && amount != "0" ? "+" : "";
    return prefix + amount + ' ' + holding.providerPnlCurrency;
}

PortfolioPresentation presentPortfolio(const QList<AssetSnapshot>& snapshots,
    const AssetValuationResult& valuation, const QList<PortfolioConnection>& connections) {
    PortfolioPresentation out;
    QMap<QString, AssetSnapshot> scopes;
    for (const auto& snapshot : snapshots) scopes[snapshot.accountScope] = snapshot;
    QHash<QString, PortfolioConnection> byConnection;
    for (const auto& connection : connections) byConnection.insert(connection.account.id, connection);
    QHash<QString, AssetPresence> presence;
    QHash<QString, AssetComponentKind> kinds;
    QHash<QString, AssetComponent> components;
    for (const auto& snapshot : snapshots) for (const auto& component : snapshot.components) {
        auto value = assetPresence({component.quantity}, {component.reportedValue});
        // A complete zero balance with an explicit zero PnL is an empty asset
        // row, not an unknown amount. Positions have their own non-counted rows
        // and stay visible even when opposing exposure nets to zero equity.
        const bool uncertainEquity = component.kind == AssetComponentKind::Equity &&
            (!snapshot.balanceComplete || assetPresence({component.providerUnrealizedPnl}) != AssetPresence::Zero);
        if (value == AssetPresence::Zero && (!component.evidence.complete || uncertainEquity))
            value = AssetPresence::NeedsCheck;
        presence.insert(snapshot.accountScope + QChar(0) + component.id, value);
        kinds.insert(snapshot.accountScope + QChar(0) + component.id, component.kind);
        components.insert(snapshot.accountScope + QChar(0) + component.id, component);
    }
    QHash<QString, qsizetype> groupIndex;
    for (const auto& row : valuation.rows) {
        const auto snapshot = scopes.value(row.accountScope);
        const auto key = row.accountScope + QChar(0) + row.componentId;
        auto identityComponent=components.value(key);
        if(identityComponent.asset.isEmpty()){identityComponent.asset=row.asset;identityComponent.id=row.componentId;}
        const auto identity = assetIdentity(snapshot, identityComponent);
        if (!groupIndex.contains(identity)) {
            groupIndex.insert(identity, out.groups.size());
            PortfolioGroupView group; group.identity = identity; group.asset = row.asset;
            out.groups.append(group);
        }
        auto& group = out.groups[groupIndex.value(identity)];
        const auto connection = byConnection.value(snapshot.connectionId);
        PortfolioHoldingView holding;
        holding.connectionId = snapshot.connectionId; holding.componentId = row.componentId; holding.accountScope = row.accountScope;
        holding.accountAlias = connection.account.alias.isEmpty() ? snapshot.provider : connection.account.alias;
        holding.provider = snapshot.provider; holding.market = snapshot.market;
        holding.asset = row.asset; holding.quantity = row.quantity; holding.value = row.value;
        holding.referenceValue = row.referenceValue; holding.reason = row.reason;
        holding.excludedByUser = row.excludedByUser;
        holding.balanceTimeMs = snapshot.timeMs; holding.priceEvidence = row.priceEvidence;holding.conversionEvidence=row.conversionEvidence;
        holding.presence = presence.value(key, AssetPresence::NeedsCheck);
        const auto kind = kinds.value(key, AssetComponentKind::Holding);
        const bool baseKind = kind == AssetComponentKind::Holding || kind == AssetComponentKind::Cash ||
            kind == AssetComponentKind::Liability || kind == AssetComponentKind::Equity;
        if (row.excludedByUser) holding.state = PortfolioValueState::NotCounted;
        else if (!row.value.isEmpty() && row.included) holding.state = PortfolioValueState::Fresh;
        else if (!row.referenceValue.isEmpty()) holding.state = PortfolioValueState::Reference;
        else if (!row.included && (!baseKind || row.reason.contains("equity"))) holding.state = PortfolioValueState::NotCounted;
        else holding.state = PortfolioValueState::Unvalued;
        if (kind == AssetComponentKind::Equity && holding.presence == AssetPresence::Zero && holding.state != PortfolioValueState::Fresh)
            holding.presence = AssetPresence::NeedsCheck;
        holding.kind = kind;
        const auto component = components.value(key);
        // Preserve the quantity receipt independently of the price/conversion.
        // A wholly absent component receipt can only cite the snapshot scope;
        // it never changes the evaluator's original eligibility or value.
        holding.balanceEvidence = component.evidence == AssetEvidence{} ? snapshot.evidence : component.evidence;
        holding.displaySymbol=component.displaySymbol;holding.displayName=component.displayName;
        holding.metadataEvidence=component.metadataEvidence;holding.metadataIssue=component.metadataIssue;holding.priceIssue=component.priceIssue;
        const auto& evidence = component.evidence;
        const auto freshAt = [&](qint64 timestamp) {
            return timestamp > 0 && timestamp <= valuation.timeMs && valuation.timeMs - timestamp <= DisplayFreshnessMs;
        };
        if (decimal::valid(magnitude(row.providerUnrealizedPnl)) && !row.providerPnlCurrency.isEmpty() &&
            evidence.complete && !evidence.source.isEmpty() && !evidence.scope.isEmpty() && evidence.scope==row.accountScope && freshAt(snapshot.timeMs) &&
            freshAt(evidence.receivedAtMs) && (evidence.sourceTimeMs == 0 || freshAt(evidence.sourceTimeMs))) {
            holding.providerPnl = row.providerUnrealizedPnl;
            holding.providerPnlCurrency = row.providerPnlCurrency;
            holding.providerPnlSource = evidence.source;
            holding.providerPnlTimeMs = evidence.receivedAtMs;
        }
        addHolding(group, holding);
    }
    for (auto& group : out.groups) finalizeGroup(group);
    sortGroups(out.groups);

    QSet<QString> displayed, displayedScopes;
    for (const auto& snapshot : snapshots) if (!snapshot.components.isEmpty()) {
        displayed.insert(snapshot.connectionId); displayedScopes.insert(snapshot.accountScope);
    }
    int completed = 0;
    for (const auto& connection : connections) {
        completed += connection.state == "complete";
        if (connection.active) out.summary.readingAccounts.append(connection.account.alias);
        const bool verifiedAlias = connection.state == "complete" && !connection.accountScope.isEmpty() &&
            displayedScopes.contains(connection.accountScope);
        if (connection.shared || verifiedAlias || (displayed.contains(connection.account.id) && (connection.state == "complete" || connection.active))) continue;
        out.missing.append({connection.account.id, connection.account.alias, connection.account.market, connection.stage, connection.active});
        if (!connection.active) out.summary.missingAccounts.append(connection.account.alias);
    }
    auto& summary = out.summary;
    summary.complete = valuation.complete && !connections.isEmpty() && completed == connections.size();
    summary.netAssets = valuation.netAssets;
    summary.knownSubtotal = valuation.knownSubtotal;
    QStringList referenceValues;
    for (const auto& group : out.groups) {
        summary.excludedGroups += group.excluded > 0;
        summary.unvaluedGroups += group.unvalued > 0;
        summary.referenceGroups += group.references > 0;
        for (const auto& holding : group.holdings)
            if (holding.state == PortfolioValueState::Reference) referenceValues.append(holding.referenceValue);
    }
    summary.referenceTotal = sumAssetAmounts(referenceValues);
    return out;
}

PortfolioPresentation recordedPortfolio(const QList<AssetSnapshot>& snapshots,
    const QList<AssetQuote>& quotes, const QList<PortfolioConnection>& connections,
    qint64 nowMs, const QSet<QString>& excludedAssets) {
    if (nowMs <= 0) return {};
    QSet<QString> connectionIds, savedConnections, scopes;
    for (const auto& connection : connections) connectionIds.insert(connection.account.id);
    QList<AssetSnapshot> saved;
    qint64 asOf = 0;
    const auto observeTime = [&](qint64 time) {
        if (time < 0 || time > nowMs) return false;
        asOf = qMax(asOf, time);
        return true;
    };
    const auto observeEvidence = [&](const AssetEvidence& evidence) {
        return observeTime(evidence.sourceTimeMs) && observeTime(evidence.receivedAtMs);
    };
    for (const auto& snapshot : snapshots) {
        if (!connectionIds.contains(snapshot.connectionId)) continue;
        if (!observeTime(snapshot.timeMs) || !observeEvidence(snapshot.evidence)) return {};
        for (const auto& component : snapshot.components)
            if (!observeEvidence(component.evidence)) return {};
        saved.append(snapshot);
        savedConnections.insert(snapshot.connectionId);
        scopes.insert(snapshot.accountScope);
    }
    if (saved.isEmpty()) return {};
    QList<AssetQuote> savedQuotes;
    for (const auto& quote : quotes) {
        if (!scopes.contains(quote.evidence.scope) && !quote.evidence.scope.startsWith("public:")) continue;
        if (!observeEvidence(quote.evidence)) return {};
        savedQuotes.append(quote);
    }
    if (asOf <= 0) return {};
    auto recordedConnections = connections;
    for (auto& connection : recordedConnections) {
        connection.active = false;
        connection.state = savedConnections.contains(connection.account.id) ? "complete" : "not_read";
        if (connection.state == "not_read") connection.stage = "저장된 자료 없음";
    }
    // A past evaluation time is explicit, not an enlarged freshness allowance.
    // Evidence that was already missing/stale in that saved state stays unknown.
    const auto valuation = AssetValuation::value(saved, savedQuotes, "KRW", asOf, DisplayFreshnessMs, {}, excludedAssets);
    auto view = presentPortfolio(saved, valuation, recordedConnections);
    view.asOfTimeMs = asOf;
    for (auto& group : view.groups)
        for (auto& holding : group.holdings) holding.asOfTimeMs = asOf;
    return view;
}

} // namespace keefi
