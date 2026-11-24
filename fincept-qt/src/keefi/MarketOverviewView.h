#pragma once
#include "keefi/MarketOverview.h"
#include <QPointer>
#include <QWidget>
class QComboBox;
class QLabel;
namespace keefi {
class ExploreMetricCard;
class ExploreEvidenceFooter;
class MarketOverviewView final : public QWidget {
    Q_OBJECT
  public:
    explicit MarketOverviewView(MarketOverview*,QWidget* parent=nullptr);
    ~MarketOverviewView() override;
    void setMarketFlowMode(bool);
    void setMode(MarketTimeMode);
    MarketTimeMode mode() const;
    void setEvidenceStart(int);
    int evidenceCount() const;
    void setBackgroundActive(bool); // The source keeps loading while the page is hidden.
    bool settled() const;           // Every source of the current cycle answered or failed.
  signals:
    void evidenceChanged();
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    void refresh();
    void refreshEvidence();
    ExploreEvidenceFooter* evidence_=nullptr;
    int evidenceStart_=1;
    QPointer<MarketOverview> overview_;
    bool marketFlowMode_=false,backgroundActive_=false;
    QWidget* flowLine_=nullptr;
    QLabel* flowValue_=nullptr,*flowDate_=nullptr;
    QComboBox* mode_;
    ExploreMetricCard* treasury_;
};
} // namespace keefi
