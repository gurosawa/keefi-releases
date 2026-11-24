#pragma once

#include <QHeaderView>
#include <QTableWidget>
#include <QTimer>
#include "keefi/KeefiVisuals.h"

namespace keefi {
// Preserve native cell backgrounds, focus, decorations, metrics and model data;
// only shape the displayed numeric/code runs with the bundled mono face.
class LedgerItemDelegate final : public QStyledItemDelegate {
 public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& original,const QModelIndex& index)const override {
        QStyleOptionViewItem option=original;initStyleOption(&option,index);
        const auto text=option.text;
        const auto* style=option.widget?option.widget->style():QApplication::style();
        const auto textRect=style->subElementRect(QStyle::SE_ItemViewItemText,&option,option.widget);
        // Native wrapping/elision remains authoritative for an overflowing cell.
        // Do not replace a user's enlarged/wrapped row with a single clipped line.
        if((option.features&QStyleOptionViewItem::WrapText)&&(text.contains('\n')||text.contains(QChar::LineSeparator)||text.contains(QChar::ParagraphSeparator)||QFontMetricsF(option.font).horizontalAdvance(text)>textRect.width())){
            QStyledItemDelegate::paint(painter,original,index);return;
        }
        option.text.clear();style->drawControl(QStyle::CE_ItemViewItem,&option,painter,option.widget);
        const auto group=option.state&QStyle::State_Enabled?QPalette::Active:QPalette::Disabled;
        painter->save();painter->setPen(option.palette.color(group,option.state&QStyle::State_Selected?QPalette::HighlightedText:QPalette::Text));
        drawLedgerText(*painter,textRect,text,FontRole::Body15,option.displayAlignment,&option.font);painter->restore();
    }
};
class LocalTableWidget final : public QTableWidget {
  public:
    explicit LocalTableWidget(QWidget* parent = nullptr) : LocalTableWidget(0, 0, parent) {}
    LocalTableWidget(int rows, int columns, QWidget* parent = nullptr) : QTableWidget(rows, columns, parent) {
        setItemDelegate(new LedgerItemDelegate(this));
        // ResizeToContents can synchronously scan all rows for every setItem.
        // Keep cell insertion cheap and measure column widths once per event batch.
        horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        auto* resize = new QTimer(this);
        resize->setSingleShot(true);
        const auto schedule = [resize] { resize->start(0); };
        connect(model(), &QAbstractItemModel::dataChanged, this, schedule);
        connect(model(), &QAbstractItemModel::headerDataChanged, this, schedule);
        connect(model(), &QAbstractItemModel::rowsInserted, this, schedule);
        connect(model(), &QAbstractItemModel::rowsRemoved, this, schedule);
        connect(model(), &QAbstractItemModel::modelReset, this, schedule);
        connect(resize, &QTimer::timeout, this, [this] { resizeColumnsToContents(); });
        schedule();
    }
};
} // namespace keefi
