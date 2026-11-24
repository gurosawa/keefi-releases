#pragma once

#include <QFrame>
#include <QHash>
#include <QPointF>
#include <QStringList>
#include <array>

class QLabel;
class QHBoxLayout;
class QTableWidget;
class QTabBar;

namespace keefi {

struct WatchlistQuote {
    QString section, id, name, price, change, cap, volume, evidence;
    double priceValue = 0, capValue = 0, volumeValue = 0;
    QList<QPointF> history;
    QString evidenceProvider, evidenceState;
    qint64 evidenceSourceTimeMs = 0;
    bool operator==(const WatchlistQuote&) const = default;
};

// Public favourites only. Each market has its own ordered list in this app's
// profile settings; vault state and account identifiers never enter the store.
class ExploreWatchlist final : public QFrame {
    Q_OBJECT
  public:
    explicit ExploreWatchlist(QWidget* parent = nullptr);
    QStringList ids(const QString& section) const;
    bool contains(const QString& section, const QString& id) const;
    void toggle(const QString& section, const QString& id);
    void setSection(const QString& section);
    QString section() const;
    void setQuotes(const QList<WatchlistQuote>& quotes);
    QStringList visibleIds() const;
    void setEvidenceNumbers(const QHash<QString, QList<int>>& quotes, const QHash<QString, QList<int>>& histories = {});
    // A narrow column keeps name, price and change; trend, market cap and volume fold away.
    void setCompact(bool compact);
    bool compact() const { return compact_; }
    // Today's dashboard uses single-line rows; ranking/map keep their existing rail.
    void setDashboardLayout(bool dashboard);

  signals:
    void favouritesChanged();
    void sectionChanged();
    void itemSelected(const QString& section, const QString& id);
    void visibilityChanged();
    void evidenceOrderChanged();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void render();
    void applyPresentation();
    void layoutColumns();
    void applyEvidenceNumbers();
    bool deferHiddenPresentation();
    QHash<QString, QStringList> lists_;
    QHash<QString, WatchlistQuote> quotes_;
    QHash<QString, QList<int>> quoteEvidenceNumbers_, historyEvidenceNumbers_;
    QHBoxLayout* head_ = nullptr;
    QTabBar* tabs_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* empty_ = nullptr;
    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    bool presentationStarted_ = false;
    bool changingSection_ = false;
    bool renderPending_ = false, hasHistory_ = false, compact_ = false;
    bool dashboardLayout_ = true;
    bool columnMeasurementsDirty_ = true;
    std::array<int, 7> measuredColumns_{};
};

} // namespace keefi
