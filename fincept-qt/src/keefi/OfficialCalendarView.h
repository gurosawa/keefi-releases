#pragma once
#include "keefi/OfficialCalendar.h"
#include <QPointer>
#include <QWidget>
class QLabel;
class QVBoxLayout;
namespace keefi {
class InfoButton;
class OfficialCalendarView final : public QWidget {
    Q_OBJECT
  public:
    explicit OfficialCalendarView(OfficialCalendar*, QWidget* parent = nullptr);
    ~OfficialCalendarView() override;
    void setMarketFlowMode(bool);
    void setPrivateContext(bool,const QString&,const QString&);
    QString publicSummary() const { return publicSummary_; }
    void setBackgroundActive(bool); // The source keeps loading while the page is hidden.
    bool settled() const;           // Every source of the current cycle answered or failed.
  signals:
    void publicSummaryChanged(QString summary);
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    void refresh();
    QPointer<OfficialCalendar> calendar_;
    bool marketFlowMode_ = false, backgroundActive_ = false;
    QString publicSummary_;
    QLabel* privateContext_ = nullptr;
    QLabel* status_ = nullptr;
    InfoButton* info_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
};
}
