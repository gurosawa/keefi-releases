#include "keefi/ValuationBundle.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>

namespace keefi {
namespace {
QString kindName(AssetComponentKind kind) {
    switch (kind) {
    case AssetComponentKind::Holding: return "Holding";
    case AssetComponentKind::Cash: return "Cash";
    case AssetComponentKind::Equity: return "Equity";
    case AssetComponentKind::Liability: return "Liability";
    case AssetComponentKind::BuyingPower: return "BuyingPower";
    case AssetComponentKind::Withdrawable: return "Withdrawable";
    case AssetComponentKind::PositionNotional: return "PositionNotional";
    case AssetComponentKind::UnrealizedPnl: return "UnrealizedPnl";
    case AssetComponentKind::ProviderPnl: return "ProviderPnl";
    }
    return "Unknown";
}
QString stateName(PortfolioValueState state) {
    switch (state) {
    case PortfolioValueState::Fresh: return "Fresh";
    case PortfolioValueState::Reference: return "Reference";
    case PortfolioValueState::Unvalued: return "Unvalued";
    case PortfolioValueState::NotCounted: return "NotCounted";
    }
    return "Unknown";
}
QString presenceName(AssetPresence presence) {
    return presence == AssetPresence::Held ? "Held" : presence == AssetPresence::Zero ? "Zero" : "NeedsCheck";
}
// The recalculator names price sources explicitly; KEEFI encodes them in the evidence scope.
QString venue(const QString& scope) {
    for (const auto& name : {QStringLiteral("upbit"), QStringLiteral("binance"), QStringLiteral("jupiter")})
        if (scope.startsWith("public:" + name + ":")) return name;
    return "account";
}
QJsonValue optional(const QString& value) { return value.isEmpty() ? QJsonValue() : QJsonValue(value); }

struct Labels {
    QHash<QString, QString> scopes, connections;
    QString connection(const QString& id) {
        if (id.isEmpty()) return id;
        if (!connections.contains(id)) connections.insert(id, QString("conn-%1").arg(connections.size() + 1));
        return connections.value(id);
    }
    QString scope(const QString& scope) {
        if (scope.isEmpty() || scope.startsWith("public:") || scope.startsWith("derived:")) return scope;
        if (scopes.contains(scope)) return scopes.value(scope);
        // Any other digest-like scope is an account identity, not a public route.
        static const QRegularExpression digest("\\A[0-9a-f]{32,128}\\z");
        if (!digest.match(scope).hasMatch()) return scope;
        scopes.insert(scope, QString("scope-%1").arg(scopes.size() + 1));
        return scopes.value(scope);
    }
    QJsonObject evidence(const AssetEvidence& e) {
        return {{"source", e.source}, {"url", e.url}, {"scope", scope(e.scope)},
                {"source_time_ms", double(e.sourceTimeMs)}, {"received_at_ms", double(e.receivedAtMs)},
                {"complete", e.complete}};
    }
    QJsonObject quote(const AssetQuote& q) {
        return {{"base", q.baseAsset}, {"quote", q.quoteAsset}, {"price", q.price},
                {"venue", venue(q.evidence.scope)}, {"evidence", evidence(q.evidence)}};
    }
};

QJsonObject response(const JournalEntry& e) {
    QJsonObject out{{"channel", e.channel}, {"operation", e.operation}, {"method", e.method},
                    {"host", e.host}, {"path", e.path}, {"status", e.status},
                    {"authenticated", e.authenticated}, {"timed_out", e.timedOut},
                    {"network_error", e.networkError}, {"received_at_ms", double(e.receivedAtMs)},
                    {"body_size", double(e.bodySize)}, {"body_sha256", QString::fromLatin1(e.bodySha256)},
                    {"body_omitted", e.bodyOmitted}};
    if (!e.body.isEmpty()) {
        const auto text = QString::fromUtf8(e.body);
        if (text.toUtf8() == e.body) out.insert("body_text", text);
        else out.insert("body_base64", QString::fromLatin1(e.body.toBase64()));
    }
    return out;
}
} // namespace

QJsonObject valuationBundle(const ValuationCapture& c, const QString& dustThreshold,
                            const QList<JournalEntry>& responses, bool recording,
                            const QString& application, const QString& version) {
    Labels labels;
    for (const auto& connection : c.connections) labels.connection(connection.account.id);
    for (const auto& snapshot : c.snapshots) {
        labels.connection(snapshot.connectionId);
        if (!snapshot.accountScope.isEmpty() && !labels.scopes.contains(snapshot.accountScope))
            labels.scopes.insert(snapshot.accountScope, QString("scope-%1").arg(labels.scopes.size() + 1));
    }

    QJsonArray connections;
    for (const auto& connection : c.connections)
        connections.append(QJsonObject{{"id", labels.connection(connection.account.id)}, {"alias", connection.account.alias},
                                       {"provider", connection.account.provider}, {"environment", connection.account.environment},
                                       {"market", connection.account.market}, {"state", connection.state},
                                       {"active", connection.active}, {"shared", connection.shared}});
    QJsonArray snapshots;
    for (const auto& s : c.snapshots) {
        QJsonArray components;
        for (const auto& part : s.components) {
            QJsonObject component{{"id", part.id}, {"asset", part.asset}, {"kind", kindName(part.kind)},
                                  {"quantity", part.quantity}, {"valuation_group", part.valuationGroup},
                                  {"evidence", labels.evidence(part.evidence)}};
            if (!part.reportedValue.isEmpty()) {
                component.insert("reported_value", part.reportedValue);
                component.insert("reported_value_currency", part.reportedValueCurrency);
            }
            if (!part.providerUnrealizedPnl.isEmpty()) {
                component.insert("provider_unrealized_pnl", part.providerUnrealizedPnl);
                component.insert("provider_pnl_currency", part.reportedValueCurrency);
            }
            if (!part.costBasis.isEmpty()) { component.insert("cost_basis", part.costBasis); component.insert("cost_currency", part.costCurrency); }
            if (!part.displaySymbol.isEmpty()) component.insert("display_symbol", part.displaySymbol);
            components.append(component);
        }
        snapshots.append(QJsonObject{{"account_scope", labels.scope(s.accountScope)}, {"connection_id", labels.connection(s.connectionId)},
                                     {"provider", s.provider}, {"environment", s.environment}, {"market", s.market},
                                     {"time_ms", double(s.timeMs)}, {"balance_complete", s.balanceComplete},
                                     {"coverage", s.coverage}, {"evidence", labels.evidence(s.evidence)},
                                     {"components", components}});
    }
    QJsonArray quotes, references;
    for (const auto& q : c.quotes) quotes.append(labels.quote(q));
    for (const auto& q : c.references) references.append(labels.quote(q));

    const auto& view = c.view;
    const auto allocationTotal = portfolioAllocationTotal(view.groups);
    QJsonArray groups, rows, hideUnvalued;
    for (const auto& group : view.groups) {
        bool fresh = false, partial = false, unvalued = !group.holdings.isEmpty();
        for (const auto& h : group.holdings) {
            fresh |= h.state == PortfolioValueState::Fresh;
            partial |= h.state == PortfolioValueState::Unvalued || h.state == PortfolioValueState::Reference;
            unvalued &= h.state == PortfolioValueState::Unvalued;
            QJsonArray legs;
            for (const auto& leg : h.conversionEvidence) legs.append(labels.evidence(leg));
            rows.append(QJsonObject{{"identity", group.identity}, {"account_scope", labels.scope(h.accountScope)},
                                    {"connection_id", labels.connection(h.connectionId)}, {"component_id", h.componentId},
                                    {"asset", h.asset}, {"kind", kindName(h.kind)}, {"quantity", h.quantity},
                                    {"state", stateName(h.state)}, {"presence", presenceName(h.presence)},
                                    {"value", optional(h.value)}, {"reference_value", optional(h.referenceValue)},
                                    {"reason", h.reason}, {"price_evidence", labels.evidence(h.priceEvidence)},
                                    {"conversion_legs", legs}, {"provider_pnl", optional(h.providerPnl)},
                                    {"excluded_by_user", h.excludedByUser},
                                    {"provider_pnl_currency", optional(h.providerPnlCurrency)}});
        }
        const auto allocation = portfolioAllocationValue(group);
        groups.append(QJsonObject{{"identity", group.identity}, {"asset", group.asset},
                                  {"quantity", optional(portfolioQuantity(group))},
                                  {"fresh_total", fresh ? optional(group.freshTotal) : QJsonValue()},
                                  {"partial", fresh && partial},
                                  {"share", decimal::positive(allocation) ? portfolioShareText(allocation, allocationTotal) : QStringLiteral("—")}});
        if (unvalued) hideUnvalued.append(group.identity);
    }
    QJsonObject dust{{"threshold", optional(dustThreshold)}};
    if (decimal::positive(dustThreshold)) {
        const auto summary = portfolioDust(view, dustThreshold, true);
        QJsonArray hidden;
        for (const auto& identity : summary.hiddenIdentities) hidden.append(identity);
        dust.insert("hidden", hidden);
        dust.insert("hidden_total", summary.hidden);
    }
    QJsonArray issues;
    for (const auto& issue : c.valuation.issues) issues.append(issue);
    const QJsonObject result{
        {"valuation", QJsonObject{{"complete", c.valuation.complete}, {"net_assets", optional(c.valuation.netAssets)},
                                  {"known_subtotal", optional(c.valuation.knownSubtotal)}, {"issues", issues}}},
        {"screen", QJsonObject{{"label", c.totalLabel}, {"figure", c.totalFigure}, {"complete", view.summary.complete},
                               {"displayed_saved_view", c.displayedSavedView}, {"reference_total", optional(view.summary.referenceTotal)},
                               {"unvalued_groups", view.summary.unvaluedGroups}, {"reference_groups", view.summary.referenceGroups},
                               {"excluded_groups", view.summary.excludedGroups},
                               {"missing_accounts", int(view.summary.missingAccounts.size())}}},
        {"groups", groups}, {"rows", rows}, {"allocation_total", optional(allocationTotal)},
        {"dust", dust}, {"hide_unvalued", hideUnvalued}};
    QJsonArray raw;
    for (const auto& entry : responses) raw.append(response(entry));
    auto excluded = c.excludedAssets.values(); excluded.sort();
    QJsonArray providerTotals;
    for (const auto& comparison : c.providerReconciliations) {
        const auto& official = comparison.official;
        providerTotals.append(QJsonObject{{"connection_id", labels.connection(official.connectionId)},
            {"account_scope", labels.scope(official.accountScope)}, {"state", providerReconciliationLabel(comparison.state)},
            {"metric", official.metric}, {"currency", official.currency}, {"coverage", official.coverage},
            {"official_amount", optional(official.amount)}, {"keefi_amount", optional(comparison.keefiAmount)},
            {"difference", optional(comparison.difference)}, {"krw_difference", optional(comparison.krwDifference)},
            {"reason", comparison.reason}, {"official_evidence", labels.evidence(official.evidence)},
            {"configuration_evidence", labels.evidence(official.configurationEvidence)},
            {"fx_evidence", labels.evidence(comparison.fxEvidence)}});
    }
    return {
        {"format", "keefi-valuation-bundle/1"},
        {"screen_presentation", 2}, // Price visibility changed in Dev 84; valuation inputs did not.
        {"as_of_ms", double(c.asOfMs)},
        {"exported_at_ms", double(QDateTime::currentMSecsSinceEpoch())},
        {"exported_by", QJsonObject{{"application", application}, {"version", version}}},
        {"privacy", QStringLiteral("평문 파일 · 잔고·평가액 포함 · API 키·금고 암호·연결 바인딩 미포함 · "
                                   "계좌 범위와 연결 ID는 이 파일 안에서만 일관된 이름")},
        {"connections", connections}, {"snapshots", snapshots},
        {"excluded_assets", QJsonArray::fromStringList(excluded)},
        {"provider_reconciliations", providerTotals},
        {"quotes", quotes}, {"reference_quotes", references},
        {"keefi_result", result},
        {"raw_responses", QJsonObject{{"recording", recording}, {"entries", raw},
                                      {"kept", QStringLiteral("요청의 호스트·경로와 응답 본문만. 질의 문자열·헤더·요청 본문은 남기지 않음")}}}};
}
} // namespace keefi
