#include "keefi/VaultMark.h"

#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/Motion.h"

#include <QEvent>
#include <QHideEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <QtMath>
#include <cmath>

namespace keefi {
namespace {
constexpr int Cell = 52, Gap = 14, CaptionGap = 10;
qreal outExpo(qreal t) { return t >= 1 ? 1 : 1 - std::pow(2.0, -10 * t); }
}

VaultMark::VaultMark(QWidget* parent) : QWidget(parent) {
    // Decoration for sighted users; the form beside it carries every condition
    // for keyboard and screen-reader users.
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(text());
    clock_.start();
    ticker_ = new QVariantAnimation(this);
    ticker_->setStartValue(0.0);
    ticker_->setEndValue(1.0);
    ticker_->setDuration(RiseMs + StaggerMs * (Letters - 1));
    connect(ticker_, &QVariantAnimation::valueChanged, this, [this] { update(); });
    connect(ticker_, &QVariantAnimation::finished, this, [this] { update(); });
    shaker_ = new QVariantAnimation(this);
    shaker_->setStartValue(0.0);
    shaker_->setEndValue(1.0);
    shaker_->setDuration(ShakeMs);
    // Three swings inside an exponentially shrinking envelope, 2^(-10t).
    connect(shaker_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        const qreal t = value.toDouble();
        shakeX_ = t >= 1 ? 0 : 8 * std::pow(2.0, -10 * t) * std::sin(t * 2 * M_PI * 3);
        update();
    });
    connect(shaker_, &QVariantAnimation::finished, this, [this] { shakeX_ = 0; update(); });
    bindThemePresentation(this, [this] { settle(); update(); });
    describe();
}

QString VaultMark::ruleName(int index) const {
    switch (index) {
    case 0: return tr("12자");
    case 1: return tr("대문자");
    case 2: return tr("소문자");
    case 3: return tr("숫자");
    default: return tr("특수문자");
    }
}

VaultMark::Letter VaultMark::letter(int index) const { return tracks_.at(index).to; }

QColor VaultMark::colour(Letter letter) const {
    const auto& c = themeColors();
    switch (letter) {
    case Letter::Off: return c.field;
    case Letter::On: return c.text;
    case Letter::Ready: return c.accent;
    case Letter::Rejected: return c.danger;
    }
    return c.text;
}

qreal VaultMark::eased(int index) const {
    const auto& track = tracks_[index];
    if (track.still || !motionEnabled()) return 1;
    const qreal t = qreal(clock_.elapsed() - track.startMs) / RiseMs;
    return t <= 0 ? 0 : outExpo(qMin<qreal>(1, t));
}

qreal VaultMark::shownDrop(int index) const {
    const auto& track = tracks_.at(index);
    return track.fromDrop + (drop(track.to) - track.fromDrop) * eased(index);
}

QColor VaultMark::shownColour(int index) const {
    const auto& track = tracks_[index];
    const qreal progress = eased(index);
    return progress >= 1 ? colour(track.to) : KeefiTheme::blend(colour(track.to), track.fromColour, progress);
}

bool VaultMark::settled() const {
    return ticker_->state() != QAbstractAnimation::Running && shaker_->state() != QAbstractAnimation::Running;
}

void VaultMark::retarget(int index, Letter target, int delayMs) {
    auto& track = tracks_[index];
    if (track.to == target) return;
    // Start from wherever the letter is drawn now, so a change mid-movement
    // continues instead of jumping.
    track.fromColour = shownColour(index);
    track.fromDrop = shownDrop(index);
    track.to = target;
    track.startMs = clock_.elapsed() + delayMs;
    track.still = !isVisible() || !motionEnabled();
}

void VaultMark::kick() {
    ticker_->stop();
    for (const auto& track : tracks_)
        if (!track.still) { ticker_->start(); break; }
    update();
}

void VaultMark::settle() {
    ticker_->stop();
    shaker_->stop();
    shakeX_ = 0;
    for (auto& track : tracks_) track.still = true;
}

void VaultMark::describe() {
    int met = 0;
    for (int i = 0; i < Letters; ++i) met += (met_ >> i) & 1;
    setAccessibleDescription(captions_ ? tr("암호 조건 %1/%2 충족").arg(met).arg(Letters) : QString{});
}

void VaultMark::setCaptionsVisible(bool visible) {
    if (captions_ == visible) return;
    captions_ = visible;
    updateGeometry();
    update();
    describe();
}

void VaultMark::setProgress(int metRules, bool ready) {
    met_ = metRules & ((1 << Letters) - 1);
    ready_ = ready;
    // Clearing the field after a failed check is not new input: keep the
    // failed pose until something is typed again.
    if (rejected_ && met_ == 0 && !ready_) { describe(); return; }
    if (rejected_) { shaker_->stop(); shakeX_ = 0; }
    rejected_ = false;
    for (int i = 0; i < Letters; ++i) {
        const Letter target = ready_ ? Letter::Ready : (met_ >> i) & 1 ? Letter::On : Letter::Off;
        // Becoming ready lights the word left to right; other changes move at once.
        retarget(i, target, ready_ ? i * StaggerMs : 0);
    }
    kick();
    describe();
}

void VaultMark::hold() {
    rejected_ = false;
    settle();
    for (auto& track : tracks_) track.to = Letter::Ready;
    if (isVisible()) repaint();
}

void VaultMark::reject() {
    rejected_ = true;
    settle();
    // The failure reads at once; only the shake takes its time.
    for (auto& track : tracks_) track.to = Letter::Rejected;
    if (isVisible() && motionEnabled()) shaker_->start();
    update();
    describe();
}

void VaultMark::reset() {
    rejected_ = false;
    met_ = 0;
    ready_ = false;
    settle();
    for (auto& track : tracks_) track.to = Letter::Off;
    update();
    describe();
}

QSize VaultMark::sizeHint() const {
    const QFontMetricsF glyphs(KeefiFonts::font(FontRole::Price44));
    qreal height = glyphs.height() + OffDrop;
    if (captions_) height += CaptionGap + QFontMetricsF(KeefiFonts::font(FontRole::CaptionStrong12)).height();
    return {Letters * Cell + (Letters - 1) * Gap, qCeil(height)};
}

QSize VaultMark::minimumSizeHint() const { return sizeHint(); }

void VaultMark::hideEvent(QHideEvent* event) {
    settle();
    QWidget::hideEvent(event);
}

void VaultMark::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange) update();
    QWidget::changeEvent(event);
}

void VaultMark::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const auto& c = themeColors();
    const QFont glyphFont = KeefiFonts::font(FontRole::Price44);
    const QFontMetricsF glyphs(glyphFont);
    const qreal left = (width() - (Letters * Cell + (Letters - 1) * Gap)) / 2.0;
    const QString word = text();
    painter.setFont(glyphFont);
    for (int i = 0; i < Letters; ++i) {
        const QString glyph = word.mid(i, 1);
        const qreal x = left + i * (Cell + Gap) + (Cell - glyphs.horizontalAdvance(glyph)) / 2;
        // A form that cannot be used shows every letter unlit.
        painter.setPen(isEnabled() ? shownColour(i) : colour(Letter::Off));
        painter.drawText(QPointF(x + shakeX_, glyphs.ascent() + shownDrop(i)), glyph);
    }
    if (!captions_) return;
    const qreal top = glyphs.height() + OffDrop + CaptionGap;
    const qreal height = QFontMetricsF(KeefiFonts::font(FontRole::CaptionStrong12)).height();
    for (int i = 0; i < Letters; ++i) {
        const bool met = ready_ || (met_ >> i) & 1;
        painter.setFont(KeefiFonts::font(met ? FontRole::CaptionStrong12 : FontRole::Caption12));
        painter.setPen(met && isEnabled() ? c.text : c.text3);
        const qreal x = left + i * (Cell + Gap);
        painter.drawText(QRectF(x - Gap / 2.0, top, Cell + Gap, height), Qt::AlignHCenter | Qt::AlignTop, ruleName(i));
    }
}

} // namespace keefi
