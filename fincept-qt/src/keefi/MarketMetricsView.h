#pragma once
#include "keefi/MarketMetrics.h"
#include "keefi/MarketFlowPresentation.h"
#include <QHash>
#include <QPointer>
#include <QWidget>
class QLabel;
class QPushButton;
class QCheckBox;
class QGridLayout;
class QResizeEvent;
namespace keefi {
class PublicQuoteStream;
class CoinbaseQuotes;
class ExploreMetricCard;
class ExploreEvidenceFooter;
// Dedicated exploration streams; no portfolio subscriptions or private data.
class MarketMetricsView final : public QWidget {
    Q_OBJECT
  public:
    // Whether the selected coin trades where 김프 is read; Missing ends the wait with a reason.
    enum class Listing { Unknown, Listed, Missing };
    static constexpr qint64 LateMs = 30000; // A source still silent after this counts as late, not loading.
    explicit MarketMetricsView(MarketMetrics*, QWidget* parent = nullptr);
    MarketMetricsView(MarketMetrics*, PublicQuoteStream* upbit, PublicQuoteStream* binance, QWidget* parent = nullptr);
    ~MarketMetricsView() override;
    void setAsset(const QString& asset);
    void setCoinbaseQuotes(CoinbaseQuotes*);
    void setPrivateContext(bool unlocked, bool hasFutures);
    void setPreviousGap(const MarketFlowHistoricalGap&);
    void setMetricAlertEnabled(MarketFlowMetric, bool enabled);
    void setAlertDemandActive(bool active);
    void setBackgroundActive(bool active); // Public streams and reads keep running while the page is hidden.
    void setListing(Listing upbitKrw, Listing binanceSpot);
    // Answered items for the selected coin (김프, 펀딩비, 미결제약정, 코인베이스 가격차) and their count.
    // Received, unlisted, failed and late items are answered.
    QPair<int, int> progress() const;
    void setCalendarSummary(const QString&);
    // A dashboard mirror consumes the existing validated display projection while this view is hidden.
    void setMirroredPresentation(bool enabled) { if (mirroredPresentation_ != enabled) { mirroredPresentation_=enabled;refresh(); } }
    QString asset() const { return asset_; }
    void setEvidenceStart(int);
    int evidenceCount() const;
  signals:
    void evidenceChanged();
    void metricAlertToggled(keefi::MarketFlowMetric metric, bool enabled);
    void snapshotReady(QString asset,keefi::ExchangePriceGap koreaGap,keefi::ExchangePriceGap coinbaseGap,keefi::PublicDerivativesMetrics derivatives);
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
  private:
    void updateSubscriptions();
    void refresh();
    void refreshEvidence();
    struct MetricPresentation {
        QWidget* row = nullptr;
        QLabel* state = nullptr;
        QLabel* meaning = nullptr;
        QCheckBox* alert = nullptr;
    };
    MetricPresentation& presentation(MarketFlowMetric);
    MetricPresentation gapPresentation_, fundingPresentation_, coinbasePresentation_, oiPresentation_;
    ExploreEvidenceFooter* evidence_=nullptr;
    int evidenceStart_=1;
    void layoutCards();
    void syncDerivativesHeader();
    void fitSummary();
    QPointer<MarketMetrics> metrics_;
    QPointer<CoinbaseQuotes> coinbase_;
    QPointer<PublicQuoteStream> upbit_, binance_;
    bool streamsConnected_ = false;
    bool mirroredPresentation_ = false;
    bool koreaGapDemandEnabled_ = true;
    bool contextUnlocked_ = false, contextHasFutures_ = false;
    bool alertDemandActive_ = false, backgroundActive_ = false, coinbaseDemandQueued_ = false;
    Listing upbitListing_ = Listing::Unknown, binanceListing_ = Listing::Unknown;
    bool coinbaseMissing_ = false;
    qint64 activeSinceMs_ = 0;
    bool streamsActive() const;
    MarketFlowHistoricalGap previousGap_;
    QString calendarSummary_;
    QString asset_;
    QHash<QString, AssetQuote> quotes_;
    QLabel* title_=nullptr;
    QLabel* summary_=nullptr;
    QPushButton* derivativesToggle_=nullptr;
    QWidget* derivatives_=nullptr;
    QLabel *derivativesNote_=nullptr,*derivativesSummary_=nullptr;
    QWidget *cardsRule_=nullptr,*separators_[2]={nullptr,nullptr};
    ExploreMetricCard *gap_=nullptr,*coinbaseGap_=nullptr,*funding_=nullptr,*oi_=nullptr;
    QGridLayout* cards_=nullptr;
    int columns_=0;
};
} // namespace keefi
