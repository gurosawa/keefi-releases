#pragma once

#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/ExploreMetricCard.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QScrollArea;
class QSplitter;
class QVBoxLayout;

namespace keefi {

struct PublicDetailIndicator { QString name,value;MovementDirection direction=MovementDirection::Unknown; };

// Display-only provenance for the existing public Upbit ticker. Preserve both
// original timestamps; freshness is independent of a complete price payload.
// A caller can share one observation clock across a screen evidence book.
ExploreMetricEvidence publicMarketQuoteEvidence(const PublicMarketRow&, qint64 nowMs);

// Selected-asset detail. Shows the values and their basis as plain text in the
// body (not only tooltips): quantity, KRW valuation state, balance time and
// the price source with its original and received times.
class AssetDetailPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit AssetDetailPanel(QWidget* parent = nullptr);
    void showGroup(const PortfolioGroupView& group, const QString& allocationTotal = {}, const PortfolioMoneyDisplay& display = {}, bool showUnvalued = true);
    void showMissing(const PortfolioMissingView& missing);
    // Public market detail. personalReadable=false shows no personal data at all;
    // holdings may be null when nothing linked by ticker was confirmed.
    void showMarket(const PublicMarketRow& row, bool personalReadable, const PortfolioGroupView* holdings, bool coverageIncomplete);
    // A read-only quote from another source (e.g. a broker's quote API): figure,
    // change, short rows and folded evidence, in the same card layout.
    void showQuote(const QString& name, const QString& code, const QString& price, const QString& changePercent,
                   const QList<QPair<QString, QString>>& rows, const QStringList& evidence, const QString& changeBasis = {}, const QString& currency = {}, const QString& sourceKey = {}, bool previousQuote = false);
    void enableExplorationControls(); // Opt-in: never appears in the private holdings tab.
    void enableAssetShareControl(); // Explicit opt-in for the My Assets detail only.
    void setPublicSelection(const QString& section, const QString& id, bool favourite);
    void setPublicEvidence(const QList<ExploreMetricEvidence>&);
    void setValuationEvidenceNumbers(const QList<int>&);
    void setPublicHistory(const ExploreMetricHistory&);
    void setPublicHistoryContext(const QString& sourceKey, const QString& unit);
    void setPrivateLinkage(bool allowed, const QString& information);
    void setPublicIndicators(const QList<PublicDetailIndicator>&); // Up to three existing public study values; absent slots fold.
    void setChartIdentity(const QString& verifiedIdentity);
    void clearDetail(); // Also used on lock: drops every personal string.
    bool hasSelection() const { return selected_; }

  signals:
    void backRequested();
    void connectionsRequested();
    void favouriteRequested(const QString& section, const QString& id);
    void valuationExclusionRequested(const QString& asset, bool excluded);
    void publicChartRequested(const QString& identity);
    void assetShareRequested(const QString& portfolioIdentity);

  private:
    // Ledger: the text head and rows. PublicCard: the public card alone. PublicFacts: the card and its fact rows.
    enum class BodyMode { Ledger, PublicCard, PublicFacts };
    // headline/sub are the card's large figure; places are short "where" rows;
    // blocks are the full evidence, folded behind the 근거 toggle.
    void setBody(const QString& title, const QString& subtitle, const QString& headline, const QString& headlineSub,
                 const QString& subTone, const QString& totals, const QList<QPair<QString, QString>>& places, const QStringList& blocks,
                 BodyMode mode = BodyMode::Ledger);
    void settleLayout();
    void applyEvidenceFold();
    void showPublicCard(const ExploreMetricCardData&);
    QPushButton* back_ = nullptr;
    QLabel* references_ = nullptr;
    ExploreEvidenceFooter* evidenceFooter_ = nullptr;
    void setReferenceNumbers(const QList<int>&);
    QLabel* headline_ = nullptr;
    QLabel* headlineSub_ = nullptr;
    QWidget* places_ = nullptr;
    QVBoxLayout* placesLayout_ = nullptr;
    QPushButton* evidence_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* totals_ = nullptr;
    QWidget* blocks_ = nullptr;
    QVBoxLayout* blockLayout_ = nullptr;
    QPushButton* connections_ = nullptr;
    QLabel* empty_ = nullptr;
    bool selected_ = false;
    QPushButton* favourite_ = nullptr;
    QString publicSection_, publicId_;
    QPushButton* valuationExclusion_ = nullptr;
    QString selectedValuationAsset_;
    bool valuationExcluded_ = false;
    ExploreMetricCard* publicCard_ = nullptr;
    QWidget* indicatorSlots_ = nullptr;
    QList<QLabel*> indicators_;
    QPushButton* chartLink_ = nullptr;
    QString chartIdentity_;
    QPushButton* assetShare_ = nullptr;
    QString assetShareIdentity_;
    bool publicPresentation_ = false;
    bool publicFacts_ = false; // Only showQuote fact rows may stay visible under the public card.
    bool preservePublicInformation_ = false, privateLinkageAllowed_ = false;
    friend class ListDetailView;
};

// Wide: list and detail side by side. Narrow: one at a time; the list keeps
// its search, scroll position and selection while the detail is shown.
class ListDetailView final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int NarrowWidth = 1000;
    // Wide screens give the detail about 30% of the width, between 340 and 520px.
    static constexpr int DetailMinimumWidth = 340, DetailMaximumWidth = 520;
    static int detailWidth(int width) { return qBound(DetailMinimumWidth, width * 3 / 10, DetailMaximumWidth); }
    ListDetailView(QWidget* list, AssetDetailPanel* detail, QWidget* parent = nullptr);
    bool narrow() const { return narrow_; }
    void showDetail();
    void showList();
    // Wide layouts normally keep the detail beside the list. With this set, the
    // detail appears only while it shows a selection.
    void setDetailOnlyWithSelection(bool only) { detailOnlyWithSelection_ = only; applyMode(); }
    void refreshMode() { applyMode(); }

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void applyMode();
    void rememberDetailShare();
    void restoreDetailShare();
    QSplitter* splitter_ = nullptr;
    QPointer<QWidget> list_;
    QPointer<AssetDetailPanel> detail_;
    bool narrow_ = false, detailShown_ = false, sized_ = false, detailOnlyWithSelection_ = false;
    double detailShare_ = 0.4; // the first wide layout's 60:40
};

} // namespace keefi
