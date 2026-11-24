#pragma once

#include <QFrame>
#include <QHash>
#include <QList>
#include <QSet>

class QLabel;
class QPushButton;
class QScrollArea;
class QComboBox;

namespace keefi {

// One Spotlight row: text symbol, one quiet sub-line, price and change.
// Display strings only; nothing here feeds valuation or orders.
struct SpotlightItem {
    QString id, symbol, sub, price, changePercent; // changePercent: "1.24" / "-0.86" (empty if unknown)
    QString display; // Optional Korean display name; the symbol then shows as the code.
    QList<double> spark; // Optional recent hourly prices, oldest first. Display only; empty draws no line.
    QString observation, observationInformation; // Optional public, completed-minute observation.
    QString study, studyInformation; // Optional selected ranking field; independent candle freshness.
    QString venue;
    qint64 sourceTimeMs=0, receivedAtMs=0;
    bool operator==(const SpotlightItem&) const = default;
};

// One independent ranking inside a market panel (market cap, gainers, and so on).
struct SpotlightMode {
    QString key, label;
    QList<SpotlightItem> items;
    bool operator==(const SpotlightMode&) const = default;
};

class SpotlightRows;
class SpotlightDots;

// Ranking pages keep their boundaries as the viewport resizes. Each page scrolls independently.
class SpotlightCard final : public QFrame {
    Q_OBJECT
  public:
    static constexpr int PageSize = 5, FirstPageRows = 19, LastPageRows = 20, MaxRows = FirstPageRows + LastPageRows, RowHeight = 44;
    // Side-by-side cards need this width so the 22px price is never cut (compact row metrics).
    static constexpr int ColumnWidth = 400;
    SpotlightCard(const QString& key, const QString& title, QWidget* parent = nullptr);
    QString key() const { return key_; }
    QString coinVenue() const;
    void setModes(const QList<SpotlightMode>& modes); // keeps the chosen mode and page when it still exists
    void setMode(const QString& key);
    QString mode() const { return mode_; }
    void setPage(int page);
    int page() const { return page_; }
    int pageCount() const;
    // Visible row bands in the scroll viewport, independent of logical rank boundaries.
    void setRowsPerPage(int rows);
    int rowsPerPage() const { return rowsPerPage_; }
    int chromeHeight() const; // Everything but the row bands: title, page marks, padding, margins and the top rule.
    QList<SpotlightItem> visibleItems() const; // All ranked items on this logical page.
    QList<SpotlightItem> viewportItems() const;
    void setSelected(const QString& id);
    void setFavourites(const QSet<QString>& ids);
    // Numbers are allocated by the containing public screen, never by a card.
    void setEvidenceNumbers(const QHash<QString, QList<int>>& numbers);
    void setEvidenceStates(const QHash<QString, QString>& states);
    void setEvidenceSources(const QHash<QString, QString>& sources);
    void setStatus(const QString& status); // Market session text such as "장중"; empty hides it.
    void setStudyValues(const QHash<QString,QPair<QString,QString>>&);

  signals:
    void itemSelected(const QString& section, const QString& id);
    void openRequested(const QString& section);
    void favouriteToggled(const QString& section, const QString& id);
    void evidenceOrderChanged();
    void coinVenueChanged();
    void modeChanged();

  private:
    void render();
    const SpotlightMode* current() const;
    int firstRank() const { return page_==0?1:FirstPageRows+1; }
    QString key_;
    QList<SpotlightMode> modes_;
    QString mode_;
    int page_ = 0, rowsPerPage_ = PageSize;
    QComboBox* coinVenueChoice_ = nullptr;
    QPushButton* modeButton_ = nullptr;
    QLabel *status_ = nullptr, *range_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QScrollArea* rowScroll_=nullptr;
    SpotlightRows* rows_ = nullptr;
    SpotlightDots* dots_ = nullptr;
    QHash<QString,QPair<QString,QString>> studyValues_;
};

} // namespace keefi
