#include "keefi/ExploreWatchlist.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/ProfileSettings.h"
#include "keefi/UsabilityPresentation.h"

#include <QHeaderView>
#include <QEvent>
#include <QFontMetricsF>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QApplication>
#include <QTabBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace keefi {
namespace {
const QStringList Sections{"coin", "kr", "us"};
constexpr int HistoryRole = Qt::UserRole + 1;
// Presentation-only observations. DisplayRole and the stored favourite IDs
// retain their existing contracts in both the compact and full tables.
constexpr int CompactValueRole = Qt::UserRole + 2;
constexpr int CompactNameRole = Qt::UserRole + 3;
constexpr int StaleQuoteRole = Qt::UserRole + 4;
int ledgerWidth(const QString& text) {
    QTextLayout layout(text,KeefiFonts::font(FontRole::Secondary14));layout.setFormats(KeefiFonts::numberRuns(text));
    layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();
    return line.isValid()?int(std::ceil(line.naturalTextWidth())):0;
}

qreal drawWatchText(QPainter* painter, const QRectF& area, const QString& text,
                    const QFont& font, const QColor& ink, Qt::Alignment alignment,
                    bool elide = true) {
    if (area.width() <= 0 || text.isEmpty()) return 0;
    const QFontMetricsF metrics(font);
    const auto shown = elide ? metrics.elidedText(text, Qt::ElideRight, area.width()) : text;
    const qreal width = metrics.horizontalAdvance(shown);
    const qreal x = alignment.testFlag(Qt::AlignRight) ? area.right() - width
        : alignment.testFlag(Qt::AlignHCenter) ? area.center().x() - width / 2 : area.left();
    painter->save();
    painter->setClipRect(area, Qt::IntersectClip);
    painter->setFont(font); painter->setPen(ink);
    painter->drawText(QPointF(x, area.center().y() + (metrics.ascent() - metrics.descent()) / 2), shown);
    painter->restore();
    return width;
}

// Today's narrow rail uses one 32px baseline for code, name, quote and change.
// Ranking/map rails and the ordinary table keep their existing presentation.
class WatchNameDelegate final : public QStyledItemDelegate {
  public:
    explicit WatchNameDelegate(QTableWidget* table) : QStyledItemDelegate(table), table_(table) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (table_->property("dashboardWatchlist").toBool()) size.setHeight(32);
        return size;
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!table_->property("compactWatchlist").toBool()) { QStyledItemDelegate::paint(p, option, index); return; }
        if (!table_->property("dashboardWatchlist").toBool()) {
            auto base=option;initStyleOption(&base,index);
            const auto lines=index.data(Qt::DisplayRole).toString().split('\n');base.text.clear();
            const auto* style=option.widget?option.widget->style():QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem,&base,p,option.widget);
            p->save();p->setClipRect(option.rect);
            const auto box=option.rect.adjusted(4,2,-4,-2);
            const auto nameFont=KeefiFonts::font(FontRole::SecondaryStrong14);
            p->setFont(nameFont);p->setPen(themeColors().text);
            p->drawText(QRect(box.left(),box.top(),box.width(),20),Qt::AlignLeft|Qt::AlignVCenter,
                QFontMetrics(nameFont).elidedText(lines.value(0),Qt::ElideRight,box.width()));
            if(lines.size()>1){
                const auto codeFont=KeefiFonts::font(FontRole::Number11);p->setFont(codeFont);p->setPen(themeColors().text3);
                p->drawText(QRect(box.left(),box.bottom()-14,box.width(),14),Qt::AlignLeft|Qt::AlignVCenter,
                    QFontMetrics(codeFont).elidedText(lines.value(1),Qt::ElideRight,box.width()));
            }
            p->restore();return;
        }
        auto base=option; initStyleOption(&base,index);
        base.text.clear();
        const auto* style=option.widget?option.widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&base,p,option.widget);
        p->save(); p->setClipRect(option.rect, Qt::IntersectClip);
        const auto box=QRectF(option.rect).adjusted(4,0,-4,0);
        const bool stale=index.data(StaleQuoteRole).toBool();
        const qreal textRight=box.right()-(stale?10:0);
        const auto code=index.data(Qt::UserRole).toString();
        const auto name=index.data(CompactNameRole).toString();
        qreal x=box.left();
        x+=drawWatchText(p,QRectF(x,box.top(),qMax(0.0,textRight-x),box.height()),code,
            KeefiFonts::font(FontRole::SecondaryStrong14),themeColors().text,Qt::AlignLeft);
        if(!name.isEmpty()&&name!=code&&textRight-x>4){
            x+=4;
            x+=drawWatchText(p,QRectF(x,box.top(),qMax(0.0,textRight-x),box.height()),name,
                KeefiFonts::font(FontRole::Caption12),themeColors().text3,Qt::AlignLeft);
        }
        if(stale&&box.width()>=10){
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);p->setBrush(themeColors().text3);
            p->drawEllipse(QRectF(qMin(x+4,box.right()-6),box.center().y()-3,6,6));
        }
        p->restore();
    }
  private:
    QTableWidget* table_;
};

class WatchValueDelegate final : public QStyledItemDelegate {
  public:
    explicit WatchValueDelegate(QTableWidget* table)
        : QStyledItemDelegate(table), table_(table), ordinary_(new WorkbenchRowDelegate(this)) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!table_->property("dashboardWatchlist").toBool())
            return index.column()==0?QStyledItemDelegate::sizeHint(option,index):ordinary_->sizeHint(option,index);
        auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(32);return size;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!table_->property("dashboardWatchlist").toBool()) {
            if(index.column()==0)QStyledItemDelegate::paint(painter,option,index);
            else ordinary_->paint(painter,option,index);
            return;
        }
        auto base=option;initStyleOption(&base,index);base.text.clear();
        const auto* style=option.widget?option.widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&base,painter,option.widget);
        const auto compactValue=index.data(CompactValueRole);
        const auto text=compactValue.isValid()?compactValue.toString():index.data(Qt::DisplayRole).toString();
        const bool star=index.column()==0;
        // Fixed numeric columns use the Number11 face without the full table's
        // 12px insets. Do not reformat values or remove movement markers.
        drawWatchText(painter,star?QRectF(option.rect):QRectF(option.rect).adjusted(2,0,-2,0),text,
            KeefiFonts::font(star?FontRole::Caption12:FontRole::Number11),base.palette.color(QPalette::Text),
            star?Qt::AlignHCenter:Qt::AlignRight,false);
    }
  private:
    QTableWidget* table_;
    WorkbenchRowDelegate* ordinary_;
};

class TrendDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto base = option;
        initStyleOption(&base, index);
        const auto points = index.data(HistoryRole).value<QList<QPointF>>();
        if (points.size() < 2) { QStyledItemDelegate::paint(p, base, index); return; }
        base.text.clear();
        const auto* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &base, p, option.widget);
        const auto [low, high] = std::minmax_element(points.cbegin(), points.cend(), [](auto a, auto b) { return a.y() < b.y(); });
        const auto box = QRectF(option.rect).adjusted(12, 13, -12, -13);
        const double span = points.last().x() - points.first().x(), range = high->y() - low->y();
        if (span <= 0 || box.width() <= 0) return;
        QPainterPath path;
        for (int i = 0; i < points.size(); ++i) {
            const QPointF at(box.left() + (points[i].x() - points.first().x()) / span * box.width(),
                             range > 0 ? box.bottom() - (points[i].y() - low->y()) / range * box.height() : box.center().y());
            if (i == 0) path.moveTo(at); else path.lineTo(at);
        }
        p->save();
        p->setClipRect(option.rect);
        p->setRenderHint(QPainter::Antialiasing);
        const auto direction=movementForSign(points.last().y()>points.first().y()?1:points.last().y()<points.first().y()?-1:0);
        p->setPen(QPen(direction == MovementDirection::Flat ? themeColors().text3 : MovementColorPolicy::fillFor(option.widget,direction), 1.6));
        p->drawPath(path);
        p->restore();
    }
};
} // namespace

ExploreWatchlist::ExploreWatchlist(QWidget* parent) : QFrame(parent) {
    setObjectName("exploreWatchlist");
    setProperty("spotlight", true);
    setAttribute(Qt::WA_StyledBackground, true);
    setThemeStyle(this, "QFrame#exploreWatchlist { background: ${bg}; border: 1px solid ${rule}; border-radius: 12px; }");
    setMinimumHeight(230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(8);
    head_ = new QHBoxLayout;
    auto* title = new QLabel(tr("관심 리스트"), this);
    title->setProperty("type", "section");
    head_->addWidget(title);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName("watchlistMarkets");
    tabs_->setExpanding(false);
    tabs_->setElideMode(Qt::ElideNone);
    tabs_->setUsesScrollButtons(false);
    for (const auto key : {UsabilityText::ExploreMarketCoin, UsabilityText::ExploreMarketKorea, UsabilityText::ExploreMarketUs}) tabs_->addTab(usabilityText(key));
    head_->addStretch();
    auto* reset = new QPushButton(tr("추가순"), this);
    reset->setObjectName("watchlistInsertionOrder");
    reset->setProperty("compact", true);
    head_->addWidget(reset);
    layout->addLayout(head_);
    layout->addWidget(tabs_, 0, Qt::AlignLeft);
    table_ = new QTableWidget(0, 7, this);
    table_->setObjectName("watchlistTable");
    setFocusProxy(table_);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setFrameShape(QFrame::NoFrame);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(58);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->setItemDelegateForColumn(1, new WatchNameDelegate(table_));
    table_->setItemDelegateForColumn(4, new TrendDelegate(table_));
    for (int column : {0,2,3}) table_->setItemDelegateForColumn(column, new WatchValueDelegate(table_));
    for (int column : {5,6}) table_->setItemDelegateForColumn(column, new WorkbenchRowDelegate(table_));
    table_->horizontalHeader()->setMinimumSectionSize(32);
    table_->viewport()->installEventFilter(this);
    layout->addWidget(table_, 1);
    empty_ = new QLabel(this);
    empty_->setObjectName("watchlistEmpty");
    empty_->setProperty("type", "muted");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    layout->addWidget(empty_, 1);
    auto* basis = new QLabel(tr("★로 저장 · 코인 24시간 / 주식 전일 대비 · MC는 표시 통화 기준"), this);
    basis->setObjectName("watchlistBasis");
    basis->setProperty("type", "muted");
    basis->setWordWrap(true);
    layout->addWidget(basis);
    auto* staleLegend = new QLabel(tr("● 이전 시세 · 갱신 대기"), this);
    staleLegend->setObjectName("watchlistStaleLegend");
    staleLegend->setProperty("type", "muted");
    staleLegend->setWordWrap(false);
    staleLegend->hide();
    layout->addWidget(staleLegend);
    for (const auto& key : Sections) {
        auto values = ProfileSettings().value("explore/watchlist/" + key).toStringList();
        values.removeAll(QString{});
        values.removeDuplicates();
        lists_.insert(key, values);
    }
    connect(tabs_, &QTabBar::currentChanged, this, [this] {
        sortColumn_ = -1;
        {
            const QScopedValueRollback guard(changingSection_, true);
            emit sectionChanged(); // The owner supplies this market's current quotes synchronously.
        }
        render();
    });
    connect(reset, &QPushButton::clicked, this, [this] { sortColumn_ = -1; render(); });
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
        if (column == 0 || column == 4) return;
        sortOrder_ = sortColumn_ == column ? (sortOrder_ == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder)
                                          : column == 1 ? Qt::AscendingOrder : Qt::DescendingOrder;
        sortColumn_ = column;
        render();
    });
    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int column) {
        auto* item = table_->item(row, 1);
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString();
        if (column == 0) toggle(section(), id); else emit itemSelected(section(), id);
    });
    connect(table_, &QTableWidget::itemActivated, this, [this](QTableWidgetItem* item) {
        const auto id = table_->item(item->row(), 1)->data(Qt::UserRole).toString();
        if (item->column() == 0) toggle(section(), id); else emit itemSelected(section(), id);
    });
    auto* remove = new QShortcut(QKeySequence(Qt::Key_Delete), table_);
    remove->setContext(Qt::WidgetWithChildrenShortcut);
    connect(remove, &QShortcut::activated, this, [this] {
        if (auto* item = table_->item(table_->currentRow(), 1)) toggle(section(), item->data(Qt::UserRole).toString());
    });
    render();
    bindMovementPresentation(this,[this]{if(!deferHiddenPresentation()&&isVisible())render();});
    bindThemePresentation(this,[this]{columnMeasurementsDirty_ = true; render();});
}

QStringList ExploreWatchlist::ids(const QString& section) const { return lists_.value(section); }
QStringList ExploreWatchlist::visibleIds() const {
    QStringList ids;for (int row = 0; row < table_->rowCount(); ++row) if (!table_->isRowHidden(row))
        if (auto* item = table_->item(row,1)) ids.append(item->data(Qt::UserRole).toString());
    return ids;
}
void ExploreWatchlist::setEvidenceNumbers(const QHash<QString, QList<int>>& quotes, const QHash<QString, QList<int>>& histories) {
    if (quoteEvidenceNumbers_ == quotes && historyEvidenceNumbers_ == histories) return;
    quoteEvidenceNumbers_ = quotes; historyEvidenceNumbers_ = histories;
    if (deferHiddenPresentation()) return;
    applyEvidenceNumbers();
    table_->viewport()->update();
}
void ExploreWatchlist::applyEvidenceNumbers() {
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto id = table_->item(row,1)->data(Qt::UserRole).toString();
        for (int column : {2,3,4,5,6}) if (auto* item = table_->item(row,column)) {
            const auto numbers = column == 4 ? historyEvidenceNumbers_.value(id) : item->text().isEmpty() || item->text()==QStringLiteral("—") ? QList<int>{} : quoteEvidenceNumbers_.value(id);
            item->setData(EvidenceNumbersRole,QVariant::fromValue(numbers));
            item->setData(Qt::AccessibleDescriptionRole,item->text());
        }
    }
}
void ExploreWatchlist::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    const bool catchUp = presentationStarted_;
    presentationStarted_ = true;
    // The existing movement binding may already catch up in its Show filter.
    if (catchUp && renderPending_) render();
    emit visibilityChanged();
}
bool ExploreWatchlist::deferHiddenPresentation() {
    // A hidden stack page may never receive Show even after its window starts.
    // Keep that start observed, including after the parent window is hidden.
    if (window() != this && window()->isVisible()) presentationStarted_ = true;
    if (presentationStarted_ && !isVisible()) { renderPending_ = true; return true; }
    return false;
}
void ExploreWatchlist::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    emit visibilityChanged();
}
bool ExploreWatchlist::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        layoutColumns();
    return QFrame::eventFilter(watched, event);
}
void ExploreWatchlist::layoutColumns() {
    for (int column : {5, 6}) table_->setColumnHidden(column, compact_);
    table_->setColumnHidden(4, compact_ || !hasHistory_);
    if (compact_ && dashboardLayout_) {
        const std::array<int,4> widths{14,qMax(0,table_->viewport()->width()-14-92-62),92,62};
        for(int column=0;column<int(widths.size());++column)
            if(table_->columnWidth(column)!=widths[column])table_->setColumnWidth(column,widths[column]);
        return;
    }
    // Keep numeric columns readable when narrow, and share extra room instead
    // of letting the final volume column consume the entire wide viewport.
    if (columnMeasurementsDirty_) {
        measuredColumns_ = compact_ ? std::array<int,7>{24,72,98,70,0,0,0}
                                    : std::array<int,7>{34,190,125,88,140,110,115};
        const int padding = compact_ ? 12 : 24;
        for (int column : {2,3,5,6}) {
            if (compact_ && column > 3) continue;
            for (int row = 0; row < table_->rowCount(); ++row)
                if (auto* item = table_->item(row,column))
                    measuredColumns_[column] = qMax(measuredColumns_[column], ledgerWidth(item->text()) + padding);
        }
        columnMeasurementsDirty_ = false;
    }
    auto minimums = measuredColumns_;
    const auto weights = compact_ ? std::array<int,7>{0,60,25,15,0,0,0}
                                  : std::array<int,7>{0,30,17,9,22,11,11};
    for (int column = 0; column < int(minimums.size()); ++column) if (table_->isColumnHidden(column)) minimums[column] = 0;
    int minimumWidth = 0;
    for (const auto width : minimums) minimumWidth += width;
    const int extra = qMax(0, table_->viewport()->width() - minimumWidth);
    int totalWeight = 0;
    for (int column = 0; column < int(minimums.size()); ++column) if (!table_->isColumnHidden(column)) totalWeight += weights[column];
    int cumulativeWeight = 0, allocated = 0;
    for (int column = 0; column < int(minimums.size()); ++column) {
        if (table_->isColumnHidden(column)) continue;
        cumulativeWeight += weights[column];
        const int through = extra * cumulativeWeight / qMax(1, totalWeight);
        const int width = minimums[column] + through - allocated;
        if (table_->columnWidth(column) != width) table_->setColumnWidth(column, width);
        allocated = through;
    }
}
bool ExploreWatchlist::contains(const QString& section, const QString& id) const { return lists_.value(section).contains(id); }
void ExploreWatchlist::setCompact(bool compact) {
    if (compact_ == compact) return;
    compact_ = compact;
    applyPresentation();
}
void ExploreWatchlist::setDashboardLayout(bool dashboard) {
    if(dashboardLayout_==dashboard)return;
    dashboardLayout_=dashboard;
    if(compact_)applyPresentation();
}
void ExploreWatchlist::applyPresentation() {
    columnMeasurementsDirty_ = true;
    const bool dashboard=compact_&&dashboardLayout_;
    table_->setProperty("compactWatchlist", compact_);
    table_->setProperty("dashboardWatchlist", dashboard);
    table_->horizontalHeader()->setVisible(!compact_||dashboard);
    table_->horizontalHeader()->setMinimumSectionSize(dashboard?0:compact_?24:32);
    table_->horizontalHeader()->setSectionResizeMode(dashboard?QHeaderView::Fixed:QHeaderView::Interactive);
    if(dashboard)table_->horizontalHeader()->setFixedHeight(24);
    else{table_->horizontalHeader()->setMinimumHeight(0);table_->horizontalHeader()->setMaximumHeight(QWIDGETSIZE_MAX);}
    table_->verticalHeader()->setDefaultSectionSize(dashboard?32:compact_?40:58);
    setThemeStyle(table_, dashboard
        ? "QTableWidget#watchlistTable { background: ${bg}; border: 0; border-radius: 0; padding: 0; } QTableWidget#watchlistTable::item { border-bottom: 1px solid ${rule}; padding: 0; } QTableWidget#watchlistTable QHeaderView::section { background: ${bg}; padding: 3px 0; }"
        : compact_ ? "QTableWidget#watchlistTable { background: ${bg}; border: 0; border-radius: 0; padding: 0; } QTableWidget#watchlistTable::item { border-bottom: 1px solid ${rule}; padding: 2px; }"
        : "QTableWidget#watchlistTable { background: ${bg}; border: 0; } QTableWidget#watchlistTable::item { border-bottom: 1px solid ${rule}; padding: 4px; }");
    if(auto* basis=findChild<QLabel*>("watchlistBasis"))basis->setVisible(!compact_);
    if(auto* legend=findChild<QLabel*>("watchlistStaleLegend"))legend->setVisible(dashboard);
    if(auto* box=qobject_cast<QVBoxLayout*>(layout())){
        if(dashboard){box->removeWidget(tabs_);head_->addWidget(tabs_);}
        else{head_->removeWidget(tabs_);box->insertWidget(1,tabs_,0,Qt::AlignLeft);}
        box->setSpacing(compact_?6:8);
    }
    render();
}
QString ExploreWatchlist::section() const { return Sections.value(tabs_->currentIndex(), "coin"); }
void ExploreWatchlist::setSection(const QString& section) {
    const int index = Sections.indexOf(section);
    if (index >= 0) tabs_->setCurrentIndex(index);
}
void ExploreWatchlist::toggle(const QString& section, const QString& id) {
    if (!Sections.contains(section) || id.isEmpty() || id.size() > 80) return;
    auto& list = lists_[section];
    if (!list.removeOne(id)) list.append(id);
    ProfileSettings().setValue("explore/watchlist/" + section, list);
    render();
    emit favouritesChanged();
}
void ExploreWatchlist::setQuotes(const QList<WatchlistQuote>& quotes) {
    QHash<QString, WatchlistQuote> next;
    for (const auto& quote : quotes) next.insert(quote.section + ":" + quote.id, quote);
    if (quotes_ == next) return;
    quotes_.swap(next);
    render();
}
void ExploreWatchlist::render() {
    // Initial callers assign internal evidence before first show. Once presented,
    // hidden quotes stay in the source cache and catch up before the next paint.
    if (changingSection_ || deferHiddenPresentation()) return;
    renderPending_ = false;
    const auto key = section();
    const bool coin = key == "coin";
    const QSignalBlocker block(table_);
    const auto* selected = table_->item(table_->currentRow(), 1);
    const auto selectedId = selected ? selected->data(Qt::UserRole).toString() : QString{};
    const int scroll = table_->verticalScrollBar()->value();
    table_->setHorizontalHeaderLabels({QStringLiteral("★"), tr("종목"), tr("현재가"), coin ? tr("24h %") : tr("전일 대비"),
                                      coin ? tr("Last 24h") : tr("최근 거래일"), tr("MC"), coin ? tr("24h Vol") : tr("거래량")});
    table_->horizontalHeader()->setSortIndicatorShown(sortColumn_ >= 0);
    if (sortColumn_ >= 0) table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
    auto ids = lists_.value(key);
    const auto quoteFor = [this, &key](const QString& id) { return quotes_.value(key + ":" + id); };
    if (sortColumn_ >= 0) std::stable_sort(ids.begin(), ids.end(), [&](const auto& a, const auto& b) {
        const auto x = quoteFor(a), y = quoteFor(b);
        if (sortColumn_ == 1) {
            const int compare = QString::localeAwareCompare(x.name.isEmpty() ? a : x.name, y.name.isEmpty() ? b : y.name);
            return sortOrder_ == Qt::AscendingOrder ? compare < 0 : compare > 0;
        }
        bool ax = true, by = true;
        const auto value = [this](const WatchlistQuote& q, bool* ok) {
            if (sortColumn_ == 3) { const auto v = q.change.toDouble(ok); *ok = *ok && std::isfinite(v); return v; }
            const double v = sortColumn_ == 2 ? q.priceValue : sortColumn_ == 5 ? q.capValue : q.volumeValue;
            *ok = std::isfinite(v) && v > 0; return v;
        };
        const double av = value(x, &ax), bv = value(y, &by);
        if (ax != by) return ax; // Unknown values always last, even when ascending.
        if (!ax) return false;
        return sortOrder_ == Qt::AscendingOrder ? av < bv : av > bv;
    });
    if (table_->rowCount() != ids.size()) columnMeasurementsDirty_ = true;
    table_->setRowCount(ids.size());
    bool hasHistory=false;
    const auto numberFont = KeefiFonts::font(FontRole::Number11);
    const auto nameFont = KeefiFonts::font(FontRole::Body15);
    const auto& colors = themeColors();
    const auto removeTip = tr("관심 리스트에서 빼기 · Delete");
    for (int row = 0; row < ids.size(); ++row) {
        const auto id = ids[row]; const auto q = quoteFor(id);
        bool ok = false; const double percent = q.change.toDouble(&ok);
        const auto displayed=ok&&std::isfinite(percent)?QString::number(percent,'f',2).toDouble():0;
        const auto direction=ok&&std::isfinite(percent)?movementForSign(displayed>0?1:displayed<0?-1:0):MovementDirection::Unknown;
        const auto change=ok&&std::isfinite(percent)?movementDecoratedText(displayed==0?QStringLiteral("0.00%"):(displayed>0?"+":"")+QString::number(percent,'f',2)+"%",direction):QStringLiteral("—");
        const auto state=usabilityFreshness(q.evidenceState);
        const bool stale=state==usabilityText(UsabilityText::Delayed);
        const auto price = usabilityPublicText(q.price);
        const auto shownPrice=q.price.isEmpty()?QStringLiteral("—"):price+(state.isEmpty()?QString{}:" · "+state);
        auto displayName=q.name;
        if(compact_&&!dashboardLayout_&&stale&&!displayName.isEmpty()&&!displayName.endsWith(QStringLiteral(" · 이전 시세")))
            displayName+=QStringLiteral(" · 이전 시세");
        const QStringList texts{QStringLiteral("★"), displayName.isEmpty() ? id : displayName + "\n" + id,
                                shownPrice, change, QStringLiteral("—"),
                                q.cap.isEmpty() ? QStringLiteral("—") : q.cap, q.volume.isEmpty() ? QStringLiteral("—") : q.volume};
        const auto sourceTip = usabilitySourceTime(q.evidenceProvider,q.evidenceSourceTimeMs);
        const auto directionColor = MovementColorPolicy::colorFor(this,direction);
        for (int col = 0; col < texts.size(); ++col) {
            auto* cell = table_->item(row, col);
            const bool newCell = !cell;
            if (newCell) cell = new QTableWidgetItem;
            if (cell->text() != texts[col]) {
                if (col == 2 || col == 3 || (!compact_ && (col == 5 || col == 6)))
                    columnMeasurementsDirty_ = true;
                cell->setText(texts[col]);
            }
            cell->setFont(col >= 2 ? numberFont : nameFont);
            cell->setTextAlignment(col == 1 ? Qt::AlignLeft | Qt::AlignVCenter : col == 4 || col == 0 ? Qt::AlignCenter : Qt::AlignRight | Qt::AlignVCenter);
            auto tooltip=col == 0 ? removeTip : sourceTip;
            if(compact_&&dashboardLayout_&&stale&&col==1)tooltip=tr("이전 시세 · 갱신 대기")+(tooltip.isEmpty()?QString{}:"\n"+tooltip);
            cell->setToolTip(tooltip);
            cell->setForeground(col==3 ? directionColor : col==0 ? colors.accentText : colors.text);
            if (col == 1) {
                cell->setData(Qt::UserRole, id);
                cell->setData(CompactNameRole, q.name);
                cell->setData(StaleQuoteRole, stale);
            } else if (col == 2) {
                cell->setData(CompactValueRole, stale&&!q.price.isEmpty()?price:shownPrice);
            } else if (col == 4) {
                cell->setData(HistoryRole, QVariant::fromValue(q.history));
            }
            if (newCell) table_->setItem(row, col, cell);
        }
        hasHistory|=q.history.size()>=2;
        if (id == selectedId) table_->setCurrentCell(row, 1);
    }
    table_->verticalScrollBar()->setValue(scroll);
    empty_->setText(compact_ ? tr("아직 관심 종목이 없어요.\n순위에서 ☆를 눌러 담아 보세요.")
                            : tr("%1에서 관심 종목의 ☆를 눌러 저장하세요.\n현재가·등락률·추이·시가총액·거래량이 여기에 표시됩니다.").arg(tabs_->tabText(tabs_->currentIndex())));
    empty_->setVisible(ids.isEmpty());
    table_->setVisible(!ids.isEmpty());
    hasHistory_ = hasHistory;
    applyEvidenceNumbers();
    layoutColumns();
    emit evidenceOrderChanged();
}
} // namespace keefi
