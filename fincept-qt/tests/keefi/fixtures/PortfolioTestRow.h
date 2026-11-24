#pragma once

#include <QPersistentModelIndex>
#include <QTreeView>

namespace keefi::test {

// Read the same model roles and view state that a user sees. A persistent
// index keeps selection/layout assertions meaningful across price reordering.
struct PortfolioTestRow {
    QTreeView* view = nullptr;
    QPersistentModelIndex index;

    explicit operator bool() const { return index.isValid(); }
    QModelIndex modelIndex() const { return index; }
    QVariant data(int column, int role) const {
        return modelIndex().siblingAtColumn(column).data(role);
    }
    QString text(int column) const { return data(column, Qt::DisplayRole).toString(); }
    int childCount() const { return view->model()->rowCount(index); }
    PortfolioTestRow child(int row) const { return {view, view->model()->index(row, 0, index)}; }
    PortfolioTestRow parent() const { return {view, index.parent()}; }
    bool isHidden() const { return view->isRowHidden(index.row(), index.parent()); }
    bool isExpanded() const { return view->isExpanded(index); }
    void setExpanded(bool on) const { view->setExpanded(index, on); }
};

inline PortfolioTestRow portfolioRow(QTreeView* view, int row) {
    return {view, view->model()->index(row, 0)};
}

inline PortfolioTestRow selectedPortfolioRow(QTreeView* view) {
    return {view, view->currentIndex().siblingAtColumn(0)};
}

} // namespace keefi::test
