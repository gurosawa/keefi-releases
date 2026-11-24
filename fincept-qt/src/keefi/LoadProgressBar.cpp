#include "keefi/LoadProgressBar.h"

#include "keefi/KeefiTheme.h"
#include "keefi/Motion.h"

#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <cmath>

namespace keefi {
namespace {
constexpr qreal TailSpan = 5; // the tail runs for five time constants, then rests
qreal tailCurve(qreal progress) {
    // 1 - e^(-t/tau), scaled so the last frame is exactly the resting length.
    return (1 - std::exp(-TailSpan * progress)) / (1 - std::exp(-TailSpan));
}
}

LoadProgressBar::LoadProgressBar(QWidget* parent) : QProgressBar(parent) {
    setTextVisible(false);
    setRange(0, 1);
    setValue(0);
    follow_ = expoMotion(this, FollowMs);
    tailMotion_ = new QVariantAnimation(this);
    tailMotion_->setDuration(int(TailSpan * TailTimeConstantMs));
    QEasingCurve curve;
    curve.setCustomType(tailCurve);
    tailMotion_->setEasingCurve(curve);
    tailMotion_->setStartValue(0.0);
    tailMotion_->setEndValue(1.0);
    connect(follow_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        shown_ = value.toDouble(); update(); emit shownChanged();
    });
    connect(tailMotion_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { tail_ = value.toDouble(); update(); });
    bindThemePresentation(this, [this] { update(); });
}

qreal LoadProgressBar::finishedShare() const {
    return maximum() > minimum() ? qBound<qreal>(0, qreal(value() - minimum()) / (maximum() - minimum()), 1) : 0;
}

qreal LoadProgressBar::shownShare() const { return qMin(shown_, finishedShare()); }

qreal LoadProgressBar::tailShare() const {
    if (!inFlight_ || maximum() <= minimum() || value() >= maximum()) return 0;
    return TailReach * tail_ / (maximum() - minimum());
}

int LoadProgressBar::shownPercent() const {
    const int span = maximum() - minimum();
    const int exact = span > 0 ? int(qint64(qBound(minimum(), value(), maximum()) - minimum()) * 100 / span) : 0;
    // While the fill is still catching up the number catches up with it,
    // rounded to the nearest percent and capped at the finished steps.
    return shown_ >= finishedShare() ? exact : qMin(exact, qRound(shown_ * 100));
}

void LoadProgressBar::setSteps(int finished, int total, bool inFlight) {
    const qreal before = finishedShare();
    setRange(0, qMax(1, total));
    setValue(qBound(0, finished, maximum()));
    const qreal target = finishedShare();
    const bool advanced = target > before;
    if (target <= shown_ || !isVisible() || !motionEnabled()) {
        follow_->stop();
        shown_ = target; // never draw past the truth, and never animate backwards
    } else if (follow_->state() != QAbstractAnimation::Running || !qFuzzyCompare(1 + follow_->endValue().toDouble(), 1 + target)) {
        // The window re-reports the same counts on every refresh; only a new
        // target restarts the ease, from wherever the fill is now.
        follow_->stop();
        follow_->setStartValue(shown_);
        follow_->setEndValue(target);
        follow_->start();
    }
    const bool wasInFlight = inFlight_;
    inFlight_ = inFlight && value() < maximum();
    // Each finished step hands the tail to the next one, starting from nothing.
    if (!inFlight_) { tailMotion_->stop(); tail_ = 0; }
    else if (advanced || !wasInFlight) restartTail();
    update();
    emit shownChanged();
}

void LoadProgressBar::resetSteps() {
    follow_->stop();
    tailMotion_->stop();
    shown_ = tail_ = 0;
    inFlight_ = false;
    setRange(0, 1);
    setValue(0);
    update();
    emit shownChanged();
}

void LoadProgressBar::restartTail() {
    tailMotion_->stop();
    tail_ = 0;
    if (isVisible() && motionEnabled()) tailMotion_->start();
}

void LoadProgressBar::showEvent(QShowEvent* event) {
    QProgressBar::showEvent(event);
    if (inFlight_ && tailMotion_->state() != QAbstractAnimation::Running) restartTail();
}

void LoadProgressBar::hideEvent(QHideEvent* event) {
    // Nothing moves off screen: the fill is the truth when the bar returns.
    follow_->stop();
    tailMotion_->stop();
    shown_ = finishedShare();
    QProgressBar::hideEvent(event);
}

void LoadProgressBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors = themeColors();
    const QRectF track = rect();
    const qreal radius = track.height() / 2;
    QPainterPath clip;
    clip.addRoundedRect(track, radius, radius);
    painter.setClipPath(clip);
    painter.fillRect(track, colors.rule);
    painter.setPen(Qt::NoPen);
    // Each segment starts left of the track so only its right end is rounded.
    const auto segment = [&](qreal share, const QColor& color) {
        if (share <= 0) return;
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(-track.height(), 0, track.height() + track.width() * share, track.height()), radius, radius);
    };
    const qreal solid = shownShare();
    const qreal reach = qMin<qreal>(1, finishedShare() + tailShare());
    if (reach > solid) segment(reach, KeefiTheme::blend(colors.accent, colors.rule, 0.38));
    segment(solid, colors.accent);
}

} // namespace keefi
