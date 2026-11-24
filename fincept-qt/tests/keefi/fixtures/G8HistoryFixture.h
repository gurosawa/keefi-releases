#pragma once

#include "keefi/PortfolioCheckpoint.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace keefi::test {
// Synthetic endpoint inputs shared with scripts/keefi-recon/check_g8_history.py.
struct G8HistoryFixture {
    QString name;
    QList<AssetSnapshot> history, currentSnapshots;
    QList<AssetQuote> quotes;
    CheckpointSelection selection;
    qint64 nowMs = 0;
    QJsonObject expected;
};

inline QList<G8HistoryFixture> loadG8HistoryFixtures(const QString& path, QString* error) {
    if (error) error->clear();
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return QList<G8HistoryFixture>{};
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail("Cannot open synthetic G8 fixture");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value("format").toString() != "keefi-g8-history-synthetic/1")
        return fail("Invalid synthetic G8 fixture format");
    const auto evidence = [](const QJsonObject& object) {
        return AssetEvidence{object.value("source").toString(), object.value("url").toString(),
            object.value("scope").toString(), object.value("source_time_ms").toInteger(),
            object.value("received_at_ms").toInteger(), object.value("complete").toBool()};
    };
    QList<G8HistoryFixture> fixtures;
    for (const auto& value : document.object().value("cases").toArray()) {
        const auto object = value.toObject();
        G8HistoryFixture fixture;
        fixture.name = object.value("name").toString();
        fixture.expected = object.value("expected").toObject();
        for (const auto* endpoint : {"opening", "closing"}) {
            const auto bundle = object.value(endpoint).toObject();
            if (bundle.value("format").toString() != "keefi-valuation-bundle/1")
                return fail("Invalid synthetic endpoint format");
            const bool closing = QLatin1StringView(endpoint) == QLatin1StringView("closing");
            if (closing) {
                fixture.nowMs = bundle.value("as_of_ms").toInteger();
                for (const auto& asset : bundle.value("excluded_assets").toArray())
                    fixture.selection.excludedAssets.insert(asset.toString());
            }
            for (const auto& item : bundle.value("snapshots").toArray()) {
                const auto source = item.toObject();
                AssetSnapshot snapshot;
                snapshot.id = source.value("id").toString();
                snapshot.accountScope = source.value("account_scope").toString();
                snapshot.connectionId = source.value("connection_id").toString();
                snapshot.connectionBinding = source.value("connection_binding").toString();
                snapshot.provider = source.value("provider").toString();
                snapshot.environment = source.value("environment").toString();
                snapshot.market = source.value("market").toString();
                snapshot.timeMs = source.value("time_ms").toInteger();
                snapshot.balanceComplete = source.value("balance_complete").toBool();
                snapshot.evidence = evidence(source.value("evidence").toObject());
                for (const auto& part : source.value("components").toArray()) {
                    const auto row = part.toObject();
                    AssetComponent component;
                    const auto kind = row.value("kind").toString();
                    if (kind == "Cash") component.kind = AssetComponentKind::Cash;
                    else if (kind == "Holding") component.kind = AssetComponentKind::Holding;
                    else if (kind == "Equity") component.kind = AssetComponentKind::Equity;
                    else if (kind == "PositionNotional") component.kind = AssetComponentKind::PositionNotional;
                    else return fail("Unsupported synthetic component kind");
                    component.id = row.value("id").toString();
                    component.asset = row.value("asset").toString();
                    component.quantity = row.value("quantity").toString();
                    component.valuationGroup = row.value("valuation_group").toString();
                    component.evidence = evidence(row.value("evidence").toObject());
                    snapshot.components.append(component);
                }
                fixture.history.append(snapshot);
                if (closing) {
                    fixture.currentSnapshots.append(snapshot);
                    fixture.selection.connections.append({snapshot.connectionId, snapshot.connectionBinding});
                }
            }
            for (const auto& item : bundle.value("quotes").toArray()) {
                const auto source = item.toObject();
                const AssetQuote quote{source.value("base").toString(), source.value("quote").toString(),
                    source.value("price").toString(), evidence(source.value("evidence").toObject())};
                if (!fixture.quotes.contains(quote)) fixture.quotes.append(quote);
            }
        }
        if (fixture.name.isEmpty() || fixture.expected.isEmpty() || fixture.currentSnapshots.isEmpty())
            return fail("Empty synthetic G8 case");
        fixtures.append(fixture);
    }
    if (fixtures.isEmpty()) return fail("No synthetic G8 cases");
    return fixtures;
}
} // namespace keefi::test
