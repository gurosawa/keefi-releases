#include "keefi/PressRevealButton.h"

#include "keefi/KeefiTheme.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

namespace keefi {
namespace {
constexpr int InlineSize = 28, InlineInset = 2, TextClearance = 22;
QIcon eyeIcon(qreal dpr) {
    QPixmap pixmap(QSize(qCeil(18 * dpr), qCeil(18 * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(18.0 / 24.0, 18.0 / 24.0);
    QPen pen(themeColors().text3, 1.75);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPainterPath eye;
    eye.moveTo(2, 12);
    eye.cubicTo(5, 6.5, 19, 6.5, 22, 12);
    eye.cubicTo(19, 17.5, 5, 17.5, 2, 12);
    painter.drawPath(eye);
    painter.drawEllipse(QPointF(12, 12), 3, 3);
    painter.end();
    return QIcon(pixmap);
}
}

PressRevealButton::PressRevealButton(QLineEdit* field, QWidget* parent) : QToolButton(parent), field_(field) {
    setText(tr("보기"));
    setToolTip(tr("누르는 동안 암호 표시"));
    setAccessibleName(tr("누르는 동안 암호 표시"));
    setAccessibleDescription(tr("버튼을 누르고 있는 동안만 이 입력란의 내용을 표시합니다. 놓으면 다시 가립니다."));
    setFocusPolicy(Qt::StrongFocus);
    setAutoRepeat(false);
    if (field_) {
        field_->setEchoMode(QLineEdit::Password);
        field_->installEventFilter(this);
    }
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) conceal();
    });
}

PressRevealButton::~PressRevealButton() { conceal(); }

void PressRevealButton::setInlineEye() {
    inline_ = true;
    setText(QString());
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setFixedSize(InlineSize, InlineSize);
    setIconSize(QSize(18, 18));
    setCursor(Qt::PointingHandCursor);
    setThemeStyle(this, QStringLiteral(R"(
        QToolButton { background: transparent; border: 2px solid transparent; border-radius: 6px; padding: 0; }
        QToolButton:hover { background: ${surface}; }
        QToolButton:pressed { background: ${rule}; }
        QToolButton:focus { border-color: ${accent}; }
        QToolButton:disabled { background: transparent; }
    )"));
    bindThemePresentation(this, [this] { setIcon(eyeIcon(devicePixelRatioF())); });
    if (field_) field_->setTextMargins(0, 0, TextClearance, 0);
    place();
    raise();
}

void PressRevealButton::place() {
    if (!inline_ || !field_ || field_->parentWidget() != parentWidget()) return;
    move(field_->x() + field_->width() - width() - InlineInset, field_->y() + (field_->height() - height()) / 2);
}

void PressRevealButton::reveal() {
    if (!field_ || !isEnabled() || !field_->isEnabled()) return;
    revealing_ = true;
    setDown(true);
    field_->setEchoMode(QLineEdit::Normal);
}

void PressRevealButton::conceal() {
    revealing_ = false;
    heldKey_ = 0;
    setDown(false);
    if (field_) field_->setEchoMode(QLineEdit::Password);
}

// The base press machinery is bypassed on purpose: QAbstractButton re-arms the
// down state when a held pointer re-enters, which would show the value again.
void PressRevealButton::mousePressEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) reveal();
}

void PressRevealButton::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    if (revealing_ && !rect().contains(event->position().toPoint())) conceal();
}

void PressRevealButton::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    conceal();
}

static bool holdKey(int key) { return key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter; }

void PressRevealButton::keyPressEvent(QKeyEvent* event) {
    if (holdKey(event->key())) {
        // Consume Enter too, so holding the button never submits the form.
        event->accept();
        if (!event->isAutoRepeat() && !revealing_) {
            heldKey_ = event->key();
            reveal();
        }
        return;
    }
    if (event->key() == Qt::Key_Escape && revealing_) {
        event->accept();
        conceal();
        return;
    }
    QToolButton::keyPressEvent(event);
}

void PressRevealButton::keyReleaseEvent(QKeyEvent* event) {
    if (holdKey(event->key())) {
        event->accept();
        if (!event->isAutoRepeat() && event->key() == heldKey_) conceal();
        return;
    }
    QToolButton::keyReleaseEvent(event);
}

void PressRevealButton::focusOutEvent(QFocusEvent* event) {
    conceal();
    QToolButton::focusOutEvent(event);
}

void PressRevealButton::hideEvent(QHideEvent* event) {
    conceal();
    QToolButton::hideEvent(event);
}

void PressRevealButton::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) conceal();
    QToolButton::changeEvent(event);
}

bool PressRevealButton::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::WindowDeactivate: case QEvent::TouchCancel: case QEvent::Leave:
        if (event->type() != QEvent::Leave || revealing_) conceal();
        break;
    default:
        break;
    }
    return QToolButton::event(event);
}

bool PressRevealButton::eventFilter(QObject* watched, QEvent* event) {
    if (watched == field_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) place();
    if (watched == field_ && (event->type() == QEvent::Hide ||
                              (event->type() == QEvent::EnabledChange && !field_->isEnabled())))
        conceal();
    return QToolButton::eventFilter(watched, event);
}

} // namespace keefi
