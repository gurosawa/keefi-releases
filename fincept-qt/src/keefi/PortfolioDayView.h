#pragma once
#include "keefi/PortfolioHistory.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/CoinPerformance.h"
#include <QWidget>
class QLabel;
class QPushButton;
namespace keefi {
class PortfolioDayChart;
class PortfolioContributionBars;
class InfoButton;
// A view of observed checkpoints; owns no account or ledger objects.
class PortfolioDayView final : public QWidget {
    Q_OBJECT
  public:
    explicit PortfolioDayView(QWidget* parent = nullptr);
    void setHistory(const PortfolioDayHistory&, const PortfolioMoneyDisplay& display = {}, bool refreshing = false);
    // References are allocated by the enclosing screen's evidence book, never by this view.
    void setEvidenceReferences(const QString&);
    // G9-C1-a: the separate coin investment PnL line under the observed change.
    void setCoinPerformance(const CoinPerformanceView&, const PortfolioMoneyDisplay& display = {});
    void clear(const QString& reason = {});
    // The chart and contribution bars open on request under the one-line change.
    void setDetailsExpanded(bool expanded);
    bool detailsExpanded() const { return expanded_; }
    bool refreshPending() const { return refreshPending_; }
  signals:
    void detailsExpandedChanged(bool expanded);
  private:
    void refreshDetailsToggle();
    QLabel *title_, *change_, *baseline_, *contributions_, *reference_, *currencyStatus_;
    QLabel *coinTitle_, *coinValue_, *coinNote_;
    PortfolioDayChart* chart_;
    PortfolioContributionBars* bars_;
    InfoButton* info_;
    QWidget* details_ = nullptr;
    QPushButton* toggle_ = nullptr;
    bool expanded_ = false;
    bool comparisonAvailable_ = false;
    bool refreshPending_ = false;
    bool hasSettledHistory_ = false;
};
} // namespace keefi
