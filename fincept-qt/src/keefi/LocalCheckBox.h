#pragma once
#include "keefi/KeefiTheme.h"

#include <QCheckBox>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>

namespace keefi {

// Keep the check mark visible when a dark stylesheet replaces native indicators.
class LocalCheckBox final : public QCheckBox {
  public:
    explicit LocalCheckBox(const QString& text, QWidget* parent = nullptr) : QCheckBox(text, parent) {}

  protected:
    bool hitButton(const QPoint& position) const override {
        // Dialog rows opt in after allocating a full 44 px pointer target.
        return property("fullRowTarget").toBool() ? rect().contains(position)
                                                 : QCheckBox::hitButton(position);
    }
    void paintEvent(QPaintEvent* event) override {
        QCheckBox::paintEvent(event);
        if (!isChecked())
            return;
        QStyleOptionButton option;
        initStyleOption(&option);
        const QRect rect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(isEnabled() ? themeColors().bg : themeColors().text3);
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        const QPointF left(rect.left() + rect.width() * 0.23, rect.top() + rect.height() * 0.52);
        const QPointF middle(rect.left() + rect.width() * 0.43, rect.top() + rect.height() * 0.72);
        const QPointF right(rect.left() + rect.width() * 0.78, rect.top() + rect.height() * 0.28);
        painter.drawLine(left, middle);
        painter.drawLine(middle, right);
    }
};

} // namespace keefi
