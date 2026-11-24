#include "keefi/PortfolioAllocation.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/Motion.h"
#include "keefi/TradingTypes.h"
#include <QAbstractAnimation>
#include <QHideEvent>
#include <QPainterPath>
#include <QStringList>
#include <QVariantAnimation>
#include <algorithm>

namespace keefi {
PortfolioAllocation::PortfolioAllocation(QWidget* parent) : QWidget(parent) {
    setObjectName("portfolioAllocation");
    setMinimumWidth(300);
    setFixedHeight(104);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    revealMotion_ = expoMotion(this, 420);
    connect(revealMotion_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        revealProgress_ = value.toDouble();
        update();
    });
    connect(revealMotion_, &QVariantAnimation::finished, this, [this] {
        revealProgress_ = 1.0;
        update();
    });
    bindThemePresentation(this,[this]{update();});
    setGroups({});
}

void PortfolioAllocation::setGroups(const QList<PortfolioGroupView>& groups, const PortfolioMoneyDisplay& display) {
    slices_.clear();
    QStringList values;
    values.reserve(groups.size());
    // Which assets are held, in no particular order. Price ticks reorder slices and move
    // assets in and out of "기타"; neither is a new composition.
    QStringList composition;
    for (const auto& group : groups) {
        const auto value = portfolioAllocationValue(group);
        values.append(value);
        if (decimal::positive(value)) {
            slices_.append({portfolioGroupLabel(group), value, portfolioAssetLabel(group.asset,group.displaySymbol)});
            composition.append(slices_.last().avatarKey);
        }
    }
    total_ = sumAssetAmounts(values);
    composition.sort();
    std::stable_sort(slices_.begin(), slices_.end(), [](const auto& a, const auto& b) {
        return decimal::compare(a.value, b.value) > 0;
    });
    if (slices_.size() > 5) {
        QString rest = "0";
        for (qsizetype i = 4; i < slices_.size(); ++i) rest = decimal::add(rest, slices_[i].value);
        slices_ = slices_.mid(0, 4); slices_.append({tr("기타"), rest,tr("기타")});
    }
    const bool hasSlices = !slices_.isEmpty() && decimal::positive(total_);
    if (!hasSlices || !isVisible() || !motionEnabled()) {
        revealMotion_->stop();
        revealProgress_ = 1.0;
    } else if (composition != composition_) {
        revealMotion_->stop();
        revealProgress_ = 0.0;
        revealMotion_->start();
    }
    composition_ = composition;
    QStringList descriptions;
    for (auto& slice : slices_) {
        slice.share = portfolioShareText(slice.value, total_);
        descriptions << slice.asset + ' ' + slice.share + " · " + display.amount(slice.value) + " " + display.unit();
    }
    if (!slices_.isEmpty() && !display.status().isEmpty()) descriptions.append(display.status());
    setAccessibleName(tr("전체 자산 비중"));
    setAccessibleDescription(descriptions.join(", "));
    updateHeight();
    setToolTip(tr("전체 계좌의 평가가 확인된 보유 자산 기준입니다. 미평가·참고 평가·부채·포지션 명목금액은 제외합니다.\n") + descriptions.join('\n'));
    update();
}

void PortfolioAllocation::updateHeight() {
    const int columns=width()>=470?3:2;
    setFixedHeight(slices_.isEmpty()?58:48+18*((slices_.size()+columns-1)/columns));
}

void PortfolioAllocation::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateHeight();
}

void PortfolioAllocation::hideEvent(QHideEvent* event) {
    if (revealMotion_) revealMotion_->stop();
    revealProgress_ = 1.0;
    QWidget::hideEvent(event);
}

void PortfolioAllocation::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(KeefiFonts::font(FontRole::Caption12));
    painter.setPen(themeColors().text3);
    painter.drawText(QRect(0, 0, width(), 22), Qt::AlignLeft | Qt::AlignVCenter, tr("전체 자산 비중"));
    if (slices_.isEmpty() || !decimal::positive(total_)) {
        painter.drawText(QRect(0, 30, width(), 24), Qt::AlignLeft | Qt::AlignVCenter, tr("평가 가능한 자산 없음"));
        return;
    }
    const QRectF bar(0, 28, width(), 8);
    QPainterPath clip; clip.addRoundedRect(bar, 4, 4);
    painter.save(); painter.setClipPath(clip); painter.fillRect(bar, themeColors().rule);
    // Ratios are only drawing coordinates. All monetary sums and displayed
    // percentages above use the exact decimal presentation helpers.
    qreal offset = 0;
    const double total = total_.toDouble();
    const qreal reveal = qBound<qreal>(0.0, revealProgress_, 1.0);
    for (const auto& slice : slices_) {
        const qreal extent = width() * slice.value.toDouble() / total * reveal;
        painter.fillRect(QRectF(offset, bar.top(), extent, bar.height()), avatarTone(slice.avatarKey).foreground);
        offset += extent;
    }
    painter.restore();
    const int columns = width() >= 470 ? 3 : 2;
    const int cell = width() / columns;
    for (qsizetype i = 0; i < slices_.size(); ++i) {
        const auto& slice = slices_[i];
        const int x = int(i % columns) * cell, y = 46 + int(i / columns) * 18;
        painter.setBrush(avatarTone(slice.avatarKey).foreground); painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(x, y + 5, 6, 6));
        painter.setPen(themeColors().text2);
        const auto& share = slice.share;
        const auto numberFont = KeefiFonts::font(FontRole::Number11);
        const int shareWidth = QFontMetrics(numberFont).horizontalAdvance(share);
        painter.setFont(KeefiFonts::font(FontRole::Caption12));
        const int labelWidth = qMax(0, cell - 18 - shareWidth - 4);
        const auto label = painter.fontMetrics().elidedText(slice.asset, Qt::ElideRight, labelWidth);
        painter.drawText(QRect(x + 13, y, labelWidth, 18), Qt::AlignLeft | Qt::AlignVCenter, label);
        const int shareX = x + 13 + painter.fontMetrics().horizontalAdvance(label) + 4;
        painter.setFont(numberFont);
        painter.drawText(QRect(shareX, y, shareWidth, 18), Qt::AlignLeft | Qt::AlignVCenter, share);
    }
}
} // namespace keefi
