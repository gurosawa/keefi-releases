#include "keefi/SplitterShare.h"
#include <QSplitter>
#include <QtMath>

namespace keefi {
int SplitterShare::extent(const QSplitter& splitter) {
    const auto area = splitter.contentsRect();
    return (splitter.orientation() == Qt::Horizontal ? area.width() : area.height()) - splitter.handleWidth();
}

void SplitterShare::remember(const QSplitter& splitter) {
    const auto sizes = splitter.sizes();
    if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0) share_ = double(sizes[1]) / (sizes[0] + sizes[1]);
}

bool SplitterShare::restore(QSplitter& splitter, int firstFloor, int secondFloor) const {
    if (splitter.count() != 2) return false;
    const int width = extent(splitter);
    if (width <= 0) return false;
    const int second = qMax(secondFloor, qMin(width - firstFloor, qRound(width * share_)));
    splitter.setSizes({width - second, second});
    return true;
}
} // namespace keefi
