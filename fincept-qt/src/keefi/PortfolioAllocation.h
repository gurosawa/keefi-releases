#pragma once
#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioMoneyDisplay.h"
#include <QStringList>
#include <QWidget>

class QHideEvent;
class QVariantAnimation;

namespace keefi {
class PortfolioAllocation final : public QWidget {
  public:
    explicit PortfolioAllocation(QWidget* parent = nullptr);
    void setGroups(const QList<PortfolioGroupView>& groups, const PortfolioMoneyDisplay& display = {});
    const QString& total() const { return total_; }
  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    void updateHeight();
    struct Slice { QString asset, value, avatarKey, share; };
    QList<Slice> slices_;
    QString total_;
    QStringList composition_; // Sorted asset keys of the last call; a change replays the reveal.
    qreal revealProgress_ = 1.0;
    QVariantAnimation* revealMotion_ = nullptr;
};
} // namespace keefi
