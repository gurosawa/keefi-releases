#pragma once
#include "keefi/EarningsCalendar.h"
#include <QPointer>
#include <QSet>
#include <QWidget>
class QLabel;
class QVBoxLayout;
namespace keefi {
class InfoButton;
// Market flow section: this week's earnings for large or followed companies only.
class EarningsCalendarView final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int MaximumRows = 10;
    static constexpr double LargeCapUsd = 200e9;
    explicit EarningsCalendarView(EarningsCalendar*, QWidget* parent = nullptr);
    ~EarningsCalendarView() override;
    // Korean 6-digit codes (KOSPI market-cap top 50 and favourites) and US favourite tickers.
    void setInterest(const QSet<QString>& koreaCodes, const QSet<QString>& usTickers);
    void setBackgroundActive(bool); // The source keeps loading while the page is hidden.
    bool settled() const;           // Every source of the current cycle answered or failed.
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    void refresh();
    QPointer<EarningsCalendar> calendar_;
    QSet<QString> korea_, us_;
    bool backgroundActive_ = false;
    InfoButton* info_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
};
} // namespace keefi
