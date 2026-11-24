#pragma once
#include "keefi/MovementDirection.h"
#include <QList>
#include <QWidget>
#include <memory>
#include <optional>

namespace keefi {
class InfoButton;
// Presentation-only provenance. id is used for deduplication and is never rendered.
// ExploreMetricCard projects provider and state into constrained display labels.
struct ExploreMetricEvidence {
    QString id, provider, sourceDate;
    qint64 sourceTimeMs = 0, receivedAtMs = 0;
    bool complete = false;
    QString state;
    bool operator==(const ExploreMetricEvidence&) const = default;
};
class ExploreEvidenceFooter final : public QWidget {
    Q_OBJECT
  public:
    explicit ExploreEvidenceFooter(QWidget* parent = nullptr);
    void setEntries(const QList<ExploreMetricEvidence>&, int start = 1);
    int evidenceCount() const;
    QList<int> numbers() const;
    QList<ExploreMetricEvidence> entries() const;
  private:
    QList<ExploreMetricEvidence> entries_;
    QList<int> numbers_;
    int count_ = 0, start_ = 1;
    void refresh();
};
struct ExploreHistoryPoint {
    qint64 atMs = 0;
    double value = 0;
    bool connectsPrevious = false;
    bool operator==(const ExploreHistoryPoint&) const = default;
};
struct ExploreMetricHistory {
    bool available = false;
    QString sourceKey, unit, reason;
    QList<ExploreHistoryPoint> points;
    bool operator==(const ExploreMetricHistory&) const = default;
};
// Public market display values only. Account/day values remain in their owning private view.
struct BreadthCounts {
    int up=0, down=0, flat=0;
    bool operator==(const BreadthCounts&) const = default;
};
struct ExploreMetricCardData {
    QString id, title, value, unit, changeText, freshnessText, sourceKey, publicInformation;
    MovementDirection direction = MovementDirection::Unknown;
    ExploreMetricHistory history;
    QList<ExploreMetricEvidence> evidence;
    std::optional<BreadthCounts> breadth;
    bool operator==(const ExploreMetricCardData&) const = default;
};
class ExploreMetricCard final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int MaxHistoryPoints = 3000;
    // Row: the default one-line ledger row. Hero: a large value with a wide
    // history line (market flow). Cell: a stacked compact value (grid cells).
    // Summary: a 22px value with a 64px history line below it.
    enum class Presentation { Row, Hero, Cell, Summary };
    explicit ExploreMetricCard(QWidget* parent = nullptr);
    ~ExploreMetricCard() override;
    void setPresentation(Presentation);
    Presentation presentation() const;
    // Stacked presentations only. Borrowed widgets are reparented into the card's own lines.
    void setHeaderAction(QWidget*);
    void setHeaderDetail(QWidget*); // Optional context directly below the title.
    void setInlineState(QWidget*);
    void setData(const ExploreMetricCardData&);
    ExploreMetricCardData data() const;
    void setEvidenceNumbers(const QList<int>&);
    void clear();
    InfoButton* informationButton() const;
    void setPrivateAccess(bool);
    bool setPrivateInformation(const QString&);
    void clearPrivate(); // Public market data remains; only transient linkage information clears.
    bool historyVisible() const;
    QList<ExploreHistoryPoint> plottedHistory() const;
  protected:
    void showEvent(QShowEvent*) override;
    void resizeEvent(QResizeEvent*) override;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
