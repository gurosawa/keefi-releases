#pragma once
#include <QDialog>
#include <QStringList>
#include <memory>

namespace keefi {
class PublicMarketCatalog;
class PublicEquityBoard;
class ChartInstrumentCatalog;
struct ChartInstrumentCandidate {
    QString key, market, symbol, name, venue;
    bool operator==(const ChartInstrumentCandidate&) const = default;
};
// Display membership never authorizes broker requests or alert input.
QList<ChartInstrumentCandidate> chartInstrumentCandidates(PublicMarketCatalog*, PublicEquityBoard*, ChartInstrumentCatalog*);
QString chartInstrumentSearchAlias(const QString& symbol);

// Public catalogue browser only. Opening and storing an identity are handled
// by the workspace, which retains broker and chart-routing authorization.
class ChartInstrumentPicker final : public QDialog {
    Q_OBJECT
  public:
    explicit ChartInstrumentPicker(PublicMarketCatalog*, PublicEquityBoard*, ChartInstrumentCatalog*, QWidget* parent = nullptr);
    ~ChartInstrumentPicker() override;
    void setQuery(const QString&);
    void setFeedback(const QString&);
    QStringList representativeIdentities() const;
  signals:
    void openRequested(const QString& identity);
    void addRequested(const QString& identity);
    void addRepresentativesRequested(const QStringList& identities);
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
