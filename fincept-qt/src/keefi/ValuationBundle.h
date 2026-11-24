#pragma once
#include "keefi/PortfolioPresentation.h"
#include "keefi/ResponseJournal.h"
#include "keefi/ProviderReconciliation.h"
#include <QJsonObject>

namespace keefi {
// The exact inputs and outputs of one on-screen valuation.
struct ValuationCapture {
    qint64 asOfMs = 0;
    QList<PortfolioConnection> connections;
    QList<AssetSnapshot> snapshots;
    QList<AssetQuote> quotes, references;
    AssetValuationResult valuation;
    PortfolioPresentation view;   // Current evidence, unfiltered.
    QString totalLabel, totalFigure; // As drawn in the headline.
    bool displayedSavedView = false;
    QSet<QString> excludedAssets;
    QList<ProviderReconciliationResult> providerReconciliations;
};

// keefi-valuation-bundle/1 for scripts/keefi-recon. The file is plain text and
// holds balances and values; it never holds credentials or connection
// bindings. Account scopes and connection IDs become labels that stay
// consistent inside one file only.
QJsonObject valuationBundle(const ValuationCapture& capture, const QString& dustThreshold,
                            const QList<JournalEntry>& responses, bool recording,
                            const QString& application, const QString& version);
} // namespace keefi
