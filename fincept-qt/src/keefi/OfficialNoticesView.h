#pragma once
#include "keefi/OfficialNotices.h"
#include <QPointer>
#include <QWidget>
class QLabel;
class QVBoxLayout;
namespace keefi {
class OfficialNoticesView final : public QWidget {
    Q_OBJECT
  public:
    explicit OfficialNoticesView(OfficialNotices*, QWidget* parent = nullptr);
    ~OfficialNoticesView() override;
    void setMarketFlowMode(bool);
    void refresh();
    void setBackgroundActive(bool); // The source keeps loading while the page is hidden.
    bool settled() const;           // Every source of the current cycle answered or failed.
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    bool marketFlowMode_ = false, backgroundActive_ = false;
    QPointer<OfficialNotices> notices_;
    QLabel* status_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
};
}
