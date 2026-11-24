#pragma once

#include <QProgressBar>

class QVariantAnimation;

namespace keefi {

// Task progress for a first read. value() and maximum() stay the real finished
// and total step counts, which is also what a screen reader is told. Only the
// drawing moves:
//  - the solid fill follows the finished share on an OutExpo curve; it may lag
//    that share for a moment but never passes it;
//  - a paler tail marks the step still in flight. It grows toward, and never
//    reaches, 80% of that one step's slot, so it cannot be read as a finished step.
class LoadProgressBar final : public QProgressBar {
    Q_OBJECT
  public:
    static constexpr int FollowMs = 480;
    static constexpr int TailTimeConstantMs = 1800;
    static constexpr qreal TailReach = 0.8; // share of one step's slot

    explicit LoadProgressBar(QWidget* parent = nullptr);
    void setSteps(int finished, int total, bool inFlight);
    void resetSteps();

    qreal finishedShare() const; // value() / maximum()
    qreal shownShare() const;    // solid fill, never above finishedShare()
    qreal tailShare() const;     // pale tail beyond the finished share
    int shownPercent() const;    // percent of the solid fill, never above the finished steps
    bool inFlight() const { return inFlight_; }

  signals:
    void shownChanged();

  protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    void restartTail();
    qreal shown_ = 0, tail_ = 0;
    bool inFlight_ = false;
    QVariantAnimation* follow_ = nullptr;
    QVariantAnimation* tailMotion_ = nullptr;
};

} // namespace keefi
