#pragma once

#include <QEasingCurve>
#include <QVariantAnimation>
#include <QtGlobal>

namespace keefi {

// Motion is presentation only. A widget's state is always its target value, so
// a hidden widget or a disabled animation shows the same final frame at once.
// KEEFI_REDUCE_MOTION turns every transition off (still renders, fixed captures).
inline bool motionEnabled() {
    static const bool reduced = qEnvironmentVariableIsSet("KEEFI_REDUCE_MOTION");
    return !reduced;
}

// One curve for every KEEFI transition: f(t) = 1 - 2^(-10t). Most of the
// distance is covered in the first fifth of the duration, then it settles.
inline QVariantAnimation* expoMotion(QObject* owner, int milliseconds) {
    auto* motion = new QVariantAnimation(owner);
    motion->setDuration(milliseconds);
    motion->setEasingCurve(QEasingCurve::OutExpo);
    motion->setStartValue(0.0);
    motion->setEndValue(1.0);
    return motion;
}

} // namespace keefi
