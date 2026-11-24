#include "keefi/PlainToolTip.h"
#include <QSignalBlocker>
#include "keefi/Spotlight.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LocalComboBox.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace keefi {
namespace {
constexpr int RowHeight = SpotlightCard::RowHeight;

QString changeLabel(const QString& percent, int* direction) {
    bool ok = false;
    const double value = percent.toDouble(&ok);
    *direction = 0;
    if (!ok || percent.isEmpty() || !std::isfinite(value)) return QStringLiteral("—");
    if (std::abs(value) < 0.005) return QStringLiteral("0.00%");
    *direction = value > 0 ? 1 : -1;
    return movementDecoratedText((value > 0 ? QStringLiteral("+") : QString{}) + QString::number(value, 'f', 2) + "%",movementForSign(*direction));
}
} // namespace

// Five painted rows; hover and click pick a row.
class SpotlightRows final : public QWidget {
  public:
    explicit SpotlightRows(QWidget* parent) : QWidget(parent) {
        setObjectName("spotlightRows");
        setMouseTracking(true);
        setRowCount(SpotlightCard::PageSize);
        setCursor(Qt::PointingHandCursor);
        bindMovementPresentation(this,[this]{update();});
        bindThemePresentation(this,[this]{update();});
        for (int i = 0; i < SpotlightCard::LastPageRows; ++i) {
            auto* star = new QToolButton(this);
            star->setObjectName(QString("spotlightStar%1").arg(i));
            star->setCheckable(true);
            star->setCursor(Qt::PointingHandCursor);
            star->setFont(KeefiFonts::font(FontRole::Title17));
            setThemeStyle(star,"QToolButton { background: transparent; border: 0; padding: 0; color: ${text3}; } QToolButton:checked { color: ${accentText}; } QToolButton:focus { border: 1px solid ${accent}; border-radius: 4px; }");
            star->setFixedSize(24, 32);
            QObject::connect(star, &QToolButton::toggled, this, [this, i] { if (i < items_.size() && favourite) favourite(items_[i].id); });
            stars_.append(star);
        }
    }
    void setRowCount(int rows) { setFixedHeight(RowHeight * rows + 4); }
    void setItems(const QList<SpotlightItem>& items, int firstRank = 1) { if (firstRank_ == firstRank && items_ == items) return; items_ = items; firstRank_ = firstRank; hover_ = -1; syncStars();placeStars();refreshAccessibility(); update(); }
    void setSelected(const QString& id) { selected_ = id; update(); }
    void setFavourites(const QSet<QString>& ids) { if (favourites_ == ids) return; favourites_ = ids; syncStars(); }
    void setEvidenceSources(const QHash<QString, QString>& sources) {if (evidenceSources_ == sources) return; evidenceSources_=sources;setToolTip(tooltipText(hover_));}
    void setEvidenceStates(const QHash<QString, QString>& states) {if (evidenceStates_ == states) return; evidenceStates_=states;refreshAccessibility();update();}
    void setEvidenceNumbers(const QHash<QString, QList<int>>& numbers) {
        if (evidenceNumbers_ == numbers) return;
        evidenceNumbers_ = numbers;
        refreshAccessibility();update();
    }

    std::function<void(const QString&)> clicked;
    std::function<void(const QString&)> favourite;

  protected:
    // Reserve the complete 22px price first. Narrow panels place the change
    // below it and the code below the name; no price digit is elided.
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), themeColors().bg);
        if (items_.isEmpty()) {
            p.setFont(KeefiFonts::font(FontRole::Secondary14));
            p.setPen(themeColors().text3);
            p.drawText(rect(), Qt::AlignCenter, tr("표시할 순위가 없습니다"));
            return;
        }
        const bool compact = width() < 520 || (width()<700&&std::any_of(items_.cbegin(),items_.cend(),[](const auto& item){return !item.study.isEmpty();}));
        const QFont numberFont = KeefiFonts::font(FontRole::Number11);
        const QFont nameFont = KeefiFonts::font(compact ? FontRole::SecondaryStrong14 : FontRole::BodyBold15);
        const QFont priceFont = KeefiFonts::font(FontRole::Value22);
        const QFont stateFont = KeefiFonts::font(FontRole::Caption12);
        for (int i = 0; i < items_.size(); ++i) {
            const auto& item = items_[i];
            const QRectF row(0, 2 + i * RowHeight, width(), RowHeight);
            if (i == hover_ || item.id == selected_) p.fillRect(row, themeColors().surface);
            p.setPen(themeColors().rule); p.drawLine(row.topLeft(), row.topRight());
            const qreal starLeft = row.right() - 24;
            const qreal valueRight = starLeft - 8;
            int direction = 0;
            const auto change = changeLabel(item.changePercent, &direction);
            const auto stateText = usabilityFreshness(evidenceStates_.value(item.id));
            auto price = usabilityPublicText(item.price);
            QString unit;
            if (price.endsWith(QStringLiteral("원"))) { unit = QStringLiteral("원"); price.chop(1); price = price.trimmed(); }
            else if (price.endsWith(QStringLiteral(" KRW"))) { unit = QStringLiteral("KRW"); price.chop(4); price = price.trimmed(); }
            const qreal unitWidth = unit.isEmpty() ? 0 : QFontMetricsF(stateFont).horizontalAdvance(unit) + 5;
            const qreal priceWidth = QFontMetricsF(priceFont).horizontalAdvance(price) + unitWidth + 2;
            const qreal priceRight = compact ? valueRight : valueRight - 84 - 12;
            const qreal priceLeft = priceRight - priceWidth;
            const QRectF priceBand(priceLeft, compact ? row.top() + 1 : row.top(), priceWidth - unitWidth,
                                   compact ? 27 : row.height());
            p.setFont(numberFont); p.setPen(themeColors().text3);
            p.drawText(QRectF(0, row.top(), 20, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(firstRank_ + i).rightJustified(2, '0'));
            p.setFont(priceFont); p.setPen(themeColors().text);
            p.drawText(priceBand, Qt::AlignRight | Qt::AlignVCenter, price);
            if (!unit.isEmpty()) {
                p.setFont(stateFont); p.setPen(themeColors().text3);
                p.drawText(QRectF(priceRight - unitWidth + 5, priceBand.top(), unitWidth - 5, priceBand.height()),
                           Qt::AlignRight | Qt::AlignVCenter, unit);
            }
            const qreal stateWidth = stateText.isEmpty() ? 0 : QFontMetricsF(stateFont).horizontalAdvance(stateText) + 6;
            const QRectF changeBand(compact ? priceLeft : valueRight - 84,
                                    compact ? row.bottom() - 16 : row.top(),
                                    compact ? priceWidth - stateWidth : 84,
                                    compact ? 15 : stateText.isEmpty() ? row.height() : row.height() / 2 + 3);
            p.setFont(numberFont); p.setPen(MovementColorPolicy::colorFor(this, movementForSign(direction)));
            p.drawText(changeBand, Qt::AlignRight | Qt::AlignVCenter, change);
            if (!stateText.isEmpty()) {
                p.setFont(stateFont); p.setPen(themeColors().text3);
                p.drawText(compact ? QRectF(priceRight - stateWidth + 6, row.bottom() - 17, stateWidth - 6, 16)
                                   : QRectF(valueRight - 84, row.center().y() + 2, 84, row.height() / 2 - 2),
                           Qt::AlignRight | Qt::AlignVCenter, stateText);
            }
            const auto name = usabilityPublicText(item.display.isEmpty() ? item.symbol : item.display);
            auto code = usabilityPublicText(item.display.isEmpty() ? (item.symbol == item.id ? QString{} : item.id) : item.symbol);
            const bool studyColumn=width()>=700&&!item.study.isEmpty();
            if(!item.study.isEmpty()&&!studyColumn)code=item.study;
            else if(!item.observation.isEmpty())code+=(code.isEmpty()?QString{}:QStringLiteral(" · "))+item.observation;
            const qreal nameLeft = 27;
            qreal nameRight = priceLeft - 10;
            if(studyColumn){const QRectF cell(nameRight-136,row.top(),130,row.height());nameRight=cell.left()-12;
                p.setFont(stateFont);p.setPen(themeColors().text2);p.drawText(cell,Qt::AlignLeft|Qt::AlignVCenter,QFontMetricsF(stateFont).elidedText(item.study,Qt::ElideRight,130));}
            // Historical points remain a quiet, optional line only in roomy rows.
            if (!compact && item.spark.size() >= 2 && nameRight - nameLeft >= 206) {
                const QRectF line(nameRight - 96, row.center().y() - 12, 96, 24); nameRight = line.left() - 14;
                const auto [low, high] = std::minmax_element(item.spark.cbegin(), item.spark.cend());
                const double span = *high - *low; QPainterPath path;
                for (int k = 0; k < item.spark.size(); ++k) {
                    const QPointF point(line.left() + line.width() * k / (item.spark.size() - 1),
                                        span > 0 ? line.bottom() - (item.spark[k] - *low) / span * line.height() : line.center().y());
                    if (k == 0) path.moveTo(point); else path.lineTo(point);
                }
                p.setBrush(Qt::NoBrush); p.setPen(QPen(themeColors().text2, 1.5)); p.drawPath(path);
            }
            const qreal nameWidth = qMax<qreal>(0, nameRight - nameLeft);
            p.setFont(nameFont); p.setPen(themeColors().text);
            const QRectF nameBand(nameLeft, compact && !code.isEmpty() ? row.top() + 4 : row.top(), nameWidth,
                                  compact && !code.isEmpty() ? 20 : row.height());
            p.drawText(nameBand, Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(nameFont).elidedText(name, Qt::ElideRight, nameWidth));
            if (!code.isEmpty()) {
                const QFont codeFont=item.observation.isEmpty()&&item.study.isEmpty()?numberFont:stateFont;
                p.setFont(codeFont); p.setPen(themeColors().text3);
                if (compact) p.drawText(QRectF(nameLeft, row.bottom() - 16, nameWidth, 14), Qt::AlignLeft | Qt::AlignVCenter,
                                        QFontMetricsF(codeFont).elidedText(code, Qt::ElideRight, nameWidth));
                else {
                    const qreal codeLeft = nameLeft + QFontMetricsF(nameFont).horizontalAdvance(name) + 8;
                    if (codeLeft + QFontMetricsF(codeFont).horizontalAdvance(code) <= nameRight)
                        p.drawText(QRectF(codeLeft, row.top(), nameRight - codeLeft, row.height()), Qt::AlignLeft | Qt::AlignVCenter, code);
                }
            }
        }
    }
    void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); placeStars(); }
    void mouseMoveEvent(QMouseEvent* event) override {
        const int row = rowAt(event->position().y());
        if (row != hover_) { hover_ = row; update(); }
        setToolTip(tooltipText(row));
    }
    void leaveEvent(QEvent*) override { hover_ = -1; update(); }
    void mousePressEvent(QMouseEvent* event) override {
        const int row = rowAt(event->position().y());
        if (event->button() == Qt::LeftButton && row >= 0 && clicked) clicked(items_[row].id);
    }

  private:
    QString tooltipText(int row)const {
        if(row<0||row>=items_.size())return {};
        const auto& item=items_[row];return plainToolTip(usabilityPublicText(item.symbol+" · "+item.price+"\n"+item.sub+"\n"+item.observationInformation+"\n"+item.studyInformation+"\n"+evidenceSources_.value(item.id)));
    }
    void refreshAccessibility() {
        QStringList lines;for(const auto& item:items_)lines.append(usabilityPublicText(item.symbol+" "+item.price)+" "+usabilityFreshness(evidenceStates_.value(item.id))+" "+item.observation+" "+item.study);
        setAccessibleDescription(lines.join('\n'));
    }
    void syncStars() {
        for (int i = 0; i < stars_.size(); ++i) {
            auto* star = stars_[i]; const QSignalBlocker block(star); star->setVisible(i < items_.size());
            if (i >= items_.size()) continue;
            const bool saved = favourites_.contains(items_[i].id);
            star->setChecked(saved); star->setText(saved ? QStringLiteral("★") : QStringLiteral("☆"));
            star->setAccessibleName(usabilityPublicText(items_[i].symbol) + (saved ? tr(" 관심 리스트에서 빼기") : tr(" 관심 리스트에 추가")));
            star->setToolTip(plainToolTip(star->accessibleName()));
        }
    }
    void placeStars() {
        for (int i = 0; i < stars_.size(); ++i) stars_[i]->move(width() - 24, 2 + i * RowHeight + (RowHeight - 32) / 2);
    }
    int rowAt(qreal y) const {
        const int row = int((y - 2) / RowHeight);
        return y >= 2 && row >= 0 && row < items_.size() ? row : -1;
    }
    QList<SpotlightItem> items_;
    int firstRank_ = 1;
    QString selected_;
    int hover_ = -1;
    QSet<QString> favourites_;
    QList<QToolButton*> stars_;
    QHash<QString, QList<int>> evidenceNumbers_;
    QHash<QString, QString> evidenceStates_,evidenceSources_;
};

// Page marks stay as small straight rules; the current page has a longer rule.
class SpotlightDots final : public QWidget {
  public:
    explicit SpotlightDots(QWidget* parent) : QWidget(parent) { setFixedSize(96, 20); setObjectName("spotlightDots"); bindThemePresentation(this,[this]{update();}); }
    void setState(int count, int current) {
        count_ = count; current_ = current;
        setFixedWidth(qMax(34, count * 20 + 14)); update();
    }
    std::function<void(int)> clicked;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        qreal x = 2;
        for (int i = 0; i < count_; ++i) {
            const qreal w = i == current_ ? 30 : 12;
            p.setBrush((i == current_ ? themeColors().text3 : themeColors().rule));
            p.drawRect(QRectF(x, 9, w, 2));
            x += w + 8;
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        qreal x = 2;
        for (int i = 0; i < count_; ++i) {
            const qreal w = i == current_ ? 30 : 12;
            if (event->position().x() >= x - 4 && event->position().x() <= x + w + 4) { if (clicked) clicked(i); return; }
            x += w + 8;
        }
    }

  private:
    int count_ = 1, current_ = 0;
};

SpotlightCard::SpotlightCard(const QString& key, const QString& title, QWidget* parent) : QFrame(parent), key_(key) {
    setObjectName("spotlightCard_" + key);
    // The dashboard restores a flat bounded panel while retaining ledger rows.
    setProperty("spotlight", true);
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(286);
    setThemeStyle(this, QStringLiteral("QFrame#%1 { background: ${bg}; border: 1px solid ${rule}; border-radius: 12px; }").arg(objectName()));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(3);
    auto* head = new QHBoxLayout;
    head->setSpacing(6);
    auto* name = new QLabel(title, this);
    name->setObjectName("spotlightTitle");
    name->setFont(KeefiFonts::font(FontRole::Title17));
    // Keep the same header and first-row baseline for all three markets.
    name->setMinimumHeight(CompactButtonHeight);
    head->addWidget(name);
    if (key == "coin") {
        auto* venue = new LocalComboBox(this);
        coinVenueChoice_ = venue;
        venue->setObjectName("rankingCoinVenue");
        venue->setAccessibleName(tr("코인 순위 거래소"));
        venue->setCompact();
        venue->addItem(tr("업비트"), QStringLiteral("upbit"));
        venue->addItem(tr("바이낸스"), QStringLiteral("binance"));
        venue->setItemData(0, tr("원화(KRW) · 전일 종가 대비 · 한국시간 오전 9시 기준"), Qt::ToolTipRole);
        venue->setItemData(1, tr("현물 USDT · 최근 24시간 변동률"), Qt::ToolTipRole);
        venue->setToolTip(venue->currentData(Qt::ToolTipRole).toString());
        venue->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        head->addWidget(venue);
    }
    modeButton_ = new QPushButton(this);
    modeButton_->setObjectName("spotlightMode");
    modeButton_->setProperty("spotlightMode", true);
    modeButton_->setCursor(Qt::PointingHandCursor);
    auto* menu = new QMenu(modeButton_);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        for (const auto& m : modes_) {
            auto* action = menu->addAction(m.label);
            action->setCheckable(true);
            action->setChecked(m.key == mode_);
            connect(action, &QAction::triggered, this, [this, key = m.key] { setMode(key); });
        }
    });
    connect(modeButton_, &QPushButton::clicked, this, [this, menu] {
        menu->popup(modeButton_->mapToGlobal(QPoint(0, modeButton_->height())));
    });
    head->addWidget(modeButton_);
    head->addStretch();
    status_ = new QLabel(this);
    status_->setObjectName("spotlightStatus");
    status_->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(status_, "QLabel{color:${text2};}");
    status_->hide();
    head->addWidget(status_);
    auto* open = new QPushButton(QStringLiteral("↗"), this);
    open->setObjectName("spotlightOpen");
    open->setProperty("spotlightIcon", true);
    open->setToolTip(tr("지도에서 보기"));
    open->setAccessibleName(tr("지도에서 보기"));
    open->setCursor(Qt::PointingHandCursor);
    connect(open, &QPushButton::clicked, this, [this] { emit openRequested(key_); });
    setThemeStyle(open, "QPushButton#spotlightOpen { background: transparent; border: 0; min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px; padding: 0; color: ${text2}; }");
    head->addWidget(open);
    layout->addLayout(head);
    rows_ = new SpotlightRows(this);
    rows_->clicked = [this](const QString& id) { rows_->setSelected(id); emit itemSelected(key_, id); };
    rows_->favourite = [this](const QString& id) { emit favouriteToggled(key_, id); };
    rowScroll_=new QScrollArea(this);rowScroll_->setObjectName("spotlightRowScroll");
    rowScroll_->setWidgetResizable(true);rowScroll_->setFrameShape(QFrame::NoFrame);
    rowScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rowScroll_->setFixedHeight(PageSize*RowHeight+4);rowScroll_->setWidget(rows_);layout->addWidget(rowScroll_);
    connect(rowScroll_->verticalScrollBar(),&QScrollBar::valueChanged,this,&SpotlightCard::evidenceOrderChanged);
    if (coinVenueChoice_) {
        connect(coinVenueChoice_, &QComboBox::currentIndexChanged, this, [this] {
            coinVenueChoice_->setToolTip(coinVenueChoice_->currentData(Qt::ToolTipRole).toString());
            rowScroll_->verticalScrollBar()->setValue(0);
            setPage(0);
            if (mode_ == "cap") setMode("gainers");
            emit coinVenueChanged();
        });
    }
    auto* foot = new QHBoxLayout;
    dots_ = new SpotlightDots(this);
    dots_->clicked = [this](int page) { setPage(page); };
    foot->addWidget(dots_, 0, Qt::AlignVCenter);
    range_=new QLabel(this);range_->setObjectName("spotlightRankRange");range_->setFont(KeefiFonts::font(FontRole::Caption12));foot->addWidget(range_);
    foot->addStretch();
    previous_ = new QPushButton(QStringLiteral("‹"), this);
    next_ = new QPushButton(QStringLiteral("›"), this);
    previous_->setObjectName("spotlightPrevious");
    next_->setObjectName("spotlightNext");
    for (auto* b : {previous_, next_}) {
        b->setProperty("spotlightIcon", true);
        b->setCursor(Qt::PointingHandCursor);
        setThemeStyle(b, QStringLiteral("QPushButton#%1 { background: transparent; border: 0; min-width: 24px; max-width: 24px; min-height: 20px; max-height: 20px; padding: 0; color: ${text2}; }").arg(b->objectName()));
        foot->addWidget(b);
    }
    previous_->setAccessibleName(tr("이전 페이지"));
    next_->setAccessibleName(tr("다음 페이지"));
    connect(previous_, &QPushButton::clicked, this, [this] { setPage(page_ - 1); });
    connect(next_, &QPushButton::clicked, this, [this] { setPage((page_ + 1) % pageCount()); });
    layout->addLayout(foot);
    render();
}

QString SpotlightCard::coinVenue() const {
    return coinVenueChoice_ ? coinVenueChoice_->currentData().toString() : QStringLiteral("upbit");
}

const SpotlightMode* SpotlightCard::current() const {
    for (const auto& m : modes_) if (m.key == mode_) return &m;
    return modes_.isEmpty() ? nullptr : &modes_.first();
}

void SpotlightCard::setModes(const QList<SpotlightMode>& modes) {
    if (modes_ == modes) return;
    modes_ = modes;
    if (!current() || current()->key != mode_) mode_ = modes_.isEmpty() ? QString{} : current()->key;
    page_ = qBound(0, page_, pageCount() - 1);
    render();
}

void SpotlightCard::setMode(const QString& key) {
    if (key == mode_) return;
    mode_ = key;
    page_ = 0;
    rowScroll_->verticalScrollBar()->setValue(0);
    render();
    emit modeChanged();
}

int SpotlightCard::pageCount() const {
    const auto* m = current();
    return m && m->items.size()>LastPageRows ? 2 : 1;
}

void SpotlightCard::setPage(int page) {
    page = qBound(0, page, pageCount() - 1);
    if (page == page_) return;
    page_ = page;
    rowScroll_->verticalScrollBar()->setValue(0);
    render();
}

QList<SpotlightItem> SpotlightCard::visibleItems() const {
    const auto* m = current();
    const int count=page_==0&&pageCount()==2?FirstPageRows:LastPageRows;
    auto items=m ? m->items.mid(firstRank()-1,count) : QList<SpotlightItem>{};
    for(auto& item:items){const auto study=studyValues_.value(item.id);item.study=study.first;item.studyInformation=study.second;}
    return items;
}
QList<SpotlightItem> SpotlightCard::viewportItems() const {
    const int offset=rowScroll_->verticalScrollBar()->value();
    return visibleItems().mid(offset/RowHeight,(offset%RowHeight+rowScroll_->viewport()->height()+RowHeight-1)/RowHeight);
}
void SpotlightCard::setStudyValues(const QHash<QString,QPair<QString,QString>>& values){
    if(studyValues_==values)return;studyValues_=values;
    rows_->setItems(visibleItems(),firstRank());setAccessibleDescription(rows_->accessibleDescription());
}

void SpotlightCard::setRowsPerPage(int rows) {
    rows = qBound(1, rows, LastPageRows);
    if (rows == rowsPerPage_) return;
    rowsPerPage_ = rows;
    rowScroll_->setFixedHeight(rows*RowHeight+4);
    render();
}

int SpotlightCard::chromeHeight() const { return qMax(0, sizeHint().height() - rowsPerPage_ * RowHeight); }

void SpotlightCard::setSelected(const QString& id) { rows_->setSelected(id); }
void SpotlightCard::setFavourites(const QSet<QString>& ids) { rows_->setFavourites(ids); }
void SpotlightCard::setEvidenceSources(const QHash<QString, QString>& sources) {rows_->setEvidenceSources(sources);}
void SpotlightCard::setEvidenceStates(const QHash<QString, QString>& states) {rows_->setEvidenceStates(states);setAccessibleDescription(rows_->accessibleDescription());}
void SpotlightCard::setEvidenceNumbers(const QHash<QString, QList<int>>& numbers) {
    rows_->setEvidenceNumbers(numbers);setAccessibleDescription(rows_->accessibleDescription());
}

void SpotlightCard::render() {
    const auto* m = current();
    modeButton_->setText((m ? m->label : QString{}) + QStringLiteral("  ▾"));
    modeButton_->setVisible(m != nullptr);
    const auto items=visibleItems();
    rows_->setRowCount(qMax(rowsPerPage_,int(items.size())));
    rows_->setItems(items, firstRank());
    dots_->setState(pageCount(), page_);
    range_->setText(items.isEmpty()?QString{}:items.size()==1?tr("%1위").arg(firstRank()):tr("%1–%2위").arg(firstRank()).arg(firstRank()+items.size()-1));
    previous_->setEnabled(page_ > 0);
    next_->setEnabled(pageCount() > 1);
    next_->setAccessibleName(page_ + 1 == pageCount() && pageCount() > 1 ? tr("첫 페이지로") : tr("다음 페이지"));
    next_->setToolTip(next_->accessibleName());
    emit evidenceOrderChanged();
}

void SpotlightCard::setStatus(const QString& status) {
    status_->setText(status); status_->setAccessibleName(status); status_->setVisible(!status.isEmpty());
}
} // namespace keefi
