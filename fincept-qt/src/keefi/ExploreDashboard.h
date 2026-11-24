#pragma once
#include "keefi/ExploreMetricCard.h"
#include "keefi/PortfolioHistory.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/OfficialNotices.h"
#include "keefi/WorldMarkets.h"
#include <QWidget>
#include <QHash>
#include <QPointer>
class QLabel;
class QPushButton;
class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
class QProgressBar;
namespace keefi {
class InfoButton;
class DashboardLine;
class DashboardMetricLine;
class TodayCalendarView;

// One aligned ledger line of the Today board: name, state mark, value, then a
// coloured change or a grey basis note. Public display data only.
// freshnessText "마감" draws a closed-market dot; "지연" marks a late value.
class TodayLedgerRow final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int RowHeight = 28;
    static constexpr int LedgerSpacing = 4;
    explicit TodayLedgerRow(QWidget* parent=nullptr);
    void setData(const ExploreMetricCardData&);
    ExploreMetricCardData data() const { return data_; }
    void setTrailWidth(int);
  protected:
    void showEvent(QShowEvent*) override;
  private:
    void ink();
    ExploreMetricCardData data_;
    QLabel *title_, *state_, *value_, *trail_;
    QPointer<QObject> policy_; // The bound MovementColorPolicy, re-inked on its changes.
};

// Pure projection of the keyless world snapshot into Today rows: the six indices,
// 원/달러·원/100엔·원/위안, 달러/엔·호주달러/엔 (Hana cross), the ECB dollar index,
// 금·은·WTI and the Japan 10-year yield ("jgb"). Never invents a missing value.
QList<QPair<QString,ExploreMetricCardData>> todayWorldMarketMetrics(const WorldMarketsSnapshot&, qint64 nowMs);

// Display projections only. The window retains all source subscriptions and privacy gates.
class ExploreTodayView final : public QWidget {
    Q_OBJECT
  public:
    explicit ExploreTodayView(QWidget* parent=nullptr);
    ~ExploreTodayView() override;
    TodayCalendarView* calendar() const { return calendar_; }
    // The caller adds/reparents this header into its surrounding Explore layout.
    // Without this call, the standalone view keeps the header above its metrics.
    QWidget* takeSummaryHeader();
    void setWideLayout(bool); // The owning window supplies its 1680px breakpoint.
    void setMetric(const QString& key, const ExploreMetricCardData&);
    void setHeadline(const QString&);
    void setPrivateSummary(bool allowed, const QString& total, const QString& unit,
                           const PortfolioDayHistory&, const PortfolioMoneyDisplay& = {});
    void clearPrivate();
    void setNoticeFeeds(const QList<OfficialNoticeFeed>&);
  signals:
    void vaultOpenRequested();
    void alertSettingsRequested();
  protected:
    void resizeEvent(QResizeEvent*) override;
  private:
    void layoutMetrics();
    void layoutSummary();
    void renderNotices();
    QPointer<QWidget> top_;
    QWidget *summaryCards_, *emptyStrip_, *emptyPrivate_, *emptyNotice_, *scheduleRow_;
    QWidget *privatePanel_, *privateBody_, *contributionPanel_, *noticePanel_, *metricPanel_;
    QHBoxLayout* privateHeading_;
    bool wideLayout_=false;
    QGridLayout* metrics_;
    QLabel *placeholder_, *total_, *unit_, *change_, *headline_, *contributionBasis_;
    QPushButton* unlock_;
    InfoButton* privateInfo_;
    DashboardLine* privateLine_;
    QWidget* gapStrip_;
    DashboardMetricLine* gapLine_;
    TodayCalendarView* calendar_;
    QHash<QString,TodayLedgerRow*> rows_;
    QList<QWidget*> groups_; // 김프·금리, 세계 지수, 환율, 원자재: fixed heights, top aligned.
    QVBoxLayout* indexRows_=nullptr;
    QString mode_="morning";
    int metricColumns_=0;
    QList<QLabel*> contributionLabels_;
    QList<QProgressBar*> contributionBars_;
    QList<QPair<QString,OfficialNotice>> notices_;
    QVBoxLayout* noticeRows_;
    QLabel* noticeStatus_;
    QPushButton* noticeMore_;
    int noticePage_=0;
    bool noticeRefreshInPlace_=false;
    QString noticeSignature_;
};

class ExploreCoinSummary final : public QWidget {
    Q_OBJECT
  public:
    explicit ExploreCoinSummary(QWidget* parent=nullptr);
    void setCoins(const QStringList&, const QString& selected);
    void setQuote(const ExploreMetricCardData&);
    void setMetrics(const ExploreMetricCardData& gap,const ExploreMetricCardData& coinbase,
                    const ExploreMetricCardData& funding,const QString& flow);
    void setPrivateHolding(bool allowed,const QString& text);
    void setRelativeStrength(const QString& text,const QString& information);
    void clearPrivate();
  signals:
    void coinSelected(const QString&);
    void chartRequested();
  private:
    QList<QPushButton*> tabs_;
    ExploreMetricCard* quote_;
    QLabel *holding_,*marketSupport_;
    QWidget* holdingRow_;
    InfoButton* holdingInfo_;
    QWidget* relativeRow_;
    QLabel* relative_;
    InfoButton* relativeInfo_;
};
}
