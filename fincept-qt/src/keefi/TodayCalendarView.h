#pragma once
#include "keefi/EarningsCalendar.h"
#include "keefi/OfficialCalendar.h"
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <functional>

class QLabel;
class QPushButton;
class QVBoxLayout;
namespace keefi {
class InfoButton;
// A bounded public presentation of the existing economic and earnings feeds.
// Source activity belongs to LocalWindow; this view never starts extra requests.
class TodayCalendarView final : public QWidget {
    Q_OBJECT
  public:
    using Clock = std::function<qint64()>;
    explicit TodayCalendarView(QWidget* parent = nullptr, Clock clock = {});
    void setCalendar(OfficialCalendar*);
    void setEarnings(EarningsCalendar*);
    void setInterest(const QSet<QString>& koreaSixDigitCodes, const QSet<QString>& usTickers);
  protected:
    void resizeEvent(QResizeEvent*) override;
  private:
    void queueRefresh();
    void refresh();
    QPointer<OfficialCalendar> calendar_;
    QPointer<EarningsCalendar> earnings_;
    Clock clock_;
    QSet<QString> korea_, us_;
    QString filter_ = QStringLiteral("all");
    int page_ = 0;
    bool refreshQueued_ = false;
    bool refreshing_ = false;
    QVBoxLayout* rows_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    InfoButton* info_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
};
} // namespace keefi
