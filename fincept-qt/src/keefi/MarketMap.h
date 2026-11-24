#pragma once

#include <QList>
#include <QHash>
#include <QRectF>
#include <QWidget>

namespace keefi {

// One tile: label shown on the map, exact price/change strings for display,
// and a weight (market cap) that only sets the tile's area.
struct MarketMapTile {
    QString id, label, name, price, changePercent; // changePercent: "1.24" / "-0.86" (empty if unknown)
    QString group;   // sector shown in the drill-down heatmap
    bool operator==(const MarketMapTile&) const = default;
    double weight = 0; // market cap; sets the tile's area only
};

// One map section (코인, 한국 주식, 미국 주식). An unavailable section shows a
// reason and one action instead of tiles; it never shows invented prices.
struct MarketMapSection {
    QString key, title, status, source;
    QList<MarketMapTile> tiles;
    bool available = false;
    QString lockedTitle, lockedText, actionText;
    bool operator==(const MarketMapSection&) const = default;
    int overviewLimit = 12; // tiles shown when several sections share the map
};

// Squarified treemap of public quotes. Area = market cap,
// colour = previous-day change (rise green ▲, fall red ▼).
class MarketMapWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit MarketMapWidget(QWidget* parent = nullptr);
    void setSections(const QList<MarketMapSection>& sections);
    void setFilter(const QString& key); // "all" or a section key
    QString filter() const { return filter_; }
    QString selectedSection() const { return selectedSection_; }
    QString selectedId() const { return selectedId_; }
    void select(const QString& section, const QString& id);
    // Layout for tests and accessibility: tile rectangles in widget coordinates.
    QList<QPair<QString, QRectF>> tileRects() const;
    QRectF actionRect(const QString& section) const;
    void setEvidenceNumbers(const QHash<QString, QList<int>>& numbers);
    void setEvidenceStates(const QHash<QString, QString>& states);
    QSize sizeHint() const override { return {820, 560}; }
    QSize minimumSizeHint() const override { return {360, 320}; }

  signals:
    void tileSelected(const QString& section, const QString& id);
    void tileActivated(const QString& section, const QString& id); // Mouse or Enter/Space, not arrow selection.
    void actionRequested(const QString& section);
    void sectionRequested(const QString& section); // header click: drill in, or back to "all"

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

  private:
    struct Placed { QString section, id; QRectF rect; int index = -1; };
    struct SectionBox { int section = -1; QRectF box, map, action, header; };
    struct GroupBox { QString label; QRectF box; };
    void relayout();
    void refreshAccessibility();
    QList<MarketMapSection> sections_;
    QList<Placed> placed_;
    QList<SectionBox> boxes_;
    QList<GroupBox> groups_;
    QString filter_ = QStringLiteral("all");
    QString selectedSection_, selectedId_, hoverKey_;
    QHash<QString, QList<int>> evidenceNumbers_;
    QHash<QString, QString> evidenceStates_;
};

// Squarified layout (Bruls et al.): areas sorted descending, sum ≤ w*h.
QList<QRectF> squarifiedRects(const QList<double>& areas, QRectF box);

} // namespace keefi
